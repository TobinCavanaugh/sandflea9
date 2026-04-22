# Implementing EXT2 Path Traversal

This document outlines the logic required to implement a path-based file search (e.g., `ext2_find_path("/dir/subdir/file.txt")`) using the existing structures in `kern_ext2.c`.

## 1. Core Components

Currently, we have:
- `get_block_ptr(block_id)`: Converts a block ID to a memory address.
- `ext2_inode_t`: Structure representing an inode, containing file size and block pointers (`block[15]`).
- `ext2_dir_entry_t`: Structure for directory entries (inode number, record length, name length, name).

## 2. General Strategy: The "Step-by-Step" Traversal

To find a file at `/a/b/c`, we must:
1. Start at the **Root Inode** (Inode #2).
2. Find directory `a` inside the Root Inode's data blocks.
3. Once found, load Inode `a`.
4. Find directory `b` inside `a`'s data blocks.
5. Load Inode `b`.
6. Find file `c` inside `b`'s data blocks.

## 3. Recommended Helper Functions

To make this clean, you should break the logic into smaller pieces:

### A. Get Inode by Number
Currently, `ext2_file_in_root` calculates the inode pointer inline. You should extract this:
```c
ext2_inode_t* ext2_get_inode(u32 inode_no) {
    // 1. Find which block group the inode belongs to
    u32 group = (inode_no - 1) / sb_ptr->inodes_per_group;
    u32 index = (inode_no - 1) % sb_ptr->inodes_per_group;

    // 2. Load the Block Group Descriptor (BGD) for that group
    // (Assuming simple layout for now)
    ext2_bgd_t *bgdt = (ext2_bgd_t *) get_block_ptr(bgdt_block); 
    
    // 3. Calculate address in the inode table
    u32 inode_size = (sb_ptr->major_version >= 1) ? sb_ptr->inode_size : 128;
    u8 *inode_table = get_block_ptr(bgdt[group].inode_table);
    
    return (ext2_inode_t *)(inode_table + (index * inode_size));
}
```

### B. Find Entry in Directory Inode
A function that searches a specific directory inode for a name:
```c
u32 ext2_find_child(ext2_inode_t* dir_inode, char* name) {
    // Iterate through all direct blocks (block[0] to block[11])
    for(int b = 0; b < 12; b++) {
        if(dir_inode->block[b] == 0) break;
        
        u8* dir_data = get_block_ptr(dir_inode->block[b]);
        u32 offset = 0;
        
        while(offset < block_size) {
            ext2_dir_entry_t* entry = (ext2_dir_entry_t*)(dir_data + offset);
            if(entry->rec_len == 0) break;
            
            if(name_equals(name, entry->name, entry->name_len)) {
                return entry->inode;
            }
            offset += entry->rec_len;
        }
    }
    return 0; // Not found
}
```

## 4. The Path Traversal Logic

With the helpers above, the path traversal becomes a loop:

1. **Tokenize the Path**: Split `"/dir1/dir2/file.txt"` into tokens: `["dir1", "dir2", "file.txt"]`.
2. **Initialize**: `current_inode_no = 2` (Root).
3. **Loop**: For each token:
   - `ext2_inode_t* current_dir = ext2_get_inode(current_inode_no)`
   - `current_inode_no = ext2_find_child(current_dir, token)`
   - If `current_inode_no == 0`, the path doesn't exist. Return NULL.
4. **Result**: After the loop, `current_inode_no` is the inode of the target file. Use `ext2_get_inode(current_inode_no)` to return the final inode pointer.

## 5. Limitations to Consider
- **Block Indirection**: The logic above only checks `block[0..11]`. Large directories use indirect blocks (`block[12..14]`).
- **Path Cleanup**: You'll need to handle leading slashes, trailing slashes, and double slashes (e.g., `//`).
- **File vs Directory**: Ensure you check `inode->mode` to verify that intermediate steps are actually directories.
