// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// contract.c - Maytera Studio's tool contract (#233 surface, tier-2 wire).
//   docs/AI_ACTION_CAPABILITY_BINDING.md sections 1.7, 3.3, 5.2.
//
// Studio already parses model output as strict JSON DATA and applies it through
// its own primitives (ai.c parse_plan/apply_step) - the safe act-half tier 2 is
// built on. This file EXPOSES a few of those primitives as contract actions the
// OS AI can invoke on the LIVE canvas: the same layer_add()/filter_apply()/
// undo_push() the menus call, run against the live g_doc.
//
// TWO GATES, by the design of CONTRACT_API.md section 11 and this doc's 4.4:
//   - `cap` (aicap id) + `risk` bound what the AI may do THROUGH the contract
//     API, checked in-process by contract_authorize()->aicap_authorize(). This
//     is the demonstrable red/green: a CT_GUARDED edit is refused with no grant
//     and allowed with one. It is exactly the tier-2-cooperative gate, because a
//     cooperative edit reaches NO gated syscall (its `needs` list is empty).
//   - `needs` is the KERNEL capability an action's own code reaches. screen_check
//     calls sys_screenshot_request(), which the kernel gates at syscall_cap_check
//     regardless of anything here: THAT is the real teeth (4.4). Its `needs`
//     entry declares the alignment so the tier-2 union (3.4) is machine-readable.
//
// A stateless SPAWN of Studio (contract_invoke fallback) has NO document, so the
// edit actions guard on g_doc.nlayers and answer "no-live-document" - which is
// precisely why the LIVE path (contract_live_poll) exists.

#include "studio.h"
#include "../../libc/contract.h"
#include "../../libc/syscall.h"
#include "../../libc/userconf.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"

// ---- read-only live state (serial-verifiable without a screendump) ---------
static int cg_width(void)    { return g_doc.w; }
static int cg_height(void)   { return g_doc.h; }
static int cg_layers(void)   { return g_doc.nlayers; }
static int cg_active(void)   { return g_doc.active; }
static int cg_modified(void) { return g_doc.modified ? 1 : 0; }
// #<config-ref>: the document's own path, as a readable item.
//
// ADDED BECAUSE contract-lint CAUGHT ITS ABSENCE. k_t_screen_check below named
// "doc.path" as the thing a caller should check, and no such item existed: the
// #235 fault (a detailed declaration pointing at nothing) reduced to one field,
// found by check 6 on its first run over this very commit. g_doc.path has always
// been there; only the row was missing. It is also what libc/ctpromise.c's "doc"
// path template resolves through, so declaring it makes that template work for
// Studio instead of silently failing to resolve.
static int cg_path(char *out, int ocap) {
    strlcpy(out, g_doc.path[0] ? g_doc.path : "(unsaved)", (size_t)ocap);
    return 0;
}

static int have_doc(char *out, int ocap) {
    if (g_doc.nlayers > 0) return 1;
    strlcpy(out, "no-live-document (spawned copy has no canvas; a running Studio is required)", (size_t)ocap);
    return 0;
}

// ---- actions on the LIVE document ------------------------------------------
static int ca_add_layer(const ct_item_t *it, int argc, char **argv,
                        char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!have_doc(out, ocap)) return CT_ERR_FAILED;
    int idx = layer_add("AI layer", 0);
    if (idx < 0) { strlcpy(out, "layer-add-failed (max layers?)", (size_t)ocap); return CT_ERR_FAILED; }
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    ui_status("MayterAI: added a layer");
    ui_full_redraw();
    snprintf(out, ocap, "added-layer index=%d layers=%d", idx, g_doc.nlayers);
    return 0;
}
CT_CHECK_ACTFN(ca_add_layer);

static int ca_invert(const ct_item_t *it, int argc, char **argv,
                     char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!have_doc(out, ocap)) return CT_ERR_FAILED;
    undo_push("AI invert");
    filter_apply(F_INVERT, 0, 0, 0);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    ui_status("MayterAI: inverted the active layer");
    ui_full_redraw();
    strlcpy(out, "inverted-active-layer", (size_t)ocap);
    return 0;
}
CT_CHECK_ACTFN(ca_invert);

static int ca_grayscale(const ct_item_t *it, int argc, char **argv,
                        char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    if (!have_doc(out, ocap)) return CT_ERR_FAILED;
    undo_push("AI grayscale");
    filter_apply(F_GRAYSCALE, 0, 0, 0);
    g_doc.comp_dirty = 1; g_doc.modified = 1;
    ui_status("MayterAI: grayscaled the active layer");
    ui_full_redraw();
    strlcpy(out, "grayscaled-active-layer", (size_t)ocap);
    return 0;
}
CT_CHECK_ACTFN(ca_grayscale);

// PERCEIVE action: capture the live canvas. The KERNEL gates
// sys_screenshot_request via screen.capture (CAP_SCREEN_CAPTURE); without a
// grant it returns CAP_EDENIED, and no manifest field here can change that -
// this is where the kernel teeth (4.4) bite for a cooperative app.
static int ca_screen_check(const ct_item_t *it, int argc, char **argv,
                           char *out, int ocap) {
    (void)it; (void)argc; (void)argv;
    char path[160];
    if (userhome_path(0, "STUDIOAI.BMP", path, sizeof(path)) != 0)
        strlcpy(path, "/HOME/STUDIOAI.BMP", sizeof(path));
    long r = sys_screenshot_request(path);
    if (r == (long)CAP_EDENIED) {
        snprintf(out, ocap,
                 "capability-denied: the kernel refused screen.capture (no grant); path=%s", path);
        return CT_ERR_FAILED;
    }
    if (r != 0) { snprintf(out, ocap, "capture-failed rc=%ld path=%s", r, path); return CT_ERR_FAILED; }
    snprintf(out, ocap, "capture-queued path=%s", path);
    return 0;
}
CT_CHECK_ACTFN(ca_screen_check);

// ---- the needs bindings (tier 2, 3.3/3.4) ----------------------------------
// Edit actions reach no gated syscall: their needs list is empty. screen_check
// reaches the kernel screen.capture gate on the current-document scope.
static const ct_cap_need_t k_needs_none[]    = { { 0, 0, 0 } };
static const ct_cap_need_t k_needs_capture[]  = {
    { (unsigned char)CAP_SCREEN_CAPTURE, (unsigned char)CAP_SCOPE_PATH, "doc" },
    { 0, 0, 0 }
};

// ---- the contract ----------------------------------------------------------
// Static items[] (Studio's control surface is not yet one table). Rows here are
// the AI-drivable actions + read-only state, NOT the whole 200-op menu system;
// that broader coverage is future work, honestly bounded, not claimed complete.

// ---- the TERMS for the four non-projected actions (#<config-ref>) ----------
// An ACTION's timing, success and depth are not derivable from its row (a value
// row's are: see contract_terms_of), so every action declares them or
// contract-lint fails the build. Each of these is a statement about the code
// directly below it, not an aspiration.
static const ct_terms_t k_t_add_layer = {
    CT_TIME_FAST, 0, CT_TIME_MS_FAST,
    CT_SUCC_ITEM, CT_DEPTH_DOC,
    CT_FAIL_NEEDSLIVE | CT_FAIL_NOSPACE,
    "doc.layers",     // one more than before: a fact the caller can read back
    0
};
static const ct_terms_t k_t_grayscale = {
    CT_TIME_SLOW, 0, CT_TIME_MS_SLOW,       // a per-pixel pass over the layer
    CT_SUCC_ITEM, CT_DEPTH_DOC,
    CT_FAIL_NEEDSLIVE,
    "doc.modified",
    0
};
static const ct_terms_t k_t_invert = {
    CT_TIME_SLOW, 0, CT_TIME_MS_SLOW,
    CT_SUCC_ITEM, CT_DEPTH_DOC,
    CT_FAIL_NEEDSLIVE | CT_FAIL_DENIED,
    "doc.modified",
    0
};
// ARMED, not done: sys_screenshot_request() queues a capture the compositor
// performs later, so this call cannot report the file. Saying CT_SUCC_ARMED is
// the honest answer; saying CT_SUCC_PATH would be a claim about a file that does
// not exist yet when the call returns.
static const ct_terms_t k_t_screen_check = {
    CT_TIME_FAST, CT_TF_ASYNC, CT_TIME_MS_FAST,
    CT_SUCC_ARMED, CT_DEPTH_FS,
    CT_FAIL_DENIED | CT_FAIL_IO,
    "doc.path",
    0
};

static const ct_item_t PAINT_ITEMS[] = {
    // read-only live state
    {
      .name = "doc.width",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 100000,
      .getfn = cg_width,
      .desc = "active document width in pixels",
      .needs = k_needs_none,
    },
    {
      .name = "doc.height",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 100000,
      .getfn = cg_height,
      .desc = "active document height in pixels",
      .needs = k_needs_none,
    },
    {
      .name = "doc.layers",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 64,
      .getfn = cg_layers,
      .desc = "number of layers in the live document",
      .needs = k_needs_none,
    },
    {
      .name = "doc.active_layer",
      .type = CT_INT,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 64,
      .getfn = cg_active,
      .desc = "index of the active layer",
      .needs = k_needs_none,
    },
    {
      .name = "doc.modified",
      .type = CT_BOOL,
      .access = CT_READ,
      .risk = CT_SAFE,
      .hi = 1,
      .getfn = cg_modified,
      .desc = "1 if the document has unsaved changes",
      .needs = k_needs_none,
    },
    {
      .name = "doc.path",
      .type = CT_STR,
      .access = CT_READ,
      .risk = CT_SAFE,
      .getstrfn = cg_path,
      .desc = "Path of the open document, or (unsaved)",
      .needs = k_needs_none,
    },

    // SAFE actions (positive control: work with no grant, verifiable via
    // doc.layers over serial without a screendump)
    {
      .name = "add_layer",
      .type = CT_ACTION,
      .access = CT_WRITE,
      .risk = CT_SAFE,
      .actfn = ca_add_layer,
      .desc = "add a new empty layer above the active one",
      .needs = k_needs_none,
      .terms = &k_t_add_layer,
      .commitfn = 0 /*commitfn*/,
    },
    {
      .name = "grayscale",
      .type = CT_ACTION,
      .access = CT_WRITE,
      .risk = CT_SAFE,
      .actfn = ca_grayscale,
      .desc = "desaturate the active layer to grayscale",
      .needs = k_needs_none,
      .terms = &k_t_grayscale,
      .commitfn = 0 /*commitfn*/,
    },

    // GUARDED edit action (the red/green: refused with no aicap grant, allowed
    // with one). cap app.paint.edit is the aicap id; needs is empty because the
    // app performs it through its own filter_apply(), reaching no gated syscall.
    {
      .name = "invert",
      .type = CT_ACTION,
      .access = CT_WRITE,
      .risk = CT_GUARDED,
      .actfn = ca_invert,
      .cap = "app.paint.edit",
      .desc = "invert the colors of the active layer",
      .needs = k_needs_none,
      .terms = &k_t_invert,
      .commitfn = 0 /*commitfn*/,
    },

    // GUARDED perceive action: its teeth are the KERNEL screen.capture gate,
    // declared in needs. aicap id app.paint.observe bounds the AI half.
    {
      .name = "screen_check",
      .type = CT_ACTION,
      .access = CT_WRITE,
      .risk = CT_GUARDED,
      .actfn = ca_screen_check,
      .cap = "app.paint.observe",
      .desc = "capture the live canvas for the AI to inspect",
      .needs = k_needs_capture,
      .terms = &k_t_screen_check,
      .commitfn = 0 /*commitfn*/,
    },
};


// ===========================================================================
// THE PROJECTION (#<config-ref>, task 1 of docs/CONTRACT_COVERAGE_PLAN.md).
//
// WHY THIS EXISTS AND WHAT IT REPLACES. This file used to declare `.project = 0`
// and a note saying the static rows were "NOT the whole 200-op menu system;
// that broader coverage is future work". That was honest and it was also the
// entire coverage problem: Studio's Colors and Filters menus are built by ui.c
// walking studio_op_count()/studio_op_get(), a RUNTIME REGISTRY of every
// adjustment, filter and generator every fx_*.c file registers. An op that is
// not in that registry cannot appear in a menu and cannot be applied. So the
// registry IS the app's structural enumeration of its filter surface, and
// projecting from it makes completeness hold BY CONSTRUCTION: adding a filter
// gives it a contract action in the same commit, and there is no second list to
// forget. That is the property a hand-written row cannot have (#235: 120 of 150
// hand-written capability tokens matched no code).
//
// The static rows above stay static and that is correct: doc.width, add_layer
// and screen_check are not menu entries and there is no table they could be
// projected from. tools/contract-lint/PROJECTION.tsv records that judgement per
// app so it has to be justified rather than assumed.
//
// NAMING. "fx.<menu>.<op name>", lowercased, every run of non-alphanumeric
// characters collapsed to one underscore: "Filters/Blur/Gaussian Blur" becomes
// fx.blur.gaussian_blur. Derived from the op's own two strings, so a renamed
// menu entry renames its contract action too, which is the point.
// ===========================================================================

// Normalise one label into a dotted-name component, in place into out[].
//
// THE ocap <= 0 GUARD IS NOT DEFENSIVE NOISE. Without it, a caller that had
// already filled the buffer would pass ocap 0 and the unconditional `out[o] = 0`
// below writes ONE BYTE PAST THE ARRAY. It is not reachable with today's menu
// and op names (the longest is about 30 of the 64 available), which is precisely
// the shape of the bug that lands the day somebody adds a longer filter name.
// Same class as the (ocap - o) truncation fault in libc/contract.c's describe
// appender, found in the same pass.
static int ct_slug(const char *s, char *out, int ocap) {
    if (ocap <= 0) return 0;
    int o = 0, prev_us = 1;
    for (int i = 0; s && s[i] && o < ocap - 1; i++) {
        char ch = s[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        int alnum = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
        if (alnum) { out[o++] = ch; prev_us = 0; }
        else if (!prev_us) { out[o++] = '_'; prev_us = 1; }
    }
    while (o > 0 && out[o - 1] == '_') o--;
    out[o] = 0;
    return o;
}

// Per-op scratch the projected row points at. A projected row must own its
// strings for the life of the call (contract.h), and contract_at() hands the
// caller one row at a time, so one buffer set is enough and is what the shared
// projection in libc expects.
static char  g_fx_name[CT_NAME_MAX];
static char  g_fx_desc[CT_DESC_MAX];
static char  g_fx_opts[CT_OPTS_MAX];
static ct_terms_t g_fx_terms;

// Every projected filter op's action. The op index travels in it->ctx, so one
// actfn serves all of them and there is no index-to-function translation table
// to drift (contract.h's note on ctx).
static int ca_fx_apply(const ct_item_t *it, int argc, char **argv,
                       char *out, int ocap) {
    if (!have_doc(out, ocap)) return CT_ERR_FAILED;
    int idx = (int)(long)it->ctx;
    const studio_op_t *op = studio_op_get(idx);
    if (!op) { strlcpy(out, "no-such-op", (size_t)ocap); return CT_ERR_FAILED; }

    // Positional arguments map onto the op's own declared params, in the order
    // the parameter dialog shows them. Anything not supplied takes the declared
    // default; studio_op_apply_params() clamps each to the op's declared range.
    int vals[STUDIO_MAX_PARAMS]; int nv = 0;
    for (int i = 0; i < argc && nv < STUDIO_MAX_PARAMS; i++) {
        const char *a = argv[i]; if (!a) break;
        int neg = 0, v = 0, any = 0;
        if (*a == '-') { neg = 1; a++; }
        while (*a >= '0' && *a <= '9') { v = v * 10 + (*a++ - '0'); any = 1; }
        if (!any) break;
        vals[nv++] = neg ? -v : v;
    }
    int rc = studio_op_apply_params(idx, vals, nv);
    if (rc == -2) { strlcpy(out, "no-live-document", (size_t)ocap); return CT_ERR_FAILED; }
    if (rc != 0)  { snprintf(out, (size_t)ocap, "apply-failed rc=%d op=%s", rc, op->name); return CT_ERR_FAILED; }
    ui_full_redraw();
    snprintf(out, (size_t)ocap, "applied=%s params=%d modified=%d",
             op->name, nv, g_doc.modified ? 1 : 0);
    return 0;
}
CT_CHECK_ACTFN(ca_fx_apply);

#define PAINT_NSTATIC ((int)(sizeof(PAINT_ITEMS) / sizeof(PAINT_ITEMS[0])))

// The needs list for a filter op: it reaches NO gated syscall. It mutates the
// app's own in-memory layer through filter code compiled into this binary, so
// its kernel-capability list is empty, and that emptiness is the tier-2
// cooperative case made machine-readable (see the header note on `needs`).
static const ct_cap_need_t k_needs_fx[] = { { 0, 0, 0 } };

static int paint_project(int idx, ct_item_t *out) {
    // The projection covers the REGISTRY only. The static rows are served by
    // contract_at() from items[]/n before it reaches here, so idx here is
    // registry-relative.
    const studio_op_t *op = studio_op_get(idx);
    if (!op) return 0;

    // Built with the REMAINING room at every step, never with a width that could
    // go negative. ct_slug returns what it actually wrote; the two literal "."
    // and "fx." writes are checked against the cap rather than assumed to fit.
    const int ncap = (int)sizeof(g_fx_name);
    int o = 0;
    g_fx_name[0] = 0;
    if (ncap > 4) { g_fx_name[o++] = 'f'; g_fx_name[o++] = 'x'; g_fx_name[o++] = '.'; g_fx_name[o] = 0; }
    o += ct_slug(op->menu, g_fx_name + o, ncap - o);
    if (o < ncap - 1) { g_fx_name[o++] = '.'; g_fx_name[o] = 0; }
    o += ct_slug(op->name, g_fx_name + o, ncap - o);
    (void)o;

    // The parameter spec, from the op's OWN sparam_t table: name(min..max,def).
    // This is what a caller needs to pass arguments, and it is derived, so a
    // parameter that is added, removed or re-ranged updates it in the same edit.
    int po = 0; g_fx_opts[0] = 0;
    int ui_owned = 0;
    int np = op->nparams; if (np > STUDIO_MAX_PARAMS) np = STUDIO_MAX_PARAMS;
    for (int k = 0; k < np && po < (int)sizeof(g_fx_opts) - 24; k++) {
        const sparam_t *sp = &op->params[k];
        if (sp->type == SP_CURVE || sp->type == SP_KERNEL) ui_owned = 1;
        po += snprintf(g_fx_opts + po, sizeof(g_fx_opts) - po, "%s%s(%d..%d,%d)",
                       po ? "|" : "", sp->name ? sp->name : "p", sp->min, sp->max, sp->def);
    }

    snprintf(g_fx_desc, sizeof(g_fx_desc),
             "%s / %s: apply this %s to the active layer%s",
             op->menu ? op->menu : "Filters", op->name ? op->name : "?",
             np > 0 ? "parameterised filter" : "filter",
             ui_owned ? " (its curve/matrix editor state is ui.c-owned: a"
                        " headless call gets the reset default)" : "");

    // ---- THE TERMS, DERIVED FROM THE OP, not written out per filter.
    // timing: every op is a per-pixel loop over the whole drawable in
    // fixed-point integer arithmetic, so SLOW (<= 5 s) is the honest class for
    // all of them; it is not INSTANT and it is not unbounded.
    // success: the caller checks doc.modified, an item this contract already
    // declares, rather than trusting the return code.
    // depth: the open document. Nothing is persisted; the user still has to save.
    __builtin_memset(&g_fx_terms, 0, sizeof(g_fx_terms));
    g_fx_terms.timing      = CT_TIME_SLOW;
    g_fx_terms.budget_ms   = CT_TIME_MS_SLOW;
    g_fx_terms.tflags      = (unsigned char)(ui_owned ? CT_TF_LIVE_ONLY : 0);
    g_fx_terms.success     = CT_SUCC_ITEM;
    g_fx_terms.success_ref = "doc.modified";
    g_fx_terms.depth       = CT_DEPTH_DOC;
    g_fx_terms.failures    = CT_FAIL_NEEDSLIVE | CT_FAIL_RANGE | CT_FAIL_DENIED;
    g_fx_terms.promise     = 0;   // it changes pixels in RAM; there is no path
                                  // postcondition to promise, and inventing one
                                  // would be the fiction this ticket removes.

    ct_item_t it;
    __builtin_memset(&it, 0, sizeof(it));
    it.name   = g_fx_name;
    it.type   = CT_ACTION;
    it.access = CT_WRITE;
    // An image edit is not cosmetic and not reversible by the caller, so it
    // takes the SAME aicap id the hand-written invert row already takes rather
    // than a new name for the same authority.
    it.risk   = CT_GUARDED;
    it.cap    = "app.paint.edit";
    it.actfn  = ca_fx_apply;
    it.ctx    = (const void *)(long)idx;
    it.options = g_fx_opts[0] ? g_fx_opts : 0;
    it.desc   = g_fx_desc;
    it.needs  = k_needs_fx;
    it.terms  = &g_fx_terms;
    *out = it;
    return 1;
}

// PAINT_CONTRACT.load(). The registry is populated by ui_splash() on the normal
// launch path, which a spawned `--contract` invocation returns from main()
// before ever reaching. Without this the projection would enumerate ZERO ops on
// the spawn path and the contract would silently describe 9 items instead of
// 300-odd: the "zero callers means the feature never runs" trap, in the one
// place it would look like a working feature. studio_register_all() is
// idempotent (registry.c), so calling it here AND from the splash is safe.
static void paint_contract_load(void) { studio_register_all(); }

const ct_contract_t PAINT_CONTRACT = {
    .app   = "paint",
    .title = "Maytera Studio",
    .note  = "Every Colors/Filters op is PROJECTED from the studio op registry "
             "(studio_op_count/studio_op_get), the same registry ui.c builds "
             "those menus from, so a filter that is not in it cannot be drawn, "
             "cannot be applied and cannot be described. The static rows are the "
             "live-state readouts and the three non-menu actions, which have no "
             "table to project from (tools/contract-lint/PROJECTION.tsv). Edits "
             "are CT_GUARDED (aicap app.paint.edit); screen_check is "
             "kernel-gated on screen.capture. A SPAWNED copy has no document, so "
             "every edit answers no-live-document: use the live path.",
    .items = PAINT_ITEMS,
    .n     = (int)(sizeof(PAINT_ITEMS) / sizeof(PAINT_ITEMS[0])),
    .project = paint_project,
    .load  = paint_contract_load,  // populate the op registry; NOT a doc reload
    .commit = 0,    // each action repaints itself via ui_full_redraw()
};
