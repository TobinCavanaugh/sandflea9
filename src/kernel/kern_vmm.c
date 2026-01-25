//
// Created by tobin on 2025-11-24.
//

#include "../include/kern_vmm.h"
#include "../include/kern_mem.h"
#include "../include/kern_serial.h"

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

u0 pmm_free(u64 phys_addr);

u0 init_pmm(struct limine_memmap_request memmap_request) {
    struct limine_memmap_response *map = memmap_request.response;
    for (u64 i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) { // This will skip any blocks that limine is keeping for itself
            u64 addr = (entry->base + 0xFFF) & ~0xFFF; // Align to 4096
            u64 top = (entry->base + entry->length);

            // Add these to our free list
            while (addr + 4096 <= top) {
                pmm_free(addr);
                addr += 4096;
            }
        }
    }
}

u0 pmm_free(u64 phys_addr) {
    if (phys_addr == 0) return;
    u64 *virt_ptr = (u64 *) (phys_addr + hhdm_offset);
    *virt_ptr = free_list_head;
    free_list_head = phys_addr;
}

u64 pmm_alloc_page() {
    if (free_list_head == 0) {
        serial_outs("OUT OF MEMORY. FAILING HARD\n");
        for (;;);
    }
    u64 ret = free_list_head;
    u64 *virt_ptr = (u64 *) (ret + hhdm_offset);
    free_list_head = *virt_ptr; // new free page is the head

    for (int i = 0; i < 512; i++) virt_ptr[i] = 0;
    // mem_set(virt_ptr, 0, 512);
    return ret;
}

u64 read_cr3() {
    u64 cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

u0 invlpg(u64 vaddr) {
    asm volatile("invlpg (%0)" : : "r"(vaddr) : "memory");
}

u0 vmm_map_page(u64 phys_addr, u64 virt_addr, u64 flags) {
    u64 pml4_phys = read_cr3();

    u64 *pml4 = (u64 *) (pml4_phys + hhdm_offset);

    u64 pml4_idx = PML4_INDEX(virt_addr);
    if (!(pml4[pml4_idx] & PAGE_PRESENT)) {
        u64 new_table = pmm_alloc_page();
        pml4[pml4_idx] = new_table | PAGE_PRESENT | PAGE_RW;
    }

    u64 *pdpt = (u64 *) ((pml4[pml4_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pdpt_idx = PDPT_INDEX(virt_addr);
    if (!(pdpt[pdpt_idx] & PAGE_PRESENT)) {
        u64 new_table = pmm_alloc_page();
        pdpt[pdpt_idx] = new_table | PAGE_PRESENT | PAGE_RW;
    }

    u64 *pd = (u64 *) ((pdpt[pdpt_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pd_idx = PD_INDEX(virt_addr);
    if (!(pd[pd_idx] & PAGE_PRESENT)) {
        u64 new_table = pmm_alloc_page();
        pd[pd_idx] = new_table | PAGE_PRESENT | PAGE_RW;
    }

    u64 *pt = (u64 *) ((pd[pd_idx] & 0xFFFFFFFFFF000) + hhdm_offset);
    u64 pt_idx = PT_INDEX(virt_addr);
    pt[pt_idx] = phys_addr | flags | PAGE_PRESENT;
    invlpg(virt_addr);
}


heap_header_t *heap_ptr = (heap_header_t *) (KHEAP_START_ADDR);

u64 heap_end_addr = KHEAP_START_ADDR;

u64 align_size(u64 size) {
    if (size % 16) return size;
    return size + (16 - (size % 16));
}

u0 heap_expand(u64 needed) {
    u64 target_end = heap_end_addr + needed;

    if (target_end % PAGE_SIZE != 0) {
        target_end = (target_end & ~(PAGE_SIZE - 1)) + PAGE_SIZE;
    }
    while (heap_end_addr < target_end) {
        u64 phys_page = pmm_alloc_page();
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

u0* kmallocz(u64 size){
    void * ptr = kmalloc(size);
    mem_set(ptr, 0, size);
    return ptr;
}

u0 *kmalloc(u64 size) {
    if (size == 0) return null;

    u64 aligned_size = align_size(size);
    heap_header_t *curr = heap_ptr;
    heap_header_t *last = null;

    while (curr != null) {
        if (curr->is_free && curr->size >= aligned_size) {
            if (curr->size > aligned_size + sizeof(heap_header_t) + 16) {
                heap_header_t *new_block = (heap_header_t *) ((u64) curr + sizeof(heap_header_t) + aligned_size);

                new_block->is_free = 1;
                new_block->size = curr->size - aligned_size - sizeof(heap_header_t);
                new_block->next = curr->next;

                curr->size = aligned_size;
                curr->next = new_block;
            }

            curr->is_free = 0;

            return (void *) ((u64) curr + sizeof(heap_header_t));
        }

        last = curr;
        curr = curr->next;
    }

    u64 old_end = heap_end_addr;
    heap_expand(aligned_size + sizeof(heap_header_t));

    heap_header_t *new_expansion = (heap_header_t *) old_end;
    u64 expansion_size = heap_end_addr - old_end;

    if (last == null) {
        heap_ptr = (heap_header_t *) old_end;
        last = heap_ptr;
    } else {
        last->next = (heap_header_t *) old_end;
    }

    heap_header_t *new_block = (heap_header_t *) old_end;
    new_block->size = expansion_size - sizeof(heap_header_t);
    new_block->is_free = 1;
    new_block->next = null;

    // Recursively return the block we've made
    return kmalloc(size);
}

u0 kfree(void *ptr) {
    if (ptr == null) return;

    heap_header_t *header = (heap_header_t *) ((u64) ptr - sizeof(heap_header_t));
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
}
