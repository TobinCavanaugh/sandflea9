//
// Created by tobin on 2025-11-25.
//

#include "../include/kern_keyboard.h"

#include "../include/kern_mem.h"

u0 toggle_capslock() {
    static u8 led_state = 0;
    led_state = led_state ^ 0x07;

    // Timeout loop 1
    u32 timeout = 100000;
    while ((inb(0x64) & 2) != 0 && --timeout);
    if (timeout == 0) return; // Controller stuck, abort

    outb(0x60, 0xED);

    // Timeout loop 2
    timeout = 100000;
    while ((inb(0x64) & 2) != 0 && --timeout);
    if (timeout == 0) return;

    outb(0x60, led_state);
}

char get_ascii_from_scancode(u8 scancode) {
    // Make sure we're only handling key presses (not releases)
    if (scancode & 0x80) {
        return 0;
    }
    // Complete mapping of keys to ASCII characters
    switch (scancode) {
        // Letters
        case 0x1E:
            return 'a';
        case 0x30:
            return 'b';
        case 0x2E:
            return 'c';
        case 0x20:
            return 'd';
        case 0x12:
            return 'e';
        case 0x21:
            return 'f';
        case 0x22:
            return 'g';
        case 0x23:
            return 'h';
        case 0x17:
            return 'i';
        case 0x24:
            return 'j';
        case 0x25:
            return 'k';
        case 0x26:
            return 'l';
        case 0x32:
            return 'm';
        case 0x31:
            return 'n';
        case 0x18:
            return 'o';
        case 0x19:
            return 'p';
        case 0x10:
            return 'q';
        case 0x13:
            return 'r';
        case 0x1F:
            return 's';
        case 0x14:
            return 't';
        case 0x16:
            return 'u';
        case 0x2F:
            return 'v';
        case 0x11:
            return 'w';
        case 0x2D:
            return 'x';
        case 0x15:
            return 'y';
        case 0x2C:
            return 'z';

            // Numbers
        case 0x02:
            return '1';
        case 0x03:
            return '2';
        case 0x04:
            return '3';
        case 0x05:
            return '4';
        case 0x06:
            return '5';
        case 0x07:
            return '6';
        case 0x08:
            return '7';
        case 0x09:
            return '8';
        case 0x0A:
            return '9';
        case 0x0B:
            return '0';

            // Special keys
        case 0x1C:
            return '\n'; // Enter
        case 0x39:
            return ' '; // Space
        case 0x0E:
            return '\b'; // Backspace
        case 0x0F:
            return '\t'; // Tab
        case 0x01:
            return '\0'; // Escape (ASCII 27)

            // Symbols on number row
        case 0x29:
            return '`'; // Grave accent
        case 0x0C:
            return '-'; // Minus
        case 0x0D:
            return '='; // Equals

            // Symbols on letter rows
        case 0x1A:
            return '['; // Left bracket
        case 0x1B:
            return ']'; // Right bracket
        case 0x2B:
            return '\\'; // Backslash
        case 0x27:
            return ';'; // Semicolon
        case 0x28:
            return '\''; // Single quote
        case 0x33:
            return ','; // Comma
        case 0x34:
            return '.'; // Period
        case 0x35:
            return '/'; // Forward slash

            // Function keys (scancodes 0x3B-0x44)
        case 0x3B: return KEY_F1;
        case 0x3C: return KEY_F2;
        case 0x3D: return KEY_F3;
        case 0x3E: return KEY_F4;
        case 0x3F: return KEY_F5;
        case 0x40: return KEY_F6;
        case 0x41: return KEY_F7;
        case 0x42: return KEY_F8;
        case 0x43: return KEY_F9;
        case 0x44: return KEY_F10;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;

        default:
            return 0; // Unmapped key
    }
}

char keyboard_shift(char c) {
    // Letters can be handled with simple ASCII math
    if (c >= 'a' && c <= 'z') {
        return c - 32; // Convert lowercase to uppercase
    }

    // For other characters, use lookup tables
    const char *unshifted = "`1234567890-=[]\\;',./";
    const char *shifted = "~!@#$%^&*()_+{}|:\"<>?";

    // Search for the character in the unshifted string
    for (int i = 0; unshifted[i] != '\0'; i++) {
        if (c == unshifted[i]) {
            return shifted[i]; // Return the corresponding shifted character
        }
    }

    // If no shift equivalent found, return the same character
    return c;
}

u8 shift_down = false;
// Ctrl is derived from scancode_pressed[0x1D] at the encoding site so that
// holding left AND right Ctrl simultaneously (and releasing them in any
// order) doesn't desync the modifier state.
#define IS_CTRL_DOWN() (scancode_pressed[0x1D])

// ─── PS/2 (i8042) keyboard state ────────────────────────────────────────────
// Phase 0 of media/writings/usb_basic_implementation_plan.md:
// PS/2 keyboard is TEMPORARILY DISABLED while we bring up xHCI USB input.
// Flip this back to true (and un-comment the IRQ33 ISR registration in
// main.c) once the USB HID boot-protocol decoder is wired up.
static bool ps2_keyboard_enabled = true;  /* re-enabled so QEMU's PS/2 scancodes from QMP send-key land in the key queue */

// Raw scancode tracking for doom/etc
bool scancode_pressed[128] = {0};
bool scancode_edge_down[128] = {0};
bool scancode_edge_up[128] = {0};

bool keyboard_scancode_is_pressed(u8 sc) {
    if (sc >= 128) return false;
    return scancode_pressed[sc];
}

bool keyboard_scancode_consume_down(u8 sc) {
    if (sc >= 128) return false;
    bool v = scancode_edge_down[sc];
    scancode_edge_down[sc] = false;
    return v;
}

bool keyboard_scancode_consume_up(u8 sc) {
    if (sc >= 128) return false;
    bool v = scancode_edge_up[sc];
    scancode_edge_up[sc] = false;
    return v;
}


// Queue definitions
#define KEY_QUEUE_SIZE 128

u8 queue[KEY_QUEUE_SIZE] = {0};
u32 queue_read_ptr = 0;
u32 queue_write_ptr = 0;

u0 keyboard_flush_queue() {
    queue_read_ptr = queue_write_ptr;
    shift_down = 0;  // also release stuck shift state
    // ctrl tracking is in scancode_pressed[]; flush it too so a stuck
    // Ctrl doesn't survive across a key reset.
    for (int i = 0; i < 128; i++) scancode_pressed[i] = false;
}

u8 keyboard_peek_key() {
    if (queue_read_ptr == queue_write_ptr) {
        return 0;
    }

    return queue[queue_read_ptr];
}

u8 keyboard_eat_key() {
    // If read and write ptrs are same, buffer is empty
    if (queue_read_ptr == queue_write_ptr) {
        return 0;
    }

    u8 c = queue[queue_read_ptr];

    // Advance read pointer, wrapping around if necessary
    queue_read_ptr = (queue_read_ptr + 1) % KEY_QUEUE_SIZE;

    return c;
}

static u8 is_extended = 0;

u0 keyboard_handle_keypress(registers_t *t) {
    // Phase 0: PS/2 (i8042) keyboard path is temporarily disabled while
    // we exercise the new xHCI USB driver. Re-enable by setting
    // ps2_keyboard_enabled = true and un-commenting the IRQ33 ISR
    // registration in main.c.
    if (!ps2_keyboard_enabled) return;

    u8 status = inb(0x64);

    if (status & 0x01) {
        u8 sc = inb(0x60);

        // Extended prefix — leave the consumer to decide how to interpret
        if (sc == 0xE0) {
            is_extended = 1;
            return;
        }

        bool is_down = (sc & 0x80) == 0;
        u8 clean_sc = is_down ? sc : (sc & 0x7F);

        // Hand off to the shared injection path: scancode tracking, ascii
        // encoding, and queue push all happen in one place now.
        kbd_inject_scancode_set1(clean_sc, is_extended != 0, is_down);

        // CRITICAL: reset extended state so the next key isn't treated as extended
        is_extended = 0;
    }
}

// ─── Shared scancode injection ──────────────────────────────────────────────
// All keyboard sources (PS/2 i8042 today, USB HID boot-protocol tomorrow)
// feed set-1 make/break events through this function.
void kbd_inject_scancode_set1(u8 sc, bool is_extended, bool is_down) {
    if (sc >= 128) return;

    if (is_down) {
        // Track raw scancode state for doom & game input
        scancode_pressed[sc] = true;
        scancode_edge_down[sc] = true;

        // Track shift state from set-1 make codes (left=0x2A, right=0x36)
        if (!is_extended && (sc == 0x2A || sc == 0x36)) {
            shift_down = 1;
        }

        u8 ascii = 0;
        if (!is_extended) {
            ascii = get_ascii_from_scancode(sc);
            if (shift_down) ascii = keyboard_shift(ascii);
            // Ctrl+letter → 0x01..0x1A (Ctrl+A..Z). scancode_pressed[0x1D]
            // is already true here, so simultaneous left+right Ctrl is fine.
            if (IS_CTRL_DOWN() && ascii >= 'a' && ascii <= 'z') {
                ascii = ascii & 0x1F;
            }
        } else {
            switch (sc) {
                case 0x48: ascii = KEY_UP; break;
                case 0x50: ascii = KEY_DOWN; break;
                case 0x49: ascii = KEY_PGUP; break;
                case 0x51: ascii = KEY_PGDN; break;
                case 0x4B: ascii = KEY_LEFT; break;
                case 0x4D: ascii = KEY_RIGHT; break;
            }
        }

        if (ascii != 0) {
            u32 next_write = (queue_write_ptr + 1) % KEY_QUEUE_SIZE;
            if (next_write != queue_read_ptr) {
                queue[queue_write_ptr] = ascii;
                queue_write_ptr = next_write;
            }
        }
    } else {
        // Release path
        scancode_pressed[sc] = false;
        scancode_edge_up[sc] = true;

        // Reset shift on a non-extended left/right shift release
        if (!is_extended && (sc == 0x2A || sc == 0x36)) {
            shift_down = 0;
        }
        // (Ctrl is implied by scancode_pressed[0x1D]; release above does
        //  that bookkeeping automatically for both transports.)
    }
}

u8 fg_key_queue[FG_QUEUE_SIZE] = {0};
volatile u32 fg_queue_read_ptr = 0;
volatile u32 fg_queue_write_ptr = 0;

u0 keyboard_fg_push(u8 key) {
    u32 next_write = (fg_queue_write_ptr + 1) % FG_QUEUE_SIZE;
    if (next_write != fg_queue_read_ptr) {
        fg_key_queue[fg_queue_write_ptr] = key;
        fg_queue_write_ptr = next_write;
    }
}

u8 keyboard_fg_eat() {
    if (fg_queue_read_ptr == fg_queue_write_ptr) {
        return 0;
    }
    u8 c = fg_key_queue[fg_queue_read_ptr];
    fg_queue_read_ptr = (fg_queue_read_ptr + 1) % FG_QUEUE_SIZE;
    return c;
}

u0 keyboard_fg_flush() {
    fg_queue_read_ptr = fg_queue_write_ptr;
}
