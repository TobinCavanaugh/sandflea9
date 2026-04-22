# Iterative EXT2 Traversal (The Iterator Pattern)

To explore a filesystem "on your own schedule" without using recursion, you need to maintain your own **Stack** in memory. This allows you to process one file/folder at a time and return control to the caller.

## 1. The Traversal State Structure

First, define a structure that holds the "address" of where the explorer currently is.

```c
typedef struct {
    u32 inode_no;    // Inode we are currently reading
    u32 block_idx;   // Which of the 12 blocks we are in
    u32 offset;      // Offset within that block
    u32 depth;       // Current folder depth
} ext2_stack_frame_t;

typedef struct {
    ext2_stack_frame_t stack[32]; // Max depth of 32 folders
    int stack_ptr;                // Current top of stack
} ext2_explorer_t;
```

## 2. The Logic: Manual DFS

Instead of the CPU's stack handling the "where was I?", your `ext2_explorer_t` does it.

### `ext2_explorer_init(explorer, start_inode)`
- Clear the stack.
- Push the `start_inode` onto the stack at index 0.
- Set `block_idx = 0`, `offset = 0`, `depth = 0`.

### `ext2_explorer_next(explorer, result_out)`
This function performs **one step** (finds the next entry) and returns:

1. **Get current frame**: `frame = &explorer->stack[explorer->stack_ptr]`.
2. **Read Inode**: Get the `ext2_inode_t` for `frame->inode_no`.
3. **Find Next Entry**:
   - Look at `block[frame->block_idx]` at `frame->offset`.
   - If `offset >= block_size`: increment `block_idx`, reset `offset = 0`.
   - If `block_idx >= 12`: **POP** the stack (we finished this directory). If stack is empty, return "DONE".
4. **Process Entry**:
   - If the entry is `.` or `..`, skip it (update `offset` and repeat step 3).
   - If the entry is a **Directory**:
     - Update `explorer->offset` so we don't find this same folder again next time.
     - **PUSH** a new frame for this directory onto the stack.
     - Return the directory info.
   - If the entry is a **File**:
     - Update `explorer->offset`.
     - Return the file info.

## 3. Usage Example

Now you can use this in your main loop or a command handler without blocking the whole system:

```c
ext2_explorer_t my_explorer;
ext2_explorer_init(&my_explorer, 2); // Start at root

while (ext2_explorer_next(&my_explorer, &entry_info)) {
    // Print the entry we found
    screen_push_linef("%*s |-- %s", my_explorer.stack_ptr * 2, "", entry_info.name);
    
    // You can break here and continue in the NEXT frame/turn!
    if (should_pause) break; 
}
```

## 4. Why this is better for a Kernel
1. **Non-Blocking**: You can process 5 files per frame, then let the rest of the OS run, then process 5 more.
2. **Memory Safety**: You control the stack size. If someone creates a directory 10,000 levels deep, your kernel won't crash; the explorer will just hit its `MAX_STACK` limit and stop.
3. **Stateful**: You can easily implement a "find" command that stops as soon as it finds a match, then resumes if the user wants "find next".
