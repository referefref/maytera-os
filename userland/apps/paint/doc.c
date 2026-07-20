// doc.c - Maytera Studio document/layer engine (see studio.h for the module
// contract). Owns the g_doc global: layer stack, the full GIMP 2.10/3
// blend-mode set, mask-aware compositing, saved-selection channels, the
// full-document snapshot undo/redo system, and the histogram.
//
// Layer order convention: layer[0] is the BOTTOM of the stack,
// layer[nlayers-1] is the TOP. layer_move(dir=+1) moves a layer up in z.
//
// All pixel buffers are malloc'd (never large statics, blame #444) and all
// per-pixel math is integer only (no libm / no float).

#include "studio.h"

doc_t g_doc;

// Rounded a*b/255 for 0..255 operands.
#define MUL255(a, b) ((((a) * (b)) + 127) / 255)

// ---------------------------------------------------------------------------
// Integer colour helpers for the component blend modes (HSL, hue 0..1535).
// ---------------------------------------------------------------------------
static int lum255(int r, int g, int b) { return (r * 77 + g * 150 + b * 29) >> 8; }

static void rgb2hsl(int r, int g, int b, int *H, int *S, int *L) {
    int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int l = (mx + mn) / 2, s = 0, h = 0, d = mx - mn;
    if (d != 0) {
        s = (l < 128) ? (d * 255) / (mx + mn) : (d * 255) / (510 - mx - mn);
        int hh;
        if (mx == r)      hh = ((g - b) * 256) / d + (g < b ? 1536 : 0);
        else if (mx == g) hh = ((b - r) * 256) / d + 512;
        else              hh = ((r - g) * 256) / d + 1024;
        h = hh % 1536; if (h < 0) h += 1536;
    }
    *H = h; *S = s; *L = l;
}
static int hue2rgb(int p, int q, int t) {
    if (t < 0) t += 1536;
    if (t >= 1536) t -= 1536;
    if (t < 256)  return p + ((q - p) * t) / 256;
    if (t < 768)  return q;
    if (t < 1024) return p + ((q - p) * (1024 - t)) / 256;
    return p;
}
static void hsl2rgb(int h, int s, int l, int *R, int *G, int *B) {
    if (s == 0) { *R = *G = *B = l; return; }
    int q = (l < 128) ? (l * (255 + s)) / 255 : (l + s - (l * s) / 255);
    int p = 2 * l - q;
    *R = clampi(hue2rgb(p, q, h + 512), 0, 255);
    *G = clampi(hue2rgb(p, q, h),       0, 255);
    *B = clampi(hue2rgb(p, q, h - 512), 0, 255);
}

// ---------------------------------------------------------------------------
// Undo/redo snapshots. Full-document copies: layer topology + every layer's
// pixels + masks + the selection. The comp buffer is a derived cache and is
// not snapshotted. Channels (saved selections) are NOT snapshotted.
// ---------------------------------------------------------------------------
typedef struct {
    int      w, h;
    int      nlayers, active;
    layer_t  layer[STUDIO_MAX_LAYERS];   // px/mask pointers own malloc'd copies
    uint8_t *sel;
    int      sel_active;
    char     label[STUDIO_NAME_LEN];
} snapshot_t;

static snapshot_t s_undo[STUDIO_MAX_UNDO];
static int        s_undo_n = 0;
static snapshot_t s_redo[STUDIO_MAX_UNDO];
static int        s_redo_n = 0;

static void snap_free(snapshot_t *s) {
    for (int i = 0; i < s->nlayers; i++) {
        if (s->layer[i].px) free(s->layer[i].px);
        if (s->layer[i].mask) free(s->layer[i].mask);
        s->layer[i].px = NULL; s->layer[i].mask = NULL;
    }
    if (s->sel) free(s->sel);
    memset(s, 0, sizeof(*s));
}

static void stack_drop_oldest(snapshot_t *st, int *n) {
    if (*n <= 0) return;
    snap_free(&st[0]);
    memmove(&st[0], &st[1], (size_t)(*n - 1) * sizeof(snapshot_t));
    (*n)--;
    memset(&st[*n], 0, sizeof(snapshot_t));
}

static int snap_capture(snapshot_t *s, const char *label) {
    memset(s, 0, sizeof(*s));
    s->w = g_doc.w; s->h = g_doc.h;
    s->nlayers = g_doc.nlayers; s->active = g_doc.active;
    s->sel_active = g_doc.sel_active;
    strlcpy(s->label, label ? label : "edit", sizeof(s->label));

    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    for (int i = 0; i < g_doc.nlayers; i++) {
        s->layer[i] = g_doc.layer[i];          // copies name/opacity/flags + ptrs
        s->layer[i].px = (uint32_t *)malloc(npx * 4);
        s->layer[i].mask = NULL;
        if (!s->layer[i].px) { s->nlayers = i; snap_free(s); return -1; }
        memcpy(s->layer[i].px, g_doc.layer[i].px, npx * 4);
        if (g_doc.layer[i].mask) {
            s->layer[i].mask = (uint8_t *)malloc(npx);
            if (!s->layer[i].mask) { s->nlayers = i + 1; snap_free(s); return -1; }
            memcpy(s->layer[i].mask, g_doc.layer[i].mask, npx);
        }
    }
    if (g_doc.sel) {
        s->sel = (uint8_t *)malloc(npx);
        if (!s->sel) { snap_free(s); return -1; }
        memcpy(s->sel, g_doc.sel, npx);
    }
    return 0;
}

static void snap_steal_current(snapshot_t *s, const char *label) {
    memset(s, 0, sizeof(*s));
    s->w = g_doc.w; s->h = g_doc.h;
    s->nlayers = g_doc.nlayers; s->active = g_doc.active;
    s->sel_active = g_doc.sel_active;
    strlcpy(s->label, label ? label : "edit", sizeof(s->label));
    for (int i = 0; i < g_doc.nlayers; i++) {
        s->layer[i] = g_doc.layer[i];
        g_doc.layer[i].px = NULL;
        g_doc.layer[i].mask = NULL;
    }
    s->sel = g_doc.sel;
    g_doc.sel = NULL;
}

static void snap_restore(snapshot_t *s) {
    for (int i = 0; i < g_doc.nlayers; i++) {
        if (g_doc.layer[i].px) free(g_doc.layer[i].px);
        if (g_doc.layer[i].mask) free(g_doc.layer[i].mask);
        g_doc.layer[i].px = NULL; g_doc.layer[i].mask = NULL;
    }
    if (g_doc.sel) { free(g_doc.sel); g_doc.sel = NULL; }

    int resized = (g_doc.w != s->w) || (g_doc.h != s->h);
    g_doc.w = s->w; g_doc.h = s->h;
    g_doc.nlayers = s->nlayers; g_doc.active = s->active;
    g_doc.sel_active = s->sel_active;
    for (int i = 0; i < s->nlayers; i++) {
        g_doc.layer[i] = s->layer[i];
        s->layer[i].px = NULL; s->layer[i].mask = NULL;
    }
    for (int i = s->nlayers; i < STUDIO_MAX_LAYERS; i++)
        memset(&g_doc.layer[i], 0, sizeof(layer_t));
    g_doc.sel = s->sel; s->sel = NULL;

    if (resized || !g_doc.comp) {
        if (g_doc.comp) free(g_doc.comp);
        g_doc.comp = (uint32_t *)malloc((size_t)g_doc.w * (size_t)g_doc.h * 4);
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    memset(s, 0, sizeof(*s));
}

static void undo_reset(void) {
    while (s_undo_n > 0) stack_drop_oldest(s_undo, &s_undo_n);
    while (s_redo_n > 0) stack_drop_oldest(s_redo, &s_redo_n);
}

void undo_push(const char *label) {
    if (!g_doc.nlayers) return;
    snapshot_t snap;
    if (snap_capture(&snap, label) != 0) {
        stack_drop_oldest(s_undo, &s_undo_n);
        if (snap_capture(&snap, label) != 0) return;
    }
    if (s_undo_n == STUDIO_MAX_UNDO) stack_drop_oldest(s_undo, &s_undo_n);
    s_undo[s_undo_n++] = snap;
    while (s_redo_n > 0) stack_drop_oldest(s_redo, &s_redo_n);
}

int undo_undo(void) {
    if (s_undo_n <= 0) return 0;
    snapshot_t prev = s_undo[--s_undo_n];
    memset(&s_undo[s_undo_n], 0, sizeof(snapshot_t));
    snapshot_t cur; snap_steal_current(&cur, prev.label);
    if (s_redo_n == STUDIO_MAX_UNDO) stack_drop_oldest(s_redo, &s_redo_n);
    s_redo[s_redo_n++] = cur;
    snap_restore(&prev);
    return 1;
}

int undo_redo(void) {
    if (s_redo_n <= 0) return 0;
    snapshot_t next = s_redo[--s_redo_n];
    memset(&s_redo[s_redo_n], 0, sizeof(snapshot_t));
    snapshot_t cur; snap_steal_current(&cur, next.label);
    if (s_undo_n == STUDIO_MAX_UNDO) stack_drop_oldest(s_undo, &s_undo_n);
    s_undo[s_undo_n++] = cur;
    snap_restore(&next);
    return 1;
}

int undo_count(void) { return s_undo_n; }
const char *undo_label(int i) {
    if (i < 0 || i >= s_undo_n) return "";
    return s_undo[i].label;
}
// Menu-driven queries: is undo/redo available, and what would the next one do.
int undo_can_undo(void) { return s_undo_n > 0; }
int undo_can_redo(void) { return s_redo_n > 0; }
const char *undo_next_label(void) { return s_undo_n > 0 ? s_undo[s_undo_n - 1].label : ""; }
const char *redo_next_label(void) { return s_redo_n > 0 ? s_redo[s_redo_n - 1].label : ""; }

// ---------------------------------------------------------------------------
// Blending - the full GIMP 2.10/3 mode set.
// ---------------------------------------------------------------------------
static int cdodge(int d, int s) { return s >= 255 ? (d ? 255 : 0) : clampi(d * 255 / (255 - s), 0, 255); }
static int cburn(int d, int s)  { return s <= 0 ? (d >= 255 ? 255 : 0) : 255 - clampi((255 - d) * 255 / s, 0, 255); }

uint32_t blend_px(uint32_t dst, uint32_t src, blend_t mode, int opacity) {
    int sa = MUL255(px_a(src), clampi(opacity, 0, 255));
    if (sa <= 0) return dst;

    int da = px_a(dst);
    int sr = px_r(src), sg = px_g(src), sb = px_b(src);
    int dr = px_r(dst), dg = px_g(dst), db = px_b(dst);
    int br = sr, bg = sg, bb = sb;

    switch (mode) {
    case BLEND_NORMAL: case BLEND_DISSOLVE: break;   // DISSOLVE approx = NORMAL (no coords here)
    case BLEND_MULTIPLY:
        br = MUL255(dr, sr); bg = MUL255(dg, sg); bb = MUL255(db, sb); break;
    case BLEND_SCREEN:
        br = 255 - MUL255(255 - dr, 255 - sr); bg = 255 - MUL255(255 - dg, 255 - sg);
        bb = 255 - MUL255(255 - db, 255 - sb); break;
    case BLEND_OVERLAY:
        br = (dr < 128) ? MUL255(2*dr, sr) : 255 - MUL255(2*(255-dr), 255-sr);
        bg = (dg < 128) ? MUL255(2*dg, sg) : 255 - MUL255(2*(255-dg), 255-sg);
        bb = (db < 128) ? MUL255(2*db, sb) : 255 - MUL255(2*(255-db), 255-sb); break;
    case BLEND_ADD:
        br = clampi(dr+sr,0,255); bg = clampi(dg+sg,0,255); bb = clampi(db+sb,0,255); break;
    case BLEND_SUBTRACT:
        br = clampi(dr-sr,0,255); bg = clampi(dg-sg,0,255); bb = clampi(db-sb,0,255); break;
    case BLEND_LIGHTEN:
        br = dr>sr?dr:sr; bg = dg>sg?dg:sg; bb = db>sb?db:sb; break;
    case BLEND_DARKEN:
        br = dr<sr?dr:sr; bg = dg<sg?dg:sg; bb = db<sb?db:sb; break;
    case BLEND_DODGE:
        br = cdodge(dr,sr); bg = cdodge(dg,sg); bb = cdodge(db,sb); break;
    case BLEND_BURN:
        br = cburn(dr,sr); bg = cburn(dg,sg); bb = cburn(db,sb); break;
    case BLEND_SOFTLIGHT:
        br = clampi(((255-2*sr)*dr*dr)/(255*255) + (2*sr*dr)/255, 0, 255);
        bg = clampi(((255-2*sg)*dg*dg)/(255*255) + (2*sg*dg)/255, 0, 255);
        bb = clampi(((255-2*sb)*db*db)/(255*255) + (2*sb*db)/255, 0, 255); break;
    case BLEND_HARDLIGHT:
        br = (sr < 128) ? MUL255(2*sr, dr) : 255 - MUL255(2*(255-sr), 255-dr);
        bg = (sg < 128) ? MUL255(2*sg, dg) : 255 - MUL255(2*(255-sg), 255-dg);
        bb = (sb < 128) ? MUL255(2*sb, db) : 255 - MUL255(2*(255-sb), 255-db); break;
    case BLEND_VIVIDLIGHT:
        br = (sr < 128) ? cburn(dr, clampi(2*sr,0,255)) : cdodge(dr, clampi(2*(sr-128),0,255));
        bg = (sg < 128) ? cburn(dg, clampi(2*sg,0,255)) : cdodge(dg, clampi(2*(sg-128),0,255));
        bb = (sb < 128) ? cburn(db, clampi(2*sb,0,255)) : cdodge(db, clampi(2*(sb-128),0,255)); break;
    case BLEND_PINLIGHT:
        br = (sr < 128) ? (dr<2*sr?dr:2*sr) : (dr>2*(sr-128)?dr:2*(sr-128));
        bg = (sg < 128) ? (dg<2*sg?dg:2*sg) : (dg>2*(sg-128)?dg:2*(sg-128));
        bb = (sb < 128) ? (db<2*sb?db:2*sb) : (db>2*(sb-128)?db:2*(sb-128)); break;
    case BLEND_LINEARLIGHT:
        br = clampi(dr+2*sr-255,0,255); bg = clampi(dg+2*sg-255,0,255); bb = clampi(db+2*sb-255,0,255); break;
    case BLEND_HARDMIX:
        br = (sr+dr>=255)?255:0; bg = (sg+dg>=255)?255:0; bb = (sb+db>=255)?255:0; break;
    case BLEND_DIFFERENCE:
        br = dr>sr?dr-sr:sr-dr; bg = dg>sg?dg-sg:sg-dg; bb = db>sb?db-sb:sb-db; break;
    case BLEND_EXCLUSION:
        br = dr+sr-2*MUL255(dr,sr); bg = dg+sg-2*MUL255(dg,sg); bb = db+sb-2*MUL255(db,sb); break;
    case BLEND_GRAINEXTRACT:
        br = clampi(dr-sr+128,0,255); bg = clampi(dg-sg+128,0,255); bb = clampi(db-sb+128,0,255); break;
    case BLEND_GRAINMERGE:
        br = clampi(dr+sr-128,0,255); bg = clampi(dg+sg-128,0,255); bb = clampi(db+sb-128,0,255); break;
    case BLEND_DIVIDE:
        br = clampi((dr*256)/(sr+1),0,255); bg = clampi((dg*256)/(sg+1),0,255); bb = clampi((db*256)/(sb+1),0,255); break;
    case BLEND_HUE: {
        int sH,sS,sL,dH,dS,dL; rgb2hsl(sr,sg,sb,&sH,&sS,&sL); rgb2hsl(dr,dg,db,&dH,&dS,&dL);
        hsl2rgb(sH,dS,dL,&br,&bg,&bb); break; }
    case BLEND_SATURATION: {
        int sH,sS,sL,dH,dS,dL; rgb2hsl(sr,sg,sb,&sH,&sS,&sL); rgb2hsl(dr,dg,db,&dH,&dS,&dL);
        hsl2rgb(dH,sS,dL,&br,&bg,&bb); break; }
    case BLEND_COLOR: {
        int sH,sS,sL,dH,dS,dL; rgb2hsl(sr,sg,sb,&sH,&sS,&sL); rgb2hsl(dr,dg,db,&dH,&dS,&dL);
        hsl2rgb(sH,sS,dL,&br,&bg,&bb); break; }
    case BLEND_VALUE: {
        int sH,sS,sL,dH,dS,dL; rgb2hsl(sr,sg,sb,&sH,&sS,&sL); rgb2hsl(dr,dg,db,&dH,&dS,&dL);
        hsl2rgb(dH,dS,sL,&br,&bg,&bb); break; }
    case BLEND_LUMA_LIGHTEN:
        if (lum255(sr,sg,sb) >= lum255(dr,dg,db)) { br=sr;bg=sg;bb=sb; } else { br=dr;bg=dg;bb=db; } break;
    case BLEND_LUMA_DARKEN:
        if (lum255(sr,sg,sb) <= lum255(dr,dg,db)) { br=sr;bg=sg;bb=sb; } else { br=dr;bg=dg;bb=db; } break;
    default: break;
    }
    // Mode result only where dst has content; degrade to NORMAL over transparency.
    br = sr + ((br - sr) * da + 127) / 255;
    bg = sg + ((bg - sg) * da + 127) / 255;
    bb = sb + ((bb - sb) * da + 127) / 255;

    int oa = sa + MUL255(da, 255 - sa);
    if (oa <= 0) return 0;
    int den = oa * 255;
    int orr = (br * sa * 255 + dr * da * (255 - sa) + den / 2) / den;
    int og  = (bg * sa * 255 + dg * da * (255 - sa) + den / 2) / den;
    int ob  = (bb * sa * 255 + db * da * (255 - sa) + den / 2) / den;
    return argb(oa, orr, og, ob);
}

const char *blend_name(blend_t b) {
    switch (b) {
    case BLEND_NORMAL: return "Normal";        case BLEND_MULTIPLY: return "Multiply";
    case BLEND_SCREEN: return "Screen";        case BLEND_OVERLAY: return "Overlay";
    case BLEND_ADD: return "Addition";         case BLEND_SUBTRACT: return "Subtract";
    case BLEND_DISSOLVE: return "Dissolve";    case BLEND_LIGHTEN: return "Lighten";
    case BLEND_DARKEN: return "Darken";        case BLEND_DODGE: return "Dodge";
    case BLEND_BURN: return "Burn";            case BLEND_SOFTLIGHT: return "Soft Light";
    case BLEND_HARDLIGHT: return "Hard Light"; case BLEND_VIVIDLIGHT: return "Vivid Light";
    case BLEND_PINLIGHT: return "Pin Light";   case BLEND_LINEARLIGHT: return "Linear Light";
    case BLEND_HARDMIX: return "Hard Mix";     case BLEND_DIFFERENCE: return "Difference";
    case BLEND_EXCLUSION: return "Exclusion";  case BLEND_GRAINEXTRACT: return "Grain Extract";
    case BLEND_GRAINMERGE: return "Grain Merge"; case BLEND_DIVIDE: return "Divide";
    case BLEND_HUE: return "Hue";              case BLEND_SATURATION: return "Saturation";
    case BLEND_COLOR: return "Color";          case BLEND_VALUE: return "Value";
    case BLEND_LUMA_LIGHTEN: return "Luma Lighten"; case BLEND_LUMA_DARKEN: return "Luma Darken";
    default: return "Normal";
    }
}

// ---------------------------------------------------------------------------
// Compositing (mask-aware)
// ---------------------------------------------------------------------------
#define CHECKER_A 0xFF666666u
#define CHECKER_B 0xFF999999u
#define CHECKER_SZ 8

void doc_composite(void) {
    if (!g_doc.nlayers) return;
    if (!g_doc.comp) {
        g_doc.comp = (uint32_t *)malloc((size_t)g_doc.w * (size_t)g_doc.h * 4);
        if (!g_doc.comp) return;
        g_doc.comp_dirty = 1;
    }
    if (!g_doc.comp_dirty) return;

    int w = g_doc.w, h = g_doc.h;
    for (int y = 0; y < h; y++) {
        uint32_t *row = g_doc.comp + (size_t)y * (size_t)w;
        int yb = (y / CHECKER_SZ) & 1;
        for (int x = 0; x < w; x++)
            row[x] = (((x / CHECKER_SZ) & 1) ^ yb) ? CHECKER_B : CHECKER_A;
    }
    size_t npx = (size_t)w * (size_t)h;
    for (int li = 0; li < g_doc.nlayers; li++) {
        layer_t *L = &g_doc.layer[li];
        if (!L->visible || !L->px || L->opacity <= 0) continue;
        if (L->mask) {
            for (size_t i = 0; i < npx; i++) {
                uint32_t s = L->px[i];
                int a = MUL255(px_a(s), L->mask[i]);
                s = (s & 0x00FFFFFFu) | ((uint32_t)a << 24);
                g_doc.comp[i] = blend_px(g_doc.comp[i], s, L->blend, L->opacity);
            }
        } else {
            for (size_t i = 0; i < npx; i++)
                g_doc.comp[i] = blend_px(g_doc.comp[i], L->px[i], L->blend, L->opacity);
        }
    }
    g_doc.comp_dirty = 0;
}

// ---------------------------------------------------------------------------
// Channels (saved selections)
// ---------------------------------------------------------------------------
typedef struct { char name[STUDIO_NAME_LEN]; uint8_t *mask; int w, h; } channel_t;
static channel_t s_chan[STUDIO_MAX_CHANNELS];
static int s_chan_n = 0;

static void channels_free(void) {
    for (int i = 0; i < s_chan_n; i++)
        if (s_chan[i].mask) { free(s_chan[i].mask); s_chan[i].mask = NULL; }
    s_chan_n = 0;
    memset(s_chan, 0, sizeof(s_chan));
}

int channel_save_selection(const char *name) {
    if (s_chan_n >= STUDIO_MAX_CHANNELS) return -1;
    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    uint8_t *m = (uint8_t *)malloc(npx);
    if (!m) return -1;
    if (g_doc.sel_active && g_doc.sel) memcpy(m, g_doc.sel, npx);
    else memset(m, 255, npx);
    channel_t *c = &s_chan[s_chan_n];
    c->mask = m; c->w = g_doc.w; c->h = g_doc.h;
    strlcpy(c->name, name ? name : "Channel", STUDIO_NAME_LEN);
    return s_chan_n++;
}

int channel_load_selection(int idx) {
    if (idx < 0 || idx >= s_chan_n) return -1;
    channel_t *c = &s_chan[idx];
    if (c->w != g_doc.w || c->h != g_doc.h) return -1;
    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    if (!g_doc.sel) { g_doc.sel = (uint8_t *)malloc(npx); if (!g_doc.sel) return -1; }
    memcpy(g_doc.sel, c->mask, npx);
    g_doc.sel_active = 1;
    return 0;
}

int channel_count(void) { return s_chan_n; }
const char *channel_name(int idx) {
    if (idx < 0 || idx >= s_chan_n) return "";
    return s_chan[idx].name;
}

// ---------------------------------------------------------------------------
// Layer masks / blend / opacity / alpha
// ---------------------------------------------------------------------------
int layer_mask_add(int idx, int init) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    layer_t *L = &g_doc.layer[idx];
    if (L->mask) return 0;
    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    uint8_t *m = (uint8_t *)malloc(npx);
    if (!m) return -1;
    if (init == 1) memset(m, 0, npx);                 // black = fully hidden
    else if (init == 2) { for (size_t i = 0; i < npx; i++) m[i] = px_a(L->px[i]); }  // from alpha
    else if (init == 3) { for (size_t i = 0; i < npx; i++)                            // from grayscale
                              m[i] = (uint8_t)lum255(px_r(L->px[i]), px_g(L->px[i]), px_b(L->px[i])); }
    else memset(m, 255, npx);                         // white = fully shown
    L->mask = m;
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

int layer_mask_apply(int idx) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    layer_t *L = &g_doc.layer[idx];
    if (!L->mask) return 0;
    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    for (size_t i = 0; i < npx; i++) {
        int a = MUL255(px_a(L->px[i]), L->mask[i]);
        L->px[i] = (L->px[i] & 0x00FFFFFFu) | ((uint32_t)a << 24);
    }
    free(L->mask); L->mask = NULL; L->mask_active = 0;
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

int layer_mask_delete(int idx) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    layer_t *L = &g_doc.layer[idx];
    if (L->mask) { free(L->mask); L->mask = NULL; }
    L->mask_active = 0;
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

int layer_set_blend(int idx, blend_t b) {
    if (idx < 0 || idx >= g_doc.nlayers || b < 0 || b >= BLEND_COUNT) return -1;
    g_doc.layer[idx].blend = b;
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

int layer_set_opacity(int idx, int op) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    g_doc.layer[idx].opacity = clampi(op, 0, 255);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

int layer_add_alpha(int idx) {
    // Studio layers are always ARGB; alpha is always present. No-op success.
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    return 0;
}

int layer_flatten_alpha(int idx, uint32_t bg) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    layer_t *L = &g_doc.layer[idx];
    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    uint32_t base = bg | 0xFF000000u;
    for (size_t i = 0; i < npx; i++) {
        uint32_t o = blend_px(base, L->px[i], BLEND_NORMAL, 255);
        L->px[i] = o | 0xFF000000u;
    }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

// ---------------------------------------------------------------------------
// Histogram
// ---------------------------------------------------------------------------
void histogram(int channel, unsigned int out[256]) {
    for (int i = 0; i < 256; i++) out[i] = 0;
    doc_composite();
    if (!g_doc.comp) return;
    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    for (size_t i = 0; i < npx; i++) {
        uint32_t p = g_doc.comp[i];
        int v;
        if (channel == 1) v = px_r(p);
        else if (channel == 2) v = px_g(p);
        else if (channel == 3) v = px_b(p);
        else v = lum255(px_r(p), px_g(p), px_b(p));
        out[v & 255]++;
    }
}

// ---------------------------------------------------------------------------
// Document lifecycle
// ---------------------------------------------------------------------------
void doc_free(void) {
    for (int i = 0; i < g_doc.nlayers; i++) {
        if (g_doc.layer[i].px) free(g_doc.layer[i].px);
        if (g_doc.layer[i].mask) free(g_doc.layer[i].mask);
    }
    if (g_doc.sel) free(g_doc.sel);
    if (g_doc.comp) free(g_doc.comp);
    memset(&g_doc, 0, sizeof(g_doc));
    channels_free();
    undo_reset();
}

int doc_new(int w, int h, uint32_t bg) {
    if (w < 1 || h < 1 || w > STUDIO_MAX_W || h > STUDIO_MAX_H) return -1;
    doc_free();

    size_t npx = (size_t)w * (size_t)h;
    g_doc.w = w; g_doc.h = h;
    g_doc.layer[0].px = (uint32_t *)malloc(npx * 4);
    if (!g_doc.layer[0].px) { memset(&g_doc, 0, sizeof(g_doc)); return -1; }
    g_doc.comp = (uint32_t *)malloc(npx * 4);
    if (!g_doc.comp) { free(g_doc.layer[0].px); memset(&g_doc, 0, sizeof(g_doc)); return -1; }
    uint32_t fill = bg | 0xFF000000u;
    for (size_t i = 0; i < npx; i++) g_doc.layer[0].px[i] = fill;
    g_doc.layer[0].opacity = 255;
    g_doc.layer[0].visible = 1;
    g_doc.layer[0].blend = BLEND_NORMAL;
    g_doc.layer[0].group = -1;
    strlcpy(g_doc.layer[0].name, "Background", STUDIO_NAME_LEN);
    g_doc.nlayers = 1;
    g_doc.active = 0;
    g_doc.comp_dirty = 1;
    g_doc.path[0] = 0;
    g_doc.modified = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// Layer stack operations
// ---------------------------------------------------------------------------
int layer_add(const char *name, uint32_t fill) {
    if (g_doc.nlayers <= 0 || g_doc.nlayers >= STUDIO_MAX_LAYERS) return -1;
    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    uint32_t *px = (uint32_t *)malloc(npx * 4);
    if (!px) return -1;
    for (size_t i = 0; i < npx; i++) px[i] = fill;

    int idx = g_doc.active + 1;
    memmove(&g_doc.layer[idx + 1], &g_doc.layer[idx],
            (size_t)(g_doc.nlayers - idx) * sizeof(layer_t));
    layer_t *L = &g_doc.layer[idx];
    memset(L, 0, sizeof(*L));
    L->px = px; L->opacity = 255; L->visible = 1; L->blend = BLEND_NORMAL; L->group = -1;
    strlcpy(L->name, name ? name : "Layer", STUDIO_NAME_LEN);
    g_doc.nlayers++;
    g_doc.active = idx;
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return idx;
}

int layer_dup(int idx) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    if (g_doc.nlayers >= STUDIO_MAX_LAYERS) return -1;
    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    uint32_t *px = (uint32_t *)malloc(npx * 4);
    if (!px) return -1;
    memcpy(px, g_doc.layer[idx].px, npx * 4);
    uint8_t *mask = NULL;
    if (g_doc.layer[idx].mask) {
        mask = (uint8_t *)malloc(npx);
        if (!mask) { free(px); return -1; }
        memcpy(mask, g_doc.layer[idx].mask, npx);
    }
    int at = idx + 1;
    memmove(&g_doc.layer[at + 1], &g_doc.layer[at],
            (size_t)(g_doc.nlayers - at) * sizeof(layer_t));
    g_doc.layer[at] = g_doc.layer[idx];
    g_doc.layer[at].px = px;
    g_doc.layer[at].mask = mask;
    g_doc.layer[at].mask_active = 0;
    strlcat(g_doc.layer[at].name, " copy", STUDIO_NAME_LEN);
    g_doc.nlayers++;
    g_doc.active = at;
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return at;
}

int layer_del(int idx) {
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    if (g_doc.nlayers <= 1) return -1;
    if (g_doc.layer[idx].px) free(g_doc.layer[idx].px);
    if (g_doc.layer[idx].mask) free(g_doc.layer[idx].mask);
    memmove(&g_doc.layer[idx], &g_doc.layer[idx + 1],
            (size_t)(g_doc.nlayers - idx - 1) * sizeof(layer_t));
    g_doc.nlayers--;
    memset(&g_doc.layer[g_doc.nlayers], 0, sizeof(layer_t));
    if (g_doc.active > idx) g_doc.active--;
    if (g_doc.active >= g_doc.nlayers) g_doc.active = g_doc.nlayers - 1;
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return 0;
}

int layer_move(int idx, int dir) {
    int to = idx + (dir > 0 ? 1 : -1);
    if (idx < 0 || idx >= g_doc.nlayers) return -1;
    if (to < 0 || to >= g_doc.nlayers) return -1;
    layer_t tmp = g_doc.layer[idx];
    g_doc.layer[idx] = g_doc.layer[to];
    g_doc.layer[to] = tmp;
    if (g_doc.active == idx) g_doc.active = to;
    else if (g_doc.active == to) g_doc.active = idx;
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    return to;
}

int layer_merge_down(int idx) {
    if (idx < 1 || idx >= g_doc.nlayers) return -1;
    layer_t *up = &g_doc.layer[idx];
    layer_t *lo = &g_doc.layer[idx - 1];
    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    if (up->visible && up->opacity > 0) {
        for (size_t i = 0; i < npx; i++) {
            uint32_t s = up->px[i];
            if (up->mask) {
                int a = MUL255(px_a(s), up->mask[i]);
                s = (s & 0x00FFFFFFu) | ((uint32_t)a << 24);
            }
            lo->px[i] = blend_px(lo->px[i], s, up->blend, up->opacity);
        }
    }
    layer_del(idx);
    g_doc.active = idx - 1;
    return 0;
}

void doc_flatten(void) {
    if (g_doc.nlayers <= 1) {
        if (g_doc.nlayers == 1 && g_doc.layer[0].mask) layer_mask_apply(0);
        return;
    }
    size_t npx = (size_t)g_doc.w * (size_t)g_doc.h;
    uint32_t *flat = (uint32_t *)malloc(npx * 4);
    if (!flat) return;
    memset(flat, 0, npx * 4);
    for (int li = 0; li < g_doc.nlayers; li++) {
        layer_t *L = &g_doc.layer[li];
        if (!L->visible || !L->px || L->opacity <= 0) continue;
        for (size_t i = 0; i < npx; i++) {
            uint32_t s = L->px[i];
            if (L->mask) { int a = MUL255(px_a(s), L->mask[i]); s = (s & 0x00FFFFFFu) | ((uint32_t)a << 24); }
            flat[i] = blend_px(flat[i], s, L->blend, L->opacity);
        }
    }
    for (int i = 0; i < g_doc.nlayers; i++) {
        if (g_doc.layer[i].px) free(g_doc.layer[i].px);
        if (g_doc.layer[i].mask) free(g_doc.layer[i].mask);
        memset(&g_doc.layer[i], 0, sizeof(layer_t));
    }
    g_doc.layer[0].px = flat;
    g_doc.layer[0].opacity = 255;
    g_doc.layer[0].visible = 1;
    g_doc.layer[0].blend = BLEND_NORMAL;
    g_doc.layer[0].group = -1;
    strlcpy(g_doc.layer[0].name, "Background", STUDIO_NAME_LEN);
    g_doc.nlayers = 1;
    g_doc.active = 0;
    g_doc.comp_dirty = 1; g_doc.modified = 1;
}
