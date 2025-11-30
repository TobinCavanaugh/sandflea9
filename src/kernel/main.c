#include "../include/kern_screen.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_serial.h"
#include "../include/kern_vmm.h"
#include "../include/kern_interrupts.h"
#include "../include/kern_keyboard.h"
#include "../include/kern_mem.h"

//TODO SWITCH TO SSFN RENDER
//TODO FAT FILE SYSTEM

typedef __builtin_va_list va_list;

#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)
#define va_copy(dest, src)  __builtin_va_copy(dest, src)

#define STB_SPRINTF_IMPLEMENTATION
#define size_t u64
#define ptrdiff_t i64
#include "../include/stb_sprintf.h"

#include "../include/dialect.h"
#include "../../limine/limine.h"

#define SSFN_CONSOLEBITMAP_TRUECOLOR

#include  "../include/ssfn.h"

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


display_t *display_main = 0;

u0 ssfn_putc2(char c) {
    // why doesn't this clamp work....
    ssfn_dst.x = clamp(ssfn_dst.x, 0, display_main->surface.width - 1 - font_width);
    ssfn_dst.y = clamp(ssfn_dst.y, 0, display_main->surface.height - 1 - font_width);

    if (c > 31) {
        ssfn_putc(c);
    } else {
        if (c == '\n') {
            ssfn_dst.x = 0;
            ssfn_dst.y += font_height;
        } else if (c == '\t') {
            ssfn_dst.x += font_width * 5;
        }
    }
}

u0 ssfn_puts(char *str) {
    i32 i = 0;
    while (str[i] != 0) ssfn_putc2(str[i++]);
}

extern char _binary_src_blob_regularfont_sfn_start;

extern u0 enable_sse(u0);


u64 sw = 0;
u32 offX = 0, offY = 0;
u0 timer_handler(const registers_t *reg) {
    sw += 10;
}

u32 str_len(const char *str) {
    i32 i = 0;
    while (str[i]) {
        i++;
    }
    return i;
}

u0 screen_puts_nb(const char *str, v2i_t loc, COLOR fg) {
    ssfn_dst.x = loc.x;
    ssfn_dst.y = loc.y;
    ssfn_dst.fg = fg;
    ssfn_dst.bg = COLOR_BLACK; // useless

    i32 i = 0;
    while (str[i] != 0) ssfn_putc2(str[i++]);
}

v2i_t screen_puts_c(const char *str, v2i_t loc, COLOR fg, COLOR bg) {
    ssfn_dst.x = loc.x;
    ssfn_dst.y = loc.y;
    ssfn_dst.fg = fg;
    ssfn_dst.bg = bg; // useless

    v2i_t newP = {ssfn_dst.x + str_len(str) * font_width, ssfn_dst.y + font_height};
    screen_draw_box(V2I(loc.x, loc.y), newP, bg);

    i32 i = 0;
    while (str[i] != 0) ssfn_putc2(str[i++]);

    newP.y -= font_height;
    return newP;
}

v2i_t screen_puts_r(const char *str, v2i_t loc, COLOR fg, COLOR bg) {
    v2i_t e = screen_puts_c(str, loc, fg, bg);
    screen_draw_rectl(loc, V2I(e.x, e.y + font_height), fg);
    return e;
}

u32 alive = 0;

void kern_entry(void) {
    init_serial();
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

    char buf[255];

    // u64 size = 0;
    // while (1) {
    //     // 1.99 GB when running with qemu -2G
    //     u64 add = PAGE_SIZE * 100;
    //     size += add;
    //     kmalloc(add);
    //     stbsp_snprintf(buf, 255, "%lld\n", size);
    //     serial_outs(buf);
    // }

    char *kpanicm = "Control Protection Exception";


    v2i_t s = {0}, e = {0};
    for (;;) {
        // // Keyboard input
        // u8 k = 0;
        // while ((k = keyboard_eat_key())) {
        //     ssfn_putc2(k);
        //     sw = 0;
        // }

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

        stbsp_snprintf(buf, 255, " IO %-5s ", "/////");
        p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;

        i32 w = (28 + 2) * font_width;
        i32 rem = display_main->surface.width - w;


        for (int i = 0; i <= font_height; i += 4) {
            screen_draw_line(V2I(p.x, i), V2I(rem, i), COLOR_WHITE);
        }

        p.x = rem;
        stbsp_snprintf(buf, 255, " %-28s ", kpanicm);
        p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;


        screen_draw();

        asm volatile("hlt");
    }
}
