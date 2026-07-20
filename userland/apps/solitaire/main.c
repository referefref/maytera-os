// solitaire - Klondike Solitaire for MayteraOS (user-space version)
// Classic card game with drag-and-drop support.
//
// Felt background + resizable/aspect-preserving layout:
//  - The table is drawn over a felt image (/SOLITBG.DAT, raw BGRA blob) that is
//    cover-scaled into the window so its perspective is never stretched.
//  - The whole board (cards, spacing, margins, toolbar) scales by one factor
//    derived from the current window content size, so resizing shrinks/grows the
//    contents proportionally. We render the offscreen buffer at the TRUE content
//    size (queried via win_get_size / EVENT_RESIZE) so the kernel does not have
//    to stretch the blit (which would distort the felt on a non-matching aspect).
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"

// Initial window size (content aspect ~ the felt image 1375x761 = 1.807)
#define SOL_WIDTH       760
#define SOL_HEIGHT      451     // content 421 + 30 (title+border) ~ 1.805 aspect

// Base (design) card dimensions, scaled by g_fnum/1000 at runtime.
#define CARD_W          71
#define CARD_H          96
#define CARD_SPACING    15  // Vertical spacing for stacked cards (design)
#define PILE_SPACING    (CARD_W + 10)

// Layout (design units, scaled at runtime)
#define TOOLBAR_H       36
#define MARGIN_LEFT     10
// Design content reference used to derive the scale factor.
#define DESIGN_W        620
#define DESIGN_H        510

// Colors
#define TABLE_COLOR     0x006B8E23  // Green felt fallback (if image missing)
#define CARD_WHITE      0x00FFFEF8  // Card face
#define CARD_BACK       0x00203080  // Blue card back
#define CARD_BORDER     0x00404040
#define CARD_RED        0x00CC2020
#define CARD_BLACK      0x00202020
#define HIGHLIGHT       0x00FFFF00
#define EMPTY_PILE      0x00507030

// Card suits
#define SUIT_HEARTS     0
#define SUIT_DIAMONDS   1
#define SUIT_CLUBS      2
#define SUIT_SPADES     3

// Card structure
typedef struct {
    uint8_t suit;    // 0-3
    uint8_t value;   // 1-13 (Ace-King)
    bool face_up;
} card_t;

// Pile types
#define PILE_STOCK      0
#define PILE_WASTE      1
#define PILE_FOUNDATION 2
#define PILE_TABLEAU    3

// Pile structure
#define MAX_PILE_SIZE   24
typedef struct {
    card_t cards[MAX_PILE_SIZE];
    int count;
    uint8_t type;
    uint8_t pile_index;
} card_pile_t;

// Game state
static int window_handle = -1;
static card_pile_t stock;
static card_pile_t waste;
static card_pile_t foundations[4];
static card_pile_t tableau[7];

// Undo / redo (full game-state snapshots)
typedef struct { card_pile_t stock, waste, foundations[4], tableau[7]; } game_snap_t;
#define UNDO_MAX 64
static game_snap_t undo_stack[UNDO_MAX]; static int undo_count = 0;
static game_snap_t redo_stack[UNDO_MAX]; static int redo_count = 0;
static void snap_save(game_snap_t *s) {
    s->stock = stock; s->waste = waste;
    for (int i = 0; i < 4; i++) s->foundations[i] = foundations[i];
    for (int i = 0; i < 7; i++) s->tableau[i] = tableau[i];
}
static void snap_load(game_snap_t *s) {
    stock = s->stock; waste = s->waste;
    for (int i = 0; i < 4; i++) foundations[i] = s->foundations[i];
    for (int i = 0; i < 7; i++) tableau[i] = s->tableau[i];
}
static int snap_eq(const game_snap_t *a, const game_snap_t *b) {
    const unsigned char *pa = (const unsigned char *)a, *pb = (const unsigned char *)b;
    for (unsigned i = 0; i < sizeof(game_snap_t); i++) if (pa[i] != pb[i]) return 0;
    return 1;
}
static void undo_push(const game_snap_t *before) {
    if (undo_count >= UNDO_MAX) { for (int i = 1; i < UNDO_MAX; i++) undo_stack[i-1] = undo_stack[i]; undo_count--; }
    undo_stack[undo_count++] = *before;
    redo_count = 0;
}
static void do_undo(void) {
    if (undo_count <= 0) return;
    game_snap_t cur; snap_save(&cur);
    if (redo_count < UNDO_MAX) redo_stack[redo_count++] = cur;
    snap_load(&undo_stack[--undo_count]);
}
static void do_redo(void) {
    if (redo_count <= 0) return;
    game_snap_t cur; snap_save(&cur);
    if (undo_count < UNDO_MAX) undo_stack[undo_count++] = cur;
    snap_load(&redo_stack[--redo_count]);
}

// Double-click detection (counts event-loop ~100ms timeouts since last click).
static int g_dbl = 0;
static int g_last_pt = -9, g_last_pi = -9, g_last_ci = -9;

// Drag state
static bool dragging = false;
static card_pile_t drag_pile;
static int drag_from_type;
static int drag_from_index;
static int drag_x, drag_y;
static int drag_offset_x, drag_offset_y;

// Selection
static int selected_pile_type = -1;
static int selected_pile_index = -1;
static int selected_card_index = -1;

// Random seed
static uint32_t rand_seed;

// Window position
static int win_x = 50;
static int win_y = 30;

// Forward declarations
static void solitaire_redraw(void);
static void new_game(void);

// Simple random number generator
static int sol_rand(void) {
    rand_seed = rand_seed * 1103515245 + 12345;
    return (rand_seed >> 16) & 0x7FFF;
}

// Check if suit is red
static bool is_red_suit(uint8_t suit) {
    return suit == SUIT_HEARTS || suit == SUIT_DIAMONDS;
}

// Check if opposite colors
static bool is_opposite_color(uint8_t suit1, uint8_t suit2) {
    return is_red_suit(suit1) != is_red_suit(suit2);
}

// Initialize a deck
static void init_deck(card_t *deck) {
    int i = 0;
    for (int suit = 0; suit < 4; suit++) {
        for (int value = 1; value <= 13; value++) {
            deck[i].suit = suit;
            deck[i].value = value;
            deck[i].face_up = false;
            i++;
        }
    }
}

// Shuffle deck using Fisher-Yates
static void shuffle_deck(card_t *deck, int count) {
    for (int i = count - 1; i > 0; i--) {
        int j = sol_rand() % (i + 1);
        card_t temp = deck[i];
        deck[i] = deck[j];
        deck[j] = temp;
    }
}

// Push card to pile
static bool pile_push(card_pile_t *pile, card_t card) {
    if (pile->count >= MAX_PILE_SIZE) return false;
    pile->cards[pile->count++] = card;
    return true;
}

// Pop card from pile
static bool pile_pop(card_pile_t *pile, card_t *card) {
    if (pile->count == 0) return false;
    *card = pile->cards[--pile->count];
    return true;
}

// Get top card
static card_t *pile_top(card_pile_t *pile) {
    if (pile->count == 0) return NULL;
    return &pile->cards[pile->count - 1];
}

// Clear pile
static void pile_clear(card_pile_t *pile) {
    pile->count = 0;
}

// ============================================================================
// Offscreen framebuffer + felt image + SVG card deck (rendered with sys_win_blit).
// sys_win_blit replaces the whole window content, so we compose the table and
// card images into g_fb at the TRUE content size, then overlay UI text.
// ============================================================================
// Max content canvas (covers a full-screen maximized window). g_fb is used at
// the CURRENT content stride (g_cw), packed, not at MAXW.
#define MAXW        1280
#define MAXH        800
#define NUM_CARDS   53                  // 1 back + 52 faces
// Felt cache (the /SOLITBG.DAT blob is preprocessed to this size, aspect 1.807).
#define FELTW       768
#define FELTH       425

static uint32_t g_fb[MAXW * MAXH];
static uint32_t g_deck[NUM_CARDS * CARD_W * CARD_H];   // BGRA card atlas
static int g_deck_loaded = 0;
static uint32_t g_felt[FELTW * FELTH];                 // felt image (op-masked on use)
static int g_felt_loaded = 0;
static int g_feltw = FELTW, g_felth = FELTH;
static uint16_t g_colidx[MAXW];                        // cover-scale lookup (per resize)
static uint16_t g_rowidx[MAXH];

// Current content size + scale.
static int g_cw = SOL_WIDTH, g_ch = SOL_HEIGHT - 30;
static int g_fnum = 1000;   // scale factor * 1000

// Scaled layout (computed by compute_layout)
static int Lcw, Lch, Lvsp, Lsp, Lml, Lmt, Ltab, Ltbh;
static int g_tbx0, g_tbw, g_tbh, g_tby, g_tbgap;

static inline int SCL(int v) { return v * g_fnum / 1000; }

static void compute_layout(void) {
    if (g_cw < 320) g_cw = 320; if (g_ch < 240) g_ch = 240;
    if (g_cw > MAXW) g_cw = MAXW; if (g_ch > MAXH) g_ch = MAXH;
    int fw = g_cw * 1000 / DESIGN_W;
    int fh = g_ch * 1000 / DESIGN_H;
    g_fnum = fw < fh ? fw : fh;
    if (g_fnum < 350) g_fnum = 350;
    if (g_fnum > 2400) g_fnum = 2400;

    Lcw  = SCL(CARD_W);
    Lch  = SCL(CARD_H);
    Lvsp = SCL(CARD_SPACING); if (Lvsp < 4) Lvsp = 4;
    Lsp  = SCL(PILE_SPACING);
    Ltbh = SCL(TOOLBAR_H);    if (Ltbh < 24) Ltbh = 24;

    g_tbx0  = SCL(8);
    g_tbw   = SCL(42);
    g_tbh   = SCL(28);
    g_tby   = SCL(4);
    g_tbgap = SCL(6);

    // Center the 7-pile board horizontally within the content.
    int boardw = 6 * Lsp + Lcw;
    Lml = (g_cw - boardw) / 2;
    if (Lml < SCL(MARGIN_LEFT)) Lml = SCL(MARGIN_LEFT);
    if (Lml < 4) Lml = 4;
    Lmt  = Ltbh + SCL(10);
    Ltab = Lmt + Lch + SCL(20);
}

// Card deck file: 'CDK1' + count(4) + w(4) + h(4) + count*w*h*4 BGRA pixels.
// Index 0 = back; faces = 1 + suit*13 + (value-1), suit order H,D,C,S.
static void load_deck(void) {
    int fd = open("/CARDS.DAT", 0);
    if (fd < 0) return;
    char hdr[16];
    if (read(fd, hdr, 16) != 16) { close(fd); return; }
    if (hdr[0] != 'C' || hdr[1] != 'D' || hdr[2] != 'K' || hdr[3] != '1') { close(fd); return; }
    int want = NUM_CARDS * CARD_W * CARD_H * 4, got = 0;
    char *dst = (char *)g_deck;
    while (got < want) { long n = read(fd, dst + got, want - got); if (n <= 0) break; got += (int)n; }
    close(fd);
    if (got == want) g_deck_loaded = 1;
}

// Felt file: 'SBG1' + w(4 LE) + h(4 LE) + w*h*4 BGRA pixels.
static void load_felt(void) {
    int fd = open("/SOLITBG.DAT", 0);
    if (fd < 0) return;
    unsigned char h[12];
    if (read(fd, (char *)h, 12) != 12) { close(fd); return; }
    if (h[0] != 'S' || h[1] != 'B' || h[2] != 'G' || h[3] != '1') { close(fd); return; }
    int w = h[4] | (h[5] << 8) | (h[6] << 16) | (h[7] << 24);
    int ht = h[8] | (h[9] << 8) | (h[10] << 16) | (h[11] << 24);
    if (w <= 0 || ht <= 0 || w > FELTW || ht > FELTH) { close(fd); return; }
    int want = w * ht * 4, got = 0;
    char *dst = (char *)g_felt;
    while (got < want) { long n = read(fd, dst + got, want - got); if (n <= 0) break; got += (int)n; }
    close(fd);
    if (got == want) { g_feltw = w; g_felth = ht; g_felt_loaded = 1; }
}

// Build the cover-scale lookup tables for the current content size.
static void build_felt_idx(void) {
    if (!g_felt_loaded) return;
    int fw = g_feltw, fh = g_felth;
    int dw, dh;
    // Cover: scale so the felt fully covers the canvas (crop overflow), aspect kept.
    if ((long)g_cw * fh >= (long)g_ch * fw) { dw = g_cw; dh = (int)((long)fh * g_cw / fw); }
    else { dh = g_ch; dw = (int)((long)fw * g_ch / fh); }
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    int ox = (g_cw - dw) / 2, oy = (g_ch - dh) / 2;  // <= 0
    for (int dx = 0; dx < g_cw; dx++) {
        int sx = (int)((long)(dx - ox) * fw / dw);
        if (sx < 0) sx = 0; if (sx >= fw) sx = fw - 1;
        g_colidx[dx] = (uint16_t)sx;
    }
    for (int dy = 0; dy < g_ch; dy++) {
        int sy = (int)((long)(dy - oy) * fh / dh);
        if (sy < 0) sy = 0; if (sy >= fh) sy = fh - 1;
        g_rowidx[dy] = (uint16_t)sy;
    }
}

static inline void win_blit_fb(void) {
    syscall5(SYS_WIN_BLIT, window_handle, 0, 0,
             (g_cw & 0xFFFF) | ((g_ch & 0xFFFF) << 16), (long)g_fb);
}

static inline uint32_t op(uint32_t rgb) { return 0xFF000000u | (rgb & 0xFFFFFF); }

// Paint the felt background (cover-scaled) into the whole canvas.
static void felt_fill(void) {
    if (!g_felt_loaded) {
        int n = g_cw * g_ch; uint32_t c = op(TABLE_COLOR);
        for (int i = 0; i < n; i++) g_fb[i] = c;
        return;
    }
    for (int dy = 0; dy < g_ch; dy++) {
        const uint32_t *row = g_felt + (int)g_rowidx[dy] * g_feltw;
        uint32_t *dst = g_fb + dy * g_cw;
        for (int dx = 0; dx < g_cw; dx++) dst[dx] = op(row[g_colidx[dx]]);
    }
}

static void fb_rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = 0; j < h; j++) { int py = y + j; if (py < 0 || py >= g_ch) continue;
        for (int i = 0; i < w; i++) { int px = x + i; if (px < 0 || px >= g_cw) continue;
            g_fb[py * g_cw + px] = c; } }
}
static void fb_outline(int x, int y, int w, int h, uint32_t c) {
    for (int i = 0; i < w; i++) { fb_rect(x + i, y, 1, 1, c); fb_rect(x + i, y + h - 1, 1, 1, c); }
    for (int j = 0; j < h; j++) { fb_rect(x, y + j, 1, 1, c); fb_rect(x + w - 1, y + j, 1, 1, c); }
}

// ---- MICO .ICN loader + blit into the offscreen fb (toolbar Zest icons) ----
// MICO: 'MICO' + w(u32 LE) + h(u32 LE) + w*h*4 BGRA. White glyphs; the grey
// value is used as coverage and recoloured to the requested ink so they stay
// theme-tinted. Small fixed cache (toolbar only needs 3).
#define SOL_MICO_DIM 64
#define SOL_MICO_N   4
typedef struct { char name[12]; int w, h, loaded; unsigned char px[SOL_MICO_DIM*SOL_MICO_DIM*4]; } sol_mico_t;
static sol_mico_t g_smico[SOL_MICO_N];
static int g_smico_n = 0;

static sol_mico_t *sol_mico_get(const char *name) {
    for (int i = 0; i < g_smico_n; i++) {
        int k = 0; while (name[k] && g_smico[i].name[k] == name[k]) k++;
        if (!name[k] && !g_smico[i].name[k]) return &g_smico[i];
    }
    if (g_smico_n >= SOL_MICO_N) return 0;
    sol_mico_t *ic = &g_smico[g_smico_n++];
    int k = 0; for (; name[k] && k < 11; k++) ic->name[k] = name[k]; ic->name[k] = 0;
    ic->loaded = -1; ic->w = ic->h = 0;
    char path[40]; int l = 0; const char *p = "/ICONS/"; while (*p) path[l++] = *p++;
    for (int i = 0; name[i] && l < 35; i++) path[l++] = name[i];
    const char *e = ".ICN"; while (*e) path[l++] = *e++; path[l] = 0;
    int fd = open(path, 0); if (fd < 0) return ic;
    unsigned char hdr[12];
    if (read(fd, (char *)hdr, 12) != 12 ||
        hdr[0]!='M' || hdr[1]!='I' || hdr[2]!='C' || hdr[3]!='O') { close(fd); return ic; }
    int w = hdr[4]|(hdr[5]<<8)|(hdr[6]<<16)|(hdr[7]<<24);
    int h = hdr[8]|(hdr[9]<<8)|(hdr[10]<<16)|(hdr[11]<<24);
    if (w<=0 || h<=0 || w>SOL_MICO_DIM || h>SOL_MICO_DIM) { close(fd); return ic; }
    int want = w*h*4, got = 0;
    while (got < want) { long n = read(fd, (char *)ic->px + got, want - got); if (n <= 0) break; got += (int)n; }
    close(fd); if (got != want) return ic;
    ic->w = w; ic->h = h; ic->loaded = 1; return ic;
}

// Blit icon `name` centered+scaled into the (bx,by,bw,bh) button into g_fb, ink-tinted.
static int fb_icon_btn(const char *name, int bx, int by, int bw, int bh, uint32_t ink) {
    sol_mico_t *ic = sol_mico_get(name);
    if (!ic || ic->loaded != 1) return 0;
    int size = (bw < bh ? bw : bh) - SCL(10); if (size < 8) size = 8;
    int ox = bx + (bw - size)/2, oy = by + (bh - size)/2;
    int tr = (ink>>16)&0xff, tg = (ink>>8)&0xff, tb = ink&0xff;
    for (int dy = 0; dy < size; dy++) {
        int py = oy + dy; if (py < 0 || py >= g_ch) continue;
        int sy = dy * ic->h / size; if (sy >= ic->h) sy = ic->h - 1;
        for (int dx = 0; dx < size; dx++) {
            int px = ox + dx; if (px < 0 || px >= g_cw) continue;
            int sx = dx * ic->w / size; if (sx >= ic->w) sx = ic->w - 1;
            const unsigned char *s = &ic->px[(sy * ic->w + sx) * 4];
            int b = s[0], g = s[1], r = s[2], a = s[3];
            if (a == 0) continue;
            int cov = (r*30 + g*59 + b*11)/100;        // white glyph -> coverage
            int aa = a * cov / 255; if (aa <= 0) continue;
            uint32_t d = g_fb[py * g_cw + px];
            int dr = (d>>16)&0xff, dg = (d>>8)&0xff, db = d&0xff;
            int rr = (tr*aa + dr*(255-aa))/255;
            int gg = (tg*aa + dg*(255-aa))/255;
            int bb = (tb*aa + db*(255-aa))/255;
            g_fb[py * g_cw + px] = 0xFF000000u | (rr<<16) | (gg<<8) | bb;
        }
    }
    return 1;
}

// Empty-foundation suit pip, lifted from the suit's Ace card art in the atlas
// (the rendered SVG cards). The pip mask is extracted (tight bbox so it never
// clips), area-averaged for a smooth core, then given a white halo + a black
// halo with a distance-based antialiased band so it reads as a crisp outlined
// sticker on the felt. suit: 0=H,1=D,2=C,3=S (atlas order).
#define PIP_MAXB 88
static unsigned char g_pip_core[PIP_MAXB * PIP_MAXB];   // AA core coverage 0..255
static int pip_isqrt(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
static void fb_suit_pip(int x, int y, int suit) {
    if (!g_deck_loaded) return;
    const uint32_t *src = g_deck + (1 + suit * 13) * CARD_W * CARD_H;
    // Generous central crop (skip the corner ranks), then find the pip's tight bbox.
    int csxl = CARD_W * 16 / 100, csxr = CARD_W * 84 / 100;
    int csyt = CARD_H * 20 / 100, csyb = CARD_H * 80 / 100;
    int minx = csxr, maxx = csxl, miny = csyb, maxy = csyt;
    for (int sy = csyt; sy < csyb; sy++)
        for (int sx = csxl; sx < csxr; sx++) {
            uint32_t c = src[sy * CARD_W + sx];
            int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
            if (r > 214 && g > 214 && b > 204) continue;          // card white
            if (sx < minx) minx = sx; if (sx > maxx) maxx = sx;
            if (sy < miny) miny = sy; if (sy > maxy) maxy = sy;
        }
    if (maxx < minx || maxy < miny) return;
    int sbw = maxx - minx + 1, sbh = maxy - miny + 1;
    int red = (suit == 0 || suit == 1);
    int pr = red ? 0xCC : 0x1C, pg = red ? 0x22 : 0x1C, pb = red ? 0x22 : 0x1C;
    int wring = SCL(2); if (wring < 2) wring = 2;       // white halo width (px)
    int ring  = SCL(2); if (ring  < 2) ring  = 2;       // black halo width (px)
    int aa = 2;                                          // outer AA band (px)
    int ow = wring + ring + aa;
    int availW = Lcw * 62 / 100 - 2 * ow, availH = Lch * 50 / 100 - 2 * ow;
    if (availW < 4 || availH < 4 || sbw < 1 || sbh < 1) return;
    int coreW, coreH;                                    // preserve pip aspect
    if (availW * sbh <= availH * sbw) { coreW = availW; coreH = availW * sbh / sbw; }
    else { coreH = availH; coreW = availH * sbw / sbh; }
    if (coreW < 3) coreW = 3; if (coreH < 3) coreH = 3;
    int badgeW = coreW + 2 * ow, badgeH = coreH + 2 * ow;
    if (badgeW > PIP_MAXB) { badgeW = PIP_MAXB; coreW = badgeW - 2 * ow; }
    if (badgeH > PIP_MAXB) { badgeH = PIP_MAXB; coreH = badgeH - 2 * ow; }
    if (coreW < 3 || coreH < 3) return;
    int bx0 = x + (Lcw - badgeW) / 2, by0 = y + (Lch - badgeH) / 2;
    // AA core coverage: area-average the source pip mask into the core box.
    for (int i = 0; i < badgeW * badgeH; i++) g_pip_core[i] = 0;
    for (int j = 0; j < coreH; j++) {
        int sy0 = miny + j * sbh / coreH, sy1 = miny + (j + 1) * sbh / coreH; if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int i = 0; i < coreW; i++) {
            int sx0 = minx + i * sbw / coreW, sx1 = minx + (i + 1) * sbw / coreW; if (sx1 <= sx0) sx1 = sx0 + 1;
            int cnt = 0, sum = 0;
            for (int sy = sy0; sy < sy1; sy++) for (int sx = sx0; sx < sx1; sx++) {
                uint32_t c = src[sy * CARD_W + sx];
                int r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
                sum += (r > 214 && g > 214 && b > 204) ? 0 : 255; cnt++;
            }
            g_pip_core[(j + ow) * badgeW + (i + ow)] = (unsigned char)(cnt ? sum / cnt : 0);
        }
    }
    // Composite felt -> black halo -> white halo -> pip, with a distance AA band.
    uint32_t felt = EMPTY_PILE;
    int fr = (felt >> 16) & 0xff, fg = (felt >> 8) & 0xff, fb = felt & 0xff;
    int rb = wring + ring, rbaa = rb + aa;
    for (int j = 0; j < badgeH; j++) {
        int py = by0 + j; if (py < 0 || py >= g_ch) continue;
        for (int i = 0; i < badgeW; i++) {
            int px = bx0 + i; if (px < 0 || px >= g_cw) continue;
            int core = g_pip_core[j * badgeW + i];
            // nearest covered (core>=96) pixel: squared distance within rbaa window
            int best = (rbaa + 1) * (rbaa + 1);
            for (int dy = -rbaa; dy <= rbaa; dy++) {
                int jj = j + dy; if (jj < 0 || jj >= badgeH) continue;
                for (int dx = -rbaa; dx <= rbaa; dx++) {
                    int ii = i + dx; if (ii < 0 || ii >= badgeW) continue;
                    if (g_pip_core[jj * badgeW + ii] < 96) continue;
                    int d2 = dx * dx + dy * dy; if (d2 < best) best = d2;
                }
            }
            int d = pip_isqrt(best);
            int blackA = d <= rb ? 255 : (d >= rbaa ? 0 : 255 * (rbaa - d) / aa);
            if (blackA == 0 && core == 0) continue;
            int whiteA = d <= wring ? 255 : (d >= wring + aa ? 0 : 255 * (wring + aa - d) / aa);
            int rr = fr, gg = fg, bb = fb;
            rr = rr * (255 - blackA) / 255; gg = gg * (255 - blackA) / 255; bb = bb * (255 - blackA) / 255;
            rr = (255 * whiteA + rr * (255 - whiteA)) / 255;
            gg = (255 * whiteA + gg * (255 - whiteA)) / 255;
            bb = (255 * whiteA + bb * (255 - whiteA)) / 255;
            rr = (pr * core + rr * (255 - core)) / 255;
            gg = (pg * core + gg * (255 - core)) / 255;
            bb = (pb * core + bb * (255 - core)) / 255;
            g_fb[py * g_cw + px] = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
        }
    }
}
// Alpha-composite card image `idx` into g_fb at (x,y), scaled to cw x ch.
// Uses an alpha-weighted AREA-AVERAGE (box filter): each destination pixel
// averages the full source box it covers, so shrinking the board (e.g. at 75%
// window width) stays crisp instead of dropping pixels (nearest-neighbor).
static void fb_card(int x, int y, int idx, int cw, int ch) {
    if (!g_deck_loaded || idx < 0 || idx >= NUM_CARDS) {
        fb_rect(x, y, cw, ch, op(CARD_WHITE));
        fb_outline(x, y, cw, ch, op(CARD_BORDER));
        return;
    }
    if (cw <= 0 || ch <= 0) return;
    uint32_t *src = g_deck + idx * CARD_W * CARD_H;
    for (int j = 0; j < ch; j++) { int py = y + j; if (py < 0 || py >= g_ch) continue;
        int sy0 = j * CARD_H / ch;
        int sy1 = (j + 1) * CARD_H / ch; if (sy1 <= sy0) sy1 = sy0 + 1; if (sy1 > CARD_H) sy1 = CARD_H;
        for (int i = 0; i < cw; i++) { int px = x + i; if (px < 0 || px >= g_cw) continue;
            int sx0 = i * CARD_W / cw;
            int sx1 = (i + 1) * CARD_W / cw; if (sx1 <= sx0) sx1 = sx0 + 1; if (sx1 > CARD_W) sx1 = CARD_W;
            // Alpha-weighted average over the covered source box.
            uint32_t asum = 0, rsum = 0, gsum = 0, bsum = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const uint32_t *srow = src + yy * CARD_W;
                for (int xx = sx0; xx < sx1; xx++) {
                    uint32_t s = srow[xx]; uint32_t a = (s >> 24) & 0xFF;
                    asum += a;
                    rsum += ((s >> 16) & 0xFF) * a;
                    gsum += ((s >> 8) & 0xFF) * a;
                    bsum += (s & 0xFF) * a;
                    n++;
                }
            }
            if (n == 0) continue;
            uint32_t a = asum / n;
            if (a == 0) continue;            // box was (near) fully transparent
            uint32_t sr = rsum / asum, sg = gsum / asum, sb = bsum / asum;
            uint32_t *d = &g_fb[py * g_cw + px];
            if (a >= 255) { *d = op((sr << 16) | (sg << 8) | sb); continue; }
            uint32_t dr = (*d >> 16) & 0xFF, dg = (*d >> 8) & 0xFF, db = *d & 0xFF;
            *d = op((((sr*a + dr*(255-a))/255) << 16) | (((sg*a + dg*(255-a))/255) << 8) | ((sb*a + db*(255-a))/255));
        }
    }
}

// Draw a card into the offscreen framebuffer (scaled).
static void draw_card(int x, int y, card_t *card, bool highlight) {
    if (card == NULL) { fb_outline(x, y, Lcw, Lch, op(EMPTY_PILE)); return; }
    if (!card->face_up) { fb_card(x, y, 0, Lcw, Lch); return; }   // back
    fb_card(x, y, 1 + card->suit * 13 + (card->value - 1), Lcw, Lch);
    if (highlight) {
        fb_outline(x + 1, y + 1, Lcw - 2, Lch - 2, op(HIGHLIGHT));
        fb_outline(x + 2, y + 2, Lcw - 4, Lch - 4, op(HIGHLIGHT));
    }
}

// Draw empty pile outline
static void draw_empty_pile(int x, int y) {
    fb_outline(x, y, Lcw, Lch, op(EMPTY_PILE));
}

// Start new game
static void new_game(void) {
    pile_clear(&stock);
    pile_clear(&waste);
    for (int i = 0; i < 4; i++) pile_clear(&foundations[i]);
    for (int i = 0; i < 7; i++) pile_clear(&tableau[i]);

    card_t deck[52];
    init_deck(deck);
    shuffle_deck(deck, 52);

    int card_idx = 0;
    for (int col = 0; col < 7; col++) {
        for (int row = col; row < 7; row++) {
            deck[card_idx].face_up = (row == col);
            pile_push(&tableau[row], deck[card_idx]);
            card_idx++;
        }
    }
    for (int i = card_idx; i < 52; i++) pile_push(&stock, deck[i]);

    selected_pile_type = -1;
    selected_pile_index = -1;
    selected_card_index = -1;
    dragging = false;
    undo_count = 0;
    redo_count = 0;
}

// Draw stock and waste piles
static void draw_stock_waste(void) {
    int x = Lml;
    int y = Lmt;
    if (stock.count > 0) draw_card(x, y, &stock.cards[stock.count - 1], false);
    else draw_empty_pile(x, y);

    x += Lsp;
    if (waste.count > 0) {
        bool highlight = (selected_pile_type == PILE_WASTE);
        draw_card(x, y, &waste.cards[waste.count - 1], highlight);
    } else draw_empty_pile(x, y);
}

// Draw foundation piles
static void draw_foundations(void) {
    int x = Lml + 3 * Lsp;
    int y = Lmt;
    for (int i = 0; i < 4; i++) {
        if (foundations[i].count > 0) {
            bool highlight = (selected_pile_type == PILE_FOUNDATION && selected_pile_index == i);
            draw_card(x, y, &foundations[i].cards[foundations[i].count - 1], highlight);
        } else { draw_empty_pile(x, y); fb_suit_pip(x, y, i); }
        x += Lsp;
    }
}

// Draw tableau piles
static void draw_tableau(void) {
    for (int col = 0; col < 7; col++) {
        int x = Lml + col * Lsp;
        int y = Ltab;
        if (tableau[col].count == 0) {
            draw_empty_pile(x, y);
        } else {
            for (int i = 0; i < tableau[col].count; i++) {
                bool highlight = (selected_pile_type == PILE_TABLEAU &&
                                  selected_pile_index == col &&
                                  i >= selected_card_index);
                draw_card(x, y, &tableau[col].cards[i], highlight);
                y += Lvsp;
            }
        }
    }
}

// ---- top command bar: New / Undo / Redo icon buttons (drawn into g_fb) ----
static int tb_btn_x(int i) { return g_tbx0 + i * (g_tbw + g_tbgap); }   // 0=New 1=Undo 2=Redo

static void fb_tri_left(int tx, int cy, int W, uint32_t c)  { for (int i = 0; i < W; i++) fb_rect(tx + i, cy - i, 1, 2*i + 1, c); }
static void fb_tri_right(int tx, int cy, int W, uint32_t c) { for (int i = 0; i < W; i++) fb_rect(tx - i, cy - i, 1, 2*i + 1, c); }

static void draw_toolbar(void) {
    // Command-bar chrome follows the active theme (read at draw time so a live
    // theme change is reflected on the next frame). The felt and cards are not
    // themed -- they are the game's visual identity.
    fb_rect(0, 0, g_cw, Ltbh, op(theme_color(THEME_COLOR_TASKBAR_BG)));
    fb_rect(0, Ltbh - 1, g_cw, 1, op(theme_color(THEME_COLOR_WINDOW_BORDER)));
    int aw = SCL(8);   // arrow half-extent
    for (int i = 0; i < 3; i++) {
        int bx = tb_btn_x(i), by = g_tby;
        int en = (i == 0) || (i == 1 && undo_count > 0) || (i == 2 && redo_count > 0);
        uint32_t ink = en ? op(theme_color(THEME_COLOR_BUTTON_TEXT))
                          : op(theme_color(THEME_COLOR_BUTTON_DISABLED));
        fb_rect(bx, by, g_tbw, g_tbh, op(theme_color(THEME_COLOR_BUTTON_FACE)));
        fb_outline(bx, by, g_tbw, g_tbh, op(theme_color(THEME_COLOR_BUTTON_DARK)));
        // Zest icons: New=reply-all, Undo=reply (UNDO.ICN), Redo=forward (REDO.ICN).
        const char *icn = (i == 0) ? "REPLYALL" : (i == 1) ? "UNDO" : "REDO";
        if (!fb_icon_btn(icn, bx, by, g_tbw, g_tbh, ink)) {
            int cx = bx + g_tbw / 2, cy = by + g_tbh / 2;   // fallback glyphs
            if (i == 0) {
                int hw = SCL(8), hh = SCL(9);
                fb_rect(cx - hw, cy - hh, hw * 2, hh * 2, 0xFFF4F4F0);
                fb_outline(cx - hw, cy - hh, hw * 2, hh * 2, 0xFF808890);
                fb_rect(cx - SCL(2), cy - SCL(5), SCL(4), SCL(10), 0xFF2FA84F);
                fb_rect(cx - SCL(5), cy - SCL(2), SCL(10), SCL(4), 0xFF2FA84F);
            } else if (i == 1) {
                fb_tri_left(cx - aw, cy, SCL(6), ink); fb_rect(cx - SCL(2), cy - 1, SCL(9), SCL(3), ink);
            } else {
                fb_tri_right(cx + aw, cy, SCL(6), ink); fb_rect(cx - SCL(7), cy - 1, SCL(9), SCL(3), ink);
            }
        }
    }
}

// Full redraw: compose felt + table + card images into g_fb, blit it, then
// overlay the small bits of UI text (which the blit can't carry).
static void solitaire_redraw(void) {
    felt_fill();                     // felt image background (cover-scaled)
    draw_toolbar();

    draw_stock_waste();
    draw_foundations();
    draw_tableau();

    if (dragging && drag_pile.count > 0) {
        int y = drag_y;
        for (int i = 0; i < drag_pile.count; i++) {
            draw_card(drag_x, y, &drag_pile.cards[i], false);
            y += Lvsp;
        }
    }

    win_blit_fb();                   // push composed table to the window

    // ---- overlays (drawn on top of the blit) ----
    if (stock.count == 0)
        win_draw_text(window_handle, Lml + Lcw/4, Lmt + Lch/2 - 8, "Reset", 0x00FFFFFF);
    // (Empty-foundation suit pips are drawn into g_fb by fb_suit_pip in
    //  draw_foundations, replacing the old H/D/C/S text overlay.)
    win_invalidate(window_handle);
}

// Get pile at point (content-local coords, scaled geometry)
static void get_pile_at_point(int x, int y, int *pile_type, int *pile_index, int *card_index) {
    *pile_type = -1;
    *pile_index = -1;
    *card_index = -1;

    if (x >= Lml && x < Lml + Lcw && y >= Lmt && y < Lmt + Lch) {
        *pile_type = PILE_STOCK; *pile_index = 0; *card_index = stock.count - 1; return;
    }
    if (x >= Lml + Lsp && x < Lml + Lsp + Lcw && y >= Lmt && y < Lmt + Lch) {
        *pile_type = PILE_WASTE; *pile_index = 0; *card_index = waste.count - 1; return;
    }
    for (int i = 0; i < 4; i++) {
        int fx = Lml + (3 + i) * Lsp;
        if (x >= fx && x < fx + Lcw && y >= Lmt && y < Lmt + Lch) {
            *pile_type = PILE_FOUNDATION; *pile_index = i; *card_index = foundations[i].count - 1; return;
        }
    }
    for (int col = 0; col < 7; col++) {
        int tx = Lml + col * Lsp;
        if (x >= tx && x < tx + Lcw) {
            if (tableau[col].count == 0) {
                if (y >= Ltab && y < Ltab + Lch) {
                    *pile_type = PILE_TABLEAU; *pile_index = col; *card_index = -1; return;
                }
            } else {
                for (int i = tableau[col].count - 1; i >= 0; i--) {
                    int cy = Ltab + i * Lvsp;
                    int ch = (i == tableau[col].count - 1) ? Lch : Lvsp;
                    if (y >= cy && y < cy + ch) {
                        *pile_type = PILE_TABLEAU; *pile_index = col; *card_index = i; return;
                    }
                }
            }
        }
    }
}

// Draw card from stock to waste
static void draw_from_stock(void) {
    if (stock.count > 0) {
        card_t card;
        pile_pop(&stock, &card);
        card.face_up = true;
        pile_push(&waste, card);
    } else {
        while (waste.count > 0) {
            card_t card;
            pile_pop(&waste, &card);
            card.face_up = false;
            pile_push(&stock, card);
        }
    }
}

static bool can_place_foundation(card_t *card, int foundation_idx) {
    if (foundations[foundation_idx].count == 0) return card->value == 1;
    card_t *top = pile_top(&foundations[foundation_idx]);
    return top->suit == card->suit && card->value == top->value + 1;
}

static bool can_place_tableau(card_t *card, int tableau_idx) {
    if (tableau[tableau_idx].count == 0) return card->value == 13;
    card_t *top = pile_top(&tableau[tableau_idx]);
    return is_opposite_color(card->suit, top->suit) && card->value == top->value - 1;
}

static bool try_auto_foundation(card_t *card, card_pile_t *from_pile) {
    for (int i = 0; i < 4; i++) {
        if (can_place_foundation(card, i)) {
            card_t c;
            pile_pop(from_pile, &c);
            pile_push(&foundations[i], c);
            return true;
        }
    }
    return false;
}

static void handle_card_click(int pile_type, int pile_index, int card_index) {
    if (pile_type == PILE_STOCK) { draw_from_stock(); selected_pile_type = -1; return; }
    if (pile_type == PILE_WASTE && waste.count > 0) {
        selected_pile_type = PILE_WASTE; selected_pile_index = 0; selected_card_index = waste.count - 1; return;
    }
    if (pile_type == PILE_TABLEAU && card_index >= 0) {
        card_t *card = &tableau[pile_index].cards[card_index];
        if (!card->face_up) {
            if (card_index == tableau[pile_index].count - 1) card->face_up = true;
            return;
        }
        selected_pile_type = PILE_TABLEAU; selected_pile_index = pile_index; selected_card_index = card_index; return;
    }
    if (pile_type == PILE_FOUNDATION) {
        selected_pile_type = PILE_FOUNDATION; selected_pile_index = pile_index; selected_card_index = foundations[pile_index].count - 1; return;
    }
    selected_pile_type = -1;
}

static void handle_destination_click(int pile_type, int pile_index) {
    if (selected_pile_type < 0) return;
    card_t *moving_card = NULL;
    if (selected_pile_type == PILE_WASTE) moving_card = pile_top(&waste);
    else if (selected_pile_type == PILE_TABLEAU) moving_card = &tableau[selected_pile_index].cards[selected_card_index];
    else if (selected_pile_type == PILE_FOUNDATION) moving_card = pile_top(&foundations[selected_pile_index]);
    if (!moving_card) { selected_pile_type = -1; return; }

    if (pile_type == PILE_FOUNDATION) {
        if (selected_card_index == (selected_pile_type == PILE_WASTE ? waste.count - 1 :
            selected_pile_type == PILE_TABLEAU ? tableau[selected_pile_index].count - 1 :
            foundations[selected_pile_index].count - 1)) {
            if (can_place_foundation(moving_card, pile_index)) {
                card_t card;
                if (selected_pile_type == PILE_WASTE) pile_pop(&waste, &card);
                else if (selected_pile_type == PILE_TABLEAU) pile_pop(&tableau[selected_pile_index], &card);
                else pile_pop(&foundations[selected_pile_index], &card);
                pile_push(&foundations[pile_index], card);
            }
        }
    }
    if (pile_type == PILE_TABLEAU) {
        if (can_place_tableau(moving_card, pile_index)) {
            if (selected_pile_type == PILE_WASTE) {
                card_t card; pile_pop(&waste, &card); pile_push(&tableau[pile_index], card);
            } else if (selected_pile_type == PILE_TABLEAU) {
                int from = selected_pile_index;
                int cards_to_move = tableau[from].count - selected_card_index;
                for (int i = 0; i < cards_to_move; i++)
                    pile_push(&tableau[pile_index], tableau[from].cards[selected_card_index + i]);
                tableau[from].count = selected_card_index;
            } else if (selected_pile_type == PILE_FOUNDATION) {
                card_t card; pile_pop(&foundations[selected_pile_index], &card); pile_push(&tableau[pile_index], card);
            }
        }
    }
    selected_pile_type = -1;
}

static bool check_win(void) {
    for (int i = 0; i < 4; i++) if (foundations[i].count != 13) return false;
    return true;
}

// ---- drag-and-drop ----------------------------------------------------------
static int s_pressed = 0, s_px, s_py, s_pt, s_pi, s_pci;
static game_snap_t s_before;

static int card_topleft(int pt, int pi, int ci, int *tx, int *ty) {
    if (pt == PILE_WASTE)      { *tx = Lml + Lsp; *ty = Lmt; return 1; }
    if (pt == PILE_FOUNDATION) { *tx = Lml + (3 + pi) * Lsp; *ty = Lmt; return 1; }
    if (pt == PILE_TABLEAU)    { *tx = Lml + pi * Lsp; *ty = Ltab + (ci < 0 ? 0 : ci) * Lvsp; return 1; }
    return 0;
}

static void begin_drag(int pt, int pi, int ci) {
    drag_pile.count = 0;
    if (pt == PILE_WASTE && waste.count > 0) {
        card_t c; pile_pop(&waste, &c); pile_push(&drag_pile, c);
        drag_from_type = PILE_WASTE; drag_from_index = 0;
    } else if (pt == PILE_FOUNDATION && foundations[pi].count > 0) {
        card_t c; pile_pop(&foundations[pi], &c); pile_push(&drag_pile, c);
        drag_from_type = PILE_FOUNDATION; drag_from_index = pi;
    } else if (pt == PILE_TABLEAU && ci >= 0 && ci < tableau[pi].count &&
               tableau[pi].cards[ci].face_up) {
        int start = ci, ok = 1;
        for (int k = ci; k < tableau[pi].count - 1; k++) {
            card_t *a = &tableau[pi].cards[k], *b = &tableau[pi].cards[k + 1];
            if (!(is_opposite_color(a->suit, b->suit) && b->value == a->value - 1)) { ok = 0; break; }
        }
        if (!ok) start = tableau[pi].count - 1;
        int n = tableau[pi].count - start;
        for (int k = 0; k < n; k++) drag_pile.cards[k] = tableau[pi].cards[start + k];
        drag_pile.count = n; tableau[pi].count = start;
        drag_from_type = PILE_TABLEAU; drag_from_index = pi;
    }
    dragging = (drag_pile.count > 0);
}

static void return_drag(void) {
    if (drag_from_type == PILE_TABLEAU)
        for (int k = 0; k < drag_pile.count; k++) pile_push(&tableau[drag_from_index], drag_pile.cards[k]);
    else if (drag_from_type == PILE_WASTE) pile_push(&waste, drag_pile.cards[0]);
    else if (drag_from_type == PILE_FOUNDATION) pile_push(&foundations[drag_from_index], drag_pile.cards[0]);
}

static void end_drag(int lx, int ly) {
    int pt, pi, ci; get_pile_at_point(lx, ly, &pt, &pi, &ci);
    int placed = 0;
    if (drag_pile.count > 0) {
        if (pt == PILE_TABLEAU && can_place_tableau(&drag_pile.cards[0], pi)) {
            for (int k = 0; k < drag_pile.count; k++) pile_push(&tableau[pi], drag_pile.cards[k]);
            placed = 1;
        } else if (pt == PILE_FOUNDATION && drag_pile.count == 1 &&
                   can_place_foundation(&drag_pile.cards[0], pi)) {
            pile_push(&foundations[pi], drag_pile.cards[0]); placed = 1;
        }
        if (!placed) return_drag();
        else if (drag_from_type == PILE_TABLEAU && tableau[drag_from_index].count > 0) {
            card_t *t = pile_top(&tableau[drag_from_index]); if (!t->face_up) t->face_up = true;
        }
    }
    drag_pile.count = 0; dragging = false;
}

// Refresh content size from the window manager, recompute layout + felt scale.
static void refresh_size(int w, int h) {
    if (w > 0 && h > 0) { g_cw = w; g_ch = h; }
    compute_layout();
    build_felt_idx();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    rand_seed = clock();

    window_handle = win_create("Solitaire", win_x, win_y, SOL_WIDTH, SOL_HEIGHT);
    if (window_handle < 0) return 1;

    printf("Solitaire window created (handle=%d)\n", window_handle);

    load_deck();
    load_felt();

    // Query the actual content size (fall back to the creation size).
    int cw = 0, ch = 0;
    if (win_get_size(window_handle, &cw, &ch) == 0 && cw > 0 && ch > 0) refresh_size(cw, ch);
    else refresh_size(SOL_WIDTH, SOL_HEIGHT - 30);

    stock.type = PILE_STOCK;
    waste.type = PILE_WASTE;
    for (int i = 0; i < 4; i++) { foundations[i].type = PILE_FOUNDATION; foundations[i].pile_index = i; }
    for (int i = 0; i < 7; i++) { tableau[i].type = PILE_TABLEAU; tableau[i].pile_index = i; }

    new_game();
    solitaire_redraw();

    gui_event_t event;
    int running = 1;

    while (running) {
        int event_type = win_get_event(window_handle, &event, 100);

        if (event_type == 0) {
            if (g_dbl > 0) g_dbl--;
            continue;
        }

        switch (event.type) {
            case EVENT_REDRAW:
                solitaire_redraw();
                break;

            case EVENT_RESIZE:
                {
                    // param: mouse_x = new width, mouse_y = new height. Prefer the
                    // authoritative content size from the WM.
                    int nw = 0, nh = 0;
                    if (win_get_size(window_handle, &nw, &nh) != 0 || nw <= 0 || nh <= 0) {
                        nw = event.mouse_x; nh = event.mouse_y;
                    }
                    refresh_size(nw, nh);
                    solitaire_redraw();
                }
                break;

            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_KEY_DOWN:
                {
                    char c = event.key_char;
                    if (c == 27) running = 0;
                    else if (c == 'n' || c == 'N') { new_game(); solitaire_redraw(); }
                    else if (c == 'u' || c == 'U') { do_undo(); solitaire_redraw(); }
                    else if (c == 'r' || c == 'R') { do_redo(); solitaire_redraw(); }
                }
                break;

            case EVENT_MOUSE_DOWN:
                {
                    int cwx, cwy; win_get_pos(window_handle, &cwx, &cwy);
                    int local_x = event.mouse_x - cwx - 2;
                    int local_y = event.mouse_y - cwy - 22;

                    if (local_y >= g_tby && local_y < g_tby + g_tbh) {
                        int hit = -1;
                        for (int i = 0; i < 3; i++) {
                            int bx = tb_btn_x(i);
                            if (local_x >= bx && local_x < bx + g_tbw) { hit = i; break; }
                        }
                        if (hit == 0) { new_game(); solitaire_redraw(); break; }
                        if (hit == 1) { do_undo(); solitaire_redraw(); break; }
                        if (hit == 2) { do_redo(); solitaire_redraw(); break; }
                    }

                    get_pile_at_point(local_x, local_y, &s_pt, &s_pi, &s_pci);
                    s_px = local_x; s_py = local_y; s_pressed = 1;
                    snap_save(&s_before);
                }
                break;

            case EVENT_MOUSE_MOVE:
                {
                    if (!s_pressed) break;
                    int cwx, cwy; win_get_pos(window_handle, &cwx, &cwy);
                    int local_x = event.mouse_x - cwx - 2;
                    int local_y = event.mouse_y - cwy - 22;
                    if (!dragging) {
                        int dx = local_x - s_px, dy = local_y - s_py;
                        if (dx < 0) dx = -dx; if (dy < 0) dy = -dy;
                        if (dx + dy > 6) {
                            int tlx, tly;
                            if (card_topleft(s_pt, s_pi, s_pci, &tlx, &tly)) {
                                drag_offset_x = s_px - tlx; drag_offset_y = s_py - tly;
                            } else { drag_offset_x = Lcw / 2; drag_offset_y = Lvsp; }
                            begin_drag(s_pt, s_pi, s_pci);
                        }
                    }
                    if (dragging) {
                        drag_x = local_x - drag_offset_x;
                        drag_y = local_y - drag_offset_y;
                        solitaire_redraw();
                    }
                }
                break;

            case EVENT_MOUSE_UP:
                {
                    int cwx, cwy; win_get_pos(window_handle, &cwx, &cwy);
                    int local_x = event.mouse_x - cwx - 2;
                    int local_y = event.mouse_y - cwy - 22;

                    if (dragging) {
                        end_drag(local_x, local_y);
                    } else if (s_pressed) {
                        int pt = s_pt, pi = s_pi, ci = s_pci;
                        int is_dbl = (g_dbl > 0 && pt == g_last_pt && pi == g_last_pi && ci == g_last_ci);
                        if (is_dbl) {
                            g_dbl = 0;
                            card_pile_t *fp = NULL; card_t *cd = NULL;
                            if (pt == PILE_WASTE && waste.count > 0) { fp = &waste; cd = pile_top(&waste); }
                            else if (pt == PILE_TABLEAU && tableau[pi].count > 0 && ci == tableau[pi].count - 1) {
                                card_t *t = pile_top(&tableau[pi]);
                                if (t->face_up) { fp = &tableau[pi]; cd = t; }
                            }
                            if (cd && fp) {
                                try_auto_foundation(cd, fp);
                                if (fp->type == PILE_TABLEAU && fp->count > 0) {
                                    card_t *nt = pile_top(fp); if (!nt->face_up) nt->face_up = true;
                                }
                            }
                            selected_pile_type = -1;
                        } else {
                            if (selected_pile_type >= 0 && pt >= 0) handle_destination_click(pt, pi);
                            else handle_card_click(pt, pi, ci);
                            g_last_pt = pt; g_last_pi = pi; g_last_ci = ci;
                            g_dbl = 4;
                        }
                    }

                    if (s_pressed) {
                        game_snap_t after; snap_save(&after);
                        if (!snap_eq(&s_before, &after)) undo_push(&s_before);
                    }
                    s_pressed = 0;
                    if (check_win()) printf("You win!\n");
                    solitaire_redraw();
                }
                break;

            default:
                break;
        }
    }

    win_destroy(window_handle);
    printf("Solitaire closed\n");
    return 0;
}
