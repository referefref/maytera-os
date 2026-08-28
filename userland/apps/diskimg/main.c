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
// "go to this mounted disc" rows later.
//
// #234i UPDATE: THAT HAPPENED, AND IT WAS NOT A FILES CHANGE. The half of the
// split above that this file deferred turned out to be free: #250 had already
// built a volume surface (SYS_VOL_LIST -> the Files "Removable" sidebar
// section and the desktop volume icons) and wired only USB hot-plug into it.
// Mounted images now publish into that SAME list from dos/diskimg.c
// (diskimg_vol_raw), so Files and the desktop grew disc and floppy rows with
// no second list and no second idea of what is mounted. Eject from those rows
// routes to the same diskimg_eject_idx() this app calls.
//
// What did NOT move: MOUNT still lives only here, because mounting names a
// path, needs a file picker and carries a permission check. This app remains
// the place you PUT a disc in; Files is where you find one that is already in.
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
#include "../../libc/textfield.h"

// (#appstyle) EVERY string in this window is antialiased TrueType from here on.
//
// THIS IS THE WHOLE TYPOGRAPHY BUG, AND IT IS NOT A WRONG CONSTANT. Until this
// line, all 16 text draws in this file went through win_draw_text(), which is
// the kernel's fixed 8x16 BITMAP font (proc/syscall.c:7172 walks a 16-byte
// glyph array and advances cx += 8). There is NO size argument on that code
// path at all, so the window could not have a type hierarchy even in
// principle: heading, description, column headers, rows and the keyboard hint
// were one size because one size was all that existed. Next to the compositor's
// own TrueType titlebar it read as a different application.
//
// The shim is the two lines files/main.c:21-22 already uses, verbatim, so this
// window and the Files window now resolve identically. 14 is type.body and 11
// is type.caption from docs/UI_STYLE_GUIDE.md 4.2. Note these are SIZE UNITS,
// not CSS px: ttf.c scales with stbtt_ScaleForPixelHeight(), which maps
// ascent-descent (not the em square) onto the number, so one unit is 0.859 em
// px on the shipped DejaVu Sans and 14 renders at em 12.03px. Picking 12 here
// "because the mock says 12px" ships the whole app 14% small.
//
// textfield.h is included ABOVE this point on purpose: its own
// gui_draw_textfield_tf() is the monospace renderer and legitimately wants the
// real win_draw_text(). Defining the macro first would silently redirect it.
#define win_draw_text(h, x, y, s, c)       win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define win_draw_text_small(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 11, (c))
#define TY_TITLE   16
#define TY_BODY    14
#define TY_CAPTION 11

#define WIN_W   660
#define WIN_H   430

// #184: WIN_W/WIN_H are what win_create() is GIVEN, and that is the OUTER
// window size: the frame and title bar come out of it, so the drawable content
// is smaller. MEASURED on build 1998 by reading the framebuffer: an outer height
// of 430 gave 406 rows of content, and the "M mount  E eject  R refresh  N of M
// images mounted" hint, drawn at WIN_H-18, landed 6px below the last drawable
// row and was never visible on screen. The app's only statement of its own key
// bindings and of how many mount slots were in use could not be read.
//
// win_get_size() (SYS_WIN_GET_SIZE) answers with the CONTENT size, so it is
// asked rather than assumed: the 24px difference is the compositor's current
// chrome, not a constant of the universe, and a theme with a taller title bar
// would silently reintroduce the same clipping under any hardcoded number.
// content_h()/content_w() are what every y/x coordinate below is computed from;
// nothing may use WIN_H as a coordinate again.
static int g_cw = WIN_W, g_ch = WIN_H;
static void sync_content_size(int w) {
    int cw = 0, ch = 0;
    if (w >= 0 && win_get_size(w, &cw, &ch) == 0 && cw > 0 && ch > 0) {
        g_cw = cw; g_ch = ch;
    }
}
#define CONTENT_H  (g_ch)
#define PAD     10
#define ROW_H   24
#define BTN_H   26
#define BTN_W   86

// (#appstyle) Bands, not floating controls. The old layout put the three
// buttons and the key hint at CONTENT_H-minus-something, and #184 records what
// that costs: an outer height of 430 gives 406 drawable rows, the hint was
// drawn at WIN_H-18, and the app's only statement of its own key bindings
// landed six pixels past the last visible row. A band whose height is
// SUBTRACTED from the list before the list is laid out cannot do that, whatever
// the chrome height turns out to be.
#define TOOLBAR_H  40
#define STATUS_H   26
#define DESC_Y     48
#define HDR_Y      70
#define LIST_Y     88

// Directories the picker looks in. A user's images realistically live under
// /WINDIR (next to the DOS drive folders) or a dedicated /IMAGES; the root is
// scanned too so a freshly copied file is findable without knowing the
// convention. A path outside all three is typed into the picker's path field
// (#appstyle: that field is new - the sentence that used to sit here claimed it
// already existed, and it did not).
static const char *SCAN_DIRS[] = { "/WINDIR", "/IMAGES", "/" };
#define NSCAN 3

#define MAXPICK 128
#define PATHW   192

static int win = -1;

// ---- palette ---------------------------------------------------------------
static unsigned int C_BG, C_CARD, C_FIELD, C_BORDER, C_INK, C_ACC, C_SEL, C_SELTX;
// (#appstyle) THREE dim inks, not one, because a single "dim" is only ever
// guaranteed against ONE background and this window has three.
//
// MEASURED on Modern Dark with a one-dim palette: the description and column
// headers (on C_BG) cleared 4.61:1, but the SAME ink measured 3.84:1 on the
// list fill and 4.16:1 / 3.94:1 in the status band, because those bands are
// C_FIELD and C_CARD. "Guaranteed readable" is a statement about a PAIR, and
// carrying one member of the pair around while changing the other is exactly
// how a colour that passed its own test ships failing.
static unsigned int C_DIM;        // on C_BG    (description, column headers)
static unsigned int C_DIM_CARD;   // on C_CARD  (status band)
static unsigned int C_DIM_FIELD;  // on C_FIELD (an unmounted row's "(empty)")

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
    C_INK = lum_ink(C_BG);
    (void)0;
    // (#appstyle) dim_ink()'s 50/50 average of ink and background MEASURES
    // 3.26:1 on retro_unix (#818384 on #EAEEF0), which is under the 4.5:1 WCAG
    // text floor - and it is the colour the column headers, the description
    // line, the status bar and every "(empty)" cell are drawn in, i.e. most of
    // the text in the window. It is kept as the STARTING point (so a theme's
    // own tone survives where it already passes) and walked to the floor by the
    // shared primitive, which guarantees it on all 14 themes rather than on the
    // one that happened to be open when the number was chosen.
    C_DIM       = gui_ensure_contrast(dim_ink(C_BG),    C_BG,    GUI_FLOOR_TEXT);
    C_DIM_CARD  = gui_ensure_contrast(dim_ink(C_CARD),  C_CARD,  GUI_FLOOR_TEXT);
    C_DIM_FIELD = gui_ensure_contrast(dim_ink(C_FIELD), C_FIELD, GUI_FLOOR_TEXT);
    // (#appstyle) SELECTION IS A TINT OF THE ACCENT, NOT THE RAW ACCENT.
    //
    // This is Files' answer (fp_sel() in apps/files/main.c) adopted verbatim,
    // and Files arrived at it the hard way: "the raw accent rendered near-black
    // on Nord and made the selected row + its text unreadable". Measured here
    // on Modern Dark, raw #0A84FF forced the label to near-BLACK to reach the
    // text floor at all - legible, but a dark label on a saturated blue is not
    // what a selected row looks like anywhere else in this OS, and the row's
    // secondary column had nowhere left to go.
    //
    // A tint keeps the row's ink at its normal colour, so a selected row and an
    // unselected one differ by BACKGROUND, which is what a selection is.
    C_SEL   = tint(dark ? 0x003C434F : 0x00CCD6E6, C_ACC, dark ? 28 : 26);
    C_SELTX = gui_ensure_contrast(lum_ink(C_SEL), C_SEL, GUI_FLOOR_TEXT);
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
        // #184. Its own sentence, not folded into "unrecognised format": this
        // file IS an ISO, so telling the user it is not one sends them looking
        // for the wrong problem. The measured cause is an incomplete copy or
        // download.
        case -15: return "That ISO is truncated or corrupt: its directory is "
                         "past the end of the file. Copy or download it again.";
        default:  return "Mount failed.";
    }
}

// ---- the picker ------------------------------------------------------------
static char g_pick[MAXPICK][PATHW];
static int  g_npick = 0;
static int  g_picksel = 0;
static int  g_picking = 0;
static gui_list_t g_picklist;

// (#appstyle) The picker now has two focusable things, so it needs a focus
// model. Two, not "the list plus a special case": Tab moves between them, both
// draw a visible focus indication, and Enter means the same thing in both
// (mount whatever the path field currently holds) so there is no state in which
// the primary action does something the user cannot see.
enum { PKF_PATH = 0, PKF_LIST = 1 };
static int         g_pkfocus = PKF_PATH;
static textfield_t g_path;
static char        g_pathbuf[PATHW];

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
static int content_w(void) { return g_cw; }
static int list_h(void)    { return CONTENT_H - LIST_Y - STATUS_H - PAD; }

// ONE column table, computed from the list's OWN geometry and used by BOTH the
// header row and the data rows.
//
// WHY IT IS A STRUCT AND NOT SIX EXPRESSIONS. The header used to be one string
// with spaces in it ("Drive   Type        Image") drawn at lx+4, while the rows
// were drawn at lx+6+8*N. Those are two independent layouts that happened to
// nearly agree under an 8px monospace cell and cannot agree under a
// proportional one - measured on build 2058, the Image header sat 14px left of
// the Image column. Deriving both from the same struct makes disagreement
// inexpressible rather than merely unlikely.
//
// The right-hand columns hang off gui_list_row_w(), which already subtracts the
// scrollbar gutter when one is showing, so Size and Format do not slide under
// the scrollbar the moment a 5th mounted image appears.
typedef struct { int drive, type, image, image_w, size_r, fmt; } cols_t;
static cols_t g_col;

static void cols_compute(int lx, int rw) {
    int rx = lx + 1;
    g_col.drive  = rx + 6;
    g_col.type   = rx + 54;
    g_col.image  = rx + 142;
    g_col.fmt    = rx + rw - 124;
    g_col.size_r = rx + rw - 134;         // RIGHT edge: numerals right-align (4.5)
    g_col.image_w = g_col.size_r - 62 - g_col.image;
    if (g_col.image_w < 60) g_col.image_w = 60;
}

// Single-line fit-with-ellipsis. gui_wrap_text_ttf() with max_lines == 1 is the
// shared ellipsizer (it measures with the real glyph metrics and appends a
// real-measured "..."), so this is not a private truncation rule.
static char g_fitbuf[1][GUI_WRAP_COL];
static const char *fit1(const char *sstr, int size, int maxw) {
    if (gui_wrap_text_ttf(sstr, size, maxw, 1, g_fitbuf) > 0) return g_fitbuf[0];
    return sstr;
}

static void draw_toolbar(void) {
    int w = content_w();
    win_draw_rect(win, 0, 0, w, TOOLBAR_H, C_CARD);
    win_draw_rect(win, 0, TOOLBAR_H - 1, w, 1, C_BORDER);

    diskimg_info_t *d = (g_sel < g_nrows) ? &g_row[g_sel] : 0;
    int can_mount = d && (d->flags & DISKIMG_F_MOUNTABLE);
    int can_eject = d && (d->flags & DISKIMG_F_MOUNTED);
    gui_button(win, PAD, 7, 96, BTN_H, "Mount...", GUI_BTN_PRIMARY,
               can_mount ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(win, PAD + 104, 7, BTN_W - 2, BTN_H, "Eject", GUI_BTN_SECONDARY,
               can_eject ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(win, PAD + 196, 7, BTN_W - 2, BTN_H, "Refresh", GUI_BTN_GHOST,
               GUI_ST_NORMAL);
}

static void draw_table(void) {
    int lx = PAD, ly = LIST_Y;
    int lw = content_w() - 2 * PAD;
    int lh = list_h();

    gui_list_config(&g_list, lx, ly, lw, lh, ROW_H, g_nrows);
    cols_compute(lx, gui_list_row_w(&g_list));

    // Column headers sit ABOVE the box, in type.caption, with a rule under
    // them: the same shape Files' details view uses.
    win_draw_text_small(win, g_col.drive, HDR_Y, "Drive",  C_DIM);
    win_draw_text_small(win, g_col.type,  HDR_Y, "Type",   C_DIM);
    win_draw_text_small(win, g_col.image, HDR_Y, "Image",  C_DIM);
    {
        int sw = gui_ttf_width("Size", TY_CAPTION);
        win_draw_text_small(win, g_col.size_r - sw, HDR_Y, "Size", C_DIM);
    }
    win_draw_text_small(win, g_col.fmt, HDR_Y, "Format", C_DIM);
    win_draw_rect(win, lx, HDR_Y + 16, lw, 1, C_BORDER);

    // Frame + track by hand because the rows are multi-column; the GEOMETRY and
    // the input are still gui_list's, which is the pattern gui_menu.c and
    // Settings both use for rows that carry more than one string.
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
        unsigned int fg = C_INK, dim = C_DIM_FIELD;   // rows sit on C_FIELD
        if (i == g_sel) {
            win_draw_rect(win, lx + 1, ry, rw, ROW_H, C_SEL);
            fg = C_SELTX; dim = C_SELTX;
        }
        int ty = ry + (ROW_H - TY_BODY) / 2;
        char letter[8];
        snprintf(letter, sizeof letter, "%c:", (char)('A' + d->letter));
        win_draw_text(win, g_col.drive, ty, letter, fg);
        win_draw_text(win, g_col.type,  ty, cls_name(d->cls), fg);

        if (d->flags & DISKIMG_F_MOUNTED) {
            win_draw_text(win, g_col.image, ty, fit1(d->name, TY_BODY, g_col.image_w), fg);
            char sz[24]; size_str(d->size, sz, sizeof sz);
            win_draw_text(win, g_col.size_r - gui_ttf_width(sz, TY_BODY), ty, sz, fg);
            char tail[48];
            snprintf(tail, sizeof tail, "%s%s%s",
                     fmt_name(d->fmt),
                     (d->flags & DISKIMG_F_JOLIET) ? " +Joliet" : "",
                     (d->flags & DISKIMG_F_INUSE) ? " (in use)" : "");
            win_draw_text(win, g_col.fmt, ty, fit1(tail, TY_BODY, 118), fg);
        } else if (d->cls == DISKIMG_CLASS_FIXED) {
            win_draw_text(win, g_col.image, ty, "(system disk)", dim);
        } else {
            win_draw_text(win, g_col.image, ty, "(empty)", dim);
        }
    }
}

static void draw_status(void) {
    int w = content_w(), y = CONTENT_H - STATUS_H;
    win_draw_rect(win, 0, y, w, STATUS_H, C_CARD);
    win_draw_rect(win, 0, y, w, 1, C_BORDER);
    int ty = y + (STATUS_H - TY_CAPTION) / 2;
    win_draw_text_small(win, PAD, ty, "M mount    E eject    R refresh", C_DIM_CARD);

    int n = 0;
    for (int i = 0; i < g_nrows; i++) if (g_row[i].flags & DISKIMG_F_MOUNTED) n++;
    char cnt[64];
    snprintf(cnt, sizeof cnt, "%d of %d images mounted", n, g_maxmounts);
    win_draw_text_small(win, w - PAD - gui_ttf_width(cnt, TY_CAPTION), ty, cnt, C_DIM_CARD);
}

// The picker is a real modal: an interlaced-scanline scrim over the parent (the
// same construction gui_confirm_render() uses, so a modal in this app and a
// modal in Files read as the same thing), and it closes ONLY on Mount, Cancel
// or Esc - never on a stray click outside it.
#define PK_W  520
#define PK_H  320
static int pk_x(void) { return (content_w() - PK_W) / 2; }
static int pk_y(void) { return (CONTENT_H - PK_H) / 2; }

static void draw_picker(void) {
    int x = pk_x(), y = pk_y();
    for (int sy = 0; sy < CONTENT_H; sy += 2)
        win_draw_rect(win, 0, sy, content_w(), 1, gui_mix(C_BG, 0x00000000, 108));

    gui_card(win, x, y, PK_W, PK_H);
    win_draw_text_ttf(win, x + 16, y + 14, "Choose a disk image", TY_TITLE, C_INK);
    win_draw_text_small(win, x + 16, y + 38,
                        ".ISO mounts as a CD-ROM.  .IMG and .IMA mount as a floppy.", C_DIM_CARD);

    // (#appstyle) THE PATH FIELD IS NEW, AND IT IS A CORRECTNESS FIX, NOT A
    // FLOURISH. This file's own header comment said "Typing a full path always
    // works regardless" - and until now it did not: the picker drew a list and
    // two buttons, there was no text entry anywhere in the GUI, and an image
    // outside /WINDIR, /IMAGES or / could only be mounted through the headless
    // `DISKIMG mount <path>` form. The sentence is now true. The field is the
    // SHARED textfield_t, so it arrives with caret, selection, the system
    // clipboard and undo rather than a hand-rolled character buffer.
    win_draw_text_small(win, x + 16, y + 62, "PATH", C_ACC);
    gui_textfield_tf(win, x + 16, y + 78, PK_W - 32, 26,
                     g_pathbuf, g_path.len, g_path.cursor, g_path.sel_anchor,
                     g_pkfocus == PKF_PATH, "/IMAGES/DISC.ISO");

    int ly = y + 118, lh = PK_H - 118 - 62;
    gui_list_config(&g_picklist, x + 16, ly, PK_W - 32, lh, ROW_H, g_npick);
    gui_list_draw(win, &g_picklist, g_picksel, C_FIELD, C_BORDER, C_INK,
                  C_SEL, C_SELTX, pick_label, 0);
    if (g_pkfocus == PKF_LIST) {
        // 2px, matching the field's own focus ring above (gui_textfield2 draws
        // a focused field's border in p->focus). A 1px ring beside a 2px one
        // reads as "this list is less focused", which is not a state.
        gui_draw_rect_outline(win, x + 14, ly - 2, PK_W - 28, lh + 4, C_ACC);
        gui_draw_rect_outline(win, x + 13, ly - 3, PK_W - 26, lh + 6, C_ACC);
    }

    char found[80];
    if (g_npick == 0)
        snprintf(found, sizeof found, "No .iso/.img files found in /WINDIR, /IMAGES or /");
    else
        snprintf(found, sizeof found, "%d found in /WINDIR, /IMAGES and /   -   Tab switches",
                 g_npick);
    win_draw_text_small(win, x + 16, ly + lh + 8, found, C_DIM_CARD);

    int by = y + PK_H - 16 - BTN_H;
    gui_button(win, x + PK_W - 16 - 96 - 8 - BTN_W, by, BTN_W, BTN_H, "Cancel",
               GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(win, x + PK_W - 16 - 96, by, 96, BTN_H, "Mount", GUI_BTN_PRIMARY,
               g_path.len ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    win_draw_text_small(win, x + 16, by + (BTN_H - TY_CAPTION) / 2,
                        "Enter mounts    Esc cancels", C_DIM_CARD);
}

static void draw(void) {
    win_draw_rect(win, 0, 0, content_w(), CONTENT_H, C_BG);
    draw_toolbar();
    win_draw_text_small(win, PAD, DESC_Y,
        "Mount a CD or floppy image so DOS and Windows 3.x programs can read it.", C_DIM);
    draw_table();
    draw_status();
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

// Keep the path field and the list selection in step: selecting a row fills the
// field, so the common case (mount one of the images we found) still needs no
// typing, and the uncommon case (an image somewhere else) is a matter of
// editing what is already there rather than knowing the syntax.
static void pk_sync_path_from_sel(void) {
    if (g_picksel >= 0 && g_picksel < g_npick) tf_set_text(&g_path, g_pick[g_picksel]);
}

static void open_picker(void) {
    scan_dirs();
    tf_init(&g_path, g_pathbuf, sizeof g_pathbuf);
    pk_sync_path_from_sel();
    g_pkfocus = g_npick ? PKF_LIST : PKF_PATH;
    g_picking = 1;
}

// The ONE place a mount is started from the picker, so the button, Enter in the
// list and Enter in the field cannot drift apart.
static void pk_commit(void) {
    if (g_path.len == 0) return;
    char chosen[PATHW];
    strncpy(chosen, g_pathbuf, PATHW - 1);
    chosen[PATHW - 1] = 0;
    g_picking = 0;
    do_mount(chosen, DISKIMG_LETTER_AUTO);
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
    sync_content_size(win);      // #184: ask, do not assume (see CONTENT_H)
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
            sync_content_size(win);   // #184: a resize changes what may be drawn
            draw(); break;
        case EVENT_WINDOW_CLOSE:
            running = 0; break;

        case EVENT_KEY_DOWN: {
            char c = ev.key_char;
            uint32_t kc = ev.keycode;
            if (g_picking) {
                if (c == 27) { g_picking = 0; draw(); break; }
                if (c == '\t') { g_pkfocus = (g_pkfocus == PKF_PATH) ? PKF_LIST : PKF_PATH;
                                 draw(); break; }
                if (c == '\n' || c == '\r') { pk_commit(); draw(); break; }
                // Up/Down always drive the list, from either focus: they are the
                // only thing a single-line field does not want them for, and a
                // user who has just typed a path still expects the arrows to
                // browse.
                if (kc == GUI_KEY_UP || kc == GUI_KEY_DOWN) {
                    gui_list_move_sel(&g_picklist, &g_picksel, kc == GUI_KEY_UP ? -1 : 1);
                    pk_sync_path_from_sel();
                    draw(); break;
                }
                if (g_pkfocus == PKF_PATH) {
                    if (tf_handle_key(&g_path, &ev)) draw();
                    break;
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
                int px = pk_x(), py = pk_y();
                int by = py + PK_H - 16 - BTN_H;
                if (ly >= by && ly < by + BTN_H) {
                    if (lx >= px + PK_W - 16 - 96 - 8 - BTN_W && lx < px + PK_W - 16 - 96)
                        g_picking = 0;                       // Cancel
                    else if (lx >= px + PK_W - 16 - 96 && lx < px + PK_W - 16)
                        pk_commit();                         // Mount
                    draw(); break;
                }
                if (ly >= py + 78 && ly < py + 78 + 26 &&
                    lx >= px + 16 && lx < px + PK_W - 16) {
                    g_pkfocus = PKF_PATH;
                    // Caret to the clicked glyph, not to the end: a click that
                    // ignores where it landed is the cursor-hostile behaviour
                    // this project has been bitten by before. Measured with
                    // gui_ttf_render_width() because that is the width function
                    // that agrees with the renderer drawing the text.
                    int rel = lx - (px + 16 + 8), best = 0;
                    for (int i = 0; i <= g_path.len; i++) {
                        char pre[PATHW];
                        int n = i < PATHW - 1 ? i : PATHW - 1;
                        for (int k = 0; k < n; k++) pre[k] = g_pathbuf[k];
                        pre[n] = 0;
                        if (gui_ttf_render_width(pre, TY_BODY) <= rel) best = i; else break;
                    }
                    tf_set_caret(&g_path, best);
                    draw(); break;
                }
                int hit = gui_list_press(&g_picklist, lx, ly);
                if (hit >= 0) { g_picksel = hit; g_pkfocus = PKF_LIST; pk_sync_path_from_sel(); }
                draw(); break;
            }
            if (ly >= 7 && ly < 7 + BTN_H) {
                if (lx >= PAD && lx < PAD + 96) {
                    if (g_sel < g_nrows && (g_row[g_sel].flags & DISKIMG_F_MOUNTABLE))
                        open_picker();
                } else if (lx >= PAD + 104 && lx < PAD + 104 + BTN_W - 2) {
                    if (g_sel < g_nrows && (g_row[g_sel].flags & DISKIMG_F_MOUNTED))
                        do_eject((int)g_row[g_sel].letter);
                } else if (lx >= PAD + 196 && lx < PAD + 196 + BTN_W - 2) {
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
