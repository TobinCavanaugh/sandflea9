#ifndef KERN_FS_H
#define KERN_FS_H

#include "dialect.h"
#include "kern_ext2.h"
#include "../util/str_slice.h"

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

// str_view_t overloads — thin wrappers that convert to C string, call the
// real fs_open/fs_create, then free. Use these when you have a str_view_t
// (e.g., from cmd_word_view) to avoid manual str_view_to_c + kfree.
i32 fs_open_view(str_view_t path);
i32 fs_create_view(str_view_t path);

// C-string API — primary implementation, zero conversion overhead.
i32 fs_open(const char *path);
i32 fs_create(const char *path);
i32 fs_close(i32 fd);
i32 fs_write(i32 fd, u8 *data, u64 size);
i32 fs_read(i32 fd, u8 *buf, u32 count);
i32 fs_seek(i32 fd, i32 offset, seek_type_t whence);
u32 fs_tell(i32 fd);
u32 fs_size(i32 fd);

// Current working directory & path resolution
const char *fs_get_cwd(void);
u0          fs_set_cwd(const char *new_cwd);
u0          fs_resolve_path(const char *path, char *out, u32 out_size);

#endif // KERN_FS_H
