// main.c - MayteraOS Userland Compositor
// Entry point and frame orchestrator for the Phase 3 desktop compositor.
// Responsibilities: init, input polling, event dispatch, and render loop.
//
// NOTE: Do NOT call sys_putchar() anywhere in this file. It writes to the
// process's PTY (fds[1]), which has no reader, causing the compositor to
// block indefinitely and hang the entire desktop.

#include "compositor.h"
#include "../../libc/notify.h"
#include "../../libc/syscall.h"
#include "../../libc/keys.h"      // #243: GUI_KEY_* - THE keycode table
#include "../../libc/theme.h"   // (#285) theme_color_of + color ids
#include "../../libc/gui_theme.h" // (#565) file-based theme loader
// #683: per-user preference paths. This was included only under
// MAYTERA_TESTHOOK, yet dock_style_write_cfg() below calls
// userconf_open_write() in EVERY build, so the shipping compositor was
// compiling that call against an implicit declaration. Unconditional now;
// userconf.h pulls in no types, so it cannot reintroduce the compositor.h
// `typedef int bool` conflict.
#include "../../libc/userconf.h"
#include "../../libc/string.h"   // #745 P2: strcmp for widgets_cfg_poll() bind validation
#include "../../libc/dock_opacity.h"  // #132: shared DOCK_OPACITY_MIN/MAX
#include "../../libc/settingscfg.h"   // #230: settingscfg_dblclick_ms()
#include "perfframe.h"
#include "idleprof.h"   // #COMPIDLE: idle-CPU reason/cost profiler
#ifdef MAYTERA_TESTHOOK
#include "testhook.h"   // #334 headless verification hook - never in a normal build
#endif

// fb_info_t, fb_map(), fb_info(), fb_flip(), grab_input(), get_mouse_evt(),
// mouse_evt_t are all provided by ../../libc/syscall.h.

// ============================================================================
// Global state definitions (declared extern in compositor.h)
// ============================================================================

uint32_t *g_fb            = NULL;
int32_t   g_fb_width      = 0;
int32_t   g_fb_height     = 0;
int32_t   g_fb_pitch      = 0;

int32_t   g_mouse_x       = 0;
int32_t   g_mouse_y       = 0;
uint32_t  g_mouse_buttons = 0;
uint32_t  g_mouse_prev_buttons = 0;

bool g_start_menu_open      = false;
bool g_context_menu_open    = false;
int32_t g_context_menu_x   = 0;
int32_t g_context_menu_y   = 0;
bool g_wallpaper_picker_open = false;

bool g_logged_in    = false;
int  g_login_uid    = -1;
char g_login_username[64] = {0};

bool     g_screensaver_active = false;
uint64_t g_idle_ms           = 0;

bool g_needs_redraw = true;
extern bool g_setup_pending;        // defined below; read by the input tick
extern bool g_setup_handover_armed; // #203: defined below; read by the input tick
void setup_pending_recheck(void);   // defined below; called from the input tick
void dock_style_write_cfg(int v);   // #387 live channel, defined below
void dock_opacity_write_cfg(int v); // #745 live channel, defined below
void dock_height_write_cfg(int v);  // #123 live channel, defined below
void dock_zoom_write_cfg(int v);    // #123 live channel, defined below

// T0 #578: per-tick input classification, set by poll_input(), read by the
// main loop to choose the cheap pointer-motion present path. g_tick_motion =
// the pointer moved this tick; g_tick_nonmotion = a key / mouse button / a
// foreground Win16 app was active this tick (anything that can change screen
// content beyond just the cursor position).
static bool g_tick_motion    = false;
static bool g_tick_nonmotion = false;

// ============================================================================
// Cursor data: 12x12 arrow
// 0 = transparent, 1 = black outline, 2 = white fill
// ============================================================================

static const uint8_t cursor_data[12][12] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,1,1,1,1,0,0,0},
    {1,2,2,1,2,1,0,0,0,0,0,0},
    {1,1,1,0,1,2,1,0,0,0,0,0},
    {0,0,0,0,0,1,1,0,0,0,0,0},
};

// ============================================================================
// cursor_render: blit the 12x12 arrow onto the framebuffer at the mouse position
// ============================================================================

// (#116) Cursor style + size, set from the UI profile (profile.c reads
// curstyle/cursize from UIPROFIL.YML). style: 0=Light, 1=Dark, 2=Glow.
// size is a scale percent: 100 = 1.0x (default, unchanged), up to 250 = 2.5x.
int g_cursor_style = 0;
int g_cursor_size  = 100;

static inline void cursor_plot(int px, int py, uint32_t color)
{
    if (px < 0 || py < 0 || px >= g_fb_width || py >= g_fb_height) return;
    g_fb[py * g_fb_pitch + px] = color;
}

// #uiscale: the cursor already had a user-persisted size PREFERENCE
// (g_cursor_size, 100..250 = 1.0x..2.5x, set via Settings -> set_cursor()).
// The UI scale factor is a SEPARATE, independent multiplier that stacks on
// top of it, not a replacement for it - a user who picked a big cursor at
// 100% scale should still get a big cursor at 150% scale, just bigger again.
// This is the SAME base-times-preference pattern taskbar.c's dock_zoom uses
// on top of the dock's own scaled base (see taskbar.c #uiscale comment).
// cursor_render() and cursor_bounds() both used to compute the exact same
// "int scale = (g_cursor_size...) ? g_cursor_size : 100;" clamp independently
// (a copy-paste-drift risk in its own right); both now call this one function.
static inline int cursor_effective_scale(void)
{
    int base = (g_cursor_size >= 50 && g_cursor_size <= 250) ? g_cursor_size : 100;
    return base * g_ui_scale_pct / 100;
}

void cursor_render(void)
{
    // (#116) Pull the live cursor style/size from the kernel every frame so a
    // change made in Settings (which calls set_cursor()) applies WITHOUT a reboot,
    // the same way theme/opacity propagate. Cheap syscall; packed style|size<<8.
    int pk = get_cursor();
    if (pk >= 0) {
        g_cursor_style = pk & 0xFF;
        int sz = (pk >> 8) & 0xFFFF;
        g_cursor_size = (sz >= 50) ? sz : 100;
    }

    int scale = cursor_effective_scale();

    if (g_cursor_style == 2) {
        // Glow: a pulsing accent disc with a thin white line on the inner AND
        // outer edge so it stays readable over accent-colored buttons. Radius +
        // brightness pulse via a free-running frame counter (no time source).
        static unsigned tick = 0;
        tick++;
        unsigned ph  = tick & 63u;
        int      tri = (ph < 32u) ? (int)ph : (int)(64u - ph);   // 0..32 triangle
        int base = 5 * scale / 100; if (base < 5) base = 5;
        int r    = base + (base / 2) * tri / 32;                  // pulse the radius
        int br   = 205 + 50 * tri / 32;                           // 205..255 brightness
        // Bright, saturated azure: low R, mid-high G, full B, brightness-pulsed.
        uint32_t accent = 0xFF000000u
                        | ((uint32_t)(br / 5)        << 16)       // R
                        | ((uint32_t)(br * 7 / 10)   << 8)        // G
                        | ((uint32_t)br);                          // B
        const uint32_t white = 0xFFFFFFFFu;
        int cx = g_mouse_x, cy = g_mouse_y;
        for (int dy = -(r + 1); dy <= r + 1; dy++) {
            for (int dx = -(r + 1); dx <= r + 1; dx++) {
                int d2 = dx * dx + dy * dy;
                uint32_t col;
                if      (d2 <= (r - 2) * (r - 2)) col = accent;   // accent core
                else if (d2 <= (r - 1) * (r - 1)) col = white;    // inner white line
                else if (d2 <=  r      *  r)      col = accent;   // accent edge
                else if (d2 <= (r + 1) * (r + 1)) col = white;    // outer white line
                else continue;
                cursor_plot(cx + dx, cy + dy, col);
            }
        }
        return;
    }

    // Light (default) or Dark arrow, nearest-neighbor scaled by `scale`.
    int outdim = 12 * scale / 100;
    if (outdim < 12) outdim = 12;
    for (int oy = 0; oy < outdim; oy++) {
        int sy = oy * 100 / scale; if (sy > 11) sy = 11;
        for (int ox = 0; ox < outdim; ox++) {
            int sx = ox * 100 / scale; if (sx > 11) sx = 11;
            uint8_t v = cursor_data[sy][sx];
            if (v == 0) continue;
            uint32_t color;
            if (g_cursor_style == 1)              // Dark: white outline, dark fill
                color = (v == 1) ? 0xFFFFFFFFu : 0xFF202020u;
            else                                  // Light: black outline, white fill
                color = (v == 1) ? 0xFF000000u : 0xFFFFFFFFu;
            cursor_plot(g_mouse_x + ox, g_mouse_y + oy, color);
        }
    }
}

// ============================================================================
// compositor_init: set up the framebuffer and all subsystems
// ============================================================================

// #185 forward declarations (definitions live further down with launch_app)
static int  g_aichat_pid;
static void aichat_write_cfg(int enabled);

// #371: the AI Chat app's ON-DISK ext2 entry is "/APPS/AICHAT" (uppercase;
// the static asset base preserves that case and build-golden.sh's overlay
// resolves the freshly-built lowercase "aichat" binary onto it case-
// insensitively, same class of name-vs-dir trap as #517's COMPOSIT vs
// COMPOSITOR). ext2 is case-sensitive at spawn time, so every sys_spawn()
// of this app MUST use this exact path, not a guessed-case literal.
#define AICHAT_APP_PATH "/APPS/AICHAT"

static int compositor_init(void)
{
    // #uiscale: MUST be the very first thing this process does, before
    // fb_info() below or anything else. This process is not yet registered
    // as the framebuffer owner at this point (that only happens once it
    // claims later on), so without this mark the kernel answers fb_info()
    // the same way it answers every ordinary app: with the LOGICAL
    // (unscaled) screen size, not the real physical one this file needs to
    // draw chrome directly onto real pixels. See uiscale.h's own comment on
    // ui_scale_mark_native() for the exact failure this order fixes
    // (a 150% boot laid the whole desktop out for 1280x720 on a 1920x1080
    // panel) - added here as soon as the kernel-side mark existed to fix it.
    ui_scale_mark_native();

    fb_info_t fi;
    if (fb_info(&fi) < 0) {
        return -1;
    }

    g_fb = (uint32_t *)(unsigned long)fb_map();
    if (!g_fb) {
        return -1;
    }

    g_fb_width  = (int32_t)fi.width;
    g_fb_height = (int32_t)fi.height;
    // pitch is in bytes; convert to pixel units (32-bit pixels = 4 bytes each)
    g_fb_pitch  = (int32_t)(fi.pitch / 4);

    // Request exclusive input so the kernel does not also process events
    grab_input(1);

    // Start mouse in screen center
    g_mouse_x = g_fb_width  / 2;
    g_mouse_y = g_fb_height / 2;

    // Capture baseline idle timestamp (monotonic ms)
    g_idle_ms = uptime_ms();


    // Subsystem initialisation (order matters: wallpaper before desktop)
    // #uiscale BUGFIX (ui-ux screendump, real 2560x1600 @200%): seed the
    // scale factor and the scale-derived desktop-icon globals BEFORE any
    // layout math runs. ui_scale_refresh() (normally a per-frame main-loop
    // call - see the main loop below) had never been called yet at this
    // point, so g_ui_scale_pct still held its startup default of 100 and
    // desktop_init() -> layout_default_top() -> grid_cell_pos() below baked
    // every desktop icon's (px,py) into a grid spaced with the UNSCALED
    // DESKTOP_ICON_SPACING_X/Y (compositor_apply_icon_size() had not run
    // with the real factor yet either). The icon BITMAP and its LABEL TEXT
    // both scale live every frame regardless (DESKTOP_ICON_SIZE is read at
    // draw time, and draw_text_ttf()/text_width_ttf() apply ui_px() at call
    // time), so the visible defect was exactly "things got bigger, the grid
    // that holds them did not" - labels from adjacent icons overlapping.
    // Calling both here, once, before desktop_init(), means the very first
    // layout pass already uses the correct scaled spacing.
    ui_scale_refresh();
    compositor_apply_icon_size(get_icon_size());

    wallpaper_init();
    desktop_init();
    taskbar_init();
    // Load shared color icons (SVG-derived) for Computer + Browser (#66).
    icon_load_color(ICON_COMPUTER, "/ICONS/COMPUTER.ICN");
    icon_load_color(ICON_BROWSER,  "/ICONS/BROWSER.ICN");
    icon_load_color(ICON_TRASH,    "/ICONS/RECYCLE.ICN");
    icon_load_color(ICON_TERMINAL, "/ICONS/TERMINAL.ICN");
    icon_load_color(ICON_COG,      "/ICONS/SETTINGS.ICN");
    // #562: ICON_GAME is the GENERIC game glyph, not DOOM's. It used to load
    // DOOM.ICN directly, so every app that fell back to ICON_GAME (Arena, Chess,
    // Squadron, GL Cube, GL Matrix) rendered the DOOM logo. Each of those now has
    // its own icon (below); ICON_GAME loads a neutral controller glyph and is the
    // fallback only for games with no dedicated art (Lemmings/Pong).
    icon_load_color(ICON_GAME,     "/ICONS/GAME.ICN");
    // #214: the GAMES start-menu entry for DOOM uses id ICON_GAME_DOOM, and the
    // desktop DOOM icon was changed from ICON_GAME to ICON_GAME_DOOM (#562, see
    // desktop.c) now that ICON_GAME is generic. Both load the real DOOM icon.
    icon_load_color(ICON_GAME_DOOM, "/ICONS/DOOM.ICN");
    icon_load_color(ICON_IRC,      "/ICONS/IRC.ICN");
    icon_load_color(ICON_HIGHLIGHT,  "/ICONS/EDITOR.ICN");
    icon_load_color(ICON_CALCULATOR, "/ICONS/CALC.ICN");
    icon_load_color(ICON_IMAGE,      "/ICONS/IMGVIEW.ICN");
    icon_load_color(ICON_MUSIC,      "/ICONS/APLAYER.ICN");
    icon_load_color(ICON_VIDEO,      "/ICONS/MPLAYER.ICN");
    icon_load_color(ICON_CLOCK,      "/ICONS/CLOCK.ICN");
    icon_load_color(ICON_FOLDER,     "/ICONS/FILES.ICN");
    icon_load_color(ICON_NETWORK,    "/ICONS/NETWORK.ICN");
    icon_load_color(ICON_PAINT,         "/ICONS/PAINT.ICN");
    icon_load_color(ICON_GAME_SOLITAIRE,"/ICONS/SOLITR.ICN");
    icon_load_color(ICON_SLIDERS,       "/ICONS/SLIDERS.ICN");
    icon_load_color(ICON_CHEVD,         "/ICONS/CHEVD.ICN");
    icon_load_color(ICON_CHEVR,         "/ICONS/CHEVR.ICN");
    icon_load_color(ICON_WIN3X,         "/ICONS/WIN3X.ICN");   // #208 Win16 games
    icon_load_color(ICON_DOSAPP,        "/ICONS/DOSAPP.ICN");  // #208 DOS games
    // #562: per-app icons for every app that was sharing another app's icon or
    // riding the generic placeholder (ICON_WINDOW/ICON_FILE). New original
    // lineart, same stroke weight/style as the existing set (see
    // assets/icons/README.md); a few reuse an existing unwired glyph that was
    // already a good semantic match (Help, Font Book, Feeds, System Monitor,
    // Services, AI Chat).
    icon_load_color(ICON_GAME_ARENA,    "/ICONS/ARENA.ICN");
    icon_load_color(ICON_GAME_CHESS,    "/ICONS/CHESS.ICN");
    icon_load_color(ICON_GAME_SQUADRON, "/ICONS/SQUADRON.ICN");
    icon_load_color(ICON_GAME_GLCUBE,   "/ICONS/GLCUBE.ICN");
    icon_load_color(ICON_GAME_GLMATRIX, "/ICONS/GLMATRIX.ICN");
    icon_load_color(ICON_AICHAT,        "/ICONS/CHATBUB.ICN"); // existing, unwired
    icon_load_color(ICON_WEATHER,       "/ICONS/WEATHER.ICN");
    icon_load_color(ICON_FEEDS,         "/ICONS/RSS.ICN");     // existing, unwired
    icon_load_color(ICON_GALLERY,       "/ICONS/GALLERY.ICN");
    icon_load_color(ICON_SNAPSHOT,      "/ICONS/SNAPSHOT.ICN");
    icon_load_color(ICON_NOTES,         "/ICONS/NOTES.ICN");
    icon_load_color(ICON_FONTBOOK,      "/ICONS/BOOK.ICN");    // existing, unwired
    icon_load_color(ICON_CONVERTER,     "/ICONS/CONVERT.ICN");
    icon_load_color(ICON_TIMERS,        "/ICONS/TIMERS.ICN");
    icon_load_color(ICON_PYTHON,        "/ICONS/PYTHON.ICN");
    icon_load_color(ICON_AUTH,          "/ICONS/AUTH.ICN");
    icon_load_color(ICON_HELP,          "/ICONS/HELP.ICN");    // existing, unwired
    icon_load_color(ICON_LAUNCHER,      "/ICONS/TILE.ICN");    // existing, unwired
    icon_load_color(ICON_TASKSWITCH,    "/ICONS/WINSWTCH.ICN");
    icon_load_color(ICON_APPSTORE,      "/ICONS/APPSTORE.ICN");
    icon_load_color(ICON_SYSMON,        "/ICONS/MONITOR.ICN"); // existing, unwired
    icon_load_color(ICON_SERVICES,      "/ICONS/GEAR.ICN");    // existing, unwired
    icon_load_color(ICON_3DPRINT,       "/ICONS/PRINT3D.ICN");
    // #deskicons: removable-volume icons (desktop.c desktop_append_volume_icons())
    // had NO color art at all - ICON_CDROM/ICON_FLOPPY/ICON_USBDRIVE only ever
    // existed as 24x24 1-bit mono glyphs in icons.c's icons[] fallback table, so
    // icon_draw_color_if_present() always missed and icon_draw_scaled() fell back
    // to that table's hand-rolled nearest-neighbour scaler with NO drop-shadow
    // pass (only the color path draws one). Reported on a real 4K/200% panel as
    // "looks like a CD but pixelated, no drop shadow" - both symptoms are this
    // ONE missing asset registration, not two different draw paths: the shape
    // (glyph) was already correct, the mono fallback path was just always ugly
    // and became strikingly so once the desktop icon size started scaling past
    // the 64x64 MICO source's native size. New 64x64 MICO art added to the asset
    // base to match (see /ICONS/CDROM.ICN, FLOPPY.ICN, USBDRIVE.ICN).
    icon_load_color(ICON_CDROM,         "/ICONS/CDROM.ICN");
    icon_load_color(ICON_FLOPPY,        "/ICONS/FLOPPY.ICN");
    icon_load_color(ICON_USBDRIVE,      "/ICONS/USBDRIVE.ICN");
    startmenu_init();
    contextmenu_init();
    traymenu_init();
    profile_load();   /* #92 apply saved UI profile */
    // #44: apply every per-user custom icon override that exists on disk
    // ("<home>/ICONS/<basename>.MICO" - see icons.c/startmenu.c). Must run
    // AFTER startmenu_init() (g_menu_items[] needs to exist to resolve which
    // icon_id each exec_path maps to) and after profile_load() has resolved
    // the session user (userhome_path() reads the passwd table via getuid(),
    // which profile_load() already depends on succeeding by this point).
    startmenu_apply_icon_overrides();
    /* (#745) SECOND-PASS ICON PLACEMENT. Any desktop icon the profile did not
       carry a position for (a file that appeared in <home>/DESKTOP since the
       profile was written, or a first boot with no profile at all) still has a
       provisional slot that a restored icon may have landed on. This gives each
       one a slot no other icon occupies. Deliberately outside profile_load(),
       whose early returns would skip it in precisely the no-profile case. */
    desktop_place_unplaced();
    stickies_load();  /* #270 load persisted sticky notes */
    {   /* #185 auto-launch the AI Chat app iff its persisted flag is enabled */
        extern int g_aichat_enabled;
        /* Not while the machine is unconfigured: the AI panel would sit on top
           of the setup wizard advertising an assistant whose API key the wizard
           is still asking for. The marker is read directly rather than via
           g_setup_pending, because THAT flag is set later in main() and this
           runs inside compositor_init(). */
        int __sfd = sys_open("/CONFIG/SETUPDONE", 0);
        int __configured = (__sfd >= 0);
        if (__sfd >= 0) sys_close(__sfd);
        if (g_aichat_enabled && __configured) { aichat_write_cfg(1); g_aichat_pid = sys_spawn(AICHAT_APP_PATH); }
    }
    set_cursor(g_cursor_style, g_cursor_size);  /* (#116) seed kernel with persisted cursor */
    dock_style_write_cfg(g_dock_style);  /* #387 seed Settings with the loaded dock style */
    /* (#745) Establish the work area for the dock style profile_load() just
       applied. taskbar_set_style() also does this, but it early-returns when
       the style is unchanged, which is the normal boot path - so without this
       call the kernel would keep its default bottom-only reservation and a
       top-panel style would still swallow window headers. Runs AFTER
       profile_load() so the icon/widget positions it re-clamps are the
       restored ones. */
    taskbar_apply_work_area();
    dock_opacity_write_cfg(g_dock_opacity);  /* #745 same, for the glass opacity */
    /* (#123) Same seeding contract for the two new marble-dock geometry
       preferences: the compositor is the owner of UIPROFIL.YML, so it publishes
       what it actually loaded into the CFG files Settings reads on open. Without
       this, Settings would show its own compiled-in default the first time it
       runs against a profile that has a different value, and the first drag of
       the slider would jump the dock. */
    dock_height_write_cfg(g_dock_height);
    dock_zoom_write_cfg(g_dock_zoom);
    profile_save();   /* ensure the profile file exists */
    screensaver_init();
    // (#73) login_init() is GONE, with the rest of the compositor's own login
    // screen. The real login gate is kernel/gui/login.c, which authenticates
    // BEFORE this process is spawned; this one was a superseded duplicate that
    // still armed a key sink. See the deletion note in CHANGELOG.md.
    lock_init();   // #566 secure session lock overlay

    return 0;
}

// ============================================================================
// process_input: read mouse and keyboard; update global state
// ============================================================================

// Frame-local input state shared between process_input and process_events
static bool s_left_pressed  = false;
static bool s_left_released = false;
static bool s_right_pressed = false;
static bool s_dbl_click     = false;
static int  s_last_key      = -1;
static bool s_dragging_sheep = false;
static bool s_dragging_dog = false;
static bool s_dragging_widget = false;
static bool s_dragging_sticky = false;    // sticky-note title-bar drag (#270)
static bool s_dragging_desktop = false;   // icon drag / rubber-band in progress
static bool s_dragging_settings = false;  // #419b settings-modal title-bar / scrollbar drag
// #566 Super+L lock hotkey. The kernel key model sends KEY_SUPER (0x9B) as a
// single one-shot event with no held/modifier state (see the s_last_key
// comment below on why 0x9B lives in the release-code range) - there is no
// true "chord" to detect. This arms a short window on a Super press; if 'l'/
// 'L' follows inside it, that is treated as the Lock hotkey. Super's normal
// action (toggling the start menu) still fires on the Super press itself
// (unchanged), but lock_enter() force-closes the start menu as part of
// entering lock, so the brief flash is hidden rather than left open.
static uint64_t s_super_press_ms = 0;
#define SUPER_L_WINDOW_MS 600
// #232: the SAME window now serves a SECOND chord, Super+Space, which opens the
// AI command launcher (Spotlight). It is one timestamp and one window because
// there is one Super press to hang both off; see the Super+Space branch in
// process_events() for why the launcher had to move off its old bare-F1
// binding, and GLOBALLY_CAPTURED_KEYS below for the full list of what this
// compositor takes before an app can see it.
// widgets.c drag API (relocatable desktop widgets)
int  widget_hit(int x, int y);
void widget_grab(int x, int y);
void widget_drag_to(int x, int y);
void widget_release(void);
// #419b widgets.c draggable settings modal + scrollable HA entity list
int  widget_settings_press(int x, int y);
void widget_settings_drag_to(int x, int y);
void widget_settings_drag_end(void);
int  widget_settings_handle_scroll(int x, int y, int delta);
// widgets.c per-widget right-click menu (Hide / Lock)
void widget_menu_open(int which, int x, int y);
int  widget_menu_is_open(void);
void widget_menu_render(void);
int  widget_menu_handle(int x, int y, int click);

// ============================================================================
// #745 (task #68) THE MODAL KEYBOARD GRAB REGISTRY - one table, not an
// ever-growing OR-chain.
//
// DRAWING on top of an app and RECEIVING that app's input are two SEPARATE
// mechanisms in this compositor. blame.md already carries the lesson from the
// elevation prompt (2026-08-10, "A modal that draws over an app is not a modal
// that INPUT respects"): a surface that copies only the DRAW half is a prompt
// the app underneath can read. That was the second instance; the "Ask Maytera
// AI" launcher was the third, and the same audit found six more surfaces and a
// remote-input path that had never been gated at all.
//
// WHY IT IS A DISCLOSURE BUG AND NOT A GLITCH: the user believes the overlay
// owns their typing. process_input() called sys_inject_key() for every key
// regardless, so an AI prompt (or a start-menu search, or a sticky note) was
// ALSO typed into whatever app window held kernel focus underneath - an open
// editor at best, a Terminal at worst. #66 requires AI conversations to be
// per-user and private; this leaked the INPUT side before storage even
// existed.
//
// THE MECHANISM FIX, not the instance: "is a modal grabbing input right now"
// has exactly ONE definition, this table, and BOTH halves read it:
//
//   * modal_key_grab(key) gates EVERY forward to an app window: the hardware
//     path in process_input() below, and the completely independent RFB path
//     in vnc.c (a remote viewer was bypassing even the session lock).
//   * process_events() DISPATCHES the key by walking this same table in table
//     order, so priority and suppression cannot disagree.
//
// The failure mode of forgetting the table is therefore LOUD: a new modal that
// does not join it receives no keys at all and is visibly dead the first time
// anyone types into it, instead of silently leaking. That is the property the
// OR-chain never had - the launcher was missed precisely because dispatch
// (which worked) and suppression (which did not) were two separate lists.
//
// TO ADD A SURFACE, this is the whole procedure:
//   1. add a row below, in the priority order you want;
//   2. is_open() returns non-zero while the surface owns the keyboard;
//   3. handle_key() takes the raw key (NULL = swallow without dispatch);
//   4. claims = MG_ALL for anything the user TYPES into; MG_ESC for a
//      pointer-driven popup whose only key is Esc.
// #66's AI tab is one row here, and nothing else.
// ============================================================================
#define MG_ALL 0   /* the surface owns every keystroke while it is open */
#define MG_ESC 1   /* pointer-driven popup: it owns Esc and nothing else   */

typedef struct {
    const char *name;                                 /* for readers, not code */
    int  (*is_open)(void);                            /* nonzero while it owns the keyboard */
    int  (*handle_key)(int key);                      /* NULL: swallow, do not dispatch */
    int  (*handle_mouse)(int x, int y, int clicked);  /* exclusive rows only */
    unsigned char claims;                             /* MG_ALL / MG_ESC */
    unsigned char exclusive;  /* also owns the POINTER and ends the input tick */
    unsigned char dispatch;   /* process_events() routes the key here */
} modal_grab_t;

/* Thin readers for the surfaces whose state is a plain bool rather than a
   predicate function. compositor.h's `typedef int bool` is why every other
   is_open()/handle_key() in the table can be used directly with no shim. */
static int mg_lock_open(void)       { return g_session_locked ? 1 : 0; }
static int mg_saver_open(void)      { return g_screensaver_active ? 1 : 0; }
static int mg_startmenu_open(void)  { return g_start_menu_open ? 1 : 0; }
static int mg_wallpaper_open(void)  { return g_wallpaper_picker_open ? 1 : 0; }
static int mg_ctxmenu_open(void)    { return g_context_menu_open ? 1 : 0; }

static const modal_grab_t g_modal_grabs[] = {
    /* --- EXCLUSIVE tier: owns the pointer too, and ends the input tick. --- */
    /* name              is_open              handle_key                          handle_mouse           claims  excl disp */
    { "lock-screen",     mg_lock_open,        lock_handle_key,                    lock_handle_mouse,     MG_ALL,  1,  0 },
    { "elevate-prompt",  elevate_modal_open,  elevate_handle_key,                 elevate_handle_mouse,  MG_ALL,  1,  0 },

    /* --- SWALLOW tier: owns the keyboard, has its own block in
       process_events() (the screensaver's early return), dispatches nothing. */
    { "screensaver",     mg_saver_open,       NULL,                               NULL,                  MG_ALL,  0,  0 },

    /* --- TYPING tier: dispatched by the walk in process_events(). Order here
       IS the priority order that used to be an else-if chain. --- */
    { "ai-launcher",     launcher_is_open,        launcher_handle_key,                NULL,              MG_ALL,  0,  1 },
    { "power-confirm",   startmenu_power_confirm_open, startmenu_power_confirm_handle_key, NULL,          MG_ALL,  0,  1 },
    { "item-properties", startmenu_properties_open,    startmenu_properties_handle_key,    NULL,          MG_ALL,  0,  1 },
    { "force-quit-confirm", taskbar_force_quit_confirm_open, taskbar_force_quit_confirm_handle_key, NULL,   MG_ALL,  0,  1 },
    { "icon-picker",     iconpicker_is_open,      iconpicker_handle_key,              NULL,              MG_ALL,  0,  1 },
    { "widget-settings", widget_settings_is_open, widget_settings_handle_key,         NULL,              MG_ALL,  0,  1 },
    { "sticky-editor",   stickies_editing,        stickies_handle_key,                NULL,              MG_ALL,  0,  1 },
    { "start-menu",      mg_startmenu_open,       startmenu_handle_key,               NULL,              MG_ALL,  0,  1 },
    /* docs/NOTIFICATIONS_DESIGN.html sect;5: the notification centre is being
       PROMOTED OUT of the ESC-ONLY tier below, not added to it - #334 makes
       pointer clicks unreliable on the owner's hardware, and this surface's
       whole job is to be read, so Esc/Tab/Enter are now its primary path.
       notif_handle_key() owns its own Esc handling internally (matching
       startmenu_handle_key()'s existing pattern) rather than routing through
       the ESC-ONLY fallthrough block. notif_handle_mouse()'s existing
       dispatch is untouched: mouse and keyboard are independent paths here,
       exactly as they already are for the start menu. */
    { "notif-center",    notif_center_open,       notif_handle_key,                   NULL,              MG_ALL,  0,  1 },

    /* --- ESC-ONLY tier: pointer-driven popups. They take no typing, so
       gating every key would only swallow input the user meant for the app.
       They DO consume Esc (in the fallthrough block of process_events), and an
       Esc that closes an overlay AND reaches the app is the exact symptom that
       exposed the elevation-modal leak, so Esc alone is claimed here. Popups
       that consume NO key at all (tray menu, per-widget menu, taskbar tile
       menu, taskbar perf popup) are deliberately absent: they never take a
       keystroke the user believes is theirs. The notification centre used to
       be named here too; it is now in the TYPING tier above (docs/
       NOTIFICATIONS_DESIGN.html sect;5). --- */
    { "wallpaper-picker", mg_wallpaper_open,      NULL,                               NULL,              MG_ESC,  0,  0 },
    { "context-menu",     mg_ctxmenu_open,        NULL,                               NULL,              MG_ESC,  0,  0 },
};

#define MG_COUNT ((int)(sizeof(g_modal_grabs) / sizeof(g_modal_grabs[0])))

// Does an open surface claim THIS key? The one gate every app-forward reads.
int modal_key_grab(int key)
{
    for (int i = 0; i < MG_COUNT; i++) {
        const modal_grab_t *m = &g_modal_grabs[i];
        if (!m->is_open || !m->is_open()) continue;
        if (m->claims == MG_ALL) return 1;
        /* MG_ESC claims the Esc PRESS (0x1B). Its release code is 0x9B, which
           the kernel key model also uses for KEY_SUPER (see the s_last_key
           comment below), so it is deliberately not claimed here: swallowing
           0x9B would eat the Super key. */
        if (m->claims == MG_ESC && key == 0x1B) return 1;
    }
    return 0;
}

// First open EXCLUSIVE surface (lock screen, elevation prompt), or NULL.
static const modal_grab_t *modal_grab_exclusive(void)
{
    for (int i = 0; i < MG_COUNT; i++) {
        if (g_modal_grabs[i].exclusive && g_modal_grabs[i].is_open()) {
            return &g_modal_grabs[i];
        }
    }
    return NULL;
}

// First open surface that wants the key DISPATCHED to it, or NULL.
static const modal_grab_t *modal_grab_keyboard(void)
{
    for (int i = 0; i < MG_COUNT; i++) {
        if (g_modal_grabs[i].dispatch && g_modal_grabs[i].is_open()) {
            return &g_modal_grabs[i];
        }
    }
    return NULL;
}

// Double-click tracking
static uint64_t s_last_click_ms = 0;
// #230 (docs/SETTINGS_CONTROL_AUDIT.md #224): was a hardcoded 500 with no
// consumer of Settings' "Double-click Speed" slider anywhere in the tree, so
// the control persisted a value nothing read. Now real:
// settingscfg_dblclick_ms() (libc/settingscfg.c) reads SETTINGS.CFG's 'k' key
// on a 2s throttle.
//
// #236 CORRECTION of the note that used to sit here. #230 wired a real
// reader, but the reader's return value (real MILLISECONDS, 150-900) was
// compared directly against a difference of sys_clock() TICKS a few lines
// below - and sys_clock() returns the raw 250Hz PIT counter (4ms per tick,
// same rate this function's own "suppressed" boot-window comment documents),
// not milliseconds. Comparing a ms threshold against a tick delta scales the
// EFFECTIVE threshold by the tick period: a 500ms setting behaved as 2000ms.
// Measured on golden 2016: the double-click boundary sat at ~1.7-2.0s
// regardless of the slider position, which is exactly what a threshold stuck
// at the pre-#230 hardcoded 500 (interpreted as 500 ticks = 2.0s) looks like.
// Fixed by switching the time source to uptime_ms() (SYS_UPTIME_MS), a real
// monotonic-millisecond clock userland already has (used by settingscfg.c's
// own poll throttle), so both sides of the compare are now the same unit.
//
// THIS WAS ALSO NOT THE ONLY DETECTOR. desktop.c's desktop_release() (the
// path that actually fires for a desktop-icon click, since s_dbl_click below
// only reaches desktop_handle_mouse() via a fallback branch the press/drag
// path does not normally take) had its OWN independent hardcoded `500ULL`
// tick comparison, and kernel gui/window.c's title-bar double-click had a
// THIRD, kernel-side hardcoded ~0.5s. Three detectors, none of them reading
// this setting. See desktop.c and window.c for the matching fixes, and
// blame.md for why tools/pref-reader-lint could not have caught any of this.
static int dbl_click_threshold(void) { return settingscfg_dblclick_ms(); }

// Push the current threshold into the kernel (title-bar double-click/
// maximize, gui/window.c) whenever it actually changes, same "only call the
// syscall on a real change" discipline settings_autosave()'s contract_hash()
// uses - dbl_click_threshold() itself is already throttled internally
// (settingscfg_reload()'s 2s poll), so this comparison is nearly free every
// frame and the syscall fires only when the user actually moves the slider
// (or the kernel default is stale on the first frame after boot).
static void dbl_click_threshold_push(void) {
    static int s_last_pushed = -1;
    int ms = dbl_click_threshold();
    if (ms != s_last_pushed) {
        sys_set_dblclick_ms(ms);
        s_last_pushed = ms;
    }
}

static bool fullscreen_app_on_top(void);   // (local 79) defined below

// #158 NATIVE FULLSCREEN forward declarations - bodies defined near
// render_frame() below (they call cursor_render/apply_display_effects/
// fb_flip, which are declared/defined further down this file), but
// native_fullscreen_watchdog_reset() is called from the F11 handler in
// process_input(), which runs earlier in this file.
static void native_fullscreen_watchdog_reset(void);
static bool native_fullscreen_try_render(void);
// apply_display_effects() is itself only forward-declared right before
// render_frame() (its own definition is later still) - pulled forward here
// too so native_fullscreen_try_render()'s call to it, defined ABOVE that
// point, does not create an implicit (non-static) declaration that then
// conflicts with the real static one.
static void apply_display_effects(void);

static void process_input(void)
{
    // Reset per-frame edge signals
    s_left_pressed  = false;
    s_left_released = false;
    s_right_pressed = false;
    s_dbl_click     = false;
    s_last_key      = -1;

    // Read absolute mouse position from kernel.
    // The kernel's SYS_GET_MOUSE returns absolute screen coordinates
    // via three separate pointer arguments (not a struct).
    int abs_x = 0, abs_y = 0;
    unsigned int buttons = 0;
    get_mouse(&abs_x, &abs_y, &buttons);

    // Detect movement by comparing with previous frame position.
    int mx = abs_x - g_mouse_x;
    int my = abs_y - g_mouse_y;

    // Use the kernel-tracked position directly (already clamped).
#ifdef MAYTERA_TESTHOOK
    // (#123) A throwaway verification build can PIN the pointer. QEMU's
    // relative-mouse injection does not land on this VM class (#334, and
    // re-confirmed for this ticket: a mouse_move to a known dock slot moved
    // nothing), so a hover-magnify screenshot is otherwise unobtainable.
    // This suppresses only the position OVERWRITE - it does not fake a hover,
    // it does not touch the magnify code, and it proves nothing about the
    // mouse DRIVER, which this ticket does not change. Compiled out of every
    // shipping binary along with the rest of the testhook.
    extern int g_th_mouse_pinned;
    if (!g_th_mouse_pinned) { g_mouse_x = abs_x; g_mouse_y = abs_y; }
#else
    g_mouse_x = abs_x;
    g_mouse_y = abs_y;
#endif

    // #236: keep the kernel's title-bar double-click detector (gui/window.c)
    // in sync with the live setting. Cheap most frames (see the function's
    // own comment); only actually reaches the kernel on a real change.
    dbl_click_threshold_push();

    // Derive button edge events
    uint32_t prev = g_mouse_prev_buttons;

    // Suppress button events during the first 2 seconds after compositor
    // start to avoid phantom right-clicks from stale mouse state.
    static uint64_t s_boot_ticks = 0;
    if (s_boot_ticks == 0) s_boot_ticks = (uint64_t)sys_clock();
    bool suppressed = ((uint64_t)sys_clock() - s_boot_ticks) < 500; // 2 seconds at 250 Hz


    // Left button
    if (!suppressed && (buttons & 1) && !(prev & 1)) {
        s_left_pressed = true;

        // Double-click detection. #236: uptime_ms() (real monotonic ms), not
        // sys_clock() (raw 250Hz ticks) - see dbl_click_threshold()'s comment.
        uint64_t now = (uint64_t)uptime_ms();
        if ((now - s_last_click_ms) < (uint64_t)dbl_click_threshold()) {
            s_dbl_click = true;
        }
        s_last_click_ms = now;
    }
    if (!(buttons & 1) && (prev & 1)) {
        s_left_released = true;
    }

    // Right button
    if (!suppressed && (buttons & 2) && !(prev & 2)) {
        s_right_pressed = true;
    }

    g_mouse_buttons      = buttons;
    g_mouse_prev_buttons = buttons;

    // Read up to 8 pending keyboard events per frame.
    // Key releases have bit 7 set (value >= 0x80 for regular keys, or in
    // special ranges). We forward releases to apps but only track presses
    // in s_last_key so that compositor shortcuts fire on the press event.
    bool got_input = (mx != 0 || my != 0 || buttons != prev);
    // T0 #578: classify this tick. Motion = pointer moved; nonmotion starts as
    // a button state change and is raised by any key event / Win16 activity
    // below. A tick that is motion-only takes the cheap cursor-rect present.
    g_tick_motion    = (mx != 0 || my != 0);
    g_tick_nonmotion = (buttons != prev);
    idleprof_motion(mx, my);   // #COMPIDLE: raw device chatter vs real input
    for (int i = 0; i < 8; i++) {
        int key = sys_get_keyboard();
        if (key < 0) {
            break;
        }
        got_input = true;
        g_tick_nonmotion = true;   // a key event can change window content
        // Forward every key (press and release) to app windows - EXCEPT while
        // the session is locked (#566 3.2): a locked session must not leak
        // ANY keystroke to an app window (password characters included). The
        // key is still read above so lock_handle_key() (called from
        // process_events()) can see it; only the forward to the kernel WM is
        // gated.
        // #745 THE ELEVATION MODAL OWNS THE KEYBOARD. Two things happen here
        // and neither of them can be done in process_events():
        //
        //  a) NO sys_inject_key. A trusted prompt whose keystrokes are also
        //     delivered to the app that raised it is worth nothing. This is the
        //     same guarantee the session lock has one line below, and it was
        //     MISSING until a VM run showed Esc closing the modal and ALSO
        //     navigating the App Store back a page.
        //
        //  b) the modal is handed the RAW PRESS, here, inside the read loop.
        //     s_last_key (used by process_events) keeps only the LAST key event
        //     of the frame, and a control character's release code collides
        //     with the special-PRESS range documented below: Enter 0x0A becomes
        //     0x8A, which is one away from F1's 0x88. Press and release arrive
        //     in the same frame for any injected keystroke, so process_events
        //     saw only the release and Enter silently did nothing while letters
        //     and Esc worked.
        if (elevate_modal_open()) {
            // #243: the nav/editing keys (GUI_KEY_HOME..GUI_KEY_DEL, 0x100-0x105)
            // are included so Home/End/Delete work in the prompt's password
            // field. They used to reach it as the letters G/O/S instead.
            if ((key >= 0 && key <= 0x8F) ||
                (key >= GUI_KEY_HOME && key <= GUI_KEY_DEL)) elevate_handle_key(key);
            continue;
        }
        // #745 (task #68) THE GATE. This single call replaced
        // `if (!g_session_locked)`, which was the whole of the suppression
        // list: the session lock, plus the elevation prompt via the `continue`
        // above. Nine more surfaces owned the keyboard and none of them
        // suppressed this forward, so every character typed into the AI
        // launcher, the start-menu search box, a sticky note or a widget
        // settings field was ALSO delivered to the focused app window.
        // g_modal_grabs[] (above) is now the only definition of who owns a
        // key, and process_events() dispatches from that same table.
        if (!modal_key_grab(key)) {
            sys_inject_key(key);
        }
        // Track key-down events for compositor shortcuts.
        // Regular presses: 0x00-0x7F (ASCII chars).
        // Special key presses: 0x80-0x8F (F1-F12, arrows, etc.).
        // Special key releases: 0x90-0x9F (special key + 0x10).
        // Regular key releases: char | 0x80 (>= 0xA0 for printable).
        // We want to process 0x00-0x8F as key-down events, plus the two
        // special-case press codes that live in the release range: 0x9B is
        // KEY_SUPER (kernel cpu/isr.h, #552) -- the Super/GUI/Windows key --
        // and 0x9D is KEY_PRINTSCREEN (#148). Neither could fit in 0x80-0x8F
        // because every byte there is already taken (several of them by more
        // than one meaning; see isr.c).
        // #243: and the nav/editing block at 0x100-0x105, which is ABOVE every
        // byte code listed above. process_events() dispatches s_last_key to
        // whichever modal owns the keyboard (start-menu search, sticky notes,
        // widget settings fields), and those are text fields that want Home/
        // End/Delete. Before #243 they received G/O/S there.
        if ((key >= 0 && key <= 0x8F) || key == 0x9B /* KEY_SUPER */
            || key == 0x9D /* KEY_PRINTSCREEN */
            || (key >= GUI_KEY_HOME && key <= GUI_KEY_DEL)) {
            s_last_key = key;
        }
    }

    // (#200 SkiFree) A running Win16 app (e.g. SkiFree/TETRIS) is the SOLE
    // keyboard consumer: SYS_GET_KEYBOARD returns -1 to us while it owns the
    // foreground, so the loop above never sees a key and our idle timer would
    // run out and the screensaver would black the game out. Treat a foreground
    // Win16 app as continuous activity so the screensaver stays away while one
    // is up (it still arms normally once the Win16 app is closed).
    if (sys_win16_active()) {
        got_input = true;
        g_tick_nonmotion = true;   // Win16 app owns the screen; never cursor-only
    }

    if (got_input) {
        screensaver_on_input();
        g_idle_ms      = uptime_ms();
        // T0 #578: pointer-motion-only input drives rendering via recent_input
        // (g_idle_ms just above) and takes the cheap cursor-rect present path;
        // it must NOT set g_needs_redraw or that path's !g_needs_redraw guard
        // would never fire. Any NON-motion input (keys, buttons, Win16) still
        // forces a full redraw exactly as before. Non-input redraw requests
        // (notifications, UI events) set g_needs_redraw at their own sites and
        // are unaffected.
        if (g_tick_nonmotion) g_needs_redraw = true;
    }

    // #745: clear the OOBE suppression once setup has finished, so the desktop
    // returns WITHOUT a reboot. This is the throttling caller that
    // setup_pending_recheck()'s own comment describes and that never existed:
    // the function was defined, compiled in, and called from nowhere, which
    // left the taskbar, dock, clock and desktop icons suppressed forever after
    // the wizard completed.
    //
    // Throttled because it opens a file. The helper early-outs on its own once
    // the flag is clear, so this costs nothing for the rest of the session; the
    // uptime check exists only to bound the cost WHILE setup is still pending.
    // #203: EITHER flag keeps this poll alive. g_setup_pending ends at Apply,
    // when SETUPDONE lands; g_setup_handover_armed ends at Finish, when the
    // machine actually changes hands. Polling only while g_setup_pending was
    // true is what made the handover watch unreachable.
    if (g_setup_pending || g_setup_handover_armed) {
        static uint64_t s_last_setup_check;
        uint64_t now = uptime_ms();
        if (now - s_last_setup_check >= 500) {
            s_last_setup_check = now;
            bool was_pending = g_setup_pending;
            setup_pending_recheck();
            // Force a real repaint on the TRANSITION (guarded by was_pending,
            // or this would request a redraw every 500ms for the rest of the
            // wizard's life now that the poll outlives g_setup_pending),
            // rather than waiting for the next non-motion input: the whole
            // point is that the desktop comes back by itself.
            if (was_pending && !g_setup_pending) g_needs_redraw = true;
        }
    }
}

// ============================================================================
// launch_app: fork + exec helper (never call sys_putchar in here)
// ============================================================================
static void launch_app(const char *path)
{
    // Use sys_spawn() instead of fork+exec. Forking the compositor process
    // hangs the OS because it duplicates the framebuffer mapping and
    // exclusive input state.
    sys_spawn(path);
}

// ============================================================================
// AI Chat widget lifecycle (#185). The chat is a separate userland app built
// from userland/apps/aichat (source dir + binary name "aichat", lowercase),
// but it is toggled like an in-compositor widget: g_aichat_enabled (widgets.c)
// is the persisted flag. Enable launches the app once; disable writes
// "enabled=0" into /CONFIG/AICHAT.CFG, which the running app polls and then exits.
// We do not have a userland kill syscall, so disable relies on this cooperative
// self-exit. The app's panel width is also stored in that file, so we preserve it.
//
// #371: the ext2 root's static asset base ships this app's ON-DISK entry as
// "/APPS/AICHAT" (uppercase; case preserved from an older asset base per the
// build-golden.sh overlay resolution, same class of trap as #517's COMPOSIT
// vs COMPOSITOR). ext2 is case-sensitive, so a spawn of the lowercase
// "/APPS/aichat" silently fails to find the binary and the panel never
// launches, even though the enabled flag and cfg write both succeed. Spawn
// the ACTUAL on-disk name via AICHAT_APP_PATH (defined near the top of this
// file, before compositor_init's startup auto-launch), not a guessed-case
// literal.
// ============================================================================
/* g_aichat_pid forward-declared above */ static int g_aichat_pid = -1;

// Rewrite /CONFIG/AICHAT.CFG keeping any existing width, with the given enabled.
static void aichat_write_cfg(int enabled)
{
    int width = 380;     // defaults preserved across rewrites
    int position = 0;    // #185: 0=right 1=left 2=top
    // local 66: the panel also persists popped= (docked edge panel vs its own
    // window). MEASURED, not assumed: this rewrite runs at boot BEFORE the app
    // is spawned, and because it re-emits only the keys it knows about, a
    // popped=1 the user had chosen was silently dropped and the panel came back
    // docked every boot. Any key this function does not preserve is a setting
    // this function deletes.
    int popped = 0;
    int fd = userconf_open_read("AICHAT.CFG", "/CONFIG/AICHAT.CFG");  // #683
    if (fd >= 0) {
        char rb[256];
        long n = sys_read(fd, rb, sizeof(rb) - 1);
        sys_close(fd);
        if (n > 0) {
            rb[n] = 0;
            for (int i = 0; rb[i]; i++) {
                if (rb[i]=='w'&&rb[i+1]=='i'&&rb[i+2]=='d'&&rb[i+3]=='t'&&rb[i+4]=='h'&&rb[i+5]=='=') {
                    int j = i + 6, v = 0, any = 0;
                    while (rb[j] >= '0' && rb[j] <= '9') { v = v*10 + (rb[j]-'0'); j++; any = 1; }
                    if (any) width = v;
                } else if (rb[i]=='p'&&rb[i+1]=='o'&&rb[i+2]=='s'&&rb[i+3]=='i'&&rb[i+4]=='t'&&
                           rb[i+5]=='i'&&rb[i+6]=='o'&&rb[i+7]=='n'&&rb[i+8]=='=') {
                    int j = i + 9, v = 0, any = 0;
                    while (rb[j] >= '0' && rb[j] <= '9') { v = v*10 + (rb[j]-'0'); j++; any = 1; }
                    if (any) position = v;
                } else if (rb[i]=='p'&&rb[i+1]=='o'&&rb[i+2]=='p'&&rb[i+3]=='p'&&rb[i+4]=='e'&&
                           rb[i+5]=='d'&&rb[i+6]=='=') {
                    int j = i + 7, v = 0, any = 0;
                    while (rb[j] >= '0' && rb[j] <= '9') { v = v*10 + (rb[j]-'0'); j++; any = 1; }
                    if (any) popped = v ? 1 : 0;
                }
            }
        }
    }
    int wfd = userconf_open_write("AICHAT.CFG");   // #683: per-user, never /etc
    if (wfd < 0) return;
    char buf[128]; int len = 0;
    const char *wk = "width=";
    for (const char *c = wk; *c; c++) buf[len++] = *c;
    { char t[8]; int tn = 0, w = width; if (w <= 0) w = 380;
      while (w) { t[tn++] = '0' + w % 10; w /= 10; } if (tn == 0) t[tn++] = '0';
      while (tn) buf[len++] = t[--tn]; }
    buf[len++] = '\n';
    const char *ek = "enabled=";
    for (const char *c = ek; *c; c++) buf[len++] = *c;
    buf[len++] = enabled ? '1' : '0';
    buf[len++] = '\n';
    const char *pk = "position=";
    for (const char *c = pk; *c; c++) buf[len++] = *c;
    buf[len++] = (char)('0' + (position >= 0 && position <= 2 ? position : 0));
    buf[len++] = '\n';
    const char *dk = "popped=";        // local 66
    for (const char *c = dk; *c; c++) buf[len++] = *c;
    buf[len++] = popped ? '1' : '0';
    buf[len++] = '\n';
    sys_write(wfd, buf, (unsigned long)len);
    sys_close(wfd);
}

// Toggle the AI Chat app on/off. Called from the tray menu (traymenu.c) and at
// startup. Robust: never wedges the compositor; guards every path.
void aichat_set_enabled(int on)
{
    extern int g_aichat_enabled;
    g_aichat_enabled = on ? 1 : 0;
    aichat_write_cfg(g_aichat_enabled);   // app polls this file to self-exit
    if (on) {
        // launch only if we have not already launched a live instance
        if (g_aichat_pid < 0) g_aichat_pid = sys_spawn(AICHAT_APP_PATH);
    } else {
        // cooperative shutdown via the cfg flag above; forget the pid so a
        // subsequent enable relaunches a fresh instance.
        g_aichat_pid = -1;
    }
}

// #387: dock-layout live channel. The Settings app writes a single ASCII digit
// (0..3) to /DOCKSTYL.CFG when the user picks a dock style; the compositor polls
// it and applies the layout live, then profile_tick() persists dock_style into
// UIPROFIL.YML. On boot the compositor writes the current (profile-loaded) value
// back so Settings shows the right selection.
void dock_style_write_cfg(int v) {
    if (v < 0 || v >= DOCK_COUNT) v = 0;  // #26 DOCK_COUNT now 5 (DOCK_XFCE added)
// #683: /DOCKSTYL.CFG was loose in the filesystem ROOT, which is why a uid-1000
// desktop was refused the write. It moves to the per-user location.
//
// CORRECTION to the first read of this file: it is NOT simply a redundant copy
// of UIPROFIL.YML and it cannot just be deleted. UIPROFIL.YML (profile.c:168,
// :308) genuinely owns PERSISTENCE of dock_style, so the persistence half IS
// redundant. But this file is also the live-apply IPC CHANNEL from Settings to
// the running compositor: Settings writes it, main.c polls it every 10 ticks
// and applies the layout without a restart, and the compositor seeds it at boot
// so Settings opens showing the right selection. Deleting it would remove the
// live apply, not just a duplicate. The compositor owns UIPROFIL.YML and
// Settings is a separate process, so Settings has no way to write the profile.
// Relocating is therefore the correct minimum; collapsing the two into one
// mechanism is a real design change and belongs with the other #683b work.
    int fd = userconf_open_write("DOCKSTYL.CFG");
    if (fd < 0) return;
    char c = (char)('0' + v);
    sys_write(fd, &c, 1);
    sys_close(fd);
}
// (#745) dock-opacity live channel, modelled exactly on the dock-style pair
// above: Settings writes an ASCII integer (DOCK_OPACITY_MIN..MAX, percent
// OPAQUE) to DOCKOPAC.CFG, the compositor polls it and applies live, profile.c
// persists it to UIPROFIL.YML, and the compositor seeds the file at boot so
// Settings opens showing what is actually running.
void dock_opacity_write_cfg(int v) {
    // (#132) This clamp only guards against a garbage/out-of-range write; it
    // is NOT the contrast floor any more (that was DOCK_OPACITY_WARN's 70/73
    // predecessors, moved into a Settings-side warning - see dock_opacity.h).
    if (v < DOCK_OPACITY_MIN) v = DOCK_OPACITY_MIN;
    if (v > DOCK_OPACITY_MAX) v = DOCK_OPACITY_MAX;
    int fd = userconf_open_write("DOCKOPAC.CFG");
    if (fd < 0) return;
    char b[4];
    int n = 0;
    if (v >= 100) { b[n++] = '1'; b[n++] = '0'; b[n++] = '0'; }
    else          { b[n++] = (char)('0' + v / 10); b[n++] = (char)('0' + v % 10); }
    sys_write(fd, b, (unsigned long)n);
    sys_close(fd);
}

// (#123) The seed-writers for the two new channels. Deliberately clamped here
// too, not only on read: this file is what SEEDS the CFG Settings opens with,
// so writing an out-of-range value would show the user a number the compositor
// would then refuse.
static void dock_num_write_cfg(const char *name, int v, int lo, int hi) {
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    int fd = userconf_open_write(name);
    if (fd < 0) return;
    char b[8];
    int n = 0;
    if (v >= 100) { b[n++] = (char)('0' + v / 100); v %= 100; b[n++] = (char)('0' + v / 10); b[n++] = (char)('0' + v % 10); }
    else          { b[n++] = (char)('0' + v / 10);  b[n++] = (char)('0' + v % 10); }
    sys_write(fd, b, (unsigned long)n);
    sys_close(fd);
}
void dock_height_write_cfg(int v) { dock_num_write_cfg("DOCKHGT.CFG",  v, 44,  96);  }
void dock_zoom_write_cfg(int v)   { dock_num_write_cfg("DOCKZOOM.CFG", v, 100, 200); }

static void dock_opacity_poll(void) {
    int fd = userconf_open_read("DOCKOPAC.CFG", "/DOCKOPAC.CFG");   // #683 fallback
    if (fd < 0) return;
    char b[8];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = '\0';
    int v = 0, any = 0;
    for (long i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++) { v = v * 10 + (b[i] - '0'); any = 1; }
    if (!any) return;
    // (#132) Was a hard 70/100 clamp ("the derived contrast floor"). That
    // floor is real (see draw.c glass_render()/taskbar.c flat_chrome_alpha()
    // for the measurements) but is now a Settings-side WARNING, not something
    // this poll silently overrides - honour whatever the user actually set,
    // down to DOCK_OPACITY_MIN.
    if (v < DOCK_OPACITY_MIN) v = DOCK_OPACITY_MIN;
    if (v > DOCK_OPACITY_MAX) v = DOCK_OPACITY_MAX;
    if (v != g_dock_opacity) {
        g_dock_opacity = v;
        glass_invalidate_all();   // alpha changed: every cached strip is stale
        g_needs_redraw = true;
    }
}

// (#123) Height + hover-zoom live channels. Same shape as the opacity pair
// above (Settings writes an ASCII integer, the compositor polls and applies,
// profile.c persists), with two differences that matter:
//   - a HEIGHT change moves the desktop work area (taskbar_bottom_inset()
//     follows g_dock_height), so it must push the new struts to the kernel WM
//     and re-clamp icons/widgets. taskbar_apply_work_area() is the ONE function
//     that does all of that; not calling it leaves maximized windows overlapping
//     the dock, exactly as a live style switch would.
//   - a height change also changes the glass surface's geometry, which
//     invalidates the cached strip. glass_render() would notice on its own
//     (geom_same compares w/h), but invalidating explicitly keeps this identical
//     to the opacity path and costs one call on a change that happens at human
//     speed.
static void dock_num_poll(const char *name, const char *legacy, int lo, int hi, int *dst,
                          int *changed) {
    int fd = userconf_open_read(name, legacy);
    if (fd < 0) return;
    char b[8];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = '\0';
    int v = 0, any = 0;
    for (long i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++) { v = v * 10 + (b[i] - '0'); any = 1; }
    if (!any) return;
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    if (v != *dst) { *dst = v; *changed = 1; }
}

static void dock_geom_poll(void) {
    int h_changed = 0, z_changed = 0;
    dock_num_poll("DOCKHGT.CFG",  "/DOCKHGT.CFG",  44,  96,  &g_dock_height, &h_changed);
    dock_num_poll("DOCKZOOM.CFG", "/DOCKZOOM.CFG", 100, 200, &g_dock_zoom,   &z_changed);
    if (h_changed) {
        glass_invalidate_all();     // the dock surface's geometry changed
        taskbar_apply_work_area();  // struts + icon/widget re-clamp
    }
    if (h_changed || z_changed) g_needs_redraw = true;
}

static void dock_style_poll(void) {
    int fd = userconf_open_read("DOCKSTYL.CFG", "/DOCKSTYL.CFG");  // #683
    if (fd < 0) return;
    char c = 0;
    long n = sys_read(fd, &c, 1);
    sys_close(fd);
    if (n != 1) return;
    int v = c - '0';
    if (v < 0 || v >= DOCK_COUNT) return;  // #26 DOCK_COUNT now 5 (DOCK_XFCE added)
    if (v != g_dock_style) {
        taskbar_set_style(v);      // apply live (forces a full redraw)
        g_needs_redraw = true;
    }
}

// #745 P2: widget live-apply channel. This is the prerequisite the first-boot
// wizard's apps/widgets page needs: the wizard is a SEPARATE process spawned
// by this running compositor (see the SETUP spawn above), so it cannot just
// flip g_show_x globals in this process, and it must not write UIPROFIL.YML
// directly - the compositor owns that file, reads it once at launch, and on
// any change to its OWN in-memory globals it full-rewrites the file FROM
// those globals, silently discarding a direct edit the next time anything
// else changes (this already ate a wizard write once, for dock style).
//
// Modelled exactly on dock_style_poll() above: throttled by the caller,
// no-op on a failed open, no busy-wait (#426). WIDGETCH.CFG holds zero or
// more "<bind>=<0|1>" lines, one per line, e.g.:
//   sheep_show=1
//   dog_show=0
// THE KEYS ARE TRAY BINDS, NOT PROFILE KEYS (traymenu.c tm_get/tm_set), which
// is not the same spelling as the profile.c persistence key for two of the
// fifteen: sheep_show/dog_show here vs "sheep"/"dog" in UIPROFIL.YML. Every
// bind is validated against widget_registry() (the single source of truth
// for the fifteen widgets, widgets.c) before being applied, so this channel
// cannot be used to reach a non-widget bind like volume or bt_power.
//
// Applied through traymenu_set_bind() (== tm_set()), never by poking a
// widget_desc_t.flag directly, because tm_set() is what fires show_aichat's
// spawn/stop side effect; setting the flag alone would flip a control that
// renders and does nothing.
//
// All fifteen flags are already terms in profile_tick()'s change-detection
// hash (each on its own prime - see the #745 comment there about what
// sharing a prime does), so applying via traymenu_set_bind() is sufficient
// for the NEXT profile_tick() to notice the change and persist it to
// UIPROFIL.YML on its own ~1s throttle. No new hash term is added here.
//
// Consumed (sys_unlink()'d) after a successful apply so the channel is
// ONE-SHOT: unlike DOCKSTYL.CFG (a level-triggered "current value" channel
// that is meant to keep re-asserting and is never deleted), a leftover
// WIDGETCH.CFG would re-fire every ~330ms and fight a later manual tray
// toggle. A file with no valid bind on it (typo, garbage) is left in place
// rather than silently discarded, so a broken write is at least visible on
// disk instead of vanishing with nothing applied.
static void widgets_cfg_poll(void) {
    int fd = userconf_open_read("WIDGETCH.CFG", 0);   // no legacy: brand-new channel
    if (fd < 0) return;
    char buf[512];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    int wc = 0;
    const widget_desc_t *reg = widget_registry(&wc);
    int applied = 0;
    long i = 0;
    while (i < n) {
        long start = i;
        while (i < n && buf[i] != '\n') i++;
        long end = i;
        if (i < n) i++;   // skip '\n'
        if (end > start && buf[end - 1] == '\r') end--;
        if (end <= start) continue;

        long eq = -1;
        for (long j = start; j < end; j++) { if (buf[j] == '=') { eq = j; break; } }
        if (eq < 0) continue;

        char bind[20];
        int bl = 0;
        for (long j = start; j < eq && bl < (int)sizeof(bind) - 1; j++) bind[bl++] = buf[j];
        bind[bl] = '\0';
        int v = (eq + 1 < end && buf[eq + 1] == '1') ? 1 : 0;

        int valid = 0;
        for (int k = 0; k < wc; k++) { if (!strcmp(reg[k].bind, bind)) { valid = 1; break; } }
        if (!valid) continue;   // not one of the fifteen widget binds: ignore

        traymenu_set_bind(bind, v);
        applied = 1;
    }

    if (applied) {
        char path[256];
        if (userconf_path("WIDGETCH.CFG", path, sizeof(path)) == 0) sys_unlink(path);
    }
}

// ============================================================================
// GLOBALLY_CAPTURED_KEYS (#232) - THE COMPLETE LIST, IN ONE PLACE
// ============================================================================
//
// This existed nowhere before, which is how a bare F1 came to be a system hotkey
// without anyone weighing it against "F1 is Help in every application ever
// written". A global key binding is a claim against EVERY app, EVERY game, the
// DOS interpreter and the Win16 interpreter at once, and none of them can
// negotiate. So the list has to be short, it has to be visible, and every entry
// has to justify itself. Anything not on this list reaches the focused window.
//
// THE RULE: a system-global binding must be a CHORD (Super+x) or a key whose
// ONLY meaning is the system action (PrintScreen, the media keys). A bare
// letter or a bare F-key is never acceptable, for exactly the reason terminals
// put their own shortcuts on Ctrl+Shift rather than on Ctrl.
//
//   KEY              WHERE                          APP ALSO SEES IT?   LIVE FULL-SCREEN?
//   Vol Up/Dn/Mute   kernel cpu/isr.c (#162)        NO, never cooked    YES: never ours
//   Caps Lock        kernel cpu/isr.c               NO, never cooked    YES: never ours
//   PrintScreen      this file, screenshot_hotkey   yes (0x9D is inert) YES: deliberate
//   F11              this file + kernel gui/window  yes                 YES: THE ESCAPE HATCH
//   Super            this file, startmenu_toggle    yes                 no (#232)
//   Super + L        this file, lock_enter (#566)   yes ('l' is sent)   no (#232)
//   Super + Space    this file, launcher_toggle     yes (' ' is sent)   no (#232)
//   Esc              this file, close-an-overlay    yes, unless a modal no overlay can be
//                                                   claims it              open under one
//
// The last column is #232's second half: a FULL-SCREEN APP OWNS THE KEYBOARD.
// See the long note at the top of process_events() for the rule, what counts as
// full screen, and why F11 is the one binding that stays live.
//
// F11 IS THE ONE BARE KEY LEFT, and it is kept deliberately, not by oversight:
// leave-fullscreen is the one action that must work when the fullscreen app
// itself is wedged and never presents again, F11 is the near-universal
// convention for exactly that, and it is what #158 was specified against. It is
// still a bare-key global and therefore still a claim against every app; if a
// title turns up that genuinely needs F11, that is the ticket to reopen, not
// this comment.
//
// THE "APP ALSO SEES IT" COLUMN IS NOT A MISTAKE. process_input() calls
// sys_inject_key() for every key that no modal claims, and it runs BEFORE this
// function, so a global shortcut here is normally an ADDITION to delivery, not
// a replacement for it. That is why removing the F1 binding was the whole fix
// and no new suppression was needed: the app was already getting F1.
//
// NOT COVERED HERE, AND KNOWN: a DOS guest reads the raw scancode tap in
// cpu/isr.c, which is armed on kernel WM focus only (#156) and is NOT informed
// when a compositor OVERLAY (this file's launcher, start-menu search, a sticky
// note) takes the keyboard. So text typed into one of those overlays still
// reaches a focused DOS guest. That is the same "two consumers, one keystroke"
// shape #156 fixed for the unfocused case and it wants the same treatment: the
// compositor telling the kernel that a modal owns the keyboard. Filed, not
// fixed here.
// ============================================================================

// ============================================================================
// process_events: dispatch input to the correct UI layer
// ============================================================================

static void process_events(void)
{
    // #232 A FULL-SCREEN APP OWNS THE KEYBOARD.
    //
    // THE PRINCIPLE, and it is the same one that took the AI launcher off F1: a
    // global hotkey is a claim against every application at once. On a desktop,
    // with a taskbar and other windows visible, "Super opens the start menu" is
    // a fair claim. Over a full-screen game it is not: the key is one the game
    // may want, and firing the binding YANKS THE USER OUT of what they were
    // doing with no warning. So while a full-screen app is on top, the
    // compositor's own bindings stand down and the app gets the keyboard.
    //
    // WHAT COUNTS AS FULL SCREEN. Both mechanisms, because both produce the
    // same situation for the user:
    //   * fullscreen_app_on_top() - the BOUNDS heuristic (#596): a visible,
    //     non-minimised window covering the entire framebuffer. A MAXIMISED
    //     window does not trip this, because maximise leaves the taskbar strip
    //     visible, so its height is short of g_fb_height.
    //   * sys_wm_fullscreen_status() >= 0 - #158 NATIVE fullscreen, the
    //     explicit kernel-tracked state a titlebar button enters.
    //
    // THE ESCAPE HATCH IS F11, and it is deliberately the ONE binding that
    // stays live (see its branch below). It is the near-universal convention
    // for leaving full screen, #158 already specified it as the way out that
    // must work even when the app itself is wedged, and it is handled here in
    // the compositor's own process, above the app. Leaving full screen restores
    // every other binding at once, so the whole rule is "one key gets you out,
    // everything else belongs to the app".
    //
    // ALSO STILL LIVE, and why: PrintScreen (screenshotting a game is a primary
    // use of a screenshot key, the key is never typed into anything, and
    // screenshot_hotkey_fire() already has a dedicated stays-fullscreen path
    // for exactly this case), and the kernel's volume/mute keys, which never
    // enter the cooked ring at all (cpu/isr.c, #162) and so were never the
    // compositor's to stand down from.
    //
    // SUPER IS PRESS-ONLY. cpu/isr.h emits KEY_SUPER (0x9B) on the press and
    // NO release code, ever - measured, and stated in libc/gui_mods.h. So this
    // suppression is written per-PRESS and never waits for a release; code that
    // waited for one would wait forever. It also declines to ARM the chord
    // window, so a Super pressed under a full-screen app cannot combine with an
    // 'l' or a space typed a moment later.
    const bool fs_owns_kbd = fullscreen_app_on_top() ||
                             (sys_wm_fullscreen_status() >= 0);

    // #566: record a Super press timestamp regardless of which branch below
    // ends up consuming it, so the Super+L chord check further down always
    // sees an up-to-date window even if the start menu (or another modal)
    // intercepts the key first.
    if (s_last_key == 0x9B /* KEY_SUPER */ && !fs_owns_kbd) {
        s_super_press_ms = uptime_ms();
    }

    // #566: while the session is locked, ALL input goes to the lock overlay
    // ONLY. No desktop/taskbar/start-menu hit-testing, no sys_inject_mouse()
    // to any app window (the block below that does that lives further down in
    // this function and is skipped entirely by this early return); key
    // forwarding to app windows is already suppressed in process_input().
    // This is the modal-input-capture guarantee 3.2 of the design doc asks
    // for, at the highest possible priority - checked before even the
    // screensaver gate below.
    // #745 (task #68) THE EXCLUSIVE TIER, from g_modal_grabs[]: the session
    // lock first, then the elevation prompt. Each owns the POINTER as well as
    // the keyboard and ends the input tick here - before Super+L, before the
    // screensaver gate, before the launcher and every other overlay - so no
    // app window and no compositor surface below sees anything.
    //
    // This used to be two hand-written early-return blocks, which is exactly
    // how the list of input owners came to differ from the list of things that
    // suppress sys_inject_key(). Both now come from the one table, so a row
    // cannot be in one and missing from the other.
    {
        const modal_grab_t *mg = modal_grab_exclusive();
        if (mg) {
            if (s_last_key >= 0 && mg->handle_key) {
                mg->handle_key(s_last_key);
                s_last_key = -1;
            }
            if (s_left_pressed && mg->handle_mouse) {
                mg->handle_mouse(g_mouse_x, g_mouse_y, 1);
            }
            return;
        }
    }

    // #566 Super+L: highest priority after the lock gate itself, so it fires
    // even if the start menu (opened by the Super press moments earlier) or
    // another overlay would otherwise capture 'l'/'L' first.
    if (s_last_key >= 0 && !fs_owns_kbd &&
        (s_last_key == 'l' || s_last_key == 'L') &&
        s_super_press_ms != 0 &&
        (uptime_ms() - s_super_press_ms) < SUPER_L_WINDOW_MS) {
        s_super_press_ms = 0;
        s_last_key = -1;
        lock_enter();
        return;
    }

    // #232 Super+Space: open/close the AI command launcher (Spotlight). Same
    // priority and same mechanism as Super+L directly above, and deliberately
    // ABOVE the g_modal_grabs[] typing tier, so the space is not typed into the
    // start-menu search field that the Super press moments earlier opened.
    //
    // WHY A CHORD AND NOT A BARE KEY. The launcher used to be on bare F1 (see
    // the long note at its old site further down this function). A single
    // unmodified key cannot be a system-global hotkey on a desktop that runs
    // full-screen games, a DOS interpreter and a Win16 interpreter: every one
    // of those has its own meaning for the F-keys and no way to negotiate.
    // Super is the one key nothing in an app's key map claims, which is why
    // the start menu, the lock chord and now this all hang off it.
    //
    // The Super press ITSELF still toggles the start menu (that is Super's
    // documented job, #552), so this closes the menu that press opened before
    // showing the launcher, the same way lock_enter() force-closes it.
    if (s_last_key == ' ' && !fs_owns_kbd && s_super_press_ms != 0 &&
        (uptime_ms() - s_super_press_ms) < SUPER_L_WINDOW_MS) {
        s_super_press_ms = 0;
        s_last_key = -1;
        if (g_start_menu_open) startmenu_toggle();
        launcher_toggle();
        return;
    }

    // While the screensaver is active, suppress all UI events
    if (screensaver_check_timeout()) {
        return;
    }

    // #148: PrintScreen takes an immediate screenshot, UNCONDITIONALLY,
    // regardless of which (if any) modal owns the keyboard - checked here,
    // before the g_modal_grabs[] typing-tier walk below, the same
    // "outranks every overlay" priority Super+L uses above. A held text
    // field (start-menu search, a sticky note, the AI launcher, ...) cannot
    // mistake byte 0x9D for a printable character, so there is nothing
    // PrintScreen could corrupt by not being swallowed; making "does my
    // screenshot key work" depend on unrelated UI state would be a worse
    // bug than the one this fixes. It IS still gated by the lock-screen
    // (exclusive tier, returns above this function) and the screensaver
    // check immediately above, matching every other shortcut - neither of
    // those is something a keystroke should be able to capture through.
    // Deliberately NOT added to g_modal_grabs[]: that table decides who
    // receives TYPED input, and this key is never typed anywhere. Capturing
    // with a menu or dropdown open is also a useful #141 cross-check (stale
    // dropdown pixels left on the framebuffer after a menu switch): the shot
    // should show what is really composited, stuck pixels included.
    if (s_last_key == 0x9D /* KEY_PRINTSCREEN, kernel cpu/isr.h */) {
        // #148 (local 164, 2026-08-18) OWNER'S FULL SPEC: PrintScreen is
        // ALWAYS a fire-and-forget full-screen capture (no drag/region UI is
        // ever reached from this hotkey - that stays an in-app Snapshot mode,
        // entered only via its own "New" + rectangle tool). What happens
        // AFTER the capture depends on fullscreen (#158) state, and
        // screenshot_hotkey_fire() (screenshot.c) makes that call itself:
        //   A. Native-fullscreen on top: STAYS fullscreen, shows the
        //      bottom-right thumbnail toast (see native_fullscreen_try_render()
        //      below for where that toast actually gets drawn).
        //   B. Otherwise: opens Maytera Snap on the capture, bottom-left,
        //      without stealing focus.
        // ORDER-SENSITIVE regardless of which path: screenshot_hotkey_fire()
        // captures BEFORE reading fullscreen status, so the shot always shows
        // whatever #158's own SYS_WM_FULLSCREEN_RENDER most recently wrote
        // into g_fb (the app's real content), never a desktop recomposite -
        // and path A never calls anything that changes fullscreen state, so
        // there is nothing later in this same key handler that could race it.
        screenshot_hotkey_fire();
        s_last_key = -1;
    }

    // Keyboard: global shortcuts
    if (s_last_key >= 0) {
        int key = s_last_key;
        const modal_grab_t *mg_kbd = modal_grab_keyboard();

        // #232: THE AI LAUNCHER IS NO LONGER ON F1. It used to be, right here,
        // as `if (key == 0x88 /* F1 */) { launcher_toggle(); }` - an
        // UNCONDITIONAL global capture, ahead of the modal walk and ahead of
        // every app. F1 is not a spare key: it is Help in essentially every
        // application ever written (libhelp's own HELP_UI_KEY_F1 is F1), and it
        // is a plain game key - DOS Joust prints "F1: START GAME" on its title
        // screen and reaches its start-a-game routine ONLY through its INT 9
        // hook on scancode 0x3B. Pressing it opened the AI prompt panel over
        // the top of the game every single time. The binding moved to
        // Super+Space, the same shape as macOS Spotlight's Cmd+Space and
        // Windows' Win+S, and for the same reason terminals put their shortcuts
        // on Ctrl+Shift: a global consumer must use a chord an ordinary
        // application will never send. The taskbar Maytera-logo button is
        // unchanged and is still the pointer route to the same launcher_toggle().
        //
        // #745 (task #68) THE DISPATCH WALK. This one branch replaced a
        // seven-deep else-if chain (launcher, power-confirm, item-properties,
        // icon picker, widget settings, sticky editor, start menu) that had to
        // be kept in step BY HAND with the suppression list in
        // process_input(). It never was: dispatch had seven entries and
        // suppression had one. Both now read g_modal_grabs[], in table order,
        // so the surface that gets the key is by construction the surface that
        // stopped the app from getting it too.
        if (mg_kbd != NULL) {
            if (mg_kbd->handle_key) mg_kbd->handle_key(key);
            s_last_key = -1;
        } else {

        // Function-key shortcuts removed per user request: F11 was opening the
        // start menu and F7 (0x8A) was launching the Terminal, both firing by
        // accident. Use the taskbar start button and the menu/desktop to launch
        // apps. (F5 settings launcher removed too for consistency.)


        // F11 (#158). CHECKED before landing this ticket: F11 was already
        // bound to sys_wm_maximize_focused() (below) - this is not a fresh
        // binding, it is an ADDITIONAL branch ahead of it. The owner's own
        // wording ("F11 to move back to windowed mode") only specifies the
        // EXIT direction, so that pre-existing maximize toggle is left
        // running unchanged for the non-fullscreen case: there is a reason
        // not to make this a full symmetric toggle (an existing, working
        // binding), so ENTER stays button-only (kernel titlebar chrome).
        // Handled HERE, above any per-app routing, in the compositor's own
        // input loop - a SEPARATE Ring-3 process from whatever app is
        // fullscreen, so this keeps working even if that app is wedged and
        // never presents again (the ticket's hard requirement: F11 must be
        // handled ABOVE the app).
        if (key == 0x85) {
            if (sys_wm_fullscreen_status() >= 0) {
                sys_wm_fullscreen_exit();
                native_fullscreen_watchdog_reset();
            } else {
                sys_wm_maximize_focused();
            }
            s_last_key = -1;
        }

        // Super/Windows/GUI key (#552): toggle the compositor start menu.
        // Same action, same function as the taskbar start button and the
        // old ESC-closes-it path below: startmenu_toggle() is the single
        // code path, so this is not a second parallel start-menu mechanism.
        // #232: NOT while a full-screen app owns the keyboard. This is the
        // binding the owner named ("disable the windows key when we have any
        // full screen app open") and it is the clearest case: opening the start
        // menu over a game covers it, takes the keyboard for the search field,
        // and is almost never what the person pressing it meant.
        if (key == 0x9B /* KEY_SUPER, kernel cpu/isr.h */ && !fs_owns_kbd) {
            startmenu_toggle();
            s_last_key = -1;
        }

        // ESC closes any open overlay (kernel maps ESC to 0x1B). The start
        // menu's own ESC handling now lives in startmenu_handle_key() (it
        // clears an in-progress search first, then closes), reached via the
        // g_start_menu_open branch above, so it is not repeated here.
        if (key == 0x1B /* ESC */) {
            if (g_wallpaper_picker_open) {
                wallpaper_picker_close();
            } else if (g_context_menu_open) {
                contextmenu_close();
            }
        }

        // (#73) THE DEAD KEY SINK WAS HERE, and the comment above it was wrong in
        // the way that matters: "post-login this is a no-op" was the reason nobody
        // looked. It was NOT a no-op. login_handle_key() ran for every key that
        // reached this fallthrough, and the compositor's own login state machine
        // was left armed in LOGIN_STATE_SELECT_USER by login_init(), so a single
        // '1'..'9' (there are 3 accounts on the golden) moved it to
        // LOGIN_STATE_PASSWORD and every printable key after that was appended to
        // a plaintext g_password[] belonging to a screen that is never drawn and
        // whose only reset path, login_run()'s loop, was commented out. Enter then
        // called sys_authenticate() for real.
        //
        // The fix is DELETION, not a guard: a guard on dead code is two things to
        // maintain and one of them is a lie. There is nothing left to forward to.
        // If a compositor-owned surface ever needs keys again it joins
        // g_modal_grabs[] above (the #68 registry), which is the ONE definition of
        // who owns a keystroke - it does not get a second, ungated call site here.
        }   // end else (widget Settings dialog not open)
    }

    // Mouse event processing: work top-to-bottom through the UI stack.
    // Each handler returns true if it consumed the event.
    bool consumed = false;

    // #745: the desktop chrome is not DRAWN while setup is pending, so it must
    // not be CLICKABLE either. Drawing was gated at 13 sites and input at none,
    // which left invisible chrome hit-testing its old screen regions and eating
    // clicks meant for the window on top of it. Only the region hit-tests need
    // this: the popup handlers below are already guarded by their own "is open"
    // state, and none of them can be opened while the chrome is suppressed.
    const bool chrome_live = !g_setup_pending;

    // (local 79) SAME BUG, SECOND CAUSE: the taskbar strip is not DRAWN while a
    // true-fullscreen app is on top (render_frame_body gates taskbar_render(),
    // startmenu_render(), traymenu_render(), clock_render() and the taskbar tile
    // menu on `fs = fullscreen_app_on_top()`), but it was still HIT-TESTED.
    // Worse, the tile hitboxes in taskbar.c (g_tb_btn_*) are recorded DURING
    // RENDER, so while a game is up they are whatever the bar last looked like
    // before it went fullscreen: stale rectangles for an invisible bar.
    //
    // The consequence is the reported flashing titlebar. A click anywhere in
    // the bottom strip - which for a fullscreen game is just part of the game
    // viewport, e.g. firing at something low on the screen - reaches
    // taskbar_handle_mouse(), lands on the game's own stale taskbar tile, and
    // taskbar.c's Windows-style toggle reads g_tb_btn_focused[i] == 1 and
    // MINIMIZES the focused window. The game then re-asserts focus on its next
    // SDL_PollEvent (sdlshim.cpp calls wm_focus every pump), which restores and
    // re-focuses it. Focused -> minimized -> focused is exactly green -> grey ->
    // green, once per click.
    //
    // Same rule and same reasoning as the #745 setup-pending gate directly
    // above, and it reads the SAME predicate the renderer uses rather than a
    // second copy, so "drawn" and "clickable" cannot disagree again.
    // #232: reuses the fs_owns_kbd computation from the top of this function
    // rather than asking again. Two queries a frame that must agree is how
    // "drawn" and "clickable" drifted apart in the first place.
    const bool bar_live = chrome_live && !fs_owns_kbd;

    // Desktop pet: once grabbed, keep dragging until the button is released,
    // then let it fall under gravity onto the taskbar.
    if (s_dragging_sheep) {
        if (g_mouse_buttons & 1) { sheep_drag_to(g_mouse_x, g_mouse_y); }
        else { sheep_release(); s_dragging_sheep = false; }
        consumed = true;
    } else if (s_dragging_dog) {
        if (g_mouse_buttons & 1) { dog_drag_to(g_mouse_x, g_mouse_y); }
        else { dog_release(); s_dragging_dog = false; }
        consumed = true;
    } else if (s_dragging_widget) {
        if (g_mouse_buttons & 1) { widget_drag_to(g_mouse_x, g_mouse_y); }
        else { widget_release(); s_dragging_widget = false; profile_save(); }
        consumed = true;
    } else if (s_dragging_settings) {
        // #419b: dragging the settings modal by its title bar (or its list scrollbar).
        if (g_mouse_buttons & 1) { widget_settings_drag_to(g_mouse_x, g_mouse_y); }
        else { widget_settings_drag_end(); s_dragging_settings = false; }
        consumed = true;
    } else if (s_dragging_sticky) {
        if (g_mouse_buttons & 1) { stickies_drag_to(g_mouse_x, g_mouse_y); }
        else { stickies_release(); s_dragging_sticky = false; }
        consumed = true;
    } else if (s_dragging_desktop) {
        // Desktop icon drag or rubber-band selection in progress.
        if (g_mouse_buttons & 1) { desktop_drag(g_mouse_x, g_mouse_y); }
        else { desktop_release(g_mouse_x, g_mouse_y); s_dragging_desktop = false; }
        consumed = true;
    }

    // The command launcher (Spotlight) is modal: while open it captures every
    // click (run a suggestion, or click outside to cancel) before anything else.
    if (!consumed && g_launcher_open) {
        launcher_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed ? 1 : 0);
        consumed = true;
    }

    // Start-menu power/session confirm dialog and item-Properties popup are
    // true modals (Cancel/confirm buttons or ESC only - never click-away), so
    // they get first crack at clicks, same priority as the launcher above.
    if (!consumed && startmenu_power_confirm_open()) {
        consumed = startmenu_power_confirm_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }
    if (!consumed && startmenu_properties_open()) {
        consumed = startmenu_properties_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }
    if (!consumed && taskbar_force_quit_confirm_open()) {
        consumed = taskbar_force_quit_confirm_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }
    // #44 "Change Icon" file picker: also a true modal, same priority.
    if (!consumed && iconpicker_is_open()) {
        consumed = iconpicker_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    // #419b: while the settings modal is open, the mouse wheel scrolls its
    // (HA) entity list. Read + consume the scroll here so it does not also
    // reach the window manager underneath the modal.
    if (!consumed && widget_settings_is_open()) {
        int sd = get_mouse_scroll();
        if (sd != 0) widget_settings_handle_scroll(g_mouse_x, g_mouse_y, sd);
    }

    // The widget Settings dialog (modal) captures clicks while open. A press on
    // the title bar starts a modal drag; a press on the list scrollbar starts a
    // scroll drag; anything else is a normal control click (#419b).
    if (!consumed && widget_settings_is_open() && s_left_pressed) {
        if (widget_settings_press(g_mouse_x, g_mouse_y)) s_dragging_settings = true;
        consumed = true;
    }

    // An open per-widget menu captures the next click (select item or dismiss).
    if (!consumed && widget_menu_is_open() && (s_left_pressed || s_right_pressed)) {
        widget_menu_handle(g_mouse_x, g_mouse_y, 1);
        consumed = true;
    }

    if (!consumed && g_wallpaper_picker_open) {
        consumed = wallpaper_picker_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    // #563: mouse wheel over the start menu's row list or an open category
    // flyout scrolls it (both are height-capped and may overflow) - same
    // "read + consume the scroll here" idiom as the widget-settings modal
    // above.
    if (!consumed && g_start_menu_open) {
        int sd = get_mouse_scroll();
        if (sd != 0) startmenu_handle_scroll(g_mouse_x, g_mouse_y, sd);
    }

    if (!consumed && g_start_menu_open) {
        // A right-click on an item opens its Pin/Add-to-Desktop/Properties
        // context menu; anything else (left-click, hover, category headers,
        // the power grid) goes through the normal handler.
        if (s_right_pressed) {
            consumed = startmenu_handle_right_click(g_mouse_x, g_mouse_y);
        }
        if (!consumed) {
            consumed = startmenu_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
        }
    }

    if (!consumed && g_context_menu_open) {
        consumed = contextmenu_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    if (!consumed && g_tray_menu_open) {
        consumed = traymenu_handle_mouse(g_mouse_x, g_mouse_y,
                                         s_left_pressed, (g_mouse_buttons & 1) != 0);
    }

    // #168: notification toasts / center get first crack at clicks.
    if (chrome_live && !consumed) {
        consumed = notif_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    // #241: while the performance popup is open, it gets first crack at the
    // click (anywhere on screen) so it can be dismissed or acted on.
    if (bar_live && !consumed && taskbar_popup_active()) {
        consumed = taskbar_popup_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    // Taskbar-tile right-click menu (Close): an already-open menu gets first
    // crack at any click (item pick or click-away dismiss); otherwise a
    // right-press on a tile opens it. Both must run BEFORE the plain
    // taskbar_handle_mouse() below - it unconditionally consumes every click
    // inside the taskbar strip once its own left-click handling has run, so
    // without this a right-click on a tile was silently swallowed with no
    // menu and no other effect.
    if (bar_live && !consumed && taskbar_menu_is_open()) {
        consumed = taskbar_menu_handle(g_mouse_x, g_mouse_y, s_left_pressed || s_right_pressed);
    }
    if (bar_live && !consumed && s_right_pressed) {
        consumed = taskbar_handle_right_click(g_mouse_x, g_mouse_y);
    }

    if (bar_live && !consumed) {
        consumed = taskbar_handle_mouse(g_mouse_x, g_mouse_y, s_left_pressed);
    }

    // --- Forward mouse to the kernel window manager (app windows) ---
    // Under exclusive mode the kernel desktop loop does not process input, so we
    // relay mouse activity into the kernel WM for window dragging, title-bar
    // buttons (minimize/maximize/close), resize grips, and click-to-focus.
    //
    // MOVE and UP are injected unconditionally so an in-progress drag/resize
    // keeps tracking and always terminates, even if the cursor passes over the
    // taskbar or desktop. The kernel WM ignores MOVE/UP when no drag is active,
    // so this is harmless. DOWN is only injected when no compositor overlay or
    // the taskbar has already consumed the click; if a window consumes the DOWN,
    // we mark the event consumed so the desktop-icon layer does not also fire.
    {
        static int s_prev_mx = -1;
        static int s_prev_my = -1;

        // Cross-window drag: refresh the ghost from the kernel session. This
        // is the ONLY per-frame cost the docking protocol adds to the
        // compositor, and it self-gates on the button mask BEFORE making any
        // syscall, so an idle desktop pays one integer test. It reads the same
        // g_mouse_x/g_mouse_y about to be injected below, so the ghost cannot
        // lag the pointer by a frame.
        dragghost_poll(g_mouse_x, g_mouse_y, (unsigned int)g_mouse_buttons);

        if (g_mouse_x != s_prev_mx || g_mouse_y != s_prev_my) {
            sys_inject_mouse(g_mouse_x, g_mouse_y, MOUSE_EVENT_MOVE, 0);
            s_prev_mx = g_mouse_x;
            s_prev_my = g_mouse_y;
        }

        // OS-wide mouse wheel: read the kernel scroll delta and dispatch an
        // EVENT_MOUSE_SCROLL to the window under the cursor so any app that
        // handles scrolling (browser, files, chat transcript, ...) responds.
        {
            int sd = get_mouse_scroll();
            if (sd != 0) {
                sys_inject_mouse(g_mouse_x, g_mouse_y, MOUSE_EVENT_SCROLL, sd);
            }
        }

        if (!consumed && s_left_pressed) {
            if (sys_inject_mouse(g_mouse_x, g_mouse_y, MOUSE_EVENT_DOWN, 1) > 0) {
                consumed = true;
            }
        }

        if (s_left_released) {
            sys_inject_mouse(g_mouse_x, g_mouse_y, MOUSE_EVENT_UP, 1);
            // Cross-window drag: the release is what commits a drop. Deliberately
            // AFTER the injection above, so a target window receives the ordinary
            // EVENT_MOUSE_UP it would have got anyway BEFORE it is told a payload
            // is waiting. No-op when no drag is in flight.
            //
            // The kernel resolves the drop target itself from this point; the
            // compositor cannot name one. That is what keeps a drop from being
            // redirected to a window that is not really under the cursor.
            dragghost_release(g_mouse_x, g_mouse_y);
        }

        // Right-button press: route to the window under the cursor so the app
        // can show its own context menu. If a window is hit, mark the event
        // consumed so the desktop context menu does NOT also open (#87: no more
        // right-clicking "through" an app onto the desktop).
        if (!consumed && s_right_pressed) {
            if (sys_inject_mouse(g_mouse_x, g_mouse_y, MOUSE_EVENT_DOWN, 2) > 0) {
                consumed = true;
            }
        }
    }

    // Sticky notes (#270): a press on a note edits/drags/closes it. Notes sit in
    // the desktop widget layer (behind app windows), so this runs after the WM
    // injection but before framework widgets / sheep / desktop icons.
    if (chrome_live && !consumed && s_left_pressed && stickies_hit(g_mouse_x, g_mouse_y) >= 0) {
        if (stickies_press(g_mouse_x, g_mouse_y)) {
            if (stickies_is_dragging()) s_dragging_sticky = true;
            consumed = true;
        }
    }
    // A left-press that misses every note still commits an in-progress edit.
    else if (chrome_live && !consumed && s_left_pressed && stickies_editing()) {
        stickies_press(g_mouse_x, g_mouse_y);   // hit<0 path: deselect + save
    }

    // Relocatable desktop widgets: grab a widget if the press landed on it and no
    // window/overlay/taskbar consumed it first (widgets sit behind app windows).
    if (chrome_live && !consumed && s_left_pressed && widget_hit(g_mouse_x, g_mouse_y) >= 0) {
        widget_grab(g_mouse_x, g_mouse_y);
        s_dragging_widget = true;
        consumed = true;
    }

    // Grab the sheep if the press landed on it (and no window/overlay took it).
    if (chrome_live && !consumed && s_left_pressed && dog_hit(g_mouse_x, g_mouse_y)) {
        dog_grab(g_mouse_x, g_mouse_y);
        s_dragging_dog = true;
        consumed = true;
    }
    if (chrome_live && !consumed && s_left_pressed && sheep_hit(g_mouse_x, g_mouse_y)) {
        sheep_grab(g_mouse_x, g_mouse_y);
        s_dragging_sheep = true;
        consumed = true;
    }

    if (chrome_live && !consumed) {
        // Right-click on a desktop widget opens ONLY its menu (Hide/Lock/Settings).
        // Previously the desktop layer ALSO opened its context menu on the same
        // right-click, so both popped up at once. Now: if the cursor is over a
        // widget, open the widget menu and do not run the desktop handler;
        // otherwise the desktop layer handles icons + its own right-click menu.
        int wh = s_right_pressed ? widget_hit(g_mouse_x, g_mouse_y) : -1;
        if (wh >= 0) {
            widget_menu_open(wh, g_mouse_x, g_mouse_y);
        } else if (s_left_pressed) {
            // Begin an icon drag or a rubber-band selection. The drag is then
            // tracked frame-to-frame at the top of process_events via
            // s_dragging_desktop; release decides click vs drag (and launches
            // on double-click within the window).
            if (desktop_press(g_mouse_x, g_mouse_y)) {
                s_dragging_desktop = true;
            }
        } else {
            // Right-click (context menu) and kernel double-click launches.
            desktop_handle_mouse(g_mouse_x, g_mouse_y,
                                 s_left_pressed, s_right_pressed, s_dbl_click);
        }
    }
}

// ============================================================================
// Window drop shadows (#160, Phase 4b) - drawn by the compositor onto the
// desktop layer BEFORE the kernel paints app windows on top, so each window
// casts a soft shadow into the desktop margin around its bottom/right edges.
// Gated on MODERN themes; Classic (theme id 4) draws nothing.
// ============================================================================

int g_compositor_theme_id = 1;   // last theme applied (set by compositor_apply_theme)

// True-fullscreen app detection: a visible, non-minimized window whose bounds
// cover the ENTIRE framebuffer (incl. the bottom dock strip window_maximize
// leaves at fb_h-80). A maximized normal window stops at fb_h-80, so it never
// matches -> it keeps the taskbar. Used to suppress the desktop chrome so a
// fullscreen game (Maytera Arena) covers the whole screen.
// Shared predicate over an ALREADY-FETCHED window list, so callers that have
// just called wm_get_windows() (the main loop) do not pay for a second syscall
// and, more importantly, cannot drift from this definition. (#596)
static bool fullscreen_in_list(const wm_window_info_t *wins, int n) {
    for (int i = 0; i < n; i++) {
        const wm_window_info_t *w = &wins[i];
        if (!w->visible || w->minimized) continue;
        if (w->x <= 0 && w->y <= 0 &&
            w->x + w->width  >= (int)g_fb_width &&
            w->y + w->height >= (int)g_fb_height)
            return true;
    }
    return false;
}

// Setup wizard modality. The first-boot wizard used to be spawned as an
// ordinary window onto a fully usable, already-autologged-in root desktop:
// taskbar, Start menu and clickable desktop icons all live behind it. That is
// not an out-of-box experience, it is a dialog on someone else's session, and
// it defeats the point of the wizard - anyone could ignore setup entirely and
// keep using the root desktop it was meant to replace.
//
// While the machine is UNCONFIGURED, the desktop is therefore suppressed the
// same way it is for a true-fullscreen app (#596): no taskbar, no Start menu,
// no dock, no clock, no icons, no version overlay. Reuses the existing
// suppression chokepoint rather than adding a second mechanism.
bool g_setup_pending = false;

// #203: ARMED WHEN THE FIRST-BOOT WIZARD IS SPAWNED, AND DELIBERATELY NOT THE
// SAME FLAG AS g_setup_pending.
//
// g_setup_pending means "hide the chrome, this machine is unconfigured", and it
// is cleared the moment /CONFIG/SETUPDONE appears. The wizard writes SETUPDONE
// at the end of apply(), which runs when Continue is pressed on the LAST page -
// one page BEFORE Finish. The first version of this handover watched for the
// Finish marker from inside setup_pending_recheck(), which the input tick only
// calls while g_setup_pending is true, so the watch stopped existing before the
// event it was watching for could happen. It never fired once, on a machine
// where every other part of the change worked.
//
// "The machine is unconfigured" and "the first-run wizard is still on screen"
// are two facts that end at two different moments. One flag cannot carry both.
bool g_setup_handover_armed = false;

// Re-read the marker cheaply; the wizard writes it as its LAST action, so this
// is how the desktop comes back without a reboot. Throttled by the caller.
//
// #136 (owner ticket, userland/apps/setup/main.rs's "Skip to Desktop" corner
// control): SETUPDONE means "genuinely configured, never show the wizard
// again". SETUPSKIP is deliberately a SEPARATE, weaker marker meaning "let
// the chrome through for the rest of THIS boot only" - the wizard writes it
// instead of SETUPDONE precisely so the next boot still finds SETUPDONE
// absent and respawns /APPS/SETUP. See the boot-time spawn site below
// (search g_setup_pending = true) for the other half of this contract: it
// deletes any stale SETUPSKIP before arming g_setup_pending, so a marker
// left behind by a skipped session can never be mistaken for consent to
// skip a LATER boot's wizard.
void setup_pending_recheck(void) {
    // THE HANDOVER WATCH, checked FIRST and gated on its OWN flag so that
    // clearing g_setup_pending (which happens one page early, at Apply) cannot
    // switch it off. See g_setup_handover_armed above.
    if (g_setup_handover_armed) {
    // =======================================================================
    // #203 (owner report) / #126 Part A: A LOGIN IS A SESSION BOUNDARY, AND
    // FINISHING THE FIRST-RUN WIZARD IS A LOGIN.
    //
    // OWNER REPORT, verbatim: "in the first run wizard we created a user
    // (james) and then it logged us in as root?" Reproduced on golden 2007:
    // every write the wizard makes SUCCEEDS and PERSISTS - the account is
    // created, LOGIN.CFG is written, the next boot honours it - and the
    // machine is STILL root's, because the wizard runs INSIDE the session that
    // shipped autologin=root and Finish only exits the SETUP process. The
    // compositor beneath it never went anywhere, so every app the owner opens
    // in the minutes after setup runs as uid 0.
    //
    // THE HANDOVER IS THE ONE THE MACHINE ALREADY HAS. Log Out is a plain
    // sys_exit(0) (startmenu.c case 3); gui/desktop.c notices the pid is gone
    // and returns; main.c's #566 login-gate loop re-runs login_init() +
    // login_run(), which RE-EVALUATES login_check_autologin() within the same
    // boot. Measured on the #203 reproduction machine:
    //
    //     compositor: Log Out -> clean exit for login re-entry
    //     [Desktop] Compositor (pid 21) exited; returning to login gate
    //     [LOGIN] Auto-login as james (uid=1001)
    //
    // No reboot and no new syscall.
    //
    // WHY /CONFIG/SETUPNEW AND NOT /CONFIG/SETUPDONE. SETUPDONE is written at
    // the END OF the wizard's apply(), which runs when Continue is pressed on
    // the last page - BEFORE the Done page is drawn. Keying this exit off
    // SETUPDONE takes the desktop away about half a second after Apply and the
    // person never sees the Done page or presses Finish. SETUPNEW is written
    // as the LAST thing Finish does, and only in the first-boot flow, so it
    // means exactly "the machine just changed hands". UNLINKED BEFORE THE
    // EXIT: a marker that survives is a marker that logs somebody out on a
    // later boot for no reason (the #136 stale-SETUPSKIP failure, one file
    // over).
    //
    // WHY THIS BELONGS ON TOP OF #126 PART A AND NOT ON ITS OWN. Exiting here
    // without Part A's session teardown would hand the incoming user the
    // OUTGOING user's live, focusable, ROOT-OWNED windows - the
    // privilege-escalation path #126 measured (a standard user driving a
    // root-owned Settings process into Users & Accounts). Part A stamps the
    // compositor as session leader and SIGKILLs the departing session at the
    // gate, which is what makes this exit safe rather than merely convenient.
    // That is why #203's standalone five-line version was not taken and this
    // branch was landed instead.
    // =======================================================================
    //
    // #229 UPDATE: THE MARKER IS NO LONGER A FILE, AND THE PARAGRAPH ABOVE IS
    // WHY IT SHOULD NEVER HAVE BEEN ONE.
    //
    // Read it again: SETUPNEW means "for this boot only", and the note about
    // unlinking it before the exit is a stale-marker cleanup bolted onto a
    // per-boot signal that was being stored on persistent media. The same
    // paragraph one file over describes #136, the identical bug with
    // SETUPSKIP. Worse, both the marker and its cleanup are writes to /CONFIG,
    // which is root-owned 0711 - so on a non-root session (every session since
    // #226 removed autologin=root) the wizard could not raise the signal AND
    // this could not clear it.
    //
    // SYS_FIRSTRUN(FR_HANDOVER_TAKE) is a CONSUMING read of one bit of kernel
    // state that starts clear at every boot because .bss starts clear at every
    // boot. There is no marker to leave behind, so there is no cleanup to get
    // right and no permission for it to fail on. See kernel/rustkern/
    // firstrun.rs.
        if (sys_firstrun(FR_HANDOVER_TAKE) == 1) {
            g_setup_pending = false;
            g_setup_handover_armed = false;
            sys_bootlog("compositor: first-run wizard finished -> clean exit to hand the session over (#203/#126)");
            sys_exit(0);
            return;   // not reached
        }
    }
    if (!g_setup_pending) return;
    // No handover marker: the wizard is done (or was skipped) but this session
    // is not changing hands, so only un-suppress the desktop. This is the
    // pre-#203 behaviour and it stays the behaviour on every path that is not
    // a first-boot Finish - a crashed wizard, a Skip to Desktop, or the #126
    // reduced personalisation flow.
    int fd = sys_open("/CONFIG/SETUPDONE", 0);
    if (fd >= 0) { sys_close(fd); g_setup_pending = false; return; }
    // #229: the escape hatch, now a kernel flag rather than /CONFIG/SETUPSKIP.
    // A uid-1000 wizard could not create that file, so "Skip to Desktop" told
    // the person "Could not reach the desktop; try again." and meant it
    // permanently: the one control that exists for when a step fails had the
    // same failure mode as the step. FR_SKIP_GET is a plain read of a bit and
    // cannot refuse. SETUPDONE is still a file because it is a DURABLE machine
    // fact; the difference is the whole point of the split.
    if (sys_firstrun(FR_SKIP_GET) == 1) { g_setup_pending = false; }
}

static bool fullscreen_app_on_top(void) {
    // NOTE: do NOT fold setup-pending into this predicate. It does not only hide
    // chrome - exclusive/fullscreen mode also changes INPUT ROUTING (a fullscreen
    // game reads input directly instead of through the window event queue), so
    // returning true here made the setup wizard stone deaf to keys and clicks.
    // Chrome suppression for setup is done at the render call sites instead.
    wm_window_info_t wins[16];
    int n = wm_get_windows(wins, 16);
    return fullscreen_in_list(wins, n);
}

// ============================================================================
// #158 NATIVE FULLSCREEN.
//
// Distinct from fullscreen_app_on_top() above (#596): that predicate is a
// BOUNDS heuristic (any window covering the whole fb hides desktop chrome)
// and still runs the full compositor_render_windows() kernel composite every
// frame. This is an EXPLICIT, kernel-tracked, user-invoked state (the
// titlebar button or F11) for exactly ONE window at a time, and it replaces
// the WHOLE render_frame_body() layer stack - wallpaper, icons, widgets, and
// the per-window chrome/corner/widget draw loop SYS_COMPOSITOR_RENDER_WINDOWS
// runs - with one direct kernel blit (SYS_WM_FULLSCREEN_RENDER). That redraw
// cost, MEASURED at 256-272 blits/s and 424-474 MB/s with two windows open
// (#153 instrumentation), is what this feature exists to cut to ~zero.
//
// THE WAY BACK lives mostly in the KERNEL (kernel/gui/window.c: focus loss,
// window close/crash, session lock, screensaver latch - see the #158 block
// there), because #157 proved a display handed away must be reclaimable even
// if the process that asked for it is wedged. This file adds the two paths
// that can only live in userland: F11 (compositor's own input loop, ABOVE
// whatever app is fullscreen) and the watchdog below (a live liveness signal
// the kernel has no reason to police on its own clock).
// ============================================================================

// #158 WATCHDOG. Same shape as #157's g_fb_flip_count-vs-elapsed-time
// discriminator: compare a monotonic "the app did real work" counter
// (SYS_WM_FULLSCREEN_STATUS's packed window-id+commit-sequence) against
// wall-clock time. An advancing counter is a live app; a stalled one is
// recovered from. Much shorter than #157's 45s boot grace - this is a LIVE
// desktop being taken over, not a one-shot boot handoff, so a wedged app
// should be recovered from quickly rather than leaving the user staring at a
// dead frame with no visible way out other than knowing to press F11 (which,
// per the ticket, must ALSO keep working - this is the belt to that brace).
#define NATIVE_FS_WATCHDOG_MS 8000
static int64_t  s_fs_watch_key       = -1;   // last (id<<32|commit_seq) observed
static uint64_t s_fs_watch_change_ms = 0;    // uptime_ms() it last changed

static void native_fullscreen_watchdog_reset(void) {
    s_fs_watch_key = -1;
    s_fs_watch_change_ms = 0;
}

// true = watchdog fired and already called sys_wm_fullscreen_exit().
static bool native_fullscreen_watchdog_tick(int64_t status) {
    uint64_t now = uptime_ms();
    if (status != s_fs_watch_key) {
        s_fs_watch_key = status;
        s_fs_watch_change_ms = now;
        return false;
    }
    if (s_fs_watch_change_ms == 0) { s_fs_watch_change_ms = now; return false; }
    if (now - s_fs_watch_change_ms >= NATIVE_FS_WATCHDOG_MS) {
        notify_post("Fullscreen App Not Responding",
                    "The app stopped presenting frames; the desktop was restored.",
                    NOTIFY_WARNING);
        sys_bootlog("compositor: #158 fullscreen watchdog fired (no content "
                    "commit for NATIVE_FS_WATCHDOG_MS) - restoring the desktop");
        sys_wm_fullscreen_exit();
        native_fullscreen_watchdog_reset();
        return true;
    }
    return false;
}

// Gate: the fast path only runs when NOTHING ELSE needs the screen. Reuses
// g_modal_grabs[] (the #745 "THE DISPATCH WALK" table, main.c above) rather
// than re-deriving "is anything open" a second way, plus the two overlay
// flags that table deliberately excludes (setup wizard - see the NOTE on
// fullscreen_app_on_top(); tray menu - consumes no keystroke, so the table
// never listed it).
static bool any_modal_or_overlay_open(void) {
    extern bool g_setup_pending;
    if (g_setup_pending || g_tray_menu_open) return true;
    for (int i = 0; i < MG_COUNT; i++) {
        if (g_modal_grabs[i].is_open && g_modal_grabs[i].is_open()) return true;
    }
    return false;
}

// The fast path. Returns true if it painted and presented a frame (caller
// does nothing else this tick); false if the caller must run a normal
// composite instead - which is what visibly restores the desktop, whether
// because nothing is fullscreen, something else needs the screen, or the
// kernel already self-healed the claim away.
static bool native_fullscreen_try_render(void) {
    int64_t status = sys_wm_fullscreen_status();
    if (status < 0) { native_fullscreen_watchdog_reset(); return false; }

    if (any_modal_or_overlay_open()) {
        // #158: anything else that needs the screen reclaims it outright
        // (Super key, launcher, wallpaper picker, ...) rather than racing an
        // ambiguous "fullscreen app behind a menu" frame. The requested
        // overlay then renders on the normal (now windowed) desktop.
        sys_wm_fullscreen_exit();
        native_fullscreen_watchdog_reset();
        return false;
    }

    if (native_fullscreen_watchdog_tick(status)) return false;

    if (sys_wm_fullscreen_render() != 0) {
        // Kernel declined: self-healed away this frame (focus lost, window
        // closed, owner died). Fall through to the normal composite, which
        // repaints the real desktop.
        native_fullscreen_watchdog_reset();
        return false;
    }

    // #148 (local 164): the PrintScreen fullscreen toast. Drawn AFTER the
    // app's own fresh frame just landed in g_fb and BEFORE the cursor, so it
    // sits on top of real current content and under the pointer, every tick
    // it is active. screenshot_fs_toast_draw() no-ops instantly when the
    // toast is not armed (screenshot_fs_toast_active() check inside it), so
    // this costs nothing on every other frame. See screenshot.c's toast
    // module comment for why this must NOT go through notif.c/a resumed
    // composite, and why no separate erase step is needed - the row-blit
    // above already overwrites this exact region from scratch every time
    // native_fullscreen_try_render() succeeds.
    screenshot_fs_toast_draw();

    cursor_render();
    apply_display_effects();
    idleprof_flip();   // #COMPIDLE: timed present
    perfframe_mark("fullscreen");   // perf62: #158 native-fullscreen fast path
    return true;
}


// #596: last uptime_ms() at which a TRUE-FULLSCREEN app window presented a
// frame. screensaver.c reads this: a game/GL app that is actively rendering is
// NOT idle, even with no keyboard/mouse input for minutes, so the screensaver
// must not black it out. Zero = no fullscreen app has ever presented.
uint64_t g_fs_present_ms = 0;
extern int g_draw_blend;          // 0-255 alpha for draw_* (defined in draw.c)

// ============================================================================
// #745: OPT-IN SOFT DROP SHADOW, drawn by the compositor because only the
// compositor owns the pixels it lands on.
//
// WHY HERE AND NOT IN THE KERNEL WM. A shadow is entirely OUTSIDE the window
// rectangle. The kernel WM draws nothing there, and cannot: this is a software
// compositor with one shared surface, and by the time SYS_COMPOSITOR_RENDER_-
// WINDOWS runs, THIS process has already painted the whole layer stack
// (wallpaper -> icons -> widgets -> stickies) into the back buffer. So at this
// point, layer 3c, the destination pixels in the shadow band are already final
// and correct, and darkening them in place is the entire job.
//
// That is also exactly why the #27 corner machinery in kernel/gui/window.c is
// NOT the thing to extend, despite being the same family of problem. Its
// snapshot/restore (wm_capture_corners / window_punch_corners) exists for ONE
// reason: the window is about to paint OVER those pixels, so they have to be
// saved first and put back after. Nothing ever paints over the shadow band, so
// there is nothing to snapshot and nothing to restore. What DOES carry over
// from #27 is win_covered_above()'s rule, and it is reproduced below as
// shadow_cut_row(): a pixel another visible window occupies is left alone, so
// one window's shadow can never punch through the window on top of it. Here it
// is load-bearing for a second reason, see DAMAGE below.
//
// SCOPE: OPT-IN, TWO GATES.
//   1. Per window: WINDOW_FLAG_SHADOW (SYS_WIN_SET_SHADOW). #189 removed the
//      blanket app-window drop shadow by an explicit user decision, so turning
//      it back on for every window would be re-litigating a settled call. The
//      wizard asks for one; nothing else does.
//   2. Per theme: radius.window, for CHROMED windows only.
// Both live in ONE place, kernel window.c window_wants_shadow(), next to the
// #27 corner logic they share a theme property with; wm_window_info_t.shadow is
// its already-decided answer. This file deliberately re-derives none of it, so
// there is no second copy of the policy to drift. Read that function before
// changing anything about WHICH windows get a shadow; this file only decides
// WHERE the pixels go.
// A (near-)fullscreen window is skipped HERE, because that is geometry rather
// than policy: its band would be off-screen or a letterbox stripe.
//
// DAMAGE. The band is outside the window rect, so it is not covered by any of
// the window's own repaint accounting. Three paths reach the framebuffer:
//   * render_frame(): full composite, band included by construction.
//   * render_frame_cursor(): CLIPPED, but it calls the SAME render_frame_body()
//     so this function runs inside the clip, over a wallpaper that was just
//     repainted in that same clip. Blending is therefore never applied twice to
//     one pixel. Its `need_windows` test only looks at window RECTS, so a
//     cursor rect that touches the band but no window skips the (unclipped)
//     kernel window composite - which is safe ONLY because shadow_cut_row()
//     refuses to write inside any window rect. Without that, this path would
//     paint shadow onto card pixels nothing was going to repaint.
//   * render_frame_idle(): reached only with zero visible windows, so there is
//     no shadow to draw. What it must not do is leave a stale band behind after
//     the last window closes - see the s_prev_apps fix at the render gate in
//     main(), which is where residue actually came from.
//
// COST. No per-pixel falloff maths at run time: shadow_build_ramp() fills a
// 33-entry quadratic ramp and a corner table once, and every pixel after
// that is one table read plus one blend inside draw_hspan_alpha(). Zero cost
// when nothing opted in (one integer test per window) and zero when the theme
// is square (one metric read for the whole frame).
//
// GEOMETRY is docs/OOBE_GLASS_CARD.html section 2, DERIVED not transcribed:
// spread 32, peak alpha 0.35 (89/255), quadratic falloff, offset y +6. For that
// table's 688x616 card at (CX,CY) this yields x = CX-32, y = CY+6-32 = CY-26,
// w = 688+64 = 752, h = 616+64 = 680, which is the spec row exactly.
//
// #745 CORNER FIX (2026-08-11). The window this shadow is cast for is a sharp
// rectangle (that is what SYS_WIN_CREATE actually allocates); the OOBE card
// drawn inside it FAKES a rounded look by sampling the wallpaper behind its
// own CARD_R corner box (setup/main.rs, card_pixel()). The original falloff
// measured Euclidean distance from the window's own sharp corner POINT
// (sh_isqrt(dx*dx+dy*dy)), which is exactly correct for a square window and
// visibly wrong for this one: it produces a shadow whose corner silhouette is
// a quarter-circle anchored at a point CARD_R pixels outside where the card's
// own arc actually curves away, so the shadow reads as square-cornered
// "around" the card's genuinely rounded corner - precisely the defect
// reported ("square corners... AROUND the correctly rounded corners").
//
// THE FIX measures distance to the CARD's own rounded corner instead: a
// circle of radius SHADOW_CORNER_R centered SHADOW_CORNER_R pixels inside
// each of the window's four corners (the Minkowski-sum offset of a rounded
// rect grown by the shadow spread). For a query point (qx,qy) pixels outside
// the window on both axes, distance to that arc is
// sh_isqrt((qx+R)^2+(qy+R)^2) - R, not sh_isqrt(qx^2+qy^2).
//
// THE DEGENERACY THE NAIVE SUBSTITUTION BREAKS, and why this is a table
// restructure and not a formula swap. The straight-edge cases are not "qx=0"
// and "qy=0" as the old single 2D table implied: they are "more than R pixels
// from the NEAREST corner along this edge". A row within R pixels of ry0/ry1
// is near a corner even though its OWN qy (distance outside the window) is 0,
// because the round arc's own tangent point is R pixels in from the corner,
// not 0. The old table conflated "outside distance on this axis" with
// "nearness to a corner", which is only the same thing far from any corner.
// shadow_axis_q() below produces a single measure, Yq (or Xq), that is valid
// in BOTH regimes: for a row/column genuinely outside the window it is the
// outside distance plus R (matching the corner formula's shifted center); for
// a row/column still inside the window's own span it is R minus the distance
// to the nearest edge on that axis (positive only within R pixels of a
// corner, <=0 once safely in the middle of a long edge). shadow_draw_flank()
// and shadow_draw_body() below then branch on sign: Yq<=0 (or Xq<=0) reduces
// EXACTLY to the old plain ramp (s_sh_ramp[qx] / s_sh_ramp[qy], proved below
// to be numerically identical, not merely similar), and only the genuinely
// near-a-corner region reads the new s_sh_diag[][] arc table. The straight
// run down the middle of a tall/wide edge (more than R pixels from either
// corner) is therefore UNCHANGED code and UNCHANGED output; only the R-pixel
// band flanking each corner - on both the diagonal AND along the two edges
// that meet there - now follows the card's arc instead of the window's point.
// ============================================================================

#define SHADOW_SPREAD    32   // spec: falloff to zero over 32 px
#define SHADOW_PEAK      89   // spec: peak alpha 0.35 -> 0.35 * 255 = 89.25
#define SHADOW_OFFX       0   // spec: no horizontal offset
#define SHADOW_OFFY       6   // spec: offset y +6
// #745: must match the OOBE wizard's CARD_R / CORNER_BOX (userland/apps/
// setup/main.rs) - the radius of the arc this shadow's corner is meant to
// follow. Hardcoded, not read back from the app, for the same reason
// SHADOW_SPREAD/PEAK/OFFY already are: this feature exists for exactly one
// caller (see SCOPE above) and there is no window-info channel for a
// per-window corner radius. If a second window ever opts into a shadow with
// a different visible corner radius, this needs to become a real field on
// wm_window_info_t rather than a second hardcoded constant.
#define SHADOW_CORNER_R  16

static uint8_t s_sh_ramp[SHADOW_SPREAD + 1];   // by edge distance, straight-edge case
static int     s_sh_ready = 0;
// Largest distance whose 8-bit alpha still rounds to something. With peak 89
// and a quadratic ramp the last ~4 px round to 0, so walking the full 32 costs
// rows of loop iterations that can only write nothing. DERIVED from the tables
// in shadow_build_ramp(), not a magic number, so changing SHADOW_PEAK or
// SHADOW_SPREAD cannot leave a stale constant behind.
static int     s_sh_vis = SHADOW_SPREAD;

// s_sh_diag[Yq][Xq]: alpha for the near-a-corner case, arc-corrected. Indexed
// 1..SH_AXQ_MAX on each axis (see shadow_axis_q(); 0 is never a valid index
// here, that value always routes to the plain ramp instead - see
// shadow_draw_flank()/shadow_draw_body()).
#define SH_AXQ_MAX (SHADOW_SPREAD + SHADOW_CORNER_R)
static uint8_t s_sh_diag[SH_AXQ_MAX + 1][SH_AXQ_MAX + 1];

static int sh_isqrt(int v)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static void shadow_build_ramp(void)
{
    if (s_sh_ready) return;
    const int S = SHADOW_SPREAD;
    const int R = SHADOW_CORNER_R;
    for (int d = 0; d <= S; d++) {
        int t = S - d;                                  // S at the edge, 0 at the outside
        s_sh_ramp[d] = (uint8_t)((SHADOW_PEAK * t * t) / (S * S));
    }
    // Distance from a point (Xq,Yq) axis-shifted by R to the arc's own
    // center (see the block comment above): sh_isqrt(Xq^2+Yq^2) - R. At
    // Xq==R or Yq==R exactly (a query point right at the window's own sharp
    // corner point) this is sh_isqrt(2*R^2)-R, a genuine reduction from the
    // old d=0 there - that reduction IS the fix.
    for (int yq = 1; yq <= SH_AXQ_MAX; yq++)
        for (int xq = 1; xq <= SH_AXQ_MAX; xq++) {
            int d = sh_isqrt(xq * xq + yq * yq) - R;
            if (d < 0) d = 0;
            s_sh_diag[yq][xq] = (d >= S) ? 0 : s_sh_ramp[d];
        }
    s_sh_vis = 0;
    for (int d = 0; d <= S; d++) if (s_sh_ramp[d]) s_sh_vis = d;
    s_sh_ready = 1;
}

// The unified "how close to a corner is this axis" measure described above.
// raw_outside is 0 when the row/column is within the window's own span on
// this axis, else the distance outside it. inside_edge_dist is only read
// when raw_outside==0: the distance from this row/column to the NEARER of
// the window's two edges on this axis (0 at the very edge row/column).
static inline int shadow_axis_q(int raw_outside, int inside_edge_dist)
{
    return (raw_outside > 0) ? (raw_outside + SHADOW_CORNER_R)
                              : (SHADOW_CORNER_R - inside_edge_dist);
}

// One flank span (left or right of the window): every pixel here is outside
// the window on the x axis, so Xq = qx_raw + SHADOW_CORNER_R always (see
// shadow_axis_q). qx_raw walks by `dir` (-1 left flank going rightward, +1
// right flank going rightward) exactly as the old ramp pointer did.
static inline void shadow_draw_flank(int x0, int x1, int y, int Yq, int qx_raw0, int dir, uint32_t SH)
{
    if (x0 > x1) return;
    if (Yq <= 0) {
        // Numerically identical to the pre-fix table: s_sh_corner[0][qx] was
        // always sh_isqrt(qx*qx)==qx exactly, i.e. s_sh_ramp[qx]. Untouched
        // straight-edge case, proved equal rather than merely re-derived.
        draw_hspan_alpha(x0, y, x1 - x0 + 1, SH, &s_sh_ramp[qx_raw0], dir);
    } else {
        draw_hspan_alpha(x0, y, x1 - x0 + 1, SH, &s_sh_diag[Yq][qx_raw0 + SHADOW_CORNER_R], dir);
    }
}

// The body span (directly above/below the window): split at SHADOW_CORNER_R
// from each side so the long middle run keeps the plain vertical ramp
// (byte-identical to the old ramp[0] constant) and only the two end zones,
// within R of a corner, read the arc table.
static inline void shadow_draw_body(int x0, int x1, int y, int Yq, int rx0, int rx1, uint32_t SH)
{
    if (x0 > x1) return;
    const int R = SHADOW_CORNER_R;
    int vd = Yq - R;                                    // Yq<=0 defensive case folds in here too
    uint8_t mid_a = (vd <= 0) ? s_sh_ramp[0] : ((vd >= SHADOW_SPREAD) ? 0 : s_sh_ramp[vd]);

    // Absolute (unclipped) boundaries of the three zones, split at R from
    // each side. leftEnd/rightStart can cross (window narrower than 2R);
    // clamp both to the window's own midpoint so the two end zones meet
    // instead of overlapping - a defensive path, not exercised by the
    // 688px-wide OOBE card this feature exists for.
    int mid       = (rx0 + rx1) / 2;
    int leftEnd   = rx0 + R - 1; if (leftEnd   > mid) leftEnd   = mid;
    int rightStart = rx1 - R + 1; if (rightStart <= mid) rightStart = mid + 1;

    int lc0 = x0, lc1 = (x1 < leftEnd) ? x1 : leftEnd;
    if (lc0 <= lc1) {
        if (Yq <= 0) {
            if (mid_a) draw_hspan_alpha(lc0, y, lc1 - lc0 + 1, SH, &mid_a, 0);
        } else {
            int xq0 = R - (lc0 - rx0);                  // R at rx0, decreasing rightward
            if (xq0 < 1) xq0 = 1;                        // clamped domain when leftEnd was pulled to mid
            draw_hspan_alpha(lc0, y, lc1 - lc0 + 1, SH, &s_sh_diag[Yq][xq0], -1);
        }
    }
    int mc0 = (x0 > leftEnd + 1) ? x0 : leftEnd + 1;
    int mc1 = (x1 < rightStart - 1) ? x1 : rightStart - 1;
    if (mc0 <= mc1 && mid_a)
        draw_hspan_alpha(mc0, y, mc1 - mc0 + 1, SH, &mid_a, 0);
    int rc0 = (x0 > rightStart) ? x0 : rightStart, rc1 = x1;
    if (rc0 <= rc1) {
        if (Yq <= 0) {
            if (mid_a) draw_hspan_alpha(rc0, y, rc1 - rc0 + 1, SH, &mid_a, 0);
        } else {
            int xq0 = R - (rx1 - rc0);                  // decreasing toward rx1
            if (xq0 < 1) xq0 = 1;
            draw_hspan_alpha(rc0, y, rc1 - rc0 + 1, SH, &s_sh_diag[Yq][xq0], +1);
        }
    }
}

// Free-x-interval list for one scanline: the band minus every window rect that
// covers this row. Same rule as kernel win_covered_above(), in screen space.
#define SH_MAXFREE 20
static int s_sh_free[SH_MAXFREE][2];
static int s_sh_nfree;

static void shadow_row_init(int x0, int x1)
{
    s_sh_nfree = 0;
    if (x0 <= x1) { s_sh_free[0][0] = x0; s_sh_free[0][1] = x1; s_sh_nfree = 1; }
}

static void shadow_row_cut(int cx0, int cx1)
{
    int out[SH_MAXFREE][2], no = 0;
    for (int i = 0; i < s_sh_nfree; i++) {
        int a = s_sh_free[i][0], b = s_sh_free[i][1];
        if (cx1 < a || cx0 > b) {
            if (no < SH_MAXFREE) { out[no][0] = a; out[no][1] = b; no++; }
            continue;
        }
        if (a < cx0 && no < SH_MAXFREE) { out[no][0] = a;       out[no][1] = cx0 - 1; no++; }
        if (b > cx1 && no < SH_MAXFREE) { out[no][0] = cx1 + 1; out[no][1] = b;       no++; }
    }
    for (int i = 0; i < no; i++) { s_sh_free[i][0] = out[i][0]; s_sh_free[i][1] = out[i][1]; }
    s_sh_nfree = no;
}

static void windows_render_shadows(void)
{
    wm_window_info_t wins[16];
    int n = wm_get_windows(wins, 16);
    if (n <= 0) return;

    // Cheapest possible early-out on the normal desktop, where nothing has
    // opted in: w->shadow is the kernel's already-decided eligibility answer.
    int any = 0;
    for (int i = 0; i < n && !any; i++)
        if (wins[i].shadow && wins[i].visible && !wins[i].minimized) any = 1;
    if (!any) return;

    shadow_build_ramp();
    const int S = s_sh_vis;   // outer ring of alpha-0 pixels is not worth walking
    const uint32_t SH = 0xFF000000;   // black; alpha comes from the tables

    // Front-first list, so iterate backwards to paint back to front.
    for (int i = n - 1; i >= 0; i--) {
        wm_window_info_t *w = &wins[i];
        if (!w->shadow || !w->visible || w->minimized) continue;
        if (w->width < 8 || w->height < 8) continue;
        // Maximized / (near-)fullscreen: band is off-screen or a letterbox stripe.
        if (w->width  >= g_fb_width  - 2) continue;
        if (w->height >= g_fb_height - 2) continue;

        // Shadow body = the window rect translated by the spec offset.
        int rx0 = w->x + SHADOW_OFFX, ry0 = w->y + SHADOW_OFFY;
        int rx1 = rx0 + w->width - 1, ry1 = ry0 + w->height - 1;
        int bx0 = rx0 - S, bx1 = rx1 + S;
        int by0 = ry0 - S, by1 = ry1 + S;

        // Clip early-out, then clamp the row/column walk to the live clip so a
        // small dirty-rect repaint does not iterate 680 rows to draw 24.
        if (bx1 < g_clip_x0 || bx0 >= g_clip_x1) continue;
        if (by1 < g_clip_y0 || by0 >= g_clip_y1) continue;
        int wx0 = bx0 < g_clip_x0 ? g_clip_x0 : bx0;
        int wx1 = bx1 > g_clip_x1 - 1 ? g_clip_x1 - 1 : bx1;
        int wy0 = by0 < g_clip_y0 ? g_clip_y0 : by0;
        int wy1 = by1 > g_clip_y1 - 1 ? g_clip_y1 - 1 : by1;

        for (int y = wy0; y <= wy1; y++) {
            int qyraw = (y < ry0) ? (ry0 - y) : ((y > ry1) ? (y - ry1) : 0);
            if (qyraw > S) continue;   /* S <= SHADOW_SPREAD, so the table index is in range */

            // #745: Yq folds qyraw (outside the window) AND, when this row is
            // still within the window's own vertical span, its distance to
            // the nearer top/bottom edge - see shadow_axis_q()'s comment.
            int edgeY = (y - ry0 < ry1 - y) ? (y - ry0) : (ry1 - y);
            int Yq = shadow_axis_q(qyraw, edgeY);

            shadow_row_init(wx0, wx1);
            // Every visible window occludes, including this one: its own rect is
            // painted by the kernel composite, and in a clipped partial repaint
            // that composite may legitimately be skipped.
            for (int k = 0; k < n && s_sh_nfree > 0; k++) {
                wm_window_info_t *o = &wins[k];
                if (!o->visible || o->minimized) continue;
                if (y < o->y || y >= o->y + o->height) continue;
                shadow_row_cut(o->x, o->x + o->width - 1);
            }

            for (int f = 0; f < s_sh_nfree; f++) {
                int a = s_sh_free[f][0], b = s_sh_free[f][1];

                // Left flank: qx = rx0 - x, so alpha walks the table BACKWARDS.
                int la = a, lb = (b < rx0 - 1) ? b : rx0 - 1;
                if (la <= lb) {
                    int qx0 = rx0 - la;                       // largest qx at the left end
                    if (qx0 > S) { la += qx0 - S; qx0 = S; }
                    if (la <= lb)
                        shadow_draw_flank(la, lb, y, Yq, qx0, -1, SH);
                }
                // Body: near-corner ends get the arc table, the long middle
                // run keeps the plain vertical ramp (shadow_draw_body splits it).
                int ma = (a > rx0) ? a : rx0, mb = (b < rx1) ? b : rx1;
                shadow_draw_body(ma, mb, y, Yq, rx0, rx1, SH);
                // Right flank: qx = x - rx1, alpha walks the table FORWARDS.
                int ra = (a > rx1 + 1) ? a : rx1 + 1, rb = b;
                if (ra <= rb) {
                    int qx0 = ra - rx1;
                    if (qx0 <= S) {
                        int lim = rx1 + S;
                        if (rb > lim) rb = lim;
                        if (ra <= rb)
                            shadow_draw_flank(ra, rb, y, Yq, qx0, +1, SH);
                    }
                }
            }
        }
    }
}

// ============================================================================
// render_frame: composite all layers and present to screen
// ============================================================================

static void apply_display_effects(void);

// render_frame_body: the full back-to-front desktop layer stack (everything
// except the exclusive lock / screensaver overlays and the final present).
// Factored out of render_frame() so the T0 #578 partial-present paths
// (render_frame_cursor) draw the EXACT same layers in the EXACT same order,
// just clipped to a small rect. Keeping a single copy is what prevents the
// classic dirty-rect stale-pixel bug (a layer added to render_frame but
// forgotten in the partial path). `draw_windows` lets the cursor path skip the
// unclipped kernel window composite when no window intersects the damage rect.
static void render_frame_body(bool draw_windows)
{
    // Layer order (back to front):
    wallpaper_render_background();   // 1. Background / wallpaper
    if (!g_setup_pending) {
        desktop_render();            // 2. Desktop icons
        desktop_render_version();    // 3. Version string overlay
    }
    if (!g_setup_pending) widgets_render();  // 3b. Desktop widgets (clock/calendar/pet)
    stickies_render();               // 3b2. Desktop sticky notes (#270)
    windows_render_shadows();        // 3c. opt-in drop shadows (#745; blanket shadows stay off per #189)
    if (draw_windows)
        compositor_render_windows(); // 4. Kernel draws app windows on our FB
    desktop_render_overlay();        // 4b. Rubber-band selection rectangle
    // Fullscreen app on top -> hide desktop chrome (taskbar/start/dock/clock).
    bool fs = fullscreen_app_on_top();

    if (!fs && !g_setup_pending) taskbar_render();        // 5. Taskbar bar (above windows)

    if (!fs && !g_setup_pending && g_start_menu_open) {
        startmenu_render();          // 6. Start menu (above taskbar)
    }

    if (g_context_menu_open) {
        contextmenu_render();        // 7. Context menu
    }

    if (startmenu_properties_open()) {
        startmenu_properties_render();   // 7b. Start-menu item Properties popup (modal)
    }

    if (startmenu_power_confirm_open()) {
        startmenu_power_confirm_render(); // 7c. Power/session confirm dialog (modal)
    }

    if (taskbar_force_quit_confirm_open()) {
        taskbar_force_quit_confirm_render(); // 7d. #745 Force Quit confirm dialog (modal)
    }

    if (iconpicker_is_open()) {
        iconpicker_render();          // 7d. #44 "Change Icon" file picker (modal)
    }

    if (g_wallpaper_picker_open) {
        wallpaper_render_picker();   // 8. Wallpaper picker dialog
    }

    if (!fs && g_tray_menu_open) {
        traymenu_render();           // 8b. Tray quick-settings menu (above taskbar)
    }

    if (!fs && !g_setup_pending) clock_render();  // 9. Floating clock widget
    if (!fs && !g_setup_pending) widget_menu_render();  // 9b. Per-widget right-click menu
    if (!fs) taskbar_menu_render();  // 9b2. Taskbar-tile right-click menu (Close)
    if (!fs && !g_setup_pending) widget_settings_render();// 9c. Per-widget Settings dialog
    if (!fs) launcher_render();      // 9c2. Command launcher (Spotlight) overlay
    notif_render();                  // 9d. Notification toasts + center (#168)
    volosd_render();                 // 9d2. #162 volume OSD (self-gating)
    dragghost_render();              // 9d3. cross-window drag ghost. Above the
                                     //     windows AND the taskbar, because a
                                     //     tab can be dragged over both, but
                                     //     below the elevation modal, which
                                     //     must stay over everything.
    elevate_render();                // 9e. #745 elevation modal: scrim + panel
                                     //     over EVERYTHING, drawn last before
                                     //     the cursor so nothing can cover it.
    cursor_render();                 // 10. Hardware cursor drawn last
    apply_display_effects();         // 10b. Brightness / night-light
}

// perf62: render_frame()'s own full-composite present (below) is reached from
// several main-loop callers whose REASON for wanting a full frame differs -
// #62's premise is that a windowed-app redraw and a plain desktop interaction
// are different paths, so they must not collapse into one bucket. The caller
// sets this immediately before calling render_frame(); it defaults to
// "interactive" so a stray call (there is exactly one other call site, the
// redundant screensaver-active call in the main loop, which never reaches
// the fb_flip() this tags - see the g_screensaver_active early-return above)
// never mis-tags anything.
static const char *s_pf_full_reason = "interactive";

static void render_frame(void)
{
    // #102/#379: a full frame always composites the whole screen; reset any clip
    // left over from an idle dirty-rect present.
    draw_clear_clip();

    // #151: tell the kernel to stop blitting user windows straight to the FB
    // whenever ANYTHING owns the whole display that a punch-through could leak
    // through - the screensaver AND the lock screen alike. This used to be
    // checked only below, AFTER the g_session_locked early-return a few lines
    // down, so it never fired for lock at all: the kernel kept compositing the
    // last-focused window's own redraws (a blinking text-field cursor, a live
    // value, any win_invalidate()) straight onto hardware on ITS OWN cadence,
    // independent of this compositor's fb_flip() of the lock overlay. Whatever
    // was on screen at lock time (e.g. an open Settings window) could then
    // still repaint ON TOP of the lock screen after every one of its own
    // redraws - reported as "I can see the window of the settings app behind
    // the clock" - on every dock style and theme, since the missing call has
    // nothing to do with rendering/CSS. Moved here (same set_win_blank()
    // primitive the screensaver already used - see below), and its condition
    // widened to cover both exclusive-display states, so it fires no matter
    // which branch below is taken this frame.
    static bool s_blank_prev = false;
    bool want_blank = g_screensaver_active || g_session_locked;
    if (want_blank != s_blank_prev) {
        set_win_blank(want_blank ? 1 : 0);
        s_blank_prev = want_blank;
    }

    // #566: the lock overlay is exclusive - while locked, render ONLY it.
    // Checked before even the screensaver branch below, so the screensaver
    // never activates (or keeps animating) over a locked session; the lock
    // screen's own darkened/blurred backdrop serves the same "screen off"
    // purpose, and this keeps there being exactly one idle-driven overlay
    // active at a time instead of two competing ones.
    if (g_session_locked) {
        lock_render();
        // #162: the lock screen is one of the places the ticket explicitly
        // names, and this branch returns before the normal overlay tail, so
        // the OSD has to be drawn here too or the volume keys would work with
        // no feedback exactly where the user can see least.
        volosd_render();
        cursor_render();
        apply_display_effects();
        idleprof_flip();   // #COMPIDLE: timed present
        perfframe_mark("lock");   // perf62: lock-screen present, not a scored path
        // #62: the lock screen exclusively owns the display this tick, so
        // chrome/windowed/interactive/cursor/fullscreen are all structurally
        // unreachable below - note it so a long lock session is not later
        // read as a stall on whichever of THOSE paths presents next.
        perfframe_note_quiescent();
        return;
    }

    // Screensaver takes over the full display
    if (g_screensaver_active) {
        // #652 BLANK-AFTER STAGE. Even at the #650 SS_FRAME_MIN_MS-throttled
        // rate (~15fps), a GL/plasma screensaver left running for hours (a
        // machine idle overnight) burns CPU forever - MEASURED ~14% of a
        // core on VM <vmid> / golden 1016 (11.6 flips/sec, top idle:85
        // COMPOSIT:14), not the 34% an earlier report claimed, but not zero
        // either, and zero is achievable because nobody is looking by then.
        // Once the screensaver has been running CONTINUOUSLY (real on-screen
        // time via screensaver_active_since_ms(), not the pre-activation
        // idle timer) for SS_BLANK_AFTER_MS, stop calling screensaver_render()
        // entirely: paint exactly one black frame on the transition, then on
        // every later tick just do this cheap uptime_ms() comparison and
        // return - no gldemo_frame()/plasma math, no cursor_render(), no
        // fb_flip(). That is the same order of cost as the idle-desktop path
        // (#564), which MEASURED shows flips not advancing at all. Waking is
        // untouched: any input still calls screensaver_on_input() (main.c,
        // unconditionally on got_input) which flips g_screensaver_active
        // false and sets g_needs_redraw, so the NEXT tick takes the normal
        // desktop composite branch below, not this one - it restores the
        // real desktop, never the screensaver.
        static bool s_ss_blanked = false;
        uint64_t since_active = uptime_ms() - screensaver_active_since_ms();
        if (since_active >= SS_BLANK_AFTER_MS) {
            if (!s_ss_blanked) {
                draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF000000);
                idleprof_flip();   // #COMPIDLE: timed present
                perfframe_mark("screensaver");   // perf62: one-shot blank transition
                s_ss_blanked = true;
            }
            // #62: this is the actual mechanism behind the 1,476-second
            // "chrome" outlier the 2026-08-20 real-iMac capture reported -
            // confirmed by the screensaver's own active window matching that
            // gap exactly. The blank-after steady state can run for as long
            // as the machine sits unattended, presenting NOTHING every tick
            // (by design, #652) - note it every such tick so whichever path
            // presents next (on real input) is timed from when the display
            // became presentable again, not from its own last mark before
            // the screensaver ever activated.
            perfframe_note_quiescent();
            return;   // steady state: nothing to compute, nothing to present
        }
        s_ss_blanked = false;   // re-armed for the next activation

        // #650 FRAME CAP. The screensaver used to render on EVERY main-loop
        // iteration, i.e. as fast as it could at the 33ms active cadence.
        // MEASURED: it was not capped at all - raising the framebuffer from
        // 1280x800 to 1920x1080 dropped it from 18.0 to 11.5 fps while CPU
        // ROSE from 34% to 58%, which is the signature of a loop bounded by
        // render cost rather than by any target rate. A screensaver has no
        // reason to render flat out; it exists to be looked at, not to be
        // fast, and on a laptop or a warm iMac the difference is battery and
        // thermals as well as CPU.
        //
        // Not a busy-wait (#426): skipping a frame returns to the main loop,
        // which still runs its normal sys_sleep(loop_sleep_ms) of 33ms or
        // more. We sleep every iteration either way; we simply do not repaint
        // on every one.
        {
            static uint64_t s_ss_last_ms = 0;
            uint64_t now = uptime_ms();
            if (s_ss_last_ms != 0 && (now - s_ss_last_ms) < SS_FRAME_MIN_MS)
                return;                      // not due yet: skip this repaint
            s_ss_last_ms = now;
        }
        screensaver_render();
        cursor_render();
        apply_display_effects();
        idleprof_flip();   // #COMPIDLE: timed present
        perfframe_mark("screensaver");   // perf62: animating screensaver present
        // #62: the screensaver exclusively owns the display for as long as
        // it stays active (blank-after or animating alike) - see the
        // blank-after call site above for the full rationale.
        perfframe_note_quiescent();
        return;
    }

    // #158 NATIVE FULLSCREEN fast path. Checked AFTER lock/screensaver (both
    // above already returned if active - the ordering itself is the "lock and
    // screensaver can always take the screen back" guarantee: this can never
    // run while either owns the display) and BEFORE the normal composite
    // below, which it replaces entirely on a hit. See the #158 block above
    // fullscreen_app_on_top() for the full contract.
    if (native_fullscreen_try_render()) {
        return;
    }

    // (#745) THE ONE PLACE THE GLASS BLUR IS ALLOWED TO READ g_fb.
    // A full frame repaints the whole screen from wallpaper upwards before any
    // chrome is drawn, so when taskbar_render()/startmenu_render() run, the
    // region under (and around) a glass surface holds wallpaper + windows and
    // nothing else. The partial paths do NOT have that property: they
    // recomposite a CLIPPED region only, so outside the clip g_fb still holds
    // the PREVIOUS frame's chrome, and blurring that would feed chrome back
    // into the glass and compound every frame. render_frame_cursor() and
    // render_frame_idle() therefore leave g_glass_live at 0 and can only blit
    // an already-built strip. See glass_render() in draw.c, where the flag is
    // tested once and every g_fb read sits inside that branch.
    glass_bump_epoch();
    g_glass_live = 1;
    render_frame_body(true);         // full desktop stack, all windows
    g_glass_live = 0;
    idleprof_flip();   // #COMPIDLE: timed present                       // 11. Present
    perfframe_mark(s_pf_full_reason);   // perf62: caller-tagged (see above)
}

// ============================================================================
// T0(a)/#578 render_frame_cursor: partial-present path for pointer motion.
//
// When the ONLY interactivity is the mouse moving over the desktop (no keys,
// no button change, no drag, no open menu/dialog, no animating app, not
// locked, no screensaver), recompositing the whole 1280x800 screen + copying
// ~4MB to the framebuffer 120x/second just to move a 12x12 arrow is pure
// waste. Instead damage the union of the old and new cursor rects (two tiny
// boxes), recomposite the full layer stack CLIPPED to that union, and present
// only it. If the pointer jumped far enough that the union is a large fraction
// of the screen, fall back to a full frame (cheaper than a giant partial).
// ============================================================================

// Bounding box of the cursor at (mx,my), generous enough to cover every style
// (arrow extends down-right, glow extends in all directions) at any size up to
// the 250% cap. Returned in screen coords, NOT yet clamped.
static void cursor_bounds(int mx, int my, int *x0, int *y0, int *x1, int *y1)
{
    int scale = cursor_effective_scale();   // #uiscale: same formula cursor_render() draws with
    int reach = 12 * scale / 100 + 6;   // arrow max extent + glow radius margin
    *x0 = mx - reach; *y0 = my - reach;
    *x1 = mx + reach; *y1 = my + reach;
}

static bool rects_intersect(int ax, int ay, int aw, int ah,
                            int bx, int by, int bw, int bh)
{
    return ax < bx + bw && bx < ax + aw && ay < by + bh && by < ay + ah;
}

static void render_frame_cursor(int old_x, int old_y,
                                const wm_window_info_t *wins, int nwins)
{
    // Union of old + new cursor boxes, clamped to the screen.
    int ax0, ay0, ax1, ay1, bx0, by0, bx1, by1;
    cursor_bounds(old_x, old_y, &ax0, &ay0, &ax1, &ay1);
    cursor_bounds(g_mouse_x, g_mouse_y, &bx0, &by0, &bx1, &by1);
    int x0 = ax0 < bx0 ? ax0 : bx0;
    int y0 = ay0 < by0 ? ay0 : by0;
    int x1 = ax1 > bx1 ? ax1 : bx1;
    int y1 = ay1 > by1 ? ay1 : by1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_fb_width)  x1 = g_fb_width;
    if (y1 > g_fb_height) y1 = g_fb_height;
    int rw = x1 - x0, rh = y1 - y0;
    if (rw <= 0 || rh <= 0) { render_frame(); vnc_mark_full_dirty(); return; }

    // Fast flick: if the union covers a large fraction of the screen, a full
    // frame is cheaper (and avoids a huge "partial" present). Threshold ~1/4.
    if ((long)rw * rh * 4 >= (long)g_fb_width * g_fb_height) {
        render_frame(); vnc_mark_full_dirty(); return;
    }

    // Only pay the unclipped kernel window composite when a visible window
    // actually intersects the damaged region; over bare desktop, skip it.
    bool need_windows = false;
    for (int i = 0; i < nwins && !need_windows; i++) {
        if (wins[i].visible && !wins[i].minimized)
            need_windows = rects_intersect(x0, y0, rw, rh,
                                           wins[i].x, wins[i].y,
                                           wins[i].width, wins[i].height);
    }

    g_widgets_draw_only = 1;         // draw widgets without advancing animation
    draw_set_clip(x0, y0, rw, rh);
    fb_damage(x0, y0, rw, rh);       // kernel presents ONLY this rect
    // (#745) render_frame_body() is SHARED with the full-frame path, so the
    // glass gate is cleared explicitly here rather than relied on to be zero.
    // This is the divergence hazard that has already bitten this file once
    // (render_frame_idle's taskbar gate, #745): the two partial paths must be
    // audited together, never assumed to match.
    g_glass_live = 0;
    render_frame_body(need_windows);
    draw_clear_clip();
    g_widgets_draw_only = 0;
    idleprof_flip();   // #COMPIDLE: timed present
    perfframe_mark("cursor");   // perf62: T0(a)/#578 pointer-motion-only partial present
    vnc_mark_rect_dirty(x0, y0, rw, rh);   // #440 parity: same rect to VNC
}

// ============================================================================
// render_frame_idle: DIRTY-RECT present (#102/#379). Recomposite ONLY the
// damage rectangles collected this frame, each redrawn back-to-front (same
// z-order as render_frame) but CLIPPED to that rect, then a single present.
//
// Reached only on the pure-idle desktop: no visible app windows, no open menu /
// dialog, no drag, no screensaver, no recent input. So the interactive overlays
// and kernel app-window compositing are not needed here; the persistent back
// buffer already holds the last full frame, and unchanged regions are left
// untouched. This is what drops idle host CPU from ~full-core to single digits:
// instead of blitting the whole 1280x800 wallpaper + every widget's TTF text at
// a fixed cadence, only the small rects that changed are redrawn (and if nothing
// changed, damage_count()==0 and we present nothing at all).
// ============================================================================
static void render_frame_idle(void)
{
    g_widgets_draw_only = 1;         // widgets draw without re-advancing state
    g_glass_live = 0;                // (#745) clipped pass: cached strip only
    int n = damage_count();
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        if (!damage_get(i, &x, &y, &w, &h)) continue;
        draw_set_clip(x, y, w, h);
        // b740: tell the kernel to present ONLY this rectangle. Without this the
        // kernel sys_fb_flip fell back to a full ~4MB back->front copy every idle
        // frame; reporting the exact redrawn rects turns the present partial.
        fb_damage(x, y, w, h);

        wallpaper_render_background();   // 1. wallpaper (clipped blit)
        if (!g_setup_pending) {
            desktop_render();            // 2. desktop icons
            desktop_render_version();    // 3. version overlay
        }
        if (!g_setup_pending) widgets_render();  // 3b. widgets + pets (draw-only)
        stickies_render();               // 3b2. sticky notes
        if (!g_setup_pending) taskbar_render();  // 5. taskbar (match render_frame_body's gate)
        notif_render();                  // 9d. any lingering toast overlapping
        cursor_render();                 // 10. cursor on top
        apply_display_effects();         // 10b. brightness / night-light (clipped)
    }
    draw_clear_clip();
    g_widgets_draw_only = 0;
    idleprof_flip();   // #COMPIDLE: timed present                       // 11. single present (kernel copies back->front)
    perfframe_mark("idle");   // perf62: pure-idle dirty-rect present
}

// ============================================================================
// render_frame_chrome (#745, local 80): DIRTY-RECT present for the
// windows-are-open-but-nothing-is-moving case.
//
// #564 stopped a merely-open window forcing a 30Hz full composite, which was
// the right call for CPU, but it did it by doing LITERALLY NOTHING on those
// ticks: no damage collection, no present. The tray gauges (CPU/RAM/DSK/NET),
// the bar clock and notification toasts are always-on-top CHROME whose values
// keep advancing (taskbar_update() runs every iteration at the top of the main
// loop and is throttled to 1Hz on its own), so their SAMPLES were fresh the
// whole time and only their PIXELS were stale: the gauges appeared frozen for
// as long as any window stayed open. That is the reported defect.
//
// The idle suppression is kept. What changes is its scope: it now applies to
// the DESKTOP layer (wallpaper, icons, widgets, pets), which sits BEHIND
// windows and is decoration, and no longer to the chrome layer, which sits in
// front of them and is the system status surface. So the chrome keeps its own
// periodic damage (the damage it already computes in taskbar_collect_damage()
// and notif_collect_damage(); no second mechanism is introduced) and gets a
// clipped present of exactly those rects.
//
// render_frame_idle() cannot be used here: it draws its own abbreviated layer
// list and never calls compositor_render_windows(), so with a window open its
// wallpaper/desktop pass would paint OVER live window pixels. This path
// therefore reuses render_frame_body(), the SAME back-to-front layer stack the
// full-frame and cursor paths use, clipped to each damage rect, and asks for
// the kernel window composite only when a visible window actually intersects
// that rect (identical test to render_frame_cursor's need_windows; on the
// normal desktop the taskbar strip intersects nothing and the composite is
// skipped). Sharing render_frame_body is what stops this becoming a third
// divergent copy of the layer order.
// ============================================================================
static void render_frame_chrome(const wm_window_info_t *wins, int nwins)
{
    int n = damage_count();
    if (n <= 0) return;
    g_widgets_draw_only = 1;         // widgets draw without re-advancing state
    g_glass_live = 0;                // clipped pass: cached glass strip only
    for (int i = 0; i < n; i++) {
        int x, y, w, h;
        if (!damage_get(i, &x, &y, &w, &h)) continue;
        bool need_windows = false;
        for (int k = 0; k < nwins && !need_windows; k++) {
            if (wins[k].visible && !wins[k].minimized)
                need_windows = rects_intersect(x, y, w, h,
                                               wins[k].x, wins[k].y,
                                               wins[k].width, wins[k].height);
        }
        draw_set_clip(x, y, w, h);
        fb_damage(x, y, w, h);       // kernel presents ONLY this rect
        render_frame_body(need_windows);
    }
    draw_clear_clip();
    g_widgets_draw_only = 0;
    idleprof_flip();   // #COMPIDLE: timed present                       // single present
    perfframe_mark("chrome");   // perf62: windows-open-but-static chrome-only present
}

// ============================================================================
// main: entry point
// ============================================================================

// --- Scalable UI text via the kernel TTF engine (#58) ---
int g_font_px = 16;   // Medium default

// #uiscale TEMPORARY opt-out (ui-ux screendump bug 3, desktop gadgets).
// widgets.c's own box literals (calendar cells, weather/HA cards, etc - 781
// of them) are NOT scaled (documented, out of scope for this pass), but
// draw_text_ttf()/text_width_ttf() below scale UNCONDITIONALLY, so gadget
// text started rendering at the full UI-scale factor inside boxes that
// stayed 1x - strictly worse than doing nothing (Calendar overlapping its
// own date columns, Weather/HA cards overflowing their frames). This depth
// counter lets a caller say "the box under me is not scaled, so my text
// must not be either" for the duration of one draw call.
//
// REMOVING THIS requires scaling widgets.c's own box geometry (ui_px()/
// ui_span() on its layout literals) FIRST, then deleting every push/pop
// site below and this counter itself. Until then it is the only thing
// stopping gadget text from outgrowing gadget boxes.
int g_ui_scale_native_text = 0;
void ui_scale_native_push(void) { g_ui_scale_native_text++; }
void ui_scale_native_pop(void)  { if (g_ui_scale_native_text > 0) g_ui_scale_native_text--; }
// UI SCALE CHOKEPOINT (task uiscale). Every text call in this compositor -
// draw_text()/draw_text_centered() via UI_TTF_SIZE (draw.c), and the ~80-odd
// direct draw_text_ttf()/text_width_ttf() call sites across clock.c, notif.c,
// taskbar.c, traymenu.c, widgets.c, elevate.c, lockscreen.c, confirmdialog.c,
// dragghost.c, screenshot.c, volosd.c and contextmenu.c that pass their own
// literal point size - ALL route through these two functions and no other
// path reaches ttf_text()/ttf_measure() (the raw kernel syscalls). Scaling
// `size` here, once, is what makes every one of those call sites scale
// without being touched individually, and keeps draw_text_ttf() and
// text_width_ttf() applying the SAME transform to the SAME logical size, so
// a caller's own "tw = text_width_ttf(s, sz); draw_text_ttf(x, y, s, sz, c)"
// pairing (there are many) never sees its measured width disagree with what
// gets drawn.
//
// font_metrics() (libc/syscall.h, off-limits to this task) is a SEPARATE
// direct-to-kernel call with its own `size` argument at a handful of sites
// (draw.c, dragghost.c, taskbar.c, volosd.c); those sites scale their own
// `size` argument to font_metrics() individually so the ascent/descent they
// get back stays proportional to what draw_text_ttf() actually drew.
void draw_text_ttf(int32_t x, int32_t y, const char *text, int size, uint32_t color) {
    // #uiscale: g_ui_scale_native_text (see its declaration above) is a
    // TEMPORARY opt-out for callers whose own box geometry is not scaled -
    // see the sweep in widgets.c/notif.c/traymenu.c/stickies.c/startmenu.c.
    size = g_ui_scale_native_text ? size : ui_px(size);
    // #102/#379 dirty-rect: text is rasterized in the KERNEL (SYS_DRAW_TTF) and
    // cannot honor the userland clip per-pixel, so cull the whole syscall when
    // the text's (generous) bounding box lies entirely outside the active clip.
    // Widgets are placed apart, so a vertical-band + left-edge test never drops
    // visible text but skips every off-region string during a clipped recompose.
    if (size < 4) size = 4;
    if (y + 2 * size < g_clip_y0 || y - size > g_clip_y1 ||
        x > g_clip_x1 || x + 4096 < g_clip_x0) {
        return;
    }
    ttf_text(x, y, text, size, color);
}
int text_width_ttf(const char *text, int size) {
    int eff = g_ui_scale_native_text ? size : ui_px(size);   // #uiscale: see draw_text_ttf() above
    return ttf_measure(text, eff);
}

// --- Display post-effects: brightness dim + night-light warm tint (#57) ---
int g_brightness = 100;   // 100 = normal
int g_win_opacity = 242;  // global default window opacity (0-255); ~95% default
int g_nightlight = 0;     // 0 = off, else warm-tint strength 1-100

static void apply_display_effects(void)
{
    if (g_brightness >= 100 && g_nightlight <= 0) return;  // nothing to do
    // #102/#379: honor the active clip so the idle path post-processes ONLY the
    // freshly-composited dirty rect (applying the effect to already-dimmed
    // unchanged pixels would darken them again every frame).
    int y0 = g_clip_y0 < 0 ? 0 : g_clip_y0;
    int x0 = g_clip_x0 < 0 ? 0 : g_clip_x0;
    int y1 = g_clip_y1 > g_fb_height ? g_fb_height : g_clip_y1;
    int x1 = g_clip_x1 > g_fb_width  ? g_fb_width  : g_clip_x1;
    for (int yy = y0; yy < y1; yy++) {
        uint32_t *row = &g_fb[yy * g_fb_pitch];
        for (int xx = x0; xx < x1; xx++) {
            uint32_t c = row[xx];
            uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            if (g_brightness < 100) {
                r = r * (uint32_t)g_brightness / 100;
                g = g * (uint32_t)g_brightness / 100;
                b = b * (uint32_t)g_brightness / 100;
            }
            if (g_nightlight > 0) {
                b = b * (uint32_t)(100 - g_nightlight) / 100;
                g = g * (uint32_t)(100 - g_nightlight / 2) / 100;
            }
            row[xx] = (c & 0xFF000000u) | (r << 16) | (g << 8) | b;
        }
    }
}

// --- Live desktop icon sizing (driven by get_icon_size(), see #63) ---
// #uiscale: these are plain globals (not macros), re-derived here from a
// logical 32/48/64 ladder and the UI scale factor together, exactly the
// "re-derive on the edge-triggered ui_scale_refresh() poll" shape the theme
// poll uses. main()'s loop calls this again (with the last-applied icon-size
// preference) whenever ui_scale_refresh() reports a change - see the loop.
int DESKTOP_ICON_SIZE = 48;
int DESKTOP_ICON_SPACING_X = 100;
int DESKTOP_ICON_SPACING_Y = 90;

void compositor_apply_icon_size(int sz)
{
    switch (sz) {
    case 0: DESKTOP_ICON_SIZE=ui_px(32); DESKTOP_ICON_SPACING_X=ui_px(72);  DESKTOP_ICON_SPACING_Y=ui_px(66);  break;
    case 2: DESKTOP_ICON_SIZE=ui_px(64); DESKTOP_ICON_SPACING_X=ui_px(128); DESKTOP_ICON_SPACING_Y=ui_px(116); break;
    default: DESKTOP_ICON_SIZE=ui_px(48); DESKTOP_ICON_SPACING_X=ui_px(100); DESKTOP_ICON_SPACING_Y=ui_px(90); break;
    }
}

// --- Theme-aware chrome colors (driven by get_theme(), see #55) ---
uint32_t CLR_TASKBAR_BG = 0xFF2D2D2D;
uint32_t CLR_TASKBAR_BORDER = 0xFF505050;
uint32_t CLR_TASKBAR_HOVER = 0xFF4A4A4A;
uint32_t CLR_TASKBAR_ACTIVE = 0xFF5A5A5A;   // #745: theme taskbar_active, see compositor.h
uint32_t CLR_START_BTN = 0xFF2D2D2D;
uint32_t CLR_MENU_BG = 0xFF2D2D2D;
uint32_t CLR_MENU_SHADOW = 0xFF1A1A1A;
uint32_t CLR_MENU_BORDER = 0xFF606060;
uint32_t CLR_MENU_ITEM_HOVER = 0xFF4A4A4A;
uint32_t CLR_MENU_ITEM_NORM = 0xFF383838;
uint32_t CLR_MENU_CAT_BG = 0xFF404050;
uint32_t CLR_MENU_TEXT = 0xFFE0E0E0;
uint32_t CLR_MENU_SEP = 0xFF505050;
uint32_t CLR_CTX_BG = 0xFF303030;
uint32_t CLR_CTX_BORDER = 0xFF606060;
uint32_t CLR_CTX_HOVER = 0xFF505050;
uint32_t CLR_CHROME_TEXT = 0xFFFFFFFF;
uint32_t CLR_GAUGE_BG = 0xFF1A1A1A;       // #110: set per-theme below
uint32_t CLR_GAUGE_BORDER = 0xFF606060;   // #110: set per-theme below
// Focused taskbar task-tile outline. Deliberately a MID-GREY, never white: a
// white outline around the dark active-tile box read as harsh in dark themes.
// Derived per-theme in compositor_apply_theme() as a 50% blend of the taskbar
// background and its readable ink, so it stays subtle in every palette.
uint32_t CLR_TASK_FOCUS_BORDER = 0xFF707070;

// Apply a system-wide theme to the compositor chrome. Maps the kernel
// theme id (see Settings: 1=Dark 2=Light 4=Classic 5=Ocean 9=Nord) to a
// chrome palette. Unknown ids fall back to the dark palette.
// (#285) Small ARGB darken helper (compositor has no gui_style link).
static uint32_t darken_argb(uint32_t c, int amt) {
    int r = (int)((c >> 16) & 0xFF) - amt; if (r < 0) r = 0;
    int g = (int)((c >> 8)  & 0xFF) - amt; if (g < 0) g = 0;
    int b = (int)( c        & 0xFF) - amt; if (b < 0) b = 0;
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

// Apply a system-wide theme to the compositor chrome. (#285) ALL chrome colors
// are now derived from the kernel theme table via SYS_THEME_COLOR, the single
// source of truth shared with the window decorator and every app. This makes
// every one of the 12 kernel themes recolor the taskbar / start menu / context
// menus consistently (the old hardcoded switch only knew 5 ids).
// ============================================================================
// (#745) GLASS MATERIAL. The tint and the neutral launcher-chip ramp are
// PER-THEME, read straight out of the active /THEMES/<slug>.mtheme file.
//
// Deliberately NOT routed through the kernel theme table: these are chrome-only
// keys with exactly one consumer (this compositor), the .mtheme file is already
// a plain key=value file that userland can read, and gui_theme.h already
// resolves the active slug. Adding kernel theme_t fields + THEME_COLOR_* ids +
// a switch case for a value only the compositor ever asks for would have meant
// a kernel rebuild to change a chrome colour, which is the opposite of what
// #711 made the theme format for.
//
// THERE IS DELIBERATELY NO glass_alpha KEY. Opacity is a USER preference
// (g_dock_opacity, UIPROFIL.YML), not a theme property, so switching theme can
// never overwrite a number the user set with the Appearance slider.
// ============================================================================
int      g_glass_enable  = 1;
uint32_t CLR_GLASS_TINT  = 0xFF1C1C21;   // #745 dockgrey: matches the new 78% derivation, see glass_theme_apply()
// (#745 taskbar/tray) CLR_CHIP_* removed here - see the comment on their
// declaration in compositor.h.

// Parse "0x00RRGGBB", "0xRRGGBB" or a plain decimal. Returns 0 on a bad value.
static uint32_t gt_parse_num(const char *v)
{
    uint32_t out = 0;
    while (*v == ' ' || *v == '\t') v++;
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X')) {
        v += 2;
        for (;;) {
            char c = *v++;
            int d;
            if      (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            out = (out << 4) | (uint32_t)d;
        }
        return out;
    }
    while (*v >= '0' && *v <= '9') out = out * 10 + (uint32_t)(*v++ - '0');
    return out;
}

static int gt_key_is(const char *line, const char *key, const char **val)
{
    int i = 0;
    while (key[i] && line[i] == key[i]) i++;
    if (key[i] != '\0' || line[i] != '=') return 0;
    *val = line + i + 1;
    return 1;
}

// A .mtheme is up to ~78KB, so it is streamed in 1KB chunks with a bounded
// line buffer rather than slurped into a static the size of the largest file.
static void glass_theme_read_file(const char *path, int *saw_enable)
{
    int fd = sys_open(path, 0);
    if (fd < 0) return;
    static char chunk[1024];
    char line[128];
    int  ln = 0;
    long n;
    while ((n = sys_read(fd, chunk, sizeof(chunk))) > 0) {
        for (long i = 0; i < n; i++) {
            char c = chunk[i];
            if (c == '\n' || c == '\r') {
                if (ln > 0) {
                    line[ln] = '\0';
                    const char *v;
                    if      (gt_key_is(line, "glass_tint", &v))
                        CLR_GLASS_TINT = 0xFF000000u | (gt_parse_num(v) & 0x00FFFFFFu);
                    else if (gt_key_is(line, "glass_enable", &v)) {
                        g_glass_enable = gt_parse_num(v) ? 1 : 0; *saw_enable = 1;
                    }
                    // (#745 taskbar/tray) chrome_chip_* keys removed: the chip
                    // plate they configured is gone (chrome_chip() in
                    // taskbar.c no longer paints a fill). Two .mtheme files
                    // (maytera_dark/light) still carry these lines; they are
                    // now silently ignored, same as any other unknown key.
                    // Tier 4 default for the retro-lineage families: beveled
                    // CDE/Win95 chrome is opaque on principle. A file that
                    // states glass_enable explicitly always wins, because the
                    // explicit key is parsed above and *saw_enable latches.
                    else if (gt_key_is(line, "style", &v)) {
                        if (!*saw_enable && v[0] == 'r' && v[1] == 'e' &&
                            v[2] == 't' && v[3] == 'r' && v[4] == 'o')
                            g_glass_enable = 0;
                    }
                }
                ln = 0;
            } else if (ln < (int)sizeof(line) - 1) {
                line[ln++] = c;
            }
        }
    }
    sys_close(fd);
}

void glass_theme_apply(void)
{
    // Derived defaults FIRST, so a theme file that names none of the keys
    // still gets a coherent material instead of maytera_dark's palette
    // stamped onto it. The derivation lands within 2/255 of the spec values
    // for maytera_dark (0x24242B -> 0x1C1C21 vs 0x1C1C21) and maytera_light
    // (0xEDEFF3 -> 0xF2F3F6 vs 0xF2F4F7).
    //
    // (#745 dockgrey, 2026-08-12) USER-REPORTED: "on the marble dock, there's
    // not enough transparency and the color should be a dark grey not black
    // ... the same is true for the taskbar - matching say the weather
    // widget". Measured off a rendered frame: the weather widget (widgets.c
    // draw_weather_card()) fills CLR_MENU_BG at ~95% opacity through a
    // straight g_draw_blend, no darkening step at all. This dark-branch "settle
    // toward black" factor was 58%, which took maytera_dark's already-dark
    // taskbar_bg (0x24242B, a genuine dark grey) down to 0x14161A (20,22,26) -
    // close enough to pure black to read as black, not grey, exactly the
    // complaint. Raised to 78%: still noticeably darker than the flat taskbar
    // token (glass has to read as a distinct material, not a copy of the bar),
    // but landing in the same tonal family as CLR_MENU_BG instead of crossing
    // into near-black. See /tmp/dockgrey_harness.py (a host-side port of this
    // exact derivation, draw_blend() and the project's WCAG contrast formula)
    // for the full 14-theme before/after table this value was chosen from;
    // g_dock_opacity's floor (this file does not own it, see draw.c) moved
    // from 60 to 70 in the SAME change for the reason documented there.
    {
        uint32_t b = CLR_TASKBAR_BG;
        uint32_t r = (b >> 16) & 0xFF, g = (b >> 8) & 0xFF, bl = b & 0xFF;
        if (draw_luminance(b) >= 140) {         // light theme: lift toward white
            r += (255 - r) * 28 / 100;
            g += (255 - g) * 28 / 100;
            bl += (255 - bl) * 28 / 100;
        } else {                                 // dark theme: settle toward black
            r = r * 78 / 100; g = g * 78 / 100; bl = bl * 78 / 100;
        }
        CLR_GLASS_TINT = 0xFF000000u | (r << 16) | (g << 8) | bl;
    }
    g_glass_enable  = 1;

    char slug[GUI_THEME_SLUG_MAX];
    if (gui_theme_get_active_slug(slug, sizeof(slug)) && slug[0]) {
        char path[80];
        int i = 0;
        const char *pre = "/THEMES/";
        while (*pre) path[i++] = *pre++;
        for (int j = 0; slug[j] && i < 66; j++) path[i++] = slug[j];
        const char *ext = ".mtheme";
        while (*ext) path[i++] = *ext++;
        path[i] = '\0';
        int saw_enable = 0;
        glass_theme_read_file(path, &saw_enable);
    }

    // A theme whose taskbar is a pure extreme (high_contrast paints it
    // 0x000000) is asking for maximum separation, not for a material: glass
    // there would reduce the very contrast the theme exists to provide.
    {
        uint32_t b = CLR_TASKBAR_BG & 0x00FFFFFFu;
        if (b == 0x000000u || b == 0xFFFFFFu) g_glass_enable = 0;
    }

    glass_invalidate_all();   // material changed: every cached strip is stale
}

// The background chrome text is actually ON. Contrast has to be measured
// against what the eye receives, and on glass that is the tint, not the opaque
// CLR_TASKBAR_BG token (which is no longer painted on a glass surface at all).
uint32_t chrome_surface_bg(void)
{
    return g_glass_enable ? CLR_GLASS_TINT : CLR_TASKBAR_BG;
}

uint32_t chrome_ink_dim(void)
{
    return readable_ink_dim_mix(chrome_surface_bg(), g_glass_enable ? 22 : 35);
}

void compositor_apply_theme(int kernel_theme_id)
{
    g_compositor_theme_id = kernel_theme_id;   // remembered for window-shadow gating (#160)

    // (#337) Fork-compat guard: the ole2c kernel (Word6 fork) does not implement
    // SYS_THEME_COLOR (#285). On an unsupported syscall the kernel returns -1, so
    // theme_color_of() yields 0xFFFFFFFF (a real theme color has a zero high byte)
    // and every chrome color would collapse to white. Detect that sentinel and
    // keep the compositor's built-in dark defaults instead, which already match
    // the Retro-UNIX palette the ole2c kernel decorator uses. On the main kernel
    // SYS_THEME_COLOR works normally, so full theming applies as before.
    if ((theme_color_of(kernel_theme_id, THEME_COLOR_TASKBAR_BG) & 0xFF000000u) != 0)
        return;

    // theme_color_of() returns 0x00RRGGBB; chrome wants opaque 0xFFRRGGBB.
    #define TC(cid) (0xFF000000u | theme_color_of(kernel_theme_id, (cid)))
    CLR_TASKBAR_BG      = TC(THEME_COLOR_TASKBAR_BG);
    CLR_TASKBAR_BORDER  = TC(THEME_COLOR_WINDOW_BORDER);
    CLR_TASKBAR_HOVER   = TC(THEME_COLOR_TASKBAR_HOVER);
    CLR_TASKBAR_ACTIVE  = TC(THEME_COLOR_TASKBAR_ACTIVE);   // #745: chip open-state wash
    CLR_START_BTN       = TC(THEME_COLOR_START_BUTTON);
    CLR_MENU_BG         = TC(THEME_COLOR_MENU_BG);
    CLR_MENU_SHADOW     = darken_argb(CLR_MENU_BG, 24);
    CLR_MENU_BORDER     = TC(THEME_COLOR_MENU_BORDER);
    CLR_MENU_ITEM_HOVER = TC(THEME_COLOR_MENU_ITEM_HOVER);
    CLR_MENU_ITEM_NORM  = TC(THEME_COLOR_MENU_BG);
    CLR_MENU_CAT_BG     = TC(THEME_COLOR_SELECTION);
    CLR_MENU_TEXT       = TC(THEME_COLOR_MENU_TEXT);
    CLR_MENU_SEP        = TC(THEME_COLOR_MENU_SEPARATOR);
    CLR_CTX_BG          = TC(THEME_COLOR_MENU_BG);
    CLR_CTX_BORDER      = TC(THEME_COLOR_MENU_BORDER);
    CLR_CTX_HOVER       = TC(THEME_COLOR_MENU_ITEM_HOVER);
    #undef TC

    // #128: guarantee legible text by deriving menu/chrome ink from the actual
    // background luminance (fixes light themes especially).
    CLR_MENU_TEXT   = readable_ink(CLR_MENU_BG);
    CLR_CHROME_TEXT = readable_ink(CLR_TASKBAR_BG);
    // Focused task-tile outline. (#711) This is now the theme's ACCENT token
    // (color.accent), not a computed 50% blend of the taskbar bg and its ink.
    // A focused tile is exactly what an accent is for, and a computed blend is
    // by definition something a theme file cannot influence. Themes that set no
    // color.accent still get their old selection colour here, because
    // color.accent's derived default IS selection_bg (see themes.c
    // theme_fill_v2_defaults), so no shipped theme changes by accident.
    CLR_TASK_FOCUS_BORDER = 0xFF000000u | theme_color_of(kernel_theme_id, THEME_COLOR_ACCENT);
    // #110: taskbar gauges (CPU/RAM/DSK/NET) follow the active theme: a darker
    // inset of the taskbar bg with the themed taskbar border.
    {
        uint32_t b = CLR_TASKBAR_BG;
        uint32_t r  = (((b >> 16) & 0xFF) * 7) / 10;
        uint32_t g  = (((b >> 8)  & 0xFF) * 7) / 10;
        uint32_t bl = (( b        & 0xFF) * 7) / 10;
        CLR_GAUGE_BG = 0xFF000000u | (r << 16) | (g << 8) | bl;
        CLR_GAUGE_BORDER = CLR_TASKBAR_BORDER;
    }

    // (#745) Resolve the glass material for the theme that was just applied.
    // Must come AFTER the CLR_* assignments above: the derived tint fallback
    // reads CLR_TASKBAR_BG.
    glass_theme_apply();
}

// #161: signal()/SIGCHLD/SIG_IGN declared locally instead of including
// <../../libc/signal.h>. That header pulls libc/types.h, whose
// `typedef _Bool bool` collides head-on with compositor.h's own
// `typedef int bool` - a pre-existing divergence in this app that is not
// this ticket's to resolve, and resolving it here would touch every file in
// the compositor. `signal` is a real symbol in libc.a (libc/signal.c:59) and
// this prototype is its exact signature, so the call is checked by the linker
// as usual. The two constants are pinned by their libc/signal.h values.
typedef void (*tb_sighandler_t)(int);
extern tb_sighandler_t signal(int signum, tb_sighandler_t handler);
#define tb_signal   signal
#define TB_SIGCHLD  17                          /* libc/signal.h SIGCHLD */
#define TB_SIG_IGN  ((tb_sighandler_t)1)        /* libc/signal.h SIG_IGN */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    // NOTE: No sys_putchar() calls here. They write to a PTY with no reader
    // and will block the compositor indefinitely.

    // #161: DECLARE THAT WE WILL NEVER wait() FOR A CHILD.
    //
    // Every app the user launches - dock, start menu, desktop icon, launcher -
    // is spawned by this process, so its ppid is ours. Nothing in this file has
    // ever called wait()/waitpid(), and this process does not exit, so on exit
    // each of those apps became a ZOMBIE that the kernel's reap policy was
    // required to keep forever (it reclaims a zombie only when nobody can still
    // wait for it, and a live parent means somebody can). MAX_PROCESSES is 64:
    // open and close apps for long enough and the table fills, "[PROC] No free
    // process slots" follows, and nothing more can be launched. It is the same
    // slow denial of service #745 task 37 fixed for kernel fetch workers,
    // reached by ordinary desktop use. The owner saw the first symptom of it:
    // a killed app that "shows as zombie" and will not go away.
    //
    // SIGCHLD == SIG_IGN is the POSIX way to say this, and it is a DECLARATION
    // rather than a heuristic: the kernel side (rustkern/procreap.rs,
    // F_NOCLDWAIT) reaps our children the moment they exit, and any process
    // that does still wait() - msh - is untouched because it says nothing.
    // Placed before compositor_init(), so the very first app we can possibly
    // spawn is already covered.
    tb_signal(TB_SIGCHLD, TB_SIG_IGN);

    // Initialise the framebuffer and all subsystems
    if (compositor_init() < 0) {
        sys_exit(1);
    }

    // #uiscale: verify the userland copy of the scale formula (uiscale.h's
    // ui_px()/ui_span()/ui_unpx(), duplicated there ONLY because the
    // compositor cannot afford a syscall per coordinate - see that header's
    // own rationale) agrees with the kernel's, once, at startup, over a wide
    // probe range including the round-trip and shared-edge properties. This
    // is the ONE place in the tree that logs with the same bare printf()
    // convention startmenu.c already uses unconditionally in the shipping
    // compositor (e.g. its "[StartMenu] rebuild:" line) - printf() here goes
    // through fputc()/stdout, NOT the sys_putchar() this file's own top-of-file
    // warning bans (that ban is about the raw PTY-fd1 syscall specifically).
    {
        extern int printf(const char *fmt, ...);
        int mismatches = ui_scale_selfcheck();
        if (mismatches != 0) {
            // LOUD: the two copies of the formula disagree. This should never
            // happen outside active development on uiscale.c/uiscale.rs; if it
            // ever fires on a shipped build, compositor chrome and the kernel's
            // own window-boundary transform can disagree about where a pixel
            // is, which is exactly the class of bug this selfcheck exists to
            // catch before it reaches a screenshot.
            printf("[UISCALE] SELFCHECK FAILED: %d mismatch(es) between the "
                   "compositor's ui_px/ui_span/ui_unpx and the kernel's - see "
                   "the MISMATCH/EDGE BROKEN lines above for the exact values.\n",
                   mismatches);
        } else {
            printf("[UISCALE] selfcheck OK: 0 mismatches (pct=%d)\n", ui_scale_pct());
        }
    }

    // (#565) Restore the persisted active theme from /CONFIG/THEME.CFG at
    // compositor startup, not just when Settings happens to be opened. The
    // kernel's own boot-time default is always index 0 (Retro UNIX) since it
    // does not read THEME.CFG itself (that file is userland's, by design -
    // see gui_theme.h); without this the desktop would boot into Retro UNIX
    // every time regardless of what the user last picked, only correcting
    // itself once Settings happened to run. A missing/unset THEME.CFG is not
    // an error (gui_theme_activate returns -1 and the kernel's index-0
    // fallback stands, which is correct on a first boot).
    {
        char active_slug[GUI_THEME_SLUG_MAX];
        if (gui_theme_get_active_slug(active_slug, sizeof(active_slug)) && active_slug[0]) {
            gui_theme_activate(active_slug);
        }
    }

    // (#73) The "TODO: re-enable login" that used to sit here is gone with the
    // code it referred to. It was never going to be re-enabled: kernel/gui/login.c
    // is the login gate and it runs BEFORE this process exists. Leaving the TODO
    // and login_init() in place is what kept the dead layer's key sink armed.
    //
    // #745 THE SHOWSTOPPER FOR THE NON-ROOT FLIP WAS HERE. These three lines
    // used to read:
    //
    //     g_login_uid = 0;
    //     strncpy(g_login_username, "root", 63);
    //
    // The kernel's login gate (kernel/gui/login.c) authenticates and sets the
    // session identity BEFORE the compositor is spawned, and the compositor is
    // spawned proc_as_session(), so it already RUNS as that user. Hardcoding
    // "root" here did not change the session; it made the compositor's idea of
    // who it was DISAGREE with the kernel's. At uid 0 the two happened to match
    // and nothing showed. At uid 1000 the lock screen sent "root" to
    // sys_session_unlock(), the kernel compared it to the real session user,
    // and no password the user could type would ever unlock the machine.
    g_logged_in = true;
    session_identity_init();

    // Reset idle timer so screensaver does not activate immediately.
    g_idle_ms = uptime_ms();

    // Main compositor loop
    notif_init();         // #168 clear the spool for a fresh session
    // #168: a startup notification demonstrates the live producer->toast pipeline.
    notify_post("MayteraOS", "Desktop ready - notifications are active", NOTIFY_INFO);

    // THROWAWAY #565 VERIFICATION HOOK - NOT FOR THE CLEAN BUILD. Headless
    // QMP mouse cannot reach the desktop-icon/start-menu hit-test (#334/#440),
    // so a marker-file auto-open is the accepted way to screenshot a
    // mouse-driven panel. Remove before the clean deploy/commit.
    {
        int mfd = sys_open("/APPSTOREAUTO.RUN", 0);
        if (mfd >= 0) { sys_close(mfd); sys_spawn("/APPS/APPSTORE"); }
    }

    // First-boot setup wizard (OOBE). The ABSENCE of /CONFIG/SETUPDONE is what
    // "unconfigured" means - nothing else - so this fires on a fresh disk
    // install AND on a live USB boot, because the ext2 root is writable in both
    // cases and the marker therefore persists on the stick.
    //
    // Spawned, NOT waited on. The wizard is an ordinary Ring-3 app whose window
    // is composited by THIS loop, so blocking here until it exits would freeze
    // the very UI we were waiting for. It draws on top instead.
    //
    // Until it completes, the machine is still running the shipped
    // autologin=root session against legacy password records; the wizard is
    // what replaces both (see users_create_first_admin, #745, which had no
    // reachable caller before this).
    {
        int sfd = sys_open("/CONFIG/SETUPDONE", 0);
        if (sfd < 0) {
            // FIRST BOOT. The full wizard, and g_setup_pending gates the
            // taskbar / desktop icons / start menu off until it finishes,
            // because until then the machine has no owner and no password.
            //
            // #136: delete any /CONFIG/SETUPSKIP left by a PREVIOUS boot's
            // "Skip to Desktop" before g_setup_pending is armed. Without
            // this, a stale skip marker from last boot would be sitting on
            // disk the instant setup_pending_recheck() first polls (see
            // its own comment), clearing g_setup_pending within ~330ms and
            // hiding the wizard we are about to spawn - exactly the
            // "reappear every boot until actually completed" requirement
            // failing silently. Result ignored: absent is the common case.
            //
            // #229: BOTH UNLINKS ARE GONE, AND SO IS THE CLASS OF BUG THEY
            // WERE ADDED FOR.
            //
            // They were writes to /CONFIG, which is root-owned 0711, so on a
            // non-root session (every session since #226 removed
            // autologin=root) the cleanup for the stale-marker bug had exactly
            // the same permission hole as the bug. Both signals are now bits of
            // kernel state (SYS_FIRSTRUN, kernel/rustkern/firstrun.rs) that
            // start clear at every boot because .bss starts clear at every
            // boot. A per-boot signal on persistent media needs a cleanup; a
            // per-boot signal in per-boot storage does not.
            //
            // FR_SKIP_CLEAR is still called, and it is NOT the old unlink under
            // a new name: it covers the second case, a LOG OUT AND BACK IN
            // within one boot on a machine that is still unconfigured. The
            // kernel bit survives that (it is per-boot, not per-session), so
            // the incoming compositor clears it as it arms a fresh first run,
            // exactly as it used to delete the file. It cannot fail.
            sys_firstrun(FR_SKIP_CLEAR);
            g_setup_pending = true;
            // #203: arm the handover watch HERE, at the one site that knows
            // this is the FIRST-BOOT wizard rather than the #126 reduced
            // personalisation flow. It stays armed for the life of this
            // compositor: the cost is one sys_open() every 500ms in a session
            // that begins with the first-run wizard, which happens once per
            // machine, and the alternative is a disarm condition that has to
            // guess when the wizard gave up.
            g_setup_handover_armed = true;
            sys_spawn("/APPS/SETUP");
        } else {
            sys_close(sfd);
            // #126: THE MACHINE IS SET UP, BUT IS THIS USER?
            //
            // /CONFIG/SETUPDONE is a single absolute path, so the FIRST person
            // to finish the wizard permanently suppressed it for everyone who
            // ever signed in afterwards. That is the reported defect: a second
            // user got a desktop configured by, and for, somebody else.
            //
            // The per-user answer lives beside the per-user profile that
            // already works: <home>/CONFIG/SETUPUSR, resolved by the same
            // passwd-table join userconf_path() performs for THEME.CFG,
            // DOCKSTYL.CFG and the rest. No parallel config system, one more
            // name in the namespace that exists. Root's home is "/", so root's
            // marker is /CONFIG/SETUPUSR: beside SETUPDONE, and not it.
            //
            // NO LEGACY FALLBACK. userconf_open_read()'s fallback exists so an
            // upgrade keeps a setting the user already had; there is no
            // pre-#126 location for this marker, and falling back to a shared
            // path would make one user's marker answer for everybody, which is
            // the bug.
            //
            // g_setup_pending is deliberately NOT set here. It gates the shell
            // off for an UNCONFIGURED MACHINE; this machine is configured and
            // the person is signed in, so the desktop stays usable and the
            // personalisation wizard is a window on top of it. (It could not
            // work anyway: setup_pending_recheck() clears the flag from
            // /CONFIG/SETUPDONE, which is already present.)
            char upath[256];
            int have_user_marker = 0;
            if (userconf_path("SETUPUSR", upath, sizeof(upath)) == 0) {
                int ufd = sys_open(upath, 0);
                if (ufd >= 0) { sys_close(ufd); have_user_marker = 1; }
            } else {
                // The path did not fit, so we do not know. Treat that as
                // "already personalised" rather than launching a wizard whose
                // completion marker cannot be written: it would reappear at
                // every single login and there would be no way to stop it.
                have_user_marker = 1;
            }
            if (!have_user_marker) sys_spawn("/APPS/SETUP");
        }
    }

    while (1) {

        idleprof_tick_begin();    // #COMPIDLE: see idleprof.h

        int loop_sleep_ms = 33;   // #102/#379 adaptive idle poll interval

        // #uiscale: ONE cheap syscall every frame (real work only when the
        // kernel's generation counter moved - see ui_scale_refresh()'s own
        // comment in uiscale.c). Must run before anything else in this frame
        // touches ui_px()/ui_span() or any of the FONT_CHAR_*/TASKBAR_*/
        // START_MENU_*/etc macros that read the g_ui_scale_pct this call
        // maintains, so the whole frame draws at one consistent factor.
        //
        // Unlike the ~2s throttled theme-file poll below (gui_theme_poll_reload,
        // which has to actually read a file off disk), this is NOT throttled:
        // the syscall itself is already gen-counter-gated, so calling it every
        // iteration costs nothing extra on the common no-change path and never
        // leaves the compositor mid-frame on a stale factor for up to 2s.
        if (ui_scale_refresh()) {
            // The factor changed (Settings' scale control, a laptop-DPI
            // auto-pick, or SYS_UI_SCALE from another process). Force a full
            // recomposite so already-drawn pixels - open app windows
            // included, same reasoning as a theme switch just below - are
            // redrawn at the new factor, and re-derive the handful of
            // compositor globals that are cached values rather than plain
            // macros (a macro re-evaluates ui_px() on every use for free;
            // these do not).
            wm_force_redraw_all();
            g_needs_redraw = true;
            compositor_apply_icon_size(get_icon_size());   // #63 desktop icon grid
        }

        taskbar_update();     // Refresh gauge samples (CPU, RAM, disk)
        lock_poll();          // #566 refresh the kernel-authoritative lock state + idle-timeout check
        elevate_poll();       // #745: mirror the kernel's elevation request and
                              //       run its watchdog, BEFORE input is routed
        process_input();      // Poll mouse and keyboard
        { static int s_dp = 0; if (++s_dp >= 10) { s_dp = 0; dock_style_poll(); dock_opacity_poll(); dock_geom_poll(); taskbar_geom_settle(); widgets_cfg_poll(); perfframe_poll(); } }  // #387 live dock style, #745 live dock opacity, #123 live dock height/zoom, + widget live-apply channel, perf62 frame-interval gate/dump
        { static int s_sp = 0; if (++s_sp >= 10) { s_sp = 0; setup_pending_recheck(); } }  // #745 fix: this was dead code with zero callers, so g_setup_pending never
                                                                                            // cleared after the OOBE wizard wrote SETUPDONE - taskbar/icons/start menu
                                                                                            // stayed gated off forever. Same ~330ms cadence as the dock-style poll above.
        desktop_home_tick();        // #745 throttled (2s) <home>/DESKTOP rescan; no-op when unchanged
    desktop_volumes_tick();     // #250 throttled (1s) removable-volume list; ONE syscall when unchanged
        startmenu_prefs_poll();     // throttled internally; live-applies Settings "Start Menu" prefs
        startmenu_favs_poll();      // #745 P1: throttled internally; live-applies the first-boot wizard's FAVCH.CFG
        startmenu_rust_poll();      // throttled internally; live-applies system/user menu config + installs
        volosd_tick();        // #162 notice a volume/mute change (1 syscall)
        profile_tick();       // #92 persist UI settings on change
        stickies_tick();      // #270 persist sticky notes when changed
        // #566: pause notification aging while locked, so a toast's timer does
        // not silently expire unseen behind the lock overlay (it is never
        // rendered while locked either way - see render_frame() below).
        if (!g_session_locked) notif_tick();   // #168 poll notification spool + age toasts
        process_events();     // Dispatch events to UI layers

        // Apply a wallpaper change requested by another app (e.g. Settings),
        // so the Appearance panel's wallpaper selector takes effect live.
        {
            // #246 progressive: a wallpaper change shows a fast coarse pass
            // first, then refines to full resolution on the next frame.
            extern int  wallpaper_current(void);
            extern void wallpaper_load_progressive(int index);
            extern int  wallpaper_refine(void);
            int wp = get_wallpaper();
            if (wp >= 0 && wp != wallpaper_current()) {
                wallpaper_load_progressive(wp);
                g_needs_redraw = true;
            } else if (wallpaper_refine()) {
                g_needs_redraw = true;
            }
        }

        // Apply a theme change requested by Settings (system-wide chrome).
        {
            static int s_cur_theme = -1;
            int th = get_theme();
            if (th != s_cur_theme) {
                s_cur_theme = th;
                compositor_apply_theme(th);
                // (#704) compositor_apply_theme() above only restyles the
                // compositor's OWN chrome globals (taskbar/menus). Nothing
                // previously reached an already-open app window's content:
                // wm_invalidate_all() (called kernel-side by sys_set_theme)
                // only flips the dead kernel-fallback full_redraw flag, and
                // win_invalidate() deliberately does not arm redraw_pending
                // (#564). So a theme switch used to look "immediate" only
                // because it changed the taskbar/start menu; an open
                // Settings/Files window kept its old interior colours until
                // resized or reopened. Measured on golden 1026 (#704).
                wm_force_redraw_all();
                g_needs_redraw = true;
            }
        }

        // (#711) LIVE THEME-FILE RELOAD. The block above only notices a theme
        // INDEX change, i.e. someone picked a different theme in Settings. This
        // one notices the ACTIVE theme FILE being edited, which is the point of
        // the whole format being data: change a colour or a metric in
        // /THEMES/<slug>.mtheme on a running system and the UI follows, with no
        // compiler, no deploy and no reboot.
        //
        // Throttled to ~2s, matching dock_style_poll()/startmenu_prefs_poll():
        // the established throttled flat-file re-read pattern in this tree.
        // There is no filesystem watcher in this kernel, and adding one is not
        // in scope. Re-selecting the theme in Settings remains the explicit
        // trigger and takes effect immediately.
        //
        // compositor_apply_theme() must be re-run explicitly, because the theme
        // INDEX has not changed (the same slot was reloaded in place) and the
        // chrome colours are cached in globals, not read per draw.
        {
            static uint64_t s_theme_poll_due = 0;
            uint64_t now_tp = uptime_ms();
            if (now_tp >= s_theme_poll_due) {
                s_theme_poll_due = now_tp + 2000;
                if (gui_theme_poll_reload()) {
                    compositor_apply_theme(get_theme());
                    // (#704) same reasoning as the explicit-switch block
                    // above: a live file edit must reach open app windows
                    // too, not just the compositor's own chrome.
                    wm_force_redraw_all();
                    g_needs_redraw = true;
                }
            }
        }

        // Apply an icon-size change requested by Settings (live).
        {
            static int s_cur_icon = -1;
            int isz = get_icon_size();
            if (isz != s_cur_icon) {
                s_cur_icon = isz;
                compositor_apply_icon_size(isz);
                g_needs_redraw = true;
            }
        }

        // Apply display effects (brightness / night-light) requested by Settings.
        {
            static int s_fx = -1;
            int fx = get_display_fx();
            if (fx != s_fx) {
                s_fx = fx;
                g_brightness = fx & 0xFF;
                g_nightlight = (fx >> 8) & 0xFF;
                g_needs_redraw = true;
            }
        }

        // Apply a font-size change requested by Settings (#58).
        {
            static int s_fs = -1;
            int fs = get_font_size();
            if (fs != s_fs) {
                s_fs = fs;
                static const int px[4] = {12, 16, 20, 24};
                g_font_px = px[(fs >= 0 && fs < 4) ? fs : 1];
                g_needs_redraw = true;
            }
        }

        // Apply a screensaver-type change requested by Settings.
        {
            static int s_cur_ss = -1;
            int sst = get_screensaver();
            if (sst != s_cur_ss) {
                s_cur_ss = sst;
                screensaver_set_type(sst);
            }
        }

        // One-shot "Test Screensaver" trigger from Settings.
        if (get_ss_test()) {
            g_screensaver_active = true;
            screensaver_note_activated();   // #570: starts the input-ignore grace
            g_needs_redraw = true;
            { static int dfd = -1; if (dfd < 0) dfd = sys_open("/SSDEBUG.TXT", 0x41|0x200);
              if (dfd >= 0) { const char *m = "TRIGGER\n"; sys_write(dfd, m, 8); } }
        }

        // Screensaver animates continuously; always redraw when active.
        if (g_screensaver_active) {
            g_needs_redraw = true;
        }

        // Dirty-rectangle compositing (#102/#379): poll input at ~30Hz (cheap).
        // When anything INTERACTIVE is happening (recent input, an app window, a
        // drag, an open menu/dialog, or the screensaver) present a full frame,
        // exactly as before. Otherwise, on the pure-idle desktop, collect the
        // rectangles that actually changed (a clock tick, a sysmon sample, the
        // walking pet) and recomposite + present ONLY those - or present nothing
        // if nothing changed. This is what finishes #102: the idle desktop no
        // longer re-blits the whole 1280x800 wallpaper + every widget's TTF text
        // every frame, so the host core drops from ~pegged to single digits.
        {
            uint64_t now = uptime_ms();
            bool recent_input = (now - g_idle_ms) < 500;   // 0.5s (real ms)
            wm_window_info_t _w[16];
            int _n = wm_get_windows(_w, 16), _apps = 0;
            for (int _i = 0; _i < _n; _i++)
                if (_w[_i].visible && !_w[_i].minimized) _apps++;
            // #564: a window merely being OPEN no longer forces a full 30Hz
            // render on its own - that was the #548-measured dominant idle-CPU
            // cost (idle:76-86/COMPOSIT:9-10 vs idle:95-96/COMPOSIT<1 with zero
            // windows open, independent of whether the window's own content
            // ever changed). Instead ask the kernel WM whether anything about
            // any open window actually changed since the last composite
            // (move/resize/focus/create/close, or an app's own
            // win_invalidate() - see kernel/gui/window.c wm_is_dirty() / #379,
            // exposed read-only via SYS_WM_APPS_DIRTY). A window sitting open
            // but not presenting (idle Calculator, idle Files) now costs
            // nothing here; a window that IS animating (AssaultCube/Arena/
            // DOOM/glcube/musicplayer's dock, all of which invalidate every
            // frame via SDL_GL_SwapWindow or their own render loop) is dirty
            // every tick and renders at full rate, unthrottled.
            // #745 DAMAGE FIX. This used to be `(_apps > 0) && wm_apps_dirty()`.
            // _apps is counted from the CURRENT window list, so on the tick
            // where the LAST window closes it is already 0, and the
            // wm_invalidate_all() that window_destroy() just raised was never
            // read - no full composite, and whatever the window (and now its
            // shadow band) had been covering stayed on screen until something
            // unrelated forced a redraw. In practice a close is preceded by a
            // click, so recent_input usually papered over it; a shadow makes
            // the hole 32 px wider on every side and much easier to see. Also
            // consult the flag for one tick AFTER the count drops to zero.
            // Bounded: that tick renders a full frame, SYS_COMPOSITOR_RENDER_-
            // WINDOWS clears the flag, s_prev_apps becomes 0, and the next tick
            // returns to the idle path.
            static int s_prev_apps = 0;
            bool apps_dirty = (_apps > 0 || s_prev_apps > 0) && (wm_apps_dirty() != 0);
            s_prev_apps = _apps;
            // #596: a true-fullscreen window that presented a frame this tick
            // counts as activity. Record the timestamp; screensaver.c's idle
            // check consults it and refuses to activate while it is fresh.
            // Reuses the window list already fetched above (no extra syscall).
            if (apps_dirty && fullscreen_in_list(_w, _n))
                g_fs_present_ms = now;
            // T0 #578: everything that forces a FULL composite, EXCEPT plain
            // recent pointer input. When only recent_input is set and this tick
            // was pointer-motion-only, the cheap cursor-rect path handles it.
            bool other_interactive = apps_dirty ||
                        s_dragging_sheep || s_dragging_dog || s_dragging_widget ||
                        s_dragging_sticky || stickies_editing() ||
                        s_dragging_desktop || widget_menu_is_open() ||
                        g_start_menu_open || g_context_menu_open ||
                        g_wallpaper_picker_open || g_tray_menu_open ||
                        g_launcher_open ||
                        startmenu_power_confirm_open() || startmenu_properties_open() ||
                        widget_settings_is_open() || taskbar_popup_active() ||
                        g_session_locked ||   // #566: always full-render so the lock screen's clock visibly ticks
                        // (#745) DOCK_XFCE hover ease in flight: without this,
                        // pure pointer motion over the dock (no menu, no drag)
                        // takes the cheap render_frame_cursor() path below,
                        // whose clip is a tiny box around the cursor - far
                        // smaller than the dock strip a growing/lifting
                        // neighbour icon needs redrawn. Forcing the full
                        // composite for exactly the ~120ms an ease is in
                        // flight (never longer: the flag clears itself once
                        // every slot settles) keeps the effect from being
                        // silently clipped away. DOCK_LUMINA's own
                        // proximity-magnify has the identical latent bug and
                        // was NOT fixed here (out of scope for #745's marble
                        // dock ask) - see blame.md.
                        taskbar_dock_animating() ||
                        // Cross-window drag ghost in flight. SAME shape as
                        // taskbar_dock_animating() directly above: the ghost
                        // is a card that moves with the pointer and is far
                        // larger than the cursor-rect clip the cheap path
                        // uses, so without this it would be clipped away and
                        // leave a trail. Bounded by construction: the flag is
                        // false again the moment the button comes up.
                        dragghost_active() ||
                        // #148 (local 164): the PrintScreen fullscreen toast's
                        // 2s dwell. SAME shape as taskbar_dock_animating()
                        // just above - force the full path (which is what
                        // reaches native_fullscreen_try_render(), the only
                        // place this toast is ever drawn or erased) for
                        // exactly as long as the toast is armed, so a
                        // perfectly STATIC fullscreen app (no input, nothing
                        // else forcing a tick) still gets a frame to draw the
                        // toast on AND a frame afterward to erase it by
                        // simply not drawing it. Without this fold-in, a
                        // static fullscreen app could leave the toast on
                        // screen indefinitely past its 2s budget (frozen,
                        // not ghosted - see screenshot.c) until something
                        // UNRELATED forced the next tick.
                        screenshot_fs_toast_active();
            // #COMPIDLE: A TICK IN WHICH NOTHING HAPPENED HAS NOTHING TO DRAW.
            //
            // `recent_input` stays true for 500ms after the last pointer or
            // key event, and while it is true loop_sleep_ms drops to 8ms. So a
            // single click used to buy roughly SIXTY consecutive full-screen
            // composites-and-presents, of which the first one drew the change
            // and the other fifty-nine reproduced it byte for byte. At
            // 1280x800 in a VM that is invisible. At 1920x1080 on the owner's
            // laptop it is 2.07 million pixels composited from scratch, sixty
            // times, per click - and if anything keeps `recent_input` pinned
            // (a touchpad that reports a one-pixel delta with a hand resting
            // near it is the obvious candidate on a laptop and cannot happen
            // in a VM) it stops being a burst and becomes the steady state.
            //
            // A tick qualifies as QUIET only if EVERY input to the frame is
            // unchanged: no pointer motion, no button/key, nothing in the
            // other_interactive set (menus, drags, animations, the lock, an
            // app window invalidating itself), and no explicit redraw request.
            // Under those conditions the composite is provably identical to
            // the one already on screen, so it is skipped and the tick falls
            // through to the idle path, which still services widget, taskbar
            // and toast damage exactly as it does at true idle.
            //
            // LATENCY IS NOT TRADED AWAY: the 8ms poll interval is still
            // applied below whenever recent_input holds, so the very next tick
            // that DOES carry an event is serviced just as fast as before.
            //
            // #158's carve-out is honoured: while a window is fullscreen,
            // render_frame()'s fast path (and the watchdog/modal-reclaim
            // checks inside it) must run every tick, so a fullscreen window
            // disqualifies the whole optimisation exactly as it disqualifies
            // cursor_only below.
            bool quiet_tick = recent_input && !g_tick_motion && !g_tick_nonmotion &&
                              !other_interactive && !g_needs_redraw &&
                              !g_screensaver_active && !fullscreen_in_list(_w, _n);
            if (quiet_tick) idleprof_reason(IP_R_QUIET);
            bool interactive = (recent_input && !quiet_tick) ||
                               g_screensaver_active || other_interactive;

            // #COMPIDLE: record WHY, from the same booleans, before any of
            // them is consumed. This is the whole diagnostic: on the owner's
            // laptop one of these counters will be pinned near the tick rate
            // and on every VM measured here they are all near zero.
            {
                unsigned _r = 0;
                if (recent_input)             _r |= IP_R_INPUT;
                if (apps_dirty)               _r |= IP_R_APPSDIRTY;
                if (g_screensaver_active)     _r |= IP_R_SCRSAVER;
                if (s_dragging_sheep || s_dragging_dog || s_dragging_widget ||
                    s_dragging_sticky || s_dragging_desktop || dragghost_active())
                                              _r |= IP_R_DRAG;
                if (widget_menu_is_open() || g_start_menu_open ||
                    g_context_menu_open || g_wallpaper_picker_open ||
                    g_tray_menu_open || g_launcher_open ||
                    startmenu_power_confirm_open() || startmenu_properties_open() ||
                    widget_settings_is_open() || taskbar_popup_active() ||
                    stickies_editing())       _r |= IP_R_MENU;
                if (g_session_locked)         _r |= IP_R_LOCKED;
                if (taskbar_dock_animating() || screenshot_fs_toast_active())
                                              _r |= IP_R_DOCKANIM;
                if (g_needs_redraw)           _r |= IP_R_REDRAW;
                idleprof_reason(_r);
            }
            // Cursor-move-only: the pointer moved this tick and nothing else of
            // consequence is happening. Repaint just the old+new cursor rects.
            // #158: a partial cursor-rect present only ever repaints a small
            // box around the pointer, so any OTHER pixel left over from
            // before native fullscreen was entered (most visibly the
            // taskbar) would linger on screen indefinitely - the fast path
            // in render_frame() (and the watchdog/modal-reclaim checks
            // inside it) must run EVERY tick a window is fullscreen, not
            // just on ticks with non-motion activity. Reuses
            // fullscreen_in_list() (already computed above for
            // g_fs_present_ms): window_fullscreen_enter() sets bounds to the
            // exact full screen, so that bounds check also catches native
            // fullscreen with no separate predicate to keep in sync.
            bool cursor_only = g_tick_motion && !g_tick_nonmotion &&
                               !other_interactive && !g_needs_redraw &&
                               !fullscreen_in_list(_w, _n);
            static int s_drawn_cursor_x = 0, s_drawn_cursor_y = 0;

            if (g_screensaver_active) {
                idleprof_path(IP_P_SCRSAVER);           // #COMPIDLE
                idleprof_damage_px((unsigned long)g_fb_width * (unsigned long)g_fb_height);
                render_frame();           // screensaver owns the whole display
                g_needs_redraw = false;
                vnc_mark_full_dirty();    // #440: whole screen changed
            } else if (interactive || g_needs_redraw) {
                // #440: only tell VNC the whole screen changed when it actually
                // did (recent input or an explicit redraw). A window/menu merely
                // sitting open keeps this branch "interactive" every frame; if we
                // marked full-dirty unconditionally we would resend the entire
                // ~4MB frame every ~33ms over VNC for no visual change, which
                // burns bandwidth (and hits the kernel TCP send limit fast). A
                // static interactive frame gets recomposited locally but is NOT
                // re-pushed to the remote viewer.
                bool vnc_changed = recent_input || g_needs_redraw;
                if (cursor_only) {
                    // T0 #578: only the pointer moved. Recomposite + present just
                    // the old+new cursor rects (render_frame_cursor feeds VNC the
                    // exact rect itself, so do NOT mark the whole screen dirty).
                    idleprof_path(IP_P_CURSOR);         // #COMPIDLE
                    render_frame_cursor(s_drawn_cursor_x, s_drawn_cursor_y, _w, _n);
                } else {
                    // perf62: tag the upcoming present by WHY it's happening -
                    // an app's own content changing ("windowed") versus plain
                    // desktop interaction (menu/drag/typing, "interactive") -
                    // #62's premise is that these differ, so they must not
                    // share one bucket. apps_dirty is exactly the signal
                    // computed above for this same tick.
                    s_pf_full_reason = apps_dirty ? "windowed" : "interactive";
                    idleprof_path(IP_P_FULL);           // #COMPIDLE
                    idleprof_damage_px((unsigned long)g_fb_width * (unsigned long)g_fb_height);
                    render_frame();       // full-screen composite + present
                    if (vnc_changed) vnc_mark_full_dirty();
                }
                g_needs_redraw = false;
                // Mouse feel: while the user is actively moving the pointer /
                // typing, poll + present at ~120Hz so the cursor tracks the hand
                // with low latency instead of the 33ms (30Hz) desktop cadence.
                // The cursor is re-composited every frame here (render_frame draws
                // it last), so it is never gated by the idle partial-present path.
            } else {
                // Not interactive this tick. render_frame_idle()'s partial
                // widget/desktop redraw does NOT recomposite app-window
                // content (only render_frame()/compositor_render_windows()
                // above does that) - it is only safe to touch the framebuffer
                // here when there is NOTHING on top that its damage rects
                // could paint over. With zero visible windows that is always
                // true (unchanged from before #564). With one or more windows
                // open but every one of them fully static (apps_dirty was
                // false above, or #564 would have taken the interactive
                // branch instead), the correct and only safe thing is to do
                // NOTHING: no desktop-widget redraw, no present. This is the
                // fix for #564 - previously reaching this situation was
                // impossible (_apps > 0 always forced the interactive branch
                // above at 30Hz forever, which is exactly the idle-CPU cost
                // #548 measured).
                static int quiet = 0;
                if (_apps > 0) {
                    // (local 80) One or more windows are open but every one of
                    // them is static. The desktop layer behind them stays
                    // suppressed (#564): no widgets_collect_damage() here, so
                    // an occluded pet or desktop clock still costs nothing. The
                    // always-on-top chrome does NOT stay suppressed, or the
                    // tray gauges, the bar clock and toasts freeze for as long
                    // as a window is open. Both collectors below are self-
                    // throttling and add damage only when a DISPLAYED value
                    // actually changed (~1Hz for the gauges), so the steady
                    // state is one small clipped present per second, not a 30Hz
                    // composite: the #564 idle-CPU win is kept.
                    damage_reset();
                    // Nothing to repaint when a fullscreen app is on top:
                    // render_frame_body() gates the taskbar off in that state,
                    // so collecting its damage would present a rect that has no
                    // taskbar in it. Same gate, read from the same helper.
                    if (!fullscreen_app_on_top()) taskbar_collect_damage();
                    notif_collect_damage();
                    if (damage_count() > 0) {
                        idleprof_path(IP_P_CHROME);     // #COMPIDLE
                        idleprof_reason(IP_R_TASKBAR);
                        render_frame_chrome(_w, _n);
                        quiet = 0;
                        loop_sleep_ms = 50;
                        // #440 parity: feed the same rects to the RFB layer.
                        int dn = damage_count();
                        for (int di = 0; di < dn; di++) {
                            int dx, dy, dw, dh;
                            if (damage_get(di, &dx, &dy, &dw, &dh)) {
                                vnc_mark_rect_dirty(dx, dy, dw, dh);
                                idleprof_damage_px((unsigned long)dw * (unsigned long)dh);
                            }
                        }
                    } else {
                        if (quiet < 60) quiet++;
                        loop_sleep_ms = (quiet > 6) ? 120 : 50;
                    }
                } else {
                // Pure idle: recomposite + present ONLY the changed rectangles,
                // or present nothing when nothing changed. widgets_collect_damage
                // advances pets/sysmon on their own time cadence (so animation
                // speed is independent of the poll rate) and records damage.
                damage_reset();
                widgets_collect_damage();
                taskbar_collect_damage();
                notif_collect_damage();   // #585: toast slide/settle rects only, never full-screen
                if (damage_count() > 0) {
                    idleprof_path(IP_P_IDLE);           // #COMPIDLE
                    idleprof_reason(IP_R_WIDGET);
                    render_frame_idle();
                    quiet = 0;
                    loop_sleep_ms = 50;    // animating idle: ~20Hz poll is plenty
                    // #440: feed the exact same damage rects to the RFB layer so
                    // a connected VNC viewer gets a real incremental update
                    // instead of a full-frame resend.
                    int n = damage_count();
                    for (int di = 0; di < n; di++) {
                        int dx, dy, dw, dh;
                        if (damage_get(di, &dx, &dy, &dw, &dh)) {
                            vnc_mark_rect_dirty(dx, dy, dw, dh);
                            idleprof_damage_px((unsigned long)dw * (unsigned long)dh);
                        }
                    }
                } else {
                    if (quiet < 60) quiet++;
                    // Static desktop: nothing changed. Back the poll interval off
                    // so an idle desktop stops burning CPU on the 30Hz loop. Any
                    // input makes the next iteration interactive (-> 33ms) and a
                    // per-second clock/sysmon tick still wakes us in time.
                    loop_sleep_ms = (quiet > 6) ? 120 : 50;
                }
                }
            }
            // Mouse feel: while the user is actively moving the pointer /
            // typing, poll + present at ~120Hz so the cursor tracks the hand
            // with low latency instead of the 33ms (30Hz) desktop cadence.
            // #COMPIDLE moved this out of the interactive branch: a quiet tick
            // now takes the idle path (which would otherwise back the interval
            // off to 50/120ms), and backing off the POLL rate inside the
            // post-input window is precisely the latency regression this must
            // not cause. Applied last so it overrides whichever branch ran.
            if (recent_input) loop_sleep_ms = 8;

            // T0 #578: remember where the cursor was last drawn so the next
            // cursor-only tick can damage the vacated rect. Every present path
            // above draws the cursor at the current g_mouse position.
            s_drawn_cursor_x = g_mouse_x;
            s_drawn_cursor_y = g_mouse_y;
        }

        // Remote screen capture: check for a /SCREENSHOT.REQ trigger at the
        // normal frame cadence (no busy-wait). g_fb now holds the frame we just
        // presented, so the capture matches exactly what is on screen.
        screenshot_poll();

        // Live VNC/RFB server (#440): rides this same adaptive cadence, never
        // blocks (see vnc.c for the no-busy-wait / non-blocking socket design).
        vnc_poll();

#ifdef MAYTERA_TESTHOOK
        // #334 headless verification hook - see testhook.c. NEVER present in a
        // normal build (only exists with `make TESTHOOK=1`); the #ifdef means
        // this call site itself does not exist in the shipping COMPOSIT.
        testhook_poll();
#endif

        idleprof_tick_end();        // #COMPIDLE: closes this tick's timing
        sys_sleep(loop_sleep_ms);   // adaptive: 33ms active, up to 120ms idle
    }

    // Not reached; suppress compiler warnings about missing return
    return 0;
}
