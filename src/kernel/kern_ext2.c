//
// Created by tobin on 2025-12-01.
//

#include "kern_ext2.h"
#include "../util/util_str.h"

u8 name_equals(char *name, char *entry_name, u8 entry_len) {
    i32 name_len = str_len(name);
    if (name_len != entry_len) return 0;

    for (i32 i = 0; i < name_len; i++) {
        if (name[i] != entry_name[i]) return 0;
    }

    return 1;
}

ext2_superblock_t *sb_ptr = 0;
u8 *fs_base = 0;
u32 block_size = 0;

u0 ext2_init(struct limine_module_request module_request) {
    if (module_request.response == NULL || module_request.response->module_count < 1) {
        serial_outs("Missing disk image, no modules found!\n");
        return;
    }

    u8 *base = null;
    if (module_request.response && module_request.response->module_count > 0) {
        struct limine_file *disk = module_request.response->modules[0];
        u8 *disk_base = (u8 *) disk->address;
        ext2_superblock_t *sb = (ext2_superblock_t *) (disk_base + 1024); // superblock offset

        if (sb->magic_signature == EXT2_SIGNATURE) {
            serial_outs("Valid EXT2 file!\n");
            u32 block_size = 1024 << sb->block_size;

            serial_outs("Block size: ");
            serial_outi64(block_size, BASE_10);
            serial_outs("\n");

            base = disk_base;
        } else {
            serial_outs("Invalid EXT2 file!\n");
        }
    }

    fs_base = base;
    sb_ptr = (ext2_superblock_t *) (base + 1024);
    block_size = 1024 << sb_ptr->block_size;
    serial_outs("Initialized ext2\n");

    ////

    struct limine_file *disk_module = module_request.response->modules[0];
    u64 disk_addr = (u64) disk_module->address;
    u64 disk_size = disk_module->size;
    serial_outs("Disk image loaded at: 0x");
    serial_outu64(disk_addr, BASE_HEX);
    serial_outs("\n");

    serial_outs("Disk image size is : ");
    serial_outu64(disk_size, BASE_10);
    serial_outs("B\n");
}

// Get memory address of block id
u8 *get_block_ptr(u32 block_id) {
    return fs_base + (block_id * block_size);
}

//ext2_inode_t **ext2_list_files() {
//
//    // Load root
//    u32 bgdt_block = (block_size == 1024) ? 2 : 1; // if in block 1, our bgdt root is at 2, otherwise its at 1
//    ext2_bgd_t *bgdt = (ext2_bgd_t *) get_block_ptr(bgdt_block);
//    u8 *inode_table_ptr = get_block_ptr(bgdt->inode_table);
//    u32 inode_size = 128;
//
//    if (sb_ptr->major_version >= 1) {
//        inode_size = sb_ptr->inode_size;
//    }
//
//    // Get the first entry of our inode table, our root
//    ext2_inode_t *root_inode = (ext2_inode_t *) (inode_table_ptr + (inode_size * (2 - 1)));
//
//    u8 *dir_data = get_block_ptr(root_inode->block[0]);
//
//    // This is assuming our directory fits in one block, so no indirection at all
//    u32 cur_offset = 0;
//    while (cur_offset < block_size) {
//        ext2_dir_entry_t *entry = (ext2_dir_entry_t *) (dir_data + cur_offset);
//        if (entry->rec_len == 0) break;
//    }
//}

ext2_inode_t *find_file_in_root(char *target_name) {
    // Load root
    u32 bgdt_block = (block_size == 1024) ? 2 : 1; // if in block 1, our bgdt root is at 2, otherwise its at 1
    ext2_bgd_t *bgdt = (ext2_bgd_t *) get_block_ptr(bgdt_block);

    // Get the inode table
    u8 *inode_table_ptr = get_block_ptr(bgdt->inode_table);

    // inode size varies based on version
    u32 inode_size = 128;
    if (sb_ptr->major_version >= 1) {
        inode_size = sb_ptr->inode_size;
    }

    serial_outs("inode size: ");
    serial_outi64(inode_size, 10);
    serial_outs("\n");

    // Get the first entry of our inode table, our root
    ext2_inode_t *root_inode = (ext2_inode_t *) (inode_table_ptr + (inode_size * (2 - 1)));

    u8 *dir_data = get_block_ptr(root_inode->block[0]);

    // This is assuming our directory fits in one block, so no indirection at all
    u32 cur_offset = 0;
    while (cur_offset < block_size) {
        ext2_dir_entry_t *entry = (ext2_dir_entry_t *) (dir_data + cur_offset);
        if (entry->rec_len == 0) break;
        if (name_equals(target_name, entry->name, entry->name_len)) {
            u32 file_inode_idx = entry->inode;
            return (ext2_inode_t *) (inode_table_ptr + (inode_size * (file_inode_idx - 1)));
        }

        cur_offset += entry->rec_len;
    }

    return 0;
}
