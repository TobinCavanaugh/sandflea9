#include "../include/kern_terminal.h"
#include "../include/kern_mem.h"
#include "../include/kern_vmm.h"
#include "../include/stbsupport.h"
#include "../include/dialect.h"
#include "../util/util_str.h"

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

i32 screen_get_line_count() {
    i32 c = 0;
    screen_text_row_t *row = screen_text_root;
    while (row) {
        ++c;
        row = row->next;
    }
    return c;
}

#define TERMINAL_MAX_ROWS 1000

u0 screen_push_buf(const char *buf, i32 len) {
    if (!buf || len <= 0) return;

    u64 irq = save_irq_and_disable();

    screen_text_row_t *cur = screen_text_root;
    i32 count = 1;
    // Advance to the end of the current list and count rows
    while (cur->next != null) {
        cur = cur->next;
        count++;
    }

    const char *ptr = buf;
    const char *end = buf + len;

    while (ptr < end) {
        // Allocate and link new row
        screen_text_row_t *new_row = kmalloc(sizeof(screen_text_row_t));
        new_row->str = kmallocz(screen_text_row_len);
        new_row->next = null;
        
        cur->next = new_row;
        cur = new_row;
        count++;

        i32 i = 0;
        while (ptr < end && *ptr != '\n' && i < screen_text_row_len - 1) {
            cur->str[i++] = *ptr++;
        }

        cur->str[i] = 0;
        if (ptr < end && *ptr == '\n') {
            ptr++;
        }
    }

    // Prune old rows if exceeding limit
    if (count > TERMINAL_MAX_ROWS) {
        i32 to_remove = count - TERMINAL_MAX_ROWS;
        while (to_remove-- > 0 && screen_text_root != null) {
            screen_text_row_t *old = screen_text_root;
            screen_text_root = screen_text_root->next;
            kfree(old->str);
            kfree(old);
        }
    }

    restore_irq(irq);
}

u0 screen_push_line(const char *str) {
    screen_push_buf(str, str_len(str));
}

static char *screen_line_stb_callback(char *buf, void *user, i32 len) {
    if (len <= 0) return buf;
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
