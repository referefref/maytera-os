// dragghost.c - the compositor half of cross-window drag ("docking").
//
// WHY THIS FILE HAS TO EXIST AT ALL
// ---------------------------------------------------------------------------
// An app can only draw inside its own window. The moment the user drags a
// terminal tab past that window's edge, the thing following the cursor is over
// the desktop, or over somebody else's window, and the source app cannot put a
// single pixel there. Only the compositor draws over everything. So the drag
// ghost is not a terminal feature that happens to live here; it is the one part
// of docking that is structurally impossible anywhere else.
//
// WHAT THIS FILE DELIBERATELY DOES NOT DO
// ---------------------------------------------------------------------------
// It does NOT draw the drop-target affordance (the insertion caret, the
// highlighted tab strip). That belongs to the receiving app, which is already
// being told the pointer is over it (there is no pointer grab in this window
// manager, so the topmost window under the cursor receives EVENT_MOUSE_MOVE
// with the button held) and which is the only party that knows where its own
// tab strip is. The compositor has no idea what is inside a window and should
// not start guessing.
//
// It does NOT read the payload. sys_drag_peek() returns the LABEL and the
// metadata, never the bytes, and that is a deliberate boundary: this file peeks
// on every frame of every drag, and a terminal pane payload can carry
// scrollback, which routinely contains secrets. Drawing a caption does not
// justify handing the desktop shell the contents. Only the window the kernel
// itself resolved as the drop target can call sys_drag_take().
//
// It does NOT resolve the drop target. dragghost_release() passes the release
// POINT to the kernel, which does its own hit-test against its own z-order and
// its own record of which windows opted in. The compositor cannot name a target
// even if it wanted to.
//
// WHAT RUNS WHEN NOBODY IS DRAGGING
// ---------------------------------------------------------------------------
// Nothing. dragghost_poll() returns on a single test of the mouse buttons
// BEFORE making any syscall, because a cross-window drag cannot exist unless a
// button is physically down. With no button held: one integer test, zero
// syscalls, per frame. With a button held but no drag in flight (dragging a
// window, an icon, a sticky note, the scrollbar of an app): exactly one
// syscall, which returns -1 immediately off the kernel's own inertness flag.
// dragghost_render() returns on a static int. There is no timer, no thread and
// no allocation in this file. Nothing here can wedge the draw loop (#426): the
// only call it ever makes is a non-blocking read of kernel state.
//
// DESIGN. Per docs/TERMINAL_KONSOLE_CHROME_SPEC.md section 3 ("Drag state ->
// Ghost tab ... follows the cursor") and docs/UI_STYLE_GUIDE.md, reusing the
// volosd.c / notif.c floating-card language verbatim (SURFACE_OVERLAY fill,
// WINDOW_BORDER outline, RADIUS_CARD, the shared shadow offset and alpha) so a
// drag ghost sits at the same apparent height as every other floating card
// instead of inventing a second look.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"

// Opaque ARGB from a theme token, exactly as volosd.c / confirmdialog.c do it.
#define TC(id) (0xFF000000u | theme_color(id))

// #uiscale: GHOST_TEXT_PX is a TTF point size already covered by the
// draw_text_ttf()/text_width_ttf() chokepoint - left unscaled here.
#define GHOST_H          ui_px(28)
#define GHOST_PAD_X      ui_px(12)
#define GHOST_MIN_W      ui_px(64)
#define GHOST_MAX_W      ui_px(240)
#define GHOST_TEXT_PX    14      // on the {11,12,14,16,18,20,24,28,32} ladder
// Offset from the hotspot so the ghost trails the pointer instead of sitting
// under it. Down-right matches the cursor's own hotspot being top-left.
#define GHOST_OFF_X      ui_px(14)
#define GHOST_OFF_Y      ui_px(10)
// Spec: "65% opacity, dashed 1px outline".
#define GHOST_ALPHA     166      // 65% of 255
#define DASH_ON           4
#define DASH_OFF          3

// Cached session, refreshed by dragghost_poll(). s_active is THE gate: every
// other entry point in this file tests it first.
static int  s_active;
static int  s_x, s_y;                       // cursor at the last poll
static char s_label[DRAG_LABEL_CAP + 1];    // NUL-terminated copy of the caption

int dragghost_active(void) { return s_active; }

// Once per compositor frame. `buttons` is the live button mask (bit 0 = left).
void dragghost_poll(int mx, int my, unsigned int buttons)
{
    // INERTNESS GATE, and the reason this feature costs the idle desktop
    // nothing. A cross-window drag cannot exist unless a button is physically
    // held, so with no button down this makes NO syscall at all UNLESS we were
    // mid-drag on the previous frame - see the recovery below. It also
    // correctly stops drawing the ghost the instant the user lets go, even
    // though the kernel session may live a moment longer while the resolved
    // target claims its payload.
    if (!(buttons & 1u)) {
        if (s_active) {
            // MISSED-RELEASE RECOVERY, and the reason this is not simply
            // `s_active = 0; return;`.
            //
            // dragghost_release() normally commits the drop on the s_left_-
            // released EDGE. That edge can be LOST: process_events() returns
            // early for an exclusive modal grab (the session lock, the
            // elevation prompt), so if one of those takes the screen while a
            // button is held, this whole function stops being called and the
            // release is never seen. The kernel session would then stay ACTIVE
            // and never RELEASED, and because an unreleased session is
            // (correctly) never preempted, sys_drag_begin() would refuse EVERY
            // future drag on the machine until the source app happened to call
            // sys_drag_end() itself.
            //
            // Observing "we thought a drag was in flight, and now no button is
            // held" is a complete and clock-free description of that, so close
            // the drag out here. On the ordinary path this simply runs one
            // frame earlier than dragghost_release() would have, at the same
            // cursor position and with the same effect; dragghost_release()
            // then sees s_active == 0 and does nothing, so the drop is still
            // committed exactly once.
            s_active = 0;
            sys_drag_release(mx, my);
        }
        return;
    }

    drag_info_t info;
    if (sys_drag_peek(&info) != 0) { s_active = 0; return; }
    if (!info.active) { s_active = 0; return; }

    s_x = mx;
    s_y = my;

    // The kernel hands back a length and a fixed buffer, NOT a C string: the
    // label is whatever bytes the source app passed and is under no obligation
    // to be terminated. Terminate it here, once, so nothing downstream can run
    // off the end of it.
    int n = info.label_len;
    if (n < 0) n = 0;
    if (n > DRAG_LABEL_CAP) n = DRAG_LABEL_CAP;
    for (int i = 0; i < n; i++) {
        unsigned char c = info.label[i];
        // A control byte in a caption is either a mistake or an attempt to
        // confuse the renderer. Neither deserves to reach draw_text_ttf().
        s_label[i] = (c >= 0x20 && c < 0x7F) ? (char)c : ' ';
    }
    s_label[n] = 0;
    if (n == 0) {
        // A source that supplied no caption still gets a ghost, because the
        // ghost is the feedback that the drag was picked up at all. Silence
        // here would read as "the drag did not start".
        s_label[0] = 'I'; s_label[1] = 't'; s_label[2] = 'e'; s_label[3] = 'm';
        s_label[4] = 0;
    }

    s_active = 1;
}

// The left button came up at (x, y). Hands the release point to the kernel,
// which resolves the target itself. Safe to call unconditionally on every
// release: it is a no-op when no drag is in flight.
//
// Call this AFTER the compositor has injected MOUSE_EVENT_UP, so a target
// window receives the ordinary button-up it would have got anyway before it is
// told a payload is waiting for it.
void dragghost_release(int x, int y)
{
    if (!s_active) return;
    s_active = 0;
    sys_drag_release(x, y);
}

// A 1px dashed rectangle. There is no dashed-outline primitive in draw.c and
// this is the only caller, so it is built from the fill primitive here rather
// than widening the shared API for one user.
static void dashed_rect(int x, int y, int w, int h, uint32_t c)
{
    const int step = DASH_ON + DASH_OFF;
    for (int i = 0; i < w; i += step) {
        int run = (i + DASH_ON > w) ? (w - i) : DASH_ON;
        draw_fill_rect(x + i, y,         run, 1, c);
        draw_fill_rect(x + i, y + h - 1, run, 1, c);
    }
    for (int j = 0; j < h; j += step) {
        int run = (j + DASH_ON > h) ? (h - j) : DASH_ON;
        draw_fill_rect(x,         y + j, 1, run, c);
        draw_fill_rect(x + w - 1, y + j, 1, run, c);
    }
}

// Drawn in render_frame_body() above the windows and the taskbar (a tab can be
// dragged over both) but below the elevation modal, which must stay over
// everything.
void dragghost_render(void)
{
    if (!s_active) return;

    int tw = text_width_ttf(s_label, GHOST_TEXT_PX);
    int w  = tw + 2 * GHOST_PAD_X;
    if (w < GHOST_MIN_W) w = GHOST_MIN_W;
    if (w > GHOST_MAX_W) w = GHOST_MAX_W;

    int x = s_x + GHOST_OFF_X;
    int y = s_y + GHOST_OFF_Y;
    // Keep the whole card on screen. A ghost half off the right edge near the
    // moment of release is exactly when the user most needs to see what they
    // are carrying.
    if (x + w > g_fb_width)  x = g_fb_width - w;
    if (y + GHOST_H > g_fb_height) y = g_fb_height - GHOST_H;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    int radius = theme_metric(THEME_METRIC_RADIUS_CARD);
    uint32_t surface = TC(THEME_COLOR_SURFACE_OVERLAY);
    uint32_t accent  = TC(THEME_COLOR_ACCENT);
    uint32_t ink     = TC(THEME_COLOR_ON_SURFACE);

    // Shadow first, at the notif.c/volosd.c offset and alpha, so a ghost reads
    // as floating at the same height as every other card in the system.
    g_draw_blend = 70;
    draw_rounded_rect(x + 3, y + 4, w, GHOST_H, radius, 0xFF000000u);

    // The card itself is translucent, which is what makes it read as a ghost
    // rather than as a real window that has appeared under the cursor.
    g_draw_blend = GHOST_ALPHA;
    draw_rounded_rect(x, y, w, GHOST_H, radius, surface);
    g_draw_blend = 255;

    // Dashed accent outline: the spec's "lifted, not yet dropped" signal.
    dashed_rect(x, y, w, GHOST_H, accent);

    // Caption, clipped to the card so a long label cannot bleed past the
    // rounded edge and paint over the desktop.
    int fm[3];
    int th = GHOST_TEXT_PX;
    // #uiscale: scale to match draw_text_ttf()/text_width_ttf(), which scale internally.
    if (font_metrics(0, ui_px(GHOST_TEXT_PX), fm) == 0) th = fm[0] - fm[1];
    draw_push_clip(x + GHOST_PAD_X, y, w - 2 * GHOST_PAD_X, GHOST_H);
    draw_text_ttf(x + GHOST_PAD_X, y + (GHOST_H - th) / 2,
                  s_label, GHOST_TEXT_PX, ink);
    draw_pop_clip();
}
