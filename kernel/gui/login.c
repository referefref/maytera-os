// login.c - Login screen for MayteraOS
// Full-screen login with user selection and password entry

#include "login.h"
#include "themes.h"
#include "image.h"
#include "ttf.h"
#include "window_decor.h"
#include "desktop.h"        // #745: desktop_session_authenticated()
#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../video/graphics.h"
#include "../drivers/mouse.h"
#include "../drivers/acpi.h"
#include "../cpu/isr.h"
#include "../proc/users.h"
#include "../proc/syscall.h"   // #745: LOGIN_MODE_* + login_mode_configured()
#include "../proc/pwpolicy.h"
#include "../proc/fdlayer.h"  // #745: sys_open_k/sys_readdir_k, sc_dirent_t
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"

// External filesystem
extern fat_fs_t g_fat_fs;

// #307: on-screen diagnostic for why autologin was NOT taken. If the machine
// lands on the interactive "Select User" stub, login_draw() renders this string
// so the exact failing branch is visible ON the display (no serial needed).
static char g_autologin_debug[96] = {0};

// External process functions
extern void proc_yield(void);

// #745: the cooked key codes keyboard_get_char() returns for the arrows.
// Named locally so which of the two KEY_UP headers wins the include order
// cannot silently change what this file means; the assert below proves the
// one that is visible here is the cooked value and not the scancode.
#define LOGIN_KEY_UP    0x80
#define LOGIN_KEY_DOWN  0x81
_Static_assert(KEY_UP == LOGIN_KEY_UP && KEY_DOWN == LOGIN_KEY_DOWN,
               "#745: login.c is seeing drivers/keyboard.h's SCANCODE KEY_UP "
               "(0x48 = 'H'), not cpu/isr.h's cooked 0x80 that "
               "keyboard_get_char() actually delivers");

// External timer
extern volatile uint64_t timer_ticks;

// Forward declarations from font.h
extern const uint8_t *font_get_glyph(char c);

// ============================================================================
// Login gate palette (#569 redesign, per the user's direction seen on real
// iMac hardware): NO panel, NO box chrome, and NO pixelated wallpaper. Every
// element floats directly on a FULL-RESOLUTION wallpaper that is only
// DARKENED; legibility comes from that scrim plus a drop shadow behind each
// text run, not from a chrome box behind it. 0x00RRGGBB values map onto the
// BGRA framebuffer on little-endian x86 (see fb_put_pixel).
// ============================================================================

#define LOGIN_SCRIM_COLOR       0x00060910  // dark scrim laid over the wallpaper
#define LOGIN_SCRIM_ALPHA       145         // DARKEN ONLY - never pixelate
#define LOGIN_GRAD_TOP          0x000D1420  // gradient fallback top
#define LOGIN_GRAD_BOT          0x00070B13  // gradient fallback bottom
#define LOGIN_TEXT_COLOR        0x00FFFFFF  // primary floating text
#define LOGIN_TEXT_DIM          0x00D8DFEA  // secondary floating text
#define LOGIN_TEXT_FAINT        0x00AAB6C8  // field captions / placeholder
#define LOGIN_SHADOW_COLOR      0x00000000  // drop shadow behind floating text
#define LOGIN_ACCENT            0x005B8CFF  // arrow (submit) button
#define LOGIN_ACCENT_HOVER      0x007CA5FF
#define LOGIN_INPUT_BG          0x00161D2B  // rounded password pill
// #745: was 0x003C4A63. Identical defect and identical hex to the compositor
// lock screen's pill - the two are INDEPENDENT COPIES, not a shared constant,
// so fixing one would have left the boot gate broken. See draw_pill_bg().
#define LOGIN_INPUT_BORDER      0x00A8B8C0
#define LOGIN_INPUT_HALO        0x00000000
#define LOGIN_INPUT_FOCUS       0x005B8CFF
#define LOGIN_ERROR_COLOR       0x00FF8C8C
// #745: the field border while an error is showing. Held until the next
// keystroke, so a wrong password is visible on the control that was wrong
// and not only in a line of text underneath it.
#define LOGIN_INPUT_ERROR       0x00EF5350
// #745 sign-in redesign (docs/OOBE_SIGNIN_OPTIONS.html section 7). The
// eyebrow and the highlighted account row are the only NEW colours; both are
// specified there. LOGIN_AVATAR_BG / LOGIN_AVATAR_RING are GONE with
// draw_avatar(): the "remove the drop-shadow avatar circle, username only"
// feedback had been applied to the lock screen and NOT to this screen, which
// is the one people actually see when the machine starts.
#define LOGIN_EYEBROW_COLOR     0x008FCFC0
#define LOGIN_ROW_SEL_FILL      0x0016241F

// Element sizing. There is deliberately no panel width/height any more.
#define LOGIN_FIELD_W           300
#define LOGIN_FIELD_H           42
// #745: the submit arrow shrinks from LOGIN_FIELD_H (it was exactly as tall
// as the field, so it read as a second button rather than an affordance on
// the end of one) and moves INSIDE the pill. The lock screen had already had
// the first half of that change; this screen had had neither.
#define LOGIN_ARROW_SZ          20
// Account picker rows (section 7 L4): username only, vertical, 40px pitch.
#define LOGIN_ROW_W             320
#define LOGIN_ROW_H             36
#define LOGIN_ROW_PITCH         40
#define LOGIN_ROWS_VISIBLE      6
#define LOGIN_EYEBROW_PX        18
#define LOGIN_NAME_PX           24
#define LOGIN_LABEL_PX          16
#define LOGIN_BODY_PX           18
#define LOGIN_POWER_PX          20
#define LOGIN_DATE_PX           22
#define LOGIN_MAX_PASSWORD      64
#define LOGIN_MAX_USERNAME      32

// ============================================================================
// Login state
// ============================================================================

typedef enum {
    LOGIN_STATE_SELECT_USER,
    LOGIN_STATE_CREATE_ACCOUNT,   // #568 first-boot: no accounts yet
    // #745: name + password, with NO account name anywhere on screen. This
    // is a whole state and not a flag on SELECT_USER because the two screens
    // share no control: one is a list you move a highlight down, the other is
    // two text fields, and the promise of this one is that it discloses
    // nothing that the other one exists to display.
    LOGIN_STATE_TYPED,
    LOGIN_STATE_PASSWORD,
    LOGIN_STATE_ERROR,
    LOGIN_STATE_SUCCESS
} login_state_t;

static struct {
    login_state_t state;
    int selected_user;          // Index into user table
    char password[LOGIN_MAX_PASSWORD];
    int password_pos;
    char error_msg[128];
    int error_timer;            // Ticks until error clears
    uint32_t screen_w;
    uint32_t screen_h;
    bool cursor_visible;        // Password cursor blink
    uint64_t cursor_blink_tick;
    int hover_user;             // User avatar being hovered (-1 = none)
    bool hover_login_btn;       // Arrow (submit) button being hovered
    int hover_power;            // Power control hover: 0 none, 1 restart, 2 shutdown
    bool initialized;
    // #745 sign-in screen mode (LOGIN_MODE_LIST / LOGIN_MODE_TYPED). Read
    // ONCE, in login_run(), from /CONFIG/LOGIN.CFG via login_mode_configured().
    // Not per frame: that would re-read a FAT file on every redraw, which is
    // the #426 pattern this tree has been paying down for months. Once is
    // correct here because nothing else runs while the gate is up, so the file
    // cannot change underneath it. The ACCOUNT COUNT is a different question
    // and IS taken fresh every draw and every hit-test - see login_active().
    int mode;
    int list_first;             // first visible picker row (scroll offset)
    // #745 typed mode: the name the person supplied. Kept separate from the
    // picker's index because in this mode there is no index - there is no list.
    char tu_user[LOGIN_MAX_USERNAME];
    int  tu_field;              // 0 = name, 1 = password
    // #568 first-boot create-account flow
    char cu_user[LOGIN_MAX_USERNAME];
    char cu_pass[LOGIN_MAX_PASSWORD];
    char cu_confirm[LOGIN_MAX_PASSWORD];
    // #745: root gets its OWN password, so this screen collects two of them.
    // Both are confirmed. A typo in root's password is not recoverable on this
    // machine: there is no sudo and no elevation, so the only way to be root is
    // to sign in as root, and a mistyped credential locks uid 0 forever.
    char cu_root[LOGIN_MAX_PASSWORD];
    char cu_rootconf[LOGIN_MAX_PASSWORD];
    int  cu_field;              // 0=username 1=password 2=confirm 3=root 4=confirm-root
    bool cu_submit_requested;   // set by the arrow button click
} login_ctx;

// ============================================================================
// Darkened wallpaper backdrop (#569): built ONCE (a full-resolution wallpaper
// blit plus a scrim is too expensive for a per-frame redraw, #426) into a
// screen-sized cache, then cheaply restored every frame. Kept outside
// login_ctx so it survives login_init()'s memset across gate re-entries.
// ============================================================================
static uint32_t *g_login_backdrop = NULL;   // cached finished backdrop (tight sw*sh)
static bool      g_backdrop_built = false;
static uint32_t  g_backdrop_w = 0, g_backdrop_h = 0;

// ============================================================================
// Text: the kernel TTF renderer when it is ready (desktop_init() loads /FONTS
// BEFORE the login gate runs, see main.c), otherwise the 8x16 bitmap font
// scaled by an integer factor. One code path, so every caller below can just
// ask for a pixel size.
// ============================================================================

static void draw_char_scaled(int32_t x, int32_t y, char c, uint32_t color, int s) {
    const uint8_t *glyph = font_get_glyph(c);
    if (!glyph) return;
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) continue;
            for (int dy = 0; dy < s; dy++)
                for (int dx = 0; dx < s; dx++)
                    fb_put_pixel(x + col * s + dx, y + row * s + dy, color);
        }
    }
}

static void draw_text_scaled(int32_t x, int32_t y, const char *str, uint32_t color, int s) {
    while (*str) {
        draw_char_scaled(x, y, *str, color, s);
        x += 8 * s;
        str++;
    }
}

static int login_bitmap_scale(int px) {
    int s = px / 16;
    return s < 1 ? 1 : s;
}

// Width of a text run at the requested pixel size.
static int login_text_w(const char *s, int px) {
    if (ttf_is_ready()) {
        int w = ttf_measure_string(s, px);
        if (w > 0) return w;
    }
    return (int)strlen(s) * 8 * login_bitmap_scale(px);
}

// Text with no shadow (used inside filled chrome such as the pill and avatar).
static void login_text_raw(int x, int y, const char *s, int px, uint32_t color) {
    if (ttf_is_ready()) { ttf_draw_string(x, y, s, px, color); return; }
    draw_text_scaled(x, y, s, color, login_bitmap_scale(px));
}

// FLOATING text: a dark offset copy first, then the light text on top. This is
// the drop shadow the redesign asks for, and it is what makes bare text legible
// on a photographic wallpaper with no surround behind it.
static void login_text(int x, int y, const char *s, int px, uint32_t color) {
    int off = px / 14;
    if (off < 1) off = 1;
    if (off > 4) off = 4;
    login_text_raw(x + off, y + off, s, px, LOGIN_SHADOW_COLOR);
    login_text_raw(x, y, s, px, color);
}

static void login_text_c(int cx, int y, const char *s, int px, uint32_t color) {
    login_text(cx - login_text_w(s, px) / 2, y, s, px, color);
}

// ============================================================================
// Floating elements
// ============================================================================

// #745: draw_avatar() IS DELETED. No disc, no initial letter, no shadow disc,
// on either sign-in surface. It had two callers left, the account picker and
// the password view, and the redesign removes both; leaving the function
// behind would only invite the next screen to call it again.


// #745: draw ONE glyph centred on (cx,cy) by its INK BOX, not by its advance
// width, which is what makes a single character look centred inside a disc.
// ttf_draw_char() places the ink at (x + g->xoff, y + ascent + g->yoff), so
// this solves that for the pen position. Returns false when the TTF renderer
// is not up (pre-font boot, missing /FONT.TTF) so the caller can fall back.
static bool login_glyph_centered(int cx, int cy, int cp, int px, uint32_t color) {
    if (!ttf_is_ready()) return false;
    ttf_glyph_t *g = ttf_get_glyph(cp, px, TTF_STYLE_BOLD);
    if (!g || !g->bitmap || g->width <= 0 || g->height <= 0) return false;
    int ascent = 0, descent = 0, line_gap = 0;
    ttf_get_metrics(px, &ascent, &descent, &line_gap);
    int gx = cx - g->width  / 2 - g->xoff;
    int gy = cy - g->height / 2 - g->yoff - ascent;
    ttf_draw_char(gx, gy, cp, px, TTF_STYLE_BOLD, color);
    return true;
}

// Rounded-corner (pill) entry field. Radius h/2, so there is not a hard corner
// anywhere on it. #745: `error` holds the error border on the control that was
// wrong until the next keystroke.
static void draw_pill_bg(int x, int y, int w, int h, bool focused, bool error) {
    // #745 (user's direct instruction, 2026-08-11): the credential boxes are
    // SQUARE-EDGED now. The user's chosen fix for the reported "pixelated"
    // rounded corners is to remove the corners, not to antialias them, so these
    // draw as plain rectangles - no radius, no corner-coverage question at all.
    // (The AA rounded-rect primitive built for this ticket stays as a shared
    // library function and is still used by the arrow DISC below; it just is not
    // used for these two boxes.)
    fb_fill_rect(x + 2, y + 3, w, h, LOGIN_SHADOW_COLOR);
    fb_fill_rect(x, y, w, h, LOGIN_INPUT_BG);
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
    // wallpaper luminance; only the corners are square now. fb_draw_rect draws a
    // 1px axis-aligned outline, which has no antialiasing question.
    fb_draw_rect(x - 1, y - 1, w + 2, h + 2, LOGIN_INPUT_HALO);
    fb_draw_rect(x, y, w, h, LOGIN_INPUT_BORDER);
    if (error || focused)
        fb_draw_rect(x + 1, y + 1, w - 2, h - 2,
                     error ? LOGIN_INPUT_ERROR : LOGIN_INPUT_FOCUS);
}

// Pill contents: masked bullets for secrets, plain text otherwise, with a
// blinking caret and a dim placeholder when empty.
// `arrow_sz` is the size of a submit arrow drawn INSIDE this field, or 0 when
// there is none. #745: the bullet run and the caret stop at
// x + w - arrow_sz - 16 so neither can ever run underneath it. Passing 0 keeps
// the create-account form, whose arrow is still outside the field, exactly as
// it was.
static void draw_pill_field(int x, int y, int w, int h, const char *text,
                            bool masked, bool focused, bool caret,
                            const char *placeholder, int arrow_sz, bool error) {
    draw_pill_bg(x, y, w, h, focused, error);
    const int stop = x + w - arrow_sz - 16;

    int tx = x + h / 2 + 2;
    int ty = y + (h - LOGIN_BODY_PX) / 2 - 1;
    int n  = (int)strlen(text);

    if (n == 0) {
        if (placeholder)
            login_text_raw(tx, ty, placeholder, LOGIN_BODY_PX, LOGIN_TEXT_FAINT);
        return;
    }

    int end_x;
    if (masked) {
        int bx = tx, by = y + h / 2;
        for (int i = 0; i < n && bx < stop; i++) {
            gfx_fill_circle(bx, by, 4, LOGIN_TEXT_COLOR);
            bx += 12;
        }
        end_x = bx;
    } else {
        login_text_raw(tx, ty, text, LOGIN_BODY_PX, LOGIN_TEXT_COLOR);
        end_x = tx + login_text_w(text, LOGIN_BODY_PX) + 2;
    }

    if (focused && caret && end_x < stop)
        fb_fill_rect(end_x, y + 10, 2, h - 20, LOGIN_TEXT_COLOR);
}

// The submit control: a BARE right-pointing chevron at the right-hand end of
// the entry field. Clicking it is the same action as pressing Enter.
static void draw_arrow_button(int x, int y, int s, bool hovered) {
    // #745 (user's direct instruction, 2026-08-11): "i dont like the blue >
    // button on the login screen a simple > triangle without the blue surround
    // would do fine as long as its crisp". The accent DISC and the drop-shadow
    // disc behind it are DELETED. What is left is the half of the control that
    // was already antialiased: ttf_draw_char() does per-pixel coverage blending
    // against the live framebuffer, so the mark stays crisp with no new
    // machinery and no AA question of its own.
    //
    // decor_fill_rounded_rect_aa() loses its LAST caller here and STAYS in
    // window_decor.c on purpose. The 100-or-0 coverage defect in the plain
    // decor_*_rounded_rect() pair is tree-wide, and the AA version is the fix
    // for it; deleting it because this one caller went away would throw the
    // primitive out with the caller.
    int cx = x + s / 2, cy = y + s / 2;

    // SIZE. The glyph cache snaps every request to the closest of ten buckets
    // {12,14,16,18,20,24,28,32,48,96}, so nothing below 12 exists however it is
    // labelled and a non-bucket size renders at a size ttf_get_metrics() was
    // not asked about. Both values below are exact bucket entries.
    //
    // They are NOT the sizes the disc version used, and that is the point. With
    // a 20px accent disc behind it a 12px glyph read as an affordance; alone it
    // is a speck. MEASURED on this exact rasterizer and this exact font, the
    // '>' bold ink box is 8x6 at 12px, 13x10 at 20px and 20x15 at 32px. 20px
    // fills the 20px inline slot and still clears the pill's own 1px stroke by
    // 12px on the right; 32px sits inside the 42px standalone slot.
    int gpx = (s >= 32) ? 32 : 20;

    // INK. The old ink was LOGIN_INPUT_BG, picked to sit on the accent fill.
    // With the fill gone, that same navy would be 1.00:1 against the pill the
    // mark now sits on - literally invisible. The ink HAD to move with the disc;
    // removing the disc alone would have removed the button.
    //
    // MEASURED (WCAG 2.x relative luminance, computed twice independently, on
    // the RENDERED core pixel - the glyph core is written at full opacity by
    // ttf_draw_char()'s alpha >= 250 branch, so the core pixel IS the token):
    //   INLINE (LOGIN_STATE_TYPED / LOGIN_STATE_PASSWORD): the arrow sits INSIDE
    //   the password pill, so its backdrop is the fixed LOGIN_INPUT_BG fill and
    //   NOT the wallpaper. 12.58:1 resting, 16.87:1 hovered.
    //   STANDALONE (LOGIN_STATE_CREATE_ACCOUNT): here the backdrop IS the
    //   scrimmed wallpaper. Swept over all 256 wallpaper luminances the scrim
    //   (#060910 at alpha 145) can produce, i.e. backdrops #030509 through
    //   #717377: worst case 3.54:1 resting and 4.75:1 hovered, both at the
    //   bright end. A single-colour mark is monotonic in backdrop luminance, so
    //   those endpoints are a BOUND and not a sample - the middle-is-worst trap
    //   applies to a two-member PAIR (as the pill's own halo+stroke boundary
    //   is), not to one mark. Both states clear the 3:1 WCAG 1.4.11 floor for a
    //   graphical control at every point of that range.
    //
    // Hover moved onto the ink because there is no fill left to lighten, and
    // LOGIN_ACCENT_HOVER is exactly the blue the user asked to be rid of.
    uint32_t ink = hovered ? LOGIN_TEXT_COLOR : LOGIN_TEXT_DIM;

    // ASCII '>' and NOT U+2192 RIGHTWARDS ARROW: ttf_draw_char() would render
    // U+2192 correctly IF the shipped font carried that glyph, but fonts live in
    // the static asset base, which is not in this repo, so glyph coverage cannot
    // be verified from source. A missing glyph is an invisible button.
    //
    // The standalone arrow floats on the photograph, so it takes the same 1px
    // dark offset copy every other floating element on this screen uses (see
    // login_text()). That is house style for anything drawn straight onto the
    // wallpaper, NOT the contrast mechanism: the mark clears the floor unaided
    // at every backdrop, and the shadow itself is only 1.03:1 at the dark end.
    // Inside the pill the backdrop is a flat fill and a shadow would do nothing
    // but muddy a 10px-tall mark.
    if (s >= 32)
        login_glyph_centered(cx + 1, cy + 1, '>', gpx, LOGIN_SHADOW_COLOR);

    if (!login_glyph_centered(cx, cy, '>', gpx, ink)) {
        // TTF not up yet: the bitmap font's own '>' (8x8 per scale step),
        // rather than reviving the hard-edged geometry this replaced.
        int sc = (s >= 32) ? 2 : 1;
        if (s >= 32)
            draw_text_scaled(cx - 4 * sc + 1, cy - 4 * sc + 1, ">",
                             LOGIN_SHADOW_COLOR, sc);
        draw_text_scaled(cx - 4 * sc, cy - 4 * sc, ">", ink, sc);
    }
}

// ============================================================================
// Backdrop (#569): FULL-RESOLUTION wallpaper, darkened only. The old block
// mosaic is gone - it is what made the wallpaper look pixelated and terrible.
// ============================================================================


// ============================================================================
// #745 BACKDROP CONTINUITY ACROSS A GATE RE-ENTRY.
//
// Reported: "the screen looked fine then i clicked switch user and the
// background image changed". The gate used to paint one FIXED default
// wallpaper regardless of what the session it had just replaced was showing,
// so every Switch User / Log Out on a machine whose wallpaper is not that one
// default is a visible image swap mid-session.
//
// The value needed is already in kernel memory. No per-user profile file is
// read and no new persistence format is introduced. MEASURED, not assumed:
// the compositor's frame loop (userland/apps/compositor/main.c, the
// SYS_GET_WALLPAPER poll) unconditionally force-syncs the displayed wallpaper
// to the kernel's ordinal every frame -
//     int wp = get_wallpaper();
//     if (wp >= 0 && wp != wallpaper_current()) wallpaper_load_progressive(wp);
// - so `g_wallpaper_idx` IS what is on screen, for every session, whether it
// was set by Settings, by the desktop picker, or replayed out of the user's
// own UIPROFIL.YML through set_wallpaper() at compositor start. Nothing clears
// it on logout, so it still holds the DEPARTING session's wallpaper at the
// moment login_run() is re-entered. That also makes it strictly more current
// than the profile file, which only reflects the last save.
//
// The stored value is an ORDINAL into wp_enumerate()'s scan order
// (userland/libc/wallpapers.c), not a filename, so resolving it means
// reproducing that scan exactly: the same directory walk (reached here through
// the documented in-kernel cores sys_open_k/sys_readdir_k, with no syscall
// boundary to cross since this is already Ring 0), the same *.BMP filter, the
// same BOOT.BMP/STUDIO.BMP blocklist, and the same order. Any divergence names
// the WRONG wallpaper, which is worse than the bug being fixed, so the filter
// below is a deliberate line-by-line mirror of that file.
// ============================================================================

typedef enum {
    LOGIN_WP_UNRESOLVED = 0,  // nothing to be continuous with; use the defaults
    LOGIN_WP_FILE,            // resolved to a BMP on the root
    LOGIN_WP_GRADIENT,        // the session was showing wp_enumerate()'s gradient
} login_wp_kind_t;

// Mirrors wp_is_bmp() in userland/libc/wallpapers.c.
static bool login_wp_is_bmp(const char *n) {
    int L = 0; while (n[L]) L++;
    if (L < 5) return false;                 // at least "x.BMP"
    const char *e = n + L - 4;
    char b = e[1], m = e[2], p = e[3];
    if (b >= 'a' && b <= 'z') b = (char)(b - 32);
    if (m >= 'a' && m <= 'z') m = (char)(m - 32);
    if (p >= 'a' && p <= 'z') p = (char)(p - 32);
    return e[0] == '.' && b == 'B' && m == 'M' && p == 'P';
}

// Mirrors wp_blocked(): non-wallpaper BMPs that also live at the root.
static bool login_wp_blocked(const char *n) {
    static const char *bl[] = { "BOOT.BMP", "STUDIO.BMP", 0 };
    for (int i = 0; bl[i]; i++) {
        const char *a = n, *b = bl[i];
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
            if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
            if (ca != cb) break;
            a++; b++;
        }
        if (*a == 0 && *b == 0) return true;
    }
    return false;
}

// Resolve a wp_enumerate() ordinal to a root-relative BMP path.
static login_wp_kind_t login_wallpaper_for_index(int idx, char *out, int cap) {
    if (idx < 0 || cap < 3) return LOGIN_WP_UNRESOLVED;
    int fd = (int)sys_open_k("/", 0);
    if (fd < 0) return LOGIN_WP_UNRESOLVED;

    sc_dirent_t de;
    int count = 0;
    login_wp_kind_t kind = LOGIN_WP_UNRESOLVED;
    // Same 1024-iteration guard wp_enumerate() uses against a pathological or
    // looping directory. Bounded by construction, no waiting of any kind.
    for (int guard = 0; guard < 1024; guard++) {
        if (sys_readdir_k(fd, &de) != 0) break;
        de.name[sizeof(de.name) - 1] = 0;
        if (!login_wp_is_bmp(de.name)) continue;
        if (login_wp_blocked(de.name)) continue;
        if (count == idx) {
            out[0] = '/';
            strncpy(out + 1, de.name, (size_t)cap - 2);
            out[cap - 1] = 0;
            kind = LOGIN_WP_FILE;
            break;
        }
        count++;
    }
    sys_close(fd);

    // wp_enumerate() appends its "Gradient (Blue)" entry AFTER the last BMP, so
    // an index exactly one past the last BMP means the departing session was
    // showing a gradient - and continuity means showing one here too, not
    // falling back to an image.
    if (kind == LOGIN_WP_UNRESOLVED && idx == count) kind = LOGIN_WP_GRADIENT;
    return kind;
}

// Decode one BMP path over the whole screen. Returns false if absent/undecodable.
static bool login_load_backdrop_file(const char *path, uint32_t sw, uint32_t sh) {
    uint32_t size = 0;
    void *data = fat_read_file_retry(&g_fat_fs, path, &size);
    bool ok = false;
    if (data && size > 54) {
        image_t img;
        if (image_load_bmp(data, size, &img) == IMAGE_SUCCESS) {
            image_blit_scaled(&img, 0, 0, sw, sh);
            image_free(&img);
            ok = true;
        }
    }
    if (data) kfree(data);
    return ok;
}

static void login_paint_backdrop_source(void) {
    uint32_t sw = login_ctx.screen_w;
    uint32_t sh = login_ctx.screen_h;
    bool have_wp = false;
    login_wp_kind_t cont = LOGIN_WP_UNRESOLVED;

    // TIER 1 - CONTINUITY (Switch User / Log Out). desktop_session_authenticated()
    // is 0 only on a true cold boot: desktop_set_session() sets it to 1 the first
    // time any account signs in this boot and nothing ever clears it, so a
    // non-zero value means a session has already run and there IS a departing
    // wallpaper to stay continuous with.
    if (g_fat_fs.mounted && desktop_session_authenticated()) {
        char cont_path[160];
        cont = login_wallpaper_for_index(syscall_get_wallpaper_idx(),
                                         cont_path, (int)sizeof(cont_path));
        if (cont == LOGIN_WP_FILE) {
            have_wp = login_load_backdrop_file(cont_path, sw, sh);
            // Resolved but unreadable (deleted between sessions, corrupt BMP):
            // fall through to the defaults rather than showing nothing.
            if (!have_wp) cont = LOGIN_WP_UNRESOLVED;
        }
    }

    // TIER 2 - the system default. Cold boot, or continuity that resolved to
    // nothing usable. Never reached when the departing session was showing the
    // gradient, because a gradient is what continuity means in that case.
    if (!have_wp && cont != LOGIN_WP_GRADIENT && g_fat_fs.mounted) {
        // #745: MAYTERA_MARBLE_LIGHT_GREY.BMP, not MAYTERA_MODERN.BMP, and only
        // for THIS pre-auth surface (gui/desktop.c's fallback desktop and the
        // compositor's WP_DEFAULT_FILE deliberately keep Modern - it is a fine
        // DESKTOP wallpaper). Modern is a poor auth backdrop on measured
        // grounds: it carries its own baked-in MayteraOS wordmark, which
        // competes with this screen's own "SIGN IN" eyebrow, and its local
        // luminance variance is std-dev 51.5 whole-frame / 43.9 in the band
        // where the pill and arrow sit, against 12.0 / 17.5 for Marble Light
        // Grey - roughly a quarter of the contrast noise behind exactly the
        // glyph strokes and pill edges that have to stay legible. Marble Light
        // Grey is already one of the user's curated wallpapers, so nothing is
        // added to or hidden from the picker. The older names stay as a
        // fallback in case a future asset base still carries one of them.
        static const char *cands[] = {"/MAYTERA_MARBLE_LIGHT_GREY.BMP",
                                      "/MAYTERA_MODERN.BMP", "/BACK.BMP",
                                      "/BACKGROUND.BMP", "/BG.BMP", NULL};
        for (int i = 0; cands[i] && !have_wp; i++)
            have_wp = login_load_backdrop_file(cands[i], sw, sh);
    }

    if (!have_wp) {
        gfx_gradient_rect(0, 0, sw, sh, LOGIN_GRAD_TOP, LOGIN_GRAD_BOT, 0);
    }

    // DARKEN ONLY. No mosaic, no blur: the wallpaper keeps its full resolution
    // and is simply dimmed so the floating text reads against it.
    fb_fill_rect_alpha(0, 0, sw, sh, LOGIN_SCRIM_COLOR, LOGIN_SCRIM_ALPHA);
}

// Build the backdrop once into a screen-sized cache. Reads back the finished
// pixels through fb_get_pixel so it is correct in both single- and double-
// buffered modes.
static void login_build_backdrop(void) {
    uint32_t sw = login_ctx.screen_w;
    uint32_t sh = login_ctx.screen_h;

    login_paint_backdrop_source();

    if (g_login_backdrop && (g_backdrop_w != sw || g_backdrop_h != sh)) {
        kfree(g_login_backdrop);
        g_login_backdrop = NULL;
    }
    if (!g_login_backdrop) {
        g_login_backdrop = (uint32_t *)kmalloc((size_t)sw * sh * 4);
        g_backdrop_w = sw;
        g_backdrop_h = sh;
    }
    if (g_login_backdrop) {
        for (uint32_t y = 0; y < sh; y++) {
            for (uint32_t x = 0; x < sw; x++) {
                g_login_backdrop[y * sw + x] = fb_get_pixel(x, y);
            }
        }
        g_backdrop_built = true;
    } else {
        // Cache alloc failed (unlikely): fall back to rebuilding each frame.
        g_backdrop_built = false;
    }
}

// Restore the cached backdrop into the active draw buffer (cheap: one memcpy
// per row via fb_put_row). This also means every frame repaints the WHOLE
// screen from the login gate's own composed source, so nothing stale from the
// pre-login boot screen can survive under a moving cursor (#569).
static void login_restore_backdrop(void) {
    uint32_t sw = login_ctx.screen_w;
    uint32_t sh = login_ctx.screen_h;
    if (g_backdrop_built && g_login_backdrop &&
        g_backdrop_w == sw && g_backdrop_h == sh) {
        for (uint32_t y = 0; y < sh; y++) {
            fb_put_row(0, y, sw, &g_login_backdrop[y * sw]);
        }
    } else {
        login_paint_backdrop_source();
    }
}

// ============================================================================
// Shared geometry: computed fresh from the same inputs by the renderer and the
// hit-testers so they cannot drift apart (lock.c idiom).
// ============================================================================

typedef struct {
    int sw, sh;
    int cx;
    int clock_px, clock_y;
    int date_y;
    int col_cy;             // vertical anchor of the credential column
    int name_y;
    int fx, fy, fw, fh;     // password pill
    int nx, ny;             // name pill (typed mode only), same w/h as above
    int ax, ay, asz;        // arrow (submit), now INSIDE the password pill
    int err_y;
    int back_y;
} login_geom_t;

static void login_geom(login_geom_t *g) {
    int sw = (int)login_ctx.screen_w;
    int sh = (int)login_ctx.screen_h;

    g->sw = sw;
    g->sh = sh;
    g->cx = sw / 2;

    // Clock: noticeably larger than the old 2x bitmap line (#569).
    // #745 (user's direct instruction, 2026-08-12): "move the clock and date
    // down about 10%" - was sh/10 (10% from the top), now sh/5 (20% from the
    // top, i.e. shifted down by ~10% of screen height). date_y is unchanged in
    // FORM (it always trails the clock by clock_px + 8) so it moves with it
    // automatically. Applied identically to lockscreen.c's lock_geom(): the two
    // gates mirror each other by design (see the credential-column comment
    // below), so moving only one here would reintroduce exactly the kind of
    // jump that comment exists to prevent. Checked against collision with the
    // credential column below at both a small (sh=480) and a large (sh>=800,
    // clock_px clamped to its 96 ceiling) framebuffer: date_y always lands well
    // above col_cy - LOGIN_NAME_PX/2 with room to spare.
    g->clock_px = sh / 8;
    if (g->clock_px < 48)  g->clock_px = 48;
    if (g->clock_px > 96)  g->clock_px = 96;   // top TTF display size
    g->clock_y = sh / 5;
    g->date_y  = g->clock_y + g->clock_px + 8;

    // #745 (2026-08-12): the credential group is CENTRED again, matching
    // lockscreen.c's lock_geom() (g->av_cy = sh/2 + sh/16, g->fx = g->cx -
    // g->fw/2). This REVERSES the bottom-left-quadrant placement from the
    // previous day (2026-08-11: "the far left of the input boxes should be at
    // 1/4 from the left" / "about 1/4 from the bottom", g->fx = sw/4, g->col_cy
    // = sh*3/4) - that was a real, explicit user instruction, and it shipped.
    // The user then hit it via Switch User, saw it land in a different spot
    // than the (still-centred) lock screen, and asked for it back the way it
    // was: "when i switch user the password entry box moves to the bottom
    // left, keep it as normal before we swap user - i know i requested this
    // before, but i dislike it" (2026-08-12). The kernel gate and the
    // compositor lock screen are two separate implementations of one control
    // (blame.md "Two sign-in surfaces, one square-off") and are meant to read
    // as the SAME screen; centring both is what makes Switch User a non-event
    // instead of a jump. Do not re-derive the bottom-left numbers from this
    // history and do not restore them without a fresh, explicit instruction -
    // this comment is the record that they were tried and reverted, not a
    // spec to reapply.
    g->col_cy = sh / 2 + sh / 16;
    g->name_y = g->col_cy - LOGIN_NAME_PX / 2;

    g->fw  = LOGIN_FIELD_W;
    g->fh  = LOGIN_FIELD_H;
    g->asz = LOGIN_ARROW_SZ;
    g->fy  = g->name_y + LOGIN_NAME_PX + 22;
    // Centred on its OWN width (matches lockscreen.c). g->cx therefore already
    // equals the field's own centre line (g->fx + g->fw/2 == g->cx), so
    // anything drawn centred on g->cx - the "Switch User" row included - lines
    // up with the field without needing a separate centre field for it.
    g->fx  = g->cx - g->fw / 2;
    g->ax  = g->fx + g->fw - g->asz - 10;
    g->ay  = g->fy + (g->fh - g->asz) / 2;

    // Typed mode only: the name field, 52px above the password field.
    g->nx  = g->fx;
    g->ny  = g->fy - 52;

    // The error row is RESERVED whether or not there is an error, so
    // "Switch User" never jumps when one appears or clears.
    g->err_y  = g->fy + g->fh + 14;
    g->back_y = g->err_y + 26;
}

// #745: how many accounts can sign in, counted FRESH on every call. Never
// cached: the count decides which screen is shown (a list of one is not a
// list, it is an extra keypress), and a stale count is a wrong screen. `sel`
// receives the Nth active account when it is non-NULL and `want` is in range.
static int login_active(int want, user_entry_t **sel) {
    if (sel) *sel = NULL;
    int user_count = 0;
    user_entry_t *users = users_get_table(&user_count);
    int n = 0;
    for (int i = 0; i < user_count; i++) {
        if (!users[i].active) continue;
        if (sel && n == want) *sel = &users[i];
        n++;
    }
    return n;
}

// Picker layout, derived from the SAME visible-row count the renderer and the
// hit-tester both pass in, so the two cannot disagree about where a row is.
static void login_list_geom(const login_geom_t *g, int nvis,
                            int *rows_y, int *eyebrow_y, int *hint_y) {
    int block = nvis > 0 ? (nvis - 1) * LOGIN_ROW_PITCH + LOGIN_ROW_H : 0;
    int top   = g->col_cy - block / 2;
    if (rows_y)    *rows_y    = top;
    if (eyebrow_y) *eyebrow_y = top - 26 - LOGIN_EYEBROW_PX;
    if (hint_y)    *hint_y    = top + block + 20;
}

// Keep the highlight and the scroll window consistent. Called from the key
// handler and from the renderer, so a table that changed underneath the gate
// cannot leave the highlight pointing off the end.
static void login_list_clamp(int total) {
    int nvis = total < LOGIN_ROWS_VISIBLE ? total : LOGIN_ROWS_VISIBLE;
    if (login_ctx.selected_user < 0) login_ctx.selected_user = 0;
    if (login_ctx.selected_user > total - 1) login_ctx.selected_user = total - 1;
    if (login_ctx.selected_user < 0) login_ctx.selected_user = 0;
    if (login_ctx.selected_user < login_ctx.list_first)
        login_ctx.list_first = login_ctx.selected_user;
    else if (login_ctx.selected_user >= login_ctx.list_first + nvis)
        login_ctx.list_first = login_ctx.selected_user - nvis + 1;
    if (login_ctx.list_first < 0) login_ctx.list_first = 0;
}

// Restart / Shut Down are plain shadowed TEXT now (no surrounds), so their hit
// rects are derived from the measured text extents.
static void login_power_rects(int *rx, int *rw, int *sx, int *sw_out,
                              int *py, int *ph) {
    int scr_w = (int)login_ctx.screen_w;
    int scr_h = (int)login_ctx.screen_h;
    *rw     = login_text_w("Restart", LOGIN_POWER_PX);
    *sw_out = login_text_w("Shut Down", LOGIN_POWER_PX);
    *ph     = LOGIN_POWER_PX + 8;
    *py     = scr_h - *ph - 24;
    *sx     = scr_w - 30 - *sw_out;
    *rx     = *sx - 32 - *rw;
}

// ============================================================================
// Login screen rendering
// ============================================================================

// Large live clock. Doubles as the liveness indicator (design 3.6): it visibly
// ticks, so a wedged screen is distinguishable from a live one.
static void login_draw_clock(const login_geom_t *g) {
    extern void rtc_read_time(int *hour, int *minute, int *second);
    extern void rtc_read_date(int *day, int *month, int *year, int *weekday);
    int h=0,m=0,sec=0,d=1,mo=1,yr=2026,wd=0;
    rtc_read_time(&h,&m,&sec);
    rtc_read_date(&d,&mo,&yr,&wd);
    (void)yr;
    char tbuf[8];
    snprintf(tbuf, sizeof(tbuf), "%02d:%02d", h, m);
    login_text_c(g->cx, g->clock_y, tbuf, g->clock_px, LOGIN_TEXT_COLOR);

    static const char *months[] = {"","January","February","March","April","May",
        "June","July","August","September","October","November","December"};
    static const char *days[] = {"Sunday","Monday","Tuesday","Wednesday",
        "Thursday","Friday","Saturday"};
    if (mo < 1 || mo > 12) mo = 1;
    if (wd < 0 || wd > 6) wd = 0;
    char dbuf[48];
    snprintf(dbuf, sizeof(dbuf), "%s, %s %d", days[wd], months[mo], d);
    login_text_c(g->cx, g->date_y, dbuf, LOGIN_DATE_PX, LOGIN_TEXT_DIM);
}

// Restart / Shut Down: high-contrast shadowed TEXT, no surrounds (#569).
static void login_draw_power(void) {
    int rx, rw, sx, sw_out, py, ph;
    login_power_rects(&rx, &rw, &sx, &sw_out, &py, &ph);
    login_text(rx, py + 4, "Restart", LOGIN_POWER_PX,
               login_ctx.hover_power == 1 ? LOGIN_TEXT_COLOR : LOGIN_TEXT_DIM);
    login_text(sx, py + 4, "Shut Down", LOGIN_POWER_PX,
               login_ctx.hover_power == 2 ? LOGIN_TEXT_COLOR : LOGIN_TEXT_DIM);
}

// ============================================================================
// #568 first-boot create-account flow (same panel-free look)
// ============================================================================

typedef struct {
    int cx;
    int title_y;
    int ix, iw, ih;
    int uy, py, cy, ry, qy;     // username, password, confirm, root, confirm-root
    int ax, ay, asz;
    int err_y;
} login_cgeom_t;

// #745: five fields now, not three. The row pitch drops from 78 to 62 and the
// block starts higher so the whole form still fits above the arrow button on a
// 600-line framebuffer (top=150, last row at 398, error at 458), not just on
// 768. The field positions are computed from ONE pitch so drawing, hit-testing
// and the caret can never disagree about where a row is.
#define LOGIN_CREATE_ROWS   5
#define LOGIN_CREATE_PITCH  62

static void login_create_geom(login_cgeom_t *c) {
    int sw = (int)login_ctx.screen_w;
    int sh = (int)login_ctx.screen_h;
    c->cx  = sw / 2;
    c->iw  = LOGIN_FIELD_W;
    c->ih  = LOGIN_FIELD_H;
    c->asz = LOGIN_FIELD_H;
    c->ix  = c->cx - (c->iw + 12 + c->asz) / 2;

    int span  = (LOGIN_CREATE_ROWS - 1) * LOGIN_CREATE_PITCH + c->ih;
    int top   = sh / 2 - span / 2 - 8;
    if (top < 96) top = 96;                 // never collide with the title
    c->title_y = top - 56;
    c->uy      = top;
    c->py      = top + 1 * LOGIN_CREATE_PITCH;
    c->cy      = top + 2 * LOGIN_CREATE_PITCH;
    c->ry      = top + 3 * LOGIN_CREATE_PITCH;
    c->qy      = top + 4 * LOGIN_CREATE_PITCH;
    c->ax      = c->ix + c->iw + 12;
    c->ay      = c->qy;
    c->err_y   = c->qy + c->ih + 14;
}

// Create-account view: same floating language as sign-in. One short prompt, no
// panel, rounded fields, arrow button to submit.
static void login_draw_create(void) {
    login_cgeom_t c;
    login_create_geom(&c);

    login_text_c(c.cx, c.title_y, "Create your account", LOGIN_NAME_PX, LOGIN_TEXT_COLOR);

    login_text(c.ix + 4, c.uy - 22, "Username", LOGIN_LABEL_PX, LOGIN_TEXT_FAINT);
    draw_pill_field(c.ix, c.uy, c.iw, c.ih, login_ctx.cu_user, false,
                    login_ctx.cu_field == 0, login_ctx.cursor_visible, "Username", 0, false);

    login_text(c.ix + 4, c.py - 22, "Password", LOGIN_LABEL_PX, LOGIN_TEXT_FAINT);
    draw_pill_field(c.ix, c.py, c.iw, c.ih, login_ctx.cu_pass, true,
                    login_ctx.cu_field == 1, login_ctx.cursor_visible, "Password", 0, false);

    login_text(c.ix + 4, c.cy - 22, "Confirm password", LOGIN_LABEL_PX, LOGIN_TEXT_FAINT);
    draw_pill_field(c.ix, c.cy, c.iw, c.ih, login_ctx.cu_confirm, true,
                    login_ctx.cu_field == 2, login_ctx.cursor_visible, "Confirm", 0, false);

    // #745: root's own password. Labelled for what it is, because "root
    // password" means nothing to someone who has never used a UNIX, and a
    // field whose purpose is unclear gets the same string typed into it as the
    // one above, which is the case this change exists to prevent.
    login_text(c.ix + 4, c.ry - 22, "Root password (must differ)", LOGIN_LABEL_PX, LOGIN_TEXT_FAINT);
    draw_pill_field(c.ix, c.ry, c.iw, c.ih, login_ctx.cu_root, true,
                    login_ctx.cu_field == 3, login_ctx.cursor_visible, "Root password", 0, false);

    login_text(c.ix + 4, c.qy - 22, "Confirm root password", LOGIN_LABEL_PX, LOGIN_TEXT_FAINT);
    draw_pill_field(c.ix, c.qy, c.iw, c.ih, login_ctx.cu_rootconf, true,
                    login_ctx.cu_field == 4, login_ctx.cursor_visible, "Confirm", 0, false);

    draw_arrow_button(c.ax, c.ay, c.asz, login_ctx.hover_login_btn);

    if (login_ctx.error_msg[0])
        login_text_c(c.cx, c.err_y, login_ctx.error_msg, LOGIN_BODY_PX, LOGIN_ERROR_COLOR);
}

static void login_draw(void) {
    // Darkened wallpaper (built once, then cheaply restored each frame).
    if (!g_backdrop_built) login_build_backdrop();
    login_restore_backdrop();

    login_geom_t g;
    login_geom(&g);

    login_draw_clock(&g);
    login_draw_power();

    if (login_ctx.state == LOGIN_STATE_SELECT_USER) {
        // #745: a VERTICAL, USERNAME-ONLY list. It replaces a horizontal strip of
        // at most five avatar discs, each with a drop-shadow disc behind it. That
        // strip was the last caller of draw_avatar(), and it was still shipping
        // long after "remove the drop-shadow avatar circle, username only" had
        // been applied to the OTHER sign-in screen.
        int total = login_active(-1, NULL);
        login_list_clamp(total);
        int nvis = total < LOGIN_ROWS_VISIBLE ? total : LOGIN_ROWS_VISIBLE;
        int rows_y, eyebrow_y, hint_y;
        login_list_geom(&g, nvis, &rows_y, &eyebrow_y, &hint_y);

        // #745 (user's direct instruction): "SIGN IN" eyebrow REMOVED here too.
        (void)eyebrow_y;

        int rx = g.cx - LOGIN_ROW_W / 2;
        for (int r = 0; r < nvis; r++) {
            int idx = login_ctx.list_first + r;
            user_entry_t *u = NULL;
            (void)login_active(idx, &u);
            if (!u) continue;
            int ry = rows_y + r * LOGIN_ROW_PITCH;
            bool on = (idx == login_ctx.selected_user);
            // State is carried by THREE independent signals, not by the fill
            // alone: the fill, the near-white text, and the row being the only
            // one the arrow keys are on. The fill measures 1.1:1 against the
            // backdrop by itself and is a supporting cue only.
            if (on) decor_fill_rounded_rect(rx, ry, LOGIN_ROW_W, LOGIN_ROW_H, 6,
                                            LOGIN_ROW_SEL_FILL);
            login_text_c(g.cx, ry + (LOGIN_ROW_H - LOGIN_EYEBROW_PX) / 2,
                         u->display_name[0] ? u->display_name : u->username,
                         LOGIN_EYEBROW_PX,
                         on ? LOGIN_TEXT_COLOR : LOGIN_TEXT_DIM);
        }
        login_text_c(g.cx, hint_y, "Use the arrow keys, then press Enter.",
                     LOGIN_LABEL_PX, LOGIN_TEXT_FAINT);

    } else if (login_ctx.state == LOGIN_STATE_TYPED) {
        // #745 name + password. NO account name appears anywhere on this screen,
        // which is the entire promise of the mode: not the username heading, not
        // a placeholder carrying a real name, and not an error message that says
        // which of the two fields was wrong.
        bool err = (login_ctx.error_msg[0] != '\0');
        // #745 (user's direct instruction): "SIGN IN" eyebrow REMOVED. Nothing
        // else was anchored to it - the username box sits at its own g.ny - so
        // there is no gap to re-anchor. "Name" is now "Username".
        draw_pill_field(g.nx, g.ny, g.fw, g.fh, login_ctx.tu_user, false,
                        login_ctx.tu_field == 0, login_ctx.cursor_visible,
                        "Username", 0, err);
        draw_pill_field(g.fx, g.fy, g.fw, g.fh, login_ctx.password, true,
                        login_ctx.tu_field == 1, login_ctx.cursor_visible,
                        "Password", g.asz, err);
        draw_arrow_button(g.ax, g.ay, g.asz, login_ctx.hover_login_btn);
        // The group is left-anchored now, so the error row is left-anchored to
        // the box column too rather than centred on the whole screen.
        if (err)
            login_text(g.fx, g.err_y, login_ctx.error_msg, LOGIN_BODY_PX,
                       LOGIN_ERROR_COLOR);
        // No "Switch User" here: the name field IS the switcher.

    } else if (login_ctx.state == LOGIN_STATE_PASSWORD) {
        user_entry_t *sel_user = NULL;
        int active_total = login_active(login_ctx.selected_user, &sel_user);

        if (sel_user) {
            bool err = (login_ctx.error_msg[0] != '\0');
            // Username only, floating, no box and no disc.
            login_text_c(g.cx, g.name_y,
                         sel_user->display_name[0] ? sel_user->display_name : sel_user->username,
                         LOGIN_NAME_PX, LOGIN_TEXT_COLOR);

            // Rounded password field with the arrow INSIDE its right-hand end.
            draw_pill_field(g.fx, g.fy, g.fw, g.fh, login_ctx.password, true,
                            true, login_ctx.cursor_visible, "Password", g.asz, err);
            draw_arrow_button(g.ax, g.ay, g.asz, login_ctx.hover_login_btn);

            if (err)
                login_text_c(g.cx, g.err_y, login_ctx.error_msg, LOGIN_BODY_PX,
                             LOGIN_ERROR_COLOR);

            // Switch User: shadowed text, no surround. Only meaningful when
            // there is more than one account to switch to, and only in LIST
            // mode - in typed mode there is no list to go back to.
            if (active_total > 1 && login_ctx.mode == LOGIN_MODE_LIST)
                login_text_c(g.cx, g.back_y, "Switch User", LOGIN_BODY_PX, LOGIN_TEXT_DIM);
        }
    } else if (login_ctx.state == LOGIN_STATE_CREATE_ACCOUNT) {
        login_draw_create();
    }

    // Draw mouse cursor
    int32_t mx, my;
    mouse_get_position(&mx, &my);
    extern void desktop_draw_cursor(int32_t x, int32_t y);
    desktop_draw_cursor(mx, my);

    // Swap buffers
    fb_swap_buffers();
}

// ============================================================================
// Login screen event handling
// ============================================================================

// Returns 1 if a power control was clicked (and acted on). Power controls are
// available WITHOUT authenticating (mirrors the lock screen).
static int login_power_click(int32_t mx, int32_t my) {
    int rx, rw, sx, sw_out, py, ph;
    login_power_rects(&rx, &rw, &sx, &sw_out, &py, &ph);
    if (my >= py && my < py + ph) {
        if (mx >= rx && mx < rx + rw) {
            bootlog_write("[LOGIN] power: Restart from login gate");
            acpi_reboot();
            return 1;
        }
        if (mx >= sx && mx < sx + sw_out) {
            bootlog_write("[LOGIN] power: Shut Down from login gate");
            acpi_shutdown();
            return 1;
        }
    }
    return 0;
}

static void login_handle_mouse_click(int32_t mx, int32_t my) {
    login_geom_t g;
    login_geom(&g);

    // Power controls first (both states).
    if (login_power_click(mx, my)) return;

    if (login_ctx.state == LOGIN_STATE_SELECT_USER) {
        int total = login_active(-1, NULL);
        login_list_clamp(total);
        int nvis = total < LOGIN_ROWS_VISIBLE ? total : LOGIN_ROWS_VISIBLE;
        int rows_y;
        login_list_geom(&g, nvis, &rows_y, NULL, NULL);
        int rx = g.cx - LOGIN_ROW_W / 2;
        for (int r = 0; r < nvis; r++) {
            int ry = rows_y + r * LOGIN_ROW_PITCH;
            if (mx >= rx && mx < rx + LOGIN_ROW_W && my >= ry && my < ry + LOGIN_ROW_H) {
                login_ctx.selected_user = login_ctx.list_first + r;
                login_ctx.state = LOGIN_STATE_PASSWORD;
                login_ctx.password[0] = '\0';
                login_ctx.password_pos = 0;
                login_ctx.error_msg[0] = '\0';
                return;
            }
        }

    } else if (login_ctx.state == LOGIN_STATE_TYPED) {
        if (mx >= g.nx && mx < g.nx + g.fw && my >= g.ny && my < g.ny + g.fh) {
            login_ctx.tu_field = 0; return;
        }
        if (mx >= g.fx && mx < g.fx + g.fw && my >= g.fy && my < g.fy + g.fh) {
            // The arrow lives inside the field now, so it is tested FIRST: a
            // click on it must submit, not merely focus the field underneath.
            if (mx >= g.ax && mx < g.ax + g.asz &&
                my >= g.ay && my < g.ay + g.asz) {
                login_ctx.state = LOGIN_STATE_SUCCESS;
                return;
            }
            login_ctx.tu_field = 1; return;
        }

    } else if (login_ctx.state == LOGIN_STATE_PASSWORD) {
        // Arrow button = submit (same action as Enter).
        if (mx >= g.ax && mx < g.ax + g.asz &&
            my >= g.ay && my < g.ay + g.asz) {
            login_ctx.state = LOGIN_STATE_SUCCESS;  // checked by the main loop
            return;
        }

        // "Switch User" text affordance. Only in LIST mode, and only when there
        // is more than one account: the drawn control and this hit-test come
        // from the same two conditions.
        int active_total = login_active(-1, NULL);
        if (active_total > 1 && login_ctx.mode == LOGIN_MODE_LIST) {
            int lw = login_text_w("Switch User", LOGIN_BODY_PX);
            if (my >= g.back_y && my < g.back_y + LOGIN_BODY_PX + 8 &&
                mx >= g.cx - lw / 2 && mx < g.cx + lw / 2) {
                login_ctx.state = LOGIN_STATE_SELECT_USER;
                login_ctx.password[0] = '\0';
                login_ctx.password_pos = 0;
                login_ctx.error_msg[0] = '\0';
            }
        }
    } else if (login_ctx.state == LOGIN_STATE_CREATE_ACCOUNT) {
        login_cgeom_t c;
        login_create_geom(&c);
        if (mx >= c.ix && mx < c.ix + c.iw) {
            if (my >= c.uy && my < c.uy + c.ih) { login_ctx.cu_field = 0; return; }
            if (my >= c.py && my < c.py + c.ih) { login_ctx.cu_field = 1; return; }
            if (my >= c.cy && my < c.cy + c.ih) { login_ctx.cu_field = 2; return; }
            if (my >= c.ry && my < c.ry + c.ih) { login_ctx.cu_field = 3; return; }
            if (my >= c.qy && my < c.qy + c.ih) { login_ctx.cu_field = 4; return; }
        }
        if (mx >= c.ax && mx < c.ax + c.asz &&
            my >= c.ay && my < c.ay + c.asz) {
            login_ctx.cu_submit_requested = true;
            return;
        }
    }
}

static void login_handle_mouse_move(int32_t mx, int32_t my) {
    login_geom_t g;
    login_geom(&g);

    login_ctx.hover_user = -1;
    login_ctx.hover_login_btn = false;
    login_ctx.hover_power = 0;

    // Power control hover (both states).
    {
        int rx, rw, sx, sw_out, py, ph;
        login_power_rects(&rx, &rw, &sx, &sw_out, &py, &ph);
        if (my >= py && my < py + ph) {
            if (mx >= rx && mx < rx + rw) login_ctx.hover_power = 1;
            else if (mx >= sx && mx < sx + sw_out) login_ctx.hover_power = 2;
        }
    }

    if (login_ctx.state == LOGIN_STATE_SELECT_USER) {
        int total = login_active(-1, NULL);
        int nvis = total < LOGIN_ROWS_VISIBLE ? total : LOGIN_ROWS_VISIBLE;
        int rows_y;
        login_list_geom(&g, nvis, &rows_y, NULL, NULL);
        int rx = g.cx - LOGIN_ROW_W / 2;
        for (int r = 0; r < nvis; r++) {
            int ry = rows_y + r * LOGIN_ROW_PITCH;
            if (mx >= rx && mx < rx + LOGIN_ROW_W && my >= ry && my < ry + LOGIN_ROW_H) {
                login_ctx.hover_user = login_ctx.list_first + r;
                break;
            }
        }

    } else if (login_ctx.state == LOGIN_STATE_PASSWORD ||
               login_ctx.state == LOGIN_STATE_TYPED) {
        if (mx >= g.ax && mx < g.ax + g.asz &&
            my >= g.ay && my < g.ay + g.asz) {
            login_ctx.hover_login_btn = true;
        }
    } else if (login_ctx.state == LOGIN_STATE_CREATE_ACCOUNT) {
        login_cgeom_t c;
        login_create_geom(&c);
        if (mx >= c.ax && mx < c.ax + c.asz &&
            my >= c.ay && my < c.ay + c.asz) {
            login_ctx.hover_login_btn = true;
        }
    }
}

// Try to authenticate the selected user
static int login_attempt(login_result_t *result) {
    user_entry_t *sel_user = NULL;
    (void)login_active(login_ctx.selected_user, &sel_user);

    if (!sel_user) {
        strncpy(login_ctx.error_msg, "Invalid user selection", sizeof(login_ctx.error_msg) - 1);
        login_ctx.state = LOGIN_STATE_PASSWORD;
        return -1;
    }

    // #566: authenticate through the RATE-LIMITED path so the kernel login gate
    // shares the same per-account failed-attempt lockout as every other auth
    // path (an empty password is rejected here too). Raw user_verify_password()
    // had no throttling.
    int ar = users_authenticate(sel_user->username, login_ctx.password);
    if (ar != 0) {
        if (ar == -2) {
            int secs = users_get_lockout(sel_user->username);
            snprintf(login_ctx.error_msg, sizeof(login_ctx.error_msg),
                     "Too many attempts. Try again in %ds.", secs);
        } else {
            strncpy(login_ctx.error_msg, "Incorrect password. Try again.",
                    sizeof(login_ctx.error_msg) - 1);
        }
        login_ctx.password[0] = '\0';
        login_ctx.password_pos = 0;
        login_ctx.state = LOGIN_STATE_PASSWORD;
        return -1;
    }

    // Success! Fill result
    result->uid = sel_user->uid;
    result->gid = sel_user->gid;
    strncpy(result->username, sel_user->username, sizeof(result->username) - 1);
    strncpy(result->home, sel_user->home, sizeof(result->home) - 1);

    kprintf("[LOGIN] User '%s' (uid=%u) authenticated successfully\n",
            sel_user->username, sel_user->uid);

    return 0;
}

// #745 typed mode: authenticate a name the person supplied.
//
// EVERY failure produces the SAME message. Not because one string is tidier,
// but because the mode's promise is that no account name is disclosed, and
// "no such user" versus "wrong password" discloses exactly that, one guess at a
// time. The lockout countdown is suppressed here for the same reason: a
// per-account countdown confirms the account exists.
//
// users_authenticate() already does a dummy verify for a name with no shadow
// record specifically to keep the timing uniform, so the two cases are not
// trivially separable by a stopwatch either.
static int login_attempt_typed(login_result_t *result) {
    int ar = users_authenticate(login_ctx.tu_user, login_ctx.password);
    user_entry_t *u = (ar == 0) ? user_lookup_name(login_ctx.tu_user) : NULL;
    if (ar != 0 || !u || !u->active) {
        strncpy(login_ctx.error_msg,
                ar == -2 ? "Too many attempts. Try again later."
                         : "Incorrect name or password.",
                sizeof(login_ctx.error_msg) - 1);
        login_ctx.error_msg[sizeof(login_ctx.error_msg) - 1] = '\0';
        // The field clears and keeps focus; the error border is held until the
        // next keystroke (which is what clears error_msg).
        login_ctx.password[0] = '\0';
        login_ctx.password_pos = 0;
        login_ctx.tu_field = 1;
        login_ctx.state = LOGIN_STATE_TYPED;
        return -1;
    }

    result->uid = u->uid;
    result->gid = u->gid;
    strncpy(result->username, u->username, sizeof(result->username) - 1);
    strncpy(result->home, u->home, sizeof(result->home) - 1);
    kprintf("[LOGIN] User '%s' (uid=%u) authenticated successfully (typed)\n",
            u->username, u->uid);
    return 0;
}

// ============================================================================
// Public API
// ============================================================================

void login_init(void) {
    memset(&login_ctx, 0, sizeof(login_ctx));
    // #745 CO-REQUISITE BUG. login_draw() only calls login_build_backdrop()
    // when !g_backdrop_built, and NOTHING reset that flag between gate visits:
    // login_init() memsets login_ctx, which is a different object, and the
    // main.c login loop does not touch it either. The only path that cleared it
    // was a kmalloc failure inside login_build_backdrop() itself. So the kernel
    // gate composed its backdrop ONCE PER BOOT and then reused that exact
    // cached bitmap on every subsequent Log Out / Switch User for the rest of
    // the boot. Without this line the continuity fix and the new default below
    // would both be invisible from the SECOND gate visit onward.
    g_backdrop_built = false;
    login_ctx.state = LOGIN_STATE_SELECT_USER;
    login_ctx.selected_user = -1;
    // #745: TYPED until the config says otherwise. login_run() overwrites
    // this from /CONFIG/LOGIN.CFG; the initialiser matters because
    // login_lock_screen() calls login_init() and goes straight to the
    // password view, and the non-disclosing value is the right one to hold
    // whenever nothing has been read.
    login_ctx.mode = LOGIN_MODE_TYPED;
    login_ctx.list_first = 0;
    login_ctx.tu_field = 0;
    login_ctx.hover_user = -1;
    login_ctx.screen_w = fb_get_width();
    login_ctx.screen_h = fb_get_height();
    login_ctx.cursor_visible = true;
    login_ctx.cursor_blink_tick = timer_ticks;
    login_ctx.initialized = true;

    // #569 repaint-artifact fix: from here on the login gate owns the display.
    // Release the boot splash so late background logging (the periodic xHCI
    // rescan worker calls gfx_boot_log() on every re-enumeration, and that used
    // to paint the boot-log console into the back buffer AND swap it to the
    // screen) can never flash over this screen again.
    gfx_boot_release_display();

    // #307 real-hardware bring-up: this is the kernel-side login screen
    // (main.c calls login_init()/login_run() before desktop_run()). Log the
    // user count it will render, same reasoning as the userland compositor's
    // login screen - whichever one is actually on screen when a real-hardware
    // boot gets stuck, this makes it visible in the persistent boot log.
    int lc = 0;
    users_get_table(&lc);
    bootlog_write("[LOGIN] kernel login_init: %d user(s) in table", lc);
}

int login_check_autologin(login_result_t *result) {
    if (!g_fat_fs.mounted) {
        // #307: FAT not mounted - autologin impossible; record for on-screen diag.
        strncpy(g_autologin_debug,
                "autologin: FAT not mounted", sizeof(g_autologin_debug) - 1);
        bootlog_write("[LOGIN] autologin: FAT not mounted; cannot read /CONFIG/LOGIN.CFG");
        return 0;
    }

    // #307: use the shared bounded-retry reader (same primitive proc/users.c
    // uses for PASSWD/SHADOW/GROUP). Real USB-MSC/ATA hardware can return a
    // single transient NULL/zero-size read that a plain fat_read_file() would
    // treat as "no autologin", dropping the machine onto the interactive stub.
    uint32_t size = 0;
    void *data = fat_read_file_retry(&g_fat_fs, "/CONFIG/LOGIN.CFG", &size);
    if (!data || size == 0) {
        snprintf(g_autologin_debug, sizeof(g_autologin_debug),
                  "autologin: LOGIN.CFG read failed data=%p size=%u",
                  data, (unsigned)size);
        bootlog_write("[LOGIN] autologin: /CONFIG/LOGIN.CFG read FAILED after retries "
                      "(data=%p size=%u)", data, (unsigned)size);
        if (data) kfree(data);
        return 0;
    }

    // Parse simple key=value format
    // Look for "autologin=username"
    const char *src = (const char *)data;
    const char *end = src + size;
    char autologin_user[32] = {0};

    while (src < end) {
        // Skip whitespace/newlines
        while (src < end && (*src == ' ' || *src == '\n' || *src == '\r' || *src == '\t')) src++;
        if (src >= end) break;

        if (strncmp(src, "autologin=", 10) == 0) {
            src += 10;
            int i = 0;
            while (src < end && *src != '\n' && *src != '\r' && i < 31) {
                autologin_user[i++] = *src++;
            }
            autologin_user[i] = '\0';
            break;
        }

        // Skip to next line
        while (src < end && *src != '\n') src++;
    }

    kfree(data);

    if (autologin_user[0]) {
        user_entry_t *u = user_lookup_name(autologin_user);
        if (u) {
            result->uid = u->uid;
            result->gid = u->gid;
            strncpy(result->username, u->username, sizeof(result->username) - 1);
            strncpy(result->home, u->home, sizeof(result->home) - 1);
            g_autologin_debug[0] = '\0';  // success: nothing to show
            kprintf("[LOGIN] Auto-login as '%s' (uid=%u)\n", u->username, u->uid);
            bootlog_write("[LOGIN] autologin: OK as '%s' (uid=%u)", u->username, u->uid);
            return 1;
        }
        // #307: parsed a name but no matching account (PASSWD not loaded yet?).
        snprintf(g_autologin_debug, sizeof(g_autologin_debug),
                  "autologin: user '%s' not found", autologin_user);
        bootlog_write("[LOGIN] autologin: user '%s' parsed but user_lookup_name failed",
                      autologin_user);
        return 0;
    }

    // #307/#566: LOGIN.CFG read OK but no autologin= line - this is the NORMAL
    // "no autologin configured" case, not a failure. Do NOT clutter the login
    // screen with a red diagnostic for it; just log it.
    g_autologin_debug[0] = '\0';
    bootlog_write("[LOGIN] autologin: LOGIN.CFG read OK but no 'autologin=' key present");
    return 0;
}

// #568: validate the create-account fields and, on success, create the first
// administrator (uid 0) and fill the login result. Returns 0 on success (caller
// proceeds to the desktop), -1 to stay on the create screen with error_msg set.
static int login_create_submit(login_result_t *result) {
    if (login_ctx.cu_user[0] == '\0') {
        strncpy(login_ctx.error_msg, "Enter a username", sizeof(login_ctx.error_msg) - 1);
        login_ctx.cu_field = 0;
        return -1;
    }
    // The SAME check the kernel will run when it sets the passwords, so this
    // screen cannot accept something the chokepoint then refuses. It used to
    // be a local 6-character test that agreed with nothing else. #745: it is
    // now the PAIR check, so it also covers root's password and the
    // must-differ rule, and the band in the return code says which of the two
    // fields to put the caret in.
    {
        int pc = users_check_first_boot_pair(login_ctx.cu_user,
                                             login_ctx.cu_pass,
                                             login_ctx.cu_root);
        if (pc != 0) {
            const char *m = "Could not create account";
            if (PW_RC_IS_POLICY(pc))      { m = pw_policy_message(PW_RC_CODE(pc));      login_ctx.cu_field = 1; }
            else if (PW_RC_ROOT_IS_POLICY(pc)) { m = pw_policy_message(PW_RC_ROOT_CODE(pc)); login_ctx.cu_field = 3; }
            strncpy(login_ctx.error_msg, m, sizeof(login_ctx.error_msg) - 1);
            login_ctx.error_msg[sizeof(login_ctx.error_msg) - 1] = '\0';
            return -1;
        }
    }
    if (strcmp(login_ctx.cu_pass, login_ctx.cu_confirm) != 0) {
        strncpy(login_ctx.error_msg, "Passwords do not match", sizeof(login_ctx.error_msg) - 1);
        login_ctx.cu_confirm[0] = '\0';
        login_ctx.cu_field = 2;
        return -1;
    }
    if (strcmp(login_ctx.cu_root, login_ctx.cu_rootconf) != 0) {
        strncpy(login_ctx.error_msg, "Root passwords do not match", sizeof(login_ctx.error_msg) - 1);
        login_ctx.cu_rootconf[0] = '\0';
        login_ctx.cu_field = 4;
        return -1;
    }

    int r = users_create_first_admin(login_ctx.cu_user, login_ctx.cu_pass,
                                     login_ctx.cu_root);
    if (r != 0) {
        if (r == -2)
            strncpy(login_ctx.error_msg, "Invalid username (no spaces, ':' or 'root')",
                    sizeof(login_ctx.error_msg) - 1);
        else if (PW_RC_IS_POLICY(r))
            strncpy(login_ctx.error_msg, pw_policy_message(PW_RC_CODE(r)),
                    sizeof(login_ctx.error_msg) - 1);
        else if (PW_RC_ROOT_IS_POLICY(r))
            strncpy(login_ctx.error_msg, pw_policy_message(PW_RC_ROOT_CODE(r)),
                    sizeof(login_ctx.error_msg) - 1);
        else
            strncpy(login_ctx.error_msg, "Could not create account",
                    sizeof(login_ctx.error_msg) - 1);
        return -1;
    }

    user_entry_t *u = user_lookup_name(login_ctx.cu_user);
    if (!u) {
        strncpy(login_ctx.error_msg, "Could not create account",
                sizeof(login_ctx.error_msg) - 1);
        return -1;
    }
    result->uid = u->uid;
    result->gid = u->gid;
    strncpy(result->username, u->username, sizeof(result->username) - 1);
    strncpy(result->home, u->home, sizeof(result->home) - 1);
    // The password is hashed + persisted; scrub the plaintext from RAM.
    memset(login_ctx.cu_pass, 0, sizeof(login_ctx.cu_pass));
    memset(login_ctx.cu_confirm, 0, sizeof(login_ctx.cu_confirm));
    memset(login_ctx.cu_root, 0, sizeof(login_ctx.cu_root));
    memset(login_ctx.cu_rootconf, 0, sizeof(login_ctx.cu_rootconf));
    kprintf("[LOGIN] First-boot account '%s' (uid=%u) created\n", u->username, u->uid);
    bootlog_write("[LOGIN] first-boot account '%s' created; entering desktop (#568)", u->username);
    return 0;
}

int login_run(login_result_t *result) {
    if (!login_ctx.initialized) {
        login_init();
    }

    // Check for auto-login first
    if (login_check_autologin(result)) {
        return 0;
    }

    // #568 first-boot: no real accounts exist yet (fresh/public install with no
    // shipped credentials). Force account creation instead of an empty user
    // picker; the account created here becomes the administrator (uid 0).
    if (users_count_active() == 0) {
        login_ctx.state = LOGIN_STATE_CREATE_ACCOUNT;
        login_ctx.cu_user[0] = '\0';
        login_ctx.cu_pass[0] = '\0';
        login_ctx.cu_confirm[0] = '\0';
        login_ctx.cu_root[0] = '\0';
        login_ctx.cu_rootconf[0] = '\0';
        login_ctx.cu_field = 0;
        login_ctx.cu_submit_requested = false;
        login_ctx.error_msg[0] = '\0';
        bootlog_write("[LOGIN] first-boot: 0 accounts present; showing create-account flow (#568)");
    }

    // #745: which sign-in screen? Read the configured mode ONCE, here. The
    // parse is rustkern/loginmode.rs and the read is proc/syscall.c, so this
    // gate and SYS_GET_LOGIN_MODE cannot disagree about what the file says.
    //
    // ANY failure to read resolves to TYPED. That is a deliberate BEHAVIOUR
    // CHANGE for an image that never ran the setup wizard: it used to show the
    // account picker, and it now shows the typed screen. Showing every account
    // name on the machine is a disclosure, and a disclosure should follow a
    // recorded decision, never a missing file. Every machine that completes
    // setup writes the key explicitly, so this affects only never-configured
    // images. The STARTUP fallback is unchanged and is still "require login".
    login_ctx.mode = login_mode_configured();
    bootlog_write("[LOGIN] sign-in screen mode: %s",
                  login_ctx.mode == LOGIN_MODE_LIST ? "list" : "typed");

    if (login_ctx.state == LOGIN_STATE_SELECT_USER) {
        if (login_ctx.mode == LOGIN_MODE_TYPED) {
            login_ctx.state = LOGIN_STATE_TYPED;
            login_ctx.tu_user[0] = '\0';
            login_ctx.tu_field = 0;
            login_ctx.password[0] = '\0';
            login_ctx.password_pos = 0;
        } else if (login_active(-1, NULL) == 1) {
            // #569/#745: LIST mode with exactly one account renders the
            // single-account view directly. A list of one is not a list, it is
            // an extra keypress and an extra screen for no decision. The setting
            // stays recorded as LIST, so the moment a second account exists the
            // list appears without anyone going back to Settings.
            login_ctx.selected_user = 0;
            login_ctx.password[0] = '\0';
            login_ctx.password_pos = 0;
            login_ctx.state = LOGIN_STATE_PASSWORD;
        } else {
            login_ctx.selected_user = 0;   // highlight the first row
            login_ctx.list_first = 0;
        }
    }

    // Set mouse bounds
    mouse_set_bounds(0, 0, login_ctx.screen_w - 1, login_ctx.screen_h - 1);

    kprintf("[LOGIN] Login screen active\n");

    // Main login loop
    while (1) {
        // Handle keyboard input
        if (keyboard_has_char()) {
            int key = keyboard_get_char();

            if (login_ctx.state == LOGIN_STATE_SELECT_USER) {
                int active_count = login_active(-1, NULL);
                // #745: arrows move the highlight, clamped, no wrap; Enter
                // chooses. Tab behaves as Down. The number keys are kept because
                // they already worked and cost nothing.
                if (key == LOGIN_KEY_UP || key == LOGIN_KEY_DOWN || key == '\t') {
                    login_ctx.selected_user += (key == LOGIN_KEY_UP) ? -1 : 1;
                    login_list_clamp(active_count);
                } else if (key == '\n') {
                    if (login_ctx.selected_user >= 0 &&
                        login_ctx.selected_user < active_count) {
                        login_ctx.state = LOGIN_STATE_PASSWORD;
                        login_ctx.password[0] = '\0';
                        login_ctx.password_pos = 0;
                        login_ctx.error_msg[0] = '\0';
                    }
                } else if (key >= '1' && key <= '9') {
                    int idx = key - '1';
                    if (idx < active_count) {
                        login_ctx.selected_user = idx;
                        login_list_clamp(active_count);
                        login_ctx.state = LOGIN_STATE_PASSWORD;
                        login_ctx.password[0] = '\0';
                        login_ctx.password_pos = 0;
                        login_ctx.error_msg[0] = '\0';
                    }
                }
            } else if (login_ctx.state == LOGIN_STATE_TYPED) {
                // Name field <-> password field on Tab; Enter advances from the
                // name and submits from the password; Esc clears both.
                char *f = (login_ctx.tu_field == 0) ? login_ctx.tu_user : login_ctx.password;
                size_t fmax = (login_ctx.tu_field == 0) ? sizeof(login_ctx.tu_user)
                                                        : sizeof(login_ctx.password);
                if (key == '\n') {
                    if (login_ctx.tu_field == 0) login_ctx.tu_field = 1;
                    else if (login_attempt_typed(result) == 0) return 0;
                } else if (key == '\t') {
                    login_ctx.tu_field ^= 1;
                } else if (key == 27) {
                    login_ctx.tu_user[0] = '\0';
                    login_ctx.password[0] = '\0';
                    login_ctx.password_pos = 0;
                    login_ctx.tu_field = 0;
                    login_ctx.error_msg[0] = '\0';
                } else if (key == '\b') {
                    size_t l = strlen(f);
                    if (l > 0) f[l - 1] = '\0';
                    login_ctx.error_msg[0] = '\0';   // clears the error border
                } else if (key >= ' ' && key < 127) {
                    // The name field carries the same two exclusions the
                    // create-account form uses: no space, no ':' (the PASSWD
                    // field delimiter).
                    if (!(login_ctx.tu_field == 0 && (key == ' ' || key == ':'))) {
                        size_t l = strlen(f);
                        if (l < fmax - 1) { f[l] = (char)key; f[l + 1] = '\0'; }
                    }
                    login_ctx.error_msg[0] = '\0';
                }
                login_ctx.password_pos = (int)strlen(login_ctx.password);
            } else if (login_ctx.state == LOGIN_STATE_PASSWORD) {
                if (key == '\n') {
                    // Attempt login
                    if (login_attempt(result) == 0) {
                        return 0;  // Success
                    }
                } else if (key == '\b') {
                    // Backspace
                    if (login_ctx.password_pos > 0) {
                        login_ctx.password_pos--;
                        login_ctx.password[login_ctx.password_pos] = '\0';
                    }
                } else if (key == 27) {
                    // ESC - back to the account list. #745: only where there IS
                    // one. In typed mode this state is reached from a screen
                    // that has no list, and in LIST mode with a single account
                    // the picker is deliberately skipped, so Esc would drop the
                    // person onto a one-row screen they were never shown.
                    if (login_ctx.mode == LOGIN_MODE_LIST && login_active(-1, NULL) > 1) {
                        login_ctx.state = LOGIN_STATE_SELECT_USER;
                        login_ctx.password[0] = '\0';
                        login_ctx.password_pos = 0;
                        login_ctx.error_msg[0] = '\0';
                    }
                } else if (key >= ' ' && key < 127) {
                    // Printable character
                    if (login_ctx.password_pos < LOGIN_MAX_PASSWORD - 1) {
                        login_ctx.password[login_ctx.password_pos++] = (char)key;
                        login_ctx.password[login_ctx.password_pos] = '\0';
                    }
                    login_ctx.error_msg[0] = '\0';   // clears the error border
                }
            } else if (login_ctx.state == LOGIN_STATE_SUCCESS) {
                // Check authentication
                if (login_attempt(result) == 0) {
                    return 0;
                }
            } else if (login_ctx.state == LOGIN_STATE_CREATE_ACCOUNT) {
                char *f; size_t fmax;
                if (login_ctx.cu_field == 0)      { f = login_ctx.cu_user;     fmax = sizeof(login_ctx.cu_user); }
                else if (login_ctx.cu_field == 1) { f = login_ctx.cu_pass;     fmax = sizeof(login_ctx.cu_pass); }
                else if (login_ctx.cu_field == 2) { f = login_ctx.cu_confirm;  fmax = sizeof(login_ctx.cu_confirm); }
                else if (login_ctx.cu_field == 3) { f = login_ctx.cu_root;     fmax = sizeof(login_ctx.cu_root); }
                else                              { f = login_ctx.cu_rootconf; fmax = sizeof(login_ctx.cu_rootconf); }

                if (key == '\n') {
                    // Enter advances through the fields, then submits on the last.
                    if (login_ctx.cu_field < LOGIN_CREATE_ROWS - 1) {
                        login_ctx.cu_field++;
                    } else if (login_create_submit(result) == 0) {
                        return 0;
                    }
                } else if (key == '\t') {
                    login_ctx.cu_field = (login_ctx.cu_field + 1) % LOGIN_CREATE_ROWS;
                } else if (key == '\b') {
                    size_t l = strlen(f);
                    if (l > 0) f[l - 1] = '\0';
                } else if (key == 27) {
                    f[0] = '\0';                 // ESC clears the focused field
                } else if (key >= ' ' && key < 127) {
                    // Username field has no spaces or ':' (the PASSWD delimiter).
                    if (!(login_ctx.cu_field == 0 && (key == ' ' || key == ':'))) {
                        size_t l = strlen(f);
                        if (l < fmax - 1) { f[l] = (char)key; f[l + 1] = '\0'; }
                    }
                }
            }
        }

        // Handle mouse input
        mouse_state_t ms;
        mouse_get_state(&ms);

        login_handle_mouse_move(ms.x, ms.y);

        if (mouse_button_clicked(MOUSE_LEFT_BTN)) {
            login_handle_mouse_click(ms.x, ms.y);

            // If state changed to SUCCESS via button click, attempt login.
            // #745: the typed view submits through the typed attempt, which is a
            // different function with a different (non-disclosing) failure
            // message - the arrow must do EXACTLY what Enter does, and Enter in
            // that view calls login_attempt_typed().
            if (login_ctx.state == LOGIN_STATE_SUCCESS) {
                int r = (login_ctx.tu_user[0] != '\0' || login_ctx.mode == LOGIN_MODE_TYPED)
                        ? login_attempt_typed(result) : login_attempt(result);
                if (r == 0) return 0;
            }

            // #568 first-boot: Create Account button was clicked.
            if (login_ctx.state == LOGIN_STATE_CREATE_ACCOUNT &&
                login_ctx.cu_submit_requested) {
                login_ctx.cu_submit_requested = false;
                if (login_create_submit(result) == 0) {
                    return 0;
                }
            }
        }

        // Cursor blink (every 500ms = 50 ticks at 100Hz)
        if (timer_ticks - login_ctx.cursor_blink_tick >= 50) {
            login_ctx.cursor_visible = !login_ctx.cursor_visible;
            login_ctx.cursor_blink_tick = timer_ticks;
        }

        // Draw
        login_draw();

        // Yield CPU
        proc_yield();
    }
}

int login_lock_screen(uint32_t uid) {
    login_init();

    // Find the user index for this UID
    int total = login_active(-1, NULL);
    for (int i = 0; i < total; i++) {
        user_entry_t *u = NULL;
        (void)login_active(i, &u);
        if (u && u->uid == uid) { login_ctx.selected_user = i; break; }
    }

    // Go directly to password state
    login_ctx.state = LOGIN_STATE_PASSWORD;
    login_ctx.password[0] = '\0';
    login_ctx.password_pos = 0;

    login_result_t result;
    return login_run(&result);
}
