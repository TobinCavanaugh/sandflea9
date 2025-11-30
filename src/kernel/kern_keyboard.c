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
        case 0x1E: return 'a';
        case 0x30: return 'b';
        case 0x2E: return 'c';
        case 0x20: return 'd';
        case 0x12: return 'e';
        case 0x21: return 'f';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x17: return 'i';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';
        case 0x32: return 'm';
        case 0x31: return 'n';
        case 0x18: return 'o';
        case 0x19: return 'p';
        case 0x10: return 'q';
        case 0x13: return 'r';
        case 0x1F: return 's';
        case 0x14: return 't';
        case 0x16: return 'u';
        case 0x2F: return 'v';
        case 0x11: return 'w';
        case 0x2D: return 'x';
        case 0x15: return 'y';
        case 0x2C: return 'z';

        // Numbers
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';

        // Special keys
        case 0x1C: return '\n'; // Enter
        case 0x39: return ' '; // Space
        case 0x0E: return '\b'; // Backspace
        case 0x0F: return '\t'; // Tab
        case 0x01: return '\0'; // Escape (ASCII 27)

        // Symbols on number row
        case 0x29: return '`'; // Grave accent
        case 0x0C: return '-'; // Minus
        case 0x0D: return '='; // Equals

        // Symbols on letter rows
        case 0x1A: return '['; // Left bracket
        case 0x1B: return ']'; // Right bracket
        case 0x2B: return '\\'; // Backslash
        case 0x27: return ';'; // Semicolon
        case 0x28: return '\''; // Single quote
        case 0x33: return ','; // Comma
        case 0x34: return '.'; // Period
        case 0x35: return '/'; // Forward slash

        default: return 0; // Unmapped key
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


// Queue definitions
#define KEY_QUEUE_SIZE 128
u8 queue[KEY_QUEUE_SIZE] = {0};
u32 queue_read_ptr = 0;
u32 queue_write_ptr = 0;

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

u0 keyboard_handle_keypress(registers_t *t) {
    u8 status = inb(0x64);

    if (status & 0x01) {
        u8 sc = inb(0x60);

        // Key release
        if (sc & 0x80) {
            u8 released = sc & 0x7F;

            if ((released == 0x2A || released == 0x36)) {
                shift_down = 0;
            }
        }

        if (sc >= 0x80) {
            return;
        }

        if ((sc == 0x2A || sc == 0x36)) {
            shift_down = 1;
            return;
        }

        u8 ascii = get_ascii_from_scancode(sc);

        if (shift_down) {
            ascii = keyboard_shift(ascii);
        }

        // Add to queue if we have a valid character
        if (ascii != 0) {
            // Calculate next write position
            u32 next_write = (queue_write_ptr + 1) % KEY_QUEUE_SIZE;

            // Only write if queue is not full (next write != read)
            if (next_write != queue_read_ptr) {
                queue[queue_write_ptr] = ascii;
                queue_write_ptr = next_write;
            }
        }
    }
}
