//
// Created by tobin on 2025-11-24.
//

#include "../include/kern_vmm.h"

#include "../include/kern_serial.h"


u64 hhdm_offset = 0;

u0 init_vmm_globals(struct limine_hhdm_request hhdm_request) {
    if (hhdm_request.response != NULL) {
        hhdm_offset = hhdm_request.response->offset;
    } else {
        serial_outc('!');
        serial_outc('V');
    }
}

u64 free_mem_ptr = 0; //TODO improve, this is crap

u0 init_pmm(struct limine_memmap_request memmap_request) {
    struct limine_memmap_response *map = memmap_request.response;
    for (u64 i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            if (entry->length > 1024 * 1024 * 4) {
                free_mem_ptr = entry->base;
                break;
            }
        }
    }
}

u64 pmm_alloc_page() {
    u64 ret = free_mem_ptr;
    free_mem_ptr += 4096;

    u64 *virt_ptr = (u64 *) (ret + hhdm_offset);
    for (int i = 0; i < 512; i++) virt_ptr[i] = 0;
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
