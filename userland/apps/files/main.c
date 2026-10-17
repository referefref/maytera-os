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
#include "../../libc/userconf.h"   // #peruser2: per-user bin + handoff flag
#include "../../libc/rbindex.h"    // #aidelete: THE shared bin location + row format
#include "../../libc/time.h"       // #aidelete: a delete records WHEN it happened
#include "../../libc/places.h"    // [filesui] the shared named-locations model
#include "../../libc/gui_style.h"
#include "../../libc/contract.h"   // the per-app tool contract (#233), task 3
#include "../../libc/ctpromise.h"  // the escrow backend for fs.paste's promise
#include "netscan.h"   // automatic LAN share discovery (worker thread)

// Route all in-window text through the antialiased TrueType path (matches Settings).
#define win_draw_text(h, x, y, s, c)       win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define win_draw_text_small(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 11, (c))

static int str_eq(const char *a, const char *b);

// ---- layout ---------------------------------------------------------------
static int g_win_w = 820, g_win_h = 560;  // live content size (EVENT_RESIZE)
#define WIN_W        g_win_w
#define WIN_H        g_win_h
// Classic CDE/Motif panel geometry (PAD 10 / TAB_H 26 / PANEL_R 12 /
// PANEL_IN 12). Four beveled panels sit on the plain theme window background:
// a header (tab pills + toolbar), the Places sidebar, the list (with the
// status row inside it) and the optional preview. These rects are shared by
// every draw function AND every hit test below, so kept as-is here.
#define PAD          10      // window margin
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

// Panel rects. ONE definition each: the draw path and every hit test read
// these, so a rect cannot drift between the two.
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

// ---- theme-aware colors ----------------------------------------------------
// Restored to the pre-glass derivation (#filesrevert, 2026-10-01): harmonious
// greys derived from the ACTIVE theme (dark/light by window-bg luminance) with
// a subtle accent tint, instead of a fixed palette that ignored the theme
// entirely. The token NAMES below (C_PANEL, C_CARD, ...) are kept because
// later Files features (the SMB credentials dialog, the Places sidebar, the
// network-share status row) all reference them through draw_panel() /
// draw_dialog_frame() / files_apply_style(); only what they RESOLVE TO changed,
// from a fixed dark-teal hex to a theme_color()-derived value, same as Settings.
static inline unsigned int fp_lum(unsigned int c){int r=(c>>16)&0xFF,g=(c>>8)&0xFF,b=c&0xFF;return (r*30+g*59+b*11)/100;}
static inline int fp_dark(void){ return fp_lum(theme_color(THEME_COLOR_WINDOW_BG)) < 128; }
static inline unsigned int fp_tint(unsigned int base, unsigned int acc, int pct){
    int br=(base>>16)&0xFF,bg=(base>>8)&0xFF,bb=base&0xFF;
    int ar=(acc>>16)&0xFF,ag=(acc>>8)&0xFF,ab=acc&0xFF;
    return ((((br*(100-pct)+ar*pct)/100)&0xFF)<<16)|((((bg*(100-pct)+ag*pct)/100)&0xFF)<<8)|(((bb*(100-pct)+ab*pct)/100)&0xFF);
}
static inline unsigned int fp_acc(void){ return theme_color(THEME_COLOR_ACCENT); }
static inline unsigned int fp_content(void){ return fp_tint(fp_dark()?0x00262A30:0x00F5F6F8, fp_acc(), 5); }
static inline unsigned int fp_panel(void)  { return fp_tint(fp_dark()?0x001E2127:0x00E8EAEE, fp_acc(), 7); }
static inline unsigned int fp_card(void)   { return fp_tint(fp_dark()?0x002C313B:0x00EDEFF3, fp_acc(), 6); }   // popup/dialog surface (was fp_toolbar)
static inline unsigned int fp_field(void)  { return fp_dark()?0x00333A45:0x00FFFFFF; }
static inline unsigned int fp_border(void) { return fp_dark()?0x003A424F:0x00CDD3DB; }
// Row selection: a readable DARK GREY (accent-tinted) on dark themes, a light
// accent-tint on light themes - NOT the raw accent, which rendered near-black on
// Nord and made the selected row + its text unreadable.
static inline unsigned int fp_sel(void){ return fp_dark()
    ? fp_tint(0x003C434F, fp_acc(), 28)
    : fp_tint(0x00CCD6E6, fp_acc(), 26); }

#define C_PANEL       fp_panel()     // the four body panels (header/sidebar/list/preview)
#define C_CARD        fp_card()      // popups, dialogs, scroll track, unselected tab pill
#define C_EDGE        fp_border()    // panel border, hairlines, separators
#define C_EDGE_GLASS  fp_border()    // dialog/popup border (no longer a distinct "glass" stroke)
#define C_ACCENT      fp_acc()
#define C_ERR         theme_color(THEME_COLOR_ERROR)   // tab close x, "(not readable)" note
#define C_IN_FILL     fp_field()
#define C_IN_BORDER   theme_color(THEME_COLOR_TEXTBOX_BORDER)
#define ERR_BAND      0x00A02020    // status-row failure band (#742); unthemed pre-glass too

// Ink for text on a given fill.
static inline unsigned int files_fg(unsigned int bg) {
    int r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    int lum = (r * 30 + g * 59 + b * 11) / 100;
    return lum > 140 ? 0x00181818u : 0x00F0F0F0u;
}
#define C_INK         files_fg(fp_panel())
#define C_ACCENT_INK  files_fg(fp_acc())
#define C_IN_PH       files_dim(fp_field())
#define BG_COLOR      fp_content()
#define SIDEBAR_BG    fp_panel()
#define ITEM_HOVER    fp_tint(fp_content(), fp_acc(), 16)
#define ITEM_SELECTED fp_sel()
#define TEXT_COLOR    files_fg(BG_COLOR)
#define SIDE_TEXT     files_fg(SIDEBAR_BG)
// Section eyebrow / dim-detail ink: same bias-toward-ink derivation pre-glass
// used, so dim text stays readable on both the panel and the selection fill.
static inline unsigned int files_dim(unsigned int bg) {
    unsigned int ink = files_fg(bg);
    int ir=(ink>>16)&0xFF, ig=(ink>>8)&0xFF, ib=ink&0xFF;
    int br=(bg>>16)&0xFF, bgc=(bg>>8)&0xFF, bb=bg&0xFF;
    return ((((ir*5+br*3)/8)&0xFF)<<16) | ((((ig*5+bgc*3)/8)&0xFF)<<8) | (((ib*5+bb*3)/8)&0xFF);
}
#define DIM_TEXT      files_dim(BG_COLOR)
#define SIDE_DIM      files_dim(SIDEBAR_BG)
#define C_INK_DIM     DIM_TEXT
#define C_EYEBROW     SIDE_DIM
#define ICON_FOLDER   0x00FFC800
#define BORDER_COLOR  fp_border()

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

// ---- Quick access --------------------------------------------------------
// [filesui] THE PLACES NOW COME FROM libc/places.h, and this comment records
// what was here, because the fault is worth remembering.
//
// It was two parallel arrays:
//
//     static const char *qa_label[] = { "Home", "Desktop", "Documents", ... };
//     static const char *qa_dir[]   = { "",     "DESKTOP", "DOCUMENT",  ... };
//
// with the note "FAT is 8.3-only so the on-disk names are <=8 chars; we show
// friendly labels". Two things were wrong with that. The 8.3 claim was STALE:
// homes live on the ext2 root (#365), and the FAT ESP holds boot assets only,
// so nothing was forcing DOCUMENT over DOCUMENTS. And a label that does not
// match the directory it opens cannot be checked by reading either array, which
// is how "Documents" sat next to DOCUMENT for as long as it did.
//
// Now the label lives once in libc/places.c and the on-disk name lives once in
// libc/userconf.c, and this app assembles no path at all. The office file
// chooser reads the same table, so Files and Writer cannot drift apart.

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
//
// #peruser2: CLASSIFIED SYSTEM, NOT PER-USER, AND DELIBERATELY NOT RELOCATED.
// The tempting read of this site is "saved mounts are my saved mounts", and it is
// wrong: kernel/net/smb.c:2255 reads /CONFIG/NETMOUNTS.CFG with fat_read_file()
// AT BOOT (smb_restore_persistent_mounts, called from smb.c:2488) to re-create
// the mounts before any account exists. Moving the file into a home would break
// boot-time mount restoration exactly the way moving /THEMES/INDEX.TXT would
// break the kernel's theme_init(). A mount is also machine-global state in this
// kernel: there is one mount table, not one per session.
//
// Two real defects at this site ARE fixed, without moving it:
//   - save_netmounts() called mkdir("/CONFIG", 0755) from Ring 3. A Ring-3 app
//     must not try to create the system configuration directory.
//   - it opened O_WRONLY|O_CREAT with NO O_TRUNC while its own comment claimed
//     "whole-file rewrite", so DELETING a saved location left the tail of the
//     longer previous file behind and load_netmounts() parsed the stale rows
//     back in as real mounts.
// And the FILE MODE is fixed in kernel/fs/perms.c: it had no seed entry, so a
// file holding share passwords IN CLEARTEXT sat on the world-readable no-entry
// default. It is now 0600. The consequence, stated rather than discovered: a
// non-root session can no longer READ the list either, so "/NET" shows no saved
// locations for a non-root user. It could never WRITE them, so the only thing
// lost is the ability to read somebody else's credentials.
//
// Persisted to /CONFIG/NETMOUNTS.CFG (one "label|server|share|user|secret" per
// line) on the ext2 root volume, so they survive reboot.
//
// #smbdisk: THE LAST FIELD IS NO LONGER A CLEARTEXT PASSWORD. It is the NT hash
// in the self-describing form "nt$" + 32 lowercase hex, derived by the kernel
// (net_cred_tag -> SYS_NET_CRED_TAG), because NTLM needs only those 16 bytes
// and there is no MD4 in userland. `nt$` makes an old record and a new record
// tellable apart BY INSPECTION rather than by guessing from length. A legacy
// cleartext record still LOADS (the kernel's mount chokepoint accepts both) and
// is migrated: the kernel rewrites the file at boot, and save_netmounts() below
// upgrades any record it is about to write.
//
// The file STAYS 0600. The NT hash is password-equivalent for SMB
// (pass-the-hash), so this does not make the file safe to expose; what it
// removes is PASSWORD-REUSE blast radius, which is the realistic harm. The virtual "/NET"
// folder lists these plus an "Add Network Location" entry; opening one mounts
// the share (net_mount syscall) and navigates into "/SMB/<server>/<share>".
#define NETMOUNTS_CFG "/CONFIG/NETMOUNTS.CFG"
#define MAX_NETMOUNTS 24
typedef struct {
    char label[40];
    char server[64];
    char share[40];
    char user[40];
    // #smbdisk: named `secret`, not `pass`, because it holds "nt$<32 hex>" (35
    // chars) for every record this build writes. 40 keeps the legacy cleartext
    // bound unchanged so an old file still parses.
    char secret[40];
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
        nm_field(buf, &i, m->secret, sizeof(m->secret));
        while (buf[i] && buf[i] != '\n') i++;
        if (buf[i] == '\n') i++;
        if (m->server[0] && m->share[0]) g_netmount_count++;
    }
}

static void save_netmounts(void) {
    // #peruser2: no mkdir("/CONFIG"), and a real whole-file rewrite. Built in one
    // buffer then handed to userconf_write_all(), which O_TRUNCs only after a
    // successful open (so a refusal leaves the previous list intact), loops on
    // short writes, fsyncs, checks close, and reports a refusal through
    // sys_bootlog. static, not on the stack: a userland thread stack is 16KB.
    static char buf[MAX_NETMOUNTS * 290];
    int o = 0;
    for (int k = 0; k < g_netmount_count; k++) {
        netmount_t *m = &g_netmounts[k];
        // #smbdisk: never WRITE a cleartext secret back out. A record loaded
        // from a legacy file is upgraded here, which is where the write already
        // happens, so no extra write is introduced.
        //
        // ON FAILURE THE FIELD IS LEFT EXACTLY AS IT WAS, deliberately. This
        // function also runs for unrelated reasons (deleting a location), and
        // dropping a credential we merely failed to UPGRADE would turn a save
        // into data loss. The credential-entry path (netmount_add) is where a
        // derivation failure is refused and reported; the kernel's boot-time
        // migration is the sweeping mechanism.
        if (m->secret[0] && !(m->secret[0]=='n' && m->secret[1]=='t' && m->secret[2]=='$')) {
            char up[40];
            if (net_cred_tag(m->secret, up, sizeof(up)) == 0)
                str_copy(m->secret, up, sizeof(m->secret));
            else
                sys_bootlog("[FILES] could not derive the NT hash for a saved "
                            "network location; its record is left unchanged");
        }
        const char *flds[5]; flds[0]=m->label; flds[1]=m->server; flds[2]=m->share;
        flds[3]=m->user; flds[4]=m->secret;
        if (o > (int)sizeof(buf) - 290) break;
        for (int f = 0; f < 5; f++) {
            for (int j = 0; flds[f][j] && j < 270; j++) buf[o++] = flds[f][j];
            buf[o++] = (f < 4) ? '|' : '\n';
        }
    }
    if (userconf_write_all(NETMOUNTS_CFG, buf, (unsigned long)o) != 0) {
        // Reported by userconf_write_all() through sys_bootlog. Expected for a
        // non-root session: this is a SYSTEM file by design (see the block
        // comment above), so the refusal is the boundary working, not a bug.
    }
}

// #smbdisk: returns 0 on success, -1 if the credential could not be stored in
// hashed form. It NEVER falls back to writing the cleartext password: doing so
// would silently reintroduce exactly what this change removes, and silently is
// the worst way for a security property to lapse. The caller reports the
// failure; the location is not saved.
static int netmount_add(const char *server, const char *share,
                        const char *user, const char *pass) {
    load_netmounts();
    if (g_netmount_count >= MAX_NETMOUNTS) return -1;
    netmount_t *m = &g_netmounts[g_netmount_count];
    int l = 0;
    for (int i = 0; share[i] && l < 38; i++) m->label[l++] = share[i];
    if (l < 38) m->label[l++] = '@';
    for (int i = 0; server[i] && l < 39; i++) m->label[l++] = server[i];
    m->label[l] = 0;
    str_copy(m->server, server, sizeof(m->server));
    str_copy(m->share,  share,  sizeof(m->share));
    str_copy(m->user,   user,   sizeof(m->user));
    if (pass && pass[0]) {
        char tag[40];
        if (net_cred_tag(pass, tag, sizeof(tag)) != 0) return -1;
        str_copy(m->secret, tag, sizeof(m->secret));
    } else {
        m->secret[0] = 0;          // no credential = guest, as before
    }
    g_netmount_count++;
    save_netmounts();
    return 0;
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
// #smbdisk: `secret` carries whatever the saved record holds ("nt$<32 hex>" for
// anything this build wrote, a legacy cleartext password for an un-migrated
// file). Both are handed to net_mount() unchanged: the kernel's
// smb_vfs_mount_creds chokepoint understands both forms, so the app does not
// have to know which it is holding.
typedef struct { unsigned char kind; char server[64]; char share[MAX_NAME_LEN]; char user[40]; char secret[40]; char export[MAX_PATH_LEN]; } net_row_t;
static net_row_t g_net_rows[MAX_ITEMS];

static void push_net_row(const char *name, bool is_dir, unsigned char kind,
                         const char *server, const char *share) {
    if (item_count >= MAX_ITEMS) return;
    str_copy(items[item_count].name, name, MAX_NAME_LEN);
    items[item_count].is_directory = is_dir;
    items[item_count].size = 0;
    item_clear_perm(item_count);
    net_row_t *r = &g_net_rows[item_count];
    r->kind = kind; r->server[0] = 0; r->share[0] = 0; r->user[0] = 0; r->secret[0] = 0; r->export[0] = 0;
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
        str_copy(g_net_rows[item_count - 1].secret, g_netmounts[k].secret, 40);
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
// #peruser2: YOUR DELETED FILES ARE YOURS. The bin was /CONFIG/RECYCLE with its
// index at /CONFIG/RBINDEX.TXT, both root-owned, so for a non-root session
// "Delete" could not create the bin, could not write the index, and (#742) the
// move-to-bin path is the one place where a failed write used to mean a
// PERMANENTLY DELETED FILE. It is also a confidentiality problem in the other
// direction: one shared bin means every user's deleted documents sit in a
// directory every other user can list.
//
// The macros are REDEFINED as calls to a resolver rather than replaced at each
// use site, deliberately: there are a dozen uses across THIS app and
// apps/recyclebin, and the two must never disagree about where the bin is. One
// resolver, driven by the same passwd lookup in both, cannot disagree. Root's
// home is "/", so a root session still uses exactly /CONFIG/RECYCLE and
// /CONFIG/RBINDEX.TXT, and root's existing bin is untouched.
//
// No legacy fallback: a deleted FILE is a record, not a preference, and listing
// another principal's deleted documents as your own is precisely the failure the
// no-fallback rule exists for.
// #aidelete: the resolver, the join, the index loader, the lookup and the
// durable remove were ALL byte-for-byte copies of apps/recyclebin's. They are
// one implementation now, in libc/rbindex.c, which carries the #742 discipline
// verbatim (O_TRUNC never unlink-then-create, fsync before the in-RAM copy is
// updated, a non-zero return on any failed step) plus the V1/V2 row parser.
// Deleting the copy IS the fix the old comment above could only ask for: two
// implementations of "where is the bin" is exactly how the two apps come to
// disagree about it.
#define TRASH_DIR    rbindex_dir()
#define TRASH_INDEX  rbindex_path()
#define RB_MAX_ITEMS 128

typedef struct {
    char name[64];
    char original_path[128];
    uint32_t size;
    unsigned long deleted_unix;   // 0 = age unknown (a V1 row); never purged
    char actor[RB_ACTOR_MAX];     // "user", "ai:xxxxxxxx", or "" for no row
    int  selected;
} rb_item_t;

static rb_item_t rb_items[RB_MAX_ITEMS];
static int       rb_count = 0;
static int       rb_scroll = 0;
static int       rb_hover = -1;

#define rb_join(o, c, d, n) rbindex_join((o), (c), (d), (n))
#define rb_idx_load()       (void)rbindex_load()
#define rb_idx_remove(n)    rbindex_remove(n)

static void rb_load(void) {
    rb_count = 0; rb_scroll = 0; rb_hover = -1;
    userconf_ensure_parent_dir(TRASH_DIR);   // #peruser2: <home>/CONFIG
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
        rb_row_t row;
        if (rbindex_lookup(entry.name, &row)) {
            str_copy(it->original_path, row.original_path, sizeof(it->original_path));
            it->deleted_unix = row.deleted_unix;
            str_copy(it->actor, row.actor, sizeof(it->actor));
        } else {
            // No row: nothing recorded where this came from, so it cannot be
            // restored and its age is unknown. Both are shown, neither guessed.
            str_copy(it->original_path, "(unknown)", sizeof(it->original_path));
            it->deleted_unix = 0;
            it->actor[0] = 0;
        }
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
        // #aidelete: rbindex_restore() IS the rename-then-fsynced-copy-then-
        // unlink sequence this loop used to inline, with the row dropped LAST and
        // only on success, so a failed restore still leaves the entry listed and
        // restorable. Flags 0 keeps today's behaviour exactly; the AI undelete
        // path passes RB_RESTORE_NOCLOBBER. One implementation, two policies.
        rb_row_t row;
        str_copy(row.name, rb_items[i].name, sizeof(row.name));
        str_copy(row.original_path, rb_items[i].original_path, sizeof(row.original_path));
        row.deleted_unix = rb_items[i].deleted_unix;
        row.actor[0] = 0;
        if (rbindex_restore(&row, 0) != 0) { failed++; continue; }
        done++;
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
        // #aidelete: rbindex_clear() is the same O_TRUNC + fsync + checked
        // close, with the in-RAM copy updated only on success.
        if (rbindex_clear() != 0)
            files_error("Recycle Bin emptied, but the index could not be cleared", TRASH_INDEX);
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

// Panel chrome (#filesrevert): classic theme draws a flat fill with a 1px
// beveled border (highlight top/left, shadow bottom/right - the CDE/Motif
// raised-panel convention every other classic control in the tree uses, via
// gui_bevel_pair()); a modern theme keeps a softly rounded card. Either way
// the panel sits on the plain window background - there is no backdrop to
// sample any more, so no outer-color guesswork is needed at the edges. `fill`
// is C_PANEL for the four window panels and C_CARD for a floating dialog.
static void draw_panel_fill(int x, int y, int w, int h, uint32_t fill, uint32_t border) {
    if (gui_theme_is_classic()) {
        win_draw_rect(window_handle, x, y, w, h, fill);
        uint32_t shadow, highlight;
        gui_bevel_pair(fill, &shadow, &highlight);
        win_draw_rect(window_handle, x, y, w, 1, highlight);          // top
        win_draw_rect(window_handle, x, y, 1, h, highlight);          // left
        win_draw_rect(window_handle, x, y + h - 1, w, 1, shadow);     // bottom
        win_draw_rect(window_handle, x + w - 1, y, 1, h, shadow);     // right
    } else {
        gui_fill_rounded_aa(window_handle, x, y, w, h, PANEL_R, fill, theme_color(THEME_COLOR_WINDOW_BG));
        gui_rounded_border(window_handle, x, y, w, h, PANEL_R, border);
    }
}
static void draw_panel(int x, int y, int w, int h) { draw_panel_fill(x, y, w, h, C_PANEL, C_EDGE); }

// Sidebar/preview section heading ("Quick access", "This PC", ...): plain
// themed label text, matching the pre-glass sidebar headings (no fixed-hue
// small-caps treatment).
static void draw_eyebrow(int x, int y, const char *label) {
    win_draw_text_small(window_handle, x, y, label, SIDE_DIM);
}

// A row highlight inside a panel: a flat rect on classic, AA-blended on modern.
static void draw_row_fill(int x, int y, int w, int h, uint32_t fill) {
    if (gui_theme_is_classic()) win_draw_rect(window_handle, x, y, w, h, fill);
    else gui_fill_rounded_aa(window_handle, x, y, w, h, ROW_R, fill, C_PANEL);
}

// The list/recycle-view scrollbar: the shared contrast-checked trough/thumb
// pair (gui_scroll.h), the same convention every other classic-or-modern app
// in the tree uses, in the SCROLL_GUT gutter reserved right of the list.
static void draw_scroll_pill(int gx, int y, int h, int scroll, int max_scroll, int visible, int total) {
    int th = (visible * h) / total; if (th < 20) th = 20;
    int ty = max_scroll ? (scroll * (h - th)) / max_scroll : 0;
    uint32_t track, thumb;
    gui_scroll_colors(0, C_PANEL, &track, &thumb);
    win_draw_rect(window_handle, gx, y, SCROLL_GUT, h, track);
    // (#117) see gui_scroll_trough_border()'s own rationale for the bevel.
    gui_scroll_trough_border(window_handle, gx, y, SCROLL_GUT, h, track, C_PANEL);
    win_draw_rect(window_handle, gx + 2, y + ty, SCROLL_GUT - 4, th, thumb);
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
#define TTF_TAB 12   // tab pill labels
// Tab pill i's left edge; the "+" pill sits at tab_x(tab_count). ONE
// definition, read by draw_tabbar() AND the tab-bar click handler.
static int tab_x(int i) { return hdr_x() + PANEL_IN + i * (TAB_W + TAB_GAP); }

// The tab strip on the header panel's top row (#filesrevert): square corners
// on classic (CDE/Motif folder tabs), rounded on modern, matching the
// pre-glass tabbar. The selected tab takes the CONTENT tone so it reads as
// merged with the body below it; the rest take the sidebar/panel tone.
static void draw_tabbar(void) {
    int ty = tabrow_y();
    int rad = gui_theme_is_classic() ? 0 : TAB_H / 2;
    for (int i = 0; i < tab_count; i++) {
        int tx = tab_x(i);
        bool act = (i == active_tab);
        uint32_t tb = act ? BG_COLOR : SIDEBAR_BG;
        gui_fill_rounded_aa(window_handle, tx, ty, TAB_W, TAB_H, rad, tb, C_PANEL);
        if (!act) gui_rounded_border(window_handle, tx, ty, TAB_W, TAB_H, rad, C_EDGE);
        char title[18]; str_copy(title, basename_of(tabs[i].path), 18);
        int lty = ty + (TAB_H - TTF_TAB) / 2 - 1;
        win_draw_text_ttf(window_handle, tx + 12, lty, title, TTF_TAB, act ? TEXT_COLOR : SIDE_TEXT);
        // (#704) tab close affordance: the theme's error/danger token.
        win_draw_text_ttf(window_handle, tx + TAB_W - 18, lty, "x", TTF_TAB, theme_color(THEME_COLOR_ERROR));
    }
    if (tab_count < MAX_TABS) {
        int ax = tab_x(tab_count);
        gui_fill_rounded_aa(window_handle, ax, ty, 24, TAB_H, rad, SIDEBAR_BG, C_PANEL);
        gui_rounded_border(window_handle, ax, ty, 24, TAB_H, rad, C_EDGE);
        win_draw_text_ttf(window_handle, ax + 8, ty + (TAB_H - TTF_TAB) / 2 - 1, "+", TTF_TAB, TEXT_COLOR);
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

// The toolbar row of the header panel: the shared gui_button / gui_textfield2
// widgets, unchanged, rendering under the theme palette files_apply_style()
// installs below.
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

// Sidebar row geometry relative to the sidebar panel's left
// edge: the current-entry pill is inset 6px from the panel (radius ROW_R),
// the icon sits 12px in, the label 34px in, the eject glyph 22px from the
// right edge. The row's hit band is unchanged (side_y[i] - 2 .. + ITEM_HEIGHT - 2).
#define SIDE_ICON_X   (side_x() + 12)
#define SIDE_LABEL_X  (side_x() + 34)
#define SIDE_EJECT_X  (side_x() + SIDEBAR_W - 22)
static void side_row_fill(int y) { draw_row_fill(side_x() + 6, y - 2, SIDEBAR_W - 12, ITEM_HEIGHT, ITEM_SELECTED); }

// ---- ONE dispatcher for a sidebar row, with TWO callers -------------------
//
// THIS IS THE POINT OF THE WHOLE FILES CONTRACT, so it is worth stating. The
// sidebar CLICK path has never been tested: QEMU pointer injection does not
// reliably land a click (#334), so every previous verification of this app drove
// the keyboard instead and the sidebar was argued to be correct rather than
// shown to be. The argument was that it shares places_path() with the keyboard
// path, which is true and is not the same thing as a test.
//
// Extracting this function is what turns the contract into a PROOF ABOUT THE
// CLICK rather than a proof about a parallel copy of it. EVENT_MOUSE_DOWN's
// sidebar branch calls exactly this, and so does the projected contract action,
// with the same kind / target / volume the draw pass recorded. There is no
// second navigate, no second eject and no second "this volume is not readable"
// message, so `app.action places.documents` exercising this function is evidence
// about the mouse route, because there is only one route.
//
// Returns 0 when the row did what it says, nonzero when it refused, and writes a
// short machine-readable reason into out[] for the contract reply.
static int side_activate(int kind, const char *target, int vol, char *out, int ocap) {
    if (out && ocap > 0) out[0] = 0;
    switch (kind) {
    case 5: {   // the eject affordance on a removable row
        if (vol < 0 || vol >= g_vol_count) {
            if (out) snprintf(out, (size_t)ocap, "no-such-volume");
            return -1;
        }
        char mnt[MAX_PATH_LEN]; str_copy(mnt, g_vols[vol].mount, MAX_PATH_LEN);
        // If we are STANDING on the volume we are about to eject, leave FIRST.
        // Otherwise the listing refreshes against a volume that is gone and
        // shows an error instead of the eject having worked.
        if (str_eq(CUR.path, mnt)) { g_in_recycle = 0; navigate_to("/"); }
        if (vol_eject(g_vols[vol].index) == 0) {
            notify_post("Removable drive", "Safe to remove", NOTIFY_SUCCESS);
            poll_volumes(); fb_redraw();
            if (out) snprintf(out, (size_t)ocap, "ejected mount=%s", mnt);
            return 0;
        }
        notify_post("Removable drive", "Eject failed", NOTIFY_ERROR);
        poll_volumes(); fb_redraw();
        if (out) snprintf(out, (size_t)ocap, "eject-failed mount=%s", mnt);
        return -1;
    }
    case 6:     // mounted, but its filesystem has no file operations
        notify_post("Removable drive",
                    "This volume's filesystem is recognised but MayteraOS "
                    "cannot read files from it yet. You can still eject it.",
                    NOTIFY_WARNING);
        if (out) snprintf(out, (size_t)ocap, "not-readable");
        return -1;
    case 3:     // the integrated Recycle Bin view, not a directory
        g_in_recycle = 1; rb_load(); fb_redraw();
        if (out) snprintf(out, (size_t)ocap, "recycle items=%d", rb_count);
        return 0;
    default:    // every other Places / device row returns to normal browsing
        g_in_recycle = 0;
        navigate_to(target);
        if (out) snprintf(out, (size_t)ocap, "path=%s items=%d", CUR.path, item_count);
        return 0;
    }
}

static void draw_sidebar(void) {
    draw_panel(side_x(), body_y(), SIDEBAR_W, body_h());
    side_rows = 0;
    int ex = side_x() + PANEL_IN;        // eyebrow x
    int y = body_y() + 10;
    // [filesui] One loop over libc/places.h, which already filtered itself for
    // this session, and which supplies the heading text as well as the rows so
    // that "Quick access" is not spelled here and differently in the chooser.
    // A heading is drawn when the section CHANGES, so a section that turns out
    // to be empty (no /MEDIA on this image) leaves no orphan header behind.
    int last_sect = -1;
    for (int i = 0; i < places_count() && side_rows < 30; i++) {
        const place_t *pl = places_at(i);
        char p[MAX_PATH_LEN];
        if (!pl || places_path(i, p, MAX_PATH_LEN) != 0) continue;
        if ((int)pl->sect != last_sect) {
            if (last_sect >= 0) y += 6;
            draw_eyebrow(ex, y, places_section_label(pl->sect)); y += 20;
            last_sect = (int)pl->sect;
        }
        bool cur = str_eq(CUR.path, p);
        if (cur) side_row_fill(y);
        uint32_t tint = cur ? files_fg(ITEM_SELECTED) : SIDE_TEXT;
        if (!draw_mico(pl->icon, SIDE_ICON_X, y, ICON_SIZE, tint)) draw_folder_icon(SIDE_ICON_X, y);
        win_draw_text(window_handle, SIDE_LABEL_X, y, pl->label, tint);
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
// The list panel: the rows sit directly on the panel fill; a
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
    // The scrollbar is the shared contrast-checked trough/thumb pair, in the
    // same SCROLL_GUT gutter reserved right of the list.
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

// The Recycle view takes the list panel, which spans to the
// right margin while g_in_recycle (prev_on() is false); its rows use the
// list's own inset (list_x / list_w plus the scroll gutter).
static int rb_area_x(void) { return list_x(); }
static int rb_area_w(void) { return list_w() + SCROLL_GUT; }
static int rb_list_y(void) { return CONTENT_Y + RB_TOOLBAR_H + RB_HEADER_H; }
static int rb_list_h(void) { return CONTENT_H - RB_TOOLBAR_H - RB_HEADER_H; }

// #aidelete: ONE definition of the five column x offsets (relative to the view's
// left edge), read by the header and by every row. Size stays where it was,
// at aw-70; By and Deleted are carved out to its left, and Original Location
// keeps the left half minus what the two new columns need.
static int rb_col_by(int aw)   { return aw - 112; }
static int rb_col_when(int aw) { return aw - 230; }
static int rb_col_path(int aw) {
    int p = aw / 2 - 40;
    if (p < 120) p = 120;
    if (p > rb_col_when(aw) - 40) p = rb_col_when(aw) - 40;
    return p;
}

// "You" or "AI", with the AI task id kept for the detail line. The distinction
// IS the requirement: a bin where an AI delete and a user delete look identical
// announces nothing.
static const char *rb_actor_label(const rb_item_t *it) {
    if (rbindex_actor_is_ai(it->actor)) return "AI";
    if (it->actor[0] == 0) return "-";
    return "You";
}

// A 0 timestamp is a V1 row: a file that was in the bin before the index
// carried a time. It renders as "unknown", NEVER as a 1970 date, because a
// made-up date beside a retention setting would read as "about to be purged"
// and the rule is the exact opposite (an unknown age is never purged).
static void rb_fmt_when(unsigned long t, char *buf, int cap) {
    if (cap < 17) { if (cap > 0) buf[0] = 0; return; }
    if (t == 0) { str_copy(buf, "unknown", (unsigned)cap); return; }
    time_t tv = (time_t)t;
    struct tm tmv;
    if (!localtime_r(&tv, &tmv)) { buf[0] = '?'; buf[1] = 0; return; }
    int y = tmv.tm_year + 1900, mo = tmv.tm_mon + 1, d = tmv.tm_mday;
    int hh = tmv.tm_hour, mi = tmv.tm_min;
    buf[0]  = (char)('0' + (y / 1000) % 10);
    buf[1]  = (char)('0' + (y / 100) % 10);
    buf[2]  = (char)('0' + (y / 10) % 10);
    buf[3]  = (char)('0' + y % 10);
    buf[4]  = '-';
    buf[5]  = (char)('0' + mo / 10); buf[6] = (char)('0' + mo % 10);
    buf[7]  = '-';
    buf[8]  = (char)('0' + d / 10);  buf[9] = (char)('0' + d % 10);
    buf[10] = ' ';
    buf[11] = (char)('0' + hh / 10); buf[12] = (char)('0' + hh % 10);
    buf[13] = ':';
    buf[14] = (char)('0' + mi / 10); buf[15] = (char)('0' + mi % 10);
    buf[16] = 0;
}

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

    // Column header. #aidelete: Deleted and By join Name / Original Location /
    // Size. The three right-hand columns are pinned to the right edge and the
    // two left-hand ones take what is left, so the layout survives a resize;
    // the row loop below reads the SAME helpers, so a header and its column
    // cannot drift apart.
    int hy = CONTENT_Y + RB_TOOLBAR_H;
    win_draw_text_small(window_handle, ax + 30, hy + 5, "Name", DIM_TEXT);
    win_draw_text_small(window_handle, ax + rb_col_path(aw), hy + 5, "Original Location", DIM_TEXT);
    win_draw_text_small(window_handle, ax + rb_col_when(aw), hy + 5, "Deleted", DIM_TEXT);
    win_draw_text_small(window_handle, ax + rb_col_by(aw), hy + 5, "By", DIM_TEXT);
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
        // Checkbox: a hollow 2px frame, no fill; the check mark in the
        // row's own ink so it reads on the selection fill.
        gui_draw_rect_outline(window_handle, ax + 8, y + 4, 16, 16, it->selected ? tint : C_EDGE_GLASS);
        gui_draw_rect_outline(window_handle, ax + 9, y + 5, 14, 14, it->selected ? tint : C_EDGE_GLASS);
        if (it->selected) {
            win_draw_rect(window_handle, ax + 11, y + 9, 10, 2, tint);
            win_draw_rect(window_handle, ax + 13, y + 7, 2, 10, tint);
        }
        // Name (trimmed)
        char nm[64]; int j = 0; while (it->name[j] && j < 60) { nm[j] = it->name[j]; j++; } nm[j] = 0;
        int namecol = rb_col_path(aw) - 30 - 8;
        if (gui_ttf_width(nm, 14) > namecol) {
            while (j > 3 && gui_ttf_width(nm, 14) > namecol - 8) nm[--j] = 0;
            if (j >= 2) { nm[j-1] = '.'; nm[j] = '.'; nm[j+1] = 0; }
        }
        win_draw_text(window_handle, ax + 30, y + 4, nm, tint);
        // Original location (trimmed)
        char op[80]; j = 0; while (it->original_path[j] && j < 76) { op[j] = it->original_path[j]; j++; } op[j] = 0;
        int opcol = rb_col_when(aw) - rb_col_path(aw) - 8;
        if (gui_ttf_width(op, 11) > opcol) {
            while (j > 3 && gui_ttf_width(op, 11) > opcol - 8) op[--j] = 0;
            if (j >= 2) { op[j-1] = '.'; op[j] = '.'; op[j+1] = 0; }
        }
        win_draw_text_small(window_handle, ax + rb_col_path(aw), y + 6, op, dimc);
        // #aidelete: Deleted (when) and By (who). An AI delete is drawn in the
        // warning ink on an unselected row so it stands out in a list of "You"
        // rows; the selection fill wins where it would otherwise hurt contrast.
        char wh[20]; rb_fmt_when(it->deleted_unix, wh, (int)sizeof(wh));
        win_draw_text_small(window_handle, ax + rb_col_when(aw), y + 6, wh, dimc);
        {
            int ai = rbindex_actor_is_ai(it->actor);
            uint32_t bc = it->selected ? tint : (ai ? theme_color(THEME_COLOR_WARNING) : dimc);
            win_draw_text_small(window_handle, ax + rb_col_by(aw), y + 6,
                                rb_actor_label(it), bc);
        }
        // Size
        char ss[24]; fmt_size(it->size, ss);
        win_draw_text_small(window_handle, ax + aw - 70, y + 6, ss, dimc);
    }
    // Scrollbar: the same shared trough/thumb pair the list view draws.
    if (rb_count > visible)
        draw_scroll_pill(ax + aw - SCROLL_GUT, ly, lh, rb_scroll, rb_count - visible, visible, rb_count);
}

// ---- THE CONTROL TABLE, and why every menu is now one ---------------------
//
// A row of fctl_t carries FOUR things about one discrete control, in ONE place:
// what the user READS (label), what the contract NAMES it (tok), what a click
// RUNS (fn or afn), and the contract TERMS (timing / success / depth / promise).
//
// Three readers walk it and there is no fourth description anywhere:
//   draw_menu()      reads .label
//   menu_select()    calls .fn
//   files_project()  emits .tok / .desc / .terms
//
// So a control that is not in a table cannot be drawn, cannot be clicked and
// cannot be described. That is completeness by construction rather than by
// inspection, which is the property #235 lacked: 120 of its 150 hand-written
// capability tokens matched no code, because a hand-written surface is a COPY of
// the app and the copy is what goes stale.
//
// WHAT THIS REPLACED. Three parallel `const char *[]` arrays plus a switch on
// the INDEX into one of them, carrying the comment "menu_select's indices below
// must stay in sync with this order" - a hand-maintained correspondence between
// a draw table and a dispatch table, i.e. exactly the #227 fault (five controls
// had drifted out of reach) one layer up. The context menu had already been
// converted to match by LABEL for that reason ("so the action stays correct as
// items are added/removed"), which is the same instinct arriving at a string
// comparison instead of at a table.
//
// EXACTLY ONE of fn / afn is set. `fn` is a control a click runs with no
// argument; `afn` is a control that needs a VALUE (the filter text, a path, a
// picker row), which a click supplies from a field and a contract call supplies
// from argv. A separator is label "--" with everything else zero, so it draws as
// a rule, is not clickable, and is not projected.
typedef struct {
    const char *label;                 // what the user reads; "--" = separator
    const char *tok;                   // contract item name; 0 for a separator
    void      (*fn)(void);
    int       (*afn)(int argc, char **argv, char *out, int ocap);
    const char *desc;                  // the one line `describe` prints
    const ct_terms_t *terms;           // declared in the SAME braces as the handler
} fctl_t;

// The menu currently open, and its length. Defined with the tables below,
// forward-declared here because draw_menu()/menu_hit() sit above them.
static const fctl_t *active_menu(int *n);
static void draw_menu(void) {
    int n = 0;
    const fctl_t *m = active_menu(&n); if (!m) return;
    int mw = 150, mh = n * 22 + 6;
    int mx = g_menu_x, my = g_menu_y;
    if (mx + mw > WIN_W) mx = WIN_W - mw - 2;
    if (my + mh > WIN_H) my = WIN_H - mh - 2;
    // A popup menu is a square themed card (matches Settings' own popups);
    // separators are the panel hairline.
    win_draw_rect(window_handle, mx, my, mw, mh, theme_color(THEME_COLOR_MENU_BG));
    gui_draw_rect_outline(window_handle, mx, my, mw, mh, C_EDGE);
    int yy = my + 3;
    for (int i = 0; i < n; i++) {
        if (!m[i].tok) { win_draw_rect(window_handle, mx + 4, yy + 10, mw - 8, 1, C_EDGE); yy += 22; continue; }
        win_draw_text(window_handle, mx + 10, yy + 4, m[i].label, files_fg(theme_color(THEME_COLOR_MENU_BG)));
        yy += 22;
    }
}

// ---- status row -----------------------------------------------------------
// Lives INSIDE the list panel, above its bottom edge, under a hairline inset
// by PANEL_R so it never meets the rounded corners.
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

// Map the Files theme palette into the shared style engine each redraw, so the
// gui_* primitives (buttons, fields, tabs, the confirm dialog) match the active
// theme and render modern (rounded/AA) or classic (beveled) like the Settings
// app. Restored from the pre-glass derivation (#filesrevert); the glass commit
// had pinned this to fixed dark tokens and GUI_STYLE_MODERN always.
static void files_apply_style(void) {
    gui_set_style(gui_theme_is_classic() ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface        = BG_COLOR;
    p.surface_raised = C_CARD;
    p.ink            = TEXT_COLOR;
    p.ink_dim        = DIM_TEXT;
    p.accent         = fp_acc();
    p.accent_hover   = gui_lighten(fp_acc(), 24);
    p.border         = theme_color(THEME_COLOR_TEXTBOX_BORDER);
    p.field_bg       = C_IN_FILL;
    p.field_border   = theme_color(THEME_COLOR_TEXTBOX_BORDER);
    p.track          = fp_tint(BG_COLOR, fp_acc(), 30);
    gui_set_palette(&p);
}

// Full redraw. The window is never translucent: every pixel the panels cover
// is drawn fresh each call (no backdrop/blit step - that was the glass-only
// mechanism, removed along with the fixed dark palette, #filesrevert).
static void fb_redraw(void) {
    // NO WINDOW, NOTHING TO DRAW. A `--contract` invocation answers and exits
    // before win_create() (contract-lint check 8), and a contract ACTION runs
    // the same handler a click runs, so every handler that ends in fb_redraw()
    // is reached with window_handle == -1 on the spawn path. Returning here is
    // what lets ONE function serve both the click and the contract call instead
    // of a windowless copy of each handler, which is the duplication this whole
    // ticket is about.
    if (window_handle < 0) return;
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
// WHAT THE CONTRACT LETS A CALLER GO AND LOOK AT.
//
// A contract ACTION must name a postcondition somebody can check
// (contract.h ct_terms_t.success), and "the function returned" is not one:
// do_new_folder() returns void whether it created a folder or hit five taken
// names, and do_paste() returns void whether it copied a file or found an empty
// clipboard. These two strings are the observable both actions were missing.
//
// THEY ARE SET ONLY ON GENUINE SUCCESS, and that is load-bearing rather than
// tidy. The measured fault in the commit before this one was an action that
// promised a file, produced none, and still reported `promise=fulfilled
// paths=0/0`, because its postcondition template resolved to a human
// placeholder, failed the absolute-path test, and was therefore never
// registered: a verifier asked nothing answers yes. So these stay at the
// PLACEHOLDER until the operation has actually landed, which is exactly what
// makes the promise on fs.paste falsifiable: a paste that does nothing leaves
// "(none)" here, ctpromise.c cannot pin it to an absolute path, and the verdict
// is UNMET with declared_fs=1 unresolved=1 rather than a vacuous yes.
#define FILES_NOTHING_YET "(none)"
static char g_last_created[MAX_PATH_LEN] = FILES_NOTHING_YET;
static char g_last_written[MAX_PATH_LEN] = FILES_NOTHING_YET;

static void do_new_folder(void) {
    if (ro_block("Creating a folder")) return;
    static const char *cand[] = { "NEWFOLD", "NEWFOLD1", "NEWFOLD2", "NEWFOLD3", "NEWFOLD4" };
    char p[MAX_PATH_LEN];
    for (int i = 0; i < 5; i++) { path_join(p, CUR.path, cand[i]);
        if (mkdir(p, 0755) == 0) { str_copy(g_last_created, p, MAX_PATH_LEN); break; } }
    load_directory(CUR.path); fb_redraw();
}
static void do_new_file(void) {
    if (ro_block("Creating a file")) return;
    static const char *cand[] = { "NEW.TXT", "NEW1.TXT", "NEW2.TXT", "NEW3.TXT" };
    char p[MAX_PATH_LEN];
    for (int i = 0; i < 4; i++) { path_join(p, CUR.path, cand[i]);
        int fd = open(p, 0x41);
        if (fd >= 0) { close(fd); str_copy(g_last_created, p, MAX_PATH_LEN); break; } }
    load_directory(CUR.path); fb_redraw();
}
// Append one index record, durably. Returns 0 only if the record is on the
// medium. Without the index a trashed file shows as "(unknown)" and can never
// be restored, so a silent failure here is a quieter version of the same data
// loss.
//
// #aidelete: THIS IS NOW A V2 ROW: "<name>|<path>|<deleted_unix>|user". Two
// things changed and both matter.
//
// 1. IT RECORDS WHEN. Retention is impossible without a timestamp, which is
//    why none existed: the row was two fields and apps/recyclebin set
//    deleted_time = 0 because there was nowhere to read one from. A row written
//    from here carries time(0), so the recycler can age it out; a row that
//    predates this change still carries no time, and an unknown age is NEVER
//    purged rather than guessed at.
// 2. IT RECORDS WHO, as "user". This is the half of the owner's requirement
//    that this app owns: a delete the person performed is attributed to the
//    person, so that an "ai:XXXXXXXX" row written by the kernel's escrow path
//    is visibly a different thing in the Recycle Bin rather than
//    indistinguishable from it.
//
// A clock that is not set yet returns 0 from time(), and 0 is exactly the
// "unknown age, never purge" encoding, so an unset clock fails safe: the file
// is kept forever rather than aged from epoch and purged on the first sweep.
static int trash_index_add(const char *name, const char *src) {
    time_t now = time(0);
    unsigned long when = (now > 0) ? (unsigned long)now : 0UL;
    return rbindex_append(name, src, when, "user");
}

static void do_delete(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    if (ro_block("Deleting")) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)")) return;
    char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];
    path_join(src, CUR.path, it->name);
    // #peruser2: mkdir("/CONFIG", 0755) IS GONE. A Ring-3 app must not try to
    // create the system configuration directory (it holds SHADOW, MFA.DB,
    // PERMS.DB and the consent store); the per-user parent is made instead.
    userconf_ensure_parent_dir(TRASH_DIR);
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
            if (net_mount(r->server, r->share, r->user, r->secret) == 0) {
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
    // [deadbtn] Open With named /APPS/PYTHON.ELF (among others) and never
    // looked at sys_spawn_args()'s return: on a build that does not carry the
    // app (e.g. a from-scratch public image with no PYTHON.ELF, see
    // build/app-presence.policy), picking it did nothing visible at all.
    if (sys_spawn_args(OW_APPS[idx].path, av, 2) < 0)
        notify_post("Cannot open with", OW_APPS[idx].label, NOTIFY_ERROR);
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
    int landed = 0;
    if (g_clip_mode == 2) {
        // Cut = move. rename() handles files and directories on the same volume;
        // fall back to copy+unlink for the file case if rename is unavailable.
        if (rename(g_clip_path, dst) == 0) landed = 1;
        else if (copy_file(g_clip_path, dst) == 0) { unlink(g_clip_path); landed = 1; }
        g_clip_mode = 0; g_clip_path[0] = 0;   // a cut item is consumed once pasted
    } else {
        // copy: files only (a directory is a no-op, and a no-op must NOT report
        // a destination, or the promise on this action becomes vacuous)
        if (copy_file(g_clip_path, dst) == 0) landed = 1;
    }
    if (landed) str_copy(g_last_written, dst, MAX_PATH_LEN);
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
// A floating dialog: a themed card panel (classic beveled / modern rounded,
// via draw_panel_fill), its title on the first row over a hairline. The
// content still starts at by + 34 (where the old 22px title band ended), so
// every hit-test offset below is unchanged.
static void draw_dialog_frame(int bx, int by, int bw, int bh, const char *title) {
    draw_panel_fill(bx, by, bw, bh, C_CARD, C_EDGE);
    win_draw_text(window_handle, bx + PANEL_IN, by + 6, title, files_fg(C_CARD));
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
    // field, so it takes the themed input tokens from the palette.
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
    if (g_cred_save && netmount_add(g_cred_server, g_cred_share,
                                    g_cred_user, g_cred_pass) != 0) {
        // #smbdisk: the mount itself SUCCEEDED, so say what did not. The
        // alternative (writing the password in cleartext so the save appears to
        // work) is the thing this change exists to stop.
        files_error("Saved location not created",
                    "the share is mounted, but its credential could not be "
                    "stored in hashed form, so it was not saved");
    }
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
// ===========================================================================
// THE CONTRACT SURFACE (#233 / docs/CONTRACT_COVERAGE_PLAN.md task 3, phase 1)
//
// Owner directive: "every manifest and every app must expose every single menu
// item, option, function and feature through the manifest API with contract
// terms for timing, depth, promise, success failure etc."
//
// EVERY ACTION ROW BELOW IS PROJECTED FROM A TABLE THIS APP ALREADY DRAWS AND
// DISPATCHES FROM. Nothing here is a hand-written list of what Files can do, and
// that is the whole difference from #235, which deleted 27 manifests declaring
// 150 capability tokens of which 120 matched no code. A projection cannot
// declare a control the app does not have, because the projection and the draw
// pass read the same braces.
//
// The VALUE rows (FILES_ITEMS[] at the bottom) are not projected, and they are
// not controls: they are the readable state a caller checks a control AGAINST.
// Their terms are DERIVED by contract_terms_of(), so none of them can carry a
// wrong timing, success or depth, and none of them needed writing out.
// ===========================================================================

// ---- the wrappers the tables need -----------------------------------------
// A menu item that sets a variable had its assignment inline in a switch on the
// row's INDEX. A one-line function per row is what makes the row self-contained,
// and self-contained is what removes the "indices must stay in sync" comment.
static void act_view_icons(void)     { g_view = VIEW_ICONS; }
static void act_view_list(void)      { g_view = VIEW_LIST; }
static void act_view_details(void)   { g_view = VIEW_DETAILS; }
static void act_sort_name(void)      { g_sort = SORT_NAME; sort_items(); }
static void act_sort_size(void)      { g_sort = SORT_SIZE; sort_items(); }
static void act_sort_type(void)      { g_sort = SORT_TYPE; sort_items(); }
static void act_sort_attr(void)      { g_sort = SORT_ATTR; sort_items(); }
static void act_toggle_preview(void) { g_preview_on = !g_preview_on; }
static void act_toggle_hidden(void)  { g_show_hidden = !g_show_hidden; load_directory(CUR.path); }
static void act_props_open(void)     { g_props_open = 1; }
static void act_zip(void)            { do_compress(".zip"); }
static void act_targz(void)          { do_compress(".tar.gz"); }
static void act_menu_new(void)       { if (!ro_block("Creating an item")) g_menu = MENU_NEW; }
static void act_menu_view(void)      { g_menu = MENU_VIEW; }
static void act_menu_ctx(void)       { g_menu = MENU_CTX; }
static void act_menu_close(void)     { g_menu = MENU_NONE; }
static void act_recycle_enter(void)  { g_in_recycle = 1; rb_load(); fb_redraw(); }
static void act_recycle_leave(void)  { if (g_in_recycle) { g_in_recycle = 0; navigate_to(CUR.path); } }
static void act_rb_restore(void)     { if (rb_count_selected()) rb_restore_selected(); fb_redraw(); }
static void act_rb_select_all(void)  { for (int i = 0; i < rb_count; i++) rb_items[i].selected = 1; fb_redraw(); }
static void act_rb_select_none(void) { for (int i = 0; i < rb_count; i++) rb_items[i].selected = 0; fb_redraw(); }
static void act_tab_new(void)        { tab_new(); }
static void act_tab_close(void)      { tab_close(active_tab); }
static void act_sel_next(void)       { if (CUR.sel < item_count - 1) { CUR.sel++; load_preview(); fb_redraw(); } }
static void act_sel_prev(void)       { if (CUR.sel > 0) { CUR.sel--; load_preview(); fb_redraw(); } }
static void act_activate(void)       { open_selected(); load_preview(); fb_redraw(); }
static void act_filter_clear(void)   { g_filter[0] = 0; load_directory(CUR.path); fb_redraw(); }
static void act_dialog_close(void)   {
    // ONE dismiss for every overlay, in the same precedence order
    // EVENT_KEY_DOWN's Esc handling uses, so the contract cannot close a
    // different dialog from the one Esc would.
    if (g_cred_open) g_cred_open = 0;
    else if (g_te_open) g_te_open = 0;
    else if (g_openwith_open) g_openwith_open = 0;
    else if (g_props_open) g_props_open = 0;
    else if (g_menu != MENU_NONE) g_menu = MENU_NONE;
    fb_redraw();
}
static void act_props_perms(void)    { if (g_props_hotspot_kind == 1) open_perms_edit();
                                       else if (g_props_hotspot_kind == 2) props_ro_toggle(); }
static void act_net_rescan(void)     { if (ns_state() == NS_SCANNING) ns_cancel(); else ns_start(); }

// ---- the arg-taking controls ----------------------------------------------
// The three places Files takes a VALUE from the user rather than a click: the
// path it navigates to, the filter it types, and the rename field. A click
// supplies the value from a field; a contract call supplies it from argv. Same
// handler either way.
static int afn_goto(int argc, char **argv, char *out, int ocap) {
    if (argc < 1 || !argv[0] || argv[0][0] != '/') {
        snprintf(out, (size_t)ocap, "need-absolute-path");
        return -1;
    }
    g_in_recycle = 0;
    navigate_to(argv[0]);
    snprintf(out, (size_t)ocap, "path=%s items=%d", CUR.path, item_count);
    return 0;
}
static int afn_filter_set(int argc, char **argv, char *out, int ocap) {
    if (argc < 1 || !argv[0]) { snprintf(out, (size_t)ocap, "need-text"); return -1; }
    str_copy(g_filter, argv[0], (int)sizeof(g_filter));
    load_directory(CUR.path); fb_redraw();
    snprintf(out, (size_t)ocap, "filter=%s items=%d", g_filter, item_count);
    return 0;
}
static int afn_rename_to(int argc, char **argv, char *out, int ocap) {
    if (argc < 1 || !argv[0] || !argv[0][0]) { snprintf(out, (size_t)ocap, "need-name"); return -1; }
    if (CUR.sel < 0 || CUR.sel >= item_count) { snprintf(out, (size_t)ocap, "no-selection"); return -1; }
    // rename_commit() is the SAME function the Rename dialog's OK button calls,
    // reached with the name from argv instead of from the text field.
    //
    // AND THE RESULT IS CHECKED, not assumed. rename_commit() returns void and
    // silently no-ops on a read-only volume, on "..", on "(empty)" and on an
    // ext2 rename that fails after the copy fallback also fails, so "we called
    // it" is not the claim "it is renamed". The relisted directory is the
    // postcondition: the new name has to actually be in it.
    files_err_clear();
    rename_commit(argv[0]);
    for (int i = 0; i < item_count; i++) {
        if (!str_eq(items[i].name, argv[0])) continue;
        CUR.sel = i; fb_redraw();
        snprintf(out, (size_t)ocap, "renamed to=%s row=%d items=%d", argv[0], i, item_count);
        return 0;
    }
    snprintf(out, (size_t)ocap, "not-renamed name=%s%s%s", argv[0],
             g_err[0] ? " err=" : "", g_err[0] ? g_err : "");
    return -1;
}
static int afn_select(int argc, char **argv, char *out, int ocap) {
    if (argc < 1 || !argv[0]) { snprintf(out, (size_t)ocap, "need-index"); return -1; }
    int v = atoi(argv[0]);
    if (v < -1 || v >= item_count) { snprintf(out, (size_t)ocap, "out-of-range 0..%d", item_count - 1); return -1; }
    CUR.sel = v;
    if (v >= 0) load_preview();
    fb_redraw();
    snprintf(out, (size_t)ocap, "selected=%d name=%s", v,
             (v >= 0 && v < item_count) ? items[v].name : "(none)");
    return 0;
}
static int afn_tab_switch(int argc, char **argv, char *out, int ocap) {
    if (argc < 1 || !argv[0]) { snprintf(out, (size_t)ocap, "need-index"); return -1; }
    int v = atoi(argv[0]);
    if (v < 0 || v >= tab_count) { snprintf(out, (size_t)ocap, "out-of-range 0..%d", tab_count - 1); return -1; }
    tab_switch(v);
    snprintf(out, (size_t)ocap, "tab=%d path=%s", active_tab, CUR.path);
    return 0;
}

// ---- THE TERMS, one object per distinct shape of consequence ---------------
// Shared by every row that has the same consequence, because a term duplicated
// per row is a term that can disagree with itself. Every success_ref names a
// contract item that really exists (contract-lint check 6a fails the build on
// one that does not, which is the #235 fault reduced to a single field).
static const ct_terms_t k_t_navigate = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_NOTFOUND | CT_FAIL_IO, "nav.path", 0
};
static const ct_terms_t k_t_view_mode = {
    CT_TIME_INSTANT, 0, 0, CT_SUCC_ITEM, CT_DEPTH_PREF, 0, "view.mode", 0
};
static const ct_terms_t k_t_view_sort = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_PREF, 0, "view.sort", 0
};
static const ct_terms_t k_t_view_preview = {
    CT_TIME_INSTANT, 0, 0, CT_SUCC_ITEM, CT_DEPTH_PREF, 0, "view.preview_pane", 0
};
static const ct_terms_t k_t_view_hidden = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_PREF, CT_FAIL_IO, "view.show_hidden", 0
};
// A create names the path it made through fs.last_created. NO escrow promise,
// and the reason is worth stating rather than leaving as an omission: both
// creates CHOOSE their own name from a candidate list (NEWFOLD..NEWFOLD4), so
// there is no path a caller could name in advance and no template that resolves
// at OPEN, which is when the escrow takes its before-snapshot. Declaring a
// promise whose postcondition can only be pinned afterwards is exactly how a
// verdict becomes vacuous, so this declares an observable instead of a promise.
static const ct_terms_t k_t_create = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_FS,
    CT_FAIL_DENIED | CT_FAIL_NOSPACE | CT_FAIL_IO, "fs.last_created", 0
};
static const ct_terms_t k_t_clip = {
    CT_TIME_INSTANT, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_NOTFOUND, "clipboard.source", 0
};
// THE ONE PROMISE IN THIS APP, and the one action where a promise is honest.
// A paste has a destination the app can name: the folder it is pasting INTO is
// the escrow scope, and the file it wrote is reported through fs.last_written.
// A paste that does nothing (empty clipboard, read-only volume, a directory in
// copy mode) leaves fs.last_written at "(none)", which is not an absolute path,
// so ctpromise.c cannot register the postcondition and reports UNMET with
// declared_fs=1 unresolved=1. That is the point: the postcondition names
// something that CAN be false. An action whose verifier is handed nothing to
// check answers yes, which is precisely the fault measured on golden 2509.
static const ct_post_t k_posts_paste[] = {
    { CT_POST_PATH, "item:fs.last_written", 0, 0 },
    { CT_POST_END,  0, 0, 0 }
};
static const ct_promise_t k_promise_paste = {
    "the pasted item exists at the destination this call reports",
    "item:nav.path",          // scope: the folder being pasted into, nothing wider
    60, k_posts_paste
};
static const ct_terms_t k_t_paste = {
    CT_TIME_SLOW, 0, 0, CT_SUCC_ITEM, CT_DEPTH_FS,
    CT_FAIL_DENIED | CT_FAIL_NOSPACE | CT_FAIL_IO | CT_FAIL_NOTFOUND,
    "fs.last_written", &k_promise_paste
};
// Delete is a MOVE to the per-user bin, so the observable is the bin's count.
// NOT an escrow promise: the escrow enforces its no-delete invariant by never
// minting a delete grant and its oracle requires delete_count == 0, so the
// source side of a trash move is not expressible as an escrow postcondition BY
// THE DESIGN OF THE THING THAT WOULD CHECK IT (CONTRACT_COVERAGE_PLAN.md 4c).
static const ct_terms_t k_t_delete = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_FS,
    CT_FAIL_DENIED | CT_FAIL_NOTFOUND | CT_FAIL_IO, "recycle.count", 0
};
// Compress and Extract hand the work to /APPS/ARCHIVE with sys_spawn_args and
// return immediately, so the archive does NOT exist when the call returns.
// ASYNC + CT_SUCC_ARMED says exactly that, and this is why do_compress() does
// NOT set fs.last_written: claiming the file is there would be false at the one
// moment a caller reads it.
static const ct_terms_t k_t_archive = {
    CT_TIME_SLOW, CT_TF_ASYNC, 0, CT_SUCC_ARMED, CT_DEPTH_FS,
    CT_FAIL_DENIED | CT_FAIL_NOTFOUND | CT_FAIL_IO | CT_FAIL_UNSUPPORTED,
    "nav.item_count", 0
};
static const ct_terms_t k_t_install_font = {
    CT_TIME_FAST, 0, 0, CT_SUCC_RC, CT_DEPTH_SYSTEM,
    CT_FAIL_DENIED | CT_FAIL_NOTFOUND | CT_FAIL_IO | CT_FAIL_NOSPACE, 0, 0
};
// Open either navigates into a folder (observable in nav.path) or launches
// another process for a file (whose result is NOT observable from here). ARMED
// with nav.path is the honest description of both halves at once.
static const ct_terms_t k_t_open = {
    CT_TIME_FAST, CT_TF_ASYNC, 0, CT_SUCC_ARMED, CT_DEPTH_VIEW,
    CT_FAIL_NOTFOUND | CT_FAIL_IO | CT_FAIL_UNSUPPORTED, "nav.path", 0
};
static const ct_terms_t k_t_open_with = {
    CT_TIME_FAST, CT_TF_ASYNC, 0, CT_SUCC_RC, CT_DEPTH_SYSTEM,
    CT_FAIL_NOTFOUND | CT_FAIL_IO, 0, 0
};
// A row that only makes sense inside a RUNNING instance: it raises a dropdown or
// a dialog that a person then answers. LIVE_ONLY rather than MODAL, and the
// distinction is real: these RETURN immediately after setting the flag, so they
// do not wedge a caller; they are simply meaningless in a spawned process that
// has no window. MODAL is for a control that BLOCKS waiting for a human, which
// in this tree is the shared chooser's nested event loop, not these.
static const ct_terms_t k_t_open_props = {
    CT_TIME_INSTANT, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_DENIED | CT_FAIL_NOTFOUND, "dialog.properties_open", 0
};
static const ct_terms_t k_t_open_picker = {
    CT_TIME_INSTANT, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_DENIED | CT_FAIL_NOTFOUND, "dialog.openwith_open", 0
};
static const ct_terms_t k_t_open_rename = {
    CT_TIME_INSTANT, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_DENIED | CT_FAIL_NOTFOUND, "dialog.rename_open", 0
};
static const ct_terms_t k_t_open_menu = {
    CT_TIME_INSTANT, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW,
    CT_FAIL_DENIED, "dialog.menu_open", 0
};
static const ct_terms_t k_t_close_overlay = {
    CT_TIME_INSTANT, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW, 0, "dialog.menu_open", 0
};
// Restore and the two destructive bin actions differ in ONE way that matters to
// a caller: Restore acts, while Delete Permanently and Empty Bin ARM a confirm
// dialog that a person has to answer. So they are ARMED + LIVE_ONLY, and the
// count they will change is named so the caller knows what to re-read.
static const ct_terms_t k_t_rb_restore = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_FS,
    CT_FAIL_DENIED | CT_FAIL_NOTFOUND | CT_FAIL_IO, "recycle.count", 0
};
static const ct_terms_t k_t_rb_confirm = {
    CT_TIME_FAST, CT_TF_LIVE_ONLY | CT_TF_ASYNC, 0, CT_SUCC_ARMED, CT_DEPTH_FS,
    CT_FAIL_DENIED | CT_FAIL_IO, "recycle.count", 0
};
static const ct_terms_t k_t_rb_select = {
    CT_TIME_INSTANT, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW, 0, "recycle.selected_count", 0
};
static const ct_terms_t k_t_tab = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW, CT_FAIL_NOSPACE, "tabs.count", 0
};
static const ct_terms_t k_t_select = {
    CT_TIME_INSTANT, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW, CT_FAIL_RANGE, "nav.selection", 0
};
static const ct_terms_t k_t_filter = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_VIEW, CT_FAIL_IO, "filter.text", 0
};
static const ct_terms_t k_t_rename = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_FS,
    CT_FAIL_DENIED | CT_FAIL_NOTFOUND | CT_FAIL_IO, "nav.selected_name", 0
};
static const ct_terms_t k_t_perms = {
    CT_TIME_INSTANT, CT_TF_LIVE_ONLY, 0, CT_SUCC_ITEM, CT_DEPTH_FS,
    CT_FAIL_DENIED | CT_FAIL_NOTFOUND | CT_FAIL_UNSUPPORTED, "dialog.rename_open", 0
};
static const ct_terms_t k_t_eject = {
    CT_TIME_SLOW, 0, 0, CT_SUCC_ITEM, CT_DEPTH_SYSTEM,
    CT_FAIL_BUSY | CT_FAIL_IO | CT_FAIL_NOTFOUND, "volumes.count", 0
};
static const ct_terms_t k_t_net_rescan = {
    CT_TIME_LONG, CT_TF_ASYNC, 0, CT_SUCC_ARMED, CT_DEPTH_VIEW,
    CT_FAIL_TIMEOUT | CT_FAIL_IO, "net.scan_state", 0
};

// ---- THE CONTROL TABLES ---------------------------------------------------
// #554's comment "menu_select's indices below must stay in sync with this order"
// is GONE, along with the switch it referred to: each row carries its own
// handler, so an item added in the wrong place is still correct.
static const fctl_t MNU_NEW[] = {
    { "New Folder",    "new.folder",    do_new_folder, 0,
      "Create a new folder in the current directory, named NEWFOLD..NEWFOLD4", &k_t_create },
    { "New Text File", "new.text_file", do_new_file,   0,
      "Create an empty text file in the current directory, named NEW.TXT..NEW3.TXT", &k_t_create },
};
static const fctl_t MNU_VIEW[] = {
    { "Icons",              "view.set_icons",      act_view_icons,     0,
      "Show the current directory as a grid of icons", &k_t_view_mode },
    { "List",               "view.set_list",       act_view_list,      0,
      "Show the current directory as a single-column name list", &k_t_view_mode },
    { "Details",            "view.set_details",    act_view_details,   0,
      "Show the current directory with name, size, type and attribute columns", &k_t_view_mode },
    { "--", 0, 0, 0, 0, 0 },
    { "Sort: Name",         "view.sort_name",      act_sort_name,      0,
      "Order the listing by file name, directories first", &k_t_view_sort },
    { "Sort: Size",         "view.sort_size",      act_sort_size,      0,
      "Order the listing by file size, directories first", &k_t_view_sort },
    { "Sort: Type",         "view.sort_type",      act_sort_type,      0,
      "Order the listing by file extension, directories first", &k_t_view_sort },
    { "Sort: Attr",         "view.sort_attr",      act_sort_attr,      0,
      "Order the listing by ext2 mode or FAT attribute byte (#554)", &k_t_view_sort },
    { "--", 0, 0, 0, 0, 0 },
    { "Preview Pane",       "view.toggle_preview", act_toggle_preview, 0,
      "Show or hide the right-hand preview pane (text, hex dump or BMP thumbnail)", &k_t_view_preview },
    { "Show Hidden Files",  "view.toggle_hidden",  act_toggle_hidden,  0,
      "Show or hide FAT ATTR_HIDDEN entries and UNIX dotfiles, then relist", &k_t_view_hidden },
};
static const fctl_t MNU_CTX[] = {
    { "Open",                "item.open",            open_selected,    0,
      "Open the selected entry: enter a folder, or launch a file's associated app", &k_t_open },
    { "Open with...",        "item.open_with",       open_openwith,    0,
      "Raise the app picker for the selected file", &k_t_open_picker },
    { "--", 0, 0, 0, 0, 0 },
    { "Install Font",        "item.install_font",    do_install_font,  0,
      "Register the selected .ttf/.otf in the system font store, live (#351)", &k_t_install_font },
    { "--", 0, 0, 0, 0, 0 },
    { "Copy",                "item.copy",            do_copy,          0,
      "Put the selected entry on the clipboard for copying", &k_t_clip },
    { "Cut",                 "item.cut",             do_cut,           0,
      "Put the selected entry on the clipboard for moving", &k_t_clip },
    { "Paste",               "item.paste",           do_paste,         0,
      "Copy or move the clipboard entry into the current directory", &k_t_paste },
    { "--", 0, 0, 0, 0, 0 },
    { "Compress to .zip",    "item.compress_zip",    act_zip,          0,
      "Hand the selected entry to /APPS/ARCHIVE to make <name>.zip beside it", &k_t_archive },
    { "Compress to .tar.gz", "item.compress_targz",  act_targz,        0,
      "Hand the selected entry to /APPS/ARCHIVE to make <name>.tar.gz beside it", &k_t_archive },
    { "Extract here",        "item.extract_here",    do_extract_here,  0,
      "Hand the selected archive to /APPS/ARCHIVE to unpack into this directory", &k_t_archive },
    { "--", 0, 0, 0, 0, 0 },
    { "Rename",              "item.rename",          open_rename,      0,
      "Raise the inline rename field for the selected entry", &k_t_open_rename },
    { "Delete",              "item.delete",          do_delete,        0,
      "Move the selected entry to this user's Recycle Bin, recording its origin", &k_t_delete },
    { "--", 0, 0, 0, 0, 0 },
    { "Properties",          "item.properties",      act_props_open,   0,
      "Raise the Properties dialog for the selected entry", &k_t_open_props },
};
// The toolbar's five controls, in the order draw_toolbar() lays them out.
static const fctl_t TOOLBAR[] = {
    { "Back",    "nav.back",    navigate_back, 0,
      "Return to the previous directory in this tab's history", &k_t_navigate },
    { "Forward", "nav.forward", navigate_fwd,  0,
      "Go to the next directory in this tab's history after a Back", &k_t_navigate },
    { "Up",      "nav.up",      navigate_up,   0,
      "Go to the parent of the current directory", &k_t_navigate },
    { "New",     "menu.new",    act_menu_new,  0,
      "Open the New dropdown; refused on a read-only volume before it opens", &k_t_open_menu },
    { "View",    "menu.view",   act_menu_view, 0,
      "Open the View dropdown (view mode, sort order, preview, hidden files)", &k_t_open_menu },
};
// The Recycle Bin view: its three toolbar buttons plus the keyboard-only
// select-all / select-none / leave verbs, which are controls with no button and
// were therefore the easiest part of this app to leave undescribed.
static const fctl_t RB_ACTS[] = {
    { "Recycle Bin",       "recycle.enter",           act_recycle_enter, 0,
      "Replace the file list with this user's Recycle Bin view", &k_t_navigate },
    { "Back to folder",    "recycle.leave",           act_recycle_leave, 0,
      "Leave the Recycle Bin view and relist the current directory", &k_t_navigate },
    { "Restore",           "recycle.restore",         act_rb_restore,    0,
      "Restore every selected bin entry to the path its index records", &k_t_rb_restore },
    { "Delete Permanently","recycle.delete_selected", rb_confirm_delete_selected, 0,
      "Ask for confirmation, then permanently delete the selected bin entries", &k_t_rb_confirm },
    { "Empty Bin",         "recycle.empty",           rb_confirm_empty,  0,
      "Ask for confirmation, then permanently delete every entry in the bin", &k_t_rb_confirm },
    { "Select All",        "recycle.select_all",      act_rb_select_all, 0,
      "Select every entry in the Recycle Bin view (Ctrl+A / a)", &k_t_rb_select },
    { "Select None",       "recycle.select_none",     act_rb_select_none, 0,
      "Clear the selection in the Recycle Bin view (d)", &k_t_rb_select },
};
// The tab strip. A close hits the ACTIVE tab, which is what a contract caller
// can name; the per-tab close boxes are the same tab_close() with an index the
// pointer supplies, and tabs.active is how a caller picks which.
static const fctl_t TAB_ACTS[] = {
    { "New Tab",   "tabs.new",   act_tab_new,   0,
      "Open another tab on this session's home folder", &k_t_tab },
    { "Close Tab", "tabs.close", act_tab_close, 0,
      "Close the active tab; refused when it is the only one", &k_t_tab },
};
// The list and the field: the controls that have a KEY but no button, which is
// the half of this app a mouse-driven harness could never have reached at all.
static const fctl_t LIST_ACTS[] = {
    { "Select next",     "list.select_next", act_sel_next,     0,
      "Move the selection down one row and refresh the preview (Down)", &k_t_select },
    { "Select previous", "list.select_prev", act_sel_prev,     0,
      "Move the selection up one row and refresh the preview (Up)", &k_t_select },
    { "Activate",        "list.activate",    act_activate,     0,
      "Activate the selected row, as Enter or a double-click does", &k_t_open },
    { "Clear filter",    "filter.clear",     act_filter_clear, 0,
      "Empty the type-to-filter box and relist the directory (Backspace)", &k_t_filter },
    { "Rescan network",  "net.rescan",       act_net_rescan,   0,
      "Start or cancel the asynchronous LAN share sweep behind the /NET view", &k_t_net_rescan },
};
// The overlays: what a dialog's own buttons do, named so a caller can finish or
// abandon one instead of leaving the app wedged behind a modal.
static const fctl_t DLG_ACTS[] = {
    { "Close dialog",      "dialog.close",              act_dialog_close, 0,
      "Dismiss the topmost open overlay, in the same order Esc dismisses them", &k_t_close_overlay },
    { "Close menu",        "menu.close",                act_menu_close,   0,
      "Close the open dropdown without choosing an item", &k_t_close_overlay },
    { "Context menu",      "menu.context",              act_menu_ctx,     0,
      "Open the per-item context menu, as a right-click on a row does", &k_t_open_menu },
    { "Edit permissions",  "properties.edit_permissions", act_props_perms, 0,
      "Run the Properties permissions hotspot: edit ext2 mode/owner/group, or toggle the FAT read-only bit", &k_t_perms },
};
// Arg-taking controls: the path, the filter text, the new name, the row index
// and the tab index. Each is a real field or list in the UI; the contract's only
// difference is where the value comes from.
static const fctl_t ARG_ACTS[] = {
    { "Go to path",   "nav.goto",      0, afn_goto,
      "Navigate to an absolute path, recording it in this tab's Back history", &k_t_navigate },
    { "Set filter",   "filter.set",    0, afn_filter_set,
      "Set the type-to-filter text and relist the current directory", &k_t_filter },
    { "Rename to",    "item.rename_to",0, afn_rename_to,
      "Rename the selected entry, the same commit path the Rename dialog's OK runs", &k_t_rename },
    { "Select row",   "nav.select",    0, afn_select,
      "Select the listing row at this index, -1 for no selection", &k_t_select },
    { "Switch tab",   "tabs.switch",   0, afn_tab_switch,
      "Make the tab at this index active and relist its directory", &k_t_tab },
};

#define FCTL_N(t) ((int)(sizeof(t) / sizeof((t)[0])))

// ---- the open menu, now read from the table -------------------------------
static const fctl_t *active_menu(int *n) {
    if (g_menu == MENU_NEW)  { *n = FCTL_N(MNU_NEW);  return MNU_NEW;  }
    if (g_menu == MENU_VIEW) { *n = FCTL_N(MNU_VIEW); return MNU_VIEW; }
    if (g_menu == MENU_CTX)  { *n = FCTL_N(MNU_CTX);  return MNU_CTX;  }
    *n = 0; return 0;
}
static void menu_select(int idx) {
    int n = 0;
    const fctl_t *m = active_menu(&n);
    // The menu closes BEFORE the handler runs, because a handler that opens an
    // overlay (Rename, Properties, Open with) must not leave the dropdown it was
    // chosen from painted underneath it.
    g_menu = MENU_NONE;
    if (m && idx >= 0 && idx < n && m[idx].fn) m[idx].fn();
    fb_redraw();
}
static int menu_hit(int lx, int ly) {  // returns item index or -1
    int n = 0;
    const fctl_t *m = active_menu(&n); if (!m) return -1;
    int mw = 150, mh = n * 22 + 6, mx = g_menu_x, my = g_menu_y;
    if (mx + mw > WIN_W) mx = WIN_W - mw - 2;
    if (my + mh > WIN_H) my = WIN_H - mh - 2;
    if (lx < mx || lx >= mx + mw || ly < my || ly >= my + mh) return -2;  // outside
    int idx = (ly - my - 3) / 22;
    if (idx < 0 || idx >= n) return -1;
    if (!m[idx].tok) return -1;                     // a separator is not clickable
    return idx;
}

// ===========================================================================
// THE SIDEBAR, PROJECTED - and this is the row that closes E1.
//
// docs/OPEN_BACKLOG.md E1: the Places sidebar's CLICK path has never been
// tested, because QEMU pointer injection does not reliably land a click (#334).
// The sidebar is built from three sources and they are all enumerable WITHOUT
// drawing, which is what makes a windowless contract call a real exercise of it:
//
//   places.h            the shared named-locations MODEL. draw_sidebar() walks
//                       places_count()/places_at()/places_path() and so does
//                       this, so the chooser's sidebar, the Files sidebar and
//                       the contract cannot disagree about what a place is
//                       called or where it goes. ONE model, three readers.
//   DEVICES[]           the four fixed rows (disk root, ext2, Network, bin).
//   g_vols[]            one navigate row and one eject row per removable volume,
//                       exactly as draw_sidebar() emits two hit rows per visual
//                       row.
//
// And every one of them dispatches through side_activate(), which is the
// function EVENT_MOUSE_DOWN calls. So `app.action places.documents` is not a
// parallel path that happens to agree: it is the same path with a different
// caller, which is why it is evidence about the click.
typedef struct {
    char        tok[48];
    char        desc[128];
    unsigned char kind;          // the side_kind[] value, see draw_sidebar()
    char        target[MAX_PATH_LEN];
    int         vol;             // volume index for kinds 4/5/6, else -1
} fside_t;

// Places (<= 30 in draw_sidebar) + 4 devices + 2 rows per volume.
#define FSIDE_MAX (30 + 4 + 2 * MAX_VOLS)
static fside_t g_side_ct[FSIDE_MAX];
static int     g_side_ct_n = 0;
static int     g_side_ct_vols = -1;   // g_vol_count the table was built for

typedef struct { const char *tok; unsigned char kind; const char *target; const char *desc; } fdev_t;
static const fdev_t DEVICES[] = {
    { "device.root",        1, "/",     "Navigate to the root of the boot volume" },
    { "device.ext2",        1, "/ext2", "Navigate to the ext2 volume the kernel mounts at /ext2 (#99)" },
    { "device.network",     2, "/NET",  "Open the Network view and start the asynchronous LAN share sweep" },
    { "device.recycle_bin", 3, 0,       "Open this user's Recycle Bin view, not a directory listing" },
};

// Lowercase, dotted slug from a human label: "Quick access" -> "quick_access".
// The place LABEL is the only name of a place that exists (places.h owns it), so
// the contract token is derived from it rather than being a second name table
// that could drift from what the sidebar draws.
static void fslug(char *out, int cap, const char *pfx, const char *label) {
    int n = 0;
    for (int i = 0; pfx[i] && n < cap - 1; i++) out[n++] = pfx[i];
    for (int i = 0; label[i] && n < cap - 1; i++) {
        char c = label[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) c = '_';
        out[n++] = c;
    }
    out[n] = 0;
}

static void fside_push(const char *tok, const char *desc, int kind,
                       const char *target, int vol) {
    if (g_side_ct_n >= FSIDE_MAX) return;
    fside_t *s = &g_side_ct[g_side_ct_n++];
    str_copy(s->tok, tok, (int)sizeof(s->tok));
    str_copy(s->desc, desc, (int)sizeof(s->desc));
    s->kind = (unsigned char)kind;
    str_copy(s->target, target ? target : "", MAX_PATH_LEN);
    s->vol = vol;
}

// Rebuilt when the removable count changes, because a volume hot-plugged after
// launch is a row the sidebar grows and therefore a row the contract must grow
// too: a surface that is only correct at startup is the stale-copy fault with a
// clock on it.
static void files_side_ct_sync(void) {
    if (g_side_ct_vols == g_vol_count) return;
    g_side_ct_vols = g_vol_count;
    g_side_ct_n = 0;

    for (int i = 0; i < places_count(); i++) {
        const place_t *pl = places_at(i);
        char p[MAX_PATH_LEN];
        if (!pl || places_path(i, p, MAX_PATH_LEN) != 0) continue;
        char tok[48]; fslug(tok, (int)sizeof(tok), "places.", pl->label);
        char d[128];
        snprintf(d, sizeof(d), "Navigate to the %s place (%s), the same row the sidebar draws",
                 pl->label, p);
        fside_push(tok, d, 0, p, -1);
    }
    for (int i = 0; i < (int)(sizeof(DEVICES) / sizeof(DEVICES[0])); i++)
        fside_push(DEVICES[i].tok, DEVICES[i].desc, DEVICES[i].kind,
                   DEVICES[i].kind == 3 ? TRASH_DIR : DEVICES[i].target, -1);
    for (int i = 0; i < g_vol_count; i++) {
        const sc_volume_t *v = &g_vols[i];
        int readable = (v->flags & MOSVOL_READABLE) && (v->flags & MOSVOL_MOUNTED);
        char tok[48], d[128];
        snprintf(tok, sizeof(tok), "volume.%d.open", i);
        snprintf(d, sizeof(d), "Browse removable volume %d, mounted at %s%s", i,
                 v->mount[0] ? v->mount : "(none)",
                 readable ? "" : " (mounted but this filesystem cannot read files)");
        fside_push(tok, d, readable ? 4 : 6, v->mount, i);
        snprintf(tok, sizeof(tok), "volume.%d.eject", i);
        snprintf(d, sizeof(d), "Eject removable volume %d, leaving it first if it is the current directory", i);
        fside_push(tok, d, 5, v->mount, i);
    }
}

// ---- the three shared actfns ----------------------------------------------
// One per table SHAPE, not one per row. Each reads the row it was projected from
// through it->ctx (contract.h calls that the identity payload), so there is no
// token-to-handler translation table, which would be a new place to drift.
static int ct_press(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    const fctl_t *a = (const fctl_t *)it->ctx;
    if (!a) return -1;
    if (a->afn) return a->afn(argc, argv, out, ocap);
    if (!a->fn) return -1;
    files_err_clear();
    a->fn();                     // the SAME function a click on that control runs
    // The reply is an OBSERVATION, not an echo of the request: it reports the
    // state a caller would go and read, so a call that did nothing is visible in
    // the answer rather than only in the return code.
    snprintf(out, (size_t)ocap,
             "path=%s items=%d sel=%d view=%d sort=%d hidden=%d recycle=%d bin=%d%s%s",
             CUR.path, item_count, CUR.sel, g_view, g_sort, g_show_hidden ? 1 : 0,
             g_in_recycle ? 1 : 0, rb_count, g_err[0] ? " err=" : "", g_err[0] ? g_err : "");
    return g_err[0] ? -1 : 0;
}
CT_CHECK_ACTFN(ct_press);
static int ct_side(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)argc; (void)argv;
    const fside_t *s = (const fside_t *)it->ctx;
    if (!s) return -1;
    return side_activate(s->kind, s->target, s->vol, out, ocap);
}
CT_CHECK_ACTFN(ct_side);
static int ct_openwith(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    (void)argc; (void)argv;
    const ow_app_t *a = (const ow_app_t *)it->ctx;
    if (!a) return -1;
    if (CUR.sel < 0 || CUR.sel >= item_count) { snprintf(out, (size_t)ocap, "no-selection"); return -1; }
    int idx = (int)(a - OW_APPS);
    ow_pick(idx);                // the SAME function a click on that picker row runs
    snprintf(out, (size_t)ocap, "launched app=%s binary=%s", a->label, a->path);
    return 0;
}
CT_CHECK_ACTFN(ct_openwith);

// ---- the readable state a caller checks a control AGAINST -------------------
static int ct_str_path(char *o, int n)     { str_copy(o, CUR.path, n); return 0; }
static int ct_str_selname(char *o, int n)  {
    str_copy(o, (CUR.sel >= 0 && CUR.sel < item_count) ? items[CUR.sel].name : FILES_NOTHING_YET, n);
    return 0;
}
static int ct_str_filter(char *o, int n)   { str_copy(o, g_filter[0] ? g_filter : FILES_NOTHING_YET, n); return 0; }
static int ct_str_clipsrc(char *o, int n)  { str_copy(o, g_clip_path[0] ? g_clip_path : FILES_NOTHING_YET, n); return 0; }
static int ct_str_err(char *o, int n)      { str_copy(o, g_err[0] ? g_err : "(none)", n); return 0; }
static int ct_str_created(char *o, int n)  { str_copy(o, g_last_created, n); return 0; }
static int ct_str_written(char *o, int n)  { str_copy(o, g_last_written, n); return 0; }
static int ct_str_home(char *o, int n)     { str_copy(o, g_home, n); return 0; }
static int ct_str_volumes(char *o, int n)  {
    int l = 0; o[0] = 0;
    for (int i = 0; i < g_vol_count && l < n - 1; i++) {
        snprintf(o + l, (size_t)(n - l), "%s%s=%s", i ? "," : "",
                 g_vols[i].mount[0] ? g_vols[i].mount : "?",
                 g_vols[i].name[0] ? g_vols[i].name : "(unnamed)");
        // Advance by what LANDED, not by snprintf's return, which is the length
        // it WOULD have written and can therefore point past the buffer.
        while (o[l]) l++;
    }
    if (!l) str_copy(o, "(none)", n);
    return 0;
}
static int ct_get_hidden(void)     { return g_show_hidden ? 1 : 0; }
static int ct_set_hidden(int v)    { g_show_hidden = v ? true : false; load_directory(CUR.path); fb_redraw(); return 0; }
static int ct_get_menu(void)       { return g_menu; }
static int ct_get_back(void)       { return CUR.back_n; }
static int ct_get_fwd(void)        { return CUR.fwd_n; }
static int ct_get_ro(void)         { return cur_is_readonly() ? 1 : 0; }
static int ct_get_rbsel(void)      { return rb_count_selected(); }
static int ct_get_netstate(void)   { return ns_state(); }
static int ct_get_places(void)     { return places_count(); }
static int ct_set_sel(int v)       { if (v < -1 || v >= item_count) return -1; CUR.sel = v; fb_redraw(); return 0; }
static int ct_get_sel(void)        { return CUR.sel; }
static int ct_set_tab(int v)       { if (v < 0 || v >= tab_count) return -1; tab_switch(v); return 0; }
static int ct_get_tab(void)        { return active_tab; }

// The VALUE rows. Not projected, because they are not controls: they are the
// state a caller reads. Their timing / success / depth are DERIVED by
// contract_terms_of() from what each row already says, so none of them can carry
// a wrong term and none of them needed one written out. The four view rows carry
// a cfgkey and are therefore genuinely PREF depth: contract_save_cfg() persists
// them through libc/userconf.c, which is new behaviour and is the point - Files
// forgot your view mode and your sort order on every launch.
static const ct_item_t FILES_ITEMS[] = {
    {
      .name = "view.mode",
      .type = CT_ENUM,
      .access = CT_RW,
      .risk = CT_SAFE,
      .cfgkey = 'v',
      .hi = 2,
      .options = "List|Details|Icons",
      .var = &g_view,
      .desc = "How the listing is laid out; the same variable the three View menu items set",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "view.sort",
      .type = CT_ENUM,
      .access = CT_RW,
      .risk = CT_SAFE,
      .cfgkey = 's',
      .hi = 3,
      .options = "Name|Size|Type|Attr",
      .var = &g_sort,
      .desc = "Listing order; the same variable the four Sort menu items set",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "view.preview_pane",
      .type = CT_BOOL,
      .access = CT_RW,
      .risk = CT_SAFE,
      .cfgkey = 'p',
      .hi = 1,
      .var = &g_preview_on,
      .desc = "Whether the right-hand preview pane is drawn",
      .commitfn = 0 /*commitfn*/,
    },
    // getfn/setfn rather than `var`, because g_show_hidden is a `bool` (one
    // byte) and ct_item_t.var is an `int *`: pointing var at it would read and
    // write three bytes that are not the flag. The one row in this table whose
    // backing store is not an int is the one row that has to say so.
    {
      .name = "view.show_hidden",
      .type = CT_BOOL,
      .access = CT_RW,
      .risk = CT_SAFE,
      .cfgkey = 'h',
      .hi = 1,
      .getfn = ct_get_hidden,
      .setfn = ct_set_hidden,
      .desc = "Whether FAT hidden entries and UNIX dotfiles appear in the listing",
      .commitfn = 0 /*commitfn*/,
    },

    {
      .name = "nav.path",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = ct_str_path,
      .desc = "Absolute path of the directory the active tab is showing",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "nav.item_count",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = MAX_ITEMS,
      .var = &item_count,
      .desc = "How many rows the listing currently holds, after filter and hidden rules",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "nav.selection",
      .type = CT_INT,
      .access = CT_RW,
      .risk = CT_SAFE,
      .lo = -1,
      .hi = MAX_ITEMS - 1,
      .getfn = ct_get_sel,
      .setfn = ct_set_sel,
      .desc = "Index of the selected listing row, -1 for none",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "nav.selected_name",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = ct_str_selname,
      .desc = "Name of the selected entry, or (none)",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "nav.in_recycle",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .desc = "Whether the content area is showing the Recycle Bin instead of a directory",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "nav.back_depth",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 64,
      .getfn = ct_get_back,
      .desc = "How many entries the active tab's Back history holds",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "nav.forward_depth",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 64,
      .getfn = ct_get_fwd,
      .desc = "How many entries the active tab's Forward history holds",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "nav.readonly",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .getfn = ct_get_ro,
      .desc = "Whether the current directory is on a read-only volume, which greys New",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "nav.home",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = ct_str_home,
      .desc = "This session's home directory, as places.h resolved it at startup",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "places.count",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 64,
      .getfn = ct_get_places,
      .desc = "How many Quick access / system places this session offers",
      .commitfn = 0 /*commitfn*/,
    },

    {
      .name = "filter.text",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = ct_str_filter,
      .desc = "The type-to-filter substring now applied to the listing, or (none)",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "clipboard.mode",
      .type = CT_ENUM,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 2,
      .options = "None|Copy|Cut",
      .var = &g_clip_mode,
      .desc = "Whether the clipboard holds nothing, a copy or a cut",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "clipboard.source",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = ct_str_clipsrc,
      .desc = "Absolute path of the entry on the clipboard, or (none)",
      .commitfn = 0 /*commitfn*/,
    },

    {
      .name = "recycle.count",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = RB_MAX_ITEMS,
      .var = &rb_count,
      .desc = "How many entries this user's Recycle Bin holds",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "recycle.selected_count",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = RB_MAX_ITEMS,
      .getfn = ct_get_rbsel,
      .desc = "How many Recycle Bin entries are selected for Restore or Delete",
      .commitfn = 0 /*commitfn*/,
    },

    {
      .name = "tabs.count",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .lo = 1,
      .hi = MAX_TABS,
      .var = &tab_count,
      .desc = "How many browser tabs are open",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "tabs.active",
      .type = CT_INT,
      .access = CT_RW,
      .risk = CT_SAFE,
      .hi = MAX_TABS - 1,
      .getfn = ct_get_tab,
      .setfn = ct_set_tab,
      .desc = "Index of the active tab; writing it switches tab and relists",
      .commitfn = 0 /*commitfn*/,
    },

    {
      .name = "volumes.count",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = MAX_VOLS,
      .var = &g_vol_count,
      .desc = "How many removable volumes the Removable section is showing",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "volumes.list",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = ct_str_volumes,
      .desc = "Comma-separated mount=name for each removable volume, or (none)",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "disks.count",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 8,
      .var = &g_disk_count,
      .desc = "How many fixed disks the This PC section enumerated",
      .commitfn = 0 /*commitfn*/,
    },

    {
      .name = "dialog.menu_open",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 3,
      .getfn = ct_get_menu,
      .desc = "Which dropdown is open: 0 none, 1 New, 2 View, 3 context",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "dialog.properties_open",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .var = &g_props_open,
      .desc = "Whether the Properties dialog is up",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "dialog.rename_open",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .var = &g_te_open,
      .desc = "Whether the inline rename / permissions text field is up",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "dialog.openwith_open",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .var = &g_openwith_open,
      .desc = "Whether the Open with app picker is up",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "dialog.credentials_open",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .var = &g_cred_open,
      .desc = "Whether the SMB credentials dialog is up (#317)",
      .commitfn = 0 /*commitfn*/,
    },

    {
      .name = "net.scan_state",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 2,
      .getfn = ct_get_netstate,
      .desc = "LAN share sweep state: 0 idle, 1 scanning, 2 done",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "net.mount_count",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 32,
      .var = &g_netmount_count,
      .desc = "How many saved network share favourites this user has",
      .commitfn = 0 /*commitfn*/,
    },

    {
      .name = "status.error",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = ct_str_err,
      .desc = "The status row's error band text, or (none). Set by a failed operation (#742)",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "fs.last_created",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = ct_str_created,
      .desc = "Absolute path of the last folder or file New actually created, or (none)",
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "fs.last_written",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = ct_str_written,
      .desc = "Absolute path the last Paste actually landed, or (none). The promise checks this",
      .commitfn = 0 /*commitfn*/,
    },
};

// ---- THE PROJECTION -------------------------------------------------------
// A flat index over every control table, built once so the row order is stable
// across calls (a caller that lists then calls by index needs that) and so a
// separator costs nothing at call time.
static const fctl_t *g_flat[160];
static int g_flat_n = 0;

static void files_flat_build(void) {
    g_flat_n = 0;
#define PUSH(t) do { for (int i = 0; i < FCTL_N(t); i++) \
        if ((t)[i].tok && g_flat_n < (int)(sizeof(g_flat)/sizeof(g_flat[0]))) \
            g_flat[g_flat_n++] = &(t)[i]; } while (0)
    PUSH(MNU_NEW); PUSH(MNU_VIEW); PUSH(MNU_CTX); PUSH(TOOLBAR);
    PUSH(RB_ACTS); PUSH(TAB_ACTS); PUSH(LIST_ACTS); PUSH(DLG_ACTS); PUSH(ARG_ACTS);
#undef PUSH
}

#define OW_CT_N ((int)(sizeof(OW_APPS) / sizeof(OW_APPS[0])))

// RISK IS DERIVED FROM THE ROW'S DECLARED DEPTH, not written out per row.
//
// Same judgement as contract_terms_of() deriving a value row's terms and
// contract_hash() deriving its own signature: a hand-written second field is a
// second thing that can disagree with the first, and the disagreement is what
// #235 was made of. A control that reaches the filesystem or another process is
// GUARDED and goes to the aicap gate (libc/aicap.c, the SAME gate the AI tool
// loop uses); a control that only changes this window or this app's own
// preference is SAFE. So a row CANNOT be marked safe while declaring that it
// writes the disk, because nobody gets to mark it at all.
//
// The capability id is "fs.write", the aicap CAPABILITY spelling, deliberately
// not the TOOL spelling "files.write" that Snapshot's rows use: aicap_authorize()
// accepts both and maps the tool to the capability, so both work, and picking the
// capability side means the row says what the gate actually checks. Recorded in
// CONTRACT_COVERAGE_PLAN.md 4d, which flagged the inconsistency.
static void ct_apply_risk(ct_item_t *it, const ct_terms_t *t) {
    int deep = t && (t->depth == CT_DEPTH_FS || t->depth == CT_DEPTH_SYSTEM);
    it->risk = deep ? CT_GUARDED : CT_SAFE;
    it->cap  = deep ? "fs.write" : 0;
}

static int files_project(int idx, ct_item_t *out) {
    files_side_ct_sync();
    if (idx < 0) return 0;
    ct_item_t it;
    __builtin_memset(&it, 0, sizeof(it));
    it.type = CT_ACTION;
    it.access = CT_WRITE;

    if (idx < g_flat_n) {
        const fctl_t *a = g_flat[idx];
        it.name = a->tok; it.desc = a->desc; it.terms = a->terms;
        it.actfn = ct_press; it.ctx = a;
        ct_apply_risk(&it, a->terms);
        *out = it; return 1;
    }
    idx -= g_flat_n;

    if (idx < OW_CT_N) {
        const ow_app_t *a = &OW_APPS[idx];
        static char tok[OW_CT_N][48], dsc[OW_CT_N][128];
        fslug(tok[idx], 48, "openwith.", a->label);
        snprintf(dsc[idx], 128, "Open the selected file with %s (%s), the same picker row", a->label, a->path);
        it.name = tok[idx]; it.desc = dsc[idx]; it.terms = &k_t_open_with;
        it.actfn = ct_openwith; it.ctx = a;
        ct_apply_risk(&it, &k_t_open_with);
        *out = it; return 1;
    }
    idx -= OW_CT_N;

    if (idx < g_side_ct_n) {
        const fside_t *s = &g_side_ct[idx];
        it.name = s->tok; it.desc = s->desc;
        it.terms = (s->kind == 5) ? &k_t_eject : &k_t_navigate;
        it.actfn = ct_side; it.ctx = s;
        ct_apply_risk(&it, it.terms);
        *out = it; return 1;
    }
    return 0;
}

// Defined AFTER the contract object they name, and declared here, because a
// tentative definition of a const aggregate is the kind of thing that compiles
// differently depending on the compiler's mood.
static void files_ct_load(void);
static void files_ct_commit(void);

static const ct_contract_t FILES_CONTRACT = {
    "files", "Files",
    "Every dropdown item, toolbar button, Recycle Bin action, tab control, "
    "keyboard-only verb, Open-with row and Places/device/volume sidebar row is "
    "PROJECTED from the table the app draws and dispatches from, so a control "
    "absent from a table can neither be drawn nor described. The sidebar rows "
    "run side_activate(), the same function EVENT_MOUSE_DOWN calls, which is why "
    "driving them here is evidence about the click path (#334, backlog E1). NOT "
    "covered, and these are gestures rather than controls: the pointer drag that "
    "resizes nothing, hover highlighting, wheel scrolling, double-click timing, "
    "the scrollbar pill, and the four SMB credential text fields, which are "
    "focus targets whose only action (Connect) IS described.",
    FILES_ITEMS, (int)(sizeof(FILES_ITEMS) / sizeof(FILES_ITEMS[0])),
    files_project,
    files_ct_load,
    files_ct_commit
};

// PERSISTENCE IS DERIVED FROM THE TABLE, so a row that gains a cfgkey is saved
// and restored in the same commit with no second key list to update (the #231
// fault: a hand-maintained hash had silently stopped watching three settings).
// Files previously persisted NOTHING: it forgot the view mode, the sort order,
// the preview pane and the hidden-file setting on every launch.
static void files_ct_load(void)   { contract_load_cfg(&FILES_CONTRACT, "FILES.CFG", 0); }
static void files_ct_commit(void) { contract_save_cfg(&FILES_CONTRACT, "FILES.CFG"); }

int main(int argc, char **argv) {

    // [filesui] Where this session starts, and the DELETED FALLBACK.
    //
    // This used to call getpwuid(getuid()) itself and, when the answer was
    // empty or "/", fall back to the literal "/HOME/ADMIN". That was wrong in
    // two ways. It was a SECOND implementation of "where is home" competing
    // with libc's userhome_root(), which is the kind of duplication that lets a
    // fallback like this survive; and what it fell back TO was ANOTHER USER'S
    // HOME. A session with no account of its own was shown admin's folders,
    // every row of which it cannot write, and on a golden that ships no
    // /CONFIG/PASSWD (which is every fresh image, by design) that was the
    // NORMAL case, not an edge case.
    //
    // places_init() asks userhome_root() once. When there is no home the USER
    // section becomes a single "Scratch" row pointing at the one directory a
    // pre-account session can actually write (/TMP, seeded 01777 by
    // fs/perms.c), which is both true and useful. Called here, in startup, and
    // never from a draw path: it does one opendir per probed system row.
    places_init();
    {
        char hb[MAX_PATH_LEN];
        if (places_have_home() && userhome_dir(0, hb, sizeof(hb)) == 0)
            str_copy(g_home, hb, MAX_PATH_LEN);
        else if (userscratch_dir(hb, sizeof(hb)) == 0)
            str_copy(g_home, hb, MAX_PATH_LEN);
        else
            str_copy(g_home, "/", MAX_PATH_LEN);
    }

    enumerate_disks();
    poll_volumes();   // #250: removable volumes, before the first paint

    for (int i = 0; i < MAX_TABS; i++) tabs[i].sel = -1;
    tabs[0].used = true; str_copy(tabs[0].path, g_home, MAX_PATH_LEN);
    current_path = tabs[0].path;

    // ---- THE CONTRACT, ANSWERED BEFORE ANY WINDOW EXISTS -------------------
    //
    // contract-lint check 8 requires this, and the requirement is a measured
    // hang rather than a style rule: contract_invoke() spawns this binary with
    // --contract and then waits on an UNBOUNDED sys_waitpid() (the kernel
    // ignores WNOHANG), so an app that opens its window instead of answering
    // never exits and wedges the caller with no timeout and no recovery. That is
    // the #469m hang, measured on the first turn of the first baseline run.
    //
    // Everything the contract needs is already set up above: places_init() for
    // the sidebar rows, poll_volumes() for the Removable rows, tab 0 for
    // nav.path. load_directory() runs here too, so nav.path and nav.item_count
    // answer about a real directory rather than an empty one, and fb_redraw() is
    // a no-op with no window (see its own note).
    files_flat_build();
    files_side_ct_sync();
    contract_load_cfg(&FILES_CONTRACT, "FILES.CFG", 0);
    load_directory(g_home);
    // The escrow backend for fs.paste's promise. contract.c FAILS THE ACTION
    // CLOSED without it: it refuses a promised action rather than run it
    // unverified, because a promise nothing checks is worse than no promise.
    contract_use_escrow_promises();
    if (contract_is_invocation(argc, argv))
        return contract_cli(argc, argv, &FILES_CONTRACT);

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
    // #peruser2: the Start-menu / desktop "Open Recycle Bin" handoff flag. It was
    // the literal "/RECYVIEW.FLG" at the root-owned ext2 root, so the COMPOSITOR
    // could not create it for a non-root session and the menu item silently
    // opened Files at the home folder instead of the bin. Both ends move together
    // (compositor/startmenu.c, compositor/desktop.c) and both run as the session
    // user, so the channel stays a channel. Root's home is "/", so a root session
    // still uses exactly /RECYVIEW.FLG.
    { char rp[256];
      if (userhome_path(0, "RECYVIEW.FLG", rp, sizeof(rp)) != 0)
          rp[0] = 0;
      int rf = rp[0] ? open(rp, 0) : -1;
      if (rf < 0) { rf = open("/RECYVIEW.FLG", 0); if (rf >= 0) rp[0] = 0; }
      if (rf >= 0) { close(rf); unlink(rp[0] ? rp : "/RECYVIEW.FLG"); g_in_recycle = 1; rb_load(); } }
    fb_redraw();

    gui_event_t event;
    int running = 1;
    while (running) {
        int et = win_get_event(window_handle, &event, 100);
        // LIVE CONTRACT DELIVERY (contract.h, tier 2). One non-blocking mailbox
        // check per tick, on the timeout path too, so a caller can reach THIS
        // instance's live state: its open document, its selection, its dialogs.
        // The spawn path can only touch a stateless copy, which is why a
        // LIVE_ONLY row (every overlay here) is reachable only this way.
        if (contract_live_poll(&FILES_CONTRACT)) fb_redraw();
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
            fb_redraw(); break;
        case EVENT_RESIZE:
            if (event.mouse_x > 0 && event.mouse_y > 0) { g_win_w = event.mouse_x; g_win_h = event.mouse_y; }
            fb_redraw(); break;
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
                        // The eject hot zone is the right-hand end of a
                        // removable row only, and that geometry is the ONE thing
                        // that is genuinely about the pointer, so it stays here.
                        // Everything the row then DOES is side_activate(), which
                        // the contract action calls with the same three values:
                        // one route, two callers, so driving it through
                        // app.action is evidence about this click.
                        if (side_kind[i] == 5 && lx < SIDE_EJECT_X - 4) continue;
                        side_activate(side_kind[i], side_target[i], side_vol[i], 0, 0);
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
