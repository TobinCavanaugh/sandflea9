//
// Created by tobin on 2025-11-25.
//

#include "../include/kern_screen.h"

#include "../include/kern_mem.h"
#include "../include/kern_vmm.h"
#include "../include/kern_serial.h"
#include "../include/ssfn.h"
#include "../util/util_str.h"

display_t *current_display;
// struct limine_framebuffer *current_display->surface;

// Returns the count of framebuffers (displays)
// places N (N=display_array_len) display_t's into display_array
u8 screen_init(struct limine_framebuffer_response *response, display_t *display_array, const u8 display_array_len) {
    i32 i = 0;
    for (; i < response->framebuffer_count; i++) {
        if (i < display_array_len) {
            struct limine_framebuffer *fb = response->framebuffers[i];

            display_t *v = &display_array[i];

            v->index = i;

            v->red_mask_shift = fb->red_mask_shift;
            v->green_mask_shift = fb->green_mask_shift;
            v->blue_mask_shift = fb->blue_mask_shift;

            v->surface.height = fb->height;
            v->surface.width = fb->width;
            v->surface.pitch = fb->pitch;
            v->surface.bpp = fb->bpp;

            v->backbuffer.height = fb->height;
            v->backbuffer.width = fb->width;
            v->backbuffer.pitch = fb->pitch;
            v->backbuffer.bpp = fb->bpp;

            u64 fb_phys = 0;
            u64 hhdm = vmm_get_hhdm();
            if ((u64)fb->address >= hhdm) {
                fb_phys = (u64)fb->address - hhdm;
            } else {
                fb_phys = vmm_get_phys_in_pml4(read_cr3(), (u64)fb->address);
            }
            u64 fb_size = (u64)fb->pitch * fb->height;
            u64 wc_virt = 0;
            if (fb_phys != 0) {
                wc_virt = vmm_wc_map_phys(fb_phys, fb_size);
            }
            if (wc_virt != 0) {
                v->trueAddress = (void *)wc_virt;
                serial_outsf("Screen: Display %d Framebuffer WC mapped at 0x%016llx (phys 0x%016llx)\n", i, wc_virt, fb_phys);
            } else {
                v->trueAddress = fb->address;
            }

            // v->surface.address = fb->address;
            v->surface.address = kmalloc(v->surface.pitch * v->surface.height);

            screen_setactive(&display_array[i]);
        }
    }

    return i;
}

u0 screen_setactive(display_t *screen) {
    current_display = screen;
}

u0 screen_draw() {
    mem_copy(current_display->trueAddress, current_display->surface.address,
             current_display->surface.pitch * current_display->surface.height);
}

u0 screen_put_pixel(v2i_t p, u32 color) {
    if (current_display->surface.address == NULL) return;

    // Boundary checks to prevent overwriting memory outside the screen
    if (p.x < 0 || p.x >= current_display->surface.width || p.y < 0 || p.y >= current_display->surface.height) return;

    // Calculate pixel index: y * (pitch / 4 bytes per pixel) + x
    u64 pixel_index = p.y * (current_display->surface.pitch / 4) + p.x;
    u32 *fb_ptr = (u32 *) current_display->surface.address;

    fb_ptr[pixel_index] = color;
}

// Draw a filled rectangle between two points
u0 screen_draw_box(v2i_t p1, v2i_t p2, u32 color) {
    if (current_display->surface.address == NULL) return;

    // Normalize coordinates (handle p1 being bottom-right, etc.)
    i64 x0 = min(p1.x, p2.x);
    i64 y0 = min(p1.y, p2.y);
    i64 x1 = max(p1.x, p2.x);
    i64 y1 = max(p1.y, p2.y);

    // Screen boundary checks (Clipping)
    // If the box is completely off-screen, do nothing
    if (x0 >= current_display->surface.width || y0 >= current_display->surface.height || x1 < 0 || y1 < 0) return;

    // Clamp coordinates to the screen edges
    x0 = max(0, x0);
    y0 = max(0, y0);
    x1 = min(current_display->surface.width - 1, x1);
    y1 = min(current_display->surface.height - 1, y1);

    u32 *fb_ptr = (u32 *) current_display->surface.address;
    u64 pitch_pixels = current_display->surface.pitch / 4; // Assuming 32bpp (4 bytes)

    // Optimized loop: calculate row offset once per line
    for (i64 y = y0; y <= y1; y++) {
        // Pointer to the start of the row + x offset
        u32 *pixel_cursor = fb_ptr + (y * pitch_pixels) + x0;
        u64 count = x1 - x0 + 1;
        if (count > 0) {
            mem_set32(pixel_cursor, color, count);
        }
    }
}

// Draw a hollow rectangle (outline) between two points
u0 screen_draw_rectl(v2i_t p1, v2i_t p2, u32 color) {
    // Determine corners
    i64 x0 = p1.x;
    i64 y0 = p1.y;
    i64 x1 = p2.x;
    i64 y1 = p2.y;

    // Draw 4 lines connecting the corners
    screen_draw_line(V2I(x0, y0), V2I(x1, y0), color);
    screen_draw_line(V2I(x0, y1), V2I(x1, y1), color);
    screen_draw_line(V2I(x0, y0), V2I(x0, y1), color);
    screen_draw_line(V2I(x1, y0), V2I(x1, y1), color);
}

u0 screen_draw_line(v2i_t a, v2i_t b, COLOR color) {
    i64 x0 = a.x, y0 = a.y, x1 = b.x, y1 = b.y;

    i64 dx = ABS(x1 - x0);
    i64 sx = x0 < x1 ? 1 : -1;
    i64 dy = -ABS(y1 - y0);
    i64 sy = y0 < y1 ? 1 : -1;
    i64 err = dx + dy;
    i64 e2;

    for (;;) {
        screen_put_pixel(V2I(x0, y0), color);

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

u0 screen_clear(u32 color) {
    u32 *fb_ptr = (u32 *) current_display->surface.address;
    u64 pixel_count = current_display->surface.width * current_display->surface.height;
    mem_set32(fb_ptr, color, pixel_count);
}

u32 color_rgb(u32 r, u32 g, u32 b) {
    if (current_display->surface.address == NULL) return 0;

    //hmm
    return ((r & 0xFF) << current_display->red_mask_shift) |
           ((g & 0xFF) << current_display->green_mask_shift) |
           ((b & 0xFF) << current_display->blue_mask_shift);
}


u0 screen_puts_nb(const char *str, v2i_t loc, COLOR fg) {
    ssfn_dst.x = loc.x;
    ssfn_dst.y = loc.y;
    ssfn_dst.fg = fg;
    ssfn_dst.bg = COLOR_BLACK; // useless

    i32 i = 0;
    while (str[i] != 0) ssfn_putc2(str[i++], true);
}

v2i_t screen_puts_c(const char *str, v2i_t loc, COLOR fg, COLOR bg) {
    ssfn_dst.x = loc.x;
    ssfn_dst.y = loc.y;
    ssfn_dst.fg = fg;
    ssfn_dst.bg = bg; // useless

    v2i_t newP = {ssfn_dst.x + str_len(str) * font_width, ssfn_dst.y + font_height};
    screen_draw_box(V2I(loc.x, loc.y), newP, bg);

    i32 i = 0;
    while (str[i] != 0) ssfn_putc2(str[i++], true);

    newP.y -= font_height;
    return newP;
}

v2i_t screen_puts_r(const char *str, v2i_t loc, COLOR fg, COLOR bg) {
    v2i_t e = screen_puts_c(str, loc, fg, bg);
    screen_draw_rectl(loc, V2I(e.x, e.y + font_height), fg);
    return e;
}


u0 ssfn_puts(char *str) {
    i32 i = 0;
    while (str[i] != 0) ssfn_putc2(str[i++], true);
}

u0 ssfn_putc2(char c, u8 wrapx) {
    // why doesn't this clamp work....
    u32 maxw = current_display->surface.width - 1 - font_width;
    ssfn_dst.x = clamp(ssfn_dst.x, 0, maxw);
    ssfn_dst.y = clamp(ssfn_dst.y, 0, current_display->surface.height - 1 - font_height);

    if (c > 31) {
        if (wrapx && ssfn_dst.x >= maxw) {
            ssfn_dst.x = 0;
            ssfn_dst.y += font_height;
        }
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

display_t * screen_current_display() {
    return current_display;
}