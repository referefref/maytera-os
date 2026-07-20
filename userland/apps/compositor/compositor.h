// compositor.h - Shared types and constants for MayteraOS Userland Compositor
// Phase 3: Complete Desktop Port
#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <stdint.h>

// ============================================================================
// Forward declarations for libc (freestanding, no standard headers)
// ============================================================================
typedef int bool;
#define true  1
#define false 0
#define NULL ((void *)0)

// String functions (from libc)
extern void *memset(void *s, int c, unsigned long n);
extern void *memcpy(void *dest, const void *src, unsigned long n);
extern void *memmove(void *dest, const void *src, unsigned long n);   // #440 vnc.c
extern int strcmp(const char *s1, const char *s2);
extern int strncmp(const char *s1, const char *s2, unsigned long n);
extern char *strncpy(char *dest, const char *src, unsigned long n);
extern unsigned long strlen(const char *s);

// ============================================================================
// Screen and framebuffer
// ============================================================================
#define MAX_SCREEN_W    1920
#define MAX_SCREEN_H    1080

extern uint32_t *g_fb;           // Framebuffer pointer
extern int32_t   g_fb_width;     // Screen width
extern int32_t   g_fb_height;    // Screen height
extern int32_t   g_fb_pitch;     // Pixels per row (usually == width)

// ============================================================================
// Color constants (ARGB format)
// ============================================================================

// Taskbar
extern uint32_t CLR_TASKBAR_BG;
extern uint32_t CLR_TASKBAR_BORDER;
extern uint32_t CLR_TASKBAR_HOVER;
extern uint32_t CLR_START_BTN;

// Gauges
extern uint32_t CLR_GAUGE_BG;       // #110: theme-driven (compositor_apply_theme)
extern uint32_t CLR_GAUGE_BORDER;   // #110: theme-driven
#define CLR_GAUGE_CPU       0xFF00AA00
#define CLR_GAUGE_RAM       0xFF0088CC
#define CLR_GAUGE_DSK       0xFFCC8800
#define CLR_GAUGE_NET       0xFF8800CC

// Start menu
extern uint32_t CLR_MENU_BG;
extern uint32_t CLR_MENU_SHADOW;
extern uint32_t CLR_MENU_BORDER;
extern uint32_t CLR_MENU_ITEM_HOVER;
extern uint32_t CLR_MENU_ITEM_NORM;
extern uint32_t CLR_MENU_CAT_BG;
extern uint32_t CLR_MENU_TEXT;
extern uint32_t CLR_MENU_SEP;

// Context menu
extern uint32_t CLR_CTX_BG;
extern uint32_t CLR_CTX_BORDER;
extern uint32_t CLR_CTX_HOVER;

// Clock widget
#define CLR_CLOCK_BG        0xCC222222
#define CLR_CLOCK_TEXT      0xFFFFFFFF

// Login screen
#define CLR_LOGIN_BG_TOP    0xFF1A2332
#define CLR_LOGIN_BG_BOT    0xFF0D1B2A
#define CLR_LOGIN_PANEL     0xFF2C3E50
#define CLR_LOGIN_BORDER    0xFF4A6278
#define CLR_LOGIN_TEXT      0xFFECEFF1
#define CLR_LOGIN_DIMMED    0xFF90A4AE
#define CLR_LOGIN_ACCENT    0xFF2196F3
#define CLR_LOGIN_HOVER     0xFF1E88E5
#define CLR_LOGIN_INPUT_BG  0xFF263238
#define CLR_LOGIN_INPUT_BR  0xFF546E7A
#define CLR_LOGIN_ERROR     0xFFEF5350
#define CLR_LOGIN_AVATAR    0xFF455A64
#define CLR_LOGIN_AVATAR_S  0xFF2196F3

// Desktop
#define CLR_DESKTOP_BG      0xFF2C5AA0
#define CLR_ICON_LABEL_BG   0x80000000
#define CLR_ICON_SEL_BG     0x40FFFFFF
#define CLR_TEXT_WHITE       0xFFFFFFFF
#define CLR_TEXT_SHADOW      0xFF000000
#define CLR_VERSION_TEXT     0xFFCCCCCC
extern uint32_t CLR_CHROME_TEXT;
void compositor_apply_theme(int kernel_theme_id);

// Wallpaper gradient
#define CLR_WP_GRAD_TOP     0xFF4A90C2
#define CLR_WP_GRAD_BOT     0xFF1E5A8A

// Wallpaper picker
#define CLR_PICKER_BG       0xFF2D2D2D
#define CLR_PICKER_BORDER   0xFF505050
#define CLR_PICKER_TITLE    0xFF3A3A3A
#define CLR_PICKER_THUMB    0xFF3A3A3A
#define CLR_PICKER_SEL      0xFF4080FF
#define CLR_PICKER_LABEL    0xFFCCCCCC

// Power buttons
#define CLR_POWER_RED       0xFFE06060

// ============================================================================
// Layout constants
// ============================================================================

// Taskbar
#define TASKBAR_HEIGHT      36
#define TASKBAR_PADDING     4
#define TASKBAR_BTN_SIZE    28
#define TASKBAR_ICON_SPACE  4

// Gauges
#define GAUGE_WIDTH         80
#define GAUGE_HEIGHT        22
#define GAUGE_SPACING       4

// Start menu
#define START_MENU_WIDTH    300
#define START_MENU_ITEM_H   26
#define START_MENU_CAT_H    28
#define START_MENU_PADDING  8
#define START_MENU_SEP_H    12
#define START_MENU_POWER_H  40
#define START_MENU_MAX_ITEMS 96

// Desktop icons
extern int DESKTOP_ICON_SIZE;
void compositor_apply_icon_size(int sz);
void draw_text_ttf(int32_t x, int32_t y, const char *text, int size, uint32_t color);

// ---- Notifications subsystem (#168, notif.c) ----
void notif_init(void);                        // reset spool at session start
void notif_tick(void);                       // poll spool + expire toasts (loop)
void notif_render(void);                      // draw toasts + notification center
int  notif_handle_mouse(int x, int y, int clicked);  // returns 1 if consumed
int  notif_unread(void);                      // tray bell unread badge count
void notif_toggle_center(void);               // bell click opens/closes center
int  notif_center_open(void);

// Desktop widgets (#77): analog clock, calendar, sheep pet.
void widgets_render(void);
void widgets_collect_damage(void);   // #102/#379 idle dirty-rect state step
extern int g_widgets_draw_only;      // 1 = widgets_render() draws only, no state advance
extern int g_widgets_enabled;
extern int g_sheep_enabled;
int  sheep_hit(int x, int y);
void sheep_grab(int x, int y);
void sheep_drag_to(int x, int y);
void sheep_release(void);
int  sheep_is_dragging(void);
extern int g_dog_enabled;
int  dog_hit(int x, int y);
void dog_grab(int x, int y);
void dog_drag_to(int x, int y);
void dog_release(void);
extern int g_show_clock, g_show_calendar, g_sheep_speed, g_sheep_size, g_sheep_style, g_sheep_count;
extern int g_show_weather, g_show_crypto, g_show_stocks;   // #81-83 internet widgets
// #274 new widgets: System Monitor (6), Timer/Stopwatch (7), World Time (8).
extern int g_show_sysmon, g_sysmon_x, g_sysmon_y, g_sysmon_locked;
extern int g_show_timer, g_timer_x, g_timer_y, g_timer_locked;
extern int g_show_worldtime, g_worldtime_x, g_worldtime_y, g_worldtime_locked;
extern int g_show_uptime, g_uptime_x, g_uptime_y, g_uptime_locked;   // #282/#341
#define WT_ZONES 3
extern int g_wt_off[WT_ZONES];
// #81-83 info-card positions + per-widget locks (persisted in the UI profile).
extern int g_weather_x, g_weather_y, g_crypto_x, g_crypto_y, g_stocks_x, g_stocks_y;
extern int g_weather_locked, g_crypto_locked, g_stocks_locked;
// Per-widget Settings dialog (weather location / crypto / stock symbols).
void widget_settings_open(int id);
int  widget_settings_is_open(void);
void widget_settings_render(void);
int  widget_settings_handle_key(int key);
int  widget_settings_handle_mouse(int x, int y, int click);

// Widget registry: the SINGLE source of truth for which desktop widgets exist.
// Both the desktop widget layer and the dynamic widgets tray menu read this so
// the tray menu is never a hardcoded duplicate list. Add a widget here and it
// appears in the tray menu automatically (also wire its bind in traymenu.c
// tm_get/tm_set and persist it in profile.c).
typedef struct {
    const char *label;   // display name in the tray menu
    const char *bind;    // tm_get/tm_set + profile key (live enable flag)
    int        *flag;    // direct pointer to the live enable flag
} widget_desc_t;
const widget_desc_t *widget_registry(int *count);

// stickies.c - Desktop sticky notes (#270)
extern int g_show_stickies;
void stickies_load(void);
void stickies_render(void);
void stickies_tick(void);
int  stickies_hit(int x, int y);
int  stickies_press(int x, int y);          // returns 1 if consumed
void stickies_drag_to(int x, int y);
void stickies_release(void);
int  stickies_is_dragging(void);
int  stickies_editing(void);
int  stickies_handle_key(int key);
void sticky_new(void);                       // create a note (desktop right-click)
int  sticky_new_at(int x, int y);

// Tray YAML menus (#90)
void traymenu_init(void);
void traymenu_render(void);
bool traymenu_handle_mouse(int mx, int my, bool pressed, bool held);
void traymenu_open_for_icon(int icon, int anchor_x);
void traymenu_close(void);
extern int g_tray_menu_open;

// UI profile persistence (#92)
void profile_load(void);
void profile_save(void);
void profile_tick(void);
int  text_width_ttf(const char *text, int size);
extern int g_font_px;   // current UI label point size (#58)
#define DESKTOP_ICON_MARGIN_X 20
#define DESKTOP_ICON_MARGIN_Y 20
extern int DESKTOP_ICON_SPACING_X;
extern int DESKTOP_ICON_SPACING_Y;
#define DESKTOP_ICON_MAX    32
#define DESKTOP_ICON_NAME_LEN 32

// Context menu
#define CTX_MENU_WIDTH      160
#define CTX_MENU_ITEM_H     24
#define CTX_MENU_SEP_H      8
#define CTX_MENU_MAX_ITEMS  16

// Clock widget
#define CLOCK_PADDING_X     12
#define CLOCK_PADDING_Y     6
#define CLOCK_MARGIN_RIGHT  16
#define CLOCK_MARGIN_TOP    10
#define CLOCK_CORNER_RADIUS 10

// Login screen
#define LOGIN_PANEL_W       400
#define LOGIN_PANEL_H       360
#define LOGIN_AVATAR_SIZE   64
#define LOGIN_AVATAR_SPACE  20
#define LOGIN_INPUT_W       280
#define LOGIN_INPUT_H       32
#define LOGIN_BUTTON_W      280
#define LOGIN_BUTTON_H      36
#define LOGIN_MAX_PASSWORD  64
#define LOGIN_MAX_USERS     16

// Wallpaper picker
#define THUMB_WIDTH         64
#define THUMB_HEIGHT        48
#define THUMB_PADDING       8
#define THUMB_COLS          5
#define THUMB_CELL_W        (THUMB_WIDTH + THUMB_PADDING)
#define THUMB_CELL_H        (THUMB_HEIGHT + THUMB_PADDING + 16)
#define PICKER_WIDTH        (THUMB_COLS * THUMB_CELL_W + THUMB_PADDING * 2)
#define PICKER_HEIGHT       400
#define PICKER_TITLE_H      24
#define MAX_WALLPAPERS      64

// Screensaver
#define SS_MAX_STARS        1600
#define SS_MAX_LINES        20
#define SS_MAX_BUBBLES      10
#define SS_MAX_OBJS         10
#define SS_DEFAULT_TIMEOUT  120  // seconds

// Font
#define FONT_CHAR_W         8
#define FONT_CHAR_H         16

// ============================================================================
// Icon IDs (must match kernel gui/icons.h)
// ============================================================================
typedef enum {
    ICON_CATEGORIES = 0,
    ICON_TERMINAL,
    ICON_HIGHLIGHT,
    ICON_FOLDER,
    ICON_CALCULATOR,
    ICON_COG,
    ICON_INFO_CIRCLE,
    ICON_IMAGE,
    ICON_MUSIC,
    ICON_WINDOW,
    ICON_POWER,
    ICON_REFRESH,
    ICON_HOME,
    ICON_FILE,
    ICON_PALETTE,
    ICON_PAINT,
    ICON_CLOCK,
    ICON_TASK_MANAGER,
    ICON_LOG_VIEWER,
    ICON_TRASH,
    ICON_TRASH_FULL,
    ICON_GAME,
    ICON_GAME_DOOM,
    ICON_GAME_PONG,
    ICON_GAME_SOLITAIRE,
    ICON_GAME_LEMMINGS,
    ICON_COMPUTER,
    ICON_BROWSER,
    ICON_IRC,
    ICON_VIDEO,
    ICON_NETWORK,
    ICON_SLIDERS,   // tray audio/quick-settings glyph (color-icon only)
    ICON_CHEVD,     // start-menu expanded indicator
    ICON_CHEVR,     // start-menu collapsed indicator
    ICON_WIN3X,     // Win16/Win3.x program glyph (color-icon only) (#208)
    ICON_DOSAPP,    // MS-DOS program glyph (color-icon only) (#208)
    ICON_COUNT
} icon_id_t;

// ============================================================================
// Structures
// ============================================================================

// Desktop icon
typedef struct {
    char name[DESKTOP_ICON_NAME_LEN];
    char exec_path[128];
    icon_id_t icon_id;
    int32_t px, py;        // absolute screen position (top-left of icon image)
    bool selected;
    bool visible;
} desktop_icon_t;

// Start menu item
typedef struct {
    char name[48];
    char exec_path[128];
    icon_id_t icon_id;
    bool is_separator;
    bool is_win16;        // launch via win16_run() instead of sys_spawn()
    int  launch_type;     // 0=native (sys_spawn), 1=win16, 2=dos (#208)
} menu_item_t;

// Start menu category
typedef struct {
    char label[40];
    bool expanded;
    int item_start;   // index into g_menu_items[]
    int item_count;
} menu_category_t;

// Context menu item
typedef struct {
    char label[32];
    bool is_separator;
    int action_id;
} ctx_menu_item_t;

// Screensaver types
typedef enum {
    SS_NONE = 0,
    SS_BLANK,
    SS_STARFIELD,
    SS_LINES,
    SS_BUBBLES,
    SS_MATRIX,
    SS_FLUX,
    SS_PLASMA,
    SS_GLCUBE,    /* #319 TinyGL spinning textured cube (reconciled #336) */
    SS_GLMATRIX   /* #319 TinyGL 3D matrix code rain (reconciled #336) */
} screensaver_type_t;

// Screensaver star
typedef struct {
    int32_t x, y, z;
} ss_star_t;

// Deep-space object (galaxy/black hole/comet/nebula/double star)
typedef struct {
    int32_t x, y, z;
    int32_t type;   // 0=galaxy 1=blackhole 2=comet 3=nebula 4=doublestar
    uint32_t color;
    uint32_t color2; // secondary (core / 2nd star)
    int16_t arms;    // spiral arm count 2..5
    int16_t incl;    // inclination 2..16 (2=edge-on, 16=face-on)
    int16_t pa;      // position angle 0..255
    int16_t spin;    // rotation speed, signed
    int16_t sizem;   // size multiplier in 1/8 units
} ss_obj_t;

// Screensaver line
typedef struct {
    int32_t x1, y1, x2, y2;
    int32_t dx1, dy1, dx2, dy2;
    uint32_t color;
} ss_line_t;

// Screensaver bubble
typedef struct {
    int32_t x, y;
    int32_t radius;
    int32_t max_radius;
    int32_t dr;
    uint32_t color;
} ss_bubble_t;

// Login state
typedef enum {
    LOGIN_STATE_SELECT_USER = 0,
    LOGIN_STATE_PASSWORD,
    LOGIN_STATE_ERROR,
    LOGIN_STATE_SUCCESS
} login_state_t;

// Wallpaper entry
typedef struct {
    const char *name;
    const char *filename;  // NULL for gradient
} wallpaper_entry_t;

// ============================================================================
// Global state access (shared across modules)
// ============================================================================

// Mouse state
extern int32_t  g_mouse_x, g_mouse_y;
extern uint32_t g_mouse_buttons;
extern uint32_t g_mouse_prev_buttons;

// Desktop state
extern bool g_start_menu_open;
extern bool g_context_menu_open;
extern int32_t g_context_menu_x, g_context_menu_y;
extern bool g_wallpaper_picker_open;

// Command launcher (Spotlight-style AI prompt) - launcher.c
extern bool g_launcher_open;
extern int  g_draw_blend;            // draw.c global alpha (255 = opaque)
void launcher_open(void);
void launcher_close(void);
void launcher_toggle(void);
int  launcher_is_open(void);
void launcher_render(void);
int  launcher_handle_key(int key);
int  launcher_handle_mouse(int x, int y, int clicked);

// Login state
extern bool g_logged_in;
extern int  g_login_uid;
extern char g_login_username[64];

// Screensaver state
extern bool g_screensaver_active;
extern uint64_t g_idle_ms;   // monotonic uptime_ms() of last user input

// Redraw flag
extern bool g_needs_redraw;

// ============================================================================
// Module APIs (each module exposes init/render/handle functions)
// ============================================================================

// draw.c - Dirty-rectangle compositing (#102/#379).
// Global clip rectangle (exclusive x1/y1) that bounds every primitive so the
// idle compositor recomposites only changed regions. Default = full screen.
extern int g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1;
void draw_set_clip(int x, int y, int w, int h);   // intersect with the screen
void draw_clear_clip(void);                        // reset to full screen
static inline int draw_pt_in_clip(int px, int py) {
    return px >= g_clip_x0 && px < g_clip_x1 && py >= g_clip_y0 && py < g_clip_y1;
}
// Per-frame damage list: the rectangles that changed. The idle compositor
// recomposites + presents only these; overlapping rects merge, overflow
// collapses to a bounding rect.
void damage_reset(void);
void damage_add(int x, int y, int w, int h);
int  damage_count(void);
int  damage_get(int i, int *x, int *y, int *w, int *h);

// draw.c - Drawing primitives
void draw_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void draw_hline(int32_t x, int32_t y, int32_t w, uint32_t color);
void draw_vline(int32_t x, int32_t y, int32_t h, uint32_t color);
void draw_rect_outline(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
void draw_rounded_rect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t r, uint32_t color);
void draw_circle_filled(int32_t cx, int32_t cy, int32_t r, uint32_t color);
void draw_circle_outline(int32_t cx, int32_t cy, int32_t r, uint32_t color);
void draw_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t top, uint32_t bot);
void draw_char(int32_t x, int32_t y, char c, uint32_t color);
void draw_text(int32_t x, int32_t y, const char *text, uint32_t color);
uint32_t draw_luminance(uint32_t c);
uint32_t readable_ink(uint32_t bg);
uint32_t readable_ink_dim(uint32_t bg);
uint32_t readable_accent(uint32_t color, uint32_t bg);
void draw_text_centered(int32_t cx, int32_t y, const char *text, uint32_t color);
void draw_text_shadow(int32_t x, int32_t y, const char *text, uint32_t fg, uint32_t shadow);
void draw_char_large(int32_t x, int32_t y, char c, uint32_t color, int scale);
void draw_text_large(int32_t x, int32_t y, const char *text, uint32_t color, int scale);
int  text_width(const char *text);
int  text_width_large(const char *text, int scale);
void draw_putpixel(int32_t x, int32_t y, uint32_t color);

// Remote screen capture (screenshot.c). screenshot_poll() runs once per frame
// and captures when /SCREENSHOT.REQ appears; screenshot_capture() writes a
// downscaled 8-bit BMP of the current composited backbuffer to `path`.
void screenshot_poll(void);
int  screenshot_capture(const char *path);

// Live remote view + control over RFB/VNC (vnc.c, #440). vnc_poll() runs once
// per frame (reverse-connects to a listening viewer per /CONFIG/VNC.CFG - see
// the comment at the top of vnc.c for why the compositor is the RFB server but
// the *client* side of the TCP connect). The two dirty-tracking hooks let the
// main loop tell the RFB layer which screen regions changed this frame, reusing
// the same #379 damage-rectangle machinery screenshot.c/widgets.c already use.
void vnc_poll(void);
void vnc_mark_full_dirty(void);
void vnc_mark_rect_dirty(int x, int y, int w, int h);

// icons.c - Icon rendering
void icon_draw(icon_id_t id, int32_t x, int32_t y, uint32_t color);
int  icon_color_count(void);
int  icon_load_color(icon_id_t id, const char *path);
int  icon_draw_color_if_present(icon_id_t id, int32_t x, int32_t y, int32_t size);
int  icon_draw_color_tinted(icon_id_t id, int32_t x, int32_t y, int32_t size, uint32_t ink);
void icon_draw_scaled(icon_id_t id, int32_t x, int32_t y, int32_t size, uint32_t color);

// login.c - Login screen
void login_init(void);
int  login_run(void);  // Returns 0 on success, blocks until authenticated
void login_render(void);
void login_handle_key(int key);
void login_handle_mouse(int32_t x, int32_t y, bool clicked);

// desktop.c - Desktop surface
void desktop_init(void);
void desktop_render(void);
void desktop_handle_mouse(int32_t x, int32_t y, bool left_click, bool right_click, bool dbl_click);
void desktop_render_version(void);
// Icon drag + rubber-band selection (driven per-frame from main.c, like widgets).
// desktop_press returns true if the press landed on an icon or empty desktop and
// started a drag/rubber-band (so the caller should treat the event as consumed
// for that frame's icon layer); contextmenu/window/taskbar layers run first.
bool desktop_press(int32_t x, int32_t y);        // left button just pressed
void desktop_drag(int32_t x, int32_t y);         // mouse moved while button held
void desktop_release(int32_t x, int32_t y);      // left button released
bool desktop_is_dragging(void);                  // a drag or rubber-band in progress
void desktop_render_overlay(void);               // draw the rubber-band rectangle
// Right-click context-menu actions (wired from contextmenu.c).
void desktop_auto_arrange(void);                 // re-flow all icons to default top grid
void desktop_align_to_grid(void);                // snap current positions to the grid
// Persistence hooks used by profile.c.
int  desktop_icon_count(void);
void desktop_get_icon_pos(int idx, int32_t *x, int32_t *y);
void desktop_set_icon_pos(int idx, int32_t x, int32_t y);
int  desktop_positions_hash(void);               // folded into the profile change hash

// taskbar.c - Taskbar with gauges
void taskbar_init(void);
void taskbar_render(void);
void taskbar_update(void);
void taskbar_collect_damage(void);              // #102/#379 idle dirty-rect
int  taskbar_cpu_snapshot(unsigned int *cores, int *ncores);  // #102 shared CPU%
bool taskbar_handle_mouse(int32_t x, int32_t y, bool clicked);
bool taskbar_popup_active(void);                                    // #241
bool taskbar_popup_handle_mouse(int32_t x, int32_t y, bool clicked); // #241
int32_t taskbar_get_y(void);

// #387 Dock / taskbar layout styles. Selectable live from Settings -> Appearance
// and persisted in the UI profile (key "dock_style"). DEFAULT reproduces the
// classic MayteraOS bottom taskbar byte-identically.
#define DOCK_DEFAULT     0   // classic bottom taskbar (start + apps + tray + gauges)
#define DOCK_LUMINA      1   // Lumina: top menu bar + floating bottom glass dock
#define DOCK_CLASSIC_UNIX 2  // Classic UNIX: beveled CDE/Motif-style front panel (workspace switcher)
#define DOCK_RETRO_BENCH 3   // Retro Bench: top screen bar (depth/zoom gadgets)
#define DOCK_COUNT       4
extern int g_dock_style;
void taskbar_set_style(int s);               // apply a layout live
int  taskbar_top_inset(void);                // px reserved at the TOP of screen
int  taskbar_bottom_inset(void);             // px reserved at the BOTTOM of screen
int  taskbar_menu_drops_from_top(void);      // 1 = start menu drops from a top bar

// startmenu.c - Start menu
void startmenu_init(void);
void startmenu_render(void);
bool startmenu_handle_mouse(int32_t x, int32_t y, bool clicked);
void startmenu_toggle(void);

// clock.c - Floating clock
void clock_render(void);

// contextmenu.c - Right-click menu
void contextmenu_init(void);
void contextmenu_render(void);
bool contextmenu_handle_mouse(int32_t x, int32_t y, bool clicked);
void contextmenu_open(int32_t x, int32_t y);
void contextmenu_close(void);

// wallpaper.c - Wallpaper system
void wallpaper_init(void);
void wallpaper_render_background(void);
void wallpaper_load(int index);
void wallpaper_render_picker(void);
bool wallpaper_picker_handle_mouse(int32_t x, int32_t y, bool clicked);
void wallpaper_picker_open(void);
void wallpaper_picker_close(void);

// screensaver.c - Screensaver
void screensaver_init(void);
void screensaver_set_type(int t);
void screensaver_render(void);
void screensaver_on_input(void);
bool screensaver_check_timeout(void);

// cursor drawing
void cursor_render(void);

#endif // COMPOSITOR_H
