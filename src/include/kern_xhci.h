// xHCI (USB 3.0) driver — public API.
//
// Phase 0 of media/writings/usb_basic_implementation_plan.md:
// bring up scaffolding, expose a single probe function and a single
// smoke-test function so we can verify PCI discovery and BAR mapping
// before any ring or interrupt code is written.

#ifndef KERN_XHCI_H
#define KERN_XHCI_H

#include "dialect.h"
#include "kern_pci.h"

// Walk pci_list_head and print every xHCI-class device found
// (class_code=0x0C, subclass=0x03, prog_if=0x30). At present this is
// read-only — it does NOT enable bus mastering, does NOT map BARs, and
// does NOT install any interrupt handler. Driver binding comes later.
void xhci_pci_probe(void);

// Once xhci_pci_probe() succeeds, this picks the first xHCI on the bus,
// enables bus mastering (PCI COMMAND bit 2), vmm-maps BAR0 into kernel
// virtual space, and reads back HCIVERSION + CAPLENGTH + DBOFF +
// HCCPARAMS1 to verify the chip is alive. Driver still doesn't run.
void xhci_smoke_test_first(void);

// Read-only port scan: finds the first xHCI controller, maps BAR0 via
// HHDM, reads HCSPARAMS1 for MaxPorts, then iterates every PORTSC
// register and prints a table of connected devices (port number, speed,
// status) to both the serial log and the kernel screen.
// Safe to call at any time — does not touch USBCMD, does not start the
// controller, does not enable interrupts or rings.
void xhci_list_devices(void);

#endif //KERN_XHCI_H
