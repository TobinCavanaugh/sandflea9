# WASM Syscall Implementation Plan: File I/O

This document outlines the strategy for implementing file-related system calls (syscalls) in the sandfleaOS WebAssembly environment using the Wasm3 engine.

## Objective
Enable WebAssembly modules to perform basic file operations:
1. `open(path)` -> returns a file descriptor.
2. `write(fd, buffer, length)` -> writes data to an open file.

## 1. Host Function Mapping
Wasm3 allows "host functions" (C functions in the kernel) to be imported by the WASM module. We will map these imports to our kernel's VFS.

### Required Host Functions
We need to register the following functions in the WASM environment:

| WASM Import Name | C Kernel Function | Description |
| :--- | :--- | :--- |
| `sys_open` | `wasm_fs_open` | Wraps `fs_open` |
| `sys_write` | `wasm_fs_write` | Wraps `fs_write` (to be implemented) |
| `sys_close` | `wasm_fs_close` | Wraps `fs_close` |

## 2. Implementation Details

### Pointer Safety
WASM operates in its own linear memory. When a WASM module passes a string pointer (for a filename) or a buffer pointer (for writing data), the kernel must:
1. Get the base address of the WASM module's linear memory.
2. Validate the offset to ensure it doesn't read outside the module's bounds.
3. Translate the WASM offset to a kernel virtual address.

### The `wasm_fs_write` Stub
Currently, `kern_fs.c` only implements `fs_read`. To support `write`, we need to:
1. Implement block allocation in the Ext2 driver.
2. Update the inode's block list and size.
3. Write data to the underlying IDE device.

*Note: For the initial prototype, we may implement `sys_write` to output to the serial console (`serial_outs`) to verify the WASM-to-Kernel bridge before completing the Ext2 write logic.*

## 3. WASM Example (`file_test.wat`)
The following WebAssembly Text (WAT) snippet demonstrates how a module would interact with these syscalls:

```wat
(module
  ;; Import the host functions from the 'env' namespace
  (import "env" "sys_open" (func $open (param i32) (result i32)))
  (import "env" "sys_write" (func $write (param i32 i32 i32) (result i32)))

  ;; Data section for the filename and content
  (data (i32.const 1024) "test.txt\00")
  (data (i32.const 2048) "Hello from WebAssembly!\n")

  (func (export "run_file_test") (result i32)
    (local $fd i32)
    
    ;; 1. Open "test.txt"
    (call $open (i32.const 1024))
    local.set $fd
    
    ;; Check if open failed (fd < 0)
    local.get $fd
    i32.const 0
    i32.lt_s
    if
      i32.const -1
      return
    end

    ;; 2. Write content to the file descriptor
    (call $write (local.get $fd) (i32.const 2048) (i32.const 24))
    drop

    local.get $fd
  )
)
```

## 4. Next Steps
1. **Define the Linker Logic:** In `kern_wasm.c` (or wherever Wasm3 is initialized), load the module into the runtime and then use `m3_LinkRawFunction` to bind the imports.
2. **Implement Memory Resolution:** Use `m3_GetMemory` to access the WASM linear memory from the host function.
3. **Extend VFS:** Add `fs_write` to `kern_fs.c` and provide the necessary Ext2 infrastructure.
4. **Standardize Entry Point:** Use `entry` as the standard export name for the main execution logic in WASM modules.
