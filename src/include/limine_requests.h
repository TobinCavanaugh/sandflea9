//
// Created by tobin on 2025-11-30.
//

#ifndef LIMINE_REQUESTS_H
#define LIMINE_REQUESTS_H


#define LIMINE_REQUEST __attribute__((used, section(".limine_requests")))

LIMINE_REQUEST static volatile LIMINE_BASE_REVISION(3);
LIMINE_REQUEST static volatile struct limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST,
    .revision = 0
};

LIMINE_REQUEST static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0
};

LIMINE_REQUEST static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0
};

LIMINE_REQUEST static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0
};

#endif //LIMINE_REQUESTS_H
