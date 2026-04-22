#ifndef KERN_TERMINAL_H
#define KERN_TERMINAL_H

#include "dialect.h"

typedef struct screen_text_row_t {
    char *str;
    struct screen_text_row_t *next;
} screen_text_row_t;

extern screen_text_row_t *screen_text_root;
extern screen_text_row_t *screen_text_tail;
extern i32 screen_text_scroll;
extern u32 screen_text_row_len;

u0 screen_lines_init(u32 row_len);
u0 screen_push_line(const char *str);
u0 screen_push_linef(const char *fmt, ...);
u0 screen_terminal_clear();

#endif //KERN_TERMINAL_H
