// USB HID class driver — stub.
//
// At this phase the decoder doesn't exist. The future layout:
//   - xhci_event_ring_isr() queues a "device X has new report" notice
//   - kbd_usb_poll() (called from the keyboard pump in kern_keyboard.c)
//     drains the queue, translates 8-byte HID boot reports to set-1
//     scancodes, and feeds them through kbd_inject_scancode_set1().
//
// Today it's just a logging stub. Once the boot-protocol decoder is
// written, this file grows by ~250 lines.

#include "../../include/dialect.h"
#include "../../include/kern_usb_hid.h"
#include "../../include/kern_serial.h"

void kbd_usb_poll(void) {
    // No-op until Phase 6 (control transfers: SET_PROTOCOL) and Phase 7
    // (interrupt transfers: read 8-byte boot report) are implemented.
    //
    // Until then, the kernel's keyboard input source is intentionally
    // disabled (see PS2_KEYBOARD_ENABLE in kern_keyboard.c). When
    // xHCI rings land, lift the disable and connect this function to
    // the per-tick polling loop.
    serial_outsf("[stub kbd_usb_poll called — no HID device yet]\n");
}
