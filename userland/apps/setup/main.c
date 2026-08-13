// setup/main.c - MayteraOS first-boot setup wizard (OOBE).
//
// Runs once on any UNCONFIGURED system: after a disk install, and equally on a
// live USB boot, because the marker it writes lives on the writable ext2 root.
// Absence of /CONFIG/SETUPDONE is what "unconfigured" means; nothing else.
//
// WHY THIS EXISTS. users_create_first_admin() (#745) already knew how to mint a
// proper root + uid-1000 pair, but kernel/gui/login.c is the only caller and
// every golden ships LOGIN.CFG "autologin=root", so login_check_autologin()
// takes the shortcut and that path had never run on a shipping image. The
// result was that every machine ran as root against legacy bare-hex password
// records. This wizard is the thing that finally calls it.
//
// IT WRITES NOTHING ITSELF. Every step goes through the primitive that already
// owns that piece of state - sys_user_create_pw, sys_set_autologin,
// gui_theme_activate, set_wallpaper, userconf_write_all - so a setting made
// here is byte-identical to the same setting made later in Settings. Adding a
// private config writer to this file would be a bug, not a shortcut.

#include "syscall.h"
#include "gui.h"
#include "stdio.h"
#include "string.h"
#include "userconf.h"
#include "gui_theme.h"
#include "wallpapers.h"

// Content box. win_create() is given the OUTER size, so the chrome (border +
// title bar) is added here; passing the content size clips the bottom row.
#define CONTENT_W 640
#define CONTENT_H 480
#define CHROME_W  4
#define CHROME_H  24
#define PAD       24

// ---------------------------------------------------------------------------
// Palette - same mapping idiom as Timers/Notes/Settings
// ---------------------------------------------------------------------------
static uint32_t COL_BG, COL_CARD, COL_SEP;
static uint32_t COL_TEXT, COL_TEXT2, COL_TEXT_DIM;
static uint32_t COL_ACCENT, COL_FIELD, COL_FIELD_BORDER, COL_SEL, COL_WARN;
static int g_last_theme = -1;

static void apply_theme(int kt) {
    switch (kt) {
        case 2:  // Light
            COL_BG=0x00FFFFFF; COL_CARD=0x00F4F4F4; COL_SEP=0x00CCCCCC;
            COL_TEXT=0x00202020; COL_TEXT2=0x00606060; COL_TEXT_DIM=0x00999999;
            COL_ACCENT=0x002D6CDF; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00CCCCCC;
            COL_SEL=0x00D6E4FB; COL_WARN=0x00C0392B; break;
        case 4:  // Classic
            COL_BG=0x00C0C0C0; COL_CARD=0x00D0D0D0; COL_SEP=0x00808080;
            COL_TEXT=0x00000000; COL_TEXT2=0x00404040; COL_TEXT_DIM=0x00808080;
            COL_ACCENT=0x00000080; COL_FIELD=0x00FFFFFF; COL_FIELD_BORDER=0x00000000;
            COL_SEL=0x00A0A0A0; COL_WARN=0x00A00000; break;
        case 5:  // Ocean
            COL_BG=0x00224455; COL_CARD=0x001E4050; COL_SEP=0x00406070;
            COL_TEXT=0x00E0F0FF; COL_TEXT2=0x0090B0C0; COL_TEXT_DIM=0x00607080;
            COL_ACCENT=0x0040C0E0; COL_FIELD=0x00183040; COL_FIELD_BORDER=0x00406070;
            COL_SEL=0x00305060; COL_WARN=0x00FF7060; break;
        case 9:  // Nord
            COL_BG=0x003B4252; COL_CARD=0x00343B49; COL_SEP=0x004C566A;
            COL_TEXT=0x00ECEFF4; COL_TEXT2=0x00AEB6C5; COL_TEXT_DIM=0x00707A8C;
            COL_ACCENT=0x0088C0D0; COL_FIELD=0x002B303B; COL_FIELD_BORDER=0x004C566A;
            COL_SEL=0x00434C5E; COL_WARN=0x00BF616A; break;
        default: // Dark
            COL_BG=0x00252525; COL_CARD=0x002E2E2E; COL_SEP=0x00404040;
            COL_TEXT=0x00FFFFFF; COL_TEXT2=0x00AAAAAA; COL_TEXT_DIM=0x00666666;
            COL_ACCENT=0x004A90D9; COL_FIELD=0x00333333; COL_FIELD_BORDER=0x00505050;
            COL_SEL=0x0037527A; COL_WARN=0x00E74C3C; break;
    }
    gui_set_style(kt == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface=COL_BG; p.surface_raised=COL_CARD; p.ink=COL_TEXT; p.ink_dim=COL_TEXT2;
    p.accent=COL_ACCENT; p.accent_hover=COL_ACCENT; p.border=COL_SEP;
    p.field_bg=COL_FIELD; p.field_border=COL_FIELD_BORDER; p.track=COL_SEP;
    gui_set_palette(&p);
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
enum {
    PG_WELCOME = 0, PG_ACCOUNT, PG_LOGIN, PG_NETWORK, PG_TIMEZONE,
    PG_THEME, PG_WALLPAPER, PG_AI, PG_APPLY, PG_DONE, PG_COUNT
};

static const char *PAGE_TITLE[PG_COUNT] = {
    "Welcome to MayteraOS", "Create your account", "Signing in",
    "Network", "Date & Time", "Appearance", "Desktop picture",
    "AI assistant", "Setting up", "You're all set"
};

static int g_window = -1;
static int g_page   = PG_WELCOME;
static char g_error[96];

// ---------------------------------------------------------------------------
// Text fields
// ---------------------------------------------------------------------------
#define FLD_MAX 64
typedef struct {
    char  buf[FLD_MAX];
    int   mask;          // render as bullets
    const char *label;
    const char *hint;
} field_t;

// Account
static field_t f_fullname = { "", 0, "Full name",   "e.g. Ada Lovelace" };
static field_t f_username = { "", 0, "Username",    "lowercase, no spaces" };
static field_t f_password = { "", 1, "Password",    "at least 6 characters" };
static field_t f_confirm  = { "", 1, "Confirm",     "type it again" };
// Network (static only; DHCP needs no fields)
static field_t f_ip   = { "", 0, "IP address", "192.0.2.1" };
static field_t f_mask = { "", 0, "Netmask",    "255.255.255.0" };
static field_t f_gw   = { "", 0, "Gateway",    "192.0.2.1" };
static field_t f_dns  = { "", 0, "DNS",        "192.0.2.1" };
// AI
static field_t f_aikey = { "", 1, "API key", "leave blank to skip" };

static field_t *g_fields[6];   // fields on the current page, in tab order
static int      g_field_count = 0;
static int      g_focus = 0;

// Choices
static int g_require_password = 1;   // 0 = autologin
static int g_use_dhcp         = 1;
static int g_tz_idx           = 12;  // UTC
static int g_theme_idx        = 0;
static int g_wp_idx           = 0;

// Timezone table: the same "UTC+HH:MM" representation Settings already uses, so
// the two agree on what a zone is called.
static const char *TZ[] = {
    "UTC-11:00","UTC-10:00","UTC-09:00","UTC-08:00","UTC-07:00","UTC-06:00",
    "UTC-05:00","UTC-04:00","UTC-03:00","UTC-02:00","UTC-01:00","UTC",
    "UTC+01:00","UTC+02:00","UTC+03:00","UTC+03:30","UTC+04:00","UTC+05:00",
    "UTC+05:30","UTC+06:00","UTC+07:00","UTC+08:00","UTC+09:00","UTC+09:30",
    "UTC+10:00","UTC+12:00"
};
#define TZ_COUNT ((int)(sizeof(TZ)/sizeof(TZ[0])))

static gui_theme_entry_t g_themes[GUI_THEME_MAX_ENTRIES];
static int g_theme_count = 0;
static wp_entry_t g_wps[WP_MAX_ENTRIES];
static int g_wp_count = 0;

// ---------------------------------------------------------------------------
// Small drawing helpers
// ---------------------------------------------------------------------------
static void text(int x, int y, const char *s, int sz, uint32_t c) {
    win_draw_text_ttf(g_window, x, y, s, sz, c);
}

static void draw_field(field_t *f, int x, int y, int w, int focused) {
    text(x, y, f->label, 12, COL_TEXT2);
    int by = y + 18, bh = 30;
    win_draw_rect(g_window, x, by, w, bh, COL_FIELD);
    // (#745) the focus ring is gui_pal()->focus, which gui_set_palette()
    // guarantees clears 3:1 against the surface; COL_ACCENT does not (1.76:1
    // on Dark). The first-run wizard is keyboard-driven, so this is the exact
    // case where an invisible ring is a functional defect (#334).
    uint32_t bc = focused ? gui_pal()->focus : COL_FIELD_BORDER;
    win_draw_rect(g_window, x, by, w, 1, bc);
    win_draw_rect(g_window, x, by + bh - 1, w, 1, bc);
    win_draw_rect(g_window, x, by, 1, bh, bc);
    win_draw_rect(g_window, x + w - 1, by, 1, bh, bc);

    int len = (int)strlen(f->buf);
    if (len == 0 && !focused) {
        text(x + 8, by + 8, f->hint, 12, COL_TEXT_DIM);
    } else if (f->mask) {
        char bul[FLD_MAX + 1];
        int n = len < FLD_MAX ? len : FLD_MAX;
        for (int i = 0; i < n; i++) bul[i] = '*';
        bul[n] = 0;
        text(x + 8, by + 8, bul, 13, COL_TEXT);
    } else {
        text(x + 8, by + 8, f->buf, 13, COL_TEXT);
    }
    if (focused) {
        const char *shown = f->mask ? "" : f->buf;
        int cw = f->mask ? (len * 8) : gui_ttf_width(shown, 13);
        win_draw_rect(g_window, x + 8 + cw + 1, by + 6, 2, bh - 12, COL_ACCENT);
    }
}

// A radio-style row. Returns its height.
static int draw_choice(int x, int y, int w, const char *label,
                       const char *sub, int selected) {
    int h = sub ? 46 : 32;
    if (selected) gui_fill_rounded_aa(g_window, x, y, w, h, 6, COL_SEL, COL_BG);
    int cy = y + (sub ? 14 : 10);
    gui_fill_circle_aa(g_window, x + 14, cy + 2, 7,
                       selected ? COL_ACCENT : COL_FIELD, COL_BG);
    if (selected) gui_fill_circle_aa(g_window, x + 14, cy + 2, 3, COL_BG, COL_ACCENT);
    text(x + 32, y + 8, label, 13, COL_TEXT);
    if (sub) text(x + 32, y + 26, sub, 11, COL_TEXT_DIM);
    return h;
}

// A scrolling-free list box (all our lists fit); returns selected index or -1.
static void draw_list(int x, int y, int w, int h, int count, int sel,
                      const char *(*name_of)(int)) {
    gui_card(g_window, x, y, w, h);
    int rows = h / 26;
    int first = 0;
    if (sel >= rows) first = sel - rows + 1;
    for (int i = 0; i < rows && first + i < count; i++) {
        int idx = first + i, ry = y + 4 + i * 26;
        if (idx == sel)
            gui_fill_rounded_aa(g_window, x + 4, ry, w - 8, 24, 4, COL_SEL, COL_CARD);
        text(x + 14, ry + 5, name_of(idx), 12,
             idx == sel ? COL_TEXT : COL_TEXT2);
    }
}

static const char *tz_name(int i)    { return TZ[i]; }
static const char *theme_name(int i) { return g_themes[i].name; }
static const char *wp_name(int i)    { return g_wps[i].name; }

// ---------------------------------------------------------------------------
// Page rendering
// ---------------------------------------------------------------------------
static void set_fields(field_t *a, field_t *b, field_t *c, field_t *d) {
    g_field_count = 0;
    if (a) g_fields[g_field_count++] = a;
    if (b) g_fields[g_field_count++] = b;
    if (c) g_fields[g_field_count++] = c;
    if (d) g_fields[g_field_count++] = d;
    if (g_focus >= g_field_count) g_focus = 0;
}

static void draw_page_body(int x, int y, int w) {
    switch (g_page) {
    case PG_WELCOME:
        set_fields(0,0,0,0);
        text(x, y, "This assistant sets up your computer.", 14, COL_TEXT);
        text(x, y + 30, "You will create an account, choose how you sign in,", 12, COL_TEXT2);
        text(x, y + 50, "and set your network, appearance and AI assistant.", 12, COL_TEXT2);
        text(x, y + 86, "Nothing is written until the last step.", 12, COL_TEXT_DIM);
        break;

    case PG_ACCOUNT: {
        set_fields(&f_fullname, &f_username, &f_password, &f_confirm);
        int half = (w - 16) / 2;
        draw_field(&f_fullname, x, y, w, g_focus == 0);
        draw_field(&f_username, x, y + 62, w, g_focus == 1);
        draw_field(&f_password, x, y + 124, half, g_focus == 2);
        draw_field(&f_confirm, x + half + 16, y + 124, half, g_focus == 3);
        text(x, y + 190, "This account can administer the computer.", 11, COL_TEXT_DIM);
        text(x, y + 208, "A separate 'root' system account is created for you.", 11, COL_TEXT_DIM);
        break;
    }

    case PG_LOGIN: {
        set_fields(0,0,0,0);
        int cy = y;
        cy += draw_choice(x, cy, w, "Require my password to sign in",
                          "Recommended. The desktop locks when idle.", g_require_password) + 8;
        draw_choice(x, cy, w, "Sign me in automatically",
                    "Anyone who can power on this machine gets your session.",
                    !g_require_password);
        break;
    }

    case PG_NETWORK: {
        int cy = y;
        cy += draw_choice(x, cy, w, "Configure automatically (DHCP)",
                          "Recommended for most networks.", g_use_dhcp) + 6;
        cy += draw_choice(x, cy, w, "Use a static address", 0, !g_use_dhcp) + 10;
        if (!g_use_dhcp) {
            set_fields(&f_ip, &f_mask, &f_gw, &f_dns);
            int half = (w - 16) / 2;
            draw_field(&f_ip,   x,             cy, half, g_focus == 0);
            draw_field(&f_mask, x + half + 16, cy, half, g_focus == 1);
            draw_field(&f_gw,   x,             cy + 62, half, g_focus == 2);
            draw_field(&f_dns,  x + half + 16, cy + 62, half, g_focus == 3);
        } else {
            set_fields(0,0,0,0);
        }
        break;
    }

    case PG_TIMEZONE:
        set_fields(0,0,0,0);
        text(x, y, "Choose your time zone.", 12, COL_TEXT2);
        draw_list(x, y + 24, w, 210, TZ_COUNT, g_tz_idx, tz_name);
        break;

    case PG_THEME:
        set_fields(0,0,0,0);
        text(x, y, "Pick a look. You can change it later in Settings.", 12, COL_TEXT2);
        if (g_theme_count > 0)
            draw_list(x, y + 24, w, 210, g_theme_count, g_theme_idx, theme_name);
        else
            text(x, y + 40, "No themes found in /THEMES/INDEX.TXT.", 12, COL_WARN);
        break;

    case PG_WALLPAPER:
        set_fields(0,0,0,0);
        text(x, y, "Choose a desktop picture.", 12, COL_TEXT2);
        if (g_wp_count > 0)
            draw_list(x, y + 24, w, 210, g_wp_count, g_wp_idx, wp_name);
        else
            text(x, y + 40, "No wallpapers found.", 12, COL_WARN);
        break;

    case PG_AI: {
        set_fields(&f_aikey, 0, 0, 0);
        text(x, y, "MayteraOS can use a hosted AI assistant.", 13, COL_TEXT);
        text(x, y + 24, "Paste an API key to enable AI Chat and the AI panel.", 12, COL_TEXT2);
        draw_field(&f_aikey, x, y + 56, w, g_focus == 0);
        text(x, y + 122, "Stored in your own home directory, not shared with", 11, COL_TEXT_DIM);
        text(x, y + 138, "other users. You can add or change it later in Settings.", 11, COL_TEXT_DIM);
        break;
    }

    case PG_APPLY:
        set_fields(0,0,0,0);
        text(x, y, "Applying your settings...", 14, COL_TEXT);
        gui_progress(g_window, x, y + 40, w, 10, 50);
        break;

    case PG_DONE:
        set_fields(0,0,0,0);
        text(x, y, "Setup is complete.", 15, COL_TEXT);
        text(x, y + 34, "Your account has been created and your preferences saved.", 12, COL_TEXT2);
        if (g_require_password)
            text(x, y + 58, "You will be asked for your password when you sign in.", 12, COL_TEXT2);
        text(x, y + 94, "Click Finish to start using MayteraOS.", 12, COL_TEXT_DIM);
        break;
    }
}

// Nav buttons
enum { NAV_NONE = 0, NAV_BACK, NAV_NEXT };
static int g_hover_nav = NAV_NONE;

static void nav_rects(int *bx, int *by, int *bw, int *bh, int *nx) {
    *bw = 110; *bh = 34;
    *by = CONTENT_H - PAD - *bh;
    *bx = PAD;
    *nx = CONTENT_W - PAD - *bw;
}

static const char *next_label(void) {
    if (g_page == PG_WELCOME) return "Continue";
    if (g_page == PG_AI)      return "Set Up";
    if (g_page == PG_DONE)    return "Finish";
    return "Continue";
}

static void draw_all(void) {
    win_draw_rect(g_window, 0, 0, CONTENT_W, CONTENT_H, COL_BG);

    // Header
    text(PAD, PAD, PAGE_TITLE[g_page], 20, COL_TEXT);
    win_draw_rect(g_window, PAD, PAD + 36, CONTENT_W - 2 * PAD, 1, COL_SEP);

    // Step dots (welcome and the terminal pages are not "steps")
    if (g_page > PG_WELCOME && g_page < PG_APPLY) {
        int total = PG_APPLY - PG_ACCOUNT;
        for (int i = 0; i < total; i++) {
            uint32_t c = (i == g_page - PG_ACCOUNT) ? COL_ACCENT : COL_SEP;
            gui_fill_circle_aa(g_window, CONTENT_W - PAD - 8 - i * 16, PAD + 12, 4, c, COL_BG);
        }
    }

    draw_page_body(PAD, PAD + 60, CONTENT_W - 2 * PAD);

    if (g_error[0]) {
        text(PAD, CONTENT_H - PAD - 62, g_error, 12, COL_WARN);
    }

    int bx, by, bw, bh, nx;
    nav_rects(&bx, &by, &bw, &bh, &nx);
    if (g_page != PG_WELCOME && g_page != PG_APPLY && g_page != PG_DONE) {
        gui_button(g_window, bx, by, bw, bh, "Back", GUI_BTN_SECONDARY,
                   g_hover_nav == NAV_BACK ? GUI_ST_HOVER : GUI_ST_NORMAL);
    }
    if (g_page != PG_APPLY) {
        gui_button(g_window, nx, by, bw, bh, next_label(), GUI_BTN_PRIMARY,
                   g_hover_nav == NAV_NEXT ? GUI_ST_HOVER : GUI_ST_NORMAL);
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
static int is_blank(const char *s) {
    for (int i = 0; s[i]; i++) if (s[i] != ' ') return 0;
    return 1;
}

static int validate_page(void) {
    g_error[0] = 0;
    if (g_page == PG_ACCOUNT) {
        if (is_blank(f_fullname.buf)) { snprintf(g_error, sizeof(g_error), "Enter your full name."); return 0; }
        if (is_blank(f_username.buf)) { snprintf(g_error, sizeof(g_error), "Enter a username."); return 0; }
        for (int i = 0; f_username.buf[i]; i++) {
            char c = f_username.buf[i];
            if (c == ':' || c == ' ' || c < 33 || c > 126) {
                snprintf(g_error, sizeof(g_error), "Username cannot contain spaces or ':'.");
                return 0;
            }
        }
        // users_create_first_admin() reserves this name; say so here rather
        // than letting the syscall fail with a bare -2 at the last step.
        if (strcmp(f_username.buf, "root") == 0) {
            snprintf(g_error, sizeof(g_error), "'root' is reserved. Choose another username.");
            return 0;
        }
        if ((int)strlen(f_password.buf) < 6) {
            snprintf(g_error, sizeof(g_error), "Password must be at least 6 characters.");
            return 0;
        }
        if (strcmp(f_password.buf, f_confirm.buf) != 0) {
            snprintf(g_error, sizeof(g_error), "Passwords do not match.");
            return 0;
        }
        return 1;
    }
    if (g_page == PG_NETWORK && !g_use_dhcp) {
        if (is_blank(f_ip.buf))   { snprintf(g_error, sizeof(g_error), "Enter an IP address, or choose DHCP."); return 0; }
        if (is_blank(f_mask.buf)) { snprintf(g_error, sizeof(g_error), "Enter a netmask."); return 0; }
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Apply. Ordered so the account exists before anything per-user is written:
// userconf_* resolves paths under the CURRENT session's home, so a key written
// before the account exists would land in the wrong place.
// ---------------------------------------------------------------------------
static int apply_settings(void) {
    // uid 0 / gid 0 / home NULL all mean "you decide", and that is deliberate.
    // The SYS_USER_CREATE_PW handler owns the uid allocator precisely because
    // callers that computed their own collided (Settings used 1000 + user_count).
    // A hardcoded 1000 here returns -1 on any shipped image, because the asset
    // base already ships an account at that uid. Letting the kernel derive the
    // home path too keeps it byte-identical to users_create_first_admin's
    // uppercased /HOME/<NAME8>, which matters because ext2 is case-sensitive.
    int uid = sys_user_create_pw(f_username.buf, f_password.buf, 0, 0, 0);
    if (uid < 0) {
        if (uid == -2)      snprintf(g_error, sizeof(g_error), "That username is not allowed.");
        else if (uid == -3) snprintf(g_error, sizeof(g_error), "Password is too short.");
        else                snprintf(g_error, sizeof(g_error), "Could not create the account (%d).", uid);
        return 0;
    }

    // Sign-in mode. Only an explicit opt-in turns autologin on; the shipped
    // default of autologin=root is exactly what this wizard exists to undo.
    int rc = sys_set_autologin(f_username.buf, f_password.buf, g_require_password ? 0 : 1);
    if (rc != 0) snprintf(g_error, sizeof(g_error), "Note: sign-in preference could not be saved.");

    // Network. DHCP is the absence of NETIP.CFG (#574), so nothing is written.
    if (!g_use_dhcp) {
        char buf[192];
        int n = snprintf(buf, sizeof(buf), "ip=%s\nmask=%s\n", f_ip.buf, f_mask.buf);
        if (!is_blank(f_gw.buf))  n += snprintf(buf + n, sizeof(buf) - n, "gw=%s\n", f_gw.buf);
        if (!is_blank(f_dns.buf)) n += snprintf(buf + n, sizeof(buf) - n, "dns=%s\n", f_dns.buf);
        if (userconf_write_all("/CONFIG/NETIP.CFG", buf, (unsigned long)n) != 0)
            snprintf(g_error, sizeof(g_error), "Note: network settings could not be saved.");
    }

    // Time zone.
    {
        char buf[48];
        int n = snprintf(buf, sizeof(buf), "tz=%s\n", TZ[g_tz_idx]);
        if (userconf_write_all("/CONFIG/TZ.CFG", buf, (unsigned long)n) != 0)
            snprintf(g_error, sizeof(g_error), "Note: time zone could not be saved.");
    }

    // Appearance. gui_theme_activate() both applies and persists.
    if (g_theme_count > 0 && g_theme_idx < g_theme_count)
        gui_theme_activate(g_themes[g_theme_idx].slug);
    if (g_wp_count > 0 && g_wp_idx < g_wp_count)
        set_wallpaper(g_wp_idx);

    // AI key, only if one was given. Same AISVC.CFG shape Settings writes.
    if (!is_blank(f_aikey.buf)) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf), "provider=moonshot\nkey=%s\n", f_aikey.buf);
        int fd = userconf_open_write("AISVC.CFG");
        if (fd < 0 || userconf_finish_write(fd, buf, (unsigned long)n) != 0)
            snprintf(g_error, sizeof(g_error), "Note: AI key could not be saved.");
    }

    // The marker goes LAST and only if the account exists. If anything above
    // failed hard we returned already, so setup re-runs on the next boot
    // rather than leaving a machine with no way back into this wizard.
    {
        const char *done = "1\n";
        if (userconf_write_all("/CONFIG/SETUPDONE", done, 2) != 0) {
            snprintf(g_error, sizeof(g_error), "Could not save setup state. It will run again.");
            return 0;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void go_next(void) {
    if (!validate_page()) return;
    if (g_page == PG_AI) {
        g_page = PG_APPLY;
        draw_all();
        g_page = apply_settings() ? PG_DONE : PG_ACCOUNT;
        g_focus = 0;
        return;
    }
    if (g_page == PG_DONE) return;   // handled by caller (exit)
    if (g_page < PG_AI) { g_page++; g_focus = 0; g_error[0] = 0; }
}

static void go_back(void) {
    if (g_page > PG_WELCOME && g_page <= PG_AI) { g_page--; g_focus = 0; g_error[0] = 0; }
}

static void on_key(gui_event_t *ev) {
    char c = ev->key_char;

    if (c == '\t') {
        if (g_field_count > 0) g_focus = (g_focus + 1) % g_field_count;
        return;
    }
    if (c == '\r' || c == '\n') { go_next(); return; }

    // List pages: up/down arrive as control chars in this build; also accept
    // '-'/'+' so the wizard is driveable from a serial-only console.
    if (g_page == PG_TIMEZONE || g_page == PG_THEME || g_page == PG_WALLPAPER) {
        int *sel = (g_page == PG_TIMEZONE) ? &g_tz_idx
                 : (g_page == PG_THEME)    ? &g_theme_idx : &g_wp_idx;
        int max  = (g_page == PG_TIMEZONE) ? TZ_COUNT
                 : (g_page == PG_THEME)    ? g_theme_count : g_wp_count;
        if (max <= 0) return;
        if (ev->keycode == 0x48 || c == '-') { if (*sel > 0) (*sel)--; return; }
        if (ev->keycode == 0x50 || c == '+') { if (*sel < max - 1) (*sel)++; return; }
        return;
    }
    if (g_page == PG_LOGIN)   { if (c == ' ') g_require_password = !g_require_password; return; }
    if (g_page == PG_NETWORK && g_field_count == 0) { if (c == ' ') g_use_dhcp = !g_use_dhcp; return; }

    if (g_field_count == 0) return;
    field_t *f = g_fields[g_focus];
    int len = (int)strlen(f->buf);
    if (c == '\b') { if (len > 0) f->buf[len - 1] = 0; return; }
    if (c >= 32 && c < 127 && len < FLD_MAX - 1) { f->buf[len] = c; f->buf[len + 1] = 0; }
}

static void on_click(int mx, int my) {
    int bx, by, bw, bh, nx;
    nav_rects(&bx, &by, &bw, &bh, &nx);
    if (my >= by && my <= by + bh) {
        if (mx >= nx && mx <= nx + bw) { go_next(); return; }
        if (mx >= bx && mx <= bx + bw && g_page != PG_WELCOME) { go_back(); return; }
    }

    int x = PAD, y = PAD + 60, w = CONTENT_W - 2 * PAD;

    if (g_page == PG_LOGIN) {
        if (my >= y && my < y + 46) g_require_password = 1;
        else if (my >= y + 54 && my < y + 100) g_require_password = 0;
        return;
    }
    if (g_page == PG_NETWORK) {
        if (my >= y && my < y + 46) g_use_dhcp = 1;
        else if (my >= y + 52 && my < y + 84) g_use_dhcp = 0;
        return;
    }
    if (g_page == PG_TIMEZONE || g_page == PG_THEME || g_page == PG_WALLPAPER) {
        int ly = y + 24;
        if (my >= ly && my < ly + 210) {
            int row = (my - ly - 4) / 26;
            int *sel = (g_page == PG_TIMEZONE) ? &g_tz_idx
                     : (g_page == PG_THEME)    ? &g_theme_idx : &g_wp_idx;
            int max  = (g_page == PG_TIMEZONE) ? TZ_COUNT
                     : (g_page == PG_THEME)    ? g_theme_count : g_wp_count;
            int rows = 210 / 26, first = 0;
            if (*sel >= rows) first = *sel - rows + 1;
            if (first + row < max) *sel = first + row;
        }
        return;
    }
    // Focus a text field by clicking its box.
    for (int i = 0; i < g_field_count; i++) {
        (void)i;
    }
    if (g_page == PG_ACCOUNT) {
        if (my >= y + 18 && my < y + 48)       g_focus = 0;
        else if (my >= y + 80 && my < y + 110) g_focus = 1;
        else if (my >= y + 142 && my < y + 172)
            g_focus = (mx < x + w / 2) ? 2 : 3;
    } else if (g_page == PG_AI) {
        if (my >= y + 74 && my < y + 104) g_focus = 0;
    }
    (void)x;
}

// ---------------------------------------------------------------------------
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    g_last_theme = get_theme();
    apply_theme(g_last_theme);

    g_theme_count = gui_theme_list(g_themes, GUI_THEME_MAX_ENTRIES);
    g_wp_count    = wp_enumerate(g_wps, WP_MAX_ENTRIES);

    g_window = win_create("Setup Assistant", 120, 60,
                          CONTENT_W + CHROME_W, CONTENT_H + CHROME_H);
    if (g_window < 0) { printf("setup: failed to create window\n"); return 1; }

    draw_all();

    gui_event_t ev;
    int running = 1;
    while (running) {
        int th = get_theme();
        if (th != g_last_theme) { g_last_theme = th; apply_theme(th); draw_all(); }

        int et = win_get_event(g_window, &ev, 250);
        if (et == 0) continue;

        switch (ev.type) {
        case EVENT_REDRAW: draw_all(); break;
        case EVENT_KEY_DOWN:
            if (g_page == PG_DONE && (ev.key_char == '\r' || ev.key_char == '\n')) {
                running = 0; break;
            }
            on_key(&ev); draw_all(); break;
        case EVENT_MOUSE_DOWN:
            if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) {
                if (g_page == PG_DONE) {
                    int bx, by, bw, bh, nx;
                    nav_rects(&bx, &by, &bw, &bh, &nx);
                    if (ev.mouse_y >= by && ev.mouse_y <= by + bh &&
                        ev.mouse_x >= nx && ev.mouse_x <= nx + bw) { running = 0; break; }
                }
                on_click(ev.mouse_x, ev.mouse_y);
                draw_all();
            }
            break;
        case EVENT_MOUSE_MOVE: {
            int bx, by, bw, bh, nx;
            nav_rects(&bx, &by, &bw, &bh, &nx);
            int nv = NAV_NONE;
            if (ev.mouse_y >= by && ev.mouse_y <= by + bh) {
                if (ev.mouse_x >= nx && ev.mouse_x <= nx + bw) nv = NAV_NEXT;
                else if (ev.mouse_x >= bx && ev.mouse_x <= bx + bw) nv = NAV_BACK;
            }
            if (nv != g_hover_nav) { g_hover_nav = nv; draw_all(); }
            break;
        }
        // No EVENT_WINDOW_CLOSE case: setup is not dismissable. Closing it
        // would leave a machine with no account and no way back to this
        // wizard except a reboot.
        default: break;
        }
    }

    win_destroy(g_window);
    return 0;
}
