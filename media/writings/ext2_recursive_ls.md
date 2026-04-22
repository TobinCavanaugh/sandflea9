# Recursive LS Implementation for EXT2

To explore an EXT2 filesystem recursively, you need to treat the directory structure as a tree where **Inodes** are nodes and **Directory Entries** are the edges.

## 1. The Strategy: Depth-First Search (DFS)

The most efficient way to "discover" everything is to:
1. Start at a given Inode (usually Root, Inode #2).
2. List all entries in that Inode's data blocks.
3. For every entry that is a **Directory**, immediately "dive" into it by calling the function again.

## 2. Handling the "Infinite Loop" Trap
Every EXT2 directory contains two special entries:
- `.` (Current Directory)
- `..` (Parent Directory)

**CRITICAL:** When recursing, you **MUST** skip these two names. If you don't, your kernel will enter an infinite loop (e.g., `/` -> `/.` -> `/.` ... or `/` -> `/folder` -> `/folder/..` -> `/folder`).

## 3. Implementation Logic

Here is how you should structure the code:

```c
void ext2_ls_recursive(u32 inode_no, int depth) {
    ext2_inode_t* inode = ext2_get_inode(inode_no);
    
    // 1. Safety check: Is this actually a directory?
    if ((inode->mode & 0xF000) != 0x4000) return;

    // 2. Iterate through data blocks
    for (int b = 0; b < 12; b++) {
        if (inode->block[b] == 0) break;
        
        u8* data = get_block_ptr(inode->block[b]);
        u32 offset = 0;
        
        while (offset < block_size) {
            ext2_dir_entry_t* entry = (ext2_dir_entry_t*)(data + offset);
            if (entry->rec_len == 0) break;
            
            // Null-terminate the name for printing
            char name[256];
            mem_copy(name, entry->name, entry->name_len);
            name[entry->name_len] = '\0';

            // 3. Print with indentation for depth
            for(int i=0; i<depth; i++) serial_outs("  ");
            serial_outsf("|-- %s (%s)\n", name, (entry->file_type == 2) ? "DIR" : "FILE");

            // 4. Recurse if it's a directory AND not . or ..
            if (entry->file_type == 2) {
                if (!str_eq(name, ".") && !str_eq(name, "..")) {
                    ext2_ls_recursive(entry->inode, depth + 1);
                }
            }

            offset += entry->rec_len;
        }
    }
}
```

## 4. Key Performance Tips

1.  **Use `entry->file_type`**: The directory entry structure already tells you if an item is a file (1) or a directory (2). Use this instead of loading the child's inode immediately; it saves a lot of memory access.
2.  **Indentation**: Passing a `depth` integer allows you to visualize the tree structure (using spaces or `|--` prefixes).
3.  **Path Resolution**: You can combine this with your `ext2_find_path` to start the recursion from any subfolder (e.g., `ls -R /home/user`).

## 5. Challenges to Solve Next
- **Indirect Blocks**: Large directories might use `inode->block[12]`. Your iterator will need to load that block and treat it as a list of block IDs to continue searching.
- **Buffer Limits**: Since you're printing to the screen, a recursive LS on a large drive will fly past very fast. You might eventually want a way to "pipe" this to your `screen_push_line` buffer.
