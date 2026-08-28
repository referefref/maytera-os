// usb_hid.c - USB Human Interface Device (HID) Class Driver
#include "usb_hid.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../proc/process.h"
#include "../fs/bootlog.h"
#include "../cpu/mono.h"   // #139: THE shared monotonic clock for the latency decomposition
#include "../sync/waitq.h"
#include "../fs/fat.h"   // #156: fat_exists() for the /XHCIHID.OFF ESP lever  // #139: wait_event_timeout - the shared wait primitive, never a hand-rolled poll

// #307: kernel input sinks (shared with the PS/2 drivers) and worker plumbing.
extern void keyboard_process_scancode(uint8_t scancode);  // cpu/isr.c
extern void mouse_inject_hid(int dx, int dy, uint8_t buttons, int wheel); // drivers/mouse.c
extern volatile int xhci_iso_quiet;

// Per-report bootlog trace ("[USB-HID] slot N (type): report #M received").
// Default OFF: on a busy desktop each HID device fires ~125 reports/sec, so the
// first+every-50th trace still floods /BOOTLOG.TXT and the serial log. Flip to 1
// only when diagnosing "are reports arriving at all?" on real hardware.
// #433/#373: default OFF (production-clean). Set at boot to 1 by main.c when
// /CONFIG/USBDEBUG.CFG is present, so USB HID input can be diagnosed on real
// hardware (the low-speed-composite-behind-TT-hub path that no VM reproduces)
// WITHOUT a source edit + rebuild each time. Also settable live for tests.
volatile int usb_hid_report_log = 0;

// =============================================================================
// #307/#763: HID usage code -> PS/2 set-1 scancode
// =============================================================================
// USB HID keyboards report boot-protocol "usage" codes. The rest of MayteraOS
// consumes PS/2 set-1 scancodes (translated in cpu/isr.c). We convert here and
// feed keyboard_process_scancode() so USB and PS/2 keyboards are identical to
// every app, the compositor and any DOS guest downstream.
//
// #763: the TABLE that does the converting used to live in this file, with a
// byte-for-byte copy of it in bt/hid.c carrying a comment promising a merge.
// Both are gone; the one table is rustkern/hidmap.rs and both drivers call it.
// A copy with a note promising a merge is still a copy, and these two had
// already drifted. They also shared two real bugs, fixed in the shared
// version: keypad / was emitted as a BARE 0x35 (indistinguishable from the
// main-block slash; it is E0 35 on real hardware), and the entire numeric
// keypad, usages 0x59..0x63, was absent and emitted nothing at all.
extern uint8_t hid_usage_to_set1_rs(uint8_t usage, uint8_t *out_ext);   // rustkern/hidmap.rs
extern int     hidmap_selftest_rs(uint32_t *out_checks);                // rustkern/hidmap.rs

// #148: the shared cooked-key ring push (cpu/isr.h), for HID usages that have
// no honest single-(code,extended) PS/2 set-1 encoding at all (PrintScreen).
extern void keyboard_push_cooked_key(uint8_t code);   // cpu/isr.c

// Emit one set-1 transition into the shared kernel scancode path. That path
// (cpu/isr.c keyboard_process_scancode) is also where the DOS raw-scancode tap
// lives, which is what makes a DOS guest work on a USB-keyboard machine at all
// (#763); do not add a second, private route to the input ring here.
static void emit_set1(uint8_t code, int extended, int pressed) {
    if (!code) return;
    if (extended) keyboard_process_scancode(0xE0);
    keyboard_process_scancode(pressed ? code : (uint8_t)(code | 0x80));
}

// #162: fire one media key into the SAME funnel every other key uses.
//
// The three Consumer usages are translated to their real PS/2 set-1 extended
// codes (E0 30 volume up, E0 2E volume down, E0 20 mute) rather than to some
// private USB-only event, for the same reason emit_set1() exists at all: the
// whole rest of MayteraOS speaks set 1, and cpu/isr.c's ONE global-hotkey
// handler then covers PS/2, USB, Bluetooth and the #334 test channel with a
// single implementation. Do not add a second, private route to the volume
// state from here.
//
// Press and release are emitted back to back. Volume is a press-only action
// with no held state, and cpu/isr.c consumes the make and ignores the break;
// sending both keeps the byte stream well-formed for anything that might
// later care.
static void hid_consumer_emit(uint8_t set1_code) {
    emit_set1(set1_code, 1, 1);
    emit_set1(set1_code, 1, 0);
}

// Kernel key callback: usb_hid_process_keyboard fires this on each key/modifier
// transition. keycode 0xE0-0xE7 are the 8 modifier bits; everything else is a
// HID usage code.
static void hid_kernel_key_cb(uint8_t keycode, int pressed, uint8_t modifiers) {
    (void)modifiers;
    // #148: PrintScreen (HID usage 0x46 = HID_KEY_PRINTSCREEN) is deliberately
    // left unmapped by rustkern/hidmap.rs: real PS/2 hardware encodes it as a
    // 4-byte set-1 sequence that does not fit the (code, extended) pair every
    // other key uses (see that file's header note). USB gives us a clean,
    // unambiguous single usage with no such mess, so push the cooked key
    // straight into the ring instead of inventing a fake multi-byte set-1
    // emission just to have cpu/isr.c decode it straight back out. Press
    // only, like every other one-shot special key: a screenshot has no held
    // state to track.
    if (keycode == HID_KEY_PRINTSCREEN) {
        if (pressed) keyboard_push_cooked_key(0x9D /* KEY_PRINTSCREEN, cpu/isr.h */);
        return;
    }
    // keycode 0xE0..0xE7 are the eight modifier bits, synthesised by
    // usb_hid_process_keyboard() from the report's modifier byte; everything
    // else is a usage from the report's keycode array. The shared translator
    // handles both, so there is no modifier special case here any more.
    uint8_t ext = 0;
    uint8_t code = hid_usage_to_set1_rs(keycode, &ext);
    emit_set1(code, ext, pressed);
}

// =============================================================================
// Global State
// =============================================================================

// #134 REAL-HARDWARE ROOT CAUSE. hid_devices[] was APPEND-ONLY: usb_hid_attach()
// only ever did hid_devices[hid_device_count++], nothing in the whole kernel ever
// released an entry, and the cap was 8. Both of the owner's devices are COMPOSITE
// (2 HID interfaces each), so the iMac boots at 4/8 and EVERY replug burns 2 more.
// The captured /USBLOG.TXT from the failing boot shows exactly that: slots 2+3 at
// boot (4 entries), replug -> slot 7 (6), replug -> slot 8 (8 = FULL), replug ->
// slot 9 logs "CONFIG_EP -> OK" for both interfaces and then NOTHING, because the
// guard below returned -1. The refusal only ever went to kprintf(), which is
// silent on a GUI-mode iMac with no serial, so the machine simply stopped
// accepting input with no diagnostic and no way in (sshd=0). Entries are now
// REUSABLE (in_use) and the re-scan worker releases them on disconnect.
#define MAX_HID_DEVICES 32
static hid_device_t hid_devices[MAX_HID_DEVICES];
static int hid_device_count = 0;   // entries currently IN USE (logging/reporting)
// #162: bounded /BOOTLOG.TXT lines for media-key presses. Enough to prove the
// path is live on the owner's machine (which has no serial console), capped so
// a stuck key cannot fill the log.
static int bootlog_hid_media_lines = 0;

// #133: how many times we will run endpoint-halt recovery on ONE device before
// declaring it dead. Recovery issues command-ring ops from the SHARED poll
// worker, so an endlessly failing device must not be allowed to starve the other
// HID devices. Any successful report resets the counter, so a flaky wireless
// link can recover indefinitely; only a device that never comes back gives up.
#define HID_MAX_HALT_RECOVERIES 8

// =============================================================================
// #139: THE one interrupt-IN arming site
// =============================================================================
// Every place that arms this device's interrupt-IN endpoint goes through here,
// so the submit timestamp cannot be forgotten at one of them. There were four
// callers of xhci_int_in_submit() in this file (attach, composite re-arm, the
// no-TD-outstanding path and the post-report resubmit) and a stamp added at
// three of them would silently mis-measure the fourth. Returns 0 on success,
// matching xhci_int_in_submit().
static int hid_arm(hid_device_t *dev) {
    dev->submit_us = (uint32_t)mono_us();
    int r = xhci_int_in_submit(dev->controller, dev->slot_id, dev->dci,
                               dev->report_buf_phys, dev->report_len);
    if (r == 0) dev->outstanding = 1;
    return r;
}

// #139: fold one delivered report into the per-device latency statistics and,
// once per window, write ONE summary line to the persistent log. Bounded twice
// over (per-device window count AND a global line budget) because blame.md's
// #134/#135 lesson is that an unbounded diagnostic burns down the very log it
// exists to fill, and this one fires on a device that reports continuously.
#define HID_STAT_WINDOW      512   // reports per summary line
#define HID_STAT_MAX_LINES    40   // global cap on summary lines per boot
static uint32_t g_hid_stat_lines = 0;

static void hid_stat_report(hid_device_t *dev) {
    uint32_t now = (uint32_t)mono_us();
    uint32_t obs = xhci_xfer_done_us(dev->slot_id, dev->dci);

    uint32_t e2e = dev->submit_us ? (now - dev->submit_us) : 0;
    uint32_t lat = obs ? (now - obs) : 0;
    // A stale/absent observe stamp would otherwise show as a ~71-minute delta;
    // anything over a second is not a poll-cadence measurement, so drop it.
    if (lat > 1000000u) lat = 0;
    if (e2e > 1000000u) e2e = 0;

    dev->st_n++;
    dev->st_e2e_sum += e2e; if (e2e > dev->st_e2e_max) dev->st_e2e_max = e2e;
    dev->st_lat_sum += lat; if (lat > dev->st_lat_max) dev->st_lat_max = lat;

    if (dev->last_inject_us) {
        uint32_t gap = now - dev->last_inject_us;
        if (gap <= 1000000u) {
            dev->st_gap_sum += gap;
            if (gap > dev->st_gap_max) dev->st_gap_max = gap;
            uint32_t ms = gap / 1000u;
            int b = ms <  2 ? 0 : ms <  4 ? 1 : ms <  8 ? 2 : ms < 12 ? 3 :
                    ms < 16 ? 4 : ms < 24 ? 5 : ms < 40 ? 6 : 7;
            if (dev->st_gap_hist[b] < 0xFFFF) dev->st_gap_hist[b]++;
        }
    }
    dev->last_inject_us = now;

    if (dev->st_n < HID_STAT_WINDOW) return;
    if (g_hid_stat_lines < HID_STAT_MAX_LINES) {
        g_hid_stat_lines++;
        dev->st_windows++;
        uint32_t n = dev->st_n;
        bootlog_write("[HIDLAT] slot %d DCI %d %s w%u n=%u | gap avg %uus max %uus"
                      " | submit->inject avg %uus max %uus"
                      " | observe->inject avg %uus max %uus"
                      " | gapms<2,<4,<8,<12,<16,<24,<40,40+ = %u,%u,%u,%u,%u,%u,%u,%u",
                      dev->slot_id, dev->dci,
                      dev->type == HID_DEVICE_KEYBOARD ? "kbd" : "mouse",
                      dev->st_windows, n,
                      dev->st_gap_sum / n, dev->st_gap_max,
                      dev->st_e2e_sum / n, dev->st_e2e_max,
                      dev->st_lat_sum / n, dev->st_lat_max,
                      dev->st_gap_hist[0], dev->st_gap_hist[1],
                      dev->st_gap_hist[2], dev->st_gap_hist[3],
                      dev->st_gap_hist[4], dev->st_gap_hist[5],
                      dev->st_gap_hist[6], dev->st_gap_hist[7]);
        if (g_hid_stat_lines == HID_STAT_MAX_LINES)
            bootlog_write("[HIDLAT] summary budget of %d lines reached; no further "
                          "latency lines this boot (the counters keep running)",
                          HID_STAT_MAX_LINES);
    }
    dev->st_n = 0;
    dev->st_e2e_sum = dev->st_e2e_max = 0;
    dev->st_lat_sum = dev->st_lat_max = 0;
    dev->st_gap_sum = dev->st_gap_max = 0;
    for (int i = 0; i < 8; i++) dev->st_gap_hist[i] = 0;
}

// #134: claim a FREE entry. Returns NULL when the table is genuinely full.
static hid_device_t *hid_alloc_entry(void) {
    for (int i = 0; i < MAX_HID_DEVICES; i++)
        if (!hid_devices[i].in_use) return &hid_devices[i];
    return NULL;
}
static int hid_live_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_HID_DEVICES; i++) if (hid_devices[i].in_use) n++;
    return n;
}

// Global callbacks
static void (*global_key_callback)(uint8_t keycode, int pressed, uint8_t modifiers) = NULL;
static void (*global_mouse_callback)(int32_t x, int32_t y, uint8_t buttons) = NULL;

// HID keycode to ASCII lookup tables (lowercase)
static const char hid_keycode_ascii_lower[] = {
    0, 0, 0, 0,                         // 0x00-0x03: Reserved
    'a', 'b', 'c', 'd', 'e', 'f', 'g',  // 0x04-0x0A
    'h', 'i', 'j', 'k', 'l', 'm', 'n',  // 0x0B-0x11
    'o', 'p', 'q', 'r', 's', 't', 'u',  // 0x12-0x18
    'v', 'w', 'x', 'y', 'z',            // 0x19-0x1D
    '1', '2', '3', '4', '5', '6', '7',  // 0x1E-0x24
    '8', '9', '0',                      // 0x25-0x27
    '\n', 0x1B, '\b', '\t', ' '          // 0x28-0x2C: Enter, Esc, Backspace, Tab, Space
};

// =============================================================================
// Initialization
// =============================================================================

void usb_hid_init(void) {
    kprintf("[USB-HID] Initializing HID subsystem...\n");
    hid_device_count = 0;
    memset(hid_devices, 0, sizeof(hid_devices));   // in_use = 0 => all entries free
    for (int i = 0; i < MAX_HID_DEVICES; i++) hid_devices[i].root_port = -1;
    // #307: route USB keyboard transitions into the shared kernel input path.
    global_key_callback = hid_kernel_key_cb;
    // #763: prove the ONE usage->set-1 table on THIS build before a key is
    // pressed. A wrong entry is otherwise only visible as "that key does
    // nothing", which is exactly how the keypad stayed broken.
    { uint32_t checks = 0; int fails = hidmap_selftest_rs(&checks);
      kprintf("[HIDMAP] usage->set1 selftest: %u checks, %d failures\n", checks, fails);
      bootlog_write("[HIDMAP] usage->set1 selftest: %u checks, %d failures", checks, fails); }
}

// =============================================================================
// HID Class Requests
// =============================================================================

// #62: HID class requests get a BOUNDED budget, not the 5s enumeration default.
// SET_PROTOCOL/SET_IDLE run during enumeration and every caller already
// tolerates failure (the real iMac's Logitech receiver answers SET_IDLE=FAIL and
// works fine); SET_REPORT runs at RUNTIME on every Caps/Num/Scroll Lock press,
// so at the 5s default one flaky keyboard could stall its caller for 5 seconds
// per LED update. Measured cost of the old default on the owner's iMac: two
// "Transfer wait TIMEOUT slot 2 DCI 1 (5000ms budget; 5000ms real)" lines in the
// boot path = 10 seconds of boot spent waiting for a device that had already
// declined the request. A device that answered GET_DESCRIPTOR answers a class
// request in milliseconds or not at all.
#define HID_CTRL_TIMEOUT_MS 250

int usb_hid_set_protocol(hid_device_t *dev, uint8_t protocol) {
    if (!dev || !dev->controller) return -1;

    int result = xhci_control_transfer_to(dev->controller, dev->slot_id,
        0x21,                   // bmRequestType: Host to Device, Class, Interface
        HID_REQ_SET_PROTOCOL,   // bRequest
        protocol,               // wValue: Protocol (0=Boot, 1=Report)
        dev->interface_num,     // wIndex: Interface
        NULL, 0, HID_CTRL_TIMEOUT_MS);

    if (result >= 0) {
        dev->protocol = protocol;
        kprintf("[USB-HID] Set protocol to %s\n", 
                protocol == HID_PROTOCOL_BOOT ? "Boot" : "Report");
    }

    return result;
}

int usb_hid_set_idle(hid_device_t *dev, uint8_t report_id, uint8_t duration) {
    if (!dev || !dev->controller) return -1;

    return xhci_control_transfer_to(dev->controller, dev->slot_id,
        0x21,                   // bmRequestType: Host to Device, Class, Interface
        HID_REQ_SET_IDLE,       // bRequest
        (duration << 8) | report_id, // wValue: Duration (upper) | Report ID (lower)
        dev->interface_num,     // wIndex: Interface
        NULL, 0, HID_CTRL_TIMEOUT_MS);
}

int usb_hid_get_report(hid_device_t *dev, uint8_t type, uint8_t id,
                       void *buf, uint16_t len) {
    if (!dev || !dev->controller || !buf) return -1;

    return xhci_control_transfer_to(dev->controller, dev->slot_id,
        0xA1,                   // bmRequestType: Device to Host, Class, Interface
        HID_REQ_GET_REPORT,     // bRequest
        (type << 8) | id,       // wValue: Report Type (upper) | Report ID (lower)
        dev->interface_num,     // wIndex: Interface
        buf, len, HID_CTRL_TIMEOUT_MS);
}

int usb_hid_set_keyboard_leds(hid_device_t *dev, uint8_t leds) {
    if (!dev || !dev->controller || dev->type != HID_DEVICE_KEYBOARD) return -1;

    // LED report is Output report type
    uint8_t report[1] = { leds };

    return xhci_control_transfer_to(dev->controller, dev->slot_id,
        0x21,                   // bmRequestType: Host to Device, Class, Interface
        HID_REQ_SET_REPORT,     // bRequest
        (HID_REPORT_OUTPUT << 8) | 0, // wValue: Output Report, ID 0
        dev->interface_num,     // wIndex: Interface
        report, 1, HID_CTRL_TIMEOUT_MS);
}

// =============================================================================
// HID Device Probe and Attach
// =============================================================================

int usb_hid_probe(xhci_controller_t *xhc, int slot_id,
                  uint8_t interface_class, uint8_t interface_subclass,
                  uint8_t interface_protocol) {
    // Check if this is an HID device
    if (interface_class != 0x03) {  // HID class
        return -1;
    }

    hid_device_t *dev = hid_alloc_entry();
    if (!dev) {
        kprintf("[USB-HID] Maximum devices reached\n");
        return -1;
    }
    memset(dev, 0, sizeof(hid_device_t));
    dev->root_port = -1;

    dev->controller = xhc;
    dev->slot_id = slot_id;
    dev->protocol = HID_PROTOCOL_BOOT;

    // Determine device type from boot interface protocol
    if (interface_subclass == HID_SUBCLASS_BOOT) {
        if (interface_protocol == HID_BOOT_PROTOCOL_KEYBOARD) {
            dev->type = HID_DEVICE_KEYBOARD;
            kprintf("[USB-HID] Detected USB keyboard (slot %d)\n", slot_id);
        } else if (interface_protocol == HID_BOOT_PROTOCOL_MOUSE) {
            dev->type = HID_DEVICE_MOUSE;
            kprintf("[USB-HID] Detected USB mouse (slot %d)\n", slot_id);
        } else {
            dev->type = HID_DEVICE_UNKNOWN;
            kprintf("[USB-HID] Detected HID device (slot %d, protocol %d)\n", 
                    slot_id, interface_protocol);
        }
    } else {
        dev->type = HID_DEVICE_UNKNOWN;
    }

    // Set boot protocol for keyboards and mice
    if (interface_subclass == HID_SUBCLASS_BOOT) {
        usb_hid_set_protocol(dev, HID_PROTOCOL_BOOT);
    }

    // Set idle rate to 0 (report only on change)
    usb_hid_set_idle(dev, 0, 0);

    dev->in_use = 1;
    hid_device_count = hid_live_count();
    return (int)(dev - hid_devices);
}

// =============================================================================
// #307: Full attach (interrupt-IN endpoint already configured by the xHCI layer)
// =============================================================================

// =============================================================================
// #162: REPORT DESCRIPTOR fetch, dump and classify
// =============================================================================
//
// This kernel had NO GET_DESCRIPTOR(Report) call anywhere before #162, which
// is why it could not see a media key: boot protocol has no consumer usages in
// it by definition, and the report descriptor is the only place a device says
// it has any. Fetching it also produces the one diagnostic that was missing
// from every previous "the keyboard does something odd" investigation - the
// device's own statement of what it sends - so the bytes are dumped to
// /USBLOG.TXT whether or not we find anything in them.
//
// SAFETY POSTURE, because this runs on the enumeration path of the owner's
// ONLY input device. Every failure mode here (no HID class descriptor, a
// refused or short control transfer, a descriptor with nothing we recognise)
// lands on exactly the pre-#162 behaviour. Nothing is reclassified on a guess.
#define HID_MAX_REPORT_DESC 1024

// bmRequestType 0x81 = Device-to-Host, STANDARD, Interface. GET_DESCRIPTOR is
// a standard request even for the class-specific Report descriptor type; a
// class request (0xA1) here returns nothing on most devices, which is a
// classic way to conclude "this device has no report descriptor" when it does.
static int hid_fetch_report_desc(hid_device_t *dev, uint8_t *buf, int len) {
    if (!dev || !dev->controller || !buf || len <= 0) return -1;
    return xhci_control_transfer_to(dev->controller, dev->slot_id,
        0x81,                       // Device-to-Host, Standard, Interface
        0x06,                       // GET_DESCRIPTOR
        (0x22 << 8) | 0,            // wValue: Report descriptor, index 0
        dev->interface_num,         // wIndex: Interface
        buf, (uint16_t)len, HID_CTRL_TIMEOUT_MS);
}

// Dump the descriptor to /USBLOG.TXT, 16 bytes a line, bounded. This is the
// evidence that settles what a device actually is; on a machine with no serial
// console it is the ONLY way to find out.
static void hid_log_report_desc(int slot_id, int iface_num, const uint8_t *d, int n) {
    char line[80];
    static const char hexd[] = "0123456789abcdef";
    int max = n > 256 ? 256 : n;          // bounded: 16 lines is plenty to identify
    usblog_write("  slot %d iface %d: REPORT descriptor, %d bytes%s",
                 slot_id, iface_num, n, n > max ? " (first 256 shown)" : "");
    for (int i = 0; i < max; i += 16) {
        int k = 0;
        for (int j = 0; j < 16 && i + j < max; j++) {
            line[k++] = hexd[(d[i + j] >> 4) & 0xF];
            line[k++] = hexd[d[i + j] & 0xF];
            line[k++] = ' ';
        }
        line[k] = 0;
        usblog_write("    %04x: %s", i, line);
    }
}

int usb_hid_attach(xhci_controller_t *xhc, int slot_id, int iface_num,
                   int ep_in, int ep_in_mps, int b_interval, int speed,
                   uint8_t subclass, uint8_t protocol, int rd_len) {
    global_key_callback = hid_kernel_key_cb;   // ensure the sink is wired

    // #433 (re-scoped) Part B: DEDUPE. Compute this endpoint's DCI up front and
    // refuse to attach the SAME (controller, slot, DCI) twice. A composite
    // keyboard parsed via multiple alt settings, or re-enumerated by the #433
    // re-scan worker after a transient CONFIG_EP failure, could otherwise create
    // two hid_devices[] entries for one physical endpoint. The single poll
    // worker would then double-submit interrupt-IN TDs on that DCI, orphaning /
    // corrupting the endpoint - exactly the "keyboard enumerates but never types"
    // symptom, and keyboard-specific since it is the one that trips the re-scan.
    int new_dci = (ep_in & 0x0F) * 2 + ((ep_in & 0x80) ? 1 : 0);
    // #134: entries are no longer packed (in_use marks live ones), so this must
    // scan the WHOLE table, not the live count, or dedupe would miss an entry
    // sitting above a released hole and double-attach the same endpoint.
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        hid_device_t *e = &hid_devices[i];
        if (e->in_use && e->controller == xhc && e->slot_id == slot_id &&
            e->dci == new_dci) {
            // #134: RE-ARM, do not just hand back the stale entry. Re-enumeration
            // of the same (slot, DCI) means the controller re-initialised that
            // endpoint context, which ORPHANS whatever TD the entry still thinks
            // is in flight. Returning early with outstanding == 1 left the entry
            // waiting forever on a completion that could never arrive - a silent,
            // permanent dead keyboard indistinguishable from the #390 symptom.
            e->outstanding = 0;
            e->failed = 0;
            e->consecutive_halts = 0;
            if (e->type == HID_DEVICE_KEYBOARD || e->type == HID_DEVICE_MOUSE) {
                if (xhci_int_in_submit(e->controller, e->slot_id, e->dci,
                                       e->report_buf_phys, e->report_len) == 0)
                    e->outstanding = 1;
            }
            usblog_write("  slot %d iface %d: HID endpoint DCI %d already attached "
                         "(index %d) - dedupe, TD RE-ARMED (outstanding=%d)",
                         slot_id, iface_num, new_dci, i, e->outstanding);
            bootlog_write("[USB-HID] slot %d DCI %d already attached; dedupe + re-armed TD",
                          slot_id, new_dci);
            return i;
        }
    }

    // #134: claim a FREE entry (entries are released on disconnect), and make a
    // genuine table-full condition LOUD. The old code logged the refusal with
    // kprintf() only, so on the iMac (GUI mode, no serial, no sshd) a total input
    // lockout produced ZERO diagnostic in /BOOTLOG.TXT - the failure was visible
    // only as an ABSENCE of the usual attach lines. Never again.
    hid_device_t *dev = hid_alloc_entry();
    if (!dev) {
        kprintf("[USB-HID] HID TABLE FULL (%d/%d): slot %d iface %d REFUSED\n",
                hid_live_count(), MAX_HID_DEVICES, slot_id, iface_num);
        bootlog_write("[USB-HID] *** HID TABLE FULL (%d/%d) *** slot %d iface %d "
                      "REFUSED - this input device will NOT work",
                      hid_live_count(), MAX_HID_DEVICES, slot_id, iface_num);
        usblog_write("  slot %d iface %d: HID ATTACH REFUSED - table full (%d/%d)",
                     slot_id, iface_num, hid_live_count(), MAX_HID_DEVICES);
        return -1;
    }
    uint64_t reuse_buf = dev->report_buf_phys;   // keep any page this entry owns
    memset(dev, 0, sizeof(hid_device_t));
    dev->report_buf_phys = reuse_buf;
    dev->root_port = -1;
    dev->controller = xhc;
    dev->slot_id = slot_id;
    dev->interface_num = iface_num;
    dev->endpoint_in = ep_in;
    dev->max_packet_size = ep_in_mps > 0 ? ep_in_mps : 8;
    dev->poll_interval = b_interval > 0 ? b_interval : 8;
    dev->speed = speed;
    dev->protocol = HID_PROTOCOL_BOOT;
    dev->dci = (ep_in & 0x0F) * 2 + ((ep_in & 0x80) ? 1 : 0);

    // #162: fetch and parse the REPORT descriptor before deciding what this
    // interface IS. Done first because the interface descriptor's own
    // bInterfaceProtocol is not trustworthy: the owner's iMac keyboard
    // (05ac:024f) presents its media interface as bInterfaceProtocol 0x02,
    // "boot mouse", on a keyboard with no pointer of any kind. Believing that
    // byte is what bound it as a mouse, parsed its 16-byte media reports as
    // {buttons,x,y,wheel}, and left the volume keys doing nothing.
    memset(&dev->cons, 0, sizeof(dev->cons));
    dev->cons_prev = 0;
    dev->cons_hold_us = 0;
    dev->cons_next_rep_us = 0;
    dev->rd_len = (rd_len > 0 && rd_len <= HID_MAX_REPORT_DESC)
                      ? (uint16_t)rd_len : 0;
    if (dev->rd_len) {
        uint8_t *rd = (uint8_t *)kmalloc(dev->rd_len);
        if (rd) {
            int got = hid_fetch_report_desc(dev, rd, dev->rd_len);
            if (got > 0) {
                int n = got < dev->rd_len ? got : dev->rd_len;
                hid_log_report_desc(slot_id, iface_num, rd, n);
                hidrepd_parse_rs(rd, (uint32_t)n, &dev->cons);
                usblog_write("  slot %d iface %d: report-desc parse -> have=0x%02x "
                             "kind=%d id=%d top{consumer=%d mouse=%d keyboard=%d}",
                             slot_id, iface_num, dev->cons.have, dev->cons.kind,
                             dev->cons.report_id, dev->cons.top_consumer,
                             dev->cons.top_mouse, dev->cons.top_keyboard);
            } else {
                // Not fatal and not even unusual; say so rather than leaving a
                // silent gap that reads like the fetch was never attempted.
                usblog_write("  slot %d iface %d: GET_DESCRIPTOR(Report, %d) "
                             "returned %d - no report descriptor, behaviour "
                             "unchanged from before #162",
                             slot_id, iface_num, dev->rd_len, got);
            }
            kfree(rd);
        }
    }

    // #388 keyboard-detection hardening: the boot interface PROTOCOL (1=keyboard,
    // 2=mouse) is what determines the boot-report layout. Some keyboards/mice
    // (notably certain Apple and no-name USB keyboards) advertise the boot
    // protocol but report bInterfaceSubClass 0 instead of 1 (Boot). Previously
    // such a device was left HID_DEVICE_UNKNOWN and its input was never routed,
    // which is a prime suspect for "the keyboard enumerates but does not type".
    // We now key off the protocol regardless of subclass, and still issue
    // SET_PROTOCOL(Boot) below so the device sends 8-byte boot reports we parse.
    if (protocol == HID_BOOT_PROTOCOL_KEYBOARD) {
        dev->type = HID_DEVICE_KEYBOARD;
        kprintf("[USB-HID] USB keyboard (slot %d, EP 0x%02x, DCI %d, sub %d)\n",
                slot_id, ep_in, dev->dci, subclass);
        bootlog_write("[USB-HID] Keyboard attached: slot %d EP 0x%02x DCI %d speed %d mps %d sub %d%s",
                      slot_id, ep_in, dev->dci, speed, dev->max_packet_size, subclass,
                      subclass == HID_SUBCLASS_BOOT ? "" : " (non-boot subclass, forced boot)");
    } else if (protocol == HID_BOOT_PROTOCOL_MOUSE) {
        dev->type = HID_DEVICE_MOUSE;
        kprintf("[USB-HID] USB mouse (slot %d, EP 0x%02x, DCI %d, sub %d)\n",
                slot_id, ep_in, dev->dci, subclass);
        bootlog_write("[USB-HID] Mouse attached: slot %d EP 0x%02x DCI %d speed %d mps %d sub %d%s",
                      slot_id, ep_in, dev->dci, speed, dev->max_packet_size, subclass,
                      subclass == HID_SUBCLASS_BOOT ? "" : " (non-boot subclass, forced boot)");
    } else if (dev->cons.have) {
        // #162: a generic HID interface that the descriptor says carries
        // Consumer controls. This was the "input not routed" case: bound,
        // logged, then given no DMA page and no TD, so its reports were never
        // read. The owner's Logitech receiver presents exactly one of these.
        dev->type = HID_DEVICE_CONSUMER;
        kprintf("[USB-HID] Consumer/media HID (slot %d, sub %d proto %d, have 0x%02x)\n",
                slot_id, subclass, protocol, dev->cons.have);
        bootlog_write("[USB-HID] Consumer/media HID attached: slot %d iface %d "
                      "EP 0x%02x DCI %d have 0x%02x kind %d id %d",
                      slot_id, iface_num, ep_in, dev->dci, dev->cons.have,
                      dev->cons.kind, dev->cons.report_id);
    } else {
        dev->type = HID_DEVICE_UNKNOWN;
        kprintf("[USB-HID] HID device (slot %d, sub %d proto %d) - input not routed\n",
                slot_id, subclass, protocol);
        bootlog_write("[USB-HID] Unrouted HID device: slot %d sub %d proto %d", slot_id, subclass, protocol);
    }

    // #162: the ONE reclassification, and the conditions are deliberately
    // narrow. An interface that claims boot-MOUSE but whose own report
    // descriptor declares Consumer controls and NO Mouse collection is not a
    // mouse. Both halves matter: without the second, a real mouse that also
    // carries media buttons (plenty do) would lose its pointer. If the
    // descriptor was never fetched, cons.have is 0 and this cannot fire, so
    // every device on every machine behaves exactly as it did before #162
    // unless its own descriptor says otherwise.
    //
    // Boot-KEYBOARD interfaces are never reclassified, whatever their
    // descriptor says: that is the one interface a machine cannot afford to
    // get wrong, and boot protocol cannot carry consumer usages anyway.
    if (protocol == HID_BOOT_PROTOCOL_MOUSE && dev->cons.have &&
        !dev->cons.top_mouse) {
        dev->type = HID_DEVICE_CONSUMER;
        kprintf("[USB-HID] slot %d iface %d: declared boot-MOUSE but its report "
                "descriptor has Consumer controls and no Mouse collection; "
                "binding as media keys, not a pointer\n", slot_id, iface_num);
        bootlog_write("[USB-HID] #162 slot %d iface %d RECLASSIFIED boot-mouse -> "
                      "consumer/media (report descriptor has Consumer, no Mouse). "
                      "This interface used to inject its media reports as "
                      "{buttons,x,y,wheel} pointer input.", slot_id, iface_num);
    }

    // Boot protocol + report-on-change (idle 0). These go over EP0 (control).
    // #433 (re-scoped) Part A: record the SET_PROTOCOL(boot)/SET_IDLE results to
    // /USBLOG.TXT. A boot-only keyboard that NAKs/STALLs SET_PROTOCOL, or reports
    // in report-protocol layout instead of the 8-byte boot layout we parse, is a
    // prime keyboard-specific failure and only visible in these result codes.
    // #162: a consumer interface must NOT be put into boot protocol. Boot
    // protocol means "send the fixed 8-byte keyboard or 3-byte mouse report",
    // which is the opposite of what we want to read here and is exactly what
    // was being asked of the owner's media interface. SET_PROTOCOL is only
    // defined for a boot-subclass interface at all, so a non-boot one is left
    // entirely alone rather than sent a request it is entitled to STALL.
    int sp;
    if (dev->type == HID_DEVICE_CONSUMER) {
        sp = (subclass == HID_SUBCLASS_BOOT)
                 ? usb_hid_set_protocol(dev, HID_PROTOCOL_REPORT) : 0;
        dev->protocol = HID_PROTOCOL_REPORT;
    } else {
        sp = usb_hid_set_protocol(dev, HID_PROTOCOL_BOOT);
    }
    int si = usb_hid_set_idle(dev, 0, 0);
    usblog_write("  slot %d iface %d DCI %d: bound as %s; SET_PROTOCOL(boot)=%s "
                 "SET_IDLE(0)=%s report_len(pending)", slot_id, iface_num, dev->dci,
                 dev->type == HID_DEVICE_KEYBOARD ? "keyboard" :
                 dev->type == HID_DEVICE_MOUSE ? "mouse" :
                 dev->type == HID_DEVICE_CONSUMER ? "consumer/media" : "unrouted-HID",
                 sp >= 0 ? "OK" : "FAIL", si >= 0 ? "OK" : "FAIL");

    // #133: only a routed device is ever polled, so do not burn a 4 KiB DMA page
    // on an unrouted HID interface. The owner's Logitech receiver presents one
    // (iface 1, generic HID, DCI 5): it was allocated a page it never used.
    if (dev->type != HID_DEVICE_KEYBOARD && dev->type != HID_DEVICE_MOUSE &&
        dev->type != HID_DEVICE_CONSUMER) {   // #162
        dev->in_use = 1;
        hid_device_count = hid_live_count();
        return (int)(dev - hid_devices);
    }

    // Allocate a DMA report buffer (identity-mapped physical page), REUSING the
    // one this entry already owns from a previous binding (#134: the page is
    // never handed back, precisely so a detach cannot race the lock-free poll
    // worker into a use-after-free).
    if (dev->report_buf_phys == 0) {
        dev->report_buf_phys = pmm_alloc_pages(1);
        if (dev->report_buf_phys == 0) {
            // #135: the OTHER silent refusal. usb_hid_attach() can return -1 for
            // two reasons and only the table-full one was made loud (#134); this
            // one still reported to kprintf() alone, which does not exist on the
            // iMac (GUI mode, no serial). A refusal that reaches no persistent log
            // presents as input that simply stops working, with the failure
            // visible only as an ABSENCE of the usual attach lines. Never again,
            // for EITHER reason.
            kprintf("[USB-HID] Failed to allocate report buffer\n");
            bootlog_write("[USB-HID] *** slot %d iface %d: DMA report-buffer "
                          "allocation FAILED - HID bind REFUSED, this input device "
                          "will NOT work ***", slot_id, iface_num);
            usblog_write("  slot %d iface %d: HID ATTACH REFUSED - report buffer "
                         "allocation failed", slot_id, iface_num);
            return -1;
        }
    }
    dev->report_buf = (uint8_t *)dev->report_buf_phys;
    memset(dev->report_buf, 0, PAGE_SIZE);

    // #133 BABBLE. This used to request a FIXED 4 bytes for any mouse (8 for a
    // keyboard). Requesting FEWER bytes than the device actually sends is a
    // Babble Detected / buffer-overrun condition on real hardware, and xHCI
    // leaves the endpoint HALTED afterwards - which, with no halt recovery in
    // this driver until now, killed the device permanently. The owner's Logitech
    // G7 reports on an 8-byte-mps endpoint and was being asked for 4. Request the
    // endpoint's FULL wMaxPacketSize instead: a device that sends less simply
    // completes Short Packet (already treated as success), and the parsers below
    // read only the leading bytes they need, so this can never under-read.
    dev->report_len = dev->max_packet_size > 0 ? dev->max_packet_size : 8;
    if (dev->report_len > 64) dev->report_len = 64;   // boot reports are tiny

    // Queue the first interrupt-IN TD so reports start flowing.
    if (dev->type == HID_DEVICE_KEYBOARD || dev->type == HID_DEVICE_MOUSE ||
        dev->type == HID_DEVICE_CONSUMER) {   // #162
        if (hid_arm(dev) == 0) {
            bootlog_write("[USB-HID] slot %d: first interrupt-IN TD submitted (report_len %d)",
                          slot_id, dev->report_len);
            usblog_write("  slot %d DCI %d: first interrupt-IN TD submitted "
                         "(report_len %d, mps %d) - polling this endpoint",
                         slot_id, dev->dci, dev->report_len, dev->max_packet_size);
        } else {
            bootlog_write("[USB-HID] slot %d: FAILED to submit first interrupt-IN TD", slot_id);
            usblog_write("  slot %d DCI %d: FAILED to submit first interrupt-IN TD",
                         slot_id, dev->dci);
        }
    }

    dev->in_use = 1;
    hid_device_count = hid_live_count();
    return (int)(dev - hid_devices);
}

// =============================================================================
// #134: detach / entry release
// =============================================================================
// Release every entry bound to (xhc, slot_id). Called by the xHCI port re-scan
// worker when a root port goes from connected to empty, so a replug reuses the
// entry instead of consuming a fresh one forever.
//
// LOCK-FREE SAFETY. The HID poll worker runs concurrently at ~250 Hz and takes
// no lock. We therefore only ever CLEAR in_use here and leave every other field
// (including report_buf_phys and its page) intact. The worst a racing poller can
// do is take one more pass over an entry we just released, on a slot that is
// gone: xhci_int_in_poll() reports nothing and it moves on. Nothing is freed, so
// there is no use-after-free window to lose.
int usb_hid_detach_slot(xhci_controller_t *xhc, int slot_id) {
    int released = 0;
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        hid_device_t *d = &hid_devices[i];
        if (!d->in_use || d->controller != xhc || d->slot_id != slot_id) continue;
        d->in_use = 0;
        d->outstanding = 0;
        d->failed = 0;
        d->consecutive_halts = 0;
        d->revivals = 0;        // #133: a real replug is a clean slate
        d->revive_skip = 0;
        released++;
        bootlog_write("[USB-HID] detached slot %d DCI %d (%s): entry %d released "
                      "for reuse", slot_id, d->dci,
                      d->type == HID_DEVICE_KEYBOARD ? "keyboard" :
                      d->type == HID_DEVICE_MOUSE ? "mouse" : "other", i);
    }
    if (released) {
        hid_device_count = hid_live_count();
        bootlog_write("[USB-HID] slot %d disconnected: %d entry(s) released, "
                      "%d/%d now in use", slot_id, released,
                      hid_device_count, MAX_HID_DEVICES);
    }
    return released;
}

// =============================================================================
// #133: un-park a device whose halt recovery was exhausted
// =============================================================================
// THE ASYMMETRY THIS EXISTS FOR. `failed` was terminal. Only usb_hid_attach()
// ever cleared it, and that runs only when a port goes from empty to occupied.
// A WIRED KEYBOARD never gets there because it never fails in the first place.
// A WIRELESS RECEIVER stays plugged in for ever while its radio link comes and
// goes, so it generates the USB Transaction Errors that halt an endpoint, hits
// HID_MAX_HALT_RECOVERIES consecutive failures, and is then dead until reboot,
// with an unaffected keyboard sitting on the same controller. That is exactly
// the reported "#133: G7 degrades to unresponsive, keyboard perfect".
//
// Runs on the xHCI RE-SCAN worker, not the HID poll worker, because the
// recovery may need a blocking control transfer and blocking the poll worker
// would freeze every input device for its duration.
//
// Bounded three ways so a device that is genuinely gone cannot make this thread
// issue command-ring operations for ever: an exponential-ish scan backoff, a
// hard cap on revivals per entry, and the pre-existing per-attempt halt cap
// that put it here.
#define HID_MAX_REVIVALS 32

int usb_hid_retry_failed(void) {
    int revived = 0;
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        hid_device_t *dev = &hid_devices[i];
        if (!dev->in_use || !dev->failed) continue;
        if (dev->type != HID_DEVICE_KEYBOARD && dev->type != HID_DEVICE_MOUSE) continue;
        if (dev->revive_skip) { dev->revive_skip--; continue; }
        if (dev->revivals >= HID_MAX_REVIVALS) continue;

        dev->revivals++;
        // Back off: 1 scan, then 2, 4, 8 ... capped at 20 scans (~60s at the
        // 3s re-scan interval). A device that comes back does so on the first
        // or second try; one that never will costs almost nothing.
        uint32_t back = dev->revivals < 6 ? (1u << (dev->revivals - 1)) : 20u;
        dev->revive_skip = back > 20u ? 20u : back;

        uint8_t why = xhci_xfer_last_error(dev->slot_id, dev->dci);

        // A STALL is a DEVICE-side halt. Reset Endpoint clears the HOST side
        // only; without CLEAR_FEATURE(ENDPOINT_HALT) the device stalls the very
        // next transaction and the recovery is a no-op that reports success.
        // usb_msc.c has done this for bulk endpoints since it was written; the
        // interrupt-IN path never did.
        int cleared = 0;
        if (why == CC_STALL_ERROR)
            cleared = (xhci_clear_endpoint_halt(dev->controller, dev->slot_id,
                                                dev->endpoint_in) >= 0);

        int rec = xhci_recover_endpoint(dev->controller, dev->slot_id, dev->dci);
        dev->outstanding = 0;
        dev->consecutive_halts = 0;
        dev->failed = 0;                    // give the poll worker the device back
        int armed = (hid_arm(dev) == 0);
        if (armed) revived++;

        bootlog_write("[USB-HID] #133: slot %d DCI %d (%s) un-parked, attempt %u/%d "
                      "(last error CC=%u%s, recover=%d, re-arm=%s); next retry in "
                      "%u scan(s)", dev->slot_id, dev->dci,
                      dev->type == HID_DEVICE_KEYBOARD ? "kbd" : "mouse",
                      dev->revivals, HID_MAX_REVIVALS, why,
                      why == CC_STALL_ERROR
                          ? (cleared ? ", device halt CLEARED" : ", device halt clear FAILED")
                          : "",
                      rec, armed ? "OK" : "FAILED", dev->revive_skip);
    }
    return revived;
}

int usb_hid_slot_has_device(xhci_controller_t *xhc, int slot_id) {
    for (int i = 0; i < MAX_HID_DEVICES; i++)
        if (hid_devices[i].in_use && hid_devices[i].controller == xhc &&
            hid_devices[i].slot_id == slot_id) return 1;
    return 0;
}

// =============================================================================
// #390 COMPOSITE-HID re-arm
// =============================================================================
// Called by xhci_configure_endpoint_ep after a Configure Endpoint command that
// re-asserted the endpoints already configured on a slot. Re-asserting an
// endpoint re-initializes its context on real controllers, which ORPHANS any
// interrupt-IN TD that was already in flight on it. The real iMac keyboard
// 1a2c:95f6 is a composite device: the keyboard endpoint (DCI 3) is armed FIRST
// (usb_hid_attach submits its first TD), then the mouse interface's CONFIG_EP
// (DCI 5) re-asserts DCI 3 and leaves the keyboard TD dead - no keystrokes ever
// arrive while the mouse works. We re-submit a fresh interrupt-IN TD + ring the
// doorbell for every already-armed HID endpoint on this slot EXCEPT the endpoint
// just configured (exclude_dci), so no keyboard transfer is left orphaned.
// Returns the number of endpoints re-armed. NO-OP for single-interface devices
// (only one HID endpoint on the slot == the excluded one) and for non-HID slots
// (e.g. a USB MSC whose bulk-out CONFIG_EP re-asserts its bulk-in: no HID device
// matches the slot, so nothing is re-armed).
int usb_hid_rearm_slot(xhci_controller_t *xhc, int slot_id, int exclude_dci) {
    int rearmed = 0;
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        hid_device_t *dev = &hid_devices[i];
        if (!dev->in_use) continue;
        if (dev->controller != xhc || dev->slot_id != slot_id) continue;
        if (dev->dci == exclude_dci) continue;
        if (dev->type != HID_DEVICE_KEYBOARD && dev->type != HID_DEVICE_MOUSE) continue;
        // Re-submit unconditionally: the previously submitted TD was orphaned by
        // the re-assert, so trust the doorbell kick rather than dev->outstanding.
        if (hid_arm(dev) == 0) {
            rearmed++;
            bootlog_write("[USB-HID] slot %d: re-armed interrupt-IN TD on DCI %d "
                          "(EP 0x%02x, %s) after composite CONFIG_EP",
                          slot_id, dev->dci, dev->endpoint_in,
                          dev->type == HID_DEVICE_KEYBOARD ? "keyboard" : "mouse");
        }
    }
    return rearmed;
}

// =============================================================================
// Polling and Report Processing
// =============================================================================

// Poll a single HID device: drain a completed interrupt-IN TD (if any), feed the
// kernel input queue, and resubmit. Non-blocking. Returns 1 if a report was
// processed, 0 if none pending, -1 on error.
// #162: decode one report against this device's consumer map and fire the
// media keys on their PRESS EDGE.
//
// Returns 1 if the report was recognised as a consumer report (matching report
// ID), so the caller can skip a pointer parse that would misread it. Returns 0
// otherwise, which on a mouse/keyboard means "this was your report, carry on".
//
// AUTO-REPEAT. A PS/2 keyboard gets repeat for free from the i8042, so holding
// volume-up there already ramps; USB reports a press once and then says nothing
// until release, so holding it would move the volume exactly one step. Users
// hold volume keys. So volume up/down repeat after HID_CONS_REPEAT_DELAY_MS and
// then every HID_CONS_REPEAT_MS. MUTE DELIBERATELY DOES NOT REPEAT: repeating a
// toggle just flickers it, and which state you land in would depend on how long
// you happened to hold the key.
//
// Timing comes from mono_us() (cpu/mono.h), the shared monotonic clock, NEVER
// timer_ticks - a KVM tick burst replays lost ticks all at once and would make
// every "N ticks from now" deadline fire immediately (see the #139 note above
// and blame.md).
#define HID_CONS_REPEAT_DELAY_MS 400
#define HID_CONS_REPEAT_MS       100

static int hid_consumer_report(hid_device_t *dev, uint32_t got) {
    if (!dev->cons.have || got == 0) return 0;

    // A report ID that does not match ours is another collection's report on
    // the same endpoint; hidrepd_decode_rs() returns 0 for it. Distinguish
    // "not ours" from "ours, nothing pressed" so a composite mouse's pointer
    // reports are still parsed as pointer reports.
    int is_ours = (dev->cons.report_id == 0) ||
                  (got >= 1 && dev->report_buf[0] == dev->cons.report_id);
    if (!is_ours) return 0;

    uint8_t mask = hidrepd_decode_rs(&dev->cons, dev->report_buf, got);
    uint8_t newly = (uint8_t)(mask & ~dev->cons_prev);
    uint32_t now = (uint32_t)mono_us();

    if (newly & HIDCONS_VOL_UP)   hid_consumer_emit(0x30);
    if (newly & HIDCONS_VOL_DOWN) hid_consumer_emit(0x2E);
    if (newly & HIDCONS_MUTE)     hid_consumer_emit(0x20);

    if (mask != dev->cons_prev) {
        dev->cons_prev = mask;
        dev->cons_hold_us = now;
        dev->cons_next_rep_us =
            mask ? now + HID_CONS_REPEAT_DELAY_MS * 1000u : 0;
        if (mask && bootlog_hid_media_lines < 16) {
            bootlog_hid_media_lines++;
            bootlog_write("[USB-HID] #162 slot %d media mask 0x%02x", dev->slot_id, mask);
        }
    } else if (mask && dev->cons_next_rep_us &&
               (int32_t)(now - dev->cons_next_rep_us) >= 0) {
        if (mask & HIDCONS_VOL_UP)   hid_consumer_emit(0x30);
        if (mask & HIDCONS_VOL_DOWN) hid_consumer_emit(0x2E);
        dev->cons_next_rep_us = now + HID_CONS_REPEAT_MS * 1000u;
    }
    return 1;
}

int usb_hid_poll(hid_device_t *dev) {
    if (!dev || !dev->controller) return -1;
    if (!dev->in_use) return 0;                       // #134: released entry
    if (dev->failed) return 0;                        // #133: recovery exhausted
    if (dev->type != HID_DEVICE_KEYBOARD && dev->type != HID_DEVICE_MOUSE &&
        dev->type != HID_DEVICE_CONSUMER) return 0;   // #162

    // If nothing is outstanding, (re)submit a TD.
    if (!dev->outstanding) {
        hid_arm(dev);
        return 0;
    }

    uint32_t got = 0;
    int r = xhci_int_in_poll(dev->controller, dev->slot_id, dev->dci,
                             &got, dev->report_len);
    if (r == 0) return 0;             // still pending
    dev->outstanding = 0;

    // -------------------------------------------------------------------------
    // #133: ENDPOINT HALT RECOVERY.
    // -------------------------------------------------------------------------
    // xHCI 4.10.2.1: a transfer completing with Stall, USB Transaction Error,
    // Babble Detected or Data Buffer Error leaves the endpoint in the HALTED
    // state. A halted endpoint executes NOTHING - enqueueing another TRB and
    // ringing the doorbell is silently ignored by the controller. Until now this
    // driver had NO recovery whatsoever: TRB_RESET_EP and TRB_SET_TR_DEQUEUE were
    // #defined in xhci.h with ZERO callers kernel-wide, and the code below simply
    // re-submitted onto the dead endpoint and set outstanding = 1. From then on
    // xhci_int_in_poll() returned "still pending" forever and the device was dead
    // until reboot, while every other HID device kept working normally - which is
    // exactly the reported "mouse starts off ok then stops responding" with an
    // unaffected keyboard. A wireless receiver generates these errors whenever its
    // radio link drops, so it hits this and a wired keyboard does not.
    //
    // Run the spec-mandated recovery instead: Reset Endpoint (halted -> stopped),
    // Set TR Dequeue Pointer (discard the aborted TD, restart the ring), then
    // re-arm. Bounded by HID_MAX_HALT_RECOVERIES so a device that is physically
    // gone cannot make the SHARED poll worker issue command-ring ops forever.
    if (r < 0) {
        dev->consecutive_halts++;
        if (dev->consecutive_halts > HID_MAX_HALT_RECOVERIES) {
            if (!dev->failed) {
                dev->failed = 1;
                dev->revive_skip = 0;   // #133: first retry on the very next scan
                bootlog_write("[USB-HID] slot %d DCI %d (%s): endpoint halt recovery "
                              "gave up after %d attempts, last error CC=%u; device "
                              "PARKED. #133: the re-scan worker will retry it (this "
                              "used to be terminal until a replug, which a wireless "
                              "receiver never gets)", dev->slot_id, dev->dci,
                              dev->type == HID_DEVICE_KEYBOARD ? "kbd" : "mouse",
                              HID_MAX_HALT_RECOVERIES,
                              xhci_xfer_last_error(dev->slot_id, dev->dci));
            }
            return -1;
        }
        bootlog_write("[USB-HID] slot %d DCI %d: interrupt-IN error; endpoint halt "
                      "recovery attempt %u/%d", dev->slot_id, dev->dci,
                      dev->consecutive_halts, HID_MAX_HALT_RECOVERIES);
        if (xhci_recover_endpoint(dev->controller, dev->slot_id, dev->dci) < 0)
            return -1;                 // retry next pass, still bounded
        hid_arm(dev);
        return -1;
    }

    if (r > 0 && got > 0) {
        // A real report proves the endpoint is healthy again, so a flaky link may
        // recover an unlimited number of times; only an unbroken run of failures
        // reaches the cap.
        dev->consecutive_halts = 0;
        // #133: and it proves the LAST un-park worked, so the revival budget is
        // forgiven too. Without this, a receiver that drops its link 32 times
        // over a long session would exhaust HID_MAX_REVIVALS and die anyway,
        // which is the same permanent-death bug with a bigger number in it.
        dev->revivals = 0;
        dev->revive_skip = 0;
        // Running per-device report counters, purely for boot-log visibility:
        // on a real machine with zero remote telemetry, this is the only way
        // to tell "zero HID reports ever arrived" (xHCI/enumeration problem)
        // apart from "reports arrive but something downstream drops them"
        // (a different bug). Logged sparsely (first + every 50th) so it
        // never becomes per-instruction tracing.
        dev->reports_seen++;
        hid_stat_report(dev);   // #139: latency/rate decomposition

#ifdef HID_PARKTEST
        // #133 DESTRUCTIVE PROOF ARM, test builds only (`make HID_PARKTEST=1`).
        // No VM halts a HID endpoint on its own - blame.md records that QEMU
        // does not even complete an in-flight interrupt-IN TD with a
        // transaction error on device_del - so usb_hid_retry_failed() would
        // otherwise ship having NEVER EXECUTED, which is the zero-callers trap
        // this tree keeps rediscovering. Park the device by hand at report 300
        // and again at 900, exactly as HID_MAX_HALT_RECOVERIES would, and watch
        // whether reports resume. Without the un-park they stop for ever.
        if (dev->reports_seen == 300 || dev->reports_seen == 900) {
            dev->failed = 1;
            dev->consecutive_halts = HID_MAX_HALT_RECOVERIES + 1;
            dev->revive_skip = 0;
            bootlog_write("[HIDPARKTEST] slot %d DCI %d FORCED into the parked state "
                          "at report %u; reports must now STOP until the re-scan "
                          "worker un-parks it", dev->slot_id, dev->dci,
                          dev->reports_seen);
        }
#endif

        if (usb_hid_report_log &&
            (dev->reports_seen == 1 || (dev->reports_seen % 50) == 0)) {
            bootlog_write("[USB-HID] slot %d (%s): report #%u received",
                          dev->slot_id,
                          dev->type == HID_DEVICE_KEYBOARD ? "keyboard" :
                          dev->type == HID_DEVICE_CONSUMER ? "consumer" : "mouse",
                          dev->reports_seen);
        }

        // #162: consumer decode runs FIRST and on every device that has a
        // map, not only on HID_DEVICE_CONSUMER ones. A composite interface
        // can carry both a pointer and media controls, disambiguated by
        // report ID; hid_consumer_report() returns 1 when it recognised the
        // report as a consumer one, and only then is the mouse parse skipped.
        // On a device with no media controls cons.have is 0 and this is one
        // predictable branch.
        int consumed_as_media = 0;
        if (dev->cons.have) consumed_as_media = hid_consumer_report(dev, got);

        if (dev->type == HID_DEVICE_CONSUMER) {
            /* handled entirely by hid_consumer_report() above */
        } else if (dev->type == HID_DEVICE_KEYBOARD) {
            memcpy(&dev->keyboard_report, dev->report_buf,
                   sizeof(hid_keyboard_report_t));
            if (usb_hid_report_log) {
                uint8_t *rb = dev->report_buf;
                bootlog_write("[USB-HID] slot %d KBD raw: %02x %02x %02x %02x %02x %02x %02x %02x",
                              dev->slot_id, rb[0],rb[1],rb[2],rb[3],rb[4],rb[5],rb[6],rb[7]);
            }
            usb_hid_process_keyboard(dev);
        } else if (!consumed_as_media) {   // mouse
            hid_mouse_report_t *m = (hid_mouse_report_t *)dev->report_buf;
            int wheel = (got >= 4) ? m->wheel : 0;
            mouse_inject_hid(m->x, m->y, m->buttons, wheel);
        }
    }

    // Resubmit for the next report.
    hid_arm(dev);
    return (r > 0) ? 1 : -1;
}

// Poll all attached HID devices once.
void usb_hid_poll_all(void) {
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (!hid_devices[i].in_use) continue;
        // Drain up to a few queued reports per device per pass to keep latency low.
        for (int n = 0; n < 8; n++) {
            if (usb_hid_poll(&hid_devices[i]) != 1) break;
        }
    }
}

// #139: is there a completion waiting on any endpoint this worker polls?
// A pure read (xhci_xfer_pending drains nothing and takes no lock), so it is
// legal as a wait-queue condition, which the wait macro evaluates with
// interrupts off.
static int hid_any_ready(void) {
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        hid_device_t *d = &hid_devices[i];
        if (!d->in_use || d->failed || !d->outstanding) continue;
        if (d->type != HID_DEVICE_KEYBOARD && d->type != HID_DEVICE_MOUSE &&
            d->type != HID_DEVICE_CONSUMER) continue;   // #162
        if (xhci_xfer_pending(d->slot_id, d->dci)) return 1;
    }
    return 0;
}

// Kernel worker: service HID devices. Suppress the xHCI per-transfer serial
// spam while running.
//
// #139 EVENT-DRIVEN, WITH THE POLL KEPT AS AN ALWAYS-ARMED BACKSTOP. This used
// to be `usb_hid_poll_all(); proc_sleep(4);` - a fixed ~250 Hz sweep, which
// #71 had already tightened from 8 ms in pursuit of the same lag. MEASURED on
// the current tree, the sweep costs a mouse 0.3-1.0 ms on average but 5-7 ms at
// the tail, and the tail is what a hand feels as a stutter. The worker now
// SLEEPS on the shared xHCI event-ring wait queue instead, and has three
// INDEPENDENT reasons to wake, in the CLAUDE.md preference order (a wake that
// can be lost is a hang, so redundancy first, timeout last):
//
//   1. xhci_msi_isr() bumps the sequence and wakes the queue on every
//      interrupt. This is the fast path and it is new: #139 armed the xHCI MSI
//      that this driver had never armed.
//   2. hid_any_ready() - the condition itself. The periodic drain workers wake
//      this same queue unconditionally on every pass, so a completion recorded
//      by ANY drainer is acted on here immediately, whether or not an
//      interrupt ever arrived.
//   3. HID_POLL_MS as the timeout, so on hardware with no working MSI at all
//      the behaviour degrades to EXACTLY the old 250 Hz sweep and not one bit
//      worse. That is the property to preserve: the real iMac's controller has
//      never been observed to interrupt, because until #139 nothing let it.
//
// Draining still happens HERE, in thread context, inside usb_hid_poll(); the
// interrupt only provides the wake. wait_event_timeout returns WAIT_OK /
// WAIT_TIMEOUT / WAIT_EINTR and every one of them leads to the same sweep, so
// the return value is deliberately not branched on: the condition is re-tested
// by doing the work, which cannot go stale.
#define HID_POLL_MS 4

// #156: THE SECOND #139 LEVER, and the reason it exists.
//
// The MSI gate in xhci.c covers the #139 change that a VM CANNOT execute. This
// covers the #139 change that a VM cannot execute DIFFERENTLY: the rewrite of
// this worker from `usb_hid_poll_all(); proc_sleep(4);` to a wait-queue sleep
// runs on every machine, VM and iMac alike, and a VM boot therefore says
// nothing about how it behaves on hardware whose tick source, port-flap rate
// and slot-recycling behaviour are all different. With the owner holding no
// bootable image and no way to rebuild, he needs to be able to take BOTH #139
// changes out independently, from the ESP, with no toolchain:
//
//   /XHCIMSI.OFF  (default) - #139's MSI is not armed
//   /XHCIMSI.TXT            - arm it
//   /XHCIHID.OFF            - this worker reverts to the exact pre-#139 sweep
//
// Default ON, unlike the MSI gate, and the asymmetry is deliberate: this code
// path HAS executed, on every VM boot since #139, whereas xhci_msi_isr() had
// executed nowhere. Gate what has never run; leave a lever on what has.
static int g_hid_evtdriven = 1;
static void hid_evtdriven_resolve(void) {
    extern fat_fs_t g_fat_fs;
    if (g_fat_fs.mounted && fat_exists(&g_fat_fs, "/XHCIHID.OFF")) {
        g_hid_evtdriven = 0;
        bootlog_write("[USB-HID] #156: /XHCIHID.OFF present -> reverting to the "
                      "pre-#139 fixed %dms sweep (no wait-queue sleep)", HID_POLL_MS);
    }
}

static void usb_hid_poll_worker(void *arg) {
    (void)arg;
    xhci_iso_quiet = 1;
    kprintf("[USB-HID] poll worker started (%d device(s))\n", hid_device_count);
    bootlog_write("[USB-HID] poll worker started (%d device(s), event-driven off the "
                  "xHCI event ring with a %dms backstop)", hid_device_count, HID_POLL_MS);
    // Hoisted: the wait macro evaluates its queue argument more than once, and
    // the queue is a fixed global inside xhci.c, so resolve it exactly once.
    wait_queue_head_t *evtq = (wait_queue_head_t *)xhci_event_waitq();
    uint64_t seen = xhci_msi_seq();
    hid_evtdriven_resolve();
    for (;;) {
        usb_hid_poll_all();
        if (!g_hid_evtdriven) {
            proc_sleep(HID_POLL_MS);   // #156: the exact pre-#139 sweep
            continue;
        }
        seen = xhci_msi_seq();
        (void)wait_event_timeout(evtq,
                                 hid_any_ready() || xhci_msi_seq() != seen,
                                 wq_ms_to_ticks(HID_POLL_MS));
    }
}

// #433: guard so the poll worker is started AT MOST ONCE. The #433 re-scan
// worker calls this again after enumerating a hot-plugged / recovered HID, and
// starting a second worker would double-submit interrupt-IN TDs on the same
// endpoints. The single running worker already polls every hid_devices[] entry,
// so a newly attached device is picked up automatically with no restart.
static int g_hid_poll_thread_started = 0;

// #703 (#71 iMac): proc_init() memset()s the whole proc_table AFTER early USB
// enumeration may have already proc_create()d this poll worker (the real iMac
// always has its keyboard/mouse present at boot), erasing that worker while the
// static above stays set -- so usb_hid_start_poll_thread() can never recreate
// it and HID input is dead at the login/user-select screen. main.c calls this
// exactly once right after proc_init() to clear the stale flag so the starter
// recreates the (now-wiped) worker in the correct post-proc_init order. Safe:
// at that point the scheduler is not running yet so no worker is executing, and
// the recreated worker polls all hid_devices[], so no second/duplicate worker.
void usb_hid_reset_poll_thread_guard(void) {
    g_hid_poll_thread_started = 0;
}

void usb_hid_start_poll_thread(void) {
    if (g_hid_poll_thread_started) {
        // Already running; it polls all hid_devices[], so any device attached
        // after boot (re-scan / hotplug) is serviced without a second thread.
        return;
    }
    hid_device_count = hid_live_count();
    if (hid_device_count <= 0) {
        kprintf("[USB-HID] no HID devices; poll thread not started\n");
        bootlog_write("[USB-HID] no HID devices enumerated; poll thread NOT started");
        // Do NOT set the started flag: a later #433 re-scan that enumerates a
        // HID can still start the worker.
        return;
    }
    // Log a keyboard/mouse breakdown so a single boot log makes it obvious
    // whether BOTH a keyboard and a mouse enumerated (the real-iMac failure was
    // only ONE HID device binding). The driver polls every attached device
    // independently, so N keyboards + M mice all work concurrently.
    int nkbd = 0, nmouse = 0, nother = 0;
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (!hid_devices[i].in_use) continue;
        if (hid_devices[i].type == HID_DEVICE_KEYBOARD) nkbd++;
        else if (hid_devices[i].type == HID_DEVICE_MOUSE) nmouse++;
        else nother++;
    }
    kprintf("[USB-HID] %d HID device(s): %d keyboard, %d mouse, %d other\n",
            hid_device_count, nkbd, nmouse, nother);
    bootlog_write("[USB-HID] enumerated %d HID device(s): %d keyboard(s), %d mouse(s), %d other",
                  hid_device_count, nkbd, nmouse, nother);
    // #389 COMPOSITE-HID: list every (slot, DCI, endpoint, type) the worker will
    // poll INDEPENDENTLY. For a composite keyboard (one device presenting a
    // keyboard interface AND a mouse interface on the SAME slot) this prints BOTH
    // slot-N endpoints, e.g. "slot 3 DCI 3 EP 0x81 (keyboard)" and
    // "slot 3 DCI 5 EP 0x82 (mouse)", so a single iMac boot log/DEVLOG proves the
    // keyboard endpoint is being polled and its reports routed to the keyboard
    // handler, not lost behind the mouse interface on the shared slot.
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        hid_device_t *d = &hid_devices[i];
        if (!d->in_use) continue;
        const char *t = d->type == HID_DEVICE_KEYBOARD ? "keyboard" :
                        d->type == HID_DEVICE_MOUSE    ? "mouse"    : "other";
        bootlog_write("[USB-HID] polling endpoint: slot %d DCI %d EP 0x%02x (%s)",
                      d->slot_id, d->dci, d->endpoint_in, t);
        kprintf("[USB-HID] polling endpoint: slot %d DCI %d EP 0x%02x (%s)\n",
                d->slot_id, d->dci, d->endpoint_in, t);
    }
    proc_create("usb_hid", usb_hid_poll_worker, NULL, PRIO_NORMAL);
    g_hid_poll_thread_started = 1;
}

// ---- #447 Cybersecurity: keystroke-injection monitor ------------------------
// Humans top out ~10-15 keys/sec (>=~60ms between keydowns). BadUSB/Rubber-Ducky
// injectors "type" far faster. We time consecutive NEW keydowns in timer ticks;
// a run of very-small gaps flags likely automated injection. Read by the
// Cybersecurity app via usb_hid_get_kbd_security(). Cheap arithmetic; never blocks.
extern volatile uint64_t timer_ticks;
static volatile uint32_t g_kbd_sec_total   = 0;
static volatile uint32_t g_kbd_sec_min_gap = 0xFFFFFFFFu;
static volatile uint32_t g_kbd_sec_peak    = 0;
static volatile uint32_t g_kbd_sec_run     = 0;
static volatile int      g_kbd_sec_inject  = 0;
static volatile uint64_t g_kbd_sec_last    = 0;
static void kbd_sec_note_keydown(void) {
    uint64_t now = timer_ticks;
    g_kbd_sec_total++;
    if (g_kbd_sec_last != 0) {
        uint64_t gap = now - g_kbd_sec_last;           // timer ticks
        if (gap < g_kbd_sec_min_gap) g_kbd_sec_min_gap = (uint32_t)gap;
        if (gap <= 2) {                                 // <= ~2 ticks apart = superhuman
            g_kbd_sec_run++;
            if (g_kbd_sec_run > g_kbd_sec_peak) g_kbd_sec_peak = g_kbd_sec_run;
            if (g_kbd_sec_run >= 12) g_kbd_sec_inject = 1;
        } else {
            g_kbd_sec_run = 0;
        }
    }
    g_kbd_sec_last = now;
}
void usb_hid_get_kbd_security(uint32_t *total, uint32_t *min_gap_ticks,
                             uint32_t *peak_burst, int *injection) {
    if (total)         *total = g_kbd_sec_total;
    if (min_gap_ticks) *min_gap_ticks = (g_kbd_sec_min_gap == 0xFFFFFFFFu) ? 0 : g_kbd_sec_min_gap;
    if (peak_burst)    *peak_burst = g_kbd_sec_peak;
    if (injection)     *injection = g_kbd_sec_inject;
}

void usb_hid_process_keyboard(hid_device_t *dev) {
    if (!dev || dev->type != HID_DEVICE_KEYBOARD) return;

    hid_keyboard_report_t *curr = &dev->keyboard_report;
    hid_keyboard_report_t *prev = &dev->prev_keyboard_report;

    // Check for modifier changes
    uint8_t mod_changed = curr->modifiers ^ prev->modifiers;
    if (mod_changed && global_key_callback) {
        for (int i = 0; i < 8; i++) {
            if (mod_changed & (1 << i)) {
                int pressed = (curr->modifiers & (1 << i)) ? 1 : 0;
                global_key_callback(0xE0 + i, pressed, curr->modifiers);
            }
        }
    }

    // Check for key releases
    for (int i = 0; i < 6; i++) {
        uint8_t old_key = prev->keycodes[i];
        if (old_key == 0) continue;

        int still_pressed = 0;
        for (int j = 0; j < 6; j++) {
            if (curr->keycodes[j] == old_key) {
                still_pressed = 1;
                break;
            }
        }

        if (!still_pressed && global_key_callback) {
            global_key_callback(old_key, 0, curr->modifiers);
        }
    }

    // Check for key presses
    for (int i = 0; i < 6; i++) {
        uint8_t new_key = curr->keycodes[i];
        if (new_key == 0) continue;

        int is_new = 1;
        for (int j = 0; j < 6; j++) {
            if (prev->keycodes[j] == new_key) {
                is_new = 0;
                break;
            }
        }

        if (is_new) {
            kbd_sec_note_keydown();   // #447 injection monitor
            if (global_key_callback) global_key_callback(new_key, 1, curr->modifiers);
        }
    }

    memcpy(prev, curr, sizeof(hid_keyboard_report_t));
}

void usb_hid_process_mouse(hid_device_t *dev) {
    if (!dev || dev->type != HID_DEVICE_MOUSE) return;

    hid_mouse_report_t *report = &dev->mouse_report;

    dev->mouse_x += report->x;
    dev->mouse_y += report->y;

    if (dev->mouse_x < 0) dev->mouse_x = 0;
    if (dev->mouse_y < 0) dev->mouse_y = 0;
    if (dev->mouse_x > 1920) dev->mouse_x = 1920;
    if (dev->mouse_y > 1080) dev->mouse_y = 1080;

    if (global_mouse_callback) {
        global_mouse_callback(dev->mouse_x, dev->mouse_y, report->buttons);
    }
}

// =============================================================================
// Keycode Conversion
// =============================================================================

char hid_keycode_to_ascii(uint8_t keycode, uint8_t modifiers) {
    if (keycode >= sizeof(hid_keycode_ascii_lower)) {
        return 0;
    }

    int shifted = (modifiers & (HID_MOD_LEFT_SHIFT | HID_MOD_RIGHT_SHIFT)) ? 1 : 0;
    char c = hid_keycode_ascii_lower[keycode];
    
    if (shifted && c >= 'a' && c <= 'z') {
        return c - 'a' + 'A';
    }
    
    return c;
}

// =============================================================================
// Device Getters and Callbacks
// =============================================================================

hid_device_t *usb_hid_get_keyboard(void) {
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (hid_devices[i].in_use && hid_devices[i].type == HID_DEVICE_KEYBOARD) {
            return &hid_devices[i];
        }
    }
    return NULL;
}

hid_device_t *usb_hid_get_mouse(void) {
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        if (hid_devices[i].in_use && hid_devices[i].type == HID_DEVICE_MOUSE) {
            return &hid_devices[i];
        }
    }
    return NULL;
}

void usb_hid_set_key_callback(void (*callback)(uint8_t keycode, int pressed, uint8_t modifiers)) {
    global_key_callback = callback;
}

void usb_hid_set_mouse_callback(void (*callback)(int32_t x, int32_t y, uint8_t buttons)) {
    global_mouse_callback = callback;
}

// =============================================================================
// Debug
// =============================================================================

void usb_hid_print_devices(void) {
    kprintf("\n[USB-HID] Device List:\n");
    for (int i = 0; i < MAX_HID_DEVICES; i++) {
        hid_device_t *dev = &hid_devices[i];
        if (!dev->in_use) continue;
        const char *type_name;
        switch (dev->type) {
            case HID_DEVICE_KEYBOARD: type_name = "Keyboard"; break;
            case HID_DEVICE_MOUSE:    type_name = "Mouse"; break;
            case HID_DEVICE_GAMEPAD:  type_name = "Gamepad"; break;
            default:                  type_name = "Unknown"; break;
        }
        kprintf("  %d: %s (slot %d)\n", i, type_name, dev->slot_id);
    }
    if (hid_device_count == 0) {
        kprintf("  (no devices)\n");
    }
}
