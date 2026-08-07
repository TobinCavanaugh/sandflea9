// xHCI ring helpers — doorbell writes, command-ring setup, event-ring
// walking, TRB enqueue/dequeue, and physical-memory ring allocation.
//
// Phase 3+ of media/writings/usb_basic_implementation_plan.md.

#include "../../include/dialect.h"
#include "../../include/kern_xhci_regs.h"
#include "../../include/kern_vmm.h"
#include "../../include/kern_mem.h"
#include "../../include/kern_serial.h"

// ── Ring state ───────────────────────────────────────────────────────────────

// Command Ring state
static volatile xhci_trb_t *cmd_ring;        // virtual address of command ring
static u64                    cmd_ring_phys;  // physical base of command ring
static u32                    cmd_enqueue;    // next TRB index to write
static u8                     cmd_ccs;        // current cycle state (0 or 1)

// Event Ring state
static volatile xhci_trb_t *event_ring;      // virtual address of event ring
static u64                    event_ring_phys; // physical base of event ring
static u32                    event_dequeue;  // next TRB index to read
static u8                     event_ccs;      // current cycle state

// Per-device transfer ring state (up to 8 slots)
// Each slot gets one transfer ring for EP0 controls.
// Interrupt endpoints get a separate ring per slot.
#define MAX_TRANSFER_RINGS      8
volatile xhci_trb_t *transfer_ring[MAX_TRANSFER_RINGS];
u64                    transfer_ring_phys_stored[MAX_TRANSFER_RINGS];
u32                    transfer_enqueue[MAX_TRANSFER_RINGS];
u8                     transfer_ccs[MAX_TRANSFER_RINGS];

// ── DMA-capable allocation ──────────────────────────────────────────────────
// Allocate a physically-contiguous page for DMA (rings, device contexts).
// Returns virtual address via HHDM. Stores physical address in *phys_out.
// The caller gets both addresses without needing page-table walks.

static u64 dma_alloc_page(u64 *phys_out) {
    u64 phys = pmm_alloc_page();
    if (!phys) return 0;
    *phys_out = phys;
    return phys + vmm_get_hhdm();
}

// Allocate a zeroed DMA buffer of arbitrary size.
// Uses kmalloc for virtual access but we derive the physical address
// via HHDM since kmalloc pages are backed by pmm_alloc_page() and
// are physically contiguous within each page.
static void *dma_alloc(u64 size, u64 *phys_out) {
    if (size == 0) return null;
    // For sizes <= PAGE_SIZE, use a single physical page.
    // For larger sizes, we'd need scatter-gather, but we only need
    // small DMA buffers (input contexts, device contexts).
    if (size <= PAGE_SIZE) {
        u64 phys = pmm_alloc_page();
        if (!phys) return null;
        *phys_out = phys;
        void *virt = (void *)(phys + vmm_get_hhdm());
        mem_set(virt, 0, size);
        return virt;
    }
    return null; // TODO: multi-page DMA allocations
}

// ── Ring helpers ─────────────────────────────────────────────────────────────

// Initialise a ring: cycle bits set to the OPPOSITE of the producer's CCS.
// The producer starts with CCS=1 and writes TRBs with bit=1. The controller
// starts with CCS=1 and flips processed TRBs to 0. The check in cmd_ring_enqueue
// compares next slot's bit against cmd_ccs to detect "ring full".
static void ring_init_full(volatile xhci_trb_t *ring, u32 count, u8 init_ccs, u64 ring_phys_base) {
    u8 init_bit = init_ccs ? 0 : 1;  // opposite of producer CCS
    for (u32 i = 0; i < count - 1; i++) {
        ring[i].parameter = 0;
        ring[i].status     = 0;
        ring[i].control    = init_bit;
    }
    // Last entry: Link TRB pointing back to ring start.
    // The cycle bit MUST be the producer's CCS so the consumer chases the link.
    // TRB_TC (Toggle Cycle) tells the controller to flip its CCS after chasing.
    ring[count - 1].parameter = ring_phys_base;
    ring[count - 1].status    = 0;
    ring[count - 1].control   = (TRB_TYPE_LINK << TRB_TYPE_SHIFT) | TRB_CH | TRB_TC
                              | (init_ccs ? 1 : 0);
}

// Write a TRB to the command ring and advance the enqueue pointer.
// The last TRB slot is a Link TRB — we skip over it when advancing.
// `control_dword` is the fully-formed bits 0-31 of the TRB control field
// (TRB type already at bits 10:15, slot ID at bits 24:31 for slot-scoped commands).
static bool cmd_ring_enqueue(u64 param, u32 status, u32 control_dword) {
    u32 next = (cmd_enqueue + 1) % XHCI_CMD_RING_TRBS;
    if (next == XHCI_CMD_RING_TRBS - 1) next = 0;  // skip Link TRB slot
    if ((cmd_ring[next].control & 1) == cmd_ccs) {
        serial_outsl("xHCI: command ring full!");
        return false;
    }
    cmd_ring[cmd_enqueue].parameter = param;
    cmd_ring[cmd_enqueue].status    = status;
    // Preserve all caller-provided control bits except cycle (bit 0),
    // which we set from cmd_ccs.
    cmd_ring[cmd_enqueue].control   = (control_dword & ~1u) | cmd_ccs;
    cmd_enqueue = next;
    return true;
}

// Ring the doorbell for a given slot + target (endpoint).
void xhci_doorbell(u64 db_base, u32 slot_id, u8 target, u8 stream_id) {
    u32 value = (stream_id << 16) | target;
    volatile u32 *db = (volatile u32 *)(db_base + XHCI_DOORBELL(slot_id));
    *db = value;
}

// ── Initialisation ──────────────────────────────────────────────────────────

// Set up the command ring: allocate one page, fill with Link TRB at the end.
u64 xhci_cmd_ring_init(volatile u32 *op_regs) {
    u64 phys;
    u64 virt = dma_alloc_page(&phys);
    if (!virt) return 0;

    cmd_ring      = (volatile xhci_trb_t *)virt;
    cmd_ring_phys = phys;
    cmd_enqueue   = 0;
    cmd_ccs       = 1;

    // All TRBs owned by controller (ccs=1), Link TRB at the end.
    ring_init_full(cmd_ring, XHCI_CMD_RING_TRBS, 1, phys);

    // Write CRCR: physical address of command ring + RCS=1
    op_regs[XHCI_CRCR_LO / 4] = (u32)(phys & 0xFFFFFFFF) | XHCI_CRCR_RCS;
    op_regs[XHCI_CRCR_HI / 4] = (u32)(phys >> 32);

    serial_outsf("xHCI: command ring at phys 0x%016llx\n", phys);
    return virt;
}

// Set up the event ring: allocate ERST entry page + event ring page.
u64 xhci_event_ring_init(volatile u32 *rt_regs) {
    // Allocate Event Ring Segment Table (one entry)
    u64 erst_phys;
    u64 erst_virt = dma_alloc_page(&erst_phys);
    if (!erst_virt) return 0;
    xhci_erst_entry_t *erst = (xhci_erst_entry_t *)erst_virt;

    // Allocate Event Ring segment
    u64 ring_phys;
    u64 ring_virt = dma_alloc_page(&ring_phys);
    if (!ring_virt) return 0;

    // Fill ERST entry
    erst[0].ring_base = ring_phys;
    erst[0].ring_size = XHCI_EVENT_RING_TRBS;
    erst[0].reserved  = 0;

    // Clear event ring with Link TRB at end
    event_ring      = (volatile xhci_trb_t *)ring_virt;
    event_ring_phys = ring_phys;
    event_dequeue   = 0;
    event_ccs       = 1;
    // Hardware is the producer (starts CCS=1), so init with bit=0 (opposite)
    // so empty slots don't match software consumer's event_ccs=1.
    ring_init_full(event_ring, XHCI_EVENT_RING_TRBS, 1, ring_phys);

    // Program interrupter 0
    rt_regs[XHCI_ERSTSZ(0)    / 4] = 1;
    rt_regs[XHCI_ERSTBA_LO(0) / 4] = (u32)(erst_phys & 0xFFFFFFFF);
    rt_regs[XHCI_ERSTBA_HI(0) / 4] = (u32)(erst_phys >> 32);
    // ERDP: point to the physical base of the event ring
    rt_regs[XHCI_ERDP_LO(0)   / 4] = (u32)(ring_phys & 0xFFFFFFFF) | (1u << 3);  // EHB
    rt_regs[XHCI_ERDP_HI(0)   / 4] = (u32)(ring_phys >> 32);

    // Enable interrupts
    rt_regs[XHCI_IMAN(0) / 4] = XHCI_IMAN_IE;
    rt_regs[XHCI_IMOD(0) / 4] = 32;   // ~4ms moderation

    serial_outsf("xHCI: event ring at phys 0x%016llx\n", ring_phys);
    return ring_virt;
}

// Allocate a transfer ring for a device slot's endpoint.
// Stores ring info in the per-slot arrays.
//
// The ring is mapped UNCACHEABLE (UC) via a dedicated MMIO-style mapping
// so that CPU writes are immediately visible to the DMA controller.
// Without this, QEMU TCG silently keeps writes in its emulated cache
// and the xHCI controller sees stale TRBs → CC_RING_UNDERRUN.
//
// Importantly we do NOT create a second WB mapping of the same physical
// page — conflicting cache attributes for the same frame is UB per the
// Intel manual.
u64 xhci_transfer_ring_init(u32 slot_id) {
    if (slot_id >= MAX_TRANSFER_RINGS) return 0;

    u64 phys = pmm_alloc_page();
    if (!phys) return 0;

    // Single uncacheable mapping — no WB alias.
    u64 virt_uc = vmm_mmio_map_phys(phys, PAGE_SIZE);
    if (!virt_uc) { pmm_free(phys); return 0; }

    // Zero the page through the UC mapping.
    mem_set((void *)virt_uc, 0, PAGE_SIZE);

    transfer_ring[slot_id]          = (volatile xhci_trb_t *)virt_uc;
    transfer_ring_phys_stored[slot_id] = phys;
    transfer_enqueue[slot_id]       = 0;
    transfer_ccs[slot_id]           = 1;

    // Fill with cycle bit=0 (opposite DCS=1).  When the controller
    // speculatively reads these during EP0 init it sees a mismatch and
    // generates CC_RING_UNDERRUN, which we consume via the post-Address-Device
    // flush.  By the time we post real TRBs (cycle bit=1) the ring is clean.
    //
    // We deliberately do NOT use No-Op TRBs — if the controller silently
    // consumes a No-Op it advances the dequeue past our Setup TRB, and the
    // Data TRB runs without a SETUP packet → device never responds → hang.
    volatile xhci_trb_t *tr = transfer_ring[slot_id];
    for (u32 i = 0; i < XHCI_TRANSFER_RING_TRBS - 1; i++) {
        tr[i].parameter = 0;
        tr[i].status    = 0;
        tr[i].control   = 0;   // cycle bit = 0, no type → controller rejects
    }
    // Link TRB at the end: cycle bit = 1, points back to start.
    tr[XHCI_TRANSFER_RING_TRBS - 1].parameter = phys;
    tr[XHCI_TRANSFER_RING_TRBS - 1].status    = 0;
    tr[XHCI_TRANSFER_RING_TRBS - 1].control   = (TRB_TYPE_LINK << TRB_TYPE_SHIFT)
                                              | TRB_CH | TRB_TC | 1;

    return phys;
}

// ── Command submission ──────────────────────────────────────────────────────

// Post a command TRB to the command ring and ring the doorbell.
// `slot_id` is 0 for Enable Slot, or the device slot for Address Device etc.
// The slot ID is embedded in bits 24:31 of the TRB control dword.
bool xhci_post_command(u64 db_base, u64 param, u32 status, u32 trb_type, u32 slot_id) {
    u32 control = (trb_type << TRB_TYPE_SHIFT) | ((slot_id & 0xFF) << 24);
    if (!cmd_ring_enqueue(param, status, control))
        return false;
    serial_outsf("xHCI: POST type=%d slot=%d parm=0x%016llx stat=0x%08x ctrl=0x%08x\n",
                 trb_type, slot_id, param, status, control);
    asm volatile("mfence" ::: "memory");
    { volatile u32 _d = cmd_ring[cmd_enqueue].control; (void)_d; }
    xhci_doorbell(db_base, 0, 0, 0);
    return true;
}

// ── Event Ring reader ────────────────────────────────────────────────────────

// Read the next event from the event ring. Returns true if an event was
// available. Advances the dequeue pointer and writes back the full
// physical address to ERDP.
bool xhci_event_dequeue_next(volatile u32 *rt_regs, xhci_trb_t *out_trb) {
    volatile xhci_trb_t *evt = &event_ring[event_dequeue];

    if ((evt->control & 1) != event_ccs)
        return false;

    *out_trb = *evt;
    event_dequeue = (event_dequeue + 1) % XHCI_EVENT_RING_TRBS;
    if (event_dequeue == 0)
        event_ccs ^= 1;

    // Write back full physical address of the dequeue pointer
    u64 erdp_phys = event_ring_phys + event_dequeue * sizeof(xhci_trb_t);
    rt_regs[XHCI_ERDP_LO(0) / 4] = (u32)(erdp_phys & 0xFFFFFFFF) | (1u << 3);  // EHB clear
    rt_regs[XHCI_ERDP_HI(0) / 4] = (u32)(erdp_phys >> 32);

    return true;
}

// ── Transfer ring helpers ────────────────────────────────────────────────────

u64 xhci_transfer_ring_phys(u32 slot_id) {
    if (slot_id >= MAX_TRANSFER_RINGS) return 0;
    return transfer_ring_phys_stored[slot_id];
}

// ── Device context helpers ──────────────────────────────────────────────────

static xhci_device_ctx_t *device_contexts[MAX_TRANSFER_RINGS] = {0};
static u64                 device_contexts_phys[MAX_TRANSFER_RINGS] = {0};
static xhci_input_ctx_t   *input_contexts[MAX_TRANSFER_RINGS] = {0};
static u64                 input_contexts_phys[MAX_TRANSFER_RINGS] = {0};
static u64                 dcbaa_phys = 0;
static u64                *dcbaa_virt = 0;

// Allocate the DCBAA + device contexts.
// Uses dma_alloc so physical addresses are directly available.
u64 xhci_dcbaa_init(volatile u32 *op_regs, u32 max_slots) {
    // DCBAA: array of 64-bit pointers, one per slot
    u64 phys;
    u64 virt = dma_alloc_page(&phys);
    if (!virt) return 0;
    dcbaa_phys = phys;
    dcbaa_virt = (u64 *)virt;
    mem_set((u8 *)dcbaa_virt, 0, PAGE_SIZE);

    // Allocate device context + input context for each slot
    for (u32 i = 1; i <= max_slots && i < MAX_TRANSFER_RINGS; i++) {
        u64 dctx_phys, ictx_phys;
        xhci_device_ctx_t *dctx = (xhci_device_ctx_t *)dma_alloc(sizeof(xhci_device_ctx_t), &dctx_phys);
        xhci_input_ctx_t   *ictx = (xhci_input_ctx_t *)dma_alloc(sizeof(xhci_input_ctx_t), &ictx_phys);
        if (!dctx || !ictx) {
            serial_outsf("xHCI: OOM allocating device context for slot %d\n", i);
            return 0;
        }
        device_contexts[i]      = dctx;
        device_contexts_phys[i] = dctx_phys;
        input_contexts[i]       = ictx;
        input_contexts_phys[i]  = ictx_phys;
        dcbaa_virt[i] = dctx_phys;
    }

    op_regs[XHCI_DCBAAP_LO / 4] = (u32)(dcbaa_phys & 0xFFFFFFFF);
    op_regs[XHCI_DCBAAP_HI / 4] = (u32)(dcbaa_phys >> 32);

    serial_outsf("xHCI: DCBAA at phys 0x%016llx, %d slots\n", dcbaa_phys, max_slots);
    return virt;
}

xhci_device_ctx_t *xhci_device_ctx(u32 slot_id) {
    if (slot_id >= MAX_TRANSFER_RINGS) return 0;
    return device_contexts[slot_id];
}

xhci_input_ctx_t *xhci_input_ctx(u32 slot_id) {
    if (slot_id >= MAX_TRANSFER_RINGS) return 0;
    return input_contexts[slot_id];
}

// Return the physical address of an input context (for Address Device cmd).
u64 xhci_input_ctx_phys(u32 slot_id) {
    if (slot_id >= MAX_TRANSFER_RINGS) return 0;
    return input_contexts_phys[slot_id];
}

u8 xhci_port_speed_to_slot_speed(u8 port_speed) {
    switch (port_speed) {
        case 1: return SLOT_SPEED_FULL;
        case 2: return SLOT_SPEED_LOW;
        case 3: return SLOT_SPEED_HIGH;
        case 4: return SLOT_SPEED_SUPER;
        default: return SLOT_SPEED_HIGH;
    }
}
