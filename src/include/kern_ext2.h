//
// Created by tobin on 2025-12-01.
//

#ifndef KERN_EXT2_H
#define KERN_EXT2_H


#include "dialect.h"
#include "kern_serial.h"
#include "../../limine/limine.h"

#define EXT2_SIGNATURE 0xEF53

// Inode file-type bits (mode & 0xF000)
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFLNK  0xA000

// Directory entry file_type values (EXT2_FT_*)
#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR     2
#define EXT2_FT_SYMLINK 7


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

// ── Multi-drive support ───────────────────────────────────────────────────

#define MAX_DRIVES 4

typedef enum {
    DRIVE_BACKEND_NONE = 0,
    DRIVE_BACKEND_IDE,
    DRIVE_BACKEND_RAMDISK
} drive_backend_t;

typedef struct {
    char            name[32];               // "A", "data", etc.
    drive_backend_t backend;                // DRIVE_BACKEND_IDE or DRIVE_BACKEND_RAMDISK
    u8              ide_drive_sel;          // IDE_DRIVE_MASTER or IDE_DRIVE_SLAVE
    u8              drive_id;               // Unique drive identifier for block cache
    u8             *ram_base;               // Base pointer for RAM disk
    u64             ram_size;               // Size in bytes for RAM disk
    bool            present;                // true if drive was detected
    ext2_superblock_t sb;                   // per-drive superblock
    u32             block_size;             // per-drive block size (1024 << sb.block_size)
    u8             *bgdt_cache;             // per-drive block group descriptor table
} drive_t;

extern drive_t drives[MAX_DRIVES];
extern u8      drive_count;
extern drive_t *active_drive;

// Current working directory (for cd/ls). Includes drive prefix when on non-A drives.
// e.g. "/", "/wasm", "//B/", "//B/wasm"
extern char cwd[256];


u0 ext2_init(struct limine_module_response *mod_resp);
u0 ext2_init_drive(drive_t *d);
u0 ext2_switch_drive(drive_t *d);
ext2_inode_t *find_file_in_root(char *target_name);
ext2_inode_t *ext2_find_path(const char *path, u32 *out_inode_no);
ext2_inode_t *ext2_get_inode(u32 inode_no, ext2_inode_t *out_inode);
u0 ext2_read_block(u32 block_id, u8 *buffer);
u0 ext2_read_blocks(u32 start_block, u32 count, u8 *buffer);
u0 ext2_write_block(u32 block_id, u8 *buffer);
u0 ext2_write_inode(u32 inode_no, ext2_inode_t *inode);
u8 *get_block_ptr(u32 block_id);
u32 ext2_get_bmap(ext2_inode_t *inode, u32 logical_block);
u32 ext2_alloc_block(void);
void ext2_set_bmap(ext2_inode_t *inode, u32 logical_block, u32 phys_block);

// Block cache — runtime-tunable LRU cache at the ext2_read_block level.
void block_cache_set_capacity(u32 entries);
void block_cache_stats(u32 *out_hits, u32 *out_misses, u32 *out_used, u32 *out_cap);

// File creation
u32 ext2_create_file(const char *path);

// Symlinks
u32  ext2_create_symlink(const char *path, const char *target);
bool ext2_read_symlink_target(ext2_inode_t *inode, char *out, u32 out_size);

// Explorer API
u0 ext2_explorer_init(ext2_explorer_t *explorer, u32 start_inode);
u0 ext2_explorer_deinit(ext2_explorer_t *explorer);
bool ext2_explorer_next(ext2_explorer_t *explorer, ext2_explore_result_t *result);

#endif //KERN_EXT2_H
