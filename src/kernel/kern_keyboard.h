//
// Created by tobin on 2025-11-25.
//

#ifndef KERN_KEYBOARD_H
#define KERN_KEYBOARD_H

#include "../include/dialect.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_interrupts.h"

u8 keyboard_eat_key();
u0 keyboard_handle_keypress(registers_t *t);

#endif //KERN_KEYBOARD_H
