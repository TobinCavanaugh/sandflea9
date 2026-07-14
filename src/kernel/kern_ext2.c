//
// Created by tobin on 2025-12-01.
//

#include "../include/kern_ext2.h"
#include "../include/kern_mem.h"
#include "../include/kern_ide.h"
#include "../include/kern_vmm.h"
#include "../util/util_str.h"

// --- Logging Control ---
#define EXT2_DEBUG 0
#define DEBUG_LOG(expr) do { if (EXT2_DEBUG) { expr; } } while (0)

ext2_superblock_t sb_static;
ext2_superblock_t *sb_ptr = &sb_static;
u32 block_size = 0;
u8 *bgdt_cache = null; // Optimized: Cache the BGDT in RAM

u0 ext2_read_block(u32 block_id, u8 *buffer) {
    if (!buffer || block_size == 0) return;
    u32 sectors_per_block = block_size / 512;
    u32 start_sector = block_id * sectors_per_block;
    ide_read_sectors(start_sector, sectors_per_block, buffer);
}

u0 ext2_read_blocks(u32 start_block, u32 count, u8 *buffer) {
    if (!buffer || block_size == 0 || count == 0) return;
    if (count == 1) { ext2_read_block(start_block, buffer); return; }
    u32 sectors_per_block = block_size / 512;
    u32 start_sector = start_block * sectors_per_block;
    u32 total_sectors = count * sectors_per_block;
    // ide_read_sectors takes u8 count; split into chunks if needed (max 255 sectors per call)
    while (total_sectors > 0) {
        u8 chunk = (total_sectors > 255) ? 255 : (u8)total_sectors;
        ide_read_sectors(start_sector, chunk, buffer);
        total_sectors -= chunk;
        start_sector += chunk;
        buffer += chunk * 512;
    }
}

u0 ext2_write_block(u32 block_id, u8 *buffer) {
    if (!buffer || block_size == 0) return;
    u32 sectors_per_block = block_size / 512;
    u32 start_sector = block_id * sectors_per_block;
    ide_write_sectors(start_sector, sectors_per_block, buffer);
}

u0 ext2_init() {
    u8 sb_buf[1024];
    ide_read_sectors(2, 2, sb_buf);
    mem_copy((u8 *) sb_ptr, sb_buf, sizeof(ext2_superblock_t));

    if (sb_ptr->magic_signature == EXT2_SIGNATURE) {
        serial_outsl("EXT2: Valid filesystem detected via IDE.");
        block_size = 1024 << sb_ptr->block_size;

        // Cache BGDT
        u32 bgdt_block = (block_size == 1024) ? 2 : 1;
        bgdt_cache = kmalloc(block_size);
        ext2_read_block(bgdt_block, bgdt_cache);

        serial_outsf("EXT2: Block size %d bytes, BGDT cached.\n", block_size);
    } else {
        serial_outsl("EXT2: Invalid magic signature on IDE drive!");
        block_size = 0;
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
static u32 ext2_alloc_block(void) {
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
