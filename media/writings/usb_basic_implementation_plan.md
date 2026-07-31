# USB Basic Implementation Plan: Keyboard and Mouse

**Status:** Design Exploration  
**Date:** October 2026  
**Target hardware:** Real i7-1165G7 (Tiger Lake) laptop with iGPU-only — no PS/2 controller on most modern laptop boards. QEMU 8/9 + `-machine q35 -device qemu-xhci -device usb-kbd -device usb-mouse` for daily iteration.

**TL;DR:** Bring up xHCI as the sole USB host controller driver — it covers every USB 3.x, 2.x, and 1.1 device on modern hardware, and QEMU emulates it cleanly. Use **HID boot protocol** (kicked on with one `SET_PROTOCOL=0`) so the kernel sees an **8-byte keyboard report** and a **3-byte mouse report** with no HID descriptor parsing. Layer it under the **existing** `kern_keyboard.c` API (`scancode_pressed[]`, `keyboard_eat_key()`, `keyboard_fg_push()`) so Doom, the shell, and WASM foreground readers keep working without modification. Total scope: **~1500 lines of new C**, achievable in a few focused weeks.

---

## 1. Why USB matters here

- The PS/2 controller (the i8042 we're currently polling at `inb(0x64)`, ISR on vector 33) is **dead silicon on most modern laptops**. The i7-1165G7 you're testing on either omits it entirely or wires it to a non-functional companion chip.
- Even when the i8042 is present, the laptop's *external* keyboard is USB (or USB-C), never PS/2, so you'd need an adapter that mostly doesn't exist anymore.
- The OS already runs a USB-aware module loader (the WASM executor) and Doom; both keep running fine on a USB-only laptop. It's just the input path that's stuck.

This is **fundamentally a tractable problem** — USB is a well-understood industry standard, the boot-protocol subset is small enough to fit in a day, and QEMU on Windows WHPX already exposes a faithful xHCI emulation. Unlike the iGPU question, USB+xHCI is something done regularly by hobby OS developers.

---

## 2. Hardware comparison (PS/2 today vs. USB target)

| Aspect | PS/2 (current state) | USB xHCI (target) |
|---|---|---|
| **Interface** | Port I/O (`inb 0x60`/`inb 0x64`) | MMIO (a single `xhci_base` map of several KB) |
| **Device discovery** | Static — i8042 is always there at boot | Enumeration: bus reset, `GET_DESCRIPTOR(8)`, `SET_ADDRESS`, `GET_DESCRIPTOR(18)`, `SET_CONFIGURATION` |
| **Reports** | 1-byte set-1 scancodes | 8-byte boot keyboard reports, 3-byte boot mouse reports |
| **Hot-plug** | N/A | Required — port status changes generate events |
| **Interrupt path** | IRQ vector 33 (IOAPIC remap of IRQ1) | One programmable MSI or legacy IRQ per xHCI controller |
| **Concurrency** | Single device, polled/interrupting | Many devices, multi-segment transfers across multiple ports |
| **Code (today's i8042 driver)** | ~370 lines | Target ~1500 lines |
| **New lines of C** | 0 | ~1500 (xHCI + HID + glue) |
| **Real-world device plumbing** | "Plug in a PS/2 keyboard" (increasingly no-op) | Plug in any USB keyboard, standard HID |

---

## 3. Decision: xHCI only — no UHCI/OHCI/EHCI

USB host controllers in chronological order:

| Class code | Prog-IF | Spec | Real hardware context |
|---|---|---|---|
| `0x0C03` | `0x00` | UHCI | Intel's USB 1.1 host (companion to ICH/PCH chipsets). Cheap, inb/outb. |
| `0x0C03` | `0x10` | OHCI | Everyone else's USB 1.1 host (Compaq et al). MMIO. |
| `0x0C03` | `0x20` | EHCI | USB 2.0 high-speed. MMIO, 1024-entry periodic frame list, reprogrammed periodically. |
| `0x0C03` | `0x30` | **xHCI** | USB 3.0+. Backwards-compatible with USB 2.0 and 1.1. MMIO, doorbells, command/event rings, TRBs. |

xHCI covers **everything** in the device tree of a modern host — it owns the ports, the controller has internal "companion" controllers for legacy USB 1.1 devices attached to USB 2.0 hubs (or xHCI native handles them), and is the only USB host controller on the i7-1165G7. UHCI/OHCI/EHCI exist only on legacy hardware older than the platform you're targeting.

**Recommendation: write xHCI only.** If we're ever on a pre-xHCI machine, use PS/2. There is no intermediate generation worth supporting — every 2010-era chipset already had xHCI and is faster than the i7-1165G7 anyway.

This is much nicer than the iGPU story, where we were stuck deciding between sub-drivers, generations, and partial support: USB has one ground-truth spec (xHCI 1.2, dated 2019, freely downloadable as a PDF from Intel), one firmware model, one descriptor model, and QEMU 7+ has a fully functional emulation of it.

---

## 4. The bits of xHCI the kernel has to know about

xHCI exposes a minimal register set plus three ring structures. The "rings" are the part that feels fresh compared to AHCI:

### 4.1 The MMIO layout

```
Offset      Size  Register / Range
0x0000      32    CAPS          (capability register, read-only)
                          CAPLENGTH at 0x00 (low byte tells you offset of operational base)
                          HCSVERS at 0x02 etc.
                          HCSPARAMS2 contains DBOFF (doorbell-array offset,
                          relative to capability base). QEMU's qemu-xhci
                          reports DBOFF = 0x4000, so the doorbell array sits
                          at offset CAPLENGTH + DBOFF (e.g. 0x4020 if CAPLENGTH = 0x20).
CAPLENGTH+DBOFF  —    Doorbell array (one 32-bit doorbell per device slot)
                —    Port Register Set (PORTSC 1, PORTSC 2, …)
                       each port is 16 bytes
```

We treat this as **MMIO via `vmm_map_phys()`** of the xHCI's BAR0 into kernel virtual space. Reads/writes go via pointer dereference with the usual `volatile u32 *` cast.

### 4.2 The "rings"

xHCI does not have on-chip command slots like AHCI does; instead the host describes commands and transfers as **Linked Lists of TRBs (Transfer Request Blocks)** that live in normal RAM. Each TRB is 16 bytes.

| Ring | Direction | Owner | TRB types we use |
|---|---|---|---|
| **Command Ring** | kernel → controller | Each TRB is one command: enable slot, address device, configure endpoint, etc. | Enable Slot, Address Device, Configure Endpoint, Evaluate Context, Reset Endpoint, Stop Endpoint, Set TR Dequeue |
| **Event Ring** | controller → kernel | Each TRB is one completion event: command complete, transfer complete, port change, etc. | Command Completion, Transfer Completion, Port Status Change |
| **Transfer Rings** (one per endpoint) | kernel → controller | Each TRB describes one chunk of data to move | Setup TRB (control), Data TRB (bulk/intr), Status TRB (control IN) |

The producer-consumer semantics are: the kernel writes into the ring via a write pointer it owns, the controller reads; the controller writes event ring TRBs, the kernel reads. A 64-bit Cycle Bit (CC) flag on each TRB tells both sides whose version of the ring is fresh.

### 4.3 Port status (PORTSC)

Each physical USB port has a 32-bit PORTSC register. The interesting bits:

- **CCS (bit 0)** — Current Connect Status. 1 = device attached.
- **PED (bit 1)** — Port Enabled. 1 = connection is up.
- **CSC (bit 17)** — Connect Status Change. 1 since last write — there's a new device (or it left).
- **PRC (bit 21)** — Port Reset Change. The reset we issued completed.

When CSC rises, we run a new enumeration round. When the enumeration succeeds, we move the slot through `ENABLE_SLOT → ADDRESS_DEVICE → SET_CONFIGURATION → SET_PROTOCOL(boot)`.

---

## 5. HID boot protocol — the easy mode

USB HID (Human Interface Devices) describes many ways to talk to a keyboard. Almost all boot-mode-capable devices support **two of them**:

| Mode | Header size | Useful when | Set with |
|---|---|---|---|
| **Boot protocol** | 8 bytes (kbd), 3 bytes (mouse) | Just need keys + mouse buttons + delta XY | `SET_PROTOCOL = 0` (control transfer) |
| **Report protocol** | Variable, defined by HID Report Descriptor | Fancy keyboards (NKRO, multimedia keys) | `SET_PROTOCOL = 1` |

**We use boot protocol.** Skip the descriptor parser entirely. Linux's HID layer still parses full HID reports because Linux must support the long tail of keyboards; we don't need that for our keyboard-emulating-in-QEMU reality.

### 5.1 The 8-byte boot keyboard report

| Byte | Meaning |
|---|---|
| 0 | Modifier bitmask: bit 0 = LCtrl, bit 1 = LShift, bit 2 = LAlt, bit 3 = LGUI, bits 4..7 = RCtrl/RShift/RAlt/RGUI |
| 1 | Reserved (always 0) |
| 2..7 | Up to 6 simultaneous "keycodes" — HID Usage IDs (0x04 = 'a', 0x05 = 'b', …, 0x1D = 'y', 0x28 = '\n', 0x29 = ESC, 0x2A = backspace, …, 0x4F = Right arrow). Reference: USB HID Usage Tables 1.5. |

Each report gives the full active state — which modifiers are held + which 6 keys are physically down *right now*. The kernel diffs the report against the previous one and synthesizes press/release events, exactly like translating PS/2 set-1 make/break codes.

### 5.2 The 3-byte boot mouse report

| Byte | Meaning |
|---|---|
| 0 | Button bitmask: bit 0 = Left, bit 1 = Right, bit 2 = Middle |
| 1 | X delta (signed 8-bit) |
| 2 | Y delta (signed 8-bit) |

The 4-byte form (`bInterfaceSubClass = 0x01` boot mouse with wheel, byte 3 = signed horizontal wheel delta) was added in HID 1.11; QEMU's `usb-mouse` does not emit it. No 5-byte boot mouse report is defined. We don't need wheels for v1.

---

## 6. Enumeration — what happens at the wire level

For a boot-protocol keyboard attached to a hub, the sequence after port-scanning detects a connection:

```
1.  [Host]  Port Reset on the affected port (PORTSC.PR = 1; wait for PRC = 1; clear)
2.  [Host]  GET_DESCRIPTOR(Device, wLength=8)
            Control transfer: 8-byte setup, 8-byte IN-data, status IN
            Returns the first 8 bytes of the Device descriptor: bLength=18,
            bcdUSB, bDeviceClass, bMaxPacketSize0, …
3.  [Host]  SET_ADDRESS (new_address)
            Control transfer: zero-data (status), Device transitions to that address
4.  [Host]  GET_DESCRIPTOR(Device, wLength=18)
            Now bMaxPacketSize0 is what we just learned; data stage must fit.
            Returns full 18-byte Device descriptor including idVendor, idProduct, bNumConfigurations.
5.  [Host]  GET_DESCRIPTOR(Configuration, wLength=config_total_length)
            Returns full configuration block, with one or more Interface descriptors inside.
6.  [Host]  GET_DESCRIPTOR(HID, wLength=9)         -- only if iInterface HID class
            Returns HID descriptor: bcdHID, bCountryCode, bNumDescriptors, …, bDescriptorType=REPORT.
6'. [Host]  GET_DESCRIPTOR(Report, wLength=huge)    -- optional; we skip this with boot protocol
7.  [Host]  SET_CONFIGURATION (config 1)
            Device is now in the active configuration.
8.  [Host]  SET_PROTOCOL (wValue=0)                 -- HID class request
            Device is now in boot protocol mode.
9.  [Host]  Issue an interrupt-IN transfer to the device's IN endpoint (typically 8 bytes / 8 ms).
10. [Dev]   Whenever the device has a report, it sends it on the IN endpoint.
11. [XHCI]  Generates a Transfer-Complete event on the Event Ring; ISR runs.
```

Step 9 is the "ongoing" part: after setup, the host queues a periodic interrupt transfer (e.g. bInterval = `0x08` = 8 frames = 8 ms) that asks the device "send me a report if you have one." The device replies as soon as a key is pressed. New `Transfer-Complete` events arrive in the event ring; we read them, decode the 8 bytes, diff against the last report, and feed the synthesized scancode events into `scancode_pressed[]` / `queue[]`.

All of the above travels as **USB control transfers** or **USB interrupt transfers**, never bulk or isochronous. For boot-protocol keyboards, we don't need isochronous, we don't need bulk, we don't get into streams.

---

## 7. QEMU dev-loop wiring

The full QEMU command line for daily keyboard/mouse iteration is:

```
qemu-system-x86_64 \
  -machine q35,accel=whpx \
  -smp 4 \
  -drive format=raw,file=sandfleaOS.iso \
  -device qemu-xhci,id=xhci \
  -device usb-kbd,bus=xhci.0 \
  -device usb-mouse,bus=xhci.0 \
  -display gtk
```

Notes:

- `qemu-xhci` is the modern native emulator. (`nec-usb-xhci` is the legacy Renesas µPD720200 emulation; both work but `qemu-xhci` is what newer QEMU defaults to.)
- `-usb` is **not** required on q35. It implicitly attaches a UHCI/EHCI pair, which we don't use.
- The QEMU `usb-kbd`/`usb-mouse` devices speak **boot protocol** out of the box, so our driver gets the easy 8-byte/3-byte reports without elaborate HID parsing.
- q35 already exposes an i8042 by default. We can leave PS/2 ISR wiring intact (it'll just never fire in this QEMU config with the i8042 emulator disabled) or wire a runtime check.
- The host's real USB keyboard reaches the guest through QEMU's display backend (`-display gtk` on Windows, or `-display sdl` / `-display curses`), which captures host input events and routes them into the emulated `usb-kbd`. This input plumbing is independent of `-accel tcg` vs `-accel whpx` (both forwards host input identically once the display backend has captured it). `-accel whpx` is only the CPU/hypervisor virtualization mode.

### 7.1 wr.bat changes

Today `wr.bat` ends with `-accel whpx`. Adding the USB devices is

```
-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-mouse,bus=xhci.0
```

We add this and leave PS/2 untouched — the i8042 stays usable and the OS gives 1 keyboard the user holds via either routing, with USB taking precedence once enumeration succeeds.

---

## 8. Bridging into the existing keyboard consumer

This is the bit that makes the migration cheap. Today we have:

```
[cust from doom/etc]
   │
   ▼
scancode_pressed[sc], scancode_edge_down[], keyboard_eat_key(), keyboard_fg_push()
   ↑
keyboard_handle_keypress() — IRQ1 ISR (vector 33) — reads port 0x60, decodes set-1 scancodes
```

USB gives us **HID Usage IDs**, not set-1 scancodes. The mapping the kernel must provide is roughly:

| Set-1 (PS/2) | HID Usage |
|---|---|
| `0x1E` ('a') | `0x04` (Keyboard a) |
| `0x0E` (backspace) | `0x2A` (Keyboard Backspace / "Delete Backspace") |
| `0x1C` (Enter) | `0x28` (Keyboard Return) |
| `0x0F` (Tab) | `0x2B` (Keyboard Tab) |
| `0x48` (Up arrow, E0-extended) | `0x52` (Keyboard UpArrow) |
| `0x2A` (Left Shift — make code, not to be confused with HID 0x2A above) | `0xE1` (Keyboard LeftShift) |
| `0x1D` (Left Ctrl — make code) | `0xE0` (Keyboard LeftCtrl) |

Translating once, then **all upstream code (Doom, the shell, the foreground queue) keeps speaking the same set-1 dialect**. This is a deliberate choice — let the HID driver act as a translator, not a parallel reimplementation.

### 8.1 The translation site

In `kern_usb_hid.c` (new file):

```c
// Map HID Usage (top 16 bits of u16) → set-1 scancode
// Map HID modifier byte → set-1 modifier flags
// Diff current 8-byte report against previous report, synthesize press/release.
```

The synthetic press/release then runs the **same code path** as `keyboard_handle_keypress`:

```c
// In kern_keyboard.c, factored out:
void kbd_inject_scancode_set1(u8 sc, bool is_extended, bool down);
// existing ISR calls this with what came from port 0x60;
// new USB HID driver calls this with what we derived from the boot report.
```

That shared injection function is the bridging point.

---

## 9. The data flow at runtime

```
[QEMU usb-kbd device]
      │  interrupt-in TRB every ~8 ms
      ▼
[XHCI controller — PORTSC.CCS, Transfer Complete Event]
      │
      ▼
[xhci_event_ring_isr() in kern_xhci.c]
      │  decoded into one "report received"
      ▼
[kbd_hid_poll() in kern_usb_hid.c]
      │  diff against previous report, synthesize set-1 make/break events
      ▼
[kbd_inject_scancode_set1(sc, ext, down)]
      │
      ├─► scancode_pressed[sc] = down
      ├─► scancode_edge_down[sc] = down
      └─► queue[queue_write_ptr++] = ascii  (with shift/ctrl applied)
      │
      ▼
[keyboard_eat_key()] / [keyboard_fg_push()] / doom input loop
```

This is clean and is the model other small hobby OSes ship — there's no user-space involvement, both interrupts and the foreground WASM process read from the same global queue.

---

## 10. Pain points

You'll trip on these as you write the driver. Each one corresponds to a known QEMU behavior I want you to know in advance:

### 10.1 The xHCI spec is huge

The spec is freely downloadable (xHCI 1.2: ~660 pages). You do not need most of it. The subset you need:

- §5 (Memory-Mapped I/O register layout)
- §4 (Data structures: TRBs, contexts, slots)
- §6.4 (Device Slot initialization / address-device command)
- §6.2 (Command Ring)
- §6.5 (Event Ring)

That's about 60 pages. The rest is for streams, isochronous, USB 3.0 bulk protocols — none of which we need for keyboard+mouse boot protocol.

### 10.2 Doorbell write-then-wait race

When you want a slot to do work, you write to a doorbell register: `mmio_write32(xhci_base + 0x4000 + slot_id * 4, 0);` (doorbell value 0 means "process what's at the command ring's enqueue pointer"). The doorbell write isn't synchronous in the way we'd like — there's no completion signal. The hardware polls the command ring's dequeue pointer. The trick is to make sure the ring is fully written (cache-coherent if applicable) **before** you ring the doorbell. On x86 MMIO is usually strongly ordered, but on other architectures you may need a `wmb()` or `sfence`-style barrier.

### 10.3 Transfer TRBs and multi-segment

Interrupt transfers have very small payloads (8 bytes for keyboards), so you can fit one TRB per transfer. **Don't reuse that assumption for bulk or control transfers** — those can have multiple data stages, and you need a "chain" of TRBs linking Data TRBs to a final Status TRB. Get that wrong and a single endpoint setup can wedge the slot forever.

### 10.4 Port reset and connection debouncing

A USB cable insertion generates a `CSC` (Connect Status Change) event. A removal also generates one. Some devices emit two CSC events in quick succession (electrical debouncing). The driver should:

- On CSC=1, write `0xF → PORTSC` to clear change bits **and** trigger a port reset by setting `PR` bit followed by clear-after-PRC
- Wait for PRC=1 (poll or wait)
- After successful reset, kick off enumeration

If you reset the port before the device is ready, you'll see "PORT_NOT_CONNECTED" indefinitely and the slot will never complete enumeration. The USB 2.0 spec §9.1.2 defines `tDRSMRSUP`-style debounce timings (typically 20–50 ms after a stable VBUS-current assertion and before issuing the warm port reset). Wait for CCS to be stably set, then issue the port reset.

### 10.5 MSI vs legacy IRQ for xHCI

Modern QEMU xHCI advertises MSI-capable and we should **prefer MSI** in the kernel — register a vector for the xHCI controller and route the interrupt there. If MSI is unavailable (rare, like an ancient chipset), the legacy IRQ is hardcoded in PCI config (typically IRQ 11 → vector 27 with our IOAPIC remap scheme).

### 10.6 Boot protocol vs report protocol mid-session toggle

If a user unplugs the boot-protocol keyboard and plugs in a fancy report-protocol one, the device on the *same enumeration slot* may not support boot protocol. The driver should just give up on that one port — we don't need to support NKRO or media keys.

### 10.7 Mouse and absolute vs relative

QEMU's `usb-mouse` (relative) is the easy target. Avoid `usb-tablet` (absolute) for now. Doom's WASM input is already key+mouse, so absolute coords aren't interesting.

### 10.8 Interrupt moderation vs input latency

xHCI uses the `IMOD` (Interrupt Moderation) register, whose units are **125 µs ticks**, on a per-interrupt basis.

- **IMOD = 0** → fire an interrupt on **every event** (lowest latency, but floods the IRQ — IRQ has nothing to do but signal completion of one event at a time).
- **Higher IMOD values** → batch events and fire less often (lower IRQ pressure, but events wait longer before reaching the driver).

**Start with IMOD = 32** (= 32 × 125 µs = **4000 µs = 4 ms**) which buckets events into 4 ms windows without any perceptible input lag. If you want minimal latency for a competitive game, drop to **IMOD = 8** (1 ms); below that, the IRQ has nothing to do.

---

## 11. Concrete file plan

```
src/kernel/kern_xhci.c       — xHCI driver (init, port reset, command rings, ISRs)
src/kernel/kern_xhci_rings.c — ring helpers (doorbell write, event-ring walking)
src/kernel/kern_usb_hid.c    — HID class driver (boot-protocol decoder, set-1 translator)
src/include/kern_xhci.h      — xHCI public API
src/include/kern_xhci_regs.h — register offsets, TRB definitions
src/include/kern_usb_hid.h   — public entrypoint
build/lib.sh                 — adds kern_xhci.c, kern_xhci_rings.c, kern_usb_hid.c
wr.bat                       — adds the -device usb-kbd/usb-mouse flags
```

We do **not** add a third transport to `kern_keyboard.c`. Instead, `kern_xhci.c` and `kern_keyboard.c` both end up calling into `kbd_inject_scancode_set1()`. Adding an `extern u8 active_keyboard_source` to flag "this came from USB" vs "this came from PS/2" is fine but optional.

---

## 12. Phased plan with rough line counts

| Phase | Deliverable | New lines | Days |
|---|---|---|---|
| **0** | Factor `kbd_inject_scancode_set1()` out of `kern_keyboard.c`; both transports call it. | -50, +30 | 1 |
| **1** | PCI probe of xHCI controllers (class `0x0C`, subclass `0x03`, prog-if `0x30`); enable bus-mastering, map BAR0. | +120 | 1 |
| **2** | xHCI MMIO config — basic init, set up device slot count, ribbon off Run/Stop, reset. | +250 | 1–2 |
| **3** | Command Ring + Doorbell. Implement `Enable_Slot` and acknowledge the resulting Command Complete event on the Event Ring. ISR is mostly empty here. | +200 | 2 |
| **4** | PORTSC handling. Detect CSC, drive port reset, wait for PED. Generate `Enable_Slot` on a port that just enabled. | +150 | 1 |
| **5** | Address_Device + Configure_Endpoint command sequence. Worst-of-5x TRBs for the device context, input context, slot state, endpoint state. | +300 | 2 |
| **6** | Control transfers via Setup/Data/Status TRB chains. Implement `GET_DESCRIPTOR`, `SET_ADDRESS`, `SET_CONFIGURATION`, `SET_PROTOCOL`. | +250 | 2 |
| **7** | HID boot-protocol diff-and-inject pipeline. Translate to set-1 scancodes, feed into `kbd_inject_scancode_set1`. | +220 | 1–2 |
| **8** | BIOS-level segment / reconnection / removal handling, including detach + re-enumerate. | +100 | 1 |
| **9** | Mouse: 3-byte report, same slot/transfer logic, separate queue. | +150 | 1 |
| **10** | MSI setup, IMOD tuning, IRQ-fast-path profiling. | +80 | 1 |
| **Total** | | **~1500 lines C** | **~2–3 weeks** |

This estimate lines up well with public hobby OS efforts (SerenityOS's first USB stack was on the order of 5000 LOC for the full driver including isochronous + bulk STREAMs; a boot-protocol-only keyboard+mouse driver is much smaller).

---

## 13. Sub-deliverable ordering

Within Phase 12's first 7 entries:

1. **Phase 0 (today)** — factor `kbd_inject_scancode_set1` first so that the existing PS/2 path can be A/B tested against itself. End-to-end nothing changes visually, but you cut the future migration risk.
2. **Phase 3 → Phase 7** — the path from "have a ring infrastructure" to "USB keyboard reports become ASCCII", in that order. Don't try to do HID + transfer rings in parallel; the rings are the hard part.
3. **Phase 9 (mouse)** after the keyboard works — the code is largely the same; the report interpretation differs. Get keyboard debugged first.

QEMU boots fast (~1 second from cold), so each phase should be a fresh test.

### 13.1 How to know a phase works

- **Phase 3**: `xhci>` prints "XHCI: 1 slot allocated, Event Ring ready."
- **Phase 4**: When you plug a `usb-kbd` into the QEMU VM, you see "PORTSC#1: connect detected, port reset done."
- **Phase 5**: After port reset, you see "Slot 1 enabled, Address_Device ok."
- **Phase 6**: After that, "GET_DESCRIPTOR returned 18 bytes. bDeviceClass=0x00, bcdUSB=0x0200."
- **Phase 7**: A keystroke on the QEMU host keyboard surfaces as a char in the VirtIO shell prompt.
- **Phase 9**: `move +0,+0` (`usb-mouse` via QEMU mouse pad) shows mouse deltas flowing.

---

## 14. What this enables

Once USB keyboard+mouse work:

- **Real-laptop boot.** No more "this only works when QEMU emulates PS/2".
- **Hot-plug.** Pull the keyboard and replug — the driver under enumeration picks it up.
- **Doom input.** Scrolling rings, fire, etc route to the WASM Doom module via the same `kbd_inject` path.
- **The CLI prompt in WASM apps.** Any WASM app that wants stdin from a real keyboard just keeps using `keyboard_fg_eat()` and `scancode_pressed[]` — no change required.
- **Pathway to other HID.** Adding a USB audio class driver later, or gamepads, becomes possible: the xHCI layer is the bottleneck and it's reusable.

---

## 15. What this does NOT bring (and that's okay)

- **Mice that aren't boot-protocol** (some "gaming" mice). We don't care for now.
- **HID report descriptors** (NKRO rollover, media keys, keyboard backlight control). Out of scope.
- **USB hubs.** The QEMU config attaches the kbd directly. Real hardware has hubs; once QEMU hub testing is added in pass 2, they're a single segment of enumeration logic.
- **USB storage class**, **USB audio**, **USB video class**, **USB CDC**. Not touched.
- **Bluetooth.** Out of scope; that's a separate driver stack entirely.

These are not regrets — they are decisions. Each can be added later by writing a class driver that sits on top of the same xHCI infrastructure.

---

## 16. Summary

| Decision | What | Why |
|---|---|---|
| **Host controller** | xHCI only (`0x0C03/0x30`) | Covers all modern hardware; UHCI/OHCI/EHCI exist only on pre-xHCI machines where we'd use PS/2 |
| **HID mode** | Boot protocol | Fixed report size, no descriptor parsing |
| **Existing API** | Keep & factor | All Doom/shell/WASM keeps working without code change |
| **Translation site** | HID Usage → set-1 scancode | Driver injects at the same point as the i8042 ISR |
| **QEMU dev** | `-device qemu-xhci -device usb-kbd -device usb-mouse` on q35/WHPX | Faithful hardware emulation, fast iteration |
| **Phases** | ~10 phases, ~1500 LOC, ~2–3 weeks | Each phase testable in QEMU in seconds |
| **Lock-in risk** | Near zero | PS/2 path stays in place; we just add a secondary source |

**Next concrete step:** factor `kbd_inject_scancode_set1()` (Phase 0) tomorrow. This is one tiny refactor and sets up every later phase.
