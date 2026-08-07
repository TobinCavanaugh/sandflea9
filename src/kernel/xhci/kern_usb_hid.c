// USB HID class driver — boot-protocol keyboard decoder.
//
// Translates 8-byte HID boot reports into PS/2 set-1 scancodes and
// feeds them through kbd_inject_scancode_set1(), exactly like the
// i8042 PS/2 keyboard ISR. This means all upstream code (shell, Doom,
// foreground WASM apps) works without modification.
//
// Phase 7 of media/writings/usb_basic_implementation_plan.md.

#include "../../include/dialect.h"
#include "../../include/kern_usb_hid.h"
#include "../../include/kern_serial.h"
#include "../../include/kern_keyboard.h"
#include "../../include/kern_xhci.h"

// HID Usage ID → PS/2 set-1 scancode mapping for standard keys.
// Only the keys we actually use in Doom + the shell are mapped.
// See: USB HID Usage Tables 1.5, §10 Keyboard/Keypad Page (0x07).

typedef struct {
    u8  hid_usage;   // USB HID Usage ID (low byte)
    u8  scancode;    // PS/2 set-1 scancode
    bool extended;   // true = E0-prefixed scancode
} hid_key_map_t;

static const hid_key_map_t key_map[] = {
    // Letters
    {0x04, 0x1E, false}, // a
    {0x05, 0x30, false}, // b
    {0x06, 0x2E, false}, // c
    {0x07, 0x20, false}, // d
    {0x08, 0x12, false}, // e
    {0x09, 0x21, false}, // f
    {0x0A, 0x22, false}, // g
    {0x0B, 0x23, false}, // h
    {0x0C, 0x17, false}, // i
    {0x0D, 0x24, false}, // j
    {0x0E, 0x25, false}, // k
    {0x0F, 0x26, false}, // l
    {0x10, 0x32, false}, // m
    {0x11, 0x31, false}, // n
    {0x12, 0x18, false}, // o
    {0x13, 0x19, false}, // p
    {0x14, 0x10, false}, // q
    {0x15, 0x13, false}, // r
    {0x16, 0x1F, false}, // s
    {0x17, 0x14, false}, // t
    {0x18, 0x16, false}, // u
    {0x19, 0x2F, false}, // v
    {0x1A, 0x11, false}, // w
    {0x1B, 0x2D, false}, // x
    {0x1C, 0x15, false}, // y
    {0x1D, 0x2C, false}, // z
    // Numbers
    {0x1E, 0x02, false}, // 1
    {0x1F, 0x03, false}, // 2
    {0x20, 0x04, false}, // 3
    {0x21, 0x05, false}, // 4
    {0x22, 0x06, false}, // 5
    {0x23, 0x07, false}, // 6
    {0x24, 0x08, false}, // 7
    {0x25, 0x09, false}, // 8
    {0x26, 0x0A, false}, // 9
    {0x27, 0x0B, false}, // 0
    // Enter, Escape, Backspace, Tab, Space
    {0x28, 0x1C, false}, // Return/Enter
    {0x29, 0x01, false}, // Escape
    {0x2A, 0x0E, false}, // Backspace
    {0x2B, 0x0F, false}, // Tab
    {0x2C, 0x39, false}, // Space
    // Symbols
    {0x2D, 0x0C, false}, // - (minus)
    {0x2E, 0x0D, false}, // = (equals)
    {0x2F, 0x1A, false}, // [ (left bracket)
    {0x30, 0x1B, false}, // ] (right bracket)
    {0x31, 0x2B, false}, // \ (backslash)
    {0x33, 0x27, false}, // ; (semicolon)
    {0x34, 0x28, false}, // ' (quote)
    {0x35, 0x29, false}, // ` (grave)
    {0x36, 0x33, false}, // , (comma)
    {0x37, 0x34, false}, // . (period)
    {0x38, 0x35, false}, // / (slash)
    // Caps Lock
    {0x39, 0x3A, false}, // Caps Lock
    // Function keys
    {0x3A, 0x3B, false}, // F1
    {0x3B, 0x3C, false}, // F2
    {0x3C, 0x3D, false}, // F3
    {0x3D, 0x3E, false}, // F4
    {0x3E, 0x3F, false}, // F5
    {0x3F, 0x40, false}, // F6
    {0x40, 0x41, false}, // F7
    {0x41, 0x42, false}, // F8
    {0x42, 0x43, false}, // F9
    {0x43, 0x44, false}, // F10
    {0x44, 0x57, false}, // F11
    {0x45, 0x58, false}, // F12
    // Navigation
    {0x46, 0x37, false}, // Print Screen (approximate)
    {0x47, 0x46, false}, // Scroll Lock (approximate)
    {0x49, 0x52, true }, // Insert (E0 52)
    {0x4A, 0x47, true }, // Home (E0 47)
    {0x4B, 0x49, true }, // Page Up (E0 49)
    {0x4C, 0x53, true }, // Delete (E0 53)
    {0x4D, 0x4F, true }, // End (E0 4F)
    {0x4E, 0x51, true }, // Page Down (E0 51)
    {0x4F, 0x4D, true }, // Right Arrow (E0 4D)
    {0x50, 0x4B, true }, // Left Arrow (E0 4B)
    {0x51, 0x50, true }, // Down Arrow (E0 50)
    {0x52, 0x48, true }, // Up Arrow (E0 48)
};

#define KEY_MAP_COUNT (sizeof(key_map) / sizeof(key_map[0]))

// Modifier byte bit definitions (boot keyboard report byte 0):
#define MOD_LCTRL    (1u << 0)
#define MOD_LSHIFT   (1u << 1)
#define MOD_LALT     (1u << 2)
#define MOD_LGUI     (1u << 3)
#define MOD_RCTRL    (1u << 4)
#define MOD_RSHIFT   (1u << 5)
#define MOD_RALT     (1u << 6)
#define MOD_RGUI     (1u << 7)

// Look up a HID Usage → set-1 scancode mapping.
// Returns scancode and sets *extended if it's an E0-prefixed key.
// Returns 0 if unmapped.
static u8 hid_to_scancode(u8 usage, bool *extended) {
    for (u32 i = 0; i < KEY_MAP_COUNT; i++) {
        if (key_map[i].hid_usage == usage) {
            *extended = key_map[i].extended;
            return key_map[i].scancode;
        }
    }
    return 0;
}

// Process an 8-byte HID boot keyboard report and inject scancodes
// for any key state changes since the last report.
//
// Report format:
//   byte 0: modifier bitmask
//   byte 1: reserved (0)
//   bytes 2–7: up to 6 held key Usage IDs (0x00 = no key)
void kbd_hid_process_report(const u8 report[8]) {
    static u8 prev_report[8] = {0};
    static bool prev_mods_injected[8] = {0}; // track which modifier scancodes we injected
    (void)prev_mods_injected;  // suppress unused warning for now

    // ── Modifier handling ────────────────────────────────────────────────
    // Modifiers: map to set-1 make/break codes.
    // LCtrl=0x1D, LShift=0x2A, LAlt=0x38
    // RCtrl=E0 0x1D, RShift=0x36, RAlt=E0 0x38
    struct {
        u8  bit;
        u8  sc;
        bool ext;
    } mods[] = {
        {MOD_LCTRL,  0x1D, false},
        {MOD_LSHIFT, 0x2A, false},
        {MOD_LALT,   0x38, false},
        {MOD_RCTRL,  0x1D, true},
        {MOD_RSHIFT, 0x36, false},
        {MOD_RALT,   0x38, true},
    };

    for (u32 m = 0; m < 6; m++) {
        u8 was = prev_report[0] & mods[m].bit;
        u8 now = report[0] & mods[m].bit;
        if (now && !was)
            kbd_inject_scancode_set1(mods[m].sc, mods[m].ext, true);
        else if (!now && was)
            kbd_inject_scancode_set1(mods[m].sc, mods[m].ext, false);
    }

    // ── Normal key handling ──────────────────────────────────────────────
    // Keys that were held last report but not this one → release
    for (u32 k = 2; k < 8; k++) {
        u8 prev_key = prev_report[k];
        if (prev_key == 0) continue;
        bool still_held = false;
        for (u32 j = 2; j < 8; j++) {
            if (report[j] == prev_key) {
                still_held = true;
                break;
            }
        }
        if (!still_held) {
            bool ext = false;
            u8 sc = hid_to_scancode(prev_key, &ext);
            if (sc) kbd_inject_scancode_set1(sc, ext, false);
        }
    }

    // Keys that appear in this report but not last → press
    for (u32 k = 2; k < 8; k++) {
        u8 new_key = report[k];
        if (new_key == 0) continue;
        bool was_held = false;
        for (u32 j = 2; j < 8; j++) {
            if (prev_report[j] == new_key) {
                was_held = true;
                break;
            }
        }
        if (!was_held) {
            bool ext = false;
            u8 sc = hid_to_scancode(new_key, &ext);
            if (sc) kbd_inject_scancode_set1(sc, ext, true);
        }
    }

    // Save current report for next diff
    for (u32 i = 0; i < 8; i++)
        prev_report[i] = report[i];
}

// Poll USB HID keyboards for new reports.
// Call from the main loop (non-ISR context) to process any queued reports.
// Each report is decoded and injected via kbd_inject_scancode_set1.
void kbd_usb_poll(void) {
    // Iterate all slots; if a report is ready, process it.
    for (u32 slot = 1; slot <= 8; slot++) {
        if (xhci_kbd_report_ready(slot)) {
            u8 *report = xhci_kbd_get_report(slot);
            if (report) {
                kbd_hid_process_report(report);
            }
        }
    }
}
