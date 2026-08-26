// src/wasm/term/term.c
//
// Standalone C terminal emulator for sandfleaOS compositing window manager.
// Freestanding WASM module (no libc dependencies) compiled to term_stub.wasm.
//
// Maintains an in-memory 2D character/color grid and software-renders glyphs
// to the allocated framebuffer canvas with smooth line scrolling.

#define MAX_COLS 200
#define MAX_ROWS 128

#define FONT_W 8
#define FONT_H 8
#define MARGIN_X 8
#define MARGIN_Y 8

#define COLOR_BG      0xFF101018  // Dark Slate
#define COLOR_FG      0xFF55FF55  // Bright Green
#define COLOR_PROMPT  0xFF55FFFF  // Cyan
#define COLOR_CURSOR  0xFFFFFFFF  // White
#define COLOR_ERR     0xFFFF5555  // Red

// ── Host imports ──────────────────────────────────────────────────────────

__attribute__((import_module("display"), import_name("claimBuffer")))
extern int claimBuffer(void);

__attribute__((import_module("display"), import_name("getResolution")))
extern int getResolution(void);

__attribute__((import_module("display"), import_name("present")))
extern int present(int offset);

__attribute__((import_module("env"), import_name("ipc_signal_wait")))
extern int ipc_signal_wait(int mask);

__attribute__((import_module("env"), import_name("get_arg_i32")))
extern int get_arg_i32(int index);

__attribute__((import_module("env"), import_name("ipc_shm_attach")))
extern int ipc_shm_attach(int shm_id);

__attribute__((import_module("env"), import_name("ipc_shm_read_byte")))
extern int ipc_shm_read_byte(int handle, int offset);

__attribute__((import_module("env"), import_name("ipc_shm_write_byte")))
extern int ipc_shm_write_byte(int handle, int offset, int val);

__attribute__((import_module("env"), import_name("getcwd")))
extern int getcwd(int buf_offset, int buf_size);

__attribute__((import_module("env"), import_name("shell_exec")))
extern int shell_exec(int cmd_offset, int cmd_len, int out_offset, int max_out);

// ── Standard 8x8 font bitmap (ASCII 0x20..0x7E) ───────────────────────────

static const unsigned char font8x8[96][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // 0x20 space
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // 0x21 !
    {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00}, // 0x22 "
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // 0x23 #
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, // 0x24 $
    {0x00,0x66,0xA6,0xD8,0x1B,0x65,0x66,0x00}, // 0x25 %
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // 0x26 &
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // 0x27 '
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // 0x28 (
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // 0x29 )
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // 0x2A *
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // 0x2B +
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // 0x2C ,
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // 0x2D -
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // 0x2E .
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // 0x2F /
    {0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0x00}, // 0x30 0
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // 0x31 1
    {0x3C,0x66,0x06,0x0C,0x18,0x30,0x7E,0x00}, // 0x32 2
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, // 0x33 3
    {0x0C,0x1C,0x34,0x64,0x7E,0x04,0x0E,0x00}, // 0x34 4
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, // 0x35 5
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, // 0x36 6
    {0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x00}, // 0x37 7
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, // 0x38 8
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, // 0x39 9
    {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00}, // 0x3A :
    {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00}, // 0x3B ;
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // 0x3C <
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, // 0x3D =
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, // 0x3E >
    {0x3C,0x66,0x06,0x0C,0x18,0x00,0x18,0x00}, // 0x3F ?
    {0x3C,0x66,0x6E,0x6E,0x60,0x62,0x3C,0x00}, // 0x40 @
    {0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0x00}, // 0x41 A
    {0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0x00}, // 0x42 B
    {0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0x00}, // 0x43 C
    {0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0x00}, // 0x44 D
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0x00}, // 0x45 E
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, // 0x46 F
    {0x3C,0x66,0x60,0x6E,0x66,0x66,0x3A,0x00}, // 0x47 G
    {0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x00}, // 0x48 H
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 0x49 I
    {0x0E,0x06,0x06,0x06,0x06,0x66,0x3C,0x00}, // 0x4A J
    {0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x00}, // 0x4B K
    {0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00}, // 0x4C L
    {0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x00}, // 0x4D M
    {0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x00}, // 0x4E N
    {0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 0x4F O
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, // 0x50 P
    {0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0x00}, // 0x51 Q
    {0x7C,0x66,0x66,0x7C,0x6C,0x66,0x63,0x00}, // 0x52 R
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, // 0x53 S
    {0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00}, // 0x54 T
    {0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00}, // 0x55 U
    {0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00}, // 0x56 V
    {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // 0x57 W
    {0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0x00}, // 0x58 X
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x00}, // 0x59 Y
    {0x7E,0x06,0x0C,0x18,0x30,0x60,0x7E,0x00}, // 0x5A Z
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // 0x5B [
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // 0x5C backslash
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // 0x5D ]
    {0x18,0x3C,0x66,0x00,0x00,0x00,0x00,0x00}, // 0x5E ^
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // 0x5F _
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // 0x60 `
    {0x00,0x00,0x3C,0x06,0x3E,0x66,0x3E,0x00}, // 0x61 a
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0x00}, // 0x62 b
    {0x00,0x00,0x3C,0x66,0x60,0x66,0x3C,0x00}, // 0x63 c
    {0x06,0x06,0x3E,0x66,0x66,0x66,0x3E,0x00}, // 0x64 d
    {0x00,0x00,0x3C,0x66,0x7E,0x60,0x3C,0x00}, // 0x65 e
    {0x1C,0x30,0x30,0x7C,0x30,0x30,0x30,0x00}, // 0x66 f
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x3C}, // 0x67 g
    {0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0x00}, // 0x68 h
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 0x69 i
    {0x0C,0x00,0x0C,0x0C,0x0C,0x0C,0x6C,0x38}, // 0x6A j
    {0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0x00}, // 0x6B k
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 0x6C l
    {0x00,0x00,0x66,0x7F,0x7F,0x6B,0x63,0x00}, // 0x6D m
    {0x00,0x00,0x7C,0x66,0x66,0x66,0x66,0x00}, // 0x6E n
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3C,0x00}, // 0x6F o
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60}, // 0x70 p
    {0x00,0x00,0x3E,0x66,0x66,0x3E,0x06,0x07}, // 0x71 q
    {0x00,0x00,0x7C,0x66,0x60,0x60,0x60,0x00}, // 0x72 r
    {0x00,0x00,0x3E,0x60,0x3C,0x06,0x7C,0x00}, // 0x73 s
    {0x30,0x30,0x7C,0x30,0x30,0x34,0x18,0x00}, // 0x74 t
    {0x00,0x00,0x66,0x66,0x66,0x66,0x3E,0x00}, // 0x75 u
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x00}, // 0x76 v
    {0x00,0x00,0x63,0x6B,0x7F,0x3E,0x36,0x00}, // 0x77 w
    {0x00,0x00,0x66,0x3C,0x18,0x3C,0x66,0x00}, // 0x78 x
    {0x00,0x00,0x66,0x66,0x66,0x3E,0x06,0x3C}, // 0x79 y
    {0x00,0x00,0x7E,0x0C,0x18,0x30,0x7E,0x00}, // 0x7A z
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // 0x7B {
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // 0x7C |
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // 0x7D }
    {0x00,0x32,0x4C,0x00,0x00,0x00,0x00,0x00}, // 0x7E ~
    {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}, // 0x7F solid block
};

// ── State ─────────────────────────────────────────────────────────────────

static int fb_offset = -1;
static int screen_w = 0;
static int screen_h = 0;
static int max_cols = 80;
static int max_rows = 24;

static char grid[MAX_ROWS][MAX_COLS];
static unsigned int fg_grid[MAX_ROWS][MAX_COLS];
static unsigned int bg_grid[MAX_ROWS][MAX_COLS];
static unsigned char row_dirty[MAX_ROWS];

static int cursor_x = 0;
static int cursor_y = 0;
static int prev_cursor_x = 0;
static int prev_cursor_y = 0;

static char cmd_buf[512];
static int cmd_len = 0;

static char out_buf[8192];

// ── Grid manipulation ─────────────────────────────────────────────────────

static void grid_clear_row(int r) {
    if (r < 0 || r >= max_rows) return;
    for (int c = 0; c < max_cols; c++) {
        grid[r][c] = ' ';
        fg_grid[r][c] = COLOR_FG;
        bg_grid[r][c] = COLOR_BG;
    }
    row_dirty[r] = 1;
}

static void grid_init(void) {
    for (int r = 0; r < MAX_ROWS; r++) {
        for (int c = 0; c < MAX_COLS; c++) {
            grid[r][c] = ' ';
            fg_grid[r][c] = COLOR_FG;
            bg_grid[r][c] = COLOR_BG;
        }
        row_dirty[r] = 1;
    }
    cursor_x = 0;
    cursor_y = 0;
}

static void term_scroll(void) {
    for (int r = 0; r < max_rows - 1; r++) {
        for (int c = 0; c < max_cols; c++) {
            grid[r][c] = grid[r + 1][c];
            fg_grid[r][c] = fg_grid[r + 1][c];
            bg_grid[r][c] = bg_grid[r + 1][c];
        }
        row_dirty[r] = 1;
    }
    grid_clear_row(max_rows - 1);
    cursor_y = max_rows - 1;
}

static void term_putc(char ch, unsigned int fg, unsigned int bg) {
    if (ch == '\n') {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= max_rows) term_scroll();
        return;
    }
    if (ch == '\r') {
        cursor_x = 0;
        return;
    }
    if (ch == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            grid[cursor_y][cursor_x] = ' ';
            row_dirty[cursor_y] = 1;
        }
        return;
    }
    if (ch == '\t') {
        int next_tab = (cursor_x + 4) & ~3;
        while (cursor_x < next_tab && cursor_x < max_cols) {
            grid[cursor_y][cursor_x] = ' ';
            fg_grid[cursor_y][cursor_x] = fg;
            bg_grid[cursor_y][cursor_x] = bg;
            cursor_x++;
        }
        row_dirty[cursor_y] = 1;
        return;
    }

    if (ch >= 0x20 && ch <= 0x7E) {
        if (cursor_x >= max_cols) {
            cursor_x = 0;
            cursor_y++;
            if (cursor_y >= max_rows) term_scroll();
        }
        grid[cursor_y][cursor_x] = ch;
        fg_grid[cursor_y][cursor_x] = fg;
        bg_grid[cursor_y][cursor_x] = bg;
        row_dirty[cursor_y] = 1;
        cursor_x++;
    }
}

static void term_puts(const char *s, unsigned int fg, unsigned int bg) {
    while (*s) {
        term_putc(*s, fg, bg);
        s++;
    }
}

// ── Rendering ─────────────────────────────────────────────────────────────

static void render_glyph(unsigned int *fb, int stride_words, int px, int py, char ch, unsigned int fg, unsigned int bg) {
    int g_idx = (ch >= 0x20 && ch <= 0x7E) ? (ch - 0x20) : 0;
    const unsigned char *glyph = font8x8[g_idx];

    for (int r = 0; r < FONT_H; r++) {
        unsigned char bits = glyph[r];
        unsigned int *line = &fb[(py + r) * stride_words + px];
        for (int c = 0; c < FONT_W; c++) {
            line[c] = (bits & (0x80 >> c)) ? fg : bg;
        }
    }
}

static void term_render(void) {
    if (fb_offset < 0) return;

    unsigned int *fb = (unsigned int *)((unsigned long)fb_offset);
    int stride_words = screen_w;

    // Erase previous cursor position if cursor moved
    if (prev_cursor_y < max_rows && prev_cursor_x < max_cols) {
        char old_ch = grid[prev_cursor_y][prev_cursor_x];
        unsigned int old_fg = fg_grid[prev_cursor_y][prev_cursor_x];
        unsigned int old_bg = bg_grid[prev_cursor_y][prev_cursor_x];
        int px = MARGIN_X + prev_cursor_x * FONT_W;
        int py = MARGIN_Y + prev_cursor_y * FONT_H;
        render_glyph(fb, stride_words, px, py, old_ch, old_fg, old_bg);
    }

    // Render all dirty rows
    for (int r = 0; r < max_rows; r++) {
        if (!row_dirty[r]) continue;
        row_dirty[r] = 0;

        int py = MARGIN_Y + r * FONT_H;
        for (int c = 0; c < max_cols; c++) {
            int px = MARGIN_X + c * FONT_W;
            char ch = grid[r][c];
            unsigned int fg = fg_grid[r][c];
            unsigned int bg = bg_grid[r][c];
            render_glyph(fb, stride_words, px, py, ch, fg, bg);
        }
    }

    // Render active cursor block (inverting background)
    if (cursor_y < max_rows && cursor_x < max_cols) {
        int px = MARGIN_X + cursor_x * FONT_W;
        int py = MARGIN_Y + cursor_y * FONT_H;
        char ch = grid[cursor_y][cursor_x];
        render_glyph(fb, stride_words, px, py, ch, COLOR_BG, COLOR_CURSOR);
        prev_cursor_x = cursor_x;
        prev_cursor_y = cursor_y;
    }

    // Clear any extra framebuffer pixels below the active rows
    int clear_start_y = MARGIN_Y + max_rows * FONT_H;
    if (clear_start_y < screen_h) {
        for (int y = clear_start_y; y < screen_h; y++) {
            unsigned int *line = &fb[y * stride_words];
            for (int x = 0; x < screen_w; x++) {
                line[x] = COLOR_BG;
            }
        }
    }

    present(fb_offset);
}

static void print_prompt(void) {
    char cwd_buf[64];
    int len = getcwd((int)(unsigned long)cwd_buf, sizeof(cwd_buf) - 1);
    if (len > 0) {
        cwd_buf[len] = '\0';
        term_puts(cwd_buf, 0xFF88CCFF, COLOR_BG);
        term_puts(" > ", COLOR_PROMPT, COLOR_BG);
    } else {
        term_puts("> ", COLOR_PROMPT, COLOR_BG);
    }
}

// ── Command Execution ─────────────────────────────────────────────────────

static void exec_command(void) {
    cmd_buf[cmd_len] = '\0';
    term_putc('\n', COLOR_FG, COLOR_BG);
    term_render();

    if (cmd_len > 0) {
        int out_len = shell_exec((int)(unsigned long)cmd_buf, cmd_len,
                                 (int)(unsigned long)out_buf, sizeof(out_buf) - 1);
        if (out_len > 0) {
            out_buf[out_len] = '\0';
            for (int i = 0; i < out_len; i++) {
                term_putc(out_buf[i], COLOR_FG, COLOR_BG);
            }
        }
    }

    if (cursor_x > 0) {
        term_putc('\n', COLOR_FG, COLOR_BG);
    }

    cmd_len = 0;
    print_prompt();
    term_render();
}

static void cancel_command(void) {
    // Terminate any running child background processes
    static const char kill_cmd[] = "killchildren";
    shell_exec((int)(unsigned long)kill_cmd, 12, (int)(unsigned long)out_buf, sizeof(out_buf));

    term_puts("^C\n", COLOR_ERR, COLOR_BG);
    cmd_len = 0;
    print_prompt();
    term_render();
}

static void update_geometry(int ring_handle) {
    int prev_r = max_rows, prev_c = max_cols;

    if (ring_handle >= 0) {
        int w_low = ipc_shm_read_byte(ring_handle, 258);
        int w_high = ipc_shm_read_byte(ring_handle, 259);
        int h_low = ipc_shm_read_byte(ring_handle, 260);
        int h_high = ipc_shm_read_byte(ring_handle, 261);
        int cw = w_low | (w_high << 8);
        int ch = h_low | (h_high << 8);
        if (cw > 32 && ch > 32) {
            int new_cols = (cw - MARGIN_X * 2) / FONT_W;
            int new_rows = (ch - MARGIN_Y * 2) / FONT_H;
            if (new_cols > MAX_COLS) new_cols = MAX_COLS;
            if (new_rows > MAX_ROWS) new_rows = MAX_ROWS;
            if (new_cols < 10) new_cols = 10;
            if (new_rows < 4) new_rows = 4;
            max_cols = new_cols;
            max_rows = new_rows;
        }
    }

    int scrolled = 0;
    if (cursor_y >= max_rows) {
        int shift = cursor_y - (max_rows - 1);
        for (int r = 0; r < max_rows; r++) {
            int src_r = r + shift;
            if (src_r < MAX_ROWS) {
                for (int c = 0; c < MAX_COLS; c++) {
                    grid[r][c] = grid[src_r][c];
                    fg_grid[r][c] = fg_grid[src_r][c];
                    bg_grid[r][c] = bg_grid[src_r][c];
                }
            } else {
                for (int c = 0; c < MAX_COLS; c++) {
                    grid[r][c] = ' ';
                    fg_grid[r][c] = COLOR_FG;
                    bg_grid[r][c] = COLOR_BG;
                }
            }
            row_dirty[r] = 1;
        }
        cursor_y = max_rows - 1;
        scrolled = 1;
    }

    // Always clear everything below the active text cursor
    for (int r = cursor_y + 1; r < MAX_ROWS; r++) {
        for (int c = 0; c < MAX_COLS; c++) {
            grid[r][c] = ' ';
            fg_grid[r][c] = COLOR_FG;
            bg_grid[r][c] = COLOR_BG;
        }
        if (r < max_rows) {
            row_dirty[r] = 1;
        }
    }
    // Clear the remainder of the current line after cursor_x
    if (cursor_y < MAX_ROWS) {
        for (int c = cursor_x; c < MAX_COLS; c++) {
            grid[cursor_y][c] = ' ';
            fg_grid[cursor_y][c] = COLOR_FG;
            bg_grid[cursor_y][c] = COLOR_BG;
        }
        row_dirty[cursor_y] = 1;
    }

    if (scrolled || max_rows != prev_r || max_cols != prev_c) {
        for (int r = 0; r < max_rows; r++) {
            row_dirty[r] = 1;
        }
        term_render();
    }
}

// ── Main Entry ────────────────────────────────────────────────────────────

__attribute__((export_name("_start")))
void _start(void) {
    fb_offset = claimBuffer();
    if (fb_offset < 0) return;

    int res = getResolution();
    screen_h = (res >> 16) & 0xFFFF;
    screen_w = res & 0xFFFF;

    // Attach to WM input ring (argv[0] = shm_id)
    int shm_id = get_arg_i32(0);
    int ring_handle = -1;
    if (shm_id > 0) {
        ring_handle = ipc_shm_attach(shm_id);
    }

    update_geometry(ring_handle);

    // Fill initial canvas with dark background
    unsigned int *fb = (unsigned int *)((unsigned long)fb_offset);
    int total_pixels = screen_w * screen_h;
    for (int i = 0; i < total_pixels; i++) {
        fb[i] = COLOR_BG;
    }

    grid_init();
    print_prompt();
    term_render();

    // Main event loop
    for (;;) {
        int sig = ipc_signal_wait(0xFFFF);

        // Signal bit 4 = SIG_CLOSE (close window / exit)
        if (sig & 4) break;

        // Signal bit 8 = SIG_KEY (keystrokes available in input ring)
        if ((sig & 8) && ring_handle >= 0) {
            update_geometry(ring_handle);

            int head = ipc_shm_read_byte(ring_handle, 0);
            int tail = ipc_shm_read_byte(ring_handle, 1);

            int modified = 0;
            while (head != tail) {
                int ch = ipc_shm_read_byte(ring_handle, 2 + head);
                head = (head + 1) & 0xFF;

                if (ch == 0x03) {  // Ctrl+C
                    cancel_command();
                    modified = 0;
                } else if (ch == '\r' || ch == '\n') {
                    exec_command();
                    modified = 0;
                } else if (ch == '\b') {
                    if (cmd_len > 0) {
                        cmd_len--;
                        term_putc('\b', COLOR_FG, COLOR_BG);
                        modified = 1;
                    }
                } else if (ch >= 0x20 && ch <= 0x7E) {
                    if (cmd_len < (int)sizeof(cmd_buf) - 2) {
                        cmd_buf[cmd_len++] = (char)ch;
                        term_putc((char)ch, COLOR_FG, COLOR_BG);
                        modified = 1;
                    }
                }
            }

            ipc_shm_write_byte(ring_handle, 0, head);
            if (modified) {
                term_render();
            }
        }
    }
}
