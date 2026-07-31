# Dashboard-as-OS-Test-Harness Plan

**Status:** Roadmap / Design Exploration  
**Date:** October 2026  
**Target hardware:** ANY host that can run QEMU on Windows. The harness makes the laptop-or-desktop appear to the OS as a thing it can be poked, screenshotted, and given USB devices to.

**TL;DR:** Two new transport primitives between a browser tab and QEMU: **noVNC for the frame stream**, **QEMU QMP for control** (sendkey, device_add, device_del, screendump). QEMU does the rest. Keys ship over QEMU's USB-emulated keyboard so the sandfleaOS xHCI driver is the only path from "browser keydown" to "shell char" — no PS/2 shortcut. A device-tree checkbox panel in the browser lets you hotplug/pre-unplug devices; one click = one QMP `device_add`. I've refactored the dashboard HTML out of the Python string literal, added a sibling `harness.py` server, and put most of the UI scaffolding in `harness.html` so this can grow without `print()` debugging the world.

---

## 1. Why a harness, why now

Three concrete payoffs:

1. **Expose the OS to anyone with a browser.** Today, sharing what sandfleaOS does means shipping the ISO and pointing someone at wr.bat. With a harness, anyone with the dashboard URL sees a live OS session in their browser tab and types into it.
2. **Make testing repeatable.** Hot-unplug the keyboard between two runs and observe whether the OS's "no input device" path works. Press a key exactly 50ms after port reset and see if enumeration survives. You can do all of this in a scriptable harness; you can't do any of it reliably by hand on a bare QEMU window.
3. **Test the xHCI path end-to-end through QEMU's USB emulation.** Today the keyboard → kbd_inject path skips QEMU's USB HID emulation, so any failure that lives in the xHCI driver itself (Phase 3+ of the USB plan) is invisible. With sendkey routed through QEMU's USB keyboard device, every keypress exercises the rings, the descriptors, and the boot-protocol parser. The first bug your future xHCI code ships will be in browser, not in a hand-rigged test.

---

## 2. Build status before this work

| Component | Status | Notes |
|---|---|---|
| `dashboard/log_server.py` | working | real-time serial log streaming via SSE, three channels, d3 flame graph |
| `dashboard/run.bat` | working | launches `log_server.py` on `localhost:8079` |
| `wr.bat` | working | QEMU with `-display gtk` (windowed) and serial file logging |
| `media/writings/usb_basic_implementation_plan.md` | working | xHCI driver 10-phase roadmap, ~1500 LOC when complete |

What's missing in the build: a way to *poke* the running OS without being at the physical screen.

---

## 3. Architecture of the harness

```
┌──────────────────────────────────┐     HTTP / WS     ┌────────────────────────────────┐
│ Browser tab (dashboard)          │──────────────────▶│ dashboard/harness.py            │
│  /                                │                   │  - serves /harness HTML         │
│  ├─ noVNC canvas (frames)         │   POST json       │  - QMP client to QEMU           │
│  ├─ USB device tree (checkboxes)  │──────────────────▶│  - WebSocket→VNC bridge         │
│  ├─ Status bar (PORTSC, slot #)   │                   │  - File watcher for serial log  │
│  ├─ Keyboard event capture        │                   └────────────┬───────────────────┘
│  └─ Optional chat-feed (COM1 log) │                                │
└──────────────────────────────────┘                                │
                                                                   │
                                                  QMP JSON-RPC over TCP (:4444)
                                                                   │
                                                                   ▼
                                                       ┌─────────────────────────┐
                                                       │ QEMU process             │
                                                       │  -vnc :0                 │
                                                       │  -qmp tcp:4444           │
                                                       │  -device qemu-xhci       │
                                                       │  -device usb-kbd         │
                                                       │  (orchestrated by wr.bat)│
                                                       └────────────┬────────────┘
                                                                    │
                                                                    │ VNC frames (RFB over WS)
                                                                    │
                                                                    ▼
                                                              Browser canvas
```

Two transports, two parts each:

- **Frames:** QEMU `-vnc :0` RFB server → noVNC.js in browser renders via WebSocket-to-RFB proxy in `harness.py`. 25 KB JS library; nothing custom.
- **Control:** Browser keyboard event → POST `/api/qmp/sendkey` → `harness.py` opens TCP to QEMU's `:4444`, sends `"execute":"qmp_capabilities"` then `"execute":"send-key"` with key names. Same socket reused for `device_add` / `device_del` / `screendump`.

The browser CAN connect directly to `localhost:5900` too — but that hardcodes the port number into the browser, breaks if we move QEMU. Going through the Python server means we can retarget without a code change.

### 3.1 What stays put

- `log_server.py` and its `index.html` continue to handle serial log streaming and the flame graph. The harness is a **sibling** page (`/harness`) that links back to `/` (logs) for the developer who needs to see what the OS printed.
- `wr.bat` adds four flags: `-vnc :0`, `-qmp tcp:127.0.0.1:4444,server,nowait`, plus optional `-device qemu-xhci,id=xhci` and `-device usb-kbd,bus=xhci.0` (the latter two are already needed for xHCI work — they enable L2 input).
- The existing POSIX serial channels (COM1/2/3) on `serial_output.log`, `test_log.txt`, `profile.log` are the right place for boot markers and profiling events from the OS — the dashboard already surfaces these.

---

## 4. The L0 / L1 / L2 fidelity ladder

The harness's *sendkey* wire-up mechanism is identical at all three levels. What changes is **where keys arrive in the guest**, which exercises different drivers:

| Level | Path that the key traverses | Why pick it |
|---|---|---|
| **L0** | QEMU monitor `sendkey` → QEMU i8042 emulation → guest PS/2 ISR (`keyboard_handle_keypress`) | Quickest to test x86 boot & kernel init. Skips the entire USB subsystem. |
| **L1** | QEMU `sendkey` → QEMU virtio-input keyboard → guest virtio-input driver | Useful when you want to test virtio without writing a virtio stack. Skips USB and PS/2 entirely. |
| **L2** ⟵ our choice | QEMU `sendkey` → QEMU's emulated USB keyboard on the qemu-xhci bus → guest xHCI driver → HID boot-protocol decoder → `kbd_inject_scancode_set1` | Tests the actual USB path. Every press exercises TRBs, the boot-protocol parser, and the bridge into the existing set-1 keyboard queue. Any regression in `kern_xhci.c` or `kern_usb_hid.c` shows up immediately as "the harness keys stop working". |

What this means for the harness: **the server-side code doesn't change between L0/L1/L2**. The difference is in the QEMU command line and in which guest ISR is wired up. We commit to L2 today because PS/2 is already disabled in `kern_keyboard.c` (`ps2_keyboard_enabled = false`), the IRQ 33 ISR is commented out in `main.c`, and the xHCI driver architecture work is in flight. Bouncing back to L0 is one QEMU-flag revert.

### 4.1 What makes L2 *true* USB

L2 only counts as "true USB" if:
- QEMU emulates the USB keyboard via the same xHCI controller that the guest enumerates (we have `-device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0` already).
- The guest has no PS/2 ISR registered (currently the case — `ps2_keyboard_enabled = false`).
- The guest's xHCI driver accepts reports and feeds `kbd_inject_scancode_set1` (Phase 7+ of the USB plan).

Today, with only `Phase 0` complete (factor + PS/2 disable), the harness's `sendkey` will deliver keys to QEMU's emulated USB keyboard and then *nothing* — the OS will drop them on the floor because there's no xHCI driver yet. That's expected. The harness is built; the OS catches up.

---

## 5. noVNC integration

[noVNC](https://novnc.com) is the canonical WebSocket-to-VNC bridge used in many OSdev harnesses. Drop into `dashboard/static/`:

```bash
npm install --no-save vnc  # (or curl noVNC web files into dashboard/static/novnc/)
```

`harness.py` exposes a binary WebSocket endpoint:

```python
# /ws/vnc → proxy-only: tunnel WebSocket frames between browser and tcp://127.0.0.1:5900
async def vnc_proxy(websocket):
    reader, writer = await asyncio.open_connection("127.0.0.1", 5900)
    pump_websock_to_tcp(websocket, writer)
```

The browser `novnc.js` library reads RFB pixel updates and renders to a `<canvas>`. Existing OSdev harnesses (SerenityOS Build Worker UI, Redox OS Browser Test) work the same way; no custom protocol code is needed on the OS side.

### 5.1 Latency expectations

VNC delta updates at ~80–120 ms round-trip on localhost. For Doom — already framebuffer-heavy — this means the visible latency on a @60fps + 1080p × 32bpp stream will be modest. Faster than that requires `virtio-gpu + venus` (3D-aware, far more setup), not VNC.

---

## 6. The USB device tree panel

The harness's nicest ergonomics feature. Replaces the "open the QEMU monitor and type `device_add usb-mouse,bus=xhci.0`" workflow with a checkbox UI:

```html
<section id="usb-tree">
  <h3>xHCI bus</h3>
  <ul>
    <li>
      <input type="checkbox" id="kbd" checked>
      <label for="kbd">usb-kbd (boot protocol)</label>
    </li>
    <li>
      <input type="checkbox" id="mouse">
      <label for="mouse">usb-mouse (boot protocol)</label>
    </li>
    <li>
      <input type="checkbox" id="tablet">
      <label for="tablet">usb-tablet (absolute pointer)</label>
    </li>
    <li>
      <input type="checkbox" id="storage">
      <label for="storage">usb-storage (bulk class)</label>
      <small>requires kernel USB mass storage driver</small>
    </li>
  </ul>
  <button id="apply">Apply</button>
</section>
```

A toggle does:
```javascript
await fetch('/api/qmp/device', { method: 'POST', body: JSON.stringify({
  action: clicked ? 'add' : 'del',
  id: idMap[kbd.checked ? 'add' : 'del'].device,
  bus: 'xhci.0'
})});
```

`harness.py` translates that to:
```python
# device_add: {"execute":"device_add","arguments":{"driver":"usb-mouse","bus":"xhci.0"}}
# device_del: {"execute":"device_del","arguments":{"id":"usb-mouse"}}
```

**Important caveat:** QEMU's `device_del` identifies by `id=`, not by `driver=`. When we add a device we name it, and remember the id on the harness side, so we can pass it back. For now the easiest invariant is to use `id=${driver}-${incrementing_counter}` (e.g. `usb-mouse-0`, `usb-kbd-1`).

---

## 7. Routes in `harness.py`

| Path | Method | Body / query | QEMU call | Purpose |
|---|---|---|---|---|
| `/harness` | GET | — | n/a | serve the harness HTML |
| `/api/qmp/sendkey` | POST | `{ keys: ["ctrl-c", "a"] }` or `{ text: "echo\n" }` | `send-key` | inject keys / type a string |
| `/api/qmp/device` | POST | `{ action, id, driver, bus }` | `device_add` / `device_del` | plug/unplug |
| `/api/qmp/screendump` | GET | — | `screendump` | one-shot PNG snapshot |
| `/api/qmp/ws` | WS | binary | (proxies to :5900) | noVNC frame stream tunnel |
| `/api/qmp/status` | GET | — | `query-status`, `query-usb` | current state for the page |
| `/ws/qmp` | WS | subscribe events | `qmp_capabilities` + `__event_filter` | live event stream: power-button, RTC tick, etc. (optional v2) |

The QMP client is a thin async wrapper that maintains a single TCP socket to `:4444`, sends JSON-RPC envelopes, awaits matching responses. GET requests return JSON. POST requests take JSON, run an awaited `execute`, return the QEMU result (or error if the device_add failed because the bus port is full, etc.).

---

## 8. Implementation phases

| Phase | What | Files | Lines (est.) |
|---|---|---|---|
| **A** | refactor: extract HTML out of `log_server.py`'s string literal | `dashboard/index.html` (new), `dashboard/log_server.py` (shrink) | ~ −300, +5 |
| **B** | skeleton `harness.py` with TCP→QEMU client + route stubs | `dashboard/harness.py`, `dashboard/run-harness.bat` | ~250 |
| **C** | skeleton `harness.html` with noVNC placeholder, USB tree, status bar | `dashboard/harness.html` (new) | ~150 |
| **D** | wire keyboard event capture → `/api/qmp/sendkey` | `dashboard/harness.html` JS | +60 |
| **E** | wire USB tree toggles → `/api/qmp/device` | `dashboard/harness.html` JS | +80 |
| **F** | QMP status polling (slot count, devices) | `dashboard/harness.py` + `harness.html` JS | +60 |
| **G** | wr.bat change: add `-vnc :0 -qmp tcp:127.0.0.1:4444,server,nowait` | `wr.bat` | +1 line |
| **H** | end-to-end smoke: `wr-harness.bat`, browser opens `/harness`, sees Limine handoff frame, types "ls\n" → keystrokes funnel to QEMU → `send-key` call → guest xHCI driver → `kbd_inject_scancode_set1` once xHCI is up | (verification) | n/a |
| **Total** | | | ~610 lines new, ~300 lines moved to file |

Each phase is independently testable. Phase A is a one-shot refactor — running `run.bat` today should be identical to before. Phase B fires up an empty harness at `:8080/harness` that 404s on `sendkey`. Phase H is the FMV (first measurable value).

### 8.1 Why this scope is right for an afternoon

The mechanical work is:
- Extract HTML (Phase A, mechanical).
- Build a JSON-RPC-over-TCP client with asyncio (Phase B, textbook).
- Write a thin SPA that wires inputs to API routes (Phases C–F, mechanical).

The architectural decisions (noVNC over a custom frame protocol, QMP for control, sibling server model) all converge on QEMU as the source of truth — which makes the harness eventually correct by construction. The only OS-side work is xHCI driver + boot-protocol decoder, which is already planned.

---

## 9. What this harness is NOT

| Not | Why |
|---|---|
| A persistent testing service | No record/replay, no audit log, no structured result reporting. Goal is interactive exploration. |
| A multi-user OS playground | One QEMU per session. (Multi-can be-added-via-QEMU's-vmware-viewer-style mechanism if demand exists later.) |
| A file drop into the guest | virtfs is available but we're skipping per current scope decision. |
| A performance harness | VNC's 80–120 ms latency hides micro-stalls. Profiler's `profile.log` channel in the dashboard is the right place for performance data. |

These are real omissions, not bugs. They each cost multi-day effort to do well.

---

## 10. Cross-references

- `media/writings/usb_basic_implementation_plan.md` — defines the xHCI stack the harness exercises.
- `media/writings/multi_serial_profiling.md` — defines the COM1/2/3 channels that the dashboard reads; the harness reuses those for OS-side debug output.
- `media/writings/intel_iris_xe_graphics_plan.md` — separate concern (framebuffer source). The harness uses QEMU's VNC server, not anything in the iGPU plan, so the two are independent.
- `media/writings/window_management.md` — separate concern. Compositor planning is orthogonal to the harness; the harness sends keys to one OS instance.

---

## 11. Open questions to resolve while building

1. **boot protocol vs report protocol toggle.** QEMU's `usb-kbd` always speaks boot protocol. If a user-unplugged and replugged a real fancy keyboard through `-device usb-host`, that'd be report-protocol. We don't ship in v1.
2. **multi-display.** When QEMU shows two `-device virtio-gpu` connected, VNC sees one of them. Switch which in `device_add`/`device_del`. Not in v1.
3. **What about capturing the WASM Doom scene?** VNC delta-only updates at ~80–120 ms; Doom at 60 fps will look stuttery. For smoother capture, replace VNC with a QEMU `-vnc :0 -display none` + a `screendump`-poll at 30–60 FPS. Optional upgrade, not v1.
4. **Should the harness keep a connection log?** "press 7 at 11:42:13 → send-key with index 9, response time 4.2 ms" — useful for debugging xHCI regressions. Optional, deferred.

---

## 12. Summary

| Decision | What |
|---|---|
| Frame transport | noVNC over QEMU's `-vnc :0`  
| Control transport | QEMU QMP socket on TCP `:4444`, JSON-RPC  
| Input fidelity | L2 (true USB through qemu-xhci + usb-kbd)  
| Hot-plug UI | checkboxes per device, plus "Apply" button — saves you the QEMU monitor typing  
| HTML | extracted from `log_server.py` into dedicated `dashboard/index.html`  
| New module | `dashboard/harness.py` (sibling server)  
| New UI | `/harness` page with noVNC canvas + USB tree + status bar  
| QEMU flag changes | one extra line in `wr.bat`  

Total scope: about 610 new LOC across the dashboard. Shipable in an afternoon if you don't get distracted by the future features. Start with Phase A (HTML extraction) today so all subsequent diffs stay clean.
