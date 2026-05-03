//
// Created by tobin on 2025-11-24.
//

#ifndef KERN_VMM_H
#define KERN_VMM_H

#include "dialect.h"
#include "../../limine/limine.h"


#define PAGE_PRESENT (1ull << 0)
#define PAGE_RW (1ull << 1)
#define PAGE_USER (1ull << 2)
#define PAGE_PWT (1ull << 3)
#define PAGE_PCD (1ull << 4)

#define PML4_INDEX(x) (((x) >> 39) & 0x1FF)
#define PDPT_INDEX(x) (((x) >> 30) & 0x1FF)
#define PD_INDEX(x) (((x) >> 21) & 0x1FF)
#define PT_INDEX(x) (((x) >> 12) & 0x1FF)

#define IOAPIC_PHYS_BASE 0xFEC00000
#define IOPACID 0x00
#define IOAPICVER 0x01
#define IOAPICARB 0x02
#define IOREDTBL 0x10

#define IA32_APIC_BASE_MSR 0x1B
#define APIC_BASE_MASK 0xFFFFF000

#define KHEAP_START_ADDR 0xFFFFFFFF88000000
#define PAGE_SIZE 4096
#define HEAP_MIN_SIZE 0x40000 // Initial 256KB

typedef struct heap_header {
    u64 size; // Size of the data block (excluding header)
    u64 is_free; // 1 if free, 0 if used
    struct heap_header *next; // Pointer to the next block in the list
    u64 padding; // Pad to 32 bytes for 16-byte alignment of data
} heap_header_t;

// API
u0 kmalloc_init();

u0 *kmalloc(u64 size);

u0* kmallocz(u64 size);

u0* kern_realloc(void* ptr, u64 size);

u0 kfree(void *ptr);

u0 init_vmm_globals(struct limine_hhdm_request hhdm_request);

u0 init_pmm(struct limine_memmap_request memmap_request);

u64 pmm_alloc_page();
u0 pmm_free(u64 phys_addr);
u64 pmm_get_free_count();

u64 vmm_get_hhdm();

u64 read_cr3();

u0 vmm_map_page(u64 phys_addr, u64 virt_addr, u64 flags);

u0 vmm_map_page_in_pml4(u64 pml4_phys, u64 phys_addr, u64 virt_addr, u64 flags);

u64 vmm_get_phys_in_pml4(u64 pml4_phys, u64 virt_addr);

u0 vmm_unmap_page_in_pml4(u64 pml4_phys, u64 virt_addr);

#endif //KERN_VMM_H
