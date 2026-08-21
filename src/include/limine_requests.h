//
// Created by tobin on 2025-11-30.
//

#ifndef LIMINE_REQUESTS_H
#define LIMINE_REQUESTS_H


#define LIMINE_REQUEST __attribute__((used, section(".limine_requests")))
#define LIMINE_REQUEST_START __attribute__((used, section(".limine_requests_start")))
#define LIMINE_REQUEST_END __attribute__((used, section(".limine_requests_end")))

LIMINE_REQUEST_START static volatile LIMINE_REQUESTS_START_MARKER;

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

LIMINE_REQUEST_END static volatile LIMINE_REQUESTS_END_MARKER;

#endif //LIMINE_REQUESTS_H
