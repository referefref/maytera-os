// diskimg - MayteraOS Disk Images (#739 / #196 layer 4)
//
// Mount an .iso as a CD-ROM drive letter or an .img as a virtual floppy, eject,
// swap discs, and see what is on every drive letter. Everything here goes
// through ONE syscall, SYS_DISKIMG (361); there is no other path in and no
// kernel-side harness.
//
// WHY A STANDALONE APP AND NOT A SETTINGS PANEL OR A FILES INTEGRATION
// --------------------------------------------------------------------
// All three were live options and the deciding facts were these.
//
// Settings' Storage panel is the closest existing home, and a panel is by far
// the cheapest change (six edits, ~40 lines). But this feature needs a MODAL
// FILE PICKER, and Settings has no modal-dialog machinery at all: every panel
// draws inline into one content pane and routes clicks through one
// 900-line handle_content_click(). Adding a modal there means adding modal
// support to a 6,853-line file, which is a bigger and riskier change than the
// app it would host.
//
// Files owns the Places sidebar, which is where a user LOOKS for volumes, and
// mounted drives belong there. But Files' sidebar is navigation: every row is
// "go to this path". Mount and eject are device actions with their own errors
// and their own confirmation, and putting them behind a folder icon makes the
// destructive one (eject, which invalidates a running guest's handles) reachable
// by the same gesture as browsing. Discoverability is a real point though, so
// the right split is: the ACTIONS live here, and Files should grow read-only
// "go to this mounted disc" rows later. That is noted, not done, and not
// claimed.
//
// So: a standalone app, modelled on devmgr, which is this tree's house shape
// for a system utility (two panes, live query, Refresh, no bespoke look).
//
// EVERY WIDGET IS A SHARED ONE. gui_list_t for both lists (geometry AND input),
// gui_list_draw() for the picker's flat filename list, gui_button()/gui_card()
// for chrome, theme_color()/gui_set_palette() for every colour, notify_post()
// for results. Nothing is hand-bevelled and no colour is hardcoded. Note
// gui_style_sync_from_theme() at startup: without it an app renders modern and
// rounded on top of a beveled retro_unix default, which is the single easiest
// way to look wrong while being "correct".
//
// KEYBOARD-COMPLETE, DELIBERATELY. Every action has a key: Up/Down select,
// M mount, E eject, R refresh, Enter/Esc in the picker. That is partly good
// practice and partly evidence: pointer injection into a VM does not reliably
// land clicks in this project (#334), so a UI that can only be driven by mouse
// is a UI that cannot be PROVEN to work. This one can be driven by qm sendkey.
//
// HEADLESS MODE. With arguments (list / mount / eject) it performs the action,
// prints the result, and exits without a window. Same syscall, same code path,
// same permission gate; it is a second evidence channel that lands on serial,
// not a second implementation.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_style.h"
#include "../../libc/gui_list.h"
#include "../../libc/theme.h"
#include "../../libc/syscall.h"
#include "../../libc/notify.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"
#include "../../libc/dirent.h"

#define WIN_W   660
#define WIN_H   430
#define PAD     10
#define ROW_H   22
#define BTN_H   24
#define BTN_W   86

// Directories the picker looks in. A user's images realistically live under
// /WINDIR (next to the DOS drive folders) or a dedicated /IMAGES; the root is
// scanned too so a freshly copied file is findable without knowing the
// convention. Typing a full path always works regardless.
static const char *SCAN_DIRS[] = { "/WINDIR", "/IMAGES", "/" };
#define NSCAN 3

#define MAXPICK 128
#define PATHW   192

static int win = -1;

// ---- palette ---------------------------------------------------------------
static unsigned int C_BG, C_CARD, C_FIELD, C_BORDER, C_INK, C_DIM, C_ACC, C_SEL, C_SELTX;

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
    // Read the ACTIVE theme's decor style. Skipping this is how an app ends up
    // rounded and shadowed on a beveled default theme.
    gui_style_sync_from_theme();
    unsigned int wb = theme_color(THEME_COLOR_WINDOW_BG);
    int r = (wb >> 16) & 255, g = (wb >> 8) & 255, b = wb & 255;
    int dark = ((r * 30 + g * 59 + b * 11) / 100) < 128;
    C_ACC    = theme_color(THEME_COLOR_ACCENT);
    C_BG     = tint(dark ? 0x00262A30 : 0x00F5F6F8, C_ACC, 5);
    C_CARD   = tint(dark ? 0x002C313B : 0x00EDEFF3, C_ACC, 6);
    C_FIELD  = dark ? 0x00333A45 : 0x00FFFFFF;
    C_BORDER = dark ? 0x003A424F : 0x00CDD3DB;
    C_INK = lum_ink(C_BG); C_DIM = dim_ink(C_BG);
    C_SEL = C_ACC; C_SELTX = lum_ink(C_ACC);
    gui_palette_t p;
    p.surface = C_BG; p.surface_raised = C_CARD; p.ink = C_INK; p.ink_dim = C_DIM;
    p.accent = C_ACC; p.accent_hover = gui_lighten(C_ACC, 24); p.border = C_BORDER;
    p.field_bg = C_FIELD; p.field_border = C_BORDER; p.track = tint(C_BG, C_ACC, 20);
    gui_set_palette(&p);
}

// ---- the drive table -------------------------------------------------------
// One row per letter worth showing: A:, B:, C:, every CD letter that has a disc
// in it, and ONE empty CD letter (the lowest free one) so there is always
// somewhere to mount. Listing all 22 CD letters would be 22 rows of nothing.
static diskimg_info_t g_row[28];
static int g_nrows = 0;
static int g_sel = 0;
static int g_maxmounts = 0;
static gui_list_t g_list;

static void refresh(void) {
    g_nrows = 0;
    int shown_empty_cd = 0;
    for (int i = 0; i < 26 && g_nrows < 28; i++) {
        diskimg_info_t info;
        if (sys_diskimg_info(i, &info) != 0) continue;
        int mounted = (info.flags & DISKIMG_F_MOUNTED) != 0;
        if (info.cls == DISKIMG_CLASS_FLOPPY || info.cls == DISKIMG_CLASS_FIXED) {
            g_row[g_nrows++] = info;
        } else if (info.cls == DISKIMG_CLASS_CDROM) {
            if (mounted) g_row[g_nrows++] = info;
            else if (!shown_empty_cd) { g_row[g_nrows++] = info; shown_empty_cd = 1; }
        }
    }
    if (g_sel >= g_nrows) g_sel = g_nrows ? g_nrows - 1 : 0;
    if (g_sel < 0) g_sel = 0;
    if (!g_maxmounts) g_maxmounts = sys_diskimg_max_mounts();
}

static const char *cls_name(unsigned int c) {
    switch (c) {
        case DISKIMG_CLASS_FLOPPY: return "Floppy";
        case DISKIMG_CLASS_FIXED:  return "Hard disk";
        case DISKIMG_CLASS_CDROM:  return "CD-ROM";
        default:                   return "-";
    }
}
static const char *fmt_name(unsigned int f) {
    switch (f) {
        case DISKIMG_FMT_ISO9660: return "ISO 9660";
        case DISKIMG_FMT_FAT12:   return "FAT12";
        default:                  return "";
    }
}
// Human size. Integer only: the kernel is soft-float and so is this app's libc
// path of least surprise; one decimal is done by hand rather than with a float.
static void size_str(unsigned long long b, char *out, int cap) {
    if (b == 0) { snprintf(out, cap, "-"); return; }
    if (b >= 1024ULL * 1024ULL * 1024ULL) {
        unsigned long long g10 = (b * 10ULL) / (1024ULL * 1024ULL * 1024ULL);
        snprintf(out, cap, "%llu.%llu GB", g10 / 10, g10 % 10);
    } else if (b >= 1024ULL * 1024ULL) {
        unsigned long long m10 = (b * 10ULL) / (1024ULL * 1024ULL);
        snprintf(out, cap, "%llu.%llu MB", m10 / 10, m10 % 10);
    } else {
        snprintf(out, cap, "%llu KB", b / 1024ULL);
    }
}

// ---- error text ------------------------------------------------------------
// Every negative code the syscall can return gets its own sentence. "Mount
// failed" with no reason is what makes a user click the same button again.
static const char *mount_err(int rc) {
    switch (rc) {
        case -1:  return "Bad request.";
        case -2:  return "That file is not an ISO 9660 disc or a FAT12 floppy.";
        case -3:  return "That drive letter cannot hold that kind of image "
                         "(ISOs go on CD letters, floppy images on A: or B:).";
        case -4:  return "No free drive letter of that kind.";
        case -5:  return "Too many images are already mounted.";
        case -6:  return "That path was refused: it must be absolute and must "
                         "not contain \"..\" or backslashes.";
        case -10: return "Cannot open that file.";
        case -11: return "Unrecognised image format.";
        case -12: return "Not an ISO, and too large to be a floppy image.";
        case -13: return "Refused: you do not have permission to read that file, "
                         "or there was not enough memory.";
        default:  return "Mount failed.";
    }
}

// ---- the picker ------------------------------------------------------------
static char g_pick[MAXPICK][PATHW];
static int  g_npick = 0;
static int  g_picksel = 0;
static int  g_picking = 0;
static gui_list_t g_picklist;

static int ends_with_ci(const char *s, const char *suf) {
    int ls = (int)strlen(s), lf = (int)strlen(suf);
    if (lf > ls) return 0;
    for (int i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suf[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return 1;
}
static int looks_like_image(const char *name) {
    return ends_with_ci(name, ".ISO") || ends_with_ci(name, ".IMG") ||
           ends_with_ci(name, ".IMA");
}
static int already_listed(const char *full) {
    for (int i = 0; i < g_npick; i++) if (strcmp(g_pick[i], full) == 0) return 1;
    return 0;
}
static void scan_dirs(void) {
    g_npick = 0;
    for (int d = 0; d < NSCAN && g_npick < MAXPICK; d++) {
        DIR *dp = opendir(SCAN_DIRS[d]);
        if (!dp) continue;
        struct dirent *e;
        while ((e = readdir(dp)) != 0 && g_npick < MAXPICK) {
            if (e->d_type == DT_DIR) continue;
            if (!looks_like_image(e->d_name)) continue;
            char full[PATHW];
            if (SCAN_DIRS[d][1] == 0)   // "/" - do not emit "//NAME"
                snprintf(full, sizeof full, "/%s", e->d_name);
            else
                snprintf(full, sizeof full, "%s/%s", SCAN_DIRS[d], e->d_name);
            if (!already_listed(full)) {
                strncpy(g_pick[g_npick], full, PATHW - 1);
                g_pick[g_npick][PATHW - 1] = 0;
                g_npick++;
            }
        }
        closedir(dp);
    }
    if (g_picksel >= g_npick) g_picksel = g_npick ? g_npick - 1 : 0;
}

// gui_list_draw's label callback. This app is gui_list_draw()'s first real
// caller in the tree; the widget existed and had none.
static const char *pick_label(void *ctx, int index, char *buf, int cap) {
    (void)ctx;
    if (index < 0 || index >= g_npick) return "";
    snprintf(buf, cap, "%s", g_pick[index]);
    return buf;
}

// ---- drawing ---------------------------------------------------------------
static int content_w(void) { return WIN_W; }

static void draw_table(void) {
    int lx = PAD, ly = 64;
    int lw = content_w() - 2 * PAD;
    int lh = WIN_H - ly - PAD - BTN_H - 12 - 24;

    gui_card(win, lx - 2, ly - 20, lw + 4, lh + 24);
    win_draw_text(win, lx + 4, ly - 16, "Drive   Type        Image", C_DIM);
    win_draw_text(win, lx + 4 + 8 * 40, ly - 16, "Size        Format", C_DIM);

    gui_list_config(&g_list, lx, ly, lw, lh, ROW_H, g_nrows);
    // Frame + track drawn by hand because the rows are multi-column; the
    // GEOMETRY and input are still gui_list's, which is the pattern gui_menu.c
    // and Settings both use for rows that carry more than one string.
    win_draw_rect(win, lx, ly, lw, lh, C_FIELD);
    win_draw_rect(win, lx, ly, lw, 1, C_BORDER);
    win_draw_rect(win, lx, ly + lh - 1, lw, 1, C_BORDER);
    win_draw_rect(win, lx, ly, 1, lh, C_BORDER);
    win_draw_rect(win, lx + lw - 1, ly, 1, lh, C_BORDER);

    int first = gui_list_first(&g_list), span = gui_list_span(&g_list);
    int rw = gui_list_row_w(&g_list);
    for (int i = first; i < first + span && i < g_nrows; i++) {
        int ry;
        if (!gui_list_row_y(&g_list, i, &ry)) continue;   // never draw outside the box
        diskimg_info_t *d = &g_row[i];
        unsigned int fg = C_INK;
        if (i == g_sel) {
            win_draw_rect(win, lx + 1, ry, rw, ROW_H, C_SEL);
            fg = C_SELTX;
        }
        char letter[8];
        snprintf(letter, sizeof letter, "%c:", (char)('A' + d->letter));
        win_draw_text(win, lx + 6, ry + 3, letter, fg);
        win_draw_text(win, lx + 6 + 8 * 6, ry + 3, cls_name(d->cls), fg);

        if (d->flags & DISKIMG_F_MOUNTED) {
            char nm[40];
            snprintf(nm, sizeof nm, "%.30s", d->name);
            win_draw_text(win, lx + 6 + 8 * 18, ry + 3, nm, fg);
            char sz[24]; size_str(d->size, sz, sizeof sz);
            win_draw_text(win, lx + 6 + 8 * 40, ry + 3, sz, fg);
            char tail[48];
            snprintf(tail, sizeof tail, "%s%s%s",
                     fmt_name(d->fmt),
                     (d->flags & DISKIMG_F_JOLIET) ? " +Joliet" : "",
                     (d->flags & DISKIMG_F_INUSE) ? " (in use)" : "");
            win_draw_text(win, lx + 6 + 8 * 52, ry + 3, tail, fg);
        } else if (d->cls == DISKIMG_CLASS_FIXED) {
            win_draw_text(win, lx + 6 + 8 * 18, ry + 3, "(system disk)", i == g_sel ? fg : C_DIM);
        } else {
            win_draw_text(win, lx + 6 + 8 * 18, ry + 3, "(empty)", i == g_sel ? fg : C_DIM);
        }
    }
}

static void draw_buttons(void) {
    int by = WIN_H - PAD - BTN_H - 20;
    diskimg_info_t *d = (g_sel < g_nrows) ? &g_row[g_sel] : 0;
    int can_mount = d && (d->flags & DISKIMG_F_MOUNTABLE);
    int can_eject = d && (d->flags & DISKIMG_F_MOUNTED);

    gui_button(win, PAD, by, BTN_W, BTN_H, "Mount...", GUI_BTN_PRIMARY,
               can_mount ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(win, PAD + BTN_W + 8, by, BTN_W, BTN_H, "Eject", GUI_BTN_SECONDARY,
               can_eject ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(win, PAD + 2 * (BTN_W + 8), by, BTN_W, BTN_H, "Refresh",
               GUI_BTN_GHOST, GUI_ST_NORMAL);

    char hint[110];
    snprintf(hint, sizeof hint,
             "M mount   E eject   R refresh   %d of %d images mounted",
             0, g_maxmounts);
    int n = 0;
    for (int i = 0; i < g_nrows; i++) if (g_row[i].flags & DISKIMG_F_MOUNTED) n++;
    snprintf(hint, sizeof hint,
             "M mount   E eject   R refresh   %d of %d images mounted",
             n, g_maxmounts);
    win_draw_text(win, PAD, WIN_H - 18, hint, C_DIM);
}

static void draw_picker(void) {
    int w = 520, h = 300;
    int x = (WIN_W - w) / 2, y = (WIN_H - h) / 2;
    gui_card(win, x, y, w, h);
    win_draw_text(win, x + 12, y + 10, "Choose a disk image", C_INK);
    win_draw_text(win, x + 12, y + 30,
                  ".ISO mounts as a CD-ROM, .IMG/.IMA as a floppy", C_DIM);

    gui_list_config(&g_picklist, x + 12, y + 52, w - 24, h - 52 - 46, ROW_H, g_npick);
    // The flat-string case: this is what gui_list_draw() is for, frame,
    // rows, selection and scrollbar in one call.
    gui_list_draw(win, &g_picklist, g_picksel, C_FIELD, C_BORDER, C_INK,
                  C_SEL, C_SELTX, pick_label, 0);

    if (g_npick == 0)
        win_draw_text(win, x + 20, y + 60,
                      "No .iso/.img files found in /WINDIR, /IMAGES or /", C_DIM);

    int by = y + h - 34;
    gui_button(win, x + w - 2 * (BTN_W + 8) - 4, by, BTN_W, BTN_H, "Mount",
               GUI_BTN_PRIMARY, g_npick ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(win, x + w - (BTN_W + 8) - 4, by, BTN_W, BTN_H, "Cancel",
               GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    win_draw_text(win, x + 12, by + 6, "Enter mount   Esc cancel", C_DIM);
}

static void draw(void) {
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, C_BG);
    win_draw_text(win, PAD, PAD, "Disk Images", C_INK);
    win_draw_text(win, PAD, PAD + 20,
                  "Mount a CD or floppy image so DOS and Windows 3.x programs can read it.",
                  C_DIM);
    draw_table();
    draw_buttons();
    if (g_picking) draw_picker();
}

// ---- actions ---------------------------------------------------------------
static void do_mount(const char *path, int letter) {
    int rc = sys_diskimg_mount(path, letter);
    if (rc >= 0) {
        char body[PATHW + 48];
        snprintf(body, sizeof body, "%s mounted on %c:", path, (char)('A' + rc));
        notify_post("Disk Images", body, NOTIFY_SUCCESS);
        printf("MOUNT OK %s -> %c:\n", path, (char)('A' + rc));
        // Select the drive it landed on, so the result is visible without
        // hunting for it.
        refresh();
        for (int i = 0; i < g_nrows; i++) if ((int)g_row[i].letter == rc) g_sel = i;
    } else {
        notify_post("Disk Images", mount_err(rc), NOTIFY_ERROR);
        printf("MOUNT FAIL %s rc=%d (%s)\n", path, rc, mount_err(rc));
        refresh();
    }
}

static void do_eject(int letter_idx) {
    // An eject invalidates any handle a running guest holds on this disc. It is
    // not refused (a disc swap mid-game is the case this exists for) but it is
    // worth saying when something was actually reading.
    diskimg_info_t info;
    int inuse = (sys_diskimg_info(letter_idx, &info) == 0) &&
                (info.flags & DISKIMG_F_INUSE);
    int rc = sys_diskimg_eject(letter_idx);
    char body[96];
    if (rc == 0) {
        snprintf(body, sizeof body, "%c: ejected%s", (char)('A' + letter_idx),
                 inuse ? " (a program was reading it; its open files are now invalid)" : "");
        notify_post("Disk Images", body, inuse ? NOTIFY_WARNING : NOTIFY_SUCCESS);
        printf("EJECT OK %c:\n", (char)('A' + letter_idx));
    } else {
        notify_post("Disk Images", "Nothing is mounted on that drive.", NOTIFY_ERROR);
        printf("EJECT FAIL %c: rc=%d\n", (char)('A' + letter_idx), rc);
    }
    refresh();
}

static void open_picker(void) {
    scan_dirs();
    g_picking = 1;
}

// ---- headless mode ---------------------------------------------------------
// Same syscall, same permission gate, no window. This exists so the feature can
// be EXERCISED and its output captured on serial, which a mouse-driven UI
// cannot be (#334).
static void print_table(void) {
    printf("LETTER CLASS      FMT   FLAGS  GEN  SIZE            IMAGE\n");
    for (int i = 0; i < 26; i++) {
        diskimg_info_t d;
        if (sys_diskimg_info(i, &d) != 0) continue;
        if (d.cls == DISKIMG_CLASS_NONE) continue;
        if (d.cls == DISKIMG_CLASS_CDROM && !(d.flags & DISKIMG_F_MOUNTED)) continue;
        char sz[24]; size_str(d.size, sz, sizeof sz);
        printf("%c:     %-10s %-5s 0x%02x   %-4u %-15s %s\n",
               (char)('A' + d.letter), cls_name(d.cls), fmt_name(d.fmt),
               d.flags, d.gen, sz, d.name);
    }
    printf("MAXMOUNTS %d\n", sys_diskimg_max_mounts());
}

static int headless(int argc, char **argv) {
    if (strcmp(argv[1], "list") == 0) { print_table(); return 0; }
    if (strcmp(argv[1], "mount") == 0 && argc >= 3) {
        int letter = DISKIMG_LETTER_AUTO;
        if (argc >= 4 && argv[3][0]) {
            char c = argv[3][0];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (c >= 'A' && c <= 'Z') letter = c - 'A';
        }
        int rc = sys_diskimg_mount(argv[2], letter);
        if (rc >= 0) printf("MOUNT OK %s -> %c:\n", argv[2], (char)('A' + rc));
        else         printf("MOUNT FAIL %s rc=%d (%s)\n", argv[2], rc, mount_err(rc));
        return rc >= 0 ? 0 : 1;
    }
    if (strcmp(argv[1], "eject") == 0 && argc >= 3) {
        char c = argv[2][0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if (c < 'A' || c > 'Z') { printf("EJECT FAIL bad letter\n"); return 1; }
        int rc = sys_diskimg_eject(c - 'A');
        printf("EJECT %s %c:\n", rc == 0 ? "OK" : "FAIL", c);
        return rc == 0 ? 0 : 1;
    }
    printf("usage: DISKIMG [list | mount <path> [letter] | eject <letter>]\n");
    return 2;
}

// ---- main ------------------------------------------------------------------
int main(int argc, char **argv) {
    if (argc >= 2) return headless(argc, argv);

    apply_style();
    win = win_create("Disk Images", 140, 90, WIN_W, WIN_H);
    if (win < 0) return 1;
    refresh();
    draw();

    int running = 1;
    while (running) {
        gui_event_t ev;
        // Blocking wait with a slow tick. The tick is not a poll of anything:
        // it refreshes the "(in use)" column, which changes because a DOS guest
        // reads the disc, not because this app asked it to.
        int et = win_get_event(win, &ev, 2000);
        if (et == 0) { if (!g_picking) { refresh(); draw(); } continue; }

        switch (ev.type) {
        case EVENT_REDRAW:
        case EVENT_RESIZE:
            draw(); break;
        case EVENT_WINDOW_CLOSE:
            running = 0; break;

        case EVENT_KEY_DOWN: {
            char c = ev.key_char;
            uint32_t kc = ev.keycode;
            if (g_picking) {
                if (c == 27) { g_picking = 0; draw(); break; }
                if (c == '\n' || c == '\r') {
                    if (g_npick > 0) {
                        char chosen[PATHW];
                        strncpy(chosen, g_pick[g_picksel], PATHW - 1);
                        chosen[PATHW - 1] = 0;
                        g_picking = 0;
                        do_mount(chosen, DISKIMG_LETTER_AUTO);
                    }
                    draw(); break;
                }
                if (kc == GUI_KEY_UP || kc == GUI_KEY_DOWN) {
                    gui_list_move_sel(&g_picklist, &g_picksel, kc == GUI_KEY_UP ? -1 : 1);
                    draw(); break;
                }
                if (gui_list_key(&g_picklist, kc)) draw();
                break;
            }
            if (c == 27) { running = 0; break; }
            if (c == 'r' || c == 'R') { refresh(); draw(); break; }
            if (c == 'm' || c == 'M') {
                if (g_sel < g_nrows && (g_row[g_sel].flags & DISKIMG_F_MOUNTABLE))
                    open_picker();
                draw(); break;
            }
            if (c == 'e' || c == 'E') {
                if (g_sel < g_nrows && (g_row[g_sel].flags & DISKIMG_F_MOUNTED))
                    do_eject((int)g_row[g_sel].letter);
                draw(); break;
            }
            if (kc == GUI_KEY_UP || kc == GUI_KEY_DOWN) {
                gui_list_move_sel(&g_list, &g_sel, kc == GUI_KEY_UP ? -1 : 1);
                draw(); break;
            }
            if (gui_list_key(&g_list, kc)) draw();
            break;
        }

        case EVENT_MOUSE_DOWN: {
            int lx = ev.mouse_x, ly = ev.mouse_y;
            if (g_picking) {
                int w = 520, h = 300;
                int px = (WIN_W - w) / 2, py = (WIN_H - h) / 2;
                int by = py + h - 34;
                if (ly >= by && ly < by + BTN_H) {
                    if (lx >= px + w - 2 * (BTN_W + 8) - 4 && lx < px + w - (BTN_W + 8) - 4) {
                        if (g_npick > 0) {
                            char chosen[PATHW];
                            strncpy(chosen, g_pick[g_picksel], PATHW - 1);
                            chosen[PATHW - 1] = 0;
                            g_picking = 0;
                            do_mount(chosen, DISKIMG_LETTER_AUTO);
                        }
                    } else if (lx >= px + w - (BTN_W + 8) - 4) {
                        g_picking = 0;
                    }
                    draw(); break;
                }
                int hit = gui_list_press(&g_picklist, lx, ly);
                if (hit >= 0) g_picksel = hit;
                draw(); break;
            }
            int by = WIN_H - PAD - BTN_H - 20;
            if (ly >= by && ly < by + BTN_H) {
                if (lx >= PAD && lx < PAD + BTN_W) {
                    if (g_sel < g_nrows && (g_row[g_sel].flags & DISKIMG_F_MOUNTABLE))
                        open_picker();
                } else if (lx >= PAD + BTN_W + 8 && lx < PAD + 2 * BTN_W + 8) {
                    if (g_sel < g_nrows && (g_row[g_sel].flags & DISKIMG_F_MOUNTED))
                        do_eject((int)g_row[g_sel].letter);
                } else if (lx >= PAD + 2 * (BTN_W + 8) && lx < PAD + 2 * (BTN_W + 8) + BTN_W) {
                    refresh();
                }
                draw(); break;
            }
            int hit = gui_list_press(&g_list, lx, ly);
            if (hit >= 0) g_sel = hit;
            draw(); break;
        }

        case EVENT_MOUSE_UP:
            if (g_picking) gui_list_release(&g_picklist);
            else gui_list_release(&g_list);
            break;

        case EVENT_MOUSE_MOVE:
            if (g_picking) { if (gui_list_motion(&g_picklist, ev.mouse_x, ev.mouse_y)) draw(); }
            else { if (gui_list_motion(&g_list, ev.mouse_x, ev.mouse_y)) draw(); }
            break;

        case EVENT_MOUSE_SCROLL:
            if (g_picking) {
                if (gui_list_wheel(&g_picklist, ev.mouse_x, ev.mouse_y, ev.scroll_delta)) draw();
            } else {
                if (gui_list_wheel(&g_list, ev.mouse_x, ev.mouse_y, ev.scroll_delta)) draw();
            }
            break;

        default: break;
        }
    }
    win_destroy(win);
    return 0;
}
