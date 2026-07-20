// browser - Web Browser for MayteraOS (userland version)
// Fetches and renders web pages via SYS_HTTP_FETCH syscall.
//
// UI uplift: modern chrome built on the libc style engine (gui_style.h),
// matching the Settings / Files apps. Themed palette, rounded address bar with
// a focus state, styled toolbar buttons (Back/Forward/Reload/Home/Go), and a
// slim status bar. Page parsing and fetch behavior are unchanged; this is a
// visual/layout uplift only.

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"

// NetSurf-backed render pipeline (hubbub->libdom parse, libcss style, our
// MIT block+inline layout). See /opt/maytera/netsurf-port/.
#include "dom_hubbub_bind.h"
#include "css_select_bind.h"
#include "layout.h"
#include "duk_dom.h"  // Duktape JS + DOM binding (Phase 2)

// Route in-window text through the antialiased TrueType path (matches Settings).
#define br_text(h, x, y, s, c)       win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define br_text_sz(h, x, y, s, sz, c) win_draw_text_ttf((h), (x), (y), (s), (sz), (c))

// ============================================================================
// Layout / window dimensions
// ============================================================================

// #89: live window size, updated on EVENT_RESIZE so the whole UI (toolbar,
// content viewport, status bar) and the page layout width reflow on resize.
static int g_win_w = 800, g_win_h = 600;
#define WIN_WIDTH       g_win_w
#define WIN_HEIGHT      g_win_h

// Toolbar (chrome) band height and the status bar height.
#define TOOLBAR_H       52
#define STATUS_H        24

// Navigation buttons live on the left of the toolbar.
#define NAV_BTN_W       34
#define NAV_BTN_H       32
#define NAV_BTN_Y       10
#define NAV_GAP         4
#define TOOLBAR_PAD     10

// Four nav buttons: Back, Forward, Reload, Home.
#define NAV_BACK_X      (TOOLBAR_PAD)
#define NAV_FWD_X       (NAV_BACK_X + NAV_BTN_W + NAV_GAP)
#define NAV_RELOAD_X    (NAV_FWD_X  + NAV_BTN_W + NAV_GAP)
#define NAV_HOME_X      (NAV_RELOAD_X + NAV_BTN_W + NAV_GAP)

// Go button on the right of the toolbar.
#define GO_BTN_W        56
#define GO_BTN_H        32
#define GO_BTN_Y        10
#define GO_BTN_X        (WIN_WIDTH - TOOLBAR_PAD - GO_BTN_W)

// Address bar fills the gap between the nav buttons and the Go button.
#define URL_BAR_X       (NAV_HOME_X + NAV_BTN_W + 12)
#define URL_BAR_Y       10
#define URL_BAR_W       (GO_BTN_X - 8 - URL_BAR_X)
#define URL_BAR_H       32

// Content viewport (kept large).
#define CONTENT_X       8
#define CONTENT_Y       (TOOLBAR_H + 6)
#define CONTENT_W       (WIN_WIDTH - 16)
#define CONTENT_H       (WIN_HEIGHT - CONTENT_Y - STATUS_H - 6)
#define SB_W            12   // vertical scrollbar width (#245)

#define HOME_URL        "https://example.com"

// ============================================================================
// Theme palette (mirrors the Settings app approach)
// ============================================================================

static uint32_t COL_WINDOW_BG;
static uint32_t COL_TOOLBAR_BG;
static uint32_t COL_CONTENT_BG;
static uint32_t COL_CARD_BG;
static uint32_t COL_STATUS_BG;
static uint32_t COL_SEPARATOR;
static uint32_t COL_TEXT;
static uint32_t COL_TEXT_DIM;
static uint32_t COL_ACCENT;
static uint32_t COL_FIELD_BG;
static uint32_t COL_FIELD_BORDER;
static uint32_t COL_ERROR;

// Build the palette from a kernel theme id and push it into the style engine
// so all gui_* primitives render in the active theme.
static void apply_theme(int kernel_theme) {
    switch (kernel_theme) {
        case 2:  // Light
            COL_WINDOW_BG    = 0x00ECECEC;
            COL_TOOLBAR_BG   = 0x00F4F4F4;
            COL_CONTENT_BG   = 0x00FFFFFF;
            COL_CARD_BG      = 0x00F8F8F8;
            COL_STATUS_BG    = 0x00E4E4E4;
            COL_SEPARATOR    = 0x00CCCCCC;
            COL_TEXT         = 0x00202020;
            COL_TEXT_DIM     = 0x00606060;
            COL_ACCENT       = 0x00569CD6;
            COL_FIELD_BG     = 0x00FFFFFF;
            COL_FIELD_BORDER = 0x00CCCCCC;
            COL_ERROR        = 0x00CC3333;
            break;
        case 4:  // Classic (Win95 / CDE)
            COL_WINDOW_BG    = 0x00C0C0C0;
            COL_TOOLBAR_BG   = 0x00C0C0C0;
            COL_CONTENT_BG   = 0x00FFFFFF;
            COL_CARD_BG      = 0x00D0D0D0;
            COL_STATUS_BG    = 0x00C0C0C0;
            COL_SEPARATOR    = 0x00808080;
            COL_TEXT         = 0x00000000;
            COL_TEXT_DIM     = 0x00404040;
            COL_ACCENT       = 0x00000080;
            COL_FIELD_BG     = 0x00FFFFFF;
            COL_FIELD_BORDER = 0x00000000;
            COL_ERROR        = 0x00CC0000;
            break;
        case 5:  // Ocean
            COL_WINDOW_BG    = 0x001A3A4A;
            COL_TOOLBAR_BG   = 0x00224455;
            COL_CONTENT_BG   = 0x00183040;
            COL_CARD_BG      = 0x001E4050;
            COL_STATUS_BG    = 0x001A3A4A;
            COL_SEPARATOR    = 0x00406070;
            COL_TEXT         = 0x00E0F0FF;
            COL_TEXT_DIM     = 0x0090B0C0;
            COL_ACCENT       = 0x0044AAAA;
            COL_FIELD_BG     = 0x00102838;
            COL_FIELD_BORDER = 0x00406070;
            COL_ERROR        = 0x00FF7766;
            break;
        case 9:  // Modern Dark / Nord-ish
            COL_WINDOW_BG    = 0x002E3440;
            COL_TOOLBAR_BG   = 0x003B4252;
            COL_CONTENT_BG   = 0x00262B33;
            COL_CARD_BG      = 0x00434C5E;
            COL_STATUS_BG    = 0x002E3440;
            COL_SEPARATOR    = 0x004C566A;
            COL_TEXT         = 0x00ECEFF4;
            COL_TEXT_DIM     = 0x00AEB6C4;
            COL_ACCENT       = 0x0088C0D0;
            COL_FIELD_BG     = 0x003B4252;
            COL_FIELD_BORDER = 0x004C566A;
            COL_ERROR        = 0x00DD7777;
            break;
        case 1:  // Dark / Midnight (default)
        default:
            COL_WINDOW_BG    = 0x001E1E1E;
            COL_TOOLBAR_BG   = 0x00252525;
            COL_CONTENT_BG   = 0x00181818;
            COL_CARD_BG      = 0x002A2A2A;
            COL_STATUS_BG    = 0x001E1E1E;
            COL_SEPARATOR    = 0x00404040;
            COL_TEXT         = 0x00F0F0F0;
            COL_TEXT_DIM     = 0x00AAAAAA;
            COL_ACCENT       = 0x00569CD6;
            COL_FIELD_BG     = 0x00333333;
            COL_FIELD_BORDER = 0x00505050;
            COL_ERROR        = 0x00DD5555;
            break;
    }

    // Classic uses the beveled CDE renderer; everything else uses modern.
    gui_set_style(kernel_theme == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);

    gui_palette_t pal;
    pal.surface        = COL_CONTENT_BG;
    pal.surface_raised = COL_CARD_BG;
    pal.ink            = COL_TEXT;
    pal.ink_dim        = COL_TEXT_DIM;
    pal.accent         = COL_ACCENT;
    pal.accent_hover   = gui_lighten(COL_ACCENT, 24);
    pal.border         = COL_FIELD_BORDER;
    pal.field_bg       = COL_FIELD_BG;
    pal.field_border   = COL_FIELD_BORDER;
    pal.track          = COL_SEPARATOR;
    gui_set_palette(&pal);
}

// ============================================================================
// State
// ============================================================================

#define MAX_URL_LEN     512
static char url_buffer[MAX_URL_LEN] = HOME_URL;
static int  url_cursor = 19;   // length of HOME_URL
static int  url_fresh  = 0;   // next keystroke replaces the whole URL

// Clickable-link hit map, rebuilt every content draw (window-relative rects).
typedef struct { int x, y, w, h; char href[256]; } link_hit_t;
static link_hit_t g_link_hits[512];
static int g_link_hit_n = 0;

// Form-control hit map (rebuilt each content draw) + focused text field + value.
typedef struct { int x, y, w, h; int kind; char name[64]; char action[256]; } form_hit_t;
static form_hit_t g_form_hits[64];
static int g_form_hit_n = 0;
static int g_focus_field = -1;        // index of focused text field in g_form_hits
static char g_field_val[256];
static int g_field_len = 0;

// Decoded inline-image cache (keyed by layout item index) + fetch scratch.
// (#247/Task2) IMG_MAX raised 24->48 so image-rich pages (e.g. footers full of
// sponsor logos) get all their raster <img> loaded; fetch buffer raised
// 700KB->1.5MB so moderately large JPEG/PNG no longer exceed the buffer and skip.
#define IMG_MAX 48
typedef struct { int item; int w, h; uint32_t *px; } img_entry_t;
static img_entry_t g_imgs[IMG_MAX];
static int g_img_n = 0;
static unsigned char g_imgfetch[1536 * 1024];
// Async image loading state (#247): one background fetch in flight at a time,
// driven by poll_images() each main-loop tick so images never block the UI.
static int g_img_scan = -1;   // next layout item to scan (-1 = idle/done)
static int g_img_job  = -1;   // active async image fetch job id (-1 = none)
static int g_img_item = -1;   // layout item index the active job is for
static void images_begin(void);
static void poll_images(void);
static void resolve_url(const char *href, char *out);
static int  url_focused = 1;   // address bar focus state

// Raw fetch buffer (64KB).
#define MAX_CONTENT (1024 * 1024)  // 1 MB raw fetch buffer (was 64KB; large pages were truncated)
static char content_buffer[MAX_CONTENT];

// Parsed display buffer (64KB).
#define MAX_DISPLAY     (1024 * 1024)
static char display_buffer[MAX_DISPLAY];
static int  display_length = 0;

// NetSurf render output: positioned text runs for the current page.
static layout_item g_items[LAYOUT_MAX_ITEMS];
static layout_result g_layout = { g_items, 0, 0 };
static int g_have_layout = 0;   // 1 once a page has been laid out

// Scroll state.
static int  scroll_offset = 0;
static int  g_content_len = 0;   // #89: length of the page in content_buffer (for re-layout on resize)
static int  last_layout_w = 0;   // #89: window width the page was last laid out at
static int  sb_dragging = 0;   // dragging the scrollbar thumb
static int  sb_drag_dy = 0;    // cursor offset within the thumb at grab

// Status message.
static char status_msg[160] = "Ready";
static int  is_loading = 0;
static int  last_was_error = 0;

// Simple back/forward history so the nav buttons are functional.
#define HIST_MAX        32
static char hist[HIST_MAX][MAX_URL_LEN];
static int  hist_count = 0;   // number of entries
static int  hist_pos = -1;    // current index into hist

// Hover/press state for toolbar buttons (-1 = none). Indices:
// 0=Back 1=Forward 2=Reload 3=Home 4=Go
static int  btn_hover = -1;
static int  btn_press = -1;

static int  window_handle = -1;
static int  running = 1;

// ============================================================================
// String helpers (freestanding)
// ============================================================================

static int str_len(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void str_cpy(char *dest, const char *src) {
    while (*src) *dest++ = *src++;
    *dest = '\0';
}

static int str_ncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

// Case-insensitive compare for tag matching.
static int tag_eq(const char *tag, const char *match) {
    while (*match) {
        char a = *tag, b = *match;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        tag++; match++;
    }
    return (*tag == '\0');
}

static void int_to_str(int val, char *buf) {
    if (val == 0) { buf[0] = '0'; buf[1] = 0; return; }
    int neg = 0;
    if (val < 0) { neg = 1; val = -val; }
    char tmp[16];
    int i = 0;
    while (val > 0 && i < 14) { tmp[i++] = '0' + (val % 10); val /= 10; }
    int p = 0;
    if (neg) buf[p++] = '-';
    while (i > 0) buf[p++] = tmp[--i];
    buf[p] = 0;
}

// ============================================================================
// HTML Parser (unchanged behavior; ported from kernel gui/browser.c)
// ============================================================================

// Parse raw HTML in content_buffer into plain text in display_buffer.
// Handles tag stripping, block elements, entity decoding, script/style
// skipping, whitespace normalization, and word-wrap at ~90 columns.
static void parse_html(const char *html, int html_len) {
    display_length = 0;
    if (!html || html_len == 0) return;

    const char *p = html;
    const char *end = html + html_len;

    int col = 0;            // current column for word-wrap
    int skip_content = 0;   // inside <script> or <style>
    int last_was_space = 1; // collapse whitespace
    int last_was_newline = 0;

    #define WRAP_COL 90

    #define EMIT_CHAR(c) do { \
        if (display_length < MAX_DISPLAY - 1) { \
            display_buffer[display_length++] = (c); \
        } \
    } while(0)

    #define EMIT_NEWLINE() do { \
        if (display_length > 0 && display_buffer[display_length-1] != '\n') { \
            EMIT_CHAR('\n'); \
            col = 0; \
            last_was_space = 1; \
            last_was_newline = 1; \
        } \
    } while(0)

    #define EMIT_BLANK_LINE() do { \
        EMIT_NEWLINE(); \
        if (!last_was_newline || (display_length >= 2 && display_buffer[display_length-2] != '\n')) { \
            EMIT_CHAR('\n'); \
        } \
        last_was_newline = 1; \
    } while(0)

    while (p < end) {
        char c = *p++;

        if (c == '<') {
            char tag[64];
            int tag_pos = 0;

            while (p < end && *p != '>' && *p != ' ' && *p != '\t'
                   && *p != '\n' && *p != '/' && tag_pos < 63) {
                tag[tag_pos++] = *p++;
            }
            tag[tag_pos] = '\0';

            while (p < end && *p != '>') p++;
            if (p < end) p++;  // skip '>'

            if (tag_eq(tag, "br") || tag_eq(tag, "br/")) {
                EMIT_NEWLINE();
            } else if (tag_eq(tag, "p") || tag_eq(tag, "/p")) {
                EMIT_BLANK_LINE();
            } else if (tag_eq(tag, "div") || tag_eq(tag, "/div")) {
                EMIT_NEWLINE();
            } else if (tag_eq(tag, "li")) {
                EMIT_NEWLINE();
                EMIT_CHAR(' '); EMIT_CHAR(' ');
                EMIT_CHAR('-'); EMIT_CHAR(' ');
                col = 4;
                last_was_space = 1;
            } else if (tag_eq(tag, "/li")) {
                EMIT_NEWLINE();
            } else if (tag_eq(tag, "ul") || tag_eq(tag, "/ul") ||
                       tag_eq(tag, "ol") || tag_eq(tag, "/ol")) {
                EMIT_NEWLINE();
            } else if (tag_eq(tag, "h1") || tag_eq(tag, "h2") || tag_eq(tag, "h3") ||
                       tag_eq(tag, "h4") || tag_eq(tag, "h5") || tag_eq(tag, "h6")) {
                EMIT_BLANK_LINE();
            } else if (tag[0] == '/' && (tag[1] == 'h' || tag[1] == 'H') &&
                       tag[2] >= '1' && tag[2] <= '6') {
                EMIT_NEWLINE();
            } else if (tag_eq(tag, "tr") || tag_eq(tag, "/tr")) {
                EMIT_NEWLINE();
            } else if (tag_eq(tag, "td") || tag_eq(tag, "th")) {
                if (!last_was_space && col > 0) {
                    EMIT_CHAR(' '); EMIT_CHAR(' ');
                    col += 2;
                    last_was_space = 1;
                }
            } else if (tag_eq(tag, "title")) {
                while (p < end) {
                    if (*p == '<' && p + 7 < end &&
                        (str_ncmp(p, "</title>", 8) == 0 ||
                         str_ncmp(p, "</TITLE>", 8) == 0 ||
                         str_ncmp(p, "</Title>", 8) == 0)) {
                        p += 8;
                        break;
                    }
                    p++;
                }
            } else if (tag_eq(tag, "script")) {
                skip_content = 1;
            } else if (tag_eq(tag, "/script")) {
                skip_content = 0;
            } else if (tag_eq(tag, "style")) {
                skip_content = 1;
            } else if (tag_eq(tag, "/style")) {
                skip_content = 0;
            } else if (tag_eq(tag, "head")) {
                while (p < end) {
                    if (*p == '<' && p + 6 < end &&
                        (str_ncmp(p, "</head>", 7) == 0 ||
                         str_ncmp(p, "</HEAD>", 7) == 0)) {
                        p += 7;
                        break;
                    }
                    p++;
                }
            }
            // All other tags are simply stripped.
            continue;
        }

        if (skip_content) continue;

        if (c == '&') {
            if (p + 4 < end && str_ncmp(p, "nbsp;", 5) == 0) {
                c = ' '; p += 5;
            } else if (p + 2 < end && str_ncmp(p, "lt;", 3) == 0) {
                c = '<'; p += 3;
            } else if (p + 2 < end && str_ncmp(p, "gt;", 3) == 0) {
                c = '>'; p += 3;
            } else if (p + 3 < end && str_ncmp(p, "amp;", 4) == 0) {
                c = '&'; p += 4;
            } else if (p + 4 < end && str_ncmp(p, "quot;", 5) == 0) {
                c = '"'; p += 5;
            } else if (p + 4 < end && str_ncmp(p, "apos;", 5) == 0) {
                c = '\''; p += 5;
            } else if (p + 4 < end && str_ncmp(p, "#160;", 5) == 0) {
                c = ' '; p += 5;
            } else if (p + 4 < end && str_ncmp(p, "#60;", 4) == 0) {
                c = '<'; p += 4;
            } else if (p + 4 < end && str_ncmp(p, "#62;", 4) == 0) {
                c = '>'; p += 4;
            } else if (p + 4 < end && str_ncmp(p, "#38;", 4) == 0) {
                c = '&'; p += 4;
            } else {
                while (p < end && *p != ';' && *p != '<' && *p != ' ') p++;
                if (p < end && *p == ';') p++;
                continue;
            }
        }

        if (c == '\r') continue;
        if (c == '\n' || c == '\t') c = ' ';

        if (c == ' ') {
            if (last_was_space) continue;
            last_was_space = 1;
        } else {
            last_was_space = 0;
            last_was_newline = 0;
        }

        if (c < ' ' || c >= 127) continue;

        if (col >= WRAP_COL && c == ' ') {
            EMIT_NEWLINE();
            continue;
        }
        if (col >= WRAP_COL + 10) {
            EMIT_NEWLINE();
        }

        EMIT_CHAR(c);
        col++;
    }

    display_buffer[display_length] = '\0';

    #undef EMIT_CHAR
    #undef EMIT_NEWLINE
    #undef EMIT_BLANK_LINE
    #undef WRAP_COL
}

// ============================================================================
// Fetch page via syscall (behavior unchanged)
// ============================================================================

// ============================================================================
// NetSurf-backed page render: parse HTML -> DOM, style with libcss, lay out
// into positioned text runs. Replaces the old plain-text parse_html output.
// ============================================================================

static int br_measure(const char *str, int size) {
    return ttf_measure(str, size);
}

// Extract the contents of all <style>...</style> blocks into  (author CSS).
static int extract_styles(const char *html, int len, char *dst, int dst_cap) {
    int o = 0; int i = 0;
    while (i < len) {
        // find "<style"
        if ((html[i]=='<') && i+6<len &&
            (html[i+1]=='s'||html[i+1]=='S') &&
            (html[i+2]=='t'||html[i+2]=='T') &&
            (html[i+3]=='y'||html[i+3]=='Y') &&
            (html[i+4]=='l'||html[i+4]=='L') &&
            (html[i+5]=='e'||html[i+5]=='E')) {
            // skip to end of opening tag
            while (i < len && html[i] != '>') i++;
            if (i < len) i++;
            // copy until </style
            while (i < len) {
                if (html[i]=='<' && i+7<len && html[i+1]=='/' &&
                    (html[i+2]=='s'||html[i+2]=='S')) break;
                if (o < dst_cap-1) dst[o++] = html[i];
                i++;
            }
            if (o < dst_cap-1) dst[o++] = '\n';
        } else {
            i++;
        }
    }
    dst[o] = '\0';
    return o;
}

// ---------------------------------------------------------------------------
// External CSS (#245): find <link rel="stylesheet" href="..."> in the raw HTML,
// fetch each sheet synchronously, and feed it to libcss as author CSS before
// layout. Inline <style> is still applied (in source order is ideal; we add
// inline first then external here, which is an accepted first-cut approximation
// of the cascade for these mostly-low-specificity sheets).
// ---------------------------------------------------------------------------

#define CSS_LINK_MAX     8                 // cap external sheets per page
#define CSS_COMBINED_MAX (1024 * 1024)     // total CSS budget (1 MB)
static char g_cssfetch[256 * 1024];        // per-sheet sync fetch scratch

// Pull up to CSS_LINK_MAX stylesheet hrefs from the raw HTML into hrefs[][].
// Matches <link ... rel=...stylesheet... href=...> case-insensitively; skips
// rel values that are clearly not a main stylesheet (alternate / preload).
// Returns the number of hrefs collected.
static int extract_link_css(const char *html, int len,
                            char hrefs[][MAX_URL_LEN], int max_links) {
    int count = 0;
    int i = 0;
    while (i < len && count < max_links) {
        if (html[i] == '<' && i + 5 < len &&
            (html[i+1]=='l'||html[i+1]=='L') &&
            (html[i+2]=='i'||html[i+2]=='I') &&
            (html[i+3]=='n'||html[i+3]=='N') &&
            (html[i+4]=='k'||html[i+4]=='K') &&
            (html[i+5]==' '||html[i+5]=='\t'||html[i+5]=='\n'||html[i+5]=='\r')) {
            int tag_end = i + 5;
            while (tag_end < len && html[tag_end] != '>') tag_end++;
            int rel_ok = 0, rel_bad = 0;
            char href[MAX_URL_LEN]; href[0] = 0;
            for (int j = i; j < tag_end; j++) {
                if ((html[j]=='r'||html[j]=='R') && j+3 < tag_end &&
                    (html[j+1]=='e'||html[j+1]=='E') &&
                    (html[j+2]=='l'||html[j+2]=='L')) {
                    int k = j + 3;
                    while (k < tag_end && (html[k]==' '||html[k]=='\t')) k++;
                    if (k < tag_end && html[k]=='=') {
                        k++;
                        while (k < tag_end && (html[k]==' '||html[k]=='\t')) k++;
                        char q = 0;
                        if (k < tag_end && (html[k]=='"' || html[k]=='\'')) { q = html[k]; k++; }
                        char rv[64]; int rn = 0;
                        while (k < tag_end && rn < 63) {
                            char c = html[k];
                            if (q && c == q) break;
                            if (!q && (c==' '||c=='\t'||c=='>')) break;
                            rv[rn++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
                            k++;
                        }
                        rv[rn] = 0;
                        for (int x = 0; x + 9 < rn + 1; x++) {
                            if (rv[x]=='s'&&rv[x+1]=='t'&&rv[x+2]=='y'&&rv[x+3]=='l'&&
                                rv[x+4]=='e'&&rv[x+5]=='s'&&rv[x+6]=='h'&&rv[x+7]=='e'&&
                                rv[x+8]=='e'&&rv[x+9]=='t') { rel_ok = 1; break; }
                        }
                        for (int x = 0; x + 8 < rn + 1; x++) {
                            if (rv[x]=='a'&&rv[x+1]=='l'&&rv[x+2]=='t'&&rv[x+3]=='e'&&
                                rv[x+4]=='r'&&rv[x+5]=='n'&&rv[x+6]=='a'&&rv[x+7]=='t'&&
                                rv[x+8]=='e') { rel_bad = 1; break; }
                        }
                        for (int x = 0; x + 6 < rn + 1; x++) {
                            if (rv[x]=='p'&&rv[x+1]=='r'&&rv[x+2]=='e'&&rv[x+3]=='l'&&
                                rv[x+4]=='o'&&rv[x+5]=='a'&&rv[x+6]=='d') { rel_bad = 1; break; }
                        }
                    }
                }
                if ((html[j]=='h'||html[j]=='H') && j+3 < tag_end &&
                    (html[j+1]=='r'||html[j+1]=='R') &&
                    (html[j+2]=='e'||html[j+2]=='E') &&
                    (html[j+3]=='f'||html[j+3]=='F')) {
                    int k = j + 4;
                    while (k < tag_end && (html[k]==' '||html[k]=='\t')) k++;
                    if (k < tag_end && html[k]=='=') {
                        k++;
                        while (k < tag_end && (html[k]==' '||html[k]=='\t')) k++;
                        char q = 0;
                        if (k < tag_end && (html[k]=='"' || html[k]=='\'')) { q = html[k]; k++; }
                        int hn = 0;
                        while (k < tag_end && hn < MAX_URL_LEN - 1) {
                            char c = html[k];
                            if (q && c == q) break;
                            if (!q && (c==' '||c=='\t'||c=='>')) break;
                            href[hn++] = c;
                            k++;
                        }
                        href[hn] = 0;
                    }
                }
            }
            if (rel_ok && !rel_bad && href[0]) {
                int z = 0;
                while (href[z] && z < MAX_URL_LEN - 1) { hrefs[count][z] = href[z]; z++; }
                hrefs[count][z] = 0;
                count++;
            }
            i = (tag_end < len) ? tag_end + 1 : len;
        } else {
            i++;
        }
    }
    return count;
}

// Fetch + apply all external stylesheets referenced by the page. Bounded by the
// existing sync fetch timeout per sheet and a total CSS byte budget. Returns the
// number of sheets successfully applied.
static int apply_external_css(mcs_ctx *css, const char *html, int html_len) {
    static char css_hrefs[CSS_LINK_MAX][MAX_URL_LEN];
    int nlinks = extract_link_css(html, html_len, css_hrefs, CSS_LINK_MAX);
    int applied = 0;
    long total = 0;
    for (int i = 0; i < nlinks && total < CSS_COMBINED_MAX; i++) {
        char abs_url[MAX_URL_LEN];
        resolve_url(css_hrefs[i], abs_url);
        if (!abs_url[0]) continue;
        unsigned int got = 0;
        int http_status = 0;
        int ret = sys_http_fetch(abs_url, g_cssfetch, sizeof(g_cssfetch) - 1,
                                 &got, &http_status);
        if (ret < 0 || got == 0) continue;
        if (http_status != 200 && http_status != 0) continue;
        g_cssfetch[got] = 0;
        if ((long)got + total > CSS_COMBINED_MAX) got = (unsigned int)(CSS_COMBINED_MAX - total);
        if (mcs_add_author_css(css, g_cssfetch, (unsigned long)got) == 0) {
            applied++;
            total += got;
        }
    }
    return applied;
}

static char author_css[16384];

static void render_page(const char *html, int html_len) {
    g_content_len = html_len;   // #89: remember for re-layout on resize
    last_layout_w = g_win_w;
    g_have_layout = 0;
    g_layout.n_items = 0;
    g_layout.content_height = 0;

    mdb_parser *p = mdb_create();
    if (!p) return;
    mdb_parse_chunk(p, (const unsigned char *) html, (unsigned long) html_len);
    mdb_parse_complete(p);

    // Run inline <script> elements under Duktape with the DOM bound.
    // DOM mutations are visible to the layout pass below.
    js_run_document(mdb_document(p), 0, 0);

    mcs_ctx *css = mcs_create();
    if (!css) { mdb_destroy(p); return; }

    int css_len = extract_styles(html, html_len, author_css, sizeof(author_css));
    if (css_len > 0)
        mcs_add_author_css(css, author_css, (unsigned long) css_len);

    // External stylesheets (#245): fetch + apply <link rel=stylesheet> sheets.
    // Synchronous; bounded by the fetch timeout per sheet and a total CSS budget.
    apply_external_css(css, html, html_len);

    g_layout.items = g_items;
    int content_width = CONTENT_W - 24; // padding both sides
    if (layout_document(css, mdb_document(p), content_width,
                        br_measure, &g_layout) == 0) {
        g_have_layout = 1;
        images_begin();
    }

    // The document + styles stay referenced by p/css. We intentionally leak
    // them here (one page at a time, small) rather than risk the teardown
    // path; freeing is not required for correctness of the rendered runs.
    (void) css;
    (void) p;
}

static int fetch_page(const char *url) {
    unsigned int bytes_read = 0;
    int http_status = 0;

    int ret = sys_http_fetch(url, content_buffer, MAX_CONTENT - 1,
                             &bytes_read, &http_status);

    if (ret < 0) {
        // ret: -1=no_mem/bad_url, -2=dns/no_net, -3=connect, -4=tls, -5=send/timeout
        //      -6=timeout/closed, -100=bad_args, -101=empty_url
        str_cpy(status_msg, "Fetch failed, code: ");
        char numbuf[16];
        int_to_str(ret, numbuf);
        int p = str_len(status_msg);
        for (int i = 0; numbuf[i]; i++) status_msg[p++] = numbuf[i];
        status_msg[p++] = ' ';
        status_msg[p++] = 's';
        status_msg[p++] = 't';
        status_msg[p++] = ':';
        int_to_str(http_status, numbuf);
        for (int i = 0; numbuf[i]; i++) status_msg[p++] = numbuf[i];
        status_msg[p] = 0;
        last_was_error = 1;
        return -1;
    }

    content_buffer[bytes_read] = '\0';

    if (http_status != 200 && http_status != 301 && http_status != 302) {
        str_cpy(status_msg, "HTTP ");
        int pos = str_len(status_msg);
        status_msg[pos++] = '0' + (http_status / 100);
        status_msg[pos++] = '0' + ((http_status / 10) % 10);
        status_msg[pos++] = '0' + (http_status % 10);
        status_msg[pos] = '\0';
        last_was_error = 1;
    } else {
        str_cpy(status_msg, "Done  ");
        int pos = str_len(status_msg);
        for (int i = 0; url[i] && pos < (int)sizeof(status_msg) - 1; i++)
            status_msg[pos++] = url[i];
        status_msg[pos] = '\0';
        last_was_error = 0;
    }

    render_page(content_buffer, (int)bytes_read);
    return 0;
}

// ============================================================================
// Drawing
// ============================================================================

// Map a toolbar button index to its rect. Returns 1 if valid.
static int nav_btn_rect(int idx, int *x, int *y, int *w, int *h) {
    switch (idx) {
        case 0: *x = NAV_BACK_X;   *y = NAV_BTN_Y; *w = NAV_BTN_W; *h = NAV_BTN_H; return 1;
        case 1: *x = NAV_FWD_X;    *y = NAV_BTN_Y; *w = NAV_BTN_W; *h = NAV_BTN_H; return 1;
        case 2: *x = NAV_RELOAD_X; *y = NAV_BTN_Y; *w = NAV_BTN_W; *h = NAV_BTN_H; return 1;
        case 3: *x = NAV_HOME_X;   *y = NAV_BTN_Y; *w = NAV_BTN_W; *h = NAV_BTN_H; return 1;
        case 4: *x = GO_BTN_X;     *y = GO_BTN_Y;  *w = GO_BTN_W;  *h = GO_BTN_H;  return 1;
        default: return 0;
    }
}

static int can_go_back(void)    { return hist_pos > 0; }
static int can_go_forward(void) { return hist_pos >= 0 && hist_pos < hist_count - 1; }

// Pick the widget state for a button, honoring enabled/hover/press.
static gui_state_t btn_state(int idx, int enabled) {
    if (!enabled) return GUI_ST_DISABLED;
    if (btn_press == idx) return GUI_ST_PRESSED;
    if (btn_hover == idx) return GUI_ST_HOVER;
    return GUI_ST_NORMAL;
}

// Small vector glyphs for the Reload/Home buttons (drawn over a blank button).
static void draw_home_icon(int bx, int by, int bw, int bh, uint32_t col) {
    int cx = bx + bw / 2, cy = by + bh / 2, sz = 5;
    // Roof: filled triangle, apex up.
    for (int r = 0; r <= sz; r++)
        gui_fill_rect(window_handle, cx - r, cy - sz - 2 + r, 2 * r + 1, 1, col);
    // Body: outlined box under the roof.
    int w = 2 * sz - 2, x = cx - w / 2, y = cy - 2, h = sz + 2;
    gui_draw_rect_outline(window_handle, x, y, w, h, col);
    // Door.
    gui_fill_rect(window_handle, cx - 1, y + h - 3, 2, 3, col);
}

static void draw_refresh_icon(int bx, int by, int bw, int bh, uint32_t col) {
    int cx = bx + bw / 2, cy = by + bh / 2;
    int ro = 7, ri = 4;                       // ~3px-thick ring
    // Open ring: a 3/4 annulus with a gap in the top-right quadrant.
    for (int dy = -ro; dy <= ro; dy++) {
        for (int dx = -ro; dx <= ro; dx++) {
            int d2 = dx * dx + dy * dy;
            if (d2 > ro * ro || d2 < ri * ri) continue;
            if (dx > 0 && dy < 0) continue;   // gap (where the arrow opens)
            gui_fill_rect(window_handle, cx + dx, cy + dy, 1, 1, col);
        }
    }
    // Arrowhead: a filled triangle at the top end of the arc, pointing right
    // (clockwise) into the gap. Vertical base across the ring, tip to the right.
    int basey_top = cy - ro - 1;
    int basey_bot = cy - ri + 1;
    int tipx = cx + ro + 1;
    for (int x = cx; x <= tipx; x++) {
        int t = x - cx;
        int yt = basey_top + t;
        int yb = basey_bot - t;
        if (yt > yb) break;
        for (int y = yt; y <= yb; y++)
            gui_fill_rect(window_handle, x, y, 1, 1, col);
    }
}

// Draw the toolbar band, nav buttons, address bar, and Go button.
static void draw_toolbar(void) {
    // Toolbar background band + bottom separator.
    gui_fill_rect(window_handle, 0, 0, WIN_WIDTH, TOOLBAR_H, COL_TOOLBAR_BG);
    gui_fill_rect(window_handle, 0, TOOLBAR_H - 1, WIN_WIDTH, 1, COL_SEPARATOR);

    // Navigation buttons. Glyph labels keep the chrome compact and modern.
    static const char *labels[4] = { "<", ">", "", "" };  // 2=Reload,3=Home use icons
    int en[4] = { can_go_back(), can_go_forward(), 1, 1 };
    for (int i = 0; i < 4; i++) {
        int bx, by, bw, bh;
        nav_btn_rect(i, &bx, &by, &bw, &bh);
        gui_button(window_handle, bx, by, bw, bh, labels[i],
                   GUI_BTN_SECONDARY, btn_state(i, en[i]));
        uint32_t ic = en[i] ? COL_TEXT : COL_TEXT_DIM;
        if (i == 2) draw_refresh_icon(bx, by, bw, bh, ic);
        else if (i == 3) draw_home_icon(bx, by, bw, bh, ic);
    }

    // Address bar: rounded field with a focus ring, TTF text and a caret.
    int fr = gui_active_style().radius;
    gui_fill_rounded(window_handle, URL_BAR_X, URL_BAR_Y, URL_BAR_W, URL_BAR_H, fr, COL_FIELD_BG);
    if (url_focused) {
        // Focus ring drawn in the accent color (two passes for a 2px feel).
        gui_rounded_border(window_handle, URL_BAR_X, URL_BAR_Y, URL_BAR_W, URL_BAR_H, fr, COL_ACCENT);
        gui_rounded_border(window_handle, URL_BAR_X - 1, URL_BAR_Y - 1,
                           URL_BAR_W + 2, URL_BAR_H + 2, fr + 1, COL_ACCENT);
    } else {
        gui_rounded_border(window_handle, URL_BAR_X, URL_BAR_Y, URL_BAR_W, URL_BAR_H, fr, COL_FIELD_BORDER);
    }

    // URL text, vertically centered, clipped to the field by the kernel.
    int text_x = URL_BAR_X + 10;
    int text_y = URL_BAR_Y + (URL_BAR_H - 14) / 2;
    if (url_buffer[0]) {
        br_text(window_handle, text_x, text_y, url_buffer, COL_TEXT);
    } else {
        br_text(window_handle, text_x, text_y, "Enter a URL", COL_TEXT_DIM);
    }

    // Caret (only when focused). Approx advance via TTF width of the typed text.
    if (url_focused) {
        char tmp[MAX_URL_LEN];
        int n = url_cursor;
        if (n > MAX_URL_LEN - 1) n = MAX_URL_LEN - 1;
        for (int i = 0; i < n; i++) tmp[i] = url_buffer[i];
        tmp[n] = '\0';
        int caret_x = text_x + gui_ttf_width(tmp, 14);
        if (caret_x < URL_BAR_X + URL_BAR_W - 4) {
            gui_fill_rect(window_handle, caret_x, URL_BAR_Y + 6, 2, URL_BAR_H - 12, COL_TEXT);
        }
    }

    // Go button (accent / primary).
    gui_button(window_handle, GO_BTN_X, GO_BTN_Y, GO_BTN_W, GO_BTN_H, "Go",
               GUI_BTN_PRIMARY, btn_state(4, 1));
}

// Draw the content viewport (parsed page text) inside a themed card.
// ---- Vertical scrollbar (#245) ----
static int scroll_max(void) {
    int view = CONTENT_H - 24;
    int m = g_layout.content_height - view;
    return m > 0 ? m : 0;
}
static int sb_thumb_geo(int *thumb_y_out, int *thumb_h_out) {
    int m = scroll_max(); if (m <= 0) return 0;
    int total = g_layout.content_height; if (total < 1) total = 1;
    int view = CONTENT_H - 24, ty = CONTENT_Y + 1, th = CONTENT_H - 2;
    int thumb_h = (int)((long)th * view / total);
    if (thumb_h < 24) thumb_h = 24;
    if (thumb_h > th) thumb_h = th;
    int thumb_y = ty + (int)((long)(th - thumb_h) * scroll_offset / m);
    *thumb_y_out = thumb_y; *thumb_h_out = thumb_h; return 1;
}
static void draw_scrollbar(void) {
    int thumb_y, thumb_h;
    if (!sb_thumb_geo(&thumb_y, &thumb_h)) return;   // page fits: no bar
    int tx = CONTENT_X + CONTENT_W - SB_W, ty = CONTENT_Y + 1, th = CONTENT_H - 2;
    gui_fill_rect(window_handle, tx, ty, SB_W, th, THEME_SCROLLBAR_BG);
    gui_fill_rect(window_handle, tx, ty, 1, th, COL_SEPARATOR);
    gui_fill_rect(window_handle, tx + 2, thumb_y, SB_W - 4, thumb_h, THEME_SCROLLBAR_THUMB);
    gui_draw_rect_outline(window_handle, tx + 2, thumb_y, SB_W - 4, thumb_h, 0x00808080);
}
static int in_scrollbar(int mx, int my) {
    if (scroll_max() <= 0) return 0;
    int tx = CONTENT_X + CONTENT_W - SB_W;
    return mx >= tx && mx < tx + SB_W && my >= CONTENT_Y && my < CONTENT_Y + CONTENT_H;
}
// Map a cursor Y (while dragging) to a scroll offset. Does NOT redraw (caller does).
static void scrollbar_drag_to(int my) {
    int m = scroll_max(); if (m <= 0) return;
    int ty = CONTENT_Y + 1, th = CONTENT_H - 2;
    int thumb_y, thumb_h; if (!sb_thumb_geo(&thumb_y, &thumb_h)) return;
    int span = th - thumb_h; if (span < 1) span = 1;
    int desired = my - sb_drag_dy - ty;
    int v = (int)((long)desired * m / span);
    if (v < 0) v = 0; if (v > m) v = m;
    scroll_offset = v;
}

static void draw_content(void) {
    // Render the page onto a white "page" canvas (like a real browser) so the
    // document's own colours read correctly regardless of the desktop theme.
    gui_fill_rect(window_handle, CONTENT_X, CONTENT_Y, CONTENT_W, CONTENT_H,
                  0x00FFFFFF);
    gui_draw_rect_outline(window_handle, CONTENT_X, CONTENT_Y,
                          CONTENT_W, CONTENT_H, COL_SEPARATOR);

    int pad_x = CONTENT_X + 12;
    int top   = CONTENT_Y + 10;
    int max_y = CONTENT_Y + CONTENT_H - 14;

    if (!g_have_layout || g_layout.n_items == 0) {
        const char *hint = is_loading ? "Loading..." :
                           "Enter a URL above and press Enter or click Go.";
        br_text(window_handle, pad_x, top, hint, COL_TEXT_DIM);
        return;
    }

    // scroll_offset is in pixels for the NetSurf render path.
    g_link_hit_n = 0;
    g_form_hit_n = 0;
    for (int i = 0; i < g_layout.n_items; i++) {
        layout_item *it = &g_layout.items[i];
        int sy = top + it->y - scroll_offset;
        int ih = (it->kind == 1) ? it->h : it->size;
        if (sy + ih < CONTENT_Y + 6) continue;   // entirely above viewport
        if (sy > max_y) continue;                  // entirely below viewport
        if (it->kind == 3) {
            int bx0 = pad_x + it->x;
            img_entry_t *e = 0;
            for (int k = 0; k < g_img_n; k++) if (g_imgs[k].item == i) { e = &g_imgs[k]; break; }
            if (e && e->px && sy >= CONTENT_Y && sy + e->h <= CONTENT_Y + CONTENT_H) {
                win_draw_image(window_handle, bx0, sy, e->w, e->h, e->px);
            } else if (sy >= CONTENT_Y && sy + it->h <= max_y) {
                gui_fill_rect(window_handle, bx0, sy, it->w, it->h, THEME_SELECTION_BG);
                gui_draw_rect_outline(window_handle, bx0, sy, it->w, it->h, COL_SEPARATOR);
            }
            continue;
        }
        if (it->kind == 1) {
            // Background / border box. Clamp to the content viewport.
            int by0 = sy, by1 = sy + it->h;
            if (by0 < CONTENT_Y + 1) by0 = CONTENT_Y + 1;
            if (by1 > CONTENT_Y + CONTENT_H - 1) by1 = CONTENT_Y + CONTENT_H - 1;
            int bx0 = pad_x + it->x;
            if (it->has_bg && by1 > by0)
                gui_fill_rect(window_handle, bx0, by0, it->w, by1 - by0, it->bg);
            if (it->border_w) {
                if (sy >= CONTENT_Y && sy <= max_y)
                    gui_fill_rect(window_handle, bx0, sy, it->w, it->border_w, it->border_col);
                int bb = sy + it->h - it->border_w;
                if (bb >= CONTENT_Y && bb <= max_y)
                    gui_fill_rect(window_handle, bx0, bb, it->w, it->border_w, it->border_col);
            }
            if (it->form_kind && g_form_hit_n < 64) {
                int fidx = g_form_hit_n;
                form_hit_t *fh = &g_form_hits[g_form_hit_n++];
                fh->x = bx0; fh->y = sy; fh->w = it->w; fh->h = it->h; fh->kind = it->form_kind;
                int k = 0; while (it->field_name[k] && k < 63) { fh->name[k] = it->field_name[k]; k++; } fh->name[k] = 0;
                k = 0; while (it->href[k] && k < 255) { fh->action[k] = it->href[k]; k++; } fh->action[k] = 0;
                if (it->form_kind == 1 && fidx == g_focus_field) {
                    if (g_field_val[0] && sy + 4 < max_y)
                        win_draw_text_ttf(window_handle, bx0 + 6, sy + 4, g_field_val, 14, 0x00000000);
                    if (sy >= CONTENT_Y && sy <= max_y)   // focus ring
                        gui_draw_rect_outline(window_handle, bx0, sy, it->w, it->h, COL_ACCENT);
                }
            }
            continue;
        }
        int sx = pad_x + it->x;
        win_draw_text_ttf(window_handle, sx, sy, it->text,
                          it->size, it->color);
        if (it->href[0] && g_link_hit_n < 512) {
            link_hit_t *lh = &g_link_hits[g_link_hit_n++];
            lh->x = sx; lh->y = sy; lh->w = ttf_measure(it->text, it->size);
            lh->h = it->size + 3;
            int k = 0;
            while (it->href[k] && k < 255) { lh->href[k] = it->href[k]; k++; }
            lh->href[k] = 0;
        }
        if (it->underline) {
            int w = ttf_measure(it->text, it->size);
            gui_fill_rect(window_handle, sx, sy + it->size,
                          w, 1, it->color);
        }
        // crude bold: redraw 1px offset
        if (it->bold)
            win_draw_text_ttf(window_handle, sx + 1, sy, it->text,
                              it->size, it->color);
    }
    draw_scrollbar();
}

// Slim status bar pinned to the bottom of the window.
static void draw_status_bar(void) {
    int sy = WIN_HEIGHT - STATUS_H;
    gui_fill_rect(window_handle, 0, sy, WIN_WIDTH, STATUS_H, COL_STATUS_BG);
    gui_fill_rect(window_handle, 0, sy, WIN_WIDTH, 1, COL_SEPARATOR);

    uint32_t ink = last_was_error ? COL_ERROR : COL_TEXT_DIM;
    if (is_loading) ink = COL_ACCENT;

    // Small accent dot as a loading/status indicator.
    gui_fill_circle_aa(window_handle, 9, sy + (STATUS_H - 8) / 2, 8,
                       is_loading ? COL_ACCENT : (last_was_error ? COL_ERROR : COL_TEXT_DIM),
                       COL_STATUS_BG);

    const char *msg = status_msg[0] ? status_msg : "Ready";
    br_text_sz(window_handle, 24, sy + (STATUS_H - 11) / 2, msg, 11, ink);
}

// Redraw the whole window.
static void redraw(void) {
    gui_fill_rect(window_handle, 0, 0, WIN_WIDTH, WIN_HEIGHT, COL_WINDOW_BG);
    draw_toolbar();
    draw_content();
    draw_status_bar();
    win_invalidate(window_handle);
}

// ============================================================================
// History
// ============================================================================

// Push the current url_buffer onto history as a new navigation (truncates any
// forward entries). Skips if it duplicates the current entry.
static void hist_push(const char *url) {
    if (hist_pos >= 0 && str_ncmp(hist[hist_pos], url, MAX_URL_LEN) == 0)
        return;
    int n = hist_pos + 1;
    if (n >= HIST_MAX) {
        // Drop the oldest entry to make room.
        for (int i = 1; i < HIST_MAX; i++)
            str_cpy(hist[i - 1], hist[i]);
        n = HIST_MAX - 1;
    }
    str_cpy(hist[n], url);
    hist_count = n + 1;
    hist_pos = n;
}

// ============================================================================
// Navigation
// ============================================================================

// Fetch whatever is in url_buffer and render it. If record_history is set, the
// URL is pushed as a new history entry.
// Load a deterministic local test page (/TEST.HTML on the boot fs) so the
// render pipeline can be exercised without the network. Triggered when the URL
// is exactly "test" or begins with "file:".
static int load_local_test(void) {
    FILE *fp = fopen("/TEST.HTML", "r");
    if (!fp) return -1;
    int n = (int) fread(content_buffer, 1, MAX_CONTENT - 1, fp);
    fclose(fp);
    if (n <= 0) return -1;
    content_buffer[n] = '\0';
    str_cpy(status_msg, "Done  /TEST.HTML");
    last_was_error = 0;
    render_page(content_buffer, n);
    return 0;
}

// --- async fetch state (#277): the page download runs in the background; the
// main loop polls poll_fetch() so the UI never freezes while loading. ---
static int g_fetch_id = -1;
static int g_fetch_retry = 0;
static int g_fetch_record = 0;

static void poll_fetch(void) {
    if (!is_loading || g_fetch_id < 0) return;
    int status = 0;
    int st = http_fetch_poll(g_fetch_id, &status, 0);
    if (st == 0) return;                       // still downloading
    if (st == 1) {                             // complete
        int n = http_fetch_read(g_fetch_id, content_buffer, MAX_CONTENT - 1);
        g_fetch_id = -1; is_loading = 0;
        if (n < 0) n = 0;
        content_buffer[n] = 0;
        if (status != 200 && status != 301 && status != 302) {
            str_cpy(status_msg, "HTTP ");
            int p = str_len(status_msg);
            status_msg[p++] = '0' + (status / 100) % 10;
            status_msg[p++] = '0' + (status / 10) % 10;
            status_msg[p++] = '0' + status % 10;
            status_msg[p] = 0;
            last_was_error = 1;
        } else {
            str_cpy(status_msg, "Done  ");
            int p = str_len(status_msg);
            for (int i = 0; url_buffer[i] && p < (int)sizeof(status_msg) - 1; i++)
                status_msg[p++] = url_buffer[i];
            status_msg[p] = 0;
            last_was_error = 0;
        }
        render_page(content_buffer, n);
        if (g_fetch_record) hist_push(url_buffer);
        url_fresh = 1;
        redraw();
    } else {                                   // error -> one retry, then report
        http_fetch_read(g_fetch_id, content_buffer, 0);   // free the job
        g_fetch_id = -1;
        if (g_fetch_retry < 1) {
            g_fetch_retry++;
            g_fetch_id = http_fetch_start(url_buffer);
            if (g_fetch_id >= 0) return;
        }
        is_loading = 0;
        str_cpy(status_msg, "Fetch failed");
        last_was_error = 1;
        str_cpy(display_buffer, status_msg);
        display_length = str_len(display_buffer);
        redraw();
    }
}

static void navigate(int record_history) {
    scroll_offset = 0;
    display_length = 0;
    is_loading = 1;
    last_was_error = 0;

    str_cpy(status_msg, "Loading...");
    redraw();

    if (str_ncmp(url_buffer, "test", 5) == 0 ||
        (url_buffer[0]=='f'&&url_buffer[1]=='i'&&url_buffer[2]=='l'&&url_buffer[3]=='e')) {
        int lr = load_local_test();
        is_loading = 0;
        if (lr == 0) { if (record_history) hist_push(url_buffer); redraw(); return; }
        str_cpy(status_msg, "TEST.HTML not found");
        last_was_error = 1;
        redraw();
        return;
    }

    // If the user typed a bare host (e.g. "google.com") with no scheme,
    // default to https:// like a real browser, and reflect it in the bar.
    {
        int has_scheme = 0;
        for (int i = 0; url_buffer[i] && url_buffer[i+1] && url_buffer[i+2]; i++) {
            if (url_buffer[i] == ':' && url_buffer[i+1] == '/' && url_buffer[i+2] == '/') {
                has_scheme = 1; break;
            }
        }
        if (!has_scheme && url_buffer[0]) {
            char tmp[MAX_URL_LEN];
            const char *pfx = "https://";
            int p = 0;
            for (int i = 0; pfx[i] && p < MAX_URL_LEN - 1; i++) tmp[p++] = pfx[i];
            for (int i = 0; url_buffer[i] && p < MAX_URL_LEN - 1; i++) tmp[p++] = url_buffer[i];
            tmp[p] = 0;
            str_cpy(url_buffer, tmp);
            url_cursor = str_len(url_buffer);
        }
    }

    // Kick off a background (non-blocking) fetch and return immediately so the
    // address bar, scrolling and Stop stay responsive while the page loads.
    // poll_fetch() in the main loop finishes it. (#277)
    g_fetch_record = record_history;
    g_fetch_retry = 0;
    g_fetch_id = http_fetch_start(url_buffer);
    if (g_fetch_id < 0) {
        // Fallback: no free job slot / worker spawn failed -> do it inline.
        int ret = fetch_page(url_buffer);
        if (ret < 0) ret = fetch_page(url_buffer);
        is_loading = 0;
        if (ret < 0) {
            str_cpy(display_buffer, status_msg);
            display_length = str_len(display_buffer);
        } else if (record_history) {
            hist_push(url_buffer);
        }
        url_fresh = 1;
    }
    redraw();
}

static void load_current_url(void) {
    navigate(1);
}

// Resolve a (possibly relative) href against the current url_buffer and navigate.
static void open_link(const char *href) {
    if (!href || !href[0] || href[0] == '#') return;   // empty / in-page anchor
    char out[MAX_URL_LEN];
    int n = 0;
    if (str_ncmp(href, "http://", 7) == 0 || str_ncmp(href, "https://", 8) == 0) {
        for (int i = 0; href[i] && n < MAX_URL_LEN - 1; i++) out[n++] = href[i];
    } else if (href[0] == '/' && href[1] == '/') {
        const char *p = "https:";
        for (int i = 0; p[i] && n < MAX_URL_LEN - 1; i++) out[n++] = p[i];
        for (int i = 0; href[i] && n < MAX_URL_LEN - 1; i++) out[n++] = href[i];
    } else {
        // Find scheme://host (and, for relative paths, the directory) in url_buffer.
        int host_end = 0, dir_end = 0, slashes = 0;
        for (int i = 0; url_buffer[i]; i++) {
            if (url_buffer[i] == '/') {
                slashes++;
                if (slashes <= 2) host_end = i + 1;     // past scheme:// then host start
                if (slashes >= 3) dir_end = i + 1;       // last '/' of the path so far
            }
        }
        // host_end currently points just after the 2nd slash (into host); advance to
        // the slash that ends the host, or end of string.
        int he = 0; slashes = 0;
        for (int i = 0; url_buffer[i]; i++) {
            if (url_buffer[i] == '/') { slashes++; if (slashes == 3) { he = i; break; } }
        }
        if (he == 0) he = (int)str_len(url_buffer);     // no path -> whole string is scheme://host
        if (href[0] == '/') {
            // host-relative: scheme://host + href
            for (int i = 0; i < he && n < MAX_URL_LEN - 1; i++) out[n++] = url_buffer[i];
            for (int i = 0; href[i] && n < MAX_URL_LEN - 1; i++) out[n++] = href[i];
        } else {
            // relative path: scheme://host + dir + href
            int upto = dir_end > he ? dir_end : he;
            for (int i = 0; i < upto && n < MAX_URL_LEN - 1; i++) out[n++] = url_buffer[i];
            if (n == 0 || out[n-1] != '/') { if (n < MAX_URL_LEN - 1) out[n++] = '/'; }
            for (int i = 0; href[i] && n < MAX_URL_LEN - 1; i++) out[n++] = href[i];
        }
    }
    out[n] = 0;
    str_cpy(url_buffer, out);
    url_cursor = (int)str_len(url_buffer);
    url_fresh = 1;
    navigate(1);
}

// Build action?name=value (URL-encoded) and navigate. Empty action => current URL.
static void submit_form(const char *action, const char *name, const char *val) {
    char q[700]; int n = 0;
    const char *base = (action && action[0]) ? action : url_buffer;
    for (int i = 0; base[i] && n < 480; i++) { if (base[i] == '?') break; q[n++] = base[i]; }
    if (n < 480) q[n++] = '?';
    for (int i = 0; name && name[i] && n < 540; i++) q[n++] = name[i];
    if (n < 540) q[n++] = '=';
    for (int i = 0; val[i] && n < 690; i++) {
        char c = val[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c=='-'||c=='_'||c=='.'||c=='~') q[n++] = c;
        else if (c == ' ') q[n++] = '+';
        else { const char *h = "0123456789ABCDEF";
               if (n < 688) { q[n++]='%'; q[n++]=h[(c>>4)&15]; q[n++]=h[c&15]; } }
    }
    q[n] = 0;
    open_link(q);
}

// Resolve an href against the current URL into out[MAX_URL_LEN] WITHOUT navigating
// (same rules as open_link). Used for inline image src resolution.
static void resolve_url(const char *href, char *out) {
    int n = 0;
    if (!href) { out[0] = 0; return; }
    if (str_ncmp(href, "http://", 7) == 0 || str_ncmp(href, "https://", 8) == 0) {
        for (int i = 0; href[i] && n < MAX_URL_LEN - 1; i++) out[n++] = href[i];
    } else if (href[0] == '/' && href[1] == '/') {
        const char *p = "https:";
        for (int i = 0; p[i] && n < MAX_URL_LEN - 1; i++) out[n++] = p[i];
        for (int i = 0; href[i] && n < MAX_URL_LEN - 1; i++) out[n++] = href[i];
    } else {
        int he = 0, slashes = 0;
        for (int i = 0; url_buffer[i]; i++) {
            if (url_buffer[i] == '/') { slashes++; if (slashes == 3) { he = i; break; } }
        }
        if (he == 0) he = (int) str_len(url_buffer);
        if (href[0] == '/') {
            for (int i = 0; i < he && n < MAX_URL_LEN - 1; i++) out[n++] = url_buffer[i];
            for (int i = 0; href[i] && n < MAX_URL_LEN - 1; i++) out[n++] = href[i];
        } else {
            int dir_end = 0, sl = 0;
            for (int i = 0; url_buffer[i]; i++) {
                if (url_buffer[i] == '/') { sl++; if (sl >= 3) dir_end = i + 1; }
            }
            int upto = dir_end > he ? dir_end : he;
            for (int i = 0; i < upto && n < MAX_URL_LEN - 1; i++) out[n++] = url_buffer[i];
            if (n == 0 || out[n-1] != '/') { if (n < MAX_URL_LEN - 1) out[n++] = '/'; }
            for (int i = 0; href[i] && n < MAX_URL_LEN - 1; i++) out[n++] = href[i];
        }
    }
    out[n] = 0;
}

static void free_images(void) {
    for (int i = 0; i < g_img_n; i++) if (g_imgs[i].px) free(g_imgs[i].px);
    g_img_n = 0;
}

// Fetch + decode (point-sampled to the box) every <img> in the current layout.
// Begin async image loading for the freshly laid-out page: drop old images,
// cancel any in-flight job, and arm the scan. poll_images() does the work.
static void images_begin(void) {
    free_images();
    if (g_img_job >= 0) { http_fetch_read(g_img_job, (char *) g_imgfetch, 0); g_img_job = -1; }
    g_img_scan = 0;
    g_img_item = -1;
}

// Drive async image loading: at most one transition per call. Poll the active
// fetch (decode + store + redraw on completion), else start the next pending
// <img>. Called every main-loop tick so the UI stays live while images download.
static void poll_images(void) {
    if (g_img_job >= 0) {
        int status = 0;
        int st = http_fetch_poll(g_img_job, &status, 0);
        if (st == 0) return;                       // still downloading
        if (st == 1 && g_img_item >= 0 && g_img_item < g_layout.n_items && g_img_n < IMG_MAX) {
            int n = http_fetch_read(g_img_job, (char *) g_imgfetch, sizeof(g_imgfetch) - 1);
            g_img_job = -1;
            if (n > 0 && status == 200) {
                layout_item *it = &g_layout.items[g_img_item];
                int cap = it->w * it->h * 4;
                if (cap > 0) {
                    uint32_t *px = (uint32_t *) malloc(cap);
                    if (px) {
                        int dims[2] = {0, 0};
                        int dn = decode_image(g_imgfetch, (unsigned) n, it->w, it->h, px, cap, dims);
                        if (dn > 0 && dims[0] > 0 && dims[1] > 0) {
                            g_imgs[g_img_n].item = g_img_item; g_imgs[g_img_n].w = dims[0];
                            g_imgs[g_img_n].h = dims[1]; g_imgs[g_img_n].px = px;
                            g_img_n++;
                            redraw();              // show the image as it lands
                        } else {
                            free(px);
                        }
                    }
                }
            }
        } else {
            http_fetch_read(g_img_job, (char *) g_imgfetch, 0);   // free the job
            g_img_job = -1;
        }
        g_img_item = -1;
        return;                                    // one transition per tick
    }

    if (g_img_scan < 0) return;
    while (g_img_scan < g_layout.n_items && g_img_n < IMG_MAX) {
        int i = g_img_scan++;
        layout_item *it = &g_layout.items[i];
        if (it->kind != 3 || !it->href[0]) continue;
        char url[MAX_URL_LEN];
        resolve_url(it->href, url);
        if (!url[0]) continue;
        int job = http_fetch_start(url);
        if (job < 0) { g_img_scan = i; return; }   // fetch table full: retry next tick
        g_img_job = job;
        g_img_item = i;
        return;
    }
    g_img_scan = -1;                               // all images scanned
}

static void go_back(void) {
    if (!can_go_back()) return;
    hist_pos--;
    str_cpy(url_buffer, hist[hist_pos]);
    url_cursor = str_len(url_buffer);
    navigate(0);
}

static void go_forward(void) {
    if (!can_go_forward()) return;
    hist_pos++;
    str_cpy(url_buffer, hist[hist_pos]);
    url_cursor = str_len(url_buffer);
    navigate(0);
}

static void go_home(void) {
    str_cpy(url_buffer, HOME_URL);
    url_cursor = str_len(url_buffer);
    navigate(1);
}

// ============================================================================
// Event handlers
// ============================================================================

static void handle_key(gui_event_t *event) {
    char c = event->key_char;

    // A focused form text field captures typing / backspace / Enter(submit).
    if (g_focus_field >= 0 && g_focus_field < g_form_hit_n &&
        g_form_hits[g_focus_field].kind == 1) {
        if (event->keycode == 0x1C || c == (char)0x0D || c == (char)0x0A) {
            submit_form(g_form_hits[g_focus_field].action,
                        g_form_hits[g_focus_field].name, g_field_val);
            return;
        }
        if (c == (char)0x08 || event->keycode == 0x0E) {
            if (g_field_len > 0) g_field_val[--g_field_len] = 0;
            draw_content(); win_invalidate(window_handle);
            return;
        }
        if (c >= 32 && c < 127) {
            if (g_field_len < (int)sizeof(g_field_val) - 1) {
                g_field_val[g_field_len++] = c; g_field_val[g_field_len] = 0;
            }
            draw_content(); win_invalidate(window_handle);
            return;
        }
    }

    // Page scrolling via keyboard (Up/Down/PageUp/PageDown/Home/End). These are
    // non-printable keys, so they apply regardless of address-bar focus and let
    // the user reach below-the-fold content (e.g. footer images).
    {
        int m = scroll_max();
        int page = (CONTENT_H - 24) - 20; if (page < 20) page = 20;
        int handled_scroll = 1;
        switch (event->keycode) {
            case 0x80: scroll_offset -= 40; break;     // Up
            case 0x81: scroll_offset += 40; break;     // Down
            case 0x49: scroll_offset -= page; break;   // Page Up
            case 0x51: scroll_offset += page; break;   // Page Down
            case 0x47: scroll_offset = 0; break;       // Home
            case 0x4F: scroll_offset = m; break;       // End
            default: handled_scroll = 0; break;
        }
        if (handled_scroll) {
            if (scroll_offset < 0) scroll_offset = 0;
            if (scroll_offset > m) scroll_offset = m;
            draw_content(); win_invalidate(window_handle);
            return;
        }
    }

    // Address-bar editing: caret-aware insert/delete/move via the shared
    // textfield helper. url_cursor is the true caret index into url_buffer.
    if (event->keycode == 0x1C || c == (char)0x0D || c == (char)0x0A) {
        // Enter: navigate.
        load_current_url();
        return;
    }

    {
        textfield_t tf;
        // A fresh URL (just navigated): the next printable keystroke replaces
        // the whole field. Movement keys keep the existing text.
        if (url_fresh && c >= 32 && c < 127) {
            url_buffer[0] = '\0';
            url_cursor = 0;
            url_fresh = 0;
        }
        tf_attach(&tf, url_buffer, MAX_URL_LEN, (int)str_len(url_buffer), url_cursor);
        if (tf_handle_key(&tf, event)) {
            url_cursor = tf.cursor;
            url_fresh  = 0;
            url_focused = 1;
            draw_toolbar();
            win_invalidate(window_handle);
        }
    }
}

static int point_in(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

// Which toolbar button (if any) is under the pointer. Returns -1 for none.
static int btn_at(int mx, int my) {
    for (int i = 0; i <= 4; i++) {
        int bx, by, bw, bh;
        nav_btn_rect(i, &bx, &by, &bw, &bh);
        if (point_in(mx, my, bx, by, bw, bh)) return i;
    }
    return -1;
}

static void handle_mouse_move(gui_event_t *event) {
    if (sb_dragging) { scrollbar_drag_to(event->mouse_y); redraw(); return; }
    int idx = btn_at(event->mouse_x, event->mouse_y);
    if (idx != btn_hover) {
        btn_hover = idx;
        draw_toolbar();
        win_invalidate(window_handle);
    }
}

static void handle_mouse_down(gui_event_t *event) {
    if (in_scrollbar(event->mouse_x, event->mouse_y)) {
        int thumb_y, thumb_h, my = event->mouse_y;
        if (sb_thumb_geo(&thumb_y, &thumb_h)) {
            if (my >= thumb_y && my < thumb_y + thumb_h) {
                sb_dragging = 1; sb_drag_dy = my - thumb_y;
            } else {
                int m = scroll_max(), page = (CONTENT_H - 24) - 20; if (page < 20) page = 20;
                scroll_offset += (my < thumb_y) ? -page : page;
                if (scroll_offset < 0) scroll_offset = 0;
                if (scroll_offset > m) scroll_offset = m;
                redraw();
            }
        }
        return;
    }
    int idx = btn_at(event->mouse_x, event->mouse_y);
    if (idx >= 0) {
        btn_press = idx;
        draw_toolbar();
        win_invalidate(window_handle);
    }
}

static void handle_mouse_up(gui_event_t *event) {
    if (sb_dragging) { sb_dragging = 0; return; }
    int mx = event->mouse_x;
    int my = event->mouse_y;
    int pressed = btn_press;
    btn_press = -1;

    int idx = btn_at(mx, my);

    // Click registers only if release lands on the same button it pressed.
    if (idx >= 0 && idx == pressed) {
        switch (idx) {
            case 0: go_back();    return;
            case 1: go_forward(); return;
            case 2: load_current_url(); return;  // Reload
            case 3: go_home();    return;
            case 4: load_current_url(); return;  // Go
        }
    }

    // Content form-control click: focus a text field, or submit.
    if (pressed < 0 && my >= CONTENT_Y) {
        for (int i = 0; i < g_form_hit_n; i++) {
            form_hit_t *fh = &g_form_hits[i];
            if (mx >= fh->x && mx < fh->x + fh->w && my >= fh->y && my < fh->y + fh->h) {
                if (fh->kind == 2) {
                    int fld = (g_focus_field >= 0 && g_focus_field < g_form_hit_n &&
                               g_form_hits[g_focus_field].kind == 1) ? g_focus_field : -1;
                    if (fld < 0) for (int j = 0; j < g_form_hit_n; j++)
                        if (g_form_hits[j].kind == 1) { fld = j; break; }
                    const char *act = fh->action[0] ? fh->action :
                                      (fld >= 0 ? g_form_hits[fld].action : "");
                    const char *nm  = (fld >= 0) ? g_form_hits[fld].name : "q";
                    submit_form(act, nm, g_field_val);
                } else {
                    g_focus_field = i; url_focused = 0;
                    g_field_val[0] = 0; g_field_len = 0;
                    draw_content(); win_invalidate(window_handle);
                }
                return;
            }
        }
    }

    // Content hyperlink click (only if no toolbar button was the press target).
    if (pressed < 0 && my >= CONTENT_Y) {
        for (int i = 0; i < g_link_hit_n; i++) {
            link_hit_t *lh = &g_link_hits[i];
            if (mx >= lh->x && mx < lh->x + lh->w &&
                my >= lh->y - 2 && my < lh->y + lh->h) {
                open_link(lh->href);
                return;
            }
        }
    }

    // Address bar focus toggle.
    int was_focused = url_focused;
    if (point_in(mx, my, URL_BAR_X, URL_BAR_Y, URL_BAR_W, URL_BAR_H)) {
        url_focused = 1;
        url_fresh = 1;   // click-to-edit selects all: first key replaces the URL
    }
    if (!point_in(mx, my, URL_BAR_X, URL_BAR_Y, URL_BAR_W, URL_BAR_H) &&
        my < TOOLBAR_H && idx < 0) {
        url_focused = 0;
    }
    if (url_focused != was_focused || idx != pressed) {
        draw_toolbar();
        win_invalidate(window_handle);
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Adopt the running kernel theme so the browser matches Settings/Files.
    apply_theme(get_theme());

    window_handle = win_create("Browser", 100, 100, WIN_WIDTH, WIN_HEIGHT);
    if (window_handle < 0) return 1;

    redraw();

    // If a local /TEST.HTML is present, auto-load it on startup so the render
    // pipeline is exercised deterministically (no network needed). Harmless on
    // systems without the file: navigate() falls back to the error message.
    // Load the home page on startup via the real fetch+render pipeline.
    // (Type "test" in the address bar to load the local /TEST.HTML fixture.)
    // Dev: if a local /TEST.HTML fixture exists, open it instead of the
    // home page (lets the render+JS pipeline be exercised offline).
    // Dev/headless: /STARTURL.TXT (first line) overrides the startup URL.
    { int sfd = open("/STARTURL.TXT", 0); if (sfd >= 0) { char ub[512]; long rn = read(sfd, ub, 511);
        close(sfd); if (rn > 0) { int k = 0; while (k < rn && ub[k] != '\n' && ub[k] != '\r') k++;
            ub[k] = 0; if (k > 0) { str_cpy(url_buffer, ub); url_cursor = k; } } } }
    { FILE *tf = fopen("/TEST.HTML", "r"); if (tf) { fclose(tf); str_cpy(url_buffer, "test"); url_cursor = 4; } }
    navigate(1);

    gui_event_t event;

    while (running) {
        poll_fetch();
        poll_images();
        int event_type = win_get_event(window_handle, &event, 50);
        if (event_type < 0) continue;

        switch (event.type) {
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_REDRAW:
                redraw();
                break;

            case EVENT_KEY_DOWN:
                handle_key(&event);
                break;

            case EVENT_MOUSE_MOVE:
                handle_mouse_move(&event);
                break;

            case EVENT_MOUSE_DOWN:
                handle_mouse_down(&event);
                break;

            case EVENT_MOUSE_UP:
                handle_mouse_up(&event);
                break;

            case EVENT_MOUSE_SCROLL: {
                scroll_offset += event.scroll_delta * 24; // pixels per notch
                if (scroll_offset < 0) scroll_offset = 0;
                int maxs = g_layout.content_height -
                           (CONTENT_H - 24);
                if (maxs < 0) maxs = 0;
                if (scroll_offset > maxs) scroll_offset = maxs;
                redraw();
                break;
            }

            case EVENT_RESIZE:
                // #89: reflow to the new window size. Re-layout the page only
                // when the WIDTH changed (height-only resize just needs a
                // redraw + scroll clamp), so vertical drags don't re-parse.
                if (event.mouse_x > 0 && event.mouse_y > 0) {
                    int new_w = event.mouse_x, new_h = event.mouse_y;
                    g_win_w = new_w;
                    g_win_h = new_h;
                    if (g_have_layout && g_content_len > 0 && new_w != last_layout_w)
                        render_page(content_buffer, g_content_len);
                    int maxs = scroll_max();
                    if (scroll_offset > maxs) scroll_offset = maxs;
                    if (scroll_offset < 0) scroll_offset = 0;
                    redraw();
                }
                break;

            default:
                break;
        }
    }

    win_destroy(window_handle);
    return 0;
}
