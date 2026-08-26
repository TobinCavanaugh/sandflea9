#include "../include/kern_fs.h"
#include "../include/kern_mem.h"
#include "../include/kern_ext2.h"
#include "../include/kern_serial.h"
#include "../include/kern_profile.h"
#include "../util/util_str.h"
#include "../include/kern_vmm.h"
#include "../include/kern_sched.h"
#include "../include/kern_terminal.h"

// TODO Async I/O implementation

u0 fs_init() {
    serial_outsl("FS: Per-process handle system ready");
}

static i32 allocate_fd(kern_process_t *proc) {
    if (!proc) return -1;
    // Reserve fds 0, 1, 2 for stdin/stdout/stderr. Host functions
    // (wasm_fd_read / wasm_fd_write) special-case these to route to the
    // keyboard / screen regardless of the fd_table, so any other process
    // that opens a file MUST land at fd >= 3 to avoid colliding with the
    // console fds. Without this, the very first file a WASM program opens
    // would clobber stdin and reads would block on the keyboard.
    for (i32 i = 3; i < MAX_FILE_HANDLES; i++) {
        if (proc->fd_table[i] == null) {
            return i;
        }
    }
    return -1;
}

// Internal helper to get physical block ID from logical block index
static u32 get_bmap(ext2_inode_t *inode, u32 logical_block) {
    return ext2_get_bmap(inode, logical_block);
}


i32 fs_write(i32 fd, u8 *data, u64 size) {
    PROFILE_SCOPE("fs:write");
    kern_process_t *proc = sched_get_current_process();
    if (!proc) return -1;

    if (fd < 0 || fd >= MAX_FILE_HANDLES || proc->fd_table[fd] == null || !data) return -1;

    file_handle_t *h = proc->fd_table[fd];

    extern u32 block_size;
    if (block_size == 0) return -1;

    u32 bytes_written = 0;
    u8 *temp_block = kmalloc(block_size);
    if (!temp_block) return -1;

    while (bytes_written < size) {
        u32 logical_block = h->pos / block_size;
        u32 offset_in_block = h->pos % block_size;
        u32 phys_block = get_bmap(&h->inode, logical_block);

        if (phys_block == 0) {
            // Auto-allocate a new block (on-demand growth)
            phys_block = ext2_alloc_block();
            if (phys_block == 0) break;  /* disk full */
            ext2_set_bmap(&h->inode, logical_block, phys_block);
            /* New block: start with zeros, don't read garbage from disk */
            mem_set(temp_block, 0, block_size);
        } else {
            /* Existing block: read-modify-write */
            ext2_read_block(phys_block, temp_block);
        }

        u32 to_write = block_size - offset_in_block;
        if (to_write > (size - bytes_written)) to_write = size - bytes_written;

        // Modify the block content
        mem_copy(temp_block + offset_in_block, data + bytes_written, to_write);

        // Write the block back
        ext2_write_block(phys_block, temp_block);

        bytes_written += to_write;
        h->pos += to_write;

        // Update the in-memory size if we grew the file
        if (h->pos > h->inode.size) {
            h->inode.size = h->pos;
        }
    }

    kfree(temp_block);

    // Persist the updated inode back to disk
    ext2_write_inode(h->inode_no, &h->inode);

    return bytes_written;
}

// Create a new file. Returns fd >= 0 on success, -1 on failure.
// Only supports files in the root directory (no nested paths yet).
i32 fs_create(const char *path) {
    kern_process_t *proc = sched_get_current_process();
    if (!proc) return -1;

    u32 inode_no = ext2_create_file(path);
    if (inode_no == 0) return -1;

    // Now open the newly created file
    i32 fd = allocate_fd(proc);
    if (fd == -1) return -1;

    ext2_inode_t inode;
    if (!ext2_get_inode(inode_no, &inode)) return -1;

    file_handle_t *h = kmalloc(sizeof(file_handle_t));
    if (!h) return -1;
    h->inode_no = inode_no;
    mem_copy((u8 *)&h->inode, (u8 *)&inode, sizeof(ext2_inode_t));
    h->pos = 0;
    h->used = true;
    proc->fd_table[fd] = h;
    return fd;
}

// str_view_t wrapper for fs_create.
i32 fs_create_view(str_view_t path_sv) {
    char *path = str_view_to_c(path_sv);
    if (!path) return -1;
    i32 result = fs_create(path);
    kfree(path);
    return result;
}

// Returns -1 if file could not be opened
i32 fs_open(const char *path) {
    kern_process_t *proc = sched_get_current_process();
    if (!proc) return -1;

    u32 inode_no = 0;
    ext2_inode_t *inode_ptr = ext2_find_path(path, &inode_no);
    if (!inode_ptr) return -1;

    i32 fd = allocate_fd(proc);
    if (fd == -1) {
        kfree(inode_ptr);
        return -1;
    }

    file_handle_t *h = kmalloc(sizeof(file_handle_t));
    h->inode_no = inode_no;
    mem_copy((u8 *) &h->inode, (u8 *) inode_ptr, sizeof(ext2_inode_t));
    h->pos = 0;
    h->used = true;

    proc->fd_table[fd] = h;
    kfree(inode_ptr);
    return fd;
}

// str_view_t wrapper for fs_open.
i32 fs_open_view(str_view_t path_sv) {
    char *path = str_view_to_c(path_sv);
    if (!path) return -1;
    i32 result = fs_open(path);
    kfree(path);
    return result;
}

i32 fs_close(i32 fd) {
    kern_process_t *proc = sched_get_current_process();
    if (!proc) return -1;

    if (fd < 0 || fd >= MAX_FILE_HANDLES || proc->fd_table[fd] == null) return -1;

    kfree(proc->fd_table[fd]);
    proc->fd_table[fd] = null;
    return 0;
}

i32 fs_read(i32 fd, u8 *buf, u32 count) {
    PROFILE_SCOPE("fs:read");
    kern_process_t *proc = sched_get_current_process();
    if (!proc) return -1;

    if (fd < 0 || fd >= MAX_FILE_HANDLES || proc->fd_table[fd] == null || !buf) return -1;

    file_handle_t *h = proc->fd_table[fd];
    if (h->pos >= h->inode.size) return 0;

    if (h->pos + count > h->inode.size) {
        count = h->inode.size - h->pos;
    }

    extern u32 block_size;
    if (block_size == 0) return -1;

    u32 bytes_read = 0;
    u8 *temp_block = kmalloc(block_size);
    if (!temp_block) return -1;

    // --- Phase 1: handle first partial block if not block-aligned ---
    u32 offset_in_block = h->pos % block_size;
    if (offset_in_block != 0) {
        u32 logical_block = h->pos / block_size;
        u32 phys_block = get_bmap(&h->inode, logical_block);
        if (phys_block == 0) {
            mem_set(buf, 0, count);  // sparse file → zero-fill
            h->pos += count;
            kfree(temp_block);
            return count;
        }
        ext2_read_block(phys_block, temp_block);
        u32 to_copy = block_size - offset_in_block;
        if (to_copy > count) to_copy = count;
        mem_copy(buf, temp_block + offset_in_block, to_copy);
        bytes_read += to_copy;
        h->pos += to_copy;
    }

    // --- Phase 2: batch-read remaining full blocks ---
    #define FS_BATCH_BLOCKS 64
    u8 *batch_buf = kmalloc(block_size * FS_BATCH_BLOCKS);
    if (!batch_buf) {
        // fallback: single-block reads
        while (bytes_read < count) {
            u32 logical_block = h->pos / block_size;
            u32 phys_block = get_bmap(&h->inode, logical_block);
            if (phys_block == 0) { mem_set(buf + bytes_read, 0, count - bytes_read); break; }
            ext2_read_block(phys_block, temp_block);
            u32 to_copy = block_size;
            if (to_copy > count - bytes_read) to_copy = count - bytes_read;
            mem_copy(buf + bytes_read, temp_block, to_copy);
            bytes_read += to_copy;
            h->pos += to_copy;
        }
    } else {
        while (bytes_read < count) {
            u32 logical_block = h->pos / block_size;
            u32 phys_block = get_bmap(&h->inode, logical_block);
            if (phys_block == 0) { mem_set(buf + bytes_read, 0, count - bytes_read); break; }

            // Determine run length of physically contiguous blocks
            u32 run_blocks = 1;
            u32 max_run = (count - bytes_read) / block_size;
            if (max_run > FS_BATCH_BLOCKS) max_run = FS_BATCH_BLOCKS;

            for (u32 r = 1; r < max_run; r++) {
                u32 next_phys = get_bmap(&h->inode, logical_block + r);
                if (next_phys != phys_block + r) break;
                run_blocks++;
            }

            u32 run_bytes = run_blocks * block_size;
            if (run_bytes > count - bytes_read) run_bytes = count - bytes_read;

            if (run_blocks == 1) {
                ext2_read_block(phys_block, temp_block);
                u32 to_copy = (run_bytes < block_size) ? run_bytes : block_size;
                mem_copy(buf + bytes_read, temp_block, to_copy);
            } else {
                ext2_read_blocks(phys_block, run_blocks, batch_buf);
                mem_copy(buf + bytes_read, batch_buf, run_bytes);
            }
            bytes_read += run_bytes;
            h->pos += run_bytes;
        }
        kfree(batch_buf);
    }
    #undef FS_BATCH_BLOCKS

    kfree(temp_block);
    return bytes_read;
}

i32 fs_seek(i32 fd, i32 offset, seek_type_t whence) {
    kern_process_t *proc = sched_get_current_process();
    if (!proc) return -1;

    if (fd < 0 || fd >= MAX_FILE_HANDLES || proc->fd_table[fd] == null) return -1;

    file_handle_t *h = proc->fd_table[fd];
    u32 new_pos = h->pos;

    switch (whence) {
        case SEEK_SET:
            new_pos = offset;
            break;
        case SEEK_CUR:
            new_pos = h->pos + offset;
            break;
        case SEEK_END:
            new_pos = h->inode.size + offset;
            break;
        default:
            return -1;
    }

    if (new_pos > h->inode.size) new_pos = h->inode.size;
    h->pos = new_pos;
    return (i32) h->pos;
}

u32 fs_tell(i32 fd) {
    kern_process_t *proc = sched_get_current_process();
    if (!proc || fd < 0 || fd >= MAX_FILE_HANDLES || proc->fd_table[fd] == null) return 0;
    return proc->fd_table[fd]->pos;
}

u32 fs_size(i32 fd) {
    kern_process_t *proc = sched_get_current_process();
    if (!proc || fd < 0 || fd >= MAX_FILE_HANDLES || proc->fd_table[fd] == null) return 0;
    return proc->fd_table[fd]->inode.size;
}

const char *fs_get_cwd(void) {
    kern_process_t *proc = sched_get_current_process();
    if (proc && proc->cwd[0] != '\0') {
        return proc->cwd;
    }
    if (active_session && active_session->cwd[0] != '\0') {
        return active_session->cwd;
    }
    if (cwd[0] != '\0') {
        return cwd;
    }
    return "//A/";
}

u0 fs_set_cwd(const char *new_cwd) {
    if (!new_cwd || new_cwd[0] == '\0') return;

    // If new_cwd has a drive prefix (e.g. "//B/" or "//A/"), switch active drive
    if (new_cwd[0] == '/' && new_cwd[1] == '/') {
        const char *dstart = new_cwd + 2;
        const char *dend = dstart;
        while (*dend && *dend != '/') dend++;
        u32 dlen = (u32)(dend - dstart);
        for (u8 i = 0; i < drive_count; i++) {
            if (!drives[i].present) continue;
            u32 dl = str_len(drives[i].name);
            if (dlen == dl && str_eql(dstart, drives[i].name, dlen)) {
                ext2_switch_drive(&drives[i]);
                break;
            }
        }
    }

    kern_process_t *proc = sched_get_current_process();
    if (proc && proc != sched_get_kernel_process()) {
        u32 len = str_len(new_cwd);
        if (len >= sizeof(proc->cwd)) len = sizeof(proc->cwd) - 1;
        mem_copy((u8*)proc->cwd, (const u8*)new_cwd, len);
        proc->cwd[len] = '\0';
        return;
    }

    if (active_session) {
        u32 len = str_len(new_cwd);
        if (len >= sizeof(active_session->cwd)) len = sizeof(active_session->cwd) - 1;
        mem_copy((u8*)active_session->cwd, (const u8*)new_cwd, len);
        active_session->cwd[len] = '\0';
    }

    kern_process_t *kproc = sched_get_kernel_process();
    if (kproc) {
        u32 len = str_len(new_cwd);
        if (len >= sizeof(kproc->cwd)) len = sizeof(kproc->cwd) - 1;
        mem_copy((u8*)kproc->cwd, (const u8*)new_cwd, len);
        kproc->cwd[len] = '\0';
    }

    u32 len = str_len(new_cwd);
    if (len >= sizeof(cwd)) len = sizeof(cwd) - 1;
    mem_copy((u8*)cwd, (const u8*)new_cwd, len);
    cwd[len] = '\0';
}

u0 fs_resolve_path(const char *path, char *out, u32 out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';

    const char *current_cwd = fs_get_cwd();
    if (!current_cwd || current_cwd[0] == '\0') current_cwd = "//A/";

    char combined[512];
    combined[0] = '\0';

    if (!path || path[0] == '\0') {
        u32 len = str_len(current_cwd);
        if (len >= out_size) len = out_size - 1;
        mem_copy((u8*)out, (const u8*)current_cwd, len);
        out[len] = '\0';
        return;
    }

    // Check if path has an explicit drive prefix "//drive/..." or "//drive"
    if (path[0] == '/' && path[1] == '/') {
        u32 len = str_len(path);
        if (len >= sizeof(combined)) len = sizeof(combined) - 1;
        mem_copy((u8*)combined, (const u8*)path, len);
        combined[len] = '\0';
    } else if (path[0] == '/') {
        // Absolute path from root of current drive: extract drive prefix from current_cwd
        char drive_pref[32];
        drive_pref[0] = '\0';
        if (current_cwd[0] == '/' && current_cwd[1] == '/') {
            const char *p = current_cwd + 2;
            while (*p && *p != '/') p++;
            u32 dlen = (u32)(p - current_cwd);
            if (dlen >= sizeof(drive_pref)) dlen = sizeof(drive_pref) - 1;
            mem_copy((u8*)drive_pref, (const u8*)current_cwd, dlen);
            drive_pref[dlen] = '\0';
        } else {
            drive_pref[0] = '/';
            drive_pref[1] = '/';
            drive_pref[2] = (active_drive && active_drive->name[0]) ? active_drive->name[0] : 'A';
            drive_pref[3] = '\0';
        }

        u32 dlen = str_len(drive_pref);
        mem_copy((u8*)combined, (const u8*)drive_pref, dlen);
        u32 plen = str_len(path);
        if (dlen + plen >= sizeof(combined)) plen = sizeof(combined) - 1 - dlen;
        mem_copy((u8*)combined + dlen, (const u8*)path, plen);
        combined[dlen + plen] = '\0';
    } else {
        // Relative path — prepend current_cwd
        u32 cwd_len = str_len(current_cwd);
        if (cwd_len >= sizeof(combined)) cwd_len = sizeof(combined) - 1;
        mem_copy((u8*)combined, (const u8*)current_cwd, cwd_len);
        combined[cwd_len] = '\0';

        if (cwd_len > 0 && combined[cwd_len - 1] != '/') {
            if (cwd_len < sizeof(combined) - 1) {
                combined[cwd_len++] = '/';
                combined[cwd_len] = '\0';
            }
        }

        u32 path_len = str_len(path);
        if (cwd_len + path_len >= sizeof(combined)) path_len = sizeof(combined) - 1 - cwd_len;
        mem_copy((u8*)combined + cwd_len, (const u8*)path, path_len);
        combined[cwd_len + path_len] = '\0';
    }

    // Canonicalize 'combined':
    // 1. Extract drive prefix if "//..."
    char drive_prefix[32];
    drive_prefix[0] = '\0';
    const char *scan = combined;

    if (combined[0] == '/' && combined[1] == '/') {
        const char *dp_end = combined + 2;
        while (*dp_end && *dp_end != '/') dp_end++;
        u32 dlen = (u32)(dp_end - combined);
        if (dlen >= sizeof(drive_prefix)) dlen = sizeof(drive_prefix) - 1;
        mem_copy((u8*)drive_prefix, (const u8*)combined, dlen);
        drive_prefix[dlen] = '\0';
        scan = dp_end;
    } else {
        drive_prefix[0] = '/';
        drive_prefix[1] = '/';
        drive_prefix[2] = (active_drive && active_drive->name[0]) ? active_drive->name[0] : 'A';
        drive_prefix[3] = '\0';
    }

    // 2. Tokenize segments
    const char *segments[32];
    u32 seg_lens[32];
    u32 depth = 0;

    while (*scan) {
        while (*scan == '/') scan++;
        if (!*scan) break;

        const char *seg_start = scan;
        while (*scan && *scan != '/') scan++;
        u32 slen = (u32)(scan - seg_start);

        if (slen == 1 && seg_start[0] == '.') {
            continue;
        }
        if (slen == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
            if (depth > 0) depth--;
            continue;
        }

        if (depth < 32) {
            segments[depth] = seg_start;
            seg_lens[depth] = slen;
            depth++;
        }
    }

    // 3. Assemble normalized output: "//<drive>/" or "//<drive>/seg1/seg2"
    u32 pos = 0;
    u32 dlen = str_len(drive_prefix);
    if (dlen >= out_size) dlen = out_size - 1;
    mem_copy((u8*)out, (const u8*)drive_prefix, dlen);
    pos = dlen;

    if (depth == 0) {
        if (pos < out_size - 1) out[pos++] = '/';
    } else {
        for (u32 i = 0; i < depth; i++) {
            if (pos < out_size - 1) out[pos++] = '/';
            u32 slen = seg_lens[i];
            if (pos + slen >= out_size) slen = out_size - 1 - pos;
            mem_copy((u8*)out + pos, (const u8*)segments[i], slen);
            pos += slen;
        }
    }
    out[pos] = '\0';
}

