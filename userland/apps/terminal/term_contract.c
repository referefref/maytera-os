// term_contract.c - the Terminal's tool contract. Read term_contract.h first;
// the "project, do not hand-write rows" rule and the live guard both live there.
//
// The appearance rows, their getters/setters and the commit hook were MOVED here
// verbatim from the bottom of term_prefs.c (they are marked below), because the
// contract is no longer a tail on the preferences file: it now covers the shell
// builtins, the menu bar, three runtime lists, the profile store's edit
// operations and the nine notification keys.

#include "term_common.h"
#include "term_util.h"
#include "term_grid.h"
#include "term_scrollback.h"
#include "term_render.h"
#include "term_theme.h"
#include "term_prefs.h"
#include "term_profile.h"
#include "term_shell.h"
#include "term_menu.h"
#include "term_notify.h"
#include "term_layout.h"
#include "term_search.h"
#include "term_select.h"
#include "term_contract.h"

// ---------------------------------------------------------------------------
// The live guard (term_contract.h documents why it is not
// contract_serving_live()).
// ---------------------------------------------------------------------------
static int g_tc_live = 0;
void term_contract_note_live(void) { g_tc_live = 1; }
int  term_ct_live(void) { return g_tc_live; }

int term_ct_no_live(char *out, int ocap) {
    if (g_tc_live) return 0;
    snprintf(out, (size_t)ocap,
             "needs-live-instance: this control acts on a Terminal window's tabs, "
             "panes, grid or chrome, and a --contract process has none and exits "
             "immediately. Deliver it to a RUNNING Terminal (contract_invoke_live)");
    return 1;
}

void term_ct_apply_risk(ct_item_t *it, const ct_terms_t *t) {
    int depth = t ? t->depth : CT_DEPTH_UNSPEC;
    int deep  = (depth == CT_DEPTH_FS || depth == CT_DEPTH_SYSTEM);
    it->risk  = deep ? CT_GUARDED : CT_SAFE;
    // cap STAYS 0, deliberately, and this is the one place the derivation differs
    // from the Files app's. With cap == 0, contract_authorize() gates on the
    // CONTRACT-QUALIFIED item name ("terminal.<item>"), which is exactly what the
    // audit line already records and is a TIGHTER grant than any shared
    // capability id. The alternative is to map three unlike system effects -
    // spawning a process, writing the system clipboard, rewriting a config file -
    // onto one capability name that describes only one of them, and a capability
    // id that does not describe what it gates is the #235 fault in one field.
    // The fail-closed behaviour is unchanged: with no consent callback in a CLI
    // process, aicap refuses unless a token was already granted.
    it->cap = 0;
}

// ===========================================================================
// MOVED VERBATIM FROM term_prefs.c: the appearance rows' option lists and their
// getters/setters. Nothing below this banner was rewritten in the move except
// where a comment says so.
//
// theme/font_family are CT_ENUM over a FIXED list (the 14 shipped themes; the
// six fonts known to ship in /FONTS, three monospace and three not, so a
// headless caller can prove BOTH "a real font changed the render" and "a
// proportional choice does not crash it"). A user-installed App Store theme or
// font is reachable only via the F9 dialog, not via this contract - documented,
// not silently unsupported. The COLOUR SCHEME is the exception: its rows are
// PROJECTED from the live /PALETTES list by term_menu_ct_project(), so an
// installed scheme IS reachable as `scheme.<slug>` even though the
// `colour_scheme` enum row below still offers the curated eight.
// ===========================================================================
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
// "system" (Follow system theme, the default). A curated FIXED set for headless
// contract testing, same reasoning as TERM_CT_THEME_OPTS/TERM_CT_FONT_OPTS.
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
static int ct_term_profile_get(char *out, int ocap) {
    const char *n = (g_term_profile_active >= 0 && g_term_profile_active < g_term_profile_count)
                    ? g_term_profiles[g_term_profile_active].name : "";
    int i = 0;
    while (n[i] && i < ocap - 1) { out[i] = n[i]; i++; }
    out[i] = 0;
    return 0;
}
// ===========================================================================
// END OF THE MOVED BLOCK. Everything below is new.
// ===========================================================================

// ---------------------------------------------------------------------------
// font_style: MEASURED as persisted (TERMPREF.CFG field 2, TERMPROF.CFG `style=`)
// and carried by every profile, with NO contract row at all - so the ONE
// appearance field a caller could not touch was the one the F9 dialog's font
// picker changes most often after the family. A curated FIXED list for the same
// reason the family list is curated; gui_font_resolve() falls back to the
// closest available face, which is what the dialog relies on too.
// ---------------------------------------------------------------------------
#define TERM_CT_STYLE_OPTS "Regular|Bold|Italic|Bold Italic"
static const char *const TERM_CT_STYLES[4] = { "Regular", "Bold", "Italic", "Bold Italic" };
static int ct_term_style_get(void) {
    for (int i = 0; i < 4; i++)
        if (strcmp(TERM_CT_STYLES[i], g_term_font.style) == 0) return i;
    return -1;       // an App Store face the curated list does not name
}
static int ct_term_style_set(int v) {
    if (v < 0 || v >= 4) return -1;
    strncpy(g_term_font.style, TERM_CT_STYLES[v], GUI_FONT_STYLE_MAX - 1);
    g_term_font.style[GUI_FONT_STYLE_MAX - 1] = 0;
    gui_font_resolve(g_term_font.family, g_term_font.style,
                     &g_term_font.face, &g_term_font.style_bits);
    term_apply_font();
    return 0;
}

// ---------------------------------------------------------------------------
// PROFILE: the read-only hole, closed.
//
// The `profile` row above is CT_STR and therefore READ-ONLY, with a comment
// saying a headless caller "can set them individually" instead of switching. A
// profile is not only its appearance - it carries a starting directory and a
// start command - so "set the seven appearance rows" is not the same operation,
// and marking a profile default did nothing a caller could reach either.
// profile.index makes switching a plain writable value row (terms DERIVED) and
// profile.default makes "which profile do new windows open with" one too.
// ---------------------------------------------------------------------------
static int ct_prof_index_get(void) { return g_term_profile_active; }
static int ct_prof_index_set(int v) {
    if (v < 0 || v >= g_term_profile_count) return -1;
    g_term_profile_active = v;
    term_profile_apply(&g_term_profiles[v]);
    // A live window also has to reflow: a profile carries a font size and a
    // scrollback depth, so the grid can change and every pane's child needs its
    // SIGWINCH. One reflow, every pane, exactly as tm_pick_profile() does it.
    if (g_tc_live) { term_menu_refresh(); term_layout_reflow(); }
    return 0;
}
static int ct_prof_count_get(void) { return g_term_profile_count; }
static int ct_prof_default_get(void) { return g_term_profile_default; }
static int ct_prof_default_set(int v) {
    if (v < 0 || v >= g_term_profile_count) return -1;
    g_term_profile_default = v;
    return 0;                       // persisted by the commit hook below
}
static int ct_prof_dir_get(char *out, int ocap) {
    const char *d = (g_term_profile_active >= 0 && g_term_profile_active < g_term_profile_count)
                    ? g_term_profiles[g_term_profile_active].start_dir : "";
    str_copy(out, d[0] ? d : "(none)", ocap);
    return 0;
}
static int ct_prof_cmd_get(char *out, int ocap) {
    const char *c = (g_term_profile_active >= 0 && g_term_profile_active < g_term_profile_count)
                    ? g_term_profiles[g_term_profile_active].start_cmd : "";
    str_copy(out, c[0] ? c : "(none)", ocap);
    return 0;
}

// ---------------------------------------------------------------------------
// The nine TERMNOTI.CFG keys. Nine getter/setter pairs, each one line, over
// term_notify.c's keyed accessors: the KEY INDEX is what binds a row to a
// variable, and term_notify.c locks those indices to its own table with a
// _Static_assert, so a row here cannot address the wrong knob.
// ---------------------------------------------------------------------------
#define TC_NCFG(sfx, key)                                                     \
    static int ct_n_##sfx##_get(void)      { return term_notify_cfg_get(key); } \
    static int ct_n_##sfx##_set(int v)     { return term_notify_cfg_set(key, v); }
TC_NCFG(enable,      TERM_NCFG_ENABLE)
TC_NCFG(finish_ms,   TERM_NCFG_FINISH_MS)
TC_NCFG(finish_un,   TERM_NCFG_FINISH_UNATTENDED_ONLY)
TC_NCFG(bell,        TERM_NCFG_BELL_NOTIFY)
TC_NCFG(vbell,       TERM_NCFG_VISUAL_BELL)
TC_NCFG(vbell_ms,    TERM_NCFG_VISUAL_BELL_MS)
TC_NCFG(activity,    TERM_NCFG_ACTIVITY)
TC_NCFG(silence,     TERM_NCFG_SILENCE)
TC_NCFG(silence_ms,  TERM_NCFG_SILENCE_MS)
#undef TC_NCFG

// ---------------------------------------------------------------------------
// Readable live state. These exist because an ACTION's success term has to name
// something a caller can go and LOOK AT (contract.h: "the return code is the one
// thing a broken action is most likely to get right"), and contract-lint check 6
// FAILS THE BUILD on a success_ref naming an item the app does not declare. Each
// one is therefore load-bearing, not decoration: delete a row here and the
// build fails on the action that cited it.
//
// All of them read 0 / "(none)" in a windowless contract process, which is the
// truth rather than a placeholder: there is no window, so there are no tabs, no
// panes, no selection and no shell.
// ---------------------------------------------------------------------------
static int ct_tab_count(void)   { return g_tc_live ? term_layout_tab_count() : 0; }
static int ct_pane_count(void)  { return g_tc_live ? term_layout_pane_count() : 0; }
static int ct_menu_visible(void){ return g_tc_live ? term_menu_bar_visible() : 0; }
static int ct_select_have(void) { return (g_tc_live && term_select_have()) ? 1 : 0; }
static int ct_sb_lines(void)    { return g_tc_live ? sb_count : 0; }
static int ct_find_open(void)   { return (g_tc_live && term_search_is_open()) ? 1 : 0; }
static int ct_find_matches(void){ return g_tc_live ? term_search_match_count() : 0; }
static int ct_bm_count(void)    { return term_menu_bookmark_count(); }
static int ct_cwd_get(char *out, int ocap) {
    str_copy(out, g_tc_live ? cwd : "(no live shell)", ocap);
    return 0;
}
// The four PER-TAB monitor switches. READ-ONLY on purpose: per-tab state only
// exists inside a live instance, and the writers are the four CT_TF_LIVE_ONLY
// monitor.toggle_* menu rows. A writable row here would be a value a windowless
// caller could set and nothing would ever read, which is the #235 fault.
static int ct_mon(int which) {
    if (!g_tc_live) return 0;
    return term_notify_get_monitor(term_layout_active_tab(), which) ? 1 : 0;
}
static int ct_mon_finish(void)   { return ct_mon(TERM_MON_FINISH); }
static int ct_mon_activity(void) { return ct_mon(TERM_MON_ACTIVITY); }
static int ct_mon_silence(void)  { return ct_mon(TERM_MON_SILENCE); }
static int ct_mon_bell(void)     { return ct_mon(TERM_MON_BELL); }

// ===========================================================================
// THE PROFILE STORE'S EDIT OPERATIONS, as a control table.
//
// These are OPERATIONS, not values: creating, duplicating, deleting and renaming
// a profile, and setting its starting directory and command, each take an
// argument or change the shape of the table, so none of them is expressible as a
// value row. They are the only actions in this app that a WINDOWLESS contract
// call can genuinely complete - the store is a file, term_prefs_load() has
// already read it by the time a verb runs, and the commit hook writes it back -
// so they are deliberately NOT marked CT_TF_LIVE_ONLY.
//
// NO ESCROW PROMISE, and the reason is worth stating rather than leaving as an
// omission. What these change is a single key=value config file this app owns
// (CT_DEPTH_PREF), not a filesystem tree, and the thing a caller should check is
// a contract item they can read back: profile.count for a create or a delete,
// profile (the active name) for a rename, profile.start_dir/_cmd for the two
// setters. A promise would add a second verifier for a postcondition the app can
// already answer exactly, and a promise nothing can falsify is worse than none -
// `promise=fulfilled paths=0/0` on an action that produced nothing is precisely
// the fault measured on golden 2509.
// ===========================================================================
static const ct_terms_t k_t_prof_add = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_PREF,
    CT_FAIL_NOSPACE | CT_FAIL_RANGE | CT_FAIL_IO, "profile.count", 0
};
static const ct_terms_t k_t_prof_del = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_PREF,
    CT_FAIL_RANGE | CT_FAIL_BUSY | CT_FAIL_IO, "profile.count", 0
};
static const ct_terms_t k_t_prof_name = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_PREF,
    CT_FAIL_RANGE | CT_FAIL_IO, "profile", 0
};
static const ct_terms_t k_t_prof_dir = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_PREF,
    CT_FAIL_RANGE | CT_FAIL_IO, "profile.start_dir", 0
};
static const ct_terms_t k_t_prof_cmd = {
    CT_TIME_FAST, 0, 0, CT_SUCC_ITEM, CT_DEPTH_PREF,
    CT_FAIL_RANGE | CT_FAIL_IO, "profile.start_cmd", 0
};

// Join argv back into one string, because a name, a path and a shell command can
// all contain spaces and `--contract call profile.rename Solarized work` arrives
// as two arguments. Same convention the shell builtins take (one raw remainder).
static void pa_join(char *out, int cap, int argc, char **argv) {
    int o = 0;
    for (int i = 0; i < argc && argv && argv[i]; i++) {
        if (o > 0 && o < cap - 1) out[o++] = ' ';
        for (const char *p = argv[i]; *p && o < cap - 1; p++) out[o++] = *p;
    }
    out[o] = 0;
}

// Persist, and REPORT A FAILED WRITE as a failed action. #743: a preference that
// silently fails to save is still a bug, and on this OS it fails for real (a
// session whose user has no /CONFIG/PASSWD entry gets home="/" and cannot write
// there). An action that reports success for a change that did not reach the disk
// is the return-code-versus-postcondition gap this whole terms mechanism exists
// to close.
static int pa_commit(char *out, int ocap, const char *what) {
    if (term_profiles_save() != 0) {
        snprintf(out, (size_t)ocap, "%s in memory but TERMPROF.CFG SAVE FAILED "
                 "(no writable per-user CONFIG?)", what);
        return -1;
    }
    term_menu_ct_invalidate();
    return 0;
}

static int pa_create(int argc, char **argv, char *out, int ocap) {
    char name[TERM_PROFILE_NAME_MAX * 2];
    pa_join(name, (int)sizeof(name), argc, argv);
    if (!name[0]) { snprintf(out, (size_t)ocap, "need-a-name"); return -1; }
    int i = term_profile_new(name, 0);
    if (i < 0) { snprintf(out, (size_t)ocap, "table-full max=%d", TERM_PROFILE_MAX); return -1; }
    if (pa_commit(out, ocap, "created") != 0) return -1;
    snprintf(out, (size_t)ocap, "created index=%d name=%s count=%d",
             i, g_term_profiles[i].name, g_term_profile_count);
    return 0;
}

static int pa_duplicate(int argc, char **argv, char *out, int ocap) {
    if (g_term_profile_active < 0 || g_term_profile_active >= g_term_profile_count) {
        snprintf(out, (size_t)ocap, "no-active-profile"); return -1;
    }
    char name[TERM_PROFILE_NAME_MAX * 2];
    pa_join(name, (int)sizeof(name), argc, argv);
    if (!name[0]) str_copy(name, g_term_profiles[g_term_profile_active].name, (int)sizeof(name));
    // term_profile_new() makes the name unique itself, so a bare duplicate of
    // "Default" becomes "Default 2" by the store's own rule rather than a second
    // copy of that rule here.
    int i = term_profile_new(name, &g_term_profiles[g_term_profile_active]);
    if (i < 0) { snprintf(out, (size_t)ocap, "table-full max=%d", TERM_PROFILE_MAX); return -1; }
    if (pa_commit(out, ocap, "duplicated") != 0) return -1;
    snprintf(out, (size_t)ocap, "duplicated index=%d name=%s count=%d",
             i, g_term_profiles[i].name, g_term_profile_count);
    return 0;
}

static int pa_delete(int argc, char **argv, char *out, int ocap) {
    int idx = (argc > 0 && argv && argv[0]) ? atoi(argv[0]) : g_term_profile_active;
    if (idx < 0 || idx >= g_term_profile_count) {
        snprintf(out, (size_t)ocap, "out-of-range 0..%d", g_term_profile_count - 1); return -1;
    }
    if (g_term_profile_count <= 1) {
        // The store refuses this, and the refusal is the right behaviour rather
        // than a limitation: an empty table would leave the next launch with no
        // profile to apply.
        snprintf(out, (size_t)ocap, "refused: the last remaining profile is not deletable");
        return -1;
    }
    char gone[TERM_PROFILE_NAME_MAX];
    str_copy(gone, g_term_profiles[idx].name, TERM_PROFILE_NAME_MAX);
    if (term_profile_delete(idx) < 0) { snprintf(out, (size_t)ocap, "refused index=%d", idx); return -1; }
    if (pa_commit(out, ocap, "deleted") != 0) return -1;
    snprintf(out, (size_t)ocap, "deleted name=%s count=%d active=%d default=%d",
             gone, g_term_profile_count, g_term_profile_active, g_term_profile_default);
    return 0;
}

static int pa_rename(int argc, char **argv, char *out, int ocap) {
    char name[TERM_PROFILE_NAME_MAX * 2];
    pa_join(name, (int)sizeof(name), argc, argv);
    if (!name[0]) { snprintf(out, (size_t)ocap, "need-a-name"); return -1; }
    if (term_profile_rename(g_term_profile_active, name) < 0) {
        snprintf(out, (size_t)ocap, "refused: no active profile, or an empty name"); return -1;
    }
    if (pa_commit(out, ocap, "renamed") != 0) return -1;
    snprintf(out, (size_t)ocap, "renamed index=%d name=%s",
             g_term_profile_active, g_term_profiles[g_term_profile_active].name);
    return 0;
}

static int pa_set_dir(int argc, char **argv, char *out, int ocap) {
    char dir[TERM_PROFILE_DIR_MAX * 2];
    pa_join(dir, (int)sizeof(dir), argc, argv);
    if (term_profile_set_start_dir(g_term_profile_active, dir) < 0) {
        snprintf(out, (size_t)ocap, "no-active-profile"); return -1;
    }
    if (pa_commit(out, ocap, "start_dir set") != 0) return -1;
    snprintf(out, (size_t)ocap, "start_dir=%s (empty = this session's home)",
             g_term_profiles[g_term_profile_active].start_dir);
    return 0;
}

static int pa_set_cmd(int argc, char **argv, char *out, int ocap) {
    char cmd[TERM_PROFILE_CMD_MAX * 2];
    pa_join(cmd, (int)sizeof(cmd), argc, argv);
    if (term_profile_set_start_cmd(g_term_profile_active, cmd) < 0) {
        snprintf(out, (size_t)ocap, "no-active-profile"); return -1;
    }
    if (pa_commit(out, ocap, "start_cmd set") != 0) return -1;
    snprintf(out, (size_t)ocap, "start_cmd=%s (empty = none)",
             g_term_profiles[g_term_profile_active].start_cmd);
    return 0;
}

typedef struct {
    const char *tok;
    const char *desc;
    int  (*fn)(int argc, char **argv, char *out, int ocap);
    const ct_terms_t *terms;
} tprof_act_t;

static const tprof_act_t PROF_ACTS[] = {
    { "profile.create",
      "Create a profile from the compiled defaults: profile.create <name>",
      pa_create,    &k_t_prof_add },
    { "profile.duplicate",
      "Copy the active profile under a new name (bare = \"<name> 2\")",
      pa_duplicate, &k_t_prof_add },
    { "profile.delete",
      "Delete profile <index>, or the active one; the last profile is refused",
      pa_delete,    &k_t_prof_del },
    { "profile.rename",
      "Rename the active profile: unique and '['/']'-free by the store's own rule",
      pa_rename,    &k_t_prof_name },
    { "profile.set_start_dir",
      "Set the directory new windows on this profile start in (empty = home)",
      pa_set_dir,   &k_t_prof_dir },
    { "profile.set_start_cmd",
      "Set the command this profile runs once at startup (empty = none)",
      pa_set_cmd,   &k_t_prof_cmd },
};
#define PROF_ACT_N ((int)(sizeof(PROF_ACTS) / sizeof(PROF_ACTS[0])))

static int ct_prof_act(const ct_item_t *it, int argc, char **argv, char *out, int ocap) {
    const tprof_act_t *a = (const tprof_act_t *)it->ctx;
    if (!a || !a->fn) return -1;
    return a->fn(argc, argv, out, ocap);
}
CT_CHECK_ACTFN(ct_prof_act);

// term_notify.c owns TERMNOTI.CFG, so the nine notify.* rows name ITS writer
// through ct_item_t.commitfn rather than leaning on the app-wide commit hook.
// Three config files and three writers; a row says which one persists it, so a
// theme write no longer rewrites the notification file and vice versa.
static void tc_commit_notify(void) { term_notify_cfg_save(); }

// ===========================================================================
// THE STATIC ROWS: values with a backing variable, and readable state.
//
// This is what static rows are FOR (contract.h: "the case where an app's control
// surface is NOT yet expressed as one table"), and it is legitimate here for a
// specific reason rather than by default: none of these is a row of a table the
// app draws and dispatches from. They are variables, and their timing, success
// and depth are DERIVED by contract_terms_of(), so they cannot carry a wrong
// term. Every ACTION is projected; nothing below is an action.
//
// INITIALISERS ARE C99-DESIGNATED, and that is a DEPARTURE from every other
// shipped ct_item_t table, which are positional. It is the right departure and
// the reason is measured rather than stylistic: ct_item_t has now been appended
// to twice (`terms`, then `commitfn`), and each time every positional table in
// the tree gained a "missing initializer for field" warning per row and a
// silent-field-shift hazard if a row spelled out its tail. contract.h's own note
// explains that `terms` had to be appended precisely BECAUSE the shipped tables
// are positional. A designated table is immune to both: the next appended field
// costs this file nothing. 36 rows written positionally produced 36 warnings
// under -Wextra; the same rows designated produce none.
//
// cfgkey is 0 on every row, and that is deliberate rather than an oversight: a
// cfgkey is what contract_save_cfg()/contract_load_cfg() persist through, and
// this app does NOT use them - TERMPROF.CFG is the source of truth and
// TERMPREF.CFG its mirror, both written by code that predates that helper and
// both hand-editable formats a generic "<key>=<int>" writer would destroy. The
// visible consequence is that contract_terms_of() derives depth VIEW rather than
// PREF for these rows; the honest fix is to move this app onto
// contract_save_cfg(), which is a separate change with its own file-format
// migration, not something to fake with a key nothing reads.
// ===========================================================================
static const ct_item_t TERMINAL_ITEMS[] = {
    // ---- appearance (MOVED from term_prefs.c; only the initialiser style
    // ---- changed, see the banner above) -----------------------------------
    { .name = "theme", .type = CT_ENUM, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 13, .options = TERM_CT_THEME_OPTS,
      .getfn = ct_term_theme_get, .setfn = ct_term_theme_set,
      .desc = "WINDOW THEME (chrome), independent of the OS system theme "
              "(default: dark). This is NOT the terminal's ANSI colour scheme - "
              "see colour_scheme below." },
    { .name = "colour_scheme", .type = CT_ENUM, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 7, .options = TERM_CT_PALETTE_OPTS,
      .getfn = ct_term_palette_get, .setfn = ct_term_palette_set,
      .desc = "COLOUR SCHEME: the 16 ANSI colours + default fg/bg/cursor/"
              "selection (Solarized/Tango/Campbell/...), in the Terminal.app/"
              "Konsole sense. 'system' (default) follows the theme item above "
              "instead of a named scheme. Every INSTALLED scheme, including one "
              "an App Store package adds, is reachable as a projected scheme.* "
              "action; this enum is the curated set for headless testing." },
    { .name = "font_family", .type = CT_ENUM, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 5, .options = TERM_CT_FONT_OPTS,
      .getfn = ct_term_font_get, .setfn = ct_term_font_set,
      .desc = "Terminal font family (cell width/height re-derived from its metrics)" },
    { .name = "font_style", .type = CT_ENUM, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 3, .options = TERM_CT_STYLE_OPTS,
      .getfn = ct_term_style_get, .setfn = ct_term_style_set,
      .desc = "Font style/weight, persisted as TERMPREF.CFG field 3 and as every "
              "profile's style= key, and until now the one appearance field with "
              "no contract row at all. Reads -1 when an installed face outside "
              "this curated list is in use; a write re-resolves the face through "
              "gui_font_resolve(), the same call the F9 picker makes." },
    { .name = "font_size", .type = CT_INT, .access = CT_RW, .risk = CT_SAFE,
      .lo = 8, .hi = 32, .var = &g_term_font.size,
      .desc = "Terminal font point size, 8-32" },
    { .name = "cursor_shape", .type = CT_ENUM, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 2, .options = TERM_CT_CURSOR_OPTS,
      .getfn = ct_term_cursor_get, .setfn = ct_term_cursor_set,
      .desc = "Cursor shape drawn in the cell grid: full block, baseline "
              "underline, or left-edge bar. Default underline, which is what it "
              "always was." },
    { .name = "cursor_blink", .type = CT_BOOL, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .var = &g_term_cursor_blink,
      .desc = "1 = the cursor blinks (~500ms), 0 = solid. The renderer honours "
              "this, so turning it off cannot desync from the event loop's blink "
              "counter." },
    { .name = "scrollback", .type = CT_INT, .access = CT_RW, .risk = CT_SAFE,
      .lo = 200, .hi = 20000,
      .getfn = ct_term_sb_get, .setfn = ct_term_sb_set,
      .desc = "Retained scrollback CAPACITY in lines. The ring is reallocated "
              "(newest lines kept) at the next redraw; the dialog offers "
              "500/1000/2000/5000/10000. scrollback.lines is how many are held." },

    // ---- profile: the store, and which one this window is running ---------
    { .name = "profile", .type = CT_STR, .access = CT_READ, .risk = CT_SAFE,
      .getstrfn = ct_term_profile_get,
      .desc = "Name of the PROFILE this terminal is running. Profiles are the "
              "source of truth; /CONFIG/TERMPREF.CFG is a mirror of live state." },
    { .name = "profile.index", .type = CT_INT, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = TERM_PROFILE_MAX - 1,
      .getfn = ct_prof_index_get, .setfn = ct_prof_index_set,
      .desc = "Which profile this window is running, as an index into the store. "
              "WRITABLE, which is the hole this closes: the read-only `profile` "
              "row above could report the profile but not switch it, and a "
              "profile is more than its appearance (it carries a starting "
              "directory and a start command). Refused at or past profile.count." },
    { .name = "profile.count", .type = CT_INT, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = TERM_PROFILE_MAX, .getfn = ct_prof_count_get,
      .desc = "How many profiles the store holds: the bound profile.index and "
              "profile.default accept, and what a create or a delete changes." },
    { .name = "profile.default", .type = CT_INT, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = TERM_PROFILE_MAX - 1,
      .getfn = ct_prof_default_get, .setfn = ct_prof_default_set,
      .desc = "Index of the profile NEW WINDOWS open with (the store's default= "
              "key). A terminal starts from this, not from the live mirror, which "
              "is what makes 'set as default' mean anything at the next launch." },
    { .name = "profile.start_dir", .type = CT_STR, .access = CT_READ, .risk = CT_SAFE,
      .getstrfn = ct_prof_dir_get,
      .desc = "The active profile's starting directory (dir= in TERMPROF.CFG), or "
              "(none) meaning this session's home. profile.set_start_dir writes it." },
    { .name = "profile.start_cmd", .type = CT_STR, .access = CT_READ, .risk = CT_SAFE,
      .getstrfn = ct_prof_cmd_get,
      .desc = "The command the active profile runs once at startup (cmd= in "
              "TERMPROF.CFG), or (none). profile.set_start_cmd writes it." },

    // ---- notifications: nine persisted, READ keys that had no control -----
    // Each one names term_notify.c's OWN writer through .commitfn, so setting a
    // notification key writes TERMNOTI.CFG and nothing else, and setting a theme
    // does not rewrite TERMNOTI.CFG. The pairing rule the app-wide commit hook
    // could not express: three config files, three writers, one row each.
    { .name = "notify.enable", .type = CT_BOOL, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_n_enable_get, .setfn = ct_n_enable_set,
      .commitfn = tc_commit_notify,
      .desc = "Master switch for terminal TOASTS (TERMNOTI.CFG enable=). The "
              "in-window attention pip and the visual bell are MARKS, not toasts, "
              "and are deliberately unaffected by it." },
    { .name = "notify.finish_ms", .type = CT_INT, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 600000, .getfn = ct_n_finish_ms_get, .setfn = ct_n_finish_ms_set,
      .commitfn = tc_commit_notify,
      .desc = "A command must run at least this many ms to be worth a finish "
              "toast; 0 = never toast on finish. Konsole's default is 10000." },
    { .name = "notify.finish_unattended_only", .type = CT_BOOL, .access = CT_RW,
      .risk = CT_SAFE, .lo = 0, .hi = 1,
      .getfn = ct_n_finish_un_get, .setfn = ct_n_finish_un_set,
      .commitfn = tc_commit_notify,
      .desc = "1 = only toast a finished command when the window is minimized, "
              "hidden or unfocused, because a user watching the pane can already "
              "see that it finished." },
    { .name = "notify.bell_notify", .type = CT_BOOL, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_n_bell_get, .setfn = ct_n_bell_set,
      .commitfn = tc_commit_notify,
      .desc = "1 = a BEL byte raises a system toast while the tab is unattended, "
              "rate limited so a program in a bell loop cannot become a toast loop." },
    { .name = "notify.visual_bell", .type = CT_BOOL, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_n_vbell_get, .setfn = ct_n_vbell_set,
      .commitfn = tc_commit_notify,
      .desc = "1 = a BEL flashes a frame just inside the pane edge, attended or "
              "not. This is chrome the terminal draws itself, not a notification." },
    { .name = "notify.visual_bell_ms", .type = CT_INT, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 5000, .getfn = ct_n_vbell_ms_get, .setfn = ct_n_vbell_ms_set,
      .commitfn = tc_commit_notify,
      .desc = "How long the visual-bell frame stays up, in ms; 0 disables it. "
              "Capped at 5000, because it still has to clear itself afterwards." },
    { .name = "notify.activity", .type = CT_BOOL, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_n_activity_get, .setfn = ct_n_activity_set,
      .commitfn = tc_commit_notify,
      .desc = "1 = toast when an unattended tab produces output (Konsole's "
              "Monitor for Activity, default OFF). A write also pushes it onto "
              "every open tab's switch, which is what makes it take effect." },
    { .name = "notify.silence", .type = CT_BOOL, .access = CT_RW, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_n_silence_get, .setfn = ct_n_silence_set,
      .commitfn = tc_commit_notify,
      .desc = "1 = toast when an unattended tab goes quiet for "
              "notify.silence_ms (Konsole's Monitor for Silence, default OFF)." },
    { .name = "notify.silence_ms", .type = CT_INT, .access = CT_RW, .risk = CT_SAFE,
      .lo = 1000, .hi = 600000,
      .getfn = ct_n_silence_ms_get, .setfn = ct_n_silence_ms_set,
      .commitfn = tc_commit_notify,
      .desc = "How long a tab must be quiet before the silence monitor fires, in "
              "ms. Floor 1000, because below that every prompt would qualify." },

    // ---- per-tab monitor overrides ----------------------------------------
    // READ-ONLY on purpose: per-tab state only exists inside a live instance, and
    // the writers are the four CT_TF_LIVE_ONLY monitor.toggle_* menu rows. A
    // writable row here would be a value a windowless caller could set and
    // nothing would ever read, which is the #235 fault.
    { .name = "monitor.finish", .type = CT_BOOL, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_mon_finish,
      .desc = "The ACTIVE TAB's notify-on-finish switch. The file key is the "
              "default and this is the override; monitor.toggle_finish writes it." },
    { .name = "monitor.activity", .type = CT_BOOL, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_mon_activity,
      .desc = "The ACTIVE TAB's monitor-for-activity switch, written by "
              "monitor.toggle_activity. Both this AND notify.activity must be on." },
    { .name = "monitor.silence", .type = CT_BOOL, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_mon_silence,
      .desc = "The ACTIVE TAB's monitor-for-silence switch, written by "
              "monitor.toggle_silence. Both this AND notify.silence must be on." },
    { .name = "monitor.bell", .type = CT_BOOL, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_mon_bell,
      .desc = "The ACTIVE TAB's bell-toast switch, written by monitor.toggle_bell. "
              "The visual bell is notify.visual_bell and is a separate decision." },

    // ---- readable live state: what an action's success term names ----------
    { .name = "window.tab_count", .type = CT_INT, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 64, .getfn = ct_tab_count,
      .desc = "Tabs open in this window: what tab.new and tab.close are checked "
              "against. 0 in a windowless contract process, because there is no "
              "window - which is the truth, not a placeholder." },
    { .name = "window.pane_count", .type = CT_INT, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 64, .getfn = ct_pane_count,
      .desc = "Panes in the active tab: what the five pane.* split commands are "
              "checked against. 0 in a windowless contract process." },
    { .name = "menu.visible", .type = CT_BOOL, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_menu_visible,
      .desc = "1 = the menu bar occupies the top 24px and every pane is that much "
              "shorter. view.show_menubar (F10) is its only writer." },
    { .name = "select.have", .type = CT_BOOL, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_select_have,
      .desc = "1 = a non-empty selection exists, which is what edit.copy needs "
              "and what edit.select_all is checked against." },
    { .name = "scrollback.lines", .type = CT_INT, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 20000, .getfn = ct_sb_lines,
      .desc = "Scrollback lines currently RETAINED, which is a different number "
              "from the `scrollback` capacity above. edit.clear_scrollback and the "
              "`clear` builtin drive it toward 0." },
    { .name = "find.bar_open", .type = CT_BOOL, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 1, .getfn = ct_find_open,
      .desc = "1 = the find-in-scrollback bar is up and owns the keyboard, which "
              "is what find.open is checked against." },
    { .name = "find.match_count", .type = CT_INT, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 1000, .getfn = ct_find_matches,
      .desc = "Matches the incremental find scan has found so far. find.next and "
              "find.previous are meaningless at 0, and their menu rows dim there." },
    { .name = "bookmarks.count", .type = CT_INT, .access = CT_READ, .risk = CT_SAFE,
      .lo = 0, .hi = 12, .getfn = ct_bm_count,
      .desc = "Directories in this user's TERMBOOK.CFG, each of which is also a "
              "projected bookmark.* action. bookmark.add and bookmark.remove "
              "change it, and a duplicate add leaves it alone." },
    { .name = "shell.cwd", .type = CT_STR, .access = CT_READ, .risk = CT_SAFE,
      .getstrfn = ct_cwd_get,
      .desc = "The built-in shell's working directory in THIS window, which drives "
              "the prompt, pwd and every relative path typed at it. A windowless "
              "contract process has no shell, so it answers (no live shell) rather "
              "than inventing a path." },
};

// ===========================================================================
// THE PROJECTION. Four chained families, in a fixed order so an index is stable
// within one process.
// ===========================================================================
static int term_contract_project(int idx, ct_item_t *out) {
    if (idx < 0) return 0;

    int n = term_menu_ct_count();
    if (idx < n) return term_menu_ct_project(idx, out);
    idx -= n;

    n = term_shell_ct_count();
    if (idx < n) return term_shell_ct_project(idx, out);
    idx -= n;

    if (idx < PROF_ACT_N) {
        const tprof_act_t *a = &PROF_ACTS[idx];
        ct_item_t it;
        __builtin_memset(&it, 0, sizeof(it));
        it.name = a->tok; it.desc = a->desc; it.terms = a->terms;
        it.type = CT_ACTION; it.access = CT_WRITE;
        it.actfn = ct_prof_act; it.ctx = a;
        term_ct_apply_risk(&it, a->terms);
        *out = it; return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// load / commit.
//
// THE LIVE GUARD ON load() IS LOAD-BEARING, and it is the one thing that had to
// change for live delivery to be safe. contract_run_verb() calls c->load() on
// EVERY verb, including a read-only `probe`. term_prefs_load() re-reads
// TERMPREF.CFG *and TERMPROF.CFG* and then re-applies the DEFAULT PROFILE to
// live state - which is exactly right for a fresh windowless process and would
// silently reset an open window's theme, font, cursor and scrollback on every
// contract call, including one that only asked a question. A live instance IS the
// truth; there is nothing for it to load.
// ---------------------------------------------------------------------------
static void term_contract_load(void) {
    if (g_tc_live) return;
    term_prefs_load();        // TERMPREF.CFG + TERMPROF.CFG + the live appearance
    term_notify_cfg_load();   // TERMNOTI.CFG, without touching the window
}

// After a var-backed write (font_size) contract.c does not know to recompute
// metrics or persist; after a getfn/setfn write the setter updated live state but
// did not persist either. commit() is the ONE place both paths funnel through
// (contract.c calls it after every successful set AND call), so it is the single
// spot that always re-derives cell metrics and saves, regardless of which row
// changed.
static void term_contract_commit(void) {
    term_apply_font();
    // Profiles are the source of truth (term_profile.h), so a contract write has
    // to land THERE as well as in the mirror, or `terminal --contract set
    // colour_scheme=tango` would be undone by the next launch reading the default
    // profile back. capture() copies only the live APPEARANCE fields, leaving the
    // profile's name, starting directory and command alone - which is what lets
    // the profile.set_start_dir/_cmd and profile.rename actions above survive the
    // commit that follows them.
    if (g_term_profile_active >= 0 && g_term_profile_active < g_term_profile_count) {
        term_profile_capture(&g_term_profiles[g_term_profile_active]);
        term_profiles_save();
    }
    term_prefs_save();
    // TERMNOTI.CFG is deliberately NOT written here. It is the third config file
    // this app owns and it has its own writer, so the nine notify.* rows name
    // that writer through ct_item_t.commitfn instead: setting a theme should not
    // rewrite the notification file, and setting a notification key should not
    // rewrite the profile store. One app-wide commit cannot express that; a
    // per-row writer can.
}

const ct_contract_t TERMINAL_CONTRACT = {
    "terminal", "Terminal",
    "PROJECTED from the tables this app draws and dispatches from: every shell "
    "builtin (g_builtins[], the same table `help` describes), every menu-bar "
    "command (TM_ACTS[], the same table a click dispatches through), the live "
    "colour-scheme, profile and bookmark lists, and the profile store's own edit "
    "operations. A control absent from those tables can be neither run nor "
    "described. The static rows are VALUES and READABLE STATE only, whose terms "
    "are derived. theme (window chrome) and colour_scheme (the 16 ANSI colours) "
    "are TWO DIFFERENT THINGS, both independent of the OS system theme. Almost "
    "every projected action is CT_TF_LIVE_ONLY because it acts on a window's "
    "tabs, panes, grid or chrome: deliver those to a RUNNING Terminal, which "
    "main.c polls for. NOT covered, because they are gestures rather than "
    "controls: the pointer drag that resizes a split or extends a selection, "
    "hover highlighting, wheel scrolling, the scrollbar pill, and the find bar's "
    "own text field, whose only actions (find.next/find.previous) ARE described.",
    TERMINAL_ITEMS, (int)(sizeof(TERMINAL_ITEMS) / sizeof(TERMINAL_ITEMS[0])),
    term_contract_project,
    term_contract_load,
    term_contract_commit
};
