#ifndef KERN_FS_H
#define KERN_FS_H

#include "dialect.h"
#include "kern_ext2.h"

#define MAX_FILE_HANDLES 128

typedef enum {
    SEEK_SET = 0,
    SEEK_CUR = 1,
    SEEK_END = 2
} seek_type_t;

typedef struct {
    u32 inode_no;
    ext2_inode_t inode;
    u32 pos;
    bool used;
} file_handle_t;

u0 fs_init();
i32 fs_open(const char *path);
i32 fs_close(i32 fd);
i32 fs_write(i32 fd, u8 *data, u64 size);
i32 fs_read(i32 fd, u8 *buf, u32 count);
i32 fs_seek(i32 fd, i32 offset, seek_type_t whence);
u32 fs_tell(i32 fd);
u32 fs_size(i32 fd);

#endif // KERN_FS_H
