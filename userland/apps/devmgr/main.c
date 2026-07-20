// devmgr - MayteraOS Device Manager (#325)
// A styled (Motif/material style engine + TTF) read-only system utility that
// enumerates hardware via the SYS_DEV_* / SYS_SYSINFO syscalls: System info,
// PCI devices, USB controllers/devices, and IRQ vectors. A left tree/category
// pane drives a right properties pane; a Refresh button re-queries the kernel.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_style.h"
#include "../../libc/theme.h"
#include "../../libc/syscall.h"
#include "../../libc/devinfo.h"
#include "../../libc/stdio.h"

#define WIN_W      800
#define WIN_H      560
#define PAD        10
#define TB_H       40
#define TREE_W     250
#define ROW_H      22
#define PROW_H     20

#define MAXPCI 64
#define MAXUSB 64
#define MAXIRQ 256
#define MAXROWS 512
#define MAXLINES 160
#define LINEW 96

static int win = -1, DW = WIN_W, DH = WIN_H;

// --- enumerated data --------------------------------------------------------
static devinfo_sysinfo_t g_sys;
static int g_sys_ok = 0;
static devinfo_pci_t g_pci[MAXPCI]; static int g_npci = 0;
static devinfo_usb_t g_usb[MAXUSB]; static int g_nusb = 0;
static devinfo_irq_t g_irq[MAXIRQ]; static int g_nirq = 0;

// --- tree state -------------------------------------------------------------
// categories: 0=System (leaf), 1=PCI, 2=USB, 3=IRQ (expandable)
static int expanded[4] = {0, 1, 1, 0};
static int sel_cat = 0, sel_item = -1;   // sel_item -1 == category header
static int tree_scroll = 0, prop_scroll = 0;

// visible-row model rebuilt every frame
static int row_cat[MAXROWS], row_item[MAXROWS], row_hdr[MAXROWS];
static int g_nrows = 0;

// property lines
static char g_lines[MAXLINES][LINEW];
static int g_nlines = 0;
static char g_title[LINEW];

#define LN(...) do { if (g_nlines < MAXLINES) { snprintf(g_lines[g_nlines], LINEW, __VA_ARGS__); g_nlines++; } } while (0)

// --- palette / style --------------------------------------------------------
static unsigned int C_BG, C_CARD, C_FIELD, C_BORDER, C_INK, C_DIM, C_ACC, C_SEL, C_SELTX;

static unsigned int lum_ink(unsigned int bg) {
    int r = (bg >> 16) & 255, g = (bg >> 8) & 255, b = bg & 255;
    return ((r * 30 + g * 59 + b * 11) / 100) > 140 ? 0x00181818u : 0x00F0F0F0u;
}
static unsigned int dim_ink(unsigned int bg) {
    unsigned int k = lum_ink(bg);
    int ir = (k >> 16) & 255, ig = (k >> 8) & 255, ib = k & 255;
    int br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    return (((ir + br) / 2) << 16) | (((ig + bgc) / 2) << 8) | ((ib + bb) / 2);
}
static unsigned int tint(unsigned int base, unsigned int acc, int pct) {
    int br = (base >> 16) & 255, bg = (base >> 8) & 255, bb = base & 255;
    int ar = (acc >> 16) & 255, ag = (acc >> 8) & 255, ab = acc & 255;
    return ((((br * (100 - pct) + ar * pct) / 100) & 255) << 16) |
           ((((bg * (100 - pct) + ag * pct) / 100) & 255) << 8) |
           (((bb * (100 - pct) + ab * pct) / 100) & 255);
}
static void apply_style(void) {
    int tid = theme_get_active();
    gui_set_style(tid == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    unsigned int wb = theme_color(THEME_COLOR_WINDOW_BG);
    int r = (wb >> 16) & 255, g = (wb >> 8) & 255, b = wb & 255;
    int dark = ((r * 30 + g * 59 + b * 11) / 100) < 128;
    C_ACC = theme_color(THEME_COLOR_ACCENT);
    C_BG    = tint(dark ? 0x00262A30 : 0x00F5F6F8, C_ACC, 5);
    C_CARD  = tint(dark ? 0x002C313B : 0x00EDEFF3, C_ACC, 6);
    C_FIELD = dark ? 0x00333A45 : 0x00FFFFFF;
    C_BORDER= dark ? 0x003A424F : 0x00CDD3DB;
    C_INK = lum_ink(C_BG); C_DIM = dim_ink(C_BG); C_SEL = C_ACC; C_SELTX = lum_ink(C_ACC);
    gui_palette_t p;
    p.surface = C_BG; p.surface_raised = C_CARD; p.ink = C_INK; p.ink_dim = C_DIM;
    p.accent = C_ACC; p.accent_hover = gui_lighten(C_ACC, 24); p.border = C_BORDER;
    p.field_bg = C_FIELD; p.field_border = C_BORDER; p.track = tint(C_BG, C_ACC, 20);
    gui_set_palette(&p);
}

// --- helpers ----------------------------------------------------------------
static const char *usb_type_name(unsigned t) {
    switch (t) {
        case DEVINFO_USB_UHCI: return "UHCI";
        case DEVINFO_USB_OHCI: return "OHCI";
        case DEVINFO_USB_EHCI: return "EHCI";
        case DEVINFO_USB_XHCI: return "xHCI";
        default: return "USB";
    }
}
static const char *usb_speed_name(unsigned s) {
    switch (s) { case 0: return "Full"; case 1: return "Low"; case 2: return "High"; case 3: return "Super"; default: return "?"; }
}
static const char *usb_class_name(unsigned c) {
    switch (c) {
        case 0x00: return "Per-interface";
        case 0x01: return "Audio";
        case 0x02: return "Communications";
        case 0x03: return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Mass Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC Data";
        case 0x0B: return "Smart Card";
        case 0x0E: return "Video";
        case 0xDC: return "Diagnostic";
        case 0xE0: return "Wireless";
        case 0xEF: return "Misc";
        case 0xFE: return "App Specific";
        case 0xFF: return "Vendor Specific";
        default: return "Unknown";
    }
}
static const char *brand_trim(const char *s) {
    while (*s == ' ') s++;
    return s;
}
static void feature_str(unsigned f, char *buf, int cap) {
    static const struct { unsigned bit; const char *n; } tbl[] = {
        {DEVINFO_FEAT_FPU, "FPU"}, {DEVINFO_FEAT_SSE, "SSE"}, {DEVINFO_FEAT_SSE2, "SSE2"},
        {DEVINFO_FEAT_SSE3, "SSE3"}, {DEVINFO_FEAT_SSSE3, "SSSE3"}, {DEVINFO_FEAT_SSE41, "SSE4.1"},
        {DEVINFO_FEAT_SSE42, "SSE4.2"}, {DEVINFO_FEAT_AVX, "AVX"}, {DEVINFO_FEAT_AVX2, "AVX2"},
        {DEVINFO_FEAT_FXSR, "FXSR"}, {DEVINFO_FEAT_APIC, "APIC"}, {DEVINFO_FEAT_HTT, "HTT"},
        {DEVINFO_FEAT_TSC, "TSC"}, {DEVINFO_FEAT_PAE, "PAE"}, {DEVINFO_FEAT_MSR, "MSR"},
        {DEVINFO_FEAT_LM, "LM64"},
    };
    int n = (int)(sizeof(tbl) / sizeof(tbl[0]));
    int pos = 0; buf[0] = 0;
    for (int i = 0; i < n; i++) {
        if (f & tbl[i].bit) {
            int wrote = snprintf(buf + pos, cap - pos, "%s%s", pos ? " " : "", tbl[i].n);
            if (wrote > 0) pos += wrote;
            if (pos >= cap - 8) break;
        }
    }
    if (!pos) snprintf(buf, cap, "(none)");
}

// --- data refresh -----------------------------------------------------------
static void refresh(void) {
    g_sys_ok = (sys_sysinfo(&g_sys) == 0);
    g_npci = sys_dev_pci_list(g_pci, MAXPCI); if (g_npci < 0) g_npci = 0;
    g_nusb = sys_dev_usb_list(g_usb, MAXUSB); if (g_nusb < 0) g_nusb = 0;
    g_nirq = sys_dev_irq_list(g_irq, MAXIRQ); if (g_nirq < 0) g_nirq = 0;
}

// --- visible tree rows ------------------------------------------------------
static int cat_count(int cat) {
    switch (cat) { case 1: return g_npci; case 2: return g_nusb; case 3: return g_nirq; default: return 0; }
}
static void build_rows(void) {
    g_nrows = 0;
    for (int c = 0; c < 4; c++) {
        if (g_nrows < MAXROWS) { row_cat[g_nrows] = c; row_item[g_nrows] = -1; row_hdr[g_nrows] = 1; g_nrows++; }
        if (c != 0 && expanded[c]) {
            int n = cat_count(c);
            for (int i = 0; i < n && g_nrows < MAXROWS; i++) {
                row_cat[g_nrows] = c; row_item[g_nrows] = i; row_hdr[g_nrows] = 0; g_nrows++;
            }
        }
    }
}
static void row_label(int cat, int item, int hdr, char *buf) {
    if (hdr) {
        switch (cat) {
            case 0: snprintf(buf, LINEW, "System"); break;
            case 1: snprintf(buf, LINEW, "PCI Devices (%d)", g_npci); break;
            case 2: snprintf(buf, LINEW, "USB Devices (%d)", g_nusb); break;
            case 3: snprintf(buf, LINEW, "IRQs (%d)", g_nirq); break;
        }
        return;
    }
    if (cat == 1) {
        const char *cn = g_pci[item].class_name[0] ? g_pci[item].class_name : "PCI Device";
        snprintf(buf, LINEW, "%s", cn);
    } else if (cat == 2) {
        if (g_usb[item].is_controller)
            snprintf(buf, LINEW, "%s Controller", usb_type_name(g_usb[item].controller_type));
        else
            snprintf(buf, LINEW, "%04x:%04x %s", g_usb[item].vendor_id, g_usb[item].product_id,
                     usb_class_name(g_usb[item].dev_class));
    } else if (cat == 3) {
        devinfo_irq_t *q = &g_irq[item];
        if (q->is_irq)
            snprintf(buf, LINEW, "Vec %u  IRQ %d", q->vector, (int)q->irq_line);
        else
            snprintf(buf, LINEW, "Vec %u", q->vector);
    }
}

// --- property pane content --------------------------------------------------
static void build_props(void) {
    g_nlines = 0;
    if (sel_cat == 0) {
        snprintf(g_title, LINEW, "System Information");
        if (!g_sys_ok) { LN("System information unavailable."); return; }
        LN("CPU: %s", brand_trim(g_sys.cpu_brand));
        LN("Vendor: %s", g_sys.cpu_vendor);
        LN("Logical CPUs: %u   (online %u)", g_sys.cpu_count, g_sys.cpu_online);
        char fb[LINEW]; feature_str(g_sys.features, fb, LINEW);
        LN("Features: %s", fb);
        LN("");
        LN("Physical Memory");
        LN("   Total: %lu MB", (unsigned long)(g_sys.mem_total / 1048576UL));
        LN("   Used:  %lu MB", (unsigned long)(g_sys.mem_used / 1048576UL));
        LN("   Free:  %lu MB", (unsigned long)(g_sys.mem_free / 1048576UL));
        LN("");
        LN("Kernel Heap");
        LN("   Total: %lu KB", (unsigned long)(g_sys.heap_total / 1024UL));
        LN("   Used:  %lu KB", (unsigned long)(g_sys.heap_used / 1024UL));
        LN("   Free:  %lu KB", (unsigned long)(g_sys.heap_free / 1024UL));
        LN("");
        LN("Timer: %u Hz", g_sys.timer_hz);
        LN("ACPI: %s", g_sys.acpi_present ? "Present" : "Absent");
        LN("Uptime: %lu s   (%lu ticks)", (unsigned long)(g_sys.uptime_ms / 1000UL),
           (unsigned long)g_sys.uptime_ticks);
        return;
    }
    if (sel_cat == 1) {
        if (sel_item < 0 || sel_item >= g_npci) {
            snprintf(g_title, LINEW, "PCI Devices");
            LN("%d PCI device(s) detected.", g_npci);
            LN("Select a device to view its properties.");
            return;
        }
        devinfo_pci_t *d = &g_pci[sel_item];
        snprintf(g_title, LINEW, "%s", d->class_name[0] ? d->class_name : "PCI Device");
        LN("Class:    %02x  Sub: %02x  ProgIF: %02x", d->class_code, d->subclass, d->prog_if);
        LN("Vendor:   %04x", d->vendor_id);
        LN("Device:   %04x", d->device_id);
        LN("Revision: %02x", d->revision);
        LN("Location: %02x:%02x.%x  (bus:slot.func)", d->bus, d->slot, d->func);
        if (d->irq_line == 0 || d->irq_line == 0xFF) LN("IRQ line: none");
        else LN("IRQ line: %u", d->irq_line);
        LN("");
        LN("Base Address Registers");
        int any = 0;
        for (int i = 0; i < 6; i++) {
            if (d->bar[i]) {
                const char *kind = (d->bar[i] & 1) ? "I/O " : "MMIO";
                LN("   BAR%d: %08x  %s", i, d->bar[i], kind);
                any = 1;
            }
        }
        if (!any) LN("   (none configured)");
        return;
    }
    if (sel_cat == 2) {
        if (sel_item < 0 || sel_item >= g_nusb) {
            snprintf(g_title, LINEW, "USB Devices");
            LN("%d USB row(s): host controllers + devices.", g_nusb);
            LN("Select a row to view its properties.");
            return;
        }
        devinfo_usb_t *u = &g_usb[sel_item];
        if (u->is_controller) {
            snprintf(g_title, LINEW, "%s Host Controller", usb_type_name(u->controller_type));
            LN("Type:  %s", usb_type_name(u->controller_type));
            LN("Index: %u", u->bus_port);
        } else {
            snprintf(g_title, LINEW, "USB Device %04x:%04x", u->vendor_id, u->product_id);
            LN("Vendor:  %04x", u->vendor_id);
            LN("Product: %04x", u->product_id);
            LN("Class:   %02x  Sub: %02x  Proto: %02x", u->dev_class, u->subclass, u->protocol);
            LN("Class name: %s", usb_class_name(u->dev_class));
            LN("Controller: %s", usb_type_name(u->controller_type));
            LN("Speed:   %s", usb_speed_name(u->speed));
            LN("Port:    %u", u->bus_port);
            LN("Address: %u", u->address);
            LN("Interfaces: %u   Endpoints: %u", u->num_interfaces, u->num_endpoints);
        }
        return;
    }
    if (sel_cat == 3) {
        if (sel_item < 0 || sel_item >= g_nirq) {
            snprintf(g_title, LINEW, "Interrupt Vectors");
            LN("%d interrupt vector(s) tracked.", g_nirq);
            LN("Select a vector to view its properties.");
            return;
        }
        devinfo_irq_t *q = &g_irq[sel_item];
        snprintf(g_title, LINEW, "Interrupt Vector %u", q->vector);
        LN("Vector:     %u", q->vector);
        if (q->is_irq) LN("Legacy IRQ: %d", (int)q->irq_line);
        else LN("Legacy IRQ: n/a");
        LN("Handler:    %s", q->has_handler ? "Registered" : "None");
        LN("Present:    %s", q->present ? "Yes" : "No");
        LN("Gate attr:  %02x", q->type_attr);
        LN("Count:      %lu", (unsigned long)q->count);
        return;
    }
}

// --- layout helpers ---------------------------------------------------------
static int tree_x(void)   { return PAD; }
static int tree_y(void)   { return PAD + TB_H; }
static int tree_w(void)   { return TREE_W; }
static int tree_h(void)   { int h = DH - tree_y() - PAD; return h < 40 ? 40 : h; }
static int prop_x(void)   { return PAD + TREE_W + 10; }
static int prop_y(void)   { return PAD + TB_H; }
static int prop_w(void)   { int w = DW - prop_x() - PAD; return w < 80 ? 80 : w; }
static int prop_h(void)   { int h = DH - prop_y() - PAD; return h < 40 ? 40 : h; }
static int tree_rows_vis(void) { return (tree_h() - 8) / ROW_H; }
static int prop_rows_vis(void) { return (prop_h() - 44) / PROW_H; }

// --- drawing ----------------------------------------------------------------
static void draw(void) {
    apply_style();
    win_get_size(win, &DW, &DH);
    if (DW < 360) DW = WIN_W;
    if (DH < 240) DH = WIN_H;
    build_rows();
    build_props();

    win_draw_rect(win, 0, 0, DW, DH, C_BG);

    // Toolbar
    win_draw_text_ttf(win, PAD + 2, 12, "Device Manager", 16, C_INK);
    if (g_sys_ok) {
        char sub[LINEW];
        snprintf(sub, LINEW, "%u CPUs  -  %lu MB RAM", g_sys.cpu_count,
                 (unsigned long)(g_sys.mem_total / 1048576UL));
        int sw = gui_ttf_width(sub, 12);
        win_draw_text_ttf(win, DW - 110 - 16 - sw, 16, sub, 12, C_DIM);
    }
    gui_button(win, DW - 110, 8, 100, 26, "Refresh", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    win_draw_rect(win, 0, TB_H + 2, DW, 1, C_BORDER);

    // Tree pane
    int tx = tree_x(), ty = tree_y(), tw = tree_w(), th = tree_h();
    gui_card(win, tx, ty, tw, th);
    int vis = tree_rows_vis();
    if (tree_scroll > g_nrows - vis) tree_scroll = g_nrows - vis;
    if (tree_scroll < 0) tree_scroll = 0;
    char lbl[LINEW];
    for (int rr = 0; rr < vis && (rr + tree_scroll) < g_nrows; rr++) {
        int idx = rr + tree_scroll;
        int cat = row_cat[idx], item = row_item[idx], hdr = row_hdr[idx];
        int ry = ty + 6 + rr * ROW_H;
        int selrow = (cat == sel_cat && item == sel_item);
        if (selrow) gui_fill_rounded_aa(win, tx + 3, ry, tw - 6, ROW_H - 2, 4, C_SEL, C_CARD);
        unsigned int ink = selrow ? C_SELTX : C_INK;
        unsigned int dink = selrow ? C_SELTX : C_DIM;
        row_label(cat, item, hdr, lbl);
        if (hdr) {
            if (cat != 0) {
                const char *gl = expanded[cat] ? "v" : ">";
                win_draw_text_ttf(win, tx + 8, ry + 4, gl, 12, dink);
            }
            win_draw_text_ttf(win, tx + 22, ry + 3, lbl, 13, ink);
        } else {
            win_draw_text_ttf(win, tx + 40, ry + 3, lbl, 12, ink);
        }
    }

    // Properties pane
    int px = prop_x(), py = prop_y(), pw = prop_w(), ph = prop_h();
    gui_card(win, px, py, pw, ph);
    unsigned int cink = lum_ink(C_CARD), cdim = dim_ink(C_CARD);
    win_draw_text_ttf(win, px + 14, py + 12, g_title, 15, cink);
    win_draw_rect(win, px + 12, py + 34, pw - 24, 1, C_BORDER);
    int pvis = prop_rows_vis();
    if (prop_scroll > g_nlines - pvis) prop_scroll = g_nlines - pvis;
    if (prop_scroll < 0) prop_scroll = 0;
    for (int rr = 0; rr < pvis && (rr + prop_scroll) < g_nlines; rr++) {
        int li = rr + prop_scroll;
        win_draw_text_ttf(win, px + 16, py + 44 + rr * PROW_H, g_lines[li], 12, cdim);
    }

    win_invalidate(win);
}

// --- hit testing ------------------------------------------------------------
static void click_tree(int lx, int ly) {
    int tx = tree_x(), ty = tree_y(), tw = tree_w();
    if (lx < tx || lx >= tx + tw) return;
    int rr = (ly - (ty + 6)) / ROW_H;
    if (rr < 0) return;
    int idx = rr + tree_scroll;
    if (idx < 0 || idx >= g_nrows) return;
    int cat = row_cat[idx], item = row_item[idx], hdr = row_hdr[idx];
    if (hdr && cat != 0) {
        expanded[cat] = !expanded[cat];
        sel_cat = cat; sel_item = -1; prop_scroll = 0;
    } else {
        sel_cat = cat; sel_item = item; prop_scroll = 0;
    }
}

int main(void) {
    win = win_create("Device Manager", 120, 70, WIN_W, WIN_H);
    if (win < 0) return 1;
    refresh();
    draw();
    int running = 1;
    while (running) {
        gui_event_t ev;
        int et = win_get_event(win, &ev, 2000);
        if (et == 0) { draw(); continue; }   // timeout: keep uptime fresh
        switch (ev.type) {
            case EVENT_REDRAW:
            case EVENT_RESIZE:
                draw(); break;
            case EVENT_WINDOW_CLOSE:
                running = 0; break;
            case EVENT_KEY_DOWN: {
                char c = ev.key_char; uint32_t kc = ev.keycode;
                if (c == 27) { running = 0; break; }
                if (c == 'r' || c == 'R') { refresh(); draw(); break; }
                if (kc == 0x80) {            // up: move selection in tree
                    for (int i = 1; i < g_nrows; i++) {
                        if (row_cat[i] == sel_cat && row_item[i] == sel_item) {
                            sel_cat = row_cat[i - 1]; sel_item = row_item[i - 1]; prop_scroll = 0; break;
                        }
                    }
                    draw();
                } else if (kc == 0x81) {     // down
                    for (int i = 0; i < g_nrows - 1; i++) {
                        if (row_cat[i] == sel_cat && row_item[i] == sel_item) {
                            sel_cat = row_cat[i + 1]; sel_item = row_item[i + 1]; prop_scroll = 0; break;
                        }
                    }
                    draw();
                } else if (kc == 0x82) {     // left: collapse
                    if (sel_cat != 0) { expanded[sel_cat] = 0; sel_item = -1; } draw();
                } else if (kc == 0x83) {     // right: expand
                    if (sel_cat != 0) { expanded[sel_cat] = 1; } draw();
                } else if (kc == 0x49) {     // page up
                    prop_scroll -= prop_rows_vis(); if (prop_scroll < 0) prop_scroll = 0; draw();
                } else if (kc == 0x51) {     // page down
                    prop_scroll += prop_rows_vis(); draw();
                }
                break;
            }
            case EVENT_MOUSE_DOWN: {
                int lx = ev.mouse_x, ly = ev.mouse_y;
                if (ly >= 8 && ly < 34 && lx >= DW - 110 && lx < DW - 10) { refresh(); draw(); break; }
                click_tree(lx, ly);
                draw();
                break;
            }
            case EVENT_MOUSE_SCROLL: {
                int d = ev.scroll_delta;
                int over_prop = (ev.mouse_x >= prop_x());
                if (over_prop) {
                    prop_scroll += (d > 0) ? 3 : -3;
                    if (prop_scroll < 0) prop_scroll = 0;
                } else {
                    tree_scroll += (d > 0) ? 1 : -1;
                    if (tree_scroll < 0) tree_scroll = 0;
                }
                draw();
                break;
            }
            default: break;
        }
    }
    win_destroy(win);
    return 0;
}
