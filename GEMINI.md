# sandfleaOS

A hobby operating system for x64 with a focus on WebAssembly as the primary execution model.

## Core Philosophy
- **Wasm-First:** Designed to run WebAssembly modules as the primary application format using the Wasm3 interpreter.
- **Non-POSIX:** No interest in 1:1 POSIX compatibility or strict adherence to the UNIX philosophy.
- **Modern Hardware:** Targeted at x64 with UEFI booting via Limine.

## Architectural Overview
- **Kernel:** Higher-half kernel built for x86_64.
- **Bootloader:** Limine (UEFI-only ISO).
- **Memory Management:**
  - **PMM:** Physical Memory Manager using a bitmap.
  - **VMM:** Virtual Memory Manager handling paging and PML4 tables.
  - **Heap:** `kmalloc` / `kfree` for kernel allocations.
- **Scheduling:** Multi-threaded kernel with support for isolated processes.
- **Execution Engine:** Wasm3 interpreter integrated directly into the kernel/userspace bridge.
- **Filesystem:** Ext2 support over IDE (AHCI transition planned), with a VFS layer for handle-based I/O.
- **Graphics:** Framebuffer-based display with SSFN font rendering for the terminal.
- **IPC:** (Under research) Planned to support process communication.

## Development & Build
- **Toolchain:** GCC (cross-compiler preferred), NASM, `xorriso`, `mtools`, `debugfs`.
- **Build System:** `build.sh` script orchestrates compilation, font processing, and ISO creation.
- **Entry Point:** `kern_entry()` in `src/kernel/main.c`.

## Coding Conventions
- **Types:** Custom type aliases in `src/include/dialect.h` (e.g., `u0` for void, `u64` for uint64_t).
- **Naming:** 
  - Kernel modules prefixed with `kern_` (e.g., `kern_vmm.c`).
  - Functions generally use `snake_case`.
- **Logging:** 
  - `serial_outsf` for formatted serial output (COM1).
  - `screen_push_linef` for formatted terminal output.
- **Error Handling:** Asserts and serial logging for fatal kernel errors.

## Wasm Integration
- WASM modules are loaded from the Ext2 filesystem.
- Syscalls are implemented as "Host Functions" linked via `m3_LinkRawFunction`.
- Standard WASI functions (like `fd_write`) are mapped to internal kernel services (terminal/serial).

## Project Structure
- `src/kernel/`: Core kernel implementation.
- `src/arch/`: Architecture-specific assembly (ISRs, entry point).
- `src/include/`: Header files and internal definitions.
- `src/wasm/`: Wasm test source files (.wat).
- `src/blob/`: Binary assets (Wasm modules, fonts, test files).
- `media/writings/`: Implementation plans and architectural notes.
