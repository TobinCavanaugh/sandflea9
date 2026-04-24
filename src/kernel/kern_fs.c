#include "../include/kern_fs.h"
#include "../include/kern_mem.h"
#include "../include/kern_ext2.h"
#include "../include/kern_serial.h"
#include "../util/util_str.h"
#include "../include/kern_vmm.h"

static file_handle_t fd_table[MAX_FILE_HANDLES];

u0 fs_init() {
    for (int i = 0; i < MAX_FILE_HANDLES; i++) {
        fd_table[i].used = false;
    }
    serial_outsl("FS: POSIX-like handle system initialized");
}

static i32 allocate_fd() {
    for (i32 i = 0; i < MAX_FILE_HANDLES; i++) {
        if (!fd_table[i].used) {
            fd_table[i].used = true;
            return i;
        }
    }
    return -1;
}

i32 fs_open(const char *path) {
    u32 inode_no = 0;
    ext2_inode_t *inode_ptr = ext2_find_path(path, &inode_no);
    if (!inode_ptr) return -1;

    i32 fd = allocate_fd();
    if (fd == -1) {
        kfree(inode_ptr);
        return -1;
    }

    fd_table[fd].inode_no = inode_no;
    mem_copy((u8*)&fd_table[fd].inode, (u8*)inode_ptr, sizeof(ext2_inode_t));
    fd_table[fd].pos = 0;
    
    kfree(inode_ptr);
    return fd;
}

i32 fs_close(i32 fd) {
    if (fd < 0 || fd >= MAX_FILE_HANDLES || !fd_table[fd].used) return -1;
    fd_table[fd].used = false;
    return 0;
}

// Internal helper to get physical block ID from logical block index
static u32 get_bmap(ext2_inode_t *inode, u32 logical_block) {
    extern u32 block_size;
    if (logical_block < 12) {
        return inode->block[logical_block];
    }
    
    // TODO: Implement indirect, double indirect, and triple indirect blocks
    // For now, only support direct blocks (first 12KB with 1KB blocks)
    return 0; 
}

i32 fs_read(i32 fd, u8 *buf, u32 count) {
    if (fd < 0 || fd >= MAX_FILE_HANDLES || !fd_table[fd].used || !buf) return -1;
    
    file_handle_t *h = &fd_table[fd];
    if (h->pos >= h->inode.size) return 0;
    
    if (h->pos + count > h->inode.size) {
        count = h->inode.size - h->pos;
    }

    extern u32 block_size;
    if (block_size == 0) return -1;

    u32 bytes_read = 0;
    u8 *temp_block = kmalloc(block_size);

    while (bytes_read < count) {
        u32 logical_block = h->pos / block_size;
        u32 offset_in_block = h->pos % block_size;
        u32 phys_block = get_bmap(&h->inode, logical_block);
        
        if (phys_block == 0) {
            // Sparse file or error
            mem_set(buf + bytes_read, 0, count - bytes_read); // Handle holes as zeros
            bytes_read = count;
            break;
        }

        ext2_read_block(phys_block, temp_block);
        
        u32 to_copy = block_size - offset_in_block;
        if (to_copy > (count - bytes_read)) to_copy = count - bytes_read;
        
        mem_copy(buf + bytes_read, temp_block + offset_in_block, to_copy);
        
        bytes_read += to_copy;
        h->pos += to_copy;
    }

    kfree(temp_block);
    return bytes_read;
}

i32 fs_seek(i32 fd, i32 offset, seek_type_t whence) {
    if (fd < 0 || fd >= MAX_FILE_HANDLES || !fd_table[fd].used) return -1;
    
    file_handle_t *h = &fd_table[fd];
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
    return (i32)h->pos;
}

u32 fs_tell(i32 fd) {
    if (fd < 0 || fd >= MAX_FILE_HANDLES || !fd_table[fd].used) return 0;
    return fd_table[fd].pos;
}

u32 fs_size(i32 fd) {
    if (fd < 0 || fd >= MAX_FILE_HANDLES || !fd_table[fd].used) return 0;
    return fd_table[fd].inode.size;
}
