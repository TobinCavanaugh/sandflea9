# Terminal Session Management & ANSI Escape Sequence Priority

**Status:** Design Writeup  
**Date:** July 2026  
**Context:** Follow-up to window management doc — deep dive on how terminal sessions should work and which ANSI sequences matter most.

---

## 1. Terminal Session Model

### 1.1 The Three Layers

Every terminal system has three layers. sandfleaOS currently has a skeleton of each, but they're tangled together in globals:

```
┌────────────────────────────────────────────┐
│  Layer 3: Session Management               │
│  - Which VT is active?                     │
│  - Which process is foreground?            │
│  - Keyboard routing                        │
│  Already partially exists:                 │
│    foreground_proc, doom_active            │
├────────────────────────────────────────────┤
│  Layer 2: Terminal Emulation               │
│  - Cell buffer (rows × cols of {char, fg,  │
│    bg, attrs})                             │
│  - Cursor position tracking                │
│  - ANSI escape sequence parsing            │
│  Does NOT exist — we have layer 1 only     │
├────────────────────────────────────────────┤
│  Layer 1: Rendering                        │
│  - SSFN font blitting                      │
│  - Framebuffer drawing                     │
│  - Scrollback management                   │
│  EXISTS: screen_push_line, ssfn_putc2,     │
│    screen_puts_c, screen_render_shell      │
└────────────────────────────────────────────┘
```

**The current problem:** `screen_push_line()` skips Layer 2 entirely. The linked-list-of-strings has no concept of cursor position, colors, or cell attributes. Every string is simply appended.

### 1.2 Session = (VT, Process, Cell Buffer)

A terminal session is the **union of three things**:

```c
typedef struct term_session {
    // Identity
    u32         id;              // 0 = kernel shell, 1+ = user sessions
    char        name[16];        // "tty0", "tty1", etc.
    
    // Layer 2: Cell buffer state
    u16         cols, rows;      // terminal dimensions (in characters)
    cell_t     *cells;           // cols × rows grid
    u16         cursor_x, cursor_y;
    bool        cursor_visible;
    
    // Cell attribute state (accumulated from escape sequences)
    cell_attr_t cur_attr;        // current fg, bg, bold, blink, etc.
    
    // Layer 1: Scrollback (linked list, same as today)
    screen_text_row_t *scrollback_head;
    screen_text_row_t *scrollback_tail;
    i32                scrollback_offset;
    
    // Input routing
    u8             fg_key_queue[FG_QUEUE_SIZE];
    volatile u32   fg_queue_read_ptr;
    volatile u32   fg_queue_write_ptr;
    kern_process_t *foreground_proc;
    
    // Dirty tracking
    u64            dirty_row_mask[DYNAMIC]; // bitmap of rows needing re-render
    bool           full_repaint;
} term_session_t;
```

Where `cell_t` is:

```c
typedef struct {
    u8   ch;            // ASCII/UTF-8 code point (single byte for now)
    u32  fg;            // foreground color
    u32  bg;            // background color
    bool bold : 1;      
    bool dim  : 1;
    bool underline : 1;
    bool blink : 1;
    bool inverse : 1;   // swap fg/bg
} cell_t;
```

The cell buffer is **16 bytes per cell × 80 cols × 24 rows = ~30 KB per session**. For 4 sessions, that's ~120 KB — trivial.

The scrollback stays as a linked list of strings (text only, no color), because scrollback data is massive and color fidelity in scrollback is not worth the memory.

### 1.3 Cell Buffer vs Linked List — Why Both?

| Aspect | Cell Buffer (Layer 2) | Scrollback (Layer 1) |
|--------|----------------------|----------------------|
| Size | Current view only (1920 cells for 80×24) | Up to 1000+ rows of text |
| Contains | Full color/attribute data | Plain text only |
| Mutable? | Yes (cursor moves, overwrites) | No (append only) |
| Used for | Rendering, cursor positioning | Scrolling up, history |
| Persists across VT switches? | Yes (per-session) | Yes (per-session) |

**Flow when output arrives:**

```
fd_write("Hello\n\033[31mRed\033[0m")
  │
  ▼
ANSI Parser (Layer 2)
  ├── "Hello" → write to cells at cursor, advance cursor
  ├── "\n"    → newline (scroll if needed, flush old row to scrollback)
  ├── "\033[31m" → set cur_attr.fg = COLOR_RED
  ├── "Red"  → write to cells at cursor with cur_attr
  └── "\033[0m" → reset cur_attr to defaults
       │
       ▼
  Mark dirty rows
       │
       ▼
Renderer (Layer 1) reads cell buffer, blits changed rows
```

### 1.4 The Scrollback Tension

**Problem:** The scrollback is plain text (no colors), but users expect scrollback to show colors.

**Solutions considered:**

1. **Text-only scrollback (simplest, ~0 overhead):** Sacrifice color in scrollback. ~90% of scrollback use cases don't care about color (you're reading, not debugging). **Recommended for V1.**

2. **Cell-row scrollback (accurate, memory heavy):** Store each scrollback row as a 1×cols cell array. ~80 KB per row at 1920 pixels wide (120 cols × 16 bytes). Would need < 50 rows to keep memory sane. Too expensive.

3. **HTML-inline style scrollback (weird, broken):** Store `"<span style='color:red'>text</span>"`. Nope.

4. **Annotated scrollback (most practical compromise):** Store `(char *text, u8 *style_runs)` where `style_runs` is a run-length encoding of color changes. E.g., "Hello\033[31mRed" → text="HelloRed", runs={7 default, 3 red}. ~2x memory overhead vs plain text, full color fidelity. **Worth doing in V2.**

### 1.5 Session Lifecycle

```
Boot ──→ Create tty0 (kernel shell, no process)
           │
           ├── User types a command
           │     │
           │     ├── "doom" → foreground_proc = doom PID
           │     │            doom owns tty0's keyboard
           │     │
           │     ├── Ctrl+C  → foreground_proc = NULL (back to shell)
           │     │
           │     └── command exits → foreground_proc = NULL (back to shell)
           │
           ├── User presses F2
           │     │
           │     ├── Save tty0 state (cells, scrollback, cursor, queue, fg_proc)
           │     │
           │     ├── Restore tty1 state
           │     │     ├── If tty1 has no cells → spawn login shell
           │     │     └── If tty1 has cells → show them as-is
           │     │
           │     └── Blit tty1's backbuffer
           │
           └── User presses Ctrl+Alt+F2
                 └── Same as F2, but more Linux-y
```

### 1.6 Session Initialization

When a VT has no session yet (first switch to it):

```c
term_session_t *session_create(u32 id) {
    term_session_t *s = kmalloc(sizeof(term_session_t));
    s->id = id;
    stbsp_snprintf(s->name, 16, "tty%d", id);
    s->cols = display_width / font_width;   // e.g. 120
    s->rows = display_height / font_height;  // e.g. 37 (accounting for status bar)
    s->cells = kmalloc(s->cols * s->rows * sizeof(cell_t));
    memset(s->cells, 0, s->cols * s->rows * sizeof(cell_t));
    s->cursor_x = s->cursor_y = 0;
    s->cursor_visible = true;
    s->cur_attr = (cell_attr_t){ .fg = COLOR_WHITE, .bg = COLOR_BLACK };
    s->scrollback_head = s->scrollback_tail = NULL;
    s->scrollback_offset = 0;
    s->foreground_proc = NULL;
    s->fg_queue_read_ptr = s->fg_queue_write_ptr = 0;
    s->full_repaint = true;
    session_list[id] = s;
    return s;
}
```

---

## 2. ANSI Escape Sequence Hierarchy

### 2.1 Core Principles

Not all ANSI sequences are created equal. This hierarchy is ordered by **how much they improve the developer experience** in sandfleaOS's specific environment. Sequences that enable interactive programs (nano/vim/htop-style) are ranked highest. Decorative sequences (colors, cursor shapes) are lower.

### 2.2 The Priority List

```
Priority  │ Sequence          │ What it does                           │ Who needs it
──────────┼───────────────────┼───────────────────────────────────────┼──────────────
   P0     │ \n                │ Newline (scroll)                      │ Everyone
          │ \r                │ Carriage return (cursor to col 0)     │ Everyone
          │ \b                │ Backspace                             │ Everyone
          │ \t                │ Tab                                   │ Everyone
          │                   │                                       │
   P1     │ ESC[2J            │ Clear entire screen                   │ Every CLI tool
          │ ESC[H             │ Cursor home (1,1)                     │ Every CLI tool
          │ ESC[row;colH      │ Cursor position (absolute)            │ ncurses, TUIs
          │                   │                                       │
   P2     │ ESC[?25l          │ Hide cursor                           │ vim, htop, text editors
          │ ESC[?25h          │ Show cursor                           │ vim, htop, text editors
          │ ESC[s             │ Save cursor position                  │ Shell prompts
          │ ESC[u             │ Restore cursor position               │ Shell prompts
          │ ESC[6n            │ Report cursor position (DSR)          │ Shell completion
          │ ESC[K             │ Clear line from cursor to end         │ Progress bars
          │                   │                                       │
   P3     │ ESC[1m            │ Bold                                  │ ls --color, git diff
          │ ESC[2m            │ Dim                                   │ Rare
          │ ESC[4m            │ Underline                             │ ls --color, git diff
          │ ESC[5m            │ Blink                                 │ Rare (cursors)
          │ ESC[7m            │ Inverse                               │ Selection, cursors
          │ ESC[0m            │ Reset all attributes                  │ Everywhere
          │ ESC[30-37m        │ Standard foreground colors            │ ls, git, grep
          │ ESC[40-47m        │ Standard background colors            │ ls, git, grep
          │ ESC[38;5;Nm       │ 256-color foreground                  │ Powerline, fancy prompts
          │ ESC[48;5;Nm       │ 256-color background                  │ Powerline, fancy prompts
          │ ESC[90-97m        │ Bright foreground colors              │ ls --color
          │ ESC[100-107m      │ Bright background colors              │ Rare
          │                   │                                       │
   P4     │ ESC[?1049h        │ Enter alternate screen buffer         │ vim, htop, less, nano
          │ ESC[?1049l        │ Exit alternate screen buffer          │ vim, htop, less, nano
          │ ESC[J             │ Clear from cursor to end of screen    │ less
          │ ESC[?7h           │ Enable line wrapping                  │ Everywhere (default)
          │ ESC[?7l           │ Disable line wrapping                 │ Rare
          │ ESC[?9h           │ Mouse reporting (X10)                 │ vim mouse, tmux
          │ ESC[?1000h        │ Mouse reporting (VT200)               │ vim mouse, tmux
          │                   │                                       │
   P5     │ ESC]0;title\007   │ Set window title                      │ Screen/tmux
          │ ESC[?1034h        │ Enable interpretation of meta key     │ Rare
          │ ESC[?1034l        │ Disable interpretation of meta key    │ Rare
          │ ESC[2h            │ Set KANJI mode (ignore)               │ Trash
          │                   │                                       │
   P6     │ ESC[?1002h        │ Mouse drag events                     │ Advanced TUI
          │ ESC[?1003h        │ Mouse any-move events                 │ Advanced TUI
          │ ESC[?1006h        │ SGR extended mouse                    │ Modern terminal apps
          │ ESC[?1005h        │ UTF-8 mouse mode                      │ Legacy
          │ ESC[?25           │ Cursor style (blinking/static)        │ Nice-to-have
```

### 2.3 Why This Order?

**P0 (already works):** `\n`, `\r`, `\b`, `\t` — these are already handled by `ssfn_putc2()` and `screen_push_buf()`. Nothing to do.

**P1 (essential for any TUI):** `ESC[2J` (clear screen) and `ESC[H` / `ESC[row;colH` (cursor positioning) are the **minimum viable set** for any non-trivial program. Without them:
- `ls` works (it's line-oriented)
- `cat` works
- `git diff` works (barely — wraps badly)
- `vim` doesn't work (needs exact cursor positioning)
- `htop` doesn't work
- Any `ncurses` program is DEAD

**P2 (essential for interactivity):** Cursor show/hide and save/restore are needed by anything that wants to do full-screen TUIs. Without hide:
- vim's cursor flickers (the hardware cursor shows during redraw)
- Shell prompt overwrites on resize

**P3 (visual polish):** Colors and text attributes. `ls --color`, `grep --color`, `git diff` all use these. No program *needs* them to function, but they make the terminal feel alive.

**P4 (advanced features):** Alternate screen buffer (`ESC[?1049h/l`) is what makes vim/less/htop seem to "restore" the terminal content when they exit. Without it, exiting vim leaves scrollback full of vim artifacts. Mouse reporting lets you click on things in vim.

**P5 (niche):** Window titles, meta key handling. Only needed by tmux and screen.

**P6 (deep mouse):** Drag events and SGR extended coordinates. Only `vimgdb` and a handful of TUIs use these. Don't bother until you have a mouse driver.

### 2.4 Implementation Strategy

**Phase 1 (this week, ~100 lines):** P0 + P1

```c
// Parser state machine
typedef enum {
    ANSI_GROUND,        // normal text
    ANSI_ESC,           // saw \x1b
    ANSI_CSI,           // saw \x1b[
    ANSI_CSI_PARAM,     // collecting params
} ansi_state_t;

typedef struct {
    ansi_state_t state;
    u16          params[16];  // max parameters
    u8           param_count;
    char         final_byte;  // the command letter (H, J, m, etc.)
} ansi_parser_t;

// Process one character through the state machine
void ansi_putc(ansi_parser_t *p, u8 c, term_session_t *sess) {
    switch (p->state) {
        case ANSI_GROUND:
            if (c == '\x1b') {
                p->state = ANSI_ESC;
                p->param_count = 0;
                memset(p->params, 0, sizeof(p->params));
            } else {
                term_putc(sess, c);  // write character to cell buffer
            }
            break;
            
        case ANSI_ESC:
            if (c == '[') {
                p->state = ANSI_CSI;
            } else if (c == ']') {
                p->state = ANSI_OSC;  // Operating System Command (ignore for now)
            } else {
                p->state = ANSI_GROUND;  // unknown ESC sequence, ignore
            }
            break;
            
        case ANSI_CSI:
            if (c >= '0' && c <= '9') {
                // Accumulate parameter digit
                p->params[p->param_count] = p->params[p->param_count] * 10 + (c - '0');
            } else if (c == ';') {
                p->param_count++;
                if (p->param_count >= 16) p->param_count = 15;
            } else {
                // Final byte
                ansi_execute(p, c, sess);
                p->state = ANSI_GROUND;
            }
            break;
    }
}

void ansi_execute(ansi_parser_t *p, u8 cmd, term_session_t *sess) {
    u16 p0 = p->params[0] ? p->params[0] : 1;  // default param is 1 for most
    u16 p1 = p->params[1] ? p->params[1] : 1;
    
    switch (cmd) {
        case 'H':  // Cursor Position
            sess->cursor_y = clamp(p0 - 1, 0, sess->rows - 1);
            sess->cursor_x = clamp(p1 - 1, 0, sess->cols - 1);
            break;
        case 'J':  // Erase in Display
            if (p0 == 2) {  // ESC[2J = clear entire screen
                memset(sess->cells, 0, sess->cols * sess->rows * sizeof(cell_t));
                sess->full_repaint = true;
            }
            break;
        case 'K':  // Erase in Line
            // Clear from cursor to end of line
            memset(&sess->cells[sess->cursor_y * sess->cols + sess->cursor_x], 0,
                   (sess->cols - sess->cursor_x) * sizeof(cell_t));
            break;
        default:
            // Ignore unknown sequences
            break;
    }
}
```

**Phase 2 (soon, ~80 lines):** P2 (cursor hide/show, save/restore)

```c
// In ansi_execute():
switch (cmd) {
    case 'h':  // DECSET — Enable mode
        if (p0 == 25) sess->cursor_visible = false;  // ESC[?25l
        break;
    case 'l':  // DECRST — Disable mode
        if (p0 == 25) sess->cursor_visible = true;   // ESC[?25h
        break;
}

// ESC[s and ESC[u use the 's' and 'u' final bytes directly:
if (cmd == 's' && p->state == ANSI_CSI) {
    sess->saved_cursor_x = sess->cursor_x;
    sess->saved_cursor_y = sess->cursor_y;
}
if (cmd == 'u' && p->state == ANSI_CSI) {
    sess->cursor_x = sess->saved_cursor_x;
    sess->cursor_y = sess->saved_cursor_y;
}
```

Note: `ESC[?25h` and `ESC[?25l` have `?` in them. In our parser, `?` would appear during CSI param collection as a character that's neither digit nor `;`. So we need to handle `?` specially:

```c
case ANSI_CSI:
    if (c >= '0' && c <= '9') {
        // accumulate
    } else if (c == ';') {
        p->param_count++;
    } else if (c == '?') {
        p->private_marker = true;  // DEC private mode marker
    } else {
        ansi_execute(p, c, sess, p->private_marker);
        p->state = ANSI_GROUND;
        p->private_marker = false;
    }
```

**Phase 3 (when colors are desired, ~50 lines):** P3 (SGR colors)

```c
// In ansi_execute():
if (cmd == 'm') {  // Select Graphic Rendition
    for (u8 i = 0; i <= p->param_count; i++) {
        u16 param = p->params[i];
        switch (param) {
            case 0:  // Reset
                sess->cur_attr = (cell_attr_t){ .fg = COLOR_WHITE, .bg = COLOR_BLACK };
                break;
            case 1:  sess->cur_attr.bold = true; break;
            case 2:  sess->cur_attr.dim = true; break;
            case 4:  sess->cur_attr.underline = true; break;
            case 5:  sess->cur_attr.blink = true; break;
            case 7:  sess->cur_attr.inverse = true; break;
            
            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                sess->cur_attr.fg = ansi_standard_color(param - 30);
                break;
            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                sess->cur_attr.bg = ansi_standard_color(param - 40);
                break;
            case 38:  // 256-color or truecolor foreground
                if (p->params[i+1] == 5) { i += 2; sess->cur_attr.fg = ansi_256_color(p->params[i]); }
                else if (p->params[i+1] == 2) { /* truecolor: skip R, G, B params */ i += 4; }
                break;
        }
    }
}
```

**Phase 4 (polish, ~40 lines):** P4 (alternate screen buffer)

```c
// Add to session struct:
cell_t        *alt_cells;         // alternate screen buffer (NULL if not in alt mode)
u16            alt_cursor_x, alt_cursor_y;
cell_attr_t    alt_attr;

// In ansi_execute(), private_marker=true:
if (cmd == 'h' && p0 == 1049) {
    // Enter alternate screen: save current cells, clear main buffer
    if (!sess->alt_cells) {
        sess->alt_cells = kmalloc(sess->cols * sess->rows * sizeof(cell_t));
    }
    memcpy(sess->alt_cells, sess->cells, sess->cols * sess->rows * sizeof(cell_t));
    memset(sess->cells, 0, sess->cols * sess->rows * sizeof(cell_t));
    sess->cursor_x = sess->cursor_y = 0;
    sess->full_repaint = true;
}
if (cmd == 'l' && p0 == 1049) {
    // Exit alternate screen: restore saved cells
    if (sess->alt_cells) {
        memcpy(sess->cells, sess->alt_cells, sess->cols * sess->rows * sizeof(cell_t));
        sess->cursor_x = sess->alt_cursor_x;
        sess->cursor_y = sess->alt_cursor_y;
        sess->full_repaint = true;
    }
}
```

### 2.5 Counting the Lines

```
Feature              │  Files       │ C LOC    
─────────────────────┼──────────────┼──────────
Cell buffer struct   │ kern_term.h  │   15     
ANSI parser state    │ kern_term.c  │   60     
P1: cursor pos + clr │ kern_term.c  │   30     
P2: cursor hide+save │ kern_term.c  │   15     
P3: SGR colors       │ kern_term.c  │   50     
P4: alt screen buf   │ kern_term.c  │   30     
Renderer: cells→fb   │ kern_term.c  │   60     
                     │              │
Total                │              │   ~260
```

That's ~260 lines to go from "line printer" to "proper terminal emulator with colors and cursor positioning." The cell buffer is the biggest single chunk, and it replaces the linked-list-of-strings for the visible area (the linked list stays for scrollback).

---

## 3. The Renderer

### 3.1 Cell-Based Rendering

The renderer reads the cell buffer and blits to the backbuffer:

```c
void term_render(term_session_t *sess) {
    if (sess->full_repaint) {
        screen_clear(COLOR_BLACK);
        for (u16 y = 0; y < sess->rows; y++) {
            render_row(sess, y);
        }
        sess->full_repaint = false;
    } else {
        // Dirty row rendering
        u64 *mask = sess->dirty_row_mask;
        for (u16 y = 0; y < sess->rows; y++) {
            if (mask[y / 64] & (1ULL << (y % 64))) {
                // Clear the row and re-render
                v2i_t pos = V2I(0, status_bar_height + y * font_height);
                screen_draw_box(pos, V2I(sess->cols * font_width, pos.y + font_height), COLOR_BLACK);
                render_row(sess, y);
                mask[y / 64] &= ~(1ULL << (y % 64));
            }
        }
    }
    
    // Render cursor
    if (sess->cursor_visible) {
        u16 cx = sess->cursor_x * font_width;
        u16 cy = status_bar_height + sess->cursor_y * font_height;
        screen_draw_box(V2I(cx, cy), V2I(cx + font_width, cy + font_height), 0x40FFFFFF);  // semi-transparent white
    }
}

void render_row(term_session_t *sess, u16 y) {
    cell_t *row = &sess->cells[y * sess->cols];
    ssfn_dst.x = 0;
    ssfn_dst.y = status_bar_height + y * font_height;
    
    for (u16 x = 0; x < sess->cols; x++) {
        cell_t c = row[x];
        if (c.ch == 0) break;  // empty rest of row
        
        u32 fg = c.inverse ? c.bg : c.fg;
        u32 bg = c.inverse ? c.fg : c.bg;
        
        // Draw background rect first
        screen_draw_box(V2I(ssfn_dst.x, ssfn_dst.y),
                       V2I(ssfn_dst.x + font_width, ssfn_dst.y + font_height), bg);
        
        // Draw character
        ssfn_dst.fg = fg;
        ssfn_dst.bg = bg;
        ssfn_putc(c.ch);
        
        ssfn_dst.x += font_width;
    }
}
```

### 3.2 The Status Bar

Keep the status bar from `screen_render_shell()` but render it as a **overlay**, not part of the cell buffer. This means:

```c
void term_render_status_bar(term_session_t *sess) {
    // Same header as today: OS name, RAM used/total, display index, etc.
    // Plus: current session name (tty0)
    v2i_t p = V2I(0, 0);
    char buf[64];
    
    stbsp_snprintf(buf, 64, " %s ", sess->name);
    p.x = 1 + screen_puts_r(buf, p, COLOR_WHITE, COLOR_BLACK).x;
    
    // ... rest of the header as before
}
```

The status bar is **not affected by cell buffer changes** — it's always rendered fresh. This means scrolling the terminal doesn't scroll the status bar, and programs can't draw over it.

---

## 4. Input Flow — Session to Process

### 4.1 Current Flow (Broken)

```
Keyboard IRQ → keyboard_handle_keypress()
                ├── Fills global queue
                └── Fills foreground queue (if foreground_proc)
       
Main loop → keyboard_eat_key()
             ├── NULL → process input directly (typingbuf)
             └── foreground_proc → keyboard_fg_push()
        
WASM program → fd_read(fd=0) → keyboard_fg_eat()
```

**Broken because:** There's only ONE foreground queue. If VT1's foreground process is Doom and VT2's foreground process is a shell, switching VTs doesn't save/restore the queues.

### 4.2 Fixed Flow (Per-Session Queues)

```
Keyboard IRQ → keyboard_handle_keypress()
                │
                ├── F1-F4 → vt_switch() (saves/restores all state)
                │
                └── Fills session[active_vt].fg_key_queue
                     │
                     ▼
                Main loop polls keyboard_eat_key()
                     │
                     ├── if session.foreground_proc == NULL:
                     │     └── process input into typingbuf (shell)
                     │
                     └── if session.foreground_proc != NULL:
                           └── Already in fg queue — WASM reads via fd_read
```

**Key change:** `keyboard_fg_push()` now takes a session parameter, or the global `fg_key_queue` is swapped on VT switch.

Simpler approach: **swap the globals on vt_switch()**:

```c
void vt_switch(u32 new_vt_id) {
    // 1. Save to current session struct
    session[active_vt].foreground_proc = foreground_proc;
    session[active_vt].fg_queue_read_ptr = fg_queue_read_ptr;
    session[active_vt].fg_queue_write_ptr = fg_queue_write_ptr;
    memcpy(session[active_vt].fg_key_queue, fg_key_queue, FG_QUEUE_SIZE);
    
    // 2. Restore from target session struct
    active_vt = new_vt_id;
    foreground_proc = session[active_vt].foreground_proc;
    fg_queue_read_ptr = session[active_vt].fg_queue_read_ptr;
    fg_queue_write_ptr = session[active_vt].fg_queue_write_ptr;
    memcpy(fg_key_queue, session[active_vt].fg_key_queue, FG_QUEUE_SIZE);
    
    // 3. Swap global terminal pointers too
    active_session = &session[active_vt];
}
```

This means `keyboard_fg_push()` and the WASM `fd_read` continue to use the same globals — they just see different values after the switch. **Minimal code change.**

---

## 5. Summary

### Session Management in One Paragraph

Each F-key gets its own `term_session_t` containing a cell buffer, scrollback, cursor state, foreground process reference, and keyboard queue. Switching VTs swaps all the globals that `screen_push_buf`, `keyboard_fg_push`, `foreground_proc`, etc., reference. The cell buffer (rows × cols) replaces the linked-list for the visible area but coexists with the linked-list scrollback (1000 rows of plain text history).

### ANSI Priority in One Paragraph

Implement in strict order: **P0** (\n\r\b\t — already done), **P1** (cursor positioning + clear screen — unlocks vim/nano/htop), **P2** (cursor hide/show, save/restore — eliminates flicker), **P3** (colors — makes `ls --color` work), **P4** (alternate screen buffer — keeps scrollback clean after vim exits). Skip P5-P6 until someone asks for them. Total implementation: ~260 lines of C across one new file and a few modifications to existing files.

### Quick Reference — What Enables What

```
Sequence                  Enables
───────────────────────── ────────────────────────────
ESC[H, ESC[row;colH      vim, nano, htop, less, man
ESC[2J                    clear, tput reset
ESC[?25h/l               vim (no flicker)
ESC[s, ESC[u              bash prompt (line editing)
ESC[30-37, 40-47          ls --color, git diff, grep
ESC[1,4,7                 bold, underline, inverse
ESC[?1049h/l              less/man/htop restore screen
ESC[?1000h                vim mouse click
```
