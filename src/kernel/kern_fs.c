#include "../include/kern_fs.h"
#include "../include/kern_mem.h"
#include "../include/kern_ext2.h"
#include "../include/kern_serial.h"
#include "../util/util_str.h"
#include "../include/kern_vmm.h"
#include "../include/kern_sched.h"

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
            // Cannot allocate new blocks, so we stop here.
            break;
        }

        // Read the existing block content
        ext2_read_block(phys_block, temp_block);

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
    h->used = true; // Still keep 'used' for internal checks if needed

    proc->fd_table[fd] = h;

    kfree(inode_ptr);
    return fd;
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
