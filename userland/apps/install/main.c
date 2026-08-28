// main.c - MayteraOS "Install to Disk" (#306, restyled #appstyle)
//
// A LINEAR, STEPPED, ONE-TIME, HIGH-STAKES FLOW - which is the same thing the
// first-boot wizard is, so it is now built the same way.
//
// WHAT CHANGED AND WHY (#appstyle). This app previously drew a 640x480 Motif
// dialog from twenty-three hardcoded colour literals, with a comment stating
// that it "intentionally does NOT follow the live system theme ... for
// predictability". Two things were wrong with that:
//
//   1. It predates the contrast primitives. "Predictable" was achievable in
//      2026-01 only by pinning hex, because there was no gui_ensure_contrast()
//      to guarantee a derived colour cleared a floor. There is now, so the
//      choice today is between a themed window that is GUARANTEED readable and
//      a fixed slate-blue one that clashes with all 14 shipped themes.
//   2. The first-boot wizard pins its palette for a REAL reason that does not
//      apply here: it runs before a theme has been chosen. This app runs inside
//      a themed desktop, launched from a themed desktop's menu.
//
// So the split is: the wizard's GEOMETRY, TYPE SCALE, CHROME and INTERACTION
// GRAMMAR are adopted exactly; the PALETTE is derived from the active theme.
// docs/UI_STYLE_GUIDE.md and the published restyle spec carry the tables.
//
// GEOMETRY, mirrored from userland/apps/setup/main.rs (CARD_W/CARD_H/CARD_PAD/
// HDR_H at 1285-1290 there) so the two windows are literally the same object:
//
//     window / card   688 x 616, NOCHROME, shadowed, centred on the framebuffer
//     CARD_PAD 24, HDR_H 48  ->  body origin (24, 72)
//     body canvas     640 x 480; header strip is body y -48..0; footer 480..520
//     step counter    centred, body y -40, size 24, accent
//     page title      (32, 24)  size 20 bold
//     page subtitle   (32, 54)  size 14
//     Back button     (32, 440, 88, 28)
//     primary button  (468, 440, 140, 28)
//     step dots       pitch 24, centred on body x 320, cy 500
//     focus ring      2px accent, drawn inward at (x-4, y-4, w+8, h+8)
//
// THREE DELIBERATE DIVERGENCES FROM THE WIZARD, stated here because each one
// looks like a mistake if you diff the two files:
//
//   NO GLASS. setup/main.rs composites a real software box-blur of the
//   wallpaper behind its card (three separable passes, per-pixel backdrop
//   sampling for every one of ~30 alpha blends, plus unshared copies of the
//   compositor's shadow and gradient constants). That is ~400 lines to
//   replicate in C inside a destructive installer for cosmetic parity. This
//   card is an opaque theme surface with the same radius, edge and shadow. If
//   the glass is ever factored out of setup into libc, this app inherits it.
//
//   THE CIRCLES ARE DRAWN CORRECTLY HERE, WHICH MEANS THEY DO NOT MATCH THE
//   WIZARD'S ARGUMENTS. gui_fill_circle_aa(h, x, y, d, ...) takes a TOP-LEFT
//   and a DIAMETER (gui.c:812 forwards straight to gui_fill_rounded_aa with
//   w=h=d, r=d/2). Every call in setup/main.rs passes a centre and a radius, so
//   its radios, step dots, mask bullets and status discs render as small blobs
//   offset down and right of where the author meant. This file calls it as
//   documented and sizes the dots to match what the wizard VISUALLY produces
//   (7px active, 6px inactive), not what its arguments claim.
//
//   SHARED WIDGETS, EVEN WHERE THE WIZARD HAND-ROLLS. gui_button() and
//   gui_progress() are imported by setup/main.rs and called only from dead
//   code; it hand-rolls both. Copying a documented piece of debt is not
//   "following the design" - the design is the layout and the type. Buttons
//   keep the wizard's rects and get the theme's own rendering. The disk list is
//   a gui_list_t, which is also how its scrollbar became real: the old one was
//   PAINTED, with no drag handler, no page handler, and no EVENT_MOUSE_SCROLL
//   case anywhere in this file.
//
// ENGINE REALITY NOTE (read before touching the progress screen). The kernel
// exposes exactly two syscalls for this: SYS_INST_ENUM (365, non-destructive
// enumeration) and SYS_INST_INSTALL (366, DESTRUCTIVE, root-only).
// installer_do_install_target() in kernel/gui/installer.c reports progress ONLY
// to kprintf (kernel serial log) - there is no syscall, shared memory, or
// callback path back to Ring 3 while the call is in flight, and it BLOCKS the
// calling thread until the clone finishes. So: a background pthread makes the
// single blocking inst_install() call while the main GUI thread keeps pumping
// window events, and the percent/stage shown is a time-estimated approximation
// (scaled off the target's sector count), capped at 95% until the real syscall
// returns. It never claims 100%/"Complete." before the engine has genuinely
// finished, and it never gets stuck: the one number that matters (did the
// install actually succeed) is always the syscall's real return code.

#include "maytera.h"
#include "gui.h"
#include "gui_style.h"
#include "gui_list.h"
#include "theme.h"
#include "pthread.h"

// ---------------------------------------------------------------------------
// Geometry. Body-local unless a name says WIN_.
// ---------------------------------------------------------------------------
#define WIN_W       688
#define WIN_H       616
#define CARD_PAD    24
#define HDR_H       48
#define ORG_X       CARD_PAD
#define ORG_Y       (CARD_PAD + HDR_H)
#define BODY_W      640
#define BODY_H      480
#define FOOT_Y      440
#define BTN_H       28
#define DOT_Y       500
#define DOT_PITCH   24
#define CONTENT_X   32
#define CONTENT_W   576

// Type scale. SIZE UNITS, not CSS px: kernel/gui/ttf.c scales with
// stbtt_ScaleForPixelHeight(), which maps ascent-descent (not the em square)
// onto the number, so one unit is 0.859 em px on the shipped DejaVu Sans and
// TY_BODY renders at em 12.03px. Reading 12 off a mockup ships everything 14%
// small; that has already happened once in this tree.
#define TY_STEP     24
#define TY_HEADING  20
#define TY_TITLE    16
#define TY_BODY     14
#define TY_CAPTION  11

// ---------------------------------------------------------------------------
// Palette, derived once from the active theme.
//
// Every field is walked to a WCAG floor by the shared contrast primitives
// rather than eyeballed: text to 4.5:1 against the surface it lands on,
// boundaries to the 3:1 non-text floor (asked at GUI_AIM_NONTEXT so a theme
// edit or a rounding difference cannot push it back under). That is what buys
// back the "predictability" the old hardcoded palette was reaching for, without
// buying it by ignoring the user's theme.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t card;        // the wizard card's own fill
    uint32_t inner;       // a card-on-card fill (list, target panel, chips)
    uint32_t edge;        // resting boundary on `card`
    uint32_t ink;         // primary text on `card`
    uint32_t dim;         // secondary text on `card`
    uint32_t ink_inner;   // primary text on `inner`
    uint32_t dim_inner;   // secondary text on `inner`
    uint32_t accent;      // step counter, focus, selection, progress fill (on `card`)
    uint32_t accent_inner;// the same role when it lands on `inner`
    uint32_t on_accent;
    uint32_t danger;      // the destructive tokens, on `card`
    uint32_t danger_soft; // a danger-toned fill (pills, callouts)
    uint32_t danger_ink;  // danger text ON danger_soft
    uint32_t success;
    uint32_t disabled;    // disabled ink
    int      radius;      // GUI_RADIUS, clamped
    int      dark;
} pal_t;
static pal_t T;

static uint32_t mix_pct(uint32_t base, uint32_t over, int pct) {
    return gui_mix(base, over, (pct * 255) / 100);
}
static int is_dark(uint32_t c) {
    return (((c >> 16 & 255) * 30 + (c >> 8 & 255) * 59 + (c & 255) * 11) / 100) < 128;
}

static void build_palette(void) {
    // Read the ACTIVE theme's decor family first. Skipping this is how an app
    // ends up rounded and shadowed on top of a beveled retro theme.
    gui_style_sync_from_theme();

    uint32_t wb  = theme_color(THEME_COLOR_WINDOW_BG);
    uint32_t acc = theme_color(THEME_COLOR_ACCENT);
    if (!acc) acc = 0x00336666;
    T.dark = is_dark(wb);

    // Same two seed greys Files uses (fp_content / fp_field), tinted 5-6% toward
    // the accent so the window belongs to the theme rather than merely sitting
    // in front of it.
    T.card  = mix_pct(T.dark ? 0x00262A30 : 0x00F5F6F8, acc, 5);
    T.inner = T.dark ? 0x00333A45 : 0x00FFFFFF;

    T.ink       = gui_ink_on(T.card);
    T.ink_inner = gui_ink_on(T.inner);
    // Dim = 5/8 toward the ink (Files' bias, which exists because a 50/50
    // average measured 3.26:1 on retro_unix), then guaranteed at the text floor.
    {
        uint32_t k = T.ink, b = T.card;
        uint32_t d = ((((k >> 16 & 255) * 5 + (b >> 16 & 255) * 3) / 8) << 16) |
                     ((((k >> 8  & 255) * 5 + (b >> 8  & 255) * 3) / 8) << 8)  |
                      (((k       & 255) * 5 + (b       & 255) * 3) / 8);
        T.dim = gui_ensure_contrast(d, T.card, GUI_FLOOR_TEXT);
        k = T.ink_inner; b = T.inner;
        d = ((((k >> 16 & 255) * 5 + (b >> 16 & 255) * 3) / 8) << 16) |
            ((((k >> 8  & 255) * 5 + (b >> 8  & 255) * 3) / 8) << 8)  |
             (((k       & 255) * 5 + (b       & 255) * 3) / 8);
        T.dim_inner = gui_ensure_contrast(d, T.inner, GUI_FLOOR_TEXT);
    }
    // The accent carries the step counter and the "you are here" dot, which are
    // TEXT and a MARK respectively, on `card`. It has to clear the text floor,
    // and on four shipped themes the raw accent does not.
    // ONE ROLE, TWO SURFACES, TWO COLOURS. A colour walked to a floor is only
    // guaranteed against the background it was walked against, and every field
    // below that appears twice does so because this window has two surfaces.
    // MEASURED on Modern Dark with a single `accent`: the step counter (on
    // `card`) cleared 4.58:1 while the SAME ink on the "WHAT HAPPENS" card
    // measured 3.82:1, and a single `danger` put "CURRENT BOOT DEVICE" at
    // 3.47:1 on its own pill. Both are the identical mistake, one surface apart.
    T.accent       = gui_ensure_contrast(acc, T.card,  GUI_FLOOR_TEXT);
    T.accent_inner = gui_ensure_contrast(acc, T.inner, GUI_FLOOR_TEXT);
    // gui_ink_on() picks black or white off one luminance threshold, which is a
    // guess. On Modern Dark's #0A84FF that guess MEASURED 3.20:1 in the sibling
    // app's selected row. On accent discs here it carries a numeral and a check
    // mark, so walk it to the text floor against the fill it actually lands on.
    T.on_accent = gui_ensure_contrast(gui_ink_on(acc), acc, GUI_FLOOR_TEXT);
    T.edge      = gui_ensure_contrast2(theme_color(THEME_COLOR_WINDOW_BORDER) ?
                                       theme_color(THEME_COLOR_WINDOW_BORDER) : acc,
                                       T.card, T.inner, GUI_AIM_NONTEXT);
    {
        uint32_t e = theme_color(THEME_COLOR_ERROR);   if (!e) e = 0x00CC0000;
        uint32_t g = theme_color(THEME_COLOR_SUCCESS); if (!g) g = 0x00006600;
        T.danger      = gui_ensure_contrast(e, T.card, GUI_FLOOR_TEXT);
        T.danger_soft = mix_pct(T.card, e, T.dark ? 24 : 12);
        T.danger_ink  = gui_ensure_contrast(e, T.danger_soft, GUI_FLOOR_TEXT);
        T.success     = gui_ensure_contrast(g, T.card, GUI_FLOOR_TEXT);
    }
    // Disabled ink lands on the LIST fill (T.inner), not on the card, and it is
    // held to the 3:1 non-text floor rather than 4.5:1 for the same reason
    // gui_button()'s disabled label is: WCAG exempts a disabled control, and
    // lifting it to full text contrast would delete the difference between
    // "you can choose this disk" and "you cannot", which is the entire job of
    // the state on this screen.
    T.disabled = gui_ensure_contrast(mix_pct(T.inner, T.ink, 45), T.inner,
                                     GUI_FLOOR_NONTEXT);

    T.radius = GUI_RADIUS;
    if (T.radius < 0)  T.radius = 0;
    if (T.radius > 16) T.radius = 16;

    gui_palette_t p;
    p.surface = T.card; p.surface_raised = T.inner;
    p.ink = T.ink; p.ink_dim = T.dim;
    p.accent = acc; p.accent_hover = gui_lighten(acc, 24);
    p.border = T.edge; p.field_bg = T.inner; p.field_border = T.edge;
    p.track = mix_pct(T.card, acc, 20);
    gui_set_palette(&p);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
enum { SCR_INTRO = 0, SCR_DISKS, SCR_CONFIRM, SCR_PROGRESS, SCR_DONE };

// Minimum install size floor used ONLY to decide whether a disk is shown as
// "too small" in the UI (a UI-side pre-filter, matching the spec's "under the
// minimum size is not offered as selectable but is still shown"). It is
// deliberately conservative; the AUTHORITATIVE check is the kernel's own
// ESP+ext2-root arithmetic inside installer_do_install_target(), which
// returns -6 ("too small") if a disk that passed this floor still does not
// fit. Two independent checks, same as the kernel's own comment about its
// two floors, kept separate on purpose.
#define MIN_INSTALL_SECTORS (4ULL * 1024 * 1024 * 1024 / 512)   // 4 GiB

// ---------------------------------------------------------------------------
// Disk row model (derived from inst_target_t at enumeration time)
// ---------------------------------------------------------------------------
typedef struct {
    inst_target_t t;
    char   name[40];       // synthesized bus label, e.g. "AHCI disk 0" - also
                            // the exact string typed to confirm on screen 3
    char   subtitle[64];    // ATA IDENTIFY model, or "<BUS> - <capacity>"
    char   capstr[16];      // "500.1 GB"
    int    have_model;      // subtitle is a real IDENTIFY model, not a bus/capacity fallback
    int    is_boot;
    int    too_small;
    int    selectable;
} disk_row_t;

static disk_row_t g_rows[INST_MAX_TARGETS];
static int        g_nrows = 0;
static int        g_selected = -1;   // index into g_rows, or -1

static int g_win = -1;
static int g_screen = SCR_INTRO;
static int g_running = 1;

// Per-screen keyboard focus (small int, meaning is screen-specific; see the
// focus tables in handle_key()/handle_click()).
static int g_focus = 0;
static int g_hover_row = -1;   // hovered disk-list row (mouse), or -1

// Screen 2 disk list. gui_list_t owns the geometry AND the input, which is how
// the scrollbar stopped being decoration: the previous code painted a thumb
// with gui_draw_scrollbar_v() and had no drag handler, no page handler, and no
// EVENT_MOUSE_SCROLL case anywhere in the file, so with more than four disks
// the list was keyboard-only and the scrollbar was a picture of a control.
static gui_list_t g_list;
#define ROW_H       76
#define LIST_X      (CONTENT_X)
#define LIST_Y      92
#define LIST_W      (CONTENT_W)
#define LIST_H      306          // 1 + 4*76 + 1

// Screen 3 confirm field
static textfield_t g_tf;
static char        g_tf_buf[64];
static int         g_tf_match = 0;

// Screen 4/5 install state
typedef struct { uint8_t kind; uint8_t index; } install_args_t;
static install_args_t   g_install_args;
static volatile int     g_install_rc = -12345;   // sentinel: not finished
static unsigned long    g_install_start_ms = 0;
static unsigned long    g_install_done_ms = 0;
static int              g_progress_pct = 1;
static char             g_progress_msg[64] = "Writing partition table... (1 of 5)";
static int              g_done_success = 0;
static char             g_done_errmsg[96];
static int              g_confirm_row = -1;      // which row we are installing

// ---------------------------------------------------------------------------
// Small formatting helpers
// ---------------------------------------------------------------------------
static void commafmt_u64(unsigned long long n, char *out, int cap) {
    char digits[24]; int nd = 0;
    if (n == 0) { digits[nd++] = '0'; }
    while (n > 0 && nd < 24) { digits[nd++] = (char)('0' + (n % 10)); n /= 10; }
    int oi = 0;
    for (int i = nd - 1; i >= 0; i--) {
        if (oi < cap - 1) out[oi++] = digits[i];
        int from_end = i; // digits before this one still to emit
        if (from_end > 0 && (from_end % 3) == 0 && oi < cap - 1) out[oi++] = ',';
    }
    out[oi] = '\0';
}

static void format_gb(unsigned long long sectors, char *out, int cap) {
    double gb = (double)sectors * 512.0 / 1000000000.0;
    int whole = (int)gb;
    int tenth = (int)((gb - (double)whole) * 10.0 + 0.5);
    if (tenth >= 10) { tenth = 0; whole++; }
    snprintf(out, cap, "%d.%d GB", whole, tenth);
}

static const char *bus_word(int kind) {
    if (kind == INST_KIND_ATA)  return "ATA";
    if (kind == INST_KIND_AHCI) return "AHCI";
    if (kind == INST_KIND_USB)  return "USB";
    return "disk";
}

// Word-wrap. gui_wrap_text_ttf() is the SHARED greedy wrapper (it measures with
// the real glyph metrics, hard-breaks a word wider than the box so it can never
// escape, and ellipsizes what is left once max_lines is reached). The private
// 24-line implementation that used to live here had two dead locals and a
// no-op self-assignment; deleting it is the point of having a shared one.
#define WRAP_LINE_MAX GUI_WRAP_COL

// ---------------------------------------------------------------------------
// Text. Face 0 = the system default DejaVu Sans; the kernel renderer resolves
// FONT_STYLE_BOLD to the REAL enrolled DejaVu Sans Bold face rather than
// synthesising it, so bold here is an outline, not a smear.
//
// EVERY x/y BELOW IS BODY-LOCAL. The (ORG_X, ORG_Y) translation is applied
// exactly ONCE, here, the same discipline setup/main.rs uses (body_to_win at
// its line 1410). Applying it at the call sites instead is how a page ends up
// shifted by (24, 72) in one place and not another.
// ---------------------------------------------------------------------------
static void ttext(int x, int y, const char *s, uint32_t color, int size, int bold) {
    win_draw_text_ttf_ex(g_win, ORG_X + x, ORG_Y + y, s, 0, size,
                         bold ? FONT_STYLE_BOLD : 0, color);
}
// Bold measures ONE PIXEL PER GLYPH wider than gui_ttf_width() reports, because
// the renderer's bold is applied after measurement. Centring or right-aligning
// a bold run on the unadjusted width overflows the box; this is the one place
// that correction lives.
static int tw_of(const char *s, int size, int bold) {
    int w = gui_ttf_width(s, size);
    if (bold) { int n = 0; while (s[n]) n++; w += n; }
    return w;
}
static void ttext_centered(int x, int y, int w, const char *s, uint32_t color, int size, int bold) {
    int tx = x + (w - tw_of(s, size, bold)) / 2; if (tx < x) tx = x;
    ttext(tx, y, s, color, size, bold);
}
static void ttext_right(int x, int y, int w, const char *s, uint32_t color, int size, int bold) {
    int tx = x + w - tw_of(s, size, bold); if (tx < x) tx = x;
    ttext(tx, y, s, color, size, bold);
}
static void bfill(int x, int y, int w, int h, uint32_t c) {
    win_draw_rect(g_win, ORG_X + x, ORG_Y + y, w, h, c);
}
// Inward frame: four rects, entirely inside (x,y,w,h). An outward border on a
// card edge is what produces the stray-pixel fringe the style engine spent #612
// removing.
static void bframe(int x, int y, int w, int h, int t, uint32_t c) {
    for (int i = 0; i < t; i++) {
        bfill(x + i, y + i, w - 2 * i, 1, c);
        bfill(x + i, y + h - 1 - i, w - 2 * i, 1, c);
        bfill(x + i, y + i, 1, h - 2 * i, c);
        bfill(x + w - 1 - i, y + i, 1, h - 2 * i, c);
    }
}
static void bround(int x, int y, int w, int h, int r, uint32_t fill, uint32_t bg) {
    gui_fill_rounded_aa(g_win, ORG_X + x, ORG_Y + y, w, h, r, fill, bg);
}
// (cx, cy) is the CENTRE. gui_fill_circle_aa() takes a TOP-LEFT and a DIAMETER
// (gui.c:812); setup/main.rs passes centre+radius to it and therefore draws
// every one of its circles offset down-right of where it meant to. This wrapper
// is why that bug cannot be reproduced here.
static void bcircle(int cx, int cy, int d, uint32_t fill, uint32_t bg) {
    gui_fill_circle_aa(g_win, ORG_X + cx - d / 2, ORG_Y + cy - d / 2, d, fill, bg);
}
static void bring(int cx, int cy, int d, int t, uint32_t col, uint32_t bg) {
    bcircle(cx, cy, d, col, bg);
    bcircle(cx, cy, d - 2 * t, bg, col);
}
// The wizard's focus ring, verbatim: 2px accent, 2px clear of the control.
static void focus_ring(int x, int y, int w, int h) {
    bframe(x - 4, y - 4, w + 8, h + 8, 2, T.accent);
}
// A drawn check mark, centred on (cx, cy). The shipped face has no check glyph
// and "OK" set at TY_CAPTION overflows a 14px disc, which is what the first
// build of this screen shipped: two letters spilling past the circle they were
// meant to sit inside. gui_thick_line() is the shared stroke primitive.
static void check_mark(int cx, int cy, int d, uint32_t col) {
    int a = d / 4;                      // arm scale
    gui_thick_line(g_win, ORG_X + cx - a - 1, ORG_Y + cy,
                          ORG_X + cx - 1,     ORG_Y + cy + a,     d > 20 ? 4 : 2, col);
    gui_thick_line(g_win, ORG_X + cx - 1,     ORG_Y + cy + a,
                          ORG_X + cx + a + 1, ORG_Y + cy - a,     d > 20 ? 4 : 2, col);
}

static int pt_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}
// Window-local -> body-local, applied once, in the event loop.
static int b_x(int wx) { return wx - ORG_X; }
static int b_y(int wy) { return wy - ORG_Y; }

// ---------------------------------------------------------------------------
// Card + shared chrome
// ---------------------------------------------------------------------------
// Which of the five screens carries a step counter and a dot. The Applying and
// Done pages carry NEITHER, matching setup/main.rs (PG_APPLY and PG_DONE are
// absent from its STEP_PAGES table): once the destructive work has started,
// "step 4 of 5" invites a user to think there is a step 5 they could still get
// to, and there is not.
#define NSTEPS 3
static int step_of(int screen) {
    switch (screen) {
        case SCR_INTRO:   return 1;
        case SCR_DISKS:   return 2;
        case SCR_CONFIRM: return 3;
        default:          return 0;    // no counter, no dots
    }
}

static void draw_card(void) {
    // The card IS the window (NOCHROME), so the whole surface is ours.
    if (T.radius > 0) {
        // The AA edge blends toward what is BEHIND the window. There is no
        // framebuffer read-back in this renderer, so the honest answer is the
        // card's own colour: a rounded card on an unknown desktop cannot know
        // its backdrop, and guessing a light grey is how the App Store got a
        // white fringe on a dark theme (#612).
        gui_fill_rounded_aa(g_win, 0, 0, WIN_W, WIN_H, T.radius, T.card, T.card);
        gui_rounded_border(g_win, 0, 0, WIN_W, WIN_H, T.radius, T.edge);
    } else {
        win_draw_rect(g_win, 0, 0, WIN_W, WIN_H, T.card);
        uint32_t sh, hi;
        gui_bevel_pair(T.card, &sh, &hi);
        win_draw_rect(g_win, 0, 0, WIN_W, 1, hi);
        win_draw_rect(g_win, 0, 0, 1, WIN_H, hi);
        win_draw_rect(g_win, 0, WIN_H - 1, WIN_W, 1, sh);
        win_draw_rect(g_win, WIN_W - 1, 0, 1, WIN_H, sh);
    }
}

static void draw_dots(int step) {
    int span = (NSTEPS - 1) * DOT_PITCH;
    int x0 = BODY_W / 2 - span / 2;
    uint32_t off = mix_pct(T.card, T.ink, 40);
    for (int k = 0; k < NSTEPS; k++) {
        int cx = x0 + k * DOT_PITCH;
        if (k == step - 1) bcircle(cx, DOT_Y, 7, T.accent, T.card);
        else               bcircle(cx, DOT_Y, 6, off,      T.card);
    }
}

// title/subtitle pair + counter + dots. subtitle may be NULL.
static void draw_head(const char *title, const char *subtitle) {
    int step = step_of(g_screen);
    if (step) {
        char buf[32];
        snprintf(buf, sizeof buf, "Step %d of %d", step, NSTEPS);
        ttext_centered(0, -40, BODY_W, buf, T.accent, TY_STEP, 0);
        draw_dots(step);
    }
    ttext(CONTENT_X, 24, title, T.ink, TY_HEADING, 1);
    if (subtitle) {
        char lines[2][WRAP_LINE_MAX];
        int n = gui_wrap_text_ttf(subtitle, TY_BODY, CONTENT_W, 2, lines);
        for (int i = 0; i < n; i++)
            ttext(CONTENT_X, 54 + i * 20, lines[i], T.dim, TY_BODY, 0);
    }
}

// Footer. `back` is the left control (NULL hides it); `act` the right one.
// Both rects are the wizard's, and BOTH the draw and the hit-test read them
// from these two macros, so they cannot disagree.
#define BACK_X 32
#define BACK_W 88
static int g_act_x = 468, g_act_w = 140;
static void draw_footer(const char *back, const char *act,
                        gui_btn_variant_t act_var, int act_enabled,
                        int act_x, int act_w) {
    g_act_x = act_x; g_act_w = act_w;
    if (back) {
        gui_button(g_win, ORG_X + BACK_X, ORG_Y + FOOT_Y, BACK_W, BTN_H, back,
                   GUI_BTN_SECONDARY, g_focus == 1 ? GUI_ST_FOCUS : GUI_ST_NORMAL);
        if (g_focus == 1) focus_ring(BACK_X, FOOT_Y, BACK_W, BTN_H);
    }
    if (act) {
        gui_button(g_win, ORG_X + act_x, ORG_Y + FOOT_Y, act_w, BTN_H, act, act_var,
                   !act_enabled ? GUI_ST_DISABLED
                                : (g_focus == 2 ? GUI_ST_FOCUS : GUI_ST_NORMAL));
        if (g_focus == 2 && act_enabled) focus_ring(act_x, FOOT_Y, act_w, BTN_H);
    }
}

// ---------------------------------------------------------------------------
// Disk enumeration + classification (section 3, 6.1)
// ---------------------------------------------------------------------------
static void classify_row(disk_row_t *r) {
    inst_target_t *t = &r->t;
    if (t->kind == INST_KIND_ATA)  snprintf(r->name, sizeof(r->name), "ATA channel %d", t->index);
    else if (t->kind == INST_KIND_AHCI) snprintf(r->name, sizeof(r->name), "AHCI disk %d", t->index);
    else if (t->kind == INST_KIND_USB)  snprintf(r->name, sizeof(r->name), "USB disk %d", t->index);
    else snprintf(r->name, sizeof(r->name), "disk %d", t->index);

    format_gb(t->sectors, r->capstr, sizeof(r->capstr));

    int have_model = 0;
    if (t->kind == INST_KIND_ATA && t->index <= 3) {
        disk_info_t di;
        if (get_disk_info((int)t->index, &di) == 0 && di.present && di.type == 0 && di.model[0]) {
            // Trim trailing spaces (IDENTIFY model strings are space-padded).
            char tmp[41]; int n = 0;
            for (; n < 40 && di.model[n]; n++) tmp[n] = di.model[n];
            while (n > 0 && tmp[n - 1] == ' ') n--;
            tmp[n] = '\0';
            if (n > 0) { strncpy(r->subtitle, tmp, sizeof(r->subtitle) - 1); r->subtitle[sizeof(r->subtitle)-1]=0; have_model = 1; }
        }
    }
    if (!have_model) snprintf(r->subtitle, sizeof(r->subtitle), "%s - %s", bus_word(t->kind), r->capstr);
    r->have_model = have_model;

    r->is_boot   = t->is_boot ? 1 : 0;
    r->too_small = (t->sectors < MIN_INSTALL_SECTORS) ? 1 : 0;
    r->selectable = !r->is_boot && !r->too_small;
}

static void enumerate_disks(void) {
    inst_target_t raw[INST_MAX_TARGETS];
    int n = inst_enum(raw, INST_MAX_TARGETS);
    g_nrows = 0;
    if (n < 0) n = 0;
    for (int i = 0; i < n && i < INST_MAX_TARGETS; i++) {
        g_rows[g_nrows].t = raw[i];
        classify_row(&g_rows[g_nrows]);
        g_nrows++;
    }
    g_selected = -1;
    gui_list_config(&g_list, ORG_X + LIST_X, ORG_Y + LIST_Y, LIST_W, LIST_H, ROW_H, g_nrows);
}

// ---------------------------------------------------------------------------
// Screen 1: Before you begin
// ---------------------------------------------------------------------------
static void draw_intro(void) {
    draw_card();
    draw_head("Before you begin",
              "This copies MayteraOS onto an internal disk so this computer can start on "
              "its own, without the USB stick.");

    // What is about to happen, as three consequences rather than two
    // paragraphs. A numbered list is honest here: this IS an ordered sequence,
    // which is the only thing that licenses numbered markers.
    int cx = CONTENT_X, cy = 112, cw = CONTENT_W, ch = 156;
    bround(cx, cy, cw, ch, T.radius, T.inner, T.card);
    bframe(cx, cy, cw, ch, 1, T.edge);
    ttext(cx + 16, cy + 14, "WHAT HAPPENS", T.accent_inner, TY_CAPTION, 1);

    static const char *steps[3] = {
        "You choose a destination disk. The disk MayteraOS is running from is listed "
        "but can never be chosen.",
        "That disk is erased: its partition table and every file on it are replaced.",
        "MayteraOS is copied onto it and made bootable."
    };
    int iy = cy + 40;
    for (int i = 0; i < 3; i++) {
        bcircle(cx + 24, iy + 7, 16, T.accent, T.inner);
        char n[4]; snprintf(n, sizeof n, "%d", i + 1);
        // Centre on the DISC's centre, and on the type's own line box, not on a
        // guessed offset: TY_CAPTION's ink is TY_CAPTION tall by definition.
        ttext_centered(cx + 16, iy + 7 - TY_CAPTION / 2, 16, n, T.on_accent, TY_CAPTION, 1);
        char lines[2][WRAP_LINE_MAX];
        int nl = gui_wrap_text_ttf(steps[i], TY_BODY, cw - 60, 2, lines);
        for (int k = 0; k < nl; k++)
            ttext(cx + 44, iy + k * 18, lines[k], T.ink_inner, TY_BODY, 0);
        iy += (nl > 1) ? 40 : 34;
    }

    // The one thing a nervous user needs, as a callout rather than a footnote.
    int wy = 292;
    bround(CONTENT_X, wy, CONTENT_W, 46, T.radius, T.danger_soft, T.card);
    bframe(CONTENT_X, wy, CONTENT_W, 46, 1, T.danger);
    bcircle(CONTENT_X + 20, wy + 23, 14, T.danger, T.danger_soft);
    ttext_centered(CONTENT_X + 13, wy + 17, 14, "!", T.on_accent, TY_CAPTION, 1);
    ttext(CONTENT_X + 40, wy + 16,
          "Nothing is written to any disk until you confirm on step 3.",
          gui_ensure_contrast(T.ink, T.danger_soft, GUI_FLOOR_TEXT), TY_BODY, 0);

    ttext(CONTENT_X, 356, "Installing requires the root account.", T.dim, TY_CAPTION, 0);
    draw_footer("Quit", "Continue", GUI_BTN_PRIMARY, 1, 468, 140);
}

// ---------------------------------------------------------------------------
// Screen 2: Choose a disk
// ---------------------------------------------------------------------------
static void draw_row(int ry, disk_row_t *r, int idx, int rw) {
    int selected = (idx == g_selected);
    int hover    = (idx == g_hover_row) && r->selectable && !selected;
    int dis      = r->is_boot || r->too_small;

    if (selected) {
        // A 12% accent wash plus a 3px rail, NOT a solid accent fill. A solid
        // fill puts the row's secondary line (the model string) on the accent,
        // where it measured unreadable on four shipped themes; the wash keeps
        // both lines on a near-surface ground and lets the rail carry the
        // "this one" signal.
        bfill(LIST_X + 1, ry, rw, ROW_H, mix_pct(T.inner, T.accent, 12));
        bfill(LIST_X + 1, ry, 3, ROW_H, T.accent);
    } else if (hover) {
        bfill(LIST_X + 1, ry, rw, ROW_H, mix_pct(T.inner, T.ink, 6));
    }

    uint32_t ink  = dis ? T.disabled : T.ink_inner;
    uint32_t dim  = dis ? T.disabled : T.dim_inner;
    uint32_t bgc  = selected ? mix_pct(T.inner, T.accent, 12)
                  : hover    ? mix_pct(T.inner, T.ink, 6) : T.inner;

    int rcx = LIST_X + 31, rcy = ry + 38;
    if (selected)      { bring(rcx, rcy, 16, 2, T.accent, bgc); bcircle(rcx, rcy, 8, T.accent, bgc); }
    else if (dis)      { bring(rcx, rcy, 16, 2, T.disabled, bgc); }
    else               { bring(rcx, rcy, 16, 2, T.edge, bgc); }

    ttext(LIST_X + 56, ry + 16, r->name, ink, TY_TITLE, 1);
    ttext(LIST_X + 56, ry + 40, r->subtitle, dim, TY_CAPTION, 0);
    // Capacities right-align so digit places stack (style guide 4.5).
    ttext_right(LIST_X, ry + 16, rw - 16, r->capstr, ink, TY_TITLE, selected);

    // An ineligible row states WHY as a labelled pill, not as the only
    // full-strength black sentence on an otherwise greyed row, which is what
    // the old layout did and which made the warning the loudest thing in a
    // control the user cannot use.
    if (r->is_boot || r->too_small) {
        const char *msg = r->is_boot ? "CURRENT BOOT DEVICE" : "TOO SMALL (needs 4 GB)";
        uint32_t pf = r->is_boot ? T.danger_soft : mix_pct(T.inner, T.ink, 10);
        uint32_t pt = r->is_boot ? T.danger_ink   : T.disabled;
        int pw = tw_of(msg, TY_CAPTION, 1) + 12;
        bround(LIST_X + 56, ry + 54, pw, 16, 3, pf, bgc);
        ttext(LIST_X + 62, ry + 56, msg, pt, TY_CAPTION, 1);
    }
}

static void draw_disks(void) {
    draw_card();
    draw_head("Choose a disk",
              "Everything on the disk you choose will be erased.");

    gui_list_config(&g_list, ORG_X + LIST_X, ORG_Y + LIST_Y, LIST_W, LIST_H,
                    ROW_H, g_nrows);

    bround(LIST_X, LIST_Y, LIST_W, LIST_H, T.radius, T.inner, T.card);
    bframe(LIST_X, LIST_Y, LIST_W, LIST_H, 1, T.edge);

    int first = gui_list_first(&g_list), span = gui_list_span(&g_list);
    int rw = gui_list_row_w(&g_list);
    for (int i = first; i < first + span && i < g_nrows; i++) {
        int wry;
        if (!gui_list_row_y(&g_list, i, &wry)) continue;   // never draw off-box
        int ry = b_y(wry);
        draw_row(ry, &g_rows[i], i, rw);
        if (i + 1 < g_nrows) bfill(LIST_X + 1, ry + ROW_H, rw, 1, T.edge);
    }
    if (g_nrows == 0)
        ttext_centered(LIST_X, LIST_Y + LIST_H / 2 - 8, LIST_W,
                       "No disks were found.", T.dim_inner, TY_BODY, 0);
    // The scrollbar is gui_scroll's, drawn by gui_list, and therefore real.
    gui_scroll_draw_on(g_win, &g_list.scroll, T.inner);

    if (g_focus == 0) focus_ring(LIST_X, LIST_Y, LIST_W, LIST_H);

    ttext(CONTENT_X, 408, "Up and Down choose a disk.    Enter continues.",
          T.dim, TY_CAPTION, 0);
    draw_footer("Back", "Continue", GUI_BTN_PRIMARY, g_selected >= 0, 468, 140);
}

// ---------------------------------------------------------------------------
// Screen 3: Confirm
// ---------------------------------------------------------------------------
#define TF_X 244
#define TF_Y 314
#define TF_W 240
#define TF_H 28

static void draw_confirm(void) {
    draw_card();
    draw_head("Confirm the disk to erase",
              "This is the last step before anything is written to the disk.");

    disk_row_t *r = &g_rows[g_confirm_row];
    int cx = CONTENT_X, cy = 92, cw = CONTENT_W, ch = 108;
    bround(cx, cy, cw, ch, T.radius, T.inner, T.card);
    bframe(cx, cy, cw, ch, 2, T.danger);
    ttext(cx + 16, cy + 16, r->name, T.ink_inner, TY_HEADING, 1);
    ttext(cx + 16, cy + 46, r->subtitle, T.dim_inner, TY_CAPTION, 0);
    char capline[80], sectstr[24];
    commafmt_u64(r->t.sectors, sectstr, sizeof sectstr);
    snprintf(capline, sizeof capline, "%s   (%s sectors)", r->capstr, sectstr);
    ttext(cx + 16, cy + 66, capline, T.dim_inner, TY_CAPTION, 0);
    {
        const char *pill = "WILL BE ERASED";
        int pw = tw_of(pill, TY_CAPTION, 1) + 16;
        bround(cx + cw - 16 - pw, cy + 14, pw, 20, 3, T.danger_soft, T.inner);
        ttext(cx + cw - 8 - pw, cy + 17, pill, T.danger_ink, TY_CAPTION, 1);
    }

    static const char *bl[3] = {
        "All partitions and files on this disk are permanently erased.",
        "This cannot be undone once installation begins.",
        "The USB stick you are running from is not touched."
    };
    for (int i = 0; i < 3; i++) {
        bcircle(CONTENT_X + 6, 216 + i * 22 + 7, 5, i < 2 ? T.danger : T.dim, T.card);
        ttext(CONTENT_X + 20, 216 + i * 22, bl[i], i < 2 ? T.ink : T.dim, TY_BODY, 0);
    }

    ttext(CONTENT_X, 296, "TYPE THE DISK NAME TO CONFIRM", T.accent, TY_CAPTION, 1);
    bround(CONTENT_X, 314, 196, TF_H, T.radius ? 4 : 0, mix_pct(T.card, T.ink, 7), T.card);
    bframe(CONTENT_X, 314, 196, TF_H, 1, T.edge);
    ttext(CONTENT_X + 10, 314 + (TF_H - TY_BODY) / 2, r->name, T.ink, TY_BODY, 1);

    // The SHARED caret-aware TTF field: caret, selection, clipboard and undo,
    // and a caret measured with the width function that agrees with the
    // renderer drawing the glyphs beside it.
    gui_textfield_tf(g_win, ORG_X + TF_X, ORG_Y + TF_Y, TF_W, TF_H,
                     g_tf_buf, g_tf.len, g_tf.cursor, g_tf.sel_anchor,
                     g_focus == 0, "type it here");
    if (g_focus == 0) focus_ring(TF_X, TF_Y, TF_W, TF_H);

    if (g_tf_match) {
        bcircle(CONTENT_X + 508, 328, 16, T.success, T.card);
        check_mark(CONTENT_X + 508, 328, 16, gui_ink_on(T.success));
    }
    ttext(CONTENT_X, 352,
          g_tf_match ? "Match confirmed. The button below is now enabled."
                     : "The button below stays disabled until this matches exactly.",
          g_tf_match ? T.success : T.dim, TY_CAPTION, 0);

    draw_footer("Back", "Erase and install", GUI_BTN_DANGER, g_tf_match, 388, 220);
}

// ---------------------------------------------------------------------------
// Screen 4: Installing.  No step counter, no dots (see step_of()).
// ---------------------------------------------------------------------------
static const char *g_stage_name[5] = {
    "Writing the partition table",
    "Copying the boot partition",
    "Copying system files",
    "Flushing disk caches",
    "Verifying the copy"
};
static int stage_of_pct(int pct) {
    if (pct < 15) return 0;
    if (pct < 35) return 1;
    if (pct < 80) return 2;
    if (pct < 95) return 3;
    return 4;
}

static void draw_progress(void) {
    draw_card();
    ttext_centered(0, 92, BODY_W, "PLEASE WAIT", T.accent, TY_CAPTION, 1);
    ttext_centered(0, 114, BODY_W, "Installing MayteraOS", T.ink, TY_HEADING, 1);

    disk_row_t *r = &g_rows[g_confirm_row];
    char tgt[144];
    if (r->have_model) snprintf(tgt, sizeof tgt, "%s - %s (%s)", r->name, r->subtitle, r->capstr);
    else               snprintf(tgt, sizeof tgt, "%s - %s", r->name, r->subtitle);
    ttext_centered(0, 148, BODY_W, tgt, T.dim, TY_CAPTION, 0);

    gui_progress(g_win, ORG_X + 170, ORG_Y + 188, 300, 8, g_progress_pct);
    char pct[8]; snprintf(pct, sizeof pct, "%d%%", g_progress_pct);
    ttext_centered(0, 206, BODY_W, pct, T.dim, TY_CAPTION, 0);

    int cur = stage_of_pct(g_progress_pct);
    for (int i = 0; i < 5; i++) {
        int y = 244 + i * 26;
        if (i < cur)       { bcircle(206, y + 7, 16, T.accent, T.card);
                             check_mark(206, y + 7, 16, T.on_accent); }
        else if (i == cur) { bring(206, y + 7, 14, 2, T.accent, T.card);
                             bcircle(206, y + 7, 6, T.accent, T.card); }
        else               { bring(206, y + 7, 14, 2, T.edge, T.card); }
        ttext(230, y, g_stage_name[i],
              i > cur ? T.dim : T.ink, TY_BODY, i == cur);
    }

    ttext_centered(0, 416, BODY_W,
        "Do not power off the computer or remove the installation media.",
        T.danger, TY_CAPTION, 1);
}

// ---------------------------------------------------------------------------
// Screen 5: Done.  No step counter, no dots.
// ---------------------------------------------------------------------------
static void draw_done(void) {
    draw_card();
    uint32_t col = g_done_success ? T.success : T.danger;
    bcircle(320, 128, 64, col, T.card);
    // The mark is drawn, not typed: there is no reliable check glyph in the
    // shipped face and a "v" would read as a letter at this size.
    if (g_done_success) {
        gui_thick_line(g_win, ORG_X + 306, ORG_Y + 130, ORG_X + 316, ORG_Y + 142, 4, T.on_accent);
        gui_thick_line(g_win, ORG_X + 316, ORG_Y + 142, ORG_X + 338, ORG_Y + 112, 4, T.on_accent);
    } else {
        gui_thick_line(g_win, ORG_X + 306, ORG_Y + 114, ORG_X + 334, ORG_Y + 142, 4, T.on_accent);
        gui_thick_line(g_win, ORG_X + 306, ORG_Y + 142, ORG_X + 334, ORG_Y + 114, 4, T.on_accent);
    }

    ttext_centered(0, 190, BODY_W,
        g_done_success ? "Installation complete" : "Installation failed",
        T.ink, TY_HEADING, 1);

    disk_row_t *r = &g_rows[g_confirm_row];
    if (g_done_success) {
        char l1[112];
        snprintf(l1, sizeof l1, "MayteraOS is installed on %s.", r->name);
        ttext_centered(0, 228, BODY_W, l1, T.dim, TY_BODY, 0);
        ttext_centered(0, 250, BODY_W,
            "Remove the USB stick and restart to boot from the internal disk.",
            T.dim, TY_BODY, 0);
        unsigned long total_s = (g_install_done_ms - g_install_start_ms) / 1000;
        unsigned long mm = total_s / 60, ss = total_s % 60;
        char fin[96];
        snprintf(fin, sizeof fin, "Finished in %lu minute%s %lu second%s.",
                 mm, mm == 1 ? "" : "s", ss, ss == 1 ? "" : "s");
        ttext_centered(0, 288, BODY_W, fin, T.dim, TY_CAPTION, 0);
    } else {
        char lines[3][WRAP_LINE_MAX];
        int nl = gui_wrap_text_ttf(
            "The disk may be left in an incomplete state. Choose a different disk, "
            "or check this one, and try again.", TY_BODY, 480, 3, lines);
        for (int i = 0; i < nl; i++)
            ttext_centered(80, 228 + i * 20, 480, lines[i], T.dim, TY_BODY, 0);
        int ew = tw_of(g_done_errmsg, TY_CAPTION, 0) + 24;
        if (ew > 480) ew = 480;
        bround(320 - ew / 2, 296, ew, 24, 3, T.danger_soft, T.card);
        ttext_centered(320 - ew / 2, 302, ew, g_done_errmsg, T.danger_ink, TY_CAPTION, 0);
    }

    draw_footer("Close", g_done_success ? "Restart now" : "Try again",
                GUI_BTN_PRIMARY, 1, 468, 140);
}

static void progress_tick(void) {
    int rc = g_install_rc;
    if (rc != -12345) {
        g_install_done_ms = uptime_ms();
        g_done_success = (rc >= 0);
        if (!g_done_success) {
            static const struct { int code; const char *msg; } table[] = {
                {-1,  "no source filesystem mounted"},
                {-2,  "no install target given"},
                {-3,  "refusing to install onto the source/boot disk"},
                {-4,  "target disk reports zero capacity"},
                {-5,  "source partition size unknown"},
                {-6,  "target disk is too small for the ESP + root partitions"},
                {-7,  "out of memory"},
                {-8,  "failed to write primary GPT header"},
                {-9,  "failed to write GPT partition array"},
                {-10, "failed to write backup GPT array"},
                {-11, "failed to write backup GPT header"},
                {-12, "failed to write protective MBR"},
                {-13, "read from source disk failed"},
                {-14, "write to target disk failed"},
                {-15, "could not locate the ext2 root partition to clone"},
            };
            const char *msg = "installation failed (unknown error)";
            for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++)
                if (table[i].code == rc) { msg = table[i].msg; break; }
            snprintf(g_done_errmsg, sizeof(g_done_errmsg), "Error: %s", msg);
        }
        g_screen = SCR_DONE;
        g_focus = 1;
        return;
    }

    unsigned long elapsed = uptime_ms() - g_install_start_ms;
    unsigned long long secs = g_rows[g_confirm_row].t.sectors;
    unsigned long assumed = (unsigned long)(secs / 2000ULL);
    if (assumed < 3000) assumed = 3000;
    if (assumed > 60000) assumed = 60000;
    int pct = (int)(elapsed * 100 / assumed);
    if (pct > 95) pct = 95;
    if (pct < 1) pct = 1;
    g_progress_pct = pct;

    if (pct < 15)      snprintf(g_progress_msg, sizeof(g_progress_msg), "Writing partition table... (1 of 5)");
    else if (pct < 35) snprintf(g_progress_msg, sizeof(g_progress_msg), "Copying boot partition... (2 of 5)");
    else if (pct < 80) snprintf(g_progress_msg, sizeof(g_progress_msg), "Copying system files... (3 of 5)");
    else               snprintf(g_progress_msg, sizeof(g_progress_msg), "Flushing disk caches... (4 of 5)");
}

// ---------------------------------------------------------------------------
// Redraw dispatch
// ---------------------------------------------------------------------------
static void redraw(void) {
    switch (g_screen) {
        case SCR_INTRO:    draw_intro();    break;
        case SCR_DISKS:    draw_disks();    break;
        case SCR_CONFIRM:  draw_confirm();  break;
        case SCR_PROGRESS: draw_progress(); break;
        case SCR_DONE:     draw_done();     break;
    }
    win_invalidate(g_win);
}

// ---------------------------------------------------------------------------
// Screen transitions
// ---------------------------------------------------------------------------
static void goto_intro(void)   { g_screen = SCR_INTRO;  g_focus = 1; redraw(); }
static void goto_disks(void)   { enumerate_disks(); g_screen = SCR_DISKS; g_focus = 0; g_hover_row = -1; redraw(); }
static void goto_confirm(void) {
    if (g_selected < 0) return;
    g_confirm_row = g_selected;
    g_screen = SCR_CONFIRM;
    g_focus = 0;
    g_tf_buf[0] = '\0';
    tf_init(&g_tf, g_tf_buf, sizeof(g_tf_buf));
    g_tf_match = 0;
    redraw();
}

static void *install_thread_fn(void *arg) {
    install_args_t *a = (install_args_t *)arg;
    int rc = inst_install(a->kind, a->index);
    g_install_rc = rc;
    return 0;
}

static void start_install(void) {
    g_install_args.kind = g_rows[g_confirm_row].t.kind;
    g_install_args.index = g_rows[g_confirm_row].t.index;
    g_install_rc = -12345;
    g_install_start_ms = uptime_ms();
    g_progress_pct = 1;
    snprintf(g_progress_msg, sizeof(g_progress_msg), "Writing partition table... (1 of 5)");
    g_screen = SCR_PROGRESS;
    redraw();

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 262144);
    pthread_t th;
    if (pthread_create(&th, &at, install_thread_fn, &g_install_args) == 0) pthread_detach(th);
    else {
        // Could not even start the thread: report as a failure rather than
        // hang on a progress screen that will never move.
        g_install_rc = -7;
    }
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
static void do_quit(void) { g_running = 0; }

static void activate_intro(int which) { if (which == 0) do_quit(); else goto_disks(); }
static void activate_disks(int which) {
    if (which == 1) goto_intro();
    else if (which == 2 && g_selected >= 0) goto_confirm();
}
static void activate_confirm(int which) {
    if (which == 1) goto_disks();
    else if (which == 2 && g_tf_match) start_install();
}
static void activate_done(int which) {
    if (which == 0) do_quit();
    else { if (g_done_success) reboot(); else goto_disks(); }
}

// Move the selection to the next SELECTABLE row and scroll it into view.
// gui_list owns the viewport, so scrolling is gui_list_move_sel's job and this
// function only has to skip the rows that cannot be chosen.
static void move_list_selection(int dir) {
    if (g_nrows == 0) return;
    int idx = g_selected;
    if (idx < 0) idx = dir > 0 ? -1 : g_nrows;
    for (int step = 0; step < g_nrows; step++) {
        idx += dir;
        if (idx < 0) idx = g_nrows - 1;
        if (idx >= g_nrows) idx = 0;
        if (g_rows[idx].selectable) {
            int cur = g_selected < 0 ? idx : g_selected;
            g_selected = cur;
            gui_list_move_sel(&g_list, &g_selected, idx - cur);
            g_selected = idx;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Input
//
// FOCUS MODEL, one table, used by both the keyboard and the pointer:
//   screen        focus 0            focus 1        focus 2
//   INTRO         (unused)           Quit           Continue
//   DISKS         the disk list      Back           Continue
//   CONFIRM       the type-to-confirm field  Back   Erase and install
//   PROGRESS      no controls at all
//   DONE          (unused)           Close          Restart now / Try again
// Focus renders as GUI_ST_FOCUS plus the wizard's 2px ring - NOT as hover,
// which is what the old code did and which made a keyboard user's position
// indistinguishable from a mouse hovering somewhere else.
// ---------------------------------------------------------------------------
static int focus_count(void) {
    switch (g_screen) {
        case SCR_INTRO:    return 3;   /* 0 unused, cycles 1<->2 via the +1 below */
        case SCR_DISKS:    return 3;
        case SCR_CONFIRM:  return 3;
        case SCR_DONE:     return 3;
        default:           return 0;
    }
}
static int focus_first(void) { return (g_screen == SCR_INTRO || g_screen == SCR_DONE) ? 1 : 0; }

static void focus_next(int dir) {
    int n = focus_count(); if (n <= 0) return;
    int lo = focus_first(), span = n - lo;
    int cur = g_focus < lo ? lo : g_focus;
    g_focus = lo + ((cur - lo + dir + span) % span);
}

static void handle_key(gui_event_t *ev) {
    char c = ev->key_char;
    uint32_t kc = ev->keycode;
    if (g_screen == SCR_PROGRESS) return;   // no controls, by design

    // The confirm field owns every printable key while it has focus, so typing
    // a disk name that contains a space cannot be swallowed as "activate".
    if (g_screen == SCR_CONFIRM && g_focus == 0 && c != '\t' && c != 27) {
        if (c == '\r' || c == '\n') { if (g_tf_match) activate_confirm(2); return; }
        if (tf_handle_key(&g_tf, ev)) {
            g_tf_match = (strcmp(g_tf_buf, g_rows[g_confirm_row].name) == 0) && g_tf.len > 0;
            redraw();
        }
        return;
    }

    if (c == 27) {   // Esc always retreats one step; it never destroys anything
        switch (g_screen) {
            case SCR_INTRO:   do_quit(); break;
            case SCR_DISKS:   goto_intro(); break;
            case SCR_CONFIRM: goto_disks(); break;
            case SCR_DONE:    do_quit(); break;
            default: break;
        }
        return;
    }
    if (c == '\t') { focus_next(1); redraw(); return; }

    if (g_screen == SCR_DISKS && g_focus == 0) {
        if (kc == GUI_KEY_UP)   { move_list_selection(-1); redraw(); return; }
        if (kc == GUI_KEY_DOWN) { move_list_selection(1);  redraw(); return; }
        if (gui_list_key(&g_list, kc)) { redraw(); return; }
    }

    int is_enter = (c == '\r' || c == '\n');
    int is_space = (c == ' ');
    if (is_enter || is_space) {
        switch (g_screen) {
            case SCR_INTRO:   activate_intro(g_focus); break;
            case SCR_DISKS:
                if (g_focus == 0) { if (is_enter && g_selected >= 0) activate_disks(2); }
                else activate_disks(g_focus);
                break;
            case SCR_CONFIRM: activate_confirm(g_focus); break;
            case SCR_DONE:    activate_done(g_focus);    break;
            default: break;
        }
    }
}

static void handle_click(int wmx, int wmy) {
    if (g_screen == SCR_PROGRESS) return;
    int mx = b_x(wmx), my = b_y(wmy);

    // Footer first: both rects come from the same two variables the draw used.
    if (my >= FOOT_Y && my < FOOT_Y + BTN_H) {
        if (pt_in(mx, my, BACK_X, FOOT_Y, BACK_W, BTN_H)) {
            g_focus = 1;
            switch (g_screen) {
                case SCR_INTRO:   activate_intro(1); break;
                case SCR_DISKS:   activate_disks(1); break;
                case SCR_CONFIRM: activate_confirm(1); break;
                case SCR_DONE:    activate_done(0); break;
                default: break;
            }
            return;
        }
        if (pt_in(mx, my, g_act_x, FOOT_Y, g_act_w, BTN_H)) {
            g_focus = 2;
            switch (g_screen) {
                case SCR_INTRO:   activate_intro(1); break;
                case SCR_DISKS:   if (g_selected >= 0) activate_disks(2); break;
                case SCR_CONFIRM: if (g_tf_match) activate_confirm(2); break;
                case SCR_DONE:    activate_done(1); break;
                default: break;
            }
            return;
        }
        return;
    }

    if (g_screen == SCR_DISKS) {
        int hit = gui_list_press(&g_list, wmx, wmy);
        if (hit >= 0 && hit < g_nrows && g_rows[hit].selectable) {
            g_selected = hit; g_focus = 0; redraw();
        } else if (hit >= 0) {
            g_focus = 0; redraw();          // a click on a disabled row still focuses the list
        }
        return;
    }
    if (g_screen == SCR_CONFIRM) {
        if (pt_in(mx, my, TF_X, TF_Y, TF_W, TF_H)) {
            g_focus = 0;
            // Caret to the clicked glyph. Measured with gui_ttf_render_width()
            // because that is the width function that agrees with the renderer
            // drawing the text (gui_ttf_width() adds kerning the drawer does
            // not apply, so the caret would drift on kerned pairs).
            int rel = mx - (TF_X + 8), best = 0;
            for (int i = 0; i <= g_tf.len; i++) {
                char pre[64];
                int n = i < 63 ? i : 63;
                for (int k = 0; k < n; k++) pre[k] = g_tf_buf[k];
                pre[n] = 0;
                if (gui_ttf_render_width(pre, TY_BODY) <= rel) best = i; else break;
            }
            tf_set_caret(&g_tf, best);
            redraw();
        }
        return;
    }
}

static void handle_move(int wmx, int wmy) {
    if (g_screen != SCR_DISKS) return;
    int changed = gui_list_motion(&g_list, wmx, wmy);
    int newhover = gui_list_row_at(&g_list, wmx, wmy);
    if (newhover >= g_nrows) newhover = -1;
    if (newhover != g_hover_row) { g_hover_row = newhover; changed = 1; }
    if (changed) redraw();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    build_palette();

    int win_x = 80, win_y = 40;
    fb_info_t fi;
    if (fb_info(&fi) == 0 && fi.width > 0 && fi.height > 0) {
        win_x = ((int)fi.width  - WIN_W) / 2;
        win_y = ((int)fi.height - 36 - WIN_H) / 2;
        if (win_x < 0) win_x = 0;
        if (win_y < 0) win_y = 0;
    }

    g_win = win_create("Install to Disk", win_x, win_y, WIN_W, WIN_H);
    if (g_win < 0) { printf("[INSTALL] win_create failed\n"); return 1; }
    // NOCHROME must be set BEFORE the first draw: it is a one-way flag and it
    // reallocates the content buffer to the full window size. For a nochrome
    // window the size handed to SYS_WIN_CREATE IS the outer size, so 688x616 of
    // card is what we get, unlike a chromed window where #184 applies.
    win_set_nochrome(g_win);
    win_set_shadow(g_win);

    g_focus = focus_first();
    redraw();

    gui_event_t ev;
    while (g_running) {
        int et = win_get_event(g_win, &ev, 120);

        if (g_screen == SCR_PROGRESS) {
            progress_tick();
            redraw();
            // Every input is deliberately swallowed: the engine cannot stop
            // once writing starts, so there is nothing an input could mean.
            (void)et;
            continue;
        }
        if (et == 0) continue;

        switch (ev.type) {
            case EVENT_REDRAW: redraw(); break;
            case EVENT_WINDOW_CLOSE: do_quit(); break;
            case EVENT_MOUSE_MOVE: handle_move(ev.mouse_x, ev.mouse_y); break;
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) handle_click(ev.mouse_x, ev.mouse_y);
                break;
            case EVENT_MOUSE_UP:
                if (g_screen == SCR_DISKS) gui_list_release(&g_list);
                break;
            // (#appstyle) THE WHEEL. There was no case for this event anywhere
            // in the file, which is half of why the disk list's scrollbar was
            // decoration.
            case EVENT_MOUSE_SCROLL:
                if (g_screen == SCR_DISKS &&
                    gui_list_wheel(&g_list, ev.mouse_x, ev.mouse_y, ev.scroll_delta))
                    redraw();
                break;
            case EVENT_KEY_DOWN: handle_key(&ev); break;
            default: break;
        }
    }

    win_destroy(g_win);
    return 0;
}
