#include "kern_keyboard.h"
#include "kern_screen.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_serial.h"
#include "../include/kern_vmm.h"
#include "../include/kern_interrupts.h"
#include "../include/kern_mem.h"


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



u0 ssfn_putc2(char c) {
    if (c > 31) ssfn_putc(c);
    else {
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

void kern_entry(void) {
    init_serial();
    enable_sse();
    serial_outc('*');

    init_vmm_globals(hhdm_request);
    serial_outc('v');

    init_pmm(memmap_request);
    serial_outc('p');

    interrupts_init();
    serial_outc('i');

    serial_outc('a');
    kmalloc_init();

    interrupt_register(33, keyboard_handle_keypress);


    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        for (;;) {
            serial_outs("///DID NOT GET FRAMEBUFFER///");
            __asm__("hlt");
        }
    }

    serial_outs("Yo\n");

    display_t displays[32];
    screen_init(framebuffer_request.response, displays, 32);

    serial_outs("init\n");

    ssfn_src = (ssfn_font_t *) &_binary_src_blob_regularfont_sfn_start;

    u32 width = displays[0].framebuffer->width, height = displays[0].framebuffer->height;

    // 3. Configure SSFN destination using the Limine framebuffer data
    ssfn_dst.ptr = (u8 *) displays->framebuffer->address; // Cast to u8* for byte-wise arithmetic if needed by library
    ssfn_dst.w = width;
    ssfn_dst.h = height;
    ssfn_dst.p = displays->framebuffer->pitch;
    ssfn_dst.x = 0; // Start cursor at 0,0
    ssfn_dst.y = 0;
    ssfn_dst.fg = 0xFFFFFFFF; // White text

    serial_outs("ssfn\n");

    // ssfn_puts(hhdm_request.response->offset);
    //

    screen_clear(COLOR_BLACK);
    ssfn_puts("sandfleaOS\n");


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
    stbsp_snprintf(buf, 255, "%.1fMiB Usable\n", (f32) usable_ram / 1024 / 1024.);
    ssfn_puts(buf);

    stbsp_snprintf(buf, 255, "%.1fMiB Total\n", (f32) total_ram / 1024 / 1024.);
    ssfn_puts(buf);

    stbsp_snprintf(buf, 255, "%ldx%ld\n", width, height); //<-- here
    ssfn_puts(buf);

    // u64 size = 0;
    // while (1) {
    //     // 1.99 GB when running with qemu -2G
    //     u64 add = PAGE_SIZE * 100;
    //     size += add;
    //     kmalloc(add);
    //     stbsp_snprintf(buf, 255, "%lld\n", size);
    //     serial_outs(buf);
    // }


    for (;;) {
        screen_draw_line(ssfn_dst.x, ssfn_dst.y, width, height, 0xFF0000FF);
        asm volatile("hlt");
        u8 k = 0;
        while ((k = keyboard_eat_key())) {
            ssfn_putc2(k);
        }
    }
}
