# Per-Process File Descriptor Management

In sandfleaOS, we are transitioning from a global file descriptor table to a per-process model. This ensures process isolation and allows the kernel to automatically clean up resources when a process exits.

## 1. The Architectural Shift

### Current State (Global Table)
- All processes share a single `static file_handle_t fd_table[MAX_FILE_HANDLES]` in `kern_fs.c`.
- If Process A opens a file, Process B can accidentally (or maliciously) read from it or close it if it guesses the FD index.
- If a process crashes, its files remain "open" in the global table forever.

### Target State (Per-Process Table)
- Every `kern_process_t` maintains its own private table of file handles.
- File descriptors are now relative to the process (e.g., FD 3 in Process A is different from FD 3 in Process B).
- The kernel can safely close all open handles during `process_exit`.

## 2. Implementation Strategy

### A. Updating `kern_process_t`
We will add the FD table to the process structure in `src/include/kern_sched.h`:

```c
typedef struct kern_process {
    i32 pid;
    u64 cr3;
    // ...
    file_handle_t* fd_table[MAX_FILE_HANDLES];
} kern_process_t;
```

### B. Modifying Filesystem Operations
Functions in `kern_fs.c` (like `fs_open`, `fs_read`, `fs_write`) will no longer use a global array. Instead, they will:
1. Call `sched_get_current_process()`.
2. Access the `fd_table` within that process.

### C. Resource Cleanup
In `process_exit`, the kernel will perform a cleanup sweep:

```c
void process_exit(kern_process_t *proc) {
    // ... free memory regions ...
    
    // Close all open files
    for (int i = 0; i < MAX_FILE_HANDLES; i++) {
        if (proc->fd_table[i]) {
            fs_close_for_process(proc, i);
        }
    }
}
```

## 3. Benefits
1. **Isolation:** Processes cannot interfere with each other's open files.
2. **Reliability:** No more "leaked" file handles on process crash/exit.
3. **Standardization:** This matches the behavior of POSIX-compliant operating systems.

## 4. Next Steps
1. Refactor `src/include/kern_sched.h` to include the FD table.
2. Update `src/kernel/kern_sched.c` to initialize and clean up the table.
3. Rewrite `src/kernel/kern_fs.c` to use the process-local table.
