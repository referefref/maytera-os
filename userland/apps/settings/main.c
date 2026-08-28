// settings - Comprehensive System Settings for MayteraOS
// Full-featured settings application with 12 panels
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_font.h"
#include "../../libc/gui_scroll.h"   // (#291/#438) shared scrollable-viewport primitive
#include "../../libc/gui_list.h"     // (#512) shared scrollable-listbox primitive
#include "../../libc/syscall.h"
#include "../../libc/wallpapers.h"   // (#517) shared wallpaper enumeration (same as compositor)
#include "../../libc/gui_theme.h"    // (#565) file-based theme loader (same as compositor/App Store)
#include "../../libc/gui_dock.h"     // (#745) THE dock-style name list, shared with the OOBE wizard
#include "../../libc/theme.h"        // theme_color()/THEME_COLOR_* + get_theme()/set_theme()
#include "../../libc/uiscale.h"   // the GLOBAL UI scale factor (one value, kernel-owned)
#include "../../libc/assoc.h"
#include "../../libc/devinfo.h"      // (#382) real CPU/RAM (SYS_SYSINFO) + PCI (SYS_DEV_PCI_LIST)
#include "../../libhelp/help_ui.h"   // (#267) help subsystem: tooltips, "?" icon, F1
#define BT_STUB_IMPL                 // (#237, was BT_MOCK_IMPL/#372) this TU owns the stub state
#include "../../libc/bt_client.h"    // (#237) Bluetooth client API + honest stub (no driver)
#define WIFI_STUB_IMPL               // (#237, was WIFI_MOCK_IMPL/#384) this TU owns the stub state
#include "../../libc/wifi_client.h"  // (#237) Wi-Fi client API + honest stub (no driver)
#include "userconf.h"   // #683: per-user preference paths
#include "../../libc/tz.h"        // #49/#50: THE timezone list, setting and clock
#include "../../libc/dock_opacity.h"  // #132: shared DOCK_OPACITY_MIN/MAX/DEFAULT/WARN

// #745: the length of an option array, derived FROM the array. Every dropdown
// count, every "is this index valid" clamp and every option-button count in
// this file goes through this macro or a *_COUNT built from it. They used to
// be hand-written literals kept in step by hand, and DOCK_OPTS had already
// fallen out of step: the clamp said 3 and the dropdown count said 4 while the
// array held 5, so the fifth dock style could not be displayed, could not be
// picked, and was silently reset to 0 by a mere repaint of the Appearance
// panel. Adding a sixth entry to any list below must not be able to do that
// again, so nothing here counts an array by eye.
#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
// Is `i` a valid index into option array `a`? Use this instead of writing the
// upper bound out as a number.
#define OPT_OK(i, a) ((i) >= 0 && (i) < ARRAY_COUNT(a))
// Clamp `i` to a valid index into `a`, falling back to 0.
#define OPT_CLAMP(i, a) (OPT_OK((i), (a)) ? (i) : 0)

// #127: route all in-window text through the antialiased TrueType path (app-font
// test). All win_draw_text(...) calls below render in TTF instead of the bitmap font.
#define win_draw_text(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define win_draw_text_small(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 11, (c))

// Window dimensions. These hold the CONTENT size (the drawable canvas), not the
// outer window size: win_create() takes the outer size and the kernel subtracts
// the chrome, so passing the content size we want would silently lose 24px of
// height. g_win_w/g_win_h are authoritative because they are set from
// win_get_size() right after create (and from EVENT_RESIZE afterwards), never
// assumed from the win_create() arguments.
static int g_win_w = 900, g_win_h = 726;  // #89: live window size (EVENT_RESIZE)
#define WIN_WIDTH       g_win_w
#define WIN_HEIGHT      g_win_h
#define SIDEBAR_WIDTH   180

// --- Sidebar geometry ------------------------------------------------------
// Derived from PANEL_COUNT, never hardcoded: adding an 18th panel must not
// silently clip the list again. That is exactly the #436 bug class (a default
// window height too small for its own content, only caught on real hardware).
#define SIDEBAR_TOP        50   // y of the first panel row
#define PANEL_PITCH        38   // row-to-row spacing
#define PANEL_ROW_H        34   // height of a row's hit/fill box
#define SIDEBAR_GAP         4   // gap between the last row and the footer rule
#define SIDEBAR_FOOTER_H   26   // separator + version line
// Full height the panel list wants. A whole number of rows by construction, so
// the viewport can be floored to a row multiple with no ragged remainder.
#define SIDEBAR_LIST_FULL  (PANEL_COUNT * PANEL_PITCH)
// Content height at which every panel is visible with no scrolling at all.
#define SIDEBAR_NATURAL_H  (SIDEBAR_TOP + SIDEBAR_LIST_FULL + SIDEBAR_GAP + SIDEBAR_FOOTER_H)

// Window chrome cost, from the kernel's gui/window.h: TITLEBAR_HEIGHT(20) plus
// BORDER_WIDTH(2) on each side, and 2*BORDER_WIDTH horizontally.
#define SET_CHROME_H       24
#define SET_CHROME_W        4
// The compositor's taskbar (compositor.h TASKBAR_HEIGHT). There is no syscall
// that reports it, so it is mirrored here; keep the two in step.
#define SET_TASKBAR_H      36
#define SET_MARGIN          6   // breathing room from the screen edges
#define SET_WANT_W        900   // preferred content width

// Preferred default width/height. The real values are computed at startup from
// the actual framebuffer (fb_info), because the framebuffer is whatever the
// firmware set: 1280x800 on the OVMF VMs, 1920x1080 on the iMac14,4 target.
// Assuming one of those is the Squadron bug in blame.md ("on the 1920x1080 iMac
// the window became a 1280x800 corner box").
#define CONTENT_X       (SIDEBAR_WIDTH + 1)
#define CONTENT_WIDTH   (WIN_WIDTH - SIDEBAR_WIDTH - 1)
#define CONTENT_HEIGHT  WIN_HEIGHT

// Character dimensions
#define CHAR_W          8
#define CHAR_H          16
#define LINE_HEIGHT     24
#define PADDING         20
#define SMALL_PAD       10

// Panel IDs - 12 comprehensive panels
enum {
    PANEL_APPEARANCE = 0,
    PANEL_DISPLAY,
    PANEL_SOUND,
    PANEL_NETWORK,
    PANEL_KEYBOARD,
    PANEL_MOUSE,
    PANEL_DATETIME,
    PANEL_USERS,
    PANEL_PRIVACY,
    PANEL_STORAGE,
    PANEL_DEFAULTS,
    PANEL_ABOUT,
    PANEL_NOTIFICATIONS,
    PANEL_DEVICES,
    PANEL_BLUETOOTH,     // (#372) Bluetooth devices panel (index 14)
    PANEL_WIFI,          // (#384) Wi-Fi panel (index 15)
    PANEL_EXTSVC,        // (#414) External Services (Home Assistant)
    PANEL_STARTMENU,     // (#: Start Menu uplift) layout/search/favorites/recents prefs
    PANEL_DOCK,          // (#745 task #67 "dockpanel") dock behaviour + contents
    PANEL_COUNT
};

// Panel names
static const char* panel_names[PANEL_COUNT] = {
    "Appearance",
    "Display",
    "Sound",
    "Network",
    "Keyboard",
    "Mouse",
    "Date & Time",
    "Users",
    "Privacy",
    "Storage",
    "Default Apps",
    "About",
    "Alerts",
    "Devices",
    "Bluetooth",
    "Wi-Fi",
    "External Services",
    "Start Menu",
    "Dock"
};

// Panel icons
static const char* panel_icons[PANEL_COUNT] = {
    "[P]",  // Palette
    "[D]",  // Display
    "[S]",  // Sound
    "[N]",  // Network
    "[K]",  // Keyboard
    "[M]",  // Mouse
    "[T]",  // Time
    "[U]",  // Users
    "[L]",  // Lock/Privacy
    "[H]",  // HDD/Storage
    "[A]",  // Associations / Default Apps
    "[i]",  // Info
    "[N]",  // Alerts
    "[R]",  // Devices / Printers
    "[B]",  // Bluetooth
    "[W]",  // Wi-Fi
    "[X]",  // External Services
    "[Y]",  // Start Menu
    "[Z]"   // Dock
};

// =============================================================================
// Theme Colors - Comprehensive color scheme
// =============================================================================

// Dark theme (default)
static uint32_t COL_SIDEBAR_BG;
// (#745) Ink for the taskbar SURFACE. The sidebar is filled with
// THEME_COLOR_TASKBAR_BG, but its labels/icons/version used COL_TEXT_PRIMARY
// (label_text, contracted against window_bg), COL_TEXT_DISABLED
// (button_disabled, against button_bg) and COL_TEXT_SECONDARY
// (menu_text_disabled, against menu_bg). On a theme with a light window and a
// dark bar - Ocean, Forest, Sunset - that is dark ink on a dark surface, and
// nothing caught it because the kernel's runtime contrast floor only ever
// checks label_text against window_bg. These three bind to tokens whose
// contract IS the surface they are painted on.
static uint32_t COL_SIDEBAR_TEXT;
static uint32_t COL_SIDEBAR_MUTED;
static uint32_t COL_SIDEBAR_SEL_TEXT;
static uint32_t COL_CONTENT_BG;
static uint32_t COL_PANEL_NORMAL;
static uint32_t COL_PANEL_HOVER;
static uint32_t COL_PANEL_ACTIVE;
static uint32_t COL_SEPARATOR;
static uint32_t COL_TEXT_PRIMARY;
static uint32_t COL_TEXT_SECONDARY;
static uint32_t COL_TEXT_DISABLED;
static uint32_t COL_ACCENT;
static uint32_t COL_ACCENT_HOVER;
static uint32_t COL_SUCCESS;
static uint32_t COL_WARNING;
static uint32_t COL_ERROR;
static uint32_t COL_INPUT_BG;
static uint32_t COL_INPUT_BORDER;
static uint32_t COL_SLIDER_TRACK;
static uint32_t COL_SLIDER_FILL;
static uint32_t COL_BUTTON_BG;
static uint32_t COL_BUTTON_HOVER;
static uint32_t COL_CHECKBOX_BG;
static uint32_t COL_CARD_BG;

// Accent color presets
// (#704) INTENTIONALLY NOT themed: this IS the palette the user picks
// COL_ACCENT (a theme token, applied via apply_theme() below) FROM. Swapping
// these swatches for theme colors would be circular - the user could no
// longer see or choose a fixed "Blue"/"Red"/etc, only whatever the current
// theme's accent already is. Same reasoning applies to avatar_palette()
// below (per-account identity tint) and to wp_fill_fallback()'s per-index
// gradient colors (fallback WALLPAPER thumbnail content, not chrome).
static const uint32_t ACCENT_COLORS[] = {
    0x00569CD6,  // Blue
    0x0066BB66,  // Green
    0x00DD8844,  // Orange
    0x00AA66DD,  // Purple
    0x00DD5555,  // Red
    0x0044AAAA,  // Teal
    0x00DDAA33,  // Gold
    0x00FF6699   // Pink
};
#define NUM_ACCENT_COLORS ARRAY_COUNT(ACCENT_COLORS)   /* #745: was a literal 8 */

// =============================================================================
// State Variables
// =============================================================================

static int window_handle = -1;
static int current_panel = PANEL_APPEARANCE;
static int hover_panel = -1;
static int win_x = 80;
static int win_y = 40;

// (#267) help integration: geometry of the in-window "?" help icon
// (window-relative, drawn top-right of the content area) and the help file.
#define HELP_Q_D    20
#define HELP_Q_X    (WIN_WIDTH - HELP_Q_D - 14)
#define HELP_Q_Y    12
static const char *HELP_FILE = "/HELP/SETTINGS.MHLP";
static int g_help_lx = -1, g_help_ly = -1;  // last mouse pos (window-rel)
// Map the current Settings panel to a help topic id.
static const char *help_topic_for_panel(int panel) {
    switch (panel) {
        case PANEL_APPEARANCE: return "appearance";
        case PANEL_DISPLAY:    return "display";
        case PANEL_SOUND:      return "sound";
        case PANEL_NETWORK:    return "network";
        default:               return "settings";
    }
}

// Scroll state for content area (#227). Was a bespoke content_scroll_y/
// max_scroll_y pair where max_scroll_y was assigned 0 and NEVER anything
// else, so the wheel handler's clamp always forced the offset back to zero
// and content_scroll_y itself was never subtracted anywhere in the draw
// path - below-the-fold controls were permanently unreachable. Replaced
// with the shared primitive already used for the sidebar (g_side_scroll)
// and the Credits modal (g_credits_scroll, both userland/libc/gui_scroll.h)
// instead of inventing a second scrollable-viewport widget.
static gui_scroll_t g_content_scroll;
// Recorded at the end of every draw_X_panel(): the panel's true content
// bottom in the SAME PADDING-relative coordinate space draw_content() reads
// it back in, so the true content height can be derived after the fact
// (the scroll offset the draw pass applied cancels out - see draw_content()).
static int g_content_bottom_y = 0;

// Active dropdown/popup state
static int active_dropdown = -1;
static int dropdown_hover_item = -1;

// =============================================================================
// Settings State - Appearance
// =============================================================================
static int current_theme = 0;       // (#565) index into g_th[]/g_th_names[] (file-based, see th_init())
static int screensaver_idx = 21;    // #124: 21 = "Plasma (Classic)" -> kernel id 22.
                                    // MUST agree with kernel g_screensaver_type (22) and the
                                    // compositor's g_ss_type (SS_PLASMACLASSIC). Overwritten at
                                    // startup by the read-only sync from the live kernel value
                                    // below, so this only shows if that lookup finds nothing.
                                    // (idx space: 0=Off,1=Starfield,2=Flux,3=Lines,4=Bubbles,...)
static int screensaver_delay_min = 10;  // (#115/#652) activation delay, minutes; MUST match kernel g_screensaver_delay (600s) and compositor SS_DEFAULT_TIMEOUT
static const int SS_DELAY_STEPS[] = {1, 2, 5, 10, 15, 20, 30, 45, 60};
#define SS_DELAY_NSTEPS ((int)(sizeof(SS_DELAY_STEPS)/sizeof(SS_DELAY_STEPS[0])))
static int  ss_delay_index(void);
static void ss_delay_label(char *buf);
static void ss_set_delay_index(int di);
static int accent_color_idx = 0;    // Index into ACCENT_COLORS
static uint32_t custom_accent = 0x00569CD6;  // Custom accent color
static int font_size = 1;           // 0=Small(12), 1=Medium(14), 2=Large(16), 3=XL(18)

// #351: the system UI FONT (family/style/size), distinct from the four-step
// "Font Size" above. Settings owns no font UI of its own: it hands this to the
// shared gui_font_dialog() and applies whatever comes back.
static gui_font_sel_t g_uifont;
static int icon_size = 1;           // 0=Small, 1=Medium, 2=Large
static bool animations_enabled = true;
static int animation_speed = 1;     // 0=Slow, 1=Normal, 2=Fast
static int transparency_level = 80; // 0-100%
// (#745) Dock/chrome glass opacity, percent OPAQUE, DOCK_OPACITY_MIN..MAX
// (dock_opacity.h). SAME SENSE as transparency_level above: the subsection is
// titled "Transparency" but it stores OPACITY (transparency_level =
// get_win_opacity() * 100 / 255), so inverting this one because the heading
// says Transparency would make the two rows mean opposite things while both
// looked correct.
// (#745 dockgrey, 2026-08-12) Floor 60->70 and default 90->75: see
// userland/apps/compositor/draw.c glass_render()'s floor comment for why the
// two moved together (the glass tint got lighter, which trades away
// low-opacity margin against a bright wallpaper).
// (#132) THIS FILE USED TO OWN A PRIVATE `#define DOCK_OPACITY_FLOOR 70`,
// "kept in sync" with the compositor's own copies ACROSS TWO BINARIES only by
// a comment - exactly the drift this project's blame.md warns about, and
// exactly why the owner reported none of his below-70 requests appeared to
// land (task #67 "dockpanel" already found a FOURTH silently-desynced copy of
// this same 60/70 literal in this file alone). dock_opacity.h is now the ONE
// definition, included by both this app and the compositor, so there is
// nothing left to keep in sync. DOCK_OPACITY_MIN is the real enforced floor
// (15, an implementation floor - see dock_opacity.h for why not 0);
// DOCK_OPACITY_WARN (73) is only where the UI below starts warning about
// contrast. Every site in this file that used to spell either number as a
// bare literal goes through these two, so this file cannot drift internally
// the way it did before.
static int dock_opacity = DOCK_OPACITY_DEFAULT;
// (#123 items 2/3) Marble dock geometry. Every bound here MUST match
// taskbar.c's XFCE_DOCK_H_MIN/MAX and XFCE_DOCK_ZOOM_MIN/MAX and profile.c's
// load clamps - four copies of a range in three binaries is exactly the drift
// this file's own DOCK_OPACITY_FLOOR comment (above) was written about, so the
// bounds are spelled ONCE here per binary and every site below goes through
// these names rather than a bare literal.
#define DOCK_HEIGHT_MIN      44
#define DOCK_HEIGHT_MAX      96
#define DOCK_HEIGHT_DEFAULT  59   // must match taskbar.c XFCE_DOCK_H_DEFAULT
#define DOCK_ZOOM_MIN       100
#define DOCK_ZOOM_MAX       200
#define DOCK_ZOOM_DEFAULT   128   // must match taskbar.c XFCE_DOCK_ZOOM_DEFAULT
static int dock_height = DOCK_HEIGHT_DEFAULT;
static int dock_zoom   = DOCK_ZOOM_DEFAULT;
// (#116) 0=Light 1=Dark 2=Glow. SAME index space as the compositor's
// UIPROFIL.YML curstyle and the kernel's SYS_SET_CURSOR, not a private one.
// #745: synced FROM the kernel at startup, never pushed INTO it; the only
// fallback is 0, the normal arrow. The old comment here still described the
// pre-#116 "Default/Dark/Light/Large" list that has not existed for months.
static int cursor_theme = 0;
static int dock_style = 0;          // #387 0=Default 1=Lumina 2=Classic UNIX 3=Retro Bench 4=Marble
                                    // (#26 internal enum stays DOCK_XFCE; #745 relabel only)
static int appearance_needs_restart = 0; // 1 if font/icon size changed

// Wallpaper selector (#517). Previously two hardcoded arrays (names + files) that
// had to stay index-matched with the compositor's own hardcoded list; they drifted
// (dead CLASSIC/DARKMODE/RETRO entries, 47 of 65 shipped BMPs unreachable). Now both
// Settings and the compositor enumerate the wallpapers from the image via the SAME
// wp_enumerate() (libc/wallpapers.h), so the shared index can never diverge or point
// at an absent file. Populated once by wp_init(); g_wp[i].file[0]==0 => gradient.
static wp_entry_t g_wp[WP_MAX_ENTRIES];
static int        g_wp_count = 0;
static void wp_init(void) { if (!g_wp_count) g_wp_count = wp_enumerate(g_wp, WP_MAX_ENTRIES); }
#define WALLPAPER_NAMES_COUNT g_wp_count
static int wallpaper_idx = 0;

// Theme picker (#565): the same pattern as the wallpaper picker above, and
// for the same reason - themes now live as files (/THEMES/*.mtheme, listed
// by /THEMES/INDEX.TXT), built-ins and anything the App Store installs
// alike, so a hardcoded name table can never drift from what actually
// exists on disk. gui_theme_list() is the single enumeration both Settings
// and (indirectly, via the same /THEMES/INDEX.TXT) the kernel's boot loader
// agree on.
static gui_theme_entry_t g_th[GUI_THEME_MAX_ENTRIES];
static int               g_th_count = 0;
static const char        *g_th_names[GUI_THEME_MAX_ENTRIES];
static void th_init(void) {
    if (!g_th_count) {
        g_th_count = gui_theme_list(g_th, GUI_THEME_MAX_ENTRIES);
        for (int i = 0; i < g_th_count; i++) g_th_names[i] = g_th[i].name;
    }
    if (g_th_count == 0) {
        // No /THEMES/INDEX.TXT (a golden built before #565, or FAT-only with
        // no ext2 root) - degrade to a single "System Default" placeholder
        // rather than an empty, unusable dropdown.
        g_th_names[0] = "System Default";
        g_th_count = 1;
    }
}

// =============================================================================
// Settings State - Display
// =============================================================================
static int brightness = 80;
static bool night_light = false;
static int night_light_strength = 50;
static int night_light_start_hour = 20;
static int night_light_end_hour = 6;
// #230: Scale/Color Temp/Gamma R/G/B were deleted (docs/SETTINGS_CONTROL_AUDIT.md
// #224 found no click handler existed for any of them - they could not even be
// changed by mouse - and no compositor/GPU LUT hook consumes them either way).
// A control that cannot work is worse than an absent one.

// #745 (local 102): display rotation. rotation_active is what THIS session
// actually booted with (SYS_GET_ROTATION, read once at startup - see
// hwinfo_load()-adjacent init below); rotation_idx is the DROPDOWN'S current
// selection, which starts equal to rotation_active and only diverges once the
// user picks something different. That divergence IS the "applies after
// restart" indicator - see draw_display_panel()'s hint - so no separate
// "pending" boolean is needed.
static int rotation_active = 0;
static int rotation_idx = 0;

// Resolution options
static const char* resolutions[] = {
    "800x600", "1024x768", "1280x720", "1280x800",
    "1366x768", "1440x900", "1600x900", "1920x1080",
    "2560x1440", "3840x2160"
};
#define NUM_RESOLUTIONS 10
#define NUM_REFRESH_RATES 5

// =============================================================================
// Settings State - Sound
// =============================================================================
static int master_volume = 80;    // REAL: mirrors kernel mixer (get/set_volume)
static bool sound_effects = true;
static bool sound_muted = false;
// (#382 pass2) Removed the simulated input_volume / output_device / input_device
// / mic_muted / 10-band equalizer / per-app volume state: there is no audio
// capture path, no device switching and no EQ DSP, so the Sound panel now shows
// honest "not available" states instead of these cosmetic fakes.

// =============================================================================
// Settings State - Network
// =============================================================================
static bool dhcp_enabled = true;
static bool ethernet_connected = true;
static char ip_address[16] = "192.0.2.50";
static char subnet_mask[16] = "255.255.255.0";
static char gateway[16] = "192.0.2.1";
// #786: dns_primary holds what the RESOLVER WILL ACTUALLY QUERY
// (dns_get_server(), via net_info_t.dns which the kernel now fills from the
// live resolver instead of guessing). Never the typed text: see net_pull_live().
static char dns_primary[16] = "8.8.8.8";
// #786: this was `dns_secondary = "8.8.4.4"`, drawn on the panel as "DNS 2".
// It was a COMPILE-TIME CONSTANT. There is no secondary resolver anywhere in
// this OS - kernel/net/dns.c has ONE `dns_server` and no fallback - so the row
// named a server that could not be queried, was not editable, was never saved
// and was never read. It is replaced by a fact that exists: what the DHCP
// lease OFFERED (net_status_t.dns_dhcp), which is exactly the number that
// makes "why is my DNS not what the panel says" answerable at a glance.
static char dns_offered[16] = "";
static char mac_address[18] = "00:00:5E:00:53:00";

// (#382 pass2) Removed vpn_enabled / vpn_protocol: there is no VPN client stack,
// so the Network panel marks VPN honestly as unavailable instead of a fake toggle.

// Proxy settings
static bool proxy_enabled = false;
static int proxy_type = 0;  // 0=HTTP, 1=SOCKS4, 2=SOCKS5
static char proxy_host[64] = "";
static int proxy_port = 8080;

// Firewall (#238). THESE ARE A MIRROR OF KERNEL STATE, NOT THE STATE.
//
// #223 measured that this panel wrote FWRULES.CFG perfectly and that nothing
// in the OS read it, because there was no packet filter. #238 built one, and
// with it a decision that removes the whole class of fault rather than the
// instance: THE KERNEL OWNS THE POLICY AND THE FILE. There is exactly one
// parser and one serialiser for /CONFIG/FWRULES.CFG, both in
// kernel/rustkern/fwfilter.rs, and this app no longer opens that file at all.
//
// So everything below is a cache of what SYS_NET_FW last reported, refreshed
// by fw_pull() and pushed by fw_push(). fw_push() re-reads the kernel's live
// answer into the same variables afterwards, so what this panel draws is
// always what is ACTUALLY in force - never what it asked for. An invalid
// ruleset is refused whole by the kernel, and the mirror then shows the
// policy that survived, not the one that was rejected.
static bool firewall_enabled = true;
static int  fw_pol_in  = 1;     // FW_ACT_ALLOW / FW_ACT_DENY
static int  fw_pol_out = 0;
#define MAX_FW_RULES FW_MAX_RULES
// fw_rule_t itself now comes from libc/syscall.h - the SAME layout the kernel
// and the Rust filter use, sizeof-locked in all three. The app no longer has
// a private rule struct that could disagree with the one being enforced.
static fw_rule_t fw_rules[MAX_FW_RULES];
static int fw_rule_count = 0;
static fw_stats_t fw_stats;         // live counters, for the panel and `ctl`
static int  fw_last_rc = 0;         // last SYS_NET_FW return code
static int  fw_reload_bad = 0;      // malformed lines reported by a reload

// #743: RECORDING A FAILED SAVE.
//
// Every save below used to be written as unlink-then-open-then-discard-both-
// results. Three separate defects lived in that one idiom:
//
//   * the unlink ALWAYS ran and the open might not, so a refused or failed open
//     DESTROYED the user's existing configuration and returned silently;
//   * sys_write()'s result was discarded; and
//   * sys_close()'s result was discarded, which on this kernel is the worst of
//     the three. For an ext2-backed fd the bytes are buffered and the REAL
//     write happens inside close()/fsync() (kernel/proc/syscall.c: the e2fd
//     family calls ext2_write_file() from sys_close and returns its rc). So the
//     discarded close() result was not a minor omission: it was the ONLY error
//     report that ever existed, thrown away.
//
// They now all go through userconf_write_all() / userconf_finish_write(), which
// never unlinks and returns 0 only if the bytes actually reached the medium.
//
// WHERE A FAILURE IS REPORTED. Most Settings panels have no on-screen error
// surface, and inventing a modal for each one is a bigger change than this
// ticket should make. So a failed save is recorded in the breadcrumb log the
// app already maintains (/SETLOG.TXT), which makes it diagnosable instead of
// invisible. The panels that DO have a status line (External Services, AI
// Services) additionally stop claiming "Saved" when the save failed.
static void setlog(const char *msg);
static void save_failed(const char *what) {
    char line[96];
    int k = 0;
    const char *pfx = "SAVE FAILED: ";
    for (int i = 0; pfx[i] && k < (int)sizeof(line) - 1; i++) line[k++] = pfx[i];
    for (int i = 0; what[i] && k < (int)sizeof(line) - 1; i++) line[k++] = what[i];
    line[k] = 0;
    setlog(line);
}

static void fw_add(int dir, int action, int proto, int port) {
    if (fw_rule_count >= MAX_FW_RULES) return;
    fw_rules[fw_rule_count].dir = (unsigned char)dir;
    fw_rules[fw_rule_count].action = (unsigned char)action;
    fw_rules[fw_rule_count].proto = (unsigned char)proto;
    fw_rules[fw_rule_count].reserved = 0;
    fw_rules[fw_rule_count].port = (unsigned short)port;
    fw_rules[fw_rule_count].reserved2 = 0;
    fw_rule_count++;
}

// #238: THE ONLY WAY THIS APP TOUCHES THE FIREWALL.
//
// No file is opened here. userconf_open_write("FWRULES.CFG") is gone, and so
// is the hand-rolled text parser that used to read it back: a second parser
// of a format the kernel also parses is the drift this ticket exists to
// remove, one level down from the original fault.
//
// fw_pull() is also how the panel learns the DEFAULTS on a virgin machine.
// There is no "seed sensible defaults on first run" block any more, because
// the kernel already has the compiled-in fail-safe policy in force from the
// first packet of boot (fwfilter.rs DEFAULT_CFG) and reports it here. Two
// copies of "the default policy" was itself a description that could
// disagree; now there is one, and it is the one being enforced.
static void fw_mirror(const fw_xfer_t *x) {
    firewall_enabled = x->cfg.enabled ? true : false;
    fw_pol_in  = x->cfg.pol_in;
    fw_pol_out = x->cfg.pol_out;
    fw_rule_count = x->cfg.rule_count;
    if (fw_rule_count > MAX_FW_RULES) fw_rule_count = MAX_FW_RULES;
    for (int i = 0; i < fw_rule_count; i++) fw_rules[i] = x->cfg.rules[i];
    fw_stats = x->stats;
}

// Read the live policy and counters. Never fails destructively: on refusal the
// mirror is left as it was and fw_last_rc records why.
static void fw_pull(void) {
    fw_xfer_t x;
    memset(&x, 0, sizeof(x));
    fw_last_rc = sys_net_fw(FW_OP_GET, &x);
    if (fw_last_rc == 0) fw_mirror(&x);
    else setlog("SET: SYS_NET_FW GET refused");
}

// Push the mirror to the kernel, which validates it, installs it whole or not
// at all, and persists it. Then re-read what actually took effect.
//
// WHY THE READ-BACK MATTERS: -22 means the kernel refused the ruleset and the
// PREVIOUS policy is still in force, so echoing the requested state into the
// UI would draw a firewall that is not running. -5 means the policy IS in
// force but could not be written to disk, which is a different problem with a
// different fix, and the two must not share one error path.
static void fw_push(void) {
    fw_xfer_t x;
    memset(&x, 0, sizeof(x));
    x.cfg.version    = FW_CFG_VERSION;
    x.cfg.enabled    = firewall_enabled ? 1 : 0;
    x.cfg.pol_in     = (unsigned char)fw_pol_in;
    x.cfg.pol_out    = (unsigned char)fw_pol_out;
    x.cfg.rule_count = (unsigned char)fw_rule_count;
    for (int i = 0; i < fw_rule_count && i < MAX_FW_RULES; i++)
        x.cfg.rules[i] = fw_rules[i];

    fw_last_rc = sys_net_fw(FW_OP_SET, &x);
    if (fw_last_rc == 0 || fw_last_rc == -5) fw_mirror(&x);
    if (fw_last_rc == -22)     save_failed("firewall (ruleset REFUSED; previous policy still in force)");
    else if (fw_last_rc == -5) save_failed("firewall (policy IS active but was not written to disk)");
    else if (fw_last_rc != 0)  save_failed("firewall (SYS_NET_FW refused)");
    if (fw_last_rc != 0) fw_pull();   // show what is really running
}

// Ask the kernel to re-read /CONFIG/FWRULES.CFG. Exposed through the contract
// API so a hand-edited or deliberately malformed file can be exercised without
// a reboot; the kernel reports how many lines it had to ignore.
static void fw_reload_from_disk(void) {
    fw_xfer_t x;
    memset(&x, 0, sizeof(x));
    fw_last_rc = sys_net_fw(FW_OP_RELOAD, &x);
    if (fw_last_rc == 0) { fw_mirror(&x); fw_reload_bad = 0; }
    else setlog("SET: SYS_NET_FW RELOAD found nothing usable");
}

// =============================================================================
// Settings State - Keyboard
// =============================================================================
// #230: Keyboard Layout, Repeat Rate, Repeat Delay, and the Num/Caps/Scroll
// Lock checkboxes were deleted (docs/SETTINGS_CONTROL_AUDIT.md #224). None had
// a real consumer: there is no keymap table to select in the kernel (one
// hardcoded US scancode table, cpu/isr.c), no PS/2 typematic (0xF3) command is
// ever sent, no software auto-repeat exists for ordinary USB keys (only
// consumer/media keys get one, drivers/usb_hid.c HID_CONS_REPEAT_*), and the
// lock-key vars were not even persisted. The Layout dropdown additionally
// could not be opened at all (drawn with a literal `false`). See blame.md.

// Keyboard shortcuts
typedef struct {
    char action[32];
    char keys[32];
} shortcut_t;

static shortcut_t shortcuts[8] = {
    {"Copy", "Ctrl+C"},
    {"Paste", "Ctrl+V"},
    {"Cut", "Ctrl+X"},
    {"Undo", "Ctrl+Z"},
    {"Save", "Ctrl+S"},
    {"Find", "Ctrl+F"},
    {"Close Window", "Alt+F4"},
    {"Switch App", "Alt+Tab"}
};

// =============================================================================
// Settings State - Mouse/Touchpad
// =============================================================================
static int pointer_speed = 50;      // 0-100 (REAL: mouse_scale_delta, kernel/drivers/mouse.c)
static int double_click_speed = 50; // 0-100 (REAL as of #230: settingscfg_dblclick_ms())
// #230: Pointer Size, Scroll Speed, Natural Scrolling, Pointer Trails + Trail
// Length, Scroll Inertia and Left-handed Mode were deleted
// (docs/SETTINGS_CONTROL_AUDIT.md #224). None had ANY consumer anywhere in the
// tree: no cursor-trail renderer, no kinetic-scroll implementation, nothing
// inverts scroll, and mouse button bits pass straight through with no swap.
// Pointer Size additionally had no click handler at all. See blame.md.

// =============================================================================
// Modal Dialog State
// =============================================================================
#define MODAL_NONE            0
#define MODAL_CHANGE_PASSWORD 1
#define MODAL_ADD_USER        2
#define MODAL_EDIT_PROFILE    3
#define MODAL_SET_DATETIME    4
#define MODAL_CREDITS         5
#define MODAL_SET_NETWORK     6
// #238: MODAL_ADD_FWRULE (7) DELETED. #223 removed its entry point and kept
// the handling "now unreachable"; this ticket removes the handling too. The
// number is left unused rather than reassigned so an old saved state cannot
// land on a different modal.
#define MODAL_ADD_PRINTER     8   // (#318) name/host/queue/port/default
#define MODAL_WIFI_PASSWORD   9   // (#384) single password field to join a secured SSID
#define MODAL_AUTOLOGIN_PW    10  // (#566) confirm password before enabling/disabling autologin
// (#786) DNS-only, one field. WHY IT HAS TO EXIST SEPARATELY FROM
// MODAL_SET_NETWORK: "Configure IP..." is drawn ONLY when DHCP is off, so on a
// DHCP machine - which is most of them - the DNS field was UNREACHABLE. The
// only way to reach it was to switch DHCP off, and doing that pins the leased
// address as a STATIC one. That is how a golden ended up hard-wired to
// 192.0.2.1 and unable to reach any other LAN. Changing your resolver must
// not cost you your DHCP lease, so this modal writes the resolver and nothing
// else, and net_persist_netcfg() correspondingly writes a dns-only
// /CONFIG/NETIP.CFG that leaves DHCP running at the next boot.
#define MODAL_SET_DNS         11
#define MODAL_MAX_FIELDS      5
static int modal_mode = MODAL_NONE;
static int modal_num_fields = 3;
static char modal_field[MODAL_MAX_FIELDS][64];
static int  modal_cursor[MODAL_MAX_FIELDS];  // field LENGTH (kept for existing logic)
static int  modal_caret[MODAL_MAX_FIELDS];   // caret index into modal_field (task #244)
// (#745) MODAL_EDIT_PROFILE's picture picker: an index into avatar_palette
// (0..7), the chosen monogram color override. Written as "mono:RRGGBB" on
// Save (see do_modal_submit()). The default picture (design doc section 10)
// is the uid-keyed palette entry with no CFG line at all, so opening the
// modal seeds this from the account's CURRENT color, not from a fixed slot -
// picking the swatch that already matches just re-writes the same value.
static int modal_avatar_idx = 0;
static int  modal_active_field = 0;
static char modal_error[64];
// Extra status variables
static int ntp_status = 0;      // 0=idle, 1=synced ok, -1=failed
static int about_status = 0;    // 0=idle, 1=up-to-date, 2=debug exported,
                                 // 3=update available, 4=check failed, 5=checking (#230)
static int sound_test_status = 0; // 0=idle, 1=no output, 2=no input

// =============================================================================
// Settings State - Date & Time
// =============================================================================
// #50: the picker's CURRENT ROW in the shared list (libc/tz.h). It is NOT the
// stored setting and is never persisted here: /CONFIG/TZ.CFG holds the zone ID
// string and tz_index() is the reader. Seeded from tz_index() at startup.
static int timezone_idx = 0;
static bool use_24hour = true;      // REAL as of #230: settingscfg_use24h()
static bool auto_time = true;
// #230: Date Format was deleted (docs/SETTINGS_CONTROL_AUDIT.md #224). Nothing
// in the tree ever formatted a date with it - not even Settings' own date
// display, which was hardcoded "DD Mon YYYY" regardless of the dropdown's
// selection. See blame.md.
static int first_day_of_week = 0;  // 0=Sunday, 1=Monday

// #50: Settings' own 26-entry timezone array USED TO LIVE HERE. It has been
// deleted, not merely stopped being read. It had diverged from the first-run
// wizard's list (no +09:30 Adelaide, no +12:45 Chatham, no +05:45 Kathmandu, no
// +13:00/+14:00, no -03:30/-09:30), and it indexed a private 't' key in
// SETTINGS.CFG that nothing else in the OS could see. One list now: ZONES[] in
// userland/libc/tz.c. Do not add a local copy back, not even "just the labels".

// ONE-TIME MIGRATION ONLY. These are the offsets of that deleted 26-entry
// array, in its order, so a user who had already picked a zone in Settings
// keeps it when TZ.CFG does not yet exist. This is a migration table, not a
// second timezone list: nothing reads it after the first run, and it must never
// gain an entry. See settings_tz_init().
static const int LEGACY_SETTINGS_TZ_OFF[26] = {
    -720, -660, -600, -540, -480, -420, -360, -300, -240, -180, -120, -60,
    0, 60, 120, 180, 240, 300, 330, 360, 420, 480, 540, 600, 660, 720
};
static int legacy_tz_idx = -1;   // set by settings_load() if the old 't' key exists

// =============================================================================
// Settings State - Users & Accounts
// =============================================================================
static int current_user_idx = 0;
// #785: the account THIS SESSION is logged in as, resolved from sys_getuid()
// against the kernel account table in users_refresh(). Kept separate from
// current_user_idx on purpose: current_user_idx is an index into the DISPLAY
// list, which is capped at 4 rows, and whose password the Change Password
// modal targets must never depend on how many rows the panel happens to draw.
// Empty means "not resolved", and the modal refuses rather than guessing.
static char g_session_user[32] = "";
static bool guest_enabled = true;

// (#566) Real autologin state, mirrored from the kernel (sys_get_autologin())
// rather than a per-launch cosmetic bool. "" = disabled; else the username
// currently configured to autologin. al_target_user/al_target_enable hold the
// pending request while MODAL_AUTOLOGIN_PW collects the confirming password.
static char autologin_user[32] = {0};
static char al_target_user[32] = {0};
static int  al_target_enable = 0;

static void autologin_refresh(void) {
    char buf[32];
    int n = sys_get_autologin(buf, sizeof(buf));
    if (n > 0) {
        if (n > 31) n = 31;
        int i = 0;
        for (; i < n; i++) autologin_user[i] = buf[i];
        autologin_user[i] = '\0';
    } else {
        autologin_user[0] = '\0';
    }
}

// Root may set autologin for ANY account with no password (the kernel ABI:
// "Root sets for anyone"); a non-root caller may only target their own
// account and must supply its current password (kernel-checked). See
// SYS_SET_AUTOLOGIN in userland/libc/syscall.h.
static int settings_is_root(void) { return sys_getuid() == 0; }
// Defined later (after draw_all()'s own forward declaration) - opens the
// MODAL_AUTOLOGIN_PW confirm-password modal for a non-root session, or
// applies directly for root.
static void autologin_request(const char *user, int enable);

typedef struct {
    char username[32];
    char fullname[64];
    char email[64];
    int role;           // 0=Admin, 1=User, 2=Guest
    bool password_set;
    uint32_t avatar_color;
    unsigned int uid;   // #745: identity-color key (design doc section 8.2) -
                         // stable across list reordering, unlike list position
} user_account_t;

static user_account_t users[4] = {
    {"admin", "Administrator", "admin@mayteraos.local", 0, true, 0x00569CD6, 0},
    {"user", "Standard User", "user@mayteraos.local", 1, true, 0x0066BB66, 0},
    {"guest", "Guest Account", "", 2, false, 0x00888888, 0},
    {"", "", "", 0, false, 0, 0}
};
static int user_count = 3;

// =============================================================================
// Settings State - Privacy & Security
// =============================================================================
static int  screen_lock_enabled = 1;
static int lock_timeout = 5;        // Minutes (0=Never, 1, 2, 5, 10, 15, 30)
static int  require_password_wake = 1;
static int  location_services = 0;
static int  diagnostics_enabled = 1;
static int  crash_reports = 1;

// (#382 pass2) Removed the simulated per-app permission matrix (app_permission_t
// / app_permissions[]): there is no capability-enforcement backend, so the
// Privacy panel states that honestly rather than showing fabricated allow/deny.

// =============================================================================
// Settings State - Storage
// =============================================================================
typedef struct {
    char name[32];
    char mount_point[32];
    uint64_t total_bytes;
    uint64_t used_bytes;
    char filesystem[16];
    char model[41];
    char serial[21];
    int  smart;        // 1=ok, 0=failing, -1=unknown
} storage_drive_t;

static storage_drive_t drives[4] = {
    {"System", "/", 0, 0, "FAT32"},  // filled at startup from kernel
    {"", "", 0, 0, ""},
    {"", "", 0, 0, ""},
    {"", "", 0, 0, ""}
};
static int drive_count = 1;

// Cache / trash sizes. Real: summed from actual directories at startup and on
// entering the Storage panel (0 if the directory is absent or empty). The
// mapped paths below are the real on-disk locations; "Clear" unlinks the files.
static uint64_t cache_thumbnails = 0;   // real: sum of /THUMBS
static uint64_t cache_apps = 0;         // real: sum of /TMP
static uint64_t cache_system = 0;       // real: sum of /CACHE
static uint64_t trash_size = 0;         // real: sum of /TRASH
#define CACHE_DIR_THUMBS "/THUMBS"
#define CACHE_DIR_APPS   "/TMP"
#define CACHE_DIR_SYSTEM "/CACHE"
#define CACHE_DIR_TRASH  "/TRASH"

// Sum the byte sizes of the regular files directly inside `path` (non-recursive).
// Returns 0 if the directory does not exist or is empty. Uses the shared readdir
// syscall wrapper; no fabricated numbers.
static uint64_t dir_size_bytes(const char *path) {
    uint64_t total = 0;
    dirent_t e;
    for (int i = 0; i < 4096; i++) {
        if (sys_readdir(path, i, &e) != 0) break;
        if (e.name[0] == '.' && (e.name[1] == 0 || (e.name[1] == '.' && e.name[2] == 0)))
            continue;
        if (!DIRENT_IS_DIR(e)) total += (uint64_t)e.size;
    }
    return total;
}

// Unlink every regular file directly inside `path`. Best-effort: leaves
// subdirectories alone. Returns the number of files removed.
static int dir_clear_files(const char *path) {
    int removed = 0;
    dirent_t e;
    // Re-scan from index 0 each time because unlinking shifts the listing.
    for (int guard = 0; guard < 4096; guard++) {
        int found = -1;
        for (int i = 0; i < 4096; i++) {
            if (sys_readdir(path, i, &e) != 0) break;
            if (e.name[0] == '.' && (e.name[1] == 0 || (e.name[1] == '.' && e.name[2] == 0)))
                continue;
            if (!DIRENT_IS_DIR(e)) { found = i; break; }
        }
        if (found < 0) break;
        char full[288]; int k = 0;
        for (int j = 0; path[j] && k < 255; j++) full[k++] = path[j];
        if (k && full[k-1] != '/') full[k++] = '/';
        for (int j = 0; e.name[j] && k < 287; j++) full[k++] = e.name[j];
        full[k] = 0;
        if (sys_unlink(full) == 0) removed++;
        else break;   // avoid an infinite loop if a file cannot be removed
    }
    return removed;
}

// Refresh the four cache/trash totals from their real directories.
static void storage_scan(void) {
    cache_thumbnails = dir_size_bytes(CACHE_DIR_THUMBS);
    cache_apps       = dir_size_bytes(CACHE_DIR_APPS);
    cache_system     = dir_size_bytes(CACHE_DIR_SYSTEM);
    trash_size       = dir_size_bytes(CACHE_DIR_TRASH);
}

// =============================================================================
// Forward Declarations
// =============================================================================
static void draw_all(void);
static void apply_theme(int theme_id);

// -------------------------------------------------------------------------
// #3 (iMac Settings-launch debug): breadcrumb log. On real iMac hardware
// Settings applies the theme + cursor on launch and then the window sometimes
// NEVER appears (it opens fine on a VM; Files/Calc/Terminal open fine on the
// iMac). Serial was not readable, so record each startup milestone to a
// persistent file. The whole in-memory buffer is rewritten (truncate) on every
// step so that even if the app hangs or faults mid-startup, /SETLOG.TXT on disk
// holds every step reached up to the failure, revealing the last step Settings
// got to. Self-contained; no shared app logger exists in this tree yet.
// -------------------------------------------------------------------------
static char g_setlog_buf[1600];
static int  g_setlog_len = 0;
static void setlog(const char *msg) {
    for (int i = 0; msg[i] && g_setlog_len < (int)sizeof(g_setlog_buf) - 2; i++)
        g_setlog_buf[g_setlog_len++] = msg[i];
    g_setlog_buf[g_setlog_len++] = '\n';
    // #743: a diagnostic breadcrumb. The result is consumed rather than
    // discarded (userconf_write_all is warn_unused_result), but deliberately
    // NOT acted on: there is no fallback for a breadcrumb, nothing the user
    // needs told about one, and reporting it through save_failed() would
    // recurse straight back into here.
    int rc = userconf_write_all("/SETLOG.TXT", g_setlog_buf,
                                (unsigned long)g_setlog_len);
    (void)rc;
}
// Log "<msg> <n>" as one breadcrumb line (for sizes, framebuffer WxH, handles).
static void setlog_n(const char *msg, long n) {
    char line[80]; int k = 0;
    for (int i = 0; msg[i] && k < 48; i++) line[k++] = msg[i];
    line[k++] = ' ';
    if (n < 0) { line[k++] = '-'; n = -n; }
    char t[20]; int ti = 0;
    if (n == 0) t[ti++] = '0'; else { while (n && ti < 19) { t[ti++] = (char)('0' + (int)(n % 10)); n /= 10; } }
    while (ti) line[k++] = t[--ti];
    line[k] = 0;
    setlog(line);
}

static void apply_display_fx(void) {
    set_display_fx(brightness, night_light ? night_light_strength : 0);
}
static void draw_panel_content(void);
static void draw_extsvc_panel(void);   // #414

// =============================================================================
// Theme Application
// =============================================================================

// (#565) apply_theme() used to be a hardcoded 8-case switch of literal
// colors for Settings' own chrome, PLUS a second hardcoded index-remap table
// (kernel_theme_map[]) to push one of the kernel's 12 compiled-in palettes
// system-wide. Both are gone: theme_id now indexes the FILE-based list
// (g_th[], from /THEMES/INDEX.TXT, see th_init()). gui_theme_activate()
// loads the theme's /THEMES/<slug>.mtheme into the kernel's live table and
// applies it system-wide (every process's theme_color()/theme_color_of()
// call reflects it on its next redraw - no polling, no IPC); Settings then
// derives its OWN chrome colors from the SAME live palette via theme_color(),
// so a brand new App-Store-installed theme recolors Settings exactly as
// well as it recolors the compositor and every other app, with no per-theme
// literal table to keep in sync.
static void apply_theme(int theme_id) {
    if (theme_id < 0 || theme_id >= g_th_count) theme_id = 0;

    int kernel_id = gui_theme_activate(g_th[theme_id].slug);
    if (kernel_id < 0) {
        setlog("apply_theme: could not load theme file, keeping previous colors");
        return;
    }

    COL_CONTENT_BG      = theme_color(THEME_COLOR_WINDOW_BG);
    COL_SIDEBAR_BG      = theme_color(THEME_COLOR_TASKBAR_BG);
    COL_PANEL_NORMAL    = theme_color(THEME_COLOR_TASKBAR_BG);
    COL_PANEL_HOVER     = theme_color(THEME_COLOR_TASKBAR_HOVER);
    COL_PANEL_ACTIVE    = theme_color(THEME_COLOR_TASKBAR_ACTIVE);
    COL_SIDEBAR_TEXT     = theme_color(THEME_COLOR_TASKBAR_TEXT);          // #745
    COL_SIDEBAR_MUTED    = theme_color(THEME_COLOR_TASKBAR_TEXT_MUTED);    // #745
    COL_SIDEBAR_SEL_TEXT = theme_color(THEME_COLOR_TASKBAR_SELECTED_TEXT); // #745
    COL_SEPARATOR       = theme_color(THEME_COLOR_MENU_SEPARATOR);
    COL_TEXT_PRIMARY    = theme_color(THEME_COLOR_LABEL_TEXT);
    COL_TEXT_SECONDARY  = theme_color(THEME_COLOR_MENU_TEXT_DISABLED);
    COL_TEXT_DISABLED   = theme_color(THEME_COLOR_BUTTON_DISABLED);
    COL_INPUT_BG        = theme_color(THEME_COLOR_TEXTBOX_BG);
    COL_INPUT_BORDER    = theme_color(THEME_COLOR_TEXTBOX_BORDER);
    COL_SLIDER_TRACK    = theme_color(THEME_COLOR_SCROLLBAR_BG);
    COL_BUTTON_BG       = theme_color(THEME_COLOR_BUTTON_FACE);
    COL_BUTTON_HOVER    = theme_color(THEME_COLOR_BUTTON_LIGHT);
    COL_CHECKBOX_BG     = theme_color(THEME_COLOR_CHECKBOX_BG);
    // A card needs to read as "slightly raised" against the content
    // background in EITHER a light or a dark theme; blending 10% toward the
    // ink color does that without a dedicated theme_color_id_t of its own.
    COL_CARD_BG         = gui_mix(COL_CONTENT_BG, COL_TEXT_PRIMARY, 10);

    // Apply accent color
    COL_ACCENT = ACCENT_COLORS[accent_color_idx];
    COL_ACCENT_HOVER = COL_ACCENT + 0x00202020;  // Lighten
    COL_SLIDER_FILL = COL_ACCENT;
    // (#704) These three were hardcoded literals even though the kernel
    // theme_t has carried color_success/warning/error since before #711 (a
    // .mtheme file could set them, but nothing read them back). Now real
    // tokens (THEME_COLOR_SUCCESS/WARNING/ERROR, added this ticket).
    COL_SUCCESS = theme_color(THEME_COLOR_SUCCESS);
    COL_WARNING = theme_color(THEME_COLOR_WARNING);
    COL_ERROR = theme_color(THEME_COLOR_ERROR);

    // Push the active palette + renderer family into the shared style engine
    // so all gui_* primitives render in this theme. A "retro"-style theme
    // file (style=retro, e.g. Classic/Retro UNIX) uses the beveled CDE
    // renderer; everything else uses the modern renderer.
    gui_set_style(g_th[theme_id].is_classic ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t pal;
    pal.surface        = COL_CONTENT_BG;
    pal.surface_raised = COL_CARD_BG;
    pal.ink            = COL_TEXT_PRIMARY;
    pal.ink_dim        = COL_TEXT_SECONDARY;
    pal.accent         = COL_ACCENT;
    pal.accent_hover   = COL_ACCENT_HOVER;
    pal.border         = COL_INPUT_BORDER;
    pal.field_bg       = COL_INPUT_BG;
    pal.field_border   = COL_INPUT_BORDER;
    pal.track          = COL_SLIDER_TRACK;
    gui_set_palette(&pal);
    (void)kernel_id;
}


// Theme picker uses a scrollable dropdown (scales to many themes), not
// buttons. (#565) Names and count now come from th_init()'s file-based
// g_th_names[]/g_th_count (see the declarations near g_wp[]/wp_init()
// above), not a hardcoded 8-entry table, so an App-Store-installed theme
// shows up here with no recompile - same as the wallpaper picker.
static void theme_dd_changed(void) { apply_theme(current_theme); }

// =============================================================================
// Utility Functions
// =============================================================================

static int my_strlen(const char *s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

static void my_strcpy(char *dst, const char *src) {
    while ((*dst++ = *src++));
}

static void format_size(uint64_t bytes, char *buf, int buf_size) {
    (void)buf_size;
    if (bytes >= 1099511627776ULL) {
        int tb = (int)(bytes / 1099511627776ULL);
        int frac = (int)((bytes % 1099511627776ULL) * 10 / 1099511627776ULL);
        gui_itoa(tb, buf, 16);
        int len = my_strlen(buf);
        buf[len++] = '.';
        buf[len++] = '0' + frac;
        buf[len++] = ' '; buf[len++] = 'T'; buf[len++] = 'B'; buf[len] = 0;
    } else if (bytes >= 1073741824ULL) {
        int gb = (int)(bytes / 1073741824ULL);
        int frac = (int)((bytes % 1073741824ULL) * 10 / 1073741824ULL);
        gui_itoa(gb, buf, 16);
        int len = my_strlen(buf);
        buf[len++] = '.';
        buf[len++] = '0' + frac;
        buf[len++] = ' '; buf[len++] = 'G'; buf[len++] = 'B'; buf[len] = 0;
    } else if (bytes >= 1048576ULL) {
        int mb = (int)(bytes / 1048576ULL);
        gui_itoa(mb, buf, 16);
        int len = my_strlen(buf);
        buf[len++] = ' '; buf[len++] = 'M'; buf[len++] = 'B'; buf[len] = 0;
    } else {
        int kb = (int)(bytes / 1024ULL);
        gui_itoa(kb, buf, 16);
        int len = my_strlen(buf);
        buf[len++] = ' '; buf[len++] = 'K'; buf[len++] = 'B'; buf[len] = 0;
    }
}

// =============================================================================
// Drawing Utility Functions
// =============================================================================

static void draw_section_header(int x, int y, const char *title) {
    // #711 loop 2 (B5/D4, typography family): section headers are primary
    // text at BOLD weight, never accent - accent is reserved for
    // interactive/selected elements only (director redirect, Section B5).
    // Size comes from type.title (theme_metric_or, no rebuild needed to
    // change it); bold is an explicit style bit at this call site, since a
    // Settings category header is a distinct usage from a window titlebar
    // (which stays regular per docs/UI_STYLE_GUIDE.md 4.4) even though both
    // read type.title's SIZE. Real DejaVu Sans Bold face is enrolled (not
    // faux-bold), see 4.3/4.7. type.title_lineheight (16*1.4=22.4 -> 22)
    // matches the existing +22 separator offset below exactly, so no layout
    // constant needed to change.
    int sz = theme_metric_or(THEME_METRIC_TYPE_TITLE, 16);
    win_draw_text_ttf_ex(window_handle, x, y, title, 0, sz, FONT_STYLE_BOLD, COL_TEXT_PRIMARY);
    win_draw_rect(window_handle, x, y + 22, CONTENT_WIDTH - 2 * PADDING, 1, COL_SEPARATOR);
}

static void draw_subsection(int x, int y, const char *title) {
    // #711 loop 2 (B5/D4, typography family): was COL_ACCENT. This is the
    // exact defect B5 names: green subsection labels reading as "selected"
    // when they are structural headings. Now type.body_strong (identical
    // 14px box as type.body, bold weight, primary text colour) per
    // docs/UI_STYLE_GUIDE.md 4.4's usage table, not an accent-coloured
    // heading. Accent stays reserved for interactive/selected elements.
    int sz = theme_metric_or(THEME_METRIC_TYPE_BODY, 14);
    win_draw_text_ttf_ex(window_handle, x, y, title, 0, sz, FONT_STYLE_BOLD, COL_TEXT_PRIMARY);
}

static void draw_label(int x, int y, const char *label) {
    win_draw_text(window_handle, x, y, label, COL_TEXT_SECONDARY);
}

static void draw_value(int x, int y, const char *value) {
    win_draw_text(window_handle, x, y, value, COL_TEXT_PRIMARY);
}

static void draw_label_value(int x, int y, const char *label, const char *value, int label_width) {
    win_draw_text(window_handle, x, y, label, COL_TEXT_SECONDARY);
    win_draw_text(window_handle, x + label_width, y, value, COL_TEXT_PRIMARY);
}

static int draw_mico(const char *name, int x, int y, int size, uint32_t tint, uint32_t bg);
// Inline status message: a Zest status glyph + small TTF text.
//   INFO=info  CCHECK=success  CIRCX=error  WARN=alert  CMINUS=inaccessible
static void draw_hint_ic(int x, int y, const char *icon, uint32_t tint, const char *hint) {
    // Nudged down + lighter text so hints don't crowd the control above.
    draw_mico(icon, x, y + 4, 13, tint, COL_CARD_BG);
    win_draw_text_small(window_handle, x + 18, y + 5, hint, COL_TEXT_SECONDARY);
}
static void draw_hint(int x, int y, const char *hint) {
    draw_hint_ic(x, y, "INFO", COL_TEXT_SECONDARY, hint);
}

static void draw_card(int x, int y, int w, int h) {
    gui_card(window_handle, x, y, w, h);
}

// #237: the Wi-Fi/Bluetooth panels no longer simulate anything, so the #230
// "SIMULATION" banner that used to sit here (docs/SETTINGS_CONTROL_AUDIT.md
// #224, ranked #4 - "the worst shape in the audit: a lie with hardware
// detection backing it") is gone WITH the fiction it warned about. Both
// panels used to be userland mocks (WIFI_MOCK_IMPL/BT_MOCK_IMPL) behind a
// REAL radio-presence probe (g_wifi_present/g_bt_present): on hardware that
// genuinely has a wireless/BT card, the gate opened and a fabricated
// successful scan/pair/join rendered with nothing to distinguish it from a
// real one. A banner that keeps warning about fakery that has been deleted
// would itself be a small lie, so it goes too - see blame.md #237.
//
// What replaced it: draw_wifi_panel()/draw_bluetooth_panel() now distinguish
// exactly two real conditions - "no adapter" (unchanged from #382) and
// "adapter present, no driver" (new, #237) - and say only that, precisely.
// Real association (a driver + WPA-supplicant equivalent) remains a
// separate, large piece of work (#383), out of scope here per the owner's
// own instruction.

// Inert (permanently disabled, non-clickable) toggle: used where a REAL
// hardware adapter was detected but no driver exists to back the switch.
// Unlike draw_toggle(), this does not call focus_add() (there is nothing to
// focus) and the caller must NOT register a click hit for it - the control
// is disabled by construction, not merely painted grey.
static void draw_toggle_inert(int x, int y, bool on) {
    gui_toggle(window_handle, x, y, 48, 24, on, GUI_ST_DISABLED);
}

// Inert (permanently disabled, non-clickable) button - same rule as above:
// no hit is ever registered for it by the caller.
static void draw_button_inert(int x, int y, int width, const char *label) {
    gui_button(window_handle, x, y, width, 30, label, GUI_BTN_SECONDARY, GUI_ST_DISABLED);
}

// --- Keyboard focus ring: lets Tab/arrows cycle controls and Enter activate
// them, so the GUI is fully usable (and testable) without a mouse. Controls
// register their rect during draw; Enter dispatches the existing click handler. ---
#define FOCUS_MAX 96
typedef struct { int x, y, w, h, sidebar; } focus_rect_t;
static focus_rect_t g_focus[FOCUS_MAX];
static int g_focus_n = 0;
static int g_focus_idx = 0;
static int g_focus_on = 0;
static void focus_reset(void) { g_focus_n = 0; }
static void focus_add(int x, int y, int w, int h, int sidebar) {
    if (g_focus_n < FOCUS_MAX) {
        g_focus[g_focus_n].x = x; g_focus[g_focus_n].y = y;
        g_focus[g_focus_n].w = w; g_focus[g_focus_n].h = h;
        g_focus[g_focus_n].sidebar = sidebar; g_focus_n++;
    }
}

static void draw_slider(int x, int y, int width, int value, int max_val, uint32_t fill_color) {
    (void)fill_color;  // engine uses the theme accent
    focus_add(x, y, width, 16, 0);
    gui_slider(window_handle, x, y, width, value, max_val, GUI_ST_NORMAL);
}

static void draw_slider_labeled(int x, int y, int width, const char *label, int value, int max_val, const char *value_str) {
    draw_label(x, y, label);
    draw_slider(x, y + 25, width, value, max_val, COL_SLIDER_FILL);
    win_draw_text(window_handle, x + width + 15, y + 25, value_str, COL_TEXT_SECONDARY);
}

static void draw_toggle(int x, int y, bool enabled) {
    focus_add(x, y, 48, 24, 0);
    gui_toggle(window_handle, x, y, 48, 24, enabled, GUI_ST_NORMAL);
}

static void draw_toggle_labeled(int x, int y, int label_width, const char *label, bool enabled) {
    draw_label(x, y + 4, label);
    draw_toggle(x + label_width, y, enabled);
}

// File-scope option lists: shared by the draw pass (value label) and the
// dropdown click handler (dropdown_open keeps the pointer while open).
static const char *const FONT_SIZE_OPTS[] = {"Small", "Medium", "Large", "X-Large"};
static const char *const ICON_SIZE_OPTS[] = {"Small", "Medium", "Large"};
// #560/#571: the ten new GL screensavers (GL Tunnel..GL Lava, kernel ids
// 10-19) were gated out of this picker after a 2026-07-21 session measured
// all ten rendering as a full-screen rainbow gradient instead of their
// geometry (GLTUNNEL had separately crashed COMPOSIT earlier that session,
// fixed in f5ee702; GLKALEIDO/GLPLATONIC rendered black from a different
// bug, fixed in 69072be). #571 (this session) reproduced the gradient bug
// from screenshots, bisected it on a throwaway TESTHOOK build, and found it
// does NOT reproduce on current HEAD (most likely fixed incidentally by one
// of the shared TinyGL bounds-check commits since 07-21). MEASURED: all ten
// boot-tested via testhook.c SAVER <id>, two screendumps each (differing
// md5, proving live animation), all rendering correct intended geometry,
// clean dismiss back to desktop, no crash/hang, CPU sane. The matching
// refusal in screensaver_set_type() (screensaver.c) has been removed too -
// see its comment for the full history. Appended at the END (ids 10-19,
// idx 11-20) so existing saved screensaver_idx values for every earlier
// entry are unchanged (screensaver_idx is persisted directly, sv_putint
// 's' below). Category labels follow docs/SCREENSAVER_PSYCHEDELIC_DESIGN.md
// section 8.1's "Category: Effect" convention.
// #ssredesign: three entries added for the psychedelic screensaver redesign
// (docs/SCREENSAVER_PSYCHEDELIC_DESIGN.md), all direct-pixel (NOT TinyGL).
// "Plasma" is relabeled "Psychedelic: Plasma" because SS_PLASMA (kernel id
// 7) was replaced in place with "Plasma Reborn" (design doc §4.3, §11 Q2) -
// same slot/id, a strict superset of the old effect. "Bloom Garden" (§4.1,
// Fractal Flame) and "Stained Glass" (§4.6, Stained-Glass Warp) are NEW
// kernel ids (20, 21 - screensaver.c's SS_FLAME/SS_STAINEDGLASS).
// #124: "Plasma (Classic)" (kernel id 22, screensaver.c's SS_PLASMACLASSIC) is
// the original #282 plasma, restored ALONGSIDE the Reborn one that took over
// its slot in ff3ca5f. Two entries with the same word in them is the whole
// point of the ticket, so the labels have to separate them by look, not by
// number: "Psychedelic: Plasma" is the smooth, bloomed, palette-cycled one;
// "Plasma (Classic)" is the hard-edged 4px-block HSV one. APPENDED at the end
// (idx 21) rather than slotted next to its namesake, because screensaver_idx
// is persisted directly into SETTINGS.CFG and inserting mid-list renumbers
// every entry after it - which is exactly what #ssredesign did when it pushed
// the ten GL effects from idx 9-18 to 11-20.
static const char *const SS_OPTS[]        = {"Off", "Starfield", "Flux", "Lines", "Bubbles", "Matrix", "Psychedelic: Plasma", "GL Cube", "GL Matrix", "Psychedelic: Bloom Garden", "Psychedelic: Stained Glass", "Rainbow: Tunnel", "Psychedelic: Kaleidoscope", "Geometric: Platonic Solids", "Geometric: Lorenz Attractor", "Geometric: Mobius Strip", "Geometric: Wave Mesh", "Geometric: Spirograph", "Geometric: Hypercube", "Geometric: Vortex", "Psychedelic: Lava Blobs", "Plasma (Classic)"};
static const char *const CURSOR_OPTS[]    = {"Light", "Dark", "Glow"};  // (#116) maps to compositor curstyle 0/1/2
static const int SS_KERNEL_MAP[]          = {0, 2, 6, 3, 4, 5, 7, 8, 9, 20, 21, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 22};
                                          // idx -> kernel screensaver id (#319 8=GL Cube 9=GL Matrix; 20/21 are the
                                          // psychedelic-redesign direct-pixel effects, see #ssredesign above; 10-19
                                          // are the #560/#571 GL effects, un-gated and appended - see comment above)
#define SS_OPTS_COUNT ARRAY_COUNT(SS_OPTS)   /* #745: was a literal 21 */
_Static_assert(ARRAY_COUNT(SS_KERNEL_MAP) == ARRAY_COUNT(SS_OPTS),
               "#745: SS_KERNEL_MAP must have exactly one kernel id per SS_OPTS label");
// idx -> kernel cursor_theme_t (kernel/gui/cursor.h: RETRO=0, MODERN_LIGHT=1,
// MODERN_DARK=2). #745: this still had FOUR entries from the pre-#116
// "Default/Dark/Light/Large" list while CURSOR_OPTS has had THREE
// (Light/Dark/Glow) since #116, so every index mapped to the wrong theme
// (Light->RETRO, Dark->MODERN_DARK, Glow->MODERN_LIGHT) and entry 3 was
// unreachable dead data. Now one entry per CURSOR_OPTS entry.
static const int CURSOR_KERNEL_MAP[]      = {1, 2, 1};
_Static_assert(ARRAY_COUNT(CURSOR_KERNEL_MAP) == ARRAY_COUNT(CURSOR_OPTS),
               "#745: CURSOR_KERNEL_MAP must have exactly one kernel theme per CURSOR_OPTS label"
               " (it carried a stale 4th entry from the pre-#116 list)");
#define CURSOR_OPTS_COUNT    ARRAY_COUNT(CURSOR_OPTS)
#define FONT_SIZE_OPTS_COUNT ARRAY_COUNT(FONT_SIZE_OPTS)
#define ICON_SIZE_OPTS_COUNT ARRAY_COUNT(ICON_SIZE_OPTS)
// #745 (local 102): display rotation (GPD MicroPC - a physically-rotated
// panel needs a software rotation, the same fix Linux users on that hardware
// make with fbcon/xrandr). Index IS the kernel's fb_rotation_t value
// (video/framebuffer.h FB_ROTATE_NONE=0/_90=1/_180=2/_270=3) - no separate
// kernel-id map needed, unlike SS_KERNEL_MAP/CURSOR_KERNEL_MAP above, because
// this dropdown's index space was DEFINED to match the kernel enum rather
// than growing an already-shipped UI list around it.
static const char *const ROTATION_OPTS[]  = {"None", "90 deg", "180 deg", "270 deg"};
#define ROTATION_OPTS_COUNT  ARRAY_COUNT(ROTATION_OPTS)
// #387 Dock / taskbar layouts, in the compositor's DOCK_* enum order.
// #745: the names USED to be a private array right here, and a second private
// array in the first-boot wizard (userland/apps/setup/main.rs). They drifted:
// this one had been renamed off the third-party desktop names while the wizard
// still displayed "macOS style", "CDE panel" and "Amiga bar". The user reported
// it twice. There is now exactly ONE list, in libc (gui_dock.h/.c), read by
// both apps, so neither owns a list that can diverge. Do not reintroduce a
// local array here; build/dock-name-gate.sh fails the build if you do.
#define DOCK_OPTS            gui_dock_style_names()
#define DOCK_OPTS_COUNT      gui_dock_style_count()
// DOCK_OPTS is now a `const char *const *`, so ARRAY_COUNT (and therefore
// OPT_CLAMP / OPT_OK) MUST NOT be used on it: sizeof on a pointer is legal C
// and would silently clamp every dock index to 0 with no warning. This is the
// clamp for this one list, and it counts the shared list, not a pointer.
#define DOCK_CLAMP(i)        (((i) >= 0 && (i) < DOCK_OPTS_COUNT) ? (i) : 0)

// --- Real dropdown widget: opens a scrollable list, current item highlighted ---
// #560: this widget is shared by every dropdown in Settings (theme, font
// size, icon size, screensaver, cursor, dock layout, timezone, date format,
// wallpaper). Long lists (wallpaper up to WP_MAX_ENTRIES, 26 timezones, and
// now 19 screensaver types after #560 added ten GL effects) already scrolled
// correctly via wheel/keyboard/scrollbar-drag, but a list longer than
// DD_VISIBLE gave NO visible signal that more items existed below the fold -
// the box looked identical whether the list had 3 items or 63. A user
// clicking "GL Matrix" (item 9 of 19, dropdown_open() centres the scroll on
// the current selection) saw a box that looked complete and never found the
// ten new effects sitting just out of view. Same bug class as #533 (font
// dialog list overflow) and #512 (no shared scrollable-list widget).
//
// #512 follow-up: that fix was landed directly in this widget (raised
// DD_VISIBLE, drew explicit scrollbar chevrons) because there was no shared
// listbox to reach for yet. There is now (libc/gui_list.h, also what the
// Editor's menus are built on, #562) so this widget is rebuilt on it here,
// retiring the hand-rolled row/clip/scrollbar math in favour of the one
// place that owns it. NO call site changes - dropdown_open/_render/_click
// keep the same signatures and the same external behaviour (centre-on-open,
// click-to-select-then-close, arrow keys, Esc/Enter to close), so every
// caller above (including the screensaver picker) benefits with zero edits
// of its own. Two small, deliberate behaviour changes come along for the
// ride, both towards OS-wide consistency rather than away from it: the
// scrollbar gutter is now 14px (GUI_SCROLL_W, matching Settings' own sidebar
// and the font dialog) instead of this widget's previous one-off 8px, and
// the wheel now moves 3 rows per notch (gui_scroll's OS-wide convention,
// also used by Files) instead of this widget's previous one-off 1 row.
//
// #74 FIX (2026-08-20): Up/Down used to call g_dd_on_change() on EVERY
// highlight move, not just on commit - arrowing from item 1 to item 5
// applied items 2, 3 and 4 on the way through. That was never cosmetic here:
// every on_change in this file does a real syscall and/or a real disk write
// (dock_dd_changed() writes DOCKSTYL.CFG, rotation_dd_changed() has the
// kernel fat_write_file() /boot/ROTATE.TXT on every call, theme_dd_changed()
// persists THEME.CFG and can pop a contrast-correction toast, wallpaper/
// cursor/font/icon-size all fire a live SYS_SET_* syscall). There is no
// consumer here that wants a free, side-effect-free live preview - every one
// of them IS the side effect - so the fix is uniform, not a dual
// on_highlight/on_commit split: Up/Down now only move *g_dd_sel (which also
// updates the label text drawn behind the closed control, so the user still
// sees what they are about to choose) and DO NOT call on_change. on_change
// fires exactly once, on an explicit commit: Enter/Space (arrow-key path) or
// clicking a row (dropdown_click). Esc, and a click that misses every row
// (dropdown_click's "committed" is 0), both CANCEL: *g_dd_sel is restored to
// g_dd_orig (the value at dropdown_open() time) and on_change is not called.
// Losing window focus (EVENT_WINDOW_BLUR) is not handled anywhere in this
// file today - it falls through to the default no-op, for every control,
// not just this one - so #74 did not add a blur handler either. That is
// safe now by construction rather than by a deliberate choice: a blurred,
// still-open dropdown just sits on whatever value the last arrow preview
// left it on, and since that preview no longer calls on_change, nothing has
// actually been applied yet. Giving blur its own explicit cancel is left as
// a follow-up, not a regression #74 introduced.
#define DD_ROW      26
#define DD_VISIBLE  12   // rows shown before the popup scrolls (a real
                         // scrollbar now, via gui_list/gui_scroll - not a
                         // hard cutoff, so raising or lowering this can no
                         // longer silently hide items).
static int   g_dd_open = 0, g_dd_x, g_dd_y, g_dd_w, g_dd_count;
static int  *g_dd_sel = 0;
static int   g_dd_orig = 0;    // #74: value at open time, for Esc/miss-click cancel
static const char *const *g_dd_items = 0;
static void (*g_dd_on_change)(void) = 0;
static gui_list_t g_dd_list;

// (#307 follow-up) Both of these are now one-line adapters over the SHARED
// gui_chevron() in libc (gui_style.h). The bodies used to live here, and the
// "upward-pointing twin" DID NOT POINT UP: its loop reversed the row ORDER but
// not the widths, so it emitted {cy:7, cy+1:5, cy+2:3, cy+3:1} - byte for byte
// what draw_chevron_down() emits. The "more items above" cue has been a DOWN
// arrow ever since it was written. Calling the shared primitive fixes that by
// construction rather than by re-deriving the same four rows a third time.
//
// The +2 preserves every existing call site's pixels: these two took a TOP-row
// y, gui_chevron() takes the mark's CENTRE.
static void draw_chevron_down(int cx, int cy, uint32_t col) {
    gui_chevron(window_handle, cx, cy + 2, GUI_CHEV_DOWN, col);
}
static void draw_chevron_up(int cx, int cy, uint32_t col) {
    gui_chevron(window_handle, cx, cy + 2, GUI_CHEV_UP, col);
}

static void draw_dropdown_n(int x, int y, int width, const char *value, bool active, int count) {
    focus_add(x, y, width, 28, 0);
    if (gui_active_style().base == GUI_STYLE_MODERN) {
        gui_fill_rounded(window_handle, x, y, width, 28, GUI_RADIUS, active ? COL_ACCENT : COL_INPUT_BORDER);
        gui_fill_rounded(window_handle, x+1, y+1, width-2, 26, GUI_RADIUS-1, COL_INPUT_BG);
    } else {
        uint32_t bg = active ? COL_PANEL_ACTIVE : COL_INPUT_BG;
        win_draw_rect(window_handle, x, y, width, 28, bg);
        gui_draw_rect_outline(window_handle, x, y, width, 28, active ? gui_pal()->focus : COL_INPUT_BORDER);  // #745
    }
    win_draw_text(window_handle, x + 10, y + 6, value, COL_TEXT_PRIMARY);
    // (#261) item-count badge in small text just left of the chevron, so the
    // user sees how many entries the list holds without opening it.
    if (count > 0) {
        char cb[8]; gui_itoa(count, cb, sizeof(cb));
        int cw = 0; for (const char *q = cb; *q; q++) cw += 6;   // ~6px/glyph advance
        win_draw_text_small(window_handle, x + width - 24 - cw, y + 9, cb, COL_TEXT_SECONDARY);
    }
    draw_chevron_down(x + width - 16, y + 11, COL_TEXT_SECONDARY);
}
// Back-compat wrapper: dropdowns with no known count pass 0 (no badge).
static void draw_dropdown(int x, int y, int width, const char *value, bool active) {
    draw_dropdown_n(x, y, width, value, active, 0);
}

// NOTE on the +2/1px margin below (was +4/2px before #512): gui_list_config()
// derives its scroll VIEWPORT as h-2 (a 1px inset all round, the same
// convention gui_font.c's lists use). Keeping this widget's own 2px margin
// while gui_list computed clamps against h-2 would make gui_scroll_max() 2px
// short of a whole row - the thumb could never quite reach a clean row
// boundary and the last row would sometimes be one wheel-notch short of
// reachable. Matching gui_list's 1px convention exactly (h = vis*DD_ROW+2)
// fixes that at the source rather than special-casing the clamp here; the
// visual difference is one pixel of margin, not visible at these sizes.
static void dropdown_open(int x, int y, int w, const char *const *items,
                          int count, int *sel, void (*on_change)(void)) {
    g_dd_open = 1; g_dd_x = x; g_dd_y = y; g_dd_w = w;
    g_dd_items = items; g_dd_count = count; g_dd_sel = sel; g_dd_on_change = on_change;
    g_dd_orig = *sel;   // #74: remember it so Esc / a miss-click can cancel back to it
    int vis = count < DD_VISIBLE ? count : DD_VISIBLE;
    if (vis < 1) vis = 1;
    // Seed a real, window-size-independent height so gui_scroll_max() is
    // already sane if a wheel/key event lands before the next draw calls
    // dropdown_refresh() with the true, WIN_HEIGHT-clamped geometry.
    gui_list_config(&g_dd_list, x, y + 28, w, vis * DD_ROW + 2, DD_ROW, count);
    int centered = *sel - vis / 2;                       // centre the current item
    if (centered > count - vis) centered = count - vis;
    if (centered < 0) centered = 0;
    gui_scroll_set(&g_dd_list.scroll, centered * DD_ROW);
}

// Recompute this frame's popup geometry into the shared list (flips above the
// control if it would overflow WIN_HEIGHT, exactly like the old
// dropdown_geom()) and return the row count shown. gui_list/gui_scroll now
// own the scroll offset/clamp/thumb math that used to be hand-rolled here.
static int dropdown_refresh(void) {
    int v = g_dd_count < DD_VISIBLE ? g_dd_count : DD_VISIBLE;
    if (v < 1) v = 1;
    int h = v * DD_ROW + 2;
    int y = g_dd_y + 28;
    if (y + h > WIN_HEIGHT - 4 && g_dd_y - h >= 0) y = g_dd_y - h;  // flip above if needed
    gui_list_config(&g_dd_list, g_dd_x, y, g_dd_w, h, DD_ROW, g_dd_count);
    return v;
}

static void dropdown_render(void) {
    if (!g_dd_open) return;
    int vis = dropdown_refresh();
    int bx = g_dd_list.x, by = g_dd_list.y, bw = g_dd_list.w, bh = g_dd_list.h;
    int scroll_row = gui_list_first(&g_dd_list);
    win_draw_rect(window_handle, bx, by, bw, bh, COL_INPUT_BG);
    gui_draw_rect_outline(window_handle, bx, by, bw, bh, COL_ACCENT);
    for (int r = 0; r < vis; r++) {
        int idx = scroll_row + r;
        if (idx < 0 || idx >= g_dd_count) continue;
        int ry = by + 1 + r * DD_ROW;
        if (idx == *g_dd_sel)
            win_draw_rect(window_handle, bx + 1, ry, bw - 2, DD_ROW, COL_ACCENT);
        win_draw_text(window_handle, bx + 10, ry + 5, g_dd_items[idx], COL_TEXT_PRIMARY);
    }
    if (g_dd_count > vis) {                              // scrollbar
        // #560: widened 5px->8px and given real contrast (COL_ACCENT
        // thumb instead of COL_TEXT_SECONDARY) so a long list reads as
        // scrollable at a glance, plus explicit up/down chevrons at the
        // track ends whenever there is more content in that direction - the
        // thumb alone was too easy to miss. Position/limits now come from
        // the shared gui_scroll_t (g_dd_list.scroll, #512) instead of a
        // hand-rolled index, but the look is unchanged (see the margin note
        // above dropdown_open for the one pixel that did move).
        int track_h = bh - 2;
        int thumb_h = track_h * vis / g_dd_count; if (thumb_h < 12) thumb_h = 12;
        int denom = g_dd_count - vis; if (denom < 1) denom = 1;
        int thumb_y = by + 1 + (track_h - thumb_h) * scroll_row / denom;
        win_draw_rect(window_handle, bx + bw - 10, by + 1, 8, track_h, COL_SLIDER_TRACK);
        win_draw_rect(window_handle, bx + bw - 10, thumb_y, 8, thumb_h, COL_ACCENT);
        if (scroll_row > 0)
            draw_chevron_up(bx + bw - 6, by + 4, COL_ACCENT);
        if (scroll_row + vis < g_dd_count)
            draw_chevron_down(bx + bw - 6, by + bh - 8, COL_ACCENT);
    }
}

// Click while a dropdown is open: click-on-a-row COMMITS it (fires
// on_change exactly once, #74), same as Enter. A click that lands inside
// the popup box but not on a real row (the scrollbar gutter, or past the
// last row) or outside the box entirely is a MISS - same as Esc, it
// CANCELS: *g_dd_sel is restored to g_dd_orig (whatever arrow-key preview
// had left it on is discarded) and on_change is not called. Before #74 a
// miss here already skipped on_change, but it also never undid a preview
// that arrow keys had already half-applied by calling on_change on every
// highlight move; now there is no such half-applied state to undo, but the
// restore is kept so a plain miss-click can never leave the CONTROL'S OWN
// label showing a value that was never committed.
static void dropdown_click(int mx, int my) {
    dropdown_refresh();
    int bx = g_dd_list.x, by = g_dd_list.y, bw = g_dd_list.w, bh = g_dd_list.h;
    int committed = 0;
    if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
        int scroll_row = gui_list_first(&g_dd_list);
        int r = (my - (by + 1)) / DD_ROW;
        int idx = scroll_row + r;
        if (idx >= 0 && idx < g_dd_count) {
            *g_dd_sel = idx;
            if (g_dd_on_change) g_dd_on_change();
            committed = 1;
        }
    }
    if (!committed) *g_dd_sel = g_dd_orig;
    g_dd_open = 0;
    draw_all();
}

static void draw_button(int x, int y, int width, const char *label, bool primary, bool hovered) {
    focus_add(x, y, width, 30, 0);
    gui_button(window_handle, x, y, width, 30, label,
               primary ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY,
               hovered ? GUI_ST_HOVER : GUI_ST_NORMAL);
}

static void draw_button_small(int x, int y, int width, const char *label, bool primary) {
    focus_add(x, y, width, 24, 0);
    gui_button(window_handle, x, y, width, 24, label,
               primary ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, GUI_ST_NORMAL);
}

static void draw_color_box(int x, int y, uint32_t color, bool selected) {
    focus_add(x, y, 32, 32, 0);
    win_draw_rect(window_handle, x, y, 32, 32, color);
    if (selected) {
        gui_draw_rect_outline(window_handle, x - 2, y - 2, 36, 36, COL_TEXT_PRIMARY);
        gui_draw_rect_outline(window_handle, x - 1, y - 1, 34, 34, COL_TEXT_PRIMARY);
    } else {
        gui_draw_rect_outline(window_handle, x, y, 32, 32, COL_INPUT_BORDER);
    }
}

static void draw_radio_button(int x, int y, const char *label, bool selected) {
    focus_add(x, y, 160, 18, 0);
    // Rounded (circular) radio in the engine palette.
    gui_fill_rounded(window_handle, x, y, 18, 18, 9, selected ? COL_ACCENT : COL_INPUT_BG);
    gui_rounded_border(window_handle, x, y, 18, 18, 9, selected ? COL_ACCENT : COL_INPUT_BORDER);
    if (selected) {
        gui_fill_rounded(window_handle, x + 5, y + 5, 8, 8, 4, gui_ink_on(COL_ACCENT));
    }
    win_draw_text(window_handle, x + 26, y + 1, label, COL_TEXT_PRIMARY);
}

static void draw_checkbox(int x, int y, const char *label, bool checked) {
    focus_add(x, y, 18, 18, 0);
    // Engine checkbox: real anti-aliased tick + theme-aware styling.
    gui_checkbox(window_handle, x, y, 18, checked, label, GUI_ST_NORMAL);
}

static void draw_progress_bar(int x, int y, int width, int percent, uint32_t color) {
    win_draw_rect(window_handle, x, y, width, 12, COL_SLIDER_TRACK);
    if (percent > 0) {
        int fill = (width * percent) / 100;
        win_draw_rect(window_handle, x, y, fill, 12, color);
    }
}


static void draw_option_buttons(int x, int y, const char **options, int count, int selected) {
    // Engine-styled segmented control: rounded (modern) or beveled (classic),
    // centered TTF labels, accent fill on the selected item.
    int r = (gui_active_style().base == GUI_STYLE_CLASSIC) ? 0 : 6;
    for (int i = 0; i < count; i++) {
        int btn_x = x + i * 90;
        focus_add(btn_x, y, 82, 28, 0);
        uint32_t bg  = (i == selected) ? COL_ACCENT : COL_BUTTON_BG;
        uint32_t ink = (i == selected) ? gui_ink_on(COL_ACCENT) : COL_TEXT_PRIMARY;
        gui_fill_rounded(window_handle, btn_x, y, 82, 28, r, bg);
        gui_rounded_border(window_handle, btn_x, y, 82, 28, r,
                           (i == selected) ? COL_ACCENT_HOVER : COL_INPUT_BORDER);
        gui_text_ttf_centered(window_handle, btn_x, y, 82, 28, options[i], ink, 14);
    }
}


// =============================================================================
// Helper Utilities
// =============================================================================

static void copy_str(char *dst, const char *src, int max_len) {
    int i = 0;
    while (i < max_len - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// =============================================================================
// #786: THE ONE place the Network panel learns what the stack is doing.
// =============================================================================
// This block used to be copy-pasted at four sites (first paint, panel entry,
// DHCP toggle, and - the one that mattered - NOT at all after Apply). The
// Apply path instead copied the TYPED TEXT into these variables, so the card
// at the top of the page echoed the form rather than reporting the machine.
// That is what made the owner doubt a correct observation: he changed DNS, the
// card agreed with him, and nothing resolved differently.
//
// One function, called from all four, so the card can only ever show live
// state. If a field here disagrees with what was typed, that disagreement is
// the TRUTH and must be visible.
static void net_pull_live(void) {
    net_info_t ni;
    if (get_net_info(&ni, (long)sizeof(ni)) == 0) {
        copy_str(ip_address,  ni.ip,      sizeof(ip_address));
        copy_str(gateway,     ni.gateway, sizeof(gateway));
        copy_str(subnet_mask, ni.netmask, sizeof(subnet_mask));
        // ni.dns is dns_get_server() as of #786 - the resolver that will
        // actually be queried, not (as before) whatever DHCP offered with the
        // gateway substituted in when it offered nothing.
        copy_str(dns_primary, ni.dns,     sizeof(dns_primary));
        copy_str(mac_address, ni.mac,     sizeof(mac_address));
        ethernet_connected = ni.connected;
    }
    net_status_t nst;
    if (sys_net_status(&nst) == 0) {
        // #144: the DHCP toggle reflects REALITY, not the initializer.
        dhcp_enabled = !nst.config_static;
        // #786: what the lease offered, rendered as a fact and not as a
        // second resolver. 0 is a REAL state ("your DHCP server gave you
        // none") and is said out loud rather than left blank.
        if (nst.dns_dhcp == 0) {
            copy_str(dns_offered, "(none offered)", sizeof(dns_offered));
        } else {
            unsigned int v = nst.dns_dhcp;
            char *o = dns_offered;
            for (int sh = 24; sh >= 0; sh -= 8) {
                unsigned int b = (v >> sh) & 0xFF;
                if (b >= 100) *o++ = (char)('0' + b / 100);
                if (b >= 10)  *o++ = (char)('0' + (b / 10) % 10);
                *o++ = (char)('0' + b % 10);
                if (sh) *o++ = '.';
            }
            *o = '\0';
        }
    }
}

// =============================================================================
// #382: real hardware facts, queried live from the kernel (no fabricated data).
// Reuses existing read-only syscalls: SYS_SYSINFO (CPUID brand/vendor, logical
// core count, real PMM RAM, uptime) and SYS_DEV_PCI_LIST (real display / network
// adapter identity). Queried once and cached; drawn by the Display + About tabs.
// =============================================================================
static devinfo_sysinfo_t g_sysinfo;
static int      g_sysinfo_ok = 0;
static char     g_gpu_name[64] = "";
static char     g_nic_name[48] = "";
static uint16_t g_gpu_vid = 0, g_gpu_did = 0;
static int      g_hwinfo_loaded = 0;
// (#382 pass2) Real presence flags probed once in hwinfo_load():
//   g_audio_*  - a PCI audio controller (class 0x04) actually exists.
//   g_bt_present / g_wifi_present - a real Bluetooth / Wi-Fi radio exists.
// These replace the previous cosmetic dropdowns / mock scan results with
// honest "not available" states when the hardware is absent.
//
// (#237) g_bt_vid/g_bt_pid and g_wifi_vid/g_wifi_did/g_wifi_is_usb are the
// REAL vendor/device (or vendor/product) id of whichever adapter tripped the
// presence probe, captured at that same point below - never invented, and 0
// when no adapter was found. They exist so the "adapter present, no driver"
// panel state can show something concrete a future driver author can grep
// a datasheet for, instead of just a bare yes/no.
static char     g_audio_name[48] = "";
static int      g_audio_present = 0;
static int      g_bt_present = 0;
static int      g_wifi_present = 0;
static uint16_t g_bt_vid = 0, g_bt_pid = 0;          // USB vendor:product of the BT radio
static uint16_t g_wifi_vid = 0, g_wifi_did = 0;      // PCI vendor:device, or USB vendor:product
static int      g_wifi_is_usb = 0;                    // 1 if g_wifi_vid/did are a USB id, not PCI

// Append s onto the NUL-terminated dst, never exceeding cap-1 chars.
static void hw_append(char *dst, int cap, const char *s) {
    int i = 0; while (dst[i]) i++;
    for (int j = 0; s[j] && i < cap - 1; j++) dst[i++] = s[j];
    dst[i] = '\0';
}

// (#237) "VVVV:DDDD" real vendor:device (or vendor:product) id, e.g.
// "14E4:4359". out must hold at least 10 bytes. Same digit layout the
// Display panel's inline GPU-id block already uses; factored out here so the
// Wi-Fi/Bluetooth "adapter present, no driver" cards don't triplicate it.
static void hw_fmt_id(uint16_t vid, uint16_t did, char *out) {
    static const char hx[] = "0123456789ABCDEF";
    out[0] = hx[(vid >> 12) & 0xF]; out[1] = hx[(vid >> 8) & 0xF];
    out[2] = hx[(vid >> 4) & 0xF];  out[3] = hx[vid & 0xF];
    out[4] = ':';
    out[5] = hx[(did >> 12) & 0xF]; out[6] = hx[(did >> 8) & 0xF];
    out[7] = hx[(did >> 4) & 0xF];  out[8] = hx[did & 0xF];
    out[9] = 0;
}

// Short human name for a PCI vendor id (best effort; empty string if unknown).
static const char *pci_vendor_name(uint16_t vid) {
    switch (vid) {
        case 0x8086: return "Intel";
        case 0x1234: return "QEMU";
        case 0x1AF4: return "VirtIO";
        case 0x1B36: return "QEMU";
        case 0x10DE: return "NVIDIA";
        case 0x1002: return "AMD";
        case 0x15AD: return "VMware";
        case 0x1013: return "Cirrus Logic";
        case 0x80EE: return "VirtualBox";
        case 0x10EC: return "Realtek";
        case 0x14E4: return "Broadcom";
        case 0x0B95: return "ASIX";
        case 0x106B: return "Apple";
        default:     return "";
    }
}

// Friendly name for a well-known PCI audio controller (0 if not specifically
// known). Only the controllers the kernel audio path actually drives / that a
// QEMU or real machine exposes are named; anything else falls back to the
// generic "<vendor> Audio Controller".
static const char *audio_device_name(uint16_t vid, uint16_t did) {
    if (vid == 0x8086 && did == 0x2415) return "Intel 82801AA AC'97 Audio";
    if (vid == 0x8086 && did == 0x2668) return "Intel HD Audio (ICH6)";
    if (vid == 0x8086 && did == 0x293E) return "Intel HD Audio";
    if (vid == 0x1274)                  return "Ensoniq AudioPCI (ES1370)";
    if (vid == 0x1AF4)                  return "VirtIO Sound";
    return 0;
}

// Friendly name for a well-known USB Ethernet chip (0 if unknown). Lets the
// Network adapter identity name a USB NIC (e.g. the iMac's ASIX AX88772B) that
// the PCI-class scan cannot see.
static const char *usb_nic_name(uint16_t vid, uint16_t pid) {
    if (vid == 0x0B95) {   // ASIX
        if (pid == 0x1790 || pid == 0x178A) return "ASIX AX88179 USB 3.0 Gigabit Ethernet";
        return "ASIX AX88772 USB 2.0 Ethernet";
    }
    if (vid == 0x0BDA && (pid == 0x8152 || pid == 0x8153)) return "Realtek USB Ethernet";
    if (vid == 0x0424)                                     return "Microchip LAN95xx USB Ethernet";
    if (vid == 0x13B1)                                     return "Linksys USB Ethernet";
    if (vid == 0x2001)                                     return "D-Link USB Ethernet";
    if (vid == 0x0B95)                                     return "ASIX USB Ethernet";
    return 0;
}

// Friendly name for well-known display adapters (0 if not specifically known).
static const char *gpu_device_name(uint16_t vid, uint16_t did) {
    if (vid == 0x1234 && did == 0x1111) return "QEMU Standard VGA";
    if (vid == 0x1AF4 && did == 0x1050) return "VirtIO GPU";
    if (vid == 0x15AD)                  return "VMware SVGA II";
    if (vid == 0x1013)                  return "Cirrus Logic GD5446";
    if (vid == 0x80EE)                  return "VirtualBox VGA";
    if (vid == 0x1B36 && did == 0x0100) return "QEMU QXL";
    if (vid == 0x8086)                  return "Intel Graphics";
    return 0;
}

static void hwinfo_load(void) {
    if (g_hwinfo_loaded) return;
    g_hwinfo_loaded = 1;
    g_sysinfo_ok = (sys_sysinfo(&g_sysinfo) == 0);

    devinfo_pci_t pl[48];
    int n = sys_dev_pci_list(pl, 48);
    if (n < 0) n = 0;
    if (n > 48) n = 48;
    for (int i = 0; i < n; i++) {
        // First display controller (class 0x03) -> graphics adapter.
        if (pl[i].class_code == 0x03 && g_gpu_name[0] == 0) {
            g_gpu_vid = pl[i].vendor_id;
            g_gpu_did = pl[i].device_id;
            const char *nm = gpu_device_name(pl[i].vendor_id, pl[i].device_id);
            if (nm) {
                copy_str(g_gpu_name, nm, sizeof(g_gpu_name));
            } else {
                const char *v = pci_vendor_name(pl[i].vendor_id);
                g_gpu_name[0] = 0;
                if (v[0]) { hw_append(g_gpu_name, sizeof(g_gpu_name), v);
                            hw_append(g_gpu_name, sizeof(g_gpu_name), " "); }
                hw_append(g_gpu_name, sizeof(g_gpu_name),
                          pl[i].class_name[0] ? pl[i].class_name : "Display adapter");
            }
        }
        // Network controller (class 0x02): subclass 0x80 = wireless (Wi-Fi),
        // anything else is a wired NIC. Name the first wired NIC; note Wi-Fi
        // presence separately so the Wi-Fi panel is honest.
        if (pl[i].class_code == 0x02) {
            if (pl[i].subclass == 0x80) {
                // (#237) capture the REAL PCI id of the first match only, same
                // "first wins" rule the GPU/audio/NIC branches already use.
                if (!g_wifi_present) {
                    g_wifi_vid = pl[i].vendor_id; g_wifi_did = pl[i].device_id;
                    g_wifi_is_usb = 0;
                }
                g_wifi_present = 1;
            } else if (g_nic_name[0] == 0) {
                const char *v = pci_vendor_name(pl[i].vendor_id);
                g_nic_name[0] = 0;
                if (v[0]) { hw_append(g_nic_name, sizeof(g_nic_name), v);
                            hw_append(g_nic_name, sizeof(g_nic_name), " "); }
                hw_append(g_nic_name, sizeof(g_nic_name),
                          pl[i].class_name[0] ? pl[i].class_name : "Network adapter");
            }
        }
        // First audio controller (class 0x04 = multimedia) -> real output device.
        if (pl[i].class_code == 0x04 && !g_audio_present) {
            g_audio_present = 1;
            const char *an = audio_device_name(pl[i].vendor_id, pl[i].device_id);
            if (an) {
                copy_str(g_audio_name, an, sizeof(g_audio_name));
            } else {
                const char *v = pci_vendor_name(pl[i].vendor_id);
                g_audio_name[0] = 0;
                if (v[0]) { hw_append(g_audio_name, sizeof(g_audio_name), v);
                            hw_append(g_audio_name, sizeof(g_audio_name), " "); }
                hw_append(g_audio_name, sizeof(g_audio_name),
                          pl[i].class_name[0] ? pl[i].class_name : "Audio Controller");
            }
        }
    }

    // USB device scan: name a USB NIC when there is no PCI NIC (e.g. the iMac's
    // ASIX AX88772B), and detect a real USB Bluetooth radio / Wi-Fi dongle.
    devinfo_usb_t ul[48];
    int un = sys_dev_usb_list(ul, 48);
    if (un < 0) un = 0;
    if (un > 48) un = 48;
    for (int i = 0; i < un; i++) {
        if (ul[i].is_controller) continue;
        // Bluetooth radio: USB base class 0xE0 (wireless) / subclass 0x01 (RF) /
        // protocol 0x01 (Bluetooth), or a known BT-dongle vendor (CSR).
        if (ul[i].dev_class == 0xE0 && ul[i].subclass == 0x01 && ul[i].protocol == 0x01) {
            if (!g_bt_present) { g_bt_vid = ul[i].vendor_id; g_bt_pid = ul[i].product_id; }
            g_bt_present = 1;
        }
        if (ul[i].vendor_id == 0x0A12) {   // Cambridge Silicon Radio
            if (!g_bt_present) { g_bt_vid = ul[i].vendor_id; g_bt_pid = ul[i].product_id; }
            g_bt_present = 1;
        }
        // USB Wi-Fi dongle: base class 0xE0 subclass 0x01 but not the BT protocol,
        // or a wireless-controller class device.
        if (ul[i].dev_class == 0xE0 && ul[i].subclass == 0x01 && ul[i].protocol != 0x01) {
            if (!g_wifi_present) {
                g_wifi_vid = ul[i].vendor_id; g_wifi_did = ul[i].product_id;
                g_wifi_is_usb = 1;
            }
            g_wifi_present = 1;
        }
        // USB Ethernet NIC identity (only if no PCI NIC named it already).
        if (g_nic_name[0] == 0) {
            const char *un_name = usb_nic_name(ul[i].vendor_id, ul[i].product_id);
            if (un_name) {
                copy_str(g_nic_name, un_name, sizeof(g_nic_name));
            } else if (ul[i].dev_class == 0x02) {   // CDC (Communications) = CDC-ECM/NCM
                copy_str(g_nic_name, "USB Ethernet (CDC)", sizeof(g_nic_name));
            }
        }
    }

    if (g_gpu_name[0] == 0) copy_str(g_gpu_name, "Unknown display adapter", sizeof(g_gpu_name));
    if (g_nic_name[0] == 0) copy_str(g_nic_name, "None", sizeof(g_nic_name));
}

static void copy_to_modal_field(int idx, const char *src) {
    int i = 0;
    while (src[i] && i < 63) { modal_field[idx][i] = src[i]; i++; }
    modal_field[idx][i] = '\0';
    modal_cursor[idx] = i;
    modal_caret[idx] = i;
}

// (#382 pass2) Per-account email is a REAL editable stored field, kept in
// /CONFIG/USEREMAIL.CFG as "username=email" lines. The kernel account DB
// (SYS_LIST_USERS) has no email column, so the Settings app owns this store:
// Edit Profile writes it and it survives reboots. Previously the "email" field
// just mirrored the username, which was a cosmetic fake.
#define USEREMAIL_CFG "/CONFIG/USEREMAIL.CFG"

static void useremail_get(const char *user, char *out, int cap) {
    out[0] = 0;
    int fd = sys_open(USEREMAIL_CFG, 0);
    if (fd < 0) return;
    static char b[1024];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    int ul = 0; while (user[ul]) ul++;
    int i = 0;
    while (b[i]) {
        int ls = i;
        while (b[i] && b[i] != '=' && b[i] != '\n') i++;
        if (b[i] == '=') {
            int keylen = i - ls;
            int match = (keylen == ul);
            for (int j = 0; match && j < keylen; j++) if (b[ls + j] != user[j]) match = 0;
            i++;   // skip '='
            int vs = i;
            while (b[i] && b[i] != '\n') i++;
            if (match) {
                int k = 0;
                for (int j = vs; j < i && k < cap - 1; j++) out[k++] = b[j];
                out[k] = 0;
                return;
            }
        } else {
            while (b[i] && b[i] != '\n') i++;
        }
        if (b[i] == '\n') i++;
    }
}

static void useremail_set(const char *user, const char *email) {
    static char b[1024]; int bn = 0; b[0] = 0;
    int fd = sys_open(USEREMAIL_CFG, 0);
    if (fd >= 0) { long n = sys_read(fd, b, sizeof(b) - 1); sys_close(fd);
                   if (n > 0) { bn = (int)n; b[bn] = 0; } }
    static char out[1200]; int on = 0;
    int ul = 0; while (user[ul]) ul++;
    // Copy every existing line except this user's (which we rewrite below).
    int i = 0;
    while (i < bn && b[i]) {
        int ls = i;
        while (b[i] && b[i] != '=' && b[i] != '\n') i++;
        int keylen = i - ls;
        int match = (keylen == ul);
        for (int j = 0; match && j < keylen; j++) if (b[ls + j] != user[j]) match = 0;
        int le = ls;
        while (b[le] && b[le] != '\n') le++;
        if (b[le] == '\n') le++;
        if (!match)
            for (int j = ls; j < le && on < (int)sizeof(out) - 1; j++) out[on++] = b[j];
        i = le;
    }
    for (int j = 0; user[j] && on < (int)sizeof(out) - 1; j++) out[on++] = user[j];
    if (on < (int)sizeof(out) - 1) out[on++] = '=';
    for (int j = 0; email[j] && on < (int)sizeof(out) - 1; j++) out[on++] = email[j];
    if (on < (int)sizeof(out) - 1) out[on++] = '\n';
    // #743: was unlink-then-open, so a failed open deleted every user's email
    // mapping. userconf_write_all never unlinks.
    if (userconf_write_all(USEREMAIL_CFG, out, (unsigned long)on) != 0)
        save_failed("USEREMAIL.CFG");
}

// (#745) Per-account avatar, same side-table pattern as USEREMAIL_CFG above -
// a direct sibling, same "username=value" format, same read-modify-write
// helper shape (design doc docs/LOGIN_AVATARS_AND_PROFILE.html section 8).
// The kernel's user_info_t/SYS_LIST_USERS has no picture column and this
// does not add one; the value grammar is:
//   (absent)        default: mono, color = avatar_palette[uid % 8]
//   mono:RRGGBB     monogram, explicit color override (written by the
//                   swatch picker in the Edit Profile modal below)
//   stock:NAME      reserved - would reference /AVATARS/NAME.ICN via the
//                   existing MICO icon loader. No stock set ships yet (see
//                   design doc 10.2); a value in this shape is currently
//                   unreachable from this UI and, if ever read back before a
//                   loader exists, must degrade to the mono default, not
//                   crash or blank.
//   custom:PATH     reserved only, never written or read - see design doc
//                   10.3 for why (no userland image decoder, no file
//                   browser). Treat exactly like the degraded case above.
#define USERAVATAR_CFG "/CONFIG/USERAVATAR.CFG"

static void useravatar_get(const char *user, char *out, int cap) {
    out[0] = 0;
    int fd = sys_open(USERAVATAR_CFG, 0);
    if (fd < 0) return;
    static char b[1024];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    int ul = 0; while (user[ul]) ul++;
    int i = 0;
    while (b[i]) {
        int ls = i;
        while (b[i] && b[i] != '=' && b[i] != '\n') i++;
        if (b[i] == '=') {
            int keylen = i - ls;
            int match = (keylen == ul);
            for (int j = 0; match && j < keylen; j++) if (b[ls + j] != user[j]) match = 0;
            i++;   // skip '='
            int vs = i;
            while (b[i] && b[i] != '\n') i++;
            if (match) {
                int k = 0;
                for (int j = vs; j < i && k < cap - 1; j++) out[k++] = b[j];
                out[k] = 0;
                return;
            }
        } else {
            while (b[i] && b[i] != '\n') i++;
        }
        if (b[i] == '\n') i++;
    }
}

static void useravatar_set(const char *user, const char *spec) {
    static char b[1024]; int bn = 0; b[0] = 0;
    int fd = sys_open(USERAVATAR_CFG, 0);
    if (fd >= 0) { long n = sys_read(fd, b, sizeof(b) - 1); sys_close(fd);
                   if (n > 0) { bn = (int)n; b[bn] = 0; } }
    static char out[1200]; int on = 0;
    int ul = 0; while (user[ul]) ul++;
    // Copy every existing line except this user's (which we rewrite below).
    int i = 0;
    while (i < bn && b[i]) {
        int ls = i;
        while (b[i] && b[i] != '=' && b[i] != '\n') i++;
        int keylen = i - ls;
        int match = (keylen == ul);
        for (int j = 0; match && j < keylen; j++) if (b[ls + j] != user[j]) match = 0;
        int le = ls;
        while (b[le] && b[le] != '\n') le++;
        if (b[le] == '\n') le++;
        if (!match)
            for (int j = ls; j < le && on < (int)sizeof(out) - 1; j++) out[on++] = b[j];
        i = le;
    }
    for (int j = 0; user[j] && on < (int)sizeof(out) - 1; j++) out[on++] = user[j];
    if (on < (int)sizeof(out) - 1) out[on++] = '=';
    for (int j = 0; spec[j] && on < (int)sizeof(out) - 1; j++) out[on++] = spec[j];
    if (on < (int)sizeof(out) - 1) out[on++] = '\n';
    // userconf_write_all never unlinks (#743) - same safety as useremail_set.
    if (userconf_write_all(USERAVATAR_CFG, out, (unsigned long)on) != 0)
        save_failed("USERAVATAR.CFG");
}

// (#704/#745) INTENTIONALLY NOT themed - per-account identity tint, not
// chrome. Same array, same values, as login.c's g_avatar_id_palette (design
// doc section 8.2: both index by uid % 8 now, not list position, so an
// account's color is stable across the list being reordered).
static const uint32_t avatar_palette[8] = {
    0x00569CD6, 0x0066BB66, 0x00CC8844, 0x00AA66CC,
    0x00CC6666, 0x0044AAAA, 0x00888888, 0x00BBAA44
};

// Parse a USERAVATAR.CFG value into a display color. Only "mono:RRGGBB" is
// currently produced or read for real; "stock:"/"custom:"/anything else
// (including an unset/empty spec) degrades to the uid-keyed palette default,
// per the design doc's forward-compatible grammar (section 8.1/10.3) - this
// must never blank or crash on a value this UI cannot yet act on.
static uint32_t avatar_color_from_spec(const char *spec, unsigned int uid) {
    if (spec[0] == 'm' && spec[1] == 'o' && spec[2] == 'n' && spec[3] == 'o' && spec[4] == ':') {
        unsigned long v = strtoul(spec + 5, 0, 16);
        return 0x00FFFFFF & (uint32_t)v;
    }
    return avatar_palette[uid % 8];
}

// Reverse lookup for the Edit Profile swatch picker: which palette slot (if
// any) an account's current color came from, so opening the modal highlights
// the color it is ALREADY using rather than always defaulting to slot 0.
static int avatar_palette_index_for(uint32_t color, unsigned int uid) {
    for (int i = 0; i < 8; i++) if (avatar_palette[i] == color) return i;
    return (int)(uid % 8);
}

// Load the real account list from the kernel so the Users panel reflects the
// actual /CONFIG/PASSWD database. Add/Remove operate on the live accounts, so
// the list is re-read after each change. Falls back to the built-in defaults
// if the syscall is unavailable (returns <= 0).
static void users_refresh(void) {
    user_info_t ui[8];
    int n = sys_list_users(ui, 8);
    if (n <= 0) return;
    // #785: bind the session identity from the FULL list, before the display
    // cap below truncates it. sys_list_users() returns table order and the
    // kernel seeds root first, so users[0] is root on every image: taking the
    // target from current_user_idx (initialised to 0 and never bound to the
    // session) meant Change Password always aimed at root, whoever was logged
    // in. A non-root session then got "Current password incorrect" for a
    // password that was perfectly correct, because the account was wrong.
    {
        unsigned int me = (unsigned int)sys_getuid();
        g_session_user[0] = 0;
        for (int i = 0; i < n; i++) {
            if (ui[i].uid == me) { copy_str(g_session_user, ui[i].username, 32); break; }
        }
    }
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) {
        copy_str(users[i].username, ui[i].username, 32);
        copy_str(users[i].fullname,
                 ui[i].display_name[0] ? ui[i].display_name : ui[i].username, 64);
        useremail_get(users[i].username, users[i].email, 64);   // real stored email, "" if unset
        users[i].role = (ui[i].uid == 0) ? 0 : 1;   // uid 0 = administrator
        users[i].password_set = true;
        users[i].uid = ui[i].uid;
        // (#745) keyed by uid, not list position `i` - design doc section
        // 8.2: a fixed uid keeps an account's color stable across reboots
        // even if the table order changes, unlike the old `i & 7`.
        {
            char spec[32];
            useravatar_get(users[i].username, spec, sizeof(spec));
            users[i].avatar_color = avatar_color_from_spec(spec, users[i].uid);
        }
    }
    user_count = n;
    if (current_user_idx >= user_count) current_user_idx = 0;
    // #785: show the account that is actually logged in, when it is on screen.
    if (g_session_user[0]) {
        for (int i = 0; i < user_count; i++) {
            if (strcmp(users[i].username, g_session_user) == 0) { current_user_idx = i; break; }
        }
    }
    autologin_refresh();   // (#566) keep the autologin indicator in sync with the kernel
}

static void format_hms(char *buf, int h, int m, int s) {
    buf[0] = '0' + h / 10; buf[1] = '0' + h % 10; buf[2] = ':';
    buf[3] = '0' + m / 10; buf[4] = '0' + m % 10; buf[5] = ':';
    buf[6] = '0' + s / 10; buf[7] = '0' + s % 10; buf[8] = '\0';
}

static void format_ymd(char *buf, int y, int mo, int d) {
    buf[0] = '0' + (y / 1000) % 10; buf[1] = '0' + (y / 100) % 10;
    buf[2] = '0' + (y / 10)  % 10;  buf[3] = '0' + y % 10; buf[4] = '-';
    buf[5] = '0' + mo / 10; buf[6] = '0' + mo % 10; buf[7] = '-';
    buf[8] = '0' + d  / 10; buf[9] = '0' + d  % 10; buf[10] = '\0';
}

// #50: bring Settings onto the shared setting at startup, migrating an install
// that only ever had the old private index.
//
// The migration runs ONLY when TZ.CFG does not yet hold a zone (tz_is_set() is
// false). That test is not "is the current zone UTC": UTC is a legitimate
// choice, and treating it as "unset" would let a stale legacy index overwrite a
// zone the user picked in the first-run wizard five minutes earlier.
static void settings_tz_init(void) {
    if (!tz_is_set() && legacy_tz_idx >= 0 &&
        legacy_tz_idx < (int)ARRAY_COUNT(LEGACY_SETTINGS_TZ_OFF)) {
        int idx = tz_index_for_offset(LEGACY_SETTINGS_TZ_OFF[legacy_tz_idx]);
        if (idx >= 0) {
            // A failed write is not fatal here: the zone simply stays unset and
            // the user can pick it again. Do not report success either way.
            int rc = tz_set_index(idx);
            (void)rc;
        }
    }
    timezone_idx = tz_index();
}

// #49/#50: the timezone dropdown's on_change. It used to re-parse "UTC+HH:MM"
// out of Settings' private label array into a private minute count that only
// Settings' own panel ever consulted. It now COMMITS the choice to the one
// persisted setting, /CONFIG/TZ.CFG, which is the same file the first-run
// wizard writes and the same file every clock in the OS reads.
//
// NO REBOOT: the compositor re-reads TZ.CFG on its own throttle (TZ_REFRESH_MS,
// 2s), so the taskbar clock, the desktop clock widget, the calendar and the
// lock screen follow within about two seconds of the dropdown closing. That
// needed no new plumbing: the file was already the hand-off channel, it simply
// had no reader.
static void update_timezone_offset(void) {
    if (tz_set_index(timezone_idx) != 0)
        save_failed("TZ.CFG (time zone)");
}

static void do_export_debug(void) {
    long fd = syscall2(SYS_OPEN, (long)"/DEBUG.TXT", 1L);
    if (fd < 0) return;
    /* (#263) live version, not a hardcoded stale string */
    char vb[64]; if (get_version(vb, sizeof(vb)) <= 0) { vb[0]='?'; vb[1]=0; }
    char header[96]; int hn = 0;
    for (const char *q = "MayteraOS Debug Export\nVersion "; *q; q++) header[hn++] = *q;
    for (int k = 0; vb[k] && hn < (int)sizeof(header)-2; k++) header[hn++] = vb[k];
    header[hn++] = '\n'; header[hn] = 0;
    syscall3(SYS_WRITE, fd, (long)header, (long)my_strlen(header));
    // Append basic IP info
    {
        net_info_t ni;
        if (get_net_info(&ni, (long)sizeof(ni)) == 0) {
            const char *ip_label = "IP: ";
            syscall3(SYS_WRITE, fd, (long)ip_label, (long)my_strlen(ip_label));
            syscall3(SYS_WRITE, fd, (long)ni.ip, (long)my_strlen(ni.ip));
            const char *nl = "\n";
            syscall3(SYS_WRITE, fd, (long)nl, 1L);
        }
    }
    syscall1(SYS_CLOSE, fd);
}

// #230 (docs/SETTINGS_CONTROL_AUDIT.md #224): "Check Updates" used to set
// about_status=1 and print "System is up to date." having contacted NOTHING.
// A real update server exists (updates.maytera.net) and userland/apps/otaupd
// already has a real, signature-verified check against it - this reuses the
// SAME manifest, the SAME server-base override file and the SAME "build "
// substring parse of get_version() that otaupd's running_build() uses, so
// there is one notion of "what build am I / what build is latest", not two.
// It deliberately does NOT verify the RSA signature (SYS_OTA_VERIFY_SIG) or
// apply anything: this is a read-only informational check with no SelfUpdate
// capability, so an unverified "latest build" number is reported as exactly
// that - information - never acted on. Blocking, user-initiated network call
// (not on the draw path), same idiom as ext_test_connection()/AI "Test" above.
#define OTA_SERVER_DEFAULT_S "http://updates.maytera.net"
static char g_update_msg[128] = {0};
static void load_ota_server_base(char *out, int cap) {
    copy_str(out, OTA_SERVER_DEFAULT_S, cap);
    int fd = sys_open("/CONFIG/OTA_SERVER.CFG", 0 /*O_RDONLY*/);
    if (fd < 0) return;
    char b[192]; long n = sys_read(fd, b, sizeof(b) - 1); sys_close(fd);
    if (n <= 0) return; b[n] = 0;
    int i = 0; while (b[i] == ' ' || b[i] == '\t') i++;
    if (b[i] == '#' || b[i] == 0) return;
    int j = 0; while (b[i] && b[i] != '\n' && b[i] != '\r' && j < cap - 1) out[j++] = b[i++];
    while (j > 0 && (out[j-1] == '/' || out[j-1] == ' ' || out[j-1] == '\t')) j--;
    out[j] = 0;
    if (out[0] == 0) copy_str(out, OTA_SERVER_DEFAULT_S, cap);
}
// Same running-build parse as otaupd's running_build(): scan for "build " in
// the live SYS_GET_VERSION string and read the digits after it.
static long settings_running_build(void) {
    char v[64]; v[0] = 0; get_version(v, sizeof(v));
    for (int i = 0; v[i]; i++) {
        if (v[i]=='b'&&v[i+1]=='u'&&v[i+2]=='i'&&v[i+3]=='l'&&v[i+4]=='d'&&v[i+5]==' ') {
            int j = i + 6; long n = 0; int any = 0;
            while (v[j] >= '0' && v[j] <= '9') { n = n * 10 + (v[j]-'0'); j++; any = 1; }
            return any ? n : -1;
        }
    }
    return -1;
}
// Tiny tolerant scan for one integer JSON field: "latest_secure_build": 1234
static long jfield_int(const char *o, long n, const char *key) {
    int kl = my_strlen(key);
    for (long i = 0; i + kl + 2 < n; i++) {
        if (o[i] != '"' || strncmp(o + i + 1, key, kl) != 0 || o[i+1+kl] != '"') continue;
        long j = i + 1 + kl + 1;
        while (j < n && (o[j] == ' ' || o[j] == ':')) j++;
        long v = 0; int any = 0;
        while (j < n && o[j] >= '0' && o[j] <= '9') { v = v*10 + (o[j]-'0'); j++; any = 1; }
        return any ? v : -1;
    }
    return -1;
}
static char g_updchk_buf[16384];
static void check_for_updates(void) {
    if (!sys_net_is_up()) {
        copy_str(g_update_msg, "Could not reach the update server: network is down.", sizeof(g_update_msg));
        about_status = 4;
        return;
    }
    char base[192]; load_ota_server_base(base, sizeof(base));
    char url[224]; int u = 0;
    for (const char *q = base; *q && u < 200; q++) url[u++] = *q;
    for (const char *q = "/security/manifest.json"; *q; q++) url[u++] = *q;
    url[u] = 0;
    unsigned int bytes = 0; int status = 0;
    int r = sys_http_fetch(url, g_updchk_buf, sizeof(g_updchk_buf) - 1, &bytes, &status);
    if (r < 0 || status != 200 || bytes == 0) {
        char m[128]; int mi = 0;
        const char *c = "Could not reach the update server (HTTP ";
        for (int k = 0; c[k]; k++) m[mi++] = c[k];
        char nb[12]; gui_itoa(status, nb, sizeof(nb));
        for (int k = 0; nb[k]; k++) m[mi++] = nb[k];
        m[mi++] = ')'; m[mi++] = '.'; m[mi] = 0;
        copy_str(g_update_msg, m, sizeof(g_update_msg));
        about_status = 4;
        return;
    }
    g_updchk_buf[bytes < sizeof(g_updchk_buf) ? bytes : sizeof(g_updchk_buf) - 1] = 0;
    long latest = jfield_int(g_updchk_buf, (long)bytes, "latest_secure_build");
    long running = settings_running_build();
    if (latest < 0 || running < 0) {
        copy_str(g_update_msg, "Update check failed: the server's response could not be read.", sizeof(g_update_msg));
        about_status = 4;
        return;
    }
    if (latest <= running) {
        char m[64]; int mi = 0;
        const char *c = "System is up to date (build ";
        for (int k = 0; c[k]; k++) m[mi++] = c[k];
        char nb[16]; gui_itoa((int)running, nb, sizeof(nb));
        for (int k = 0; nb[k]; k++) m[mi++] = nb[k];
        m[mi++] = ')'; m[mi++] = '.'; m[mi] = 0;
        copy_str(g_update_msg, m, sizeof(g_update_msg));
        about_status = 1;
    } else {
        char m[96]; int mi = 0;
        const char *c = "Update available: build ";
        for (int k = 0; c[k]; k++) m[mi++] = c[k];
        char nb[16]; gui_itoa((int)latest, nb, sizeof(nb));
        for (int k = 0; nb[k]; k++) m[mi++] = nb[k];
        const char *c2 = " (you have build ";
        for (int k = 0; c2[k]; k++) m[mi++] = c2[k];
        char rb[16]; gui_itoa((int)running, rb, sizeof(rb));
        for (int k = 0; rb[k]; k++) m[mi++] = rb[k];
        m[mi++] = ')'; m[mi++] = '.'; m[mi] = 0;
        copy_str(g_update_msg, m, sizeof(g_update_msg));
        about_status = 3;
    }
}

// =============================================================================
// MICO .ICN icon loader + alpha blitter (self-contained, mirrors files app)
// MICO format: 12-byte header ('MICO' + width u32 LE + height u32 LE), then
// width*height*4 bytes BGRA. White glyphs are tinted to the sidebar text color
// so they track selected/normal states. Cached; falls back to letter chip if
// an icon is missing.
// =============================================================================
#define MICO_DIM   64
#define MICO_CACHE 16
typedef struct {
    char     name[16];   // cache key (basename, no ".ICN")
    int      w, h;
    int      loaded;     // 1=present, -1=tried-and-missing, 0=empty slot
    uint8_t  px[MICO_DIM * MICO_DIM * 4];   // BGRA
} mico_icon_t;
static mico_icon_t g_mico[MICO_CACHE];
static int g_mico_count = 0;

static int mico_streq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static mico_icon_t *mico_get(const char *name) {
    if (!name) return NULL;   // defensive: a NULL icon name must never deref addr 0
    for (int i = 0; i < g_mico_count; i++)
        if (mico_streq(g_mico[i].name, name)) return &g_mico[i];
    if (g_mico_count >= MICO_CACHE) return NULL;
    mico_icon_t *ic = &g_mico[g_mico_count++];
    int n = 0; while (name[n] && n < 15) { ic->name[n] = name[n]; n++; } ic->name[n] = 0;
    ic->loaded = -1; ic->w = ic->h = 0;
    char path[48]; int l = 0;
    const char *p = "/ICONS/"; while (*p) path[l++] = *p++;
    for (int i = 0; name[i] && l < 40; i++) path[l++] = name[i];
    const char *e = ".ICN"; while (*e) path[l++] = *e++;
    path[l] = 0;
    int fd = sys_open(path, 0);
    if (fd < 0) return ic;
    uint8_t hdr[12];
    if (sys_read(fd, (char *)hdr, 12) != 12 ||
        hdr[0] != 'M' || hdr[1] != 'I' || hdr[2] != 'C' || hdr[3] != 'O') {
        sys_close(fd); return ic;
    }
    int w = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24);
    int h = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (hdr[11] << 24);
    if (w <= 0 || h <= 0 || w > MICO_DIM || h > MICO_DIM) { sys_close(fd); return ic; }
    int want = w * h * 4, got = 0;
    while (got < want) {
        long r = sys_read(fd, (char *)ic->px + got, want - got);
        if (r <= 0) break;
        got += (int)r;
    }
    sys_close(fd);
    if (got != want) return ic;
    ic->w = w; ic->h = h; ic->loaded = 1;
    return ic;
}

// Draw a cached icon scaled into size x size at (x,y), recoloured to tint
// (white glyph luminance used as coverage). bg is the surface color the icon
// sits on, used to approximate alpha blend (no framebuffer read-back).
// Returns 1 if drawn, 0 if not present (caller falls back).
static int draw_mico(const char *name, int x, int y, int size, uint32_t tint, uint32_t bg) {
    mico_icon_t *ic = mico_get(name);
    if (!ic || ic->loaded != 1 || size <= 0) return 0;
    int tr = (tint >> 16) & 0xFF, tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;
    int br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bb = bg & 0xFF;
    for (int dy = 0; dy < size; dy++) {
        int sy = (dy * ic->h) / size; if (sy >= ic->h) sy = ic->h - 1;
        for (int dx = 0; dx < size; dx++) {
            int sx = (dx * ic->w) / size; if (sx >= ic->w) sx = ic->w - 1;
            const uint8_t *s = &ic->px[(sy * ic->w + sx) * 4];
            int b = s[0], g = s[1], r = s[2], a = s[3];
            if (a == 0) continue;
            int cov = (r * 30 + g * 59 + b * 11) / 100;   // white glyph -> coverage
            a = (a * cov) / 255;
            if (a == 0) continue;
            int px = x + dx, py = y + dy;
            if (a >= 250) {
                gui_draw_pixel(window_handle, px, py, (tr << 16) | (tg << 8) | tb);
            } else {
                int rr = (tr  * a + br  * (255 - a)) / 255;
                int rg = (tg  * a + bgc * (255 - a)) / 255;
                int rb = (tb  * a + bb  * (255 - a)) / 255;
                gui_draw_pixel(window_handle, px, py, (rr << 16) | (rg << 8) | rb);
            }
        }
    }
    return 1;
}

// Panel index -> /ICONS basename (no extension). All exist at /ICONS/.
static const char* panel_mico[PANEL_COUNT] = {
    "POST",      // Appearance
    "MONITOR",   // Display
    "HEADPHON",  // Sound
    "RSS",       // Network
    "BLIST",     // Keyboard
    "SASS",      // Mouse
    "CLOCK",     // Date & Time
    "SMILE",     // Users
    "ANGRY",     // Privacy
    "BOOK",      // Storage
    "CODEFILE",  // Default Apps
    "LIGHT",     // About
    "INFO",      // Alerts (Notifications)
    "PRINTER",   // Devices / Printers (falls back to [R])
    "RSS",       // Bluetooth (falls back to [B]; custom glyph drawn in-panel)
    "RSS",       // Wi-Fi (custom glyph drawn in-panel)
    "RSS",       // External Services (#414). MUST exist: this array is indexed by
                 // PANEL_COUNT and a missing entry left panel_mico[PANEL_EXTSVC]
                 // NULL, which made draw_sidebar() -> draw_mico(NULL) ->
                 // mico_get(NULL) dereference address 0 and crash Settings on
                 // every launch (the window was created then the process killed,
                 // so it "never appeared"). Keep this list length == PANEL_COUNT.
    "RSS",       // Start Menu (#: uplift). Reuses the same safe fallback as the
                 // three panels above rather than risking a non-existent /ICONS
                 // basename (same lesson as the PANEL_EXTSVC crash noted above).
    "RSS"        // Dock (#745 task #67). Same safe fallback; falls back to [Z].
};

// (#168) Alerts / notifications prefs. Persisted to /CONFIG/ALERTS.CFG, which
// the compositor reads to drive toast popups (master enable, per-severity,
// toast duration in seconds, do-not-disturb).
static int alerts_enabled = 1;
static int alerts_info = 1, alerts_success = 1, alerts_warning = 1, alerts_error = 1;
static int alerts_duration = 4;
static int alerts_dnd = 0;


// =============================================================================
// Draw Sidebar
// =============================================================================

// (#291/#438) Sidebar scroll state, driven by the shared gui_scroll primitive.
// The Settings sidebar was the motivating case: 17 panels at a 38px pitch need
// 646px, which does not fit under the taskbar on a 1280x800 framebuffer, and
// with no scroll the overflow was simply unreachable.
static gui_scroll_t g_side_scroll;

// Recompute the sidebar viewport. Cheap, and called on every draw so a resize
// (EVENT_RESIZE) is picked up with no separate invalidation path.
static void sidebar_layout(void) {
    int list_y = SIDEBAR_TOP;
    int avail  = WIN_HEIGHT - SIDEBAR_FOOTER_H - SIDEBAR_GAP - SIDEBAR_TOP;
    // Floor the viewport to a whole number of rows. Combined with snap, this is
    // what guarantees a row is never left half-drawn across the viewport edge,
    // which matters because there is no clip region: a partially drawn row's
    // TEXT would spill over the title block above it.
    int list_h = (avail / PANEL_PITCH) * PANEL_PITCH;
    if (list_h < PANEL_PITCH) list_h = PANEL_PITCH;
    gui_scroll_config(&g_side_scroll, 0, list_y, SIDEBAR_WIDTH, list_h,
                      SIDEBAR_LIST_FULL, PANEL_PITCH);
    g_side_scroll.snap = 1;   // a list of fixed-height rows
}

// Screen y of panel row i in the current scroll position.
static int sidebar_row_y(int i) {
    return g_side_scroll.y + i * PANEL_PITCH - g_side_scroll.offset;
}

// Panel index at a window-local y, or -1. Shared by click and hover so the two
// can never disagree about where a row is (they previously duplicated the
// formula, which is how a scroll offset gets applied to one and not the other).
static int sidebar_hit(int local_y) {
    if (local_y < g_side_scroll.y ||
        local_y >= g_side_scroll.y + g_side_scroll.h) return -1;
    for (int i = 0; i < PANEL_COUNT; i++) {
        int y = sidebar_row_y(i);
        if (local_y >= y && local_y < y + PANEL_ROW_H) return i;
    }
    return -1;
}

// Scroll the minimum distance needed to bring the selected panel on screen.
// Called whenever the selection moves by keyboard: without this, arrowing onto a
// panel below the fold would select an invisible row (the classic "keyboard
// works but you cannot see what you selected" bug).
static void sidebar_reveal_current(void) {
    sidebar_layout();
    gui_scroll_reveal(&g_side_scroll, current_panel * PANEL_PITCH, PANEL_ROW_H);
}

static void draw_sidebar(void) {
    sidebar_layout();

    // Sidebar background
    win_draw_rect(window_handle, 0, 0, SIDEBAR_WIDTH, WIN_HEIGHT, COL_SIDEBAR_BG);

    // Title area with icon
    if (!draw_mico("MENU", 13, 9, 22, COL_SIDEBAR_TEXT, COL_SIDEBAR_BG))
        win_draw_text(window_handle, 15, 12, "[*]", COL_SIDEBAR_TEXT);
    win_draw_text(window_handle, 45, 12, "Settings", COL_SIDEBAR_TEXT);
    win_draw_rect(window_handle, 10, 38, SIDEBAR_WIDTH - 20, 1, COL_SEPARATOR);

    // Panel buttons. Only rows fully inside the viewport are drawn; the viewport
    // is a whole number of rows and the offset snaps to rows, so this clips
    // cleanly with no partial row and no spill.
    int vy0 = g_side_scroll.y;
    int vy1 = g_side_scroll.y + g_side_scroll.h;
    for (int i = 0; i < PANEL_COUNT; i++) {
        int y = sidebar_row_y(i);
        if (y < vy0 || y + PANEL_ROW_H > vy1) continue;

        // Background based on state
        uint32_t bg = COL_PANEL_NORMAL;
        if (i == current_panel) {
            bg = COL_PANEL_ACTIVE;
        } else if (i == hover_panel) {
            bg = COL_PANEL_HOVER;
        }

        focus_add(5, y, SIDEBAR_WIDTH - 10, PANEL_ROW_H, 1);
        win_draw_rect(window_handle, 5, y, SIDEBAR_WIDTH - 10, PANEL_ROW_H, bg);

        // Active indicator bar
        if (i == current_panel) {
            win_draw_rect(window_handle, 5, y, 3, PANEL_ROW_H, COL_ACCENT);
        }

        // Panel name color (also used to tint the icon so it tracks state)
        uint32_t text_color = (i == current_panel) ? COL_SIDEBAR_SEL_TEXT
                                                   : COL_SIDEBAR_TEXT;

        // Icon: real MICO glyph tinted to the row text color; fall back to the
        // letter chip if the icon file is missing so nothing breaks.
        if (!draw_mico(panel_mico[i], 14, y + 7, 20, text_color, bg)) {
            win_draw_text(window_handle, 15, y + 9, panel_icons[i], text_color);
        }

        win_draw_text(window_handle, 48, y + 9, panel_names[i], text_color);
    }

    // Scrollbar: themed, and drawn only when the list actually overflows, so on
    // a screen tall enough to show all panels no gutter is spent.
    // The left nav has its OWN surface (taskbar_bg since #745), so the thumb
    // is contrasted against that, not against the content panel it is not on.
    gui_scroll_draw_on(window_handle, &g_side_scroll, COL_SIDEBAR_BG);

    // Version at bottom: queried live from the running kernel (SYS_GET_VERSION)
    // rather than baked in. This line used to read a hardcoded "v1.8.0" while
    // the OS shipped 1.95.0, because a literal here goes stale the moment the
    // kernel moves and nothing rebuilds Settings.
    {
        char vb[48]; vb[0] = 0;
        char vline[56]; int n = 0;
        vline[n++] = 'v';
        if (get_version(vb, sizeof(vb)) > 0 && vb[0]) {
            for (int k = 0; vb[k] && n < (int)sizeof(vline) - 1; k++) vline[n++] = vb[k];
        } else {
            // Deliberately NOT a baked-in version literal on the failure path:
            // a wrong-but-plausible number is worse than an obvious "unknown",
            // and a literal is what went stale here in the first place.
            const char *f = "?";
            for (int k = 0; f[k] && n < (int)sizeof(vline) - 1; k++) vline[n++] = f[k];
        }
        vline[n] = 0;
        win_draw_rect(window_handle, 10, WIN_HEIGHT - SIDEBAR_FOOTER_H,
                      SIDEBAR_WIDTH - 20, 1, COL_SEPARATOR);
        win_draw_text_small(window_handle, 15, WIN_HEIGHT - SIDEBAR_FOOTER_H + 8,
                            vline, COL_SIDEBAR_MUTED);
    }

    // Separator line between sidebar and content
    win_draw_rect(window_handle, SIDEBAR_WIDTH, 0, 1, WIN_HEIGHT, COL_SEPARATOR);
}

// =============================================================================
// Panel: Appearance
// =============================================================================

// --- Settings persistence: the Settings app owns prefs that the compositor
//     profile does not (timezone, clock/date formats, accent, cursor, pointer).
//     Persisted to SETTINGS.CFG so they survive reboots AND kernel updates. ---
static char *sv_putint(char *p, char key, int v) {
    *p++ = key; *p++ = '=';
    char t[12]; int n = 0, neg = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (!v) t[n++] = '0';
    while (v) { t[n++] = '0' + v % 10; v /= 10; }
    if (neg) *p++ = '-';
    while (n) *p++ = t[--n];
    *p++ = '\n';
    return p;
}
// =============================================================================
// Tool contract (#233) - THE table, not a document about the table.
//
// SETTINGS_ITEMS[] below is the single description of this app's contract
// surface, and it is load-bearing three ways at once:
//
//   1. settings_save()  walks it to emit /CONFIG/SETTINGS.CFG;
//   2. settings_load()  walks it to parse the same file back;
//   3. settings_autosave() derives its change signature from it
//      (contract_hash), so a newly persisted row is watched the moment it
//      exists - #231 removed exactly the opposite fault from the widget
//      serializer, where a hand-maintained hash had silently stopped
//      watching three settings.
//
// So a row cannot be "in the contract but not really persisted", or
// "persisted but not in the contract": there is one list. The seven
// hand-written sv_putint() lines this replaces were the second description;
// tools/pref-reader-lint/READERS.tsv (#230) now lints against THIS table
// rather than against those lines, so it is a gate on the contract instead
// of a third description of the same keys.
//
// COMPLETENESS is the harder invariant, and here it is GATED, not structural.
// Settings is 8809 lines of mostly bespoke drawing; unlike Calculator, whose
// every key already lives in one const table that draw and hit-test share, most
// of this file's panels still compute controls inline. tools/contract-lint
// enumerates every *_geom_t / *_layout_t field - which after #227 is the
// app's own shared draw/hit-test structure, so a control that is drawn AND
// clickable in a converted panel MUST have a field there - and FAILS the build
// when a field has no contract row and no justified allowlist entry.
//
// That gate is strictly weaker than Calculator's structural guarantee, and it
// is labelled as weaker in docs/CONTRACT_API.md section 6. The way to make it
// structural is to finish the #227 conversion: a panel converted to a shared
// geometry pass gains hit-box-drift immunity AND contract coverage in one
// move. Seven panels are still on independent arithmetic and are therefore
// invisible to both.
// =============================================================================
#include "../../libc/contract.h"

// Forward declarations for handlers defined later in this file. The contract
// calls the SAME functions the click handlers call; it never reimplements one.
// dir_clear_files()/storage_scan() are defined above; no forward declaration needed.

// ---- setters that mirror what the UI's own click handler does ---------------

// Date & Time "Set time automatically". The panel's handler runs an NTP sync
// on enable (main.c, PANEL_DATETIME case); doing the same here is what makes
// a contract write equivalent to a click rather than merely similar.
static int ct_set_auto_time(int v) {
    auto_time = v;
    if (auto_time) {
        long r = ntp_sync();
        ntp_status = (r == 0) ? 1 : -1;
    }
    return 0;
}
static int ct_get_auto_time(void) { return auto_time ? 1 : 0; }

// Timezone. The stored setting is /CONFIG/TZ.CFG (libc/tz.c), NOT a key in
// SETTINGS.CFG - #50 deleted the second copy. timezone_idx is the picker row,
// and update_timezone_offset() is the same commit path the dropdown uses.
static int ct_get_tz(void) { return tz_index(); }
static int ct_set_tz(int v) {
    if (v < 0 || v >= tz_count()) return -1;
    timezone_idx = v;
    update_timezone_offset();
    return 0;
}

// Theme. current_theme indexes g_th[] (file-based themes, #565); apply_theme()
// is the same function the Appearance dropdown calls, and it reaches
// gui_theme_activate() -> SYS_SET_THEME, which the compositor edge-detects.
static int ct_get_theme_idx(void) { return current_theme; }
static int ct_set_theme_idx(int v) {
    th_init();
    if (v < 0 || v >= g_th_count) return -1;
    // Set the SAME variable the dropdown writes, then call the SAME commit
    // callback it registers (dropdown_open(..., &current_theme,
    // theme_dd_changed) in the PANEL_APPEARANCE click case). Calling
    // apply_theme(v) directly, as this did first, applied the colours but left
    // current_theme stale, so the contract getter kept reporting the old
    // index. #233's read-back-after-write check caught that on its first live
    // run and reported note=clamped-or-ignored-by-app instead of echoing the
    // value it had been asked for, which is the whole reason the check exists.
    current_theme = v;
    theme_dd_changed();
    return 0;
}

static int ct_get_use24(void)  { return use_24hour ? 1 : 0; }
static int ct_set_use24(int v) { use_24hour = v ? 1 : 0; return 0; }

// ---- #235: the rows libc/aiclient.c used to describe by itself --------------
//
// These seven controls were reachable ONLY from AI chat, through a hardcoded
// if-else chain in aiclient.c's settings_apply() that called the kernel setters
// directly and never went near this app. That chain was one of the four
// partial, mutually disagreeing descriptions of the Settings surface
// (contract.h "WHAT WAS HERE BEFORE"). It now delegates to
// /APPS/SETTINGS --contract set, so these rows are what it reaches, and there
// is one description again.
//
// None of them carries a cfgkey: they are LIVE KERNEL state, not SETTINGS.CFG
// keys. settings_load()/settings_save() correctly do not touch them, and the
// app re-reads them from the kernel at startup (get_volume(), get_win_opacity(),
// get_display_fx()), which is why a persisted copy here would be the drift.
// Each setter writes the SAME variable and calls the SAME kernel setter the
// panel's own click handler does - the ct_set_theme_idx lesson above, applied
// before it could bite a second time.

static int ct_get_font_size(void)  { return font_size; }
static int ct_set_font_size(int v) { font_size = v; set_font_size(v); return 0; }

// Percent OPAQUE, matching the slider's own sense (transparency_level), and the
// same 0-255 conversion the PANEL_APPEARANCE click case performs.
static int ct_get_transparency(void)  { return transparency_level; }
static int ct_set_transparency(int v) {
    transparency_level = v;
    set_win_opacity(v * 255 / 100);
    return 0;
}

static int ct_get_volume(void)  { return get_volume(); }   // the kernel mixer is the truth
static int ct_set_volume(int v) { master_volume = v; set_volume(v); return 0; }

static int ct_get_mute(void)  { return sound_muted ? 1 : 0; }
static int ct_set_mute(int v) { sound_muted = v ? true : false; set_mute(v ? 1 : 0); return 0; }

static int ct_get_brightness(void)  { return brightness; }
static int ct_set_brightness(int v) { brightness = v; apply_display_fx(); return 0; }

static int ct_get_night_light(void)  { return night_light ? 1 : 0; }
static int ct_set_night_light(int v) { night_light = v ? true : false; apply_display_fx(); return 0; }

static int ct_get_night_strength(void)  { return night_light_strength; }
static int ct_set_night_strength(int v) { night_light_strength = v; apply_display_fx(); return 0; }

// Theme NAME, read and written. #235: the name list lives HERE, in g_th_names[],
// which th_init() enumerates from /THEMES/INDEX.TXT and the Appearance dropdown
// draws from. It is deliberately not a table anywhere else: libc/aiclient.c used
// to carry its own ("0=Retro,1=Dark,2=Light,4=Classic,5=Ocean,9=Nord") and that
// copy was doubly wrong - it went stale the moment the App Store installed a
// theme, and its numbers were KERNEL theme ids while current_theme indexes
// g_th[], so the two descriptions did not even share an index space.
static int ct_get_theme_name(char *out, int ocap) {
    th_init();
    int i = current_theme;
    if (i < 0 || i >= g_th_count) i = 0;
    strlcpy(out, g_th_names[i] ? g_th_names[i] : "?", (size_t)ocap);
    return 0;
}

static int ct_name_eq_ci(const char *a, const char *b) {
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (!x) return 1;
    }
}

// ---- actions: each one calls the click handler's own body -------------------

// Select a theme by NAME. Present so a caller that knows "Nord" and not "3" can
// still drive the control, WITHOUT anybody outside this file holding a name
// table. On a miss it answers with the names that actually exist, so the caller
// corrects itself from the live list rather than from a stale copy.
static int ct_act_theme_by_name(const ct_item_t *it, int argc, char **argv,
                                char *out, int ocap) {
    (void)it;
    th_init();
    if (argc < 1 || !argv[0] || !argv[0][0]) {
        strlcpy(out, "needs-a-theme-name", (size_t)ocap);
        return -1;
    }
    for (int i = 0; i < g_th_count; i++) {
        if (g_th_names[i] && ct_name_eq_ci(g_th_names[i], argv[0])) {
            current_theme = i;
            theme_dd_changed();            // the dropdown's own commit callback
            snprintf(out, ocap, "%s index=%d", g_th_names[i], i);
            return 0;
        }
    }
    int o = 0;
    o += snprintf(out + o, ocap - o, "no-such-theme available=");
    for (int i = 0; i < g_th_count && o < ocap - 2; i++)
        o += snprintf(out + o, ocap - o, i ? "|%s" : "%s", g_th_names[i]);
    return -1;
}

static int ct_act_clear(const ct_item_t *it, int argc, char **argv,
                        char *out, int ocap) {
    (void)argc; (void)argv;
    dir_clear_files((const char *)it->ctx);
    storage_scan();
    strlcpy(out, "cleared", (size_t)ocap);
    return 0;
}

static int ct_act_ntp(const ct_item_t *it, int argc, char **argv,
                      char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    long r = ntp_sync();
    ntp_status = (r == 0) ? 1 : -1;
    strlcpy(out, r == 0 ? "synced" : "sync-failed", (size_t)ocap);
    return r == 0 ? 0 : -1;
}

// ---- the table --------------------------------------------------------------
//
// cfgkey != 0 means the row IS a /CONFIG/SETTINGS.CFG key. Those seven rows and
// tools/pref-reader-lint/READERS.tsv must agree exactly, both ways, or
// repo-guard fails: a key with no proven reader, or a claimed reader for a key
// nobody writes, are equally dishonest.
// --- #238 packet filter -----------------------------------------------------
// Every one of these goes through fw_pull()/fw_push(), i.e. through
// SYS_NET_FW, i.e. through the same path the panel's own toggles take. There
// is no second way to change the firewall from this app, which is why a
// contract row here cannot describe something the UI does differently.
static int ct_fw_get_enabled(void) { fw_pull(); return firewall_enabled ? 1 : 0; }
static int ct_fw_set_enabled(int v) {
    fw_pull();
    firewall_enabled = v ? true : false;
    fw_push();
    return (fw_last_rc == 0) ? 0 : -1;
}
static int ct_fw_get_pin(void)  { fw_pull(); return fw_pol_in; }
static int ct_fw_set_pin(int v) {
    if (v < 0 || v > 1) return -1;
    fw_pull(); fw_pol_in = v; fw_push();
    return (fw_last_rc == 0) ? 0 : -1;
}
static int ct_fw_get_pout(void)  { fw_pull(); return fw_pol_out; }
static int ct_fw_set_pout(int v) {
    if (v < 0 || v > 1) return -1;
    fw_pull(); fw_pol_out = v; fw_push();
    return (fw_last_rc == 0) ? 0 : -1;
}

static int ct_fw_get_rules(char *out, int ocap) {
    fw_pull();
    int n = 0;
    out[0] = 0;
    if (fw_rule_count == 0) {
        snprintf(out, (size_t)ocap, "(none; defaults decide everything)");
        return 0;
    }
    for (int i = 0; i < fw_rule_count && n < ocap - 1; i++) {
        n += snprintf(out + n, (size_t)(ocap - n), "%s%s/%s/%s/%u",
                      i ? "," : "",
                      fw_rules[i].dir == FW_DIR_IN ? "in" : "out",
                      fw_rules[i].action == FW_ACT_DENY ? "deny" : "allow",
                      fw_rules[i].proto == FW_PROTO_UDP ? "udp" : "tcp",
                      (unsigned)fw_rules[i].port);
    }
    return 0;
}

// The counters, as one line. drop_in/drop_out are the numbers that prove the
// filter is on the packet path; cyc_max is its worst-case cost per packet in
// TSC cycles, which is what #238 is required to report rather than assert.
static int ct_fw_get_stats(char *out, int ocap) {
    fw_pull();
    unsigned long long avg = fw_stats.calls ? (fw_stats.cyc_tot / fw_stats.calls) : 0;
    snprintf(out, (size_t)ocap,
             "calls=%llu ct_hit=%llu new_in=%llu new_out=%llu "
             "pass_in=%llu pass_out=%llu drop_in=%llu drop_out=%llu "
             "exempt=%llu unfiltered=%llu malformed=%llu frag=%llu "
             "ct_used=%u ct_evict=%llu cyc_avg=%llu cyc_max=%llu",
             (unsigned long long)fw_stats.calls,
             (unsigned long long)fw_stats.ct_hit,
             (unsigned long long)fw_stats.new_in,
             (unsigned long long)fw_stats.new_out,
             (unsigned long long)fw_stats.pass_in,
             (unsigned long long)fw_stats.pass_out,
             (unsigned long long)fw_stats.drop_in,
             (unsigned long long)fw_stats.drop_out,
             (unsigned long long)fw_stats.exempt,
             (unsigned long long)fw_stats.unfiltered,
             (unsigned long long)fw_stats.malformed,
             (unsigned long long)fw_stats.frag,
             (unsigned)fw_stats.ct_used,
             (unsigned long long)fw_stats.ct_evict,
             avg, (unsigned long long)fw_stats.cyc_max);
    return 0;
}

static int fw_word(const char *w, const char *a, const char *b) {
    if (strcmp(w, a) == 0) return 0;
    if (strcmp(w, b) == 0) return 1;
    return -1;
}

// add_rule <in|out> <allow|deny> <tcp|udp> <port>
//
// Every field is REJECTED rather than coerced if it is not one of the two
// words or a port in 1..65535. A firewall rule that was silently reinterpreted
// is the worst possible outcome of a typo, so nothing here guesses.
static int ct_fw_act_add(const ct_item_t *it, int argc, char **argv,
                         char *out, int ocap) {
    (void)it;
    if (argc < 4) {
        snprintf(out, (size_t)ocap, "usage: add_rule <in|out> <allow|deny> <tcp|udp> <port>");
        return -1;
    }
    int dir   = fw_word(argv[0], "in", "out");
    int act   = fw_word(argv[1], "allow", "deny");
    int proto = fw_word(argv[2], "tcp", "udp");
    int port  = atoi(argv[3]);
    if (dir < 0 || act < 0 || proto < 0 || port < 1 || port > 65535) {
        snprintf(out, (size_t)ocap,
                 "rejected: dir must be in|out, action allow|deny, proto tcp|udp, port 1..65535");
        return -1;
    }
    fw_pull();
    if (fw_rule_count >= MAX_FW_RULES) {
        snprintf(out, (size_t)ocap, "rejected: rule table is full (%d)", MAX_FW_RULES);
        return -1;
    }
    fw_add(dir, act, proto, port);
    fw_push();
    if (fw_last_rc != 0) {
        snprintf(out, (size_t)ocap, "kernel refused the ruleset (rc=%d); "
                 "previous policy still in force", fw_last_rc);
        return -1;
    }
    snprintf(out, (size_t)ocap, "installed; %d rule(s) now in force", fw_rule_count);
    return 0;
}

static int ct_fw_act_clear(const ct_item_t *it, int argc, char **argv,
                           char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    fw_pull();
    fw_rule_count = 0;
    fw_push();
    if (fw_last_rc != 0) {
        snprintf(out, (size_t)ocap, "kernel refused (rc=%d)", fw_last_rc);
        return -1;
    }
    snprintf(out, (size_t)ocap,
             "all explicit rules removed; the two default policies now decide "
             "every connection");
    return 0;
}

static int ct_fw_act_reload(const ct_item_t *it, int argc, char **argv,
                            char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    fw_reload_from_disk();
    if (fw_last_rc != 0) {
        snprintf(out, (size_t)ocap,
                 "/CONFIG/FWRULES.CFG carried nothing usable (rc=%d); the "
                 "previous policy is still in force", fw_last_rc);
        return -1;
    }
    snprintf(out, (size_t)ocap, "reloaded; %d rule(s) in force", fw_rule_count);
    return 0;
}

static const ct_item_t SETTINGS_ITEMS[] = {
// --- Firewall (#238): the packet filter, which is real as of this ticket ----
//
// GUARDED, not SAFE. Weakening a packet filter is a security decision, and
// docs/CONTRACT_API.md section 4 is explicit that this gate is in-process and
// is not a defence against a hostile Ring-3 binary. THE REAL BOUNDARY IS THE
// SYSCALL: SYS_NET_FW refuses every mutating op from a non-root euid. What the
// GUARDED class adds here is a consent prompt and a per-call audit entry, and
// it means a headless caller must have been granted "net.firewall" beforehand
// rather than being able to mint its own authority.
//
// The two read-only rows are SAFE: knowing the policy is not a capability, and
// a filter whose state cannot be read is a filter nobody can verify.
{ "firewall.enabled", CT_BOOL, CT_RW, CT_GUARDED, 0, 0, 1, 0,
  0, ct_fw_get_enabled, ct_fw_set_enabled, 0, 0, 0, "system.network.firewall",
  "Turn the kernel packet filter on or off; off means every packet passes" },
{ "firewall.policy_in", CT_ENUM, CT_RW, CT_GUARDED, 0, 0, 1, "Allow|Deny",
  0, ct_fw_get_pin, ct_fw_set_pin, 0, 0, 0, "system.network.firewall",
  "Default action for UNSOLICITED inbound connections; replies always pass" },
{ "firewall.policy_out", CT_ENUM, CT_RW, CT_GUARDED, 0, 0, 1, "Allow|Deny",
  0, ct_fw_get_pout, ct_fw_set_pout, 0, 0, 0, "system.network.firewall",
  "Default action for connections this machine opens to somewhere else" },
{ "firewall.rules", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
  0, 0, 0, 0, ct_fw_get_rules, 0, 0,
  "The live rule list read back from the kernel, dir/action/proto/port" },
{ "firewall.stats", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
  0, 0, 0, 0, ct_fw_get_stats, 0, 0,
  "Live filter counters: packets dropped, flows tracked, cost in TSC cycles" },
{ "firewall.add_rule", CT_ACTION, CT_WRITE, CT_GUARDED, 0, 0, 0, 0,
  0, 0, 0, ct_fw_act_add, 0, 0, "system.network.firewall",
  "Add a rule: add_rule <in|out> <allow|deny> <tcp|udp> <port 1..65535>" },
{ "firewall.clear_rules", CT_ACTION, CT_WRITE, CT_GUARDED, 0, 0, 0, 0,
  0, 0, 0, ct_fw_act_clear, 0, 0, "system.network.firewall",
  "Remove every explicit rule, leaving only the two default policies" },
{ "firewall.reload", CT_ACTION, CT_WRITE, CT_GUARDED, 0, 0, 0, 0,
  0, 0, 0, ct_fw_act_reload, 0, 0, "system.network.firewall",
  "Re-read /CONFIG/FWRULES.CFG; reports how many lines it had to ignore" },


// --- persisted preferences (the SETTINGS.CFG keys) --------------------------
{ "clock.use_24hour", CT_BOOL, CT_RW, CT_SAFE, 'h', 0, 1, 0,
  0, ct_get_use24, ct_set_use24, 0, 0, 0, 0,
  "24-hour clock; read live by the taskbar clock via libc settingscfg_use24h()" },
{ "appearance.accent_color", CT_ENUM, CT_RW, CT_SAFE, 'a', 0, 7,
  "Blue|Green|Orange|Purple|Red|Teal|Pink|Amber",
  &accent_color_idx, 0, 0, 0, 0, 0, 0,
  "Accent colour index; recolors this app's own buttons (Settings-scoped, #230)" },
{ "pointer.cursor_style", CT_ENUM, CT_RW, CT_SAFE, 'c', 0, 2, "Light|Dark|Glow",
  &cursor_theme, 0, 0, 0, 0, 0, 0,
  "Pointer glyph; the compositor's cursor_render() reads it every frame" },
{ "pointer.speed", CT_INT, CT_RW, CT_SAFE, 'p', 0, 100, 0,
  &pointer_speed, 0, 0, 0, 0, 0, 0,
  "Mouse sensitivity 0-100; scales every packet in kernel mouse_scale_delta()" },
{ "pointer.double_click_speed", CT_INT, CT_RW, CT_SAFE, 'k', 0, 100, 0,
  &double_click_speed, 0, 0, 0, 0, 0, 0,
  "Double-click speed 0-100; the compositor threshold via settingscfg_dblclick_ms()" },
{ "screensaver.type", CT_INT, CT_RW, CT_SAFE, 's', 0, 40, 0,
  &screensaver_idx, 0, 0, 0, 0, 0, 0,
  "Screensaver index; the compositor calls screensaver_set_type() on change" },
{ "screensaver.delay_minutes", CT_INT, CT_RW, CT_SAFE, 'z', 1, 120, 0,
  &screensaver_delay_min, 0, 0, 0, 0, 0, 0,
  "Idle minutes before the screensaver arms; read live by get_ss_delay()" },

// --- Date & Time panel: all five datetime_geom_t fields ---------------------
{ "datetime.auto_time", CT_BOOL, CT_RW, CT_SAFE, 0, 0, 1, 0,
  0, ct_get_auto_time, ct_set_auto_time, 0, 0, 0, 0,
  "Set time automatically; enabling runs an NTP sync, as the panel's toggle does" },
{ "datetime.timezone", CT_INT, CT_RW, CT_SAFE, 0, 0, 63, 0,
  0, ct_get_tz, ct_set_tz, 0, 0, 0, 0,
  "Timezone row in the shared libc/tz.c ZONES list; commits to /CONFIG/TZ.CFG" },
{ "datetime.week_start", CT_ENUM, CT_RW, CT_SAFE, 0, 0, 1, "Sunday|Monday",
  &first_day_of_week, 0, 0, 0, 0, 0, 0,
  "First day of the week shown in calendars" },
{ "datetime.sync_now", CT_ACTION, CT_WRITE, CT_SAFE, 0, 0, 0, 0,
  0, 0, 0, ct_act_ntp, 0, 0, 0,
  "Run an NTP time sync now; the same call the auto-time toggle makes" },
{ "datetime.set_clock", CT_ACTION, CT_WRITE, CT_DENIED, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0,
  "Set the hardware RTC (the Set Date & Time modal). DENIED: see the note below" },

// --- Storage panel: all four storage_geom_t fields --------------------------
// GUARDED, not SAFE. These delete real files and cannot be undone, and
// docs/CONTRACT_ARCHITECTURE.md section 7 makes irreversibility the primary
// axis of risk. A capability token or user consent is required.
{ "storage.clear_thumbnails", CT_ACTION, CT_WRITE, CT_GUARDED, 0, 0, 0, 0,
  0, 0, 0, ct_act_clear, 0, CACHE_DIR_THUMBS, "fs.write",
  "Delete every cached thumbnail; irreversible, so it is capability-gated" },
{ "storage.clear_app_cache", CT_ACTION, CT_WRITE, CT_GUARDED, 0, 0, 0, 0,
  0, 0, 0, ct_act_clear, 0, CACHE_DIR_APPS, "fs.write",
  "Delete the application cache; irreversible, so it is capability-gated" },
{ "storage.clear_system_cache", CT_ACTION, CT_WRITE, CT_GUARDED, 0, 0, 0, 0,
  0, 0, 0, ct_act_clear, 0, CACHE_DIR_SYSTEM, "fs.write",
  "Delete the system cache; irreversible, so it is capability-gated" },
{ "storage.empty_trash", CT_ACTION, CT_WRITE, CT_GUARDED, 0, 0, 0, 0,
  0, 0, 0, ct_act_clear, 0, CACHE_DIR_TRASH, "fs.write",
  "Empty the recycle bin permanently; irreversible, so it is capability-gated" },

// --- Privacy panel: all six privacy_geom_t fields ---------------------------
{ "privacy.screen_lock", CT_BOOL, CT_RW, CT_SAFE, 0, 0, 1, 0,
  &screen_lock_enabled, 0, 0, 0, 0, 0, 0,
  "Enable the idle screen lock; persisted to /CONFIG/PRIVACY.CFG" },
{ "privacy.lock_timeout_min", CT_INT, CT_RW, CT_SAFE, 0, 0, 60, 0,
  &lock_timeout, 0, 0, 0, 0, 0, 0,
  "Idle minutes before locking; 0 means never" },
{ "privacy.require_password_wake", CT_BOOL, CT_RW, CT_GUARDED, 0, 0, 1, 0,
  &require_password_wake, 0, 0, 0, 0, 0, "system.settings.write",
  "Require a password on wake. GUARDED: turning it off weakens login" },
{ "privacy.location_services", CT_BOOL, CT_RW, CT_SAFE, 0, 0, 1, 0,
  &location_services, 0, 0, 0, 0, 0, 0,
  "Allow apps to request location; persisted to /CONFIG/PRIVACY.CFG" },
{ "privacy.send_diagnostics", CT_BOOL, CT_RW, CT_SAFE, 0, 0, 1, 0,
  &diagnostics_enabled, 0, 0, 0, 0, 0, 0,
  "Send anonymous diagnostics; persisted to /CONFIG/PRIVACY.CFG" },
{ "privacy.crash_reports", CT_BOOL, CT_RW, CT_SAFE, 0, 0, 1, 0,
  &crash_reports, 0, 0, 0, 0, 0, 0,
  "Send crash reports; persisted to /CONFIG/PRIVACY.CFG" },

// --- Appearance: the rows that have a real live setter ----------------------
{ "appearance.theme", CT_INT, CT_RW, CT_SAFE, 0, 0, 31, 0,
  0, ct_get_theme_idx, ct_set_theme_idx, 0, 0, 0, 0,
  "Theme index into the file-based theme list; applies live via SYS_SET_THEME" },
{ "appearance.theme_name", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
  0, 0, 0, 0, ct_get_theme_name, 0, 0,
  "Name of the active theme, from the same g_th_names[] the dropdown draws" },
{ "appearance.set_theme_by_name", CT_ACTION, CT_WRITE, CT_SAFE, 0, 0, 0, 0,
  0, 0, 0, ct_act_theme_by_name, 0, 0, 0,
  "Select a theme by name; answers with the live list of names on a miss" },
{ "appearance.font_size", CT_ENUM, CT_RW, CT_SAFE, 0, 0, 3, "Small|Medium|Large|XL",
  0, ct_get_font_size, ct_set_font_size, 0, 0, 0, 0,
  "UI font size; calls set_font_size(), as the Appearance dropdown does" },
{ "appearance.window_transparency", CT_INT, CT_RW, CT_SAFE, 0, 5, 100, 0,
  0, ct_get_transparency, ct_set_transparency, 0, 0, 0, 0,
  "Window opacity percent (100 = opaque); the same slider the panel draws" },

// --- Sound / Display: live kernel state, no SETTINGS.CFG key ----------------
{ "sound.master_volume", CT_INT, CT_RW, CT_SAFE, 0, 0, 100, 0,
  0, ct_get_volume, ct_set_volume, 0, 0, 0, 0,
  "Master output volume 0-100; reads and writes the real kernel mixer" },
{ "sound.mute", CT_BOOL, CT_RW, CT_SAFE, 0, 0, 1, 0,
  0, ct_get_mute, ct_set_mute, 0, 0, 0, 0,
  "Mute the master output; the same set_mute() the Mute button calls" },
{ "display.brightness", CT_INT, CT_RW, CT_SAFE, 0, 0, 100, 0,
  0, ct_get_brightness, ct_set_brightness, 0, 0, 0, 0,
  "Display brightness 0-100; committed through apply_display_fx()" },
{ "display.night_light", CT_BOOL, CT_RW, CT_SAFE, 0, 0, 1, 0,
  0, ct_get_night_light, ct_set_night_light, 0, 0, 0, 0,
  "Enable the warm night-light tint; committed through apply_display_fx()" },
{ "display.night_light_strength", CT_INT, CT_RW, CT_SAFE, 0, 0, 100, 0,
  0, ct_get_night_strength, ct_set_night_strength, 0, 0, 0, 0,
  "Night-light strength 0-100; only visible while night_light is on" },

// --- Users: declared and refused --------------------------------------------
// Present on purpose. Enumeration must be honest that the surface exists and
// is refused; an API that silently omitted it would let a caller conclude
// Settings cannot change a password, which is false.
{ "users.change_password", CT_ACTION, CT_WRITE, CT_DENIED, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0,
  "Change an account password. DENIED through this API in every case" },
};

#define SETTINGS_ITEM_COUNT ((int)(sizeof(SETTINGS_ITEMS)/sizeof(SETTINGS_ITEMS[0])))

static void settings_save(void);
static void settings_load(void);

static const ct_contract_t SETTINGS_CONTRACT = {
    "settings", "Settings",
    "Persisted rows (cfgkey) ARE settings_save()/settings_load(); there is no "
    "second key list. Coverage of the app's full control surface is PARTIAL "
    "and gated by tools/contract-lint, not structural - see "
    "docs/CONTRACT_API.md section 6 for the exact number.",
    SETTINGS_ITEMS, SETTINGS_ITEM_COUNT,
    0,                 // no projection: this app's controls are not one table
    settings_load,
    settings_save
};

static void settings_save(void) {
    char buf[512]; char *p = buf;
    // #233: DERIVED from SETTINGS_ITEMS[]. There is no second list of keys.
    // Adding a row with a cfgkey persists it here, exposes it through the
    // contract API and brings it under pref-reader-lint, all in one edit;
    // deleting the row removes all three. The seven hand-written sv_putint()
    // lines this replaces were the drift surface.
    // #50: 't' (timezone) is deliberately not written - the zone lives in
    // /CONFIG/TZ.CFG and a second copy here is the divergence #50 removed. It
    // is still READ below, once, to migrate an existing install.
    for (int i = 0; i < SETTINGS_ITEM_COUNT; i++) {
        const ct_item_t *it = &SETTINGS_ITEMS[i];
        if (!it->cfgkey) continue;
        p = sv_putint(p, it->cfgkey, contract_get(it));
    }
    // #743: was unlink-then-open on a RELATIVE path, so every desktop
    // preference was deleted by a failed open and otherwise saved to wherever
    // cwd pointed.
    int fd = userconf_open_write("SETTINGS.CFG");
    if (userconf_finish_write(fd, buf, (unsigned long)(p - buf)) != 0)
        save_failed("SETTINGS.CFG (desktop preferences)");
}
static void settings_load(void) {
    // #743: per-user path, falling back to the legacy relative name (#683).
    int fd = userconf_open_read("SETTINGS.CFG", "SETTINGS.CFG");
    if (fd < 0) return;
    static char b[512];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    int i = 0;
    while (b[i]) {
        char key = b[i];
        int val = 0, neg = 0;
        if (b[i+1] == '=') {
            i += 2;
            if (b[i] == '-') { neg = 1; i++; }
            while (b[i] >= '0' && b[i] <= '9') { val = val * 10 + (b[i] - '0'); i++; }
            if (neg) val = -val;
            // #233: DERIVED from SETTINGS_ITEMS[]. A key the table persists
            // is a key the table parses, by construction.
            ct_item_t cit;
            if (key == 't') {
                legacy_tz_idx = val;          // #50: migration only
            } else if (contract_by_key(&SETTINGS_CONTRACT, key, &cit)) {
                contract_put(&cit, val);
            }
            // Keys from an older build that the table no longer carries
            // (#230 deleted 14 of them) fall through and are ignored, which
            // is correct: they name controls that no longer exist.
        }
        while (b[i] && b[i] != '\n') i++;
        if (b[i] == '\n') i++;
    }
}
static void settings_autosave(void) {
    static int last = -1;
    // #233: the change signature is DERIVED from SETTINGS_ITEMS[]
    // (contract_hash walks every persisted row), replacing a hand-written
    // weighted sum that had to be edited every time a key was added. #231
    // removed the identical fault from the widget serializer, where the
    // hand-maintained hash had silently stopped watching three settings.
    int h = contract_hash(&SETTINGS_CONTRACT);
    if (last == -1) { last = h; return; }
    if (h != last) { last = h; settings_save(); }
}

// (#168) Alerts prefs persist to /CONFIG/ALERTS.CFG as "key=value" lines so the
// compositor can read them without any kernel key/value plumbing.
static char *a_putkv(char *p, const char *key, int val) {
    while (*key) *p++ = *key++;
    *p++ = '=';
    char nb[16]; gui_itoa(val, nb, sizeof(nb));
    char *q = nb; while (*q) *p++ = *q++;
    *p++ = '\n';
    return p;
}
static void alerts_save(void) {
    char buf[256]; char *p = buf;
    p = a_putkv(p, "enabled", alerts_enabled);
    p = a_putkv(p, "sev_info", alerts_info);
    p = a_putkv(p, "sev_success", alerts_success);
    p = a_putkv(p, "sev_warning", alerts_warning);
    p = a_putkv(p, "sev_error", alerts_error);
    p = a_putkv(p, "duration", alerts_duration);
    p = a_putkv(p, "dnd", alerts_dnd);
    // #743: was unlink-then-open. See save_failed() above.
    if (userconf_write_all("/CONFIG/ALERTS.CFG", buf, (unsigned long)(p - buf)) != 0)
        save_failed("ALERTS.CFG");
}
static int a_kv(const char *b, const char *key, int def) {
    int kl = 0; while (key[kl]) kl++;
    for (const char *p = b; *p; ) {
        const char *ls = p; int i = 0;
        while (key[i] && ls[i] == key[i]) i++;
        if (i == kl && ls[kl] == '=') {
            const char *v = ls + kl + 1; int neg = 0, val = 0, any = 0;
            if (*v == '-') { neg = 1; v++; }
            while (*v >= '0' && *v <= '9') { val = val*10 + (*v - '0'); v++; any = 1; }
            return any ? (neg ? -val : val) : def;
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return def;
}
static void alerts_load(void) {
    int fd = sys_open("/CONFIG/ALERTS.CFG", 0);
    if (fd < 0) return;
    static char b[256];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    alerts_enabled = a_kv(b, "enabled", 1);
    alerts_info    = a_kv(b, "sev_info", 1);
    alerts_success = a_kv(b, "sev_success", 1);
    alerts_warning = a_kv(b, "sev_warning", 1);
    alerts_error   = a_kv(b, "sev_error", 1);
    alerts_duration = a_kv(b, "duration", 4);
    if (alerts_duration < 1) alerts_duration = 1;
    if (alerts_duration > 20) alerts_duration = 20;
    alerts_dnd = a_kv(b, "dnd", 0);
}

// (#382 pass2) Privacy toggles persist to /CONFIG/PRIVACY.CFG as real "key=value"
// settings instead of being per-launch no-ops. There is no userland capability
// syscall to enforce these, so they are honest persisted preferences (the panel
// labels say so); the screen-lock values are readable by the lock subsystem.
static void privacy_save(void) {
    char buf[256]; char *p = buf;
    p = a_putkv(p, "screen_lock", screen_lock_enabled);
    p = a_putkv(p, "lock_timeout", lock_timeout);
    p = a_putkv(p, "require_pw_wake", require_password_wake);
    p = a_putkv(p, "location_services", location_services);
    p = a_putkv(p, "diagnostics", diagnostics_enabled);
    p = a_putkv(p, "crash_reports", crash_reports);
    // #743: was unlink-then-open. A failed open reset every privacy toggle,
    // including lock timeout and "require password on wake", to its default.
    if (userconf_write_all("/CONFIG/PRIVACY.CFG", buf, (unsigned long)(p - buf)) != 0)
        save_failed("PRIVACY.CFG");
}
static void privacy_load(void) {
    int fd = sys_open("/CONFIG/PRIVACY.CFG", 0);
    if (fd < 0) return;
    static char b[256];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    screen_lock_enabled   = a_kv(b, "screen_lock", 1) ? true : false;
    lock_timeout          = a_kv(b, "lock_timeout", 5);
    require_password_wake = a_kv(b, "require_pw_wake", 1) ? true : false;
    location_services     = a_kv(b, "location_services", 0) ? true : false;
    diagnostics_enabled   = a_kv(b, "diagnostics", 1) ? true : false;
    crash_reports         = a_kv(b, "crash_reports", 1) ? true : false;
}

// (#: Start Menu uplift) Persisted to /CONFIG/STARTMENU.PREFS as "key=value"
// lines (same idiom as ALERTS.CFG/PRIVACY.CFG above), polled live by the
// compositor's startmenu_prefs_poll() (throttled, same cadence idea as
// main.c's dock_style_poll) so changes apply without reopening the menu.
// Favorites/Recents themselves are NOT here: those are compositor-owned state
// (/CONFIG/STARTMENU.CFG, written on pin/unpin/launch) that this panel never
// touches, so the two files can never race or clobber each other.
static int sm_view          = 0;   // 0 = Categories accordion, 1 = All Apps
static bool sm_show_fav      = true;
static bool sm_show_recent   = true;
static int  sm_recent_count  = 5;   // 1-10
static bool sm_focus_search  = true;
static int  sm_width         = 300; // 220-420
static int  sm_icon_size     = 20;  // 14-28

static const char *const SM_VIEW_OPTS[] = { "Categories", "All Apps" };
#define SM_VIEW_OPTS_COUNT ARRAY_COUNT(SM_VIEW_OPTS)

static void sm_save(void) {
    char buf[256]; char *p = buf;
    p = a_putkv(p, "view", sm_view);
    p = a_putkv(p, "show_fav", sm_show_fav ? 1 : 0);
    p = a_putkv(p, "show_recent", sm_show_recent ? 1 : 0);
    p = a_putkv(p, "recent_count", sm_recent_count);
    p = a_putkv(p, "focus_search", sm_focus_search ? 1 : 0);
    p = a_putkv(p, "width", sm_width);
    p = a_putkv(p, "icon_size", sm_icon_size);
    // #743: was unlink-then-open. See save_failed() above.
    if (userconf_write_all("/CONFIG/STARTMENU.PREFS", buf, (unsigned long)(p - buf)) != 0)
        save_failed("STARTMENU.PREFS");
}
static void sm_load(void) {
    int fd = sys_open("/CONFIG/STARTMENU.PREFS", 0);
    if (fd < 0) return;
    static char b[256];
    long n = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    sm_view = a_kv(b, "view", 0);
    if (sm_view < 0 || sm_view > 1) sm_view = 0;
    sm_show_fav = a_kv(b, "show_fav", 1) ? true : false;
    sm_show_recent = a_kv(b, "show_recent", 1) ? true : false;
    sm_recent_count = a_kv(b, "recent_count", 5);
    if (sm_recent_count < 1) sm_recent_count = 1;
    if (sm_recent_count > 10) sm_recent_count = 10;
    sm_focus_search = a_kv(b, "focus_search", 1) ? true : false;
    sm_width = a_kv(b, "width", 300);
    if (sm_width < 220) sm_width = 220;
    if (sm_width > 420) sm_width = 420;
    sm_icon_size = a_kv(b, "icon_size", 20);
    if (sm_icon_size < 14) sm_icon_size = 14;
    if (sm_icon_size > 28) sm_icon_size = 28;
}
static void sm_view_changed(void) { sm_save(); }

// --- Wallpaper preview on a little monitor (macOS-style), top-right of the
//     Appearance tab. Samples the actual BMP into a small thumbnail (cached). ---
#define WPT_W 156
#define WPT_H 96
static uint32_t s_wp_thumb[WPT_W * WPT_H];
static int      s_wp_thumb_idx = -2;
static uint8_t  s_wp_row[8192];

// Disk thumbnail cache, keyed by the wallpaper's OWN filename rather than its
// wp_enumerate() index. wallpaper-freeze fix: the previous index-keyed "/TWP<idx>.DAT" scheme
// depended on the filesystem readdir order matching whatever order the cache
// was generated in, which is exactly the fragile coupling #517 removed from
// the wallpaper name/file arrays; a filename key cannot drift. Stored under
// CACHE_DIR_THUMBS as "<STEM>.DAT" = 'WTH1' + w(u16 LE) + h(u16 LE) + w*h u32
// (0x00RRGGBB). Pre-generated by tools/wallpapers/gen-wallpapers.sh (area-
// averaged) and deployed, so the picker never decodes a full-size BMP. Falls
// back to a runtime BMP sample if the cache is missing, and saves the result
// so the next view of that wallpaper is instant too (self-healing for any
// wallpaper added after the image shipped).
static void wp_cache_path(char *out, const char *fn) {
    int n = 0; const char *d = CACHE_DIR_THUMBS "/"; while (*d) out[n++] = *d++;
    const char *s = fn; if (*s == '/') s++;
    while (*s && *s != '.' && n < 55) out[n++] = *s++;   // stem only, drop extension
    const char *e = ".DAT"; while (*e) out[n++] = *e++;
    out[n] = 0;
}
static int wp_load_cache(const char *fn) {
    if (!fn) return 0;
    char path[64]; wp_cache_path(path, fn);
    int fd = sys_open(path, 0);
    if (fd < 0) return 0;
    uint8_t hd[8];
    if (sys_read(fd, hd, 8) != 8 || hd[0]!='W'||hd[1]!='T'||hd[2]!='H'||hd[3]!='1') { sys_close(fd); return 0; }
    int w = hd[4] | (hd[5] << 8), h = hd[6] | (hd[7] << 8);
    if (w != WPT_W || h != WPT_H) { sys_close(fd); return 0; }
    int need = WPT_W * WPT_H * 4;
    int got = sys_read(fd, (uint8_t *)s_wp_thumb, need);
    sys_close(fd);
    return got == need;
}
static void wp_save_cache(const char *fn) {
    if (!fn) return;
    sys_mkdir(CACHE_DIR_THUMBS, 0755);   // harmless if it already exists
    char path[64]; wp_cache_path(path, fn);
    int fd = sys_open(path, 0x41 | 0x200);   // O_WRONLY|O_CREAT|O_TRUNC
    if (fd < 0) return;
    uint8_t hd[8] = {'W','T','H','1',
                      (uint8_t)(WPT_W & 0xFF), (uint8_t)(WPT_W >> 8),
                      (uint8_t)(WPT_H & 0xFF), (uint8_t)(WPT_H >> 8)};
    // #743: header and pixels are one logical record, so they are written as
    // one call: two unchecked writes could leave an 8-byte header with no
    // pixels behind it, which the loader would read as a valid-looking thumb.
    static uint8_t rec[8 + WPT_W * WPT_H * 4];
    for (int i = 0; i < 8; i++) rec[i] = hd[i];
    const uint8_t *px = (const uint8_t *)s_wp_thumb;
    for (int i = 0; i < WPT_W * WPT_H * 4; i++) rec[8 + i] = px[i];
    if (userconf_finish_write(fd, rec, sizeof(rec)) != 0) {
        // A thumbnail is a CACHE: the wallpaper picker regenerates it on the
        // next open. Deleting the partial file is the right recovery, because a
        // truncated thumb would be decoded as if it were complete.
        sys_unlink(path);
        save_failed("wallpaper thumbnail cache");
    }
}

// #517: wallpaper BMP filenames now come from the shared enumeration g_wp[] (see
// wp_init() above), indexed identically to the display names and to the compositor.

static uint32_t wp_rd_u32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint16_t wp_rd_u16(const uint8_t *p){ return (uint16_t)(p[0]|(p[1]<<8)); }

static void wp_fill_fallback(int idx) {
    for (int cy = 0; cy < WPT_H; cy++) {
        int t = cy * 200 / WPT_H;
        for (int cx = 0; cx < WPT_W; cx++) {
            uint32_t c;
            if (idx == 12)      c = 0x00181818;                                   // Dark Mode
            else if (idx == 11) c = 0x00204060;                                   // Classic
            else if (idx == 13) c = (uint32_t)(((90 + t/3) << 16) | (45 << 8) | 25); // Retro
            else                c = (uint32_t)((25 << 16) | ((55 + t/4) << 8) | (110 + t/3)); // blue
            s_wp_thumb[cy*WPT_W + cx] = c;
        }
    }
}

static void wp_build_thumb(int idx) {
    if (idx == s_wp_thumb_idx) return;
    s_wp_thumb_idx = idx;
    const char *fn = (idx >= 0 && idx < g_wp_count && g_wp[idx].file[0]) ? g_wp[idx].file : 0;
    if (wp_load_cache(fn)) return;           // fast pre-built disk thumbnail
    if (!fn) { wp_fill_fallback(idx); return; }
    int fd = sys_open(fn, 0);
    if (fd < 0) { wp_fill_fallback(idx); return; }
    uint8_t hdr[54];
    if (sys_read(fd, hdr, 54) != 54 || hdr[0] != 'B' || hdr[1] != 'M') {
        sys_close(fd); wp_fill_fallback(idx); return;
    }
    uint32_t off = wp_rd_u32(hdr + 10);
    int32_t  w   = (int32_t)wp_rd_u32(hdr + 18);
    int32_t  h   = (int32_t)wp_rd_u32(hdr + 22);
    int      bpp = wp_rd_u16(hdr + 28);
    int topdown = 0;
    if (h < 0) { h = -h; topdown = 1; }
    if (w <= 0 || h <= 0 || bpp < 24) { sys_close(fd); wp_fill_fallback(idx); return; }
    int bypp = bpp / 8;
    int stride = (w * bypp + 3) & ~3;
    if (stride > (int)sizeof(s_wp_row)) { sys_close(fd); wp_fill_fallback(idx); return; }
    int ok = 1;
    for (int cy = 0; cy < WPT_H; cy++) {
        int sy = cy * h / WPT_H;
        int frow = topdown ? sy : (h - 1 - sy);
        sys_seek(fd, (long)off + (long)frow * stride, 0 /*SEEK_SET*/);
        if (sys_read(fd, s_wp_row, stride) != stride) { wp_fill_fallback(idx); ok = 0; break; }
        for (int cx = 0; cx < WPT_W; cx++) {
            int sx = cx * w / WPT_W;
            uint8_t *p = s_wp_row + sx * bypp;
            s_wp_thumb[cy*WPT_W + cx] = ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
        }
    }
    sys_close(fd);
    // Self-heal: a wallpaper decoded at runtime (no pre-built cache shipped
    // for it) gets its thumbnail saved, so every later view is instant.
    if (ok) wp_save_cache(fn);
}

// --- Wallpaper picker: a dropdown (consistent with every other setting on
//     this tab) plus a single preview thumbnail of the SELECTED wallpaper
//     only. Lives in the right column of the Appearance tab.
//
//     HISTORY: #117 replaced the original dropdown with a thumbnail
//     GRID (one cell per wallpaper). With ~63 wallpapers and no pre-built
//     disk cache actually shipping on the image, the grid's first draw built
//     EVERY cell's thumbnail by decoding its full-size (up to 3MB) BMP
//     synchronously on the UI thread - a ~60s freeze the first time Settings
//     opened. A dropdown only ever needs the thumbnail for the one selected
//     item, so opening Settings can never block on wallpaper count again,
//     independent of whether the disk cache (wp_load_cache) is populated. ---
#define WP_DD_X      545      // dropdown/preview origin x (right column)
#define WP_DD_Y      120      // section header y
#define WP_DD_W      260

static const char *g_wp_names[WP_MAX_ENTRIES];
static void wp_names_init(void) {
    static int done = 0; if (done) return; done = 1;
    for (int i = 0; i < g_wp_count; i++) g_wp_names[i] = g_wp[i].name;
}

static uint32_t s_wp_preview[WPT_W * WPT_H];   // BGRA (alpha forced opaque) for win_draw_image
static int      s_wp_preview_idx = -2;

static void draw_wallpaper_picker(void) {
    wp_names_init();
    int wp_y0 = WP_DD_Y - g_content_scroll.offset;   // (#227) content area now scrolls
    draw_subsection(WP_DD_X, wp_y0, "Wallpaper");
    int dy = wp_y0 + 25;
    const char *nm = (wallpaper_idx >= 0 && wallpaper_idx < g_wp_count) ? g_wp[wallpaper_idx].name : "";
    draw_dropdown_n(WP_DD_X, dy, WP_DD_W, nm, g_dd_open && g_dd_sel == &wallpaper_idx, g_wp_count);

    wp_build_thumb(wallpaper_idx);             // fills s_wp_thumb; instant if cached
    if (s_wp_preview_idx != wallpaper_idx) {
        s_wp_preview_idx = wallpaper_idx;
        for (int i = 0; i < WPT_W * WPT_H; i++) s_wp_preview[i] = s_wp_thumb[i] | 0xFF000000u;
    }
    int py = dy + 40;
    win_draw_image(window_handle, WP_DD_X, py, WPT_W, WPT_H, s_wp_preview);
    gui_draw_rect_outline(window_handle, WP_DD_X, py, WPT_W, WPT_H, gui_darken(COL_CONTENT_BG, 40));
}

static void draw_appearance_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    char buf[32];

    draw_wallpaper_picker();           // dropdown + single preview, right column

    draw_section_header(x, y, "Appearance");
    y += 40;

    // Theme selection with preview
    draw_subsection(x, y, "Theme");
    y += 25;

    // Scrollable dropdown (scales to many themes), not a row of buttons.
    draw_dropdown_n(x, y, 220,
                  (current_theme >= 0 && current_theme < g_th_count) ? g_th_names[current_theme] : "",
                  g_dd_open && g_dd_sel == &current_theme, g_th_count);
    // #745: live preview of the highlighted theme, drawn by the SAME shared
    // primitive the first-boot wizard uses (libc gui_theme_win_preview), so the
    // two surfaces cannot show two different pictures of one theme. A dropdown
    // naming a theme told the user nothing about what it looks like; this is
    // the top-right corner of a real window in that theme, at 1:1, from its own
    // tokens. It sits between the dropdown (ends x+220) and the wallpaper
    // column (WP_DD_X 545), and the 1px surround is what keeps a dark theme's
    // near-black frame from vanishing into the panel.
    if (current_theme >= 0 && current_theme < g_th_count) {
        // Sits in the gap between the theme dropdown (ends x+220) and the
        // wallpaper column (WP_DD_X 545, content-local). 100px is the crop the
        // largest shipped title-bar metric needs for its four buttons; a first
        // pass used 112 and the on-device screendump showed it running under
        // the "Wallpaper" label.
        int pvx = x + 234, pvy = y - 2, pvw = 100, pvh = 44;
        // Contract the surround against the panel it is ACTUALLY drawn on.
        gui_draw_rect_outline(window_handle, pvx - 1, pvy - 1, pvw + 2, pvh + 2,
                              gui_ensure_contrast(theme_color(THEME_COLOR_WINDOW_BORDER),
                                                  COL_CONTENT_BG, GUI_FLOOR_NONTEXT));
        gui_theme_win_preview(window_handle, pvx, pvy, pvw, pvh,
                              g_th[current_theme].index, COL_CONTENT_BG);
    }
    y += 50;

    // Accent color picker
    draw_subsection(x, y, "Accent Color");
    y += 25;

    for (int i = 0; i < NUM_ACCENT_COLORS; i++) {
        draw_color_box(x + i * 42, y, ACCENT_COLORS[i], i == accent_color_idx);
    }
    // #230 (docs/SETTINGS_CONTROL_AUDIT.md #224): this DOES apply live and DOES
    // persist (COL_ACCENT is re-derived from accent_color_idx by apply_theme(),
    // used throughout this file) - but only within THIS process. Say so, so
    // "accent color" is not read as a system-wide setting it is not; making it
    // one would mean threading an accent override through the kernel theme
    // engine (kernel/gui/themes.c) that every app queries, which is real
    // future work, not a one-line fix. Fits inside the row's existing 55px
    // budget below, so PANEL_APPEARANCE's independently-computed font_y
    // (handle_content_click(), allowlist.tsv DEBT row) does not move.
    draw_hint_ic(x, y + 32, "CMINUS", theme_color(THEME_COLOR_MUTED),
                 "Applies to this Settings window only; other apps keep the theme's own accent.");
    y += 55;

    // Font settings
    draw_subsection(x, y, "Text");
    y += 25;

    draw_label(x, y, "Font Size");
    draw_dropdown_n(x + 120, y - 3, 160, FONT_SIZE_OPTS[OPT_CLAMP(font_size, FONT_SIZE_OPTS)],
                  g_dd_open && g_dd_sel == &font_size, FONT_SIZE_OPTS_COUNT);
    y += 40;

    draw_label(x, y, "Icon Size");
    draw_dropdown_n(x + 120, y - 3, 160, ICON_SIZE_OPTS[OPT_CLAMP(icon_size, ICON_SIZE_OPTS)],
                  g_dd_open && g_dd_sel == &icon_size, ICON_SIZE_OPTS_COUNT);
    y += 40;

    // UI Font (#351). The button shows the live selection and opens the SHARED
    // picker; "Install..." is deliberately absent here because fonts are
    // installed from Files, where the user is already looking at the file.
    {
        char fb[96];
        snprintf(fb, sizeof(fb), "%s %s, %dpt",
                 g_uifont.family[0] ? g_uifont.family : "Default",
                 g_uifont.style[0] ? g_uifont.style : "Regular",
                 g_uifont.size > 0 ? g_uifont.size : 14);
        draw_label(x, y, "UI Font");
        draw_button_small(x + 120, y - 3, 230, fb, false);
        draw_button_small(x + 360, y - 3, 90, "Choose...", true);
    }
    y += 50;

    // Screensaver
    draw_subsection(x, y, "Screensaver");
    y += 25;
    draw_dropdown_n(x, y, 160, SS_OPTS[OPT_CLAMP(screensaver_idx, SS_OPTS)],
                  g_dd_open && g_dd_sel == &screensaver_idx, SS_OPTS_COUNT);
    /* (#115) no Test button when the screensaver is Off (idx 0) */
    if (screensaver_idx != 0)
        draw_button_small(x + 200, y + 1, 90, "Test", false);
    y += 34;

    /* (#115) Activation-delay slider (custom steps), hidden when Off; the
       vertical space is reserved either way so controls below stay put. */
    if (screensaver_idx != 0) {
        char db[16];
        ss_delay_label(db);
        draw_label(x, y + 2, "Activate after");
        draw_slider(x + 120, y, 170, ss_delay_index(), SS_DELAY_NSTEPS - 1, COL_SLIDER_FILL);
        win_draw_text(window_handle, x + 300, y, db, COL_TEXT_SECONDARY);
    }
    y += 36;

    // #745: the Cursor picker MOVED to the Mouse panel (draw_mouse_panel).
    // It is deliberately not duplicated here: one control, one home.

    // #745 task #67 "dockpanel": Dock Style and Dock glass opacity MOVED to
    // the new Settings > Dock panel (draw_dock_panel), which also owns dock
    // CONTENTS (pinned favourites - previously nowhere in Settings at all,
    // see that panel's own header comment). Same precedent as the Cursor
    // picker above: one control, one home. dock_dd_changed()/
    // dock_opacity_write() are unchanged and are now called from there.

    // Wallpaper selection is the dropdown in the right column
    // (draw_wallpaper_picker above); its click handling lives with the
    // other right-column dropdowns, not here.

    // Window transparency (#112): global default opacity, applied live + persisted
    draw_subsection(x, y, "Transparency");
    y += 25;
    draw_label(x, y + 4, "Window");
    {
        char tb[8];
        gui_itoa(transparency_level, tb, sizeof(tb));
        int len = my_strlen(tb);
        tb[len++] = '%'; tb[len] = 0;
        // (#117 fixup) match the screensaver-delay slider geometry (width 170,
        // value label at x+300) so the "%" label no longer overlaps the
        // wallpaper thumbnail grid in the right column.
        draw_slider(x + 120, y, 170, transparency_level, 100, COL_SLIDER_FILL);
        win_draw_text(window_handle, x + 300, y, tb, COL_TEXT_SECONDARY);
    }
    y += 45;
    g_content_bottom_y = y;   // (#227)
}

// =============================================================================
// Panel: Display
// =============================================================================

// ===========================================================================
// ONE DESCRIPTION OF THE DISPLAY PANEL'S VERTICAL LAYOUT.
//
// Every offset below used to be written out twice: once as a running `y +=`
// in draw_display_panel(), and once as a hand-added constant in the
// PANEL_DISPLAY click handler ("int bright_y = base_y + 299;"). That is the
// same duplicated-geometry shape as the kernel's eleven copies of the
// title-bar button x, and it had already failed twice - the file's own
// comments record the rotation offset being re-measured after Scale content
// "had grown past it and nobody had re-measured".
//
// IT WAS STALE AGAIN WHEN THIS WAS WRITTEN. Walking draw_display_panel()'s
// own arithmetic puts the Brightness slider at +249:
//     40 header + 25 Resolution + 30 res row + 16 hint + 24 hint
//   + 25 Rotation + 34 dropdown + 30 hint + 25 Brightness  =  249
// while the handler tested +299. The slider was drawn in one place and
// clicked for 50px lower down: a control that looks live and is not, which is
// exactly ticket class #208. Night Light's toggle and strength are computed
// RELATIVE to that constant, so both were displaced with it.
//
// So the offsets move here, computed once, and BOTH sides read them. Adding a
// section can no longer silently break a control below it, which matters
// immediately: this change adds one.
// ===========================================================================
typedef struct {
    int rotation_dd;     // y of the Rotation dropdown (draws at y-3)
    int uiscale_dd;      // y of the UI Scale dropdown
    int brightness;      // y of the Brightness slider
    int nightlight;      // y of the Night Light toggle
    int nl_strength;     // y of the Night Light strength slider
    int card;            // y of the Display Information card
} display_layout_t;

static display_layout_t display_layout(void) {
    display_layout_t L;
    int y = 0;
    y += 40;                       // section header
    y += 25;                       // "Resolution" subsection
    y += 30;                       // resolution + colour depth row
    y += 16; y += 24;              // two hint lines
    y += 25;                       // "Rotation" subsection
    L.rotation_dd = y - 3;         // the draw_dropdown_n(y-3) convention
    y += 34;
    y += 30;                       // rotation hint
    y += 25;                       // "UI Scale" subsection
    L.uiscale_dd = y - 3;
    y += 34;
    // The hint block is four lines, plus a fifth ONLY when the scale is pinned
    // by the boot-partition file. display_layout() has to know that, or every
    // control below it moves by 16px in the draw and not in the hit test - the
    // exact fault this function was written to end.
    y += 16; y += 16; y += 16; y += 22;   // four hint lines
    if (ui_scale_src() == UI_SRC_ESP) y += 16;
    y += 5;
    y += 25;                       // "Brightness" subsection
    L.brightness = y;
    y += 45;
    y += 25;                       // "Night Light" subsection
    L.nightlight = y;
    y += 35;
    L.nl_strength = y;             // only present while night_light is on
    if (night_light) y += 35 + 40;
    y += 5;
    L.card = y;
    return L;
}

// The UI scale steps this machine can actually carry. The list is BUILT from
// the kernel's own cap (ui_scale_max()) rather than being a fixed array,
// because a step the display cannot carry is a step that would be clamped on
// selection - and a control that silently does something other than what it
// says is worse than one that does not offer the choice. On a 1366x768 panel
// the list is just "100%"; on 4K it runs to 300%.
#define UISCALE_MAX_OPTS 9
static const char *g_uiscale_opts[UISCALE_MAX_OPTS];
static char        g_uiscale_optbuf[UISCALE_MAX_OPTS][12];
static int         g_uiscale_pcts[UISCALE_MAX_OPTS];
static int         g_uiscale_count = 0;
static int         g_uiscale_idx = 0;

static void uiscale_opts_build(void) {
    int maxp = ui_scale_max();
    if (maxp < 100) maxp = 100;
    int live = ui_scale_pct();
    g_uiscale_count = 0;
    for (int p = 100; p <= maxp && g_uiscale_count < UISCALE_MAX_OPTS; p += 25) {
        char *b = g_uiscale_optbuf[g_uiscale_count];
        gui_itoa(p, b, 12);
        int n = my_strlen(b);
        b[n++] = '%'; b[n] = 0;
        if (p == 100) { b[n++] = ' '; b[n++] = '('; b[n++] = '1'; b[n++] = 'x'; b[n++] = ')'; b[n] = 0; }
        g_uiscale_opts[g_uiscale_count] = b;
        g_uiscale_pcts[g_uiscale_count] = p;
        if (p == live) g_uiscale_idx = g_uiscale_count;
        g_uiscale_count++;
    }
    if (g_uiscale_count == 0) {
        copy_str(g_uiscale_optbuf[0], "100% (1x)", 12);
        g_uiscale_opts[0] = g_uiscale_optbuf[0];
        g_uiscale_pcts[0] = 100;
        g_uiscale_count = 1;
        g_uiscale_idx = 0;
    }
}

// Applies LIVE and persists. Live matters: the whole point is that the owner
// can try 125, 150 and 175 and LOOK at them. A scale setting that needs a
// reboot to evaluate is a setting nobody tunes.
//
// REPORTS WHAT WAS ADOPTED, NOT WHAT WAS ASKED. ui_scale_set() clamps to what
// the display can carry and returns the value actually in force; showing the
// requested value instead would be a control that lies about its own state.
static void uiscale_dd_changed(void) {
    if (g_uiscale_idx < 0 || g_uiscale_idx >= g_uiscale_count) return;
    int got = ui_scale_set(g_uiscale_pcts[g_uiscale_idx]);
    (void)ui_scale_save();
    for (int i = 0; i < g_uiscale_count; i++)
        if (g_uiscale_pcts[i] == got) g_uiscale_idx = i;
}

static void draw_display_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    char buf[32];

    draw_section_header(x, y, "Display");
    y += 40;

    // Resolution and refresh rate
    draw_subsection(x, y, "Resolution");
    y += 25;

    // The framebuffer mode (resolution + colour depth) is established by the
    // UEFI GOP before ExitBootServices and is fixed for the running session.
    // These are read-only real values from SYS_FB_INFO, not selectable fakes.
    {
        fb_info_t fi;
        char resbuf[32]; char depthbuf[16];
        int have_fi = (fb_info(&fi) == 0);
        if (have_fi) {
            char a[12], b[12];
            // THE REAL PANEL GEOMETRY, NOT THIS APP'S LOGICAL CANVAS.
            // fb_info() answers a scale-transparent app in LOGICAL pixels on
            // purpose - that is the coordinate system it lays out in - so at
            // 150% on a 1920x1080 display it says 1280x720. Correct for
            // layout, and a lie to read on a settings page whose whole job is
            // to tell the user what their hardware is doing.
            int pw = 0, ph = 0;
            ui_scale_fb_phys(&pw, &ph);
            if (pw <= 0 || ph <= 0) { pw = (int)fi.width; ph = (int)fi.height; }
            gui_itoa(pw, a, sizeof(a));
            gui_itoa(ph, b, sizeof(b));
            int k = 0;
            for (int i = 0; a[i]; i++) resbuf[k++] = a[i];
            resbuf[k++] = ' '; resbuf[k++] = 'x'; resbuf[k++] = ' ';
            for (int i = 0; b[i]; i++) resbuf[k++] = b[i];
            resbuf[k] = 0;
            char c[8]; gui_itoa((int)fi.bpp, c, sizeof(c));
            depthbuf[0] = 0; hw_append(depthbuf, sizeof(depthbuf), c);
            hw_append(depthbuf, sizeof(depthbuf), "-bit");
        } else {
            resbuf[0] = '?'; resbuf[1] = 0;
            copy_str(depthbuf, "?", sizeof(depthbuf));
        }
        // Read-only value fields (bevel box, no dropdown chevron).
        draw_label(x, y + 4, "Resolution");
        win_draw_rect(window_handle, x + 120, y, 160, 28, COL_INPUT_BG);
        gui_draw_rect_outline(window_handle, x + 120, y, 160, 28, COL_INPUT_BORDER);
        win_draw_text(window_handle, x + 130, y + 6, resbuf, COL_TEXT_PRIMARY);

        draw_label(x + 310, y + 4, "Color Depth");
        win_draw_rect(window_handle, x + 410, y, 90, 28, COL_INPUT_BG);
        gui_draw_rect_outline(window_handle, x + 410, y, 90, 28, COL_INPUT_BORDER);
        win_draw_text(window_handle, x + 420, y + 6, depthbuf, COL_TEXT_PRIMARY);
    }
    y += 30;
    draw_hint(x, y, "Display mode is set by UEFI firmware (GOP) at boot; it cannot");
    y += 16;
    draw_hint(x, y, "be changed while running. Refresh rate is firmware-controlled.");
    y += 24;

    // Rotation (#745 local 102): for panels physically mounted rotated in
    // hardware (e.g. the GPD MicroPC, whose panel scans out portrait) - the
    // same fix Linux users on that class of device make with an fbcon/xrandr
    // transform. Reboot-to-apply: the kernel decides fb_rotation once, in
    // fb_init(), before any userland exists (so the boot splash rotates too,
    // not just the desktop) - see kernel/video/framebuffer.c. There is no
    // live re-layout of a running desktop's window positions/taskbar/back
    // buffer size, so this control persists a CHOICE for next boot rather
    // than pretending to apply immediately and doing nothing.
    display_layout_t L = display_layout();
    int y0 = PADDING - g_content_scroll.offset;

    draw_subsection(x, y, "Rotation");
    y += 25;
    draw_label(x, y + 4, "Orientation");
    draw_dropdown_n(x + 120, y0 + L.rotation_dd, 160,
                  ROTATION_OPTS[OPT_CLAMP(rotation_idx, ROTATION_OPTS)],
                  g_dd_open && g_dd_sel == &rotation_idx, ROTATION_OPTS_COUNT);
    y += 34;
    if (rotation_idx != rotation_active) {
        draw_hint_ic(x, y, "CMINUS", COL_WARNING,
                     "Applies after restart - the current session stays as it is.");
    } else {
        draw_hint(x, y, "For a panel mounted rotated in hardware (e.g. GPD MicroPC).");
    }
    y += 30;

    // ---------------------------------------------------------------------
    // UI SCALE.
    //
    // #230 deleted a "Scale" SLIDER from this exact spot, and was right to:
    // docs/SETTINGS_CONTROL_AUDIT.md #224 found it had no click handler at
    // all and nothing consumed its value. This is not that control coming
    // back. It is a dropdown wired to a factor that reaches TTF type sizes,
    // widget metrics, window chrome, app content and hit-testing, and it
    // applies live. The steps offered are built from what this display can
    // actually carry, so the control cannot offer a choice it would clamp.
    // ---------------------------------------------------------------------
    uiscale_opts_build();
    draw_subsection(x, y, "UI Scale");
    y += 25;
    draw_label(x, y + 4, "Interface size");
    draw_dropdown_n(x + 120, y0 + L.uiscale_dd, 160,
                  g_uiscale_opts[OPT_CLAMP(g_uiscale_idx, g_uiscale_opts)],
                  g_dd_open && g_dd_sel == &g_uiscale_idx, g_uiscale_count);
    y += 34;
    {
        // SAY WHERE THE VALUE CAME FROM. A user who can see "the machine chose
        // this" as distinct from "I chose this" can tell a setting they can
        // trust from one they are fighting.
        int src = ui_scale_src();
        int autop = ui_scale_auto();
        int lap = ui_scale_laptop();
        char ab[16]; gui_itoa(autop, ab, sizeof(ab));
        int an = my_strlen(ab); ab[an++] = '%'; ab[an] = 0;
        char line[96];
        line[0] = 0;
        hw_append(line, sizeof(line),
                  src == UI_SRC_ESP    ? "PINNED by UISCALE.TXT on the boot disk. " :
                  src == UI_SRC_USER   ? "Chosen here. " :
                  src == UI_SRC_CONFIG ? "From /CONFIG/DISPLAY.CFG. " :
                  src == UI_SRC_AUTO   ? "Detected automatically. " : "Default. ");
        hw_append(line, sizeof(line), "This display suggests ");
        hw_append(line, sizeof(line), ab);
        hw_append(line, sizeof(line), ".");
        draw_hint(x, y, line);
        y += 16;
        // THE PIN IS SAID OUT LOUD. A change made here still applies to the
        // running session, but the file wins at the next boot, and a control
        // whose effect quietly evaporates overnight is worse than one that
        // explains why. The file exists for the case where the UI is too small
        // to operate, so it has to outrank a saved preference.
        if (src == UI_SRC_ESP) {
            draw_hint_ic(x, y, "CMINUS", COL_WARNING,
                "Delete UISCALE.TXT from the boot partition to choose here.");
            y += 16;
        }
        draw_hint(x, y, lap == 1
            ? "A battery was detected, so this is treated as a laptop panel."
            : (lap == 0
               ? "No battery: treated as a desktop panel at about 96 PPI."
               : "The firmware could not be asked about a battery."));
        y += 16;
        draw_hint(x, y, "Panel size cannot be read, so a 15in and a 22in 1080p screen");
        y += 16;
        draw_hint(x, y, "look identical here. Change this if the guess looks wrong.");
        y += 22;
    }

    // #230: the Color Temp and Gamma sliders were deleted below for the same
    // reason the old Scale slider was - no click handler, nothing consuming
    // the value.

    y += 5;

    // Brightness
    draw_subsection(x, y, "Brightness");
    y += 25;

    gui_itoa(brightness, buf, sizeof(buf));
    int len = my_strlen(buf);
    buf[len++] = '%'; buf[len] = 0;
    draw_slider(x, y0 + L.brightness, 350, brightness, 100, COL_WARNING);
    win_draw_text(window_handle, x + 370, y0 + L.brightness, buf, COL_TEXT_SECONDARY);
    y += 45;

    // Night light
    draw_subsection(x, y, "Night Light");
    y += 25;

    draw_toggle_labeled(x, y0 + L.nightlight, 300, "Enable Night Light", night_light);
    y += 35;

    if (night_light) {
        draw_label(x + 20, y, "Strength");
        gui_itoa(night_light_strength, buf, sizeof(buf));
        len = my_strlen(buf);
        buf[len++] = '%'; buf[len] = 0;
        // (#704) INTENTIONALLY NOT themed: warm-amber is the universal Night
        // Light / Night Shift affordance color across desktop OSes, meant to
        // read as "warmth" regardless of the active theme.
        draw_slider(x + 120, y0 + L.nl_strength, 200, night_light_strength, 100, 0x00FF9933);
        win_draw_text(window_handle, x + 335, y, buf, COL_TEXT_SECONDARY);
        y += 35;

        draw_label(x + 20, y, "Schedule");
        gui_itoa(night_light_start_hour, buf, sizeof(buf));
        len = my_strlen(buf);
        buf[len++] = ':'; buf[len++] = '0'; buf[len++] = '0'; buf[len] = 0;
        win_draw_text(window_handle, x + 120, y, buf, COL_TEXT_PRIMARY);
        win_draw_text(window_handle, x + 180, y, "to", COL_TEXT_SECONDARY);
        gui_itoa(night_light_end_hour, buf, sizeof(buf));
        len = my_strlen(buf);
        buf[len++] = ':'; buf[len++] = '0'; buf[len++] = '0'; buf[len] = 0;
        win_draw_text(window_handle, x + 210, y, buf, COL_TEXT_PRIMARY);
        y += 40;
    }

    // #230: Color Temp and Gamma R/G/B were deleted here for the same reason
    // as Scale above - docs/SETTINGS_CONTROL_AUDIT.md #224 found no click
    // handler existed for any of them, and no compositor/GPU LUT hook
    // consumes them. (#382 pass2) Honest: Brightness and Night Light ARE
    // applied live (SYS_SET_DISPLAY_FX); this build has no LUT hook for the
    // rest, so rather than leave sliders that could never move (Scale/Color
    // Temp/Gamma) or that moved but did nothing, they are gone until a LUT
    // hook exists to back them.
    y += 5;

    // Display info card - real adapter (PCI class 0x03) + real framebuffer (#382)
    hwinfo_load();
    {
        fb_info_t fi; int have_fi = (fb_info(&fi) == 0);

        // PCI vendor:device id, e.g. "1AF4:1050"
        char idbuf[16];
        {
            static const char hx[] = "0123456789ABCDEF";
            idbuf[0] = hx[(g_gpu_vid >> 12) & 0xF]; idbuf[1] = hx[(g_gpu_vid >> 8) & 0xF];
            idbuf[2] = hx[(g_gpu_vid >> 4) & 0xF];  idbuf[3] = hx[g_gpu_vid & 0xF];
            idbuf[4] = ':';
            idbuf[5] = hx[(g_gpu_did >> 12) & 0xF]; idbuf[6] = hx[(g_gpu_did >> 8) & 0xF];
            idbuf[7] = hx[(g_gpu_did >> 4) & 0xF];  idbuf[8] = hx[g_gpu_did & 0xF];
            idbuf[9] = 0;
        }

        // Real colour depth from the framebuffer, e.g. "32-bit"
        char depth_line[16];
        { char c[8]; gui_itoa(have_fi ? (int)fi.bpp : 0, c, sizeof(c));
          depth_line[0] = 0; hw_append(depth_line, sizeof(depth_line), c);
          hw_append(depth_line, sizeof(depth_line), "-bit"); }

        // Real framebuffer size (pitch * height), honest instead of a fake VRAM number
        char fbsize_line[24];
        if (have_fi) format_size((uint64_t)fi.pitch * fi.height, fbsize_line, sizeof(fbsize_line));
        else copy_str(fbsize_line, "?", sizeof(fbsize_line));

        draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 80);
        win_draw_text(window_handle, x + 15, y + 10, "Display Information", COL_TEXT_PRIMARY);
        draw_label_value(x + 15,  y + 35, "Adapter:", g_gpu_name, 80);
        draw_label_value(x + 15,  y + 55, "PCI ID:", idbuf, 80);
        draw_label_value(x + 250, y + 35, "Buffer:", fbsize_line, 80);
        draw_label_value(x + 250, y + 55, "Depth:", depth_line, 80);
        g_content_bottom_y = y + 80;   // (#227)
    }
}

// =============================================================================
// Panel: Sound
// =============================================================================

static void draw_sound_panel(void) {
    // (#382 pass2) Only master volume/mute and the WAV test are real (kernel
    // mixer + SYS_PLAY_WAV). The output DEVICE is the real PCI-enumerated audio
    // controller (read-only, no device switching support). There is no audio
    // capture path and no equalizer DSP, so the Input + Equalizer sections show
    // an honest "not available" state instead of the old fake dropdowns / VU
    // meter / 10-band sliders.
    hwinfo_load();
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    char buf[32];

    draw_section_header(x, y, "Sound");
    y += 40;                                  // 60

    // ---- Output (real device identity; volume/mute REAL) ----
    draw_subsection(x, y, "Output");
    y += 25;                                  // 85

    draw_label(x, y + 4, "Device");
    win_draw_rect(window_handle, x + 100, y, 260, 28, COL_INPUT_BG);
    gui_draw_rect_outline(window_handle, x + 100, y, 260, 28, COL_INPUT_BORDER);
    win_draw_text(window_handle, x + 110, y + 6,
                  g_audio_present ? g_audio_name : "No audio device detected",
                  g_audio_present ? COL_TEXT_PRIMARY : COL_TEXT_DISABLED);
    draw_button_small(x + 380, y + 2, 70, sound_muted ? "Unmute" : "Mute", false);
    y += 40;                                  // 125

    draw_label(x, y, "Volume");
    master_volume = get_volume();             // live real master level
    gui_itoa(master_volume, buf, sizeof(buf));
    int len = my_strlen(buf);
    buf[len++] = '%'; buf[len] = 0;
    draw_slider(x + 100, y, 260, master_volume, 100, sound_muted ? COL_TEXT_DISABLED : COL_SUCCESS);
    win_draw_text(window_handle, x + 375, y, buf, COL_TEXT_SECONDARY);
    y += 50;                                  // 175

    // ---- Input (no capture support) ----
    draw_subsection(x, y, "Input");
    y += 25;                                  // 200
    draw_hint_ic(x, y, "CMINUS", theme_color(THEME_COLOR_MUTED) /* (#704) was hardcoded 0x00A0A0A8 */,
                 "Audio input (microphone capture) is not supported by this build.");
    y += 40;                                  // 240

    // ---- Equalizer (no DSP) ----
    draw_subsection(x, y, "Equalizer");
    y += 25;                                  // 265
    draw_hint_ic(x, y, "CMINUS", theme_color(THEME_COLOR_MUTED) /* (#704) was hardcoded 0x00A0A0A8 */,
                 "No hardware equalizer or audio DSP is available.");
    y += 40;                                  // 305

    // ---- System sounds (real preference) ----
    draw_subsection(x, y, "System Sounds");
    y += 25;                                  // 330
    draw_toggle_labeled(x, y, 300, "System Sound Effects", sound_effects);
    y += 40;                                  // 370

    // Test Speakers plays a real WAV via SYS_PLAY_WAV (needs a real device).
    draw_button(x, y, 130, "Test Speakers", false, false);
    y += 38;                                  // 408
    if (!g_audio_present)
        draw_hint_ic(x, y, "CMINUS", theme_color(THEME_COLOR_MUTED) /* (#704) was hardcoded 0x00A0A0A8 */, "No audio output device present.");
    else if (sound_test_status == 3)
        draw_hint(x, y, "Playing test sound...");
    g_content_bottom_y = y + 20;   // (#227)
}

// =============================================================================
// Panel: Network
// =============================================================================

// (#227/#223) ONE source of truth for the DHCP toggle, the conditional
// "Configure IP..." button and the Firewall toggle, consumed by BOTH
// draw_network_panel() and the PANEL_NETWORK branch of handle_content_click().
// This REPLACES the previous instance fix, `fw_y = dhcp_y + 120 +
// (dhcp_enabled ? 0 : 36)`: that ternary happened to be arithmetically
// correct (it mirrored the "Configure IP..." button's 36px cost), but it was
// a re-guessed constant that could silently go wrong again the moment a
// THIRD row was ever inserted between DHCP and Firewall - exactly the class
// of bug it was patching. This function instead walks the SAME conditional
// draw_network_panel() walks (whatever is or is not drawn between DHCP and
// Firewall), so the two structurally cannot diverge.
typedef struct {
    int dhcp, cfgip, fw;   // cfgip is -1 when DHCP is on (button not drawn)
    // (#786) "Set DNS..." occupies the same row as "Configure IP...", and is
    // drawn INSTEAD of it when DHCP is on. -1 when DHCP is off. Exactly one of
    // cfgip/dnsbtn is >= 0, so the row cost is unconditional and the geometry
    // below no longer changes height with the toggle.
    int dnsbtn;
    // #238: the two default-policy toggles. They are in the SAME struct as
    // everything else in this panel, which is what keeps them inside
    // hitbox-lint's and contract-lint's reach (docs/CONTRACT_API.md 6).
    int pol_in, pol_out;
} network_geom_t;
static void network_geom(network_geom_t *g) {
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    y += 40;                  /* section header */
    y += 125;                 /* connection status card + gap */
    g->dhcp = y; y += 40;
    if (!dhcp_enabled) {
        g->cfgip = y; g->dnsbtn = -1;
    } else {
        g->cfgip = -1; g->dnsbtn = y;   // (#786) DNS is reachable on DHCP too
    }
    y += 36;
    y += 25;                  /* "VPN" subsection heading */
    y += 30;                  /* VPN hint line */
    y += 25;                  /* "Firewall" subsection heading */
    g->fw = y; y += 40;
    g->pol_in  = y; y += 30;
    g->pol_out = y;
}

static void draw_network_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;
    network_geom_t g; network_geom(&g);

    draw_section_header(x, y, "Network");
    y += 40;

    // Connection status card
    draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 110);

    win_draw_text(window_handle, x + 15, y + 10, "Ethernet Connection", COL_TEXT_PRIMARY);

    if (ethernet_connected) {
        gui_fill_rounded(window_handle, x + 180, y + 8, 90, 22, 11, COL_SUCCESS);
        gui_text_ttf_centered(window_handle, x + 180, y + 8, 90, 22, "Connected", gui_ink_on(COL_SUCCESS), 12);
    } else {
        gui_fill_rounded(window_handle, x + 180, y + 8, 100, 22, 11, COL_ERROR);
        gui_text_ttf_centered(window_handle, x + 180, y + 8, 100, 22, "Disconnected", gui_ink_on(COL_ERROR), 12);
    }

    draw_label_value(x + 15, y + 40, "IP Address:", ip_address, 100);
    draw_label_value(x + 15, y + 60, "Subnet:", subnet_mask, 100);
    draw_label_value(x + 15, y + 80, "MAC:", mac_address, 100);

    draw_label_value(x + 280, y + 40, "Gateway:", gateway, 80);
    // #786: "DNS (live)" is dns_get_server(), the server the resolver will
    // query. "DHCP offered" is what the lease supplied. They CAN differ - a
    // static config or a Settings change pins the first and ignores the
    // second - and when they do, showing only one of them is how a user ends
    // up debugging a machine that is not doing what the screen says.
    draw_label_value(x + 280, y + 60, "DNS (live):", dns_primary, 80);
    draw_label_value(x + 280, y + 80, "DHCP offered:", dns_offered, 80);

    y += 125;

    // DHCP toggle
    draw_toggle_labeled(x, g.dhcp, 300, "Obtain IP automatically (DHCP)", dhcp_enabled);
    y = g.dhcp + 40;

    if (!dhcp_enabled) {
        draw_button_small(x + 20, g.cfgip, 160, "Configure IP...", false);
        y = g.cfgip + 36;
    } else {
        // (#786) On DHCP the address is not yours to set, but the RESOLVER is.
        draw_button_small(x + 20, g.dnsbtn, 160, "Set DNS...", false);
        y = g.dnsbtn + 36;
    }

    // VPN section. There is no VPN client / tunnel stack in this build, so this
    // is honestly marked unavailable rather than a toggle with a fake connected
    // state. Kept the same vertical footprint so the Firewall layout is stable.
    draw_subsection(x, y, "VPN");
    y += 25;

    draw_hint_ic(x, y, "CMINUS", theme_color(THEME_COLOR_MUTED) /* (#704) was hardcoded 0x00A0A0A8 */,
                 "VPN is not available (no VPN client in this build).");
    y += 30;

    // Firewall (#238). Every control below now reaches a real packet filter.
    draw_subsection(x, g.fw - 25, "Firewall");
    draw_toggle_labeled(x, g.fw, 300, "Enable Firewall", firewall_enabled);
    // The two default policies, as toggles rather than a bespoke Allow/Deny
    // widget: "block" == DENY, which is the direction a person thinks in, and
    // it reuses the shared control instead of inventing a fourth kind of
    // selector (CLAUDE.md: reuse the existing primitive).
    draw_toggle_labeled(x, g.pol_in, 300,  "Block unsolicited inbound",
                        fw_pol_in == FW_ACT_DENY);
    draw_toggle_labeled(x, g.pol_out, 300, "Block outbound connections",
                        fw_pol_out == FW_ACT_DENY);
    y = g.pol_out + 32;

    // #238: the honest scope statement stays, but it now says what IS true.
    // #223 put a "this does nothing" note here because there was no filter;
    // that note is retired, not deleted, because the panel must still be
    // explicit about what the filter does NOT cover. A control that overstates
    // its reach is the same defect one size smaller.
    //
    // The rule list is READ-ONLY in this panel. Rules are added and removed
    // through the contract API, and the panel says so rather than drawing an
    // editor that does not exist - the #223 lesson applied to its own fix.
    {
        char line[128];
        int n = fw_rule_count;
        if (n == 0) {
            draw_hint_ic(x, y, "CMINUS", theme_color(THEME_COLOR_MUTED),
                         "No explicit rules: the two default policies above "
                         "decide every connection.");
            y += 22;
        }
        for (int i = 0; i < n && i < MAX_FW_RULES; i++) {
            snprintf(line, sizeof(line), "%s  %s  %s  port %u",
                     fw_rules[i].dir == FW_DIR_IN ? "in " : "out",
                     fw_rules[i].action == FW_ACT_DENY ? "deny " : "allow",
                     fw_rules[i].proto == FW_PROTO_UDP ? "udp" : "tcp",
                     (unsigned)fw_rules[i].port);
            win_draw_text(window_handle, x + 20, y, line, COL_TEXT_PRIMARY);
            y += 20;
        }
        y += 8;

        // The counters. This is the panel telling the truth about itself: a
        // firewall that has never dropped anything says so, and one that is
        // dropping says how much. It is also the fastest way for anyone to
        // check that the engine is actually on the packet path.
        snprintf(line, sizeof(line),
                 "Filtered %llu packets; dropped %llu inbound, %llu outbound; "
                 "%llu tracked-flow passes.",
                 (unsigned long long)fw_stats.calls,
                 (unsigned long long)fw_stats.drop_in,
                 (unsigned long long)fw_stats.drop_out,
                 (unsigned long long)fw_stats.ct_hit);
        win_draw_text_small(window_handle, x, y, line, COL_TEXT_SECONDARY);
        y += 22;

        draw_hint_ic(x, y, "CMINUS", theme_color(THEME_COLOR_MUTED),
                     "Rules apply to TCP and UDP by destination port. ARP, "
                     "ICMP (ping) and the DHCP exchange are never filtered. "
                     "Replies to connections this machine opened always pass. "
                     "Edit rules with: ctl set settings firewall.add_rule");
        y += 30;
    }
    g_content_bottom_y = y;   // (#227)
}


// Panel: Keyboard
// =============================================================================

// #230 (docs/SETTINGS_CONTROL_AUDIT.md #224): the Keyboard Layout dropdown,
// Repeat Rate/Delay sliders, Lock Keys checkboxes, "Edit Shortcuts" button and
// "Test Area" box are ALL DELETED. None had a real consumer: there is no
// keymap table anywhere but the one hardcoded US scancode table (cpu/isr.c),
// no PS/2 typematic (0xF3) command is ever sent, no software auto-repeat
// exists for ordinary keys (only consumer/media keys get one - see
// drivers/usb_hid.c HID_CONS_REPEAT_*), the lock-key vars were not even
// persisted, and the Layout dropdown could not even be OPENED (drawn with a
// literal `false`, no dropdown_open() call anywhere). This panel therefore
// has NO interactive rows left, so no shared geometry function is needed -
// draw_keyboard_panel() and handle_content_click() cannot drift apart when
// there is nothing left to drift.
static void draw_keyboard_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;

    draw_section_header(x, y, "Keyboard");
    y += 40;

    // Keyboard shortcuts (read-only reference list; not user-editable, so
    // showing it is not a claim of anything this build cannot do).
    draw_subsection(x, y, "Shortcuts");
    y += 25;

    for (int i = 0; i < 6 && i < 8; i++) {
        int row = i / 2;
        int col = i % 2;
        int sx = x + col * 250;
        int sy = y + row * 25;

        draw_label_value(sx, sy, shortcuts[i].action, shortcuts[i].keys, 120);
    }
    y += 85;

    // #230: layout selection, typematic rate/delay and lock-key indicators
    // removed - see the comment above draw_keyboard_panel().
    draw_hint_ic(x, y, "CMINUS", theme_color(THEME_COLOR_MUTED),
                 "Keyboard layout selection, repeat rate/delay and lock-key "
                 "indicators are not implemented in this build (no keymap "
                 "table or typematic driver exists) and have been removed "
                 "until they are.");
    g_content_bottom_y = y + 32;   // (#227) hint line height (wraps to 2 lines)
}

// =============================================================================
// Panel: Mouse/Touchpad
// =============================================================================

// #745/#230: ONE source of truth for the Mouse panel's row positions,
// consumed by BOTH draw_mouse_panel() and the PANEL_MOUSE branch of
// handle_content_click(). #230 (docs/SETTINGS_CONTROL_AUDIT.md #224) deleted
// Pointer Size, Pointer Trails + Trail Length, Scroll Speed, Natural
// Scrolling, Scroll Inertia and Left-handed Mode: none had ANY consumer
// anywhere in the tree (no cursor-trail renderer, no kinetic-scroll
// implementation, nothing inverts scroll, mouse button bits pass straight
// through with no swap), and Pointer Size additionally had no click handler
// at all. Double-click Speed is now REAL (settingscfg_dblclick_ms(), read by
// the compositor's double-click detector) instead of a round trip.
#define MG_SUBSECTION 25   /* vertical cost of one draw_subsection() heading */
typedef struct {
    int sens, dbl, cursor;
} mouse_geom_t;
static void mouse_geom(mouse_geom_t *m) {
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    y += 40;                                  /* section header */
    y += MG_SUBSECTION;                       /* "Pointer" */
    m->sens      = y; y += 35;
    m->dbl       = y; y += 45;
    y += MG_SUBSECTION;                       /* "Cursor" (#745) */
    m->cursor    = y;
}

static void draw_mouse_panel(void) {
    int x = CONTENT_X + PADDING;
    char buf[32];
    mouse_geom_t m; mouse_geom(&m);

    draw_section_header(x, PADDING, "Mouse & Touchpad");

    // Pointer
    draw_subsection(x, m.sens - MG_SUBSECTION, "Pointer");

    draw_label(x, m.sens, "Mouse Sensitivity");
    gui_itoa(pointer_speed, buf, sizeof(buf));
    draw_slider(x + 140, m.sens, 250, pointer_speed, 100, COL_SLIDER_FILL);
    win_draw_text(window_handle, x + 405, m.sens, buf, COL_TEXT_SECONDARY);

    draw_label(x, m.dbl, "Double-click Speed");
    gui_itoa(double_click_speed, buf, sizeof(buf));
    draw_slider(x + 160, m.dbl, 230, double_click_speed, 100, COL_SLIDER_FILL);
    win_draw_text(window_handle, x + 405, m.dbl, buf, COL_TEXT_SECONDARY);

    // Cursor (#745: moved here from Appearance - the pointer's look belongs
    // with the pointer's behaviour, and it is NOT left behind on Appearance).
    // Same shared dropdown widget, same label column and same 140px control
    // offset as the rows above, per docs/UI_STYLE_GUIDE.md.
    draw_subsection(x, m.cursor - MG_SUBSECTION, "Cursor");
    draw_label(x, m.cursor + 6, "Cursor Style");
    {
        /* read-only clamp: a draw pass must never write app state (#745) */
        int ci = OPT_CLAMP(cursor_theme, CURSOR_OPTS);
        draw_dropdown_n(x + 140, m.cursor, 160, CURSOR_OPTS[ci],
                        g_dd_open && g_dd_sel == &cursor_theme, CURSOR_OPTS_COUNT);
    }

    // #230: Pointer Size, Pointer Trails/Trail Length, Scroll Speed, Natural
    // Scrolling, Scroll Inertia and Left-handed Mode removed - see the
    // comment above mouse_geom().
    int hy = m.cursor + 45;
    draw_hint_ic(x, hy, "CMINUS", theme_color(THEME_COLOR_MUTED),
                 "Pointer size, trails, scroll tuning and left-handed mode "
                 "are not implemented in this build and have been removed "
                 "until they are.");
    g_content_bottom_y = hy + 32;   // (#227) hint line height (wraps to 2 lines)
}

// =============================================================================
// Panel: Date & Time
// =============================================================================

// (#227) ONE source of truth for this panel's row positions, consumed by BOTH
// draw_datetime_panel() and the PANEL_DATETIME branch of handle_content_click().
// Those two used to carry independent arithmetic chains and had drifted 50px
// apart starting at Date Format: the click handler's `dfmt_y = fmt_y + 95`
// chain never accounted for the "Time Zone" subsection heading + dropdown the
// draw pass inserts in between, so Date Format, Week-starts-on and the "Set
// Date & Time" button were all unreachable by mouse (docs/SETTINGS_CONTROL_
// AUDIT.md #224). auto_time and use_24hour were NOT drifted - the shared
// function reproduces that too, rather than "fixing" rows that were already
// correct.
//
// #230: Date Format is DELETED (its own field with it) - nothing in the tree
// ever formatted a date with it, not even this panel's own clock card above,
// which was hardcoded "DD Mon YYYY" regardless of the dropdown's selection.
// Use 24-hour format is now REAL: settingscfg_use24h() (libc/settingscfg.c)
// drives both this panel's clock card and the taskbar clock.
typedef struct {
    int auto_time, use24, tz, weekstart, setbtn;
} datetime_geom_t;
static void datetime_geom(datetime_geom_t *g) {
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    y += 40;                 /* section header */
    y += 115;                /* clock card + gap */
    g->auto_time = y; y += 10;
    y += 30;                 /* hint line + NTP status gap */
    y += 20;
    g->use24 = y; y += 45;
    y += 25;                 /* "Time Zone" subsection heading */
    g->tz = y; y += 50;
    g->weekstart = y; y += 50;
    g->setbtn = y;
}

static void draw_datetime_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;
    datetime_geom_t g; datetime_geom(&g);

    draw_section_header(x, y, "Date & Time");
    y += 40;

    // Current time display (large)
    draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 100);

    // #49: the shared local-clock helper, so this panel and the taskbar cannot
    // disagree. It also rolls the DATE across the offset, which the old private
    // arithmetic here did not: at 08:00 UTC on the 1st, an Auckland user was
    // shown 20:00 on the 1st instead of 20:00 on the 2nd.
    tz_time_t lt;
    tz_local_now(&lt);
    int rtc_sec = lt.sec;
    int rtc_day = lt.day, rtc_month = lt.month, rtc_year = lt.year;
    int display_h = lt.hour;
    int display_m = lt.min;

    // Format HH:MM:SS (or hh:MM:SS AM/PM). #230: this panel's own clock now
    // honors "Use 24-hour format" instead of ignoring it - a caller-visible
    // change from just toggling the switch below, with no reboot/relaunch.
    int fmt_h = display_h; const char *ampm = "";
    if (!use_24hour) {
        ampm = (display_h >= 12) ? " PM" : " AM";
        fmt_h = display_h % 12; if (fmt_h == 0) fmt_h = 12;
    }
    char time_str[16];
    time_str[0] = '0' + fmt_h / 10; time_str[1] = '0' + fmt_h % 10;
    time_str[2] = ':';
    time_str[3] = '0' + display_m / 10; time_str[4] = '0' + display_m % 10;
    time_str[5] = ':';
    time_str[6] = '0' + rtc_sec / 10;   time_str[7] = '0' + rtc_sec % 10;
    time_str[8] = 0;
    if (ampm[0]) { int tl = my_strlen(time_str); for (int i2 = 0; ampm[i2]; i2++) time_str[tl++] = ampm[i2]; time_str[tl] = 0; }

    // Format "DD Mon YYYY"
    const char *mon_names[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
    const char *mn = (rtc_month >= 1 && rtc_month <= 12) ? mon_names[rtc_month - 1] : "???";
    char date_str[32];
    int dsi = 0;
    date_str[dsi++] = '0' + rtc_day / 10;
    date_str[dsi++] = '0' + rtc_day % 10;
    date_str[dsi++] = ' ';
    date_str[dsi++] = mn[0]; date_str[dsi++] = mn[1]; date_str[dsi++] = mn[2];
    date_str[dsi++] = ' ';
    date_str[dsi++] = '0' + rtc_year / 1000;
    date_str[dsi++] = '0' + (rtc_year / 100) % 10;
    date_str[dsi++] = '0' + (rtc_year / 10) % 10;
    date_str[dsi++] = '0' + rtc_year % 10;
    date_str[dsi] = 0;

    // Big digital clock
    win_draw_text(window_handle, x + 30, y + 20, time_str, COL_TEXT_PRIMARY);
    win_draw_text(window_handle, x + 30, y + 50, date_str, COL_TEXT_SECONDARY);

    // Analog clock representation (simple square)
    int cx = x + 400, cy = y + 50;
    char hm_str[8];
    hm_str[0] = '0' + fmt_h / 10; hm_str[1] = '0' + fmt_h % 10;
    hm_str[2] = ':';
    hm_str[3] = '0' + display_m / 10; hm_str[4] = '0' + display_m % 10;
    hm_str[5] = 0;
    win_draw_rect(window_handle, cx - 35, cy - 35, 70, 70, COL_CONTENT_BG);
    gui_draw_rect_outline(window_handle, cx - 35, cy - 35, 70, 70, COL_TEXT_SECONDARY);
    win_draw_text(window_handle, cx - 16, cy - 8, hm_str, COL_TEXT_PRIMARY);

    y += 115;

    // Auto time toggle
    draw_toggle_labeled(x, g.auto_time, 300, "Set time automatically", auto_time);
    y = g.auto_time + 10;
    draw_hint(x, y + 20, "Synchronize with network time servers");
    y += 30;
    // NTP status feedback
    if (ntp_status == 1)
        draw_hint_ic(x + 20, y + 20, "CCHECK", theme_color(THEME_COLOR_SUCCESS), "Time synchronized successfully.");
    else if (ntp_status == -1)
        win_draw_text(window_handle, x + 20, y + 20, "NTP sync failed (no network?).", theme_color(THEME_COLOR_ERROR));

    // Time format
    draw_toggle_labeled(x, g.use24, 300, "Use 24-hour format", use_24hour);

    // Timezone
    draw_subsection(x, g.tz - 25, "Time Zone");
    draw_dropdown(x, g.tz, 350, tz_label(timezone_idx), false);

    // #230: Date Format deleted here - see the comment above datetime_geom().

    // First day of week
    draw_label(x, g.weekstart, "Week starts on");
    const char* week_days[] = {"Sunday", "Monday"};
    draw_option_buttons(x + 140, g.weekstart - 3, week_days, ARRAY_COUNT(week_days), first_day_of_week);

    // Manual time setting button
    if (!auto_time) {
        draw_button(x, g.setbtn, 160, "Set Date & Time", true, false);
    }
    g_content_bottom_y = g.setbtn + 40;   // (#227) button height + margin
}

// =============================================================================
// Panel: Users & Accounts
// =============================================================================

// (#745) Antialiased avatar badge - the design doc's fix for two separate
// bugs at once: the swatch was a hard-edged SQUARE (a third, inconsistent
// avatar treatment alongside the login screen's circle and the kernel/lock
// screens' none-at-all), and the initial letter was drawn in a FIXED
// COL_TEXT_PRIMARY straight on top of whichever saturated identity color the
// account got - measured against the dark theme, 4 of the 8 palette colors
// fail 4.5:1 outright (design doc section 9). Both fixed by construction
// here: `bg` is the REAL surface this badge is composited over (the card),
// and `ink` (ring + letter) is derived from it via gui_ensure_contrast(),
// which is guaranteed to clear the floor rather than merely observed to on
// the colors someone happened to check by eye.
//
// The app toolkit (gui_style.h) has no dedicated AA ring primitive; this
// reuses the "bigger AA circle behind, smaller AA circle on top" technique
// already shipping in setup/main.rs's radio buttons and install/main.c's
// completion dots (grep gui_fill_circle_aa) rather than adding a new one.
static void draw_avatar_badge(int x, int y, int d, uint32_t fill, uint32_t bg, char letter_ch) {
    uint32_t ink = gui_ensure_contrast(gui_ink_on(bg), bg, GUI_FLOOR_TEXT);
    int ring_w = (d >= 48) ? 2 : 1;
    gui_fill_circle_aa(window_handle, x - ring_w, y - ring_w, d + ring_w * 2, ink, bg);
    gui_fill_circle_aa(window_handle, x, y, d, fill, ink);

    char letter[2] = { letter_ch, 0 };
    if (letter[0] >= 'a' && letter[0] <= 'z') letter[0] -= 32;
    int fs = (d >= 48) ? 22 : 13;
    gui_text_ttf_centered(window_handle, x, y, d, d, letter, ink, fs);
}

// (#227) ONE source of truth for this panel's interactive rows, consumed by
// BOTH draw_users_panel() and the PANEL_USERS branch of
// handle_content_click(). The per-row "Other Users" list and the trailing
// Add User / Guest toggle rows all depend on user_count, which used to be
// re-added independently in each place; the two chains had drifted 5px
// apart on the Add User / Guest rows (docs/SETTINGS_CONTROL_AUDIT.md #224).
// This walks the exact same user_count/current_user_idx skip-loop
// draw_users_panel() does, so the two cannot diverge.
typedef struct {
    int edit_profile, autologin, chpw, other_row0, adduser, guest;
} users_geom_t;
static void users_geom(users_geom_t *g) {
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    y += 40;                    /* section header */
    g->edit_profile = y + 35;   /* current-user card's Edit Profile button */
    y += 115;                   /* card + gap */
    y += 25;                    /* "Account Settings" subsection heading */
    g->autologin = y; y += 10;
    y += 45;                    /* hint line + advance to Change Password */
    g->chpw = y; y += 50;
    y += 25;                    /* "Other Users" subsection heading */
    g->other_row0 = y;
    int rows = 0;
    for (int i = 0; i < user_count; i++) {
        if (i == current_user_idx) continue;
        if (users[i].username[0] == 0) continue;
        rows++;
    }
    y += rows * 60;
    y += 10;
    g->adduser = y; y += 45;
    g->guest = y;
}

static void draw_users_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;
    users_geom_t g; users_geom(&g);

    draw_section_header(x, y, "Users & Accounts");
    y += 40;

    // Current user card
    draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 100);

    // Avatar
    draw_avatar_badge(x + 15, y + 20, 60, users[current_user_idx].avatar_color,
                      COL_CARD_BG, users[current_user_idx].fullname[0]);

    // User info
    win_draw_text(window_handle, x + 90, y + 20, users[current_user_idx].fullname, COL_TEXT_PRIMARY);
    if (users[current_user_idx].email[0])
        win_draw_text(window_handle, x + 90, y + 42, users[current_user_idx].email, COL_TEXT_SECONDARY);
    else
        win_draw_text(window_handle, x + 90, y + 42, "(no email set)", COL_TEXT_DISABLED);

    const char* roles[] = {"Administrator", "Standard User", "Guest"};
    win_draw_text(window_handle, x + 90, y + 62, roles[users[current_user_idx].role], COL_TEXT_DISABLED);

    draw_button_small(x + 350, g.edit_profile, 100, "Edit Profile", false);

    y += 115;

    // Account settings
    draw_subsection(x, y, "Account Settings");
    y += 25;

    // (#566) Real per-account autologin, mirrored from the kernel
    // (sys_get_autologin()) - not a per-launch cosmetic bool. Toggling opens a
    // password-confirm modal for a non-root session (see autologin_request());
    // root can toggle any account's without one (kernel ABI: "Root sets for
    // anyone").
    {
        int is_al_cur = (autologin_user[0] &&
                         strcmp(autologin_user, users[current_user_idx].username) == 0);
        char al_label[80];
        {
            const char *pre = "Automatically log in as ";
            const char *u = users[current_user_idx].username;
            int k = 0;
            while (pre[k] && k < (int)sizeof(al_label) - 1) { al_label[k] = pre[k]; k++; }
            int j = 0;
            while (u[j] && k < (int)sizeof(al_label) - 1) { al_label[k++] = u[j++]; }
            al_label[k] = '\0';
        }
        draw_toggle_labeled(x, g.autologin, 340, al_label, is_al_cur);
    }
    draw_hint(x, g.autologin + 30, "Skips the login screen at boot for this account only.");

    draw_button(x, g.chpw, 160, "Change Password", false, false);
    y = g.chpw + 50;

    // Other users
    draw_subsection(x, y, "Other Users");
    y += 25;

    for (int i = 0; i < user_count; i++) {
        if (i == current_user_idx) continue;
        if (users[i].username[0] == 0) continue;

        draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 50);

        // Avatar
        draw_avatar_badge(x + 15, y + 10, 30, users[i].avatar_color,
                          COL_CARD_BG, users[i].fullname[0]);

        win_draw_text(window_handle, x + 60, y + 10, users[i].fullname, COL_TEXT_PRIMARY);
        win_draw_text(window_handle, x + 60, y + 28, roles[users[i].role], COL_TEXT_SECONDARY);

        // (#566) Only root can set autologin for an account that is not its
        // own, per the kernel ABI - so this control only appears for a root
        // Settings session.
        if (settings_is_root()) {
            int is_al = (autologin_user[0] && strcmp(autologin_user, users[i].username) == 0);
            draw_button_small(x + 290, y + 13, 100, is_al ? "Auto-login: ON" : "Auto-login", is_al);
        }

        draw_button_small(x + 400, y + 13, 80, "Remove", false);

        y += 60;
    }

    draw_button(x, g.adduser, 120, "Add User", true, false);
    draw_toggle_labeled(x, g.guest, 300, "Enable Guest Account", guest_enabled);
    g_content_bottom_y = g.guest + 40;   // (#227) toggle height + margin
}

// =============================================================================
// Panel: Privacy & Security
// =============================================================================

// (#227) ONE source of truth for this panel's toggle rows, consumed by BOTH
// draw_privacy_panel() and the PANEL_PRIVACY branch of handle_content_click().
// "Require password on wake" carried an X-AXIS bug independent of this y
// conversion: draw_toggle_labeled(x + 20, y, 280, ...) draws the actual
// switch at x + 20 + 280 = x + 300 (label_width indent + label_width, the
// same convention every other 48px-wide toggle switch on this panel uses -
// see draw_toggle()/draw_toggle_labeled() above), but the click handler
// tested x + 280..x + 328 - 20px left of the real switch, forgetting the x+20
// indent. Fixed by testing x + 300..x + 348 like every other toggle here.
typedef struct {
    int lock, timeout_dd, reqpw;   // timeout_dd/reqpw are -1 when lock is off
    int location, diagnostics, crash;
} privacy_geom_t;
static void privacy_geom(privacy_geom_t *g) {
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    y += 40;                  /* section header */
    y += 25;                  /* "Screen Lock" subsection heading */
    g->lock = y; y += 35;
    if (screen_lock_enabled) {
        g->timeout_dd = y; y += 40;
        g->reqpw = y; y += 10;
        y += 30;               /* hint line */
    } else {
        g->timeout_dd = -1;
        g->reqpw = -1;
    }
    y += 25;                  /* "Privacy" subsection heading */
    g->location = y; y += 10;
    y += 45;                  /* hint line + advance */
    g->diagnostics = y; y += 10;
    y += 45;
    g->crash = y;
}

static void draw_privacy_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;
    char buf[32];
    privacy_geom_t g; privacy_geom(&g);

    draw_section_header(x, y, "Privacy & Security");
    y += 40;

    // Screen lock
    draw_subsection(x, y, "Screen Lock");
    y += 25;

    draw_toggle_labeled(x, g.lock, 300, "Enable Screen Lock", screen_lock_enabled);
    y = g.lock + 35;

    if (screen_lock_enabled) {
        draw_label(x + 20, g.timeout_dd, "Lock after");
        const char* timeouts[] = {"Never", "1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
        int timeout_idx = 0;
        if (lock_timeout == 1) timeout_idx = 1;
        else if (lock_timeout == 2) timeout_idx = 2;
        else if (lock_timeout == 5) timeout_idx = 3;
        else if (lock_timeout == 10) timeout_idx = 4;
        else if (lock_timeout == 15) timeout_idx = 5;
        else if (lock_timeout == 30) timeout_idx = 6;
        draw_dropdown(x + 120, g.timeout_dd, 120, timeouts[timeout_idx], false);

        draw_toggle_labeled(x + 20, g.reqpw, 280, "Require password on wake", require_password_wake);
        draw_hint(x + 20, g.reqpw + 30, "(#566) MayteraOS's lock always requires a password to "
                                   "unlock; this toggle has no bypass to disable.");
        y = g.reqpw + 40;
    }

    // Privacy. These persist to /CONFIG/PRIVACY.CFG (real stored settings); the
    // hints are honest about the fact that there is no telemetry/location backend.
    draw_subsection(x, y, "Privacy");
    y += 25;

    draw_toggle_labeled(x, g.location, 300, "Location Services", location_services);
    draw_hint(x, g.location + 30, "Preference only: no location provider exists in this build.");

    draw_toggle_labeled(x, g.diagnostics, 300, "Send Diagnostics", diagnostics_enabled);
    draw_hint(x, g.diagnostics + 30, "Preference only: no diagnostics are collected or transmitted.");

    draw_toggle_labeled(x, g.crash, 300, "Send Crash Reports", crash_reports);
    y = g.crash + 45;

    // App permissions: there is no per-app capability enforcement in this build,
    // so instead of a fabricated allow/deny matrix we state that honestly.
    draw_subsection(x, y, "App Permissions");
    y += 25;
    draw_hint_ic(x, y, "CMINUS", theme_color(THEME_COLOR_MUTED) /* (#704) was hardcoded 0x00A0A0A8 */,
                 "Per-app permission enforcement is not implemented in this build.");
    y += 18;
    draw_hint(x, y, "All apps run with the user's full rights (capability tokens are planned).");
    g_content_bottom_y = y + 20;   // (#227) hint line height
}

// =============================================================================
// Panel: Storage
// =============================================================================

// (#227) ONE source of truth for the four Cache/Trash button rows, consumed
// by BOTH draw_storage_panel() and the PANEL_STORAGE branch of
// handle_content_click(). The handler used to derive `cache_base` from
// `base_y + 40 + drive_count*80 + 40`, a re-guessed total that landed 17-20px
// below where the buttons are actually drawn (`y - 3` relative to the label
// row) - the three "Clear" buttons were live in only a ~7px band of their
// 24px height, and "Empty Trash" had no overlap at all
// (docs/SETTINGS_CONTROL_AUDIT.md #224). Each field IS the button's drawn
// top-left y (the `-3` already applied), so the handler's hit rect can equal
// the drawn rect exactly rather than re-deriving it.
typedef struct {
    int thumb_clear, app_clear, sys_clear, empty_trash;
} storage_geom_t;
static void storage_geom(storage_geom_t *g) {
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    y += 40;                 /* section header */
    y += 25;                 /* "Drives" subsection heading */
    for (int i = 0; i < drive_count; i++) {
        if (drives[i].name[0] == 0) continue;
        y += 80;
    }
    y += 10;
    y += 25;                 /* "Cache & Temporary Files" subsection heading */
    g->thumb_clear = y - 3; y += 30;
    g->app_clear   = y - 3; y += 30;
    g->sys_clear   = y - 3; y += 40;
    y += 25;                 /* "Trash" subsection heading */
    g->empty_trash = y - 3;
}

static void draw_storage_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;
    char buf[32];
    storage_geom_t g; storage_geom(&g);

    draw_section_header(x, y, "Storage");
    y += 40;

    // Drives
    draw_subsection(x, y, "Drives");
    y += 25;

    for (int i = 0; i < drive_count; i++) {
        if (drives[i].name[0] == 0) continue;

        draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 70);

        // Drive icon
        win_draw_text(window_handle, x + 15, y + 25, "[H]", COL_ACCENT);

        // Drive name and mount point
        win_draw_text(window_handle, x + 50, y + 12, drives[i].name, COL_TEXT_PRIMARY);
        win_draw_text(window_handle, x + 50, y + 32, drives[i].mount_point, COL_TEXT_SECONDARY);
        {
            char fsline[96];
            int n = 0;
            const char *fs = drives[i].filesystem;
            for (int j = 0; fs[j] && n < 94; j++) fsline[n++] = fs[j];
            if (drives[i].model[0]) {
                const char *sep = "  -  ";
                for (int j = 0; sep[j] && n < 94; j++) fsline[n++] = sep[j];
                for (int j = 0; drives[i].model[j] && n < 94; j++) fsline[n++] = drives[i].model[j];
            }
            fsline[n] = 0;
            win_draw_text(window_handle, x + 50, y + 50, fsline, COL_TEXT_DISABLED);
        }

        // Usage bar
        int percent = (int)((drives[i].used_bytes * 100) / drives[i].total_bytes);
        uint32_t bar_color = (percent > 90) ? COL_ERROR : (percent > 75) ? COL_WARNING : COL_ACCENT;
        draw_progress_bar(x + 200, y + 18, 200, percent, bar_color);

        // Usage text
        char used_str[32], total_str[32];
        format_size(drives[i].used_bytes, used_str, sizeof(used_str));
        format_size(drives[i].total_bytes, total_str, sizeof(total_str));

        gui_itoa(percent, buf, sizeof(buf));
        int len = my_strlen(buf);
        buf[len++] = '%'; buf[len] = 0;
        win_draw_text(window_handle, x + 410, y + 15, buf, COL_TEXT_PRIMARY);

        win_draw_text(window_handle, x + 200, y + 38, used_str, COL_TEXT_SECONDARY);
        win_draw_text(window_handle, x + 280, y + 38, "of", COL_TEXT_DISABLED);
        win_draw_text(window_handle, x + 310, y + 38, total_str, COL_TEXT_SECONDARY);

        // SMART health indicator
        {
            const char *sh = (drives[i].smart == 1) ? "SMART: OK" :
                             (drives[i].smart == 0) ? "SMART: FAIL" : "SMART: n/a";
            uint32_t shc = (drives[i].smart == 1) ? COL_SUCCESS :
                           (drives[i].smart == 0) ? COL_ERROR : COL_TEXT_DISABLED;
            win_draw_text(window_handle, x + 410, y + 38, sh, shc);
        }

        y += 80;
    }

    y += 10;

    // Cache management
    draw_subsection(x, y, "Cache & Temporary Files");
    y += 25;

    char cache_str[32];

    format_size(cache_thumbnails, cache_str, sizeof(cache_str));
    draw_label_value(x, g.thumb_clear + 3, "Thumbnails:", cache_str, 140);
    draw_button_small(x + 280, g.thumb_clear, 60, "Clear", false);
    y = g.thumb_clear + 3 + 30;

    format_size(cache_apps, cache_str, sizeof(cache_str));
    draw_label_value(x, g.app_clear + 3, "App Cache:", cache_str, 140);
    draw_button_small(x + 280, g.app_clear, 60, "Clear", false);
    y = g.app_clear + 3 + 30;

    format_size(cache_system, cache_str, sizeof(cache_str));
    draw_label_value(x, g.sys_clear + 3, "System Cache:", cache_str, 140);
    draw_button_small(x + 280, g.sys_clear, 60, "Clear", false);
    y = g.sys_clear + 3 + 40;

    // Trash
    draw_subsection(x, y, "Trash");
    y += 25;

    format_size(trash_size, cache_str, sizeof(cache_str));
    draw_label_value(x, g.empty_trash + 3, "Trash Size:", cache_str, 140);
    draw_button_small(x + 280, g.empty_trash, 100, "Empty Trash", false);
    y = g.empty_trash + 3 + 45;

    // Total summary
    uint64_t total_all = 0, used_all = 0;
    for (int i = 0; i < drive_count; i++) {
        total_all += drives[i].total_bytes;
        used_all += drives[i].used_bytes;
    }

    draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 50);
    win_draw_text(window_handle, x + 15, y + 17, "Total Storage:", COL_TEXT_SECONDARY);

    char total_str[32], used_str[32];
    format_size(used_all, used_str, sizeof(used_str));
    format_size(total_all, total_str, sizeof(total_str));

    win_draw_text(window_handle, x + 140, y + 17, used_str, COL_TEXT_PRIMARY);
    win_draw_text(window_handle, x + 220, y + 17, "used of", COL_TEXT_DISABLED);
    win_draw_text(window_handle, x + 290, y + 17, total_str, COL_TEXT_PRIMARY);
    g_content_bottom_y = y + 50;   // (#227) total-storage card height
}

// =============================================================================
// Panel: About
// =============================================================================

// #84 Default Apps: pick which app opens each file category. Persisted to /ASSOC.CFG.
static int defaults_app_index(int ci, const char *cur) {
    int n; const assoc_category_t *cats = assoc_categories(&n);
    if (ci < 0 || ci >= n) return 0;
    for (int a = 0; a < cats[ci].napps; a++) {
        const char *p = cats[ci].apps[a]; int i = 0, eq = 1;
        for (; p[i] || cur[i]; i++) { if (p[i] != cur[i]) { eq = 0; break; } }
        if (eq) return a;
    }
    return 0;
}
static void defaults_set_category(int ci, const char *app) {
    int n; const assoc_category_t *cats = assoc_categories(&n);
    if (ci < 0 || ci >= n) return;
    const char *e = cats[ci].exts; char ext[16];
    while (*e) {
        while (*e == ' ') e++;
        int k = 0; while (*e && *e != ' ' && k < 15) ext[k++] = *e++;
        ext[k] = 0;
        if (k) assoc_set_default(ext, app);
    }
}
// (#262) Cache the per-category "current app" strings. assoc_category_current()
// does a full /ASSOC.CFG disk read on every call; draw_defaults_panel runs once
// per category EVERY redraw, and the compositor issues periodic EVENT_REDRAWs, so
// on the ext2 root that per-frame disk I/O made only this panel visibly re-pop
// ("flashing" every ~0.5s). Read once into the cache; refresh only on panel entry
// or when the user changes a default (defaults_invalidate_cache()).
#define DEF_CACHE_MAX 16
static char g_def_cur[DEF_CACHE_MAX][80];
static int  g_def_cached = 0;
static void defaults_invalidate_cache(void) { g_def_cached = 0; }
static void defaults_refresh_cache(void) {
    int n; assoc_categories(&n);
    if (n > DEF_CACHE_MAX) n = DEF_CACHE_MAX;
    for (int i = 0; i < n; i++)
        assoc_category_current(i, g_def_cur[i], sizeof(g_def_cur[i]));
    g_def_cached = 1;
}
static void draw_defaults_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    draw_section_header(x, y, "Default Apps");
    y += 36;
    draw_hint(x, y, "Choose which app opens each file type (saved system-wide to /ASSOC.CFG).");
    y += 28;
    int n; const assoc_category_t *cats = assoc_categories(&n);
    if (!g_def_cached) defaults_refresh_cache();
    for (int i = 0; i < n; i++) {
        const char *cur = (i < DEF_CACHE_MAX) ? g_def_cur[i] : "";
        const char *base = cur; for (const char *p = cur; *p; p++) if (*p == '/') base = p + 1;
        draw_label(x, y + 6, cats[i].label);
        win_draw_rect(window_handle, x + 130, y, 200, 28, COL_INPUT_BG);
        gui_draw_rect_outline(window_handle, x + 130, y, 200, 28, COL_INPUT_BORDER);
        win_draw_text(window_handle, x + 140, y + 7, base, COL_TEXT_PRIMARY);
        if (cats[i].napps > 1) {
            focus_add(x + 345, y, 90, 28, 0);
            draw_button_small(x + 345, y + 2, 90, "Change", false);
        }
        y += 40;
    }
    g_content_bottom_y = y;   // (#227)
}

// About-tab logo: /MAYLOGO.DAT = u16 w, u16 h (LE), then w*h RGBA bytes
// (from maytera-logo.png). Alpha-composited over the card colour so it reads on
// any theme. Replaces the old "M/OS" placeholder box.
static unsigned char s_logo[80 * 80 * 4];
static int s_logo_w = 0, s_logo_h = 0, s_logo_state = -1; // -1 untried, 0 fail, 1 ok
// =============================================================================
// Credits (#745, local queue item 71)
// =============================================================================
// WHY THIS EXISTS. TinyGL's licence (userland/libgl/src/LICENSE) is a MODIFIED
// zlib licence. Clause 1 reads "If you use this software in a product, an
// acknowledgment in the product and its documentation *is* required" (upstream's
// own emphasis), where stock zlib says "would be appreciated but is not
// required". ATTRIBUTION.md satisfies the documentation half; this screen is the
// product half. The CC BY 4.0 icon sets (CoreUI Icons Free, Boxicons) and the
// CC BY SVG Repo icons create the same in-product attribution obligation, which
// a file in the source tree does not discharge for a user who only ever runs
// the binary.
//
// THE LIST IS NOT IN THIS FILE, ON PURPOSE. It is read from /CONFIG/CREDITS.DAT,
// which build/build-golden.sh generates from ATTRIBUTION.md on every build via
// tools/attribution/gen-credits.py. A credits list typed into an app would be a
// second source of truth for what we ship, and a divergent second copy is this
// project's most repeated failure (two g_wallpapers[] arrays, two Task Managers,
// two Settings implementations). This app parses nothing but "<style>|<text>":
// adding a component to ATTRIBUTION.md needs no change here and no rebuild of
// this binary.
#define CREDITS_PATH    "/CONFIG/CREDITS.DAT"
#define CREDITS_BUF     12288
#define CREDITS_MAXLN   400
#define CREDITS_LINE_H  18

// y of the About panel's button row, PUBLISHED BY THE DRAW and read by the
// hit-test (#745, local queue item 71).
//
// THE BUG THIS ENDS. handle_content_click() computed the row as
// "base_y + 40 + 115 + 155 + 25 + 25 + 35", a hand-added chain of the panel's
// section heights. draw_about_panel() then grew a Features block of eight text
// lines (7 x 18px + 35px), which moved the real buttons 126px down and left the
// chain untouched. Measured on a booted VM at build 1867: the buttons draw at
// y=541 and the hit-test accepted only y=415..445, so Check Updates, Export
// Debug and Credits were all DEAD, by mouse and by keyboard alike. Nothing
// reported it because a button that does nothing looks exactly like a button
// nobody pressed.
//
// Correcting the literal would only reset the clock: the next line added to
// Features breaks it again, silently, in the same way. So the draw now records
// where it actually put the row and the hit-test reads that, which is the same
// discipline modal_dh()/modal_dw() and sidebar_row_y() already use. 0 means
// "this panel has not been drawn yet", and the hit-test then matches nothing.
static int   g_about_btn_y = 0;

static char  credits_buf[CREDITS_BUF];
static char *credits_text[CREDITS_MAXLN];
static char  credits_style[CREDITS_MAXLN];
static int   credits_n = 0;
static int   credits_loaded = 0;
static int   credits_truncated = 0;
static gui_scroll_t g_credits_scroll;   // shared primitive; see gui_scroll.h

// The ONLY hardcoded acknowledgment in this file, and why it is allowed to be
// here: if /CONFIG/CREDITS.DAT is missing (an image assembled without the
// generator, or an older image), a Credits screen showing nothing would fail
// the very obligation it was built to meet. gen-credits.py refuses to generate
// output that has no TinyGL entry, so the generated list and this fallback
// cannot disagree about whether TinyGL ships. Everything else is deliberately
// absent from the fallback rather than half-copied.
static const char *const CREDITS_FALLBACK[] = {
    "T|MayteraOS",
    "B|Attribution data was not installed on this image",
    "B|(/CONFIG/CREDITS.DAT is missing). The full text is",
    "B|ATTRIBUTION.md in the MayteraOS source distribution.",
    "R|",
    "N|TinyGL",
    "L|zlib-style licence, in-product acknowledgment required",
    "D|(C) 1997-2021 Fabrice Bellard, Gek (DMHSW), C-Chads",
};

// Register one display record. A line this does not understand is IGNORED
// rather than guessed at: a malformed record must not be able to render as a
// component with the wrong licence next to it.
static void credits_add(char *line) {
    if (credits_n >= CREDITS_MAXLN) { credits_truncated = 1; return; }
    if (!line[0] || line[0] == '#') return;
    if (line[1] != '|') return;
    credits_style[credits_n] = line[0];
    credits_text[credits_n]  = line + 2;
    credits_n++;
}

static void credits_load(void) {
    if (credits_loaded) return;
    credits_loaded = 1;
    credits_n = 0;
    credits_truncated = 0;

    long total = 0;
    int fd = sys_open(CREDITS_PATH, 0);
    if (fd >= 0) {
        long n;
        // Loop rather than assume one read returns the file: this is a plain
        // file read, not a wait, so there is no wait-queue question here.
        while (total < (long)sizeof(credits_buf) - 1 &&
               (n = sys_read(fd, credits_buf + total,
                             (long)sizeof(credits_buf) - 1 - total)) > 0)
            total += n;
        sys_close(fd);
        if (total >= (long)sizeof(credits_buf) - 1) credits_truncated = 1;
    }
    if (total <= 0) {
        // Same buffer, same splitter, one code path. Copying the fallback in
        // rather than pointing at the const strings keeps credits_text[]
        // uniformly owned by credits_buf.
        for (unsigned i = 0; i < ARRAY_COUNT(CREDITS_FALLBACK); i++) {
            const char *q = CREDITS_FALLBACK[i];
            while (*q && total < (long)sizeof(credits_buf) - 2)
                credits_buf[total++] = *q++;
            credits_buf[total++] = '\n';
        }
    }
    credits_buf[total] = 0;

    char *q = credits_buf;
    while (*q) {
        char *e = q;
        while (*e && *e != '\n') e++;
        int last = (*e == 0);
        *e = 0;
        credits_add(q);
        if (last) break;
        q = e + 1;
    }
}

// Every ink drawn on the About/Credits surface is floored through the SHARED
// gui_ensure_contrast(), instead of being trusted to pass. The screen this
// replaces is exactly why: it drew its heading in COL_ACCENT and two of its
// five lines in COL_TEXT_SECONDARY / COL_TEXT_DISABLED, which against
// COL_CONTENT_BG on retro_unix (THEME.CFG active=retro_unix, the theme the
// machine ships on) measure 1.42:1 and 1.90:1. Flooring here rather than
// picking per-theme literals means a theme added later cannot reintroduce the
// same defect: it is a property of the code, not of a review.
//
// THE FLOOR IS 5:1, NOT THE 4.5:1 MINIMUM. gui_ensure_contrast() walks toward
// black or white in 8/255 steps and returns the FIRST step that clears, so a
// floor of 450 lands at 4.50 to 4.74:1 on every theme: a pass with no headroom,
// the state blame.md warns about. 500 costs one or two extra mix steps, moves
// the theme's hue no further than that, and leaves real margin. Measure any
// change to this number with tools/attribution/credits-contrast.sh, which reads
// the constant out of this very line so the two cannot drift apart.
#define CREDITS_CONTRAST_X100 500
static uint32_t credits_ink(uint32_t want) {
    return gui_ensure_contrast(want, COL_CONTENT_BG, CREDITS_CONTRAST_X100);
}

static int draw_about_logo(int dx, int dy_card_top, int card_h) {
    if (s_logo_state < 0) {
        s_logo_state = 0;
        int fd = sys_open("/MAYLOGO.DAT", 0);
        if (fd >= 0) {
            unsigned char hd[4];
            if (sys_read(fd, hd, 4) == 4) {
                int w = hd[0] | (hd[1] << 8), h = hd[2] | (hd[3] << 8);
                if (w > 0 && h > 0 && w <= 80 && h <= 80) {
                    int need = w * h * 4;
                    if (sys_read(fd, s_logo, need) == need) {
                        s_logo_w = w; s_logo_h = h; s_logo_state = 1;
                    }
                }
            }
            sys_close(fd);
        }
    }
    if (s_logo_state != 1) return 0;
    uint32_t bgc = COL_CARD_BG;
    int br = (bgc >> 16) & 0xFF, bgg = (bgc >> 8) & 0xFF, bb = bgc & 0xFF;
    int oy = dy_card_top + (card_h - s_logo_h) / 2;   // vertically center in card
    for (int yy = 0; yy < s_logo_h; yy++) {
        for (int xx = 0; xx < s_logo_w; xx++) {
            unsigned char *p = s_logo + (yy * s_logo_w + xx) * 4;
            int a = p[3];
            if (a < 8) continue;   // transparent
            int r = (p[0] * a + br  * (255 - a)) / 255;
            int g = (p[1] * a + bgg * (255 - a)) / 255;
            int b = (p[2] * a + bb  * (255 - a)) / 255;
            gui_draw_pixel(window_handle, dx + xx, oy + yy,
                           (uint32_t)((r << 16) | (g << 8) | b));
        }
    }
    return 1;
}

static void draw_notifications_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    char buf[40];

    draw_section_header(x, y, "Alerts"); y += 40;
    draw_toggle_labeled(x, y, 220, "Enable notifications", alerts_enabled != 0); y += 44;

    draw_subsection(x, y, "Show toasts for these severities"); y += 28;
    draw_checkbox(x, y, "Information", alerts_info != 0);    y += 26;
    draw_checkbox(x, y, "Success",     alerts_success != 0); y += 26;
    draw_checkbox(x, y, "Warning",     alerts_warning != 0); y += 26;
    draw_checkbox(x, y, "Error",       alerts_error != 0);   y += 38;

    draw_label(x, y, "Toast duration");
    gui_itoa(alerts_duration, buf, sizeof(buf));
    { int l = my_strlen(buf); buf[l++] = 's'; buf[l] = 0; }
    draw_slider(x + 140, y, 240, alerts_duration, 20, COL_ACCENT);
    win_draw_text(window_handle, x + 395, y, buf, COL_TEXT_SECONDARY); y += 44;

    draw_toggle_labeled(x, y, 220, "Do not disturb", alerts_dnd != 0); y += 40;
    draw_hint_ic(x, y, alerts_dnd ? "CMINUS" : "INFO", COL_TEXT_SECONDARY,
                 alerts_dnd ? "Do Not Disturb: toasts hidden, still logged in the bell"
                            : "Toasts slide in top-right, stack, and auto-dismiss");
    g_content_bottom_y = y + 20;   // (#227)
}

// (#: Start Menu uplift) Layout, Favorites/Recent visibility, search-focus,
// menu width and item icon size. Favorites/Recents CONTENT is managed from the
// Start Menu itself (right-click an item -> Pin/Unpin, or launch it to grow
// Recents) - this panel only controls whether those sections show and how
// many recents to keep, never the app.
static void draw_startmenu_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    char buf[16];

    draw_section_header(x, y, "Start Menu"); y += 40;

    draw_label(x, y, "Layout");
    draw_dropdown_n(x + 120, y - 3, 160, SM_VIEW_OPTS[OPT_CLAMP(sm_view, SM_VIEW_OPTS)],
                     g_dd_open && g_dd_sel == &sm_view, 2);
    y += 40;

    draw_toggle_labeled(x, y, 220, "Show Favorites section", sm_show_fav); y += 40;
    draw_toggle_labeled(x, y, 220, "Show Recent section", sm_show_recent); y += 40;

    draw_label(x, y, "Recent items to show");
    gui_itoa(sm_recent_count, buf, sizeof(buf));
    draw_slider(x + 180, y, 160, sm_recent_count - 1, 9, COL_ACCENT);
    win_draw_text(window_handle, x + 350, y, buf, COL_TEXT_SECONDARY);
    y += 40;

    draw_toggle_labeled(x, y, 260, "Focus search box when menu opens", sm_focus_search); y += 40;

    draw_label(x, y, "Menu width");
    gui_itoa(sm_width, buf, sizeof(buf));
    draw_slider(x + 180, y, 160, sm_width - 220, 200, COL_ACCENT);
    win_draw_text(window_handle, x + 350, y, buf, COL_TEXT_SECONDARY);
    y += 40;

    draw_label(x, y, "Item icon size");
    gui_itoa(sm_icon_size, buf, sizeof(buf));
    draw_slider(x + 180, y, 160, sm_icon_size - 14, 14, COL_ACCENT);
    win_draw_text(window_handle, x + 350, y, buf, COL_TEXT_SECONDARY);
    y += 44;

    draw_hint_ic(x, y, "INFO", COL_TEXT_SECONDARY,
                 "Favorites are also the dock's pinned apps; manage them from the Start "
                 "Menu itself (right-click an item to Pin/Unpin) or from Settings > Dock. "
                 "Recent items track automatically as you launch apps. A Grid layout and "
                 "a custom menu screen position are not implemented yet.");
    g_content_bottom_y = y + 40;   // (#227) multi-line hint
}

// =============================================================================
// Panel: Dock (#745 task #67 "dockpanel")
// =============================================================================
//
// USER-REPORTED GAP THIS PANEL CLOSES: the first-boot wizard offers a dock
// style, but writes it into UIPROFIL.YML via the compositor (see below) and
// has a Skip button - a user who skips, or who picks one style then later
// changes their mind, previously had NO WAY BACK: nothing in Settings could
// touch dock style, opacity, or WHICH apps are pinned. This panel is that
// way back, covering both BEHAVIOUR (style, opacity - moved here from
// Appearance, see its own "#745 task #67" comment) and CONTENTS (pinned
// favourites: view, add, remove).
//
// ONE WRITER FOR THE FAVOURITES LIST. startmenu.c (compositor) owns
// <home>/CONFIG/STARTMENU.CFG (#236: per-user path, was root-owned
// /CONFIG/STARTMENU.CFG - see startmenu.c's sm_save_state() for why that
// never reached disk under a uid-1000 session) and is the ONLY thing that
// ever writes it (sm_save_state()); the dock's own right-click Pin/Unpin
// (#44) and the wizard's apps page both drive that SAME writer, one straight
// (same process, direct call), one through a channel (FAVCH.CFG, #745 task
// #63/P1). Settings is a SEPARATE PROCESS and cannot call sm_save_state()
// directly, so this panel is a THIRD caller of the SAME FAVCH.CFG channel,
// never a second writer of STARTMENU.CFG. Read side: this panel parses
// STARTMENU.CFG's "FAV|<path>" lines directly (read-only) to display what
// is actually pinned, the same file format sm_save_state() writes, never a
// second on-disk copy of the list. #236: this read went through the SAME
// userconf per-user resolution as the compositor's own reader, or this panel
// would show "No apps are pinned" while the taskbar/Start menu correctly show
// what is pinned - exactly the divergence #236 reported, because this used
// to read the (permanently empty, for a uid-1000 session) root-owned path
// while startmenu.c's in-memory list came from FAVCH.CFG live-apply.
//
// FAVCH.CFG WAS ADD-ONLY BEFORE THIS CHANGE (dockfav channel, startmenu.c
// sm_load_favs_channel()): a line was "pin this path if not already
// pinned", by design (see its own header comment - toggle semantics would
// have unpinned an app the user pinned by hand before the wizard ran). A
// Settings panel needs REMOVE too, so sm_load_favs_channel() gained a
// leading '-' prefix meaning "unpin this path if currently pinned" (a
// no-op otherwise, never a toggle here either - this panel already knows
// whether a row is pinned before it offers a Remove button on it). See
// startmenu.c for the exact diff; reported to the compositor owner rather
// than folded into their concurrent main.c work, per this ticket's scope.
//
// CONTENTS CATALOG: "which apps could I pin" is read from the SAME
// data-driven source the compositor itself merges into the Start Menu -
// build/assets/startmenu/system.d/*.MENU fragments, shipped at
// /CONFIG/STARTMENU/SYSTEM.D/*.MENU (or /ext2/... on the ext2-root golden),
// PLUS the session user's own <home>/CONFIG/STARTMENU/*.MENU (App Store
// installs, startmenu_reg.c). This is a THIRD reader of that data, never a
// hand-maintained duplicate app list: a golden that ships without one of
// these apps, or that gains a new one via the App Store, is reflected here
// with no code change, the same defensive shape sm_seed_default_favorites()
// already uses against a missing shipped app.
//
// A CONTROL THAT DOES NOTHING IS WORSE THAN NO CONTROL. Reordering pinned
// items (drag-to-reorder) is NOT offered here: startmenu.c's favourites
// list has no reorder primitive (FAVCH.CFG only adds/removes by path, and
// the dock always renders g_fav_paths in array order), so a drag handle
// would be pure decoration. Renaming a pinned item's dock label is not
// offered either: the dock draws whatever name resolves from
// g_menu_items/g_menu_items-equivalent lookup, not a per-favourite label,
// so a rename control here would have nothing to write.

// Must equal startmenu.c's MAX_FAVORITES. Settings cannot #include
// compositor.h (its unguarded `typedef int bool` collides with
// libc/types.h, see gui_dock.h's own comment on the same class of problem),
// so this is a matching literal, not a shared constant - same tradeoff
// gui_dock.h documents for GUI_DOCK_COUNT, without that file's build-time
// gate (out of scope to add one for a single int here; if this ever drifts
// the failure mode is cosmetic - a few pinned apps invisible in this list -
// not a crash, unlike the PANEL_EXTSVC-class array-length bugs above).
// (#123 item 1) 12 -> 28, per the user's ask. MUST match the compositor's
// startmenu.c MAX_FAVORITES / taskbar.c XFCE_MAX_FAVS: this app only writes a
// pin CHANNEL (FAVCH.CFG), the compositor owns the list, so a larger number
// here would let the user queue pins the compositor then silently drops.
#define DOCKFAV_MAX 28

// ---- Contents: catalog of pinnable apps, read from the *.MENU fragments ----
#define DOCKFAV_CATALOG_MAX 80
typedef struct { char name[40]; char path[128]; } dockfav_cat_t;
static dockfav_cat_t g_dockfav_catalog[DOCKFAV_CATALOG_MAX];
static int g_dockfav_catalog_n = 0;

static void dockfav_catalog_add(const char *name, const char *path) {
    for (int i = 0; i < g_dockfav_catalog_n; i++)
        if (!strcmp(g_dockfav_catalog[i].path, path)) return;   // de-dup by path
    if (g_dockfav_catalog_n >= DOCKFAV_CATALOG_MAX) return;      // silently drop past the cap
    copy_str(g_dockfav_catalog[g_dockfav_catalog_n].name, name, sizeof(g_dockfav_catalog[0].name));
    copy_str(g_dockfav_catalog[g_dockfav_catalog_n].path, path, sizeof(g_dockfav_catalog[0].path));
    g_dockfav_catalog_n++;
}

// Parse one *.MENU fragment's text (already read into a NUL-terminated
// buffer) for "item: <name> | <path> | icon=... [| type=...]" lines. Ignores
// "category:" headers, blank lines and "#" comments - the same grammar
// startmenu_model.rs documents, read only for the two fields this panel
// needs. A leading/trailing space around a field is trimmed; a malformed
// line (no '|') is skipped, not fatal.
static void dockfav_parse_menu_text(char *text) {
    char *p = text;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n') p++;
        if (*p) { *p = 0; p++; }
        char *t = line;
        while (*t == ' ' || *t == '\t') t++;
        if (t[0] == 0 || t[0] == '#') continue;
        if (t[0] != 'i' || strncmp(t, "item:", 5) != 0) continue;

        char *f1 = t + 5;
        char *bar1 = strchr(f1, '|');
        if (!bar1) continue;
        *bar1 = 0;
        char *f2 = bar1 + 1;
        char *bar2 = strchr(f2, '|');
        if (bar2) *bar2 = 0;   // drop icon=/type= fields, not needed here

        // Trim both fields in place.
        char *ne = f1 + strlen(f1); while (ne > f1 && (ne[-1] == ' ' || ne[-1] == '\t')) *--ne = 0;
        while (*f1 == ' ' || *f1 == '\t') f1++;
        char *pe = f2 + strlen(f2); while (pe > f2 && (pe[-1] == ' ' || pe[-1] == '\t')) *--pe = 0;
        while (*f2 == ' ' || *f2 == '\t') f2++;

        if (f1[0] && f2[0]) dockfav_catalog_add(f1, f2);
    }
}

// Read every *.MENU file directly inside `dir` (non-recursive, same
// dir_size_bytes()-style readdir loop already used elsewhere in this file)
// and feed it to the parser above. Suffix match is case-insensitive: the
// repo source is lowercase ("00-internet.MENU"); FAT upper-cases on write,
// ext2 preserves exactly what shipped.
static void dockfav_feed_dir(const char *dir) {
    dirent_t e;
    for (int i = 0; i < 64; i++) {
        if (sys_readdir(dir, i, &e) != 0) break;
        if (DIRENT_IS_DIR(e)) continue;
        int L = 0; while (e.name[L]) L++;
        if (L < 6) continue;
        const char *s = e.name + L - 5;
        int ok = (s[0] == '.') &&
                 (s[1] == 'M' || s[1] == 'm') && (s[2] == 'E' || s[2] == 'e') &&
                 (s[3] == 'N' || s[3] == 'n') && (s[4] == 'U' || s[4] == 'u');
        if (!ok) continue;

        char full[300]; int k = 0;
        for (int j = 0; dir[j] && k < 255; j++) full[k++] = dir[j];
        if (k && full[k-1] != '/') full[k++] = '/';
        for (int j = 0; e.name[j] && k < 299; j++) full[k++] = e.name[j];
        full[k] = 0;

        int fd = sys_open(full, 0);
        if (fd < 0) continue;
        static char text[8192];
        long n = sys_read(fd, text, sizeof(text) - 1);
        sys_close(fd);
        if (n <= 0) continue;
        text[n] = 0;
        dockfav_parse_menu_text(text);
    }
}

// Same three directories startmenu.c's sm_feed_system_layer()/
// sm_feed_user_layer() read, in the same order: the all-users system layer
// (build-time + App-Store-at-install-time), then the session user's own
// per-user layer (an unprivileged App Store install), so a self-installed
// app is offered for pinning exactly like a built-in one.
static void dockfav_catalog_build(void) {
    g_dockfav_catalog_n = 0;
    dockfav_feed_dir("/CONFIG/STARTMENU/SYSTEM.D");
    dockfav_feed_dir("/ext2/CONFIG/STARTMENU/SYSTEM.D");
    {
        char hdir[192];
        if (userhome_path("CONFIG", "STARTMENU", hdir, sizeof(hdir)) == 0)
            dockfav_feed_dir(hdir);
    }
}

// ---- Contents: currently-pinned favourites, read-only mirror of STARTMENU.CFG ----
static char g_dockfav_pinned[DOCKFAV_MAX][128];
static int  g_dockfav_pinned_n = 0;
static int  g_dockfav_cached = 0;

static void dockfav_refresh_pinned(void) {
    g_dockfav_pinned_n = 0;
    // #236: per-user path first, legacy /CONFIG copy as a migration fallback -
    // the SAME two-path read startmenu.c's sm_load_state()/
    // sm_startmenu_cfg_exists() use, so this panel can never disagree with
    // what the compositor actually has pinned.
    int fd = userconf_open_read("STARTMENU.CFG", "/CONFIG/STARTMENU.CFG");
    if (fd >= 0) {
        static char buf[4096];
        long n = sys_read(fd, buf, sizeof(buf) - 1);
        sys_close(fd);
        if (n > 0) {
            buf[n] = 0;
            char *p = buf;
            while (*p && g_dockfav_pinned_n < DOCKFAV_MAX) {
                char *line = p;
                while (*p && *p != '\n') p++;
                if (*p) { *p = 0; p++; }
                if (strncmp(line, "FAV|", 4) == 0) {
                    copy_str(g_dockfav_pinned[g_dockfav_pinned_n], line + 4, sizeof(g_dockfav_pinned[0]));
                    g_dockfav_pinned_n++;
                }
            }
        }
    }
    // #63: an absent STARTMENU.CFG (never configured) and a present-but-empty
    // one (deliberately unpinned everything) both read as g_dockfav_pinned_n
    // == 0 here, which is CORRECT for display purposes - this panel only
    // ever shows what is actually pinned, right now, and never re-seeds
    // defaults itself (that stays sm_seed_default_favorites()'s job, in the
    // one process that owns the write). Nothing here decides "should
    // defaults exist"; it only ever reflects reality.
    g_dockfav_cached = 1;
}

static void dockfav_invalidate_cache(void) { g_dockfav_cached = 0; }

static int dockfav_is_pinned(const char *path) {
    for (int i = 0; i < g_dockfav_pinned_n; i++)
        if (!strcmp(g_dockfav_pinned[i], path)) return 1;
    return 0;
}

// Display name for a pinned path: looked up in the catalog; a path that
// resolves to no known catalog entry (a renamed/removed app - the dock
// itself would also silently skip such a favourite, see startmenu.c's own
// comment on that) falls back to showing the raw path so the row is never
// blank.
static const char *dockfav_name_for_path(const char *path) {
    for (int i = 0; i < g_dockfav_catalog_n; i++)
        if (!strcmp(g_dockfav_catalog[i].path, path)) return g_dockfav_catalog[i].name;
    return path;
}

// ---- Contents: "Add to dock" candidates = catalog minus already-pinned ----
static char g_dockfav_cand_name[DOCKFAV_CATALOG_MAX][40];
static char g_dockfav_cand_path[DOCKFAV_CATALOG_MAX][128];
static int  g_dockfav_cand_n = 0;
// index 0 is always the sentinel "Choose an app..."; indices 1..n map to
// g_dockfav_cand_name/path[i-1]. A stateless action list, not a persisted
// selection - every successful add resets this back to 0.
static int  g_dockfav_add_sel = 0;
static const char *g_dockfav_dd_items[DOCKFAV_CATALOG_MAX + 1];

static void dockfav_rebuild_candidates(void) {
    g_dockfav_cand_n = 0;
    for (int i = 0; i < g_dockfav_catalog_n; i++) {
        if (dockfav_is_pinned(g_dockfav_catalog[i].path)) continue;
        if (g_dockfav_cand_n >= DOCKFAV_CATALOG_MAX) break;
        copy_str(g_dockfav_cand_name[g_dockfav_cand_n], g_dockfav_catalog[i].name, sizeof(g_dockfav_cand_name[0]));
        copy_str(g_dockfav_cand_path[g_dockfav_cand_n], g_dockfav_catalog[i].path, sizeof(g_dockfav_cand_path[0]));
        g_dockfav_cand_n++;
    }
    g_dockfav_dd_items[0] = "Choose an app...";
    for (int i = 0; i < g_dockfav_cand_n; i++) g_dockfav_dd_items[i + 1] = g_dockfav_cand_name[i];
    if (g_dockfav_add_sel > g_dockfav_cand_n) g_dockfav_add_sel = 0;
}

static void dockfav_refresh_all(void) {
    dockfav_catalog_build();
    dockfav_refresh_pinned();
    dockfav_rebuild_candidates();
    g_dockfav_add_sel = 0;
}

// ---- Contents: writing through the FAVCH.CFG live-apply channel ----
// userconf_open_write() always O_TRUNCs (see userconf.c), so a plain write
// would destroy any line the compositor's ~1s poll has not yet consumed.
// Read-modify-write against whatever is already queued instead - the same
// shape as every other read-then-append config edit in this file, applied
// to a channel a live sibling process also reads.
static void dockfav_write_channel_line(const char *line) {
    char buf[2048];
    int n = 0;
    int rfd = userconf_open_read("FAVCH.CFG", 0);   // no legacy: brand-new channel (startmenu.c)
    if (rfd >= 0) {
        long rn = sys_read(rfd, buf, sizeof(buf) - 130);
        sys_close(rfd);
        if (rn > 0) n = (int)rn;
    }
    int o = n;
    for (const char *s = line; *s && o < (int)sizeof(buf) - 2; s++) buf[o++] = *s;
    buf[o++] = '\n';
    int fd = userconf_open_write("FAVCH.CFG");
    if (fd < 0) { save_failed("FAVCH.CFG"); return; }
    if (userconf_finish_write(fd, buf, (unsigned long)o) != 0) save_failed("FAVCH.CFG");
}

// Pin `path`. Optimistically updates the local display list too, rather
// than waiting up to ~1s for the compositor's poll and this panel's own
// next refresh - see the panel-open poll below for the eventual
// reconciliation against the real, compositor-owned list.
static void dockfav_add(const char *path) {
    dockfav_write_channel_line(path);
    if (!dockfav_is_pinned(path) && g_dockfav_pinned_n < DOCKFAV_MAX) {
        copy_str(g_dockfav_pinned[g_dockfav_pinned_n], path, sizeof(g_dockfav_pinned[0]));
        g_dockfav_pinned_n++;
    }
    dockfav_rebuild_candidates();
}

// Unpin `path`. The '-' prefix is the removal half of the FAVCH.CFG channel
// added for this panel; see the panel header comment and startmenu.c.
static void dockfav_remove(const char *path) {
    char line[130];
    line[0] = '-';
    copy_str(line + 1, path, sizeof(line) - 1);
    dockfav_write_channel_line(line);
    for (int i = 0; i < g_dockfav_pinned_n; i++) {
        if (!strcmp(g_dockfav_pinned[i], path)) {
            for (int j = i; j < g_dockfav_pinned_n - 1; j++)
                copy_str(g_dockfav_pinned[j], g_dockfav_pinned[j + 1], sizeof(g_dockfav_pinned[0]));
            g_dockfav_pinned_n--;
            break;
        }
    }
    dockfav_rebuild_candidates();
}

static void dockfav_add_dd_changed(void) {
    if (g_dockfav_add_sel > 0 && g_dockfav_add_sel <= g_dockfav_cand_n)
        dockfav_add(g_dockfav_cand_path[g_dockfav_add_sel - 1]);
    g_dockfav_add_sel = 0;
    draw_all();
}

// Shared geometry: EVERY y-coordinate the draw function and the click
// handler both need to agree on comes from here, computed once. This is
// the fix for the exact "two arithmetic chains, one column" hit-box-drift
// bug class already recorded in blame.md for this file's Mouse panel -
// Contents has a variable number of rows (0..DOCKFAV_MAX), which is exactly
// the shape that bites hardest when geometry is hand-duplicated.
typedef struct {
    int style_y;      // Dock Style dropdown row
    int opacity_y;     // Dock Opacity slider row
    int height_y;      // (#123) Dock Height slider row
    int zoom_y;        // (#123) Hover Zoom slider row
    int contents_y;    // "Contents" subsection header
    int hint_y;        // hint line under the subsection header
    int row0_y;         // first pinned-item row
    int row_h;           // pinned-item row height
    int col_w;            // (#123) width of ONE pinned-item column
    int cols;              // (#123) number of pinned-item columns (1 or 2)
    int rows;               // (#123) pinned rows actually used (ceil(n/cols))
    int add_y;               // "Add app" dropdown row (only meaningful if !full && has_candidates)
    int full;                 // dock is at DOCKFAV_MAX: no Add row
    int has_candidates;        // catalog has at least one un-pinned app: only meaningful if !full
} dock_layout_t;

// (#123) Screen y of pinned item `i`, and its column x offset. ONE function,
// so the draw loop and the Remove-button hit test cannot disagree about which
// cell an item is in. Column-major (fill column 0 top-to-bottom, then column
// 1) so the visual order still reads down the page like the old single list.
static void dock_pin_cell(const dock_layout_t *L, int i, int *cx, int *cy) {
    int col = (L->cols > 1) ? (i / L->rows) : 0;
    int row = (L->cols > 1) ? (i % L->rows) : i;
    if (cx) *cx = col * (L->col_w + 12);
    if (cy) *cy = L->row0_y + row * L->row_h;
}

static dock_layout_t dock_panel_layout(void) {
    dock_layout_t L;
    int y = PADDING + 40 - g_content_scroll.offset;   // (#227) content area now scrolls; after "Dock" section header (y += 40)
    y += 25;                 // after "Behaviour" subsection header (y += 25)
    L.style_y = y; y += 45;
    L.opacity_y = y; y += 45;
    // (#123) The two new sliders sit at a 40px pitch rather than the 45 the
    // rows above use. This is not cosmetic: the Settings content area does NOT
    // scroll (content_scroll_y exists but max_scroll_y is never set to anything
    // but 0, so the wheel handler clamps every scroll to zero - MEASURED, see
    // the CHANGELOG entry), so everything this panel draws must fit inside
    // WIN_HEIGHT or it is simply unreachable. Two extra 45px rows plus a
    // 28-item pin list did not fit at 1280x800; 40px rows plus the two-column
    // pin grid below do, with room to spare.
    L.height_y = y; y += 40;
    L.zoom_y = y; y += 40;
    y += 12;                  // breathing room before the next subsection
    L.contents_y = y; y += 25;
    L.hint_y = y; y += 24;
    L.row0_y = y;
    L.row_h = 26;
    // (#123 item 1) TWO COLUMNS once the list is long enough to need them. At
    // the old 12-item cap a single 30px column was 360px and always fitted; at
    // 28 it is 840px, which overflows the window on every supported display.
    // The threshold is derived from the space actually left, not picked: fill
    // one column until it would run past the Add row's own space, then split.
    {
        int avail = WIN_HEIGHT - L.row0_y - 60;      // leave room for the Add row
        int per_col = avail / L.row_h;
        if (per_col < 1) per_col = 1;
        if (g_dockfav_pinned_n > per_col) {
            L.cols = 2;
            L.rows = (g_dockfav_pinned_n + 1) / 2;
            if (L.rows > per_col) L.rows = per_col;   // still clipped only in the absurd case
        } else {
            L.cols = 1;
            L.rows = g_dockfav_pinned_n;
        }
        L.col_w = (L.cols > 1) ? ((CONTENT_WIDTH - 2 * PADDING - 12) / 2)
                               : (CONTENT_WIDTH - 2 * PADDING);
    }
    y += L.rows * L.row_h;
    y += 8;
    L.full = (g_dockfav_pinned_n >= DOCKFAV_MAX);
    L.has_candidates = (g_dockfav_cand_n > 0);
    L.add_y = y;
    return L;
}

static void draw_dock_panel(void) {
    int x = CONTENT_X + PADDING;
    char buf[8];

    if (!g_dockfav_cached) dockfav_refresh_all();
    dock_layout_t L = dock_panel_layout();

    draw_section_header(x, PADDING - g_content_scroll.offset, "Dock");

    // ---- Behaviour ----
    draw_subsection(x, PADDING + 40 - g_content_scroll.offset, "Behaviour");

    draw_label(x, L.style_y + 4, "Style");
    draw_dropdown_n(x + 120, L.style_y - 3, 220, DOCK_OPTS[DOCK_CLAMP(dock_style)],
                     g_dd_open && g_dd_sel == &dock_style, DOCK_OPTS_COUNT);

    draw_label(x, L.opacity_y + 4, "Opacity");
    {
        if (dock_opacity < DOCK_OPACITY_MIN) dock_opacity = DOCK_OPACITY_MIN;
        if (dock_opacity > DOCK_OPACITY_MAX) dock_opacity = DOCK_OPACITY_MAX;
        gui_itoa(dock_opacity, buf, sizeof(buf));
        int len = my_strlen(buf);
        buf[len++] = '%'; buf[len] = 0;
        draw_slider(x + 120, L.opacity_y, 170, dock_opacity, 100, COL_SLIDER_FILL);
        // (#132) This used to be a DISALLOWED span the knob could not enter at
        // all (the hatch WAS the clamp, drawn from 0 to the old hard floor).
        // The owner's standing decision replaced the clamp with a warning: the
        // knob can now go all the way down to DOCK_OPACITY_MIN, and this hatch
        // is only a heads-up for the DOCK_OPACITY_WARN..MIN span where chrome
        // text contrast is no longer guaranteed on every theme - it is drawn,
        // not enforced, exactly so a user who wants a very transparent dock
        // can still drag straight through it.
        {
            int tx = x + 120, tw = 170 * DOCK_OPACITY_WARN / 100;
            for (int i = 0; i < tw; i += 6) {
                int seg = (tw - i) < 3 ? (tw - i) : 3;
                win_draw_rect(window_handle, tx + i,     L.opacity_y + 5, seg, 6, 0x00E0B34D);
                int seg2 = (tw - i - 3) < 3 ? (tw - i - 3) : 3;
                if (seg2 > 0)
                    win_draw_rect(window_handle, tx + i + 3, L.opacity_y + 5, seg2, 6, 0x00D4A63E);
            }
        }
        win_draw_text(window_handle, x + 300, L.opacity_y, buf, COL_TEXT_SECONDARY);
    }
    {
        char hb[96];
        // (#132) Was "Below %d%% chrome labels stop meeting the contrast
        // minimum" tied to the hard floor - it read as a rule, because it was
        // one. Now it is a warning about a value the compositor WILL honour:
        // wording says so explicitly, so a low setting does not look like a
        // bug when the user goes looking for it (that is the exact complaint
        // this ticket exists to fix).
        snprintf(hb, sizeof(hb), "Below %d%%, chrome text contrast is not guaranteed on every theme (value is still applied).",
                 DOCK_OPACITY_WARN);
        win_draw_text(window_handle, x + 120, L.opacity_y + 22, hb, COL_TEXT_SECONDARY);
    }

    // ---- (#123 items 2/3) Marble dock geometry ----
    // Both reuse this panel's EXISTING slider idiom (draw_slider + a value
    // label at x+300), the same one the Opacity row above uses, rather than a
    // new control. They only affect DOCK_MARBLE, so the panel says so instead
    // of leaving a user wondering why the Default bar did not move.
    draw_label(x, L.height_y + 4, "Max height");
    {
        if (dock_height < DOCK_HEIGHT_MIN) dock_height = DOCK_HEIGHT_MIN;
        if (dock_height > DOCK_HEIGHT_MAX) dock_height = DOCK_HEIGHT_MAX;
        gui_itoa(dock_height, buf, sizeof(buf));
        int len = my_strlen(buf);
        buf[len++] = 'p'; buf[len++] = 'x'; buf[len] = 0;
        draw_slider(x + 120, L.height_y, 170, dock_height - DOCK_HEIGHT_MIN,
                    DOCK_HEIGHT_MAX - DOCK_HEIGHT_MIN, COL_SLIDER_FILL);
        win_draw_text(window_handle, x + 300, L.height_y, buf, COL_TEXT_SECONDARY);
    }

    draw_label(x, L.zoom_y + 4, "Hover zoom");
    {
        if (dock_zoom < DOCK_ZOOM_MIN) dock_zoom = DOCK_ZOOM_MIN;
        if (dock_zoom > DOCK_ZOOM_MAX) dock_zoom = DOCK_ZOOM_MAX;
        gui_itoa(dock_zoom, buf, sizeof(buf));
        int len = my_strlen(buf);
        buf[len++] = '%'; buf[len] = 0;
        draw_slider(x + 120, L.zoom_y, 170, dock_zoom - DOCK_ZOOM_MIN,
                    DOCK_ZOOM_MAX - DOCK_ZOOM_MIN, COL_SLIDER_FILL);
        win_draw_text(window_handle, x + 300, L.zoom_y, buf, COL_TEXT_SECONDARY);
    }
    // (#123 auto-scale) The height control is a PREFERRED MAXIMUM, and the user
    // has to be told, or an auto-scaled dock reads as the slider not working.
    // The compositor shrinks below this value (never above it) when the pinned
    // + running item count would make the pane wider than 75% of the screen,
    // and returns to it when the count drops - see taskbar.c's contract comment
    // on XFCE_DOCK_WIDTH_BUDGET_PCT.
    win_draw_text(window_handle, x + 120, L.zoom_y + 20,
                  "Marble dock only. Height is a maximum: the dock shrinks below it",
                  COL_TEXT_SECONDARY);
    win_draw_text(window_handle, x + 120, L.zoom_y + 34,
                  "when the icons would span more than 75% of the screen.",
                  COL_TEXT_SECONDARY);

    // ---- Contents ----
    draw_subsection(x, L.contents_y, "Contents");
    if (g_dockfav_pinned_n == 0) {
        draw_hint(x, L.hint_y, "No apps are pinned to the dock.");
    } else {
        draw_hint(x, L.hint_y, "Apps pinned to the dock. Drag-to-reorder is not supported yet.");
    }

    // (#123) Column-major grid via dock_pin_cell(), the one geometry function
    // the click handler below also calls. An item past L.cols*L.rows would be
    // off the bottom of the window (only reachable on an absurdly short
    // display), so it is not drawn rather than drawn where it cannot be seen.
    for (int i = 0; i < g_dockfav_pinned_n && i < L.cols * L.rows; i++) {
        int cx0 = 0, ry = 0;
        dock_pin_cell(&L, i, &cx0, &ry);
        draw_card(x + cx0, ry, L.col_w, L.row_h - 4);
        win_draw_text(window_handle, x + cx0 + 10, ry + 5,
                      dockfav_name_for_path(g_dockfav_pinned[i]), COL_TEXT_PRIMARY);
        draw_button_small(x + cx0 + L.col_w - 76, ry - 1, 70, "Remove", false);
    }

    if (L.full) {
        char fb[64];
        snprintf(fb, sizeof(fb), "Dock is full (%d/%d). Remove one to add another.",
                 g_dockfav_pinned_n, DOCKFAV_MAX);
        win_draw_text(window_handle, x, L.add_y + 4, fb, COL_TEXT_SECONDARY);
    } else if (!L.has_candidates) {
        win_draw_text(window_handle, x, L.add_y + 4, "All available apps are already pinned.", COL_TEXT_SECONDARY);
    } else {
        draw_label(x, L.add_y + 4, "Add app");
        draw_dropdown_n(x + 120, L.add_y - 3, 220,
                         g_dockfav_dd_items[(g_dockfav_add_sel >= 0 && g_dockfav_add_sel <= g_dockfav_cand_n) ? g_dockfav_add_sel : 0],
                         g_dd_open && g_dd_sel == &g_dockfav_add_sel, g_dockfav_cand_n + 1);
    }
    g_content_bottom_y = L.add_y + 30;   // (#227)
}

static void draw_about_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls

    draw_section_header(x, y, "About MayteraOS");
    y += 40;

    // Logo and title card
    draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 100);

    // Logo (real maytera-logo from /MAYLOGO.DAT; falls back to the M/OS box)
    if (!draw_about_logo(x + 16, y, 100)) {
        win_draw_rect(window_handle, x + 20, y + 20, 60, 60, COL_ACCENT);
        win_draw_text(window_handle, x + 40, y + 35, "M", COL_TEXT_PRIMARY);
        win_draw_text(window_handle, x + 35, y + 55, "OS", COL_TEXT_PRIMARY);
    }

    // Title + live version (queried from the kernel so it never goes stale)
    char vbuf[64]; int vl = get_version(vbuf, sizeof(vbuf));
    if (vl <= 0) { vbuf[0] = '?'; vbuf[1] = 0; }
    char vline[96]; int vn = 0;
    for (const char *q = "Version "; *q; q++) vline[vn++] = *q;
    for (int k = 0; vbuf[k] && vn < (int)sizeof(vline) - 1; k++) vline[vn++] = vbuf[k];
    vline[vn] = 0;
    int tx = x + 110;
    win_draw_text(window_handle, tx, y + 18, "MayteraOS", COL_TEXT_PRIMARY);
    win_draw_text(window_handle, tx, y + 40, vline, COL_ACCENT);
    win_draw_text(window_handle, tx, y + 60, "64-bit UEFI Operating System", COL_TEXT_SECONDARY);
    win_draw_text(window_handle, tx, y + 80, "The First LLM-Native OS", COL_WARNING);

    y += 115;

    // System specifications
    draw_subsection(x, y, "System Specifications");
    y += 25;

    draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 140);

    // #382: every hardware fact below is queried live from the kernel, not baked in.
    hwinfo_load();

    // Processor: real CPUID brand string (leading spaces on some CPUs trimmed).
    char cpu_line[64];
    {
        const char *b = g_sysinfo_ok ? g_sysinfo.cpu_brand : "";
        while (*b == ' ') b++;
        if (!*b) b = (g_sysinfo_ok && g_sysinfo.cpu_vendor[0]) ? g_sysinfo.cpu_vendor : "x86_64 CPU";
        copy_str(cpu_line, b, sizeof(cpu_line));
    }
    // Memory: real total physical RAM from the PMM.
    char mem_line[32];
    if (g_sysinfo_ok) format_size(g_sysinfo.mem_total, mem_line, sizeof(mem_line));
    else copy_str(mem_line, "Unknown", sizeof(mem_line));
    // Display: real framebuffer geometry + depth.
    char disp_line[48];
    {
        fb_info_t fi;
        if (fb_info(&fi) == 0) {
            char a[12], bb[12], cc[8];
            gui_itoa((int)fi.width, a, sizeof(a));
            gui_itoa((int)fi.height, bb, sizeof(bb));
            gui_itoa((int)fi.bpp, cc, sizeof(cc));
            disp_line[0] = 0;
            hw_append(disp_line, sizeof(disp_line), a);
            hw_append(disp_line, sizeof(disp_line), " x ");
            hw_append(disp_line, sizeof(disp_line), bb);
            hw_append(disp_line, sizeof(disp_line), " (");
            hw_append(disp_line, sizeof(disp_line), cc);
            hw_append(disp_line, sizeof(disp_line), "-bit)");
        } else {
            copy_str(disp_line, "Unknown", sizeof(disp_line));
        }
    }
    // Logical core count (real, from SYS_SYSINFO cpu_count).
    char cores_line[24];
    {
        int nc = g_sysinfo_ok ? (int)g_sysinfo.cpu_count : 1;
        char cb[8]; gui_itoa(nc, cb, sizeof(cb));
        cores_line[0] = 0; hw_append(cores_line, sizeof(cores_line), cb);
        hw_append(cores_line, sizeof(cores_line), nc == 1 ? " core" : " cores");
    }
    // Storage: real primary disk model + capacity (filled at startup from the kernel).
    char storage_line[56];
    if (drive_count > 0 && drives[0].total_bytes > 0) {
        char ts[24]; format_size(drives[0].total_bytes, ts, sizeof(ts));
        storage_line[0] = 0;
        if (drives[0].model[0]) {
            hw_append(storage_line, sizeof(storage_line), drives[0].model);
            hw_append(storage_line, sizeof(storage_line), " ");
        }
        hw_append(storage_line, sizeof(storage_line), "(");
        hw_append(storage_line, sizeof(storage_line), ts);
        hw_append(storage_line, sizeof(storage_line), ")");
    } else {
        copy_str(storage_line, "FAT32 + ext2", sizeof(storage_line));
    }

    draw_label_value(x + 15, y + 15, "Processor:", cpu_line, 100);
    draw_label_value(x + 15, y + 35, "Memory:", mem_line, 100);
    draw_label_value(x + 15, y + 55, "Display:", disp_line, 100);
    draw_label_value(x + 15, y + 75, "Graphics:", g_gpu_name, 100);
    draw_label_value(x + 15, y + 95, "Network:", g_nic_name, 100);
    draw_label_value(x + 15, y + 115, "Storage:", storage_line, 100);

    draw_label_value(x + 300, y + 15, "Version:", vbuf, 80);
    draw_label_value(x + 300, y + 35, "Cores:", cores_line, 80);
    draw_label_value(x + 300, y + 55, "Arch:", "x86_64 (AMD64)", 80);
    draw_label_value(x + 300, y + 75, "Boot:", "UEFI", 80);

    y += 155;

    // Features
    draw_subsection(x, y, "Features");
    y += 25;

    win_draw_text(window_handle, x, y, "UEFI boot, Ring0/Ring3 protection, preemptive scheduler, ELF loader", COL_TEXT_SECONDARY);
    y += 18;
    win_draw_text(window_handle, x, y, "POSIX subset: pipes, signals, pty/tty, fork/execve, dup/fcntl, VFS", COL_TEXT_SECONDARY);
    y += 18;
    win_draw_text(window_handle, x, y, "Filesystems: FAT32 (rw, LFN) + ext2 (rw, root); demand paging; kmalloc heap", COL_TEXT_SECONDARY);
    y += 18;
    win_draw_text(window_handle, x, y, "Userland compositor desktop: windows, taskbar, system tray, widgets, pets", COL_TEXT_SECONDARY);
    y += 18;
    win_draw_text(window_handle, x, y, "TCP/IP stack (ARP/IP/UDP/TCP/DHCP/DNS), E1000/VirtIO NICs, remote control", COL_TEXT_SECONDARY);
    y += 18;
    // TLS 1.2 landed alongside 1.3, and the CA trust store now ships as
    // /CONFIG/CACERTS.PEM, which together are what made real HTTPS sites load.
    win_draw_text(window_handle, x, y, "HTTPS: TLS 1.2 + TLS 1.3, HTTP/2, 125-cert CA trust store, DHCP leases", COL_TEXT_SECONDARY);
    y += 18;
    // Report our own Rust honestly: no third-party crates are vendored, so no
    // adopted-crate LOC is being counted here as ours.
    win_draw_text(window_handle, x, y, "Kernel being rewritten incrementally in Rust (no_std, core+alloc, no crates)", COL_TEXT_SECONDARY);
    y += 18;
    win_draw_text(window_handle, x, y, "Apps: terminal, files, editor, browser, RSS, IRC, DOOM, Python; HD Audio; themes", COL_TEXT_SECONDARY);
    y += 35;

    // Actions. Record the row before drawing it: handle_content_click() reads
    // g_about_btn_y rather than recomputing the panel's height, so the two
    // cannot drift apart again (see the declaration for what happened when
    // they could).
    g_about_btn_y = y;
    draw_button(x, y, 130, "Check Updates", true, false);
    draw_button(x + 145, y, 130, "Export Debug", false, false);
    draw_button(x + 290, y, 100, "Credits", false, false);
    y += 35;
    // #230: about_status now has real states from an ACTUAL network check
    // (check_for_updates()) instead of unconditionally printing "up to date"
    // for a button that contacted nothing. 3=update found, 4=check failed -
    // both drawn in a way that cannot be confused with a green "you're fine".
    if (about_status == 1)
        draw_hint_ic(x, y, "CCHECK", theme_color(THEME_COLOR_SUCCESS), g_update_msg);
    else if (about_status == 2)
        draw_hint(x, y, "Debug info written to /DEBUG.TXT");
    else if (about_status == 3)
        draw_hint_ic(x, y, "CMINUS", theme_color(THEME_COLOR_WARNING), g_update_msg);
    else if (about_status == 4)
        draw_hint_ic(x, y, "CMINUS", theme_color(THEME_COLOR_ERROR), g_update_msg);
    else if (about_status == 5)
        draw_hint(x, y, g_update_msg);
    y += 15;

    // Legal. (local 71) Three corrections here, all factual:
    //  1. "Licensed under MIT License" was simply wrong. The kernel statically
    //     links GPLv2 components (libmad, faad2), so the combined MayteraOS
    //     binary is GPLv2-or-later; MIT covers userland/libc/ only. See
    //     ATTRIBUTION.md, "Vendored open-source libraries".
    //  2. COL_TEXT_DISABLED on COL_CONTENT_BG is 1.88:1 on retro_unix, the
    //     shipped default theme. Floored through the shared primitive.
    //  3. TinyGL's licence requires its acknowledgment "in the product", not
    //     "in a dialog the user may choose to open", so it is stated here on
    //     the always-visible panel as well as inside Credits. The wording is
    //     upstream's own copyright line from userland/libgl/src/LICENSE.
    uint32_t legal = credits_ink(COL_TEXT_SECONDARY);
    win_draw_text(window_handle, x, y, "Copyright 2024-2026 MayteraOS Project", legal);
    y += 18;
    win_draw_text(window_handle, x, y, "GPLv2-or-later; userland/libc/ is MIT. See Credits.", legal);
    y += 18;
    win_draw_text(window_handle, x, y,
                  "Includes TinyGL (C) 1997-2021 Fabrice Bellard, Gek (DMHSW), C-Chads", legal);
    g_content_bottom_y = y + 20;   // (#227)
}

// =============================================================================
// Panel: Devices / Printers  (#318)
// =============================================================================
// Network-printing syscalls (kernel b545). Not yet wrapped in libc, so the
// numbers + thin wrappers + the on-wire printer struct are declared locally.
#ifndef SYS_PRINT_LIST
#define SYS_PRINT_LIST    291
#define SYS_PRINT_JOB     292
#define SYS_PRINT_ADD     293
#define SYS_PRINT_REMOVE  294
#define SYS_PRINT_IMAGE   296
#endif
#define PRT_NAME_LEN   32
#define PRT_HOST_LEN   64
#define PRT_QUEUE_LEN  64
#define PRT_MAX        8
typedef struct {
    char           name[PRT_NAME_LEN];
    char           host[PRT_HOST_LEN];
    char           queue[PRT_QUEUE_LEN];
    unsigned short port;
    int            is_default;
    int            valid;
} prt_cfg_t;   // MUST match kernel printer_cfg_t (net/ipp.h) byte-for-byte

static inline int prt_list(prt_cfg_t *out, int max) {
    return (int)syscall2(SYS_PRINT_LIST, (long)out, max);
}
static inline int prt_add(const char *name, const char *host, int port,
                          const char *queue, int make_default) {
    return (int)syscall5(SYS_PRINT_ADD, (long)name, (long)host, port,
                         (long)queue, make_default);
}
static inline int prt_remove(const char *name) {
    return (int)syscall1(SYS_PRINT_REMOVE, (long)name);
}
static inline int prt_job(const char *printer, const char *title, const char *text) {
    return (int)syscall3(SYS_PRINT_JOB, (long)printer, (long)title, (long)text);
}

static prt_cfg_t g_printers[PRT_MAX];
static int  g_printer_count  = 0;
static int  g_printers_seeded = 0;
static char g_print_status[100] = "";

static void printers_refresh(void) {
    int n = prt_list(g_printers, PRT_MAX);
    if (n < 0) n = 0;
    if (n > PRT_MAX) n = PRT_MAX;
    g_printer_count = n;
}

// Seed the user's Brother HL-L3230CDW example once, if nothing is configured.
static void printers_seed_once(void) {
    if (g_printers_seeded) return;
    g_printers_seeded = 1;
    printers_refresh();
    if (g_printer_count == 0) {
        // Name has no spaces: PRINTERS.CFG is space-delimited (kernel cfg_field).
        prt_add("Brother_HL-L3230CDW", "192.0.2.246", 631, "BrotherIPP", 1);
        printers_refresh();
    }
}

// Build "host:port" into out (out must hold >= PRT_HOST_LEN + 8).
static void prt_fmt_hostport(const prt_cfg_t *p, char *out) {
    int i = 0; const char *h = p->host;
    while (*h && i < PRT_HOST_LEN) out[i++] = *h++;
    out[i++] = ':';
    char pb[8]; gui_itoa(p->port ? p->port : 631, pb, sizeof(pb));
    const char *q = pb; while (*q) out[i++] = *q++;
    out[i] = '\0';
}

// Row / button geometry (kept in sync with the click handler).
// (#227) content area now scrolls: DEV_ROW_Y0 is read by BOTH the draw pass
// and the click handler, so applying the offset here keeps them in lockstep
// exactly like every other shared geometry function in this file.
#define DEV_ROW_Y0   (PADDING + 50 - g_content_scroll.offset)
#define DEV_ROW_H    76
#define DEV_CARD_H   66
#define DEV_BTN_W    82
#define DEV_BTN_H    24

static void draw_devices_panel(void) {
    printers_seed_once();   // loads the list + seeds the Brother example on first view
    int x  = CONTENT_X + PADDING;
    int y  = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    int cw = CONTENT_WIDTH - 2 * PADDING;

    draw_section_header(x, y, "Printers & Devices");
    // Right-aligned "Add Printer" action on the header row.
    draw_button_small(x + cw - 140, y - 4, 140, "+ Add Printer", true);

    if (g_printer_count == 0) {
        int ry = DEV_ROW_Y0;
        draw_card(x, ry, cw, DEV_CARD_H);
        win_draw_text(window_handle, x + 15, ry + 12, "No printers configured.", COL_TEXT_PRIMARY);
        win_draw_text_small(window_handle, x + 15, ry + 36,
            "Click \"+ Add Printer\" to add a network (IPP/CUPS) printer.", COL_TEXT_SECONDARY);
        y = ry + DEV_CARD_H + 14;
    } else {
        for (int i = 0; i < g_printer_count; i++) {
            prt_cfg_t *p = &g_printers[i];
            int ry = DEV_ROW_Y0 + i * DEV_ROW_H;
            draw_card(x, ry, cw, DEV_CARD_H);

            win_draw_text(window_handle, x + 15, ry + 10, p->name, COL_TEXT_PRIMARY);
            if (p->is_default) {
                gui_fill_rounded(window_handle, x + 250, ry + 8, 74, 18, 9, COL_SUCCESS);
                gui_text_ttf_centered(window_handle, x + 250, ry + 8, 74, 18,
                                      "DEFAULT", gui_ink_on(COL_SUCCESS), 10);
            }
            char hp[PRT_HOST_LEN + 8];
            prt_fmt_hostport(p, hp);
            draw_label_value(x + 15, ry + 34, "Host:", hp, 42);
            draw_label_value(x + 15, ry + 50, "Queue:", p->queue, 42);

            // Row actions, right-aligned: Test / Default / Remove.
            int bx = x + cw - (DEV_BTN_W * 3 + 20);
            int by = ry + 12;
            draw_button_small(bx,               by, DEV_BTN_W, "Test Page", false);
            draw_button_small(bx + DEV_BTN_W+3, by, DEV_BTN_W,
                              p->is_default ? "Default" : "Set Def.", false);
            draw_button_small(bx + (DEV_BTN_W+3)*2, by, DEV_BTN_W, "Remove", false);
        }
        y = DEV_ROW_Y0 + g_printer_count * DEV_ROW_H + 6;
    }

    if (g_print_status[0])
        draw_hint_ic(x, y, "INFO", COL_ACCENT, g_print_status);
    y += 24;
    draw_hint(x, y, "Brother HL-L3230CDW: text via CUPS 192.0.2.246/BrotherIPP;");
    y += 20;
    draw_hint(x, y, "direct image via 192.0.2.55 queue /ipp/print (port 631).");
    g_content_bottom_y = y + 20;   // (#227)
}

// =============================================================================
// Panel: Bluetooth (#372, honesty pass #237)
// UI codes against the bt_client.h contract, which mirrors the architect's
// kernel bt_ctrl.h one-to-one. The backend is an honest stub (no invented
// devices, no simulated pairing) until the SYS_BT_* stack lands; swapping it
// in is a one-line change per function inside bt_client.h and nothing in
// this panel changes.
// =============================================================================

// Click regions recorded during the draw pass and consumed by the click
// handler, so draw geometry and hit testing never drift apart.
enum { BTA_NONE = 0, BTA_POWER, BTA_SCAN, BTA_PAIR, BTA_CONN, BTA_FORGET };
typedef struct { int x, y, w, h, action, dev; } bt_hit_t;
#define BT_HITS_MAX 48
static bt_hit_t g_bt_hits[BT_HITS_MAX];
static int g_bt_nhits = 0;
static bt_device_t g_bt_dev[BT_MAX_DEVICES];
static int g_bt_ndev = 0;
static int g_bt_spin = 0;                 // spinner frame, advanced from idle tick
// A single in-flight pair/connect the UI initiated. The real API is async (the
// device's link state changes when the operation completes), so the UI tracks
// what it asked for and shows a spinner until the target link state is reached.
static bt_addr_t g_bt_pend_addr;
static int g_bt_pend_active = 0;
static bt_link_state_t g_bt_pend_target = BT_LINK_NONE;

static void bt_hit_reset(void) { g_bt_nhits = 0; }
static void bt_hit_add(int x, int y, int w, int h, int action, int dev) {
    if (g_bt_nhits < BT_HITS_MAX) {
        g_bt_hits[g_bt_nhits].x = x; g_bt_hits[g_bt_nhits].y = y;
        g_bt_hits[g_bt_nhits].w = w; g_bt_hits[g_bt_nhits].h = h;
        g_bt_hits[g_bt_nhits].action = action; g_bt_hits[g_bt_nhits].dev = dev;
        g_bt_nhits++;
    }
}
// Is device `d` the one with an in-flight operation still pending?
static int bt_dev_pending(const bt_device_t *d) {
    if (!g_bt_pend_active) return 0;
    if (!bt_addr_eq(&d->addr, &g_bt_pend_addr)) return 0;
    if (g_bt_pend_target == BT_LINK_PAIRED    && d->paired)    return 0;   // reached
    if (g_bt_pend_target == BT_LINK_CONNECTED && d->connected) return 0;
    return 1;
}

// 1px Bresenham line (settings has no line primitive; used for the BT rune).
static void bt_line(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = adx - ady;
    for (;;) {
        win_draw_rect(window_handle, x0, y0, 2, 2, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -ady) { err -= ady; x0 += sx; }
        if (e2 <  adx) { err += adx; y0 += sy; }
    }
}
// The Bluetooth rune scaled to a box of side `s` at (x,y).
static void bt_draw_rune(int x, int y, int s, uint32_t c) {
    int cx = x + s / 2;
    int t = y + 1, b = y + s - 1;
    int lft = x + 2, rgt = x + s - 3;
    bt_line(cx, t, cx, b, c);
    bt_line(cx, t, rgt, y + s / 4, c);
    bt_line(rgt, y + s / 4, lft, y + (3 * s) / 4, c);
    bt_line(cx, b, rgt, y + (3 * s) / 4, c);
    bt_line(rgt, y + (3 * s) / 4, lft, y + s / 4, c);
}
// Coarse device-class glyph in an 18px box.
static void bt_draw_dev_icon(int x, int y, int cls, uint32_t c) {
    switch (cls) {
        case BT_DEV_KEYBOARD:
            gui_draw_rect_outline(window_handle, x, y + 3, 18, 12, c);
            for (int r = 0; r < 2; r++)
                for (int k = 0; k < 4; k++)
                    win_draw_rect(window_handle, x + 3 + k * 4, y + 6 + r * 4, 2, 2, c);
            break;
        case BT_DEV_MOUSE:
            gui_draw_rect_outline(window_handle, x + 4, y + 1, 10, 16, c);
            win_draw_rect(window_handle, x + 9, y + 3, 1, 5, c);
            break;
        case BT_DEV_AUDIO:
            for (int k = -6; k <= 6; k++)
                win_draw_rect(window_handle, x + 9 + k, y + 3 + (k * k) / 12, 2, 2, c);
            win_draw_rect(window_handle, x + 2,  y + 8, 4, 8, c);
            win_draw_rect(window_handle, x + 12, y + 8, 4, 8, c);
            break;
        case BT_DEV_PHONE:
            gui_draw_rect_outline(window_handle, x + 4, y + 1, 10, 16, c);
            win_draw_rect(window_handle, x + 7, y + 14, 4, 1, c);
            break;
        case BT_DEV_COMPUTER:
            gui_draw_rect_outline(window_handle, x, y + 2, 18, 11, c);
            win_draw_rect(window_handle, x + 5, y + 14, 8, 2, c);
            break;
        default:
            bt_draw_rune(x + 4, y + 1, 12, c);
            break;
    }
}
// 8-dot rotating spinner centred at (cx, cy).
static void bt_draw_spinner(int cx, int cy) {
    static const int ox[8] = { 0,  5,  7,  5,  0, -5, -7, -5 };
    static const int oy[8] = {-7, -5,  0,  5,  7,  5,  0, -5 };
    for (int i = 0; i < 8; i++) {
        int d = (g_bt_spin - i) & 7;
        uint32_t col = (d == 0) ? COL_ACCENT
                     : (d <= 2) ? COL_TEXT_SECONDARY
                                : COL_SLIDER_TRACK;
        win_draw_rect(window_handle, cx + ox[i] - 1, cy + oy[i] - 1, 3, 3, col);
    }
}
// 4 signal bars from an RSSI in dBm (0 = unknown -> no bars).
static void bt_draw_signal(int x, int y, int rssi) {
    int bars = 0;
    if (rssi != 0) {
        if      (rssi >= -50) bars = 4;
        else if (rssi >= -60) bars = 3;
        else if (rssi >= -70) bars = 2;
        else                  bars = 1;
    }
    for (int i = 0; i < 4; i++) {
        int bh = 4 + i * 3;
        uint32_t col = (i < bars) ? COL_ACCENT : COL_SLIDER_TRACK;
        win_draw_rect(window_handle, x + i * 5, y + (13 - bh), 3, bh, col);
    }
}

#define BT_CARD_H   54
#define BT_ROW_STEP 62

// (#237) Kept for #383 (the real Bluetooth driver): renders one paired/
// available device row from REAL data once a driver exists to supply it.
// Unreachable today - no driver means no devices are ever listed - which is
// why this is marked unused rather than deleted: it is scaffolding, not
// dead code left behind by accident.
__attribute__((unused)) static void bt_draw_device_card(int x, int y, int cw, int dev, int paired_section) {
    bt_device_t *d = &g_bt_dev[dev];
    draw_card(x, y, cw, BT_CARD_H);
    bt_draw_dev_icon(x + 14, y + 16, d->cls, COL_TEXT_PRIMARY);

    win_draw_text(window_handle, x + 44, y + 8, d->name, COL_TEXT_PRIMARY);
    char abuf[20]; bt_addr_fmt(&d->addr, abuf);
    win_draw_text_small(window_handle, x + 44, y + 30, abuf, COL_TEXT_SECONDARY);
    win_draw_text_small(window_handle, x + 44 + 140, y + 30,
                        d->is_le ? "LE" : "Classic", COL_TEXT_DISABLED);

    int right = x + cw - 12;

    if (bt_dev_pending(d)) {
        const char *lbl = (g_bt_pend_target == BT_LINK_PAIRED) ? "Pairing..." : "Connecting...";
        win_draw_text_small(window_handle, right - 100, y + 20, lbl, COL_ACCENT);
        bt_draw_spinner(right - 116, y + BT_CARD_H / 2);
        return;
    }

    if (paired_section) {
        uint32_t bc = d->connected ? COL_SUCCESS : COL_INPUT_BORDER;
        const char *bl = d->connected ? "Connected" : "Paired";
        gui_fill_rounded(window_handle, x + 44 + 210, y + 6, 88, 18, 9, bc);
        gui_text_ttf_centered(window_handle, x + 44 + 210, y + 6, 88, 18, bl,
                              d->connected ? gui_ink_on(COL_SUCCESS) : COL_TEXT_SECONDARY, 10);
        int bw = 92, gap = 6;
        int fx = right - bw;
        int cxb = fx - gap - bw;
        bt_hit_add(cxb, y + 14, bw, 24, BTA_CONN, dev);
        draw_button_small(cxb, y + 14, bw, d->connected ? "Disconnect" : "Connect", !d->connected);
        bt_hit_add(fx, y + 14, bw, 24, BTA_FORGET, dev);
        draw_button_small(fx, y + 14, bw, "Forget", false);
    } else {
        bt_draw_signal(right - 130, y + 20, d->rssi);
        int bw = 88;
        int fx = right - bw;
        bt_hit_add(fx, y + 14, bw, 24, BTA_PAIR, dev);
        draw_button_small(fx, y + 14, bw, "Pair", true);
    }
}

static void draw_bluetooth_panel(void) {
    bt_hit_reset();
    hwinfo_load();
    int x  = CONTENT_X + PADDING;
    int y  = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    int cw = CONTENT_WIDTH - 2 * PADDING;
    int on = bt_is_powered();

    draw_section_header(x, y, "Bluetooth");

    // (#382 pass2) MayteraOS has no Bluetooth driver/stack. Rather than a mock
    // power switch + fake device list, reflect the real radio presence. No BT
    // adapter is present on any current target (QEMU VMs, the iMac), so this
    // honestly reports "no adapter" instead of scanning fake devices.
    if (!g_bt_present) {
        y += 44;
        int ch = 130;
        draw_card(x, y, cw, ch);
        bt_draw_rune(x + cw / 2 - 14, y + 20, 28, COL_TEXT_DISABLED);
        gui_text_ttf_centered(window_handle, x, y + 58, cw, 20,
                              "No Bluetooth adapter detected", COL_TEXT_PRIMARY, 15);
        gui_text_ttf_centered(window_handle, x, y + 84, cw, 16,
                              "This system has no Bluetooth radio, and MayteraOS has no Bluetooth stack.",
                              COL_TEXT_SECONDARY, 12);
        g_bt_ndev = 0;
        g_content_bottom_y = y + ch;   // (#227)
        return;
    }

    // (#237) A real adapter WAS found (g_bt_present, above), but MayteraOS has
    // no Bluetooth driver/stack to run it - a genuinely different condition
    // from "no adapter" (nothing to build on) and from a hypothetical "driver
    // present, scanning" or "driver present, no devices found" (neither of
    // which can occur: nothing here ever scans). `on` is always false in this
    // build (bt_is_powered() is a stub), so every control below is drawn with
    // the shared GUI_ST_DISABLED treatment via draw_toggle_inert()/
    // draw_button_inert() and NO click hit is registered for any of them -
    // inert by construction, not merely painted grey. This is the scaffolding
    // #383's real driver fills in: same sections, same layout, zero invented
    // data.
    y += 44;
    int tgx = x + cw - 52, tgy = y - 2;
    win_draw_text(window_handle, tgx - 34, y + 2, on ? "On" : "Off", COL_TEXT_SECONDARY);
    draw_toggle_inert(tgx, tgy, on);
    y += 44;

    {
        int ch = 96;
        draw_card(x, y, cw, ch);
        bt_draw_rune(x + 14, y + 12, 24, COL_TEXT_DISABLED);
        win_draw_text(window_handle, x + 48, y + 8, "Bluetooth adapter detected", COL_TEXT_PRIMARY);
        win_draw_text_small(window_handle, x + 48, y + 28,
                            "MayteraOS has no Bluetooth driver yet. Pairing and connecting",
                            COL_TEXT_SECONDARY);
        win_draw_text_small(window_handle, x + 48, y + 44,
                            "are not available in this build.", COL_TEXT_SECONDARY);
        // Real USB vendor:product id of the radio that tripped the presence
        // probe (hwinfo_load(), captured once, never invented) - concrete
        // scaffolding for whoever builds the real driver.
        char idbuf[10]; hw_fmt_id(g_bt_vid, g_bt_pid, idbuf);
        char idline[32] = "USB "; hw_append(idline, sizeof(idline), idbuf);
        win_draw_text_small(window_handle, x + 48, y + 64, idline, COL_TEXT_DISABLED);
        y += ch + 20;
    }

    draw_button_inert(x, y, 160, "Scan for devices");
    y += 44;

    draw_subsection(x, y, "Paired Devices");
    y += 26;
    draw_hint(x, y, "Not available: no Bluetooth driver is installed.");
    y += 26 + 8;

    draw_subsection(x, y, "Available Devices");
    y += 26;
    draw_hint(x, y, "Not available: no Bluetooth driver is installed.");
    y += 26;

    g_bt_ndev = 0;
    g_content_bottom_y = y + 20;   // (#227)
}

// Does the Bluetooth panel have a live animation (scan or pending op) worth a
// redraw on the idle tick?
static int bt_panel_animating(void) {
    if (!bt_is_powered()) return 0;
    if (bt_scan_active()) return 1;
    if (g_bt_pend_active) {
        // Clear the pending flag once the device reached its target link state.
        for (int i = 0; i < g_bt_ndev; i++)
            if (bt_dev_pending(&g_bt_dev[i])) return 1;
        g_bt_pend_active = 0;
    }
    return 0;
}

// =============================================================================
// Panel: Wi-Fi (#384, honesty pass #237)
// Wired network status shown elsewhere (Network tab) is REAL (get_net_info).
// Wi-Fi scan/connect has no driver (#383 not built) and wifi_client.h is now
// an honest stub, not a mock: every function returns a fixed "not available"
// result instead of a fabricated one. Same structure as the Bluetooth panel.
// =============================================================================
enum { WFA_NONE = 0, WFA_POWER, WFA_SCAN, WFA_CONNECT, WFA_DISCONNECT, WFA_FORGET };
typedef struct { int x, y, w, h, action, net; } wf_hit_t;
#define WF_HITS_MAX 48
static wf_hit_t g_wf_hits[WF_HITS_MAX];
static int g_wf_nhits = 0;
static wifi_network_t g_wf_net[WIFI_MAX_NETWORKS];
static int g_wf_nnet = 0;
static char g_wf_target[WIFI_SSID_MAX];   // SSID awaiting a password / association
static int  g_wf_pend_active = 0;

static void wf_hit_reset(void) { g_wf_nhits = 0; }
static void wf_hit_add(int x, int y, int w, int h, int action, int net) {
    if (g_wf_nhits < WF_HITS_MAX) {
        g_wf_hits[g_wf_nhits].x = x; g_wf_hits[g_wf_nhits].y = y;
        g_wf_hits[g_wf_nhits].w = w; g_wf_hits[g_wf_nhits].h = h;
        g_wf_hits[g_wf_nhits].action = action; g_wf_hits[g_wf_nhits].net = net;
        g_wf_nhits++;
    }
}
static int wf_streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}
// Signal bars from a 0..100 percentage.
static void wf_draw_signal(int x, int y, int pct, uint32_t on_col) {
    int bars = pct >= 75 ? 4 : pct >= 50 ? 3 : pct >= 25 ? 2 : pct > 0 ? 1 : 0;
    for (int i = 0; i < 4; i++) {
        int bh = 4 + i * 3;
        uint32_t col = (i < bars) ? on_col : COL_SLIDER_TRACK;
        win_draw_rect(window_handle, x + i * 5, y + (13 - bh), 3, bh, col);
    }
}
// Small padlock glyph for secured networks.
static void wf_draw_lock(int x, int y, uint32_t c) {
    win_draw_rect(window_handle, x, y + 6, 12, 9, c);          // body
    gui_draw_rect_outline(window_handle, x + 3, y + 1, 6, 7, c); // shackle
    win_draw_rect(window_handle, x + 5, y + 9, 2, 3, COL_CARD_BG); // keyhole
}
// A Wi-Fi arcs glyph (three nested arcs + base dot) in an ~18px box.
static void wf_draw_glyph(int x, int y, int s, uint32_t c) {
    int cx = x + s / 2, base = y + s - 2;
    win_draw_rect(window_handle, cx - 1, base - 1, 3, 3, c);   // base dot
    for (int a = 1; a <= 3; a++) {
        int r = a * (s / 6);
        for (int dx = -r; dx <= r; dx++) {
            int dy = (dx * dx) / (r > 0 ? (r * 2) : 1);
            win_draw_rect(window_handle, cx + dx, base - r + dy - 2, 2, 2, c);
        }
    }
}

// Whether this network row is mid-association (spinner).
static int wf_net_pending(const wifi_network_t *w) {
    if (!g_wf_pend_active) return 0;
    if (!wf_streq(w->ssid, g_wf_target)) return 0;
    return w->connected ? 0 : 1;
}

#define WF_CARD_H   52
#define WF_ROW_STEP 60

// (#237) Kept for #383 (the real Wi-Fi driver): renders one available/saved
// network row from REAL data once a driver exists to supply it. Unreachable
// today - no driver means no networks are ever listed - which is why this is
// marked unused rather than deleted: it is scaffolding, not dead code left
// behind by accident.
__attribute__((unused)) static void wf_draw_net_card(int x, int y, int cw, int idx, int saved_section) {
    wifi_network_t *w = &g_wf_net[idx];
    draw_card(x, y, cw, WF_CARD_H);
    wf_draw_signal(x + 14, y + 18, w->signal, COL_ACCENT);

    win_draw_text(window_handle, x + 44, y + 8, w->ssid, COL_TEXT_PRIMARY);
    if (w->security != WIFI_SEC_OPEN) wf_draw_lock(x + 44, y + 30, COL_TEXT_SECONDARY);
    const char *sec = w->security == WIFI_SEC_WPA3 ? "WPA3"
                    : w->security == WIFI_SEC_WPA2 ? "WPA2" : "Open";
    win_draw_text_small(window_handle, x + 44 + (w->security != WIFI_SEC_OPEN ? 18 : 0), y + 31,
                        sec, COL_TEXT_DISABLED);
    char pb[8]; gui_itoa(w->signal, pb, sizeof(pb));
    int pl = my_strlen(pb); pb[pl++] = '%'; pb[pl] = 0;
    win_draw_text_small(window_handle, x + 44 + 120, y + 31, pb, COL_TEXT_SECONDARY);

    int right = x + cw - 12;
    if (wf_net_pending(w)) {
        win_draw_text_small(window_handle, right - 100, y + 18, "Connecting...", COL_ACCENT);
        bt_draw_spinner(right - 116, y + WF_CARD_H / 2);
        return;
    }
    if (w->connected) {
        gui_fill_rounded(window_handle, x + 44 + 175, y + 6, 88, 18, 9, COL_SUCCESS);
        gui_text_ttf_centered(window_handle, x + 44 + 175, y + 6, 88, 18, "Connected",
                              gui_ink_on(COL_SUCCESS), 10);
        int bw = 100, fx = right - bw;
        wf_hit_add(fx, y + 13, bw, 24, WFA_DISCONNECT, idx);
        draw_button_small(fx, y + 13, bw, "Disconnect", false);
    } else if (saved_section) {
        int bw = 92, gap = 6, fx = right - bw, cxb = fx - gap - bw;
        wf_hit_add(cxb, y + 13, bw, 24, WFA_CONNECT, idx);
        draw_button_small(cxb, y + 13, bw, "Connect", true);
        wf_hit_add(fx, y + 13, bw, 24, WFA_FORGET, idx);
        draw_button_small(fx, y + 13, bw, "Forget", false);
    } else {
        int bw = 88, fx = right - bw;
        wf_hit_add(fx, y + 13, bw, 24, WFA_CONNECT, idx);
        draw_button_small(fx, y + 13, bw, "Connect", true);
    }
}

static void draw_wifi_panel(void) {
    wf_hit_reset();
    hwinfo_load();
    int x  = CONTENT_X + PADDING;
    int y  = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    int cw = CONTENT_WIDTH - 2 * PADDING;
    int on = wifi_is_powered();

    draw_section_header(x, y, "Wi-Fi");

    // (#382 pass2) No Wi-Fi driver/adapter on current targets (the iMac networks
    // over USB Ethernet). Reflect real adapter presence instead of a mock scan.
    if (!g_wifi_present) {
        y += 44;
        draw_card(x, y, cw, 130);
        wf_draw_glyph(x + cw / 2 - 12, y + 22, 26, COL_TEXT_DISABLED);
        gui_text_ttf_centered(window_handle, x, y + 60, cw, 20,
                              "No Wi-Fi adapter detected", COL_TEXT_PRIMARY, 15);
        gui_text_ttf_centered(window_handle, x, y + 86, cw, 16,
                              "This system has no wireless adapter. Use the Network tab for wired Ethernet.",
                              COL_TEXT_SECONDARY, 12);
        g_wf_nnet = 0;
        g_content_bottom_y = y + 130;   // (#227)
        return;
    }

    // (#237) A real adapter WAS found (g_wifi_present, above), but MayteraOS
    // has no Wi-Fi driver to run it - a genuinely different condition from
    // "no adapter" (nothing to build on) and from a hypothetical "driver
    // present, scanning" or "driver present, no networks found" (neither of
    // which can occur: nothing here ever scans). This is exactly the
    // situation that made the old mock dangerous: the iMac's real wireless
    // card opened the hardware gate, and a fabricated successful join
    // rendered behind it with nothing to mark it as fake - see blame.md
    // #230/#237. `on` is always false in this build (wifi_is_powered() is a
    // stub), so every control below is drawn with the shared GUI_ST_DISABLED
    // treatment via draw_toggle_inert()/draw_button_inert() and NO click hit
    // is registered for any of them - inert by construction, not merely
    // painted grey. This is the scaffolding #383's real driver fills in:
    // same sections, same layout, zero invented data.
    y += 44;
    int tgx = x + cw - 52, tgy = y - 2;
    win_draw_text(window_handle, tgx - 34, y + 2, on ? "On" : "Off", COL_TEXT_SECONDARY);
    draw_toggle_inert(tgx, tgy, on);
    y += 44;

    {
        int ch = 96;
        draw_card(x, y, cw, ch);
        wf_draw_glyph(x + 16, y + 12, 22, COL_TEXT_DISABLED);
        win_draw_text(window_handle, x + 48, y + 8, "Wireless adapter detected", COL_TEXT_PRIMARY);
        win_draw_text_small(window_handle, x + 48, y + 28,
                            "MayteraOS has no Wi-Fi driver yet. Scanning and joining",
                            COL_TEXT_SECONDARY);
        win_draw_text_small(window_handle, x + 48, y + 44,
                            "a network are not available in this build.", COL_TEXT_SECONDARY);
        // Real vendor:device (PCI) or vendor:product (USB) id of the adapter
        // that tripped the presence probe (hwinfo_load(), captured once,
        // never invented) - concrete scaffolding for whoever builds #383.
        char idbuf[10]; hw_fmt_id(g_wifi_vid, g_wifi_did, idbuf);
        char idline[32]; copy_str(idline, g_wifi_is_usb ? "USB " : "PCI ", sizeof(idline));
        hw_append(idline, sizeof(idline), idbuf);
        win_draw_text_small(window_handle, x + 48, y + 64, idline, COL_TEXT_DISABLED);
        y += ch + 20;
    }

    draw_button_inert(x, y, 160, "Scan for networks");
    y += 44;

    draw_subsection(x, y, "Available Networks");
    y += 26;
    draw_hint(x, y, "Not available: no Wi-Fi driver is installed.");
    y += 26 + 8;

    draw_subsection(x, y, "Saved Networks");
    y += 26;
    draw_hint(x, y, "Not available: no Wi-Fi driver is installed.");
    y += 26;

    g_wf_nnet = 0;
    g_content_bottom_y = y + 20;   // (#227)
}

static int wifi_panel_animating(void) {
    if (!wifi_is_powered()) return 0;
    if (wifi_scan_active()) return 1;
    if (g_wf_pend_active) {
        for (int i = 0; i < g_wf_nnet; i++)
            if (wf_net_pending(&g_wf_net[i])) return 1;
        g_wf_pend_active = 0;
    }
    return 0;
}

// =============================================================================
// Draw Content Area
// =============================================================================

static void draw_content(void) {
    // #3: on the first content draw, record which panel we are about to render
    // so a crash inside a panel draw is attributable from /SETLOG.TXT.
    static int s_first_content = 1;
    if (s_first_content) { setlog_n("SET: draw_content panel", current_panel); s_first_content = 0; }
    // Clear content area
    win_draw_rect(window_handle, CONTENT_X, 0, CONTENT_WIDTH, CONTENT_HEIGHT, COL_CONTENT_BG);

    // Draw the appropriate panel
    switch (current_panel) {
        case PANEL_APPEARANCE:  draw_appearance_panel(); break;
        case PANEL_DISPLAY:     draw_display_panel(); break;
        case PANEL_SOUND:       draw_sound_panel(); break;
        case PANEL_NETWORK:     draw_network_panel(); break;
        case PANEL_KEYBOARD:    draw_keyboard_panel(); break;
        case PANEL_MOUSE:       draw_mouse_panel(); break;
        case PANEL_DATETIME:    draw_datetime_panel(); break;
        case PANEL_USERS:       draw_users_panel(); break;
        case PANEL_PRIVACY:     draw_privacy_panel(); break;
        case PANEL_STORAGE:     draw_storage_panel(); break;
        case PANEL_DEFAULTS:    draw_defaults_panel(); break;
        case PANEL_ABOUT:       draw_about_panel(); break;
        case PANEL_NOTIFICATIONS: draw_notifications_panel(); break;
        case PANEL_DEVICES:     draw_devices_panel(); break;
        case PANEL_BLUETOOTH:   draw_bluetooth_panel(); break;
        case PANEL_WIFI:        draw_wifi_panel(); break;
        case PANEL_EXTSVC:      draw_extsvc_panel(); break;
        case PANEL_STARTMENU:   draw_startmenu_panel(); break;
        case PANEL_DOCK:        draw_dock_panel(); break;
    }

    // (#227) Compute the panel's true content height, independent of the
    // scroll offset that was just baked into every y the panel drew at (the
    // -g_content_scroll.offset term cancels: g_content_bottom_y was captured
    // in the same shifted coordinate space, so adding the offset back gives
    // the UNshifted height). gui_scroll_config() re-clamps the current offset
    // against the new max, so a panel switch or a shrinking window can never
    // strand the view past the new bottom.
    int raw_h = g_content_bottom_y + g_content_scroll.offset - PADDING;
    gui_scroll_config(&g_content_scroll, CONTENT_X, 0, CONTENT_WIDTH, CONTENT_HEIGHT,
                      raw_h, 40 /* step: ~one control row */);
    gui_scroll_draw_on(window_handle, &g_content_scroll, COL_CONTENT_BG);
}

// =============================================================================
// Modal Dialog: Change Password
// =============================================================================

// Dialog height: scales with field count so 4/5-field forms (Add Printer) fit.
// (#745) Extra row height reserved for MODAL_EDIT_PROFILE's picture picker
// (avatar preview + 8 swatches), drawn below the two text fields. Kept as
// its own constant so draw_modal() and the click hit-test below cannot drift
// apart the way a hand-copied number in each would risk.
#define MODAL_AVATAR_ROW_H 56

static int modal_dh(void) {
    if (modal_mode == MODAL_CREDITS) {
        // Credits is a document, not a 5-line notice: give it the window.
        int h = WIN_HEIGHT - 100;
        if (h > 460) h = 460;
        if (h < 200) h = 200;
        return h;
    }
    int dh = 92 + modal_num_fields * 44;
    if (modal_mode == MODAL_EDIT_PROFILE) dh += MODAL_AVATAR_ROW_H;
    return dh;
}

// Dialog WIDTH. 360 was a literal in both draw_modal() and the click hit-test;
// Credits needs a wider box (the generated lines wrap at 62 characters, which is
// ~496px in the 8x16 bitmap font), so the number becomes shared for the same
// reason modal_dh() already is: a hand-copied literal in each is how a dialog's
// draw and its hit-test drift apart.
static int modal_dw(void) {
    if (modal_mode != MODAL_CREDITS) return 360;
    int w = WIN_WIDTH - 60;
    if (w > 620) w = 620;
    if (w < 360) w = 360;
    if (w > WIN_WIDTH - 20) w = WIN_WIDTH - 20;
    return w;
}

// THE credits viewport, window-local. One definition, called by the draw and by
// every input path, so a scroll offset can never be applied to one and not the
// other. That is not hypothetical: the comment at sidebar_hit() records the same
// bug being fixed for the panel list.
static void credits_layout(void) {
    int dw = modal_dw(), dh = modal_dh();
    int dx = (WIN_WIDTH - dw) / 2, dy = (WIN_HEIGHT - dh) / 2;
    int avail = dh - 44 - 46;                 // below the title bar, above Close
    // Floor to whole rows. With snap, this is what guarantees a row is never
    // left half-drawn across the viewport edge, which matters because a
    // userland app has no clip region: a partial row's text would spill over
    // the dialog title bar above it.
    int vh = (avail / CREDITS_LINE_H) * CREDITS_LINE_H;
    if (vh < CREDITS_LINE_H) vh = CREDITS_LINE_H;
    gui_scroll_config(&g_credits_scroll, dx + 14, dy + 44, dw - 28, vh,
                      credits_n * CREDITS_LINE_H, CREDITS_LINE_H);
    g_credits_scroll.snap = 1;   // a list of fixed-height rows; see gui_scroll.h
}

// y of the top of the picture-picker row, shared by draw_modal() and the
// click handler.
static int modal_avatar_row_y(int dy) {
    return dy + 46 + modal_num_fields * 44;
}

static void draw_modal(void) {
    if (modal_mode == MODAL_NONE) return;

    // Dim (not black-out) the background: a cheap interlaced scrim. Apps have no
    // framebuffer read-back, so we draw a dark line on every other scanline -
    // the panel behind stays visible through the gaps but reads as darkened.
    // (#704) INTENTIONALLY NOT themed: this is pure black used as a dimming
    // agent, not a style color. A themed value here (e.g. WINDOW_BG) would
    // defeat the effect entirely on a light theme, where the background IS
    // white/near-white and "dimming" toward it does nothing.
    for (int yy = 0; yy < WIN_HEIGHT; yy += 2)
        win_draw_rect(window_handle, 0, yy, WIN_WIDTH, 1, 0x00000000);

    // Dialog dimensions vary by type
    int dw = modal_dw();
    int dh = modal_dh();
    int dx = (WIN_WIDTH  - dw) / 2;
    int dy = (WIN_HEIGHT - dh) / 2;

    win_draw_rect(window_handle, dx, dy, dw, dh, COL_CONTENT_BG);
    gui_draw_rect_outline(window_handle, dx, dy, dw, dh, COL_ACCENT);
    win_draw_rect(window_handle, dx, dy, dw, 34, COL_SIDEBAR_BG);
    win_draw_rect(window_handle, dx, dy + 34, dw, 1, COL_SEPARATOR);

    // Title based on modal type
    const char *title = "Dialog";
    if (modal_mode == MODAL_CHANGE_PASSWORD) title = "Change Password";
    else if (modal_mode == MODAL_ADD_USER)   title = "Add User";
    else if (modal_mode == MODAL_EDIT_PROFILE) title = "Edit Profile";
    else if (modal_mode == MODAL_SET_DATETIME) title = "Set Date & Time";
    else if (modal_mode == MODAL_CREDITS)    title = "Credits";
    else if (modal_mode == MODAL_SET_NETWORK) title = "Network Configuration";
    else if (modal_mode == MODAL_SET_DNS)     title = "DNS Server";
    else if (modal_mode == MODAL_ADD_PRINTER) title = "Add Printer";
    else if (modal_mode == MODAL_WIFI_PASSWORD) title = "Connect to Wi-Fi";
    else if (modal_mode == MODAL_AUTOLOGIN_PW) title = "Confirm Password";
    win_draw_text(window_handle, dx + 14, dy + 10, title, COL_SIDEBAR_TEXT);

    if (modal_mode == MODAL_CREDITS) {
        credits_load();
        credits_layout();
        gui_scroll_t *sc = &g_credits_scroll;
        int cw  = gui_scroll_needed(sc) ? sc->w - GUI_SCROLL_W - 6 : sc->w;
        int vy0 = sc->y, vy1 = sc->y + sc->h;
        // Inks are floored against the surface they land on, not assumed.
        uint32_t ink = credits_ink(COL_TEXT_PRIMARY);
        uint32_t dim = credits_ink(COL_TEXT_SECONDARY);
        uint32_t lic = credits_ink(COL_ACCENT);
        int title_sz = theme_metric_or(THEME_METRIC_TYPE_TITLE, 16);
        int body_sz  = theme_metric_or(THEME_METRIC_TYPE_BODY, 14);

        // Live version, from the SAME call the desktop's version string uses
        // (SYS_GET_VERSION via get_version()), so this screen cannot report a
        // different build number from the one on the desktop. Drawn here rather
        // than baked into CREDITS.DAT for the same one-source-of-truth reason
        // the component list is not baked into this file.
        char vb[64]; char vline[80]; int vn = 0;
        vb[0] = 0; get_version(vb, sizeof(vb));
        for (const char *q = "Version "; *q; q++) vline[vn++] = *q;
        if (!vb[0]) { vb[0] = '?'; vb[1] = 0; }
        for (int k = 0; vb[k] && vn < (int)sizeof(vline) - 1; k++) vline[vn++] = vb[k];
        vline[vn] = 0;

        for (int i = 0; i < credits_n; i++) {
            int ly = sc->y + i * CREDITS_LINE_H - sc->offset;
            // A userland app has NO clip region (gui_scroll.h / draw_sidebar()),
            // so a row that is not FULLY inside the viewport is skipped rather
            // than half-drawn: otherwise its text spills over the title bar.
            if (ly < vy0 || ly + CREDITS_LINE_H > vy1) continue;
            const char *t = credits_text[i];
            switch (credits_style[i]) {
                case 'T':
                    win_draw_text_ttf_ex(window_handle, sc->x, ly, t, 0,
                                         title_sz, FONT_STYLE_BOLD, ink);
                    break;
                case 'H':
                    win_draw_text_ttf_ex(window_handle, sc->x, ly + 2, t, 0,
                                         body_sz, FONT_STYLE_BOLD, ink);
                    break;
                case 'N':
                    win_draw_text(window_handle, sc->x + 8, ly + 1, t, ink);
                    break;
                case 'L':
                    win_draw_text(window_handle, sc->x + 24, ly + 1, t, lic);
                    break;
                case 'D':
                    win_draw_text_small(window_handle, sc->x + 24, ly + 5, t, dim);
                    break;
                case 'R':
                    win_draw_rect(window_handle, sc->x,
                                  ly + CREDITS_LINE_H / 2, cw, 1, COL_SEPARATOR);
                    break;
                default:
                    win_draw_text(window_handle, sc->x, ly + 1, t, ink);
                    break;
            }
        }
        // The dialog body is filled with COL_CONTENT_BG, so say so rather than
        // leaning on the default: the default happens to be the same colour
        // today, and a dialog restyle would silently make it the wrong one.
        gui_scroll_draw_on(window_handle, sc, COL_CONTENT_BG);
        // Version and any truncation notice sit on the footer row, outside the
        // scrolled region, so they are readable at any scroll position.
        win_draw_text_small(window_handle, dx + 14, dy + dh - 30,
                            credits_truncated
                              ? "Attribution list truncated; see ATTRIBUTION.md"
                              : vline, dim);
        draw_button(dx + dw - 96, dy + dh - 40, 80, "Close", true, false);
        return;
    }

    // Field labels by modal type
    const char *labels[MODAL_MAX_FIELDS] = {"", "", "", "", ""};
    int use_stars[MODAL_MAX_FIELDS] = {0, 0, 0, 0, 0}; // 1 = show asterisks
    if (modal_mode == MODAL_CHANGE_PASSWORD) {
        labels[0] = "Current password:";
        labels[1] = "New password:";
        labels[2] = "Confirm password:";
        use_stars[0] = use_stars[1] = use_stars[2] = 1;
    } else if (modal_mode == MODAL_ADD_USER) {
        labels[0] = "Username:";
        labels[1] = "Password:";
        labels[2] = "Confirm password:";
        use_stars[1] = use_stars[2] = 1;
    } else if (modal_mode == MODAL_EDIT_PROFILE) {
        labels[0] = "Full Name:";
        labels[1] = "Email:";
    } else if (modal_mode == MODAL_SET_DATETIME) {
        labels[0] = "Time (HH:MM:SS):";
        labels[1] = "Date (YYYY-MM-DD):";
    } else if (modal_mode == MODAL_SET_NETWORK) {
        labels[0] = "IP Address:";
        labels[1] = "Subnet Mask:";
        labels[2] = "Gateway:";
        labels[3] = "DNS Server:";
    } else if (modal_mode == MODAL_SET_DNS) {
        labels[0] = "DNS Server:";
    } else if (modal_mode == MODAL_ADD_PRINTER) {
        labels[0] = "Name:";
        labels[1] = "Host / IP:";
        labels[2] = "Queue:";
        labels[3] = "Port (631):";
        labels[4] = "Default (y/n):";
    } else if (modal_mode == MODAL_WIFI_PASSWORD) {
        labels[0] = "Password:";
        use_stars[0] = 1;
    } else if (modal_mode == MODAL_AUTOLOGIN_PW) {
        labels[0] = "Your account password:";
        use_stars[0] = 1;
    }

    for (int i = 0; i < modal_num_fields; i++) {
        int fy = dy + 46 + i * 44;
        win_draw_text(window_handle, dx + 14, fy, labels[i], COL_TEXT_SECONDARY);
        uint32_t border = (modal_active_field == i) ? COL_ACCENT : COL_INPUT_BORDER;
        win_draw_rect(window_handle, dx + 14, fy + 16, dw - 28, 22, COL_INPUT_BG);
        gui_draw_rect_outline(window_handle, dx + 14, fy + 16, dw - 28, 22, border);
        char display[66];
        int flen = modal_cursor[i];
        if (flen > 63) flen = 63;
        // Horizontal scroll so the caret stays visible (max ~30 chars shown).
        int vis = 30;
        int start = 0;
        if (modal_active_field == i && modal_caret[i] > vis) {
            start = modal_caret[i] - vis;
        }
        int n = flen - start; if (n > vis) n = vis; if (n < 0) n = 0;
        for (int j = 0; j < n; j++) {
            display[j] = use_stars[i] ? '*' : modal_field[i][start + j];
        }
        display[n] = '\0';
        win_draw_text(window_handle, dx + 18, fy + 20, display, COL_TEXT_PRIMARY);
        // Caret: vertical bar at the caret column (8px monospace cells).
        if (modal_active_field == i) {
            int caret_col = modal_caret[i] - start;
            if (caret_col < 0) caret_col = 0;
            if (caret_col > vis) caret_col = vis;
            int caret_x = dx + 18 + caret_col * 8;
            win_draw_rect(window_handle, caret_x, fy + 18, 1, 18, COL_TEXT_PRIMARY);
        }
    }

    // (#745) Picture picker: avatar preview + an 8-swatch monogram-color row,
    // MODAL_EDIT_PROFILE only. Custom photo upload is deferred (design doc
    // section 10.3 - no userland image decoder, no file-browser surface in
    // Settings), so this IS the "assigned a default picture, configurable in
    // the user profile settings" surface: pick which of the 8 identity
    // colors this account's monogram uses. The unselected ring is drawn in
    // the same color as its own background, which is the double-AA-circle
    // trick's cheap way to make a ring invisible without a third primitive.
    if (modal_mode == MODAL_EDIT_PROFILE) {
        int ay = modal_avatar_row_y(dy);
        win_draw_text(window_handle, dx + 14, ay, "Picture:", COL_TEXT_SECONDARY);
        char pletter = modal_field[0][0] ? modal_field[0][0]
                                          : users[current_user_idx].username[0];
        draw_avatar_badge(dx + 14, ay + 16, 32, avatar_palette[modal_avatar_idx],
                          COL_CONTENT_BG, pletter);
        uint32_t sel_ring = gui_ensure_contrast(gui_ink_on(COL_CONTENT_BG),
                                                COL_CONTENT_BG, GUI_FLOOR_NONTEXT);
        int sx = dx + 60;
        for (int i = 0; i < 8; i++) {
            int cx = sx + i * 30, cy = ay + 32;
            uint32_t ring = (i == modal_avatar_idx) ? sel_ring : COL_CONTENT_BG;
            gui_fill_circle_aa(window_handle, cx - 13, cy - 13, 26, ring, COL_CONTENT_BG);
            gui_fill_circle_aa(window_handle, cx - 11, cy - 11, 22, avatar_palette[i], ring);
        }
    }

    // Error message
    int err_y = dy + 46 + modal_num_fields * 44;
    if (modal_mode == MODAL_EDIT_PROFILE) err_y += MODAL_AVATAR_ROW_H;
    if (err_y > dy + dh - 50) err_y = dy + dh - 50;
    if (modal_error[0]) {
        uint32_t errc = theme_color(THEME_COLOR_ERROR);
        draw_mico("CIRCX", dx + 14, err_y - 2, 14, errc, COL_CARD_BG);
        win_draw_text(window_handle, dx + 32, err_y, modal_error, errc);
    }

    draw_button(dx + dw - 184, dy + dh - 40, 80, "Cancel", false, false);
    draw_button(dx + dw -  96, dy + dh - 40, 80, "OK",     true,  false);
}

// =============================================================================
// Modal Submission
// =============================================================================

// #786: net_append_kv() / net_write_cfg() DELETED.
//
// They formatted /CONFIG/NETIP.CFG and wrote it with userconf_write_all(). The
// write was REFUSED for every non-root user - /CONFIG is root-owned mode 0711
// in /CONFIG/PERMS.DB, so sys_open(O_CREAT) under it fails for uid != 0 - and
// the save_failed() breadcrumb could not be written either, because
// /SETLOG.TXT is under "/" (root, 0755). Measured on VM <vmid>: click OK, panel
// updates, no file, no log, no clue.
//
// The kernel now writes that file itself (kernel/net/net.c net_persist_netcfg),
// from inside the same syscalls that APPLY the configuration. That is not just
// a permissions workaround: it makes "what is running" and "what boots" the
// same object, and leaves exactly ONE writer of the file instead of two that
// could format it differently. Do not add a second one back here.

// (#566) See the forward declaration + settings_is_root()/autologin_refresh()
// near the Users & Accounts globals. Root sets autologin for anyone with no
// password (kernel ABI); a non-root caller can only target their OWN account
// and the kernel requires their current password, so open a one-field modal
// to collect it rather than acting on a bare toggle click.
static void autologin_request(const char *user, int enable) {
    if (settings_is_root()) {
        int rc = sys_set_autologin(user, "", enable);
        if (rc == 0) autologin_refresh();
        draw_all();
        return;
    }
    copy_str(al_target_user, user, sizeof(al_target_user));
    al_target_enable = enable;
    modal_mode = MODAL_AUTOLOGIN_PW;
    modal_num_fields = 1;
    modal_field[0][0] = '\0';
    modal_cursor[0] = 0;
    modal_caret[0]  = 0;
    modal_active_field = 0;
    modal_error[0] = '\0';
    draw_all();
}

static void do_modal_submit(void) {
    if (modal_mode == MODAL_AUTOLOGIN_PW) {
        int rc = sys_set_autologin(al_target_user, modal_field[0], al_target_enable);
        if (rc != 0) {
            const char *msg = "Incorrect password.";
            int i = 0;
            while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
            draw_all();
            return;
        }
        autologin_refresh();
        modal_mode = MODAL_NONE;
        draw_all();
        return;
    }
    if (modal_mode == MODAL_WIFI_PASSWORD) {
        // (#384, honesty pass #237) This modal can no longer be reached: no
        // network row is ever drawn to click "Connect" on (draw_wifi_panel()
        // shows only the inert scaffolding, #237), so wifi_connect() below is
        // dead code kept for #383's real driver, not a live path. It now
        // returns -1 unconditionally (wifi_client.h's honest stub) instead
        // of the old mock's "accepts any password".
        if (modal_field[0][0] == '\0') { copy_str(modal_error, "Enter the network password", sizeof(modal_error)); return; }
        wifi_connect(g_wf_target, modal_field[0]);
        g_wf_pend_active = 1;
        modal_mode = MODAL_NONE;
        draw_all();
        return;
    }
    if (modal_mode == MODAL_CHANGE_PASSWORD) {
        // Existing password change logic
        if (modal_cursor[1] == 0) {
            const char *msg = "New password cannot be empty.";
            int i = 0;
            while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
            draw_all(); return;
        }
        int match = (modal_cursor[1] == modal_cursor[2]);
        if (match) {
            for (int i = 0; i < modal_cursor[1]; i++) {
                if (modal_field[1][i] != modal_field[2][i]) { match = 0; break; }
            }
        }
        if (!match) {
            const char *msg = "Passwords do not match.";
            int i = 0;
            while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
            draw_all(); return;
        }
        // #785: target the SESSION account, not users[current_user_idx].
        // The display list is capped at 4 rows and ordered by the kernel table,
        // so that index is a row number, not an identity. If we could not
        // resolve who is logged in, REFUSE rather than guess: silently changing
        // some other account's password is exactly the "reported one thing,
        // did another" class this codebase has shipped before.
        if (!g_session_user[0]) {
            const char *msg = "Cannot determine the logged-in account.";
            int i = 0;
            while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
            draw_all(); return;
        }
        int res = passwd_change(g_session_user, modal_field[0], modal_field[1]);
        if (res < 0) {
            // Report the reason the kernel actually gave. -2 is the #566
            // escalating lockout; calling that "incorrect password" sends the
            // user off retyping a password that was already right.
            const char *msg = (res == -2)
                ? "Account locked out. Wait and try again."
                : "Current password incorrect.";
            int i = 0;
            while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
        } else {
            modal_mode = MODAL_NONE;
        }
        draw_all();
        return;
    }

    if (modal_mode == MODAL_ADD_USER) {
        if (modal_cursor[0] == 0) {
            const char *msg = "Username cannot be empty.";
            int i = 0;
            while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
            draw_all(); return;
        }
        int match = (modal_cursor[1] == modal_cursor[2]);
        if (match) {
            for (int i = 0; i < modal_cursor[1]; i++) {
                if (modal_field[1][i] != modal_field[2][i]) { match = 0; break; }
            }
        }
        if (!match) {
            const char *msg = "Passwords do not match.";
            int i = 0;
            while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
            draw_all(); return;
        }
        // #745 TWO BUGS FIXED HERE, and the account was unusable either way.
        //
        // 1. THE PASSWORD WAS DISCARDED. The two password fields were checked
        //    against each other and then thrown away: adduser() has no password
        //    parameter and never wrote a shadow record, so every account created
        //    through this dialog could never authenticate. The `ref` account
        //    shipped at uid 1002 in every golden is exactly that account.
        //    sys_user_create_pw() sets the password in the SAME call and rolls
        //    the account back if it cannot, so the half-created state this
        //    produced is no longer expressible.
        //
        // 2. THE UID WAS COUNTED, NOT ALLOCATED. `1000 + user_count` collides
        //    the moment an account is deleted: with root(0) and ref(1002) left,
        //    it returns 1002, user_create refuses, and the user gets "Failed to
        //    add user" with no way forward. Passing uid 0 asks the KERNEL to
        //    allocate, which is where the account table actually is.
        //
        // Home is left to the kernel too (NULL), so the /HOME/<NAME8> rule lives
        // in one place instead of being re-derived, slightly differently, here.
        long r = sys_user_create_pw(modal_field[0], modal_field[1], 0, 0, 0);
        if (r >= 0) {
            modal_mode = MODAL_NONE;
            users_refresh();
        } else {
            const char *msg = "Could not create the account.";
            int i = 0;
            while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
        }
        draw_all();
        return;
    }


    if (modal_mode == MODAL_ADD_PRINTER) {
        // Name and Host are required; queue/port default sensibly.
        if (modal_field[0][0] == '\0' || modal_field[1][0] == '\0') {
            const char *msg = "Name and Host/IP are required.";
            int i = 0; while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; } modal_error[i] = '\0';
            draw_all(); return;
        }
        int port = 0; const char *ps = modal_field[3];
        while (*ps == ' ') ps++;
        while (*ps >= '0' && *ps <= '9') { port = port * 10 + (*ps - '0'); ps++; }
        if (port <= 0) port = 631;
        const char *queue = modal_field[2][0] ? modal_field[2] : "ipp/print";
        int mkdef = (modal_field[4][0] == 'y' || modal_field[4][0] == 'Y' ||
                     modal_field[4][0] == '1');
        // First printer is always the default.
        if (g_printer_count == 0) mkdef = 1;
        // PRINTERS.CFG is space-delimited, so the name cannot contain spaces
        // (the kernel parser would truncate it); fold spaces to underscores.
        for (int si = 0; modal_field[0][si]; si++)
            if (modal_field[0][si] == ' ') modal_field[0][si] = '_';
        int r = prt_add(modal_field[0], modal_field[1], port, queue, mkdef);
        if (r < 0) {
            const char *msg = "Could not add printer (max 8).";
            int i = 0; while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; } modal_error[i] = '\0';
            draw_all(); return;
        }
        printers_refresh();
        const char *ok = "Printer added.";
        int i = 0; while (ok[i] && i < 63) { g_print_status[i] = ok[i]; i++; } g_print_status[i] = '\0';
        modal_mode = MODAL_NONE;
        draw_all();
        return;
    }

    if (modal_mode == MODAL_SET_DNS) {
        // (#786) Resolver only. Deliberately does NOT call net_set_static():
        // the machine keeps its DHCP lease, and net_persist_netcfg() writes a
        // dns-only /CONFIG/NETIP.CFG so the next boot still runs DHCP.
        int rc = net_set_dns(modal_field[0]);
        if (rc == NET_SET_DNS_EINVAL) {
            const char *msg = "DNS server is not a valid IPv4 address.";
            int i = 0; while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
            draw_all();
            return;   // modal stays open: nothing was applied
        }
        // RE-READ the stack, never echo the form. If the kernel took a
        // different value than was typed, the panel shows the kernel's.
        net_pull_live();
        if (rc == NET_SET_DNS_EPERSIST)
            save_failed("NETIP.CFG (DNS applied live, will revert on reboot)");
        modal_mode = MODAL_NONE;
        draw_all();
        return;
    }

    if (modal_mode == MODAL_SET_NETWORK) {
        // =====================================================================
        // #786: APPLY, then PERSIST, then RE-READ. All three, in that order.
        // =====================================================================
        // What this used to do, and why the owner was right to be suspicious:
        //
        //   1. net_set_static(ip, mask, gw)  - real, and it worked.
        //   2. copy the TYPED TEXT of all FOUR fields into the panel variables.
        //   3. net_write_cfg(...) - a Ring-3 write of /CONFIG/NETIP.CFG.
        //
        // Step 2 is the lie: it made the card at the top of the page agree with
        // the form no matter what the machine did, and the DNS field had no
        // apply path AT ALL (net_set_static takes three arguments). Step 3 was
        // REFUSED for every non-root user, because /CONFIG is root-owned 0711 -
        // measured on VM <vmid>: after a successful-looking OK the file did not
        // exist, and the app's own /SETLOG.TXT breadcrumb could not be written
        // either (it lives under "/", also root-owned), so the failure left no
        // trace anywhere at all.
        //
        // Now: the kernel applies AND persists (net_persist_netcfg runs inside
        // both syscalls, in Ring 0, where the permission question does not
        // arise), and this app re-reads the LIVE stack. If what comes back
        // disagrees with what was typed, the panel shows the disagreement.
        net_set_static(modal_field[0], modal_field[1], modal_field[2]);

        int dns_rc = NET_SET_DNS_OK;
        if (modal_field[3][0]) dns_rc = net_set_dns(modal_field[3]);

        // RE-READ, never echo. This is the whole fix in one line.
        net_pull_live();

        // Report the two outcomes the user must not be left to discover.
        if (dns_rc == NET_SET_DNS_EINVAL) {
            const char *msg = "DNS server is not a valid IPv4 address.";
            int i = 0; while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
            draw_all();
            return;   // keep the modal open: nothing about DNS was applied
        }
        if (dns_rc == NET_SET_DNS_EPERSIST)
            save_failed("NETIP.CFG (DNS applied live, will revert on reboot)");

        modal_mode = MODAL_NONE;
        draw_all();
        return;
    }

    if (modal_mode == MODAL_EDIT_PROFILE) {
        copy_str(users[current_user_idx].fullname, modal_field[0], 64);
        copy_str(users[current_user_idx].email,    modal_field[1], 64);
        // Persist the edited email to the real per-account store.
        useremail_set(users[current_user_idx].username, users[current_user_idx].email);
        // (#745) Persist the picked monogram color the same way - a sibling
        // side-table, /CONFIG/USERAVATAR.CFG, written as "mono:RRGGBB" (six
        // raw hex digits, no "0x" - gui_itoa_hex() always prepends "0x", the
        // wrong shape for this grammar, so this is formatted by hand).
        {
            static const char hexd[] = "0123456789ABCDEF";
            char spec[16];
            uint32_t c = avatar_palette[modal_avatar_idx] & 0x00FFFFFF;
            spec[0]='m'; spec[1]='o'; spec[2]='n'; spec[3]='o'; spec[4]=':';
            for (int i = 0; i < 6; i++)
                spec[5 + i] = hexd[(c >> (20 - i * 4)) & 0xF];
            spec[11] = '\0';
            useravatar_set(users[current_user_idx].username, spec);
            users[current_user_idx].avatar_color = avatar_palette[modal_avatar_idx];
        }
        modal_mode = MODAL_NONE;
        draw_all();
        return;
    }

    if (modal_mode == MODAL_SET_DATETIME) {
        // Parse time: field[0] = "HH:MM:SS"
        char *tf = modal_field[0];
        int h = 0, m = 0, s = 0;
        if (modal_cursor[0] >= 8) {
            h = (tf[0]-'0')*10 + (tf[1]-'0');
            m = (tf[3]-'0')*10 + (tf[4]-'0');
            s = (tf[6]-'0')*10 + (tf[7]-'0');
        }
        // Parse date: field[1] = "YYYY-MM-DD"
        char *df = modal_field[1];
        int y = 2026, mo = 1, d = 1;
        if (modal_cursor[1] >= 10) {
            y  = (df[0]-'0')*1000 + (df[1]-'0')*100 + (df[2]-'0')*10 + (df[3]-'0');
            mo = (df[5]-'0')*10 + (df[6]-'0');
            d  = (df[8]-'0')*10 + (df[9]-'0');
        }
        // #49: THE RTC HOLDS UTC (that is what NTP writes into it, and what
        // every reader now assumes). The user typed a LOCAL wall-clock time, so
        // convert back by shifting by the negated offset before storing. Without
        // this, setting the clock by hand in a +09:30 zone would store 09:30
        // ahead of UTC and every clock would then add another 09:30 on display.
        tz_shift(-tz_offset_minutes(), &h, &m, &s, &d, &mo, &y);
        set_rtc_time(h, m, s);
        set_rtc_date(y, mo, d);
        modal_mode = MODAL_NONE;
        draw_all();
        return;
    }
}

// =============================================================================
// Full Redraw
// =============================================================================

static void draw_all(void) {
    // #3: confirm the very first draw actually starts and finishes (a hang here,
    // e.g. in a panel asset load, would otherwise look like "window never shown").
    static int s_first_draw = 1;
    if (s_first_draw) setlog("SET: draw_all enter (first)");
    focus_reset();
    draw_sidebar();
    draw_content();
    if (g_focus_on && g_focus_n > 0 && modal_mode == MODAL_NONE) {
        if (g_focus_idx >= g_focus_n) g_focus_idx = 0;
        focus_rect_t *fr = &g_focus[g_focus_idx];
        // (#745) THE keyboard focus ring for the whole app, and the one a
        // live screendump caught still drawing in COL_ACCENT: measured on the
        // running machine under Retro UNIX it was accent #66BB66 on surface
        // #B4B4B4 = 1.14:1, effectively invisible, on the app people navigate
        // by Tab more than any other. gui_pal()->focus is repaired to the 3:1
        // non-text floor by gui_set_palette().
        uint32_t ring = gui_pal()->focus;
        gui_draw_rect_outline(window_handle, fr->x - 2, fr->y - 2, fr->w + 4, fr->h + 4, ring);
        gui_draw_rect_outline(window_handle, fr->x - 1, fr->y - 1, fr->w + 2, fr->h + 2, ring);
    }
    if (modal_mode != MODAL_NONE) draw_modal();
    dropdown_render();                 // overlay: open dropdown list on top of all
    // (#267) Register a hover tooltip for each sidebar panel row, draw the
    // context-help "?" icon, then paint any due tooltip on top.
    for (int i = 0; i < PANEL_COUNT; i++) {
        int y = sidebar_row_y(i);
        // Only rows actually on screen get a tooltip region, otherwise a scrolled
        // -away panel would still claim hover area over the title or the footer.
        if (y < g_side_scroll.y ||
            y + PANEL_ROW_H > g_side_scroll.y + g_side_scroll.h) continue;
        help_ui_register(window_handle, 4, y, SIDEBAR_WIDTH - 8, PANEL_ROW_H, panel_names[i]);
    }
    help_ui_register(window_handle, HELP_Q_X, HELP_Q_Y, HELP_Q_D, HELP_Q_D,
                     "Open help for this panel (F1)");
    help_ui_question_icon(window_handle, HELP_Q_X, HELP_Q_Y, HELP_Q_D);
    help_ui_draw(window_handle);
    win_invalidate(window_handle);
    if (s_first_draw) { setlog("SET: draw_all done (first)"); s_first_draw = 0; }
}



// =============================================================================
// Event Handling
// =============================================================================

static void handle_sidebar_click(int local_x, int local_y) {
    // A press in the scrollbar gutter belongs to the scrollbar, not to a row:
    // handle it first and do not let it also select whatever panel is behind it.
    if (gui_scroll_press(&g_side_scroll, local_x, local_y)) { draw_all(); return; }

    {
        int i = sidebar_hit(local_y);
        if (i >= 0) {
            if (current_panel != i) {
                current_panel = i;
                gui_scroll_set(&g_content_scroll, 0);
                // (#262) Re-read the Default Apps state once on panel entry (picks
                // up external /ASSOC.CFG changes) instead of per redraw frame.
                if (i == PANEL_DEFAULTS) defaults_invalidate_cache();
                // (#745 task #67) Re-read pinned favourites + rebuild the
                // pinnable-apps catalog once on entering Dock, same idiom as
                // Default Apps above (picks up a dock right-click Pin/Unpin
                // or an App Store install that happened while this panel was
                // closed).
                if (i == PANEL_DOCK) dockfav_invalidate_cache();
                // (#318) On entering Devices, load printers (seed the Brother
                // example once if nothing is configured).
                if (i == PANEL_DEVICES) { printers_seed_once(); printers_refresh(); g_print_status[0] = '\0'; }
                // Refresh real cache/trash sizes from disk on entering Storage.
                if (i == PANEL_STORAGE) storage_scan();
                // Populate live network data when switching to Network panel
                if (i == PANEL_NETWORK) {
                    net_pull_live();   // #786: one definition, see above
                }
                draw_all();
            }
        }
    }
}

static void handle_sidebar_hover(int local_x, int local_y) {
    (void)local_x;

    // sidebar_hit() applies the scroll offset, so hover and click always agree
    // about which row is under the pointer.
    int new_hover = sidebar_hit(local_y);

    if (new_hover != hover_panel) {
        hover_panel = new_hover;
        draw_all();
    }
}

// Dropdown on_change handlers: apply the newly-selected value live (dropdown_click
// has already written the new index into the bound variable before calling these).
static void font_dd_changed(void)      { set_font_size(font_size); }
static void icon_dd_changed(void)      { set_icon_size(icon_size); }
// #745 (local 102): persists immediately (matches every other dropdown here -
// e.g. font_dd_changed above), but this one does NOT take effect until the
// next boot (kernel/proc/syscall.c SYS_SET_ROTATION comment explains why).
// rotation_active is untouched, so draw_display_panel()'s "applies after
// restart" hint keeps showing until the user actually reboots.
static void rotation_dd_changed(void)  { set_display_rotation(rotation_idx); }
// #387: publish the chosen dock layout via /DOCKSTYL.CFG; the compositor polls
// the file and applies + persists it live (no restart).
static void dock_dd_changed(void) {
    dock_style = DOCK_CLAMP(dock_style);   // #745: derived, not a literal bound
    int fd = userconf_open_write("DOCKSTYL.CFG");   // #683: per-user, see compositor main.c
    if (fd < 0) return;
    char c = (char)('0' + dock_style);
    // #743: the compositor POLLS this file and applies what it finds, so a
    // silently failed write left the dock disagreeing with the control.
    if (userconf_finish_write(fd, &c, 1) != 0) save_failed("DOCKSTYL.CFG");
}
// (#745) Publish the chosen glass opacity via /CONFIG/DOCKOPAC.CFG; the
// compositor polls the file (same site and cadence as DOCKSTYL.CFG) and applies
// it live, then persists it into UIPROFIL.YML. Same userconf pair, so the #683
// per-user path and its legacy-root read fallback both come along unchanged.
static void dock_opacity_write(void) {
    if (dock_opacity < DOCK_OPACITY_MIN) dock_opacity = DOCK_OPACITY_MIN;
    if (dock_opacity > DOCK_OPACITY_MAX) dock_opacity = DOCK_OPACITY_MAX;
    int fd = userconf_open_write("DOCKOPAC.CFG");
    if (fd < 0) { save_failed("DOCKOPAC.CFG"); return; }
    char b[4];
    int n = 0;
    if (dock_opacity >= 100) { b[n++] = '1'; b[n++] = '0'; b[n++] = '0'; }
    else { b[n++] = (char)('0' + dock_opacity / 10); b[n++] = (char)('0' + dock_opacity % 10); }
    // #743: the compositor POLLS this file and applies what it finds, so a
    // silently failed write leaves the dock disagreeing with the control.
    if (userconf_finish_write(fd, b, (unsigned long)n) != 0) save_failed("DOCKOPAC.CFG");
}

// (#123 items 2/3) Publish the marble dock's height and hover-zoom the same way
// the opacity slider publishes its own value: an ASCII integer in a
// per-user CFG that the compositor polls (main.c dock_geom_poll), applies live
// and persists into UIPROFIL.YML. Deliberately NOT a second mechanism - same
// userconf pair, so the #683 per-user path and its legacy-root read fallback
// come along unchanged, and #743's "a silently failed write leaves the control
// disagreeing with the dock" reporting comes along too.
static void dock_num_write(const char *name, int v) {
    int fd = userconf_open_write(name);
    if (fd < 0) { save_failed(name); return; }
    char b[8];
    int n = 0;
    if (v >= 100) { b[n++] = (char)('0' + v / 100); v %= 100; b[n++] = (char)('0' + v / 10); b[n++] = (char)('0' + v % 10); }
    else          { b[n++] = (char)('0' + v / 10);  b[n++] = (char)('0' + v % 10); }
    if (userconf_finish_write(fd, b, (unsigned long)n) != 0) save_failed(name);
}
static void dock_height_write(void) {
    if (dock_height < DOCK_HEIGHT_MIN) dock_height = DOCK_HEIGHT_MIN;
    if (dock_height > DOCK_HEIGHT_MAX) dock_height = DOCK_HEIGHT_MAX;
    dock_num_write("DOCKHGT.CFG", dock_height);
}
static void dock_zoom_write(void) {
    if (dock_zoom < DOCK_ZOOM_MIN) dock_zoom = DOCK_ZOOM_MIN;
    if (dock_zoom > DOCK_ZOOM_MAX) dock_zoom = DOCK_ZOOM_MAX;
    dock_num_write("DOCKZOOM.CFG", dock_zoom);
}

static int ss_delay_index(void) {
    int best = 0;
    for (int i = 0; i < SS_DELAY_NSTEPS; i++)
        if (SS_DELAY_STEPS[i] <= screensaver_delay_min) best = i;
    return best;
}
static void ss_delay_label(char *buf) {
    gui_itoa(screensaver_delay_min, buf, 12);
    int n = 0; while (buf[n]) n++;
    buf[n++] = ' '; buf[n++] = 'm'; buf[n++] = 'i'; buf[n++] = 'n'; buf[n] = 0;
}
static void ss_set_delay_index(int di) {
    if (di < 0) di = 0;
    if (di >= SS_DELAY_NSTEPS) di = SS_DELAY_NSTEPS - 1;
    screensaver_delay_min = SS_DELAY_STEPS[di];
    set_ss_delay(screensaver_delay_min * 60);
}
static void ss_dd_changed(void)        { set_screensaver(SS_KERNEL_MAP[screensaver_idx]); }
/* (#116) live cursor: 0/1/2 = Light/Dark/Glow. This is the ONLY place Settings
 * is allowed to write the cursor: a real user selection in the picker.
 * #745: the size argument was a hardcoded 100, so choosing a style also threw
 * away whatever cursor SIZE the compositor had persisted in UIPROFIL.YML's
 * cursize. Read the live size back out of the kernel and preserve it. */
static void cursor_dd_changed(void) {
    cursor_theme = OPT_CLAMP(cursor_theme, CURSOR_OPTS);
    int pk = get_cursor();                       /* packed style | size<<8 */
    int sz = (pk >= 0) ? ((pk >> 8) & 0xFFFF) : 100;
    if (sz < 50 || sz > 250) sz = 100;
    set_cursor_theme(CURSOR_KERNEL_MAP[cursor_theme]);
    set_cursor(cursor_theme, sz);
}
static void wallpaper_dd_changed(void) { set_wallpaper(wallpaper_idx); }

// =============================================================================
// #414 External Services (Home Assistant) panel: base URL + long-lived token
// (masked) + refresh + Test Connection. Persists to /CONFIG/EXTSVC.CFG, the same
// file the background haservice reads.
// =============================================================================
static char g_ext_url[128]   = "";
static char g_ext_token[400] = "";
static char g_ext_refresh[8] = "10";
static char g_ext_status[96] = "";
static int  g_ext_focus      = -1;   // 0=url 1=token 2=refresh, -1=none
static int  g_ext_loaded     = 0;
static char ext_httpbuf[1050000];

static int  ex_len(const char *a){ int n=0; while(a[n]) n++; return n; }
static int  ex_has(const char *s,const char *k){ int kl=ex_len(k); for(int i=0;s[i];i++){int j=0;while(j<kl&&s[i+j]==k[j])j++; if(j==kl)return 1;} return 0; }
static void ex_itoa(int v,char *o){ char t[12]; int n=0; if(v==0)t[n++]='0'; while(v){t[n++]=(char)('0'+v%10);v/=10;} int i=0; while(n) o[i++]=t[--n]; o[i]=0; }
static void ex_set_status(const char *m){ int i=0; for(;m[i]&&i<95;i++) g_ext_status[i]=m[i]; g_ext_status[i]=0; }

static void ext_load_cfg(void){
    g_ext_loaded = 1;
    int fd = sys_open("/CONFIG/EXTSVC.CFG", 0);
    if (fd < 0) fd = sys_open("/EXTSVC.CFG", 0);
    if (fd < 0) return;
    static char b[1600]; int got=0;
    while(got<(int)sizeof(b)-1){ long n=sys_read(fd,b+got,sizeof(b)-1-got); if(n<=0) break; got+=(int)n; }
    b[got]=0; sys_close(fd);
    int i=0;
    while(b[i]){
        int ls=i; while(b[i]&&b[i]!='\n'&&b[i]!='\r') i++; int le=i; while(b[i]=='\n'||b[i]=='\r') i++;
        int eq=-1; for(int j=ls;j<le;j++){ if(b[j]=='='){eq=j;break;} } if(eq<0) continue;
        char key[16]; int k=0; for(int j=ls;j<eq&&k<15;j++) key[k++]=b[j]; key[k]=0;
        int ve=le; while(ve>eq+1&&b[ve-1]==' ') ve--;
        if(ex_has(key,"url")){ int n=0; for(int j=eq+1;j<ve&&n<127;j++) g_ext_url[n++]=b[j]; g_ext_url[n]=0; }
        else if(ex_has(key,"token")){ int n=0; for(int j=eq+1;j<ve&&n<399;j++) g_ext_token[n++]=b[j]; g_ext_token[n]=0; }
        else if(ex_has(key,"refresh")){ int n=0; for(int j=eq+1;j<ve&&n<7;j++) g_ext_refresh[n++]=b[j]; g_ext_refresh[n]=0; }
    }
}
static void ext_save_cfg(void){
    // #743: three faults here. (a) The buffer was built AFTER the open, so the
    // fallback could not be retried with it. (b) Neither open carried O_TRUNC,
    // which on a FAT-backed path leaves the tail of a longer previous config
    // behind (on ext2 the write replaces the file regardless, so this was
    // filesystem-dependent, not universal). (c) It reported "Saved" without
    // ever looking at the write or the close, and on an ext2 fd the close IS
    // the write, so a full volume produced a cheerful success message and no
    // config. Build first, then write, then tell the truth.
    static char out[700]; int o=0;
    const char *k1="url="; for(int j=0;k1[j];j++) out[o++]=k1[j]; for(int j=0;g_ext_url[j];j++) out[o++]=g_ext_url[j]; out[o++]='\n';
    const char *k2="token="; for(int j=0;k2[j];j++) out[o++]=k2[j]; for(int j=0;g_ext_token[j];j++) out[o++]=g_ext_token[j]; out[o++]='\n';
    const char *k3="refresh="; for(int j=0;k3[j];j++) out[o++]=k3[j]; for(int j=0;g_ext_refresh[j];j++) out[o++]=g_ext_refresh[j]; out[o++]='\n';
    if (userconf_write_all("/CONFIG/EXTSVC.CFG", out, (unsigned long)o) == 0) {
        ex_set_status("Saved to /CONFIG/EXTSVC.CFG");
    } else if (userconf_write_all("/EXTSVC.CFG", out, (unsigned long)o) == 0) {
        ex_set_status("Saved to /EXTSVC.CFG");
    } else {
        save_failed("EXTSVC.CFG");
        ex_set_status("Could not write config - NOT saved");
    }
}
// Blocking, user-initiated connectivity test (not on the draw path).
static void ext_test_connection(void){
    if(!g_ext_url[0]||!g_ext_token[0]){ ex_set_status("Enter base URL and token first"); return; }
    if(!sys_net_is_up()){ ex_set_status("Network is down"); return; }
    ex_set_status("Testing...");
    char url[220]; int u=0; for(int j=0;g_ext_url[j]&&u<180;j++) url[u++]=g_ext_url[j];
    const char *ap="/api/states"; for(int j=0;ap[j];j++) url[u++]=ap[j]; url[u]=0;
    char hdr[460]; int h=0; const char *pf="Authorization: Bearer ";
    for(int j=0;pf[j];j++) hdr[h++]=pf[j]; for(int j=0;g_ext_token[j]&&h<440;j++) hdr[h++]=g_ext_token[j]; hdr[h++]='\r'; hdr[h++]='\n'; hdr[h]=0;
    unsigned int bytes=0; int status=0;
    int r=sys_http_fetch_hdr(url,hdr,ext_httpbuf,sizeof(ext_httpbuf)-1,&bytes,&status);
    if(r>=0 && status==200 && bytes>0){
        ext_httpbuf[bytes<sizeof(ext_httpbuf)?bytes:sizeof(ext_httpbuf)-1]=0;
        int cnt=0; const char *k="\"entity_id\"", *p=ext_httpbuf; int kl=ex_len(k);
        for(int i=0;p[i];i++){ int j=0; while(j<kl&&p[i+j]==k[j])j++; if(j==kl) cnt++; }
        char num[12]; ex_itoa(cnt,num);
        char m[96]; int mi=0; const char *c1="Connected - "; for(int j=0;c1[j];j++) m[mi++]=c1[j];
        for(int j=0;num[j];j++) m[mi++]=num[j]; const char *c2=" entities"; for(int j=0;c2[j];j++) m[mi++]=c2[j]; m[mi]=0;
        ex_set_status(m);
    } else if(status==401||status==403){ ex_set_status("Auth failed - check token (HTTP 401)"); }
    else { char m[64]; int mi=0; const char *c="Failed (HTTP "; for(int j=0;c[j];j++) m[mi++]=c[j]; char num[12]; ex_itoa(status,num); for(int j=0;num[j];j++) m[mi++]=num[j]; m[mi++]=')'; m[mi]=0; ex_set_status(m); }
}

// Field rectangles (kept in sync between draw and click). (#227) content area
// now scrolls: these already avoided PART A's class of drift by being a
// shared function both draw and handler call, so applying the offset once
// here keeps them scroll-consistent too.
static void ext_field_rect(int idx,int *fx,int *fy,int *fw){
    int x = CONTENT_X + PADDING, y = PADDING + 40 - g_content_scroll.offset;
    *fx = x + 15; *fw = CONTENT_WIDTH - 2*PADDING - 30;
    *fy = y + 28 + idx*52;
}
static void ext_btn_test_rect(int *bx,int *by,int *bw){ int x=CONTENT_X+PADDING, y=PADDING+40-g_content_scroll.offset; *bx=x+15; *by=y+28+3*52+6; *bw=150; }
static void ext_btn_save_rect(int *bx,int *by,int *bw){ int x=CONTENT_X+PADDING, y=PADDING+40-g_content_scroll.offset; *bx=x+175; *by=y+28+3*52+6; *bw=110; }

// =============================================================================
// #367 AI Provider (LLM) config - provider-agnostic. Persists /CONFIG/AISVC.CFG,
// which userland/libc aiclient.c reads (endpoint/model/api_key/api_style). Most
// providers are OpenAI-compatible (api_style=bearer); Anthropic uses its own
// Messages API (api_style=anthropic).
// =============================================================================
typedef struct { const char *name; const char *endpoint; const char *model; int style; } ai_preset_t;
static const ai_preset_t g_ai_presets[] = {
    {"OpenAI",          "https://api.openai.com/v1/chat/completions",      "gpt-4o-mini",              0},
    {"Anthropic",       "https://api.anthropic.com/v1/messages",           "claude-3-5-sonnet-latest", 1},
    {"Moonshot (Kimi)", "https://api.moonshot.ai/v1/chat/completions",     "kimi-k2.6",                0},
    {"OpenRouter",      "https://openrouter.ai/api/v1/chat/completions",   "openai/gpt-4o-mini",       0},
    {"Groq",            "https://api.groq.com/openai/v1/chat/completions", "llama-3.3-70b-versatile",  0},
    {"DeepSeek",        "https://api.deepseek.com/v1/chat/completions",    "deepseek-chat",            0},
    {"Ollama (local)",  "http://localhost:11434/v1/chat/completions",      "llama3",                   0},
    {"Custom",          "",                                                "",                         0},
};
#define AI_NPRESET ((int)(sizeof(g_ai_presets)/sizeof(g_ai_presets[0])))
static char g_ai_endpoint[200] = "";
static char g_ai_model[96]     = "";
static char g_ai_key[300]      = "";
static int  g_ai_style         = 0;   // 0=bearer 1=anthropic
static int  g_ai_preset        = 2;   // default Moonshot(Kimi)
static char g_ai_status[96]    = "";
static int  g_ai_focus         = -1;  // 0=endpoint 1=model 2=key
static int  g_ai_loaded        = 0;

static void ai_set_status(const char *m){ int i=0; for(;m[i]&&i<95;i++) g_ai_status[i]=m[i]; g_ai_status[i]=0; }
static void ai_cpy(char *d,int cap,const char *s){ int i=0; for(;s[i]&&i<cap-1;i++) d[i]=s[i]; d[i]=0; }

static void ai_apply_preset(int idx){
    g_ai_preset = idx;
    if(idx<0||idx>=AI_NPRESET) return;
    if(g_ai_presets[idx].endpoint[0]) ai_cpy(g_ai_endpoint,sizeof(g_ai_endpoint),g_ai_presets[idx].endpoint);
    if(g_ai_presets[idx].model[0])    ai_cpy(g_ai_model,sizeof(g_ai_model),g_ai_presets[idx].model);
    g_ai_style = g_ai_presets[idx].style;
}
static void ai_match_preset(void){
    for(int i=0;i<AI_NPRESET;i++)
        if(g_ai_presets[i].endpoint[0] && ex_has(g_ai_endpoint,g_ai_presets[i].endpoint) &&
           ex_len(g_ai_endpoint)==ex_len(g_ai_presets[i].endpoint)){ g_ai_preset=i; return; }
    g_ai_preset = AI_NPRESET-1;   // Custom
}
static void ai_load_cfg(void){
    g_ai_loaded = 1;
    // #684: the user's own protected copy only. No /CONFIG fallback and no
    // KIMI.KEY read: the seed reaches this file via the kernel provisioner.
    int fd = userconf_open_read("AISVC.CFG", 0);
    if (fd < 0){
        // No config yet: default to Moonshot(Kimi); surface an existing KIMI.KEY masked.
        ai_apply_preset(2);
        // #684: Settings no longer reads /CONFIG/KIMI.KEY to pre-fill the key
        // field. That read was the last userland opener of the seed, and it is
        // exactly what the user's decision removes. On a machine that HAS a
        // seed, the kernel provisioner has already written it into this user's
        // AISVC.CFG before the desktop starts, so the branch above finds it and
        // this one is not reached. On a machine with no seed the field is
        // correctly empty and the user types their own key.
        return;
    }
    static char b[1200]; int got=0;
    while(got<(int)sizeof(b)-1){ long n=sys_read(fd,b+got,sizeof(b)-1-got); if(n<=0) break; got+=(int)n; }
    b[got]=0; sys_close(fd);
    int i=0;
    while(b[i]){
        int ls=i; while(b[i]&&b[i]!='\n'&&b[i]!='\r') i++; int le=i; while(b[i]=='\n'||b[i]=='\r') i++;
        int eq=-1; for(int j=ls;j<le;j++){ if(b[j]=='='){eq=j;break;} } if(eq<0) continue;
        char key[16]; int k=0; for(int j=ls;j<eq&&k<15;j++) key[k++]=b[j]; key[k]=0;
        int ve=le; while(ve>eq+1&&b[ve-1]==' ') ve--;
        if(ex_has(key,"endpoint"))       { int n=0; for(int j=eq+1;j<ve&&n<199;j++) g_ai_endpoint[n++]=b[j]; g_ai_endpoint[n]=0; }
        else if(ex_has(key,"model"))     { int n=0; for(int j=eq+1;j<ve&&n<95;j++)  g_ai_model[n++]=b[j];    g_ai_model[n]=0; }
        else if(ex_has(key,"api_key"))   { int n=0; for(int j=eq+1;j<ve&&n<299;j++) g_ai_key[n++]=b[j];      g_ai_key[n]=0; }
        else if(ex_has(key,"api_style")) { g_ai_style = ex_has(b+eq+1,"anthropic") ? 1 : 0; }
    }
    ai_match_preset();
}
static void ai_save_cfg(void){
    // #684: write the user's own copy (O_TRUNC too: the old open lacked it,
    // so a shorter config left a stale tail behind).
    int fd = userconf_open_write("AISVC.CFG");
    if (fd < 0){ ai_set_status("Could not write config"); return; }
    static char out[900]; int o=0;
    const char *pn=(g_ai_preset>=0&&g_ai_preset<AI_NPRESET)?g_ai_presets[g_ai_preset].name:"Custom";
    const char *k0="provider=";  for(int j=0;k0[j];j++) out[o++]=k0[j]; for(int j=0;pn[j];j++) out[o++]=pn[j]; out[o++]='\n';
    const char *k1="endpoint=";  for(int j=0;k1[j];j++) out[o++]=k1[j]; for(int j=0;g_ai_endpoint[j];j++) out[o++]=g_ai_endpoint[j]; out[o++]='\n';
    const char *k2="model=";     for(int j=0;k2[j];j++) out[o++]=k2[j]; for(int j=0;g_ai_model[j];j++)    out[o++]=g_ai_model[j];    out[o++]='\n';
    const char *k3="api_key=";   for(int j=0;k3[j];j++) out[o++]=k3[j]; for(int j=0;g_ai_key[j];j++)      out[o++]=g_ai_key[j];      out[o++]='\n';
    const char *k4="api_style="; for(int j=0;k4[j];j++) out[o++]=k4[j]; const char *st=g_ai_style?"anthropic":"bearer"; for(int j=0;st[j];j++) out[o++]=st[j]; out[o++]='\n';
    // #743: said "Saved" without checking anything. On an ext2 fd the close is
    // where the bytes are actually written, so this claimed success on exactly
    // the failures it should have reported.
    if (userconf_finish_write(fd, out, (unsigned long)o) == 0) {
        ai_set_status("Saved to your account settings");
    } else {
        save_failed("AISVC.CFG");
        ai_set_status("Could not write config - NOT saved");
    }
}
// Real, user-initiated minimal POST to the configured provider (not on draw path).
static void ai_test(void){
    if(!g_ai_endpoint[0]||!g_ai_key[0]){ ai_set_status("Enter endpoint and API key first"); return; }
    if(!sys_net_is_up()){ ai_set_status("Network is down"); return; }
    ai_set_status("Testing...");
    static char hdr[400]; int h=0;
    if(g_ai_style){ const char *p1="x-api-key: "; for(int j=0;p1[j];j++) hdr[h++]=p1[j]; for(int j=0;g_ai_key[j]&&h<330;j++) hdr[h++]=g_ai_key[j];
        const char *p2="\r\nanthropic-version: 2023-06-01\r\n"; for(int j=0;p2[j];j++) hdr[h++]=p2[j]; hdr[h]=0; }
    else { const char *p1="Authorization: Bearer "; for(int j=0;p1[j];j++) hdr[h++]=p1[j]; for(int j=0;g_ai_key[j]&&h<360;j++) hdr[h++]=g_ai_key[j]; hdr[h++]='\r'; hdr[h++]='\n'; hdr[h]=0; }
    static char body[400]; int bo=0;
    const char *m1="{\"model\":\""; for(int j=0;m1[j];j++) body[bo++]=m1[j]; for(int j=0;g_ai_model[j];j++) body[bo++]=g_ai_model[j];
    if(g_ai_style){ const char *m2="\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}"; for(int j=0;m2[j];j++) body[bo++]=m2[j]; }
    else          { const char *m2="\",\"max_tokens\":16,\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}]}"; for(int j=0;m2[j];j++) body[bo++]=m2[j]; }
    body[bo]=0;
    int status=0; int r=sys_http_post(g_ai_endpoint,hdr,body,ext_httpbuf,sizeof(ext_httpbuf)-1,&status);
    if(r>=0 && status==200)               ai_set_status("Connected - provider OK (HTTP 200)");
    else if(status==401||status==403)     ai_set_status("Auth failed - check API key");
    else if(status>0){ char m[64]; int mi=0; const char *c="Provider error (HTTP "; for(int j=0;c[j];j++) m[mi++]=c[j]; char num[12]; ex_itoa(status,num); for(int j=0;num[j];j++) m[mi++]=num[j]; m[mi++]=')'; m[mi]=0; ai_set_status(m); }
    else                                  ai_set_status("No response - check endpoint/network");
}
static int  ai_card_y(void){ return PADDING + 40 + 246 + 14 - g_content_scroll.offset; }   // (#227)
static void ai_field_rect(int idx,int *fx,int *fy,int *fw){ int x=CONTENT_X+PADDING; *fx=x+15; *fw=CONTENT_WIDTH-2*PADDING-30; *fy=ai_card_y()+64+idx*46; }
static void ai_preset_rect(int *bx,int *by,int *bw){ int x=CONTENT_X+PADDING; *bx=x+15;  *by=ai_card_y()+30; *bw=CONTENT_WIDTH-2*PADDING-30; }
static void ai_btn_test_rect(int *bx,int *by,int *bw){ int x=CONTENT_X+PADDING; *bx=x+15;  *by=ai_card_y()+64+3*46+6; *bw=130; }
static void ai_btn_save_rect(int *bx,int *by,int *bw){ int x=CONTENT_X+PADDING; *bx=x+155; *by=ai_card_y()+64+3*46+6; *bw=100; }

static void draw_extsvc_panel(void){
    if(!g_ext_loaded) ext_load_cfg();
    int x = CONTENT_X + PADDING, y = PADDING - g_content_scroll.offset;   // (#227) content area now scrolls
    draw_section_header(x, y, "External Services");
    y += 40;
    draw_card(x, y, CONTENT_WIDTH - 2*PADDING, 246);
    win_draw_text(window_handle, x+15, y+8, "Home Assistant", COL_TEXT_PRIMARY);
    const char *labels[3] = {"Base URL (http://host:8123)","Long-Lived Access Token","Refresh interval (seconds)"};
    for(int i=0;i<3;i++){
        int fx,fy,fw; ext_field_rect(i,&fx,&fy,&fw);
        win_draw_text(window_handle, fx, fy-14, labels[i], COL_TEXT_SECONDARY);
        win_draw_rect(window_handle, fx, fy, fw, 24, COL_INPUT_BG);
        // (#745) gui_pal()->focus, not COL_ACCENT. This field is hand-drawn
        // rather than a gui_textfield2(), so it did not inherit the shared
        // engine's focus token and kept drawing the ring in the theme accent:
        // 1.76:1 on the Dark theme's surface, below the 3:1 non-text floor.
        // gui_pal()->focus is repaired to the floor by gui_set_palette().
        gui_draw_rect_outline(window_handle, fx, fy, fw, 24, (g_ext_focus==i)?gui_pal()->focus:COL_INPUT_BORDER);
        char disp[64]; int di=0;
        const char *src = (i==0)?g_ext_url : (i==1)?g_ext_token : g_ext_refresh;
        if(i==1){ int L=ex_len(src); int show=L>24?24:L; for(int j=0;j<show;j++) disp[di++]='*'; }
        else { for(int j=0;src[j]&&di<40;j++) disp[di++]=src[j]; }
        if(g_ext_focus==i) disp[di++]='_';
        disp[di]=0;
        win_draw_text(window_handle, fx+6, fy+5, disp, COL_TEXT_PRIMARY);
    }
    int bx,by,bw; ext_btn_test_rect(&bx,&by,&bw); draw_button(bx,by,bw,"Test Connection",true,false);
    ext_btn_save_rect(&bx,&by,&bw); draw_button(bx,by,bw,"Save",false,false);
    if(g_ext_status[0]) win_draw_text(window_handle, x+15, by+42, g_ext_status, COL_TEXT_SECONDARY);

    // #367 AI Provider (LLM) card
    if(!g_ai_loaded) ai_load_cfg();
    int aiy = ai_card_y();
    draw_card(x, aiy, CONTENT_WIDTH - 2*PADDING, 268);
    win_draw_text(window_handle, x+15, aiy+8, "AI Provider (LLM)", COL_TEXT_PRIMARY);
    { int px,py,pw; ai_preset_rect(&px,&py,&pw);
      win_draw_text(window_handle, px, py-14, "Provider (click to change)", COL_TEXT_SECONDARY);
      win_draw_rect(window_handle, px, py, pw, 24, COL_INPUT_BG);
      gui_draw_rect_outline(window_handle, px, py, pw, 24, COL_INPUT_BORDER);
      const char *pn=(g_ai_preset>=0&&g_ai_preset<AI_NPRESET)?g_ai_presets[g_ai_preset].name:"Custom";
      char pl[72]; int pi=0; for(int j=0;pn[j]&&pi<40;j++) pl[pi++]=pn[j];
      const char *sfx=g_ai_style?"   [Messages API]":"   [OpenAI-compatible]"; for(int j=0;sfx[j]&&pi<70;j++) pl[pi++]=sfx[j]; pl[pi]=0;
      win_draw_text(window_handle, px+6, py+5, pl, COL_TEXT_PRIMARY);
    }
    const char *ailabels[3]={"Endpoint URL","Model","API Key"};
    for(int i=0;i<3;i++){
        int fx,fy,fw; ai_field_rect(i,&fx,&fy,&fw);
        win_draw_text(window_handle, fx, fy-14, ailabels[i], COL_TEXT_SECONDARY);
        win_draw_rect(window_handle, fx, fy, fw, 24, COL_INPUT_BG);
        // (#745) see the note on the External-services field above.
        gui_draw_rect_outline(window_handle, fx, fy, fw, 24, (g_ai_focus==i)?gui_pal()->focus:COL_INPUT_BORDER);
        char disp[80]; int di=0;
        const char *src=(i==0)?g_ai_endpoint : (i==1)?g_ai_model : g_ai_key;
        if(i==2){ int L=ex_len(src); int show=L>24?24:L; for(int j=0;j<show;j++) disp[di++]='*'; }
        else { for(int j=0;src[j]&&di<56;j++) disp[di++]=src[j]; }
        if(g_ai_focus==i) disp[di++]='_';
        disp[di]=0;
        win_draw_text(window_handle, fx+6, fy+5, disp, COL_TEXT_PRIMARY);
    }
    { int abx,aby,abw; ai_btn_test_rect(&abx,&aby,&abw); draw_button(abx,aby,abw,"Test",true,false);
      ai_btn_save_rect(&abx,&aby,&abw); draw_button(abx,aby,abw,"Save",false,false);
      if(g_ai_status[0]) win_draw_text(window_handle, x+15, aby+42, g_ai_status, COL_TEXT_SECONDARY);
      g_content_bottom_y = aby + 60;   // (#227)
    }
}

// Panel key handler: types into the focused field. Returns 1 if consumed.
static int ext_key(int ch,int keycode){
    (void)keycode;
    if(g_ai_focus>=0){
        char *abuf=(g_ai_focus==0)?g_ai_endpoint : (g_ai_focus==1)?g_ai_model : g_ai_key;
        int acap=(g_ai_focus==0)?199 : (g_ai_focus==1)?95 : 299;
        int AL=ex_len(abuf);
        if(ch==27){ g_ai_focus=-1; return 1; }
        if(ch=='\t'){ g_ai_focus=(g_ai_focus+1)%3; return 1; }
        if(ch=='\b'||ch==8||ch==127){ if(AL>0) abuf[AL-1]=0; return 1; }
        if(ch=='\r'||ch=='\n'){ g_ai_focus=-1; return 1; }
        if(ch>=0x20 && ch<0x7F && AL<acap){ abuf[AL]=(char)ch; abuf[AL+1]=0; return 1; }
        return 1;
    }
    if(g_ext_focus<0) return 0;
    char *buf = (g_ext_focus==0)?g_ext_url : (g_ext_focus==1)?g_ext_token : g_ext_refresh;
    int cap = (g_ext_focus==0)?127 : (g_ext_focus==1)?399 : 7;
    int L=ex_len(buf);
    if(ch==27){ g_ext_focus=-1; return 1; }
    if(ch=='\t'){ g_ext_focus=(g_ext_focus+1)%3; return 1; }
    if(ch=='\b'||ch==8||ch==127){ if(L>0) buf[L-1]=0; return 1; }
    if(ch=='\r'||ch=='\n'){ g_ext_focus=-1; return 1; }
    if(ch>=0x20 && ch<0x7F && L<cap){ buf[L]=(char)ch; buf[L+1]=0; return 1; }
    return 1;
}

static void handle_content_click(int local_x, int local_y) {
    int x = CONTENT_X + PADDING;
    // (#227) ONE line applies the current scroll offset to every hit test in
    // this function uniformly, converted or not: base_y always matches the
    // y-origin every draw_X_panel() now draws from (PADDING -
    // g_content_scroll.offset), so scrolling the content area can never by
    // itself desynchronize a hit box from what is on screen.
    int base_y = PADDING - g_content_scroll.offset;

    switch (current_panel) {
        case PANEL_EXTSVC: {
            for (int i = 0; i < 3; i++) { int fx,fy,fw; ext_field_rect(i,&fx,&fy,&fw);
                if (local_x>=fx && local_x<fx+fw && local_y>=fy && local_y<fy+24) { g_ext_focus=i; g_ai_focus=-1; draw_all(); return; } }
            int bx,by,bw; ext_btn_test_rect(&bx,&by,&bw);
            if (local_x>=bx && local_x<bx+bw && local_y>=by && local_y<by+30) { ext_test_connection(); draw_all(); return; }
            ext_btn_save_rect(&bx,&by,&bw);
            if (local_x>=bx && local_x<bx+bw && local_y>=by && local_y<by+30) { ext_save_cfg(); draw_all(); return; }
            // #367 AI Provider card: preset selector cycles on click; fields; Test/Save.
            { int px,py,pw; ai_preset_rect(&px,&py,&pw);
              if (local_x>=px && local_x<px+pw && local_y>=py && local_y<py+24) { ai_apply_preset((g_ai_preset+1)%AI_NPRESET); g_ai_focus=-1; g_ext_focus=-1; ai_set_status(""); draw_all(); return; } }
            for (int i = 0; i < 3; i++) { int fx,fy,fw; ai_field_rect(i,&fx,&fy,&fw);
                if (local_x>=fx && local_x<fx+fw && local_y>=fy && local_y<fy+24) { g_ai_focus=i; g_ext_focus=-1; draw_all(); return; } }
            ai_btn_test_rect(&bx,&by,&bw);
            if (local_x>=bx && local_x<bx+bw && local_y>=by && local_y<by+30) { ai_test(); draw_all(); return; }
            ai_btn_save_rect(&bx,&by,&bw);
            if (local_x>=bx && local_x<bx+bw && local_y>=by && local_y<by+30) { ai_save_cfg(); draw_all(); return; }
            g_ext_focus = -1; g_ai_focus = -1; draw_all(); return;
        }
        case PANEL_BLUETOOTH: {
            for (int i = 0; i < g_bt_nhits; i++) {
                bt_hit_t *h = &g_bt_hits[i];
                if (local_x < h->x || local_x >= h->x + h->w ||
                    local_y < h->y || local_y >= h->y + h->h) continue;
                switch (h->action) {
                    case BTA_POWER:
                        bt_power(bt_is_powered() ? 0 : 1);
                        if (!bt_is_powered()) g_bt_pend_active = 0;
                        break;
                    case BTA_SCAN:
                        if (bt_scan_active()) bt_scan_stop();
                        else                  bt_scan_start();
                        break;
                    case BTA_PAIR:
                        if (h->dev >= 0 && h->dev < g_bt_ndev) {
                            bt_pair(&g_bt_dev[h->dev].addr);
                            g_bt_pend_addr = g_bt_dev[h->dev].addr;
                            g_bt_pend_target = BT_LINK_PAIRED;
                            g_bt_pend_active = 1;
                        }
                        break;
                    case BTA_CONN:
                        if (h->dev >= 0 && h->dev < g_bt_ndev) {
                            if (g_bt_dev[h->dev].connected) {
                                bt_disconnect_dev(&g_bt_dev[h->dev].addr);
                            } else {
                                bt_connect(&g_bt_dev[h->dev].addr);
                                g_bt_pend_addr = g_bt_dev[h->dev].addr;
                                g_bt_pend_target = BT_LINK_CONNECTED;
                                g_bt_pend_active = 1;
                            }
                        }
                        break;
                    case BTA_FORGET:
                        if (h->dev >= 0 && h->dev < g_bt_ndev) bt_forget(&g_bt_dev[h->dev].addr);
                        break;
                }
                draw_all();
                return;
            }
        } break;
        case PANEL_WIFI: {
            for (int i = 0; i < g_wf_nhits; i++) {
                wf_hit_t *h = &g_wf_hits[i];
                if (local_x < h->x || local_x >= h->x + h->w ||
                    local_y < h->y || local_y >= h->y + h->h) continue;
                switch (h->action) {
                    case WFA_POWER:
                        wifi_power(wifi_is_powered() ? 0 : 1);
                        if (!wifi_is_powered()) g_wf_pend_active = 0;
                        break;
                    case WFA_SCAN:
                        if (wifi_scan_active()) wifi_scan_stop(); else wifi_scan_start();
                        break;
                    case WFA_CONNECT:
                        if (h->net >= 0 && h->net < g_wf_nnet) {
                            copy_str(g_wf_target, g_wf_net[h->net].ssid, sizeof(g_wf_target));
                            if (g_wf_net[h->net].security != WIFI_SEC_OPEN) {
                                // Secured: prompt for the password via a modal.
                                modal_mode = MODAL_WIFI_PASSWORD;
                                modal_num_fields = 1;
                                copy_to_modal_field(0, "");
                                modal_active_field = 0;
                                modal_error[0] = '\0';
                            } else {
                                wifi_connect(g_wf_target, "");
                                g_wf_pend_active = 1;
                            }
                        }
                        break;
                    case WFA_DISCONNECT:
                        wifi_disconnect();
                        break;
                    case WFA_FORGET:
                        if (h->net >= 0 && h->net < g_wf_nnet) wifi_forget(g_wf_net[h->net].ssid);
                        break;
                }
                draw_all();
                return;
            }
        } break;
        case PANEL_DEVICES: {
            int cw = CONTENT_WIDTH - 2 * PADDING;
            // "+ Add Printer" header button.
            int addx = x + cw - 140, addy = base_y - 4;
            if (local_x >= addx && local_x < addx + 140 &&
                local_y >= addy && local_y < addy + 24) {
                modal_mode = MODAL_ADD_PRINTER;
                modal_num_fields = 5;
                copy_to_modal_field(0, "");
                copy_to_modal_field(1, "");
                copy_to_modal_field(2, "ipp/print");
                copy_to_modal_field(3, "631");
                copy_to_modal_field(4, (g_printer_count == 0) ? "y" : "n");
                modal_active_field = 0;
                modal_error[0] = '\0';
                draw_all();
                return;
            }
            // Per-row action buttons: Test / Set Default / Remove.
            for (int i = 0; i < g_printer_count; i++) {
                int ry = DEV_ROW_Y0 + i * DEV_ROW_H;
                int by = ry + 12;
                if (local_y < by || local_y >= by + DEV_BTN_H) continue;
                int bx = x + cw - (DEV_BTN_W * 3 + 20);
                prt_cfg_t *p = &g_printers[i];
                if (local_x >= bx && local_x < bx + DEV_BTN_W) {
                    // Test page.
                    int r = prt_job(p->name, "MayteraOS Test Page",
                        "This is a test page from MayteraOS.\n"
                        "If you can read this, network printing works.\n");
                    const char *m = (r == 0) ? "Test page sent." : "Test page failed (check printer/network).";
                    int k = 0; while (m[k] && k < 99) { g_print_status[k] = m[k]; k++; } g_print_status[k] = '\0';
                    draw_all(); return;
                }
                if (local_x >= bx + DEV_BTN_W + 3 && local_x < bx + 2 * DEV_BTN_W + 3) {
                    // Set as default: re-add same config with make_default=1.
                    prt_add(p->name, p->host, p->port, p->queue, 1);
                    printers_refresh();
                    draw_all(); return;
                }
                if (local_x >= bx + 2 * (DEV_BTN_W + 3) && local_x < bx + 3 * DEV_BTN_W + 6) {
                    // Remove.
                    prt_remove(p->name);
                    printers_refresh();
                    const char *m = "Printer removed.";
                    int k = 0; while (m[k] && k < 99) { g_print_status[k] = m[k]; k++; } g_print_status[k] = '\0';
                    draw_all(); return;
                }
            }
        } break;
        case PANEL_NOTIFICATIONS: {
            int en_y = base_y + 40;
            if (local_x >= x + 220 && local_x < x + 268 && local_y >= en_y && local_y < en_y + 24) {
                alerts_enabled = !alerts_enabled; alerts_save(); draw_all(); return; }
            int info_y = base_y + 112;
            if (local_x >= x && local_x < x + 220 && local_y >= info_y && local_y < info_y + 18) {
                alerts_info = !alerts_info; alerts_save(); draw_all(); return; }
            int succ_y = base_y + 138;
            if (local_x >= x && local_x < x + 220 && local_y >= succ_y && local_y < succ_y + 18) {
                alerts_success = !alerts_success; alerts_save(); draw_all(); return; }
            int warn_y = base_y + 164;
            if (local_x >= x && local_x < x + 220 && local_y >= warn_y && local_y < warn_y + 18) {
                alerts_warning = !alerts_warning; alerts_save(); draw_all(); return; }
            int err_y = base_y + 190;
            if (local_x >= x && local_x < x + 220 && local_y >= err_y && local_y < err_y + 18) {
                alerts_error = !alerts_error; alerts_save(); draw_all(); return; }
            int dur_y = base_y + 228;
            if (local_y >= dur_y && local_y < dur_y + 16 && local_x >= x + 140 && local_x < x + 380) {
                int v = ((local_x - (x + 140)) * 20) / 240;
                if (v < 1) v = 1; if (v > 20) v = 20;
                alerts_duration = v; alerts_save(); draw_all(); return; }
            int dnd_y = base_y + 272;
            if (local_x >= x + 220 && local_x < x + 268 && local_y >= dnd_y && local_y < dnd_y + 24) {
                alerts_dnd = !alerts_dnd; alerts_save(); draw_all(); return; }
        } break;
        case PANEL_STARTMENU: {
            int layout_y = base_y + 40;
            if (local_y >= layout_y && local_y < layout_y + 28 &&
                local_x >= x + 120 && local_x < x + 120 + 160) {
                dropdown_open(x + 120, layout_y - 3, 160, SM_VIEW_OPTS, SM_VIEW_OPTS_COUNT, &sm_view, sm_view_changed);
                draw_all(); return;
            }
            int fav_y = base_y + 80;
            if (local_x >= x + 220 && local_x < x + 268 && local_y >= fav_y && local_y < fav_y + 24) {
                sm_show_fav = !sm_show_fav; sm_save(); draw_all(); return; }
            int rec_y = base_y + 120;
            if (local_x >= x + 220 && local_x < x + 268 && local_y >= rec_y && local_y < rec_y + 24) {
                sm_show_recent = !sm_show_recent; sm_save(); draw_all(); return; }
            int rc_y = base_y + 160;
            if (local_y >= rc_y && local_y < rc_y + 16 && local_x >= x + 180 && local_x < x + 340) {
                int v = ((local_x - (x + 180)) * 9) / 160 + 1;
                if (v < 1) v = 1; if (v > 10) v = 10;
                sm_recent_count = v; sm_save(); draw_all(); return; }
            int fs_y = base_y + 200;
            if (local_x >= x + 260 && local_x < x + 308 && local_y >= fs_y && local_y < fs_y + 24) {
                sm_focus_search = !sm_focus_search; sm_save(); draw_all(); return; }
            int w_y = base_y + 240;
            if (local_y >= w_y && local_y < w_y + 16 && local_x >= x + 180 && local_x < x + 340) {
                int v = ((local_x - (x + 180)) * 200) / 160 + 220;
                if (v < 220) v = 220; if (v > 420) v = 420;
                sm_width = v; sm_save(); draw_all(); return; }
            int is_y = base_y + 280;
            if (local_y >= is_y && local_y < is_y + 16 && local_x >= x + 180 && local_x < x + 340) {
                int v = ((local_x - (x + 180)) * 14) / 160 + 14;
                if (v < 14) v = 14; if (v > 28) v = 28;
                sm_icon_size = v; sm_save(); draw_all(); return; }
        } break;
        case PANEL_DOCK: {
            // Every y here comes from dock_panel_layout(), the SAME helper
            // draw_dock_panel() calls - one geometry function, not two
            // arithmetic chains that can drift (see this file's own "hit
            // boxes that drift away from their controls" lesson, Mouse panel).
            dock_layout_t L = dock_panel_layout();

            if (local_y >= L.style_y - 3 && local_y < L.style_y - 3 + 28 &&
                local_x >= x + 120 && local_x < x + 120 + 220) {
                dropdown_open(x + 120, L.style_y - 3, 220, DOCK_OPTS, DOCK_OPTS_COUNT,
                              &dock_style, dock_dd_changed);
                draw_all(); return;
            }

            if (local_y >= L.opacity_y - 4 && local_y < L.opacity_y + 18 &&
                local_x >= x + 120 && local_x < x + 120 + 170) {
                int pct = ((local_x - (x + 120)) * 100) / 170;
                if (pct < DOCK_OPACITY_MIN) pct = DOCK_OPACITY_MIN;
                if (pct > DOCK_OPACITY_MAX) pct = DOCK_OPACITY_MAX;
                pct = ((pct + 2) / 5) * 5;    // snap to 5 on release
                if (pct < DOCK_OPACITY_MIN) pct = DOCK_OPACITY_MIN;
                if (pct > DOCK_OPACITY_MAX) pct = DOCK_OPACITY_MAX;
                dock_opacity = pct;
                dock_opacity_write();
                draw_all();
                return;
            }

            if (local_y >= L.height_y - 4 && local_y < L.height_y + 18 &&
                local_x >= x + 120 && local_x < x + 120 + 170) {
                int span = DOCK_HEIGHT_MAX - DOCK_HEIGHT_MIN;
                int v = ((local_x - (x + 120)) * span) / 170 + DOCK_HEIGHT_MIN;
                if (v < DOCK_HEIGHT_MIN) v = DOCK_HEIGHT_MIN;
                if (v > DOCK_HEIGHT_MAX) v = DOCK_HEIGHT_MAX;
                dock_height = v;
                dock_height_write();
                draw_all();
                return;
            }

            if (local_y >= L.zoom_y - 4 && local_y < L.zoom_y + 18 &&
                local_x >= x + 120 && local_x < x + 120 + 170) {
                int span = DOCK_ZOOM_MAX - DOCK_ZOOM_MIN;
                int v = ((local_x - (x + 120)) * span) / 170 + DOCK_ZOOM_MIN;
                v = ((v + 2) / 5) * 5;    // snap to 5%, same idiom as Opacity
                if (v < DOCK_ZOOM_MIN) v = DOCK_ZOOM_MIN;
                if (v > DOCK_ZOOM_MAX) v = DOCK_ZOOM_MAX;
                dock_zoom = v;
                dock_zoom_write();
                draw_all();
                return;
            }

            // (#123) Remove buttons, addressed through the SAME dock_pin_cell()
            // the draw loop uses. The old chain recomputed `ry` and the button x
            // independently; with a two-column grid that second chain is exactly
            // the hit-box drift this panel's own header comment warns about.
            for (int i = 0; i < g_dockfav_pinned_n && i < L.cols * L.rows; i++) {
                int cx0 = 0, ry = 0;
                dock_pin_cell(&L, i, &cx0, &ry);
                int bx = x + cx0 + L.col_w - 76, by = ry - 1, bw = 70;
                if (local_x >= bx && local_x < bx + bw && local_y >= by && local_y < by + 24) {
                    dockfav_remove(g_dockfav_pinned[i]);
                    draw_all(); return;
                }
            }

            if (!L.full && L.has_candidates &&
                local_y >= L.add_y - 3 && local_y < L.add_y - 3 + 28 &&
                local_x >= x + 120 && local_x < x + 120 + 220) {
                dropdown_open(x + 120, L.add_y - 3, 220, g_dockfav_dd_items, g_dockfav_cand_n + 1,
                              &g_dockfav_add_sel, dockfav_add_dd_changed);
                draw_all(); return;
            }
        } break;
        case PANEL_APPEARANCE: {
            // Theme dropdown (y ~ 65)
            int theme_y = base_y + 65;
            if (local_y >= theme_y && local_y < theme_y + 28 &&
                local_x >= x && local_x < x + 220) {
                dropdown_open(x, theme_y, 220, g_th_names, g_th_count,
                              &current_theme, theme_dd_changed);
                draw_all();
                return;
            }

            // Accent colors (y ~ 140)
            int color_y = theme_y + 75;
            if (local_y >= color_y && local_y < color_y + 32) {
                for (int i = 0; i < NUM_ACCENT_COLORS; i++) {
                    if (local_x >= x + i * 42 && local_x < x + i * 42 + 32) {
                        accent_color_idx = i;
                        apply_theme(current_theme);
                        draw_all();
                        return;
                    }
                }
            }

            // Font size dropdown
            int font_y = color_y + 80;
            if (local_y >= font_y && local_y < font_y + 28 &&
                local_x >= x + 120 && local_x < x + 120 + 160) {
                dropdown_open(x + 120, font_y, 160, FONT_SIZE_OPTS, FONT_SIZE_OPTS_COUNT,
                              &font_size, font_dd_changed);
                draw_all(); return;
            }

            // Icon size dropdown
            int icon_y = font_y + 40;
            if (local_y >= icon_y && local_y < icon_y + 28 &&
                local_x >= x + 120 && local_x < x + 120 + 160) {
                dropdown_open(x + 120, icon_y, 160, ICON_SIZE_OPTS, ICON_SIZE_OPTS_COUNT,
                              &icon_size, icon_dd_changed);
                draw_all(); return;
            }

            // UI Font row (#351): the shared picker. On OK this persists the
            // choice AND flips the kernel's active face, so every app restyles
            // live rather than at the next boot.
            int uifont_y = icon_y + 40;
            if (local_y >= uifont_y - 3 && local_y < uifont_y + 25 &&
                local_x >= x + 120 && local_x < x + 450) {
                g_uifont.title = "System UI Font";
                g_uifont.preview_text = "The quick brown fox jumps over the lazy dog";
                if (gui_font_dialog(&g_uifont))
                    gui_font_set_system(&g_uifont);
                draw_all(); return;
            }

            // Screensaver dropdown + Test
            int ss_y = icon_y + 115;
            if (local_y >= ss_y && local_y < ss_y + 28) {
                if (local_x >= x && local_x < x + 160) {
                    dropdown_open(x, ss_y, 160, SS_OPTS, SS_OPTS_COUNT,
                                  &screensaver_idx, ss_dd_changed);
                    draw_all(); return;
                }
                if (screensaver_idx != 0 && local_x >= x + 200 && local_x < x + 290) {
                    test_screensaver(); return;
                }
            }

            // (#115) Activation-delay slider row (only when a saver is selected)
            int ssdelay_y = ss_y + 34;
            if (screensaver_idx != 0 &&
                local_y >= ssdelay_y - 2 && local_y < ssdelay_y + 16 &&
                local_x >= x + 120 && local_x < x + 290) {
                int di = ((local_x - (x + 120)) * (SS_DELAY_NSTEPS - 1) + 85) / 170;
                ss_set_delay_index(di);
                draw_all(); return;
            }

            // #745 task #67 "dockpanel": Dock Style dropdown and Dock glass
            // opacity slider MOVED to PANEL_DOCK (see its own click-handler
            // case below) along with their draw code. Transparency (the
            // WINDOW opacity row, unrelated to the dock) now sits directly
            // below the screensaver block with no dock row in between.

            // Wallpaper dropdown (right column, mirrors the other
            // setting dropdowns above rather than a per-cell thumbnail grid).
            {
                int wp_dy = WP_DD_Y - g_content_scroll.offset + 25;   // (#227) scroll-aware
                if (local_y >= wp_dy && local_y < wp_dy + 28 &&
                    local_x >= WP_DD_X && local_x < WP_DD_X + WP_DD_W) {
                    wp_names_init();
                    dropdown_open(WP_DD_X, wp_dy, WP_DD_W, g_wp_names, g_wp_count,
                                  &wallpaper_idx, wallpaper_dd_changed);
                    draw_all(); return;
                }
            }

            // Transparency slider (#112): click anywhere on the 200px track to set
            // the global window opacity. Applied live; the compositor persists it.
            // #745 task #67: now directly below the screensaver block (the Dock
            // Style row that used to sit between them is gone from this panel).
            int tr_y = ss_y + 95;
            if (local_y >= tr_y - 4 && local_y < tr_y + 18 &&
                local_x >= x + 120 && local_x < x + 120 + 170) {
                int pct = ((local_x - (x + 120)) * 100) / 170;
                if (pct < 5)   pct = 5;
                if (pct > 100) pct = 100;
                transparency_level = pct;
                set_win_opacity(pct * 255 / 100);
                draw_all();
                return;
            }
            break;
        }

        case PANEL_DISPLAY: {
            // Rotation dropdown (#745 local 102). y matches draw_display_
            // panel()'s cumulative offset at the "Rotation" subsection: 40
            // (header) + 25 (Resolution subsection) + 30 (res/depth row) + 16
            // + 24 (two hint lines) + 25 (Rotation subsection) - 3 (the
            // draw_dropdown_n y-3 convention every other dropdown here uses)
            // = 157.
            // EVERY offset here now comes from display_layout(), the same
            // function draw_display_panel() draws from. The hand-added
            // constants that used to live here had drifted 50px out of step
            // with the drawing (see the comment on display_layout()).
            display_layout_t L = display_layout();
            int rot_y = base_y + L.rotation_dd;
            if (local_y >= rot_y && local_y < rot_y + 28 &&
                local_x >= x + 120 && local_x < x + 120 + 160) {
                dropdown_open(x + 120, rot_y, 160, ROTATION_OPTS, ROTATION_OPTS_COUNT,
                              &rotation_idx, rotation_dd_changed);
                draw_all(); return;
            }

            // UI scale dropdown.
            int uis_y = base_y + L.uiscale_dd;
            if (local_y >= uis_y && local_y < uis_y + 28 &&
                local_x >= x + 120 && local_x < x + 120 + 160) {
                uiscale_opts_build();
                dropdown_open(x + 120, uis_y, 160, g_uiscale_opts, g_uiscale_count,
                              &g_uiscale_idx, uiscale_dd_changed);
                draw_all(); return;
            }

            // Brightness slider. #745 (local 102): recomputed from
            // draw_display_panel()'s actual cumulative y (40+25+30+16+24+25
            // (new Rotation subsection)+34+30+50+25 = 299) - the OLD "+130"
            // predates this ticket and was already stale before the Rotation
            // section was added (traced against draw_display_panel() as it
            // stood pre-#102: true position was +210, not +130 - Scale/hint
            // content had grown past it and nobody had re-measured). night
            // light toggle/strength below are computed RELATIVE to bright_y
            // (+70, +35) and those relative offsets were already correct, so
            // fixing this one constant fixes the whole PANEL_DISPLAY chain.
            int bright_y = base_y + L.brightness;
            if (local_y >= bright_y && local_y < bright_y + 16 &&
                local_x >= x && local_x < x + 350) {
                brightness = ((local_x - x) * 100) / 350;
                if (brightness < 0) brightness = 0;
                if (brightness > 100) brightness = 100;
                apply_display_fx(); draw_all();
                return;
            }

            // Night light toggle
            int nl_y = base_y + L.nightlight;
            if (local_y >= nl_y && local_y < nl_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                night_light = !night_light;
                apply_display_fx(); draw_all();
                return;
            }

            // Night light strength slider (only when enabled)
            if (night_light) {
                int nls_y = base_y + L.nl_strength;
                if (local_y >= nls_y && local_y < nls_y + 16 &&
                    local_x >= x + 120 && local_x < x + 320) {
                    night_light_strength = ((local_x - x - 120) * 100) / 200;
                    if (night_light_strength < 0) night_light_strength = 0;
                    if (night_light_strength > 100) night_light_strength = 100;
                    apply_display_fx(); draw_all(); return;
                }
            }
            break;
        }

        case PANEL_SOUND: {
            // Layout mirrors draw_sound_panel (base_y = PADDING = 20).
            // Mute button (output device row @ y=85, button at y+2)
            int dev_y = base_y + 65;                 // 85
            if (local_y >= dev_y + 2 && local_y < dev_y + 26 &&
                local_x >= x + 380 && local_x < x + 450) {
                sound_muted = !sound_muted;
                set_mute(sound_muted ? 1 : 0);
                draw_all();
                return;
            }

            // Master volume slider @ y=125, width 260 (REAL kernel mixer).
            int vol_y = base_y + 105;                // 125
            if (local_y >= vol_y && local_y < vol_y + 16 &&
                local_x >= x + 100 && local_x < x + 360) {
                master_volume = ((local_x - x - 100) * 100) / 260;
                if (master_volume < 0) master_volume = 0;
                if (master_volume > 100) master_volume = 100;
                set_volume(master_volume);
                draw_all();
                return;
            }

            // System Sound Effects toggle @ y=330
            int ss_y = base_y + 310;                 // 330
            if (local_y >= ss_y && local_y < ss_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                sound_effects = !sound_effects;
                draw_all();
                return;
            }

            // Test Speakers button @ y=370 (real WAV, only if a device exists).
            int test_spk_y = base_y + 350;           // 370
            if (local_y >= test_spk_y && local_y < test_spk_y + 30 &&
                local_x >= x && local_x < x + 130) {
                if (g_audio_present) {
                    sys_play_wav("/SOUNDS/SHORTINT.MP3");
                    sound_test_status = 3;
                }
                draw_all();
                return;
            }
            break;
        }

        case PANEL_NETWORK: {
            // (#227/#223) network_geom() WALKS the same conditional
            // draw_network_panel() walks (whether "Configure IP..." is drawn
            // between DHCP and Firewall), rather than re-deriving a fixed
            // offset by hand. This replaces the previous instance fix - a
            // ternary that mirrored the 36px cost of that button but could
            // not survive a future row being inserted between DHCP and
            // Firewall without silently going stale again the same way.
            network_geom_t g; network_geom(&g);

            // DHCP toggle
            if (local_y >= g.dhcp && local_y < g.dhcp + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                dhcp_enabled = !dhcp_enabled;
                if (dhcp_enabled) {
                    net_dhcp();        // acquire a lease
                    net_pull_live();   // #786
                }
                draw_all();
                return;
            }

            // (#786) "Set DNS..." - present only when DHCP is ON, in the same
            // row "Configure IP..." occupies when it is off.
            if (g.dnsbtn >= 0 &&
                local_y >= g.dnsbtn && local_y < g.dnsbtn + 28 &&
                local_x >= x + 20 && local_x < x + 180) {
                modal_mode = MODAL_SET_DNS;
                modal_num_fields = 1;
                copy_to_modal_field(0, dns_primary);
                modal_active_field = 0;
                modal_error[0] = '\0';
                draw_all();
                return;
            }

            // Configure IP button (only present when DHCP is off; g.cfgip is
            // -1 otherwise, matching mouse_geom()'s traillen convention)
            if (g.cfgip >= 0 &&
                local_y >= g.cfgip && local_y < g.cfgip + 28 &&
                local_x >= x + 20 && local_x < x + 180) {
                modal_mode = MODAL_SET_NETWORK;
                modal_num_fields = 4;
                copy_to_modal_field(0, ip_address);
                copy_to_modal_field(1, subnet_mask);
                copy_to_modal_field(2, gateway);
                copy_to_modal_field(3, dns_primary);
                modal_active_field = 0;
                modal_error[0] = '\0';
                draw_all();
                return;
            }

            // VPN is now an honest "not available" note (no toggle).

            // Firewall (#238). Each of the three now calls fw_push(), which
            // installs the policy in the kernel, persists it, and re-reads
            // what actually took effect before the panel is redrawn. A click
            // that the kernel refuses therefore leaves the toggle showing the
            // policy that is running, not the one that was asked for.
            if (local_y >= g.fw && local_y < g.fw + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                firewall_enabled = !firewall_enabled;
                fw_push();
                draw_all();
                return;
            }
            if (local_y >= g.pol_in && local_y < g.pol_in + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                fw_pol_in = (fw_pol_in == FW_ACT_DENY) ? FW_ACT_ALLOW : FW_ACT_DENY;
                fw_push();
                draw_all();
                return;
            }
            if (local_y >= g.pol_out && local_y < g.pol_out + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                fw_pol_out = (fw_pol_out == FW_ACT_DENY) ? FW_ACT_ALLOW : FW_ACT_DENY;
                fw_push();
                draw_all();
                return;
            }

            // The per-rule editor is still not drawn here (see
            // draw_network_panel): rules are edited through the contract API,
            // and the panel says so. A hit-test for a control that is not
            // drawn is exactly what #223 removed and it is not coming back.
            break;
        }


        case PANEL_KEYBOARD:
            // #230: no interactive rows left in this panel - see the comment
            // above draw_keyboard_panel(). Nothing to hit-test.
            break;

        case PANEL_MOUSE: {
            // #745: every hit box below comes from the SAME mouse_geom() the
            // draw pass uses, so a control and its hit box cannot drift apart.
            mouse_geom_t m; mouse_geom(&m);

            // Pointer speed slider
            if (local_y >= m.sens && local_y < m.sens + 16 &&
                local_x >= x + 140 && local_x < x + 390) {
                pointer_speed = ((local_x - x - 140) * 100) / 250;
                if (pointer_speed < 0) pointer_speed = 0;
                if (pointer_speed > 100) pointer_speed = 100;
                set_mouse_speed(1 + (pointer_speed * 9) / 100);
                draw_all();
                return;
            }

            // Double-click speed slider. #230: now a REAL setting - the
            // compositor's double-click detector reads this back via
            // settingscfg_dblclick_ms() (libc/settingscfg.c) instead of a
            // hardcoded 500ms constant, once settings_autosave() (called
            // every main-loop iteration, same as every other slider here)
            // notices the value changed and writes it. No explicit save call
            // here - adding one would double-write on every drag frame,
            // since settings_autosave() has its own independent change-hash
            // and would not know a direct settings_save() call already ran.
            if (local_y >= m.dbl && local_y < m.dbl + 16 &&
                local_x >= x + 160 && local_x < x + 390) {
                double_click_speed = ((local_x - x - 160) * 100) / 230;
                if (double_click_speed < 0) double_click_speed = 0;
                if (double_click_speed > 100) double_click_speed = 100;
                draw_all();
                return;
            }

            // Cursor style dropdown (#745: moved here from Appearance).
            // cursor_dd_changed() is the ONLY writer of the live cursor.
            if (local_y >= m.cursor && local_y < m.cursor + 28 &&
                local_x >= x + 140 && local_x < x + 300) {
                dropdown_open(x + 140, m.cursor, 160, CURSOR_OPTS, CURSOR_OPTS_COUNT,
                              &cursor_theme, cursor_dd_changed);
                draw_all(); return;
            }

            // #230: Pointer Size, Pointer Trails/Trail Length, Scroll Speed,
            // Natural Scrolling, Scroll Inertia and Left-handed Mode removed -
            // see the comment above mouse_geom(). Nothing to hit-test.
            break;
        }

        case PANEL_DATETIME: {
            // (#227) every hit box below comes from the SAME datetime_geom()
            // the draw pass uses, so a control and its hit box cannot drift
            // apart again. This replaces an independent `base_y + <literal>`
            // chain that had drifted 50px starting at Date Format.
            datetime_geom_t g; datetime_geom(&g);

            // Auto time toggle (with NTP sync on enable)
            if (local_y >= g.auto_time && local_y < g.auto_time + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                auto_time = !auto_time;
                if (auto_time) {
                    long r = ntp_sync();
                    ntp_status = (r == 0) ? 1 : -1;
                }
                draw_all();
                return;
            }

            // 24-hour toggle. #230: now REAL - settingscfg_use24h() (libc/
            // settingscfg.c) drives the taskbar clock too, once
            // settings_autosave() (every main-loop iteration) notices the
            // change and writes it - no explicit save call needed here, see
            // the same note on the double-click slider above.
            if (local_y >= g.use24 && local_y < g.use24 + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                use_24hour = !use_24hour;
                draw_all();
                return;
            }

            // Timezone dropdown: open a scrollable list (current item highlighted)
            if (local_y >= g.tz && local_y < g.tz + 28 &&
                local_x >= x && local_x < x + 350) {
                dropdown_open(x, g.tz, 350, tz_labels(), tz_count(),
                              &timezone_idx, update_timezone_offset);
                draw_all(); return;
            }

            // #230: Date Format dropdown deleted - see the comment above
            // datetime_geom().

            // Week start buttons
            if (local_y >= g.weekstart && local_y < g.weekstart + 28) {
                for (int i = 0; i < 2; i++) {
                    if (local_x >= x + 140 + i * 90 && local_x < x + 140 + i * 90 + 82) {
                        first_day_of_week = i;
                        draw_all();
                        return;
                    }
                }
            }

            // Set Date & Time button (only when auto_time is off)
            if (!auto_time) {
                if (local_y >= g.setbtn && local_y < g.setbtn + 30 &&
                    local_x >= x && local_x < x + 160) {
                    modal_mode = MODAL_SET_DATETIME;
                    modal_num_fields = 2;
                    // #49: seed with LOCAL time. The field is what the user
                    // reads off their wall, so it must match the taskbar; the
                    // commit path below converts it back to UTC for the RTC.
                    tz_time_t nowl; tz_local_now(&nowl);
                    format_hms(modal_field[0], nowl.hour, nowl.min, nowl.sec);
                    format_ymd(modal_field[1], nowl.year, nowl.month, nowl.day);
                    modal_cursor[0] = 8;
                    modal_cursor[1] = 10;
                    modal_cursor[2] = 0;
                    modal_caret[0] = 8; modal_caret[1] = 10; modal_caret[2] = 0;
                    modal_active_field = 0;
                    modal_error[0] = '\0';
                    draw_all(); return;
                }
            }
            break;
        }

        case PANEL_USERS: {
            // (#227) every hit box below comes from the SAME users_geom()
            // the draw pass uses, so a control and its hit box cannot drift
            // apart again (previous drift: a uniform 5px on Add User/Guest,
            // both of which depend on user_count).
            users_geom_t g; users_geom(&g);

            // Edit Profile button (inside current user card)
            if (local_y >= g.edit_profile && local_y < g.edit_profile + 30 &&
                local_x >= x + 350 && local_x < x + 450) {
                modal_mode = MODAL_EDIT_PROFILE;
                modal_num_fields = 2;
                copy_to_modal_field(0, users[current_user_idx].fullname);
                copy_to_modal_field(1, users[current_user_idx].email);
                modal_avatar_idx = avatar_palette_index_for(
                    users[current_user_idx].avatar_color, users[current_user_idx].uid);
                modal_active_field = 0;
                modal_error[0] = '\0';
                draw_all(); return;
            }

            // Auto-login toggle (#566: real, targets the CURRENT user's own
            // account - always allowed, but a non-root session still needs
            // that account's password, collected via MODAL_AUTOLOGIN_PW).
            if (local_y >= g.autologin && local_y < g.autologin + 24 &&
                local_x >= x + 340 && local_x < x + 388) {
                int is_al_cur = (autologin_user[0] &&
                                 strcmp(autologin_user, users[current_user_idx].username) == 0);
                autologin_request(users[current_user_idx].username, !is_al_cur);
                return;
            }

            // Change Password button
            if (local_y >= g.chpw && local_y < g.chpw + 30 &&
                local_x >= x && local_x < x + 160) {
                modal_mode = MODAL_CHANGE_PASSWORD;
                modal_field[0][0] = modal_field[1][0] = modal_field[2][0] = '\0';
                modal_cursor[0] = modal_cursor[1] = modal_cursor[2] = 0;
                modal_caret[0] = modal_caret[1] = modal_caret[2] = 0;
                modal_active_field = 0;
                modal_error[0] = '\0';
                draw_all();
                return;
            }

            // Per-user "Remove" buttons (+ root-only Auto-login toggle) in the
            // Other Users list. row_y comes from g.other_row0, the SAME
            // starting point draw_users_panel()'s loop uses.
            {
                int row_y = g.other_row0;
                for (int i = 0; i < user_count; i++) {
                    if (i == current_user_idx) continue;
                    if (users[i].username[0] == 0) continue;
                    // (#566) Root-only: set/clear autologin for this OTHER
                    // account (kernel ABI - "root sets for anyone", no
                    // password needed since Settings cannot know it).
                    if (settings_is_root() &&
                        local_x >= x + 290 && local_x < x + 390 &&
                        local_y >= row_y + 13 && local_y < row_y + 37) {
                        int is_al = (autologin_user[0] && strcmp(autologin_user, users[i].username) == 0);
                        autologin_request(users[i].username, !is_al);
                        return;
                    }
                    if (local_x >= x + 400 && local_x < x + 480 &&
                        local_y >= row_y + 13 && local_y < row_y + 37) {
                        if (users[i].role == 0) {
                            const char *m = "Cannot remove the administrator.";
                            int k = 0; while (m[k] && k < 63) { modal_error[k] = m[k]; k++; }
                            modal_error[k] = '\0';
                        } else {
                            delete_user(users[i].username);
                            users_refresh();
                        }
                        draw_all(); return;
                    }
                    row_y += 60;
                }
            }

            // Add User button (after other-users list)
            if (local_y >= g.adduser && local_y < g.adduser + 30 &&
                local_x >= x && local_x < x + 120) {
                modal_mode = MODAL_ADD_USER;
                modal_num_fields = 3;
                modal_field[0][0] = modal_field[1][0] = modal_field[2][0] = '\0';
                modal_cursor[0] = modal_cursor[1] = modal_cursor[2] = 0;
                modal_caret[0] = modal_caret[1] = modal_caret[2] = 0;
                modal_active_field = 0;
                modal_error[0] = '\0';
                draw_all(); return;
            }

            // Guest enabled toggle (position varies based on user count)
            if (local_y >= g.guest && local_y < g.guest + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                guest_enabled = !guest_enabled;
                draw_all();
                return;
            }
            break;
        }

        case PANEL_PRIVACY: {
            // (#227) every hit box below comes from the SAME privacy_geom()
            // the draw pass uses. This also fixes "Require password on wake",
            // which was 20px off ON THE X AXIS: the toggle actually draws at
            // x+300..x+348 (draw_toggle_labeled(x+20, y, 280, ...) => switch
            // at x+20+280 = x+300), not the x+280..x+328 the old test used.
            // Every change persists to /CONFIG/PRIVACY.CFG (real stored setting).
            privacy_geom_t g; privacy_geom(&g);

            // Screen lock toggle
            if (local_y >= g.lock && local_y < g.lock + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                screen_lock_enabled = !screen_lock_enabled;
                privacy_save();
                draw_all();
                return;
            }

            // Location services toggle
            if (local_y >= g.location && local_y < g.location + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                location_services = !location_services;
                privacy_save();
                draw_all();
                return;
            }

            // Diagnostics toggle
            if (local_y >= g.diagnostics && local_y < g.diagnostics + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                diagnostics_enabled = !diagnostics_enabled;
                privacy_save();
                draw_all();
                return;
            }

            // Crash reports toggle
            if (local_y >= g.crash && local_y < g.crash + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                crash_reports = !crash_reports;
                privacy_save();
                draw_all();
                return;
            }

            // Screen lock timeout dropdown (cycle values when clicked; only
            // present when screen_lock_enabled - g.timeout_dd is -1 otherwise)
            if (g.timeout_dd >= 0 &&
                local_y >= g.timeout_dd && local_y < g.timeout_dd + 28 &&
                local_x >= x + 120 && local_x < x + 240) {
                int vals[] = {0, 1, 2, 5, 10, 15, 30};
                int ci = 0;
                for (int ii = 0; ii < 7; ii++) {
                    if (vals[ii] == lock_timeout) { ci = ii; break; }
                }
                lock_timeout = vals[(ci + 1) % 7];
                privacy_save();
                draw_all(); return;
            }
            // Require password on wake toggle (only present when
            // screen_lock_enabled; g.reqpw is -1 otherwise). Real switch is
            // x+300..x+348 (see the case comment above for the x+20 indent
            // this used to miss).
            if (g.reqpw >= 0 &&
                local_y >= g.reqpw && local_y < g.reqpw + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                require_password_wake = !require_password_wake;
                privacy_save();
                draw_all(); return;
            }
            break;
        }

        case PANEL_STORAGE: {
            // (#227) every hit box below comes from the SAME storage_geom()
            // the draw pass uses. The old `cache_base = base_y + 40 +
            // drive_count*80 + 40` re-derivation landed 17-20px below the
            // buttons' actual drawn position (they draw at `label_y - 3`),
            // leaving the three "Clear" buttons live in only a ~7px band and
            // "Empty Trash" fully unreachable.
            storage_geom_t g; storage_geom(&g);

            // Thumbnails clear (unlinks real files, then rescans)
            if (local_y >= g.thumb_clear && local_y < g.thumb_clear + 24 &&
                local_x >= x + 280 && local_x < x + 340) {
                dir_clear_files(CACHE_DIR_THUMBS); storage_scan(); draw_all(); return;
            }
            // App cache clear
            if (local_y >= g.app_clear && local_y < g.app_clear + 24 &&
                local_x >= x + 280 && local_x < x + 340) {
                dir_clear_files(CACHE_DIR_APPS); storage_scan(); draw_all(); return;
            }
            // System cache clear
            if (local_y >= g.sys_clear && local_y < g.sys_clear + 24 &&
                local_x >= x + 280 && local_x < x + 340) {
                dir_clear_files(CACHE_DIR_SYSTEM); storage_scan(); draw_all(); return;
            }
            // Empty Trash
            if (local_y >= g.empty_trash && local_y < g.empty_trash + 24 &&
                local_x >= x + 280 && local_x < x + 380) {
                dir_clear_files(CACHE_DIR_TRASH); storage_scan(); draw_all(); return;
            }
            break;
        }

        case PANEL_DEFAULTS: {
            // Rows start after header(+36) + hint(+28); each row is 40px tall.
            int first = base_y + 64;
            int n; const assoc_category_t *cats = assoc_categories(&n);
            for (int i = 0; i < n; i++) {
                int ry = first + i * 40;
                if (cats[i].napps > 1 &&
                    local_y >= ry && local_y < ry + 28 &&
                    local_x >= x + 345 && local_x < x + 345 + 90) {
                    char cur[80]; assoc_category_current(i, cur, sizeof(cur));
                    int idx = defaults_app_index(i, cur);
                    int nx  = (idx + 1) % cats[i].napps;
                    defaults_set_category(i, cats[i].apps[nx]);
                    defaults_invalidate_cache();   // (#262) refresh cached labels
                    draw_all();
                    return;
                }
            }
            break;
        }

        case PANEL_ABOUT: {
            // Button row: Check Updates, Export Debug, Credits. The y comes
            // from the draw (g_about_btn_y), never from a re-derived section
            // total; see that variable's declaration for the 126px drift this
            // replaces. draw_button() uses a fixed height of 30.
            int about_btn_y = g_about_btn_y;
            if (about_btn_y > 0 &&
                local_y >= about_btn_y && local_y < about_btn_y + 30) {
                // Check Updates. #230: real network check (check_for_updates()),
                // not a fabricated "up to date". Draw the transitional state
                // first so the click has visible feedback before the blocking
                // HTTP call (which can take a few seconds on a slow/absent
                // network) returns.
                if (local_x >= x && local_x < x + 130) {
                    about_status = 5;
                    copy_str(g_update_msg, "Checking for updates...", sizeof(g_update_msg));
                    draw_all();
                    check_for_updates();
                    draw_all();
                    return;
                }
                // Export Debug
                if (local_x >= x + 145 && local_x < x + 275) {
                    do_export_debug();
                    about_status = 2; draw_all(); return;
                }
                // Credits
                if (local_x >= x + 290 && local_x < x + 390) {
                    modal_mode = MODAL_CREDITS;
                    modal_num_fields = 0;
                    draw_all(); return;
                }
            }
            break;
        }

        default:
            break;
    }
}

// =============================================================================
// /SETVALS.TXT self-report (full-review verification)
// =============================================================================
// Writes every Settings field value that is queried from a live source to
// /SETVALS.TXT, grouped by tab, so displayed-vs-real can be cross-checked over
// SSH on real hardware in one read. Every value here is the SAME datum the panel
// renders. Called once at startup after all probes (below), and re-callable.
static char g_sv_buf[3200];
static int  g_sv_len;
static void sv_raw(const char *s) {
    for (int i = 0; s[i] && g_sv_len < (int)sizeof(g_sv_buf) - 2; i++)
        g_sv_buf[g_sv_len++] = s[i];
}
static void sv_int(long n) {
    char t[24]; int ti = 0; int neg = (n < 0); unsigned long u = neg ? (unsigned long)(-n) : (unsigned long)n;
    if (u == 0) t[ti++] = '0'; else while (u && ti < 23) { t[ti++] = (char)('0' + (int)(u % 10)); u /= 10; }
    if (neg && g_sv_len < (int)sizeof(g_sv_buf) - 2) g_sv_buf[g_sv_len++] = '-';
    while (ti) { if (g_sv_len < (int)sizeof(g_sv_buf) - 2) g_sv_buf[g_sv_len++] = t[--ti]; else ti--; }
}
// One "key=value\n" line with a string value.
static void sv_kvs(const char *k, const char *v) { sv_raw(k); sv_raw("="); sv_raw(v && v[0] ? v : "(empty)"); sv_raw("\n"); }
// One "key=value\n" line with an integer value.
static void sv_kvi(const char *k, long v) { sv_raw(k); sv_raw("="); sv_int(v); sv_raw("\n"); }
// One "key=<bytes formatted>\n" line.
static void sv_kvb(const char *k, uint64_t bytes) {
    char b[24]; format_size(bytes, b, sizeof(b)); sv_raw(k); sv_raw("="); sv_raw(b);
    sv_raw(" ("); sv_int((long)bytes); sv_raw(" B)\n");
}
static void write_setvals(void) {
    g_sv_len = 0;
    hwinfo_load();

    sv_raw("# MayteraOS Settings self-report (/SETVALS.TXT)\n");
    sv_raw("# Every value below is queried live; it matches what the panel shows.\n\n");

    // ---- System / About ----
    sv_raw("[About]\n");
    { char vb[64]; if (get_version(vb, sizeof(vb)) <= 0) copy_str(vb, "?", sizeof(vb)); sv_kvs("version", vb); }
    if (g_sysinfo_ok) {
        const char *b = g_sysinfo.cpu_brand; while (*b == ' ') b++;
        sv_kvs("cpu_brand", b[0] ? b : g_sysinfo.cpu_vendor);
        sv_kvs("cpu_vendor", g_sysinfo.cpu_vendor);
        sv_kvi("cpu_cores", (long)g_sysinfo.cpu_count);
        sv_kvb("mem_total", g_sysinfo.mem_total);
    } else sv_kvs("sysinfo", "UNAVAILABLE");
    sv_kvs("graphics", g_gpu_name);
    sv_kvs("network_adapter", g_nic_name);
    sv_kvs("audio_device", g_audio_present ? g_audio_name : "NONE");
    sv_kvs("bluetooth_adapter", g_bt_present ? "PRESENT" : "NONE");
    sv_kvs("wifi_adapter", g_wifi_present ? "PRESENT" : "NONE");

    // ---- Display ----
    sv_raw("\n[Display]\n");
    { fb_info_t fi; if (fb_info(&fi) == 0) {
        sv_kvi("fb_width", (long)fi.width); sv_kvi("fb_height", (long)fi.height);
        sv_kvi("fb_bpp", (long)fi.bpp);     sv_kvi("fb_pitch", (long)fi.pitch);
    } else sv_kvs("fb_info", "UNAVAILABLE"); }
    sv_kvi("brightness_pct", brightness);
    sv_kvi("night_light", night_light);
    sv_kvi("night_light_strength", night_light_strength);
    // #230: scaling_factor deleted along with the Scale slider (no click
    // handler had ever existed for it - see draw_display_panel()).

    // ---- Sound ----
    sv_raw("\n[Sound]\n");
    sv_kvs("output_device", g_audio_present ? g_audio_name : "NONE (no audio hardware)");
    sv_kvi("master_volume", get_volume());
    sv_kvi("muted", sound_muted);
    sv_kvs("input_capture", "NOT_SUPPORTED");
    sv_kvs("equalizer", "NOT_AVAILABLE");
    sv_kvi("sound_effects", sound_effects);

    // ---- Network ----
    sv_raw("\n[Network]\n");
    sv_kvi("ethernet_connected", ethernet_connected);
    sv_kvs("ip_address", ip_address);
    sv_kvs("subnet_mask", subnet_mask);
    sv_kvs("gateway", gateway);
    sv_kvs("dns_primary", dns_primary);
    sv_kvs("mac_address", mac_address);
    sv_kvi("dhcp_enabled", dhcp_enabled);
    sv_kvi("firewall_enabled", firewall_enabled);
    // #238: the self-report now carries what the KERNEL says, so a reader of
    // /SETVALS.TXT can see displayed-vs-enforced rather than displayed-only.
    sv_kvi("fw_policy_in_deny", fw_pol_in == FW_ACT_DENY);
    sv_kvi("fw_policy_out_deny", fw_pol_out == FW_ACT_DENY);
    sv_kvi("fw_rule_count", fw_rule_count);
    sv_kvi("fw_dropped_in", (int)fw_stats.drop_in);
    sv_kvi("fw_dropped_out", (int)fw_stats.drop_out);
    sv_kvi("fw_last_rc", fw_last_rc);

    // ---- Storage ----
    sv_raw("\n[Storage]\n");
    if (drive_count > 0) {
        sv_kvs("drive0_model", drives[0].model[0] ? drives[0].model : "(none)");
        sv_kvs("drive0_serial", drives[0].serial[0] ? drives[0].serial : "(none)");
        sv_kvb("drive0_total", drives[0].total_bytes);
        sv_kvb("drive0_used", drives[0].used_bytes);
        sv_kvi("drive0_smart", drives[0].smart);
    }
    sv_kvb("cache_thumbnails", cache_thumbnails);
    sv_kvb("cache_apps", cache_apps);
    sv_kvb("cache_system", cache_system);
    sv_kvb("trash", trash_size);

    // ---- Date & Time ----
    sv_raw("\n[DateTime]\n");
    { int h=0,m=0,s=0,d=1,mo=1,yr=2026; get_rtc_time(&h,&m,&s); get_rtc_date(&d,&mo,&yr);
      char tb[12]; format_hms(tb,h,m,s); sv_kvs("rtc_time_utc", tb);
      char db[12]; format_ymd(db,yr,mo,d); sv_kvs("rtc_date", db); }
    // #49: the ID string is what is actually on disk in TZ.CFG; publish that
    // (not the pretty label) so a support dump can be compared to the file.
    sv_kvs("timezone", tz_id(tz_index()));
    sv_kvs("timezone_label", tz_label(tz_index()));
    sv_kvi("tz_offset_min", tz_offset_minutes());
    sv_kvi("tz_is_set", tz_is_set() ? 1 : 0);
    { tz_time_t lt2; tz_local_now(&lt2);
      char lb[12]; format_hms(lb, lt2.hour, lt2.min, lt2.sec); sv_kvs("local_time", lb);
      char ld[12]; format_ymd(ld, lt2.year, lt2.month, lt2.day); sv_kvs("local_date", ld); }

    // ---- Appearance ----
    sv_raw("\n[Appearance]\n");
    sv_kvs("theme", (current_theme >= 0 && current_theme < g_th_count) ? g_th[current_theme].slug : "");
    sv_kvi("accent_idx", accent_color_idx);
    sv_kvs("dock_style", DOCK_OPTS[DOCK_CLAMP(dock_style)]);
    // #745: publish both the picker's index and the kernel's live value so
    // "did opening Settings change the cursor?" can be answered from a file
    // over serial/SSH, with no mouse and no screenshot (#334).
    sv_kvs("cursor_style", CURSOR_OPTS[OPT_CLAMP(cursor_theme, CURSOR_OPTS)]);
    { int _pk = get_cursor(); sv_kvi("cursor_style_kernel", (_pk >= 0) ? (_pk & 0xFF) : -1); }
    sv_kvi("wallpaper_idx", wallpaper_idx);
    sv_kvi("transparency_pct", transparency_level);
    sv_kvi("dock_opacity_pct", dock_opacity);   // #745
    sv_kvi("dock_height_px",   dock_height);    // #123
    sv_kvi("dock_zoom_pct",    dock_zoom);      // #123

    // ---- Users ----
    sv_raw("\n[Users]\n");
    sv_kvi("user_count", user_count);
    if (user_count > 0) {
        sv_kvs("current_user", users[current_user_idx].username);
        static const char* roles[] = {"Administrator","Standard User","Guest"};
        sv_kvs("current_role", roles[users[current_user_idx].role]);
        sv_kvs("current_email", users[current_user_idx].email[0] ? users[current_user_idx].email : "(unset)");
    }

    // ---- Privacy (real persisted settings; no enforcement backend) ----
    sv_raw("\n[Privacy]\n");
    sv_kvi("screen_lock", screen_lock_enabled);
    sv_kvi("lock_timeout_min", lock_timeout);
    sv_kvi("require_pw_wake", require_password_wake);
    sv_kvi("location_services", location_services);
    sv_kvi("diagnostics", diagnostics_enabled);
    sv_kvi("crash_reports", crash_reports);
    sv_kvs("app_permission_matrix", "NOT_IMPLEMENTED");

    // ---- Devices ----
    sv_raw("\n[Devices]\n");
    sv_kvi("printer_count", g_printer_count);
    for (int i = 0; i < g_printer_count && i < 4; i++) {
        sv_raw("printer"); sv_int(i); sv_raw("="); sv_raw(g_printers[i].name);
        sv_raw(" @ "); sv_raw(g_printers[i].host); sv_raw("\n");
    }

    // #743: diagnostic dump. Result consumed, and reported through the
    // breadcrumb log (unlike setlog() itself, this one cannot recurse).
    if (userconf_write_all("/SETVALS.TXT", g_sv_buf, (unsigned long)g_sv_len) != 0)
        save_failed("SETVALS.TXT (diagnostic dump)");
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char **argv) {
    // #233: answer a contract invocation BEFORE any window exists, so the API
    // is usable from a headless harness and from the AI tool loop with no
    // compositor. th_init() runs first because appearance.theme resolves
    // against the enumerated theme list; wp_init() is not needed for any
    // contract row and is skipped on this path.
    if (contract_is_invocation(argc, argv)) {
        th_init();
        return contract_cli(argc, argv, &SETTINGS_CONTRACT);
    }
    (void)argc;
    (void)argv;

    wp_init();   // #517: enumerate wallpapers from the image (shared with compositor)
    th_init();   // #565: enumerate themes from /THEMES/INDEX.TXT

    // #745 (local 102): what rotation THIS session actually booted with.
    // rotation_idx starts equal to it; the dropdown only moves rotation_idx
    // away from rotation_active, which is what drives the "applies after
    // restart" hint in draw_display_panel().
    rotation_active = get_display_rotation();
    if (rotation_active < 0 || rotation_active > 3) rotation_active = 0;
    rotation_idx = rotation_active;

    // #3: startup breadcrumbs. If the next iMac boot fails to show the Settings
    // window, /SETLOG.TXT will end at the last step reached.
    setlog("SET: start");
    {
        fb_info_t fi;
        if (fb_info(&fi) == 0) { setlog_n("SET: fb w", (long)fi.width);
                                 setlog_n("SET: fb h", (long)fi.height); }
        else                     setlog("SET: fb_info FAILED");
    }
    // Default size: big enough for the WHOLE sidebar, shrunk to fit the actual
    // framebuffer. The old default (850x660) was hardcoded and too short: 17
    // panels need 646px of list, so the last two rows were simply unreachable.
    // The framebuffer is whatever the firmware set (1280x800 on the OVMF VMs,
    // 1920x1080 on the iMac14,4), so it MUST be queried, never assumed.
    //
    // win_create() takes the OUTER window size and the kernel subtracts the
    // chrome to get the drawable canvas, so the chrome is added on here; the
    // canvas is then re-read from win_get_size() below rather than inferred.
    {
        int total_w = SET_WANT_W + SET_CHROME_W;
        int total_h = SIDEBAR_NATURAL_H + SET_CHROME_H;
        fb_info_t fi;
        if (fb_info(&fi) == 0 && fi.width > 0 && fi.height > 0) {
            int max_tw = (int)fi.width  - 2 * SET_MARGIN;
            int max_th = (int)fi.height - SET_TASKBAR_H - 2 * SET_MARGIN;
            if (total_w > max_tw) total_w = max_tw;
            if (total_h > max_th) total_h = max_th;   // shorter screen: sidebar scrolls
            win_x = ((int)fi.width - total_w) / 2;
            win_y = ((int)fi.height - SET_TASKBAR_H - total_h) / 2;
        }
        if (win_x < SET_MARGIN) win_x = SET_MARGIN;
        if (win_y < SET_MARGIN) win_y = SET_MARGIN;
        g_win_w = total_w - SET_CHROME_W;
        g_win_h = total_h - SET_CHROME_H;
        setlog_n("SET: window backing bytes", (long)g_win_w * (long)g_win_h * 4);

        setlog("SET: window create begin");
        window_handle = win_create("Settings", win_x, win_y, total_w, total_h);
    gui_font_sel_default(&g_uifont);   // #351: reflect the live system UI font
    }
    if (window_handle < 0) { setlog("SET: window create FAILED (NULL handle)"); return 1; }
    setlog_n("SET: window created handle", window_handle);

    // Authoritative canvas size. win_create()'s arguments are the OUTER size, so
    // deriving the canvas from them silently overstates it by the chrome height
    // and the bottom of the layout (the version footer) falls off the buffer.
    // Ask the kernel what the canvas actually is instead of computing it.
    {
        int cw = 0, ch = 0;
        if (win_get_size(window_handle, &cw, &ch) == 0 && cw > 0 && ch > 0) {
            g_win_w = cw; g_win_h = ch;
        }
        setlog_n("SET: content w", (long)g_win_w);
        setlog_n("SET: content h", (long)g_win_h);
    }

    // #74: open the panel the desktop context menu asked for (one-shot).
    {
        int _tab = get_settings_tab();
        if (_tab >= 0 && _tab < PANEL_COUNT) current_panel = _tab;
        set_settings_tab(-1);
    }

    // (#565) Sync current_theme from /CONFIG/THEME.CFG's active slug so the
    // dropdown highlights the correct selection when the window opens. This
    // replaces a hardcoded kernel-index switch (dark=1/light=2/classic=4/...)
    // that only knew about the 12 themes that used to be compiled into the
    // kernel; a file-based/App-Store-installed theme has no fixed index, so
    // slug is the only stable identity now.
    {
        char active_slug[GUI_THEME_SLUG_MAX];
        current_theme = 0;
        if (gui_theme_get_active_slug(active_slug, sizeof(active_slug)) && active_slug[0]) {
            for (int i = 0; i < g_th_count; i++) {
                if (strcmp(g_th[i].slug, active_slug) == 0) { current_theme = i; break; }
            }
        }
    }

    // Load persisted Settings-app preferences (timezone, formats, accent, etc.)
    // before applying theme/timezone so the restored values take effect.
    settings_load();
    alerts_load();
    privacy_load();          // (#382 pass2) restore persisted privacy settings
    sm_load();               // (#: Start Menu uplift) restore persisted Start Menu prefs
    setlog("SET: settings_load done");
    // #387: read the compositor's current dock style so the picker shows it.
    {
        int fd = userconf_open_read("DOCKSTYL.CFG", "/DOCKSTYL.CFG");  // #683
        if (fd >= 0) {
            char c = 0;
            // #745: the accepted digit range comes from DOCK_OPTS_COUNT. It used
            // to be a literal '4', a third place that had to be edited by hand
            // whenever a dock style was added.
            if (sys_read(fd, &c, 1) == 1 &&
                c >= '0' && c < (char)('0' + DOCK_OPTS_COUNT)) dock_style = c - '0';
            sys_close(fd);
        }
    }
    // #745-rootcause: this was an unconditional set_cursor() call, a
    // PUSH of Settings' own last-persisted index into the kernel
    // on every launch - the identical anti-pattern #560 fixed for the
    // screensaver a few lines below, and left flagged in blame.md as an
    // unfixed follow-up. It bit for real because the shipped asset base
    // carries a stale /SETTINGS.CFG containing `c=2` (Glow) while
    // /UIPROFIL.YML carries `curstyle: 0`, so merely OPENING Settings, for any
    // reason, replaced the user's arrow with the pulsing Glow disc - nobody
    // ever touched the picker. Now a READ-ONLY sync FROM live kernel state
    // (same shape as the current_theme and screensaver syncs): opening
    // Settings cannot mutate the cursor, the picker shows what is actually
    // running, and an out-of-range kernel value falls back to 0, the normal
    // Light arrow. Glow is never a fallback, only an explicit choice.
    {
        int pk = get_cursor();                        /* style | size<<8 */
        int kstyle = (pk >= 0) ? (pk & 0xFF) : 0;
        cursor_theme = OPT_CLAMP(kstyle, CURSOR_OPTS);
    }
    setlog("SET: cursor synced from kernel (read-only)");
    // (#745) Same for the glass opacity, so the slider opens showing what the
    // compositor is ACTUALLY using rather than whatever Settings last wrote.
    // The CFG file stays authoritative on load; the SETTINGS.CFG mirror is for
    // Settings' own restore only, so the two can never fight.
    {
        int fd = userconf_open_read("DOCKOPAC.CFG", "/DOCKOPAC.CFG");  // #683
        if (fd >= 0) {
            char b[8];
            long n = sys_read(fd, b, sizeof(b) - 1);
            sys_close(fd);
            if (n > 0) {
                b[n] = 0;
                int v = 0, any = 0;
                for (long i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++) {
                    v = v * 10 + (b[i] - '0'); any = 1;
                }
                if (any && v >= DOCK_OPACITY_MIN && v <= DOCK_OPACITY_MAX) dock_opacity = v;
            }
        }
    }
    // (#123) Same read-back for the two geometry preferences, for the same
    // reason: the compositor seeds these CFGs from UIPROFIL.YML at startup, so
    // reading them here is what makes the sliders open showing what the dock is
    // ACTUALLY running rather than this app's compiled-in default (which would
    // make the first click of the slider jump the dock).
    {
        struct { const char *nm; const char *legacy; int lo; int hi; int *dst; } G[2] = {
            { "DOCKHGT.CFG",  "/DOCKHGT.CFG",  DOCK_HEIGHT_MIN, DOCK_HEIGHT_MAX, &dock_height },
            { "DOCKZOOM.CFG", "/DOCKZOOM.CFG", DOCK_ZOOM_MIN,   DOCK_ZOOM_MAX,   &dock_zoom   },
        };
        for (int gi = 0; gi < 2; gi++) {
            int fd = userconf_open_read(G[gi].nm, G[gi].legacy);
            if (fd < 0) continue;
            char b[8];
            long n = sys_read(fd, b, sizeof(b) - 1);
            sys_close(fd);
            if (n <= 0) continue;
            b[n] = 0;
            int v = 0, any = 0;
            for (long i = 0; i < n && b[i] >= '0' && b[i] <= '9'; i++) {
                v = v * 10 + (b[i] - '0'); any = 1;
            }
            if (any && v >= G[gi].lo && v <= G[gi].hi) *G[gi].dst = v;
        }
    }
    if (cursor_theme < 0 || cursor_theme > 2) cursor_theme = 0;
    set_cursor(cursor_theme, 100);   // (#116) sync kernel cursor with loaded pref
    setlog("SET: cursor applied");
    // #560-rootcause: SYNC FROM the live kernel state instead of pushing
    // Settings' own last-persisted idx/delay INTO the kernel unconditionally.
    // Settings keeps its own copy of "which screensaver" in SETTINGS.CFG (a
    // different file, a different index space, from the compositor's
    // UIPROFIL.YML/set_screensaver() kernel state). The old code pushed that
    // stale local copy into the kernel on EVERY Settings launch, so merely
    // opening Settings for any reason (not touching the screensaver picker at
    // all) silently reverted whatever type/delay had been set through
    // UIPROFIL.YML, the tray menu, or AI chat back to whatever Settings last
    // remembered. Read-only sync here (same pattern as the current_theme sync
    // above) so opening Settings can never change what is currently running;
    // the type/delay only change when the user actually picks a new one in
    // this session (ss_dd_changed / the Apply Delay button).
    {
        int kss = get_screensaver();
        int found = -1;
        for (int i = 0; i < SS_OPTS_COUNT; i++) {
            if (SS_KERNEL_MAP[i] == kss) { found = i; break; }
        }
        // #124: the unmapped fallback now points at the same default the
        // kernel and the compositor use (Plasma (Classic), idx 21), rather
        // than a third answer. A fallback that disagrees with the real
        // default shows the user a saver the system is not running.
        screensaver_idx = (found >= 0) ? found : 21;   // 21=Plasma (Classic) if unmapped
    }
    {
        int kdelay = get_ss_delay();
        // #652: fallback was 2 minutes while the kernel default is now 600s.
        // A fallback that disagrees with the real default silently shows the
        // user a number the system is not using.
        screensaver_delay_min = (kdelay >= 5) ? ((kdelay + 59) / 60) : 10;
    }

    // Apply initial theme colors (also calls set_theme to confirm kernel state)
    apply_theme(current_theme);
    setlog("SET: theme applied");

    // #49/#50: adopt THE shared setting. Order matters: settings_load() has
    // already run, so legacy_tz_idx is populated if this install had a zone
    // stored under the old private 't' key.
    settings_tz_init();

    // Populate initial network data
    setlog("SET: net probe begin");
    // #786: one definition (net_pull_live), so the FIRST paint cannot drift
    // from the panel-entry paint the way two copies of this block could. #144's
    // config_static -> dhcp_enabled fix is inside it.
    net_pull_live();
    setlog("SET: net probe done");

    // Read live kernel state so sliders/values reflect reality on first open
    setlog("SET: volume/opacity/mouse probe begin");
    master_volume = get_volume();
    transparency_level = get_win_opacity() * 100 / 255;   // window opacity %, #112
    if (transparency_level < 5)   transparency_level = 5;
    if (transparency_level > 100) transparency_level = 100;
    {
        int spd = get_mouse_speed();  // kernel range 1-10
        pointer_speed = (spd - 1) * 100 / 9;
        if (pointer_speed < 0)   pointer_speed = 0;
        if (pointer_speed > 100) pointer_speed = 100;
    }
    setlog("SET: disk probe begin (get_disk_total/free/info+SMART)");
    {
        long total_mb = get_disk_total_mb();
        long free_mb  = get_disk_free_mb();
        if (drive_count > 0) {
            drives[0].total_bytes = (uint64_t)total_mb * 1024 * 1024;
            drives[0].used_bytes  = (uint64_t)(total_mb - free_mb) * 1024 * 1024;
        }
        // Real drive identity + SMART health from the kernel ATA driver.
        drives[0].smart = -1;
        {
            disk_info_t di;
            if (get_disk_info(0, &di) == 0 && di.present) {
                copy_str(drives[0].model,  di.model,  sizeof(drives[0].model));
                copy_str(drives[0].serial, di.serial, sizeof(drives[0].serial));
                drives[0].smart = di.smart;
            }
        }
    }
    setlog("SET: disk probe done");

    // Load the real account list from the kernel.
    users_refresh();
    setlog("SET: users_refresh done");

    // #238: read the LIVE packet-filter policy out of the kernel. No file is
    // opened and no default is seeded here: the kernel already holds the
    // policy in force, including its compiled-in fail-safe on a virgin image.
    fw_pull();
    setlog("SET: fw_pull done");

    // Sync the wallpaper selector with the compositor's current background.
    {
        int wp = get_wallpaper();
        if (wp >= 0 && wp < WALLPAPER_NAMES_COUNT) wallpaper_idx = wp;
    }

    // Scan real cache/trash directory sizes so Storage shows live numbers.
    storage_scan();
    setlog("SET: storage_scan done");

    // Write the full-review self-report so displayed-vs-real can be checked
    // over SSH (cat /SETVALS.TXT) even if the window never appears.
    write_setvals();
    setlog("SET: setvals written");

    // Create window

    printf("Settings: Comprehensive Settings app started (handle=%d)\n", window_handle);
    printf("Settings: 12 panels with full interactive controls\n");

    // Final draw with live hardware data (window was already shown by the early
    // draw above; this just refreshes it).
    setlog("SET: final draw begin");
    draw_all();
    setlog("SET: final draw done");

    // Event loop
    setlog("SET: event loop entered");
    gui_event_t event;
    int running = 1;

    while (running) {
        int event_type = win_get_event(window_handle, &event, 100);

        settings_autosave();   // persist Settings-app prefs when they change

        // (#704) Settings caches theme colors into COL_* globals inside
        // apply_theme(), set once at startup and on an explicit user action
        // (theme dropdown, accent swatch). Unlike an app that calls
        // theme_color() live from its draw function, Settings will NOT pick
        // up a live .mtheme file edit just because the compositor's poll
        // reloaded the kernel's live table and forced an EVENT_REDRAW
        // (SYS_WM_FORCE_REDRAW_ALL): fb_redraw() would just repaint the same
        // stale COL_* values. So Settings needs its own copy of the same
        // throttled poll (dock_style_poll()/startmenu_prefs_poll() idiom,
        // ~2s) to notice the file changed and re-run apply_theme(). Cheap:
        // gui_theme_poll_reload() is a content-hash compare against a small
        // buffer, not a redraw, and no-ops when nothing changed.
        {
            static uint64_t s_theme_poll_due = 0;
            uint64_t now_tp = uptime_ms();
            if (now_tp >= s_theme_poll_due) {
                s_theme_poll_due = now_tp + 2000;
                if (gui_theme_poll_reload()) {
                    apply_theme(current_theme);
                    draw_all();
                }
            }
        }

        // (#745 task #67) While the Dock panel is open, re-read the pinned
        // favourites every ~2s (same throttled idiom as the theme poll just
        // above) so a right-click Pin/Unpin done directly in the live dock
        // is reflected here without the user having to leave and re-enter
        // the panel. Cheap: a small STARTMENU.CFG read, not a full redraw,
        // unless the pinned set actually changed.
        if (current_panel == PANEL_DOCK) {
            static uint64_t s_dockfav_poll_due = 0;
            uint64_t now_df = uptime_ms();
            if (now_df >= s_dockfav_poll_due) {
                s_dockfav_poll_due = now_df + 2000;
                int old_n = g_dockfav_pinned_n;
                char old_paths[DOCKFAV_MAX][128];
                for (int i = 0; i < old_n; i++) copy_str(old_paths[i], g_dockfav_pinned[i], sizeof(old_paths[0]));
                dockfav_refresh_pinned();
                int changed = (g_dockfav_pinned_n != old_n);
                if (!changed) for (int i = 0; i < old_n; i++)
                    if (strcmp(old_paths[i], g_dockfav_pinned[i]) != 0) { changed = 1; break; }
                if (changed) { dockfav_rebuild_candidates(); draw_all(); }
            }
        }

        // #74/#382: honour a live tab-switch request even while already open, so
        // the desktop context menu (or a repeated "Display Settings") retargets
        // this window's panel instead of doing nothing. One-shot; -1 = no request.
        {
            int _rt = get_settings_tab();
            if (_rt >= 0 && _rt < PANEL_COUNT) {
                if (_rt != current_panel) { current_panel = _rt; draw_all(); }
                set_settings_tab(-1);
            }
        }

        // (#372/#384, honesty pass #237) bt_tick()/wifi_tick() are now no-ops
        // and bt_panel_animating()/wifi_panel_animating() always return 0 (no
        // driver -> nothing ever scans, pairs or connects, so nothing ever
        // animates). Left in place rather than deleted: it is the real
        // per-panel poll seam a future driver's animation would tick through,
        // and removing it would be one more thing to re-add for #383.
        {
            static unsigned long last_conn_tick = 0;
            unsigned long nowc = uptime_ms();
            if (nowc - last_conn_tick >= 90) {
                last_conn_tick = nowc;
                if (current_panel == PANEL_BLUETOOTH) {
                    bt_tick();
                    if (bt_panel_animating()) { g_bt_spin = (g_bt_spin + 1) & 7; draw_all(); }
                } else if (current_panel == PANEL_WIFI) {
                    wifi_tick();
                    if (wifi_panel_animating()) { g_bt_spin = (g_bt_spin + 1) & 7; draw_all(); }
                }
            }
        }

        if (event_type == 0) {
            // (#267) reveal a hover tooltip after the mouse has been still.
            if (g_help_lx >= 0 && modal_mode == MODAL_NONE && !g_dd_open) {
                help_ui_tick(g_help_lx, g_help_ly, uptime_ms());
                draw_all();
            }
            continue;
        }

        switch (event.type) {
            case EVENT_RESIZE:
                if (event.mouse_x > 0 && event.mouse_y > 0) { g_win_w = event.mouse_x; g_win_h = event.mouse_y; }
                draw_all();
                break;
            case EVENT_REDRAW:
                draw_all();
                break;

            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_KEY_DOWN:
                // Route all keypresses to the modal dialog if one is active
                if (modal_mode != MODAL_NONE) {
                    // (local 71) Credits is a scrollable document, so the shared
                    // scroll primitive gets first refusal on the navigation
                    // keys. This is the ONLY route to the rest of the list
                    // wherever the pointer has no wheel (the Magic Mouse on the
                    // iMac14,4 target, #438) or the mouse is dead. It returns 0
                    // for keys it does not own, so ESC/Tab/Enter still work.
                    if (modal_mode == MODAL_CREDITS) {
                        credits_layout();
                        if (gui_scroll_key(&g_credits_scroll, event.keycode)) {
                            draw_all();
                            break;
                        }
                    }
                    if (event.key_char == 27) {  // ESC: cancel
                        modal_mode = MODAL_NONE;
                        draw_all();
                    } else if (event.key_char == '\t') {  // Tab: cycle fields
                        modal_active_field = (modal_active_field + 1) % (modal_num_fields > 0 ? modal_num_fields : 1);
                        modal_error[0] = '\0';
                        draw_all();
                    } else if (event.key_char == '\r' || event.key_char == '\n') {  // Enter
                        if (modal_num_fields == 0) {
                            modal_mode = MODAL_NONE; draw_all();
                        } else if (modal_active_field < modal_num_fields - 1) {
                            modal_active_field++;
                            draw_all();
                        } else {
                            do_modal_submit();
                        }
                    } else {
                        // Caret-aware editing (Left/Right/Home/End/Delete/
                        // Backspace/insert) via the shared textfield helper.
                        // modal_cursor[f] tracks LENGTH; modal_caret[f] the caret.
                        int f = modal_active_field;
                        textfield_t tf;
                        tf_attach(&tf, modal_field[f], 64, modal_cursor[f], modal_caret[f]);
                        if (tf_handle_key(&tf, &event)) {
                            modal_cursor[f] = tf.len;
                            modal_caret[f]  = tf.cursor;
                            modal_error[0]  = '\0';
                            draw_all();
                        }
                    }
                    break;
                }
                // (#267) F1 opens context help for the current panel.
                if (help_ui_is_f1(event.keycode)) {
                    help_ui_open_topic(HELP_FILE, help_topic_for_panel(current_panel));
                    break;
                }
                // (#261, revised #74) When a dropdown is OPEN, arrow keys navigate
                // ITS list. Up/Down used to call on_change on every highlight move
                // ("live preview"), but every on_change here is a real syscall
                // and/or a real disk write (see the #74 comment above dropdown_open),
                // so passing through option 2 on the way from 1 to 5 used to APPLY
                // option 2. Up/Down now only move *g_dd_sel (which still updates the
                // label drawn behind the closed control) and reveal it in the
                // scrolled popup - no on_change call, so browsing is now a real
                // preview instead of four unwanted applies. Enter/Space COMMIT
                // (fire on_change exactly once, for whatever is currently
                // highlighted) and close. Esc CANCELS: restores *g_dd_sel to
                // g_dd_orig (the value dropdown_open() captured) and closes without
                // ever calling on_change.
                if (g_dd_open && g_dd_sel) {
                    if (event.keycode == 0x80) {                 // Up: preview only
                        dropdown_refresh();
                        if (*g_dd_sel > 0) (*g_dd_sel)--;
                        gui_scroll_reveal(&g_dd_list.scroll, *g_dd_sel * DD_ROW, DD_ROW);
                        draw_all(); break;
                    }
                    if (event.keycode == 0x81) {                 // Down: preview only
                        dropdown_refresh();
                        if (*g_dd_sel < g_dd_count - 1) (*g_dd_sel)++;
                        gui_scroll_reveal(&g_dd_list.scroll, *g_dd_sel * DD_ROW, DD_ROW);
                        draw_all(); break;
                    }
                    if (event.key_char == '\r' || event.key_char == '\n' ||
                        event.key_char == ' ') {                 // commit
                        if (g_dd_on_change) g_dd_on_change();
                        g_dd_open = 0;
                        draw_all(); break;
                    }
                    if (event.key_char == 27) {                  // Esc: cancel
                        *g_dd_sel = g_dd_orig;
                        g_dd_open = 0;
                        draw_all(); break;
                    }
                }
                if (event.key_char == 27) {  // ESC
                    running = 0;
                } else if (event.key_char == '\t') {
                    // Tab cycles the keyboard focus ring (sidebar + controls).
                    g_focus_on = 1;
                    if (g_focus_n > 0) g_focus_idx = (g_focus_idx + 1) % g_focus_n;
                    draw_all();
                } else if (current_panel == PANEL_EXTSVC && (g_ext_focus >= 0 || g_ai_focus >= 0) && ext_key(event.key_char, event.keycode)) {
                    draw_all();
                } else if (event.keycode == 0x80) {  // Up arrow: previous control
                    g_focus_on = 1;
                    if (g_focus_n > 0) g_focus_idx = (g_focus_idx + g_focus_n - 1) % g_focus_n;
                    draw_all();
                } else if (event.keycode == 0x81) {  // Down arrow: next control
                    g_focus_on = 1;
                    if (g_focus_n > 0) g_focus_idx = (g_focus_idx + 1) % g_focus_n;
                    draw_all();
                } else if (event.key_char == '\r' || event.key_char == '\n' || event.key_char == ' ') {
                    // Enter/Space activates the focused control via its click handler.
                    if (g_focus_on && g_focus_n > 0) {
                        if (g_focus_idx >= g_focus_n) g_focus_idx = 0;
                        focus_rect_t *fr = &g_focus[g_focus_idx];
                        int cx = fr->x + fr->w / 2, cy = fr->y + fr->h / 2;
                        if (fr->sidebar) handle_sidebar_click(cx, cy);
                        else             handle_content_click(cx, cy);
                        draw_all();
                    }
                } else if (event.keycode == GUI_KEY_LEFT) {   // previous panel
                    if (current_panel > 0) { current_panel--; gui_scroll_set(&g_content_scroll, 0); g_focus_idx = 0; sidebar_reveal_current(); draw_all(); }
                } else if (event.keycode == GUI_KEY_RIGHT) {  // next panel
                    if (current_panel < PANEL_COUNT - 1) { current_panel++; gui_scroll_set(&g_content_scroll, 0); g_focus_idx = 0; sidebar_reveal_current(); draw_all(); }
                } else if (event.keycode == GUI_KEY_HOME) {
                    current_panel = 0; gui_scroll_set(&g_content_scroll, 0); g_focus_idx = 0; sidebar_reveal_current(); draw_all();
                } else if (event.keycode == GUI_KEY_END) {
                    current_panel = PANEL_COUNT - 1; gui_scroll_set(&g_content_scroll, 0); g_focus_idx = 0; sidebar_reveal_current(); draw_all();
                } else if (event.keycode == GUI_KEY_PGUP || event.keycode == GUI_KEY_PGDN) {
                    // (#227) Context-sensitive: before this fix the content
                    // area had NO keyboard scroll route at all (this branch
                    // only ever paged the sidebar), so a control below the
                    // fold was unreachable without a mouse wheel. When the
                    // keyboard focus ring is currently sitting on a CONTENT
                    // control (not a sidebar row), page the content instead;
                    // otherwise keep the existing sidebar-paging behaviour -
                    // the keyboard-only route to a panel scrolled out of
                    // view wherever the pointer's wheel is unavailable (the
                    // Magic Mouse on the iMac, #438) or the mouse is dead.
                    if (g_focus_on && g_focus_n > 0 && g_focus_idx < g_focus_n &&
                        !g_focus[g_focus_idx].sidebar) {
                        if (gui_scroll_key(&g_content_scroll, event.keycode)) draw_all();
                    } else {
                        if (gui_scroll_key(&g_side_scroll, event.keycode)) draw_all();
                    }
                }
                break;

            case EVENT_MOUSE_DOWN: {
                // Get current window position (it may have moved since startup)
                win_get_pos(window_handle, &win_x, &win_y);
                int local_x = event.mouse_x;
                int local_y = event.mouse_y;

                // An open dropdown captures the next click (select item or dismiss)
                if (g_dd_open) { dropdown_click(local_x, local_y); break; }

                if (local_x < 0 || local_y < 0) break;

                // If modal is open, only handle modal clicks
                if (modal_mode != MODAL_NONE) {
                    // dw/dh MUST match draw_modal exactly or the button
                    // hit-test misses; both now come from the shared helpers.
                    int dw = modal_dw();
                    int dh = modal_dh();
                    int dx = (WIN_WIDTH  - dw) / 2;
                    int dy = (WIN_HEIGHT - dh) / 2;
                    // Credits has a single Close button at the OK position.
                    if (modal_mode == MODAL_CREDITS) {
                        int by = dy + dh - 40;
                        if (local_y >= by && local_y < by + 30 &&
                            local_x >= dx + dw - 96 && local_x < dx + dw - 16) {
                            modal_mode = MODAL_NONE; draw_all();
                            break;
                        }
                        // A press in the scrollbar gutter belongs to the
                        // scrollbar: grab the thumb, or page toward the click.
                        credits_layout();
                        if (gui_scroll_press(&g_credits_scroll, local_x, local_y))
                            draw_all();
                        break;
                    }
                    // Field click: select field
                    int hit_field = 0;
                    for (int i = 0; i < modal_num_fields; i++) {
                        int fy = dy + 46 + i * 44;
                        if (local_y >= fy + 16 && local_y < fy + 38 &&
                            local_x >= dx + 14 && local_x < dx + 14 + dw - 28) {
                            modal_active_field = i;
                            modal_error[0] = '\0';
                            draw_all();
                            hit_field = 1;
                            break;
                        }
                    }
                    // (#745) Picture picker swatch click, MODAL_EDIT_PROFILE
                    // only - geometry MUST match draw_modal()'s swatch row
                    // exactly (same reason the dh comment above gives for the
                    // OK/Cancel buttons), so it reuses modal_avatar_row_y().
                    if (!hit_field && modal_mode == MODAL_EDIT_PROFILE) {
                        int ay = modal_avatar_row_y(dy);
                        int sx = dx + 60;
                        for (int i = 0; i < 8; i++) {
                            int cx = sx + i * 30, cy = ay + 32;
                            int ddx = local_x - cx, ddy = local_y - cy;
                            if (ddx * ddx + ddy * ddy <= 15 * 15) {
                                modal_avatar_idx = i;
                                draw_all();
                                hit_field = 1;
                                break;
                            }
                        }
                    }
                    if (!hit_field) {
                        int btn_y = dy + dh - 40;
                        if (local_y >= btn_y && local_y < btn_y + 30) {
                            // Cancel button
                            if (local_x >= dx + dw - 184 && local_x < dx + dw - 104) {
                                modal_mode = MODAL_NONE;
                                draw_all();
                            }
                            // OK button
                            if (local_x >= dx + dw - 96 && local_x < dx + dw - 16) {
                                do_modal_submit();
                            }
                        }
                    }
                    break;
                }

                // (#267) Click the "?" help icon -> open context help.
                if (help_ui_question_hit(HELP_Q_X, HELP_Q_Y, HELP_Q_D,
                                         local_x, local_y)) {
                    help_ui_open_topic(HELP_FILE,
                                       help_topic_for_panel(current_panel));
                    break;
                }
                if (local_x < SIDEBAR_WIDTH) {
                    handle_sidebar_click(local_x, local_y);
                } else {
                    handle_content_click(local_x, local_y);
                }
                break;
            }

            case EVENT_MOUSE_MOVE: {
                win_get_pos(window_handle, &win_x, &win_y);
                int local_x = event.mouse_x;
                int local_y = event.mouse_y;

                // (local 71) A modal owns the pointer. Route the drag to the
                // credits scrollbar and do not also run the hover tracker for
                // controls that are behind the dialog.
                if (modal_mode == MODAL_CREDITS) {
                    if (gui_scroll_motion(&g_credits_scroll, local_x, local_y))
                        draw_all();
                    break;
                }

                // (#267) feed the hover-tooltip tracker (window-relative coords).
                g_help_lx = local_x; g_help_ly = local_y;
                help_ui_tick(local_x, local_y, uptime_ms());
                draw_all();
                // (#291/#438) Scrollbar thumb drag + thumb hover. A live drag
                // owns the pointer, so it must not also hover-highlight the rows
                // sliding under it.
                if (gui_scroll_motion(&g_side_scroll, local_x, local_y)) draw_all();
                if (g_side_scroll.drag) break;
                if (local_x >= 0 && local_x < SIDEBAR_WIDTH && local_y >= 0) {
                    handle_sidebar_hover(local_x, local_y);
                } else if (hover_panel != -1) {
                    hover_panel = -1;
                    draw_all();
                }
                break;
            }

            // (#291/#438) Settings never handled EVENT_MOUSE_UP at all, so a
            // scrollbar thumb grab had no way to be released and the thumb would
            // stay stuck to the pointer. Any future drag interaction needs this.
            case EVENT_MOUSE_UP:
                gui_scroll_release(&g_side_scroll);
                gui_scroll_release(&g_credits_scroll);   // (local 71)
                break;

            case EVENT_MOUSE_SCROLL: {
                // (local 71) A modal owns the wheel. Before this, wheel events
                // fell straight through to the sidebar and content branches
                // while a dialog was up, scrolling what was behind it.
                if (modal_mode == MODAL_CREDITS) {
                    credits_layout();
                    if (gui_scroll_wheel(&g_credits_scroll, event.scroll_delta))
                        draw_all();
                    break;
                }
                if (modal_mode != MODAL_NONE) break;

                // An open dropdown scrolls its list. Uses gui_scroll_by()
                // directly rather than gui_scroll_wheel() (#512) to keep this
                // widget's established one-row-per-notch feel; gui_scroll_by
                // still owns the clamp against gui_scroll_max() instead of
                // the hand-rolled "vis"/count clamp this used to do.
                // Root-cause fix (shared gui_list primitive): gate on
                // gui_list_hit() - anywhere over the popup's full box,
                // scrollbar gutter included - instead of applying to the
                // popup no matter where the cursor is in the whole window.
                if (g_dd_open) {
                    dropdown_refresh();
                    if (gui_list_hit(&g_dd_list, event.mouse_x, event.mouse_y))
                        gui_scroll_by(&g_dd_list.scroll, -event.scroll_delta * DD_ROW);
                    draw_all();
                    break;
                }
                // Scroll content if needed
                win_get_pos(window_handle, &win_x, &win_y);
                int local_x = event.mouse_x;
                if (local_x < SIDEBAR_WIDTH) {
                    // (#291/#438) Wheel over the sidebar scrolls the panel list.
                    // Direction and step come from the shared primitive, so this
                    // cannot drift out of agreement with every other list.
                    if (gui_scroll_wheel(&g_side_scroll, event.scroll_delta)) draw_all();
                } else {
                    // (#227) Same shared primitive, same convention as the
                    // sidebar above - this used to be a hand-rolled
                    // content_scroll_y/max_scroll_y pair where max_scroll_y
                    // was never assigned anything but 0, so every scroll
                    // clamped straight back to zero.
                    if (gui_scroll_wheel(&g_content_scroll, event.scroll_delta)) draw_all();
                }
                break;
            }

            default:
                break;
        }
    }

    // Cleanup
    win_destroy(window_handle);
    printf("Settings: Closed\n");

    return 0;
}
