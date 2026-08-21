//
// Created by tobin on 2025-11-25.
//

#ifndef KERN_KEYBOARD_H
#define KERN_KEYBOARD_H

#include "dialect.h"
#include "kern_asmstubs.h"
#include "kern_interrupts.h"

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_PGUP 0x84
#define KEY_PGDN 0x85

// Function keys
#define KEY_F1  0x86
#define KEY_F2  0x87
#define KEY_F3  0x88
#define KEY_F4  0x89
#define KEY_F5  0x8A
#define KEY_F6  0x8B
#define KEY_F7  0x8C
#define KEY_F8  0x8D
#define KEY_F9  0x8E
#define KEY_F10 0x8F
#define KEY_F11 0x90
#define KEY_F12 0x91

// Ctrl+C (ASCII ETX) — the canonical "interrupt foreground process" keystroke.
// The keyboard handler emits this when Ctrl is held and 'c' is pressed.
#define KEY_CTRL_C 0x03

u8 keyboard_eat_key();
u0 keyboard_handle_keypress(registers_t *t);
i32 screen_get_line_count();

// Shared scancode injection point. Used by BOTH the i8042 PS/2 ISR and
// (eventually) the USB HID boot-protocol decoder. Phase 0 refactor
// consolidates the queue update + ascii encoding into one well-tested
// path; both transports feed into the same global queue.
//
// `is_extended`: true if the E0 prefix was seen for this event
//                (set-1 extended codes like arrow keys).
// `is_down`:     true if make (press), false if break (release).
// Caller passes the CLEAN set-1 scancode (low 7 bits) — break-bit
// stripping is the caller's job, since it knows the source format.
void kbd_inject_scancode_set1(u8 sc, bool is_extended, bool is_down);

u8 keyboard_peek_key();
u0 set_keyboard_leds(u8 state);
u0 toggle_capslock(void);
u0 debug_delay_ms(u32 approx_ms);
u0 debug_blink_code(u8 blinks);

// Raw scancode state for game input (doom etc)
extern bool scancode_pressed[128];
bool keyboard_scancode_is_pressed(u8 sc);
bool keyboard_scancode_consume_down(u8 sc);
bool keyboard_scancode_consume_up(u8 sc);
u0 keyboard_flush_queue();

#define FG_QUEUE_SIZE 256
extern u8 fg_key_queue[FG_QUEUE_SIZE];
extern volatile u32 fg_queue_read_ptr;
extern volatile u32 fg_queue_write_ptr;

u0 keyboard_fg_push(u8 key);
u8 keyboard_fg_eat();
u0 keyboard_fg_flush();

#endif //KERN_KEYBOARD_H
