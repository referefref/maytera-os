// term_prefs.c
// PHASE 0 (terminal uplift): this file is one of ten produced by a PURE,
// BEHAVIOUR-PRESERVING split of what used to be a single 3407-line main.c.
// Every line of logic below was MOVED, not rewritten. The only edits made
// during the split were: `static` removed from symbols another module needs,
// forward declarations replaced by these headers, and includes added.

#include "term_common.h"
#include "term_util.h"
#include "term_grid.h"
#include "term_render.h"
#include "term_scrollback.h"
#include "term_theme.h"
#include "term_prefs.h"
#include "term_profile.h"
#include "../../libc/textfield.h"   // shared caret/selection/clipboard state machine

// Content hash of the last-loaded TERMPREF.CFG, for term_prefs_poll_reload()
// (#241, same "poll a flat file on a throttle, no filesystem watcher exists"
// idiom as gui_theme_poll_reload() and the compositor's SETTINGS.CFG poll).
static unsigned int g_term_pref_hash = 0;
static int  g_term_pref_have_baseline = 0;
static uint32_t g_term_pref_last_poll_ms = 0;
// Persist the current theme/font selection. Same discarded-write-is-silent
// fix #743 applied to EDFONT.CFG: userconf_finish_write()'s result is
// checked, not thrown away (a cosmetic preference that silently fails to
// save is still a bug users report as "it didn't save").
// READ, at last. This flag was flagged in docs/TERMINAL_MODULES.md as debt:
// "written and never read". It is now what the preferences dialog checks to
// decide whether OK actually succeeded - see the save_err handling in
// term_prefs_dialog(). A cosmetic preference that silently fails to save is
// still a bug users report as "it didn't save" (#743), and MEASURED on golden
// 2049 it fails for real: a session whose user has no /CONFIG/PASSWD entry gets
// home="/" from userhome_root()'s documented fallback, and /CONFIG is 0711
// root-owned in PERMS.DB, so every per-user preference write is denied. The
// dialog used to close on OK as though it had worked.
int g_term_pref_save_failed = 0;
void term_prefs_save(void) {
    // Tier 2 (docs/TERMINAL_PARITY.md) added a 5th "|"-delimited field, the
    // COLOUR SCHEME slug, after font_size. term_prefs_load()/
    // term_prefs_poll_reload() below tolerate its absence (an old 4-field
    // file with no 4th "|" still parses; the slug then stays at its
    // compiled-in "system" default), so this write never breaks a config
    // written by a build that predates colour schemes.
    // Profiles added fields 6-8 (cursor shape, blink, scrollback depth) after
    // the colour-scheme slug, by the same append-only rule: the reader below
    // requires only the first four, so a file written by ANY earlier build
    // still parses and a file written here still parses in an earlier build.
    char buf[256];
    snprintf(buf, sizeof(buf), "%s|%s|%s|%d|%s|%d|%d|%d\n",
             g_term_theme_slug,
             g_term_font.family[0] ? g_term_font.family : TERM_DEFAULT_FONT_FAMILY,
             g_term_font.style[0] ? g_term_font.style : TERM_DEFAULT_FONT_STYLE,
             g_term_font.size,
             g_term_palette_slug,
             g_term_cursor_shape,
             g_term_cursor_blink ? 1 : 0,
             sb_want);
    int fd = userconf_open_write("TERMPREF.CFG");
    if (userconf_finish_write(fd, buf, strlen(buf)) != 0) g_term_pref_save_failed = 1;
    else g_term_pref_save_failed = 0;
    // Refresh the poll baseline to OUR OWN write, so term_prefs_poll_reload()
    // does not immediately "reload" the value we just applied.
    unsigned int h = 0;
    for (int i = 0; buf[i]; i++) h = h * 131 + (unsigned char)buf[i];
    g_term_pref_hash = h;
    g_term_pref_have_baseline = 1;
}

// Load (or, on a virgin image, CREATE) /CONFIG/TERMPREF.CFG. This is the
// mechanism that makes "default as dark mode" true on first boot rather
// than merely true of the compiled-in constant: if the file does not exist
// yet, the compiled defaults above are written out immediately, so the very
// first terminal window on a fresh image is dark AND the choice is already
// durable (a second boot with no user action still reads the same file,
// rather than re-deriving "no file -> defaults" every time, which would
// silently stop being true the moment something else created the file with
// different bytes).
// ---------------------------------------------------------------------------
// ONE parser for TERMPREF.CFG, used by BOTH the initial load and the ~1/s
// live-reload poll. Those two had verbatim copies of a hand-rolled four-
// pointer scan; extending it to eight fields by copying it a third time is how
// two readers of the same file come to disagree about it. Fields are
// positional and OPTIONAL after the fourth, so every historical version of
// this file (4, 5 and now 8 fields) parses here:
//
//   0 theme slug | 1 font family | 2 font style | 3 size
//   | 4 colour scheme slug | 5 cursor shape | 6 blink | 7 scrollback lines
// ---------------------------------------------------------------------------
#define TP_MIRROR_FIELDS 8
static int tp_fields(char *buf, char **f, int maxf) {
    int n = 0;
    f[n++] = buf;
    for (char *q = buf; *q; q++) {
        if (*q == '\n' || *q == '\r') { *q = 0; break; }
        if (*q == '|' && n < maxf) { *q = 0; f[n++] = q + 1; }
    }
    return n;
}

// Apply a parsed mirror line to live state. Returns 0 if the line was too
// short to be a valid record (fewer than the four fields the original format
// always had), in which case NOTHING was written and the caller keeps the
// defaults it had already set.
static int tp_apply_fields(char **f, int n) {
    if (n < 4) return 0;
    strncpy(g_term_theme_slug, f[0], GUI_THEME_SLUG_MAX - 1);
    g_term_theme_slug[GUI_THEME_SLUG_MAX - 1] = 0;
    strncpy(g_term_font.family, f[1], GUI_FONT_NAME_MAX - 1);
    g_term_font.family[GUI_FONT_NAME_MAX - 1] = 0;
    strncpy(g_term_font.style, f[2], GUI_FONT_STYLE_MAX - 1);
    g_term_font.style[GUI_FONT_STYLE_MAX - 1] = 0;
    int sz = atoi(f[3]);
    if (sz >= 8 && sz <= 32) g_term_font.size = sz;
    if (n >= 5) {
        strncpy(g_term_palette_slug, f[4], GUI_PALETTE_SLUG_MAX - 1);
        g_term_palette_slug[GUI_PALETTE_SLUG_MAX - 1] = 0;
    } else {
        strncpy(g_term_palette_slug, GUI_PALETTE_SYSTEM_SLUG, GUI_PALETTE_SLUG_MAX - 1);
        g_term_palette_slug[GUI_PALETTE_SLUG_MAX - 1] = 0;
    }
    if (n >= 6) {
        int cs = atoi(f[5]);
        if (cs >= 0 && cs < TERM_CURSOR_SHAPE_COUNT) g_term_cursor_shape = cs;
    }
    if (n >= 7) g_term_cursor_blink = atoi(f[6]) ? 1 : 0;
    if (n >= 8) {
        int sb = atoi(f[7]);
        if (sb >= 200 && sb <= 20000) term_scrollback_set_capacity(sb);
    }
    return 1;
}

void term_prefs_load(void) {
    strncpy(g_term_theme_slug, TERM_DEFAULT_THEME_SLUG, GUI_THEME_SLUG_MAX - 1);
    g_term_theme_slug[GUI_THEME_SLUG_MAX - 1] = 0;
    strncpy(g_term_palette_slug, GUI_PALETTE_SYSTEM_SLUG, GUI_PALETTE_SLUG_MAX - 1);
    g_term_palette_slug[GUI_PALETTE_SLUG_MAX - 1] = 0;
    memset(&g_term_font, 0, sizeof(g_term_font));
    strncpy(g_term_font.family, TERM_DEFAULT_FONT_FAMILY, GUI_FONT_NAME_MAX - 1);
    strncpy(g_term_font.style, TERM_DEFAULT_FONT_STYLE, GUI_FONT_STYLE_MAX - 1);
    g_term_font.size = TERM_DEFAULT_FONT_SIZE;

    int fd = userconf_open_read("TERMPREF.CFG", TERM_PREF_CFG);
    int existed = (fd >= 0);
    if (fd >= 0) {
        char buf[256];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            char *f[TP_MIRROR_FIELDS];
            tp_apply_fields(f, tp_fields(buf, f, TP_MIRROR_FIELDS));
        }
    }

    // PROFILES ARE THE SOURCE OF TRUTH, and this is where that becomes true
    // rather than merely documented. The mirror was read first only so that a
    // virgin image with no TERMPROF.CFG seeds its "Default" profile from
    // whatever settings the previous build had saved, instead of resetting
    // them; term_profiles_load() then either finds a real profile table or
    // creates one from exactly that state.
    term_profiles_load();
    // A terminal STARTS from the DEFAULT profile. Reading the mirror instead
    // would make "Set default" change nothing on the next launch, which is the
    // dead-control failure (#208) this whole surface is written against.
    if (g_term_profile_count > 0) {
        if (g_term_profile_default < 0 || g_term_profile_default >= g_term_profile_count)
            g_term_profile_default = 0;
        g_term_profile_active = g_term_profile_default;
        term_profile_apply(&g_term_profiles[g_term_profile_active]);
    }
    term_resolve_theme();
    term_resolve_palette();
    gui_font_resolve(g_term_font.family, g_term_font.style,
                     &g_term_font.face, &g_term_font.style_bits);
    term_apply_font();
    if (!existed) term_prefs_save();   // virgin image: make the dark default durable
}

// Throttled live-reload (#241), same idiom as gui_theme_poll_reload(): a
// currently-open terminal window picks up a change made via `terminal
// --contract set ...` (a SEPARATE process; see the contract below) or a
// hand-edit of TERMPREF.CFG without needing to be relaunched. Content-hash
// compared, not mtime (same reasoning gui_theme_poll_reload() documents:
// this ext2 writer is not relied on to move mtime). Returns 1 if the grid
// was reflowed (caller must also tell the pty/redraw), 0 otherwise.
int term_prefs_poll_reload(void) {
    uint32_t now = (uint32_t)uptime_ms();
    if (g_term_pref_have_baseline && (now - g_term_pref_last_poll_ms) < 1000) return 0;
    g_term_pref_last_poll_ms = now;

    int fd = userconf_open_read("TERMPREF.CFG", TERM_PREF_CFG);
    if (fd < 0) return 0;
    char buf[256];
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = 0;
    unsigned int h = 0;
    for (int i = 0; buf[i]; i++) h = h * 131 + (unsigned char)buf[i];
    if (g_term_pref_have_baseline && h == g_term_pref_hash) return 0;
    g_term_pref_hash = h;
    g_term_pref_have_baseline = 1;

    // Same ONE parser the initial load uses; see tp_fields()/tp_apply_fields().
    {
        char *f[TP_MIRROR_FIELDS];
        if (!tp_apply_fields(f, tp_fields(buf, f, TP_MIRROR_FIELDS))) return 0;
    }
    term_resolve_theme();
    term_resolve_palette();
    gui_font_resolve(g_term_font.family, g_term_font.style,
                     &g_term_font.face, &g_term_font.style_bits);
    term_apply_font();
    if (window_handle >= 0) term_handle_resize(term_px_w, term_px_h);
    return 1;
}
// ---- Terminal Profiles dialog ---------------------------------------------
// GEOMETRY IS A TRANSCRIPTION, NOT A GUESS. The layout was designed first as
// HTML/CSS at 1:1 (docs/terminal-profiles-mockup.html, which also carries the
// token table and the rationale); every constant below is a cell of that
// document's geometry table. That is the standing project rule for UI work,
// and it is why this dialog has a two-column master/detail shape instead of
// the stack of lists that grew here one ticket at a time.
//
// THE ONE IDEA THE LAYOUT CARRIES. Two lists sit one above the other, and each
// has its own live preview tile beside it. The upper preview is a miniature
// TERMINAL painted only from the selected COLOUR SCHEME. The lower preview is a
// miniature WINDOW painted only from the selected WINDOW THEME, and contains no
// cell text at all. A user who changes the wrong one sees which surface they
// just repainted without reading a label. The product conflated these two
// concepts once and the owner corrected it; this is that correction expressed
// as pixels rather than as a pair of captions.
//
// EVERY CONTROL HERE DOES SOMETHING (#208). Nothing is drawn that cannot be
// honoured today. In particular there is deliberately NO transparency control:
// see the note at the end of this file for the measurement behind that.
#define PD_W            760
#define PD_H            548
#define PD_CHROME_W       4
#define PD_CHROME_H      24
#define PD_ROW_H         22

#define PD_L_X           12
#define PD_L_W          196
#define PD_LIST_Y        30
#define PD_LIST_H       362
#define PD_BROW1_Y      402
#define PD_BROW2_Y      434
#define PD_BW            94
#define PD_BH            26
#define PD_B2_X         114

#define PD_R_X          220
#define PD_R_W          528
#define PD_NAME_Y        26
#define PD_NAME_W       400
#define PD_FLD_H         24
#define PD_BADGE_X      628
#define PD_BADGE_W      120

#define PD_SEP1_Y        58
#define PD_SCHEME_LBL_Y  66
#define PD_SCHEME_Y      82
#define PD_SUBLIST_W    200
#define PD_SCHEME_H     126
#define PD_PREV_X       428
#define PD_PREV_W       320

#define PD_THEME_LBL_Y  216
#define PD_THEME_Y      232
#define PD_THEME_H       84

#define PD_SEP2_Y       324
#define PD_FONT_Y       336
#define PD_FONT_BTN_X   648
#define PD_FONT_BTN_W   100
#define PD_SBH           24
#define PD_CTL_X        300

#define PD_CUR_Y        366
#define PD_CUR_BLOCK_W   74
#define PD_CUR_UND_X    376
#define PD_CUR_UND_W     88
#define PD_CUR_BAR_X    466
#define PD_CUR_BAR_W     60
#define PD_CHK_X        556
#define PD_CHK_SZ        14

#define PD_SB_Y         396
#define PD_SB_TRACK_W   260

#define PD_DIR_Y        426
#define PD_FLD2_X       340
#define PD_FLD2_W       408
#define PD_CMD_Y        456

#define PD_FOOT_SEP_Y   504
#define PD_BTN_Y        512
#define PD_BTNW          88
#define PD_OK_X         660
#define PD_CANCEL_X     564

// KEYBOARD REACH, stated so the gaps are known rather than discovered: Tab
// cycles the eight stops below, Up/Down drives a focused list, Left/Right and
// Space drive the cursor row and the scrollback slider, Insert adds a profile,
// Enter is OK and Esc is Cancel. STILL MOUSE-ONLY: Duplicate, Delete, Set
// default and Choose... Delete is mouse-only on purpose (destructive, no
// confirm step); the other three are a real gap, not a decision.
//
// Focus targets for Tab / arrow keys. There is no shared "next control"
// plumbing in this toolkit, so the dialog owns one explicit ring; a mouse
// click on any control sets the same variable, so keyboard and mouse never
// disagree about what Up/Down or a typed character applies to.
enum { PDF_PROFILES = 0, PDF_NAME, PDF_SCHEME, PDF_THEME,
       PDF_CURSOR, PDF_SB, PDF_DIR, PDF_CMD, PDF_COUNT };

// Caret slot for a focused text field. A lookup, not `focus - PDF_NAME`: the
// ring above is ordered to match the dialog top-to-bottom, so the three fields
// are not adjacent, and arithmetic on the enum would index pd_caret[] out of
// range the moment anyone reorders it again.
static int pd_caret_slot(int focus) {
    if (focus == PDF_NAME) return 0;
    if (focus == PDF_DIR)  return 1;
    return 2;                       // PDF_CMD
}

// One reused transient text-field wrapper (the pattern textfield.h documents
// for app-owned buffers): the buffers ARE the profile's own fields, so a
// keystroke edits the profile directly with no third copy to keep in sync.
// Caret positions live here because tf_attach() is rebuilt per event.
static textfield_t pd_tf;
static int pd_caret[3];   // name, dir, cmd

static const char *pd_profile_label(void *ctx, int index, char *buf, int cap) {
    (void)ctx;
    str_copy(buf, g_term_profiles[index].name, cap);
    return buf;
}
static const char *pd_theme_label(void *ctx, int index, char *buf, int cap) {
    gui_theme_entry_t *ents = (gui_theme_entry_t *)ctx;
    str_copy(buf, ents[index].name, cap);
    return buf;
}
static const char *pd_palette_label(void *ctx, int index, char *buf, int cap) {
    gui_palette_entry_t *ents = (gui_palette_entry_t *)ctx;
    str_copy(buf, ents[index].name, cap);
    return buf;
}

static int pd_hit(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && mx < x + w && my >= y && my < y + h;
}

// A text field drawn in the SAME antialiased TrueType face as every label in
// this dialog. textfield.h's own one-call renderer is fixed-width-font only,
// and its header says apps with their own layout should draw the selection and
// caret themselves; this is that. The state machine (insert/caret/selection/
// clipboard) is still the shared one, only the pixels are local.
static void pd_field(int win, int x, int y, int w, int h,
                     const char *text, int caret, int focused) {
    uint32_t bg = theme_color(THEME_COLOR_TEXTBOX_BG);
    uint32_t bd = focused ? theme_color(THEME_COLOR_FOCUS_RING)
                          : theme_color(THEME_COLOR_TEXTBOX_BORDER);
    uint32_t tx = theme_color(THEME_COLOR_TEXTBOX_TEXT);
    win_draw_rect(win, x, y, w, h, bg);
    gui_draw_rect(win, x, y, w, h, bd);

    int len = 0; while (text[len]) len++;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;

    // HORIZONTAL SCROLL. There is no clipping in this drawing stack: text
    // handed to win_draw_text_ttf() is painted wherever it lands, so a
    // starting directory longer than the box would spill straight across the
    // dialog behind it. The field therefore shows a WINDOW of the string,
    // chosen so the caret is always inside it.
    //
    // Widths are summed PER CHARACTER walking backwards from the caret rather
    // than by re-measuring longer and longer substrings, which would be
    // quadratic in a function that runs on every one of this dialog's ~5
    // repaints a second. gui_ttf_render_width() is documented as matching
    // win_draw_text_ttf() exactly (no kerning), so per-character widths sum to
    // the same answer the renderer will produce.
    int avail = w - 10;
    int start = 0;
    {
        char one[2] = { 0, 0 };
        int total = gui_ttf_render_width(text, 13);
        if (total > avail) {
            int acc = 0;
            start = caret;
            while (start > 0) {
                one[0] = text[start - 1];
                int cwid = gui_ttf_render_width(one, 13);
                if (acc + cwid > avail) break;
                acc += cwid;
                start--;
            }
            // Room left over after reaching the caret: extend forward, so a
            // caret at the end of a long path still shows the tail rather than
            // only the characters before it.
            int fwd = caret;
            while (fwd < len) {
                one[0] = text[fwd];
                int cwid = gui_ttf_render_width(one, 13);
                if (acc + cwid > avail) break;
                acc += cwid;
                fwd++;
            }
        }
    }

    char view[TERM_PROFILE_CMD_MAX];
    int n = len - start;
    if (n > (int)sizeof(view) - 1) n = (int)sizeof(view) - 1;
    for (int i = 0; i < n; i++) view[i] = text[start + i];
    view[n] = 0;
    win_draw_text_ttf(win, x + 5, y + (h - 13) / 2, view, 13, tx);

    if (focused) {
        char head[TERM_PROFILE_CMD_MAX];
        int hn = caret - start;
        if (hn < 0) hn = 0;
        if (hn > (int)sizeof(head) - 1) hn = (int)sizeof(head) - 1;
        for (int i = 0; i < hn; i++) head[i] = text[start + i];
        head[hn] = 0;
        int cx = x + 5 + gui_ttf_render_width(head, 13);
        if (cx > x + w - 3) cx = x + w - 3;
        win_draw_rect(win, cx, y + 4, 1, h - 8, tx);
    }
}

// Advance width of `s` at `size` IN A GIVEN FACE.
//
// FOUND BY RUNNING IT, not by reading it (golden 2049, screendump v11): the
// preview laid its sample `ls` output out with gui_ttf_render_width() while
// DRAWING it with win_draw_text_ttf_ex(g_term_font.face, ...).
// gui_ttf_width() has no face parameter - it measures in the DEFAULT UI face -
// so the two were measuring different fonts and the words overlapped each
// other on screen. Anything laid out for win_draw_text_ttf_ex() must be
// measured per glyph in the SAME face, which is what font_glyph()'s advance
// return is for (term_apply_font() already derives the cell width from it).
static int pd_face_width(const char *s, int face, int size, int style) {
    font_glyph_meta_t meta;
    int w = 0;
    for (int i = 0; s[i]; i++) {
        int adv = font_glyph(face, size, style, (unsigned char)s[i], &meta, 0, 0);
        w += (adv > 0) ? adv : size * 6 / 10;
    }
    return w;
}

// The CELL GRID preview: a miniature terminal, painted from the colour scheme
// and from nothing else. "Follow system theme" resolves its default fg/bg/
// cursor through the SELECTED WINDOW THEME, exactly as term_bg_color()/
// term_fg_color()/term_cursor_color() do at runtime, so the preview cannot
// promise something the real grid will not do.
static void pd_draw_scheme_preview(int win, const char *slug, int theme_index,
                                   int face, int style_bits) {
    // Cache the loaded scheme. gui_palette_load() OPENS AND READS
    // /PALETTES/<slug>.tpalette, and this dialog repaints every 200ms whether
    // anything happened or not, so calling it straight would be five file
    // reads a second for as long as the window is open. The cache key is the
    // slug, which is exactly what decides the contents.
    static term_palette_t pv;
    static char pv_slug[GUI_PALETTE_SLUG_MAX] = { 1, 0 };   // 1 = "no slug can match"
    static int  sysmode = 1;
    if (!str_eq(pv_slug, slug)) {
        sysmode = (gui_palette_load(slug, &pv) != 0);
        str_copy(pv_slug, slug, GUI_PALETTE_SLUG_MAX);
    }
    uint32_t bg  = sysmode ? theme_color_of(theme_index, THEME_COLOR_TEXTBOX_BG)     : pv.bg;
    uint32_t fg  = sysmode ? theme_color_of(theme_index, THEME_COLOR_TEXTBOX_TEXT)   : pv.fg;
    uint32_t cur = sysmode ? theme_color_of(theme_index, THEME_COLOR_TEXTBOX_CURSOR) : pv.cursor;

    int x = PD_PREV_X, y = PD_SCHEME_Y, w = PD_PREV_W, h = PD_SCHEME_H;
    win_draw_rect(win, x, y, w, h, bg);
    gui_draw_rect(win, x, y, w, h, theme_color(THEME_COLOR_TEXTBOX_BORDER));

    win_draw_text_ttf_ex(win, x + 8, y + 8, "user@maytera:~$ ls", face, 12, style_bits, fg);
    // Four words in four ANSI slots: the colours `ls` itself would use.
    int wx = x + 8;
    const char *words[4] = { "bin", "docs", "run.sh", "err.log" };
    int slots[4] = { 4, 6, 2, 1 };
    for (int i = 0; i < 4; i++) {
        win_draw_text_ttf_ex(win, wx, y + 24, words[i], face, 12, style_bits, pv.ansi[slots[i]]);
        wx += pd_face_width(words[i], face, 12, style_bits) + 8;
    }
    win_draw_text_ttf_ex(win, x + 8, y + 40, "user@maytera:~$", face, 12, style_bits, fg);
    win_draw_rect(win, x + 8 + pd_face_width("user@maytera:~$ ", face, 12, style_bits),
                  y + 39, 7, 13, cur);

    // The 16 ANSI slots, in SGR order, so a scheme is judged by what it will
    // actually paint rather than by its name.
    for (int i = 0; i < 16; i++)
        win_draw_rect(win, x + 8 + i * 18, y + 62, 18, 14, pv.ansi[i]);
    uint32_t ink = gui_ensure_contrast(gui_ink_on(bg), bg, GUI_FLOOR_TEXT);
    win_draw_text_ttf(win, x + 8, y + 80, "16 ANSI colours", 11, ink);

    // fg / bg / cursor chips: the three colours no single cell carries.
    win_draw_rect(win, x + 8,   y + 96, 22, 14, fg);
    win_draw_text_ttf(win, x + 34, y + 98, "fg", 11, ink);
    win_draw_rect(win, x + 56,  y + 96, 22, 14, bg);
    gui_draw_rect(win, x + 56,  y + 96, 22, 14, ink);
    win_draw_text_ttf(win, x + 82, y + 98, "bg", 11, ink);
    win_draw_rect(win, x + 104, y + 96, 22, 14, cur);
    win_draw_text_ttf(win, x + 130, y + 98, "cursor", 11, ink);
}

// The IN-WINDOW CHROME preview.
//
// CORRECTED AFTER RUNNING IT (golden 2049, screendumps v18/v19/v21). This tile
// used to call gui_theme_win_preview(), which draws a faithful crop of what the
// KERNEL DECORATOR paints: title bar, min/max/close buttons, frame. That is a
// truthful preview of a SYSTEM theme, and it was the wrong thing to show here,
// because the terminal's own Window Theme DOES NOT PAINT THE OS TITLE BAR.
//
// MEASURED, not assumed: setting this to Light Mode and pressing OK left the
// terminal's title bar exactly as it was. `win->theme_override` in
// kernel/gui/window.c is per-window and would do it, but it takes only
// system/dark/light and NO SYSCALL EXPOSES IT - `grep theme_override
// kernel/proc/syscall.c userland/libc/syscall.h` returns nothing. An app cannot
// ask the decorator to draw its frame in an arbitrary theme.
//
// docs/TERMINAL_MODULES.md says g_term_theme_slug "governs the CHROME", which
// is how the old preview came to promise a title bar. What it actually governs,
// via theme_color_of(g_term_theme_index, ...), is the chrome THE TERMINAL DRAWS
// ITSELF: the tab strip and pane headers (term_layout.c's tl_c(), tokens TAB_BG
// / TAB_ACTIVE / TAB_BORDER / MENU_TEXT / TITLEBAR_ACTIVE / TITLEBAR_TEXT) and
// the selection colours (term_select.c). So that is what this tile draws, in
// exactly those tokens. A preview must promise only what the control delivers;
// the previous one promised the one surface it cannot touch.
static void pd_draw_theme_preview(int win, int ti) {
    int x = PD_PREV_X, y = PD_THEME_Y, w = PD_PREV_W, h = PD_THEME_H;
    uint32_t wbg = theme_color_of(ti, THEME_COLOR_WINDOW_BG);
    win_draw_rect(win, x, y, w, h, wbg);

    // --- tab strip, the same tokens term_layout.c's tl_draw_strip() uses ----
    int sx = x + 6, sy = y + 5, sw = w - 12, sh = 18;
    win_draw_rect(win, sx, sy, sw, sh, theme_color_of(ti, THEME_COLOR_TAB_BG));
    win_draw_rect(win, sx, sy + sh - 1, sw, 1, theme_color_of(ti, THEME_COLOR_TAB_BORDER));
    win_draw_rect(win, sx + 2, sy + 3, 14, 12, theme_color_of(ti, THEME_COLOR_BUTTON_FACE));
    gui_text_ttf_centered(win, sx + 2, sy + 2, 14, 14, "+",
                          theme_color_of(ti, THEME_COLOR_BUTTON_TEXT), 10);
    win_draw_rect(win, sx + 20, sy + 1, 84, sh - 2, theme_color_of(ti, THEME_COLOR_TAB_ACTIVE));
    gui_draw_rect(win, sx + 20, sy + 1, 84, sh - 2, theme_color_of(ti, THEME_COLOR_TAB_BORDER));
    win_draw_text_ttf(win, sx + 26, sy + 5, "shell", 10, theme_color_of(ti, THEME_COLOR_MENU_TEXT));
    gui_draw_rect(win, sx + 106, sy + 1, 84, sh - 2, theme_color_of(ti, THEME_COLOR_TAB_BORDER));
    win_draw_text_ttf(win, sx + 112, sy + 5, "build", 10,
                      theme_color_of(ti, THEME_COLOR_MENU_TEXT_DISABLED));

    // --- a focused pane: header bar + accent frame + a selection sample -----
    int px = x + 6, py = y + 27, pw = w - 12, ph = h - 46;
    gui_draw_rect(win, px, py, pw, ph, theme_color_of(ti, THEME_COLOR_ACCENT));
    win_draw_rect(win, px + 1, py + 1, pw - 2, 14, theme_color_of(ti, THEME_COLOR_TITLEBAR_ACTIVE));
    win_draw_text_ttf(win, px + 5, py + 3, "shell", 10, theme_color_of(ti, THEME_COLOR_TITLEBAR_TEXT));
    win_draw_rect(win, px + 6, py + 19, 96, 13, theme_color_of(ti, THEME_COLOR_SELECTION));
    win_draw_text_ttf(win, px + 9, py + 20, "selected text", 10,
                      theme_color_of(ti, THEME_COLOR_SELECTION_TEXT));

    // Say what it paints, INSIDE the tile, so the promise and the picture are
    // the same object and cannot drift apart the way the old caption did.
    uint32_t cap = gui_ensure_contrast(theme_color_of(ti, THEME_COLOR_LABEL_TEXT), wbg, GUI_FLOOR_TEXT);
    win_draw_text_ttf(win, x + 6, y + h - 15, "tabs, pane headers, selection", 10, cap);

    gui_draw_rect(win, x, y, w, h, theme_color(THEME_COLOR_TEXTBOX_BORDER));
}

// Nearest scrollback stop for a value, so a file hand-edited to 3000 lands on
// a real slider position instead of leaving the knob somewhere it can never be
// dragged back to.
// Pointer x -> stop index. One definition: the press, the drag and the initial
// click all round the same way, so the knob cannot land somewhere the value
// readout disagrees with.
static int pd_sb_at(int mx) {
    int rel = mx - PD_CTL_X;
    if (rel < 0) rel = 0;
    if (rel > PD_SB_TRACK_W) rel = PD_SB_TRACK_W;
    return (rel * (TERM_SB_STOPS - 1) * 2 + PD_SB_TRACK_W) / (2 * PD_SB_TRACK_W);
}

static int pd_sb_index(int lines) {
    int best = 0, bestd = -1;
    for (int i = 0; i < TERM_SB_STOPS; i++) {
        int d = lines - term_sb_stops[i];
        if (d < 0) d = -d;
        if (bestd < 0 || d < bestd) { bestd = d; best = i; }
    }
    return best;
}

// Blocks until closed (OK/Cancel/window-close), same contract as
// gui_font_dialog(). Returns 1 if anything was applied + persisted. F9 opens
// it; it is only ever reached between commands, so there is no "stole a
// keystroke from vi" risk.
int term_prefs_dialog(void) {
    // STATIC, not stack. These two arrays are 2.7 KB and 2.5 KB, and
    // PROCESS_STACK_SIZE is 16 KB (kernel/exec/elf.c) - and this function calls
    // gui_font_dialog(), which builds its own family/style/size lists on top of
    // this frame. Better than 5 KB of a 16 KB stack spent on two tables that
    // are read-only for the life of the call. The dialog is modal and
    // non-reentrant (it is only ever reached from the event loop, between
    // commands), so file scope is safe here in a way it would not be for a
    // widget that can nest inside itself.
    static gui_theme_entry_t ents[GUI_THEME_MAX_ENTRIES];
    int tn = gui_theme_list(ents, GUI_THEME_MAX_ENTRIES);
    if (tn <= 0) return 0;

    // Colour schemes: index 0 is the synthetic "Follow system theme" slug
    // (gui_palette.h; not a file), then every /PALETTES/*.tpalette found. An
    // image built without /PALETTES still leaves entry 0 usable, so this list
    // is never empty and never unusable.
    static gui_palette_entry_t pents[GUI_PALETTE_MAX_ENTRIES + 1];
    str_copy(pents[0].slug, GUI_PALETTE_SYSTEM_SLUG, GUI_PALETTE_SLUG_MAX);
    str_copy(pents[0].name, "Follow system theme", GUI_PALETTE_NAME_MAX);
    pents[0].index = 0;
    int pn = gui_palette_list(pents + 1, GUI_PALETTE_MAX_ENTRIES) + 1;

    // The state to restore if the user cancels. The profile TABLE is restored
    // by re-reading TERMPROF.CFG (nothing is written to it before OK), so no
    // second copy of the table is kept in memory.
    term_profile_t entry_live;
    term_profile_defaults(&entry_live);
    term_profile_capture(&entry_live);
    int entry_active = g_term_profile_active;

    int sel = g_term_profile_active;
    if (sel < 0 || sel >= g_term_profile_count) sel = 0;

    int win_w = PD_W + PD_CHROME_W, win_h = PD_H + PD_CHROME_H;
    int wx = 120, wy = 60;
    {
        fb_info_t fi;
        if (fb_info(&fi) == 0 && (int)fi.width > win_w && (int)fi.height > win_h + 36) {
            wx = ((int)fi.width - win_w) / 2;
            wy = ((int)fi.height - 36 - win_h) / 2;
            if (wy < 6) wy = 6;
        }
    }
    int win = win_create("Terminal Profiles", wx, wy, win_w, win_h);
    if (win < 0) return 0;

    gui_list_t plist, slist, tlist;
    gui_list_config(&plist, PD_L_X, PD_LIST_Y, PD_L_W, PD_LIST_H, PD_ROW_H, g_term_profile_count);
    gui_list_config(&slist, PD_R_X, PD_SCHEME_Y, PD_SUBLIST_W, PD_SCHEME_H, PD_ROW_H, pn);
    gui_list_config(&tlist, PD_R_X, PD_THEME_Y, PD_SUBLIST_W, PD_THEME_H, PD_ROW_H, tn);

    int ssel = 0, tsel = 0, focus = PDF_PROFILES, changed = 0, running = 1;
    int sb_drag = 0, press_row = 0, save_err = 0;
    int cache_sel = -1, cache_face = 0, cache_style = 0;
    pd_caret[0] = pd_caret[1] = pd_caret[2] = 0;

    while (running) {
        term_profile_t *p = &g_term_profiles[sel];

        // Keep the two sub-lists' selections in step with the profile being
        // shown. Derived from the profile every frame rather than tracked
        // separately, so selecting another profile can never leave a list
        // pointing at the previous one's scheme.
        ssel = 0;
        for (int i = 0; i < pn; i++) if (str_eq(pents[i].slug, p->palette)) { ssel = i; break; }
        tsel = 0;
        for (int i = 0; i < tn; i++) if (str_eq(ents[i].slug, p->theme)) { tsel = i; break; }
        int ti = ents[tsel].index;

        if (cache_sel != sel) {
            gui_font_resolve(p->font_family, p->font_style, &cache_face, &cache_style);
            cache_sel = sel;
            pd_caret[0] = pd_caret[1] = pd_caret[2] = 0;
        }

        uint32_t bg   = theme_color(THEME_COLOR_WINDOW_BG);
        uint32_t lbl  = theme_color(THEME_COLOR_LABEL_TEXT);
        uint32_t line = theme_color(THEME_COLOR_TEXTBOX_BORDER);
        uint32_t hint = gui_ensure_contrast(theme_color(THEME_COLOR_MUTED), bg, GUI_FLOOR_TEXT);

        win_draw_rect(win, 0, 0, PD_W, PD_H, bg);

        // ---- left column: the profile table --------------------------------
        win_draw_text_ttf(win, PD_L_X, 10, "PROFILES", 11, lbl);
        gui_list_config(&plist, PD_L_X, PD_LIST_Y, PD_L_W, PD_LIST_H, PD_ROW_H, g_term_profile_count);
        gui_list_draw(win, &plist, sel, theme_color(THEME_COLOR_TEXTBOX_BG),
                      focus == PDF_PROFILES ? theme_color(THEME_COLOR_FOCUS_RING) : line,
                      theme_color(THEME_COLOR_TEXTBOX_TEXT),
                      theme_color(THEME_COLOR_SELECTION),
                      theme_color(THEME_COLOR_SELECTION_TEXT),
                      pd_profile_label, 0);
        // The default profile is marked in the list itself, not only in the
        // badge, so "which one will a new window use" is answerable without
        // clicking through every row.
        {
            int ry;
            if (gui_list_row_y(&plist, g_term_profile_default, &ry)) {
                uint32_t c = (g_term_profile_default == sel)
                             ? theme_color(THEME_COLOR_SELECTION_TEXT)
                             : gui_ensure_contrast(theme_color(THEME_COLOR_SUCCESS),
                                                   theme_color(THEME_COLOR_TEXTBOX_BG), GUI_FLOOR_TEXT);
                win_draw_text_ttf(win, PD_L_X + gui_list_row_w(&plist) - 26, ry + 4, "DEF", 10, c);
            }
        }
        int can_add = (g_term_profile_count < TERM_PROFILE_MAX);
        gui_button(win, PD_L_X,  PD_BROW1_Y, PD_BW, PD_BH, "New",       GUI_BTN_SECONDARY,
                   can_add ? GUI_ST_NORMAL : GUI_ST_DISABLED);
        gui_button(win, PD_B2_X, PD_BROW1_Y, PD_BW, PD_BH, "Duplicate", GUI_BTN_SECONDARY,
                   can_add ? GUI_ST_NORMAL : GUI_ST_DISABLED);
        gui_button(win, PD_L_X,  PD_BROW2_Y, PD_BW, PD_BH, "Delete",    GUI_BTN_SECONDARY,
                   g_term_profile_count > 1 ? GUI_ST_NORMAL : GUI_ST_DISABLED);
        gui_button(win, PD_B2_X, PD_BROW2_Y, PD_BW, PD_BH, "Set default", GUI_BTN_SECONDARY,
                   g_term_profile_default == sel ? GUI_ST_DISABLED : GUI_ST_NORMAL);
        win_draw_text_ttf(win, PD_L_X, PD_BROW2_Y + 34, "The default profile is what", 11, hint);
        win_draw_text_ttf(win, PD_L_X, PD_BROW2_Y + 48, "a NEW terminal opens with.", 11, hint);

        // ---- right column: the selected profile ----------------------------
        win_draw_text_ttf(win, PD_R_X, 10, "PROFILE NAME", 11, lbl);
        pd_field(win, PD_R_X, PD_NAME_Y, PD_NAME_W, PD_FLD_H, p->name, pd_caret[0], focus == PDF_NAME);
        if (g_term_profile_default == sel) {
            uint32_t ok = gui_ensure_contrast(theme_color(THEME_COLOR_SUCCESS), bg, GUI_FLOOR_TEXT);
            gui_draw_rect(win, PD_BADGE_X, PD_NAME_Y, PD_BADGE_W, PD_FLD_H, ok);
            // (colour, size) - that argument order is NOT interchangeable:
            // win_draw_text_ttf packs the size as (size & 0xFF) << 24, so a
            // swapped pair renders at the low byte of an RGB value.
            gui_text_ttf_centered(win, PD_BADGE_X, PD_NAME_Y, PD_BADGE_W, PD_FLD_H,
                                  "DEFAULT PROFILE", ok, 10);
        }
        win_draw_rect(win, PD_R_X, PD_SEP1_Y, PD_R_W, 1, line);

        win_draw_text_ttf(win, PD_R_X, PD_SCHEME_LBL_Y, "CELL GRID  /  COLOUR SCHEME", 11, lbl);
        gui_list_draw(win, &slist, ssel, theme_color(THEME_COLOR_TEXTBOX_BG),
                      focus == PDF_SCHEME ? theme_color(THEME_COLOR_FOCUS_RING) : line,
                      theme_color(THEME_COLOR_TEXTBOX_TEXT),
                      theme_color(THEME_COLOR_SELECTION),
                      theme_color(THEME_COLOR_SELECTION_TEXT),
                      pd_palette_label, pents);
        pd_draw_scheme_preview(win, p->palette, ti, cache_face, cache_style);

        win_draw_text_ttf(win, PD_R_X, PD_THEME_LBL_Y, "TERMINAL CHROME  /  WINDOW THEME", 11, lbl);
        gui_list_draw(win, &tlist, tsel, theme_color(THEME_COLOR_TEXTBOX_BG),
                      focus == PDF_THEME ? theme_color(THEME_COLOR_FOCUS_RING) : line,
                      theme_color(THEME_COLOR_TEXTBOX_TEXT),
                      theme_color(THEME_COLOR_SELECTION),
                      theme_color(THEME_COLOR_SELECTION_TEXT),
                      pd_theme_label, ents);
        pd_draw_theme_preview(win, ti);

        win_draw_rect(win, PD_R_X, PD_SEP2_Y, PD_R_W, 1, line);

        // Font
        win_draw_text_ttf(win, PD_R_X, PD_FONT_Y, "Font", 13, lbl);
        {
            char cap[96];
            snprintf(cap, sizeof(cap), "%s %s %dpt",
                     p->font_family[0] ? p->font_family : "Default",
                     p->font_style[0] ? p->font_style : "Regular", p->font_size);
            win_draw_text_ttf(win, PD_CTL_X, PD_FONT_Y, cap, 13,
                              theme_color(THEME_COLOR_TEXTBOX_TEXT));
        }
        gui_button(win, PD_FONT_BTN_X, PD_FONT_Y - 6, PD_FONT_BTN_W, PD_SBH, "Choose...",
                   GUI_BTN_SECONDARY, GUI_ST_NORMAL);

        // Cursor: three adjacent buttons acting as a segmented control (the
        // shared button primitive, not a hand-rolled widget), plus blink.
        // A focus ring around the whole row: these two stops are groups of
        // controls, not single widgets, so the ring goes round the group.
        if (focus == PDF_CURSOR)
            gui_draw_rect(win, PD_R_X - 4, PD_CUR_Y - 9, PD_R_W - 10, 30,
                          theme_color(THEME_COLOR_FOCUS_RING));
        win_draw_text_ttf(win, PD_R_X, PD_CUR_Y, "Cursor", 13, lbl);
        gui_button(win, PD_CTL_X, PD_CUR_Y - 6, PD_CUR_BLOCK_W, PD_SBH, "Block",
                   p->cursor_shape == TERM_CURSOR_BLOCK ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, GUI_ST_NORMAL);
        gui_button(win, PD_CUR_UND_X, PD_CUR_Y - 6, PD_CUR_UND_W, PD_SBH, "Underline",
                   p->cursor_shape == TERM_CURSOR_UNDERLINE ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, GUI_ST_NORMAL);
        gui_button(win, PD_CUR_BAR_X, PD_CUR_Y - 6, PD_CUR_BAR_W, PD_SBH, "Bar",
                   p->cursor_shape == TERM_CURSOR_BAR ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, GUI_ST_NORMAL);
        // gui_checkbox() draws its own label; do not add a second one beside it.
        gui_checkbox(win, PD_CHK_X, PD_CUR_Y - 1, PD_CHK_SZ, p->cursor_blink ? true : false,
                     "Blink", GUI_ST_NORMAL);

        // Scrollback. The byte cost is SHOWN, and computed from
        // sizeof(term_cell_t) rather than written down, because the cell grew
        // from 3 bytes to 16 when the emulation core landed and every
        // hand-written figure in this tree went stale in the same commit. At 16
        // bytes a 170-column line is 2720 bytes, so the top stop is 27 MB: not
        // unaffordable, but not something a slider should let a drag land on
        // without saying so.
        if (focus == PDF_SB)
            gui_draw_rect(win, PD_R_X - 4, PD_SB_Y - 5, PD_R_W - 10, 30,
                          theme_color(THEME_COLOR_FOCUS_RING));
        win_draw_text_ttf(win, PD_R_X, PD_SB_Y, "Scrollback", 13, lbl);
        {
            int si = pd_sb_index(p->scrollback);
            gui_slider(win, PD_CTL_X, PD_SB_Y + 8, PD_SB_TRACK_W, si, TERM_SB_STOPS - 1, GUI_ST_NORMAL);
            char cap[64];
            long bytes = (long)term_sb_stops[si] * TERM_MAX_COLS * (long)sizeof(term_cell_t);
            snprintf(cap, sizeof(cap), "%d lines", term_sb_stops[si]);
            win_draw_text_ttf(win, PD_CTL_X + PD_SB_TRACK_W + 16, PD_SB_Y, cap, 13,
                              theme_color(THEME_COLOR_TEXTBOX_TEXT));
            snprintf(cap, sizeof(cap), "%ld.%ld MB", bytes / 1048576, (bytes % 1048576) * 10 / 1048576);
            win_draw_text_ttf(win, PD_FONT_BTN_X, PD_SB_Y + 1, cap, 11, hint);
        }

        win_draw_text_ttf(win, PD_R_X, PD_DIR_Y, "Starting directory", 13, lbl);
        pd_field(win, PD_FLD2_X, PD_DIR_Y - 6, PD_FLD2_W, PD_FLD_H, p->start_dir, pd_caret[1], focus == PDF_DIR);
        win_draw_text_ttf(win, PD_R_X, PD_CMD_Y, "Command on start", 13, lbl);
        pd_field(win, PD_FLD2_X, PD_CMD_Y - 6, PD_FLD2_W, PD_FLD_H, p->start_cmd, pd_caret[2], focus == PDF_CMD);
        win_draw_text_ttf(win, PD_FLD2_X, PD_CMD_Y + 22,
                          "Starting directory and command apply to NEW terminal windows.", 11, hint);

        // ---- foot ----------------------------------------------------------
        win_draw_rect(win, 0, PD_FOOT_SEP_Y, PD_W, 1, line);
        if (save_err) {
            uint32_t dang = gui_ensure_contrast(theme_color(THEME_COLOR_DANGER), bg, GUI_FLOOR_TEXT);
            win_draw_text_ttf(win, PD_L_X, PD_BTN_Y + 1,
                              "COULD NOT SAVE to " TERM_PROFILE_CFG " - this session cannot write there.", 11, dang);
            win_draw_text_ttf(win, PD_L_X, PD_BTN_Y + 15,
                              "Applied to this window; it will be lost on restart.", 11, dang);
        } else {
            win_draw_text_ttf(win, PD_L_X, PD_BTN_Y + 7,
                              "Colour scheme paints the CELL GRID. Window theme paints the TABS, PANE HEADERS and SELECTION.", 11, hint);
        }
        gui_button(win, PD_CANCEL_X, PD_BTN_Y, PD_BTNW, PD_BH, "Cancel", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
        gui_button(win, PD_OK_X,     PD_BTN_Y, PD_BTNW, PD_BH, "OK",     GUI_BTN_PRIMARY,   GUI_ST_NORMAL);
        win_invalidate(win);

        gui_event_t ev;
        int et = win_get_event(win, &ev, 200);
        if (et == 0) continue;

        switch (ev.type) {
            case EVENT_WINDOW_CLOSE:
                running = 0;
                break;

            case EVENT_KEY_DOWN: {
                if (ev.keycode == GUI_KEY_ESC) { running = 0; break; }
                if (ev.keycode == GUI_KEY_TAB) { focus = (focus + 1) % PDF_COUNT; break; }
                // Enter is the DEFAULT BUTTON, and it is checked BEFORE the
                // text-field branch on purpose: in every dialog on this desktop
                // Enter commits, including while a field has focus. Tab moves
                // between controls, Esc cancels.
                if (ev.keycode == GUI_KEY_ENTER || ev.key_char == '\n' || ev.key_char == '\r')
                    goto do_ok;
                if (focus == PDF_NAME || focus == PDF_DIR || focus == PDF_CMD) {
                    int fi2 = pd_caret_slot(focus);
                    char *buf = (focus == PDF_NAME) ? p->name
                              : (focus == PDF_DIR)  ? p->start_dir : p->start_cmd;
                    int cap  = (focus == PDF_NAME) ? TERM_PROFILE_NAME_MAX
                              : (focus == PDF_DIR)  ? TERM_PROFILE_DIR_MAX : TERM_PROFILE_CMD_MAX;
                    int len = 0; while (buf[len]) len++;
                    tf_attach(&pd_tf, buf, cap, len, pd_caret[fi2]);
                    tf_handle_key(&pd_tf, &ev);
                    pd_caret[fi2] = pd_tf.cursor;
                    break;
                }
                if (focus == PDF_CURSOR) {
                    // Left/Right picks the shape, Space toggles blink. Both are
                    // wrapped, so the row can be cycled without looking.
                    if (ev.keycode == GUI_KEY_LEFT)
                        p->cursor_shape = (p->cursor_shape + TERM_CURSOR_SHAPE_COUNT - 1) % TERM_CURSOR_SHAPE_COUNT;
                    else if (ev.keycode == GUI_KEY_RIGHT)
                        p->cursor_shape = (p->cursor_shape + 1) % TERM_CURSOR_SHAPE_COUNT;
                    else if (ev.key_char == ' ')
                        p->cursor_blink = !p->cursor_blink;
                    break;
                }
                if (focus == PDF_SB) {
                    int si = pd_sb_index(p->scrollback);
                    if (ev.keycode == GUI_KEY_LEFT  && si > 0) p->scrollback = term_sb_stops[si - 1];
                    if (ev.keycode == GUI_KEY_RIGHT && si < TERM_SB_STOPS - 1) p->scrollback = term_sb_stops[si + 1];
                    break;
                }
                // Insert adds a profile from the list. Delete is deliberately
                // NOT bound: it is the one destructive action here and there is
                // no confirm step, so it stays behind a deliberate click rather
                // than one keystroke away from a list that has focus by default.
                if (focus == PDF_PROFILES && ev.keycode == GUI_KEY_INS) {
                    term_profile_t seed;
                    term_profile_defaults(&seed);
                    int ni = term_profile_new("Profile", &seed);
                    if (ni >= 0) { sel = ni; focus = PDF_NAME; }
                    break;
                }
                if (ev.keycode == GUI_KEY_UP || ev.keycode == GUI_KEY_DOWN) {
                    int d = (ev.keycode == GUI_KEY_DOWN) ? 1 : -1;
                    if (focus == PDF_PROFILES) gui_list_move_sel(&plist, &sel, d);
                    else if (focus == PDF_SCHEME) {
                        gui_list_move_sel(&slist, &ssel, d);
                        str_copy(p->palette, pents[ssel].slug, GUI_PALETTE_SLUG_MAX);
                    } else if (focus == PDF_THEME) {
                        gui_list_move_sel(&tlist, &tsel, d);
                        str_copy(p->theme, ents[tsel].slug, GUI_THEME_SLUG_MAX);
                    }
                }
                break;
            }

            case EVENT_MOUSE_DOWN:
                // The hit band matches what gui_slider() actually PAINTS: it draws its
                // thumb from y..y+16 around the track it puts at y+5, and it is
                // handed PD_SB_Y+8. A band computed from the track alone would
                // leave the bottom two rows of the visible thumb unclickable.
                sb_drag = 0;   // a press that misses the band ENDS any stale drag
                if (pd_hit(ev.mouse_x, ev.mouse_y, PD_CTL_X - 8, PD_SB_Y - 2, PD_SB_TRACK_W + 16, 26)) {
                    sb_drag = 1;
                    focus = PDF_SB;
                    p->scrollback = term_sb_stops[pd_sb_at(ev.mouse_x)];
                    break;
                }
                // Every list here draws a real scrollbar (gui_list_draw), and a
                // scrollbar nobody wires up is a control that looks draggable
                // and is not. gui_list_press() takes the scrollbar first and
                // only then reports a row, so this is the shared widget's own
                // intended wiring, not a second hit-test.
                // Remember WHICH list (if any) the press landed a ROW on.
                // gui_list_press() may instead consume the click as a scrollbar
                // grab, and a thumb drag that drifts left out of the 12px gutter
                // ends its MOUSE_UP over the rows - where an unconditional
                // gui_list_row_at() would read it as "the user picked this
                // scheme". gui_list.h points at gui_list_press_consumed() for
                // exactly this and THAT FUNCTION DOES NOT EXIST anywhere in
                // userland/libc; only the comment recommending it does. So the
                // press outcome is captured here instead.
                press_row = 0;
                if (gui_list_press(&plist, ev.mouse_x, ev.mouse_y) >= 0) press_row = 1;
                if (gui_list_press(&slist, ev.mouse_x, ev.mouse_y) >= 0) press_row = 2;
                if (gui_list_press(&tlist, ev.mouse_x, ev.mouse_y) >= 0) press_row = 3;
                break;

            case EVENT_MOUSE_MOVE:
                if (sb_drag) { p->scrollback = term_sb_stops[pd_sb_at(ev.mouse_x)]; break; }
                gui_list_motion(&plist, ev.mouse_x, ev.mouse_y);
                gui_list_motion(&slist, ev.mouse_x, ev.mouse_y);
                gui_list_motion(&tlist, ev.mouse_x, ev.mouse_y);
                break;

            case EVENT_MOUSE_UP: {
                int mx = ev.mouse_x, my = ev.mouse_y;
                gui_list_release(&plist);
                gui_list_release(&slist);
                gui_list_release(&tlist);
                if (sb_drag) { sb_drag = 0; break; }

                // A row is only honoured when the PRESS landed on that same
                // list's rows (see the EVENT_MOUSE_DOWN comment).
                int row = (press_row == 1) ? gui_list_row_at(&plist, mx, my) : -1;
                if (row >= 0) { sel = row; focus = PDF_PROFILES; press_row = 0; break; }
                int srow = (press_row == 2) ? gui_list_row_at(&slist, mx, my) : -1;
                if (srow >= 0) {
                    str_copy(p->palette, pents[srow].slug, GUI_PALETTE_SLUG_MAX);
                    focus = PDF_SCHEME; press_row = 0; break;
                }
                int trow = (press_row == 3) ? gui_list_row_at(&tlist, mx, my) : -1;
                if (trow >= 0) {
                    str_copy(p->theme, ents[trow].slug, GUI_THEME_SLUG_MAX);
                    focus = PDF_THEME; press_row = 0; break;
                }
                if (press_row) { press_row = 0; break; }

                if (pd_hit(mx, my, PD_L_X, PD_BROW1_Y, PD_BW, PD_BH)) {          // New
                    term_profile_t seed;
                    term_profile_defaults(&seed);
                    int ni = term_profile_new("Profile", &seed);
                    if (ni >= 0) { sel = ni; focus = PDF_NAME; }
                    break;
                }
                if (pd_hit(mx, my, PD_B2_X, PD_BROW1_Y, PD_BW, PD_BH)) {         // Duplicate
                    term_profile_t copy = *p;
                    int ni = term_profile_new(p->name, &copy);
                    if (ni >= 0) { sel = ni; focus = PDF_NAME; }
                    break;
                }
                if (pd_hit(mx, my, PD_L_X, PD_BROW2_Y, PD_BW, PD_BH)) {          // Delete
                    int ni = term_profile_delete(sel);
                    if (ni >= 0) { sel = ni; cache_sel = -1; }
                    break;
                }
                if (pd_hit(mx, my, PD_B2_X, PD_BROW2_Y, PD_BW, PD_BH)) {         // Set default
                    g_term_profile_default = sel;
                    break;
                }
                if (pd_hit(mx, my, PD_R_X, PD_NAME_Y, PD_NAME_W, PD_FLD_H)) { focus = PDF_NAME; break; }
                if (pd_hit(mx, my, PD_FLD2_X, PD_DIR_Y - 6, PD_FLD2_W, PD_FLD_H)) { focus = PDF_DIR; break; }
                if (pd_hit(mx, my, PD_FLD2_X, PD_CMD_Y - 6, PD_FLD2_W, PD_FLD_H)) { focus = PDF_CMD; break; }

                if (pd_hit(mx, my, PD_FONT_BTN_X, PD_FONT_Y - 6, PD_FONT_BTN_W, PD_SBH)) {
                    // The shared ChooseFont dialog, in place. g_term_font is
                    // the live selection and is restored on Cancel, so using it
                    // as the dialog's in/out buffer costs nothing and avoids a
                    // second font-selection struct.
                    str_copy(g_term_font.family, p->font_family, GUI_FONT_NAME_MAX);
                    str_copy(g_term_font.style,  p->font_style,  GUI_FONT_STYLE_MAX);
                    g_term_font.size = p->font_size;
                    gui_font_resolve(g_term_font.family, g_term_font.style,
                                     &g_term_font.face, &g_term_font.style_bits);
                    g_term_font.title = "Terminal Font";
                    g_term_font.preview_text = "user@maytera:~$ ls -la";
                    gui_font_dialog(&g_term_font);
                    str_copy(p->font_family, g_term_font.family, GUI_FONT_NAME_MAX);
                    str_copy(p->font_style,  g_term_font.style,  GUI_FONT_STYLE_MAX);
                    p->font_size = g_term_font.size;
                    cache_sel = -1;   // re-resolve the preview face
                    break;
                }
                if (pd_hit(mx, my, PD_CTL_X, PD_CUR_Y - 6, PD_CUR_BLOCK_W, PD_SBH)) { p->cursor_shape = TERM_CURSOR_BLOCK; focus = PDF_CURSOR; break; }
                if (pd_hit(mx, my, PD_CUR_UND_X, PD_CUR_Y - 6, PD_CUR_UND_W, PD_SBH)) { p->cursor_shape = TERM_CURSOR_UNDERLINE; focus = PDF_CURSOR; break; }
                if (pd_hit(mx, my, PD_CUR_BAR_X, PD_CUR_Y - 6, PD_CUR_BAR_W, PD_SBH)) { p->cursor_shape = TERM_CURSOR_BAR; focus = PDF_CURSOR; break; }
                if (pd_hit(mx, my, PD_CHK_X - 2, PD_CUR_Y - 4, PD_CHK_SZ + 60, PD_CHK_SZ + 8)) {
                    p->cursor_blink = !p->cursor_blink; focus = PDF_CURSOR; break;
                }

                if (pd_hit(mx, my, PD_CANCEL_X, PD_BTN_Y, PD_BTNW, PD_BH)) { running = 0; break; }
                if (pd_hit(mx, my, PD_OK_X, PD_BTN_Y, PD_BTNW, PD_BH)) {
do_ok:
                    // The name is what `default=` refers to on disk, so two
                    // profiles must never share one. Enforced here rather than
                    // per-keystroke so a rename can pass through an
                    // intermediate colliding value while being typed.
                    for (int i = 0; i < g_term_profile_count; i++) {
                        if (!g_term_profiles[i].name[0])
                            str_copy(g_term_profiles[i].name, "Profile", TERM_PROFILE_NAME_MAX);
                        // Bounded: at most one pass per earlier profile, and a
                        // name that cannot take a suffix (already at the length
                        // cap) is simply left alone rather than retried, which
                        // is what an unbounded "start over" loop would have
                        // spun on forever.
                        for (int attempt = 0; attempt < TERM_PROFILE_MAX; attempt++) {
                            int clash = 0;
                            for (int j = 0; j < i; j++)
                                if (str_eq(g_term_profiles[i].name, g_term_profiles[j].name)) { clash = 1; break; }
                            if (!clash) break;
                            int n = 0; while (g_term_profiles[i].name[n]) n++;
                            if (n > TERM_PROFILE_NAME_MAX - 4) break;
                            g_term_profiles[i].name[n]     = ' ';
                            g_term_profiles[i].name[n + 1] = (char)('2' + (attempt % 8));
                            g_term_profiles[i].name[n + 2] = 0;
                        }
                    }
                    g_term_profile_active = sel;
                    term_profile_apply(&g_term_profiles[sel]);
                    // BOTH results are checked, and a failure KEEPS THE DIALOG
                    // OPEN. Closing on a failed write is the worst available
                    // behaviour: everything looks applied (it IS applied, in
                    // memory), and the settings are simply gone after a reboot
                    // with nothing ever having said so. That is the shape of
                    // #132, which stayed open because the drag-then-reboot
                    // check was never run.
                    int e_prof = term_profiles_save();
                    term_prefs_save();
                    if (e_prof != 0 || g_term_pref_save_failed) {
                        save_err = 1;
                        changed = 1;   // the LIVE state did change; only the save failed
                        break;         // stay open, and say so
                    }
                    save_err = 0;
                    changed = 1;
                    running = 0;
                    break;
                }
                break;
            }

            case EVENT_MOUSE_SCROLL:
                if (gui_list_wheel(&plist, ev.mouse_x, ev.mouse_y, ev.scroll_delta)) break;
                if (gui_list_wheel(&slist, ev.mouse_x, ev.mouse_y, ev.scroll_delta)) break;
                gui_list_wheel(&tlist, ev.mouse_x, ev.mouse_y, ev.scroll_delta);
                break;

            default:
                break;
        }
    }

    win_destroy(win);
    if (!changed) {
        // Cancel / close: nothing was written, so the table is restored by
        // re-reading the file, and the live appearance by re-applying the
        // snapshot taken when the dialog opened.
        // Restore the LIVE state FIRST. term_profiles_load() falls back to
        // seeding a "Default" profile FROM LIVE STATE when TERMPROF.CFG cannot
        // be read at that instant, and in the other order that seed would
        // capture the font the user had just picked and then cancelled,
        // persisting the one edit Cancel exists to discard.
        term_profile_apply(&entry_live);
        term_profiles_load();
        g_term_profile_active = entry_active;
        if (g_term_profile_active < 0 || g_term_profile_active >= g_term_profile_count)
            g_term_profile_active = g_term_profile_default;
        term_profile_apply(&entry_live);
    }
    if (window_handle >= 0) term_handle_resize(term_px_w, term_px_h);
    return changed;
}

// ---------------------------------------------------------------------------
// NOT SHIPPED, DELIBERATELY: a transparency control.
//
// The brief was "transparency if the compositor supports it. CHECK whether it
// does before offering it." It was checked, in source, and it does not - not
// in the way a per-profile control needs.
//
// SYS_SET_WIN_OPACITY (233) is the only opacity entry point userland has, and
// it is GLOBAL: kernel/gui/window.c's wm_set_default_opacity() sets
// g_default_window_opacity AND then walks the entire window list assigning that
// same value to every window. A terminal calling it would fade the whole
// desktop. Worse, the reverse is also true: the Settings / tray "Window
// Opacity" slider calls the same function, so ANY later move of that slider
// would silently overwrite whatever a terminal had set for itself. A control
// that works until an unrelated slider is touched somewhere else is a #208 dead
// control with extra steps, and shipping one to fill a checkbox on a brief is
// exactly the failure this app is not allowed to repeat.
//
// The per-window field (window_t::opacity) and the blit path that honours it
// (window.c's `if (win->opacity >= 255)` fast paths) BOTH already exist, so the
// gap is narrow and worth filing: one self-scoped syscall that sets only the
// calling process's own window, plus a decision about who wins when the global
// slider moves afterwards. Until that exists there is nothing honest to draw
// here, so nothing is drawn.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Tool contract (#241/#233): "you can drive settings through the new
// contract API" per the ticket. Static rows (the app's control surface -
// theme/font/size - is not one shared draw/hit table the way Calculator's
// buttons are), gated by tools/contract-lint (repo-guard check 15) only in
// the sense that every NEW control here has a row; contract-lint's
// structural checks today are scoped to settings.c/calc.c, so this is the
// terminal's OWN honest, if not yet structurally-enforced, contract.
//
// theme/font_family are CT_ENUM over a FIXED list (the 14 shipped themes;
// the six fonts known to ship in /FONTS, three monospace + three not, so a
// headless caller can prove BOTH "a real font changed the render" and "a
// proportional choice does not crash it"). A user-installed App Store theme
// or font is reachable only via the dialog above, not via this contract -
// documented, not silently unsupported.
// ---------------------------------------------------------------------------
#define TERM_CT_THEME_OPTS \
    "dark|light|retro_unix|classic|ocean|sunset|forest|high_contrast|" \
    "modern_light|modern_dark|fluent_light|fluent_dark|maytera_light|maytera_dark"
static const char *const TERM_CT_THEME_SLUGS[14] = {
    "dark","light","retro_unix","classic","ocean","sunset","forest","high_contrast",
    "modern_light","modern_dark","fluent_light","fluent_dark","maytera_light","maytera_dark"
};
#define TERM_CT_FONT_OPTS \
    "DejaVu Sans Mono|Inconsolata|IBM Plex Mono|Source Code Pro|DejaVu Sans|Lato"
static const char *const TERM_CT_FONT_FAMILIES[6] = {
    "DejaVu Sans Mono", "Inconsolata", "IBM Plex Mono", "Source Code Pro",
    "DejaVu Sans", "Lato"
};

static int ct_term_theme_get(void) {
    for (int i = 0; i < 14; i++)
        if (strcmp(TERM_CT_THEME_SLUGS[i], g_term_theme_slug) == 0) return i;
    return 0;
}
static int ct_term_theme_set(int v) {
    if (v < 0 || v >= 14) return -1;
    strncpy(g_term_theme_slug, TERM_CT_THEME_SLUGS[v], GUI_THEME_SLUG_MAX - 1);
    g_term_theme_slug[GUI_THEME_SLUG_MAX - 1] = 0;
    term_resolve_theme();
    return 0;
}
static int ct_term_font_get(void) {
    for (int i = 0; i < 6; i++)
        if (strcmp(TERM_CT_FONT_FAMILIES[i], g_term_font.family) == 0) return i;
    return -1;
}
static int ct_term_font_set(int v) {
    if (v < 0 || v >= 6) return -1;
    strncpy(g_term_font.family, TERM_CT_FONT_FAMILIES[v], GUI_FONT_NAME_MAX - 1);
    strncpy(g_term_font.style, "Regular", GUI_FONT_STYLE_MAX - 1);
    gui_font_resolve(g_term_font.family, g_term_font.style,
                     &g_term_font.face, &g_term_font.style_bits);
    term_apply_font();
    return 0;
}

// Tier 2 (docs/TERMINAL_PARITY.md): the seven shipped colour schemes plus
// "system" (Follow system theme, the default). This list is a curated FIXED
// set for headless contract testing, same reasoning as TERM_CT_THEME_OPTS/
// TERM_CT_FONT_OPTS above; the F9 dialog reaches every /PALETTES/*.tpalette
// file via gui_palette_list(), including any an App Store package adds.
#define TERM_CT_PALETTE_OPTS \
    "system|classic|homebrew|solarized_dark|solarized_light|tango|campbell|novel"
static const char *const TERM_CT_PALETTE_SLUGS[8] = {
    "system", "classic", "homebrew", "solarized_dark", "solarized_light",
    "tango", "campbell", "novel"
};
static int ct_term_palette_get(void) {
    for (int i = 0; i < 8; i++)
        if (strcmp(TERM_CT_PALETTE_SLUGS[i], g_term_palette_slug) == 0) return i;
    return 0;
}
static int ct_term_palette_set(int v) {
    if (v < 0 || v >= 8) return -1;
    strncpy(g_term_palette_slug, TERM_CT_PALETTE_SLUGS[v], GUI_PALETTE_SLUG_MAX - 1);
    g_term_palette_slug[GUI_PALETTE_SLUG_MAX - 1] = 0;
    term_resolve_palette();
    return 0;
}

// Profiles added three more real controls to this app's surface, so they get
// contract rows too. This is not decoration: it is what lets the persistence
// claim be VERIFIED headlessly (set, reboot, get) instead of asserted from
// reading the save code, which is exactly how #132's drag-then-reboot check
// came to be skipped.
#define TERM_CT_CURSOR_OPTS "block|underline|bar"
static int ct_term_cursor_get(void) { return g_term_cursor_shape; }
static int ct_term_cursor_set(int v) {
    if (v < 0 || v >= TERM_CURSOR_SHAPE_COUNT) return -1;
    g_term_cursor_shape = v;
    return 0;
}
static int ct_term_sb_get(void) { return sb_want; }
static int ct_term_sb_set(int v) {
    if (v < 200 || v > 20000) return -1;
    term_scrollback_set_capacity(v);
    return 0;
}
// Read-only: switching profile is a GUI action with a preview attached, and a
// headless caller that wants a profile's settings can set them individually.
// Reporting WHICH profile is live is what a test actually needs.
static int ct_term_profile_get(char *out, int ocap) {
    const char *n = (g_term_profile_active >= 0 && g_term_profile_active < g_term_profile_count)
                    ? g_term_profiles[g_term_profile_active].name : "";
    int i = 0;
    while (n[i] && i < ocap - 1) { out[i] = n[i]; i++; }
    out[i] = 0;
    return 0;
}

static const ct_item_t TERMINAL_ITEMS[] = {
    { "theme", CT_ENUM, CT_RW, CT_SAFE, 0, 0, 13, TERM_CT_THEME_OPTS,
      0, ct_term_theme_get, ct_term_theme_set, 0, 0, 0, 0,
      "WINDOW THEME (chrome), independent of the OS system theme (default: dark). "
      "This is NOT the terminal's ANSI colour scheme - see colour_scheme below." },
    { "colour_scheme", CT_ENUM, CT_RW, CT_SAFE, 0, 0, 7, TERM_CT_PALETTE_OPTS,
      0, ct_term_palette_get, ct_term_palette_set, 0, 0, 0, 0,
      "COLOUR SCHEME: the 16 ANSI colours + default fg/bg/cursor/selection "
      "(Solarized/Tango/Campbell/...), in the Terminal.app/Konsole sense. "
      "'system' (default) follows the theme item above instead of a named scheme." },
    { "font_family", CT_ENUM, CT_RW, CT_SAFE, 0, 0, 5, TERM_CT_FONT_OPTS,
      0, ct_term_font_get, ct_term_font_set, 0, 0, 0, 0,
      "Terminal font family (cell width/height re-derived from its metrics)" },
    { "font_size", CT_INT, CT_RW, CT_SAFE, 0, 8, 32, 0,
      &g_term_font.size, 0, 0, 0, 0, 0, 0,
      "Terminal font point size, 8-32" },
    { "cursor_shape", CT_ENUM, CT_RW, CT_SAFE, 0, 0, 2, TERM_CT_CURSOR_OPTS,
      0, ct_term_cursor_get, ct_term_cursor_set, 0, 0, 0, 0,
      "Cursor shape drawn in the cell grid: full block, baseline underline, or "
      "left-edge bar. Default underline, which is what it always was." },
    { "cursor_blink", CT_BOOL, CT_RW, CT_SAFE, 0, 0, 1, 0,
      &g_term_cursor_blink, 0, 0, 0, 0, 0, 0,
      "1 = the cursor blinks (~500ms), 0 = solid. The renderer honours this, "
      "so turning it off cannot desync from the event loop's blink counter." },
    { "scrollback", CT_INT, CT_RW, CT_SAFE, 0, 200, 20000, 0,
      0, ct_term_sb_get, ct_term_sb_set, 0, 0, 0, 0,
      "Retained scrollback lines. The ring is reallocated (newest lines kept) "
      "at the next redraw; the dialog offers 500/1000/2000/5000/10000." },
    { "profile", CT_STR, CT_READ, CT_SAFE, 0, 0, 0, 0,
      0, 0, 0, 0, ct_term_profile_get, 0, 0,
      "Name of the PROFILE this terminal is running. Profiles are the source "
      "of truth; /CONFIG/TERMPREF.CFG is a mirror of the live state." },
};

// After a var-backed write (font_size) contract.c does not know to recompute
// metrics or persist; after a getfn/setfn write the setter already updated
// live state but did not persist either. commit() is the ONE place both
// paths funnel through (contract.c calls it after every successful set/call
// - see libc/contract.c ct_do_set()), so it is the single spot that always
// re-derives cell metrics and saves, regardless of which row changed.
static void term_contract_commit(void) {
    term_apply_font();
    // Profiles are the source of truth (term_profile.h), so a contract write
    // has to land THERE as well as in the mirror, or `terminal --contract set
    // colour_scheme=tango` would be undone by the next launch reading the
    // default profile back. capture() copies only the live APPEARANCE fields,
    // leaving the profile's name, starting directory and command alone.
    if (g_term_profile_active >= 0 && g_term_profile_active < g_term_profile_count) {
        term_profile_capture(&g_term_profiles[g_term_profile_active]);
        term_profiles_save();
    }
    term_prefs_save();
}

const ct_contract_t TERMINAL_CONTRACT = {
    "terminal", "Terminal",
    "theme (window chrome) and colour_scheme (the 16 ANSI colours) are TWO "
    "DIFFERENT THINGS, both independent of the OS system theme; theme "
    "defaults to Dark, colour_scheme defaults to 'system' (follow theme "
    "above). font_family is a curated 6-name list (3 monospace, 3 not) for "
    "headless testing - the GUI preferences dialog (F9) reaches the full "
    "installed theme/colour-scheme/font sets via gui_theme_list()/"
    "gui_palette_list()/gui_font_dialog(). Every row here belongs to a named "
    "PROFILE (/CONFIG/TERMPROF.CFG); a write lands in the active profile as "
    "well as in the live mirror, so it survives a reboot.",
    TERMINAL_ITEMS, (int)(sizeof(TERMINAL_ITEMS) / sizeof(TERMINAL_ITEMS[0])),
    0,                    // no projection: three rows, not a drawn button table
    term_prefs_load,      // load: read TERMPREF.CFG before answering get/set
    term_contract_commit  // commit: re-derive metrics + persist after a set
};
