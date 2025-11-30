//
// Created by tobin on 2025-11-25.
//

#ifndef KERN_SCREEN_H
#define KERN_SCREEN_H

#include "dialect.h"
#include "../../limine/limine.h"

//@formatter:off

#define COLOR_RED   0x00FF0000
#define COLOR_GREEN 0x0000FF00
#define COLOR_BLUE  0x000000FF
#define COLOR_WHITE 0x00FFFFFF
#define COLOR_BLACK 0xFF000000

/* Secondary Colors (CMY) */
#define COLOR_YELLOW        0x00FFFF00
#define COLOR_CYAN          0x0000FFFF
#define COLOR_MAGENTA       0x00FF00FF

/* Grayscale */
#define COLOR_GRAY          0x00808080  // Standard mid-gray
#define COLOR_SILVER        0x00C0C0C0  // Lighter gray
#define COLOR_DIM_GRAY      0x00696969  // Darker gray

/* Earth & Warm Tones */
#define COLOR_ORANGE        0x00FFA500
#define COLOR_BROWN         0x008B4513  // Saddle Brown
#define COLOR_MAROON        0x00800000
#define COLOR_GOLD          0x00FFD700

/* Cool & Dark Tones */
#define COLOR_PURPLE        0x00800080
#define COLOR_NAVY          0x00000080
#define COLOR_TEAL          0x00008080
#define COLOR_OLIVE         0x00808000

/* Pastels / Brights */
#define COLOR_PINK          0x00FFC0CB
#define COLOR_LIME          0x0032CD32//@formatter:on

typedef struct {
    u32 *address;
    u64 width, height;
    u64 pitch;
    u32 bpp;
} draw_surface_t;

typedef struct {
    draw_surface_t surface;
    draw_surface_t backbuffer;
    void *trueAddress;

    u8 index;
    u8 red_mask_shift;
    u8 green_mask_shift;
    u8 blue_mask_shift;
} display_t;


typedef struct {
    i32 x, y;
} v2i_t;

#define ABS(N) ((N<0)?(-N):(N))
#define V2I(__x, __y) (v2i_t) { .x = __x, .y = __y }
#define COLOR u32

//@formatter:off
u8 screen_init(struct limine_framebuffer_response *response, display_t *display_array, const u8 display_array_len) ;
u0 screen_put_pixel(v2i_t p, COLOR color);
u0 screen_draw_line(v2i_t a, v2i_t b, COLOR color);
u0 screen_clear(u32 color);
u0 screen_setactive(display_t *screen);
u0 screen_draw_rectl(v2i_t p1, v2i_t p2, COLOR color);
u0 screen_draw_box(v2i_t p1, v2i_t p2, COLOR color);
u0 screen_draw();
u0 screen_draw_rectl(v2i_t p1, v2i_t p2, u32 color);
//@formatter:on

static const i8 font_height = 16;
static const i8 font_width = 8;


#endif //KERN_SCREEN_H
