// xHCI (USB 3.0) driver — public API.
//
// Full driver: PCI probe, controller init, port enumeration, control
// transfers, interrupt transfers, and boot-protocol keyboard input.

#ifndef KERN_XHCI_H
#define KERN_XHCI_H

#include "dialect.h"
#include "kern_pci.h"
#include "kern_xhci_regs.h"

// ── PCI probe + smoke test ──────────────────────────────────────────────────

void xhci_pci_probe(void);
void xhci_smoke_test_first(void);

// ── Controller init ─────────────────────────────────────────────────────────
// Full controller initialisation: map BAR, stop controller, set up rings,
// DCBAA, event ring, start controller. Call once at boot.
// Returns true on success.
bool xhci_init(void);

// ── Port scanning ───────────────────────────────────────────────────────────
u32  xhc_max_ports(void);
void xhci_list_devices(void);

// ── Ring / context helpers ─────────────────────────────────────────────────
u64  xhci_input_ctx_phys(u32 slot_id);

// ── Device enumeration ─────────────────────────────────────────────────────
// Reset a port and enumerate the attached device.
// Returns true if a device was successfully reset and enabled.
bool xhci_port_reset(u32 port_num);

// Fully enumerate a USB HID boot-protocol keyboard on the given port.
// Returns the slot ID (1–8) on success, 0 on failure.
u32  xhci_enumerate_keyboard(u32 port_num);

// ── Keyboard input ──────────────────────────────────────────────────────────

// Start polling a keyboard for HID reports. Must call after enumeration.
void xhci_kbd_start_polling(u32 slot_id);

// Check if a new HID report is available (non-blocking).
// Polls event ring automatically.
bool xhci_kbd_report_ready(u32 slot_id);

// Retrieve the raw HID report buffer for a keyboard.
// Returns pointer to 8-byte report, or null. Re-queues the interrupt transfer.
u8  *xhci_kbd_get_report(u32 slot_id);

// Process all pending events on the event ring.
// Called from polling loops and ISRs.
void xhci_process_events(void);

// Check and handle any pending hot-plug enumeration requests.
// Call from the main loop. Enumerates newly-connected keyboards.
// Returns the slot ID if a new keyboard was enumerated, 0 otherwise.
u32  xhci_pending_enumerate(void);

#endif //KERN_XHCI_H
