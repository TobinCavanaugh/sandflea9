// xHCI driver — full controller init, port reset, device enumeration,
// control transfers, and interrupt transfer handling for boot-protocol
// keyboard input.
//
// Phases 2–7 of media/writings/usb_basic_implementation_plan.md.

#include "../../include/dialect.h"
#include "../../include/kern_xhci.h"
#include "../../include/kern_xhci_regs.h"
#include "../../include/kern_serial.h"
#include "../../include/kern_vmm.h"
#include "../../include/kern_pci.h"
#include "../../include/kern_terminal.h"
#include "../../include/kern_mem.h"
#include "../../include/kern_keyboard.h"

// ── Forward declarations from rings module ───────────────────────────────────
extern u64  xhci_cmd_ring_init(volatile u32 *op_regs);
extern u64  xhci_event_ring_init(volatile u32 *rt_regs);
extern u64  xhci_transfer_ring_init(u32 slot_id);
extern bool xhci_post_command(u64 db_base, u64 param, u32 status, u32 trb_type, u32 slot_id);
extern bool xhci_event_dequeue_next(volatile u32 *rt_regs, xhci_trb_t *out_trb);
extern u64  xhci_dcbaa_init(volatile u32 *op_regs, u32 max_slots);
extern xhci_device_ctx_t *xhci_device_ctx(u32 slot_id);
extern xhci_input_ctx_t   *xhci_input_ctx(u32 slot_id);
extern u64  xhci_input_ctx_phys(u32 slot_id);
extern u64  xhci_transfer_ring_phys(u32 slot_id);
extern u8   xhci_port_speed_to_slot_speed(u8 port_speed);
extern void xhci_doorbell(u64 db_base, u32 slot_id, u8 target, u8 stream_id);

// Ring state shared from rings module
extern volatile xhci_trb_t *transfer_ring[];
extern u32                    transfer_enqueue[];
extern u8                     transfer_ccs[];

// ── Global xHCI state ───────────────────────────────────────────────────────
static struct {
    volatile u32 *mmio;
    volatile u32 *op_regs;
    volatile u32 *rt_regs;
    u64           db_base;
    u8            caplength;
    u32           max_slots;
    u32           max_ports;
    bool          running;
    u32           pending_enumerate;  // port to enumerate (0=none)
} xhc;

// ── Enumerated device state ──────────────────────────────────────────────────
#define MAX_SLOTS 8

typedef struct {
    u8  slot_id;
    u8  port_num;
    u8  speed;
    u16 vendor_id;
    u16 product_id;
    u8  ep0_max_packet;
    u8  ep_intf_in;
    u8  ep_intf_interval;
    u16 ep_intf_max_packet;
    bool configured;
    bool is_hid_keyboard;
    u8  last_hid_report[8];
} xhci_device_t;

static xhci_device_t devices[MAX_SLOTS] = {0};

// ── Low-level helpers ────────────────────────────────────────────────────────

static inline u32 xhci_pci_cfg(u8 bus, u8 slot, u8 func, u8 off) {
    u32 addr = (u32)((bus << 16) | (slot << 11) | (func << 8) | (off | 0x80000000));
    outl(0xCF8, addr);
    return inl(0xCFC);
}

static inline u64 xhci_bar64_low(u8 bus, u8 slot, u8 func, u8 bar_idx) {
    u32 lo = xhci_pci_cfg(bus, slot, func, (u8)(0x10 + bar_idx * 4));
    return (u64)(lo & ~0xFu);
}

static inline u64 xhci_bar64_high(u8 bus, u8 slot, u8 func, u8 bar_idx) {
    return (u64)xhci_pci_cfg(bus, slot, func, (u8)(0x10 + (bar_idx + 1) * 4));
}

static pci_device_t *first_xhci_device(void) {
    pci_device_t *node = pci_get_device(0x0C, 0x03);
    while (node) {
        if (node->class_code == 0x0C && node->subclass == 0x03 && node->prog_if == 0x30)
            return node;
        node = node->next;
    }
    return null;
}

// ── Controller init ──────────────────────────────────────────────────────────

void xhci_pci_probe(void) {
    u32 found = 0;
    serial_outsl("xHCI: scanning PCI class 0x0C / sub 0x03 / prog-IF 0x30...");
    pci_device_t *node = pci_get_device(0x0C, 0x03);
    while (node) {
        if (node->class_code == 0x0C && node->subclass == 0x03 && node->prog_if == 0x30) {
            u32 bar_lo = xhci_pci_cfg(node->bus, node->slot, node->func, 0x10);
            u32 bar_hi = xhci_pci_cfg(node->bus, node->slot, node->func, 0x14);
            u64 bar0 = ((u64)bar_hi << 32) | (bar_lo & ~0xFu);
            serial_outsf("xHCI: %04x:%04x at %02x:%02x.%d, BAR0=0x%016llx\n",
                         node->vendor_id, node->device_id,
                         node->bus, node->slot, node->func, bar0);
            found++;
        }
        node = node->next;
    }
    if (found == 0) serial_outsl("xHCI: no controllers found.");
    else serial_outsf("xHCI: probe done, %d controller(s) listed.\n", found);
}

// ── Full controller initialisation ──────────────────────────────────────────

bool xhci_init(void) {
    pci_device_t *xhci_pci = first_xhci_device();
    if (!xhci_pci) { serial_outsl("xHCI: init failed — no controller found."); return false; }

    serial_outsf("xHCI: initialising controller at %02x:%02x.%d\n",
                 xhci_pci->bus, xhci_pci->slot, xhci_pci->func);

    u32 cmd = xhci_pci_cfg(xhci_pci->bus, xhci_pci->slot, xhci_pci->func, 0x04);
    cmd |= 0x04;
    u32 cfg_addr = (u32)((xhci_pci->bus << 16) | (xhci_pci->slot << 11) |
                         (xhci_pci->func << 8) | (0x04 | 0x80000000));
    outl(0xCF8, cfg_addr);
    outl(0xCFC, cmd);

    u64 bar_lo = xhci_bar64_low(xhci_pci->bus, xhci_pci->slot, xhci_pci->func, 0);
    u64 bar_hi = xhci_bar64_high(xhci_pci->bus, xhci_pci->slot, xhci_pci->func, 0);
    if ((bar_lo & 1u) != 0) { serial_outsl("xHCI: BAR0 is I/O space — refusing."); return false; }
    u64 bar0_phys = (bar_hi << 32) | bar_lo;
    xhc.mmio = (volatile u32 *)vmm_mmio_map_phys(bar0_phys, 0x10000);
    serial_outsf("xHCI: BAR0 0x%016llx → virt 0x%016llx\n", bar0_phys, (u64)xhc.mmio);

    xhc.caplength  = (u8)(xhc.mmio[XHCI_CAPLENGTH / 4] & 0xFF);
    u32 hcsparams1 = xhc.mmio[XHCI_HCSPARAMS1 / 4];
    u32 dboff      = xhc.mmio[XHCI_DBOFF / 4];
    u32 rtsoff     = xhc.mmio[XHCI_RTSOFF / 4];

    xhc.max_slots = (u8)(hcsparams1 & 0xFF);
    xhc.max_ports = (u8)((hcsparams1 >> 24) & 0xFF);
    if (xhc.max_slots == 0) xhc.max_slots = XHCI_DEFAULT_SLOT_COUNT;

    serial_outsf("xHCI: CAPLENGTH=0x%02x Slots=%d Ports=%d DBOFF=0x%x RTSOFF=0x%x\n",
                 xhc.caplength, xhc.max_slots, xhc.max_ports, dboff, rtsoff);

    xhc.op_regs = (volatile u32 *)((u64)xhc.mmio + xhc.caplength);
    xhc.rt_regs = (volatile u32 *)((u64)xhc.mmio + rtsoff);
    xhc.db_base = (u64)xhc.mmio + dboff;

    // Stop controller
    u32 usbcmd = xhc.op_regs[XHCI_USBCMD / 4];
    usbcmd &= ~XHCI_USBCMD_RS;
    xhc.op_regs[XHCI_USBCMD / 4] = usbcmd;
    for (volatile int i = 0; i < 100000; i++) {
        if (xhc.op_regs[XHCI_USBSTS / 4] & XHCI_USBSTS_HCH) break;
        asm volatile("pause");
    }
    if (!(xhc.op_regs[XHCI_USBSTS / 4] & XHCI_USBSTS_HCH)) {
        serial_outsl("xHCI: timeout waiting for HCH (stop)"); return false;
    }
    serial_outsl("xHCI: controller halted");

    // Host Controller Reset — clears all state. Required before programming
    // CRCR/DCBAAP etc., otherwise the NEC controller may silently reject them.
    usbcmd = xhc.op_regs[XHCI_USBCMD / 4];
    usbcmd |= XHCI_USBCMD_HCRST;
    xhc.op_regs[XHCI_USBCMD / 4] = usbcmd;
    for (volatile int i = 0; i < 100000; i++) {
        if (!(xhc.op_regs[XHCI_USBCMD / 4] & XHCI_USBCMD_HCRST)) break;
        asm volatile("pause");
    }
    if (xhc.op_regs[XHCI_USBCMD / 4] & XHCI_USBCMD_HCRST) {
        serial_outsl("xHCI: timeout waiting for HCRST"); return false;
    }
    serial_outsl("xHCI: HCRST complete");

    // Wait for CNR to clear — operational registers are gated while CNR=1.
    for (volatile int i = 0; i < 100000; i++) {
        if (!(xhc.op_regs[XHCI_USBSTS / 4] & XHCI_USBSTS_CNR)) break;
        asm volatile("pause");
    }
    if (xhc.op_regs[XHCI_USBSTS / 4] & XHCI_USBSTS_CNR) {
        serial_outsl("xHCI: timeout waiting for CNR"); return false;
    }
    serial_outsl("xHCI: CNR cleared");

    xhc.op_regs[XHCI_CONFIG / 4] = xhc.max_slots;

    if (!xhci_dcbaa_init(xhc.op_regs, xhc.max_slots)) { serial_outsl("xHCI: DCBAA init failed"); return false; }
    if (!xhci_cmd_ring_init(xhc.op_regs)) { serial_outsl("xHCI: command ring init failed"); return false; }
    if (!xhci_event_ring_init(xhc.rt_regs)) { serial_outsl("xHCI: event ring init failed"); return false; }

    usbcmd = xhc.op_regs[XHCI_USBCMD / 4];
    usbcmd |= XHCI_USBCMD_RS | XHCI_USBCMD_INTE;
    xhc.op_regs[XHCI_USBCMD / 4] = usbcmd;
    for (volatile int i = 0; i < 100000; i++) {
        if (!(xhc.op_regs[XHCI_USBSTS / 4] & XHCI_USBSTS_HCH)) break;
        asm volatile("pause");
    }
    if (xhc.op_regs[XHCI_USBSTS / 4] & XHCI_USBSTS_HCH) {
        serial_outsl("xHCI: timeout waiting for controller to start"); return false;
    }

    xhc.running = true;
    serial_outsl("xHCI: controller running, rings operational");
    return true;
}

// ── Event processing ─────────────────────────────────────────────────────────

// Separate flags for commands and transfers to avoid races.
static volatile bool cmd_complete = false;
static u8  cmd_comp_code = 0;
static u32 cmd_comp_slot = 0;

static volatile bool transfer_done = false;
static u32 transfer_done_slot = 0;
static u8  transfer_comp_code = 0;   // completion code of the last transfer event

// HID report buffer (declared before xhci_process_events)
static u8  hid_report_buf[MAX_SLOTS][8];
static bool hid_report_ready[MAX_SLOTS];

void xhci_process_events(void) {
    xhci_trb_t evt;
    while (xhci_event_dequeue_next(xhc.rt_regs, &evt)) {
        u32 trb_type = (evt.control >> 10) & 0x3F;

        if (trb_type == EVT_TRB_CMD_COMPLETE) {
            u8 code = (u8)((evt.status >> EVT_TRB_CMPL_CODE_SHIFT) & EVT_TRB_CMPL_CODE_MASK);
            u32 slot = (u32)((evt.control >> 24) & 0xFF);
            cmd_complete  = true;
            cmd_comp_code = code;
            cmd_comp_slot = slot;
            serial_outsf("xHCI: EVT cmd-compl code=%d slot=%d evt(raw)=%016llx_%08x_%08x\n",
                         code, slot, evt.parameter, evt.status, evt.control);
            if (code != CC_SUCCESS)
                serial_outsf("xHCI: command failed — code=%d slot=%d\n", code, slot);
        } else if (trb_type == EVT_TRB_TRANSFER) {
            u8 code = (u8)((evt.status >> EVT_TRB_CMPL_CODE_SHIFT) & EVT_TRB_CMPL_CODE_MASK);
            u32 slot = (u32)((evt.control >> 24) & 0xFF);
            // Always signal completion — error codes are still a transfer result.
            transfer_done      = true;
            transfer_done_slot = slot;
            transfer_comp_code = code;
            if (code == CC_SUCCESS || code == CC_SHORT_PACKET) {
                if (slot < MAX_SLOTS && devices[slot].is_hid_keyboard)
                    hid_report_ready[slot] = true;
            } else {
                serial_outsf("xHCI: transfer error slot=%d code=%d\n", slot, code);
                serial_outsf("xHCI: EVT transfer slot=%d code=%d evt(raw)=%016llx_%08x_%08x\n",
                             slot, code, evt.parameter, evt.status, evt.control);
            }
        } else if (trb_type == EVT_TRB_PORT_STATUS) {
            u32 port_id = (u32)((evt.parameter >> 24) & 0xFF);
            serial_outsf("xHCI: port %d status change event\n", port_id);
            // Check PORTSC for connect/disconnect
            if (port_id > 0 && port_id <= xhc.max_ports) {
                u32 port_off = (0x400 + (port_id - 1) * 0x10) / 4;
                u32 portsc = xhc.op_regs[port_off];
                if (portsc & XHCI_PORTSC_CSC) {
                    // Clear CSC
                    xhc.op_regs[port_off] = portsc | XHCI_PORTSC_CSC;
                    if (portsc & XHCI_PORTSC_CCS) {
                        serial_outsf("xHCI: port %d device connected — will enumerate\n", port_id);
                        // Flag for enumeration from main loop to avoid recursion
                        xhc.pending_enumerate = port_id;
                    } else {
                        serial_outsf("xHCI: port %d device disconnected\n", port_id);
                    }
                }
            }
        }
    }
}

static bool xhci_wait_command(u32 *out_slot) {
    cmd_complete = false;
    for (volatile int timeout = 0; timeout < 500000; timeout++) {
        xhci_process_events();
        if (cmd_complete) {
            if (out_slot) *out_slot = cmd_comp_slot;
            return cmd_comp_code == CC_SUCCESS;
        }
        for (volatile int j = 0; j < 100; j++) asm volatile("pause");
    }
    serial_outsl("xHCI: command timeout");
    return false;
}

static bool xhci_wait_transfer(u32 *out_slot) {
    transfer_done = false;
    transfer_comp_code = 0;
    for (volatile int timeout = 0; timeout < 500000; timeout++) {
        xhci_process_events();
        if (transfer_done) {
            if (out_slot) *out_slot = transfer_done_slot;
            return (transfer_comp_code == CC_SUCCESS ||
                    transfer_comp_code == CC_SHORT_PACKET);
        }
        for (volatile int j = 0; j < 100; j++) asm volatile("pause");
    }
    serial_outsl("xHCI: transfer timeout");
    return false;
}

// ── Port reset ──────────────────────────────────────────────────────────────

bool xhci_port_reset(u32 port_num) {
    u32 port_off = (0x400 + (port_num - 1) * 0x10) / 4;
    u32 portsc = xhc.op_regs[port_off];

    if (!(portsc & XHCI_PORTSC_CCS)) {
        serial_outsf("xHCI: port %d — no device connected\n", port_num);
        return false;
    }

    u8 speed = (u8)((portsc >> XHCI_PORTSC_PORT_SPEED_SHIFT) & XHCI_PORTSC_PORT_SPEED_MASK);

    u32 clear_bits = XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_PRC | XHCI_PORTSC_WRC;
    xhc.op_regs[port_off] = portsc | clear_bits;

    serial_outsf("xHCI: port %d reset — speed=%d\n", port_num, speed);
    xhc.op_regs[port_off] = (xhc.op_regs[port_off] & ~clear_bits) | XHCI_PORTSC_PR;

    for (volatile int i = 0; i < 500000; i++) {
        portsc = xhc.op_regs[port_off];
        if (portsc & XHCI_PORTSC_PRC) break;
        asm volatile("pause");
    }

    if (!(portsc & XHCI_PORTSC_PRC)) {
        serial_outsf("xHCI: port %d reset timeout\n", port_num);
        return false;
    }

    xhc.op_regs[port_off] = portsc | XHCI_PORTSC_PRC;
    portsc = xhc.op_regs[port_off];
    if (!(portsc & XHCI_PORTSC_PED)) {
        serial_outsf("xHCI: port %d not enabled after reset\n", port_num);
        return false;
    }

    serial_outsf("xHCI: port %d reset done, enabled\n", port_num);
    return true;
}

// ── Device enumeration commands ─────────────────────────────────────────────

u32 xhci_enable_slot(void) {
    if (!xhci_post_command(xhc.db_base, 0, 0, TRB_TYPE_ENABLE_SLOT, 0)) return 0;
    u32 slot;
    if (!xhci_wait_command(&slot)) return 0;
    serial_outsf("xHCI: enable slot → %d\n", slot);
    return slot;
}

bool xhci_address_device(u32 slot_id, u8 port_num, u8 speed, bool set_address) {
    xhci_input_ctx_t *ictx = xhci_input_ctx(slot_id);
    if (!ictx) return false;

    mem_set((u8 *)ictx, 0, sizeof(xhci_input_ctx_t));
    ictx->add_flags = (1u << 0) | (1u << 1);

    xhci_slot_ctx_t *sc = &ictx->ctx.slot;
    u8 slot_speed = xhci_port_speed_to_slot_speed(speed);
    // Route String = 0 for root-hub-attached devices (was wrongly set to port_num).
    // CE = 1 (slot=0, EP0=context index 1). Port number goes in dw1 for LPM.
    sc->dw0 = (1u << 27) | (slot_speed << 20);
    sc->dw1 = (port_num << 16);
    sc->dw2 = 0; sc->dw3 = 0;
    sc->dw4 = set_address ? ((slot_id & 0xFF) | (SLOT_STATE_ADDRESSED << 27))
                           : (SLOT_STATE_DEFAULT << 27);

    serial_outsf("xHCI: ADDR-DEV slot=%d port=%d speed=%d set_addr=%d\n",
                 slot_id, port_num, speed, set_address);
    serial_outsf("xHCI:   SC dw0=%08x dw1=%08x dw2=%08x dw3=%08x\n",
                 sc->dw0, sc->dw1, sc->dw2, sc->dw3);
    serial_outsf("xHCI:   SC dw4=%08x dw5=%08x dw6=%08x dw7=%08x\n",
                 sc->dw4, sc->dw5, sc->dw6, sc->dw7);

    xhci_ep_ctx_t *ep0 = &ictx->ctx.eps[0];
    ep0->dw0 = EP_TYPE_CONTROL << 28;
    ep0->dw1 = 8;
    u64 tr_phys = xhci_transfer_ring_phys(slot_id);
    ep0->dw2 = (u32)(tr_phys & 0xFFFFFFFF) | 1;   // DCS is bit 0 of LOW dword
    ep0->dw3 = (u32)(tr_phys >> 32);
    ep0->dw4 = 8;

    serial_outsf("xHCI:  EP0 dw0=%08x dw1=%08x dw2=%08x dw3=%08x\n",
                 ep0->dw0, ep0->dw1, ep0->dw2, ep0->dw3);
    serial_outsf("xHCI:  EP0 dw4=%08x tr_phys=0x%016llx\n",
                 ep0->dw4, tr_phys);

    u64 ictx_phys = xhci_input_ctx_phys(slot_id);
    serial_outsf("xHCI:  ictx_phys=0x%016llx add_flags=0x%08x\n",
                 ictx_phys, ictx->add_flags);
    u32 bsr = set_address ? 0 : (1u << 9);
    if (!xhci_post_command(xhc.db_base, ictx_phys, bsr, TRB_TYPE_ADDRESS_DEVICE, slot_id)) return false;
    u32 result_slot;
    if (!xhci_wait_command(&result_slot)) return false;
    if (result_slot != slot_id) {
        serial_outsf("xHCI: address device slot mismatch: expected %d, got %d\n", slot_id, result_slot);
        return false;
    }
    serial_outsf("xHCI: slot %d addressed (BSR=%d)\n", slot_id, bsr ? 1 : 0);

    // Dump output EP0 context to verify DCS and TR dequeue pointer
    xhci_device_ctx_t *dctx = xhci_device_ctx(slot_id);
    if (dctx) {
        serial_outsf("xHCI:  output EP0 dw0=%08x dw1=%08x dw2=%08x dw3=%08x (DCS=%d)\n",
                     dctx->eps[0].dw0, dctx->eps[0].dw1,
                     dctx->eps[0].dw2, dctx->eps[0].dw3,
                     dctx->eps[0].dw2 & 1);   // DCS is bit 0 of dw2 (low dword)
    }
    return true;
}

bool xhci_configure_endpoint(u32 slot_id, u8 ep_num, u8 ep_type, u16 max_packet,
                              u8 interval, u64 tr_phys) {
    xhci_input_ctx_t *ictx = xhci_input_ctx(slot_id);
    if (!ictx) return false;

    mem_set((u8 *)ictx, 0, sizeof(xhci_input_ctx_t));
    ictx->add_flags = (1u << 0) | (1u << (ep_num + 1));

    xhci_device_ctx_t *dctx = xhci_device_ctx(slot_id);
    mem_copy((u8 *)&ictx->ctx.slot, (u8 *)&dctx->slot, sizeof(xhci_slot_ctx_t));
    // Update Context Entries = max endpoint index (new EP being added).
    ictx->ctx.slot.dw0 = (ictx->ctx.slot.dw0 & ~(0x1Fu << 27)) | (ep_num << 27);

    xhci_ep_ctx_t *ep = &ictx->ctx.eps[ep_num];
    ep->dw0 = (ep_type << 28) | (interval << 16);
    ep->dw1 = max_packet | (3 << 23);
    ep->dw2 = (u32)(tr_phys & 0xFFFFFFFF) | 1;   // DCS is bit 0 of LOW dword
    ep->dw3 = (u32)(tr_phys >> 32);
    ep->dw4 = max_packet;

    u64 ictx_phys = xhci_input_ctx_phys(slot_id);
    if (!xhci_post_command(xhc.db_base, ictx_phys, 0, TRB_TYPE_CONFIGURE_EP, slot_id)) return false;
    u32 result_slot;
    if (!xhci_wait_command(&result_slot)) return false;
    if (result_slot != slot_id) return false;

    serial_outsf("xHCI: slot %d EP%d configured (type=%d mps=%d int=%d)\n",
                 slot_id, ep_num, ep_type, max_packet, interval);
    return true;
}

// ── Transfer-ring enqueue helper ─────────────────────────────────────────────
// Advance to the next TRB slot, skipping the Link TRB at the last position.
static inline u32 tr_enq_next(u32 enq) {
    u32 next = (enq + 1) % XHCI_TRANSFER_RING_TRBS;
    if (next == XHCI_TRANSFER_RING_TRBS - 1)
        return 0;
    return next;
}

// ── Control transfer ─────────────────────────────────────────────────────────

i32 xhci_control_transfer(u32 slot_id, const u8 setup_data[8],
                           u8 *buffer, u32 buflen, bool data_in) {
    if (slot_id >= MAX_SLOTS) return -1;

    u32 sl = slot_id;

    u32 trt;
    if (setup_data[6] == 0 && setup_data[7] == 0)
        trt = TRB_TRT_NONE;
    else if (data_in)
        trt = TRB_TRT_IN;
    else
        trt = TRB_TRT_OUT;

    // Get physical address of buffer (stack buffers are in HHDM space)
    u64 data_phys = 0;
    if (buflen > 0 && buffer)
        data_phys = (u64)buffer - vmm_get_hhdm();

    // 1. Setup Stage TRB — always chained (Data or Status follows), no IOC
    u64 setup_word;
    mem_copy((u8 *)&setup_word, setup_data, 8);
    {
        u32 enq = transfer_enqueue[sl];
        u32 next = tr_enq_next(enq);
        transfer_ring[sl][enq].parameter = setup_word;
        transfer_ring[sl][enq].status    = 8;
        transfer_ring[sl][enq].control   = (TRB_TYPE_SETUP_STAGE << TRB_TYPE_SHIFT)
                                         | trt | TRB_CH | transfer_ccs[sl];
        transfer_enqueue[sl] = next;
    }

    // 2. Data Stage TRB (if there is data) — chained to Status
    if (buflen > 0 && buffer) {
        u32 enq = transfer_enqueue[sl];
        u32 next = tr_enq_next(enq);
        transfer_ring[sl][enq].parameter = data_phys;
        transfer_ring[sl][enq].status    = buflen;  // TD Size = 0 for single TRB
        u32 dir_trt = data_in ? TRB_TRT_IN : TRB_TRT_OUT;
        transfer_ring[sl][enq].control   = (TRB_TYPE_DATA_STAGE << TRB_TYPE_SHIFT)
                                         | dir_trt | TRB_CH | transfer_ccs[sl];
        transfer_enqueue[sl] = next;

        // 3. Status Stage TRB
        enq = transfer_enqueue[sl];
        next = tr_enq_next(enq);
        u32 status_trt = data_in ? TRB_TRT_OUT : TRB_TRT_IN;
        transfer_ring[sl][enq].parameter = 0;
        transfer_ring[sl][enq].status    = 0;
        // Last TRB in chain — IOC to signal completion.
        transfer_ring[sl][enq].control   = (TRB_TYPE_STATUS_STAGE << TRB_TYPE_SHIFT)
                                         | status_trt | TRB_IOC
                                         | transfer_ccs[sl];
        transfer_enqueue[sl] = next;
    } else {
        // No data: Status Stage after Setup
        u32 enq = transfer_enqueue[sl];
        u32 next = tr_enq_next(enq);
        transfer_ring[sl][enq].parameter = 0;
        transfer_ring[sl][enq].status    = 0;
        // No-data path: Status TRB after Setup is the last TRB — IOC.
        transfer_ring[sl][enq].control   = (TRB_TYPE_STATUS_STAGE << TRB_TYPE_SHIFT)
                                         | TRB_TRT_IN | TRB_IOC
                                         | transfer_ccs[sl];
        transfer_enqueue[sl] = next;
    }

    // MFENCE + dummy read: flush CPU write buffer before doorbell.
    asm volatile("mfence" ::: "memory");
    { volatile u32 _d = transfer_ring[sl][0].control; (void)_d; }

    xhci_doorbell(xhc.db_base, slot_id, 1, 0);

    // Diagnostic: poll for any event type to see what arrives
    serial_outsf("xHCI: doorbell rung for slot %d EP0 — waiting for event...\n", slot_id);

    u32 result_slot;
    if (!xhci_wait_transfer(&result_slot)) {
        serial_outsf("xHCI: control transfer timeout (slot %d)\n", slot_id);
        return -1;
    }
    return (i32)buflen;
}

// ── Interrupt transfer posting ──────────────────────────────────────────────

void xhci_queue_interrupt_in(u32 slot_id, u8 ep_num, u8 *buffer, u32 buflen) {
    if (slot_id >= MAX_SLOTS) return;

    u64 data_phys = (u64)buffer - vmm_get_hhdm();
    u32 sl = slot_id;

    u32 enq = transfer_enqueue[sl];
    u32 next = tr_enq_next(enq);
    transfer_ring[sl][enq].parameter = data_phys;
    transfer_ring[sl][enq].status    = buflen;
    transfer_ring[sl][enq].control   = (TRB_TYPE_NORMAL << TRB_TYPE_SHIFT)
                                     | TRB_IOC | transfer_ccs[sl];
    transfer_enqueue[sl] = next;

    asm volatile("mfence" ::: "memory");
    { volatile u32 _d = transfer_ring[sl][0].control; (void)_d; }
    xhci_doorbell(xhc.db_base, slot_id, ep_num + 1, 0);
}

// ── Full keyboard enumeration ────────────────────────────────────────────────

u32 xhci_enumerate_keyboard(u32 port_num) {
    if (!xhci_port_reset(port_num)) return 0;

    u32 port_off = (0x400 + (port_num - 1) * 0x10) / 4;
    u32 portsc = xhc.op_regs[port_off];
    u8 speed = (u8)((portsc >> XHCI_PORTSC_PORT_SPEED_SHIFT) & XHCI_PORTSC_PORT_SPEED_MASK);

    u32 slot_id = xhci_enable_slot();
    if (!slot_id) return 0;

    u64 tr_phys = xhci_transfer_ring_init(slot_id);
    if (!tr_phys) return 0;

    // Single Address Device with SET_ADDRESS — skip BSR to avoid NEC quirk.
    if (!xhci_address_device(slot_id, port_num, speed, true)) {
        serial_outsf("xHCI: address device failed for slot %d\n", slot_id);
        return 0;
    }

    // Flush any Transfer Events the controller may have generated during
    // Address Device (e.g. from speculatively processing No-Op TRBs).
    transfer_done = false;
    xhci_process_events();

    xhci_device_t *dev = &devices[slot_id];
    mem_set((u8 *)dev, 0, sizeof(xhci_device_t));
    dev->slot_id  = slot_id;
    dev->port_num = port_num;
    dev->speed    = speed;
    dev->ep0_max_packet = 8;

    // GET_DESCRIPTOR(Device, wLength=18)
    u8 setup[8];
    u8 dev_desc_buf[18];
    mem_set(dev_desc_buf, 0, 18);

    setup[0] = 0x80; setup[1] = 6; setup[2] = 0x01; setup[3] = 0x00;
    setup[4] = 0x00; setup[5] = 0x00; setup[6] = 18;  setup[7] = 0;

    i32 ret = xhci_control_transfer(slot_id, setup, dev_desc_buf, 18, true);
    if (ret < 0) { serial_outsf("xHCI: GET_DESCRIPTOR(18) failed on slot %d\n", slot_id); return 0; }

    dev->ep0_max_packet = dev_desc_buf[7];
    serial_outsf("xHCI: slot %d bMaxPacketSize0=%d\n", slot_id, dev->ep0_max_packet);

    dev->vendor_id  = (u16)(dev_desc_buf[8]  | (dev_desc_buf[9]  << 8));
    dev->product_id = (u16)(dev_desc_buf[10] | (dev_desc_buf[11] << 8));
    serial_outsf("xHCI: slot %d Vendor=0x%04x Product=0x%04x\n", slot_id, dev->vendor_id, dev->product_id);

    // GET_DESCRIPTOR(Configuration)
    u8 cfg_buf[128];
    mem_set(cfg_buf, 0, 128);
    setup[2] = 0x02; setup[6] = 9; setup[7] = 0;
    ret = xhci_control_transfer(slot_id, setup, cfg_buf, 9, true);
    if (ret < 0) { serial_outsf("xHCI: GET_DESCRIPTOR(Config:9) failed on slot %d\n", slot_id); return 0; }

    u16 config_total = (u16)(cfg_buf[2] | (cfg_buf[3] << 8));
    if (config_total > 128) config_total = 128;
    setup[6] = (u8)(config_total & 0xFF);
    setup[7] = (u8)(config_total >> 8);
    ret = xhci_control_transfer(slot_id, setup, cfg_buf, config_total, true);
    if (ret < 0) { serial_outsf("xHCI: GET_DESCRIPTOR(Config:%d) failed on slot %d\n", config_total, slot_id); return 0; }

    // Parse configuration descriptor
    u32 offset = 9;
    bool found_hid = false;
    u16 intf_num = 0;

    while (offset < config_total) {
        u8 desc_len  = cfg_buf[offset];
        u8 desc_type = cfg_buf[offset + 1];

        if (desc_type == 0x04) {
            u8 intf_class    = cfg_buf[offset + 5];
            u8 intf_subclass = cfg_buf[offset + 6];
            intf_num         = cfg_buf[offset + 2];
            u8 num_endpoints = cfg_buf[offset + 4];

            if (intf_class == 0x03 && intf_subclass == 0x01) {
                found_hid = true;
                serial_outsf("xHCI: slot %d HID keyboard interface %d\n", slot_id, intf_num);
            }
            offset += desc_len;

            for (u8 ep_idx = 0; ep_idx < num_endpoints && offset < config_total; ep_idx++) {
                desc_len  = cfg_buf[offset];
                desc_type = cfg_buf[offset + 1];
                if (desc_type == 0x05 && found_hid) {
                    u8  ep_addr = cfg_buf[offset + 2];
                    u8  ep_attr = cfg_buf[offset + 3];
                    u16 ep_mps  = (u16)(cfg_buf[offset + 4] | (cfg_buf[offset + 5] << 8));
                    u8  ep_int  = cfg_buf[offset + 6];
                    bool is_in  = (ep_addr & 0x80) != 0;
                    u8 ep_type  = ep_attr & 0x03;
                    if (is_in && ep_type == 3) {
                        dev->ep_intf_in         = ep_addr & 0x0F;
                        dev->ep_intf_max_packet = ep_mps;
                        dev->ep_intf_interval   = ep_int;
                        dev->is_hid_keyboard    = true;
                        serial_outsf("xHCI: slot %d interrupt IN EP%d mps=%d interval=%d\n",
                                     slot_id, dev->ep_intf_in, ep_mps, ep_int);
                    }
                }
                offset += desc_len;
            }
        } else {
            offset += desc_len > 0 ? desc_len : 1;
        }
    }

    if (!found_hid || !dev->is_hid_keyboard) {
        serial_outsf("xHCI: slot %d — not a HID keyboard\n", slot_id);
        return 0;
    }

    // SET_CONFIGURATION
    setup[0] = 0x00; setup[1] = 9; setup[2] = (u8)cfg_buf[5]; setup[3] = 0;
    setup[4] = 0; setup[5] = 0; setup[6] = 0; setup[7] = 0;
    ret = xhci_control_transfer(slot_id, setup, null, 0, false);
    if (ret < 0) { serial_outsf("xHCI: SET_CONFIGURATION failed on slot %d\n", slot_id); return 0; }
    dev->configured = true;
    serial_outsf("xHCI: slot %d SET_CONFIGURATION OK\n", slot_id);

    // SET_PROTOCOL (boot protocol)
    setup[0] = 0x21; setup[1] = 0x0B; setup[2] = 0x00; setup[3] = 0x00;
    setup[4] = (u8)intf_num; setup[5] = 0x00; setup[6] = 0x00; setup[7] = 0x00;
    ret = xhci_control_transfer(slot_id, setup, null, 0, false);
    if (ret < 0)
        serial_outsf("xHCI: SET_PROTOCOL failed on slot %d (non-fatal)\n", slot_id);
    else
        serial_outsf("xHCI: slot %d SET_PROTOCOL (boot) OK\n", slot_id);

    // Configure interrupt IN endpoint
    if (dev->ep_intf_in) {
        u64 ep_tr_phys = xhci_transfer_ring_init(slot_id);
        if (!ep_tr_phys) { serial_outsf("xHCI: interrupt ring alloc failed for slot %d\n", slot_id); return 0; }
        if (!xhci_configure_endpoint(slot_id, dev->ep_intf_in, EP_TYPE_INTERRUPT_IN,
                                      dev->ep_intf_max_packet, dev->ep_intf_interval, ep_tr_phys)) {
            serial_outsf("xHCI: configure EP failed for slot %d\n", slot_id);
            return 0;
        }
    }

    serial_outsf("xHCI: slot %d keyboard enumeration complete\n", slot_id);
    return slot_id;
}

// ── Keyboard polling ─────────────────────────────────────────────────────────

void xhci_kbd_start_polling(u32 slot_id) {
    xhci_device_t *dev = &devices[slot_id];
    if (!dev->is_hid_keyboard || !dev->configured) return;
    mem_set(hid_report_buf[slot_id], 0, 8);
    hid_report_ready[slot_id] = false;
    xhci_queue_interrupt_in(slot_id, dev->ep_intf_in, hid_report_buf[slot_id], 8);
}

bool xhci_kbd_report_ready(u32 slot_id) {
    if (slot_id >= MAX_SLOTS) return false;
    xhci_process_events();
    return hid_report_ready[slot_id];
}

u8 *xhci_kbd_get_report(u32 slot_id) {
    if (slot_id >= MAX_SLOTS) return null;
    hid_report_ready[slot_id] = false;
    xhci_device_t *dev = &devices[slot_id];
    if (dev->is_hid_keyboard && dev->configured)
        xhci_queue_interrupt_in(slot_id, dev->ep_intf_in, hid_report_buf[slot_id], 8);
    return hid_report_buf[slot_id];
}

u32 xhc_max_ports(void) { return xhc.max_ports; }

u32 xhci_pending_enumerate(void) {
    if (xhc.pending_enumerate == 0) return 0;
    u32 port = xhc.pending_enumerate;
    xhc.pending_enumerate = 0;
    serial_outsf("xHCI: hot-plug enumerate port %d\n", port);
    return xhci_enumerate_keyboard(port);
}

// ── Smoke test ───────────────────────────────────────────────────────────────

void xhci_smoke_test_first(void) {
    pci_device_t *xhci_pci = first_xhci_device();
    if (!xhci_pci) { serial_outsl("xHCI: smoke-test skipped."); return; }
    u64 bar0_phys = (xhci_bar64_high(xhci_pci->bus, xhci_pci->slot, xhci_pci->func, 0) << 32)
                  | xhci_bar64_low(xhci_pci->bus, xhci_pci->slot, xhci_pci->func, 0);
    u64 base_virt = vmm_mmio_map_phys(bar0_phys, 0x10000);
    volatile u32 *mmio = (volatile u32 *)base_virt;
    u8  caplength  = (u8)(mmio[XHCI_CAPLENGTH / 4] & 0xFF);
    u16 hciversion = (u16)((mmio[XHCI_HCIVERSION / 4] >> 16) & 0xFFFF);
    serial_outsf("xHCI: CAPLENGTH=0x%02x, HCIVERSION=0x%04x\n", caplength, hciversion);
    serial_outsl("xHCI: smoke-test OK.");
}

// ── usb command ──────────────────────────────────────────────────────────────

static const char *xhci_port_speed_name(u8 speed) {
    switch (speed) {
        case 1: return "Full-speed (12 Mbps)";
        case 2: return "Low-speed (1.5 Mbps)";
        case 3: return "High-speed (480 Mbps)";
        case 4: return "SuperSpeed (5 Gbps)";
        default: return (speed >= 5 && speed <= 15) ? "SuperSpeedPlus (10+ Gbps)" : "unknown";
    }
}

void xhci_list_devices(void) {
    if (!xhc.mmio) {
        serial_outsl("usb: xHCI not initialized.");
        screen_push_line("usb: xHCI not initialized.");
        return;
    }
    u32 max_ports = xhc.max_ports;
    screen_push_linef("usb: xHCI controller, %d port(s)", max_ports);
    if (max_ports == 0) { screen_push_line("usb: controller reports 0 ports."); return; }

    u32 found = 0;
    screen_push_line("PORT  SPEED                     STATUS");
    screen_push_line("----  -----                     ------");
    for (u32 port = 0; port < max_ports; port++) {
        u32 portsc = xhc.op_regs[(0x400 + port * 0x10) / 4];
        bool ccs = (portsc & XHCI_PORTSC_CCS) != 0;
        bool ped = (portsc & XHCI_PORTSC_PED) != 0;
        bool pp  = (portsc & XHCI_PORTSC_PP) != 0;
        u8 speed = (u8)((portsc >> XHCI_PORTSC_PORT_SPEED_SHIFT) & XHCI_PORTSC_PORT_SPEED_MASK);
        const char *status = !pp ? "no power" : !ccs ? "empty" : !ped ? "connected (not enabled)" : "connected + enabled";
        if (ccs) { screen_push_linef("%-4d  %-24s  %s", port + 1, xhci_port_speed_name(speed), status); found++; }
    }
    if (found == 0) screen_push_line("(no devices connected)");
    screen_push_linef("usb: %d device(s) connected.", found);
}
