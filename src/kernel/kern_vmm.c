//
// Created by tobin on 2025-11-24.
//

#include "../include/kern_vmm.h"
#include "../include/kern_mem.h"
#include "../include/kern_serial.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_sched.h"

u64 hhdm_offset = 0;
u64 free_list_head = 0;

u0 init_vmm_globals(struct limine_hhdm_request hhdm_request) {
    if (hhdm_request.response != NULL) {
        hhdm_offset = hhdm_request.response->offset;
    } else {
        serial_outs("Limine gave us no HHDM. Failing Hard.\n");
        for (;;);
    }
}

u64 vmm_get_hhdm() {
    return hhdm_offset;
}

u0 pmm_free(u64 phys_addr) {
    if (phys_addr == 0) return;
    u64 irq = save_irq_and_disable();
    u64 *virt_ptr = (u64 *) (phys_addr + hhdm_offset);
    *virt_ptr = free_list_head;
    free_list_head = phys_addr;
    restore_irq(irq);
}

u0 init_pmm(struct limine_memmap_request memmap_request) {
    struct limine_memmap_response *map = memmap_request.response;
    for (u64 i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            u64 addr = (entry->base + 0xFFF) & ~0xFFF;
            u64 top = (entry->base + entry->length);

            while (addr + 4096 <= top) {
                pmm_free(addr);
                addr += 4096;
            }
        }
    }
}

u64 pmm_alloc_page() {
    u64 irq = save_irq_and_disable();
    if (free_list_head == 0) {
        serial_outs("OUT OF MEMORY. FAILING HARD\n");
        for (;;);
    }
    u64 ret = free_list_head;
    u64 *virt_ptr = (u64 *) (ret + hhdm_offset);
    free_list_head = *virt_ptr;
    restore_irq(irq);

    for (int i = 0; i < 512; i++) virt_ptr[i] = 0;
    return ret;
}

u64 pmm_get_free_count() {
    u64 irq = save_irq_and_disable();
    u64 count = 0;
    u64 curr = free_list_head;
    while (curr != 0) {
        count++;
        curr = *(u64 *) (curr + hhdm_offset);
    }
    restore_irq(irq);
    return count;
}

u64 read_cr3() {
    u64 cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

u0 invlpg(u64 vaddr) {
    asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

u0 vmm_map_page_in_pml4(u64 pml4_phys, u64 phys_addr, u64 virt_addr, u64 flags) {
    u64 irq = save_irq_and_disable();
    u64 *pml4 = (u64 *) (pml4_phys + hhdm_offset);

    u64 pml4_idx = PML4_INDEX(virt_addr);
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        u64 new_table = pmm_alloc_page();
        pml4[pml4_idx] = new_table | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
    }

    u64 *pdpt = (u64 *) ((pml4[pml4_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pdpt_idx = PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        u64 new_table = pmm_alloc_page();
        pdpt[pdpt_idx] = new_table | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
    }

    u64 *pd = (u64 *) ((pdpt[pdpt_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pd_idx = PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        u64 new_table = pmm_alloc_page();
        pd[pd_idx] = new_table | PAGE_PRESENT | PAGE_RW | (flags & PAGE_USER);
    }

    u64 *pt = (u64 *) ((pd[pd_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pt_idx = PT_INDEX(virt_addr);
    pt[pt_idx] = phys_addr | flags | PAGE_PRESENT;

    if (pml4_phys == read_cr3()) {
        invlpg(virt_addr);
    }
    restore_irq(irq);
}

u0 vmm_map_page(u64 phys_addr, u64 virt_addr, u64 flags) {
    if (virt_addr >= 0xFFFF800000000000) {
        kern_process_t *kp = sched_get_kernel_process();
        u64 master_cr3 = kp ? kp->cr3 : read_cr3();
        vmm_map_page_in_pml4(master_cr3, phys_addr, virt_addr, flags);

        if (master_cr3 != read_cr3()) {
            u64 pml4_idx = PML4_INDEX(virt_addr);
            u64 *current_pml4 = (u64 *) (read_cr3() + hhdm_offset);
            u64 *master_pml4 = (u64 *) (master_cr3 + hhdm_offset);
            current_pml4[pml4_idx] = master_pml4[pml4_idx];

            // Full TLB flush by reloading CR3
            u64 cr3;
            asm volatile("mov %%cr3, %0" : "=r"(cr3));
            asm volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
        }
    } else {
        vmm_map_page_in_pml4(read_cr3(), phys_addr, virt_addr, flags);
    }
}

u64 vmm_get_phys_in_pml4(u64 pml4_phys, u64 virt_addr) {
    u64 irq = save_irq_and_disable();
    u64 *pml4 = (u64 *) (pml4_phys + hhdm_offset);
    u64 pml4_idx = PML4_INDEX(virt_addr);
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        restore_irq(irq);
        return 0;
    }

    u64 *pdpt = (u64 *) ((pml4[pml4_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pdpt_idx = PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        restore_irq(irq);
        return 0;
    }

    u64 *pd = (u64 *) ((pdpt[pdpt_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pd_idx = PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        restore_irq(irq);
        return 0;
    }

    u64 *pt = (u64 *) ((pd[pd_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pt_idx = PT_INDEX(virt_addr);
    if (!(pt[pt_idx] & PAGE_PRESENT)) {
        restore_irq(irq);
        return 0;
    }

    u64 ret = (pt[pt_idx] & 0xFFFFFFFFFF000);
    restore_irq(irq);
    return ret;
}

u0 vmm_unmap_page_in_pml4(u64 pml4_phys, u64 virt_addr) {
    u64 irq = save_irq_and_disable();
    u64 *pml4 = (u64 *) (pml4_phys + hhdm_offset);
    u64 pml4_idx = PML4_INDEX(virt_addr);
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        restore_irq(irq);
        return;
    }

    u64 *pdpt = (u64 *) ((pml4[pml4_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pdpt_idx = PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        restore_irq(irq);
        return;
    }

    u64 *pd = (u64 *) ((pdpt[pdpt_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pd_idx = PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        restore_irq(irq);
        return;
    }

    u64 *pt = (u64 *) ((pd[pd_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pt_idx = PT_INDEX(virt_addr);
    pt[pt_idx] = 0;

    if (pml4_phys == read_cr3()) {
        invlpg(virt_addr);
    }
    restore_irq(irq);
}

u0 *pmallocz(u64 size) {
    u0 *mem = pmalloc(size);
    mem_set(mem, 0, size);
    return mem;
}

// Allocate memory in the current process, effectively malloc
// Will allocate memory in the kernel process if no process is open
u0 *pmalloc(u64 size) {
    if (size == 0) return null;
    kern_process_t *proc = sched_get_current_process();
    if (!proc) return kmalloc(size);

    u64 irq = save_irq_and_disable();
    u64 page_count = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    u64 virt_start = proc->heap_vptr;

    kern_mem_region_t *region = kmalloc(sizeof(kern_mem_region_t));
    region->virt = virt_start;
    region->page_count = page_count;
    region->phys = 0;

    for (u64 i = 0; i < page_count; i++) {
        u64 phys = pmm_alloc_page();
        if (i == 0) region->phys = phys;
        vmm_map_page_in_pml4(proc->cr3, phys, virt_start + (i * PAGE_SIZE), PAGE_PRESENT | PAGE_RW | PAGE_USER);
    }

    region->next = proc->mem_regions;
    proc->mem_regions = region;
    proc->heap_vptr += page_count * PAGE_SIZE;
    restore_irq(irq);
    return (u0 *) virt_start;
}

u0 pfree(void *ptr) {
    if (!ptr) return;
    kern_process_t *proc = sched_get_current_process();
    if (!proc) return;

    u64 irq = save_irq_and_disable();
    kern_mem_region_t *curr = proc->mem_regions;
    kern_mem_region_t *prev = null;

    while (curr) {
        if (curr->virt == (u64) ptr) {
            for (u64 i = 0; i < curr->page_count; i++) {
                u64 vaddr = curr->virt + (i * PAGE_SIZE);
                u64 phys = vmm_get_phys_in_pml4(proc->cr3, vaddr);
                if (phys) {
                    pmm_free(phys);
                    vmm_unmap_page_in_pml4(proc->cr3, vaddr);
                }
            }
            if (prev) prev->next = curr->next;
            else proc->mem_regions = curr->next;

            kfree(curr);
            restore_irq(irq);
            return;
        }
        prev = curr;
        curr = curr->next;
    }
    restore_irq(irq);
}

heap_header_t *heap_ptr = (heap_header_t *) (KHEAP_START_ADDR);
u64 heap_end_addr = KHEAP_START_ADDR;

u64 align_size(u64 size) {
    if (size % 16 == 0) return size;
    return size + (16 - (size % 16));
}

u0 heap_expand(u64 needed) {
    u64 target_end = heap_end_addr + needed;
    if (target_end % PAGE_SIZE != 0) {
        target_end = (target_end & ~(PAGE_SIZE - 1)) + PAGE_SIZE;
    }

    serial_outsf("VMM: Expanding heap from %llX to %llX (needed %llX)\n", heap_end_addr, target_end, needed);

    while (heap_end_addr < target_end) {
        u64 phys_page = pmm_alloc_page();
        if (phys_page == 0) {
            serial_outsl("VMM: FAILED TO ALLOCATE PHYSICAL PAGE FOR HEAP EXPANSION!");
            return;
        }
        vmm_map_page(phys_page, heap_end_addr, PAGE_PRESENT | PAGE_RW);
        heap_end_addr += PAGE_SIZE;
    }
}

u0 kmalloc_init() {
    heap_expand(HEAP_MIN_SIZE);
    heap_ptr->size = HEAP_MIN_SIZE - sizeof(heap_header_t);
    heap_ptr->is_free = 1;
    heap_ptr->next = null;
    serial_outs("Kernel Heap Initialized\n");
}

u0 *kmallocz(u64 size) {
    void *ptr = kmalloc(size);
    if (ptr) mem_set(ptr, 0, size);
    return ptr;
}

u0 *kmalloc(u64 size) {
    if (size == 0) return null;

    u64 aligned_size = align_size(size);
    u64 irq = save_irq_and_disable();

    while (1) {
        heap_header_t *curr = heap_ptr;
        heap_header_t *last = null;

        while (curr != null) {
            if (curr->is_free && curr->size >= aligned_size) {
                // Ensure we have enough room for a header (32 bytes) and some data (16 bytes)
                u64 total_needed = aligned_size + sizeof(heap_header_t) + 16;
                if (curr->size >= total_needed) {
                    heap_header_t *new_block = (heap_header_t *) ((u64) curr + sizeof(heap_header_t) + aligned_size);
                    new_block->is_free = 1;
                    new_block->size = curr->size - aligned_size - sizeof(heap_header_t);
                    new_block->next = curr->next;
                    curr->size = aligned_size;
                    curr->next = new_block;
                }
                curr->is_free = 0;
                void *ret = (void *) ((u64) curr + sizeof(heap_header_t));
                restore_irq(irq);
                return ret;
            }
            last = curr;
            curr = curr->next;
        }

        // Expansion needed
        u64 old_end = heap_end_addr;
        heap_expand(aligned_size + sizeof(heap_header_t));
        u64 expansion_size = heap_end_addr - old_end;

        if (expansion_size == 0) {
            restore_irq(irq);
            return null;
        }

        heap_header_t *new_block = (heap_header_t *) old_end;
        new_block->size = expansion_size - sizeof(heap_header_t);
        new_block->is_free = 1;
        new_block->next = null;

        if (last == null) {
            heap_ptr = new_block;
        } else {
            last->next = new_block;
        }
    }
}

u0 *kern_realloc(void *ptr, u64 size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return null;
    }

    u64 irq = save_irq_and_disable();
    heap_header_t *header = (heap_header_t *) ((u64) ptr - sizeof(heap_header_t));
    if (header->size >= size) {
        restore_irq(irq);
        return ptr;
    }
    restore_irq(irq);

    void *new_ptr = kmalloc(size);
    if (!new_ptr) return null;

    mem_copy(new_ptr, ptr, header->size);
    kfree(ptr);
    return new_ptr;
}

u0 kfree(void *ptr) {
    if (ptr == null) return;

    u64 irq = save_irq_and_disable();
    heap_header_t *header = (heap_header_t *) ((u64) ptr - sizeof(heap_header_t));
    if (header->is_free) {
        restore_irq(irq);
        return;
    }
    header->is_free = 1;

    heap_header_t *curr = heap_ptr;
    while (curr != null && curr->next != null) {
        if (curr->is_free && curr->next->is_free) {
            if ((u64) curr + sizeof(heap_header_t) + curr->size == (u64) curr->next) {
                curr->size += sizeof(heap_header_t) + curr->next->size;
                curr->next = curr->next->next;
            } else {
                curr = curr->next;
            }
        } else {
            curr = curr->next;
        }
    }
    restore_irq(irq);
}