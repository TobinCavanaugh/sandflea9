//
// Created by tobin on 2025-11-24.
// Refactored 2026-07-15: multi-channel serial (PRIMARY / TEST / PROFILE).
//

#include "../include/kern_serial.h"

#include "../util/util_str.h"
#include "../include/stbsupport.h"
#include "../include/stb_sprintf.h"

// ── Per-channel device state ────────────────────────────────────────────────

static serial_device_t serial_devs[SERIAL_CH_COUNT] = {
    { SERIAL_PORT_COM1, 0 },
    { SERIAL_PORT_COM2, 0 },
    { SERIAL_PORT_COM3, 0 },
};

// ── Low-level helpers ───────────────────────────────────────────────────────

static u8 serial_init_port(u16 port) {
    outb(port + 1, 0x00); // Disable all interrupts
    outb(port + 3, 0x80); // Enable DLAB (set baud rate divisor)
    outb(port + 0, 0x03); // Set divisor to 3 (lo byte) 38400 baud
    outb(port + 1, 0x00); //                  (hi byte)
    outb(port + 3, 0x03); // 8 bits, no parity, one stop bit
    outb(port + 2, 0xC7); // Enable FIFO, clear them, with 14-byte threshold
    outb(port + 4, 0x0B); // IRQs enabled, RTS/DSR set
    outb(port + 4, 0x1E); // Set in loopback mode, test the serial chip
    outb(port + 0, 0xAE); // Test serial chip (send byte 0xAE and check if serial returns same byte)

    // Check if serial is faulty (i.e: not same byte as sent)
    if (inb(port + 0) != 0xAE) {
        return 1;
    }

    // If serial is not faulty set it in normal operation mode
    outb(port + 4, 0x0F);
    return 0;
}

static u0 serial_port_outc(u16 port, char c) {
    // Wait for the transmit buffer to be empty (with timeout).
    for (u32 timeout = 0; timeout < 0xFFFFF; timeout++) {
        if (inb(port + 5) & 0x20) break;
        asm volatile("pause");
    }
    outb(port, c);
}

// ── Initialisation ──────────────────────────────────────────────────────────

u8 init_serial(void) {
    return serial_init_port(SERIAL_PORT_COM1);
}

u0 serial_init_all(void) {
    for (u8 i = 0; i < SERIAL_CH_COUNT; i++) {
        u8 ok = (serial_init_port(serial_devs[i].port) == 0);
        serial_devs[i].present = ok;
    }
}

// ── Channel info ────────────────────────────────────────────────────────────

u8 serial_channel_present(serial_channel_t ch) {
    if (ch >= SERIAL_CH_COUNT) return 0;
    return serial_devs[ch].present;
}

u16 serial_channel_port(serial_channel_t ch) {
    if (ch >= SERIAL_CH_COUNT) return 0;
    return serial_devs[ch].port;
}

// ── Channel-aware output ────────────────────────────────────────────────────

u0 serial_outc_ch(serial_channel_t ch, char c) {
    if (ch >= SERIAL_CH_COUNT || !serial_devs[ch].present) return;
    serial_port_outc(serial_devs[ch].port, c);
}

u0 serial_outs_ch(serial_channel_t ch, const char *str) {
    if (ch >= SERIAL_CH_COUNT || !serial_devs[ch].present) return;
    i32 i = 0;
    while (str[i] != '\0') {
        serial_port_outc(serial_devs[ch].port, str[i++]);
    }
}

u0 serial_outsl_ch(serial_channel_t ch, const char *str) {
    serial_outs_ch(ch, str);
    serial_outc_ch(ch, '\n');
}

u0 serial_outsf_ch(serial_channel_t ch, const char *fmt, ...) {
    if (ch >= SERIAL_CH_COUNT || !serial_devs[ch].present) return;

    // Format into a stack buffer, then blast it out.
    char buf[512];
    va_list args;
    va_start(args, fmt);
    stbsp_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    serial_outs_ch(ch, buf);
}

// ── Legacy output (PRIMARY channel) ─────────────────────────────────────────

u0 serial_outc(char c)           { serial_outc_ch (SERIAL_CH_PRIMARY, c); }
u0 serial_outs(const char *str)  { serial_outs_ch (SERIAL_CH_PRIMARY, str); }
u0 serial_outsl(const char *str) { serial_outsl_ch(SERIAL_CH_PRIMARY, str); }

u0 serial_outsf(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    stbsp_vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    serial_outs_ch(SERIAL_CH_PRIMARY, buf);
}

// ── Numeric helpers (PRIMARY only) ──────────────────────────────────────────

u0 serial_outi64(i64 val, BASE_FMT base) {
    char buf[64];
    i64_to_sn(val, buf, base, 64);
    serial_outs(buf);
}

u0 serial_outu64(u64 val, BASE_FMT base) {
    char buf[64];
    u64_to_sn(val, buf, base, 64);
    serial_outs(buf);
}

// ── PCI serial passthrough (unchanged) ──────────────────────────────────────

u0 serial_set_pci(pci_device_t *dev) {
    (void)dev;
}