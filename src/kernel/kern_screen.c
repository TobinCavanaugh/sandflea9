//
// Created by tobin on 2025-11-25.
//

#include "kern_screen.h"

#include "../include/kern_mem.h"

// display_t *current_display;

struct limine_framebuffer *g_fb;

// Returns the count of framebuffers (displays)
// places N (N=display_array_len) display_t's into display_array
u8 screen_init(struct limine_framebuffer_response *response, display_t *display_array, const u8 display_array_len) {
    i32 i = 0;
    for (; i < response->framebuffer_count; i++) {
        if (i < display_array_len) {
            struct limine_framebuffer *fb = response->framebuffers[i];

            display_t *v = &display_array[i];

            v->framebuffer = fb;

            v->surface.height = fb->height;
            v->surface.width = fb->width;
            v->surface.pitch = fb->pitch;
            v->surface.bpp = fb->bpp;
            // v->surface->address  // malloc

            v->backbuffer.height = fb->height;
            v->backbuffer.width = fb->width;
            v->backbuffer.pitch = fb->pitch;
            v->backbuffer.bpp = fb->bpp;

            screen_setactive(&display_array[i]);
        }
    }

    return i;
}

u0 screen_setactive(display_t *screen) {
    g_fb = screen->framebuffer;
    // current_display = screen;''
}

u0 screen_draw() {
    // mem_move()
    // mem_copy(current_display->framebuffer->address)
}

u0 screen_put_pixel(v2i_t p, u32 color) {
    // __auto_type g_fb = current_display->backbuffer;
    if (g_fb == NULL) return;

    // Boundary checks to prevent overwriting memory outside the screen
    if (p.x < 0 || p.x >= g_fb->width || p.y < 0 || p.y >= g_fb->height) return;

    // Calculate pixel index: y * (pitch / 4 bytes per pixel) + x
    u64 pixel_index = p.y * (g_fb->pitch / 4) + p.x;
    u32 *fb_ptr = (u32 *) g_fb->address;

    fb_ptr[pixel_index] = color;
}

u0 screen_draw_line(i64 x0, i64 y0, i64 x1, i64 y1, u32 color) {
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
    u32 *fb_ptr = (u32 *) g_fb->address;
    u64 pixel_count = g_fb->width * g_fb->height;
    for (u64 i = 0; i < pixel_count; i++) {
        fb_ptr[i] = color;
    }
}

u32 color_rgb(u32 r, u32 g, u32 b) {
    if (g_fb == NULL) return 0;

    return ((r & 0xFF) << g_fb->red_mask_shift) |
           ((g & 0xFF) << g_fb->green_mask_shift) |
           ((b & 0xFF) << g_fb->blue_mask_shift);
}
