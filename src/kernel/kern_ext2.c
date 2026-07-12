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

u32 ext2_get_bmap(ext2_inode_t *inode, u32 logical_block) {
    if (block_size == 0) return 0;
    u32 n = block_size / 4;

    if (logical_block < 12) {
        return inode->block[logical_block];
    }
    logical_block -= 12;

    if (logical_block < n) {
        u32 indirect_block = inode->block[12];
        if (indirect_block == 0) return 0;
        u32 *table = kmalloc(block_size);
        ext2_read_block(indirect_block, (u8 *) table);
        u32 phys = table[logical_block];
        kfree(table);
        return phys;
    }
    logical_block -= n;

    if (logical_block < n * n) {
        u32 d_indirect_block = inode->block[13];
        if (d_indirect_block == 0) return 0;
        u32 *d_table = kmalloc(block_size);
        ext2_read_block(d_indirect_block, (u8 *) d_table);
        u32 indirect_block = d_table[logical_block / n];
        kfree(d_table);
        if (indirect_block == 0) return 0;

        u32 *table = kmalloc(block_size);
        ext2_read_block(indirect_block, (u8 *) table);
        u32 phys = table[logical_block % n];
        kfree(table);
        return phys;
    }
    logical_block -= n * n;

    if (logical_block < n * n * n) {
        u32 t_indirect_block = inode->block[14];
        if (t_indirect_block == 0) return 0;
        u32 *t_table = kmalloc(block_size);
        ext2_read_block(t_indirect_block, (u8 *) t_table);
        u32 d_indirect_block = t_table[logical_block / (n * n)];
        kfree(t_table);
        if (d_indirect_block == 0) return 0;

        u32 *d_table = kmalloc(block_size);
        ext2_read_block(d_indirect_block, (u8 *) d_table);
        u32 indirect_block = d_table[(logical_block / n) % n];
        kfree(d_table);
        if (indirect_block == 0) return 0;

        u32 *table = kmalloc(block_size);
        ext2_read_block(indirect_block, (u8 *) table);
        u32 phys = table[logical_block % n];
        kfree(table);
        return phys;
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

u8 *get_block_ptr(u32 block_id) {
    if (block_size == 0) return null;
    u8 *buf = kmalloc(block_size);
    if (buf) ext2_read_block(block_id, buf);
    return buf;
}
