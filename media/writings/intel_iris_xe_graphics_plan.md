# Intel Iris Xe (Gen12) Graphics Bring-Up Plan

**Status:** Roadmap / Design Exploration  
**Date:** October 2026  
**Target hardware:** Intel Core i7-1165G7 (Tiger Lake-UP3, 11th Gen) with Iris Xe Graphics GT2 (96 EUs, Gen12 / Xe-LP, integrated only — no dGPU on this SKU)

**TL;DR:** Real bare-metal Intel iGPU support in a hobby OS is a multi-month project that mostly involves replaying work Linux's `i915` driver has done over fifteen years. The pragmatic path is **dual-track**: develop visually meaningful changes against QEMU's `virtio-gpu` (or Bochs VBE mode 0xE0 when even virtio is overkill), and reserve the iGPU driver itself for the day we need hardware-accelerated rendering or a real multi-display setup on this laptop.

---

## 1. Current State vs. Target State

| Layer | Today | After today (QEMU dev) | Long-term (bare metal) |
|---|---|---|---|
| **Source of the linear FB** | Limine UEFI/GOP handoff (`limine_framebuffer_request`, mapped in `kern_screen.c` via `screen_init`) | Same; eventually a `virtio-gpu` resource pointing into RAM | Iris Xe Pipe A primary plane, scanout from a GGTT-mapped framebuffer |
| **Mode set / timings** | Whatever UEFI GOP initialized | QEMU's `std-vga` Bochs VBE (mode 0xE0) or Virtio-GPU 2D mode set | Tiger Lake PLL → Transcoder → DDI tap dance |
| **Acceleration** | Pure software (`screen_put_pixel`, `screen_draw_box`) | Same, maybe with `simd_fill_32` | Eventually DMA blits via the BCS (Blitter) engine on Gen12 |
| **Multi-monitor** | Whatever Limine reports (`display_array[32]`) | One QEMU window; multiple via `-device secondary-vga` | Up to 4 pipes + multiple DDIs on Tiger Lake |
| **Compute / GL / Vulkan** | None | None (Venus is Linux-side virtio-3D, irrelevant to our guest) | Effectively impossible without running full i915's GuC submission path |
| **Lines of kernel code added** | 0 | ~200 for a minimal `virtio-gpu` 2D driver | ~3000+ to duplicate i915's display engine bring-up |

**Honest conclusion up front:** the real hardware path is possible but enormous. Most of this doc is therefore about scoping the *minimum* that justifies the effort, and ensuring that what we *do* build in QEMU is portable to bare metal later.

---

## 2. Why the iGPU on *this* laptop is unusually hostile

You bought a Tiger Lake laptop with no discrete GPU. That is good for battery life and bad for OS development.

### 2.1 Single iGPU → no VFIO passthrough

- The Tiger Lake iGPU is the **only** GPU in the system, hardwired to the PCH.
- iGPU passthrough via `vfio-pci` on a single-iGPU mobile system is virtually impossible: the BIOS usually has no "IOMMU" option for the iGPU, ACS is not present on the platform, and even where you can bind it to `vfio-pci`, the host has no fallback GPU and the laptop's screen goes blank.
- "Dual-graphics" SKUs (Iris Xe MAX with a Gen12 dGPU) exist, but the i7-1165G7 you have is iGPU-only.

This means **all our GPU development must happen against an emulated device tree** — i.e., QEMU. The bare-metal path is a real hardware target, but you cannot iterate on it daily without booting your actual laptop.

### 2.2 Tiger Lake is Gen12 — well past the "old i915 was 5 files" era

- Pre-Gen6 (Ironlake through Gen7.5/Haswell) GPUs were tractable: ~5 files in the i915 driver, no GuC, simple ring submissions, mostly public PRM.
- Gen8 (Broadwell) onward added GuC and HuC firmware, execlists, power wells.
- Gen9.5/Gen11 added fence/ Timeline objects, logical contexts.
- Gen12 (Tiger Lake, your chip) added Xe-LP, a completely reworked display engine (combo-PHYs, Type-C mux, per-DDI voltage), and the engine reset topology is essentially undocumented outside of internal `Bspec` material.

Translation: bringing up the GT engine (compute/render) is **not realistic** in a hobby OS short of a multi-year project. The display engine (display-only, KMS-style) is technically feasible if we limit ourselves to one Pipe, one DDI, an eDP or HDMI panel, and accept a lot of trial-and-error against the live hardware.

### 2.3 Public Intel documentation is patchy

- The "Intel Open Source HD Graphics Programmer's Reference Manual" (PRM) is public for some generations, partial for Tiger Lake.
- Many of the specific register definitions for combo-PHYs, Type-C mux tables, voltage scaling curves, and engine reset behavior live in Intel's internal `Bspec` (not public).
- The Linux kernel's i915 driver is therefore the **most authoritative source** for register-level behavior — but it's ~250k lines and sprawling.

---

## 3. The Hardware on your Desk (i7-1165G7 specifics)

| Subsystem | Detail |
|---|---|
| **PCI address** | `00:02.0` (vendor `0x8086`, device **`0x9A49`** for the i7-1165G7; sibling Tiger Lake IDs: `0x9A40`, `0x9A78`) |
| **BAR0** | 64-bit non-prefetchable, size ~16 MB — partitioned into GT and display register blocks |
| **BAR2** | 64-bit prefetchable, ~256 MB (sometimes 512 MB) — the GGTT aperture |
| **Other BARs** | Generally unused on modern iGPUs |
| **Class code** | `0x03` (display controller), subclass `0x00`, prog-if `0x00` |
| **Microarchitecture** | Gen12 / Xe-LP |
| **EU count** | 96 EUs (the i7-1165G7 is GT2) |
| **Engine classes** | RCS (render), BCS (blitter), VCS (video decode), VECS (video enhance) |
| **Display pipe count** | 4 (Pipe A–D), each independent |
| **DDI count** | 3 combo-PHY DDIs (DDI A, B, C) plus a Type-C mux |
| **eDP** | Built-in panel typically wired to DDI A |
| **Firmware blobs** | DMC ~15–30 KB, GuC ~300 KB–1 MB, HuC ~200–300 KB |
| **Stolen memory (DSM)** | 32 MB to 512 MB, locked by BIOS at boot; queried via `BGSM` (Base of Graphics Stolen Memory) PCI config register |

The PCI enumeration already exists in `src/kernel/kern_pci.c` (`pci_get_device(class_code, subclass)` walks the device list). The first concrete probe for an iGPU will look like:

```c
// In kern_pci.c or a future kern_gpu.c:
pci_device_t *igpu = pci_get_device(0x03, 0x00);   // find display controller
if (igpu && igpu->vendor_id == 0x8086 && igpu->device_id == 0x9A49) {
    // Tiger Lake i7-1165G7 — keep going
    serial_outsf("iGPU detected: %04x:%04x at %02x:%02x.%d\n",
                 igpu->vendor_id, igpu->device_id,
                 igpu->bus, igpu->slot, igpu->function);
}
```

---

## 4. Bare-Metal Init: What the OS Has To Do

This is the *Display-Engine-Only* path. It assumes there is one eDP panel on Pipe A and that we never need 3D compute. It is the irreducible skill set for getting *any* pixels out of the chip on this laptop.

### 4.1 Sequence

```
PCI probe            → confirm 00:02.0 is Intel (0x8086), 0x9A49
Map BAR0             → kmalloc + vmm_map_phys, 16 MB
Map BAR2             → same, 256–512 MB GGTT aperture
Enable bus mastering → set PCI COMMAND.BME + MEM
Acquire forcewake    → before ANY GT or display register read/write
Find stolen memory   → read BGSM (PCI config) + GSM (MMIO BGSM equivalent)
Load DMC firmware    → DMA copy into stolen memory, signal display MCU
Power up display     → PG1, then PG2; bit-poke power-well registers
Pick a pipe          → Pipe A (idiomatic — DDI A is wired to eDP)
Choose DDI           → DDI A (eDP)
Pick PLL             → DPLL0 (or 1/2/3 for Tiger Lake)
Configure PLL        → OUI, fractional divider for the panel's pixel clock
Set transcoder       → Transcoder A timings: HACTIVE, VACTIVE, HSW, HBP, etc.
Pick a plane         → Primary plane A on Pipe A
Point plane          → GGTT physical address of our framebuffer (in stolen memory or system RAM via GTT)
Enable panel power   → PP_CONTROL: power sequence on, backlight enable
Enable pipe          → Set PIPE_CONF_ENABLE
Enable DDI_BUF       → Set DDI_BUF_CTL_ENABLE on DDI A
Done                 → pixels appear on the eDP panel
```

This is *roughly* 1000 lines of hand-crafted poking when you include workarounds, debug prints, and reverse-engineered comments. i915 does it in ~80,000 lines because they handle every port, every panel timing, every quirk, every power state, every reset.

### 4.2 Approximate register layout (BAR0, 16 MB)

```
0x000000 – 0x00FFFF   Global control, forcewake, IRQ, status
0x060000 – 0x0FFFFF   Display engine: pipes, ports, transcoders, planes, DDI buffers
0x100000 – 0x1FFFFF   Display power wells, watermarks, FDI/cascade state
0x200000 – 0x2FFFFF   GT / render engines: RCS, BCS, VCS, VECS ring registers
... (engine pages, MOCS, etc., extending up to ~16 MB)
```

For the display-only path we mostly touch display engine registers (`0x060000`-`0x100000`). Forcing `forcewake` on the **display** well (rather than the GT render well) is the bit we actually need.

### 4.3 Firmware blobs

| Blob | Required? | Why |
|---|---|---|
| **DMC** (Display MicroController) | Strong recommended | Without it, the display engine can drop into low-power states unrecoverably; on Tiger Lake, missing DMC on suspend-resume cycles is famous. Boot without it *might* work once, but the moment the panel tries DC5/DC6 on idle, it'll lock. Recommended: ship it. |
| **GuC** (Graphics microController) | Optional for display | GuC replaces legacy ring-buffer submission with scheduled contexts. A pure-display-only path using legacy execlists can skip GuC. Required for 3D/compute. Ship it if/when we want any compute. |
| **HuC** (HEVC microController) | Never, probably | HuC decodes HEVC. We have no video pipeline. Skip. |

DMC is ~30 KB. It lives in `drivers/gpu/drm/i915/`'s firmware table; a hobby OS-friendly approach is to **vendor it in `src/external/i915_firmware/tgl_dmc.bin`** and C-include it as a `static const u8[]`. (Same pattern as our existing `xxhash.c` vendor.) This sidesteps the Linux fw_loader subsystem entirely.

### 4.4 Stolen memory & GTT

The iGPU carves out a chunk of system RAM at boot (or really, the BIOS does and the OS just has to live with it). The relevant registers:

- `BGSM` (in PCI config space, offset 0xB0 on Gen6+ integrated graphics) — Base of Graphics Stolen Memory.
- `GSM` (in MMIO, `0x108100` on Tiger Lake) — read of the same value, canonical source.

On a typical Tiger Lake laptop, the BIOS sets this to 32, 64, 128, 256, or 512 MB. Our plan:

```
stolen_base_phys = mmio_read32(BAR0 + 0x108100) & 0xFFFFFFF0;
stolen_size      = (mmio_read32(BAR0 + 0x108100) >> 32) & 0xFF;  // actually a shift + mask
```

The framebuffer we want the panel to scan out of must be a contiguous physical region **inside this stolen region** (or, on Gen12+, anywhere in system RAM mapped via the GGTT). The simplest first choice: use the stolen region itself, allocating from its base for now, with a small slab allocator.

---

## 5. Pain Points (Tiger Lake specific)

Each one of these has killed someone else's first attempt.

### 5.1 Forcewake (must do, easy to forget)

Before reading or writing any engine-side register, the host must "wake up" the relevant power well. Otherwise:

- Reads return `0xFFFFFFFF` (not zero, `0xFFFFFFFF`)
- Writes appear to succeed but never reach silicon
- The bus can wedge; the only recovery is a full reset

For the display engine we need:

```c
// Request display engine forcewake
mmio_write32(BAR0 + 0x60200, 0x1);                 // FORCEWAKE_DISP_MT
while ((mmio_read32(BAR0 + 0x60200) & 0x1) == 0) {} // wait ack
```

There are similar sequences for `FORCEWAKE_RENDER`, `FORCEWAKE_BLITTER`, `FORCEWAKE_MEDIA`. Each is a different well. Each must be released on idle or the chip won't sleep. **Forgetting to release forcewake is the classic "my laptop fans spin forever after I exit my OS" bug**.

### 5.2 DMC Firmware Loading

Without DMC firmware, the display pipes drop into DC-state 5 or 6 (panel power off) shortly after the OS starts. With DMC, the firmware mediates these transitions, and they work.

Loading is roughly: copy the DMC blob to a known physical address (e.g., the base of stolen memory), set `DMC_PROGRAM_ADDR` MMIO register, set `DMC_PROGRAM_LOAD`, and wait for `DMC_INIT_DONE` to flip.

Reference: `drivers/gpu/drm/i915/display/intel_dmc.c` in the Linux kernel. About 350 lines including the program structure table.

### 5.3 PLL → Transcoder → DDI Routing

The order matters, and there are per-port table lookups ("voltage tables", "clock tables") that depend on the specific SKU. Tiger Lake's combo-PHY can be either HDMI or DP, which is selected by writing a "port selector" bit (varies with PHY revision).

The minimal eDP-on-DDI-A path is the easiest to get right because eDP has fixed wiring — no mux to negotiate. Any external port (HDMI/DP/Type-C) needs the voltage table lookup, which lives in Bspec-only territory. **For the first boot, target eDP only**.

### 5.4 Stolen Memory & Framebuffer Placement

The framebuffer you want the panel to scan out of must be reachable via the GGTT. If you put it in system RAM and forget to map it in the GGTT, the panel reads `0x00000000` and gives you a black screen (or hangs). Until GGTT is set up, **use a stolen-memory address**.

### 5.5 Public Document Gaps

For Tiger Lake specifically:

- **Combo-PHY voltage tables** are not in the public PRM. The Linux driver has hard-coded tables per SKU, derived from internal Bspec.
- **Gen12 execlist / context submission** behavior is partially public.
- **Engine reset paths** are largely absent from public docs.

The compensation: when bring-up fails, the recovery is to read the relevant i915 driver function and replay it, blindly for now, with debug prints.

---

## 6. The QEMU Development Path

This is the high-velocity side: do real work against an emulated GPU every day, separate from any timing risk of touching the real iGPU.

### 6.1 The three QEMU options

| `-vga` / `-device` | What it is | Lines to support | Notes |
|---|---|---|---|
| `none` (no `-vga`) | No display | 0 | Limine can't get a GOP framebuffer; **don't** |
| `std` (Bochs VBE compatible) | Legacy QEMU/legacy VGA PCI | ~50 | Already gives us a 32 MB linear FB at mode 0xE0; bare-bones works today via Limine |
| `virtio-gpu` | Modern 2D-accelerated GPU | ~350 for a 2D driver | Realistic target for our future "VM render target" |

For *today*, Limine + `-vga std` gives us the linear framebuffer we already use; no work required. For *any meaningful graphics code we want to test*, we want `virtio-gpu`.

### 6.2 Standard QEMU invocation for sandfleaOS

For QEMU 8+/9+:

```
qemu-system-x86_64 -machine q35 \
  -cpu host -accel whpx \
  -drive format=raw,file=sandfleaOS.iso \
  -device virtio-gpu,xres=1920,yres=1080 \
  -display gtk
```

- `q35` is the chipset with PCIe support matching what we want for our PCI walk.
- `whpx` is the Windows Hypervisor Platform accelerator — required on Windows hosts because WHPX is the only thing with reasonable performance.
- `-device virtio-gpu,xres=1920,yres=1080`: requests a 1920x1080 scanout surface. QEMU fills in a 32-bit XRGB framebuffer.

The resulting device appears as a virtio-pci device; `lspci` inside the guest lists it as `00:01.0 ... Display controller [0300]: Red Hat, Inc. Virtio GPU`.

(Note: the existing `wr.bat` already moves from `-accel tcg` to `-accel whpx`. The QEMU command line currently doesn't specify virtio-gpu — it relies on the Limine framebuffer handoff. That's fine for *today*. See Section 6.4.)

### 6.3 What a minimal virtio-gpu driver looks like

The virtio-gpu spec defines a few core commands we need:

| Command | Purpose |
|---|---|
| `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D` | Allocate a 2D image resource (host-side) |
| `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING` | Point it at guest RAM (a list of (phys_addr, length) tuples) |
| `VIRTIO_GPU_CMD_SET_SCANOUT` | Tell the host "scan this resource into scanout N" |
| `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D` | "I just wrote to the resource; ask the host to refresh its copy" |
| `VIRTIO_GPU_CMD_RESOURCE_FLUSH` | Wait for the host to finish its draw |

Pseudocode of the driver surface we need:

```c
typedef struct gpu_resource {
    u32 id;             // our handle
    u32 width, height;
    u32 format;         // VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM or similar
    u64 phys_addr;      // guest RAM guest allocates and writes to
    u32 scanout_id;
} gpu_resource_t;

void gpu_init(void);
void gpu_create_2d(gpu_resource_t *r, u32 w, u32 h, u32 format);
void gpu_attach_backing(gpu_resource_t *r, u64 phys, u32 length);
void gpu_set_scanout(u32 scanout_id, gpu_resource_t *r);
void gpu_transfer_to_host_2d(const gpu_resource_t *r,
                              u32 x, u32 y, u32 w, u32 h);
void gpu_flush(const gpu_resource_t *r, u32 x, u32 y, u32 w, u32 h);
```

The host kernel should:

1. At boot, after Limine gives us a real linear FB, set up the virtio-gpu resource with the same physical pages (or hand off entirely — see below).
2. After every "render pass" in `kern_screen.c`, call `gpu_transfer_to_host_2d()` for the dirty region, then `gpu_flush()`.
3. Replace `screen_draw()` (which currently does `memcpy(surface → trueAddress)`) with `gpu_transfer_to_host_2d + gpu_flush`.

That's the *whole* "page-flip equivalent" we'll get from virtio-gpu. After that, the existing compositor / VT code becomes 100% portable to a real iGPU by swapping `gpu_flush` for the equivalent Pipe A frame-on-scanout path.

### 6.4 Limine + virtio interop

Limine's `framebuffer_request` reads mode info from **whatever Limine decides to use** — typically the GOP on UEFI firmware. In a QEMU VM where the GOP was QEMU/std-vga, Limine hands us a pointer to a framebuffer in guest RAM at boot. We can either:

- **Path A — Coexistence:** Keep using Limine's FB as today. Add virtio-gpu only for *additional* surfaces (offscreen rendering buffers, future windows). Limine FB continues to be the primary scanout target.
- **Path B — Hand off entirely** (recommended for future): Reject the Limine FB (`LIMINE_NO_FRAMEBUFFER`), set up virtio-gpu ourselves to claim the only scanout, and treat virtio-gpu as the canonical display. Cleanest separation in the long run.

For now: Path A. Path B is a one-line change once we trust virtio-gpu enough.

---

## 7. Phased Implementation Plan

### Phase A — QEMU virtio-gpu (real, achievable, ~3 days)

Day 1: PCI probe for `0x1AF4:0x1050` (virtio GPU device, vendor Red Hat)
Day 2: Implement virtqueue init + `RESOURCE_CREATE_2D` + `ATTACH_BACKING` + `SET_SCANOUT`
Day 3: Implement `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`, swap `screen_draw` to use them
Day 4: Test in `wr.bat` QEMU run with `-device virtio-gpu`

This phase can be done in **isolation** from any real hardware risk. Estimated kernel lines: ~400.

### Phase B — Bare-metal iGPU probe (reading-only, ~1 day)

1. Add `pci_get_device(0x03, 0x00)` filter for the iGPU
2. Print the BARs, class code, BGSM (stolen memory base), GMS size, panel info if any
3. **Bail on boot** if not Tiger Lake (refuse to bring up wrong-generation hardware)

Estimated lines: ~100 (all serial output, no register writes).

### Phase C — Bare-metal display engine (Tiger Lake only, ~3–4 weeks)

1. Map BAR0 and BAR2; enable bus mastering
2. Acquire and release display forcewake correctly
3. Vendor DMC firmware (`src/external/i915_firmware/tgl_dmc.bin`), load it
4. Power up display wells PG1, PG2
5. Configure Pipe A + DDI A in eDP mode
6. Compute the LCD panel's pixel clock from EDID (read from the eDP AUX channel)
7. Program DPLL, transcoder timings, primary-plane stride and address
8. Enable DDI_BUF and pipe
9. If anything fails, print a diagnostic and fall back to Limine's FB

This is a multi-iteration project. The whole thing is debuggable entirely on the laptop — read a register, print it, compare to expected. Estimated kernel lines: ~1500–3000.

### Phase D — GuC / multi-pipe / compute (only when we need it)

For Doom at 60 fps with proper GPU acceleration. Probably never in this project — Doom already runs via the software framebuffer in 1080p on the i7-1165G7 fast enough.

Estimate: 1+ years of effort. Don't start this.

---

## 8. Decision: Which path do we commit to today?

**Recommendation: dual-track, but biased heavily toward the QEMU side.**

- The OS today runs in QEMU with `-accel whpx` (`wr.bat`). All graphics code we write lives here.
- Real-hardware iGPU is a *weekend project* for the bare minimum but a *months long* project for any real rigour. Only attempt it on Tiger Lake after Phase C is fully understood against QEMU (where the logic is identical except for which MMIO registers you poke).
- If we ever need a real machine driver, the **virtio-gpu → iGPU** porting story is small because the underlying model is the same: write pixels to a region of memory, tell the scanout engine where it is.
- If we instead conclude "the Limine framebuffer is enough forever", we save 3000 lines by deciding today. The case for that conclusion is the `multi_serial_profiling.md` / `framebuffer_app_architecture.md` work — Doom at full speed, multiple VTs, and software compositing hit 60 fps with software rendering on this hardware. The iGPU is overkill for what we actually run.

**Avoid committing to Phase C until Phases A and the "is Limine FB enough" question have actually been tested.** A clean experiment:

1. Boot the laptop directly (not QEMU). Run Doom for 5 minutes.
2. Watch `htop` / `serial_output.log` for CPU usage during Doom.
3. If Doom is below 30 fps, the iGPU matters. If it's fine, **stop**.

---

## 9. Open Questions

1. **Stop here or push to bare-metal?** The above experiment will answer it; if Limine FB is sufficient we save months of pain.
2. **Should we replace `-vga std` in `wr.bat` with `-device virtio-gpu`?** Only after Phase A is done. Until then the default VGA suffices.
3. **What about `pg_helpers` / Linux-style panel drivers?** The full set of eDP/HMI/DP panel timings across vendors is thousands of panels. We either vendor the EDID(s) of the laptop's actual panel, hard-code one fixed mode, or punt and use whatever the BIOS/Uefi GOP handed us via Limine.
4. **Do we need GuC?** Almost certainly not. A display-only path can use legacy execlists and skip GuC. Re-evaluate when 3D is on the table.
5. **What does limine offer for bare-metal graphics?** Limine's GOP handoff *already* does all of Phase C for us, on most laptops. The argument for writing our own driver is "intellectual satisfaction" + "fallback when Limine changes." Not a strong argument.

---

## 10. Summary

| Phase | What | Effort | Standalone? |
|---|---|---|---|
| **A** | virtio-gpu 2D driver in QEMU | ~400 lines, ~3 days | ✅ Independent |
| **B** | Bare-metal iGPU probe (read-only) | ~100 lines, ~1 day | ✅ Safe; no side effects |
| **C** | Bare-metal display engine bring-up | ~1500–3000 lines, ~3–4 weeks | ⚠️ Buggy; needs real-hardware iteration |
| **D** | GuC / 3D / multi-pipe / HDMI out | ~5000+ lines, ~months | ❌ Not justified yet |

**Start with Phase A. Use QEMU exclusively.** Phase B can happen on a Saturday afternoon. Phase C is a real commitment — only enter it once A is done and Doom performance over a software-rendered Limine FB has been measured on this exact laptop.

**Minimum viable plan:** Phase A in the next sprint, plus a 5-minute benchmark of Doom-on-real-laptop to decide whether Phases B–C are worth the bet.
