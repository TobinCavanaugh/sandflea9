# Program Memory Management in sandfleaOS

This document outlines the strategy for evolving the current thread-based "shimmy" functions into isolated processes with their own virtual memory and automated cleanup.

## 1. Evolving the Task Structure (PCB)

Currently, `kern_task_t` only tracks the stack pointer and basic state. To support programs, we need a **Process Control Block (PCB)**.

### Proposed Structure
```c
typedef struct kern_proc {
    u64 rsp;             // Saved stack pointer
    u64 cr3;             // Physical address of the PML4 (Page Table)
    i32 pid;
    i32 state;
    
    // Memory Tracking
    struct allocation *allocs; 
    
    struct kern_proc *next;
} kern_proc_t;

typedef struct allocation {
    u64 virt_addr;
    u64 phys_addr;
    u32 page_count;
    struct allocation *next;
} allocation_t;
```

## 2. Virtual Memory Isolation

To give every program its own address space, we must create a unique page table (PML4) for each process.

### The "Clone" Strategy
1.  **Kernel Mapping:** Every process PML4 must include the kernel's memory (higher half). When creating a new process, we copy the kernel's top-level PML4 entries into the new PML4.
2.  **User Mapping:** User-level code and data are mapped into the lower half of the virtual address space.
3.  **Switching:** During `sched_run_next`, the kernel must update the `CR3` register to the next process's `cr3` value.

## 3. Implementing `proc_malloc`

`proc_malloc` will be the primary way a process requests memory. Unlike `kmalloc` (which uses a global heap), `proc_malloc` will:

1.  **PMM Allocation:** Request `N` physical pages from `pmm_alloc_page()`.
2.  **VMM Mapping:** Map these physical pages to a free virtual address in the process's page table.
3.  **Registration:** Create an `allocation_t` record and attach it to the process's PCB.

### Example Workflow
```c
u0 *proc_malloc(kern_proc_t *proc, u64 size) {
    u64 pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 phys = pmm_alloc_pages(pages);
    u64 virt = find_free_vaddr(proc, pages);
    
    vmm_map_pages(proc->cr3, phys, virt, pages, PAGE_USER | PAGE_RW);
    
    register_allocation(proc, virt, phys, pages);
    return (u0*)virt;
}
```

## 4. Automatic Cleanup (The "Reaper")

When a process exits or is killed:
1.  **Iterate Allocs:** Walk the `proc->allocs` list.
2.  **Free Physical:** Call `pmm_free_page()` for every physical address in the list.
3.  **Unmap Virtual:** Remove the entries from the process's PML4.
4.  **Free Tables:** Carefully free the page table structures (PML3, PML2, PML1) that were unique to this process.
5.  **Free PCB:** Finally, free the `kern_proc_t` itself.

## 5. Roadmap for Implementation

1.  **VMM Extension:** Update `kern_vmm.c` to support mapping into a specific PML4 (currently it likely assumes the active one).
2.  **PCB Update:** Modify `kern_sched.h` and `kern_sched.c` to use the new process structure.
3.  **Context Switch:** Update `task_switch_asm` to handle `CR3` switching.
4.  **Allocation Tracker:** Implement a simple linked list manager for process allocations.
5.  **The `proc_exit` function:** Logic to trigger the cleanup sequence.
