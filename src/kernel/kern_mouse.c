// kern_mouse.c — PS/2 mouse driver.
//
// Handles IRQ 12 (vector 44), decodes 3-byte movement packets,
// and pushes events into a ring buffer for the compositor / foreground app.

#include "../include/kern_mouse.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_mem.h"
#include "../include/kern_serial.h"
#include "../include/kern_sched.h"

// ── PS/2 helpers ──────────────────────────────────────────────────────────

static inline void ps2_wait_write(void) {
    for (u32 timeout = 0; timeout < 50000; timeout++) {
        if ((inb(0x64) & 2) == 0) return;
        asm volatile("pause");
    }
}

static inline void ps2_wait_read(void) {
    for (u32 timeout = 0; timeout < 50000; timeout++) {
        if ((inb(0x64) & 1) != 0) return;
        asm volatile("pause");
    }
}

// ── State ─────────────────────────────────────────────────────────────────

static mouse_event_t g_mouse_queue[MOUSE_EVENT_QUEUE_SIZE];
static volatile u32   g_mouse_read  = 0;
static volatile u32   g_mouse_write = 0;

// 3-byte packet assembly
static u8   g_mouse_cycle   = 0;   // 0=waiting for byte 1, 1=got byte1, 2=got byte2
static u8   g_mouse_bytes[3];
static u8   g_btn_state     = 0;   // bitmask of currently held buttons

// ── Init ──────────────────────────────────────────────────────────────────

void mouse_init(void) {
    serial_outsl("MOUSE: initializing PS/2 mouse...");

    // 1. Enable auxiliary PS/2 port (command 0xA8)
    ps2_wait_write();
    outb(0x64, 0xA8);

    // 2. Read current configuration byte, enable IRQ 12 + aux clock
    ps2_wait_write();
    outb(0x64, 0x20);
    ps2_wait_read();
    u8 cfg = inb(0x60);

    cfg |=  (1 << 1);   // bit 1: enable IRQ 12 (mouse interrupts)
    cfg &= ~(1 << 5);   // bit 5: disable mouse clock (0=enabled)

    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, cfg);

    // 3. Set sample rate to 80 samples/sec (for smooth cursor)
    // Command sequence: 0xF3, then rate byte
    static const u8 rate = 80;
    ps2_wait_write(); outb(0x64, 0xD4);  // next byte is for mouse
    ps2_wait_write(); outb(0x60, 0xF3);  // "set sample rate" command
    ps2_wait_write(); outb(0x64, 0xD4);
    ps2_wait_write(); outb(0x60, rate);

    // 4. Set resolution to 4 counts/mm (default)
    ps2_wait_write(); outb(0x64, 0xD4);
    ps2_wait_write(); outb(0x60, 0xE8);  // "set resolution" command
    ps2_wait_write(); outb(0x64, 0xD4);
    ps2_wait_write(); outb(0x60, 2);     // 4 counts/mm (2 << 1)

    // 5. Enable data reporting (command 0xF4)
    ps2_wait_write(); outb(0x64, 0xD4);
    ps2_wait_write(); outb(0x60, 0xF4);

    // Drain any stale bytes from the aux port that may have accumulated
    // during init (ACKs, etc).
    for (int i = 0; i < 16; i++) {
        if (inb(0x64) & 1) inb(0x60);
    }

    serial_outsl("MOUSE: initialized OK");
}

// ── ISR ───────────────────────────────────────────────────────────────────

void mouse_handle_interrupt(const registers_t *t) {
    (void) t;

    // Read status port
    u8 status = inb(0x64);

    // Bit 0 must be set (output buffer full)
    if (!(status & 0x01)) return;

    // Bit 5 must be set (mouse data, not keyboard)
    if (!(status & 0x20)) return;

    u8 data = inb(0x60);

    // Packet assembly
    switch (g_mouse_cycle) {
        case 0:
            // First byte must have bit 3 set (always 1)
            if (!(data & 0x08)) return;
            g_mouse_bytes[0] = data;
            g_mouse_cycle = 1;
            break;

        case 1:
            g_mouse_bytes[1] = data;
            g_mouse_cycle = 2;
            break;

        case 2: {
            g_mouse_bytes[2] = data;
            g_mouse_cycle = 0;

            // ── Decode packet ──────────────────────────────────────
            u8 flags = g_mouse_bytes[0];

            // Overflow bits: if set, discard the packet
            if (flags & 0xC0) return;

            i8 dx = (i8)g_mouse_bytes[1];
            i8 dy = (i8)g_mouse_bytes[2];

            // Y axis is inverted in PS/2 (positive = up)
            dy = -dy;

            // ── Button changes ─────────────────────────────────────
            u8 new_btns = flags & 0x07;  // bits 0,1,2 = left, right, middle
            u8 changed  = g_btn_state ^ new_btns;
            g_btn_state = new_btns;

            for (int btn = 0; btn < 3; btn++) {
                u8 mask = (1 << btn);
                if (changed & mask) {
                    u8 down = (new_btns & mask) ? 1 : 0;
                    u32 next = (g_mouse_write + 1) % MOUSE_EVENT_QUEUE_SIZE;
                    if (next != g_mouse_read) {
                        g_mouse_queue[g_mouse_write].type = MOUSE_EVENT_BTN;
                        g_mouse_queue[g_mouse_write].dx   = (i8)(btn + 1);  // 1=left, 2=right, 3=middle
                        g_mouse_queue[g_mouse_write].dy   = (i8)down;
                        g_mouse_write = next;
                    }
                    sched_idle_wake();
                }
            }

            // ── Movement ──────────────────────────────────────────
            if (dx != 0 || dy != 0) {
                u32 next = (g_mouse_write + 1) % MOUSE_EVENT_QUEUE_SIZE;
                if (next != g_mouse_read) {
                    g_mouse_queue[g_mouse_write].type = MOUSE_EVENT_MOVE;
                    g_mouse_queue[g_mouse_write].dx   = dx;
                    g_mouse_queue[g_mouse_write].dy   = dy;
                    g_mouse_write = next;
                }
                sched_idle_wake();
            }
            break;
        }
    }
}

// ── Consumer API ──────────────────────────────────────────────────────────

u8 mouse_eat_event(mouse_event_t *out) {
    if (g_mouse_read == g_mouse_write) return 0;
    u64 irq = save_irq_and_disable();
    if (g_mouse_read == g_mouse_write) {
        restore_irq(irq);
        return 0;
    }
    *out = g_mouse_queue[g_mouse_read];
    g_mouse_read = (g_mouse_read + 1) % MOUSE_EVENT_QUEUE_SIZE;
    restore_irq(irq);
    return 1;
}