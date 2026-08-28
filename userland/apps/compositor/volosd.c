// volosd.c - #162: the volume on-screen display.
//
// WHAT THIS IS AND WHAT IT IS NOT
// ---------------------------------------------------------------------------
// This file is FEEDBACK ONLY. It does not set the volume, does not decide what
// a key means, and has no keyboard handling of any kind. The media keys are a
// KERNEL global hotkey (cpu/isr.c, see the long comment there for why it
// cannot live out here: a running Win16 app is the sole keyboard consumer, so
// SYS_GET_KEYBOARD returns -1 to this process while one is up, and a hotkey
// checked here would be dead for every Win16 app on the machine). By the time
// this file learns anything, the volume has already changed.
//
// So the whole job is: notice, and draw.
//
//   volosd_tick()    once per compositor loop. ONE syscall (SYS_VOL_STATE)
//                    returning level + mute + two change counters packed into
//                    one word. Cheaper than the four calls it replaces.
//   volosd_render()  draws the card if it is within its linger window.
//
// WHY TWO COUNTERS. `keyseq` counts MEDIA-KEY changes; `seq` counts changes
// from any source including the tray slider and Settings. The OSD arms off
// keyseq, so dragging the tray volume slider does not pop an OSD on top of
// the slider the user is already looking at, while the tray gauge still
// tracks a hotkey change because that path reads the same live level.
//
// PERSISTENCE IS NOT DONE HERE, ON PURPOSE. profile_tick() already hashes
// get_volume()*29 once a second and saves UIPROFIL.YML when the hash moves.
// That was dead weight before #162 because sys_get_volume() returned a
// hardcoded 80 forever, so the hash term never changed and the volume never
// persisted whatever the user did. Now that the getter is real, hotkey
// changes persist through the existing path with no new code. Do not add a
// second saver here.
//
// DESIGN. Specified before implementation per the project's design-first rule,
// against docs/UI_STYLE_GUIDE.md and the confirmdialog.c overlay family
// (SURFACE_OVERLAY fill, WINDOW_BORDER outline, RADIUS_CARD, the notif.c
// shadow offset/alpha reused verbatim). Anchored top-centre because that is
// the one screen zone left empty by all five dock styles, the notification
// stack (top-right) and the tray/start menus (dock-anchored), and because
// anchoring off taskbar_top_inset() puts it below a top bar automatically
// instead of hand-rolling per-dock-style branching.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"

// Opaque ARGB from a theme token, exactly as confirmdialog.c does it.
#define TC(id) (0xFF000000u | theme_color(id))

// #uiscale: OSD_TEXT_PX is a TTF point size already covered by the
// draw_text_ttf()/text_width_ttf() chokepoint - left unscaled here.
#define OSD_W            ui_px(220)
#define OSD_H            ui_px(84)
#define OSD_PAD          ui_px(16)
#define OSD_ICON         ui_px(32)
#define OSD_BAR_H        ui_px(8)
#define OSD_BAR_R        ui_px(4)
#define OSD_GAP          ui_px(12)   // icon row -> bar
#define OSD_TOP_MARGIN   ui_px(20)
#define OSD_TEXT_PX       16   // on the {11,12,14,16,18,20,24,28,32} ladder
// How long the card stays after the LAST change. Re-armed, never stacked:
// there is exactly one instance of this state, so holding a volume key gives
// one card that keeps re-arming rather than a queue of them.
#define OSD_LINGER_MS   1200

static int      g_vol_level     = -1;   // -1 = never polled
static int      g_vol_muted     = 0;
static unsigned g_vol_keyseq    = 0;
static int      g_vol_seen      = 0;    // have we ever read a state?
static unsigned long g_vol_shown_ms = 0;

// ---------------------------------------------------------------------------
// Poll. One syscall per compositor loop.
// ---------------------------------------------------------------------------
void volosd_tick(void)
{
    unsigned long long st = vol_state();
    int      level  = vol_state_level(st);
    int      muted  = vol_state_muted(st);
    unsigned keyseq = vol_state_keyseq(st);

    if (!g_vol_seen) {
        // First poll of the session: adopt the state WITHOUT showing an OSD.
        // A card that appears by itself at login because the compositor just
        // started is a bug report, not feedback.
        g_vol_seen   = 1;
        g_vol_level  = level;
        g_vol_muted  = muted;
        g_vol_keyseq = keyseq;
        return;
    }

    g_vol_level = level;
    g_vol_muted = muted;

    // Compared for INEQUALITY, never ordering: the counter is 16-bit and
    // wraps, and a wrap is a non-event under this test.
    if (keyseq != g_vol_keyseq) {
        g_vol_keyseq   = keyseq;
        g_vol_shown_ms = uptime_ms();   // overwritten, not queued: re-arm
        g_needs_redraw = true;
    }
}

int volosd_visible(void)
{
    if (!g_vol_shown_ms) return 0;
    return (uptime_ms() - g_vol_shown_ms) < OSD_LINGER_MS;
}

// ---------------------------------------------------------------------------
// The speaker glyph, built from primitives that exist.
// ---------------------------------------------------------------------------
// There is no polygon fill and no runtime icon scaling in this compositor
// (mico_blit() is nearest-neighbour over a pre-rasterised /ICONS/*.ICN, and
// there is no "volume" asset), so the cone is drawn column by column with
// draw_vline() - the same per-row/per-column technique draw_rounded_rect()
// itself uses - and the waves are full AA rings clipped to their right half,
// because the clip stack exists and an arc primitive does not.
static void volosd_icon(int x, int y, int level, int muted, uint32_t ink)
{
    // Driver body.
    draw_fill_rect(x + 4, y + 12, 5, 8, ink);

    // Cone: 8 columns flaring 8px -> 20px, tip at the icon box's centre.
    for (int c = 0; c < 8; c++) {
        int h = 8 + (c * 12) / 7;
        draw_vline(x + 9 + c, y + 16 - h / 2, h, ink);
    }

    if (muted) {
        // The slash REPLACES the waves. Always the error tint, whatever the
        // theme, matching the existing tray mute treatment in taskbar.c.
        draw_line_aa(x + 3, y + 4, x + 24, y + 28, 3.0f,
                     TC(THEME_COLOR_ERROR), 255);
        return;
    }

    // 0%% unmuted draws the cone with NO waves, which is what makes it read
    // differently from muted: muted also shows the cone, but slashed, in the
    // error tint, with "MUTED" instead of a percentage and an error-toned
    // bar. Three independent signals separate "silent" from "muted", so no
    // single bad colour in one theme can collapse the distinction.
    int waves = (level <= 0) ? 0 : (level <= 33) ? 1 : (level <= 66) ? 2 : 3;
    if (waves == 0) return;

    draw_push_clip(x + 18, y, 14, OSD_ICON);
    if (waves >= 1) draw_circle_ring_aa(x + 16, y + 16,  6.0f, 2.0f, ink, 255);
    if (waves >= 2) draw_circle_ring_aa(x + 16, y + 16, 10.0f, 2.0f, ink, 255);
    if (waves >= 3) draw_circle_ring_aa(x + 16, y + 16, 14.0f, 2.0f, ink, 255);
    draw_pop_clip();
}

// ---------------------------------------------------------------------------
// Render. Called from BOTH render paths (the normal overlay tail and the
// exclusive lock-screen branch), because "from anywhere" in #162 explicitly
// includes the lock screen and that branch returns before the tail.
// ---------------------------------------------------------------------------
void volosd_render(void)
{
    if (!volosd_visible()) return;

    int level = g_vol_level < 0 ? 0 : g_vol_level > 100 ? 100 : g_vol_level;
    int muted = g_vol_muted;

    int w = OSD_W;
    if (w > g_fb_width - 16) w = g_fb_width - 16;   // 640x480 and narrower
    if (w < 120) return;                            // nowhere sane to put it

    int x = (g_fb_width - w) / 2;
    int y = taskbar_top_inset() + OSD_TOP_MARGIN;
    int radius = theme_metric(THEME_METRIC_RADIUS_CARD);

    uint32_t surface = TC(THEME_COLOR_SURFACE_OVERLAY);
    uint32_t border  = TC(THEME_COLOR_WINDOW_BORDER);
    uint32_t ink     = TC(THEME_COLOR_ON_SURFACE);
    uint32_t track   = TC(THEME_COLOR_GAUGE_BG);
    uint32_t fill    = muted ? TC(THEME_COLOR_ERROR) : TC(THEME_COLOR_GAUGE_FG);

    // Shadow: same offset and alpha as notif.c's toast, reused rather than
    // re-chosen, so the two floating cards sit at the same apparent height.
    g_draw_blend = 70;
    draw_rounded_rect(x + 3, y + 4, w, OSD_H, radius, 0xFF000000u);
    g_draw_blend = 255;

    draw_rounded_rect(x, y, w, OSD_H, radius, surface);
    draw_rect_outline(x, y, w, OSD_H, border);

    volosd_icon(x + OSD_PAD, y + OSD_PAD, level, muted, ink);

    // Readout, right-aligned on the icon row, vertically centred against the
    // 32px icon box using the real font metrics rather than a tuned constant.
    char buf[8];
    if (muted) {
        buf[0]='M'; buf[1]='U'; buf[2]='T'; buf[3]='E'; buf[4]='D'; buf[5]=0;
    } else {
        int n = 0;
        if (level >= 100)     { buf[n++] = '1'; buf[n++] = '0'; buf[n++] = '0'; }
        else if (level >= 10) { buf[n++] = (char)('0' + level / 10);
                                buf[n++] = (char)('0' + level % 10); }
        else                  { buf[n++] = (char)('0' + level); }
        buf[n++] = '%';
        buf[n]   = 0;
    }
    int fm[3];
    int th = OSD_TEXT_PX;
    // #uiscale: scale to match draw_text_ttf()/text_width_ttf(), which scale internally.
    if (font_metrics(0, ui_px(OSD_TEXT_PX), fm) == 0) th = fm[0] - fm[1];
    int tw = text_width_ttf(buf, OSD_TEXT_PX);
    draw_text_ttf(x + w - OSD_PAD - tw,
                  y + OSD_PAD + (OSD_ICON - th) / 2,
                  buf, OSD_TEXT_PX, muted ? TC(THEME_COLOR_ERROR) : ink);

    // Bar. The fill is drawn at the real level even while muted, so the card
    // shows what unmuting will return you to; the error tint is what says it
    // is not audible right now.
    int bx = x + OSD_PAD;
    int by = y + OSD_PAD + OSD_ICON + OSD_GAP;
    int bw = w - 2 * OSD_PAD;
    draw_rounded_rect(bx, by, bw, OSD_BAR_H, OSD_BAR_R, track);
    int fw = level * bw / 100;
    if (fw > 0) draw_rounded_rect(bx, by, fw, OSD_BAR_H, OSD_BAR_R, fill);
}
