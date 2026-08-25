// kern_mouse.h — PS/2 mouse driver interface.
//
// The PS/2 mouse sends 3-byte movement packets on IRQ 12 (vector 44).
// This driver accumulates those bytes, decodes them into signed deltas,
// and pushes mouse events into a ring buffer consumed by the compositor
// or foreground game via mouse_eat_event().

#ifndef SANDFLEA9_KERN_MOUSE_H
#define SANDFLEA9_KERN_MOUSE_H

#include "dialect.h"
#include "kern_interrupts.h"

// ── Mouse event types ─────────────────────────────────────────────────────

#define MOUSE_EVENT_MOVE  1   // d0=dx, d1=dy
#define MOUSE_EVENT_BTN   2   // d0=button (1=left, 2=right, 4=middle), d1=down(1)/up(0)

// ── Event struct ──────────────────────────────────────────────────────────
// Small fixed-size struct so the ring buffer is cache-friendly.
typedef struct {
    u8  type;   // MOUSE_EVENT_MOVE or MOUSE_EVENT_BTN
    i8  dx;     // signed x delta (move) or button id (btn)
    i8  dy;     // signed y delta (move) or 1=down/0=up (btn)
} mouse_event_t;

#define MOUSE_EVENT_QUEUE_SIZE 128

// ── API ───────────────────────────────────────────────────────────────────

// Initialize PS/2 mouse: enable aux port, set sample rate, enable data reporting.
void mouse_init(void);

// PS/2 mouse ISR (IRQ 12, vector 44). Registered via interrupt_register.
void mouse_handle_interrupt(const registers_t *t);

// Pop one event from the queue. Returns 0 if queue is empty.
u8 mouse_eat_event(mouse_event_t *out);

#endif // SANDFLEA9_KERN_MOUSE_H