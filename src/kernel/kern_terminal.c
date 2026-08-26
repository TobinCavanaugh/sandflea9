// src/kernel/kern_terminal.c
//
// Terminal emulation — two layers:
//   Layer 1: Scrollback (linked list of text rows) — unchanged, ~200 callers
//   Layer 2: Cell buffer (cols × rows grid with colors) + ANSI parser
//
// Sessions bundle a cell buffer + scrollback + keyboard queue into one unit.
// Global swap on session switch keeps all old code working.

#include "../include/kern_terminal.h"
#include "../include/kern_mem.h"
#include "../include/kern_vmm.h"
#include "../include/kern_profile.h"
#include "../include/stbsupport.h"
#include "../include/dialect.h"
#include "../include/ssfn.h"
#include "../include/kern_screen.h"
#include "../include/kern_sched.h"
#include "../include/kern_compositor.h"
#include "../include/kern_ext2.h"
#include "../include/string.h"
#include "../util/util_str.h"

// ============================================================================
// External globals from other translation units
// ============================================================================

// Defined in main.c, used by session_save/restore_globals
// (foreground_proc is declared in kern_sched.h via the include above)
extern char typingbuf[256];

// ============================================================================
// Forward declarations
// ============================================================================

static void ansi_putc(u8 c);
static void ansi_execute(ansi_parser_t *p, term_session_t *s);
static void apply_sgr(term_session_t *s, ansi_parser_t *p);
static void scroll_cell_buffer(term_session_t *s);
static u32 ansi_std_color(u8 index);
static void draw_header_bar();

// Constants for the status bar / header area
#define STATUS_BAR_HEIGHT font_height
#define HEADER_HEIGHT STATUS_BAR_HEIGHT

// ============================================================================
// Scrollback globals (old API — swapped on session switch)
// ============================================================================

screen_text_row_t *screen_text_root = NULL;
screen_text_row_t *screen_text_tail = NULL;
i32 screen_text_scroll = 0;
u32 screen_text_row_len = 0;

// ============================================================================
// Session globals
// ============================================================================

term_session_t *active_session = NULL;
term_session_t sessions[MAX_SESSIONS] = {0};

// ============================================================================
// ANSI parser global state (one parser, shared by all sessions)
// ============================================================================

static ansi_parser_t ansi_parser = {0};

// ============================================================================
// Helper: get screen dimensions from current display
// ============================================================================

static void term_get_dims(u16 *cols, u16 *rows) {
    display_t *disp = screen_current_display();
    if (disp) {
        *cols = (u16)(disp->surface.width / font_width);
        // Account for status bar + input line (2 rows at top/bottom)
        *rows = (u16)((disp->surface.height - STATUS_BAR_HEIGHT * 2) / font_height);
        if (*rows < 10) *rows = 10;
        if (*cols < 40) *cols = 40;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

// ============================================================================
// Session Management
// ============================================================================

i32 session_init(u32 id, u16 cols, u16 rows) {
    if (id >= MAX_SESSIONS) return -1;

    term_session_t *s = &sessions[id];
    kfree(s->cells);        // free any previous buffer (re-init)
    s->cells = NULL;
    kfree(s->alt_cells);
    s->alt_cells = NULL;

    s->id   = id;
    stbsp_snprintf(s->name, sizeof(s->name), "tty%d", id);
    s->cols = cols;
    s->rows = rows;

    s->cells = kmalloc(cols * rows * sizeof(cell_t));
    if (!s->cells) return -1;
    memset(s->cells, 0, cols * rows * sizeof(cell_t));

    s->cursor_x = 0;
    s->cursor_y = 0;
    s->cursor_visible = true;
    s->cur_attr.fg = COLOR_WHITE;
    s->cur_attr.bg = COLOR_BLACK;
    s->full_repaint = true;

    s->saved_cursor_x = 0;
    s->saved_cursor_y = 0;
    s->alt_cells = NULL;

    // Scrollback inherits from whatever is currently in the globals.
    // During boot the scrollback is empty, so this saves NULL/empty.
    s->text_root = NULL;
    s->text_tail = NULL;
    s->text_scroll = 0;

    s->fg_queue_read_ptr = 0;
    s->fg_queue_write_ptr = 0;
    memset(s->fg_key_queue, 0, FG_QUEUE_SIZE);
    s->foreground_proc = NULL;
    memset(s->typingbuf, 0, 256);
    stbsp_snprintf(s->cwd, sizeof(s->cwd), "//A/");
    s->owns_framebuffer = false;

    return 0;
}

static void session_save_globals(term_session_t *s) {
    s->text_root             = screen_text_root;
    s->text_tail             = screen_text_tail;
    s->text_scroll           = screen_text_scroll;

    s->fg_queue_read_ptr     = fg_queue_read_ptr;
    s->fg_queue_write_ptr    = fg_queue_write_ptr;
    mem_copy((u8*)s->fg_key_queue, (u8*)fg_key_queue, FG_QUEUE_SIZE);

    s->foreground_proc       = foreground_proc;

    mem_copy((u8*)s->typingbuf, (u8*)typingbuf, 256);
    mem_copy((u8*)s->cwd, (u8*)cwd, 256);

    // owns_framebuffer is a global declared in kern_tests.h
    // Since we can't include that here, we just save what we know.
    // The active session tracks its own owns_framebuffer flag.
}

static void session_restore_globals(term_session_t *s) {
    screen_text_root         = s->text_root;
    screen_text_tail         = s->text_tail;
    screen_text_scroll       = s->text_scroll;

    fg_queue_read_ptr        = s->fg_queue_read_ptr;
    fg_queue_write_ptr       = s->fg_queue_write_ptr;
    mem_copy((u8*)fg_key_queue, (u8*)s->fg_key_queue, FG_QUEUE_SIZE);

    foreground_proc          = s->foreground_proc;

    mem_copy((u8*)typingbuf, (u8*)s->typingbuf, 256);
    mem_copy((u8*)cwd, (u8*)s->cwd, 256);
}

void session_switch(u32 id) {
    if (id >= MAX_SESSIONS) return;
    if (id == active_session->id) return;  // already on this session

    u64 irq = save_irq_and_disable();

    // 1. Save current globals to the active session
    session_save_globals(active_session);

    // 2. Switch active session pointer
    active_session = &sessions[id];

    // 3. Restore globals from the target session
    session_restore_globals(active_session);

    // 4. Sync owns_framebuffer global from the active session's flag
    //    (owns_framebuffer is extern'd in kern_tests.h; we handle it via
    //     a separate mechanism — see the note in main.c)

    restore_irq(irq);

    // 5. If switching to compositor session, trigger full repaint in WM.
    //    If switching to a text console session, redraw cell buffer to hardware.
    if (compositor_is_active()) {
        compositor_push_event(3, 0, 0, 0); // CHILD_DIRTY (full repaint)
    } else {
        term_render();
    }
}

term_session_t *session_by_id(u32 id) {
    if (id >= MAX_SESSIONS) return NULL;
    return &sessions[id];
}

// ============================================================================
// Term Init: initialize all sessions and activate session 0
// ============================================================================

void term_init(u16 cols, u16 rows) {
    // Session 0 always uses the scrollback that was populated during boot
    // (screen_lines_init was already called). We take ownership of those
    // scrollback pointers into session 0.
    for (u32 i = 0; i < MAX_SESSIONS; i++) {
        session_init(i, cols, rows);
    }

    // Session 0 inherits the boot scrollback
    sessions[0].text_root = screen_text_root;
    sessions[0].text_tail = screen_text_tail;
    sessions[0].text_scroll = screen_text_scroll;

    active_session = &sessions[0];
}

// ============================================================================
// Re-entrant guard for screen_push_buf → term_write → scroll → screen_push
// ============================================================================

// When screen_push_line writes to the cell buffer via term_write("\n"), and
// that triggers scroll_cell_buffer which calls screen_push_line again, the
// nested call must write ONLY to the linked-list scrollback — not back into
// the cell buffer — otherwise we get infinite recursion. This guard breaks
// the cycle.
static bool screen_cellbuf_line_active = false;

// ============================================================================
// Per-process session routing helper
// ============================================================================

// Resolve the terminal session that the CURRENT THREAD's output should be
// routed to.  Returns the process-associated session (if the process has
// one), or the globally active session as a fallback.
static term_session_t *term_target_session() {
    kern_process_t *proc = sched_get_current_process();
    if (proc && proc->terminal_session) {
        return (term_session_t *)proc->terminal_session;
    }
    return active_session;
}

// ============================================================================
// Old scrollback API (unchanged behavior, writes to global linked list)
// ============================================================================

u0 screen_lines_init(u32 row_len) {
    screen_text_row_len = row_len;
    screen_text_root = kmalloc(sizeof(screen_text_row_t));
    screen_text_root->str = kmallocz(screen_text_row_len);
    screen_text_root->next = NULL;
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
    if (!buf || len <= 0 || !screen_text_root) return;

    u64 irq = save_irq_and_disable();

    const char *ptr = buf;
    const char *end = buf + len;

    while (ptr < end) {
        screen_text_row_t *new_row = kmalloc(sizeof(screen_text_row_t));
        if (!new_row) break;
        new_row->str = kmallocz(screen_text_row_len);
        if (!new_row->str) {
            kfree(new_row);
            break;
        }
        new_row->next = NULL;

        if (screen_text_tail) {
            screen_text_tail->next = new_row;
            screen_text_tail = new_row;
        } else {
            screen_text_root = new_row;
            screen_text_tail = new_row;
        }

        i32 i = 0;
        while (ptr < end && *ptr != '\n' && i < screen_text_row_len - 1) {
            new_row->str[i++] = *ptr++;
        }

        new_row->str[i] = 0;
        if (ptr < end && *ptr == '\n') {
            ptr++;
        }
    }

    // Prune old rows if exceeding limit
    i32 count = screen_get_line_count();
    if (count > TERMINAL_MAX_ROWS) {
        i32 to_remove = count - TERMINAL_MAX_ROWS;
        while (to_remove-- > 0 && screen_text_root != NULL) {
            screen_text_row_t *old = screen_text_root;
            screen_text_root = screen_text_root->next;
            if (old->str) kfree(old->str);
            kfree(old);
        }
        if (screen_text_root == NULL) screen_text_tail = NULL;
    }

    restore_irq(irq);
}

static kern_process_t *term_get_capture_proc(void) {
    kern_process_t *proc = sched_get_current_process();
    if (proc && proc->output_capture_buf && proc->output_capture_max > 0) return proc;
    if (proc && proc->stdout_parent_pid > 0) {
        kern_process_t *parent = sched_get_process_by_pid(proc->stdout_parent_pid);
        if (parent && parent->output_capture_buf && parent->output_capture_max > 0) return parent;
    }
    return NULL;
}

u0 screen_push_line(const char *str) {
    PROFILE_SCOPE("screen:push_line");
    if (!str) return;

    u64 irq = save_irq_and_disable();
    kern_process_t *cap_proc = term_get_capture_proc();
    if (cap_proc && cap_proc->output_capture_buf && cap_proc->output_capture_max > 0) {
        i32 slen = str_len(str);
        for (i32 i = 0; i < slen && cap_proc->output_capture_len + 2 < cap_proc->output_capture_max; i++) {
            cap_proc->output_capture_buf[cap_proc->output_capture_len++] = str[i];
        }
        if (cap_proc->output_capture_len + 1 < cap_proc->output_capture_max) {
            cap_proc->output_capture_buf[cap_proc->output_capture_len++] = '\n';
        }
        cap_proc->output_capture_buf[cap_proc->output_capture_len] = '\0';
        restore_irq(irq);
        return; // Isolated process output — do not leak into global kernel console!
    }
    restore_irq(irq);

    // Re-entrant guard: if we're already inside a cell-buffer write
    // (called from scroll_cell_buffer), skip straight to the scrollback.
    if (screen_cellbuf_line_active) {
        screen_push_buf(str, str_len(str));
        return;
    }

    // Resolve which session this thread's output belongs to.
    term_session_t *target = term_target_session();
    if (!target) target = active_session;
    if (!target) {
        screen_push_buf(str, str_len(str));
        return;
    }

    // Cell buffer path: write to the target's cell buffer WITH a trailing
    // newline so the cursor advances.  We temporarily swap active_session
    // so that term_write / ansi_putc / term_putc operate on the right
    // session's cell buffer.
    if (target->cells && !target->owns_framebuffer) {
        screen_cellbuf_line_active = true;

        term_session_t *saved_active = active_session;
        active_session = target;

        i32 len = str_len(str);
        if (len > 0) term_write(str, len);
        term_write("\n", 1);

        active_session = saved_active;
        screen_cellbuf_line_active = false;
        return;
    }

    // Scrollback-only path: swap the globals to target's scrollback for
    // the duration of the push, so the linked list nodes land in the
    // correct session.
    {
        screen_text_row_t *saved_root = screen_text_root;
        screen_text_row_t *saved_tail = screen_text_tail;
        screen_text_root = target->text_root;
        screen_text_tail = target->text_tail;

        screen_push_buf(str, str_len(str));

        target->text_root = screen_text_root;
        target->text_tail = screen_text_tail;
        screen_text_root = saved_root;
        screen_text_tail = saved_tail;
    }
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
    PROFILE_SCOPE("screen:push_linef");
    char local_buf[STB_SPRINTF_MIN + 1];

    va_list args;
    va_start(args, fmt);
    stbsp_vsprintfcb((char *(*)(const char *, void *, int)) screen_line_stb_callback, NULL, local_buf, fmt, args);
    va_end(args);
}

u0 screen_terminal_clear() {
    screen_text_row_t *row = screen_text_root;
    while (row != NULL) {
        screen_text_row_t *next = row->next;
        kfree(row->str);
        kfree(row);
        row = next;
    }
    screen_text_root = NULL;
    screen_text_tail = NULL;
    screen_lines_init(screen_text_row_len);
}

// ============================================================================
// Cell Buffer Management
// ============================================================================

// Move cursor down one row, scrolling the cell buffer if needed.
// The scrolled-off top row is pushed to the scrollback linked list.
static void term_newline(term_session_t *s) {
    s->cursor_y++;
    s->cursor_x = 0;
    if (s->cursor_y >= s->rows) {
        scroll_cell_buffer(s);
        s->cursor_y = s->rows - 1;
    }
}

// Scroll the cell buffer up by one row. The top row is serialised to
// the scrollback linked list, then rows 1..N are shifted up, and the
// bottom row is zeroed.
static void scroll_cell_buffer(term_session_t *s) {
    // Build a plain-text row from the top cell row
    char row_str[512];
    u16 len = 0;
    cell_t *top_row = &s->cells[0];
    for (u16 x = 0; x < s->cols && len < (u16)(sizeof(row_str) - 2); x++) {
        if (top_row[x].ch >= 32) {
            row_str[len++] = top_row[x].ch;
        } else if (top_row[x].ch == '\t') {
            row_str[len++] = ' ';
        }
    }
    row_str[len] = 0;

    // Save the globals temporarily so screen_push_line writes to
    // THIS session's scrollback (not the global list).
    screen_text_row_t *saved_root = screen_text_root;
    screen_text_row_t *saved_tail = screen_text_tail;
    screen_text_root = s->text_root;
    screen_text_tail = s->text_tail;

    if (len > 0) {
        screen_push_line(row_str);
    } else {
        screen_push_line("");  // push an empty line to maintain scrollback sync
    }

    s->text_root = screen_text_root;
    s->text_tail = screen_text_tail;
    screen_text_root = saved_root;
    screen_text_tail = saved_tail;

    // Shift cell rows up
    u32 row_bytes = s->cols * sizeof(cell_t);
    for (u16 y = 0; y < s->rows - 1; y++) {
        mem_copy((u8*)&s->cells[y * s->cols], (u8*)&s->cells[(y + 1) * s->cols], row_bytes);
    }
    // Clear bottom row
    memset(&s->cells[(s->rows - 1) * s->cols], 0, row_bytes);

    s->full_repaint = true;
}

// Write ONE character to the cell buffer at cursor, handling special chars.
// This is the lowest-level cell write — called from the ANSI parser's GROUND state.
void term_putc(u8 c) {
    term_session_t *s = term_target_session();
    if (!s || !s->cells) return;

    switch (c) {
        case '\n':
            term_newline(s);
            break;

        case '\r':
            s->cursor_x = 0;
            break;

        case '\b':
            if (s->cursor_x > 0) s->cursor_x--;
            break;

        case '\t': {
            u16 tab_stop = (s->cursor_x + 8) & ~7;
            s->cursor_x = min(tab_stop, s->cols - 1);
            if (s->cursor_x >= s->cols) {
                s->cursor_x = 0;
                term_newline(s);
            }
            break;
        }

        default:
            if (c >= 32) {
                cell_t *cell = &s->cells[s->cursor_y * s->cols + s->cursor_x];
                cell->ch = c;
                cell->fg = s->cur_attr.fg;
                cell->bg = s->cur_attr.bg;

                s->cursor_x++;
                if (s->cursor_x >= s->cols) {
                    s->cursor_x = 0;
                    term_newline(s);
                }
            }
            break;
    }
}

// ============================================================================
// ANSI Parser
// ============================================================================

// Write a buffer through the ANSI parser. Each byte in GROUND state gets
// dispatched to term_putc() which writes it into the cell buffer.
void term_write(const char *buf, i32 len) {
    PROFILE_SCOPE("term:write");
    if (!buf || len <= 0) return;

    u64 irq = save_irq_and_disable();
    kern_process_t *cap_proc = term_get_capture_proc();
    if (cap_proc && cap_proc->output_capture_buf && cap_proc->output_capture_max > 0) {
        for (i32 i = 0; i < len && cap_proc->output_capture_len + 1 < cap_proc->output_capture_max; i++) {
            cap_proc->output_capture_buf[cap_proc->output_capture_len++] = buf[i];
        }
        cap_proc->output_capture_buf[cap_proc->output_capture_len] = '\0';
        restore_irq(irq);
        return;
    }
    restore_irq(irq);

    term_session_t *s = term_target_session();
    if (!s) return;
    for (i32 i = 0; i < len; i++) {
        ansi_putc((u8)buf[i]);
    }
}

static void ansi_putc(u8 c) {
    term_session_t *s = term_target_session();
    if (!s) return;

    switch (ansi_parser.state) {
        case ANSI_GROUND:
            if (c == '\x1b') {
                ansi_parser.state = ANSI_ESC;
                ansi_parser.param_count = 0;
                ansi_parser.private_marker = false;
                memset(ansi_parser.params, 0, sizeof(ansi_parser.params));
            } else {
                term_putc(c);
            }
            break;

        case ANSI_ESC:
            if (c == '[') {
                ansi_parser.state = ANSI_CSI;
            } else if (c == ']') {
                ansi_parser.state = ANSI_OSC;
            } else if (c == '\\') {
                // ST (string terminator) — just go back to ground
                ansi_parser.state = ANSI_GROUND;
            } else if (c == 'c') {
                // RIS — full reset (skip for now)
                ansi_parser.state = ANSI_GROUND;
            } else {
                // Unknown ESC sequence, ignore it
                ansi_parser.state = ANSI_GROUND;
            }
            break;

        case ANSI_CSI:
            if (c >= '0' && c <= '9') {
                ansi_parser.params[ansi_parser.param_count] =
                    ansi_parser.params[ansi_parser.param_count] * 10 + (c - '0');
            } else if (c == ';') {
                if (ansi_parser.param_count < 15) ansi_parser.param_count++;
            } else if (c == '?') {
                ansi_parser.private_marker = true;
            } else {
                ansi_parser.final_byte = c;
                ansi_execute(&ansi_parser, s);
                ansi_parser.state = ANSI_GROUND;
                ansi_parser.param_count = 0;
                ansi_parser.private_marker = false;
                memset(ansi_parser.params, 0, sizeof(ansi_parser.params));
            }
            break;

        case ANSI_OSC:
            // OSC: ignore everything until BEL (\x07) or ST (\x1b\\)
            if (c == '\x07') {
                ansi_parser.state = ANSI_GROUND;
            } else if (c == '\x1b') {
                // Could be start of ST; next char needs to be '\\'
                ansi_parser.state = ANSI_ESC;
            }
            break;
    }
}

static void ansi_execute(ansi_parser_t *p, term_session_t *s) {
    u16 p0 = p->params[0];
    if (p0 == 0) p0 = 1;  // most CSI sequences default to 1
    u16 p1 = p->params[1];
    if (p1 == 0) p1 = 1;

    switch (p->final_byte) {
        case 'H':  // Cursor Position ESC[row;colH
        case 'f':  // same (HV position)
            s->cursor_y = clamp(p0 - 1, 0, s->rows - 1);
            s->cursor_x = clamp(p1 - 1, 0, s->cols - 1);
            break;

        case 'A':  // Cursor Up ESC[nA
            s->cursor_y = (s->cursor_y >= p0) ? (u16)(s->cursor_y - p0) : 0;
            break;

        case 'B':  // Cursor Down ESC[nB
            s->cursor_y = min((u16)(s->cursor_y + p0), (u16)(s->rows - 1));
            break;

        case 'C':  // Cursor Forward ESC[nC
            s->cursor_x = min((u16)(s->cursor_x + p0), (u16)(s->cols - 1));
            break;

        case 'D':  // Cursor Back ESC[nD
            s->cursor_x = (s->cursor_x >= p0) ? (u16)(s->cursor_x - p0) : 0;
            break;

        case 'J':  // Erase in Display
            if (p0 == 2) {
                // Clear entire screen
                memset(s->cells, 0, s->cols * s->rows * sizeof(cell_t));
                s->full_repaint = true;
            } else if (p0 == 0) {
                // Clear from cursor to end of screen
                u32 start = s->cursor_y * s->cols + s->cursor_x;
                u32 count = s->cols * s->rows - start;
                memset(&s->cells[start], 0, count * sizeof(cell_t));
                s->full_repaint = true;
            } else if (p0 == 1) {
                // Clear from beginning of screen to cursor
                u32 end = s->cursor_y * s->cols + s->cursor_x;
                memset(s->cells, 0, (end + 1) * sizeof(cell_t));
                s->full_repaint = true;
            }
            break;

        case 'K':  // Erase in Line
            if (p0 == 2 || p0 == 0) {
                // Clear entire line or from cursor to end
                u16 start = (p0 == 0) ? s->cursor_x : 0;
                memset(&s->cells[s->cursor_y * s->cols + start], 0,
                       (s->cols - start) * sizeof(cell_t));
            } else if (p0 == 1) {
                // Clear from beginning of line to cursor
                memset(&s->cells[s->cursor_y * s->cols], 0,
                       (s->cursor_x + 1) * sizeof(cell_t));
            }
            break;

        case 'm':  // SGR — Select Graphic Rendition
            apply_sgr(s, p);
            break;

        case 's':  // Save cursor position (non-private)
            if (!p->private_marker) {
                s->saved_cursor_x = s->cursor_x;
                s->saved_cursor_y = s->cursor_y;
            }
            break;

        case 'u':  // Restore cursor position (non-private)
            if (!p->private_marker) {
                s->cursor_x = s->saved_cursor_x;
                s->cursor_y = s->saved_cursor_y;
            }
            break;

        case 'h':  // DECSET — enable DEC private mode
            if (p->private_marker) {
                if (p0 == 25) {
                    s->cursor_visible = false;
                } else if (p0 == 1049) {
                    // Enter alternate screen buffer
                    if (!s->alt_cells) {
                        s->alt_cells = kmalloc(s->cols * s->rows * sizeof(cell_t));
                    }
                    if (s->alt_cells) {
                        mem_copy((u8*)s->alt_cells, (u8*)s->cells,
                                 s->cols * s->rows * sizeof(cell_t));
                        memset(s->cells, 0, s->cols * s->rows * sizeof(cell_t));
                        s->cursor_x = 0;
                        s->cursor_y = 0;
                        s->full_repaint = true;
                    }
                }
            }
            break;

        case 'l':  // DECRST — disable DEC private mode
            if (p->private_marker) {
                if (p0 == 25) {
                    s->cursor_visible = true;
                } else if (p0 == 1049) {
                    // Exit alternate screen buffer
                    if (s->alt_cells) {
                        mem_copy((u8*)s->cells, (u8*)s->alt_cells,
                                 s->cols * s->rows * sizeof(cell_t));
                        s->cursor_x = s->saved_cursor_x;
                        s->cursor_y = s->saved_cursor_y;
                        s->full_repaint = true;
                    }
                }
            }
            break;

        default:
            break;
    }
}

// ============================================================================
// SGR Color Handling
// ============================================================================

static u32 ansi_std_color(u8 index) {
    // Standard 8 ANSI colors (index 0-7)
    switch (index) {
        case 0: return COLOR_BLACK;
        case 1: return COLOR_RED;
        case 2: return COLOR_GREEN;
        case 3: return 0x00B8860B;  // brown → dark yellow
        case 4: return COLOR_BLUE;
        case 5: return COLOR_MAGENTA;
        case 6: return COLOR_CYAN;
        case 7: return COLOR_SILVER;
        default: return COLOR_WHITE;
    }
}

static u32 ansi_bright_color(u8 index) {
    // Bright variants (index 0-7 maps to 90-97 / 100-107)
    switch (index) {
        case 0: return COLOR_DIM_GRAY;
        case 1: return 0x00FF6666;
        case 2: return 0x0066FF66;
        case 3: return COLOR_YELLOW;
        case 4: return 0x006666FF;
        case 5: return COLOR_PINK;
        case 6: return 0x0066FFFF;
        case 7: return COLOR_WHITE;
        default: return COLOR_WHITE;
    }
}

static void apply_sgr(term_session_t *s, ansi_parser_t *p) {
    for (u8 i = 0; i <= p->param_count; i++) {
        u16 param = p->params[i];

        switch (param) {
            case 0:  // Reset all attributes
                s->cur_attr.fg = COLOR_WHITE;
                s->cur_attr.bg = COLOR_BLACK;
                break;

            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                s->cur_attr.fg = ansi_std_color((u8)(param - 30));
                break;

            case 38:  // 256-color or truecolor foreground
                if (i + 2 <= p->param_count && p->params[i + 1] == 5) {
                    i += 2;
                    // 256-color palette — use a simple mapping
                    u8 color_idx = (u8)p->params[i];
                    if (color_idx < 16) {
                        // 16 standard colors
                        static const u32 palette16[16] = {
                            COLOR_BLACK, COLOR_RED, COLOR_GREEN, 0x00B8860B,
                            COLOR_BLUE, COLOR_MAGENTA, COLOR_CYAN, COLOR_SILVER,
                            COLOR_DIM_GRAY, 0x00FF6666, 0x0066FF66, COLOR_YELLOW,
                            0x006666FF, COLOR_PINK, 0x0066FFFF, COLOR_WHITE
                        };
                        s->cur_attr.fg = palette16[color_idx];
                    } else {
                        // For now, map to grayscale or just white
                        s->cur_attr.fg = COLOR_WHITE;
                    }
                } else if (i + 4 <= p->param_count && p->params[i + 1] == 2) {
                    // Truecolor: ESC[38;2;R;G;Bm
                    i += 4;
                    u8 r = (u8)p->params[i - 2];
                    u8 g = (u8)p->params[i - 1];
                    u8 b = (u8)p->params[i];
                    s->cur_attr.fg = (0xFF << 24) | (r << 16) | (g << 8) | b;
                }
                break;

            case 39:  // Default foreground
                s->cur_attr.fg = COLOR_WHITE;
                break;

            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                s->cur_attr.bg = ansi_std_color((u8)(param - 40));
                break;

            case 48:  // 256-color or truecolor background
                if (i + 2 <= p->param_count && p->params[i + 1] == 5) {
                    i += 2;
                    s->cur_attr.bg = COLOR_BLACK;  // simplified
                } else if (i + 4 <= p->param_count && p->params[i + 1] == 2) {
                    i += 4;
                    s->cur_attr.bg = COLOR_BLACK;
                }
                break;

            case 49:  // Default background
                s->cur_attr.bg = COLOR_BLACK;
                break;

            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                s->cur_attr.fg = ansi_bright_color((u8)(param - 90));
                break;

            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                s->cur_attr.bg = ansi_bright_color((u8)(param - 100));
                break;
        }
    }
}

// ============================================================================
// Rendering
// ============================================================================

// Draw the header/status bar at the top of the screen.
// This is always drawn fresh — never part of the cell buffer.
static void draw_header_bar() {
    display_t *disp = screen_current_display();
    if (!disp) return;

    v2i_t p = V2I(0, 0);
    char buf[64];

    // OS name
    p.x = 1 + screen_puts_r(" sandfleaOS ", p, COLOR_WHITE, COLOR_BLACK).x;

    // RAM usage
    extern u64 usable_ram;
    u64 free_ram = pmm_get_free_count() * PAGE_SIZE;
    u64 used_ram = usable_ram - free_ram;

    stbsp_snprintf(buf, sizeof(buf), " %4lld MiB ", used_ram / 1024 / 1024);
    p.x = 1 + screen_puts_r(buf, p, COLOR_GREEN, COLOR_BLACK).x;

    stbsp_snprintf(buf, sizeof(buf), " %4lld MiB ", usable_ram / 1024 / 1024);
    p.x = 1 + screen_puts_r(buf, p, COLOR_BLUE, COLOR_BLACK).x;

    // Display index
    stbsp_snprintf(buf, sizeof(buf), " Display %-2d ", disp->index);
    p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;

    // Active session name
    if (active_session) {
        stbsp_snprintf(buf, sizeof(buf), " %s ", active_session->name);
        p.x = 1 + screen_puts_r(buf, p, COLOR_CYAN, COLOR_BLACK).x;
    }

    // Heartbeat indicators
    extern i64 heartbeat1, heartbeat2, heartbeat3;
    stbsp_snprintf(buf, sizeof(buf), " %s ", (heartbeat1 % 2) ? "*" : " ");
    p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;
    stbsp_snprintf(buf, sizeof(buf), " %s ", (heartbeat2 % 2) ? "*" : " ");
    p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;
    stbsp_snprintf(buf, sizeof(buf), " %s ", (heartbeat3 % 2) ? "*" : " ");
    p.x = 1 + screen_puts_r(buf, p, COLOR_GRAY, COLOR_BLACK).x;

    // Separator line
    for (int i = 0; i <= font_height; i += 4) {
        screen_draw_line(V2I(p.x, i), V2I(disp->surface.width, i), COLOR_DIM_GRAY);
    }
}

// Render the active session's cell buffer to the framebuffer.
void term_render() {
    if (!active_session || !active_session->cells) {
        screen_render_shell();  // fallback to the old renderer
        return;
    }

    display_t *disp = screen_current_display();
    if (!disp) return;

    // Track whether we've cleared the real framebuffer for the current
    // doom session. Resets when term_render processes a non-doom session.
    static bool fb_cleared_for_doom = false;

    term_session_t *s = active_session;

    // If this session has a fullscreen app (owns_framebuffer), the app owns
    // the real framebuffer directly — don't touch it at all.
    // Clear the real framebuffer to black on first entry so the app
    // starts on a clean canvas (no leftover terminal text or header bar).
    if (s->owns_framebuffer) {
        if (!fb_cleared_for_doom) {
            fb_cleared_for_doom = true;
            if (disp && disp->trueAddress) {
                mem_set32((u32 *)disp->trueAddress, COLOR_BLACK,
                          disp->surface.pitch * disp->surface.height / 4);
            }
        }
        return;
    }
    fb_cleared_for_doom = false;

    screen_clear(COLOR_BLACK);

    // Render cell buffer rows
    for (u16 y = 0; y < s->rows; y++) {
        cell_t *row = &s->cells[y * s->cols];

        // Quick check: skip entirely empty rows past the cursor
        // (cursor_y > y + 5 heuristic: don't scan rows far below cursor)
        // Always scan up to cursor_y + 1 for safety.
        if (y > s->cursor_y + 5) {
            bool empty = true;
            for (u16 x = 0; x < s->cols; x++) {
                if (row[x].ch != 0) { empty = false; break; }
            }
            if (empty) continue;
        }

        ssfn_dst.x = 0;
        ssfn_dst.y = HEADER_HEIGHT + y * font_height;

        // Ensure we don't render past the bottom of the screen
        if (ssfn_dst.y + font_height > disp->surface.height) break;

        for (u16 x = 0; x < s->cols; x++) {
            cell_t c = row[x];

            if (c.ch == 0) {
                ssfn_dst.x += font_width;
                continue;
            }

            // Draw background first (fill the cell)
            screen_draw_box(V2I(ssfn_dst.x, ssfn_dst.y),
                           V2I(ssfn_dst.x + font_width, ssfn_dst.y + font_height), c.bg);

            // Draw the character
            ssfn_dst.fg = c.fg;
            ssfn_dst.bg = c.bg;
            ssfn_putc(c.ch);
            // NOTE: ssfn_putc already advances ssfn_dst.x by font_width.
        }
    }

    // Draw cursor
    if (s->cursor_visible) {
        u16 cx = s->cursor_x * font_width;
        u16 cy = HEADER_HEIGHT + s->cursor_y * font_height;
        if (cy + font_height <= disp->surface.height) {
            screen_draw_box(V2I(cx, cy), V2I(cx + font_width, cy + font_height), 0x40FFFFFF);
        }
    }

    // Draw header bar
    draw_header_bar();

    // Draw input prompt (only if no foreground process)
    if (s->foreground_proc == NULL) {
        ssfn_dst.x = 0;
        ssfn_dst.y = disp->surface.height - font_height;
        ssfn_puts("#>");
        // Use global typingbuf — it's the live copy; s->typingbuf is only
        // synced on session_switch()
        ssfn_puts(typingbuf);
    }

    screen_draw();
}

// Legacy render — calls term_render() and is kept so the old callers
// (wasm_spawn.c etc.) continue to work after the session refactor.
u0 screen_render_shell() {
    term_render();
}
