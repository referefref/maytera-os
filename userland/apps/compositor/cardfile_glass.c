// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// cardfile_glass.c - the Cardfile deck's glass chrome passes. See
// cardfile_glass.h for the contract and the N-surface rationale.
//
// Every number below is a transcription of the approved design's CSS
// (the build host:/root/cardfile-mockup.html), light / dark:
//   --glass-a      .58 / .52   tint alpha on chrome            -> 148 / 133
//   --glass-sheen  .34 / .10   white sheen at a pane's top     ->  87 /  26
//   --glass-border .65 / .16   luminous 1px border             -> 166 /  41
//   --glass-hi     .85 / .22   inset top-edge highlight        -> 217 /  56
//   --glass-shadow .16 / .50   drop shadows                    ->  41 / 128
//   rail .42, pane .62, popup .74 (fixed alphas in the same CSS)
//   --focus-rgb    #1856C0 / #4C8EFF
//   backdrop-filter: blur(14-24px) saturate(150-160%): the shared plane's
//   sigma-14 blur and a single 150% saturate (glass_backdrop_prepare(150)).
//   .spine filter: brightness(1 - depth*0.045): the per-step dim, applied
//   by glass_backdrop_fill()'s dim_x10 to the finished pane.
// Shadows have no GPU blur here; a CSS "offset o, blur b, spread s" shadow
// is rendered as (o + b/2 + s) one-pixel lines whose alpha falls off
// quadratically from the pane's edge, which reads as the same soft cast at
// these sizes (10-30px) and costs a handful of blended lines per pane.

#include "cardfile_glass.h"
#include "compositor.h"

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------
typedef struct {
    int      glass_a, sheen, border, hi, shadow;
    uint32_t focus;
} cfg_tokens_t;

static const cfg_tokens_t CFG_LIGHT = { 148, 87, 166, 217,  41, 0xFF1856C0u };
static const cfg_tokens_t CFG_DARK  = { 133, 26,  41,  56, 128, 0xFF4C8EFFu };

#define CFG_RAIL_A   107   // .42
#define CFG_PANE_A   158   // .62
#define CFG_POPUP_A  189   // .74
#define CFG_SAT_PCT  150   // backdrop saturate()
#define CFG_INNER_HAIRLINE_A 31   // .card .app border rgba(0,0,0,.12)

int cfg_is_dark(void) { return draw_luminance(CLR_GLASS_TINT) < 140; }
static const cfg_tokens_t *cfg_tok(void) { return cfg_is_dark() ? &CFG_DARK : &CFG_LIGHT; }

uint32_t cfg_surface_color(void) { return CLR_MENU_BG; }

void cfg_begin_frame(void)
{
    if (!g_glass_enable) return;
    (void)glass_backdrop_prepare(CFG_SAT_PCT);   // fill() self-falls-back if it returns 0
}

int cfg_edge_side(const int *open_idx, int n_open, int idx)
{
    int best = -1, best_d = 0x7FFFFFFF;
    for (int k = 0; k < n_open; k++) {
        int d = open_idx[k] - idx;
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = open_idx[k]; }
    }
    if (best < 0 || best == idx) return 0;
    return (best > idx) ? 1 : -1;
}

// ---------------------------------------------------------------------------
// Small local helpers
// ---------------------------------------------------------------------------
static uint32_t cfg_mix(uint32_t a, uint32_t b, int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int rr = ar + (br - ar) * pct / 100;
    int rg = ag + (bg - ag) * pct / 100;
    int rb = ab + (bb - ab) * pct / 100;
    return 0xFF000000u | ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
}

// "Edge = card darkened 14% (lightened 14% on dark)" (design palette note).
static uint32_t cfg_edge_color(uint32_t card)
{
    return cfg_mix(card, cfg_is_dark() ? 0xFFFFFFFFu : 0xFF000000u, 14);
}

static void cfg_line_h(int32_t x, int32_t y, int32_t w, uint32_t c, int a)
{
    if (w <= 0 || a <= 0) return;
    int ob = g_draw_blend;
    g_draw_blend = a > 255 ? 255 : a;
    draw_hline(x, y, w, c);
    g_draw_blend = ob;
}
static void cfg_line_v(int32_t x, int32_t y, int32_t h, uint32_t c, int a)
{
    if (h <= 0 || a <= 0) return;
    int ob = g_draw_blend;
    g_draw_blend = a > 255 ? 255 : a;
    draw_vline(x, y, h, c);
    g_draw_blend = ob;
}
static void cfg_outline(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c, int a)
{
    if (w <= 0 || h <= 0 || a <= 0) return;
    int ob = g_draw_blend;
    g_draw_blend = a > 255 ? 255 : a;
    draw_rect_outline(x, y, w, h, c);
    g_draw_blend = ob;
}
static void cfg_fill(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c, int a)
{
    if (w <= 0 || h <= 0 || a <= 0) return;
    int ob = g_draw_blend;
    g_draw_blend = a > 255 ? 255 : a;
    draw_fill_rect(x, y, w, h, c);
    g_draw_blend = ob;
}

// Soft one-sided shadows: n one-pixel lines stepping away from a pane edge,
// alpha falling off quadratically from `peak` at the edge to 0.
static void cfg_shadow_v(int32_t x_first, int dir, int32_t y, int32_t h, int n, int peak)
{
    if (n <= 0 || peak <= 0 || h <= 0) return;
    for (int i = 0; i < n; i++) {
        int k = n - i;
        int a = peak * k * k / (n * n);
        if (a <= 0) break;
        cfg_line_v(x_first + i * dir, y, h, 0xFF000000u, a);
    }
}
static void cfg_shadow_h(int32_t y_first, int dir, int32_t x, int32_t w, int n, int peak)
{
    if (n <= 0 || peak <= 0 || w <= 0) return;
    for (int i = 0; i < n; i++) {
        int k = n - i;
        int a = peak * k * k / (n * n);
        if (a <= 0) break;
        cfg_line_h(x, y_first + i * dir, w, 0xFF000000u, a);
    }
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------
void cfg_rail(int32_t x, int32_t y, int32_t w, int32_t h)
{
    const cfg_tokens_t *t = cfg_tok();
    glass_bd_style_t st = { cfg_surface_color(), CFG_RAIL_A, 0, t->sheen, w, x, 0 };
    glass_backdrop_fill(x, y, w, h, &st);
    cfg_line_v(x + w - 1, y, h, 0xFFFFFFFFu, t->border);      // luminous right border
}

// (cfdock) See cardfile_glass.h's own comment: cfg_rail() generalised to any
// screen edge (or a between-cards bar, which passes CF_DOCK_EDGE_LEFT by
// convention). TOP/BOTTOM bars are horizontal (sheen runs along their width,
// x-anchored); LEFT/RIGHT bars are vertical (sheen runs along their height,
// y-anchored) - matching cfg_rail()'s own w/x sheen parameters for its
// (always vertical) rail.
void cfg_dock_bar(cf_rect_t r, int dock_edge)
{
    const cfg_tokens_t *t = cfg_tok();
    int horiz = (dock_edge == CF_DOCK_EDGE_TOP || dock_edge == CF_DOCK_EDGE_BOTTOM);
    glass_bd_style_t st = {
        cfg_surface_color(), CFG_RAIL_A, 0, t->sheen,
        horiz ? r.w : r.h, horiz ? r.x : r.y, 0
    };
    glass_backdrop_fill(r.x, r.y, r.w, r.h, &st);
    switch (dock_edge) {
        case CF_DOCK_EDGE_TOP:    cfg_line_h(r.x, r.y + r.h - 1, r.w, 0xFFFFFFFFu, t->border); break;
        case CF_DOCK_EDGE_BOTTOM: cfg_line_h(r.x, r.y,           r.w, 0xFFFFFFFFu, t->border); break;
        case CF_DOCK_EDGE_RIGHT:  cfg_line_v(r.x,           r.y, r.h, 0xFFFFFFFFu, t->border); break;
        default: /* LEFT, and the between-cards convention */
                                  cfg_line_v(r.x + r.w - 1, r.y, r.h, 0xFFFFFFFFu, t->border); break;
    }
}

void cfg_edge(cf_rect_t r, uint32_t card, int dim_x10, int side)
{
    const cfg_tokens_t *t = cfg_tok();
    int dist = dim_x10 / 45;   // cf_depth_dim_x10() is dist * 45
    glass_bd_style_t st = { cfg_edge_color(card), t->glass_a, dim_x10,
                            t->sheen, r.w * 60 / 100, r.x, 0 };
    glass_backdrop_fill(r.x, r.y, r.w, r.h, &st);

    // The shadow this spine RECEIVES from its nearer neighbour, on the side
    // facing the open card. A stowed spine's cast is "-10px 0 18px -6px"
    // (reach 13); the open card's own cast onto the first right spine is
    // "0 18px 40px -12px", wider and fainter at its sides (reach 16, and the
    // first LEFT spine gets nothing: the open spine casts no shadow).
    if (r.w > 4) {
        int reach = 0, peak = 0;
        if (dist >= 2)                 { reach = 13; peak = t->shadow * 3 / 4; }
        else if (dist == 1 && side < 0) { reach = 16; peak = t->shadow / 2; }
        if (reach > r.w - 3) reach = r.w - 3;
        if (side > 0)      cfg_shadow_v(r.x + r.w - 1, -1, r.y, r.h, reach, peak);
        else if (side < 0) cfg_shadow_v(r.x + 2,       +1, r.y, r.h, reach, peak);
    }
    cfg_line_v(r.x,     r.y, r.h, 0xFFFFFFFFu, t->border);    // border-left
    cfg_line_v(r.x + 1, r.y, r.h, 0xFFFFFFFFu, t->hi);        // inset highlight
}

void cfg_plate(cf_rect_t r, uint32_t card, int is_open, int dim_x10)
{
    const cfg_tokens_t *t = cfg_tok();
    corner_capture_t cap;
    draw_round_corners_capture(&cap, r.x, r.y, r.w, r.h, ui_px(7), CORNER_ALL);

    // drop shadow "0 6px 16px -4px" (open: "0 10px 24px -6px", +.12 alpha)
    int reach = is_open ? 16 : 10;
    int peak  = is_open ? (t->shadow + 31) : t->shadow;
    cfg_shadow_h(r.y + r.h, +1, r.x + 2, r.w - 4, reach, peak);
    cfg_shadow_v(r.x + r.w, +1, r.y + 4, r.h - 4, 4, peak / 3);
    cfg_shadow_v(r.x - 1,   -1, r.y + 4, r.h - 4, 4, peak / 3);

    glass_bd_style_t st = { card, t->glass_a, is_open ? 0 : dim_x10,
                            t->sheen, r.h * 55 / 100, r.y, 1 };
    glass_backdrop_fill(r.x, r.y, r.w, r.h, &st);
    cfg_outline(r.x, r.y, r.w, r.h, 0xFFFFFFFFu, t->border);
    cfg_line_h(r.x + 1, r.y + 1, r.w - 2, 0xFFFFFFFFu, t->hi);
    draw_round_corners_restore(&cap);
}

// (cfmaxwidth) See cardfile_glass.h's own comment. CFG_TAB_R_OUTER matches
// cfg_plate()'s uniform 7px.
//
// (cfdock, scallop-fix) CFG_TAB_R_JOIN used to be a much bigger radius
// applied to the TAB'S OWN TL/BL corners via draw_round_corners_capture()/
// restore() - the same primitive cfg_plate() uses for an ordinary rounded
// corner. That primitive only ever ERODES the rect it is given (coverage is
// 0, fully erased, exactly at that rect's own corner point, rising to 1 away
// from it - see draw.c). Applied to the tab's own join corners with a big
// radius, it eroded the tab's material AWAY from the join at top and bottom,
// leaving a concave notch that curves IN toward the tab's centre - backwards
// from a seamless flare, and the owner's reported bug ("the scallop curves
// in when it should curve out towards the card edge"). See
// cf_draw_join_bulge() (cardfile.c) for the full write-up of the fix, which
// this mirrors: erode an AUXILIARY box positioned OUTWARD from the tab's own
// join corner (filled with the tab's own glass backdrop style first) instead
// of the tab's own corner box, so the tab's silhouette bulges OUT into that
// box rather than receding into itself. Kept modest (matches CFG_TAB_R_OUTER's
// scale, not the old 16px) since this function only receives `r`, not the
// surrounding edge/neighbour geometry, so a big reach risks visibly
// overlapping an adjacent slot's own tab when many cards are stowed.
#define CFG_TAB_R_OUTER 7
#define CFG_TAB_R_JOIN  9

// Erodes a jr x jr auxiliary box straddling (cx,cy) - the tab's own TL
// corner when top=1, its own BL corner when top=0 - filled with the SAME
// glass backdrop style as the tab itself so the bulge reads as part of the
// same surface, fading to the true background at the box's own far corner.
static void cfg_draw_join_bulge(int32_t cx, int32_t cy, int32_t jr, int top,
                                const glass_bd_style_t *st) {
    if (jr <= 0) return;
    corner_capture_t cap;
    int32_t bx = cx - jr, by = cy - jr, bs = jr * 2;
    draw_round_corners_capture(&cap, bx, by, bs, bs, jr, top ? CORNER_TL : CORNER_BL);
    glass_backdrop_fill(bx, top ? by : cy, jr, jr, st);
    draw_round_corners_restore(&cap);
}

void cfg_tab(cf_rect_t r, uint32_t card, int is_open, int dim_x10)
{
    const cfg_tokens_t *t = cfg_tok();
    corner_capture_t cap_outer;
    draw_round_corners_capture(&cap_outer, r.x, r.y, r.w, r.h, ui_px(CFG_TAB_R_OUTER), CORNER_TR | CORNER_BR);

    // Drop shadow on the RIGHT/bottom (free/outer sides) only - no shadow on
    // the left (join) side: the tab is not "floating above" anything
    // different there, it is the same card.
    int reach = is_open ? 16 : 10;
    int peak  = is_open ? (t->shadow + 31) : t->shadow;
    cfg_shadow_h(r.y + r.h, +1, r.x + 2, r.w - 4, reach, peak);
    cfg_shadow_v(r.x + r.w, +1, r.y + 4, r.h - 4, 4, peak / 3);

    glass_bd_style_t st = { card, t->glass_a, is_open ? 0 : dim_x10,
                            t->sheen, r.h * 55 / 100, r.y, 1 };
    glass_backdrop_fill(r.x, r.y, r.w, r.h, &st);
    // Border: top, right, bottom only - the left (join) side carries no
    // stroke, so nothing crosses the flare into the card.
    cfg_line_h(r.x, r.y, r.w, 0xFFFFFFFFu, t->border);
    cfg_line_v(r.x + r.w - 1, r.y, r.h, 0xFFFFFFFFu, t->border);
    cfg_line_h(r.x, r.y + r.h - 1, r.w, 0xFFFFFFFFu, t->border);
    cfg_line_h(r.x + 1, r.y + 1, r.w - 2, 0xFFFFFFFFu, t->hi);

    draw_round_corners_restore(&cap_outer);

    // (cfdock, scallop-fix) The join-side bulge, drawn OUTSIDE r - see the
    // comment above CFG_TAB_R_JOIN.
    int32_t jr = ui_px(CFG_TAB_R_JOIN);
    if (jr > r.h / 2) jr = r.h / 2;
    cfg_draw_join_bulge(r.x, r.y,       jr, 1, &st);   // top join corner
    cfg_draw_join_bulge(r.x, r.y + r.h, jr, 0, &st);   // bottom join corner
}

void cfg_tab_button(cf_rect_t r, int hover)
{
    const cfg_tokens_t *t = cfg_tok();
    int a = t->sheen + (hover ? 56 : 15);      // sheen + .22 / + .06
    cfg_fill(r.x, r.y, r.w, r.h, 0xFFFFFFFFu, a);
    cfg_outline(r.x, r.y, r.w, r.h, 0xFFFFFFFFu, t->border);
    cfg_line_h(r.x + 1, r.y + 1, r.w - 2, 0xFFFFFFFFu, t->hi);
}

void cfg_card_frame(cf_rect_t body, uint32_t card, int frame_px, int foot_px, int is_focus)
{
    const cfg_tokens_t *t = cfg_tok();
    if (frame_px < 1) frame_px = 1;
    if (foot_px < 1) foot_px = 1;
    glass_bd_style_t st = { card, t->glass_a, 0, t->sheen, body.h * 30 / 100, body.y, 1 };
    // Four non-overlapping bands (overlapping them would double the tint at
    // the corners), never the interior.
    int mid_y = body.y + frame_px, mid_h = body.h - frame_px - foot_px;
    glass_backdrop_fill(body.x, body.y, body.w, frame_px, &st);                          // top
    if (mid_h > 0) {
        glass_backdrop_fill(body.x, mid_y, frame_px, mid_h, &st);                        // left
        glass_backdrop_fill(body.x + body.w - frame_px, mid_y, frame_px, mid_h, &st);    // right
    }
    glass_backdrop_fill(body.x, body.y + body.h - foot_px, body.w, foot_px, &st);        // foot

    // border on three sides (the edge is flush on the left), top highlight
    cfg_line_h(body.x, body.y, body.w, 0xFFFFFFFFu, t->border);
    cfg_line_h(body.x, body.y + body.h - 1, body.w, 0xFFFFFFFFu, t->border);
    cfg_line_v(body.x + body.w - 1, body.y, body.h, 0xFFFFFFFFu, t->border);
    cfg_line_h(body.x + 1, body.y + 1, body.w - 2, 0xFFFFFFFFu, t->hi);
    // inner hairline where the frame meets the app content
    if (mid_h > 0 && body.w > 2 * frame_px)
        cfg_outline(body.x + frame_px - 1, body.y + frame_px - 1,
                    body.w - 2 * frame_px + 2, mid_h + 2, 0xFF000000u, CFG_INNER_HAIRLINE_A);
    // focus ring: inset 1px at the design's focus colour, .55
    if (is_focus)
        cfg_outline(body.x + 1, body.y + 1, body.w - 2, body.h - 2, t->focus, 140);
}

void cfg_pane(cf_rect_t p, uint32_t card, int header_px, int is_focus)
{
    const cfg_tokens_t *t = cfg_tok();
    int ring = ui_px(4);
    if (header_px < 1) header_px = 1;
    glass_bd_style_t st = { card, CFG_PANE_A, 0, t->sheen, p.h * 40 / 100, p.y, 1 };
    int mid_y = p.y + header_px, mid_h = p.h - header_px - ring;
    glass_backdrop_fill(p.x, p.y, p.w, header_px, &st);                     // header
    if (mid_h > 0) {
        glass_backdrop_fill(p.x, mid_y, ring, mid_h, &st);                  // left
        glass_backdrop_fill(p.x + p.w - ring, mid_y, ring, mid_h, &st);     // right
    }
    glass_backdrop_fill(p.x, p.y + p.h - ring, p.w, ring, &st);            // bottom

    cfg_outline(p.x, p.y, p.w, p.h, 0xFFFFFFFFu, t->border);
    cfg_line_h(p.x + 1, p.y + 1, p.w - 2, 0xFFFFFFFFu, t->hi);
    if (mid_h > 0 && p.w > 2 * ring)
        cfg_outline(p.x + ring - 1, mid_y - 1, p.w - 2 * ring + 2, mid_h + 2,
                    0xFF000000u, CFG_INNER_HAIRLINE_A);
    if (is_focus)
        cfg_outline(p.x + 1, p.y + 1, p.w - 2, p.h - 2, t->focus, 153);
}

void cfg_popup(int32_t x, int32_t y, int32_t w, int32_t h)
{
    const cfg_tokens_t *t = cfg_tok();
    corner_capture_t cap;
    draw_round_corners_capture(&cap, x, y, w, h, ui_px(12), CORNER_ALL);
    // "0 20px 48px -12px", +.2 alpha: reach 32 below, 12 at the sides
    int peak = t->shadow + 51;
    cfg_shadow_h(y + h, +1, x + 6, w - 12, 24, peak);
    cfg_shadow_v(x + w, +1, y + 8, h - 8, 10, peak / 2);
    cfg_shadow_v(x - 1, -1, y + 8, h - 8, 10, peak / 2);

    glass_bd_style_t st = { cfg_surface_color(), CFG_POPUP_A, 0, t->sheen, h * 40 / 100, y, 1 };
    glass_backdrop_fill(x, y, w, h, &st);
    cfg_outline(x, y, w, h, 0xFFFFFFFFu, t->border);
    cfg_line_h(x + 1, y + 1, w - 2, 0xFFFFFFFFu, t->hi);
    draw_round_corners_restore(&cap);
}
