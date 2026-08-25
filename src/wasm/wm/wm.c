// wm.c — sandfleaOS compositing window manager (wasm32-wasi target)
//
// Imports linked by wasm_spawn via host function table:
//   display.claimCompositor, display.getResolution, display.claimBuffer,
//   display.present, input.pollEvents, proc.spawn, proc.signal

// ── WASM imports ──────────────────────────────────────────────────────────

__attribute__((import_module("display"), import_name("claimCompositor")))
extern int claimCompositor(void);

__attribute__((import_module("display"), import_name("getResolution")))
extern int getResolution(void);

__attribute__((import_module("display"), import_name("claimBuffer")))
extern int claimBuffer(void);

__attribute__((import_module("display"), import_name("present")))
extern int present(int offset);

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
static int   fb_offset;
static window_t windows[MAX_WINDOWS];
static int   window_count;
static int   focused_idx = -1;
static int   cursor_x, cursor_y;                    // mouse cursor position
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

// ── Drawing ───────────────────────────────────────────────────────────────

static void fill_rect(int x, int y, int w, int h, unsigned int color) {
    int x1 = x + w; if (x1 > screen_w) x1 = screen_w;
    int y1 = y + h; if (y1 > screen_h) y1 = screen_h;
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x1 <= x || y1 <= y) return;
    unsigned int *fb = (unsigned int*)WASM_U8(fb_offset);
    for (int py = y; py < y1; py++)
        for (int px = x; px < x1; px++)
            fb[py * screen_w + px] = color;
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
    unsigned int *fb = (unsigned int*)WASM_U8(fb_offset);
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
    if (focused_idx >= 0) {
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
    if (focused_idx > idx) focused_idx--;
    if (focused_idx >= window_count) focused_idx = window_count > 0 ? 0 : -1;
    if (focused_idx >= 0) {
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

    // Place path string in a static buffer (.bss, past code/data)
    static char path_buf[64];
    const char *name = "hello.wasm";
    int plen = 0;
    while (name[plen]) plen++;
    wm_memcpy(path_buf, name, plen);
    path_buf[plen] = 0;

    // Enqueue the spawn request — proc.spawn just stores it.
    // The actual spawn happens in dequeue_pending_spawns() below.
    int ok = proc_spawn((int)(unsigned long)path_buf, 0, 0);
    if (ok != 0) return;
}

// Call once per event loop iteration to drain spawn completions.
// proc.dequeueSpawn() returns the new PID, or -1 if nothing pending.
static void dequeue_pending_spawns(void) {
    for (;;) {
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
    }
}

// ── Composite ─────────────────────────────────────────────────────────────

static void composite_frame(void) {
    fill_rect(0, 0, screen_w, screen_h, 0xFF1A1A2E);

    // Draw focused last (on top)
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < window_count; i++) {
            if ((pass == 0) == (i == focused_idx)) continue;
            window_t *w = &windows[i];
            int cx = w->canvas_x + BORDER_W;
            int cy = w->canvas_y + TITLE_BAR_H;
            int cw = w->w - BORDER_W * 2;
            int ch = w->h - TITLE_BAR_H - BORDER_W;
            fill_rect(cx, cy, cw, ch, 0xFF222222);
            draw_title_bar(w);
            draw_window_borders(w);
        }
    }

    // Draw mouse cursor (4x4 white dot with black outline, hot at center)
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int px = cursor_x + dx;
            int py = cursor_y + dy;
            if (px < 0 || py < 0 || px >= screen_w || py >= screen_h) continue;
            unsigned int *fb = (unsigned int*)WASM_U8(fb_offset);
            unsigned int col = (dx == 0 && dy == 0) ? 0xFF000000 : 0xFFFFFFFF;
            fb[py * screen_w + px] = col;
        }
    }

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

    cursor_x = screen_w / 2;
    cursor_y = screen_h / 2;
    composite_frame();

    for (;;) {
        // Drain spawn completions first — this is the safe point
        // (minimal wasm3 recursion depth) for wasm_spawn calls.
        dequeue_pending_spawns();

        int n = pollEvents((int)(unsigned long)event_buf, EVENT_BUF_SZ);
        int redraw = 0;

        for (int i = 0; i < n; i++) {
            int type = event_buf[i * 4 + 0];
            int d0   = event_buf[i * 4 + 1];
            int d1   = event_buf[i * 4 + 2];

            if (type == 0) {  // KEY_DOWN
                char k = (char)d0;

                if (k == '\r' || k == '\n') {
                    enqueue_spawn();
                    redraw = 1;
                } else if (k == '\t') {
                    if (window_count > 0) {
                        focus_window((focused_idx + 1) % window_count);
                        redraw = 1;
                    }
                } else if (k == 'q' || k == 'Q') {
                    if (focused_idx >= 0) {
                        close_window(focused_idx);
                        redraw = 1;
                    }
                } else if (k == '\x1B') {
                    return;
                }
            }
            else if (type == 1) {  // MOUSE_MOVE
                cursor_x += (int)d0;  // sign-extended dx
                cursor_y += (int)d1;  // sign-extended dy
                if (cursor_x < 0) cursor_x = 0;
                if (cursor_y < 0) cursor_y = 0;
                if (cursor_x >= screen_w) cursor_x = screen_w - 1;
                if (cursor_y >= screen_h) cursor_y = screen_h - 1;
                redraw = 1;
            }
            else if (type == 2) {  // MOUSE_BTN
                int btn = (int)d0;  // 1=left, 2=right, 3=middle
                int down = (int)d1;
                if (btn == 1 && down) {
                    // Left click: hit-test title bars to focus
                    int hit = hit_test(cursor_x, cursor_y);
                    if (hit >= 0) {
                        focus_window(hit);
                        redraw = 1;
                    }
                }
            }
        }

        if (redraw) composite_frame();
    }
}