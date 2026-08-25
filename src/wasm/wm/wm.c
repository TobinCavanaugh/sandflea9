// wm.c — sandfleaOS compositing window manager (wasm32-wasi target)
//
// Imports linked by wasm_spawn via host function table:
//   display.claimCompositor, display.getResolution, display.claimBuffer,
//   display.present, display.presentRect, input.pollEvents, proc.spawn, proc.signal

// ── WASM imports ──────────────────────────────────────────────────────────

__attribute__((import_module("display"), import_name("claimCompositor")))
extern int claimCompositor(void);

__attribute__((import_module("display"), import_name("getResolution")))
extern int getResolution(void);

__attribute__((import_module("display"), import_name("claimBuffer")))
extern int claimBuffer(void);

__attribute__((import_module("display"), import_name("present")))
extern int present(int offset);

__attribute__((import_module("display"), import_name("presentRect")))
extern int presentRect(int offset, int x, int y, int w, int h);

__attribute__((import_module("display"), import_name("blitFromPid")))
extern int blitFromPid(int pid, int sx, int sy, int dx, int dy, int w, int h);

__attribute__((import_module("input"), import_name("pollEvents")))
extern int pollEvents(int buf, int max);

__attribute__((import_module("proc"), import_name("spawn")))
extern int proc_spawn(int path_ptr, int argc, int argv_ptr);

__attribute__((import_module("proc"), import_name("dequeueSpawn")))
extern int proc_dequeue_spawn(void);

__attribute__((import_module("proc"), import_name("signal")))
extern int proc_signal(int pid, int event, int data);

// ── Constants ─────────────────────────────────────────────────────────────

#define MAX_WINDOWS   32
#define TITLE_BAR_H   20
#define BORDER_W      2
#define EVENT_BUF_SZ  64

#define SIG_FOCUS_GAINED  1
#define SIG_FOCUS_LOST    2
#define SIG_CLOSE         3

// ── Window struct ─────────────────────────────────────────────────────────

typedef struct {
    int   pid;
    int   canvas_x, canvas_y;
    int   w, h;
    int   focused;
    char  title[64];
} window_t;

// ── Globals ───────────────────────────────────────────────────────────────

static int   screen_w, screen_h;
static int   fb_offset;                             // active frontbuffer with cursor
static int   clean_fb_offset;                       // clean backbuffer without cursor
static window_t windows[MAX_WINDOWS];
static int   window_count;
static int   focused_idx = -1;
static int   cursor_x, cursor_y;                    // mouse cursor position
static int   prev_cursor_x, prev_cursor_y;          // previous cursor position for dirty damage
static int   event_buf[EVENT_BUF_SZ * 4];

// ── Tiny utilities ────────────────────────────────────────────────────────

static void wm_memset(char *p, char v, int n) {
    for (int i = 0; i < n; i++) p[i] = v;
}

static void wm_memcpy(char *dst, const char *src, int n) {
    for (int i = 0; i < n; i++) dst[i] = src[i];
}

// WASM32 linear memory is flat from address 0.
#define WASM_U8(off)  ((unsigned char*)((unsigned long long)(off)))

// ── Drawing (to clean backbuffer) ─────────────────────────────────────────

static void fill_rect(int x, int y, int w, int h, unsigned int color) {
    int x1 = x + w; if (x1 > screen_w) x1 = screen_w;
    int y1 = y + h; if (y1 > screen_h) y1 = screen_h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x1 <= x || y1 <= y) return;
    unsigned int *fb = (unsigned int*)WASM_U8(clean_fb_offset);
    int span = x1 - x;
    for (int py = y; py < y1; py++) {
        unsigned int *row = &fb[py * screen_w + x];
        for (int px = 0; px < span; px++)
            row[px] = color;
    }
}

// Minimal 6x8 font — uppercase A-Z
static const unsigned char font6x8[26][8] = {
    {0x1C,0x22,0x22,0x3E,0x22,0x22,0x22,0x00},
    {0x3C,0x22,0x22,0x3C,0x22,0x22,0x3C,0x00},
    {0x1C,0x22,0x20,0x20,0x20,0x22,0x1C,0x00},
    {0x3C,0x22,0x22,0x22,0x22,0x22,0x3C,0x00},
    {0x3E,0x20,0x20,0x3C,0x20,0x20,0x3E,0x00},
    {0x3E,0x20,0x20,0x3C,0x20,0x20,0x20,0x00},
    {0x1E,0x20,0x20,0x26,0x22,0x22,0x1E,0x00},
    {0x22,0x22,0x22,0x3E,0x22,0x22,0x22,0x00},
    {0x1C,0x08,0x08,0x08,0x08,0x08,0x1C,0x00},
    {0x0E,0x04,0x04,0x04,0x24,0x24,0x18,0x00},
    {0x22,0x24,0x28,0x30,0x28,0x24,0x22,0x00},
    {0x20,0x20,0x20,0x20,0x20,0x20,0x3E,0x00},
    {0x22,0x36,0x2A,0x2A,0x22,0x22,0x22,0x00},
    {0x22,0x32,0x2A,0x26,0x22,0x22,0x22,0x00},
    {0x1C,0x22,0x22,0x22,0x22,0x22,0x1C,0x00},
    {0x3C,0x22,0x22,0x3C,0x20,0x20,0x20,0x00},
    {0x1C,0x22,0x22,0x22,0x2A,0x24,0x1A,0x00},
    {0x3C,0x22,0x22,0x3C,0x28,0x24,0x22,0x00},
    {0x1E,0x20,0x20,0x1C,0x02,0x02,0x3C,0x00},
    {0x3E,0x08,0x08,0x08,0x08,0x08,0x08,0x00},
    {0x22,0x22,0x22,0x22,0x22,0x22,0x1C,0x00},
    {0x22,0x22,0x22,0x22,0x22,0x14,0x08,0x00},
    {0x22,0x22,0x22,0x2A,0x2A,0x36,0x22,0x00},
    {0x22,0x22,0x14,0x08,0x14,0x22,0x22,0x00},
    {0x22,0x22,0x14,0x08,0x08,0x08,0x08,0x00},
    {0x3E,0x02,0x04,0x08,0x10,0x20,0x3E,0x00},
};

static void fb_write(int x, int y, unsigned int color) {
    if (x < 0 || y < 0 || x >= screen_w || y >= screen_h) return;
    unsigned int *fb = (unsigned int*)WASM_U8(clean_fb_offset);
    fb[y * screen_w + x] = color;
}

static void draw_glyph(int x, int y, char c, unsigned int fg, unsigned int bg) {
    int idx;
    if (c >= 'A' && c <= 'Z') idx = c - 'A';
    else if (c >= 'a' && c <= 'z') idx = c - 'a';
    else idx = -1;
    if (idx < 0 || idx >= 26) { fill_rect(x, y, 6, 8, bg); return; }
    for (int row = 0; row < 8; row++) {
        unsigned char bits = font6x8[idx][row];
        for (int col = 0; col < 6; col++)
            fb_write(x + col, y + row, (bits & (1 << (5 - col))) ? fg : bg);
    }
}

static void draw_text(int x, int y, const char *s, unsigned int fg, unsigned int bg) {
    while (*s) { draw_glyph(x, y, *s, fg, bg); x += 6; s++; }
}

// ── Window chrome ─────────────────────────────────────────────────────────

static void draw_title_bar(window_t *win) {
    int tx = win->canvas_x, ty = win->canvas_y;
    unsigned int bg = win->focused ? 0xFF4040CC : 0xFF555555;
    fill_rect(tx, ty, win->w, TITLE_BAR_H, bg);
    draw_text(tx + 4, ty + 6, win->title, 0xFFFFFFFF, bg);
    fill_rect(tx + win->w - 18, ty + 3, 14, 14, 0xFFFF2222);
}

static void draw_window_borders(window_t *win) {
    int bx = win->canvas_x;
    int by = win->canvas_y + TITLE_BAR_H;
    int bw = win->w, bh = win->h - TITLE_BAR_H;
    fill_rect(bx, by, BORDER_W, bh, 0xFF808080);
    fill_rect(bx + bw - BORDER_W, by, BORDER_W, bh, 0xFF808080);
    fill_rect(bx, by + bh - BORDER_W, bw, BORDER_W, 0xFF808080);
}

// ── Window management ─────────────────────────────────────────────────────

static void focus_window(int idx) {
    if (idx < 0 || idx >= window_count || focused_idx == idx) return;
    if (focused_idx >= 0 && focused_idx < window_count) {
        windows[focused_idx].focused = 0;
        proc_signal(windows[focused_idx].pid, SIG_FOCUS_LOST, 0);
    }
    focused_idx = idx;
    windows[idx].focused = 1;
    proc_signal(windows[idx].pid, SIG_FOCUS_GAINED, 0);
}

static void close_window(int idx) {
    if (idx < 0 || idx >= window_count) return;
    proc_signal(windows[idx].pid, SIG_CLOSE, 0);
    for (int i = idx; i + 1 < window_count; i++)
        windows[i] = windows[i + 1];
    window_count--;
    if (window_count == 0) {
        focused_idx = -1;
        return;
    }
    if (focused_idx > idx) focused_idx--;
    if (focused_idx >= window_count) focused_idx = window_count - 1;
    if (focused_idx >= 0 && focused_idx < window_count) {
        windows[focused_idx].focused = 1;
        proc_signal(windows[focused_idx].pid, SIG_FOCUS_GAINED, 0);
    }
}

// ── Tiling ────────────────────────────────────────────────────────────────

static void tile_all(void) {
    if (window_count == 0) return;
    int cols = 1;
    while (cols * cols < window_count) cols++;
    int rows = (window_count + cols - 1) / cols;
    int cell_w = screen_w / cols;
    int cell_h = screen_h / rows;
    for (int i = 0; i < window_count; i++) {
        windows[i].canvas_x = (i % cols) * cell_w;
        windows[i].canvas_y = (i / cols) * cell_h;
        windows[i].w = cell_w;
        windows[i].h = cell_h;
    }
}

// ── Spawn ─────────────────────────────────────────────────────────────────

static void enqueue_spawn(void) {
    if (window_count >= MAX_WINDOWS) return;

    static char path_buf[64];
    const char *name = "hello.wasm";
    int plen = 0;
    while (name[plen]) plen++;
    wm_memcpy(path_buf, name, plen);
    path_buf[plen] = 0;

    proc_spawn((int)(unsigned long)path_buf, 0, 0);
}

// Call once per event loop iteration to drain spawn completions.
// Returns number of newly spawned windows.
static int dequeue_pending_spawns(void) {
    int spawned = 0;
    while (window_count < MAX_WINDOWS) {
        int pid = proc_dequeue_spawn();
        if (pid < 0) break;

        window_t *win = &windows[window_count];
        win->pid = pid;
        win->canvas_x = 100 + window_count * 30;
        win->canvas_y = 100 + window_count * 30;
        win->w = 400;
        win->h = 300;
        win->focused = 0;
        wm_memcpy(win->title, "Terminal", 8);
        win->title[8] = 0;

        int idx = window_count;
        window_count++;
        focus_window(idx);
        tile_all();
        spawned++;
    }
    return spawned;
}

// ── Composite & Cursor ────────────────────────────────────────────────────

static void draw_cursor(int cx, int cy) {
    unsigned int *fb = (unsigned int*)WASM_U8(fb_offset);
    for (int dy = -2; dy <= 2; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= screen_h) continue;
        for (int dx = -2; dx <= 2; dx++) {
            int px = cx + dx;
            if (px < 0 || px >= screen_w) continue;
            unsigned int col = (dx == 0 && dy == 0) ? 0xFF000000 : 0xFFFFFFFF;
            fb[py * screen_w + px] = col;
        }
    }
}

// Restores clean pixels from clean_fb_offset back into active fb_offset
static void restore_cursor(int cx, int cy) {
    unsigned int *fb = (unsigned int*)WASM_U8(fb_offset);
    unsigned int *clean = (unsigned int*)WASM_U8(clean_fb_offset);
    for (int dy = -2; dy <= 2; dy++) {
        int py = cy + dy;
        if (py < 0 || py >= screen_h) continue;
        for (int dx = -2; dx <= 2; dx++) {
            int px = cx + dx;
            if (px < 0 || px >= screen_w) continue;
            int idx = py * screen_w + px;
            fb[idx] = clean[idx];
        }
    }
}

static void composite_frame(void) {
    fill_rect(0, 0, screen_w, screen_h, 0xFF1A1A2E);

    // Draw unfocused windows first into clean backbuffer
    for (int i = 0; i < window_count; i++) {
        if (i == focused_idx) continue;
        window_t *w = &windows[i];
        int cx = w->canvas_x + BORDER_W;
        int cy = w->canvas_y + TITLE_BAR_H;
        int cw = w->w - BORDER_W * 2;
        int ch = w->h - TITLE_BAR_H - BORDER_W;
        fill_rect(cx, cy, cw, ch, 0xFF222222);
        draw_title_bar(w);
        draw_window_borders(w);
    }

    // Draw focused window last (on top) into clean backbuffer
    if (focused_idx >= 0 && focused_idx < window_count) {
        window_t *w = &windows[focused_idx];
        int cx = w->canvas_x + BORDER_W;
        int cy = w->canvas_y + TITLE_BAR_H;
        int cw = w->w - BORDER_W * 2;
        int ch = w->h - TITLE_BAR_H - BORDER_W;
        fill_rect(cx, cy, cw, ch, 0xFF222222);
        draw_title_bar(w);
        draw_window_borders(w);
    }

    // Copy clean backbuffer to frontbuffer
    unsigned int *dst = (unsigned int*)WASM_U8(fb_offset);
    unsigned int *src = (unsigned int*)WASM_U8(clean_fb_offset);
    int total_pixels = screen_w * screen_h;
    for (int i = 0; i < total_pixels; i++) {
        dst[i] = src[i];
    }

    draw_cursor(cursor_x, cursor_y);
    present(fb_offset);
}

// ── Cursor ───────────────────────────────────────────────────────────────

// Hit test: returns window index under (x,y), or -1 for none.
static int hit_test(int mx, int my) {
    for (int i = window_count - 1; i >= 0; i--) {
        window_t *w = &windows[i];
        if (mx >= w->canvas_x && mx < w->canvas_x + w->w &&
            my >= w->canvas_y && my < w->canvas_y + w->h) {
            return i;
        }
    }
    return -1;
}

// ── Main ──────────────────────────────────────────────────────────────────

__attribute__((export_name("_start")))
void _start(void) {
    if (claimCompositor() != 0) return;

    int res = getResolution();
    screen_h = (res >> 16) & 0xFFFF;
    screen_w = res & 0xFFFF;

    fb_offset = claimBuffer();
    if (fb_offset < 0) return;

    clean_fb_offset = claimBuffer();
    if (clean_fb_offset < 0) return;

    cursor_x = screen_w / 2;
    cursor_y = screen_h / 2;
    prev_cursor_x = cursor_x;
    prev_cursor_y = cursor_y;
    composite_frame();

    for (;;) {
        int spawned = dequeue_pending_spawns();
        int full_redraw = (spawned > 0);
        int mouse_moved = 0;

        int n = pollEvents((int)(unsigned long)event_buf, EVENT_BUF_SZ);

        for (int i = 0; i < n; i++) {
            int type = event_buf[i * 4 + 0];
            int d0   = event_buf[i * 4 + 1];
            int d1   = event_buf[i * 4 + 2];

            if (type == 0) {  // KEY_DOWN
                char k = (char)d0;

                if (k == '\r' || k == '\n') {
                    if (window_count < MAX_WINDOWS) {
                        enqueue_spawn();
                        dequeue_pending_spawns();
                        full_redraw = 1;
                    }
                } else if (k == '\t') {
                    if (window_count > 0) {
                        focus_window((focused_idx + 1) % window_count);
                        full_redraw = 1;
                    }
                } else if (k == 'q' || k == 'Q') {
                    if (focused_idx >= 0) {
                        close_window(focused_idx);
                        tile_all();
                        full_redraw = 1;
                    }
                } else if (k == '\x1B') {
                    return;
                }
            }
            else if (type == 1) {  // MOUSE_MOVE
                cursor_x += (int)d0;
                cursor_y += (int)d1;
                if (cursor_x < 0) cursor_x = 0;
                if (cursor_y < 0) cursor_y = 0;
                if (cursor_x >= screen_w) cursor_x = screen_w - 1;
                if (cursor_y >= screen_h) cursor_y = screen_h - 1;
                mouse_moved = 1;
            }
            else if (type == 2) {  // MOUSE_BTN
                int btn = (int)d0;
                int down = (int)d1;
                if (btn == 1 && down) {
                    int hit = hit_test(cursor_x, cursor_y);
                    if (hit >= 0 && hit != focused_idx) {
                        focus_window(hit);
                        full_redraw = 1;
                    }
                }
            }
        }

        if (full_redraw) {
            composite_frame();
            prev_cursor_x = cursor_x;
            prev_cursor_y = cursor_y;
        } else if (mouse_moved && (cursor_x != prev_cursor_x || cursor_y != prev_cursor_y)) {
            // Restore exact clean pixels under previous cursor from clean backbuffer
            restore_cursor(prev_cursor_x, prev_cursor_y);
            // Draw cursor sprite onto active frontbuffer
            draw_cursor(cursor_x, cursor_y);

            // Compute bounding box covering both old and new cursor positions
            int min_x = (prev_cursor_x < cursor_x ? prev_cursor_x : cursor_x) - 2;
            int min_y = (prev_cursor_y < cursor_y ? prev_cursor_y : cursor_y) - 2;
            int max_x = (prev_cursor_x > cursor_x ? prev_cursor_x : cursor_x) + 3;
            int max_y = (prev_cursor_y > cursor_y ? prev_cursor_y : cursor_y) + 3;

            if (min_x < 0) min_x = 0;
            if (min_y < 0) min_y = 0;
            if (max_x > screen_w) max_x = screen_w;
            if (max_y > screen_h) max_y = screen_h;

            presentRect(fb_offset, min_x, min_y, max_x - min_x, max_y - min_y);
            prev_cursor_x = cursor_x;
            prev_cursor_y = cursor_y;
        }
    }
}