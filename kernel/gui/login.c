// login.c - Login screen for MayteraOS
// Full-screen login with user selection and password entry

#include "login.h"
#include "themes.h"
#include "../types.h"
#include "../string.h"
#include "../serial.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../video/graphics.h"
#include "../drivers/mouse.h"
#include "../cpu/isr.h"
#include "../proc/users.h"
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

// External timer
extern volatile uint64_t timer_ticks;

// Forward declarations from font.h
extern void draw_string_small(int32_t x, int32_t y, const char *str, uint32_t color);
extern const uint8_t *font_get_glyph(char c);

// ============================================================================
// Login screen colors (dark theme, professional look)
// ============================================================================

#define LOGIN_BG_COLOR          0x001A2332  // Dark blue-grey
#define LOGIN_PANEL_BG          0x002C3E50  // Darker panel
#define LOGIN_PANEL_BORDER      0x004A6278  // Subtle border
#define LOGIN_TEXT_COLOR        0x00ECEFF1  // Off-white text
#define LOGIN_TEXT_DIM          0x0090A4AE  // Dimmed text
#define LOGIN_ACCENT            0x002196F3  // Blue accent
#define LOGIN_ACCENT_HOVER      0x001E88E5  // Darker blue hover
#define LOGIN_INPUT_BG          0x00263238  // Dark input field
#define LOGIN_INPUT_BORDER      0x00546E7A  // Input border
#define LOGIN_INPUT_FOCUS       0x002196F3  // Focused input border
#define LOGIN_ERROR_COLOR       0x00EF5350  // Red error text
#define LOGIN_AVATAR_BG         0x00455A64  // Avatar circle background
#define LOGIN_AVATAR_SELECTED   0x002196F3  // Selected avatar outline

// Layout constants
#define LOGIN_PANEL_WIDTH       400
#define LOGIN_PANEL_HEIGHT      360
#define LOGIN_AVATAR_SIZE       64
#define LOGIN_AVATAR_SPACING    20
#define LOGIN_INPUT_HEIGHT      32
#define LOGIN_INPUT_WIDTH       280
#define LOGIN_BUTTON_WIDTH      280
#define LOGIN_BUTTON_HEIGHT     36
#define LOGIN_MAX_PASSWORD      64

// ============================================================================
// Login state
// ============================================================================

typedef enum {
    LOGIN_STATE_SELECT_USER,
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
    bool hover_login_btn;       // Login button being hovered
    bool initialized;
} login_ctx;

// ============================================================================
// Drawing helpers
// ============================================================================

// Draw a character at standard font size (8x16) with color
static void draw_char_std(int32_t x, int32_t y, char c, uint32_t color) {
    const uint8_t *glyph = font_get_glyph(c);
    if (!glyph) return;

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                fb_put_pixel(x + col, y + row, color);
            }
        }
    }
}

// Draw a string at standard font size (8x16)
static void draw_text(int32_t x, int32_t y, const char *str, uint32_t color) {
    while (*str) {
        draw_char_std(x, y, *str, color);
        x += 8;
        str++;
    }
}

// Draw a larger character (2x scale, 16x32)
static void draw_char_large(int32_t x, int32_t y, char c, uint32_t color) {
    const uint8_t *glyph = font_get_glyph(c);
    if (!glyph) return;

    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                fb_put_pixel(x + col*2,     y + row*2,     color);
                fb_put_pixel(x + col*2 + 1, y + row*2,     color);
                fb_put_pixel(x + col*2,     y + row*2 + 1, color);
                fb_put_pixel(x + col*2 + 1, y + row*2 + 1, color);
            }
        }
    }
}

// Draw a string at 2x scale
static void draw_text_large(int32_t x, int32_t y, const char *str, uint32_t color) {
    while (*str) {
        draw_char_large(x, y, *str, color);
        x += 16;
        str++;
    }
}

// Get text width at standard size
static int text_width(const char *str) {
    return (int)strlen(str) * 8;
}

// Get text width at large size
static int text_width_large(const char *str) {
    return (int)strlen(str) * 16;
}

// Draw centered text at standard size
static void draw_text_centered(int32_t cx, int32_t y, const char *str, uint32_t color) {
    int w = text_width(str);
    draw_text(cx - w / 2, y, str, color);
}

// Draw centered text at large size
static void draw_text_large_centered(int32_t cx, int32_t y, const char *str, uint32_t color) {
    int w = text_width_large(str);
    draw_text_large(cx - w / 2, y, str, color);
}

// Draw a filled rounded rectangle (approximated with straight edges + corner pixels)
static void draw_panel(int32_t x, int32_t y, int32_t w, int32_t h,
                       uint32_t bg, uint32_t border) {
    // Fill background
    fb_fill_rect(x + 2, y, w - 4, h, bg);
    fb_fill_rect(x, y + 2, 2, h - 4, bg);
    fb_fill_rect(x + w - 2, y + 2, 2, h - 4, bg);
    // Corner pixels
    fb_put_pixel(x + 1, y + 1, bg);
    fb_put_pixel(x + w - 2, y + 1, bg);
    fb_put_pixel(x + 1, y + h - 2, bg);
    fb_put_pixel(x + w - 2, y + h - 2, bg);

    // Border
    fb_draw_rect(x + 2, y, w - 4, 1, border);
    fb_draw_rect(x + 2, y + h - 1, w - 4, 1, border);
    fb_draw_rect(x, y + 2, 1, h - 4, border);
    fb_draw_rect(x + w - 1, y + 2, 1, h - 4, border);
}

// Draw a user avatar (circle with initial)
static void draw_avatar(int32_t cx, int32_t cy, int radius, char initial,
                        uint32_t bg, uint32_t border, uint32_t text_color) {
    gfx_fill_circle(cx, cy, radius, bg);
    gfx_draw_circle(cx, cy, radius, border);
    gfx_draw_circle(cx, cy, radius - 1, border);

    // Draw initial letter centered in circle
    draw_char_large(cx - 8, cy - 16, initial, text_color);
}

// Draw a text input field
static void draw_input_field(int32_t x, int32_t y, int32_t w, int32_t h,
                             const char *text, int cursor_pos, bool focused,
                             bool is_password) {
    uint32_t bg = LOGIN_INPUT_BG;
    uint32_t border = focused ? LOGIN_INPUT_FOCUS : LOGIN_INPUT_BORDER;

    fb_fill_rect(x, y, w, h, bg);
    fb_draw_rect(x, y, w, h, border);
    if (focused) {
        fb_draw_rect(x + 1, y + 1, w - 2, h - 2, border);
    }

    // Draw text (or dots for password)
    int tx = x + 8;
    int ty = y + (h - 16) / 2;
    int len = strlen(text);

    for (int i = 0; i < len && tx < x + w - 16; i++) {
        if (is_password) {
            // Draw bullet character
            gfx_fill_circle(tx + 3, ty + 8, 3, LOGIN_TEXT_COLOR);
            tx += 8;
        } else {
            draw_char_std(tx, ty, text[i], LOGIN_TEXT_COLOR);
            tx += 8;
        }
    }

    // Draw cursor
    if (focused && login_ctx.cursor_visible) {
        int cursor_x = x + 8 + cursor_pos * 8;
        if (cursor_x < x + w - 8) {
            fb_fill_rect(cursor_x, ty, 2, 16, LOGIN_TEXT_COLOR);
        }
    }
}

// Draw a button
static void draw_button(int32_t x, int32_t y, int32_t w, int32_t h,
                        const char *label, bool hovered) {
    uint32_t bg = hovered ? LOGIN_ACCENT_HOVER : LOGIN_ACCENT;
    fb_fill_rect(x, y, w, h, bg);

    // Center text
    int tw = text_width(label);
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 16) / 2;
    draw_text(tx, ty, label, 0x00FFFFFF);
}

// ============================================================================
// Login screen rendering
// ============================================================================

static void login_draw(void) {
    uint32_t sw = login_ctx.screen_w;
    uint32_t sh = login_ctx.screen_h;

    // Draw gradient background
    gfx_gradient_rect(0, 0, sw, sh, LOGIN_BG_COLOR, 0x000D1B2A, 0);

    // Center panel position
    int px = (sw - LOGIN_PANEL_WIDTH) / 2;
    int py = (sh - LOGIN_PANEL_HEIGHT) / 2 - 20;

    // Draw title "MayteraOS" above panel
    draw_text_large_centered(sw / 2, py - 60, "MayteraOS", LOGIN_TEXT_COLOR);
    draw_text_centered(sw / 2, py - 20, "Welcome. Please sign in.", LOGIN_TEXT_DIM);

    // Draw main panel
    draw_panel(px, py, LOGIN_PANEL_WIDTH, LOGIN_PANEL_HEIGHT,
               LOGIN_PANEL_BG, LOGIN_PANEL_BORDER);

    // Get user table
    int user_count = 0;
    user_entry_t *users = users_get_table(&user_count);

    if (login_ctx.state == LOGIN_STATE_SELECT_USER) {
        // Show user avatars
        draw_text_centered(sw / 2, py + 20, "Select User", LOGIN_TEXT_COLOR);

        // #307: if we fell through to this stub, show WHY autologin did not fire
        // (FAT not mounted / LOGIN.CFG read failed with size / user not found),
        // so a real-hardware failure is self-explaining on the display itself.
        if (g_autologin_debug[0]) {
            draw_text_centered(sw / 2, py + 40, g_autologin_debug, LOGIN_ERROR_COLOR);
        }

        int total_width = 0;
        int display_count = 0;
        for (int i = 0; i < user_count && display_count < 5; i++) {
            if (!users[i].active) continue;
            if (display_count > 0) total_width += LOGIN_AVATAR_SPACING;
            total_width += LOGIN_AVATAR_SIZE;
            display_count++;
        }

        int ax = (sw - total_width) / 2 + LOGIN_AVATAR_SIZE / 2;
        int ay = py + 100;

        int idx = 0;
        for (int i = 0; i < user_count && idx < 5; i++) {
            if (!users[i].active) continue;

            char initial = users[i].username[0];
            if (initial >= 'a' && initial <= 'z') initial -= 32;

            uint32_t avatar_bg = LOGIN_AVATAR_BG;
            uint32_t avatar_border = LOGIN_PANEL_BORDER;

            if (idx == login_ctx.hover_user) {
                avatar_border = LOGIN_ACCENT;
            }

            draw_avatar(ax, ay, LOGIN_AVATAR_SIZE / 2, initial,
                       avatar_bg, avatar_border, LOGIN_TEXT_COLOR);

            // Username below avatar
            draw_text_centered(ax, ay + LOGIN_AVATAR_SIZE / 2 + 8,
                             users[i].display_name[0] ? users[i].display_name : users[i].username,
                             LOGIN_TEXT_COLOR);

            ax += LOGIN_AVATAR_SIZE + LOGIN_AVATAR_SPACING;
            idx++;
        }

    } else if (login_ctx.state == LOGIN_STATE_PASSWORD) {
        // Show selected user and password field
        user_entry_t *sel_user = NULL;
        int idx = 0;
        for (int i = 0; i < user_count; i++) {
            if (!users[i].active) continue;
            if (idx == login_ctx.selected_user) {
                sel_user = &users[i];
                break;
            }
            idx++;
        }

        if (sel_user) {
            // Draw selected user's avatar (larger)
            char initial = sel_user->username[0];
            if (initial >= 'a' && initial <= 'z') initial -= 32;
            draw_avatar(sw / 2, py + 80, 36, initial,
                       LOGIN_ACCENT, LOGIN_ACCENT, 0x00FFFFFF);

            // Username
            draw_text_centered(sw / 2, py + 125,
                             sel_user->display_name[0] ? sel_user->display_name : sel_user->username,
                             LOGIN_TEXT_COLOR);

            // Password label
            draw_text(px + 60, py + 160, "Password:", LOGIN_TEXT_DIM);

            // Password input field
            int input_x = px + (LOGIN_PANEL_WIDTH - LOGIN_INPUT_WIDTH) / 2;
            int input_y = py + 180;
            draw_input_field(input_x, input_y, LOGIN_INPUT_WIDTH, LOGIN_INPUT_HEIGHT,
                           login_ctx.password, login_ctx.password_pos, true, true);

            // Login button
            int btn_x = px + (LOGIN_PANEL_WIDTH - LOGIN_BUTTON_WIDTH) / 2;
            int btn_y = py + 230;
            draw_button(btn_x, btn_y, LOGIN_BUTTON_WIDTH, LOGIN_BUTTON_HEIGHT,
                       "Sign In", login_ctx.hover_login_btn);

            // "Back" link
            draw_text_centered(sw / 2, py + 290, "< Back to user list", LOGIN_TEXT_DIM);

            // Error message
            if (login_ctx.error_msg[0]) {
                draw_text_centered(sw / 2, py + 320, login_ctx.error_msg, LOGIN_ERROR_COLOR);
            }
        }
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

static void login_handle_mouse_click(int32_t mx, int32_t my) {
    uint32_t sw = login_ctx.screen_w;
    uint32_t sh = login_ctx.screen_h;
    int px = (sw - LOGIN_PANEL_WIDTH) / 2;
    int py = (sh - LOGIN_PANEL_HEIGHT) / 2 - 20;

    int user_count = 0;
    user_entry_t *users = users_get_table(&user_count);

    if (login_ctx.state == LOGIN_STATE_SELECT_USER) {
        // Check avatar clicks
        int total_width = 0;
        int display_count = 0;
        for (int i = 0; i < user_count; i++) {
            if (!users[i].active) continue;
            if (display_count > 0) total_width += LOGIN_AVATAR_SPACING;
            total_width += LOGIN_AVATAR_SIZE;
            display_count++;
        }

        int ax = (sw - total_width) / 2;
        int ay = py + 100 - LOGIN_AVATAR_SIZE / 2;

        int idx = 0;
        for (int i = 0; i < user_count && idx < 5; i++) {
            if (!users[i].active) continue;

            if (mx >= ax && mx < ax + LOGIN_AVATAR_SIZE &&
                my >= ay && my < ay + LOGIN_AVATAR_SIZE + 30) {
                // User selected
                login_ctx.selected_user = idx;
                login_ctx.state = LOGIN_STATE_PASSWORD;
                login_ctx.password[0] = '\0';
                login_ctx.password_pos = 0;
                login_ctx.error_msg[0] = '\0';
                return;
            }

            ax += LOGIN_AVATAR_SIZE + LOGIN_AVATAR_SPACING;
            idx++;
        }

    } else if (login_ctx.state == LOGIN_STATE_PASSWORD) {
        // Check login button
        int btn_x = px + (LOGIN_PANEL_WIDTH - LOGIN_BUTTON_WIDTH) / 2;
        int btn_y = py + 230;
        if (mx >= btn_x && mx < btn_x + LOGIN_BUTTON_WIDTH &&
            my >= btn_y && my < btn_y + LOGIN_BUTTON_HEIGHT) {
            // Attempt login (handled via enter key path)
            // Simulate enter key
            login_ctx.state = LOGIN_STATE_SUCCESS;  // Will be checked in main loop
            return;
        }

        // Check "Back" link
        int back_y = py + 290;
        if (my >= back_y && my < back_y + 16 &&
            mx >= (int)(sw/2 - 80) && mx < (int)(sw/2 + 80)) {
            login_ctx.state = LOGIN_STATE_SELECT_USER;
            login_ctx.password[0] = '\0';
            login_ctx.password_pos = 0;
            login_ctx.error_msg[0] = '\0';
        }
    }
}

static void login_handle_mouse_move(int32_t mx, int32_t my) {
    uint32_t sw = login_ctx.screen_w;
    uint32_t sh = login_ctx.screen_h;
    int px = (sw - LOGIN_PANEL_WIDTH) / 2;
    int py = (sh - LOGIN_PANEL_HEIGHT) / 2 - 20;

    int user_count = 0;
    user_entry_t *users = users_get_table(&user_count);

    login_ctx.hover_user = -1;
    login_ctx.hover_login_btn = false;

    if (login_ctx.state == LOGIN_STATE_SELECT_USER) {
        // Check avatar hover
        int total_width = 0;
        int display_count = 0;
        for (int i = 0; i < user_count; i++) {
            if (!users[i].active) continue;
            if (display_count > 0) total_width += LOGIN_AVATAR_SPACING;
            total_width += LOGIN_AVATAR_SIZE;
            display_count++;
        }

        int ax = (sw - total_width) / 2;
        int ay = py + 100 - LOGIN_AVATAR_SIZE / 2;

        int idx = 0;
        for (int i = 0; i < user_count && idx < 5; i++) {
            if (!users[i].active) continue;

            if (mx >= ax && mx < ax + LOGIN_AVATAR_SIZE &&
                my >= ay && my < ay + LOGIN_AVATAR_SIZE + 30) {
                login_ctx.hover_user = idx;
                break;
            }

            ax += LOGIN_AVATAR_SIZE + LOGIN_AVATAR_SPACING;
            idx++;
        }

    } else if (login_ctx.state == LOGIN_STATE_PASSWORD) {
        // Check login button hover
        int btn_x = px + (LOGIN_PANEL_WIDTH - LOGIN_BUTTON_WIDTH) / 2;
        int btn_y = py + 230;
        if (mx >= btn_x && mx < btn_x + LOGIN_BUTTON_WIDTH &&
            my >= btn_y && my < btn_y + LOGIN_BUTTON_HEIGHT) {
            login_ctx.hover_login_btn = true;
        }
    }
}

// Try to authenticate the selected user
static int login_attempt(login_result_t *result) {
    int user_count = 0;
    user_entry_t *users = users_get_table(&user_count);

    // Find the selected user
    user_entry_t *sel_user = NULL;
    int idx = 0;
    for (int i = 0; i < user_count; i++) {
        if (!users[i].active) continue;
        if (idx == login_ctx.selected_user) {
            sel_user = &users[i];
            break;
        }
        idx++;
    }

    if (!sel_user) {
        strncpy(login_ctx.error_msg, "Invalid user selection", sizeof(login_ctx.error_msg) - 1);
        login_ctx.state = LOGIN_STATE_PASSWORD;
        return -1;
    }

    // Verify password
    if (user_verify_password(sel_user->username, login_ctx.password) != 0) {
        strncpy(login_ctx.error_msg, "Incorrect password. Try again.", sizeof(login_ctx.error_msg) - 1);
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

// ============================================================================
// Public API
// ============================================================================

void login_init(void) {
    memset(&login_ctx, 0, sizeof(login_ctx));
    login_ctx.state = LOGIN_STATE_SELECT_USER;
    login_ctx.selected_user = -1;
    login_ctx.hover_user = -1;
    login_ctx.screen_w = fb_get_width();
    login_ctx.screen_h = fb_get_height();
    login_ctx.cursor_visible = true;
    login_ctx.cursor_blink_tick = timer_ticks;
    login_ctx.initialized = true;

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

    // #307: LOGIN.CFG read OK but no autologin= line found.
    strncpy(g_autologin_debug,
            "autologin: no autologin= line in LOGIN.CFG", sizeof(g_autologin_debug) - 1);
    bootlog_write("[LOGIN] autologin: LOGIN.CFG read OK but no 'autologin=' key present");
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

    // Set mouse bounds
    mouse_set_bounds(0, 0, login_ctx.screen_w - 1, login_ctx.screen_h - 1);

    kprintf("[LOGIN] Login screen active\n");

    // Main login loop
    while (1) {
        // Handle keyboard input
        if (keyboard_has_char()) {
            int key = keyboard_get_char();

            if (login_ctx.state == LOGIN_STATE_SELECT_USER) {
                // Number keys 1-9 to select users directly
                if (key >= '1' && key <= '9') {
                    int idx = key - '1';
                    int user_count = 0;
                    user_entry_t *users = users_get_table(&user_count);
                    int active_count = 0;
                    for (int i = 0; i < user_count; i++) {
                        if (users[i].active) active_count++;
                    }
                    if (idx < active_count) {
                        login_ctx.selected_user = idx;
                        login_ctx.state = LOGIN_STATE_PASSWORD;
                        login_ctx.password[0] = '\0';
                        login_ctx.password_pos = 0;
                        login_ctx.error_msg[0] = '\0';
                    }
                }
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
                    // ESC - go back to user selection
                    login_ctx.state = LOGIN_STATE_SELECT_USER;
                    login_ctx.password[0] = '\0';
                    login_ctx.password_pos = 0;
                    login_ctx.error_msg[0] = '\0';
                } else if (key >= ' ' && key < 127) {
                    // Printable character
                    if (login_ctx.password_pos < LOGIN_MAX_PASSWORD - 1) {
                        login_ctx.password[login_ctx.password_pos++] = (char)key;
                        login_ctx.password[login_ctx.password_pos] = '\0';
                    }
                }
            } else if (login_ctx.state == LOGIN_STATE_SUCCESS) {
                // Check authentication
                if (login_attempt(result) == 0) {
                    return 0;
                }
            }
        }

        // Handle mouse input
        mouse_state_t ms;
        mouse_get_state(&ms);

        login_handle_mouse_move(ms.x, ms.y);

        if (mouse_button_clicked(MOUSE_LEFT_BTN)) {
            login_handle_mouse_click(ms.x, ms.y);

            // If state changed to SUCCESS via button click, attempt login
            if (login_ctx.state == LOGIN_STATE_SUCCESS) {
                if (login_attempt(result) == 0) {
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
    int user_count = 0;
    user_entry_t *users = users_get_table(&user_count);
    int idx = 0;
    for (int i = 0; i < user_count; i++) {
        if (!users[i].active) continue;
        if (users[i].uid == uid) {
            login_ctx.selected_user = idx;
            break;
        }
        idx++;
    }

    // Go directly to password state
    login_ctx.state = LOGIN_STATE_PASSWORD;
    login_ctx.password[0] = '\0';
    login_ctx.password_pos = 0;

    login_result_t result;
    return login_run(&result);
}
