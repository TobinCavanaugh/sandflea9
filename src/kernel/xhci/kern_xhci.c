// xHCI driver — Phase 1 (read-only PCI probe) + Phase 2 (BAR map +
// CAPS smoke test). No rings, no interrupts, no enumeration yet.

#include "../../include/dialect.h"
#include "../../include/kern_xhci.h"
#include "../../include/kern_xhci_regs.h"
#include "../../include/kern_serial.h"
#include "../../include/kern_vmm.h"
#include "../../include/kern_pci.h"
#include "../../include/kern_terminal.h"

// Reads a 32-bit value from PCI config space at the given BDF.
static inline u32 xhci_pci_cfg(u8 bus, u8 slot, u8 func, u8 off) {
    u32 addr = (u32) ((bus << 16) | (slot << 11) | (func << 8) | (off | 0x80000000));
    outl(0xCF8, addr);
    return inl(0xCFC);
}

// Returns the lower 32 bits of a 64-bit MMIO BAR, with the low 4 bits
// (type indicator + prefetchable) stripped — those aren't part of the
// address. Caller must check that bit 0 is 0 (MMIO, not I/O space).
static inline u64 xhci_bar64_low(u8 bus, u8 slot, u8 func, u8 bar_idx) {
    u32 lo = xhci_pci_cfg(bus, slot, func, (u8) (0x10 + bar_idx * 4));
    return (u64) (lo & ~0xFu);
}

// Returns the upper 32 bits of a 64-bit MMIO BAR. Caller is expected to
// verify the BAR is 64-bit (next BAR's low bits have bit 0x4 set, by the
// PCI spec convention). For xHCI's BAR0 that's always the case.
static inline u64 xhci_bar64_high(u8 bus, u8 slot, u8 func, u8 bar_idx) {
    return (u64) xhci_pci_cfg(bus, slot, func, (u8) (0x10 + (bar_idx + 1) * 4));
}

// Walk the USB-class list for the first xHCI entry
// (class_code=0x0C, subclass=0x03, prog_if=0x30). Used by both probe
// and the smoke test so they stay in lockstep. Returns NULL if none.
static pci_device_t *first_xhci_device(void) {
    pci_device_t *node = pci_get_device(0x0C, 0x03);
    while (node) {
        if (node->class_code == 0x0C && node->subclass == 0x03 && node->prog_if == 0x30) {
            return node;
        }
        node = node->next;
    }
    return null;
}

void xhci_pci_probe(void) {
    u32 found = 0;
    serial_outsl("xHCI: scanning PCI class 0x0C / sub 0x03 / prog-IF 0x30...");

    pci_device_t *node = pci_get_device(0x0C, 0x03);
    while (node) {
        if (node->class_code == 0x0C && node->subclass == 0x03 && node->prog_if == 0x30) {
            u32 bar_lo = xhci_pci_cfg(node->bus, node->slot, node->func, 0x10);
            u32 bar_hi = xhci_pci_cfg(node->bus, node->slot, node->func, 0x14);
            u64 bar0 = ((u64) bar_hi << 32) | (bar_lo & ~0xFu);

            serial_outsf("xHCI: %04x:%04x at %02x:%02x.%d, BAR0=0x%016llx\n",
                         node->vendor_id, node->device_id,
                         node->bus, node->slot, node->func,
                         bar0);
            found++;
        }
        node = node->next;
    }

    if (found == 0) {
        serial_outsl("xHCI: no controllers found.");
    } else {
        serial_outsf("xHCI: probe done, %d controller(s) listed, driver not yet bound.\n", found);
    }
}

void xhci_smoke_test_first(void) {
    pci_device_t *xhci = first_xhci_device();
    if (!xhci) {
        serial_outsl("xHCI: smoke-test skipped, no controller present.");
        return;
    }

    serial_outsf("xHCI: smoke-testing device at %02x:%02x.%d\n",
                 xhci->bus, xhci->slot, xhci->func);

    // 1. Enable bus mastering only (PCI COMMAND bit 2). MEM_ENABLE (bit 1)
    //    is usually already set by BIOS enumeration. IO_ENABLE (bit 0)
    //    must NOT be set: xHCI has no I/O BARs and enabling it activates
    //    I/O-cycle decode for the slot, which collides with everything
    //    else on the bus.
    //
    //    INTX_DISABLE (bit 10) is left at BIOS default (0). When Phase 4
    //    wires up MSI, we'll need to set it to 1 so legacy INTx and MSI
    //    don't double-deliver interrupts. For now no interrupts fire.
    u32 cmd = xhci_pci_cfg(xhci->bus, xhci->slot, xhci->func, 0x04);
    cmd |= 0x04;
    u32 cfg_addr = (u32) ((xhci->bus << 16) | (xhci->slot << 11) | (xhci->func << 8) | (0x04 | 0x80000000));
    outl(0xCF8, cfg_addr);
    outl(0xCFC, cmd);

    // 2. Resolve BAR0 64-bit address.
    u64 bar_lo_node = xhci_bar64_low(xhci->bus, xhci->slot, xhci->func, 0);
    u64 bar_hi_node = xhci_bar64_high(xhci->bus, xhci->slot, xhci->func, 0);
    if ((bar_lo_node & 1u) != 0) {
        serial_outsf("xHCI: BAR0 low bits 0x%llx — looks like I/O space, expected MMIO. Refusing.\n",
                     bar_lo_node & 0xFu);
        return;
    }
    u64 bar0_phys = (bar_hi_node << 32) | bar_lo_node;
    serial_outsf("xHCI: BAR0 = 0x%016llx\n", bar0_phys);

    // 3. Read BAR0 via the HHDM direct map. **Caveat**: Limine's HHDM
    //    maps *usable* RAM; PCI memory space (including this BAR) is often
    //    marked Reserved in the memory map and may not be present in the
    //    HHDM. If the read of CAPLENGTH returns all-ones or hangs, the
    //    BAR is not in HHDM — Phase 3 must add an explicit
    //    `vmm_mmio_map_phys(phys, size)` helper that walks pages and
    //    calls `vmm_map_page(phys, virt, PAGE_RW | PAGE_PCD | PAGE_PWT)`
    //    per 4 KB page, then do all MMIO access through that.
    //    For Phase 2's READ-ONLY smoke test we trust that the BAR
    //    address falls inside HHDM coverage and see what we get.
    u64 base_virt = bar0_phys + vmm_get_hhdm();
    serial_outsf("xHCI: BAR0 0x%016llx → virt 0x%016llx (HHDM; READ-ONLY smoke test)\n",
                 bar0_phys, base_virt);

    volatile u32 * const mmio = (volatile u32 *) base_virt;

    // 4. Read capability registers — fully read-only, no side effects.
    u8 caplength = (u8) (mmio[XHCI_CAPLENGTH / 4] & 0xFF);
    u16 hciversion = (u16) ((mmio[XHCI_HCIVERSION / 4] >> 16) & 0xFFFF);
    u32 dboff = mmio[XHCI_DBOFF / 4];
    u32 rtsoff = mmio[XHCI_RTSOFF / 4];
    u32 hccparams1 = mmio[XHCI_HCCPARAMS1 / 4];

    serial_outsf("xHCI: CAPLENGTH=0x%02x, HCIVERSION=0x%04x\n", caplength, hciversion);
    serial_outsf("xHCI: DBOFF=0x%08x, RTSOFF=0x%08x\n", dboff, rtsoff);
    serial_outsf("xHCI: HCCPARAMS1=0x%08x (AC64=%d CSZ=%d)\n",
                 hccparams1,
                 (hccparams1 >> 0) & 1,    // AC64
                 (hccparams1 >> 2) & 1);   // CSZ

    serial_outsl("xHCI: smoke-test OK. Driver not yet initialised beyond this point.");
}

// ── usb command: list connected USB devices ──────────────────────────────────
// Reads port status registers from the first xHCI controller.
// Reuses the same BAR0-via-HHDM approach as the smoke test.
// Prints to serial for the dashboard; also calls screen_push_linef
// so the output is visible on the kernel console.

static const char *xhci_port_speed_name(u8 speed) {
    switch (speed) {
        case 1: return "Full-speed (12 Mbps)";
        case 2: return "Low-speed (1.5 Mbps)";
        case 3: return "High-speed (480 Mbps)";
        case 4: return "SuperSpeed (5 Gbps)";
        default:
            if (speed >= 5 && speed <= 15) return "SuperSpeedPlus (10+ Gbps)";
            return "unknown";
    }
}

void xhci_list_devices(void) {
    pci_device_t *xhci = first_xhci_device();
    if (!xhci) {
        serial_outsl("usb: no xHCI controller found.");
        screen_push_line("usb: no xHCI controller found on PCI bus.");
        return;
    }

    serial_outsf("usb: scanning xHCI at %02x:%02x.%d\n",
                 xhci->bus, xhci->slot, xhci->func);

    // Map BAR0 through HHDM (same trust-the-HHDM approach as smoke test).
    u64 bar_lo_node = xhci_bar64_low(xhci->bus, xhci->slot, xhci->func, 0);
    u64 bar_hi_node = xhci_bar64_high(xhci->bus, xhci->slot, xhci->func, 0);
    if ((bar_lo_node & 1u) != 0) {
        serial_outsl("usb: BAR0 is I/O space, cannot map.");
        screen_push_line("usb: xHCI BAR0 is not MMIO (unexpected).");
        return;
    }
    u64 bar0_phys = (bar_hi_node << 32) | bar_lo_node;
    u64 base_virt = bar0_phys + vmm_get_hhdm();
    volatile u32 * const mmio = (volatile u32 *) base_virt;

    // Read CAPLENGTH to find the operational register base.
    u8 caplength = (u8) (mmio[XHCI_CAPLENGTH / 4] & 0xFF);
    volatile u32 * const op_mmio = (volatile u32 *) (base_virt + caplength);

    // HCSPARAMS1 bits 31:24 = MaxPorts (total physical ports on the HC).
    u32 hcsparams1 = mmio[XHCI_HCSPARAMS1 / 4];
    u32 max_ports = (hcsparams1 >> 24) & 0xFF;

    serial_outsf("usb: CAPLENGTH=0x%02x, MaxPorts=%d\n", caplength, max_ports);
    screen_push_linef("usb: xHCI cap regs ok, %d port(s)", max_ports);

    if (max_ports == 0) {
        screen_push_line("usb: controller reports 0 ports.");
        return;
    }

    u32 found = 0;
    // Port registers start at op-base + 0x400; each port is 0x10 bytes.
    // PORTSC is at offset 0x00 within each port's register block.
    const u32 port_reg_base  = 0x400;
    const u32 port_stride    = 0x10;

    screen_push_line("PORT  SPEED                     STATUS");
    screen_push_line("----  -----                     ------");
    serial_outsl("usb: connected devices:");

    for (u32 port = 0; port < max_ports; port++) {
        u32 port_off = (port_reg_base + port * port_stride) / 4;
        u32 portsc = op_mmio[port_off];

        bool ccs   = (portsc & XHCI_PORTSC_CCS) != 0;
        bool ped   = (portsc & XHCI_PORTSC_PED) != 0;
        bool pp    = (portsc & XHCI_PORTSC_PP) != 0;
        u8   speed = (u8) ((portsc >> XHCI_PORTSC_PORT_SPEED_SHIFT) & XHCI_PORTSC_PORT_SPEED_MASK);

        // Build a human-readable status string.
        const char *status;
        if (!pp) {
            status = "no power";
        } else if (!ccs) {
            status = "empty";
        } else if (!ped) {
            status = "connected (not enabled)";
        } else {
            status = "connected + enabled";
        }

        if (ccs) {
            serial_outsf("  port %d: speed=%d (%s), ped=%d\n",
                         port + 1, speed, xhci_port_speed_name(speed), ped);
            screen_push_linef("%-4d  %-24s  %s",
                              port + 1, xhci_port_speed_name(speed), status);
            found++;
        } else {
            // Still show empty ports at debug level so the user knows
            // how many ports the controller reports.
            serial_outsf("  port %d: %s (speed=%d, ped=%d, pp=%d)\n",
                         port + 1, status, speed, ped, pp);
        }
    }

    if (found == 0) {
        screen_push_line("(no devices connected)");
    }

    serial_outsf("usb: %d device(s) connected out of %d port(s).\n", found, max_ports);
    screen_push_linef("usb: %d device(s) connected.", found);
}
