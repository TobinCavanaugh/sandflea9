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

u8 keyboard_eat_key();
u0 keyboard_handle_keypress(registers_t *t);
i32 screen_get_line_count();

u8 keyboard_peek_key();
u0 toggle_capslock();
#endif //KERN_KEYBOARD_H
