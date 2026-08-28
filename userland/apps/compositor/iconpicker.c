// iconpicker.c - #44 "Change Icon" file picker (custom dock/menu icons).
//
// Lists PNG/JPG/BMP files under the session user's home directory (a flat,
// non-recursive scan - the user drops or copies a source image there with
// Files first, the same discoverability model wallpaper_load()'s existing
// picker already uses for BMPs under "/") and imports the selected one via
// icon_import_and_apply() (icons.c) when clicked.
//
// A TRUE MODAL: dims the desktop and swallows every click while open, closes
// only via its own Cancel button or ESC - same idiom as
// startmenu_power_confirm_render()/handle_mouse() ("true modal: swallow
// every other click while open"), not the lightweight click-away popups
// contextmenu.c uses for menus. This has real state (a file list, a
// selection) worth protecting, which is exactly the distinction the project
// style guide draws between a menu and a dialog.
//
// The directory scan and the import itself both run from an input-tick
// action (open, or a row click), never from the draw/render path (#426):
// sys_open()+sys_readdir_raw() here is the SAME pattern desktop.c's
// desktop_rescan_home() already uses for the same reason.

#include "compositor.h"
#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/userconf.h"   // userhome_root() - home dir with no pwd.h/bool conflict

#define ICONPICK_MAX_ENTRIES 24   // a COUNT, not a pixel size - not scaled
// #uiscale: scaled at the definition (see compositor.h's block comment for
// the rationale). No compile-time-context use found (grepped for array size/
// static init).
#define ICONPICK_W           ui_px(340)
#define ICONPICK_TITLE_H     ui_px(26)
#define ICONPICK_ROW_H       ui_px(22)
#define ICONPICK_BTN_H       ui_px(28)

static bool      g_iconpick_open;
static char      g_iconpick_target_path[128];
static icon_id_t g_iconpick_target_icon;
static char      g_iconpick_entries[ICONPICK_MAX_ENTRIES][48];
static int       g_iconpick_count;
static int       g_iconpick_hover;   // row index, or -1
static char      g_iconpick_status[80];   // "" normally; set on import failure

bool iconpicker_is_open(void) { return g_iconpick_open; }

// Case-insensitive "does name end with one of the supported extensions".
// CHECK WHAT SYS_DECODE_IMAGE ACTUALLY SUPPORTS: PNG, JPEG (via header
// auto-detect in kernel/gui/image.c's image_load()) and BMP - but ONLY
// 24/32bpp BMP (bmp_decode_c() hard-rejects 8-bit indexed BMP, see icons.c's
// icon_import_and_apply() comment). Listing a .BMP here that turns out to be
// indexed still fails cleanly at import time (icon_import_and_apply()
// returns -1, g_iconpick_status reports it) rather than silently, which is
// the best this picker can do without decoding every candidate just to list
// it.
static int has_image_ext(const char *name) {
    int len = 0;
    while (name[len]) len++;
    static const char *exts[] = { ".PNG", ".JPG", ".JPEG", ".BMP" };
    for (int e = 0; e < 4; e++) {
        int elen = 0;
        while (exts[e][elen]) elen++;
        if (len < elen) continue;
        int match = 1;
        for (int i = 0; i < elen; i++) {
            char a = name[len - elen + i], b = exts[e][i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (a != b) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

static void iconpick_rescan(void) {
    g_iconpick_count = 0;
    char home[128];
    if (userhome_root(home, sizeof(home)) != 0) return;
    int fd = sys_open(home, 0);
    if (fd < 0) return;
    dirent_t de;
    while (g_iconpick_count < ICONPICK_MAX_ENTRIES && sys_readdir_raw(fd, &de) == 0) {
        de.name[sizeof(de.name) - 1] = '\0';
        if (de.name[0] == '\0' || de.name[0] == '.') continue;
        if (DIRENT_IS_DIR(de)) continue;
        if (!has_image_ext(de.name)) continue;
        strncpy(g_iconpick_entries[g_iconpick_count], de.name, sizeof(g_iconpick_entries[0]) - 1);
        g_iconpick_entries[g_iconpick_count][sizeof(g_iconpick_entries[0]) - 1] = '\0';
        g_iconpick_count++;
    }
    sys_close(fd);
}

void iconpicker_open(const char *exec_path, icon_id_t icon_id) {
    if (!exec_path || !exec_path[0]) return;   // no target: nothing to change
    strncpy(g_iconpick_target_path, exec_path, sizeof(g_iconpick_target_path) - 1);
    g_iconpick_target_path[sizeof(g_iconpick_target_path) - 1] = '\0';
    g_iconpick_target_icon = icon_id;
    g_iconpick_status[0]   = '\0';
    g_iconpick_hover       = -1;
    iconpick_rescan();
    g_iconpick_open   = true;
    g_needs_redraw    = true;
}

void iconpicker_close(void) {
    g_iconpick_open = false;
    g_needs_redraw  = true;
}

static int32_t iconpick_panel_h(void) {
    int rows = g_iconpick_count > 0 ? g_iconpick_count : 1;   // 1 row for the "none found" message
    return ICONPICK_TITLE_H + rows * ICONPICK_ROW_H + ICONPICK_BTN_H + 24;
}

static void iconpick_rect(int32_t *px, int32_t *py, int32_t *pw, int32_t *ph) {
    *pw = ICONPICK_W;
    *ph = iconpick_panel_h();
    if (*ph > g_fb_height - 40) *ph = g_fb_height - 40;
    *px = (g_fb_width  - *pw) / 2;
    *py = (g_fb_height - *ph) / 2;
}

// #uiscale hit-test fix: list_y/list_h and the Cancel button rect were each
// duplicated (identical formula, but a duplicate all the same - the same
// pattern that DOES drift elsewhere in this audit whenever only one copy
// gets touched later) between iconpicker_render() and
// iconpicker_handle_mouse(). Shared now.
static void iconpick_list_rect(int32_t px, int32_t py, int32_t pw, int32_t ph,
                               int32_t *list_y, int32_t *list_h) {
    (void)px; (void)pw;
    *list_y = py + ICONPICK_TITLE_H + ui_px(4);
    *list_h = ph - ICONPICK_TITLE_H - ICONPICK_BTN_H - ui_px(24);
}
static void iconpick_cancel_rect(int32_t px, int32_t py, int32_t pw, int32_t ph,
                                 int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bw = ui_px(90);
    *bh = ICONPICK_BTN_H - ui_px(6);
    *bx = px + pw - *bw - ui_px(12);
    *by = py + ph - *bh - ui_px(10);
}

void iconpicker_render(void) {
    if (!g_iconpick_open) return;
    int32_t px, py, pw, ph;
    iconpick_rect(&px, &py, &pw, &ph);

    // Dim the desktop behind the dialog - same technique launcher.c/the power
    // confirm dialog use, so this reads as the same class of surface.
    g_draw_blend = 130;
    draw_fill_rect(0, 0, g_fb_width, g_fb_height, 0xFF0B0D12);
    g_draw_blend = 255;

    draw_fill_rect(px + 3, py + 3, pw, ph, CLR_MENU_SHADOW);
    draw_fill_rect(px, py, pw, ph, CLR_MENU_BG);
    draw_rect_outline(px, py, pw, ph, CLR_MENU_BORDER);

    draw_text(px + 12, py + (ICONPICK_TITLE_H - FONT_CHAR_H) / 2, "Change Icon", CLR_MENU_TEXT);
    draw_hline(px, py + ICONPICK_TITLE_H, pw, CLR_MENU_BORDER);

    int32_t list_y, list_h;
    iconpick_list_rect(px, py, pw, ph, &list_y, &list_h);   // #uiscale: shared with the hit-test
    int rows_vis = list_h / ICONPICK_ROW_H;
    if (rows_vis < 1) rows_vis = 1;

    if (g_iconpick_count == 0) {
        char msg[100];
        int n = 0;
        const char *m1 = "No PNG/JPG/BMP files in your home folder.";
        while (m1[n] && n < 99) { msg[n] = m1[n]; n++; }
        msg[n] = '\0';
        draw_text(px + 12, list_y + 4, msg, readable_ink_dim(CLR_MENU_BG));
        draw_text(px + 12, list_y + 4 + FONT_CHAR_H + 4,
                  "Copy one there with Files, then try again.", readable_ink_dim(CLR_MENU_BG));
    } else {
        int shown = g_iconpick_count < rows_vis ? g_iconpick_count : rows_vis;
        for (int i = 0; i < shown; i++) {
            int32_t ry = list_y + i * ICONPICK_ROW_H;
            if (i == g_iconpick_hover) {
                draw_fill_rect(px + 4, ry, pw - 8, ICONPICK_ROW_H, CLR_MENU_ITEM_HOVER);
            }
            uint32_t ink = readable_ink(i == g_iconpick_hover ? CLR_MENU_ITEM_HOVER : CLR_MENU_BG);
            draw_text(px + 12, ry + (ICONPICK_ROW_H - FONT_CHAR_H) / 2, g_iconpick_entries[i], ink);
        }
    }

    if (g_iconpick_status[0]) {
        draw_text(px + 12, py + ph - ICONPICK_BTN_H - 16, g_iconpick_status, 0xFFCC4040);
    }

    // Cancel button, bottom-right.
    int32_t bx, by, bw, bh;
    iconpick_cancel_rect(px, py, pw, ph, &bx, &by, &bw, &bh);   // #uiscale: shared with the hit-test
    draw_fill_rect(bx, by, bw, bh, CLR_MENU_ITEM_NORM);
    draw_rect_outline(bx, by, bw, bh, CLR_MENU_BORDER);
    draw_text_centered(bx + bw / 2, by + (bh - FONT_CHAR_H) / 2, "Cancel", CLR_MENU_TEXT);
}

bool iconpicker_handle_mouse(int32_t x, int32_t y, bool clicked) {
    if (!g_iconpick_open) return false;
    int32_t px, py, pw, ph;
    iconpick_rect(&px, &py, &pw, &ph);

    int32_t bx, by, bw, bh;
    iconpick_cancel_rect(px, py, pw, ph, &bx, &by, &bw, &bh);   // #uiscale: shared with the draw side
    if (clicked && x >= bx && x < bx + bw && y >= by && y < by + bh) {
        iconpicker_close();
        return true;
    }

    int32_t list_y, list_h;
    iconpick_list_rect(px, py, pw, ph, &list_y, &list_h);   // #uiscale: shared with the draw side
    int rows_vis = list_h / ICONPICK_ROW_H;
    if (rows_vis < 1) rows_vis = 1;
    int shown = g_iconpick_count < rows_vis ? g_iconpick_count : rows_vis;

    int hover = -1;
    if (x >= px && x < px + pw && y >= list_y && y < list_y + shown * ICONPICK_ROW_H) {
        int row = (int)((y - list_y) / ICONPICK_ROW_H);
        if (row >= 0 && row < shown) hover = row;
    }
    if (hover != g_iconpick_hover) { g_iconpick_hover = hover; g_needs_redraw = true; }

    if (clicked && hover >= 0) {
        char src[192];
        char home[128];
        if (userhome_root(home, sizeof(home)) == 0) {
            int L = 0;
            while (home[L] && L < 180) { src[L] = home[L]; L++; }
            if (L == 0 || src[L - 1] != '/') src[L++] = '/';
            const char *fname = g_iconpick_entries[hover];
            int j = 0;
            while (fname[j] && L < 190) src[L++] = fname[j++];
            src[L] = '\0';

            char base[40];
            icon_mico_basename(g_iconpick_target_path, base, sizeof(base));
            char out[192];
            if (base[0] && userhome_path("ICONS", base, out, sizeof(out)) == 0) {
                // Ensure "<home>/ICONS" exists before writing into it.
                // sys_mkdir() on an already-existing directory just fails
                // harmlessly - not checked, matching userconf_open_write()'s
                // own "creating <home>/CONFIG if needed" idiom for the same
                // shape of problem.
                char icondir[160];
                int dl = 0;
                while (home[dl] && dl < 150) { icondir[dl] = home[dl]; dl++; }
                if (dl == 0 || icondir[dl - 1] != '/') icondir[dl++] = '/';
                icondir[dl++] = 'I'; icondir[dl++] = 'C'; icondir[dl++] = 'O';
                icondir[dl++] = 'N'; icondir[dl++] = 'S'; icondir[dl] = '\0';
                sys_mkdir(icondir, 0755);
                int rc = icon_import_and_apply(g_iconpick_target_icon, src, out);
                if (rc == 0) {
                    iconpicker_close();
                } else {
                    int n = 0;
                    const char *m = "Could not import that image (unsupported format?).";
                    while (m[n] && n < 79) { g_iconpick_status[n] = m[n]; n++; }
                    g_iconpick_status[n] = '\0';
                    g_needs_redraw = true;
                }
            }
        }
        return true;
    }

    // True modal: swallow every other click while open (same idiom as
    // startmenu_power_confirm_handle_mouse()).
    return true;
}

int iconpicker_handle_key(int key) {
    if (!g_iconpick_open) return 0;
    if (key == 27 /* ESC */) { iconpicker_close(); return 1; }
    return 1;   // swallow all other keys while open
}
