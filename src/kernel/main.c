#include "../include/dialect.h"

#define STB_SPRINTF_IMPLEMENTATION

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
#include "kern_ext2.h"


#define SSFN_CONSOLEBITMAP_TRUECOLOR

#include "../include/kern_sched.h"
#include  "../include/ssfn.h"
#include "../include/util_cmd.h"

//TODO FAT FILE SYSTEM

display_t *display_main = 0;

extern char _binary_src_blob_regularfont_sfn_start;

extern u0 enable_sse(u0);

volatile u64 sw = 0;
u32 offX = 0, offY = 0;


typedef struct screen_text_row_t {
    char *str;
    struct screen_text_row_t *next;
} screen_text_row_t;

screen_text_row_t *screen_text_root = null;
i32 screen_text_scroll = 0;
u32 screen_text_row_len;

u0 screen_lines_init() {
    screen_text_root = kmalloc(sizeof(screen_text_row_t));
    screen_text_root->str = kmallocz(screen_text_row_len);
    screen_text_root->next = null;
    stbsp_snprintf(screen_text_root->str, screen_text_row_len, "sandfleaOS v0.0");
    return;
}

u0 screen_push_line(const char *str) {
    screen_text_row_t *cur = screen_text_root;
    while (cur->next != null) cur = cur->next;

    const char *ptr = str;

    while (*ptr) {
        cur->next = kmalloc(sizeof(screen_text_row_t));
        cur = cur->next;
        cur->next = null;
        cur->str = kmallocz(screen_text_row_len);

        i32 i = 0;
        while (*ptr && *ptr != '\n' && i < screen_text_row_len - 1) {
            cur->str[i++] = *ptr++;
        }
        cur->str[i] = 0;
        if (*ptr == '\n') ++ptr;

//        stbsp_snprintf(cur->next->str, screen_text_row_len, str);
//        cur->next->next = null;
    }
}

static char *screen_line_stb_callback(char *buf, void *user, i32 len) {
    char save = buf[len];
    buf[len] = '\0';
    screen_push_line(buf);
    buf[len] = save;
    return buf;
}

u0 screen_push_linef(const char *fmt, ...) {
    char local_buf[STB_SPRINTF_MIN + 1];

    va_list args;
    va_start(args, fmt);
    stbsp_vsprintfcb((char *(*)(const char *, void *, int)) screen_line_stb_callback, null, local_buf, fmt, args);
    va_end(args);
}


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

volatile u64 alive = 0;

typedef struct {
    display_t *display_main;
    display_t **display_array;

    u64 usable_mem_size;
    u64 total_mem_size;

    pci_device_t *pci_list_head;
} system_t;

system_t system = {0};

char *content = null;
char typingbuf[255] = {0};

u0 handle_command() {
    char workingbuf[256] = {0};
    u64 add = PAGE_SIZE * 1000;

    if (typingbuf[0] == '\0') return;


    cmd_word_t *word = cmd_parse(typingbuf, kmalloc);

    serial_outsf("[[%s]]\n", word->loc);

//    serial_outs("{{");
//    serial_outs(word->len);
//    serial_outs("}}");

    const char *_kmalloc = "kmalloc";
    if (str_eql(word->loc, _kmalloc, str_len(_kmalloc))) {
        u64 size = 0;

        serial_outs("Testing Kmalloc and page fault handling.\n");
        i64 inc = 0;
        while (1) {
            // 1.99 GB when running with qemu -2G
            size += add;
            kmalloc(add);
            stbsp_snprintf(workingbuf, 255, "%lldB\n", size);
            serial_outsf("%lldB\n", size);

            if (inc % 2 == 0) {
                screen_puts_r(workingbuf, V2I(0, font_height * 2), COLOR_WHITE, COLOR_BLACK);
                screen_draw();
            }

            ++inc;
        }

        goto Label_Free;
    }

    if (str_eql(word->loc, "cls", str_len("cls"))) {
        typingbuf[0] = 0;

        screen_text_row_t *row = screen_text_root;
        while (row != null) {
            screen_text_row_t *next = row->next;
            kfree(row->str);
            kfree(row);
            row = next;
        }

        goto Label_Free;
    }

    if (str_eql(word->loc, "ext2", str_len("ext2")) && word->next != null) {

        char *tmp = str_dup(word->next->loc, kmalloc);
        tmp[word->next->len] = 0;

        ext2_inode_t *i = find_file_in_root(tmp);
        screen_push_linef("File Size: %dB", i->size);

        kfree(tmp);

        if (i != null) {
            serial_outs("Yipeee\n");
            content = (char *) get_block_ptr(i->block[0]);
            screen_push_linef(content);
            serial_outsf("vvv\n%s\n^^^", content);
        } else {
            serial_outs("Bummer");
            content = "File Not Found";
        }

        goto Label_Free;
    }

    if (str_eql(word->loc, "kmalloc2", str_len("kmalloc2"))) {
        u64 sum = 0;

        serial_outs("Testing Kmalloc and freeing. This should result in no errors.\n");

        // Cycle through our memory 4 times, this should be enough to catch any page faults
        while (sum < system.total_mem_size * 4) {
            sum += add;
            void *dat = kmalloc(add);
            mem_set(dat, COLOR_MAGENTA, add);
            kfree(dat);

            stbsp_snprintf(workingbuf, 255, "%lld\n", sum);
            serial_outs(workingbuf);
        }
        goto Label_Free;
    }


    if (str_eq(typingbuf, "pci")) {
        ssfn_dst.y = font_height * 2;
        ssfn_dst.x = 0;

        pci_device_t *dev = system.pci_list_head;
        while (dev) {
            stbsp_snprintf(workingbuf, 255, "C:%X S:%X | V:%X D:%X\n",
                           dev->class_code, dev->subclass,
                           dev->vendor_id, dev->device_id);
            ssfn_puts(workingbuf);
            dev = dev->next;
        }

        ssfn_puts("Enter any key to continue\n");

        screen_draw();
        u8 k = 0;
        while (!(k = keyboard_eat_key())) {
            asm volatile("hlt");
        }
    }

    if (str_eq(typingbuf, "cls")) {
        typingbuf[0] = 0;
        // TODO CLEAR THE BUFFER
    }

    Label_Fail:
    // Fail here
    const char *failure = "Failure to parse command";
    screen_push_line(failure);
    serial_outsf("%s\n", failure);

    Label_Free:
    cmd_parse_free(word, kfree);

}


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
    serial_outsl("Serial initialized");

    enable_sse(); // cpu extension
    serial_outsl("SSE Enabled");

    init_vmm_globals(hhdm_request); // virtual memory management
    serial_outsl("VMM Initialized");

    init_pmm(memmap_request); // physical memory management
    serial_outsl("PMM Initialized");

    interrupts_init(); // interrupts
    serial_outsl("Interrupts initialized");

    kmalloc_init(); // malloc
    serial_outsl("Kmalloc Initialized");

    sched_init();
    serial_outsl("Scheduling Initialized");


    system.pci_list_head = pci_init_system();

    // In kern_entry...
    pci_device_t *pci_uart = system.pci_list_head;
    while (pci_uart) {
        // Look specifically for the WCH CH382 (1C00:3253)
        if (pci_uart->class_code == 0x7 && pci_uart->subclass == 0x0) {
            serial_outsl("[[[ ]]]");
            break;
        }
        pci_uart = pci_uart->next;
    }

    if (pci_uart) {
        serial_outsl("Found UART PCI Card");
        pci_enable_device_io(pci_uart); // Enable Bus Master / IO

        // // VISUAL DEBUG: Print BARs to the screen buffer so you can read them
        // char debug_buf[64];
        //
        // // Check BAR 0
        // stbsp_snprintf(debug_buf, 64, "BAR0: %x (IO=%d)",
        //                pci_uart->bars[0], pci_uart->bars[0] & 1);
        // // Assuming you have a function to print to screen directly since serial is dead
        // screen_puts_r(debug_buf, V2I(0, 0), COLOR_RED, COLOR_BLACK);
        //
        // // Check BAR 1
        // stbsp_snprintf(debug_buf, 64, "BAR1: %x (IO=%d)",
        //                pci_uart->bars[1], pci_uart->bars[1] & 1);
        // screen_puts_r(debug_buf, V2I(0, 16), COLOR_RED, COLOR_BLACK);

        // Try initializing BOTH BARs
        for (int i = 0; i < 2; i++) {
            u32 bar = pci_uart->bars[i];
            if ((bar & 1) == 1) {
                // If IO space
                u32 io_base = bar & 0xFFFFFFFC;
                if (io_base < 0xFFFF) {
                    pci_enable_device_io(pci_uart);
                    pci_init_uart_port(io_base);

                    // Send specific identifier for each port
                    if (i == 0) pci_serial_putsl(pci_uart, "OUTPUT FROM BAR 0");
                    if (i == 1) pci_serial_putsl(pci_uart, "OUTPUT FROM BAR 1");
                }
            }
        }
    }

    // --------------

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        for (;;) {
            serial_outs("///DID NOT GET FRAMEBUFFER///");
            __asm__("hlt");
        }
    }


    display_t displays[32];
    screen_init(framebuffer_request.response, displays, 32);
    display_main = &displays[0];
    serial_outs("Yo\n");

    serial_outs("init\n");

    ssfn_src = (ssfn_font_t *) &_binary_src_blob_regularfont_sfn_start;

    u32 width = displays[0].surface.width, height = displays[0].surface.height;
    screen_text_row_len = width / font_width;

    // 3. Configure SSFN destination using the Limine framebuffer data
    ssfn_dst.ptr = (u8 *) displays->surface.address;
    // Cast to u8* for byte-wise arithmetic if needed by library
    ssfn_dst.w = width;
    ssfn_dst.h = height;
    ssfn_dst.p = displays->surface.pitch;
    ssfn_dst.x = 0; // Start cursor at 0,0
    ssfn_dst.y = 0;
    ssfn_dst.fg = 0xFFFFFFFF; // White text

    serial_outs("ssfn\n");

    // ssfn_puts(hhdm_request.response->offset);
    //

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

    serial_outs("memmapped\n");


    serial_outsf("%d\n", screen_text_row_len);

    // Screen buffer
    screen_lines_init();

    serial_outs("text screen buffer\n");

    ext2_init(module_request);


    char buf[255];


    sched_create_thread(shimmy);
    sched_create_thread(shimmy2);
    sched_create_thread(shimmy3);

    interrupt_register(32, timer_handler);
    interrupt_register(33, (void (*)(const registers_t *)) keyboard_handle_keypress);

    v2i_t s = {0}, e = {0};
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
                typingbuf[len - 1] = '\0';
            } else if (k == KEY_DOWN) {
                ++screen_text_scroll;
                serial_outsf("DOWN");
            } else if (k == KEY_UP) {
                --screen_text_scroll;
                screen_text_scroll = max(screen_text_scroll, 0);
                serial_outsf("UP");
            } else {
                typingbuf[len] = k;
                typingbuf[len + 1] = 0;
                sw = 0;
            }
        }

        screen_clear(COLOR_BLACK);

        u32 start_y = font_height;
        screen_text_row_t *current = screen_text_root;
        i32 i = 0;
        while (current != null) {
            // Check if the current line is visible (index >= scroll)
            if (i >= screen_text_scroll) {

                // Calculate relative row index
                i32 relative_row = i - screen_text_scroll;

                ssfn_dst.x = 0;
                // Logic: Header Offset + (Row Index * Height)
                ssfn_dst.y = start_y + (relative_row * font_height);

                // Optional: Stop drawing if we go off the bottom of the screen
                if (ssfn_dst.y < display_main->surface.height - font_height) {
                    ssfn_puts(current->str);
                }
            }

            // ALWAYS advance the list, regardless of whether we drew the line
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
            //
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

        ssfn_dst.x = 100;
//        ssfn_putc(heartbeat1 ? 'A' : 'B');

        screen_draw();

        asm volatile("hlt");
    }
}
