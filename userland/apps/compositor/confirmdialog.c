// confirmdialog.c - see confirmdialog.h for the "why" (docs/CONFIRM_MODAL_DESIGN.html).
#include "compositor.h"
#include "confirmdialog.h"
#include "syscall.h"
#include "string.h"
#include "../../libc/theme.h"

// gui_ensure_contrast()/GUI_FLOOR_TEXT: real symbols/constant in libc.a
// (userland/libc/gui.c, declared in gui_style.h). gui_style.h itself cannot
// be #included here: it pulls in libc/types.h, whose `typedef _Bool bool`
// collides head-on with compositor.h's own `typedef int bool` - the exact
// pre-existing divergence main.c documents next to its own hand-declared
// `signal()` prototype (see main.c ~2792). Hand-declare the one function and
// the one constant needed instead of the whole header, same fix.
#define GUI_FLOOR_TEXT 450
extern uint32_t gui_ensure_contrast(uint32_t fg, uint32_t bg, int min_x100);

// Input settle: verbatim copy of elevate.c's ELEV_SETTLE_MS mechanism (#745).
// Everything typed in the first 250ms after the card is drawn is discarded,
// so a buffered keystroke run from before the dialog opened cannot land on a
// button, and a click cannot be timed through the appearance. This is the
// SECOND of the two defects the design doc's audit found in the old power
// confirm dialog (which had no settle timer at all).
#define CONFIRM_SETTLE_MS 250

// Scrim, verbatim value: matches kernel/gui/login.c's LOGIN_SCRIM_ALPHA and
// elevate.c's ELEV_SCRIM_A (design doc section 6 - proven safe as a ONE-TIME
// draw at these three shipping call sites, because all three already suspend
// repaint of everything under them while open).
#define CONFIRM_SCRIM_A 145

// Card geometry, exact px per design doc section 2.1. Not on the 4px spacing
// grid by the style guide's own stated exception for overall panel widths.
// #uiscale: scaled at the definition. TITLE_PX/BODY_PX are TTF point sizes
// already covered by the draw_text_ttf()/text_width_ttf() chokepoint (they
// are passed straight through as `size`), so those two are deliberately
// left as plain logical numbers here - scaling them again would double-scale.
#define CARD_W        ui_px(360)
#define TITLE_X       ui_px(16)
#define TITLE_Y       ui_px(16)
#define TITLE_PX      20
#define RULE_Y        ui_px(56)
#define RULE_X        ui_px(16)
#define RULE_W        ui_px(328)
#define BODY_X        ui_px(16)
#define BODY_Y        ui_px(73)
#define BODY_W        ui_px(328)
#define BODY_PX       14
#define BODY_LH       ui_px(20)
#define FOOTER_H      ui_px(60)
#define BTN_H         ui_px(28)
#define BTN_MINW      ui_px(96)
#define BTN_PAD_H     ui_px(16)
#define BTN_GAP       ui_px(8)
#define BTN_INSET_R   16
#define FOCUS_W       2
#define FOCUS_GAP     2

static int card_h(int n_lines) { return 157 + n_lines * BODY_LH; }

// theme_color() returns 0x00RRGGBB (see userland/libc/theme.h); every fill
// primitive in this file's draw.c neighbors (CLR_MENU_BG et al, main.c's own
// TC() macro in compositor_apply_theme()) expects opaque 0xFFRRGGBB. Same
// fix, same reason, applied at every read site below via this macro instead
// of copy-pasting the OR.
#define TC(id) (0xFF000000u | theme_color(id))

// Synthetic bold: this TTF renderer (draw_text_ttf(), shared with elevate.c)
// has no bold weight - see gui_style.h's GUI_FOCUS_W comment for the same
// class of "the primitive doesn't have this yet" gap. A 1px double-draw is a
// cheap, house-precedented approximation (same trick used for shadowed text
// elsewhere in this file's neighbors); real bold glyphs are a font-rendering
// project of their own and out of scope here.
static void draw_text_ttf_bold(int32_t x, int32_t y, const char *s, int size, uint32_t color)
{
    draw_text_ttf(x, y, s, size, color);
    draw_text_ttf(x + 1, y, s, size, color);
}

void confirm_dialog_open(confirm_dialog_t *d, confirm_variant_t variant,
                         const char *title,
                         const char *const *lines, int n_lines,
                         const char *cancel_label, const char *action_label)
{
    memset(d, 0, sizeof(*d));
    d->open = 1;
    d->variant = variant;
    strncpy(d->title, title ? title : "", sizeof(d->title) - 1);

    // #204 fix: join whatever paragraph(s) the caller passed into one buffer,
    // then wrap FOR REAL against the card's actual 328px body width
    // (BODY_W below) - see confirmdialog.h's header comment for why "the
    // caller pre-wraps" was a broken assumption at every real call site
    // (all 7 measured 474-727px on a single unwrapped line). Reuses
    // notif.c's proven wrap_text_ttf() (#762) rather than a second hand-
    // rolled wrap loop.
    if (n_lines < 1) n_lines = 1;
    if (n_lines > CONFIRM_MAX_LINES) n_lines = CONFIRM_MAX_LINES;
    char joined[CONFIRM_LINE_MAX * CONFIRM_MAX_LINES];
    joined[0] = 0;
    for (int i = 0; i < n_lines; i++) {
        if (i > 0) strncat(joined, " ", sizeof(joined) - strlen(joined) - 1);
        if (lines && lines[i])
            strncat(joined, lines[i], sizeof(joined) - strlen(joined) - 1);
    }
    d->n_lines = wrap_text_ttf(joined, BODY_PX, BODY_W, CONFIRM_MAX_LINES, d->lines);
    if (d->n_lines < 1) d->n_lines = 1;

    if (variant != CONFIRM_NOTICE)
        strncpy(d->cancel_label, cancel_label ? cancel_label : "Cancel", sizeof(d->cancel_label) - 1);
    strncpy(d->action_label, action_label ? action_label : "OK", sizeof(d->action_label) - 1);

    // THE FIX for defect #1 (design doc section 3, "Enter" row): destructive
    // opens with focus on Cancel, matching elevate.c's proven rule exactly -
    // a bare Enter on open therefore cancels, never approves. Neutral opens
    // on Action because there is nothing to protect against there (Log
    // Out/Lock have no data-loss consequence). Notice has one control, so
    // "focus" is not meaningful, but is set to 1 (its single button) so any
    // code that reads it uniformly sees "the one thing" as focused.
    d->focus = (variant == CONFIRM_DESTRUCTIVE) ? 0 : 1;
    d->shown_ms = uptime_ms();
}

void confirm_dialog_close(confirm_dialog_t *d)
{
    d->open = 0;
    g_needs_redraw = true;
}

// Flat scrim over the whole screen, per design doc section 6. Exposed
// separately for a caller that wants to draw it once itself, but
// confirm_dialog_render() below already calls this every time it runs - the
// SAME pattern elevate.c's elevate_render() already ships (it re-blends its
// own scrim on every render() call too). This is safe, not merely
// convenient: render_frame_body() (main.c) recomposites its ENTIRE layer
// stack - wallpaper first, THEN this scrim, THEN the card - on every actual
// call, clipped to the current damage rect, and only actually RUNS when a
// real redraw is needed (main.c's g_needs_redraw gate). So each call blends
// this scrim onto a FRESHLY redrawn, unscrimmed wallpaper/desktop, never onto
// the previous call's already-scrimmed pixels - there is no accumulation to
// avoid, and matching the two already-shipping system-modal surfaces exactly
// is simpler than inventing a "scrim already drawn" flag this architecture
// does not need.
void confirm_dialog_scrim(void)
{
    int ob = g_draw_blend;
    g_draw_blend = CONFIRM_SCRIM_A;
    draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);
    g_draw_blend = ob;
}

static int btn_w(const char *label)
{
    int tw = text_width_ttf(label, BODY_PX);
    int w = tw + BTN_PAD_H * 2;
    return (w < BTN_MINW) ? BTN_MINW : w;
}

// All geometry, screen-centered (system-modal is always screen-centered -
// design doc: "screen-centred from the framebuffer size... An app-positioned
// prompt looks like it belongs to the app", the same reasoning elevate.c's
// elev_geom() states for the elevation panel).
typedef struct {
    int px, py, pw, ph;
    int footer_y;
    int btn_y;
    int cancel_x, cancel_w;
    int act_x, act_w;      // for NOTICE, this is the single OK button
} cd_geom_t;

static void cd_geom(const confirm_dialog_t *d, cd_geom_t *g)
{
    g->pw = CARD_W;
    g->ph = card_h(d->n_lines);
    g->px = (g_fb_width - g->pw) / 2;
    g->py = (g_fb_height - g->ph) / 2;
    g->footer_y = g->ph - FOOTER_H;
    g->btn_y = g->footer_y + (FOOTER_H - BTN_H) / 2;   // = footer_y + 16

    g->act_w = btn_w(d->action_label);
    g->act_x = g->pw - BTN_INSET_R - g->act_w;
    if (d->variant != CONFIRM_NOTICE) {
        g->cancel_w = btn_w(d->cancel_label);
        g->cancel_x = g->act_x - BTN_GAP - g->cancel_w;
    } else {
        g->cancel_w = 0;
        g->cancel_x = 0;
    }
}

// One button: fill/border/ink per its role, plus the focus ring OUTSIDE the
// control (design doc 2.1: "2px outline, offset 2px outside the focused
// button's own edge, square corners always"). `danger`/`primary` fills are
// floored through gui_ensure_contrast() at draw time (design doc 5.1: 7 of
// 14 shipped themes fail the 4.5:1 text floor on the raw on_danger/danger or
// on_accent/accent pair - this is the recommended mitigation, reusing the
// exact primitive Settings' avatar-swatch ring already uses instead of
// hand-picking colors per theme).
typedef enum { CD_BTN_STANDARD, CD_BTN_PRIMARY, CD_BTN_DANGER } cd_btn_style_t;

static void cd_draw_button(int x, int y, int w, int h, const char *label,
                           cd_btn_style_t style, int focused, int radius,
                           uint32_t on_surface, uint32_t border_strong,
                           uint32_t surface_raised, uint32_t focus_ring)
{
    uint32_t fill, ink;
    int bold = 0;
    if (style == CD_BTN_STANDARD) {
        fill = surface_raised;
        ink  = on_surface;
        draw_rounded_rect(x, y, w, h, radius, fill);
        draw_rect_outline(x, y, w, h, border_strong);
    } else if (style == CD_BTN_PRIMARY) {
        uint32_t accent    = TC(THEME_COLOR_ACCENT);
        uint32_t on_accent = TC(THEME_COLOR_ON_ACCENT);
        on_accent = gui_ensure_contrast(on_accent, accent, GUI_FLOOR_TEXT);
        fill = accent; ink = on_accent; bold = 1;
        draw_rounded_rect(x, y, w, h, radius, fill);
    } else {   // CD_BTN_DANGER
        uint32_t danger    = TC(THEME_COLOR_DANGER);
        uint32_t on_danger = TC(THEME_COLOR_ON_DANGER);
        on_danger = gui_ensure_contrast(on_danger, danger, GUI_FLOOR_TEXT);
        fill = danger; ink = on_danger; bold = 0;   // deliberately NOT bold - see confirm_variant_t CONFIRM_DESTRUCTIVE
        draw_rounded_rect(x, y, w, h, radius, fill);
    }
    int tw = text_width_ttf(label, BODY_PX);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - BODY_PX) / 2 - 1;
    if (bold) draw_text_ttf_bold(tx, ty, label, BODY_PX, ink);
    else      draw_text_ttf(tx, ty, label, BODY_PX, ink);

    if (focused) {
        int rx = x - FOCUS_GAP - FOCUS_W, ry = y - FOCUS_GAP - FOCUS_W;
        int rw = w + 2 * (FOCUS_GAP + FOCUS_W), rh = h + 2 * (FOCUS_GAP + FOCUS_W);
        draw_fill_rect(rx, ry, rw, FOCUS_W, focus_ring);
        draw_fill_rect(rx, ry + rh - FOCUS_W, rw, FOCUS_W, focus_ring);
        draw_fill_rect(rx, ry, FOCUS_W, rh, focus_ring);
        draw_fill_rect(rx + rw - FOCUS_W, ry, FOCUS_W, rh, focus_ring);
    }
}

void confirm_dialog_render(const confirm_dialog_t *d)
{
    if (!d->open) return;
    confirm_dialog_scrim();
    cd_geom_t g; cd_geom(d, &g);

    uint32_t surface_overlay = TC(THEME_COLOR_SURFACE_OVERLAY);
    uint32_t surface_raised  = TC(THEME_COLOR_SURFACE_RAISED);
    uint32_t on_surface      = TC(THEME_COLOR_ON_SURFACE);
    uint32_t on_surface_mut  = TC(THEME_COLOR_MUTED);        // color.on_surface_muted
    uint32_t border_strong   = TC(THEME_COLOR_WINDOW_BORDER); // color.border_strong (same field, see themes.c case 9)
    uint32_t border_subtle   = TC(THEME_COLOR_BORDER_SUBTLE);
    uint32_t focus_ring      = TC(THEME_COLOR_FOCUS_RING);
    int radius_card = theme_metric(THEME_METRIC_RADIUS_CARD);
    int radius_btn  = theme_metric(THEME_METRIC_RADIUS_BTN);

    // (#glassmodal) Panel shadow: a 3-band approximation of the wizard's
    // continuous falloff (SH_SPREAD/SH_PEAK, docs/UI_GLASS_DESIGN_SYSTEM.md),
    // reusing draw_rounded_rect (no new shadow primitive needed) instead of
    // the old single hard-offset opaque rect. The design doc's
    // elevation_edge_w_modal/alpha_modal .mtheme tokens are still unwired
    // (grep confirms zero readers) - out of scope here too; this reuses the
    // same reasoned-not-measured numbers docs/GLASS_MODALS_AND_POPOUTS.html
    // documents (INFERRED, see CHANGELOG, not perceptually tuned on real
    // hardware).
    {
        int ob = g_draw_blend;
        g_draw_blend = 46;
        draw_rounded_rect(g.px + 2, g.py + 2, g.pw, g.ph, radius_card + 2, 0xFF000000);
        g_draw_blend = 28;
        draw_rounded_rect(g.px + 4, g.py + 4, g.pw, g.ph, radius_card + 4, 0xFF000000);
        g_draw_blend = 14;
        draw_rounded_rect(g.px + 6, g.py + 6, g.pw, g.ph, radius_card + 6, 0xFF000000);
        g_draw_blend = ob;
    }

    // (#glassmodal) Capture the real backdrop (the scrim just drawn by
    // confirm_dialog_scrim() above, and whatever desktop is under it) beneath
    // all four corners BEFORE painting the card - see
    // draw_round_corners_capture()'s contract in compositor.h. Restored at
    // the very end of this function, after the footer band and both buttons,
    // so their edges get rounded off too instead of squaring the bottom
    // corners back off.
    corner_capture_t cc;
    draw_round_corners_capture(&cc, g.px, g.py, g.pw, g.ph, radius_card, CORNER_ALL);

    // Card body: glass when enabled (the SAME CLR_GLASS_TINT/GLASS_SURF_MODAL
    // recipe the taskbar/dock/start menu already use - one glass tint token
    // used everywhere is the whole point), flat surface_overlay fill
    // otherwise. This preserves the exact old look for retro_unix/classic/
    // high_contrast, which turn glass off deliberately (see glass_or_flat()
    // in draw.c) - only a plain rect either way; the corner capture/restore
    // above/below does the rounding, not this fill.
    if (g_glass_enable) glass_render(g.px, g.py, g.pw, g.ph, CLR_GLASS_TINT, GLASS_SURF_MODAL);
    else                 draw_fill_rect(g.px, g.py, g.pw, g.ph, surface_overlay);
    draw_rect_outline(g.px, g.py, g.pw, g.ph, border_strong);
    glass_highlight_h(g.px + radius_card, g.py + 1, g.pw - 2 * radius_card);

    draw_text_ttf_bold(g.px + TITLE_X, g.py + TITLE_Y, d->title, TITLE_PX, on_surface);
    draw_fill_rect(g.px + RULE_X, g.py + RULE_Y, RULE_W, 1, border_subtle);
    for (int i = 0; i < d->n_lines; i++)
        draw_text_ttf(g.px + BODY_X, g.py + BODY_Y + i * BODY_LH, d->lines[i], BODY_PX, on_surface_mut);

    // Footer band: one elevation step below the card body (surface_raised),
    // 1px top rule in border_subtle - mirrors the wizard's own footer being a
    // step duller than its own background (design doc 2.1).
    draw_fill_rect(g.px, g.py + g.footer_y, g.pw, FOOTER_H, surface_raised);
    draw_fill_rect(g.px, g.py + g.footer_y, g.pw, 1, border_subtle);
    // Bottom corners share the card's own radius - draw_rounded_rect handles
    // the card's outer rounding above; the footer's own bottom edge sits
    // flush inside that already-rounded outline, so no separate corner mask
    // is needed here (same approach draw_popup_panel() uses house-wide).

    if (d->variant == CONFIRM_NOTICE) {
        cd_draw_button(g.px + g.act_x, g.py + g.btn_y, g.act_w, BTN_H, d->action_label,
                       CD_BTN_PRIMARY, 1, radius_btn, on_surface, border_strong, surface_raised, focus_ring);
    } else {
        cd_btn_style_t act_style = (d->variant == CONFIRM_DESTRUCTIVE) ? CD_BTN_DANGER : CD_BTN_PRIMARY;
        cd_draw_button(g.px + g.cancel_x, g.py + g.btn_y, g.cancel_w, BTN_H, d->cancel_label,
                       CD_BTN_STANDARD, d->focus == 0, radius_btn, on_surface, border_strong, surface_raised, focus_ring);
        cd_draw_button(g.px + g.act_x, g.py + g.btn_y, g.act_w, BTN_H, d->action_label,
                       act_style, d->focus == 1, radius_btn, on_surface, border_strong, surface_raised, focus_ring);
    }

    // Restore LAST, after the footer band and both buttons - see
    // draw_round_corners_capture()'s contract.
    draw_round_corners_restore(&cc);
}

// Returns 0 = no decision (settling, Tab, or an unhandled key), 1 = Cancel/
// dismiss, 2 = Action. This return value is ONLY the decision; it is not a
// "was the key consumed" flag. Callers (startmenu.c, taskbar.c) must still
// swallow every key while d->open regardless of this return value - a true
// modal owns the whole keyboard, matching startmenu_power_confirm_handle_key()'s
// pre-existing "return 1 unconditionally while open" contract at the call site.
int confirm_dialog_handle_key(confirm_dialog_t *d, int key)
{
    if (!d->open) return 0;

    // Settle timer: THE FIX for defect #2. See confirmdialog.h.
    if (uptime_ms() - d->shown_ms < CONFIRM_SETTLE_MS) return 0;

    // Esc always cancels/dismisses, from any state, no exception (design doc
    // section 3). For NOTICE there is no separate Cancel outcome; Esc
    // dismisses the same as OK.
    if (key == 27) {
        int r = (d->variant == CONFIRM_NOTICE) ? 2 : 1;
        confirm_dialog_close(d);
        return r;
    }

    if (key == '\t') {
        // 2 stops for a 2-button dialog, wrapping. NOTICE has one control, so
        // Tab is a no-op there (nothing to cycle to).
        if (d->variant != CONFIRM_NOTICE) {
            d->focus = d->focus ? 0 : 1;
            g_needs_redraw = true;
        }
        return 0;
    }

    if (key == '\n' || key == '\r' || key == ' ') {
        // THE FIX for defect #1: this activates the CURRENTLY FOCUSED
        // button, never a global "Enter = destructive action" binding. See
        // confirm_dialog_open()'s initial-focus comment for why a bare Enter
        // on a freshly-opened destructive dialog is therefore always safe.
        if (d->variant == CONFIRM_NOTICE) { confirm_dialog_close(d); return 2; }
        int r = d->focus ? 2 : 1;
        confirm_dialog_close(d);
        return r;
    }

    return 0;   // true modal: every other key is swallowed, nothing dispatched
}

int confirm_dialog_handle_mouse(confirm_dialog_t *d, int32_t x, int32_t y, int clicked)
{
    if (!d->open) return 0;
    if (uptime_ms() - d->shown_ms < CONFIRM_SETTLE_MS) return 0;
    if (!clicked) return 0;

    cd_geom_t g; cd_geom(d, &g);
    int by = g.py + g.btn_y;
    if (y >= by && y < by + BTN_H) {
        if (d->variant == CONFIRM_NOTICE) {
            int ax = g.px + g.act_x;
            if (x >= ax && x < ax + g.act_w) { confirm_dialog_close(d); return 2; }
            return 0;
        }
        int cx = g.px + g.cancel_x, ax = g.px + g.act_x;
        if (x >= cx && x < cx + g.cancel_w) { confirm_dialog_close(d); return 1; }
        if (x >= ax && x < ax + g.act_w)    { confirm_dialog_close(d); return 2; }
    }
    return 0;   // true modal: every other click is swallowed, never dismisses
}
