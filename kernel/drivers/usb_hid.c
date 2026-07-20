// usb_hid.c - USB Human Interface Device (HID) Class Driver
#include "usb_hid.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../proc/process.h"
#include "../fs/bootlog.h"

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
// #307: HID usage code -> PS/2 set-1 scancode translation
// =============================================================================
// USB HID keyboards report boot-protocol "usage" codes. The rest of MayteraOS
// consumes PS/2 set-1 scancodes (translated in cpu/isr.c). We convert here and
// feed keyboard_process_scancode() so USB and PS/2 keyboards are identical to
// every app/compositor downstream. Table maps HID usage (0x00-0x73) -> set-1
// make code (0 == no mapping). Extended (0xE0-prefixed) keys handled separately.
static const uint8_t hid_to_set1[0x74] = {
    [0x04]=0x1E,[0x05]=0x30,[0x06]=0x2E,[0x07]=0x20,[0x08]=0x12,[0x09]=0x21,
    [0x0A]=0x22,[0x0B]=0x23,[0x0C]=0x17,[0x0D]=0x24,[0x0E]=0x25,[0x0F]=0x26,
    [0x10]=0x32,[0x11]=0x31,[0x12]=0x18,[0x13]=0x19,[0x14]=0x10,[0x15]=0x13,
    [0x16]=0x1F,[0x17]=0x14,[0x18]=0x16,[0x19]=0x2F,[0x1A]=0x11,[0x1B]=0x2D,
    [0x1C]=0x15,[0x1D]=0x2C,
    [0x1E]=0x02,[0x1F]=0x03,[0x20]=0x04,[0x21]=0x05,[0x22]=0x06,[0x23]=0x07,
    [0x24]=0x08,[0x25]=0x09,[0x26]=0x0A,[0x27]=0x0B,
    [0x28]=0x1C,[0x29]=0x01,[0x2A]=0x0E,[0x2B]=0x0F,[0x2C]=0x39,
    [0x2D]=0x0C,[0x2E]=0x0D,[0x2F]=0x1A,[0x30]=0x1B,[0x31]=0x2B,
    [0x33]=0x27,[0x34]=0x28,[0x35]=0x29,[0x36]=0x33,[0x37]=0x34,[0x38]=0x35,
    [0x39]=0x3A,
    [0x3A]=0x3B,[0x3B]=0x3C,[0x3C]=0x3D,[0x3D]=0x3E,[0x3E]=0x3F,[0x3F]=0x40,
    [0x40]=0x41,[0x41]=0x42,[0x42]=0x43,[0x43]=0x44,[0x44]=0x57,[0x45]=0x58,
    [0x53]=0x45,   // Num Lock
    [0x54]=0x35,   // Keypad / (approx)
    [0x55]=0x37,   // Keypad *
    [0x56]=0x4A,   // Keypad -
    [0x57]=0x4E,   // Keypad +
};

// Extended (E0-prefixed) navigation keys: HID usage -> set-1 code.
static uint8_t hid_ext_set1(uint8_t usage) {
    switch (usage) {
        case 0x49: return 0x52; // Insert
        case 0x4A: return 0x47; // Home
        case 0x4B: return 0x49; // Page Up
        case 0x4C: return 0x53; // Delete
        case 0x4D: return 0x4F; // End
        case 0x4E: return 0x51; // Page Down
        case 0x4F: return 0x4D; // Right
        case 0x50: return 0x4B; // Left
        case 0x51: return 0x50; // Down
        case 0x52: return 0x48; // Up
        case 0x58: return 0x1C; // Keypad Enter
        default:   return 0x00;
    }
}

static void emit_set1(uint8_t code, int extended, int pressed) {
    if (!code) return;
    if (extended) keyboard_process_scancode(0xE0);
    keyboard_process_scancode(pressed ? code : (uint8_t)(code | 0x80));
}

// Kernel key callback: usb_hid_process_keyboard fires this on each key/modifier
// transition. keycode 0xE0-0xE7 are the 8 modifier bits; everything else is a
// HID usage code.
static void hid_kernel_key_cb(uint8_t keycode, int pressed, uint8_t modifiers) {
    (void)modifiers;
    if (keycode >= 0xE0 && keycode <= 0xE7) {
        switch (keycode) {
            case 0xE0: emit_set1(0x1D, 0, pressed); break; // Left Ctrl
            case 0xE1: emit_set1(0x2A, 0, pressed); break; // Left Shift
            case 0xE2: emit_set1(0x38, 0, pressed); break; // Left Alt
            case 0xE3: emit_set1(0x5B, 1, pressed); break; // Left GUI
            case 0xE4: emit_set1(0x1D, 1, pressed); break; // Right Ctrl
            case 0xE5: emit_set1(0x36, 0, pressed); break; // Right Shift
            case 0xE6: emit_set1(0x38, 1, pressed); break; // Right Alt (AltGr)
            case 0xE7: emit_set1(0x5C, 1, pressed); break; // Right GUI
        }
        return;
    }
    uint8_t ext = hid_ext_set1(keycode);
    if (ext) { emit_set1(ext, 1, pressed); return; }
    if (keycode < sizeof(hid_to_set1)) {
        emit_set1(hid_to_set1[keycode], 0, pressed);
    }
}

// =============================================================================
// Global State
// =============================================================================

#define MAX_HID_DEVICES 8
static hid_device_t hid_devices[MAX_HID_DEVICES];
static int hid_device_count = 0;

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
    memset(hid_devices, 0, sizeof(hid_devices));
    // #307: route USB keyboard transitions into the shared kernel input path.
    global_key_callback = hid_kernel_key_cb;
}

// =============================================================================
// HID Class Requests
// =============================================================================

int usb_hid_set_protocol(hid_device_t *dev, uint8_t protocol) {
    if (!dev || !dev->controller) return -1;

    int result = xhci_control_transfer(dev->controller, dev->slot_id,
        0x21,                   // bmRequestType: Host to Device, Class, Interface
        HID_REQ_SET_PROTOCOL,   // bRequest
        protocol,               // wValue: Protocol (0=Boot, 1=Report)
        dev->interface_num,     // wIndex: Interface
        NULL, 0);

    if (result >= 0) {
        dev->protocol = protocol;
        kprintf("[USB-HID] Set protocol to %s\n", 
                protocol == HID_PROTOCOL_BOOT ? "Boot" : "Report");
    }

    return result;
}

int usb_hid_set_idle(hid_device_t *dev, uint8_t report_id, uint8_t duration) {
    if (!dev || !dev->controller) return -1;

    return xhci_control_transfer(dev->controller, dev->slot_id,
        0x21,                   // bmRequestType: Host to Device, Class, Interface
        HID_REQ_SET_IDLE,       // bRequest
        (duration << 8) | report_id, // wValue: Duration (upper) | Report ID (lower)
        dev->interface_num,     // wIndex: Interface
        NULL, 0);
}

int usb_hid_get_report(hid_device_t *dev, uint8_t type, uint8_t id,
                       void *buf, uint16_t len) {
    if (!dev || !dev->controller || !buf) return -1;

    return xhci_control_transfer(dev->controller, dev->slot_id,
        0xA1,                   // bmRequestType: Device to Host, Class, Interface
        HID_REQ_GET_REPORT,     // bRequest
        (type << 8) | id,       // wValue: Report Type (upper) | Report ID (lower)
        dev->interface_num,     // wIndex: Interface
        buf, len);
}

int usb_hid_set_keyboard_leds(hid_device_t *dev, uint8_t leds) {
    if (!dev || !dev->controller || dev->type != HID_DEVICE_KEYBOARD) return -1;

    // LED report is Output report type
    uint8_t report[1] = { leds };

    return xhci_control_transfer(dev->controller, dev->slot_id,
        0x21,                   // bmRequestType: Host to Device, Class, Interface
        HID_REQ_SET_REPORT,     // bRequest
        (HID_REPORT_OUTPUT << 8) | 0, // wValue: Output Report, ID 0
        dev->interface_num,     // wIndex: Interface
        report, 1);
}

// =============================================================================
// HID Device Probe and Attach
// =============================================================================

int usb_hid_probe(xhci_controller_t *xhc, int slot_id,
                  uint8_t interface_class, uint8_t interface_subclass,
                  uint8_t interface_protocol) {
    if (hid_device_count >= MAX_HID_DEVICES) {
        kprintf("[USB-HID] Maximum devices reached\n");
        return -1;
    }

    // Check if this is an HID device
    if (interface_class != 0x03) {  // HID class
        return -1;
    }

    hid_device_t *dev = &hid_devices[hid_device_count];
    memset(dev, 0, sizeof(hid_device_t));

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

    hid_device_count++;
    return hid_device_count - 1;
}

// =============================================================================
// #307: Full attach (interrupt-IN endpoint already configured by the xHCI layer)
// =============================================================================

int usb_hid_attach(xhci_controller_t *xhc, int slot_id, int iface_num,
                   int ep_in, int ep_in_mps, int b_interval, int speed,
                   uint8_t subclass, uint8_t protocol) {
    if (hid_device_count >= MAX_HID_DEVICES) {
        kprintf("[USB-HID] Maximum devices reached\n");
        return -1;
    }
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
    for (int i = 0; i < hid_device_count; i++) {
        hid_device_t *e = &hid_devices[i];
        if (e->controller == xhc && e->slot_id == slot_id && e->dci == new_dci) {
            usblog_write("  slot %d iface %d: HID endpoint DCI %d already attached "
                         "(index %d) - not re-attaching (dedupe)",
                         slot_id, iface_num, new_dci, i);
            bootlog_write("[USB-HID] slot %d DCI %d already attached; dedupe (no re-attach)",
                          slot_id, new_dci);
            return i;
        }
    }

    hid_device_t *dev = &hid_devices[hid_device_count];
    memset(dev, 0, sizeof(hid_device_t));
    dev->controller = xhc;
    dev->slot_id = slot_id;
    dev->interface_num = iface_num;
    dev->endpoint_in = ep_in;
    dev->max_packet_size = ep_in_mps > 0 ? ep_in_mps : 8;
    dev->poll_interval = b_interval > 0 ? b_interval : 8;
    dev->speed = speed;
    dev->protocol = HID_PROTOCOL_BOOT;
    dev->dci = (ep_in & 0x0F) * 2 + ((ep_in & 0x80) ? 1 : 0);

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
    } else {
        dev->type = HID_DEVICE_UNKNOWN;
        kprintf("[USB-HID] HID device (slot %d, sub %d proto %d) - input not routed\n",
                slot_id, subclass, protocol);
        bootlog_write("[USB-HID] Unrouted HID device: slot %d sub %d proto %d", slot_id, subclass, protocol);
    }

    // Boot protocol + report-on-change (idle 0). These go over EP0 (control).
    // #433 (re-scoped) Part A: record the SET_PROTOCOL(boot)/SET_IDLE results to
    // /USBLOG.TXT. A boot-only keyboard that NAKs/STALLs SET_PROTOCOL, or reports
    // in report-protocol layout instead of the 8-byte boot layout we parse, is a
    // prime keyboard-specific failure and only visible in these result codes.
    int sp = usb_hid_set_protocol(dev, HID_PROTOCOL_BOOT);
    int si = usb_hid_set_idle(dev, 0, 0);
    usblog_write("  slot %d iface %d DCI %d: bound as %s; SET_PROTOCOL(boot)=%s "
                 "SET_IDLE(0)=%s report_len(pending)", slot_id, iface_num, dev->dci,
                 dev->type == HID_DEVICE_KEYBOARD ? "keyboard" :
                 dev->type == HID_DEVICE_MOUSE ? "mouse" : "unrouted-HID",
                 sp >= 0 ? "OK" : "FAIL", si >= 0 ? "OK" : "FAIL");

    // Allocate a DMA report buffer (identity-mapped physical page).
    dev->report_buf_phys = pmm_alloc_pages(1);
    if (dev->report_buf_phys == 0) {
        kprintf("[USB-HID] Failed to allocate report buffer\n");
        return -1;
    }
    dev->report_buf = (uint8_t *)dev->report_buf_phys;
    memset(dev->report_buf, 0, PAGE_SIZE);
    dev->report_len = (dev->type == HID_DEVICE_KEYBOARD) ? 8 : 4;
    if (dev->report_len > dev->max_packet_size && dev->max_packet_size > 0)
        dev->report_len = dev->max_packet_size;

    // Queue the first interrupt-IN TD so reports start flowing.
    if (dev->type == HID_DEVICE_KEYBOARD || dev->type == HID_DEVICE_MOUSE) {
        if (xhci_int_in_submit(xhc, slot_id, dev->dci,
                               dev->report_buf_phys, dev->report_len) == 0) {
            dev->outstanding = 1;
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

    hid_device_count++;
    return hid_device_count - 1;
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
    for (int i = 0; i < hid_device_count; i++) {
        hid_device_t *dev = &hid_devices[i];
        if (dev->controller != xhc || dev->slot_id != slot_id) continue;
        if (dev->dci == exclude_dci) continue;
        if (dev->type != HID_DEVICE_KEYBOARD && dev->type != HID_DEVICE_MOUSE) continue;
        // Re-submit unconditionally: the previously submitted TD was orphaned by
        // the re-assert, so trust the doorbell kick rather than dev->outstanding.
        if (xhci_int_in_submit(dev->controller, dev->slot_id, dev->dci,
                               dev->report_buf_phys, dev->report_len) == 0) {
            dev->outstanding = 1;
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
int usb_hid_poll(hid_device_t *dev) {
    if (!dev || !dev->controller) return -1;
    if (dev->type != HID_DEVICE_KEYBOARD && dev->type != HID_DEVICE_MOUSE) return 0;

    // If nothing is outstanding, (re)submit a TD.
    if (!dev->outstanding) {
        if (xhci_int_in_submit(dev->controller, dev->slot_id, dev->dci,
                               dev->report_buf_phys, dev->report_len) == 0) {
            dev->outstanding = 1;
        }
        return 0;
    }

    uint32_t got = 0;
    int r = xhci_int_in_poll(dev->controller, dev->slot_id, dev->dci,
                             &got, dev->report_len);
    if (r == 0) return 0;             // still pending
    dev->outstanding = 0;

    if (r > 0 && got > 0) {
        // Running per-device report counters, purely for boot-log visibility:
        // on a real machine with zero remote telemetry, this is the only way
        // to tell "zero HID reports ever arrived" (xHCI/enumeration problem)
        // apart from "reports arrive but something downstream drops them"
        // (a different bug). Logged sparsely (first + every 50th) so it
        // never becomes per-instruction tracing.
        dev->reports_seen++;
        if (usb_hid_report_log &&
            (dev->reports_seen == 1 || (dev->reports_seen % 50) == 0)) {
            bootlog_write("[USB-HID] slot %d (%s): report #%u received",
                          dev->slot_id,
                          dev->type == HID_DEVICE_KEYBOARD ? "keyboard" : "mouse",
                          dev->reports_seen);
        }

        if (dev->type == HID_DEVICE_KEYBOARD) {
            memcpy(&dev->keyboard_report, dev->report_buf,
                   sizeof(hid_keyboard_report_t));
            if (usb_hid_report_log) {
                uint8_t *rb = dev->report_buf;
                bootlog_write("[USB-HID] slot %d KBD raw: %02x %02x %02x %02x %02x %02x %02x %02x",
                              dev->slot_id, rb[0],rb[1],rb[2],rb[3],rb[4],rb[5],rb[6],rb[7]);
            }
            usb_hid_process_keyboard(dev);
        } else {   // mouse
            hid_mouse_report_t *m = (hid_mouse_report_t *)dev->report_buf;
            int wheel = (got >= 4) ? m->wheel : 0;
            mouse_inject_hid(m->x, m->y, m->buttons, wheel);
        }
    }

    // Resubmit for the next report.
    if (xhci_int_in_submit(dev->controller, dev->slot_id, dev->dci,
                           dev->report_buf_phys, dev->report_len) == 0) {
        dev->outstanding = 1;
    }
    return (r > 0) ? 1 : -1;
}

// Poll all attached HID devices once.
void usb_hid_poll_all(void) {
    for (int i = 0; i < hid_device_count; i++) {
        // Drain up to a few queued reports per device per pass to keep latency low.
        for (int n = 0; n < 8; n++) {
            if (usb_hid_poll(&hid_devices[i]) != 1) break;
        }
    }
}

// Kernel worker: continuously poll HID devices. Suppress the xHCI per-transfer
// serial spam while running.
//
// #71 mouse-lag fix: the real Apple mouse (05ac:0304) reports bInterval=10
// (10ms / ~100Hz). The previous 8ms poll (~125Hz) already undercut that in
// theory, but only by a ~2ms margin -- on real hardware (scheduler/BKL
// contention, other USB traffic sharing the same xHCI event-ring trylock with
// xhci_drain_events) that margin can evaporate and reports end up sitting
// queued for a full extra pass. Tightening to 4ms (one PIT tick at 250Hz, the
// finest granularity proc_sleep can practically deliver) roughly doubles the
// safety margin over the fastest interrupt-IN endpoint on this box (the
// mouse) at negligible extra cost (a few MMIO reads + trylock attempt every
// 4ms is nothing next to a whole scheduler tick). This is still proc_sleep()
// -- a real cooperative yield, not a busy-spin (#426) -- and every completed
// TD is resubmitted immediately inside usb_hid_poll(), so the interrupt-IN
// endpoint is effectively kept continuously armed.
static void usb_hid_poll_worker(void *arg) {
    (void)arg;
    xhci_iso_quiet = 1;
    kprintf("[USB-HID] poll worker started (%d device(s))\n", hid_device_count);
    bootlog_write("[USB-HID] poll worker started (%d device(s), ~250Hz)", hid_device_count);
    for (;;) {
        usb_hid_poll_all();
        proc_sleep(4);   // 1 tick at 250Hz -> ~250Hz input polling
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
    for (int i = 0; i < hid_device_count; i++) {
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
    for (int i = 0; i < hid_device_count; i++) {
        hid_device_t *d = &hid_devices[i];
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
    for (int i = 0; i < hid_device_count; i++) {
        if (hid_devices[i].type == HID_DEVICE_KEYBOARD) {
            return &hid_devices[i];
        }
    }
    return NULL;
}

hid_device_t *usb_hid_get_mouse(void) {
    for (int i = 0; i < hid_device_count; i++) {
        if (hid_devices[i].type == HID_DEVICE_MOUSE) {
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
    for (int i = 0; i < hid_device_count; i++) {
        hid_device_t *dev = &hid_devices[i];
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
