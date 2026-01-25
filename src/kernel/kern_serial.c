//
// Created by tobin on 2025-11-24.
//

#include "../include/kern_serial.h"

#include "../util/util_str.h"
#include "../include/stbsupport.h"
#include "../include/stb_sprintf.h"

u8 init_serial() {
    outb(SERIAL_PORT + 1, 0x00); // Disable all interrupts
    outb(SERIAL_PORT + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(SERIAL_PORT + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(SERIAL_PORT + 1, 0x00); //                  (hi byte)
    outb(SERIAL_PORT + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(SERIAL_PORT + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(SERIAL_PORT + 4, 0x0B); // IRQs enabled, RTS/DSR set
    outb(SERIAL_PORT + 4, 0x1E); // Set in loopback mode, test the serial chip
    outb(SERIAL_PORT + 0, 0xAE); // Test serial chip (send byte 0xAE and check if serial returns same byte)

    // Check if serial is faulty (i.e: not same byte as sent)
    if (inb(SERIAL_PORT + 0) != 0xAE) {
        return 1;
    }

    // If serial is not faulty set it in normal operation mode
    // (not-loopback with IRQs enabled and OUT#1 and OUT#2 bits enabled)
    outb(SERIAL_PORT + 4, 0x0F);
    return 0;
}

u0 serial_set_pci(pci_device_t *dev) {
}

u0 serial_outc(char c) {
    //TODO Make it also output out of pci serial

    outb(SERIAL_PORT, c);
}

u0 serial_outs(const char *str) {
    i32 i = 0;
    while (str[i] != '\0') {
        serial_outc(str[i++]);
    }
}


u0 serial_outsl(const char *str) {
    serial_outs(str);
    serial_outc('\n');
}

u0 serial_outi64(i64 val, BASE_FMT base) {
    char buf[64];
    i64_to_sn(val, buf, base, 64);
}

u0 serial_outu64(u64 val, BASE_FMT base) {
    char buf[64];
    u64_to_sn(val, buf, base, 64);
    serial_outs(buf);
}

static char *serial_stb_callback(char *buf, void *user, i32 len) {
    for (int i = 0; i < len; i++) serial_outc(buf[i]);
    return buf;
}

u0 serial_outsf(const char *fmt, ...) {
    char local_buf[STB_SPRINTF_MIN];
    va_list args;

    va_start(args, fmt);
    stbsp_vsprintfcb((char *(*)(const char *, void *, int)) serial_stb_callback, null, local_buf, fmt, args);
    va_end(args);
}