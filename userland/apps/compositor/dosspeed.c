// dosspeed.c - #778 per-window DOS guest speed control.
//
// THE PROBLEM (measured, see CHANGELOG/blame.md): #232 added a guest CPU cycle
// cap read once at launch from <program dir>/SPEED.CFG (or a DOSBox-style
// `cycles=` line in START.bat, or /CONFIG/DOSCYCLES.CFG), defaulting to NO CAP.
// Eight of the ten shipped DOS titles have no such file, so they run 4x-40x too
// fast. The owner asked for a per-window control next to the existing (global)
// Window Opacity slider so a running guest's speed can be tuned live, the same
// way opacity already is.
//
// WHY A NEW FILE AND NOT A WIDGET.C ADDITION: widgets.c's g_wsettings modal
// configures DESKTOP WIDGETS (weather/crypto/stocks/HA), a different concept
// from a per-WINDOW control. This owns exactly one thing: the Speed dialog for
// a DOS guest window, reachable from that window's taskbar/dock right-click
// menu (see taskbar.c's tbmenu and contextmenu.c's CTX_MODE_DOCK, both of
// which call dosspeed_window_is_dos() before offering the item).
//
// IDENTIFYING A DOS WINDOW: kernel/dos/dosexec.c's dos_guest_title() appends
// " (DOS)" to every DOS guest's window title (both the in-kernel path and the
// Ring-3 /APPS/DOSUSER host - same shared source, see dosring3/Makefile). That
// suffix is the ONLY identity a DOS window carries in wm_window_info_t: its
// app_id resolves to the shared kernel-owned host name, not a per-game name
// (see contextmenu.c's own comment on the "Win16/DOS shared host name" case),
// so app_id/exec_path cannot be used here. The suffix is reused rather than
// duplicated: strip it and what remains is the exact directory-under-/DOS name
// dos_guest_title() itself preferred, so "<title minus suffix>" round-trips
// straight back to /DOS/<name>/, which is where dos_speed_cycles_for() already
// looks for SPEED.CFG.
//
// LIMITATION, STATED RATHER THAN HIDDEN: a guest whose derived title collapsed
// to the literal fallback "DOS" (no directory segment to prefer - see
// dos_guest_title()'s own comment) is still recognised as a DOS window by this
// file's suffix-or-exact-match check, but its game directory cannot be
// reconstructed from a bare "DOS" title, so no shipped title needs it and the
// Speed item is not offered for it. Every one of the 10 shipped titles lives at
// /DOS/<GAME>/<PROG>.EXE (dos_guest_title()'s own documented shipped layout),
// so this covers the whole catalog.
//
// PERSISTENCE: Save writes <program dir>/SPEED.CFG directly - the SAME file
// dos_speed_cycles_for() already reads as its highest-priority candidate, and
// the SAME file the compositor also reads back to show the dialog's current
// value. One file, one format (a bare decimal, or "off"/"unlimited" for no
// cap - dos_cycles_parse() already accepts both), read by three things
// (in-kernel DOS, Ring-3 DOSUSER, this dialog) instead of inventing a fourth
// channel. This is deliberately the SAME mechanism as #745's dock-opacity live
// channel (a plain-text CFG file the running side polls on a throttle), not a
// new one.
//
// LIVE APPLY: the actual guest-side re-poll lives in kernel/dos/dosexec.c
// (#778, DOS_SPEED_LIVE_POLL_MS) - this file only ever writes the config file;
// it never reaches into a guest process's memory. That is what lets ONE write
// path serve both the in-kernel guest and the Ring-3 DOSUSER host: they are
// different processes (one is not even a process), but both re-read the same
// file on their own throttle, exactly as dock_opacity_poll() re-reads
// DOCKOPAC.CFG on the compositor's OWN throttle.
//
// TRUE MODAL, PER CLAUDE.md: closes only via Save/Cancel/Esc, never
// click-away. Follows the g_modal_grabs[] table in main.c (row "dos-speed"),
// the same mechanism widget-settings/icon-picker/force-quit-confirm use.
//
// STYLING: reuses the exact widgets.c g_wsettings visual grammar (CLR_MENU_BG
// panel, CLR_MENU_ITEM_HOVER header strip, 0x00005FB8 Save / 0x00444444 Cancel
// buttons) and the traymenu.c TM_SLIDER track/handle grammar (CLR_MENU_CAT_BG
// track, readable_accent() fill, draw_circle_filled() handle) rather than
// inventing a third look for the same kind of control.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/fcntl.h"
#include "../../libc/math.h"

// Mirrors kernel/dos/dosexec.c's DOS_CYCLES_MIN/MAX exactly (#232). The two
// trees cannot share a header (kernel vs Ring-3 userland ABI boundary), so
// this is a second literal by construction - if the kernel's sanity floor/
// ceiling ever changes, this comment is the pointer back to keep them in step
// (see blame.md).
#define DS_CYCLES_MIN   20u
#define DS_CYCLES_MAX   200000u
// The exact four era presets from dosexec.c's own classification comment
// (PC-XT 8088 / 286-class / 386-class / 486-class), so the dialog's buttons
// produce the SAME numbers the kernel's own [dos] #232 log line would call by
// those names - not a second, differently-tuned guess at "what a 286 feels
// like".
#define DS_PRESET_8088   315u
#define DS_PRESET_286    900u
#define DS_PRESET_386    6000u
#define DS_PRESET_486    20000u

#define DS_W        ui_px(380)
#define DS_ROW_H    ui_px(26)
#define DS_PRESET_H ui_px(28)
#define DS_PAD      ui_px(14)

static int  g_ds_open   = 0;      // 0 closed, 1 open
static int  g_ds_win_id = -1;
static char g_ds_game[40];        // display name, e.g. "ROGUE"
static char g_ds_path[80];        // "/DOS/ROGUE/SPEED.CFG"
static uint32_t g_ds_cycles = 0;  // pending value, 0 = uncapped
static int  g_ds_dragging = 0;    // slider drag in progress
// (#speedcap) A FAILED SAVE MUST BE VISIBLE. See ds_write_cycles().
static const char *g_ds_err = 0;

// ---------------------------------------------------------------------------
// Detection
// ---------------------------------------------------------------------------

// Returns 1 and fills game[] (the directory-under-/DOS name, NUL-terminated)
// if win_id is a DOS guest window; 0 otherwise. game may be NULL to just test.
int dosspeed_window_is_dos(int win_id, char *game, int cap) {
    wm_window_info_t wins[32];
    int n = wm_get_windows(wins, 32);
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        if (wins[i].id != win_id) continue;
        const char *t = wins[i].title;
        int tlen = 0;
        while (t[tlen] && tlen < 64) tlen++;
        static const char suf[] = " (DOS)";
        int slen = 6;
        if (tlen > slen &&
            t[tlen-6]==' ' && t[tlen-5]=='(' && t[tlen-4]=='D' &&
            t[tlen-3]=='O' && t[tlen-2]=='S' && t[tlen-1]==')') {
            int glen = tlen - slen;
            if (game && cap > 0) {
                int c = glen < cap - 1 ? glen : cap - 1;
                for (int k = 0; k < c; k++) game[k] = t[k];
                game[c] = '\0';
            }
            return 1;
        }
        (void)suf;
        return 0;   // includes the bare "DOS" fallback title - see file header
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Config file read/write - the SAME <program dir>/SPEED.CFG dos_speed_cycles_for()
// reads, per this file's header.
// ---------------------------------------------------------------------------

static uint32_t ds_parse_cycles(const char *b, int n, int *found) {
    *found = 0;
    int i = 0;
    while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;
    if (i + 1 < n && (b[i]=='o'||b[i]=='O') && (b[i+1]=='f'||b[i+1]=='F')) { *found = 1; return 0; }
    if (i + 1 < n && (b[i]=='u'||b[i]=='U') && (b[i+1]=='n'||b[i+1]=='N')) { *found = 1; return 0; }
    uint32_t v = 0; int any = 0;
    while (i < n && b[i] >= '0' && b[i] <= '9') { v = v * 10 + (uint32_t)(b[i]-'0'); i++; any = 1; }
    if (!any) return 0;
    *found = 1;
    if (v == 0) return 0;
    if (v < DS_CYCLES_MIN) v = DS_CYCLES_MIN;
    if (v > DS_CYCLES_MAX) v = DS_CYCLES_MAX;
    return v;
}

// Best-effort current value for the dialog to open showing something real:
// checks SPEED.CFG first (what Save writes), then a `cycles=` line in
// START.bat (the other shipped convention, e.g. /DOS/JOUST/START.bat).
// Deliberately does NOT also check /CONFIG/DOSCYCLES.CFG (the SYSTEM default):
// that is not this window's own setting, and showing it here would make a
// per-window dialog silently claim a value nobody set for this game.
static uint32_t ds_read_current(const char *dir) {
    char path[80];
    int i = 0; for (; dir[i] && i < (int)sizeof(path)-11; i++) path[i] = dir[i];
    const char *tail = "/SPEED.CFG"; for (int k = 0; tail[k]; k++) path[i++] = tail[k];
    path[i] = '\0';
    int fd = sys_open(path, O_RDONLY);
    if (fd >= 0) {
        char b[16]; long n = sys_read(fd, b, sizeof(b));
        sys_close(fd);
        if (n > 0) { int found = 0; uint32_t v = ds_parse_cycles(b, (int)n, &found); if (found) return v; }
    }
    i = 0; for (; dir[i] && i < (int)sizeof(path)-11; i++) path[i] = dir[i];
    tail = "/START.bat"; for (int k = 0; tail[k]; k++) path[i++] = tail[k];
    path[i] = '\0';
    fd = sys_open(path, O_RDONLY);
    if (fd >= 0) {
        char b[512]; long n = sys_read(fd, b, sizeof(b)-1);
        sys_close(fd);
        if (n > 0) {
            b[n] = '\0';
            for (long k = 0; k + 6 < n; k++) {
                if ((b[k]=='c'||b[k]=='C') && (b[k+1]=='y'||b[k+1]=='Y') &&
                    (b[k+2]=='c'||b[k+2]=='C') && (b[k+3]=='l'||b[k+3]=='L') &&
                    (b[k+4]=='e'||b[k+4]=='E') && (b[k+5]=='s'||b[k+5]=='S')) {
                    long q = k + 6;
                    while (q < n && b[q] != '=' && b[q] != '\n') q++;
                    if (q < n && b[q] == '=') {
                        q++;
                        int found = 0;
                        uint32_t v = ds_parse_cycles(b + q, (int)(n - q), &found);
                        if (found) return v;
                    }
                }
            }
        }
    }
    return 0;   // no override found: dialog opens showing Uncapped
}

// Returns 0 on success, -1 if the file could not be written.
//
// (#speedcap) THIS USED TO RETURN VOID AND SWALLOW THE FAILURE, and the failure
// was not hypothetical. MEASURED on golden 2300: /DOS/<GAME> is root-owned 0755
// and the compositor runs as the desktop user, so this open was REFUSED
// ([PERMS-DENY] proc=COMPOSIT uid=1000 gid=1000 want=-wx path=/DOS/KEEN5) and the
// dialog closed as though it had worked. The whole persistence half of #778 was
// inert on a real image and nothing anywhere said so.
//
// The kernel half of the fix (dos_speed_cfg_make_writable() in dos/dosexec.c)
// makes an existing SPEED.CFG mode 0666 at guest launch, so this now succeeds.
// This half exists so that if it ever fails again for some OTHER reason (a
// read-only mount, a title whose directory was never launched, a future perms
// change) the user is told rather than quietly given a control wired to nothing.
static int ds_write_cycles(uint32_t cycles) {
    int fd = sys_open(g_ds_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return -1;
    char b[8]; int n = 0;
    if (cycles == 0) {
        const char *off = "off";
        for (; off[n]; n++) b[n] = off[n];
    } else {
        char tmp[8]; int t = 0;
        uint32_t v = cycles;
        while (v > 0 && t < 8) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
        for (int k = 0; k < t; k++) b[n++] = tmp[t-1-k];
    }
    long wr = sys_write(fd, b, (unsigned long)n);
    sys_close(fd);
    return (wr == (long)n) ? 0 : -1;
}

// ---------------------------------------------------------------------------
// Open / state
// ---------------------------------------------------------------------------

void dosspeed_open(int win_id, const char *game) {
    g_ds_win_id = win_id;
    int i = 0; for (; game[i] && i < (int)sizeof(g_ds_game)-1; i++) g_ds_game[i] = game[i];
    g_ds_game[i] = '\0';
    int p = 0;
    const char *pre = "/DOS/"; for (int k = 0; pre[k]; k++) g_ds_path[p++] = pre[k];
    for (int k = 0; g_ds_game[k] && p < (int)sizeof(g_ds_path)-11; k++) g_ds_path[p++] = g_ds_game[k];
    const char *tail = "/SPEED.CFG"; for (int k = 0; tail[k] && p < (int)sizeof(g_ds_path)-1; k++) g_ds_path[p++] = tail[k];
    g_ds_path[p] = '\0';
    // ds_read_current() wants the bare directory (no /SPEED.CFG suffix).
    char dir[70]; int d = 0;
    const char *pre2 = "/DOS/"; for (int k = 0; pre2[k]; k++) dir[d++] = pre2[k];
    for (int k = 0; g_ds_game[k] && d < (int)sizeof(dir)-1; k++) dir[d++] = g_ds_game[k];
    dir[d] = '\0';
    g_ds_cycles = ds_read_current(dir);
    g_ds_dragging = 0;
    g_ds_err = 0;
    g_ds_open = 1;
}

int dosspeed_is_open(void) { return g_ds_open; }

static void ds_close(void) { g_ds_open = 0; g_ds_win_id = -1; g_ds_dragging = 0; g_ds_err = 0; }

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

static int ds_height(void) {
    // header + label + presets row + slider row + value/hint + buttons
    return ui_px(26) + ui_px(10) + DS_PRESET_H + ui_px(14) + ui_px(48) + ui_px(20) + ui_px(44);
}
static void ds_geom(int *x, int *y, int *w, int *h) {
    *w = DS_W; *h = ds_height();
    *x = (g_fb_width - *w) / 2;
    *y = (g_fb_height - *h) / 2;
}
static void ds_preset_box(int idx, int *bx, int *by, int *bw, int *bh) {
    int x, y, w, h; ds_geom(&x, &y, &w, &h);
    int gap = ui_px(6);
    *bw = (w - DS_PAD*2 - gap*4) / 5;
    *bh = DS_PRESET_H;
    *bx = x + DS_PAD + idx * (*bw + gap);
    *by = y + ui_px(26) + ui_px(10);
}
static void ds_slider_track(int *tx, int *ty, int *tw) {
    int x, y, w, h; ds_geom(&x, &y, &w, &h);
    *tx = x + DS_PAD;
    *tw = w - DS_PAD*2;
    *ty = y + ui_px(26) + ui_px(10) + DS_PRESET_H + ui_px(20);
}
static void ds_buttons(int *x, int *y, int *w, int *h, int *by) {
    ds_geom(x, y, w, h);
    *by = *y + *h - ui_px(36);
}

// cycles <-> slider fraction, log-mapped across [DS_CYCLES_MIN, DS_CYCLES_MAX].
static double ds_lmin(void) { return log((double)DS_CYCLES_MIN); }
static double ds_lmax(void) { return log((double)DS_CYCLES_MAX); }
static double ds_frac_of(uint32_t cycles) {
    if (cycles == 0) return 0.0;
    double c = (double)cycles;
    if (c < DS_CYCLES_MIN) c = DS_CYCLES_MIN;
    if (c > DS_CYCLES_MAX) c = DS_CYCLES_MAX;
    double f = (log(c) - ds_lmin()) / (ds_lmax() - ds_lmin());
    if (f < 0.0) f = 0.0; if (f > 1.0) f = 1.0;
    return f;
}
static uint32_t ds_cycles_of_frac(double f) {
    if (f < 0.0) f = 0.0; if (f > 1.0) f = 1.0;
    double c = exp(ds_lmin() + f * (ds_lmax() - ds_lmin()));
    uint32_t v = (uint32_t)(c + 0.5);
    if (v < DS_CYCLES_MIN) v = DS_CYCLES_MIN;
    if (v > DS_CYCLES_MAX) v = DS_CYCLES_MAX;
    return v;
}

static const char *ds_era_label(uint32_t cycles) {
    if (cycles == 0) return "uncapped (host speed)";
    if (cycles <= 400)  return "PC-XT 8088 4.77MHz";
    if (cycles <= 1500) return "286-class";
    if (cycles <= 8000) return "386-class";
    return "486-class";
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void dosspeed_render(void) {
    if (!g_ds_open) return;
    int x, y, w, h; ds_geom(&x, &y, &w, &h);
    draw_fill_rect(x, y, w, h, CLR_MENU_BG);
    draw_rect_outline(x, y, w, h, CLR_MENU_BORDER);
    draw_fill_rect(x, y, w, ui_px(26), CLR_MENU_ITEM_HOVER);
    char title[64]; int t = 0;
    const char *pre = "Speed - ";
    for (int k = 0; pre[k]; k++) title[t++] = pre[k];
    for (int k = 0; g_ds_game[k] && t < (int)sizeof(title)-1; k++) title[t++] = g_ds_game[k];
    title[t] = '\0';
    draw_text(x + DS_PAD - 2, y + ui_px(7), title, CLR_MENU_TEXT);

    static const char *plabel[5] = { "8088", "286", "386", "486", "Uncapped" };
    static const uint32_t pval[5] = { DS_PRESET_8088, DS_PRESET_286, DS_PRESET_386, DS_PRESET_486, 0 };
    for (int i = 0; i < 5; i++) {
        int bx, by, bw, bh; ds_preset_box(i, &bx, &by, &bw, &bh);
        int active = (g_ds_cycles == pval[i]);
        uint32_t bg = active ? readable_accent(0xFF5A78B0, CLR_MENU_BG) : CLR_MENU_CAT_BG;
        draw_fill_rect(bx, by, bw, bh, bg);
        draw_rect_outline(bx, by, bw, bh, CLR_MENU_BORDER);
        draw_text_centered(bx + bw/2, by + (bh - ui_px(12))/2, plabel[i], active ? readable_ink(bg) : readable_ink_dim(CLR_MENU_BG));
    }

    int tx, ty, tw; ds_slider_track(&tx, &ty, &tw);
    draw_text(tx, ty - ui_px(16), "Cycles (drag for a specific value):", readable_ink_dim(CLR_MENU_BG));
    draw_fill_rect(tx, ty, tw, ui_px(6), CLR_MENU_CAT_BG);
    double f = ds_frac_of(g_ds_cycles);
    int fillw = (int)(f * (double)tw);
    if (g_ds_cycles > 0) {
        draw_fill_rect(tx, ty, fillw, ui_px(6), readable_accent(0xFF5A78B0, CLR_MENU_BG));
        draw_circle_filled(tx + fillw, ty + ui_px(3), ui_px(6), readable_ink_dim(CLR_MENU_BG));
    }

    char val[64]; int vlen = 0;
    if (g_ds_cycles == 0) {
        const char *s = "Uncapped - host speed (no cap)";
        for (; s[vlen]; vlen++) val[vlen] = s[vlen];
    } else {
        char num[16]; int nlen = 0; uint32_t v = g_ds_cycles; char tmp[16]; int tt = 0;
        while (v > 0) { tmp[tt++] = (char)('0' + v % 10); v /= 10; }
        for (int k = 0; k < tt; k++) num[nlen++] = tmp[tt-1-k];
        num[nlen] = '\0';
        const char *s1 = num; for (; *s1; s1++) val[vlen++] = *s1;
        const char *s2 = " cycles (~"; for (; *s2; s2++) val[vlen++] = *s2;
        const char *era = ds_era_label(g_ds_cycles); for (; *era; era++) val[vlen++] = *era;
        val[vlen++] = ')';
    }
    val[vlen] = '\0';
    draw_text(tx, ty + ui_px(16), val, CLR_MENU_TEXT);
    // (#speedcap) The failure line, under the value it failed to store. Drawn in
    // the theme's own warning-ish ink rather than a bespoke red so it follows the
    // shared style engine like the rest of this dialog.
    if (g_ds_err) draw_text(tx, ty + ui_px(30), g_ds_err, 0x00D06060);

    int bx, by, bw, bh2, brow; ds_buttons(&bx, &by, &bw, &bh2, &brow);
    draw_fill_rect(bx + bw - ui_px(184), brow, ui_px(84), ui_px(28), 0x00005FB8);
    draw_text_centered(bx + bw - ui_px(184) + ui_px(42), brow + ui_px(8), "Save", 0x00FFFFFF);
    draw_fill_rect(bx + bw - ui_px(94), brow, ui_px(84), ui_px(28), 0x00444444);
    draw_text_centered(bx + bw - ui_px(94) + ui_px(42), brow + ui_px(8), "Cancel", 0x00FFFFFF);
}

// ---------------------------------------------------------------------------
// Input - MG_ALL true-modal row in main.c's g_modal_grabs[] table.
// ---------------------------------------------------------------------------

int dosspeed_handle_key(int key) {
    if (!g_ds_open) return 0;
    if (key == 27) { ds_close(); return 1; }   // Esc cancels, same as every other true modal here
    return 1;   // swallow everything else: this dialog takes no typed input
}

// Slider press/drag/end, mirroring widget_settings_press()/_drag_to()/_drag_end()'s
// shape so main.c's per-frame drag-continuation block can treat this the same way.
int dosspeed_press(int x, int y) {
    if (!g_ds_open) return 0;
    int tx, ty, tw; ds_slider_track(&tx, &ty, &tw);
    int tol = ui_px(8);
    if (x >= tx - tol && x < tx + tw + tol && y >= ty - tol && y < ty + ui_px(6) + tol) {
        g_ds_dragging = 1;
        double f = (double)(x - tx) / (double)(tw < 1 ? 1 : tw);
        g_ds_cycles = ds_cycles_of_frac(f);
        return 1;
    }
    return 0;
}
void dosspeed_drag_to(int x, int y) {
    if (!g_ds_open || !g_ds_dragging) return;
    int tx, ty, tw; ds_slider_track(&tx, &ty, &tw);
    (void)y;
    double f = (double)(x - tx) / (double)(tw < 1 ? 1 : tw);
    g_ds_cycles = ds_cycles_of_frac(f);
}
void dosspeed_drag_end(void) { g_ds_dragging = 0; }

int dosspeed_handle_mouse(int x, int y, int click) {
    if (!g_ds_open) return 0;
    if (!click) return 1;   // swallow hover: nothing beneath should see it

    for (int i = 0; i < 5; i++) {
        int bx, by, bw, bh; ds_preset_box(i, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh) {
            static const uint32_t pval[5] = { DS_PRESET_8088, DS_PRESET_286, DS_PRESET_386, DS_PRESET_486, 0 };
            g_ds_cycles = pval[i];
            return 1;
        }
    }
    if (dosspeed_press(x, y)) return 1;   // slider click also sets an initial value

    int bx, by, bw, bh2, brow; ds_buttons(&bx, &by, &bw, &bh2, &brow);
    if (y >= brow && y < brow + ui_px(28)) {
        if (x >= bx + bw - ui_px(184) && x < bx + bw - ui_px(100)) {
            // (#speedcap) Close ONLY on a write that actually happened. A dialog
            // that dismisses itself on failure is indistinguishable from one that
            // worked, which is exactly how this shipped inert.
            if (ds_write_cycles(g_ds_cycles) == 0) {
                ds_close();
            } else {
                g_ds_err = "Could not write SPEED.CFG (permission denied)";
            }
            return 1;
        }
        if (x >= bx + bw - ui_px(94) && x < bx + bw - ui_px(10)) {
            ds_close();
            return 1;
        }
    }
    // No click-away dismiss (CLAUDE.md): a click elsewhere inside the modal's
    // bounds that hits nothing is simply swallowed, and the modal stays open.
    return 1;
}
