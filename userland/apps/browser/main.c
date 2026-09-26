// browser - Web Browser for MayteraOS (userland version)
// Fetches and renders web pages via SYS_HTTP_FETCH syscall.
//
// [no-ticket] (browser-glass): chrome restyled into the shared dark-teal glass
// language already shipped in the first-run wizard, App Repo and Task Manager
// (docs/UI_GLASS_DESIGN_SYSTEM.md, section 10's "regardless of the active
// theme" pattern, same as App Repo's #glassrepo override). Existing nav/
// address/Go geometry is unchanged; a tab strip and a bookmarks bar are added
// above and below the toolbar, and a Settings (overflow) button is added next
// to Go. Page parsing and fetch behavior are unchanged.
//
// (browserglass) Second pass: the flat dark-teal bands above became the REAL
// glass the seven glassed apps share (Task Manager, Calculator, Image Viewer,
// Media Player, Editor, Terminal, Files): a frosted-wallpaper backdrop in a
// PAD 10 margin (userland/libc gui_glass_backdrop_sync/_blit/_at, the shared
// recipe, not a private copy), tab pills ON the backdrop, and ONE rounded
// glass panel (PANEL_R 12, inset PANEL_IN 12) that holds the toolbar row, the
// bookmarks row, the progress line, the page viewport and the status row.
// The PAGE VIEWPORT is untouched: an opaque page_bg() sheet the engine paints
// into exactly as before (draw_content() is not edited), so live content
// never sits on a blurred backdrop. Designed first as an HTML/CSS spec
// (the build host:/root/browserglass-proof/browserglass-spec.html), then ported.

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/syscall.h"
#include "../../libc/theme.h"
#include "../../libc/gui_scroll.h"   // shared scrollbar contrast rule (#745 item 77)
#include "../../libc/gui_style.h"    // gui_fill_rounded_aa / gui_soft_shadow / gui_glass_backdrop_* (browserglass)

// NetSurf-backed render pipeline (hubbub->libdom parse, libcss style, our
// MIT block+inline layout). See <workspace>
#include "dom_hubbub_bind.h"
#include "css_select_bind.h"
#include "cssvar.h"   // #245 enggrad: gradient table (parsed at CSS preprocess time)
#include <math.h>     // #245 enggrad: sin/cos/sqrt for the colour ramp (userland SSE2 float)
#include "layout.h"
#include "fontmap.h"
#include "rrect.h"
#include "duk_dom.h"  // Duktape JS + DOM binding (Phase 2)
#include "../../libc/keys.h"   // #243: GUI_KEY_* nav codes

// Route in-window text through the antialiased TrueType path (matches Settings).
#define br_text(h, x, y, s, c)       win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define br_text_sz(h, x, y, s, sz, c) win_draw_text_ttf((h), (x), (y), (s), (sz), (c))

// ============================================================================
// Layout / window dimensions
// ============================================================================

// #89: live window size, updated on EVENT_RESIZE so the whole UI (toolbar,
// content viewport, status bar) and the page layout width reflow on resize.
// [no-ticket] (browser-glass): height raised 600->664 (+64, the tab strip
// (34) plus bookmarks bar (28) plus progress line (2) added below) so the
// content viewport keeps the same size it had before this restyle rather
// than shrinking by the height of the new chrome.
static int g_win_w = 800, g_win_h = 664;
#define WIN_WIDTH       g_win_w
#define WIN_HEIGHT      g_win_h

// (browserglass) Geometry. One definition each; draw and hit-test both read
// the rect functions / macros below (glass doc section 8). PAD / TAB_H /
// TAB_GAP / PANEL_R / PANEL_IN and the panel top (PAD + TAB_H + 10) are the
// Task Manager's, the Calculator's and the Image Viewer's, so the glassed
// windows share one geometry. Top to bottom: tab pills on the backdrop, then
// ONE glass panel holding the toolbar row, the bookmarks row, the progress
// line, the page viewport and the status row, each inset PANEL_IN from the
// panel edge (PANEL_IN >= PANEL_R, so no band ever touches a rounded corner).
#define PAD             10                    // window margin: the backdrop shows here
#define TAB_H           26                    // tab pill height (radius TAB_H/2)
#define TAB_GAP         6                     // gap between pills
#define PANEL_Y         (PAD + TAB_H + 10)    // panel top (46)
#define PANEL_R         12                    // panel corner radius
#define PANEL_IN        12                    // inset from the panel edge to its content
#define ROW_H           28                    // toolbar row: pills, address field, Go
#define INNER_X         (PAD + PANEL_IN)      // left edge of everything inside the panel (22)
#define INNER_W         (WIN_WIDTH - 2 * INNER_X)

// The tab strip BAND for hit-testing: everything above the panel.
#define TABSTRIP_H      (PANEL_Y)
#define TOOLBAR_Y       (PANEL_Y + PANEL_IN)  // 58
#define TOOLBAR_H       ROW_H
#define BOOKMARKS_H     20
#define PROGRESS_H      2
#define STATUS_H        18

// Navigation pills live on the left of the toolbar row.
#define NAV_BTN_W       36
#define NAV_BTN_H       ROW_H
#define NAV_BTN_Y       (TOOLBAR_Y)
#define NAV_GAP         6
#define TOOLBAR_PAD     INNER_X

// Four nav pills: Back, Forward, Reload, Home.
#define NAV_BACK_X      (TOOLBAR_PAD)
#define NAV_FWD_X       (NAV_BACK_X + NAV_BTN_W + NAV_GAP)
#define NAV_RELOAD_X    (NAV_FWD_X  + NAV_BTN_W + NAV_GAP)
#define NAV_HOME_X      (NAV_RELOAD_X + NAV_BTN_W + NAV_GAP)

// Go button on the right of the toolbar row.
#define GO_BTN_W        50
#define GO_BTN_H        ROW_H
#define GO_BTN_Y        (TOOLBAR_Y)
#define GO_BTN_X        (WIN_WIDTH - INNER_X - GO_BTN_W)

// Settings (overflow) pill: the three-dot control, just left of Go. Opens a
// small glass menu (Home, Reconnect, About).
#define SETTINGS_BTN_W  30
#define SETTINGS_BTN_H  ROW_H
#define SETTINGS_BTN_Y  (TOOLBAR_Y)
#define SETTINGS_BTN_X  (GO_BTN_X - 8 - SETTINGS_BTN_W)

// Address field fills the gap between the nav pills and the Settings pill.
#define URL_BAR_X       (NAV_HOME_X + NAV_BTN_W + 10)
#define URL_BAR_Y       (TOOLBAR_Y)
#define URL_BAR_W       (SETTINGS_BTN_X - 8 - URL_BAR_X)
#define URL_BAR_H       ROW_H
// Bookmark star hit-box, inset from the address field's right edge.
#define URL_STAR_W      22

// Bookmarks row: a row of glass chips under the toolbar row.
#define BOOKMARKS_Y     (TOOLBAR_Y + TOOLBAR_H + 8)
// The accent progress line sits between the bookmarks row and the viewport.
#define PROGRESS_Y      (BOOKMARKS_Y + BOOKMARKS_H + 8)
// The status row sits inside the panel, PANEL_IN above its bottom edge.
#define STATUS_Y        (WIN_HEIGHT - PAD - PANEL_IN - STATUS_H)

// Content viewport: the page sheet, inside the panel. Its right edge is the
// panel's inner edge (INNER_X from the window edge), and the scrollbar sits
// flush against THAT edge (gui_scroll_bar_x()'s "right-aligned inside the
// viewport"); the old "viewport right edge IS the window right edge" rule
// predates the panel and no longer applies.
#define CONTENT_X       INNER_X
#define CONTENT_Y       (PROGRESS_Y + PROGRESS_H + 4)
#define CONTENT_W       INNER_W
#define CONTENT_H       (STATUS_Y - 6 - CONTENT_Y)
#define SB_W            12   // vertical scrollbar width (#245)

#define HOME_URL        "https://maytera.net"

// #25: mirrors kernel/net/http_progress.h's http_phase_t. Userland has no
// access to that kernel header, so the values are restated here; they are
// part of the SYS_HTTP_FETCH_PROGRESS ABI and do not change independently.
#define HTTP_PHASE_IDLE       0
#define HTTP_PHASE_RESOLVING  1
#define HTTP_PHASE_CONNECTING 2
#define HTTP_PHASE_TLS        3
#define HTTP_PHASE_SENDING    4
#define HTTP_PHASE_RECEIVING  5
#define HTTP_PHASE_DONE       6
#define HTTP_PHASE_ERROR      7

// ============================================================================
// Theme palette (mirrors the Settings app approach)
// ============================================================================

// (browserglass) Glass tokens: docs/UI_GLASS_DESIGN_SYSTEM.md section 1, the
// exact values the Task Manager, Calculator and Image Viewer use (their
// C_PANEL / C_CARD / C_EDGE / C_INK / C_INK_DIM / C_ACCENT / C_ACCENT_INK).
// The COL_ names predate this pass and are kept because dozens of call
// sites read them; the section-1 name is beside each value. Fixed dark glass
// regardless of the active theme: the window carries its own backdrop, so
// there is no light-theme surface for these to sit on.
static uint32_t COL_WINDOW_BG;     // DK_CARD_FILL: pal.surface for the shared widgets
static uint32_t COL_CARD_BG;       // DK_CARD_FILL: the overflow menu's card
static uint32_t COL_SEPARATOR;     // DK_STROKE_UNSEL: panel / pill / viewport 1px border
static uint32_t COL_TEXT;          // DK_HEADLINE
static uint32_t COL_TEXT_DIM;      // DK_BODY
static uint32_t COL_ACCENT;        // DK_ACCENT
static uint32_t COL_FIELD_BG;      // DK_INPUT_FILL
static uint32_t COL_FIELD_BORDER;  // DK_INPUT_BORDER
static uint32_t COL_ERROR;
static uint32_t COL_PANEL;         // WEL_BG_MID: the glass panel, pill fills, the backdrop tint
static uint32_t COL_RAISED;        // hovered row inside the overflow menu
static uint32_t COL_FAINT;         // least emphasis text (inactive tab labels)
static uint32_t COL_ACCENT_INK;    // text on the accent (the active tab pill)
static uint32_t COL_TRACK;         // DK_PROGRESS_TRACK: the progress line's track
static uint32_t COL_EDGE_GLASS;    // DK_EDGE_GLASS: a stroke that floats over the page (overflow menu)
#define WEL_BG_TOP     0x000A1614  // backdrop gradient fallback, top stop
#define WEL_BG_BOTTOM  0x00050A09  // backdrop gradient fallback, bottom stop

// Build the palette and push it into the style engine so all gui_*
// primitives render consistently.
//
// [no-ticket] (browser-glass): this used to branch on the live kernel theme
// (Light/Classic/Ocean/Modern Dark/Dark) and pick one of five palettes. The
// owner-approved glass redesign asks for the SAME dark-teal glass chrome the
// first-run wizard, App Repo and Task Manager already ship REGARDLESS of the
// active desktop theme - the exact override App Repo's setup_palette()
// applies (userland/apps/appstore/main.c, "#glassrepo": "the owner wants
// this store to read like the first-run wizard's glass regardless of the
// active theme"). So the browser now does the same thing: one fixed palette,
// not five theme-derived ones. `kernel_theme` is kept as a parameter (still
// passed get_theme() at the call site) only so a future per-theme carve-out
// does not require touching every caller.
static void apply_theme(int kernel_theme) {
    (void) kernel_theme;

    COL_WINDOW_BG    = 0x000E1D1B;   // DK_CARD_FILL
    COL_PANEL        = 0x00122420;   // WEL_BG_MID (browserglass: was the flat band's 0x16302A)
    COL_CARD_BG      = 0x000E1D1B;   // DK_CARD_FILL
    COL_RAISED       = 0x001B3A33;   // hovered menu row
    COL_SEPARATOR    = 0x002C4A44;   // DK_STROKE_UNSEL
    COL_TEXT         = 0x00F3FBF9;   // DK_HEADLINE
    COL_TEXT_DIM     = 0x00A9D9CC;   // DK_BODY
    COL_FAINT        = 0x006E9E92;   // faint
    COL_ACCENT       = 0x006AE2CF;   // DK_ACCENT
    COL_FIELD_BG     = 0x00213B34;   // DK_INPUT_FILL (address field; browserglass, was the card token)
    COL_FIELD_BORDER = 0x004E7168;   // DK_INPUT_BORDER (resting; focus is the accent)
    COL_ACCENT_INK   = 0x0004231A;
    COL_TRACK        = 0x0016241F;   // DK_PROGRESS_TRACK
    COL_EDGE_GLASS   = 0x006FA99E;   // DK_EDGE_GLASS
    // Semantic: kept as its own danger tone, never recolored to fit the
    // palette (an insecure/error state must stay legible as "different").
    COL_ERROR        = 0x00E2795A;

    gui_set_style(GUI_STYLE_MODERN);

    gui_palette_t pal;
    pal.surface        = COL_WINDOW_BG;
    pal.surface_raised = COL_CARD_BG;
    pal.ink            = COL_TEXT;
    pal.ink_dim        = COL_TEXT_DIM;
    pal.accent         = COL_ACCENT;
    pal.accent_hover   = gui_lighten(COL_ACCENT, 18);
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
static int  url_cursor = (int)(sizeof(HOME_URL) - 1);   // length of HOME_URL
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
// #245 enggrad: rendered-gradient cache. A kind-4 layout item's colour ramp is
// rasterised once into a BGRA buffer (win_draw_image order == 0x00RRGGBB) and
// reused across redraws/scroll, mirroring the decoded-image cache above. Keyed
// by (layout item, w, h) so a resize re-rasterises at the new box size.
#define GRAD_MAX 16
typedef struct { int item; int w, h; uint32_t *px; } grad_entry_t;
static grad_entry_t g_gradc[GRAD_MAX];
static int g_gradc_n = 0;
static void grad_cache_free(void);
static uint32_t *grad_cache_get(int item, int w, int h, int gidx);
static void resolve_url(const char *href, char *out);
static int  fetch_progress_text(char *out, int cap);   // #25
// [no-ticket] (browser-glass): defined in the Tabs/Bookmarks sections below,
// used by draw_tabstrip()/draw_toolbar() above them.
static void tab_label(const char *url, char *out, int cap);
static int  bookmark_find(const char *url);
// Defined alongside the fetch-progress helpers below (needs g_fetch_phase
// etc.), called from redraw() above them.
static void draw_progress_line(void);
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
// engscroll (#245): per-overflow-container vertical scroll offsets, keyed by
// the block-close-order index layout reports in g_layout.scrolls[]. g_active_scroll
// is the container the keyboard drives. All zero for a page with no scroll box,
// so the whole feature is inert (AE=0) unless a page actually has one.
static int  g_box_scroll[LAYOUT_MAX_SCROLLS];
// brhscroll (#245): per-overflow-container HORIZONTAL scroll offsets, keyed by
// the same block-close-order index as g_box_scroll. All zero for a page with no
// horizontally-overflowing box, so the feature is inert (AE=0).
static int  g_box_scroll_x[LAYOUT_MAX_SCROLLS];
static int  g_active_scroll = 0;
// engscroll (#245): when /BRDUMP.TXT exists (a throwaway-image harness marker,
// same class as /TEST.HTML), dump box geometry + scroll descriptors to serial
// after each layout. Absent on a normal boot, so this is inert (AE=0).
static int  g_brdump = 0;

// #245 engflexclose: cssvar.c's 47-case self-test had ZERO CALLERS and had
// never executed inside MayteraOS. Its cases were only ever run by compiling
// cssvar.c natively on the build host, which tests the HOST's compiler, libc,
// malloc and int widths, not the freestanding userland the browser actually
// runs in. This project keeps finding that class of defect (a gate that never
// ran, an increment script that did not increment, an assert that was
// documented for a month before it was written), so the suite is wired to a
// hook here rather than left as a thing someone could run.
//
// THE MECHANISM IS THE ONE THIS APP ALREADY HAS, not a new one. The detail
// lines ride the SAME /BRDUMP.TXT marker g_brdump above gates, which the
// verification harness already creates on the throwaway image; nothing new
// has to be staged for the per-case output to appear.
//
// THE ONE-LINE SUMMARY IS UNCONDITIONAL, which is the part that makes this a
// test rather than a debug facility: every launch of the browser on a
// shipping image proves on serial that the 47 cases pass ON THE TARGET. The
// cost is 47 cssvar_preprocess() calls over CSS strings of a few dozen bytes,
// once, before the first page is fetched.
//
// The tag is [CSSVAR], deliberately NOT [BRDUMP], so the layout-dump
// extraction the engine harness greps for is untouched.
static int  g_cv_cases  = 0;   // cases the suite actually reported
static int  g_cv_detail = 0;   // print each case (set from g_brdump)
static void cssvar_report(const char *line)
{
    g_cv_cases++;
    if (g_cv_detail) printf("[CSSVAR] %s\n", line);
}
static int  g_content_len = 0;   // #89: length of the page in content_buffer (for re-layout on resize)
static int  last_layout_w = 0;   // #89: window width the page was last laid out at
// #resizereflow: the current page's PARSED DOM and BUILT CSSOM, kept alive so a
// resize can re-run LAYOUT ONLY against them. A resize changes the viewport
// WIDTH; the DOM is already parsed, the stylesheets already fetched + parsed,
// the inline JS already run. Rebuilding any of that on resize was the 30s stall
// the owner saw (the CSS re-fetch, synchronous, over a slow network). These are
// borrowed: render_page intentionally leaks one page's structures per
// navigation (the teardown path is not trusted), so we simply overwrite the
// pointers on each new load and reuse them on resize.
static mdb_parser *g_doc_parser = 0;
static mcs_ctx    *g_css_ctx    = 0;
static int  sb_dragging = 0;   // dragging the scrollbar thumb
static int  sb_drag_dy = 0;    // cursor offset within the thumb at grab

// Status message.
static char status_msg[160] = "Ready";



// ---------------------------------------------------------------------------
// #549 CONNECTIVITY BREAKER: WAIT FOR THE RE-PROBE WINDOW INSTEAD OF GIVING UP.
// (browsenet, 2026-09-01. Owner report on golden 2317: "I can ping 1.1.1.1
// from the terminal but the browser is failing to load example.com".)
//
// MEASURED on the owner's machine, from its own /BOOTLOG.TXT:
//   [NETDIAG] drv=CDC-ECM carrier=1 state=FAULTY cfg=dhcp dhcp=BOUND
//             ip=192.0.2.1 gw=192.0.2.1 gwarp=RESOLVED dns=192.0.2.1
// Link up, lease held, gateway ARP resolved, resolver known - and the kernel's
// connectivity breaker had marked the interface FAULTY. While FAULTY,
// sys_http_fetch_start() refuses every request with NET_ERR_FAULTY (-3) except
// ONE re-probe per 30 SECONDS, shared by every process on the machine. ICMP is
// not gated at all, which is exactly why ping to a literal IP kept working and
// made this look like a DNS fault.
//
// WHAT THIS BROWSER USED TO DO WITH THAT. http_fetch_start() returned -3, the
// code below fell through to two SYNCHRONOUS fetch_page() calls, and those are
// gated on the same breaker, so all three were refused within microseconds of
// each other and the page reported a bare "Fetch failed". The user therefore
// had a window of a few microseconds every 30 seconds in which to click Go, and
// was competing for it with the App Repo and the update service, which take the
// same token. In practice the browser could never load anything again.
//
// A single click is not a retry storm - the storm #549 exists to stop was
// background pollers retrying continuously - so waiting out one 30s window is
// both correct and cheap: one syscall every 2s for at most ~35s, and it stops
// the moment the request is admitted, the page loads, or the user hits Stop.
// It does NOT widen the wire budget: the kernel still admits at most one
// request per 30s. It only stops us throwing away our turn.
#define BR_FAULTY_RETRY_MS   2000    // ask again this often
#define BR_FAULTY_WINDOW_MS  35000   // one full 30s probe interval, plus slack
static unsigned long g_faulty_next_ms;    // when to retry http_fetch_start
static unsigned long g_faulty_until_ms;   // give up after this
static int           g_faulty_wait;       // 1 = waiting out the breaker
static int  is_loading = 0;
static int  last_was_error = 0;

// Simple back/forward history so the nav buttons are functional.
#define HIST_MAX        32
static char hist[HIST_MAX][MAX_URL_LEN];
static int  hist_count = 0;   // number of entries
static int  hist_pos = -1;    // current index into hist

// Hover/press state for toolbar buttons (-1 = none). Indices:
// 0=Back 1=Forward 2=Reload 3=Home 4=Go 5=Settings (overflow)
static int  btn_hover = -1;
static int  btn_press = -1;
// Last known mouse position (window-relative), for the bookmarks-bar chip
// hover highlight in draw_bookmarks_bar().
static int  g_mx_last = -1, g_my_last = -1;

// ============================================================================
// Tabs [no-ticket] (browser-glass)
//
// SCOPE, STATED HONESTLY: the render pipeline (content_buffer, the NetSurf
// DOM/CSS parse trees g_doc_parser/g_css_ctx, the layout item list g_layout,
// the image cache g_imgs[]) is a SET OF SINGLETONS - there is exactly one
// live document at a time, by construction (see the #resizereflow comment on
// g_doc_parser above: "render_page intentionally leaks one page's structures
// per navigation"). Duplicating all of that per tab would multiply the
// browser's static memory footprint by MAX_TABS for buffers that are already
// 1MB+2MB+1.5MB apiece, and is a separate, larger change than this restyle.
//
// So a tab here holds exactly what the singleton model can actually give it:
// its own URL, its own back/forward history stack, and its own scroll
// position. `hist[]`/`hist_count`/`hist_pos`/`url_buffer`/`scroll_offset`
// above stay the ACTIVE tab's live working copy (every existing function
// that reads them keeps working unmodified); tab_switch()/tab_new()/
// tab_close() save that live copy into g_tabs[] and load another one in,
// then drive a real navigate() through the one shared render pipeline. This
// is "real tabs, not frozen ones": switching tabs re-fetches and re-renders
// that tab's current URL rather than instantly restoring a cached frame, and
// only one tab's fetch is ever in flight (an inactive tab cannot be
// "loading" in the background). Scroll position is restored (not merely
// stored) once that re-fetch completes - see g_pending_scroll_restore by
// poll_fetch()'s render_page() call.
#define MAX_TABS 8
typedef struct {
    char url[MAX_URL_LEN];
    char hist[HIST_MAX][MAX_URL_LEN];
    int  hist_count;
    int  hist_pos;
    int  scroll_offset;
    uint32_t chip_col;   // per-tab favicon-chip color (index-derived, cosmetic only)
} tab_t;
static tab_t g_tabs[MAX_TABS];
static int   g_tab_count = 1;
static int   g_tab_active = 0;
static int   g_tab_hover = -1;      // tab index under the pointer, or -1
static int   g_tab_close_hover = 0; // 1 when g_tab_hover's close-x is what's hovered
// Applied once the in-flight fetch a tab switch/close triggers completes;
// -1 means "nothing pending". See poll_fetch()'s render_page() call site.
static int   g_pending_scroll_restore = -1;

// A small fixed palette of chip colors so tabs are visually distinct without
// fetching real favicons (#245 is page rendering; a per-site favicon fetch is
// out of scope for this chrome restyle).
static const uint32_t TAB_CHIP_COLORS[] = {
    0x006AE2CF, 0x00E6C15A, 0x008A7BE6, 0x00E2795A, 0x006FD98C, 0x00E67BC1,
};
#define TAB_CHIP_N ((int)(sizeof(TAB_CHIP_COLORS) / sizeof(TAB_CHIP_COLORS[0])))

// ============================================================================
// Bookmarks bar [no-ticket] (browser-glass)
// ============================================================================
#define MAX_BOOKMARKS 12
typedef struct { char label[24]; char url[MAX_URL_LEN]; uint32_t chip_col; } bookmark_t;
// The Home entry uses HOME_URL itself, not a re-typed literal: they were
// briefly out of sync (a trailing "/" here, none in HOME_URL), which meant
// bookmark_find() never matched the actual startup URL and the address
// bar's star showed unfilled on the one page that IS bookmarked by
// default. Verified on VM 2778 (2026-09-06) before this fix.
static bookmark_t g_bookmarks[MAX_BOOKMARKS] = {
    { "Home",      HOME_URL,                             0x006AE2CF },
    { "App Repo",  "https://maytera.net/downloads.html", 0x00E6C15A },
    { "Docs",      "https://maytera.net/docs.html",      0x008A7BE6 },
    { "Changelog", "https://maytera.net/changelog.html", 0x006FD98C },
};
static int g_bookmark_count = 4;   // entries populated above; the star can append more

// Settings (overflow) menu.
static int g_settings_open = 0;
#define SETTINGS_MENU_W   150
#define SETTINGS_ITEM_H   28
#define SETTINGS_ITEM_N   3   // Home, Reconnect, About

static int  window_handle = -1;
static int  running = 1;

// ---------------------------------------------------------------------------
// (browserglass) The frosted-wallpaper backdrop: the shared libc recipe
// (userland/libc/gui_style.h gui_glass_backdrop_*, blame.md glasslib). This
// app owns only the two persistent pieces the API asks for; the scratch
// (raw thumbnail bytes, blur plane) lives in gui.c.
// ---------------------------------------------------------------------------
static uint32_t g_bd[GUI_GLASS_BD_W * GUI_GLASS_BD_H];
static int g_bd_wi = GUI_GLASS_BD_NEVER;   // wallpaper index the backdrop was built for
static int g_chrome_dirty = 1;             // the ONLY thing that can make redraw() blit the backdrop
static int g_last_tab_count = -1;          // tab pills uncover margin when their count changes

// The backdrop colour under content pixel (x, y): what the kernel's nearest-
// neighbour scale put there, to within the blur. Every AA edge and shadow
// drawn onto the backdrop takes its outer colour from here; a flat guess is
// what produces a square halo around a round corner (glass doc section 2).
static uint32_t bd_at(int x, int y) {
    return gui_glass_backdrop_at(g_bd, g_win_w, g_win_h, x, y);
}

// Rebuild the backdrop if the wallpaper changed. SYS_GET_WALLPAPER is the
// same poll the compositor makes; a changed index marks the chrome dirty.
static void sync_backdrop(void) {
    if (gui_glass_backdrop_sync(g_bd, &g_bd_wi, COL_PANEL, 158, WEL_BG_TOP, WEL_BG_BOTTOM))
        g_chrome_dirty = 1;
}

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

// ---------------------------------------------------------------------------
// #netfix2: SAY WHAT WENT WRONG.
//
// This browser could only ever draw the string "Fetch failed", and that was not
// the browser's fault: the kernel destroyed the cause before userland could see
// it. sys_http_fetch() collapses DNS, connect, TLS and timeout to a flat -1;
// sys_http_fetch_poll() returns only running/done/error plus an HTTP status;
// and async_fetch_worker() overwrites the live phase with HTTP_PHASE_ERROR the
// instant an attempt fails. So the ONE thing the user could see said nothing.
//
// The owner has been reporting "the browser doesn't work" for months, on two
// machines, and every time the only artifact available was a screen that said
// "Fetch failed" and a /BOOTLOG.TXT that (until now) said nothing about fetches
// at all. Four completely different faults - an unreachable resolver, a
// firewalled port, a wrong system clock, an untrusted certificate - render
// identically as a page that does not load, and they need four different
// actions from him.
//
// SYS_NET_LAST_ERROR returns the reason the kernel's fetch chokepoint recorded
// for THIS process's last fetch. net_error_advice() turns it into a sentence
// worth reading. If nothing was recorded we fall back to the old text rather
// than inventing a diagnosis, because a confident wrong message is worse than
// a vague right one.
static void br_set_fetch_error(const char *fallback) {
    char reason[96];
    reason[0] = 0;
    int code = net_last_error(reason, sizeof(reason));
    if (code <= 0 || !reason[0]) {
        str_cpy(status_msg, fallback);
        return;
    }
    const char *advice = net_error_advice(reason);
    if (advice && advice[0]) {
        str_cpy(status_msg, advice);
    } else {
        // No sentence for this one yet: show the reason NAME rather than
        // "Fetch failed". A name he can read out to us is still worth far more
        // than two words that say nothing.
        str_cpy(status_msg, "Fetch failed: ");
        int p = str_len(status_msg);
        for (int i = 0; reason[i] && p < (int)sizeof(status_msg) - 1; i++)
            status_msg[p++] = reason[i];
        status_msg[p] = 0;
    }
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

// #245: measure in the FACE AND STYLE the run will be drawn in. ttf_measure()
// only ever spoke for the active face, so a page naming a second font family
// was laid out on widths from the wrong typeface; the mono half of a page like
// maytera.net wrapped at the wrong word and overlapped its neighbour.
static int br_measure(const char *str, int size, int face, int style) {
    return ttf_measure_ex(str, face, size, style);
}

// The three hooks fontmap.c needs to see the font registry. On the device they
// are the SYS_FONT_* syscalls; the host-side harness answers the same three from
// FreeType over a copy of the same .TTF files, so one resolver serves both.
int fm_plat_font_count(void) { return font_count(); }
int fm_plat_font_name(int idx, char *buf, int cap) { return font_name(idx, buf, cap); }
int fm_plat_font_style(int idx, char *buf, int cap) { return font_style(idx, buf, cap); }

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

// #245 perf: one line per page render, phase by phase, straight to serial.
// This is the measurement harness the "browser is slow" work is judged on;
// it costs one printf per LOAD (not per token), so it stays in permanently.
extern int printf(const char *fmt, ...);

/*
 * engwalkstack (#245): one provenance line per launch.
 *
 * The md5s are injected by the build (-DENGWALK_LAYOUT_STAMP for the engine,
 * -DENGWALK_MAIN_STAMP for this file) and default to "unstamped", so a build
 * that did not pass them says so instead of printing a plausible wrong thing.
 * The point is that a serial log now NAMES the sources that produced the
 * binary that wrote it, which is what makes an A/B measurement checkable:
 * "the two binaries differ" does not tell you the edit you are measuring is
 * in either of them (blame.md, engflexclose).
 */
#ifndef ENGWALK_MAIN_STAMP
#define ENGWALK_MAIN_STAMP "unstamped"
#endif
static void br_buildstamp(void) {
    printf("[BUILDSTAMP] browser layout.c=%s main.c=%s\n",
           layout_src_stamp(), ENGWALK_MAIN_STAMP);
}

/*
 * engwalkstack (#245): report a page the layout walk had to cut short. One
 * line, only when it happened, so a normal page pays nothing.
 */
static void br_report_depth(void) {
    if (g_layout.deep_truncated)
        printf("[LAYOUT] page TRUNCATED: DOM nested past the walk depth ceiling; "
               "the over-deep subtree was not laid out (peak depth %d)\n",
               g_layout.walk_depth_peak);
}

static void br_layout_dump(const char *tag) {
    if (!g_brdump) return;
    printf("[BRDUMP] %s items=%d n_scrolls=%d\n", tag,
           g_layout.n_items, g_layout.n_scrolls);
    for (int i = 0; i < g_layout.n_items; i++) {
        layout_item *it = &g_layout.items[i];
        if (it->kind != 1) continue;
        printf("[BRDUMP]  box[%d] x=%d y=%d w=%d h=%d bg=%06x\n",
               i, it->x, it->y, it->w, it->h, (unsigned)it->bg);
        /* #245 engoutline: only printed when an outline is actually armed, so
         * a page that authors none dumps byte-identically to before. */
        if (it->ol_w)
            printf("[BRDUMP]   outline[%d] w=%d style=%d col=%06x\n",
                   i, it->ol_w, it->ol_style, (unsigned)it->ol_col);
    }
    for (int i = 0; i < g_layout.n_scrolls; i++) {
        scroll_box *sb = &g_layout.scrolls[i];
        printf("[BRDUMP]  scroll[%d] cx=%d cy=%d cw=%d ch=%d extent=%d off=%d extentw=%d offx=%d\n",
               i, sb->content_x, sb->content_y, sb->content_w, sb->content_h,
               sb->extent_h, sb->offset, sb->extent_w, sb->offset_x);
    }
}

static void render_page(const char *html, int html_len) {
    unsigned long t_a = uptime_ms();
    g_content_len = html_len;   // #89: remember for re-layout on resize
    // engscroll (#245): a new page starts with every overflow container at the
    // top. Reset before layout so scroll offsets never leak across navigations.
    for (int i = 0; i < LAYOUT_MAX_SCROLLS; i++) { g_box_scroll[i] = 0; g_box_scroll_x[i] = 0; }
    g_active_scroll = 0;
    layout_reset_scroll_requests();
    last_layout_w = g_win_w;
    g_have_layout = 0;
    g_layout.n_items = 0;
    g_layout.content_height = 0;

    // #245 enggrad: the gradient table accumulates across this page's author
    // sheets (one cssvar_preprocess per <style>/<link>), so clear it before any
    // sheet is parsed. Layout reads back an index into this table via the
    // outline-color carrier; a page with no gradients leaves it empty.
    cssvar_gradients_reset();
    grad_cache_free();

    mdb_parser *p = mdb_create();
    if (!p) return;
    g_doc_parser = p;   // #resizereflow: keep for layout-only reflow on resize
    mdb_parse_chunk(p, (const unsigned char *) html, (unsigned long) html_len);
    mdb_parse_complete(p);
    unsigned long t_parse = uptime_ms();

    // Run inline <script> elements under Duktape with the DOM bound.
    // DOM mutations are visible to the layout pass below.
    js_run_document(mdb_document(p), 0, 0);
    unsigned long t_js = uptime_ms();

    mcs_ctx *css = mcs_create();
    if (!css) { mdb_destroy(p); return; }
    g_css_ctx = css;   // #resizereflow: keep for layout-only reflow on resize

    // The wrap width, hoisted above the stylesheet work because libcss needs it.
    // @media conditions are evaluated against the viewport we declare, and
    // vw/vh in the author sheet resolve against it. It used to be hardcoded to
    // 1024x768 inside mcs_create() while layout ran at ~760px, so a responsive
    // page was styled for a viewport nobody was looking at.
    int content_width = CONTENT_W - 24; // padding both sides
    mcs_set_viewport(css, content_width, CONTENT_H);

    int css_len = extract_styles(html, html_len, author_css, sizeof(author_css));
    if (css_len > 0)
        mcs_add_author_css(css, author_css, (unsigned long) css_len);
    unsigned long t_css = uptime_ms();

    // External stylesheets (#245): fetch + apply <link rel=stylesheet> sheets.
    // Synchronous; bounded by the fetch timeout per sheet and a total CSS budget.
    apply_external_css(css, html, html_len);
    unsigned long t_ext = uptime_ms();

    unsigned long m0 = g_layout_measure_calls;
    unsigned long w0 = g_layout_measure_words;
    g_layout.items = g_items;
    if (layout_document(css, mdb_document(p), content_width,
                        br_measure, &g_layout) == 0) {
        g_have_layout = 1;
        images_begin();
    }
    br_layout_dump("render");
    br_report_depth();   // engwalkstack (#245)
    unsigned long t_lay = uptime_ms();
#ifdef BROWSER_PERF
    printf("[BRPERF] bytes=%d parse=%lu js=%lu css=%lu extcss=%lu layout=%lu "
           "total=%lu ms items=%d words=%lu measures=%lu\n",
           html_len, t_parse - t_a, t_js - t_parse, t_css - t_js,
           t_ext - t_css, t_lay - t_ext, t_lay - t_a,
           g_layout.n_items, g_layout_measure_words - w0,
           g_layout_measure_calls - m0);
    printf("[BRPROF] Mcyc walk=%lu style=%lu text=%lu flow=%lu meas=%lu "
           "attr=%lu elems=%lu texts=%lu\n",
           (unsigned long)(g_lp_walk / 1000000ULL),
           (unsigned long)(g_lp_style / 1000000ULL),
           (unsigned long)(g_lp_text / 1000000ULL),
           (unsigned long)(g_lp_flow / 1000000ULL),
           (unsigned long)(g_lp_meas / 1000000ULL),
           (unsigned long)(g_lp_attr / 1000000ULL),
           g_lp_nelem, g_lp_ntext);
#else
    (void) t_parse; (void) t_js; (void) t_css; (void) t_ext;
    (void) t_lay; (void) t_a; (void) m0; (void) w0;
#endif

    // The document + styles stay referenced by p/css. We intentionally leak
    // them here (one page at a time, small) rather than risk the teardown
    // path; freeing is not required for correctness of the rendered runs.
    (void) css;
    (void) p;
}

// #resizereflow: compare two hrefs for equality (freestanding, no libc dep).
static int br_href_eq(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}

// #resizereflow: LAYOUT-ONLY reflow for a pure resize. A resize changes the
// viewport WIDTH only, so re-run the layout pass against the ALREADY-parsed DOM
// and ALREADY-built CSSOM at the new width. NO network I/O, NO re-parse, NO JS.
// This is the whole fix for the owner's 30s+ stall: the old path called
// render_page() on every width change, which re-parsed the HTML, re-ran the
// inline JS, and RE-FETCHED external <link> stylesheets synchronously over the
// (slow, ICS) network. Width-dependent CSS still re-evaluates: mcs_set_viewport
// updates c->media / c->unit, and mcs_compute_style re-runs css_select_style
// against them for every node on every layout, so @media and vw/vh resolve at
// the new width from the EXISTING parsed sheets (a re-cascade, not a re-fetch).
static void relayout_page(void) {
    if (!g_css_ctx || !g_doc_parser) return;

    // Preserve already-decoded images across the reflow so a resize re-fetches
    // ZERO of them. The image cache is keyed by layout-item index, which the
    // reflow renumbers, so snapshot each cached image's href (from the OLD, still
    // live layout) BEFORE we overwrite g_layout, then re-key by href to the new
    // indices below. Without this a resize would drop and re-fetch every image
    // (async, but still network I/O plus a visible reload flicker).
    static char img_url[IMG_MAX][LAYOUT_HREF_MAX];
    int saved = g_img_n;
    for (int k = 0; k < saved; k++) {
        img_url[k][0] = 0;
        int oi = g_imgs[k].item;
        if (oi >= 0 && oi < g_layout.n_items && g_layout.items[oi].kind == 3) {
            int z = 0; const char *h = g_layout.items[oi].href;
            while (h[z] && z < LAYOUT_HREF_MAX - 1) { img_url[k][z] = h[z]; z++; }
            img_url[k][z] = 0;
        }
    }

    // #245 enggrad: the gradient cache is keyed by (item index, w, h), both of
    // which change on a resize; drop it so each box re-rasterises at the new
    // size against the still-live gradient table (the table is NOT rebuilt here,
    // exactly like the CSSOM).
    grad_cache_free();

    last_layout_w = g_win_w;
    g_have_layout = 0;
    g_layout.n_items = 0;
    g_layout.content_height = 0;

    int content_width = CONTENT_W - 24; // padding both sides, matches render_page
    mcs_set_viewport(g_css_ctx, content_width, CONTENT_H);

    // engscroll (#245): carry the current per-container scroll offsets into
    // this layout pass (so a resize preserves scroll position), then clamp our
    // stored offsets to whatever extent layout reports back.
    layout_reset_scroll_requests();
    for (int i = 0; i < LAYOUT_MAX_SCROLLS; i++) {
        layout_set_scroll_request(i, g_box_scroll[i]);
        layout_set_scroll_request_x(i, g_box_scroll_x[i]);
    }
    g_layout.items = g_items;
    if (layout_document(g_css_ctx, mdb_document(g_doc_parser), content_width,
                        br_measure, &g_layout) != 0)
        return;                  // layout failed: keep the last good render
    g_have_layout = 1;
    for (int i = 0; i < g_layout.n_scrolls && i < LAYOUT_MAX_SCROLLS; i++) {
        int mo = g_layout.scrolls[i].extent_h - g_layout.scrolls[i].content_h;
        if (mo < 0) mo = 0;
        if (g_box_scroll[i] > mo) g_box_scroll[i] = mo;
        if (g_box_scroll[i] < 0) g_box_scroll[i] = 0;
        int mox = g_layout.scrolls[i].extent_w - g_layout.scrolls[i].content_w;
        if (mox < 0) mox = 0;
        if (g_box_scroll_x[i] > mox) g_box_scroll_x[i] = mox;
        if (g_box_scroll_x[i] < 0) g_box_scroll_x[i] = 0;
    }
    br_layout_dump("relayout");
    br_report_depth();   // engwalkstack (#245)

    // Re-key each cached image to its new item index by matching href; free any
    // whose <img> no longer appears at the new width, then compact the cache.
    for (int k = 0; k < saved; k++) {
        int newi = -1;
        if (img_url[k][0]) {
            for (int i = 0; i < g_layout.n_items; i++) {
                if (g_layout.items[i].kind == 3 &&
                    br_href_eq(g_layout.items[i].href, img_url[k])) { newi = i; break; }
            }
        }
        if (newi >= 0) {
            g_imgs[k].item = newi;
        } else {
            if (g_imgs[k].px) free(g_imgs[k].px);
            g_imgs[k].px = 0;
            g_imgs[k].item = -1;   // dead; compacted out below
        }
    }
    { int w = 0; for (int k = 0; k < g_img_n; k++)
          if (g_imgs[k].item >= 0) g_imgs[w++] = g_imgs[k];
      g_img_n = w; }

    // Drop any in-flight image fetch (its target item index is now stale) and
    // rescan; poll_images() skips items already in the cache, so only images not
    // yet loaded are fetched. An all-images-cached page fetches nothing on resize.
    if (g_img_job >= 0) { http_fetch_read(g_img_job, (char *) g_imgfetch, 0); g_img_job = -1; }
    g_img_item = -1;
    g_img_scan = 0;
}

static int fetch_page(const char *url) {
    unsigned int bytes_read = 0;
    int http_status = 0;

    int ret = sys_http_fetch(url, content_buffer, MAX_CONTENT - 1,
                             &bytes_read, &http_status);

    if (ret < 0) {
        // #netfix2: THE COMMENT THAT USED TO BE HERE WAS WRONG, and it mattered.
        // It listed "-2=dns/no_net, -3=connect, -4=tls, -5=send/timeout" as if
        // those codes arrived. They never do: sys_http_fetch() (syscall.c) turns
        // every one of them into a flat -1 before returning, so this branch has
        // always printed "Fetch failed, code: -1 st:0" and the reader was left
        // trusting a legend for values the kernel does not emit.
        br_set_fetch_error("Fetch failed");
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

// Map a toolbar button index to its rect. Returns 1 if valid. Index 5 (the
// Settings/overflow button) reads from the SAME SETTINGS_BTN_* constants
// draw_toolbar() draws it at, so the hit-test can never drift from the
// drawn geometry (this app's own history of click-coordinate bugs, see
// blame.md's "sys-win-create-takes-outer-size" and "userland-click-coords").
static int nav_btn_rect(int idx, int *x, int *y, int *w, int *h) {
    switch (idx) {
        case 0: *x = NAV_BACK_X;      *y = NAV_BTN_Y;      *w = NAV_BTN_W;      *h = NAV_BTN_H;      return 1;
        case 1: *x = NAV_FWD_X;       *y = NAV_BTN_Y;      *w = NAV_BTN_W;      *h = NAV_BTN_H;      return 1;
        case 2: *x = NAV_RELOAD_X;    *y = NAV_BTN_Y;      *w = NAV_BTN_W;      *h = NAV_BTN_H;      return 1;
        case 3: *x = NAV_HOME_X;      *y = NAV_BTN_Y;      *w = NAV_BTN_W;      *h = NAV_BTN_H;      return 1;
        case 4: *x = GO_BTN_X;        *y = GO_BTN_Y;       *w = GO_BTN_W;       *h = GO_BTN_H;       return 1;
        case 5: *x = SETTINGS_BTN_X;  *y = SETTINGS_BTN_Y; *w = SETTINGS_BTN_W; *h = SETTINGS_BTN_H; return 1;
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

// #25: Stop glyph (a thick X), drawn over the Reload button while a fetch is
// in flight - the button morphs Reload <-> Stop the way every real browser's
// does, rather than adding a fifth toolbar slot.
static void draw_stop_icon(int bx, int by, int bw, int bh, uint32_t col) {
    int cx = bx + bw / 2, cy = by + bh / 2, r = 6;
    for (int d = -r; d <= r; d++) {
        gui_fill_rect(window_handle, cx + d, cy + d, 2, 2, col);
        gui_fill_rect(window_handle, cx + d, cy - d, 2, 2, col);
    }
}

// Settings (overflow) button: three vertical dots, matching the approved
// mockup exactly. Hard-edged rects (same house style as the icons above)
// rather than AA circles, so no button-fill color needs to be threaded in.
static void draw_dots_icon(int bx, int by, int bw, int bh, uint32_t col) {
    int cx = bx + bw / 2, cy = by + bh / 2;
    for (int i = -1; i <= 1; i++)
        gui_fill_rect(window_handle, cx - 2, cy + i * 7 - 2, 4, 4, col);
}

// TLS lock glyph for the address bar. `locked` draws a closed shackle
// (accent-green caller); the insecure/unknown case draws the shackle
// slid open, in the caller's (danger-tone) color - a shape difference, not
// just a color difference, so it still reads on a color-blind pass.
static void draw_lock_icon(int x, int y, int sz, uint32_t col, int locked) {
    int bw = sz, bh = (sz * 3) / 4;
    int by = y + sz - bh;
    gui_fill_rounded(window_handle, x, by, bw, bh, 2, col);
    int sw = (sz * 2) / 3, sh = (sz * 2) / 3;
    int sx = x + (bw - sw) / 2 + (locked ? 0 : sw / 2);
    int sy = by - sh + 3;
    gui_draw_rect_outline(window_handle, sx, sy, sw, sh, col);
}

// Truncate `s` to fit within max_w px at `size`, appending "..." (real-
// measured, not a fixed suffix count) when it does not already fit. Local
// to this file, same idea as App Repo's trunc_fit() - built from the shared
// gui_ttf_width() measuring primitive, not a forked text-shaping routine.
static void trunc_fit(const char *s, int size, int max_w, char *out, int cap) {
    int n = str_len(s);
    if (n > cap - 1) n = cap - 1;
    for (int i = 0; i < n; i++) out[i] = s[i];
    out[n] = 0;
    if (gui_ttf_width(out, size) <= max_w) return;
    while (n > 0) {
        n--;
        out[n] = 0;
        char tmp[64];
        int tn = n;
        if (tn > (int) sizeof(tmp) - 4) tn = (int) sizeof(tmp) - 4;
        int i = 0;
        for (; i < tn; i++) tmp[i] = out[i];
        tmp[i++] = '.'; tmp[i++] = '.'; tmp[i++] = '.'; tmp[i] = 0;
        if (gui_ttf_width(tmp, size) <= max_w) { str_cpy(out, tmp); return; }
    }
    out[0] = 0;
}

// ---- Tab strip geometry (shared by draw_tabstrip() and hit-testing) ----
// (browserglass) Tabs are pills ON the backdrop, y = PAD, height TAB_H,
// exactly the Task Manager's tab row. The "+" is a round pill at the right.
#define TAB_Y          PAD
#define TAB_MIN_W      90
#define TAB_MAX_W      170
#define NEWTAB_W       TAB_H

static void tabstrip_geom(int *tab_w_out, int *area_x_out) {
    int area_x = PAD;
    int area_w = WIN_WIDTH - PAD - NEWTAB_W - TAB_GAP - PAD;
    int n = g_tab_count > 0 ? g_tab_count : 1;
    int w = (area_w - (n - 1) * TAB_GAP) / n;
    if (w > TAB_MAX_W) w = TAB_MAX_W;
    if (w < TAB_MIN_W) w = TAB_MIN_W;
    *tab_w_out = w;
    *area_x_out = area_x;
}
static int tab_rect(int i, int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= g_tab_count) return 0;
    int tw, ax;
    tabstrip_geom(&tw, &ax);
    *x = ax + i * (tw + TAB_GAP);
    *y = TAB_Y;
    *w = tw;
    *h = TAB_H;
    return 1;
}
// The close (x) hit-box: the right end of the tab, same rect draw_tabstrip()
// paints its glyph into.
static void tab_close_rect(int i, int *x, int *y, int *w, int *h) {
    int tx = 0, ty = 0, tw = 0, th = 0;
    tab_rect(i, &tx, &ty, &tw, &th);
    *w = 18; *h = th;
    *x = tx + tw - *w - 6;
    *y = ty;
}
static int newtab_rect(int *x, int *y, int *w, int *h) {
    *x = WIN_WIDTH - PAD - NEWTAB_W;
    *y = TAB_Y;
    *w = NEWTAB_W;
    *h = TAB_H;
    return 1;
}

// (browserglass) One pill on the frosted backdrop: the Task Manager /
// Calculator / Image Viewer tab pill. Selected = accent fill; otherwise
// panel-coloured glass with the panel border, hover lightens. The AA edge
// takes the backdrop colour at the pill's own centre. Returns the fill so
// the caller can blend what it draws inside toward it.
static uint32_t draw_bd_pill(int x, int y, int w, int h, int sel, int hov) {
    uint32_t outer = bd_at(x + w / 2, y + h / 2);
    uint32_t fill = sel ? COL_ACCENT : (hov ? gui_lighten(COL_PANEL, 18) : COL_PANEL);
    gui_fill_rounded_aa(window_handle, x, y, w, h, h / 2, fill, outer);
    if (!sel) gui_rounded_border(window_handle, x, y, w, h, h / 2, COL_SEPARATOR);
    return fill;
}

// Draw the tab strip: one pill per tab on the backdrop (accent for the
// active one), a chip dot and a truncated label inside each, a stroked close
// x on each when there is more than one, and a round "+" pill.
//
// Anti-flash note (glass doc section 11): every pixel this paints lies
// inside a pill that is REFILLED here, so a hover repaint is idempotent and
// a changed label never smears. The only thing that can leave stale pill
// pixels on the backdrop is a change in the NUMBER of tabs (the pills get
// wider or narrower), which redraw() catches by comparing g_tab_count with
// the previous frame's and re-blitting the backdrop (edglass trap 3: track
// every "uncovers margin" transition at ONE place).
static void draw_tabstrip(void) {
    for (int i = 0; i < g_tab_count; i++) {
        int tx, ty, tw, th;
        tab_rect(i, &tx, &ty, &tw, &th);
        int active = (i == g_tab_active);
        const char *url = active ? url_buffer : g_tabs[i].url;
        uint32_t chip = (i < TAB_CHIP_N) ? TAB_CHIP_COLORS[i] : g_tabs[i].chip_col;

        uint32_t fill = draw_bd_pill(tx, ty, tw, th, active, i == g_tab_hover);
        uint32_t ink  = active ? COL_ACCENT_INK : (i == g_tab_hover ? COL_TEXT_DIM : COL_FAINT);

        // Chip dot: on the accent pill the chip reads as the accent ink (a
        // teal chip on a teal pill would vanish); elsewhere its own colour.
        int chip_d = 8;
        int ccx = tx + 10, ccy = ty + (th - chip_d) / 2;
        gui_fill_circle_aa(window_handle, ccx, ccy, chip_d, active ? COL_ACCENT_INK : chip, fill);

        char lbl[40];
        tab_label(url, lbl, sizeof(lbl));
        int lx = ccx + chip_d + 8;
        int lw = tx + tw - (g_tab_count > 1 ? 26 : 12) - lx;
        char fit[40];
        trunc_fit(lbl, 12, lw > 0 ? lw : 1, fit, sizeof(fit));
        win_draw_text_ttf(window_handle, lx, ty + (th - 12) / 2 - 1, fit, 12, ink);

        if (g_tab_count > 1) {
            int xx, xy, xw, xh;
            tab_close_rect(i, &xx, &xy, &xw, &xh);
            int cx = xx + xw / 2, cy = xy + xh / 2;
            uint32_t xcol = active ? COL_ACCENT_INK
                          : (i == g_tab_hover && g_tab_close_hover) ? COL_TEXT : COL_FAINT;
            gui_line(window_handle, cx - 3, cy - 3, cx + 3, cy + 3, xcol);
            gui_line(window_handle, cx - 3, cy + 3, cx + 3, cy - 3, xcol);
        }
    }

    // New tab: a round pill with a STROKED plus (glass doc section 6: never a
    // typed glyph for a mark that must look like a mark).
    int nx, ny, nw, nh;
    newtab_rect(&nx, &ny, &nw, &nh);
    draw_bd_pill(nx, ny, nw, nh, 0, 0);
    int pcx = nx + nw / 2, pcy = ny + nh / 2;
    gui_thick_line(window_handle, pcx - 5, pcy, pcx + 5, pcy, 2, COL_TEXT_DIM);
    gui_thick_line(window_handle, pcx, pcy - 5, pcx, pcy + 5, 2, COL_TEXT_DIM);
}

// ---- Bookmarks row geometry (shared by draw_bookmarks_bar() and hit-testing) ----
// (browserglass) Chips are 20px pills inside the panel: an 8px dot, an 11px
// label. Width is measured, never guessed.
#define BMK_GAP    6
#define BMK_CHIP   8
#define BMK_TTF    11

static int bookmark_rect(int i, int *x, int *y, int *w, int *h) {
    if (i < 0 || i >= g_bookmark_count) return 0;
    int bx = INNER_X;
    for (int j = 0; j < i; j++) {
        int jw = gui_ttf_width(g_bookmarks[j].label, BMK_TTF) + BMK_CHIP + 22;
        bx += jw + BMK_GAP;
    }
    *w = gui_ttf_width(g_bookmarks[i].label, BMK_TTF) + BMK_CHIP + 22;
    *x = bx;
    *y = BOOKMARKS_Y;
    *h = BOOKMARKS_H;
    return 1;
}

// The bookmarks row: its band of the panel is refilled first (flat C_PANEL,
// idempotent), then each chip. A hovered chip gets the lightened pill fill;
// the dot and the label are drawn over whichever fill is under them.
static void draw_bookmarks_bar(void) {
    gui_fill_rect(window_handle, INNER_X, BOOKMARKS_Y, INNER_W, BOOKMARKS_H, COL_PANEL);
    for (int i = 0; i < g_bookmark_count; i++) {
        int bx, by, bw, bh;
        if (!bookmark_rect(i, &bx, &by, &bw, &bh)) break;
        if (bx + bw > INNER_X + INNER_W) break;   // out of room: drop silently, no scroll (v1)
        int hov = (bx <= g_mx_last && g_mx_last < bx + bw && by <= g_my_last && g_my_last < by + bh);
        uint32_t fill = hov ? gui_lighten(COL_PANEL, 18) : COL_PANEL;
        if (hov) gui_fill_rounded_aa(window_handle, bx, by, bw, bh, bh / 2, fill, COL_PANEL);
        int ccy = by + (bh - BMK_CHIP) / 2;
        gui_fill_circle_aa(window_handle, bx + 8, ccy, BMK_CHIP, g_bookmarks[i].chip_col, fill);
        win_draw_text_ttf(window_handle, bx + 8 + BMK_CHIP + 6, by + (bh - BMK_TTF) / 2 - 1,
                          g_bookmarks[i].label, BMK_TTF, hov ? COL_TEXT : COL_TEXT_DIM);
    }
}

// ---- Settings (overflow) menu geometry ----
static void settings_menu_rect(int *x, int *y, int *w, int *h) {
    *w = SETTINGS_MENU_W;
    *h = SETTINGS_ITEM_N * SETTINGS_ITEM_H + 8;
    *x = SETTINGS_BTN_X + SETTINGS_BTN_W - *w;
    if (*x < 4) *x = 4;
    *y = SETTINGS_BTN_Y + SETTINGS_BTN_H + 4;
}
static void settings_item_rect(int idx, int *x, int *y, int *w, int *h) {
    int mx, my, mw, mh;
    settings_menu_rect(&mx, &my, &mw, &mh);
    *x = mx + 4;
    *w = mw - 8;
    *h = SETTINGS_ITEM_H;
    *y = my + 4 + idx * SETTINGS_ITEM_H;
}
static const char *SETTINGS_ITEM_LABELS[SETTINGS_ITEM_N] = { "Home", "Reconnect", "About" };

static void draw_settings_menu(void) {
    if (!g_settings_open) return;
    int mx, my, mw, mh;
    settings_menu_rect(&mx, &my, &mw, &mh);
    // (browserglass) The popout floats over the bookmarks row / the page
    // sheet, whose colour is the document's: a non-AA fill (no outer colour
    // to blend toward) with the DK_EDGE_GLASS stroke that is rated for an
    // unknown surface (glass doc section 1), not the panel's own border.
    gui_fill_rounded(window_handle, mx, my, mw, mh, 8, COL_CARD_BG);
    gui_rounded_border(window_handle, mx, my, mw, mh, 8, COL_EDGE_GLASS);
    for (int i = 0; i < SETTINGS_ITEM_N; i++) {
        int ix, iy, iw, ih;
        settings_item_rect(i, &ix, &iy, &iw, &ih);
        // point_in() lives in the Event handlers section, below this
        // function; inlined here rather than forward-declaring it.
        int hov = (g_mx_last >= ix && g_mx_last < ix + iw && g_my_last >= iy && g_my_last < iy + ih);
        // Refill the row every draw (flat card colour) so a hover that moved
        // off it never leaves the highlight behind.
        gui_fill_rounded(window_handle, ix, iy, iw, ih, 5, hov ? COL_RAISED : COL_CARD_BG);
        win_draw_text_ttf(window_handle, ix + 10, iy + (ih - 12) / 2 - 1,
                          SETTINGS_ITEM_LABELS[i], 12, COL_TEXT);
    }
}

// Bookmark-star hit-box inside the address bar. Shared by draw_toolbar()
// (which draws it) and handle_mouse_up() (which hit-tests it), the same
// draw/hit-test-from-one-source discipline as nav_btn_rect().
static void url_star_rect(int *x, int *y, int *w, int *h) {
    *w = 14; *h = 14;
    *x = URL_BAR_X + URL_BAR_W - URL_STAR_W + (URL_STAR_W - *w) / 2;
    *y = URL_BAR_Y + (URL_BAR_H - *h) / 2;
}

// (browserglass) A toolbar pill ON the panel: same shape as the tab pills
// but the outer colour is the flat panel fill, so a repaint over itself is
// exact. Hover lightens, press lightens more, disabled keeps the resting
// fill (its glyph goes faint instead: a control that still does something
// is never greyed as a whole, glass doc section 8).
static void draw_panel_pill(int x, int y, int w, int h, gui_state_t st) {
    uint32_t fill = COL_PANEL;
    if (st == GUI_ST_HOVER)   fill = gui_lighten(COL_PANEL, 18);
    if (st == GUI_ST_PRESSED) fill = gui_lighten(COL_PANEL, 30);
    gui_fill_rounded_aa(window_handle, x, y, w, h, h / 2, fill, COL_PANEL);
    gui_rounded_border(window_handle, x, y, w, h, h / 2, COL_SEPARATOR);
}

// Back / Forward arrows: STROKED with the shared gui_thick_line (a typed "<"
// is a thin chevron at 1:1; glass doc section 6, "the arrow is stroked").
static void draw_arrow_icon(int bx, int by, int bw, int bh, int forward, uint32_t col) {
    int cx = bx + bw / 2, cy = by + bh / 2;
    int tip = forward ? cx + 5 : cx - 5, tail = forward ? cx - 5 : cx + 5;
    int back = forward ? tip - 4 : tip + 4;
    gui_thick_line(window_handle, tail, cy, tip, cy, 2, col);
    gui_thick_line(window_handle, tip, cy, back, cy - 4, 2, col);
    gui_thick_line(window_handle, tip, cy, back, cy + 4, 2, col);
}

// Draw the toolbar row: nav pills, address field, overflow pill and Go.
static void draw_toolbar(void) {
    // The row's band of the panel, refilled flat (idempotent) so a control
    // that shrank (the caret, a shorter URL) leaves nothing behind.
    gui_fill_rect(window_handle, INNER_X, TOOLBAR_Y, INNER_W, TOOLBAR_H, COL_PANEL);

    // Navigation pills with stroked faces.
    int en[4] = { can_go_back(), can_go_forward(), 1, 1 };
    for (int i = 0; i < 4; i++) {
        int bx, by, bw, bh;
        nav_btn_rect(i, &bx, &by, &bw, &bh);
        draw_panel_pill(bx, by, bw, bh, btn_state(i, en[i]));
        uint32_t ic = en[i] ? COL_TEXT : COL_FAINT;
        if (i == 0)      draw_arrow_icon(bx, by, bw, bh, 0, ic);
        else if (i == 1) draw_arrow_icon(bx, by, bw, bh, 1, ic);
        else if (i == 2) {
            if (is_loading) draw_stop_icon(bx, by, bw, bh, ic);   // #25
            else draw_refresh_icon(bx, by, bw, bh, ic);
        } else draw_home_icon(bx, by, bw, bh, ic);
    }

    // Address field: a pill-shaped input (DK_INPUT_FILL / DK_INPUT_BORDER),
    // an accent focus ring, a TLS lock, TTF text + caret, and a bookmark star.
    int fr = URL_BAR_H / 2;
    gui_fill_rounded_aa(window_handle, URL_BAR_X, URL_BAR_Y, URL_BAR_W, URL_BAR_H, fr, COL_FIELD_BG, COL_PANEL);
    if (url_focused) {
        // (#745) was COL_ACCENT, the theme accent, which is 1.76:1 on the
        // Dark theme's surface. gui_pal()->focus is the keyboard-position
        // token and is repaired to the 3:1 non-text floor by
        // gui_set_palette(). Still two passes for the 2px feel.
        uint32_t ring = gui_pal()->focus;
        gui_rounded_border(window_handle, URL_BAR_X, URL_BAR_Y, URL_BAR_W, URL_BAR_H, fr, ring);
        gui_rounded_border(window_handle, URL_BAR_X - 1, URL_BAR_Y - 1,
                           URL_BAR_W + 2, URL_BAR_H + 2, fr + 1, ring);
    } else {
        gui_rounded_border(window_handle, URL_BAR_X, URL_BAR_Y, URL_BAR_W, URL_BAR_H, fr, COL_FIELD_BORDER);
    }

    // TLS lock: closed + accent-green for https, open + danger-tone for
    // anything else. Semantic color, never recolored to fit the palette.
    int is_https = (str_ncmp(url_buffer, "https://", 8) == 0);
    int lock_sz = 14;
    int lock_x = URL_BAR_X + 10;
    draw_lock_icon(lock_x, URL_BAR_Y + (URL_BAR_H - lock_sz) / 2, lock_sz,
                  is_https ? COL_ACCENT : COL_ERROR, is_https);

    // URL text, vertically centered, starting after the lock.
    int text_x = lock_x + lock_sz + 8;
    int text_y = URL_BAR_Y + (URL_BAR_H - 14) / 2;
    int star_w = URL_STAR_W;
    int text_max_w = URL_BAR_X + URL_BAR_W - star_w - 6 - text_x;
    if (url_buffer[0]) {
        br_text(window_handle, text_x, text_y, url_buffer, COL_TEXT);
    } else {
        br_text(window_handle, text_x, text_y, "Enter a URL", COL_TEXT_DIM);
    }
    (void) text_max_w;   // the field clips; kept for the caret-limit check below

    // Caret (only when focused). Approx advance via TTF width of the typed text.
    if (url_focused) {
        char tmp[MAX_URL_LEN];
        int n = url_cursor;
        if (n > MAX_URL_LEN - 1) n = MAX_URL_LEN - 1;
        for (int i = 0; i < n; i++) tmp[i] = url_buffer[i];
        tmp[n] = '\0';
        int caret_x = text_x + gui_ttf_width(tmp, 14);
        if (caret_x < URL_BAR_X + URL_BAR_W - star_w - 4) {
            gui_fill_rect(window_handle, caret_x, URL_BAR_Y + 6, 2, URL_BAR_H - 12, COL_TEXT);
        }
    }

    // Bookmark star, right edge of the address bar. Filled accent when the
    // current page is already bookmarked, a faint outline otherwise.
    int star_bookmarked = (bookmark_find(url_buffer) >= 0);
    int sx, sy, sw, sh;
    url_star_rect(&sx, &sy, &sw, &sh);
    gui_fill_star_aa(window_handle, sx, sy, sw,
                     star_bookmarked ? 100 : 0, COL_ACCENT, COL_FAINT, COL_FIELD_BG);

    // Settings (overflow): three-dot pill.
    {
        int bx, by, bw, bh;
        nav_btn_rect(5, &bx, &by, &bw, &bh);
        draw_panel_pill(bx, by, bw, bh, g_settings_open ? GUI_ST_PRESSED : btn_state(5, 1));
        draw_dots_icon(bx, by, bw, bh, COL_TEXT);
    }

    // Go button: the shared primary button (palette accent), the only solid
    // accent element on the panel.
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
static uint32_t page_bg(void);   // defined with draw_content, used by the gutter
static void draw_scrollbar(void) {
    int thumb_y, thumb_h;
    if (!sb_thumb_geo(&thumb_y, &thumb_h)) return;   // page fits: no bar
    int tx = CONTENT_X + CONTENT_W - SB_W, ty = CONTENT_Y + 1, th = CONTENT_H - 2;
    // Colours via the shared rule (#745 item 77). The page bar sits on the
    // page fill, which is the content surface, not window_bg.
    uint32_t sb_track, sb_thumb;
    // The surface under this gutter is the page sheet, not window_bg, and the
    // page sheet is now the document's own background colour rather than a
    // hardcoded white. A white trough down the side of a dark page was the
    // most visible remnant of that assumption.
    gui_scroll_colors(0, page_bg(), &sb_track, &sb_thumb);
    gui_fill_rect(window_handle, tx, ty, SB_W, th, sb_track);
    // (#117) was a single hardcoded-per-theme COL_SEPARATOR pixel on the left
    // edge only, not contrast-checked against the page surface; the shared,
    // floor-verified trough border (see gui_scroll_trough_border() in
    // gui_scroll.h) draws all four edges and supersedes it.
    gui_scroll_trough_border(window_handle, tx, ty, SB_W, th, sb_track, page_bg());
    gui_fill_rect(window_handle, tx + 2, thumb_y, SB_W - 4, thumb_h, sb_thumb);
    // The outline used to be a hardcoded mid-grey, which is invisible on a
    // dark theme and fights the thumb on a light one. Derive it from the
    // repaired thumb instead so it stays an edge, never a second colour.
    gui_draw_rect_outline(window_handle, tx + 2, thumb_y, SB_W - 4, thumb_h,
                          gui_scroll_thumb_ink(sb_thumb, sb_thumb, sb_track));
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

// The colour of the page sheet itself. A real browser propagates the <html> (or
// failing that <body>) background to the canvas; without that a dark site is
// drawn as dark rectangles floating on a white page, with white gutters, a
// white scrollbar trough and white margins. White stays the fallback for a
// document that declares no background, which is what a bare HTML page expects.
static uint32_t page_bg(void) {
    if (g_have_layout && g_layout.has_doc_bg) return g_layout.doc_bg;
    return 0x00FFFFFF;
}

// Fill a rect clipped to [cx0,cy0)-(cx1,cy1). A layout box can be taller and
// wider than the viewport and can start above it, so every edge needs clipping,
// not just the vertical ones.
static void clip_fill(int x, int y, int w, int h, int on, uint32_t col,
                      int cx0, int cy0, int cx1, int cy1) {
    if (!on || w <= 0 || h <= 0) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < cx0) x0 = cx0;
    if (y0 < cy0) y0 = cy0;
    if (x1 > cx1) x1 = cx1;
    if (y1 > cy1) y1 = cy1;
    if (x1 <= x0 || y1 <= y0) return;
    gui_fill_rect(window_handle, x0, y0, x1 - x0, y1 - y0, col);
}

// #245: the sink rrect.c's span generator paints through. One clip rectangle
// and one colour per box, so a rounded box is clipped to the viewport exactly
// like a square one. THIS IS WHY THE ROUNDING IS DONE HERE RATHER THAN WITH
// gui_fill_rounded_aa(): that primitive has no clipping (so a card taller than
// the viewport could not use it), and it emits one win_draw_pixel syscall per
// corner pixel and one win_draw_rect per scanline, which for a page of cards is
// thousands of syscalls a frame. rrect.c coalesces equal-inset rows, so a 10px
// radius costs about 13 fills instead of 40 plus 400 pixels.
static struct { uint32_t col; int cx0, cy0, cx1, cy1; } g_span;
static void span_fill(void *ctx, int sx, int sy, int sw, int sh) {
    (void) ctx;
    clip_fill(sx, sy, sw, sh, 1, g_span.col,
              g_span.cx0, g_span.cy0, g_span.cx1, g_span.cy1);
}

// Record a form control's hit rectangle and paint the focused field's value.
// Extracted so the rounded-box painter and the square one share it: two copies
// of hit-rectangle bookkeeping is how a control ends up clickable in one code
// path and dead in the other.
static void form_hit_record(const layout_item *it, int bx0, int sy) {
    int max_y = CONTENT_Y + CONTENT_H - 14;
    int fidx = g_form_hit_n;
    form_hit_t *fh;
    if (g_form_hit_n >= 64) return;
    fh = &g_form_hits[g_form_hit_n++];
    fh->x = bx0; fh->y = sy; fh->w = it->w; fh->h = it->h; fh->kind = it->form_kind;
    { int k = 0; while (it->field_name[k] && k < 63) { fh->name[k] = it->field_name[k]; k++; } fh->name[k] = 0; }
    { int k = 0; while (it->href[k] && k < 255) { fh->action[k] = it->href[k]; k++; } fh->action[k] = 0; }
    if (it->form_kind == 1 && fidx == g_focus_field) {
        if (g_field_val[0] && sy + 4 < max_y)
            win_draw_text_ttf(window_handle, bx0 + 6, sy + 4, g_field_val, 14, 0x00000000);
        if (sy >= CONTENT_Y && sy <= max_y)   // focus ring
            gui_draw_rect_outline(window_handle, bx0, sy, it->w, it->h, COL_ACCENT);
    }
}

// ===========================================================================
// #245 enggrad: CSS gradient rasterisation.
//
// A kind-4 layout item carries a gradient-table index in it->bg (set by
// layout.c from the outline-color carrier that cssvar.c parsed the gradient
// into). We rasterise the colour ramp once into a BGRA buffer and blit it with
// win_draw_image, reusing the exact clipped-blit path the raster
// background-image uses. Userland is hardware SSE2 float, so this is plain C
// float math, not fixed point.
// ===========================================================================

// Resolve stop positions into [0,1], auto-distributing unspecified ones and
// enforcing a non-decreasing sequence, then bake a 256-entry colour LUT so the
// per-pixel inner loop is one table lookup.
static void grad_build_lut(const cssvar_gradient *g, uint32_t *lut) {
    int n = g->nstops;
    double pos[CSSVAR_GRAD_MAX_STOPS];
    if (n < 2) n = 2;
    if (n > CSSVAR_GRAD_MAX_STOPS) n = CSSVAR_GRAD_MAX_STOPS;

    // First/last default to the ends; specified positions come through as-is.
    for (int i = 0; i < n; i++)
        pos[i] = (g->stop_pos[i] >= 0) ? g->stop_pos[i] / 1000.0 : -1.0;
    if (pos[0] < 0) pos[0] = 0.0;
    if (pos[n-1] < 0) pos[n-1] = 1.0;
    // Evenly distribute runs of unspecified stops between fixed anchors.
    for (int i = 1; i < n - 1; i++) {
        if (pos[i] >= 0) continue;
        int j = i;
        while (j < n - 1 && pos[j] < 0) j++;      // [i, j) unspecified, j fixed
        double lo = pos[i-1], hi = pos[j];
        int cnt = j - (i - 1);
        for (int k = i; k < j; k++)
            pos[k] = lo + (hi - lo) * (double)(k - (i - 1)) / (double) cnt;
        i = j - 1;
    }
    // Enforce non-decreasing (a later stop before an earlier one is clamped up).
    for (int i = 1; i < n; i++) if (pos[i] < pos[i-1]) pos[i] = pos[i-1];

    for (int s = 0; s < 256; s++) {
        double t = s / 255.0;
        uint32_t col;
        if (t <= pos[0]) {
            col = g->stop_col[0];
        } else if (t >= pos[n-1]) {
            col = g->stop_col[n-1];
        } else {
            int i = 1;
            while (i < n && t > pos[i]) i++;
            if (i >= n) i = n - 1;
            double span = pos[i] - pos[i-1];
            double f = (span > 0.0) ? (t - pos[i-1]) / span : 0.0;
            uint32_t a = g->stop_col[i-1], b = g->stop_col[i];
            int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
            int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
            int r = ar + (int)((br - ar) * f + 0.5);
            int gg = ag + (int)((bg - ag) * f + 0.5);
            int bl = ab + (int)((bb - ab) * f + 0.5);
            col = ((uint32_t) r << 16) | ((uint32_t) gg << 8) | (uint32_t) bl;
        }
        lut[s] = col;   // 0x00RRGGBB == BGRA byte order for win_draw_image
    }
}

static void grad_rasterise(const cssvar_gradient *g, int w, int h, uint32_t *px) {
    uint32_t lut[256];
    grad_build_lut(g, lut);
    double cx = w / 2.0, cy = h / 2.0;

    if (g->type == 0) {
        // Linear. Screen coords (x right, y down); axis direction for a
        // CSS angle (0 = to top, clockwise) is (sin a, -cos a). The ramp
        // length is the box projected onto that axis.
        double a = g->angle * 3.14159265358979 / 180.0;
        double dx = sin(a), dy = -cos(a);
        double len = fabs(w * dx) + fabs(h * dy);
        if (len < 1.0) len = 1.0;
        for (int y = 0; y < h; y++) {
            double yy = (y + 0.5) - cy;
            uint32_t *row = px + (long) y * w;
            for (int x = 0; x < w; x++) {
                double proj = ((x + 0.5) - cx) * dx + yy * dy;
                double t = proj / len + 0.5;
                int idx = (int)(t * 255.0 + 0.5);
                if (idx < 0) idx = 0; else if (idx > 255) idx = 255;
                row[x] = lut[idx];
            }
        }
    } else {
        // Radial from the box centre. circle: farthest-corner radius;
        // ellipse: farthest-side radii (rx=w/2, ry=h/2).
        double rx, ry;
        if (g->shape == 1) { double r = sqrt(cx*cx + cy*cy); rx = ry = (r > 0 ? r : 1); }
        else { rx = (cx > 0 ? cx : 1); ry = (cy > 0 ? cy : 1); }
        for (int y = 0; y < h; y++) {
            double ey = ((y + 0.5) - cy) / ry;
            uint32_t *row = px + (long) y * w;
            for (int x = 0; x < w; x++) {
                double ex = ((x + 0.5) - cx) / rx;
                double t = sqrt(ex*ex + ey*ey);
                int idx = (int)(t * 255.0 + 0.5);
                if (idx < 0) idx = 0; else if (idx > 255) idx = 255;
                row[x] = lut[idx];
            }
        }
    }
}

static void grad_cache_free(void) {
    for (int i = 0; i < g_gradc_n; i++)
        if (g_gradc[i].px) { free(g_gradc[i].px); g_gradc[i].px = 0; }
    g_gradc_n = 0;
}

// Return a rasterised buffer for (item,w,h,gidx), rendering and caching on a
// miss. NULL if the box is degenerate, the table index is stale, or memory is
// short (the caller then paints nothing new, so the box keeps its solid bg).
static uint32_t *grad_cache_get(int item, int w, int h, int gidx) {
    if (w <= 0 || h <= 0) return 0;
    // A very large box would allocate a huge buffer; cap total pixels.
    if ((long) w * h > 4L * 1024 * 1024) return 0;
    for (int i = 0; i < g_gradc_n; i++)
        if (g_gradc[i].item == item && g_gradc[i].w == w && g_gradc[i].h == h)
            return g_gradc[i].px;
    const cssvar_gradient *g = cssvar_gradient_get(gidx);
    if (!g) return 0;
    if (g_gradc_n >= GRAD_MAX) grad_cache_free();   // page churn: reclaim
    uint32_t *px = (uint32_t *) malloc((long) w * h * 4);
    if (!px) return 0;
    grad_rasterise(g, w, h, px);
    g_gradc[g_gradc_n].item = item; g_gradc[g_gradc_n].w = w;
    g_gradc[g_gradc_n].h = h; g_gradc[g_gradc_n].px = px;
    g_gradc_n++;
    return px;
}

// ============================================================================
// #245 engletsp: CSS letter-spacing at paint time.
//
// WHY THE PAINTER HAS TO CARE. layout.c adds letter-spacing to the advance it
// gives a run, which is what makes the box the right width and the wrap point
// right. The OS text call draws a whole string in ONE go, advancing the pen by
// the font's own per-glyph step, so if the painter used it unchanged the glyphs
// would sit tight at the left of a box laid out wide: measured width would stop
// equalling drawn width, which is exactly the invariant #589 exists to hold.
//
// So a run with non-zero letter-spacing is drawn one glyph at a time. The step
// between glyphs is recovered from SUFFIX widths:
//
//     step(i) = measure(text + i) - measure(text + i + 1)
//
// which is the glyph's advance PLUS its kern pair with the next glyph, because
// ttf_measure_string_f() and every draw path share one cursor step (#589). That
// matters: chopping the string into one-character measures instead would drop
// every kerning pair and the run would come out narrower than layout was told.
// The suffix differences telescope, so with letter_spacing 0 this loop lands
// every glyph on exactly the pixel the single-call path would have.
//
// COST, stated rather than hidden: one extra SYS_MEASURE_TTF per glyph of a
// spaced run, per repaint, for the visible items only. Runs with no tracking
// (every run on a page that never authors the property) take the untouched
// single-call path and pay nothing.
static int run_drawn_w(const layout_item *it) {
    int w = ttf_measure_ex(it->text, it->face, it->size, it->fstyle);
    if (it->letter_spacing) {
        int n = 0;
        while (it->text[n]) n++;   // post-squash Latin-1: one byte, one glyph
        w += it->letter_spacing * n;
    }
    return w;
}

static void draw_run_spaced(int win, int x, int y, const layout_item *it) {
    char g[2];
    int i = 0;
    int cur = ttf_measure_ex(it->text, it->face, it->size, it->fstyle);
    g[1] = '\0';
    while (it->text[i]) {
        int next = it->text[i + 1]
                 ? ttf_measure_ex(it->text + i + 1, it->face, it->size, it->fstyle)
                 : 0;
        g[0] = it->text[i];
        if (g[0] != ' ')   // a space paints nothing; skip the syscall
            win_draw_text_ttf_ex(win, x, y, g, it->face, it->size,
                                 it->fstyle, it->color);
        x += (cur - next) + it->letter_spacing;
        cur = next;
        i++;
    }
}

static void draw_content(void) {
    // Render the page onto a canvas the colour the document asked for, so the
    // document's own colours read correctly regardless of the desktop theme.
    gui_fill_rect(window_handle, CONTENT_X, CONTENT_Y, CONTENT_W, CONTENT_H,
                  page_bg());
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
        int ih = (it->kind == 0) ? it->size : it->h;
        if (sy + ih < CONTENT_Y + 6) continue;   // entirely above viewport
        if (sy > max_y) continue;                  // entirely below viewport
        if (it->kind == 4) {
            // #245 enggrad: background gradient. Rasterise/cache the ramp for
            // this box, then blit it with the SAME vertical-band clip the
            // raster background-image uses (win_draw_image has no clip of its
            // own). it->bg holds the gradient-table index.
            int bx0 = pad_x + it->x;
            uint32_t *px = grad_cache_get(i, it->w, it->h, (int) it->bg);
            if (px) {
                int r0 = 0, r1 = it->h;
                int top_lim = CONTENT_Y + 1;
                int bot_lim = CONTENT_Y + CONTENT_H - 1;
                if (sy < top_lim) r0 = top_lim - sy;
                if (sy + r1 > bot_lim) r1 = bot_lim - sy;
                if (r1 > r0 && r0 < it->h)
                    win_draw_image(window_handle, bx0, sy + r0, it->w, r1 - r0,
                                   px + (long) r0 * it->w);
            }
            continue;
        }
        if (it->kind == 3) {
            int bx0 = pad_x + it->x;
            img_entry_t *e = 0;
            for (int k = 0; k < g_img_n; k++) if (g_imgs[k].item == i) { e = &g_imgs[k]; break; }
            if (e && e->px) {
                // win_draw_image has no clipping of its own, so this used to
                // require the WHOLE image to be inside the viewport before it
                // would draw anything. A 460px-tall hero screenshot in a 510px
                // viewport is only fully visible at one exact scroll position,
                // so in practice the page's largest images never appeared and
                // the empty placeholder box showed instead.
                //
                // The pixel buffer is w*h tightly packed, so a horizontal band
                // of it is just a pointer offset and a smaller height: rows
                // [r0, r1) start at px + r0*w. Clip vertically that way and let
                // the compositor handle the window edges horizontally.
                int r0 = 0, r1 = e->h;
                int top_lim = CONTENT_Y + 1;
                int bot_lim = CONTENT_Y + CONTENT_H - 1;
                if (sy < top_lim) r0 = top_lim - sy;
                if (sy + r1 > bot_lim) r1 = bot_lim - sy;
                if (r1 > r0 && r0 < e->h)
                    win_draw_image(window_handle, bx0, sy + r0, e->w, r1 - r0,
                                   e->px + (long) r0 * e->w);
            } else {
                int cx0 = CONTENT_X + 1, cx1 = CONTENT_X + CONTENT_W - 1;
                int cy0 = CONTENT_Y + 1, cy1 = CONTENT_Y + CONTENT_H - 1;
                if (scroll_max() > 0) cx1 -= SB_W;
                uint32_t ph = gui_mix(page_bg(), 0x00808080u, 40);
                clip_fill(bx0, sy, it->w, it->h, 1, ph, cx0, cy0, cx1, cy1);
                clip_fill(bx0, sy, it->w, 1, 1, gui_mix(ph, 0x00FFFFFFu, 40), cx0, cy0, cx1, cy1);
                clip_fill(bx0, sy + it->h - 1, it->w, 1, 1, gui_mix(ph, 0x00000000u, 40), cx0, cy0, cx1, cy1);
            }
            continue;
        }
        // CSS OUTLINE (#245 engoutline). Painted just OUTSIDE the border edge
        // and occupying no layout space, which is the whole difference between
        // an outline and a border: layout never saw ol_w, so the box geometry
        // below is bit-for-bit what it would have been without this block.
        //
        // AE=0 by construction: layout leaves ol_w at 0 on every item of a page
        // that never authors an outline (item_new() memsets, and LAYOUT_OL_NONE
        // is 0), so the guard is false and not one pixel differs.
        //
        // Placed BEFORE both box painters, and taking no `continue`, so it runs
        // for the rounded path and the square path alike. Consequence worth
        // stating: the ring is square even on a radius-rounded box, and a later
        // sibling's background can paint over it because real browsers draw
        // outlines in a later stacking phase and this engine has only one.
        //
        // dashed and dotted are PAINTED SOLID. The only primitive here is
        // clip_fill(), an axis-aligned opaque rect; a dash generator is out of
        // scope. it->ol_style keeps the distinction for whoever adds one.
        if (it->kind == 1 && it->ol_w > 0) {
            int ow = it->ol_w;
            int ox0 = pad_x + it->x - ow;
            int oy0 = sy - ow;
            int oww = it->w + 2 * ow;
            int ohh = it->h + 2 * ow;
            int cx0 = CONTENT_X + 1, cx1 = CONTENT_X + CONTENT_W - 1;
            int cy0 = CONTENT_Y + 1, cy1 = CONTENT_Y + CONTENT_H - 1;
            int mid = ohh - 2 * ow;
            if (scroll_max() > 0) cx1 -= SB_W;
            clip_fill(ox0, oy0, oww, ow, 1, it->ol_col, cx0, cy0, cx1, cy1);
            clip_fill(ox0, oy0 + ohh - ow, oww, ow, 1, it->ol_col,
                      cx0, cy0, cx1, cy1);
            if (mid > 0) {
                clip_fill(ox0, oy0 + ow, ow, mid, 1, it->ol_col,
                          cx0, cy0, cx1, cy1);
                clip_fill(ox0 + oww - ow, oy0 + ow, ow, mid, 1, it->ol_col,
                          cx0, cy0, cx1, cy1);
            }
        }
        if (it->kind == 1 && it->radius > 0) {
            // ROUNDED BOX. A uniform border is painted as the outer shape in
            // the border colour with the inner shape painted over it in the
            // background colour, which needs a background to paint with; a
            // bordered box with none takes the ring path instead. A box with
            // per-side borders that differ falls through to the square painter
            // rather than draw a shape that is wrong in a different way.
            int bx0 = pad_x + it->x;
            int cx0 = CONTENT_X + 1, cx1 = CONTENT_X + CONTENT_W - 1;
            int cy0 = CONTENT_Y + 1, cy1 = CONTENT_Y + CONTENT_H - 1;
            int bw0 = it->bw[LB_TOP];
            int uniform = bw0 > 0 &&
                it->bw[LB_RIGHT] == bw0 && it->bw[LB_BOTTOM] == bw0 &&
                it->bw[LB_LEFT] == bw0 &&
                it->bcol[LB_RIGHT] == it->bcol[LB_TOP] &&
                it->bcol[LB_BOTTOM] == it->bcol[LB_TOP] &&
                it->bcol[LB_LEFT] == it->bcol[LB_TOP];
            int plain = !it->bw[LB_TOP] && !it->bw[LB_RIGHT] &&
                !it->bw[LB_BOTTOM] && !it->bw[LB_LEFT];
            if (scroll_max() > 0) cx1 -= SB_W;
            if (uniform || plain) {
                g_span.cx0 = cx0; g_span.cy0 = cy0;
                g_span.cx1 = cx1; g_span.cy1 = cy1;
                if (uniform) {
                    g_span.col = it->bcol[LB_TOP];
                    if (it->has_bg)
                        rrect_spans(bx0, sy, it->w, it->h, it->radius,
                                    span_fill, 0);
                    else
                        rrect_ring_spans(bx0, sy, it->w, it->h, it->radius,
                                         bw0, span_fill, 0);
                }
                if (it->has_bg) {
                    int in = uniform ? bw0 : 0;
                    int ir = it->radius - in;
                    g_span.col = it->bg;
                    rrect_spans(bx0 + in, sy + in, it->w - 2 * in,
                                it->h - 2 * in, ir > 0 ? ir : 0,
                                span_fill, 0);
                }
                if (it->form_kind) form_hit_record(it, bx0, sy);
                continue;
            }
        }
        if (it->kind == 1) {
            // Background / border box, clipped to the content viewport on all
            // four sides. It used to be clipped vertically only, so a box wider
            // than the viewport painted straight over the scrollbar gutter.
            int bx0 = pad_x + it->x;
            int by0 = sy, by1 = sy + it->h;
            int cx0 = CONTENT_X + 1, cx1 = CONTENT_X + CONTENT_W - 1;
            int cy0 = CONTENT_Y + 1, cy1 = CONTENT_Y + CONTENT_H - 1;
            if (scroll_max() > 0) cx1 -= SB_W;
            clip_fill(bx0, by0, it->w, it->h, it->has_bg ? 1 : 0, it->bg,
                      cx0, cy0, cx1, cy1);
            // Per-side borders. A single width painted on the top and bottom
            // edges gave every border-bottom a phantom top rule and dropped the
            // left and right edges of every card.
            if (it->bw[LB_TOP])
                clip_fill(bx0, by0, it->w, it->bw[LB_TOP], 1, it->bcol[LB_TOP],
                          cx0, cy0, cx1, cy1);
            if (it->bw[LB_BOTTOM])
                clip_fill(bx0, by1 - it->bw[LB_BOTTOM], it->w, it->bw[LB_BOTTOM],
                          1, it->bcol[LB_BOTTOM], cx0, cy0, cx1, cy1);
            if (it->bw[LB_LEFT])
                clip_fill(bx0, by0, it->bw[LB_LEFT], it->h, 1, it->bcol[LB_LEFT],
                          cx0, cy0, cx1, cy1);
            if (it->bw[LB_RIGHT])
                clip_fill(bx0 + it->w - it->bw[LB_RIGHT], by0, it->bw[LB_RIGHT],
                          it->h, 1, it->bcol[LB_RIGHT], cx0, cy0, cx1, cy1);
            if (it->form_kind) form_hit_record(it, bx0, sy);
            continue;
        }
        int sx = pad_x + it->x;
        // #245: draw in the (face, style) the run was MEASURED in. The old call
        // could only ever use the active face, and faked bold by redrawing at
        // x+1, which painted the run one pixel wider than the width layout had
        // been told. A real bold face exists for every family this resolves to;
        // where one does not, fstyle carries the bit and the rasteriser
        // emboldens with an advance that measure() sees too.
        // #245 engletsp: letter_spacing is 0 for every run on a page that does
        // not author the property, and that case takes the original single
        // call below, unchanged.
        if (it->letter_spacing)
            draw_run_spaced(window_handle, sx, sy, it);
        else
            win_draw_text_ttf_ex(window_handle, sx, sy, it->text,
                                 it->face, it->size, it->fstyle, it->color);
        if (it->href[0] && g_link_hit_n < 512) {
            link_hit_t *lh = &g_link_hits[g_link_hit_n++];
            lh->x = sx; lh->y = sy;
            lh->w = run_drawn_w(it);
            lh->h = it->size + 3;
            int k = 0;
            while (it->href[k] && k < 255) { lh->href[k] = it->href[k]; k++; }
            lh->href[k] = 0;
        }
        if (it->underline) {
            int w = run_drawn_w(it);
            gui_fill_rect(window_handle, sx, sy + it->size,
                          w, 1, it->color);
        }
    }
    draw_scrollbar();
}

// Slim status bar pinned to the bottom of the window. While a page is
// loading this shows REAL progress (#25): the live phase from
// SYS_HTTP_FETCH_PROGRESS, and a shared-style gui_progress() bar whenever the
// server sent a Content-Length. No Content-Length means no bar - a guessed
// percentage would be worse than none, so it is withheld, not faked.
// (browserglass) The status ROW lives inside the panel now (STATUS_Y, under
// the viewport); its band is refilled flat every draw so a changed string
// never smears (the Image Viewer's status row does the same).
static void draw_status_bar(void) {
    int sy = STATUS_Y;
    gui_fill_rect(window_handle, INNER_X, sy, INNER_W, STATUS_H, COL_PANEL);

    uint32_t ink = last_was_error ? COL_ERROR : COL_TEXT_DIM;
    if (is_loading) ink = COL_ACCENT;

    // Small dot as a loading/status indicator.
    gui_fill_circle_aa(window_handle, INNER_X, sy + (STATUS_H - 8) / 2, 8,
                       is_loading ? COL_ACCENT : (last_was_error ? COL_ERROR : COL_TEXT_DIM),
                       COL_PANEL);

    int text_x = INNER_X + 16;
    int text_y = sy + (STATUS_H - 11) / 2 - 1;
    if (is_loading) {
        char line[160];
        int pct = fetch_progress_text(line, sizeof(line));
        if (pct >= 0) {
            int bar_w = 120;
            int bar_x = INNER_X + INNER_W - bar_w;
            int bar_y = sy + (STATUS_H - 8) / 2;
            gui_progress(window_handle, bar_x, bar_y, bar_w, 8, pct);
        }
        br_text_sz(window_handle, text_x, text_y, line, 11, ink);
        return;
    }

    const char *msg = status_msg[0] ? status_msg : "Ready";
    br_text_sz(window_handle, text_x, text_y, msg, 11, ink);
}

// (browserglass) The glass panel: soft shadow, AA rounded fill, 1px border,
// 1px top highlight; the same layering the Task Manager's, the Calculator's
// and the Image Viewer's draw_panel() use. Every outer colour is sampled from
// the backdrop under that edge, so the fringe and corners match what the
// blit put there. Drawn EVERY frame on purpose: the same inputs give the
// same pixels, so the repaint is idempotent (section 11: only the backdrop
// blit is a commit on the chrome side).
static void draw_panel(int x, int y, int w, int h) {
    uint32_t below = bd_at(x + w / 2, y + h + 3);
    // Mean of the four corner samples: gui_fill_rounded_aa takes ONE outer
    // colour, and the blur keeps the four within a few levels of each other.
    uint32_t c0 = bd_at(x + 4, y + 4),     c1 = bd_at(x + w - 4, y + 4),
             c2 = bd_at(x + 4, y + h - 4), c3 = bd_at(x + w - 4, y + h - 4);
    uint32_t outer = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        uint32_t m = (((c0 >> sh) & 0xFF) + ((c1 >> sh) & 0xFF) + ((c2 >> sh) & 0xFF) + ((c3 >> sh) & 0xFF)) / 4;
        outer |= m << sh;
    }
    gui_soft_shadow(window_handle, x, y + 2, w, h, PANEL_R, below);
    gui_fill_rounded_aa(window_handle, x, y, w, h, PANEL_R, COL_PANEL, outer);
    gui_rounded_border(window_handle, x, y, w, h, PANEL_R, COL_SEPARATOR);
    win_draw_rect(window_handle, x + PANEL_R, y + 1, w - 2 * PANEL_R, 1, gui_lighten(COL_PANEL, 16));
}

// Redraw the whole window.
//
// (browserglass) THE ANTI-FLASH CONTRACT (docs/UI_GLASS_DESIGN_SYSTEM.md
// section 11). The backdrop blit self-commits, so it runs ONLY when the
// chrome is dirty: start, EVENT_RESIZE, EVENT_REDRAW, a wallpaper change, or
// a change in the number of tab pills (they get wider or narrower and would
// leave stale pill pixels on the backdrop). Everything else is a plain draw
// onto the panel, refilled every frame. The page viewport is drawn LAST
// among the chrome-adjacent bands (draw_content() is the engine's paint and
// is not edited here; its inline-image win_draw_image calls self-commit, as
// they always did, but by then every band above and beside the page is
// already in the content buffer). The window is never cleared with a flat
// fill: the panel and pills cover every pixel that changes, and the margins
// are the backdrop.
static void redraw(void) {
    unsigned long t_r0 = uptime_ms();
    sync_backdrop();
    if (g_tab_count != g_last_tab_count) {
        g_last_tab_count = g_tab_count;
        g_chrome_dirty = 1;
    }
    if (g_chrome_dirty) {
        gui_glass_backdrop_blit(window_handle, g_bd);
        g_chrome_dirty = 0;
    }
    draw_panel(PAD, PANEL_Y, WIN_WIDTH - 2 * PAD, WIN_HEIGHT - PAD - PANEL_Y);
    draw_tabstrip();
    draw_toolbar();
    draw_bookmarks_bar();
    draw_progress_line();
    draw_status_bar();
    draw_content();
    draw_settings_menu();   // drawn last so it overlays the content below it
    win_invalidate(window_handle);
#ifdef BROWSER_PERF
    printf("[BRPERF] redraw=%lu ms items=%d\n", uptime_ms() - t_r0,
           g_layout.n_items);
#else
    (void) t_r0;
#endif
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

// #25: real, event-driven load progress (not a fake timer animation).
// Refreshed from SYS_HTTP_FETCH_PROGRESS every poll_fetch() tick while a
// fetch is in flight; drives both the status-bar text and its progress bar.
static int          g_fetch_phase       = HTTP_PHASE_IDLE;
static unsigned int g_fetch_bytes       = 0;
static unsigned int g_fetch_content_len = 0;

// Format a byte count as "N B" / "N.F KB" / "N.F MB" into buf (>=16 bytes).
static void fmt_size(unsigned int n, char *buf) {
    if (n < 1024) {
        int_to_str((int) n, buf);
        int p = str_len(buf); buf[p++] = ' '; buf[p++] = 'B'; buf[p] = 0;
        return;
    }
    unsigned int whole, frac; const char *unit;
    if (n < 1024u * 1024u) { whole = n / 1024; frac = (n % 1024) * 10 / 1024; unit = "KB"; }
    else { whole = n / (1024u * 1024u); frac = (n % (1024u * 1024u)) * 10 / (1024u * 1024u); unit = "MB"; }
    int_to_str((int) whole, buf);
    int p = str_len(buf);
    buf[p++] = '.'; buf[p++] = (char) ('0' + frac); buf[p++] = ' ';
    buf[p++] = unit[0]; buf[p++] = unit[1]; buf[p] = 0;
}

static const char *phase_word(int phase) {
    switch (phase) {
        case HTTP_PHASE_RESOLVING:  return "Resolving host";
        case HTTP_PHASE_CONNECTING: return "Connecting";
        case HTTP_PHASE_TLS:        return "Securing connection";
        case HTTP_PHASE_SENDING:    return "Sending request";
        case HTTP_PHASE_RECEIVING:  return "Receiving";
        default:                    return "Loading";
    }
}

// Build the live status line ("Connecting..." / "Receiving 42 KB of 118 KB
// (35%)") and return the known percentage, or -1 when the server did not
// send a Content-Length (the caller then withholds the progress bar instead
// of guessing one).
static int fetch_progress_text(char *out, int cap) {
    if (g_fetch_phase == HTTP_PHASE_RECEIVING && g_fetch_bytes > 0) {
        char bb[16];
        fmt_size(g_fetch_bytes, bb);
        if (g_fetch_content_len > 0) {
            char cb[16]; fmt_size(g_fetch_content_len, cb);
            int pct = (int) (((unsigned long long) g_fetch_bytes * 100) / g_fetch_content_len);
            if (pct > 100) pct = 100;
            int p = 0;
            const char *pfx = "Receiving ";
            for (int i = 0; pfx[i] && p < cap - 1; i++) out[p++] = pfx[i];
            for (int i = 0; bb[i] && p < cap - 1; i++) out[p++] = bb[i];
            const char *of = " of ";
            for (int i = 0; of[i] && p < cap - 1; i++) out[p++] = of[i];
            for (int i = 0; cb[i] && p < cap - 1; i++) out[p++] = cb[i];
            if (p < cap - 1) out[p++] = ' ';
            if (p < cap - 1) out[p++] = '(';
            char pb[8]; int_to_str(pct, pb);
            for (int i = 0; pb[i] && p < cap - 1; i++) out[p++] = pb[i];
            if (p < cap - 2) { out[p++] = '%'; out[p++] = ')'; }
            out[p] = 0;
            return pct;
        }
        int p = 0;
        const char *pfx = "Receiving ";
        for (int i = 0; pfx[i] && p < cap - 1; i++) out[p++] = pfx[i];
        for (int i = 0; bb[i] && p < cap - 1; i++) out[p++] = bb[i];
        out[p] = 0;
        return -1;
    }
    const char *w = phase_word(g_fetch_phase);
    int p = 0;
    for (int i = 0; w[i] && p < cap - 1; i++) out[p++] = w[i];
    if (p < cap - 3) { out[p++] = '.'; out[p++] = '.'; out[p++] = '.'; }
    out[p] = 0;
    return -1;
}

// Thin accent progress line under the bookmarks bar (mockup's ".prog"). A
// REAL fraction when the server sent a Content-Length (the same rule
// draw_status_bar()'s bar already follows - a guessed percentage would be
// worse than none); otherwise a small fixed sliver as an honest "something
// is happening" indicator, never a fabricated percentage.
static void draw_progress_line(void) {
    // (browserglass) Inside the panel, spanning the viewport width: the panel
    // colour when idle (invisible), the DK_PROGRESS_TRACK plus the accent
    // fraction while loading.
    gui_fill_rect(window_handle, INNER_X, PROGRESS_Y, INNER_W, PROGRESS_H, COL_PANEL);
    if (!is_loading) return;
    gui_fill_rect(window_handle, INNER_X, PROGRESS_Y, INNER_W, PROGRESS_H, COL_TRACK);
    int pct = 8;
    if (g_fetch_phase == HTTP_PHASE_RECEIVING && g_fetch_content_len > 0) {
        pct = (int) (((unsigned long long) g_fetch_bytes * 100) / g_fetch_content_len);
        if (pct > 100) pct = 100;
        if (pct < 8) pct = 8;
    }
    int w = (INNER_W * pct) / 100;
    gui_fill_rect(window_handle, INNER_X, PROGRESS_Y, w, PROGRESS_H, COL_ACCENT);
}

static void poll_fetch(void) {
    // Waiting out the #549 breaker's 30s re-probe interval (see the note by
    // g_faulty_wait). One syscall every BR_FAULTY_RETRY_MS, bounded.
    if (g_faulty_wait) {
        unsigned long now = uptime_ms();
        if (now < g_faulty_next_ms) return;
        g_faulty_next_ms = now + BR_FAULTY_RETRY_MS;
        int id = http_fetch_start(url_buffer);
        if (id >= 0) {                     // admitted: resume the normal path
            g_faulty_wait = 0;
            g_fetch_id = id;
            g_fetch_phase = HTTP_PHASE_IDLE;
            g_fetch_bytes = 0; g_fetch_content_len = 0;
            str_cpy(status_msg, "Loading...");
            redraw();
            return;
        }
        if (id == NET_ERR_FAULTY && now < g_faulty_until_ms) return;   // keep waiting
        // Either a different failure, or the window expired with the interface
        // still faulty. Report the REAL reason: "Fetch failed" sent a previous
        // investigation after DNS.
        g_faulty_wait = 0;
        is_loading = 0;
        g_fetch_phase = HTTP_PHASE_ERROR;
        str_cpy(status_msg,
                id == NET_ERR_FAULTY
                    ? "Network disabled by the connectivity breaker (NET_FAULTY). "
                      "Try Settings > Network > reconnect, or run nslookup."
                    : "Fetch failed");
        last_was_error = 1;
        str_cpy(display_buffer, status_msg);
        display_length = str_len(display_buffer);
        redraw();
        return;
    }
    if (!is_loading || g_fetch_id < 0) return;
    int status = 0;
    int st = http_fetch_poll(g_fetch_id, &status, 0);
    if (st == 0) {
        // #25: still downloading - pull the REAL phase/byte-count from the
        // kernel job (net/http_progress.h) and redraw only when it actually
        // changed, so this stays a live meter, not a fake tick-driven one.
        int phase = g_fetch_phase;
        unsigned int bytes = g_fetch_bytes, clen = g_fetch_content_len;
        if (http_fetch_progress(g_fetch_id, &phase, &bytes, &clen) == 0 &&
            (phase != g_fetch_phase || bytes != g_fetch_bytes || clen != g_fetch_content_len)) {
            g_fetch_phase = phase; g_fetch_bytes = bytes; g_fetch_content_len = clen;
            draw_status_bar(); win_invalidate(window_handle);
        }
        return;
    }
    if (st == 1) {                             // complete
        int n = http_fetch_read(g_fetch_id, content_buffer, MAX_CONTENT - 1);
        g_fetch_id = -1;
        g_fetch_phase = HTTP_PHASE_DONE;
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
        // #25: a real (not timer-based) parsing/rendering phase. The fetch
        // itself is over, but HTML/CSS/JS parse + layout (render_page(),
        // which can take a visible moment on a heavy page) has not run yet.
        // is_loading stays 1 through the call so the dot/bar keep reading
        // "busy" instead of going idle and then jumping straight to "Done".
        {
            char final_msg[160]; str_cpy(final_msg, status_msg);
            str_cpy(status_msg, "Parsing and rendering page...");
            redraw();
            render_page(content_buffer, n);
            // [no-ticket] (browser-glass): a tab switch/close set this to the
            // scroll offset that tab had when it was last left, then drove a
            // fresh navigate() through the one shared render pipeline (see
            // the Tabs scope comment by g_tabs). Apply it now that the new
            // layout actually exists, clamped to what the freshly laid-out
            // page can scroll to.
            if (g_pending_scroll_restore >= 0) {
                scroll_offset = g_pending_scroll_restore;
                int m = scroll_max();
                if (scroll_offset > m) scroll_offset = m;
                if (scroll_offset < 0) scroll_offset = 0;
                g_pending_scroll_restore = -1;
            }
            str_cpy(status_msg, final_msg);
        }
        is_loading = 0;
        if (g_fetch_record) hist_push(url_buffer);
        url_fresh = 1;
        redraw();
    } else {                                   // error -> one retry, then report
        http_fetch_read(g_fetch_id, content_buffer, 0);   // free the job
        g_fetch_id = -1;
        if (g_fetch_retry < 1) {
            g_fetch_retry++;
            g_fetch_phase = HTTP_PHASE_IDLE; g_fetch_bytes = 0; g_fetch_content_len = 0;
            g_fetch_id = http_fetch_start(url_buffer);
            if (g_fetch_id >= 0) return;
            if (g_fetch_id == NET_ERR_FAULTY) {   // breaker tripped mid-load
                g_fetch_id = -1;
                g_faulty_wait     = 1;
                g_faulty_next_ms  = uptime_ms() + BR_FAULTY_RETRY_MS;
                g_faulty_until_ms = uptime_ms() + BR_FAULTY_WINDOW_MS;
                str_cpy(status_msg,
                        "Network disabled after repeated failures; retrying...");
                redraw();
                return;
            }
            g_fetch_id = -1;
        }
        is_loading = 0;
        g_fetch_phase = HTTP_PHASE_ERROR;
        br_set_fetch_error("Fetch failed");   // #netfix2
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
    g_fetch_phase = HTTP_PHASE_IDLE; g_fetch_bytes = 0; g_fetch_content_len = 0;   // #25
    g_fetch_id = http_fetch_start(url_buffer);
    if (g_fetch_id == NET_ERR_FAULTY) {
        // The breaker refused this request. Do NOT fall through to the two
        // synchronous fetch_page() calls below: they are gated on the same
        // breaker and would be refused in the same instant, turning a
        // recoverable wait into a hard failure. Wait for the re-probe window.
        g_fetch_id       = -1;   // -3 is not a job id; do not leave it in the slot
        g_faulty_wait    = 1;
        g_faulty_next_ms = uptime_ms() + BR_FAULTY_RETRY_MS;
        g_faulty_until_ms = uptime_ms() + BR_FAULTY_WINDOW_MS;
        is_loading = 1;
        last_was_error = 0;
        str_cpy(status_msg,
                "Network disabled after repeated failures; retrying...");
        redraw();
        return;
    }
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
        int already = 0;   // #resizereflow: skip images already decoded (re-keyed across a reflow)
        for (int k = 0; k < g_img_n; k++) if (g_imgs[k].item == i) { already = 1; break; }
        if (already) continue;
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

// #25: cancel the in-flight fetch. http_fetch_cancel() / SYS_HTTP_FETCH_CANCEL
// already existed (#277) but had no caller anywhere in the browser - progress
// without a way to interrupt it is only half the feature.
static void stop_load(void) {
    g_faulty_wait = 0;   // browsenet: Stop also cancels a breaker re-probe wait
    if (g_fetch_id >= 0) { http_fetch_cancel(g_fetch_id); g_fetch_id = -1; }
    is_loading = 0;
    g_fetch_phase = HTTP_PHASE_IDLE; g_fetch_bytes = 0; g_fetch_content_len = 0;
    str_cpy(status_msg, "Stopped");
    last_was_error = 0;
    redraw();
}

// ============================================================================
// Tabs [no-ticket] (browser-glass) - see the scope comment by g_tabs above.
// ============================================================================

// Short label for a tab: strip the scheme, keep host (+ a hint of path), cut
// to a length the tab strip can actually show. Cheap and URL-derived rather
// than a tracked <title> (the NetSurf pipeline does not surface a document
// title to this app today; adding that is a separate, engine-side change).
static void tab_label(const char *url, char *out, int cap) {
    const char *p = url;
    if (str_ncmp(p, "https://", 8) == 0) p += 8;
    else if (str_ncmp(p, "http://", 7) == 0) p += 7;
    int n = 0;
    while (p[n] && n < cap - 1) n++;
    if (n == 0) { str_cpy(out, "New Tab"); return; }
    int i = 0;
    for (; i < n && i < cap - 1; i++) out[i] = p[i];
    out[i] = 0;
}

// Save the currently-live working state (url_buffer/hist[]/scroll_offset)
// back into g_tabs[g_tab_active]. Call before switching the active index.
static void tab_save_active(void) {
    if (g_tab_active < 0 || g_tab_active >= g_tab_count) return;
    tab_t *t = &g_tabs[g_tab_active];
    str_cpy(t->url, url_buffer);
    for (int i = 0; i < HIST_MAX; i++) str_cpy(t->hist[i], hist[i]);
    t->hist_count = hist_count;
    t->hist_pos = hist_pos;
    t->scroll_offset = scroll_offset;
}

// Load g_tabs[idx] into the live working state and drive a real navigation
// (see the Tabs scope comment: this is a genuine re-fetch, not a frozen
// thumbnail swap). record_history=0: the tab's own history is already
// correct, this is not a new navigation event.
static void tab_load_into_live(int idx) {
    tab_t *t = &g_tabs[idx];
    str_cpy(url_buffer, t->url);
    url_cursor = str_len(url_buffer);
    for (int i = 0; i < HIST_MAX; i++) str_cpy(hist[i], t->hist[i]);
    hist_count = t->hist_count;
    hist_pos = t->hist_pos;
    g_pending_scroll_restore = t->scroll_offset;
    if (is_loading) stop_load();
    navigate(0);
}

static void tab_switch(int idx) {
    if (idx < 0 || idx >= g_tab_count || idx == g_tab_active) return;
    tab_save_active();
    g_tab_active = idx;
    tab_load_into_live(idx);
}

static void tab_new(void) {
    if (g_tab_count >= MAX_TABS) return;   // fixed-size array; a real limit, shown by a disabled "+" is future polish
    tab_save_active();
    int idx = g_tab_count++;
    tab_t *t = &g_tabs[idx];
    str_cpy(t->url, HOME_URL);
    t->hist_count = 0;
    t->hist_pos = -1;
    t->scroll_offset = 0;
    t->chip_col = TAB_CHIP_COLORS[idx % TAB_CHIP_N];
    g_tab_active = idx;
    tab_load_into_live(idx);
}

static void tab_close(int idx) {
    if (idx < 0 || idx >= g_tab_count) return;
    if (g_tab_count <= 1) {
        // Last tab: close-to-home rather than leaving an empty tab strip.
        go_home();
        return;
    }
    int was_active = (idx == g_tab_active);
    for (int i = idx; i < g_tab_count - 1; i++) g_tabs[i] = g_tabs[i + 1];
    g_tab_count--;
    if (g_tab_active > idx) {
        g_tab_active--;
    } else if (was_active) {
        if (g_tab_active >= g_tab_count) g_tab_active = g_tab_count - 1;
        tab_load_into_live(g_tab_active);
    }
    if (g_tab_hover >= g_tab_count) g_tab_hover = -1;
}

// ============================================================================
// Bookmarks bar [no-ticket] (browser-glass)
// ============================================================================

static int bookmark_find(const char *url) {
    for (int i = 0; i < g_bookmark_count; i++)
        if (str_ncmp(g_bookmarks[i].url, url, MAX_URL_LEN) == 0) return i;
    return -1;
}

// Star click: add the current page if not already bookmarked, else remove it.
// Session-only (no persistence) - a real bookmarks file is a nice-to-have
// noted in CHANGELOG.md, not required for this restyle.
static void bookmark_toggle_current(void) {
    int existing = bookmark_find(url_buffer);
    if (existing >= 0) {
        for (int i = existing; i < g_bookmark_count - 1; i++) g_bookmarks[i] = g_bookmarks[i + 1];
        g_bookmark_count--;
        return;
    }
    if (g_bookmark_count >= MAX_BOOKMARKS) return;   // full: silently ignore rather than corrupt the array
    bookmark_t *b = &g_bookmarks[g_bookmark_count];
    tab_label(url_buffer, b->label, sizeof(b->label));
    str_cpy(b->url, url_buffer);
    b->chip_col = TAB_CHIP_COLORS[g_bookmark_count % TAB_CHIP_N];
    g_bookmark_count++;
}

static void bookmark_navigate(int idx) {
    if (idx < 0 || idx >= g_bookmark_count) return;
    str_cpy(url_buffer, g_bookmarks[idx].url);
    url_cursor = str_len(url_buffer);
    navigate(1);
}

// ============================================================================
// Settings (overflow) menu [no-ticket] (browser-glass)
// ============================================================================

// "Reconnect": clear any #549 breaker wait state and retry the current page
// immediately, rather than waiting out the rest of the 30s re-probe window.
static void settings_reconnect(void) {
    g_faulty_wait = 0;
    load_current_url();
}

static void settings_about(void) {
    str_cpy(status_msg, "MayteraOS Browser - NetSurf render pipeline, Duktape JS");
    last_was_error = 0;
    redraw();
}

static void settings_menu_action(int item) {
    g_settings_open = 0;
    switch (item) {
        case 0: go_home(); break;
        case 1: settings_reconnect(); break;
        case 2: settings_about(); break;
        default: redraw(); break;
    }
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

    // engscroll (#245): keyboard scrolling of an overflow:scroll / overflow:auto
    // inner container. Only reachable when the page actually has one (n_scrolls
    // > 0), so a normal page keeps its exact old behaviour (AE=0). For the
    // bounded common case (the page itself fits, one inner scroll box) the
    // arrow / page keys drive the active box; ] and [ always drive it and }
    // (Shift-]) cycles which box is active. Mouse-drag thumbs are deferred:
    // #334 makes pointer injection unreliable, which is why keyboard drives it.
    if (g_layout.n_scrolls > 0) {
        if (g_active_scroll < 0 || g_active_scroll >= g_layout.n_scrolls)
            g_active_scroll = 0;
        int page_can_scroll = (scroll_max() > 0);
        int delta = 0, cycle = 0;
        int boxpg = g_layout.scrolls[g_active_scroll].content_h - 20;
        if (boxpg < 20) boxpg = 20;
        switch (event->keycode) {
            case 0x80: if (!page_can_scroll) delta = -40; break;     // Up
            case 0x81: if (!page_can_scroll) delta =  40; break;     // Down
            case GUI_KEY_PGUP: if (!page_can_scroll) delta = -boxpg; break;
            case GUI_KEY_PGDN: if (!page_can_scroll) delta =  boxpg; break;
            default: break;
        }
        if (!url_focused) {
            if (c == ']') delta =  40;
            else if (c == '[') delta = -40;
            else if (c == '}') cycle = 1;
        }
        if (cycle) {
            g_active_scroll = (g_active_scroll + 1) % g_layout.n_scrolls;
            draw_content(); win_invalidate(window_handle);
            return;
        }
        if (delta != 0) {
            int mo = g_layout.scrolls[g_active_scroll].extent_h -
                     g_layout.scrolls[g_active_scroll].content_h;
            if (mo < 0) mo = 0;
            g_box_scroll[g_active_scroll] += delta;
            if (g_box_scroll[g_active_scroll] < 0) g_box_scroll[g_active_scroll] = 0;
            if (g_box_scroll[g_active_scroll] > mo) g_box_scroll[g_active_scroll] = mo;
            relayout_page();
            draw_content(); win_invalidate(window_handle);
            return;
        }
        // brhscroll (#245): horizontal scrolling of the active overflow box.
        // Left/Right (and , / .) drive it only when the address bar is NOT
        // focused, so URL caret movement is unaffected. The browser page itself
        // never scrolls horizontally, so these keys are free here.
        // Arrows drive horizontal scroll when the ACTIVE box actually overflows
        // horizontally (extent_w > content_w), regardless of address-bar focus,
        // exactly as vertical Up/Down drive a vertical box. This can never fire
        // on a page whose active box has no horizontal overflow (AE=0), so URL
        // Left/Right caret editing is unchanged on all normal and vertical pages.
        int hbox = (g_layout.scrolls[g_active_scroll].extent_w >
                    g_layout.scrolls[g_active_scroll].content_w);
        int delta_x = 0;
        if (hbox) {
            if (event->keycode == GUI_KEY_LEFT) delta_x = -40;
            else if (event->keycode == GUI_KEY_RIGHT) delta_x = 40;
        }
        if (!url_focused) {
            if (c == '.') delta_x = 40;
            else if (c == ',') delta_x = -40;
        }
        if (delta_x != 0) {
            int mox = g_layout.scrolls[g_active_scroll].extent_w -
                      g_layout.scrolls[g_active_scroll].content_w;
            if (mox < 0) mox = 0;
            g_box_scroll_x[g_active_scroll] += delta_x;
            if (g_box_scroll_x[g_active_scroll] < 0) g_box_scroll_x[g_active_scroll] = 0;
            if (g_box_scroll_x[g_active_scroll] > mox) g_box_scroll_x[g_active_scroll] = mox;
            relayout_page();
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
            case GUI_KEY_PGUP: scroll_offset -= page; break;
            case GUI_KEY_PGDN: scroll_offset += page; break;
            case GUI_KEY_HOME: scroll_offset = 0; break;
            case GUI_KEY_END:  scroll_offset = m; break;
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
// Index 5 is Settings; see nav_btn_rect()'s comment on why this can never
// drift from what draw_toolbar() actually drew.
static int btn_at(int mx, int my) {
    for (int i = 0; i <= 5; i++) {
        int bx, by, bw, bh;
        nav_btn_rect(i, &bx, &by, &bw, &bh);
        if (point_in(mx, my, bx, by, bw, bh)) return i;
    }
    return -1;
}

static void handle_mouse_move(gui_event_t *event) {
    if (sb_dragging) { scrollbar_drag_to(event->mouse_y); redraw(); return; }
    int mx = event->mouse_x, my = event->mouse_y;
    int need_invalidate = 0;

    if (g_settings_open) {
        // Small, cheap surface: just repaint it on every move while open, for
        // the item hover highlight (draw_settings_menu() reads g_mx_last/
        // g_my_last, updated below).
        g_mx_last = mx; g_my_last = my;
        draw_settings_menu();
        win_invalidate(window_handle);
        return;
    }

    int idx = btn_at(mx, my);
    if (idx != btn_hover) {
        btn_hover = idx;
        draw_toolbar();
        need_invalidate = 1;
    }

    // Tab hover (and whether the hover is specifically over its close-x),
    // read from the SAME tab_rect()/tab_close_rect() draw_tabstrip() uses.
    int new_tab_hover = -1, new_close_hover = 0;
    if (my < TABSTRIP_H) {
        for (int i = 0; i < g_tab_count; i++) {
            int tx, ty, tw, th;
            tab_rect(i, &tx, &ty, &tw, &th);
            if (point_in(mx, my, tx, ty, tw, th)) {
                new_tab_hover = i;
                int xx, xy, xw, xh;
                tab_close_rect(i, &xx, &xy, &xw, &xh);
                new_close_hover = point_in(mx, my, xx, xy, xw, xh);
                break;
            }
        }
    }
    if (new_tab_hover != g_tab_hover || new_close_hover != g_tab_close_hover) {
        g_tab_hover = new_tab_hover;
        g_tab_close_hover = new_close_hover;
        draw_tabstrip();
        need_invalidate = 1;
    }

    // Bookmarks-bar chip hover reads g_mx_last/g_my_last live in
    // draw_bookmarks_bar(), so repaint it on entry, movement within, or exit.
    int was_in_bmk = (g_my_last >= BOOKMARKS_Y && g_my_last < BOOKMARKS_Y + BOOKMARKS_H);
    int now_in_bmk = (my >= BOOKMARKS_Y && my < BOOKMARKS_Y + BOOKMARKS_H);
    g_mx_last = mx; g_my_last = my;
    if (was_in_bmk || now_in_bmk) {
        draw_bookmarks_bar();
        need_invalidate = 1;
    }

    if (need_invalidate) win_invalidate(window_handle);
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

    // Settings menu: while open, this click either picks an item or (any
    // other target, including the gear itself) dismisses it. Handled first
    // so one click can never both close the menu and fire an unrelated
    // control underneath it.
    if (g_settings_open) {
        int mmx, mmy, mmw, mmh;
        settings_menu_rect(&mmx, &mmy, &mmw, &mmh);
        if (point_in(mx, my, mmx, mmy, mmw, mmh)) {
            for (int i = 0; i < SETTINGS_ITEM_N; i++) {
                int ix, iy, iw, ih;
                settings_item_rect(i, &ix, &iy, &iw, &ih);
                if (point_in(mx, my, ix, iy, iw, ih)) { settings_menu_action(i); return; }
            }
            return;   // inside the menu but not on an item
        }
        g_settings_open = 0;
        redraw();
        return;
    }

    int pressed = btn_press;
    btn_press = -1;

    int idx = btn_at(mx, my);

    // Click registers only if release lands on the same button it pressed.
    if (idx >= 0 && idx == pressed) {
        switch (idx) {
            case 0: go_back();    return;
            case 1: go_forward(); return;
            case 2: if (is_loading) stop_load(); else load_current_url(); return;  // Reload/Stop (#25)
            case 3: go_home();    return;
            case 4: load_current_url(); return;  // Go
            case 5: g_settings_open = 1; redraw(); return;   // open the overflow menu
        }
    }

    // Bookmark star, inside the address bar.
    if (pressed < 0) {
        int sx, sy, sw, sh;
        url_star_rect(&sx, &sy, &sw, &sh);
        if (point_in(mx, my, sx, sy, sw, sh)) {
            bookmark_toggle_current();
            draw_toolbar();
            win_invalidate(window_handle);
            return;
        }
    }

    // Tab strip: new tab, close a tab, or switch to one.
    if (pressed < 0 && my < TABSTRIP_H) {
        int nx, ny, nw, nh;
        newtab_rect(&nx, &ny, &nw, &nh);
        if (point_in(mx, my, nx, ny, nw, nh)) { tab_new(); return; }
        for (int i = 0; i < g_tab_count; i++) {
            int xx, xy, xw, xh;
            tab_close_rect(i, &xx, &xy, &xw, &xh);
            if (point_in(mx, my, xx, xy, xw, xh)) {
                tab_close(i);
                redraw();   // tab_close() only navigates (and so only redraws) when the ACTIVE tab was the one closed
                return;
            }
            int tx, ty, tw, th;
            tab_rect(i, &tx, &ty, &tw, &th);
            if (point_in(mx, my, tx, ty, tw, th)) { tab_switch(i); return; }
        }
    }

    // Bookmarks bar.
    if (pressed < 0 && my >= BOOKMARKS_Y && my < BOOKMARKS_Y + BOOKMARKS_H) {
        for (int i = 0; i < g_bookmark_count; i++) {
            int bx, by, bw, bh;
            if (!bookmark_rect(i, &bx, &by, &bw, &bh)) break;
            if (point_in(mx, my, bx, by, bw, bh)) { bookmark_navigate(i); return; }
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
    // Was `my < TOOLBAR_H` (the toolbar was the only chrome band above the
    // content). With the tab strip and bookmarks bar added, "clicked chrome,
    // missed every control" now means anywhere above CONTENT_Y.
    if (!point_in(mx, my, URL_BAR_X, URL_BAR_Y, URL_BAR_W, URL_BAR_H) &&
        my < CONTENT_Y && idx < 0) {
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

    // #25: SYS_WIN_CREATE takes OUTER (chrome-included) size, not the
    // content size every layout constant in this file (CONTENT_W, STATUS_H,
    // GO_BTN_X, ...) assumes it is drawing into. Requesting exactly
    // WIN_WIDTH x WIN_HEIGHT silently clipped the bottom 24px - the whole
    // status bar, so the new load-progress bar/text was being drawn
    // almost entirely off the composited window. +4/+24 is the established
    // idiom (see userland/apps/terminal/main.c); only the CREATE size needs
    // it, draw calls stay content-relative.
    window_handle = win_create("Browser", 100, 100, WIN_WIDTH + 4, WIN_HEIGHT + 24);
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
    { FILE *df = fopen("/BRDUMP.TXT", "r"); if (df) { fclose(df); g_brdump = 1; } }

    // #245 engflexclose: RUN the cssvar self-test, in the OS, on every launch.
    // Placed after the /BRDUMP.TXT read so the marker can turn the per-case
    // detail on, and before navigate() so the result is on serial before any
    // page can crash the app.
    // engwalkstack (#245): name the sources this binary was built from, before
    // anything else can go wrong, so every serial log is self-describing.
    br_buildstamp();

    g_cv_detail = g_brdump;
    { int cvf = cssvar_selftest(cssvar_report);
      printf("[CSSVAR] selftest: %d cases, %d failing, %s\n",
             g_cv_cases, cvf, cvf ? "FAIL" : "PASS"); }

    // [no-ticket] (browser-glass): seed tab 0 with whatever url_buffer ended
    // up as above (home page, or a STARTURL.TXT/TEST.HTML override).
    str_cpy(g_tabs[0].url, url_buffer);
    g_tabs[0].hist_count = 0;
    g_tabs[0].hist_pos = -1;
    g_tabs[0].scroll_offset = 0;
    g_tabs[0].chip_col = TAB_CHIP_COLORS[0];

    navigate(1);

    gui_event_t event;

    while (running) {
        poll_fetch();
        poll_images();
        int event_type = win_get_event(window_handle, &event, 50);
        // #548: event_type == 0 means the 50ms poll timed out with no event,
        // and sys_win_get_event() leaves event_buf UNTOUCHED on a timeout (see
        // kernel/proc/syscall.c sys_win_get_event). The old `< 0` check only
        // skipped the error case, so every idle timeout fell straight into the
        // switch below on the STALE `event` left over from the last real
        // event - e.g. a single scroll notch kept re-adding scroll_offset
        // every 50ms forever with no further input, and any other event type
        // would similarly replay its handler on every idle tick. Skip both
        // "error" (<0) and "no event" (==0); only a genuine event (>0) should
        // reach the switch.
        if (event_type <= 0) continue;

        switch (event.type) {
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_REDRAW:
                g_chrome_dirty = 1;   // (browserglass) a compositor-requested full frame re-blits the backdrop
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
                // OS-wide wheel convention (userland/libc/gui_scroll.h,
                // matching kernel gui/terminal.c): a POSITIVE scroll_delta
                // scrolls UP, i.e. TOWARD THE CONTENT START, which means it
                // must DECREASE scroll_offset (0 == top of the page). This
                // used to be `+=`, which scrolled every wheel notch the
                // wrong way relative to every other app on the system - the
                // exact bug gui_scroll.h documents Files having had before it
                // adopted the shared gui_scroll_wheel() primitive. The browser
                // still hand-rolls its own scroll_offset (it predates that
                // primitive and shares scroll_max()/the scrollbar geometry
                // below, so folding it into a gui_scroll_t is a separate,
                // larger change), but the DIRECTION is not a matter of local
                // taste - fix the sign, not the convention. Same one notch
                // (~1.5 text lines at the default 14px UI font) as before;
                // gui_scroll_wheel()'s own 'three rows per notch' feel is
                // list-row-sized (16px rows -> 48px), which is not directly
                // comparable to this pixel-scrolled page viewport.
                scroll_offset -= event.scroll_delta * 24; // pixels per notch
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
                    if (new_w != g_win_w || new_h != g_win_h) g_chrome_dirty = 1;   // (browserglass) the backdrop is scaled to the content rect
                    g_win_w = new_w;
                    g_win_h = new_h;
                    if (g_have_layout && g_content_len > 0 && new_w != last_layout_w)
                        relayout_page();   // #resizereflow: layout-only, no fetch/parse/JS
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
