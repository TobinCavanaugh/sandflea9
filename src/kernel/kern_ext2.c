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

u8 name_equals(char *name, char *entry_name, u8 entry_len) {
    i32 name_len = str_len(name);
    if (name_len != entry_len) return 0;

    for (i32 i = 0; i < name_len; i++) {
        if (name[i] != entry_name[i]) return 0;
    }

    return 1;
}

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

u0 ext2_init() {
    u8 sb_buf[1024];
    ide_read_sectors(2, 2, sb_buf); 
    mem_copy((u8*)sb_ptr, sb_buf, sizeof(ext2_superblock_t));

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
    
    u32 block_offset = (index * inode_size) / block_size;
    u32 final_block = inode_table_block + block_offset;
    u32 offset_in_block = (index * inode_size) % block_size;

    u8 *buf = kmalloc(block_size);
    ext2_read_block(final_block, buf);
    mem_copy((u8*)out_inode, buf + offset_in_block, inode_size);
    kfree(buf);

    return out_inode;
}

static u32 ext2_find_child(ext2_inode_t *dir_inode, const char *name) {
    if ((dir_inode->mode & 0xF000) != 0x4000) return 0;

    u8 *dir_data = kmalloc(block_size);
    if (!dir_data) return 0;

    for (int b = 0; b < 12; b++) {
        if (dir_inode->block[b] == 0) break;

        ext2_read_block(dir_inode->block[b], dir_data);
        u32 cur_offset = 0;
        while (cur_offset < block_size) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *) (dir_data + cur_offset);
            if (entry->rec_len < 8) break; 
            if (entry->inode != 0 && name_equals((char *) name, entry->name, entry->name_len)) {
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
    ext2_inode_t *current_inode = kmalloc(sizeof(ext2_inode_t));
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

ext2_inode_t *find_file_in_root(char *target_name) {
    if (target_name[0] == '/') {
        return ext2_find_path(target_name, null);
    } else {
        char path_buf[256];
        path_buf[0] = '/';
        int i = 0;
        while (target_name[i] && i < 254) {
            path_buf[i + 1] = target_name[i];
            i++;
        }
        path_buf[i + 1] = '\0';
        return ext2_find_path(path_buf, null);
    }
}

u0 ext2_explorer_init(ext2_explorer_t *explorer, u32 start_inode) {
    explorer->stack_ptr = 0;
    explorer->stack[0].inode_no = start_inode;
    explorer->stack[0].block_idx = 0;
    explorer->stack[0].offset = 0;
}

bool ext2_explorer_next(ext2_explorer_t *explorer, ext2_explore_result_t *result) {
    if (block_size == 0) return false;
    
    ext2_inode_t *current_inode = kmalloc(sizeof(ext2_inode_t));
    if (!current_inode) return false;

    u8 *dir_data = kmalloc(block_size);
    if (!dir_data) {
        kfree(current_inode);
        return false;
    }

    while (explorer->stack_ptr >= 0) {
        ext2_stack_frame_t *frame = &explorer->stack[explorer->stack_ptr];
        
        DEBUG_LOG(serial_outsf("LSR_STEP: Frame %d, Inode %d, BIdx %d, Off %d\n", 
                      explorer->stack_ptr, frame->inode_no, frame->block_idx, frame->offset));

        if (!ext2_get_inode(frame->inode_no, current_inode)) {
            DEBUG_LOG(serial_outsf("LSR_ERR: Failed to get inode %d\n", frame->inode_no));
            explorer->stack_ptr--;
            continue;
        }

        while (frame->block_idx < 12) {
            if (current_inode->block[frame->block_idx] == 0) {
                frame->block_idx = 12;
                break;
            }

            DEBUG_LOG(serial_outsf("LSR_READ: Reading block %d (ID %d)\n", frame->block_idx, current_inode->block[frame->block_idx]));
            ext2_read_block(current_inode->block[frame->block_idx], dir_data);
            
            if (frame->offset >= block_size) {
                frame->block_idx++;
                frame->offset = 0;
                continue;
            }

            ext2_dir_entry_t *entry = (ext2_dir_entry_t *) (dir_data + frame->offset);
            if (entry->rec_len < 8 || frame->offset + entry->rec_len > block_size) {
                DEBUG_LOG(serial_outsf("LSR_WARN: Invalid rec_len %d at offset %d\n", entry->rec_len, frame->offset));
                frame->block_idx = 12;
                break;
            }

            u32 entry_inode = entry->inode;
            u8 entry_type = entry->file_type;
            u32 entry_rec_len = entry->rec_len;
            frame->offset += entry_rec_len;

            if (entry_inode == 0) {
                continue;
            }
            
            mem_set(result->name, 0, 256);
            mem_copy((u8*)result->name, (u8*)entry->name, entry->name_len);
            result->inode_no = entry_inode;
            result->file_type = entry_type;
            result->is_dir = (entry_type == 2);
            result->depth = (u8)explorer->stack_ptr;

            if (name_equals(".", result->name, entry->name_len) || 
                name_equals("..", result->name, entry->name_len)) {
                continue;
            }

            if (entry_type == 2 && explorer->stack_ptr < 15) {
                DEBUG_LOG(serial_outsf("LSR_PUSH: Entering directory '%s' (Inode %d)\n", result->name, entry_inode));
                explorer->stack_ptr++;
                explorer->stack[explorer->stack_ptr].inode_no = entry_inode;
                explorer->stack[explorer->stack_ptr].block_idx = 0;
                explorer->stack[explorer->stack_ptr].offset = 0;
            } else {
                DEBUG_LOG(serial_outsf("LSR_FOUND: '%s'\n", result->name));
            }

            kfree(dir_data);
            kfree(current_inode);
            return true;
        }
        DEBUG_LOG(serial_outsf("LSR_POP: Finished inode %d\n", frame->inode_no));
        explorer->stack_ptr--;
    }

    kfree(dir_data);
    kfree(current_inode);
    return false;
}

u8 *get_block_ptr(u32 block_id) {
    if (block_size == 0) return null;
    u8 *buf = kmalloc(block_size);
    if (buf) ext2_read_block(block_id, buf);
    return buf;
}
