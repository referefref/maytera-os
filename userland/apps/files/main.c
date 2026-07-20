// files - Modern-style file browser for MayteraOS (user-space)
//
// Features: tabbed views, back/forward/up navigation with history, a Places
// sidebar (Quick access home folders + dynamically-enumerated drives + Network),
// a command bar (New / View / Sort / Filter), and a toggleable preview pane that
// previews text, binaries (hex dump) and BMP images.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_font.h"
#include "../../libc/notify.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"
#include "../../libc/pwd.h"
#include "../../libc/unistd.h"
#include "../../libc/assoc.h"
#include "../../libc/gui_style.h"

// Route all in-window text through the antialiased TrueType path (matches Settings).
#define win_draw_text(h, x, y, s, c)       win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define win_draw_text_small(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 11, (c))

static int str_eq(const char *a, const char *b);

// ---- layout ---------------------------------------------------------------
static int g_win_w = 820, g_win_h = 560;  // live content size (EVENT_RESIZE)
#define WIN_W        g_win_w
#define WIN_H        g_win_h
#define TABBAR_H     26
#define TOOLBAR_H    40
#define TOP_H        (TABBAR_H + TOOLBAR_H)
#define STATUS_H     22
#define SIDEBAR_W    160
#define PREVIEW_W    230
#define ITEM_HEIGHT  24
#define ICON_SIZE    16
#define MAX_ITEMS    512
#define MAX_PATH_LEN 256
#define MAX_NAME_LEN 64
#define MAX_TABS     8
#define MAX_HIST     24

// content/list geometry (preview pane optional)
#define CONTENT_Y    TOP_H
#define CONTENT_H    (WIN_H - TOP_H - STATUS_H)
static int g_preview_on = 1;
static int  g_last_click_idx = -1;   // double-click detection (file list)
static long g_last_click_ticks = 0;
static int list_x(void) { return SIDEBAR_W; }
static int list_w(void) { return WIN_W - SIDEBAR_W - (g_preview_on ? PREVIEW_W : 0) - 16; }

// ---- theme-aware colors ---------------------------------------------------
static inline unsigned int files_fg(unsigned int bg) {
    int r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    int lum = (r * 30 + g * 59 + b * 11) / 100;
    return lum > 140 ? 0x00181818u : 0x00F0F0F0u;
}
// ---- cohesive theme palette (#Files): derive harmonious greys from the theme
// mode (dark/light by window-bg luminance) with a subtle accent tint, instead of
// the theme's raw black/white pairs which clashed badly.
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
static inline unsigned int fp_toolbar(void){ return fp_tint(fp_dark()?0x002C313B:0x00EDEFF3, fp_acc(), 6); }
static inline unsigned int fp_field(void)  { return fp_dark()?0x00333A45:0x00FFFFFF; }
static inline unsigned int fp_border(void) { return fp_dark()?0x003A424F:0x00CDD3DB; }
// Row selection: a readable DARK GREY (accent-tinted) on dark themes, a light
// accent-tint on light themes - NOT the raw accent, which rendered near-black on
// Nord and made the selected row + its text unreadable.
static inline unsigned int fp_sel(void){ return fp_dark()
    ? fp_tint(0x003C434F, fp_acc(), 28)
    : fp_tint(0x00CCD6E6, fp_acc(), 26); }
#define BG_COLOR      fp_content()
#define SIDEBAR_BG    fp_panel()
#define TOOLBAR_BG    fp_toolbar()
#define STATUS_BG     fp_toolbar()
#define ITEM_HOVER    fp_tint(fp_content(), fp_acc(), 16)
#define ITEM_SELECTED fp_sel()
#define TEXT_COLOR    files_fg(BG_COLOR)
#define SIDE_TEXT     files_fg(SIDEBAR_BG)
static inline unsigned int files_dim(unsigned int bg) {
    unsigned int ink = files_fg(bg);
    int ir=(ink>>16)&0xFF, ig=(ink>>8)&0xFF, ib=ink&0xFF;
    int br=(bg>>16)&0xFF, bgc=(bg>>8)&0xFF, bb=bg&0xFF;
    // Bias toward the ink (5/8) so dim/detail text stays readable - a light grey
    // on dark themes (the old 50/50 average was too dark on Nord).
    return ((((ir*5+br*3)/8)&0xFF)<<16) | ((((ig*5+bgc*3)/8)&0xFF)<<8) | (((ib*5+bb*3)/8)&0xFF);
}
#define INPUT_BG      fp_field()
#define INPUT_TEXT    files_fg(fp_field())
#define BTN_FACE      fp_toolbar()
#define BTN_TEXT      files_fg(fp_toolbar())
#define DIM_TEXT      files_dim(BG_COLOR)
#define SIDE_DIM      files_dim(SIDEBAR_BG)
#define ICON_FOLDER   0x00FFC800
#define BORDER_COLOR  fp_border()

// ---- model ----------------------------------------------------------------
typedef struct {
    char name[MAX_NAME_LEN];
    bool is_directory;
    uint32_t size;
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
enum { SORT_NAME, SORT_SIZE, SORT_TYPE };
static int  g_view = VIEW_DETAILS;
static int  g_sort = SORT_NAME;
static char g_filter[48] = "";

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

static int window_handle = -1;
static int win_x = 90, win_y = 40;
static char g_home[MAX_PATH_LEN] = "/APPS";
static int g_in_recycle = 0;   // 1 = content area shows the integrated Recycle Bin

// active tab convenience
#define CUR (tabs[active_tab])
static char *current_path = NULL;   // points at CUR.path after init

static void fb_redraw(void);
static void load_directory(const char *path);
static void navigate_to(const char *path);                 // defined below (navigation)
static int  copy_file(const char *src, const char *dst);   // defined below (clipboard)
static void open_add_network(void);                        // #317 Network "Add" dialog

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

// Parse "server share [user] [pass]" or "server/share [user] [pass]".
static void parse_add_input(const char *in) {
    char server[64]={0}, share[40]={0}, user[40]={0}, pass[40]={0};
    int i = 0;
    char *dst[4]; int sz[4];
    dst[0]=server; sz[0]=64; dst[1]=share; sz[1]=40;
    dst[2]=user;   sz[2]=40; dst[3]=pass;  sz[3]=40;
    for (int t = 0; t < 4; t++) {
        while (in[i] == ' ') i++;
        int o = 0;
        while (in[i] && in[i] != ' ' && o < sz[t]-1) dst[t][o++] = in[i++];
        dst[t][o] = 0;
    }
    // Accept "server/share" in the first token.
    if (!share[0]) {
        for (int j = 0; server[j]; j++) {
            if (server[j] == '/') {
                int o = 0; for (int k = j+1; server[k] && o < 39; k++) share[o++] = server[k];
                share[o] = 0; server[j] = 0; break;
            }
        }
    }
    if (!server[0] || !share[0]) return;
    netmount_add(server, share, user, pass);
}

void open_add_network(void) {
    str_copy(g_te_title, "Add: server share [user] [pass]", sizeof(g_te_title));
    g_te_buf[0] = 0; g_te_len = 0;
    g_te_purpose = 2;       // 2 = add network location
    g_te_open = 1;
    fb_redraw();
}

static void load_directory(const char *path) {
    str_copy(CUR.path, path, MAX_PATH_LEN);
    current_path = CUR.path;
    item_count = 0;
    if (CUR.sel >= 0) CUR.sel = -1;
    CUR.scroll = 0;

    // #317: the virtual Network folder lists saved SMB mounts + an Add entry.
    if (str_eq(path, "/NET")) {
        load_netmounts();
        str_copy(items[item_count].name, "[+ Add Network Location]", MAX_NAME_LEN);
        items[item_count].is_directory = false; items[item_count].size = 0; item_count++;
        for (int k = 0; k < g_netmount_count && item_count < MAX_ITEMS; k++) {
            str_copy(items[item_count].name, g_netmounts[k].label, MAX_NAME_LEN);
            items[item_count].is_directory = true; items[item_count].size = 0; item_count++;
        }
        return;
    }

    if (path[0] == '/' && path[1] != '\0') {
        str_copy(items[item_count].name, "..", MAX_NAME_LEN);
        items[item_count].is_directory = true; items[item_count].size = 0; item_count++;
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
                item_count++;
            }
            close(fd);
        }
        sort_items();
        if (item_count == 0) {
            str_copy(items[0].name, "(empty)", MAX_NAME_LEN);
            items[0].is_directory = false; items[0].size = 0; item_count = 1;
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
        item_count++;
    }
    sort_items();
    if (item_count == 0) {
        str_copy(items[0].name, "(empty)", MAX_NAME_LEN);
        items[0].is_directory = false; items[0].size = 0; item_count = 1;
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

// Remove the index line for "name" and rewrite TRASH_INDEX.
static void rb_idx_remove(const char *name) {
    char nb[8192]; int nl = 0; int i = 0;
    while (i < rb_idx_len) {
        int ls = i; while (i < rb_idx_len && rb_idx_buf[i] != '\n') i++;
        int le = i; if (i < rb_idx_len) i++;
        int bar = ls; while (bar < le && rb_idx_buf[bar] != '|') bar++;
        int match = 0;
        if (bar < le) { int k = ls, t = 0, m = 1; while (k < bar) { if (rb_idx_buf[k] != name[t]) { m = 0; break; } k++; t++; } if (m && name[t] == 0) match = 1; }
        if (!match) { for (int j = ls; j < le && nl < (int)sizeof(nb) - 1; j++) nb[nl++] = rb_idx_buf[j]; if (nl < (int)sizeof(nb) - 1) nb[nl++] = '\n'; }
    }
    unlink(TRASH_INDEX);
    int fd = open(TRASH_INDEX, 0x41);
    if (fd >= 0) { if (nl) write(fd, nb, nl); close(fd); }
    for (int j = 0; j < nl; j++) rb_idx_buf[j] = nb[j];
    rb_idx_len = nl; rb_idx_buf[nl] = 0;
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
static void rb_restore_selected(void) {
    for (int i = rb_count - 1; i >= 0; i--) {
        if (!rb_items[i].selected) continue;
        char src[200]; rb_join(src, sizeof(src), TRASH_DIR, rb_items[i].name);
        if (rb_items[i].original_path[0] && !str_eq(rb_items[i].original_path, "(unknown)")) {
            if (rename(src, rb_items[i].original_path) != 0) {        // (#239) ext2 rename fallback
                if (copy_file(src, rb_items[i].original_path) == 0) unlink(src);
            }
        }
        rb_idx_remove(rb_items[i].name);
    }
    rb_load();
}

// Permanently unlink every selected trashed file.
static void rb_delete_selected(void) {
    for (int i = rb_count - 1; i >= 0; i--) {
        if (!rb_items[i].selected) continue;
        char p[200]; rb_join(p, sizeof(p), TRASH_DIR, rb_items[i].name);
        unlink(p);
        rb_idx_remove(rb_items[i].name);
    }
    rb_load();
}

static void rb_empty(void) {
    for (int i = 0; i < rb_count; i++) {
        char p[200]; rb_join(p, sizeof(p), TRASH_DIR, rb_items[i].name);
        unlink(p);
    }
    unlink(TRASH_INDEX);
    rb_load();
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
            if (str_eq(e, "bmp") && build_bmp_thumb(PREVIEW_W - 20, CONTENT_H - 100)) {
                int k = 0; while (key[k] && k < (int)sizeof(prev_thumb_key) - 1) { prev_thumb_key[k] = key[k]; k++; }
                prev_thumb_key[k] = 0;
            }
        }
    }
    else if (is_text_ext(e)) prev_kind = PV_TEXT;
    else prev_kind = PV_HEX;
}

static void draw_preview(void) {
    int px = WIN_W - PREVIEW_W, py = CONTENT_Y;
    win_draw_rect(window_handle, px, py, PREVIEW_W, CONTENT_H, SIDEBAR_BG);
    win_draw_rect(window_handle, px, py, 1, CONTENT_H, BORDER_COLOR);
    win_draw_text(window_handle, px + 10, py + 8, "Preview", DIM_TEXT);
    int cx = px + 10, cy = py + 30, cw = PREVIEW_W - 20;
    if (CUR.sel < 0 || CUR.sel >= item_count || prev_kind == PV_NONE) {
        if (CUR.sel >= 0 && CUR.sel < item_count && items[CUR.sel].is_directory)
            win_draw_text(window_handle, cx, cy, "(folder)", SIDE_TEXT);
        else win_draw_text(window_handle, cx, cy, "No preview", SIDE_TEXT);
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
            win_draw_text(window_handle, cx + 8, cy + 52, "(image)", SIDE_TEXT);
        }
        return;
    }
    if (prev_kind == PV_TEXT) {
        int x = cx, y = cy; int col = 0; int maxcol = cw / 8;
        for (int i = 0; i < prev_len && y < py + CONTENT_H - 16; i++) {
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
    for (int i = 0; i < shown && y < py + CONTENT_H - 16; i += 8) {
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
#define TAB_W 130
static void draw_tabbar(void) {
    win_draw_rect(window_handle, 0, 0, WIN_W, TABBAR_H, SIDEBAR_BG);
    win_draw_rect(window_handle, 0, TABBAR_H - 1, WIN_W, 1, BORDER_COLOR);
    int rad = (theme_get_active() == 4) ? 0 : 6;
    for (int i = 0; i < tab_count; i++) {
        int tx = 4 + i * (TAB_W + 2);
        bool act = (i == active_tab);
        uint32_t tb = act ? BG_COLOR : SIDEBAR_BG;
        gui_fill_rounded_aa(window_handle, tx, 3, TAB_W, TABBAR_H - 3, rad, tb, SIDEBAR_BG);
        char title[18]; str_copy(title, basename_of(tabs[i].path), 18);
        win_draw_text(window_handle, tx + 10, 6, title, act ? TEXT_COLOR : SIDE_TEXT);
        win_draw_text(window_handle, tx + TAB_W - 16, 6, "x", 0x00C05050);
    }
    if (tab_count < MAX_TABS) {
        int ax = 4 + tab_count * (TAB_W + 2);
        gui_fill_rounded_aa(window_handle, ax, 3, 24, TABBAR_H - 3, rad, SIDEBAR_BG, SIDEBAR_BG);
        win_draw_text(window_handle, ax + 8, 6, "+", TEXT_COLOR);
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

static void draw_toolbar(void) {
    int ty = TABBAR_H;
    win_draw_rect(window_handle, 0, ty, WIN_W, TOOLBAR_H, TOOLBAR_BG);
    win_draw_rect(window_handle, 0, ty + TOOLBAR_H - 1, WIN_W, 1, BORDER_COLOR);
    int by = ty + 8;
    uint32_t ink = files_fg(fp_toolbar());
    // nav icon buttons (Zest chevrons; dim when the action is unavailable)
    files_icon_btn(8,  by, 26, 24, "CHEVL", 0, CUR.back_n ? ink : files_dim(fp_toolbar()), CUR.back_n != 0, false);
    files_icon_btn(36, by, 26, 24, "CHEVR", 1, CUR.fwd_n  ? ink : files_dim(fp_toolbar()), CUR.fwd_n  != 0, false);
    files_icon_btn(64, by, 26, 24, "CHEVU", 2, ink, true, false);
    // address bar
    int axx = 98, aw = WIN_W - 98 - 8 - 3 * 64;
    gui_textfield2(window_handle, axx, by, aw, 24, CUR.path, false);
    // command buttons: New / View
    int bx = WIN_W - 8 - 3 * 64;
    gui_button(window_handle, bx, by, 60, 24, "New", GUI_BTN_SECONDARY,
               g_menu == MENU_NEW ? GUI_ST_PRESSED : GUI_ST_NORMAL);
    gui_button(window_handle, bx + 64, by, 60, 24, "View", GUI_BTN_SECONDARY,
               g_menu == MENU_VIEW ? GUI_ST_PRESSED : GUI_ST_NORMAL);
    // filter box
    int fx = bx + 128;
    gui_textfield2(window_handle, fx, by, 60, 24, g_filter[0] ? g_filter : "", false);
    if (!g_filter[0]) win_draw_text_small(window_handle, fx + 6, by + 7, "Filter..", files_dim(fp_field()));
}

// ---- sidebar (Places) -----------------------------------------------------
// Build a flat clickable list: section headers + rows. We recompute hit rows
// each draw and store their target paths for click handling.
static char side_target[40][MAX_PATH_LEN];
static int  side_y[40];
static int  side_kind[40];   // 0=path nav, 1=device(root), 2=network
static int  side_rows = 0;

static void draw_sidebar(void) {
    win_draw_rect(window_handle, 0, CONTENT_Y, SIDEBAR_W, CONTENT_H, SIDEBAR_BG);
    win_draw_rect(window_handle, SIDEBAR_W - 1, CONTENT_Y, 1, CONTENT_H, BORDER_COLOR);
    side_rows = 0;
    int y = CONTENT_Y + 8;
    win_draw_text(window_handle, 8, y, "Quick access", SIDE_TEXT); y += 20;
    for (int i = 0; qa_label[i]; i++) {
        char p[MAX_PATH_LEN]; qa_path(i, p);
        bool cur = str_eq(CUR.path, p);
        if (cur) win_draw_rect(window_handle, 4, y - 2, SIDEBAR_W - 8, ITEM_HEIGHT, ITEM_SELECTED);
        { const char *icn = (i == 0) ? "HOME" : "FOLDER";
          uint32_t tint = cur ? files_fg(ITEM_SELECTED) : SIDE_TEXT;
          if (!draw_mico(icn, 12, y, ICON_SIZE, tint)) draw_folder_icon(12, y); }
        win_draw_text(window_handle, 34, y, qa_label[i], SIDE_TEXT);
        str_copy(side_target[side_rows], p, MAX_PATH_LEN);
        side_y[side_rows] = y; side_kind[side_rows] = 0; side_rows++;
        y += ITEM_HEIGHT;
    }
    y += 6;
    win_draw_text(window_handle, 8, y, "This PC", SIDE_TEXT); y += 20;
    for (int i = 0; i < g_disk_count; i++) {
        win_draw_rect(window_handle, 12, y + 2, ICON_SIZE, ICON_SIZE - 2, 0x00808890);
        gui_draw_rect_outline(window_handle, 12, y + 2, ICON_SIZE, ICON_SIZE - 2, 0x00505860);
        char lbl[40]; int l = 0;
        const char *pfx = "Disk "; while (*pfx) lbl[l++] = *pfx++;
        lbl[l++] = '0' + i; lbl[l++] = ' '; lbl[l++] = '(';
        char mb[16]; gui_itoa(g_disks[i].size_mb, mb, 16);
        for (int k = 0; mb[k] && l < 30; k++) lbl[l++] = mb[k];
        const char *sfx = "MB)"; while (*sfx && l < 38) lbl[l++] = *sfx++;
        lbl[l] = 0;
        win_draw_text_small(window_handle, 34, y + 4, lbl, SIDE_TEXT);
        str_copy(side_target[side_rows], "/", MAX_PATH_LEN);
        side_y[side_rows] = y; side_kind[side_rows] = 1; side_rows++;
        y += ITEM_HEIGHT;
    }
    if (g_disk_count == 0) { win_draw_text_small(window_handle, 34, y + 4, "(no drives)", SIDE_DIM); y += ITEM_HEIGHT; }
    // ext2 volume mounted at /ext2 by the kernel ext2 driver (#99). Browsable
    // (read + create) through the normal file API.
    {
        if (str_eq(CUR.path, "/ext2")) win_draw_rect(window_handle, 4, y - 2, SIDEBAR_W - 8, ITEM_HEIGHT, ITEM_SELECTED);
        win_draw_rect(window_handle, 12, y + 2, ICON_SIZE, ICON_SIZE - 2, 0x00608060);
        gui_draw_rect_outline(window_handle, 12, y + 2, ICON_SIZE, ICON_SIZE - 2, 0x00405040);
        win_draw_text_small(window_handle, 34, y + 4, "ext2 (/ext2)", SIDE_TEXT);
        str_copy(side_target[side_rows], "/ext2", MAX_PATH_LEN);
        side_y[side_rows] = y; side_kind[side_rows] = 1; side_rows++;
        y += ITEM_HEIGHT;
    }
    y += 6;
    win_draw_text(window_handle, 8, y, "Network", SIDE_TEXT); y += 20;
    if (!draw_mico("NETWORK", 12, y, ICON_SIZE, SIDE_TEXT))
        win_draw_rect(window_handle, 12, y + 3, ICON_SIZE, ICON_SIZE - 4, 0x004A78C0);
    win_draw_text(window_handle, 34, y, "Network", SIDE_TEXT);
    str_copy(side_target[side_rows], "/NET", MAX_PATH_LEN);
    side_y[side_rows] = y; side_kind[side_rows] = 2; side_rows++;
    y += ITEM_HEIGHT;

    // Recycle Bin: opens the integrated trash view (side_kind 3) instead of a
    // directory. Highlighted when the recycle view is the active content.
    y += 6;
    win_draw_text(window_handle, 8, y, "System", SIDE_TEXT); y += 20;
    if (g_in_recycle) win_draw_rect(window_handle, 4, y - 2, SIDEBAR_W - 8, ITEM_HEIGHT, ITEM_SELECTED);
    { uint32_t tint = g_in_recycle ? files_fg(ITEM_SELECTED) : SIDE_TEXT;
      if (!draw_mico("RECYCLE", 12, y, ICON_SIZE, tint)) {
          // Simple trash-can fallback glyph.
          win_draw_rect(window_handle, 13, y + 4, ICON_SIZE - 2, ICON_SIZE - 4, 0x00808890);
          win_draw_rect(window_handle, 11, y + 1, ICON_SIZE + 2, 3, 0x00606870);
      } }
    win_draw_text(window_handle, 34, y, "Recycle Bin", SIDE_TEXT);
    str_copy(side_target[side_rows], TRASH_DIR, MAX_PATH_LEN);
    side_y[side_rows] = y; side_kind[side_rows] = 3; side_rows++;
}

// ---- file list ------------------------------------------------------------
static void draw_file_list(void) {
    int lx = list_x(), lw = list_w();
    win_draw_rect(window_handle, lx, CONTENT_Y, lw + 16, CONTENT_H, BG_COLOR);

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
            if (i == CUR.sel) win_draw_rect(window_handle, cx + 4, cy, cw - 8, chh - 4, ITEM_SELECTED);
            else if (i == hover_item) win_draw_rect(window_handle, cx + 4, cy, cw - 8, chh - 4, ITEM_HOVER);
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
            win_draw_text_small(window_handle, cx + cw/2 - gui_ttf_width(nm, 11)/2, cy + 46, nm, TEXT_COLOR);
        }
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
        if (i == CUR.sel) win_draw_rect(window_handle, lx + 2, y, lw - 4, ITEM_HEIGHT, ITEM_SELECTED);
        else if (i == hover_item) win_draw_rect(window_handle, lx + 2, y, lw - 4, ITEM_HEIGHT, ITEM_HOVER);
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
        int namecol = (g_view == VIEW_DETAILS) ? (lw - 150) : (lw - 80);
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
    if (item_count > visible) {
        int sbx = lx + lw, sh = CONTENT_H;
        int th = (visible * sh) / item_count; if (th < 20) th = 20;
        int ty = max_scroll ? (CUR.scroll * (sh - th)) / max_scroll : 0;
        win_draw_rect(window_handle, sbx, CONTENT_Y, 14, sh, THEME_SCROLLBAR_BG);
        win_draw_rect(window_handle, sbx + 2, CONTENT_Y + ty, 10, th, THEME_SCROLLBAR_THUMB);
    }
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

static int rb_area_x(void) { return SIDEBAR_W; }
static int rb_area_w(void) { return WIN_W - SIDEBAR_W; }
static int rb_list_y(void) { return CONTENT_Y + RB_TOOLBAR_H + RB_HEADER_H; }
static int rb_list_h(void) { return CONTENT_H - RB_TOOLBAR_H - RB_HEADER_H; }

static void draw_recycle_view(void) {
    int ax = rb_area_x(), aw = rb_area_w();
    // Background spans the whole content area (covers where preview would be).
    win_draw_rect(window_handle, ax, CONTENT_Y, aw, CONTENT_H, BG_COLOR);

    // Action toolbar
    win_draw_rect(window_handle, ax, CONTENT_Y, aw, RB_TOOLBAR_H, TOOLBAR_BG);
    win_draw_rect(window_handle, ax, CONTENT_Y + RB_TOOLBAR_H - 1, aw, 1, BORDER_COLOR);
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
    win_draw_rect(window_handle, ax, hy, aw, RB_HEADER_H, fp_panel());
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
        if (it->selected) win_draw_rect(window_handle, ax + 2, y, aw - 4, RB_ROW_H, ITEM_SELECTED);
        else if (idx == rb_hover) win_draw_rect(window_handle, ax + 2, y, aw - 4, RB_ROW_H, ITEM_HOVER);
        uint32_t tint = it->selected ? files_fg(ITEM_SELECTED) : TEXT_COLOR;
        uint32_t dimc = it->selected ? files_dim(ITEM_SELECTED) : DIM_TEXT;
        // Checkbox
        win_draw_rect(window_handle, ax + 8, y + 4, 16, 16, fp_field());
        gui_draw_rect_outline(window_handle, ax + 8, y + 4, 16, 16, BORDER_COLOR);
        if (it->selected) {
            win_draw_rect(window_handle, ax + 11, y + 9, 10, 2, fp_acc());
            win_draw_rect(window_handle, ax + 13, y + 7, 2, 10, fp_acc());
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
    // Scrollbar
    if (rb_count > visible) {
        int sbx = ax + aw - 14, sh = lh;
        int th = (visible * sh) / rb_count; if (th < 20) th = 20;
        int maxs = rb_count - visible;
        int ty = maxs ? (rb_scroll * (sh - th)) / maxs : 0;
        win_draw_rect(window_handle, sbx, ly, 14, sh, THEME_SCROLLBAR_BG);
        win_draw_rect(window_handle, sbx + 2, ly + ty, 10, th, THEME_SCROLLBAR_THUMB);
    }
}

// ---- dropdown menus -------------------------------------------------------
static const char *menu_new_items[]  = { "New Folder", "New Text File", NULL };
static const char *menu_view_items[] = { "Icons", "List", "Details", "--", "Sort: Name", "Sort: Size", "Sort: Type", "--", "Preview Pane", NULL };
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
    win_draw_rect(window_handle, mx, my, mw, mh, theme_color(THEME_COLOR_MENU_BG));
    gui_draw_rect_outline(window_handle, mx, my, mw, mh, BORDER_COLOR);
    int yy = my + 3;
    for (int i = 0; m[i]; i++) {
        if (str_eq(m[i], "--")) { win_draw_rect(window_handle, mx + 4, yy + 10, mw - 8, 1, BORDER_COLOR); yy += 22; continue; }
        win_draw_text(window_handle, mx + 10, yy + 4, m[i], files_fg(theme_color(THEME_COLOR_MENU_BG)));
        yy += 22;
    }
}

// ---- status bar -----------------------------------------------------------
static void draw_status(void) {
    int y = WIN_H - STATUS_H;
    win_draw_rect(window_handle, 0, y, WIN_W, STATUS_H, STATUS_BG);
    win_draw_rect(window_handle, 0, y, WIN_W, 1, BORDER_COLOR);
    if (g_in_recycle) {
        char s[64]; gui_itoa(rb_count, s, 16); int l = str_len(s);
        const char *it = " items in Recycle Bin"; while (*it) s[l++] = *it++; s[l] = 0;
        win_draw_text(window_handle, 8, y + 4, s, TEXT_COLOR);
        return;
    }
    char s[48]; gui_itoa(item_count, s, 16); int l = str_len(s);
    const char *it = " items"; while (*it) s[l++] = *it++; s[l] = 0;
    win_draw_text(window_handle, 8, y + 4, s, TEXT_COLOR);
    if (CUR.sel >= 0 && CUR.sel < item_count)
        win_draw_text(window_handle, 200, y + 4, items[CUR.sel].name, TEXT_COLOR);
}

// Map the Files theme palette into the shared style engine each redraw, so the
// gui_* primitives (buttons, fields, tabs) match the active theme + render
// modern (rounded/AA) or classic (beveled) like the Settings app.
static void files_apply_style(void) {
    gui_set_style(theme_get_active() == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface        = fp_content();
    p.surface_raised = fp_toolbar();
    p.ink            = files_fg(fp_content());
    p.ink_dim        = files_dim(fp_content());
    p.accent         = fp_acc();
    p.accent_hover   = gui_lighten(fp_acc(), 24);
    p.border         = theme_color(THEME_COLOR_TEXTBOX_BORDER);
    p.field_bg       = fp_field();
    p.field_border   = theme_color(THEME_COLOR_TEXTBOX_BORDER);
    p.track          = fp_tint(fp_content(), fp_acc(), 30);
    gui_set_palette(&p);
}

static void fb_redraw(void) {
    files_apply_style();
    draw_tabbar();
    draw_toolbar();
    draw_sidebar();
    if (g_in_recycle) {
        draw_recycle_view();
    } else {
        draw_file_list();
        if (g_preview_on) draw_preview();
    }
    draw_status();
    if (g_menu != MENU_NONE) draw_menu();
    if (g_props_open) draw_props();   // (#251) Properties dialog on top
    if (g_openwith_open) draw_openwith();  // Task C: Open with picker
    if (g_te_open) draw_te();              // Task C: inline Rename text entry
    win_invalidate(window_handle);
}

// ---- operations -----------------------------------------------------------
static void do_new_folder(void) {
    static const char *cand[] = { "NEWFOLD", "NEWFOLD1", "NEWFOLD2", "NEWFOLD3", "NEWFOLD4" };
    char p[MAX_PATH_LEN];
    for (int i = 0; i < 5; i++) { path_join(p, CUR.path, cand[i]); if (mkdir(p, 0755) == 0) break; }
    load_directory(CUR.path); fb_redraw();
}
static void do_new_file(void) {
    static const char *cand[] = { "NEW.TXT", "NEW1.TXT", "NEW2.TXT", "NEW3.TXT" };
    char p[MAX_PATH_LEN];
    for (int i = 0; i < 4; i++) { path_join(p, CUR.path, cand[i]);
        int fd = open(p, 0x41); if (fd >= 0) { close(fd); break; } }
    load_directory(CUR.path); fb_redraw();
}
static void do_delete(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..") || str_eq(it->name, "(empty)")) return;
    char src[MAX_PATH_LEN], dst[MAX_PATH_LEN];
    path_join(src, CUR.path, it->name);
    mkdir("/CONFIG", 0755);
    mkdir(TRASH_DIR, 0755);
    path_join(dst, TRASH_DIR, it->name);
    // (#239) ext2 rename() is unreliable, so fall back to copy_file()+unlink for
    // files. Only when the file truly reaches the bin do we record the index.
    int moved = 0;
    if (rename(src, dst) == 0) moved = 1;
    else if (!it->is_directory && copy_file(src, dst) == 0) { unlink(src); moved = 1; }
    if (moved) {
        int fd = open(TRASH_INDEX, 0x41);
        if (fd >= 0) { sys_seek(fd, 0, 2);
            char line[MAX_PATH_LEN * 2]; int l = 0;
            for (int i = 0; it->name[i] && l < 120; i++) line[l++] = it->name[i];
            line[l++] = '|';
            for (int i = 0; src[i] && l < (int)sizeof(line) - 2; i++) line[l++] = src[i];
            line[l++] = '\n'; write(fd, line, l); close(fd); }
    } else { if (it->is_directory) rmdir(src); else unlink(src); }
    load_directory(CUR.path); fb_redraw();
}
static void open_selected(void) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
    file_entry_t *it = &items[CUR.sel];
    if (str_eq(it->name, "..")) { navigate_up(); return; }
    if (str_eq(it->name, "(empty)")) return;
    // #317: Network folder entries -> Add dialog or mount+browse a saved share.
    if (str_eq(CUR.path, "/NET")) {
        if (it->name[0] == '[') { open_add_network(); return; }
        for (int k = 0; k < g_netmount_count; k++) {
            if (str_eq(g_netmounts[k].label, it->name)) {
                netmount_t *m = &g_netmounts[k];
                net_mount(m->server, m->share, m->user, m->pass);
                char np[MAX_PATH_LEN]; int l = 0;
                const char *pfx = "/SMB/"; while (*pfx) np[l++] = *pfx++;
                for (int i = 0; m->server[i] && l < MAX_PATH_LEN-2; i++) np[l++] = m->server[i];
                np[l++] = '/';
                for (int i = 0; m->share[i] && l < MAX_PATH_LEN-1; i++) np[l++] = m->share[i];
                np[l] = 0;
                navigate_to(np);
                return;
            }
        }
        return;
    }
    if (it->is_directory) { char np[MAX_PATH_LEN]; path_join(np, CUR.path, it->name); navigate_to(np); }
    else { char full[MAX_PATH_LEN]; path_join(full, CUR.path, it->name);
        const char *app = default_app_for(it->name);
        char *av[2]; av[0] = (char *)app; av[1] = full; sys_spawn_args(app, av, 2); }
}

// ---- Task C: inline Rename (reusable text-entry overlay) -------------------
static void rename_commit(const char *newname) {
    if (CUR.sel < 0 || CUR.sel >= item_count) return;
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
        else if (g_te_purpose == 2) { parse_add_input(g_te_buf); navigate_to("/NET"); }  // #317
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
    { "Text Editor",  "/APPS/editor"   },
    { "Image Viewer", "/APPS/imgview"  },
    { "Maytera Studio", "/APPS/paint"  },
    { "Web Browser",  "/APPS/browser"  },
    { "Music Player", "/APPS/musicplr" },
    { "Media Player", "/APPS/mplayer"  },
    { "Terminal",     "/APPS/terminal" },
    { "Python",       "/APPS/python"   },
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

// Stream-copy a regular file src -> dst. Returns 0 on success.
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
    close(in); close(out);
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

// ---- Properties dialog (#251) ---------------------------------------------
static void draw_props(void) {
    if (!g_props_open) return;
    if (CUR.sel < 0 || CUR.sel >= item_count) { g_props_open = 0; return; }
    file_entry_t *it = &items[CUR.sel];
    int bw = 340, bh = 168;
    int bx = (WIN_W - bw) / 2, by = (WIN_H - bh) / 2;
    win_draw_rect(window_handle, bx, by, bw, bh, theme_color(THEME_COLOR_MENU_BG));
    gui_draw_rect_outline(window_handle, bx, by, bw, bh, BORDER_COLOR);
    win_draw_rect(window_handle, bx, by, bw, 22, fp_acc());
    win_draw_text(window_handle, bx + 10, by + 4, "Properties", files_fg(fp_acc()));
    int tx = bx + 16, ty = by + 34;
    win_draw_text(window_handle, tx, ty, "Name:", TEXT_COLOR);
    win_draw_text(window_handle, tx + 90, ty, it->name, TEXT_COLOR); ty += 24;
    win_draw_text(window_handle, tx, ty, "Type:", TEXT_COLOR);
    win_draw_text(window_handle, tx + 90, ty, it->is_directory ? "Folder" : "File", TEXT_COLOR); ty += 24;
    win_draw_text(window_handle, tx, ty, "Size:", TEXT_COLOR);
    { char sz[24]; fmt_size((uint32_t)it->size, sz); win_draw_text(window_handle, tx + 90, ty, sz, TEXT_COLOR); } ty += 24;
    win_draw_text(window_handle, tx, ty, "Location:", TEXT_COLOR);
    win_draw_text(window_handle, tx + 90, ty, CUR.path, TEXT_COLOR); ty += 30;
    win_draw_text(window_handle, bx + bw - 130, by + bh - 26, "[ click to close ]", DIM_TEXT);
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
    win_draw_rect(window_handle, bx, by, TE_W, TE_H, theme_color(THEME_COLOR_MENU_BG));
    gui_draw_rect_outline(window_handle, bx, by, TE_W, TE_H, BORDER_COLOR);
    win_draw_rect(window_handle, bx, by, TE_W, 22, fp_acc());
    win_draw_text(window_handle, bx + 10, by + 4, g_te_title, files_fg(fp_acc()));
    win_draw_text(window_handle, bx + 16, by + 34, "New name:", TEXT_COLOR);
    // editable field (with a simple trailing caret)
    char disp[MAX_NAME_LEN + 2];
    str_copy(disp, g_te_buf, MAX_NAME_LEN);
    { int l = str_len(disp); if (l < MAX_NAME_LEN) { disp[l] = '_'; disp[l+1] = 0; } }
    gui_draw_textfield(window_handle, bx + 16, by + 56, TE_W - 32, 28,
                       disp, INPUT_BG, INPUT_TEXT, BORDER_COLOR);
    // OK / Cancel buttons
    int oy = by + TE_H - TE_BTN_H - 12;
    int okx = bx + TE_W - 2 * TE_BTN_W - 24;
    int cax = bx + TE_W - TE_BTN_W - 12;
    gui_draw_button(window_handle, okx, oy, TE_BTN_W, TE_BTN_H, "OK",
                    fp_acc(), files_fg(fp_acc()), false, false);
    gui_draw_button(window_handle, cax, oy, TE_BTN_W, TE_BTN_H, "Cancel",
                    BTN_FACE, BTN_TEXT, false, false);
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

// Open-with picker geometry.
#define OW_W 300
#define OW_ROWH 28
static inline int ow_h(void){ return 34 + OW_N * OW_ROWH + 14; }
static inline int ow_bx(void){ return (WIN_W - OW_W) / 2; }
static inline int ow_by(void){ return (WIN_H - ow_h()) / 2; }
static void draw_openwith(void) {
    if (!g_openwith_open) return;
    int bx = ow_bx(), by = ow_by(), bh = ow_h();
    win_draw_rect(window_handle, bx, by, OW_W, bh, theme_color(THEME_COLOR_MENU_BG));
    gui_draw_rect_outline(window_handle, bx, by, OW_W, bh, BORDER_COLOR);
    win_draw_rect(window_handle, bx, by, OW_W, 22, fp_acc());
    win_draw_text(window_handle, bx + 10, by + 4, "Open with", files_fg(fp_acc()));
    if (CUR.sel >= 0 && CUR.sel < item_count)
        win_draw_text(window_handle, bx + 16, by + 28, items[CUR.sel].name, DIM_TEXT);
    int yy = by + 46;
    for (int i = 0; i < OW_N; i++) {
        if (i == g_ow_hover)
            win_draw_rect(window_handle, bx + 4, yy - 3, OW_W - 8, OW_ROWH - 2, ITEM_HOVER);
        win_draw_text(window_handle, bx + 22, yy + 3, OW_APPS[i].label,
                      files_fg(theme_color(THEME_COLOR_MENU_BG)));
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
            case 8: g_preview_on = !g_preview_on; break;
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
    (void)argc; (void)argv;

    // Resolve the current user's home directory for Quick access. Desktop apps
    // run as root (home "/"), so fall back to the admin home where the standard
    // skeleton folders live, rather than pointing Quick access at "/".
    struct passwd *pw = getpwuid(getuid());
    if (pw && pw->pw_dir && pw->pw_dir[0] && !(pw->pw_dir[0] == '/' && pw->pw_dir[1] == '\0'))
        str_copy(g_home, pw->pw_dir, MAX_PATH_LEN);
    else
        str_copy(g_home, "/HOME/ADMIN", MAX_PATH_LEN);

    enumerate_disks();

    for (int i = 0; i < MAX_TABS; i++) tabs[i].sel = -1;
    tabs[0].used = true; str_copy(tabs[0].path, g_home, MAX_PATH_LEN);
    current_path = tabs[0].path;

    window_handle = win_create("Files", win_x, win_y, WIN_W, WIN_H);
    if (window_handle < 0) return 1;

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
    load_directory(startpath);
    // #239: the desktop / start-menu Recycle Bin launches drop a one-shot
    // sentinel so Files opens straight into the integrated Recycle Bin view.
    { int rf = open("/RECYVIEW.FLG", 0); if (rf >= 0) { close(rf); unlink("/RECYVIEW.FLG"); g_in_recycle = 1; rb_load(); } }
    fb_redraw();

    gui_event_t event;
    int running = 1;
    while (running) {
        int et = win_get_event(window_handle, &event, 100);
        if (et == 0) continue;
        switch (event.type) {
        case EVENT_REDRAW: fb_redraw(); break;
        case EVENT_RESIZE:
            if (event.mouse_x > 0 && event.mouse_y > 0) { g_win_w = event.mouse_x; g_win_h = event.mouse_y; }
            fb_redraw(); break;
        case EVENT_WINDOW_CLOSE: running = 0; break;
        case EVENT_KEY_DOWN: {
            char c = event.key_char; uint32_t kc = event.keycode;
            // Task C: modal overlays capture keys first.
            if (g_te_open) { te_key(c, kc); fb_redraw(); break; }
            if (g_openwith_open) { if (c == 27) { g_openwith_open = 0; fb_redraw(); } break; }
            if (g_props_open && c == 27) { g_props_open = 0; fb_redraw(); break; }
            if (g_menu != MENU_NONE && c == 27) { g_menu = MENU_NONE; fb_redraw(); break; }
            // (#251) clipboard keyboard shortcuts (Ctrl+C/X/V arrive as control chars)
            if (!g_in_recycle) {
                if (c == 0x03) { do_copy();  break; }   // Ctrl+C
                if (c == 0x18) { do_cut();   break; }   // Ctrl+X
                if (c == 0x16) { do_paste(); break; }   // Ctrl+V (do_paste redraws)
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
            // Task C: modal overlays consume clicks first.
            if (g_te_open) {
                int h = te_hit(lx, ly);
                if (h == 0) { g_te_open = 0; if (g_te_purpose == 1) rename_commit(g_te_buf); }
                else if (h == 1 || h == -2) { g_te_open = 0; }
                fb_redraw(); break;
            }
            if (g_openwith_open) {
                int h = ow_hit(lx, ly);
                if (h >= 0) ow_pick(h);
                else if (h == -2) g_openwith_open = 0;
                fb_redraw(); break;
            }
            // (#251) Properties dialog is modal: any click dismisses it.
            if (g_props_open) { g_props_open = 0; fb_redraw(); break; }
            // menu first
            if (g_menu != MENU_NONE) {
                int h = menu_hit(lx, ly);
                if (h == -2) { g_menu = MENU_NONE; fb_redraw(); }
                else if (h >= 0) menu_select(h);
                break;
            }
            // tab bar
            if (ly >= 0 && ly < TABBAR_H) {
                for (int i = 0; i < tab_count; i++) {
                    int tx = 4 + i * (TAB_W + 2);
                    if (lx >= tx && lx < tx + TAB_W) {
                        if (lx >= tx + TAB_W - 16) tab_close(i); else tab_switch(i);
                        break;
                    }
                }
                int ax = 4 + tab_count * (TAB_W + 2);
                if (tab_count < MAX_TABS && lx >= ax && lx < ax + 24) tab_new();
                break;
            }
            // toolbar
            int by = TABBAR_H + 8;
            if (ly >= by && ly < by + 24) {
                if (lx >= 8 && lx < 34) navigate_back();
                else if (lx >= 36 && lx < 62) navigate_fwd();
                else if (lx >= 64 && lx < 90) navigate_up();
                else {
                    int bx = WIN_W - 8 - 3 * 64;
                    if (lx >= bx && lx < bx + 60) { g_menu = MENU_NEW; g_menu_x = bx; g_menu_y = by + 26; fb_redraw(); }
                    else if (lx >= bx + 64 && lx < bx + 124) { g_menu = MENU_VIEW; g_menu_x = bx + 64; g_menu_y = by + 26; fb_redraw(); }
                }
                break;
            }
            // sidebar
            if (lx < SIDEBAR_W && ly >= CONTENT_Y) {
                for (int i = 0; i < side_rows; i++) {
                    if (ly >= side_y[i] - 2 && ly < side_y[i] + ITEM_HEIGHT - 2) {
                        if (side_kind[i] == 3) {
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
                        if (rb_count_selected()) rb_delete_selected();
                        fb_redraw();
                    } else if (rx >= RB_BTN_EMPTY_X && rx < RB_BTN_EMPTY_X + RB_BTN_EMPTY_W) {
                        if (rb_count) rb_empty();
                        fb_redraw();
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
            // preview pane (ignore clicks)
            if (g_preview_on && lx >= WIN_W - PREVIEW_W) break;
            // file list
            if (lx >= list_x() && ly >= CONTENT_Y && ly < CONTENT_Y + CONTENT_H) {
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
            if (g_view != VIEW_ICONS && lx >= list_x() && (!g_preview_on || lx < WIN_W - PREVIEW_W)
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
    win_destroy(window_handle);
    return 0;
}
