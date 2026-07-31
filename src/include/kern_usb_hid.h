// USB HID class driver — boot protocol only.
//
// At this phase, only prototypes live here. The actual decode-from-boot-
// report and translate-to-set-1-scancode code is the next phase of
// media/writings/usb_basic_implementation_plan.md. We keep the header
// committed so the build knows there will be a HID layer, and so the
// xHCI driver can drop a placeholder call into the eventual HID decoder
// without compiler churn.

#ifndef KERN_USB_HID_H
#define KERN_USB_HID_H

#include "dialect.h"

// Placeholder call site. Future signature: takes a (slot_id, endpoint_id)
// and either blocks or returns whether new report data is available. For
// now this is a no-op stub.
void kbd_usb_poll(void);

#endif //KERN_USB_HID_H
