// login.c - Login screen for the MayteraOS userland compositor
// Handles user selection, password entry, auto-login, and authentication.
//
// #567 visual polish pass (docs/mockups/login-mockup.html "Login screen" tab,
// approved with the user's edits already applied there: no wordmark, no
// instructional text). Brings this screen up to the same aesthetic as
// lockscreen.c (#566): a softened/darkened wallpaper backdrop (reusing
// a full-resolution darkened wallpaper, no mosaic), a
// centered frosted panel, a big live clock, and the same avatar/field/button
// styling. Pure layout/visual change: do_authenticate()/login_handle_key()'s
// auth logic and login_init()'s autologin-config read are untouched.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"

// ============================================================================
// Static state
// ============================================================================

static login_state_t g_login_state;
static user_info_t   g_users[LOGIN_MAX_USERS];
static int           g_user_count;
static int           g_selected_user;
static char          g_password[LOGIN_MAX_PASSWORD];
static int           g_password_len;
static int           g_cursor_blink;   // 1 = cursor visible, 0 = hidden
static int           g_blink_counter;  // frames since last toggle
static char          g_error_msg[64];

// ============================================================================
// Internal helpers
// ============================================================================

// Return the screen width stored in g_fb_width.
static inline int sw(void) { return (int)g_fb_width; }

// Return the screen height stored in g_fb_height.
static inline int sh(void) { return (int)g_fb_height; }

// Compute the X origin of the centered login panel.
static inline int panel_x(void) { return (sw() - LOGIN_PANEL_W) / 2; }

// Compute the Y origin of the centered login panel. #567: +30 leaves room
// for the live clock drawn above it, the same offset lockscreen.c's own
// lock_geom() uses, so the two panels sit at the same visual height.
static inline int panel_y(void) { return (sh() - LOGIN_PANEL_H) / 2 + 30; }

// Copy at most (dst_len - 1) characters from src into dst and NUL-terminate.
// Returns number of characters written (excluding the NUL).
static int safe_copy(char *dst, const char *src, int dst_len)
{
    int i = 0;
    while (i < dst_len - 1 && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return i;
}

// Locate the first occurrence of needle in haystack (both NUL-terminated).
// Returns a pointer to the match, or NULL if not found.
static const char *str_find(const char *haystack, const char *needle)
{
    unsigned long nlen = strlen(needle);
    unsigned long hlen = strlen(haystack);
    if (nlen == 0) return haystack;
    if (nlen > hlen) return NULL;
    for (unsigned long i = 0; i <= hlen - nlen; i++) {
        if (strncmp(haystack + i, needle, nlen) == 0)
            return haystack + i;
    }
    return NULL;
}

// Extract a NUL-terminated token from src starting after prefix up to
// the first newline, carriage-return, or NUL. Writes into out (up to
// out_len bytes including NUL). Returns 1 on success, 0 if prefix is
// not present or output would be empty.
static int extract_after(const char *src, const char *prefix,
                         char *out, int out_len)
{
    const char *pos = str_find(src, prefix);
    if (!pos) return 0;
    pos += strlen(prefix);
    int i = 0;
    while (i < out_len - 1 && pos[i] != '\0' &&
           pos[i] != '\n' && pos[i] != '\r') {
        out[i] = pos[i];
        i++;
    }
    out[i] = '\0';
    return (i > 0) ? 1 : 0;
}

// Attempt to authenticate with username and password.
// On success: updates g_logged_in, g_login_uid, g_login_username,
// plays startup sound, and returns 1.
// On failure: sets g_error_msg and returns 0.
// #567: auth logic unchanged from the pre-polish version - still the same
// sys_authenticate() call, same bootlog lines, same failure state machine.
static int do_authenticate(const char *username, const char *password)
{
    char logmsg[96];
    int uid = sys_authenticate(username, password);
    if (uid >= 0) {
        g_logged_in     = true;
        g_login_uid     = uid;
        safe_copy(g_login_username, username, 64);
        sys_play_wav("/SOUNDS/STARTUP.WAV");
        g_login_state = LOGIN_STATE_SUCCESS;
        snprintf(logmsg, sizeof(logmsg), "compositor login: authenticate('%s') OK uid=%d", username, uid);
        sys_bootlog(logmsg);
        return 1;
    }
    // Authentication failed.
    safe_copy(g_error_msg, "Incorrect password. Please try again.", 64);
    g_login_state = LOGIN_STATE_ERROR;
    g_password_len = 0;
    g_password[0]  = '\0';
    snprintf(logmsg, sizeof(logmsg), "compositor login: authenticate('%s') FAILED", username);
    sys_bootlog(logmsg);
    return 0;
}

// ============================================================================
// Rendering helpers
// ============================================================================

// Draw a single bullet dot representing one masked password character.
// cx, cy is the center of the dot.
// #566: shared with lockscreen.c (prototyped in compositor.h) - not static.
void draw_bullet(int32_t cx, int32_t cy)
{
    draw_circle_filled(cx, cy, 4, CLR_LOGIN_TEXT);
}

// Draw the password input field box at position (x, y) with dimensions
// (LOGIN_INPUT_W x LOGIN_INPUT_H). Shows bullets for each character and a
// blinking cursor at the end. password_len/cursor_blink are passed in
// (rather than read from this file's own g_password_len/g_cursor_blink)
// so the lock screen (lockscreen.c, #566) can reuse this exact renderer
// with its own independent password-entry state instead of a second copy.
void draw_password_field(int32_t x, int32_t y, int password_len, int cursor_blink)
{
    // Background.
    draw_fill_rect(x, y, LOGIN_INPUT_W, LOGIN_INPUT_H, CLR_LOGIN_INPUT_BG);
    // Border: accent color when active.
    draw_rect_outline(x, y, LOGIN_INPUT_W, LOGIN_INPUT_H, CLR_LOGIN_ACCENT);

    // Draw one bullet per typed character.
    int32_t bx = x + 10;
    int32_t by = y + LOGIN_INPUT_H / 2;
    for (int i = 0; i < password_len; i++) {
        draw_bullet(bx + i * 12, by);
    }

    // Blinking cursor: a vertical bar after the last bullet.
    if (cursor_blink) {
        int32_t cx = bx + password_len * 12;
        draw_fill_rect(cx, y + 5, 2, LOGIN_INPUT_H - 10, CLR_LOGIN_TEXT);
    }
}

// Draw a button rectangle with centered text.
// #566: shared with lockscreen.c (prototyped in compositor.h) - not static.
void draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                 const char *label, uint32_t bg)
{
    draw_rounded_rect(x, y, w, h, 4, bg);
    draw_rect_outline(x, y, w, h, CLR_LOGIN_BORDER);
    int lw = text_width(label);
    draw_text(x + (w - lw) / 2, y + (h - FONT_CHAR_H) / 2, label, CLR_LOGIN_TEXT);
}

// #745 identity dot palette: reused VERBATIM from Settings'
// avatar_palette (userland/apps/settings/main.c users_refresh()), same 8
// hexes, same uid%8 indexing (section 8.2 of the design doc) - kept as an
// independent literal copy rather than a shared header because the two
// apps are separate binaries on separate toolkits (draw.c vs the gui_*
// handle-based one) with no common color-token module between them; a
// mismatch here would only ever be cosmetic (which of 8 hues), never a
// correctness bug, and grep is the check that keeps them in sync.
static const uint32_t g_avatar_id_palette[8] = {
    0xFF569CD6, 0xFF66BB66, 0xFFCC8844, 0xFFAA66CC,
    0xFFCC6666, 0xFF44AAAA, 0xFF888888, 0xFFBBAA44
};

// Draw the user avatar: an antialiased "glass" disc (#745 port of
// docs/LOGIN_AVATARS_AND_PROFILE.html) with the user's initial inside.
//
// GLASS, HONESTLY: the real glass_render() backdrop blur cannot reach a 64px
// circle (no circular mask primitive, no spare cache surface, and its own
// bleed would wash a disc this small to near-flat anyway - design doc
// section 4) and in any case this panel is a flat opaque fill, not glass, so
// there is no backdrop behind the avatar to blur. This draws a translucent
// tint fill plus a soft highlight patch (upper-left) and shade patch
// (lower-right), the two patches sized/offset so they stay fully inside the
// fill disc - no circular clip needed. It reads as a lit, translucent
// material; it does not claim to show blurred content, because there is none
// to show.
//
// CONTRAST BY CONSTRUCTION: the fill is now the SAME token in every state
// (CLR_LOGIN_AVATAR_FILL) - the old code swapped to a blue fill on
// "selected", which put the fixed-color initial at 2.71:1, under the 4.5:1
// text floor (design doc section 3). Selection is communicated by the ring
// alone, which is why `state` only ever changes the ring here.
//
// #566: shared with lockscreen.c (prototyped in compositor.h) - not static.
// (lockscreen.c does not currently call it - #745 removed its own avatar
// disc as a considered "username only" decision - but the prototype stays
// shared in case that decision is revisited.)
void draw_avatar(int32_t cx, int32_t cy, const char *username, unsigned int uid, int state)
{
    float r = (float)(LOGIN_AVATAR_SIZE / 2);

    // Base glass fill, uniform across all states.
    draw_circle_filled_aa(cx, cy, r, CLR_LOGIN_AVATAR_FILL, 255);
    // Soft highlight patch, upper-left (specular glint).
    draw_circle_filled_aa(cx - (int32_t)(r * 0.30f), cy - (int32_t)(r * 0.32f),
                          r * 0.50f, CLR_LOGIN_AVATAR_HI, 90);
    // Soft shade patch, lower-right.
    draw_circle_filled_aa(cx + (int32_t)(r * 0.30f), cy + (int32_t)(r * 0.32f),
                          r * 0.45f, CLR_LOGIN_AVATAR_SHADE, 80);

    // Ring: the only state-dependent boundary.
    uint32_t ring_color;
    float stroke;
    if (state == AVATAR_ST_SELECTED) {
        ring_color = CLR_LOGIN_AVATAR_RING_SEL;
        stroke = 3.0f;
        // 1px soft outer glow (design doc 7.1).
        draw_circle_ring_aa(cx, cy, r + 4.5f, 2.0f, CLR_LOGIN_AVATAR_RING_SEL, 45);
    } else if (state == AVATAR_ST_HOVER) {
        ring_color = CLR_LOGIN_AVATAR_RING_HOVER;
        stroke = 2.0f;
    } else {
        ring_color = CLR_LOGIN_AVATAR_RING;
        stroke = 2.0f;
    }
    draw_circle_ring_aa(cx, cy, r + stroke * 0.5f, stroke, ring_color, 255);

    // Identity dot (decorative only - design doc section 7.3, exempt from the
    // 3:1 boundary floor: removing it changes nothing about which account is
    // which). 12px, 2px panel-color border, bottom-right.
    int32_t dot_r = 6;
    int32_t dot_cx = cx + (int32_t)(r * 0.74f);
    int32_t dot_cy = cy + (int32_t)(r * 0.74f);
    draw_circle_filled_aa(dot_cx, dot_cy, (float)(dot_r + 2), CLR_LOGIN_PANEL, 255);
    draw_circle_filled_aa(dot_cx, dot_cy, (float)dot_r, g_avatar_id_palette[uid % 8], 255);

    // Draw the first letter of the username, scaled up, centered. One ink,
    // every state (5.58:1 against the fill - design doc 7.2) - no longer
    // needs to change with the fill because the fill no longer changes.
    char letter[2] = { username[0], '\0' };
    if (letter[0] >= 'a' && letter[0] <= 'z') {
        letter[0] -= 32; // uppercase
    }
    int lw = text_width_large(letter, 2);
    draw_text_large(cx - lw / 2, cy - FONT_CHAR_H, letter, CLR_LOGIN_TEXT, 2);
}

// ============================================================================
// Layout (#567): computed fresh each call from the same inputs so render and
// hit-testing cannot drift apart, the same idiom lockscreen.c's lock_geom()
// uses - replaces the old "must match login_render" duplicated-arithmetic
// comments with a single source of the numbers.
// ============================================================================

typedef struct {
    int content_y;    // y of the top of the avatar row
    int avatar_cell;  // avatar width + spacing
    int row_w;        // total row width (0 users -> 0)
    int row_x;        // x of the first avatar
} login_select_geom_t;

static void login_select_geom(login_select_geom_t *g)
{
    int px = panel_x();
    int py = panel_y();
    g->content_y   = py + 34;   // top padding inside the panel (no title/subtitle any more)
    g->avatar_cell = LOGIN_AVATAR_SIZE + LOGIN_AVATAR_SPACE;
    g->row_w       = (g_user_count > 0) ? (g_user_count * g->avatar_cell - LOGIN_AVATAR_SPACE) : 0;
    g->row_x       = px + (LOGIN_PANEL_W - g->row_w) / 2;
}

typedef struct {
    int avatar_cy;
    int field_y;
    int err_y;
    int btn_y;
    int back_y;
} login_pw_geom_t;

static void login_pw_geom(login_pw_geom_t *g)
{
    int py = panel_y();

    int content_y = py + 26;
    g->avatar_cy = content_y + LOGIN_AVATAR_SIZE / 2;
    content_y += LOGIN_AVATAR_SIZE + 8 + FONT_CHAR_H + 16;

    g->field_y = content_y;
    content_y += LOGIN_INPUT_H + 12;

    g->err_y = content_y;
    if (g_login_state == LOGIN_STATE_ERROR && g_error_msg[0] != '\0') content_y += FONT_CHAR_H + 8;

    g->btn_y = content_y;
    content_y += LOGIN_BUTTON_H + 18;

    g->back_y = content_y;
}

// ============================================================================
// Public API
// ============================================================================

void login_init(void)
{
    g_login_state   = LOGIN_STATE_SELECT_USER;
    g_selected_user = 0;
    g_password_len  = 0;
    g_password[0]   = '\0';
    g_cursor_blink  = 1;
    g_blink_counter = 0;
    g_error_msg[0]  = '\0';

    // Fetch system user list.
    g_user_count = sys_list_users(g_users, LOGIN_MAX_USERS);
    if (g_user_count < 0) g_user_count = 0;

    // #307 real-hardware bring-up: this is the exact call whose result the
    // physical iMac14,4 "No user accounts found" bug is about. Log it (and
    // the compositor reaching the login screen at all) to the persistent
    // boot log so a failed real-hardware boot can be diagnosed offline.
    char logmsg[96];
    snprintf(logmsg, sizeof(logmsg),
             "compositor login_init: sys_list_users returned %d", g_user_count);
    sys_bootlog(logmsg);
    if (g_user_count == 0) {
        sys_bootlog("compositor login: 0 users - will show 'No user accounts found'");
    }
}

// ----------------------------------------------------------------------------

int login_run(void)
{
    // Attempt auto-login by reading /CONFIG/LOGIN.CFG.
    int cfg_fd = sys_open("/CONFIG/LOGIN.CFG", 0);
    if (cfg_fd >= 0) {
        char buf[256];
        long n = sys_read(cfg_fd, buf, sizeof(buf) - 1);
        sys_close(cfg_fd);
        if (n > 0) {
            buf[n] = '\0';
            char autologin_user[64];
            if (extract_after(buf, "autologin=", autologin_user, 64)) {
                int uid = sys_authenticate(autologin_user, "");
                if (uid >= 0) {
                    g_logged_in = true;
                    g_login_uid = uid;
                    safe_copy(g_login_username, autologin_user, 64);
                    sys_play_wav("/SOUNDS/STARTUP.WAV");
                    g_login_state = LOGIN_STATE_SUCCESS;
                    return 0;
                }
            }
        }
    }

    // Interactive login loop.
    while (g_login_state != LOGIN_STATE_SUCCESS) {
        // Update cursor blink every 30 frames.
        g_blink_counter++;
        if (g_blink_counter >= 30) {
            g_blink_counter = 0;
            g_cursor_blink  = g_cursor_blink ? 0 : 1;
        }

        // Read and dispatch keyboard input.
        int key = sys_get_keyboard();
        if (key >= 0) {
            login_handle_key(key);
        }

        // Read and dispatch mouse input.
        {
            static unsigned int prev_btn = 0;
            int abs_x = 0, abs_y = 0;
            unsigned int mbtn = 0;
            get_mouse(&abs_x, &abs_y, &mbtn);

            g_mouse_x = abs_x;
            g_mouse_y = abs_y;

            // Detect left-button press edge (not held).
            bool clicked = ((mbtn & 1) && !(prev_btn & 1));
            prev_btn = mbtn;
            if (clicked) {
                login_handle_mouse(g_mouse_x, g_mouse_y, true);
            }
        }

        // After an error frame, stay in ERROR state for one render pass so
        // the user can see the message, then return to PASSWORD.
        if (g_login_state == LOGIN_STATE_ERROR) {
            login_render();
            cursor_render();
            fb_flip();
            sys_sleep(1200);
            g_login_state = LOGIN_STATE_PASSWORD;
            g_error_msg[0] = '\0';
            continue;
        }

        login_render();
        cursor_render();
        fb_flip();
        sys_sleep(16);
    }

    return 0;
}

// ----------------------------------------------------------------------------

void login_render(void)
{
    int s_w = sw();
    int s_h = sh();

    // #569 backdrop: the real wallpaper at FULL RESOLUTION, DARKENED ONLY. The
    // block-mosaic "frosted glass" pass this used to run is deleted - it is
    // what made the wallpaper look pixelated on real hardware.
    wallpaper_render_background();
    g_draw_blend = 145;
    draw_fill_rect(0, 0, s_w, s_h, 0xFF0B0F18);
    g_draw_blend = 255;

    // Big live clock + date: same formatting helpers and position as the
    // lock screen (clock.c's lock_clock_hms()/lock_clock_date()), also the
    // liveness indicator across two screenshots (see blame.md).
    {
        char tbuf[16], dbuf[24];
        lock_clock_hms(tbuf, 1);
        lock_clock_date(dbuf);
        int tw = text_width_ttf(tbuf, 64);
        int cy = s_h * 14 / 100;
        draw_text_ttf((s_w - tw) / 2, cy, tbuf, 64, CLR_LOGIN_TEXT);
        draw_text_centered(s_w / 2, cy + 74, dbuf, CLR_LOGIN_DIMMED);
    }

    int px = panel_x();
    int py = panel_y();

    // Frosted panel: same rounded-12 glassy treatment as the lock screen.
    // Per the approved mockup (docs/mockups/login-mockup.html, user's edits
    // already applied): no wordmark, no instructional text - just the
    // clock/date above, avatars/field/button inside, power controls below.
    draw_rounded_rect(px, py, LOGIN_PANEL_W, LOGIN_PANEL_H, 12, CLR_LOGIN_PANEL);
    draw_rect_outline(px, py, LOGIN_PANEL_W, LOGIN_PANEL_H, CLR_LOGIN_BORDER);

    if (g_login_state == LOGIN_STATE_SELECT_USER) {
        login_select_geom_t g;
        login_select_geom(&g);

        if (g_user_count == 0) {
            const char *no_user = "No user accounts found";
            int nw = text_width(no_user);
            draw_text(px + (LOGIN_PANEL_W - nw) / 2, g.content_y + 20,
                      no_user, CLR_LOGIN_DIMMED);
        } else {
            int32_t r = LOGIN_AVATAR_SIZE / 2;
            for (int i = 0; i < g_user_count; i++) {
                int32_t cx = g.row_x + i * g.avatar_cell + LOGIN_AVATAR_SIZE / 2;
                int32_t cy = g.content_y + LOGIN_AVATAR_SIZE / 2;
                // #745: genuine mouse hover (no click needed), not "index 0
                // always looks selected" - the old `i == g_selected_user`
                // check was true for avatar 0 by default (g_selected_user
                // inits to 0 and only ever moves on a click that leaves this
                // screen), so it permanently highlighted the first avatar
                // for no reason tied to the mouse. That was the same fill
                // swap the design doc's contrast section flags as a defect,
                // now replaced entirely: nothing is "selected" on this
                // screen, only hovered or not.
                bool hover = (g_mouse_x >= cx - r && g_mouse_x <= cx + r &&
                             g_mouse_y >= cy - r && g_mouse_y <= cy + r);
                draw_avatar(cx, cy, g_users[i].username, g_users[i].uid,
                           hover ? AVATAR_ST_HOVER : AVATAR_ST_NORMAL);

                // Username label below avatar.
                const char *uname = g_users[i].display_name[0] != '\0'
                                    ? g_users[i].display_name
                                    : g_users[i].username;
                int uw = text_width(uname);
                int lx = cx - uw / 2;
                int ly = cy + LOGIN_AVATAR_SIZE / 2 + 6;
                draw_text(lx, ly, uname, hover ? CLR_LOGIN_TEXT : CLR_LOGIN_DIMMED);
            }
        }
    } else if (g_login_state == LOGIN_STATE_PASSWORD ||
               g_login_state == LOGIN_STATE_ERROR) {

        login_pw_geom_t g;
        login_pw_geom(&g);
        int32_t cx = px + LOGIN_PANEL_W / 2;

        // Selected user avatar + name (mirrors the lock screen's user-chip).
        const char *uname = (g_selected_user >= 0 && g_selected_user < g_user_count)
                            ? (g_users[g_selected_user].display_name[0] != '\0'
                               ? g_users[g_selected_user].display_name
                               : g_users[g_selected_user].username)
                            : "Unknown";
        unsigned int uid = (g_selected_user >= 0 && g_selected_user < g_user_count)
                           ? g_users[g_selected_user].uid : 0;
        draw_avatar(cx, g.avatar_cy, uname, uid, AVATAR_ST_SELECTED);
        {
            int unw = text_width(uname);
            draw_text(cx - unw / 2, g.avatar_cy + LOGIN_AVATAR_SIZE / 2 + 10, uname, CLR_LOGIN_TEXT);
        }

        // Password field, centered in the panel.
        int32_t field_x = px + (LOGIN_PANEL_W - LOGIN_INPUT_W) / 2;
        draw_password_field(field_x, g.field_y, g_password_len, g_cursor_blink);

        // Error message (shown in ERROR state).
        if (g_login_state == LOGIN_STATE_ERROR && g_error_msg[0] != '\0') {
            int ew = text_width(g_error_msg);
            draw_text(cx - ew / 2, g.err_y, g_error_msg, CLR_LOGIN_ERROR);
        }

        // "Sign In" button.
        int32_t btn_x = px + (LOGIN_PANEL_W - LOGIN_BUTTON_W) / 2;
        draw_button(btn_x, g.btn_y, LOGIN_BUTTON_W, LOGIN_BUTTON_H,
                    "Sign In", CLR_LOGIN_ACCENT);

        // "Back" link: dimmed text, left-aligned in the panel - the same
        // position/style lockscreen.c uses for "Switch User", so the two
        // panels' bottom rows read as the same control.
        draw_text(px + 16, g.back_y, "Back", CLR_LOGIN_DIMMED);
    }

    // Power controls: available before any authentication, same geometry as
    // the lock screen's (lockscreen.c lock_render()) so the two screens
    // present the identical control in the identical place.
    {
        int bw = 100, bh = 34, gap = 12;
        int by = s_h - bh - 20;
        int bx2 = s_w - 20 - bw;
        int bx1 = bx2 - gap - bw;
        draw_button(bx1, by, bw, bh, "Restart",   CLR_MENU_ITEM_NORM);
        draw_button(bx2, by, bw, bh, "Shut Down", CLR_POWER_RED);
    }
}

// ----------------------------------------------------------------------------

void login_handle_key(int key)
{
    if (g_login_state == LOGIN_STATE_SELECT_USER) {
        // Keys '1' through '9' select a user by index.
        if (key >= '1' && key <= '9') {
            int idx = key - '1';
            if (idx < g_user_count) {
                g_selected_user = idx;
                g_password_len  = 0;
                g_password[0]   = '\0';
                g_error_msg[0]  = '\0';
                g_login_state   = LOGIN_STATE_PASSWORD;
            }
        }
        return;
    }

    if (g_login_state == LOGIN_STATE_PASSWORD ||
        g_login_state == LOGIN_STATE_ERROR) {

        if (key == '\r' || key == '\n') {
            // Submit password.
            if (g_selected_user >= 0 && g_selected_user < g_user_count) {
                do_authenticate(g_users[g_selected_user].username, g_password);
            }
        } else if (key == 8 || key == 127) {
            // Backspace: delete last character.
            if (g_password_len > 0) {
                g_password_len--;
                g_password[g_password_len] = '\0';
            }
        } else if (key == 27) {
            // Escape: back to user selection.
            g_login_state  = LOGIN_STATE_SELECT_USER;
            g_password_len = 0;
            g_password[0]  = '\0';
            g_error_msg[0] = '\0';
        } else if (key >= 32 && key <= 126) {
            // Printable character: append to password buffer.
            if (g_password_len < LOGIN_MAX_PASSWORD - 1) {
                g_password[g_password_len++] = (char)key;
                g_password[g_password_len]   = '\0';
            }
        }
    }
}

// ----------------------------------------------------------------------------

void login_handle_mouse(int32_t x, int32_t y, bool clicked)
{
    if (!clicked) return;

    int s_w = sw();
    int s_h = sh();

    // Power controls: available before authentication (mirrors lockscreen.c
    // lock_handle_mouse() exactly - same geometry as login_render() above).
    {
        int bw = 100, bh = 34, gap = 12;
        int by = s_h - bh - 20;
        int bx2 = s_w - 20 - bw;
        int bx1 = bx2 - gap - bw;
        if (y >= by && y <= by + bh) {
            if (x >= bx1 && x <= bx1 + bw) { reboot();   return; }
            if (x >= bx2 && x <= bx2 + bw) { poweroff(); return; }
        }
    }

    int px = panel_x();

    if (g_login_state == LOGIN_STATE_SELECT_USER) {
        if (g_user_count == 0) return;

        login_select_geom_t g;
        login_select_geom(&g);

        for (int i = 0; i < g_user_count; i++) {
            int32_t cx = g.row_x + i * g.avatar_cell + LOGIN_AVATAR_SIZE / 2;
            int32_t cy = g.content_y + LOGIN_AVATAR_SIZE / 2;
            int32_t r  = LOGIN_AVATAR_SIZE / 2;

            // Hit-test: bounding box of the avatar circle.
            if (x >= cx - r && x <= cx + r &&
                y >= cy - r && y <= cy + r) {
                g_selected_user = i;
                g_password_len  = 0;
                g_password[0]   = '\0';
                g_error_msg[0]  = '\0';
                g_login_state   = LOGIN_STATE_PASSWORD;
                return;
            }
        }
        return;
    }

    if (g_login_state == LOGIN_STATE_PASSWORD ||
        g_login_state == LOGIN_STATE_ERROR) {

        login_pw_geom_t g;
        login_pw_geom(&g);

        // "Sign In" button bounds.
        int32_t btn_x = px + (LOGIN_PANEL_W - LOGIN_BUTTON_W) / 2;
        if (x >= btn_x && x <= btn_x + LOGIN_BUTTON_W &&
            y >= g.btn_y && y <= g.btn_y + LOGIN_BUTTON_H) {
            if (g_selected_user >= 0 && g_selected_user < g_user_count) {
                do_authenticate(g_users[g_selected_user].username, g_password);
            }
            return;
        }

        // "Back" link bounds (matches the render position: left-aligned,
        // px+16, sized to the actual text width plus a little slack).
        int back_w = text_width("Back");
        if (x >= px + 16 && x <= px + 16 + back_w + 12 &&
            y >= g.back_y && y <= g.back_y + FONT_CHAR_H + 4) {
            g_login_state  = LOGIN_STATE_SELECT_USER;
            g_password_len = 0;
            g_password[0]  = '\0';
            g_error_msg[0] = '\0';
        }
    }
}
