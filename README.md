# sandfleaOS

sandfleaOS is a hobbyist x86_64 operating system designed with a modern, WebAssembly-first execution model. It targets UEFI hardware and focuses on a pragmatic, security-conscious architecture that leverages Wasm as the primary application format.

## Core Philosophy

sandfleaOS is built on a few specific design principles:

*   Apps are WASM first, write in WASM once, use (hopefully) anywhere. WASM code runs in wasm3, no kernel rings, so don't find vulns in my vendored wasm3 pretty please.
*   **Non-POSIX Compliance:** The system does not aim for 1:1 POSIX compatibility or strict adherence to UNIX traditions. Standards are adopted only when they provide a genuine technical advantage or convenience.
*   x64 Limine only atm. Might do ARM support if i feel like learning ARM assembly and rewriting the hard parts of the OS.

## Key Features

*   **Wasm Execution Engine:** Integrated Wasm3 interpreter allowing cross-platform bytecode execution.
*   **Handle-based VFS:** A Virtual Filesystem layer supporting handle-based I/O for persistent storage.
*   **Ext2 Support:** Robust implementation for opening, reading, and closing files on Ext2 partitions.
*   **IDE/Storage:** Support for hard drive I/O via IDE (with AHCI transition planned).
*   **Memory Management:** Full virtual memory support with PML4 paging and a bitmap-based Physical Memory Manager.
*   **Multitasking:** Kernel-level scheduling supporting isolated processes.
*   **Hardware Discovery:** PCI device discovery and enumeration.
*   **Graphics & UI:** Framebuffer-based display with high-quality text rendering via the SSFN font system.
*   **Input:** Interrupt-driven keyboard support.

## Architecture

### Kernel and Bootloader
*   **Higher-Half Kernel:** The kernel resides in the higher half of the virtual address space.
*   **Limine Bootloader:** Uses Limine for UEFI booting and passing system information (memory maps, framebuffer, etc.).

### Memory Management
*   **Physical Memory Manager (PMM):** Manages physical frames using a bitmap for efficient allocation and tracking.
*   **Virtual Memory Manager (VMM):** Handles paging structures and memory protection for kernel and user processes.
*   **Heap:** Provides `kmalloc` and `kfree` for dynamic kernel-side allocations.

### Execution Model
*   **Host Functions:** Syscalls are implemented as "Host Functions" linked via `m3_LinkRawFunction`, bridging Wasm modules to kernel services.
*   **WASI Support:** Standard WASI functions (like `fd_write`) are mapped to internal kernel services such as serial output and terminal display.

### Filesystem and Storage
*   **Ext2 Implementation:** Supports directory traversal and file access on Ext2-formatted disks.
*   **VFS Layer:** Abstracted filesystem interface for consistent I/O operations across different backends.

## Project Structure

*   `src/kernel/`: Core kernel implementation including scheduling, memory management, and syscalls.
*   `src/arch/`: Architecture-specific assembly for ISRs, IRQs, and the kernel entry point.
*   `src/include/`: System headers and internal definitions.
*   `src/wasm/`: WebAssembly source files (.wat) for testing and system utilities.
*   `src/blob/`: Binary assets including fonts, test Wasm modules, and configuration files.
*   `media/`: Project documentation, architectural writings, and media assets.

## Development

The project is built using a custom toolchain and build scripts designed for cross-compilation.

*   **Toolchain:** GCC (cross-compiler), NASM, `xorriso`, `mtools`, and `debugfs`.
*   **Build System:** `build.sh` because cmake is hell. Works with wsl if you install the programs
*   **Entry Point:** The system begins execution at `kern_entry()` in `src/kernel/main.c`.

## Roadmap

See [todo.md](todo.md)

Future development focuses on expanding system capabilities and modernizing hardware support:

*   **AHCI Implementation:** Replacing the legacy IDE driver with a modern AHCI driver.
*   **Multicore Support:** Implementing ACPI/MADT parsing and Symmetric Multiprocessing (SMP).
*   **IPC:** Researching and implementing a robust Inter-Process Communication mechanism.
*   **ELF Support:** Adding the ability to execute native ELF binaries alongside Wasm modules.
*   **UTF-8:** Transitioning all internal string handling to full UTF-8 support.

---

![](https://github.com/TobinCavanaugh/sandflea9/blob/828da5dd6d612636fd0f12043ef45fcdc0417e1b/media/gif/2026-05-20.gif)
