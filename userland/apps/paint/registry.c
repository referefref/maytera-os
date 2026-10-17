// registry.c - Maytera Studio op registry + drawable helpers (integrator-owned).
// Filters/adjustments/generators self-register a studio_op_t; ui.c builds the
// Colors/Filters menus + a generic parameter dialog from the registry.
#include "studio.h"

#define STUDIO_MAX_OPS 320
static const studio_op_t *g_ops[STUDIO_MAX_OPS];
static int g_nops = 0;

void studio_register(const studio_op_t *op) {
    if (op && g_nops < STUDIO_MAX_OPS) g_ops[g_nops++] = op;
}
int studio_op_count(void) { return g_nops; }
const studio_op_t *studio_op_get(int i) {
    if (i < 0 || i >= g_nops) return (const studio_op_t *)0;
    return g_ops[i];
}

// Active drawable = the active layer's ARGB pixel buffer. Op/tool authors that
// support mask editing may special-case g_doc.layer[active].mask_active.
// Preview-target override. When set (by the in-dialog filter preview pane in
// ui.c), an op's apply() renders into this small scratch buffer instead of the
// active layer, so a filter can be previewed cheaply without touching the doc.
// Mirrors the mask-vs-layer selection idea: one global the drawable accessors
// honour. The caller disables the selection so draw_cov() reports full coverage.
static uint32_t *g_prev_px = 0;
static int       g_prev_w = 0, g_prev_h = 0;
void draw_preview_target_set(uint32_t *px, int w, int h){ g_prev_px = px; g_prev_w = w; g_prev_h = h; }
void draw_preview_target_clear(void){ g_prev_px = 0; g_prev_w = 0; g_prev_h = 0; }

uint32_t *draw_px(void) {
    if (g_prev_px) return g_prev_px;
    if (g_doc.active < 0 || g_doc.active >= g_doc.nlayers) return (uint32_t *)0;
    return g_doc.layer[g_doc.active].px;
}
int draw_w(void) { return g_prev_px ? g_prev_w : g_doc.w; }
int draw_h(void) { return g_prev_px ? g_prev_h : g_doc.h; }

// Called once from main() after doc_new(). Each registrar adds its ops.
//
// IDEMPOTENT (#<config-ref>). It used to be called from exactly one place
// (ui_splash step 4), so "called once" was true by inspection. The contract
// projection now needs the registry populated on the SPAWNED --contract path
// too, which returns from main() before the splash ever runs, so
// PAINT_CONTRACT.load() calls this as well. studio_register() appends without
// dedup, so a second call would have listed all 300-odd ops twice and the
// projection would have declared every filter twice. Guarding here rather than
// at each caller keeps it one fact in one place.
static int g_registered = 0;
void studio_register_all(void) {
    if (g_registered) return;
    g_registered = 1;
    color_register_all();
    fx_blur_register_all();
    fx_enhance_register_all();
    fx_edge_register_all();
    fx_generic_register_all();
    fx_distort_register_all();
    fx_light_register_all();
    fx_noise_register_all();
    fx_decor_register_all();
    fx_map_register_all();
    fx_artistic_register_all();
    fx_combine_register_all();
    render_register_all();
    fx_brushstrokes_register_all();
    fx_sketch_register_all();
    fx_stylize_register_all();
    fx_texture_register_all();
    fx_pixelate_register_all();
}

// ---------------------------------------------------------------------------
// The ONE non-interactive apply path (#<config-ref>).
//
// ui.c's run_op() either applies a zero-parameter op immediately or OPENS THE
// PARAMETER DIALOG and waits for a person. A contract call has no person, so it
// needs the apply without the dialog, with the parameters supplied. This is the
// same three steps run_op() and pd_commit() both perform (undo_push, apply,
// mark dirty), in one place, so a headless call and a dialog OK cannot diverge
// into two different notions of "applying a filter".
//
// Parameters not supplied take the op's declared default, and each supplied one
// is CLAMPED to the op's own declared [min,max]: p[] indexes per-pixel loops and
// lookup tables, and an out-of-range value from a caller is INPUT, exactly as
// contract_load_cfg() treats a config file as input.
//
// HONEST LIMIT, and the projection says so per row: SP_CURVE and SP_KERNEL
// parameters are ui.c-owned interactive state (the tone-curve editor and the 5x5
// convolution grid). A headless call gets whatever those editors currently hold,
// which in a freshly spawned process is their reset default. Such ops are
// projected with CT_TF_LIVE_ONLY for that reason.
int studio_op_apply_params(int i, const int *vals, int nvals) {
    const studio_op_t *op = studio_op_get(i);
    if (!op || !op->apply) return -1;
    if (g_doc.nlayers <= 0) return -2;          // no drawable: a spawned copy
    int p[STUDIO_MAX_PARAMS];
    int np = op->nparams; if (np > STUDIO_MAX_PARAMS) np = STUDIO_MAX_PARAMS;
    for (int k = 0; k < np; k++) {
        int v = (vals && k < nvals) ? vals[k] : op->params[k].def;
        if (op->params[k].type != SP_COLOR) {   // SP_COLOR's def is 0xRRGGBB, not a range
            if (v < op->params[k].min) v = op->params[k].min;
            if (v > op->params[k].max) v = op->params[k].max;
        }
        p[k] = v;
    }
    undo_push(op->name);
    int rc = op->apply(np > 0 ? p : (const int *)0);
    g_doc.comp_dirty = 1;
    g_doc.modified = 1;
    return rc;
}
