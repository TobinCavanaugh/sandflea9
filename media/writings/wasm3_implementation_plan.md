# Wasm3 Integration Plan for SandfleaOS

This document outlines the architectural changes and features required to implement `wasm3` as the primary execution engine for SandfleaOS.

## 1. Process Support (Infrastructure)

Currently, SandfleaOS supports only kernel threads sharing the same address space. To support isolated applications (even if interpreted by Wasm3), a proper process model is required.

### A. Virtual Address Space Management
- **Address Space Creation:** Implement a function to create a new PML4 table, mapping the kernel (higher half) and HHDM into every process while leaving lower half for user-space.
- **Switching:** Modify the scheduler to track the physical address of the PML4 for each task and update `CR3` during context switches.
- **User Heap:** Implement a per-process memory allocator for the lower-half address space.

### B. User Mode (Ring 3)
- **GDT Update:** Add User Code and User Data segments to the GDT.
- **TSS (Task State Segment):** Required for handling interrupts while in User Mode (specifically for switching back to the kernel stack).
- **Ring Transitions:** Implement `iretq` or `sysret` logic to enter Ring 3 and `syscall` logic to return to the kernel.

### C. Syscall Interface
- **Wasm-to-Kernel:** A mechanism for the Wasm3 interpreter (running either in kernel or user mode) to request OS services.
- **Standardization:** Map these syscalls to a subset of WASI (WebAssembly System Interface) for compatibility.

## 2. Wasm3 Runtime Integration

`wasm3` is a fast, lightweight interpreter that can be embedded into the kernel or run as a privileged user-space task.

### A. Environment Setup
- **Memory Allocation:** Map `m3_malloc` and `m3_free` to `kmalloc`/`kfree` (if in kernel) or the user heap.
- **Internal Integration:** Compile the `wasm3` source (currently in `src/kernel/wasm3-0.5.0`) into the kernel image.

### B. WASI Implementation
To make Wasm3 viable, we must implement the "Host Functions" that WebAssembly modules expect:
- **`fd_write`**: Route to `kern_terminal` or `kern_serial`.
- **`fd_read`**: Route to `kern_keyboard`.
- **`path_open` / `fd_read`**: Interface with the existing Ext2/FS implementation to allow WASM modules to read data files.
- **`proc_exit`**: Properly terminate the process and clean up the address space.

### C. Graphics Bindings
Since SandfleaOS has a framebuffer, custom host functions should be provided for:
- Drawing pixels/rectangles.
- Accessing the framebuffer directly (via shared linear memory or syscalls).

## 3. Execution Model

### A. The Wasm Loader
Implement a command (e.g., `run /bin/app.wasm`) that:
1. Reads the WASM binary from the Ext2 filesystem.
2. Creates a new process and address space.
3. Initializes the Wasm3 environment and runtime.
4. Loads the module into the runtime.
5. Spawns a thread to execute `m3_CallMain`.

### B. Kernel as Supervisor
The long-term goal is for the kernel's `main.c` loop to transition from handling commands directly to being a supervisor that manages the lifecycle of Wasm3-based processes.

## 4. Immediate Next Steps

1. **Update `kern_task_t`**: Add a `u64 cr3` field.
2. **Update `task_switch_asm`**: Add `mov cr3, reg` logic to handle address space switching.
3. **Wasm3 "Hello World"**: Create a kernel thread that manually initializes Wasm3 and executes a hardcoded WASM buffer to verify the interpreter works in the SandfleaOS environment.
4. **GDT/TSS Implementation**: Move towards Ring 3 support for better isolation.
