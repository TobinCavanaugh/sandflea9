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
#include  "../include/ssfn.h"

//TODO FAT FILE SYSTEM

display_t *display_main = 0;

extern char _binary_src_blob_regularfont_sfn_start;

extern u0 enable_sse(u0);

u64 sw = 0;
u32 offX = 0, offY = 0;
u0 timer_handler(const registers_t *reg) {
    sw += 10;
}

u64 alive = 0;

// typedef struct {
//     u8 bus, u8 slot, u8 func;
// } pci_device_t;

typedef struct {
    display_t *display_main;
    display_t **display_array;
    u64 usable_mem_size;
    u64 total_mem_size;
} system_t;

system_t system = {0};

char *content = null;
char typingbuf[255] = {0};

u0 handle_command() {
    char workingbuf[256] = {0};
    u64 add = PAGE_SIZE * 1000;

    if (str_eq(typingbuf, "kmalloc")) {
        u64 size = 0;

        serial_outs("Testing Kmalloc and page fault handling.\n");
        i64 inc = 0;
        while (1) {
            // 1.99 GB when running with qemu -2G
            size += add;
            kmalloc(add);
            stbsp_snprintf(workingbuf, 255, "%lldB\n", size);
            serial_outs(workingbuf);

            if (inc % 2 == 0) {
                screen_puts_r(workingbuf, V2I(0, font_height * 2), COLOR_WHITE, COLOR_BLACK);
                screen_draw();
            }

            ++inc;
        }
    }

    if (str_eq(typingbuf, "kmalloc2")) {
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
    }

    if (str_sw(typingbuf, "ext2")) {
        char *word = typingbuf;
        i32 x = 0;
        while (word[x] != 0) {
            if (word[x] == ' ') break;
            x++;
        }

        serial_outs(">>>");
        serial_outs(word + x + 1);
        serial_outs("<<<");

        ext2_inode_t *i = find_file_in_root(word + x + 1);

        if (i != null) {
            serial_outs("Yipeee\n");

            content = (char *) get_block_ptr(i->block[0]);
            serial_outs("vvv\n");
            serial_outs(content);
            serial_outs("\n^^^");
        } else {
            serial_outs("Bummer");
            content = "File Not Found";
        }
    }

    if (str_eq(typingbuf, "cls")) {
        typingbuf[0] = 0;
        content = null;
    }
}

void kern_entry(void) {
    init_serial();
    pci_scan_and_init_serial();

    enable_sse(); // cpu extension
    serial_outc('*');

    init_vmm_globals(hhdm_request); // virtual memory management
    serial_outc('v');

    init_pmm(memmap_request); // physical memory management
    serial_outc('p');

    interrupts_init(); // interrupts
    serial_outc('i');

    serial_outc('a');
    kmalloc_init(); // malloc

    interrupt_register(32, timer_handler);
    interrupt_register(33, keyboard_handle_keypress);

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
            default: { break; }
        }
    }
    system.usable_mem_size = usable_ram;
    system.total_mem_size = total_ram;

    serial_outs("memmapped\n");

    char buf[255];

    ext2_init(module_request);

    // failing here vvv

    v2i_t s = {0}, e = {0};
    for (;;) {
        // Keyboard input
        u8 k = 0;
        while ((k = keyboard_eat_key())) {
            i32 len = str_len(typingbuf);

            if (k == '\n') {
                handle_command();
                typingbuf[0] = 0;
            } else if (k == '\b') {
                typingbuf[len - 1] = '\0';
            } else {
                typingbuf[len] = k;
                typingbuf[len + 1] = 0;
                sw = 0;
            }
        }

        screen_clear(COLOR_BLACK);

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

        stbsp_snprintf(buf, 255, " %s ", (alive++ % 2) ? "*" : " ");
        p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;

        i32 w = (28 + 1) * font_width + 1;
        i32 rem = display_main->surface.width - w;

        for (int i = 0; i <= font_height; i += 4) {
            screen_draw_line(V2I(p.x, i), V2I(rem, i), COLOR_DIM_GRAY);
        }

        // p.x = rem;
        // stbsp_snprintf(buf, 255, " %-28s ", kpanicm);
        // p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;


        ssfn_dst.x = 0;
        ssfn_dst.y = font_height * 2;
        if (content != null) ssfn_puts(content);

        ssfn_dst.x = 0;
        ssfn_dst.y = display_main->surface.height - font_height;
        ssfn_puts("#>");
        ssfn_puts(typingbuf);

        screen_draw();

        asm volatile("hlt");
    }
}
