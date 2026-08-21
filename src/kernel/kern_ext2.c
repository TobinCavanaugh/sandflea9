//
// Created by tobin on 2025-12-01.
//

#include "../include/kern_ext2.h"
#include "../include/kern_mem.h"
#include "../include/kern_ide.h"
#include "../include/kern_vmm.h"
#include "../include/kern_profile.h"
#include "../util/util_str.h"
#include "../external/xxhash/xxhash.h"

// --- Logging Control ---
#define EXT2_DEBUG 0
#define DEBUG_LOG(expr) do { if (EXT2_DEBUG) { expr; } } while (0)

// ── Multi-drive globals ───────────────────────────────────────────────────
drive_t drives[MAX_DRIVES] = {0};
u8      drive_count = 0;
drive_t *active_drive = null;

// Current working directory. Tracks the active drive implicitly — if the
// user does `cd //B`, the drive switches AND cwd captures the prefix.
// On the boot drive (A), cwd is just "/path". On other drives: "//B/path".
char cwd[256] = "/";

// Legacy globals — kept for source compatibility with existing code.
// They are repointed by ext2_switch_drive() whenever the active drive changes.
ext2_superblock_t *sb_ptr  = null;
u32               block_size = 0;
u8               *bgdt_cache = null;

// ── Drive switching ───────────────────────────────────────────────────────

u0 ext2_switch_drive(drive_t *d) {
    if (!d || !d->present) return;
    active_drive = d;
    sb_ptr      = &d->sb;
    block_size  = d->block_size;
    bgdt_cache  = d->bgdt_cache;
}

// ── Sector-level I/O abstraction ─────────────────────────────────────────

static u0 drive_read_sectors(drive_t *d, u32 lba, u32 count, u8 *buffer) {
    if (!d || !d->present || !buffer || count == 0) return;
    if (d->backend == DRIVE_BACKEND_RAMDISK) {
        u64 offset = (u64)lba * 512;
        u64 bytes = (u64)count * 512;
        if (offset + bytes <= d->ram_size && d->ram_base != null) {
            mem_copy(buffer, d->ram_base + offset, bytes);
        } else {
            mem_set(buffer, 0, bytes);
        }
    } else if (d->backend == DRIVE_BACKEND_IDE) {
        ide_read_sectors(d->ide_drive_sel, lba, (u8)count, buffer);
    }
}

static u0 drive_write_sectors(drive_t *d, u32 lba, u32 count, u8 *buffer) {
    if (!d || !d->present || !buffer || count == 0) return;
    if (d->backend == DRIVE_BACKEND_RAMDISK) {
        u64 offset = (u64)lba * 512;
        u64 bytes = (u64)count * 512;
        if (offset + bytes <= d->ram_size && d->ram_base != null) {
            mem_copy(d->ram_base + offset, buffer, bytes);
        }
    } else if (d->backend == DRIVE_BACKEND_IDE) {
        ide_write_sectors(d->ide_drive_sel, lba, (u8)count, buffer);
    }
}

// ── Drive initialization ──────────────────────────────────────────────────

u0 ext2_init_drive(drive_t *d) {
    if (!d || !d->present) return;

    u8 sb_buf[1024];
    drive_read_sectors(d, 2, 2, sb_buf);
    mem_copy((u8 *)&d->sb, sb_buf, sizeof(ext2_superblock_t));

    if (d->sb.magic_signature != EXT2_SIGNATURE) {
        serial_outsf("EXT2: Drive '%s' — no valid ext2 signature (0x%X), skipping\n",
                     d->name, (u32)d->sb.magic_signature);
        d->present = false;
        return;
    }

    d->block_size = 1024 << d->sb.block_size;

    // Cache BGDT — read directly with drive_read_sectors (no ext2_read_block dependency)
    u32 bgdt_block = (d->block_size == 1024) ? 2 : 1;
    u32 sectors_per_block = d->block_size / 512;
    u32 start_sector = bgdt_block * sectors_per_block;
    d->bgdt_cache = kmalloc(d->block_size);
    if (d->bgdt_cache) {
        drive_read_sectors(d, start_sector, sectors_per_block, d->bgdt_cache);
    }

    serial_outsf("EXT2: Drive '%s' (%s) — label '%.16s', %d bytes/block, BGDT cached\n",
                 d->name,
                 (d->backend == DRIVE_BACKEND_RAMDISK) ? "RAMDISK" : "IDE",
                 d->sb.volume_name, d->block_size);
}

// ============================================================================
// Block cache — transparent LRU cache at the ext2_read_block level.
//
// Hash-table with open addressing + linear probing. LRU eviction via a
// monotonic tick counter. Keys on (drive_sel, block_id) so entries from
// different drives never collide.
//
// Tune at runtime: `cache 128` in the shell, or via block_cache_set_capacity().
// Default: 64 entries (~64 KB at 1KB blocks, ~128 KB at 2KB blocks).
// ============================================================================

#define BLOCK_CACHE_MIN_ENTRIES  16
#define BLOCK_CACHE_MAX_ENTRIES  16384  // ~16 MB at 1KB blocks, fits a 14MB WAD
#define BLOCK_CACHE_DEFAULT      64    // ~64 KB — conservative, bump via `cache N`

typedef struct {
    u8   drive_sel;
    u32  block_id;
    u8  *data;          // kmalloc'd buffer, block_size bytes
    u64  last_access;   // monotonic tick for LRU eviction
    bool valid;
} bc_entry_t;

static bc_entry_t *g_bc       = NULL;
static u32         g_bc_cap    = 0;   // table capacity (always power of 2)
static u32         g_bc_used   = 0;   // currently occupied slots
static u64         g_bc_tick   = 0;   // LRU clock — incremented on every access
static u32         g_bc_hits   = 0;   // stats: successful lookups
static u32         g_bc_misses = 0;   // stats: lookups that missed
static u32         g_bc_target = BLOCK_CACHE_DEFAULT;  // user-configurable size

// ── Internal helpers ─────────────────────────────────────────────────────

// Hash (drive_sel, block_id) → slot index. drive_sel is unique per physical
// drive (0xE0 master, 0xF0 slave), so keys from different drives never collide
// in a meaningful way even if block_id overlaps.
static u32 bc_slot(u8 ds, u32 bid) {
    u64 k = ((u64)ds << 32) | (u64)bid;
    return (u32)(XXH64(&k, sizeof(k), 0) & (g_bc_cap - 1));
}

// Allocate or resize the table. Called lazily on first read, or explicitly
// via block_cache_set_capacity().
static void bc_init(void) {
    u32 target = g_bc_target;
    if (target < BLOCK_CACHE_MIN_ENTRIES) target = BLOCK_CACHE_MIN_ENTRIES;
    if (target > BLOCK_CACHE_MAX_ENTRIES) target = BLOCK_CACHE_MAX_ENTRIES;

    // Round up to next power of 2 (fast modulo via bitmask)
    u32 cap = 1;
    while (cap < target) cap <<= 1;

    // No-op if already the right size
    if (g_bc && g_bc_cap == cap) return;

    // Free old table
    if (g_bc) {
        for (u32 i = 0; i < g_bc_cap; i++) {
            if (g_bc[i].valid && g_bc[i].data) kfree(g_bc[i].data);
        }
        kfree(g_bc);
    }

    // Allocate and zero new table
    u32 bytes = cap * sizeof(bc_entry_t);
    g_bc = (bc_entry_t *) kmalloc(bytes);
    if (!g_bc) {
        g_bc_cap  = 0;
        g_bc_used = 0;
        serial_outsl("BC: cache init OOM — caching disabled");
        return;
    }
    mem_set((u8 *)g_bc, 0, bytes);
    g_bc_cap    = cap;
    g_bc_used   = 0;
    g_bc_target = target;

    serial_outsf("BC: cache ready — %u entries (~%u KB)\n",
                 cap, block_size ? (cap * block_size) / 1024 : 0);
}

// Lookup — copies cached data into out_buf. Returns true on hit.
static bool bc_lookup(u8 ds, u32 bid, u8 *out_buf) {
    if (!g_bc || g_bc_cap == 0 || block_size == 0) return false;

    u32 slot = bc_slot(ds, bid);
    u32 start = slot;

    do {
        bc_entry_t *e = &g_bc[slot];
        if (!e->valid) {
            slot = (slot + 1) & (g_bc_cap - 1);
            continue;
        }
        if (e->drive_sel == ds && e->block_id == bid) {
            mem_copy(out_buf, e->data, block_size);
            e->last_access = ++g_bc_tick;
            g_bc_hits++;
            return true;
        }
        slot = (slot + 1) & (g_bc_cap - 1);
    } while (slot != start);

    g_bc_misses++;
    return false;
}

// Insert — copies data into cache. Evicts LRU entry if table is full.
// If the block is already cached, updates in-place.
static void bc_insert(u8 ds, u32 bid, u8 *data) {
    if (!g_bc || g_bc_cap == 0 || block_size == 0) return;

    u32 slot = bc_slot(ds, bid);
    u32 start = slot;
    u32 empty_slot = 0xFFFFFFFFu;
    u32 lru_slot   = 0;
    u64 lru_tick   = 0xFFFFFFFFFFFFFFFFull;

    do {
        bc_entry_t *e = &g_bc[slot];
        if (!e->valid) {
            empty_slot = slot;
            break;
        }
        // Already present — update in-place (shouldn't happen normally
        // since caller checks bc_lookup first, but be safe).
        if (e->drive_sel == ds && e->block_id == bid) {
            mem_copy(e->data, data, block_size);
            e->last_access = ++g_bc_tick;
            return;
        }
        if (e->last_access < lru_tick) {
            lru_tick = e->last_access;
            lru_slot = slot;
        }
        slot = (slot + 1) & (g_bc_cap - 1);
    } while (slot != start);

    // No empty slot found — evict LRU
    if (empty_slot == 0xFFFFFFFFu) {
        empty_slot = lru_slot;
        if (g_bc[empty_slot].data) kfree(g_bc[empty_slot].data);
        g_bc[empty_slot].data  = NULL;
        g_bc[empty_slot].valid = false;
        if (g_bc_used > 0) g_bc_used--;
    }

    // Allocate buffer for the new entry
    if (!g_bc[empty_slot].data) {
        g_bc[empty_slot].data = kmalloc(block_size);
        if (!g_bc[empty_slot].data) return;  // OOM — skip caching this block
    }

    bc_entry_t *e = &g_bc[empty_slot];
    e->drive_sel   = ds;
    e->block_id    = bid;
    e->last_access = ++g_bc_tick;
    e->valid       = true;
    mem_copy(e->data, data, block_size);
    g_bc_used++;
}

// Update an existing cache entry (called from ext2_write_block).
// If the block isn't cached, inserts it (write-through with cache fill).
static void bc_update(u8 ds, u32 bid, u8 *data) {
    if (!g_bc || g_bc_cap == 0 || block_size == 0) return;

    u32 slot = bc_slot(ds, bid);
    u32 start = slot;

    do {
        bc_entry_t *e = &g_bc[slot];
        if (!e->valid) {
            slot = (slot + 1) & (g_bc_cap - 1);
            continue;
        }
        if (e->drive_sel == ds && e->block_id == bid) {
            mem_copy(e->data, data, block_size);
            e->last_access = ++g_bc_tick;
            return;
        }
        slot = (slot + 1) & (g_bc_cap - 1);
    } while (slot != start);

    // Not in cache — insert so subsequent reads hit
    bc_insert(ds, bid, data);
}

// ── Public API ────────────────────────────────────────────────────────────

// Resize the cache at runtime. Safe to call at any time — rehashes.
void block_cache_set_capacity(u32 entries) {
    if (entries < BLOCK_CACHE_MIN_ENTRIES) entries = BLOCK_CACHE_MIN_ENTRIES;
    if (entries > BLOCK_CACHE_MAX_ENTRIES) entries = BLOCK_CACHE_MAX_ENTRIES;
    if (entries == g_bc_target && g_bc) return;
    g_bc_target = entries;
    bc_init();
}

// Query cache statistics.
void block_cache_stats(u32 *out_hits, u32 *out_misses, u32 *out_used, u32 *out_cap) {
    if (out_hits)   *out_hits   = g_bc_hits;
    if (out_misses) *out_misses = g_bc_misses;
    if (out_used)   *out_used   = g_bc_used;
    if (out_cap)    *out_cap    = g_bc_cap;
}

// ── ext2_read_block (with cache) ──────────────────────────────────────────

u0 ext2_read_block(u32 block_id, u8 *buffer) {
    PROFILE_SCOPE("ext2:read_block");
    if (!buffer || block_size == 0 || !active_drive || !active_drive->present) return;

    // Fast-path: RAM disk read is direct memory copy
    if (active_drive->backend == DRIVE_BACKEND_RAMDISK) {
        u64 offset = (u64)block_id * block_size;
        if (offset + block_size <= active_drive->ram_size && active_drive->ram_base != null) {
            mem_copy(buffer, active_drive->ram_base + offset, block_size);
        } else {
            mem_set(buffer, 0, block_size);
        }
        return;
    }

    // Lazy-init cache on first read
    if (!g_bc) bc_init();

    // Check cache first
    if (bc_lookup(active_drive->drive_id, block_id, buffer)) {
        return;  // cache hit — no IDE I/O
    }

    // Cache miss — read from disk
    u32 sectors_per_block = block_size / 512;
    u32 start_sector = block_id * sectors_per_block;
    ide_read_sectors(active_drive->ide_drive_sel, start_sector, sectors_per_block, buffer);

    // Insert into cache for next time
    bc_insert(active_drive->drive_id, block_id, buffer);
}

u0 ext2_read_blocks(u32 start_block, u32 count, u8 *buffer) {
    PROFILE_SCOPE("ext2:read_blocks");
    if (!buffer || block_size == 0 || count == 0 || !active_drive || !active_drive->present) return;
    if (count == 1) { ext2_read_block(start_block, buffer); return; }

    // Fast-path: RAM disk multi-block read is single direct memory copy
    if (active_drive->backend == DRIVE_BACKEND_RAMDISK) {
        u64 offset = (u64)start_block * block_size;
        u64 bytes = (u64)count * block_size;
        if (offset + bytes <= active_drive->ram_size && active_drive->ram_base != null) {
            mem_copy(buffer, active_drive->ram_base + offset, bytes);
        } else {
            mem_set(buffer, 0, bytes);
        }
        return;
    }

    // Lazy-init cache so multi-block reads also use it
    if (!g_bc) bc_init();

    // ── Cache fast-path ──────────────────────────────────────────────
    // If every block is already cached, we can skip IDE I/O entirely.
    // This is what makes the second `doom` launch near-instant.
    // On the first block miss we bail — the whole run will come from IDE.
    // (Partial-hit handling would break IDE batching, so we don't bother.)
    bool all_cached = (g_bc != NULL);
    if (all_cached) {
        for (u32 i = 0; i < count; i++) {
            if (!bc_lookup(active_drive->drive_id, start_block + i,
                           buffer + i * block_size)) {
                all_cached = false;
                break;   // first miss → fall through to IDE path
            }
        }
    }
    if (all_cached) return;  // zero IDE I/O, purely cache-served

    // ── IDE path (cache miss) ────────────────────────────────────────
    u32 sectors_per_block = block_size / 512;
    u32 start_sector = start_block * sectors_per_block;
    u32 total_sectors = count * sectors_per_block;

    u8 *read_ptr   = buffer;
    u32 remaining  = total_sectors;
    u32 sector     = start_sector;

    // Read all sectors in chunks (max 255 per IDE PIO command)
    while (remaining > 0) {
        u8 chunk = (remaining > 255) ? 255 : (u8)remaining;
        ide_read_sectors(active_drive->ide_drive_sel, sector, chunk, read_ptr);
        remaining -= chunk;
        sector    += chunk;
        read_ptr  += chunk * 512;
    }

    // Populate cache so the NEXT read hits the fast-path above.
    // Only populate if the entire run fits — otherwise each insert would
    // evict the previous one, burning CPU for zero future benefit.
    if (g_bc && count <= g_bc_cap) {
        for (u32 i = 0; i < count; i++) {
            bc_insert(active_drive->drive_id, start_block + i,
                      buffer + i * block_size);
        }
    }
}

u0 ext2_write_block(u32 block_id, u8 *buffer) {
    if (!buffer || block_size == 0 || !active_drive || !active_drive->present) return;

    if (active_drive->backend == DRIVE_BACKEND_RAMDISK) {
        u64 offset = (u64)block_id * block_size;
        if (offset + block_size <= active_drive->ram_size && active_drive->ram_base != null) {
            mem_copy(active_drive->ram_base + offset, buffer, block_size);
        }
        return;
    }

    u32 sectors_per_block = block_size / 512;
    u32 start_sector = block_id * sectors_per_block;
    ide_write_sectors(active_drive->ide_drive_sel, start_sector, sectors_per_block, buffer);

    // Keep cache coherent — update or insert the written data
    // (write-through with cache fill, so subsequent reads hit)
    if (g_bc) bc_update(active_drive->drive_id, block_id, buffer);
}

u0 ext2_init(struct limine_module_response *mod_resp) {
    // Probe and initialize all present drives
    // Drive 0 (master) = "A" (boot drive)
    drive_count = 0;

    drives[0].name[0] = 'A';
    drives[0].name[1] = '\0';
    drives[0].drive_id = 0;
    drives[0].present = false;
    drives[0].backend = DRIVE_BACKEND_NONE;

    // 1. Try IDE Master first if hardware detected
    if (ide_detect(IDE_DRIVE_MASTER)) {
        drives[0].backend = DRIVE_BACKEND_IDE;
        drives[0].ide_drive_sel = IDE_DRIVE_MASTER;
        drives[0].drive_id = IDE_DRIVE_MASTER;
        drives[0].present = true;
        ext2_init_drive(&drives[0]);
    }

    // 2. If IDE Master was not detected or failed, fallback to Limine boot module (RAM disk)
    if (!drives[0].present && mod_resp && mod_resp->module_count > 0) {
        for (u64 i = 0; i < mod_resp->module_count; i++) {
            struct limine_file *mod = mod_resp->modules[i];
            if (!mod || !mod->address || mod->size == 0) continue;

            drives[0].backend = DRIVE_BACKEND_RAMDISK;
            drives[0].ram_base = (u8 *)mod->address;
            drives[0].ram_size = mod->size;
            drives[0].drive_id = (u8)(i + 1);
            drives[0].present = true;
            ext2_init_drive(&drives[0]);
            if (drives[0].present) {
                serial_outsf("EXT2: Mounted Limine boot module '%s' (%lld KiB) as RAMDISK drive 'A'\n",
                             mod->path ? mod->path : "disk.img", (long long)(mod->size / 1024));
                break;
            }
        }
    }

    if (drives[0].present) drive_count++;

    // Drive 1 = "B" (persistent data drive if IDE Slave is present)
    drives[1].name[0] = 'B';
    drives[1].name[1] = '\0';
    drives[1].drive_id = IDE_DRIVE_SLAVE;
    drives[1].backend = DRIVE_BACKEND_IDE;
    drives[1].ide_drive_sel = IDE_DRIVE_SLAVE;
    drives[1].present = ide_detect(IDE_DRIVE_SLAVE);
    if (drives[1].present) {
        ext2_init_drive(&drives[1]);
        if (drives[1].present) drive_count++;
    }

    // Activate drive 0 (A) by default if present
    if (drives[0].present) {
        ext2_switch_drive(&drives[0]);
        serial_outsf("EXT2: Active drive set to '%s' (%s)\n",
                     drives[0].name,
                     (drives[0].backend == DRIVE_BACKEND_RAMDISK) ? "RAMDISK" : "IDE");
    } else {
        serial_outsl("EXT2: WARNING - No root filesystem (drive 'A') found!");
    }
}

ext2_inode_t *ext2_get_inode(u32 inode_no, ext2_inode_t *out_inode) {
    if (inode_no == 0 || !out_inode || block_size == 0 || inode_no > sb_ptr->total_inodes) {
        return null;
    }

    ext2_bgd_t *bgdt = (ext2_bgd_t *) bgdt_cache;
    u32 group = (inode_no - 1) / sb_ptr->inodes_per_group;
    u32 index = (inode_no - 1) % sb_ptr->inodes_per_group;

    u32 inode_table_block = bgdt[group].inode_table;
    u32 inode_size = (sb_ptr->major_version >= 1) ? sb_ptr->inode_size : 128;
    u32 copy_size = (inode_size < sizeof(ext2_inode_t)) ? inode_size : sizeof(ext2_inode_t);

    u32 block_offset = (index * inode_size) / block_size;
    u32 final_block = inode_table_block + block_offset;
    u32 offset_in_block = (index * inode_size) % block_size;

    u8 *buf = kmalloc(block_size);
    ext2_read_block(final_block, buf);
    mem_copy((u8 *) out_inode, buf + offset_in_block, copy_size);
    kfree(buf);

    return out_inode;
}

u0 ext2_write_inode(u32 inode_no, ext2_inode_t *inode) {
    if (inode_no == 0 || !inode || block_size == 0 || inode_no > sb_ptr->total_inodes) {
        return;
    }

    ext2_bgd_t *bgdt = (ext2_bgd_t *) bgdt_cache;
    u32 group = (inode_no - 1) / sb_ptr->inodes_per_group;
    u32 index = (inode_no - 1) % sb_ptr->inodes_per_group;

    u32 inode_table_block = bgdt[group].inode_table;
    u32 inode_size = (sb_ptr->major_version >= 1) ? sb_ptr->inode_size : 128;
    u32 copy_size = (inode_size < sizeof(ext2_inode_t)) ? inode_size : sizeof(ext2_inode_t);

    u32 block_offset = (index * inode_size) / block_size;
    u32 final_block = inode_table_block + block_offset;
    u32 offset_in_block = (index * inode_size) % block_size;

    u8 *buf = kmalloc(block_size);
    if (!buf) return;
    
    ext2_read_block(final_block, buf);
    mem_copy(buf + offset_in_block, (u8 *) inode, copy_size);
    ext2_write_block(final_block, buf);
    kfree(buf);
}

// --- bmap cache: avoids re-reading indirect blocks thousands of times ---
// For a 14MB WAD with 1KB blocks, single-indirect is read 14K+ times without cache
static u32   bmap_cached_block   = 0;  // indirect block number cached
static u32  *bmap_cached_table   = null; // pointer to kmalloc'd table copy
static u32   bmap_cached_d_block = 0;  // double-indirect block cached
static u32  *bmap_cached_d_table = null;

static u32 *bmap_get_indirect(u32 block_no) {
    if (block_no == 0) return null;
    if (block_no == bmap_cached_block && bmap_cached_table)
        return bmap_cached_table;
    if (!bmap_cached_table) bmap_cached_table = kmalloc(block_size);
    if (!bmap_cached_table) return null;
    ext2_read_block(block_no, (u8 *)bmap_cached_table);
    bmap_cached_block = block_no;
    return bmap_cached_table;
}

static u32 *bmap_get_double_indirect(u32 d_block_no) {
    if (d_block_no == 0) return null;
    if (d_block_no == bmap_cached_d_block && bmap_cached_d_table)
        return bmap_cached_d_table;
    if (!bmap_cached_d_table) bmap_cached_d_table = kmalloc(block_size);
    if (!bmap_cached_d_table) return null;
    ext2_read_block(d_block_no, (u8 *)bmap_cached_d_table);
    bmap_cached_d_block = d_block_no;
    return bmap_cached_d_table;
}

u32 ext2_get_bmap(ext2_inode_t *inode, u32 logical_block) {
    if (block_size == 0) return 0;
    u32 n = block_size / 4;

    if (logical_block < 12) {
        return inode->block[logical_block];
    }
    logical_block -= 12;

    if (logical_block < n) {
        u32 *table = bmap_get_indirect(inode->block[12]);
        if (!table) return 0;
        return table[logical_block];
    }
    logical_block -= n;

    if (logical_block < n * n) {
        u32 *d_table = bmap_get_double_indirect(inode->block[13]);
        if (!d_table) return 0;
        u32 indirect_block = d_table[logical_block / n];
        if (indirect_block == 0) return 0;
        u32 *table = bmap_get_indirect(indirect_block);
        if (!table) return 0;
        return table[logical_block % n];
    }
    logical_block -= n * n;

    if (logical_block < n * n * n) {
        u32 t_block = inode->block[14];
        if (t_block == 0) return 0;
        // triple-indirect: rare enough to not cache
        u32 *t_table = kmalloc(block_size);
        ext2_read_block(t_block, (u8 *) t_table);
        u32 d_indirect_block = t_table[logical_block / (n * n)];
        kfree(t_table);
        if (d_indirect_block == 0) return 0;
        u32 *d_table = bmap_get_double_indirect(d_indirect_block);
        if (!d_table) return 0;
        u32 indirect_block = d_table[(logical_block / n) % n];
        if (indirect_block == 0) return 0;
        u32 *table = bmap_get_indirect(indirect_block);
        if (!table) return 0;
        return table[logical_block % n];
    }

    return 0;
}

/* ext2_set_bmap — inverse of ext2_get_bmap: write a physical block number
   into the inode's block pointer table at logical_block. Allocates indirect
   blocks on demand. Caller is responsible for allocating the data block
   via ext2_alloc_block() and passing it as phys_block. */
void ext2_set_bmap(ext2_inode_t *inode, u32 logical_block, u32 phys_block) {
    if (block_size == 0 || phys_block == 0) return;
    u32 n = block_size / 4;  /* entries per indirect block */

    /* allocate_and_clear — helper lambda (manual inline) */
    #define ALLOC_AND_CLEAR(target) do {                                 \
        (target) = ext2_alloc_block();                                   \
        if ((target) == 0) return;                                       \
        u8 *zbuf = kmalloc(block_size);                                  \
        if (zbuf) { mem_set(zbuf, 0, block_size);                        \
                    ext2_write_block((target), zbuf); kfree(zbuf); }     \
        else { u8 zsec[512]; mem_set(zsec, 0, 512);                      \
               for (u32 _s = 0; _s < block_size/512; _s++)               \
                   drive_write_sectors(active_drive,(target)*(block_size/512)+_s,1,zsec);} \
        inode->blocks += block_size / 512;                               \
    } while(0)

    if (logical_block < 12) {
        if (inode->block[logical_block] == 0)
            inode->blocks += block_size / 512;
        inode->block[logical_block] = phys_block;
        return;
    }
    logical_block -= 12;

    if (logical_block < n) {
        /* single indirect */
        if (inode->block[12] == 0) ALLOC_AND_CLEAR(inode->block[12]);
        u8 *buf = kmalloc(block_size);
        if (!buf) return;
        ext2_read_block(inode->block[12], buf);
        if (((u32*)buf)[logical_block] == 0) inode->blocks += block_size / 512;
        ((u32*)buf)[logical_block] = phys_block;
        ext2_write_block(inode->block[12], buf);
        kfree(buf);
        return;
    }
    logical_block -= n;

    if (logical_block < n * n) {
        /* double indirect */
        if (inode->block[13] == 0) ALLOC_AND_CLEAR(inode->block[13]);
        u32 d_idx = logical_block / n;
        u32 i_idx = logical_block % n;
        u8 *d_buf = kmalloc(block_size);
        if (!d_buf) return;
        ext2_read_block(inode->block[13], d_buf);
        u32 *d_table = (u32*)d_buf;
        if (d_table[d_idx] == 0) ALLOC_AND_CLEAR(d_table[d_idx]);
        u8 *i_buf = kmalloc(block_size);
        if (i_buf) {
            ext2_read_block(d_table[d_idx], i_buf);
            if (((u32*)i_buf)[i_idx] == 0) inode->blocks += block_size / 512;
            ((u32*)i_buf)[i_idx] = phys_block;
            ext2_write_block(d_table[d_idx], i_buf);
            kfree(i_buf);
        }
        ext2_write_block(inode->block[13], d_buf);
        kfree(d_buf);
        return;
    }
    logical_block -= n * n;

    if (logical_block < n * n * n) {
        /* triple indirect */
        if (inode->block[14] == 0) ALLOC_AND_CLEAR(inode->block[14]);
        u32 t_idx = logical_block / (n * n);
        u32 d_idx = (logical_block / n) % n;
        u32 i_idx = logical_block % n;
        u8 *t_buf = kmalloc(block_size);
        if (!t_buf) return;
        ext2_read_block(inode->block[14], t_buf);
        u32 *t_table = (u32*)t_buf;
        if (t_table[t_idx] == 0) ALLOC_AND_CLEAR(t_table[t_idx]);
        u8 *d_buf = kmalloc(block_size);
        if (!d_buf) { kfree(t_buf); return; }
        ext2_read_block(t_table[t_idx], d_buf);
        u32 *d_table = (u32*)d_buf;
        if (d_table[d_idx] == 0) ALLOC_AND_CLEAR(d_table[d_idx]);
        u8 *i_buf = kmalloc(block_size);
        if (i_buf) {
            ext2_read_block(d_table[d_idx], i_buf);
            if (((u32*)i_buf)[i_idx] == 0) inode->blocks += block_size / 512;
            ((u32*)i_buf)[i_idx] = phys_block;
            ext2_write_block(d_table[d_idx], i_buf);
            kfree(i_buf);
        }
        ext2_write_block(t_table[t_idx], d_buf);
        kfree(d_buf);
        ext2_write_block(inode->block[14], t_buf);
        kfree(t_buf);
        return;
    }

    #undef ALLOC_AND_CLEAR
}

static u32 ext2_find_child(ext2_inode_t *dir_inode, const char *name) {
    if ((dir_inode->mode & 0xF000) != 0x4000) return 0;

    u8 *dir_data = kmalloc(block_size);
    if (!dir_data) return 0;

    u32 total_blocks = (dir_inode->size + block_size - 1) / block_size;

    for (u32 b = 0; b < total_blocks; b++) {
        u32 phys_block = ext2_get_bmap(dir_inode, b);
        if (phys_block == 0) continue;

        ext2_read_block(phys_block, dir_data);
        u32 cur_offset = 0;
        while (cur_offset < block_size) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *) (dir_data + cur_offset);
            if (entry->rec_len < 8) break;

            if (entry->inode != 0 && str_eql((char *) name, entry->name, entry->name_len)) {
                u32 result = entry->inode;
                kfree(dir_data);
                return result;
            }
            cur_offset += entry->rec_len;
        }
    }
    kfree(dir_data);
    return 0;
}

ext2_inode_t *ext2_find_path(const char *path, u32 *out_inode_no) {
    if (!path || *path == '\0' || block_size == 0) return null;

    // ── //drive prefix: cross to a different drive ──────────────────────
    if (path[0] == '/' && path[1] == '/') {
        const char *drive_start = path + 2;

        // Extract drive name (up to next '/' or end of string)
        const char *scan = drive_start;
        while (*scan && *scan != '/') scan++;
        u32 name_len = (u32)(scan - drive_start);

        if (name_len == 0) {
            // "//" with no drive name — synthetic root, handled at command level
            return null;
        }

        // Look up drive by name
        for (u8 i = 0; i < drive_count; i++) {
            if (!drives[i].present) continue;
            // Compare name_len chars and ensure whole-names match
            u32 dn_len = 0;
            while (dn_len < 32 && drives[i].name[dn_len] != '\0') dn_len++;
            if (name_len == dn_len) {
                bool match = true;
                for (u32 j = 0; j < name_len; j++) {
                    if (drive_start[j] != drives[i].name[j]) { match = false; break; }
                }
                if (match) {
                    ext2_switch_drive(&drives[i]);
                    // Advance path past the drive name
                    path = (*scan == '/') ? scan : "/";
                    goto drive_switched;
                }
            }
        }
        // No matching drive found — // was present but drive name invalid
        return null;
    drive_switched:;
    }

    // ── Normal path resolution (same regardless of drive) ───────────────
    u32 current_inode_no = 2;
    u32 actual_inode_size = max(sizeof(ext2_inode_t), (u32) sb_ptr->inode_size);
    ext2_inode_t *current_inode = kmalloc(actual_inode_size);
    if (!current_inode) return null;

    if (path[0] == '/' && path[1] == '\0') {
        if (out_inode_no) *out_inode_no = 2;
        return ext2_get_inode(2, current_inode);
    }

    const char *ptr = path;
    if (*ptr == '/') ptr++;

    char name_buf[256];
    while (*ptr) {
        int i = 0;
        while (*ptr && *ptr != '/' && i < 255) {
            name_buf[i++] = *ptr++;
        }
        name_buf[i] = '\0';

        if (i > 0) {
            if (!ext2_get_inode(current_inode_no, current_inode)) {
                kfree(current_inode);
                return null;
            }
            current_inode_no = ext2_find_child(current_inode, name_buf);
            if (current_inode_no == 0) {
                kfree(current_inode);
                return null;
            }
        }
        if (*ptr == '/') ptr++;
    }

    if (out_inode_no) *out_inode_no = current_inode_no;
    return ext2_get_inode(current_inode_no, current_inode);
}


u0 ext2_explorer_init(ext2_explorer_t *explorer, u32 start_inode) {
    explorer->stack_ptr = 0;
    explorer->stack[0].inode_no = start_inode;
    explorer->stack[0].block_idx = 0;
    explorer->stack[0].offset = 0;

    explorer->block_buf = kmalloc(block_size);
    u32 actual_inode_size = max(sizeof(ext2_inode_t), (u32) sb_ptr->inode_size);
    explorer->inode_buf = kmalloc(actual_inode_size);
    explorer->last_read_block = 0;
}

u0 ext2_explorer_deinit(ext2_explorer_t *explorer) {
    if (explorer->block_buf) kfree(explorer->block_buf);
    if (explorer->inode_buf) kfree(explorer->inode_buf);
    explorer->block_buf = null;
    explorer->inode_buf = null;
}

bool ext2_explorer_next(ext2_explorer_t *explorer, ext2_explore_result_t *result) {
    if (block_size == 0 || !explorer->block_buf || !explorer->inode_buf) return false;

    while (explorer->stack_ptr >= 0) {
        ext2_stack_frame_t *frame = &explorer->stack[explorer->stack_ptr];

        if (!ext2_get_inode(frame->inode_no, explorer->inode_buf)) {
            explorer->stack_ptr--;
            continue;
        }

        u32 total_blocks = (explorer->inode_buf->size + block_size - 1) / block_size;
        while (frame->block_idx < total_blocks) {
            u32 block_id = ext2_get_bmap(explorer->inode_buf, frame->block_idx);
            if (block_id == 0) {
                frame->block_idx++;
                frame->offset = 0;
                continue;
            }

            if (explorer->last_read_block != block_id) {
                ext2_read_block(block_id, explorer->block_buf);
                explorer->last_read_block = block_id;
            }

            if (frame->offset >= block_size) {
                frame->block_idx++;
                frame->offset = 0;
                continue;
            }

            ext2_dir_entry_t *entry = (ext2_dir_entry_t *) (explorer->block_buf + frame->offset);
            if (entry->rec_len < 8 || frame->offset + entry->rec_len > block_size) {
                frame->block_idx = total_blocks;
                break;
            }

            u32 entry_inode = entry->inode;
            u8 entry_type = entry->file_type;
            u32 entry_rec_len = entry->rec_len;
            u8 entry_name_len = entry->name_len;
            frame->offset += entry_rec_len;

            if (entry_inode == 0) continue;

            // Skip . and ..
            if (entry_name_len == 1 && entry->name[0] == '.') continue;
            if (entry_name_len == 2 && entry->name[0] == '.' && entry->name[1] == '.') continue;

            // Check for circular directory loops in the recursion stack
            bool already_in_stack = false;
            for (int s = 0; s <= explorer->stack_ptr; s++) {
                if (explorer->stack[s].inode_no == entry_inode) {
                    already_in_stack = true;
                    break;
                }
            }
            if (already_in_stack) continue;

            // Fallback for ext2 filesystems without EXT2_FEATURE_INCOMPAT_FILETYPE
            if (entry_type == 0) {
                ext2_inode_t temp_ino;
                if (ext2_get_inode(entry_inode, &temp_ino)) {
                    if ((temp_ino.mode & 0xF000) == 0x4000) entry_type = 2; // Directory
                    else if ((temp_ino.mode & 0xF000) == 0x8000) entry_type = 1; // Regular file
                }
            }

            mem_set(result->name, 0, 256);
            mem_copy((u8 *) result->name, (u8 *) entry->name, entry_name_len);
            result->inode_no = entry_inode;
            result->file_type = entry_type;
            result->is_dir = (entry_type == 2);
            result->depth = (u8) explorer->stack_ptr;

            if (entry_type == 2 && explorer->stack_ptr < 15) {
                explorer->stack_ptr++;
                explorer->stack[explorer->stack_ptr].inode_no = entry_inode;
                explorer->stack[explorer->stack_ptr].block_idx = 0;
                explorer->stack[explorer->stack_ptr].offset = 0;
            }

            return true;
        }
        explorer->stack_ptr--;
    }

    return false;
}

// ============================================================================
// File creation: allocate inodes, blocks, and add directory entries.
// Minimal implementation for wat2wasm output and iterative development.
// ============================================================================

// Find and allocate a free inode. Returns 0 if no free inodes.
static u32 ext2_alloc_inode(void) {
    if (!bgdt_cache || block_size == 0 || !sb_ptr) return 0;

    ext2_bgd_t *bgdt = (ext2_bgd_t *)bgdt_cache;
    u32 inodes_per_group = sb_ptr->inodes_per_group;
    u32 groups = (sb_ptr->total_blocks + sb_ptr->blocks_per_group - 1)
                 / sb_ptr->blocks_per_group;

    for (u32 g = 0; g < groups && g < 256; g++) {
        u32 bitmap_block = bgdt[g].inode_bitmap;
        if (bitmap_block == 0) continue;

        u8 *bitmap = kmalloc(block_size);
        if (!bitmap) continue;
        ext2_read_block(bitmap_block, bitmap);

        u32 start_bit = (g == 0) ? sb_ptr->first_inode - 1 : 0;
        u32 max_bits = inodes_per_group;
        if (max_bits > block_size * 8) max_bits = block_size * 8;

        for (u32 bit = start_bit; bit < max_bits; bit++) {
            u32 byte_idx = bit / 8;
            u8  bit_mask = 1 << (bit % 8);
            if (!(bitmap[byte_idx] & bit_mask)) {
                bitmap[byte_idx] |= bit_mask;
                ext2_write_block(bitmap_block, bitmap);
                bgdt[g].free_inodes_count--;
                kfree(bitmap);
                return g * inodes_per_group + bit + 1;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

// Find and allocate a free data block. Returns 0 if no free blocks.
u32 ext2_alloc_block(void) {
    if (!bgdt_cache || block_size == 0 || !sb_ptr) return 0;

    ext2_bgd_t *bgdt = (ext2_bgd_t *)bgdt_cache;
    u32 blocks_per_group = sb_ptr->blocks_per_group;
    u32 groups = (sb_ptr->total_blocks + blocks_per_group - 1)
                 / blocks_per_group;

    for (u32 g = 0; g < groups && g < 256; g++) {
        u32 bitmap_block = bgdt[g].block_bitmap;
        if (bitmap_block == 0) continue;

        u8 *bitmap = kmalloc(block_size);
        if (!bitmap) continue;
        ext2_read_block(bitmap_block, bitmap);

        u32 max_bits = blocks_per_group;
        if (max_bits > block_size * 8) max_bits = block_size * 8;

        for (u32 bit = 0; bit < max_bits; bit++) {
            u32 byte_idx = bit / 8;
            u8  bit_mask = 1 << (bit % 8);
            if (!(bitmap[byte_idx] & bit_mask)) {
                bitmap[byte_idx] |= bit_mask;
                ext2_write_block(bitmap_block, bitmap);
                bgdt[g].free_blocks_count--;
                kfree(bitmap);
                return g * blocks_per_group + bit;
            }
        }
        kfree(bitmap);
    }
    return 0;
}

// Create a regular file with one data block in the given parent directory.
// Returns the new inode number, or 0 on failure.
// The parent inode's directory block is modified to include the new entry.
static u32 ext2_create_file_in_dir(const char *name, u32 parent_inode_no) {
    if (!name || name[0] == 0 || parent_inode_no == 0) return 0;

    u32 name_len = str_len(name);
    if (name_len > 250) return 0;  // ext2 max name length

    // Allocate a new inode
    u32 new_inode_no = ext2_alloc_inode();
    if (new_inode_no == 0) {
        serial_outsf("ext2_create: no free inode for %s\n", name);
        return 0;
    }

    // Allocate one data block
    u32 data_block = ext2_alloc_block();
    if (data_block == 0) {
        serial_outsf("ext2_create: no free block for %s\n", name);
        // Release the inode we just allocated
        ext2_bgd_t *bgdt = (ext2_bgd_t *)bgdt_cache;
        u32 g = (new_inode_no - 1) / sb_ptr->inodes_per_group;
        u32 bit = (new_inode_no - 1) % sb_ptr->inodes_per_group;
        u32 bitmap_block = bgdt[g].inode_bitmap;
        if (bitmap_block) {
            u8 *ib = kmalloc(block_size);
            if (ib) {
                ext2_read_block(bitmap_block, ib);
                ib[bit / 8] &= ~(1 << (bit % 8));
                ext2_write_block(bitmap_block, ib);
                bgdt[g].free_inodes_count++;
                kfree(ib);
            }
        }
        return 0;
    }

    // Initialize the new inode (regular file, 0644)
    u32 actual_inode_size = max(sizeof(ext2_inode_t), (u32)sb_ptr->inode_size);
    ext2_inode_t *new_inode = kmalloc(actual_inode_size);
    if (!new_inode) return 0;
    mem_set((u8*)new_inode, 0, actual_inode_size);

    new_inode->mode = 0x81A4;          // regular file, 0644 permissions
    new_inode->size = 0;               // empty file — grows via fs_write
    new_inode->blocks = block_size / 512;  // sectors count
    new_inode->block[0] = data_block;
    new_inode->links_count = 1;

    ext2_write_inode(new_inode_no, new_inode);
    kfree(new_inode);

    // Add directory entry to parent
    ext2_inode_t parent_inode_buf;
    if (!ext2_get_inode(parent_inode_no, &parent_inode_buf)) {
        serial_outsf("ext2_create: cannot read parent inode %u\n", parent_inode_no);
        return 0;
    }

    // Calculate entry size needed: header(8) + name_len, padded to 4-byte align
    u32 entry_size = 8 + name_len;
    entry_size = (entry_size + 3) & ~3u;  // 4-byte align

    // Read the last directory block (or first if empty) to append the entry
    u32 total_blocks = parent_inode_buf.size > 0
        ? (parent_inode_buf.size + block_size - 1) / block_size
        : 1;

    // If no blocks yet, allocate one for the parent directory
    if (parent_inode_buf.size == 0 || ext2_get_bmap(&parent_inode_buf, 0) == 0) {
        u32 dir_block = ext2_alloc_block();
        if (dir_block == 0) return 0;
        parent_inode_buf.block[0] = dir_block;
        parent_inode_buf.size = block_size;
        parent_inode_buf.blocks = block_size / 512;
    }

    u32 last_block_idx = total_blocks - 1;
    u32 phys_block = ext2_get_bmap(&parent_inode_buf, last_block_idx);
    if (phys_block == 0) return 0;

    u8 *dir_buf = kmalloc(block_size);
    if (!dir_buf) return 0;
    ext2_read_block(phys_block, dir_buf);

    // Find the last entry in the directory block
    u32 cur_off = 0;
    u32 last_real_entry_off = 0;
    ext2_dir_entry_t *last_entry = null;

    while (cur_off < block_size) {
        ext2_dir_entry_t *e = (ext2_dir_entry_t *)(dir_buf + cur_off);
        if (e->rec_len < 8) break;
        if (e->inode != 0) {
            last_real_entry_off = cur_off;
            last_entry = e;
        }
        if (cur_off + e->rec_len >= block_size) break;
        cur_off += e->rec_len;
    }

    // Where does the new entry go?
    // If we have a last entry, shrink its rec_len to its actual size and place
    // the new entry after it. Otherwise start at offset 0.
    u32 new_entry_off;
    if (last_entry) {
        u32 actual_size = 8 + last_entry->name_len;
        actual_size = (actual_size + 3) & ~3u;
        if (last_entry->rec_len >= actual_size + entry_size) {
            // Enough slack space in the last entry's record
            u32 remaining = last_entry->rec_len - actual_size;
            last_entry->rec_len = actual_size;
            new_entry_off = last_real_entry_off + actual_size;
            entry_size = remaining;  // take all remaining space
        } else {
            kfree(dir_buf);
            serial_outsl("ext2_create: no room in directory block");
            return 0;
        }
    } else {
        new_entry_off = 0;
        // Take the whole block as one entry (only entry)
        entry_size = block_size;
    }

    if (new_entry_off + entry_size > block_size) {
        kfree(dir_buf);
        return 0;
    }

    // Write the new directory entry
    ext2_dir_entry_t *new_entry = (ext2_dir_entry_t *)(dir_buf + new_entry_off);
    new_entry->inode = new_inode_no;
    new_entry->rec_len = entry_size;
    new_entry->name_len = name_len;
    new_entry->file_type = 1;  // regular file
    mem_copy((u8*)new_entry->name, (const u8*)name, name_len);

    ext2_write_block(phys_block, dir_buf);
    kfree(dir_buf);

    // Update parent inode
    ext2_write_inode(parent_inode_no, &parent_inode_buf);

    serial_outsf("ext2_create: created %s (inode %u, block %u)\n",
                 name, new_inode_no, data_block);
    return new_inode_no;
}

// Create a file at the given path (must be direct child of root "/")
// Returns 0 on failure, new inode number on success.
u32 ext2_create_file(const char *path) {
    if (!path || path[0] == 0 || block_size == 0) return 0;

    // For MVP: only support creating files in root directory
    // Parse: /filename or just filename
    const char *name = path;
    if (name[0] == '/') name++;
    if (name[0] == 0) return 0;

    // Validate: no subdirectories in path
    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            serial_outsf("ext2_create: nested paths not supported: %s\n", path);
            return 0;
        }
    }

    return ext2_create_file_in_dir(name, 2);  // 2 = root inode
}

u8 *get_block_ptr(u32 block_id) {
    if (block_size == 0) return null;
    u8 *buf = kmalloc(block_size);
    if (buf) ext2_read_block(block_id, buf);
    return buf;
}
