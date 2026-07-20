// colorpick.c - Maytera Studio professional colour picker + palette engine.
//
// Implements the module contract declared in studio.h: an integer HSV S x V
// square + vertical hue strip (both malloc'd ARGB caches rebuilt only when the
// hue changes, blitted with win_draw_image exactly like the layer thumbnails),
// FG/BG chips, an editable hex field, six HSV/RGB steppers, a modal "Change
// FG/BG Color" dialog with a before/after split swatch, and a persisted palette
// system (recent ring + stock "Maytera"/"Grays" + user .gpl on /CONFIG).
//
// Freestanding, integer-only maths (no libm, no floats). Every pixel buffer is
// malloc'd once and reused (no big statics, blame #444). This module never
// blocks or spins: caches are built on demand and reused, matching the
// async-fetch-then-cache mandate in CLAUDE.md.
//
// Drawing goes straight to the libc window primitives (win_draw_rect / _pixel /
// _image, gui_draw_rect_outline, win_draw_text_ttf, gui_text_ttf_centered),
// never the R()/T() statics in ui.c. Chrome colours are pulled from the shared
// style engine (gui_pal()) so the picker follows the active OS theme.
#include "studio.h"
#include "../../libc/gui.h"

// ===========================================================================
// Integer colour-space conversion (H 0..359, S/V 0..255)
// ===========================================================================
void cp_hsv2rgb(int h, int s, int v, int *r, int *g, int *b) {
    h = ((h % 360) + 360) % 360;
    s = clampi(s, 0, 255);
    v = clampi(v, 0, 255);
    if (s == 0) { *r = *g = *b = v; return; }
    int region = h / 60, rem = h % 60;
    int p = v * (255 - s) / 255;
    int q = v * (255 - (s * rem) / 60) / 255;
    int t = v * (255 - (s * (60 - rem)) / 60) / 255;
    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}
void cp_rgb2hsv(int r, int g, int b, int *ph, int *ps, int *pv) {
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = mx - mn, h = 0;
    if (d != 0) {
        if (mx == r)      h = (60 * (g - b) / d + 360) % 360;
        else if (mx == g) h =  60 * (b - r) / d + 120;
        else              h =  60 * (r - g) / d + 240;
        h = ((h % 360) + 360) % 360;
    }
    *ph = h;
    *ps = mx == 0 ? 0 : d * 255 / mx;
    *pv = mx;
}
static int cp_luma(uint32_t c) {
    return (px_r(c) * 77 + px_g(c) * 150 + px_b(c) * 29) >> 8;
}

// ===========================================================================
// Module state
// ===========================================================================
static int    g_win = -1;
static pick_t g_pick;                 // active picker colour (mirrors the target)
static int    g_edit_target = 0;      // 0 = FG, 1 = BG

// H-keyed square + hue-strip caches (dock + modal, malloc'd once, reused).
#define DSQ  120                       // dock SV square edge
#define DHUW 16                        // dock hue strip width
#define MSQ  256                       // modal SV square edge
#define MHUW 22                        // modal hue strip width
static uint32_t *g_sq_dock = 0;   static int g_sq_dock_hue  = -1;
static uint32_t *g_sq_modal = 0;  static int g_sq_modal_hue = -1;
static uint32_t *g_hue_dock = 0;  static int g_hue_dock_ok  = 0;
static uint32_t *g_hue_modal = 0; static int g_hue_modal_ok = 0;

// Hex editor (shared textfield machinery from libc).
static char        g_hex_buf[8];
static textfield_t g_hex_tf;
static int         g_hex_focus = 0;

// Stepper geometry recorded during draw, hit-tested on click (one shared list;
// the dock draw fills it, the modal draw overwrites it since it draws last).
enum { CPCH_H = 0, CPCH_S, CPCH_V, CPCH_R, CPCH_G, CPCH_B };
typedef struct { int x, y, w, h, ch; } step_t;
static step_t  g_step[8];
static int     g_nstep = 0;

// Drag state (dock and modal kept separate so routing never confuses them).
enum { DRAG_NONE = 0, DRAG_SV, DRAG_HUE, DRAG_STEP };
static int g_ddrag = DRAG_NONE, g_ddrag_ch = 0, g_ddrag_fx = 0, g_ddrag_fw = 1;
static int g_mdrag = DRAG_NONE, g_mdrag_ch = 0, g_mdrag_fx = 0, g_mdrag_fw = 1;
// SV/hue origins cached at draw time so a drag keeps tracking outside the rect.
static int g_dk_sqx = 0, g_dk_sqy = 0, g_dk_hux = 0, g_dk_huy = 0;
static int g_md_sqx = 0, g_md_sqy = 0, g_md_hux = 0, g_md_huy = 0;

// Double-click tracking for the FG/BG chips (opens the modal).
static unsigned long g_last_click_ms = 0;
static int           g_last_click_chip = -1;

// ---- Modal state ----------------------------------------------------------
static int      g_modal = 0;           // 0 closed, 1 editing FG/BG, 2 editing a value
static int      g_modal_bg = 0;        // target when g_modal==1
static pick_t   g_modal_pick;          // working colour
static uint32_t g_modal_old = 0;       // original (before/after + revert)
static int     *g_modal_dest = 0;      // SP_COLOR write-back (g_modal==2)
static void   (*g_modal_on_ok)(void) = 0;

// ===========================================================================
// Palette / swatch model
// ===========================================================================
typedef struct { uint8_t r, g, b, a; char name[24]; } swatch_t;
typedef struct {
    char     name[32];
    uint16_t count;
    uint8_t  columns;
    uint8_t  editable;
    swatch_t entries[256];
} palette_t;
typedef struct { uint32_t c[12]; int head, len; } recent_t;

#define CP_MAX_PAL 8
static palette_t g_pal[CP_MAX_PAL];
static int       g_npal = 0, g_pal_active = 0;
static recent_t  g_recent;
static int       g_user_pal = -1;      // index of the editable user palette
#define USER_PAL_PATH "/CONFIG/PALETTE.GPL"

// The stock "Maytera" 16 (kept in sync with ui.c SWATCH[] on purpose; this is
// data, not logic, so a local copy is fine and avoids un-static'ing ui.c).
static const uint32_t MAYTERA16[16] = {
    0x00000000, 0x00404040, 0x00808080, 0x00ffffff,
    0x00ff0000, 0x00ff8c00, 0x00ffff00, 0x0000c000,
    0x0000c0c0, 0x000060ff, 0x007000ff, 0x00ff00c0,
    0x008b4513, 0x00c68642, 0x00ffd7b0, 0x00224488
};

// ===========================================================================
// Small drawing helpers (libc primitives + theme palette)
// ===========================================================================
static void cr(int x, int y, int w, int h, uint32_t c)  { win_draw_rect(g_win, x, y, w, h, c); }
static void cout(int x, int y, int w, int h, uint32_t c) { gui_draw_rect_outline(g_win, x, y, w, h, c); }
static void ct(int x, int y, const char *s, int sz, uint32_t c) { win_draw_text_ttf(g_win, x, y, s, sz, c); }
static int  inr(int px, int py, int x, int y, int w, int h) { return px >= x && py >= y && px < x + w && py < y + h; }

static uint32_t C_LINE_(void)  { gui_palette_t *p = gui_pal(); return p ? p->border : 0x00141618; }
static uint32_t C_TEXT_(void)  { gui_palette_t *p = gui_pal(); return p ? p->ink : 0x00e8e8e8; }
static uint32_t C_DIM_(void)   { gui_palette_t *p = gui_pal(); return p ? p->ink_dim : 0x009aa0a8; }
static uint32_t C_ACC_(void)   { gui_palette_t *p = gui_pal(); return p ? p->accent_hover : 0x0055a0e0; }
static uint32_t C_PANEL_(void) { gui_palette_t *p = gui_pal(); return p ? p->surface_raised : 0x002b2e33; }
static uint32_t C_FIELD_(void) { gui_palette_t *p = gui_pal(); return p ? p->field_bg : 0x0025282c; }

// A small flat button (no hover feedback; clicks still register). Returns nothing.
static void cbtn(int x, int y, int w, int h, const char *label) {
    gui_palette_t *p = gui_pal();
    uint32_t base = p ? gui_lighten(p->surface_raised, 20) : 0x003a3e44;
    cr(x, y, w, h, base);
    cout(x, y, w, h, C_LINE_());
    if (label && label[0]) gui_text_ttf_centered(g_win, x, y, w, h, label, C_TEXT_(), 11);
}
// A sunken numeric/text field frame.
static void cfield(int x, int y, int w, int h) {
    cr(x, y, w, h, C_FIELD_());
    cout(x, y, w, h, C_LINE_());
}

// ===========================================================================
// Cache builders
// ===========================================================================
static void build_square(uint32_t *buf, int dim, int hue) {
    for (int y = 0; y < dim; y++) {
        int v = 255 - y * 255 / (dim - 1);
        for (int x = 0; x < dim; x++) {
            int s = x * 255 / (dim - 1), r, g, b;
            cp_hsv2rgb(hue, s, v, &r, &g, &b);
            buf[y * dim + x] = argb(255, r, g, b);
        }
    }
}
static uint32_t *square_dock(int hue) {
    if (!g_sq_dock) g_sq_dock = (uint32_t *)malloc((size_t)DSQ * DSQ * 4);
    if (g_sq_dock && g_sq_dock_hue != hue) { build_square(g_sq_dock, DSQ, hue); g_sq_dock_hue = hue; }
    return g_sq_dock;
}
static uint32_t *square_modal(int hue) {
    if (!g_sq_modal) g_sq_modal = (uint32_t *)malloc((size_t)MSQ * MSQ * 4);
    if (g_sq_modal && g_sq_modal_hue != hue) { build_square(g_sq_modal, MSQ, hue); g_sq_modal_hue = hue; }
    return g_sq_modal;
}
static void build_hue(uint32_t *buf, int w, int h) {
    for (int y = 0; y < h; y++) {
        int hue = y * 359 / (h - 1), r, g, b;
        cp_hsv2rgb(hue, 255, 255, &r, &g, &b);
        uint32_t c = argb(255, r, g, b);
        for (int x = 0; x < w; x++) buf[y * w + x] = c;
    }
}
static uint32_t *hue_dock(void) {
    if (!g_hue_dock) g_hue_dock = (uint32_t *)malloc((size_t)DHUW * DSQ * 4);
    if (g_hue_dock && !g_hue_dock_ok) { build_hue(g_hue_dock, DHUW, DSQ); g_hue_dock_ok = 1; }
    return g_hue_dock;
}
static uint32_t *hue_modal(void) {
    if (!g_hue_modal) g_hue_modal = (uint32_t *)malloc((size_t)MHUW * MSQ * 4);
    if (g_hue_modal && !g_hue_modal_ok) { build_hue(g_hue_modal, MHUW, MSQ); g_hue_modal_ok = 1; }
    return g_hue_modal;
}

// Markers (contrast-picked, drawn over the cache, never baked into it).
static void marker_sv(int sx, int sy, int dim, int s, int v, uint32_t rgb) {
    int px = sx + s * (dim - 1) / 255;
    int py = sy + (255 - v) * (dim - 1) / 255;
    uint32_t a = cp_luma(rgb) > 128 ? 0x00000000 : 0x00ffffff;
    uint32_t b = a ^ 0x00ffffff;
    cout(px - 4, py - 4, 9, 9, b);
    cout(px - 3, py - 3, 7, 7, a);
}
static void marker_hue(int hx, int hy, int w, int h, int hue) {
    int py = hy + hue * (h - 1) / 359;
    cr(hx - 2, py - 1, w + 4, 1, 0x00000000);
    cr(hx - 2, py,     w + 4, 1, 0x00ffffff);
    cr(hx - 2, py + 1, w + 4, 1, 0x00000000);
}

// ===========================================================================
// HSV/RGB channel plumbing + hex helpers
// ===========================================================================
static int step_val(pick_t *pk, int ch) {
    switch (ch) {
        case CPCH_H: return pk->H;
        case CPCH_S: return pk->S;
        case CPCH_V: return pk->V;
        case CPCH_R: return px_r(pk->rgb);
        case CPCH_G: return px_g(pk->rgb);
        default:     return px_b(pk->rgb);
    }
}
static void step_apply(pick_t *pk, int ch, int nv) {
    if (ch <= CPCH_V) {
        if (ch == CPCH_H) pk->H = ((nv % 360) + 360) % 360;
        else if (ch == CPCH_S) pk->S = clampi(nv, 0, 255);
        else pk->V = clampi(nv, 0, 255);
        int r, g, b; cp_hsv2rgb(pk->H, pk->S, pk->V, &r, &g, &b);
        pk->rgb = argb(0, r, g, b);
    } else {
        nv = clampi(nv, 0, 255);
        int r = px_r(pk->rgb), g = px_g(pk->rgb), b = px_b(pk->rgb);
        if (ch == CPCH_R) r = nv; else if (ch == CPCH_G) g = nv; else b = nv;
        pk->rgb = argb(0, r, g, b);
        int h, s, v; cp_rgb2hsv(r, g, b, &h, &s, &v);
        if (s == 0) h = pk->H;                 // achromatic guard: keep prior hue
        pk->H = h; pk->S = s; pk->V = v;
    }
}
static void pick_from_rgb(pick_t *pk, uint32_t rgb) {
    rgb &= 0xFFFFFF;
    int h, s, v; cp_rgb2hsv(px_r(rgb), px_g(rgb), px_b(rgb), &h, &s, &v);
    if (s == 0) h = pk->H;                       // keep hue on grays
    pk->H = h; pk->S = s; pk->V = v; pk->rgb = rgb;
}

static int hexdig(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int hex_parse(const char *s, uint32_t *out) {
    if (*s == '#') s++;
    uint32_t v = 0; int n = 0;
    for (; s[n]; n++) { int d = hexdig(s[n]); if (d < 0) return 0; v = (v << 4) | (unsigned)d; }
    if (n != 6) return 0;
    *out = v & 0xFFFFFF; return 1;
}
static void hex_format(uint32_t rgb, char *out) {
    static const char H[] = "0123456789ABCDEF";
    rgb &= 0xFFFFFF;
    for (int i = 5; i >= 0; i--) { out[i] = H[rgb & 15]; rgb >>= 4; }
    out[6] = 0;
}

// ===========================================================================
// Target + recent glue
// ===========================================================================
static uint32_t target_rgb(void) { return (g_edit_target ? g_tool.bg : g_tool.fg) & 0xFFFFFF; }
static void set_target(uint32_t rgb) {
    rgb &= 0xFFFFFF;
    if (g_edit_target) g_tool.bg = rgb; else g_tool.fg = rgb;
}
static void sync_from_target(void) { pick_from_rgb(&g_pick, target_rgb()); }

void cp_push_recent(uint32_t rgb) {
    rgb &= 0xFFFFFF;
    if (g_recent.len > 0) {                       // dedup consecutive
        int top = (g_recent.head - 1 + 12) % 12;
        if (g_recent.c[top] == rgb) return;
    }
    g_recent.c[g_recent.head] = rgb;
    g_recent.head = (g_recent.head + 1) % 12;
    if (g_recent.len < 12) g_recent.len++;
}
void cp_set_rgb(uint32_t rgb) {
    set_target(rgb);
    cp_push_recent(rgb);
    sync_from_target();
}

// ===========================================================================
// Palette persistence (.gpl, GIMP ASCII)
// ===========================================================================
static void pal_clear(palette_t *p, const char *name, int editable) {
    int i = 0; for (; name && name[i] && i < 31; i++) p->name[i] = name[i]; p->name[i] = 0;
    p->count = 0; p->columns = 8; p->editable = (uint8_t)editable;
}
static void pal_add(palette_t *p, uint32_t rgb, const char *name) {
    if (p->count >= 256) return;
    swatch_t *e = &p->entries[p->count++];
    e->r = (uint8_t)px_r(rgb); e->g = (uint8_t)px_g(rgb); e->b = (uint8_t)px_b(rgb); e->a = 255;
    int i = 0; if (name) for (; name[i] && i < 23; i++) e->name[i] = name[i];
    e->name[i] = 0;
}
static uint32_t sw_rgb(const swatch_t *e) { return argb(0, e->r, e->g, e->b); }

// Minimal integer-to-ASCII into a small buffer; returns chars written.
static int put_uint(char *o, unsigned v) {
    char t[12]; int n = 0;
    if (v == 0) { o[0] = '0'; return 1; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) o[i] = t[n - 1 - i];
    return n;
}
// Append helper for building the .gpl text.
static int gpl_row(char *o, int r, int g, int b) {
    int n = 0;
    // 3-wide right-aligned columns keep the file readable (GIMP style).
    for (int col = 0, val; col < 3; col++) {
        val = col == 0 ? r : (col == 1 ? g : b);
        int digs = val >= 100 ? 3 : (val >= 10 ? 2 : 1);
        for (int s = digs; s < 3; s++) o[n++] = ' ';
        n += put_uint(o + n, (unsigned)val);
        o[n++] = col < 2 ? ' ' : '\t';
    }
    o[n++] = 'U'; o[n++] = 'n'; o[n++] = 't'; o[n++] = 'i'; o[n++] = 't';
    o[n++] = 'l'; o[n++] = 'e'; o[n++] = 'd'; o[n++] = '\n';
    return n;
}
static void pal_save_user(void) {
    if (g_user_pal < 0) return;
    palette_t *p = &g_pal[g_user_pal];
    // Build the whole file in one malloc'd buffer, then a single write.
    int cap = 128 + p->count * 24;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) return;
    int n = 0;
    const char *hdr = "GIMP Palette\nName: ";
    for (int i = 0; hdr[i]; i++) buf[n++] = hdr[i];
    for (int i = 0; p->name[i] && i < 31; i++) buf[n++] = p->name[i];
    buf[n++] = '\n';
    const char *cl = "Columns: ";
    for (int i = 0; cl[i]; i++) buf[n++] = cl[i];
    n += put_uint(buf + n, p->columns ? p->columns : 8);
    buf[n++] = '\n'; buf[n++] = '#'; buf[n++] = '\n';
    for (int i = 0; i < p->count; i++)
        n += gpl_row(buf + n, p->entries[i].r, p->entries[i].g, p->entries[i].b);
    int fd = sys_open(USER_PAL_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) { sys_write(fd, buf, (unsigned long)n); sys_close(fd); }
    free(buf);
}
static void pal_load_user(void) {
    int fd = sys_open(USER_PAL_PATH, O_RDONLY);
    if (fd < 0) return;
    long cap = 8192;
    char *buf = (char *)malloc((size_t)cap);
    if (!buf) { sys_close(fd); return; }
    long len = 0;
    for (;;) {
        if (len == cap) { long nc = cap * 2; char *nb = (char *)realloc(buf, (size_t)nc); if (!nb) break; buf = nb; cap = nc; }
        long got = sys_read(fd, buf + len, (unsigned long)(cap - len));
        if (got <= 0) break;
        len += got;
    }
    sys_close(fd);
    if (len <= 0) { free(buf); return; }
    if (g_npal >= CP_MAX_PAL) { free(buf); return; }
    palette_t *p = &g_pal[g_npal];
    pal_clear(p, "User", 1);
    // Line-by-line parse. Header lines start with a letter or '#'; data rows
    // begin with a digit or space then three decimal channels.
    long i = 0;
    while (i < len) {
        long ls = i;
        while (i < len && buf[i] != '\n') i++;
        long le = i; if (i < len) i++;             // consume newline
        // Trim leading spaces.
        long a = ls; while (a < le && (buf[a] == ' ' || buf[a] == '\t')) a++;
        if (a >= le) continue;
        char c0 = buf[a];
        if (c0 == '#') continue;
        if (c0 == 'N' && a + 5 < le && buf[a+1]=='a' && buf[a+2]=='m' && buf[a+3]=='e' && buf[a+4]==':') {
            long v = a + 5; while (v < le && buf[v] == ' ') v++;
            int k = 0; while (v < le && k < 31) p->name[k++] = buf[v++]; p->name[k] = 0;
            continue;
        }
        if (c0 == 'C' && a + 8 < le && buf[a+1]=='o' && buf[a+2]=='l') {
            long v = a; while (v < le && buf[v] != ':') v++; if (v < le) v++;
            while (v < le && buf[v] == ' ') v++;
            int col = 0; while (v < le && buf[v] >= '0' && buf[v] <= '9') col = col * 10 + (buf[v++] - '0');
            if (col > 0 && col < 64) p->columns = (uint8_t)col;
            continue;
        }
        if (c0 == 'G') continue;                    // "GIMP Palette" banner
        if (c0 >= '0' && c0 <= '9') {
            int ch[3] = {0,0,0}, ci = 0; long v = a;
            while (v < le && ci < 3) {
                while (v < le && (buf[v] == ' ' || buf[v] == '\t')) v++;
                if (v >= le || buf[v] < '0' || buf[v] > '9') break;
                int val = 0; while (v < le && buf[v] >= '0' && buf[v] <= '9') val = val * 10 + (buf[v++] - '0');
                ch[ci++] = val;
            }
            if (ci == 3) pal_add(p, argb(0, clampi(ch[0],0,255), clampi(ch[1],0,255), clampi(ch[2],0,255)), 0);
        }
    }
    free(buf);
    if (p->count > 0 || p->name[0]) { g_user_pal = g_npal; g_npal++; }
}

// ===========================================================================
// Lifecycle
// ===========================================================================
void cp_init(int win) {
    g_win = win;
    g_recent.head = 0; g_recent.len = 0;
    g_npal = 0; g_pal_active = 0; g_user_pal = -1;
    // Stock palettes (editable = 0).
    pal_clear(&g_pal[g_npal], "Maytera", 0);
    for (int i = 0; i < 16; i++) pal_add(&g_pal[g_npal], MAYTERA16[i], 0);
    g_npal++;
    pal_clear(&g_pal[g_npal], "Grays", 0);
    for (int i = 0; i < 16; i++) { int v = i * 255 / 15; pal_add(&g_pal[g_npal], argb(0, v, v, v), 0); }
    g_npal++;
    // User palette from /CONFIG (creates an empty editable one if none on disk).
    pal_load_user();
    if (g_user_pal < 0 && g_npal < CP_MAX_PAL) {
        pal_clear(&g_pal[g_npal], "User", 1);
        g_user_pal = g_npal; g_npal++;
    }
    hex_format(0, g_hex_buf);
    tf_init(&g_hex_tf, g_hex_buf, sizeof(g_hex_buf));
    g_edit_target = 0;
    sync_from_target();
}

// ===========================================================================
// Stepper draw + hit (shared by dock and modal)
// ===========================================================================
static const char *CH_LABEL[6] = { "H", "S", "V", "R", "G", "B" };
static void draw_stepper(int x, int y, int w, int ch, pick_t *pk) {
    ct(x, y + 1, CH_LABEL[ch], 11, C_DIM_());
    int fx = x + 12, fw = w - 12;
    cfield(fx, y, fw, 15);
    char vb[8]; int v = step_val(pk, ch), n = 0;
    n = put_uint(vb, (unsigned)v); vb[n] = 0;
    ct(fx + 4, y + 1, vb, 11, C_TEXT_());
    if (g_nstep < 8) { step_t *s = &g_step[g_nstep++]; s->x = fx; s->y = y; s->w = fw; s->h = 15; s->ch = ch; }
}
// Hit a recorded stepper. Left 22% = -1, right 22% = +1, middle = scrub-drag.
// Returns 1 if consumed; on scrub start fills *fx,*fw for the drag mapper.
static int hit_stepper(int mx, int my, pick_t *pk, int *dch, int *dfx, int *dfw) {
    for (int i = 0; i < g_nstep; i++) {
        step_t *s = &g_step[i];
        if (!inr(mx, my, s->x, s->y, s->w, s->h)) continue;
        int vmax = s->ch == CPCH_H ? 359 : 255;
        int cur = step_val(pk, s->ch);
        int edge = s->w / 5; if (edge < 6) edge = 6;
        if (mx < s->x + edge)        { step_apply(pk, s->ch, cur - 1); }
        else if (mx >= s->x + s->w - edge) { step_apply(pk, s->ch, cur + 1); }
        else { int rel = mx - s->x; step_apply(pk, s->ch, rel * vmax / (s->w > 1 ? s->w - 1 : 1));
               *dch = s->ch; *dfx = s->x; *dfw = s->w; return 2; }   // 2 = scrub started
        (void)vmax; (void)my;
        return 1;
    }
    return 0;
}

// ===========================================================================
// Dock mini-picker
// ===========================================================================
// Layout constants, all relative to dock origin dx and section anchor y0.
#define DK_SQX(dx)  ((dx) + 12)
#define DK_SQY(y0)  ((y0) + 6)
#define DK_HUX(dx)  ((dx) + 140)
#define DK_HUY(y0)  ((y0) + 6)
#define DK_RX(dx)   ((dx) + 162)          // right column
static void dk_chip_rects(int dx, int y0, int *fx, int *fy, int *bx, int *by) {
    *fx = DK_RX(dx);      *fy = y0 + 6;    // FG chip 44x24
    *bx = DK_RX(dx) + 24; *by = y0 + 18;   // BG chip 26x18 (overlaps bottom-right)
}

int cp_dock_height(void) { return 200; }

void cp_draw_dock(int dx, int y0) {
    if (g_win < 0) return;
    if (g_ddrag == DRAG_NONE && !g_hex_focus) sync_from_target();
    if (!g_hex_focus) hex_format(g_pick.rgb, g_hex_buf);   // keep field fresh

    int sqx = DK_SQX(dx), sqy = DK_SQY(y0);
    g_dk_sqx = sqx; g_dk_sqy = sqy;
    uint32_t *sq = square_dock(g_pick.H);
    if (sq) win_draw_image(g_win, sqx, sqy, DSQ, DSQ, sq);
    cout(sqx - 1, sqy - 1, DSQ + 2, DSQ + 2, C_LINE_());
    marker_sv(sqx, sqy, DSQ, g_pick.S, g_pick.V, g_pick.rgb);

    int hux = DK_HUX(dx), huy = DK_HUY(y0);
    g_dk_hux = hux; g_dk_huy = huy;
    uint32_t *hs = hue_dock();
    if (hs) win_draw_image(g_win, hux, huy, DHUW, DSQ, hs);
    cout(hux - 1, huy - 1, DHUW + 2, DSQ + 2, C_LINE_());
    marker_hue(hux, huy, DHUW, DSQ, g_pick.H);

    // FG/BG chips (active target gets the accent outline).
    int fx, fy, bx, by; dk_chip_rects(dx, y0, &fx, &fy, &bx, &by);
    cr(fx, fy, 44, 24, g_tool.fg & 0xFFFFFF); cout(fx, fy, 44, 24, C_LINE_());
    cr(bx, by, 26, 18, g_tool.bg & 0xFFFFFF); cout(bx, by, 26, 18, C_LINE_());
    if (g_edit_target == 0) cout(fx - 1, fy - 1, 46, 26, C_ACC_());
    else                    cout(bx - 1, by - 1, 28, 20, C_ACC_());

    int rx = DK_RX(dx);
    cbtn(rx,      y0 + 40, 20, 14, "..");
    cbtn(rx + 22, y0 + 40, 20, 14, "sw");
    cbtn(rx + 44, y0 + 40, 20, 14, "rs");

    // Hex field (# + editable six hex digits).
    int hxx = rx, hxy = y0 + 58;
    ct(hxx, hxy + 1, "#", 11, C_DIM_());
    cfield(hxx + 9, hxy, 59, 16);
    ct(hxx + 13, hxy + 1, g_hex_buf, 11, C_TEXT_());
    if (g_hex_focus) {
        int cw = gui_string_width(g_hex_buf);
        cr(hxx + 13 + cw, hxy + 1, 1, 13, C_ACC_());
        cout(hxx + 8, hxy - 1, 61, 18, C_ACC_());
    }

    // Steppers: row1 HSV, row2 RGB.
    g_nstep = 0;
    int r1 = y0 + 132, r2 = y0 + 152;
    draw_stepper(dx + 8,   r1, 68, CPCH_H, &g_pick);
    draw_stepper(dx + 80,  r1, 68, CPCH_S, &g_pick);
    draw_stepper(dx + 152, r1, 68, CPCH_V, &g_pick);
    draw_stepper(dx + 8,   r2, 68, CPCH_R, &g_pick);
    draw_stepper(dx + 80,  r2, 68, CPCH_G, &g_pick);
    draw_stepper(dx + 152, r2, 68, CPCH_B, &g_pick);

    cbtn(dx + 12, y0 + 174, 64, 16, "eyedrop");
    cbtn(dx + 80, y0 + 174, 74, 16, "+ swatch");
}

static void dock_commit_live(void) {           // during a dock drag: write target
    set_target(g_pick.rgb);
    hex_format(g_pick.rgb, g_hex_buf);
    ui_full_redraw();
}

int cp_click_dock(int dx, int y0, int mx, int my, int shift, int alt) {
    (void)shift; (void)alt;
    int sqx = DK_SQX(dx), sqy = DK_SQY(y0);
    int hux = DK_HUX(dx), huy = DK_HUY(y0);

    if (inr(mx, my, sqx, sqy, DSQ, DSQ)) {
        g_hex_focus = 0;
        g_pick.S = clampi((mx - sqx) * 255 / (DSQ - 1), 0, 255);
        g_pick.V = clampi(255 - (my - sqy) * 255 / (DSQ - 1), 0, 255);
        int r, g, b; cp_hsv2rgb(g_pick.H, g_pick.S, g_pick.V, &r, &g, &b);
        g_pick.rgb = argb(0, r, g, b);
        g_ddrag = DRAG_SV; dock_commit_live(); return 1;
    }
    if (inr(mx, my, hux, huy, DHUW, DSQ)) {
        g_hex_focus = 0;
        g_pick.H = clampi((my - huy) * 359 / (DSQ - 1), 0, 359);
        int r, g, b; cp_hsv2rgb(g_pick.H, g_pick.S, g_pick.V, &r, &g, &b);
        g_pick.rgb = argb(0, r, g, b);
        g_ddrag = DRAG_HUE; dock_commit_live(); return 1;
    }

    int fx, fy, bx, by; dk_chip_rects(dx, y0, &fx, &fy, &bx, &by);
    // BG chip is drawn on top, so test it first.
    if (inr(mx, my, bx, by, 26, 18) || inr(mx, my, fx, fy, 44, 24)) {
        int chip = inr(mx, my, bx, by, 26, 18) ? 1 : 0;
        unsigned long now = uptime_ms();
        if (chip == g_last_click_chip && now - g_last_click_ms < 400) {
            cp_open_modal(chip); g_last_click_chip = -1;
        } else {
            g_edit_target = chip; g_hex_focus = 0; sync_from_target();
            g_last_click_chip = chip; g_last_click_ms = now;
            hex_format(target_rgb(), g_hex_buf);
            ui_full_redraw();
        }
        return 1;
    }

    int rx = DK_RX(dx);
    if (inr(mx, my, rx, y0 + 40, 20, 14)) { cp_open_modal(g_edit_target); return 1; }   // "..."
    if (inr(mx, my, rx + 22, y0 + 40, 20, 14)) {                                          // swap
        uint32_t t = g_tool.fg; g_tool.fg = g_tool.bg; g_tool.bg = t;
        sync_from_target(); hex_format(target_rgb(), g_hex_buf);
        cp_push_recent(target_rgb()); ui_full_redraw(); return 1;
    }
    if (inr(mx, my, rx + 44, y0 + 40, 20, 14)) {                                          // reset B/W
        g_tool.fg = 0x00000000; g_tool.bg = 0x00ffffff;
        sync_from_target(); hex_format(target_rgb(), g_hex_buf); ui_full_redraw(); return 1;
    }

    int hxy = y0 + 58;
    if (inr(mx, my, rx, hxy, 68, 16)) {
        g_hex_focus = 1; hex_format(target_rgb(), g_hex_buf);
        tf_init(&g_hex_tf, g_hex_buf, sizeof(g_hex_buf)); ui_full_redraw(); return 1;
    }

    // Steppers.
    int dch, dfx, dfw;
    int hs = hit_stepper(mx, my, &g_pick, &dch, &dfx, &dfw);
    if (hs) {
        if (hs == 2) { g_ddrag = DRAG_STEP; g_ddrag_ch = dch; g_ddrag_fx = dfx; g_ddrag_fw = dfw; }
        dock_commit_live(); return 1;
    }

    if (inr(mx, my, dx + 12, y0 + 174, 64, 16)) { g_tool.id = TL_PICK; ui_full_redraw(); return 1; }
    if (inr(mx, my, dx + 80, y0 + 174, 74, 16)) {
        if (g_user_pal >= 0) { pal_add(&g_pal[g_user_pal], target_rgb(), 0); g_pal_active = g_user_pal; pal_save_user(); ui_full_redraw(); }
        return 1;
    }
    return 1;   // consume any other click in the section body
}

int cp_drag_dock(int mx, int my) {
    if (g_ddrag == DRAG_NONE) return 0;
    if (g_ddrag == DRAG_SV) {
        g_pick.S = clampi((mx - g_dk_sqx) * 255 / (DSQ - 1), 0, 255);
        g_pick.V = clampi(255 - (my - g_dk_sqy) * 255 / (DSQ - 1), 0, 255);
        int r, g, b; cp_hsv2rgb(g_pick.H, g_pick.S, g_pick.V, &r, &g, &b);
        g_pick.rgb = argb(0, r, g, b);
    } else if (g_ddrag == DRAG_HUE) {
        g_pick.H = clampi((my - g_dk_huy) * 359 / (DSQ - 1), 0, 359);
        int r, g, b; cp_hsv2rgb(g_pick.H, g_pick.S, g_pick.V, &r, &g, &b);
        g_pick.rgb = argb(0, r, g, b);
    } else if (g_ddrag == DRAG_STEP) {
        int vmax = g_ddrag_ch == CPCH_H ? 359 : 255;
        int rel = mx - g_ddrag_fx;
        step_apply(&g_pick, g_ddrag_ch, rel * vmax / (g_ddrag_fw > 1 ? g_ddrag_fw - 1 : 1));
    }
    dock_commit_live();
    return 1;
}
void cp_dock_release(void) {
    if (g_ddrag != DRAG_NONE) { g_ddrag = DRAG_NONE; cp_set_rgb(g_pick.rgb); ui_full_redraw(); }
}

int cp_key_dock(const void *ev) {
    if (!g_hex_focus) return 0;
    const gui_event_t *e = (const gui_event_t *)ev;
    char ch = e->key_char;
    if (ch == 27) { g_hex_focus = 0; hex_format(target_rgb(), g_hex_buf); ui_full_redraw(); return 1; }
    if (ch == '\n' || ch == '\r' || e->keycode == 0x1C) {
        uint32_t rgb;
        if (hex_parse(g_hex_buf, &rgb)) cp_set_rgb(rgb);
        else hex_format(target_rgb(), g_hex_buf);
        g_hex_focus = 0; ui_full_redraw(); return 1;
    }
    tf_handle_key(&g_hex_tf, e);
    ui_full_redraw();
    return 1;
}

// ===========================================================================
// Palettes / swatches dock section
// ===========================================================================
#define PL_CELL 18
#define PL_GAP  2
#define PL_COLS 11                       // fits (240-24)/(18+2) columns in the dock
static int pl_grid_rows(void) {
    if (g_pal_active < 0 || g_pal_active >= g_npal) return 1;
    int n = g_pal[g_pal_active].count;
    int rows = (n + PL_COLS - 1) / PL_COLS; if (rows < 1) rows = 1; if (rows > 6) rows = 6;
    return rows;
}
int cp_pal_height(void) {
    // header row + palette grid + recent row + button row.
    return 22 + pl_grid_rows() * (PL_CELL + PL_GAP) + 8 + 22 + 20;
}

void cp_draw_pal(int dx, int y0) {
    if (g_win < 0) return;
    // Header: palette name + AI button.
    palette_t *p = (g_pal_active >= 0 && g_pal_active < g_npal) ? &g_pal[g_pal_active] : 0;
    ct(dx + 8, y0 + 2, "Palette:", 11, C_DIM_());
    ct(dx + 60, y0 + 2, p ? p->name : "-", 11, C_TEXT_());
    cbtn(dx + 150, y0, 20, 16, "<");         // prev palette
    cbtn(dx + 172, y0, 20, 16, ">");         // next palette
    cbtn(dx + 196, y0, 32, 16, "AI");        // AI palette
    int y = y0 + 22;

    // Swatch grid.
    if (p) {
        for (int i = 0; i < p->count; i++) {
            int c = i % PL_COLS, r = i / PL_COLS;
            if (r >= 6) break;
            int cx = dx + 12 + c * (PL_CELL + PL_GAP);
            int cy = y + r * (PL_CELL + PL_GAP);
            uint32_t rgb = sw_rgb(&p->entries[i]);
            cr(cx, cy, PL_CELL, PL_CELL, rgb);
            cout(cx, cy, PL_CELL, PL_CELL, C_LINE_());
            if (rgb == (g_tool.fg & 0xFFFFFF)) cout(cx - 1, cy - 1, PL_CELL + 2, PL_CELL + 2, C_ACC_());
        }
    }
    y += pl_grid_rows() * (PL_CELL + PL_GAP) + 6;

    // Recent-colours ring.
    ct(dx + 8, y + 1, "Recent", 11, C_DIM_());
    int ry = y + 14;
    for (int i = 0; i < 12; i++) {
        int cx = dx + 12 + i * (PL_CELL + PL_GAP);
        cr(cx, ry, PL_CELL, PL_CELL, C_PANEL_());
        cout(cx, ry, PL_CELL, PL_CELL, C_LINE_());
        if (i < g_recent.len) {
            int idx = (g_recent.head - 1 - i + 24) % 12;
            uint32_t rgb = g_recent.c[idx];
            cr(cx + 1, ry + 1, PL_CELL - 2, PL_CELL - 2, rgb);
        }
    }
    y = ry + PL_CELL + 6;
    cbtn(dx + 12, y, 60, 16, "+ FG");        // add current FG to user palette
}

int cp_click_pal(int dx, int y0, int mx, int my, int shift, int alt) {
    if (inr(mx, my, dx + 150, y0, 20, 16)) {                 // prev palette
        if (g_npal > 0) { g_pal_active = (g_pal_active - 1 + g_npal) % g_npal; ui_full_redraw(); }
        return 1;
    }
    if (inr(mx, my, dx + 172, y0, 20, 16)) {                 // next palette
        if (g_npal > 0) { g_pal_active = (g_pal_active + 1) % g_npal; ui_full_redraw(); }
        return 1;
    }
    if (inr(mx, my, dx + 196, y0, 32, 16)) return 2;         // AI: caller opens the flow

    int y = y0 + 22;
    palette_t *p = (g_pal_active >= 0 && g_pal_active < g_npal) ? &g_pal[g_pal_active] : 0;
    if (p) {
        for (int i = 0; i < p->count; i++) {
            int c = i % PL_COLS, r = i / PL_COLS;
            if (r >= 6) break;
            int cx = dx + 12 + c * (PL_CELL + PL_GAP);
            int cy = y + r * (PL_CELL + PL_GAP);
            if (!inr(mx, my, cx, cy, PL_CELL, PL_CELL)) continue;
            uint32_t rgb = sw_rgb(&p->entries[i]);
            if (alt) {                                       // Alt-click deletes
                if (p->editable) {
                    for (int k = i; k + 1 < p->count; k++) p->entries[k] = p->entries[k + 1];
                    p->count--; if (g_user_pal == g_pal_active) pal_save_user();
                } else {
                    ui_status("Stock palette is read-only");
                }
                ui_full_redraw(); return 1;
            }
            g_edit_target = shift ? 1 : 0;                   // Shift-click sets BG
            cp_set_rgb(rgb);
            hex_format(target_rgb(), g_hex_buf);
            ui_full_redraw(); return 1;
        }
    }
    y += pl_grid_rows() * (PL_CELL + PL_GAP) + 6;

    // Recent ring.
    int ry = y + 14;
    for (int i = 0; i < 12 && i < g_recent.len; i++) {
        int cx = dx + 12 + i * (PL_CELL + PL_GAP);
        if (inr(mx, my, cx, ry, PL_CELL, PL_CELL)) {
            int idx = (g_recent.head - 1 - i + 24) % 12;
            g_edit_target = shift ? 1 : 0;
            cp_set_rgb(g_recent.c[idx]);
            hex_format(target_rgb(), g_hex_buf);
            ui_full_redraw(); return 1;
        }
    }
    y = ry + PL_CELL + 6;
    if (inr(mx, my, dx + 12, y, 60, 16)) {                   // + FG
        if (g_user_pal >= 0) { pal_add(&g_pal[g_user_pal], target_rgb(), 0); g_pal_active = g_user_pal; pal_save_user(); ui_full_redraw(); }
        return 1;
    }
    return 1;
}

void cp_load_ai_palette(const char *name, const uint32_t *cols, int n) {
    if (n <= 0) return;
    int slot = g_user_pal;
    // Prefer replacing the user palette if it is empty; else append a new one.
    if (slot < 0 || g_pal[slot].count > 0) {
        if (g_npal >= CP_MAX_PAL) slot = g_user_pal >= 0 ? g_user_pal : 0;
        else { slot = g_npal; g_npal++; }
    }
    pal_clear(&g_pal[slot], name && name[0] ? name : "AI", 1);
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) pal_add(&g_pal[slot], cols[i] & 0xFFFFFF, 0);
    g_pal[slot].columns = (uint8_t)(n < 8 ? (n < 1 ? 1 : n) : 8);
    g_user_pal = slot; g_pal_active = slot;
    pal_save_user();
    cp_set_rgb(cols[0] & 0xFFFFFF);
    hex_format(target_rgb(), g_hex_buf);
}

// ===========================================================================
// Modal "Change FG/BG Color" dialog (520x360, self-centering)
// ===========================================================================
#define MODW 520
#define MODH 360
static void modal_origin(int *bx, int *by) {
    int vw = 1180, vh = 740;
    win_get_size(g_win, &vw, &vh);
    if (vw < MODW) vw = MODW;
    if (vh < MODH) vh = MODH;
    *bx = (vw - MODW) / 2; *by = (vh - MODH) / 2;
}

void cp_open_modal(int editing_bg) {
    g_modal = 1; g_modal_bg = editing_bg; g_modal_dest = 0; g_modal_on_ok = 0;
    uint32_t cur = (editing_bg ? g_tool.bg : g_tool.fg) & 0xFFFFFF;
    g_modal_old = cur;
    g_modal_pick.H = g_pick.H;              // seed hue so grays keep a sensible hue
    pick_from_rgb(&g_modal_pick, cur);
    g_mdrag = DRAG_NONE;
    hex_format(cur, g_hex_buf);
    tf_init(&g_hex_tf, g_hex_buf, sizeof(g_hex_buf));
    ui_full_redraw();
}
void cp_open_modal_for_value(uint32_t initial, int *dest, void (*on_ok)(void)) {
    g_modal = 2; g_modal_dest = dest; g_modal_on_ok = on_ok; g_modal_bg = 0;
    initial &= 0xFFFFFF; g_modal_old = initial;
    g_modal_pick.H = g_pick.H;
    pick_from_rgb(&g_modal_pick, initial);
    g_mdrag = DRAG_NONE;
    hex_format(initial, g_hex_buf);
    tf_init(&g_hex_tf, g_hex_buf, sizeof(g_hex_buf));
    ui_full_redraw();
}
int cp_modal_open(void) { return g_modal != 0; }

static void modal_close(void) { g_modal = 0; g_mdrag = DRAG_NONE; }
static void modal_ok(void) {
    uint32_t rgb = g_modal_pick.rgb & 0xFFFFFF;
    if (g_modal == 2) {
        if (g_modal_dest) *g_modal_dest = (int)rgb;
        cp_push_recent(rgb);
        void (*cb)(void) = g_modal_on_ok;
        modal_close();
        if (cb) cb();
    } else {
        g_edit_target = g_modal_bg;
        cp_set_rgb(rgb);
        hex_format(target_rgb(), g_hex_buf);
        modal_close();
    }
    ui_full_redraw();
}
static void modal_cancel(void) { modal_close(); ui_full_redraw(); }

void cp_draw_modal(void) {
    if (!g_modal || g_win < 0) return;
    int bx, by; modal_origin(&bx, &by);
    // Frame.
    cr(bx - 2, by - 2, MODW + 4, MODH + 4, C_LINE_());
    cr(bx, by, MODW, MODH, C_PANEL_());
    cout(bx, by, MODW, MODH, C_ACC_());
    ct(bx + 16, by + 12, g_modal == 2 ? "Change Color" : (g_modal_bg ? "Change Background Color" : "Change Foreground Color"), 13, C_TEXT_());
    cbtn(bx + MODW - 28, by + 8, 20, 20, "x");

    // SV square + hue strip.
    int sqx = bx + 16, sqy = by + 40;
    g_md_sqx = sqx; g_md_sqy = sqy;
    uint32_t *sq = square_modal(g_modal_pick.H);
    if (sq) win_draw_image(g_win, sqx, sqy, MSQ, MSQ, sq);
    cout(sqx - 1, sqy - 1, MSQ + 2, MSQ + 2, C_LINE_());
    marker_sv(sqx, sqy, MSQ, g_modal_pick.S, g_modal_pick.V, g_modal_pick.rgb);

    int hux = bx + 284, huy = by + 40;
    g_md_hux = hux; g_md_huy = huy;
    uint32_t *hs = hue_modal();
    if (hs) win_draw_image(g_win, hux, huy, MHUW, MSQ, hs);
    cout(hux - 1, huy - 1, MHUW + 2, MSQ + 2, C_LINE_());
    marker_hue(hux, huy, MHUW, MSQ, g_modal_pick.H);

    // Numeric steppers: HSV column + RGB column.
    g_nstep = 0;
    int nx1 = bx + 330, nx2 = bx + 422, ny = by + 44;
    draw_stepper(nx1, ny,      84, CPCH_H, &g_modal_pick);
    draw_stepper(nx1, ny + 22, 84, CPCH_S, &g_modal_pick);
    draw_stepper(nx1, ny + 44, 84, CPCH_V, &g_modal_pick);
    draw_stepper(nx2, ny,      82, CPCH_R, &g_modal_pick);
    draw_stepper(nx2, ny + 22, 82, CPCH_G, &g_modal_pick);
    draw_stepper(nx2, ny + 44, 82, CPCH_B, &g_modal_pick);

    // Hex.
    int hxx = bx + 330, hxy = by + 120;
    ct(hxx, hxy + 2, "#", 13, C_DIM_());
    cfield(hxx + 12, hxy, 108, 20);
    ct(hxx + 18, hxy + 2, g_hex_buf, 13, C_TEXT_());
    {
        int cw = gui_string_width(g_hex_buf);
        cr(hxx + 18 + cw, hxy + 3, 1, 15, C_ACC_());
    }

    // Before/after split swatch: left = old (click reverts), right = new.
    int swx = bx + 330, swy = by + 156;
    cr(swx, swy, 60, 44, g_modal_old);
    cr(swx + 60, swy, 60, 44, g_modal_pick.rgb & 0xFFFFFF);
    cout(swx, swy, 120, 44, C_LINE_());
    ct(swx, swy + 46, "old", 11, C_DIM_());
    ct(swx + 96, swy + 46, "new", 11, C_DIM_());

    cbtn(bx + 330, by + 210, 88, 22, "eyedrop");
    cbtn(bx + 422, by + 210, 82, 22, "+ Swatch");

    // Recent-colours strip.
    ct(bx + 16, by + 290, "Recent", 11, C_DIM_());
    for (int i = 0; i < 12; i++) {
        int cx = bx + 16 + i * 20, cy = by + 306;
        cr(cx, cy, 18, 18, C_PANEL_()); cout(cx, cy, 18, 18, C_LINE_());
        if (i < g_recent.len) { int idx = (g_recent.head - 1 - i + 24) % 12; cr(cx + 1, cy + 1, 16, 16, g_recent.c[idx]); }
    }
    cbtn(bx + 360, by + 306, 70, 24, "OK");
    cbtn(bx + 438, by + 306, 70, 24, "Cancel");
}

static void modal_commit_live(void) {
    hex_format(g_modal_pick.rgb, g_hex_buf);
    ui_full_redraw();
}

int cp_click_modal(int mx, int my) {
    if (!g_modal) return 0;
    int bx, by; modal_origin(&bx, &by);

    if (inr(mx, my, bx + MODW - 28, by + 8, 20, 20)) { modal_cancel(); return 1; }
    if (inr(mx, my, bx + 360, by + 306, 70, 24)) { modal_ok(); return 1; }
    if (inr(mx, my, bx + 438, by + 306, 70, 24)) { modal_cancel(); return 1; }

    int sqx = bx + 16, sqy = by + 40;
    if (inr(mx, my, sqx, sqy, MSQ, MSQ)) {
        g_modal_pick.S = clampi((mx - sqx) * 255 / (MSQ - 1), 0, 255);
        g_modal_pick.V = clampi(255 - (my - sqy) * 255 / (MSQ - 1), 0, 255);
        int r, g, b; cp_hsv2rgb(g_modal_pick.H, g_modal_pick.S, g_modal_pick.V, &r, &g, &b);
        g_modal_pick.rgb = argb(0, r, g, b);
        g_mdrag = DRAG_SV; modal_commit_live(); return 1;
    }
    int hux = bx + 284, huy = by + 40;
    if (inr(mx, my, hux, huy, MHUW, MSQ)) {
        g_modal_pick.H = clampi((my - huy) * 359 / (MSQ - 1), 0, 359);
        int r, g, b; cp_hsv2rgb(g_modal_pick.H, g_modal_pick.S, g_modal_pick.V, &r, &g, &b);
        g_modal_pick.rgb = argb(0, r, g, b);
        g_mdrag = DRAG_HUE; modal_commit_live(); return 1;
    }

    // Steppers.
    int dch, dfx, dfw;
    int hs = hit_stepper(mx, my, &g_modal_pick, &dch, &dfx, &dfw);
    if (hs) {
        if (hs == 2) { g_mdrag = DRAG_STEP; g_mdrag_ch = dch; g_mdrag_fx = dfx; g_mdrag_fw = dfw; }
        modal_commit_live(); return 1;
    }

    int hxx = bx + 330, hxy = by + 120;
    if (inr(mx, my, hxx, hxy, 120, 20)) return 1;    // hex focus is implicit in modal (always typeable)

    // Before/after: click "old" half reverts.
    int swx = bx + 330, swy = by + 156;
    if (inr(mx, my, swx, swy, 60, 44)) {
        pick_from_rgb(&g_modal_pick, g_modal_old);
        modal_commit_live(); return 1;
    }

    if (inr(mx, my, bx + 330, by + 210, 88, 22)) {   // eyedrop from canvas -> activate the tool + close
        g_tool.id = TL_PICK; modal_cancel(); ui_status("Eyedropper: click the canvas"); return 1;
    }
    if (inr(mx, my, bx + 422, by + 210, 82, 22)) {   // add new to user palette
        if (g_user_pal >= 0) { pal_add(&g_pal[g_user_pal], g_modal_pick.rgb, 0); g_pal_active = g_user_pal; pal_save_user(); }
        modal_commit_live(); return 1;
    }

    // Recent strip -> load into working colour.
    for (int i = 0; i < 12 && i < g_recent.len; i++) {
        int cx = bx + 16 + i * 20, cy = by + 306;
        if (inr(mx, my, cx, cy, 18, 18)) {
            int idx = (g_recent.head - 1 - i + 24) % 12;
            pick_from_rgb(&g_modal_pick, g_recent.c[idx]);
            modal_commit_live(); return 1;
        }
    }
    return 1;   // consume clicks anywhere inside the modal
}

int cp_drag_modal(int mx, int my) {
    if (g_mdrag == DRAG_NONE) return 0;
    if (g_mdrag == DRAG_SV) {
        g_modal_pick.S = clampi((mx - g_md_sqx) * 255 / (MSQ - 1), 0, 255);
        g_modal_pick.V = clampi(255 - (my - g_md_sqy) * 255 / (MSQ - 1), 0, 255);
        int r, g, b; cp_hsv2rgb(g_modal_pick.H, g_modal_pick.S, g_modal_pick.V, &r, &g, &b);
        g_modal_pick.rgb = argb(0, r, g, b);
    } else if (g_mdrag == DRAG_HUE) {
        g_modal_pick.H = clampi((my - g_md_huy) * 359 / (MSQ - 1), 0, 359);
        int r, g, b; cp_hsv2rgb(g_modal_pick.H, g_modal_pick.S, g_modal_pick.V, &r, &g, &b);
        g_modal_pick.rgb = argb(0, r, g, b);
    } else if (g_mdrag == DRAG_STEP) {
        int vmax = g_mdrag_ch == CPCH_H ? 359 : 255;
        int rel = mx - g_mdrag_fx;
        step_apply(&g_modal_pick, g_mdrag_ch, rel * vmax / (g_mdrag_fw > 1 ? g_mdrag_fw - 1 : 1));
    }
    modal_commit_live();
    return 1;
}
void cp_modal_release(void) { g_mdrag = DRAG_NONE; }

int cp_key_modal(const void *ev) {
    if (!g_modal) return 0;
    const gui_event_t *e = (const gui_event_t *)ev;
    char ch = e->key_char;
    if (ch == 27) { modal_cancel(); return 1; }
    if (ch == '\n' || ch == '\r' || e->keycode == 0x1C) { modal_ok(); return 1; }
    // Hex typing.
    if (tf_handle_key(&g_hex_tf, e)) {
        uint32_t rgb;
        if (hex_parse(g_hex_buf, &rgb)) pick_from_rgb(&g_modal_pick, rgb);
        ui_full_redraw();
    }
    return 1;
}
