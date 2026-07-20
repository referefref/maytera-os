// rss - Modern RSS / Atom / RDF feed reader for MayteraOS (user-mode).
//
// A 3-pane reader (feeds sidebar | article list | article reader) with a proper
// toolbar, in the Settings/Files design language and theme-aware. Feed parsing
// is done by the Rust core (rss_rs.rs / librss_rs.a) which handles RSS 2.0,
// Atom 1.0 and RSS 1.0/RDF and is hardened against malformed/hostile network
// input (bounds-checked, panic-free). See rss_rs.h for the FFI.
//
// Features: search (feeds + articles), add/remove/reorder feeds persisted to
// /CONFIG/RSSFEEDS.CFG, adjustable reader font size + family (live), AI article
// summarization via the shared aiclient (Kimi) on a worker thread so the UI
// never blocks, and async HTTP/HTTPS fetch (http_fetch_start/poll/read).
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"
#include "../../libc/pthread.h"
#include "../../libc/aiclient.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"
#include "../../libc/gui_style.h"
#include "rss_rs.h"

// The Mono reader typeface deliberately uses the crisp 8x16 bitmap font, so
// capture the real bitmap entry point BEFORE the TTF redirect macro below
// rewrites every other win_draw_text call site.
static inline void rss_text_bitmap(int h, int x, int y, const char *s, uint32_t c) {
    win_draw_text(h, x, y, s, c);
}

// Route all chrome text through the antialiased TrueType path (matches Files and
// Settings). 14pt is the shared GUI_TTF_SIZE body size; 11pt is the small/label
// size used for column headers and the status bar.
#define win_draw_text(h, x, y, s, c)       win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define win_draw_text_small(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 11, (c))

#define WIN_W 940
#define WIN_H 620
#define CHAR_W 8
#define CHAR_H 16

#define TOOLBAR_H  36
#define STATUS_H   20
#define SIDEBAR_W  186
#define ROW_H      20

// Fetch buffer: feeds run from a few KB to a few hundred KB (NASA ~345 KB).
#define FETCH_MAX (512 * 1024)
static char g_fetch[FETCH_MAX];

// ---- Theme-aware palette --------------------------------------------------
// Derived live from the kernel theme via theme_color(), exactly as the Files and
// Settings apps do. The previous hardcoded switch over five theme ids was the
// bespoke look the style guide forbids: any theme outside the switch fell
// through to Dark, so a light theme rendered light ink on a light surface.
static inline unsigned int rss_fg(unsigned int bg) {
    int r = (bg >> 16) & 0xFF, g = (bg >> 8) & 0xFF, b = bg & 0xFF;
    int lum = (r * 30 + g * 59 + b * 11) / 100;
    return lum > 140 ? 0x00181818u : 0x00F0F0F0u;
}
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
// Row selection: an accent-tinted dark grey on dark themes, a light accent tint
// on light themes. Never the raw accent, which renders near-black on Nord.
static inline unsigned int fp_sel(void){ return fp_dark()
    ? fp_tint(0x003C434F, fp_acc(), 28)
    : fp_tint(0x00CCD6E6, fp_acc(), 26); }
static inline unsigned int rss_dim(unsigned int bg) {
    unsigned int ink = rss_fg(bg);
    int ir=(ink>>16)&0xFF, ig=(ink>>8)&0xFF, ib=ink&0xFF;
    int br=(bg>>16)&0xFF, bgc=(bg>>8)&0xFF, bb=bg&0xFF;
    // Bias toward the ink (5/8) so secondary text stays readable on dark themes.
    return ((((ir*5+br*3)/8)&0xFF)<<16) | ((((ig*5+bgc*3)/8)&0xFF)<<8) | (((ib*5+bb*3)/8)&0xFF);
}
// Pull an accent-ish hue toward readable contrast against a given surface, so
// title/link/status inks never wash out on a light theme.
static inline unsigned int rss_on(unsigned int fg, unsigned int bg) {
    int need_dark = fp_lum(bg) > 140;
    int lum = fp_lum(fg);
    if (need_dark && lum > 120) return fp_tint(fg, 0x00000000, 55);
    if (!need_dark && lum < 110) return fp_tint(fg, 0x00FFFFFF, 45);
    return fg;
}

#define BG_COLOR      fp_content()
#define LIST_BG       fp_content()
#define FEEDS_BG      fp_panel()
#define DETAIL_BG     fp_content()
#define TOOLBAR_BG    fp_toolbar()
#define STATUS_BG     fp_toolbar()
#define SEL_BG        fp_sel()
#define BTN_BG        fp_toolbar()
#define BTN_HOVER     fp_tint(fp_toolbar(), fp_acc(), 16)
#define TEXT_COLOR    rss_fg(fp_content())
#define DIM_COLOR     rss_dim(fp_content())
#define BORDER_COLOR  fp_border()
#define TITLE_COLOR   rss_on(fp_acc(), fp_content())
#define LINK_COLOR    rss_on(fp_acc(), fp_content())
#define ERR_COLOR     rss_on(0x00FF6666, fp_toolbar())
#define OK_COLOR      rss_on(0x0080D080, fp_toolbar())
#define ACCENT_TXT    rss_on(fp_acc(), fp_toolbar())

// Map the RSS palette into the shared style engine each redraw, so the gui_*
// primitives (buttons, fields) match the active theme and render modern
// (rounded/AA) or classic (beveled) exactly like Files and Settings.
static void rss_apply_style(void) {
    gui_set_style(theme_get_active() == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface        = fp_content();
    p.surface_raised = fp_toolbar();
    p.ink            = rss_fg(fp_content());
    p.ink_dim        = rss_dim(fp_content());
    p.accent         = fp_acc();
    p.accent_hover   = gui_lighten(fp_acc(), 24);
    p.border         = fp_border();
    p.field_bg       = fp_field();
    p.field_border   = theme_color(THEME_COLOR_TEXTBOX_BORDER);
    p.track          = fp_tint(fp_content(), fp_acc(), 30);
    gui_set_palette(&p);
}

static int win = -1;
static int g_w = WIN_W, g_h = WIN_H;
static int g_theme_last = -1;   // last seen theme id, for repaint-on-switch

// ---- Data model: feeds ----------------------------------------------------
#define MAX_FEEDS 40
typedef struct { char name[40]; char url[220]; } feed_t;
static feed_t g_feeds[MAX_FEEDS];
static int g_feed_count = 0;
static int g_cur_feed = 0;

static const feed_t DEFAULT_FEEDS[] = {
    { "BBC World",   "https://feeds.bbci.co.uk/news/world/rss.xml" },
    { "Hacker News", "https://hnrss.org/frontpage" },
    { "Lobsters",    "https://lobste.rs/rss" },
    { "Slashdot",    "http://rss.slashdot.org/Slashdot/slashdotMain" },
    { "Reddit Rust", "https://www.reddit.com/r/rust/.rss" },
    { "xkcd",        "https://xkcd.com/rss.xml" },
};
#define N_DEFAULT ((int)(sizeof(DEFAULT_FEEDS)/sizeof(DEFAULT_FEEDS[0])))

// ---- Data model: items (display copies of the parsed feed) ----------------
#define MAX_ITEMS  200
#define TITLE_MAX  256
#define LINK_MAX   300
#define DATE_MAX   64
#define AUTHOR_MAX 96
#define DESC_MAX   4096
#define IMGTXT_MAX 512
typedef struct {
    char title[TITLE_MAX];
    char link[LINK_MAX];
    char date[DATE_MAX];
    char author[AUTHOR_MAX];
    char desc[DESC_MAX];
    // Inline image, from the Rust parser. image_url may be relative and is
    // attacker-controlled: it is resolved and scheme-gated before any fetch.
    char image_url[LINK_MAX];
    char image_alt[IMGTXT_MAX];
    char image_title[IMGTXT_MAX];
} item_t;
static item_t g_items[MAX_ITEMS];
static int g_item_count = 0;
static int g_sel_item = -1;
static int g_list_scroll = 0;
static int g_reader_scroll = 0;      // pixels
static char g_feed_title[80];
static int g_feed_format = RSS_FMT_UNKNOWN;

// ---- Search + filtered views ----------------------------------------------
static char g_search[80];
static textfield_t g_search_tf;
static int g_feed_view[MAX_FEEDS];   static int g_feed_view_n = 0;
static int g_item_view[MAX_ITEMS];   static int g_item_view_n = 0;

// ---- Reader font controls -------------------------------------------------
// Two selectable reader typefaces using only the available draw syscalls:
//   0 = "Sans" : antialiased TrueType (proportional, size-adjustable)
//   1 = "Mono" : crisp 8x16 bitmap (fixed size, for dense/technical reading)
enum { RF_SANS, RF_MONO, RF_COUNT };
static const char *RF_NAME[RF_COUNT] = { "Sans (TTF)", "Mono (bitmap)" };
static int g_reader_size = 15;       // point size (10..30), used in Sans mode
static int g_reader_font = RF_SANS;  // reader typeface

// ---- Fetch state ----------------------------------------------------------
// ST_WAIT_NET: the app wants a feed but DHCP has not bound yet. The reader is
// launched from the desktop (or autostart) well before the lease completes, so
// latching a hard error at that moment left the app permanently showing
// "Network is down." on a machine whose network came up seconds later, and only
// a manual Refresh recovered it. We now park in ST_WAIT_NET and start the fetch
// ourselves the moment the stack reports up.
enum { ST_IDLE, ST_FETCHING, ST_READY, ST_ERROR, ST_WAIT_NET };
static int  g_state = ST_IDLE;
static int  g_net_wait_frames = 0;
// ~50ms per event-loop pass, so this is about 60 seconds of DHCP grace.
#define NET_WAIT_MAX_FRAMES 1200
static int  g_job = -1;
static int  g_poll_frames = 0;
#define FETCH_TIMEOUT_FRAMES 600
static char g_status_msg[140] = "Select a feed to read.";

// ---- Inline images ---------------------------------------------------------
// Feed images are ATTACKER-CONTROLLED bytes fed to a Ring-0 decoder: the kernel
// decodes BMP/PNG/JPEG in gui/image.c, and that code has a history (a JPEG OOB
// write, MAYTERA-SEC-2026-0013; a PNG IHDR overflow, #500). So we refuse
// obviously-hostile input from Ring 3 BEFORE the syscall:
//   - http/https only, and no literal private/loopback host
//   - a hard byte cap
//   - a magic-byte sniff, so non-images never reach the decoder at all
//   - a DECLARED-dimension cap, read straight from the header
// The dimension cap is the one that earns its keep: sys_decode_image() calls
// image_load(), which allocates the FULL-RESOLUTION source in the KERNEL heap
// and only then downscales into our box. The target box therefore bounds OUR
// memory, not the kernel's: a 200KB PNG declaring 30000x30000 asks the kernel
// for 3.6GB. That is the classic decompression bomb, and the byte cap does not
// stop it because the whole point is that it compresses well.
//
// This is DEFENSE IN DEPTH, NOT A SECURITY BOUNDARY. Any process can call
// SYS_DECODE_IMAGE directly with whatever it likes, so the kernel decoders still
// have to stand on their own; these checks only keep this app from being the
// thing that hands them a bomb.
#define IMG_MAX_BYTES  (1024 * 1024)      // transport caps bodies at 1MB anyway
#define IMG_MAX_DIM    4096               // == the kernel's JPEG cap; PNG/BMP have none
#define IMG_MAX_PIXELS (8 * 1024 * 1024)  // 8Mpx => <=32MB kernel decode buffer
#define IMG_BOX_MAX_W  900
#define IMG_BOX_MAX_H  1200
#define IMG_CACHE_N    8
#define IMG_BUDGET     (12 * 1024 * 1024) // total cached bytes (raw + decoded)
#define IMG_PLACE_H    64                 // placeholder strip height
// Arrowing down the article list must not spawn (and then cancel) a kernel
// fetch worker per keystroke: each http_fetch_start() creates a real process
// with a 128KB stack. Wait for the selection to settle first (~300ms).
#define IMG_SETTLE_FRAMES 6

enum { IC_FREE, IC_WANT, IC_FETCHING, IC_READY, IC_FAILED };
typedef struct {
    char url[LINK_MAX];
    int  state;
    unsigned char *raw;  int raw_len;     // encoded bytes: cached so a re-decode
                                          // (zoom/resize) never re-fetches
    unsigned int  *px;   int px_w, px_h;  // decoded BGRA, as produced
    int  box_w, box_h;                    // the box px was decoded for
    int  src_w, src_h;                    // declared source dimensions
    const char *err;                      // placeholder text when IC_FAILED
    char errbuf[96];                      // storage when `err` is per-image text
    const char *fmt;                      // "PNG"/"JPEG"/"BMP", for diagnostics
    unsigned int lru;
} imgent_t;
static imgent_t g_imgc[IMG_CACHE_N];
static unsigned int g_lru_clock = 0;
static int g_img_job = -1;        // async fetch job, or -1
static int g_img_slot = -1;       // cache slot that job belongs to
static int g_img_frames = 0;      // fetch timeout counter
static int g_img_settle = 0;      // debounce counter
static char g_img_last[LINK_MAX]; // last URL the reader asked for

// ---- Add-feed input mode --------------------------------------------------
static int g_add_mode = 0;
static char g_add_buf[220];
static textfield_t g_add_tf;

// ---- Focus ----------------------------------------------------------------
enum { FOCUS_LIST, FOCUS_SEARCH };
static int g_focus = FOCUS_LIST;

// ---- AI summarize (worker thread; UI never blocks) ------------------------
enum { AI_IDLE, AI_RUNNING, AI_DONE, AI_ERROR };
static volatile int g_ai_state = AI_IDLE;
static int  g_show_summary = 0;      // reader shows the AI summary vs. article
static char g_ai_prompt[6000];
static char g_ai_result[6000];
static pthread_t g_ai_thread;

// ---- Small helpers --------------------------------------------------------
static void scpy(char *dst, const char *src, int max) {
    int i;
    for (i = 0; i < max - 1 && src && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}
static char lc(char c){ return (c>='A'&&c<='Z')?c+32:c; }
// case-insensitive substring test
static int ci_contains(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (hay[i+j] && needle[j] && lc(hay[i+j]) == lc(needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

// Decode one HTML entity at s (points at '&'). Consumes >=1 source char.
static int decode_entity(const char *s, char *out) {
    if (strncmp(s, "&amp;", 5) == 0)  { *out = '&';  return 5; }
    if (strncmp(s, "&lt;", 4) == 0)   { *out = '<';  return 4; }
    if (strncmp(s, "&gt;", 4) == 0)   { *out = '>';  return 4; }
    if (strncmp(s, "&quot;", 6) == 0) { *out = '"';  return 6; }
    if (strncmp(s, "&apos;", 6) == 0) { *out = '\''; return 6; }
    if (strncmp(s, "&nbsp;", 6) == 0) { *out = ' ';  return 6; }
    if (s[1] == '#') {
        int v = 0, i = 2, hex = 0;
        if (s[2] == 'x' || s[2] == 'X') { hex = 1; i = 3; }
        while (s[i] && s[i] != ';' && i < 10) {
            char c = s[i];
            if (hex) {
                if (c>='0'&&c<='9') v=v*16+(c-'0');
                else if (c>='a'&&c<='f') v=v*16+(c-'a'+10);
                else if (c>='A'&&c<='F') v=v*16+(c-'A'+10);
                else break;
            } else { if (c>='0'&&c<='9') v=v*10+(c-'0'); else break; }
            i++;
        }
        if (s[i] == ';') { *out = (v>=32&&v<127)?(char)v:'?'; return i+1; }
    }
    *out = '&';
    return 1;
}

// Strip markup tags, unwrap CDATA leftovers, decode entities, collapse spaces.
// (The Rust parser already decoded XML entities and unwrapped CDATA, but feed
// descriptions frequently carry HTML markup inside; this renders that to text.)
static void clean_text(const char *src, char *out, int cap) {
    int o = 0, i = 0, lastsp = 1;
    int srclen = (int)strlen(src);
    while (i < srclen && o < cap - 1) {
        if (strncmp(src + i, "<![CDATA[", 9) == 0) { i += 9; continue; }
        if (strncmp(src + i, "]]>", 3) == 0) { i += 3; continue; }
        char c = src[i];
        if (c == '<') {
            while (i < srclen && src[i] != '>') i++;
            if (i < srclen) i++;
            if (!lastsp && o < cap - 1) { out[o++] = ' '; lastsp = 1; }
            continue;
        }
        if (c == '&') {
            char dc; i += decode_entity(src + i, &dc);
            if (dc == ' ' || dc == '\t') { if (!lastsp) { out[o++]=' '; lastsp=1; } }
            else { out[o++] = dc; lastsp = 0; }
            continue;
        }
        if (c=='\r'||c=='\n'||c=='\t'||c==' ') { if (!lastsp){out[o++]=' ';lastsp=1;} i++; continue; }
        if ((unsigned char)c < 32 || (unsigned char)c > 126) { i++; continue; }
        out[o++] = c; lastsp = 0; i++;
    }
    while (o > 0 && out[o-1] == ' ') o--;
    out[o] = '\0';
}

// ---- Config persistence: /CONFIG/RSSFEEDS.CFG (one "name\turl" per line) ---
#define CFG_PATH "/CONFIG/RSSFEEDS.CFG"

static void seed_default_feeds(void) {
    g_feed_count = 0;
    for (int i = 0; i < N_DEFAULT && g_feed_count < MAX_FEEDS; i++)
        g_feeds[g_feed_count++] = DEFAULT_FEEDS[i];
}

static void save_feeds(void) {
    sys_mkdir("/CONFIG", 0755);
    int fd = sys_open(CFG_PATH, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    char line[300];
    for (int i = 0; i < g_feed_count; i++) {
        int n = snprintf(line, sizeof(line), "%s\t%s\n", g_feeds[i].name, g_feeds[i].url);
        sys_write(fd, line, n);
    }
    sys_close(fd);
}

static void load_feeds(void) {
    int fd = sys_open(CFG_PATH, O_RDONLY);
    if (fd < 0) { seed_default_feeds(); save_feeds(); return; }
    static char buf[8192];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n <= 0) { seed_default_feeds(); save_feeds(); return; }
    buf[n] = '\0';
    g_feed_count = 0;
    int i = 0;
    while (buf[i] && g_feed_count < MAX_FEEDS) {
        char line[300]; int l = 0;
        while (buf[i] && buf[i] != '\n' && buf[i] != '\r') {
            if (l < (int)sizeof(line) - 1) line[l++] = buf[i];
            i++;
        }
        while (buf[i] == '\n' || buf[i] == '\r') i++;
        line[l] = '\0';
        if (!line[0] || line[0] == '#') continue;
        // split on first tab (or space); if no separator, url only.
        char *tab = strstr(line, "\t");
        feed_t *f = &g_feeds[g_feed_count];
        if (tab) {
            *tab = '\0';
            scpy(f->name, line, sizeof(f->name));
            scpy(f->url, tab + 1, sizeof(f->url));
        } else {
            scpy(f->url, line, sizeof(f->url));
            scpy(f->name, line, sizeof(f->name));
        }
        if (f->url[0]) g_feed_count++;
    }
    if (g_feed_count == 0) { seed_default_feeds(); save_feeds(); }
}

// Derive a short display name from a URL host.
static void name_from_url(const char *url, char *out, int cap) {
    const char *h = strstr(url, "://");
    h = h ? h + 3 : url;
    if (strncmp(h, "www.", 4) == 0) h += 4;
    int i = 0;
    while (h[i] && h[i] != '/' && h[i] != ':' && i < cap - 1) { out[i] = h[i]; i++; }
    out[i] = '\0';
    if (!out[0]) scpy(out, "feed", cap);
}

// ---- Parse fetched bytes via the Rust core --------------------------------
static void parse_feed(const char *doc, int len) {
    g_item_count = 0; g_sel_item = -1; g_list_scroll = 0; g_reader_scroll = 0;
    g_feed_title[0] = '\0'; g_feed_format = RSS_FMT_UNKNOWN;

    rss_feed_t *fd = rss_parse((const unsigned char *)doc, (unsigned long)len);
    if (!fd) return;
    g_feed_format = fd->format;
    if (fd->title && fd->title[0]) clean_text(fd->title, g_feed_title, sizeof(g_feed_title));

    int n = fd->item_count;
    if (n > MAX_ITEMS) n = MAX_ITEMS;
    for (int i = 0; i < n; i++) {
        rss_item_t *s = &fd->items[i];
        item_t *d = &g_items[g_item_count];
        if (s->title && s->title[0]) clean_text(s->title, d->title, TITLE_MAX);
        else scpy(d->title, "(untitled)", TITLE_MAX);
        scpy(d->link, s->link ? s->link : "", LINK_MAX);
        clean_text(s->pub_date ? s->pub_date : "", d->date, DATE_MAX);
        clean_text(s->author ? s->author : "", d->author, AUTHOR_MAX);
        clean_text(s->description ? s->description : "", d->desc, DESC_MAX);
        // The image URL is a URL, not prose: copy it verbatim (clean_text would
        // mangle it). alt/title ARE prose and may carry entities/markup.
        scpy(d->image_url, s->image_url ? s->image_url : "", LINK_MAX);
        clean_text(s->image_alt ? s->image_alt : "", d->image_alt, IMGTXT_MAX);
        clean_text(s->image_title ? s->image_title : "", d->image_title, IMGTXT_MAX);
        g_item_count++;
    }
    rss_free(fd);
    if (g_item_count > 0) g_sel_item = 0;
}

// ---- Filtered views (search) ----------------------------------------------
static void rebuild_views(void) {
    g_feed_view_n = 0;
    for (int i = 0; i < g_feed_count; i++)
        if (ci_contains(g_feeds[i].name, g_search) || ci_contains(g_feeds[i].url, g_search))
            g_feed_view[g_feed_view_n++] = i;
    g_item_view_n = 0;
    for (int i = 0; i < g_item_count; i++)
        if (ci_contains(g_items[i].title, g_search) || ci_contains(g_items[i].desc, g_search))
            g_item_view[g_item_view_n++] = i;
    // keep selection valid within the filtered item view
    if (g_sel_item >= 0) {
        int found = 0;
        for (int i = 0; i < g_item_view_n; i++) if (g_item_view[i] == g_sel_item) { found = 1; break; }
        if (!found) g_sel_item = (g_item_view_n > 0) ? g_item_view[0] : -1;
    }
}

// ---- Fetch driving ---------------------------------------------------------
static void start_fetch(void) {
    if (g_job >= 0) { http_fetch_cancel(g_job); g_job = -1; }
    g_show_summary = 0;
    if (g_cur_feed < 0 || g_cur_feed >= g_feed_count) return;
    if (!sys_net_is_up()) {
        // Not an error yet: wait for DHCP rather than latching a dead state.
        g_state = ST_WAIT_NET; g_net_wait_frames = 0;
        scpy(g_status_msg, "Waiting for network ...", sizeof(g_status_msg));
        return;
    }
    g_job = http_fetch_start(g_feeds[g_cur_feed].url);
    if (g_job < 0) { g_state = ST_ERROR; scpy(g_status_msg, "Could not start fetch.", sizeof(g_status_msg)); return; }
    g_state = ST_FETCHING; g_poll_frames = 0;
    snprintf(g_status_msg, sizeof(g_status_msg), "Fetching %s ...", g_feeds[g_cur_feed].name);
}

// Drive the ST_WAIT_NET state: re-arm the fetch as soon as the stack is up.
// This is an event-loop poll of a kernel status flag, not a blocking wait: the
// loop is already running at ~20Hz for input, so it adds no new wait primitive
// and never blocks the UI.
static void poll_net_wait(void) {
    if (g_state != ST_WAIT_NET) return;
    if (sys_net_is_up()) {
        scpy(g_status_msg, "Network up, fetching ...", sizeof(g_status_msg));
        start_fetch();
        return;
    }
    if (++g_net_wait_frames > NET_WAIT_MAX_FRAMES) {
        g_state = ST_ERROR;
        scpy(g_status_msg, "Network is down (no DHCP lease). Press Refresh to retry.",
             sizeof(g_status_msg));
    }
}

static void poll_fetch(void) {
    if (g_state != ST_FETCHING || g_job < 0) return;
    int status = 0; unsigned int len = 0;
    int st = http_fetch_poll(g_job, &status, &len);
    if (st == 0) {
        if (++g_poll_frames > FETCH_TIMEOUT_FRAMES) {
            http_fetch_cancel(g_job); g_job = -1;
            g_state = ST_ERROR; scpy(g_status_msg, "Fetch timed out.", sizeof(g_status_msg));
        }
        return;
    }
    if (st == 1) {
        int n = http_fetch_read(g_job, g_fetch, FETCH_MAX - 1);
        g_job = -1;
        if (n <= 0 || (status != 200 && status != 0)) {
            g_state = ST_ERROR;
            snprintf(g_status_msg, sizeof(g_status_msg), "HTTP error (status %d).", status);
            return;
        }
        parse_feed(g_fetch, n);
        rebuild_views();
        if (g_item_count > 0) {
            g_state = ST_READY;
            snprintf(g_status_msg, sizeof(g_status_msg), "%s  -  %d items  [%s]",
                     g_feed_title[0] ? g_feed_title : g_feeds[g_cur_feed].name,
                     g_item_count, rss_format_name(g_feed_format));
        } else {
            g_state = ST_ERROR;
            scpy(g_status_msg, "No items (not a recognised RSS/Atom/RDF feed?).", sizeof(g_status_msg));
        }
    } else {
        http_fetch_read(g_job, g_fetch, 0); g_job = -1;
        g_state = ST_ERROR; scpy(g_status_msg, "Fetch failed.", sizeof(g_status_msg));
    }
}

// ---- AI summarize ----------------------------------------------------------
static void *ai_worker(void *arg) {
    (void)arg;
    static char out[6000];
    int rc = aiclient_ask(g_ai_prompt, out, sizeof(out), 0);
    if (rc == 0 && out[0]) { scpy(g_ai_result, out, sizeof(g_ai_result)); g_ai_state = AI_DONE; }
    else { scpy(g_ai_result, out[0] ? out : "AI request failed.", sizeof(g_ai_result)); g_ai_state = AI_ERROR; }
    return 0;
}
static void start_summarize(void) {
    if (g_ai_state == AI_RUNNING) return;
    if (g_sel_item < 0 || g_sel_item >= g_item_count) return;
    if (!aiclient_have_key()) {
        g_show_summary = 1; g_ai_state = AI_ERROR;
        scpy(g_ai_result, "No AI key found at /CONFIG/KIMI.KEY. Add your Moonshot key to enable summaries.",
             sizeof(g_ai_result));
        return;
    }
    item_t *it = &g_items[g_sel_item];
    snprintf(g_ai_prompt, sizeof(g_ai_prompt),
             "Summarize this news article in 3 to 4 short sentences, plainly, no preamble.\n\n"
             "Title: %s\n\n%s", it->title, it->desc[0] ? it->desc : "(no body text available)");
    g_show_summary = 1; g_reader_scroll = 0; g_ai_state = AI_RUNNING;
    scpy(g_ai_result, "Summarizing ...", sizeof(g_ai_result));
    if (pthread_create(&g_ai_thread, 0, ai_worker, 0) != 0) {
        g_ai_state = AI_ERROR; scpy(g_ai_result, "Could not start AI worker.", sizeof(g_ai_result));
    } else {
        pthread_detach(g_ai_thread);
    }
}

// ---- Toolbar buttons -------------------------------------------------------
enum { BTN_ADD, BTN_DEL, BTN_UP, BTN_DOWN, BTN_REFRESH, BTN_AI,
       BTN_ARTICLE, BTN_SIZE_DN, BTN_SIZE_UP, BTN_FONT, BTN_COUNT };
static const char *BTN_LABEL[BTN_COUNT] = {
    "+ Add", "Del", "Up", "Dn", "Refresh", "Summarize",
    "Article", "A-", "A+", "Font"
};
typedef struct { int x, y, w, h; } rect_t;
static rect_t g_btn[BTN_COUNT];
static rect_t g_search_box;

// Chrome text is proportional TTF now, so measure it with the shared metric
// helper rather than assuming a fixed 8px cell (which mis-sized every button
// and put the search caret in the wrong place).
static int text_px(const char *s){ return gui_ttf_width(s, 14); }
static int text_px_small(const char *s){ return gui_ttf_width(s, 11); }

// Trim s in place until it fits max_px, appending an ellipsis. Proportional TTF
// has no fixed cell, so the old strlen-based cut either clipped mid-glyph or
// truncated far too early depending on the string.
static void ellipsize(char *s, int cap, int max_px) {
    if (max_px <= 0 || cap < 5) return;
    if (text_px(s) <= max_px) return;
    // Longest prefix p such that p + "..." fits. Needs room for 3 dots + NUL.
    int n = (int)strlen(s);
    if (n > cap - 4) n = cap - 4;
    char probe[400];
    while (n > 0) {
        if (n > (int)sizeof(probe) - 4) { n--; continue; }
        for (int k = 0; k < n; k++) probe[k] = s[k];
        probe[n] = '.'; probe[n+1] = '.'; probe[n+2] = '.'; probe[n+3] = '\0';
        if (text_px(probe) <= max_px) break;
        n--;
    }
    if (n <= 0) { s[0] = '\0'; return; }
    s[n] = '.'; s[n+1] = '.'; s[n+2] = '.'; s[n+3] = '\0';
}

static void layout_buttons(void) {
    int x = 6, y = 6, h = TOOLBAR_H - 12;
    for (int i = 0; i < BTN_COUNT; i++) {
        int w = text_px(BTN_LABEL[i]) + 14;
        g_btn[i].x = x; g_btn[i].y = y; g_btn[i].w = w; g_btn[i].h = h;
        x += w + 5;
        if (i == BTN_AI) x += 10;   // small gap before reader controls group
    }
    // search box occupies the right end of the toolbar
    int sw = 200;
    g_search_box.x = g_w - sw - 8; g_search_box.y = 6; g_search_box.w = sw; g_search_box.h = h;
    if (g_search_box.x < x + 8) { g_search_box.x = x + 8; g_search_box.w = g_w - g_search_box.x - 8; }
    if (g_search_box.w < 80) g_search_box.w = 80;
}

// ---- Layout (panes) --------------------------------------------------------
static int list_x, list_y, list_w, list_h;
static int det_x, det_y, det_w, det_h;
static int side_x, side_y, side_w, side_h;

static void recompute_layout(void) {
    win_get_size(win, &g_w, &g_h);
    if (g_w < 640) g_w = 640;
    if (g_h < 380) g_h = 380;
    int body_y = TOOLBAR_H;
    int body_h = g_h - TOOLBAR_H - STATUS_H;

    side_x = 0; side_y = body_y; side_w = SIDEBAR_W; side_h = body_h;

    int listw = (g_w - SIDEBAR_W) * 34 / 100;
    if (listw < 220) listw = 220;
    if (listw > 380) listw = 380;
    list_x = SIDEBAR_W; list_y = body_y; list_w = listw; list_h = body_h;

    det_x = SIDEBAR_W + listw; det_y = body_y;
    det_w = g_w - det_x; det_h = body_h;
    layout_buttons();
}

static int list_rows(void){ int r=(list_h-28)/ROW_H; return r<1?1:r; }

static unsigned int rd_be32(const unsigned char *p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8) | (unsigned int)p[3];
}
static unsigned int rd_be16(const unsigned char *p) {
    return ((unsigned int)p[0] << 8) | (unsigned int)p[1];
}
static unsigned int rd_le32(const unsigned char *p) {
    return ((unsigned int)p[3] << 24) | ((unsigned int)p[2] << 16) |
           ((unsigned int)p[1] << 8) | (unsigned int)p[0];
}

// Identify the format and read the DECLARED dimensions WITHOUT decoding.
// Returns 0 and fills w/h, or -1 if this is not something the kernel decodes.
static int img_probe(const unsigned char *d, int n, int *w, int *h, const char **fmt) {
    if (n < 26) return -1;
    // PNG: 8-byte signature, then the IHDR chunk (type at 12, dims at 16/20).
    if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G' &&
        d[4] == 0x0D && d[5] == 0x0A && d[6] == 0x1A && d[7] == 0x0A) {
        if (d[12] != 'I' || d[13] != 'H' || d[14] != 'D' || d[15] != 'R') return -1;
        *w = (int)(rd_be32(d + 16) & 0x7FFFFFFFu);
        *h = (int)(rd_be32(d + 20) & 0x7FFFFFFFu);
        *fmt = "PNG";
        return 0;
    }
    // BMP: 'BM', dims in the info header. A negative height means top-down.
    if (d[0] == 'B' && d[1] == 'M') {
        int bw = (int)rd_le32(d + 18), bh = (int)rd_le32(d + 22);
        if (bh < 0) bh = -bh;
        *w = bw; *h = bh;
        *fmt = "BMP";
        return 0;
    }
    // JPEG: walk the marker chain to the frame header (SOFn) and read its dims.
    if (d[0] == 0xFF && d[1] == 0xD8) {
        int i = 2;
        while (i + 3 < n) {
            if (d[i] != 0xFF) { i++; continue; }              // resync
            unsigned char m = d[i + 1];
            if (m == 0xFF) { i++; continue; }                 // fill byte
            if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) { i += 2; continue; } // no payload
            if (m == 0xD9 || m == 0xDA) break;                // EOI / scan: no SOF
            int seglen = (int)rd_be16(d + i + 2);
            if (seglen < 2) return -1;
            // SOF0..SOF15, excluding DHT (C4), JPGA (C8) and DAC (CC).
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                if (i + 8 >= n) return -1;
                *h = (int)rd_be16(d + i + 5);
                *w = (int)rd_be16(d + i + 7);
                *fmt = "JPEG";
                return 0;
            }
            i += 2 + seglen;
        }
        return -1;
    }
    return -1;
}

static int url_is_http(const char *u) {
    return strncmp(u, "http://", 7) == 0 || strncmp(u, "https://", 8) == 0;
}

// True if the URL's host is a LITERAL private/loopback address.
//
// This is a partial control and worth being precise about: the kernel's SSRF
// gate in net/wget.c only covers REDIRECTS (wget_redirect_allowed), so the
// FIRST hop of any fetch is ungated by design, to keep Home Assistant on
// 192.168.x.x working. A hostile feed carrying <img src="http://<private-ip>/">
// would otherwise be fetched with no click at all. This blocks the literal
// form; a HOSTNAME that resolves to a private address is NOT blocked here,
// because Ring 3 cannot see the resolution. Closing that needs a first-hop
// origin check in wget.c.
static int url_host_is_private_literal(const char *u) {
    const char *h = strstr(u, "://");
    if (!h) return 0;
    h += 3;
    char host[128];
    int i = 0;
    while (h[i] && h[i] != '/' && h[i] != ':' && h[i] != '?' &&
           i < (int)sizeof(host) - 1) { host[i] = lc(h[i]); i++; }
    host[i] = '\0';
    if (strcmp(host, "localhost") == 0) return 1;
    // IPv6 literal: the stack is IPv4-only, so nothing legitimate arrives this
    // way; refuse rather than try to classify ::1 / fc00::/7 here.
    if (host[0] == '[') return 1;
    int o[4], k = 0, v = 0, dig = 0;
    for (int j = 0; ; j++) {
        char c = host[j];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            dig = 1;
            if (v > 255) return 0;
        } else if (c == '.' || c == '\0') {
            if (!dig || k >= 4) return 0;
            o[k++] = v; v = 0; dig = 0;
            if (c == '\0') break;
        } else {
            return 0;   // a name, not a dotted quad: cannot classify it here
        }
    }
    if (k != 4) return 0;
    if (o[0] == 127 || o[0] == 10 || o[0] == 0) return 1;
    if (o[0] == 192 && o[1] == 168) return 1;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return 1;
    if (o[0] == 169 && o[1] == 254) return 1;
    return 0;
}

// Resolve a possibly-relative feed image URL against the feed's own URL, and
// gate the scheme. Returns 0 with an absolute http/https URL in `out`, else -1.
static int resolve_img_url(const char *src, const char *base, char *out, int cap) {
    if (!src || !src[0] || !base) return -1;
    char tmp[LINK_MAX * 2];
    if (strncmp(src, "//", 2) == 0) {
        // protocol-relative: inherit the feed's scheme
        snprintf(tmp, sizeof(tmp), "%s%s",
                 strncmp(base, "http://", 7) == 0 ? "http:" : "https:", src);
    } else if (url_is_http(src)) {
        scpy(tmp, src, sizeof(tmp));
    } else if (src[0] == '/') {
        // root-relative: scheme://host of the feed
        const char *h = strstr(base, "://");
        if (!h) return -1;
        const char *slash = strchr(h + 3, '/');
        int hostlen = slash ? (int)(slash - base) : (int)strlen(base);
        if (hostlen <= 0 || hostlen >= (int)sizeof(tmp)) return -1;
        int k = 0;
        for (; k < hostlen; k++) tmp[k] = base[k];
        tmp[k] = '\0';
        int l = (int)strlen(tmp);
        snprintf(tmp + l, sizeof(tmp) - l, "%s", src);
    } else {
        // Anything carrying a scheme we do not fetch (data:, file:, javascript:)
        // is REFUSED, not pasted onto the base URL. The browser pastes, which
        // only works by accident: it produces a nonsense URL that happens to
        // fail at fetch time.
        for (int k = 0; src[k] && src[k] != '/' && k < 12; k++)
            if (src[k] == ':') return -1;
        char dir[LINK_MAX];
        scpy(dir, base, sizeof(dir));
        char *q = strchr(dir, '?');
        if (q) *q = '\0';
        int l = (int)strlen(dir);
        while (l > 0 && dir[l - 1] != '/') l--;
        if (l <= 0) return -1;
        dir[l] = '\0';
        snprintf(tmp, sizeof(tmp), "%s%s", dir, src);
    }
    if (!url_is_http(tmp)) return -1;
    if (url_host_is_private_literal(tmp)) return -1;
    if ((int)strlen(tmp) >= cap) return -1;
    scpy(out, tmp, cap);
    return 0;
}

// ---- Image cache -----------------------------------------------------------
static imgent_t *img_find(const char *url) {
    for (int i = 0; i < IMG_CACHE_N; i++)
        if (g_imgc[i].state != IC_FREE && strcmp(g_imgc[i].url, url) == 0) {
            g_imgc[i].lru = ++g_lru_clock;
            return &g_imgc[i];
        }
    return 0;
}
static void img_release(imgent_t *e) {
    if (e->raw) { free(e->raw); e->raw = 0; }
    if (e->px)  { free(e->px);  e->px = 0; }
    e->raw_len = 0; e->px_w = e->px_h = 0; e->box_w = e->box_h = 0;
    e->src_w = e->src_h = 0; e->err = 0; e->errbuf[0] = '\0'; e->fmt = 0;
    e->url[0] = '\0'; e->state = IC_FREE;
}
static int img_bytes_used(void) {
    int t = 0;
    for (int i = 0; i < IMG_CACHE_N; i++)
        t += g_imgc[i].raw_len + g_imgc[i].px_w * g_imgc[i].px_h * 4;
    return t;
}
// Evict least-recently-used entries until a slot is free AND we are under
// budget. Never evicts one that is mid-fetch (a job holds a pointer to it).
static void img_trim(void) {
    for (;;) {
        int have_slot = 0;
        for (int i = 0; i < IMG_CACHE_N; i++)
            if (g_imgc[i].state == IC_FREE) { have_slot = 1; break; }
        if (have_slot && img_bytes_used() <= IMG_BUDGET) return;
        imgent_t *v = 0;
        for (int i = 0; i < IMG_CACHE_N; i++) {
            imgent_t *e = &g_imgc[i];
            if (e->state == IC_FREE || e->state == IC_FETCHING) continue;
            if (!v || e->lru < v->lru) v = e;
        }
        if (!v) return;   // everything is pinned: nothing to give back
        img_release(v);
    }
}
static imgent_t *img_intern(const char *url) {
    imgent_t *e = img_find(url);
    if (e) return e;
    img_trim();
    for (int i = 0; i < IMG_CACHE_N; i++) {
        if (g_imgc[i].state != IC_FREE) continue;
        e = &g_imgc[i];
        scpy(e->url, url, sizeof(e->url));
        e->state = IC_WANT;
        e->raw = 0; e->raw_len = 0; e->px = 0; e->px_w = e->px_h = 0;
        e->box_w = e->box_h = 0; e->src_w = e->src_h = 0; e->err = 0;
        e->lru = ++g_lru_clock;
        return e;
    }
    return 0;
}

// The image box for the reader: pane width scaled by the reader zoom, so the
// picture tracks the text. The kernel scaler only ever scales DOWN, so a small
// feed thumbnail renders at its own size rather than being blown up and blocky.
static void img_box(int *bw, int *bh) {
    int pane = det_w - 28;
    if (pane < 64) pane = 64;
    int w = pane * g_reader_size / 15;
    if (w > pane) w = pane;
    if (w > IMG_BOX_MAX_W) w = IMG_BOX_MAX_W;
    if (w < 32) w = 32;
    *bw = w;
    *bh = IMG_BOX_MAX_H;
}

// Decode cached bytes into the current box. Only runs when the box actually
// changes (selection, zoom, resize), never per frame.
static void img_decode(imgent_t *e, int bw, int bh) {
    if (e->px) { free(e->px); e->px = 0; e->px_w = e->px_h = 0; }
    e->box_w = bw; e->box_h = bh;
    // Size the buffer to the EXACT output, by mirroring the kernel's fit math
    // on the dimensions img_probe already read out of the header. Allocating
    // bw*bh*4 instead would ask for 2.2MB to hold a 240x134 thumbnail, and
    // out_cap is not a safety net: when the result does not fit, the kernel
    // CROPS rows off the bottom rather than failing, so an over-small guess
    // silently yields half an image.
    int dw = e->src_w, dh = e->src_h;
    if (dw > bw) { dh = (int)((long)dh * bw / dw); dw = bw; }
    if (dh > bh) { dw = (int)((long)dw * bh / dh); dh = bh; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    // Slack: a JPEG's decoded size can round up to the 16px MCU grid, so the
    // kernel's own dw/dh may exceed what the header implies. Over-allocating a
    // little is free; under-allocating costs rows off the bottom of the image.
    unsigned int cap = (unsigned int)(dw + 16) * (unsigned int)(dh + 16) * 4u;
    unsigned int *px = (unsigned int *)malloc(cap);
    if (!px) { e->state = IC_FAILED; e->err = "Out of memory for image."; return; }
    // Fault every page of the destination in from Ring 3 BEFORE the kernel
    // writes it. sys_decode_image writes this buffer from Ring 0, and a fresh
    // malloc is only mmap-reserved: the pages are not backed until first touch.
    // Without this, only the already-backed first page of the buffer survives
    // the decode and the rest of the image comes back black. Proven on a
    // 160x80 PNG: exactly 6 rows landed, and 4096/(160*4) = 6.4.
    memset(px, 0, cap);
    int dims[2] = { 0, 0 };
    int rc = decode_image(e->raw, (unsigned int)e->raw_len, bw, bh, px, cap, dims);
    if (rc <= 0 || dims[0] <= 0 || dims[1] <= 0) {
        free(px);
        e->state = IC_FAILED;
        // SYS_DECODE_IMAGE collapses every kernel-side reason (PNG_ERR_CRC,
        // PNG_ERR_INFLATE, PNG_ERR_NOMEM, unsupported subformat...) into a bare
        // -1, so the app cannot say WHICH. Report what we do know: the format we
        // identified and its size, which is what a bug report actually needs.
        // (Surfacing the real PNG_ERR_*/JPEG_ERR_* needs a kernel change.)
        snprintf(e->errbuf, sizeof(e->errbuf),
                 "Could not decode this %s (%dx%d, %d KB).",
                 e->fmt, e->src_w, e->src_h, e->raw_len / 1024);
        e->err = e->errbuf;
        return;
    }
    e->px = px; e->px_w = dims[0]; e->px_h = dims[1];
}

// ---- Inline image fetch driving -------------------------------------------
// The URL the reader currently needs, resolved and gated. Returns 0 on success.
// Pure: both poll_img() and draw_reader() call it, so they never disagree.
static int current_img_url(char *out, int cap) {
    if (g_show_summary) return -1;
    if (g_sel_item < 0 || g_sel_item >= g_item_count) return -1;
    if (g_cur_feed < 0 || g_cur_feed >= g_feed_count) return -1;
    item_t *it = &g_items[g_sel_item];
    if (!it->image_url[0]) return -1;
    return resolve_img_url(it->image_url, g_feeds[g_cur_feed].url, out, cap);
}

// Consume a finished image fetch. The job slot is released on EVERY path
// (http_fetch_read frees the kernel body and clears in_use, even with max=0).
static void img_fetch_finish(imgent_t *e, int st, int status, unsigned int len) {
    char dump;
    if (st != 1) {
        http_fetch_read(g_img_job, &dump, 0);
        e->state = IC_FAILED; e->err = "Image download failed.";
        return;
    }
    if (status != 200 && status != 0) {
        http_fetch_read(g_img_job, &dump, 0);
        e->state = IC_FAILED; e->err = "Image unavailable (server error).";
        return;
    }
    if (len == 0 || len > IMG_MAX_BYTES) {
        http_fetch_read(g_img_job, &dump, 0);
        e->state = IC_FAILED;
        e->err = len ? "Image too large to display." : "Image was empty.";
        return;
    }
    unsigned char *buf = (unsigned char *)malloc(len);
    if (!buf) {
        http_fetch_read(g_img_job, &dump, 0);
        e->state = IC_FAILED; e->err = "Out of memory for image.";
        return;
    }
    // Fault every page in from Ring 3 before the kernel memcpy's the body into
    // it. A fresh malloc is only mmap-reserved, and a Ring-0 write to a
    // not-yet-backed user page is LOST: http_fetch_read still returns the full
    // length, but only the first 4096 bytes actually arrive and the rest stay
    // zero. That silently truncates every image over one page, which the PNG
    // decoder then correctly rejects as a CRC/inflate error (a 63KB xkcd comic
    // decodes fine offline; it only failed here). Same reason as the memset in
    // img_decode, other direction. See blame.md.
    memset(buf, 0, len);
    int n = http_fetch_read(g_img_job, (char *)buf, len);
    if (n <= 0) {
        free(buf);
        e->state = IC_FAILED; e->err = "Image download failed.";
        return;
    }
    // Untrusted bytes. Identify and bound them BEFORE the Ring-0 decoder runs.
    int sw = 0, sh = 0;
    const char *fmt = "image";
    if (img_probe(buf, n, &sw, &sh, &fmt) != 0) {
        free(buf);
        e->state = IC_FAILED; e->err = "Unsupported image format (only BMP, PNG and JPEG).";
        return;
    }
    if (sw <= 0 || sh <= 0 || sw > IMG_MAX_DIM || sh > IMG_MAX_DIM ||
        (long)sw * (long)sh > IMG_MAX_PIXELS) {
        free(buf);
        e->state = IC_FAILED;
        snprintf(e->errbuf, sizeof(e->errbuf),
                 "Image refused: %dx%d exceeds the %dx%d limit.",
                 sw, sh, IMG_MAX_DIM, IMG_MAX_DIM);
        e->err = e->errbuf;
        return;
    }
    e->raw = buf; e->raw_len = n; e->src_w = sw; e->src_h = sh; e->fmt = fmt;
    e->state = IC_READY;   // decoded lazily, for whatever box the reader wants
}

// Drive image fetch + decode. Called once per event-loop pass, exactly like
// poll_fetch(): it polls a kernel job status and returns immediately, so the UI
// thread never blocks and no new wait primitive is introduced.
static void poll_img(void) {
    char url[LINK_MAX];
    int have = (current_img_url(url, sizeof(url)) == 0);

    // The reader moved on: drop an in-flight fetch nobody is waiting for, so the
    // next article's image is not stuck behind it.
    if (g_img_job >= 0 && g_img_slot >= 0) {
        if (!have || strcmp(url, g_imgc[g_img_slot].url) != 0) {
            http_fetch_cancel(g_img_job);
            img_release(&g_imgc[g_img_slot]);
            g_img_job = -1; g_img_slot = -1;
        }
    }

    // Finish the in-flight fetch.
    if (g_img_job >= 0 && g_img_slot >= 0) {
        imgent_t *e = &g_imgc[g_img_slot];
        int status = 0; unsigned int len = 0;
        int st = http_fetch_poll(g_img_job, &status, &len);
        if (st == 0) {
            if (++g_img_frames > FETCH_TIMEOUT_FRAMES) {
                http_fetch_cancel(g_img_job);
                e->state = IC_FAILED; e->err = "Image fetch timed out.";
                g_img_job = -1; g_img_slot = -1;
            }
            return;
        }
        img_fetch_finish(e, st, status, len);
        g_img_job = -1; g_img_slot = -1;
    }

    if (!have) { g_img_last[0] = '\0'; g_img_settle = 0; return; }

    // Debounce: only act once the selection has stopped moving.
    if (strcmp(url, g_img_last) != 0) {
        scpy(g_img_last, url, sizeof(g_img_last));
        g_img_settle = 0;
    } else if (g_img_settle < IMG_SETTLE_FRAMES) {
        g_img_settle++;
    }

    imgent_t *e = img_find(url);
    if (e && e->state == IC_READY) {
        // Already have the bytes: decode for the current box if it changed
        // (zoom/resize). No debounce, no network.
        int bw, bh; img_box(&bw, &bh);
        if (!e->px || e->box_w != bw || e->box_h != bh) img_decode(e, bw, bh);
        return;
    }
    if (g_img_settle < IMG_SETTLE_FRAMES) return;
    if (e && (e->state == IC_FAILED || e->state == IC_FETCHING)) return;  // no retry storm
    if (g_img_job >= 0) return;              // one image in flight at a time
    if (!sys_net_is_up()) return;            // poll_net_wait owns the status line

    if (!e) e = img_intern(url);
    if (!e || e->state != IC_WANT) return;
    int job = http_fetch_start(url);
    if (job < 0) return;   // the 6 async slots are shared OS-wide: retry, not an error
    g_img_job = job; g_img_slot = (int)(e - g_imgc);
    g_img_frames = 0; e->state = IC_FETCHING;
}


// ---- Drawing helpers -------------------------------------------------------
static int pt_in(rect_t r, int x, int y){ return x>=r.x && x<r.x+r.w && y>=r.y && y<r.y+r.h; }

// Toolbar button: the shared style-engine primitive, so it picks up the active
// renderer family (beveled on Classic, rounded/AA on modern themes) and the
// four control states, identical to Files and Settings. The old hand-rolled
// version drew its own bevel and hardcoded white ink on the active state, which
// was invisible against the pale selection fill of any light theme.
static void draw_button(rect_t r, const char *label, int enabled, int active) {
    gui_state_t st = !enabled ? GUI_ST_DISABLED
                   : active   ? GUI_ST_PRESSED
                              : GUI_ST_NORMAL;
    gui_button(win, r.x, r.y, r.w, r.h, label,
               active ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, st);
}

static void draw_toolbar(void) {
    win_draw_rect(win, 0, 0, g_w, TOOLBAR_H, TOOLBAR_BG);
    win_draw_rect(win, 0, TOOLBAR_H-1, g_w, 1, BORDER_COLOR);

    if (g_add_mode) {
        const char *lbl = "Add feed URL:";
        win_draw_text(win, 8, (TOOLBAR_H-CHAR_H)/2, lbl, ACCENT_TXT);
        int fx = 8 + text_px(lbl) + 8;
        const char *hint = "Enter/Esc";
        int hint_w = text_px_small(hint);
        int fw = g_w - fx - hint_w - 16;
        if (fw < 80) fw = 80;
        gui_textfield2(win, fx, 6, fw, TOOLBAR_H-12, "", 1);
        uint32_t field_ink = rss_fg(fp_field());
        // Scroll the URL horizontally by measured pixels so the caret stays in
        // view; a fixed-cell estimate drifted badly on long proportional URLs.
        int start = 0;
        {
            char pre[260]; int n = g_add_tf.cursor;
            if (n > (int)sizeof(pre)-1) n = (int)sizeof(pre)-1;
            for (int k = 0; k < n; k++) pre[k] = g_add_buf[k];
            pre[n] = '\0';
            while (start < n && text_px(pre + start) > fw - 12) start++;
        }
        win_draw_text(win, fx+5, (TOOLBAR_H-CHAR_H)/2, g_add_buf + start, field_ink);
        {
            char pre[260]; int n = g_add_tf.cursor - start;
            if (n < 0) n = 0;
            if (n > (int)sizeof(pre)-1) n = (int)sizeof(pre)-1;
            for (int k = 0; k < n; k++) pre[k] = g_add_buf[start + k];
            pre[n] = '\0';
            int cx = fx + 5 + text_px(pre);
            if (cx < fx + fw - 2) win_draw_rect(win, cx, 9, 1, TOOLBAR_H-18, field_ink);
        }
        win_draw_text_small(win, g_w - hint_w - 8, (TOOLBAR_H-CHAR_H)/2 + 2, hint, DIM_COLOR);
        return;
    }

    int can_ai = (g_sel_item >= 0);
    for (int i = 0; i < BTN_COUNT; i++) {
        int active = 0, enabled = 1;
        if (i == BTN_AI) enabled = can_ai;
        if (i == BTN_ARTICLE) active = !g_show_summary;
        if (i == BTN_ARTICLE && !g_show_summary) active = 1;
        draw_button(g_btn[i], BTN_LABEL[i], enabled, active);
    }
    // Search box: shared style-engine field, so focus ring, fill and border all
    // match Files/Settings instead of being four hand-drawn 1px rects.
    rect_t s = g_search_box;
    gui_textfield2(win, s.x, s.y, s.w, s.h, "", g_focus == FOCUS_SEARCH);
    int ty = s.y + (s.h - CHAR_H)/2;
    uint32_t field_ink = rss_fg(fp_field());
    if (!g_search[0] && g_focus != FOCUS_SEARCH) {
        win_draw_text(win, s.x+6, ty, "Search feeds & articles", rss_dim(fp_field()));
    } else {
        win_draw_text(win, s.x+6, ty, g_search, field_ink);
        if (g_focus == FOCUS_SEARCH) {
            // Caret rides on the measured pixel width of the text before the
            // cursor: a fixed cell width put it in the wrong place under TTF.
            char pre[80]; int n = g_search_tf.cursor;
            if (n > (int)sizeof(pre)-1) n = (int)sizeof(pre)-1;
            for (int k = 0; k < n; k++) pre[k] = g_search[k];
            pre[n] = '\0';
            int cx = s.x + 6 + text_px(pre);
            if (cx < s.x + s.w - 2) win_draw_rect(win, cx, s.y+4, 1, s.h-8, field_ink);
        }
    }
}

static void draw_sidebar(void) {
    win_draw_rect(win, side_x, side_y, side_w, side_h, FEEDS_BG);
    win_draw_rect(win, side_x+side_w-1, side_y, 1, side_h, BORDER_COLOR);
    win_draw_text_small(win, side_x+8, side_y+8, "FEEDS", rss_dim(FEEDS_BG));
    win_draw_rect(win, side_x, side_y+CHAR_H+8, side_w-1, 1, BORDER_COLOR);
    int y0 = side_y + CHAR_H + 12;
    for (int v = 0; v < g_feed_view_n; v++) {
        int i = g_feed_view[v];
        int ry = y0 + v * ROW_H;
        if (ry + ROW_H > side_y + side_h) break;
        if (i == g_cur_feed) win_draw_rect(win, side_x, ry, side_w-1, ROW_H, SEL_BG);
        // Ink is derived from whatever it actually sits on, so the selected row
        // stays readable on light themes (a hardcoded white was invisible on the
        // pale selection fill) and on dark ones alike.
        uint32_t col = (i == g_cur_feed) ? rss_fg(SEL_BG) : rss_fg(FEEDS_BG);
        char nm[38]; scpy(nm, g_feeds[i].name, sizeof(nm));
        ellipsize(nm, sizeof(nm), side_w - 16);
        win_draw_text(win, side_x+8, ry+2, nm, col);
    }
    // count line
    char c[40]; snprintf(c, sizeof(c), "%d feed%s", g_feed_count, g_feed_count==1?"":"s");
    win_draw_text_small(win, side_x+8, side_y+side_h-CHAR_H-2, c, rss_dim(FEEDS_BG));
}

static void draw_list(void) {
    win_draw_rect(win, list_x, list_y, list_w, list_h, LIST_BG);
    win_draw_rect(win, list_x+list_w-1, list_y, 1, list_h, BORDER_COLOR);
    // header
    char hdr[100];
    if (g_search[0]) snprintf(hdr, sizeof(hdr), "ARTICLES  (%d match)", g_item_view_n);
    else snprintf(hdr, sizeof(hdr), "ARTICLES  (%d)", g_item_view_n);
    win_draw_text_small(win, list_x+8, list_y+8, hdr, rss_dim(LIST_BG));
    win_draw_rect(win, list_x, list_y+CHAR_H+8, list_w-1, 1, BORDER_COLOR);
    int y0 = list_y + CHAR_H + 12;
    int rows = list_rows();

    if (g_item_view_n == 0) {
        const char *m = (g_state==ST_FETCHING) ? "Loading ..." :
                        (g_state==ST_WAIT_NET) ? "Waiting for the network ..." :
                        (g_search[0] ? "No articles match your search." :
                                       "No articles. Pick a feed on the left.");
        win_draw_text(win, list_x+10, y0, m, DIM_COLOR);
        return;
    }
    if (g_list_scroll > g_item_view_n - rows) g_list_scroll = g_item_view_n - rows;
    if (g_list_scroll < 0) g_list_scroll = 0;

    for (int r = 0; r < rows; r++) {
        int v = g_list_scroll + r;
        if (v >= g_item_view_n) break;
        int idx = g_item_view[v];
        int ry = y0 + r * ROW_H;
        if (idx == g_sel_item) win_draw_rect(win, list_x, ry, list_w-1, ROW_H, SEL_BG);
        uint32_t col = (idx == g_sel_item) ? rss_fg(SEL_BG) : rss_fg(LIST_BG);
        char row[300];
        scpy(row, g_items[idx].title, sizeof(row));
        ellipsize(row, sizeof(row), list_w - 18);
        win_draw_text(win, list_x+8, ry+2, row, col);
    }
    // scrollbar
    if (g_item_view_n > rows) {
        int track = list_h - (CHAR_H+16);
        int th = track * rows / g_item_view_n; if (th < 14) th = 14;
        int ty = y0 + (track - th) * g_list_scroll / (g_item_view_n - rows);
        win_draw_rect(win, list_x+list_w-6, y0, 3, track, FEEDS_BG);
        win_draw_rect(win, list_x+list_w-6, ty, 3, th, BORDER_COLOR);
    }
}

// Approximate glyph metrics for the current reader typeface/size, cached.
// (The server libc exposes win_draw_text_ttf only, no font-metrics syscall, so
// we estimate a proportional advance from the point size for word-wrap. The
// kernel clips TTF drawing to the window, so a slightly generous estimate only
// wraps a touch early, never overflows.)
static int g_avg_adv = 9;
static int g_line_h = 20;
static void measure_reader_font(void) {
    if (g_reader_font == RF_MONO) {
        g_avg_adv = CHAR_W; g_line_h = CHAR_H + 2;
    } else {
        g_avg_adv = g_reader_size * 58 / 100; if (g_avg_adv < 4) g_avg_adv = 4;
        g_line_h = g_reader_size + g_reader_size / 4 + 3;
    }
}

// Draw a wrapped paragraph; returns the y after the last line drawn. Lines are
// clipped to [clip_top, clip_bot); the reader scroll shifts everything up. Uses
// antialiased TTF (Sans) or the 8x16 bitmap font (Mono) per g_reader_font.
static int draw_para_ttf(const char *s, int x, int w, int y, int clip_top,
                         int clip_bot, int size, uint32_t col) {
    int adv = (g_reader_font == RF_MONO) ? CHAR_W : (size * 58 / 100);
    if (adv < 4) adv = 4;
    int maxc = w / adv; if (maxc < 6) maxc = 6;
    int len = (int)strlen(s), pos = 0;
    while (pos < len) {
        int take = len - pos; if (take > maxc) take = maxc;
        if (pos + take < len) {
            int br = -1;
            for (int k = take; k > take/2; k--) if (s[pos+k-1] == ' ') { br = k; break; }
            if (br > 0) take = br;
        }
        char tmp[300]; int t = 0;
        for (int k = 0; k < take && t < (int)sizeof(tmp)-1; k++) tmp[t++] = s[pos+k];
        tmp[t] = '\0';
        if (y + g_line_h > clip_top && y < clip_bot) {
            if (g_reader_font == RF_MONO) rss_text_bitmap(win, x, y, tmp, col);
            else win_draw_text_ttf(win, x, y, tmp, size, col);
        }
        y += g_line_h;
        pos += take;
        while (pos < len && s[pos] == ' ') pos++;
    }
    return y;
}

// A bordered strip carrying a short message, used while an image loads and when
// it cannot be shown. Same fixed height either way, so the article text below
// does not jump when the picture lands.
static int draw_img_strip(int x, int w, int y, int clip_top, int clip_bot,
                          const char *msg) {
    int bw = w > IMG_BOX_MAX_W ? IMG_BOX_MAX_W : w;
    if (y + IMG_PLACE_H > clip_top && y < clip_bot) {
        win_draw_rect(win, x, y, bw, IMG_PLACE_H, fp_panel());
        gui_draw_rect_outline(win, x, y, bw, IMG_PLACE_H, BORDER_COLOR);
        win_draw_text(win, x + 10, y + (IMG_PLACE_H - CHAR_H) / 2, msg,
                      rss_dim(fp_panel()));
    }
    return y + IMG_PLACE_H + 8;
}

// Draw the selected article's inline image at y; returns the y below it.
//
// Rows outside [clip_top, clip_bot) are dropped by advancing into the pixel
// buffer rather than by skipping the blit: win_draw_image clips to the WINDOW,
// not to this pane, so an unclipped image would paint over the toolbar. (The
// browser sidesteps this by only drawing images that fit entirely on screen,
// which makes them pop in and out while scrolling. A reader scrolls through
// pictures constantly, so it is worth the few lines to clip properly.)
static int draw_reader_image(int x, int w, int y, int clip_top, int clip_bot) {
    char url[LINK_MAX];
    if (current_img_url(url, sizeof(url)) != 0) {
        // There IS an image, but we will not fetch it (bad scheme, private host).
        item_t *it = &g_items[g_sel_item];
        if (it->image_url[0])
            return draw_img_strip(x, w, y, clip_top, clip_bot,
                                  "Image skipped (address not allowed).");
        return y;
    }
    imgent_t *e = img_find(url);
    if (!e || e->state == IC_WANT || e->state == IC_FETCHING)
        return draw_img_strip(x, w, y, clip_top, clip_bot, "Loading image ...");
    if (e->state == IC_FAILED)
        return draw_img_strip(x, w, y, clip_top, clip_bot,
                              e->err ? e->err : "Image unavailable.");
    if (!e->px)
        return draw_img_strip(x, w, y, clip_top, clip_bot, "Decoding image ...");

    int iy = y, ih = e->px_h, skip = 0;
    if (iy < clip_top) { skip = clip_top - iy; ih -= skip; iy = clip_top; }
    if (iy + ih > clip_bot) ih = clip_bot - iy;
    if (ih > 0 && skip < e->px_h)
        win_draw_image(win, x, iy, e->px_w, ih,
                       e->px + (long)skip * (long)e->px_w);
    return y + e->px_h + 8;
}

static void draw_reader(void) {
    win_draw_rect(win, det_x, det_y, det_w, det_h, DETAIL_BG);
    int x = det_x + 14, w = det_w - 28;
    int header_h = 0;

    if (g_sel_item < 0 || g_sel_item >= g_item_count) {
        win_draw_text(win, det_x+14, det_y+14, "Select an article to read.", DIM_COLOR);
        return;
    }
    item_t *it = &g_items[g_sel_item];

    // Body region below a fixed header band.
    int hb_top = det_y;
    int content_top = det_y + 8 - g_reader_scroll;   // scrolls
    int clip_top = det_y, clip_bot = det_y + det_h;
    int y = content_top;

    // Title (larger TTF)
    y = draw_para_ttf(it->title, x, w, y, clip_top, clip_bot,
                      g_reader_size + 6, TITLE_COLOR);
    y += 4;
    // meta line: date + author
    char meta[200]; meta[0] = '\0';
    if (it->date[0]) scpy(meta, it->date, sizeof(meta));
    if (it->author[0]) {
        if (meta[0]) { int l=strlen(meta); snprintf(meta+l, sizeof(meta)-l, "   -   %s", it->author); }
        else scpy(meta, it->author, sizeof(meta));
    }
    if (meta[0]) { y = draw_para_ttf(meta, x, w, y, clip_top, clip_bot, g_reader_size-2<10?10:g_reader_size-2, DIM_COLOR); }
    if (it->link[0]) {
        char lk[LINK_MAX]; scpy(lk, it->link, sizeof(lk));
        y = draw_para_ttf(lk, x, w, y, clip_top, clip_bot, g_reader_size-2<10?10:g_reader_size-2, LINK_COLOR);
    }
    y += 6;
    // separator
    if (y > clip_top && y < clip_bot) win_draw_rect(win, x, y, w, 1, BORDER_COLOR);
    y += 8;

    // body: article text OR AI summary
    if (g_show_summary) {
        uint32_t hc = (g_ai_state==AI_ERROR)?ERR_COLOR:(g_ai_state==AI_DONE?OK_COLOR:ACCENT_TXT);
        const char *label = (g_ai_state==AI_RUNNING)?"AI SUMMARY  (working...)":"AI SUMMARY";
        y = draw_para_ttf(label, x, w, y, clip_top, clip_bot, g_reader_size, hc);
        y += 4;
        y = draw_para_ttf(g_ai_result, x, w, y, clip_top, clip_bot, g_reader_size, TEXT_COLOR);
    } else {
        // Picture first: for xkcd the comic IS the article, and for a news item
        // it is the lede. The strip keeps its place while it loads.
        if (it->image_url[0])
            y = draw_reader_image(x, w, y, clip_top, clip_bot);

        // Caption. xkcd hides the joke in the title attribute, so it is worth
        // real estate; alt is the accessible description and the fallback.
        const char *cap = it->image_title[0] ? it->image_title
                        : (it->image_alt[0] ? it->image_alt : 0);
        if (it->image_url[0] && cap) {
            int cs = g_reader_size - 2 < 10 ? 10 : g_reader_size - 2;
            y = draw_para_ttf(cap, x, w, y, clip_top, clip_bot, cs, DIM_COLOR);
            y += 6;
        }

        // With an image present, an empty description is normal (xkcd's whole
        // body is the img tag), so do not claim the article is empty.
        const char *body = it->desc[0] ? it->desc
                         : (it->image_url[0] ? "" : "(no article text in this feed item)");
        if (body[0])
            y = draw_para_ttf(body, x, w, y, clip_top, clip_bot, g_reader_size, TEXT_COLOR);
    }
    (void)hb_top; (void)header_h;

    // scroll hint / font info footer over the top band is unnecessary; the
    // status bar (drawn last) already masks any overflow at the bottom.
}

static void draw_status(void) {
    int y = g_h - STATUS_H;
    win_draw_rect(win, 0, y, g_w, STATUS_H, STATUS_BG);
    win_draw_rect(win, 0, y, g_w, 1, BORDER_COLOR);
    // Status inks are contrast-corrected against the toolbar surface, so the
    // error/fetching/ready states stay legible on light themes too.
    uint32_t col = rss_fg(STATUS_BG);
    if (g_state == ST_ERROR) col = ERR_COLOR;
    else if (g_state == ST_FETCHING || g_state == ST_WAIT_NET) col = ACCENT_TXT;
    else if (g_state == ST_READY) col = OK_COLOR;
    win_draw_text_small(win, 8, y + (STATUS_H-CHAR_H)/2 + 2, g_status_msg, col);
    // right side: reader font info
    char fi[80];
    if (g_reader_font == RF_MONO) snprintf(fi, sizeof(fi), "Font: %s", RF_NAME[g_reader_font]);
    else snprintf(fi, sizeof(fi), "Font: %s  %dpt", RF_NAME[g_reader_font], g_reader_size);
    win_draw_text_small(win, g_w - text_px_small(fi) - 8, y + (STATUS_H-CHAR_H)/2 + 2, fi,
                        rss_dim(STATUS_BG));
}

static void draw_all(void) {
    rss_apply_style();   // refresh the shared style engine from the live theme
    recompute_layout();
    win_draw_rect(win, 0, 0, g_w, g_h, BG_COLOR);
    draw_reader();      // draw first so top/bottom overflow is masked by chrome
    draw_sidebar();
    draw_list();
    draw_toolbar();
    draw_status();
}

// ---- Selection helpers -----------------------------------------------------
static void select_item_view(int v) {
    if (v < 0 || v >= g_item_view_n) return;
    g_sel_item = g_item_view[v];
    g_reader_scroll = 0; g_show_summary = 0;
    int rows = list_rows();
    if (v < g_list_scroll) g_list_scroll = v;
    if (v >= g_list_scroll + rows) g_list_scroll = v - rows + 1;
}
static int sel_view_index(void) {
    for (int i = 0; i < g_item_view_n; i++) if (g_item_view[i] == g_sel_item) return i;
    return -1;
}

// ---- Feed management -------------------------------------------------------
static void add_feed_submit(void) {
    g_add_mode = 0;
    if (!g_add_buf[0]) return;
    if (g_feed_count >= MAX_FEEDS) { scpy(g_status_msg, "Feed list is full.", sizeof(g_status_msg)); return; }
    feed_t *f = &g_feeds[g_feed_count];
    scpy(f->url, g_add_buf, sizeof(f->url));
    name_from_url(g_add_buf, f->name, sizeof(f->name));
    g_cur_feed = g_feed_count++;
    save_feeds();
    rebuild_views();
    start_fetch();
}
static void remove_current_feed(void) {
    if (g_feed_count <= 0 || g_cur_feed < 0 || g_cur_feed >= g_feed_count) return;
    for (int i = g_cur_feed; i < g_feed_count - 1; i++) g_feeds[i] = g_feeds[i+1];
    g_feed_count--;
    if (g_cur_feed >= g_feed_count) g_cur_feed = g_feed_count - 1;
    save_feeds();
    rebuild_views();
    g_item_count = 0; g_item_view_n = 0; g_sel_item = -1;
    if (g_feed_count > 0) start_fetch();
    else { g_state = ST_IDLE; scpy(g_status_msg, "No feeds. Use + Add to add one.", sizeof(g_status_msg)); }
}
static void move_feed(int dir) {
    int j = g_cur_feed + dir;
    if (g_cur_feed < 0 || j < 0 || j >= g_feed_count) return;
    feed_t t = g_feeds[g_cur_feed]; g_feeds[g_cur_feed] = g_feeds[j]; g_feeds[j] = t;
    g_cur_feed = j;
    save_feeds();
    rebuild_views();
}
static void change_size(int d) {
    g_reader_size += d;
    if (g_reader_size < 10) g_reader_size = 10;
    if (g_reader_size > 30) g_reader_size = 30;
    measure_reader_font();
}
static void cycle_font(void) {
    g_reader_font = (g_reader_font + 1) % RF_COUNT;
    measure_reader_font();
}

// ---- Input -----------------------------------------------------------------
static void handle_toolbar_click(int x, int y) {
    for (int i = 0; i < BTN_COUNT; i++) {
        if (pt_in(g_btn[i], x, y)) {
            switch (i) {
                case BTN_ADD: g_add_mode = 1; g_focus = FOCUS_LIST; tf_set_text(&g_add_tf, "https://"); break;
                case BTN_DEL: remove_current_feed(); break;
                case BTN_UP: move_feed(-1); break;
                case BTN_DOWN: move_feed(+1); break;
                case BTN_REFRESH: start_fetch(); break;
                case BTN_AI: start_summarize(); break;
                case BTN_ARTICLE: g_show_summary = 0; g_reader_scroll = 0; break;
                case BTN_SIZE_DN: change_size(-1); break;
                case BTN_SIZE_UP: change_size(+1); break;
                case BTN_FONT: cycle_font(); break;
            }
            return;
        }
    }
    if (pt_in(g_search_box, x, y)) { g_focus = FOCUS_SEARCH; return; }
    g_focus = FOCUS_LIST;
}

static void handle_click(int x, int y) {
    if (y < TOOLBAR_H) { handle_toolbar_click(x, y); return; }
    g_focus = FOCUS_LIST;
    // sidebar
    if (x < SIDEBAR_W && y >= side_y) {
        int y0 = side_y + CHAR_H + 12;
        if (y >= y0) {
            int v = (y - y0) / ROW_H;
            if (v >= 0 && v < g_feed_view_n) { g_cur_feed = g_feed_view[v]; start_fetch(); }
        }
        return;
    }
    // article list
    if (x >= list_x && x < list_x + list_w && y >= list_y) {
        int y0 = list_y + CHAR_H + 12;
        if (y >= y0) {
            int r = (y - y0) / ROW_H;
            int v = g_list_scroll + r;
            if (v >= 0 && v < g_item_view_n) select_item_view(v);
        }
        return;
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    win = win_create("Feeds - RSS Reader", 50, 34, WIN_W, WIN_H);
    if (win < 0) return 1;

    // FFI sizeof-lock (the kernel's _Static_assert discipline, done at runtime
    // because the two definitions live in different compilers). rss_rs.h and
    // rss_rs.rs declare rss_item_t/CItem independently; if they ever drift,
    // every string pointer past the drift is garbage, which would surface as
    // impossible-looking corruption rather than an obvious failure. Refuse to
    // parse instead.
    if (rss_abi_item_size() != (unsigned int)sizeof(rss_item_t)) {
        g_state = ST_ERROR;
        snprintf(g_status_msg, sizeof(g_status_msg),
                 "Internal error: feed parser ABI mismatch (C=%u Rust=%u).",
                 (unsigned int)sizeof(rss_item_t), rss_abi_item_size());
        draw_all();
        win_invalidate(win);
        for (;;) {
            gui_event_t ev;
            if (win_get_event(win, &ev, 200) > 0 && ev.type == EVENT_WINDOW_CLOSE) break;
        }
        win_destroy(win);
        return 1;
    }

    g_theme_last = theme_get_active();
    tf_init(&g_add_tf, g_add_buf, sizeof(g_add_buf));
    tf_init(&g_search_tf, g_search, sizeof(g_search));
    aiclient_init();            // loads /CONFIG/KIMI.KEY (ok if absent)
    load_feeds();
    recompute_layout();
    measure_reader_font();
    rebuild_views();
    if (g_feed_count > 0) { g_cur_feed = 0; start_fetch(); }

    int running = 1;
    while (running) {
        // The palette macros read theme_color() live, so a theme switch only
        // needs a repaint (#280 theme-aware app pattern).
        { int th = theme_get_active();
          if (th != g_theme_last) { g_theme_last = th; draw_all(); win_invalidate(win); } }

        gui_event_t ev;
        int ret = win_get_event(win, &ev, 50);
        if (ret > 0) {
            switch (ev.type) {
                case EVENT_WINDOW_CLOSE: running = 0; break;
                case EVENT_KEY_DOWN:
                    if (g_add_mode) {
                        if (ev.keycode == 0x01) g_add_mode = 0;
                        else if (ev.keycode == 0x1C || ev.key_char=='\n' || ev.key_char=='\r') add_feed_submit();
                        else tf_handle_key(&g_add_tf, &ev);
                        break;
                    }
                    if (g_focus == FOCUS_SEARCH) {
                        if (ev.keycode == 0x01) { g_focus = FOCUS_LIST; }         // Esc
                        else if (ev.keycode == 0x1C || ev.key_char=='\n') { g_focus = FOCUS_LIST; }
                        else { tf_handle_key(&g_search_tf, &ev); g_list_scroll = 0; rebuild_views(); }
                        break;
                    }
                    // list-focused shortcuts
                    if (ev.keycode == 0x01) running = 0;                            // Esc
                    else if (ev.keycode == 0x80) { int v=sel_view_index(); select_item_view(v-1); }  // Up
                    else if (ev.keycode == 0x81) { int v=sel_view_index(); select_item_view(v+1); }  // Down
                    else if (ev.keycode == 0x49) { int v=sel_view_index(); select_item_view(v-list_rows()); } // PgUp
                    else if (ev.keycode == 0x51) { int v=sel_view_index(); int n=v+list_rows(); select_item_view(n>=g_item_view_n?g_item_view_n-1:n); }
                    else if (ev.key_char=='r' || ev.key_char=='R' || ev.keycode==0x3F) start_fetch();
                    else if (ev.key_char=='a' || ev.key_char=='A') { g_add_mode=1; tf_set_text(&g_add_tf,"https://"); }
                    else if (ev.key_char=='/' ) g_focus = FOCUS_SEARCH;
                    else if (ev.key_char=='s' || ev.key_char=='S') start_summarize();
                    else if (ev.key_char=='+' || ev.key_char=='=') change_size(+1);
                    else if (ev.key_char=='-' || ev.key_char=='_') change_size(-1);
                    else if (ev.key_char=='f' || ev.key_char=='F') cycle_font();
                    else if (ev.keycode==0x0F || ev.key_char=='\t') {              // Tab: next feed
                        if (g_feed_count>0){ g_cur_feed=(g_cur_feed+1)%g_feed_count; start_fetch(); }
                    }
                    break;
                case EVENT_MOUSE_DOWN:
                    handle_click(ev.mouse_x, ev.mouse_y);
                    break;
                case EVENT_MOUSE_SCROLL:
                    if (ev.mouse_x >= det_x && ev.mouse_y >= det_y && ev.mouse_y < det_y+det_h) {
                        g_reader_scroll -= ev.scroll_delta * g_line_h;
                        if (g_reader_scroll < 0) g_reader_scroll = 0;
                        if (g_reader_scroll > 20000) g_reader_scroll = 20000;
                    } else if (ev.mouse_x >= list_x && ev.mouse_x < list_x+list_w) {
                        g_list_scroll -= ev.scroll_delta * 3;
                        if (g_list_scroll < 0) g_list_scroll = 0;
                    }
                    break;
                default: break;
            }
        }

        poll_net_wait();   // re-arm the startup fetch once DHCP binds
        poll_fetch();
        poll_img();        // inline images: fetch + decode, never blocking
        // AI worker completion updates the reader on the next redraw; if it just
        // finished, refresh the status line.
        if (g_ai_state == AI_DONE && g_show_summary) {
            scpy(g_status_msg, "AI summary ready.", sizeof(g_status_msg));
            g_ai_state = AI_IDLE;   // consumed; result stays in g_ai_result
        } else if (g_ai_state == AI_ERROR && g_show_summary) {
            g_ai_state = AI_IDLE;
        }
        draw_all();
        win_invalidate(win);
    }

    if (g_job >= 0) http_fetch_cancel(g_job);
    if (g_img_job >= 0) http_fetch_cancel(g_img_job);
    for (int i = 0; i < IMG_CACHE_N; i++) img_release(&g_imgc[i]);
    win_destroy(win);
    return 0;
}
