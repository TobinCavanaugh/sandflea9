#ifndef KERN_TESTS_H
#define KERN_TESTS_H

#include "dialect.h"
#include "kern_pci.h"
#include "kern_screen.h"

typedef struct {
    display_t *display_main;
    display_t **display_array;

    u64 usable_mem_size;
    u64 total_mem_size;

    pci_device_t *pci_list_head;
} system_t;

extern system_t system;
extern char typingbuf[255];

u0 handle_command();

#endif //KERN_TESTS_H
