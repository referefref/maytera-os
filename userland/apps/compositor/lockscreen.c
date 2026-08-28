// lockscreen.c - Secure session lock overlay for the MayteraOS compositor
// (#566, docs/SECURE_LOGIN_DESIGN.md sections 3.2/3.4/4.2, mockup
// docs/mockups/login-mockup.html "lock screen" tab).
//
// SECURITY MODEL (this is the whole point of the file, see 3.2/3.4 of the
// design doc): the KERNEL is the sole authority for "is this session locked"
// (SYS_SESSION_IS_LOCKED). g_session_locked is a DISPLAY CACHE, refreshed
// from the kernel every main-loop iteration by lock_poll() - it is never
// itself the decision. Unlocking happens ONLY via sys_session_unlock()
// returning 0 (a real, rate-limited kernel password check); there is no ESC,
// no click-away, no local bypass. main.c's process_input()/process_events()
// gate on g_session_locked to make sure that while locked:
//   - process_input() does NOT call sys_inject_key() toward any app window
//     (it still reads the raw key so THIS file can see it).
//   - process_events() routes 100% of key/mouse events to lock_handle_key()/
//     lock_handle_mouse() and returns immediately - no sys_inject_mouse(),
//     no desktop-icon/taskbar/start-menu hit-testing.
//   - render_frame() renders ONLY this overlay (see main.c) - the screensaver,
//     notification toasts, and every other overlay are unreachable while
//     locked (short-circuited before they are ever drawn or ticked).
//
// VISUAL (#569 redesign, superseding the earlier panel mockup): NO panel and no
// box chrome. A FULL-RESOLUTION wallpaper that is only DARKENED (the old block
// mosaic is gone - it is what made the wallpaper look pixelated), with the
// session user's profile icon + name, a rounded-corner masked password field
// with an arrow button on its right, a large live clock (the liveness
// indicator: it must visibly tick across two screenshots, see blame.md), and
// Switch User / Restart / Shut Down as high-contrast shadowed TEXT with no
// surrounds. No notification count is shown at all.

#include "compositor.h"
#include "../../libc/string.h"   // #73 secure_zero
#include "../../libc/notify.h"   // #745: NOTIFY_WARNING for a declined lock
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"

bool g_session_locked = false;   // #566 display cache - see compositor.h

#define LOCK_MAX_PASSWORD LOGIN_MAX_PASSWORD

static char     g_lock_password[LOCK_MAX_PASSWORD];
static int      g_lock_password_len;
static int      g_lock_cursor_blink;
static int      g_lock_blink_counter;
static char     g_lock_error[80];
// #745 typed mode: the name the person supplied. In LIST mode this stays
// empty and sys_session_unlock() resolves the session user itself, which is
// the sanctioned call and the one that cannot be broken by a bad name.
#define LOCK_MAX_NAME 64
static char     g_lock_name[LOCK_MAX_NAME];
static int      g_lock_name_len;
static int      g_lock_field;      // 0 = name (typed mode only), 1 = password
// The configured sign-in screen mode. Read from the KERNEL, not from
// /CONFIG/LOGIN.CFG: that file is 0600 root:root and this process is the
// session user, so opening it fails at the permission check, not the parse.
// Refreshed at lock_init() and every time a lock actually takes - NOT per
// frame, which would be a syscall that reads a FAT file on every redraw
// (#426). Once per lock is sufficient: the mode cannot usefully change
// between the screen appearing and the person typing into it.
static int      s_login_mode = LOGIN_MODE_TYPED;

static void lock_refresh_mode(void)
{
    s_login_mode = sys_get_login_mode();
}

// ----------------------------------------------------------------------------
// /CONFIG/PRIVACY.CFG polling (idle-timeout lock, #566 4.2): Settings already
// persists "screen_lock" (bool) and "lock_timeout" (minutes, 0=Never) as real
// key=value settings to this file (see userland/apps/settings/main.c
// privacy_save()/privacy_load() - its own comment already says "the
// screen-lock values are readable by the lock subsystem"). No kernel change
// and no new file: this is the project's standard flat-file service hand-off
// (see CLAUDE.md), read on a throttle so this module self-refreshes.
// ----------------------------------------------------------------------------
#define PRIVACY_CFG "/CONFIG/PRIVACY.CFG"

static int cfg_find_int(const char *buf, const char *key, int defval)
{
    unsigned long klen = strlen(key);
    unsigned long blen = strlen(buf);
    if (klen >= blen) return defval;
    for (unsigned long i = 0; i + klen <= blen; i++) {
        if (strncmp(buf + i, key, klen) == 0) {
            unsigned long j = i + klen;
            int neg = 0;
            if (buf[j] == '-') { neg = 1; j++; }
            int v = 0, any = 0;
            while (buf[j] >= '0' && buf[j] <= '9') { v = v * 10 + (buf[j] - '0'); j++; any = 1; }
            if (any) return neg ? -v : v;
        }
    }
    return defval;
}

static int      s_lock_enabled     = 1;
static int      s_lock_timeout_min = 5;
static uint64_t s_privacy_poll_ms  = 0;
static int      s_idle_lock_tried  = 0;   // one idle-lock attempt per idle period (#566)

static void privacy_cfg_poll(void)
{
    uint64_t now = uptime_ms();
    if (now - s_privacy_poll_ms < 2000) return;   // throttle: at most every 2s
    s_privacy_poll_ms = now;

    int fd = sys_open(PRIVACY_CFG, 0);
    if (fd < 0) return;
    char buf[256];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';
    s_lock_enabled     = cfg_find_int(buf, "screen_lock=", 1);
    s_lock_timeout_min = cfg_find_int(buf, "lock_timeout=", 5);
}

// ----------------------------------------------------------------------------
// Public lifecycle
// ----------------------------------------------------------------------------

void lock_init(void)
{
    // In case a prior compositor life on this boot left the kernel flag set
    // (should not normally happen - a fresh session starts unlocked - but the
    // kernel flag is authoritative, so read it rather than assume).
    g_session_locked     = (sys_session_is_locked() == 1);
    g_lock_password_len  = 0;
    secure_zero(g_lock_password, sizeof(g_lock_password));   /* #73 */
    g_lock_name_len      = 0;
    g_lock_name[0]       = '\0';
    g_lock_error[0]      = '\0';
    g_lock_cursor_blink  = 1;
    g_lock_blink_counter = 0;
    lock_refresh_mode();
    g_lock_field = (s_login_mode == LOGIN_MODE_TYPED) ? 0 : 1;
}

// #745: establish who this session is from the KERNEL, not from a constant.
//
// sys_geteuid() is the identity the kernel gave this process, and the
// compositor is spawned proc_as_session(), so it IS the session uid by
// construction. The NAME is then looked up for DISPLAY only.
//
// The split matters and is the whole design: the security-critical path
// (unlock, lockout query) passes "" and lets the kernel resolve the session
// user itself, so it CANNOT be broken by this lookup failing. If the name
// lookup fails, the lock screen shows a fallback label and the user can still
// unlock. There is no failure mode here that costs anyone their session.
void session_identity_init(void)
{
    g_login_uid = (int)geteuid();
    g_login_username[0] = '\0';

    user_info_t ui[16];
    int n = sys_list_users(ui, 16);
    for (int i = 0; i < n; i++) {
        if ((int)ui[i].uid == g_login_uid) {
            int j = 0;
            while (ui[i].username[j] && j < 63) { g_login_username[j] = ui[i].username[j]; j++; }
            g_login_username[j] = '\0';
            break;
        }
    }
    if (!g_login_username[0]) {
        // Cosmetic fallback only. Never fatal: see the note above.
        snprintf(g_login_username, sizeof(g_login_username), "uid%d", g_login_uid);
    }
    sys_bootlog("compositor: session identity resolved from the kernel");
}

void lock_enter(void)
{
    lock_enter_reason(SESSION_LOCK_EXPLICIT);
}

void lock_enter_reason(int reason)
{
    // Request a lock. The KERNEL is the authority and MAY REFUSE. Two reasons
    // it can refuse, and they are not the same thing (#745):
    //
    //   - SESSION_LOCK_IDLE on an autologin box: expected, by design (#566), a
    //     macOS-style autologin session must not idle-lock itself out.
    //   - the session user has no usable password: the kernel refuses to lock a
    //     session that could never be unlocked. That is a protection, and the
    //     user needs to be TOLD, because "I clicked Lock and nothing happened"
    //     is otherwise indistinguishable from a bug.
    //
    // We are only a DISPLAY CACHE, so take the lock state from what the kernel
    // actually did, never assume success.
    int rc = sys_session_lock(reason);
    g_session_locked = (sys_session_is_locked() == 1);
    if (!g_session_locked) {
        if (rc != 0 && reason == SESSION_LOCK_EXPLICIT) {
            // An explicit request is only ever declined for the no-credential
            // reason now, so this is worth surfacing rather than logging.
            notify_post("Screen Lock",
                        "This account has no password set, so the screen cannot be locked.",
                        NOTIFY_WARNING);
        }
        sys_bootlog("compositor: lock declined by kernel - staying unlocked");
        return;
    }
    g_lock_password_len = 0;
    secure_zero(g_lock_password, sizeof(g_lock_password));   /* #73 */
    g_lock_name_len     = 0;
    g_lock_name[0]      = '\0';
    g_lock_error[0]     = '\0';
    g_lock_cursor_blink  = 1;
    g_lock_blink_counter = 0;
    // #745: re-read the mode as the lock takes, so a change made in Settings
    // during this session is honoured the next time the screen appears.
    lock_refresh_mode();
    g_lock_field = (s_login_mode == LOGIN_MODE_TYPED) ? 0 : 1;

    // The lock overlay must be the ONLY thing on screen while active (#566
    // 3.2). Force-close overlays that would otherwise linger underneath it
    // (and would look wrong if they reappeared unexpectedly right after
    // unlock, since render_frame() skips drawing everything else while
    // locked - see main.c).
    g_start_menu_open      = false;
    g_context_menu_open    = false;
    g_wallpaper_picker_open = false;
    g_tray_menu_open       = 0;
    if (g_launcher_open) launcher_close();

    sys_bootlog("compositor: session locked");
    g_needs_redraw = true;
}

// Submit the typed password. Always clears the typed buffer afterwards
// (success or failure) - a failed attempt must not leave the password
// sitting in the field.
static void do_unlock(void)
{
    // #745: in LIST mode "" means THE SESSION USER. The compositor does not name
    // the account it is trying to unlock, because it is not the authority on
    // that and never was: the kernel checked whatever name we sent against the
    // real session user anyway, so supplying one could only ever agree or fail.
    // It used to send a hardcoded "root", which at uid 1000 failed every time.
    //
    // In TYPED mode the person supplied a name, so that name IS sent, and the
    // kernel does exactly what it always did with a non-empty name: checks it
    // against the session user, and burns a failed attempt if it disagrees, so
    // this cannot be used to probe other accounts for free.
    int typed = (s_login_mode == LOGIN_MODE_TYPED);
    int rc = sys_session_unlock(typed ? g_lock_name : "", g_lock_password);
    g_lock_password_len = 0;
    secure_zero(g_lock_password, sizeof(g_lock_password));   /* #73 */

    if (rc == 0) {
        g_lock_error[0] = '\0';
        sys_bootlog("compositor: session unlocked");
        g_needs_redraw = true;
        // g_session_locked itself flips to false via lock_poll()'s next
        // kernel read (next main-loop iteration, <1 frame away) - it is a
        // display cache, never set to false directly here (#566 3.4).
    } else if (rc == -2) {
        // The lockout countdown names no account and is the session user's own,
        // so it is safe in both modes: this screen already belongs to one
        // session and reveals nothing typed mode was hiding.
        int secs = sys_auth_lockout("");   // #745: "" = the session user
        snprintf(g_lock_error, sizeof(g_lock_error),
                 "Too many attempts. Try again in %ds.", secs);
    } else if (typed) {
        // MUST NOT say which of the two was wrong. "No such user" versus "wrong
        // password" is the disclosure this mode exists to prevent, handed over
        // one guess at a time.
        snprintf(g_lock_error, sizeof(g_lock_error), "Incorrect name or password.");
    } else {
        snprintf(g_lock_error, sizeof(g_lock_error), "Incorrect password. Try again.");
    }
}

void lock_poll(void)
{
    g_session_locked = (sys_session_is_locked() == 1);
    if (g_session_locked) { s_idle_lock_tried = 0; return; }  // already locked

    privacy_cfg_poll();
    if (!s_lock_enabled || s_lock_timeout_min <= 0) { s_idle_lock_tried = 0; return; }

    // Reuses the exact idle-tracking primitive the screensaver already uses
    // (g_idle_ms / uptime_ms(), main.c) - an independent "lock after N
    // minutes" that composes with, but does not replace, the screensaver.
    uint64_t idle_ms = uptime_ms() - g_idle_ms;
    if (idle_ms < (uint64_t)s_lock_timeout_min * 60000ULL) {
        s_idle_lock_tried = 0;   // fresh activity within the window: re-arm
        return;
    }
    // Idle threshold reached. Attempt the lock ONCE per idle period. If the
    // kernel refuses (autologin), lock_enter() leaves g_session_locked false;
    // do NOT re-call it every frame (that would re-read /CONFIG/LOGIN.CFG from
    // disk each frame, #426). We re-arm only when input drops idle back below
    // the threshold (the branch above).
    if (s_idle_lock_tried) return;
    s_idle_lock_tried = 1;
    lock_enter_reason(SESSION_LOCK_IDLE);   // #745: the idle timer, not the user
}

// ----------------------------------------------------------------------------
// Layout + floating elements (#569 redesign, user direction seen on real iMac
// hardware): NO panel and NO box chrome. The profile icon, username, a
// ROUNDED-CORNER password field with an ARROW button on its right, the clock,
// Switch User and the power controls all float directly on a full-resolution,
// DARKENED wallpaper. Legibility comes from the scrim plus a drop shadow behind
// every text run. Geometry is computed fresh each call from the same inputs so
// render and hit-testing cannot drift apart.
// ----------------------------------------------------------------------------

#define LOCK_SHADOW      0xFF000000
#define LOCK_SCRIM       0xFF060910
#define LOCK_SCRIM_A     145          // DARKEN ONLY - never pixelate
#define LOCK_FIELD_W     ui_px(300)
#define LOCK_FIELD_H     ui_px(42)
// User request: a smaller submit arrow. It was LOCK_FIELD_H, i.e. exactly as
// tall as the password field, which read as a second button rather than an
// affordance on the end of the field. #745 finishes the move: 28 -> 20, and
// it now sits INSIDE the field's right-hand end instead of floating 12px
// outside it, so the field is centred on the screen on its own.
#define LOCK_ARROW_SZ    ui_px(20)
// LOCK_AVATAR_R IS GONE. It had been defined-but-unused since the avatar disc
// was removed; #745 removes the constant too rather than leave a name that
// invites the disc back.
#define LOCK_EYEBROW_PX  18
#define LOCK_NAME_PX     24
#define LOCK_BODY_PX     18
#define LOCK_POWER_PX    20
#define LOCK_DATE_PX     22
#define LOCK_PILL_BG     0xFF161D2B
// #745: was 0xFF3C4A63, which never cleared 3:1 against any wallpaper. See
// lock_draw_pill() for the measurement and for why this is now a PAIR.
// LOCK_PILL_EDGE is CLR_LOGIN_DIMMED #90A4AE lifted 57/255 toward white, the
// least lift that clears the floor at every backdrop; LOCK_PILL_HALO is the
// existing LOCK_SHADOW black, so the pair introduces exactly one new colour.
#define LOCK_PILL_EDGE   0xFFA8B8C0
#define LOCK_PILL_HALO   0xFF000000

typedef struct {
    int sw, sh, cx;
    int clock_px, clock_y, date_y;
    int av_cy, name_y;
    int fx, fy, fw, fh;       // password pill
    int nx, ny;               // name pill (typed mode only), same w/h
    int eyebrow_y;            // "SIGN IN" (typed mode only)
    int ax, ay, asz;          // arrow (submit), now INSIDE the password pill
    int err_y, switch_y, switch_w, switch_cx;
    int pw_y, pw_h;           // power text row
    int rx, rw, sx, sxw;      // Restart / Shut Down text rects
} lock_geom_t;

static void lock_geom(lock_geom_t *g)
{
    int sw = (int)g_fb_width, sh = (int)g_fb_height;
    g->sw = sw; g->sh = sh; g->cx = sw / 2;

    // Clock: noticeably larger than before, and it still ticks (liveness).
    // #745 (user's direct instruction, 2026-08-12): "move the clock and date
    // down about 10%" - was sh/10 (10% from the top), now sh/5 (~20% from the
    // top). date_y keeps trailing the clock by clock_px + 8, so it moves with
    // it automatically. Applied identically in kernel/gui/login.c's
    // login_geom(): this screen and the kernel gate mirror each other by
    // design (see lock_draw_pill()'s comment on the two sign-in surfaces), so
    // moving only one clock would reintroduce the same kind of jump the
    // credential-column fix below exists to prevent. Checked for collision
    // with the username/password column at both a small (sh=480) and a large
    // (sh>=800, clock_px clamped to its 96 ceiling) framebuffer: date_y always
    // lands well above av_cy - LOCK_NAME_PX/2 with room to spare.
    g->clock_px = sh / 8;
    if (g->clock_px < 48)  g->clock_px = 48;
    if (g->clock_px > 96)  g->clock_px = 96;   // top TTF display size
    g->clock_y = sh / 5;
    g->date_y  = g->clock_y + g->clock_px + 8;

    g->av_cy  = sh / 2 + sh / 16;
    // User request: no avatar disc, just the username. The name moves UP into
    // the space the disc occupied instead of leaving a gap where it was.
    g->name_y = g->av_cy - LOCK_NAME_PX / 2;

    g->fw  = LOCK_FIELD_W;
    g->fh  = LOCK_FIELD_H;
    g->asz = LOCK_ARROW_SZ;
    g->fy  = g->name_y + LOCK_NAME_PX + 22;
    // #745: the field is centred on its OWN width now. It used to be centred
    // on field+gap+arrow, which pushed it left of centre by 16px to make room
    // for an arrow that is now inside it.
    g->fx  = g->cx - g->fw / 2;
    g->ax  = g->fx + g->fw - g->asz - 10;
    g->ay  = g->fy + (g->fh - g->asz) / 2;   // centred on the field, not top-aligned

    // Typed mode: the name field 52px above the password field, with the
    // eyebrow above that. The vertical anchor of the column is UNCHANGED in
    // both modes - only the contents of the stack differ.
    g->nx = g->fx;
    g->ny = g->fy - 52;
    g->eyebrow_y = g->ny - 26 - LOCK_EYEBROW_PX;

    g->err_y    = g->fy + g->fh + 14;
    // #745: the error row is RESERVED whether or not there is an error, so
    // "Switch User" no longer jumps 18px down the screen the moment somebody
    // mistypes - which is exactly when they are least able to afford a
    // control moving out from under the pointer.
    g->switch_y = g->err_y + 26;
    g->switch_w = text_width_ttf("Switch User", LOCK_BODY_PX);
    g->switch_cx = g->fx + g->fw / 2;   // the password field's own centre line

    g->rw   = text_width_ttf("Restart", LOCK_POWER_PX);
    g->sxw  = text_width_ttf("Shut Down", LOCK_POWER_PX);
    g->pw_h = LOCK_POWER_PX + 8;
    g->pw_y = sh - g->pw_h - 24;
    g->sx   = sw - 30 - g->sxw;
    g->rx   = g->sx - 32 - g->rw;
}

// FLOATING text: a dark offset copy first, then the light text on top. This is
// the drop shadow that makes bare text legible on a photographic wallpaper with
// nothing behind it.
static void lock_text(int x, int y, const char *s, int px, uint32_t fg)
{
    int off = px / 14;
    if (off < 1) off = 1;
    if (off > 4) off = 4;
    draw_text_ttf(x + off, y + off, s, px, LOCK_SHADOW);
    draw_text_ttf(x, y, s, px, fg);
}

static void lock_text_c(int cx, int y, const char *s, int px, uint32_t fg)
{
    lock_text(cx - text_width_ttf(s, px) / 2, y, s, px, fg);
}

// SQUARE-EDGED credential field. Bullets reuse login.c's shared draw_bullet().
//
// #745 (user's direct instruction, 2026-08-11): the kernel sign-in gate's boxes
// were squared off because the user's chosen fix for their reported "pixelated"
// rounded corners was to remove the corners, not to antialias them. This screen
// is a SEPARATE implementation of the same control (kernel/gui/login.c is the
// Ring-0 boot gate, this is the compositor's lock screen) and had been left on
// the old rounded treatment, so locking the screen showed a control the user had
// already rejected. Structure below is now a line-for-line mirror of
// draw_pill_bg() in kernel/gui/login.c, with this file's primitives in place of
// the framebuffer ones (draw_fill_rect <-> fb_fill_rect, draw_rect_outline <->
// fb_draw_rect). An axis-aligned 1px outline has no antialiasing question.
//
// #745: one function draws BOTH fields. `masked` picks bullets or plain text,
// `arrow_sz` is the size of a submit arrow drawn INSIDE this field (0 when
// there is none) and the bullet run and the caret both stop at
// x + w - arrow_sz - 16 so neither can run underneath it, and `err` holds the
// error border on the control until the next keystroke.
// #745: NO LONGER STATIC. The elevation modal (elevate.c) draws its password
// field with this exact function, so the OS has one credential-field
// implementation rather than two that look alike until one of them is changed.
void lock_draw_pill(int x, int y, int w, int h, const char *text,
                    int masked, int focused, int caret, int err,
                    const char *placeholder, int arrow_sz)
{
    draw_fill_rect(x + 2, y + 3, w, h, LOCK_SHADOW);
    draw_fill_rect(x, y, w, h, LOCK_PILL_BG);
// #745 BOUNDARY PAIR. The pill used to have ONE stroke, and a single stroke
// cannot bound this control: the field sits on a wallpaper the user chooses,
// darkened by a fixed scrim, so its backdrop ranges over #030509 (black
// wallpaper) to #717377 (white wallpaper). A dark-blue stroke measured 2.28:1
// against the dark end and 1.88:1 against the light end, worst case 1.54:1 at
// a mid-grey wallpaper: below the 3:1 WCAG 1.4.11 non-text floor everywhere.
// No single colour can fix that, and a lighter blue makes the bright end
// WORSE. So the boundary is carried by a PAIR, and whichever member is losing
// against the current backdrop, the other one is winning:
//     dark member  = a 1px halo OUTSIDE the pill, pure black
//     light member = the 1px stroke on the pill itself
// Measured worst case over EVERY wallpaper luminance the scrim can produce
// (all 256 grey steps, which spans the achievable backdrop range, so this is a
// bound and not a sample): PAIR MAX 3.22:1 at backdrop #5C5D61.
//
// Focus and error are drawn as a ring INSIDE the structural pair instead of
// replacing the stroke. They used to replace it, which meant focusing the
// field or getting the password wrong COST the control its boundary (accent
// 2.60:1, error 2.46:1 worst case). Inside, against the fill, they measure
// 5.40:1 and 4.84:1.
    // Same structural boundary PAIR as before (a black halo OUTSIDE the box and
    // a light stroke ON it), which is what keeps the edge legible over any
    // wallpaper luminance; only the corners are square now, and the fill is
    // painted once above instead of being the innermost of four nested rects.
    draw_rect_outline(x - 1, y - 1, w + 2, h + 2, LOCK_PILL_HALO);
    draw_rect_outline(x, y, w, h, LOCK_PILL_EDGE);
    if (err || focused)
        draw_rect_outline(x + 1, y + 1, w - 2, h - 2,
                          err ? CLR_LOGIN_ERROR : CLR_LOGIN_ACCENT);

    const int stop = x + w - arrow_sz - 16;
    int tx = x + h / 2 + 2;
    int ty = y + (h - LOCK_BODY_PX) / 2 - 1;
    int n  = (int)strlen(text);

    if (n == 0) {
        if (placeholder)
            draw_text_ttf(tx, ty, placeholder, LOCK_BODY_PX, CLR_LOGIN_DIMMED);
        if (focused && caret)
            draw_fill_rect(tx, y + 10, 2, h - 20, CLR_LOGIN_TEXT);
        return;
    }

    int end_x;
    if (masked) {
        int bx = tx, by = y + h / 2;
        for (int i = 0; i < n && bx < stop; i++) {
            draw_bullet(bx, by);
            bx += 12;
        }
        end_x = bx;
    } else {
        draw_text_ttf(tx, ty, text, LOCK_BODY_PX, CLR_LOGIN_TEXT);
        end_x = tx + text_width_ttf(text, LOCK_BODY_PX) + 2;
    }
    if (focused && caret && end_x < stop)
        draw_fill_rect(end_x, y + 10, 2, h - 20, CLR_LOGIN_TEXT);
}

// The submit control: a BARE right-pointing chevron at the right-hand end of
// the password field. Clicking it does exactly what Enter does (do_unlock()).
static void lock_draw_arrow(int x, int y, int s)
{
    // #745 (user's direct instruction, 2026-08-11): "i dont like the blue >
    // button on the login screen a simple > triangle without the blue surround
    // would do fine as long as its crisp". The accent DISC, its drop-shadow
    // disc, the hand-drawn rectangular shaft and the hand-drawn scanline
    // triangle head are all DELETED. The old head was a per-row draw_hline()
    // fan with no coverage step, so every one of its edges was a hard stairstep
    // on a 20px control - which is the "not crisp" half of the complaint.
    //
    // What replaces them is one real font glyph. draw_text_ttf() goes through
    // SYS_DRAW_TTF into the kernel's ttf_draw_string(), which does per-pixel
    // coverage blending against the live framebuffer, so the mark is
    // antialiased by the same path that already draws every label on this
    // screen. This is the same treatment kernel/gui/login.c's
    // draw_arrow_button() now uses, so the boot gate and the lock screen show
    // the same control again.
    int cx = x + s / 2, cy = y + s / 2;

    // SIZE: an exact glyph-cache bucket ({12,14,16,18,20,24,28,32,48,96}; a
    // non-bucket size renders at a different size than it is measured at).
    // MEASURED on the real rasterizer with the shipped font: '>' at 20px has a
    // 12x10 ink box unstyled, which fills the 20px slot and still clears the
    // field's own 1px stroke by 12px on the right.
    const int gpx = 20;

    // WEIGHT. The kernel gate draws this glyph with TTF_STYLE_BOLD. Userland
    // has no styled SCREEN text entry point at all - SYS_DRAW_TTF renders
    // TTF_STYLE_NORMAL, and the only style-aware call, SYS_WIN_DRAW_TTF_EX,
    // needs a window handle, which a full-screen lock has no such thing as. So
    // the bold is reproduced rather than requested: ttf.c's apply_bold() is
    // exactly "merge each column with the one to its left, clamped", so drawing
    // the same glyph a second time one pixel to the right reproduces it with the
    // primitive this side of the syscall boundary actually has.
    // MEASURED against the real rasterizer, this double draw and the kernel's
    // TTF_STYLE_BOLD produce the SAME 13x10 ink box and the SAME 68 inked
    // pixels; they differ only in the antialiased fringe (mean ink luminance
    // 165.5 vs 173.8 of 255, max single-channel delta 51). The durable fix is a
    // style-capable screen-TTF entry point, which is a syscall addition and is
    // deliberately out of scope for a visual change.
    //
    // CENTRING: horizontally on the glyph's ADVANCE, which for a lone '>' puts
    // the ink box dead centre (MEASURED: advance 14, ink 12 at xoff 1, so the
    // ink centre lands exactly on cx); vertically on this file's own
    // "y + (h - px) / 2" idiom, which at gpx=20 lands the 10px ink box exactly
    // on cy (MEASURED: ascent 15, yoff -10, so ink top = y + 5 = cy - 5).
    int tx = cx - text_width_ttf(">", gpx) / 2;
    int ty = cy - gpx / 2;

    // INK: this screen's own primary text token, the same one the password
    // bullets beside it use. MEASURED on the rendered core pixel: 14.61:1
    // against the LOCK_PILL_BG fill the mark sits on. (The mark is INSIDE the
    // field, so its backdrop is that fixed fill and never the wallpaper. There
    // is no hover state to carry here - lock_draw_arrow() has never been passed
    // one, unlike the kernel gate's draw_arrow_button().)
    draw_text_ttf(tx,     ty, ">", gpx, CLR_LOGIN_TEXT);
    draw_text_ttf(tx + 1, ty, ">", gpx, CLR_LOGIN_TEXT);
}

void lock_render(void)
{
    lock_geom_t g;
    lock_geom(&g);

    // 1. Backdrop: the real wallpaper at FULL RESOLUTION, DARKENED ONLY. The
    //    old block-mosaic "frosted glass" pass is gone (#569): it is what made
    //    the wallpaper look pixelated and terrible.
    wallpaper_render_background();
    g_draw_blend = LOCK_SCRIM_A;
    draw_fill_rect(0, 0, g.sw, g.sh, LOCK_SCRIM);
    g_draw_blend = 255;

    // 2. Large live clock + date: the liveness indicator (#566 3.6 - two
    //    screenshots must show a different reading, never a wedged frame).
    {
        char tbuf[16], dbuf[24];
        lock_clock_hms(tbuf, 1);
        lock_clock_date(dbuf);
        lock_text_c(g.cx, g.clock_y, tbuf, g.clock_px, CLR_LOGIN_TEXT);
        lock_text_c(g.cx, g.date_y,  dbuf, LOCK_DATE_PX, CLR_LOGIN_DIMMED);
    }

    // 3. Username, floating, no box and no avatar disc. #745: NOT SHOWN AT ALL
    //    in typed mode, including here, where this process already knows exactly
    //    who the session is. The mode's promise is that no account name is on
    //    screen; "we happen to know it" is not an exception to that, it is the
    //    only reason the exception would ever be made.
    g_lock_blink_counter++;
    if (g_lock_blink_counter >= 30) { g_lock_blink_counter = 0; g_lock_cursor_blink ^= 1; }
    int typed = (s_login_mode == LOGIN_MODE_TYPED);
    int err   = (g_lock_error[0] != '\0');

    if (typed) {
        lock_text_c(g.cx, g.eyebrow_y, "SIGN IN", LOCK_EYEBROW_PX, CLR_LOGIN_DIMMED);
        lock_draw_pill(g.nx, g.ny, g.fw, g.fh, g_lock_name, 0,
                       g_lock_field == 0, g_lock_cursor_blink, err, "Name", 0);
    } else {
        const char *uname = g_login_username[0] ? g_login_username : "?";
        lock_text_c(g.cx, g.name_y, uname, LOCK_NAME_PX, CLR_LOGIN_TEXT);
    }

    // 4. Rounded password field with the arrow INSIDE its right-hand end.
    lock_draw_pill(g.fx, g.fy, g.fw, g.fh, g_lock_password, 1,
                   !typed || g_lock_field == 1, g_lock_cursor_blink, err,
                   "Password", g.asz);
    lock_draw_arrow(g.ax, g.ay, g.asz);

    // 5. Error / lockout message. The row it sits in is reserved either way, so
    //    "Switch User" below does not move when this appears or clears.
    if (err)
        lock_text_c(g.cx, g.err_y, g_lock_error, LOCK_BODY_PX, CLR_LOGIN_ERROR);

    // 6. "Switch User" (v1: ends this session and returns to the kernel login
    //    gate, #566 1.1): shadowed text, no surround. The notification count is
    //    deliberately GONE (#569) - nothing about notifications is shown here.
    lock_text_c(g.switch_cx, g.switch_y, "Switch User", LOCK_BODY_PX, CLR_LOGIN_DIMMED);

    // 7. Power controls: shadowed text, no surrounds, still available WITHOUT
    //    authenticating (#566 4.2).
    lock_text(g.rx, g.pw_y + 4, "Restart",   LOCK_POWER_PX, CLR_LOGIN_DIMMED);
    lock_text(g.sx, g.pw_y + 4, "Shut Down", LOCK_POWER_PX, CLR_LOGIN_DIMMED);
}

int lock_handle_key(int key)
{
    // Still NO ESC DISMISS: the lock closes ONLY via a correct password
    // (#566 3.2). #745 gives Esc a job in typed mode - clearing both fields -
    // which is a different thing entirely and leaves the session locked.
    int typed = (s_login_mode == LOGIN_MODE_TYPED);
    int on_name = (typed && g_lock_field == 0);

    if (key == '\r' || key == '\n') {
        // Enter moves off the name field the first time and submits from the
        // password field, so a person who types name-Enter-password-Enter (the
        // shape every other two-field form in this OS has) is never surprised.
        if (on_name) g_lock_field = 1;
        else         do_unlock();
    } else if (key == '\t' && typed) {
        g_lock_field ^= 1;
    } else if (key == 27 && typed) {
        g_lock_name_len = 0; g_lock_name[0] = '\0';
        g_lock_password_len = 0; secure_zero(g_lock_password, sizeof(g_lock_password));   /* #73 */
        g_lock_error[0] = '\0';
        g_lock_field = 0;
    } else if (key == 8 || key == 127) {
        if (on_name) {
            if (g_lock_name_len > 0) g_lock_name[--g_lock_name_len] = '\0';
        } else if (g_lock_password_len > 0) {
            g_lock_password[--g_lock_password_len] = '\0';
            /* #73: the dropped byte is erased, not just un-terminated. */
        }
        g_lock_error[0] = '\0';   // clears the held error border
    } else if (key >= 32 && key <= 126) {
        if (on_name) {
            // Same two exclusions the account forms use: no space and no ':',
            // which is the /CONFIG/PASSWD field delimiter.
            if (key != ' ' && key != ':' && g_lock_name_len < LOCK_MAX_NAME - 1) {
                g_lock_name[g_lock_name_len++] = (char)key;
                g_lock_name[g_lock_name_len]   = '\0';
            }
        } else if (g_lock_password_len < LOCK_MAX_PASSWORD - 1) {
            g_lock_password[g_lock_password_len++] = (char)key;
            g_lock_password[g_lock_password_len]   = '\0';
        }
        g_lock_error[0] = '\0';
    }
    g_needs_redraw = true;
    return 1;   // always consumed
}

int lock_handle_mouse(int32_t x, int32_t y, bool clicked)
{
    if (!clicked) return 1;   // still fully consumed: nothing passes through

    lock_geom_t g;
    lock_geom(&g);

    // Arrow button = submit (the same action as Enter). Tested BEFORE the
    // fields, because #745 moved it inside the password pill: a click on it
    // must submit, not merely focus the field underneath it.
    if (x >= g.ax && x < g.ax + g.asz && y >= g.ay && y < g.ay + g.asz) {
        do_unlock();
        return 1;
    }
    // #745 typed mode: clicking a field focuses it.
    if (s_login_mode == LOGIN_MODE_TYPED) {
        if (x >= g.nx && x < g.nx + g.fw && y >= g.ny && y < g.ny + g.fh) {
            g_lock_field = 0; g_needs_redraw = true; return 1;
        }
        if (x >= g.fx && x < g.fx + g.fw && y >= g.fy && y < g.fy + g.fh) {
            g_lock_field = 1; g_needs_redraw = true; return 1;
        }
    }

    // "Switch User": ends this session cleanly so the kernel re-enters the
    // login gate (v1 scope - no fast user switching, #566 1.1/4.2).
    if (y >= g.switch_y && y < g.switch_y + LOCK_BODY_PX + 8 &&
        x >= g.switch_cx - g.switch_w / 2 && x < g.switch_cx + g.switch_w / 2) {
        sys_bootlog("compositor: Switch User from lock screen -> clean exit for login re-entry");
        sys_exit(0);
        return 1;
    }

    // Power controls: available without authenticating. Now plain shadowed
    // text, so the hit rects come from the measured text extents.
    if (y >= g.pw_y && y < g.pw_y + g.pw_h) {
        if (x >= g.rx && x < g.rx + g.rw)  { reboot();   return 1; }
        if (x >= g.sx && x < g.sx + g.sxw) { poweroff(); return 1; }
    }

    return 1;   // true modal: swallow every other click while locked
}
