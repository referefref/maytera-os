// main.c - MayteraOS "Install to Disk" (#306)
//
// Built strictly from the per-element spec tables in
// docs/INSTALLER_UI_DESIGN.html (five 640x480 screens: intro, disk
// selection, destructive confirmation, progress, done/failure). Every color,
// coordinate and behavior below traces to a row in one of that document's
// tables; nothing here is invented chrome.
//
// ENGINE REALITY NOTE (read before touching the progress screen). The kernel
// exposes exactly two syscalls for this: SYS_INST_ENUM (365, non-destructive
// enumeration) and SYS_INST_INSTALL (366, DESTRUCTIVE, root-only). The design
// spec's progress screen assumes a live percent/stage feed, but
// installer_do_install_target() in kernel/gui/installer.c reports progress
// ONLY to kprintf (kernel serial log) - there is no syscall, shared memory,
// or callback path back to Ring 3 while the call is in flight, and it BLOCKS
// the calling thread until the clone finishes. So: a background pthread
// makes the single blocking inst_install() call while the main GUI thread
// keeps pumping window events, and the percent/stage text shown is a
// time-estimated approximation (scaled off the target's sector count),
// capped at 95% until the real syscall returns, at which point we go
// straight to the real success/failure result. It never claims 100%/
// "Complete." before the engine has genuinely finished, and it never gets
// stuck: the one number that matters (did the install actually succeed) is
// always the syscall's real return code, never guessed.
//
// NOCHROME: this window carries no compositor decoration (win_set_nochrome);
// the app draws its own 20px titlebar and close box. See docs/UI_STYLE_GUIDE.md
// and userland/apps/aichat|musicplayer for the established NOCHROME idiom.

#include "maytera.h"
#include "gui.h"
#include "pthread.h"

// ---------------------------------------------------------------------------
// Geometry (fixed 640x480, never resizes; see spec section 11)
// ---------------------------------------------------------------------------
#define WIN_W 640
#define WIN_H 480

// ---------------------------------------------------------------------------
// Colors - literal hex from spec section 1. This app intentionally does NOT
// follow the live system theme: it is a destructive, security-relevant
// dialog and the spec's own callout is explicit that it inherits the
// Settings/Files retro-unix chrome language regardless of what theme is
// active elsewhere, for predictability. Every value below is transcribed
// from the design doc's color table, not derived from gui_pal()/theme.h.
// ---------------------------------------------------------------------------
#define C_BASE_BG        0x00AEB2C3
#define C_BASE_FG        0x00000000
#define C_ACCENT         0x004B6983
#define C_ACCENT_SEC     0x008B8682
#define C_TITLE_TEXT     0x00FFFFFF
#define C_BORDER_LIGHT   0x00DCDAD5
#define C_BORDER_DARK    0x00565248
#define C_BORDER_OUTLINE 0x00000000
#define C_BTN_BG         0x00C0C0C0
#define C_BTN_HOVER      0x00D0D0D0
#define C_BTN_PRESSED    0x00A0A0A0
#define C_BTN_HI         0x00FFFFFF
#define C_BTN_SH         0x00808080
#define C_BTN_OUTER_SH   0x00404040
#define C_INPUT_BG       0x00FFFFFF
#define C_SEL_BG         0x004B6983
#define C_SEL_FG         0x00FFFFFF
#define C_ERROR          0x00CC0000
#define C_SUCCESS        0x004E9A06
#define C_DISABLED_FG    0x00808080
#define C_DISABLED_BG    0x00D4D4D4
#define C_ROW_HOVER      0x00ECECEC
#define C_DANGER_HI      0x00E33B2E
#define C_DANGER_SH      0x008C0000

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------
enum { SCR_INTRO = 0, SCR_DISKS, SCR_CONFIRM, SCR_PROGRESS, SCR_DONE };

// Minimum install size floor used ONLY to decide whether a disk is shown as
// "too small" in the UI (a UI-side pre-filter, matching the spec's "under the
// minimum size is not offered as selectable but is still shown"). It is
// deliberately conservative; the AUTHORITATIVE check is the kernel's own
// ESP+ext2-root arithmetic inside installer_do_install_target(), which
// returns -6 ("too small") if a disk that passed this floor still does not
// fit. Two independent checks, same as the kernel's own comment about its
// two floors, kept separate on purpose.
#define MIN_INSTALL_SECTORS (4ULL * 1024 * 1024 * 1024 / 512)   // 4 GiB

// ---------------------------------------------------------------------------
// Disk row model (derived from inst_target_t at enumeration time)
// ---------------------------------------------------------------------------
typedef struct {
    inst_target_t t;
    char   name[40];       // synthesized bus label, e.g. "AHCI disk 0" - also
                            // the exact string typed to confirm on screen 3
    char   subtitle[64];    // ATA IDENTIFY model, or "<BUS> - <capacity>"
    char   capstr[16];      // "500.1 GB"
    int    have_model;      // subtitle is a real IDENTIFY model, not a bus/capacity fallback
    int    is_boot;
    int    too_small;
    int    selectable;
} disk_row_t;

static disk_row_t g_rows[INST_MAX_TARGETS];
static int        g_nrows = 0;
static int        g_selected = -1;   // index into g_rows, or -1

static int g_win = -1;
static int g_screen = SCR_INTRO;
static int g_running = 1;

// Per-screen keyboard focus (small int, meaning is screen-specific; see the
// focus tables in handle_key()/handle_click()).
static int g_focus = 0;
static int g_hover_row = -1;   // hovered disk-list row (mouse), or -1

// Screen 2 list scroll (first visible row index), for the rare >4-disk case.
static int g_list_scroll = 0;
#define ROW_H       68
#define LIST_X      8
#define LIST_Y      112
#define LIST_W      624
#define LIST_H      280
#define ROW_X       12
#define ROW_W       616
#define LIST_VISIBLE_ROWS (LIST_H / ROW_H)   // 4

// Screen 3 confirm field
static textfield_t g_tf;
static char        g_tf_buf[64];
static int         g_tf_match = 0;

// Screen 4/5 install state
typedef struct { uint8_t kind; uint8_t index; } install_args_t;
static install_args_t   g_install_args;
static volatile int     g_install_rc = -12345;   // sentinel: not finished
static unsigned long    g_install_start_ms = 0;
static unsigned long    g_install_done_ms = 0;
static int              g_progress_pct = 1;
static char             g_progress_msg[64] = "Writing partition table... (1 of 5)";
static int              g_done_success = 0;
static char             g_done_errmsg[96];
static int              g_confirm_row = -1;      // which row we are installing

// ---------------------------------------------------------------------------
// Small formatting helpers
// ---------------------------------------------------------------------------
static void commafmt_u64(unsigned long long n, char *out, int cap) {
    char digits[24]; int nd = 0;
    if (n == 0) { digits[nd++] = '0'; }
    while (n > 0 && nd < 24) { digits[nd++] = (char)('0' + (n % 10)); n /= 10; }
    int oi = 0;
    for (int i = nd - 1; i >= 0; i--) {
        if (oi < cap - 1) out[oi++] = digits[i];
        int from_end = i; // digits before this one still to emit
        if (from_end > 0 && (from_end % 3) == 0 && oi < cap - 1) out[oi++] = ',';
    }
    out[oi] = '\0';
}

static void format_gb(unsigned long long sectors, char *out, int cap) {
    double gb = (double)sectors * 512.0 / 1000000000.0;
    int whole = (int)gb;
    int tenth = (int)((gb - (double)whole) * 10.0 + 0.5);
    if (tenth >= 10) { tenth = 0; whole++; }
    snprintf(out, cap, "%d.%d GB", whole, tenth);
}

static const char *bus_word(int kind) {
    if (kind == INST_KIND_ATA)  return "ATA";
    if (kind == INST_KIND_AHCI) return "AHCI";
    if (kind == INST_KIND_USB)  return "USB";
    return "disk";
}

// Greedy word-wrap into caller-supplied line buffers, measured with the real
// TTF metrics (gui_ttf_width), so layout holds even if wording changes.
#define WRAP_LINE_MAX 160
static int wrap_text(const char *s, int maxw, int size, char lines[][WRAP_LINE_MAX], int maxlines) {
    int nlines = 0;
    int i = 0, n = (int)strlen(s);
    while (i < n && nlines < maxlines) {
        int start = i, last_space = -1, len = 0;
        char buf[WRAP_LINE_MAX]; buf[0] = '\0';
        while (i < n) {
            int j = i;
            while (j < n && s[j] != ' ') j++;
            int wordlen = j - start >= WRAP_LINE_MAX ? WRAP_LINE_MAX - 1 : j - start;
            char trial[WRAP_LINE_MAX];
            int tlen = j - start; if (tlen > WRAP_LINE_MAX - 1) tlen = WRAP_LINE_MAX - 1;
            memcpy(trial, s + start, (size_t)tlen); trial[tlen] = '\0';
            (void)wordlen;
            if (gui_ttf_width(trial, size) > maxw && len > 0) break;
            memcpy(buf, s + start, (size_t)tlen); buf[tlen] = '\0';
            len = tlen;
            if (j < n && s[j] == ' ') { last_space = j; i = j + 1; }
            else { i = j; break; }
            (void)last_space;
        }
        strncpy(lines[nlines], buf, WRAP_LINE_MAX - 1); lines[nlines][WRAP_LINE_MAX - 1] = '\0';
        nlines++;
        start = start; // silence unused in some paths
    }
    return nlines;
}

// ---------------------------------------------------------------------------
// Text drawing helpers (face 0 = system default DejaVu Sans; the kernel
// renderer resolves FONT_STYLE_BOLD to the real enrolled DejaVu Sans Bold
// face rather than synthetic emboldening - see userland/apps/settings/main.c
// draw_section_header()'s comment, and spec section 2's "real Bold face").
// ---------------------------------------------------------------------------
static void ttext(int x, int y, const char *s, uint32_t color, int size, int bold) {
    win_draw_text_ttf_ex(g_win, x, y, s, 0, size, bold ? FONT_STYLE_BOLD : 0, color);
}
static void ttext_centered(int x, int y, int w, const char *s, uint32_t color, int size, int bold) {
    int tw = gui_ttf_width(s, size);
    int tx = x + (w - tw) / 2; if (tx < x) tx = x;
    win_draw_text_ttf_ex(g_win, tx, y, s, 0, size, bold ? FONT_STYLE_BOLD : 0, color);
}
static void ttext_right(int x, int y, int w, const char *s, uint32_t color, int size, int bold) {
    int tw = gui_ttf_width(s, size);
    int tx = x + w - tw; if (tx < x) tx = x;
    win_draw_text_ttf_ex(g_win, tx, y, s, 0, size, bold ? FONT_STYLE_BOLD : 0, color);
}

// ---------------------------------------------------------------------------
// Bevel primitives
// ---------------------------------------------------------------------------
static void sunken(int x, int y, int w, int h, uint32_t fill, uint32_t dark, uint32_t light) {
    win_draw_rect(g_win, x, y, w, h, fill);
    win_draw_rect(g_win, x, y, w, 1, dark);          // top
    win_draw_rect(g_win, x, y, 1, h, dark);          // left
    win_draw_rect(g_win, x, y + h - 1, w, 1, light); // bottom
    win_draw_rect(g_win, x + w - 1, y, 1, h, light); // right
}
#define sunken_panel(x,y,w,h) sunken((x),(y),(w),(h), C_BTN_BG,  C_BORDER_DARK, C_BORDER_LIGHT)
#define sunken_white(x,y,w,h) sunken((x),(y),(w),(h), C_INPUT_BG, C_BTN_SH,     C_INPUT_BG)

enum { BTN_NORMAL = 0, BTN_HOVER, BTN_PRESSED, BTN_DISABLED };

// Motif push button, style guide 6.1 / spec section 4.1.
static void draw_button(int x, int y, int w, int h, const char *label, int state,
                         int is_default, int is_danger, int has_focus) {
    uint32_t fill, hi, sh, text_color;
    if (state == BTN_DISABLED) {
        fill = is_danger ? C_DISABLED_BG : C_BTN_BG;
        win_draw_rect(g_win, x, y, w, h, fill);
        win_draw_rect(g_win, x, y, w, 1, C_DISABLED_FG);
        win_draw_rect(g_win, x, y, 1, h, C_DISABLED_FG);
        win_draw_rect(g_win, x, y + h - 1, w, 1, C_DISABLED_FG);
        win_draw_rect(g_win, x + w - 1, y, 1, h, C_DISABLED_FG);
        ttext_centered(x, y + (h - 14) / 2, w, label, C_DISABLED_FG, 14, is_danger);
        return;
    }
    if (is_danger) { fill = C_ERROR; hi = C_DANGER_HI; sh = C_DANGER_SH; text_color = C_SEL_FG; }
    else {
        fill = (state == BTN_HOVER) ? C_BTN_HOVER : (state == BTN_PRESSED) ? C_BTN_PRESSED : C_BTN_BG;
        hi = C_BTN_HI; sh = C_BTN_SH; text_color = C_BASE_FG;
    }
    win_draw_rect(g_win, x, y, w, h, fill);
    if (state == BTN_PRESSED) {
        win_draw_rect(g_win, x, y, w, 1, sh);
        win_draw_rect(g_win, x, y, 1, h, sh);
        win_draw_rect(g_win, x, y + h - 1, w, 1, hi);
        win_draw_rect(g_win, x + w - 1, y, 1, h, hi);
    } else {
        win_draw_rect(g_win, x, y, w, 1, hi);
        win_draw_rect(g_win, x, y, 1, h, hi);
        win_draw_rect(g_win, x, y + h - 1, w, 1, sh);
        win_draw_rect(g_win, x + w - 1, y, 1, h, sh);
        win_draw_rect(g_win, x + w, y + 1, 1, h, C_BTN_OUTER_SH);
        win_draw_rect(g_win, x + 1, y + h, w, 1, C_BTN_OUTER_SH);
    }
    ttext_centered(x, y + (h - 14) / 2, w, label, text_color, 14, is_danger);
    if (is_default) {
        win_draw_rect(g_win, x - 2, y - 2, w + 4, 1, C_ACCENT);
        win_draw_rect(g_win, x - 2, y + h + 1, w + 4, 1, C_ACCENT);
        win_draw_rect(g_win, x - 2, y - 2, 1, h + 4, C_ACCENT);
        win_draw_rect(g_win, x + w + 1, y - 2, 1, h + 4, C_ACCENT);
    }
    if (has_focus) {
        // 1px dotted focus ring, inset 3px. No dotted-line primitive exists,
        // so this is a dashed rect of 1px-on/1px-off segments (spec 4.1's own
        // fallback instruction), never a solid ring (would look like the
        // default-action marker above).
        int fx = x + 3, fy = y + 3, fw = w - 6, fh = h - 6;
        for (int i = 0; i < fw; i += 2) win_draw_rect(g_win, fx + i, fy, 1, 1, C_BASE_FG);
        for (int i = 0; i < fw; i += 2) win_draw_rect(g_win, fx + i, fy + fh - 1, 1, 1, C_BASE_FG);
        for (int i = 0; i < fh; i += 2) win_draw_rect(g_win, fx, fy + i, 1, 1, C_BASE_FG);
        for (int i = 0; i < fh; i += 2) win_draw_rect(g_win, fx + fw - 1, fy + i, 1, 1, C_BASE_FG);
    }
}

static int pt_in(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

// ---------------------------------------------------------------------------
// Chrome (self-drawn titlebar + close box; every screen)
// ---------------------------------------------------------------------------
static void draw_chrome(int closebox_enabled) {
    win_draw_rect(g_win, 0, 0, WIN_W, WIN_H, C_BASE_BG);
    win_draw_rect(g_win, 0, 0, WIN_W, 20, C_ACCENT);
    ttext(8, 2, "Install to Disk", C_TITLE_TEXT, 16, 0);
    if (closebox_enabled) {
        win_draw_rect(g_win, 616, 2, 16, 16, C_BTN_BG);
        win_draw_rect(g_win, 616, 2, 16, 1, C_BTN_HI);
        win_draw_rect(g_win, 616, 2, 1, 16, C_BTN_HI);
        win_draw_rect(g_win, 616, 17, 16, 1, C_BTN_SH);
        win_draw_rect(g_win, 631, 2, 1, 16, C_BTN_SH);
        ttext_centered(616, 5, 16, "X", C_BASE_FG, 10, 1);
    } else {
        win_draw_rect(g_win, 616, 2, 16, 16, C_DISABLED_BG);
        win_draw_rect(g_win, 616, 2, 16, 1, C_DISABLED_FG);
        win_draw_rect(g_win, 616, 2, 1, 16, C_DISABLED_FG);
        win_draw_rect(g_win, 616, 17, 16, 1, C_DISABLED_FG);
        win_draw_rect(g_win, 631, 2, 1, 16, C_DISABLED_FG);
        ttext_centered(616, 5, 16, "X", C_DISABLED_FG, 10, 1);
    }
    win_draw_rect(g_win, 0, 20, WIN_W, 1, C_BORDER_OUTLINE);
    win_draw_rect(g_win, 8, 436, 624, 1, C_BORDER_DARK);
    win_draw_rect(g_win, 8, 437, 624, 1, C_BORDER_LIGHT);
}

static int closebox_hit(int mx, int my) { return pt_in(mx, my, 616, 2, 16, 16); }

// ---------------------------------------------------------------------------
// Disk enumeration + classification (section 3, 6.1)
// ---------------------------------------------------------------------------
static void classify_row(disk_row_t *r) {
    inst_target_t *t = &r->t;
    if (t->kind == INST_KIND_ATA)  snprintf(r->name, sizeof(r->name), "ATA channel %d", t->index);
    else if (t->kind == INST_KIND_AHCI) snprintf(r->name, sizeof(r->name), "AHCI disk %d", t->index);
    else if (t->kind == INST_KIND_USB)  snprintf(r->name, sizeof(r->name), "USB disk %d", t->index);
    else snprintf(r->name, sizeof(r->name), "disk %d", t->index);

    format_gb(t->sectors, r->capstr, sizeof(r->capstr));

    int have_model = 0;
    if (t->kind == INST_KIND_ATA && t->index <= 3) {
        disk_info_t di;
        if (get_disk_info((int)t->index, &di) == 0 && di.present && di.type == 0 && di.model[0]) {
            // Trim trailing spaces (IDENTIFY model strings are space-padded).
            char tmp[41]; int n = 0;
            for (; n < 40 && di.model[n]; n++) tmp[n] = di.model[n];
            while (n > 0 && tmp[n - 1] == ' ') n--;
            tmp[n] = '\0';
            if (n > 0) { strncpy(r->subtitle, tmp, sizeof(r->subtitle) - 1); r->subtitle[sizeof(r->subtitle)-1]=0; have_model = 1; }
        }
    }
    if (!have_model) snprintf(r->subtitle, sizeof(r->subtitle), "%s - %s", bus_word(t->kind), r->capstr);
    r->have_model = have_model;

    r->is_boot   = t->is_boot ? 1 : 0;
    r->too_small = (t->sectors < MIN_INSTALL_SECTORS) ? 1 : 0;
    r->selectable = !r->is_boot && !r->too_small;
}

static void enumerate_disks(void) {
    inst_target_t raw[INST_MAX_TARGETS];
    int n = inst_enum(raw, INST_MAX_TARGETS);
    g_nrows = 0;
    if (n < 0) n = 0;
    for (int i = 0; i < n && i < INST_MAX_TARGETS; i++) {
        g_rows[g_nrows].t = raw[i];
        classify_row(&g_rows[g_nrows]);
        g_nrows++;
    }
    g_selected = -1;
    g_list_scroll = 0;
}

// ---------------------------------------------------------------------------
// Screen 1: Intro
// ---------------------------------------------------------------------------
static void draw_intro(void) {
    draw_chrome(1);
    ttext(8, 28, "Install MayteraOS to Disk", C_BASE_FG, 20, 1);

    char lines[4][WRAP_LINE_MAX];
    int nl = wrap_text("This installs MayteraOS onto an internal disk so this computer can start "
                        "MayteraOS on its own, without the USB stick.", 624, 14, lines, 4);
    for (int i = 0; i < nl; i++) ttext(8, 64 + i * 20, lines[i], C_BASE_FG, 14, 0);

    nl = wrap_text("The installer erases the disk you choose in the next step and copies the full "
                    "system onto it. Choose the destination disk carefully: everything currently "
                    "stored on it will be permanently lost.", 624, 14, lines, 4);
    for (int i = 0; i < nl; i++) ttext(8, 112 + i * 20, lines[i], C_BASE_FG, 14, 0);

    sunken_panel(8, 184, 624, 72);
    ttext(24, 196, "Note:", C_BASE_FG, 14, 1);
    nl = wrap_text("The next screen lists the disks this computer can see. The disk MayteraOS is "
                    "running from right now is always shown but can never be selected.", 592, 14, lines, 4);
    for (int i = 0; i < nl; i++) ttext(24, 216 + i * 20, lines[i], C_BASE_FG, 14, 0);

    ttext(8, 408, "Installing requires root privileges.", C_BASE_FG, 11, 0);

    draw_button(8, 448, 88, 24, "Quit", g_focus == 0 ? BTN_HOVER : BTN_NORMAL, 0, 0, g_focus == 0);
    draw_button(536, 448, 96, 24, "Next >", g_focus == 1 ? BTN_HOVER : BTN_NORMAL, 1, 0, g_focus == 1);
}

// ---------------------------------------------------------------------------
// Screen 2: Disk selection
// ---------------------------------------------------------------------------
static void draw_row(int ry, disk_row_t *r, int idx) {
    int selected = (idx == g_selected);
    int hover = (idx == g_hover_row) && r->selectable && !selected;
    uint32_t bg = r->is_boot || r->too_small ? C_DISABLED_BG
                : selected ? C_SEL_BG
                : hover ? C_ROW_HOVER
                : C_INPUT_BG;
    int h = ROW_H;
    win_draw_rect(g_win, ROW_X, ry, ROW_W, h, bg);

    int icx = ROW_X + 16, icy = ry + 27;
    if (r->is_boot || r->too_small) {
        gui_draw_rect_outline(g_win, icx, icy, 14, 14, C_DISABLED_FG);
    } else if (selected) {
        gui_fill_circle_aa(g_win, icx, icy, 14, C_SEL_FG, bg);
        gui_fill_circle_aa(g_win, icx + 2, icy + 2, 10, C_SEL_BG, C_SEL_FG);
        gui_fill_circle_aa(g_win, icx + 3, icy + 3, 8, C_SEL_FG, C_SEL_BG);
    } else {
        gui_fill_circle_aa(g_win, icx, icy, 14, C_ACCENT_SEC, bg);
        gui_fill_circle_aa(g_win, icx + 2, icy + 2, 10, bg, C_ACCENT_SEC);
    }

    uint32_t text_c = (r->is_boot || r->too_small) ? C_DISABLED_FG : selected ? C_SEL_FG : C_BASE_FG;
    ttext(ROW_X + 48, ry + 14, r->name, text_c, 14, 1);
    ttext(ROW_X + 48, ry + 36, r->subtitle, text_c, 11, 0);
    ttext_right(ROW_X + 456, ry + 14, 160, r->capstr, text_c, 14, selected);
    if (r->is_boot) ttext(ROW_X + 48, ry + 50, "CURRENT BOOT DEVICE - cannot be selected", C_BASE_FG, 11, 1);
    else if (r->too_small) ttext(ROW_X + 48, ry + 50, "TOO SMALL TO INSTALL", C_BASE_FG, 11, 1);
}

static void draw_disks(void) {
    draw_chrome(1);
    ttext(8, 28, "Choose a Disk", C_BASE_FG, 20, 1);
    ttext(8, 64, "Select the disk to install MayteraOS on. Everything on the selected disk will be erased.",
          C_BASE_FG, 14, 0);

    sunken_white(LIST_X, LIST_Y, LIST_W, LIST_H);

    int visible = LIST_VISIBLE_ROWS;
    for (int vi = 0; vi < visible; vi++) {
        int idx = g_list_scroll + vi;
        if (idx >= g_nrows) break;
        int ry = LIST_Y + 4 + vi * ROW_H;
        draw_row(ry, &g_rows[idx], idx);
        if (vi < visible - 1 && idx + 1 < g_nrows) win_draw_rect(g_win, ROW_X, ry + ROW_H, ROW_W, 1, C_BTN_BG);
    }
    if (g_nrows > visible) {
        int total = g_nrows * ROW_H;
        int thumb = (LIST_H - 8) * visible / g_nrows; if (thumb < 20) thumb = 20;
        int maxscroll = g_nrows - visible;
        int pos = maxscroll > 0 ? (LIST_H - 8 - thumb) * g_list_scroll / maxscroll : 0;
        gui_draw_scrollbar_v(g_win, LIST_X + LIST_W - 16, LIST_Y + 4, LIST_H - 8, pos, thumb, C_BTN_BG);
        (void)total;
    }

    int next_enabled = g_selected >= 0;
    draw_button(8, 448, 88, 24, "< Back", g_focus == 1 ? BTN_HOVER : BTN_NORMAL, 0, 0, g_focus == 1);
    draw_button(536, 448, 96, 24, "Next >", next_enabled ? (g_focus == 2 ? BTN_HOVER : BTN_NORMAL) : BTN_DISABLED,
                next_enabled, 0, g_focus == 2 && next_enabled);
}

// ---------------------------------------------------------------------------
// Screen 3: Confirmation
// ---------------------------------------------------------------------------
static void draw_confirm(void) {
    draw_chrome(1);
    win_draw_rect(g_win, 8, 26, 4, 30, C_ERROR);
    ttext(20, 28, "Confirm Disk Erase", C_BASE_FG, 20, 1);
    ttext(8, 60, "This is the last step before anything is written. Read this carefully.", C_BASE_FG, 14, 0);

    disk_row_t *r = &g_rows[g_confirm_row];
    sunken_white(8, 92, 624, 150);
    ttext(16, 104, r->name, C_BASE_FG, 16, 1);
    ttext(16, 128, r->subtitle, C_BASE_FG, 14, 0);
    char capline[80], sectstr[24];
    commafmt_u64(r->t.sectors, sectstr, sizeof(sectstr));
    snprintf(capline, sizeof(capline), "%s (%s sectors)", r->capstr, sectstr);
    ttext(16, 150, capline, C_BASE_FG, 14, 0);
    win_draw_rect(g_win, 16, 174, 592, 1, C_BTN_BG);
    ttext(16, 182, "- All partitions and files on this disk will be permanently erased.", C_BASE_FG, 14, 0);
    ttext(16, 202, "- This cannot be undone once installation begins.", C_BASE_FG, 14, 0);
    ttext(16, 222, "- The disk MayteraOS is running from now (the USB stick) is not touched.", C_BASE_FG, 14, 0);

    ttext(8, 254, "Type the disk name shown above to confirm you have the right disk:", C_BASE_FG, 14, 1);
    sunken_white(8, 276, 220, 24);
    ttext(16, 280, r->name, C_BASE_FG, 14, 1);

    sunken_white(8, 308, 300, 22);
    if (g_tf.len == 0) {
        ttext(12, 311, "Type here", C_ACCENT_SEC, 14, 0);
    } else {
        ttext(12, 311, g_tf_buf, C_BASE_FG, 14, 0);
    }
    if (g_focus == 0) {
        char pre[64]; int cl = g_tf.cursor; if (cl > 63) cl = 63;
        memcpy(pre, g_tf_buf, (size_t)cl); pre[cl] = '\0';
        int cx = 12 + gui_ttf_width(pre, 14);
        win_draw_rect(g_win, cx, 310, 1, 18, C_BASE_FG);
    }

    if (g_tf_match) ttext(8, 334, "Confirmed - button enabled below.", C_SUCCESS, 11, 1);
    else            ttext(8, 334, "Waiting for match.", C_BASE_FG, 11, 0);

    draw_button(8, 448, 88, 24, "< Back", g_focus == 1 ? BTN_HOVER : BTN_NORMAL, 0, 0, g_focus == 1);
    draw_button(368, 448, 264, 24, "Erase and Install",
                g_tf_match ? (g_focus == 2 ? BTN_HOVER : BTN_NORMAL) : BTN_DISABLED,
                0, 1, g_focus == 2 && g_tf_match);
}

// ---------------------------------------------------------------------------
// Screen 4: Progress
// ---------------------------------------------------------------------------
static void draw_progress(void) {
    draw_chrome(0);
    ttext(8, 28, "Installing MayteraOS", C_BASE_FG, 20, 1);

    disk_row_t *r = &g_rows[g_confirm_row];
    char tgt[128];
    if (r->have_model) snprintf(tgt, sizeof(tgt), "Target: %s - %s (%s)", r->name, r->subtitle, r->capstr);
    else                snprintf(tgt, sizeof(tgt), "Target: %s - %s", r->name, r->subtitle);
    ttext(8, 60, tgt, C_BASE_FG, 11, 0);

    char pctstr[8]; snprintf(pctstr, sizeof(pctstr), "%d%%", g_progress_pct);
    ttext_centered(270, 148, 100, pctstr, C_BASE_FG, 20, 1);

    sunken_panel(8, 180, 624, 28);
    int filled = (g_progress_pct + 4) / 5; if (filled > 20) filled = 20; if (filled < 0) filled = 0;
    for (int n = 0; n < 20; n++) {
        int sx = 21 + n * 30;
        if (n < filled) { win_draw_rect(g_win, sx, 184, 28, 20, C_ACCENT); gui_draw_rect_outline(g_win, sx, 184, 28, 20, C_BORDER_DARK); }
        else            { win_draw_rect(g_win, sx, 184, 28, 20, C_BTN_BG); gui_draw_rect_outline(g_win, sx, 184, 28, 20, 0x00A0A0A0); }
    }

    ttext_centered(8, 220, 624, g_progress_msg, C_BASE_FG, 14, 0);
    ttext_centered(8, 400, 624, "Do not power off the computer or remove the installation media. This cannot be cancelled.",
                   C_ERROR, 11, 1);
}

static void progress_tick(void) {
    int rc = g_install_rc;
    if (rc != -12345) {
        g_install_done_ms = uptime_ms();
        g_done_success = (rc >= 0);
        if (!g_done_success) {
            static const struct { int code; const char *msg; } table[] = {
                {-1,  "no source filesystem mounted"},
                {-2,  "no install target given"},
                {-3,  "refusing to install onto the source/boot disk"},
                {-4,  "target disk reports zero capacity"},
                {-5,  "source partition size unknown"},
                {-6,  "target disk is too small for the ESP + root partitions"},
                {-7,  "out of memory"},
                {-8,  "failed to write primary GPT header"},
                {-9,  "failed to write GPT partition array"},
                {-10, "failed to write backup GPT array"},
                {-11, "failed to write backup GPT header"},
                {-12, "failed to write protective MBR"},
                {-13, "read from source disk failed"},
                {-14, "write to target disk failed"},
                {-15, "could not locate the ext2 root partition to clone"},
            };
            const char *msg = "installation failed (unknown error)";
            for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++)
                if (table[i].code == rc) { msg = table[i].msg; break; }
            snprintf(g_done_errmsg, sizeof(g_done_errmsg), "Error: %s", msg);
        }
        g_screen = SCR_DONE;
        g_focus = 1;
        return;
    }

    unsigned long elapsed = uptime_ms() - g_install_start_ms;
    unsigned long long secs = g_rows[g_confirm_row].t.sectors;
    unsigned long assumed = (unsigned long)(secs / 2000ULL);
    if (assumed < 3000) assumed = 3000;
    if (assumed > 60000) assumed = 60000;
    int pct = (int)(elapsed * 100 / assumed);
    if (pct > 95) pct = 95;
    if (pct < 1) pct = 1;
    g_progress_pct = pct;

    if (pct < 15)      snprintf(g_progress_msg, sizeof(g_progress_msg), "Writing partition table... (1 of 5)");
    else if (pct < 35) snprintf(g_progress_msg, sizeof(g_progress_msg), "Copying boot partition... (2 of 5)");
    else if (pct < 80) snprintf(g_progress_msg, sizeof(g_progress_msg), "Copying system files... (3 of 5)");
    else               snprintf(g_progress_msg, sizeof(g_progress_msg), "Flushing disk caches... (4 of 5)");
}

// ---------------------------------------------------------------------------
// Screen 5: Done
// ---------------------------------------------------------------------------
static void draw_done(void) {
    draw_chrome(1);
    int cx = 292 + 28, cy = 82 + 28;
    uint32_t icolor = g_done_success ? C_SUCCESS : C_ERROR;
    gui_fill_circle_aa(g_win, 292, 82, 56, icolor, C_BASE_BG);
    if (g_done_success) {
        gui_thick_line(g_win, cx - 11, cy + 1, cx - 3, cy + 9, 3, C_SEL_FG);
        gui_thick_line(g_win, cx - 3, cy + 9, cx + 12, cy - 10, 3, C_SEL_FG);
    } else {
        gui_thick_line(g_win, cx - 12, cy - 12, cx + 12, cy + 12, 3, C_SEL_FG);
        gui_thick_line(g_win, cx - 12, cy + 12, cx + 12, cy - 12, 3, C_SEL_FG);
    }

    ttext_centered(8, 150, 624, g_done_success ? "Installation Complete" : "Installation Failed", C_BASE_FG, 20, 1);

    disk_row_t *r = &g_rows[g_confirm_row];
    char lines[4][WRAP_LINE_MAX]; int nl;
    if (g_done_success) {
        char l1[96]; snprintf(l1, sizeof(l1), "MayteraOS is installed on %s.", r->name);
        ttext_centered(70, 190, 500, l1, C_BASE_FG, 14, 0);
        ttext_centered(70, 210, 500, "Remove the USB stick and restart this computer to boot from the internal disk.",
                       C_BASE_FG, 14, 0);
        unsigned long total_s = (g_install_done_ms - g_install_start_ms) / 1000;
        unsigned long mm = total_s / 60, ss = total_s % 60;
        char fin[80];
        snprintf(fin, sizeof(fin), "Finished in %lu minute%s %lu second%s.",
                 mm, mm == 1 ? "" : "s", ss, ss == 1 ? "" : "s");
        ttext_centered(8, 256, 624, fin, C_BASE_FG, 11, 0);
    } else {
        nl = wrap_text("The disk may be left in an incomplete state. Choose a different disk, or check the disk, and try again.",
                        500, 14, lines, 4);
        for (int i = 0; i < nl; i++) ttext_centered(70, 190 + i * 20, 500, lines[i], C_BASE_FG, 14, 0);
        sunken_white(170, 240, 300, 24);
        ttext_centered(170, 245, 300, g_done_errmsg, C_ERROR, 12, 0);
    }

    draw_button(8, 448, 88, 24, "Close", g_focus == 0 ? BTN_HOVER : BTN_NORMAL, 0, 0, g_focus == 0);
    draw_button(368, 448, 264, 24, g_done_success ? "Restart Now" : "Try Again",
                g_focus == 1 ? BTN_HOVER : BTN_NORMAL, 1, 0, g_focus == 1);
}

// ---------------------------------------------------------------------------
// Redraw dispatch
// ---------------------------------------------------------------------------
static void redraw(void) {
    switch (g_screen) {
        case SCR_INTRO:    draw_intro();    break;
        case SCR_DISKS:    draw_disks();    break;
        case SCR_CONFIRM:  draw_confirm();  break;
        case SCR_PROGRESS: draw_progress(); break;
        case SCR_DONE:     draw_done();     break;
    }
    win_invalidate(g_win);
}

// ---------------------------------------------------------------------------
// Screen transitions
// ---------------------------------------------------------------------------
static void goto_intro(void)   { g_screen = SCR_INTRO;  g_focus = 1; redraw(); }
static void goto_disks(void)   { enumerate_disks(); g_screen = SCR_DISKS; g_focus = 0; g_hover_row = -1; redraw(); }
static void goto_confirm(void) {
    if (g_selected < 0) return;
    g_confirm_row = g_selected;
    g_screen = SCR_CONFIRM;
    g_focus = 0;
    g_tf_buf[0] = '\0';
    tf_init(&g_tf, g_tf_buf, sizeof(g_tf_buf));
    g_tf_match = 0;
    redraw();
}

static void *install_thread_fn(void *arg) {
    install_args_t *a = (install_args_t *)arg;
    int rc = inst_install(a->kind, a->index);
    g_install_rc = rc;
    return 0;
}

static void start_install(void) {
    g_install_args.kind = g_rows[g_confirm_row].t.kind;
    g_install_args.index = g_rows[g_confirm_row].t.index;
    g_install_rc = -12345;
    g_install_start_ms = uptime_ms();
    g_progress_pct = 1;
    snprintf(g_progress_msg, sizeof(g_progress_msg), "Writing partition table... (1 of 5)");
    g_screen = SCR_PROGRESS;
    redraw();

    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setstacksize(&at, 262144);
    pthread_t th;
    if (pthread_create(&th, &at, install_thread_fn, &g_install_args) == 0) pthread_detach(th);
    else {
        // Could not even start the thread: report as a failure rather than
        // hang on a progress screen that will never move.
        g_install_rc = -7;
    }
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
static void do_quit(void) { g_running = 0; }

static void activate_intro(int which) { if (which == 0) do_quit(); else goto_disks(); }
static void activate_disks(int which) {
    if (which == 1) goto_intro();
    else if (which == 2 && g_selected >= 0) goto_confirm();
}
static void activate_confirm(int which) {
    if (which == 1) goto_disks();
    else if (which == 2 && g_tf_match) start_install();
}
static void activate_done(int which) {
    if (which == 0) do_quit();
    else { if (g_done_success) reboot(); else goto_disks(); }
}

static void move_list_selection(int dir) {
    if (g_nrows == 0) return;
    int idx = g_selected;
    if (idx < 0) idx = 0;
    for (int step = 0; step < g_nrows; step++) {
        idx += dir;
        if (idx < 0) idx = g_nrows - 1;
        if (idx >= g_nrows) idx = 0;
        if (g_rows[idx].selectable) { g_selected = idx; break; }
    }
    if (g_selected >= 0) {
        if (g_selected < g_list_scroll) g_list_scroll = g_selected;
        if (g_selected >= g_list_scroll + LIST_VISIBLE_ROWS) g_list_scroll = g_selected - LIST_VISIBLE_ROWS + 1;
    }
}

static void handle_key(gui_event_t *ev) {
    char c = ev->key_char;
    uint32_t kc = ev->keycode;

    if (g_screen == SCR_CONFIRM && g_focus == 0) {
        if (c == '\t') { g_focus = 1; redraw(); return; }
        if (tf_handle_key(&g_tf, ev)) {
            g_tf_match = (strcmp(g_tf_buf, g_rows[g_confirm_row].name) == 0) && g_tf.len > 0;
            redraw();
        }
        return;
    }

    if (c == '\t') {
        int nfocus = (g_screen == SCR_INTRO) ? 2 : (g_screen == SCR_DISKS) ? 3
                   : (g_screen == SCR_CONFIRM) ? 3 : (g_screen == SCR_DONE) ? 2 : 1;
        if (g_screen == SCR_PROGRESS) return;
        g_focus = (g_focus + 1) % nfocus;
        redraw();
        return;
    }

    if (g_screen == SCR_DISKS && g_focus == 0) {
        if (kc == 0x80) { move_list_selection(-1); redraw(); return; }   // Up
        if (kc == 0x81) { move_list_selection(1);  redraw(); return; }   // Down
    }

    int is_enter = (c == '\r' || c == '\n' || kc == 0x1C);
    int is_space = (c == ' ');
    if (is_enter || is_space) {
        switch (g_screen) {
            case SCR_INTRO:   activate_intro(g_focus);   break;
            case SCR_DISKS:
                if (g_focus == 0) { if (is_enter) activate_disks(2); }
                else activate_disks(g_focus);
                break;
            case SCR_CONFIRM: activate_confirm(g_focus); break;
            case SCR_DONE:    activate_done(g_focus);    break;
            default: break;
        }
    }
}

static void handle_click(int mx, int my) {
    if (closebox_hit(mx, my)) {
        if (g_screen != SCR_PROGRESS) do_quit();
        return;
    }
    switch (g_screen) {
        case SCR_INTRO:
            if (pt_in(mx, my, 8, 448, 88, 24)) { g_focus = 0; activate_intro(0); }
            else if (pt_in(mx, my, 536, 448, 96, 24)) { g_focus = 1; activate_intro(1); }
            break;
        case SCR_DISKS: {
            for (int vi = 0; vi < LIST_VISIBLE_ROWS; vi++) {
                int idx = g_list_scroll + vi;
                if (idx >= g_nrows) break;
                int ry = LIST_Y + 4 + vi * ROW_H;
                if (pt_in(mx, my, ROW_X, ry, ROW_W, ROW_H) && g_rows[idx].selectable) {
                    g_selected = idx; g_focus = 0; redraw(); return;
                }
            }
            if (pt_in(mx, my, 8, 448, 88, 24)) { g_focus = 1; activate_disks(1); }
            else if (pt_in(mx, my, 536, 448, 96, 24) && g_selected >= 0) { g_focus = 2; activate_disks(2); }
            break;
        }
        case SCR_CONFIRM:
            if (pt_in(mx, my, 8, 308, 300, 22)) { g_focus = 0; redraw(); }
            else if (pt_in(mx, my, 8, 448, 88, 24)) { g_focus = 1; activate_confirm(1); }
            else if (pt_in(mx, my, 368, 448, 264, 24) && g_tf_match) { g_focus = 2; activate_confirm(2); }
            break;
        case SCR_DONE:
            if (pt_in(mx, my, 8, 448, 88, 24)) { g_focus = 0; activate_done(0); }
            else if (pt_in(mx, my, 368, 448, 264, 24)) { g_focus = 1; activate_done(1); }
            break;
        default: break;
    }
}

static void handle_move(int mx, int my) {
    int changed = 0;
    if (g_screen == SCR_DISKS) {
        int newhover = -1;
        for (int vi = 0; vi < LIST_VISIBLE_ROWS; vi++) {
            int idx = g_list_scroll + vi;
            if (idx >= g_nrows) break;
            int ry = LIST_Y + 4 + vi * ROW_H;
            if (pt_in(mx, my, ROW_X, ry, ROW_W, ROW_H)) { newhover = idx; break; }
        }
        if (newhover != g_hover_row) { g_hover_row = newhover; changed = 1; }
    }
    if (changed) redraw();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    int win_x = 80, win_y = 40;
    fb_info_t fi;
    if (fb_info(&fi) == 0 && fi.width > 0 && fi.height > 0) {
        win_x = ((int)fi.width - WIN_W) / 2;
        win_y = ((int)fi.height - 36 - WIN_H) / 2;
        if (win_x < 0) win_x = 0;
        if (win_y < 0) win_y = 0;
    }

    g_win = win_create("Install to Disk", win_x, win_y, WIN_W, WIN_H);
    if (g_win < 0) { printf("[INSTALL] win_create failed\n"); return 1; }
    win_set_nochrome(g_win);

    g_focus = 1;
    redraw();

    gui_event_t ev;
    while (g_running) {
        int et = win_get_event(g_win, &ev, 120);

        if (g_screen == SCR_PROGRESS) {
            progress_tick();
            redraw();
            if (et == EVENT_MOUSE_DOWN || et == EVENT_KEY_DOWN || et == EVENT_WINDOW_CLOSE) {
                // Progress screen has no interactive controls and cannot be
                // closed: the engine cannot stop once writing starts (see
                // spec section 8). Every input is deliberately swallowed.
            }
            continue;
        }
        if (et == 0) continue;

        switch (ev.type) {
            case EVENT_REDRAW: redraw(); break;
            case EVENT_WINDOW_CLOSE: do_quit(); break;
            case EVENT_MOUSE_MOVE: handle_move(ev.mouse_x, ev.mouse_y); break;
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) handle_click(ev.mouse_x, ev.mouse_y);
                break;
            case EVENT_KEY_DOWN: handle_key(&ev); break;
            default: break;
        }
    }

    win_destroy(g_win);
    return 0;
}
