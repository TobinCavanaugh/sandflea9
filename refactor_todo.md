# sandfleaOS Refactoring TODO

This document outlines the structural changes required to improve the modularity and maintainability of the kernel, in alignment with its Wasm-first and non-POSIX philosophy.

## 1. Hardware Abstraction Layer (HAL) Cleanup
Goal: Isolate architecture-specific initialization from high-level interrupt dispatching.
- [ ] Split `kern_interrupts.c`:
    - [x] Create `src/kernel/arch/x64/idt.c` for GDT/IDT table setup and descriptor logic.
    - [ ] Create `src/kernel/arch/x64/apic.c` for LAPIC and IOAPIC management.
    - [ ] Create `src/kernel/drivers/timer.c` for PIT and APIC timer calibration.
- [ ] Clean up `kern_interrupts.c` to act solely as a high-level dispatcher for `isr_handler`.

## 2. Memory Management Layering
Goal: Separate physical, virtual, and heap memory logic.
- [ ] Deconstruct `kern_vmm.c`:
    - [ ] Create `src/kernel/mem/pmm.c` for physical page bitmap/free-list logic.
    - [ ] Create `src/kernel/mem/vmm.c` for paging and PML4 manipulation.
    - [ ] Create `src/kernel/mem/heap.c` for `kmalloc`, `kfree`, and block-splitting logic.

## 3. Process vs. Scheduling Separation
Goal: Isolate process resource management from the task execution loop.
- [ ] Refactor `kern_sched.c`:
    - [ ] Create `src/kernel/proc/process.c` for `process_create`, `process_exit`, and FD table management.
    - [ ] Keep context switching and task list management in `src/kernel/proc/sched.c`.

## 4. Elevate Wasm Integration
Goal: Move Wasm host functions from `kern_tests.c` to a first-class subsystem.
- [ ] Create `src/kernel/wasm/host_funcs/`:
    - [ ] `fd.c`: I/O related host functions (WASI-like).
    - [ ] `proc.c`: Process and lifecycle host functions.
- [ ] Move Wasm3 initialization and linking logic to `src/kernel/wasm/wasm_runtime.c`.

## 5. Filesystem Driver vs. VFS
Goal: Decouple raw Ext2 logic from high-level path traversal.
- [ ] Refactor `kern_ext2.c`:
    - [ ] `ext2_driver.c`: Block, inode, and bitmap manipulation.
    - [ ] `ext2_vfs.c`: Explorer, path traversal, and child finding logic.

## 6. Target Directory Structure
Migrate from flat `src/kernel/` to a hierarchical layout:
```text
src/kernel/
├── arch/x64/      # GDT, IDT, APIC, ISR stubs
├── mem/           # PMM, VMM, Heap
├── proc/          # Process, Sched
├── fs/            # VFS, ext2/, ide/
├── wasm/          # Wasm3 integration, Host Functions
├── drivers/       # Serial, Keyboard, PCI, Timer
└── terminal/      # SSFN engine, screen line buffer
```
