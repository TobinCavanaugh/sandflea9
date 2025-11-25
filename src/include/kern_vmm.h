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

u0 init_vmm_globals(struct limine_hhdm_request hhdm_request);

u0 init_pmm(struct limine_memmap_request memmap_request);

u0 vmm_map_page(u64 phys_addr, u64 virt_addr, u64 flags);

#endif //KERN_VMM_H
