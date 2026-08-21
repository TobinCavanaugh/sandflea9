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
#include "../include/kern_ide.h"
#include "../include/kern_fs.h"
#include "../include/kern_profile.h"

display_t *display_main = 0;
u64 usable_ram = 0;

extern char _binary_src_blob_regularfont_sfn_start;

extern u0 enable_sse(u0);

volatile u64 sw = 0;

u0 timer_handler(const registers_t *reg) {
    PROFILE_SCOPE("timer_handler");
    sw += 10;
    apic_eoi(0xFFFFFFFF10000000);
    sched_run_next();
}

u0 delay(u64 ms) {
    sched_sleep(ms);
}

system_t system = {0};
char typingbuf[255] = {0};

i64 heartbeat1 = 0;
i64 heartbeat2 = 0;
i64 heartbeat3 = 0;

u0 shimmy3(u0 *arg) {
    while (1) {
        heartbeat3 = heartbeat3 == 0 ? 1 : 0;
        delay(100);
    }
}

u0 shimmy2(u0 *arg) {
    while (1) {
        heartbeat2 = heartbeat2 == 0 ? 1 : 0;
        delay(500);
    }
}

u0 shimmy(u0 *arg) {
    while (1) {
        heartbeat1 = heartbeat1 == 0 ? 1 : 0;
        delay(1000);
    }
}


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

    char buf[255];

    sched_create_thread(shimmy, null);
    sched_create_thread(shimmy2, null);
    sched_create_thread(shimmy3, null);
    serial_outsl("Threads: Heartbeat threads spawned");

    interrupt_register(32, timer_handler);
    // [PHASE 0 / PS/2 DISABLED] IRQ 33 (i8042 keyboard) handler is
    // temporarily not registered while we bring up xHCI USB. Restore the
    // line below once the USB HID boot-protocol decoder is fully wired
    // up and ready to take over input.
    interrupt_register(33, (void (*)(const registers_t *)) keyboard_handle_keypress);
    sti();
    serial_outsl("Interrupts: Timer and keyboard handlers registered (sti)");

    // Initialize terminal sessions (cell buffers, ANSI parser state, etc.)
    term_init(width / font_width, (height - font_height * 2) / font_height);
    serial_outsl("Terminal: Sessions initialized");

    PROFILE_INSTANT("boot:complete");
    serial_outsl("--- Initialization Complete. Entering Main Loop ---");

    for (;;) {
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

        // Render the active session (cell buffer, cursor, header bar, input prompt)
        // If the session's foreground app takes over the framebuffer (doom),
        // term_render() just draws the header bar and skips the cell buffer.
        term_render();
        asm volatile("hlt");
    }
}

// Legacy wrapper — screen_render_shell is declared in kern_terminal.h.
// The actual implementation moved into kern_terminal.c::term_render().
// This wrapper is kept so existing callers (wasm_spawn.c) continue to link.
// (The real body is in kern_terminal.c.)
