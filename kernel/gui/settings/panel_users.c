// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// panel_users.c - Users Panel for MayteraOS Unified Settings
// Provides user account management, login options, and profile settings

#include "settings_panel.h"
#include "settings_widgets.h"
#include "../window.h"
#include "../desktop.h"
#include "../../types.h"
#include "../../serial.h"
#include "../../mm/heap.h"
#include "../../string.h"
#include "../../video/framebuffer.h"
#include "../../video/font.h"
#include "../themes.h"

// ============================================================================
// User Account Structures
// ============================================================================

#define MAX_USERS           8
#define MAX_USERNAME_LEN    32
#define MAX_PASSWORD_LEN    64
#define MAX_DISPLAY_NAME    64

// User account types
typedef enum {
    USER_TYPE_STANDARD = 0,
    USER_TYPE_ADMIN    = 1,
    USER_TYPE_GUEST    = 2
} user_type_t;

// User account structure
typedef struct {
    char username[MAX_USERNAME_LEN];
    char display_name[MAX_DISPLAY_NAME];
    char password_hash[MAX_PASSWORD_LEN];
    user_type_t type;
    uint32_t avatar_color;
    bool active;
    bool logged_in;
} user_account_t;

// Panel user data
typedef struct {
    user_account_t users[MAX_USERS];
    int user_count;
    int selected_user;
    int current_user_id;

    // Login options
    bool auto_login_enabled;
    int auto_login_user_id;
    bool show_user_list;
    bool guest_enabled;

    // UI state
    int scroll_offset;
    int sub_section;  // 0=Users, 1=Login Options
} users_panel_data_t;

// ============================================================================
// Layout Constants
// ============================================================================

#define USERS_LIST_WIDTH    160
#define USER_ITEM_HEIGHT    44
#define AVATAR_SIZE         32
#define BUTTON_HEIGHT       26
#define BUTTON_WIDTH        110

#define COLOR_USER_SELECTED     0x4A90C2
#define COLOR_USER_BG           THEME_WINDOW_BG
#define COLOR_ADMIN_BADGE       0xE74C3C
#define COLOR_STANDARD_BADGE    0x3498DB
#define COLOR_GUEST_BADGE       0x95A5A6

// ============================================================================
// Helper Functions
// ============================================================================

static void hash_password(const char *password, char *hash_out) {
    uint32_t h = 5381;
    while (*password) {
        h = ((h << 5) + h) + (uint8_t)*password;
        password++;
    }
    snprintf(hash_out, MAX_PASSWORD_LEN, "%08X", h);
}

static const char *get_user_type_name(user_type_t type) {
    switch (type) {
        case USER_TYPE_ADMIN:    return "Administrator";
        case USER_TYPE_GUEST:    return "Guest";
        default:                 return "Standard";
    }
}

static uint32_t get_user_type_color(user_type_t type) {
    switch (type) {
        case USER_TYPE_ADMIN:    return COLOR_ADMIN_BADGE;
        case USER_TYPE_GUEST:    return COLOR_GUEST_BADGE;
        default:                 return COLOR_STANDARD_BADGE;
    }
}

// Draw text helper
static void draw_text(int32_t x, int32_t y, const char *text, uint32_t color) {
    while (*text) {
        const uint8_t *glyph = font_get_glyph(*text);
        if (glyph) {
            for (int row = 0; row < FONT_HEIGHT; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < FONT_WIDTH; col++) {
                    if (bits & (0x80 >> col)) {
                        fb_put_pixel(x + col, y + row, color);
                    }
                }
            }
        }
        x += FONT_WIDTH;
        text++;
    }
}

// Draw avatar circle with initials
static void draw_avatar(int32_t x, int32_t y, int32_t size, uint32_t color, const char *initials) {
    int32_t radius = size / 2;
    int32_t cx = x + radius;
    int32_t cy = y + radius;

    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (dx * dx + dy * dy <= radius * radius) {
                fb_put_pixel(cx + dx, cy + dy, color);
            }
        }
    }

    if (initials && initials[0]) {
        int text_len = strlen(initials);
        if (text_len > 2) text_len = 2;
        int32_t text_x = cx - (text_len * FONT_WIDTH) / 2;
        int32_t text_y = cy - FONT_HEIGHT / 2;
        for (int i = 0; i < text_len && initials[i]; i++) {
            const uint8_t *glyph = font_get_glyph(initials[i]);
            if (glyph) {
                for (int row = 0; row < FONT_HEIGHT; row++) {
                    uint8_t bits = glyph[row];
                    for (int col = 0; col < FONT_WIDTH; col++) {
                        if (bits & (0x80 >> col)) {
                            fb_put_pixel(text_x + col, text_y + row, THEME_TEXTBOX_BG);
                        }
                    }
                }
            }
            text_x += FONT_WIDTH;
        }
    }
}

// ============================================================================
// Panel Lifecycle Callbacks
// ============================================================================

static void users_panel_init(settings_panel_t *panel) {
    kprintf("[Users Panel] Initializing...\n");

    users_panel_data_t *data = (users_panel_data_t *)kzalloc(sizeof(users_panel_data_t));
    if (!data) {
        kprintf("[Users Panel] Failed to allocate data\n");
        return;
    }

    // Create default admin user
    user_account_t *admin = &data->users[0];
    strcpy(admin->username, "admin");
    strcpy(admin->display_name, "Administrator");
    hash_password("admin", admin->password_hash);
    admin->type = USER_TYPE_ADMIN;
    admin->avatar_color = 0x2ECC71;
    admin->active = true;
    admin->logged_in = true;

    // Create guest user (inactive by default)
    user_account_t *guest = &data->users[1];
    strcpy(guest->username, "guest");
    strcpy(guest->display_name, "Guest User");
    hash_password("", guest->password_hash);
    guest->type = USER_TYPE_GUEST;
    guest->avatar_color = 0x95A5A6;
    guest->active = false;
    guest->logged_in = false;

    data->user_count = 2;
    data->current_user_id = 0;
    data->selected_user = 0;
    data->show_user_list = true;
    data->guest_enabled = false;
    data->auto_login_enabled = false;
    data->auto_login_user_id = -1;

    panel->user_data = data;
    kprintf("[Users Panel] Initialized with %d users\n", data->user_count);
}

static void users_panel_draw(settings_panel_t *panel, int32_t x, int32_t y,
                             int32_t width, int32_t height) {
    users_panel_data_t *data = (users_panel_data_t *)panel->user_data;
    if (!data) return;

    int32_t cx = x;
    int32_t cy = y;

    // Section: User List
    draw_text(cx, cy, "User Accounts", THEME_LABEL_TEXT);
    cy += CONTENT_LINE_HEIGHT + 8;

    // User list box
    fb_fill_rect(cx, cy, USERS_LIST_WIDTH, data->user_count * USER_ITEM_HEIGHT + 4, THEME_TEXTBOX_BG);
    fb_draw_rect(cx, cy, USERS_LIST_WIDTH, data->user_count * USER_ITEM_HEIGHT + 4, 0xCCCCCC);

    int32_t list_y = cy + 2;
    for (int i = 0; i < data->user_count; i++) {
        user_account_t *user = &data->users[i];
        bool selected = (i == data->selected_user);
        bool is_current = (i == data->current_user_id);

        // Item background
        uint32_t bg = selected ? COLOR_USER_SELECTED : (i % 2 == 0 ? THEME_TEXTBOX_BG : COLOR_USER_BG);
        fb_fill_rect(cx + 2, list_y, USERS_LIST_WIDTH - 4, USER_ITEM_HEIGHT - 2, bg);

        // Avatar
        char initials[3] = {0};
        if (user->display_name[0]) {
            initials[0] = toupper(user->display_name[0]);
            const char *space = strchr(user->display_name, ' ');
            if (space && space[1]) initials[1] = toupper(space[1]);
        }
        draw_avatar(cx + 6, list_y + 6, AVATAR_SIZE, user->avatar_color, initials);

        // Name
        uint32_t text_color = selected ? THEME_TEXTBOX_BG : THEME_LABEL_TEXT;
        draw_text(cx + 6 + AVATAR_SIZE + 8, list_y + 8, user->display_name, text_color);

        // Type badge
        uint32_t badge_color = selected ? 0xCCCCCC : get_user_type_color(user->type);
        draw_text(cx + 6 + AVATAR_SIZE + 8, list_y + 22, get_user_type_name(user->type), badge_color);

        // Current user indicator (green dot)
        if (is_current) {
            int32_t dot_x = cx + USERS_LIST_WIDTH - 14;
            int32_t dot_y = list_y + USER_ITEM_HEIGHT / 2;
            for (int dy = -3; dy <= 3; dy++) {
                for (int dx = -3; dx <= 3; dx++) {
                    if (dx * dx + dy * dy <= 9) {
                        fb_put_pixel(dot_x + dx, dot_y + dy, 0x2ECC71);
                    }
                }
            }
        }

        list_y += USER_ITEM_HEIGHT;
    }

    cy += data->user_count * USER_ITEM_HEIGHT + 12;

    // Add User button
    user_account_t *current = &data->users[data->current_user_id];
    bool is_admin = (current->type == USER_TYPE_ADMIN);
    uint32_t btn_color = is_admin ? 0x4A90C2 : 0xBDC3C7;
    fb_fill_rect(cx, cy, BUTTON_WIDTH, BUTTON_HEIGHT, btn_color);
    draw_text(cx + 20, cy + 5, "Add User", is_admin ? THEME_TEXTBOX_BG : 0x7F8C8D);

    // Selected User Details (right column)
    int32_t detail_x = cx + USERS_LIST_WIDTH + 30;
    int32_t detail_y = y;

    if (data->selected_user >= 0 && data->selected_user < data->user_count) {
        user_account_t *sel = &data->users[data->selected_user];

        draw_text(detail_x, detail_y, "Profile", 0x4A90C2);
        detail_y += CONTENT_LINE_HEIGHT + 8;

        // Large avatar
        char initials[3] = {0};
        if (sel->display_name[0]) {
            initials[0] = toupper(sel->display_name[0]);
            const char *space = strchr(sel->display_name, ' ');
            if (space && space[1]) initials[1] = toupper(space[1]);
        }
        draw_avatar(detail_x, detail_y, 48, sel->avatar_color, initials);

        draw_text(detail_x + 60, detail_y + 5, sel->display_name, THEME_LABEL_TEXT);
        draw_text(detail_x + 60, detail_y + 20, sel->username, THEME_MENU_TEXT_DISABLED);
        draw_text(detail_x + 60, detail_y + 35, get_user_type_name(sel->type),
                  get_user_type_color(sel->type));

        detail_y += 60;

        // Action buttons
        bool is_self = (data->selected_user == data->current_user_id);

        if (is_self) {
            fb_fill_rect(detail_x, detail_y, BUTTON_WIDTH, BUTTON_HEIGHT, 0x4A90C2);
            draw_text(detail_x + 8, detail_y + 5, "Change Name", THEME_TEXTBOX_BG);
            detail_y += BUTTON_HEIGHT + 8;

            fb_fill_rect(detail_x, detail_y, BUTTON_WIDTH + 20, BUTTON_HEIGHT, 0x4A90C2);
            draw_text(detail_x + 8, detail_y + 5, "Change Password", THEME_TEXTBOX_BG);
            detail_y += BUTTON_HEIGHT + 8;

            fb_fill_rect(detail_x, detail_y, BUTTON_WIDTH + 10, BUTTON_HEIGHT, 0x4A90C2);
            draw_text(detail_x + 8, detail_y + 5, "Change Avatar", THEME_TEXTBOX_BG);
        } else if (is_admin && sel->type != USER_TYPE_ADMIN) {
            fb_fill_rect(detail_x, detail_y, BUTTON_WIDTH + 20, BUTTON_HEIGHT, 0x7F8C8D);
            draw_text(detail_x + 8, detail_y + 5, "Reset Password", THEME_TEXTBOX_BG);
            detail_y += BUTTON_HEIGHT + 8;

            fb_fill_rect(detail_x, detail_y, BUTTON_WIDTH, BUTTON_HEIGHT, 0xE74C3C);
            draw_text(detail_x + 8, detail_y + 5, "Delete User", THEME_TEXTBOX_BG);
        }
    }

    // Login Options Section
    int32_t opts_y = y + 200;
    draw_text(cx, opts_y, "Login Options", 0x4A90C2);
    opts_y += CONTENT_LINE_HEIGHT + 8;

    // Checkboxes using settings widgets
    sw_draw_checkbox(cx, opts_y, data->auto_login_enabled, "Automatic login");
    opts_y += 24;

    sw_draw_checkbox(cx, opts_y, data->show_user_list, "Show user list at login");
    opts_y += 24;

    sw_draw_checkbox(cx, opts_y, data->guest_enabled, "Enable guest account");
}

static void users_panel_event(settings_panel_t *panel, gui_event_t *event) {
    users_panel_data_t *data = (users_panel_data_t *)panel->user_data;
    if (!data) return;

    if (event->type != EVENT_MOUSE_UP) return;

    int32_t mx = event->mouse_x;
    int32_t my = event->mouse_y;

    // Convert to content coordinates
    int32_t cx = panel->content_x;
    int32_t cy = panel->content_y;

    // Check user list clicks
    int32_t list_y = cy + CONTENT_LINE_HEIGHT + 8 + 2;
    if (mx >= cx && mx < cx + USERS_LIST_WIDTH) {
        for (int i = 0; i < data->user_count; i++) {
            if (my >= list_y && my < list_y + USER_ITEM_HEIGHT) {
                data->selected_user = i;
                kprintf("[Users Panel] Selected user: %s\n", data->users[i].display_name);
                settings_panel_mark_dirty(panel);
                return;
            }
            list_y += USER_ITEM_HEIGHT;
        }
    }

    // Check login option checkboxes
    int32_t opts_y = cy + 200 + CONTENT_LINE_HEIGHT + 8;

    if (mx >= cx && mx < cx + 200) {
        if (my >= opts_y && my < opts_y + 20) {
            data->auto_login_enabled = !data->auto_login_enabled;
            kprintf("[Users Panel] Auto-login: %s\n",
                    data->auto_login_enabled ? "enabled" : "disabled");
            settings_panel_mark_dirty(panel);
            return;
        }
        opts_y += 24;

        if (my >= opts_y && my < opts_y + 20) {
            data->show_user_list = !data->show_user_list;
            kprintf("[Users Panel] Show user list: %s\n",
                    data->show_user_list ? "enabled" : "disabled");
            settings_panel_mark_dirty(panel);
            return;
        }
        opts_y += 24;

        if (my >= opts_y && my < opts_y + 20) {
            data->guest_enabled = !data->guest_enabled;
            data->users[1].active = data->guest_enabled;
            kprintf("[Users Panel] Guest account: %s\n",
                    data->guest_enabled ? "enabled" : "disabled");
            settings_panel_mark_dirty(panel);
            return;
        }
    }
}

static void users_panel_apply(settings_panel_t *panel) {
    kprintf("[Users Panel] Applying changes...\n");
    // In a real OS, this would save to persistent storage
}

static void users_panel_cleanup(settings_panel_t *panel) {
    if (panel->user_data) {
        kfree(panel->user_data);
        panel->user_data = NULL;
    }
    kprintf("[Users Panel] Cleaned up\n");
}

// ============================================================================
// Panel Registration
// ============================================================================

static settings_panel_def_t users_panel_def = {
    .name = "Users",
    .icon = "user",
    .category = SETTINGS_CAT_SECURITY,
    .priority = 10,
    .init = users_panel_init,
    .draw = users_panel_draw,
    .handle_event = users_panel_event,
    .apply = users_panel_apply,
    .cleanup = users_panel_cleanup
};

void users_panel_register(void) {
    settings_register_panel(&users_panel_def);
}
