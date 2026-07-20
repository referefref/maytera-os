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
void studio_register_all(void) {
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
