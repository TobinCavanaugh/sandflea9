// xHCI register family — capability register offsets, operational register
// offsets, and PORTSC bit definitions. The subset we need through Phase 2
// of the USB plan (PROBE + BAR + CAPS smoke test). Numbers match xHCI 1.2.

#ifndef KERN_XHCI_REGS_H
#define KERN_XHCI_REGS_H

// ── Capability registers (relative to BAR0 base) ─────────────────────────────
#define XHCI_CAPLENGTH      0x00   // [byte]     low-byte tells oper base offset
#define XHCI_RSVD_01        0x01
#define XHCI_HCIVERSION     0x02   // [word]     BCD host controller interface
#define XHCI_HCSPARAMS1     0x04   // [dword]    MaxDeviceSlots, MaxInterrupters, ...
#define XHCI_HCSPARAMS2     0x08   // [dword]    MaxScratchpadBufs, doorbell-array offset
#define XHCI_HCSPARAMS3     0x0C   // [dword]    MaxU1Latency, MaxU2Latency
#define XHCI_HCCPARAMS1     0x10   // [dword]    AC64, CSZ, MaxPrimaryStreamArrays
#define XHCI_DBOFF          0x14   // [dword]    doorbell-array offset (relative to cap base)
#define XHCI_RTSOFF         0x18   // [dword]    runtime-register offset (relative to cap base)
#define XHCI_HCCPARAMS2     0x1C   // [dword]

// ── Operational registers (relative to (BAR0 + CAPLENGTH)) ───────────────────
// Add CAPLENGTH to each. QEMU's qemu-xhci reports CAPLENGTH=0x20.
#define XHCI_USBCMD         0x00   // command/control
#define XHCI_USBSTS         0x04   // status
#define XHCI_PAGESIZE       0x08
#define XHCI_DNCTRL         0x14   // device notification control
#define XHCI_CRCR           0x18   // command-ring control register
#define XHCI_DCBAAP         0x30   // device-context base address array pointer
#define XHCI_CONFIG         0x38

// ── PORTSC bit definitions (xHCI 1.2 §5.4.10) ───────────────────────────────
#define XHCI_PORTSC_CCS     (1u << 0)   // current connect status
#define XHCI_PORTSC_PED     (1u << 1)   // port enabled
#define XHCI_PORTSC_OCA     (1u << 2)   // over-current active
#define XHCI_PORTSC_PR      (1u << 4)   // port reset (write-1 to start)
#define XHCI_PORTSC_PP      (1u << 8)   // port power
#define XHCI_PORTSC_PORT_SPEED_MASK 0xFu
#define XHCI_PORTSC_PORT_SPEED_SHIFT 10
#define XHCI_PORTSC_CSC     (1u << 17)  // connect-status change (R/WC)
#define XHCI_PORTSC_PEC     (1u << 19)  // port-enabled change (R/WC)
#define XHCI_PORTSC_WRC     (1u << 20)  // warm-reset change (R/WC, USB3)
#define XHCI_PORTSC_PRC     (1u << 21)  // port-reset change (R/WC)
#define XHCI_PORTSC_WPR     (1u << 31)  // warm-port-reset (W1, USB3)

// ── USBCMD bits (operational register) ──────────────────────────────────────
#define XHCI_USBCMD_RS      (1u << 0)   // run/stop
#define XHCI_USBCMD_HCRST   (1u << 1)   // host controller reset
#define XHCI_USBCMD_INTE    (1u << 2)   // interrupts enable
#define XHCI_USBCMD_HSEE    (1u << 3)   // host system error enable
#define XHCI_USBCMD_LHCRST  (1u << 7)   // light host controller reset

// ── USBSTS bits ──────────────────────────────────────────────────────────────
#define XHCI_USBSTS_HCH     (1u << 0)   // host controller halted
#define XHCI_USBSTS_HSE     (1u << 2)   // host system error
#define XHCI_USBSTS_EINT    (1u << 3)   // event interrupt
#define XHCI_USBSTS_PCD     (1u << 4)   // port change detect
#define XHCI_USBSTS_CFLR    (1u << 12)  // controller fatal error

#endif //KERN_XHCI_REGS_H
