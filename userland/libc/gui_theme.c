// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// gui_theme.c - file-based theme loader (#565). See gui_theme.h.
#include "gui_theme.h"
#include "syscall.h"
#include "string.h"
#include "stdio.h"
#include "theme.h"
#include "notify.h"

#define THEME_INDEX_PATH  "/THEMES/INDEX.TXT"
#define THEME_DIR_PREFIX  "/THEMES/"
// #683: per-user preference. Reads fall back to the legacy system copy so an
// upgrade (and an administrator-set system default) still applies; writes go to
// the user's home. See libc/userconf.c for the rule.
// KNOWN, DEFERRED to #683b: kernel/gui/theme.c writes "theme=/index=" to this
// same NAME while this file writes "active="; two writers, two schemas. The
// schema must be resolved WITH the kernel-side relocation, never after, or
// there would be two formats in two locations.
#include "userconf.h"
#define THEME_CFG_NAME    "THEME.CFG"
#define THEME_CFG_LEGACY  "/CONFIG/THEME.CFG"

static int gt_read_whole(const char *path, char *buf, int cap) {
    int fd = sys_open(path, 0);
    if (fd < 0) return -1;
    long n = sys_read(fd, buf, (unsigned long)(cap - 1));
    sys_close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

// strip a trailing ".mtheme" (if present) to get the slug.
static void gt_slug_from_filename(const char *fname, char *slug, int cap) {
    int len = 0;
    while (fname[len]) len++;
    int slen = len;
    if (len > 7 && strcmp(fname + len - 7, ".mtheme") == 0) slen = len - 7;
    if (slen > cap - 1) slen = cap - 1;
    int i = 0;
    for (; i < slen; i++) slug[i] = fname[i];
    slug[i] = 0;
}

// Peek a theme file's "name=" / "dark=" lines only (the full 51-color parse
// happens kernel-side on activation; the picker just needs display metadata).
static void gt_peek_metadata(const char *path, gui_theme_entry_t *e) {
    static char fb[3072];
    int fn = gt_read_whole(path, fb, sizeof(fb));
    if (fn <= 0) return;
    int q = 0;
    while (q < fn) {
        char line[96];
        int ll = 0;
        while (q < fn && fb[q] != '\n' && ll < (int)sizeof(line) - 1) { line[ll++] = fb[q]; q++; }
        if (q < fn && fb[q] == '\n') q++;
        if (ll > 0 && line[ll - 1] == '\r') ll--;
        line[ll] = 0;
        if (strncmp(line, "name=", 5) == 0) {
            int k = 0;
            const char *v = line + 5;
            while (v[k] && k < GUI_THEME_NAME_MAX - 1) { e->name[k] = v[k]; k++; }
            e->name[k] = 0;
        } else if (strncmp(line, "dark=", 5) == 0) {
            e->is_dark = (line[5] == '1');
        } else if (strncmp(line, "style=", 6) == 0) {
            e->is_classic = (strcmp(line + 6, "retro") == 0);
        }
    }
}

int gui_theme_list(gui_theme_entry_t *out, int max) {
    static char idx[4096];
    int n = gt_read_whole(THEME_INDEX_PATH, idx, sizeof(idx));
    if (n <= 0 || max <= 0) return 0;

    int count = 0;
    int p = 0;
    while (p < n && count < max) {
        char fname[64];
        int fl = 0;
        while (p < n && idx[p] != '\n' && fl < (int)sizeof(fname) - 1) { fname[fl++] = idx[p]; p++; }
        if (p < n && idx[p] == '\n') p++;
        if (fl > 0 && fname[fl - 1] == '\r') fl--;
        fname[fl] = 0;
        if (fl == 0) continue;

        gui_theme_entry_t *e = &out[count];
        gt_slug_from_filename(fname, e->slug, GUI_THEME_SLUG_MAX);
        e->name[0] = 0;
        e->is_dark = 0;
        e->is_classic = 0;
        e->index = count;

        char path[96];
        snprintf(path, sizeof(path), "%s%s", THEME_DIR_PREFIX, fname);
        gt_peek_metadata(path, e);

        if (e->name[0] == 0) {
            int k = 0;
            while (e->slug[k] && k < GUI_THEME_NAME_MAX - 1) { e->name[k] = e->slug[k]; k++; }
            e->name[k] = 0;
        }
        count++;
    }
    return count;
}

int gui_theme_get_active_slug(char *slug, int cap) {
    slug[0] = 0;
    static char buf[128];
    // #683: per-user first, then the legacy system copy.
    char _tp[256];
    int n = -1;
    if (userconf_path(THEME_CFG_NAME, _tp, sizeof(_tp)) == 0)
        n = gt_read_whole(_tp, buf, sizeof(buf));
    if (n <= 0)
        n = gt_read_whole(THEME_CFG_LEGACY, buf, sizeof(buf));
    if (n <= 0) return 0;
    int p = 0;
    while (p < n) {
        char line[96];
        int ll = 0;
        while (p < n && buf[p] != '\n' && ll < (int)sizeof(line) - 1) { line[ll++] = buf[p]; p++; }
        if (p < n && buf[p] == '\n') p++;
        if (ll > 0 && line[ll - 1] == '\r') ll--;
        line[ll] = 0;
        if (strncmp(line, "active=", 7) == 0) {
            int k = 0;
            const char *v = line + 7;
            while (v[k] && k < cap - 1) { slug[k] = v[k]; k++; }
            slug[k] = 0;
            // #711: tolerate an "active=<slug>.mtheme" written by the pre-fix
            // writer above. Every image built before this fix has one, and a
            // reader that does not tolerate it silently loses the saved theme.
            if (k > 7 && strcmp(slug + k - 7, ".mtheme") == 0) slug[k - 7] = 0;
            return 1;
        }
    }
    return 0;
}

int gui_theme_is_classic(void) {
    char slug[GUI_THEME_SLUG_MAX];
    if (!gui_theme_get_active_slug(slug, sizeof(slug)) || slug[0] == 0) {
        // No THEME.CFG yet (pre-first-boot-write) - fall back to the old
        // hardcoded id check so behavior in that corner case is unchanged.
        return theme_get_active() == 4;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s%s.mtheme", THEME_DIR_PREFIX, slug);

    gui_theme_entry_t e;
    e.name[0] = 0;
    e.is_dark = 0;
    e.is_classic = 0;
    gt_peek_metadata(path, &e);
    return e.is_classic;
}

int gui_theme_index_append(const char *filename) {
    static char idx[4096];
    int n = gt_read_whole(THEME_INDEX_PATH, idx, sizeof(idx));
    if (n < 0) n = 0;

    // Already listed?
    int p = 0;
    int flen = 0;
    while (filename[flen]) flen++;
    while (p < n) {
        int start = p;
        while (p < n && idx[p] != '\n') p++;
        int ll = p - start;
        if (ll > 0 && idx[start + ll - 1] == '\r') ll--;
        if (ll == flen && strncmp(idx + start, filename, (size_t)flen) == 0) {
            return 0; // already present
        }
        if (p < n && idx[p] == '\n') p++;
    }

    // Rewrite the whole (small, bounded) file with the line appended, the
    // same unlink-then-create idiom Settings uses for its own *.CFG files.
    sys_unlink(THEME_INDEX_PATH);
    int fd = sys_open(THEME_INDEX_PATH, 0x0001 | 0x0040); // O_WRONLY|O_CREAT
    if (fd < 0) return -1;
    if (n > 0) sys_write(fd, idx, (unsigned long)n);
    if (n > 0 && idx[n - 1] != '\n') sys_write(fd, "\n", 1);
    sys_write(fd, filename, (unsigned long)flen);
    sys_write(fd, "\n", 1);
    sys_close(fd);
    return 0;
}

int gui_theme_activate_path(const char *mtheme_path, const char *index_filename) {
    if (!mtheme_path || !mtheme_path[0]) return -1;

    int idx = theme_load_file(mtheme_path);
    if (idx < 0) {
        // FAIL-CLOSED, VISIBLY (themes ticket, 2026-08-07): the kernel already
        // refused to touch the live theme table (theme_load_file_runtime()
        // returns -1 without installing anything on a read/parse failure), so
        // the desktop keeps whatever theme was already active. That part was
        // silent before this: Settings only wrote it to SETLOG.TXT, a file
        // nobody looks at outside a debugging session. A toast makes "your
        // theme didn't load" visible without the user having to go dig for it.
        notify_post("Theme could not be applied",
                    "The theme file could not be read or parsed. The current theme was kept.",
                    NOTIFY_ERROR);
        return -1;
    }

    theme_set_active(idx);

    // (themes ticket) The kernel's runtime contrast floor (theme_ensure_all_
    // contrast()/theme_ensure_v2_contrast() in kernel/gui/themes.c) may have
    // force-corrected one or more fg/bg pairs that were too close to read.
    // The desktop is never at risk either way (that is the floor's job), but
    // a theme author or App Store package with a genuine contrast bug should
    // find out, not have it silently patched over every time.
    {
        int corrected = theme_contrast_corrections(idx);
        if (corrected > 0) {
            char body[128];
            snprintf(body, sizeof(body),
                     "%d color pair(s) were too low-contrast to read and were auto-corrected.",
                     corrected);
            notify_post("Theme adjusted for readability", body, NOTIFY_WARNING);
        }
    }

    if (index_filename && index_filename[0]) {
        gui_theme_index_append(index_filename);
    }

    // Derive the slug (basename minus ".mtheme") to persist in THEME.CFG.
    //
    // #711 BUG FIX: the strcmp below used to run BEFORE slug[sl]=0, so it
    // compared ".mtheme" against uninitialised stack that ran past the end of
    // the copied name. It essentially never matched, so the suffix was never
    // stripped and THEME.CFG was persisted as "active=retro_unix.mtheme". Every
    // later reader then built "/THEMES/retro_unix.mtheme.mtheme":
    //   - gui_theme_is_classic() silently reported MODERN for EVERY theme,
    //     including Retro UNIX and Classic, because its metadata peek could
    //     never open the file (that is the exact id==4 failure mode this
    //     function was written to retire, reintroduced by a missing NUL);
    //   - the compositor's startup restore of the saved theme could not find
    //     the file either, so the saved choice silently did not survive a
    //     reboot;
    //   - and #711's live file poll had nothing it could read.
    // Terminate first, then test.
    const char *base = mtheme_path;
    for (const char *c = mtheme_path; *c; c++) if (*c == '/') base = c + 1;
    char slug[GUI_THEME_SLUG_MAX];
    int sl = 0;
    while (base[sl] && sl < GUI_THEME_SLUG_MAX - 1) { slug[sl] = base[sl]; sl++; }
    slug[sl] = 0;
    if (sl > 7 && strcmp(slug + sl - 7, ".mtheme") == 0) { sl -= 7; slug[sl] = 0; }

    // #683: write the user's own copy. The unlink+create pair is kept (it is how
    // this file has always avoided a stale tail), but both now target the
    // per-user path, so the system default in /CONFIG is never modified.
    char _tw[256];
    if (userconf_path(THEME_CFG_NAME, _tw, sizeof(_tw)) != 0) return -1;
    sys_unlink(_tw);
    int fd = userconf_open_write(THEME_CFG_NAME);
    if (fd >= 0) {
        char buf[96];
        int n = snprintf(buf, sizeof(buf), "active=%s\n", slug);
        if (n > 0) sys_write(fd, buf, (unsigned long)n);
        sys_close(fd);
    }
    return idx;
}

// (#711) See gui_theme.h. Content-hash poll of the ACTIVE theme file.
int gui_theme_poll_reload(void) {
    static char s_slug[GUI_THEME_SLUG_MAX];
    static unsigned long s_hash;
    static int s_primed;

    char slug[GUI_THEME_SLUG_MAX];
    if (!gui_theme_get_active_slug(slug, sizeof(slug)) || slug[0] == 0) return 0;

    char path[96];
    snprintf(path, sizeof(path), "%s%s.mtheme", THEME_DIR_PREFIX, slug);

    static char tb[8192];
    int n = gt_read_whole(path, tb, sizeof(tb));
    if (n <= 0) return 0;

    // FNV-1a over the bytes, salted with the length so a truncating edit that
    // happens to collide still differs.
    unsigned long h = 2166136261UL;
    for (int i = 0; i < n; i++) { h ^= (unsigned long)(unsigned char)tb[i]; h *= 16777619UL; }
    h ^= (unsigned long)n * 16777619UL;

    int changed = (!s_primed) || (strcmp(slug, s_slug) != 0) || (h != s_hash);
    if (!changed) return 0;

    int first = !s_primed;
    int k = 0;
    while (slug[k] && k < GUI_THEME_SLUG_MAX - 1) { s_slug[k] = slug[k]; k++; }
    s_slug[k] = 0;
    s_hash = h;
    s_primed = 1;
    if (first) return 0;          // baseline only

    int idx = theme_load_file(path);
    if (idx < 0) {
        // Same fail-closed contract as gui_theme_activate_path(): a live edit
        // that broke the file leaves the PREVIOUSLY active theme running (the
        // kernel never installed the bad parse), but that needs to be visible
        // to whoever is editing the file, not just a silent no-op every 2s.
        notify_post("Theme file edit not applied",
                    "The edited theme file could not be read or parsed. The current theme was kept.",
                    NOTIFY_ERROR);
        return 0;
    }
    theme_set_active(idx);
    {
        int corrected = theme_contrast_corrections(idx);
        if (corrected > 0) {
            char body[128];
            snprintf(body, sizeof(body),
                     "%d color pair(s) were too low-contrast to read and were auto-corrected.",
                     corrected);
            notify_post("Theme adjusted for readability", body, NOTIFY_WARNING);
        }
    }
    return 1;
}

int gui_theme_activate(const char *slug) {
    if (!slug || !slug[0]) return -1;

    char path[96];
    snprintf(path, sizeof(path), "%s%s.mtheme", THEME_DIR_PREFIX, slug);
    char fname[GUI_THEME_SLUG_MAX + 8];
    snprintf(fname, sizeof(fname), "%s.mtheme", slug);

    return gui_theme_activate_path(path, fname);
}


// ===========================================================================
// (#745) Theme preview: the top-right corner of an example window.
// See gui_theme.h for what this mirrors and why each token is the one it is.
// Design spec: docs/OOBE_THEME_PREVIEW.html (the mock is the spec).
// ===========================================================================

// The kernel's integer luma, byte for byte (kernel/gui/window.c). Not a WCAG
// luminance: this is the exact expression the two decisions below are made
// with in the decorator, and a preview that used a different one would
// disagree with the real window at the boundary.
static unsigned int gtp_luma(unsigned int c) {
    return (((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 + (c & 0xFF) * 29) >> 8;
}
// win_title_ink() from kernel/gui/window.c.
static unsigned int gtp_ink(unsigned int bg) {
    return (gtp_luma(bg) >= 140) ? 0x00232018u : 0x00EDE4D0u;
}
static unsigned int gtp_mix(unsigned int a, unsigned int b, int t, int n) {
    if (n <= 0) n = 1;
    unsigned int r = (((a >> 16) & 0xFF) * (n - t) + ((b >> 16) & 0xFF) * t) / (unsigned)n;
    unsigned int g = (((a >>  8) & 0xFF) * (n - t) + ((b >>  8) & 0xFF) * t) / (unsigned)n;
    unsigned int bl = ((a & 0xFF) * (n - t) + (b & 0xFF) * t) / (unsigned)n;
    return (r << 16) | (g << 8) | bl;
}
// theme_metric_raw_of() returns 0 for "this kernel does not know that id";
// the kernel decorator's own win_metric_or() treats any non-positive value as
// "use the fallback", so this does too and the two cannot disagree.
//
// (#wizflash) Deliberately theme_metric_raw_of(), NOT theme_metric_of(). This
// preview draws through the CALLER's own window (a scale_on window: Settings
// and the first-boot wizard both are), and every win_draw_rect()/win_draw_
// pixel() call below is scaled ONCE at that window's syscall boundary. The
// scaled getter, theme_metric_of(), ALSO multiplies by the current UI scale,
// which would apply the factor twice - measured at 200%: a 20px title bar
// came out 80px instead of 40, overflowing the fixed-size crop this function
// draws into. See SYS_THEME_METRIC_RAW's comment in kernel/proc/syscall.h.
static int gtp_metric(int ti, theme_metric_id_t id, int fallback) {
    int v = theme_metric_raw_of(ti, id);
    return (v > 0) ? v : fallback;
}

void gui_theme_win_preview(int handle, int x, int y, int w, int h,
                           int theme_index, unsigned int cut_bg) {
    if (w <= 0 || h <= 0) return;

    unsigned int win_bg  = theme_color_of(theme_index, THEME_COLOR_WINDOW_BG);
    unsigned int border  = theme_color_of(theme_index, THEME_COLOR_WINDOW_BORDER);
    unsigned int closec  = theme_color_of(theme_index, THEME_COLOR_CLOSE_BUTTON);
    unsigned int tb      = theme_color_of(theme_index, THEME_COLOR_TITLEBAR_ACTIVE);

    int bw   = gtp_metric(theme_index, THEME_METRIC_BORDER_W,        2);
    int tbh  = gtp_metric(theme_index, THEME_METRIC_TITLEBAR_H,     20);
    int btn  = gtp_metric(theme_index, THEME_METRIC_TITLEBAR_BTN,   16);
    int gap  = gtp_metric(theme_index, THEME_METRIC_TITLEBAR_BTN_GAP, 2);
    // An enum/boolean metric, not a pixel count: never scaled by either
    // getter (theme_get_metric_by_id() only multiplies TM_PX ids), so there
    // is no double-scale risk here and no reason to prefer one getter over
    // the other. Left on the plain (scaled-getter) call for that reason.
    int grad = theme_metric_of(theme_index, THEME_METRIC_DECOR_TITLEBAR_GRADIENT);
    // radius 0 is a LEGAL value (square corners), so this one is read
    // UNFILTERED (not through gtp_metric's positive-fallback), but it is
    // still a TM_PX metric and so still needs the UNSCALED getter for the
    // same double-scale reason as bw/tbh/btn/gap above.
    int bev  = theme_metric_raw_of(theme_index, THEME_METRIC_RADIUS_WINDOW);
    if (bev > 6) bev = 6;            // kernel WIN_BEVEL_MAX
    if (bev < 0) bev = 0;

    // #140: a near-white active titlebar is recoloured to the taskbar colour by
    // the decorator, so four of the shipped light themes NEVER show the white
    // this token holds. A preview that skipped this would show the user a
    // titlebar that does not exist.
    int recoloured = 0;
    if (gtp_luma(tb) >= 200) {
        tb = theme_color_of(theme_index, THEME_COLOR_TASKBAR_BG);
        recoloured = 1;
    }
    unsigned int ink = gtp_ink(tb);

    // 1. window background over the whole crop
    win_draw_rect(handle, x, y, w, h, win_bg);

    // 2. titlebar (inset by the border width, exactly as the decorator does)
    int tby = y + bw, tbw = w - bw;
    if (tbw > 0 && tbh > 0) {
        if (grad) {
            unsigned int gt = theme_color_of(theme_index, THEME_COLOR_TITLEBAR_TOP);
            unsigned int gb = theme_color_of(theme_index, THEME_COLOR_TITLEBAR_BOTTOM);
            // The decorator drops the file's stops whenever the fill it is
            // actually painting is not the theme's titlebar colour, which is
            // precisely the recoloured case above.
            if (recoloured || gt == gb) {
                unsigned int r = (tb >> 16) & 0xFF, g = (tb >> 8) & 0xFF, b = tb & 0xFF;
                gt = (((r + (255 - r) * 22 / 100) & 0xFF) << 16)
                   | (((g + (255 - g) * 22 / 100) & 0xFF) << 8)
                   |  ((b + (255 - b) * 22 / 100) & 0xFF);
                gb = tb;
            }
            int span = (tbh > 1) ? tbh - 1 : 1;
            for (int j = 0; j < tbh; j++)
                win_draw_rect(handle, x, tby + j, tbw, 1, gtp_mix(gt, gb, j, span));
        } else {
            win_draw_rect(handle, x, tby, tbw, tbh, tb);
        }
    }

    // 3. the four titlebar buttons, right to left: close, maximise, minimise,
    //    transparency-filter. Same origin arithmetic as the decorator.
    int by = y + bw + (tbh - btn) / 2;
    for (int k = 0; k < 4; k++) {
        int bx = x + w - (k + 1) * btn - 2 - k * gap;
        if (bx < x) break;                        // ran out of crop: stop, never wrap
        unsigned int fill = (k == 0) ? closec : tb;
        unsigned int gl   = (k == 0) ? 0x00FFFFFFu : ink;
        win_draw_rect(handle, bx, by, btn, btn, fill);
        int cx = bx + btn / 2, cy = by + btn / 2;
        if (k == 0) {                              // close: X
            for (int i = -4; i <= 4; i++) {
                win_draw_pixel(handle, cx + i, cy + i, gl);
                win_draw_pixel(handle, cx + i, cy - i, gl);
            }
        } else if (k == 1) {                       // maximise: 7x7 square outline
            win_draw_rect(handle, cx - 3, cy - 3, 7, 1, gl);
            win_draw_rect(handle, cx - 3, cy + 3, 7, 1, gl);
            win_draw_rect(handle, cx - 3, cy - 3, 1, 7, gl);
            win_draw_rect(handle, cx + 3, cy - 3, 1, 7, gl);
        } else if (k == 2) {                       // minimise: bar
            win_draw_rect(handle, cx - 4, cy, 9, 1, gl);
        } else {                                   // transparency: funnel
            for (int i = 0; i < 5; i++) {
                win_draw_pixel(handle, cx - 4 + i, cy - 3 + i, gl);
                win_draw_pixel(handle, cx + 4 - i, cy - 3 + i, gl);
            }
            win_draw_rect(handle, cx, cy + 1, 1, 3, gl);
        }
    }

    // 4. the frame: 1px on the TOP and RIGHT edges only. The left and bottom
    //    edges of this rect are the CROP, not the window, so drawing a border
    //    there would claim the window ends where it does not.
    win_draw_rect(handle, x, y, w, 1, border);
    win_draw_rect(handle, x + w - 1, y, 1, h, border);

    // 5. the corner. A CHAMFER (45 degrees, on the pixel grid), never a radius:
    //    that is what the kernel cuts, and a rounded preview would misrepresent
    //    every theme. bev == 0 leaves the hard square corner retro_unix,
    //    Classic and High Contrast actually have.
    for (int j = 0; j < bev; j++) {
        int cut = bev - j;                        // pixels removed on this row
        if (cut > 0) win_draw_rect(handle, x + w - cut, y + j, cut, 1, cut_bg);
        win_draw_pixel(handle, x + w - 1 - cut, y + j, border);
    }
}
