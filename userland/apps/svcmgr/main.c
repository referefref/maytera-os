// svcmgr - MayteraOS Services & Startup Manager (UI for the #95 kernel
// services subsystem and the #317 AUTORUN.CFG boot auto-launch).
//
// The kernel builds its service registry at boot from a built-in default
// (heartbeat -> /APPS/SVCHB) merged with /CONFIG/SERVICES.CFG, one service per
// whitespace-separated line:
//     name  exec-path  account  uid  perms  autostart  enabled
// (see kernel/proc/services.c). Until now that file could only be edited by
// hand and there was no UI at all for services or the startup app. This app:
//   - parses /CONFIG/SERVICES.CFG with the same grammar the kernel uses and
//     mirrors the kernel's built-in heartbeat default,
//   - shows the LIVE run state of each service by matching the service name /
//     exec basename against the real process table (SYS_PROC_LIST),
//   - toggles Enabled / Autostart and removes entries by rewriting the config
//     (kernel re-reads it at boot, so registry changes apply on next boot),
//   - Run/Stop act immediately: SYS_SPAWN launches the service ELF now (note:
//     as a regular process; the kernel-side sandbox tag is only applied when
//     the kernel itself starts a service) and SYS_KILL (SIGTERM) stops it,
//   - manages the startup application: /CONFIG/AUTORUN.CFG holds one app path
//     the desktop launches after boot; pick any entry enumerated from /APPS
//     (SYS_OPEN/SYS_READDIR), or clear it (SYS_UNLINK).
//
// Note: on an ext2-root install (/ROOTEXT2 marker) userland /CONFIG paths
// resolve to ext2 while the kernel loads SERVICES.CFG/AUTORUN.CFG from the FAT
// volume; the default (FAT-root) images edit exactly the file the kernel reads.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_style.h"
#include "../../libc/theme.h"
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"
#include "../../libc/fcntl.h"
#include "../../libc/signal.h"
#include "../../libc/dirent.h"

#define WIN_W 800
#define WIN_H 560
#define PAD   12
#define HDR_H 40
#define SROW_H 36
#define AROW_H 20
#define DETAIL_H 150

#define MAXSVC  16
#define MAXAPPS 96
#define CFG_MAX 8192

#define SVC_CFG_PATH   "/CONFIG/SERVICES.CFG"
#define AUTORUN_PATH   "/CONFIG/AUTORUN.CFG"

static int win = -1, DW = WIN_W, DH = WIN_H;

// ---- model -------------------------------------------------------------------
typedef struct {
    char name[24];
    char exec[64];
    char account[24];
    char perms[32];
    unsigned int uid;
    int autostart, enabled;
    int builtin;            // mirrored kernel built-in, not (yet) in the cfg file
    int pid;                // live pid from SYS_PROC_LIST, 0 = stopped
} svc_t;

static svc_t g_svc[MAXSVC];
static int g_nsvc = 0;
static int g_cfg_present = 0;     // SERVICES.CFG existed on disk
static int sel_svc = 0;

static char g_apps[MAXAPPS][32];
static int g_napps = 0;
static int sel_app = -1, app_scroll = 0, svc_scroll = 0;

static char g_autorun[128];       // current AUTORUN.CFG content ("" = none)

static char status_msg[112];
static unsigned long status_ms = 0;

// ---- palette (Settings/Files design language) ---------------------------------
static unsigned int C_BG, C_CARD, C_FIELD, C_BORDER, C_INK, C_DIM, C_ACC, C_SEL, C_SELTX;
static unsigned int C_OK = 0x003C9A5F, C_BAD = 0x00B0503C;

static unsigned int lum_ink(unsigned int bg) {
    int r = (bg >> 16) & 255, g = (bg >> 8) & 255, b = bg & 255;
    return ((r * 30 + g * 59 + b * 11) / 100) > 140 ? 0x00181818u : 0x00F0F0F0u;
}
static unsigned int dim_ink(unsigned int bg) {
    unsigned int k = lum_ink(bg);
    int ir = (k >> 16) & 255, ig = (k >> 8) & 255, ib = k & 255;
    int br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    return (((ir + br) / 2) << 16) | (((ig + bgc) / 2) << 8) | ((ib + bb) / 2);
}
static unsigned int tint(unsigned int base, unsigned int acc, int pct) {
    int br = (base >> 16) & 255, bg = (base >> 8) & 255, bb = base & 255;
    int ar = (acc >> 16) & 255, ag = (acc >> 8) & 255, ab = acc & 255;
    return ((((br * (100 - pct) + ar * pct) / 100) & 255) << 16) |
           ((((bg * (100 - pct) + ag * pct) / 100) & 255) << 8) |
           (((bb * (100 - pct) + ab * pct) / 100) & 255);
}
static void apply_style(void) {
    int tid = theme_get_active();
    gui_set_style(tid == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    unsigned int wb = theme_color(THEME_COLOR_WINDOW_BG);
    int r = (wb >> 16) & 255, g = (wb >> 8) & 255, b = wb & 255;
    int dark = ((r * 30 + g * 59 + b * 11) / 100) < 128;
    C_ACC = theme_color(THEME_COLOR_ACCENT);
    C_BG    = tint(dark ? 0x00262A30 : 0x00F5F6F8, C_ACC, 5);
    C_CARD  = tint(dark ? 0x002C313B : 0x00EDEFF3, C_ACC, 6);
    C_FIELD = dark ? 0x00333A45 : 0x00FFFFFF;
    C_BORDER= dark ? 0x003A424F : 0x00CDD3DB;
    C_INK = lum_ink(C_BG); C_DIM = dim_ink(C_BG); C_SEL = C_ACC; C_SELTX = lum_ink(C_ACC);
    gui_palette_t p;
    p.surface = C_BG; p.surface_raised = C_CARD; p.ink = C_INK; p.ink_dim = C_DIM;
    p.accent = C_ACC; p.accent_hover = gui_lighten(C_ACC, 24); p.border = C_BORDER;
    p.field_bg = C_FIELD; p.field_border = C_BORDER; p.track = tint(C_BG, C_ACC, 20);
    gui_set_palette(&p);
}

// ---- small helpers -------------------------------------------------------------
static void set_status(const char *msg) {
    snprintf(status_msg, sizeof(status_msg), "%s", msg);
    status_ms = uptime_ms();
}
static unsigned int s_atou(const char *s) {
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned int)(*s++ - '0');
    return v;
}
static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int ci_eq(const char *a, const char *b) {
    while (*a && *b) { if (lc(*a) != lc(*b)) return 0; a++; b++; }
    return *a == 0 && *b == 0;
}
static const char *basename_of(const char *p) {
    const char *r = p;
    for (const char *q = p; *q; q++) if (*q == '/') r = q + 1;
    return r;
}
// One whitespace-separated field; same tokenizer shape as the kernel parser.
static int next_field(const char **pp, char *out, int outlen) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0' || *p == '\n' || *p == '\r') { *pp = p; return 0; }
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        if (n < outlen - 1) out[n++] = *p;
        p++;
    }
    out[n] = '\0';
    *pp = p;
    return 1;
}

// ---- config load/save -----------------------------------------------------------
static int read_whole(const char *path, char *buf, int cap) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n = sys_read(fd, buf, (unsigned long)(cap - 1));
    sys_close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return (int)n;
}

static svc_t *svc_find(const char *name) {
    for (int i = 0; i < g_nsvc; i++)
        if (strcmp(g_svc[i].name, name) == 0) return &g_svc[i];
    return 0;
}

static void load_services(void) {
    static char data[CFG_MAX];
    g_nsvc = 0;
    g_cfg_present = 0;

    // Mirror the kernel's always-present built-in default first (svc_init()):
    // it is registered before the config file is merged, so the registry always
    // contains it even when SERVICES.CFG is absent.
    svc_t *hb = &g_svc[g_nsvc++];
    memset(hb, 0, sizeof(*hb));
    strncpy(hb->name, "heartbeat", sizeof(hb->name) - 1);
    strncpy(hb->exec, "/APPS/SVCHB", sizeof(hb->exec) - 1);
    strncpy(hb->account, "svc_hb", sizeof(hb->account) - 1);
    strncpy(hb->perms, "fs", sizeof(hb->perms) - 1);
    hb->uid = 0; hb->autostart = 1; hb->enabled = 1; hb->builtin = 1;

    int n = read_whole(SVC_CFG_PATH, data, sizeof(data));
    if (n < 0) return;
    g_cfg_present = 1;

    const char *p = data;
    while (*p) {
        // isolate one line
        const char *ls = p;
        while (*p && *p != '\n') p++;
        const char *le = p;
        if (*p) p++;
        while (ls < le && (*ls == ' ' || *ls == '\t' || *ls == '\r')) ls++;
        if (ls >= le || *ls == '#') continue;

        char line[256];
        int ll = 0;
        for (const char *q = ls; q < le && ll < (int)sizeof(line) - 1; q++) line[ll++] = *q;
        line[ll] = '\0';

        const char *cur = line;
        char name[24], exec[64], account[24], uidf[16], permsf[32], autof[8], enf[8];
        if (!next_field(&cur, name, sizeof(name))) continue;
        if (!next_field(&cur, exec, sizeof(exec))) continue;
        if (!next_field(&cur, account, sizeof(account))) strncpy(account, "service", sizeof(account));
        if (!next_field(&cur, uidf, sizeof(uidf)))       strncpy(uidf, "0", sizeof(uidf));
        if (!next_field(&cur, permsf, sizeof(permsf)))   permsf[0] = '\0';
        if (!next_field(&cur, autof, sizeof(autof)))     strncpy(autof, "0", sizeof(autof));
        if (!next_field(&cur, enf, sizeof(enf)))         strncpy(enf, "1", sizeof(enf));

        svc_t *s = svc_find(name);           // merge-by-name, like the kernel
        if (!s) {
            if (g_nsvc >= MAXSVC) continue;
            s = &g_svc[g_nsvc++];
            memset(s, 0, sizeof(*s));
        }
        strncpy(s->name, name, sizeof(s->name) - 1);       s->name[sizeof(s->name) - 1] = 0;
        strncpy(s->exec, exec, sizeof(s->exec) - 1);       s->exec[sizeof(s->exec) - 1] = 0;
        strncpy(s->account, account, sizeof(s->account) - 1); s->account[sizeof(s->account) - 1] = 0;
        strncpy(s->perms, permsf, sizeof(s->perms) - 1);   s->perms[sizeof(s->perms) - 1] = 0;
        s->uid = s_atou(uidf);
        s->autostart = s_atou(autof) != 0;
        s->enabled   = s_atou(enf) != 0;
        s->builtin = 0;
    }
    if (sel_svc >= g_nsvc) sel_svc = g_nsvc ? g_nsvc - 1 : 0;
}

static int save_services(void) {
    static char out[CFG_MAX];
    int pos = 0;
    pos += snprintf(out + pos, sizeof(out) - pos,
                    "# MayteraOS services (#95), written by svcmgr\n"
                    "# name exec account uid perms autostart enabled\n");
    for (int i = 0; i < g_nsvc; i++) {
        svc_t *s = &g_svc[i];
        pos += snprintf(out + pos, sizeof(out) - pos, "%s %s %s %u %s %u %u\n",
                        s->name, s->exec,
                        s->account[0] ? s->account : "service",
                        s->uid,
                        s->perms[0] ? s->perms : "none",
                        (unsigned)s->autostart, (unsigned)s->enabled);
        if (pos >= (int)sizeof(out) - 128) break;
    }
    int fd = sys_open(SVC_CFG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { set_status("Error: cannot open /CONFIG/SERVICES.CFG for writing"); return -1; }
    long w = sys_write(fd, out, (unsigned long)pos);
    sys_close(fd);
    if (w != pos) { set_status("Error: short write to SERVICES.CFG"); return -1; }
    for (int i = 0; i < g_nsvc; i++) g_svc[i].builtin = 0;   // now all in the file
    g_cfg_present = 1;
    return 0;
}

static void load_autorun(void) {
    static char data[256];
    g_autorun[0] = '\0';
    if (read_whole(AUTORUN_PATH, data, sizeof(data)) <= 0) return;
    int i = 0;
    while (data[i] && data[i] != '\n' && data[i] != '\r' && i < (int)sizeof(g_autorun) - 1) {
        g_autorun[i] = data[i]; i++;
    }
    g_autorun[i] = '\0';
}

static void load_apps(void) {
    g_napps = 0;
    DIR *d = opendir("/APPS");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != 0 && g_napps < MAXAPPS) {
        if (e->d_name[0] == '.') continue;
        if (e->d_type == DT_DIR) continue;
        strncpy(g_apps[g_napps], e->d_name, sizeof(g_apps[0]) - 1);
        g_apps[g_napps][sizeof(g_apps[0]) - 1] = 0;
        g_napps++;
    }
    closedir(d);
}

// ---- live run state --------------------------------------------------------------
static void refresh_status(void) {
    static proc_info_t procs[64];
    int np = sys_proc_list(procs, 64);
    if (np < 0) np = 0;
    for (int i = 0; i < g_nsvc; i++) {
        g_svc[i].pid = 0;
        const char *bn = basename_of(g_svc[i].exec);
        for (int j = 0; j < np; j++) {
            if (procs[j].state == 5) continue;               // zombie
            if (ci_eq(procs[j].name, g_svc[i].name) || ci_eq(procs[j].name, bn)) {
                g_svc[i].pid = (int)procs[j].pid;
                break;
            }
        }
    }
}

static void refresh_all(void) {
    load_services();
    load_autorun();
    load_apps();
    refresh_status();
}

// ---- layout -----------------------------------------------------------------------
static int left_w(void)   { int w = (DW - 2 * PAD - 10) * 58 / 100; return w < 300 ? 300 : w; }
static int card_y(void)   { return HDR_H + 8; }
static int card_h(void)   { int h = DH - card_y() - PAD; return h < 200 ? 200 : h; }
static int svclist_h(void){ return card_h() - DETAIL_H; }
static int right_x(void)  { return PAD + left_w() + 10; }
static int right_w(void)  { int w = DW - right_x() - PAD; return w < 180 ? 180 : w; }
static int svc_rows_vis(void) { return (svclist_h() - 34) / SROW_H; }
static int app_list_y(void)   { return card_y() + 78; }
static int app_list_h(void)   { return card_h() - 78 - 44; }
static int app_rows_vis(void) { return app_list_h() / AROW_H; }

// ---- drawing ------------------------------------------------------------------------
static void draw(void) {
    apply_style();
    win_get_size(win, &DW, &DH);
    if (DW < 520) DW = WIN_W;
    if (DH < 360) DH = WIN_H;
    win_draw_rect(win, 0, 0, DW, DH, C_BG);
    char buf[144];

    // header
    win_draw_text_ttf(win, PAD + 2, 10, "Services & Startup", 16, C_INK);
    gui_button(win, DW - 104, 8, 92, 26, "Refresh", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    if (status_msg[0] && (uptime_ms() - status_ms) < 5000) {
        int sw = gui_ttf_width(status_msg, 11);
        win_draw_text_ttf(win, DW - 116 - sw, 14, status_msg, 11, C_ACC);
    }
    win_draw_rect(win, 0, HDR_H, DW, 1, C_BORDER);

    unsigned int cink = lum_ink(C_CARD), cdim = dim_ink(C_CARD);

    // ---- left: service list card ----
    int lx = PAD, ly = card_y(), lw = left_w();
    gui_card(win, lx, ly, lw, svclist_h());
    snprintf(buf, sizeof(buf), "Services (%d)%s", g_nsvc,
             g_cfg_present ? "" : "   [SERVICES.CFG not found: kernel defaults]");
    win_draw_text_ttf(win, lx + 12, ly + 8, buf, 13, cink);
    int vis = svc_rows_vis();
    if (svc_scroll > g_nsvc - vis) svc_scroll = g_nsvc - vis;
    if (svc_scroll < 0) svc_scroll = 0;
    for (int rr = 0; rr < vis && (rr + svc_scroll) < g_nsvc; rr++) {
        int i = rr + svc_scroll;
        svc_t *s = &g_svc[i];
        int ry = ly + 30 + rr * SROW_H;
        int selrow = (i == sel_svc);
        if (selrow) gui_fill_rounded_aa(win, lx + 4, ry, lw - 8, SROW_H - 3, 4, C_SEL, C_CARD);
        unsigned int tx = selrow ? C_SELTX : cink, td = selrow ? C_SELTX : cdim;
        win_draw_text_ttf(win, lx + 12, ry + 2, s->name, 13, tx);
        snprintf(buf, sizeof(buf), "%s%s", s->exec, s->builtin ? "   (built-in)" : "");
        win_draw_text_ttf(win, lx + 12, ry + 18, buf, 10, td);
        // right-aligned status badge
        const char *st = s->pid ? "Running" : (s->enabled ? "Stopped" : "Disabled");
        unsigned int sc = s->pid ? C_OK : (s->enabled ? td : C_BAD);
        if (s->pid) { snprintf(buf, sizeof(buf), "Running (pid %d)", s->pid); st = buf; }
        int sw = gui_ttf_width(st, 11);
        win_draw_text_ttf(win, lx + lw - 14 - sw, ry + 8, st, 11, selrow ? C_SELTX : sc);
    }

    // ---- left: detail / control strip ----
    int dy = ly + svclist_h() + 8, dh = DETAIL_H - 8;
    gui_card(win, lx, dy, lw, dh);
    if (g_nsvc > 0 && sel_svc < g_nsvc) {
        svc_t *s = &g_svc[sel_svc];
        win_draw_text_ttf(win, lx + 12, dy + 8, s->name, 14, cink);
        snprintf(buf, sizeof(buf), "exec %s   account %s (uid %u)   perms %s",
                 s->exec, s->account[0] ? s->account : "service", s->uid,
                 s->perms[0] ? s->perms : "none");
        win_draw_text_ttf(win, lx + 12, dy + 28, buf, 11, cdim);
        if (s->pid) snprintf(buf, sizeof(buf), "Status: running as pid %d", s->pid);
        else        snprintf(buf, sizeof(buf), "Status: not running");
        win_draw_text_ttf(win, lx + 12, dy + 46, buf, 11, s->pid ? C_OK : cdim);

        gui_checkbox(win, lx + 12, dy + 70, 16, s->enabled ? true : false,
                     "Enabled", GUI_ST_NORMAL);
        gui_checkbox(win, lx + 132, dy + 70, 16, s->autostart ? true : false,
                     "Autostart at boot", GUI_ST_NORMAL);
        win_draw_text_ttf(win, lx + 12, dy + 92, "Registry changes are saved to SERVICES.CFG and apply at next boot.",
                          9, cdim);

        gui_button(win, lx + 12, dy + dh - 40, 88, 28, "Run now",
                   GUI_BTN_PRIMARY, s->pid ? GUI_ST_DISABLED : GUI_ST_NORMAL);
        gui_button(win, lx + 108, dy + dh - 40, 88, 28, "Stop",
                   GUI_BTN_SECONDARY, s->pid ? GUI_ST_NORMAL : GUI_ST_DISABLED);
        gui_button(win, lx + lw - 100, dy + dh - 40, 88, 28, "Remove",
                   GUI_BTN_SECONDARY, s->builtin ? GUI_ST_DISABLED : GUI_ST_NORMAL);
    } else {
        win_draw_text_ttf(win, lx + 12, dy + 10, "No services.", 12, cdim);
    }

    // ---- right: startup app (AUTORUN.CFG) ----
    int rx = right_x(), rw = right_w();
    gui_card(win, rx, ly, rw, card_h());
    win_draw_text_ttf(win, rx + 12, ly + 8, "Startup application", 13, cink);
    win_draw_text_ttf(win, rx + 12, ly + 28, "Launched by the desktop ~14 s after boot (AUTORUN.CFG).",
                      9, cdim);
    if (g_autorun[0]) {
        snprintf(buf, sizeof(buf), "Current: %s", g_autorun);
        win_draw_text_ttf(win, rx + 12, ly + 46, buf, 11, C_ACC);
    } else {
        win_draw_text_ttf(win, rx + 12, ly + 46, "Current: (none)", 11, cdim);
    }
    win_draw_rect(win, rx + 10, app_list_y() - 6, rw - 20, 1, C_BORDER);
    int avis = app_rows_vis();
    if (app_scroll > g_napps - avis) app_scroll = g_napps - avis;
    if (app_scroll < 0) app_scroll = 0;
    for (int rr = 0; rr < avis && (rr + app_scroll) < g_napps; rr++) {
        int i = rr + app_scroll;
        int ry = app_list_y() + rr * AROW_H;
        int selrow = (i == sel_app);
        if (selrow) gui_fill_rounded_aa(win, rx + 6, ry, rw - 12, AROW_H - 1, 3, C_SEL, C_CARD);
        // mark the app that is currently the autorun target
        char full[160];
        snprintf(full, sizeof(full), "/APPS/%s", g_apps[i]);
        int is_cur = g_autorun[0] && ci_eq(full, g_autorun);
        win_draw_text_ttf(win, rx + 14, ry + 2, g_apps[i], 11,
                          selrow ? C_SELTX : (is_cur ? C_ACC : cink));
        if (is_cur && !selrow)
            win_draw_text_ttf(win, rx + rw - 26, ry + 2, "*", 12, C_ACC);
    }
    int by = ly + card_h() - 38;
    gui_button(win, rx + 12, by, (rw - 34) / 2, 28, "Set startup",
               GUI_BTN_PRIMARY, sel_app >= 0 ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(win, rx + 22 + (rw - 34) / 2, by, (rw - 34) / 2, 28, "Clear",
               GUI_BTN_SECONDARY, g_autorun[0] ? GUI_ST_NORMAL : GUI_ST_DISABLED);

    win_invalidate(win);
}

// ---- actions ------------------------------------------------------------------------
static void act_run(void) {
    if (sel_svc >= g_nsvc) return;
    svc_t *s = &g_svc[sel_svc];
    if (!s->enabled) { set_status("Service is disabled; enable it first"); return; }
    if (s->pid) { set_status("Already running"); return; }
    int pid = sys_spawn(s->exec);
    char m[96];
    if (pid > 0) snprintf(m, sizeof(m), "Started %s (pid %d, unsandboxed)", s->name, pid);
    else         snprintf(m, sizeof(m), "Failed to start %s (%d)", s->name, pid);
    set_status(m);
    refresh_status();
}
static void act_stop(void) {
    if (sel_svc >= g_nsvc) return;
    svc_t *s = &g_svc[sel_svc];
    if (!s->pid) { set_status("Not running"); return; }
    int r = kill(s->pid, SIGTERM);
    char m[96];
    if (r == 0) snprintf(m, sizeof(m), "SIGTERM sent to %s (pid %d)", s->name, s->pid);
    else        snprintf(m, sizeof(m), "kill failed for pid %d", s->pid);
    set_status(m);
    refresh_status();
}
static void act_toggle(int what) {          // 0=enabled 1=autostart
    if (sel_svc >= g_nsvc) return;
    svc_t *s = &g_svc[sel_svc];
    if (what == 0) s->enabled = !s->enabled;
    else           s->autostart = !s->autostart;
    if (save_services() == 0) set_status("Saved SERVICES.CFG (applies at next boot)");
}
static void act_remove(void) {
    if (sel_svc >= g_nsvc) return;
    if (g_svc[sel_svc].builtin) { set_status("Built-in service cannot be removed"); return; }
    for (int i = sel_svc; i < g_nsvc - 1; i++) g_svc[i] = g_svc[i + 1];
    g_nsvc--;
    if (sel_svc >= g_nsvc && sel_svc > 0) sel_svc--;
    if (save_services() == 0) set_status("Removed; SERVICES.CFG saved");
}
static void act_set_autorun(void) {
    if (sel_app < 0 || sel_app >= g_napps) return;
    char full[160];
    snprintf(full, sizeof(full), "/APPS/%s\n", g_apps[sel_app]);
    int fd = sys_open(AUTORUN_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { set_status("Error: cannot write AUTORUN.CFG"); return; }
    long w = sys_write(fd, full, (unsigned long)strlen(full));
    sys_close(fd);
    if (w <= 0) { set_status("Error: short write to AUTORUN.CFG"); return; }
    load_autorun();
    set_status("Startup app saved (launches after next boot)");
}
static void act_clear_autorun(void) {
    if (!g_autorun[0]) return;
    if (sys_unlink(AUTORUN_PATH) == 0) {
        g_autorun[0] = '\0';
        set_status("Startup app cleared");
    } else {
        set_status("Error: could not remove AUTORUN.CFG");
    }
}

// ---- hit testing ----------------------------------------------------------------------
static void on_click(int lx0, int ly0) {
    int lx = PAD, ly = card_y(), lw = left_w();
    // header refresh
    if (ly0 >= 8 && ly0 < 34 && lx0 >= DW - 104 && lx0 < DW - 12) {
        refresh_all(); set_status("Refreshed"); return;
    }
    // service list rows
    if (lx0 >= lx && lx0 < lx + lw && ly0 >= ly + 30 && ly0 < ly + svclist_h()) {
        int rr = (ly0 - (ly + 30)) / SROW_H, idx = rr + svc_scroll;
        if (idx >= 0 && idx < g_nsvc) sel_svc = idx;
        return;
    }
    // detail strip controls
    int dy = ly + svclist_h() + 8, dh = DETAIL_H - 8;
    if (lx0 >= lx && lx0 < lx + lw && ly0 >= dy && ly0 < dy + dh && g_nsvc > 0) {
        if (ly0 >= dy + 66 && ly0 < dy + 90) {
            if (lx0 >= lx + 12 && lx0 < lx + 120)  { act_toggle(0); return; }
            if (lx0 >= lx + 132 && lx0 < lx + 300) { act_toggle(1); return; }
        }
        if (ly0 >= dy + dh - 40 && ly0 < dy + dh - 12) {
            if (lx0 >= lx + 12 && lx0 < lx + 100)   { act_run(); return; }
            if (lx0 >= lx + 108 && lx0 < lx + 196)  { act_stop(); return; }
            if (lx0 >= lx + lw - 100 && lx0 < lx + lw - 12) { act_remove(); return; }
        }
        return;
    }
    // right pane
    int rx = right_x(), rw = right_w();
    if (lx0 >= rx && lx0 < rx + rw) {
        if (ly0 >= app_list_y() && ly0 < app_list_y() + app_list_h()) {
            int rr = (ly0 - app_list_y()) / AROW_H, idx = rr + app_scroll;
            if (idx >= 0 && idx < g_napps) sel_app = idx;
            return;
        }
        int by = ly + card_h() - 38;
        if (ly0 >= by && ly0 < by + 28) {
            int half = (rw - 34) / 2;
            if (lx0 >= rx + 12 && lx0 < rx + 12 + half)              act_set_autorun();
            else if (lx0 >= rx + 22 + half && lx0 < rx + 22 + 2*half) act_clear_autorun();
            return;
        }
    }
}

// ---- main ------------------------------------------------------------------------------
int main(void) {
    win = win_create("Services", 130, 80, WIN_W, WIN_H);
    if (win < 0) return 1;
    refresh_all();
    draw();
    int running = 1;
    unsigned long last = uptime_ms();
    while (running) {
        gui_event_t ev;
        int et = win_get_event(win, &ev, 2000);
        unsigned long now = uptime_ms();
        if (now - last >= 1900) { refresh_status(); last = now; draw(); }
        if (et == 0) continue;
        switch (ev.type) {
            case EVENT_REDRAW:
            case EVENT_RESIZE:
                draw(); break;
            case EVENT_WINDOW_CLOSE:
                running = 0; break;
            case EVENT_KEY_DOWN:
                if (ev.key_char == 27) running = 0;
                else if (ev.key_char == 'r' || ev.key_char == 'R') { refresh_all(); draw(); }
                else if (ev.keycode == 0x80 && sel_svc > 0)         { sel_svc--; draw(); }
                else if (ev.keycode == 0x81 && sel_svc < g_nsvc - 1){ sel_svc++; draw(); }
                break;
            case EVENT_MOUSE_DOWN:
                on_click(ev.mouse_x, ev.mouse_y);
                draw();
                break;
            case EVENT_MOUSE_SCROLL: {
                int d = (ev.scroll_delta > 0) ? 2 : -2;
                if (ev.mouse_x >= right_x()) {
                    app_scroll += d;
                    if (app_scroll < 0) app_scroll = 0;
                } else {
                    svc_scroll += (d > 0) ? 1 : -1;
                    if (svc_scroll < 0) svc_scroll = 0;
                }
                draw();
                break;
            }
            default: break;
        }
    }
    win_destroy(win);
    return 0;
}
