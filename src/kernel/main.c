#define d_m3EnableOpProfiling 0
#if DoM3Logging == 1
# define d_m3LogParse           1   // .wasm binary decoding info
# define d_m3LogModule          1   // Wasm module info
# define d_m3LogCompile         1   // wasm -> metacode generation phase
# define d_m3LogWasmStack       1   // dump the wasm stack when pushed or popped
# define d_m3LogEmit            1   // metacode-generation info
# define d_m3LogCodePages       1   // dump metacode pages when released
# define d_m3LogRuntime         1   // higher-level runtime information
# define d_m3LogNativeStack     1   // track the memory usage of the C-stack
#else
# define d_m3LogParse           0   // .wasm binary decoding info
# define d_m3LogModule          0   // Wasm module info
# define d_m3LogCompile         0   // wasm -> metacode generation phase
# define d_m3LogWasmStack       0   // dump the wasm stack when pushed or popped
# define d_m3LogEmit            0   // metacode-generation info
# define d_m3LogCodePages       0   // dump metacode pages when released
# define d_m3LogRuntime         0   // higher-level runtime information
# define d_m3LogNativeStack     0   // track the memory usage of the C-stack
#endif

#include "../include/dialect.h"
#include "../include/stbsupport.h"
#include "../include/kern_asmstubs.h"
#include "../include/kern_serial.h"
#include "../include/kern_vmm.h"
#include "../include/kern_keyboard.h"
#include "../include/kern_mem.h"
#include "../util/util_str.h"
#include "../../limine/limine.h"
#include "../include/limine_requests.h"
#include "../include/kern_pci.h"
#include "../include/kern_xhci.h"
#include "../include/kern_usb_hid.h"
#include "../include/kern_interrupts.h"
#include "../include/kern_screen.h"
#include "../include/kern_ext2.h"
#include "../include/kern_sched.h"
#include "../include/ssfn.h"
#include "../include/util_cmd.h"
#include "../include/kern_terminal.h"
#include "../include/kern_tests.h"
#include "../include/kern_compositor.h"
#include "../include/kern_mouse.h"
#include "../include/kern_ide.h"
#include "../include/kern_fs.h"
#include "../include/kern_profile.h"

display_t *display_main = 0;
u64 usable_ram = 0;

extern char _binary_src_blob_regularfont_sfn_start;

extern u0 enable_sse(u0);

volatile u64 sw = 0;

i64 heartbeat1 = 0;
i64 heartbeat2 = 0;
i64 heartbeat3 = 0;

u0 timer_handler(const registers_t *reg) {
    // PROFILE_SCOPE here is gated by PROFILE_ENABLED (default OFF, see
    // kern_profile.h). When enabled it emits 2 serial lines per 10ms tick
    // and busy-waits ~260us/char at 38400 baud — ~29% of all CPU time in
    // QEMU — so only enable it in profiling builds (PROFILE=1).
    PROFILE_SCOPE("timer_handler");
    sw += 10;
    if (sw % 1000 == 0) heartbeat1 = heartbeat1 == 0 ? 1 : 0;
    if (sw % 500 == 0)  heartbeat2 = heartbeat2 == 0 ? 1 : 0;
    if (sw % 100 == 0)  heartbeat3 = heartbeat3 == 0 ? 1 : 0;
    apic_eoi(0xFFFFFFFF10000000);
    // CPU accounting: credit the task that ran the previous quantum.
    kern_task_t *cur_task = sched_get_current_task();
    if (cur_task) cur_task->run_ticks++;
    sched_run_next();
}

u0 delay(u64 ms) {
    volatile u64 start = sw;
    while (sw - start < ms) {
        asm volatile("hlt");
    }
}

system_t system = {0};
char typingbuf[255] = {0};


u0 direct_fb_fill(u32 color) {
    if (framebuffer_request.response && framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        if (fb && fb->address) {
            u32 *pixels = (u32 *)fb->address;
            u64 count = (fb->pitch / 4) * fb->height;
            for (u64 i = 0; i < count; i++) {
                pixels[i] = color;
            }
        }
    }
}

void kern_entry(void) {
    // Init all 3 serial channels — COM1 (PRIMARY), COM2 (TEST), COM3 (PROFILE)
    serial_init_all();
    serial_outsl("--- sandfleaOS Kernel Entry ---");
    serial_outsl("Serial: COM1 (PRIMARY) ready");
    if (serial_channel_present(SERIAL_CH_TEST))
        serial_outsl("Serial: COM2 (TEST) ready");
    if (serial_channel_present(SERIAL_CH_PROFILE))
        serial_outsl("Serial: COM3 (PROFILE) ready");

    // SSE must be enabled before calling any variadic function (e.g.,
    // serial_outsf_ch inside profile_init / PROFILE_INSTANT) because the
    // x86-64 ABI variadic prologue emits SSE movaps/movups instructions.
    serial_outsl("CPU: enabling SSE...");
    enable_sse(); // cpu extension
    serial_outsl("CPU: SSE extensions enabled");

    // Init profiling framework — writes to COM3 (PROFILE channel).
    // Do this early so we can instrument the rest of boot.
    serial_outsl("PROFILE: initializing...");
    profile_init();
    serial_outsl("PROFILE: init done");
    PROFILE_INSTANT("boot:kernel_entry");
    serial_outsl("PROFILE: boot marker emitted");

    init_vmm_globals(hhdm_request); // virtual memory management
    serial_outsl("VMM: Virtual Memory Management initialized");
    PROFILE_INSTANT("boot:vmm_done");

    init_pmm(memmap_request); // physical memory management
    serial_outsl("PMM: Physical Memory Management initialized");
    PROFILE_INSTANT("boot:pmm_done");

    interrupts_init(); // interrupts
    serial_outsl("IDT: Interrupts and GDT stubs initialized");
    PROFILE_INSTANT("boot:idt_done");

    kmalloc_init(); // malloc
    serial_outsl("Heap: kmalloc initialized and ready for allocations");
    PROFILE_INSTANT("boot:heap_done");

    sched_init();
    serial_outsl("Scheduler: Multi-threading support initialized");
    PROFILE_INSTANT("boot:sched_done");

    compositor_init();
    serial_outsl("Compositor: Initialized");


    system.pci_list_head = pci_init_system();
    serial_outsl("PCI: System bus scanned");

    // xHCI driver — PCI probe only. USB keyboard enumeration is shelved
    // (control-transfer DMA bug); input is via PS/2 i8042 instead.
    // When the DMA issue is fixed, uncomment the block below.
    xhci_pci_probe();
    serial_outsl("xHCI: USB keyboard enumeration shelved — using PS/2");
#if 0  // ── xHCI keyboard enumeration (shelved) ─────────────────────────
    if (xhci_init()) {
        serial_outsl("xHCI: controller initialised, scanning ports...");
        for (u32 port = 1; port <= xhc_max_ports(); port++) {
            u32 slot = xhci_enumerate_keyboard(port);
            if (slot) {
                xhci_kbd_start_polling(slot);
                serial_outsf("xHCI: keyboard on port %d → slot %d, polling started\n", port, slot);
                break;
            }
        }
    } else {
        serial_outsl("xHCI: init failed — USB keyboard not available");
        xhci_smoke_test_first();
    }
#endif

    // In kern_entry...
    pci_device_t *pci_uart = system.pci_list_head;
    while (pci_uart) {
        // Look specifically for the WCH CH382 (1C00:3253)
        if (pci_uart->vendor_id == 0x1C00 && pci_uart->device_id == 0x3253) {
            serial_outsf("PCI: Found WCH CH382 Serial Controller at %X:%X\n", pci_uart->vendor_id,
                         pci_uart->device_id);
            break;
        }
        pci_uart = pci_uart->next;
    }

    if (pci_uart) {
        serial_outsl("UART: Initializing PCI UART Card...");
        pci_enable_device_io(pci_uart); // Enable Bus Master / IO

        // Try initializing BOTH BARs
        for (int i = 0; i < 2; i++) {
            u32 bar = pci_uart->bars[i];
            if ((bar & 1) == 1) {
                // If IO space
                u32 io_base = bar & 0xFFFFFFFC;
                if (io_base < 0xFFFF) {
                    pci_init_uart_port(io_base);
                    serial_outsf("UART: Initialized Port at IO Base 0x%X (BAR %d)\n", io_base, i);

                    // Send specific identifier for each port
                    if (i == 0) pci_serial_putsl(pci_uart, "PCI_UART: Output stream started from BAR 0");
                    if (i == 1) pci_serial_putsl(pci_uart, "PCI_UART: Output stream started from BAR 1");
                }
            }
        }
    }

    if (framebuffer_request.response == NULL || framebuffer_request.response->framebuffer_count < 1) {
        serial_outsl("FATAL: No framebuffer provided by Limine!");
        for (;;) {
            __asm__("hlt");
        }
    }


    display_t *displays = kmalloc(sizeof(display_t) * 32);
    u8 fb_count = screen_init(framebuffer_request.response, displays, 32);
    display_main = &displays[0];
    serial_outsf("Video: Primary display surface address: %p, width=%d, height=%d\n", 
                 display_main->surface.address, (int)display_main->surface.width, (int)display_main->surface.height);
    serial_outsf("Video: %d framebuffer(s) found. Primary: %dx%d %dbpp\n",
                 fb_count, display_main->surface.width, display_main->surface.height, display_main->surface.bpp);

    // Early visual confirmation on bare-metal screen
    if (display_main->surface.address) {
        mem_set(display_main->surface.address, 0x1B, display_main->surface.pitch * display_main->surface.height);
        screen_draw();
    }

    u64 stack_ptr;
    asm volatile("mov %%rsp, %0" : "=r"(stack_ptr));
    serial_outsf("Kernel Stack Pointer: %llX\n", stack_ptr);

    ssfn_src = (ssfn_font_t *) &_binary_src_blob_regularfont_sfn_start;
    serial_outsl("Font: SSFN regular font loaded");

    u32 width = display_main->surface.width, height = display_main->surface.height;
    u32 row_len = width / font_width;

    // 3. Configure SSFN destination using the Limine framebuffer data
    ssfn_dst.ptr = (u8 *) display_main->surface.address;
    ssfn_dst.w = width;
    ssfn_dst.h = height;
    ssfn_dst.p = display_main->surface.pitch;
    ssfn_dst.x = 0; // Start cursor at 0,0
    ssfn_dst.y = 0;
    ssfn_dst.fg = 0xFFFFFFFF; // White text

    u64 total_ram = 0;
    usable_ram = 0;
    struct limine_memmap_response *mm = memmap_request.response;
    for (u64 i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *me = mm->entries[i];
        switch (me->type) {
            case LIMINE_MEMMAP_USABLE: {
                total_ram += me->length;
                usable_ram += me->length;
                break;
            }
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            case LIMINE_MEMMAP_KERNEL_AND_MODULES:
            case LIMINE_MEMMAP_ACPI_NVS: {
                total_ram += me->length;
                break;
            }
            default: {
                break;
            }
        }
    }
    system.usable_mem_size = usable_ram;
    system.total_mem_size = total_ram;

    serial_outsf("Memory: Total %lld MiB, Usable %lld MiB\n", total_ram / 1024 / 1024, usable_ram / 1024 / 1024);

    // Screen buffer
    screen_lines_init(row_len);
    serial_outsl("Terminal: Screen line buffer initialized");

    ide_init();
    serial_outsl("FS: IDE Initialized");
    PROFILE_INSTANT("boot:ide_done");

    ext2_init(module_request.response);
    serial_outsl("FS: Ext2 driver initialized");
    PROFILE_INSTANT("boot:ext2_done");

    fs_init();
    serial_outsl("FS: FS initialized");
    PROFILE_INSTANT("boot:fs_done");

    interrupt_register(32, timer_handler);
    // [PHASE 0 / PS/2 DISABLED] IRQ 33 (i8042 keyboard) handler is
    // temporarily not registered while we bring up xHCI USB. Restore the
    // line below once the USB HID boot-protocol decoder is fully wired
    // up and ready to take over input.
    interrupt_register(33, (void (*)(const registers_t *)) keyboard_handle_keypress);
    interrupt_register(44, mouse_handle_interrupt);
    mouse_init();
    sti();
    serial_outsl("Interrupts: Timer, keyboard, and mouse handlers registered (sti)");

    // CPU clock / P-state / HWP (Intel Speed Shift) initialization:
    // On bare metal (e.g. Tiger Lake i5-1135G7 / i7-1165G7), without HWP/P-state init
    // the CPU remains locked at minimum low-frequency mode (400-800MHz).
    {
        u32 eax, ebx, ecx, edx;
        asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(6));

        // Check for HWP (Intel Speed Shift) base support (CPUID.06H:EAX[bit 7])
        if (eax & (1 << 7)) {
            // Enable EIST in IA32_MISC_ENABLE (MSR 0x1A0)
            u64 misc = rdmsr(0x1A0);
            wrmsr(0x1A0, misc | (1ULL << 16));

            // Enable HWP autonomous management via IA32_PM_ENABLE (MSR 0x770)
            wrmsr(0x770, 1);

            // Read hardware capability limits from IA32_HWP_CAPABILITIES (MSR 0x771)
            u64 hwp_cap = rdmsr(0x771);
            u8 highest = hwp_cap & 0xFF;         // Max Turbo (e.g. 42 = 4.2GHz)
            u8 guaranteed = (hwp_cap >> 8) & 0xFF; // Base Clock (e.g. 24 = 2.4GHz)
            if (highest == 0) highest = 42;
            if (guaranteed == 0) guaranteed = 24;

            // Configure IA32_HWP_REQUEST (MSR 0x774):
            // - Bits 7:0   = Minimum performance (guaranteed base clock)
            // - Bits 15:8  = Maximum performance (max turbo)
            // - Bits 23:16 = Desired performance (explicitly request max turbo)
            // - Bits 31:24 = Energy Performance Preference (0 = Max Performance)
            u64 hwp_req = ((u64)0 << 24) | ((u64)highest << 16) | ((u64)highest << 8) | (u64)guaranteed;
            wrmsr(0x774, hwp_req);

            // Also configure package-level HWP request (MSR 0x772) if supported (CPUID.06H:EAX[bit 11])
            if (eax & (1 << 11)) {
                wrmsr(0x772, hwp_req);
            }

            // Set IA32_ENERGY_PERF_BIAS (MSR 0x1B0) to 0 (Performance)
            wrmsr(0x1B0, 0);

            // Clear BD_PROCHOT in MSR_POWER_CTL (0x1FC) to prevent EC from clamping clock to 400MHz
            u64 pwr_ctl = rdmsr(0x1FC);
            wrmsr(0x1FC, pwr_ctl & ~1ULL);

            // Also set IA32_PERF_CTL (MSR 0x199) ratio
            wrmsr(0x199, (u64)highest << 8);

            serial_outsf("CPU: Intel HWP set Min=%dx (%dMHz), Desired/Max=%dx (%dMHz), EPP=0, BD_PROCHOT cleared\n",
                         guaranteed, guaranteed * 100, highest, highest * 100);
        }

        if (ecx & 1) {
            u64 m0 = rdmsr(0xE7); // IA32_MPERF — max-possible cycles
            u64 a0 = rdmsr(0xE8); // IA32_APERF — actual cycles
            delay(50);
            u64 m1 = rdmsr(0xE7);
            u64 a1 = rdmsr(0xE8);
            u64 mdiff = m1 - m0;
            u64 ratio = mdiff ? ((a1 - a0) * 100) / mdiff : 0;
            serial_outsf("CPU: clock ratio %llu%% of nominal (MPERF/APERF), TSC ~%llu MHz\n",
                         ratio, profile_tsc_mhz());
            if (ratio && ratio < 75) {
                serial_outsf("CPU: WARNING — core running at %llu%% of nominal speed\n", ratio);
            }
        } else {
            serial_outsf("CPU: MPERF/APERF not present; TSC ~%llu MHz\n", profile_tsc_mhz());
        }
    }

    // Initialize terminal sessions (cell buffers, ANSI parser state, etc.)
    term_init(width / font_width, (height - font_height * 2) / font_height);
    serial_outsl("Terminal: Sessions initialized");

    PROFILE_INSTANT("boot:complete");
    serial_outsl("--- Initialization Complete. Entering Main Loop ---");

    for (;;) {
        // We are the idle (boot/shell) thread: the scheduler skips us while
        // other threads are ready unless the keyboard ISR woke us with
        // pending input. Clear that wakeup now that we're running.
        sched_idle_clear();

        // USB HID polling shelved — input is via PS/2 i8042.
        // Uncomment below when xHCI control-transfer DMA is fixed.
#if 0
        kbd_usb_poll();
        u32 new_slot = xhci_pending_enumerate();
        if (new_slot) {
            xhci_kbd_start_polling(new_slot);
            serial_outsf("USB: hot-plug keyboard on slot %d, polling started\n", new_slot);
        }
#endif

        // Keyboard input — always processed (even when a session's foreground
        // app is in fullscreen mode like Doom), so session switching via
        // F1-F4 works regardless of what the foreground app is doing.
        u8 k = 0;
        while ((k = keyboard_eat_key())) {
            // F1-F4: switch virtual terminal sessions
            if (k >= KEY_F1 && k <= KEY_F4) {
                u32 target = k - KEY_F1;  // F1=0, F2=1, F3=2, F4=3
                if (target < MAX_SESSIONS && target != active_session->id) {
                    serial_outsf("VT switch: %s -> %s\n",
                                 active_session ? active_session->name : "?",
                                 sessions[target].name);
                    session_switch(target);
                }
                continue;
            }

            // Global Ctrl+C: always intercepts and terminates the active session's foreground process
            if (k == KEY_CTRL_C) {
                if (active_session && active_session->foreground_proc != NULL) {
                    kern_process_t *fp = (kern_process_t *) active_session->foreground_proc;
                    i32 pid = fp->pid;
                    screen_push_line("^C");
                    serial_outsf("Killed Process %d with Ctrl+C\n", pid);
                    sched_kill_process(pid);
                    active_session->foreground_proc = NULL;
                    active_session->owns_framebuffer = false;
                    keyboard_fg_flush();
                } else {
                    typingbuf[0] = 0;
                    screen_push_line("^C");
                    serial_outsf("Ctrl+C (no foreground process)\n");
                }
                continue;
            }

            i32 len = str_len(typingbuf);

            // If the compositor is active, route all input to it.
            if (g_compositor_pid != -1) {
                compositor_push_event(0, k, 0, 0);  // KEY_DOWN
                continue;
            }

            // If the active session has a foreground process, forward
            // keyboard to the per-session foreground queue.
            if (active_session && active_session->foreground_proc != NULL) {
                keyboard_fg_push(k);
                continue;
            }

            if (k == '\n') {
                screen_push_linef("#>%s", typingbuf);
                handle_command();
                typingbuf[0] = 0;
            } else if (k == '\b') {
                if (len > 0) typingbuf[len - 1] = '\0';
            } else if (k == KEY_DOWN) {
                ++screen_text_scroll;
            } else if (k == KEY_UP) {
                --screen_text_scroll;
                screen_text_scroll = max(screen_text_scroll, 0);
            } else if (k == KEY_PGUP) {
                screen_text_scroll = 0;
            } else if (k == KEY_PGDN) {
                screen_text_scroll = max(0, screen_get_line_count() - 2);
            } else {
                if (len < 254) {
                    typingbuf[len] = k;
                    typingbuf[len + 1] = 0;
                }
            }
        }

        // Mouse events — route to compositor when it's active.
        {
            mouse_event_t mev;
            while (mouse_eat_event(&mev)) {
                if (g_compositor_pid != -1) {
                    compositor_push_event(mev.type, (u32)(i32)mev.dx, (u32)(i32)mev.dy, 0);
                }
            }
        }

        // Render the active session (cell buffer, cursor, header bar, input prompt).
        // Skip when the compositor owns the display — it handles all rendering
        // via display.present() and we'd just overwrite its pixels.
        if (g_compositor_pid == -1) {
            term_render();
        }
        asm volatile("hlt");
    }
}

// Legacy wrapper — screen_render_shell is declared in kern_terminal.h.
// The actual implementation moved into kern_terminal.c::term_render().
// This wrapper is kept so existing callers (wasm_spawn.c) continue to link.
// (The real body is in kern_terminal.c.)
