// xHCI ring helpers — currently a stub.
//
// This file exists in the build today so the architecture is obvious from
// the beginning and so future commits can add doorbell + command-ring
// + event-ring code without doing a folder shake-up. Phase 3 of the
// USB plan fans this out.

#include "../../include/dialect.h"
#include "../../include/kern_xhci_regs.h"

// xHCI Transfer Request Block (TRB) — 16 bytes, four 32-bit fields. The
// high bit of the first field is the cycle-bit (CC) flag that the producer
// (kernel) flips when it overwrites the slot and the consumer (controller)
// reads to know whose version of the slot is current.
typedef struct xhci_trb {
    u32 q;        // low: parameter / pointer low (varies by type)
    u32 p_status; // type-specific status
    u32 c_high;   // control + pointer high
    u32 d_cc;     // high bit is Cycle Bit (CC); low bits = TRB type
} xhci_trb_t;

// Stub: compute the number of TRBs that fit in a ring of given size.
// Used by Phase 3 ring alloc; kept alongside for early-warning linkage.
u64 xhci_ring_count(u64 byte_size) {
    return byte_size / sizeof(xhci_trb_t);
}
