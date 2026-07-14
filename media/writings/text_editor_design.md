# Text Editor for sandfleaOS

## Terminal Constraints

Our current terminal (`kern_terminal.c`) is a **scrollable line buffer**:
- Text is pushed as lines via `screen_push_line()`
- Lines scroll up when the buffer fills
- There is NO cursor positioning, NO escape sequence processing, NO raw mode
- The framebuffer CAN render text at arbitrary pixel positions (`screen_puts_c`, `ssfn_putc2`)
- Foreground WASM programs (like Doom) take over the screen entirely and draw directly

This means a traditional terminal-based editor (Vim, Nano) won't work without either:
- Implementing ANSI escape sequences in the kernel terminal driver
- Having the editor bypass the terminal and write to the framebuffer directly

**Recommended approach**: Two-phase rollout. Phase 1 works TODAY with zero kernel changes. Phase 2 adds framebuffer host functions for a real visual editor.

---

## Phase 1: Line-Based Editor (ed-like, ~200 lines WAT)

### Concept

A line-based editor modeled after the original Unix `ed`:
- Prints line numbers and content
- Commands are typed at a prompt
- No cursor movement, no full-screen redraw
- Works with the EXISTING terminal as-is

### How It Works

The WASM program:
1. Opens the file via `fd_open`, reads all lines into memory
2. Shows a prompt `*` to the user
3. Reads commands line-by-line from `fd_read(stdin)`
4. Processes commands (print, insert, delete, write, quit)
5. Writes the modified file back

### Command Set

```
* 1,$p           Print all lines
* 1,10p          Print lines 1-10
* 5              Print line 5 (like ed's default command)
* i              Enter insert mode before current line
  (lines of text)
  .              End insert mode
* a              Append after current line
* w test.txt     Write to test.txt
* q              Quit (fails if unsaved changes)
* wq             Write and quit
* 3d             Delete line 3
* s/old/new/     Substitute on current line
* h              Help (show commands)
```

### Implementation Sketch (WAT)

```wat
(module
  (import "env" "fd_open"       (func $open   (param i32) (result i32)))
  (import "env" "fd_read"       (func $read   (param i32 i32 i32) (result i32)))
  (import "env" "fd_write"      (func $write  (param i32 i32 i32 i32) (result i32)))
  (import "env" "fd_close"      (func $close  (param i32) (result i32)))
  (import "env" "get_arg_count" (func $argc   (result i32)))
  (import "env" "get_arg"       (func $getarg (param i32 i32 i32) (result i32)))

  ;; Memory: 2 pages (128KB)
  ;; Layout:
  ;;   0-8191:   Line pointer table (1024 entries × 8 bytes = 8KB)
  ;;   8192-131071: Line data (~120KB for file content)
  (memory 2)

  (func $read_file (param $path i32) (result i32)
    ;; Open, read entire file into memory, build line table
    ;; Return line count
  )

  (func $write_file (param $path i32) (result i32)
    ;; Write all lines from memory back to file
  )

  (func $print_lines (param $start i32) (param $end i32)
    ;; Print lines $start to $end (1-indexed)
  )

  (func $insert_mode (param $after i32)
    ;; Read lines until '.', insert after line $after
  )

  (func (export "_start")
    ;; 1. Get filename from argv
    ;; 2. Read file into memory
    ;; 3. Command loop: read input, parse, execute
    ;; 4. Write if modified, exit
  )
)
```

### What It Needs from the Kernel

**Nothing new.** It uses only existing imports:
- `fd_open`, `fd_read`, `fd_write`, `fd_close`
- `get_arg_count`, `get_arg`
- stdin (fd 0) for commands and insert text
- stdout (fd 1) for output

### Limitations

- You can't see the file while editing it (must type `1,$p` to see all lines)
- Not intuitive for anyone who doesn't know `ed`
- But it's **immediately useful** for editing config files and small source files

---

## Phase 2: Full-Screen Visual Editor

### New Host Functions Needed

To draw text at arbitrary positions on the framebuffer (like Doom does), the WASM program needs:

```wat
;; New host functions for screen drawing
(import "env" "draw_text"     (func $draw_text (param i32 i32 i32 i32 i32) (result i32)))
;;   params: x_pixels, y_pixels, text_offset, fg_color, bg_color
;;   Returns: 0 on success

(import "env" "clear_screen"  (func $clear_screen (param i32) (result i32)))
;;   param: color (0xAARRGGBB)

(import "env" "fill_rect"     (func $fill_rect (param i32 i32 i32 i32 i32) (result i32)))
;;   params: x, y, width, height, color

(import "env" "screen_size"   (func $screen_size (param i32 i32) (result i32)))
;;   params: width_offset, height_offset (output pointers in WASM memory)
;;   Returns char columns at width_offset, char rows at width_offset+4
```

These are tiny additions to the kernel (~10 lines each in `wasm_spawn.c`):

```c
m3ApiRawFunction(wasm_draw_text) {
    m3ApiGetArg(u32, x)
    m3ApiGetArg(u32, y)
    m3ApiGetArg(u32, text_offset)
    m3ApiGetArg(u32, fg)
    m3ApiGetArg(u32, bg)

    u32 mem_size = 0;
    u8 *mem = m3_GetMemory(runtime, &mem_size, 0);
    if (!mem || text_offset >= mem_size) m3ApiReturn(-1);

    display_t *disp = screen_current_display();
    if (!disp) m3ApiReturn(-1);

    ssfn_dst.x = x;
    ssfn_dst.y = y;
    ssfn_dst.fg = fg;
    ssfn_dst.bg = bg;

    const char *str = (const char *)(mem + text_offset);
    v2i_t end = screen_puts_c(str, V2I(x, y), fg, bg);
    (void)end;
    m3ApiReturn(0);
}
```

### Visual Editor Design

A minimal visual editor similar to **Kilo** (antirez's 1K-line editor) but using WASM framebuffer host functions instead of ANSI escape sequences.

```
┌─────────────────────────────────────────────────────┐
│ sandfleaOS Editor — test.txt                  Ln 12 │ ← Status bar
├─────────────────────────────────────────────────────┤
│ # Text Editor for sandfleaOS                         │
│                                                      │
│ ## Terminal Constraints                              │
│                                                      │
│ Our current terminal (`kern_terminal.c`) is a        │
│ scrollable line buffer.                              │
│                                                      │
│ This means a traditional terminal-based editor       │
│ (Vim, Nano) won't work without... █                  │ ← Cursor
│                                                      │
│ **Recommended approach**: Two-phase rollout.         │
│                                                      │
│ ## Phase 1: Line-Based Editor                        │
│                                                      │
│ A line-based editor modeled after the original       │
│ Unix `ed`.                                           │
├─────────────────────────────────────────────────────┤
│ CTRL+S: Save  CTRL+Q: Quit  CTRL+F: Find            │ ← Help bar
└─────────────────────────────────────────────────────┘
```

### Features

| Feature | Implementation | Complexity |
|---------|---------------|------------|
| Open file | `fd_open(path)` + `fd_read()` | Trivial |
| Save file | `fd_write(fd, lines...)` | Trivial |
| Display lines | `draw_text(0, y, line, WHITE, BLACK)` for each visible line | Trivial |
| Cursor movement | Track x,y in WASM memory, draw cursor char at position | Easy |
| Insert character | Shift line content right, insert byte | Easy |
| Delete character | Shift line content left, remove byte | Easy |
| Scroll (pgup/pgdn) | Track scroll offset, redraw visible lines | Easy |
| Line wrapping | Track column, wrap to next line when exceeding screen width | Medium |
| Status bar | Draw at fixed y = bottom of screen | Easy |
| Find text | Scan line data for substring, scroll to first match | Medium |
| Syntax highlighting | Colorize keywords/strings based on file extension | Medium |

### Implementation Plan (C, compiled to WASI)

A full-screen editor in C (~800-1000 lines) would be more maintainable than WAT:

```c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Screen dimensions (from host function)
int screen_cols, screen_rows;

// Editor state
typedef struct {
    char **lines;        // array of line strings
    int   num_lines;
    int   cursor_x;      // column within line
    int   cursor_y;      // row within file (line number)
    int   scroll_row;    // first visible line
    int   scroll_col;    // first visible column
    char *filename;
    int   dirty;         // unsaved changes?
} editor_t;

// Host function wrappers (these compile to WASM imports)
extern int draw_text(int x, int y, const char *text, int fg, int bg);
extern int clear_screen(int color);
extern int screen_size(int *width, int *height);

void editor_refresh_screen(editor_t *e) {
    clear_screen(COLOR_BLACK);
    
    // Draw status bar
    char status[256];
    snprintf(status, sizeof(status),
             " sandfleaOS Editor — %s  Ln %d/%d  %s",
             e->filename ? e->filename : "(new)",
             e->cursor_y + 1, e->num_lines,
             e->dirty ? "[Modified]" : "");
    draw_text(0, 0, status, COLOR_WHITE, COLOR_BLUE);
    
    // Draw file lines
    for (int i = 0; i < screen_rows - 2; i++) {
        int line_idx = e->scroll_row + i;
        if (line_idx >= e->num_lines) break;
        
        // Line number gutter
        char gutter[8];
        snprintf(gutter, sizeof(gutter), "%3d ", line_idx + 1);
        draw_text(0, (i + 1) * FONT_HEIGHT, gutter, COLOR_GRAY, COLOR_BLACK);
        
        // Line content (with offset for scroll_col)
        draw_text(GUTTER_WIDTH, (i + 1) * FONT_HEIGHT,
                  e->lines[line_idx] + e->scroll_col,
                  COLOR_WHITE, COLOR_BLACK);
    }
    
    // Draw cursor
    int cursor_px_x = GUTTER_WIDTH + (e->cursor_x - e->scroll_col) * CHAR_WIDTH;
    int cursor_px_y = (e->cursor_y - e->scroll_row + 1) * FONT_HEIGHT;
    draw_cursor(cursor_px_x, cursor_px_y, true);  // need cursor host function
    
    // Draw help bar
    draw_text(0, screen_rows * FONT_HEIGHT,
              " CTRL+S:Save  CTRL+Q:Quit  CTRL+F:Find",
              COLOR_DIM_GRAY, COLOR_BLACK);
}

void editor_process_keypress(editor_t *e, char c) {
    switch (c) {
        case CTRL_S: editor_save(e); break;
        case CTRL_Q: editor_quit(e); break;
        case CTRL_F: editor_find(e); break;
        case KEY_UP:    if (e->cursor_y > 0) e->cursor_y--; break;
        case KEY_DOWN:  if (e->cursor_y < e->num_lines - 1) e->cursor_y++; break;
        case KEY_LEFT:  if (e->cursor_x > 0) e->cursor_x--; break;
        case KEY_RIGHT: /* clamp to line length */ break;
        case '\n': /* insert newline, split line */ break;
        case '\b': /* delete char before cursor */ break;
        default:   /* insert char at cursor */ break;
    }
}
```

Compiled with wasi-sdk: `clang --target=wasm32-wasi editor.c -o editor.wasm`

---

## Comparison: Existing Editors vs Custom

| Editor | Language | LOC | WASI Compat? | Terminal Deps | Verdict |
|--------|----------|-----|-------------|---------------|---------|
| **ed** | C | ~2K | ✅ Yes | None (line-based) | ✅ Great for Phase 1 |
| **Kilo** | C | ~1K | ✅ Yes with porting | ANSI escapes (must replace) | ✅ Best Phase 2 candidate |
| **micro** | Go | ~20K | ❌ Go WASM ≠ WASI | ANSI escapes | ❌ Runtime mismatch |
| **Nano** | C | ~30K | ❌ Uses ncurses | Heavy terminal deps | ❌ Too complex |
| **Vim** | C | ~300K | ❌ Massive | Massive | ❌ Way overkill |
| **Custom ed** | WAT | ~200 | ✅ Already works | None | ✅ **Ship now** |
| **Custom visual** | C (WASI) | ~800 | ✅ After host funcs | Framebuffer host funcs | ✅ **The real goal** |

---

## Two-Phase Roadmap

### Phase 1: Line Editor (Week 1)

**Goal**: A usable text editor that works TODAY with zero kernel changes.

```
Files:
  src/wasm/wat/ed.wat          ← ~200 lines WAT
```

**Steps**:
1. Write `ed.wat` with commands: p, i, a, w, q, d, wq, h
2. Compile to `ed.wasm` via `wat2wasm`
3. Deploy to disk image
4. Use: `ed test.txt` to edit a file

**What it enables**: You can now edit `testfile.txt`, module source files, config files, etc. from the shell.

**Kernel changes**: **Zero.** Works with the existing host function set.

### Phase 2: Visual Editor (Week 2-3)

**Goal**: A full-screen visual editor with cursor movement, real-time display, and file editing.

**Kernel changes**: Add ~5 new host functions to `wasm_spawn.c`:
- `draw_text(x, y, text, fg, bg)`
- `clear_screen(color)`
- `fill_rect(x, y, w, h, color)`
- `screen_size(out_ptr)` — returns char columns/rows
- `draw_cursor(x, y, visible)` — toggle a cursor at pixel position

**Files**:
```
src/tools/editor/editor.c      ← ~800 lines C
src/wasm/wat/ed.wat            ← Phase 1 editor (still useful for scripting)
```

**Build**: `clang --target=wasm32-wasi editor.c -o editor.wasm`

**What it enables**: Visual editing of source files. Syntax highlighting can be added later.

### Phase 3: Polish (Future)

- Syntax highlighting for `.wat`, `.c`, `.asm`
- Search and replace
- Multiple buffers/tabs
- Cut/copy/paste within editor
- Integration with build system (editor command to compile and run)

---

## Summary

| Phase | Editor | Type | Kernel Changes | Time | How to Build |
|-------|--------|------|---------------|------|-------------|
| 1 | `ed.wat` | Line-based | **None** | 1 day | `wat2wasm ed.wat → ed.wasm` |
| 2 | `editor.wasm` | Full-screen visual | ~5 host functions | 1 week | `clang --target=wasm32-wasi` |

**Phase 1 ships today.** Phase 2 needs the WASI host functions (which we were going to add anyway for WABT) plus ~5 framebuffer drawing host functions.

Both are WASM programs — no ELF loading needed. Pure WASM-first.
