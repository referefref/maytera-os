// settings - Comprehensive System Settings for MayteraOS
// Full-featured settings application with 12 panels
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_font.h"
#include "../../libc/gui_scroll.h"   // (#291/#438) shared scrollable-viewport primitive
#include "../../libc/syscall.h"
#include "../../libc/wallpapers.h"   // (#517) shared wallpaper enumeration (same as compositor)
#include "../../libc/assoc.h"
#include "../../libc/devinfo.h"      // (#382) real CPU/RAM (SYS_SYSINFO) + PCI (SYS_DEV_PCI_LIST)
#include "../../libhelp/help_ui.h"   // (#267) help subsystem: tooltips, "?" icon, F1
#define BT_MOCK_IMPL                 // (#372) this TU owns the Bluetooth mock state
#include "../../libc/bt_client.h"    // (#372) Bluetooth client API + mock
#define WIFI_MOCK_IMPL               // (#384) this TU owns the Wi-Fi mock state
#include "../../libc/wifi_client.h"  // (#384) Wi-Fi client API + mock

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
    "External Services"
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
    "[X]"   // External Services
};

// =============================================================================
// Theme Colors - Comprehensive color scheme
// =============================================================================

// Dark theme (default)
static uint32_t COL_SIDEBAR_BG;
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
#define NUM_ACCENT_COLORS 8

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

// Scroll state for content areas
static int content_scroll_y = 0;
static int max_scroll_y = 0;

// Active dropdown/popup state
static int active_dropdown = -1;
static int dropdown_hover_item = -1;

// =============================================================================
// Settings State - Appearance
// =============================================================================
static int current_theme = 0;       // 0=Dark, 1=Light, 2=Classic, 3=Ocean, 4=Nord
static int screensaver_idx = 1;     // 0=Off,1=Starfield,2=Flux,3=Lines,4=Bubbles
static int screensaver_delay_min = 2;   // (#115) activation delay, minutes (custom steps)
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
static int cursor_theme = 0;        // 0=Default, 1=Dark, 2=Light, 3=Large
static int dock_style = 0;          // #387 0=Default 1=Lumina 2=Classic UNIX 3=Retro Bench
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

// =============================================================================
// Settings State - Display
// =============================================================================
static int brightness = 80;
static bool night_light = false;
static int night_light_strength = 50;
static int night_light_start_hour = 20;
static int night_light_end_hour = 6;
static int scaling_factor = 100;    // 100%, 125%, 150%, 175%, 200%
static int color_temp = 6500;       // Color temperature in Kelvin
static int gamma_r = 100, gamma_g = 100, gamma_b = 100;  // Gamma per channel

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
static char dns_primary[16] = "8.8.8.8";
static char dns_secondary[16] = "8.8.4.4";
static char mac_address[18] = "BC:24:11:80:C9:5B";

// (#382 pass2) Removed vpn_enabled / vpn_protocol: there is no VPN client stack,
// so the Network panel marks VPN honestly as unavailable instead of a fake toggle.

// Proxy settings
static bool proxy_enabled = false;
static int proxy_type = 0;  // 0=HTTP, 1=SOCKS4, 2=SOCKS5
static char proxy_host[64] = "";
static int proxy_port = 8080;

// Firewall (iptables-style: default in/out policy + explicit allow/deny rules)
static bool firewall_enabled = true;
static int  fw_pol_in  = 1;     // default inbound policy:  0=Allow, 1=Deny
static int  fw_pol_out = 0;     // default outbound policy: 0=Allow, 1=Deny
#define MAX_FW_RULES 12
typedef struct { int dir; int action; int proto; int port; } fw_rule_t;
// dir: 0=IN, 1=OUT   action: 0=ALLOW, 1=DENY   proto: 0=TCP, 1=UDP
static fw_rule_t fw_rules[MAX_FW_RULES];
static int fw_rule_count = 0;

static void fw_add(int dir, int action, int proto, int port) {
    if (fw_rule_count >= MAX_FW_RULES) return;
    fw_rules[fw_rule_count].dir = dir; fw_rules[fw_rule_count].action = action;
    fw_rules[fw_rule_count].proto = proto; fw_rules[fw_rule_count].port = port;
    fw_rule_count++;
}

static void fw_save(void) {
    char buf[1024]; char *p = buf;
    const char *e = firewall_enabled ? "on" : "off";
    while (*e) *p++ = *e++;
    *p++ = '\n';
    // "in N" / "out N"
    p += 0;
    char hdr[32];
    int n;
    n = 0; { const char *s = "pin "; while (*s) hdr[n++] = *s++; } hdr[n++] = '0' + fw_pol_in; hdr[n++] = '\n'; hdr[n] = 0;
    { const char *s = hdr; while (*s) *p++ = *s++; }
    n = 0; { const char *s = "pout "; while (*s) hdr[n++] = *s++; } hdr[n++] = '0' + fw_pol_out; hdr[n++] = '\n'; hdr[n] = 0;
    { const char *s = hdr; while (*s) *p++ = *s++; }
    for (int i = 0; i < fw_rule_count; i++) {
        fw_rule_t *r = &fw_rules[i];
        char line[24]; int li = 0;
        line[li++] = 'r'; line[li++] = ' ';
        line[li++] = '0' + r->dir;    line[li++] = ' ';
        line[li++] = '0' + r->action; line[li++] = ' ';
        line[li++] = '0' + r->proto;  line[li++] = ' ';
        char t[8]; int tn = 0; int v = r->port;
        if (v == 0) t[tn++] = '0';
        while (v) { t[tn++] = '0' + v % 10; v /= 10; }
        while (tn) line[li++] = t[--tn];
        line[li++] = '\n'; line[li] = 0;
        const char *s = line; while (*s) *p++ = *s++;
    }
    sys_unlink("FWRULES.CFG");
    int fd = sys_open("FWRULES.CFG", 0x41);   // O_WRONLY|O_CREAT
    if (fd < 0) return;
    sys_write(fd, buf, (unsigned long)(p - buf));
    sys_close(fd);
}

static void fw_load(void) {
    fw_rule_count = 0;
    int fd = sys_open("FWRULES.CFG", 0);
    if (fd < 0) {
        // Sensible defaults: deny inbound by default, allow common services.
        fw_pol_in = 1; fw_pol_out = 0;
        fw_add(0, 0, 0, 22);    // allow in  tcp ssh
        fw_add(0, 0, 0, 2323);  // allow in  tcp remote-control
        fw_add(1, 0, 0, 80);    // allow out tcp http
        fw_add(1, 0, 0, 443);   // allow out tcp https
        fw_add(1, 0, 1, 53);    // allow out udp dns
        return;
    }
    static char b[1024];
    long got = sys_read(fd, b, sizeof(b) - 1);
    sys_close(fd);
    if (got <= 0) return;
    b[got] = 0;
    int i = 0;
    while (b[i]) {
        // parse one line
        if (b[i] == 'p' && b[i+1] == 'i' && b[i+2] == 'n') { fw_pol_in = (b[i+4] == '1'); }
        else if (b[i] == 'p' && b[i+1] == 'o' && b[i+2] == 'u' && b[i+3] == 't') { fw_pol_out = (b[i+5] == '1'); }
        else if (b[i] == 'r' && b[i+1] == ' ') {
            int j = i + 2;
            int dir = b[j] - '0'; j += 2;
            int act = b[j] - '0'; j += 2;
            int pro = b[j] - '0'; j += 2;
            int port = 0;
            while (b[j] >= '0' && b[j] <= '9') { port = port * 10 + (b[j] - '0'); j++; }
            fw_add(dir, act, pro, port);
        }
        while (b[i] && b[i] != '\n') i++;
        if (b[i] == '\n') i++;
    }
}

// =============================================================================
// Settings State - Keyboard
// =============================================================================
static int keyboard_layout = 0;
static int key_repeat_rate = 30;    // Characters per second
static int key_repeat_delay = 250;  // Milliseconds
static bool num_lock = true;
static bool caps_lock = false;
static bool scroll_lock = false;

static const char* keyboard_layouts[] = {
    "US English", "UK English", "German (QWERTZ)", "French (AZERTY)",
    "Spanish", "Italian", "Portuguese", "Russian", "Japanese", "Korean"
};
#define NUM_KEYBOARD_LAYOUTS 10

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
static int pointer_speed = 50;      // 0-100
static int double_click_speed = 50; // 0-100 (maps to ms)
static int scroll_speed = 50;
static bool natural_scrolling = false;
static int pointer_size = 1;        // 0=Small, 1=Normal, 2=Large, 3=XLarge
static bool left_handed = false;
static bool pointer_trails = false;
static int pointer_trail_length = 5;
static bool scroll_inertia = true;

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
#define MODAL_ADD_FWRULE      7
#define MODAL_ADD_PRINTER     8   // (#318) name/host/queue/port/default
#define MODAL_WIFI_PASSWORD   9   // (#384) single password field to join a secured SSID
#define MODAL_MAX_FIELDS      5
static int modal_mode = MODAL_NONE;
static int modal_num_fields = 3;
static char modal_field[MODAL_MAX_FIELDS][64];
static int  modal_cursor[MODAL_MAX_FIELDS];  // field LENGTH (kept for existing logic)
static int  modal_caret[MODAL_MAX_FIELDS];   // caret index into modal_field (task #244)
static int  modal_active_field = 0;
static char modal_error[64];
// Extra status variables
static int ntp_status = 0;      // 0=idle, 1=synced ok, -1=failed
static int about_status = 0;    // 0=idle, 1=up-to-date, 2=debug exported
static int sound_test_status = 0; // 0=idle, 1=no output, 2=no input
static int timezone_offset_minutes = 0;

// =============================================================================
// Settings State - Date & Time
// =============================================================================
static int timezone_idx = 12;
static bool use_24hour = true;
static bool auto_time = true;
static int date_format = 0;     // 0=YYYY-MM-DD, 1=MM/DD/YYYY, 2=DD/MM/YYYY
static int first_day_of_week = 0;  // 0=Sunday, 1=Monday

static const char* timezones[] = {
    "UTC-12:00 Baker Island",
    "UTC-11:00 Samoa",
    "UTC-10:00 Hawaii",
    "UTC-09:00 Alaska",
    "UTC-08:00 Pacific (LA)",
    "UTC-07:00 Mountain",
    "UTC-06:00 Central",
    "UTC-05:00 Eastern (NY)",
    "UTC-04:00 Atlantic",
    "UTC-03:00 Buenos Aires",
    "UTC-02:00 Mid-Atlantic",
    "UTC-01:00 Azores",
    "UTC+00:00 London/GMT",
    "UTC+01:00 Paris/Berlin",
    "UTC+02:00 Cairo",
    "UTC+03:00 Moscow",
    "UTC+04:00 Dubai",
    "UTC+05:00 Karachi",
    "UTC+05:30 Mumbai",
    "UTC+06:00 Dhaka",
    "UTC+07:00 Bangkok",
    "UTC+08:00 Singapore",
    "UTC+09:00 Tokyo",
    "UTC+10:00 Sydney",
    "UTC+11:00 Solomon Is.",
    "UTC+12:00 Auckland"
};
#define NUM_TIMEZONES 26

// =============================================================================
// Settings State - Users & Accounts
// =============================================================================
static int current_user_idx = 0;
static bool auto_login = false;
static bool guest_enabled = true;

typedef struct {
    char username[32];
    char fullname[64];
    char email[64];
    int role;           // 0=Admin, 1=User, 2=Guest
    bool password_set;
    uint32_t avatar_color;
} user_account_t;

static user_account_t users[4] = {
    {"admin", "Administrator", "admin@mayteraos.local", 0, true, 0x00569CD6},
    {"user", "Standard User", "user@mayteraos.local", 1, true, 0x0066BB66},
    {"guest", "Guest Account", "", 2, false, 0x00888888},
    {"", "", "", 0, false, 0}
};
static int user_count = 3;

// =============================================================================
// Settings State - Privacy & Security
// =============================================================================
static bool screen_lock_enabled = true;
static int lock_timeout = 5;        // Minutes (0=Never, 1, 2, 5, 10, 15, 30)
static bool require_password_wake = true;
static bool location_services = false;
static bool diagnostics_enabled = true;
static bool crash_reports = true;

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
    int fd = sys_open("/SETLOG.TXT", 0x0001 | 0x0040 | 0x0200);  // O_WRONLY|O_CREAT|O_TRUNC
    if (fd < 0) return;
    sys_write(fd, g_setlog_buf, (unsigned long)g_setlog_len);
    sys_close(fd);
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

static void apply_theme(int theme_id) {
    switch (theme_id) {
        case 0:  // Dark (default)
            COL_SIDEBAR_BG = 0x001E1E1E;
            COL_CONTENT_BG = 0x00252525;
            COL_PANEL_NORMAL = 0x001E1E1E;
            COL_PANEL_HOVER = 0x002D2D2D;
            COL_PANEL_ACTIVE = 0x00383838;
            COL_SEPARATOR = 0x00404040;
            COL_TEXT_PRIMARY = 0x00FFFFFF;
            COL_TEXT_SECONDARY = 0x00AAAAAA;
            COL_TEXT_DISABLED = 0x00666666;
            COL_INPUT_BG = 0x00333333;
            COL_INPUT_BORDER = 0x00505050;
            COL_SLIDER_TRACK = 0x00404040;
            COL_BUTTON_BG = 0x00404040;
            COL_BUTTON_HOVER = 0x00505050;
            COL_CHECKBOX_BG = 0x00333333;
            COL_CARD_BG = 0x002A2A2A;
            break;
        case 1:  // Light
            COL_SIDEBAR_BG = 0x00F0F0F0;
            COL_CONTENT_BG = 0x00FFFFFF;
            COL_PANEL_NORMAL = 0x00F0F0F0;
            COL_PANEL_HOVER = 0x00E0E0E0;
            COL_PANEL_ACTIVE = 0x00D0D0D0;
            COL_SEPARATOR = 0x00CCCCCC;
            COL_TEXT_PRIMARY = 0x00202020;
            COL_TEXT_SECONDARY = 0x00606060;
            COL_TEXT_DISABLED = 0x00999999;
            COL_INPUT_BG = 0x00FFFFFF;
            COL_INPUT_BORDER = 0x00CCCCCC;
            COL_SLIDER_TRACK = 0x00DDDDDD;
            COL_BUTTON_BG = 0x00E8E8E8;
            COL_BUTTON_HOVER = 0x00D8D8D8;
            COL_CHECKBOX_BG = 0x00FFFFFF;
            COL_CARD_BG = 0x00F8F8F8;
            break;
        case 2:  // Classic (Win95 style)
            COL_SIDEBAR_BG = 0x00C0C0C0;
            COL_CONTENT_BG = 0x00C0C0C0;
            COL_PANEL_NORMAL = 0x00C0C0C0;
            COL_PANEL_HOVER = 0x00D0D0D0;
            COL_PANEL_ACTIVE = 0x00000080;
            COL_SEPARATOR = 0x00808080;
            COL_TEXT_PRIMARY = 0x00000000;
            COL_TEXT_SECONDARY = 0x00404040;
            COL_TEXT_DISABLED = 0x00808080;
            COL_INPUT_BG = 0x00FFFFFF;
            COL_INPUT_BORDER = 0x00000000;
            COL_SLIDER_TRACK = 0x00808080;
            COL_BUTTON_BG = 0x00C0C0C0;
            COL_BUTTON_HOVER = 0x00D0D0D0;
            COL_CHECKBOX_BG = 0x00FFFFFF;
            COL_CARD_BG = 0x00D0D0D0;
            break;
        case 3:  // Ocean
            COL_SIDEBAR_BG = 0x001A3A4A;
            COL_CONTENT_BG = 0x00224455;
            COL_PANEL_NORMAL = 0x001A3A4A;
            COL_PANEL_HOVER = 0x00254555;
            COL_PANEL_ACTIVE = 0x00305060;
            COL_SEPARATOR = 0x00406070;
            COL_TEXT_PRIMARY = 0x00E0F0FF;
            COL_TEXT_SECONDARY = 0x0090B0C0;
            COL_TEXT_DISABLED = 0x00607080;
            COL_INPUT_BG = 0x00183040;
            COL_INPUT_BORDER = 0x00406070;
            COL_SLIDER_TRACK = 0x00305060;
            COL_BUTTON_BG = 0x00305060;
            COL_BUTTON_HOVER = 0x00406070;
            COL_CHECKBOX_BG = 0x00183040;
            COL_CARD_BG = 0x001E4050;
            break;
        case 4:  // Nord
            COL_SIDEBAR_BG = 0x002E3440;
            COL_CONTENT_BG = 0x003B4252;
            COL_PANEL_NORMAL = 0x002E3440;
            COL_PANEL_HOVER = 0x00434C5E;
            COL_PANEL_ACTIVE = 0x004C566A;
            COL_SEPARATOR = 0x004C566A;
            COL_TEXT_PRIMARY = 0x00ECEFF4;
            COL_TEXT_SECONDARY = 0x00D8DEE9;
            COL_TEXT_DISABLED = 0x00616E88;
            COL_INPUT_BG = 0x003B4252;
            COL_INPUT_BORDER = 0x004C566A;
            COL_SLIDER_TRACK = 0x004C566A;
            COL_BUTTON_BG = 0x004C566A;
            COL_BUTTON_HOVER = 0x005E6A82;
            COL_CHECKBOX_BG = 0x003B4252;
            COL_CARD_BG = 0x00434C5E;
            break;
        case 5:  // Sunset (warm cream / peach) (#282)
            COL_SIDEBAR_BG = 0x00FFF0E8;
            COL_CONTENT_BG = 0x00FFF8F0;
            COL_PANEL_NORMAL = 0x00FFF0E8;
            COL_PANEL_HOVER = 0x00F8E0D0;
            COL_PANEL_ACTIVE = 0x00E0A080;
            COL_SEPARATOR = 0x00E8C8B0;
            COL_TEXT_PRIMARY = 0x00402010;
            COL_TEXT_SECONDARY = 0x00805840;
            COL_TEXT_DISABLED = 0x00B09080;
            COL_INPUT_BG = 0x00FFFFF8;
            COL_INPUT_BORDER = 0x00C0A080;
            COL_SLIDER_TRACK = 0x00E8C8B0;
            COL_BUTTON_BG = 0x00E0A080;
            COL_BUTTON_HOVER = 0x00F0B090;
            COL_CHECKBOX_BG = 0x00FFFFF8;
            COL_CARD_BG = 0x00FFF4EC;
            break;
        case 6:  // Forest (light green) (#282)
            COL_SIDEBAR_BG = 0x00E8F4E8;
            COL_CONTENT_BG = 0x00F0F8F0;
            COL_PANEL_NORMAL = 0x00E8F4E8;
            COL_PANEL_HOVER = 0x00D8ECD8;
            COL_PANEL_ACTIVE = 0x0080C080;
            COL_SEPARATOR = 0x00C0DCC0;
            COL_TEXT_PRIMARY = 0x00203020;
            COL_TEXT_SECONDARY = 0x00506850;
            COL_TEXT_DISABLED = 0x0090A890;
            COL_INPUT_BG = 0x00FFFFFF;
            COL_INPUT_BORDER = 0x00A0C0A0;
            COL_SLIDER_TRACK = 0x00C0DCC0;
            COL_BUTTON_BG = 0x0080C080;
            COL_BUTTON_HOVER = 0x0090D090;
            COL_CHECKBOX_BG = 0x00FFFFFF;
            COL_CARD_BG = 0x00EAF6EA;
            break;
        case 7:  // Slate Dark (#282)
            COL_SIDEBAR_BG = 0x00202020;
            COL_CONTENT_BG = 0x00282828;
            COL_PANEL_NORMAL = 0x00202020;
            COL_PANEL_HOVER = 0x00323232;
            COL_PANEL_ACTIVE = 0x00484848;
            COL_SEPARATOR = 0x003A3A3A;
            COL_TEXT_PRIMARY = 0x00FFFFFF;
            COL_TEXT_SECONDARY = 0x00B0B0B0;
            COL_TEXT_DISABLED = 0x00707070;
            COL_INPUT_BG = 0x00303030;
            COL_INPUT_BORDER = 0x00484848;
            COL_SLIDER_TRACK = 0x00404040;
            COL_BUTTON_BG = 0x00323232;
            COL_BUTTON_HOVER = 0x003C3C3C;
            COL_CHECKBOX_BG = 0x00303030;
            COL_CARD_BG = 0x002E2E2E;
            break;
    }

    // Apply accent color
    COL_ACCENT = ACCENT_COLORS[accent_color_idx];
    COL_ACCENT_HOVER = COL_ACCENT + 0x00202020;  // Lighten
    COL_SLIDER_FILL = COL_ACCENT;
    COL_SUCCESS = 0x0066BB66;
    COL_WARNING = 0x00DDAA44;
    COL_ERROR = 0x00DD5555;

    // Map local theme index to kernel theme ID and apply system-wide
    // Kernel theme IDs: 0=Default(Retro), 1=Dark, 2=Light, 4=Classic, 5=Ocean, 9=Modern Dark
    static const int kernel_theme_map[] = { 1, 2, 4, 5, 9, 6, 7, 11 };
    int kernel_id = kernel_theme_map[theme_id];
    set_theme(kernel_id);

    // Push the active palette + renderer family into the shared style engine
    // so all gui_* primitives render in this theme. Classic theme (id 2) uses
    // the beveled CDE renderer; everything else uses the modern renderer.
    gui_set_style(theme_id == 2 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
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
}

// Theme picker uses a scrollable dropdown (scales to many themes), not buttons.
// Names match apply_theme's id order.
/* (#260) Theme labels: the old "Dark" is now "Midnight"; the old "Nord" is now
 * "Dark". Order/indices are unchanged so persisted prefs (saved by index) still
 * resolve to the same theme. */
static const char *const g_theme_names[] = {"Midnight", "Light", "Classic", "Ocean", "Dark", "Sunset", "Forest", "Slate Dark"};
#define NUM_THEMES 8
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
    win_draw_text(window_handle, x, y, title, COL_TEXT_PRIMARY);
    win_draw_rect(window_handle, x, y + 22, CONTENT_WIDTH - 2 * PADDING, 1, COL_SEPARATOR);
}

static void draw_subsection(int x, int y, const char *title) {
    win_draw_text(window_handle, x, y, title, COL_ACCENT);
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
static const char *const SS_OPTS[]        = {"Off", "Starfield", "Flux", "Lines", "Bubbles", "Matrix", "Plasma", "GL Cube", "GL Matrix"};
static const char *const CURSOR_OPTS[]    = {"Light", "Dark", "Glow"};  // (#116) maps to compositor curstyle 0/1/2
static const int SS_KERNEL_MAP[]          = {0, 2, 6, 3, 4, 5, 7, 8, 9};   // idx -> kernel screensaver id (#319 8=GL Cube 9=GL Matrix, reconciled #336)
static const int CURSOR_KERNEL_MAP[]      = {0, 2, 1, 0};      // idx -> kernel cursor theme
static const char *const DATE_FMT_OPTS[]  = {"YYYY-MM-DD", "MM/DD/YYYY", "DD/MM/YYYY"};
// #387 Dock / taskbar layouts. Order matches the compositor's DOCK_* enum.
// Labels are original, non-infringing names (persisted by numeric index in
// /DOCKSTYL.CFG, so relabeling never changes the applied layout):
//   1 Lumina      = glass dock + translucent top menu bar (was "macOS Sonoma")
//   2 Classic UNIX = beveled CDE/Motif-style panel (was "CDE / UNIX")
//   3 Retro Bench = title-bar-at-top workbench layout (was "Amiga Workbench")
static const char *const DOCK_OPTS[]      = {"Default (MayteraOS)", "Lumina", "Classic UNIX", "Retro Bench"};

// --- Real dropdown widget: opens a scrollable list, current item highlighted ---
#define DD_ROW      26
#define DD_VISIBLE  9
static int   g_dd_open = 0, g_dd_x, g_dd_y, g_dd_w, g_dd_count, g_dd_scroll;
static int  *g_dd_sel = 0;
static const char *const *g_dd_items = 0;
static void (*g_dd_on_change)(void) = 0;

// Small refined downward chevron (filled triangle) centred at (cx, cy-top).
static void draw_chevron_down(int cx, int cy, uint32_t col) {
    for (int r = 0; r < 4; r++) {
        int w = 7 - r * 2; if (w < 1) w = 1;
        win_draw_rect(window_handle, cx - w / 2, cy + r, w, 1, col);
    }
}

static void draw_dropdown_n(int x, int y, int width, const char *value, bool active, int count) {
    focus_add(x, y, width, 28, 0);
    if (gui_active_style().base == GUI_STYLE_MODERN) {
        gui_fill_rounded(window_handle, x, y, width, 28, GUI_RADIUS, active ? COL_ACCENT : COL_INPUT_BORDER);
        gui_fill_rounded(window_handle, x+1, y+1, width-2, 26, GUI_RADIUS-1, COL_INPUT_BG);
    } else {
        uint32_t bg = active ? COL_PANEL_ACTIVE : COL_INPUT_BG;
        win_draw_rect(window_handle, x, y, width, 28, bg);
        gui_draw_rect_outline(window_handle, x, y, width, 28, active ? COL_ACCENT : COL_INPUT_BORDER);
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

static void dropdown_open(int x, int y, int w, const char *const *items,
                          int count, int *sel, void (*on_change)(void)) {
    g_dd_open = 1; g_dd_x = x; g_dd_y = y; g_dd_w = w;
    g_dd_items = items; g_dd_count = count; g_dd_sel = sel; g_dd_on_change = on_change;
    int vis = count < DD_VISIBLE ? count : DD_VISIBLE;
    g_dd_scroll = *sel - vis / 2;                       // centre the current item
    if (g_dd_scroll > count - vis) g_dd_scroll = count - vis;
    if (g_dd_scroll < 0) g_dd_scroll = 0;
}

static void dropdown_geom(int *by, int *bh, int *vis) {
    int v = g_dd_count < DD_VISIBLE ? g_dd_count : DD_VISIBLE;
    int h = v * DD_ROW + 4;
    int y = g_dd_y + 28;
    if (y + h > WIN_HEIGHT - 4 && g_dd_y - h >= 0) y = g_dd_y - h;  // flip above if needed
    *by = y; *bh = h; *vis = v;
}

static void dropdown_render(void) {
    if (!g_dd_open) return;
    int by, bh, vis; dropdown_geom(&by, &bh, &vis);
    int bx = g_dd_x, bw = g_dd_w;
    win_draw_rect(window_handle, bx, by, bw, bh, COL_INPUT_BG);
    gui_draw_rect_outline(window_handle, bx, by, bw, bh, COL_ACCENT);
    for (int r = 0; r < vis; r++) {
        int idx = g_dd_scroll + r;
        if (idx < 0 || idx >= g_dd_count) continue;
        int ry = by + 2 + r * DD_ROW;
        if (idx == *g_dd_sel)
            win_draw_rect(window_handle, bx + 2, ry, bw - 4, DD_ROW, COL_ACCENT);
        win_draw_text(window_handle, bx + 10, ry + 6, g_dd_items[idx], COL_TEXT_PRIMARY);
    }
    if (g_dd_count > vis) {                              // scrollbar
        int track_h = bh - 4;
        int thumb_h = track_h * vis / g_dd_count; if (thumb_h < 12) thumb_h = 12;
        int denom = g_dd_count - vis; if (denom < 1) denom = 1;
        int thumb_y = by + 2 + (track_h - thumb_h) * g_dd_scroll / denom;
        win_draw_rect(window_handle, bx + bw - 7, by + 2, 5, track_h, COL_SLIDER_TRACK);
        win_draw_rect(window_handle, bx + bw - 7, thumb_y, 5, thumb_h, COL_TEXT_SECONDARY);
    }
}

// Returns 1 if the click was consumed by an open dropdown.
static void dropdown_click(int mx, int my) {
    int by, bh, vis; dropdown_geom(&by, &bh, &vis);
    int bx = g_dd_x, bw = g_dd_w;
    if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
        int r = (my - (by + 2)) / DD_ROW;
        int idx = g_dd_scroll + r;
        if (idx >= 0 && idx < g_dd_count) {
            *g_dd_sel = idx;
            if (g_dd_on_change) g_dd_on_change();
        }
    }
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
static char     g_audio_name[48] = "";
static int      g_audio_present = 0;
static int      g_bt_present = 0;
static int      g_wifi_present = 0;

// Append s onto the NUL-terminated dst, never exceeding cap-1 chars.
static void hw_append(char *dst, int cap, const char *s) {
    int i = 0; while (dst[i]) i++;
    for (int j = 0; s[j] && i < cap - 1; j++) dst[i++] = s[j];
    dst[i] = '\0';
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
        if (ul[i].dev_class == 0xE0 && ul[i].subclass == 0x01 && ul[i].protocol == 0x01)
            g_bt_present = 1;
        if (ul[i].vendor_id == 0x0A12) g_bt_present = 1;   // Cambridge Silicon Radio
        // USB Wi-Fi dongle: base class 0xE0 subclass 0x01 but not the BT protocol,
        // or a wireless-controller class device.
        if (ul[i].dev_class == 0xE0 && ul[i].subclass == 0x01 && ul[i].protocol != 0x01)
            g_wifi_present = 1;
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
    sys_unlink(USEREMAIL_CFG);
    fd = sys_open(USEREMAIL_CFG, 0x41);   // O_WRONLY|O_CREAT
    if (fd < 0) return;
    sys_write(fd, out, (unsigned long)on);
    sys_close(fd);
}

// Load the real account list from the kernel so the Users panel reflects the
// actual /CONFIG/PASSWD database. Add/Remove operate on the live accounts, so
// the list is re-read after each change. Falls back to the built-in defaults
// if the syscall is unavailable (returns <= 0).
static void users_refresh(void) {
    user_info_t ui[8];
    int n = sys_list_users(ui, 8);
    if (n <= 0) return;
    static const uint32_t avatar_palette[8] = {
        0x00569CD6, 0x0066BB66, 0x00CC8844, 0x00AA66CC,
        0x00CC6666, 0x0044AAAA, 0x00888888, 0x00BBAA44
    };
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) {
        copy_str(users[i].username, ui[i].username, 32);
        copy_str(users[i].fullname,
                 ui[i].display_name[0] ? ui[i].display_name : ui[i].username, 64);
        useremail_get(users[i].username, users[i].email, 64);   // real stored email, "" if unset
        users[i].role = (ui[i].uid == 0) ? 0 : 1;   // uid 0 = administrator
        users[i].password_set = true;
        users[i].avatar_color = avatar_palette[i & 7];
    }
    user_count = n;
    if (current_user_idx >= user_count) current_user_idx = 0;
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

static void update_timezone_offset(void) {
    // Timezone strings look like "UTC+05:30", "UTC-08:00", "UTC"
    const char *s = timezones[timezone_idx];
    int sign = 1, h = 0, m = 0, i = 0;
    while (s[i] && s[i] != '+' && s[i] != '-') i++;
    if (!s[i]) { timezone_offset_minutes = 0; return; }
    sign = (s[i] == '+') ? 1 : -1;
    i++;
    if (s[i] >= '0' && s[i] <= '9' && s[i+1] >= '0' && s[i+1] <= '9') {
        h = (s[i] - '0') * 10 + (s[i+1] - '0');
        if (s[i+2] == ':' && s[i+3] >= '0' && s[i+3] <= '9' && s[i+4] >= '0' && s[i+4] <= '9') {
            m = (s[i+3] - '0') * 10 + (s[i+4] - '0');
        }
    }
    timezone_offset_minutes = sign * (h * 60 + m);
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
    "RSS"        // External Services (#414). MUST exist: this array is indexed by
                 // PANEL_COUNT and a missing entry left panel_mico[PANEL_EXTSVC]
                 // NULL, which made draw_sidebar() -> draw_mico(NULL) ->
                 // mico_get(NULL) dereference address 0 and crash Settings on
                 // every launch (the window was created then the process killed,
                 // so it "never appeared"). Keep this list length == PANEL_COUNT.
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
    if (!draw_mico("MENU", 13, 9, 22, COL_ACCENT, COL_CARD_BG))
        win_draw_text(window_handle, 15, 12, "[*]", COL_ACCENT);
    win_draw_text(window_handle, 45, 12, "Settings", COL_TEXT_PRIMARY);
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
        uint32_t text_color = (i == current_panel) ? COL_ACCENT : COL_TEXT_PRIMARY;

        // Icon: real MICO glyph tinted to the row text color; fall back to the
        // letter chip if the icon file is missing so nothing breaks.
        if (!draw_mico(panel_mico[i], 14, y + 7, 20, text_color, bg)) {
            win_draw_text(window_handle, 15, y + 9, panel_icons[i], COL_TEXT_SECONDARY);
        }

        win_draw_text(window_handle, 48, y + 9, panel_names[i], text_color);
    }

    // Scrollbar: themed, and drawn only when the list actually overflows, so on
    // a screen tall enough to show all panels no gutter is spent.
    gui_scroll_draw(window_handle, &g_side_scroll);

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
                            vline, COL_TEXT_DISABLED);
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
static void settings_save(void) {
    char buf[512]; char *p = buf;
    p = sv_putint(p, 't', timezone_idx);
    p = sv_putint(p, 'h', use_24hour ? 1 : 0);
    p = sv_putint(p, 'd', date_format);
    p = sv_putint(p, 'a', accent_color_idx);
    p = sv_putint(p, 'c', cursor_theme);
    p = sv_putint(p, 'p', pointer_speed);
    p = sv_putint(p, 'k', double_click_speed);
    p = sv_putint(p, 's', screensaver_idx);
    p = sv_putint(p, 'z', screensaver_delay_min);
    // (#382 pass2) Persist the Keyboard/Mouse/Display preference sliders so they
    // survive relaunch (they were previously per-launch no-ops). Uppercase keys
    // to avoid colliding with the lowercase keys above.
    p = sv_putint(p, 'R', key_repeat_rate);
    p = sv_putint(p, 'E', key_repeat_delay);
    p = sv_putint(p, 'Y', keyboard_layout);
    p = sv_putint(p, 'W', scroll_speed);
    p = sv_putint(p, 'F', natural_scrolling ? 1 : 0);
    p = sv_putint(p, 'J', scroll_inertia ? 1 : 0);
    p = sv_putint(p, 'Q', left_handed ? 1 : 0);
    p = sv_putint(p, 'X', pointer_trails ? 1 : 0);
    p = sv_putint(p, 'S', scaling_factor);
    p = sv_putint(p, 'C', color_temp);
    p = sv_putint(p, 'U', gamma_r);
    p = sv_putint(p, 'V', gamma_g);
    p = sv_putint(p, 'B', gamma_b);
    sys_unlink("SETTINGS.CFG");
    int fd = sys_open("SETTINGS.CFG", 0x41);   // O_WRONLY|O_CREAT
    if (fd < 0) return;
    sys_write(fd, buf, (unsigned long)(p - buf));
    sys_close(fd);
}
static void settings_load(void) {
    int fd = sys_open("SETTINGS.CFG", 0);
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
            switch (key) {
                case 't': timezone_idx = val; break;
                case 'h': use_24hour = val ? true : false; break;
                case 'd': date_format = val; break;
                case 'a': accent_color_idx = val; break;
                case 'c': cursor_theme = val; break;
                case 'p': pointer_speed = val; break;
                case 'k': double_click_speed = val; break;
                case 's': screensaver_idx = val; break;
                case 'z': screensaver_delay_min = val; break;
                case 'R': key_repeat_rate = val; break;
                case 'E': key_repeat_delay = val; break;
                case 'Y': keyboard_layout = val; break;
                case 'W': scroll_speed = val; break;
                case 'F': natural_scrolling = val ? true : false; break;
                case 'J': scroll_inertia = val ? true : false; break;
                case 'Q': left_handed = val ? true : false; break;
                case 'X': pointer_trails = val ? true : false; break;
                case 'S': scaling_factor = val; break;
                case 'C': color_temp = val; break;
                case 'U': gamma_r = val; break;
                case 'V': gamma_g = val; break;
                case 'B': gamma_b = val; break;
            }
        }
        while (b[i] && b[i] != '\n') i++;
        if (b[i] == '\n') i++;
    }
}
static void settings_autosave(void) {
    static int last = -1;
    int h = timezone_idx*7 + (use_24hour?1:0)*13 + date_format*17 + accent_color_idx*23
          + cursor_theme*29 + pointer_speed*31 + double_click_speed*37
          + screensaver_idx*41 + screensaver_delay_min*43
          + key_repeat_rate*47 + key_repeat_delay*53 + keyboard_layout*59
          + scroll_speed*61 + (natural_scrolling?1:0)*67 + (scroll_inertia?1:0)*71
          + (left_handed?1:0)*73 + (pointer_trails?1:0)*79 + scaling_factor*83
          + color_temp*89 + gamma_r*97 + gamma_g*101 + gamma_b*103;
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
    sys_unlink("/CONFIG/ALERTS.CFG");
    int fd = sys_open("/CONFIG/ALERTS.CFG", 0x41);
    if (fd < 0) return;
    sys_write(fd, buf, (unsigned long)(p - buf));
    sys_close(fd);
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
    sys_unlink("/CONFIG/PRIVACY.CFG");
    int fd = sys_open("/CONFIG/PRIVACY.CFG", 0x41);   // O_WRONLY|O_CREAT
    if (fd < 0) return;
    sys_write(fd, buf, (unsigned long)(p - buf));
    sys_close(fd);
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
    sys_write(fd, hd, 8);
    sys_write(fd, (uint8_t *)s_wp_thumb, WPT_W * WPT_H * 4);
    sys_close(fd);
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
    draw_subsection(WP_DD_X, WP_DD_Y, "Wallpaper");
    int dy = WP_DD_Y + 25;
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
    int y = PADDING;
    char buf[32];

    draw_wallpaper_picker();           // dropdown + single preview, right column

    draw_section_header(x, y, "Appearance");
    y += 40;

    // Theme selection with preview
    draw_subsection(x, y, "Theme");
    y += 25;

    // Scrollable dropdown (scales to many themes), not a row of buttons.
    draw_dropdown_n(x, y, 220, g_theme_names[current_theme],
                  g_dd_open && g_dd_sel == &current_theme, NUM_THEMES);
    y += 50;

    // Accent color picker
    draw_subsection(x, y, "Accent Color");
    y += 25;

    for (int i = 0; i < NUM_ACCENT_COLORS; i++) {
        draw_color_box(x + i * 42, y, ACCENT_COLORS[i], i == accent_color_idx);
    }
    y += 55;

    // Font settings
    draw_subsection(x, y, "Text");
    y += 25;

    draw_label(x, y, "Font Size");
    draw_dropdown_n(x + 120, y - 3, 160, FONT_SIZE_OPTS[font_size],
                  g_dd_open && g_dd_sel == &font_size, 4);
    y += 40;

    draw_label(x, y, "Icon Size");
    draw_dropdown_n(x + 120, y - 3, 160, ICON_SIZE_OPTS[icon_size],
                  g_dd_open && g_dd_sel == &icon_size, 3);
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
    draw_dropdown_n(x, y, 160, SS_OPTS[screensaver_idx],
                  g_dd_open && g_dd_sel == &screensaver_idx, 9);
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

    // Cursor theme
    draw_subsection(x, y, "Cursor");
    y += 25;
    if (cursor_theme < 0 || cursor_theme > 2) cursor_theme = 0;  // (#116) 3 styles now
    draw_dropdown_n(x, y, 160, CURSOR_OPTS[cursor_theme],
                  g_dd_open && g_dd_sel == &cursor_theme, 3);
    y += 45;

    // #387 Dock style (taskbar / dock layout) picker.
    draw_subsection(x, y, "Dock Style");
    y += 25;
    if (dock_style < 0 || dock_style > 3) dock_style = 0;
    draw_dropdown_n(x, y, 220, DOCK_OPTS[dock_style],
                  g_dd_open && g_dd_sel == &dock_style, 4);
    y += 45;

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
}

// =============================================================================
// Panel: Display
// =============================================================================

static void draw_display_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING;
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
            gui_itoa((int)fi.width, a, sizeof(a));
            gui_itoa((int)fi.height, b, sizeof(b));
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

    // Scaling
    draw_label(x, y + 4, "Scale");
    gui_itoa(scaling_factor, buf, sizeof(buf));
    int len = my_strlen(buf);
    buf[len++] = '%'; buf[len] = 0;
    draw_slider(x + 120, y, 200, scaling_factor - 100, 100, COL_SLIDER_FILL);
    win_draw_text(window_handle, x + 335, y, buf, COL_TEXT_SECONDARY);
    y += 50;

    // Brightness
    draw_subsection(x, y, "Brightness");
    y += 25;

    gui_itoa(brightness, buf, sizeof(buf));
    len = my_strlen(buf);
    buf[len++] = '%'; buf[len] = 0;
    draw_slider(x, y, 350, brightness, 100, COL_WARNING);
    win_draw_text(window_handle, x + 370, y, buf, COL_TEXT_SECONDARY);
    y += 45;

    // Night light
    draw_subsection(x, y, "Night Light");
    y += 25;

    draw_toggle_labeled(x, y, 300, "Enable Night Light", night_light);
    y += 35;

    if (night_light) {
        draw_label(x + 20, y, "Strength");
        gui_itoa(night_light_strength, buf, sizeof(buf));
        len = my_strlen(buf);
        buf[len++] = '%'; buf[len] = 0;
        draw_slider(x + 120, y, 200, night_light_strength, 100, 0x00FF9933);
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

    // Color calibration
    draw_subsection(x, y, "Color Calibration");
    y += 25;

    draw_label(x, y, "Color Temp");
    gui_itoa(color_temp, buf, sizeof(buf));
    len = my_strlen(buf);
    buf[len++] = 'K'; buf[len] = 0;
    win_draw_text(window_handle, x + 350, y, buf, COL_TEXT_SECONDARY);
    draw_slider(x + 100, y, 240, color_temp - 4000, 5000, COL_SLIDER_FILL);
    y += 35;

    // Gamma controls
    draw_label(x, y, "Gamma R");
    draw_slider(x + 80, y, 100, gamma_r, 150, 0x00FF4444);
    draw_label(x + 200, y, "G");
    draw_slider(x + 220, y, 100, gamma_g, 150, 0x0044FF44);
    draw_label(x + 340, y, "B");
    draw_slider(x + 360, y, 100, gamma_b, 150, 0x004444FF);
    y += 35;
    // (#382 pass2) Honest: Brightness and Night Light ARE applied live (SYS_SET_
    // DISPLAY_FX). Scale / Color Temp / Gamma have no compositor or GPU LUT hook
    // in this build, so they are shown for reference and are not applied.
    draw_hint_ic(x, y, "CMINUS", 0x00A0A0A8,
                 "Scale, Color Temp and Gamma are not applied in this build (no LUT hook).");
    y += 20;

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
    int y = PADDING;
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
    draw_hint_ic(x, y, "CMINUS", 0x00A0A0A8,
                 "Audio input (microphone capture) is not supported by this build.");
    y += 40;                                  // 240

    // ---- Equalizer (no DSP) ----
    draw_subsection(x, y, "Equalizer");
    y += 25;                                  // 265
    draw_hint_ic(x, y, "CMINUS", 0x00A0A0A8,
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
        draw_hint_ic(x, y, "CMINUS", 0x00A0A0A8, "No audio output device present.");
    else if (sound_test_status == 3)
        draw_hint(x, y, "Playing test sound...");
}

// =============================================================================
// Panel: Network
// =============================================================================

static void draw_network_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING;

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
    draw_label_value(x + 280, y + 60, "DNS 1:", dns_primary, 80);
    draw_label_value(x + 280, y + 80, "DNS 2:", dns_secondary, 80);

    y += 125;

    // DHCP toggle
    draw_toggle_labeled(x, y, 300, "Obtain IP automatically (DHCP)", dhcp_enabled);
    y += 40;

    if (!dhcp_enabled) {
        draw_button_small(x + 20, y, 160, "Configure IP...", false);
        y += 36;
    }

    // VPN section. There is no VPN client / tunnel stack in this build, so this
    // is honestly marked unavailable rather than a toggle with a fake connected
    // state. Kept the same vertical footprint so the Firewall layout is stable.
    draw_subsection(x, y, "VPN");
    y += 25;

    draw_hint_ic(x, y, "CMINUS", 0x00A0A0A8,
                 "VPN is not available (no VPN client in this build).");
    y += 30;

    // Firewall
    draw_subsection(x, y, "Firewall");
    y += 25;

    draw_toggle_labeled(x, y, 300, "Enable Firewall", firewall_enabled);
    y += 35;

    if (firewall_enabled) {
        const char *pol[] = {"Allow", "Deny"};
        draw_label(x + 20, y + 6, "Default Inbound");
        draw_option_buttons(x + 170, y, pol, 2, fw_pol_in);
        y += 34;
        draw_label(x + 20, y + 6, "Default Outbound");
        draw_option_buttons(x + 170, y, pol, 2, fw_pol_out);
        y += 34;

        draw_label(x + 20, y, "Rules");
        draw_hint(x + 70, y + 3, "click ALLOW / IN / TCP chips to cycle, X to remove");
        y += 22;
        for (int i = 0; i < fw_rule_count; i++) {
            fw_rule_t *r = &fw_rules[i];
            int rx = x + 20;
            uint32_t acol = (r->action == 0) ? 0x00207A20 : 0x008A2020;
            gui_fill_rounded(window_handle, rx, y, 56, 20, 5, acol);
            gui_text_ttf_centered(window_handle, rx, y, 56, 20, r->action == 0 ? "ALLOW" : "DENY", gui_ink_on(acol), 11);
            gui_fill_rounded(window_handle, rx + 64, y, 44, 20, 5, COL_BUTTON_BG);
            gui_text_ttf_centered(window_handle, rx + 64, y, 44, 20, r->dir == 0 ? "IN" : "OUT", COL_TEXT_PRIMARY, 11);
            gui_fill_rounded(window_handle, rx + 116, y, 44, 20, 5, COL_BUTTON_BG);
            gui_text_ttf_centered(window_handle, rx + 116, y, 44, 20, r->proto == 0 ? "TCP" : "UDP", COL_TEXT_PRIMARY, 11);
            char pbuf[8]; gui_itoa(r->port, pbuf, sizeof(pbuf));
            gui_fill_rounded(window_handle, rx + 168, y, 60, 20, 5, COL_INPUT_BG);
            gui_text_ttf_centered(window_handle, rx + 168, y, 60, 20, pbuf, COL_TEXT_PRIMARY, 11);
            gui_fill_rounded(window_handle, rx + 240, y, 24, 20, 5, 0x008A2020);
            gui_text_ttf_centered(window_handle, rx + 240, y, 24, 20, "X", gui_ink_on(0x008A2020), 11);
            y += 26;
        }
        if (fw_rule_count < MAX_FW_RULES)
            draw_button_small(x + 20, y, 120, "+ Add Rule", true);
    }
}


// Panel: Keyboard
// =============================================================================

static void draw_keyboard_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING;
    char buf[32];

    draw_section_header(x, y, "Keyboard");
    y += 40;

    // Layout
    draw_subsection(x, y, "Layout");
    y += 25;

    draw_label(x, y + 4, "Keyboard Layout");
    draw_dropdown(x + 140, y, 200, keyboard_layouts[keyboard_layout], false);
    y += 45;

    // Typing settings
    draw_subsection(x, y, "Typing");
    y += 25;

    draw_label(x, y, "Repeat Rate");
    gui_itoa(key_repeat_rate, buf, sizeof(buf));
    int len = my_strlen(buf);
    buf[len++] = '/'; buf[len++] = 's'; buf[len] = 0;
    draw_slider(x + 140, y, 200, key_repeat_rate, 50, COL_SLIDER_FILL);
    win_draw_text(window_handle, x + 355, y, buf, COL_TEXT_SECONDARY);
    y += 35;

    draw_label(x, y, "Repeat Delay");
    gui_itoa(key_repeat_delay, buf, sizeof(buf));
    len = my_strlen(buf);
    buf[len++] = 'm'; buf[len++] = 's'; buf[len] = 0;
    draw_slider(x + 140, y, 200, key_repeat_delay, 500, COL_SLIDER_FILL);
    win_draw_text(window_handle, x + 355, y, buf, COL_TEXT_SECONDARY);
    y += 50;

    // Lock indicators
    draw_subsection(x, y, "Lock Keys");
    y += 25;

    draw_checkbox(x, y, "Num Lock", num_lock);
    draw_checkbox(x + 150, y, "Caps Lock", caps_lock);
    draw_checkbox(x + 300, y, "Scroll Lock", scroll_lock);
    y += 45;

    // Keyboard shortcuts
    draw_subsection(x, y, "Shortcuts");
    y += 25;

    // Show first 6 shortcuts
    for (int i = 0; i < 6 && i < 8; i++) {
        int row = i / 2;
        int col = i % 2;
        int sx = x + col * 250;
        int sy = y + row * 25;

        draw_label_value(sx, sy, shortcuts[i].action, shortcuts[i].keys, 120);
    }
    y += 85;

    draw_button(x, y, 150, "Edit Shortcuts", false, false);
    y += 50;

    // Test area
    draw_subsection(x, y, "Test Area");
    y += 25;

    win_draw_rect(window_handle, x, y, CONTENT_WIDTH - 2 * PADDING, 40, COL_INPUT_BG);
    gui_draw_rect_outline(window_handle, x, y, CONTENT_WIDTH - 2 * PADDING, 40, COL_INPUT_BORDER);
    win_draw_text(window_handle, x + 10, y + 12, "Type here to test keyboard settings...", COL_TEXT_DISABLED);
    y += 50;
    // (#382 pass2) Honest: these persist to SETTINGS.CFG but the current build
    // does not apply repeat rate/delay or layout live to the keyboard driver.
    draw_hint_ic(x, y, "CMINUS", 0x00A0A0A8,
                 "Repeat rate/delay and layout are saved preferences; not yet applied live.");
}

// =============================================================================
// Panel: Mouse/Touchpad
// =============================================================================

static void draw_mouse_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING;
    char buf[32];

    draw_section_header(x, y, "Mouse & Touchpad");
    y += 40;

    // Pointer speed
    draw_subsection(x, y, "Pointer");
    y += 25;

    draw_label(x, y, "Mouse Sensitivity");
    gui_itoa(pointer_speed, buf, sizeof(buf));
    draw_slider(x + 140, y, 250, pointer_speed, 100, COL_SLIDER_FILL);
    win_draw_text(window_handle, x + 405, y, buf, COL_TEXT_SECONDARY);
    y += 35;

    draw_label(x, y, "Double-click Speed");
    gui_itoa(double_click_speed, buf, sizeof(buf));
    draw_slider(x + 160, y, 230, double_click_speed, 100, COL_SLIDER_FILL);
    win_draw_text(window_handle, x + 405, y, buf, COL_TEXT_SECONDARY);
    y += 45;

    // Pointer appearance
    draw_label(x, y, "Pointer Size");
    const char* ptr_sizes[] = {"Small", "Normal", "Large", "XL"};
    draw_option_buttons(x + 140, y - 3, ptr_sizes, 4, pointer_size);
    y += 45;

    draw_toggle_labeled(x, y, 300, "Pointer Trails", pointer_trails);
    y += 35;

    if (pointer_trails) {
        draw_label(x + 20, y, "Trail Length");
        draw_slider(x + 140, y, 150, pointer_trail_length, 10, COL_SLIDER_FILL);
        y += 40;
    }

    // Scrolling
    draw_subsection(x, y, "Scrolling");
    y += 25;

    draw_label(x, y, "Scroll Speed");
    gui_itoa(scroll_speed, buf, sizeof(buf));
    draw_slider(x + 140, y, 200, scroll_speed, 100, COL_SLIDER_FILL);
    win_draw_text(window_handle, x + 355, y, buf, COL_TEXT_SECONDARY);
    y += 35;

    draw_toggle_labeled(x, y, 300, "Natural Scrolling", natural_scrolling);
    y += 10;
    draw_hint(x, y + 20, "Content moves in the direction of your fingers");
    y += 45;

    draw_toggle_labeled(x, y, 300, "Scroll Inertia", scroll_inertia);
    y += 45;

    // Button configuration
    draw_subsection(x, y, "Buttons");
    y += 25;

    draw_toggle_labeled(x, y, 300, "Left-handed Mode", left_handed);
    y += 10;
    draw_hint(x, y + 20, "Swap primary and secondary mouse buttons");
    y += 45;
    // Honest: Mouse Sensitivity IS applied live (set_mouse_speed) and persisted
    // to UIPROFIL by the compositor. The other controls persist to SETTINGS.CFG
    // but have no live driver effect yet.
    draw_hint_ic(x, y, "CMINUS", 0x00A0A0A8,
                 "Saved preferences; Mouse Sensitivity is applied live and persists across reboots.");
}

// =============================================================================
// Panel: Date & Time
// =============================================================================

static void draw_datetime_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING;

    draw_section_header(x, y, "Date & Time");
    y += 40;

    // Current time display (large)
    draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 100);

    // Read real time and date from RTC
    int rtc_hour = 0, rtc_min = 0, rtc_sec = 0;
    int rtc_day = 1, rtc_month = 1, rtc_year = 2026;
    get_rtc_time(&rtc_hour, &rtc_min, &rtc_sec);
    get_rtc_date(&rtc_day, &rtc_month, &rtc_year);

    // Apply timezone offset for display
    int total_m = rtc_hour * 60 + rtc_min + timezone_offset_minutes;
    int display_h = ((total_m / 60) % 24 + 24) % 24;
    int display_m = ((total_m % 60) + 60) % 60;

    // Format HH:MM:SS
    char time_str[12];
    time_str[0] = '0' + display_h / 10; time_str[1] = '0' + display_h % 10;
    time_str[2] = ':';
    time_str[3] = '0' + display_m / 10; time_str[4] = '0' + display_m % 10;
    time_str[5] = ':';
    time_str[6] = '0' + rtc_sec / 10;   time_str[7] = '0' + rtc_sec % 10;
    time_str[8] = 0;

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
    hm_str[0] = '0' + display_h / 10; hm_str[1] = '0' + display_h % 10;
    hm_str[2] = ':';
    hm_str[3] = '0' + display_m / 10; hm_str[4] = '0' + display_m % 10;
    hm_str[5] = 0;
    win_draw_rect(window_handle, cx - 35, cy - 35, 70, 70, COL_CONTENT_BG);
    gui_draw_rect_outline(window_handle, cx - 35, cy - 35, 70, 70, COL_TEXT_SECONDARY);
    win_draw_text(window_handle, cx - 16, cy - 8, hm_str, COL_TEXT_PRIMARY);

    y += 115;

    // Auto time toggle
    draw_toggle_labeled(x, y, 300, "Set time automatically", auto_time);
    y += 10;
    draw_hint(x, y + 20, "Synchronize with network time servers");
    y += 30;
    // NTP status feedback
    if (ntp_status == 1)
        draw_hint_ic(x + 20, y + 20, "CCHECK", 0x0044B860, "Time synchronized successfully.");
    else if (ntp_status == -1)
        win_draw_text(window_handle, x + 20, y + 20, "NTP sync failed (no network?).", 0x00CC3333);
    y += 20;

    // Time format
    draw_toggle_labeled(x, y, 300, "Use 24-hour format", use_24hour);
    y += 45;

    // Timezone
    draw_subsection(x, y, "Time Zone");
    y += 25;

    draw_dropdown(x, y, 350, timezones[timezone_idx], false);
    y += 50;

    // Date format
    draw_subsection(x, y, "Date Format");
    y += 25;

    draw_dropdown(x, y, 160, DATE_FMT_OPTS[date_format],
                  g_dd_open && g_dd_sel == &date_format);
    y += 45;

    // First day of week
    draw_label(x, y, "Week starts on");
    const char* week_days[] = {"Sunday", "Monday"};
    draw_option_buttons(x + 140, y - 3, week_days, 2, first_day_of_week);
    y += 50;

    // Manual time setting button
    if (!auto_time) {
        draw_button(x, y, 160, "Set Date & Time", true, false);
    }
}

// =============================================================================
// Panel: Users & Accounts
// =============================================================================

static void draw_users_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING;

    draw_section_header(x, y, "Users & Accounts");
    y += 40;

    // Current user card
    draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 100);

    // Avatar
    win_draw_rect(window_handle, x + 15, y + 20, 60, 60, users[current_user_idx].avatar_color);
    char initial[2] = {users[current_user_idx].fullname[0], 0};
    win_draw_text(window_handle, x + 37, y + 40, initial, COL_TEXT_PRIMARY);

    // User info
    win_draw_text(window_handle, x + 90, y + 20, users[current_user_idx].fullname, COL_TEXT_PRIMARY);
    if (users[current_user_idx].email[0])
        win_draw_text(window_handle, x + 90, y + 42, users[current_user_idx].email, COL_TEXT_SECONDARY);
    else
        win_draw_text(window_handle, x + 90, y + 42, "(no email set)", COL_TEXT_DISABLED);

    const char* roles[] = {"Administrator", "Standard User", "Guest"};
    win_draw_text(window_handle, x + 90, y + 62, roles[users[current_user_idx].role], COL_TEXT_DISABLED);

    draw_button_small(x + 350, y + 35, 100, "Edit Profile", false);

    y += 115;

    // Account settings
    draw_subsection(x, y, "Account Settings");
    y += 25;

    draw_toggle_labeled(x, y, 300, "Auto-login on startup", auto_login);
    y += 10;
    draw_hint(x, y + 20, "Skip the login screen at boot");
    y += 45;

    draw_button(x, y, 160, "Change Password", false, false);
    y += 50;

    // Other users
    draw_subsection(x, y, "Other Users");
    y += 25;

    for (int i = 0; i < user_count; i++) {
        if (i == current_user_idx) continue;
        if (users[i].username[0] == 0) continue;

        draw_card(x, y, CONTENT_WIDTH - 2 * PADDING, 50);

        // Avatar
        win_draw_rect(window_handle, x + 15, y + 10, 30, 30, users[i].avatar_color);
        char init[2] = {users[i].fullname[0], 0};
        win_draw_text(window_handle, x + 25, y + 17, init, COL_TEXT_PRIMARY);

        win_draw_text(window_handle, x + 60, y + 10, users[i].fullname, COL_TEXT_PRIMARY);
        win_draw_text(window_handle, x + 60, y + 28, roles[users[i].role], COL_TEXT_SECONDARY);

        draw_button_small(x + 400, y + 13, 80, "Remove", false);

        y += 60;
    }

    y += 10;
    draw_button(x, y, 120, "Add User", true, false);

    y += 45;
    draw_toggle_labeled(x, y, 300, "Enable Guest Account", guest_enabled);
}

// =============================================================================
// Panel: Privacy & Security
// =============================================================================

static void draw_privacy_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING;
    char buf[32];

    draw_section_header(x, y, "Privacy & Security");
    y += 40;

    // Screen lock
    draw_subsection(x, y, "Screen Lock");
    y += 25;

    draw_toggle_labeled(x, y, 300, "Enable Screen Lock", screen_lock_enabled);
    y += 35;

    if (screen_lock_enabled) {
        draw_label(x + 20, y, "Lock after");
        const char* timeouts[] = {"Never", "1 min", "2 min", "5 min", "10 min", "15 min", "30 min"};
        int timeout_idx = 0;
        if (lock_timeout == 1) timeout_idx = 1;
        else if (lock_timeout == 2) timeout_idx = 2;
        else if (lock_timeout == 5) timeout_idx = 3;
        else if (lock_timeout == 10) timeout_idx = 4;
        else if (lock_timeout == 15) timeout_idx = 5;
        else if (lock_timeout == 30) timeout_idx = 6;
        draw_dropdown(x + 120, y, 120, timeouts[timeout_idx], false);
        y += 40;

        draw_toggle_labeled(x + 20, y, 280, "Require password on wake", require_password_wake);
        y += 40;
    }

    // Privacy. These persist to /CONFIG/PRIVACY.CFG (real stored settings); the
    // hints are honest about the fact that there is no telemetry/location backend.
    draw_subsection(x, y, "Privacy");
    y += 25;

    draw_toggle_labeled(x, y, 300, "Location Services", location_services);
    y += 10;
    draw_hint(x, y + 20, "Preference only: no location provider exists in this build.");
    y += 45;

    draw_toggle_labeled(x, y, 300, "Send Diagnostics", diagnostics_enabled);
    y += 10;
    draw_hint(x, y + 20, "Preference only: no diagnostics are collected or transmitted.");
    y += 45;

    draw_toggle_labeled(x, y, 300, "Send Crash Reports", crash_reports);
    y += 45;

    // App permissions: there is no per-app capability enforcement in this build,
    // so instead of a fabricated allow/deny matrix we state that honestly.
    draw_subsection(x, y, "App Permissions");
    y += 25;
    draw_hint_ic(x, y, "CMINUS", 0x00A0A0A8,
                 "Per-app permission enforcement is not implemented in this build.");
    y += 18;
    draw_hint(x, y, "All apps run with the user's full rights (capability tokens are planned).");
}

// =============================================================================
// Panel: Storage
// =============================================================================

static void draw_storage_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING;
    char buf[32];

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
    draw_label_value(x, y, "Thumbnails:", cache_str, 140);
    draw_button_small(x + 280, y - 3, 60, "Clear", false);
    y += 30;

    format_size(cache_apps, cache_str, sizeof(cache_str));
    draw_label_value(x, y, "App Cache:", cache_str, 140);
    draw_button_small(x + 280, y - 3, 60, "Clear", false);
    y += 30;

    format_size(cache_system, cache_str, sizeof(cache_str));
    draw_label_value(x, y, "System Cache:", cache_str, 140);
    draw_button_small(x + 280, y - 3, 60, "Clear", false);
    y += 40;

    // Trash
    draw_subsection(x, y, "Trash");
    y += 25;

    format_size(trash_size, cache_str, sizeof(cache_str));
    draw_label_value(x, y, "Trash Size:", cache_str, 140);
    draw_button_small(x + 280, y - 3, 100, "Empty Trash", false);
    y += 45;

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
    int y = PADDING;
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
}

// About-tab logo: /MAYLOGO.DAT = u16 w, u16 h (LE), then w*h RGBA bytes
// (from maytera-logo.png). Alpha-composited over the card colour so it reads on
// any theme. Replaces the old "M/OS" placeholder box.
static unsigned char s_logo[80 * 80 * 4];
static int s_logo_w = 0, s_logo_h = 0, s_logo_state = -1; // -1 untried, 0 fail, 1 ok
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
    int y = PADDING;
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
}

static void draw_about_panel(void) {
    int x = CONTENT_X + PADDING;
    int y = PADDING;

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

    // Actions
    draw_button(x, y, 130, "Check Updates", true, false);
    draw_button(x + 145, y, 130, "Export Debug", false, false);
    draw_button(x + 290, y, 100, "Credits", false, false);
    y += 35;
    if (about_status == 1)
        draw_hint_ic(x, y, "CCHECK", 0x0044B860, "System is up to date.");
    else if (about_status == 2)
        draw_hint(x, y, "Debug info written to /DEBUG.TXT");
    y += 15;

    // Legal
    win_draw_text(window_handle, x, y, "Copyright 2024-2026 MayteraOS Project", COL_TEXT_DISABLED);
    y += 18;
    win_draw_text(window_handle, x, y, "Licensed under MIT License", COL_TEXT_DISABLED);
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
#define DEV_ROW_Y0   (PADDING + 50)
#define DEV_ROW_H    76
#define DEV_CARD_H   66
#define DEV_BTN_W    82
#define DEV_BTN_H    24

static void draw_devices_panel(void) {
    printers_seed_once();   // loads the list + seeds the Brother example on first view
    int x  = CONTENT_X + PADDING;
    int y  = PADDING;
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
}

// =============================================================================
// Panel: Bluetooth (#372)
// UI codes against the bt_client.h contract, which mirrors the architect's
// kernel bt_ctrl.h one-to-one. The backend is a mock until the SYS_BT_* stack
// lands; swapping it in is a one-line change per function inside bt_client.h and
// nothing in this panel changes.
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

static void bt_draw_device_card(int x, int y, int cw, int dev, int paired_section) {
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
    int y  = PADDING;
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
        return;
    }

    int tgx = x + cw - 52, tgy = y - 2;
    win_draw_text(window_handle, tgx - 34, y + 2, on ? "On" : "Off",
                  on ? COL_SUCCESS : COL_TEXT_SECONDARY);
    bt_hit_add(tgx, tgy, 48, 24, BTA_POWER, -1);
    draw_toggle(tgx, tgy, on);
    y += 44;

    if (!on) {
        int ch = 120;
        draw_card(x, y, cw, ch);
        bt_draw_rune(x + cw / 2 - 14, y + 20, 28, COL_TEXT_DISABLED);
        gui_text_ttf_centered(window_handle, x, y + 56, cw, 20,
                              "Bluetooth is off", COL_TEXT_PRIMARY, 15);
        gui_text_ttf_centered(window_handle, x, y + 82, cw, 16,
                              "Turn on Bluetooth to connect keyboards, mice, headphones and more.",
                              COL_TEXT_SECONDARY, 12);
        g_bt_ndev = 0;
        return;
    }

    // Adapter status card.
    draw_card(x, y, cw, 48);
    bt_draw_rune(x + 14, y + 12, 24, COL_ACCENT);
    win_draw_text(window_handle, x + 48, y + 8, "MayteraOS Bluetooth", COL_TEXT_PRIMARY);
    g_bt_ndev = bt_get_devices(g_bt_dev, BT_MAX_DEVICES);
    {
        int nconn = 0;
        for (int i = 0; i < g_bt_ndev; i++) if (g_bt_dev[i].connected) nconn++;
        char sb[48]; int k = 0;
        if (nconn > 0) {
            for (const char *q = "Connected: "; *q; q++) sb[k++] = *q;
            sb[k++] = (char)('0' + (nconn > 9 ? 9 : nconn));
            for (const char *q = " device(s)"; *q; q++) sb[k++] = *q;
        } else {
            for (const char *q = "Ready"; *q; q++) sb[k++] = *q;
        }
        sb[k] = 0;
        win_draw_text_small(window_handle, x + 48, y + 28, sb, COL_TEXT_SECONDARY);
    }
    y += 60;

    // Scan control row.
    int scanning = bt_scan_active();
    bt_hit_add(x, y, 160, 30, BTA_SCAN, -1);
    draw_button(x, y, 160, scanning ? "Stop scanning" : "Scan for devices", !scanning, false);
    if (scanning) {
        bt_draw_spinner(x + 180, y + 15);
        win_draw_text(window_handle, x + 200, y + 8, "Scanning for devices...", COL_TEXT_SECONDARY);
    }
    y += 44;

    // ---- Paired devices ----
    draw_subsection(x, y, "Paired Devices");
    y += 26;
    int any_paired = 0;
    for (int i = 0; i < g_bt_ndev; i++) {
        if (!g_bt_dev[i].paired) continue;
        bt_draw_device_card(x, y, cw, i, 1);
        y += BT_ROW_STEP;
        any_paired = 1;
    }
    if (!any_paired) { draw_hint(x, y, "No paired devices yet. Scan and pair a device below."); y += 26; }
    y += 8;

    // ---- Available devices ----
    draw_subsection(x, y, "Available Devices");
    y += 26;
    int any_avail = 0;
    for (int i = 0; i < g_bt_ndev; i++) {
        if (g_bt_dev[i].paired) continue;
        bt_draw_device_card(x, y, cw, i, 0);
        y += BT_ROW_STEP;
        any_avail = 1;
    }
    if (!any_avail) {
        if (scanning) draw_hint_ic(x, y, "INFO", COL_ACCENT, "Searching for nearby devices...");
        else          draw_hint(x, y, "Tap \"Scan for devices\" to discover nearby Bluetooth devices.");
    }
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
// Panel: Wi-Fi (#384)
// Wired network status shown here is REAL (get_net_info); the Wi-Fi scan/connect
// is a mock behind wifi_client.h, so swapping in the real Wi-Fi driver later is
// a one-line change per function. Same structure as the Bluetooth panel.
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

static void wf_draw_net_card(int x, int y, int cw, int idx, int saved_section) {
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
    int y  = PADDING;
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
        return;
    }

    int tgx = x + cw - 52, tgy = y - 2;
    win_draw_text(window_handle, tgx - 34, y + 2, on ? "On" : "Off",
                  on ? COL_SUCCESS : COL_TEXT_SECONDARY);
    wf_hit_add(tgx, tgy, 48, 24, WFA_POWER, -1);
    draw_toggle(tgx, tgy, on);
    y += 44;

    if (!on) {
        draw_card(x, y, cw, 120);
        wf_draw_glyph(x + cw / 2 - 12, y + 22, 26, COL_TEXT_DISABLED);
        gui_text_ttf_centered(window_handle, x, y + 58, cw, 20, "Wi-Fi is off", COL_TEXT_PRIMARY, 15);
        gui_text_ttf_centered(window_handle, x, y + 84, cw, 16,
                              "Turn on Wi-Fi to see and join available networks.",
                              COL_TEXT_SECONDARY, 12);
        g_wf_nnet = 0;
        return;
    }

    // Status card: current SSID (mock) + REAL IP from the kernel if we have one.
    draw_card(x, y, cw, 48);
    wf_draw_glyph(x + 16, y + 14, 22, COL_ACCENT);
    char cur[WIFI_SSID_MAX];
    if (wifi_current(cur)) {
        char line[64]; int k = 0;
        for (const char *q = "Connected to "; *q; q++) line[k++] = *q;
        for (int i = 0; cur[i] && k < 60; i++) line[k++] = cur[i];
        line[k] = 0;
        win_draw_text(window_handle, x + 48, y + 8, line, COL_TEXT_PRIMARY);
        // Real network detail (IP) when the wired/kernel net is actually up.
        net_info_t ni;
        if (get_net_info(&ni, (long)sizeof(ni)) == 0 && ni.connected)
            draw_label_value(x + 48, y + 28, "IP:", ni.ip, 28);
        else
            win_draw_text_small(window_handle, x + 48, y + 28, "Wi-Fi link (no kernel IP)", COL_TEXT_SECONDARY);
    } else {
        win_draw_text(window_handle, x + 48, y + 8, "Not connected", COL_TEXT_PRIMARY);
        win_draw_text_small(window_handle, x + 48, y + 28, "Select a network below to join.", COL_TEXT_SECONDARY);
    }
    y += 60;

    int scanning = wifi_scan_active();
    wf_hit_add(x, y, 160, 30, WFA_SCAN, -1);
    draw_button(x, y, 160, scanning ? "Stop scanning" : "Scan for networks", !scanning, false);
    if (scanning) {
        bt_draw_spinner(x + 180, y + 15);
        win_draw_text(window_handle, x + 200, y + 8, "Scanning for networks...", COL_TEXT_SECONDARY);
    }
    y += 44;

    g_wf_nnet = wifi_get_networks(g_wf_net, WIFI_MAX_NETWORKS);

    draw_subsection(x, y, "Available Networks");
    y += 26;
    int any_avail = 0;
    for (int i = 0; i < g_wf_nnet; i++) {
        if (g_wf_net[i].saved && !g_wf_net[i].connected) continue;   // saved go below
        wf_draw_net_card(x, y, cw, i, 0);
        y += WF_ROW_STEP; any_avail = 1;
    }
    if (!any_avail) {
        if (scanning) draw_hint_ic(x, y, "INFO", COL_ACCENT, "Searching for nearby networks...");
        else          draw_hint(x, y, "Tap \"Scan for networks\" to find nearby Wi-Fi networks.");
        y += 26;
    }
    y += 8;

    draw_subsection(x, y, "Saved Networks");
    y += 26;
    int any_saved = 0;
    for (int i = 0; i < g_wf_nnet; i++) {
        if (!g_wf_net[i].saved || g_wf_net[i].connected) continue;
        wf_draw_net_card(x, y, cw, i, 1);
        y += WF_ROW_STEP; any_saved = 1;
    }
    if (!any_saved) draw_hint(x, y, "No saved networks. Networks you join are remembered here.");
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
    }
}

// =============================================================================
// Modal Dialog: Change Password
// =============================================================================

// Dialog height: scales with field count so 4/5-field forms (Add Printer) fit.
static int modal_dh(void) {
    if (modal_mode == MODAL_CREDITS) return 200;
    return 92 + modal_num_fields * 44;
}

static void draw_modal(void) {
    if (modal_mode == MODAL_NONE) return;

    // Dim (not black-out) the background: a cheap interlaced scrim. Apps have no
    // framebuffer read-back, so we draw a dark line on every other scanline -
    // the panel behind stays visible through the gaps but reads as darkened.
    for (int yy = 0; yy < WIN_HEIGHT; yy += 2)
        win_draw_rect(window_handle, 0, yy, WIN_WIDTH, 1, 0x00000000);

    // Dialog dimensions vary by type
    int dw = 360;
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
    else if (modal_mode == MODAL_ADD_PRINTER) title = "Add Printer";
    else if (modal_mode == MODAL_WIFI_PASSWORD) title = "Connect to Wi-Fi";
    win_draw_text(window_handle, dx + 14, dy + 10, title, COL_TEXT_PRIMARY);

    if (modal_mode == MODAL_CREDITS) {
        // Credits text
        win_draw_text(window_handle, dx + 14, dy + 50, "MayteraOS", COL_ACCENT);
        win_draw_text(window_handle, dx + 14, dy + 72, "The first LLM-native operating system.", COL_TEXT_PRIMARY);
        win_draw_text(window_handle, dx + 14, dy + 92, "Designed and built with Claude Code.", COL_TEXT_SECONDARY);
        win_draw_text(window_handle, dx + 14, dy + 114, "Copyright 2024-2026 MayteraOS Project.", COL_TEXT_SECONDARY);
        win_draw_text(window_handle, dx + 14, dy + 134, "Licensed under MIT License.", COL_TEXT_DISABLED);
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
    } else if (modal_mode == MODAL_ADD_FWRULE) {
        labels[0] = "Port (e.g. 22):";
        labels[1] = "Direction (in/out):";
        labels[2] = "Action (allow/deny):";
    } else if (modal_mode == MODAL_ADD_PRINTER) {
        labels[0] = "Name:";
        labels[1] = "Host / IP:";
        labels[2] = "Queue:";
        labels[3] = "Port (631):";
        labels[4] = "Default (y/n):";
    } else if (modal_mode == MODAL_WIFI_PASSWORD) {
        labels[0] = "Password:";
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

    // Error message
    int err_y = dy + 46 + modal_num_fields * 44;
    if (err_y > dy + dh - 50) err_y = dy + dh - 50;
    if (modal_error[0]) {
        draw_mico("CIRCX", dx + 14, err_y - 2, 14, 0x00CC3333, COL_CARD_BG);
        win_draw_text(window_handle, dx + 32, err_y, modal_error, 0x00CC3333);
    }

    draw_button(dx + dw - 184, dy + dh - 40, 80, "Cancel", false, false);
    draw_button(dx + dw -  96, dy + dh - 40, 80, "OK",     true,  false);
}

// =============================================================================
// Modal Submission
// =============================================================================

static void net_append_kv(char **pp, const char *key, const char *val) {
    char *p = *pp;
    while (*key) *p++ = *key++;
    *p++ = '=';
    while (*val) *p++ = *val++;
    *p++ = '\n';
    *pp = p;
}

// Persist the static network configuration to /CONFIG/NETIP.CFG in the exact
// plain key=value format the kernel parses at boot (net_apply_static_config()
// in kernel/net/net.c): "ip=...", "mask=...", "gw=...", "dns=..." (mask/gw/dns
// optional). Written so a static assignment set in the GUI survives a reboot.
// NOTE (honest caveat): net.c checks /NETCFG.TXT (FAT root) BEFORE this file, so
// if a /NETCFG.TXT image override is present it still wins at boot.
static void net_write_cfg(const char *ip, const char *mask,
                          const char *gw, const char *dns) {
    char buf[256]; char *p = buf;
    if (ip   && ip[0])   net_append_kv(&p, "ip",   ip);
    if (mask && mask[0]) net_append_kv(&p, "mask", mask);
    if (gw   && gw[0])   net_append_kv(&p, "gw",   gw);
    if (dns  && dns[0])  net_append_kv(&p, "dns",  dns);
    sys_unlink("/CONFIG/NETIP.CFG");
    int fd = sys_open("/CONFIG/NETIP.CFG", 0x41);   // O_WRONLY|O_CREAT
    if (fd < 0) return;
    sys_write(fd, buf, (unsigned long)(p - buf));
    sys_close(fd);
}

static void do_modal_submit(void) {
    if (modal_mode == MODAL_WIFI_PASSWORD) {
        // (#384) join the pending SSID with the entered key (mock accepts any).
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
        int res = passwd_change(users[current_user_idx].username,
                                modal_field[0], modal_field[1]);
        if (res < 0) {
            const char *msg = "Current password incorrect.";
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
        // Determine next UID (struct has no uid field, use count-based assignment)
        int next_uid = 1000 + user_count;
        // Build home path
        char home[64] = "/HOME/";
        int hi = 6, fi = 0;
        while (modal_field[0][fi] && hi < 62) { home[hi++] = modal_field[0][fi++]; }
        home[hi] = '\0';
        long r = adduser(modal_field[0], next_uid, next_uid, home, "/APPS/MSH");
        if (r == 0) {
            modal_mode = MODAL_NONE;
            users_refresh();
        } else {
            const char *msg = "Failed to add user.";
            int i = 0;
            while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; }
            modal_error[i] = '\0';
        }
        draw_all();
        return;
    }

    if (modal_mode == MODAL_ADD_FWRULE) {
        int port = 0; const char *ps = modal_field[0];
        while (*ps == ' ') ps++;
        while (*ps >= '0' && *ps <= '9') { port = port * 10 + (*ps - '0'); ps++; }
        if (port <= 0 || port > 65535) {
            const char *msg = "Enter a valid port (1-65535)";
            int i = 0; while (msg[i] && i < 63) { modal_error[i] = msg[i]; i++; } modal_error[i] = '\0';
            return;
        }
        int dir = (modal_field[1][0] == 'o' || modal_field[1][0] == 'O') ? 1 : 0;
        int act = (modal_field[2][0] == 'd' || modal_field[2][0] == 'D') ? 1 : 0;
        fw_add(dir, act, 0 /* TCP */, port);
        fw_save();
        modal_mode = MODAL_NONE;
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

    if (modal_mode == MODAL_SET_NETWORK) {
        // Live-apply ip/mask/gw to the running interface (kernel SYS_NET_SET_STATIC).
        net_set_static(modal_field[0], modal_field[1], modal_field[2]);
        // Adopt exactly what the user entered as the on-screen truth. The DNS
        // server has no live-apply syscall yet (net_set_static takes only
        // ip/mask/gw), so it is persisted below and applied by the kernel on the
        // next boot via net_apply_static_config(); live DNS apply is a follow-up.
        copy_str(ip_address,  modal_field[0], sizeof(ip_address));
        copy_str(subnet_mask, modal_field[1], sizeof(subnet_mask));
        copy_str(gateway,     modal_field[2], sizeof(gateway));
        copy_str(dns_primary, modal_field[3], sizeof(dns_primary));
        // Persist so the static config survives a reboot.
        net_write_cfg(modal_field[0], modal_field[1], modal_field[2], modal_field[3]);
        net_info_t ni;
        if (get_net_info(&ni, (long)sizeof(ni)) == 0)
            ethernet_connected = ni.connected;
        modal_mode = MODAL_NONE;
        draw_all();
        return;
    }

    if (modal_mode == MODAL_EDIT_PROFILE) {
        copy_str(users[current_user_idx].fullname, modal_field[0], 64);
        copy_str(users[current_user_idx].email,    modal_field[1], 64);
        // Persist the edited email to the real per-account store.
        useremail_set(users[current_user_idx].username, users[current_user_idx].email);
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
        gui_draw_rect_outline(window_handle, fr->x - 2, fr->y - 2, fr->w + 4, fr->h + 4, COL_ACCENT);
        gui_draw_rect_outline(window_handle, fr->x - 1, fr->y - 1, fr->w + 2, fr->h + 2, COL_ACCENT);
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
                content_scroll_y = 0;
                // (#262) Re-read the Default Apps state once on panel entry (picks
                // up external /ASSOC.CFG changes) instead of per redraw frame.
                if (i == PANEL_DEFAULTS) defaults_invalidate_cache();
                // (#318) On entering Devices, load printers (seed the Brother
                // example once if nothing is configured).
                if (i == PANEL_DEVICES) { printers_seed_once(); printers_refresh(); g_print_status[0] = '\0'; }
                // Refresh real cache/trash sizes from disk on entering Storage.
                if (i == PANEL_STORAGE) storage_scan();
                // Populate live network data when switching to Network panel
                if (i == PANEL_NETWORK) {
                    net_info_t ni;
                    if (get_net_info(&ni, (long)sizeof(ni)) == 0) {
                        copy_str(ip_address,  ni.ip,      sizeof(ip_address));
                        copy_str(gateway,     ni.gateway, sizeof(gateway));
                        copy_str(subnet_mask, ni.netmask, sizeof(subnet_mask));
                        copy_str(dns_primary, ni.dns,     sizeof(dns_primary));
                        copy_str(mac_address, ni.mac,     sizeof(mac_address));
                        ethernet_connected = ni.connected;
                    }
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
// #387: publish the chosen dock layout via /DOCKSTYL.CFG; the compositor polls
// the file and applies + persists it live (no restart).
static void dock_dd_changed(void) {
    if (dock_style < 0 || dock_style > 3) dock_style = 0;
    int fd = sys_open("/DOCKSTYL.CFG", 0x41 | 0x200 /*O_WRONLY|O_CREAT|O_TRUNC*/);
    if (fd < 0) return;
    char c = (char)('0' + dock_style);
    sys_write(fd, &c, 1);
    sys_close(fd);
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
static void cursor_dd_changed(void)    { set_cursor_theme(CURSOR_KERNEL_MAP[cursor_theme]); set_cursor(cursor_theme, 100); }  /* (#116) live cursor: 0/1/2 = Light/Dark/Glow */
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
    int fd = sys_open("/CONFIG/EXTSVC.CFG", 0x0001|0x0040);   // O_WRONLY|O_CREAT
    if (fd < 0) fd = sys_open("/EXTSVC.CFG", 0x0001|0x0040);
    if (fd < 0) { ex_set_status("Could not write config"); return; }
    static char out[700]; int o=0;
    const char *k1="url="; for(int j=0;k1[j];j++) out[o++]=k1[j]; for(int j=0;g_ext_url[j];j++) out[o++]=g_ext_url[j]; out[o++]='\n';
    const char *k2="token="; for(int j=0;k2[j];j++) out[o++]=k2[j]; for(int j=0;g_ext_token[j];j++) out[o++]=g_ext_token[j]; out[o++]='\n';
    const char *k3="refresh="; for(int j=0;k3[j];j++) out[o++]=k3[j]; for(int j=0;g_ext_refresh[j];j++) out[o++]=g_ext_refresh[j]; out[o++]='\n';
    sys_write(fd,out,o); sys_close(fd);
    ex_set_status("Saved to /CONFIG/EXTSVC.CFG");
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

// Field rectangles (kept in sync between draw and click).
static void ext_field_rect(int idx,int *fx,int *fy,int *fw){
    int x = CONTENT_X + PADDING, y = PADDING + 40;
    *fx = x + 15; *fw = CONTENT_WIDTH - 2*PADDING - 30;
    *fy = y + 28 + idx*52;
}
static void ext_btn_test_rect(int *bx,int *by,int *bw){ int x=CONTENT_X+PADDING, y=PADDING+40; *bx=x+15; *by=y+28+3*52+6; *bw=150; }
static void ext_btn_save_rect(int *bx,int *by,int *bw){ int x=CONTENT_X+PADDING, y=PADDING+40; *bx=x+175; *by=y+28+3*52+6; *bw=110; }

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
    int fd = sys_open("/CONFIG/AISVC.CFG", 0);
    if (fd < 0) fd = sys_open("/AISVC.CFG", 0);
    if (fd < 0){
        // No config yet: default to Moonshot(Kimi); surface an existing KIMI.KEY masked.
        ai_apply_preset(2);
        int kf = sys_open("/CONFIG/KIMI.KEY", 0);
        if(kf>=0){ char kb[300]; long n=sys_read(kf,kb,sizeof(kb)-1); sys_close(kf);
            if(n>0){ kb[n]=0; int e=(int)n-1; while(e>=0&&(kb[e]=='\n'||kb[e]=='\r'||kb[e]==' '||kb[e]=='\t')) kb[e--]=0; ai_cpy(g_ai_key,sizeof(g_ai_key),kb); } }
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
    int fd = sys_open("/CONFIG/AISVC.CFG", 0x0001|0x0040);   // O_WRONLY|O_CREAT
    if (fd < 0) fd = sys_open("/AISVC.CFG", 0x0001|0x0040);
    if (fd < 0){ ai_set_status("Could not write config"); return; }
    static char out[900]; int o=0;
    const char *pn=(g_ai_preset>=0&&g_ai_preset<AI_NPRESET)?g_ai_presets[g_ai_preset].name:"Custom";
    const char *k0="provider=";  for(int j=0;k0[j];j++) out[o++]=k0[j]; for(int j=0;pn[j];j++) out[o++]=pn[j]; out[o++]='\n';
    const char *k1="endpoint=";  for(int j=0;k1[j];j++) out[o++]=k1[j]; for(int j=0;g_ai_endpoint[j];j++) out[o++]=g_ai_endpoint[j]; out[o++]='\n';
    const char *k2="model=";     for(int j=0;k2[j];j++) out[o++]=k2[j]; for(int j=0;g_ai_model[j];j++)    out[o++]=g_ai_model[j];    out[o++]='\n';
    const char *k3="api_key=";   for(int j=0;k3[j];j++) out[o++]=k3[j]; for(int j=0;g_ai_key[j];j++)      out[o++]=g_ai_key[j];      out[o++]='\n';
    const char *k4="api_style="; for(int j=0;k4[j];j++) out[o++]=k4[j]; const char *st=g_ai_style?"anthropic":"bearer"; for(int j=0;st[j];j++) out[o++]=st[j]; out[o++]='\n';
    sys_write(fd,out,o); sys_close(fd);
    ai_set_status("Saved to /CONFIG/AISVC.CFG");
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
static int  ai_card_y(void){ return PADDING + 40 + 246 + 14; }
static void ai_field_rect(int idx,int *fx,int *fy,int *fw){ int x=CONTENT_X+PADDING; *fx=x+15; *fw=CONTENT_WIDTH-2*PADDING-30; *fy=ai_card_y()+64+idx*46; }
static void ai_preset_rect(int *bx,int *by,int *bw){ int x=CONTENT_X+PADDING; *bx=x+15;  *by=ai_card_y()+30; *bw=CONTENT_WIDTH-2*PADDING-30; }
static void ai_btn_test_rect(int *bx,int *by,int *bw){ int x=CONTENT_X+PADDING; *bx=x+15;  *by=ai_card_y()+64+3*46+6; *bw=130; }
static void ai_btn_save_rect(int *bx,int *by,int *bw){ int x=CONTENT_X+PADDING; *bx=x+155; *by=ai_card_y()+64+3*46+6; *bw=100; }

static void draw_extsvc_panel(void){
    if(!g_ext_loaded) ext_load_cfg();
    int x = CONTENT_X + PADDING, y = PADDING;
    draw_section_header(x, y, "External Services");
    y += 40;
    draw_card(x, y, CONTENT_WIDTH - 2*PADDING, 246);
    win_draw_text(window_handle, x+15, y+8, "Home Assistant", COL_TEXT_PRIMARY);
    const char *labels[3] = {"Base URL (http://host:8123)","Long-Lived Access Token","Refresh interval (seconds)"};
    for(int i=0;i<3;i++){
        int fx,fy,fw; ext_field_rect(i,&fx,&fy,&fw);
        win_draw_text(window_handle, fx, fy-14, labels[i], COL_TEXT_SECONDARY);
        win_draw_rect(window_handle, fx, fy, fw, 24, COL_INPUT_BG);
        gui_draw_rect_outline(window_handle, fx, fy, fw, 24, (g_ext_focus==i)?COL_ACCENT:COL_INPUT_BORDER);
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
        gui_draw_rect_outline(window_handle, fx, fy, fw, 24, (g_ai_focus==i)?COL_ACCENT:COL_INPUT_BORDER);
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
      if(g_ai_status[0]) win_draw_text(window_handle, x+15, aby+42, g_ai_status, COL_TEXT_SECONDARY); }
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
    int base_y = PADDING;

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
        case PANEL_APPEARANCE: {
            // Theme dropdown (y ~ 65)
            int theme_y = base_y + 65;
            if (local_y >= theme_y && local_y < theme_y + 28 &&
                local_x >= x && local_x < x + 220) {
                dropdown_open(x, theme_y, 220, g_theme_names, NUM_THEMES,
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
                dropdown_open(x + 120, font_y, 160, FONT_SIZE_OPTS, 4,
                              &font_size, font_dd_changed);
                draw_all(); return;
            }

            // Icon size dropdown
            int icon_y = font_y + 40;
            if (local_y >= icon_y && local_y < icon_y + 28 &&
                local_x >= x + 120 && local_x < x + 120 + 160) {
                dropdown_open(x + 120, icon_y, 160, ICON_SIZE_OPTS, 3,
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
                    dropdown_open(x, ss_y, 160, SS_OPTS, 9,
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

            // Cursor theme dropdown
            int ctheme_y = ss_y + 95;
            if (local_y >= ctheme_y && local_y < ctheme_y + 28 &&
                local_x >= x && local_x < x + 160) {
                dropdown_open(x, ctheme_y, 160, CURSOR_OPTS, 3,
                              &cursor_theme, cursor_dd_changed);
                draw_all(); return;
            }

            // #387 Dock style dropdown (below Cursor).
            int dock_y = ctheme_y + 70;
            if (local_y >= dock_y && local_y < dock_y + 28 &&
                local_x >= x && local_x < x + 220) {
                dropdown_open(x, dock_y, 220, DOCK_OPTS, 4,
                              &dock_style, dock_dd_changed);
                draw_all(); return;
            }

            // Wallpaper dropdown (right column, mirrors the other
            // setting dropdowns above rather than a per-cell thumbnail grid).
            {
                int wp_dy = WP_DD_Y + 25;
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
            // #387: shifted down by the Dock Style row (dock_y + its 70px block).
            int tr_y = dock_y + 70;
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
            // Brightness slider
            int bright_y = base_y + 130;
            if (local_y >= bright_y && local_y < bright_y + 16 &&
                local_x >= x && local_x < x + 350) {
                brightness = ((local_x - x) * 100) / 350;
                if (brightness < 0) brightness = 0;
                if (brightness > 100) brightness = 100;
                apply_display_fx(); draw_all();
                return;
            }

            // Night light toggle
            int nl_y = bright_y + 70;
            if (local_y >= nl_y && local_y < nl_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                night_light = !night_light;
                apply_display_fx(); draw_all();
                return;
            }

            // Night light strength slider (only when enabled)
            if (night_light) {
                int nls_y = nl_y + 35;
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
            // DHCP toggle
            int dhcp_y = base_y + 165;
            if (local_y >= dhcp_y && local_y < dhcp_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                dhcp_enabled = !dhcp_enabled;
                if (dhcp_enabled) {
                    net_dhcp();   // acquire a lease
                    net_info_t ni;
                    if (get_net_info(&ni, (long)sizeof(ni)) == 0) {
                        copy_str(ip_address,  ni.ip,      sizeof(ip_address));
                        copy_str(gateway,     ni.gateway, sizeof(gateway));
                        copy_str(subnet_mask, ni.netmask, sizeof(subnet_mask));
                        copy_str(dns_primary, ni.dns,     sizeof(dns_primary));
                        ethernet_connected = ni.connected;
                    }
                }
                draw_all();
                return;
            }

            // Configure IP button (only shown when DHCP is off)
            if (!dhcp_enabled) {
                int cfg_y = dhcp_y + 40;
                if (local_y >= cfg_y && local_y < cfg_y + 28 &&
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
            }

            // VPN is now an honest "not available" note (no toggle).

            // Firewall toggle. VPN block is a fixed 55px (subsection + hint), so
            // the firewall row sits 120px below the DHCP toggle.
            int fw_y = dhcp_y + 120;
            if (local_y >= fw_y && local_y < fw_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                firewall_enabled = !firewall_enabled;
                draw_all();
                return;
            }

            // Firewall rules (iptables-style) when enabled
            if (firewall_enabled) {
                int fc = fw_y + 35;
                if (local_y >= fc && local_y < fc + 28) {            // default inbound
                    for (int i = 0; i < 2; i++)
                        if (local_x >= x + 170 + i*90 && local_x < x + 170 + i*90 + 82) {
                            fw_pol_in = i; fw_save(); draw_all(); return;
                        }
                }
                if (local_y >= fc + 34 && local_y < fc + 34 + 28) {  // default outbound
                    for (int i = 0; i < 2; i++)
                        if (local_x >= x + 170 + i*90 && local_x < x + 170 + i*90 + 82) {
                            fw_pol_out = i; fw_save(); draw_all(); return;
                        }
                }
                int rules_y = fc + 90;
                for (int i = 0; i < fw_rule_count; i++) {
                    int ry = rules_y + i * 26;
                    if (local_y >= ry && local_y < ry + 20) {
                        if (local_x >= x + 20  && local_x < x + 76)  { fw_rules[i].action ^= 1; fw_save(); draw_all(); return; }
                        if (local_x >= x + 84  && local_x < x + 128) { fw_rules[i].dir    ^= 1; fw_save(); draw_all(); return; }
                        if (local_x >= x + 136 && local_x < x + 180) { fw_rules[i].proto  ^= 1; fw_save(); draw_all(); return; }
                        if (local_x >= x + 260 && local_x < x + 284) {
                            for (int k = i; k < fw_rule_count - 1; k++) fw_rules[k] = fw_rules[k+1];
                            fw_rule_count--; fw_save(); draw_all(); return;
                        }
                    }
                }
                if (fw_rule_count < MAX_FW_RULES) {
                    int add_y = rules_y + fw_rule_count * 26;
                    if (local_y >= add_y && local_y < add_y + 24 &&
                        local_x >= x + 20 && local_x < x + 140) {
                        modal_mode = MODAL_ADD_FWRULE;
                        modal_num_fields = 3;
                        copy_to_modal_field(0, "");
                        copy_to_modal_field(1, "in");
                        copy_to_modal_field(2, "allow");
                        modal_active_field = 0;
                        modal_error[0] = '\0';
                        draw_all();
                        return;
                    }
                }
            }
            break;
        }


        case PANEL_KEYBOARD: {
            // Repeat rate slider
            int rate_y = base_y + 130;
            if (local_y >= rate_y && local_y < rate_y + 16 &&
                local_x >= x + 140 && local_x < x + 340) {
                key_repeat_rate = ((local_x - x - 140) * 50) / 200;
                if (key_repeat_rate < 1) key_repeat_rate = 1;
                if (key_repeat_rate > 50) key_repeat_rate = 50;
                draw_all();
                return;
            }

            // Repeat delay slider
            int delay_y = rate_y + 35;
            if (local_y >= delay_y && local_y < delay_y + 16 &&
                local_x >= x + 140 && local_x < x + 340) {
                key_repeat_delay = ((local_x - x - 140) * 500) / 200;
                if (key_repeat_delay < 50) key_repeat_delay = 50;
                if (key_repeat_delay > 500) key_repeat_delay = 500;
                draw_all();
                return;
            }

            // Lock key checkboxes
            int lock_y = delay_y + 75;
            if (local_y >= lock_y && local_y < lock_y + 18) {
                if (local_x >= x && local_x < x + 100) {
                    num_lock = !num_lock;
                    draw_all();
                    return;
                }
                if (local_x >= x + 150 && local_x < x + 280) {
                    caps_lock = !caps_lock;
                    draw_all();
                    return;
                }
                if (local_x >= x + 300 && local_x < x + 440) {
                    scroll_lock = !scroll_lock;
                    draw_all();
                    return;
                }
            }
            break;
        }

        case PANEL_MOUSE: {
            // Pointer speed slider
            int speed_y = base_y + 65;
            if (local_y >= speed_y && local_y < speed_y + 16 &&
                local_x >= x + 140 && local_x < x + 390) {
                pointer_speed = ((local_x - x - 140) * 100) / 250;
                if (pointer_speed < 0) pointer_speed = 0;
                if (pointer_speed > 100) pointer_speed = 100;
                set_mouse_speed(1 + (pointer_speed * 9) / 100);
                draw_all();
                return;
            }

            // Double-click speed slider
            int dbl_y = speed_y + 35;
            if (local_y >= dbl_y && local_y < dbl_y + 16 &&
                local_x >= x + 160 && local_x < x + 390) {
                double_click_speed = ((local_x - x - 160) * 100) / 230;
                if (double_click_speed < 0) double_click_speed = 0;
                if (double_click_speed > 100) double_click_speed = 100;
                draw_all();
                return;
            }

            // Pointer trails toggle
            int trails_y = dbl_y + 90;
            if (local_y >= trails_y && local_y < trails_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                pointer_trails = !pointer_trails;
                draw_all();
                return;
            }

            // Trail length slider (only when pointer_trails enabled)
            if (pointer_trails) {
                int trl_y = trails_y + 35;
                if (local_y >= trl_y && local_y < trl_y + 16 &&
                    local_x >= x + 140 && local_x < x + 290) {
                    pointer_trail_length = ((local_x - x - 140) * 10) / 150;
                    if (pointer_trail_length < 1) pointer_trail_length = 1;
                    if (pointer_trail_length > 10) pointer_trail_length = 10;
                    draw_all(); return;
                }
            }

            // Natural scrolling toggle
            int nat_y = trails_y + (pointer_trails ? 110 : 75);

            // Scroll speed slider (section before natural scrolling)
            {
                int scroll_spd_y = nat_y - 35;
                if (local_y >= scroll_spd_y && local_y < scroll_spd_y + 16 &&
                    local_x >= x + 140 && local_x < x + 340) {
                    scroll_speed = ((local_x - x - 140) * 100) / 200;
                    if (scroll_speed < 0) scroll_speed = 0;
                    if (scroll_speed > 100) scroll_speed = 100;
                    draw_all(); return;
                }
            }

            if (local_y >= nat_y && local_y < nat_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                natural_scrolling = !natural_scrolling;
                draw_all();
                return;
            }

            // Scroll inertia toggle
            int inert_y = nat_y + 55;
            if (local_y >= inert_y && local_y < inert_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                scroll_inertia = !scroll_inertia;
                draw_all();
                return;
            }

            // Left-handed toggle
            int left_y = inert_y + 70;
            if (local_y >= left_y && local_y < left_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                left_handed = !left_handed;
                draw_all();
                return;
            }
            break;
        }

        case PANEL_DATETIME: {
            // Auto time toggle (with NTP sync on enable)
            int auto_y = base_y + 155;
            if (local_y >= auto_y && local_y < auto_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                auto_time = !auto_time;
                if (auto_time) {
                    long r = ntp_sync();
                    ntp_status = (r == 0) ? 1 : -1;
                }
                draw_all();
                return;
            }

            // 24-hour toggle
            int fmt_y = auto_y + 60;
            if (local_y >= fmt_y && local_y < fmt_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                use_24hour = !use_24hour;
                draw_all();
                return;
            }

            // Timezone dropdown: open a scrollable list (current item highlighted)
            int tz_y = fmt_y + 70;
            if (local_y >= tz_y && local_y < tz_y + 28 &&
                local_x >= x && local_x < x + 350) {
                dropdown_open(x, tz_y, 350, timezones, NUM_TIMEZONES,
                              &timezone_idx, update_timezone_offset);
                draw_all(); return;
            }

            // Date format dropdown
            int dfmt_y = fmt_y + 95;
            if (local_y >= dfmt_y && local_y < dfmt_y + 28 &&
                local_x >= x && local_x < x + 160) {
                dropdown_open(x, dfmt_y, 160, DATE_FMT_OPTS, 3, &date_format, 0);
                draw_all(); return;
            }

            // Week start buttons
            int week_y = dfmt_y + 45;
            if (local_y >= week_y && local_y < week_y + 28) {
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
                int setdt_y = week_y + 50;
                if (local_y >= setdt_y && local_y < setdt_y + 30 &&
                    local_x >= x && local_x < x + 160) {
                    modal_mode = MODAL_SET_DATETIME;
                    modal_num_fields = 2;
                    int rh = 0, rm = 0, rs = 0;
                    int rd = 1, rmo = 1, ry = 2026;
                    get_rtc_time(&rh, &rm, &rs);
                    get_rtc_date(&rd, &rmo, &ry);
                    format_hms(modal_field[0], rh, rm, rs);
                    format_ymd(modal_field[1], ry, rmo, rd);
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
            // Edit Profile button (inside current user card)
            int ep_y = base_y + 75;
            if (local_y >= ep_y && local_y < ep_y + 30 &&
                local_x >= x + 350 && local_x < x + 450) {
                modal_mode = MODAL_EDIT_PROFILE;
                modal_num_fields = 2;
                copy_to_modal_field(0, users[current_user_idx].fullname);
                copy_to_modal_field(1, users[current_user_idx].email);
                modal_active_field = 0;
                modal_error[0] = '\0';
                draw_all(); return;
            }

            // Auto-login toggle
            int al_y = base_y + 180;
            if (local_y >= al_y && local_y < al_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                auto_login = !auto_login;
                draw_all();
                return;
            }

            // Change Password button (al_y + 10 for y+=10 after toggle + 45 for hint = al_y+55)
            int chpw_y = al_y + 55;
            if (local_y >= chpw_y && local_y < chpw_y + 30 &&
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

            // Per-user "Remove" buttons in the Other Users list. Mirrors the
            // row layout in draw_users_panel (rows from base_y+310, 60px pitch).
            {
                int row_y = base_y + 310;
                for (int i = 0; i < user_count; i++) {
                    if (i == current_user_idx) continue;
                    if (users[i].username[0] == 0) continue;
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
            int adduser_btn_y = al_y + 140 + (user_count - 1) * 60;
            if (local_y >= adduser_btn_y && local_y < adduser_btn_y + 30 &&
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
            int guest_y = al_y + 180 + (user_count - 1) * 60;
            if (local_y >= guest_y && local_y < guest_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                guest_enabled = !guest_enabled;
                draw_all();
                return;
            }
            break;
        }

        case PANEL_PRIVACY: {
            // Every change persists to /CONFIG/PRIVACY.CFG (real stored setting).
            // Screen lock toggle
            int lock_y = base_y + 65;
            if (local_y >= lock_y && local_y < lock_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                screen_lock_enabled = !screen_lock_enabled;
                privacy_save();
                draw_all();
                return;
            }

            // Location services toggle
            int loc_y = lock_y + (screen_lock_enabled ? 115 : 35) + 25;
            if (local_y >= loc_y && local_y < loc_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                location_services = !location_services;
                privacy_save();
                draw_all();
                return;
            }

            // Diagnostics toggle
            int diag_y = loc_y + 55;
            if (local_y >= diag_y && local_y < diag_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                diagnostics_enabled = !diagnostics_enabled;
                privacy_save();
                draw_all();
                return;
            }

            // Crash reports toggle (Diagnostics now has an honest hint line, so
            // Crash sits 55px below Diagnostics: 10px gap + 45px advance.)
            int crash_y = diag_y + 55;
            if (local_y >= crash_y && local_y < crash_y + 24 &&
                local_x >= x + 300 && local_x < x + 348) {
                crash_reports = !crash_reports;
                privacy_save();
                draw_all();
                return;
            }

            // Screen lock timeout dropdown (cycle values when clicked)
            if (screen_lock_enabled) {
                int timeout_dd_y = lock_y + 35;
                if (local_y >= timeout_dd_y && local_y < timeout_dd_y + 28 &&
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
                // Require password on wake toggle
                int rpw_y = lock_y + 75;
                if (local_y >= rpw_y && local_y < rpw_y + 24 &&
                    local_x >= x + 280 && local_x < x + 328) {
                    require_password_wake = !require_password_wake;
                    privacy_save();
                    draw_all(); return;
                }
            }
            break;
        }

        case PANEL_STORAGE: {
            // Cache and trash clear buttons
            // drive_count drives each take 80px; cache section starts after
            int cache_base = base_y + 40 + drive_count * 80 + 40;

            // Thumbnails clear (unlinks real files, then rescans)
            if (local_y >= cache_base && local_y < cache_base + 24 &&
                local_x >= x + 280 && local_x < x + 340) {
                dir_clear_files(CACHE_DIR_THUMBS); storage_scan(); draw_all(); return;
            }
            // App cache clear
            int ca_y = cache_base + 30;
            if (local_y >= ca_y && local_y < ca_y + 24 &&
                local_x >= x + 280 && local_x < x + 340) {
                dir_clear_files(CACHE_DIR_APPS); storage_scan(); draw_all(); return;
            }
            // System cache clear
            int cs_y = ca_y + 30;
            if (local_y >= cs_y && local_y < cs_y + 24 &&
                local_x >= x + 280 && local_x < x + 340) {
                dir_clear_files(CACHE_DIR_SYSTEM); storage_scan(); draw_all(); return;
            }
            // Empty Trash
            int trash_btn_y = cs_y + 55;
            if (local_y >= trash_btn_y && local_y < trash_btn_y + 24 &&
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
            // Button row: Check Updates, Export Debug, Credits
            int about_btn_y = base_y + 40 + 115 + 155 + 25 + 25 + 35;
            if (local_y >= about_btn_y && local_y < about_btn_y + 30) {
                // Check Updates
                if (local_x >= x && local_x < x + 130) {
                    about_status = 1; draw_all(); return;
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
    sv_kvi("scaling_pct", scaling_factor);

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
    sv_kvs("timezone", timezones[timezone_idx]);
    sv_kvi("tz_offset_min", timezone_offset_minutes);

    // ---- Appearance ----
    sv_raw("\n[Appearance]\n");
    sv_kvs("theme", g_theme_names[current_theme]);
    sv_kvi("accent_idx", accent_color_idx);
    sv_kvs("dock_style", DOCK_OPTS[dock_style]);
    sv_kvi("wallpaper_idx", wallpaper_idx);
    sv_kvi("transparency_pct", transparency_level);

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

    int fd = sys_open("/SETVALS.TXT", 0x0001 | 0x0040 | 0x0200);  // O_WRONLY|O_CREAT|O_TRUNC
    if (fd < 0) return;
    sys_write(fd, g_sv_buf, (unsigned long)g_sv_len);
    sys_close(fd);
}

// =============================================================================
// Main Entry Point
// =============================================================================

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    wp_init();   // #517: enumerate wallpapers from the image (shared with compositor)

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

    // Sync current_theme from the actual running kernel theme so buttons
    // highlight the correct selection when the window opens.
    {
        int kt = get_theme();
        switch (kt) {
            case 1: current_theme = 0; break;  // Dark
            case 2: current_theme = 1; break;  // Light
            case 4: current_theme = 2; break;  // Classic
            case 5: current_theme = 3; break;  // Ocean
            case 9: current_theme = 4; break;  // Nord
            default: current_theme = 0; break;
        }
    }

    // Load persisted Settings-app preferences (timezone, formats, accent, etc.)
    // before applying theme/timezone so the restored values take effect.
    settings_load();
    alerts_load();
    privacy_load();          // (#382 pass2) restore persisted privacy settings
    setlog("SET: settings_load done");
    // #387: read the compositor's current dock style so the picker shows it.
    {
        int fd = sys_open("/DOCKSTYL.CFG", 0 /*O_RDONLY*/);
        if (fd >= 0) {
            char c = 0;
            if (sys_read(fd, &c, 1) == 1 && c >= '0' && c <= '3') dock_style = c - '0';
            sys_close(fd);
        }
    }
    if (cursor_theme < 0 || cursor_theme > 2) cursor_theme = 0;
    set_cursor(cursor_theme, 100);   // (#116) sync kernel cursor with loaded pref
    setlog("SET: cursor applied");
    if (screensaver_idx < 0 || screensaver_idx > 8) screensaver_idx = 1;
    set_screensaver(SS_KERNEL_MAP[screensaver_idx]);   // (#115) sync saver type
    if (screensaver_delay_min < 1) screensaver_delay_min = 2;
    set_ss_delay(screensaver_delay_min * 60);           // (#115) sync activation delay

    // Apply initial theme colors (also calls set_theme to confirm kernel state)
    apply_theme(current_theme);
    setlog("SET: theme applied");

    // Initialize timezone offset from current selection
    update_timezone_offset();

    // Populate initial network data
    setlog("SET: net probe begin");
    {
        net_info_t ni;
        if (get_net_info(&ni, (long)sizeof(ni)) == 0) {
            copy_str(ip_address,  ni.ip,      sizeof(ip_address));
            copy_str(gateway,     ni.gateway, sizeof(gateway));
            copy_str(subnet_mask, ni.netmask, sizeof(subnet_mask));
            copy_str(dns_primary, ni.dns,     sizeof(dns_primary));
            copy_str(mac_address, ni.mac,     sizeof(mac_address));
            ethernet_connected = ni.connected;
        }
    }
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

    // Load firewall rules (or seed sensible defaults on first run).
    fw_load();
    setlog("SET: fw_load done");

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

        // (#372/#384) Advance the Bluetooth / Wi-Fi mock state machines at ~10Hz
        // REGARDLESS of event flow. The compositor issues periodic EVENT_REDRAWs,
        // so win_get_event rarely times out; ticking only in the timeout branch
        // stalls the scan/pair/connect animations. Time-throttle so a busy event
        // stream does not over-tick.
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
                // (#261) When a dropdown is OPEN, arrow keys navigate ITS list (live
                // preview via on_change, like clicking), and Enter/Space/Esc close it.
                // Previously Up/Down moved the focus ring instead, so list keys did nothing.
                if (g_dd_open && g_dd_sel) {
                    int vis = g_dd_count < DD_VISIBLE ? g_dd_count : DD_VISIBLE;
                    if (event.keycode == 0x80) {                 // Up
                        if (*g_dd_sel > 0) (*g_dd_sel)--;
                        if (*g_dd_sel < g_dd_scroll) g_dd_scroll = *g_dd_sel;
                        if (g_dd_on_change) g_dd_on_change();
                        draw_all(); break;
                    }
                    if (event.keycode == 0x81) {                 // Down
                        if (*g_dd_sel < g_dd_count - 1) (*g_dd_sel)++;
                        if (*g_dd_sel >= g_dd_scroll + vis) g_dd_scroll = *g_dd_sel - vis + 1;
                        if (g_dd_on_change) g_dd_on_change();
                        draw_all(); break;
                    }
                    if (event.key_char == '\r' || event.key_char == '\n' ||
                        event.key_char == ' ' || event.key_char == 27) {  // commit/close
                        g_dd_open = 0; draw_all(); break;
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
                    if (current_panel > 0) { current_panel--; content_scroll_y = 0; g_focus_idx = 0; sidebar_reveal_current(); draw_all(); }
                } else if (event.keycode == GUI_KEY_RIGHT) {  // next panel
                    if (current_panel < PANEL_COUNT - 1) { current_panel++; content_scroll_y = 0; g_focus_idx = 0; sidebar_reveal_current(); draw_all(); }
                } else if (event.keycode == GUI_KEY_HOME) {
                    current_panel = 0; content_scroll_y = 0; g_focus_idx = 0; sidebar_reveal_current(); draw_all();
                } else if (event.keycode == GUI_KEY_END) {
                    current_panel = PANEL_COUNT - 1; content_scroll_y = 0; g_focus_idx = 0; sidebar_reveal_current(); draw_all();
                } else if (event.keycode == GUI_KEY_PGUP || event.keycode == GUI_KEY_PGDN) {
                    // Page the sidebar list itself. This is the keyboard-only
                    // route to a panel that is scrolled out of view, which is the
                    // ONLY route wherever the pointer's wheel is unavailable (the
                    // Magic Mouse on the iMac, #438) or the mouse is dead.
                    if (gui_scroll_key(&g_side_scroll, event.keycode)) draw_all();
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
                    // dh MUST match draw_modal exactly or the button hit-test misses.
                    int dw = 360;
                    int dh = modal_dh();
                    int dx = (WIN_WIDTH  - dw) / 2;
                    int dy = (WIN_HEIGHT - dh) / 2;
                    // Credits has a single Close button at the OK position.
                    if (modal_mode == MODAL_CREDITS) {
                        int by = dy + dh - 40;
                        if (local_y >= by && local_y < by + 30 &&
                            local_x >= dx + dw - 96 && local_x < dx + dw - 16) {
                            modal_mode = MODAL_NONE; draw_all();
                        }
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
                break;

            case EVENT_MOUSE_SCROLL: {
                // An open dropdown scrolls its list
                if (g_dd_open) {
                    int vis = g_dd_count < DD_VISIBLE ? g_dd_count : DD_VISIBLE;
                    g_dd_scroll -= event.scroll_delta;
                    if (g_dd_scroll > g_dd_count - vis) g_dd_scroll = g_dd_count - vis;
                    if (g_dd_scroll < 0) g_dd_scroll = 0;
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
                    content_scroll_y -= event.scroll_delta * 30;
                    if (content_scroll_y < 0) content_scroll_y = 0;
                    if (content_scroll_y > max_scroll_y) content_scroll_y = max_scroll_y;
                    draw_all();
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
