//
// Created by tobin on 2025-11-24.
//
// Multi-channel serial I/O layer.
// Three logical channels map to three physical COM ports in QEMU:
//   PRIMARY (COM1, 0x3F8) — general kernel logging
//   TEST    (COM2, 0x2F8) — test status, unit test output, assert logs
//   PROFILE (COM3, 0x3E8) — structured profiling events (flame-graph compatible)
//

#ifndef KERN_SERIAL_H
#define KERN_SERIAL_H

#include "dialect.h"
#include "kern_asmstubs.h"

// Forward-declare to avoid circular include with kern_pci.h.
typedef struct pci_device pci_device_t;

// ── Physical COM port addresses ────────────────────────────────────────────
#define SERIAL_PORT_COM1  0x3F8
#define SERIAL_PORT_COM2  0x2F8
#define SERIAL_PORT_COM3  0x3E8

// Legacy alias kept for backward compatibility.
#define SERIAL_PORT       SERIAL_PORT_COM1

typedef enum : u8 {
    BASE_10      = 10,
    BASE_HEX     = 16,
    BASE_OCTAL   = 8,
    BASE_DECIMAL = 10,
    BASE_BINARY  = 2,
} BASE_FMT;

// ── Channel abstraction ─────────────────────────────────────────────────────

typedef enum : u8 {
    SERIAL_CH_PRIMARY = 0,
    SERIAL_CH_TEST    = 1,
    SERIAL_CH_PROFILE = 2,
    SERIAL_CH_COUNT   = 3
} serial_channel_t;

typedef struct {
    u16 port;
    u8  present;   // non-zero after successful init
} serial_device_t;

// ── Initialisation ──────────────────────────────────────────────────────────

u8 init_serial(void);            // legacy: init COM1 only
u0 serial_init_all(void);        // init COM1 + COM2 + COM3

// ── Channel-aware output (primary API) ──────────────────────────────────────

u0 serial_outc_ch (serial_channel_t ch, char c);
u0 serial_outs_ch (serial_channel_t ch, const char *str);
u0 serial_outsl_ch(serial_channel_t ch, const char *str);
u0 serial_outsf_ch(serial_channel_t ch, const char *fmt, ...);

// ── Legacy output (delegates to SERIAL_CH_PRIMARY) ──────────────────────────

u0 serial_outc (char c);
u0 serial_outs (const char *str);
u0 serial_outsl(const char *str);
u0 serial_outsf(const char *fmt, ...);

u0 serial_outi64(i64 val, BASE_FMT base);
u0 serial_outu64(u64 val, BASE_FMT base);

// ── Channel info ────────────────────────────────────────────────────────────

u8  serial_channel_present(serial_channel_t ch);
u16 serial_channel_port(serial_channel_t ch);

#endif //KERN_SERIAL_H
