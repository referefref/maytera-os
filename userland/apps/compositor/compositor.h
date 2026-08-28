// compositor.h - Shared types and constants for MayteraOS Userland Compositor
// Phase 3: Complete Desktop Port
#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <stdint.h>
#include "../../libc/tz.h"   // #49/#50: THE timezone list + local-clock helper
// UI SCALE (task uiscale, compositor half): the compositor draws chrome
// directly to the physical framebuffer, outside the kernel's per-window
// coordinate transform, so it must scale its own geometry. ui_px()/ui_span()/
// ui_unpx() and the cached g_ui_scale_pct/g_ui_scale_gen live here; see
// uiscale.h for the full contract. Included at this single chokepoint so
// every compositor .c file (all of which include compositor.h already) gets
// the scaling primitives without a per-file include.
#include "../../libc/uiscale.h"

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
// (#745 taskbar/tray) theme key taskbar_active - parsed since #711, read by
// the kernel side (theme_get_color_by_id case 31) but never by the
// compositor until now. Used for the Start/Maytera chip's open-state wash;
// see chrome_chip() in taskbar.c.
extern uint32_t CLR_TASKBAR_ACTIVE;
extern uint32_t CLR_START_BTN;

// Gauges
extern uint32_t CLR_GAUGE_BG;       // #110: theme-driven (compositor_apply_theme)
extern uint32_t CLR_GAUGE_BORDER;   // #110: theme-driven
#define CLR_GAUGE_CPU       0xFF00AA00
#define CLR_GAUGE_RAM       0xFF0088CC
#define CLR_GAUGE_DSK       0xFFCC8800
#define CLR_GAUGE_NET       0xFF8800CC

// #battmeter: battery tray meter fill colour by charge band. Same house
// style as the CLR_GAUGE_* constants immediately above (fixed semantic
// accent, not a theme token - battery health colour is a universal
// convention, not a look the theme should be able to recolour away).
#define CLR_BATTERY_OK       0xFF2FBF4F   // > 30%: green
#define CLR_BATTERY_LOW      0xFFCC8800   // 16-30%: amber (matches CLR_GAUGE_DSK)
#define CLR_BATTERY_CRIT     0xFFCC3333   // <= 15%: red

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
// #745: CLR_LOGIN_INPUT_BR 0xFF546E7A REMOVED. It had ZERO callers in the
// whole tree - login.c's draw_password_field() strokes its field with
// CLR_LOGIN_ACCENT unconditionally and never had a resting state. It was
// nonetheless cited in review as the shared token that would fix the lock
// screen, the boot gate and future modals "in one change". It was shared with
// nothing. Deleting it stops the next reader making the same inference.
#define CLR_LOGIN_ERROR     0xFFEF5350
// #745 avatar port (docs/LOGIN_AVATARS_AND_PROFILE.html): the fill no longer
// changes color on selection (the old CLR_LOGIN_AVATAR_S swap put the fixed
// CLR_LOGIN_TEXT initial at 2.71:1 on the accent-blue fill, under the 4.5:1
// text floor - see the design doc section 3). One uniform, verified fill;
// selection is communicated by the ring alone (section 7.2). CLR_LOGIN_AVATAR
// is kept as the old name/value so startmenu.c's account chip (line ~1710,
// out of scope for this port) keeps compiling and looking the way it always
// has; new code should read CLR_LOGIN_AVATAR_FILL.
#define CLR_LOGIN_AVATAR_FILL   0xFF52606F   // draw_blend(CLR_LOGIN_PANEL, white, 46)
#define CLR_LOGIN_AVATAR        CLR_LOGIN_AVATAR_FILL
#define CLR_LOGIN_AVATAR_HI     0xFF8F98A1   // decorative highlight patch, upper-left (glass approx)
#define CLR_LOGIN_AVATAR_SHADE  0xFF3B4550   // decorative shade patch, lower-right (glass approx)
#define CLR_LOGIN_AVATAR_RING       0xFF9BC0D6   // resting   - 5.70:1 vs panel, 3.34:1 vs fill
#define CLR_LOGIN_AVATAR_RING_HOVER 0xFFB0CEDF   // hover     - 6.66:1 vs panel, 3.91:1 vs fill
#define CLR_LOGIN_AVATAR_RING_SEL   0xFF9ED6FF   // selected  - 7.07:1 vs panel, 4.14:1 vs fill
// draw_avatar() state (replaces the old bool "highlight" - see login.c).
#define AVATAR_ST_NORMAL   0
#define AVATAR_ST_HOVER    1
#define AVATAR_ST_SELECTED 2

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

// #uiscale: every pixel dimension below is scaled AT THE DEFINITION via
// ui_px(), so every consumer (draw side AND hit-test side alike) picks up
// the scale factor automatically with no per-call-site edit, and draw/hit
// can never drift apart the way a copy-pasted literal would. Verified none
// of these are used as a compile-time array size, switch case, or #if
// operand anywhere in the tree (grepped for "[NAME]" / "case NAME" / "#if
// ... NAME" across every .c/.h here) before converting - a macro used in any
// of those contexts would need a raw _BASE form plus a scaled accessor
// instead, the way FONT_CHAR_W/H just above does. COUNTS/LIMITS (e.g.
// START_MENU_MAX_ITEMS, DESKTOP_ICON_MAX, CTX_MENU_MAX_ITEMS, THUMB_COLS,
// LOGIN_MAX_PASSWORD/USERS, MAX_WALLPAPERS) are left untouched: they are not
// pixel measurements.

// Taskbar
#define TASKBAR_HEIGHT      ui_px(36)
#define TASKBAR_PADDING     ui_px(4)
#define TASKBAR_BTN_SIZE    ui_px(28)
#define TASKBAR_ICON_SPACE  ui_px(4)

// Gauges
#define GAUGE_WIDTH         ui_px(80)
#define GAUGE_HEIGHT        ui_px(22)
#define GAUGE_SPACING       ui_px(4)

// Start menu
#define START_MENU_WIDTH    ui_px(300)
#define START_MENU_ITEM_H   ui_px(26)
#define START_MENU_CAT_H    ui_px(28)
#define START_MENU_PADDING  ui_px(8)
#define START_MENU_SEP_H    ui_px(12)
#define START_MENU_POWER_H  ui_px(40)
// #<startmenu-rust> was 96 (fit the old hardcoded ~58-item default menu with
// little headroom). Now that every entry (defaults, Win16 program groups, App
// Store installs, user pins) is data merged at runtime, 96 is too tight to
// grow into without a silent, invisible truncation. Bumped with real headroom;
// see startmenu.c's add_item_typed(), which already drops (does not corrupt)
// anything past this cap.
#define START_MENU_MAX_ITEMS 256
// Whisker-Menu-style uplift: user/profile header row + search box, drawn above
// the Favorites/Recent/category rows. Menu width/item icon size are now
// runtime-configurable (Settings "Start Menu" panel); START_MENU_WIDTH above
// stays as the compiled-in default.
#define START_MENU_HEADER_H ui_px(40)
#define START_MENU_SEARCH_H ui_px(32)

// Desktop icons
extern int DESKTOP_ICON_SIZE;
void compositor_apply_icon_size(int sz);
void draw_text_ttf(int32_t x, int32_t y, const char *text, int size, uint32_t color);

// ---- Notifications subsystem (#168, notif.c) ----
void notif_init(void);                        // reset spool at session start
void notif_tick(void);                       // poll spool + expire toasts (loop)
void notif_collect_damage(void);              // #585: per-toast damage rects (idle path, #379)
void notif_render(void);                      // draw toasts + notification center
int  notif_handle_mouse(int x, int y, int clicked);  // returns 1 if consumed
int  notif_handle_key(int key);               // docs/NOTIFICATIONS_DESIGN.html sect;5: Esc/Tab/Enter, returns 1 if consumed
int  notif_unread(void);                      // tray bell unread badge count
void notif_toggle_center(void);               // bell click opens/closes center
int  notif_center_open(void);
// #127 verification-only: push a notification straight into history/toast
// state via the exact same push_notification() the spool poller calls,
// bypassing the file write. Used by testhook.c's NOTIFY verb ONLY (gated the
// same as every other verb in that file); never called from shipping code.
void notif_test_push(int sev, const char *title, const char *body);

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
// #742: non-zero while the last sticky-note save FAILED. The notes are still
// live in RAM and the save is retried on every tick; this is what the renderer
// uses to tell the user, on the notes themselves, without needing the disk.
extern int g_stickies_save_failed;
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
// #745 P2: the single apply route for a tray bind (widgets, sheep, sound,
// power toggles, ...); see traymenu.c tm_set(). Used by main.c's widget
// live-apply channel so it never sets a widget_desc_t.flag directly and
// misses a bind's side effect (show_aichat spawns/stops the app).
void traymenu_set_bind(const char *bind, int v);
extern int g_tray_menu_open;

// UI profile persistence (#92)
void profile_load(void);
void profile_save(void);
void profile_tick(void);
void profile_current_username(char *out, int outsz);
int  text_width_ttf(const char *text, int size);

// #204: word-wrap primitive shared with confirmdialog.c. Was private to
// notif.c (#762 toast wrapping) until the confirm/notice card needed the
// exact same greedy word-wrap + hard-break-a-too-wide-word + real-measured-
// ellipsis behavior - every one of the confirm dialog's 7 call sites (Shut
// Down/Restart/Log Out/Lock, Force Quit, Recycle Bin Delete/Empty, End Task/
// Kill) was passing one whole unwrapped sentence and overflowing the card's
// 328px body width (measured 474-727px, docs/CONFIRM_MODAL_DESIGN.html
// section 8), so this got promoted rather than re-hand-rolled a second time
// - the exact "several bespoke implementations, none shared" pattern this
// project keeps re-finding. UI_WRAP_COL is the shared per-line buffer
// stride; keep it equal to notif.c's NBODY and confirmdialog.h's
// CONFIRM_LINE_MAX (confirmdialog.h defines CONFIRM_LINE_MAX as this macro
// directly, so there is nothing to drift).
#define UI_WRAP_COL 120
int wrap_text_ttf(const char *body, int size, int max_w, int max_lines,
                  char out[][UI_WRAP_COL]);
extern int g_font_px;   // current UI label point size (#58)
// #uiscale TEMPORARY opt-out - see the declaration/rationale in main.c next
// to g_ui_scale_native_text. push() before, pop() after (every return path,
// or one guarded exit) drawing a surface whose own box geometry is not yet
// scaled, so its text renders at native 1x to match.
extern int g_ui_scale_native_text;
void ui_scale_native_push(void);
void ui_scale_native_pop(void);
#define DESKTOP_ICON_MARGIN_X ui_px(20)
#define DESKTOP_ICON_MARGIN_Y ui_px(20)
extern int DESKTOP_ICON_SPACING_X;
extern int DESKTOP_ICON_SPACING_Y;
#define DESKTOP_ICON_MAX    32
#define DESKTOP_ICON_NAME_LEN 32

// Context menu
#define CTX_MENU_WIDTH      ui_px(160)
#define CTX_MENU_ITEM_H     ui_px(24)
#define CTX_MENU_SEP_H      ui_px(8)
#define CTX_MENU_MAX_ITEMS  16
// Which surface opened the context menu (contextmenu.c): the desktop's fixed
// action set, or a Start-menu item's Pin/Add-to-Desktop/Properties set.
#define CTX_MODE_DESKTOP  0
#define CTX_MODE_MENUITEM 1
#define CTX_MODE_DOCK     2   // #44: dock item right-click (taskbar.c XFCE dock)
#define CTX_MODE_VOLUME   3   // #250: removable-volume desktop icon right-click

// Clock widget
#define CLOCK_PADDING_X     ui_px(12)
#define CLOCK_PADDING_Y     ui_px(6)
#define CLOCK_MARGIN_RIGHT  ui_px(16)
#define CLOCK_MARGIN_TOP    ui_px(10)
#define CLOCK_CORNER_RADIUS ui_px(10)

// Login screen
// #567: matches lockscreen.c's LOCK_PANEL_W/H (380x340) exactly so the login
// and lock panels read as the same sibling shape, per the approved mockup.
#define LOGIN_PANEL_W       ui_px(380)
#define LOGIN_PANEL_H       ui_px(340)
#define LOGIN_AVATAR_SIZE   ui_px(64)
#define LOGIN_AVATAR_SPACE  ui_px(20)
#define LOGIN_INPUT_W       ui_px(280)
#define LOGIN_INPUT_H       ui_px(32)
#define LOGIN_BUTTON_W      ui_px(280)
#define LOGIN_BUTTON_H      ui_px(36)
#define LOGIN_MAX_PASSWORD  64
#define LOGIN_MAX_USERS     16

// Wallpaper picker
#define THUMB_WIDTH         ui_px(64)
#define THUMB_HEIGHT        ui_px(48)
#define THUMB_PADDING       ui_px(8)
#define THUMB_COLS          5   // a COUNT, not a pixel size - not scaled
#define THUMB_CELL_W        (THUMB_WIDTH + THUMB_PADDING)
#define THUMB_CELL_H        (THUMB_HEIGHT + THUMB_PADDING + ui_px(16))
#define PICKER_WIDTH        (THUMB_COLS * THUMB_CELL_W + THUMB_PADDING * 2)
#define PICKER_HEIGHT       ui_px(400)
#define PICKER_TITLE_H      ui_px(24)
#define MAX_WALLPAPERS      64

// Screensaver
#define SS_MAX_STARS        1600
#define SS_MAX_LINES        20
#define SS_MAX_BUBBLES      10
#define SS_MAX_OBJS         10
// #652: was 120s (2 minutes) - the very bottom of the Settings 1-60min
// slider range (#115). 10 minutes is the conventional idle-timeout default.
// IMPORTANT: this is a FALLBACK GUARD ONLY - screensaver_check_timeout()
// below uses it when get_ss_delay() returns something bogus (< 5s), not as
// the live default. On a fresh boot with no Settings override yet, the
// number that actually governs is the kernel's own g_screensaver_delay
// initializer (kernel/proc/syscall.c, SYS_GET_SS_DELAY backing store),
// which is a SEPARATE constant this session did not touch (compositor-only
// scope, see #652 CHANGELOG entry) - it must be updated to match or a fresh
// boot still activates at the old 120s regardless of this define.
#define SS_DEFAULT_TIMEOUT  600  // seconds (10 minutes)
// #650: minimum milliseconds between screensaver repaints (~15 fps). The
// screensaver was previously uncapped and repainted on every main-loop
// iteration; see the FRAME CAP note at its render call site in main.c. The
// main loop sleeps 33ms when active, so this quantises to a repaint every
// second iteration.
#define SS_FRAME_MIN_MS  60ULL
// #652: once the screensaver has been running CONTINUOUSLY (not idle-before-
// activation, but actual on-screen runtime) for this long, main.c's
// render_frame() stops calling screensaver_render() altogether, paints one
// black frame, and then does nothing further per tick until real input wakes
// it (screensaver_on_input(), unchanged). This bounds the steady-state cost
// of an unattended machine to ~0 instead of the ~14% of a core measured
// (VM <vmid>, golden 1016) for the GL/plasma savers running indefinitely at
// the #650 SS_FRAME_MIN_MS-throttled rate. 15 minutes here plus the
// SS_DEFAULT_TIMEOUT idle wait above totals ~25 minutes idle before the
// display goes fully quiet. See screensaver_active_since_ms() below.
#define SS_BLANK_AFTER_SEC   900  // 15 minutes
#define SS_BLANK_AFTER_MS    (SS_BLANK_AFTER_SEC * 1000ULL)
// #596: how long after a true-fullscreen app's last presented frame the
// screensaver stays suppressed. A few seconds, so a game that pauses rendering
// entirely (or exits) still gets a saver eventually, but normal frame-to-frame
// gaps (even a stalled load) never let it in.
#define SS_FS_PRESENT_GRACE_MS  5000ULL
// #570: grace window after activation during which wake-on-input is ignored.
// Without this, the button-UP (and any stray motion) of the very click that
// activates the saver (Settings "Test") dismisses it before it is even
// visible on screen.
#define SS_ACTIVATE_GRACE_MS 500

// Font
// #uiscale: FONT_CHAR_W/H have TWO different jobs in this tree, and only one
// of them may scale.
//   (1) THE RAW 8x16 BITMAP GLYPH DIMENSIONS - draw_char()/draw_text_bitmap()/
//       draw_char_large() in draw.c index font_data[128][16] and advance the
//       cursor by these values one-for-one against that fixed-size table.
//       Scaling THAT use would read past the end of font_data. draw.c uses
//       the _RAW forms below for exactly this reason.
//   (2) A GENERAL "TEXT CELL" CONSTANT that ~55 call sites across
//       iconpicker.c/startmenu.c/taskbar.c/wallpaper.c/widgets.c/stickies.c
//       use to vertically center or lay out TTF-rendered text (draw_text()/
//       draw_text_centered()) against an assumed row height - none of those
//       sites touch the bitmap font at all. THIS is the meaning everywhere
//       outside draw.c's own glyph rendering, and it must scale with the UI
//       scale factor or every one of those labels drifts off-center as text
//       grows. ui_px() independently on W and H is fine here: this is a
//       single cell size, not two rects that must share an edge.
#define FONT_CHAR_W_RAW     8
#define FONT_CHAR_H_RAW     16
#define FONT_CHAR_W         ui_px(FONT_CHAR_W_RAW)
#define FONT_CHAR_H         ui_px(FONT_CHAR_H_RAW)

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
    ICON_WIFI_OFF,  // #159 sect 6b: wifi bars + slash, for the desktop-widget
                     // offline component. NOT ICON_NETWORK - that id points at
                     // icon_data_info_circle (icons.c), a mislabeled entry.
    ICON_SLIDERS,   // tray audio/quick-settings glyph (color-icon only)
    ICON_CHEVD,     // start-menu expanded indicator
    ICON_CHEVR,     // start-menu collapsed indicator
    ICON_WIN3X,     // Win16/Win3.x program glyph (color-icon only) (#208)
    ICON_DOSAPP,    // MS-DOS program glyph (color-icon only) (#208)
    // #562: per-app icons so no app is left showing a generic/borrowed glyph.
    ICON_GAME_ARENA,     // Maytera Arena (color-icon only)
    ICON_GAME_CHESS,     // Maytera Chess (color-icon only)
    ICON_GAME_SQUADRON,  // Maytera Squadron (color-icon only)
    ICON_GAME_GLCUBE,    // GL Cube demo (color-icon only)
    ICON_GAME_GLMATRIX,  // GL Matrix demo (color-icon only)
    ICON_AICHAT,         // AI Chat (color-icon only)
    ICON_WEATHER,        // Weather (color-icon only)
    ICON_FEEDS,          // Feeds/RSS (color-icon only)
    ICON_GALLERY,        // Gallery (color-icon only)
    ICON_SNAPSHOT,       // Snapshot (color-icon only)
    ICON_NOTES,          // Notes (color-icon only)
    ICON_FONTBOOK,       // Font Book (color-icon only)
    ICON_CONVERTER,      // Converter (color-icon only)
    ICON_TIMERS,         // Timers (color-icon only)
    ICON_PYTHON,         // Python (color-icon only)
    ICON_AUTH,           // Authenticator (color-icon only)
    ICON_HELP,           // Help (color-icon only)
    ICON_LAUNCHER,       // Launcher (color-icon only)
    ICON_TASKSWITCH,     // Task Switcher (color-icon only)
    ICON_APPSTORE,       // App Store (color-icon only)
    ICON_SYSMON,         // System Monitor (color-icon only)
    ICON_SERVICES,       // Services (color-icon only)
    ICON_3DPRINT,        // 3D Print (color-icon only)
    // #723 Home Assistant entity-state glyphs (monochrome, tinted per state -
    // see ha_format.c). Deliberately NOT color-icons: state semantics need a
    // theme/semantic tint (success/warning/error/muted), which only works on
    // the 1bpp path (icon_draw's `color` argument), not the fixed-palette
    // color-icon blit.
    ICON_HA_SUN,          // sun.state == above_horizon; weather sunny
    ICON_HA_MOON,         // sun.state == below_horizon; weather clear-night
    ICON_HA_DOOR_OPEN,    // cover/binary_sensor door-like, open
    ICON_HA_DOOR_CLOSED,  // cover/binary_sensor door-like, closed
    ICON_HA_LOCK_LOCKED,  // lock domain locked; alarm armed
    ICON_HA_LOCK_UNLOCKED,// lock domain unlocked; alarm disarmed
    ICON_HA_BULB,         // light domain (tinted lit/unlit)
    ICON_HA_MOTION,       // binary_sensor motion/occupancy/vibration, detected
    ICON_HA_THERMOMETER,  // sensor/number temperature; climate
    ICON_HA_DROPLET,      // sensor humidity; binary_sensor moisture; valve water
    ICON_HA_BOLT,         // sensor power/energy/current/voltage; switch on
    ICON_HA_BATTERY,      // sensor/binary_sensor battery
    ICON_HA_WARN,         // problem/smoke/gas/tamper/safety alert; unavailable
    ICON_HA_CHECK,        // problem-class clear; person/tracker home; connected
    // #250: hot-plugged USB volume. APPENDED, never inserted: these values are
    // persisted numerically (UIPROFIL.YML screensaver type, desktop icon keys),
    // so inserting one renumbers what is already on disk.
    ICON_USBDRIVE,
    // #234i: a mounted disk image is a removable volume too, and a CD drawn as
    // a USB stick is a lie the user has to click to discover.
    ICON_CDROM,
    ICON_FLOPPY,
    ICON_COUNT
} icon_id_t;

// ============================================================================
// Structures
// ============================================================================

// Desktop icon kinds (#745). SYSTEM covers the compiled-in set AND anything
// added this session via the Start menu's "Add to Desktop"; the other three
// come from enumerating <home>/DESKTOP and each has its OWN activation rule
// (see activate_icon() in desktop.c). There is no "no-op" kind on purpose:
// every icon that renders does something when opened.
#define DESK_KIND_SYSTEM 0   // compiled-in / session-added app shortcut
#define DESK_KIND_DIR    1   // a directory in <home>/DESKTOP
#define DESK_KIND_EXEC   2   // an executable file in <home>/DESKTOP
#define DESK_KIND_FILE   3   // any other file in <home>/DESKTOP
// #250: a hot-plugged removable volume. NOT a file and NOT a system shortcut:
// it appears and disappears with the physical device, so it is rebuilt from
// the kernel's volume list on every desktop rescan rather than persisted.
// Opening it opens Files at its mount point; right-clicking it offers Eject.
#define DESK_KIND_VOLUME 4

// Desktop icon
typedef struct {
    char name[DESKTOP_ICON_NAME_LEN];
    char exec_path[128];
    // (#745) STABLE persistence key. Positions used to be stored as
    // "ico<N>x"/"ico<N>y", keyed by the icon's INDEX in the list. That is only
    // safe while the list is a fixed compiled-in array: the moment icons come
    // from a directory, adding or deleting one file renumbers every icon after
    // it and every saved coordinate lands on the wrong icon. The key is derived
    // from IDENTITY instead ('s' + app basename for a system icon, 'u' + the
    // file name + a hash of the full name for a user one), so a position
    // follows its icon regardless of what else appears or disappears.
    char key[20];
    unsigned char kind;    // DESK_KIND_*
    // (#745) 1 once this icon's position is AUTHORITATIVE: either restored from
    // the profile by key, or assigned a collision-free slot. It exists because
    // placement and restore cannot be done in one pass. On boot the order is
    // scan -> default grid -> profile restore, and during a live rescan it is
    // add -> restore-from-previous; in BOTH, an icon with no saved position is
    // positioned BEFORE some other icon's saved position has been applied, so a
    // "is this cell free?" test asked at that moment gets a stale answer. This
    // was not theory: on the verification VM a newly created AA0.TXT was placed
    // exactly on top of AAA.TXT's restored slot, hiding one icon completely.
    // Unplaced icons are therefore swept up in a SECOND pass, once every
    // authoritative position is known.
    unsigned char pos_set;
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
    // (#845) Only meaningful when launch_type==LAUNCH_WIN16: -1=auto (derive
    // from the NE header), 0=force real, 1=force protected. Set from the
    // .MENU fragment's optional "mode=" field (startmenu_model.rs); items
    // from other sources (the legacy /WIN16GRP.CFG loader) default to auto.
    int  win16_mode;
} menu_item_t;

// #26 XFCE-style dock: a favorite/pinned app descriptor returned by
// startmenu_get_favorites(). The dock reuses the Start Menu's existing
// Favorites list (/CONFIG/STARTMENU.CFG) as its "pinned apps" row instead of
// inventing a second, parallel pin file - one pin set, editable from either
// surface. See docs/DESKTOP_SHELL_RESEARCH.md 4.2/5.2 and
// docs/DOCK_XFCE_MOCKUP.html.
typedef struct {
    char       name[48];
    char       exec_path[128];
    icon_id_t  icon_id;
    int        launch_type;   // 0=native,1=win16,2=dos - matches menu_item_t
} sm_fav_info_t;

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
    SS_GLCUBE,        /* #319 TinyGL spinning textured cube (reconciled #336) */
    SS_GLMATRIX,      /* #319 TinyGL 3D matrix code rain (reconciled #336) */
    /* #560: ten psychedelic/geometric TinyGL screensavers. Keep this block
       contiguous and last, immediately after SS_GLMATRIX, and keep SS_GLLAVA
       as the final enumerator: screensaver.c's ss_is_gl_type() and
       screensaver_set_type()'s bound check both rely on that. */
    SS_GLTUNNEL,      /* rainbow ring tunnel flythrough */
    SS_GLKALEIDO,     /* mirrored rotating kaleidoscope, 8-fold symmetry */
    SS_GLPLATONIC,    /* tetra/cube/octa/icosahedron morph cycle */
    SS_GLLORENZ,      /* Lorenz strange attractor, rainbow trail */
    SS_GLMOBIUS,      /* twisting Mobius strip, hue along its length */
    SS_GLWAVEMESH,    /* grid deformed by interfering sine waves */
    SS_GLSPIROGRAPH,  /* 3D harmonograph / epitrochoid sweep */
    SS_GLHYPERCUBE,   /* rotating tesseract (4D->3D->2D projection) */
    SS_GLVORTEX,      /* swirling particle vortex, point sprites */
    SS_GLLAVA,        /* drifting low-poly lava blobs (additive, not true metaballs) */
    /* Psychedelic redesign lead trio (docs/SCREENSAVER_PSYCHEDELIC_DESIGN.md).
       New IDs, deliberately >= 20 and OUTSIDE the SS_GLCUBE..SS_GLLAVA range:
       both are direct-pixel (screensaver_gfx.c pipeline), NOT TinyGL, so
       ss_is_gl_type() and the #560 GL crash-gate in screensaver_set_type()
       (which only checks SS_GLTUNNEL..SS_GLLAVA) correctly leave them alone. */
    SS_FLAME,          /* §4.1 Fractal Flame "Bloom Garden": IFS chaos-game histogram */
    SS_STAINEDGLASS,   /* §4.6 Stained-Glass Warp: Voronoi cells, hard ink edges */
    /* #124: the ORIGINAL plasma (#282), restored ALONGSIDE Plasma Reborn
       rather than instead of it. ff3ca5f replaced SS_PLASMA in place, so the
       classic look (blocky step=4 blocks straight to the framebuffer, indexed
       through the raw HSV wheel, no radial term, no bloom, no palette
       cycling) had disappeared from the product entirely. Both now ship.
       Same rule as the trio above: id >= 20 and OUTSIDE the contiguous
       SS_GLTUNNEL..SS_GLLAVA block, because this is direct-pixel, not TinyGL,
       so ss_is_gl_type() and the GL range checks correctly ignore it.
       APPENDED, never inserted: these ids are persisted numerically as
       screensaver:<n> in UIPROFIL.YML, so renumbering would silently change
       what every existing profile selects. */
    SS_PLASMACLASSIC   /* #282 original plasma: 4px blocks, raw HSV wheel */
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

// (#73) login_state_t deleted with login.c - it had no other user. The
// kernel login gate has its own, unrelated, enum of the same name.

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
// #745: horizontal run with per-pixel alpha from a table (astep = +1 / -1 / 0).
void draw_hspan_alpha(int32_t x, int32_t y, int32_t w,
                      uint32_t color, const uint8_t *alpha, int astep);
void launcher_open(void);
void launcher_close(void);
void launcher_toggle(void);
int  launcher_is_open(void);
void launcher_render(void);
int  launcher_handle_key(int key);
int  launcher_handle_mouse(int x, int y, int clicked);

// ============================================================================
// #745 (task #68) THE MODAL KEYBOARD GRAB. Defined in main.c; the ONE answer
// to "is a compositor surface owning the keyboard right now".
//
// Returns non-zero if some open overlay (lock screen, elevation prompt,
// screensaver, AI launcher, start menu + its dialogs, icon picker, widget
// settings, sticky-note editor, ...) claims THIS key. Every path that forwards
// a keystroke to an app window - process_input()'s hardware path AND vnc.c's
// independent RFB path - must consult it before calling sys_inject_key(), or
// the user's typing is delivered to two places at once.
//
// Do NOT add a second `&& !g_foo_open` at a call site: add a row to
// g_modal_grabs[] in main.c, which is also what routes the key to the surface.
// ============================================================================
int modal_key_grab(int key);

// Login state
extern bool g_logged_in;
extern int  g_login_uid;
extern char g_login_username[64];

// #566 secure session lock. The KERNEL (SYS_SESSION_IS_LOCKED) is the sole
// authority for whether the session is locked; g_session_locked is a DISPLAY
// CACHE refreshed every frame by lock_poll(), never the decision itself. See
// lockscreen.c.
extern bool g_session_locked;

// Screensaver state
extern bool g_screensaver_active;
extern uint64_t g_idle_ms;   // monotonic uptime_ms() of last user input

// Redraw flag
extern bool g_needs_redraw;

// #162 volosd.c - the volume OSD. FEEDBACK ONLY: the media keys are a KERNEL
// global hotkey (cpu/isr.c), so by the time this file hears anything the
// volume has already changed. volosd_tick() is one SYS_VOL_STATE syscall per
// loop; volosd_render() must be called from BOTH render paths, including the
// exclusive lock-screen branch, because #162 covers the lock screen.
void volosd_tick(void);
void volosd_render(void);
int  volosd_visible(void);

// ============================================================================
// Module APIs (each module exposes init/render/handle functions)
// ============================================================================

// draw.c - Dirty-rectangle compositing (#102/#379).
// Global clip rectangle (exclusive x1/y1) that bounds every primitive so the
// idle compositor recomposites only changed regions. Default = full screen.
extern int g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1;
void draw_set_clip(int x, int y, int w, int h);   // REPLACES the active clip
void draw_clear_clip(void);                        // reset to full screen
// (#745) draw_set_clip()/draw_clear_clip() are a single global with no stack.
// A widget that wants a temporary sub-clip (e.g. the wifi tray glyph clipping
// its AA rings to their upper half) cannot safely call draw_clear_clip() when
// it is finished: if it was invoked from inside render_frame_idle()'s
// per-damage-rect loop, that call resets the clip to the WHOLE SCREEN and
// every draw after it for the rest of that damage rect is no longer confined
// - silently defeating #379 for that frame. draw_push_clip() INTERSECTS with
// whatever clip is already active and saves it; draw_pop_clip() restores
// exactly that. Depth 4, matching the deepest nesting anywhere in this file
// (damage-rect > taskbar > tray-icon > AA-ring-clip).
void draw_push_clip(int x, int y, int w, int h);
void draw_pop_clip(void);
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
// #745: coverage-based (antialiased) circle primitives, next to the plain
// midpoint ones above which stay in use for small hand-authored pixel art
// (taskbar glyphs, tray knob, widget icons) where a crisp edge is intended.
// See docs/LOGIN_AVATARS_AND_PROFILE.html section 11 for the coverage rule.
// r/stroke_w are float so a stroke can move from 2px to 3px (selected ring)
// without a second function. alpha is 0..255, composited via the existing
// draw_hspan_alpha()/draw_blend() path (honors the active clip).
void draw_circle_filled_aa(int32_t cx, int32_t cy, float r, uint32_t color, int alpha);
void draw_circle_ring_aa(int32_t cx, int32_t cy, float r, float stroke_w, uint32_t color, int alpha);
// (#745 follow-up) Antialiased line segment, same coverage-based family as
// the two circle primitives above. `width` is the full stroke width in px.
void draw_line_aa(int32_t x0, int32_t y0, int32_t x1, int32_t y1, float width, uint32_t color, int alpha);
void draw_gradient_v(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t top, uint32_t bot);
void draw_char(int32_t x, int32_t y, char c, uint32_t color);
void draw_text(int32_t x, int32_t y, const char *text, uint32_t color);
uint32_t draw_luminance(uint32_t c);
uint32_t readable_ink(uint32_t bg);
uint32_t readable_ink_dim(uint32_t bg);
// (#745) The same muted ink with an explicit mix percentage. 35 is the opaque
// default kept by readable_ink_dim(); glass surfaces use GLASS_DIM_MIX (22).
uint32_t readable_ink_dim_mix(uint32_t bg, int mix);
// (readable_accent() is declared further below, next to its other chrome-
// helper neighbors - kept legible as a small accent bar/icon/dot, NOT for use
// as a filled button surface under fixed-color text; see
// draw_popup_button_centered() below for that case.)

// #127/#128: the ONE shared tray/system popup panel (rounded CLR_MENU_BG body,
// soft drop shadow, CLR_MENU_BORDER hairline, 1px top highlight) - every popup
// hung off the tray (notifications center, quick-control popups) must use
// this instead of a bespoke rect+border combo. See draw.c for the full
// rationale and docs/TRAY_POPUP_DESIGN_LANGUAGE.md for the spec this ports.
void draw_popup_panel(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius);
// A neutral (non-accent-colored) popup action button with centered TTF text,
// contrast-safe on all 14 shipping themes by construction (both fill and ink
// derive from CLR_MENU_BG). Use this for any "Clear all"/"Dismiss"/plain
// action button in a tray popup - never a raw severity/accent color fill
// under fixed white text (measured 3.68:1, fails WCAG AA on every theme).
void draw_popup_button_centered(int32_t x, int32_t y, int32_t w, int32_t h,
                                int32_t radius, const char *label, int ttf_size);

// ============================================================================
// (#745) GLASS chrome: translucent surfaces with a blurred backdrop.
// docs/DOCK_XFCE_GLASS.html is the authority. Implementation in draw.c.
// ============================================================================
#define GLASS_SURF_PANEL   0     // default taskbar / XFCE top panel / Lumina bar
#define GLASS_SURF_DOCK    1     // XFCE bottom dock
#define GLASS_SURF_MENU    2     // start menu root panel
// (#glassmodal) 4th slot: the confirm/shutdown/log-off modal card AND the
// CPU/RAM/DSK/NET perf pop-out both use this ONE slot. Safe to share because
// the two are never open at the same time in practice (the confirm dialog is
// a true modal - see confirmdialog.c) and glass_render()'s own cache stores
// only ONE geometry/tint per slot, so two genuinely-simultaneous callers
// would just alternate cold recomputes rather than corrupt anything.
#define GLASS_SURF_MODAL   3     // confirm/shutdown modal card + perf pop-out
#define GLASS_SURF_COUNT   4

// Composite a glass surface: blurred backdrop, then `tint` at the user's
// dock_opacity, into the given rect. Honours the clip, caches per surface.
void glass_render(int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t tint, int surf);
void glass_bump_epoch(void);
void glass_invalidate_all(void);
int  glass_perf_get(int surf, uint32_t *cold_n, uint32_t *cold_ms,
                    uint32_t *cold_worst, uint32_t *hit_n, int *tier);

// (#glassmodal) Shared glass/chrome helpers, promoted from taskbar.c
// (previously static, ~lines 130-195 there) so confirmdialog.c and
// draw_perf_popup() can reuse the SAME edge/highlight/opacity-floor logic
// instead of each hand-rolling a third copy (CLAUDE.md: improve the shared
// primitive, do not fork). Definitions live in draw.c, next to
// glass_render(). No behavior change at any existing taskbar.c call site.
int      flat_chrome_alpha(void);
void     glass_or_flat(int32_t x, int32_t y, int32_t w, int32_t h, int surf);
void     glass_edge_h(int32_t x, int32_t y, int32_t w, uint32_t c);
void     glass_edge_v(int32_t x, int32_t y, int32_t h, uint32_t c);
void     glass_highlight_h(int32_t x, int32_t y, int32_t w);

// (#glassmodal) Generalized AA corner rounding for a FLOATING rect with an
// arbitrary corner mask. The dock (taskbar.c: xfce_dock_paint_rounded() /
// xfce_corner_coverage()) already proved this technique for a rect flush to
// the screen edge, which only ever needs its top two corners rounded; a
// floating modal or pop-out needs all four. Contract for callers:
//   1. capture() BEFORE painting anything, so the captured pixels are the
//      real backdrop (desktop, scrim, whatever is genuinely behind this
//      panel), not something this panel already drew;
//   2. paint the panel as a plain square rect (glass or flat);
//   3. draw ALL interior content, including any footer/title band drawn
//      after the main fill;
//   4. restore() LAST, after every other draw call for this panel. Restoring
//      last is what lets a footer band (painted after the main panel fill)
//      still get correctly rounded corners at the panel's own outer radius,
//      instead of being squared back off by a footer that was drawn UNDER an
//      earlier restore.
// corner_capture_t is cheap (4 * 24 * 24 * 4 bytes ~= 9KB) - use it as a
// stack-local at each call site, not a global; it is transient per-render.
#define CORNER_TL 1
#define CORNER_TR 2
#define CORNER_BL 4
#define CORNER_BR 8
#define CORNER_ALL (CORNER_TL|CORNER_TR|CORNER_BL|CORNER_BR)
#define CORNER_CAP_MAXR 24

typedef struct {
    uint32_t px[4][CORNER_CAP_MAXR][CORNER_CAP_MAXR];
    int32_t  x, y, w, h, r;
    int      mask;
} corner_capture_t;

void draw_round_corners_capture(corner_capture_t *cap, int32_t x, int32_t y,
                                 int32_t w, int32_t h, int32_t r, int mask);
void draw_round_corners_restore(const corner_capture_t *cap);

// The user preference, percent OPAQUE (60..100, default 90). ONE preference
// covering the default taskbar, the XFCE panel and dock, the Lumina bar and
// the start menu. Deliberately NOT a theme key: opacity is the user's, so a
// theme switch never overwrites a number the user set.
extern int g_dock_opacity;

// 1 ONLY inside a full render_frame() body. The blur may read g_fb only then;
// in a clipped pass the region under a chrome surface is stale or half-written
// and blurring it would feed chrome back into the glass.
extern int g_glass_live;

// Measured-milliseconds threshold for the tier-2 auto-downgrade (default 4).
extern int g_glass_downgrade_ms;
extern uint64_t g_backdrop_epoch;

// Per-theme glass material, resolved from the active .mtheme by
// glass_theme_apply() in taskbar.c. glass_tint is the surface colour.
extern int      g_glass_enable;      // 0 = tier 4, today's opaque chrome
extern uint32_t CLR_GLASS_TINT;
// (#745 taskbar/tray) CLR_CHIP_REST/HOVER/OPEN/BORDER/BEVEL/INK REMOVED. They
// were the ASK-2 opaque launcher-chip plate (0xE4E7EB fixed on every theme,
// see blame.md); the plate is gone (chrome_chip() now paints ink straight on
// CLR_TASKBAR_BG like every other tray icon, no fill at rest) and grep
// confirmed zero other callers. Two .mtheme files (maytera_dark/light) still
// carry harmless chrome_chip_* override lines from that era; nothing reads
// them now.
void     glass_theme_apply(void);
// The effective background of chrome text: the glass tint when glass is on,
// CLR_TASKBAR_BG otherwise. Contrast must be measured against what the eye
// receives, which on glass is the tint, not the opaque token.
uint32_t chrome_surface_bg(void);
// Muted chrome ink at the right mix for the active material (22% on glass,
// 35% opaque).
uint32_t chrome_ink_dim(void);
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

// #148 (local 164, 2026-08-18): PrintScreen hotkey. ALWAYS a fire-and-forget
// full-screen capture (owner clarification: drag/region-select is an in-app
// Snapshot mode only, never reached from this hotkey). Full-resolution
// capture to <home>/SCREENSHOTS, timestamped filename. Two paths after the
// capture, both in screenshot.c:
//   A. Native-fullscreen (#158) on top: STAYS fullscreen, shows the
//      bottom-right thumbnail toast (screenshot_fs_toast_draw() below).
//   B. Otherwise: opens Maytera Snap on the capture, bottom-left, without
//      stealing focus (win_create_bg()).
void screenshot_hotkey_fire(void);

// #148 (local 164): the fullscreen-only capture toast. main.c's
// native_fullscreen_try_render() calls screenshot_fs_toast_draw() (which
// itself consults screenshot_fs_toast_active()) every tick it successfully
// re-blits the app's frame, AFTER that blit and BEFORE cursor_render(); the
// scheduling block folds screenshot_fs_toast_active() into its "force a full
// render tick" predicate so the dwell always gets a real frame to end on even
// against a perfectly static fullscreen app. See screenshot.c's toast module
// comment for the full contract (why this cannot go through notif.c, and why
// no separate erase/restore step is needed).
int  screenshot_fs_toast_active(void);
void screenshot_fs_toast_draw(void);

// Live remote view + control over RFB/VNC (vnc.c, #440). vnc_poll() runs once
// per frame (reverse-connects to a listening viewer per /CONFIG/VNC.CFG - see
// the comment at the top of vnc.c for why the compositor is the RFB server but
// the *client* side of the TCP connect). The two dirty-tracking hooks let the
// main loop tell the RFB layer which screen regions changed this frame, reusing
// the same #379 damage-rectangle machinery screenshot.c/widgets.c already use.
void vnc_poll(void);
void vnc_mark_full_dirty(void);
void vnc_mark_rect_dirty(int x, int y, int w, int h);

#ifdef MAYTERA_TESTHOOK
// #334 headless GUI verification hook (testhook.c). NOT part of any normal
// build: only exists when compiled with `make TESTHOOK=1` (see the
// Makefile), which is never how the shipping COMPOSIT is built - so this
// prototype, testhook.c's object file, and every symbol it defines are
// simply ABSENT from a normal binary, not just runtime-disabled. Polls the
// flat file /TESTHOOK.CMD once per frame (same no-busy-wait pattern as
// screenshot_poll()/vnc_poll()) for one NAME-driven command per request
// (LAUNCH/SAVER/STARTMENU/ICON/MENUITEM - see testhook.c for the exact
// grammar) and appends a result line to /TESTHOOK.OUT. Driving the UI by
// NAME sidesteps the pixel-calibration problem entirely, at the cost of
// proving the app's logic rather than the real click/hit-test path - see
// the #334 CHANGELOG entry for the tradeoff this was deliberately chosen
// to accept.
void testhook_poll(void);
#endif

// icons.c - Icon rendering
void icon_draw(icon_id_t id, int32_t x, int32_t y, uint32_t color);
int  icon_color_count(void);
int  icon_load_color(icon_id_t id, const char *path);
int  icon_draw_color_if_present(icon_id_t id, int32_t x, int32_t y, int32_t size);
int  icon_draw_color_tinted(icon_id_t id, int32_t x, int32_t y, int32_t size, uint32_t ink);
// #63/#745: dock-only variant of icon_draw_color_tinted() that also draws
// the same drop shadow icon_draw_color_if_present() gives raw-color icons.
// See icons.c for the full rationale.
int  icon_draw_dock_icon(icon_id_t id, int32_t x, int32_t y, int32_t size, uint32_t ink);
void icon_draw_scaled(icon_id_t id, int32_t x, int32_t y, int32_t size, uint32_t color);
// #44 custom dock icons (PNG/JPG/BMP import). See icons.c for the format
// notes and the alpha-forcing rationale.
void icon_mico_basename(const char *exec_path, char *out, int cap);
int  icon_import_and_apply(icon_id_t id, const char *src_path, const char *out_mico_path);

// (#73) login.c is DELETED. The compositor's own login screen was a superseded
// duplicate of the kernel login gate (kernel/gui/login.c) that was never drawn
// and never entered, but was still called on every keystroke and still built a
// plaintext password buffer out of them. Its five entry points (login_init,
// login_run, login_render, login_handle_key, login_handle_mouse) are gone.
// Its still-used drawing primitives moved to draw.c.
//
// Shared login/lock-screen drawing primitives (now in draw.c, reused by
// lockscreen.c and elevate.c, #566 - "reuse, never reinvent" rather than a
// second copy of avatar/password-field rendering).
void draw_bullet(int32_t cx, int32_t cy);

// #745: the SHIPPED password-field primitive. It was static to lockscreen.c;
// the elevation modal (elevate.c) reuses it VERBATIM rather than forking a
// private copy, per the shared-primitive rule in CLAUDE.md. Radius h/2, the
// draw_bullet() run at a 12px pitch, the 2px caret, and the #745 boundary pair
// all come with it, so the two credential surfaces in this OS cannot drift.
void lock_draw_pill(int x, int y, int w, int h, const char *text,
                    int masked, int focused, int caret, int err,
                    const char *placeholder, int arrow_sz);

// #745 THE ELEVATION MODAL (elevate.c). Compositor-owned, system-modal, and the
// only surface in the OS that may ask for a password on an app's behalf. See
// the header comment in elevate.c for why it cannot live in the App Store.
void elevate_poll(void);            // once per frame: mirror the kernel + watchdog
void elevate_render(void);          // scrim + panel, drawn above everything
int  elevate_modal_open(void);      // 1 while it owns the screen
int  elevate_handle_key(int key);   // 1 if consumed (it consumes everything)
int  elevate_handle_mouse(int32_t x, int32_t y, int clicked);
// (#73) draw_password_field() deleted: zero callers (both credential surfaces
// use lock_draw_pill()) and it was the dead login layer's own password field.
void draw_button(int32_t x, int32_t y, int32_t w, int32_t h, const char *label, uint32_t bg);
// #745: state replaces the old bool "highlight" (AVATAR_ST_* above). uid
// selects the decorative identity dot color (Settings' avatar_palette,
// keyed by uid so it is stable across account-list reordering - see
// docs/LOGIN_AVATARS_AND_PROFILE.html section 8.2).
void draw_avatar(int32_t cx, int32_t cy, const char *username, unsigned int uid, int state);
// lockscreen.c - Secure session lock overlay (#566, docs/SECURE_LOGIN_DESIGN.md
// section 3.2/4.2). A TRUE modal, stronger than the launcher/widget-settings
// modals it copies the pattern from: while g_session_locked, main.c's
// process_input()/process_events() route ALL key/mouse/scroll events to
// lock_handle_key()/lock_handle_mouse() ONLY - no sys_inject_key/mouse to any
// app window, no desktop/taskbar/start-menu hit-testing. Closes only via a
// correct sys_session_unlock() (never ESC, never click-away).
void lock_init(void);                 // once, at compositor_init()
void lock_enter(void);                // explicit request: Start Menu, Super+L
void lock_enter_reason(int reason);   // #745: SESSION_LOCK_IDLE for the idle timer
// #745: establish g_login_uid / g_login_username from the identity the KERNEL
// gave this process, replacing the hardcoded root. Called once at startup.
void session_identity_init(void);
void lock_poll(void);                 // once per main-loop iteration (cheap, no blocking)
void lock_render(void);
int  lock_handle_key(int key);
int  lock_handle_mouse(int32_t x, int32_t y, bool clicked);

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

// --- cross-window drag ("docking"), dragghost.c ----------------------------
// The drag ghost that follows the cursor between windows. Only the compositor
// can draw it: the source app cannot put a pixel outside its own window, which
// is the entire reason this lives here rather than in the terminal.
//
// INERT WHEN UNUSED: dragghost_poll() makes NO syscall unless a mouse button is
// physically held, and dragghost_render()/dragghost_active() return on a static
// int. There is no timer, no thread and no allocation behind any of these.
int  dragghost_active(void);
void dragghost_poll(int mx, int my, unsigned int buttons);
void dragghost_release(int x, int y);
void dragghost_render(void);
// Right-click context-menu actions (wired from contextmenu.c).
void desktop_auto_arrange(void);                 // re-flow all icons to default top grid
void desktop_align_to_grid(void);                // snap current positions to the grid
// Start-menu "Add to Desktop" (#: right-click item -> Add to Desktop). Appends
// a new desktop icon this session if there is room and it is not already
// present (matched by exec_path). Returns false if DESKTOP_ICON_MAX is full or
// the path is already on the desktop. NOTE: unlike icon POSITIONS (persisted
// by profile.c), the icon SET itself is not yet persisted across reboot - a
// session-added icon does not survive a restart. See CHANGELOG for this as a
// known follow-on, not silently faked.
bool desktop_add_icon(const char *name, const char *path, icon_id_t icon);
// (#745) Create an entry in <home>/DESKTOP. These back the context menu's
// "New Folder" / "New File", which were dead stubs until the desktop had a
// real backing directory. Both post an error notification if the folder is not
// writable, so a failure is visible rather than silent.
void desktop_new_folder(void);
void desktop_new_file(void);
// (#745) Re-enumerate <home>/DESKTOP. force=0 returns immediately when the
// directory listing is unchanged since the last scan. Called from the input
// tick on a throttle, and from "Refresh" with force=1. NEVER call this from a
// render path: it does file I/O (#426).
void desktop_rescan_home(int force);
void desktop_home_tick(void);                    // throttled caller of the above
// #250 removable volumes. desktop_volumes_tick() is the polled entry point,
// called once per compositor loop and throttled internally; it is ONE syscall
// in the unchanged case and triggers a rescan only when the set of plugged-in
// drives actually changes. Like desktop_home_tick() it runs from the input
// tick and NEVER from a render path.
void desktop_volumes_tick(void);
// Volume behind desktop icon `idx`, or -1. Used by the context menu to offer
// Eject on the right icon.
int  desktop_icon_volume(int idx, int *out_kernel_index, int *out_readable);
// Eject the volume behind desktop icon `idx`. Returns 1 on success.
int  desktop_icon_eject(int idx);
// Open the volume behind desktop icon `idx` in Files.
void desktop_icon_open_volume(int idx);
// Which desktop icon is at (x, y), or -1. Exposed so the right-click path can
// offer an icon-specific menu instead of the desktop background one.
int  desktop_icon_at(int32_t x, int32_t y);

// Persistence hooks used by profile.c.
int  desktop_icon_count(void);
void desktop_get_icon_pos(int idx, int32_t *x, int32_t *y);
void desktop_set_icon_pos(int idx, int32_t x, int32_t y);
// (#231) desktop_positions_hash() removed: profile_tick()'s change-detection
// now hashes profile_build()'s actual serialized bytes (which already carry
// every icon's key/kind/position) instead of a hand-maintained parallel hash.
// (#745) Number of leading icons that are NOT sourced from <home>/DESKTOP.
// profile.c uses it to bound the LEGACY index-keyed "ico<N>" restore to
// exactly the set those keys were written for.
int  desktop_builtin_count(void);
// (#745) The icon's stable persistence key. Returns 0 on success.
int  desktop_icon_key(int idx, char *out, int cap);
// (#745) Restore a position by stable key. axis: 0 = x, 1 = y. A key that
// matches no current icon is ignored, which is what makes a saved position for
// a file the user has since deleted harmless.
void desktop_set_icon_pos_by_key(const char *key, int axis, int v);
// (#745) SECOND PASS: give every icon that still has no authoritative position
// a collision-free grid slot. Call after all restores are done (profile.c calls
// it at the end of profile_load(); the rescan calls it itself). Idempotent.
void desktop_place_unplaced(void);
#ifdef MAYTERA_TESTHOOK
// #334 headless verification hook ONLY (see testhook.c). Launches the icon
// whose label matches `name` exactly, calling the same launch_app() a real
// click calls - but by NAME, not by hit-testing a pixel, so it sidesteps
// the QEMU-mouse-calibration problem entirely. Returns true if a matching
// icon was found (whether or not the launch itself succeeds).
bool desktop_launch_icon_by_name(const char *name);
#endif

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
#ifdef MAYTERA_TESTHOOK
// (#glassmodal) headless verification hook ONLY (see testhook.c). Opens the
// CPU/RAM/DSK/NET perf pop-out for the given gauge index (0=CPU..3=NET),
// calling the same code path a real gauge click uses - sidesteps the
// gauge's mouse hit-test (#334/#440).
void taskbar_test_open_perf_popup(int gauge);
#endif

// Per-app taskbar-tile right-click menu (Close). A right-click on a running
// app's taskbar button was previously silently swallowed by
// taskbar_handle_mouse() (it consumes every click inside the taskbar strip
// unconditionally) with no visible effect - no in-app context menu equivalent
// existed for the taskbar itself. See taskbar.c for the menu implementation.
bool taskbar_handle_right_click(int32_t x, int32_t y);
bool taskbar_menu_is_open(void);
bool taskbar_menu_handle(int32_t x, int32_t y, int click);
void taskbar_menu_render(void);
// #44: exported for contextmenu.c's CTX_MODE_DOCK dispatch (Close synthesizes
// a click on the real close button - see taskbar.c for why; Force Quit finds
// a pid by matching SYS_PROC_LIST's name against a window's app_id, since
// wm_window_info_t carries no pid, and does nothing if no match is found).
void taskbar_close_window(int32_t win_id);
void taskbar_force_quit_app_id(const char *app_id);
// #745 (docs/CONFIRM_MODAL_DESIGN.html): Force Quit is now gated behind a
// confirm dialog (see taskbar.c) instead of firing SIGKILL immediately.
bool taskbar_force_quit_confirm_open(void);
void taskbar_force_quit_confirm_render(void);
int  taskbar_force_quit_confirm_handle_key(int key);
bool taskbar_force_quit_confirm_handle_mouse(int32_t x, int32_t y, bool clicked);
int  taskbar_dock_animating(void);   // #745: DOCK_XFCE hover ease in flight this frame?

// #129: named indices into apps/settings/main.c's PANEL_* enum, so every
// compositor call site that wants a specific Settings panel (tray icon deep
// links, the desktop "Personalize" menu, the Start-menu gear, the digital
// clock widget) names it instead of repeating a bare "case 14" a reader has
// to cross-reference against settings/main.c by hand. MUST stay in sync with
// that enum (settings/main.c has its own "keep in sync" comment at its one
// pre-existing raw-number call site, startmenu.c's PANEL_STARTMENU).
#define SETTINGS_PANEL_APPEARANCE  0
#define SETTINGS_PANEL_DISPLAY     1
#define SETTINGS_PANEL_NETWORK     3
#define SETTINGS_PANEL_DATETIME    6
#define SETTINGS_PANEL_BLUETOOTH   14
#define SETTINGS_PANEL_WIFI        15
#define SETTINGS_PANEL_STARTMENU   17
// #129: THE one way to open Settings on a specific panel. Every previous call
// site (contextmenu.c x2, traymenu.c, startmenu.c) did a blind
// set_settings_tab()+sys_spawn(), which spawns a DUPLICATE Settings window
// every time one is already open - sys_spawn() has no singleton check, and
// nothing else provided one either. This focuses (and thereby un-minimizes)
// an existing Settings window instead of spawning a second one; Settings
// already polls get_settings_tab() on every event-loop pass while running
// (see settings/main.c's "#74/#382: honour a live tab-switch request even
// while already open" - the live-retarget path already existed, nothing
// was driving it), so the ALREADY-OPEN case needs no new IPC, only the
// missing "is one already open" check before deciding to spawn.
void settings_open_panel(int tab);

// #387 Dock / taskbar layout styles. Selectable live from Settings -> Appearance
// and persisted in the UI profile (key "dock_style"). DEFAULT reproduces the
// classic MayteraOS bottom taskbar byte-identically.
#define DOCK_DEFAULT     0   // classic bottom taskbar (start + apps + tray + gauges)
#define DOCK_LUMINA      1   // Lumina: top menu bar + floating bottom glass dock
#define DOCK_CLASSIC_UNIX 2  // Classic UNIX: beveled CDE/Motif-style front panel (workspace switcher)
#define DOCK_RETRO_BENCH 3   // Retro Bench: top screen bar (depth/zoom gadgets)
#define DOCK_XFCE        4   // #26 XFCE ("Marble" in Settings): glass top panel + glass flush bottom dock (pinned+running), icons ease a hover lift/grow (#745)
#define DOCK_COUNT       5
extern int g_dock_style;
// (#123) Marble-dock geometry preferences. Owned by taskbar.c (the only
// consumer of the geometry), persisted by profile.c into UIPROFIL.YML, and
// live-applied from /CONFIG/DOCKHGT.CFG and /CONFIG/DOCKZOOM.CFG by main.c -
// the same three-part arrangement g_dock_style and g_dock_opacity already use.
// Both are CLAMPED at the point of use inside taskbar.c (xfce_dock_h() /
// xfce_dock_zoom()), so no caller has to know the legal range.
//   g_dock_height - total marble dock height in px (44..96, default 59)
//   g_dock_zoom   - hover magnification, percent of the base icon size
//                   (100..200, default 128; 100 = no zoom)
// g_dock_height is a PREFERRED MAXIMUM. taskbar.c auto-scales the marble dock
// BELOW it (never above) when the pinned+running item count would make the pane
// wider than 75% of the current framebuffer width, and returns to the
// preference by construction when the count drops. taskbar_bottom_inset()
// reports the EFFECTIVE height; g_dock_height is only ever the ceiling.
extern int g_dock_height;
extern int g_dock_zoom;
// (#123) Apply a work-area change caused by an auto-scale step. Returns 1 if a
// change was pending. Called on main.c's poll cadence, never from the renderer.
int taskbar_geom_settle(void);
void taskbar_set_style(int s);               // apply a layout live
int  taskbar_top_inset(void);                // px reserved at the TOP of screen
int  taskbar_bottom_inset(void);             // px reserved at the BOTTOM of screen
int  taskbar_left_inset(void);               // px reserved at the LEFT of screen
int  taskbar_right_inset(void);              // px reserved at the RIGHT of screen
// (#745) THE work area: screen minus the four insets of the ACTIVE dock style.
// This is the single geometry every placement/clamp path must use - desktop
// icons, relocatable widgets, widget modals, and (via
// taskbar_publish_work_area -> SYS_WM_SET_WORK_AREA) the kernel window
// manager's window placement, maximize and title-bar drag. Do not re-derive it
// from TASKBAR_HEIGHT or from a literal; a top-panel style reserves the TOP.
void taskbar_work_area(int *x, int *y, int *w, int *h);
// (#745, local 81) Fit a popup of (w,h) on screen, preferring (*x,*y). Every
// popup/menu/flyout/overlay placement path must use this instead of clamping
// against g_fb_width/g_fb_height by hand: the raw framebuffer includes the
// taskbar/dock strip, and every one of these surfaces is drawn ABOVE that
// strip, so a hand-rolled clamp silently paints the last rows over the dock.
// Work-area aware (so a top-panel style is handled), max-then-min so an
// oversized popup still starts inside.
#define POPUP_EDGE_MARGIN 4
void popup_clamp_to_work_area(int w, int h, int *x, int *y);
// Call whenever the work area is established or changes: pushes the strut to
// the kernel window manager AND re-clamps compositor-side saved geometry.
void taskbar_apply_work_area(void);
void desktop_reclamp_icons(void);            // (#745) icons -> current work area
// (#745/#40) Relocatable widgets -> the current WIDGET AREA (see
// taskbar_widget_area() below; it was the work area until #40 let a widget
// sit under an overlay dock). Renamed with that change so the name cannot
// claim a bound the body does not apply.
void widgets_clamp_to_bounds(void);
// (#40) A plain screen-space rectangle, used for the chrome-as-obstacle list
// below. Deliberately int (not int32_t) to match the work-area accessors it
// sits next to, so a caller never has to mix the two widths in one clamp.
typedef struct { int x, y, w, h; } chrome_rect_t;
// (#40) Bounds for FLOATING DESKTOP FURNITURE (relocatable widgets, pets),
// as distinct from taskbar_work_area() which is what an app WINDOW may
// occupy. Identical to the work area except under an overlay-dock style
// (taskbar_dock_overlays_desktop(), today the marble dock), where it runs to
// the BOTTOM edge of the screen because that style's taskbar is at the TOP.
// Every widget placement/clamp path must use this one, not a literal.
void taskbar_widget_area(int *x, int *y, int *w, int *h);
int  taskbar_dock_overlays_desktop(void);
// (#40) The active style's chrome (top panel and/or bottom dock) as SOLID
// SURFACES in screen coordinates, so a collision consumer adds them to the
// rectangle list it already scans instead of writing a second, style-aware
// collision path. Returns the count written, never more than `max`; the
// marble dock's entry carries its LIVE centred width, and is absent on the
// very first frame, before the dock has painted once.
int  taskbar_panel_rects(chrome_rect_t *out, int max);
int  taskbar_menu_drops_from_top(void);      // 1 = start menu drops from a top bar

// startmenu.c - Start menu (Whisker-Menu-style uplift: search / favorites /
// recents / user header / power confirm - see startmenu.c's top comment).
void startmenu_init(void);
void startmenu_render(void);
bool startmenu_handle_mouse(int32_t x, int32_t y, bool clicked);
// Right-click on a Start-menu item: opens its context menu (Pin/Unpin, Add to
// Desktop, Properties) via contextmenu_open_for_menuitem(). Returns true if
// the click landed on an item.
bool startmenu_handle_right_click(int32_t x, int32_t y);
// #563: mouse wheel over the root row-list viewport or an open category
// flyout scrolls it (both are height-capped to the screen and may overflow).
// Returns 1 if consumed, 0 otherwise (menu closed, or the wheel was elsewhere).
int  startmenu_handle_scroll(int32_t x, int32_t y, int delta);
// Keyboard: the search box + ESC/Enter/Super, plus (#563) Up/Down/Left/Right
// to navigate categories and their cascading flyout. Captures every key while
// the menu is open (same "modal while open" idiom as launcher.c). Returns 1 if
// consumed, 0 if the menu is not open.
int  startmenu_handle_key(int key);
void startmenu_toggle(void);
// Throttled poll of /CONFIG/STARTMENU.PREFS (Settings "Start Menu" panel),
// same cadence idiom as main.c's dock_style_poll() - call once per frame.
void startmenu_prefs_poll(void);
// Throttled re-run of the Rust two-layer config merge (startmenu_model.rs),
// same cadence idiom as startmenu_prefs_poll() - call once per frame. This is
// what makes an App Store install (which writes a new system-layer fragment)
// or a hand-edited user fragment show up live, with no compositor rebuild and
// no menu reopen required.
void startmenu_rust_poll(void);
// #745 P1: throttled poll of the first-boot wizard's favourites live-apply
// channel (FAVCH.CFG), same cadence idiom as startmenu_prefs_poll() above -
// call once per frame. See startmenu_favs_poll()/sm_load_favs_channel() in
// startmenu.c for the channel format, the MAX_FAVORITES-overflow policy, and
// one-shot consume semantics.
void startmenu_favs_poll(void);
// Per-item actions the Start-menu context menu (contextmenu.c) dispatches into.
bool startmenu_item_is_favorite(int item_idx);
void startmenu_item_toggle_favorite(int item_idx);
// #26: path-keyed favorite accessors for the XFCE dock (see sm_fav_info_t
// above) - the dock has no menu_item_t index for a pinned app once the Start
// Menu itself is closed, only the persisted exec_path.
bool startmenu_is_favorite_path(const char *path);
void startmenu_toggle_favorite_path(const char *path);
int  startmenu_get_favorites(sm_fav_info_t *out, int max);
void startmenu_launch_path(const char *path, int launch_type);
// #44: reverse-lookup a kernel-resolved app_id back to its g_menu_items[]
// entry (exec_path/icon_id/launch_type), for a RUNNING-but-not-pinned dock
// tile's "Pin to Dock"/"Change Icon" actions. False (out untouched) if
// app_id is empty or unresolved - callers must omit those actions, not
// guess. Also applies every on-disk custom icon override at startup.
bool startmenu_find_by_app_id(const char *app_id, sm_fav_info_t *out);
void startmenu_apply_icon_overrides(void);
void startmenu_item_add_to_desktop(int item_idx);
void startmenu_item_open_properties(int item_idx);
// Properties popup (right-click -> Properties): a true modal, closes only via
// its own Close button or ESC.
bool startmenu_properties_open(void);
void startmenu_properties_render(void);
bool startmenu_properties_handle_mouse(int32_t x, int32_t y, bool clicked);
int  startmenu_properties_handle_key(int key);
// Power/session confirm dialog (Shutdown/Restart/Log Out/Lock): a true modal,
// closes only via its Cancel/confirm buttons or ESC.
bool startmenu_power_confirm_open(void);
void startmenu_power_confirm_render(void);
bool startmenu_power_confirm_handle_mouse(int32_t x, int32_t y, bool clicked);
int  startmenu_power_confirm_handle_key(int key);
#ifdef MAYTERA_TESTHOOK
// #334 headless verification hook ONLY (see testhook.c). Launches the start
// menu item whose label matches `name` exactly, the same launch switch a
// real click on the row runs (native/win16/dos), by NAME rather than by
// hit-testing a pixel inside the (possibly-not-even-open) menu popup.
bool startmenu_launch_item_by_name(const char *name);
// Lookup-only (does not launch): finds an item's index by its exact label,
// for the MENUCTX/MENUPIN test verbs (open a context menu / toggle a
// favorite by name without needing to hit-test a click).
int  startmenu_find_item_by_name(const char *name);
// #223 rd2: simulate the exact in-memory glitch under investigation
// (g_fav_count zeroed with no legitimate write) so the sm_save_recents_only()
// amplifier guard can be verified without needing to reproduce the real
// trigger. Diagnostic-only, same gate as everything else in this block.
void startmenu_debug_force_fav_zero(void);
// #223 rd3: call sm_record_recent() directly with an arbitrary (possibly
// >=128 byte) path, so the round-2 adjacency hypothesis (g_recent_paths
// ends exactly where g_fav_count begins in .bss) can be tested with a real
// long path instead of a scripted open/close repro. Diagnostic only, same
// gate as everything else in this block.
void startmenu_debug_record_recent(const char *path);
// #223 rd3: runs the full >=128-byte-path stress sequence (fill all
// MAX_RECENTS slots, force the already-full eviction branch, then a
// found>0 promote of a mid-list long entry) in one call, so a single
// one-shot TESTHOOK.CMD can exercise every shift/insert branch at once.
void startmenu_debug_recent_stress(void);
// #223 rd3b: forces a real nonzero g_fav_count baseline (via the same
// sm_seed_default_favorites() a real first boot calls) FIRST, confirmed via
// sm_favdebug_check, THEN runs startmenu_debug_recent_stress(). Needed
// because a test whose subject starts at g_fav_count==0 cannot show a
// nonzero-to-zero transition even if one exists.
void startmenu_debug_seed_and_stress(void);
// Sets the search query directly (bypasses key-by-key injection) so the live
// type-to-filter logic can be verified deterministically.
void startmenu_set_search(const char *q);
// #563: opens the Start menu (if not already open) and the named category's
// cascading flyout, by exact label match - bypasses hit-testing the same way
// the verbs above do, to verify the height-cap/render logic (screenshot) with
// zero mouse dependency. Hit-test correctness is a separate concern (see the
// #440 VNC guidance in testhook.c's file-top comment).
void startmenu_open_category_by_name(const char *label);
// (#glassmodal) Opens a power confirm dialog (1=Shut Down, 2=Restart,
// 3=Log Out, 4=Lock) by action number, calling the same static
// startmenu_power_confirm_show() the real power-grid icon click uses -
// bypasses that icon's mouse hit-test the same way the verbs above do.
void startmenu_test_power_confirm(int action);
#endif

// clock.c - Floating clock
void clock_render(void);
// #566: shared time/date formatting reused by the lock screen (lockscreen.c).
void lock_clock_hms(char *buf, int with_secs);   // "HH:MM" or "HH:MM:SS"
void lock_clock_date(char *buf);                 // "Wed, Jul 23"

// contextmenu.c - Right-click menu
void contextmenu_init(void);
void contextmenu_render(void);
bool contextmenu_handle_mouse(int32_t x, int32_t y, bool clicked);
void contextmenu_open(int32_t x, int32_t y);
// Opens the context menu in "Start menu item" mode: Pin/Unpin Favorite, Add to
// Desktop, Properties - dispatching into the startmenu_item_* functions above
// instead of the desktop's CTX_ACTION_* set. Reuses the same render/hit-test
// primitive as the desktop context menu (CLAUDE.md: reuse, don't hand-roll).
void contextmenu_open_for_menuitem(int32_t x, int32_t y, int menu_item_idx);
// #44: opens the context menu in "dock item" mode - the SAME render/hit-test
// primitive as the two modes above, extended with the actions requested for
// #44 (show/minimize/maximize/close/force quit/pin/unpin/change icon),
// omitting whichever of those has no real target rather than shipping a menu
// entry that does nothing (see taskbar.c's caller for exactly what each
// argument gates). win_id < 0 means "no window" (a pinned favorite that is
// not currently running): every window action is omitted. app_id/exec_path
// empty means "identity unresolved": Force Quit (needs app_id to find a pid)
// and Pin/Change Icon (need exec_path/icon_id) are independently omitted.
// #250: menu for a removable-volume desktop icon. `icon_idx` is the desktop
// icon index (desktop_icon_at()); `readable` says whether Open is offered.
void contextmenu_open_for_volume(int32_t x, int32_t y, int icon_idx, int readable);
void contextmenu_open_for_dock(int32_t x, int32_t y, int win_id, bool maximized,
                               const char *app_id, const char *exec_path,
                               icon_id_t icon_id, bool is_favorite);
void contextmenu_close(void);

// iconpicker.c - #44 "Change Icon" file picker. A TRUE MODAL (closes only via
// its own Cancel button or ESC, never click-away - it has real navigation
// state worth protecting, unlike contextmenu.c's lightweight popups): lists
// PNG/JPG/BMP files under the session user's home directory (non-recursive;
// the user drops/copies a source image there with Files first, the same
// discoverability model wallpaper_load()'s picker already uses for BMPs) and
// imports the selected one via icon_import_and_apply() (icons.c) when
// confirmed.
void iconpicker_open(const char *exec_path, icon_id_t icon_id);
void iconpicker_close(void);
bool iconpicker_is_open(void);
void iconpicker_render(void);
bool iconpicker_handle_mouse(int32_t x, int32_t y, bool clicked);
int  iconpicker_handle_key(int key);

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
void screensaver_note_activated(void);   // #570: record activation time for the input grace
uint64_t screensaver_active_since_ms(void);  // #652: uptime_ms() of last activation, for the blank-after stage
// #596: stamped by main.c when a TRUE-FULLSCREEN window presents a frame;
// read by screensaver_check_timeout() to suppress activation over a running
// fullscreen app. uptime_ms() units, 0 = never.
extern uint64_t g_fs_present_ms;

// cursor drawing
void cursor_render(void);

#endif // COMPOSITOR_H
