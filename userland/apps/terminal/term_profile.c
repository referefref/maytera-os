// term_profile.c - the named-profile store. See term_profile.h for the design,
// the two-file truth rule, and the on-disk format.

#include "term_common.h"
#include "term_util.h"
#include "term_grid.h"
#include "term_scrollback.h"
#include "term_render.h"
#include "term_theme.h"
#include "term_prefs.h"
#include "term_profile.h"

const int term_sb_stops[TERM_SB_STOPS] = { 500, 1000, 2000, 5000, 10000 };

term_profile_t g_term_profiles[TERM_PROFILE_MAX];
int g_term_profile_count   = 0;
int g_term_profile_default = 0;
int g_term_profile_active  = 0;

// One buffer, reused for both directions. 12 profiles * 11 keys * ~64 bytes is
// under 8.5 KB; 12 KB leaves room for hand-added comments and long paths, and
// a file bigger than this is truncated at a line boundary by the parser rather
// than misread (see tp_read).
#define TP_BUF 12288
static char tp_buf[TP_BUF];

// --- cursor shape <-> name -------------------------------------------------
// Written as a name, not an integer, so the file stays hand-editable and so
// renumbering TERM_CURSOR_* can never silently reinterpret an existing file.
static const char *const TP_CURSOR_NAMES[TERM_CURSOR_SHAPE_COUNT] = {
    "block", "underline", "bar"
};
static int tp_cursor_from_name(const char *s) {
    for (int i = 0; i < TERM_CURSOR_SHAPE_COUNT; i++)
        if (str_eq(s, TP_CURSOR_NAMES[i])) return i;
    return TERM_CURSOR_UNDERLINE;
}
static const char *tp_cursor_name(int shape) {
    if (shape < 0 || shape >= TERM_CURSOR_SHAPE_COUNT) shape = TERM_CURSOR_UNDERLINE;
    return TP_CURSOR_NAMES[shape];
}

void term_profile_defaults(term_profile_t *p) {
    memset(p, 0, sizeof(*p));
    str_copy(p->name,        TERM_PROFILE_DEFAULT_NAME, TERM_PROFILE_NAME_MAX);
    str_copy(p->palette,     GUI_PALETTE_SYSTEM_SLUG,   GUI_PALETTE_SLUG_MAX);
    str_copy(p->theme,       TERM_DEFAULT_THEME_SLUG,   GUI_THEME_SLUG_MAX);
    str_copy(p->font_family, TERM_DEFAULT_FONT_FAMILY,  GUI_FONT_NAME_MAX);
    str_copy(p->font_style,  TERM_DEFAULT_FONT_STYLE,   GUI_FONT_STYLE_MAX);
    p->font_size     = TERM_DEFAULT_FONT_SIZE;
    p->cursor_shape  = TERM_CURSOR_UNDERLINE;
    p->cursor_blink  = 1;
    p->scrollback    = SCROLLBACK_LINES;
    p->start_dir[0]  = 0;
    p->start_cmd[0]  = 0;
}

// --- live state <-> profile -------------------------------------------------
// capture() deliberately does NOT touch name/start_dir/start_cmd: those three
// are properties of the PROFILE, not of the running window, and there is no
// live variable that could round-trip them. Overwriting them from "live state"
// would blank a profile's starting directory every time anything else was
// captured.
void term_profile_capture(term_profile_t *p) {
    str_copy(p->palette, g_term_palette_slug, GUI_PALETTE_SLUG_MAX);
    str_copy(p->theme,   g_term_theme_slug,   GUI_THEME_SLUG_MAX);
    str_copy(p->font_family,
             g_term_font.family[0] ? g_term_font.family : TERM_DEFAULT_FONT_FAMILY,
             GUI_FONT_NAME_MAX);
    str_copy(p->font_style,
             g_term_font.style[0] ? g_term_font.style : TERM_DEFAULT_FONT_STYLE,
             GUI_FONT_STYLE_MAX);
    p->font_size    = g_term_font.size;
    p->cursor_shape = g_term_cursor_shape;
    p->cursor_blink = g_term_cursor_blink;
    p->scrollback   = sb_want;
}

void term_profile_apply(const term_profile_t *p) {
    str_copy(g_term_palette_slug, p->palette, GUI_PALETTE_SLUG_MAX);
    str_copy(g_term_theme_slug,   p->theme,   GUI_THEME_SLUG_MAX);
    str_copy(g_term_font.family,  p->font_family, GUI_FONT_NAME_MAX);
    str_copy(g_term_font.style,   p->font_style,  GUI_FONT_STYLE_MAX);
    g_term_font.size = (p->font_size >= 8 && p->font_size <= 32)
                       ? p->font_size : TERM_DEFAULT_FONT_SIZE;
    g_term_cursor_shape = (p->cursor_shape >= 0 && p->cursor_shape < TERM_CURSOR_SHAPE_COUNT)
                          ? p->cursor_shape : TERM_CURSOR_UNDERLINE;
    g_term_cursor_blink = p->cursor_blink ? 1 : 0;
    term_scrollback_set_capacity(p->scrollback);
    term_resolve_theme();
    term_resolve_palette();
    gui_font_resolve(g_term_font.family, g_term_font.style,
                     &g_term_font.face, &g_term_font.style_bits);
    term_apply_font();
}

// --- parse ------------------------------------------------------------------
static int tp_read(void) {
    int fd = userconf_open_read("TERMPROF.CFG", TERM_PROFILE_CFG);
    if (fd < 0) return -1;
    // A file larger than the buffer is truncated MID-LINE at TP_BUF-1, not at a
    // line boundary: the partial last line then fails the key=value split and is
    // ignored, so the profiles before it still load.
    int n = read(fd, tp_buf, TP_BUF - 1);
    close(fd);
    if (n < 0) n = 0;
    tp_buf[n] = 0;
    return n;
}

// Trim CR and surrounding spaces in place; returns the (possibly advanced)
// start pointer.
static char *tp_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int n = 0;
    while (s[n]) n++;
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) s[--n] = 0;
    return s;
}

void term_profiles_load(void) {
    g_term_profile_count   = 0;
    g_term_profile_default = 0;

    int n = tp_read();
    char want_default[TERM_PROFILE_NAME_MAX];
    want_default[0] = 0;

    if (n > 0) {
        char *p = tp_buf;
        term_profile_t *cur = 0;
        while (*p) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p == '\n') { *p = 0; p++; }
            line = tp_trim(line);
            if (!line[0] || line[0] == '#') continue;

            if (line[0] == '[') {
                // New section: [Profile Name]
                char *close = line + 1;
                while (*close && *close != ']') close++;
                *close = 0;
                if (g_term_profile_count >= TERM_PROFILE_MAX) { cur = 0; continue; }
                cur = &g_term_profiles[g_term_profile_count++];
                term_profile_defaults(cur);
                str_copy(cur->name, line + 1, TERM_PROFILE_NAME_MAX);
                if (!cur->name[0]) str_copy(cur->name, "Profile", TERM_PROFILE_NAME_MAX);
                continue;
            }

            char *eq = line;
            while (*eq && *eq != '=') eq++;
            if (!*eq) continue;
            *eq = 0;
            char *key = tp_trim(line);
            char *val = tp_trim(eq + 1);

            if (!cur) {
                // File-level keys, before the first [Section].
                if (str_eq(key, "default")) str_copy(want_default, val, TERM_PROFILE_NAME_MAX);
                continue;
            }
            if      (str_eq(key, "palette"))    str_copy(cur->palette, val, GUI_PALETTE_SLUG_MAX);
            else if (str_eq(key, "theme"))      str_copy(cur->theme, val, GUI_THEME_SLUG_MAX);
            else if (str_eq(key, "font"))       str_copy(cur->font_family, val, GUI_FONT_NAME_MAX);
            else if (str_eq(key, "style"))      str_copy(cur->font_style, val, GUI_FONT_STYLE_MAX);
            else if (str_eq(key, "size"))       { int v = atoi(val); if (v >= 8 && v <= 32) cur->font_size = v; }
            else if (str_eq(key, "cursor"))     cur->cursor_shape = tp_cursor_from_name(val);
            else if (str_eq(key, "blink"))      cur->cursor_blink = atoi(val) ? 1 : 0;
            else if (str_eq(key, "scrollback")) { int v = atoi(val); if (v >= 200 && v <= 20000) cur->scrollback = v; }
            else if (str_eq(key, "dir"))        str_copy(cur->start_dir, val, TERM_PROFILE_DIR_MAX);
            else if (str_eq(key, "cmd"))        str_copy(cur->start_cmd, val, TERM_PROFILE_CMD_MAX);
            // Any other key: ignored on purpose, so a file written by a later
            // build still loads here instead of failing.
        }
    }

    if (g_term_profile_count == 0) {
        // No file, or an unusable one. Seed ONE profile from whatever the live
        // state currently is - which at startup is TERMPREF.CFG's saved values
        // (see term_prefs_load) - so upgrading from a build that predates
        // profiles keeps the theme/font/scheme the user had chosen, as a
        // profile called "Default", rather than resetting them.
        term_profile_defaults(&g_term_profiles[0]);
        term_profile_capture(&g_term_profiles[0]);
        str_copy(g_term_profiles[0].name, TERM_PROFILE_DEFAULT_NAME, TERM_PROFILE_NAME_MAX);
        g_term_profile_count = 1;
        g_term_profile_default = 0;
        term_profiles_save();
    } else if (want_default[0]) {
        for (int i = 0; i < g_term_profile_count; i++)
            if (str_eq(g_term_profiles[i].name, want_default)) { g_term_profile_default = i; break; }
    }
    if (g_term_profile_default >= g_term_profile_count) g_term_profile_default = 0;
    if (g_term_profile_active  >= g_term_profile_count) g_term_profile_active  = g_term_profile_default;
}

// --- write ------------------------------------------------------------------
static int tp_cat(int at, const char *s) {
    while (*s && at < TP_BUF - 1) tp_buf[at++] = *s++;
    return at;
}
static int tp_cat_int(int at, int v) {
    char n[16];
    int_to_str(v, n);
    return tp_cat(at, n);
}
static int tp_kv(int at, const char *k, const char *v) {
    at = tp_cat(at, k); at = tp_cat(at, "="); at = tp_cat(at, v); return tp_cat(at, "\n");
}
static int tp_kvi(int at, const char *k, int v) {
    at = tp_cat(at, k); at = tp_cat(at, "="); at = tp_cat_int(at, v); return tp_cat(at, "\n");
}

int term_profiles_save(void) {
    if (g_term_profile_count <= 0) return -1;
    if (g_term_profile_default < 0 || g_term_profile_default >= g_term_profile_count)
        g_term_profile_default = 0;

    int at = 0;
    at = tp_cat(at, "# MayteraOS terminal profiles. Written by the Terminal app (F9).\n");
    at = tp_cat(at, "# palette = COLOUR SCHEME, paints the CELL GRID.\n");
    at = tp_cat(at, "# theme   = WINDOW THEME, paints the TERMINAL's own chrome: the tab\n");
    at = tp_cat(at, "#           strip, pane headers and selection. NOT the OS title bar,\n");
    at = tp_cat(at, "#           which no syscall lets an app restyle. Two different things.\n");
    at = tp_kv(at, "default", g_term_profiles[g_term_profile_default].name);
    for (int i = 0; i < g_term_profile_count; i++) {
        const term_profile_t *p = &g_term_profiles[i];
        at = tp_cat(at, "\n[");
        at = tp_cat(at, p->name);
        at = tp_cat(at, "]\n");
        at = tp_kv (at, "palette",    p->palette);
        at = tp_kv (at, "theme",      p->theme);
        at = tp_kv (at, "font",       p->font_family);
        at = tp_kv (at, "style",      p->font_style);
        at = tp_kvi(at, "size",       p->font_size);
        at = tp_kv (at, "cursor",     tp_cursor_name(p->cursor_shape));
        at = tp_kvi(at, "blink",      p->cursor_blink ? 1 : 0);
        at = tp_kvi(at, "scrollback", p->scrollback);
        at = tp_kv (at, "dir",        p->start_dir);
        at = tp_kv (at, "cmd",        p->start_cmd);
    }
    tp_buf[at] = 0;

    int fd = userconf_open_write("TERMPROF.CFG");
    // #743: the result is CHECKED. A preference that silently fails to save is
    // still a bug, and it is the one users report as "it didn't save".
    if (userconf_finish_write(fd, tp_buf, (unsigned long)at) != 0) return -1;
    return 0;
}

// --- table edits ------------------------------------------------------------
// Make `base` unique in the table by appending " 2", " 3", ... The dialog can
// therefore never produce two profiles with the same name, which matters
// because `default=` in the file names the default BY NAME.
// '[' and ']' delimit a section on disk, so a name containing one would be
// written as [x]y] and read back as "x" on the next load: the profile would
// silently rename itself. Replace rather than reject, so a keystroke is never
// swallowed while the user is still typing; the substitution only happens when
// the name is committed.
static void tp_sanitise_name(char *n) {
    for (int i = 0; n[i]; i++) if (n[i] == '[' || n[i] == ']') n[i] = '(';
}

// If `base` is already within a few characters of the length cap there is no
// room for a " 2" suffix. Truncate it to make room rather than returning a name
// that still clashes, which is what the old `if (n < cap - 5)` guard did
// silently after spinning its whole attempt budget.
static void tp_unique_name(char *out, int cap, const char *base, int skip_idx) {
    str_copy(out, base, cap);
    { int n = 0; while (out[n]) n++;
      if (n > cap - 5) out[cap - 5] = 0; }
    for (int attempt = 2; attempt < 100; attempt++) {
        int clash = 0;
        for (int i = 0; i < g_term_profile_count; i++) {
            if (i == skip_idx) continue;
            if (str_eq(g_term_profiles[i].name, out)) { clash = 1; break; }
        }
        if (!clash) return;
        char suffix[8];
        int_to_str(attempt, suffix);
        str_copy(out, base, cap);
        int n = 0; while (out[n]) n++;
        if (n > cap - 5) { n = cap - 5; out[n] = 0; }
        out[n++] = ' '; out[n] = 0;
        str_copy(out + n, suffix, cap - n);
    }
}

int term_profile_new(const char *name, const term_profile_t *seed) {
    if (g_term_profile_count >= TERM_PROFILE_MAX) return -1;
    term_profile_t *p = &g_term_profiles[g_term_profile_count];
    if (seed) *p = *seed; else term_profile_defaults(p);
    char uniq[TERM_PROFILE_NAME_MAX];
    tp_unique_name(uniq, TERM_PROFILE_NAME_MAX, name && name[0] ? name : "Profile", g_term_profile_count);
    str_copy(p->name, uniq, TERM_PROFILE_NAME_MAX);
    tp_sanitise_name(p->name);
    return g_term_profile_count++;
}

int term_profile_delete(int idx) {
    // The last profile is never deletable: an empty table would leave startup
    // with nothing to apply, and "delete everything then reopen" is not a state
    // a user should be able to reach from a settings dialog.
    if (g_term_profile_count <= 1) return -1;
    if (idx < 0 || idx >= g_term_profile_count) return -1;
    for (int i = idx; i < g_term_profile_count - 1; i++)
        g_term_profiles[i] = g_term_profiles[i + 1];
    g_term_profile_count--;
    if (g_term_profile_default >= g_term_profile_count) g_term_profile_default = g_term_profile_count - 1;
    else if (g_term_profile_default > idx) g_term_profile_default--;
    if (g_term_profile_active >= g_term_profile_count) g_term_profile_active = g_term_profile_default;
    else if (g_term_profile_active > idx) g_term_profile_active--;
    return idx < g_term_profile_count ? idx : g_term_profile_count - 1;
}

// --- startup ----------------------------------------------------------------
void term_profile_startup(char *cwd_buf, int cap) {
    if (g_term_profile_count <= 0) return;
    if (g_term_profile_default < 0 || g_term_profile_default >= g_term_profile_count)
        g_term_profile_default = 0;
    g_term_profile_active = g_term_profile_default;
    term_profile_apply(&g_term_profiles[g_term_profile_active]);

    const term_profile_t *p = &g_term_profiles[g_term_profile_active];
    if (!cwd_buf || cap <= 1 || !p->start_dir[0]) return;
    // A starting directory that no longer exists must NOT strand the shell in
    // an unusable cwd: chdir's own result decides, and on failure the caller's
    // $HOME-derived value is left exactly as it was.
    if ((long)syscall1(SYS_CHDIR, (long)p->start_dir) < 0) return;
    str_copy(cwd_buf, p->start_dir, cap);
}

const char *term_profile_start_cmd(void) {
    if (g_term_profile_active < 0 || g_term_profile_active >= g_term_profile_count) return 0;
    return g_term_profiles[g_term_profile_active].start_cmd;
}
