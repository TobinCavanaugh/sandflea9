#include "../include/dialect.h"
#include "../include/stbsupport.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_serial.h"
#include "../include/kern_vmm.h"
#include "../include/kern_keyboard.h"
#include "../include/kern_mem.h"
#include "../util/util_str.h"
#include "../../limine/limine.h"
#include "../include/limine_requests.h"
#include "../include/kern_pci.h"
#include "../include/kern_interrupts.h"
#include "../include/kern_screen.h"
#include "../include/kern_ext2.h"
#include "../include/kern_sched.h"
#include "../include/ssfn.h"
#include "../include/util_cmd.h"
#include "../include/kern_terminal.h"
#include "../include/kern_tests.h"
#include "../include/kern_ide.h"

display_t *display_main = 0;

extern char _binary_src_blob_regularfont_sfn_start;
extern u0 enable_sse(u0);

volatile u64 sw = 0;

u0 timer_handler(const registers_t *reg) {
    sw += 10;
    apic_eoi(0xFFFFFFFF10000000);
    sched_run_next();
}

u0 delay(u64 ms) {
    volatile u64 start = sw;
    while (sw - start < ms) {
        asm volatile("hlt");
    }
}

system_t system = {0};
char typingbuf[255] = {0};

i64 heartbeat1 = 0;
i64 heartbeat2 = 0;
i64 heartbeat3 = 0;

u0 shimmy3() {
    while (1) {
        heartbeat3 = heartbeat3 == 0 ? 1 : 0;
        delay(100);
    }
}

u0 shimmy2() {
    while (1) {
        heartbeat2 = heartbeat2 == 0 ? 1 : 0;
        delay(500);
    }
}

u0 shimmy() {
    while (1) {
        heartbeat1 = heartbeat1 == 0 ? 1 : 0;
        delay(1000);
    }
}


void kern_entry(void) {
    init_serial();
    serial_outsl("--- sandfleaOS Kernel Entry ---");
    serial_outsl("Serial initialized: COM1 ready");

    enable_sse(); // cpu extension
    serial_outsl("CPU: SSE extensions enabled");

    init_vmm_globals(hhdm_request); // virtual memory management
    serial_outsl("VMM: Virtual Memory Management initialized");

    init_pmm(memmap_request); // physical memory management
    serial_outsl("PMM: Physical Memory Management initialized");

    interrupts_init(); // interrupts
    serial_outsl("IDT: Interrupts and GDT stubs initialized");

    kmalloc_init(); // malloc
    serial_outsl("Heap: kmalloc initialized and ready for allocations");

    sched_init();
    serial_outsl("Scheduler: Multi-threading support initialized");


    system.pci_list_head = pci_init_system();
    serial_outsl("PCI: System bus scanned");

    // In kern_entry...
    pci_device_t *pci_uart = system.pci_list_head;
    while (pci_uart) {
        // Look specifically for the WCH CH382 (1C00:3253)
        if (pci_uart->class_code == 0x7 && pci_uart->subclass == 0x0) {
            serial_outsf("PCI: Found Serial Controller (Class 0x7, Sub 0x0) at %X:%X\n", pci_uart->vendor_id, pci_uart->device_id);
            break;
        }
        pci_uart = pci_uart->next;
    }

    if (pci_uart) {
        serial_outsl("UART: Initializing PCI UART Card...");
        pci_enable_device_io(pci_uart); // Enable Bus Master / IO

        // Try initializing BOTH BARs
        for (int i = 0; i < 2; i++) {
            u32 bar = pci_uart->bars[i];
            if ((bar & 1) == 1) {
                // If IO space
                u32 io_base = bar & 0xFFFFFFFC;
                if (io_base < 0xFFFF) {
                    pci_init_uart_port(io_base);
                    serial_outsf("UART: Initialized Port at IO Base 0x%X (BAR %d)\n", io_base, i);

                    // Send specific identifier for each port
                    if (i == 0) pci_serial_putsl(pci_uart, "PCI_UART: Output stream started from BAR 0");
                    if (i == 1) pci_serial_putsl(pci_uart, "PCI_UART: Output stream started from BAR 1");
                }
            }
        }
    }

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        serial_outsl("FATAL: No framebuffer provided by Limine!");
        for (;;) {
            __asm__("hlt");
        }
    }


    display_t displays[32];
    u8 fb_count = screen_init(framebuffer_request.response, displays, 32);
    display_main = &displays[0];
    serial_outsf("Video: %d framebuffer(s) found. Primary: %dx%d %dbpp\n", 
                 fb_count, display_main->surface.width, display_main->surface.height, display_main->surface.bpp);

    ssfn_src = (ssfn_font_t *) &_binary_src_blob_regularfont_sfn_start;
    serial_outsl("Font: SSFN regular font loaded");

    u32 width = displays[0].surface.width, height = displays[0].surface.height;
    u32 row_len = width / font_width;

    // 3. Configure SSFN destination using the Limine framebuffer data
    ssfn_dst.ptr = (u8 *) displays->surface.address;
    ssfn_dst.w = width;
    ssfn_dst.h = height;
    ssfn_dst.p = displays->surface.pitch;
    ssfn_dst.x = 0; // Start cursor at 0,0
    ssfn_dst.y = 0;
    ssfn_dst.fg = 0xFFFFFFFF; // White text

    u64 total_ram = 0;
    u64 usable_ram = 0;
    struct limine_memmap_response *mm = memmap_request.response;
    for (u64 i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *me = mm->entries[i];
        switch (me->type) {
            case LIMINE_MEMMAP_USABLE: {
                total_ram += me->length;
                usable_ram += me->length;
                break;
            }
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            case LIMINE_MEMMAP_KERNEL_AND_MODULES:
            case LIMINE_MEMMAP_ACPI_NVS: {
                total_ram += me->length;
                break;
            }
            default: {
                break;
            }
        }
    }
    system.usable_mem_size = usable_ram;
    system.total_mem_size = total_ram;

    serial_outsf("Memory: Total %lld MiB, Usable %lld MiB\n", total_ram / 1024 / 1024, usable_ram / 1024 / 1024);

    // Screen buffer
    screen_lines_init(row_len);
    serial_outsl("Terminal: Screen line buffer initialized");

    ide_init();
    serial_outsl("FS: IDE Initialized");

    ext2_init();
    serial_outsl("FS: Ext2 driver initialized");

    char buf[255];

    sched_create_thread(shimmy);
    sched_create_thread(shimmy2);
    sched_create_thread(shimmy3);
    serial_outsl("Threads: Heartbeat threads spawned");

    interrupt_register(32, timer_handler);
    interrupt_register(33, (void (*)(const registers_t *)) keyboard_handle_keypress);

    serial_outsl("--- Initialization Complete. Entering Main Loop ---");

    for (;;) {
        // Keyboard input
        u8 k = 0;
        while ((k = keyboard_eat_key())) {
            i32 len = str_len(typingbuf);

            if (k == '\n') {
                screen_push_linef("#>%s", typingbuf);
                handle_command();
                typingbuf[0] = 0;
            } else if (k == '\b') {
                if (len > 0) typingbuf[len - 1] = '\0';
            } else if (k == KEY_DOWN) {
                ++screen_text_scroll;
                serial_outsf("DOWN");
            } else if (k == KEY_UP) {
                --screen_text_scroll;
                screen_text_scroll = max(screen_text_scroll, 0);
                serial_outsf("UP");
            } else {
                if (len < 254) {
                    typingbuf[len] = k;
                    typingbuf[len + 1] = 0;
                }
                sw = 0;
            }
        }

        screen_clear(COLOR_BLACK);

        u32 start_y = font_height;
        screen_text_row_t *current = screen_text_root;
        i32 i = 0;
        while (current != null) {
            if (i >= screen_text_scroll) {
                i32 relative_row = i - screen_text_scroll;
                ssfn_dst.x = 0;
                ssfn_dst.y = start_y + (relative_row * font_height);

                if (ssfn_dst.y < display_main->surface.height - font_height) {
                    ssfn_puts(current->str);
                }
            }
            current = current->next;
            ++i;
        }

        Draw_Header:
        {
            ssfn_dst.x = 0;
            ssfn_dst.y = 0;

            v2i_t p = V2I(0, 0);
            p.x = 1 + screen_puts_r(" sandfleaOS ", p, COLOR_WHITE, COLOR_BLACK).x;

            stbsp_snprintf(buf, 255, " %.1fMiB ", (f32) usable_ram / 1024 / 1024.);
            p.x = 1 + screen_puts_r(buf, p, COLOR_BLUE, COLOR_BLACK).x;

            stbsp_snprintf(buf, 255, " %.1fMiB ", (f32) total_ram / 1024 / 1024.);
            p.x = 1 + screen_puts_r(buf, p, COLOR_GREEN, COLOR_BLACK).x;

            stbsp_snprintf(buf, 255, " Display %-2d ", display_main->index);
            p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;

            {
                stbsp_snprintf(buf, 255, " %s ", (heartbeat1 % 2) ? "*" : " ");
                p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;

                stbsp_snprintf(buf, 255, " %s ", (heartbeat2 % 2) ? "*" : " ");
                p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;

                stbsp_snprintf(buf, 255, " %s ", (heartbeat3 % 2) ? "*" : " ");
                p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;
            }

            for (int i = 0; i <= font_height; i += 4) {
                screen_draw_line(V2I(p.x, i), V2I(display_main->surface.width, i), COLOR_DIM_GRAY);
            }
        };

        // Clear part of screen for input line
        screen_draw_rectl((v2i_t) {0, display_main->surface.height - font_height},
                          (v2i_t) {display_main->surface.width, display_main->surface.height}, COLOR_BLACK);

        ssfn_dst.x = 0;
        ssfn_dst.y = display_main->surface.height - font_height;
        ssfn_puts("#>");
        ssfn_puts(typingbuf);

        screen_draw();
        asm volatile("hlt");
    }
}
