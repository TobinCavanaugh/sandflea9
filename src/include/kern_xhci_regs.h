// xHCI register family — capability register offsets, operational register
// offsets, PORTSC bit definitions, TRB types, and context structures.
// Subset needed for boot-protocol keyboard enumeration and input.
// Numbers match xHCI 1.2.

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
#define XHCI_USBCMD         0x00   // command/control
#define XHCI_USBSTS         0x04   // status
#define XHCI_PAGESIZE       0x08
#define XHCI_DNCTRL         0x14   // device notification control
#define XHCI_CRCR_LO        0x18   // command-ring control register (low)
#define XHCI_CRCR_HI        0x1C   // command-ring control register (high)
#define XHCI_DCBAAP_LO      0x30   // device-context base address array pointer (low)
#define XHCI_DCBAAP_HI      0x34   // device-context base address array pointer (high)
#define XHCI_CONFIG         0x38

// ── Runtime registers (relative to (BAR0 + RTSOFF)) ─────────────────────────
#define XHCI_IMAN(x)        (0x0020 + (x)*0x20)  // interrupt management
#define XHCI_IMOD(x)        (0x0024 + (x)*0x20)  // interrupt moderation
#define XHCI_ERSTSZ(x)      (0x0028 + (x)*0x20)  // event ring segment table size
#define XHCI_ERSTBA_LO(x)   (0x0030 + (x)*0x20)  // ERST base address low
#define XHCI_ERSTBA_HI(x)   (0x0034 + (x)*0x20)  // ERST base address high
#define XHCI_ERDP_LO(x)     (0x0038 + (x)*0x20)  // event ring dequeue pointer low
#define XHCI_ERDP_HI(x)     (0x003C + (x)*0x20)  // event ring dequeue pointer high

// ── Doorbell array (relative to (BAR0 + DBOFF)) ─────────────────────────────
#define XHCI_DOORBELL(slot) (slot * 4)           // doorbell target for slot N

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
#define XHCI_USBSTS_CNR     (1u << 11)  // controller not ready
#define XHCI_USBSTS_CFLR    (1u << 12)  // controller fatal error

// ── Interrupter Management (IMAN) bits ──────────────────────────────────────
#define XHCI_IMAN_IE        (1u << 1)   // interrupt enable
#define XHCI_IMAN_IP        (1u << 0)   // interrupt pending (R/WC)

// ── CRCR bits ────────────────────────────────────────────────────────────────
#define XHCI_CRCR_RCS       (1u << 0)   // ring cycle state
#define XHCI_CRCR_CS        (1u << 1)   // command stop
#define XHCI_CRCR_CA        (1u << 2)   // command abort
#define XHCI_CRCR_CRR       (1u << 3)   // command ring running (RO)

// ── TRB types (low 6 bits of field 3) ───────────────────────────────────────
#define TRB_TYPE_NORMAL          1
#define TRB_TYPE_SETUP_STAGE     2
#define TRB_TYPE_DATA_STAGE      3
#define TRB_TYPE_STATUS_STAGE    4
#define TRB_TYPE_LINK            6
#define TRB_TYPE_ENABLE_SLOT     9
#define TRB_TYPE_ADDRESS_DEVICE 11
#define TRB_TYPE_CONFIGURE_EP   12
#define TRB_TYPE_EVAL_CONTEXT   13
#define TRB_TYPE_RESET_EP       14
#define TRB_TYPE_STOP_EP        15
#define TRB_TYPE_SET_TR_DEQUEUE 16
#define TRB_TYPE_NO_OP          23

// ── Event TRB types ─────────────────────────────────────────────────────────
#define EVT_TRB_TRANSFER        32
#define EVT_TRB_CMD_COMPLETE    33
#define EVT_TRB_PORT_STATUS     34

// ── TRB flags ────────────────────────────────────────────────────────────────
#define TRB_CYCLE_BIT           (1u << 0)
#define TRB_ENT                (1u << 1)   // evaluate next TRB
#define TRB_TC                 (1u << 1)   // toggle cycle (bit 1 in Link TRB)
#define TRB_CH                 (1u << 4)   // chain bit
#define TRB_IOC                (1u << 5)   // interrupt on complete
#define TRB_TYPE_SHIFT          10          // TRB Type field is at bits 10:15
#define TRB_TRT_OUT            0
#define TRB_TRT_IN             (3u << 16)  // transfer type: IN
#define TRB_TRT_NONE           (0u << 16)

#define EVT_TRB_CMPL_CODE_SHIFT 24
#define EVT_TRB_CMPL_CODE_MASK  0xFF

// ── Completion codes ─────────────────────────────────────────────────────────
#define CC_SUCCESS              1
#define CC_TRB_ERROR             5
#define CC_STALL_ERROR           6
#define CC_SHORT_PACKET         13
#define CC_RING_UNDERRUN        14
#define CC_RING_OVERRUN         15

// ── Slot context speeds ──────────────────────────────────────────────────────
#define SLOT_SPEED_FULL          1
#define SLOT_SPEED_LOW           2
#define SLOT_SPEED_HIGH          3
#define SLOT_SPEED_SUPER         4

// ── Slot context state ───────────────────────────────────────────────────────
#define SLOT_STATE_DISABLED      0
#define SLOT_STATE_DEFAULT       1
#define SLOT_STATE_ADDRESSED     2
#define SLOT_STATE_CONFIGURED    3

// ── Endpoint type ────────────────────────────────────────────────────────────
#define EP_TYPE_NOT_VALID        0
#define EP_TYPE_ISOCH_OUT        1
#define EP_TYPE_BULK_OUT         2
#define EP_TYPE_INTERRUPT_OUT    3
#define EP_TYPE_CONTROL          4
#define EP_TYPE_ISOCH_IN         5
#define EP_TYPE_BULK_IN          6
#define EP_TYPE_INTERRUPT_IN     7

// ── TRB Transfer lengths ─────────────────────────────────────────────────────
#define TRB_TD_SIZE_SHIFT       17

// ── Context structures (xHCI 1.2 §6.2) ──────────────────────────────────────

// TRB: 16 bytes
typedef struct {
    u64 parameter;       // data buffer or context pointer
    u32 status;          // transfer length (low 24 bits) + completion code
    u32 control;         // cycle bit (bit 0) + TRB type (bits 10:15)
} __attribute__((packed)) xhci_trb_t;

// Event Ring Segment Table entry (16 bytes)
typedef struct {
    u64 ring_base;       // 64-bit physical address of Event Ring segment
    u32 ring_size;       // number of TRBs in the segment
    u32 reserved;
} __attribute__((packed)) xhci_erst_entry_t;

// Slot context (32 bytes, part of Device Context)
typedef struct {
    u32 dw0;  // context entries (27:31) + speed (20:23) + root hub port (0:19)
    u32 dw1;  // max exit latency + root hub port number (bits 16:23)
    u32 dw2;  // interrupter target (bits 22:31)
    u32 dw3;  // TT hub slot + TT port
    u32 dw4;  // device address (bits 0:7) + slot state (bits 27:31)
    u32 dw5;
    u32 dw6;
    u32 dw7;
} __attribute__((packed)) xhci_slot_ctx_t;

// Endpoint context (32 bytes)
typedef struct {
    u32 dw0;  // EP state (0:2=RsvdZ) + mult (4:5) + interval (16:23) + EP type (28:30) + Halted (31)
    u32 dw1;  // max packet size + max burst + HID + error count + CErr (bits 23:25)
    u32 dw2;  // TR dequeue pointer low  (bits 31:0 of 64-bit TRDP), bit 0 = DCS
    u32 dw3;  // TR dequeue pointer high (bits 63:32)
    u32 dw4;  // average TRB length
    u32 dw5;  // max ESIT payload
    u32 dw6;
    u32 dw7;
} __attribute__((packed)) xhci_ep_ctx_t;

// Device context: slot context + 31 endpoint contexts (1024 bytes, 64-byte aligned)
typedef struct {
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t   eps[31];
} __attribute__((packed, aligned(64))) xhci_device_ctx_t;

// Input context (8 + 8 + 6*4 = 40 bytes header + device context)
typedef struct {
    u32 drop_flags;       // bitmask: bit i set = drop context i
    u32 add_flags;        // bitmask: bit i set = add context i
    u32 rsvd[6];
    xhci_device_ctx_t ctx;
} __attribute__((packed, aligned(64))) xhci_input_ctx_t;

// Extended Capability ID constants
#define XHCI_EXT_CAP_USB_LEGACY  1
#define XHCI_EXT_CAP_SUPPORTED   2

// ── Ring management constants ────────────────────────────────────────────────
#define XHCI_CMD_RING_TRBS     256    // command ring: 256 TRBs (one 4KB page)
#define XHCI_EVENT_RING_TRBS   256    // event ring: 256 TRBs
#define XHCI_TRANSFER_RING_TRBS 32    // transfer ring: 32 TRBs per endpoint
#define XHCI_DEFAULT_SLOT_COUNT 8

#endif //KERN_XHCI_REGS_H
