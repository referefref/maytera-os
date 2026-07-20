// cybersecurity.c - MayteraOS Cybersecurity app
// ============================================================================
// Feature 1 (#447): BadUSB / USB threat detection.
//   - Static descriptor analysis of every enumerated USB device: flags the
//     classic BadUSB shapes (a HID KEYBOARD interface combined with mass-storage
//     or a vendor-specific interface = Rubber-Ducky-with-exfil), disguised
//     keyboards, and more-than-one-keyboard.
//   - Runtime keystroke-injection monitor: the HID keyboard path (drivers/
//     usb_hid.c) records inter-keystroke timing; a burst far above human typing
//     speed is surfaced here as a likely automated-injection attack.
// Honest framing: BadUSB cannot be detected with certainty (malicious firmware
// can perfectly mimic a legitimate device). This raises the bar, it is not a
// guarantee. In-kernel GUI app; reads xhci_get_enum_* directly like devinfo.c.
// ============================================================================
#include "window.h"
#include "icons.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../drivers/xhci.h"
#include "../drivers/usb_hid.h"
#include "../security/nova.h"   // #449 LLM Guard: Nova prompt-injection ruleset

// Palette (dark, matches the syslog/CDE-ish aesthetic; severity = traffic light)
#define CS_BG        0x14141Fu
#define CS_HEADER    0x20203Au
#define CS_PANEL     0x1B1B2Eu
#define CS_ROW       0x22223Au
#define CS_ROW_ALT   0x1D1D30u
#define CS_BORDER    0x3A3A5Au
#define CS_TEXT      0xE6E6EAu
#define CS_DIM       0x8890A0u
#define CS_GREEN     0x35C46Au
#define CS_AMBER     0xE0A020u
#define CS_RED       0xE0483Cu
#define CS_ACCENT    0x4A80D0u

#define CS_HEADER_H  30
#define CS_SUMMARY_H 26
#define CS_ROW_H     40
#define CS_MON_H     46
#define CS_FOOTER_H  30
#define CS_PAD       10

// Risk levels
#define RISK_SAFE       0
#define RISK_SUSPICIOUS 1
#define RISK_DANGEROUS  2

typedef struct {
    window_t *window;
    int scroll;
    int tab;        // 0 = USB threat scan, 1 = LLM Guard (Nova rules)
} cyber_app_t;

// Tab geometry (recomputed from window bounds; kept in sync between draw+event).
#define CS_TAB_W  80
#define CS_TAB_H  20

// ---- bitmap-font text (8x16), same convention as syslog.c (y = top) ----------
static void cs_text(const char *s, int x, int y, uint32_t color) {
    for (int i = 0; s[i]; i++) {
        const uint8_t *g = font_get_glyph(s[i]);
        for (int r = 0; r < 16; r++) {
            uint8_t bits = g[r];
            for (int c = 0; c < 8; c++)
                if (bits & (0x80 >> c)) fb_put_pixel(x + i * 8 + c, y + r, color);
        }
    }
}
static int cs_text_w(const char *s) { return (int)strlen(s) * 8; }

// ---- tiny formatting helpers (no libc printf in this file) -------------------
static char *cs_u32(char *p, uint32_t v) {
    char tmp[12]; int i = 0;
    if (v == 0) { *p++ = '0'; return p; }
    while (v > 0) { tmp[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) *p++ = tmp[--i];
    return p;
}
static char *cs_hex4(char *p, uint16_t v) {
    const char *H = "0123456789abcdef";
    for (int s = 12; s >= 0; s -= 4) *p++ = H[(v >> s) & 0xF];
    return p;
}

// ---- per-device descriptor analysis -----------------------------------------
typedef struct {
    int has_kbd, has_mouse, has_hid_other, has_msc, has_vendor, has_audio, has_net;
    int n_iface;
} iface_flags_t;

// Walk a device's raw CONFIGURATION descriptor blob and collect interface classes.
static void cs_parse_ifaces(const xhci_enum_dev_t *d, iface_flags_t *f) {
    memset(f, 0, sizeof(*f));
    int total = d->cfg_len;
    if (total > (int)sizeof(d->cfg)) total = (int)sizeof(d->cfg);
    int i = 0;
    while (i + 2 <= total) {
        int blen = d->cfg[i];
        int btype = d->cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9) {           // INTERFACE descriptor
            int icls  = d->cfg[i + 5];
            int iprot = d->cfg[i + 7];
            f->n_iface++;
            if (icls == 0x03) {                       // HID
                if (iprot == 1)      f->has_kbd = 1;
                else if (iprot == 2) f->has_mouse = 1;
                else                 f->has_hid_other = 1;
            } else if (icls == 0x08) f->has_msc = 1;   // Mass storage
            else if (icls == 0xFF)   f->has_vendor = 1; // Vendor-specific
            else if (icls == 0x01)   f->has_audio = 1;
            else if (icls == 0x02 || icls == 0x0A || icls == 0xE0) f->has_net = 1;
        }
        i += blen;
    }
}

// Assess one device. `kbd_total` = number of keyboards on the whole bus (for the
// "more than one keyboard" heuristic). Returns risk level; sets *reason.
static int cs_assess(const xhci_enum_dev_t *d, const iface_flags_t *f,
                     int kbd_total, const char **reason) {
    if (f->has_kbd && f->has_msc) {
        *reason = "Keyboard + storage on one device: classic BadUSB exfil shape";
        return RISK_DANGEROUS;
    }
    if (f->has_kbd && f->has_vendor) {
        *reason = "Keyboard + vendor interface: possible BadUSB payload channel";
        return RISK_DANGEROUS;
    }
    // A keyboard interface on a device whose DEVICE descriptor advertises a
    // non-HID, non-composite class is a disguise worth flagging.
    if (f->has_kbd && d->dev_class != 0x00 && d->dev_class != 0x03) {
        *reason = "Keyboard interface on a device disguised as another class";
        return RISK_SUSPICIOUS;
    }
    if (f->has_kbd && kbd_total > 1) {
        *reason = "More than one keyboard present: verify you plugged them all in";
        return RISK_SUSPICIOUS;
    }
    // Safe: give a plain description of what it is.
    if (d->is_hub)            *reason = "USB hub";
    else if (f->has_kbd)      *reason = "HID keyboard";
    else if (f->has_mouse)    *reason = "HID pointing device";
    else if (f->has_hid_other)*reason = "HID device";
    else if (f->has_msc)      *reason = "Mass storage";
    else if (f->has_audio)    *reason = "Audio device";
    else if (f->has_net)      *reason = "Network device";
    else                      *reason = "USB device";
    return RISK_SAFE;
}

static uint32_t risk_color(int risk) {
    return risk == RISK_DANGEROUS ? CS_RED : risk == RISK_SUSPICIOUS ? CS_AMBER : CS_GREEN;
}
static const char *risk_word(int risk) {
    return risk == RISK_DANGEROUS ? "DANGER" : risk == RISK_SUSPICIOUS ? "CHECK" : "SAFE";
}

static const char *speed_name(int s) {
    switch (s) { case 1: return "FS"; case 2: return "LS"; case 3: return "HS";
                 case 4: return "SS"; case 5: return "SS+"; default: return "?"; }
}

// Count keyboards across all enumerated devices (for multi-keyboard heuristic).
static int cs_count_keyboards(void) {
    int n = xhci_get_enum_count(), kb = 0;
    for (int i = 0; i < n; i++) {
        const xhci_enum_dev_t *d = xhci_get_enum_device(i);
        if (!d) continue;
        iface_flags_t f; cs_parse_ifaces(d, &f);
        if (f.has_kbd) kb++;
    }
    return kb;
}

// ---- app lifecycle -----------------------------------------------------------
static cyber_app_t *cyber_create(void) {
    cyber_app_t *a = (cyber_app_t *)kmalloc(sizeof(cyber_app_t));
    if (!a) return NULL;
    memset(a, 0, sizeof(*a));
    a->window = window_create("Cybersecurity", 120, 70, 560, 470);
    if (!a->window) { kfree(a); return NULL; }
    a->scroll = 0;
    return a;
}

static void cyber_destroy(cyber_app_t *a) {
    if (!a) return;
    if (a->window) window_destroy(a->window);
    kfree(a);
}

// ---- LLM Guard view (Nova prompt-injection ruleset) --------------------------
static void cyber_draw_llm(cyber_app_t *a, int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    int sy = wy + CS_HEADER_H + 1;
    fb_fill_rect(wx, sy, ww, CS_SUMMARY_H, CS_PANEL);
    {
        char buf[48]; char *p = buf;
        p = cs_u32(p, (uint32_t)nova_rule_count());
        const char *s = " Nova rules active";
        for (const char *q = s; *q; q++) *p++ = *q;
        *p = '\0';
        cs_text(buf, wx + CS_PAD, sy + 5, CS_DIM);
        const char *v = "PROMPT-INJECTION GUARD";
        cs_text(v, wx + ww - CS_PAD - cs_text_w(v), sy + 5, CS_GREEN);
    }
    fb_fill_rect(wx, sy + CS_SUMMARY_H, ww, 1, CS_BORDER);

    int list_y = sy + CS_SUMMARY_H + 1;
    int list_h = wh - CS_HEADER_H - CS_SUMMARY_H - CS_MON_H - CS_FOOTER_H - 3;
    int rows = list_h / CS_ROW_H;
    int nrules = nova_rule_count();
    if (a->scroll > nrules - rows) a->scroll = nrules - rows;
    if (a->scroll < 0) a->scroll = 0;
    int drawn = 0;
    for (int i = a->scroll; i < nrules && drawn < rows; i++) {
        const char *name, *cat; int sev;
        if (nova_rule_info(i, &name, &cat, &sev) != 0) continue;
        uint32_t rc = sev == NOVA_SEV_HIGH ? CS_RED
                    : sev == NOVA_SEV_MEDIUM ? CS_AMBER : CS_GREEN;
        int ry = list_y + drawn * CS_ROW_H;
        fb_fill_rect(wx, ry, ww, CS_ROW_H, (drawn & 1) ? CS_ROW_ALT : CS_ROW);
        fb_fill_rect(wx, ry, 4, CS_ROW_H, rc);
        cs_text(name, wx + 12, ry + 4, CS_TEXT);
        const char *sv = nova_sev_name(sev);
        cs_text(sv, wx + ww - CS_PAD - cs_text_w(sv), ry + 4, rc);
        cs_text(cat, wx + 12, ry + 21, CS_DIM);
        drawn++;
    }

    // Info panel (honest scope + MIT attribution, per the license)
    int my = wy + wh - CS_MON_H - CS_FOOTER_H;
    fb_fill_rect(wx, my, ww, 1, CS_BORDER);
    fb_fill_rect(wx, my + 1, ww, CS_MON_H - 1, CS_PANEL);
    cs_text("Keyword layer only - no semantic/LLM eval on-device",
            wx + CS_PAD, my + 6, CS_DIM);
    cs_text("Nova (c) 2025 T.Roccia @fr0gger_  MIT  github.com/Nova-Hunting",
            wx + CS_PAD, my + 24, CS_DIM);

    int fy = wy + wh - CS_FOOTER_H;
    fb_fill_rect(wx, fy, ww, 1, CS_BORDER);
    fb_fill_rect(wx, fy + 1, ww, CS_FOOTER_H - 1, CS_HEADER);
    cs_text("Screens prompts sent to Kimi / AI chat", wx + CS_PAD, fy + 8, CS_DIM);
    cs_text("Maytera Security", wx + ww - CS_PAD - cs_text_w("Maytera Security"),
            fy + 8, CS_DIM);
}

// ---- USB threat-scan view ----------------------------------------------------
static void cyber_draw_usb(cyber_app_t *a, int32_t wx, int32_t wy, int32_t ww, int32_t wh) {
    // Scan all devices, tally severity
    int n = xhci_get_enum_count();
    int kbd_total = cs_count_keyboards();
    int worst = RISK_SAFE, cnt_safe = 0, cnt_susp = 0, cnt_dang = 0;
    for (int i = 0; i < n; i++) {
        const xhci_enum_dev_t *d = xhci_get_enum_device(i);
        if (!d) continue;
        iface_flags_t f; cs_parse_ifaces(d, &f);
        const char *r; int risk = cs_assess(d, &f, kbd_total, &r);
        if (risk == RISK_DANGEROUS) cnt_dang++;
        else if (risk == RISK_SUSPICIOUS) cnt_susp++;
        else cnt_safe++;
        if (risk > worst) worst = risk;
    }

    // Summary banner
    int sy = wy + CS_HEADER_H + 1;
    fb_fill_rect(wx, sy, ww, CS_SUMMARY_H, CS_PANEL);
    {
        char buf[96]; char *p = buf;
        p = cs_u32(p, (uint32_t)n); const char *s1 = " devices  ";
        for (const char *q = s1; *q; q++) *p++ = *q;
        p = cs_u32(p, (uint32_t)cnt_dang); *p++='!'; *p++=' ';
        p = cs_u32(p, (uint32_t)cnt_susp); *p++='?'; *p++=' ';
        p = cs_u32(p, (uint32_t)cnt_safe); const char *s2 = " ok";
        for (const char *q = s2; *q; q++) *p++ = *q;
        *p = '\0';
        cs_text(buf, wx + CS_PAD, sy + 5, CS_DIM);
        const char *verdict = worst == RISK_DANGEROUS ? "THREAT DETECTED"
                            : worst == RISK_SUSPICIOUS ? "REVIEW ADVISED" : "NO THREATS FOUND";
        uint32_t vc = risk_color(worst);
        cs_text(verdict, wx + ww - CS_PAD - cs_text_w(verdict), sy + 5, vc);
    }
    fb_fill_rect(wx, sy + CS_SUMMARY_H, ww, 1, CS_BORDER);

    // Device list
    int list_y = sy + CS_SUMMARY_H + 1;
    int list_h = wh - CS_HEADER_H - CS_SUMMARY_H - CS_MON_H - CS_FOOTER_H - 3;
    int rows = list_h / CS_ROW_H;
    if (a->scroll > n - rows) a->scroll = n - rows;
    if (a->scroll < 0) a->scroll = 0;

    if (n == 0) {
        cs_text("No USB devices enumerated.", wx + CS_PAD, list_y + 10, CS_DIM);
    }
    int drawn = 0;
    for (int i = a->scroll; i < n && drawn < rows; i++) {
        const xhci_enum_dev_t *d = xhci_get_enum_device(i);
        if (!d) continue;
        iface_flags_t f; cs_parse_ifaces(d, &f);
        const char *r; int risk = cs_assess(d, &f, kbd_total, &r);
        uint32_t rc = risk_color(risk);
        int ry = list_y + drawn * CS_ROW_H;
        fb_fill_rect(wx, ry, ww, CS_ROW_H, (drawn & 1) ? CS_ROW_ALT : CS_ROW);
        // severity stripe
        fb_fill_rect(wx, ry, 4, CS_ROW_H, rc);
        // line 1: VID:PID  speed  [SAFE/CHECK/DANGER]
        char line[80]; char *p = line;
        p = cs_hex4(p, d->vendor_id); *p++ = ':'; p = cs_hex4(p, d->product_id);
        *p++ = ' '; *p++ = ' ';
        const char *sp = speed_name(d->speed);
        for (const char *q = sp; *q; q++) *p++ = *q;
        *p = '\0';
        cs_text(line, wx + 12, ry + 4, CS_TEXT);
        const char *rw = risk_word(risk);
        cs_text(rw, wx + ww - CS_PAD - cs_text_w(rw), ry + 4, rc);
        // line 2: reason (dim)
        cs_text(r, wx + 12, ry + 21, CS_DIM);
        drawn++;
    }

    // Keystroke-injection monitor
    int my = wy + wh - CS_MON_H - CS_FOOTER_H;
    fb_fill_rect(wx, my, ww, 1, CS_BORDER);
    fb_fill_rect(wx, my + 1, ww, CS_MON_H - 1, CS_PANEL);
    {
        uint32_t total = 0, min_gap = 0, peak = 0; int inj = 0;
        usb_hid_get_kbd_security(&total, &min_gap, &peak, &inj);
        uint32_t rc = inj ? CS_RED : CS_GREEN;
        cs_text("Keystroke Injection Monitor", wx + CS_PAD, my + 6, CS_TEXT);
        const char *st = inj ? "ALERT: automated typing detected" : "Normal (human typing rate)";
        cs_text(st, wx + CS_PAD, my + 24, rc);
        // stats on the right: keys / fastest gap (ticks * ~4ms) / burst
        char buf[80]; char *p = buf;
        p = cs_u32(p, total); const char *l1 = " keys, gap ";
        for (const char *q = l1; *q; q++) *p++ = *q;
        p = cs_u32(p, min_gap * 4u); *p++='m'; *p++='s'; const char *l2=", burst ";
        for (const char *q = l2; *q; q++) *p++ = *q;
        p = cs_u32(p, peak); *p='\0';
        cs_text(buf, wx + ww - CS_PAD - cs_text_w(buf), my + 24, CS_DIM);
    }

    // Footer
    int fy = wy + wh - CS_FOOTER_H;
    fb_fill_rect(wx, fy, ww, 1, CS_BORDER);
    fb_fill_rect(wx, fy + 1, ww, CS_FOOTER_H - 1, CS_HEADER);
    // Rescan button
    fb_fill_rect(wx + CS_PAD, fy + 5, 70, 20, CS_ACCENT);
    fb_draw_rect(wx + CS_PAD, fy + 5, 70, 20, CS_BORDER);
    cs_text("Rescan", wx + CS_PAD + 11, fy + 7, 0x0A0A14u);
    cs_text("Maytera Security", wx + ww - CS_PAD - cs_text_w("Maytera Security"),
            fy + 8, CS_DIM);
}

// ---- top-level draw: header + tab bar, then dispatch to the active view ------
static void cyber_draw(cyber_app_t *a) {
    if (!a || !a->window) return;
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(a->window, &wx, &wy, &ww, &wh);

    fb_fill_rect(wx, wy, ww, wh, CS_BG);

    // Header + tab chips (USB | LLM Guard)
    fb_fill_rect(wx, wy, ww, CS_HEADER_H, CS_HEADER);
    icon_draw(ICON_COG, wx + CS_PAD, wy + 2, CS_ACCENT);
    cs_text("Cybersecurity", wx + CS_PAD + 28, wy + 7, CS_TEXT);

    int ty = wy + 5;
    int t2x = wx + ww - CS_PAD - CS_TAB_W;
    int t1x = t2x - CS_TAB_W - 6;
    fb_fill_rect(t1x, ty, CS_TAB_W, CS_TAB_H, a->tab == 0 ? CS_ACCENT : CS_PANEL);
    fb_draw_rect(t1x, ty, CS_TAB_W, CS_TAB_H, CS_BORDER);
    cs_text("USB", t1x + (CS_TAB_W - cs_text_w("USB")) / 2, ty + 2,
            a->tab == 0 ? 0x0A0A14u : CS_DIM);
    fb_fill_rect(t2x, ty, CS_TAB_W, CS_TAB_H, a->tab == 1 ? CS_ACCENT : CS_PANEL);
    fb_draw_rect(t2x, ty, CS_TAB_W, CS_TAB_H, CS_BORDER);
    cs_text("LLM Guard", t2x + (CS_TAB_W - cs_text_w("LLM Guard")) / 2, ty + 2,
            a->tab == 1 ? 0x0A0A14u : CS_DIM);

    fb_fill_rect(wx, wy + CS_HEADER_H, ww, 1, CS_BORDER);

    if (a->tab == 1) cyber_draw_llm(a, wx, wy, ww, wh);
    else             cyber_draw_usb(a, wx, wy, ww, wh);
}

// ---- events ------------------------------------------------------------------
static void cyber_event(cyber_app_t *a, gui_event_t *e) {
    if (!a || !e) return;
    switch (e->type) {
        case EVENT_KEY_DOWN:
            if (e->keycode == 0x48) { if (a->scroll > 0) a->scroll--; }       // Up
            else if (e->keycode == 0x50) a->scroll++;                          // Down
            break;
        case EVENT_MOUSE_SCROLL:
            a->scroll += (int)e->keycode;   // scroll delta packed in keycode by compositor
            break;
        case EVENT_MOUSE_DOWN: {
            int32_t wx, wy, ww, wh;
            window_get_content_bounds(a->window, &wx, &wy, &ww, &wh);
            // Tab chips (header): switch view + reset scroll
            int ty = wy + 5;
            int t2x = wx + ww - CS_PAD - CS_TAB_W;
            int t1x = t2x - CS_TAB_W - 6;
            if (e->mouse_y >= ty && e->mouse_y < ty + CS_TAB_H) {
                if (e->mouse_x >= t1x && e->mouse_x < t1x + CS_TAB_W) { a->tab = 0; a->scroll = 0; break; }
                if (e->mouse_x >= t2x && e->mouse_x < t2x + CS_TAB_W) { a->tab = 1; a->scroll = 0; break; }
            }
            // Rescan button (USB view footer only)
            if (a->tab == 0) {
                int fy = wy + wh - CS_FOOTER_H;
                if (e->mouse_x >= wx + CS_PAD && e->mouse_x < wx + CS_PAD + 70 &&
                    e->mouse_y >= fy + 5 && e->mouse_y < fy + 25) {
                    a->scroll = 0;   // Rescan = redraw from top (scan is live each frame)
                }
            }
            break;
        }
        default: break;
    }
}

// ---- launch ------------------------------------------------------------------
void cybersecurity_launch(void) {
    cyber_app_t *a = cyber_create();
    if (!a) { kprintf("[Cyber] failed to create window\n"); return; }
    wm_register_app(a->window, a,
                    (app_event_handler_t)cyber_event,
                    (app_draw_handler_t)cyber_draw,
                    (app_destroy_handler_t)cyber_destroy);
    kprintf("[Cyber] Cybersecurity app launched\n");
}
