// studio.h - Maytera Studio shared contract. Every module includes ONLY this
// header plus the libc headers it needs. Do not invent cross-module APIs that
// are not declared here; if you must deviate, add a "// CONTRACT-DEVIATION:"
// comment and keep the declared signatures working.
//
// Modules (one owner each):
//   doc.c     - document/layer model, blending, compositing, undo
//   select.c  - selection masks (rect/ellipse/lasso/wand)
//   tools.c   - paint tools operating on the active layer through the mask
//   filters.c - adjustments + convolution filters (integer/fixed-point)
//   imgio.c   - BMP load/save, native layered .MSTU, PNG export
//   ai.c      - native LLM integration (Kimi over sys_http_post)
//   ui.c      - all panels/menus/canvas view/event handling
//   main.c    - window + event loop wiring only
#ifndef STUDIO_H
#define STUDIO_H

#include "../../libc/maytera.h"

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------
#define STUDIO_MAX_LAYERS 12
#define STUDIO_MAX_UNDO   4          // full-doc snapshots; memory-guarded
#define STUDIO_MAX_W      1600
#define STUDIO_MAX_H      1200
#define STUDIO_DEF_W      960
#define STUDIO_DEF_H      600
#define STUDIO_NAME_LEN   24
#define STUDIO_PATH_LEN   128

// Pixels are non-premultiplied ARGB8888. A = coverage/alpha of that layer's
// paint (base layer is created opaque). NO large static arrays anywhere:
// every pixel buffer is malloc'd (user.ld base 0x80000000 breaks big .bss,
// see blame #444).

// ---------------------------------------------------------------------------
// Document / layers (doc.c)
// ---------------------------------------------------------------------------
// GIMP 2.10/3 blend-mode set. The first 6 keep their v1 values (do NOT reorder;
// files reference them). doc.c blend_px() implements ALL of them; ui shows names
// via blend_name(). LCh modes may approximate in integer space.
typedef enum {
    BLEND_NORMAL = 0, BLEND_MULTIPLY, BLEND_SCREEN, BLEND_OVERLAY,
    BLEND_ADD, BLEND_SUBTRACT,
    BLEND_DISSOLVE, BLEND_LIGHTEN, BLEND_DARKEN, BLEND_DODGE, BLEND_BURN,
    BLEND_SOFTLIGHT, BLEND_HARDLIGHT, BLEND_VIVIDLIGHT, BLEND_PINLIGHT,
    BLEND_LINEARLIGHT, BLEND_HARDMIX, BLEND_DIFFERENCE, BLEND_EXCLUSION,
    BLEND_GRAINEXTRACT, BLEND_GRAINMERGE, BLEND_DIVIDE,
    BLEND_HUE, BLEND_SATURATION, BLEND_COLOR, BLEND_VALUE,
    BLEND_LUMA_LIGHTEN, BLEND_LUMA_DARKEN,
    BLEND_COUNT
} blend_t;
const char *blend_name(blend_t b);

typedef struct {
    uint32_t *px;                 // malloc'd doc.w*doc.h ARGB
    int       opacity;            // 0..255
    int       visible;            // 0/1
    blend_t   blend;
    char      name[STUDIO_NAME_LEN];
    // GIMP-parity additions (doc.c owns lifecycle; NULL/0 defaults keep v1 behavior)
    uint8_t  *mask;               // optional layer mask, grayscale w*h, or NULL
    int       mask_active;        // 1 = paint/filters target the mask, not pixels
    int       lock_alpha;         // 1 = preserve existing alpha when painting
    int       group;              // group id, or -1 for none
} layer_t;

typedef struct {
    int      w, h;
    layer_t  layer[STUDIO_MAX_LAYERS];
    int      nlayers;
    int      active;              // index of active layer
    uint8_t *sel;                 // malloc'd w*h selection mask 0..255, or NULL
    int      sel_active;          // 1 while a selection exists
    uint32_t *comp;               // malloc'd w*h composited view (opaque ARGB)
    int      comp_dirty;
    char     path[STUDIO_PATH_LEN];
    int      modified;
} doc_t;

extern doc_t g_doc;

int  doc_new(int w, int h, uint32_t bg);      // frees old, 1 opaque layer "Background"
void doc_free(void);
int  layer_add(const char *name, uint32_t fill);   // insert ABOVE active, ret idx or -1
int  layer_dup(int idx);                            // ret new idx or -1
int  layer_del(int idx);                            // refuses if nlayers==1
int  layer_move(int idx, int dir);                  // dir: +1 = up in z, -1 = down
int  layer_merge_down(int idx);                     // blend idx onto idx-1
void doc_flatten(void);
void doc_composite(void);                           // rebuilds g_doc.comp if comp_dirty
// One pixel of `src` (with its alpha) over `dst` using mode + layer opacity 0..255.
uint32_t blend_px(uint32_t dst, uint32_t src, blend_t mode, int opacity);

// Undo: full-document snapshots (layers + topology). If malloc fails, silently
// drop the oldest snapshot and retry once; never crash.
void        undo_push(const char *label);           // call BEFORE mutating
int         undo_undo(void);                        // 1 if applied
int         undo_redo(void);
int         undo_count(void);
const char *undo_label(int i);                      // 0..undo_count()-1, newest last
int         undo_can_undo(void);                    // 1 if an undo is available
int         undo_can_redo(void);                    // 1 if a redo is available
const char *undo_next_label(void);                  // label of the action a Ctrl+Z would undo
const char *redo_next_label(void);                  // label of the action a Ctrl+Y would redo

// ---------------------------------------------------------------------------
// Selection (select.c). No selection == everything selected.
// ---------------------------------------------------------------------------
void sel_clear(void);
void sel_all(void);
void sel_invert(void);
void sel_rect(int x0, int y0, int x1, int y1, int feather);
void sel_ellipse(int x0, int y0, int x1, int y1, int feather);
void sel_lasso_begin(int x, int y);
void sel_lasso_point(int x, int y);
void sel_lasso_end(void);                           // closes + fills polygon
void sel_wand(int x, int y, int tolerance);         // flood select from comp
static inline int sel_at(int x, int y) {
    if (!g_doc.sel_active || !g_doc.sel) return 255;
    if (x < 0 || y < 0 || x >= g_doc.w || y >= g_doc.h) return 0;
    return g_doc.sel[y * g_doc.w + x];
}

// ---------------------------------------------------------------------------
// Tools (tools.c)
// ---------------------------------------------------------------------------
typedef enum {
    TL_BRUSH = 0, TL_PENCIL, TL_ERASER, TL_AIRBRUSH, TL_CLONE, TL_SMUDGE,
    TL_BLUR, TL_FILL, TL_GRADIENT, TL_LINE, TL_RECT, TL_ELLIPSE,
    TL_TEXT, TL_MOVE, TL_PICK,
    TL_SEL_RECT, TL_SEL_ELLIPSE, TL_SEL_LASSO, TL_SEL_WAND,
    // GIMP-parity additions (append only; do NOT reorder the above)
    TL_HEAL, TL_DODGE, TL_BURN, TL_SHARPEN, TL_INK, TL_SEL_BYCOLOR,
    TL_CROP, TL_MEASURE, TL_PATH,
    TL_COUNT
} tool_id_t;

typedef struct {
    tool_id_t id;
    uint32_t  fg, bg;
    int       size;               // 1..64
    int       opacity;            // 0..255
    int       hardness;           // 0..255 soft-edge falloff
    int       wand_tolerance;     // 0..255
    int       flow;               // 0..255 paint flow
    int       sel_mode;           // 0 new,1 add,2 subtract,3 intersect
    int       feather;            // selection feather radius (px)
    int       grad_blend;         // blend_t for the gradient tool (default BLEND_NORMAL)
    char      text[64];           // TL_TEXT string
    // TL_TEXT typography (TrueType via the OS font registry)
    int       text_font;          // installed face index (0 = default UI font)
    int       text_bold;          // 0/1
    int       text_italic;        // 0/1
    int       text_underline;     // 0/1
} toolstate_t;

extern toolstate_t g_tool;

// Canvas-space coordinates. begin/drag/end fully own stroke lifecycle,
// including undo_push at begin and comp_dirty marking. Shape tools (line/
// rect/ellipse/gradient) commit on end; ui.c draws the rubber-band preview.
void tool_begin(int x, int y);
void tool_drag(int x, int y);
void tool_end(int x, int y);
const char *tool_name(tool_id_t t);

// ---------------------------------------------------------------------------
// Filters / adjustments (filters.c). Apply to ACTIVE layer clipped by the
// selection mask. Caller (ui.c/ai.c) does undo_push first. Integer math only
// in per-pixel loops. Returns 0 ok.
// p1/p2/p3 meaning per filter, all -255..255 unless noted:
//   F_BRIGHTNESS p1=amount           F_CONTRAST  p1=amount
//   F_HUESAT     p1=hue(-180..180) p2=sat p3=lightness
//   F_LEVELS     p1=black(0..254) p2=white(1..255) p3=gamma_x100(10..300)
//   F_BLUR       p1=radius(1..16)    F_SHARPEN  p1=amount(0..255)
//   F_THRESHOLD  p1=level(0..255)    F_POSTERIZE p1=levels(2..16)
//   F_NOISE      p1=amount(0..255)
// ---------------------------------------------------------------------------
typedef enum {
    F_BRIGHTNESS = 0, F_CONTRAST, F_HUESAT, F_LEVELS, F_INVERT, F_GRAYSCALE,
    F_SEPIA, F_BLUR, F_SHARPEN, F_EDGE, F_EMBOSS, F_THRESHOLD, F_POSTERIZE,
    F_NOISE, F_COUNT
} filter_id_t;

int         filter_apply(filter_id_t f, int p1, int p2, int p3);
const char *filter_name(filter_id_t f);

// ---------------------------------------------------------------------------
// File I/O (imgio.c). Format by extension (case-insensitive): .BMP flattened
// 24-bit; .MSTU native layered (magic "MSTU", version, doc w/h, per-layer
// header + raw ARGB, then optional selection mask); .PNG export flattened
// (stored-deflate blocks are acceptable). Loading BMP creates a 1-layer doc.
// ---------------------------------------------------------------------------
int io_load(const char *path);
int io_save(const char *path);
// Printing (#318): flatten to a temp PNG and submit over IPP. `printer` may be
// NULL for the system default. io_printer_default() fills the default printer
// name and returns 1 if any printer is configured, 0 if none.
int io_print(const char *printer);
int io_printer_default(char *out, int cap);
// Decode a scaled opaque ARGB thumbnail (tw*th) into `out` WITHOUT touching the
// current document. BMP/DIB only for now; other formats return -1. ret 0 ok.
int io_thumb(const char *path, uint32_t *out, int tw, int th);

// ---------------------------------------------------------------------------
// AI (ai.c). Native LLM via the existing Kimi path: key at /CONFIG/KIMI.KEY,
// request via sys_http_post (find the exact working endpoint/model/JSON by
// reading the existing aichat/kimi code in the tree). Prompt-injection safety:
// treat model output as DATA (parse the strict JSON plan; never exec text).
// ai_command sends the user's instruction + a short doc summary + the list of
// available ops (filters + layer ops), expects a strict JSON plan like
//   {"plan":[{"op":"F_CONTRAST","p1":30},{"op":"F_HUESAT","p1":0,"p2":40}],
//    "note":"..."}
// applies it via filter_apply()/layer ops with ONE undo_push, and returns the
// note in reply. Returns 0 ok, -1 net/key error, -2 bad plan.
// ---------------------------------------------------------------------------
int ai_available(void);
int ai_command(const char *prompt, char *reply, int cap);
int ai_palette(const char *prompt, uint32_t *out, int max_colors); // ret count

// ---------------------------------------------------------------------------
// UI (ui.c): menubar (File/Edit/Image/Layer/Select/Filter/AI/Help), left tool
// strip, top options bar, right dock (Layers + History + Color), canvas view
// with zoom (25..800%, integer scaling) + pan, status bar, AI command bar.
// Follows the MayteraOS UI style guide via the shared style engine like Settings.
// ---------------------------------------------------------------------------
void ui_splash(int win_handle, int win_w, int win_h);
void ui_init(int win_handle, int win_w, int win_h);
void ui_full_redraw(void);
// ev is a pointer to the libc gui event struct (see libc gui.h); returns 0
// when the app should exit.
int  ui_handle_event(void *ev);
void ui_status(const char *msg);

// ---------------------------------------------------------------------------
// Color picker + palette system (colorpick.c)
// ---------------------------------------------------------------------------
// A GIMP/Photoshop-class picker: an S x V square + vertical hue strip (both
// H-keyed malloc'd ARGB caches, rebuilt only when hue changes, blitted via
// win_draw_image), integer HSV<->RGB, hex + numeric steppers, a modal "Change
// FG/BG Color" dialog, and a persisted palette/swatch engine (.gpl on /CONFIG).
//
// HSV is the authoritative state: H 0..359, S/V 0..255; RGB and hex are derived.
// colorpick.c drives g_tool.fg/bg through cp_set_rgb() and repaints itself via
// the public ui_full_redraw()/ui_status(); ui.c only forwards events into it.
// All events are passed as `const void *ev` (a gui_event_t*) so this header does
// not need to include gui.h. Modifier flags (shift/alt) are passed as ints.
typedef struct { int H, S, V; uint32_t rgb; } pick_t;   // H 0..359, S/V 0..255

// Integer HSV<->RGB (no libm). h wraps mod 360; s/v/r/g/b are 0..255.
void cp_hsv2rgb(int h, int s, int v, int *r, int *g, int *b);
void cp_rgb2hsv(int r, int g, int b, int *h, int *s, int *v);

// Lifecycle. cp_init receives the window handle and loads /CONFIG/PALETTE.GPL.
void cp_init(int win);
// Commit funnel: set the ACTIVE target (FG or BG) from an RGB and push it to the
// recent-colors ring. Swap / default-colors / AI-palette / hex all route here so
// picker state stays consistent (fixes the "BG is second-class" gap).
void cp_set_rgb(uint32_t rgb);
void cp_push_recent(uint32_t rgb);

// Dock mini-picker (rewritten SEC_COLOR body). y0 = section content anchor.
void cp_draw_dock(int dx, int y0);
int  cp_dock_height(void);                 // pixels the section body occupies
int  cp_click_dock(int dx, int y0, int mx, int my, int shift, int alt);  // 1 if consumed
int  cp_drag_dock(int mx, int my);         // continue an SV/hue/stepper drag; 1 if active
void cp_dock_release(void);                // end any dock drag (commit to recent)
int  cp_key_dock(const void *ev);          // hex-field key; 1 if the field consumed it

// Palettes / swatches dock section (new SEC_SWATCHES body).
void cp_draw_pal(int dx, int y0);
int  cp_pal_height(void);
// Returns 0 = not consumed, 1 = consumed, 2 = caller should open AI-palette flow.
int  cp_click_pal(int dx, int y0, int mx, int my, int shift, int alt);
// Load N colors into a new editable palette named `name`, make it active, and
// set FG to the first color. Used by the AI-palette hook.
void cp_load_ai_palette(const char *name, const uint32_t *cols, int n);

// Modal "Change Foreground/Background Color" dialog (520x360, self-centering).
void cp_open_modal(int editing_bg);        // 0 = edit FG, 1 = edit BG
// Open the modal to edit a standalone value (SP_COLOR): on OK writes (*dest) and
// invokes on_ok() if non-NULL; on Cancel leaves *dest untouched.
void cp_open_modal_for_value(uint32_t initial, int *dest, void (*on_ok)(void));
int  cp_modal_open(void);
void cp_draw_modal(void);
int  cp_click_modal(int mx, int my);       // 1 if consumed
int  cp_drag_modal(int mx, int my);        // 1 if a modal drag is active
void cp_modal_release(void);
int  cp_key_modal(const void *ev);         // 1 if consumed (Enter=OK, Esc=Cancel)

// ---------------------------------------------------------------------------
// Shared small helpers (header-only)
// ---------------------------------------------------------------------------
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint32_t argb(int a, int r, int g, int b) {
    return ((uint32_t)(a & 255) << 24) | ((uint32_t)(r & 255) << 16) |
           ((uint32_t)(g & 255) << 8) | (uint32_t)(b & 255);
}
static inline int px_a(uint32_t p) { return (int)(p >> 24) & 255; }
static inline int px_r(uint32_t p) { return (int)(p >> 16) & 255; }
static inline int px_g(uint32_t p) { return (int)(p >> 8) & 255; }
static inline int px_b(uint32_t p) { return (int)p & 255; }

// ===========================================================================
// GIMP 3 PARITY EXPANSION (see the internal Studio GIMP 3 parity notes)
// ===========================================================================

// --- Op registry -----------------------------------------------------------
// Every adjustment/filter/generator registers a studio_op_t. ui.c builds the
// Colors + Filters menus from the registry and renders a generic parameter
// dialog from params[]. The registry lives in registry.c (integrator-owned).
// An op's apply(p) operates on the ACTIVE DRAWABLE = active layer pixels (or
// its mask if layer.mask_active) clipped by the selection (use draw_cov()),
// preserves alpha for colour-only ops, sets g_doc.comp_dirty=1 and
// g_doc.modified=1, and returns 0 on success. undo_push() is done by ui.c
// BEFORE calling apply. Integer/fixed-point only in per-pixel loops.
// Phase 1 (task #538) added SP_ANGLE (a circular degree dial, 0..360) and
// SP_CURVE (an interactive 256-entry tone-curve editor whose state is owned by
// ui.c, mirroring how colorpick.c owns the SP_COLOR modal state). Phase 2 added
// SP_KERNEL (an editable 5x5 signed convolution grid, ui.c-owned like SP_CURVE,
// read by ops via kernel_get25()). Append only; files reference these by value.
typedef enum { SP_SLIDER, SP_INT, SP_ENUM, SP_COLOR, SP_CHECK, SP_ANGLE, SP_CURVE, SP_KERNEL } sparam_type_t;
// Max parameters per op. Was a hard-coded 6; the flagship Lighting Effects op
// needs more, so it is a named constant used by params[], pd_val[] and the
// Colors builder. Keep pd_val[] in ui.c sized to match.
#define STUDIO_MAX_PARAMS 14
typedef struct {
    sparam_type_t type;
    const char   *name;
    int           min, max, def;   // SP_COLOR: def is 0xRRGGBB; SP_CHECK: 0/1
    const char   *choices;         // SP_ENUM only: "Linear|Radial|Zoom"
} sparam_t;
typedef struct studio_op {
    const char *menu;              // "Colors","Blur","Distorts","Artistic",...
    const char *name;              // "Gaussian Blur"
    int         nparams;
    sparam_t    params[STUDIO_MAX_PARAMS];
    int       (*apply)(const int *p);
    // On-canvas positional handles (Phase 1). An op that wants the user to set a
    // position by dragging a marker over the canvas during preview flags one or
    // two param-index PAIRS here. Each pair (handle_xp, handle_yp) names two int
    // params holding a position normalized to 0..1000 of the drawable w/h. 0 =
    // no handles; existing ops leave these zero (no handle at param 0/0 unless
    // nhandles>0). ui.c draws + drags the markers generically.
    int         nhandles;          // 0..2
    int         handle_xp[2];      // param index of the X coord (0..1000)
    int         handle_yp[2];      // param index of the Y coord (0..1000)
} studio_op_t;
void               studio_register(const studio_op_t *op);  // op must be static/persistent
int                studio_op_count(void);
const studio_op_t *studio_op_get(int i);
void               studio_register_all(void);   // registry.c: calls every *_register_all()

// SP_CURVE bridge: the interactive tone-curve editor lives in ui.c and owns the
// control-point state (like colorpick owns the SP_COLOR modal). An op with an
// SP_CURVE param reads the composited per-channel 256-entry LUTs the user drew
// via this accessor; each of lr/lg/lb receives 256 entries. Returns the identity
// mapping when no curve has been edited. Defined in ui.c.
void curve_get_lut3(int *lr, int *lg, int *lb);

// SP_KERNEL bridge: the editable 5x5 convolution-matrix grid lives in ui.c and
// owns the cell state (SP_CURVE pattern). An op with an SP_KERNEL param treats
// that p[] slot as a placeholder and reads the 25 signed cells (-999..999,
// row-major) via kernel_get25(). kernel_reset() restores the default sharpen
// matrix (center 5, orthogonals -1), never all-zeros; the dialog Reset button
// and dialog open both invoke it. Defined in ui.c.
void kernel_get25(int *k25);
void kernel_reset(void);

// Drawable helpers for op/tool authors (registry.c / doc.c provide these).
uint32_t *draw_px(void);           // active drawable pixels (layer px; ARGB). mask editing writes
                                   // gray into RGB with A=255 - authors may special-case mask_active.
int       draw_w(void);            // == g_doc.w
int       draw_h(void);            // == g_doc.h
// Preview-target override (ui.c filter preview pane): while set, draw_px/_w/_h
// resolve to a small scratch buffer instead of the active layer so an op's
// apply() can render a cheap in-dialog preview. Caller disables the selection
// for full coverage; clear when done. NULL/0,0 = normal active-layer behaviour.
void      draw_preview_target_set(uint32_t *px, int w, int h);
void      draw_preview_target_clear(void);
static inline int draw_cov(int x, int y) { return sel_at(x, y); }  // 0..255 selection coverage

// Each category file implements ONE of these; main init calls them all.
void color_register_all(void);
void fx_blur_register_all(void);
void fx_enhance_register_all(void);
void fx_edge_register_all(void);
void fx_generic_register_all(void);
void fx_distort_register_all(void);
void fx_light_register_all(void);
void fx_noise_register_all(void);
void fx_decor_register_all(void);
void fx_map_register_all(void);
void fx_artistic_register_all(void);
void fx_combine_register_all(void);
void render_register_all(void);
void fx_brushstrokes_register_all(void);
void fx_sketch_register_all(void);
void fx_stylize_register_all(void);
void fx_texture_register_all(void);
void fx_pixelate_register_all(void);

// --- Layers: masks / groups / channels / alpha (doc.c) ---------------------
int  layer_mask_add(int idx, int init);   // init: 0 white(opaque),1 black,2 from alpha,3 from grayscale
int  layer_mask_apply(int idx);           // bake mask into alpha, remove mask
int  layer_mask_delete(int idx);          // discard mask
int  layer_set_blend(int idx, blend_t b);
int  layer_set_opacity(int idx, int op);
int  layer_add_alpha(int idx);            // ensure alpha channel present
int  layer_flatten_alpha(int idx, uint32_t bg);  // remove alpha, composite over bg
#define STUDIO_MAX_CHANNELS 8
int  channel_save_selection(const char *name);   // ret channel idx or -1
int  channel_load_selection(int idx);            // set selection from saved channel
int  channel_count(void);
const char *channel_name(int idx);

// --- Selection ops (select.c) ---------------------------------------------
void sel_grow(int px);
void sel_shrink(int px);
void sel_feather(int radius);
void sel_border(int px);
void sel_by_color(int x, int y, int tolerance);  // whole-image, not flood
void sel_round(int radius);

// --- Transforms on the active layer (tools.c) ------------------------------
int  layer_flip(int idx, int horizontal);        // horizontal!=0 -> mirror L/R else T/B
int  layer_rotate90(int idx, int cw);
int  layer_rotate_arbitrary(int idx, int degrees);   // integer degrees, bilinear
int  layer_scale(int idx, int new_w, int new_h);
int  layer_shear(int idx, int shear_x_pct, int shear_y_pct);
int  doc_crop_to_selection(void);
int  doc_resize(int w, int h, int anchor);       // anchor 0..8 (like GIMP)

// --- Paths (tools.c): a single working bezier path -------------------------
void path_reset(void);
void path_add_node(int x, int y, int in_dx, int in_dy, int out_dx, int out_dy);
void path_close(void);
void path_stroke(int width, uint32_t color);
void path_to_selection(int feather);
int  path_node_count(void);

// --- Histogram (doc.c): 256-bin, channel 0=luminance,1=R,2=G,3=B -----------
void histogram(int channel, unsigned int out[256]);

#endif // STUDIO_H
