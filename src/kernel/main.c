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

typedef struct {
    i32 x, y;
} v2i_t;

#define V2I(__x, __y) (v2i_t) { .x = __x, .y = __y }

u0 toggle_capslock() {
    static u8 led_state = 0;
    led_state = led_state ^ 0x07;

    // Timeout loop 1
    u32 timeout = 100000;
    while ((inb(0x64) & 2) != 0 && --timeout);
    if (timeout == 0) return; // Controller stuck, abort

    outb(0x60, 0xED);

    // Timeout loop 2
    timeout = 100000;
    while ((inb(0x64) & 2) != 0 && --timeout);
    if (timeout == 0) return;

    outb(0x60, led_state);
}

char get_ascii_from_scancode(u8 scancode) {
    // Make sure we're only handling key presses (not releases)
    if (scancode & 0x80) {
        return 0;
    }
    // Complete mapping of keys to ASCII characters
    switch (scancode) {
        // Letters
        case 0x1E: return 'a';
        case 0x30: return 'b';
        case 0x2E: return 'c';
        case 0x20: return 'd';
        case 0x12: return 'e';
        case 0x21: return 'f';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x17: return 'i';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';
        case 0x32: return 'm';
        case 0x31: return 'n';
        case 0x18: return 'o';
        case 0x19: return 'p';
        case 0x10: return 'q';
        case 0x13: return 'r';
        case 0x1F: return 's';
        case 0x14: return 't';
        case 0x16: return 'u';
        case 0x2F: return 'v';
        case 0x11: return 'w';
        case 0x2D: return 'x';
        case 0x15: return 'y';
        case 0x2C: return 'z';

        // Numbers
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';

        // Special keys
        case 0x1C: return '\n'; // Enter
        case 0x39: return ' '; // Space
        case 0x0E: return '\b'; // Backspace
        case 0x0F: return '\t'; // Tab
        case 0x01: return '\0'; // Escape (ASCII 27)

        // Symbols on number row
        case 0x29: return '`'; // Grave accent
        case 0x0C: return '-'; // Minus
        case 0x0D: return '='; // Equals

        // Symbols on letter rows
        case 0x1A: return '['; // Left bracket
        case 0x1B: return ']'; // Right bracket
        case 0x2B: return '\\'; // Backslash
        case 0x27: return ';'; // Semicolon
        case 0x28: return '\''; // Single quote
        case 0x33: return ','; // Comma
        case 0x34: return '.'; // Period
        case 0x35: return '/'; // Forward slash

        default: return 0; // Unmapped key
    }
}

char keyboard_shift(char c) {
    // Letters can be handled with simple ASCII math
    if (c >= 'a' && c <= 'z') {
        return c - 32; // Convert lowercase to uppercase
    }

    // For other characters, use lookup tables
    const char *unshifted = "`1234567890-=[]\\;',./";
    const char *shifted = "~!@#$%^&*()_+{}|:\"<>?";

    // Search for the character in the unshifted string
    for (int i = 0; unshifted[i] != '\0'; i++) {
        if (c == unshifted[i]) {
            return shifted[i]; // Return the corresponding shifted character
        }
    }

    // If no shift equivalent found, return the same character
    return c;
}

u8 shift_down = false;
u0 handle_keypress(registers_t *t) {
    u8 status = inb(0x64);

    if (status & 0x01) {
        u8 sc = inb(0x60);

        // Key release
        if (sc & 0x80) {
            u8 released = sc & 0x7F;

            if ((released == 0x2A || released == 0x36)) {
                shift_down = 0;
            }
        }

        if (sc >= 0x80) {
            return;
        }

        if ((sc == 0x2A || sc == 0x36)) {
            shift_down = 1;
            return;
        }

        u8 ascii = get_ascii_from_scancode(sc);

        if (shift_down) {
            ascii = keyboard_shift(ascii);
        }

        ssfn_putc(ascii);
    }
}


#define ABS(N) ((N<0)?(-N):(N))

static struct limine_framebuffer *g_fb = NULL;

u0 put_pixel(v2i_t p, u32 color) {
    if (g_fb == NULL) return;

    // Boundary checks to prevent overwriting memory outside the screen
    if (p.x < 0 || p.x >= g_fb->width || p.y < 0 || p.y >= g_fb->height) return;

    // Calculate pixel index: y * (pitch / 4 bytes per pixel) + x
    u64 pixel_index = p.y * (g_fb->pitch / 4) + p.x;
    u32 *fb_ptr = (u32 *) g_fb->address;

    fb_ptr[pixel_index] = color;
}

u0 draw_line(i64 x0, i64 y0, i64 x1, i64 y1, u32 color) {
    i64 dx = ABS(x1 - x0);
    i64 sx = x0 < x1 ? 1 : -1;
    i64 dy = -ABS(y1 - y0);
    i64 sy = y0 < y1 ? 1 : -1;
    i64 err = dx + dy;
    i64 e2;

    for (;;) {
        put_pixel(V2I(x0, y0), color);

        if (x0 == x1 && y0 == y1) break;

        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

u0 draw_clear(u32 color) {
    u32 *fb_ptr = (u32 *) g_fb->address;
    u64 pixel_count = g_fb->width * g_fb->height;
    for (u64 i = 0; i < pixel_count; i++) {
        fb_ptr[i] = color;
    }
}

//@formatter:off
#define COLOR_RED   0x00FF0000
#define COLOR_GREEN 0x0000FF00
#define COLOR_BLUE  0x000000FF
#define COLOR_WHITE 0x00FFFFFF
#define COLOR_BLACK 0x00000000
//@formatter:on

u32 color_rgb(u32 r, u32 g, u32 b) {
    if (g_fb == NULL) return 0;

    return ((r & 0xFF) << g_fb->red_mask_shift) |
           ((g & 0xFF) << g_fb->green_mask_shift) |
           ((b & 0xFF) << g_fb->blue_mask_shift);
}

i8 font_height = 16;
i8 font_width = 8;

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

    interrupt_register(33, handle_keypress);

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        for (;;) {
            serial_outs("///DID NOT GET FRAMEBUFFER///");
            __asm__("hlt");
        }
    }

    g_fb = framebuffer_request.response->framebuffers[0];

    ssfn_src = (ssfn_font_t *) &_binary_src_blob_regularfont_sfn_start;

    // 3. Configure SSFN destination using the Limine framebuffer data
    ssfn_dst.ptr = (u8 *) g_fb->address; // Cast to u8* for byte-wise arithmetic if needed by library
    ssfn_dst.w = g_fb->width;
    ssfn_dst.h = g_fb->height;
    ssfn_dst.p = g_fb->pitch;
    ssfn_dst.x = 0; // Start cursor at 0,0
    ssfn_dst.y = 0;
    ssfn_dst.fg = 0xFFFFFFFF; // White text

    // ssfn_puts(hhdm_request.response->offset);
    //

    draw_clear(COLOR_BLACK);
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
    stbsp_snprintf(buf, 255, "%fMiB\n", (f32) total_ram / 1024 / 1024.);
    ssfn_puts(buf);

    stbsp_snprintf(buf, 255, "%ldx%ld\n", g_fb->width, g_fb->height); //<-- here
    ssfn_puts(buf);


    for (;;) {
        draw_line(ssfn_dst.x, ssfn_dst.y, g_fb->width, g_fb->height, 0xFF0000FF);
        asm volatile("hlt");
    }
}
