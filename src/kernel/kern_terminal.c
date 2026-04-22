#include "../include/kern_terminal.h"
#include "../include/kern_mem.h"
#include "../include/kern_vmm.h"
#include "../include/stbsupport.h"
#include "../include/dialect.h"

screen_text_row_t *screen_text_root = null;
screen_text_row_t *screen_text_tail = null;
i32 screen_text_scroll = 0;
u32 screen_text_row_len = 0;

u0 screen_lines_init(u32 row_len) {
    screen_text_row_len = row_len;
    screen_text_root = kmalloc(sizeof(screen_text_row_t));
    screen_text_root->str = kmallocz(screen_text_row_len);
    screen_text_root->next = null;
    screen_text_tail = screen_text_root;
    stbsp_snprintf(screen_text_root->str, screen_text_row_len, "sandfleaOS v0.0");
}

u0 screen_push_line(const char *str) {
    if (!screen_text_tail) return;

    const char *ptr = str;

    while (*ptr) {
        screen_text_row_t *new_row = kmalloc(sizeof(screen_text_row_t));
        new_row->next = null;
        new_row->str = kmallocz(screen_text_row_len);

        i32 i = 0;
        while (*ptr && *ptr != '\n' && i < screen_text_row_len - 1) {
            new_row->str[i++] = *ptr++;
        }
        new_row->str[i] = 0;
        if (*ptr == '\n') ++ptr;

        screen_text_tail->next = new_row;
        screen_text_tail = new_row;
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

u0 screen_terminal_clear() {
    screen_text_row_t *row = screen_text_root;
    while (row != null) {
        screen_text_row_t *next = row->next;
        kfree(row->str);
        kfree(row);
        row = next;
    }
    screen_text_root = null;
    screen_text_tail = null;
    screen_lines_init(screen_text_row_len);
}
