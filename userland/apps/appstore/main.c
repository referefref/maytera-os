// appstore - MayteraOS App Repo (task #402; renamed from "App Store" #200)
//
// NAME SPLIT, stated here so nobody has to guess which name is authoritative.
// The USER-VISIBLE DISPLAY NAME is "App Repo" (#200, owner request): the window
// title, the header heading, the Start-menu/desktop/launcher labels and the
// status sentences below. Everything else deliberately still says appstore:
// the binary (userland/apps/appstore -> /APPS/APPSTORE), the icon id
// (icon=appstore), the server side (server/appstore, updates.maytera.net,
// /APPS/STORE.SRC, the SIGNED manifest that is the install trust anchor), and
// the comments throughout this tree that name the COMPONENT rather than the
// label. Renaming the binary would walk into the #196/#517 install-name
// resolver trap (build-golden.sh invents an install name when its lookups
// miss - that is how /APPS/ENVPROBE never existed for months) and the server
// name is covered by a signature. So: DISPLAY = App Repo, COMPONENT = appstore.
//
// A modern, comprehensive software store for MayteraOS built on top of the
// #97 package/update server (the .mpkg repo served over HTTP). It fetches the
// repository manifest, presents a Discover hero + category-filtered card grid +
// live search + a rich detail page with a screenshot gallery, and installs /
// updates / launches packages. All rendering uses the native MayteraOS style
// engine (gui_style primitives, theme_color palette, antialiased TTF text) so
// the app recolors with the active theme and feels first-class, while the
// layout follows the Apple App Store / KDE Discover / GNOME Software patterns.
//
// Transport is NOT reinvented here: catalog + packages come from the existing
// repo (manifest.json + <id>-<ver>.mpkg tar.gz), fetched via SYS_HTTP_FETCH and
// unpacked with the shared libarchive (arc_targz_extract). Installed state and
// versions are tracked in /APPS/STORE.DB; installed apps are registered into
// the Start menu's live all-users config layer via startmenu_register_app()
// (userland/libc/startmenu_reg.c) - see that file's header for why this
// replaced an /APPS/REGINI.CFG write that reached no reader.
//
// #B2 (App Store Phase B2): the B1 server (docs/APPSTORE_SERVER.md) added a
// richer catalog (type, tags[], summary, preview_images[]) and an unsigned
// social layer (/api/catalog, /api/item/<id>, download + rating POSTs). This
// phase consumes both: the manifest stays the ONLY thing that gates trust
// (signature + per-package sha256, unchanged), and the /api/ stats are merged
// in purely for display - a wrong download count or rating can never install
// code, so they are read with the same tolerant JSON helpers used for the
// manifest but never fed into pkgsig_verify_package().

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_scroll.h"   // (#96) shared scrollbar: this app never had one
#include "../../libc/syscall.h"
#include "../../libc/theme.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/fcntl.h"
#include "../../libc/unistd.h"
#include "../../libc/wallpapers.h"
#include "../../libc/gui_theme.h"    // (#565) file-based theme loader
#include "arc.h"
#include "../../libc/pkgsig.h"
#include "../../libc/sha256.h"    // #570: incremental hashing for streamed downloads
#include "../../libc/unistd.h"     // #607 lseek for the on-disk size check
#include "../../libc/startmenu_reg.h"   // #<startmenu-rust> live Start-menu registration
#include "../../libc/userconf.h"   // #745 per-user writable scratch path
#include "../../libc/pkgdest.h"    // #745 per-user install destination confinement
#include "../../libc/pwd.h"        // #745 the session user's name, for the UI

// #<startmenu-rust>: SYS_DESKTOP_MENU_RELOAD (syscall 300, #402) used to be
// called here after every install. It resolves to desktop_menu_reload() in
// kernel/gui/desktop.c, which is a documented no-op ("the kernel start menu
// is gone... now a no-op") - the userland compositor's Start menu has never
// listened for it. Removed rather than kept as a harmless-looking call:
// startmenu_register_app() (startmenu_reg.c) below writes straight into the
// live config directory the compositor's startmenu_rust_poll() throttle-polls
// (userland/apps/compositor/startmenu.c), which is what actually makes an
// install show up without a restart.

// Package-manager write to the FAT ESP: userland cannot open /APPS files for
// writing (the kernel routes those opens to the ext2 root), so the kernel does
// the FAT write via fat_write_file behind SYS_PKG_WRITE. Returns 0 on success.
#ifndef SYS_PKG_WRITE
#define SYS_PKG_WRITE 301
#endif
static inline int pkg_write(const char *path, const void *data, unsigned len) {
    return (int)syscall3(SYS_PKG_WRITE, (long)path, (long)data, (long)len);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
/* The repository is addressed by HOSTNAME, never by a LAN IP: an IP literal
 * only ever resolves from one network, which is why a client shipped with an
 * internal address baked in would report "could not reach the app
 * repository" on any machine that was not on that one LAN. This client has
 * never hardcoded the internal dev/social server (the build container, 192.0.2.1) -
 * that address only ever appears below in a comment, as an example of what
 * an *optional* /APPS/STORE.SRC override can point at for local testing.
 *
 * The repo base is HTTPS by default (updates.maytera.net sits behind
 * Cloudflare). https is REQUIRED for the /api/ social endpoints:
 * sys_http_post_start() in the kernel refuses any POST whose URL does not
 * start with "https://", so with an http:// repo base the rating/download-
 * count POSTs (bump_download/submit_rating) fail silently. #590: the large-
 * package chunked Range download now stays on https too - #576 taught the
 * kernel's sys_http_fetch_hdr to dispatch to the TLS client (https_get_hdr)
 * for an https:// URL, so the old repo_url_http() https->http downgrade
 * workaround is gone and every fetch here (manifest, small + large packages,
 * /api/ POSTs) goes out over the real https g_repo. */

/* #559: detached signature over manifest.json. The manifest is the TRUST
 * ANCHOR (the Debian apt model): this signature authenticates the manifest,
 * and the manifest's per-package sha256 then covers every .mpkg, so one
 * signature protects many artifacts. Verified in the kernel against a public
 * key compiled into the kernel image, so Ring-3 cannot swap the key out.
 * Fail closed at every step: there is no unverified fallback. */

// Route in-window text through the antialiased TrueType path.
static int g_win = -1;
static int g_win_w = 980, g_win_h = 680;

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------
static char    g_manifest[128 * 1024];        // manifest.json text
// #570: package acquisition buffers. Small packages (<= DL_SMALL_MAX) still use
// the proven single-fetch-into-a-static-buffer path. The old buffer was a fixed
// 3MB (g_dl[3*1024*1024]); nothing over the kernel's 1MB per-fetch cap could
// ever be fetched into it anyway, so it is trimmed to the real one-fetch regime.
// Larger packages take the chunked Range path below, whose peak RAM is one
// DL_CHUNK regardless of package size (see acquire_verified_package()).
#define DL_SMALL_MAX  (1000 * 1024)            // one-fetch ceiling (kernel WGET_BUFFER_SIZE is 1MB incl. headers)
#define DL_CHUNK      (256 * 1024)             // Range chunk: well inside the ~1MB fetch cap and the reliable small-response regime (#420/#426)
// #745 THE DOWNLOAD SCRATCH FILE IS NOT A SYSTEM FILE, SO IT DOES NOT LIVE IN
// A SYSTEM DIRECTORY. It used to be the compile-time literal "/STOREDL.TMP",
// i.e. the FILESYSTEM ROOT, which the shipped /CONFIG/PERMS.DB records as
// "/:0:0:0755". Since #676 an O_CREAT of a path that does not exist is checked
// as a WRITE TO THE PARENT DIRECTORY, so creating anything directly in "/"
// requires uid 0. Every test of this code ran on the shipped autologin=root
// image and passed; the moment the #745 first-boot wizard started creating a
// NON-root desktop account (sys_user_create_pw with uid 0 meaning "you decide",
// which allocates a uid ABOVE 0), the very first step of every large install
// returned -EACCES and the store said "Cannot open download temp file".
// MEASURED on a uid-1000 session, golden 1798:
//     [PERMS-DENY] proc=APPSTORE uid=1000 gid=1000 want=-wx path=/
//     open("/STOREDL.TMP", O_WRONLY|O_CREAT|O_TRUNC=0x241) = -13
//
// The fix is the SAME one userconf.c (#683) already states for the desktop's
// own preference files: relocating a file the user owns REMOVES the need for a
// privilege instead of granting it. The scratch goes in the session user's
// home, via the shared userhome_path() join rather than a private copy of it.
// ROOT IS A NO-OP BY CONSTRUCTION: /CONFIG/PASSWD gives root the home "/", so a
// root session still uses exactly "/STOREDL.TMP" and its behaviour is unchanged.
static const char *store_tmp_path(void) {
    static char p[192];
    if (p[0] == 0) {
        if (userhome_path(0, "STOREDL.TMP", p, sizeof(p)) != 0) {
            // Only reachable if the home path does not fit, and a truncated
            // path is a different file. Fall back to the historical location
            // rather than writing somewhere unintended; the open below reports
            // the real errno if that is not writable either.
            strcpy(p, "/STOREDL.TMP");
        }
    }
    return p;
}
#define STORE_TMP_PATH store_tmp_path()        // scratch for a streamed download

// ---------------------------------------------------------------------------
// #745 PER-USER INSTALL (flow A of docs/APPSTORE_PER_USER_INSTALL_DESIGN.md).
//
// THE POINT, and it is a security point, not a convenience one: installing an
// app INTO YOUR OWN PROFILE IS NOT A PRIVILEGE TRANSITION, so it does not
// prompt. A consent dialog that appears every time and is approved every time
// trains the user to click through the one that actually matters. There is no
// dialog on this path and there must never be one.
//
// The store was never refusing anything. Every failure was a kernel
// perms_check() denial surfaced as -13, because every destination in every
// package manifest is a root-owned system directory:
//
//     [PERMS-DENY] proc=APPSTORE uid=1000 gid=1000 want=-w- path=/APPS
//     [PERMS-DENY] proc=APPSTORE uid=1000 gid=1000 want=-w- path=/GAMES
//
// The fix is NOT to loosen those modes. A desktop session that can rewrite
// /APPS/MSH owns the next root login. The fix is that a non-root user has
// somewhere of their own to install into, which since #745 is <home>/APPS,
// created by users_make_home_skeleton() and owned by them at 0750. Relocating
// the destination REMOVES the need for the privilege instead of granting it,
// the same rule userconf.c (#683) already states for preferences.
//
// ONE CODE PATH, NOT TWO. The sandbox is the session user's home, and root's
// home is "/" (see /CONFIG/PASSWD), so for a root session pkgdest_confine()
// rewrites nothing and every destination is byte-identical to the manifest's
// own. There is no "if root" branch in the destination logic to get wrong.
// g_sysscope exists only to choose WHICH registry and WHICH Start-menu layer to
// write, both of which genuinely differ.
static char g_home[PKGDEST_MAX];        // the sandbox root: "<home>", "/" for root
static char g_home_apps[PKGDEST_MAX];   // "<home>/APPS": where a per-user app lands
static int  g_sysscope;                 // 1 = uid 0, so system registry + system menu

// #745 FLOW B: SYSTEM-WIDE INSTALL.
//
// The SESSION scope above never changes. What changes for one install is the
// scope that install writes with, so there is a second, EXPLICIT set of
// "active install scope" variables. Reusing g_sysscope for both would have made
// "which account am I" and "what am I installing right now" the same variable,
// and the first thing that goes wrong then is that the Start-menu layer and the
// registry silently follow the wrong one.
//
// g_ins_* is the session scope for an ordinary install, and exactly ("/",
// "/APPS", 1) for the duration of an elevated one.
static char g_ins_home[PKGDEST_MAX];
static char g_ins_apps[PKGDEST_MAX];
static int  g_ins_sys;

// May THIS account be offered the elevation prompt at all? One boolean from the
// kernel (SYS_ELEV_MAY), asked once. It decides Surface A vs Surface C, and it
// is asked about the caller and nobody else, so it discloses nothing about
// which accounts exist. Root gets 1 and is never prompted: the kernel refuses
// to raise a prompt for uid 0 (ELEV_EROOT), because root already IS the
// privilege and a prompt there would be the most-fired, least-meaningful
// dialog in the OS.
static int  g_may_elevate;

// Keyboard focus on the detail page's action cluster. 0 = the primary
// (Install/Open/Update), 1 = "Install for all users...". A DISABLED secondary
// is SKIPPED, so it can never be focused and therefore can never be activated
// into a refusal.
static int  g_detail_focus;

static void scope_init(void) {
    if (userhome_root(g_home, sizeof(g_home)) != 0) {
        // Only reachable if the home path does not fit. Fall back to "/", which
        // confines nothing and therefore behaves exactly as this app did before
        // #745: the kernel's own permission check is then the control, and it
        // will refuse a non-root write to /APPS with the -13 the status line
        // now explains properly.
        g_home[0] = '/'; g_home[1] = 0;
    }
    if (userhome_path(0, "APPS", g_home_apps, sizeof(g_home_apps)) != 0)
        strcpy(g_home_apps, "/APPS");
    g_sysscope = (getuid() == 0);
    // The active install scope starts as the session scope; do_action_system()
    // is the only thing that ever changes it, and it always changes it back.
    strcpy(g_ins_home, g_home);
    strcpy(g_ins_apps, g_home_apps);
    g_ins_sys = g_sysscope;
    g_may_elevate = (sys_elev_may() == 1);
}

// "<user>", for the status line and the detail page. Display only.
static const char *scope_user_name(void) {
    struct passwd *pw = getpwuid(getuid());
    return (pw && pw->pw_name && pw->pw_name[0]) ? pw->pw_name : "this account";
}
static uint8_t g_dl[1024 * 1024];              // small-package (<= DL_SMALL_MAX) download + extraction scratch
static uint8_t g_chunk[DL_CHUNK];              // #570: one Range chunk (bounded, size-independent)
static uint8_t g_sig[1024];                    // detached manifest signature (#559)

// ---------------------------------------------------------------------------
// #559: repository source. Defaults to the public repo, overridable by a single
// line in /APPS/STORE.SRC (e.g. "http://192.0.2.1:8559"). This is apt's
// sources.list idea: WHERE packages come from is configuration, WHETHER they
// are trusted is decided by the signature over the manifest. A redirected
// client still refuses anything the signing key did not sign, so making the
// source configurable adds no trust surface, and it lets the shipping binary
// be verified against a controlled test repo.
// ---------------------------------------------------------------------------
#define REPO_DEFAULT "https://updates.maytera.net"
static char g_repo[160] = REPO_DEFAULT;

static void repo_load(void) {
    int fd = sys_open("/APPS/STORE.SRC", O_RDONLY);
    if (fd < 0) return;
    char b[192];
    int n = sys_read(fd, b, (int)sizeof(b) - 1);
    sys_close(fd);
    if (n <= 0) return;
    b[n] = 0;
    int i = 0;
    while (b[i] && b[i] != '\n' && b[i] != '\r' && b[i] != ' ') i++;
    b[i] = 0;
    // #B3: an http:// override still works (useful for pointing at a plain-
    // HTTP dev/test repo like the build container), but https:// is accepted too now that
    // it is the compiled-in default; anything else leaves the compiled-in
    // default in place rather than half-applying.
    int is_http  = strncmp(b, "http://", 7) == 0  && i > 7;
    int is_https = strncmp(b, "https://", 8) == 0 && i > 8;
    if ((is_http || is_https) && i < (int)sizeof(g_repo))
        strcpy(g_repo, b);
}

// Join the repo base and a relative path into `out`, with exactly one slash.
static void repo_url(const char *rel, char *out, int cap) {
    int o = 0;
    for (const char *p = g_repo; *p && o < cap - 1; p++) out[o++] = *p;
    if (o > 0 && out[o - 1] == '/') o--;
    if (o < cap - 1) out[o++] = '/';
    while (*rel == '/') rel++;
    while (*rel && o < cap - 1) out[o++] = *rel++;
    out[o] = 0;
}

// #590: the https->http Range-download workaround (repo_url_http) was removed
// here. #576 gave the kernel's sys_http_fetch_hdr an https_get_hdr() TLS path,
// so the large-package chunked Range download now uses the https repo URL
// directly - see acquire_verified_package().
static uint8_t g_shotraw[1024 * 1024];         // raw screenshot bytes (BMP) - shared scratch
static uint32_t g_shotpx[560 * 340];           // decoded screenshot pixels (BGRA) - detail page

// ---------------------------------------------------------------------------
// Package model
// ---------------------------------------------------------------------------
#define MAXPKG   64
#define MAXSHOT  5
#define MAXTAG   8
#define TAG_LEN  20
typedef struct {
    char id[32];
    char name[48];
    char version[16];
    char category[16];
    char author[40];
    char tagline[112];
    char path[96];
    char sha256[65];                 // #559: hex sha256 from the SIGNED manifest
    char desc[640];
    char whatsnew[320];
    char shots[MAXSHOT][96];
    int  nshots;
    int  size;
    int  installed_size;
    int  featured;
    // #B2: content type + facets, from the SIGNED manifest (trusted).
    char type[16];                   // "app" | "theme" | "wallpaper" (empty == app, legacy items)
    char tags[MAXTAG][TAG_LEN];
    int  ntags;
    // #B2: social stats, merged in from the UNSIGNED /api/catalog (display only,
    // never used for install trust decisions).
    int  download_count;
    int  rating_avg100;              // fixed-point, avg * 100 (e.g. 438 == 4.38)
    int  rating_count;
    // runtime install state
    int  installed;
    char inst_ver[16];
    int  has_update;
    // #745 (#77): the path the installer ACTUALLY wrote, read back from the
    // registry. Empty for an entry written by a build before the registry
    // carried a third field, and for a package installed outside the store.
    // launch_pkg() prefers this over guessing "<apps dir>/<pkg id>", because
    // the guess is a SECOND opinion about a fact the installer already knew.
    char inst_path[128];
} pkg_t;

static pkg_t g_pkg[MAXPKG];
static int   g_npkg = 0;

// #B2: per-card preview thumbnails for the browse grid. Decoded lazily (never
// in a hot path that runs unconditionally every frame): a card falls back to
// its letter-avatar tile until its thumbnail has been fetched, same idea as
// the detail page's "Loading preview..." placeholder. Loading is capped to a
// small budget per repaint (see ensure_thumb) so opening the store or
// scrolling the grid never blocks the UI thread waiting on the network for
// more than one or two small images at a time.
#define THUMB_SZ 40
static uint32_t g_thumbpx[MAXPKG][THUMB_SZ * THUMB_SZ];
static int      g_thumb_w[MAXPKG], g_thumb_h[MAXPKG];   // 0 == not loaded
static int      g_thumb_tried[MAXPKG];                  // 1 == already attempted (success or fail)
static int      g_thumb_budget;                          // new fetches allowed in this draw_all()
static void     ensure_thumb(int idx);                   // defined near the screenshot cache below

// ---------------------------------------------------------------------------
// Categories (server ids -> friendly modern labels)
// ---------------------------------------------------------------------------
typedef struct { const char *id; const char *label; } cat_t;
static cat_t g_cats[] = {
    { "games",  "Games"       },
    { "office", "Office"      },
    { "system", "Utilities"   },
    { "media",  "Media"       },
    { "coding", "Development" },
    { "themes", "Themes"      },
};
// #745: derive every list length from the array itself and pin parallel arrays
// to each other. A hand-counted length is how the Appearance panel's fifth dock
// style became unselectable (blame.md, "A hand-counted array length made the
// fifth option unselectable AND destroyed it on repaint"). NCAT was already
// derived; NTYPEFILT below was a bare literal 4 over two parallel arrays.
#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define NCAT ARRAY_COUNT(g_cats)

static const char *cat_label(const char *id) {
    for (int i = 0; i < NCAT; i++)
        if (strcmp(g_cats[i].id, id) == 0) return g_cats[i].label;
    return id;
}

// ---------------------------------------------------------------------------
// Views + navigation state
// ---------------------------------------------------------------------------
enum { V_DISCOVER = 0, V_CATEGORY, V_INSTALLED, V_UPDATES, V_DETAIL, V_SEARCH };
static int  g_view = V_DISCOVER;
static int  g_cat_sel = -1;         // selected category index for V_CATEGORY
static int  g_detail = -1;          // package index for V_DETAIL
static int  g_shot_sel = 0;         // selected screenshot in the detail gallery
static char g_search[64];
static int  g_search_len = 0;
static int  g_search_focus = 0;
static int  g_scroll = 0;           // vertical scroll offset for the content grid
static int  g_content_h = 0;        // last computed content height (for scroll clamp)
// (#96) The App Store computed g_content_h and handled the wheel (below) but
// never drew a scrollbar and never accepted a thumb drag: the shared gui_scroll_t
// widget was never wired in, so a user with an all-uncached, overflowing list
// had no on-screen sign that there was more to see. g_scroll stays the single
// source of truth for the ~15 layout call sites that already read it directly;
// g_sb is configured from it each draw_all() purely for the widget's geometry,
// rendering and thumb-drag input, matching the split settings.c's g_side_scroll
// would use if this app were being written fresh.
static gui_scroll_t g_sb;

// #B2: content-type filter, orthogonal to the sidebar's Discover/category/etc
// view and the search box. 0 = all types; else index into TYPE_VALUES below.
static const char *TYPE_LABELS[] = { "All Types", "Apps", "Themes", "Wallpapers" };
static const char *TYPE_VALUES[] = { "",          "app",  "theme",  "wallpaper" };
#define NTYPEFILT ARRAY_COUNT(TYPE_LABELS)
_Static_assert(ARRAY_COUNT(TYPE_LABELS) == ARRAY_COUNT(TYPE_VALUES),
               "TYPE_LABELS and TYPE_VALUES are both indexed by g_type_sel");
static int g_type_sel = 0;
static struct { int x, y, w, h; } g_typehit[NTYPEFILT];
static int g_ntypehit = 0;

// #B2: interactive 1..5 star rating control hit-zones on the detail page.
static struct { int x, y, w, h; } g_ratehit[5];
static int g_rate_hover = -1;        // 0..4 while the mouse is over a rating star, else -1

static int  g_mx = -1, g_my = -1;   // last mouse position (content-relative)

// status / progress banner
static char g_status[128] = {0};
static int  g_status_kind = 0;      // 0 none, 1 info, 2 success, 3 error

// ---------------------------------------------------------------------------
// Theme palette
// ---------------------------------------------------------------------------
static uint32_t C_surface, C_panel, C_card, C_ink, C_ink_dim, C_accent,
                C_accent_ink, C_border, C_hair, C_hero1, C_hero2, C_ok, C_err;

static void setup_palette(void) {
    uint32_t wbg  = theme_color(THEME_COLOR_WINDOW_BG);
    uint32_t ink  = theme_color(THEME_COLOR_LABEL_TEXT);
    uint32_t acc  = theme_color(THEME_COLOR_ACCENT);
    uint32_t bord = theme_color(THEME_COLOR_WINDOW_BORDER);

    // Guard against unset theme values.
    if (wbg == 0 && ink == 0) { wbg = 0x1E1E1E; ink = 0xFFFFFF; }
    if (acc == 0) acc = 0x2D7DF6;

    // Decide light vs dark from surface luminance.
    int lum = ((wbg >> 16 & 0xFF) * 30 + (wbg >> 8 & 0xFF) * 59 + (wbg & 0xFF) * 11) / 100;
    int dark = (lum < 128);

    C_surface     = wbg;
    C_panel       = dark ? gui_lighten(wbg, 8)  : gui_darken(wbg, 6);
    C_card        = dark ? gui_lighten(wbg, 16) : 0xFFFFFF;
    C_ink         = ink;
    C_ink_dim     = gui_mix(ink, wbg, 105);
    C_accent      = acc;
    C_accent_ink  = gui_ink_on(acc);
    C_border      = bord ? bord : (dark ? gui_lighten(wbg, 30) : gui_darken(wbg, 30));
    C_hair        = dark ? gui_lighten(wbg, 22) : gui_darken(wbg, 16);
    C_hero1       = gui_mix(acc, wbg, 60);
    C_hero2       = gui_mix(acc, wbg, 140);
    C_ok          = 0x3FA34D;
    C_err         = 0xC0392B;

    // Feed the shared style engine so gui_* primitives match. #612: this used
    // to compare theme_get_active() to the built-in Classic theme's id (2) -
    // the exact anti-pattern gui_theme.h's gui_theme_is_classic() comment
    // warns every app away from, because it only matches the ONE built-in
    // theme by numeric id. A custom or App Store theme with style=retro
    // still got the modern/rounded widget family under the old check.
    // gui_theme_is_classic() reads the active theme FILE's style= line, so it
    // is correct for any installed theme, not just the built-in one.
    gui_set_style(gui_theme_is_classic() ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface        = C_surface;
    p.surface_raised = C_card;
    p.ink            = C_ink;
    p.ink_dim        = C_ink_dim;
    p.accent         = C_accent;
    p.accent_hover   = gui_lighten(C_accent, 18);
    p.border         = C_border;
    p.field_bg       = dark ? gui_lighten(wbg, 12) : 0xFFFFFF;
    p.field_border   = C_border;
    p.track          = C_hair;
    gui_set_palette(&p);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static void T(int x, int y, const char *s, int size, uint32_t c) {
    win_draw_text_ttf(g_win, x, y, s, size, c);
}
static int TW(const char *s, int size) { return gui_ttf_width(s, size); }

// Truncate s to fit within max_w px at font size; writes into out (cap).
static void trunc_fit(const char *s, int size, int max_w, char *out, int cap) {
    int n = strlen(s);
    if (n > cap - 1) n = cap - 1;
    // fast path
    for (int i = 0; i < n; i++) out[i] = s[i];
    out[n] = 0;
    if (TW(out, size) <= max_w) return;
    while (n > 1) {
        n--;
        out[n] = 0;
        char tmp[160];
        int m = n; if (m > (int)sizeof(tmp) - 3) m = sizeof(tmp) - 3;
        for (int i = 0; i < m; i++) tmp[i] = out[i];
        tmp[m] = '.'; tmp[m+1] = '.'; tmp[m+2] = 0;
        if (TW(tmp, size) <= max_w) {
            for (int i = 0; i < m + 2; i++) out[i] = tmp[i];
            out[m+2] = 0;
            return;
        }
    }
}

// Word-wrap draw: returns the y after the last line.
static int draw_wrapped(int x, int y, int w, const char *s, int size, int lh, uint32_t c) {
    char line[256];
    int ll = 0;
    int lastspace = -1;
    const char *p = s;
    while (*p) {
        char ch = *p++;
        if (ch == '\n') {
            line[ll] = 0; T(x, y, line, size, c); y += lh; ll = 0; lastspace = -1; continue;
        }
        if (ll < (int)sizeof(line) - 1) { line[ll] = ch; if (ch == ' ') lastspace = ll; ll++; }
        line[ll] = 0;
        if (TW(line, size) > w) {
            if (lastspace > 0) {
                int keep = lastspace;
                char saved[256];
                int si = 0;
                for (int i = keep + 1; i < ll; i++) saved[si++] = line[i];
                saved[si] = 0;
                line[keep] = 0;
                T(x, y, line, size, c); y += lh;
                ll = 0;
                for (int i = 0; saved[i]; i++) line[ll++] = saved[i];
                line[ll] = 0;
                lastspace = -1;
            } else {
                line[ll-1] = 0;
                T(x, y, line, size, c); y += lh;
                line[0] = (char)(p[-1]); ll = 1; line[1] = 0; lastspace = -1;
            }
        }
    }
    if (ll > 0) { line[ll] = 0; T(x, y, line, size, c); y += lh; }
    return y;
}

static int point_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

// ---------------------------------------------------------------------------
// Synchronous HTTP GET via the async fetch worker (keeps kernel CR3 safe).
// Returns body length into buf (cap), or -1.
// ---------------------------------------------------------------------------
static int http_get(const char *url, uint8_t *buf, int cap) {
    int job = http_fetch_start(url);
    if (job < 0) return -1;
    for (int i = 0; i < 600; i++) {           // up to ~30s
        int status = 0; unsigned int len = 0;
        int st = http_fetch_poll(job, &status, &len);
        if (st < 0) { http_fetch_cancel(job); return -1; }
        if (st == 1) {                        // done
            int n = http_fetch_read(job, (char *)buf, (unsigned)cap);
            return n;
        }
        if (st == 2) { http_fetch_read(job, (char *)buf, (unsigned)cap); return -1; }
        usleep(50000);
    }
    http_fetch_cancel(job);
    return -1;
}

// ---------------------------------------------------------------------------
// #B3: same as http_get(), but for the INITIAL catalog load - the one fetch
// slow/unreachable enough (up to 4 retries x ~30s in load_manifest()) that a
// plain blocking wait made the whole window look wedged: main() drew exactly
// ONE frame ("Loading catalog...") before load_manifest() and then nothing
// redrew and no window event was serviced until the fetch finally settled.
// This drives the SAME http_fetch_start/poll/read protocol but in short
// slices, so it can update an animated status line and drain the window's
// event queue (staying repaintable/resizable, and honoring a close) while it
// waits - liveness is provable with two screendumps that show the spinner
// and elapsed time having moved, not a single frozen frame.
// ---------------------------------------------------------------------------
static void draw_all(void);
static int g_close_requested = 0;

static int http_get_live(const char *url, uint8_t *buf, int cap, const char *what) {
    int job = http_fetch_start(url);
    if (job < 0) return -1;
    static const char spin[4] = { '|', '/', '-', '\\' };
    for (int i = 0; i < 600 && !g_close_requested; i++) {   // up to ~30s, same budget as http_get()
        int status = 0; unsigned int len = 0;
        int st = http_fetch_poll(job, &status, &len);
        if (st < 0) { http_fetch_cancel(job); return -1; }
        if (st == 1) { int n = http_fetch_read(job, (char *)buf, (unsigned)cap); return n; }
        if (st == 2) { http_fetch_read(job, (char *)buf, (unsigned)cap); return -1; }

        char s[80]; strcpy(s, what);
        int sl = strlen(s);
        if (sl < (int)sizeof(s) - 4) { s[sl] = ' '; s[sl+1] = ' '; s[sl+2] = spin[i & 3]; s[sl+3] = 0; }
        strncpy(g_status, s, sizeof(g_status) - 1); g_status[sizeof(g_status) - 1] = 0;
        g_status_kind = 1;
        draw_all();

        gui_event_t ev;
        int et = win_get_event(g_win, &ev, 50);   // ~50ms slice -> spinner ticks ~20Hz while polling
        if (et != 0) {
            if (ev.type == EVENT_WINDOW_CLOSE) { g_close_requested = 1; }
            else if (ev.type == EVENT_RESIZE && ev.mouse_x > 200 && ev.mouse_y > 200) {
                g_win_w = ev.mouse_x; g_win_h = ev.mouse_y;
            }
        }
    }
    http_fetch_cancel(job);
    return -1;
}

// ---------------------------------------------------------------------------
// #570: chunked HTTP Range GET, for downloading packages larger than one fetch.
//
// WHY: the old path buffered a whole package in one fixed 3MB RAM buffer fetched
// in a SINGLE http_get. Two hard limits made that unfit for large packages (the
// ~391MB OpenArena package, #568, is ~130x over that buffer):
//   1. the kernel HTTP client caps ONE fetch at WGET_BUFFER_SIZE (1MB), so any
//      package over ~1MB was truncated and failed outright; and
//   2. a single sustained TCP stream tears down after ~8-14MB (a kernel TCP
//      send-buffer limit), so a merely bigger buffer would not have carried it.
// FIX (the design the kernel's own wget.c comment already calls for): download
// in small in-regime Range chunks, each a FRESH short connection (wget uses
// keep_alive=false, so every chunk is its own connection and no single stream
// approaches the teardown limit), hashing each chunk on the fly. Peak app RAM is
// one DL_CHUNK, independent of package size.
//
// Vehicle: SYS_HTTP_FETCH_HDR, the existing headers-capable blocking GET that
// netinfo/haservice already use safely from Ring 3, with a Range: header. #576
// taught it to dispatch on URL scheme, so an https:// URL goes over the TLS
// client (https_get_hdr) and an http:// override uses the plain client. Returns
// bytes received into buf (<= cap), 0 at a clean EOF, or -1 on error; *status
// carries the HTTP status (206 partial content, 200 whole body if the server
// ignored Range, 416 past EOF).
// ---------------------------------------------------------------------------
// Signed sibling of fmt_ulong below, for reporting a negative syscall return.
static void fmt_long(long v, char *out) {
    int i = 0;
    if (v < 0) { out[i++] = '-'; v = -v; }
    char t[24]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) out[i++] = t[--n];
    out[i] = 0;
}
static int fmt_ulong(unsigned long v, char *out) {
    char tmp[24]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
    return n;
}

static int http_get_range(const char *url, unsigned long off, unsigned long want,
                          uint8_t *buf, unsigned int cap, int *status) {
    if (status) *status = 0;
    if (want > cap) want = cap;
    if (want == 0) return 0;

    // Build "Range: bytes=OFF-END\r\n"
    char hdr[64];
    int o = 0;
    const char *pre = "Range: bytes=";
    while (*pre) hdr[o++] = *pre++;
    char a[24], b[24];
    fmt_ulong(off, a);
    fmt_ulong(off + want - 1, b);
    for (char *p = a; *p; p++) hdr[o++] = *p;
    hdr[o++] = '-';
    for (char *p = b; *p; p++) hdr[o++] = *p;
    hdr[o++] = '\r'; hdr[o++] = '\n'; hdr[o] = 0;

    unsigned int got = 0; int st = 0;
    int r = sys_http_fetch_hdr(url, hdr, (char *)buf, cap, &got, &st);
    if (status) *status = st;
    if (r < 0) return -1;
    return (int)got;
}

// #570/#613: acquire a signature-verified package and say WHERE it now lives.
// Small packages take the exact proven pre-#570 path (one fetch into g_dl).
// Larger packages are streamed to disk in Range chunks, hashed incrementally,
// verified against the SIGNED manifest sha256, and then re-hashed OFF DISK -
// all in bounded slices, with no whole-package allocation anywhere. The caller
// then STREAMS the unpack from whichever source this reports (see run_stream);
// there is no longer a size at which a package becomes un-unpackable, so the
// old -2 "too large to buffer" result is gone. Returns 0 on success, -1 on a
// download/verify failure; g_status is set in every failure path.
static void draw_all(void);
static void mkparents(const char *dest);
// ---------------------------------------------------------------------------
// #613: streamed unpack (inflate -> tar -> destination file), constant memory
// ---------------------------------------------------------------------------
// The old path decompressed the WHOLE .mpkg into one heap buffer and then made
// a second heap copy of every member (arc_targz_extract), so peak userland RAM
// was O(package size): on the 103,563,185-byte OpenArena package that meant a
// ~104MB malloc on top of a ~104MB read-back, and the measured 92 s
// read-back/unpack. Now the verified file is pulled through the shared
// libarchive streaming extractor (arc_targz_extract_stream) in bounded slices
// and each member is written straight to its destination in 32KB appends, so
// this app's own working set is a fixed ~42KB no matter how big the package is.
//
// SECURITY ORDER IS UNCHANGED AND NON-NEGOTIABLE: the package is downloaded to
// /STOREDL.TMP, its on-disk sha256 is verified against the SIGNED manifest, and
// ONLY THEN is a single byte unpacked. Nothing is ever extracted straight off
// the network. libarchive additionally rejects unsafe member names/types
// (absolute paths, "..", symlink/hardlink/device/fifo) and cleans up a partial
// destination file if the stream aborts.

// Human-readable form of a libarchive streaming result, so a refusal says WHY
// on the status line (the only channel a GUI app has on a headless VM).
static const char *arc_rc_text(int rc) {
    switch (rc) {
        case ARC_OK:        return "ok";
        case ARC_E_INPUT:   return "truncated or unreadable";
        case ARC_E_FORMAT:  return "malformed archive";
        case ARC_E_CORRUPT: return "checksum mismatch";
        case ARC_E_UNSAFE:  return "unsafe member path or type";
        case ARC_E_SINK:    return "write failed";
        case ARC_E_NOMEM:   return "out of memory";
        default:            return "unknown error";
    }
}

// ---- readers -------------------------------------------------------------
typedef struct { int fd; } src_fd_t;
static int src_fd_read(void *ctx, uint8_t *buf, size_t cap) {
    long r = sys_read(((src_fd_t *)ctx)->fd, buf, (unsigned long)cap);
    if (r < 0) return -1;
    return (int)r;
}
typedef struct { const uint8_t *p; size_t n, off; } src_mem_t;
static int src_mem_read(void *ctx, uint8_t *buf, size_t cap) {
    src_mem_t *m = (src_mem_t *)ctx;
    size_t want = m->n - m->off;
    if (want == 0) return 0;
    if (want > cap) want = cap;
    memcpy(buf, m->p + m->off, want);
    m->off += want;
    return (int)want;
}

// Where a verified package lives: either the static small-download buffer or
// the on-disk temp file. Never both, and never unverified.
typedef struct {
    int             from_file;             // 1 = STORE_TMP_PATH, 0 = memory
    const uint8_t  *mem;
    size_t          memlen;
} pkgsrc_t;

static int run_stream(const pkgsrc_t *src, const arc_sink *sink, void *ctx, int verify_crc) {
    if (src->from_file) {
        int fd = sys_open(STORE_TMP_PATH, O_RDONLY);
        if (fd < 0) return ARC_E_INPUT;
        src_fd_t r = { fd };
        int rc = arc_targz_extract_stream(src_fd_read, &r, sink, ctx, verify_crc);
        sys_close(fd);
        return rc;
    }
    src_mem_t r = { src->mem, src->memlen, 0 };
    return arc_targz_extract_stream(src_mem_read, &r, sink, ctx, verify_crc);
}

// ---- pass 1: capture just the INSTALL manifest ---------------------------
// The manifest is what maps archive members to destinations, and real packages
// do NOT put it first (OpenArena's INSTALL is the 6th of 10 members, after
// ~101MB of .pk3 data), so its position cannot be assumed. It is a few KB, so
// pass 1 streams the archive purely to lift that one member out, then stops.
#define INSTALL_MAX 8192
typedef struct {
    char        want[160];       // "<id>/INSTALL"
    char        buf[INSTALL_MAX];
    size_t      len;
    int         found;
} man_sink_t;

static int man_member(void *ctx, const arc_member *m) {
    man_sink_t *s = (man_sink_t *)ctx;
    if (m->is_dir) return 1;
    if (strcmp(m->name, s->want) != 0) return 1;              // skip
    if (m->size >= INSTALL_MAX) return -1;                     // absurd manifest
    s->len = 0;
    return 0;
}
static int man_data(void *ctx, const uint8_t *d, size_t len) {
    man_sink_t *s = (man_sink_t *)ctx;
    if (s->len + len >= INSTALL_MAX) return -1;
    memcpy(s->buf + s->len, d, len);
    s->len += len;
    return 0;
}
static int man_end(void *ctx, const arc_member *m) {
    man_sink_t *s = (man_sink_t *)ctx;
    (void)m;
    s->buf[s->len] = 0;
    s->found = 1;
    return ARC_STOP;      // got what we came for; do not inflate the rest
}
static const arc_sink MAN_SINK = { man_member, man_data, man_end, 0 };

// ---- pass 2: write each mapped member straight to its destination --------
#define UNPACK_CHUNK 32768
typedef struct {
    const char *pkgid;
    const char *man;
    size_t      mlen;
    int         fd;                  // open destination, -1 when idle
    char        dest[PKGDEST_MAX];   // #745: CONFINED destination, never the raw one
    int         count;               // files actually written
    char        firstlaunch[PKGDEST_MAX];
    char        theme_dest[PKGDEST_MAX];
    int         write_err;
    // #745: the first destination that could not be created, and the errno
    // sys_open returned for it. Kept so the failure can name itself.
    char        fail_dest[PKGDEST_MAX];
    int         fail_rc;
    // #745: the first destination REFUSED by pkgdest_confine(), and why. A
    // refusal is not a disk error and must not be reported as one.
    char        reject_dest[PKGDEST_MAX];
    int         reject_rc;
    // #745: the first four PAYLOAD bytes of the member currently being
    // written, for the ELF check in unp_end(). Detected from the BYTES and not
    // from the destination path, for the reason kernel/proc/syscall.c:1899
    // gives about pkg-write: "under /APPS" is a convention, the magic is a fact.
    unsigned char magic[4];
    int         nmagic;
    // #745: the first chmod that failed after a successful write, so an
    // install that lands but will not launch says so instead of claiming
    // success.
    char        chmod_dest[PKGDEST_MAX];
} unp_sink_t;

// Resolve an archive member name to its INSTALL destination.
// INSTALL lines are "<src> -> <dest>", <src> being relative to "<id>/".
static int install_dest_for(const char *man, size_t mlen, const char *pkgid,
                            const char *member, char *dest, int destcap) {
    int idl = strlen(pkgid);
    if (strncmp(member, pkgid, idl) != 0 || member[idl] != '/') return 0;
    const char *rel = member + idl + 1;
    size_t p = 0;
    while (p < mlen) {
        char line[256]; int ll = 0;
        while (p < mlen && man[p] != '\n' && ll < 255) line[ll++] = man[p++];
        if (p < mlen) p++;
        line[ll] = 0;
        if (line[0] == '#' || line[0] == 0) continue;
        char *arrow = strstr(line, "->");
        if (!arrow) continue;
        char src[128]; int si = 0;
        char *s = line;
        while (s < arrow && *s == ' ') s++;
        while (s < arrow && *s != ' ' && si < 127) src[si++] = *s++;
        src[si] = 0;
        if (strcmp(src, rel) != 0) continue;
        char *d = arrow + 2;
        while (*d == ' ') d++;
        int di = 0;
        while (*d && *d != ' ' && *d != '\r' && di < destcap - 1) dest[di++] = *d++;
        dest[di] = 0;
        return dest[0] ? 1 : 0;
    }
    return 0;
}

static int unp_member(void *ctx, const arc_member *m) {
    unp_sink_t *u = (unp_sink_t *)ctx;
    if (m->is_dir) return 1;
    char dest[PKGDEST_MAX];
    if (!install_dest_for(u->man, u->mlen, u->pkgid, m->name, dest, (int)sizeof(dest)))
        return 1;                                  // not referenced by INSTALL: skip
    if (dest[0] != '/') return 1;                  // destinations are absolute by design

    // #745 CONFINE, and this is the single most security-relevant line in the
    // install path. A package manifest can name ANY absolute destination; the
    // only check that ever existed is the one immediately above. That was
    // survivable while the signature was the sole control and root was the only
    // installer. It is NOT survivable once this client starts REWRITING
    // destinations into a per-user prefix, because a naive prefix join is
    // steered by the very string it is prefixing:
    //
    //     "/HOME/ADMIN" + "/APPS/../../CONFIG/SHADOW" -> /CONFIG/SHADOW
    //
    // pkgdest_confine() canonicalizes FIRST against the root (so ".." can never
    // rise above it) and only then joins the sandbox, which makes escape
    // impossible by construction, then re-canonicalizes and requires the result
    // to lie under the sandbox as an independent proof. It also enforces the
    // installable-prefix allowlist and refuses the boot medium. See
    // userland/libc/tests/run_pkgdest.sh for the hostile-input battery, which
    // includes a negative arm proving the naive join really does escape.
    //
    // FAIL CLOSED. A refused destination aborts the whole install; it is never
    // skipped, because a package that half-installed is not a package.
    char cdest[PKGDEST_MAX];
    int crc = pkgdest_confine(g_ins_home, dest, cdest, sizeof(cdest));
    if (crc != PKGDEST_OK) {
        u->write_err = 1;
        if (u->reject_dest[0] == 0) {
            strncpy(u->reject_dest, dest, sizeof(u->reject_dest) - 1);
            u->reject_dest[sizeof(u->reject_dest) - 1] = 0;
            u->reject_rc = crc;
        }
        return -1;
    }

    mkparents(cdest);
    sys_unlink(cdest);
    int fd = sys_open(cdest, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        u->write_err = 1;
        if (u->fail_dest[0] == 0) {
            strncpy(u->fail_dest, cdest, sizeof(u->fail_dest) - 1);
            u->fail_dest[sizeof(u->fail_dest) - 1] = 0;
            u->fail_rc = fd;
        }
        return -1;
    }
    u->fd = fd;
    strncpy(u->dest, cdest, sizeof(u->dest) - 1);
    u->dest[sizeof(u->dest) - 1] = 0;
    u->nmagic = 0;
    return 0;
}
static int unp_data(void *ctx, const uint8_t *d, size_t len) {
    unp_sink_t *u = (unp_sink_t *)ctx;
    if (u->fd < 0) return -1;
    // #745: keep the first four payload bytes for the ELF test in unp_end().
    // Written as a fill loop rather than a single memcpy because a member's
    // first chunk is not guaranteed to be at least four bytes long.
    for (size_t k = 0; k < len && u->nmagic < 4; k++)
        u->magic[u->nmagic++] = d[k];
    size_t off = 0;
    while (off < len) {
        size_t take = len - off;
        if (take > UNPACK_CHUNK) take = UNPACK_CHUNK;
        if (sys_write(u->fd, d + off, (unsigned long)take) != (long)take) {
            u->write_err = 1;
            return -1;
        }
        off += take;
    }
    return 0;
}
static int unp_end(void *ctx, const arc_member *m) {
    unp_sink_t *u = (unp_sink_t *)ctx;
    (void)m;
    sys_close(u->fd);
    u->fd = -1;
    u->count++;

    // ---- #745 THE EXECUTE BIT -------------------------------------------
    // Without this, a per-user install writes every byte correctly and then
    // refuses to start, which reads as a completely different bug.
    //
    // spawn_impl() enforces X_OK on the binary for PRIV_USER callers (#700 B8,
    // kernel/proc/syscall.c), and perms_on_create() stamps every newly created
    // file 0644 (kernel/fs/perms.c). These member writes go through sys_open,
    // NOT through SYS_PKG_WRITE, so pkg_write_stamp()'s existing ELF-detecting
    // 0555 stamp (#689) never runs for them. Only STORE.DB and STORE.CFG use
    // pkg_write. So the file is created non-executable and the launch is
    // refused.
    //
    // 0555 and not 0755, deliberately mirroring #689: the installing user owns
    // it and can delete and replace it (a write to the parent directory, which
    // they have already passed), but cannot mutate in place the bytes of a
    // binary that may already have been launched.
    //
    // THIS ALSO FIXES THE ROOT CASE, which was broken and unnoticed. A root
    // install today leaves /APPS/<app> with a root:root 0644 entry, so a
    // NON-root session cannot launch an app that root installed from the store:
    // owner bits do not apply, group bits do not apply, and other is r-- with
    // no x. chmod is applied in BOTH scopes for that reason.
    //
    // rk_chmod_route() sends an ext2/POSIX path to perms_chmod(), which lets a
    // non-root caller chmod a file it OWNS. It owns this one: perms_on_create()
    // recorded the creating process as owner when sys_open created it a moment
    // ago. No privilege is needed and none is requested.
    if (u->nmagic == 4 && u->magic[0] == 0x7F && u->magic[1] == 'E' &&
        u->magic[2] == 'L' && u->magic[3] == 'F') {
        if (chmod(u->dest, 0555) != 0 && u->chmod_dest[0] == 0) {
            strncpy(u->chmod_dest, u->dest, sizeof(u->chmod_dest) - 1);
            u->chmod_dest[sizeof(u->chmod_dest) - 1] = 0;
        }
    }

    int dl = strlen(u->dest);
    if (u->theme_dest[0] == 0 && dl > 7 && strcmp(u->dest + dl - 7, ".mtheme") == 0) {
        strncpy(u->theme_dest, u->dest, sizeof(u->theme_dest) - 1);
        u->theme_dest[sizeof(u->theme_dest) - 1] = 0;
    }
    // #745: the launch path is now under the CONFINED applications directory,
    // g_home_apps, which is "<home>/APPS" for a user session and exactly
    // "/APPS" for a root one. Comparing against the literal "/APPS/" here would
    // have silently found nothing for every per-user install, and the app would
    // have installed correctly and never appeared in the Start menu.
    {
        int hal = strlen(g_ins_apps);
        if (u->firstlaunch[0] == 0 && strncmp(u->dest, g_ins_apps, hal) == 0 &&
            u->dest[hal] == '/') {
            int lower = 1;
            for (const char *c = u->dest + hal + 1; *c; c++)
                if (*c >= 'A' && *c <= 'Z') { lower = 0; break; }
            if (lower) {
                strncpy(u->firstlaunch, u->dest, sizeof(u->firstlaunch) - 1);
                u->firstlaunch[sizeof(u->firstlaunch) - 1] = 0;
            }
        }
    }
    return 0;
}
// The stream failed with a destination still open: close it and remove the
// truncated file rather than leaving a half-written binary on the volume.
static void unp_abort(void *ctx) {
    unp_sink_t *u = (unp_sink_t *)ctx;
    if (u->fd >= 0) { sys_close(u->fd); u->fd = -1; }
    if (u->dest[0]) sys_unlink(u->dest);
}
static const arc_sink UNP_SINK = { unp_member, unp_data, unp_end, unp_abort };

static int acquire_verified_package(pkg_t *pk, pkgsrc_t *src) {
    src->from_file = 0; src->mem = 0; src->memlen = 0;
    char url[224];
    repo_url(pk->path, url, (int)sizeof(url));
    unsigned long total = (pk->size > 0) ? (unsigned long)pk->size : 0;

    // ---- Small: one in-regime fetch into the static buffer (unchanged path,
    // and the real https g_repo works fine here - http_get -> http_fetch_start
    // dispatches to https_get for an https:// URL).
    if (total == 0 || total <= DL_SMALL_MAX) {
        int n = http_get(url, g_dl, (int)sizeof(g_dl));
        if (n <= 0) { strcpy(g_status, "Download failed"); g_status_kind = 3; return -1; }
        int vrc = pkgsig_verify_package(g_dl, (size_t)n, pk->sha256);
        if (vrc != PKGSIG_OK) {
            strcpy(g_status, "REFUSED: ");
            strncat(g_status, pkgsig_strerror(vrc), sizeof(g_status) - strlen(g_status) - 1);
            g_status_kind = 3; return -1;
        }
        src->from_file = 0; src->mem = g_dl; src->memlen = (size_t)n;
        return 0;
    }

    // ---- Larger: chunked Range download streamed to disk, hashed on the fly.
    // #590: uses the same https `url` as every other fetch - #576 gave
    // sys_http_fetch_hdr (behind http_get_range) an https_get_hdr() TLS path,
    // so the Range download stays on https with no scheme downgrade.
    sys_unlink(STORE_TMP_PATH);
    int fd = sys_open(STORE_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        // #745: SAY WHICH PATH AND WHICH ERRNO. sys_open returns -errno, so -13
        // is EACCES (this path is not writable by the signed-in account) and -2
        // is ENOENT (its directory does not exist). The old text named neither,
        // which is why a permission problem read as a mystery.
        char nb[24];
        strcpy(g_status, "Cannot open download temp file ");
        strncat(g_status, STORE_TMP_PATH, sizeof(g_status) - strlen(g_status) - 1);
        strncat(g_status, " (rc=", sizeof(g_status) - strlen(g_status) - 1);
        fmt_long(fd, nb);
        strncat(g_status, nb, sizeof(g_status) - strlen(g_status) - 1);
        strncat(g_status, fd == -13 ? ", permission denied)" : ")",
                sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 3; return -1;
    }

    sha256_ctx_t hctx; sha256_init(&hctx);
    unsigned long off = 0; int ok = 1; int last_draw = 0;
    while (off < total) {
        unsigned long want = total - off;
        if (want > DL_CHUNK) want = DL_CHUNK;
        // #608: ONE failed chunk must not abandon a 100MB download. Each chunk
        // is its own short connection, so a transient failure (peer stall, a
        // dropped segment, a proxy hiccup) is retried IN PLACE from the same
        // offset before the whole install is given up. Before this, the very
        // first failure broke the loop and no second Range request was ever
        // put on the wire, which is exactly what the server logs showed.
        // Bounded at 4 attempts with a short backoff, so an endpoint that is
        // genuinely down still fails fast instead of looping.
        int st = 0, got = -1;
        for (int attempt = 0; attempt < 4; attempt++) {
            st = 0;
            got = http_get_range(url, off, want, g_chunk, DL_CHUNK, &st);
            if (got > 0 && (st == 206 || st == 200)) break;
            got = -1;
            usleep(400000);
        }
        if (got <= 0) { ok = 0; break; }
        // sys_write buffers into the kernel's per-fd write buffer and only
        // flushes to the volume on close; for a very large file that buffer can
        // itself exhaust the kernel heap. That is the #570 KERNEL dependency:
        // when it triggers, sys_write returns short and we fail cleanly here.
        if (sys_write(fd, g_chunk, (unsigned long)got) != (long)got) { ok = 0; break; }
        sha256_update(&hctx, g_chunk, (size_t)got);
        off += (unsigned long)got;
        // Progress banner (throttled to ~every 4MB so redraw is not a hot path).
        if ((int)(off >> 22) != last_draw) {
            last_draw = (int)(off >> 22);
            char pc[8]; fmt_ulong(total ? (off * 100 / total) : 0, pc);
            strcpy(g_status, "Downloading ");
            strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, " ", sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, pc, sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, "%", sizeof(g_status) - strlen(g_status) - 1);
            g_status_kind = 1;
            draw_all();
        }
        if (st == 200) break;   // server ignored Range and sent the whole body
    }
    sys_close(fd);
    if (!ok || off < total) {
        // #608: this is a DOWNLOAD failure, and it must say so. The old text
        // blamed "package too large for this build to buffer" for a failure
        // that happens while bytes are still being fetched, which sent the
        // next person hunting a nonexistent size ceiling when the real fault
        // was that chunk 1 never completed (kernel tcp_recv length truncation,
        // fixed under the same task). The "too large to unpack" cases below
        // keep their own, separate wording.
        char _pc[8]; fmt_ulong(total ? (off * 100 / total) : 0, _pc);
        strcpy(g_status, "Download failed at ");
        strncat(g_status, _pc, sizeof(g_status) - strlen(g_status) - 1);
        strncat(g_status, "% (network or server error; nothing was installed)",
                sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 3;
        sys_unlink(STORE_TMP_PATH);
        return -1;
    }

    uint8_t digest[32]; sha256_final(&hctx, digest);
    int vrc = pkgsig_verify_package_digest(digest, pk->sha256);
    if (vrc != PKGSIG_OK) {
        strcpy(g_status, "REFUSED: ");
        strncat(g_status, pkgsig_strerror(vrc), sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 3;
        sys_unlink(STORE_TMP_PATH);
        return -1;
    }

    /* #613: the digest above covers the NETWORK bytes. Re-hash what is
       actually ON DISK too (the 2c15a45 on-disk check), because that file -
       not the network stream - is what gets unpacked. This is a bounded
       DL_CHUNK-sliced read pass with NO allocation at all: the old code
       malloc()ed a whole second copy of the package (a ~104MB allocation on
       OpenArena) purely so the buffered extractor could be handed a pointer,
       which is exactly the ceiling #613 removes. VERIFY-BEFORE-EXTRACT is the
       reason this pass exists at all and it stays first: not one byte is
       unpacked until the on-disk copy matches the SIGNED manifest. */
    int rfd = sys_open(STORE_TMP_PATH, O_RDONLY);
    if (rfd < 0) {
        strcpy(g_status, "Downloaded, but the temp file could not be reopened to verify");
        g_status_kind = 3; return -1;
    }
    sha256_ctx_t dctx; sha256_init(&dctx);
    unsigned long rd = 0;
    while (rd < off) {
        unsigned long want = off - rd;
        if (want > DL_CHUNK) want = DL_CHUNK;
        long r = sys_read(rfd, g_chunk, want);
        if (r <= 0) break;
        sha256_update(&dctx, g_chunk, (size_t)r);
        rd += (unsigned long)r;
    }
    sys_close(rfd);
    if (rd != off) {
        char nb[24];
        strcpy(g_status, "Read-back failed rd=");
        fmt_ulong(rd, nb);  strncat(g_status, nb, sizeof(g_status)-strlen(g_status)-1);
        strncat(g_status, " exp=", sizeof(g_status)-strlen(g_status)-1);
        fmt_ulong(off, nb); strncat(g_status, nb, sizeof(g_status)-strlen(g_status)-1);
        g_status_kind = 3; return -1;
    }
    uint8_t ddig[32]; sha256_final(&dctx, ddig);
    if (pkgsig_verify_package_digest(ddig, pk->sha256) != PKGSIG_OK) {
        strcpy(g_status, "REFUSED: on-disk copy does not match the verified download");
        g_status_kind = 3; return -1;   /* temp file RETAINED for inspection */
    }
    src->from_file = 1;
    return 0;
}

// #B2: JSON POST for the unsigned /api/ social endpoints (download count,
// star ratings). Same async-poll shape as http_get above; returns the HTTP
// status (>0) with the response body in resp[], or a negative value on a
// network/timeout error. Never used for anything trust-relevant: nothing
// this touches feeds pkgsig_verify_package().
static int http_post_json(const char *url, const char *body, char *resp, int respcap) {
    static const char headers[] = "Content-Type: application/json\r\n";
    int job = http_post_start(url, headers, body);
    if (job < 0) return -1;
    for (int i = 0; i < 200; i++) {            // up to ~10s
        int status = 0; unsigned int len = 0;
        int st = http_post_poll(job, &status, &len);
        if (st < 0) { http_post_cancel(job); return -1; }
        if (st == 1) {
            int n = http_post_read(job, resp, (unsigned)(respcap - 1));
            if (n < 0) n = 0;
            resp[n] = 0;
            return status > 0 ? status : 200;
        }
        if (st == 2) { http_post_read(job, resp, (unsigned)(respcap - 1)); return -2; }
        usleep(50000);
    }
    http_post_cancel(job);
    return -3;
}

// ---------------------------------------------------------------------------
// Minimal tolerant JSON field extraction over a single object substring.
// ---------------------------------------------------------------------------
static const char *obj_find_key(const char *obj, const char *end, const char *key) {
    int kl = strlen(key);
    for (const char *p = obj; p + kl + 2 < end; p++) {
        if (p[0] == '"' && strncmp(p + 1, key, kl) == 0 && p[1 + kl] == '"') {
            const char *q = p + 1 + kl + 1;
            while (q < end && (*q == ' ' || *q == ':' )) q++;
            return q;   // points at value start
        }
    }
    return 0;
}

static void json_str(const char *obj, const char *end, const char *key, char *out, int cap) {
    out[0] = 0;
    const char *v = obj_find_key(obj, end, key);
    if (!v || v >= end || *v != '"') return;
    v++;
    int o = 0;
    while (v < end && *v != '"' && o < cap - 1) {
        char c = *v++;
        if (c == '\\' && v < end) {
            char e = *v++;
            if (e == 'n') c = '\n';
            else if (e == 't') c = ' ';
            else c = e;
        }
        out[o++] = c;
    }
    out[o] = 0;
}

static int json_int(const char *obj, const char *end, const char *key) {
    const char *v = obj_find_key(obj, end, key);
    if (!v || v >= end) return 0;
    int neg = 0; if (*v == '-') { neg = 1; v++; }
    int n = 0, got = 0;
    while (v < end && *v >= '0' && *v <= '9') { n = n * 10 + (*v - '0'); v++; got = 1; }
    return got ? (neg ? -n : n) : 0;
}

static int json_bool(const char *obj, const char *end, const char *key) {
    const char *v = obj_find_key(obj, end, key);
    if (!v || v >= end) return 0;
    return (*v == 't');
}

// #B2: parse a JSON number with up to 2 decimal digits into a fixed-point
// integer scaled *100 (e.g. "rating_avg":4.38 -> 438). Used for rating_avg,
// which the /api/ social layer emits as a float; everything else in this file
// stays integer arithmetic. Extra decimal digits beyond 2 are read past but
// dropped (fine for a 1-5 star average - a third decimal never changes what
// the UI displays or rounds to).
static int json_fixed2(const char *obj, const char *end, const char *key) {
    const char *v = obj_find_key(obj, end, key);
    if (!v || v >= end) return 0;
    int neg = 0; if (*v == '-') { neg = 1; v++; }
    int ip = 0;
    while (v < end && *v >= '0' && *v <= '9') { ip = ip * 10 + (*v - '0'); v++; }
    int frac = 0, fdig = 0;
    if (v < end && *v == '.') {
        v++;
        while (v < end && *v >= '0' && *v <= '9' && fdig < 2) { frac = frac * 10 + (*v - '0'); fdig++; v++; }
        while (fdig < 2) { frac *= 10; fdig++; }
    }
    int val = ip * 100 + frac;
    return neg ? -val : val;
}

// Generic array-of-strings parser: writes up to maxn strings (each up to
// stride-1 chars) into a caller `char out[maxn][stride]`-shaped buffer passed
// as a flat char*. Shared by shots[] (stride 96) and tags[] (stride TAG_LEN)
// so both facets reuse one parser instead of near-duplicate copies.
static void json_str_array_g(const char *obj, const char *end, const char *key,
                             char *out, int stride, int maxn, int *count) {
    *count = 0;
    const char *v = obj_find_key(obj, end, key);
    if (!v || v >= end || *v != '[') return;
    v++;
    while (v < end && *v != ']' && *count < maxn) {
        while (v < end && *v != '"' && *v != ']') v++;
        if (v >= end || *v == ']') break;
        v++;
        char *dst = out + (*count) * stride;
        int o = 0;
        while (v < end && *v != '"' && o < stride - 1) dst[o++] = *v++;
        dst[o] = 0;
        if (o > 0) (*count)++;
        while (v < end && *v != ',' && *v != ']') v++;
        if (v < end && *v == ',') v++;
    }
}

// ---------------------------------------------------------------------------
// Installed registry: "id version" per line.
//
// #745: there are now TWO of them and reading MERGES them.
//
//   /APPS/STORE.DB          what root installed for everybody. Root-owned, so
//                           a non-root session can read it and cannot write it.
//   <home>/CONFIG/STORE.DB  what THIS account installed for itself.
//
// Merging on read is what makes the store honest: a package installed either
// way reads as installed, so the button says "Open" rather than offering to
// install a second copy. Writing goes to whichever file this session is
// actually allowed to write, decided once in scope_init().
//
// For a root session <home> is "/", so the per-user path is /CONFIG/STORE.DB.
// That file does not exist on any shipped image and root never writes it, so a
// root session reads exactly /APPS/STORE.DB and writes exactly /APPS/STORE.DB,
// as it always has.
// ---------------------------------------------------------------------------
static void user_registry_path(char *out, unsigned long cap) {
    if (userhome_path("CONFIG", "STORE.DB", out, cap) != 0) {
        // Refuse rather than truncate; an empty path simply opens nothing.
        if (cap) out[0] = 0;
    }
}

// Fold one registry file into the package list. `scope_user` is recorded so the
// UI can say WHERE a package is installed, which is the difference between
// "you have this" and "the administrator installed this".
static void registry_merge(const char *path) {
    if (!path || !path[0]) return;
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return;
    static char buf[8192];
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char *p = buf;
    while (*p) {
        char id[32]; char ver[16]; char epath[128];
        int i = 0;
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        while (*p && *p != ' ' && *p != '\n' && i < 31) id[i++] = *p++;
        id[i] = 0;
        while (*p == ' ') p++;
        int j = 0;
        while (*p && *p != ' ' && *p != '\n' && *p != '\r' && j < 15) ver[j++] = *p++;
        ver[j] = 0;
        // #745 (#77): OPTIONAL third field, the launch path the installer wrote.
        // A line written by an older build has only two fields and stops at the
        // newline here, which is exactly what the pre-existing reader did with
        // anything after the version - so old registries still load, and a
        // registry written by this build is still readable by an older one.
        while (*p == ' ') p++;
        int e = 0;
        while (*p && *p != ' ' && *p != '\n' && *p != '\r' && e < 127) epath[e++] = *p++;
        epath[e] = 0;
        while (*p && *p != '\n') p++;
        if (id[0]) {
            for (int k = 0; k < g_npkg; k++) {
                if (strcmp(g_pkg[k].id, id) == 0) {
                    g_pkg[k].installed = 1;
                    strncpy(g_pkg[k].inst_ver, ver, sizeof(g_pkg[k].inst_ver) - 1);
                    // Absolute paths only. A relative or empty third field is
                    // dropped rather than joined onto anything, so a malformed
                    // registry degrades to the old guess instead of spawning a
                    // path this code assembled from a fragment.
                    if (epath[0] == '/') {
                        strncpy(g_pkg[k].inst_path, epath, sizeof(g_pkg[k].inst_path) - 1);
                        g_pkg[k].inst_path[sizeof(g_pkg[k].inst_path) - 1] = 0;
                    }
                    if (strcmp(ver, g_pkg[k].version) != 0) g_pkg[k].has_update = 1;
                }
            }
        }
    }
}

static void load_registry(void) {
    for (int i = 0; i < g_npkg; i++) {
        g_pkg[i].installed = 0;
        g_pkg[i].inst_ver[0] = 0;
        g_pkg[i].inst_path[0] = 0;
        g_pkg[i].has_update = 0;
    }
    registry_merge("/APPS/STORE.DB");
    if (!g_sysscope) {
        char up[PKGDEST_MAX];
        user_registry_path(up, sizeof(up));
        registry_merge(up);          // the user's own entries win: merged last
    }
}

// The per-user registry write. Uses the ordinary file path rather than
// SYS_PKG_WRITE because the destination is inside the user's own home, which
// they may simply open; userconf_write_all() is the shared "replace the whole
// file, report whether it actually landed" helper from #743, so a failed save
// cannot be reported as a good one and cannot destroy the previous file.
static int user_registry_write(const char *data, unsigned len) {
    char up[PKGDEST_MAX];
    user_registry_path(up, sizeof(up));
    if (!up[0]) return -1;
    // <home>/CONFIG exists since #745 (users_make_home_skeleton) and is created
    // on demand by userconf_open_write(); mkdir here covers a home that
    // predates the change. Result ignored: "already exists" and "created" are
    // equally fine and the write below is the real test.
    {
        char dir[PKGDEST_MAX];
        int cut = 0;
        for (int i = 0; up[i]; i++) if (up[i] == '/') cut = i;
        if (cut > 0 && cut < (int)sizeof(dir)) {
            for (int k = 0; k < cut; k++) dir[k] = up[k];
            dir[cut] = 0;
            sys_mkdir(dir, 0755);
        }
    }
    return userconf_write_all(up, data, len);
}

// #745 (#77): `exec_path` is the path this install actually wrote and is
// recorded as an optional third field. It may be NULL/empty (a theme or
// wallpaper has nothing to launch), in which case the line keeps the historical
// two-field shape.
static void registry_set(const char *id, const char *ver, const char *exec_path) {
    static char buf[8192];
    int len = 0;
    char upath[PKGDEST_MAX];
    user_registry_path(upath, sizeof(upath));
    const char *rpath = g_ins_sys ? "/APPS/STORE.DB" : upath;
    int fd = (rpath && rpath[0]) ? sys_open(rpath, O_RDONLY) : -1;
    if (fd >= 0) { len = sys_read(fd, buf, sizeof(buf) - 1); sys_close(fd); if (len < 0) len = 0; }
    buf[len] = 0;
    // rebuild omitting any existing line for id
    static char out[8192];
    int o = 0;
    char *p = buf;
    while (*p) {
        char *ls = p;
        while (*p && *p != '\n') p++;
        int llen = p - ls;
        if (*p == '\n') p++;
        // does this line start with id + space?
        int idl = strlen(id);
        if (!(llen > idl && strncmp(ls, id, idl) == 0 && ls[idl] == ' ')) {
            for (int i = 0; i < llen && o < (int)sizeof(out) - 2; i++) out[o++] = ls[i];
            if (o < (int)sizeof(out) - 2) out[o++] = '\n';
        }
    }
    // append new
    const char *a = id;
    while (*a && o < (int)sizeof(out) - 2) out[o++] = *a++;
    if (o < (int)sizeof(out) - 2) out[o++] = ' ';
    const char *b = ver;
    while (*b && o < (int)sizeof(out) - 2) out[o++] = *b++;
    if (exec_path && exec_path[0] == '/') {
        if (o < (int)sizeof(out) - 2) out[o++] = ' ';
        const char *c = exec_path;
        // A space in the path would make the field unparseable on read-back and
        // there is no quoting in this format, so a path containing one is simply
        // not recorded: launch_pkg() then falls back to the guess, which is the
        // pre-existing behaviour rather than a corrupt registry.
        int spaced = 0;
        for (const char *q = exec_path; *q; q++) if (*q == ' ') { spaced = 1; break; }
        if (spaced) o--;
        else while (*c && o < (int)sizeof(out) - 2) out[o++] = *c++;
    }
    if (o < (int)sizeof(out) - 2) out[o++] = '\n';
    if (g_ins_sys) pkg_write("/APPS/STORE.DB", out, o);
    else            (void)user_registry_write(out, (unsigned)o);
}

// regini_register() (the /APPS/REGINI.CFG drop-and-rebuild writer) is GONE:
// it wrote a file nothing read (see the #<startmenu-rust> note above). Start-
// menu registration is now startmenu_register_app() (userland/libc/
// startmenu_reg.c), shared with the auto-updater instead of duplicated - see
// that file's header for the full rationale.

// ---------------------------------------------------------------------------
// Auto-update policy (/APPS/STORE.CFG "policy=off|notify|auto"). Read by the
// background updater daemon (/APPS/UPDATED) launched from cron.
// ---------------------------------------------------------------------------
enum { POL_OFF = 0, POL_NOTIFY = 1, POL_AUTO = 2 };
static int g_policy = POL_NOTIFY;

static void load_policy(void) {
    int fd = sys_open("/APPS/STORE.CFG", O_RDONLY);
    if (fd < 0) return;
    char buf[256]; int n = sys_read(fd, buf, sizeof(buf) - 1); sys_close(fd);
    if (n <= 0) return; buf[n] = 0;
    char *p = strstr(buf, "policy=");
    if (!p) return; p += 7;
    if (strncmp(p, "auto", 4) == 0) g_policy = POL_AUTO;
    else if (strncmp(p, "off", 3) == 0) g_policy = POL_OFF;
    else g_policy = POL_NOTIFY;
}

static void save_policy(void) {
    const char *v = g_policy == POL_AUTO ? "auto" : g_policy == POL_OFF ? "off" : "notify";
    char buf[64]; int o = 0;
    const char *pre = "policy=";
    for (int i = 0; pre[i]; i++) buf[o++] = pre[i];
    for (int i = 0; v[i]; i++) buf[o++] = v[i];
    buf[o++] = '\n';
    pkg_write("/APPS/STORE.CFG", buf, o);
}

// ---------------------------------------------------------------------------
// Install / update a package.
// ---------------------------------------------------------------------------
static void mkparents(const char *dest) {
    // create leading directories of an absolute path (best effort)
    //
    // #745: the buffer was 128 bytes with the length clamped to 127, so a
    // destination longer than that had its deepest parent directories silently
    // not created and the member write then failed with a confusing error. A
    // per-user destination carries a home prefix and is longer than a system
    // one, which makes the old bound reachable. PKGDEST_MAX is the same bound
    // the confinement uses, so a path that survived pkgdest_confine() always
    // fits here.
    char tmp[PKGDEST_MAX];
    int n = strlen(dest); if (n > PKGDEST_MAX - 1) n = PKGDEST_MAX - 1;
    for (int i = 1; i < n; i++) {
        if (dest[i] == '/') {
            int j;
            for (j = 0; j < i; j++) tmp[j] = dest[j];
            tmp[j] = 0;
            sys_mkdir(tmp, 0755);
        }
    }
}

// #B2/#611: best-effort POST to /api/item/<id>/download, called ONLY after a
// package has actually finished installing (see the single call site below,
// which runs after pk->installed = 1) - never on a button click and never on
// download start, so a cancelled/failed install is never counted. This is
// pure social bookkeeping - a failure here does not undo or fail the
// install; the app is already on disk and usable either way (main.c:23's
// trust boundary: these numbers are display-only and can never influence an
// install decision).
//
// The kernel's sys_http_post_start() refuses any URL that does not start
// "https://" (see its comment in kernel/proc/syscall.c) - a deliberate,
// OS-wide policy this app does not weaken just to make a LAN test repo
// (STORE.SRC pointing at plain http://) convenient. That combination used to
// fail completely silently, which is exactly why download_log stayed empty
// through every dev-test pass. Returns 1 if the server actually counted the
// download, 0 otherwise - the caller uses this to say so instead of staying
// silent (#611).
/* #616: what the stats POST actually achieved. */
#define BUMP_FAILED   0   /* not attempted, or the server did not accept it */
#define BUMP_COUNTED  1   /* the server recorded a new download */
#define BUMP_DEDUPED  2   /* accepted, but inside the server's 6h per-IP window */
static int bump_download(int idx) {
    pkg_t *pk = &g_pkg[idx];
    if (strncmp(g_repo, "https://", 8) != 0) return 0;  // would be refused before any network I/O
    char path[80]; strcpy(path, "api/item/");
    strncat(path, pk->id, sizeof(path) - strlen(path) - 1);
    strncat(path, "/download", sizeof(path) - strlen(path) - 1);
    static char url[224]; repo_url(path, url, (int)sizeof(url));
    static char resp[256];
    int st = http_post_json(url, "{}", resp, sizeof(resp));
    if (st > 0 && st < 400) {
        pk->download_count = json_int(resp, resp + strlen(resp), "download_count");
        /* #616: the stats server de-duplicates to at most one counted download
         * per (item, client IP) per 6h and SAYS SO with "counted": false. That
         * field was ignored, so a POST the server deliberately did not count
         * was reported to the user as a recorded download. Three outcomes now,
         * not two. A server that predates the field is treated as counted. */
        if (!strstr(resp, "\"counted\"")) return BUMP_COUNTED;
        return json_bool(resp, resp + strlen(resp), "counted") ? BUMP_COUNTED
                                                               : BUMP_DEDUPED;
    }
    return BUMP_FAILED;
}

// #B2: submit a 1..5 star rating for the open detail package. Updates the
// on-screen average/count from the server's response on success so the user
// sees their rating reflected immediately, without a full catalog refetch.
static void submit_rating(int idx, int stars) {
    pkg_t *pk = &g_pkg[idx];
    if (stars < 1 || stars > 5) return;
    char path[80]; strcpy(path, "api/item/");
    strncat(path, pk->id, sizeof(path) - strlen(path) - 1);
    strncat(path, "/rating", sizeof(path) - strlen(path) - 1);
    static char url[224]; repo_url(path, url, (int)sizeof(url));
    char body[24]; strcpy(body, "{\"stars\":");
    char nb[4]; gui_itoa(stars, nb, sizeof(nb));
    strncat(body, nb, sizeof(body) - strlen(body) - 1);
    strncat(body, "}", sizeof(body) - strlen(body) - 1);
    static char resp[256];
    int st = http_post_json(url, body, resp, sizeof(resp));
    if (st > 0 && st < 400) {
        pk->rating_avg100 = json_fixed2(resp, resp + strlen(resp), "rating_avg");
        pk->rating_count  = json_int(resp, resp + strlen(resp), "rating_count");
        strcpy(g_status, "Thanks for rating "); strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
        strncat(g_status, "!", sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 2;
    } else {
        strcpy(g_status, "Could not submit rating (network)");
        g_status_kind = 3;
    }
}

static int install_pkg(int idx) {
    pkg_t *pk = &g_pkg[idx];
    g_status_kind = 1;
    strcpy(g_status, "Downloading ");
    strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
    strncat(g_status, "...", sizeof(g_status) - strlen(g_status) - 1);
    // draw_all is called by caller before this to show the banner.

    // ---- #570 + #559: acquire the package bytes and verify them against the
    // sha256 from the SIGNATURE-VERIFIED manifest BEFORE unpacking. For small
    // packages this is one fetch into a static buffer (unchanged); for larger
    // ones it is a chunked Range download streamed to disk and hashed on the
    // fly (peak RAM = one chunk). gzip CRC32 (the only prior integrity check)
    // detects corruption, not tampering; this digest check is what makes the
    // install authenticated. Fail closed: a package that does not match, or that
    // is too large for this build to buffer, is never unpacked and never
    // written to disk.
    /* #613: measured, not asserted. heap_end never shrinks, so the delta over
       the whole acquire+unpack is this app's peak heap growth for THIS package;
       comparing a 2.6MB package with a 103MB one is what proves the unpack is
       size-independent. Reported on the status line because a GUI app's printf
       never reaches the serial console. */
    size_t heap0 = malloc_heap_highwater();
    pkgsrc_t src;
    int ar = acquire_verified_package(pk, &src);
    if (ar != 0) return -1;   // g_status already set by the helper

    // ---- #613 pass 1: lift the INSTALL manifest out of the VERIFIED archive.
    // Its position is not fixed (OpenArena's sits after ~101MB of payload), so
    // the stream is walked until it turns up, then stopped (ARC_STOP).
    static man_sink_t ms;
    memset(&ms, 0, sizeof(ms));
    strncpy(ms.want, pk->id, sizeof(ms.want) - 10);
    strncat(ms.want, "/INSTALL", sizeof(ms.want) - strlen(ms.want) - 1);
    strcpy(g_status, "Unpacking ");
    strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
    strncat(g_status, "...", sizeof(g_status) - strlen(g_status) - 1);
    g_status_kind = 1; draw_all();

    int rc = run_stream(&src, &MAN_SINK, &ms, 0);
    if (rc != ARC_OK && !ms.found) {
        strcpy(g_status, "Package unpack failed (");
        strncat(g_status, arc_rc_text(rc), sizeof(g_status) - strlen(g_status) - 1);
        strncat(g_status, ")", sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 3;
        if (src.from_file) sys_unlink(STORE_TMP_PATH);
        return -1;
    }
    if (!ms.found) {
        strcpy(g_status, "No INSTALL manifest"); g_status_kind = 3;
        if (src.from_file) sys_unlink(STORE_TMP_PATH);
        return -1;
    }

    // ---- #613 pass 2: stream every mapped member straight to its destination.
    static unp_sink_t us;
    memset(&us, 0, sizeof(us));
    us.pkgid = pk->id; us.man = ms.buf; us.mlen = ms.len; us.fd = -1;
    rc = run_stream(&src, &UNP_SINK, &us, 1);
    if (rc != ARC_OK) {
        if (us.reject_dest[0]) {
            // #745: a destination the CLIENT refused, not one the kernel did.
            // Name the path the package asked for (not the rewritten one, which
            // would hide what the package tried to do) and the reason.
            strcpy(g_status, "REFUSED destination ");
            strncat(g_status, us.reject_dest, sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, ": ", sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, pkgdest_strerror(us.reject_rc),
                    sizeof(g_status) - strlen(g_status) - 1);
        } else if (us.fail_dest[0]) {
            // #745: NAME THE PATH AND THE ERRNO. A -13 here is not a disk
            // fault: it is this account being refused a write to a system
            // directory. The package downloaded and verified fine; what failed
            // is authority, and the message has to say so or the next person
            // goes looking for a bad sector. MayteraOS has no sudo and no
            // elevation (#745), so installing into a root-owned location means
            // signing in as root.
            // g_status is 128 bytes, so the wording is kept short enough that
            // the PATH and the ERRNO - the two facts that identify the fault -
            // always survive rather than being strncat-truncated away.
            char nb[24];
            strcpy(g_status, "Cannot write ");
            strncat(g_status, us.fail_dest, sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, " rc=", sizeof(g_status) - strlen(g_status) - 1);
            fmt_long(us.fail_rc, nb);
            strncat(g_status, nb, sizeof(g_status) - strlen(g_status) - 1);
            // #745: this used to read "denied; sign in as root to install".
            // After per-user install that advice is wrong AND useless: the
            // destination is now inside the user's own home, so a -13 here does
            // not mean "you are not root", it means this account cannot write
            // its own profile. Say the thing the user can act on.
            // #745 FIX THE LIE, part 1. A -13 is not a disk fault and it is
            // not a mystery: it is a REFUSAL, and the message has to say which
            // refusal and what the user can do instead. "Sign in as root" was
            // deleted for being unactionable; "denied writing a system path"
            // was accurate and still left the user with nowhere to go.
            if (us.fail_rc == -13) {
                if (g_ins_sys && !g_sysscope)
                    strncat(g_status, " - not permitted for all users; use Install",
                            sizeof(g_status) - strlen(g_status) - 1);
                else if (g_sysscope)
                    strncat(g_status, " - denied writing a system path",
                            sizeof(g_status) - strlen(g_status) - 1);
                else
                    strncat(g_status, " - your home directory is not writable",
                            sizeof(g_status) - strlen(g_status) - 1);
            }
        } else {
            // #745 FIX THE LIE, part 2. This branch is reached when a write
            // failed with no destination recorded, and it said "Install failed
            // writing to disk" for EVERY cause including a permission refusal.
            // A denial presented as a hardware or capacity problem sends the
            // next person looking for a bad sector. arc_rc_text() already
            // carries the real reason; the sentence in front of it now says
            // "could not be written" and lets the reason speak, instead of
            // asserting a fault nobody measured.
            strcpy(g_status, us.write_err ? "Not installed: a file could not be written ("
                                          : "Package REFUSED (");
            strncat(g_status, arc_rc_text(rc), sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, ")", sizeof(g_status) - strlen(g_status) - 1);
        }
        g_status_kind = 3;
        if (src.from_file) sys_unlink(STORE_TMP_PATH);
        return -1;
    }
    if (src.from_file) sys_unlink(STORE_TMP_PATH);

    char *man = ms.buf;
    size_t mlen = ms.len;
    char firstlaunch[PKGDEST_MAX];
    strncpy(firstlaunch, us.firstlaunch, sizeof(firstlaunch) - 1);
    firstlaunch[sizeof(firstlaunch) - 1] = 0;
    char theme_dest[PKGDEST_MAX];
    strncpy(theme_dest, us.theme_dest, sizeof(theme_dest) - 1);
    theme_dest[sizeof(theme_dest) - 1] = 0;
    int installed_files = us.count;

    // Fallback: if no lowercase alias was written, use the first /APPS/ dest
    // named by the manifest.
    //
    // #745 AND THIS IS THE NORMAL PATH, NOT AN EDGE CASE. install_dest_for()
    // returns the FIRST manifest line whose source matches, so a package that
    // maps one member to both "/APPS/COUNTER" and "/APPS/counter" (which every
    // app package in the repository does) only ever writes the FIRST, the
    // uppercase one. The sink's own firstlaunch only records a LOWERCASE alias,
    // so it is empty for those packages and this fallback always runs.
    //
    // MEASURED BUG THIS FIXES: the fallback took the destination STRAIGHT OUT
    // OF THE MANIFEST, unconfined. On a per-user install that produced a
    // Start-menu fragment reading "item: Counter | /APPS/COUNTER", pointing at
    // a system path that does not exist for that user, so the menu's existence
    // check silently dropped the entry and the app installed correctly and was
    // invisible. The install said "Installed Counter for ada" and it was true;
    // only the LAUNCH PATH was wrong. Confining it here is the same rewrite the
    // member writes get, through the same function, and for a root session
    // (sandbox "/") it is the identity, so root behaviour is unchanged.
    if (firstlaunch[0] == 0) {
        size_t p = 0;
        while (p < mlen) {
            char line[256]; int ll = 0;
            while (p < mlen && man[p] != '\n' && ll < 255) line[ll++] = man[p++];
            if (p < mlen) p++;
            line[ll] = 0;
            char *arrow = strstr(line, "->");
            if (!arrow) continue;
            char *d = arrow + 2; while (*d == ' ') d++;
            if (strncmp(d, "/APPS/", 6) == 0) {
                char raw[PKGDEST_MAX];
                int di = 0;
                while (d[di] && d[di] != ' ' && d[di] != '\r' && di < (int)sizeof(raw) - 1) {
                    raw[di] = d[di]; di++;
                }
                raw[di] = 0;
                if (pkgdest_confine(g_ins_home, raw, firstlaunch, sizeof(firstlaunch)) != PKGDEST_OK)
                    firstlaunch[0] = 0;   // fail closed: no menu entry beats a broken one
                break;
            }
        }
    }

    if (installed_files == 0) { strcpy(g_status, "Nothing installed"); g_status_kind = 3; return -1; }

    // #745: the bytes are on disk but at least one binary is not executable, so
    // it will be refused at launch (spawn_impl X_OK, #700 B8). Reporting a
    // plain success here is exactly the "a write reporting success may be doing
    // nothing" failure: the install looks fine and the app will not start.
    if (us.chmod_dest[0]) {
        strcpy(g_status, "Installed but not executable: ");
        strncat(g_status, us.chmod_dest, sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 3;
        return -1;
    }

    // Record the install (version tracking applies to every content type).
    // #745 (#77): the launch path goes in with it. Before this the registry held
    // only id+version, so the Open button had nothing to launch FROM and had to
    // reconstruct a path from the package id - a second opinion about something
    // the line above already knows exactly.
    registry_set(pk->id, pk->version, firstlaunch);
    strncpy(pk->inst_path, firstlaunch, sizeof(pk->inst_path) - 1);
    pk->inst_path[sizeof(pk->inst_path) - 1] = 0;

    // #B2: install-by-type branching. Only "app" (or a type-less legacy
    // entry) gets a Start-menu entry - a wallpaper or theme BMP is not
    // something to launch, so registering it there would put a non-app in the
    // menu (the thing task #B2 explicitly calls out to avoid).
    int is_app       = (pk->type[0] == 0 || strcmp(pk->type, "app") == 0);
    int is_wallpaper = (strcmp(pk->type, "wallpaper") == 0);
    int is_theme     = (strcmp(pk->type, "theme") == 0);

    // (#565) a real theme install: the package wrote a /THEMES/*.mtheme file
    // above (theme_dest[]); load it into the kernel's live table and make it
    // the active theme right away, the same call Settings' theme picker
    // uses, so a store-installed theme actually re-colors the OS instead of
    // only being a wallpaper BMP.
    int theme_applied = 0;
    if (is_theme && theme_dest[0]) {
        const char *base = theme_dest;
        for (const char *c = theme_dest; *c; c++) if (*c == '/') base = c + 1;
        int r = gui_theme_activate_path(theme_dest, base);
        theme_applied = (r >= 0);
    }

    int menu_ok = 1;
    if (is_app) {
        // #745: root registers into the all-users system layer exactly as
        // before; a user registers into their OWN layer, <home>/CONFIG/
        // STARTMENU/, which the compositor now feeds (sm_feed_user_layer()).
        // Writing the system layer as a non-root user would fail with -13,
        // because /CONFIG is root:root 0711 and stays that way.
        //
        // #745 (#77): pk->category is passed through so the entry lands in its
        // real group (OpenArena in Games) instead of the hardcoded "Installed"
        // every install used to get. The string is from the signed manifest and
        // is treated as UNTRUSTED all the same: startmenu_reg.c only COMPARES it
        // against a fixed table and writes one of its own literals, so it can
        // neither inject a directive nor invent a group. See sm_group_for().
        if (firstlaunch[0])
            menu_ok = (g_ins_sys ? startmenu_register_app(pk->id, pk->name, firstlaunch, pk->category)
                                  : startmenu_register_app_user(pk->id, pk->name, firstlaunch, pk->category)) == 0;
    } else if (is_wallpaper) {
        // Re-enumerate so this process's own view of the wallpaper set is
        // current; the compositor's live picker also re-scans on open (#B2
        // fix in wallpaper.c) so the new BMP is selectable without a reboot.
        wp_entry_t tmp[WP_MAX_ENTRIES];
        wp_enumerate(tmp, WP_MAX_ENTRIES);
    }
    // themes: no Start-menu registration - a palette is not launchable.

    pk->installed = 1;
    strncpy(pk->inst_ver, pk->version, sizeof(pk->inst_ver) - 1);
    pk->has_update = 0;

    size_t heap_peak = malloc_heap_highwater();
    int counted = bump_download(idx);   // #B2/#611: count this as a download regardless of type

    if (is_wallpaper) {
        strcpy(g_status, "Installed wallpaper: ");
        strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
        strncat(g_status, " - select it in Settings > Appearance", sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 2;
    } else if (is_theme) {
        strcpy(g_status, "Installed ");
        strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
        if (theme_applied) {
            // (#565) real palette swap: this package shipped a .mtheme file
            // and it is now the active system theme.
            strncat(g_status, " - theme applied", sizeof(g_status) - strlen(g_status) - 1);
            g_status_kind = 2;
        } else {
            // This package did not carry a .mtheme payload (an old
            // wallpaper-only "theme" package, docs/APPSTORE_SERVER.md #5).
            strncat(g_status, " - no palette file in this package (wallpaper only)",
                     sizeof(g_status) - strlen(g_status) - 1);
            g_status_kind = 1;
        }
    } else {
        strcpy(g_status, "Installed ");
        strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
        // #745: say WHERE it went. "Installed" alone hides the difference
        // between a system-wide install and one only this account can see,
        // which is the single most confusing thing about two scopes.
        if (g_ins_sys && !g_sysscope) {
            // Copy deck 8: an elevated install names WHO did it, and that line
            // is worth more than the prompt was. A harvested credential used
            // later still leaves a record the owner can see.
            strncat(g_status, " for all users by ", sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, scope_user_name(), sizeof(g_status) - strlen(g_status) - 1);
        } else if (!g_ins_sys) {
            strncat(g_status, " for ", sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, scope_user_name(), sizeof(g_status) - strlen(g_status) - 1);
        }
        if (!menu_ok)
            strncat(g_status, " (not added to the Start menu)",
                    sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 2;
    }

    // #611: this used to be silent either way. The install itself is
    // unaffected (bump_download()'s header comment), but a repo configured
    // over plain http (STORE.SRC pointing at a LAN dev server, not the
    // https:// production default) deserves to be TOLD its install count did
    // not move, instead of only discoverable by independently checking
    // <internal server path> on the stats server.
    // #611 left a hole this run MEASURED: it only spoke up when the repo was
    // plain http. With the shipped https repo the POST can still fail (it did:
    // a wp-cyber install over https://updates.maytera.net completed while
    // download_log stayed empty, and a curl POST to the same endpoint from the
    // same LAN returned 200 and DID insert a row - so the failure is on the
    // guest side, not the server's), and the user was told nothing at all.
    // Any uncounted install now says so, with which reason applies.
    if (counted == BUMP_DEDUPED) {
        /* #616: the POST WORKED. The server simply already has a download from
         * this machine for this package inside its 6h window. Saying "POST
         * failed" here would be a lie in the other direction. */
        strncat(g_status, " (already counted for this machine in the last 6h)",
                 sizeof(g_status) - strlen(g_status) - 1);
    } else if (counted != BUMP_COUNTED) {
        strncat(g_status,
                (strncmp(g_repo, "https://", 8) != 0)
                    ? " (count not recorded: repo is not https)"
                    : " (count not recorded: stats POST failed)",
                 sizeof(g_status) - strlen(g_status) - 1);
    }

    // #613: the bounded-memory claim, on screen, as a number.
    {
        char nb[24];
        strncat(g_status, " [heap ", sizeof(g_status) - strlen(g_status) - 1);
        fmt_ulong((unsigned long)(heap_peak / 1024), nb);
        strncat(g_status, nb, sizeof(g_status) - strlen(g_status) - 1);
        strncat(g_status, "K peak, +", sizeof(g_status) - strlen(g_status) - 1);
        fmt_ulong((unsigned long)((heap_peak - heap0) / 1024), nb);
        strncat(g_status, nb, sizeof(g_status) - strlen(g_status) - 1);
        strncat(g_status, "K for unpack]", sizeof(g_status) - strlen(g_status) - 1);
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Manifest fetch + parse
// ---------------------------------------------------------------------------
static int load_manifest(void) {
    g_npkg = 0;
    // Wait for the network stack to come up (the store can launch before DHCP
    // finishes), then retry the fetch a few times. #B3: this used to be a
    // silent usleep loop - up to 10s where the window never redrew - now it
    // shows a live "Waiting for network..." spinner (see http_get_live()'s
    // header comment for why a frozen-looking loading state was its own bug,
    // separate from whatever the fetch itself reports).
    for (int w = 0; w < 20 && !sys_net_is_up() && !g_close_requested; w++) {
        char s[32]; strcpy(s, "Waiting for network");
        static const char spin[4] = { '|', '/', '-', '\\' };
        int sl = strlen(s); s[sl]=' '; s[sl+1]=' '; s[sl+2]=spin[w & 3]; s[sl+3]=0;
        strcpy(g_status, s); g_status_kind = 1; draw_all();
        gui_event_t ev;
        if (win_get_event(g_win, &ev, 500) != 0 && ev.type == EVENT_WINDOW_CLOSE) g_close_requested = 1;
    }
    if (g_close_requested) return -1;
    int n = -1;
    for (int attempt = 0; attempt < 4 && n <= 0 && !g_close_requested; attempt++) {
        if (attempt) usleep(1000000);
        char murl[224]; repo_url("manifest.json", murl, (int)sizeof(murl));
        n = http_get_live(murl, (uint8_t *)g_manifest, sizeof(g_manifest) - 1, "Loading catalog");
    }
    if (g_close_requested) return -1;
    if (n <= 0) { strcpy(g_status, "Couldn't reach the App Repo server"); g_status_kind = 3; return -1; }
    g_manifest[n] = 0;

    // ---- #559: AUTHENTICATE THE MANIFEST BEFORE TRUSTING A SINGLE BYTE OF IT.
    // Everything downstream (package list, versions, and critically the per-
    // package sha256 values) is only trustworthy because of this check, so it
    // runs before the parse, not after. A repository that cannot present a
    // valid detached signature is refused outright.
    {
        int sn = -1;
        for (int attempt = 0; attempt < 3 && sn <= 0 && !g_close_requested; attempt++) {
            if (attempt) usleep(500000);
            char surl[224]; repo_url("manifest.json.sig", surl, (int)sizeof(surl));
            sn = http_get_live(surl, g_sig, (int)sizeof(g_sig), "Verifying signature");
        }
        if (g_close_requested) return -1;
        if (sn <= 0) {
            strcpy(g_status, "Repo signature missing - refusing to trust manifest");
            g_status_kind = 3; g_npkg = 0; return -1;
        }
        int vrc = pkgsig_verify_manifest(g_manifest, (size_t)n, g_sig, (size_t)sn);
        if (vrc != PKGSIG_OK) {
            strcpy(g_status, "Repo signature REFUSED: ");
            strncat(g_status, pkgsig_strerror(vrc), sizeof(g_status) - strlen(g_status) - 1);
            g_status_kind = 3; g_npkg = 0; return -1;
        }
    }

    // Find the "packages" array.
    char *pk = strstr(g_manifest, "\"packages\"");
    if (!pk) { strcpy(g_status, "Malformed manifest"); g_status_kind = 3; return -1; }
    char *arr = strchr(pk, '[');
    if (!arr) return -1;
    char *p = arr + 1;
    char *manend = g_manifest + n;

    while (p < manend && g_npkg < MAXPKG) {
        // find start of next object
        while (p < manend && *p != '{' && *p != ']') p++;
        if (p >= manend || *p == ']') break;
        // brace-match to find object end
        char *obj = p;
        int depth = 0;
        char *q = p;
        while (q < manend) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }
        char *end = q;
        pkg_t *e = &g_pkg[g_npkg];
        memset(e, 0, sizeof(*e));
        json_str(obj, end, "id",          e->id, sizeof(e->id));
        json_str(obj, end, "name",        e->name, sizeof(e->name));
        json_str(obj, end, "version",     e->version, sizeof(e->version));
        json_str(obj, end, "category",    e->category, sizeof(e->category));
        json_str(obj, end, "author",      e->author, sizeof(e->author));
        // #B2: prefer the B1 names (summary/preview_images) going forward, but
        // the legacy pre-B1 keys (tagline/screenshots) still work as a
        // fallback if a leaner manifest ever drops the aliases.
        json_str(obj, end, "summary",     e->tagline, sizeof(e->tagline));
        if (e->tagline[0] == 0) json_str(obj, end, "tagline", e->tagline, sizeof(e->tagline));
        json_str(obj, end, "path",        e->path, sizeof(e->path));
        json_str(obj, end, "sha256",      e->sha256, sizeof(e->sha256));
        json_str(obj, end, "description", e->desc, sizeof(e->desc));
        json_str(obj, end, "whatsnew",    e->whatsnew, sizeof(e->whatsnew));
        json_str_array_g(obj, end, "preview_images", (char *)e->shots, 96, MAXSHOT, &e->nshots);
        if (e->nshots == 0) json_str_array_g(obj, end, "screenshots", (char *)e->shots, 96, MAXSHOT, &e->nshots);
        json_str(obj, end, "type",       e->type, sizeof(e->type));
        json_str_array_g(obj, end, "tags", (char *)e->tags, TAG_LEN, MAXTAG, &e->ntags);
        e->size           = json_int(obj, end, "size");
        e->installed_size = json_int(obj, end, "installed_size");
        e->featured       = json_bool(obj, end, "featured");
        if (e->author[0] == 0) strcpy(e->author, "MayteraOS");
        if (e->type[0] == 0) strcpy(e->type, "app");   // legacy items predate B1's type field
        if (e->tagline[0] == 0) {
            trunc_fit(e->desc[0] ? e->desc : e->name, 12, 400, e->tagline, sizeof(e->tagline));
        }
        if (e->id[0]) g_npkg++;
        p = end;
    }
    load_registry();
    if (g_npkg == 0) { strcpy(g_status, "Repository is empty"); g_status_kind = 3; return -1; }
    g_status[0] = 0; g_status_kind = 0;
    return 0;
}

// ---------------------------------------------------------------------------
// #B2: live social stats (unsigned, display-only - see the file-header note).
// Fetches /api/catalog once and merges download_count/rating_avg/rating_count
// into the already-loaded, already-trusted g_pkg[] entries by id. A failure
// here is NOT fatal to the store (the manifest already verified and loaded
// fine): packages just show zero stats rather than blocking the whole app on
// a second, non-authoritative endpoint.
// ---------------------------------------------------------------------------
static char g_apicat[96 * 1024];

static void load_stats(void) {
    char url[224]; repo_url("api/catalog", url, (int)sizeof(url));
    int n = http_get(url, (uint8_t *)g_apicat, sizeof(g_apicat) - 1);
    if (n <= 0) return;   // no stats today; cards just show 0s, not an error
    g_apicat[n] = 0;
    char *pk = strstr(g_apicat, "\"packages\"");
    if (!pk) return;
    char *arr = strchr(pk, '[');
    if (!arr) return;
    char *p = arr + 1;
    char *end_all = g_apicat + n;
    while (p < end_all) {
        while (p < end_all && *p != '{' && *p != ']') p++;
        if (p >= end_all || *p == ']') break;
        char *obj = p;
        int depth = 0;
        char *q = p;
        while (q < end_all) {
            if (*q == '{') depth++;
            else if (*q == '}') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }
        char *oend = q;
        char id[32];
        json_str(obj, oend, "id", id, sizeof(id));
        if (id[0]) {
            for (int i = 0; i < g_npkg; i++) {
                if (strcmp(g_pkg[i].id, id) == 0) {
                    g_pkg[i].download_count = json_int(obj, oend, "download_count");
                    g_pkg[i].rating_avg100  = json_fixed2(obj, oend, "rating_avg");
                    g_pkg[i].rating_count   = json_int(obj, oend, "rating_count");
                    break;
                }
            }
        }
        p = oend;
    }
}

// ---------------------------------------------------------------------------
// Layout metrics
// ---------------------------------------------------------------------------
#define HEADER_H   58
#define SIDEBAR_W  186
#define CONTENT_X  (SIDEBAR_W)
#define PAD        22

// (#96) Reserve the scrollbar gutter unconditionally so the grid/detail layout
// never reflows the instant a list crosses the overflow threshold (the gutter
// simply sits unused, like Files' and Settings' reserved margins, when nothing
// needs to scroll: gui_scroll_draw() itself draws nothing in that case).
static int content_w(void) { return g_win_w - CONTENT_X - PAD - GUI_SCROLL_W - 6; }

// A card in the grid; also used for hit-testing.
#define CARD_W     288
#define CARD_H     132
#define CARD_GAP   16

// #B2: draw a 5-star row (read-only) for a rating_avg100 fixed-point value.
// The star nearest the fractional boundary gets a partial fill so a 4.3
// average visibly differs from a flat 4-star, not just in the number beside
// it. rating_avg100 == 0 (no ratings yet) draws all 5 stars empty.
static void draw_rating_stars(int x, int y, int sz, int gap, int rating_avg100,
                              uint32_t fill_c, uint32_t empty_c, uint32_t bg) {
    for (int s = 0; s < 5; s++) {
        int pct = rating_avg100 - s * 100;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        gui_fill_star_aa(g_win, x + s * (sz + gap), y, sz, pct, fill_c, empty_c, bg);
    }
}

// "4.4" from a *100 fixed-point average, or "-" when there are no ratings yet.
static void fmt_rating1(int avg100, char *out, int cap) {
    if (avg100 <= 0) { strncpy(out, "-", cap - 1); out[cap - 1] = 0; return; }
    int ip = avg100 / 100, fp = (avg100 % 100) / 10;
    char b[8]; gui_itoa(ip, b, sizeof(b));
    strncpy(out, b, cap - 1); out[cap - 1] = 0;
    strncat(out, ".", cap - strlen(out) - 1);
    char f[4]; gui_itoa(fp, f, sizeof(f));
    strncat(out, f, cap - strlen(out) - 1);
}

// ---------------------------------------------------------------------------
// Drawing: app icon tile (letter avatar, accent-tinted)
// ---------------------------------------------------------------------------
static uint32_t icon_tint(const char *id) {
    unsigned h = 2166136261u;
    for (const char *p = id; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    // pastel-ish hue derived from hash, blended toward accent for cohesion
    uint32_t base = 0;
    int r = 90 + (h & 0x7F);
    int g = 90 + ((h >> 7) & 0x7F);
    int b = 90 + ((h >> 14) & 0x7F);
    base = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    return gui_mix(base, C_accent, 90);
}

// #612 pass 2: `bg` is the colour of the pixels this tile ACTUALLY lands on.
// gui_fill_rounded_aa has no framebuffer read-back, so its rounded edge blends
// toward whatever background the caller declares; the hardcoded C_card here was
// right on a card tile and WRONG on the featured hero banner (a gradient) and
// on the detail page (C_surface), which left a visible pale fringe hugging the
// icon's curve on those coloured backgrounds - the same reported "stray white
// pixels around the curves" defect that #612 fixed for buttons, still live on
// the icon tile because the tile is a different call site, not a button.
// Every caller now passes its own real destination colour.
static void draw_icon(int x, int y, int sz, const char *id, const char *name, uint32_t bg) {
    uint32_t t = icon_tint(id);
    gui_fill_rounded_aa(g_win, x, y, sz, sz, sz / 5, t, bg);
    // letter
    char L[2]; L[0] = name[0]; if (L[0] >= 'a' && L[0] <= 'z') L[0] -= 32; L[1] = 0;
    int fs = sz * 3 / 5;
    int lw = TW(L, fs);
    T(x + (sz - lw) / 2, y + (sz - fs) / 2 - sz/16, L, fs, gui_ink_on(t));
}

// #B3: the one font size every pill uses to draw AND to measure itself for
// layout (draw_type_filter's pill widths, hero/card action buttons). Before
// this fix draw_type_filter() sized its pills from TW(label, 12) while
// draw_pill() drew (and centered) the same label at a hardcoded 13 - a real
// measure/render font-size mismatch that, stacked on top of the kerning bug
// described in gui_text_ttf_centered()'s comment, is why the type-filter
// pills ("Apps"/"Themes"/"Wallpapers") rendered with their text jammed
// against one edge instead of centered. One named constant, used by both the
// sizing and the drawing code, keeps that class of bug from coming back.
// #612: was a private hand-rolled capsule (h/2 radius) that called
// gui_fill_rounded_aa() directly with a hardcoded C_card background - correct
// only on a card tile, wrong on the featured hero banner, the detail page and
// the automatic-updates dialog, which is exactly why the button showed
// "stray light pixels around the curves" wherever it sat on anything else:
// the antialiased edge blended toward the WRONG assumed background (there is
// no framebuffer readback - see gui_fill_rounded_aa()'s header comment in
// libc/gui.c). It was also a fork of a primitive that already exists
// (gui_button()), which is the standing "never hand-roll a widget that
// already exists" rule.
//
// Now a thin adapter over the shared gui_button(): square-edged (house
// Motif style, docs/UI_STYLE_GUIDE.md) with no AA blend to get wrong, styled
// from the SAME palette/theme every other app uses, and it will pick up any
// future shared-engine improvement (focus rings, disabled state, etc.) with
// no further change here. label size follows the shared engine's
// GUI_TTF_SIZE (draw_type_filter's width measurement below matches it).
//
// kind: 0 = secondary/outlined (grid "Open"/"Installed" on a card, "< Back",
// filter chips), 1 = primary/accent ("Get"), 2 = muted secondary (installed,
// deprioritized), 3 = success/green ("Update").
static void draw_pill(int x, int y, int w, int h, const char *label, int kind, int hover) {
    gui_btn_variant_t variant = (kind == 1) ? GUI_BTN_PRIMARY
                               : (kind == 3) ? GUI_BTN_SUCCESS
                               :                GUI_BTN_SECONDARY;  // kind 0 and kind 2
    gui_state_t st = hover ? GUI_ST_HOVER : GUI_ST_NORMAL;
    gui_button(g_win, x, y, w, h, label, variant, st);
}

// primary action label + kind for a package. #B2: a wallpaper/theme has no
// executable to launch, so an installed one reads "Installed" rather than
// "Open" (which would silently no-op against sys_spawn - see do_action()).
static void action_for(pkg_t *pk, const char **label, int *kind) {
    int is_app = (pk->type[0] == 0 || strcmp(pk->type, "app") == 0);
    if (pk->has_update)      { *label = "Update";    *kind = 3; }
    else if (pk->installed)  { *label = is_app ? "Open" : "Installed"; *kind = 2; }
    else                     { *label = "Get";       *kind = 1; }
}

// ---------------------------------------------------------------------------
// Build the filtered list for the current view.
// ---------------------------------------------------------------------------
static int g_list[MAXPKG];
static int g_nlist = 0;

static int str_has_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca += 32;
            if (cb >= 'A' && cb <= 'Z') cb += 32;
            if (ca != cb) break;
            a++; b++;
        }
        if (!*b) return 1;
    }
    return 0;
}

static void build_list(void) {
    g_nlist = 0;
    for (int i = 0; i < g_npkg; i++) {
        pkg_t *pk = &g_pkg[i];
        int keep = 0;
        if (g_view == V_DISCOVER)      keep = 1;
        else if (g_view == V_CATEGORY) keep = (g_cat_sel >= 0 && strcmp(pk->category, g_cats[g_cat_sel].id) == 0);
        else if (g_view == V_INSTALLED) keep = pk->installed;
        else if (g_view == V_UPDATES)   keep = pk->has_update;
        else if (g_view == V_SEARCH) {
            keep = (str_has_ci(pk->name, g_search) || str_has_ci(pk->desc, g_search) ||
                    str_has_ci(pk->tagline, g_search) || str_has_ci(pk->category, g_search));
            // #B2: also match tags[] - "retro" should find a wallpaper tagged
            // retro even if the word never appears in its name/description.
            if (!keep) for (int t = 0; t < pk->ntags; t++)
                if (str_has_ci(pk->tags[t], g_search)) { keep = 1; break; }
        }
        // #B2: type filter (Apps/Themes/Wallpapers) applies on top of whatever
        // view/category/search already selected, so "Wallpapers" + a tag
        // search both narrow the same list rather than being exclusive modes.
        if (keep && g_type_sel > 0 && strcmp(pk->type, TYPE_VALUES[g_type_sel]) != 0) keep = 0;
        if (keep) g_list[g_nlist++] = i;
    }
}

static int update_count(void) {
    int c = 0; for (int i = 0; i < g_npkg; i++) if (g_pkg[i].has_update) c++; return c;
}

// ---------------------------------------------------------------------------
// Header + sidebar
// ---------------------------------------------------------------------------
static void draw_header(void) {
    // #B3: the header was a single flat fill + a 1px hairline - depth-free
    // next to Settings/Files' beveled/shadowed chrome (docs/UI_STYLE_GUIDE.md).
    // A faint top-lit gradient plus a soft drop shadow (same darken-and-offset
    // technique gui_soft_shadow() uses for cards) gives it a real edge over
    // the content below without inventing a new look.
    gui_fill_rounded_grad(g_win, 0, 0, g_win_w, HEADER_H, 0, gui_lighten(C_panel, 5), C_panel);
    gui_fill_rect(g_win, 0, HEADER_H - 1, g_win_w, 1, C_border);
    gui_fill_rect(g_win, 0, HEADER_H,     g_win_w, 1, gui_mix(C_surface, 0x00000000, 26));
    gui_fill_rect(g_win, 0, HEADER_H + 1, g_win_w, 1, gui_mix(C_surface, 0x00000000, 13));
    gui_fill_rect(g_win, 0, HEADER_H + 2, g_win_w, 1, gui_mix(C_surface, 0x00000000, 5));
    // logo mark
    gui_soft_shadow(g_win, 16, 14, 32, 32, 8, C_panel);
    gui_fill_rounded_aa(g_win, 16, 13, 32, 32, 8, C_accent, C_panel);
    T(24, 20, "M", 20, C_accent_ink);
    T(58, 12, "App Repo", 20, C_ink);
    T(58, 36, "MayteraOS Software", 11, C_ink_dim);

    // search box (right)
    int sw = 280; if (sw > g_win_w - 360) sw = g_win_w - 360; if (sw < 140) sw = 140;
    int sx = g_win_w - sw - 18, sy = 14, sh = 30;
    gui_fill_rounded_aa(g_win, sx, sy, sw, sh, sh / 2, gui_lighten(C_panel, 8), C_panel);
    gui_rounded_border(g_win, sx, sy, sw, sh, sh / 2, g_search_focus ? C_accent : C_border);
    const char *ph = g_search_len ? g_search : "Search name, summary, or tags";
    uint32_t pc = g_search_len ? C_ink : C_ink_dim;
    char shown[80];
    trunc_fit(ph, 13, sw - 30, shown, sizeof(shown));
    T(sx + 14, sy + 8, shown, 13, pc);
    if (g_search_focus) {
        int cw = TW(g_search_len ? g_search : "", 13);
        gui_fill_rect(g_win, sx + 14 + cw + 1, sy + 7, 1, 16, C_accent);
    }
}

// ONE geometry function for the nav rail, called by BOTH the draw pass and the
// hit test. blame.md, "Two panels, two arithmetic chains, one column: hit boxes
// that drift away from their controls": draw_sidebar() applied the CATEGORIES
// spacer only to row 3 (`if (i == 3) y += 8`) while sidebar_click() applied it
// to rows 3..8 (`if (i >= 3) y += 8`), so every category row below Games was
// DRAWN 8px above the rectangle that accepted its click. The defect is not the
// wrong constant, it is that there were two places to put one.
#define NAV_ROW_H   34
#define NAV_HIT_H   28
#define NAV_CAT_GAP 8       // spacer above the CATEGORIES section
static int nav_row_y(int i) {
    return HEADER_H + 14 + i * NAV_ROW_H + (i >= 3 ? NAV_CAT_GAP : 0);
}

// The ONE place a nav selection is applied.
//
// g_list[] is a CACHE of the filtered package indices, rebuilt only by
// build_list(). handle_click()'s sidebar branch was `if (sidebar_click(...))
// { draw_all(); }` with no build_list(), and draw_all() does not rebuild it
// either - so selecting a category updated g_view/g_cat_sel (the sidebar
// highlight and the content title both changed, which is why it looked wired
// up) and then repainted the PREVIOUS view's cached list. Every category, plus
// Installed and Updates, showed the Discover list. Every other view-changing
// site in this file already called build_list(); the sidebar was the one that
// forgot, which is the argument for making the pair inexpressible rather than
// adding a sixth call site that has to remember.
static void set_view(int view, int cat_sel) {
    g_view = view;
    if (view == V_CATEGORY) g_cat_sel = cat_sel;
    g_scroll = 0;
    build_list();
}

static void draw_sidebar(void) {
    // #B3: same flat-panel-plus-hairline problem as the header; add a matching
    // soft shadow band down its right edge so it reads as a raised rail over
    // the content, not a same-plane gray rectangle.
    gui_fill_rect(g_win, 0, HEADER_H, SIDEBAR_W, g_win_h - HEADER_H, C_panel);
    gui_fill_rect(g_win, SIDEBAR_W - 1, HEADER_H, 1, g_win_h - HEADER_H, C_border);
    gui_fill_rect(g_win, SIDEBAR_W,     HEADER_H, 1, g_win_h - HEADER_H, gui_mix(C_surface, 0x00000000, 26));
    gui_fill_rect(g_win, SIDEBAR_W + 1, HEADER_H, 1, g_win_h - HEADER_H, gui_mix(C_surface, 0x00000000, 13));
    gui_fill_rect(g_win, SIDEBAR_W + 2, HEADER_H, 1, g_win_h - HEADER_H, gui_mix(C_surface, 0x00000000, 5));

    // rows: Discover, Installed, Updates(n), --- categories ---
    int i = 0;
    struct { const char *lbl; int view; int cat; int badge; } rows[3 + NCAT];
    rows[0].lbl = "Discover";  rows[0].view = V_DISCOVER;  rows[0].cat = -1; rows[0].badge = 0;
    rows[1].lbl = "Installed"; rows[1].view = V_INSTALLED; rows[1].cat = -1; rows[1].badge = 0;
    rows[2].lbl = "Updates";   rows[2].view = V_UPDATES;   rows[2].cat = -1; rows[2].badge = update_count();
    for (int c = 0; c < NCAT; c++) {
        rows[3 + c].lbl = g_cats[c].label; rows[3 + c].view = V_CATEGORY; rows[3 + c].cat = c; rows[3 + c].badge = 0;
    }
    int total = 3 + NCAT;
    for (i = 0; i < total; i++) {
        int y = nav_row_y(i);
        int active = 0;
        if (rows[i].view == V_CATEGORY) active = (g_view == V_CATEGORY && g_cat_sel == rows[i].cat);
        else active = (g_view == rows[i].view);
        int hov = point_in(g_mx, g_my, 10, y - 4, SIDEBAR_W - 20, NAV_HIT_H);
        if (active) {
            gui_fill_rounded_aa(g_win, 10, y - 4, SIDEBAR_W - 20, NAV_HIT_H, 7, gui_mix(C_accent, C_panel, 150), C_panel);
            // #B3: a small accent rail on the selected row, the same "which
            // one is active" affordance Settings' own nav list uses, on top
            // of the tinted fill so it reads even at a glance.
            gui_fill_rect(g_win, 10, y - 2, 3, 24, C_accent);
        } else if (hov) gui_fill_rounded_aa(g_win, 10, y - 4, SIDEBAR_W - 20, NAV_HIT_H, 7, gui_lighten(C_panel, 8), C_panel);
        uint32_t tc = active ? C_accent : C_ink;
        T(22, y, rows[i].lbl, 14, tc);
        if (rows[i].badge > 0) {
            char b[8]; gui_itoa(rows[i].badge, b, sizeof(b));
            int bw = TW(b, 11) + 12;
            gui_fill_rounded_aa(g_win, SIDEBAR_W - 22 - bw, y - 1, bw, 18, 9, C_ok, C_panel);
            T(SIDEBAR_W - 22 - bw + 6, y + 1, b, 11, 0xFFFFFF);
        }
    }
    // "CATEGORIES" label above category rows
    T(22, nav_row_y(3) - 18, "CATEGORIES", 10, C_ink_dim);
}

// nav hit test -> sets view; returns 1 if handled
static int sidebar_click(int mx, int my) {
    int total = 3 + NCAT;
    for (int i = 0; i < total; i++) {
        int y = nav_row_y(i);
        if (point_in(mx, my, 10, y - 4, SIDEBAR_W - 20, NAV_HIT_H)) {
            if (i == 0)      set_view(V_DISCOVER,  -1);
            else if (i == 1) set_view(V_INSTALLED, -1);
            else if (i == 2) set_view(V_UPDATES,   -1);
            else             set_view(V_CATEGORY,  i - 3);
            return 1;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Card grid + hero
// ---------------------------------------------------------------------------
// Store card rects for hit testing during the last paint.
static struct { int idx, x, y, w, h, bx, by, bw, bh; } g_cardhit[MAXPKG];
static int g_ncardhit = 0;
static struct { int x, y, w, h; } g_polhit[3];
static int g_hero_idx = -1, g_hero_bx, g_hero_by, g_hero_bw, g_hero_bh;

static int draw_card(int x, int y, int w, pkg_t *pk, int idx) {
    int h = CARD_H;
    // shadow + card
    gui_soft_shadow(g_win, x, y + 2, w, h, 12, C_surface);
    gui_fill_rounded_aa(g_win, x, y, w, h, 12, C_card, C_surface);
    gui_rounded_border(g_win, x, y, w, h, 12, C_border);

    int ic = 56;
    // #B2: a fetched preview thumbnail replaces the letter-avatar tile once it
    // loads; ensure_thumb() is budget-gated so this card never blocks the
    // frame waiting on the network - it just shows the avatar a little longer.
    ensure_thumb(idx);
    if (g_thumb_w[idx] > 0) {
        gui_fill_rounded_aa(g_win, x + 14, y + 14, ic, ic, ic / 5, C_card, C_card);
        int dw = g_thumb_w[idx], dh = g_thumb_h[idx];
        if (dw > ic) dw = ic;
        if (dh > ic) dh = ic;
        win_draw_image(g_win, x + 14 + (ic - dw) / 2, y + 14 + (ic - dh) / 2, dw, dh, g_thumbpx[idx]);
    } else {
        draw_icon(x + 14, y + 14, ic, pk->id, pk->name, C_card);   // sits on the card
    }

    int tx = x + 14 + ic + 12;
    int tw = w - (14 + ic + 12) - 12;
    char nm[64]; trunc_fit(pk->name, 15, tw, nm, sizeof(nm));
    T(tx, y + 14, nm, 15, C_ink);
    char cat[40]; strcpy(cat, cat_label(pk->category));
    T(tx, y + 33, cat, 11, C_ink_dim);
    char tl[80]; trunc_fit(pk->tagline[0] ? pk->tagline : pk->desc, 12, tw, tl, sizeof(tl));
    T(tx, y + 50, tl, 12, C_ink_dim);

    // #B2: rating stars + review count, and a download count, merged in from
    // /api/catalog. A package with no ratings yet still shows 5 empty stars
    // and "no ratings" rather than nothing, so the row's presence is
    // consistent across the whole grid.
    int sy = y + 70;
    draw_rating_stars(tx, sy, 11, 2, pk->rating_avg100, C_accent, C_hair, C_card);
    char rt[8]; fmt_rating1(pk->rating_avg100, rt, sizeof(rt));
    int sx_after = tx + 5 * 13;
    T(sx_after + 6, sy - 1, rt, 11, C_ink_dim);
    char dl[32];
    if (pk->rating_count > 0) {
        strcpy(dl, "("); char c[8]; gui_itoa(pk->rating_count, c, sizeof(c));
        strncat(dl, c, sizeof(dl) - strlen(dl) - 1); strncat(dl, ")", sizeof(dl) - strlen(dl) - 1);
    } else strcpy(dl, "(no ratings)");
    T(sx_after + 6 + TW(rt, 11) + 6, sy - 1, dl, 11, C_ink_dim);
    // #611: a zero count is not "0 installs" - it is no data yet. Rendering
    // "0 installs" on every freshly-published or seed-reset package invents
    // a (negative) user-activity signal just as much as a fake positive one
    // would; hide the line entirely until there is a real count to show.
    if (pk->download_count > 0) {
        char dc[24]; gui_itoa(pk->download_count, dc, sizeof(dc));
        strncat(dc, pk->download_count == 1 ? " install" : " installs", sizeof(dc) - strlen(dc) - 1);
        int dcw = TW(dc, 11);
        T(x + w - 12 - dcw, sy - 1, dc, 11, C_ink_dim);
    }

    // action pill
    const char *lbl; int kind; action_for(pk, &lbl, &kind);
    int bw = 74, bh = 26;
    int bx = x + w - bw - 12, by = y + h - bh - 12;
    int hov = point_in(g_mx, g_my, bx, by, bw, bh);
    draw_pill(bx, by, bw, bh, lbl, kind, hov);

    // record hit
    g_cardhit[g_ncardhit].idx = idx;
    g_cardhit[g_ncardhit].x = x; g_cardhit[g_ncardhit].y = y; g_cardhit[g_ncardhit].w = w; g_cardhit[g_ncardhit].h = h;
    g_cardhit[g_ncardhit].bx = bx; g_cardhit[g_ncardhit].by = by; g_cardhit[g_ncardhit].bw = bw; g_cardhit[g_ncardhit].bh = bh;
    g_ncardhit++;
    return h;
}

// Draw the featured hero banner; returns height used.
static int draw_hero(int x, int y, int w) {
    // pick first featured package
    int fi = -1;
    for (int i = 0; i < g_npkg; i++) if (g_pkg[i].featured) { fi = i; break; }
    if (fi < 0 && g_nlist > 0) fi = g_list[0];
    else if (fi < 0 && g_npkg > 0) fi = 0;
    if (fi < 0) return 0;
    pkg_t *pk = &g_pkg[fi];
    g_hero_idx = fi;

    // #612: was 170, tuned only for the icon + name/tagline rows. It left
    // just 10px between the meta ("<category> - by <author>") line and the
    // action button, less than that line's own rendered height, so the
    // button's opaque pill painted over the bottom of the text - the
    // reported "Get button overlaps the second line of text" bug. Grown to
    // give the text column genuine, measured (not guessed) clearance above
    // the button - see the font_metrics() check below, which is the real
    // fix; this is just giving it somewhere to land.
    int h = 190;
    gui_soft_shadow(g_win, x, y + 3, w, h, 16, C_surface);
    gui_fill_rounded_grad(g_win, x, y, w, h, 16, C_hero2, C_hero1);
    gui_rounded_border(g_win, x, y, w, h, 16, C_border);

    uint32_t hi = gui_ink_on(gui_mix(C_hero1, C_hero2, 128));
    T(x + 26, y + 22, "FEATURED", 11, hi);
    int ic = 76;
    // #612 pass 2: the hero is a VERTICAL GRADIENT (gui_fill_rounded_grad above
    // paints row j as gui_mix(C_hero2, C_hero1, j*255/(h-1))), so the tile's AA
    // edge must blend toward the gradient colour at the tile's own mid-row, not
    // toward C_card. Sampling it here keeps the icon fringe-free even if the
    // hero height or the icon's offset changes later.
    uint32_t hero_bg = (h > 1) ? gui_mix(C_hero2, C_hero1, (44 + ic / 2) * 255 / (h - 1)) : C_hero2;
    draw_icon(x + 26, y + 44, ic, pk->id, pk->name, hero_bg);
    int tx = x + 26 + ic + 20;
    int max_tw = w - (tx - x) - 40;

    // #612: reserve the action button's rect BEFORE laying out the text
    // column (was computed after, so nothing ever checked whether a text row
    // actually fit above it).
    const char *lbl; int kind; action_for(pk, &lbl, &kind);
    int bw = 120, bh = 34;
    int bx = tx, by = y + h - bh - 22;
    int hov = point_in(g_mx, g_my, bx, by, bw, bh);

    T(tx, y + 46, pk->name, 26, hi);
    char tl[100]; trunc_fit(pk->tagline[0] ? pk->tagline : pk->desc, 14, max_tw, tl, sizeof(tl));
    T(tx, y + 82, tl, 14, hi);

    char meta[64];
    strcpy(meta, cat_label(pk->category));
    strncat(meta, "  \xb7  by ", sizeof(meta) - strlen(meta) - 1);
    strncat(meta, pk->author, sizeof(meta) - strlen(meta) - 1);
    char metaf[64]; trunc_fit(meta, 12, max_tw, metaf, sizeof(metaf));

    // #612: only draw the meta line if its REAL rendered height (from
    // font_metrics - ascent-descent - not a guessed line-height constant)
    // clears the button with a safety gap. A long author/category string, a
    // future font-size change, or a squeezed window can never overlap the
    // button again: the line is elided instead, rather than drawn and then
    // painted over.
    int meta_y = y + 104;
    int fm[3] = { 12, 0, 0 };
    font_metrics(0, 12, fm);
    int meta_line_h = fm[0] - fm[1]; if (meta_line_h <= 0) meta_line_h = 12;
    const int HERO_GAP = 10;
    if (meta_y + meta_line_h + HERO_GAP <= by) T(tx, meta_y, metaf, 12, hi);

    draw_pill(bx, by, bw, bh, lbl, kind == 2 ? 0 : kind, hov);
    g_hero_bx = bx; g_hero_by = by; g_hero_bw = bw; g_hero_bh = bh;
    return h;
}

// ---------------------------------------------------------------------------
// Screenshot cache for the detail page
// ---------------------------------------------------------------------------
static char g_shot_loaded[96] = {0};
static int  g_shot_w = 0, g_shot_h = 0;

static void load_shot(const char *rel) {
    if (strcmp(g_shot_loaded, rel) == 0) return;   // cached
    g_shot_loaded[0] = 0; g_shot_w = 0; g_shot_h = 0;
    if (!rel[0]) return;
    static char url[160];
    repo_url("", url, 160);
    strncat(url, rel, sizeof(url) - strlen(url) - 1);
    int n = http_get(url, g_shotraw, sizeof(g_shotraw));
    if (n <= 0) return;
    int dims[2] = {0, 0};
    int r = decode_image(g_shotraw, (unsigned)n, 520, 300, g_shotpx, sizeof(g_shotpx), dims);
    if (r > 0 && dims[0] > 0 && dims[1] > 0) {
        g_shot_w = dims[0]; g_shot_h = dims[1];
        strncpy(g_shot_loaded, rel, sizeof(g_shot_loaded) - 1);
    }
}

// #B2: lazily fetch + decode a card's preview thumbnail (its first preview
// image, downscaled to THUMB_SZ). Budget-gated: draw_content() resets
// g_thumb_budget to a small number at the top of each repaint, and this
// decrements it, so scrolling a long, all-uncached list can only kick off a
// couple of new network fetches per frame rather than blocking the whole grid
// on every missing thumbnail at once. Already-loaded and already-failed
// entries are marked "tried" so a permanently-unreachable image is not
// retried every single frame either.
static void ensure_thumb(int idx) {
    if (g_thumb_tried[idx] || g_thumb_w[idx] > 0) return;
    if (g_thumb_budget <= 0) return;
    pkg_t *pk = &g_pkg[idx];
    if (pk->nshots == 0) { g_thumb_tried[idx] = 1; return; }
    g_thumb_budget--;
    g_thumb_tried[idx] = 1;
    static char url[160];
    repo_url("", url, 160);
    strncat(url, pk->shots[0], sizeof(url) - strlen(url) - 1);
    int n = http_get(url, g_shotraw, sizeof(g_shotraw));
    if (n <= 0) return;
    int dims[2] = {0, 0};
    int r = decode_image(g_shotraw, (unsigned)n, THUMB_SZ, THUMB_SZ, g_thumbpx[idx],
                         sizeof(g_thumbpx[idx]), dims);
    if (r > 0 && dims[0] > 0 && dims[1] > 0) { g_thumb_w[idx] = dims[0]; g_thumb_h[idx] = dims[1]; }
}

// ---------------------------------------------------------------------------
// Detail page
// ---------------------------------------------------------------------------
static int g_detail_back_x, g_detail_back_y, g_detail_back_w, g_detail_back_h;
static int g_detail_act_x, g_detail_act_y, g_detail_act_w, g_detail_act_h;
// #745: the "Install for all users..." rect. w == 0 means the control is not
// on screen at all (root, or an already-installed package), which is a
// different thing from disabled and is why the hit test checks the width.
static int g_detail_act2_x, g_detail_act2_y, g_detail_act2_w, g_detail_act2_h;
static struct { int x, y, w, h, i; } g_thumbhit[MAXSHOT];
static int g_nthumb = 0;

static void draw_detail(void) {
    pkg_t *pk = &g_pkg[g_detail];
    int x = CONTENT_X + PAD;
    int w = content_w();
    int y = HEADER_H + 16 - g_scroll;

    // back button (fixed, drawn later over header not needed; put at top of content)
    int bkx = x, bky = HEADER_H + 14, bkw = 78, bkh = 28;
    // (drawn after content so it stays visible; store hit)
    g_detail_back_x = bkx; g_detail_back_y = bky; g_detail_back_w = bkw; g_detail_back_h = bkh;

    y = HEADER_H + 54 - g_scroll;

    // header block: icon + name + author + version + action
    int ic = 92;
    draw_icon(x, y, ic, pk->id, pk->name, C_surface);   // detail page background
    int tx = x + ic + 22;

    // #612: reserve the action button's rect BEFORE laying out the title, so
    // a long package name is elided (trunc_fit) instead of running under the
    // button - the same class of bug as the featured-hero overlap, just
    // horizontal here since the button sits to the right of the title on the
    // same row rather than below it.
    const char *lbl; int kind; action_for(pk, &lbl, &kind);
    int aw = 130, ah = 38;
    int ax = x + w - aw, ay = y + 6;
    int ahov = point_in(g_mx, g_my, ax, ay, aw, ah);
    const int DETAIL_GAP = 16;
    int name_max_w = (ax - DETAIL_GAP) - tx;

    char nm[80]; trunc_fit(pk->name, 26, name_max_w, nm, sizeof(nm));
    T(tx, y + 2, nm, 26, C_ink);
    char meta[80];
    strcpy(meta, "by "); strncat(meta, pk->author, sizeof(meta) - strlen(meta) - 1);
    T(tx, y + 40, meta, 13, C_ink_dim);
    char vc[64];
    strcpy(vc, "Version "); strncat(vc, pk->version, sizeof(vc) - strlen(vc) - 1);
    strncat(vc, "  \xb7  ", sizeof(vc) - strlen(vc) - 1);
    strncat(vc, cat_label(pk->category), sizeof(vc) - strlen(vc) - 1);
    T(tx, y + 60, vc, 12, C_ink_dim);
    // #745: the install SCOPE, stated before the user presses the button rather
    // than discovered afterwards. There is deliberately no scope CHOICE here:
    // system-wide install is a genuine privilege transition and is a separate
    // piece of work (flow B); offering a toggle that cannot yet work would be
    // worse than saying plainly what this build does.
    {
        char sc[128];
        if (g_sysscope) {
            // Copy deck 4. root's variant, and the sentence says why no prompt
            // is coming.
            strcpy(sc, "Installs to /APPS for every user.");
        } else if (g_detail_focus == 1 && g_may_elevate) {
            // Copy deck 5: while the elevating control has focus the scope line
            // states ITS consequence, so it is legible BEFORE activation, from
            // the keyboard.
            strcpy(sc, "Installs to /APPS for every user. Asks for your password.");
        } else {
            // Copy deck 3. "No password needed" is doing real work: it sets the
            // expectation that a password prompt HERE would be abnormal, which
            // is the whole basis of the anti-phishing property.
            strcpy(sc, "Installs for ");
            strncat(sc, scope_user_name(), sizeof(sc) - strlen(sc) - 1);
            strncat(sc, " only. No password needed.", sizeof(sc) - strlen(sc) - 1);
        }
        T(tx, y + 78, sc, 12, C_ink_dim);
    }

    draw_pill(ax, ay, aw, ah, lbl, kind == 2 ? 0 : kind,
              ahov || g_detail_focus == 0);
    g_detail_act_x = ax; g_detail_act_y = ay; g_detail_act_w = aw; g_detail_act_h = ah;

    // ---- #745 SURFACES A-2 and C -----------------------------------------
    // The second verb. Same size, same type as the primary; the ONLY resting
    // difference is the ellipsis, which is the long-established convention for
    // "this one is going to ask you something" and teaches the difference
    // BEFORE the click rather than after it.
    //
    // root never sees it: for root the two scopes are the same thing and the
    // system-wide one needs no prompt, so a second button would be two buttons
    // that do the same thing, one of which lies about asking.
    g_detail_act2_w = 0;
    if (!g_sysscope && !pk->installed) {
        int a2w = 200, a2h = 30;
        int a2x = ax + aw - a2w, a2y = ay + ah + 10;
        int a2hov = point_in(g_mx, g_my, a2x, a2y, a2w, a2h);
        if (g_may_elevate) {
            draw_pill(a2x, a2y, a2w, a2h, "Install for all users...", 0,
                      a2hov || g_detail_focus == 1);
            g_detail_act2_x = a2x; g_detail_act2_y = a2y;
            g_detail_act2_w = a2w; g_detail_act2_h = a2h;
        } else {
            // SURFACE C. Visible but disabled, and skipped by Tab. Hiding it
            // makes the capability undiscoverable and generates "why can't I";
            // disabling it with no explanation is the classic sin, which the
            // line below fixes. The label drops the ellipsis: it will not ask
            // you anything, because it will not do anything.
            //
            // This is NOT a dialog, and that is the point. A modal here would
            // be a prompt that appears every time and is dismissed every time,
            // which is exactly the reflex this whole design exists to avoid.
            // Nothing has failed: the state is computed BEFORE the buttons are
            // drawn, so the user never begins an action that will be refused.
            gui_rounded_border(g_win, a2x, a2y, a2w, a2h, 6, C_border);
            int lw = TW("Install for all users", 13);
            T(a2x + (a2w - lw) / 2, a2y + (a2h - 13) / 2, "Install for all users",
              13, C_ink_dim);
            const char *rf = "Only an administrator can install for all users.";
            int rw = TW(rf, 12);
            // ALWAYS VISIBLE, never a hover tooltip, never behind a click: this
            // line is the carrier of the meaning, which is what lets the
            // disabled label above sit below 4.5:1 under the WCAG 1.4.3
            // disabled-control exemption.
            T(ax + aw - rw, a2y + a2h + 6, rf, 12, C_ink);
        }
    }

    y += ic + 24;

    // #B2: community rating - a read-only average (stars + "4.4 (23 ratings)"
    // + download count) and, below it, an interactive 1..5 star control the
    // user can click to submit their own rating (POST /api/item/<id>/rating).
    {
        T(x, y, "Ratings", 15, C_ink); y += 24;
        draw_rating_stars(x, y, 16, 3, pk->rating_avg100, C_accent, C_hair, C_surface);
        char rt[8]; fmt_rating1(pk->rating_avg100, rt, sizeof(rt));
        int sxend = x + 5 * 19;
        T(sxend + 8, y, rt, 13, C_ink);
        char cnt[48];
        if (pk->rating_count > 0) {
            strcpy(cnt, " (");
            char c[8]; gui_itoa(pk->rating_count, c, sizeof(c));
            strncat(cnt, c, sizeof(cnt) - strlen(cnt) - 1);
            strncat(cnt, pk->rating_count == 1 ? " rating)" : " ratings)", sizeof(cnt) - strlen(cnt) - 1);
        } else strcpy(cnt, " (no ratings yet)");
        T(sxend + 8 + TW(rt, 13) + 2, y + 1, cnt, 12, C_ink_dim);
        // #611: hide the install count entirely at zero (see draw_card()).
        if (pk->download_count > 0) {
            char dlx[32]; gui_itoa(pk->download_count, dlx, sizeof(dlx));
            strncat(dlx, pk->download_count == 1 ? " install" : " installs", sizeof(dlx) - strlen(dlx) - 1);
            int dlw = TW(dlx, 12);
            int dlx_at = x + w - 260 - dlw - 8;
            if (dlx_at > sxend + 8 + TW(rt, 13) + TW(cnt, 12) + 20) T(dlx_at, y + 1, dlx, 12, C_ink_dim);
        }
        y += 32;

        const char *ratelbl = "Rate this:";
        T(x, y, ratelbl, 12, C_ink_dim);
        int rsx = x + TW(ratelbl, 12) + 10;
        int rsz = 22;
        for (int i = 0; i < 5; i++) {
            g_ratehit[i].x = rsx + i * (rsz + 4);
            g_ratehit[i].y = y - 4;
            g_ratehit[i].w = rsz;
            g_ratehit[i].h = rsz + 6;
        }
        g_rate_hover = -1;
        for (int i = 0; i < 5; i++)
            if (point_in(g_mx, g_my, g_ratehit[i].x, g_ratehit[i].y, g_ratehit[i].w, g_ratehit[i].h)) g_rate_hover = i;
        for (int i = 0; i < 5; i++) {
            int pct = (g_rate_hover >= 0 && i <= g_rate_hover) ? 100 : 0;
            gui_fill_star_aa(g_win, g_ratehit[i].x, g_ratehit[i].y, rsz, pct, C_accent, C_hair, C_surface);
        }
        y += rsz + 16;
    }

    // #B2: tag chips (also searchable from the header search box)
    if (pk->ntags > 0) {
        T(x, y, "Tags", 15, C_ink); y += 24;
        int cx2 = x, chy = y;
        int limit_w = x + w - 260;
        for (int i = 0; i < pk->ntags; i++) {
            int tw2 = TW(pk->tags[i], 11) + 18;
            if (cx2 + tw2 > limit_w && cx2 != x) { cx2 = x; chy += 26; }
            // #612: these chips sit directly on the detail page's scrolling
            // content area, which is C_surface (draw_all fills the whole
            // window with it) - NOT C_card. Passing C_card here was the same
            // "assumed background" mismatch as the Get button fringe, just
            // subtler because the antialiased chip corners are small.
            gui_fill_rounded_aa(g_win, cx2, chy, tw2, 20, 10, gui_mix(C_accent, C_card, 200), C_surface);
            T(cx2 + 9, chy + 4, pk->tags[i], 11, C_ink_dim);
            cx2 += tw2 + 8;
        }
        y = chy + 26 + 10;
    }

    // Screenshot gallery
    g_nthumb = 0;
    if (pk->nshots > 0) {
        T(x, y, "Preview", 15, C_ink); y += 26;
        // main screenshot area
        int gw = w; if (gw > 540) gw = 540;
        int gh = gw * 300 / 520;
        gui_fill_rounded_aa(g_win, x, y, gw, gh, 10, gui_darken(C_card, 4), C_surface);
        gui_rounded_border(g_win, x, y, gw, gh, 10, C_border);
        if (g_shot_sel >= pk->nshots) g_shot_sel = 0;
        // NOTE: screenshots are fetched in the click handler (ensure_shot), never
        // in the draw path, so a slow network never freezes the UI.
        if (g_shot_w > 0 && strcmp(g_shot_loaded, pk->shots[g_shot_sel]) == 0) {
            int dw = g_shot_w, dh = g_shot_h;
            int px = x + (gw - dw) / 2, py = y + (gh - dh) / 2;
            if (dw <= gw && dh <= gh)
                win_draw_image(g_win, px, py, dw, dh, g_shotpx);
        } else {
            T(x + 16, y + gh/2 - 6, "Loading preview...", 12, C_ink_dim);
        }
        y += gh + 12;
        // thumbnails row
        int thx = x;
        for (int i = 0; i < pk->nshots; i++) {
            int tw = 66, th = 44;
            int sel = (i == g_shot_sel);
            gui_fill_rounded_aa(g_win, thx, y, tw, th, 6, gui_darken(C_card, 2), C_surface);
            gui_rounded_border(g_win, thx, y, tw, th, 6, sel ? C_accent : C_border);
            char n[4]; gui_itoa(i + 1, n, sizeof(n));
            T(thx + tw/2 - 4, y + th/2 - 7, n, 12, sel ? C_accent : C_ink_dim);
            g_thumbhit[g_nthumb].x = thx; g_thumbhit[g_nthumb].y = y;
            g_thumbhit[g_nthumb].w = tw; g_thumbhit[g_nthumb].h = th; g_thumbhit[g_nthumb].i = i;
            g_nthumb++;
            thx += tw + 8;
        }
        y += 44 + 22;
    }

    // Description
    T(x, y, "About", 15, C_ink); y += 26;
    y = draw_wrapped(x, y, w - 260, pk->desc[0] ? pk->desc : pk->tagline, 13, 20, C_ink_dim);
    y += 10;

    // What's New
    if (pk->whatsnew[0]) {
        T(x, y, "What's New", 15, C_ink); y += 26;
        y = draw_wrapped(x, y, w - 260, pk->whatsnew, 13, 20, C_ink_dim);
        y += 10;
    }

    // Information panel (right column card)
    int panx = x + w - 240, pany = HEADER_H + 54 - g_scroll + 130;
    int panw = 240;
    int panh = 174;
    // draw an info card near top-right below the action
    int py = pany;
    gui_fill_rounded_aa(g_win, panx, py, panw, panh, 10, C_card, C_surface);
    gui_rounded_border(g_win, panx, py, panw, panh, 10, C_border);
    int iy = py + 14;
    struct { const char *k; char v[48]; } rows2[5];
    const char *typelbl = strcmp(pk->type, "wallpaper") == 0 ? "Wallpaper" :
                          strcmp(pk->type, "theme") == 0 ? "Theme" : "App";
    rows2[0].k = "Type";     strncpy(rows2[0].v, typelbl, 47); rows2[0].v[47]=0;
    rows2[1].k = "Category"; strncpy(rows2[1].v, cat_label(pk->category), 47); rows2[1].v[47]=0;
    rows2[2].k = "Version";  strncpy(rows2[2].v, pk->version, 47); rows2[2].v[47]=0;
    rows2[3].k = "Size";     { char s[24]; gui_itoa((pk->size + 1023) / 1024, s, sizeof(s)); strncpy(rows2[3].v, s, 40); rows2[3].v[40]=0; strcat(rows2[3].v, " KB"); }
    rows2[4].k = "Author";   strncpy(rows2[4].v, pk->author, 47); rows2[4].v[47]=0;
    for (int i = 0; i < 5; i++) {
        T(panx + 14, iy, rows2[i].k, 12, C_ink_dim);
        int vw = TW(rows2[i].v, 12);
        T(panx + panw - 14 - vw, iy, rows2[i].v, 12, C_ink);
        iy += 24;
        if (i < 4) gui_fill_rect(g_win, panx + 14, iy - 6, panw - 28, 1, C_hair);
    }
    if (pk->installed) {
        char st[64]; strcpy(st, "Installed: v"); strncat(st, pk->inst_ver, sizeof(st)-strlen(st)-1);
        T(panx + 14, iy + 2, st, 11, C_ok);
    }
    if (strcmp(pk->type, "theme") == 0) {
        T(panx + 14, iy + (pk->installed ? 18 : 2), "Palette needs a", 10, C_ink_dim);
        T(panx + 14, iy + (pk->installed ? 32 : 16), "theme loader to apply.", 10, C_ink_dim);
    }

    g_content_h = (y - (HEADER_H + 54 - g_scroll)) + 80;

    // Fixed back button on top
    int bhov = point_in(g_mx, g_my, bkx, bky, bkw, bkh);
    // repaint header strip region under back button not needed; draw pill
    draw_pill(bkx, bky, bkw, bkh, "< Back", 0, bhov);
}

// ---------------------------------------------------------------------------
// Main content (grid views)
// ---------------------------------------------------------------------------
// #B2: content-type filter pills (All Types / Apps / Themes / Wallpapers),
// shown above every list view. Returns the height consumed.
static int draw_type_filter(int x, int y) {
    int ph = 26, pad = 8;
    int cx = x;
    for (int i = 0; i < NTYPEFILT; i++) {
        // #B3/#612: measure at the SAME size (GUI_TTF_SIZE, what the shared
        // gui_button() - now behind draw_pill() - actually draws labels at)
        // and with the SAME kerning-free metric (gui_ttf_render_width) it
        // renders with, so the pill is sized to fit the label it gets.
        int lw = gui_ttf_render_width(TYPE_LABELS[i], GUI_TTF_SIZE);
        int pw = lw + 26;
        int on = (g_type_sel == i);
        int hov = point_in(g_mx, g_my, cx, y, pw, ph);
        draw_pill(cx, y, pw, ph, TYPE_LABELS[i], on ? 1 : 0, hov);
        g_typehit[i].x = cx; g_typehit[i].y = y; g_typehit[i].w = pw; g_typehit[i].h = ph;
        cx += pw + pad;
    }
    g_ntypehit = NTYPEFILT;
    return ph + 18;
}

// ---------------------------------------------------------------------------
// #B3: empty-state panel for the content grid. Before this, an empty g_nlist
// always drew the exact same small gray "No matching items." line whether
// the catalog genuinely had zero packages matching a filter/search OR the
// manifest fetch/signature check had failed outright and NOTHING ever
// loaded (g_npkg == 0) - so a real network/signature failure LOOKED like an
// empty store rather than an error, which is the literal "it says there's
// no apps" complaint. This distinguishes the two: a true fetch/signature
// failure gets its own styled panel with the SPECIFIC reason (reusing the
// g_status text load_manifest() already set) and a Retry action; a genuinely
// empty view/filter/search gets a softer, view-appropriate message. Also
// gives the app store a piece of chrome with real depth (shadow + rounded
// card + border) to sit in an otherwise flat, empty content area.
// ---------------------------------------------------------------------------
static struct { int x, y, w, h; int active; } g_retry_hit;

// mode: 0 = genuinely empty (view/filter/search turned up nothing), 1 =
// failed (manifest/signature never loaded - g_status carries why), 2 =
// still loading (the FIRST http_get_live() frame lands here too, since
// g_npkg is still 0 at that point - without this mode it showed "No
// matching items" WHILE the live spinner in the status banner was still
// ticking, a confusing "empty AND loading" contradiction on the very frame
// meant to prove the load is live, not stuck).
static void draw_empty_state(int x, int y, int w, int mode) {
    int failed = (mode == 1);
    int loading = (mode == 2);
    int panel_w = w; if (panel_w > 440) panel_w = 440;
    int px = x + (w - panel_w) / 2;
    int ph = failed ? 176 : 108;

    gui_soft_shadow(g_win, px, y + 2, panel_w, ph, 14, C_surface);
    gui_fill_rounded_aa(g_win, px, y, panel_w, ph, 14, C_card, C_surface);
    gui_rounded_border(g_win, px, y, panel_w, ph, 14, C_border);

    int ic = 48;
    int icx = px + (panel_w - ic) / 2, icy = y + 20;
    uint32_t icol = failed ? C_err : loading ? C_accent : C_ink_dim;
    gui_fill_rounded_aa(g_win, icx, icy, ic, ic, ic / 2, gui_mix(icol, C_card, 185), C_card);
    const char *glyph = failed ? "!" : loading ? "..." : "i";
    int gsz = loading ? 16 : 22;
    int gw = TW(glyph, gsz);
    T(icx + (ic - gw) / 2, icy + (ic - gsz) / 2, glyph, gsz, icol);

    const char *heading =
        failed                     ? "Couldn't load the App Repo" :
        loading                    ? "Loading the App Repo..." :
        g_view == V_UPDATES        ? "Everything is up to date" :
        g_view == V_INSTALLED      ? "Nothing installed yet" :
        g_view == V_SEARCH         ? "No results" :
                                      "No matching items";
    int hw = TW(heading, 15);
    T(px + (panel_w - hw) / 2, icy + ic + 16, heading, 15, C_ink);

    const char *detail =
        (failed && g_status[0])    ? g_status :
        loading                    ? "Fetching the catalog and verifying its signature." :
        g_view == V_UPDATES        ? "Every installed package is on its latest version." :
        g_view == V_INSTALLED      ? "Packages you install will show up here." :
        g_view == V_SEARCH         ? "Try a different name, category, or tag." :
                                      "Try All Types, or a different category.";
    char dl[96]; trunc_fit(detail, 11, panel_w - 40, dl, sizeof(dl));
    int dw = TW(dl, 11);
    T(px + (panel_w - dw) / 2, icy + ic + 38, dl, 11, C_ink_dim);

    g_retry_hit.active = 0;
    if (failed) {
        int bw = 100, bh = 30;
        int bx = px + (panel_w - bw) / 2, by = icy + ic + 62;
        int hov = point_in(g_mx, g_my, bx, by, bw, bh);
        draw_pill(bx, by, bw, bh, "Retry", 1, hov);
        g_retry_hit.x = bx; g_retry_hit.y = by; g_retry_hit.w = bw; g_retry_hit.h = bh;
        g_retry_hit.active = 1;
    }
}

static void draw_content(void) {
    g_ncardhit = 0;
    int x0 = CONTENT_X + PAD;
    int w = content_w();
    int y = HEADER_H + 18 - g_scroll;

    y += draw_type_filter(x0, y);

    // Title row
    const char *title = "Discover";
    if (g_view == V_CATEGORY && g_cat_sel >= 0) title = g_cats[g_cat_sel].label;
    else if (g_view == V_INSTALLED) title = "Installed";
    else if (g_view == V_UPDATES) title = "Updates";
    else if (g_view == V_SEARCH) title = "Search Results";

    if (g_view != V_DISCOVER) {
        T(x0, y, title, 22, C_ink);
        char sub[48]; gui_itoa(g_nlist, sub, sizeof(sub));
        // #B2: the grid can hold wallpapers/themes too now, not just apps.
        strncat(sub, g_nlist == 1 ? " item" : " items", sizeof(sub) - strlen(sub) - 1);
        T(x0, y + 30, sub, 12, C_ink_dim);
        y += 58;
        if (g_view == V_UPDATES) {
            // Automatic-updates policy control (Off / Notify / Auto).
            gui_fill_rounded_aa(g_win, x0, y, w, 56, 10, C_card, C_surface);
            gui_rounded_border(g_win, x0, y, w, 56, 10, C_border);
            T(x0 + 16, y + 10, "Automatic Updates", 14, C_ink);
            T(x0 + 16, y + 31, "How MayteraOS handles new versions of installed apps", 11, C_ink_dim);
            const char *pl[3] = { "Off", "Notify", "Auto" };
            int pw = 78, ph = 28, gap = 8;
            int px = x0 + w - (pw * 3 + gap * 2) - 16, py = y + 14;
            for (int i = 0; i < 3; i++) {
                int bx = px + i * (pw + gap);
                int on = (g_policy == i);
                int hov = point_in(g_mx, g_my, bx, py, pw, ph);
                draw_pill(bx, py, pw, ph, pl[i], on ? 1 : 0, hov);
                g_polhit[i].x = bx; g_polhit[i].y = py; g_polhit[i].w = pw; g_polhit[i].h = ph;
            }
            y += 70;
        }
    } else {
        // Discover: hero first
        int hh = draw_hero(x0, y, w);
        y += hh + 26;
        T(x0, y, "All Apps", 18, C_ink);
        y += 30;
    }

    if (g_nlist == 0) {
        // #B3: g_npkg == 0 means nothing has loaded yet - either it FAILED
        // (g_status_kind == 3: network/signature error, g_status carries
        // the reason) or it is STILL LOADING (g_status_kind == 1: the first
        // http_get_live() frame lands here too, before the fetch completes).
        // Only a genuine view/filter/search with a real manifest loaded
        // (g_npkg > 0) is "no matching items".
        int mode = (g_npkg > 0) ? 0 : (g_status_kind == 3) ? 1 : 2;
        draw_empty_state(x0, y + 6, w, mode);
        g_content_h = (y + (mode == 1 ? 210 : 140)) - (HEADER_H + 18 - g_scroll);
        return;
    }

    // Grid
    int cols = w / (CARD_W + CARD_GAP);
    if (cols < 1) cols = 1;
    int cw = (w - (cols - 1) * CARD_GAP) / cols;
    int col = 0;
    int rowy = y;
    for (int i = 0; i < g_nlist; i++) {
        int cx = x0 + col * (cw + CARD_GAP);
        // cull offscreen (still record nothing) - but we need hits so only skip draw far away
        if (rowy + CARD_H > HEADER_H && rowy < g_win_h) {
            draw_card(cx, rowy, cw, &g_pkg[g_list[i]], g_list[i]);
        } else {
            // still record hit for scroll-in-progress correctness
            g_cardhit[g_ncardhit].idx = g_list[i];
            g_cardhit[g_ncardhit].x = cx; g_cardhit[g_ncardhit].y = rowy;
            g_cardhit[g_ncardhit].w = cw; g_cardhit[g_ncardhit].h = CARD_H;
            g_cardhit[g_ncardhit].bx = cx + cw - 74 - 12; g_cardhit[g_ncardhit].by = rowy + CARD_H - 26 - 12;
            g_cardhit[g_ncardhit].bw = 74; g_cardhit[g_ncardhit].bh = 26;
            g_ncardhit++;
        }
        col++;
        if (col >= cols) { col = 0; rowy += CARD_H + CARD_GAP; }
    }
    if (col != 0) rowy += CARD_H + CARD_GAP;
    g_content_h = (rowy + 20) - (HEADER_H + 18 - g_scroll);
}

// ---------------------------------------------------------------------------
// Full repaint
// ---------------------------------------------------------------------------
static void draw_all(void) {
    // background
    gui_fill_rect(g_win, 0, 0, g_win_w, g_win_h, C_surface);

    // #B2: at most 2 new thumbnail fetches per repaint (see ensure_thumb).
    g_thumb_budget = 2;

    // content (scrolled), drawn first so header/sidebar mask overflow
    if (g_view == V_DETAIL) draw_detail();
    else draw_content();

    // mask top + left with header/sidebar
    draw_sidebar();
    draw_header();

    // (#96) Scrollbar. Configured from the g_content_h draw_detail()/draw_content()
    // just finished computing, and from the same viewport formula clamp_scroll()
    // already uses (g_win_h - HEADER_H - 20), so gui_scroll_max() and
    // clamp_scroll()'s `maxs` never disagree. g_scroll remains authoritative;
    // g_sb.offset is a read-back copy purely for the widget's own geometry math.
    // Draws nothing when the content fits (thumb_geom() returns early).
    {
        int vp_h = g_win_h - HEADER_H - 20;
        if (vp_h < 0) vp_h = 0;
        gui_scroll_config(&g_sb, g_win_w - PAD - GUI_SCROLL_W, HEADER_H, GUI_SCROLL_W, vp_h,
                          g_content_h, CARD_H + CARD_GAP);
        g_sb.offset = g_scroll;
        // gui_scroll_draw_on(), not gui_scroll_draw(): C_surface is
        // theme_color(THEME_COLOR_WINDOW_BG) EXCEPT when that theme value is
        // unset (setup_palette()'s wbg==0&&ink==0 guard, which substitutes
        // 0x1E1E1E) - gui_scroll_draw()'s own internal theme_color() call would
        // not see that substitution and could compute a repair against a
        // colour nothing on screen is actually painted with.
        gui_scroll_draw_on(g_win, &g_sb, C_surface);
    }

    // status banner
    if (g_status[0]) {
        int bw = TW(g_status, 13) + 32;
        int bx = CONTENT_X + (content_w() - bw) / 2 + PAD;
        int by = g_win_h - 46;
        uint32_t bg = g_status_kind == 2 ? C_ok : g_status_kind == 3 ? C_err : C_accent;
        gui_fill_rounded_aa(g_win, bx, by, bw, 30, 15, bg, C_surface);
        int tw = TW(g_status, 13);
        T(bx + (bw - tw) / 2, by + 8, g_status, 13, 0xFFFFFF);
    }

    win_invalidate(g_win);
}

// ---------------------------------------------------------------------------
// Launch an installed app
// ---------------------------------------------------------------------------
// #745: an app installed for this user lives in <home>/APPS and NOWHERE else,
// so a launcher that only knows "/APPS" cannot start it. Both directories are
// tried, the user's own first, in the lowercase-then-uppercase order this
// function already used (packages install both aliases). For a root session
// g_home_apps IS "/APPS", so the second pair is a duplicate of the first and a
// root launch behaves identically.
static int spawn_in_dir(const char *dir, const char *id) {
    char path[PKGDEST_MAX];
    int b = 0;
    for (int i = 0; dir[i] && b < (int)sizeof(path) - 66; i++) path[b++] = dir[i];
    if (b == 0 || path[b - 1] != '/') path[b++] = '/';
    int base = b;
    for (int i = 0; id[i] && b < (int)sizeof(path) - 1; i++) path[b++] = id[i];
    path[b] = 0;
    if (sys_spawn(path) >= 0) return 0;
    b = base;
    for (int i = 0; id[i] && b < (int)sizeof(path) - 1; i++) {
        char c = id[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        path[b++] = c;
    }
    path[b] = 0;
    return sys_spawn(path) >= 0 ? 0 : -1;
}

// #745 (#77) THE OPEN BUTTON. Returns 0 if a process was actually created.
//
// TWO THINGS WERE WRONG HERE, and only the second one is visible in a
// screenshot.
//
// 1. IT RETURNED void. Every sys_spawn() result was discarded and do_action()
//    printed "Launched <name>" unconditionally, so a launch that failed on all
//    four candidate paths was reported in the SUCCESS colour. Measured on
//    golden 1860: four "[FAT] fat_read_file: open failed" lines on the serial
//    console under a green "Launched Lemmings" banner. The store already knows
//    how to report this class ("Installed but not executable:"), so the silence
//    was an omission, not a missing capability.
//
// 2. IT GUESSED THE PATH. "<apps dir>/<pkg id>", lowercase then uppercase, is a
//    reconstruction of something install_pkg() had in its hand and threw away.
//    It happens to work whenever a package's binary is named after its id, which
//    is why it survived, but it is a second definition of the launch path and
//    the Start menu uses the FIRST one (the fragment's exec path). Two
//    definitions of one fact is how the two paths come to disagree, which is
//    exactly the shape of the reported "the start menu launches it, the store
//    does not" complaint. The registry now carries the real path, so the guess
//    is only a fallback for entries written by an older build.
static int launch_pkg(pkg_t *pk) {
    if (pk->inst_path[0] == '/' && sys_spawn(pk->inst_path) >= 0) return 0;
    if (spawn_in_dir(g_home_apps, pk->id) == 0) return 0;
    if (strcmp(g_home_apps, "/APPS") != 0 && spawn_in_dir("/APPS", pk->id) == 0) return 0;
    return -1;
}

// perform the primary action for a package
static void do_action(int idx) {
    pkg_t *pk = &g_pkg[idx];
    if (pk->has_update || !pk->installed) {
        g_status[0] = 0;
        // show downloading banner immediately
        strcpy(g_status, pk->has_update ? "Updating " : "Installing ");
        strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
        strncat(g_status, "...", sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 1;
        draw_all();
        install_pkg(idx);
        build_list();
        draw_all();
    } else if (pk->type[0] == 0 || strcmp(pk->type, "app") == 0) {
        if (launch_pkg(pk) == 0) {
            // #745 (#77): "Starting", not "Launched". MEASURED on golden 1860:
            // OpenArena's spawn returns immediately and the process then spends
            // ~60 s loading (a 30 MB BSS image, then ~200 MB of pk3 data off
            // USB-MSC) before it creates a window, so for that whole minute the
            // store is still the top window, the taskbar shows nothing new, and
            // the only thing on screen is this banner. A past-tense success
            // sentence in front of a screen where nothing has changed is what
            // makes a working launch read as a broken one. See task #78 for the
            // real fix (a visible starting-up affordance).
            strcpy(g_status, "Starting ");
            strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, " - a large app can take a minute to appear",
                    sizeof(g_status) - strlen(g_status) - 1);
            g_status_kind = 2;
        } else {
            // The failure is now NAMED. It was silent, which is how a launch
            // that never happened looked identical to one that did.
            strcpy(g_status, "Couldn't start ");
            strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, pk->inst_path[0] ? ": " : ": no installed binary found for ",
                    sizeof(g_status) - strlen(g_status) - 1);
            strncat(g_status, pk->inst_path[0] ? pk->inst_path : pk->id,
                    sizeof(g_status) - strlen(g_status) - 1);
            g_status_kind = 3;
        }
        draw_all();
    } else {
        // #B2: a wallpaper/theme is already installed and has nothing to
        // "open" (see action_for's honesty note) - point the user at where
        // it actually lives instead of silently no-op'ing sys_spawn.
        if (strcmp(pk->type, "wallpaper") == 0)
            strcpy(g_status, "Already installed - pick it in Settings > Appearance");
        else
            strcpy(g_status, "Already installed - palette needs a theme loader to apply");
        g_status_kind = 1;
        draw_all();
    }
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// #745 FLOW B: the system-wide install.
//
// WHAT THIS FUNCTION DOES NOT DO, and cannot: it does not draw a dialog, it
// does not read a keystroke, and it never sees the password. It asks the KERNEL
// to raise a prompt, the COMPOSITOR draws that prompt and authenticates against
// the caller's own account, and this function finds out only whether it was
// granted. If this app could call users_authenticate() itself the prompt would
// be theatre: an app that can read the password does not need to ask for it.
// ---------------------------------------------------------------------------
static void elev_status_for(long rc, const char *name) {
    switch (rc) {
    case ELEV_EROOT:
        // Unreachable from the button (root never sees it) but not silent if it
        // ever becomes reachable: root should just install, not be prompted.
        strcpy(g_status, "Already an administrator - use Install.");
        g_status_kind = 1; break;
    case ELEV_EPERM:
        strcpy(g_status, "Only an administrator can install for all users.");
        g_status_kind = 1; break;
    case ELEV_EBUSY:
        // REFUSED, not queued. Queueing would let an app stack prompts until
        // one is approved by fatigue.
        strcpy(g_status, "Another password prompt is already open.");
        g_status_kind = 1; break;
    case ELEV_ENOINPUT:
        strcpy(g_status, "Press the button again to ask for permission.");
        g_status_kind = 1; break;
    default:
        strcpy(g_status, "Not installed. ");
        strncat(g_status, name, sizeof(g_status) - strlen(g_status) - 1);
        strncat(g_status, " was not installed for all users.",
                sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 3; break;
    }
}

static void do_action_system(int idx) {
    pkg_t *pk = &g_pkg[idx];

    elev_request_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.name, pk->name, sizeof(req.name) - 1);
    strncpy(req.version, pk->version, sizeof(req.version) - 1);
    // HONEST LIMIT, stated where it is decided: this string is supplied by THIS
    // app and the kernel can only sanitise it, not verify it. The manifest
    // signature is checked in the kernel during acquire_verified_package(), but
    // nothing ties that verdict to this sentence, so a compromised App Store
    // could claim "signed" here. The fact rows are app-supplied DISPLAY data;
    // the title, "Installs to" and "Runs as" are the kernel's own chrome and
    // are the parts of that panel a user can rely on.
    strncpy(req.source, "MayteraOS App Repo", sizeof(req.source) - 1);

    long seq = sys_elev_request(&req);
    if (seq <= 0) { elev_status_for(seq, pk->name); draw_all(); return; }

    strcpy(g_status, "Waiting for your password...");
    g_status_kind = 1;
    draw_all();

    // The modal is up and the compositor owns every keystroke, so this app can
    // do nothing except wait for the verdict. sys_sleep() is the shared
    // primitive; there is deliberately no spin here.
    long st;
    for (;;) {
        st = sys_elev_status((unsigned long long)seq);
        if (st != ELEV_ST_OPEN) break;
        sys_sleep(80);
    }

    if (st != ELEV_ST_GRANTED) {
        // Cancelled is NOT an error and does not deserve a line of text, but
        // three wrong passwords is a thing that did not happen and must say so.
        g_status[0] = 0; g_status_kind = 0;
        elev_status_for(0, pk->name);
        draw_all();
        return;
    }

    // GRANTED. The kernel has put a bounded, path-scoped grant on THIS process.
    // Switch the ACTIVE INSTALL SCOPE for exactly the length of this install and
    // put it back whatever happens, so a failure cannot leave the app believing
    // it is root for the next package.
    char sh[PKGDEST_MAX], sa[PKGDEST_MAX];
    int  ss = g_ins_sys;
    strcpy(sh, g_ins_home); strcpy(sa, g_ins_apps);
    strcpy(g_ins_home, "/");
    strcpy(g_ins_apps, "/APPS");
    g_ins_sys = 1;

    strcpy(g_status, "Installing ");
    strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
    strncat(g_status, " for all users...", sizeof(g_status) - strlen(g_status) - 1);
    g_status_kind = 1;
    draw_all();
    install_pkg(idx);

    strcpy(g_ins_home, sh); strcpy(g_ins_apps, sa); g_ins_sys = ss;
    build_list();
    draw_all();
}

static void clamp_scroll(void) {
    int viewport = g_win_h - HEADER_H - 20;
    int maxs = g_content_h - viewport;
    if (maxs < 0) maxs = 0;
    if (g_scroll > maxs) g_scroll = maxs;
    if (g_scroll < 0) g_scroll = 0;
}

static void handle_click(int mx, int my) {
    // header search box
    int sw = 280; if (sw > g_win_w - 360) sw = g_win_w - 360; if (sw < 140) sw = 140;
    int sx = g_win_w - sw - 18;
    if (point_in(mx, my, sx, 14, sw, 30)) { g_search_focus = 1; return; }
    else g_search_focus = 0;

    if (my < HEADER_H) return;

    // sidebar
    if (mx < SIDEBAR_W) { if (sidebar_click(mx, my)) { draw_all(); } return; }

    // #B3: Retry on the "couldn't load the App Repo" empty-state panel.
    if (g_retry_hit.active && point_in(mx, my, g_retry_hit.x, g_retry_hit.y, g_retry_hit.w, g_retry_hit.h)) {
        strcpy(g_status, "Retrying..."); g_status_kind = 1; draw_all();
        if (load_manifest() == 0) load_stats();
        build_list(); draw_all();
        return;
    }

    if (g_view == V_DETAIL) {
        if (point_in(mx, my, g_detail_back_x, g_detail_back_y, g_detail_back_w, g_detail_back_h)) {
            set_view(V_DISCOVER, -1); draw_all(); return;
        }
        // #745: the elevating control. Checked BEFORE the primary because the
        // two rects never overlap and this keeps the reading order of the code
        // the same as the reading order on screen.
        if (g_detail_act2_w > 0 &&
            point_in(mx, my, g_detail_act2_x, g_detail_act2_y,
                     g_detail_act2_w, g_detail_act2_h)) {
            g_detail_focus = 1;
            do_action_system(g_detail);
            return;
        }
        if (point_in(mx, my, g_detail_act_x, g_detail_act_y, g_detail_act_w, g_detail_act_h)) {
            do_action(g_detail); return;
        }
        for (int i = 0; i < g_nthumb; i++)
            if (point_in(mx, my, g_thumbhit[i].x, g_thumbhit[i].y, g_thumbhit[i].w, g_thumbhit[i].h)) {
                g_shot_sel = g_thumbhit[i].i;
                if (g_pkg[g_detail].nshots > 0) load_shot(g_pkg[g_detail].shots[g_shot_sel]);
                draw_all(); return;
            }
        // #B2: interactive 1..5 star rating control - click star N to submit
        // an N-star rating for the open package.
        for (int i = 0; i < 5; i++)
            if (point_in(mx, my, g_ratehit[i].x, g_ratehit[i].y, g_ratehit[i].w, g_ratehit[i].h)) {
                submit_rating(g_detail, i + 1);
                draw_all(); return;
            }
        return;
    }

    // #B2: content-type filter pills (All Types / Apps / Themes / Wallpapers)
    for (int i = 0; i < g_ntypehit; i++)
        if (point_in(mx, my, g_typehit[i].x, g_typehit[i].y, g_typehit[i].w, g_typehit[i].h)) {
            g_type_sel = i; g_scroll = 0; build_list(); draw_all(); return;
        }

    // policy pills (Updates view)
    if (g_view == V_UPDATES) {
        for (int i = 0; i < 3; i++)
            if (point_in(mx, my, g_polhit[i].x, g_polhit[i].y, g_polhit[i].w, g_polhit[i].h)) {
                g_policy = i; save_policy(); draw_all(); return;
            }
    }

    // hero action / open detail
    if (g_view == V_DISCOVER && g_hero_idx >= 0) {
        if (point_in(mx, my, g_hero_bx, g_hero_by, g_hero_bw, g_hero_bh)) { do_action(g_hero_idx); return; }
    }

    // cards
    for (int i = 0; i < g_ncardhit; i++) {
        if (point_in(mx, my, g_cardhit[i].bx, g_cardhit[i].by, g_cardhit[i].bw, g_cardhit[i].bh)) {
            do_action(g_cardhit[i].idx); return;
        }
        if (point_in(mx, my, g_cardhit[i].x, g_cardhit[i].y, g_cardhit[i].w, g_cardhit[i].h)) {
            g_detail = g_cardhit[i].idx; g_view = V_DETAIL; g_scroll = 0; g_shot_sel = 0;
            // #745: focus belongs to the PAGE, not to the app. Carrying it over
            // left a freshly opened package showing "Asks for your password."
            // as its scope line while the primary button would have installed
            // per-user, which is the one sentence on this page that must never
            // describe a control the user is not on.
            g_detail_focus = 0;
            g_shot_loaded[0] = 0; g_shot_w = 0;
            draw_all();   // show the detail chrome immediately
            if (g_pkg[g_detail].nshots > 0) { load_shot(g_pkg[g_detail].shots[0]); draw_all(); }
            return;
        }
    }
}

static void handle_key(gui_event_t *ev) {
    if (!g_search_focus) {
        // arrow/back shortcuts
        if (ev->key_char == 27) { // ESC
            if (g_view == V_DETAIL) { set_view(V_DISCOVER, -1); draw_all(); }
            return;
        }
        // #745 KEYBOARD PATH for the detail page's action cluster. This is not
        // a convenience: #334 says pointer injection does not reliably land
        // clicks on this platform, so a control that can only be reached with
        // the mouse is a control that cannot be verified and, on real hardware
        // with a flaky pointer, cannot be used.
        //
        // Tab cycles the ENABLED controls in reading order; a DISABLED
        // secondary (Surface C) is skipped, so it can never be focused and
        // therefore can never be activated into a refusal. Enter/Space
        // activates the focused one. Focus starts on the primary, which is the
        // safe, no-prompt action, so the default landing spot is the harmless
        // one.
        if (g_view == V_DETAIL) {
            char k = ev->key_char;
            if (k == '\t') {
                g_detail_focus = (g_detail_focus == 0 && g_detail_act2_w > 0) ? 1 : 0;
                draw_all();
                return;
            }
            if (k == '\n' || k == '\r' || k == ' ') {
                if (g_detail_focus == 1 && g_detail_act2_w > 0)
                    do_action_system(g_detail);
                else
                    do_action(g_detail);
                return;
            }
        }
        return;
    }
    char c = ev->key_char;
    if (c == 8) { // backspace
        if (g_search_len > 0) g_search[--g_search_len] = 0;
    } else if (c == 27) {
        g_search_len = 0; g_search[0] = 0; g_search_focus = 0;
    } else if (c >= 32 && c < 127) {
        if (g_search_len < (int)sizeof(g_search) - 1) { g_search[g_search_len++] = c; g_search[g_search_len] = 0; }
    } else {
        return;
    }
    if (g_search_len > 0) { g_view = V_SEARCH; g_scroll = 0; }
    else if (g_view == V_SEARCH) { g_view = V_DISCOVER; }
    build_list();
    draw_all();
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
// #607: see the call site in main() for the full rationale. Reads one line
// "install <pkgid>" from /APPS/STORE.DO, resolves it against the catalog that
// was just loaded AND SIGNATURE-VERIFIED, and runs the ordinary install path.
static void scripted_install(void) {
    int fd = sys_open("/APPS/STORE.DO", O_RDONLY);
    if (fd < 0) return;
    char b[128];
    int n = sys_read(fd, b, (int)sizeof(b) - 1);
    sys_close(fd);
    sys_unlink("/APPS/STORE.DO");          // one shot, never a boot loop
    if (n <= 0) return;
    b[n] = 0;
    if (strncmp(b, "install ", 8) != 0) return;
    char id[64]; int o = 0;
    for (const char *p = b + 8; *p && *p != '\n' && *p != '\r' && *p != ' ' &&
                                o < (int)sizeof(id) - 1; p++)
        id[o++] = *p;
    id[o] = 0;
    if (!id[0]) return;
    for (int i = 0; i < g_npkg; i++) {
        if (strcmp(g_pkg[i].id, id) == 0) {
            strcpy(g_status, "Scripted install: ");
            strncat(g_status, g_pkg[i].name, sizeof(g_status) - strlen(g_status) - 1);
            g_status_kind = 1;
            draw_all();
            install_pkg(i);
            draw_all();
            return;
        }
    }
    strcpy(g_status, "Scripted install: no such package");
    g_status_kind = 3;
    draw_all();
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    g_win = win_create("App Repo", 60, 40, g_win_w, g_win_h);
    if (g_win < 0) return 1;

    // #745: FIRST, because load_registry(), every destination confinement and
    // every status message depend on knowing whose profile this is.
    scope_init();

    setup_palette();
    load_policy();
    repo_load();      // #559: repo source before any fetch
    strcpy(g_status, "Loading catalog...");
    g_status_kind = 1;
    draw_all();

    if (load_manifest() == 0 && !g_close_requested) load_stats();   // #B2: social stats are best-effort, never gate the store
    if (g_close_requested) { win_destroy(g_win); return 0; }   // closed while the live loading spinner was up
    build_list();
    draw_all();

    // ---- #607 scripted-install hook (test/kiosk aid, exactly the same shape
    // and rationale as the kernel /CONFIG/AUTORUN.CFG hook added under #317).
    // If /APPS/STORE.DO exists and holds a line "install <pkgid>", drive the
    // SAME install_pkg() the Get button drives, then remove the file so it
    // never repeats. It exists because QEMU pointer injection does not
    // reliably deliver a button press to this OS (#334), which makes the store
    // install path untestable end to end on a VM by clicking.
    // IT VERIFIES NOTHING LESS THAN A REAL CLICK DOES: install_pkg() is
    // entered unchanged, so the signed-manifest check (#559), the per-package
    // sha256 check and every fail-closed refusal still run. There is no bypass
    // flag and none can be requested through this file.
    scripted_install();

    gui_event_t ev;
    int running = 1;
    while (running) {
        int et = win_get_event(g_win, &ev, 120);
        if (et == 0) continue;
        switch (ev.type) {
            case EVENT_RESIZE:
                if (ev.mouse_x > 200 && ev.mouse_y > 200) { g_win_w = ev.mouse_x; g_win_h = ev.mouse_y; }
                draw_all();
                break;
            case EVENT_REDRAW:
                draw_all();
                break;
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;
            case EVENT_MOUSE_MOVE:
                g_mx = ev.mouse_x; g_my = ev.mouse_y;
                // (#96) thumb drag / hover. gui_scroll_motion() is a cheap no-op
                // unless a drag from gui_scroll_press() below is in progress or
                // the thumb is being hovered, so this costs nothing on the
                // common path (it already redraws unconditionally for hover
                // highlighting elsewhere in this handler).
                if (gui_scroll_motion(&g_sb, g_mx, g_my)) { g_scroll = g_sb.offset; clamp_scroll(); }
                draw_all();
                break;
            case EVENT_MOUSE_DOWN:
                g_mx = ev.mouse_x; g_my = ev.mouse_y;
                // (#96) Scrollbar gutter/thumb first: gui_scroll_press() only
                // returns 1 for a press inside its own 14px column, so this
                // cannot steal a click meant for a card, button or sidebar item.
                if (gui_scroll_press(&g_sb, g_mx, g_my)) {
                    g_scroll = g_sb.offset; clamp_scroll(); draw_all();
                } else {
                    handle_click(ev.mouse_x, ev.mouse_y);
                }
                break;
            case EVENT_MOUSE_UP:
                gui_scroll_release(&g_sb);
                break;
            case EVENT_MOUSE_SCROLL:
                g_scroll -= ev.scroll_delta * 48;
                clamp_scroll();
                draw_all();
                break;
            case EVENT_KEY_DOWN:
                handle_key(&ev);
                break;
            default: break;
        }
    }
    win_destroy(g_win);
    return 0;
}
