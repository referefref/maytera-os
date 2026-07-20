// devlog.c - #388 comprehensive boot-time device inventory to /DEVLOG.TXT
//
// Real-hardware diagnostic (iMac14,4 live-USB): on the physical machine the user
// has no serial/SSH, so we need a complete, self-describing device inventory on
// the USB stick after boot. /BOOTLOG.TXT captures the running commentary;
// /DEVLOG.TXT is the structured snapshot: every PCI function, every xHCI root
// port, every enumerated USB device with its full descriptor tree (device /
// config / interfaces / endpoints), every USB hub with its downstream-port
// status (so a keyboard sitting behind an internal hub is visible even if it
// never bound), and the HD Audio codec identity (which lives on the HDA link,
// not PCI). This is what makes "why does the keyboard not work" answerable from
// a single file.
#include "devlog.h"
#include "../serial.h"
#include "../string.h"
#include "../version.h"
#include "../drivers/pci.h"
#include "../drivers/xhci.h"
#include "../drivers/hda.h"
#include <stdarg.h>

#define DEVLOG_PATH "/DEVLOG.TXT"
#define DEVLOG_CAP  (64 * 1024)

static char     g_devlog[DEVLOG_CAP];
static uint32_t g_devlog_len;
static int      g_devlog_full;

static void dl_puts(const char *s) {
    if (!s) return;
    uint32_t n = (uint32_t)strlen(s);
    if (g_devlog_len + n >= DEVLOG_CAP) { g_devlog_full = 1; return; }
    memcpy(g_devlog + g_devlog_len, s, n);
    g_devlog_len += n;
}

// printf-style line append (adds no newline; callers include one).
static void dl_line(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((uint32_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    dl_puts(buf);
}

// Callback handed to hda_devlog_scan(): append one codec line.
static void dl_emit_line(const char *line) {
    dl_puts("  ");
    dl_puts(line ? line : "");
    dl_puts("\n");
}

static const char *speed_short(int spd) {
    switch (spd) {
        case XHCI_SPEED_FULL:       return "FS(12M)";
        case XHCI_SPEED_LOW:        return "LS(1.5M)";
        case XHCI_SPEED_HIGH:       return "HS(480M)";
        case XHCI_SPEED_SUPER:      return "SS(5G)";
        case XHCI_SPEED_SUPER_PLUS: return "SS+(10G)";
        default:                    return "?";
    }
}

static const char *ep_type_name(uint8_t bmAttributes) {
    switch (bmAttributes & 0x03) {
        case 0: return "control";
        case 1: return "isoch";
        case 2: return "bulk";
        case 3: return "interrupt";
    }
    return "?";
}

// Best-effort human name for a USB (interface/device) class code.
static const char *usb_class_name(uint8_t cls) {
    switch (cls) {
        case 0x00: return "per-interface";
        case 0x01: return "Audio";
        case 0x02: return "CDC-Comm";
        case 0x03: return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Mass-Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0B: return "SmartCard";
        case 0x0E: return "Video";
        case 0xE0: return "Wireless";
        case 0xEF: return "Misc";
        case 0xFF: return "Vendor";
        default:   return "?";
    }
}

// Decode a HID interface protocol into a friendly tag.
static const char *hid_proto_name(uint8_t proto) {
    switch (proto) {
        case 0:  return "none";
        case 1:  return "keyboard";
        case 2:  return "mouse";
        default: return "?";
    }
}

// Walk a stored configuration descriptor and emit its interfaces + endpoints.
static void dl_dump_config(const uint8_t *cfg, int len) {
    if (len < 9) { dl_line("    (no config descriptor captured)\n"); return; }
    int total   = cfg[2] | (cfg[3] << 8);
    if (total > len) total = len;
    int nifaces = cfg[4];
    dl_line("    CONFIG: bNumInterfaces=%d wTotalLength=%d bmAttributes=0x%02x bMaxPower=%dmA\n",
            nifaces, total, cfg[7], cfg[8] * 2);
    int i = 9;
    while (i + 2 <= total) {
        int blen = cfg[i];
        int btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9) {          // INTERFACE
            uint8_t inum = cfg[i + 2], alt = cfg[i + 3], neps = cfg[i + 4];
            uint8_t icls = cfg[i + 5], isub = cfg[i + 6], iproto = cfg[i + 7];
            if (icls == 0x03) {
                dl_line("    IFACE %d alt %d: class 0x%02x(%s) sub 0x%02x proto 0x%02x(%s) nEP=%d\n",
                        inum, alt, icls, usb_class_name(icls), isub, iproto,
                        hid_proto_name(iproto), neps);
            } else {
                dl_line("    IFACE %d alt %d: class 0x%02x(%s) sub 0x%02x proto 0x%02x nEP=%d\n",
                        inum, alt, icls, usb_class_name(icls), isub, iproto, neps);
            }
        } else if (btype == 0x05 && blen >= 7) {   // ENDPOINT
            uint8_t eaddr = cfg[i + 2], eattr = cfg[i + 3];
            int emps = cfg[i + 4] | (cfg[i + 5] << 8);
            uint8_t eintv = cfg[i + 6];
            dl_line("      EP 0x%02x %s %s mps=%d interval=%d\n",
                    eaddr, (eaddr & 0x80) ? "IN " : "OUT",
                    ep_type_name(eattr), emps, eintv);
        } else if (btype == 0x21) {                // HID descriptor
            dl_line("      (HID descriptor, %d bytes)\n", blen);
        }
        i += blen;
    }
}

static void dl_dump_pci(void) {
    int n = pci_get_device_count();
    dl_line("=== PCI DEVICES (%d) ===\n", n);
    dl_line("  B:D.F  vendor:device  class:sub:progif  IRQ  BAR0        claimed-by       description\n");
    for (int i = 0; i < n; i++) {
        pci_device_t *d = pci_get_device(i);
        if (!d) continue;
        uint64_t bar0 = pci_get_bar_address(d, 0);
        // #418: explicit claimed-by-driver column, so the next boot's
        // /DEVLOG.TXT answers "does this device even have a driver" with
        // certainty (e.g. the iMac's Broadcom NIC/WiFi, for which this
        // kernel has no PCI driver at all) instead of an absence-of-evidence
        // argument built from grepping source for vendor strings.
        dl_line("  %02x:%02x.%x %04x:%04x    %02x:%02x:%02x        %3d  0x%08llx  %-16s %s\n",
                d->bus, d->slot, d->func, d->vendor_id, d->device_id,
                d->class_code, d->subclass, d->prog_if, d->interrupt_line,
                (unsigned long long)bar0,
                d->claimed ? d->claimed_by : "(none)",
                pci_get_class_name(d->class_code, d->subclass));
    }
    dl_puts("\n");
}

static void dl_dump_usb(void) {
    int nctrl = xhci_get_controller_count();
    dl_line("=== USB / xHCI (%d controller(s)) ===\n", nctrl);

    for (int c = 0; c < nctrl; c++) {
        xhci_controller_t *xhc = xhci_get_controller(c);
        if (!xhc) continue;
        dl_line("xHCI controller %d: %u root port(s), %u slots\n",
                c, xhc->max_ports, xhc->max_slots);
        for (int p = 0; p < (int)xhc->max_ports; p++) {
            int conn = 0, en = 0, spd = 0;
            if (!xhci_root_port_info(xhc, p, &conn, &en, &spd)) continue;
            dl_line("  root port %d: connected=%d enabled=%d port-speed=%d\n",
                    p + 1, conn, en, spd);
        }
    }
    dl_puts("\n");

    // Enumerated device tree (root-port + behind-hub devices, in enumerate order).
    int nd = xhci_get_enum_count();
    dl_line("--- ENUMERATED USB DEVICES (%d) ---\n", nd);
    if (nd == 0) dl_line("  (no USB devices enumerated)\n");
    for (int i = 0; i < nd; i++) {
        const xhci_enum_dev_t *e = xhci_get_enum_device(i);
        if (!e) continue;
        const uint8_t *dd = e->dev_desc;
        dl_line("[%s] slot %d root-port %d route 0x%05x depth %d speed %s%s\n",
                e->label[0] ? e->label : "?", e->slot_id, e->port + 1,
                (unsigned)e->route, e->depth, speed_short(e->speed),
                e->is_hub ? "  *HUB*" : "");
        dl_line("    DEVICE: %04x:%04x class 0x%02x(%s) sub 0x%02x proto 0x%02x "
                "bcdUSB %x.%02x mps0=%d numCfg=%d\n",
                e->vendor_id, e->product_id, e->dev_class,
                usb_class_name(e->dev_class), e->dev_subclass, e->dev_protocol,
                dd[3], dd[2], dd[7], dd[17]);
        dl_dump_config(e->cfg, e->cfg_len);
    }
    dl_puts("\n");

    // Hub inventory (bNbrPorts + downstream-port status, even for empty ports).
    int nh = xhci_get_hub_count();
    dl_line("--- USB HUBS (%d) ---\n", nh);
    if (nh == 0) dl_line("  (no USB hubs found)\n");
    for (int i = 0; i < nh; i++) {
        const xhci_hub_rec_t *h = xhci_get_hub_record(i);
        if (!h) continue;
        dl_line("HUB slot %d root-port %d route 0x%05x depth %d: %d downstream port(s) "
                "wHubCharacteristics=0x%04x\n",
                h->hub_slot, h->root_port + 1, (unsigned)h->route, h->depth,
                h->nports, h->hubchar);
        for (int p = 0; p < h->nports && p < 15; p++) {
            if (!h->ports[p].valid) {
                dl_line("    down-port %d: (not probed)\n", p + 1);
                continue;
            }
            dl_line("    down-port %d: status=0x%04x change=0x%04x connected=%d speed=%s\n",
                    p + 1, h->ports[p].status, h->ports[p].change,
                    h->ports[p].connected,
                    h->ports[p].connected ? speed_short(h->ports[p].speed) : "-");
        }
    }
    dl_puts("\n");
}

static void dl_dump_hda(void) {
    dl_line("=== HD AUDIO CODEC ===\n");
    hda_devlog_scan(dl_emit_line);
    dl_puts("\n");
}

void devlog_dump(fat_fs_t *fs) {
    if (!fs || !fs->mounted) {
        kprintf("[DEVLOG] filesystem not mounted; /DEVLOG.TXT not written\n");
        return;
    }
    g_devlog_len = 0;
    g_devlog_full = 0;

    dl_line("MayteraOS device inventory (/DEVLOG.TXT) - build %s v%s\n",
            MAYTERA_BUILD_DATE, MAYTERA_VERSION_STRING);
    dl_line("Generated at boot. See /BOOTLOG.TXT for the running enumeration log.\n\n");

    dl_dump_pci();
    dl_dump_usb();
    dl_dump_hda();

    if (g_devlog_full)
        dl_puts("\n[DEVLOG] NOTE: inventory truncated (buffer full)\n");

    int r = fat_write_file(fs, DEVLOG_PATH, g_devlog, g_devlog_len);
    kprintf("[DEVLOG] wrote %s (%u bytes, result %d): PCI=%d USB=%d HUBS=%d\n",
            DEVLOG_PATH, (unsigned)g_devlog_len, r,
            pci_get_device_count(), xhci_get_enum_count(), xhci_get_hub_count());
}
