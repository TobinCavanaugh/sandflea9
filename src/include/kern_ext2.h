//
// Created by tobin on 2025-12-01.
//

#ifndef KERN_EXT2_H
#define KERN_EXT2_H


#include "dialect.h"
#include "kern_serial.h"
#include "../../limine/limine.h"

#define EXT2_SIGNATURE 0xEF53


// Superblock
typedef struct {
    u32 total_inodes, total_blocks, reserved_blocks, free_blocks, free_inodes, superblock_block_number, block_size,
            fragment_size, blocks_per_group, fragments_per_group, inodes_per_group, mount_time, write_time;
    u16 mount_count, max_mount_count, magic_signature, state, error_behavior, minor_version;
    u32 last_check, check_interval, creator_os, major_version;
    u16 uid_reserved, gid_reserved;

    u32 first_inode; // Offset 84 (First non-reserved inode)
    u16 inode_size; // Offset 88 (Size of inode structure)
    u16 block_group_nr; // Offset 90
    u32 feature_compat; // Offset 92
    u32 feature_incompat; // Offset 96
    u32 feature_ro_compat; // Offset 100
    u8 uuid[16]; // Offset 104
    char volume_name[16]; // Offset 120
    char last_mounted[64]; // Offset 136
    u32 algo_bitmap; // Offset 200
} __attribute__((packed)) ext2_superblock_t;

// Block group descriptor
typedef struct {
    u32 block_bitmap; // id of block
    u32 inode_bitmap; // id of inode usage
    u32 inode_table; // id of start of inode table
    u16 free_blocks_count, free_inodes_count, used_dirs_count;
    u16 pad;
    u32 reserved[3];
} __attribute__((packed)) ext2_bgd_t;

// Inode
typedef struct {
    u16 mode; // Perms and type (file / dir /etc)
    u16 uid;
    u32 size;
    u32 atime; // access
    u32 ctime; // creation
    u32 mtime; // modification
    u32 dtime; // deletion

    u16 gid;
    u16 links_count;

    u32 blocks; // 512 byte blocks
    u32 flags;
    u32 osd1;

    u32 block[15]; // 12 direct pointers, 3 indirect

    u32 generation;
    u32 file_acl;
    u32 dir_acl;
    u32 faddr;
    u8 osd2[12];
} __attribute__((packed)) ext2_inode_t;

typedef struct {
    u32 inode;
    u16 rec_len; // length of this entire entry
    u8 name_len; // length of name
    u8 file_type; //1=file 2=dir
    char name[]; // no null term
} __attribute__((packed)) ext2_dir_entry_t;

// Explorer/Iterator Structures
typedef struct {
    u32 inode_no;
    u32 block_idx;
    u32 offset;
} ext2_stack_frame_t;

typedef struct {
    ext2_stack_frame_t stack[16];
    int stack_ptr;
    u8 *block_buf;      // Cache for current directory block
    ext2_inode_t *inode_buf; // Cache for current inode
    u32 last_read_block; // Track last read block to avoid redundant IDE reads
} ext2_explorer_t;

typedef struct {
    u32 inode_no;
    u8 file_type;
    u8 is_dir;
    u8 depth;
    char name[256];
} ext2_explore_result_t;


u0 ext2_init();
ext2_inode_t *find_file_in_root(char *target_name);
ext2_inode_t *ext2_find_path(const char *path, u32 *out_inode_no);
ext2_inode_t *ext2_get_inode(u32 inode_no, ext2_inode_t *out_inode);
u0 ext2_read_block(u32 block_id, u8 *buffer);
u0 ext2_read_blocks(u32 start_block, u32 count, u8 *buffer);
u0 ext2_write_block(u32 block_id, u8 *buffer);
u0 ext2_write_inode(u32 inode_no, ext2_inode_t *inode);
u8 *get_block_ptr(u32 block_id);
u32 ext2_get_bmap(ext2_inode_t *inode, u32 logical_block);

// Explorer API
u0 ext2_explorer_init(ext2_explorer_t *explorer, u32 start_inode);
u0 ext2_explorer_deinit(ext2_explorer_t *explorer);
bool ext2_explorer_next(ext2_explorer_t *explorer, ext2_explore_result_t *result);

#endif //KERN_EXT2_H
