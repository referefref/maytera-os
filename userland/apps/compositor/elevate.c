// elevate.c - #745 THE ELEVATION MODAL (Surface B of docs/APPSTORE_ELEVATION.html).
//
// ===========================================================================
// WHY THIS FILE IS IN THE COMPOSITOR AND NOT IN THE APP STORE
// ---------------------------------------------------------------------------
// A consent dialog drawn by the process that wants the privilege is worth
// nothing: it can draw anything and lie about the answer. So the App Store
// never draws this, never receives a keystroke while it is up, and never sees
// the password. It sends a request to the KERNEL (SYS_ELEV_REQUEST) and later
// reads a verdict; everything in between happens here and in proc/elevate.c.
//
// It has to be SYSTEM-modal rather than app-modal, and that is a measured
// constraint rather than a preference: kernel/gui/window.h has no modal and no
// topmost flag, and z-order is a bare counter bumped on focus, so NOTHING in
// the window manager can pin a window above another. A window claiming to be
// modal would be covered by the next window that takes focus.
//
// The compositor already implements a true modal three times - lock_enter(),
// startmenu_power_confirm_render() and the widget settings dialog - and already
// owns the whole input stream via grab_input(1) in main.c. This is the FOURTH
// one, built on that same machinery, not a parallel mechanism: it draws
// straight to the framebuffer under the same full-screen scrim idiom the lock
// screen uses, and it reuses lock_draw_pill() VERBATIM for the password field
// (which is why that function stopped being static).
//
// WHAT AN APP CANNOT DO, and what it still can:
//   * it cannot draw this. is_compositor() in kernel/gui/fb_syscall.c latches
//     the first process to map the framebuffer and rejects every other pid, so
//     the compositor is a principal an app cannot become.
//   * it cannot be covered while it is up, and no other app receives a single
//     keystroke while it is up.
//   * it cannot influence x or y: the panel is screen-centred from the
//     framebuffer size. An app-positioned prompt looks like it belongs to the
//     app and lets an attacker line a fake up over the real one.
//   * it CAN still paint a convincing fake in its own window if it is
//     fullscreen, and no pixel-level marker fixes that. The honest claim is
//     that the real prompt is rare (per-user install never prompts, root never
//     prompts, and the kernel refuses a prompt not raised in response to
//     recent dispatched input), un-obscurable while real, and audited
//     afterwards. A secure attention sequence is the complete fix and is a
//     follow-on, not a claim this file makes.
// ===========================================================================
#include "compositor.h"
#include "../../libc/string.h"   // #73 secure_zero
#include "syscall.h"
#include "string.h"

// ---- the mechanism, mirrored from kernel/proc/elevate.h -------------------
// These constants cross Ring 0 <-> Ring 3. libc/syscall.h carries the syscall
// NUMBERS (and the syscall-number-lint checks them against the kernel header);
// the shapes below are checked by _Static_assert on sizeof, because a struct
// that grows on one side and not the other reads garbage with no diagnostic.
#define ELEV_ST_IDLE     0
#define ELEV_ST_OPEN     1
#define ELEV_ST_GRANTED  2
#define ELEV_ST_DENIED   3
#define ELEV_ACT_CANCEL   0
#define ELEV_ACT_SUBMIT   1
#define ELEV_ACT_LOCKSECS 2
#define ELEV_EATTEMPTS (-6)
#define ELEV_ELOCKED   (-7)

typedef struct {
    unsigned long long seq;
    unsigned long long opened_ms;
    unsigned int state, req_pid, req_uid, attempts_used, attempts_max, pad;
    char name[64];
    char version[32];
    char source[64];
    char dest[96];
} elev_view_t;
_Static_assert(sizeof(elev_view_t) == 296,
               "#745: elev_view_t must match kernel/proc/elevate.h (296 bytes)");

// ---- palette -------------------------------------------------------------
// Every colour below except the panel fill is an EXISTING compositor
// CLR_LOGIN_* constant, so this surface is theme-invariant exactly as the lock
// screen and the boot gate already are. That is deliberate and it is the
// point: "the system is asking me to prove who I am" should always look the
// same, because this is the surface people have to learn to recognise.
//
// ELEV_PANEL_BG is the one addition (design B-1). It is 100% OPAQUE: an
// elevation prompt is the last surface in the OS whose effective background
// should be a blend result that has to be re-derived every time the wallpaper
// changes. Opaque means its ink is measured against its own flat fill, once.
#define ELEV_PANEL_BG   0xFF151A24
#define ELEV_SCRIM      0xFF060910
#define ELEV_SCRIM_A    145
#define ELEV_SHADOW     0xFF000000

// Keys must be discarded for this long after the panel is drawn, so a burst of
// buffered keystrokes typed before it appeared cannot land in the field, and
// so a click cannot be timed through the appearance.
#define ELEV_SETTLE_MS  250

#define PW_MAX 128

typedef struct {
    int sw, sh;
    int px, py, pw, ph;       // panel
    int compact;
    int title_px, body_px, fine_px, pill_px;
    int cx;                   // content x (panel-relative + px)
    int cw;                   // content width
    int rule1_y, sub_y, rows_y, row_pitch, rule2_y, fine_y, fine2_y;
    int credlab_y, fx, fy, fw, fh;
    int err_y, err_h;
    int b_y, b_h;
    int cancel_x, cancel_w, act_x, act_w;
} elev_geom_t;

static int      g_elev_open;            // 1 while the modal owns the screen
static elev_view_t g_elev;              // the facts, as the KERNEL sanitised them
static char     g_elev_pw[PW_MAX + 1];
static int      g_elev_pwlen;
static int      g_elev_focus;           // 0 Cancel, 1 field, 2 elevating action
static char     g_elev_errs[80];
static int      g_elev_errflag;         // hold the pill's error border until the next keystroke
static int      g_elev_locked;          // B4: lockout state, field inert
static unsigned long long g_elev_shown_ms;
static int      g_elev_caret;

int  elevate_modal_open(void) { return g_elev_open; }

static void elev_geom(elev_geom_t *g)
{
    fb_info_t fi;
    fb_info(&fi);
    g->sw = (int)fi.width;
    g->sh = (int)fi.height;

    // COMPACT below 800x600. It never scrolls: an elevation prompt with content
    // below the fold is not a prompt.
    g->compact = (g->sh < 600 || g->sw < 800);
    if (!g->compact) {
        g->pw = 520; g->ph = 468;
        g->title_px = 24; g->body_px = 16; g->fine_px = 14; g->pill_px = 18;
        g->cx = 24; g->cw = 472;
        g->rule1_y = 66; g->sub_y = 78;
        g->rows_y = 114; g->row_pitch = 28;
        g->rule2_y = 236; g->fine_y = 248; g->fine2_y = 270;
        g->credlab_y = 300; g->fy = 328; g->fh = 42;
        g->err_y = 384; g->err_h = 26;
        g->b_y = 410; g->b_h = 34;
        g->cancel_x = 174; g->cancel_w = 104;
        g->act_x = 288; g->act_w = 208;
    } else {
        g->pw = 592; g->ph = 386;
        g->title_px = 20; g->body_px = 14; g->fine_px = 12; g->pill_px = 18;
        g->cx = 16; g->cw = 560;
        g->rule1_y = 50; g->sub_y = 60;
        g->rows_y = 90; g->row_pitch = 24;
        g->rule2_y = 192; g->fine_y = 202; g->fine2_y = -1;   // one fine-print line
        g->credlab_y = 232; g->fy = 256; g->fh = 36;
        g->err_y = 304; g->err_h = 22;
        g->b_y = 340; g->b_h = 30;
        g->cancel_x = 352; g->cancel_w = 88;
        g->act_x = 448; g->act_w = 128;
    }
    g->fx = g->cx;
    g->fw = g->cw;

    // Screen-centred, NEVER anchored to the requesting window.
    g->px = (g->sw - g->pw) / 2;
    if (g->px < 0) g->px = 0;
    int top = g->compact ? 34 : 40;
    int bot = g->sh - g->ph - (g->compact ? 12 : 16);
    int y = ((g->sh - g->ph) * 38) / 100;
    if (y < top) y = top;
    if (bot >= top && y > bot) y = bot;
    if (y < 0) y = 0;
    g->py = y;
}

static void elev_btn(int x, int y, int w, int h, const char *label, int filled,
                     int focused, int px)
{
    if (filled) draw_fill_rect(x, y, w, h, CLR_LOGIN_PANEL);
    // Square corners: GUI_BTN_RADIUS_FALLBACK is 0 house-wide.
    draw_fill_rect(x, y, w, 1, CLR_LOGIN_DIMMED);
    draw_fill_rect(x, y + h - 1, w, 1, CLR_LOGIN_DIMMED);
    draw_fill_rect(x, y, 1, h, CLR_LOGIN_DIMMED);
    draw_fill_rect(x + w - 1, y, 1, h, CLR_LOGIN_DIMMED);
    int tw = text_width_ttf(label, px);
    draw_text_ttf(x + (w - tw) / 2, y + (h - px) / 2 - 1, label, px, CLR_LOGIN_TEXT);
    if (focused) {
        // OUTSIDE the control, on the panel fill. Inside the Cancel fill the
        // same ring measures 3.52:1 in this palette and 2.14:1 in a light one;
        // outside it is always measured against one known background (5.58:1).
        // The 2px gap is a SHAPE change as well as a colour change, so focus is
        // not signalled by colour alone.
        int rx = x - 4, ry = y - 4, rw = w + 8, rh = h + 8;
        draw_fill_rect(rx, ry, rw, 2, CLR_LOGIN_ACCENT);
        draw_fill_rect(rx, ry + rh - 2, rw, 2, CLR_LOGIN_ACCENT);
        draw_fill_rect(rx, ry, 2, rh, CLR_LOGIN_ACCENT);
        draw_fill_rect(rx + rw - 2, ry, 2, rh, CLR_LOGIN_ACCENT);
    }
}

static void elev_row(const elev_geom_t *g, int y, const char *k, const char *v)
{
    int labw = g->compact ? 116 : 140;
    draw_text_ttf(g->px + g->cx, g->py + y, k, g->body_px, CLR_LOGIN_DIMMED);
    draw_text_ttf(g->px + g->cx + labw, g->py + y, v, g->body_px, CLR_LOGIN_TEXT);
}

void elevate_render(void)
{
    if (!g_elev_open) return;
    elev_geom_t g;
    elev_geom(&g);

    // The scrim covers EVERYTHING: wallpaper, every window, the panel bar and
    // the taskbar. Nothing on screen is interactive while this is up, so
    // nothing on screen should look interactive. Same idiom, same constants as
    // lockscreen.c:495.
    g_draw_blend = ELEV_SCRIM_A;
    draw_fill_rect(0, 0, g.sw, g.sh, ELEV_SCRIM);
    g_draw_blend = 255;

    // Panel: opaque fill, then the two-tone edge. The pixel behind the edge is a
    // SCRIM BLEND and is different for every wallpaper, so a single edge colour
    // cannot survive both extremes: over pure white the black outer edge carries
    // it at 4.42:1, over pure black the light inner edge carries it at 7.87:1.
    draw_fill_rect(g.px + 3, g.py + 3, g.pw, g.ph, ELEV_SHADOW);
    draw_fill_rect(g.px, g.py, g.pw, g.ph, ELEV_PANEL_BG);
    draw_fill_rect(g.px, g.py, g.pw, 1, 0xFF000000);
    draw_fill_rect(g.px, g.py + g.ph - 1, g.pw, 1, 0xFF000000);
    draw_fill_rect(g.px, g.py, 1, g.ph, 0xFF000000);
    draw_fill_rect(g.px + g.pw - 1, g.py, 1, g.ph, 0xFF000000);
    draw_fill_rect(g.px + 1, g.py + 1, g.pw - 2, 1, CLR_LOGIN_DIMMED);
    draw_fill_rect(g.px + 1, g.py + g.ph - 2, g.pw - 2, 1, CLR_LOGIN_DIMMED);
    draw_fill_rect(g.px + 1, g.py + 1, 1, g.ph - 2, CLR_LOGIN_DIMMED);
    draw_fill_rect(g.px + g.pw - 2, g.py + 1, 1, g.ph - 2, CLR_LOGIN_DIMMED);

    // TITLE: FIXED CHROME. It never contains the package name, by rule, so the
    // first thing the eye lands on is a sentence the requesting app did not
    // write and cannot influence.
    draw_text_ttf(g.px + g.cx, g.py + (g.compact ? 16 : 24),
                  "Install for all users", g.title_px, CLR_LOGIN_TEXT);
    draw_fill_rect(g.px + g.cx, g.py + g.rule1_y, g.cw, 1, CLR_LOGIN_DIMMED);
    draw_text_ttf(g.px + g.cx, g.py + g.sub_y,
                  "This affects every account on this computer.",
                  g.body_px, CLR_LOGIN_DIMMED);

    // THE FACT ROWS: the ONLY place package-supplied text appears anywhere on
    // this surface. The kernel sanitised it (control bytes and non-ASCII
    // dropped, truncated to 40 glyphs) before this process ever saw it.
    char pkg[100];
    strncpy(pkg, g_elev.name, sizeof(pkg) - 1); pkg[sizeof(pkg) - 1] = 0;
    if (g_elev.version[0]) {
        strncat(pkg, " ", sizeof(pkg) - strlen(pkg) - 1);
        strncat(pkg, g_elev.version, sizeof(pkg) - strlen(pkg) - 1);
    }
    int ry = g.rows_y;
    elev_row(&g, ry, "Package",     pkg);                 ry += g.row_pitch;
    elev_row(&g, ry, "Source",      g_elev.source[0] ? g_elev.source : "unknown");
    ry += g.row_pitch;
    elev_row(&g, ry, "Installs to", g_elev.dest);         ry += g.row_pitch;
    elev_row(&g, ry, "Runs as",     "root");

    draw_fill_rect(g.px + g.cx, g.py + g.rule2_y, g.cw, 1, CLR_LOGIN_DIMMED);

    // Fine print. Line 1 separates INSTALLER privilege from APP privilege,
    // which people conflate constantly and is the one thing this dialog can
    // usefully teach. Line 2 states the durable consequence, including how hard
    // it is to undo.
    if (!g.compact) {
        draw_text_ttf(g.px + g.cx, g.py + g.fine_y,
                      "The installer runs as root to write to /APPS. The app itself will run as you.",
                      g.fine_px, CLR_LOGIN_DIMMED);
        draw_text_ttf(g.px + g.cx, g.py + g.fine2_y,
                      "Every user will be able to run it. Only an administrator can remove it.",
                      g.fine_px, CLR_LOGIN_DIMMED);
    } else {
        draw_text_ttf(g.px + g.cx, g.py + g.fine_y,
                      "The installer runs as root. Only an administrator can remove it.",
                      g.fine_px, CLR_LOGIN_DIMMED);
    }

    draw_text_ttf(g.px + g.cx, g.py + g.credlab_y,
                  g_elev_locked ? "Elevation is temporarily locked"
                                : "Enter your password to continue",
                  g.body_px, CLR_LOGIN_TEXT);

    // The password field is the SHIPPED primitive, verbatim: the draw_bullet()
    // run at a 12px pitch, the 2px caret, the boundary pair. #745: that
    // primitive is SQUARE-EDGED now (the user asked for the sign-in boxes'
    // corners to be removed rather than antialiased), so the focus ring drawn
    // around it below is square too - a rounded ring around a square field
    // reads as a rendering bug, and this file inherits the change because it
    // shares the one credential-field implementation on purpose.
    // arrow_sz = 0: there is no submit arrow here, the footer button is submit.
    if (!g_elev_locked) {
        lock_draw_pill(g.px + g.fx, g.py + g.fy, g.fw, g.fh, g_elev_pw, 1,
                       g_elev_focus == 1, g_elev_caret, g_elev_errflag,
                       "Password", 0);
        if (g_elev_focus == 1) {
            int rx = g.px + g.fx - 4, ry2 = g.py + g.fy - 4;
            int rw = g.fw + 8, rh = g.fh + 8;
            draw_rect_outline(rx, ry2, rw, rh, CLR_LOGIN_ACCENT);
            draw_rect_outline(rx + 1, ry2 + 1, rw - 2, rh - 2, CLR_LOGIN_ACCENT);
            lock_draw_pill(g.px + g.fx, g.py + g.fy, g.fw, g.fh, g_elev_pw, 1,
                           1, g_elev_caret, g_elev_errflag, "Password", 0);
        }
    }

    // The error row's height is RESERVED whether or not there is a message, so
    // the footer never moves when a wrong password appears. Same idiom as
    // lock_geom().
    if (g_elev_errs[0])
        draw_text_ttf(g.px + g.cx, g.py + g.err_y, g_elev_errs,
                      g.body_px, CLR_LOGIN_ERROR);

    if (g_elev_locked) {
        // B4: both actions collapse to one, and it is Close, not Cancel: there
        // is nothing left to cancel.
        elev_btn(g.px + g.act_x, g.py + g.b_y, g.act_w, g.b_h, "Close", 1, 1, g.body_px);
    } else {
        // THE SAFE ACTION IS THE FILLED ONE. The elevating action is outline
        // only, and deliberately NOT red and not a warning colour: the dialog
        // says what will happen, the colour does not have to.
        elev_btn(g.px + g.cancel_x, g.py + g.b_y, g.cancel_w, g.b_h, "Cancel",
                 1, g_elev_focus == 0, g.body_px);
        elev_btn(g.px + g.act_x, g.py + g.b_y, g.act_w, g.b_h,
                 g.compact ? "Install" : "Install for all users",
                 0, g_elev_focus == 2, g.body_px);
    }
}

static void elev_close(void)
{
    g_elev_open = 0;
    // Zero the buffer on EVERY exit, including cancel and lockout.
    secure_zero(g_elev_pw, sizeof(g_elev_pw));   /* #73: was memset, which -O2 may elide */
    g_elev_pwlen = 0;
    g_elev_errs[0] = 0;
    g_elev_errflag = 0;
    g_elev_locked = 0;
    g_needs_redraw = true;
}

static void elev_set_attempts_msg(int left)
{
    char n[8];
    int i = 0;
    if (left < 0) left = 0;
    n[i++] = (char)('0' + (left % 10)); n[i] = 0;
    strcpy(g_elev_errs, "Incorrect password. ");
    strncat(g_elev_errs, n, sizeof(g_elev_errs) - strlen(g_elev_errs) - 1);
    strncat(g_elev_errs, left == 1 ? " attempt left." : " attempts left.",
            sizeof(g_elev_errs) - strlen(g_elev_errs) - 1);
}

static void elev_submit(void)
{
    if (g_elev_pwlen == 0) return;   // no-op, no error, no flash
    int r = (int)sys_elev_resolve(g_elev.seq, ELEV_ACT_SUBMIT, g_elev_pw);
    secure_zero(g_elev_pw, sizeof(g_elev_pw));   /* #73: was memset, which -O2 may elide */
    g_elev_pwlen = 0;
    if (r == 0) { elev_close(); return; }             // granted: no success dialog
    if (r == ELEV_EATTEMPTS) { elev_close(); return; } // three used: abandoned
    if (r == ELEV_ELOCKED) {
        int secs = (int)sys_elev_resolve(g_elev.seq, ELEV_ACT_LOCKSECS, 0);
        char n[12]; int i = 0, v = secs < 0 ? 0 : secs;
        char tmp[12]; int t = 0;
        if (v == 0) tmp[t++] = '0';
        while (v > 0 && t < 11) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
        while (t > 0) n[i++] = tmp[--t];
        n[i] = 0;
        strcpy(g_elev_errs, "Too many attempts. Try again in ");
        strncat(g_elev_errs, n, sizeof(g_elev_errs) - strlen(g_elev_errs) - 1);
        strncat(g_elev_errs, " seconds.", sizeof(g_elev_errs) - strlen(g_elev_errs) - 1);
        g_elev_locked = 1;
        g_elev_errflag = 1;
        g_needs_redraw = true;
        return;
    }
    if (r > 0) {
        // "Incorrect password." and nothing else. There is no username field on
        // this dialog, so there is nothing to be wrong about and nothing to
        // leak: the message is identical whatever the account is.
        elev_set_attempts_msg(r);
        g_elev_errflag = 1;
        g_elev_focus = 1;
        g_needs_redraw = true;
        return;
    }
    elev_close();   // ESTALE or anything unexpected: fail closed, never approve
}

// Returns 1 if the key was consumed. Called from process_events() at the
// highest priority after the session-lock gate, so nothing else in the
// compositor and no app window can see a key while this is up.
int elevate_handle_key(int key)
{
    if (!g_elev_open) return 0;

    // INPUT SETTLING. Everything typed in the first 250ms after the panel is
    // drawn is discarded, so a buffered keystroke run cannot land in the field
    // and a click cannot be timed through the appearance.
    if (uptime_ms() - g_elev_shown_ms < ELEV_SETTLE_MS) return 1;

    // ESC CANCELS FROM EVERY STATE, ALWAYS, including mid-authentication and
    // during lockout. Non-negotiable: Esc is the reflex exit and must always
    // land on the safe outcome.
    if (key == 0x1B) {
        sys_elev_resolve(g_elev.seq, ELEV_ACT_CANCEL, 0);
        elev_close();
        return 1;
    }

    if (g_elev_locked) {
        // Only Esc (above) and Enter act; both close. The field is inert.
        if (key == '\n' || key == '\r') {
            sys_elev_resolve(g_elev.seq, ELEV_ACT_CANCEL, 0);
            elev_close();
        }
        return 1;
    }

    if (key == '\t') {
        // Ring: password field -> Cancel -> Install for all users -> wrap.
        // Tab cycles FORWARD only. Shift+Tab is not implemented because the
        // keyboard path delivers 0x09 for both, so a reverse binding would be
        // indistinguishable from a forward one; with three stops and a wrap,
        // every control is reachable either way.
        g_elev_focus = (g_elev_focus == 1) ? 0 : (g_elev_focus == 0 ? 2 : 1);
        g_needs_redraw = true;
        return 1;
    }

    if (key == '\n' || key == '\r') {
        // ENTER IS DELIBERATELY EXCLUDED FROM TYPE-AHEAD. Initial focus is
        // Cancel, so an Enter buffered from BEFORE the dialog appeared cancels;
        // it must never be promoted into an approval by the type-ahead rule.
        if (g_elev_focus == 0) {
            sys_elev_resolve(g_elev.seq, ELEV_ACT_CANCEL, 0);
            elev_close();
        } else if (g_elev_focus == 1) {
            elev_submit();
        } else {
            if (g_elev_pwlen == 0) { g_elev_focus = 1; g_needs_redraw = true; }
            else elev_submit();
        }
        return 1;
    }

    if (key == '\b' || key == 127) {
        if (g_elev_focus == 1 && g_elev_pwlen > 0) {
            g_elev_pw[--g_elev_pwlen] = 0;
            g_elev_errflag = 0;
            g_needs_redraw = true;
        }
        return 1;
    }

    // Arrows / navigation: no-op. There is no caret movement inside a masked
    // field and no list to traverse.
    if (key >= 0x80) return 1;

    if (key == ' ' && g_elev_focus == 0) {
        sys_elev_resolve(g_elev.seq, ELEV_ACT_CANCEL, 0);
        elev_close();
        return 1;
    }
    if (key == ' ' && g_elev_focus == 2) { elev_submit(); return 1; }

    if (key >= 0x20 && key < 0x7F) {
        // TYPE-AHEAD. Any printable key moves focus to the field and inserts
        // itself, which is what pays for initial focus being on Cancel: "click
        // the button, start typing" works with no extra keypress.
        g_elev_focus = 1;
        g_elev_errflag = 0;
        if (g_elev_pwlen < PW_MAX) {
            g_elev_pw[g_elev_pwlen++] = (char)key;
            g_elev_pw[g_elev_pwlen] = 0;
        }
        g_needs_redraw = true;
        return 1;
    }
    return 1;   // swallow everything else: this is a modal
}

// Mouse is the ALTERNATIVE path, never the primary one (#334: pointer
// injection does not reliably land clicks on this platform, so every state
// above is reachable with the mouse unplugged).
int elevate_handle_mouse(int32_t x, int32_t y, int clicked)
{
    if (!g_elev_open) return 0;
    if (!clicked) return 1;
    if (uptime_ms() - g_elev_shown_ms < ELEV_SETTLE_MS) return 1;
    elev_geom_t g;
    elev_geom(&g);
    int by = g.py + g.b_y;
    if (y >= by && y < by + g.b_h) {
        if (g_elev_locked) {
            if (x >= g.px + g.act_x && x < g.px + g.act_x + g.act_w) {
                sys_elev_resolve(g_elev.seq, ELEV_ACT_CANCEL, 0);
                elev_close();
            }
            return 1;
        }
        if (x >= g.px + g.cancel_x && x < g.px + g.cancel_x + g.cancel_w) {
            sys_elev_resolve(g_elev.seq, ELEV_ACT_CANCEL, 0);
            elev_close();
            return 1;
        }
        if (x >= g.px + g.act_x && x < g.px + g.act_x + g.act_w) {
            if (g_elev_pwlen == 0) { g_elev_focus = 1; g_needs_redraw = true; }
            else elev_submit();
            return 1;
        }
    }
    if (!g_elev_locked && y >= g.py + g.fy && y < g.py + g.fy + g.fh &&
        x >= g.px + g.fx && x < g.px + g.fx + g.fw) {
        g_elev_focus = 1;
        g_needs_redraw = true;
    }
    return 1;
}

// Called once per frame from the main loop. The kernel is the authority on
// whether a request exists; this only mirrors it. That is also the watchdog:
// SYS_ELEV_VIEW runs the kernel-side tick, so if the requesting app dies or the
// request expires, the view goes empty on the next frame and the grab and the
// scrim are dropped here. A crashed helper can never leave an undismissable
// scrim over an unusable desktop.
void elevate_poll(void)
{
    elev_view_t v;
    memset(&v, 0, sizeof(v));
    long r = sys_elev_view(&v);
    if (r != 1) {
        if (g_elev_open) elev_close();
        return;
    }
    if (!g_elev_open || v.seq != g_elev.seq) {
        g_elev = v;
        g_elev_open = 1;
        secure_zero(g_elev_pw, sizeof(g_elev_pw));   /* #73: was memset, which -O2 may elide */
        g_elev_pwlen = 0;
        g_elev_errs[0] = 0;
        g_elev_errflag = 0;
        g_elev_locked = 0;
        // INITIAL FOCUS IS CANCEL. A keystroke buffered from before the dialog
        // appeared therefore CANCELS; it can never approve. That is the whole
        // reason for this choice, and type-ahead is what pays for it.
        g_elev_focus = 0;
        g_elev_shown_ms = uptime_ms();
        g_needs_redraw = true;
    } else {
        g_elev = v;
    }
    // Caret blink, driven from the same clock as everything else here.
    g_elev_caret = ((uptime_ms() - g_elev_shown_ms) / 500) % 2 == 0;
}
