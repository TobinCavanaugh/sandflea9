//
// Created by tobin on 2025-11-25.
//

#ifndef KERN_SCREEN_H
#define KERN_SCREEN_H

#include "../include/dialect.h"
#include "../../limine/limine.h"

//@formatter:off
#define COLOR_RED   0x00FF0000
#define COLOR_GREEN 0x0000FF00
#define COLOR_BLUE  0x000000FF
#define COLOR_WHITE 0x00FFFFFF
#define COLOR_BLACK 0x00000000
//@formatter:on

typedef struct {
    u32 *address;
    u64 width, height;
    u64 pitch;
    u32 bpp;
} draw_surface_t;

typedef struct {
    draw_surface_t surface;
    draw_surface_t backbuffer;
    struct limine_framebuffer *framebuffer;
} display_t;


typedef struct {
    i32 x, y;
} v2i_t;

#define ABS(N) ((N<0)?(-N):(N))
#define V2I(__x, __y) (v2i_t) { .x = __x, .y = __y }

//@formatter:off
u8 screen_init(struct limine_framebuffer_response *response, display_t *display_array, const u8 display_array_len) ;
u0 screen_put_pixel(v2i_t p, u32 color);
u0 screen_draw_line(i64 x0, i64 y0, i64 x1, i64 y1, u32 color);
u0 screen_clear(u32 color);
u0 screen_setactive(display_t *screen);
//@formatter:on

static const i8 font_height = 16;
static const i8 font_width = 8;


#endif //KERN_SCREEN_H
