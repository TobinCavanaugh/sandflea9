#ifndef KERN_TERMINAL_H
#define KERN_TERMINAL_H

#include "dialect.h"
#include "kern_keyboard.h"

#define MAX_SESSIONS 4

// ============================================================================
// Cell & Cell Attributes — one character in the terminal grid
// ============================================================================

typedef struct {
    u32 fg;  // foreground color (ARGB)
    u32 bg;  // background color (ARGB)
} cell_attr_t;

typedef struct {
    u8  ch;  // character (ASCII for now)
    u32 fg;  // foreground color
    u32 bg;  // background color
} cell_t;

// ============================================================================
// Scrollback row (linked list — unchanged from old API)
// ============================================================================

typedef struct screen_text_row {
    char *str;
    struct screen_text_row *next;
} screen_text_row_t;

// Old scrollback globals — still used by screen_push_buf / screen_push_line.
// They get swapped on session switch so ALL existing callers keep working.
extern screen_text_row_t *screen_text_root;
extern screen_text_row_t *screen_text_tail;
extern i32 screen_text_scroll;
extern u32 screen_text_row_len;

// ============================================================================
// Terminal Session — bundles a cell buffer, cursor state, scrollback,
// keyboard queue, foreground process, and input buffer into one unit.
// Each F-key gets one session.
// ============================================================================

typedef struct {
    u32   id;                     // 0 = kernel shell, 1+ = user sessions
    char  name[16];               // "tty0", "tty1", etc.

    // Cell buffer (visible area)
    cell_t *cells;                // cols × rows grid
    u16    cols, rows;            // terminal dimensions in characters
    u16    cursor_x, cursor_y;    // current cursor position
    bool   cursor_visible;        // ESC[?25h / ESC[?25l
    cell_attr_t cur_attr;         // current fg/bg (from SGR sequences)
    bool   full_repaint;          // true = redraw every cell next frame

    // Saved cursor (ESC[s / ESC[u])
    u16    saved_cursor_x, saved_cursor_y;

    // Alternate screen buffer (ESC[?1049h)
    cell_t *alt_cells;

    // Scrollback (swapped when session switches)
    screen_text_row_t *text_root;
    screen_text_row_t *text_tail;
    i32               text_scroll;

    // Keyboard queue (per-session, swapped when session switches)
    u8      fg_key_queue[FG_QUEUE_SIZE];
    volatile u32 fg_queue_read_ptr;
    volatile u32 fg_queue_write_ptr;

    // Foreground process for this session
    void    *foreground_proc;

    // Shell input buffer (per-session, swapped when session switches)
    char    typingbuf[256];

    // Does this session's foreground app take over the framebuffer directly?
    bool    owns_framebuffer;
} term_session_t;

// Active session + session array
extern term_session_t *active_session;
extern term_session_t sessions[MAX_SESSIONS];

// ============================================================================
// ANSI Parser State Machine
// ============================================================================

typedef enum {
    ANSI_GROUND = 0,
    ANSI_ESC,
    ANSI_CSI,
    ANSI_OSC,
} ansi_state_t;

typedef struct {
    ansi_state_t state;
    u16          params[16];
    u8           param_count;
    char         final_byte;
    bool         private_marker;  // '?' prefix in CSI param (DEC private mode)
} ansi_parser_t;

// ============================================================================
// Old API — scrollback-only (unchanged, ~200 callers)
// ============================================================================

u0 screen_lines_init(u32 row_len);
i32 screen_get_line_count();
u0 screen_push_line(const char *str);
u0 screen_push_linef(const char *fmt, ...);
u0 screen_terminal_clear();
u0 screen_push_buf(const char *str, i32 len);

// ============================================================================
// New Terminal API
// ============================================================================

// Init: create MAX_SESSIONS sessions, activate session 0
u0 term_init(u16 cols, u16 rows);

// Write data through the ANSI parser into the active session's cell buffer
u0 term_write(const char *buf, i32 len);

// Session management
i32 session_init(u32 id, u16 cols, u16 rows);
u0  session_switch(u32 id);

// Render the active session's cell buffer to the framebuffer
u0 term_render();

// Legacy render (now just calls term_render)
u0 screen_render_shell();

#endif //KERN_TERMINAL_H
