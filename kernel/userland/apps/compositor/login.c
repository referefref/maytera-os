// login.c - Login screen for the MayteraOS userland compositor
// Handles user selection, password entry, auto-login, and authentication.

#include "compositor.h"
#include "../../libc/syscall.h"

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

// Compute the Y origin of the centered login panel.
static inline int panel_y(void) { return (sh() - LOGIN_PANEL_H) / 2; }

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

// Check whether the two strings are equal (NUL-terminated).
static int str_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
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
static int do_authenticate(const char *username, const char *password)
{
    int uid = sys_authenticate(username, password);
    if (uid >= 0) {
        g_logged_in     = true;
        g_login_uid     = uid;
        safe_copy(g_login_username, username, 64);
        sys_play_wav("/SOUNDS/STARTUP.WAV");
        g_login_state = LOGIN_STATE_SUCCESS;
        return 1;
    }
    // Authentication failed.
    safe_copy(g_error_msg, "Incorrect password. Please try again.", 64);
    g_login_state = LOGIN_STATE_ERROR;
    g_password_len = 0;
    g_password[0]  = '\0';
    return 0;
}

// ============================================================================
// Rendering helpers
// ============================================================================

// Draw a single bullet dot representing one masked password character.
// cx, cy is the center of the dot.
static void draw_bullet(int32_t cx, int32_t cy)
{
    draw_circle_filled(cx, cy, 4, CLR_LOGIN_TEXT);
}

// Draw the password input field box at position (x, y) with dimensions
// (LOGIN_INPUT_W x LOGIN_INPUT_H). Shows bullets for each character and a
// blinking cursor at the end.
static void draw_password_field(int32_t x, int32_t y)
{
    // Background.
    draw_fill_rect(x, y, LOGIN_INPUT_W, LOGIN_INPUT_H, CLR_LOGIN_INPUT_BG);
    // Border: accent color when active.
    draw_rect_outline(x, y, LOGIN_INPUT_W, LOGIN_INPUT_H, CLR_LOGIN_ACCENT);

    // Draw one bullet per typed character.
    int32_t bx = x + 10;
    int32_t by = y + LOGIN_INPUT_H / 2;
    for (int i = 0; i < g_password_len; i++) {
        draw_bullet(bx + i * 12, by);
    }

    // Blinking cursor: a vertical bar after the last bullet.
    if (g_cursor_blink) {
        int32_t cx = bx + g_password_len * 12;
        draw_fill_rect(cx, y + 5, 2, LOGIN_INPUT_H - 10, CLR_LOGIN_TEXT);
    }
}

// Draw a button rectangle with centered text.
static void draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                        const char *label, uint32_t bg)
{
    draw_rounded_rect(x, y, w, h, 4, bg);
    draw_rect_outline(x, y, w, h, CLR_LOGIN_BORDER);
    int lw = text_width(label);
    draw_text(x + (w - lw) / 2, y + (h - FONT_CHAR_H) / 2, label, CLR_LOGIN_TEXT);
}

// Draw the user avatar circle with the user's initial inside.
// highlight controls whether the avatar is drawn in the selected color.
static void draw_avatar(int32_t cx, int32_t cy, const char *username, bool highlight)
{
    uint32_t fill = highlight ? CLR_LOGIN_AVATAR_S : CLR_LOGIN_AVATAR;
    draw_circle_filled(cx, cy, LOGIN_AVATAR_SIZE / 2, fill);
    draw_circle_outline(cx, cy, LOGIN_AVATAR_SIZE / 2, CLR_LOGIN_BORDER);

    // Draw the first letter of the username, scaled up, centered.
    char letter[2] = { username[0], '\0' };
    if (letter[0] >= 'a' && letter[0] <= 'z') {
        letter[0] -= 32; // uppercase
    }
    int lw = text_width_large(letter, 2);
    draw_text_large(cx - lw / 2, cy - FONT_CHAR_H, letter, CLR_LOGIN_TEXT, 2);
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
            mouse_evt_t mevt;
            memset(&mevt, 0, sizeof(mevt));
            get_mouse_evt(&mevt);

            g_mouse_x += mevt.dx;
            g_mouse_y += mevt.dy;
            if (g_mouse_x < 0)            g_mouse_x = 0;
            if (g_mouse_y < 0)            g_mouse_y = 0;
            if (g_mouse_x >= g_fb_width)  g_mouse_x = g_fb_width  - 1;
            if (g_mouse_y >= g_fb_height) g_mouse_y = g_fb_height - 1;

            // Detect left-button press edge (not held).
            bool clicked = ((mevt.buttons & 1) && !(prev_btn & 1));
            prev_btn = mevt.buttons;
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

    // Full-screen dark gradient background.
    draw_gradient_v(0, 0, s_w, s_h, CLR_LOGIN_BG_TOP, CLR_LOGIN_BG_BOT);

    int px = panel_x();
    int py = panel_y();

    // Central panel.
    draw_rounded_rect(px, py, LOGIN_PANEL_W, LOGIN_PANEL_H, 8, CLR_LOGIN_PANEL);
    draw_rect_outline(px, py, LOGIN_PANEL_W, LOGIN_PANEL_H, CLR_LOGIN_BORDER);

    // Title: "MayteraOS" in scale-2 text, centered horizontally in the panel.
    const char *title = "MayteraOS";
    int title_w = text_width_large(title, 2);
    int title_x = px + (LOGIN_PANEL_W - title_w) / 2;
    int title_y = py + 18;
    draw_text_large(title_x, title_y, title, CLR_LOGIN_TEXT, 2);

    // Subtitle.
    const char *subtitle = "Sign in to continue";
    int sub_w = text_width(subtitle);
    int sub_x = px + (LOGIN_PANEL_W - sub_w) / 2;
    int sub_y = title_y + FONT_CHAR_H * 2 + 8;
    draw_text(sub_x, sub_y, subtitle, CLR_LOGIN_DIMMED);

    // Divider line below subtitle.
    int div_y = sub_y + FONT_CHAR_H + 10;
    draw_hline(px + 16, div_y, LOGIN_PANEL_W - 32, CLR_LOGIN_BORDER);

    int content_y = div_y + 14;

    if (g_login_state == LOGIN_STATE_SELECT_USER) {
        // Draw user avatars in a centered horizontal row.
        if (g_user_count == 0) {
            const char *no_user = "No user accounts found";
            int nw = text_width(no_user);
            draw_text(px + (LOGIN_PANEL_W - nw) / 2, content_y + 20,
                      no_user, CLR_LOGIN_DIMMED);
        } else {
            // Compute total row width.
            int avatar_cell = LOGIN_AVATAR_SIZE + LOGIN_AVATAR_SPACE;
            int row_w = g_user_count * avatar_cell - LOGIN_AVATAR_SPACE;
            int row_x = px + (LOGIN_PANEL_W - row_w) / 2;

            for (int i = 0; i < g_user_count; i++) {
                int32_t cx = row_x + i * avatar_cell + LOGIN_AVATAR_SIZE / 2;
                int32_t cy = content_y + LOGIN_AVATAR_SIZE / 2;
                bool hi = (i == g_selected_user);
                draw_avatar(cx, cy, g_users[i].username, hi);

                // Username label below avatar.
                const char *uname = g_users[i].display_name[0] != '\0'
                                    ? g_users[i].display_name
                                    : g_users[i].username;
                int uw = text_width(uname);
                int lx = cx - uw / 2;
                int ly = cy + LOGIN_AVATAR_SIZE / 2 + 6;
                draw_text(lx, ly, uname, hi ? CLR_LOGIN_TEXT : CLR_LOGIN_DIMMED);
            }

            // Hint text at the bottom of the panel.
            const char *hint = "Click an account or press 1-9";
            int hw = text_width(hint);
            draw_text(px + (LOGIN_PANEL_W - hw) / 2,
                      py + LOGIN_PANEL_H - FONT_CHAR_H - 12,
                      hint, CLR_LOGIN_DIMMED);
        }
    } else if (g_login_state == LOGIN_STATE_PASSWORD ||
               g_login_state == LOGIN_STATE_ERROR) {

        // Selected user name.
        const char *uname = (g_selected_user >= 0 && g_selected_user < g_user_count)
                            ? (g_users[g_selected_user].display_name[0] != '\0'
                               ? g_users[g_selected_user].display_name
                               : g_users[g_selected_user].username)
                            : "Unknown";
        int unw = text_width(uname);
        draw_text(px + (LOGIN_PANEL_W - unw) / 2, content_y, uname, CLR_LOGIN_TEXT);
        content_y += FONT_CHAR_H + 20;

        // Password field, centered in the panel.
        int32_t field_x = px + (LOGIN_PANEL_W - LOGIN_INPUT_W) / 2;
        int32_t field_y = content_y;
        draw_password_field(field_x, field_y);
        content_y += LOGIN_INPUT_H + 14;

        // Error message (shown in ERROR state).
        if (g_login_state == LOGIN_STATE_ERROR && g_error_msg[0] != '\0') {
            int ew = text_width(g_error_msg);
            draw_text(px + (LOGIN_PANEL_W - ew) / 2, content_y,
                      g_error_msg, CLR_LOGIN_ERROR);
            content_y += FONT_CHAR_H + 10;
        }

        // "Sign In" button.
        int32_t btn_x = px + (LOGIN_PANEL_W - LOGIN_BUTTON_W) / 2;
        draw_button(btn_x, content_y, LOGIN_BUTTON_W, LOGIN_BUTTON_H,
                    "Sign In", CLR_LOGIN_ACCENT);
        content_y += LOGIN_BUTTON_H + 10;

        // "Back" link text.
        const char *back_label = "Back";
        int bw = text_width(back_label);
        draw_text(px + (LOGIN_PANEL_W - bw) / 2, content_y,
                  back_label, CLR_LOGIN_DIMMED);
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

    int px = panel_x();
    int py = panel_y();

    if (g_login_state == LOGIN_STATE_SELECT_USER) {
        if (g_user_count == 0) return;

        // Reconstruct avatar row geometry (must match login_render).
        int div_y     = py + 18 + FONT_CHAR_H * 2 + 8 + FONT_CHAR_H + 10;
        int content_y = div_y + 14;

        int avatar_cell = LOGIN_AVATAR_SIZE + LOGIN_AVATAR_SPACE;
        int row_w = g_user_count * avatar_cell - LOGIN_AVATAR_SPACE;
        int row_x = px + (LOGIN_PANEL_W - row_w) / 2;

        for (int i = 0; i < g_user_count; i++) {
            int32_t cx = row_x + i * avatar_cell + LOGIN_AVATAR_SIZE / 2;
            int32_t cy = content_y + LOGIN_AVATAR_SIZE / 2;
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

        // Reconstruct content_y to locate buttons (must match login_render).
        int div_y     = py + 18 + FONT_CHAR_H * 2 + 8 + FONT_CHAR_H + 10;
        int content_y = div_y + 14;
        // Skip username line.
        content_y += FONT_CHAR_H + 20;
        // Skip password field.
        content_y += LOGIN_INPUT_H + 14;
        // Skip error line if visible.
        if (g_login_state == LOGIN_STATE_ERROR && g_error_msg[0] != '\0') {
            content_y += FONT_CHAR_H + 10;
        }

        // "Sign In" button bounds.
        int32_t btn_x = px + (LOGIN_PANEL_W - LOGIN_BUTTON_W) / 2;
        int32_t btn_y = content_y;
        if (x >= btn_x && x <= btn_x + LOGIN_BUTTON_W &&
            y >= btn_y && y <= btn_y + LOGIN_BUTTON_H) {
            if (g_selected_user >= 0 && g_selected_user < g_user_count) {
                do_authenticate(g_users[g_selected_user].username, g_password);
            }
            return;
        }
        content_y += LOGIN_BUTTON_H + 10;

        // "Back" link bounds (approximate: full-width strip, one text line).
        if (y >= content_y && y <= content_y + FONT_CHAR_H + 4) {
            g_login_state  = LOGIN_STATE_SELECT_USER;
            g_password_len = 0;
            g_password[0]  = '\0';
            g_error_msg[0] = '\0';
        }
    }
}
