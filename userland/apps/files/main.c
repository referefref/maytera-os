// files - Modern-style file browser for MayteraOS (user-space)
//
// Features: tabbed views, back/forward/up navigation with history, a Places
// sidebar (Quick access home folders + dynamically-enumerated drives + Network),
// a command bar (New / View / Sort / Filter), and a toggleable preview pane that
// previews text, binaries (hex dump) and BMP images.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_font.h"
#include "../../libc/gui_scroll.h"   // shared scrollbar contrast rule (#745 item 77)
#include "../../libc/notify.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"
#include "../../libc/gui_theme.h"
#include "../../libc/pwd.h"
#include "../../libc/unistd.h"
#include "../../libc/assoc.h"
#include "../../libc/gui_style.h"
#include "netscan.h"   // automatic LAN share discovery (worker thread)

// Route all in-window text through the antialiased TrueType path (matches Settings).
#define win_draw_text(h, x, y, s, c)       win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define win_draw_text_small(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 11, (c))

static int str_eq(const char *a, const char *b);

// ---- layout ---------------------------------------------------------------
static int g_win_w = 820, g_win_h = 560;  // live content size (EVENT_RESIZE)
#define WIN_W        g_win_w
#define WIN_H        g_win_h
// (filesglass) Glass geometry: the SAME numbers the Calculator, Task Manager,
// Editor and Terminal glass restyles use (PAD 10 / TAB_H 26 / PANEL_R 12 /
// PANEL_IN 12), so every glass window shares one geometry. Four panels sit on
// the frosted backdrop: a header (tab pills + toolbar), the Places sidebar,
// the list (with the status row inside it) and the optional preview.
#define PAD          10      // window margin: the backdrop shows here
#define PANEL_R      12      // panel corner radius
#define PANEL_IN     12      // inset from a panel edge to its content
#define PANEL_GAP    8       // gap between neighbouring panels
#define TAB_H        26      // tab pill height (radius TAB_H/2)
#define TOOL_H       24      // toolbar control height
#define HDR_H        (6 + TAB_H + 6 + TOOL_H + 8)   // 70
#define STATUS_H     24
#define SIDEBAR_W    160
#define PREVIEW_W    230
#define ITEM_HEIGHT  24
#define ROW_R        6       // row highlight / nested card radius
#define SCROLL_GUT   14      // scrollbar gutter reserved right of the list
#define ICON_SIZE    16
#define MAX_ITEMS    512
#define MAX_PATH_LEN 256
#define MAX_NAME_LEN 64
#define MAX_TABS     8
#define MAX_HIST     24

static int g_preview_on = 1;
static int  g_last_click_idx = -1;   // double-click detection (file list)
static long g_last_click_ticks = 0;
static int g_in_recycle = 0;   // 1 = content area shows the integrated Recycle Bin

// (filesglass) Panel rects. ONE definition each: the draw path and every hit
// test read these, so a rect cannot drift between the two.
static int hdr_x(void)   { return PAD; }
static int hdr_y(void)   { return PAD; }
static int hdr_w(void)   { return WIN_W - 2 * PAD; }
static int tabrow_y(void){ return hdr_y() + 6; }                    // tab pills
static int tool_y(void)  { return hdr_y() + 6 + TAB_H + 6; }        // toolbar controls
static int body_y(void)  { return PAD + HDR_H + PANEL_GAP; }        // top of the three body panels
static int body_h(void)  { return WIN_H - PAD - body_y(); }
static int side_x(void)  { return PAD; }
static int prev_x(void)  { return WIN_W - PAD - PREVIEW_W; }
static int prev_on(void) { return g_preview_on && !g_in_recycle; }  // the preview panel is drawn
static int cont_x(void)  { return PAD + SIDEBAR_W + PANEL_GAP; }    // list panel (or recycle panel)
static int cont_w(void)  { return (prev_on() ? prev_x() - PANEL_GAP : WIN_W - PAD) - cont_x(); }
// The file list inside the list panel: 6px in from the panel's rounded
// corners, a SCROLL_GUT gutter on the right, the status row below it.
static int list_x(void)  { return cont_x() + 6; }
static int list_y(void)  { return body_y() + 6; }
static int list_w(void)  { return cont_w() - 12 - SCROLL_GUT; }
static int list_h(void)  { return body_h() - 6 - STATUS_H - 4; }
static int status_y(void){ return body_y() + body_h() - STATUS_H; }
#define CONTENT_Y    list_y()
#define CONTENT_H    list_h()

// ---------------------------------------------------------------------------
// (filesglass) Colour tokens: docs/UI_GLASS_DESIGN_SYSTEM.md section 1, the
// same names and hex the Calculator / Task Manager / Media Player / Image
// Viewer / Editor / Terminal glass restyles use. FIXED dark glass regardless
// of theme, as those are: the window carries its own frosted backdrop, so the
// theme's window colours never appear inside it. (The previous palette derived
// tinted greys from theme_color(); that derivation is gone, and the shared
// gui_* widgets get these tokens through gui_set_palette() in
// files_apply_style() below, so the widgets themselves are unchanged.)
// ---------------------------------------------------------------------------
#define C_PANEL       0x00122420   // WEL_BG_MID: panel fill, glass tint, the outer colour AA edges blend toward
#define C_CARD        0x000E1D1B   // DK_CARD_FILL: popup / dialog fill, scroll track, unselected tab pill
#define C_EDGE        0x002C4A44   // DK_STROKE_UNSEL: panel border, hairlines, separators
#define C_EDGE_GLASS  0x006FA99E   // DK_EDGE_GLASS: strokes on glass (popup + dialog border), scroll thumb
#define C_INK         0x00F3FBF9   // DK_HEADLINE: names, values, status
#define C_INK_DIM     0x00A9D9CC   // DK_BODY: detail columns, tab labels, dialog labels
#define C_EYEBROW     0x006AE2CF   // DK_EYEBROW: sidebar section headers, "PREVIEW"
#define C_ACCENT      0x006AE2CF   // DK_ACCENT: selected row / tab / Places entry, hotspot links
#define C_ACCENT_INK  0x0004231A   // text on the accent
#define C_ERR         0x00FFAAA2   // DK_ERROR: tab close x, "(not readable)" note
#define C_IN_PH       0x0087ABA2   // DK_INPUT_PH: filter placeholder
#define C_IN_FILL     0x00213B34   // DK_INPUT_FILL
#define C_IN_BORDER   0x004E7168   // DK_INPUT_BORDER (resting)
#define WEL_BG_TOP    0x000A1614   // backdrop gradient fallback, top stop
#define WEL_BG_BOTTOM 0x00050A09   // backdrop gradient fallback, bottom stop
#define ERR_BAND      0x00A02020   // status-row failure band (#742), white ink

// Ink for text on a given fill: the accent (and any light fill) takes the
// accent ink, every glass fill takes the headline ink.
static inline unsigned int files_fg(unsigned int bg) {
    int r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    int lum = (r * 30 + g * 59 + b * 11) / 100;
    return lum > 140 ? C_ACCENT_INK : C_INK;
}
static inline unsigned int fp_acc(void){ return C_ACCENT; }
#define BG_COLOR      C_PANEL
#define SIDEBAR_BG    C_PANEL
#define ITEM_HOVER    gui_lighten(C_PANEL, 10)
#define ITEM_SELECTED C_ACCENT
#define TEXT_COLOR    C_INK
#define SIDE_TEXT     C_INK
static inline unsigned int files_dim(unsigned int bg) {
    if (bg == C_PANEL) return C_INK_DIM;
    unsigned int ink = files_fg(bg);
    int ir=(ink>>16)&0xFF, ig=(ink>>8)&0xFF, ib=ink&0xFF;
    int br=(bg>>16)&0xFF, bgc=(bg>>8)&0xFF, bb=bg&0xFF;
    // Bias toward the ink (5/8) so dim/detail text stays readable on the
    // selection fill.
    return ((((ir*5+br*3)/8)&0xFF)<<16) | ((((ig*5+bgc*3)/8)&0xFF)<<8) | (((ib*5+bb*3)/8)&0xFF);
}
#define DIM_TEXT      C_INK_DIM
#define SIDE_DIM      C_INK_DIM
#define ICON_FOLDER   0x00FFC800
#define BORDER_COLOR  C_EDGE

// (filesglass) The frosted-wallpaper backdrop: the shared libc recipe
// (userland/libc/gui_style.h gui_glass_backdrop_*, consolidated at glasslib).
// This app owns only the two persistent pieces the API asks for.
static uint32_t g_bd[GUI_GLASS_BD_W * GUI_GLASS_BD_H];
static int g_bd_wi = GUI_GLASS_BD_NEVER;   // wallpaper index the backdrop was built for
static int g_chrome_dirty = 1;             // the ONLY thing that can make fb_redraw() blit the backdrop
// Signature of the layout facts the last blitted frame was composed for
// (window size, which panels exist, which popup/dialog floats over the
// margins). A change in any of them UNCOVERS margin (the preview panel
// hiding, the Recycle view widening the list panel, a menu or dialog
// closing), so the next frame must blit the backdrop again (blame.md
// audglass trap 4 / edglass trap 3). Tracked at the top of fb_redraw() by
// construction rather than at each toggle site.
static unsigned long g_layout_sig = ~0ul;

static void sync_backdrop(void) {
    if (gui_glass_backdrop_sync(g_bd, &g_bd_wi, C_PANEL, 158, WEL_BG_TOP, WEL_BG_BOTTOM))
        g_chrome_dirty = 1;
}

// ---- model ----------------------------------------------------------------
// #554: per-entry filesystem-aware permission/attribute info, fetched via
// SYS_FS_PERM_INFO alongside the existing readdir data. fs_type tells the
// UI (details column, Properties tab) which of mode/uid/gid vs fat_attr is
// meaningful - never both, and never a fabricated value for the one that
// doesn't apply on this filesystem (see docs/UI_STYLE_GUIDE.md #554).
typedef struct {
    char name[MAX_NAME_LEN];
    bool is_directory;
    uint32_t size;
    uint8_t  fs_type;        // FSPERM_TYPE_POSIX / _FAT / _OTHER
    uint8_t  has_perm_entry; // fs_type==POSIX only
    uint8_t  fat_attr;       // fs_type==FAT only
    uint16_t mode;           // fs_type==POSIX only
    uint32_t uid;            // fs_type==POSIX only
    uint32_t gid;            // fs_type==POSIX only
} file_entry_t;

// A browser tab: its own location + navigation history + view state.
typedef struct {
    char path[MAX_PATH_LEN];
    char back[MAX_HIST][MAX_PATH_LEN];  int back_n;
    char fwd[MAX_HIST][MAX_PATH_LEN];   int fwd_n;
    int  sel;
    int  scroll;
    bool used;
} tab_t;

static tab_t tabs[MAX_TABS];
static int   tab_count = 1;
static int   active_tab = 0;

static file_entry_t items[MAX_ITEMS];
static int item_count = 0;
static int hover_item = -1;

// view + sort + filter
enum { VIEW_LIST, VIEW_DETAILS, VIEW_ICONS };
enum { SORT_NAME, SORT_SIZE, SORT_TYPE, SORT_ATTR };  // #554: SORT_ATTR
static int  g_view = VIEW_DETAILS;
static int  g_sort = SORT_NAME;
static char g_filter[48] = "";
// #554: filter out FAT hidden-attribute files and UNIX-convention dotfiles.
// Off by default so existing behavior (everything shown) is unchanged; the
// View menu toggles it. This is the "filtering ... on attribute columns" half
// of task #554 - the other half is the sortable Attr column below.
static bool g_show_hidden = false;

// dropdown menu state
enum { MENU_NONE, MENU_NEW, MENU_VIEW, MENU_CTX };
static int  g_menu = MENU_NONE;
static int  g_menu_x = 0, g_menu_y = 0;

// (#251) clipboard for Copy / Cut / Paste, and Properties dialog state.
static char g_clip_path[MAX_PATH_LEN] = "";
static int  g_clip_mode = 0;     // 0=none, 1=copy, 2=cut
static int  g_props_open = 0;    // 1 = Properties dialog visible
static void draw_props(void);    // defined alongside the file operations below

// (#251 Task C) reusable modal text-entry overlay (inline Rename) + "Open with"
// app picker. g_te_* drives the generic text-entry box; g_openwith_* the picker.
static int   g_te_open = 0;                 // text-entry overlay visible
static char  g_te_title[40] = "";
static char  g_te_buf[MAX_NAME_LEN] = "";
static int   g_te_len = 0;
static int   g_te_purpose = 0;              // 1 = rename
static int   g_openwith_open = 0;           // app-picker overlay visible
static int   g_ow_hover = -1;
static void  draw_te(void);
static void  draw_openwith(void);

// #317: authenticated SMB browse credentials dialog. Separate Server / Share /
// Username / Password fields (the password is MASKED, never shown or logged in
// cleartext), plus Connect / Cancel and a "Save as favourite" checkbox. This
// supersedes the crude one-line "Add: server share [user] [pass]" text entry as
// the primary path, and is also auto-opened when an auth-gated SMB server or
// share fails to list. Entered credentials flow through the SAME net_mount +
// "/SMB/<server>/<share>" navigation path the discovered/saved rows already use.
// Each field reuses the shared caret/selection/clipboard textfield widget
// (textfield.h); the whole dialog is drawn with the shared style engine
// (gui_textfield_tf / gui_checkbox / gui_button), never a hand-rolled widget.
static int   g_cred_open  = 0;              // credentials dialog visible
static int   g_cred_focus = 0;             // 0=Server 1=Share 2=Username 3=Password
static int   g_cred_save  = 0;             // "Save as favourite" checkbox state
static char  g_cred_server[64] = "";
static char  g_cred_share[40]  = "";
static char  g_cred_user[40]   = "";
static char  g_cred_pass[40]   = "";
static textfield_t g_cred_tf[4];           // one caret-aware field per input

static int window_handle = -1;
static int win_x = 90, win_y = 40;
static char g_home[MAX_PATH_LEN] = "/APPS";

// active tab convenience
#define CUR (tabs[active_tab])
static char *current_path = NULL;   // points at CUR.path after init

static void fb_redraw(void);
static void load_directory(const char *path);
static void navigate_to(const char *path);                 // defined below (navigation)
static int  copy_file(const char *src, const char *dst);   // defined below (clipboard)
static void open_add_network(void);                        // #317 Network "Add" dialog
static void draw_cred(void);                               // #317 SMB credentials dialog
static void open_cred_dialog(const char *server, const char *share, const char *user); // #317
static void files_error(const char *what, const char *detail);   // #742
static void files_err_clear(void);                               // #742

// ---- #742 failure reporting ------------------------------------------------
// The thing that failed is the DISK, so the report must not need the disk.
// g_err lives in RAM and is drawn in the status bar until the next action;
// notify_post() (which writes /CONFIG/NOTIFY.TXT) and printf() are best-effort
// extras on top, never the only channel. That ordering is the point: an error
// path whose only output is another write to the failing medium is not a report.
static char g_err[192];

static void files_err_clear(void) { g_err[0] = 0; }

static void files_error(const char *what, const char *detail) {
    int o = 0;
    for (int i = 0; what && what[i] && o < (int)sizeof(g_err) - 2; i++) g_err[o++] = what[i];
    if (detail && detail[0]) {
        if (o < (int)sizeof(g_err) - 3) { g_err[o++] = ':'; g_err[o++] = ' '; }
        for (int i = 0; detail[i] && o < (int)sizeof(g_err) - 1; i++) g_err[o++] = detail[i];
    }
    g_err[o] = 0;
    printf("[files] %s\n", g_err);
    notify_post("Files", g_err, NOTIFY_ERROR);   // best effort; may itself fail
}

// ---- tiny string helpers --------------------------------------------------
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void str_copy(char *d, const char *s, int max) { int i = 0; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0; }
static int str_eq(const char *a, const char *b) { while (*a && *b && *a == *b) { a++; b++; } return *a == *b; }
static int ci_has(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (hay[i + j] && needle[j]) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}
static void ext_of(const char *name, char *e) {
    int n = 0; while (name[n]) n++; int dot = -1;
    for (int i = n - 1; i >= 0; i--) { if (name[i] == '.') { dot = i; break; } }
    int j = 0;
    if (dot >= 0) for (int i = dot + 1; name[i] && j < 7; i++) { char c = name[i]; if (c >= 'A' && c <= 'Z') c += 32; e[j++] = c; }
    e[j] = 0;
}
static const char *basename_of(const char *path) {
    int n = str_len(path);
    if (n <= 1) return "Root";
    int i = n - 1; if (path[i] == '/') i--;
    int end = i;
    while (i > 0 && path[i] != '/') i--;
    if (path[i] == '/') i++;
    static char b[40]; int k = 0;
    for (; i <= end && k < 39; i++) b[k++] = path[i];
    b[k] = 0;
    return b[0] ? b : "Root";
}
static void path_join(char *out, const char *dir, const char *name) {
    int len = str_len(dir);
    str_copy(out, dir, MAX_PATH_LEN);
    if (len > 1) out[len++] = '/';
    int i = 0; while (name[i] && len < MAX_PATH_LEN - 1) out[len++] = name[i++];
    out[len] = 0;
}

// #84 file associations: resolved OS-wide via /ASSOC.CFG (Settings > Default Apps).
static const char *default_app_for(const char *name) {
    static char appbuf[80];
    return assoc_app_for(name, appbuf, sizeof(appbuf));
}
static int is_image_ext(const char *e) { return str_eq(e,"bmp")||str_eq(e,"png")||str_eq(e,"jpg")||str_eq(e,"jpeg")||str_eq(e,"gif"); }
static int is_text_ext(const char *e) {
    return str_eq(e,"txt")||str_eq(e,"c")||str_eq(e,"h")||str_eq(e,"md")||str_eq(e,"cfg")||
           str_eq(e,"ini")||str_eq(e,"log")||str_eq(e,"sh")||str_eq(e,"yml")||str_eq(e,"yaml")||
           str_eq(e,"json")||str_eq(e,"js")||str_eq(e,"asm")||str_eq(e,"conf")||str_eq(e,"")||str_eq(e,"me");
}

// ---- Quick-access (home) folders ------------------------------------------
// FAT is 8.3-only so the on-disk names are <=8 chars; we show friendly labels.
static const char *qa_label[] = { "Home", "Desktop", "Documents", "Downloads", "Pictures", "Music", "Videos", NULL };
static const char *qa_dir[]   = { "",     "DESKTOP", "DOCUMENT",  "DOWNLOAD",  "PICTURES", "MUSIC", "VIDEOS" };
static void qa_path(int i, char *out) {
    if (i == 0) { str_copy(out, g_home, MAX_PATH_LEN); return; }
    path_join(out, g_home, qa_dir[i]);
}

// ---- dynamically-enumerated drives ----------------------------------------
static disk_info_t g_disks[4];
static int g_disk_count = 0;

// ===========================================================================
// #250 REMOVABLE VOLUMES (hot-plugged USB drives).
//
// disk_info_t above is FIXED ATA disks: it has no mount point, so every Disk
// row in the sidebar navigates to "/" and none of them can be ejected. A USB
// stick is a different thing and needs the volume list, which is one syscall.
//
// POLLED, NOT PUSHED, and polled from the EVENT LOOP TIMEOUT, never from a
// draw path. draw_sidebar() runs on every repaint and must not do I/O.
// ===========================================================================
// #234i: SC_VOL_MAX (16), not 8. The list gained a second producer (mounted
// floppy and CD-ROM disk images join the USB hot-plug volumes), and a buffer
// sized to only one of them silently drops the other's rows.
#define MAX_VOLS SC_VOL_MAX
static sc_volume_t g_vols[MAX_VOLS];
static int g_vol_count = 0;

// ===========================================================================
// #234i: WHICH VOLUME IS THIS PATH ON?
// ===========================================================================
// ADVISORY, NOT ENFORCEMENT. The kernel is the enforcer: fs/fat.c:681 refuses
// fat_write() on any image-backed handle, so a write to a mounted disc fails
// whatever this app believes. What this decides is whether Files OFFERS the
// action, which is a display question and therefore belongs in the app.
//
// The matching rule (case-insensitive, and the character after the mount point
// must be a separator so /USB1 cannot swallow /USB10) is the same rule
// split_core() implements in kernel/rustkern/hotplug.rs, which is the
// authority. Kept short and stated rather than shared, because exporting a
// kernel string routine to Ring 3 for a tooltip is a worse trade than fifteen
// lines that cannot affect correctness of any write.
static char up1(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

static const sc_volume_t *vol_for_path(const char *path) {
    if (!path || path[0] != '/') return 0;
    for (int i = 0; i < g_vol_count; i++) {
        const char *m = g_vols[i].mount;
        if (!m[0]) continue;
        int k = 0;
        while (m[k] && path[k] && up1(m[k]) == up1(path[k])) k++;
        if (m[k] != 0) continue;                    // mount point not fully matched
        if (path[k] == 0 || path[k] == '/') return &g_vols[i];
    }
    return 0;
}

// 1 if the directory Files is showing lives on a volume that refuses writes.
static int cur_is_readonly(void) {
    const sc_volume_t *v = vol_for_path(CUR.path);
    return (v && (v->flags & MOSVOL_READONLY)) ? 1 : 0;
}

// THE gate for every mutating action. Returns 1 when the action must not
// proceed, and says why on the way out: a button that silently does nothing
// is the failure mode this is here to avoid.
static int ro_block(const char *what) {
    const sc_volume_t *v = vol_for_path(CUR.path);
    if (!v || !(v->flags & MOSVOL_READONLY)) return 0;
    char msg[128]; int l = 0;
    const char *a = what;              while (*a && l < 40) msg[l++] = *a++;
    const char *b = " is not possible on ";  while (*b && l < 90) msg[l++] = *b++;
    for (int k = 0; k < 24 && v->name[k]; k++) msg[l++] = v->name[k];
    const char *c = ": this volume is read-only.";
    while (*c && l < (int)sizeof(msg) - 1) msg[l++] = *c++;
    msg[l] = 0;
    notify_post("Read-only volume", msg, NOTIFY_WARNING);
    return 1;
}

// Cheap change detector, so the common case (nothing plugged in) redraws
// nothing. Folds the fields a sidebar row actually shows.
static unsigned volumes_sig(void) {
    unsigned sig = 2166136261u;
    for (int i = 0; i < g_vol_count; i++) {
        sig = (sig ^ (unsigned)g_vols[i].index) * 16777619u;
        sig = (sig ^ g_vols[i].flags) * 16777619u;
        sig = (sig ^ (unsigned)(g_vols[i].total_bytes >> 20)) * 16777619u;
        for (int k = 0; k < 32 && g_vols[i].mount[k]; k++)
            sig = (sig ^ (unsigned char)g_vols[i].mount[k]) * 16777619u;
        for (int k = 0; k < 64 && g_vols[i].name[k]; k++)
            sig = (sig ^ (unsigned char)g_vols[i].name[k]) * 16777619u;
    }
    return sig ^ (unsigned)g_vol_count;
}
static unsigned g_vol_sig = 0;

// Returns 1 if the list changed since the last call.
//
// #234i: AND LEAVES A VOLUME THAT HAS GONE. Eject is no longer only a thing
// this app does to itself: the Disk Images app, a DOS guest swapping discs, or
// a second Files window can all unmount the volume this window is standing in.
// The eject button already navigated away before ejecting (see the sidebar
// click handler); that only covers the case where WE did it. Without this, a
// disc ejected from anywhere else leaves the window showing a directory
// listing of a volume that no longer exists, with every row dead.
static int poll_volumes(void) {
    char was_on[MAX_PATH_LEN]; was_on[0] = 0;
    {
        const sc_volume_t *v = vol_for_path(CUR.path);
        if (v) str_copy(was_on, v->mount, MAX_PATH_LEN);
    }

    int n = vol_list(g_vols, MAX_VOLS);
    g_vol_count = (n > 0) ? n : 0;

    if (was_on[0]) {
        int still = 0;
        for (int i = 0; i < g_vol_count; i++)
            if (str_eq(g_vols[i].mount, was_on)) { still = 1; break; }
        if (!still) {
            // Home, not "/": the user is being moved somewhere they can act,
            // and every tab that was on the dead volume moves, not just the
            // visible one.
            for (int t = 0; t < tab_count; t++) {
                const char *tp = tabs[t].path;
                int k = 0;
                while (was_on[k] && tp[k] && up1(was_on[k]) == up1(tp[k])) k++;
                if (was_on[k] == 0 && (tp[k] == 0 || tp[k] == '/'))
                    str_copy(tabs[t].path, g_home, MAX_PATH_LEN);
            }
            g_in_recycle = 0;
            // load_directory, not navigate_to: the tab's path has already
            // been rewritten above, so navigate_to() would push a history
            // entry pointing at the folder we are already in.
            load_directory(CUR.path);
            notify_post("Volume removed",
                        "That drive was ejected, so Files went back to your home folder.",
                        NOTIFY_INFO);
            g_vol_sig = volumes_sig();
            return 1;
        }
    }

    unsigned sig = volumes_sig();
    if (sig == g_vol_sig) return 0;
    g_vol_sig = sig;
    return 1;
}

// Human size for a volume row: whole GB above 1 GB, MB above 1 MB, else KB.
//
// #234i: THE KB ARM IS NOT COSMETIC. Every volume this ever described was a
// USB stick measured in gigabytes, so integer-dividing by 1 MB was safe. A
// 720 KB floppy image and a 354 KB ISO both came out as the string "0MB",
// which is not a rounding artefact to a reader: it says the disc is empty.
// Two of the three volumes on screen said 0MB the first time this shipped.
static void vol_size_label(const sc_volume_t *v, char *out, int cap) {
    unsigned long long kb = v->total_bytes / 1024ULL;
    unsigned long long mb = kb / 1024ULL;
    char num[24];
    const char *unit;
    if (mb >= 1024) { gui_itoa((int)(mb / 1024), num, sizeof(num)); unit = "GB"; }
    else if (mb >= 1) { gui_itoa((int)mb, num, sizeof(num)); unit = "MB"; }
    else            { gui_itoa((int)kb, num, sizeof(num)); unit = "KB"; }
    int l = 0;
    for (int k = 0; num[k] && l < cap - 4; k++) out[l++] = num[k];
    while (*unit && l < cap - 1) out[l++] = *unit++;
    out[l] = 0;
}
static void enumerate_disks(void) {
    g_disk_count = 0;
    for (int i = 0; i < 4; i++) {
        disk_info_t di;
        if (get_disk_info(i, &di) == 0 && di.present) g_disks[g_disk_count++] = di;
    }
}

// ---- MICO .ICN icon loader + alpha blitter --------------------------------
// MICO format: 12-byte header ('MICO' + width u32 LE + height u32 LE), then
// width*height*4 bytes BGRA. We cache a small set of loaded icons (white glyphs)
// and alpha-composite them, scaled with nearest-neighbour, over the window
// content via gui_draw_pixel. Optionally tinted: the icon's grey value is used
// as coverage and recoloured to the requested ink so glyphs stay theme-aware.
#define MICO_DIM   64
#define MICO_CACHE 24
typedef struct {
    char     name[16];   // cache key (basename, no ".ICN")
    int      w, h;
    int      loaded;     // 1=present, -1=tried-and-missing, 0=empty slot
    uint8_t  px[MICO_DIM * MICO_DIM * 4];   // BGRA
} mico_icon_t;
static mico_icon_t g_mico[MICO_CACHE];
static int g_mico_count = 0;

static mico_icon_t *mico_get(const char *name) {
    for (int i = 0; i < g_mico_count; i++)
        if (str_eq(g_mico[i].name, name)) return &g_mico[i];
    if (g_mico_count >= MICO_CACHE) return NULL;
    mico_icon_t *ic = &g_mico[g_mico_count++];
    str_copy(ic->name, name, 16);
    ic->loaded = -1; ic->w = ic->h = 0;
    char path[48]; int l = 0;
    const char *p = "/ICONS/"; while (*p) path[l++] = *p++;
    for (int i = 0; name[i] && l < 40; i++) path[l++] = name[i];
    const char *e = ".ICN"; while (*e) path[l++] = *e++;
    path[l] = 0;
    int fd = open(path, 0);
    if (fd < 0) return ic;
    uint8_t hdr[12];
    if (read(fd, (char *)hdr, 12) != 12 ||
        hdr[0] != 'M' || hdr[1] != 'I' || hdr[2] != 'C' || hdr[3] != 'O') {
        close(fd); return ic;
    }
    int w = hdr[4] | (hdr[5] << 8) | (hdr[6] << 16) | (hdr[7] << 24);
    int h = hdr[8] | (hdr[9] << 8) | (hdr[10] << 16) | (hdr[11] << 24);
    if (w <= 0 || h <= 0 || w > MICO_DIM || h > MICO_DIM) { close(fd); return ic; }
    int want = w * h * 4, got = 0;
    while (got < want) {
        int n = read(fd, (char *)ic->px + got, want - got);
        if (n <= 0) break;
        got += n;
    }
    close(fd);
    if (got != want) return ic;
    ic->w = w; ic->h = h; ic->loaded = 1;
    return ic;
}

// Draw a cached icon scaled into size x size at (x,y). If tint!=0 the icon's
// luminance is used as coverage and recoloured to tint; otherwise native BGRA.
// Returns 1 if the icon was drawn, 0 if not present (caller can fall back).
static int draw_mico(const char *name, int x, int y, int size, uint32_t tint) {
    mico_icon_t *ic = mico_get(name);
    if (!ic || ic->loaded != 1 || size <= 0) return 0;
    int tr = (tint >> 16) & 0xFF, tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;
    for (int dy = 0; dy < size; dy++) {
        int sy = (dy * ic->h) / size; if (sy >= ic->h) sy = ic->h - 1;
        for (int dx = 0; dx < size; dx++) {
            int sx = (dx * ic->w) / size; if (sx >= ic->w) sx = ic->w - 1;
            const uint8_t *s = &ic->px[(sy * ic->w + sx) * 4];
            int b = s[0], g = s[1], r = s[2], a = s[3];
            if (a == 0) continue;
            int cr, cg, cb;
            if (tint) {
                int cov = (r * 30 + g * 59 + b * 11) / 100;   // white glyph -> coverage
                a = (a * cov) / 255;
                if (a == 0) continue;
                cr = tr; cg = tg; cb = tb;
            } else { cr = r; cg = g; cb = b; }
            int px = x + dx, py = y + dy;
            if (a >= 250) {
                gui_draw_pixel(window_handle, px, py, (cr << 16) | (cg << 8) | cb);
            } else {
                // No read-back available; approximate by blending toward window bg.
                int br = (BG_COLOR >> 16) & 0xFF, bgc = (BG_COLOR >> 8) & 0xFF, bb = BG_COLOR & 0xFF;
                int rr = (cr * a + br  * (255 - a)) / 255;
                int rg = (cg * a + bgc * (255 - a)) / 255;
                int rb = (cb * a + bb  * (255 - a)) / 255;
                gui_draw_pixel(window_handle, px, py, (rr << 16) | (rg << 8) | rb);
            }
        }
    }
    return 1;
}

// Map a file entry to its glyph icon name. dir => folder (open folder when sel).
static const char *icon_for_entry(const char *name, int is_dir, int selected) {
    if (is_dir) return selected ? "OPENFLDR" : "FOLDER";
    char e[8]; ext_of(name, e);
    if (str_eq(e,"c")||str_eq(e,"h")||str_eq(e,"cpp")||str_eq(e,"asm")||str_eq(e,"js")) return "CODEFILE";
    if (str_eq(e,"py")) return "PYFILE";
    if (str_eq(e,"rb")) return "RBFILE";
    if (str_eq(e,"pdf")) return "PDFFILE";
    if (str_eq(e,"doc")||str_eq(e,"docx")||str_eq(e,"rtf")) return "WORDFILE";
    if (str_eq(e,"xls")||str_eq(e,"xlsx")||str_eq(e,"csv")) return "XLSFILE";
    if (str_eq(e,"ppt")||str_eq(e,"pptx")) return "PPTFILE";
    if (str_eq(e,"one")) return "ONEFILE";
    if (str_eq(e,"txt")||str_eq(e,"md")||str_eq(e,"cfg")||str_eq(e,"ini")||
        str_eq(e,"log")||str_eq(e,"yml")||str_eq(e,"yaml")) return "DOC";
    if (is_image_ext(e)) return "FILE";
    return "FILE";
}

// ---- icons ----------------------------------------------------------------
// (#704) The literal colors from here down through draw_file_icon() are
// INTENTIONALLY NOT themed: they are file-type / icon-glyph identity colors
// (folder gold, image green, code blue, media purple, archive orange, the
// generic-file grey), the same category as a syntax highlighter's palette
// or a chart series. A folder icon needs to keep reading as "gold folder"
// regardless of the active theme, the same way it does in every other
// desktop OS; recoloring it per-theme would make file types LESS
// identifiable, not more consistent. These are also mostly fallback glyphs
// (draw_mico() failed to find the real icon-font asset), used rarely on a
// system with a complete icon set.
static void draw_folder_icon(int x, int y) {
    win_draw_rect(window_handle, x, y + 2, 6, 4, ICON_FOLDER);
    win_draw_rect(window_handle, x, y + 4, ICON_SIZE, ICON_SIZE - 4, ICON_FOLDER);
    gui_draw_rect_outline(window_handle, x, y + 4, ICON_SIZE, ICON_SIZE - 4, 0x00B89000);
}
static uint32_t file_type_color(const char *name) {
    char e[8]; ext_of(name, e);
    if (is_image_ext(e)) return 0x0039A85B;
    if (str_eq(e,"c")||str_eq(e,"h")||str_eq(e,"py")||str_eq(e,"sh")||str_eq(e,"asm")||str_eq(e,"js")) return 0x003A78D6;
    if (str_eq(e,"wav")||str_eq(e,"mp3")||str_eq(e,"ogg")||str_eq(e,"mp4")||str_eq(e,"avi")) return 0x009B59B6;
    if (str_eq(e,"zip")||str_eq(e,"tar")||str_eq(e,"gz")||str_eq(e,"wad")||str_eq(e,"elf")) return 0x00E08020;
    if (str_eq(e,"cfg")||str_eq(e,"ini")||str_eq(e,"yml")||str_eq(e,"yaml")||str_eq(e,"db")) return 0x00808890;
    return 0x005A6270;
}
static void draw_file_icon(int x, int y, uint32_t accent) {
    win_draw_rect(window_handle, x + 2, y, ICON_SIZE - 4, ICON_SIZE, 0x00FFFFFF);
    win_draw_rect(window_handle, x + ICON_SIZE - 6, y, 4, 4, accent);
    gui_draw_rect_outline(window_handle, x + 2, y, ICON_SIZE - 4, ICON_SIZE, accent);
    win_draw_rect(window_handle, x + 2, y + 1, ICON_SIZE - 4, 3, accent);
    win_draw_rect(window_handle, x + 4, y + 7, 6, 1, 0x00808080);
    win_draw_rect(window_handle, x + 4, y + 10, 8, 1, 0x00808080);
}

// Simple chevron/arrow glyphs for nav buttons (drawn with small rects).
static void draw_arrow(int x, int y, int dir, uint32_t c) {  // 0=left 1=right 2=up
    if (dir == 0) for (int i = 0; i < 5; i++) win_draw_rect(window_handle, x + 4 - i, y + 4 - i, 2, 2 + i * 2, c);
    else if (dir == 1) for (int i = 0; i < 5; i++) win_draw_rect(window_handle, x + i, y + 4 - i, 2, 2 + i * 2, c);
    else for (int i = 0; i < 5; i++) win_draw_rect(window_handle, x + 4 - i, y + i, 2 + i * 2, 2, c);
}

// #554: format the Attr details-view column + the Properties permissions
// text. ext2 gets a real symbolic rwxrwxrwx mode string (perms.c-backed);
// FAT gets the real on-disk attribute flags in the SAME letter order the
// kernel itself already uses for its own diagnostic listing (fs/fat.c
// fat_list_dir_inner: D,R,H,S,A) so the two never drift apart; SMB/NFS (no
// local permission model) show a plain dash rather than a guess.
static void fmt_attr(const file_entry_t *it, char *out) {
    if (it->fs_type == FSPERM_TYPE_POSIX) {
        static const char rwx[3] = {'r', 'w', 'x'};
        for (int grp = 0; grp < 3; grp++) {
            uint16_t bits = (it->mode >> ((2 - grp) * 3)) & 7;
            for (int b = 0; b < 3; b++) out[grp * 3 + b] = (bits & (4 >> b)) ? rwx[b] : '-';
        }
        out[9] = '\0';
    } else if (it->fs_type == FSPERM_TYPE_FAT) {
        out[0] = (it->fat_attr & FSPERM_FAT_DIRECTORY) ? 'D' : '-';
        out[1] = (it->fat_attr & FSPERM_FAT_READONLY)  ? 'R' : '-';
        out[2] = (it->fat_attr & FSPERM_FAT_HIDDEN)    ? 'H' : '-';
        out[3] = (it->fat_attr & FSPERM_FAT_SYSTEM)    ? 'S' : '-';
        out[4] = (it->fat_attr & FSPERM_FAT_ARCHIVE)   ? 'A' : '-';
        out[5] = '\0';
    } else {
        out[0] = '-'; out[1] = '\0';
    }
}

// ---- size formatting ------------------------------------------------------
static void fmt_size(uint32_t sz, char *out) {
    char num[24];
    if (sz < 1024) { gui_itoa(sz, num, 16); int l = str_len(num); num[l]=' '; num[l+1]='B'; num[l+2]=0; }
    else if (sz < 1024 * 1024) { gui_itoa(sz / 1024, num, 16); int l = str_len(num); num[l]=' '; num[l+1]='K'; num[l+2]='B'; num[l+3]=0; }
    else { gui_itoa(sz / (1024 * 1024), num, 16); int l = str_len(num); num[l]=' '; num[l+1]='M'; num[l+2]='B'; num[l+3]=0; }
    str_copy(out, num, 24);
}

// ---- directory loading + sort + filter ------------------------------------
static void sort_items(void) {
    for (int i = 1; i < item_count; i++) {
        file_entry_t key = items[i];
        int j = i - 1;
        while (j >= 0) {
            file_entry_t *a = &items[j];
            // ".." always first, dirs before files, then by sort key
            int swap = 0;
            int a_dd = str_eq(a->name, "..") ? 0 : 1;
            int k_dd = str_eq(key.name, "..") ? 0 : 1;
            if (a_dd != k_dd) swap = (a_dd > k_dd);
            else if (a->is_directory != key.is_directory) swap = (!a->is_directory && key.is_directory);
            else {
                if (g_sort == SORT_SIZE) swap = (a->size > key.size);
                else if (g_sort == SORT_TYPE) { char ea[8], ek[8]; ext_of(a->name, ea); ext_of(key.name, ek);
                    int c = 0; while (ea[c] && ea[c] == ek[c]) c++; swap = ((unsigned char)ea[c] > (unsigned char)ek[c]); }
                else if (g_sort == SORT_ATTR) {
                    // #554: compare within the same fs_type first (ext2 mode
                    // bits and FAT attribute bits are not the same scale), so
                    // a mixed listing degrades to fs_type order rather than an
                    // apples-to-oranges numeric compare.
                    if (a->fs_type != key.fs_type) swap = (a->fs_type > key.fs_type);
                    else if (a->fs_type == FSPERM_TYPE_FAT) swap = (a->fat_attr > key.fat_attr);
                    else swap = (a->mode > key.mode);
                }
                else { int c = 0; char ca, cb;
                    do { ca = a->name[c]; cb = key.name[c]; if (ca>='A'&&ca<='Z')ca+=32; if (cb>='A'&&cb<='Z')cb+=32; c++; }
                    while (ca && ca == cb); swap = ((unsigned char)ca > (unsigned char)cb); }
            }
            if (!swap) break;
            items[j + 1] = items[j]; j--;
        }
        items[j + 1] = key;
    }
}

// ---- #317 Network locations (saved SMB mounts) ----------------------------
// Persisted to /CONFIG/NETMOUNTS.CFG (one "label|server|share|user|pass" per
// line) on the ext2 root volume, so they survive reboot. The virtual "/NET"
// folder lists these plus an "Add Network Location" entry; opening one mounts
// the share (net_mount syscall) and navigates into "/SMB/<server>/<share>".
#define NETMOUNTS_CFG "/CONFIG/NETMOUNTS.CFG"
#define MAX_NETMOUNTS 24
typedef struct {
    char label[40];
    char server[64];
    char share[40];
    char user[40];
    char pass[40];
} netmount_t;
static netmount_t g_netmounts[MAX_NETMOUNTS];
static int g_netmount_count = 0;

static void nm_field(const char *src, int *pi, char *out, int outsz) {
    int i = *pi, o = 0;
    while (src[i] && src[i] != '|' && src[i] != '\n' && o < outsz - 1) out[o++] = src[i++];
    out[o] = 0;
    if (src[i] == '|') i++;
    *pi = i;
}

static void load_netmounts(void) {
    g_netmount_count = 0;
    int fd = open(NETMOUNTS_CFG, 0);
    if (fd < 0) return;
    static char buf[4096];
    int n = 0, r;
    while ((r = read(fd, buf + n, sizeof(buf) - 1 - n)) > 0) {
        n += r; if (n >= (int)sizeof(buf) - 1) break;
    }
    close(fd);
    buf[n] = 0;
    int i = 0;
    while (buf[i] && g_netmount_count < MAX_NETMOUNTS) {
        if (buf[i] == '\n' || buf[i] == '\r') { i++; continue; }
        if (buf[i] == '#') { while (buf[i] && buf[i] != '\n') i++; continue; }
        netmount_t *m = &g_netmounts[g_netmount_count];
        nm_field(buf, &i, m->label,  sizeof(m->label));
        nm_field(buf, &i, m->server, sizeof(m->server));
        nm_field(buf, &i, m->share,  sizeof(m->share));
        nm_field(buf, &i, m->user,   sizeof(m->user));
        nm_field(buf, &i, m->pass,   sizeof(m->pass));
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i] == '\n') i++;
        if (m->server[0] && m->share[0]) g_netmount_count++;
    }
}

static void save_netmounts(void) {
    mkdir("/CONFIG", 0755);
    int fd = open(NETMOUNTS_CFG, 0x41);   // O_CREAT|O_WRONLY (whole-file rewrite)
    if (fd < 0) return;
    for (int k = 0; k < g_netmount_count; k++) {
        netmount_t *m = &g_netmounts[k];
        const char *flds[5]; flds[0]=m->label; flds[1]=m->server; flds[2]=m->share;
        flds[3]=m->user; flds[4]=m->pass;
        char line[280]; int l = 0;
        for (int f = 0; f < 5; f++) {
            for (int j = 0; flds[f][j] && l < 270; j++) line[l++] = flds[f][j];
            line[l++] = (f < 4) ? '|' : '\n';
        }
        write(fd, line, l);
    }
    close(fd);
}

static void netmount_add(const char *server, const char *share,
                         const char *user, const char *pass) {
    load_netmounts();
    if (g_netmount_count >= MAX_NETMOUNTS) return;
    netmount_t *m = &g_netmounts[g_netmount_count];
    int l = 0;
    for (int i = 0; share[i] && l < 38; i++) m->label[l++] = share[i];
    if (l < 38) m->label[l++] = '@';
    for (int i = 0; server[i] && l < 39; i++) m->label[l++] = server[i];
    m->label[l] = 0;
    str_copy(m->server, server, sizeof(m->server));
    str_copy(m->share,  share,  sizeof(m->share));
    str_copy(m->user,   user,   sizeof(m->user));
    str_copy(m->pass,   pass,   sizeof(m->pass));
    g_netmount_count++;
    save_netmounts();
}

// #317: the "[+ Add Network Location]" row now opens the proper credentials
// dialog (open_cred_dialog, defined near draw_te below), replacing the old
// one-line "server share user pass" text entry that put the password in
// cleartext with no masking and no per-field UI.
void open_add_network(void) {
    open_cred_dialog(0, 0, 0);   // empty; user fills in Server/Share/creds
}

// #554: zero the permission/attribute fields for a freshly-pushed items[i] -
// items[] is a static array reused across directory loads, so a slot not
// explicitly re-stamped here would show whatever fs_type/mode/fat_attr a
// PREVIOUS, unrelated directory's entry left behind at that index.
static void item_clear_perm(int i) {
    items[i].fs_type = FSPERM_TYPE_OTHER;
    items[i].has_perm_entry = 0;
    items[i].fat_attr = 0;
    items[i].mode = 0;
    items[i].uid = 0;
    items[i].gid = 0;
}
// Fetch the real filesystem-aware permission/attribute info for items[i],
// which must already have its name set; dirpath is the containing directory
// (CUR.path at load time - a local path, never SMB, so this is skipped for
// the SMB readdir loop, which has no local permission model anyway).
static void item_fetch_perm(int i, const char *dirpath) {
    char full[MAX_PATH_LEN];
    path_join(full, dirpath, items[i].name);
    fsperm_info_t fi;
    if (sys_fs_perm_info(full, &fi) == 0) {
        items[i].fs_type        = fi.fs_type;
        items[i].has_perm_entry = fi.has_perm_entry;
        items[i].fat_attr       = fi.fat_attr;
        items[i].mode           = fi.mode;
        items[i].uid            = fi.uid;
        items[i].gid            = fi.gid;
    }
}
// #554 hidden-file filter: a FAT ATTR_HIDDEN entry, or a UNIX-convention
// dotfile (name starts with '.', excluding the synthesized ".." row).
static bool item_is_hidden(int i) {
    if (items[i].fs_type == FSPERM_TYPE_FAT && (items[i].fat_attr & FSPERM_FAT_HIDDEN)) return true;
    if (items[i].name[0] == '.' && !str_eq(items[i].name, "..")) return true;
    return false;
}

// ---- #netshares: automatic LAN share discovery listing --------------------
// The virtual "/NET" folder is built from four sources: the two control rows
// (Add / Rescan), a status row, the user's saved mounts, and the live results
// of the async subnet sweep in netscan.c. Each row carries an action kind plus
// its target so open_selected() dispatches without re-parsing display text.
enum { NR_NONE = 0, NR_ADD, NR_RESCAN, NR_STATUS, NR_SAVED, NR_DISC_SHARE, NR_DISC_SMB, NR_DISC_NFS };
typedef struct { unsigned char kind; char server[64]; char share[MAX_NAME_LEN]; char user[40]; char pass[40]; char export[MAX_PATH_LEN]; } net_row_t;
static net_row_t g_net_rows[MAX_ITEMS];

static void push_net_row(const char *name, bool is_dir, unsigned char kind,
                         const char *server, const char *share) {
    if (item_count >= MAX_ITEMS) return;
    str_copy(items[item_count].name, name, MAX_NAME_LEN);
    items[item_count].is_directory = is_dir;
    items[item_count].size = 0;
    item_clear_perm(item_count);
    net_row_t *r = &g_net_rows[item_count];
    r->kind = kind; r->server[0] = 0; r->share[0] = 0; r->user[0] = 0; r->pass[0] = 0; r->export[0] = 0;
    if (server) str_copy(r->server, server, sizeof(r->server));
    if (share)  str_copy(r->share,  share,  sizeof(r->share));
    item_count++;
}

// Rebuild the /NET item list from the current discovery state. Does NOT touch
// history and preserves CUR.sel/scroll (clamped), so a live refresh mid-scan
// does not fight the user. Safe to call on every scan-progress tick.
static void build_net_listing(void) {
    item_count = 0;
    int st = ns_state();

    push_net_row("[+ Add Network Location]", false, NR_ADD, 0, 0);
    push_net_row(st == NS_SCANNING ? "[x Stop scan]" : "[o Rescan network]",
                 false, NR_RESCAN, 0, 0);

    char stat[96];
    if (st == NS_SCANNING) {
        int d = 0, t = 0; int sv = ns_progress(&d, &t);
        snprintf(stat, sizeof(stat), "Scanning LAN %d/%d - %d found", d, t, sv);
    } else if (st == NS_DONE && !ns_available()) {
        snprintf(stat, sizeof(stat), "Network unavailable (no carrier or address)");
    } else if (st == NS_DONE) {
        int sv = ns_server_count();
        snprintf(stat, sizeof(stat), "Scan complete - %d server%s found", sv, sv == 1 ? "" : "s");
    } else {
        snprintf(stat, sizeof(stat), "Rescan to discover shares on your network");
    }
    push_net_row(stat, false, NR_STATUS, 0, 0);

    load_netmounts();
    for (int k = 0; k < g_netmount_count && item_count < MAX_ITEMS; k++) {
        push_net_row(g_netmounts[k].label, true, NR_SAVED,
                     g_netmounts[k].server, g_netmounts[k].share);
        str_copy(g_net_rows[item_count - 1].user, g_netmounts[k].user, 40);
        str_copy(g_net_rows[item_count - 1].pass, g_netmounts[k].pass, 40);
    }

    int nsv = ns_server_count();
    for (int i = 0; i < nsv && item_count < MAX_ITEMS; i++) {
        ns_server_t s;
        if (ns_get_server(i, &s) != 0) continue;
        if (s.smb) {
            if (s.nshares > 0) {
                for (int j = 0; j < s.nshares && item_count < MAX_ITEMS; j++) {
                    char label[MAX_NAME_LEN];
                    snprintf(label, sizeof(label), "%s on %s", s.shares[j], s.ip_str);
                    push_net_row(label, true, NR_DISC_SHARE, s.ip_str, s.shares[j]);
                }
            } else {
                char label[MAX_NAME_LEN];
                snprintf(label, sizeof(label), "%s (SMB, no shares listed)", s.ip_str);
                push_net_row(label, true, NR_DISC_SMB, s.ip_str, 0);
            }
        }
        if (s.nfs) {
            if (s.nexports > 0) {
                for (int j = 0; j < s.nexports && item_count < MAX_ITEMS; j++) {
                    char label[MAX_NAME_LEN];
                    snprintf(label, sizeof(label), "%s on %s (NFS)", s.exports[j], s.ip_str);
                    push_net_row(label, true, NR_DISC_NFS, s.ip_str, 0);
                    str_copy(g_net_rows[item_count - 1].export, s.exports[j], MAX_PATH_LEN);
                }
            } else {
                char label[MAX_NAME_LEN];
                snprintf(label, sizeof(label), "%s (NFS server)", s.ip_str);
                push_net_row(label, false, NR_DISC_NFS, s.ip_str, 0);
            }
        }
    }

    if (CUR.sel >= item_count) CUR.sel = -1;
    if (CUR.scroll < 0) CUR.scroll = 0;
}

static void load_directory(const char *path) {
    bool was_net = str_eq(CUR.path, "/NET");
    bool now_net = str_eq(path, "/NET");
    str_copy(CUR.path, path, MAX_PATH_LEN);
    current_path = CUR.path;
    item_count = 0;
    if (CUR.sel >= 0) CUR.sel = -1;
    CUR.scroll = 0;

    // #netshares: entering the Network view kicks off an async LAN sweep;
    // leaving it cancels the worker (freeing its sockets). The listing itself
    // is built from live discovery state by build_net_listing().
    if (now_net && !was_net) ns_start();
    if (was_net && !now_net) ns_cancel();

    if (now_net) { build_net_listing(); return; }

    if (path[0] == '/' && path[1] != '\0') {
        str_copy(items[item_count].name, "..", MAX_NAME_LEN);
        items[item_count].is_directory = true; items[item_count].size = 0; item_clear_perm(item_count); item_count++;
    }
    // #317: for network (/SMB) paths, enumerate with a SINGLE open + sequential
    // fd-based readdir. The path-based sys_readdir() wrapper re-opens the dir for
    // every index (O(n^2) SMB round-trips), which is slow and fragile over the
    // network; one open + a readdir loop reuses one SMB dir handle and is robust.
    if (path[0]=='/' && (path[1]=='S'||path[1]=='s') && (path[2]=='M'||path[2]=='m') &&
        (path[3]=='B'||path[3]=='b') && path[4]=='/') {
        int fd = open(path, 0);
        if (fd >= 0) {
            dirent_t entry;
            while (item_count < MAX_ITEMS && sys_readdir_raw(fd, &entry) == 0) {
                if (g_filter[0] && !ci_has(entry.name, g_filter)) continue;
                str_copy(items[item_count].name, entry.name, MAX_NAME_LEN);
                items[item_count].is_directory = (entry.type == 1);
                items[item_count].size = entry.size;
                // #554: SMB has no local permission model (enforced server-side);
                // fs_type stays FSPERM_TYPE_OTHER, no extra round trip per file.
                item_clear_perm(item_count);
                item_count++;
            }
            close(fd);
        }
        sort_items();
        if (item_count == 0) {
            str_copy(items[0].name, "(empty)", MAX_NAME_LEN);
            items[0].is_directory = false; items[0].size = 0; item_clear_perm(0); item_count = 1;
        }
        return;
    }
    dirent_t entry; int index = 0;
    while (item_count < MAX_ITEMS) {
        int r = sys_readdir(path, index, &entry);
        if (r != 0) break;
        index++;
        // apply filter (keep ".." always)
        if (g_filter[0] && !ci_has(entry.name, g_filter)) continue;
        str_copy(items[item_count].name, entry.name, MAX_NAME_LEN);
        items[item_count].is_directory = (entry.type == 1);
        items[item_count].size = entry.size;
        // #554: real, filesystem-aware permission/attribute info (ext2 mode
        // via perms.c, or the native FAT attribute byte - never both, never
        // fabricated; see item_fetch_perm's doc comment).
        item_fetch_perm(item_count, path);
        if (!g_show_hidden && item_is_hidden(item_count)) continue;
        item_count++;
    }
    sort_items();
    if (item_count == 0) {
        str_copy(items[0].name, "(empty)", MAX_NAME_LEN);
        items[0].is_directory = false; items[0].size = 0; item_clear_perm(0); item_count = 1;
    }
}

// ---- navigation -----------------------------------------------------------
static void navigate_to(const char *path) {
    g_in_recycle = 0;
    if (CUR.path[0] && !str_eq(CUR.path, path)) {
        if (CUR.back_n < MAX_HIST) str_copy(CUR.back[CUR.back_n++], CUR.path, MAX_PATH_LEN);
        else { for (int i = 1; i < MAX_HIST; i++) str_copy(CUR.back[i-1], CUR.back[i], MAX_PATH_LEN);
               str_copy(CUR.back[MAX_HIST-1], CUR.path, MAX_PATH_LEN); }
        CUR.fwd_n = 0;   // new branch clears forward
    }
    load_directory(path);
    fb_redraw();
}
static void navigate_back(void) {
    g_in_recycle = 0;
    if (CUR.back_n <= 0) return;
    if (CUR.fwd_n < MAX_HIST) str_copy(CUR.fwd[CUR.fwd_n++], CUR.path, MAX_PATH_LEN);
    char prev[MAX_PATH_LEN]; str_copy(prev, CUR.back[--CUR.back_n], MAX_PATH_LEN);
    load_directory(prev); fb_redraw();
}
static void navigate_fwd(void) {
    g_in_recycle = 0;
    if (CUR.fwd_n <= 0) return;
    if (CUR.back_n < MAX_HIST) str_copy(CUR.back[CUR.back_n++], CUR.path, MAX_PATH_LEN);
    char nx[MAX_PATH_LEN]; str_copy(nx, CUR.fwd[--CUR.fwd_n], MAX_PATH_LEN);
    load_directory(nx); fb_redraw();
}
static void navigate_up(void) {
    g_in_recycle = 0;
    int len = str_len(CUR.path);
    if (len <= 1) return;
    len--; while (len > 0 && CUR.path[len] != '/') len--;
    char parent[MAX_PATH_LEN];
    if (len == 0) { parent[0] = '/'; parent[1] = 0; }
    else { for (int i = 0; i < len; i++) parent[i] = CUR.path[i]; parent[len] = 0; }
    navigate_to(parent);
}

// ---- Recycle Bin (integrated trash view) ----------------------------------
// Ported from the standalone recyclebin app: it reads the real trash backend
// (TRASH_DIR + TRASH_INDEX), the same store that this app's do_delete() writes
// to. When g_in_recycle is set, the content area shows the trash list plus a
// Restore / Delete Permanently / Empty Bin action row instead of a directory.
#define TRASH_DIR    "/CONFIG/RECYCLE"
#define TRASH_INDEX  "/CONFIG/RBINDEX.TXT"
#define RB_MAX_ITEMS 128

typedef struct {
    char name[64];
    char original_path[128];
    uint32_t size;
    int  selected;
} rb_item_t;

static rb_item_t rb_items[RB_MAX_ITEMS];
static int       rb_count = 0;
static int       rb_scroll = 0;
static int       rb_hover = -1;

static char rb_idx_buf[8192];
static int  rb_idx_len = 0;

static void rb_join(char *out, int outsz, const char *dir, const char *name) {
    int j = 0;
    for (int i = 0; dir[i] && j < outsz - 1; i++) out[j++] = dir[i];
    if (j > 0 && out[j-1] != '/' && j < outsz - 1) out[j++] = '/';
    for (int i = 0; name[i] && j < outsz - 1; i++) out[j++] = name[i];
    out[j] = 0;
}

static void rb_idx_load(void) {
    rb_idx_len = 0; rb_idx_buf[0] = 0;
    int fd = open(TRASH_INDEX, 0);
    if (fd < 0) return;
    char tmp[512]; int n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0)
        for (int i = 0; i < n && rb_idx_len < (int)sizeof(rb_idx_buf) - 1; i++) rb_idx_buf[rb_idx_len++] = tmp[i];
    rb_idx_buf[rb_idx_len] = 0;
    close(fd);
}

// Look up "name" in the index; copies the original path (text after the '|') out.
static int rb_idx_lookup(const char *name, char *out, int outsz) {
    int i = 0;
    while (i < rb_idx_len) {
        int ls = i; while (i < rb_idx_len && rb_idx_buf[i] != '\n') i++;
        int le = i; if (i < rb_idx_len) i++;
        int bar = ls; while (bar < le && rb_idx_buf[bar] != '|') bar++;
        if (bar < le) {
            int k = ls, t = 0, m = 1;
            while (k < bar) { if (rb_idx_buf[k] != name[t]) { m = 0; break; } k++; t++; }
            if (m && name[t] == 0) { int o = 0, p = bar + 1; while (p < le && o < outsz - 1) out[o++] = rb_idx_buf[p++]; out[o] = 0; return 1; }
        }
    }
    return 0;
}

// Remove the index line for "name" and rewrite TRASH_INDEX. Returns 0 only if
// the rewritten index reached the medium.
//
// #742: this used to unlink(TRASH_INDEX) FIRST and then hope the create
// succeeded. If it did not, the restore information for EVERY item in the bin
// was gone, not just this one, and nothing said so. O_TRUNC does the same job
// in one operation with no window in which the index does not exist.
static int rb_idx_remove(const char *name) {
    char nb[8192]; int nl = 0; int i = 0;
    while (i < rb_idx_len) {
        int ls = i; while (i < rb_idx_len && rb_idx_buf[i] != '\n') i++;
        int le = i; if (i < rb_idx_len) i++;
        int bar = ls; while (bar < le && rb_idx_buf[bar] != '|') bar++;
        int match = 0;
        if (bar < le) { int k = ls, t = 0, m = 1; while (k < bar) { if (rb_idx_buf[k] != name[t]) { m = 0; break; } k++; t++; } if (m && name[t] == 0) match = 1; }
        if (!match) { for (int j = ls; j < le && nl < (int)sizeof(nb) - 1; j++) nb[nl++] = rb_idx_buf[j]; if (nl < (int)sizeof(nb) - 1) nb[nl++] = '\n'; }
    }
    int fd = open(TRASH_INDEX, 0x241);   // O_WRONLY|O_CREAT|O_TRUNC
    if (fd < 0) return -1;
    int rc = 0;
    if (nl && write(fd, nb, nl) != nl) rc = -1;
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    if (rc != 0) return rc;              // keep the in-RAM copy consistent with disk
    for (int j = 0; j < nl; j++) rb_idx_buf[j] = nb[j];
    rb_idx_len = nl; rb_idx_buf[nl] = 0;
    return 0;
}

static void rb_load(void) {
    rb_count = 0; rb_scroll = 0; rb_hover = -1;
    mkdir(TRASH_DIR, 0755);
    rb_idx_load();
    dirent_t entry; int index = 0;
    while (rb_count < RB_MAX_ITEMS) {
        int r = sys_readdir(TRASH_DIR, index, &entry);
        if (r != 0) break;
        index++;
        if (entry.name[0] == '.') continue;
        if (str_eq(entry.name, "..")) continue;
        if (str_eq(entry.name, "RBINDEX.TXT")) continue;
        if (entry.type == 1) continue;   // skip directories
        rb_item_t *it = &rb_items[rb_count];
        str_copy(it->name, entry.name, sizeof(it->name));
        if (!rb_idx_lookup(entry.name, it->original_path, sizeof(it->original_path)))
            str_copy(it->original_path, "(unknown)", sizeof(it->original_path));
        it->size = entry.size;
        it->selected = 0;
        rb_count++;
    }
}

static int rb_count_selected(void) {
    int c = 0; for (int i = 0; i < rb_count; i++) if (rb_items[i].selected) c++; return c;
}

// Move every selected trashed file back to its recorded original location.
// #742: every step is checked. An item whose restore FAILED keeps its index
// entry and therefore stays in the bin and stays visible, which is the whole
// point of a bin: the previous code dropped the index entry unconditionally, so
// a failed restore made the file vanish from the UI while still sitting in
// /CONFIG/RECYCLE with no record of where it belonged.
static void rb_restore_selected(void) {
    int failed = 0, done = 0;
    files_err_clear();
    for (int i = rb_count - 1; i >= 0; i--) {
        if (!rb_items[i].selected) continue;
        char src[200]; rb_join(src, sizeof(src), TRASH_DIR, rb_items[i].name);
        int ok = 0;
        if (rb_items[i].original_path[0] && !str_eq(rb_items[i].original_path, "(unknown)")) {
            if (rename(src, rb_items[i].original_path) == 0) ok = 1;
            else if (copy_file(src, rb_items[i].original_path) == 0) {
                // copy_file() fsync'd, so the restored copy is real before the
                // bin copy goes.
                ok = (unlink(src) == 0);
            }
        }
        if (!ok) { failed++; continue; }          // leave it in the bin, visible
        if (rb_idx_remove(rb_items[i].name) != 0) failed++;
        else done++;
    }
    if (failed) {
        files_error(done ? "Some items could not be restored and are still in the Recycle Bin"
                         : "Could not restore; the item is still in the Recycle Bin", 0);
    }
    rb_load();
}

// Permanently unlink every selected trashed file. #742: a failed unlink used to
// still drop the index entry, so the file stayed on disk forever, invisible and
// unrestorable, and the user was shown a successful delete.
static void rb_delete_selected(void) {
    int failed = 0;
    files_err_clear();
    for (int i = rb_count - 1; i >= 0; i--) {
        if (!rb_items[i].selected) continue;
        char p[200]; rb_join(p, sizeof(p), TRASH_DIR, rb_items[i].name);
        if (unlink(p) != 0) { failed++; continue; }   // keep the index entry
        if (rb_idx_remove(rb_items[i].name) != 0) failed++;
    }
    if (failed) files_error("Some items could not be deleted and are still in the Recycle Bin", 0);
    rb_load();
}

static void rb_empty(void) {
    int failed = 0;
    files_err_clear();
    for (int i = 0; i < rb_count; i++) {
        char p[200]; rb_join(p, sizeof(p), TRASH_DIR, rb_items[i].name);
        if (unlink(p) != 0) failed++;
    }
    if (failed) {
        // #742: do NOT wipe the index while files remain. Their entries are the
        // only record of where they came from; dropping the index turns a
        // recoverable failure into a permanent one.
        files_error("Could not empty the Recycle Bin; some items remain", 0);
    } else {
        int fd = open(TRASH_INDEX, 0x241);   // O_WRONLY|O_CREAT|O_TRUNC
        int rc = (fd < 0) ? -1 : 0;
        if (rc == 0 && fsync(fd) != 0) rc = -1;
        if (fd >= 0 && close(fd) != 0) rc = -1;
        if (rc != 0) files_error("Recycle Bin emptied, but the index could not be cleared", TRASH_INDEX);
        else { rb_idx_len = 0; rb_idx_buf[0] = 0; }
    }
    rb_load();
}

// #745 (docs/CONFIRM_MODAL_DESIGN.html): both actions above used to fire with
// NO confirmation at all - the audit's single biggest finding ("permanently
// destroying files has less friction than shutting the machine down"). Now
// gated behind the shared window-modal confirm card (libc/gui_style.h's
// gui_confirm_singleton_*), the same component Task Manager's End Task/Kill
// uses. g_rb_pending_action: 0 none, 1 = delete selected, 2 = empty bin.
static int g_rb_pending_action;

static void rb_confirm_delete_selected(void) {
    int n = rb_count_selected();
    if (!n) return;
    char body[96];
    strcpy(body, "Permanently delete the selected item");
    strcat(body, n == 1 ? "" : "s");
    strcat(body, "? This cannot be undone.");
    g_rb_pending_action = 1;
    gui_confirm_open_s(GUI_CONFIRM_DESTRUCTIVE, "Delete Permanently",
                       body, 0, 0, 1, "Cancel", "Delete");
    fb_redraw();
}

static void rb_confirm_empty(void) {
    if (!rb_count) return;
    char body[96];
    strcpy(body, "Permanently delete all ");
    { char n[12]; int v = rb_count, i = 0, t; char tmp[12];
      if (v == 0) tmp[i++] = '0'; else { t = 0; while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
      while (t > 0) n[i++] = tmp[--t]; }
      n[i] = 0; strcat(body, n); }
    strcat(body, " items in the Recycle Bin? This cannot be undone.");
    g_rb_pending_action = 2;
    gui_confirm_open_s(GUI_CONFIRM_DESTRUCTIVE, "Empty Recycle Bin",
                       body, 0, 0, 1, "Cancel", "Empty Bin");
    fb_redraw();
}

// ---- preview pane ---------------------------------------------------------
static unsigned char prev_buf[8192];
static int prev_len = 0;
static char prev_path[MAX_PATH_LEN] = "";
enum { PV_NONE, PV_TEXT, PV_HEX, PV_IMAGE };
static int prev_kind = PV_NONE;

// Decoded-thumbnail cache: a BMP is decoded + scaled into prev_thumb ONCE when
// the selection changes (keyed by path + size), then just blitted from RAM on
// every redraw - no more re-opening + re-reading + re-decoding the file from
// disk on each paint (that was the "constantly redrawing" preview).
#define THUMB_W_MAX 212
#define THUMB_H_MAX 470
static uint32_t prev_thumb[THUMB_W_MAX * THUMB_H_MAX];
static int  prev_thumb_w = 0, prev_thumb_h = 0;
static int  prev_thumb_ok = 0;
static char prev_thumb_key[MAX_PATH_LEN + 24] = "";

static unsigned char bmp_row[16384];   // one source row, up to 4096*4 bpp

// Decode the BMP at prev_path scaled to fit (maxw x maxh) into prev_thumb.
// Sets prev_thumb_w/h, prev_thumb_ok; returns 1 on success.
static int build_bmp_thumb(int maxw, int maxh) {
    prev_thumb_ok = 0;
    if (prev_len < 54 || prev_buf[0] != 'B' || prev_buf[1] != 'M') return 0;
    uint32_t off = prev_buf[10] | (prev_buf[11]<<8) | (prev_buf[12]<<16) | (prev_buf[13]<<24);
    int w = prev_buf[18] | (prev_buf[19]<<8) | (prev_buf[20]<<16) | (prev_buf[21]<<24);
    int h = prev_buf[22] | (prev_buf[23]<<8) | (prev_buf[24]<<16) | (prev_buf[25]<<24);
    int bpp = prev_buf[28] | (prev_buf[29]<<8);
    int flip = 1; if (h < 0) { h = -h; flip = 0; }
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return 0;
    if (bpp != 24 && bpp != 32) return 0;
    int bypp = bpp / 8;
    int row = (w * bypp + 3) & ~3;
    if (row > (int)sizeof(bmp_row)) return 0;
    if (maxw > THUMB_W_MAX) maxw = THUMB_W_MAX;
    if (maxh > THUMB_H_MAX) maxh = THUMB_H_MAX;
    if (maxw < 1 || maxh < 1) return 0;
    int dw = maxw, dh = maxh;                 // scale to fit, preserve aspect
    if (w * maxh > h * maxw) dh = (h * maxw) / w; else dw = (w * maxh) / h;
    if (dw < 1) dw = 1; if (dh < 1) dh = 1;
    if (dw > THUMB_W_MAX) dw = THUMB_W_MAX;
    if (dh > THUMB_H_MAX) dh = THUMB_H_MAX;
    int fd = open(prev_path, 0);
    if (fd < 0) return 0;
    int last_srcy = -1;
    for (int yy = 0; yy < dh; yy++) {
        int sy = (yy * h) / dh; int srcy = flip ? (h - 1 - sy) : sy;
        if (srcy != last_srcy) {
            lseek(fd, (long)off + (long)srcy * row, SEEK_SET);
            int got = 0;
            while (got < row) { int r = read(fd, (char *)bmp_row + got, row - got); if (r <= 0) break; got += r; }
            while (got < row) bmp_row[got++] = 0;
            last_srcy = srcy;
        }
        uint32_t *drow = prev_thumb + (uint32_t)yy * dw;
        for (int xx = 0; xx < dw; xx++) {
            int sx = (xx * w) / dw;
            uint32_t idx = (uint32_t)sx * bypp;
            if (idx + 2 >= (uint32_t)row) { drow[xx] = 0; continue; }
            drow[xx] = ((uint32_t)bmp_row[idx+2] << 16) | ((uint32_t)bmp_row[idx+1] << 8) | bmp_row[idx];
        }
    }
    close(fd);
    prev_thumb_w = dw; prev_thumb_h = dh; prev_thumb_ok = 1;
    return 1;
}

static void load_preview(void) {
    prev_kind = PV_NONE; prev_len = 0; prev_path[0] = 0;
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (it->is_directory || str_eq(it->name, "(empty)")) return;
    path_join(prev_path, CUR.path, it->name);
    char e[8]; ext_of(it->name, e);
    int fd = open(prev_path, 0);
    if (fd < 0) return;
    prev_len = read(fd, prev_buf, sizeof(prev_buf));
    close(fd);
    if (prev_len < 0) prev_len = 0;
    if (is_image_ext(e)) {
        prev_kind = PV_IMAGE;
        // Cache identity = path + ":" + size. Decode only when it differs from
        // the cached thumbnail (so re-selecting the same file is free; an edit
        // that changes the size invalidates it).
        char key[MAX_PATH_LEN + 24];
        int n = 0; while (prev_path[n] && n < MAX_PATH_LEN) { key[n] = prev_path[n]; n++; }
        key[n++] = ':';
        unsigned long sz = (unsigned long)it->size; char ds[20]; int dn = 0;
        if (sz == 0) ds[dn++] = '0';
        while (sz) { ds[dn++] = (char)('0' + (sz % 10)); sz /= 10; }
        while (dn) key[n++] = ds[--dn];
        key[n] = 0;
        if (!(prev_thumb_ok && str_eq(key, prev_thumb_key))) {
            prev_thumb_ok = 0; prev_thumb_key[0] = 0;
            if (str_eq(e, "bmp") && build_bmp_thumb(PREVIEW_W - 2 * PANEL_IN, body_h() - 100)) {
                int k = 0; while (key[k] && k < (int)sizeof(prev_thumb_key) - 1) { prev_thumb_key[k] = key[k]; k++; }
                prev_thumb_key[k] = 0;
            }
        }
    }
    else if (is_text_ext(e)) prev_kind = PV_TEXT;
    else prev_kind = PV_HEX;
}

// (filesglass) The backdrop colour under content pixel (x, y): what the
// kernel's nearest-neighbour scale put there, to within the blur. Every AA
// edge and shadow drawn onto the backdrop takes its outer colour from here;
// a flat guess is what produces a square halo around a round corner.
static uint32_t bd_at(int x, int y) {
    return gui_glass_backdrop_at(g_bd, WIN_W, WIN_H, x, y);
}

// One glass panel: soft shadow, AA rounded fill, 1px border, 1px top
// highlight; the layering the Calculator's / Editor's draw_panel() use.
// Every outer colour is sampled from the backdrop under that edge so the
// fringe and corners match what the blit put there. Drawn EVERY frame: the
// same inputs give the same pixels, so the repaint is idempotent and there
// is no static/dynamic chrome split to keep in step (glass doc section 11:
// only the backdrop blit itself is a commit). `fill` is C_PANEL for the four
// window panels and C_CARD for a floating dialog.
static void draw_panel_fill(int x, int y, int w, int h, uint32_t fill, uint32_t border) {
    uint32_t below = bd_at(x + w / 2, y + h + 3);
    uint32_t c0 = bd_at(x + 4, y + 4),     c1 = bd_at(x + w - 4, y + 4),
             c2 = bd_at(x + 4, y + h - 4), c3 = bd_at(x + w - 4, y + h - 4);
    uint32_t outer = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t m = (((c0 >> sh) & 0xFF) + ((c1 >> sh) & 0xFF) + ((c2 >> sh) & 0xFF) + ((c3 >> sh) & 0xFF)) / 4;
        outer |= m << sh;
    }
    gui_soft_shadow(window_handle, x, y + 2, w, h, PANEL_R, below);
    gui_fill_rounded_aa(window_handle, x, y, w, h, PANEL_R, fill, outer);
    gui_rounded_border(window_handle, x, y, w, h, PANEL_R, border);
    win_draw_rect(window_handle, x + PANEL_R, y + 1, w - 2 * PANEL_R, 1, gui_lighten(fill, 16));
}
static void draw_panel(int x, int y, int w, int h) { draw_panel_fill(x, y, w, h, C_PANEL, C_EDGE); }

// Section eyebrow (glass doc section 5: small bold caps in DK_EYEBROW). The
// TTF path has no bold face here, so the caps + the accent hue carry it.
static void draw_eyebrow(int x, int y, const char *label) {
    char up[24]; int i = 0;
    for (; label[i] && i < 23; i++) up[i] = (label[i] >= 'a' && label[i] <= 'z') ? (char)(label[i] - 32) : label[i];
    up[i] = 0;
    win_draw_text_small(window_handle, x, y, up, C_EYEBROW);
}

// A row highlight inside a panel: rounded, AA-blended toward the panel.
static void draw_row_fill(int x, int y, int w, int h, uint32_t fill) {
    gui_fill_rounded_aa(window_handle, x, y, w, h, ROW_R, fill, C_PANEL);
}

// The list scrollbar: a 6px pill track (C_CARD) centred in the SCROLL_GUT
// gutter with a C_EDGE_GLASS thumb, the Editor's grammar; geometry unchanged
// from the flat version (thumb position = scroll * (h - th) / max).
static void draw_scroll_pill(int gx, int y, int h, int scroll, int max_scroll, int visible, int total) {
    int th = (visible * h) / total; if (th < 20) th = 20;
    int ty = max_scroll ? (scroll * (h - th)) / max_scroll : 0;
    int px = gx + (SCROLL_GUT - 6) / 2;
    gui_fill_rounded_aa(window_handle, px, y, 6, h, 3, C_CARD, C_PANEL);
    gui_fill_rounded_aa(window_handle, px, y + ty, 6, th, 3, C_EDGE_GLASS, C_CARD);
}

static void draw_preview(void) {
    int px = prev_x(), py = body_y();
    draw_panel(px, py, PREVIEW_W, body_h());
    draw_eyebrow(px + PANEL_IN, py + 10, "Preview");
    int cx = px + PANEL_IN, cy = py + 30, cw = PREVIEW_W - 2 * PANEL_IN;
    int bottom = py + body_h() - PANEL_IN;
    if (CUR.sel < 0 || CUR.sel >= item_count || prev_kind == PV_NONE) {
        if (CUR.sel >= 0 && CUR.sel < item_count && items[CUR.sel].is_directory)
            win_draw_text(window_handle, cx, cy, "(folder)", DIM_TEXT);
        else win_draw_text(window_handle, cx, cy, "No preview", DIM_TEXT);
        return;
    }
    // file name header
    win_draw_text(window_handle, cx, cy, items[CUR.sel].name, SIDE_TEXT);
    cy += 22;
    if (prev_kind == PV_IMAGE) {
        if (prev_thumb_ok) {
            // Blit the pre-decoded thumbnail straight from RAM (no disk, no
            // re-decode) - centered horizontally in the preview column.
            int ox = cx + ((cw - prev_thumb_w) / 2); if (ox < cx) ox = cx;
            for (int yy = 0; yy < prev_thumb_h; yy++) {
                uint32_t *drow = prev_thumb + (uint32_t)yy * prev_thumb_w;
                for (int xx = 0; xx < prev_thumb_w; xx++)
                    gui_draw_pixel(window_handle, ox + xx, cy + yy, drow[xx]);
            }
        } else {
            gui_draw_rect_outline(window_handle, cx, cy, cw, 120, BORDER_COLOR);
            win_draw_text(window_handle, cx + 8, cy + 52, "(image)", DIM_TEXT);
        }
        return;
    }
    if (prev_kind == PV_TEXT) {
        int x = cx, y = cy; int col = 0; int maxcol = cw / 8;
        for (int i = 0; i < prev_len && y < bottom - 12; i++) {
            char ch = (char)prev_buf[i];
            if (ch == '\n' || col >= maxcol) { y += 14; col = 0; if (ch == '\n') continue; }
            if (ch == '\r' || ch == '\t') continue;
            char s[2] = { ch, 0 };
            if (ch >= 32 && ch < 127) win_draw_text_small(window_handle, x + col * 6, y, s, SIDE_TEXT);
            col++;
        }
        return;
    }
    // PV_HEX
    int y = cy; int shown = prev_len < 256 ? prev_len : 256;
    for (int i = 0; i < shown && y < bottom - 12; i += 8) {
        char line[64]; int l = 0;
        char hx[4];
        for (int j = 0; j < 8 && i + j < shown; j++) {
            gui_itoa_hex(prev_buf[i + j], hx, 2);
            line[l++] = hx[0]; line[l++] = hx[1]; line[l++] = ' ';
        }
        line[l++] = ' ';
        for (int j = 0; j < 8 && i + j < shown; j++) {
            unsigned char c = prev_buf[i + j];
            line[l++] = (c >= 32 && c < 127) ? (char)c : '.';
        }
        line[l] = 0;
        win_draw_text_small(window_handle, cx, y, line, SIDE_TEXT);
        y += 12;
    }
}

// ---- tab bar --------------------------------------------------------------
#define TAB_W   130
#define TAB_GAP 4
#define TTF_TAB 12   // tab pill labels (glass doc section 5: labels at 12)
// Tab pill i's left edge; the "+" pill sits at tab_x(tab_count). ONE
// definition, read by draw_tabbar() AND the tab-bar click handler.
static int tab_x(int i) { return hdr_x() + PANEL_IN + i * (TAB_W + TAB_GAP); }

// (filesglass) The tab strip lives on the header panel's top row: the
// selected tab is the accent pill with the accent ink (the Calculator's
// selected-pill grammar), the others are nested cards (C_CARD + C_EDGE).
static void draw_tabbar(void) {
    int ty = tabrow_y();
    for (int i = 0; i < tab_count; i++) {
        int tx = tab_x(i);
        bool act = (i == active_tab);
        gui_fill_rounded_aa(window_handle, tx, ty, TAB_W, TAB_H, TAB_H / 2, act ? C_ACCENT : C_CARD, C_PANEL);
        if (!act) gui_rounded_border(window_handle, tx, ty, TAB_W, TAB_H, TAB_H / 2, C_EDGE);
        char title[18]; str_copy(title, basename_of(tabs[i].path), 18);
        int lty = ty + (TAB_H - TTF_TAB) / 2 - 1;
        win_draw_text_ttf(window_handle, tx + 12, lty, title, TTF_TAB, act ? C_ACCENT_INK : C_INK_DIM);
        // (#704) tab close affordance in the error ink; on the accent pill the
        // accent ink, because DK_ERROR on DK_ACCENT is under 1.5:1.
        win_draw_text_ttf(window_handle, tx + TAB_W - 18, lty, "x", TTF_TAB, act ? C_ACCENT_INK : C_ERR);
    }
    if (tab_count < MAX_TABS) {
        int ax = tab_x(tab_count);
        gui_fill_rounded_aa(window_handle, ax, ty, 24, TAB_H, TAB_H / 2, C_CARD, C_PANEL);
        gui_rounded_border(window_handle, ax, ty, 24, TAB_H, TAB_H / 2, C_EDGE);
        win_draw_text_ttf(window_handle, ax + 8, ty + (TAB_H - TTF_TAB) / 2 - 1, "+", TTF_TAB, C_INK);
    }
}

// ---- toolbar (command bar) ------------------------------------------------
// A style-engine icon button: styled background + a centered MICO glyph (with an
// arrow fallback). dir is the draw_arrow fallback direction (0=left,1=right,2=up).
static void files_icon_btn(int x, int y, int w, int h, const char *icn, int dir,
                           uint32_t tint, bool enabled, bool pressed) {
    gui_state_t st = !enabled ? GUI_ST_DISABLED : (pressed ? GUI_ST_PRESSED : GUI_ST_NORMAL);
    gui_button(window_handle, x, y, w, h, "", GUI_BTN_SECONDARY, st);
    int ix = x + (w - 16) / 2, iy = y + (h - 16) / 2;
    if (!draw_mico(icn, ix, iy, 16, tint)) draw_arrow(ix + 2, y + h / 2, dir, tint);
}

// Toolbar geometry, ONE definition each (draw + hit-test): three 26px nav
// buttons 28px apart from the panel's left inset, the address field filling
// the middle, New / View / filter (60px each, 64px pitch) against the right.
static int nav_x(int k)  { return hdr_x() + PANEL_IN + k * 28; }
static int addr_x(void)  { return hdr_x() + PANEL_IN + 90; }
static int cmd_x(void)   { return hdr_x() + hdr_w() - PANEL_IN - 3 * 64; }   // "New"; View at +64, filter at +128
static int addr_w(void)  { return cmd_x() - 8 - addr_x(); }

// (filesglass) The toolbar row of the header panel: the shared gui_button /
// gui_textfield2 widgets, unchanged, rendering under the glass palette that
// files_apply_style() installs.
static void draw_toolbar(void) {
    int by = tool_y();
    uint32_t ink = C_INK;
    // nav icon buttons (Zest chevrons; dim when the action is unavailable)
    files_icon_btn(nav_x(0), by, 26, TOOL_H, "CHEVL", 0, CUR.back_n ? ink : C_INK_DIM, CUR.back_n != 0, false);
    files_icon_btn(nav_x(1), by, 26, TOOL_H, "CHEVR", 1, CUR.fwd_n  ? ink : C_INK_DIM, CUR.fwd_n  != 0, false);
    files_icon_btn(nav_x(2), by, 26, TOOL_H, "CHEVU", 2, ink, true, false);
    // address bar
    gui_textfield2(window_handle, addr_x(), by, addr_w(), TOOL_H, CUR.path, false);
    // command buttons: New / View
    int bx = cmd_x();
    // #234i: New is DISABLED, not merely refused, on a read-only volume. A
    // control that can be pressed and then explains itself is worse than one
    // that visibly cannot be pressed; ro_block() still guards the action, so
    // this is the affordance and not the enforcement.
    gui_button(window_handle, bx, by, 60, TOOL_H, "New", GUI_BTN_SECONDARY,
               cur_is_readonly() ? GUI_ST_DISABLED
                                 : (g_menu == MENU_NEW ? GUI_ST_PRESSED : GUI_ST_NORMAL));
    gui_button(window_handle, bx + 64, by, 60, TOOL_H, "View", GUI_BTN_SECONDARY,
               g_menu == MENU_VIEW ? GUI_ST_PRESSED : GUI_ST_NORMAL);
    // filter box
    int fx = bx + 128;
    gui_textfield2(window_handle, fx, by, 60, TOOL_H, g_filter[0] ? g_filter : "", false);
    if (!g_filter[0]) win_draw_text_small(window_handle, fx + 6, by + 7, "Filter..", C_IN_PH);
}

// #234i fallback glyphs for the two disk-image volume classes. There is no
// CD or floppy asset in /ICONS (114 files, none of them a drive), so these are
// drawn, not loaded, in the same "icon identity colour, not chrome" class as
// the USB stick outline and the disk swatch above: they are the picture of a
// physical object, and theming them would make a CD stop looking like a CD.
//
// The disc is filled by row spans rather than per-pixel, which is one
// win_draw_rect per row instead of ~200 pixel calls across the compositor
// boundary; at 16px that is 16 calls.
// A disc: light body, dark rim, dark hub ring, and the row's own background
// showing through the centre hole.
//
// Three tones, not one. Measured at the 14px this is actually drawn at: a
// pale filled circle with a 2px dot read as a smudge, and a single-tone
// annulus read as a cog, because at that size the outer stair-steps ARE the
// silhouette. Filling the body first and then laying a dark rim on top gives
// the eye a clean circular edge to latch onto, and the punched hole is what
// makes it a disc rather than a button.
//
// `bg` is the colour of the row behind it, because the centre hole is a HOLE:
// there is no read-back through win_draw_*, so transparency has to be painted.
static void draw_disc_icon(int x, int y, int sz, uint32_t bg) {
    int r = sz / 2, cx = x + r, cy = y + r;
    int r2 = r * r;
    // MEASURED at r = 7 (the 14px this is drawn at), by rendering the pixel
    // pattern before shipping it. A 2px rim and a 3px hub ring left the light
    // data area ONE pixel wide, and the result read as a cog, not a disc: at
    // this size the ratio of the bands IS the identity. 1px rim, 1px hub ring,
    // a 3px hole and everything else data area is what reads correctly.
    int rim2 = (r - 1) * (r - 1); if (r < 3) rim2 = 0;
    int rh = r / 7; if (rh < 1) rh = 1;             // hole radius
    int rhub = rh + 1;                              // hub ring outer radius
    for (int dy = -r; dy < r; dy++) {
        for (int dx = -r; dx < r; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > r2) continue;
            uint32_t c;
            if (d2 <= rh * rh)              c = bg;              // centre hole
            else if (d2 <= rhub * rhub)     c = 0x00566270;      // hub ring
            else if (d2 >= rim2)            c = 0x00566270;      // rim
            else                            c = 0x00C2CCD8;      // data area
            gui_draw_pixel(window_handle, cx + dx, cy + dy, c);
        }
    }
    gui_draw_pixel(window_handle, cx - r + 2, cy - r + 3, 0x00EEF2F6);
}

static void draw_floppy_icon(int x, int y, int sz) {
    // Body, shutter (top centre), and label panel (bottom) - the three parts
    // that make a 3.5" disk readable at 16px.
    win_draw_rect(window_handle, x, y, sz, sz, 0x004A5560);
    gui_draw_rect_outline(window_handle, x, y, sz, sz, 0x00202830);
    int sw = sz / 2, sh = sz / 3;
    win_draw_rect(window_handle, x + (sz - sw) / 2, y + 1, sw, sh, 0x00B8C0C8);
    win_draw_rect(window_handle, x + 3, y + sz - sh - 1, sz - 6, sh, 0x00E8ECF0);
}

// ---- sidebar (Places) -----------------------------------------------------
// Build a flat clickable list: section headers + rows. We recompute hit rows
// each draw and store their target paths for click handling.
static char side_target[40][MAX_PATH_LEN];
static int  side_y[40];
// 0=path nav, 1=fixed device, 2=network, 3=recycle bin,
// 4=removable volume (navigate), 5=removable volume EJECT button,
// 6=removable volume that is mounted but whose files cannot be read
static int  side_kind[40];
static int  side_vol[40];    // #250: volume index for kinds 4/5/6, else -1
static int  side_rows = 0;

// (filesglass) Sidebar row geometry relative to the sidebar panel's left
// edge: the current-entry pill is inset 6px from the panel (radius ROW_R),
// the icon sits 12px in, the label 34px in, the eject glyph 22px from the
// right edge. The row's hit band is unchanged (side_y[i] - 2 .. + ITEM_HEIGHT - 2).
#define SIDE_ICON_X   (side_x() + 12)
#define SIDE_LABEL_X  (side_x() + 34)
#define SIDE_EJECT_X  (side_x() + SIDEBAR_W - 22)
static void side_row_fill(int y) { draw_row_fill(side_x() + 6, y - 2, SIDEBAR_W - 12, ITEM_HEIGHT, ITEM_SELECTED); }

static void draw_sidebar(void) {
    draw_panel(side_x(), body_y(), SIDEBAR_W, body_h());
    side_rows = 0;
    int ex = side_x() + PANEL_IN;        // eyebrow x
    int y = body_y() + 10;
    draw_eyebrow(ex, y, "Quick access"); y += 20;
    for (int i = 0; qa_label[i]; i++) {
        char p[MAX_PATH_LEN]; qa_path(i, p);
        bool cur = str_eq(CUR.path, p);
        if (cur) side_row_fill(y);
        { const char *icn = (i == 0) ? "HOME" : "FOLDER";
          uint32_t tint = cur ? files_fg(ITEM_SELECTED) : SIDE_TEXT;
          if (!draw_mico(icn, SIDE_ICON_X, y, ICON_SIZE, tint)) draw_folder_icon(SIDE_ICON_X, y); }
        win_draw_text(window_handle, SIDE_LABEL_X, y, qa_label[i], cur ? files_fg(ITEM_SELECTED) : SIDE_TEXT);
        str_copy(side_target[side_rows], p, MAX_PATH_LEN);
        side_y[side_rows] = y; side_kind[side_rows] = 0; side_vol[side_rows] = -1; side_rows++;
        y += ITEM_HEIGHT;
    }
    y += 6;
    draw_eyebrow(ex, y, "This PC"); y += 20;
    // (#704) Sidebar device/network/trash icon literals below are fallback
    // glyphs (draw_mico() found no icon-font asset), same "icon identity
    // color, not chrome" classification as the file-type colors above -
    // intentionally not themed.
    for (int i = 0; i < g_disk_count; i++) {
        win_draw_rect(window_handle, SIDE_ICON_X, y + 2, ICON_SIZE, ICON_SIZE - 2, 0x00808890);
        gui_draw_rect_outline(window_handle, SIDE_ICON_X, y + 2, ICON_SIZE, ICON_SIZE - 2, 0x00505860);
        char lbl[40]; int l = 0;
        const char *pfx = "Disk "; while (*pfx) lbl[l++] = *pfx++;
        lbl[l++] = '0' + i; lbl[l++] = ' '; lbl[l++] = '(';
        char mb[16]; gui_itoa(g_disks[i].size_mb, mb, 16);
        for (int k = 0; mb[k] && l < 30; k++) lbl[l++] = mb[k];
        const char *sfx = "MB)"; while (*sfx && l < 38) lbl[l++] = *sfx++;
        lbl[l] = 0;
        win_draw_text_small(window_handle, SIDE_LABEL_X, y + 4, lbl, SIDE_TEXT);
        str_copy(side_target[side_rows], "/", MAX_PATH_LEN);
        side_y[side_rows] = y; side_kind[side_rows] = 1; side_vol[side_rows] = -1; side_rows++;
        y += ITEM_HEIGHT;
    }
    if (g_disk_count == 0) { win_draw_text_small(window_handle, SIDE_LABEL_X, y + 4, "(no drives)", SIDE_DIM); y += ITEM_HEIGHT; }
    // ext2 volume mounted at /ext2 by the kernel ext2 driver (#99). Browsable
    // (read + create) through the normal file API.
    {
        bool cur = str_eq(CUR.path, "/ext2");
        if (cur) side_row_fill(y);
        win_draw_rect(window_handle, SIDE_ICON_X, y + 2, ICON_SIZE, ICON_SIZE - 2, 0x00608060);
        gui_draw_rect_outline(window_handle, SIDE_ICON_X, y + 2, ICON_SIZE, ICON_SIZE - 2, 0x00405040);
        win_draw_text_small(window_handle, SIDE_LABEL_X, y + 4, "ext2 (/ext2)", cur ? files_fg(ITEM_SELECTED) : SIDE_TEXT);
        str_copy(side_target[side_rows], "/ext2", MAX_PATH_LEN);
        side_y[side_rows] = y; side_kind[side_rows] = 1; side_vol[side_rows] = -1; side_rows++;
        y += ITEM_HEIGHT;
    }
    // ------------------------------------------------------------------
    // #250 Removable. One row per hot-plugged USB volume, with an eject
    // affordance on the right of the row. The section header is drawn only
    // when there is something in it, so an unremarkable machine looks
    // exactly as it did before.
    // ------------------------------------------------------------------
    if (g_vol_count > 0) {
        y += 6;
        draw_eyebrow(ex, y, "Removable"); y += 20;
        for (int i = 0; i < g_vol_count && side_rows < 38; i++) {
            const sc_volume_t *v = &g_vols[i];
            int readable = (v->flags & MOSVOL_READABLE) && (v->flags & MOSVOL_MOUNTED);
            bool cur = readable && str_eq(CUR.path, v->mount);
            if (cur) side_row_fill(y);
            uint32_t row_ink = cur ? files_fg(ITEM_SELECTED) : SIDE_TEXT;
            uint32_t row_dim = cur ? files_dim(ITEM_SELECTED) : SIDE_DIM;

            // Icon, one per volume CLASS. #234i added the disc and floppy
            // glyphs: a mounted CD and a mounted floppy drawn with the same
            // USB-stick outline would be three rows the user has to read to
            // tell apart. draw_mico() is tried first for each so a real asset
            // can be dropped into /ICONS later without touching this file;
            // none of the three exists today, so all three fall through.
            {
                uint32_t tint = row_ink;
                int ix = SIDE_ICON_X;
                if (v->flags & MOSVOL_OPTICAL) {
                    if (!draw_mico("CDROM", ix, y, ICON_SIZE, tint))
                        draw_disc_icon(ix, y + 1, ICON_SIZE - 2,
                                       cur ? ITEM_SELECTED : SIDEBAR_BG);
                } else if (v->flags & MOSVOL_FLOPPY) {
                    if (!draw_mico("FLOPPY", ix, y, ICON_SIZE, tint))
                        draw_floppy_icon(ix, y + 1, ICON_SIZE - 2);
                } else if (!draw_mico("USBDRIVE", ix, y, ICON_SIZE, tint)) {
                    win_draw_rect(window_handle, ix + 3, y + 5, ICON_SIZE - 6, ICON_SIZE - 6, 0x0060A0D0);
                    gui_draw_rect_outline(window_handle, ix + 3, y + 5, ICON_SIZE - 6, ICON_SIZE - 6, 0x00305070);
                    win_draw_rect(window_handle, ix + 6, y + 1, ICON_SIZE - 12, 4, 0x00B0B8C0);
                }
            }

            // Label: volume name, trimmed to fit, with the size beneath it.
            // #234i: the fallback name is per class, because "USB Drive" under
            // a disc icon is worse than no label at all. dos/diskimg.c already
            // guarantees an image volume has a name (the medium's label, else
            // the image filename), so these are last resorts.
            char nm[40]; int l = 0;
            for (int k = 0; v->name[k] && l < 24; k++) nm[l++] = v->name[k];
            if (l == 0) {
                const char *u = (v->flags & MOSVOL_OPTICAL) ? "CD-ROM"
                              : (v->flags & MOSVOL_FLOPPY)  ? "Floppy Disk"
                                                            : "USB Drive";
                while (*u && l < 24) nm[l++] = *u++;
            }
            nm[l] = 0;
            win_draw_text_small(window_handle, SIDE_LABEL_X, y + 1, nm, row_ink);

            char sub[48]; int sl = 0;
            char sz[24]; vol_size_label(v, sz, sizeof(sz));
            for (int k = 0; sz[k] && sl < 12; k++) sub[sl++] = sz[k];
            sub[sl++] = ' ';
            for (int k = 0; k < 8 && v->fsname[k] && sl < 30; k++) sub[sl++] = v->fsname[k];
            // #234i: SAY IT IS READ-ONLY before the user finds out by being
            // refused. Same principle as the "(not readable)" note below.
            if (readable && (v->flags & MOSVOL_READONLY)) {
                // "R/O", not "read-only". MEASURED: the sidebar is 160px, the
                // subtitle starts at x=34 and the eject glyph starts at
                // SIDEBAR_W-22 = 138, which leaves 104px, and
                // win_draw_text_small is ~6px per character. "354KB ISO9660
                // read-only" is 21 characters (126px) and drew straight
                // through the eject button. 17 characters is the fit.
                const char *w = " R/O";
                while (*w && sl < 17) sub[sl++] = *w++;
            }
            if (!readable) {
                // SAY SO. An exFAT volume mounts and reports its size and
                // then every file on it fails to open, because fs/exfat.c
                // implements mount/unmount/free-space and nothing else.
                // Showing it as an ordinary browsable drive would be a lie
                // the user only discovers by double-clicking. Hiding it
                // would be the bug this whole change exists to fix.
                const char *w = " (not readable)";
                while (*w && sl < 46) sub[sl++] = *w++;
            }
            sub[sl] = 0;
            win_draw_text_small(window_handle, SIDE_LABEL_X, y + 12, sub, readable ? row_dim : C_ERR);

            // Eject button: a small triangle-over-bar, right-aligned.
            int ejx = SIDE_EJECT_X;
            win_draw_rect(window_handle, ejx + 2, y + 13, 10, 2, row_ink);
            for (int r = 0; r < 5; r++)
                win_draw_rect(window_handle, ejx + 6 - r, y + 10 - r, 1 + 2 * r, 1, row_ink);

            // TWO rows in the hit table for ONE visual row: the eject button
            // is listed FIRST so the click scan finds it before the
            // navigate row that shares the same y band.
            str_copy(side_target[side_rows], v->mount, MAX_PATH_LEN);
            side_y[side_rows] = y; side_kind[side_rows] = 5; side_vol[side_rows] = i; side_rows++;

            str_copy(side_target[side_rows], v->mount, MAX_PATH_LEN);
            side_y[side_rows] = y;
            side_kind[side_rows] = readable ? 4 : 6;
            side_vol[side_rows] = i;
            side_rows++;

            y += ITEM_HEIGHT + 4;
        }
    }

    y += 6;
    draw_eyebrow(ex, y, "Network"); y += 20;
    if (!draw_mico("NETWORK", SIDE_ICON_X, y, ICON_SIZE, SIDE_TEXT))
        win_draw_rect(window_handle, SIDE_ICON_X, y + 3, ICON_SIZE, ICON_SIZE - 4, 0x004A78C0);
    win_draw_text(window_handle, SIDE_LABEL_X, y, "Network", SIDE_TEXT);
    str_copy(side_target[side_rows], "/NET", MAX_PATH_LEN);
    side_y[side_rows] = y; side_kind[side_rows] = 2; side_vol[side_rows] = -1; side_rows++;
    y += ITEM_HEIGHT;

    // Recycle Bin: opens the integrated trash view (side_kind 3) instead of a
    // directory. Highlighted when the recycle view is the active content.
    y += 6;
    draw_eyebrow(ex, y, "System"); y += 20;
    if (g_in_recycle) side_row_fill(y);
    { uint32_t tint = g_in_recycle ? files_fg(ITEM_SELECTED) : SIDE_TEXT;
      if (!draw_mico("RECYCLE", SIDE_ICON_X, y, ICON_SIZE, tint)) {
          // Simple trash-can fallback glyph.
          win_draw_rect(window_handle, SIDE_ICON_X + 1, y + 4, ICON_SIZE - 2, ICON_SIZE - 4, 0x00808890);
          win_draw_rect(window_handle, SIDE_ICON_X - 1, y + 1, ICON_SIZE + 2, 3, 0x00606870);
      } }
    win_draw_text(window_handle, SIDE_LABEL_X, y, "Recycle Bin", g_in_recycle ? files_fg(ITEM_SELECTED) : SIDE_TEXT);
    str_copy(side_target[side_rows], TRASH_DIR, MAX_PATH_LEN);
    side_y[side_rows] = y; side_kind[side_rows] = 3; side_vol[side_rows] = -1; side_rows++;
}

// ---- file list ------------------------------------------------------------
// (filesglass) The list panel: the rows sit directly on the panel fill; a
// selected row is the accent pill, a hovered row a lightened panel pill.
// The panel is drawn every frame (idempotent), and the rows over it, so no
// flat clear is needed.
static void draw_file_list(void) {
    int lx = list_x(), lw = list_w();
    draw_panel(cont_x(), body_y(), cont_w(), body_h());

    if (g_view == VIEW_ICONS) {
        int cols = lw / 96; if (cols < 1) cols = 1;
        int cw = lw / cols, chh = 76;
        int visible_rows = CONTENT_H / chh;
        int total_rows = (item_count + cols - 1) / cols;
        if (CUR.scroll > total_rows - visible_rows) CUR.scroll = total_rows - visible_rows;
        if (CUR.scroll < 0) CUR.scroll = 0;
        for (int i = CUR.scroll * cols; i < item_count; i++) {
            int r = i / cols - CUR.scroll, c = i % cols;
            int cx = lx + c * cw, cy = CONTENT_Y + 6 + r * chh;
            if (cy + chh > CONTENT_Y + CONTENT_H) break;
            if (i == CUR.sel) draw_row_fill(cx + 4, cy, cw - 8, chh - 4, ITEM_SELECTED);
            else if (i == hover_item) draw_row_fill(cx + 4, cy, cw - 8, chh - 4, ITEM_HOVER);
            int icx = cx + cw / 2 - 16, icy = cy + 8;
            {
                const char *icn = icon_for_entry(items[i].name, items[i].is_directory, i == CUR.sel);
                uint32_t tint = (i == CUR.sel) ? files_fg(ITEM_SELECTED) : TEXT_COLOR;
                if (!draw_mico(icn, icx, icy, 32, tint)) {
                    if (items[i].is_directory) { win_draw_rect(window_handle, icx, icy+4, 32, 24, ICON_FOLDER); win_draw_rect(window_handle, icx, icy, 12, 6, ICON_FOLDER); }
                    else { win_draw_rect(window_handle, icx + 4, icy, 24, 30, 0x00FFFFFF); win_draw_rect(window_handle, icx+4, icy, 24, 4, file_type_color(items[i].name)); gui_draw_rect_outline(window_handle, icx+4, icy, 24, 30, file_type_color(items[i].name)); }
                }
            }
            char nm[16]; str_copy(nm, items[i].name, 14);
            win_draw_text_small(window_handle, cx + cw/2 - gui_ttf_width(nm, 11)/2, cy + 46, nm,
                                (i == CUR.sel) ? files_fg(ITEM_SELECTED) : TEXT_COLOR);
        }
        if (total_rows > visible_rows && visible_rows > 0)
            draw_scroll_pill(lx + lw, CONTENT_Y, CONTENT_H, CUR.scroll, total_rows - visible_rows, visible_rows, total_rows);
        return;
    }

    // LIST / DETAILS share a row layout; DETAILS adds size + type columns.
    int visible = CONTENT_H / ITEM_HEIGHT;
    int max_scroll = item_count > visible ? item_count - visible : 0;
    if (CUR.scroll > max_scroll) CUR.scroll = max_scroll;
    if (CUR.scroll < 0) CUR.scroll = 0;
    int y = CONTENT_Y;
    for (int i = CUR.scroll; i < item_count && y < CONTENT_Y + CONTENT_H - ITEM_HEIGHT + 1; i++) {
        file_entry_t *it = &items[i];
        if (i == CUR.sel) draw_row_fill(lx, y, lw, ITEM_HEIGHT, ITEM_SELECTED);
        else if (i == hover_item) draw_row_fill(lx, y, lw, ITEM_HEIGHT, ITEM_HOVER);
        uint32_t tint = (i == CUR.sel) ? files_fg(ITEM_SELECTED) : TEXT_COLOR;
        {
            const char *icn = icon_for_entry(it->name, it->is_directory, i == CUR.sel);
            if (!draw_mico(icn, lx + 8, y + 4, ICON_SIZE, tint)) {
                if (it->is_directory) draw_folder_icon(lx + 8, y + 4);
                else draw_file_icon(lx + 8, y + 4, file_type_color(it->name));
            }
        }
        // TTF is variable-width: measure the name and trim with an ellipsis to
        // fit the name column (fixes the bitmap-width assumption that clipped text).
        // #554: DETAILS reserves an extra ~80px on the right for the Attr column.
        int namecol = (g_view == VIEW_DETAILS) ? (lw - 230) : (lw - 80);
        char nm[64]; int j = 0;
        while (it->name[j] && j < 60) { nm[j] = it->name[j]; j++; }
        nm[j] = 0;
        if (gui_ttf_width(nm, 14) > namecol) {
            while (j > 3 && gui_ttf_width(nm, 14) > namecol - 8) { nm[--j] = 0; }
            if (j >= 2) { nm[j-1] = '.'; nm[j] = '.'; nm[j+1] = 0; }
        }
        // selected row: name + detail use selection-aware colors so they stay
        // readable on the selection bg (the old code always used TEXT/DIM here).
        uint32_t dimc = (i == CUR.sel) ? files_dim(ITEM_SELECTED) : DIM_TEXT;
        win_draw_text(window_handle, lx + 32, y + 5, nm, tint);
        if (g_view == VIEW_DETAILS) {
            // #554: Attr column (real per-filesystem attributes; see fmt_attr).
            { char as[10]; fmt_attr(it, as);
                win_draw_text_small(window_handle, lx + lw - 210, y + 7, as, dimc); }
            if (!it->is_directory) { char ss[24]; fmt_size(it->size, ss);
                win_draw_text_small(window_handle, lx + lw - 130, y + 7, ss, dimc); }
            char e[8]; ext_of(it->name, e);
            const char *t = it->is_directory ? "Folder" : (e[0] ? e : "File");
            // right-align the type label in its column so it is never clipped
            int tw = gui_ttf_width(t, 11);
            win_draw_text_small(window_handle, lx + lw - 8 - tw, y + 7, t, dimc);
        }
        y += ITEM_HEIGHT;
    }
    // (filesglass) The scrollbar is the glass pill (track C_CARD, thumb
    // C_EDGE_GLASS: 3.9:1 on the panel), the Editor's grammar, in the same
    // SCROLL_GUT gutter the flat trough used; gui_scroll_colors() is not
    // consulted because the colours are fixed tokens, not a theme surface.
    if (item_count > visible)
        draw_scroll_pill(lx + lw, CONTENT_Y, CONTENT_H, CUR.scroll, max_scroll, visible, item_count);
}

// ---- recycle view ---------------------------------------------------------
// Geometry within the content area (right of the sidebar). The first row is an
// action toolbar (Restore / Delete Permanently / Empty Bin), then a column
// header, then the trashed-item list. Buttons live at fixed x ranges that the
// click handler mirrors exactly.
#define RB_TOOLBAR_H 34
#define RB_HEADER_H  24
#define RB_ROW_H     24
#define RB_BTN_RESTORE_X  8
#define RB_BTN_RESTORE_W  80
#define RB_BTN_DELETE_X   96
#define RB_BTN_DELETE_W   140
#define RB_BTN_EMPTY_X    244
#define RB_BTN_EMPTY_W    100

// (filesglass) The Recycle view takes the list panel, which spans to the
// right margin while g_in_recycle (prev_on() is false); its rows use the
// list's own inset (list_x / list_w plus the scroll gutter).
static int rb_area_x(void) { return list_x(); }
static int rb_area_w(void) { return list_w() + SCROLL_GUT; }
static int rb_list_y(void) { return CONTENT_Y + RB_TOOLBAR_H + RB_HEADER_H; }
static int rb_list_h(void) { return CONTENT_H - RB_TOOLBAR_H - RB_HEADER_H; }

static void draw_recycle_view(void) {
    int ax = rb_area_x(), aw = rb_area_w();
    draw_panel(cont_x(), body_y(), cont_w(), body_h());

    // Action toolbar row, then a hairline (inset by PANEL_R so it never
    // meets the rounded corners).
    win_draw_rect(window_handle, cont_x() + PANEL_R, CONTENT_Y + RB_TOOLBAR_H - 1, cont_w() - 2 * PANEL_R, 1, C_EDGE);
    int by = CONTENT_Y + 5;
    int sel = rb_count_selected();
    gui_button(window_handle, ax + RB_BTN_RESTORE_X, by, RB_BTN_RESTORE_W, 24, "Restore",
               GUI_BTN_SECONDARY, sel ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(window_handle, ax + RB_BTN_DELETE_X, by, RB_BTN_DELETE_W, 24, "Delete Perm.",
               GUI_BTN_SECONDARY, sel ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(window_handle, ax + RB_BTN_EMPTY_X, by, RB_BTN_EMPTY_W, 24, "Empty Bin",
               GUI_BTN_PRIMARY, rb_count ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    if (sel > 0) {
        char s[24]; gui_itoa(sel, s, 16); int l = str_len(s);
        const char *suf = " selected"; for (int i = 0; suf[i]; i++) s[l++] = suf[i]; s[l] = 0;
        int tw = gui_ttf_width(s, 11);
        win_draw_text_small(window_handle, ax + aw - tw - 10, by + 7, s, DIM_TEXT);
    }

    // Column header
    int hy = CONTENT_Y + RB_TOOLBAR_H;
    win_draw_text_small(window_handle, ax + 30, hy + 5, "Name", DIM_TEXT);
    win_draw_text_small(window_handle, ax + aw / 2, hy + 5, "Original Location", DIM_TEXT);
    win_draw_text_small(window_handle, ax + aw - 70, hy + 5, "Size", DIM_TEXT);

    int ly = rb_list_y(), lh = rb_list_h();
    if (rb_count == 0) {
        win_draw_text(window_handle, ax + aw / 2 - 70, ly + lh / 2 - 8, "Recycle Bin is empty", DIM_TEXT);
        return;
    }
    int visible = lh / RB_ROW_H;
    if (rb_scroll > rb_count - visible) rb_scroll = rb_count - visible;
    if (rb_scroll < 0) rb_scroll = 0;
    for (int i = 0; i < visible && i + rb_scroll < rb_count; i++) {
        int idx = i + rb_scroll;
        int y = ly + i * RB_ROW_H;
        rb_item_t *it = &rb_items[idx];
        if (it->selected) draw_row_fill(ax, y, aw - SCROLL_GUT, RB_ROW_H, ITEM_SELECTED);
        else if (idx == rb_hover) draw_row_fill(ax, y, aw - SCROLL_GUT, RB_ROW_H, ITEM_HOVER);
        uint32_t tint = it->selected ? files_fg(ITEM_SELECTED) : TEXT_COLOR;
        uint32_t dimc = it->selected ? files_dim(ITEM_SELECTED) : DIM_TEXT;
        // Checkbox (glass doc section 6: a hollow 2px frame, no fill; the
        // check mark in the row's own ink so it reads on the accent pill).
        gui_draw_rect_outline(window_handle, ax + 8, y + 4, 16, 16, it->selected ? tint : C_EDGE_GLASS);
        gui_draw_rect_outline(window_handle, ax + 9, y + 5, 14, 14, it->selected ? tint : C_EDGE_GLASS);
        if (it->selected) {
            win_draw_rect(window_handle, ax + 11, y + 9, 10, 2, tint);
            win_draw_rect(window_handle, ax + 13, y + 7, 2, 10, tint);
        }
        // Name (trimmed)
        char nm[64]; int j = 0; while (it->name[j] && j < 60) { nm[j] = it->name[j]; j++; } nm[j] = 0;
        int namecol = aw / 2 - 40;
        if (gui_ttf_width(nm, 14) > namecol) {
            while (j > 3 && gui_ttf_width(nm, 14) > namecol - 8) nm[--j] = 0;
            if (j >= 2) { nm[j-1] = '.'; nm[j] = '.'; nm[j+1] = 0; }
        }
        win_draw_text(window_handle, ax + 30, y + 4, nm, tint);
        // Original location (trimmed)
        char op[80]; j = 0; while (it->original_path[j] && j < 76) { op[j] = it->original_path[j]; j++; } op[j] = 0;
        int opcol = aw / 2 - 30;
        if (gui_ttf_width(op, 11) > opcol) {
            while (j > 3 && gui_ttf_width(op, 11) > opcol - 8) op[--j] = 0;
            if (j >= 2) { op[j-1] = '.'; op[j] = '.'; op[j+1] = 0; }
        }
        win_draw_text_small(window_handle, ax + aw / 2, y + 6, op, dimc);
        // Size
        char ss[24]; fmt_size(it->size, ss);
        win_draw_text_small(window_handle, ax + aw - 70, y + 6, ss, dimc);
    }
    // Scrollbar: the same glass pill the list view draws.
    if (rb_count > visible)
        draw_scroll_pill(ax + aw - SCROLL_GUT, ly, lh, rb_scroll, rb_count - visible, visible, rb_count);
}

// ---- dropdown menus -------------------------------------------------------
static const char *menu_new_items[]  = { "New Folder", "New Text File", NULL };
// #554: added "Sort: Attr" (ext2 mode / FAT attributes) and "Show Hidden
// Files" (FAT ATTR_HIDDEN + UNIX dotfiles). menu_select's indices below must
// stay in sync with this order.
static const char *menu_view_items[] = { "Icons", "List", "Details", "--", "Sort: Name", "Sort: Size", "Sort: Type", "Sort: Attr", "--", "Preview Pane", "Show Hidden Files", NULL };
static const char *menu_ctx_items[]  = { "Open", "Open with...", "--", "Install Font", "--", "Copy", "Cut", "Paste", "--", "Compress to .zip", "Compress to .tar.gz", "Extract here", "--", "Rename", "Delete", "--", "Properties", NULL };

static const char **active_menu_items(void) {
    if (g_menu == MENU_NEW) return menu_new_items;
    if (g_menu == MENU_VIEW) return menu_view_items;
    if (g_menu == MENU_CTX) return menu_ctx_items;
    return NULL;
}
static void draw_menu(void) {
    const char **m = active_menu_items(); if (!m) return;
    int n = 0; while (m[n]) n++;
    int mw = 150, mh = n * 22 + 6;
    int mx = g_menu_x, my = g_menu_y;
    if (mx + mw > WIN_W) mx = WIN_W - mw - 2;
    if (my + mh > WIN_H) my = WIN_H - mh - 2;
    // (filesglass) A popup floats over panels AND margin, so it is a square
    // nested card with the on-glass stroke (DK_EDGE_GLASS), the Editor's
    // menu-popup grammar; separators are the panel hairline.
    win_draw_rect(window_handle, mx, my, mw, mh, C_CARD);
    gui_draw_rect_outline(window_handle, mx, my, mw, mh, C_EDGE_GLASS);
    int yy = my + 3;
    for (int i = 0; m[i]; i++) {
        if (str_eq(m[i], "--")) { win_draw_rect(window_handle, mx + 4, yy + 10, mw - 8, 1, C_EDGE); yy += 22; continue; }
        win_draw_text(window_handle, mx + 10, yy + 4, m[i], C_INK);
        yy += 22;
    }
}

// ---- status row -----------------------------------------------------------
// (filesglass) Lives INSIDE the list panel, above its bottom edge, under a
// hairline inset by PANEL_R so it never meets the rounded corners.
static void draw_status(void) {
    int y = status_y();
    int x0 = cont_x() + PANEL_IN, w = cont_w() - 2 * PANEL_IN;
    win_draw_rect(window_handle, cont_x() + PANEL_R, y - 1, cont_w() - 2 * PANEL_R, 1, C_EDGE);
    if (g_err[0]) {
        // Deliberately takes over the whole status row and stays until the next
        // action. A failed delete that scrolls away has not been reported.
        gui_fill_rounded_aa(window_handle, x0 - 6, y + 2, w + 12, STATUS_H - 6, ROW_R, ERR_BAND, C_PANEL);
        win_draw_text(window_handle, x0, y + 3, g_err, 0x00FFFFFF);
        return;
    }
    if (g_in_recycle) {
        char s[64]; gui_itoa(rb_count, s, 16); int l = str_len(s);
        const char *it = " items in Recycle Bin"; while (*it) s[l++] = *it++; s[l] = 0;
        win_draw_text(window_handle, x0, y + 3, s, TEXT_COLOR);
        return;
    }
    // #netshares: on the Network view, the status row reports live sweep state.
    if (str_eq(CUR.path, "/NET")) {
        char ns[80];
        int st = ns_state();
        if (st == NS_SCANNING) {
            int d = 0, t = 0; int sv = ns_progress(&d, &t);
            snprintf(ns, sizeof(ns), "Discovering LAN shares  %d/%d  (%d found)", d, t, sv);
        } else if (st == NS_DONE && !ns_available()) {
            snprintf(ns, sizeof(ns), "Network unavailable");
        } else {
            int sv = ns_server_count();
            snprintf(ns, sizeof(ns), "%d server%s on your network", sv, sv == 1 ? "" : "s");
        }
        win_draw_text(window_handle, x0, y + 3, ns, TEXT_COLOR);
        return;
    }
    char s[48]; gui_itoa(item_count, s, 16); int l = str_len(s);
    const char *it = " items"; while (*it) s[l++] = *it++; s[l] = 0;
    win_draw_text(window_handle, x0, y + 3, s, TEXT_COLOR);
    if (CUR.sel >= 0 && CUR.sel < item_count)
        win_draw_text(window_handle, x0 + 120, y + 3, items[CUR.sel].name, TEXT_COLOR);
}

// (filesglass) Map the glass tokens into the shared style engine each redraw,
// so the gui_* primitives (buttons, fields, the confirm dialog) render in
// the glass palette. The WIDGETS are untouched: this is the same
// gui_set_palette() call every app makes, with fixed tokens instead of a
// theme derivation. Always the modern (rounded/AA) family: the window is
// fixed dark glass, and the glass doc forbids mixing the beveled classic
// grammar into a glass window.
static void files_apply_style(void) {
    gui_set_style(GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface        = C_PANEL;
    p.surface_raised = C_CARD;
    p.ink            = C_INK;
    p.ink_dim        = C_INK_DIM;
    p.accent         = C_ACCENT;
    p.accent_hover   = gui_lighten(C_ACCENT, 24);
    p.border         = C_EDGE;
    p.field_bg       = C_IN_FILL;
    p.field_border   = C_IN_BORDER;
    p.track          = C_CARD;
    gui_set_palette(&p);
}

// The layout facts the frame is composed for. Any change uncovers margin
// (or leaves popup / dialog pixels over margin), so it forces a backdrop blit.
static unsigned long layout_sig(void) {
    unsigned long s = 2166136261ul;
#define SIG(v) s = (s ^ (unsigned long)(v)) * 16777619ul
    SIG(WIN_W); SIG(WIN_H); SIG(prev_on()); SIG(g_in_recycle);
    SIG(g_menu); SIG(g_menu_x); SIG(g_menu_y);
    SIG(g_props_open); SIG(g_te_open); SIG(g_openwith_open); SIG(g_cred_open);
    SIG(gui_confirm_singleton_is_open());
#undef SIG
    return s;
}

// Full redraw.
//
// (filesglass) THE ANTI-FLASH CONTRACT (docs/UI_GLASS_DESIGN_SYSTEM.md
// section 11). SYS_WIN_BLIT self-commits: the kernel publishes the window the
// instant the backdrop lands, and a compositor sample taken between that
// commit and the win_invalidate() below would show a backdrop with no
// panels on it. So the blit runs ONLY when the chrome is dirty (start,
// EVENT_RESIZE, EVENT_REDRAW, wallpaper change, and a layout_sig() change),
// never on a hover, a selection or a scroll. Everything else is plain
// draws, which accumulate unpublished until the single invalidate at the
// end. The window is never cleared with a flat fill: the panels cover every
// pixel that changes, and the margins are the backdrop.
static void fb_redraw(void) {
    unsigned long sig = layout_sig();
    if (sig != g_layout_sig) { g_layout_sig = sig; g_chrome_dirty = 1; }
    sync_backdrop();
    if (g_chrome_dirty) {
        gui_glass_backdrop_blit(window_handle, g_bd);
        g_chrome_dirty = 0;
    }
    files_apply_style();
    draw_panel(hdr_x(), hdr_y(), hdr_w(), HDR_H);
    draw_tabbar();
    draw_toolbar();
    draw_sidebar();
    if (g_in_recycle) {
        draw_recycle_view();
    } else {
        draw_file_list();
        if (prev_on()) draw_preview();
    }
    draw_status();
    if (g_menu != MENU_NONE) draw_menu();
    if (g_props_open) draw_props();   // (#251) Properties dialog on top
    if (g_openwith_open) draw_openwith();  // Task C: Open with picker
    if (g_te_open) draw_te();              // Task C: inline Rename text entry
    if (g_cred_open) draw_cred();          // #317: SMB credentials dialog
    // #745: Recycle Bin delete/empty confirm - drawn LAST so it sits on top
    // of everything else, true modal (see the gate in EVENT_KEY_DOWN/
    // EVENT_MOUSE_DOWN below: it is checked before any other overlay).
    if (gui_confirm_singleton_is_open())
        gui_confirm_singleton_render(window_handle, WIN_W, WIN_H);
    win_invalidate(window_handle);
}

// ---- operations -----------------------------------------------------------
static void do_new_folder(void) {
    if (ro_block("Creating a folder")) return;
    static const char *cand[] = { "NEWFOLD", "NEWFOLD1", "NEWFOLD2", "NEWFOLD3", "NEWFOLD4" };
    char p[MAX_PATH_LEN];
    for (int i = 0; i < 5; i++) { path_join(p, CUR.path, cand[i]); if (mkdir(p, 0755) == 0) break; }
    load_directory(CUR.path); fb_redraw();
}
static void do_new_file(void) {
    if (ro_block("Creating a file")) return;
    static const char *cand[] = { "NEW.TXT", "NEW1.TXT", "NEW2.TXT", "NEW3.TXT" };
    char p[MAX_PATH_LEN];
    for (int i = 0; i < 4; i++) { path_join(p, CUR.path, cand[i]);
        int fd = open(p, 0x41); if (fd >= 0) { close(fd); break; } }
    load_directory(CUR.path); fb_redraw();
}
// Append one "<name>|<original path>" record to the trash index, durably.
// Returns 0 only if the record is on the medium. Without the index a trashed
// file shows as "(unknown)" and can never be restored, so a silent failure here
// is a quieter version of the same data loss.
static int trash_index_add(const char *name, const char *src) {
    int fd = open(TRASH_INDEX, 0x41);
    if (fd < 0) return -1;
    char line[MAX_PATH_LEN * 2]; int l = 0;
    for (int i = 0; name[i] && l < 120; i++) line[l++] = name[i];
    line[l++] = '|';
    for (int i = 0; src[i] && l < (int)sizeof(line) - 2; i++) line[l++] = src[i];
    line[l++] = '\n';
    int rc = 0;
    if (sys_seek(fd, 0, 2) < 0) rc = -1;
    if (rc == 0 && write(fd, line, l) != l) rc = -1;
    if (rc == 0 && fsync(fd) != 0) rc = -1;
    if (close(fd) != 0) rc = -1;
    return rc;
}

static void do_delete(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    if (ro_block("Deleting")) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)")) return;
    char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];
    path_join(src, CUR.path, it->name);
    mkdir("/CONFIG", 0755);
    mkdir(TRASH_DIR, 0755);
    path_join(dst, TRASH_DIR, it->name);
    files_err_clear();
    // (#239) ext2 rename() is unreliable, so fall back to copy_file()+unlink for
    // files. Only when the file truly reaches the bin do we record the index.
    int moved = 0;
    if (rename(src, dst) == 0) moved = 1;
    else if (!it->is_directory && copy_file(src, dst) == 0) {
        // copy_file() has fsync'd, so the bin copy is real. ONLY NOW may the
        // source go, and only if the unlink itself succeeds.
        if (unlink(src) == 0) moved = 1;
        else {
            files_error("Copied to Recycle Bin but could not remove the original", src);
            load_directory(CUR.path); fb_redraw();
            return;
        }
    }
    if (!moved) {
        // #742 THE BUG. This branch used to read:
        //     else { if (it->is_directory) rmdir(src); else unlink(src); }
        // A failed move to the Recycle Bin was "handled" by PERMANENTLY DELETING
        // the file: the one thing the Recycle Bin exists to prevent, done as the
        // fallback for the operation that was supposed to prevent it. The return
        // was discarded too, so the reload simply showed the item gone and the
        // user was told it had worked. An undoable delete is not a fallback for
        // an undoable delete. Report it and change nothing.
        files_error("Could not move to Recycle Bin; the file was NOT deleted", src);
        fb_redraw();
        return;
    }
    if (trash_index_add(it->name, src) != 0) {
        // The file IS in the bin, so this is not a loss, but Restore will not
        // know where it came from. Say so rather than discover it later.
        files_error("Moved to Recycle Bin, but the index write failed; "
                    "Restore will not know the original location", it->name);
    }
    load_directory(CUR.path); fb_redraw();
}
static void open_selected(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..")) { navigate_up(); return; }
    if (str_eq(it->name, "(empty)")) return;
    // #317/#netshares: Network folder rows dispatch by their action kind (see
    // build_net_listing()): controls, saved mounts, and live-discovered shares.
    if (str_eq(CUR.path, "/NET")) {
        net_row_t *r = &g_net_rows[CUR.sel];
        char np[MAX_PATH_LEN];
        switch (r->kind) {
        case NR_ADD:
            open_add_network();
            return;
        case NR_RESCAN:
            // Toggle: cancel an in-flight sweep, or start a fresh one. Rebuild
            // the listing immediately so the button label/status flip at once.
            if (ns_state() == NS_SCANNING) ns_cancel(); else ns_start();
            build_net_listing();
            fb_redraw();
            return;
        case NR_STATUS:
            return;   // informational, not clickable
        case NR_SAVED:
        case NR_DISC_SHARE:
            // Mount (saved or guest creds) then browse via the EXISTING /SMB VFS
            // path - no invented mount syscall. #317: if the mount is refused
            // (auth-gated share, or stale saved credentials) fall through to the
            // credentials dialog, pre-filled with what we know, so the user can
            // supply a username/password rather than hit a silent empty listing.
            if (net_mount(r->server, r->share, r->user, r->pass) == 0) {
                snprintf(np, sizeof(np), "/SMB/%s/%s", r->server, r->share);
                navigate_to(np);
            } else {
                open_cred_dialog(r->server, r->share, r->user);
            }
            return;
        case NR_DISC_SMB:
            // SMB host answered but shares could not be enumerated (guest denied
            // srvsvc), which usually means the server is auth-gated. #317: open
            // the credentials dialog pre-filled with the server so the user can
            // supply the share name plus a username/password.
            open_cred_dialog(r->server, "", "");
            return;
        case NR_DISC_NFS:
            // #317 nfsbrowse: an NFS export row carries the server-side export
            // path in r->export. Mount it on demand (SYS_NFS_MOUNT reuses the
            // kernel NFSv3 client) then browse via the returned /NFS mount
            // point. Like the SMB net_mount above, the mount is synchronous in
            // this app's own thread (a deliberate click, not the compositor);
            // the slow part, export enumeration, already ran on the netscan
            // worker. A bare NFS-server row (showmount denied) has no export to
            // mount, so say so honestly.
            if (r->export[0]) {
                char mp[MAX_PATH_LEN]; mp[0] = 0;
                if (net_nfs_mount(r->server, r->export, mp, sizeof(mp)) == 0 && mp[0]) {
                    navigate_to(mp);
                } else {
                    files_error("NFS mount failed", "could not mount the selected export");
                    fb_redraw();
                }
            } else {
                files_error("NFS server detected", "no exports could be listed (server denied showmount)");
                fb_redraw();
            }
            return;
        default:
            return;
        }
    }
    if (it->is_directory) { char np[MAX_PATH_LEN]; path_join(np, CUR.path, it->name); navigate_to(np); }
    else { char full[MAX_PATH_LEN]; path_join(full, CUR.path, it->name);
        const char *app = default_app_for(it->name);
        char *av[2]; av[0] = (char *)app; av[1] = full; sys_spawn_args(app, av, 2); }
}

// ---- Task C: inline Rename (reusable text-entry overlay) -------------------
static void rename_commit(const char *newname) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    if (ro_block("Renaming")) return;
    if (!newname || newname[0] == 0) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)")) return;
    char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];
    path_join(src, CUR.path, it->name);
    path_join(dst, CUR.path, newname);
    if (!str_eq(src, dst)) {
        // ext2 rename() is unreliable here (same as Cut/Paste/Delete), so fall
        // back to copy+unlink for files when the in-place rename fails.
        if (rename(src, dst) != 0 && !it->is_directory) {
            if (copy_file(src, dst) == 0) unlink(src);
        }
    }
    load_directory(CUR.path);
    fb_redraw();
}
// #554: after load_directory() (which always resets CUR.sel to -1), re-find
// an item by name so a permissions edit can keep the Properties dialog open
// and showing the NEW value - required to actually demonstrate an edit
// persisted, rather than just that the dialog silently closed.
static void reselect_by_name(const char *name) {
    for (int k = 0; k < item_count; k++) {
        if (str_eq(items[k].name, name)) { CUR.sel = k; return; }
    }
}

// #554: Properties permissions edit, ext2/POSIX paths only (FAT is toggled
// directly by clicking the read-only hotspot - see props_ro_toggle() - since
// a FAT path has nothing else to edit). buf is "MODE" or "MODE:UID:GID",
// MODE always octal (e.g. "644"); UID/GID are decimal and optional. Applies
// chmod always, chown only when both UID and GID were given (chown requires
// root; a non-root caller just sees chown's normal EPERM, silently no-op'd
// here exactly like every other Files op that can fail against permissions).
static void perms_edit_commit(const char *buf) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (it->fs_type != FSPERM_TYPE_POSIX) return;
    if (!buf || !buf[0]) return;

    unsigned mode = 0; int i = 0;
    while (buf[i] >= '0' && buf[i] <= '7') { mode = (mode << 3) | (unsigned)(buf[i] - '0'); i++; }
    if (i == 0) return;  // not a valid octal mode; refuse rather than guess

    char full[MAX_PATH_LEN];
    path_join(full, CUR.path, it->name);
    char savedname[MAX_NAME_LEN]; str_copy(savedname, it->name, MAX_NAME_LEN);
    chmod(full, (mode_t)mode);

    if (buf[i] == ':') {
        i++;
        unsigned uid = 0; int j = i;
        while (buf[j] >= '0' && buf[j] <= '9') { uid = uid * 10 + (unsigned)(buf[j] - '0'); j++; }
        if (j > i && buf[j] == ':') {
            int k = j + 1; unsigned gid = 0; int gs = k;
            while (buf[k] >= '0' && buf[k] <= '9') { gid = gid * 10 + (unsigned)(buf[k] - '0'); k++; }
            if (k > gs) chown(full, (uid_t)uid, (gid_t)gid);
        }
    }
    load_directory(CUR.path);
    reselect_by_name(savedname);
    g_props_open = 1;
    fb_redraw();
}
// Opens the generic text-entry overlay pre-filled "mode:uid:gid" for the
// currently-selected ext2/POSIX item.
static void open_perms_edit(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (it->fs_type != FSPERM_TYPE_POSIX) return;
    char buf[40]; int n = 0;
    unsigned mode = it->mode & 0777;
    char oct[8]; int oi = 0;
    if (mode == 0) oct[oi++] = '0';
    else { char tmp[8]; int t = 0; unsigned v = mode;
        while (v) { tmp[t++] = (char)('0' + (v % 8)); v /= 8; }
        while (t > 0) oct[oi++] = tmp[--t]; }
    oct[oi] = 0;
    for (int k = 0; oct[k]; k++) buf[n++] = oct[k];
    buf[n++] = ':';
    { char tmp[16]; int t = 0; unsigned v = it->uid; if (v == 0) tmp[t++] = '0';
      while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
      while (t > 0) buf[n++] = tmp[--t]; }
    buf[n++] = ':';
    { char tmp[16]; int t = 0; unsigned v = it->gid; if (v == 0) tmp[t++] = '0';
      while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
      while (t > 0) buf[n++] = tmp[--t]; }
    buf[n] = 0;
    str_copy(g_te_title, "Mode[:uid:gid]", sizeof(g_te_title));
    str_copy(g_te_buf, buf, MAX_NAME_LEN);
    g_te_len = str_len(g_te_buf);
    g_te_purpose = 3;       // #554: permissions edit
    g_te_open = 1;
}
// #554: FAT paths have no mode to edit, only ONE genuine toggle - the
// on-disk read-only attribute (chmod's owner-write bit, per fs/fat.c
// fat_set_readonly()). Applied directly on click; no text entry needed.
static void props_ro_toggle(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (it->fs_type != FSPERM_TYPE_FAT) return;
    char full[MAX_PATH_LEN];
    path_join(full, CUR.path, it->name);
    char savedname[MAX_NAME_LEN]; str_copy(savedname, it->name, MAX_NAME_LEN);
    int was_ro = (it->fat_attr & FSPERM_FAT_READONLY) ? 1 : 0;
    chmod(full, was_ro ? 0644 : 0444);
    load_directory(CUR.path);
    reselect_by_name(savedname);
    g_props_open = 1;
    fb_redraw();
}
// Open the generic text-entry overlay seeded with the selected file's name.
static void open_rename(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)")) return;
    str_copy(g_te_title, "Rename", sizeof(g_te_title));
    str_copy(g_te_buf, it->name, MAX_NAME_LEN);
    g_te_len = str_len(g_te_buf);
    g_te_purpose = 1;       // rename
    g_te_open = 1;
}
// Drive a key into the open text-entry overlay. Returns 1 if it consumed it.
static int te_key(char c, uint32_t kc) {
    if (!g_te_open) return 0;
    if (c == 27) { g_te_open = 0; }                       // Esc = cancel
    else if (c == '\n' || c == '\r' || kc == 0x1C) {      // Enter = confirm
        g_te_open = 0;
        if (g_te_purpose == 1) rename_commit(g_te_buf);
        else if (g_te_purpose == 3) perms_edit_commit(g_te_buf);  // #554
    } else if (c == '\b' || kc == 0x0E) {
        if (g_te_len > 0) g_te_buf[--g_te_len] = 0;
    } else if (c >= 32 && c < 127 && g_te_len < MAX_NAME_LEN - 1) {
        g_te_buf[g_te_len++] = c; g_te_buf[g_te_len] = 0;
    }
    return 1;
}

// ---- Task C: "Open with" app picker ---------------------------------------
typedef struct { const char *label; const char *path; } ow_app_t;
static const ow_app_t OW_APPS[] = {
    { "Text Editor",  "/APPS/EDITOR"   },
    { "Image Viewer", "/APPS/IMAGEVIEWER"  },
    { "Maytera Studio", "/APPS/PAINT"  },
    { "Web Browser",  "/APPS/BROWSER"  },
    { "Music Player", "/APPS/MUSICPLR" },
    { "Media Player", "/APPS/MEDIAPLAYER"  },
    { "Terminal",     "/APPS/TERMINAL" },
    { "Python",       "/APPS/PYTHON.ELF" },
};
#define OW_N ((int)(sizeof(OW_APPS)/sizeof(OW_APPS[0])))
static void open_openwith(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)") || it->is_directory) return;
    g_ow_hover = -1;
    g_openwith_open = 1;
}
static void ow_pick(int idx) {
    g_openwith_open = 0;
    if (idx < 0 || idx >= OW_N) return;
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    char full[MAX_PATH_LEN]; path_join(full, CUR.path, items[CUR.sel].name);
    char *av[2]; av[0] = (char *)OW_APPS[idx].path; av[1] = full;
    sys_spawn_args(OW_APPS[idx].path, av, 2);
}

// ---- clipboard: Copy / Cut / Paste (#251) ---------------------------------
// Insert "_1" before the extension of base -> out (e.g. ASSOC.CFG -> ASSOC_1.CFG).
static void name_with_suffix(char *out, const char *base) {
    int dot = -1; for (int i = 0; base[i]; i++) if (base[i] == '.') dot = i;
    int o = 0;
    if (dot < 0) {
        for (int k = 0; base[k] && o < MAX_NAME_LEN - 3; k++) out[o++] = base[k];
        out[o++] = '_'; out[o++] = '1';
    } else {
        for (int k = 0; k < dot && o < MAX_NAME_LEN - 5; k++) out[o++] = base[k];
        out[o++] = '_'; out[o++] = '1';
        for (int k = dot; base[k] && o < MAX_NAME_LEN - 1; k++) out[o++] = base[k];
    }
    out[o] = 0;
}

// Stream-copy a regular file src -> dst. Returns 0 ONLY if every byte is on the
// medium.
//
// #742: this used to return 0 after an unchecked close(), and THREE callers go
// on to unlink() the SOURCE on that 0. That is the data-loss mechanism, and it
// does not need a broken disk: sys_fsync()'s own contract says that on a
// non-zero return "the file may be EMPTY OR ABSENT, and is NEVER the previous
// contents", because ext2_write_file truncates the destination before it
// discovers there is no room and the rollback leaves a valid ZERO-BYTE file. So
// a full volume could produce a successful-looking copy of nothing, followed by
// the deletion of the only real copy. fsync() is the call that makes the answer
// mean something, and it must be checked BEFORE close(), because close()
// consumes the fd whether or not it reports an error.
static int copy_file(const char *src, const char *dst) {
    int in = open(src, 0);              // O_RDONLY
    if (in < 0) return -1;
    int out = open(dst, 0x41);          // O_CREAT|O_WRONLY (matches do_new_file)
    if (out < 0) { close(in); return -1; }
    char buf[4096];
    int n, rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        int w = 0;
        while (w < n) { int k = write(out, buf + w, n - w); if (k <= 0) { rc = -1; break; } w += k; }
        if (rc) break;
    }
    if (n < 0) rc = -1;
    if (rc == 0 && fsync(out) != 0) rc = -1;   // durable, or it did not happen
    if (close(out) != 0) rc = -1;              // the final flush can still fail
    close(in);
    return rc;
}

static void do_copy(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)")) return;
    path_join(g_clip_path, CUR.path, it->name);
    g_clip_mode = 1;
}
static void do_cut(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)")) return;
    path_join(g_clip_path, CUR.path, it->name);
    g_clip_mode = 2;
}
static void do_paste(void) {
    if (g_clip_mode == 0 || g_clip_path[0] == 0) return;
    // The DESTINATION is what has to be writable. A Cut also unlinks the
    // source, so a cut FROM a read-only volume is refused too, rather than
    // copying the file and then silently failing to remove the original,
    // which would look like a successful move that duplicated the file.
    if (ro_block("Pasting")) return;
    if (g_clip_mode == 2) {
        const sc_volume_t *sv = vol_for_path(g_clip_path);
        if (sv && (sv->flags & MOSVOL_READONLY)) {
            notify_post("Read-only volume",
                        "That item was cut from a read-only volume, so it cannot be "
                        "moved. Copy it instead.", NOTIFY_WARNING);
            return;
        }
    }
    char base[MAX_NAME_LEN]; str_copy(base, basename_of(g_clip_path), MAX_NAME_LEN);
    char dst[MAX_PATH_LEN]; path_join(dst, CUR.path, base);
    // Pasting into the source's own directory would target the original file;
    // give the copy a distinct "_1" name so we never overwrite the source.
    if (str_eq(dst, g_clip_path)) {
        char nm[MAX_NAME_LEN]; name_with_suffix(nm, base);
        path_join(dst, CUR.path, nm);
        if (str_eq(dst, g_clip_path)) return;   // paranoia: still equal -> bail
    }
    if (g_clip_mode == 2) {
        // Cut = move. rename() handles files and directories on the same volume;
        // fall back to copy+unlink for the file case if rename is unavailable.
        if (rename(g_clip_path, dst) != 0) {
            if (copy_file(g_clip_path, dst) == 0) unlink(g_clip_path);
        }
        g_clip_mode = 0; g_clip_path[0] = 0;   // a cut item is consumed once pasted
    } else {
        copy_file(g_clip_path, dst);            // copy: files only (dirs are a no-op)
    }
    load_directory(CUR.path);
    fb_redraw();
}

// ---- Properties dialog (#251, #554 permissions section) -------------------
// #554: the permissions section is filesystem-aware, per the docs/UI_STYLE_
// GUIDE.md design decision - ext2/POSIX shows real owner/group/mode with an
// edit hotspot; genuine FAT (ESP: /boot, /EFI) shows the real on-disk
// attribute flags with ONLY a read-only toggle (the one genuine equivalent
// that exists); neither ever shows a fabricated value for the model that
// does not apply to that path.
#define PROPS_BW 340
#define PROPS_BH 300
static inline int props_bx(void) { return (WIN_W - PROPS_BW) / 2; }
static inline int props_by(void) { return (WIN_H - PROPS_BH) / 2; }
// (filesglass) A floating dialog: a nested card (C_CARD, PANEL_R) with the
// on-glass stroke, its title on the first row over a hairline. The content
// still starts at by + 34 (where the old 22px title band ended), so every
// hit-test offset below is unchanged.
static void draw_dialog_frame(int bx, int by, int bw, int bh, const char *title) {
    draw_panel_fill(bx, by, bw, bh, C_CARD, C_EDGE_GLASS);
    win_draw_text(window_handle, bx + PANEL_IN, by + 6, title, C_INK);
    win_draw_rect(window_handle, bx + PANEL_R, by + 28, bw - 2 * PANEL_R, 1, C_EDGE);
}
// Y of the permissions-edit / read-only-toggle hotspot line, shared by
// draw_props() and props_hit() so they can never disagree.
static int g_props_hotspot_y = 0;
static int g_props_hotspot_kind = 0;  // 0 = none this frame, 1 = edit perms, 2 = toggle RO

static void draw_props(void) {
    if (!g_props_open) return;
    if (CUR.sel < 0 || CUR.sel >= item_count) { g_props_open = 0; return; }
    file_entry_t *it = &items[CUR.sel];
    int bw = PROPS_BW, bh = PROPS_BH;
    int bx = props_bx(), by = props_by();
    draw_dialog_frame(bx, by, bw, bh, "Properties");
    int tx = bx + 16, ty = by + 34;
    win_draw_text(window_handle, tx, ty, "Name:", TEXT_COLOR);
    win_draw_text(window_handle, tx + 90, ty, it->name, TEXT_COLOR); ty += 24;
    win_draw_text(window_handle, tx, ty, "Type:", TEXT_COLOR);
    win_draw_text(window_handle, tx + 90, ty, it->is_directory ? "Folder" : "File", TEXT_COLOR); ty += 24;
    win_draw_text(window_handle, tx, ty, "Size:", TEXT_COLOR);
    { char sz[24]; fmt_size((uint32_t)it->size, sz); win_draw_text(window_handle, tx + 90, ty, sz, TEXT_COLOR); } ty += 24;
    win_draw_text(window_handle, tx, ty, "Location:", TEXT_COLOR);
    win_draw_text(window_handle, tx + 90, ty, CUR.path, TEXT_COLOR); ty += 30;

    win_draw_rect(window_handle, bx + 8, ty, bw - 16, 1, BORDER_COLOR); ty += 12;
    g_props_hotspot_kind = 0;
    g_props_hotspot_y = 0;

    if (it->fs_type == FSPERM_TYPE_POSIX) {
        win_draw_text(window_handle, tx, ty, "Filesystem:", TEXT_COLOR);
        win_draw_text(window_handle, tx + 90, ty, "ext2 (POSIX permissions)", TEXT_COLOR); ty += 24;
        win_draw_text(window_handle, tx, ty, "Owner:", TEXT_COLOR);
        { char s[16]; gui_itoa((int)it->uid, s, 16); struct passwd *pw = getpwuid(it->uid);
          char line[48]; int n = 0; const char *p = s; while (*p) line[n++] = *p++;
          if (pw && pw->pw_name[0]) { line[n++] = ' '; line[n++] = '('; const char *q = pw->pw_name;
              while (*q && n < 44) { line[n++] = *q++; }
              line[n++] = ')'; }
          line[n] = 0; win_draw_text(window_handle, tx + 90, ty, line, TEXT_COLOR); } ty += 24;
        win_draw_text(window_handle, tx, ty, "Group:", TEXT_COLOR);
        { char s[16]; gui_itoa((int)it->gid, s, 16); win_draw_text(window_handle, tx + 90, ty, s, TEXT_COLOR); } ty += 24;
        win_draw_text(window_handle, tx, ty, "Mode:", TEXT_COLOR);
        { char rwx[10]; fmt_attr(it, rwx);
          char oct[8]; int oi = 0; unsigned v = it->mode & 0777;
          if (v == 0) oct[oi++] = '0'; else { char tmp[8]; int t = 0;
              while (v) { tmp[t++] = (char)('0' + (v % 8)); v /= 8; } while (t > 0) oct[oi++] = tmp[--t]; }
          oct[oi] = 0;
          char line[24]; int n = 0; for (int k = 0; rwx[k]; k++) line[n++] = rwx[k];
          line[n++] = ' '; line[n++] = '('; for (int k = 0; oct[k]; k++) line[n++] = oct[k]; line[n++] = ')'; line[n] = 0;
          win_draw_text(window_handle, tx + 90, ty, line, TEXT_COLOR); }
        ty += 26;
        g_props_hotspot_y = ty; g_props_hotspot_kind = 1;
        win_draw_text(window_handle, tx, ty, "[ Edit Permissions... ]", fp_acc());
        ty += 24;
        if (!it->has_perm_entry) win_draw_text_small(window_handle, tx, ty, "(default: no explicit entry yet)", DIM_TEXT);
    } else if (it->fs_type == FSPERM_TYPE_FAT) {
        win_draw_text(window_handle, tx, ty, "Filesystem:", TEXT_COLOR);
        win_draw_text(window_handle, tx + 90, ty, "FAT (ESP - no owner/mode)", TEXT_COLOR); ty += 24;
        win_draw_text(window_handle, tx, ty, "Attributes:", TEXT_COLOR);
        { char a[10]; fmt_attr(it, a);
          char line[40]; int n = 0; for (int k = 0; a[k]; k++) line[n++] = a[k];
          const char *legend = "  (D=Dir R=RO H=Hid S=Sys A=Arc)";
          for (int k = 0; legend[k] && n < 38; k++) line[n++] = legend[k];
          line[n] = 0; win_draw_text_small(window_handle, tx + 90, ty + 2, line, TEXT_COLOR); }
        ty += 30;
        g_props_hotspot_y = ty; g_props_hotspot_kind = 2;
        win_draw_text(window_handle, tx, ty,
                      (it->fat_attr & FSPERM_FAT_READONLY) ? "[ Clear Read-Only ]" : "[ Set Read-Only ]",
                      fp_acc());
        ty += 24;
        win_draw_text_small(window_handle, tx, ty, "(FAT has no owner/exec bit; this is the only", DIM_TEXT); ty += 14;
        win_draw_text_small(window_handle, tx, ty, "real permission FAT supports)", DIM_TEXT);
    } else {
        win_draw_text(window_handle, tx, ty, "Filesystem:", TEXT_COLOR);
        win_draw_text(window_handle, tx + 90, ty, "Network share", TEXT_COLOR); ty += 24;
        win_draw_text_small(window_handle, tx, ty, "Permissions are enforced by the server; no local model.", DIM_TEXT);
    }
    win_draw_text(window_handle, bx + bw - 130, by + bh - 26, "[ click to close ]", DIM_TEXT);
}
// Returns: 1 = the perms-edit hotspot was hit, 2 = the RO-toggle hotspot was
// hit, 0 = click landed elsewhere (caller should close the dialog). Must be
// called only while g_props_open, right after a draw_props() so
// g_props_hotspot_y/_kind reflect the CURRENT selection.
static int props_hit(int lx, int ly) {
    if (g_props_hotspot_kind == 0) return 0;
    int bx = props_bx(), tx = bx + 16;
    int hw = 220;  // generous click width for either label
    if (lx >= tx && lx < tx + hw && ly >= g_props_hotspot_y - 14 && ly < g_props_hotspot_y + 8)
        return g_props_hotspot_kind;
    return 0;
}

// ---- Task C overlays: geometry, draw, hit-test ----------------------------
// Text-entry overlay geometry.
#define TE_W 380
#define TE_H 148
static inline int te_bx(void){ return (WIN_W - TE_W) / 2; }
static inline int te_by(void){ return (WIN_H - TE_H) / 2; }
#define TE_BTN_W 88
#define TE_BTN_H 26
static void draw_te(void) {
    if (!g_te_open) return;
    int bx = te_bx(), by = te_by();
    draw_dialog_frame(bx, by, TE_W, TE_H, g_te_title);
    win_draw_text(window_handle, bx + 16, by + 34, "New name:", DIM_TEXT);
    // editable field (with a simple trailing caret): the shared focused
    // field, so it takes the glass input tokens from the palette.
    char disp[MAX_NAME_LEN + 2];
    str_copy(disp, g_te_buf, MAX_NAME_LEN);
    { int l = str_len(disp); if (l < MAX_NAME_LEN) { disp[l] = '_'; disp[l+1] = 0; } }
    gui_textfield2(window_handle, bx + 16, by + 56, TE_W - 32, 28, disp, true);
    // OK / Cancel buttons: the shared primary / secondary buttons at the
    // same rects te_hit() tests.
    int oy = by + TE_H - TE_BTN_H - 12;
    int okx = bx + TE_W - 2 * TE_BTN_W - 24;
    int cax = bx + TE_W - TE_BTN_W - 12;
    gui_button(window_handle, okx, oy, TE_BTN_W, TE_BTN_H, "OK", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    gui_button(window_handle, cax, oy, TE_BTN_W, TE_BTN_H, "Cancel", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
}
// Returns: 0 = OK, 1 = Cancel, -1 = inside box (swallow), -2 = outside (cancel).
static int te_hit(int lx, int ly) {
    int bx = te_bx(), by = te_by();
    if (lx < bx || lx >= bx + TE_W || ly < by || ly >= by + TE_H) return -2;
    int oy = by + TE_H - TE_BTN_H - 12;
    int okx = bx + TE_W - 2 * TE_BTN_W - 24;
    int cax = bx + TE_W - TE_BTN_W - 12;
    if (ly >= oy && ly < oy + TE_BTN_H) {
        if (lx >= okx && lx < okx + TE_BTN_W) return 0;
        if (lx >= cax && lx < cax + TE_BTN_W) return 1;
    }
    return -1;
}

// ---- #317 authenticated SMB credentials dialog ----------------------------
// A modal, four-field connect dialog. The password field is drawn masked (each
// character shown as a dot), and the real password bytes never leave the
// g_cred_pass buffer, never appear in a draw call and never reach a log/serial
// line. Fields reuse the shared caret/selection/clipboard textfield widget
// (textfield.h) and the shared style renderer (gui_textfield_tf); Connect,
// Cancel and the "Save as favourite" checkbox use gui_button / gui_checkbox.
#define CRED_W      400
#define CRED_H      270
#define CRED_LBL_W  84
#define CRED_BTN_W  96
#define CRED_BTN_H  26
static inline int cred_bx(void){ return (WIN_W - CRED_W) / 2; }
static inline int cred_by(void){ return (WIN_H - CRED_H) / 2; }
static inline int cred_field_y(int i){ return cred_by() + 42 + i * 34; }
static inline int cred_chk_y(void){ return cred_by() + 42 + 4 * 34 + 6; }

// Open the dialog, optionally pre-filling Server / Share / Username. The
// password always starts empty. Focus lands on the first empty field.
static void open_cred_dialog(const char *server, const char *share, const char *user) {
    str_copy(g_cred_server, server ? server : "", sizeof(g_cred_server));
    str_copy(g_cred_share,  share  ? share  : "", sizeof(g_cred_share));
    str_copy(g_cred_user,   user   ? user   : "", sizeof(g_cred_user));
    g_cred_pass[0] = 0;
    tf_init(&g_cred_tf[0], g_cred_server, sizeof(g_cred_server));
    tf_init(&g_cred_tf[1], g_cred_share,  sizeof(g_cred_share));
    tf_init(&g_cred_tf[2], g_cred_user,   sizeof(g_cred_user));
    tf_init(&g_cred_tf[3], g_cred_pass,   sizeof(g_cred_pass));
    g_cred_focus = !g_cred_server[0] ? 0 : (!g_cred_share[0] ? 1 : 2);
    g_cred_save = 0;
    g_cred_open = 1;
    fb_redraw();
}

static void draw_cred(void) {
    if (!g_cred_open) return;
    int bx = cred_bx(), by = cred_by();
    draw_dialog_frame(bx, by, CRED_W, CRED_H, "Connect to Server");
    static const char *labels[4] = { "Server:", "Share:", "Username:", "Password:" };
    static const char *phs[4]    = { "host or IP", "share name",
                                     "(guest if blank)", "(guest if blank)" };
    int fx = bx + 16 + CRED_LBL_W;
    int fw = CRED_W - 32 - CRED_LBL_W;
    for (int i = 0; i < 4; i++) {
        int fy = cred_field_y(i);
        win_draw_text(window_handle, bx + 16, fy + 7, labels[i], DIM_TEXT);
        textfield_t *tf = &g_cred_tf[i];
        const char *txt = tf->buf;
        char mbuf[41];
        if (i == 3) {   // password: render a masked copy, never the real bytes
            int n = tf->len; if (n > 40) n = 40;
            for (int k = 0; k < n; k++) mbuf[k] = '*';
            mbuf[n] = 0;
            txt = mbuf;
        }
        const char *ph = (tf->len == 0) ? phs[i] : 0;
        gui_textfield_tf(window_handle, fx, fy, fw, 28, txt, tf->len, tf->cursor,
                         tf->sel_anchor, g_cred_focus == i, ph);
    }
    if (g_cred_focus == 4)  // keyboard focus ring on the Save-favourite checkbox
        gui_draw_rect(window_handle, bx + 12, cred_chk_y() - 3, CRED_W - 24, 24, 0x3AA6FF);
    gui_checkbox(window_handle, bx + 16, cred_chk_y(), 18, g_cred_save != 0,
                 "Save as favourite (persists reboots)", GUI_ST_NORMAL);
    int oy  = by + CRED_H - CRED_BTN_H - 12;
    int cnx = bx + CRED_W - 2 * CRED_BTN_W - 24;
    int cax = bx + CRED_W - CRED_BTN_W - 12;
    gui_button(window_handle, cnx, oy, CRED_BTN_W, CRED_BTN_H, "Connect",
               GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    gui_button(window_handle, cax, oy, CRED_BTN_W, CRED_BTN_H, "Cancel",
               GUI_BTN_SECONDARY, GUI_ST_NORMAL);
}

// Attempt the mount with the entered credentials. On success optionally saves
// the connection as a favourite (reusing netmount_add -> NETMOUNTS.CFG) and
// navigates into the share; on failure keeps the dialog open so the user can
// correct the credentials. The password is never included in any status text.
static void cred_connect(void) {
    if (!g_cred_server[0] || !g_cred_share[0]) {
        files_error("Connect failed", "Server and Share are both required");
        return;   // keep dialog open
    }
    if (net_mount(g_cred_server, g_cred_share, g_cred_user, g_cred_pass) != 0) {
        files_error("Connect failed", "could not mount the share (check name and credentials)");
        return;   // keep dialog open so the credentials can be corrected
    }
    if (g_cred_save)
        netmount_add(g_cred_server, g_cred_share, g_cred_user, g_cred_pass);
    g_cred_open = 0;
    char np[MAX_PATH_LEN];
    snprintf(np, sizeof(np), "/SMB/%s/%s", g_cred_server, g_cred_share);
    navigate_to(np);
}

// Drive a key event into the open credentials dialog. Esc cancels, Enter
// connects, Tab / Up / Down move focus between fields, everything else goes to
// the focused field's shared textfield handler (caret, selection, clipboard).
static void cred_key(const gui_event_t *ev) {
    if (!g_cred_open) return;
    char c = ev->key_char; uint32_t kc = ev->keycode;
    if (c == 27) { g_cred_open = 0; return; }                 // Esc = cancel
    // Focus order: 0=Server 1=Share 2=Username 3=Password 4=Save-favourite checkbox.
    // The checkbox is in the keyboard focus cycle so the whole dialog is keyboard-
    // drivable (mouse clicks do not land headless, #334; validation 2026-09-23).
    if (c == '\t') { g_cred_focus = (g_cred_focus + 1) % 5; return; }  // Tab
    if (kc == 0x81) { if (g_cred_focus < 4) g_cred_focus++; return; }  // Down
    if (kc == 0x80) { if (g_cred_focus > 0) g_cred_focus--; return; }  // Up
    if (g_cred_focus == 4) {                                   // Save-favourite checkbox focused
        if (c == ' ') { g_cred_save = !g_cred_save; return; } // Space toggles it
        if (c == '\n' || c == '\r' || kc == 0x1C) { cred_connect(); return; }
        return;                                                // no text entry on the checkbox
    }
    if (c == '\n' || c == '\r' || kc == 0x1C) { cred_connect(); return; }  // Enter = connect
    tf_handle_key(&g_cred_tf[g_cred_focus], ev);
}

// Hit-test a click in the dialog. Returns 0 = Connect, 1 = Cancel, 2 = toggle
// the checkbox, 10+i = focus field i, -1 = inside (swallow), -2 = outside.
static int cred_hit(int lx, int ly) {
    int bx = cred_bx(), by = cred_by();
    if (lx < bx || lx >= bx + CRED_W || ly < by || ly >= by + CRED_H) return -2;
    int oy  = by + CRED_H - CRED_BTN_H - 12;
    int cnx = bx + CRED_W - 2 * CRED_BTN_W - 24;
    int cax = bx + CRED_W - CRED_BTN_W - 12;
    if (ly >= oy && ly < oy + CRED_BTN_H) {
        if (lx >= cnx && lx < cnx + CRED_BTN_W) return 0;
        if (lx >= cax && lx < cax + CRED_BTN_W) return 1;
    }
    int chy = cred_chk_y();
    if (ly >= chy - 2 && ly < chy + 22 && lx >= bx + 16 && lx < bx + CRED_W - 16) return 2;
    for (int i = 0; i < 4; i++) {
        int fy = cred_field_y(i);
        if (ly >= fy && ly < fy + 28 && lx >= bx + 16 && lx < bx + CRED_W - 16) return 10 + i;
    }
    return -1;
}

// Open-with picker geometry.
#define OW_W 300
#define OW_ROWH 28
static inline int ow_h(void){ return 34 + OW_N * OW_ROWH + 14; }
static inline int ow_bx(void){ return (WIN_W - OW_W) / 2; }
static inline int ow_by(void){ return (WIN_H - ow_h()) / 2; }
static void draw_openwith(void) {
    if (!g_openwith_open) return;
    int bx = ow_bx(), by = ow_by(), bh = ow_h();
    draw_dialog_frame(bx, by, OW_W, bh, "Open with");
    if (CUR.sel >= 0 && CUR.sel < item_count)
        win_draw_text_small(window_handle, bx + 16, by + 31, items[CUR.sel].name, DIM_TEXT);
    int yy = by + 46;
    for (int i = 0; i < OW_N; i++) {
        if (i == g_ow_hover)
            gui_fill_rounded_aa(window_handle, bx + 6, yy - 3, OW_W - 12, OW_ROWH - 2, ROW_R, ITEM_HOVER, C_CARD);
        win_draw_text(window_handle, bx + 22, yy + 3, OW_APPS[i].label, C_INK);
        yy += OW_ROWH;
    }
    win_draw_text(window_handle, bx + OW_W - 110, by + bh - 22, "Esc = Cancel", DIM_TEXT);
}
// Returns app index, -1 = inside non-item (swallow), -2 = outside (cancel).
static int ow_hit(int lx, int ly) {
    int bx = ow_bx(), by = ow_by(), bh = ow_h();
    if (lx < bx || lx >= bx + OW_W || ly < by || ly >= by + bh) return -2;
    int y0 = by + 46;
    if (ly < y0) return -1;
    int idx = (ly - y0) / OW_ROWH;
    if (idx >= 0 && idx < OW_N) return idx;
    return -1;
}

// ---- tabs -----------------------------------------------------------------
static void tab_new(void) {
    g_in_recycle = 0;
    if (tab_count >= MAX_TABS) return;
    int i = tab_count++;
    tabs[i].used = true; tabs[i].back_n = 0; tabs[i].fwd_n = 0; tabs[i].sel = -1; tabs[i].scroll = 0;
    str_copy(tabs[i].path, g_home, MAX_PATH_LEN);
    active_tab = i;
    load_directory(tabs[i].path);
    fb_redraw();
}
static void tab_close(int i) {
    if (tab_count <= 1) return;
    for (int k = i; k < tab_count - 1; k++) tabs[k] = tabs[k + 1];
    tab_count--;
    if (active_tab >= tab_count) active_tab = tab_count - 1;
    load_directory(CUR.path);
    fb_redraw();
}
static void tab_switch(int i) {
    g_in_recycle = 0;
    if (i < 0 || i >= tab_count) return;
    active_tab = i;
    load_directory(CUR.path);   // reload active tab's dir
    fb_redraw();
}

// ---- #321 archiver context-menu actions -----------------------------------
// Wire the selected file/dir to the /APPS/ARCHIVE terminal tool. Compress
// makes <name>.zip / <name>.tar.gz next to the item; Extract here unpacks an
// archive into the current directory.
static void do_compress(const char *ext) {
    if (ro_block("Creating an archive")) return;
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)")) return;
    char src[MAX_PATH_LEN]; path_join(src, CUR.path, it->name);
    char out[MAX_PATH_LEN]; path_join(out, CUR.path, it->name);
    int l = str_len(out);
    for (int i = 0; ext[i] && l < MAX_PATH_LEN - 1; i++) out[l++] = ext[i];
    out[l] = 0;
    char *av[4]; av[0] = (char *)"/APPS/ARCHIVE"; av[1] = (char *)"c"; av[2] = out; av[3] = src;
    sys_spawn_args("/APPS/ARCHIVE", av, 4);
    load_directory(CUR.path); fb_redraw();
}
// #351: install the selected .ttf/.otf into the system font store. The font is
// registered live, so every app's font picker sees it immediately, with no
// reboot and no restart of the app. Result is reported through the existing
// notification path rather than a bespoke dialog.
static void do_install_font(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)")) return;
    char src[MAX_PATH_LEN];
    path_join(src, CUR.path, it->name);

    int face = gui_font_install(src);
    char msg[160];
    if (face >= 0) {
        char fam[GUI_FONT_NAME_MAX] = {0}, sty[GUI_FONT_STYLE_MAX] = {0};
        font_name(face, fam, sizeof(fam));
        font_style(face, sty, sizeof(sty));
        // Report the family from the font's NAME TABLE, not the filename: they
        // routinely disagree (SourceCodePro-SemiBoldItalic.ttf is "Source Code
        // Pro" / "Semibold Italic"), and the name table is the truth.
        snprintf(msg, sizeof(msg), "Installed %s %s", fam[0] ? fam : it->name, sty);
    } else if (face == -2) {
        snprintf(msg, sizeof(msg), "%s is not a TrueType/OpenType font", it->name);
    } else if (face == -4) {
        snprintf(msg, sizeof(msg), "Font registry full");
    } else {
        snprintf(msg, sizeof(msg), "Could not install %s", it->name);
    }
    notify_post("Fonts", msg, 0);
    load_directory(CUR.path);
    fb_redraw();
}

static void do_extract_here(void) {
    if (ro_block("Extracting here")) return;
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)")) return;
    char src[MAX_PATH_LEN]; path_join(src, CUR.path, it->name);
    char dst[MAX_PATH_LEN]; str_copy(dst, CUR.path, MAX_PATH_LEN);
    char *av[4]; av[0] = (char *)"/APPS/ARCHIVE"; av[1] = (char *)"x"; av[2] = src; av[3] = dst;
    sys_spawn_args("/APPS/ARCHIVE", av, 4);
    load_directory(CUR.path); fb_redraw();
}
// ---- menu actions ---------------------------------------------------------
static void menu_select(int idx) {
    if (g_menu == MENU_NEW) { if (idx == 0) do_new_folder(); else if (idx == 1) do_new_file(); }
    else if (g_menu == MENU_VIEW) {
        switch (idx) {
            case 0: g_view = VIEW_ICONS; break;
            case 1: g_view = VIEW_LIST; break;
            case 2: g_view = VIEW_DETAILS; break;
            case 4: g_sort = SORT_NAME; sort_items(); break;
            case 5: g_sort = SORT_SIZE; sort_items(); break;
            case 6: g_sort = SORT_TYPE; sort_items(); break;
            case 7: g_sort = SORT_ATTR; sort_items(); break;
            case 9: g_preview_on = !g_preview_on; break;
            case 10: g_show_hidden = !g_show_hidden; load_directory(CUR.path); break;
        }
    } else if (g_menu == MENU_CTX) {
        // Match by label so the action stays correct as items are added/removed.
        const char *lab = menu_ctx_items[idx];
        if (str_eq(lab, "Open")) open_selected();
        else if (str_eq(lab, "Open with...")) open_openwith();
        else if (str_eq(lab, "Copy")) do_copy();
        else if (str_eq(lab, "Cut")) do_cut();
        else if (str_eq(lab, "Paste")) do_paste();
        else if (str_eq(lab, "Rename")) open_rename();
        else if (str_eq(lab, "Delete")) do_delete();
        else if (str_eq(lab, "Compress to .zip")) do_compress(".zip");
        else if (str_eq(lab, "Compress to .tar.gz")) do_compress(".tar.gz");
        else if (str_eq(lab, "Extract here")) do_extract_here();
        else if (str_eq(lab, "Install Font")) do_install_font();
        else if (str_eq(lab, "Properties")) g_props_open = 1;
    }
    g_menu = MENU_NONE;
    fb_redraw();
}
static int menu_hit(int lx, int ly) {  // returns item index or -1
    const char **m = active_menu_items(); if (!m) return -1;
    int n = 0; while (m[n]) n++;
    int mw = 150, mh = n * 22 + 6, mx = g_menu_x, my = g_menu_y;
    if (mx + mw > WIN_W) mx = WIN_W - mw - 2;
    if (my + mh > WIN_H) my = WIN_H - mh - 2;
    if (lx < mx || lx >= mx + mw || ly < my || ly >= my + mh) return -2;  // outside
    int idx = (ly - my - 3) / 22;
    if (idx < 0 || idx >= n) return -1;
    if (str_eq(m[idx], "--")) return -1;
    return idx;
}

int main(int argc, char **argv) {

    // Resolve the current user's home directory for Quick access. Desktop apps
    // run as root (home "/"), so fall back to the admin home where the standard
    // skeleton folders live, rather than pointing Quick access at "/".
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] && !(pw->pw_dir[0] == '/' && pw->pw_dir[1] == '\0'))
        str_copy(g_home, pw->pw_dir, MAX_PATH_LEN);
    else
        str_copy(g_home, "/HOME/ADMIN", MAX_PATH_LEN);

    enumerate_disks();
    poll_volumes();   // #250: removable volumes, before the first paint

    for (int i = 0; i < MAX_TABS; i++) tabs[i].sel = -1;
    tabs[0].used = true; str_copy(tabs[0].path, g_home, MAX_PATH_LEN);
    current_path = tabs[0].path;

    // (filesglass, #528 class) g_win_w/g_win_h are the CONTENT size (what
    // EVENT_RESIZE delivers and what every layout helper assumes), and
    // win_create() takes the OUTER size, so add the chrome (the Editor's /
    // Settings' constants) and then read the real canvas back. Passing the
    // content size directly, as this did, made the kernel carve the 24px
    // chrome out of it and silently clipped the bottom of the layout: with
    // the flat chrome the status bar's bottom rows went missing; with the
    // glass panels the whole bottom margin and status row were cut off.
    window_handle = win_create("Files", win_x, win_y, WIN_W + 4, WIN_H + 24);
    if (window_handle < 0) return 1;
    {
        int cw = 0, chh = 0;
        if (win_get_size(window_handle, &cw, &chh) == 0 && cw > 0 && chh > 0) { g_win_w = cw; g_win_h = chh; }
    }

    // #317: one-shot start-path override. If /CONFIG/FILESPATH.CFG exists, open
    // that path (e.g. "/NET" or "/SMB/<server>/<share>") instead of home, then
    // consume the file. Used by AUTORUN for deterministic verification; harmless
    // otherwise.
    const char *startpath = g_home;
    {
        int pf = open("/CONFIG/FILESPATH.CFG", 0);
        if (pf >= 0) {
            static char sp[MAX_PATH_LEN];
            int n = read(pf, sp, MAX_PATH_LEN - 1);
            close(pf);
            unlink("/CONFIG/FILESPATH.CFG");
            if (n > 0) {
                int e = n;
                while (e > 0 && (sp[e-1]=='\n'||sp[e-1]=='\r'||sp[e-1]==' '||sp[e-1]=='\t')) e--;
                sp[e] = 0;
                if (sp[0] == '/') { str_copy(tabs[0].path, sp, MAX_PATH_LEN); startpath = tabs[0].path; }
            }
        }
    }
    // (#745) argv[1] as a start path, so a desktop folder icon can open Files
    // AT that folder. Applied AFTER the /CONFIG/FILESPATH.CFG one-shot above so
    // the existing AUTORUN override still wins, and only for an absolute path.
    // The desktop needs this because it cannot rely on writing /CONFIG: a
    // non-root session has no authority there (#683), so a sentinel file would
    // work for the root autologin and silently fail for everyone else.
    if (argc >= 2 && argv[1] && argv[1][0] == '/') {
        str_copy(tabs[0].path, argv[1], MAX_PATH_LEN);
        startpath = tabs[0].path;
    }
    load_directory(startpath);
    // #239: the desktop / start-menu Recycle Bin launches drop a one-shot
    // sentinel so Files opens straight into the integrated Recycle Bin view.
    { int rf = open("/RECYVIEW.FLG", 0); if (rf >= 0) { close(rf); unlink("/RECYVIEW.FLG"); g_in_recycle = 1; rb_load(); } }
    fb_redraw();

    gui_event_t event;
    int running = 1;
    while (running) {
        int et = win_get_event(window_handle, &event, 100);
        if (et == 0) {
            // #250: idle. This is the ONLY place volumes are polled: the
            // event loop's existing 100 ms timeout, throttled to ~1 s, and
            // never from draw_sidebar(), which runs on every repaint. One
            // syscall that touches no device; the redraw only happens when
            // the list actually changed.
            static unsigned long s_last_vol = 0;
            unsigned long now = (unsigned long)get_ticks();
            if (now - s_last_vol >= 18 || s_last_vol == 0) {   // ~1s at 18Hz ticks
                s_last_vol = now;
                if (poll_volumes()) fb_redraw();
            }
            // #netshares: while the Network view is open, refresh the listing
            // whenever the async sweep has made progress (new probes done, a
            // server found, or the scan finished). Cheap: an atomic read, and a
            // rebuild+redraw only when something actually changed.
            if (str_eq(CUR.path, "/NET")) {
                static int s_np = -1, s_ns = -1, s_st = -1;
                int d = 0, t = 0; int sv = ns_progress(&d, &t); int stt = ns_state();
                if (d != s_np || sv != s_ns || stt != s_st) {
                    s_np = d; s_ns = sv; s_st = stt;
                    build_net_listing();
                    fb_redraw();
                }
            }
            continue;
        }
        switch (event.type) {
        case EVENT_REDRAW:
            // (filesglass) the compositor asked for the whole window again:
            // chrome dirty, so the backdrop is blitted before the panels.
            g_chrome_dirty = 1; fb_redraw(); break;
        case EVENT_RESIZE:
            // (filesglass) a resize reallocates the content buffer, so the
            // backdrop must be blitted again: chrome dirty.
            if (event.mouse_x > 0 && event.mouse_y > 0) { g_win_w = event.mouse_x; g_win_h = event.mouse_y; }
            g_chrome_dirty = 1; fb_redraw(); break;
        case EVENT_WINDOW_CLOSE: running = 0; break;
        case EVENT_KEY_DOWN: {
            char c = event.key_char; uint32_t kc = event.keycode;
            // #745: the Recycle Bin confirm is a true modal - it gets first
            // crack at every key, above even the other modal overlays below.
            if (gui_confirm_singleton_is_open()) {
                int r = gui_confirm_singleton_handle_key(c);
                if (r == 2) {
                    if (g_rb_pending_action == 1) rb_delete_selected();
                    else if (g_rb_pending_action == 2) rb_empty();
                    g_rb_pending_action = 0;
                }
                fb_redraw();
                break;
            }
            // Task C: modal overlays capture keys first.
            // #317: the SMB credentials dialog is modal - it gets keys first.
            if (g_cred_open) { cred_key(&event); fb_redraw(); break; }
            if (g_te_open) { te_key(c, kc); fb_redraw(); break; }
            if (g_openwith_open) { if (c == 27) { g_openwith_open = 0; fb_redraw(); } break; }
            if (g_props_open) {
                if (c == 27) { g_props_open = 0; fb_redraw(); break; }
                // #554: 'e' triggers the SAME action the permissions hotspot's
                // mouse click does (edit mode/owner/group on ext2, toggle
                // read-only on FAT) - keyboard-only path, verified this way.
                if ((c == 'e' || c == 'E') && g_props_hotspot_kind != 0) {
                    if (g_props_hotspot_kind == 1) open_perms_edit();
                    else props_ro_toggle();
                    fb_redraw(); break;
                }
            }
            if (g_menu != MENU_NONE && c == 27) { g_menu = MENU_NONE; fb_redraw(); break; }
            // (#251) clipboard keyboard shortcuts (Ctrl+C/X/V arrive as control chars)
            if (!g_in_recycle) {
                if (c == 0x03) { do_copy();  break; }   // Ctrl+C
                if (c == 0x18) { do_cut();   break; }   // Ctrl+X
                if (c == 0x16) { do_paste(); break; }   // Ctrl+V (do_paste redraws)
                // #554: Ctrl+P opens Properties on the selected item, mouse-free
                // (the context menu's "Properties" item was previously the only
                // way in; keyboard-only per the same reasoning as the other
                // Ctrl shortcuts here, and it is what let this dialog's
                // permissions section be verified without mouse injection).
                if (c == 0x10 && CUR.sel >= 0 && CUR.sel < item_count) { g_props_open = 1; fb_redraw(); break; }
            }
            if (g_in_recycle) {
                // ESC leaves the recycle view back to the current folder; a/d
                // select/deselect all; r restores the selection.
                if (c == 27) { g_in_recycle = 0; navigate_to(CUR.path); }
                // #542: Ctrl+A is the OS-wide select-all (0x01); keep bare a/d too.
                else if (c == 0x01 || c == 'a' || c == 'A') { for (int i = 0; i < rb_count; i++) rb_items[i].selected = 1; fb_redraw(); }
                else if (c == 'd' || c == 'D') { for (int i = 0; i < rb_count; i++) rb_items[i].selected = 0; fb_redraw(); }
                else if (c == 'r' || c == 'R') { if (rb_count_selected()) rb_restore_selected(); fb_redraw(); }
                break;
            }
            if (c == 27) { running = 0; }
            else if (kc == 0x1C || c == '\n' || c == '\r') { open_selected(); load_preview(); fb_redraw(); }
            else if (c == '\b' || kc == 0x0E) {
                if (g_filter[0]) { g_filter[str_len(g_filter) - 1] = 0; load_directory(CUR.path); fb_redraw(); }
                else navigate_up();
            }
            else if (kc == 0x80) { if (CUR.sel > 0) { CUR.sel--; if (CUR.sel < CUR.scroll) CUR.scroll = CUR.sel; load_preview(); fb_redraw(); } }
            else if (kc == 0x81) { if (CUR.sel < item_count - 1) { CUR.sel++; int v = CONTENT_H / ITEM_HEIGHT; if (CUR.sel >= CUR.scroll + v) CUR.scroll = CUR.sel - v + 1; load_preview(); fb_redraw(); } }
            else if (c >= 32 && c < 127) {  // type into filter
                int fl = str_len(g_filter); if (fl < (int)sizeof(g_filter) - 1) { g_filter[fl] = c; g_filter[fl+1] = 0; load_directory(CUR.path); fb_redraw(); }
            }
        } break;
        case EVENT_MOUSE_DOWN: {
            int wx, wy; win_get_pos(window_handle, &wx, &wy);
            int lx = event.mouse_x, ly = event.mouse_y;
            int right = (event.mouse_buttons & MOUSE_BUTTON_RIGHT) ? 1 : 0;
            // #745: the Recycle Bin confirm is a true modal - first crack at
            // every click, never dismissed by clicking away from it.
            if (gui_confirm_singleton_is_open()) {
                int r = gui_confirm_singleton_handle_mouse(lx, ly, 1);
                if (r == 2) {
                    if (g_rb_pending_action == 1) rb_delete_selected();
                    else if (g_rb_pending_action == 2) rb_empty();
                    g_rb_pending_action = 0;
                }
                fb_redraw();
                break;
            }
            // Task C: modal overlays consume clicks first.
            // #317: the SMB credentials dialog is modal - clicks go to it first.
            if (g_cred_open) {
                int h = cred_hit(lx, ly);
                if (h == 0) cred_connect();                 // Connect
                else if (h == 1 || h == -2) g_cred_open = 0; // Cancel / click-away
                else if (h == 2) g_cred_save = !g_cred_save; // toggle favourite
                else if (h >= 10) g_cred_focus = h - 10;     // focus that field
                fb_redraw(); break;
            }
            if (g_te_open) {
                int h = te_hit(lx, ly);
                if (h == 0) {
                    g_te_open = 0;
                    if (g_te_purpose == 1) rename_commit(g_te_buf);
                    else if (g_te_purpose == 3) perms_edit_commit(g_te_buf);  // #554
                }
                else if (h == 1 || h == -2) { g_te_open = 0; }
                fb_redraw(); break;
            }
            if (g_openwith_open) {
                int h = ow_hit(lx, ly);
                if (h >= 0) ow_pick(h);
                else if (h == -2) g_openwith_open = 0;
                fb_redraw(); break;
            }
            // (#251, #554) Properties dialog is modal. A click on the
            // permissions hotspot acts instead of dismissing; any other click
            // (inside or outside the dialog) still dismisses it, unchanged.
            if (g_props_open) {
                int h = props_hit(lx, ly);
                if (h == 1) { open_perms_edit(); fb_redraw(); break; }         // ext2: edit mode/owner/group
                if (h == 2) { props_ro_toggle(); break; }                      // FAT: toggle read-only
                g_props_open = 0; fb_redraw(); break;
            }
            // menu first
            if (g_menu != MENU_NONE) {
                int h = menu_hit(lx, ly);
                if (h == -2) { g_menu = MENU_NONE; fb_redraw(); }
                else if (h >= 0) menu_select(h);
                break;
            }
            // tab bar (the header panel's top row; the rects are tab_x())
            if (ly >= tabrow_y() && ly < tabrow_y() + TAB_H) {
                for (int i = 0; i < tab_count; i++) {
                    int tx = tab_x(i);
                    if (lx >= tx && lx < tx + TAB_W) {
                        if (lx >= tx + TAB_W - 22) tab_close(i); else tab_switch(i);
                        break;
                    }
                }
                int ax = tab_x(tab_count);
                if (tab_count < MAX_TABS && lx >= ax && lx < ax + 24) tab_new();
                break;
            }
            // toolbar (the header panel's second row; nav_x()/cmd_x() rects)
            int by = tool_y();
            if (ly >= by && ly < by + TOOL_H) {
                if (lx >= nav_x(0) && lx < nav_x(0) + 26) navigate_back();
                else if (lx >= nav_x(1) && lx < nav_x(1) + 26) navigate_fwd();
                else if (lx >= nav_x(2) && lx < nav_x(2) + 26) navigate_up();
                else {
                    int bx = cmd_x();
                    if (lx >= bx && lx < bx + 60) {
                        // #234i: a DISABLED button must not open its menu. The
                        // draw path greys it on a read-only volume; without
                        // this the menu still opened and the refusal only
                        // arrived one click later, which is the shape that
                        // teaches people to ignore disabled styling.
                        if (ro_block("Creating an item")) break;
                        g_menu = MENU_NEW; g_menu_x = bx; g_menu_y = by + 26; fb_redraw();
                    }
                    else if (lx >= bx + 64 && lx < bx + 124) { g_menu = MENU_VIEW; g_menu_x = bx + 64; g_menu_y = by + 26; fb_redraw(); }
                }
                break;
            }
            // anything else above the body panels (header panel gaps) is inert
            if (ly < body_y()) break;
            // sidebar panel
            if (lx >= side_x() && lx < side_x() + SIDEBAR_W) {
                for (int i = 0; i < side_rows; i++) {
                    if (ly >= side_y[i] - 2 && ly < side_y[i] + ITEM_HEIGHT - 2) {
                        // #250: the eject hot zone is the right-hand end of a
                        // removable row only. Listed before the navigate row
                        // for the same volume, so a click inside it wins.
                        if (side_kind[i] == 5) {
                            if (lx < SIDE_EJECT_X - 4) continue;   // not on the button
                            int vi = side_vol[i];
                            if (vi < 0 || vi >= g_vol_count) break;
                            char mnt[MAX_PATH_LEN];
                            str_copy(mnt, g_vols[vi].mount, MAX_PATH_LEN);
                            char nm[64]; str_copy(nm, g_vols[vi].name, 64);
                            // If we are standing on the volume we are about to
                            // eject, leave FIRST. Otherwise the directory
                            // listing would refresh against a volume that is
                            // gone and show an error instead of the eject
                            // having worked.
                            if (str_eq(CUR.path, mnt)) { g_in_recycle = 0; navigate_to("/"); }
                            if (vol_eject(g_vols[vi].index) == 0)
                                notify_post("Removable drive", "Safe to remove", NOTIFY_SUCCESS);
                            else
                                notify_post("Removable drive", "Eject failed", NOTIFY_ERROR);
                            poll_volumes();
                            fb_redraw();
                        } else if (side_kind[i] == 6) {
                            // Mounted, but its filesystem's file operations do
                            // not exist. Say why rather than opening an empty
                            // window.
                            notify_post("Removable drive",
                                        "This volume's filesystem is recognised but MayteraOS "
                                        "cannot read files from it yet. You can still eject it.",
                                        NOTIFY_WARNING);
                        } else if (side_kind[i] == 3) {
                            // Enter the integrated Recycle Bin view.
                            g_in_recycle = 1; rb_load(); fb_redraw();
                        } else {
                            // Any other Places entry returns to normal browsing.
                            g_in_recycle = 0; navigate_to(side_target[i]);
                        }
                        break;
                    }
                }
                break;
            }
            // recycle view: action toolbar + item list (replaces the file list)
            if (g_in_recycle && lx >= rb_area_x() && ly >= CONTENT_Y) {
                int ax = rb_area_x();
                int by = CONTENT_Y + 5;
                if (ly >= by && ly < by + 24) {
                    int rx = lx - ax;
                    if (rx >= RB_BTN_RESTORE_X && rx < RB_BTN_RESTORE_X + RB_BTN_RESTORE_W) {
                        if (rb_count_selected()) rb_restore_selected();
                        fb_redraw();
                    } else if (rx >= RB_BTN_DELETE_X && rx < RB_BTN_DELETE_X + RB_BTN_DELETE_W) {
                        rb_confirm_delete_selected();
                    } else if (rx >= RB_BTN_EMPTY_X && rx < RB_BTN_EMPTY_X + RB_BTN_EMPTY_W) {
                        rb_confirm_empty();
                    }
                    break;
                }
                int ly0 = rb_list_y(), lh = rb_list_h();
                if (ly >= ly0 && ly < ly0 + lh) {
                    int row = (ly - ly0) / RB_ROW_H + rb_scroll;
                    if (row >= 0 && row < rb_count) {
                        rb_items[row].selected = !rb_items[row].selected;
                        fb_redraw();
                    }
                }
                break;
            }
            // preview panel and the gap before it (ignore clicks)
            if (lx >= cont_x() + cont_w()) break;
            // file list
            if (lx >= list_x() && lx < list_x() + list_w() + SCROLL_GUT && ly >= CONTENT_Y && ly < CONTENT_Y + CONTENT_H) {
                int idx;
                if (g_view == VIEW_ICONS) {
                    int lw = list_w(); int cols = lw / 96; if (cols < 1) cols = 1; int cw = lw / cols, chh = 76;
                    int c = (lx - list_x()) / cw, r = (ly - CONTENT_Y - 6) / chh;
                    idx = (CUR.scroll + r) * cols + c;
                } else idx = (ly - CONTENT_Y) / ITEM_HEIGHT + CUR.scroll;
                if (idx >= 0 && idx < item_count) {
                    CUR.sel = idx;
                    if (right) { g_menu = MENU_CTX; g_menu_x = lx; g_menu_y = ly; fb_redraw(); }
                    else if (items[idx].is_directory) open_selected();
                    else {
                        // Double-click (same item within ~450ms) opens the file
                        // via its association; a single click just previews it.
                        long now = get_ticks();
                        if (idx == g_last_click_idx && (now - g_last_click_ticks) <= 45) {
                            g_last_click_idx = -1;   // consume, avoid triple-trigger
                            open_selected();
                        } else {
                            g_last_click_idx = idx; g_last_click_ticks = now;
                            load_preview(); fb_redraw();
                        }
                    }
                } else if (right) { g_menu = MENU_NEW; g_menu_x = lx; g_menu_y = ly; fb_redraw(); }
            }
        } break;
        case EVENT_MOUSE_MOVE: {
            int wx, wy; win_get_pos(window_handle, &wx, &wy);
            int lx = event.mouse_x, ly = event.mouse_y;
            if (g_openwith_open) {
                int h = ow_hit(lx, ly); int nh = (h >= 0) ? h : -1;
                if (nh != g_ow_hover) { g_ow_hover = nh; fb_redraw(); }
                break;
            }
            if (g_in_recycle) {
                int ly0 = rb_list_y(), lh = rb_list_h();
                int nrh = -1;
                if (lx >= rb_area_x() && ly >= ly0 && ly < ly0 + lh) {
                    nrh = (ly - ly0) / RB_ROW_H + rb_scroll;
                    if (nrh < 0 || nrh >= rb_count) nrh = -1;
                }
                if (nrh != rb_hover) { rb_hover = nrh; fb_redraw(); }
                break;
            }
            int nh = -1;
            if (g_view != VIEW_ICONS && lx >= list_x() && lx < list_x() + list_w()
                && ly >= CONTENT_Y && ly < CONTENT_Y + CONTENT_H) {
                int idx = (ly - CONTENT_Y) / ITEM_HEIGHT + CUR.scroll;
                if (idx >= 0 && idx < item_count) nh = idx;
            }
            if (nh != hover_item) { hover_item = nh; fb_redraw(); }
        } break;
        case EVENT_MOUSE_SCROLL: {
            int d = event.scroll_delta;
            if (g_in_recycle) {
                int visible = rb_list_h() / RB_ROW_H;
                int maxs = rb_count > visible ? rb_count - visible : 0;
                if (d < 0 && rb_scroll > 0) { rb_scroll -= 3; if (rb_scroll < 0) rb_scroll = 0; fb_redraw(); }
                else if (d > 0 && rb_scroll < maxs) { rb_scroll += 3; if (rb_scroll > maxs) rb_scroll = maxs; fb_redraw(); }
                break;
            }
            int visible = CONTENT_H / ITEM_HEIGHT;
            int max_scroll = item_count > visible ? item_count - visible : 0;
            if (d < 0 && CUR.scroll > 0) { CUR.scroll -= 3; if (CUR.scroll < 0) CUR.scroll = 0; fb_redraw(); }
            else if (d > 0 && CUR.scroll < max_scroll) { CUR.scroll += 3; if (CUR.scroll > max_scroll) CUR.scroll = max_scroll; fb_redraw(); }
        } break;
        default: break;
        }
    }
    ns_cancel();   // #netshares: stop the discovery worker + free its sockets
    win_destroy(window_handle);
    return 0;
}
