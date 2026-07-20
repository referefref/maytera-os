// appstore - MayteraOS App Store (task #402)
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
// the Start menu via /APPS/REGINI.CFG + a live desktop_menu_reload().

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/fcntl.h"
#include "../../libc/unistd.h"
#include "arc.h"

// Live Start-menu refresh after an install (kernel syscall added for #402).
#ifndef SYS_DESKTOP_MENU_RELOAD
#define SYS_DESKTOP_MENU_RELOAD 300
#endif
static inline void sys_desktop_menu_reload(void) { syscall0(SYS_DESKTOP_MENU_RELOAD); }

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
#define REPO_HOST     "<UPDATE_SERVER>"          // #97 update server (LXC maytera-update)
#define MANIFEST_URL  "http://<UPDATE_SERVER>/manifest.json"

// Route in-window text through the antialiased TrueType path.
static int g_win = -1;
static int g_win_w = 980, g_win_h = 680;

// ---------------------------------------------------------------------------
// Buffers
// ---------------------------------------------------------------------------
static char    g_manifest[128 * 1024];        // manifest.json text
static uint8_t g_dl[3 * 1024 * 1024];          // .mpkg download buffer
static uint8_t g_shotraw[1024 * 1024];         // raw screenshot bytes (BMP)
static uint32_t g_shotpx[560 * 340];           // decoded screenshot pixels (BGRA)

// ---------------------------------------------------------------------------
// Package model
// ---------------------------------------------------------------------------
#define MAXPKG   64
#define MAXSHOT  5
typedef struct {
    char id[32];
    char name[48];
    char version[16];
    char category[16];
    char author[40];
    char tagline[112];
    char path[96];
    char desc[640];
    char whatsnew[320];
    char shots[MAXSHOT][96];
    int  nshots;
    int  size;
    int  installed_size;
    int  featured;
    // runtime install state
    int  installed;
    char inst_ver[16];
    int  has_update;
} pkg_t;

static pkg_t g_pkg[MAXPKG];
static int   g_npkg = 0;

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
#define NCAT ((int)(sizeof(g_cats)/sizeof(g_cats[0])))

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
    int th = theme_get_active();
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

    // Feed the shared style engine so gui_* primitives match.
    gui_set_style(th == 2 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
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

// Parse an array-of-strings value ("screenshots":[ "a","b" ]) into pkg shots.
static void json_str_array(const char *obj, const char *end, const char *key,
                           char out[MAXSHOT][96], int *count) {
    *count = 0;
    const char *v = obj_find_key(obj, end, key);
    if (!v || v >= end || *v != '[') return;
    v++;
    while (v < end && *v != ']' && *count < MAXSHOT) {
        while (v < end && *v != '"' && *v != ']') v++;
        if (v >= end || *v == ']') break;
        v++;
        int o = 0;
        while (v < end && *v != '"' && o < 95) out[*count][o++] = *v++;
        out[*count][o] = 0;
        if (o > 0) (*count)++;
        while (v < end && *v != ',' && *v != ']') v++;
        if (v < end && *v == ',') v++;
    }
}

// ---------------------------------------------------------------------------
// Installed registry (/APPS/STORE.DB): "id version" per line.
// ---------------------------------------------------------------------------
static void load_registry(void) {
    for (int i = 0; i < g_npkg; i++) {
        g_pkg[i].installed = 0;
        g_pkg[i].inst_ver[0] = 0;
        g_pkg[i].has_update = 0;
    }
    int fd = sys_open("/APPS/STORE.DB", O_RDONLY);
    if (fd < 0) return;
    static char buf[8192];
    int n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    // parse lines
    char *p = buf;
    while (*p) {
        char id[32]; char ver[16];
        int i = 0;
        while (*p == ' ' || *p == '\n' || *p == '\r') p++;
        while (*p && *p != ' ' && *p != '\n' && i < 31) id[i++] = *p++;
        id[i] = 0;
        while (*p == ' ') p++;
        int j = 0;
        while (*p && *p != ' ' && *p != '\n' && *p != '\r' && j < 15) ver[j++] = *p++;
        ver[j] = 0;
        while (*p && *p != '\n') p++;
        if (id[0]) {
            for (int k = 0; k < g_npkg; k++) {
                if (strcmp(g_pkg[k].id, id) == 0) {
                    g_pkg[k].installed = 1;
                    strncpy(g_pkg[k].inst_ver, ver, sizeof(g_pkg[k].inst_ver) - 1);
                    if (strcmp(ver, g_pkg[k].version) != 0) g_pkg[k].has_update = 1;
                }
            }
        }
    }
}

static void registry_set(const char *id, const char *ver) {
    static char buf[8192];
    int len = 0;
    int fd = sys_open("/APPS/STORE.DB", O_RDONLY);
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
    if (o < (int)sizeof(out) - 2) out[o++] = '\n';
    pkg_write("/APPS/STORE.DB", out, o);
}

// Append/replace a Start-menu registration for an installed app.
static void regini_register(const char *name, const char *launch_path) {
    static char buf[16384];
    int len = 0;
    int fd = sys_open("/APPS/REGINI.CFG", O_RDONLY);
    if (fd >= 0) { len = sys_read(fd, buf, sizeof(buf) - 1); sys_close(fd); if (len < 0) len = 0; }
    buf[len] = 0;
    // Rebuild, dropping any prior APP= line whose name matches (idempotent).
    static char out[16384];
    int o = 0;
    char *p = buf;
    while (*p) {
        char *ls = p;
        while (*p && *p != '\n') p++;
        int llen = p - ls;
        if (*p == '\n') p++;
        int drop = 0;
        if (llen > 4 && strncmp(ls, "APP=", 4) == 0) {
            // APP=Name,icon,path  -> compare Name
            const char *nm = ls + 4;
            int nl = strlen(name);
            if (strncmp(nm, name, nl) == 0 && nm[nl] == ',') drop = 1;
        }
        if (!drop) {
            for (int i = 0; i < llen && o < (int)sizeof(out) - 2; i++) out[o++] = ls[i];
            if (o < (int)sizeof(out) - 2) out[o++] = '\n';
        }
    }
    // Ensure an "Installed" category header exists.
    if (!strstr(out, "CATEGORY=Installed")) {
        const char *hdr = "CATEGORY=Installed\n";
        for (int i = 0; hdr[i] && o < (int)sizeof(out) - 2; i++) out[o++] = hdr[i];
    }
    // APP=Name,terminal,/APPS/xxx
    const char *pre = "APP=";
    for (int i = 0; pre[i] && o < (int)sizeof(out) - 2; i++) out[o++] = pre[i];
    for (const char *a = name; *a && o < (int)sizeof(out) - 2; a++) out[o++] = *a;
    const char *mid = ",terminal,";
    for (int i = 0; mid[i] && o < (int)sizeof(out) - 2; i++) out[o++] = mid[i];
    for (const char *a = launch_path; *a && o < (int)sizeof(out) - 2; a++) out[o++] = *a;
    if (o < (int)sizeof(out) - 2) out[o++] = '\n';
    pkg_write("/APPS/REGINI.CFG", out, o);
}

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
static int find_entry(arc_entry *e, int n, const char *id, const char *rel) {
    // match "<id>/<rel>" exactly
    static char want[160];
    int w = 0;
    for (const char *a = id; *a && w < 158; a++) want[w++] = *a;
    if (w < 158) want[w++] = '/';
    for (const char *a = rel; *a && w < 159; a++) want[w++] = *a;
    want[w] = 0;
    for (int i = 0; i < n; i++) if (strcmp(e[i].name, want) == 0) return i;
    return -1;
}

static void mkparents(const char *dest) {
    // create leading directories of an absolute path (best effort)
    char tmp[128];
    int n = strlen(dest); if (n > 127) n = 127;
    for (int i = 1; i < n; i++) {
        if (dest[i] == '/') {
            int j;
            for (j = 0; j < i; j++) tmp[j] = dest[j];
            tmp[j] = 0;
            sys_mkdir(tmp, 0755);
        }
    }
}

static int install_pkg(int idx) {
    pkg_t *pk = &g_pkg[idx];
    g_status_kind = 1;
    strcpy(g_status, "Downloading ");
    strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
    strncat(g_status, "...", sizeof(g_status) - strlen(g_status) - 1);
    // draw_all is called by caller before this to show the banner.

    static char url[160];
    strcpy(url, "http://" REPO_HOST "/");
    strncat(url, pk->path, sizeof(url) - strlen(url) - 1);

    int n = http_get(url, g_dl, sizeof(g_dl));
    if (n <= 0) { strcpy(g_status, "Download failed"); g_status_kind = 3; return -1; }

    int count = 0;
    arc_entry *ents = arc_targz_extract(g_dl, (size_t)n, &count);
    if (!ents || count <= 0) { strcpy(g_status, "Package unpack failed"); g_status_kind = 3; return -1; }

    // Find and parse the INSTALL manifest.
    int ii = find_entry(ents, count, pk->id, "INSTALL");
    if (ii < 0) { arc_free_entries(ents, count); strcpy(g_status, "No INSTALL manifest"); g_status_kind = 3; return -1; }

    char *man = (char *)ents[ii].data;
    size_t mlen = ents[ii].size;
    char firstlaunch[96]; firstlaunch[0] = 0;
    int installed_files = 0;

    // Iterate INSTALL lines: "files/NAME -> /DEST"
    size_t p = 0;
    while (p < mlen) {
        char line[256]; int ll = 0;
        while (p < mlen && man[p] != '\n' && ll < 255) line[ll++] = man[p++];
        if (p < mlen) p++;
        line[ll] = 0;
        if (line[0] == '#' || line[0] == 0) continue;
        // split on "->"
        char *arrow = strstr(line, "->");
        if (!arrow) continue;
        char src[128]; char dest[128];
        int si = 0; char *s = line;
        while (s < arrow && *s == ' ') s++;
        while (s < arrow && *s != ' ' && si < 127) src[si++] = *s++;
        src[si] = 0;
        char *d = arrow + 2;
        while (*d == ' ') d++;
        int di = 0;
        while (*d && *d != ' ' && *d != '\r' && di < 127) dest[di++] = *d++;
        dest[di] = 0;
        if (!src[0] || !dest[0]) continue;
        // find payload entry <id>/<src>
        int fe = find_entry(ents, count, pk->id, src);
        if (fe < 0) continue;
        mkparents(dest);
        if (pkg_write(dest, ents[fe].data, ents[fe].size) >= 0)
            installed_files++;
        // remember a lowercase /APPS/ path to launch/register
        if (firstlaunch[0] == 0 && strncmp(dest, "/APPS/", 6) == 0) {
            // prefer a lowercase alias (launcher expects lowercase name)
            int lower = 1;
            for (const char *c = dest + 6; *c; c++) if (*c >= 'A' && *c <= 'Z') { lower = 0; break; }
            if (lower) strcpy(firstlaunch, dest);
        }
    }
    // fallback: if no lowercase alias, use the first /APPS/ dest by re-scanning
    if (firstlaunch[0] == 0) {
        p = 0;
        while (p < mlen) {
            char line[256]; int ll = 0;
            while (p < mlen && man[p] != '\n' && ll < 255) line[ll++] = man[p++];
            if (p < mlen) p++;
            line[ll] = 0;
            char *arrow = strstr(line, "->");
            if (!arrow) continue;
            char *d = arrow + 2; while (*d == ' ') d++;
            if (strncmp(d, "/APPS/", 6) == 0) { int di=0; while (d[di] && d[di]!=' ' && d[di]!='\r' && di<95){firstlaunch[di]=d[di];di++;} firstlaunch[di]=0; break; }
        }
    }

    arc_free_entries(ents, count);

    if (installed_files == 0) { strcpy(g_status, "Nothing installed"); g_status_kind = 3; return -1; }

    // Record install + register in the Start menu.
    registry_set(pk->id, pk->version);
    if (firstlaunch[0]) regini_register(pk->name, firstlaunch);
    sys_desktop_menu_reload();

    pk->installed = 1;
    strncpy(pk->inst_ver, pk->version, sizeof(pk->inst_ver) - 1);
    pk->has_update = 0;

    strcpy(g_status, "Installed ");
    strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
    g_status_kind = 2;
    return 0;
}

// ---------------------------------------------------------------------------
// Manifest fetch + parse
// ---------------------------------------------------------------------------
static int load_manifest(void) {
    g_npkg = 0;
    // Wait for the network stack to come up (the store can launch before DHCP
    // finishes), then retry the fetch a few times.
    for (int w = 0; w < 20 && !sys_net_is_up(); w++) usleep(500000);
    int n = -1;
    for (int attempt = 0; attempt < 4 && n <= 0; attempt++) {
        if (attempt) usleep(1000000);
        n = http_get(MANIFEST_URL, (uint8_t *)g_manifest, sizeof(g_manifest) - 1);
    }
    if (n <= 0) { strcpy(g_status, "Could not reach the app repository"); g_status_kind = 3; return -1; }
    g_manifest[n] = 0;

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
        json_str(obj, end, "tagline",     e->tagline, sizeof(e->tagline));
        json_str(obj, end, "path",        e->path, sizeof(e->path));
        json_str(obj, end, "description", e->desc, sizeof(e->desc));
        json_str(obj, end, "whatsnew",    e->whatsnew, sizeof(e->whatsnew));
        json_str_array(obj, end, "screenshots", e->shots, &e->nshots);
        e->size           = json_int(obj, end, "size");
        e->installed_size = json_int(obj, end, "installed_size");
        e->featured       = json_bool(obj, end, "featured");
        if (e->author[0] == 0) strcpy(e->author, "MayteraOS");
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
// Layout metrics
// ---------------------------------------------------------------------------
#define HEADER_H   58
#define SIDEBAR_W  186
#define CONTENT_X  (SIDEBAR_W)
#define PAD        22

static int content_w(void) { return g_win_w - CONTENT_X - PAD; }

// A card in the grid; also used for hit-testing.
#define CARD_W     288
#define CARD_H     104
#define CARD_GAP   16

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

static void draw_icon(int x, int y, int sz, const char *id, const char *name) {
    uint32_t t = icon_tint(id);
    gui_fill_rounded_aa(g_win, x, y, sz, sz, sz / 5, t, C_card);
    // letter
    char L[2]; L[0] = name[0]; if (L[0] >= 'a' && L[0] <= 'z') L[0] -= 32; L[1] = 0;
    int fs = sz * 3 / 5;
    int lw = TW(L, fs);
    T(x + (sz - lw) / 2, y + (sz - fs) / 2 - sz/16, L, fs, gui_ink_on(t));
}

// A pill button; returns nothing. state: 0 normal, 1 accent(primary), 2 installed, 3 update
static void draw_pill(int x, int y, int w, int h, const char *label, int kind, int hover) {
    uint32_t fill, ink;
    if (kind == 1)      { fill = hover ? gui_lighten(C_accent, 18) : C_accent; ink = C_accent_ink; }
    else if (kind == 2) { fill = C_hair; ink = C_ink_dim; }
    else if (kind == 3) { fill = hover ? gui_lighten(C_ok, 18) : C_ok; ink = 0xFFFFFF; }
    else                { fill = hover ? gui_lighten(C_card, 14) : C_card; ink = C_ink; }
    gui_fill_rounded_aa(g_win, x, y, w, h, h / 2, fill, C_card);
    if (kind == 0) gui_rounded_border(g_win, x, y, w, h, h / 2, C_border);
    int lw = TW(label, 13);
    T(x + (w - lw) / 2, y + (h - 13) / 2, label, 13, ink);
}

// primary action label + kind for a package
static void action_for(pkg_t *pk, const char **label, int *kind) {
    if (pk->has_update)      { *label = "Update"; *kind = 3; }
    else if (pk->installed)  { *label = "Open";   *kind = 2; }
    else                     { *label = "Get";    *kind = 1; }
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
        }
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
    gui_fill_rect(g_win, 0, 0, g_win_w, HEADER_H, C_panel);
    gui_fill_rect(g_win, 0, HEADER_H - 1, g_win_w, 1, C_border);
    // logo mark
    gui_fill_rounded_aa(g_win, 16, 13, 32, 32, 8, C_accent, C_panel);
    T(24, 20, "M", 20, C_accent_ink);
    T(58, 12, "App Store", 20, C_ink);
    T(58, 36, "MayteraOS Software", 11, C_ink_dim);

    // search box (right)
    int sw = 280; if (sw > g_win_w - 360) sw = g_win_w - 360; if (sw < 140) sw = 140;
    int sx = g_win_w - sw - 18, sy = 14, sh = 30;
    gui_fill_rounded_aa(g_win, sx, sy, sw, sh, sh / 2, gui_lighten(C_panel, 8), C_panel);
    gui_rounded_border(g_win, sx, sy, sw, sh, sh / 2, g_search_focus ? C_accent : C_border);
    const char *ph = g_search_len ? g_search : "Search apps";
    uint32_t pc = g_search_len ? C_ink : C_ink_dim;
    char shown[80];
    trunc_fit(ph, 13, sw - 30, shown, sizeof(shown));
    T(sx + 14, sy + 8, shown, 13, pc);
    if (g_search_focus) {
        int cw = TW(g_search_len ? g_search : "", 13);
        gui_fill_rect(g_win, sx + 14 + cw + 1, sy + 7, 1, 16, C_accent);
    }
}

static int nav_hit_y(int i) { return HEADER_H + 14 + i * 34; }

static void draw_sidebar(void) {
    gui_fill_rect(g_win, 0, HEADER_H, SIDEBAR_W, g_win_h - HEADER_H, C_panel);
    gui_fill_rect(g_win, SIDEBAR_W - 1, HEADER_H, 1, g_win_h - HEADER_H, C_border);

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
        int y = nav_hit_y(i);
        if (i == 3) {
            // category section label + spacer
            y += 8;
        }
        int active = 0;
        if (rows[i].view == V_CATEGORY) active = (g_view == V_CATEGORY && g_cat_sel == rows[i].cat);
        else active = (g_view == rows[i].view);
        int hov = point_in(g_mx, g_my, 10, y - 4, SIDEBAR_W - 20, 28);
        if (active) gui_fill_rounded_aa(g_win, 10, y - 4, SIDEBAR_W - 20, 28, 7, gui_mix(C_accent, C_panel, 150), C_panel);
        else if (hov) gui_fill_rounded_aa(g_win, 10, y - 4, SIDEBAR_W - 20, 28, 7, gui_lighten(C_panel, 8), C_panel);
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
    T(22, nav_hit_y(3) - 18, "CATEGORIES", 10, C_ink_dim);
}

// nav hit test -> sets view; returns 1 if handled
static int sidebar_click(int mx, int my) {
    int total = 3 + NCAT;
    for (int i = 0; i < total; i++) {
        int y = nav_hit_y(i);
        if (i >= 3) y += 8;
        if (point_in(mx, my, 10, y - 4, SIDEBAR_W - 20, 28)) {
            g_scroll = 0;
            if (i == 0) { g_view = V_DISCOVER; }
            else if (i == 1) { g_view = V_INSTALLED; }
            else if (i == 2) { g_view = V_UPDATES; }
            else { g_view = V_CATEGORY; g_cat_sel = i - 3; }
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
    draw_icon(x + 14, y + 14, ic, pk->id, pk->name);

    int tx = x + 14 + ic + 12;
    int tw = w - (14 + ic + 12) - 12;
    char nm[64]; trunc_fit(pk->name, 15, tw, nm, sizeof(nm));
    T(tx, y + 14, nm, 15, C_ink);
    char cat[40]; strcpy(cat, cat_label(pk->category));
    T(tx, y + 34, cat, 11, C_ink_dim);
    char tl[80]; trunc_fit(pk->tagline[0] ? pk->tagline : pk->desc, 12, tw, tl, sizeof(tl));
    T(tx, y + 52, tl, 12, C_ink_dim);

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

    int h = 170;
    gui_soft_shadow(g_win, x, y + 3, w, h, 16, C_surface);
    gui_fill_rounded_grad(g_win, x, y, w, h, 16, C_hero2, C_hero1);
    gui_rounded_border(g_win, x, y, w, h, 16, C_border);

    uint32_t hi = gui_ink_on(gui_mix(C_hero1, C_hero2, 128));
    T(x + 26, y + 22, "FEATURED", 11, hi);
    int ic = 76;
    draw_icon(x + 26, y + 44, ic, pk->id, pk->name);
    int tx = x + 26 + ic + 20;
    T(tx, y + 46, pk->name, 26, hi);
    char tl[100]; trunc_fit(pk->tagline[0] ? pk->tagline : pk->desc, 14, w - (tx - x) - 40, tl, sizeof(tl));
    T(tx, y + 82, tl, 14, hi);
    char meta[64];
    strcpy(meta, cat_label(pk->category));
    strncat(meta, "  \xb7  by ", sizeof(meta) - strlen(meta) - 1);
    strncat(meta, pk->author, sizeof(meta) - strlen(meta) - 1);
    T(tx, y + 104, meta, 12, hi);

    const char *lbl; int kind; action_for(pk, &lbl, &kind);
    int bw = 120, bh = 34;
    int bx = tx, by = y + h - bh - 22;
    int hov = point_in(g_mx, g_my, bx, by, bw, bh);
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
    strcpy(url, "http://" REPO_HOST "/");
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

// ---------------------------------------------------------------------------
// Detail page
// ---------------------------------------------------------------------------
static int g_detail_back_x, g_detail_back_y, g_detail_back_w, g_detail_back_h;
static int g_detail_act_x, g_detail_act_y, g_detail_act_w, g_detail_act_h;
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
    draw_icon(x, y, ic, pk->id, pk->name);
    int tx = x + ic + 22;
    T(tx, y + 2, pk->name, 26, C_ink);
    char meta[80];
    strcpy(meta, "by "); strncat(meta, pk->author, sizeof(meta) - strlen(meta) - 1);
    T(tx, y + 40, meta, 13, C_ink_dim);
    char vc[64];
    strcpy(vc, "Version "); strncat(vc, pk->version, sizeof(vc) - strlen(vc) - 1);
    strncat(vc, "  \xb7  ", sizeof(vc) - strlen(vc) - 1);
    strncat(vc, cat_label(pk->category), sizeof(vc) - strlen(vc) - 1);
    T(tx, y + 60, vc, 12, C_ink_dim);

    const char *lbl; int kind; action_for(pk, &lbl, &kind);
    int aw = 130, ah = 38;
    int ax = x + w - aw, ay = y + 6;
    int ahov = point_in(g_mx, g_my, ax, ay, aw, ah);
    draw_pill(ax, ay, aw, ah, lbl, kind == 2 ? 0 : kind, ahov);
    g_detail_act_x = ax; g_detail_act_y = ay; g_detail_act_w = aw; g_detail_act_h = ah;

    y += ic + 24;

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
    // draw an info card near top-right below the action
    int py = pany;
    gui_fill_rounded_aa(g_win, panx, py, panw, 150, 10, C_card, C_surface);
    gui_rounded_border(g_win, panx, py, panw, 150, 10, C_border);
    int iy = py + 14;
    struct { const char *k; char v[48]; } rows2[4];
    rows2[0].k = "Category"; strncpy(rows2[0].v, cat_label(pk->category), 47); rows2[0].v[47]=0;
    rows2[1].k = "Version";  strncpy(rows2[1].v, pk->version, 47); rows2[1].v[47]=0;
    rows2[2].k = "Size";     { char s[24]; gui_itoa((pk->size + 1023) / 1024, s, sizeof(s)); strncpy(rows2[2].v, s, 40); rows2[2].v[40]=0; strcat(rows2[2].v, " KB"); }
    rows2[3].k = "Author";   strncpy(rows2[3].v, pk->author, 47); rows2[3].v[47]=0;
    for (int i = 0; i < 4; i++) {
        T(panx + 14, iy, rows2[i].k, 12, C_ink_dim);
        int vw = TW(rows2[i].v, 12);
        T(panx + panw - 14 - vw, iy, rows2[i].v, 12, C_ink);
        iy += 24;
        if (i < 3) gui_fill_rect(g_win, panx + 14, iy - 6, panw - 28, 1, C_hair);
    }
    if (pk->installed) {
        char st[64]; strcpy(st, "Installed: v"); strncat(st, pk->inst_ver, sizeof(st)-strlen(st)-1);
        T(panx + 14, iy + 2, st, 11, C_ok);
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
static void draw_content(void) {
    g_ncardhit = 0;
    int x0 = CONTENT_X + PAD;
    int w = content_w();
    int y = HEADER_H + 18 - g_scroll;

    // Title row
    const char *title = "Discover";
    if (g_view == V_CATEGORY && g_cat_sel >= 0) title = g_cats[g_cat_sel].label;
    else if (g_view == V_INSTALLED) title = "Installed";
    else if (g_view == V_UPDATES) title = "Updates";
    else if (g_view == V_SEARCH) title = "Search Results";

    if (g_view != V_DISCOVER) {
        T(x0, y, title, 22, C_ink);
        char sub[48]; gui_itoa(g_nlist, sub, sizeof(sub));
        strncat(sub, g_nlist == 1 ? " app" : " apps", sizeof(sub) - strlen(sub) - 1);
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
        T(x0, y + 20, g_view == V_UPDATES ? "Everything is up to date." :
                      g_view == V_INSTALLED ? "No apps installed yet." :
                      "No matching apps.", 14, C_ink_dim);
        g_content_h = (y + 60) - (HEADER_H + 18 - g_scroll);
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

    // content (scrolled), drawn first so header/sidebar mask overflow
    if (g_view == V_DETAIL) draw_detail();
    else draw_content();

    // mask top + left with header/sidebar
    draw_sidebar();
    draw_header();

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
static void launch_pkg(pkg_t *pk) {
    // Try lowercase then uppercase /APPS/<id>
    char path[64];
    strcpy(path, "/APPS/");
    strncat(path, pk->id, sizeof(path) - strlen(path) - 1);
    int r = sys_spawn(path);
    if (r < 0) {
        // uppercase
        char up[64]; strcpy(up, "/APPS/");
        int b = strlen(up);
        for (int i = 0; pk->id[i] && b < 62; i++) { char c = pk->id[i]; if (c>='a'&&c<='z') c-=32; up[b++]=c; }
        up[b]=0;
        sys_spawn(up);
    }
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
    } else {
        launch_pkg(pk);
        strcpy(g_status, "Launched ");
        strncat(g_status, pk->name, sizeof(g_status) - strlen(g_status) - 1);
        g_status_kind = 2;
        draw_all();
    }
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
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

    if (g_view == V_DETAIL) {
        if (point_in(mx, my, g_detail_back_x, g_detail_back_y, g_detail_back_w, g_detail_back_h)) {
            g_view = V_DISCOVER; g_scroll = 0; build_list(); draw_all(); return;
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
        return;
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
            if (g_view == V_DETAIL) { g_view = V_DISCOVER; build_list(); draw_all(); }
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
int main(int argc, char **argv) {
    (void)argc; (void)argv;
    g_win = win_create("App Store", 60, 40, g_win_w, g_win_h);
    if (g_win < 0) return 1;

    setup_palette();
    load_policy();
    strcpy(g_status, "Loading catalog...");
    g_status_kind = 1;
    draw_all();

    load_manifest();
    build_list();
    draw_all();

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
                draw_all();
                break;
            case EVENT_MOUSE_DOWN:
                g_mx = ev.mouse_x; g_my = ev.mouse_y;
                handle_click(ev.mouse_x, ev.mouse_y);
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
