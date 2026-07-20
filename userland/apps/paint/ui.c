// ui.c - Maytera Studio UI: menubar, tool strip, options bar, right dock
// (Layers / Channels / Paths / Histogram / History / Color), zoomable+pannable
// canvas with rulers/grid/guides, registry-driven Colors + Filters menus with a
// generic live-preview parameter dialog, blend-mode picker, layer masks, modal
// text/message boxes, and the AI command bar. All app logic lives here; main.c
// is only the window + event loop. Uses the module APIs declared in studio.h.
#include "studio.h"
#include "brushes.h"        // brush/pattern/gradient engine (SEC_BRUSHES/SEC_PATTERNS + gradient options)
#include "../../libc/gui.h"
#include "../../libc/dirent.h"

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------
#define MENU_H    22
#define OPT_H     30
#define STRIP_W   46
#define DOCK_W    240
#define STATUS_H  22
#define RULER     14        // top ruler HEIGHT
#define RULER_Y   26        // left ruler WIDTH (wider so 3-4 digit labels fit)

static int   g_win = -1;
static int   g_w = 1180, g_h = 740;

// Canvas viewport (screen space) - the top ruler eats RULER px off the top and
// the left ruler eats RULER_Y px off the left.
static int cv_x(void){ return STRIP_W + RULER_Y; }
static int cv_y(void){ return MENU_H + OPT_H + RULER; }
static int cv_w(void){ int v = g_w - STRIP_W - RULER_Y - DOCK_W; return v < 16 ? 16 : v; }
static int cv_h(void){ int v = g_h - MENU_H - OPT_H - RULER - STATUS_H; return v < 16 ? 16 : v; }

// ---------------------------------------------------------------------------
// Palette (studio chrome). The chrome colors track the shared OS style engine
// (gui_pal()) via theme_sync(), so Studio matches the Settings/Files theme. The
// literals below are the dark-studio fallback used before theme_sync() runs.
// ---------------------------------------------------------------------------
static uint32_t TH_BG=0x00202225, TH_PANEL=0x002b2e33, TH_PANEL2=0x0025282c,
                TH_BTN=0x003a3e44, TH_BTN_HOV=0x004a4f57,
                TH_ACCENT=0x00337ab7, TH_ACCENT2=0x0055a0e0,
                TH_LINE=0x00141618, TH_TEXT=0x00e8e8e8, TH_DIM=0x009aa0a8,
                TH_FIELD=0x0025282c, TH_FIELD_BORDER=0x00141618;
#define C_BG      TH_BG
#define C_PANEL   TH_PANEL
#define C_PANEL2  TH_PANEL2
#define C_BTN     TH_BTN
#define C_BTN_HOV TH_BTN_HOV
#define C_ACCENT  TH_ACCENT
#define C_ACCENT2 TH_ACCENT2
#define C_LINE    TH_LINE
#define C_TEXT    TH_TEXT
#define C_DIM     TH_DIM
#define C_FIELD        TH_FIELD
#define C_FIELD_BORDER TH_FIELD_BORDER
#define C_CANVASBG 0x00181a1c
// Pull the chrome palette from the shared OS style engine so Studio follows the
// active Settings/Files theme. Derives the extra button/panel tints the flat OS
// palette doesn't carry from surface_raised, keeping the beveled studio look.
static void theme_sync(void){
    gui_palette_t *p = gui_pal();
    if(!p) return;
    TH_BG           = p->surface;
    TH_PANEL        = p->surface_raised;
    TH_PANEL2       = gui_darken(p->surface_raised, 14);
    TH_BTN          = gui_lighten(p->surface_raised, 22);
    TH_BTN_HOV      = gui_lighten(p->surface_raised, 44);
    TH_ACCENT       = p->accent;
    TH_ACCENT2      = p->accent_hover;
    TH_LINE         = p->border;
    TH_TEXT         = p->ink;
    TH_DIM          = p->ink_dim;
    TH_FIELD        = p->field_bg;
    TH_FIELD_BORDER = p->field_border;
}
// Canvas sheet framing (item 2)
#define C_PASTEBOARD  0x00161719   // flat pasteboard behind the document sheet
#define C_SHEETBORDER 0x00565e66   // 1px sheet edge
#define C_CK_LT       0x00c8c8c8   // transparency checker light
#define C_CK_DK       0x00909090   // transparency checker dark

// ---------------------------------------------------------------------------
// View state
// ---------------------------------------------------------------------------
// Continuous zoom percentage (item 2: granular Lightroom/Photoshop-style zoom).
// zoom() is a plain percent used everywhere as *100/z and *z/100, so ANY value
// works, not just powers of two. Keyboard/scroll/steppers snap to "nice" stops;
// the status-bar slider drags anywhere in between.
#define ZOOM_MIN 5
#define ZOOM_MAX 800
static int g_zoom_pct = 100;
static int pan_x = 0, pan_y = 0;
static int zoom(void){ return g_zoom_pct; }
static const int ZSTOPS[] = { 5,10,15,25,33,50,67,75,100,125,150,200,300,400,600,800 };
#define NZSTOP ((int)(sizeof(ZSTOPS)/sizeof(ZSTOPS[0])))

static int grid_on = 0, grid_sp = 32;
#define MAX_GUIDES 32
static int guide_v[MAX_GUIDES], nguide_v = 0;   // vertical guides at canvas-x
static int guide_h[MAX_GUIDES], nguide_h = 0;   // horizontal guides at canvas-y
static int dragging_guide = 0;                  // 1=new vertical, 2=new horizontal
static int drag_guide_pos = 0;

static uint32_t *g_blit = 0;
static int g_blit_cap = 0;

// Interaction
static int hover_tool = -1;
static int painting = 0;
static int shaping = 0;
static int sh_x0, sh_y0, sh_x1, sh_y1;
static int space_down = 0, panning = 0, pan_mx = 0, pan_my = 0;
// TL_TEXT typing: click the canvas to place a caret, type, Enter/click commits.
static int text_typing = 0;
// #542 shared text-field for the Text tool: caret + selection + system-wide
// clipboard (Ctrl+A/C/X/V) + per-field undo/redo (Ctrl+Z/Y), bound to g_tool.text.
static textfield_t g_text_tf;
static int text_ax, text_ay;   // canvas-space anchor (top-left) of the text
static char g_status[96] = "Ready";
// Status-bar zones (item 3): coord/color read-out is separate from the action
// message so mouse hover never overwrites the left zone.
static int g_cur_valid=0, g_cur_x=0, g_cur_y=0; static uint32_t g_cur_col=0;
static int g_zoom_bx=0,g_zoom_by=0,g_zoom_bw=0,g_zoom_bh=0;   // zoom % read-out box
// Granular zoom cluster geometry (item 2), recorded in draw_status() and hit-
// tested in click_status(); the slider is dragged in EVENT_MOUSE_MOVE.
static int zc_minus_x=0,zc_plus_x=0,zc_slider_x=0,zc_slider_w=0,zc_fit_x=0,zc_100_x=0;
static int zc_y=0,zc_h=0,zc_step_w=0,zc_btn_w=0;
static int zoom_slider_drag=0;
static int g_selbb_on=0,g_selbb_w=0,g_selbb_h=0,g_selbb_dirty=1; // cached selection bbox

static int menu_open = -1;      // top menubar menu index
static int submenu_cat = -1;    // when Filters menu open: which category submenu

// Blend-mode popup (from the Layers panel)
static int blend_pop = 0;
static int blend_pop_x = 0, blend_pop_y = 0;

// Right-dock scroll offset (dock content can exceed height)
static int dock_scroll = 0;

// Global mouse state - drives immediate-mode hover/press feedback for buttons.
static int g_mx = -1, g_my = -1, g_mdown = 0;
// Keyboard modifier state (declared early so dock/palette click handlers can read
// Shift/Alt for BG-set and delete-swatch, mirroring GIMP conventions).
static int mod_shift=0, mod_alt=0;

// Dock: collapsible sections, scroll metrics, layer-thumbnail + histogram cache.
enum { SEC_LAYERS=0, SEC_CHANNELS, SEC_PATHS, SEC_HIST, SEC_HISTORY, SEC_COLOR, SEC_SWATCHES,
       SEC_BRUSHES, SEC_PATTERNS, SEC_N };
// Brushes/Patterns default collapsed (1): each is a tall 6-column grid of all
// stock assets, so leaving them open would swamp the dock on first launch.
static int SEC_COL[SEC_N] = {0,0,0,0,0,0,0,1,1};   // 1 = collapsed
static int sec_hdr_y[SEC_N];
static const char *SEC_NAME[SEC_N] = {"Layers","Channels","Paths","Histogram","History","Color","Swatches","Brushes","Patterns"};
static int dock_content_h = 0, dock_max_scroll = 0;
static int lop_drag = 0;                      // dragging the active-layer opacity slider
static int LO_y = 0;                          // opacity-slider row y (for hit testing)
#define THUMB 34
static uint32_t *thumb_buf[STUDIO_MAX_LAYERS];
static int g_thumbs_dirty = 1;
// Cached Brushes/Patterns dock-cell thumbnails. The stock assets never change,
// so each stock cell is tiled/rendered ONCE into a malloc'd buffer and blitted
// thereafter. Re-tiling all 58 patterns (676 pattern_px() lookups each) on every
// dock repaint was the redraw-loop that made the Patterns panel flash (see
// blame.md). Cells are indexed by stock index; the parametric round brush and
// the "none" pattern are cheap and drawn directly (not cached).
#define BPCACHE_MAX 128
static uint32_t *g_patcell_cache[BPCACHE_MAX];
static uint32_t *g_brucell_cache[BPCACHE_MAX];
// Dock grid-hover gate: the Brushes/Patterns cell grids have NO hover feedback,
// so repainting the whole dock while the pointer merely tracks across them is
// pure waste (and the source of the flashing). We record the grid body rects and
// skip the hover repaint while the pointer sits inside them.
static int BRG_y0=0,BRG_y1=0,PTG_y0=0,PTG_y1=0;
static int g_last_over_grid=0;
// Channels panel: per-channel visibility + grayscale thumbnails from the composite
#define CHTHUMB 16
static int       g_chan_vis[4] = {1,1,1,1};      // R,G,B,A display visibility
static uint32_t *g_chan_thumb[4] = {0,0,0,0};
// Forward declarations for helpers defined later but used earlier in the file.
static void draw_brush_tip(int cx,int cy,int rad,int hardness,uint32_t col,uint32_t bg);
static void draw_ring_clip(int cx,int cy,int r,uint32_t c);
static void draw_filebrowser(void);   // item 3: file-browser modal overlay
static void draw_guard(void);         // item 3: unsaved-changes guard overlay
static void draw_print_preview(void); // Print / Preview modal overlay
static int  click_print_preview(int mx,int my);
static void open_print_preview(void);
static unsigned int g_hist_cache[256];
static int g_hist_ch_cache = -1, g_hist_dirty = 1;

// Modal input
static int   modal = 0;         // 0 none, 1 text, 2 message
static char  modal_prompt[64];
static char  modal_buf[160];
static textfield_t modal_tf;
static int   modal_purpose = 0;
static char  modal_msg[512];
enum { MP_OPEN = 1, MP_SAVEAS, MP_EXPORT, MP_AICMD, MP_AIPAL, MP_NEWSZ,
       MP_SGROW, MP_SSHRINK, MP_SBORDER, MP_SFEATHER, MP_SROUND,
       MP_ROT, MP_SCALE, MP_CANVAS, MP_GRIDSP, MP_LRENAME, MP_EXPORTBMP };

// Print / Preview modal state (its own overlay, like the file browser).
static int   pr_open = 0, pr_have = 0;
static char  pr_printer[40] = {0};

// Generic parameter dialog (modal==3 conceptually; own state)
static int pd_open = 0;
static int pd_opi = -1;
static int pd_np = 0;
static int pd_val[STUDIO_MAX_PARAMS];
static int pd_drag = -1;            // param index whose slider is being dragged
static int pd_dialdrag = -1;        // param index whose SP_ANGLE dial is being dragged
static int pd_cvdrag = -1;          // param index of the SP_CURVE being edited (a point is dragging)
static int pd_hdrag = -1;           // on-canvas positional handle being dragged (0..1), or -1
static int pd_kdrag = 0;            // 1 while an SP_KERNEL cell value-drag is active
static uint32_t *pd_backup = 0;     // active-layer pixel backup for live preview
static uint8_t  *pd_maskbk = 0;
static int pd_backup_layer = -1;
// In-dialog filter preview pane (#541 Photoshop-parity LOUPE): a zoomable,
// pannable window into the active layer. pd_pvwork is the fixed on-screen
// display box (aspect-fit inside PV_BOXW x PV_BOXH). pd_region is a pristine
// CROP of the active layer (a window sized by zoom+center); pd_regwork is the
// per-render op scratch at region resolution. Render path: copy region->regwork,
// route the op into regwork via draw_preview_target_set (so it renders on JUST
// the window), then scale regwork into the display box. Rebuild the region on
// zoom/center change; re-run the op on every param change. At 1:1 the region is
// pd_pv_w x pd_pv_h actual pixels (a crop); zooming in shrinks the region and
// nearest-upscales it; "Fit" makes the region the whole layer (downscaled view,
// matching the old thumbnail). Sampled from pd_backup (the full pristine layer).
#define PV_BOXW 320
#define PV_BOXH 150
static uint32_t *pd_pvwork = 0;   // display-box output (pd_pv_w x pd_pv_h)
static uint32_t *pd_region = 0;   // pristine crop source (pd_reg_w x pd_reg_h)
static uint32_t *pd_regwork = 0;  // op scratch at region resolution
static int pd_pv_w = 0, pd_pv_h = 0;    // display box size (on screen)
static int pd_reg_w = 0, pd_reg_h = 0;  // current region (source-pixel) size
static int pd_reg_x = 0, pd_reg_y = 0;  // region top-left in source pixels
static int pd_zoom = 100;               // loupe zoom, percent (100 = 1:1)
static int pd_cx = 0, pd_cy = 0;        // loupe center in source-pixel coords
static int pd_loupe_drag = 0;           // 1 while dragging inside the pane to pan
static int pd_drag_mx0 = 0, pd_drag_my0 = 0; // pointer at pan-drag start
static int pd_drag_cx0 = 0, pd_drag_cy0 = 0; // center at pan-drag start
static const int PD_ZOOMS[] = {25,50,75,100,150,200,300,400,600,800};
#define PD_NZOOM ((int)(sizeof(PD_ZOOMS)/sizeof(PD_ZOOMS[0])))

// Histogram channel
static int hist_ch = 0;   // 0 lum,1 R,2 G,3 B
static int hist_log = 0;  // 0 = linear, 1 = log scale (Lin/Log toggle)

// ---------------------------------------------------------------------------
// Small draw helpers
// ---------------------------------------------------------------------------
static void R(int x,int y,int w,int h,uint32_t c){ win_draw_rect(g_win,x,y,w,h,c); }
// All Studio text is antialiased TrueType (matches the Settings/Files design
// language). T = body 13px, Ts = small 11px.
static void T(int x,int y,const char*s,uint32_t c){ win_draw_text_ttf(g_win,x,y,s,13,c); }
static void Ts(int x,int y,const char*s,uint32_t c){ win_draw_text_ttf(g_win,x,y,s,11,c); }
static void OUT(int x,int y,int w,int h,uint32_t c){ gui_draw_rect_outline(g_win,x,y,w,h,c); }
static int  IN(int px,int py,int x,int y,int w,int h){ return px>=x&&px<x+w&&py>=y&&py<y+h; }

// ---- gradient + bevel chrome (kills the flat/boxy look) --------------------
static uint32_t mix(uint32_t a,uint32_t b,int t){ // t 0..255, a->b
    int ar=(a>>16)&255,ag=(a>>8)&255,ab=a&255, br=(b>>16)&255,bg=(b>>8)&255,bb=b&255;
    int r=ar+(br-ar)*t/255, g=ag+(bg-ag)*t/255, bl=ab+(bb-ab)*t/255;
    return (r<<16)|(g<<8)|bl;
}
// vertical gradient fill top->bottom
static void vgrad(int x,int y,int w,int h,uint32_t top,uint32_t bot){
    if(h<=0) return;
    for(int i=0;i<h;i++) R(x,y+i,w,1, mix(top,bot,h>1?i*255/(h-1):0));
}
// 1px raised bevel: light top/left, dark bottom/right
static void bevel(int x,int y,int w,int h,uint32_t lite,uint32_t dark){
    R(x,y,w,1,lite); R(x,y,1,h,lite);
    R(x,y+h-1,w,1,dark); R(x+w-1,y,1,h,dark);
}
// raised gradient panel with a hairline frame
static void panelg(int x,int y,int w,int h){
    vgrad(x,y,w,h, mix(C_PANEL,0x00ffffff,14), C_PANEL2);
    bevel(x,y,w,h, mix(C_PANEL,0x00ffffff,40), C_LINE);
}
static int text_w(const char*s){ return gui_string_width(s); }

// Buttons compute hover/press from the global mouse state, so every call site
// gets feedback without threading a hot= flag through. `hot` still forces hover.
static void button(int x,int y,int w,int h,const char*label,int hot,int on){
    int hov = IN(g_mx,g_my,x,y,w,h);
    int prs = hov && g_mdown;
    hot = hot || hov;
    if(on)       vgrad(x,y,w,h, C_ACCENT2, C_ACCENT);
    else if(prs) vgrad(x,y,w,h, C_PANEL2, C_BTN);                         // inset
    else         vgrad(x,y,w,h, mix(hot?C_BTN_HOV:C_BTN,0x00ffffff,30), hot?C_BTN:C_PANEL2);
    if(prs) bevel(x,y,w,h, C_LINE, mix(C_BTN_HOV,0x00ffffff,50));         // inverted bevel
    else    bevel(x,y,w,h, mix(C_BTN_HOV,0x00ffffff,50), C_LINE);
    if(label && label[0]) gui_text_ttf_centered(g_win, x+(prs?1:0), y+(prs?1:0), w, h, label, C_TEXT, 13);
}
static void sbutton(int x,int y,int w,int h,const char*label,int on){
    int hov = IN(g_mx,g_my,x,y,w,h);
    int prs = hov && g_mdown;
    if(on)       vgrad(x,y,w,h, C_ACCENT2, C_ACCENT);
    else if(prs) vgrad(x,y,w,h, C_PANEL2, C_BTN);
    else         vgrad(x,y,w,h, mix(hov?C_BTN_HOV:C_BTN,0x00ffffff,24), hov?C_BTN:C_PANEL2);
    if(prs) bevel(x,y,w,h, C_LINE, mix(C_BTN_HOV,0x00ffffff,40));
    else    bevel(x,y,w,h, mix(C_BTN_HOV,0x00ffffff,40), C_LINE);
    if(label&&label[0]) gui_text_ttf_centered(g_win, x+(prs?1:0), y+(prs?1:0), w, h, label, C_TEXT, 11);
}
// horizontal slider: track + accent fill + raised knob. val/max in [0..max].
static void slider(int x,int y,int w,int val,int mx){
    int cy=y+7;
    R(x,cy-2,w,4, C_LINE);                       // groove
    bevel(x,cy-2,w,4, C_PANEL2, mix(C_PANEL,0x00ffffff,20));
    int fill = mx>0 ? val*(w-10)/mx : 0;
    vgrad(x, cy-2, fill+5, 4, C_ACCENT2, C_ACCENT);   // filled portion
    int kx = x + (mx>0 ? val*(w-12)/mx : 0);     // knob
    vgrad(kx, y+1, 12, 12, mix(C_BTN_HOV,0x00ffffff,40), C_BTN);
    bevel(kx, y+1, 12, 12, mix(C_BTN_HOV,0x00ffffff,70), C_LINE);
}
// Sunken numeric field: dark field fill + inverted (inset) bevel so value
// read-outs read as recessed inputs, matching the Settings/Files field style.
static void field_inset(int x,int y,int w,int h){
    R(x,y,w,h, C_FIELD);
    bevel(x,y,w,h, C_LINE, mix(C_FIELD,0x00ffffff,26));   // dark top/left, light bottom/right
    OUT(x,y,w,h, C_FIELD_BORDER);
}
// Fixed-point log2(v)*16 (0 for v==0), used to log-scale the histogram bars so
// low-frequency bins stay visible next to a dominant peak. No libm needed.
static int ilog2s(unsigned int v){
    if(v==0) return 0;
    int hb=0; unsigned int t=v; while(t>1){ t>>=1; hb++; }        // floor(log2 v)
    int frac=0;
    if(hb>0){ unsigned int mant = v - (1u<<hb); frac=(int)((unsigned long long)mant*16/(1u<<hb)); }
    return hb*16+frac;
}

// ---------------------------------------------------------------------------
// Tool-contextual options bar (item 1). Controls are recorded during draw so
// the hit-tester in click_optbar() shares the exact geometry (no duplication).
// ---------------------------------------------------------------------------
enum { OC_SLIDER=0, OC_CYCLE, OC_SWAP, OC_DEFCOL, OC_TOGGLE };
typedef struct { int type,x,y,w,h; int *val; int vmax; int pct; const char **choices; int nch; } optctl_t;
static optctl_t g_oc[24];
static int g_noc = 0;
static int opt_drag = -1;                 // index into g_oc of the slider being dragged
static const char *SELMODE_NAMES[4] = {"New","Add","Subtract","Intersect"};
// Gradient blend-mode cycle: a small contiguous index maps to the (non-contiguous)
// blend_t enum values so OC_CYCLE can advance it 0..4 and store the mode into
// g_tool.grad_blend on redraw (see draw_optbar gradient branch).
static const char  *GRADBLEND_NAMES[5] = {"Normal","Multiply","Screen","Difference","Overlay"};
static const blend_t GRADBLEND_MODES[5] = {BLEND_NORMAL,BLEND_MULTIPLY,BLEND_SCREEN,BLEND_DIFFERENCE,BLEND_OVERLAY};
static int grad_mode_idx = 0;
// Gradient shape / repeat cycles (P3-GRADSHAPE/REPEAT). These contiguous indices
// map 1:1 onto the GRAD_* enums in brushes.h, so draw_optbar pushes them straight
// into grad_shape_set()/grad_repeat_set() after drawing (mirrors grad_mode_idx).
static const char *GRADSHAPE_NAMES[GRAD_SHAPE_COUNT]  = {"Linear","Bi-Lin","Radial","Square","Conical","Spiral"};
static const char *GRADREPEAT_NAMES[GRAD_REPEAT_COUNT] = {"None","Saw","Tri"};
static int grad_shape_idx = 0, grad_repeat_idx = 0, grad_rev_idx = 0;
// Brush preset picker geometry (recorded during draw_optbar, hit-tested in click_optbar)
static int bp_shown=0, bp_x0=0, bp_x1=0, bp_y=0, bp_sz=0;

static int tool_is_selection(tool_id_t t){
    return t==TL_SEL_RECT||t==TL_SEL_ELLIPSE||t==TL_SEL_LASSO||t==TL_SEL_WAND||t==TL_SEL_BYCOLOR;
}
static int tool_is_brush(tool_id_t t){
    return t==TL_BRUSH||t==TL_PENCIL||t==TL_ERASER||t==TL_AIRBRUSH||t==TL_CLONE||
           t==TL_SMUDGE||t==TL_BLUR||t==TL_HEAL||t==TL_DODGE||t==TL_BURN||t==TL_SHARPEN||t==TL_INK;
}
// A labeled slider control (item 1): draws label + shared slider() + value.
static int oc_slider(int x,int y0,const char*label,int*val,int vmax,int pct){
    int ty=y0+8;
    Ts(x,ty,label,C_DIM); x+=text_w(label)+7;
    int sw=80;
    slider(x,y0+4,sw,*val,vmax);
    if(g_noc<24){ optctl_t*o=&g_oc[g_noc++]; o->type=OC_SLIDER; o->x=x; o->y=y0+4; o->w=sw; o->h=16;
                  o->val=val; o->vmax=vmax; o->pct=pct; o->choices=0; o->nch=0; }
    x+=sw+6;
    char b[16]; int dv = pct ? (*val*100/(vmax>0?vmax:1)) : *val;
    if(pct) snprintf(b,sizeof b,"%d%%",dv); else snprintf(b,sizeof b,"%d",dv);
    int fw = pct?34:28;
    field_inset(x-2, y0+4, fw, 19);
    Ts(x+2,ty,b,C_TEXT); x += fw;
    return x;
}
// A labeled cycle control (click advances through choices), e.g. selection Mode.
static int oc_cycle(int x,int y0,const char*label,int*val,const char**ch,int nch){
    int ty=y0+8;
    Ts(x,ty,label,C_DIM); x+=text_w(label)+7;
    int cur=*val; if(cur<0)cur=0; if(cur>=nch)cur=nch-1;
    int cw=82;
    sbutton(x,y0+4,cw,22,ch[cur],0);
    if(g_noc<24){ optctl_t*o=&g_oc[g_noc++]; o->type=OC_CYCLE; o->x=x; o->y=y0+4; o->w=cw; o->h=22;
                  o->val=val; o->vmax=nch; o->pct=0; o->choices=ch; o->nch=nch; }
    x+=cw+10;
    return x;
}
// A compact on/off toggle button (click flips *val). Used for gradient Reverse.
static int oc_toggle(int x,int y0,const char*label,int*val){
    int w=34;
    sbutton(x,y0+4,w,22,label,*val);
    if(g_noc<24){ optctl_t*o=&g_oc[g_noc++]; o->type=OC_TOGGLE; o->x=x; o->y=y0+4; o->w=w; o->h=22;
                  o->val=val; o->vmax=2; o->pct=0; o->choices=0; o->nch=2; }
    x+=w+8;
    return x;
}

// Branded splash shown for ~0.8s at launch (the STUDIO.BMP art if present,
// else a drawn gradient title card). Runs before the main UI is built.
// Integer sqrt (local to the splash; the brush engine's isqrt64 is file-static).
static int splash_isqrt(int v){ if(v<=0) return 0; int x=v,y=(x+1)/2; while(y<x){ x=y; y=(x+v/x)/2; } return x; }
// Filled disc (scanline), used for the splash sun.
static void splash_disc(int cx,int cy,int r,uint32_t c){
    for(int dy=-r;dy<=r;dy++){ int dx=splash_isqrt(r*r-dy*dy); R(cx-dx,cy+dy,dx*2+1,1,c); }
}

// #472 Rectangular image-editor splash: a maritime scene above a dark studio
// info panel that reveals verbose loading lines + a progress bar. The whole
// window IS the splash card (Photoshop/GIMP style), branded "Maytera Studio".
void ui_splash(int win, int w, int h){
    g_win = win;
    theme_sync();

    int artH = (h*58)/100;                 // maritime art occupies the top ~58%
    int horizon = (artH*72)/100;           // sea starts here

    // --- Sky: dusk gradient, deep indigo up top warming to amber at the horizon.
    vgrad(0,0,w,horizon, 0x000d1430, 0x00e8933a);
    // Sun low over the water, with a soft halo.
    int sx=(w*68)/100, sy=(horizon*82)/100, sr=w/16; if(sr<26) sr=26; if(sr>70) sr=70;
    splash_disc(sx,sy,sr+8, mix(0x00e8933a,0x00ffd98a,90));   // halo
    splash_disc(sx,sy,sr,   0x00ffe4a0);                       // sun body
    splash_disc(sx,sy,sr-6, 0x00fff2cf);                       // hot core

    // --- Sea: darker gradient below the horizon, with the sun's shimmer + waves.
    vgrad(0,horizon,w,artH-horizon, 0x00223a58, 0x00081020);
    for(int y=horizon+3;y<artH;y+=5){                          // horizontal wave streaks
        uint32_t wc = mix(0x00223a58,0x005a86b8, 40 - (y-horizon)/3);
        for(int x=0;x<w;x+=7) R(x,y,4,1,wc);
    }
    for(int y=horizon;y<artH;y+=2){                            // sun reflection column
        int spread=((y-horizon)*sr)/(artH-horizon)+3;
        R(sx-spread/2,y,spread,1, mix(0x00ffe4a0, 0x00223a58, ((y-horizon)*180)/(artH-horizon)));
    }
    // --- Sailboat silhouette to the left, riding the horizon.
    { int bx=(w*26)/100, by=horizon+4, bw=w/10; if(bw<48) bw=48;
      uint32_t sil=0x000a1420;
      for(int i=0;i<bw/2;i++) R(bx-bw/2+i, by-(i*bw/bw), bw-2*i, 2, sil);   // hull
      int mh=bw; for(int i=0;i<mh;i++){ R(bx, by-mh+i, 1, 1, sil);          // mast
        R(bx+1, by-mh+i, 1+(i*bw/3)/mh, 1, sil); }                          // mainsail
      for(int i=0;i<mh*2/3;i++) R(bx-1-(i*bw/4)/(mh), by-mh*2/3+i, 1, 1, sil); // jib
    }

    // --- Studio info panel (the rectangular editor-style plate).
    R(0,artH,w,h-artH, 0x00141821);
    R(0,artH,w,2, C_ACCENT);                                   // accent divider
    // Logo chevrons.
    int lx=w/2-150, ly=artH+34;
    for(int i=0;i<3;i++){ int s=20-i*6; uint32_t c=mix(C_ACCENT,0x00ffffff,i*46);
        for(int t=0;t<=s;t++){ R(lx-t, ly+s-t, 2,2,c); R(lx+t-2, ly+s-t, 2,2,c); } }
    win_draw_text_ttf(win, lx+28, artH+22, "Maytera Studio", 30, 0x00f2f5f8);
    win_draw_text_ttf(win, lx+30, artH+54, "Image Editor  \xC2\xB7  Version 2.0", 13, 0x008fa0b4);

    // Progress bar + verbose loading lines revealed one at a time.
    static const char *steps[] = {
        "Initializing canvas + layer engine",
        "Loading 66 stock brushes",
        "Loading 59 tileable patterns",
        "Building gradient + palette engine",
        "Registering filters and effects",
        "Maytera Studio ready",
    };
    int nsteps = (int)(sizeof(steps)/sizeof(steps[0]));
    int barX = w/2-150, barY = h-46, barW = 300, barH = 8;
    int listX = w/2-150, listY = artH+92, lineH = (barY-8-listY)/nsteps; if(lineH<14) lineH=14;
    for(int s=0;s<nsteps;s++){
        // bar track + fill
        R(barX,barY,barW,barH, 0x00263040);
        int fill = (barW*(s+1))/nsteps;
        vgrad(barX,barY,fill,barH, C_ACCENT2, C_ACCENT);
        // reveal loading lines up to the current step; done lines get a tick.
        for(int i=0;i<=s;i++){
            uint32_t tc = (i==s)? 0x00e8edf2 : 0x00768399;
            Ts(listX, listY+i*lineH, (i<s)?"\xE2\x9C\x93":"\xC2\xBB", i<s?C_ACCENT:C_ACCENT2);
            Ts(listX+18, listY+i*lineH, steps[i], tc);
        }
        gui_text_ttf_centered(win, barX, barY-20, barW, 16, steps[s], 0x00aab6c4, 12);
        win_invalidate(win);
        sys_sleep(s==nsteps-1?360:180);
    }
}

// ---------------------------------------------------------------------------
// Registry menu grouping
// ---------------------------------------------------------------------------
#define MAX_CATS 20
static const char *g_cats[MAX_CATS];
static int g_ncats = 0;
static int g_ncolor = 0;   // count of Colors ops
static void cats_init(void){
    g_ncats = 0; g_ncolor = 0;
    for(int i=0;i<studio_op_count();i++){
        const studio_op_t *op = studio_op_get(i);
        if(!op || !op->menu) continue;
        if(!strcmp(op->menu,"Colors")){ g_ncolor++; continue; }
        int found=0; for(int k=0;k<g_ncats;k++) if(!strcmp(g_cats[k],op->menu)){found=1;break;}
        if(!found && g_ncats<MAX_CATS) g_cats[g_ncats++] = op->menu;
    }
}

// ---------------------------------------------------------------------------
// Menu model. Static menus + two dynamic ones (Colors, Filters).
// ---------------------------------------------------------------------------
enum {
    A_NONE=0, A_NEW, A_OPEN, A_SAVE, A_SAVEAS, A_EXPORTPNG, A_EXPORTBMP, A_PRINT, A_QUIT,
    A_UNDO, A_REDO, A_FLATTEN,
    A_LNEW, A_LDUP, A_LDEL, A_LUP, A_LDOWN, A_LMERGE, A_LOPUP, A_LOPDN,
    A_LMASKADD, A_LMASKAPPLY, A_LMASKDEL, A_LMASKTOG, A_LLOCKA, A_LBLENDPOP,
    A_SELALL, A_SELNONE, A_SELINV, A_SGROW, A_SSHRINK, A_SBORDER, A_SFEATHER, A_SROUND,
    A_IFLIPH, A_IFLIPV, A_IROT90CW, A_IROT90CCW, A_IROTARB, A_ISCALE, A_ICROP, A_ICANVAS,
    A_VGRID, A_VGRIDSP, A_VGCLEAR, A_ZIN, A_ZOUT, A_ZFIT,
    A_CHSAVE,
    A_AICMD, A_AIPAL, A_ABOUT,
    A_REVERT,
    A_CUT, A_COPY, A_PASTE, A_FILLFG, A_FILLBG, A_STROKE,
    A_SEP = 4000,         // separator sentinel (non-selectable divider row)
    A_OP_BASE = 5000,     // + registry op index
    A_CAT_BASE = 6000     // + category index (Filters submenu open)
};

// `sc` is the optional right-aligned shortcut hint (NULL = none). Keeping it in a
// separate column (instead of padded into the label with spaces) is what makes
// the menus line up cleanly (item 3).
typedef struct { const char *label; int action; const char *sc; } mitem_t;
typedef struct { const char *name; const mitem_t *items; int n; int kind; } menu_t;
enum { MK_STATIC=0, MK_COLORS, MK_FILTERS };

static const mitem_t M_FILE[] = {
    {"New", A_NEW}, {"Open..", A_OPEN}, {"", A_SEP},
    {"Save", A_SAVE, "Ctrl+S"}, {"Save As..", A_SAVEAS}, {"", A_SEP},
    {"Export PNG..", A_EXPORTPNG}, {"Export BMP..", A_EXPORTBMP},
    {"Print / Preview..", A_PRINT}, {"", A_SEP},
    {"Revert", A_REVERT}, {"", A_SEP}, {"Quit", A_QUIT}
};
static const mitem_t M_EDIT[] = {
    {"Undo", A_UNDO, "Ctrl+Z"}, {"Redo", A_REDO, "Ctrl+Y"}, {"", A_SEP},
    {"Cut", A_CUT, "Ctrl+X"}, {"Copy", A_COPY, "Ctrl+C"}, {"Paste", A_PASTE, "Ctrl+V"},
    {"", A_SEP},
    {"Fill with FG", A_FILLFG}, {"Fill with BG", A_FILLBG}, {"Stroke Selection", A_STROKE}
};
static const mitem_t M_SELECT[] = {
    {"All", A_SELALL, "Ctrl+A"}, {"None", A_SELNONE, "Ctrl+D"}, {"Invert", A_SELINV, "Ctrl+I"},
    {"Grow..", A_SGROW}, {"Shrink..", A_SSHRINK}, {"Border..", A_SBORDER},
    {"Feather..", A_SFEATHER}, {"Rounded..", A_SROUND}, {"Save to Channel", A_CHSAVE}
};
static const mitem_t M_LAYER[] = {
    {"New Layer", A_LNEW}, {"Duplicate", A_LDUP}, {"Delete", A_LDEL},
    {"Raise", A_LUP}, {"Lower", A_LDOWN}, {"Merge Down", A_LMERGE}, {"", A_SEP},
    {"Add Mask", A_LMASKADD}, {"Apply Mask", A_LMASKAPPLY}, {"Delete Mask", A_LMASKDEL},
    {"Edit Mask", A_LMASKTOG}, {"Lock Alpha", A_LLOCKA}, {"", A_SEP},
    {"Blend Mode..", A_LBLENDPOP}, {"Opacity +", A_LOPUP}, {"Opacity -", A_LOPDN}
};
static const mitem_t M_IMAGE[] = {
    {"Flip Horizontal", A_IFLIPH}, {"Flip Vertical", A_IFLIPV},
    {"Rotate 90 CW", A_IROT90CW}, {"Rotate 90 CCW", A_IROT90CCW},
    {"Rotate..", A_IROTARB}, {"Scale Layer..", A_ISCALE},
    {"Crop to Selection", A_ICROP}, {"Canvas Size..", A_ICANVAS}, {"Flatten", A_FLATTEN}
};
static const mitem_t M_VIEW[] = {
    {"Zoom In", A_ZIN, "+"}, {"Zoom Out", A_ZOUT, "-"}, {"Fit", A_ZFIT}, {"", A_SEP},
    {"Grid", A_VGRID}, {"Grid Spacing..", A_VGRIDSP}, {"Clear Guides", A_VGCLEAR}
};
static const mitem_t M_AI[] = { {"AI Command..", A_AICMD}, {"AI Palette..", A_AIPAL} };
static const mitem_t M_HELP[] = { {"About Maytera Studio", A_ABOUT} };

#define MKA(a) (a), (int)(sizeof(a)/sizeof(a[0]))
static menu_t MENUS[12];
static int    NMENU = 0;
static void menus_init(void){
    int i=0;
    MENUS[i++]=(menu_t){"File",  MKA(M_FILE), MK_STATIC};
    MENUS[i++]=(menu_t){"Edit",  MKA(M_EDIT), MK_STATIC};
    MENUS[i++]=(menu_t){"Select",MKA(M_SELECT), MK_STATIC};
    MENUS[i++]=(menu_t){"View",  MKA(M_VIEW), MK_STATIC};
    MENUS[i++]=(menu_t){"Image", MKA(M_IMAGE), MK_STATIC};
    MENUS[i++]=(menu_t){"Layer", MKA(M_LAYER), MK_STATIC};
    MENUS[i++]=(menu_t){"Colors",0, g_ncolor, MK_COLORS};
    MENUS[i++]=(menu_t){"Filters",0, g_ncats, MK_FILTERS};
    MENUS[i++]=(menu_t){"AI",    MKA(M_AI), MK_STATIC};
    MENUS[i++]=(menu_t){"Help",  MKA(M_HELP), MK_STATIC};
    NMENU = i;
}
static int menu_x(int i){ int x=6; for(int k=0;k<i;k++) x += gui_string_width(MENUS[k].name)+16; return x; }
static int menu_w(int i){ return gui_string_width(MENUS[i].name)+16; }

// Global index of the Nth Colors op / Nth op in a category.
static int nth_color_op(int n){
    int c=0; for(int i=0;i<studio_op_count();i++){ const studio_op_t*o=studio_op_get(i);
        if(o&&o->menu&&!strcmp(o->menu,"Colors")){ if(c==n) return i; c++; } }
    return -1;
}
static int nth_cat_op(const char*cat,int n){
    int c=0; for(int i=0;i<studio_op_count();i++){ const studio_op_t*o=studio_op_get(i);
        if(o&&o->menu&&!strcmp(o->menu,cat)){ if(c==n) return i; c++; } }
    return -1;
}
static int cat_op_count(const char*cat){
    int c=0; for(int i=0;i<studio_op_count();i++){ const studio_op_t*o=studio_op_get(i);
        if(o&&o->menu&&!strcmp(o->menu,cat)) c++; }
    return c;
}

// ---------------------------------------------------------------------------
// Clipboard (item 2: Edit menu). A rectangular ARGB buffer captured from the
// active layer, masked by the current selection (alpha scaled by coverage).
// ---------------------------------------------------------------------------
static uint32_t *g_clip=0; static int g_clip_w=0,g_clip_h=0,g_clip_x=0,g_clip_y=0;

// ---------------------------------------------------------------------------
// Menu item state: separators, checkmarks on toggles, greyed(disabled) items.
// ---------------------------------------------------------------------------
static int action_disabled(int a){
    switch(a){
        case A_UNDO:       return !undo_can_undo();
        case A_REDO:       return !undo_can_redo();
        case A_PASTE:      return g_clip==0;
        case A_REVERT:     return g_doc.path[0]==0 || !g_doc.modified;
        case A_SAVE:       return !g_doc.modified;
        case A_LDEL:       return g_doc.nlayers<=1;
        case A_LMERGE:     return g_doc.active<=0;
        case A_LMASKAPPLY:
        case A_LMASKDEL:
        case A_LMASKTOG:   return g_doc.layer[g_doc.active].mask==0;
        case A_LMASKADD:   return g_doc.layer[g_doc.active].mask!=0;
        case A_ICROP:      return !g_doc.sel_active;
        case A_STROKE:     return !g_doc.sel_active;
        default:           return 0;
    }
}
static int action_checked(int a){
    switch(a){
        case A_VGRID:    return grid_on;
        case A_LMASKTOG: return g_doc.layer[g_doc.active].mask && g_doc.layer[g_doc.active].mask_active;
        case A_LLOCKA:   return g_doc.layer[g_doc.active].lock_alpha;
        default:         return 0;
    }
}

// ---------------------------------------------------------------------------
// Tool strip (must cover ALL TL_COUNT tools)
// ---------------------------------------------------------------------------
static const char *TOOL_AB[TL_COUNT] = {
    "Br","Pn","Er","Ab","Cl","Sm","Bl","Fi","Gr","Ln","Rc","El","Tx","Mv","Pk",
    "[]","()","La","Wd",
    "He","Do","Bu","Sh","In","Sc","Cr","Ms","Pa"
};

// ---------------------------------------------------------------------------
// Enum-choice parsing (SP_ENUM "A|B|C")
// ---------------------------------------------------------------------------
static int enum_count(const char*s){ if(!s||!*s) return 0; int n=1; for(const char*p=s;*p;p++) if(*p=='|') n++; return n; }
static void enum_choice(const char*s,int idx,char*out,int cap){
    out[0]=0; if(!s) return; int cur=0,o=0;
    for(const char*p=s;;p++){
        if(*p=='|'||*p==0){ if(cur==idx){ out[o]=0; return; } cur++; o=0; }
        else if(cur==idx && o<cap-1){ out[o++]=*p; }
        if(*p==0) break;
    }
    out[o]=0;
}

// ---------------------------------------------------------------------------
// Canvas <-> screen mapping
// ---------------------------------------------------------------------------
static void screen_to_canvas(int sx,int sy,int *cx,int *cy){
    int z = zoom();
    *cx = pan_x + (sx - cv_x()) * 100 / z;
    *cy = pan_y + (sy - cv_y()) * 100 / z;
}
static int pt_in_canvas(int sx,int sy){ return IN(sx,sy,cv_x(),cv_y(),cv_w(),cv_h()); }

// ---- Zoom control (item 2) -------------------------------------------------
// Fit-to-window zoom percent.
static int zoom_fit_pct(void){
    int zx=cv_w()*100/(g_doc.w>0?g_doc.w:1), zy=cv_h()*100/(g_doc.h>0?g_doc.h:1);
    int z=zx<zy?zx:zy; return clampi(z,ZOOM_MIN,ZOOM_MAX);
}
// Set zoom to np%, keeping the canvas point currently under screen (ax,ay) fixed.
static void zoom_to(int np,int ax,int ay){
    np=clampi(np,ZOOM_MIN,ZOOM_MAX); if(np==g_zoom_pct) return;
    int zo=g_zoom_pct;
    int cxp=pan_x+(ax-cv_x())*100/zo, cyp=pan_y+(ay-cv_y())*100/zo;
    g_zoom_pct=np;
    pan_x=cxp-(ax-cv_x())*100/np; pan_y=cyp-(ay-cv_y())*100/np;
}
// Zoom centered on the canvas viewport middle (buttons / keys / slider).
static void zoom_to_center(int np){ zoom_to(np, cv_x()+cv_w()/2, cv_y()+cv_h()/2); }
// Step to the next/prev nice stop (dir +1/-1), centered on the viewport.
static void zoom_step(int dir){
    int cur=g_zoom_pct, np;
    if(dir>0){ np=ZOOM_MAX; for(int i=0;i<NZSTOP;i++) if(ZSTOPS[i]>cur){ np=ZSTOPS[i]; break; } }
    else     { np=ZOOM_MIN; for(int i=NZSTOP-1;i>=0;i--) if(ZSTOPS[i]<cur){ np=ZSTOPS[i]; break; } }
    zoom_to_center(np);
}

// ---------------------------------------------------------------------------
// Canvas blit (scaled) with optional grid overlay baked in.
// ---------------------------------------------------------------------------
// Fill a rect clipped to the canvas viewport (used for sheet border/shadow so
// they never spill onto the rulers or dock).
static void Rclip(int x,int y,int w,int h,uint32_t c){
    int x0=cv_x(),y0=cv_y(),x1=x0+cv_w(),y1=y0+cv_h();
    int ax=x<x0?x0:x, ay=y<y0?y0:y, bx=x+w>x1?x1:x+w, by=y+h>y1?y1:y+h;
    if(bx>ax && by>ay) R(ax,ay,bx-ax,by-ay,c);
}
static void blit_canvas(void){
    if(g_doc.comp_dirty){ g_thumbs_dirty=1; g_hist_dirty=1; }  // content changed -> refresh dock caches
    doc_composite();
    int vw = cv_w(), vh = cv_h();
    int need = vw * vh;
    if(need > g_blit_cap){
        if(g_blit) free(g_blit);
        g_blit = (uint32_t*)malloc((size_t)need * 4);
        g_blit_cap = g_blit ? need : 0;
    }
    if(!g_blit){ R(cv_x(),cv_y(),vw,vh,C_PASTEBOARD); return; }
    int z = zoom(), W = g_doc.w, H = g_doc.h;
    uint32_t *comp = g_doc.comp;
    uint32_t rgbmask = (g_chan_vis[0]?0x00FF0000u:0)|(g_chan_vis[1]?0x0000FF00u:0)|(g_chan_vis[2]?0x000000FFu:0);
    int alpha_vis = g_chan_vis[3];
    for(int vy=0; vy<vh; vy++){
        int sy = pan_y + vy*100/z;
        uint32_t *row = &g_blit[vy*vw];
        for(int vx=0; vx<vw; vx++){
            int sxp = pan_x + vx*100/z;
            if(comp && sxp>=0 && sxp<W && sy>=0 && sy<H){
                uint32_t src = comp[sy*W + sxp];
                if(rgbmask != 0x00FFFFFFu) src = (src & 0xFF000000u) | (src & rgbmask);
                int a = alpha_vis ? px_a(src) : 255;
                uint32_t px;
                if(a>=255){
                    px = src | 0xFF000000u;
                }else{
                    // real transparency: composite over a fixed-size checker
                    uint32_t bg = (((vx>>3)+(vy>>3))&1) ? C_CK_DK : C_CK_LT;
                    int r=(px_r(src)*a+px_r(bg)*(255-a))/255;
                    int g=(px_g(src)*a+px_g(bg)*(255-a))/255;
                    int b=(px_b(src)*a+px_b(bg)*(255-a))/255;
                    px = 0xFF000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
                }
                if(grid_on && grid_sp>1 && ((sxp%grid_sp)==0 || (sy%grid_sp)==0)){
                    // blend a faint grid line
                    int r=(px>>16)&255,g=(px>>8)&255,b=px&255;
                    r=(r+90)/2; g=(g+90)/2; b=(b+110)/2;
                    px=0xFF000000u|((uint32_t)r<<16)|((uint32_t)g<<8)|(uint32_t)b;
                }
                row[vx] = px;
            }else{
                row[vx] = C_PASTEBOARD;                 // flat pasteboard (no checker)
            }
        }
    }
    win_draw_image(g_win, cv_x(), cv_y(), vw, vh, g_blit);
    // ---- sheet framing: drop shadow (bottom+right) + 1px border ----
    int sx0=cv_x()+(0-pan_x)*z/100, sy0=cv_y()+(0-pan_y)*z/100;
    int sw=W*z/100, sh=H*z/100;
    for(int i=0;i<3;i++){
        uint32_t sc=mix(C_PASTEBOARD,0x00000000,120-i*34);
        Rclip(sx0+2+i, sy0+sh+i, sw, 1, sc);            // bottom shadow band
        Rclip(sx0+sw+i, sy0+2+i, 1, sh, sc);            // right shadow band
    }
    Rclip(sx0-1, sy0-1,  sw+2, 1, C_SHEETBORDER);       // top
    Rclip(sx0-1, sy0+sh, sw+2, 1, C_SHEETBORDER);       // bottom
    Rclip(sx0-1, sy0-1,  1, sh+2, C_SHEETBORDER);       // left
    Rclip(sx0+sw, sy0-1, 1, sh+2, C_SHEETBORDER);       // right
}

// Rulers along the top and left of the canvas.
static void draw_tool_icon(int t,int x,int y,uint32_t c);   // fwd decl (defined near the strip)

static void draw_rulers(void){
    int z=zoom();
    R(cv_x()-RULER_Y, cv_y()-RULER, cv_w()+RULER_Y, RULER, C_PANEL2);  // top ruler strip
    R(cv_x()-RULER_Y, cv_y(), RULER_Y, cv_h(), C_PANEL2);              // left ruler strip (26px gutter)
    R(cv_x()-RULER_Y, cv_y()-RULER, RULER_Y, RULER, C_PANEL);          // corner box
    // tick interval scales with zoom so on-screen spacing stays ~48-90px
    static const int STEPS[]={1,2,5,10,25,50,100,250,500,1000};
    int step=STEPS[9];
    for(int i=0;i<10;i++){ if(STEPS[i]*z/100 >= 48){ step=STEPS[i]; break; } }
    for(int cx=0; cx<g_doc.w; cx+=step){          // top ruler: tick + label
        int sx = cv_x() + (cx-pan_x)*z/100;
        if(sx<cv_x()||sx>=cv_x()+cv_w()) continue;
        R(sx,cv_y()-5,1,5,C_DIM);
        char b[8]; snprintf(b,sizeof(b),"%d",cx); Ts(sx+2,cv_y()-RULER+1,b,C_DIM);
    }
    for(int cy=0; cy<g_doc.h; cy+=step){          // left ruler: tick + label
        int sy = cv_y() + (cy-pan_y)*z/100;
        if(sy<cv_y()||sy>=cv_y()+cv_h()) continue;
        R(cv_x()-5,sy,5,1,C_DIM);
        // right-align the label inside the 26px gutter, just left of the tick,
        // so 3-4 digit values fit fully instead of bleeding onto the canvas.
        char b[8]; snprintf(b,sizeof(b),"%d",cy);
        int lw=gui_string_width(b), lxp=cv_x()-6-lw;
        if(lxp<cv_x()-RULER_Y+1) lxp=cv_x()-RULER_Y+1;
        Ts(lxp,sy+1,b,C_DIM);
    }
    if(pt_in_canvas(g_mx,g_my)){                   // pointer-tracking ticks
        R(g_mx,cv_y()-RULER,1,RULER,C_ACCENT2);
        R(cv_x()-RULER_Y,g_my,RULER_Y,1,C_ACCENT2);
    }
}

// Draw guides + marching ants + rubber-band over the canvas.
static void draw_overlays(void){
    int z=zoom(), W=g_doc.w, H=g_doc.h;
    // guides
    for(int i=0;i<nguide_v;i++){
        int sx=cv_x()+(guide_v[i]-pan_x)*z/100;
        if(sx>=cv_x()&&sx<cv_x()+cv_w()) for(int y=cv_y();y<cv_y()+cv_h();y+=2) win_draw_pixel(g_win,sx,y,0x0000a8ff);
    }
    for(int i=0;i<nguide_h;i++){
        int sy=cv_y()+(guide_h[i]-pan_y)*z/100;
        if(sy>=cv_y()&&sy<cv_y()+cv_h()) for(int x=cv_x();x<cv_x()+cv_w();x+=2) win_draw_pixel(g_win,x,sy,0x0000a8ff);
    }
    if(dragging_guide==1){ int sx=cv_x()+(drag_guide_pos-pan_x)*z/100; if(sx>=cv_x()&&sx<cv_x()+cv_w()) R(sx,cv_y(),1,cv_h(),C_ACCENT2); }
    if(dragging_guide==2){ int sy=cv_y()+(drag_guide_pos-pan_y)*z/100; if(sy>=cv_y()&&sy<cv_y()+cv_h()) R(cv_x(),sy,cv_w(),1,C_ACCENT2); }
    // marching ants
    if(g_doc.sel_active && g_doc.sel){
        int vw=cv_w(), vh=cv_h();
        for(int vy=0; vy<vh; vy++){
            int sy=pan_y+vy*100/z; if(sy<0||sy>=H) continue;
            for(int vx=0; vx<vw; vx++){
                int sx=pan_x+vx*100/z; if(sx<0||sx>=W) continue;
                if(g_doc.sel[sy*W+sx]<=127) continue;
                int edge=(sx==0||sy==0||sx==W-1||sy==H-1)||
                    g_doc.sel[sy*W+sx-1]<=127||g_doc.sel[sy*W+sx+1]<=127||
                    g_doc.sel[(sy-1)*W+sx]<=127||g_doc.sel[(sy+1)*W+sx]<=127;
                if(edge){ uint32_t c=(((vx+vy)>>2)&1)?0x00ffffff:0x00000000; win_draw_pixel(g_win,cv_x()+vx,cv_y()+vy,c); }
            }
        }
    }
    // rubber band
    if(shaping){
        int ax=cv_x()+(sh_x0-pan_x)*z/100, ay=cv_y()+(sh_y0-pan_y)*z/100;
        int bx=cv_x()+(sh_x1-pan_x)*z/100, by=cv_y()+(sh_y1-pan_y)*z/100;
        if(g_tool.id==TL_LINE){ OUT(ax-1,ay-1,3,3,C_ACCENT2); OUT(bx-1,by-1,3,3,C_ACCENT2); }
        else{ int x=ax<bx?ax:bx,y=ay<by?ay:by,w=(ax<bx?bx-ax:ax-bx)+1,h=(ay<by?by-ay:ay-by)+1; OUT(x,y,w,h,C_ACCENT2); }
    }
    // live text preview while typing (real TrueType: selected face/size/style)
    if(text_typing){
        int bx=cv_x()+(text_ax-pan_x)*z/100, by=cv_y()+(text_ay-pan_y)*z/100;
        int sz=clampi(g_tool.size,6,64)*z/100; if(sz<8)sz=8; if(sz>96)sz=96;
        int style=(g_tool.text_bold?FONT_STYLE_BOLD:0)|(g_tool.text_italic?FONT_STYLE_ITALIC:0);
        // Caret at the fields cursor position (#542), not just the end.
        char prev[80]; int i=0, k=0, cur=g_text_tf.cursor;
        for(;g_tool.text[i]&&k<78;i++){ if(i==cur) prev[k++]='|'; prev[k++]=g_tool.text[i]; }
        if(cur>=i && k<79) prev[k++]='|';
        prev[k]=0;
        win_draw_text_ttf_ex(g_win, bx+1, by+1, prev, g_tool.text_font, sz, style, 0x00000000);
        win_draw_text_ttf_ex(g_win, bx,   by,   prev, g_tool.text_font, sz, style, g_tool.fg & 0x00FFFFFF);
    }
    // brush-size ring cursor (follows the pointer over the canvas)
    if(tool_is_brush(g_tool.id) && !panning && !space_down && pt_in_canvas(g_mx,g_my)){
        int rad = g_tool.size*z/100/2; if(rad<3) rad=3;
        draw_ring_clip(g_mx, g_my, rad,   0x00000000);
        draw_ring_clip(g_mx, g_my, rad+1, 0x00ffffff);
    }
}

// ---------------------------------------------------------------------------
// Chrome panels
// ---------------------------------------------------------------------------
static void draw_menubar(void){
    vgrad(0,0,g_w,MENU_H, mix(C_PANEL,0x00ffffff,12), C_PANEL2);
    R(0,0,g_w,1, mix(C_PANEL,0x00ffffff,28));            // top highlight line
    R(0,MENU_H-1,g_w,1,C_LINE);
    for(int i=0;i<NMENU;i++){
        int x=menu_x(i), w=menu_w(i);
        int open=(i==menu_open), hot=(menu_open<0 && IN(g_mx,g_my,x,0,w,MENU_H));
        if(open)     vgrad(x,0,w,MENU_H, C_ACCENT2, C_ACCENT);
        else if(hot) vgrad(x,0,w,MENU_H, mix(C_BTN,0x00ffffff,22), C_PANEL2);
        T(x+8, 3, MENUS[i].name, open?0x00ffffff:C_TEXT);
    }
}

// Tool-contextual options bar. Branches on the active tool and lays out shared
// slider()/cycle controls, recording their geometry in g_oc for hit testing.
static void draw_optbar(void){
    int y0 = MENU_H;
    vgrad(0,y0,g_w,OPT_H, mix(C_PANEL2,0x00ffffff,7), C_PANEL2);
    R(0,y0+OPT_H-1,g_w,1,C_LINE);
    g_noc = 0;
    bp_shown = 0;
    tool_id_t t = g_tool.id;
    int x=8;
    draw_tool_icon((int)t, x, y0+7, C_ACCENT2); x += 20;   // active tool glyph
    T(x,y0+8, tool_name(t), C_TEXT); x += text_w(tool_name(t))+8;
    R(x,y0+6,1,OPT_H-12,C_LINE); x+=12;                 // group separator

    if(tool_is_selection(t)){
        x=oc_cycle (x,y0,"Mode",   &g_tool.sel_mode, SELMODE_NAMES,4);
        x=oc_slider(x,y0,"Feather",&g_tool.feather, 64,0);
        if(t==TL_SEL_WAND||t==TL_SEL_BYCOLOR)
            x=oc_slider(x,y0,"Tolerance",&g_tool.wand_tolerance,255,0);
    }else if(tool_is_brush(t)){
        // brush preset picker (Hard/Soft Round) with a rendered tip preview
        Ts(x,y0+8,"Brush",C_DIM); x+=text_w("Brush")+7;
        { int sz=22, py=y0+3; bp_shown=1; bp_y=py; bp_sz=sz;
          int hard=(g_tool.hardness>=200), soft=!hard;
          bp_x0=x; sbutton(x,py,sz,sz,"",hard);
          draw_brush_tip(x+sz/2,py+sz/2,sz/2-3,255,C_TEXT, hard?C_ACCENT:C_BTN);
          x+=sz+3;
          bp_x1=x; sbutton(x,py,sz,sz,"",soft);
          draw_brush_tip(x+sz/2,py+sz/2,sz/2-3,70,C_TEXT, soft?C_ACCENT:C_BTN);
          x+=sz+10;
        }
        x=oc_slider(x,y0,"Size",    &g_tool.size,     64,0);
        x=oc_slider(x,y0,"Hardness",&g_tool.hardness, 255,1);
        x=oc_slider(x,y0,"Opacity", &g_tool.opacity,  255,1);
        x=oc_slider(x,y0,"Flow",    &g_tool.flow,     255,1);
    }else if(t==TL_LINE||t==TL_RECT||t==TL_ELLIPSE){
        x=oc_slider(x,y0,"Size",    &g_tool.size,    64,0);
        x=oc_slider(x,y0,"Opacity", &g_tool.opacity, 255,1);
    }else if(t==TL_FILL){
        x=oc_slider(x,y0,"Tolerance",&g_tool.wand_tolerance,255,0);
        x=oc_slider(x,y0,"Opacity",  &g_tool.opacity,       255,1);
    }else if(t==TL_GRADIENT){
        x=oc_slider(x,y0,"Opacity", &g_tool.opacity, 255,1);
        x=oc_cycle (x,y0,"Mode",    &grad_mode_idx, GRADBLEND_NAMES,5);
        g_tool.grad_blend = GRADBLEND_MODES[clampi(grad_mode_idx,0,4)];
        // Shape + Repeat + Reverse (P3-GRADSHAPE/REPEAT). Short labels (Shp/Rep)
        // keep the cluster clear of the right-aligned FG/BG swatches.
        x=oc_cycle (x,y0,"Shp",     &grad_shape_idx,  GRADSHAPE_NAMES,  GRAD_SHAPE_COUNT);
        grad_shape_set(grad_shape_idx);
        x=oc_cycle (x,y0,"Rep",     &grad_repeat_idx, GRADREPEAT_NAMES, GRAD_REPEAT_COUNT);
        grad_repeat_set(grad_repeat_idx);
        x=oc_toggle(x,y0,"Rev",     &grad_rev_idx);
        grad_reverse_set(grad_rev_idx);
    }else if(t==TL_TEXT){
        // Installed TrueType faces from the OS font registry (queried once).
        static char        fn_buf[16][40];
        static const char *fn_ptr[16];
        static int         fn_n = 0;
        if(fn_n==0){
            int nc=font_count(); if(nc<1)nc=1; if(nc>16)nc=16;
            for(int i=0;i<nc;i++){
                font_name(i, fn_buf[i], (int)sizeof(fn_buf[i]));
                if(!fn_buf[i][0]){ fn_buf[i][0]='F'; fn_buf[i][1]=(char)('0'+i%10); fn_buf[i][2]=0; }
                fn_ptr[i]=fn_buf[i];
            }
            fn_n=nc;
        }
        if(g_tool.text_font>=fn_n) g_tool.text_font=0;
        x=oc_cycle (x,y0,"Font",    &g_tool.text_font, fn_ptr, fn_n);
        x=oc_slider(x,y0,"Size",    &g_tool.size,    64,0);
        x=oc_toggle(x,y0,"B",       &g_tool.text_bold);
        x=oc_toggle(x,y0,"I",       &g_tool.text_italic);
        x=oc_toggle(x,y0,"U",       &g_tool.text_underline);
        x=oc_slider(x,y0,"Opacity", &g_tool.opacity, 255,1);
    }else{
        const char*hint = (t==TL_MOVE)   ? "Drag on the canvas to move the active layer" :
                          (t==TL_PICK)   ? "Click the canvas to pick a colour" :
                          (t==TL_CROP)   ? "Drag a rectangle, release to crop" :
                          (t==TL_MEASURE)? "Drag to measure distance and angle" :
                          (t==TL_PATH)   ? "Click to add path nodes, release to close" : "";
        if(hint[0]) Ts(x,y0+8,hint,C_DIM);
    }

    // foreground/background colour + swap, kept right-aligned in the visible bar
    int rx=(g_w-DOCK_W)-96; if(rx<x+8) rx=x+8;
    R(rx,y0+4,22,22,g_tool.fg);       OUT(rx,y0+4,22,22,C_LINE);
    R(rx+14,y0+10,16,16,g_tool.bg);   OUT(rx+14,y0+10,16,16,C_LINE);
    sbutton(rx+38,y0+4,22,22,"x",0);
    if(g_noc<24){ optctl_t*o=&g_oc[g_noc++]; o->type=OC_SWAP; o->x=rx+38; o->y=y0+4; o->w=22; o->h=22; o->val=0; o->nch=0; }
    sbutton(rx+62,y0+4,20,22,"D",0);                    // default colours (black/white)
    if(g_noc<24){ optctl_t*o=&g_oc[g_noc++]; o->type=OC_DEFCOL; o->x=rx+62; o->y=y0+4; o->w=20; o->h=22; o->val=0; o->nch=0; }
}

// ---- tiny vector-glyph primitives for tool icons --------------------------
static void px(int x,int y,uint32_t c){ R(x,y,1,1,c); }
static void gline(int x0,int y0,int x1,int y1,uint32_t c){
    int dx=x1-x0, dy=y1-y0, sx=dx<0?-1:1, sy=dy<0?-1:1;
    if(dx<0)dx=-dx; if(dy<0)dy=-dy; int err=dx-dy;
    for(;;){ px(x0,y0,c); if(x0==x1&&y0==y1)break; int e2=2*err;
        if(e2>-dy){err-=dy;x0+=sx;} if(e2<dx){err+=dx;y0+=sy;} }
}
static void gdisc(int cx,int cy,int r,uint32_t c){
    for(int yy=-r;yy<=r;yy++)for(int xx=-r;xx<=r;xx++)
        if(xx*xx+yy*yy<=r*r) px(cx+xx,cy+yy,c);
}
static void gring(int cx,int cy,int r,uint32_t c){
    for(int yy=-r;yy<=r;yy++)for(int xx=-r;xx<=r;xx++){
        int d=xx*xx+yy*yy; if(d<=r*r && d>(r-1)*(r-1)) px(cx+xx,cy+yy,c); }
}
// Rendered brush tip: a disc with hardness-based edge falloff, composited over bg.
static void draw_brush_tip(int cx,int cy,int rad,int hardness,uint32_t col,uint32_t bg){
    if(rad<1) rad=1;
    int r2=rad*rad;
    int hr=rad*hardness/255; if(hr<0)hr=0; int hr2=hr*hr;
    int cr=px_r(col),cg=px_g(col),cb=px_b(col);
    int br=px_r(bg),bg2=px_g(bg),bb=px_b(bg);
    for(int dy=-rad; dy<=rad; dy++)
        for(int dx=-rad; dx<=rad; dx++){
            int d2=dx*dx+dy*dy; if(d2>r2) continue;
            int a;
            if(d2<=hr2) a=255;
            else { int den=r2-hr2; a= den>0 ? (r2-d2)*255/den : 0; if(a<0)a=0; if(a>255)a=255; }
            int rr=(cr*a+br*(255-a))/255, gg=(cg*a+bg2*(255-a))/255, bl=(cb*a+bb*(255-a))/255;
            win_draw_pixel(g_win, cx+dx, cy+dy, argb(255,rr,gg,bl));
        }
}
// Brush-size ring cursor over the canvas, clipped to the canvas view rect.
static void ring_px_clip(int x,int y,uint32_t c){
    if(x>=cv_x() && x<cv_x()+cv_w() && y>=cv_y() && y<cv_y()+cv_h()) win_draw_pixel(g_win,x,y,c);
}
static void draw_ring_clip(int cx,int cy,int r,uint32_t c){
    if(r<1) return;
    int x=r,y=0,err=1-r;
    while(x>=y){
        ring_px_clip(cx+x,cy+y,c); ring_px_clip(cx-x,cy+y,c);
        ring_px_clip(cx+x,cy-y,c); ring_px_clip(cx-x,cy-y,c);
        ring_px_clip(cx+y,cy+x,c); ring_px_clip(cx-y,cy+x,c);
        ring_px_clip(cx+y,cy-x,c); ring_px_clip(cx-y,cy-x,c);
        y++; if(err<0) err+=2*y+1; else { x--; err+=2*(y-x)+1; }
    }
}
// Draw a 16x16 monochrome tool glyph at (x,y) in colour c.
static void draw_tool_icon(int t,int x,int y,uint32_t c){
    #define P(a,b)     px(x+(a),y+(b),c)
    #define LN(a,b,d,e) gline(x+(a),y+(b),x+(d),y+(e),c)
    #define BOX(a,b,w2,h2) OUT(x+(a),y+(b),(w2),(h2),c)
    switch(t){
    case TL_BRUSH:   LN(3,13,10,6); LN(4,13,11,7); gdisc(x+12,y+4,2,c); LN(2,14,4,12); break;
    case TL_PENCIL:  LN(3,13,11,5); LN(5,13,13,5); LN(2,14,4,12); P(2,14); break;
    case TL_ERASER:  LN(4,11,9,6); LN(11,8,6,13); LN(4,11,6,13); LN(9,6,11,8); LN(6,13,11,13); break;
    case TL_AIRBRUSH:R(x+6,y+2,4,4,c); P(11,7);P(13,8);P(12,10);P(14,11);P(11,12);P(13,13);P(10,14); break;
    case TL_CLONE:   R(x+4,y+2,8,3,c); R(x+7,y+5,2,6,c); R(x+5,y+11,6,3,c); break;
    case TL_SMUDGE:  gdisc(x+6,y+6,3,c); LN(8,8,13,13); LN(13,13,11,13); LN(13,13,13,11); break;
    case TL_BLUR:    gring(x+8,y+9,4,c); P(8,4);P(7,5);P(9,5);P(6,6);P(10,6); break;
    case TL_FILL:    LN(4,4,11,7); LN(11,7,9,13); LN(9,13,3,10); LN(3,10,4,4); gdisc(x+12,y+11,2,c); break;
    case TL_GRADIENT:for(int i=0;i<12;i++) R(x+2+i,y+3,1,10, mix(0x00202020,0x00f0f0f0,i*255/11)); BOX(2,3,12,10); break;
    case TL_LINE:    LN(3,13,13,3); gdisc(x+3,y+13,1,c); gdisc(x+13,y+3,1,c); break;
    case TL_RECT:    BOX(3,4,10,8); break;
    case TL_ELLIPSE: gring(x+8,y+8,5,c); break;
    case TL_TEXT:    R(x+4,y+3,8,2,c); R(x+7,y+3,2,10,c); break;
    case TL_MOVE:    LN(8,2,8,14); LN(2,8,14,8); LN(8,2,6,4);LN(8,2,10,4); LN(8,14,6,12);LN(8,14,10,12);
                     LN(2,8,4,6);LN(2,8,4,10); LN(14,8,12,6);LN(14,8,12,10); break;
    case TL_PICK:    LN(11,3,13,5); LN(12,4,6,10); LN(4,12,6,10); gdisc(x+4,y+12,1,c); break;
    case TL_SEL_RECT:for(int i=3;i<13;i+=2){P(i,4);P(i,12);} for(int j=4;j<12;j+=2){P(3,j);P(13,j);} break;
    case TL_SEL_ELLIPSE:{ int r=5; for(int a=0;a<360;a+=30){ int px2=x+8+(r*8*((a%60)?1:1))/8; (void)px2; }
                     for(int yy=-r;yy<=r;yy++)for(int xx=-r;xx<=r;xx++){int d=xx*xx+yy*yy;
                        if(d<=r*r&&d>(r-1)*(r-1)&&((xx+yy)&1)) px(x+8+xx,y+8+yy,c);} } break;
    case TL_SEL_LASSO:gring(x+7,y+7,5,c); LN(7,12,10,15); break;
    case TL_SEL_WAND: LN(4,13,11,6); P(13,3);P(12,3);P(13,2);P(11,4);P(15,4); break;
    case TL_HEAL:    R(x+4,y+6,9,4,c); R(x+7,y+7,1,2,0x00000000); P(9,7);P(9,9); break;
    case TL_DODGE:   gdisc(x+5,y+5,3,c); LN(7,7,13,13); break;
    case TL_BURN:    gring(x+8,y+9,4,c); R(x+8,y+2,1,4,c); break;
    case TL_SHARPEN: for(int r=0;r<7;r++) R(x+8-r,y+3+r,2*r+1,1,c); break;
    case TL_INK:     LN(6,3,10,3); LN(6,3,8,13); LN(10,3,8,13); P(8,8); break;
    case TL_SEL_BYCOLOR: R(x+3,y+3,4,4,c); R(x+8,y+5,4,4,c); R(x+5,y+9,4,4,c); break;
    case TL_CROP:    LN(3,3,3,8); LN(3,3,8,3); LN(13,13,13,8); LN(13,13,8,13); break;
    case TL_MEASURE: BOX(2,6,12,4); P(4,6);P(4,7); P(8,6);P(8,7); P(12,6);P(12,7); break;
    case TL_PATH:    LN(3,12,7,5); LN(7,5,11,11); BOX(2,11,3,3); BOX(10,10,3,3); gdisc(x+7,y+4,1,c); break;
    default: Ts(x+2,y+3, TOOL_AB[t]?TOOL_AB[t]:"?", c); break;
    }
    #undef P
    #undef LN
    #undef BOX
}

static void draw_strip(void){
    vgrad(0,MENU_H+OPT_H,STRIP_W,g_h-MENU_H-OPT_H-STATUS_H, C_PANEL, C_PANEL2);
    R(STRIP_W-1,MENU_H+OPT_H,1,g_h-MENU_H-OPT_H-STATUS_H,C_LINE);
    int bw=22,bh=22, x0=(STRIP_W-2*bw-2)/2, y0=MENU_H+OPT_H+4;
    for(int i=0;i<TL_COUNT;i++){
        int col=i&1, row=i>>1;
        int x=x0+col*(bw+2), y=y0+row*(bh+2);
        int on = ((int)g_tool.id==i), hot=(hover_tool==i);
        if(on)       vgrad(x,y,bw,bh, C_ACCENT2, C_ACCENT);
        else         vgrad(x,y,bw,bh, mix(hot?C_BTN_HOV:C_BTN,0x00ffffff,24), C_PANEL2);
        bevel(x,y,bw,bh, mix(C_BTN_HOV,0x00ffffff,40), C_LINE);
        draw_tool_icon(i, x+3, y+3, on?0x00ffffff:C_TEXT);
    }
}

// Dock section y positions for hit testing.
static int LP_y, LM_y, LMODE_y, CH_y, CHSB_y, CHS_y, PA_y, HI_y, HP_y, CP_y, SW_y;
static int BR_y, PT_y;   // Brushes / Patterns grid body top-y (for hit testing)

// --- Dock helpers: layer thumbnails, eye glyph, collapsible section header ---
// Regenerate all layer thumbnails (nearest-neighbour, over a checkerboard) only
// when the composite changed. Buffers are malloc'd lazily and cached per layer.
static void ensure_thumbs(void){
    if(!g_thumbs_dirty) return;
    int W=g_doc.w, H=g_doc.h; if(W<1||H<1) return;
    for(int li=0; li<g_doc.nlayers && li<STUDIO_MAX_LAYERS; li++){
        if(!thumb_buf[li]){ thumb_buf[li]=(uint32_t*)malloc((size_t)THUMB*THUMB*4); if(!thumb_buf[li]) continue; }
        layer_t *L=&g_doc.layer[li]; uint32_t *b=thumb_buf[li];
        for(int ty=0; ty<THUMB; ty++){
            int sy=ty*H/THUMB; if(sy>=H) sy=H-1;
            for(int tx=0; tx<THUMB; tx++){
                int sx=tx*W/THUMB; if(sx>=W) sx=W-1;
                uint32_t p = L->px ? L->px[sy*W+sx] : 0;
                int a=px_a(p);
                uint32_t bg = (((tx>>3)+(ty>>3))&1) ? 0x00808080 : 0x00b8b8b8;
                int r=(px_r(p)*a+px_r(bg)*(255-a))/255;
                int g=(px_g(p)*a+px_g(bg)*(255-a))/255;
                int bl=(px_b(p)*a+px_b(bg)*(255-a))/255;
                b[ty*THUMB+tx]=argb(255,r,g,bl);
            }
        }
    }
    // channel thumbnails: grayscale of each R/G/B/A sampled from the composite
    if(g_doc.comp){
        for(int ci=0; ci<4; ci++){
            if(!g_chan_thumb[ci]){ g_chan_thumb[ci]=(uint32_t*)malloc((size_t)CHTHUMB*CHTHUMB*4); if(!g_chan_thumb[ci]) continue; }
            uint32_t *cb=g_chan_thumb[ci];
            for(int ty=0; ty<CHTHUMB; ty++){
                int sy=ty*H/CHTHUMB; if(sy>=H) sy=H-1;
                for(int tx=0; tx<CHTHUMB; tx++){
                    int sx=tx*W/CHTHUMB; if(sx>=W) sx=W-1;
                    uint32_t p=g_doc.comp[sy*W+sx];
                    int v = ci==0?px_r(p):ci==1?px_g(p):ci==2?px_b(p):px_a(p);
                    cb[ty*CHTHUMB+tx]=argb(255,v,v,v);
                }
            }
        }
    }
    g_thumbs_dirty=0;
}
static void draw_eye(int x,int y,int on){
    uint32_t c = on?C_ACCENT2:C_DIM;
    int cx=x+8, cy=y+8;
    gring(cx,cy,5,c);
    if(on) gdisc(cx,cy,2,c);
    else   gline(x+3,y+13,x+13,y+3,C_DIM);
}
// Collapsible section header bar with a chevron. Advances *py past the header.
// Returns 1 if the section is expanded (draw its body), 0 if collapsed.
static int dock_header(int dx,int *py,int sec){
    int y=*py; sec_hdr_y[sec]=y;
    int hov = IN(g_mx,g_my,dx+4,y,DOCK_W-8,18);
    vgrad(dx+4,y,DOCK_W-8,18, mix(C_PANEL,0x00ffffff,hov?26:16), C_PANEL2);
    bevel(dx+4,y,DOCK_W-8,18, mix(C_PANEL,0x00ffffff,34), C_LINE);
    int cx=dx+13, cy=y+9;
    if(SEC_COL[sec]){ gline(cx-1,cy-3,cx+2,cy,C_TEXT); gline(cx-1,cy+3,cx+2,cy,C_TEXT); }  // >
    else            { gline(cx-3,cy-1,cx,cy+2,C_TEXT); gline(cx+3,cy-1,cx,cy+2,C_TEXT); }  // v
    T(dx+22,y+1,SEC_NAME[sec],C_TEXT);
    *py = y+22;
    return !SEC_COL[sec];
}

// ---- Brushes / Patterns dock grids -----------------------------------------
// Shared cell geometry: 26x26 footprint on a 28px stride, 6 columns. draw and
// hit-test both derive coords from these so the geometry stays single-sourced.
#define BPCELL   26
#define BPSTRIDE 28
#define BPCOLS   6
// Render one brush cell. idx<0 = the parametric round brush drawn as a filled
// antialiased disc (3x3 supersampled coverage); idx>=0 samples the stock bitmap
// brush scaled to fit. Dark C_TEXT ink over the panel background.
// Render a brush cell into `buf` at size sz. idx<0 = parametric round brush.
static void render_brush_cell(uint32_t *buf,int sz,int idx){
    uint32_t bg=C_PANEL2, ink=C_TEXT;
    if(idx<0){
        int SS=3;
        int cen=sz*SS;                 // 2*center*SS, in 1/(2*SS)px units
        int rr=(sz-4)*SS;              // radius (sz/2-2 px) in the same units
        if(rr<SS) rr=SS;
        for(int j=0;j<sz;j++)for(int i=0;i<sz;i++){
            int in=0;
            for(int t=0;t<SS;t++)for(int s=0;s<SS;s++){
                int ddx=(i*SS+s)*2+1-cen, ddy=(j*SS+t)*2+1-cen;
                if(ddx*ddx+ddy*ddy<=rr*rr) in++;
            }
            buf[j*sz+i]=0xFF000000u|mix(bg,ink,in*255/(SS*SS));
        }
    }else{
        const studio_brush_t*b=brush_get(idx);
        int bw=(b&&b->w>0)?b->w:1, bh=(b&&b->h>0)?b->h:1;
        int m=bw>bh?bw:bh;             // fit longest side into the cell, centered
        for(int j=0;j<sz;j++)for(int i=0;i<sz;i++){
            int bx=i*m/sz-(m-bw)/2, by=j*m/sz-(m-bh)/2;
            buf[j*sz+i]=0xFF000000u|mix(bg,ink,brush_sample(idx,bx,by));
        }
    }
}
static void draw_brush_cell(int x,int y,int sz,int idx){
    static uint32_t buf[BPCELL*BPCELL];
    if(sz>BPCELL) sz=BPCELL; if(sz<1) return;
    // Cache stock brush cells at the standard grid size; blit thereafter.
    if(idx>=0 && idx<BPCACHE_MAX && sz==BPCELL-2){
        if(!g_brucell_cache[idx]){
            g_brucell_cache[idx]=(uint32_t*)malloc((size_t)sz*sz*4);
            if(g_brucell_cache[idx]) render_brush_cell(g_brucell_cache[idx],sz,idx);
        }
        if(g_brucell_cache[idx]){ win_draw_image(g_win,x,y,sz,sz,g_brucell_cache[idx]); return; }
    }
    render_brush_cell(buf,sz,idx);
    win_draw_image(g_win,x,y,sz,sz,buf);
}
// Render one pattern cell by tiling pattern_px() from the origin into `buf`.
static void render_pattern_cell(uint32_t *buf,int sz,int idx){
    for(int j=0;j<sz;j++)for(int i=0;i<sz;i++)
        buf[j*sz+i]=0xFF000000u|(pattern_px(idx,i,j)&0x00FFFFFFu);
}
static void draw_pattern_cell(int x,int y,int sz,int idx){
    static uint32_t buf[BPCELL*BPCELL];
    if(sz>BPCELL) sz=BPCELL; if(sz<1) return;
    // Cache stock pattern cells at the standard grid size; blit thereafter.
    if(idx>=0 && idx<BPCACHE_MAX && sz==BPCELL-2){
        if(!g_patcell_cache[idx]){
            g_patcell_cache[idx]=(uint32_t*)malloc((size_t)sz*sz*4);
            if(g_patcell_cache[idx]) render_pattern_cell(g_patcell_cache[idx],sz,idx);
        }
        if(g_patcell_cache[idx]){ win_draw_image(g_win,x,y,sz,sz,g_patcell_cache[idx]); return; }
    }
    render_pattern_cell(buf,sz,idx);
    win_draw_image(g_win,x,y,sz,sz,buf);
}

static void draw_dock(void){
    int dx=g_w-DOCK_W;
    R(dx,MENU_H,DOCK_W,g_h-MENU_H,C_PANEL);
    R(dx,MENU_H,1,g_h-MENU_H,C_LINE);
    ensure_thumbs();
    int y = MENU_H+8 - dock_scroll;

    // ---- Layers ----
    if(dock_header(dx,&y,SEC_LAYERS)){
        LP_y=y;
        const char *lb[6]={"+","D","-","^","v","M"};
        for(int i=0;i<6;i++) button(dx+8+i*29,y,26,20,lb[i],0,0);
        y+=24;
        int maxrows=g_doc.nlayers, shown=0;
        for(int li=g_doc.nlayers-1; li>=0 && shown<maxrows; li--, shown++){
            layer_t *L=&g_doc.layer[li];
            int rh=38, ry=y, active=(li==g_doc.active);
            if(active) vgrad(dx+8,ry,DOCK_W-16,rh, C_ACCENT2, C_ACCENT);
            else       vgrad(dx+8,ry,DOCK_W-16,rh, mix(C_PANEL2,0x00ffffff,6), C_PANEL2);
            OUT(dx+8,ry,DOCK_W-16,rh,C_LINE);
            R(dx+11,ry+1,THUMB+2,THUMB+2,C_LINE);
            if(thumb_buf[li]) win_draw_image(g_win,dx+12,ry+2,THUMB,THUMB,thumb_buf[li]);
            draw_eye(dx+50,ry+3,L->visible);
            uint32_t tc=active?0x00ffffff:C_TEXT;
            Ts(dx+72,ry+5,L->name,tc);
            if(L->mask) Ts(dx+DOCK_W-18,ry+5,"m",active?0x00ffffff:C_ACCENT2);
            char ob[8]; snprintf(ob,sizeof ob,"%d%%",L->opacity*100/255);
            Ts(dx+72,ry+21,ob,active?0x00d8e8f8:C_DIM);
            y+=rh+3;
        }
        // active-layer blend mode ("Mode:") control, in the layer row header
        LMODE_y=y;
        layer_t *ALb=&g_doc.layer[g_doc.active];
        Ts(dx+10,y+3,"Mode:",C_DIM);
        sbutton(dx+52,y,DOCK_W-62,18,blend_name(ALb->blend),blend_pop);
        { int cx2=dx+DOCK_W-18, cy2=y+9; uint32_t ac=blend_pop?0x00ffffff:C_TEXT;
          gline(cx2-3,cy2-1,cx2,cy2+2,ac); gline(cx2+3,cy2-1,cx2,cy2+2,ac); }
        y+=22;
        // active-layer opacity slider, wired to layer_set_opacity
        LO_y=y;
        layer_t *ALo=&g_doc.layer[g_doc.active];
        Ts(dx+10,y+2,"Opacity",C_DIM);
        slider(dx+62,y,DOCK_W-100, ALo->opacity, 255);
        { char ob[8]; snprintf(ob,sizeof ob,"%d%%",ALo->opacity*100/255); Ts(dx+DOCK_W-30,y+2,ob,C_TEXT); }
        y+=22;
        // mask row (active layer)
        LM_y=y;
        layer_t *AL=&g_doc.layer[g_doc.active];
        Ts(dx+10,y+3,"Mask",C_DIM);
        int has_mask = AL->mask!=0;
        sbutton(dx+90,y,24,18,"M+",has_mask);   // lit when the layer carries a mask
        sbutton(dx+116,y,24,18,"M!",0);
        sbutton(dx+142,y,24,18,"M-",0);
        sbutton(dx+168,y,18,18,AL->mask_active?"e":".",AL->mask_active);
        y+=20;
        sbutton(dx+8,y,46,16,AL->lock_alpha?"Lck A":"lck a",AL->lock_alpha);
        if(AL->mask) Ts(dx+60,y+2,AL->mask_active?"editing mask":"mask",C_DIM);
        y+=24;
    }

    // ---- Channels ----
    if(dock_header(dx,&y,SEC_CHANNELS)){
        // composite R/G/B/A channel rows (visibility eye + grayscale thumbnail)
        CH_y=y;
        const char *chn[4]={"Red","Green","Blue","Alpha"};
        for(int ci=0; ci<4; ci++){
            int ry=y, rh=18, hov=IN(g_mx,g_my,dx+6,ry,DOCK_W-12,rh);
            vgrad(dx+6,ry,DOCK_W-12,rh, mix(C_PANEL2,0x00ffffff,hov?12:4), C_PANEL2);
            OUT(dx+6,ry,DOCK_W-12,rh,C_LINE);
            draw_eye(dx+9,ry+1,g_chan_vis[ci]);
            R(dx+29,ry+1,CHTHUMB+2,CHTHUMB+2,C_LINE);
            if(g_chan_thumb[ci]) win_draw_image(g_win,dx+30,ry+2,CHTHUMB,CHTHUMB,g_chan_thumb[ci]);
            Ts(dx+52,ry+4,chn[ci], g_chan_vis[ci]?C_TEXT:C_DIM);
            y+=rh+1;
        }
        y+=6;
        // saved selections (stored channels)
        Ts(dx+10,y+2,"Saved selections",C_DIM);
        CHSB_y=y;
        sbutton(dx+DOCK_W-58,y,50,16,"Save Sel",0);
        y+=18;
        CHS_y=y;
        int nc=channel_count();
        for(int i=0;i<nc && i<3;i++){ Ts(dx+14,y,channel_name(i),C_TEXT); y+=14; }
        if(nc==0){ Ts(dx+14,y,"(none)",C_DIM); y+=14; }
        y+=6;
    }

    // ---- Paths ----
    if(dock_header(dx,&y,SEC_PATHS)){
        PA_y=y;
        char pb[16]; snprintf(pb,sizeof pb,"%d nodes",path_node_count()); Ts(dx+DOCK_W-58,y+2,pb,C_DIM);
        sbutton(dx+8,y,40,16,"Clear",0); sbutton(dx+52,y,50,16,"Stroke",0);
        y+=18;
        sbutton(dx+8,y,60,16,"To Sel",0);
        y+=22;
    }

    // ---- Histogram ----
    if(dock_header(dx,&y,SEC_HIST)){
        HI_y=y;
        const char*hcn[4]={"L","R","G","B"};
        sbutton(dx+DOCK_W-108,y,32,16, hist_log?"Log":"Lin", hist_log);   // scale toggle
        for(int i=0;i<4;i++) sbutton(dx+DOCK_W-70+i*17,y,15,16,hcn[i], hist_ch==i);
        y+=18;
        // Cache the 256-bin histogram; recomputed only when the composite changed
        // (g_hist_dirty is raised from comp_dirty in blit_canvas) or channel switched.
        if(g_hist_dirty || g_hist_ch_cache!=hist_ch){ histogram(hist_ch,g_hist_cache); g_hist_ch_cache=hist_ch; g_hist_dirty=0; }
        unsigned int hmax=1; for(int i=0;i<256;i++) if(g_hist_cache[i]>hmax) hmax=g_hist_cache[i];
        int lmax = ilog2s(hmax);
        int gw=DOCK_W-20, gh=44, gx=dx+10, gy=y;
        R(gx,gy,gw,gh,C_PANEL2); OUT(gx,gy,gw,gh,C_LINE);
        for(int c=0;c<gw;c++){
            // Accumulate the MAX over every bin mapped to this column so no bin is
            // dropped when the graph is narrower than 256 px.
            int b0=c*256/gw, b1=(c+1)*256/gw; if(b1<=b0) b1=b0+1; if(b1>256) b1=256;
            unsigned int v=0; for(int b=b0;b<b1;b++) if(g_hist_cache[b]>v) v=g_hist_cache[b];
            int bh = hist_log ? (v>0 ? ilog2s(v)*(gh-2)/(lmax>0?lmax:1) : 0)
                              : (int)((unsigned long long)v*(gh-2)/hmax);
            if(bh>0) R(gx+c,gy+gh-1-bh,1,bh,C_ACCENT2);
        }
        y+=gh+6;
    }

    // ---- History ----
    if(dock_header(dx,&y,SEC_HISTORY)){
        HP_y=y;
        int n=undo_count(), start=n>4?n-4:0;
        for(int i=start;i<n;i++){ Ts(dx+12,y,undo_label(i),(i==n-1)?C_TEXT:C_DIM); y+=14; }
        if(n==0){ Ts(dx+12,y,"(empty)",C_DIM); y+=14; }
        y+=6;
    }

    // ---- Color (professional picker, colorpick.c) ----
    if(dock_header(dx,&y,SEC_COLOR)){
        CP_y=y;
        cp_draw_dock(dx, y);
        y += cp_dock_height();
    }

    // ---- Swatches / palettes (colorpick.c) ----
    if(dock_header(dx,&y,SEC_SWATCHES)){
        SW_y=y;
        cp_draw_pal(dx, y);
        y += cp_pal_height();
    }

    // ---- Brushes (parametric round + all stock bitmap brushes) ----
    if(dock_header(dx,&y,SEC_BRUSHES)){
        BR_y=y;
        int total=1+brush_count();           // cell 0 = parametric round brush
        int cur=brush_current();             // -1 == parametric round
        int rows=(total+BPCOLS-1)/BPCOLS;
        BRG_y0=y; BRG_y1=y+rows*BPSTRIDE;    // grid body rect (no hover feedback)
        for(int c=0;c<total;c++){
            int idx=c-1;                     // cell 0 -> -1 (parametric)
            int cxp=dx+8+(c%BPCOLS)*BPSTRIDE, cyp=y+(c/BPCOLS)*BPSTRIDE;
            R(cxp,cyp,BPCELL,BPCELL,C_PANEL2);
            draw_brush_cell(cxp+1,cyp+1,BPCELL-2,idx);
            OUT(cxp,cyp,BPCELL,BPCELL, idx==cur?C_ACCENT:C_LINE);
        }
        y += rows*BPSTRIDE + 6;
    } else { BRG_y0=BRG_y1=0; }

    // ---- Patterns (none + all stock tiling patterns) ----
    if(dock_header(dx,&y,SEC_PATTERNS)){
        PT_y=y;
        int total=1+pattern_count();         // cell 0 = "none"
        int cur=pattern_current();           // -1 == none
        int rows=(total+BPCOLS-1)/BPCOLS;
        PTG_y0=y; PTG_y1=y+rows*BPSTRIDE;    // grid body rect (no hover feedback)
        for(int c=0;c<total;c++){
            int idx=c-1;                     // cell 0 -> -1 (none)
            int cxp=dx+8+(c%BPCOLS)*BPSTRIDE, cyp=y+(c/BPCOLS)*BPSTRIDE;
            R(cxp,cyp,BPCELL,BPCELL,C_PANEL2);
            if(idx<0){                       // "none": diagonal slash on the panel
                for(int k=0;k<BPCELL-2;k++) R(cxp+1+k,cyp+BPCELL-2-k,1,1,C_DIM);
            }else{
                draw_pattern_cell(cxp+1,cyp+1,BPCELL-2,idx);
            }
            OUT(cxp,cyp,BPCELL,BPCELL, idx==cur?C_ACCENT:C_LINE);
        }
        y += rows*BPSTRIDE + 6;
    } else { PTG_y0=PTG_y1=0; }

    // ---- content metrics + scrollbar ----
    dock_content_h = (y+dock_scroll) - MENU_H;
    int vis = g_h-MENU_H;
    dock_max_scroll = dock_content_h - vis + 8;
    if(dock_max_scroll<0) dock_max_scroll=0;
    if(dock_scroll>dock_max_scroll) dock_scroll=dock_max_scroll;
    if(dock_max_scroll>0){
        int tx=g_w-4, ty=MENU_H+2, th=g_h-MENU_H-4;
        R(tx,ty,3,th,C_PANEL2);
        int tb=th*vis/dock_content_h; if(tb<20)tb=20; if(tb>th)tb=th;
        int typ=ty + (dock_scroll*(th-tb))/dock_max_scroll;
        R(tx,typ,3,tb,C_ACCENT);
    }
}

// Recompute the selection bounding box (cached; only rescanned when marked
// dirty by a full redraw, so mouse-hover status repaints stay cheap).
static void update_sel_bbox(void){
    g_selbb_on=0;
    if(!g_doc.sel_active || !g_doc.sel) return;
    int W=g_doc.w,H=g_doc.h, minx=W,miny=H,maxx=-1,maxy=-1;
    for(int y=0;y<H;y++){ const uint8_t*rowp=&g_doc.sel[y*W];
        for(int x=0;x<W;x++) if(rowp[x]>127){ if(x<minx)minx=x; if(x>maxx)maxx=x; if(y<miny)miny=y; if(y>maxy)maxy=y; } }
    if(maxx>=minx){ g_selbb_on=1; g_selbb_w=maxx-minx+1; g_selbb_h=maxy-miny+1; }
}
// Zoned status bar: left=action message (untouched by mouse), center=cursor x/y
// + colour chip, right=tool | granular zoom cluster | selection.
// Zoom cluster (item 2, drawn right-to-left): [readout%] [-] [==slider==] [+]
// [Fit] [1:1]. The slider drags continuously ZOOM_MIN..ZOOM_MAX; the steppers and
// Fit/1:1 snap. All rects are recorded here and hit-tested in click_status().
static void draw_status(void){
    int y=g_h-STATUS_H;
    if(g_selbb_dirty){ update_sel_bbox(); g_selbb_dirty=0; }
    vgrad(0,y,g_w,STATUS_H, C_PANEL, C_PANEL2);
    R(0,y,g_w,1,C_LINE);
    // left: last action message
    T(10,y+3,g_status,C_TEXT);
    // center: cursor coordinates + colour chip under the pointer
    if(g_cur_valid){
        char cb[40]; snprintf(cb,sizeof cb,"X %d   Y %d",g_cur_x,g_cur_y);
        int cw=text_w(cb)+22, cx=g_w/2 - cw/2;
        R(cx,y+4,14,14,g_cur_col); OUT(cx,y+4,14,14,C_LINE);
        T(cx+20,y+3,cb,C_DIM);
    }
    char sb[32]; if(g_selbb_on) snprintf(sb,sizeof sb,"Sel %dx%d",g_selbb_w,g_selbb_h); else sb[0]=0;
    const char *rt=tool_name(g_tool.id);
    int pad=10, wt=text_w(rt), ws=sb[0]?text_w(sb):0;
    int rx=g_w-10;
    if(sb[0]){ rx-=ws; T(rx,y+3,sb,C_DIM); rx-=pad; R(rx+pad/2,y+3,1,STATUS_H-5,C_LINE); }
    // ---- zoom cluster (right-to-left) ----
    int by=y+3, bh=STATUS_H-6; zc_y=by; zc_h=bh;
    int b2w=26, bsw=16; zc_btn_w=b2w; zc_step_w=bsw;
    rx-=b2w; zc_100_x=rx; sbutton(rx,by,b2w,bh,"1:1", g_zoom_pct==100);
    rx-=4;
    rx-=b2w; zc_fit_x=rx; sbutton(rx,by,b2w,bh,"Fit", 0);
    rx-=6;
    rx-=bsw; zc_plus_x=rx; sbutton(rx,by,bsw,bh,"+",0);
    rx-=3;
    int slw=92; rx-=slw; zc_slider_x=rx; zc_slider_w=slw;
    { int cy=by+bh/2;
      R(rx,cy-1,slw,3,C_LINE); bevel(rx,cy-1,slw,3,C_PANEL2,mix(C_PANEL,0x00ffffff,20));
      int fill=(g_zoom_pct-ZOOM_MIN)*(slw-8)/(ZOOM_MAX-ZOOM_MIN);
      vgrad(rx,cy-1,fill+4,3,C_ACCENT2,C_ACCENT);
      int kx=rx+(g_zoom_pct-ZOOM_MIN)*(slw-8)/(ZOOM_MAX-ZOOM_MIN);
      vgrad(kx,by,8,bh,mix(C_BTN_HOV,0x00ffffff,40),C_BTN);
      bevel(kx,by,8,bh,mix(C_BTN_HOV,0x00ffffff,70),C_LINE); }
    rx-=3;
    rx-=bsw; zc_minus_x=rx; sbutton(rx,by,bsw,bh,"-",0);
    rx-=4;
    char zb[16]; snprintf(zb,sizeof zb,"%d%%",g_zoom_pct);
    int zbw=text_w(zb)+8; rx-=zbw; g_zoom_bx=rx; g_zoom_by=by; g_zoom_bw=zbw; g_zoom_bh=bh;
    field_inset(rx,by,zbw,bh); Ts(rx+4,y+4,zb,C_TEXT);
    rx-=pad; R(rx+pad/2,y+3,1,STATUS_H-5,C_LINE);
    rx-=wt; T(rx,y+3,rt,C_DIM);
}

// ---------------------------------------------------------------------------
// Dropdown menus (2-level for Filters)
// ---------------------------------------------------------------------------
static void menu_label(int mi, int idx, char *out, int cap){
    menu_t *m=&MENUS[mi];
    if(m->kind==MK_STATIC){
        int act=m->items[idx].action;
        if(act==A_SEP){ out[0]=0; return; }
        // Dynamic Undo/Redo labels reflect what the next operation would affect.
        if(act==A_UNDO || act==A_REDO){
            const char*verb=(act==A_UNDO)?"Undo":"Redo";
            const char*what=(act==A_UNDO)?undo_next_label():redo_next_label();
            if(what && what[0]) snprintf(out,cap,"%s %s",verb,what);
            else               snprintf(out,cap,"%s",verb);
            return;
        }
        int i=0; const char*s=m->items[idx].label; while(s[i]&&i<cap-1){out[i]=s[i];i++;} out[i]=0; return; }
    if(m->kind==MK_COLORS){ int gi=nth_color_op(idx); const studio_op_t*o=gi>=0?studio_op_get(gi):0; int i=0; const char*s=o?o->name:"?"; while(s[i]&&i<cap-1){out[i]=s[i];i++;} out[i]=0; return; }
    // MK_FILTERS: category names
    int i=0; const char*s=(idx<g_ncats)?g_cats[idx]:"?"; while(s[i]&&i<cap-1){out[i]=s[i];i++;} out[i]=0;
}
// Right-aligned shortcut hint for a menu row, or NULL. Only static menus carry one.
static const char *menu_short(int mi,int idx){
    menu_t *m=&MENUS[mi];
    if(m->kind!=MK_STATIC) return 0;
    if(m->items[idx].action==A_SEP) return 0;
    return m->items[idx].sc;
}
// Dropdown width: label column + optional shortcut column, single-sourced so
// draw_dropdown() and click_dropdown() (and hover-open) agree on the geometry.
#define MENU_LEFT   22       // left indent (check/icon gutter + label)
#define MENU_RIGHTPAD 14     // right margin after the shortcut column
static int dropdown_w(int mi){
    menu_t *m=&MENUS[mi];
    int lw=0, sw=0; char lbl[48];
    for(int i=0;i<m->n;i++){
        menu_label(mi,i,lbl,sizeof lbl); int w=gui_string_width(lbl); if(w>lw) lw=w;
        const char *s=menu_short(mi,i); if(s){ int w2=gui_string_width(s); if(w2>sw) sw=w2; }
    }
    int w = MENU_LEFT + lw + (sw? (18+sw) : 0) + MENU_RIGHTPAD;
    if(m->kind==MK_FILTERS) w += 12;    // room for the ">" submenu arrow
    if(w<150) w=150;
    return w;
}

static void draw_dropdown(void){
    if(menu_open<0) return;
    menu_t *m=&MENUS[menu_open];
    int x=menu_x(menu_open), y=MENU_H;
    int n=m->n; if(n<=0){ return; }
    int w=dropdown_w(menu_open);
    char lbl[48];
    int h=n*20+6;
    R(x+2,y+h,w,3, mix(C_LINE,0x00000000,40));          // soft drop shadow
    panelg(x,y,w,h);
    for(int i=0;i<n;i++){
        int ry=y+2+i*20;
        int act=(m->kind==MK_STATIC)?m->items[i].action:A_NONE;
        if(act==A_SEP){ R(x+8,ry+9,w-16,1,C_LINE); continue; }   // divider row
        menu_label(menu_open,i,lbl,sizeof(lbl));
        int dis=(m->kind==MK_STATIC)&&action_disabled(act);
        if(!dis && IN(g_mx,g_my,x,ry,w,20)) R(x+1,ry,w-2,20,C_BTN_HOV);
        if((m->kind==MK_STATIC)&&action_checked(act)){       // checkmark on toggles
            int cxp=x+8, cyy=ry+10; gline(cxp,cyy,cxp+3,cyy+3,C_ACCENT2); gline(cxp+3,cyy+3,cxp+8,cyy-3,C_ACCENT2);
        }
        T(x+MENU_LEFT, y+4+i*20, lbl, dis?C_DIM:C_TEXT);
        // right-aligned shortcut hint (kept in its own column so rows line up)
        const char *sc=menu_short(menu_open,i);
        if(sc){ int scw=gui_string_width(sc); Ts(x+w-MENU_RIGHTPAD-scw, y+6+i*20, sc, dis?C_LINE:C_DIM); }
        if(m->kind==MK_FILTERS) Ts(x+w-12,y+6+i*20,">",C_DIM);
    }
    // Filters submenu
    if(m->kind==MK_FILTERS && submenu_cat>=0 && submenu_cat<g_ncats){
        const char*cat=g_cats[submenu_cat];
        int cn=cat_op_count(cat);
        int sx=x+w+2, sy=y+submenu_cat*20;
        int sw=0; for(int i=0;i<cn;i++){ int gi=nth_cat_op(cat,i); const studio_op_t*o=gi>=0?studio_op_get(gi):0; if(o){int lw=gui_string_width(o->name); if(lw>sw)sw=lw;} }
        sw+=24; if(sw<120)sw=120; int sh=cn*20+6;
        R(sx+2,sy+sh,sw,3, mix(C_LINE,0x00000000,40));
        panelg(sx,sy,sw,sh);
        for(int i=0;i<cn;i++){ int gi=nth_cat_op(cat,i); const studio_op_t*o=gi>=0?studio_op_get(gi):0;
            int ry=sy+2+i*20; if(IN(g_mx,g_my,sx,ry,sw,20)) R(sx+1,ry,sw-2,20,C_BTN_HOV);
            if(o) T(sx+10,sy+4+i*20,o->name,C_TEXT); }
    }
}

// Blend-mode popup, grouped by GIMP mode families with a check on the current mode.
typedef struct { const char *group; const blend_t *modes; int n; } blendgrp_t;
static const blend_t BG_NORMAL[]  = { BLEND_NORMAL, BLEND_DISSOLVE };
static const blend_t BG_LIGHTEN[] = { BLEND_LIGHTEN, BLEND_SCREEN, BLEND_DODGE, BLEND_ADD, BLEND_LUMA_LIGHTEN };
static const blend_t BG_DARKEN[]  = { BLEND_DARKEN, BLEND_MULTIPLY, BLEND_BURN, BLEND_LUMA_DARKEN };
static const blend_t BG_CONTRAST[]= { BLEND_OVERLAY, BLEND_SOFTLIGHT, BLEND_HARDLIGHT, BLEND_VIVIDLIGHT, BLEND_PINLIGHT, BLEND_LINEARLIGHT, BLEND_HARDMIX };
static const blend_t BG_INVERT[]  = { BLEND_DIFFERENCE, BLEND_EXCLUSION, BLEND_GRAINEXTRACT, BLEND_GRAINMERGE };
static const blend_t BG_ARITH[]   = { BLEND_SUBTRACT, BLEND_DIVIDE };
static const blend_t BG_COMP[]    = { BLEND_HUE, BLEND_SATURATION, BLEND_COLOR, BLEND_VALUE };
static const blendgrp_t BLEND_GROUPS[] = {
    {"Normal",     BG_NORMAL,   2}, {"Lighten",    BG_LIGHTEN,  5},
    {"Darken",     BG_DARKEN,   4}, {"Contrast",   BG_CONTRAST, 7},
    {"Inversion",  BG_INVERT,   4}, {"Arithmetic", BG_ARITH,    2},
    {"Components", BG_COMP,     4},
};
#define BLEND_NGROUPS 7
// Shared geometry so draw + hit-test never diverge. Returns the row height.
static int blend_pop_geom(int *ox,int *oy,int *ow,int *oh){
    int rowh=14, w=158;
    int nrows=BLEND_NGROUPS; for(int g=0;g<BLEND_NGROUPS;g++) nrows+=BLEND_GROUPS[g].n;
    int h=nrows*rowh+6;
    int x=blend_pop_x, y=blend_pop_y;
    if(y+h>g_h) y=g_h-h; if(y<MENU_H) y=MENU_H;
    if(x+w>g_w) x=g_w-w; if(x<0) x=0;
    *ox=x; *oy=y; *ow=w; *oh=h; return rowh;
}
static void draw_blend_pop(void){
    if(!blend_pop) return;
    int x,y,w,h; int rowh=blend_pop_geom(&x,&y,&w,&h);
    vgrad(x,y,w,h, mix(C_BTN,0x00ffffff,10), C_BTN);
    bevel(x,y,w,h, mix(C_BTN_HOV,0x00ffffff,45), C_LINE);
    OUT(x,y,w,h,C_LINE);
    blend_t cur=g_doc.layer[g_doc.active].blend;
    int ry=y+3;
    for(int g=0;g<BLEND_NGROUPS;g++){
        R(x+2,ry+rowh-1,w-4,1,C_LINE);
        Ts(x+7,ry+1, BLEND_GROUPS[g].group, C_DIM);
        ry+=rowh;
        for(int i=0;i<BLEND_GROUPS[g].n;i++){
            blend_t b=BLEND_GROUPS[g].modes[i];
            if(IN(g_mx,g_my,x,ry,w,rowh)) R(x+1,ry,w-2,rowh,C_BTN_HOV);
            if(b==cur){ int cx=x+11, cyy=ry+rowh/2; gline(cx-3,cyy,cx-1,cyy+2,C_ACCENT2); gline(cx-1,cyy+2,cx+3,cyy-3,C_ACCENT2); }
            Ts(x+22,ry+1, blend_name(b), (b==cur)?C_ACCENT2:C_TEXT);
            ry+=rowh;
        }
    }
}

// ---------------------------------------------------------------------------
// Generic parameter dialog
// ---------------------------------------------------------------------------
// --- integer trig for the SP_ANGLE dial + on-canvas handles (screen space,
// y-down; consistent with fx_sin/fx_cos used by the filters). Q12 output. ------
static int ui_sin(int deg){ deg%=360; if(deg<0)deg+=360; int s=1; if(deg>180){deg-=180;s=-1;}
    long num=4L*deg*(180-deg); long den=40500L-(long)deg*(180-deg); if(den==0)den=1; return (int)(s*(num*4096/den)); }
static int ui_cos(int deg){ return ui_sin(deg+90); }
static int ui_atan2(int dy,int dx){ if(dx==0&&dy==0)return 0; int ax=dx<0?-dx:dx, ay=dy<0?-dy:dy; long a;
    if(ax>=ay)a=(long)ay*45/(ax?ax:1); else a=90-(long)ax*45/(ay?ay:1); int ang=(int)a;
    if(dx<0&&dy>=0)ang=180-ang; else if(dx<0&&dy<0)ang=180+ang; else if(dx>=0&&dy<0)ang=360-ang; return ((ang%360)+360)%360; }
static void ui_line(int x0,int y0,int x1,int y1,uint32_t c){ int dx=x1-x0,dy=y1-y0; int adx=dx<0?-dx:dx,ady=dy<0?-dy:dy;
    int n=adx>ady?adx:ady; if(n<1)n=1; for(int i=0;i<=n;i++){ int x=x0+dx*i/n, y=y0+dy*i/n; R(x,y,2,2,c); } }
static void ui_circle(int cx,int cy,int r,uint32_t c){ for(int a=0;a<360;a+=4){ int x=cx+ui_cos(a)*r/4096, y=cy+ui_sin(a)*r/4096; R(x,y,2,2,c); } }

// ===========================================================================
// SP_CURVE tone-curve editor state (ui.c owns it, like colorpick owns SP_COLOR).
// 4 channels: 0=RGB (composite, applied to all channels first), 1=R,2=G,3=B.
// Control points (x,y) 0..255, kept sorted by x, endpoints always present.
// ===========================================================================
#define CV_MAXPTS 16
static int cvw_px[4][CV_MAXPTS], cvw_py[4][CV_MAXPTS], cvw_n[4];
static int cvw_chan = 0;     // active channel tab
static int cvw_sel  = -1;    // selected/dragged control point in the active channel
static void curve_reset_all(void){
    for(int c=0;c<4;c++){ cvw_px[c][0]=0; cvw_py[c][0]=0; cvw_px[c][1]=255; cvw_py[c][1]=255; cvw_n[c]=2; }
    cvw_chan=0; cvw_sel=-1;
}
static void curve_chan_lut(int c,int *lut){
    int n=cvw_n[c]; if(n<2){ for(int i=0;i<256;i++)lut[i]=i; return; }
    for(int i=0;i<n-1;i++){
        int x0=cvw_px[c][i], y0=cvw_py[c][i], x1=cvw_px[c][i+1], y1=cvw_py[c][i+1];
        if(x1<=x0) x1=x0+1;
        for(int x=x0;x<=x1 && x<256;x++){ if(x<0)continue; int y=y0+(y1-y0)*(x-x0)/(x1-x0); lut[x]=clampi(y,0,255); }
    }
    for(int x=0;x<cvw_px[c][0] && x<256;x++) lut[x]=clampi(cvw_py[c][0],0,255);
    for(int x=cvw_px[c][n-1];x<256;x++) if(x>=0) lut[x]=clampi(cvw_py[c][n-1],0,255);
}
// Public (studio.h): composite the RGB curve then the per-channel curves.
void curve_get_lut3(int *lr,int *lg,int *lb){
    int base[256], cr[256], cg[256], cb[256];
    curve_chan_lut(0,base); curve_chan_lut(1,cr); curve_chan_lut(2,cg); curve_chan_lut(3,cb);
    for(int i=0;i<256;i++){ int v=clampi(base[i],0,255); lr[i]=cr[v]; lg[i]=cg[v]; lb[i]=cb[v]; }
}

// ===========================================================================
// SP_KERNEL 5x5 convolution-grid editor state (ui.c owns it, SP_CURVE pattern).
// Cells are signed -999..999, row-major. Ops read them via kernel_get25(); the
// SP_KERNEL slot in p[] is a placeholder. Reset restores the default sharpen
// matrix (center 5, orthogonals -1), never all-zeros (blueprint rule).
// ===========================================================================
static int kw_cell[25];
static int kw_sel = 12;         // selected cell (defaults to the center)
static int kw_drag_y0 = 0;      // vertical value-drag anchor
static int kw_drag_v0 = 0;
static int kw_inited = 0;
void kernel_reset(void){
    for(int i=0;i<25;i++) kw_cell[i]=0;
    kw_cell[12]=5; kw_cell[7]=-1; kw_cell[11]=-1; kw_cell[13]=-1; kw_cell[17]=-1;
    kw_sel=12; kw_inited=1;
}
void kernel_get25(int *k25){
    if(!kw_inited) kernel_reset();
    for(int i=0;i<25;i++) k25[i]=kw_cell[i];
}
// Starting matrices (retired fx_generic "Convolution Matrix" presets, 3x3
// embedded in the 5x5 center) + their Scale/Offset companions.
static const int KW_P3[5][9]={
    {  0,-1, 0,-1, 5,-1, 0,-1, 0 },     // Sharpen
    { -1,-1,-1,-1, 8,-1,-1,-1,-1 },     // Edge
    { -2,-1, 0,-1, 1, 1, 0, 1, 2 },     // Emboss
    {  1, 1, 1, 1, 1, 1, 1, 1, 1 },     // Blur
    { -1,-1,-1,-1, 8,-1,-1,-1,-1 },     // Outline
};
static const int KW_PDIV[5]={1,1,1,9,1}, KW_POFF[5]={0,0,128,0,0};
static void kernel_seed_preset(int t){
    if(t<0)t=0; if(t>4)t=4;
    for(int i=0;i<25;i++) kw_cell[i]=0;
    for(int r=0;r<3;r++) for(int c=0;c<3;c++) kw_cell[(r+1)*5+(c+1)]=KW_P3[t][r*3+c];
    kw_sel=12; kw_inited=1;
}
// Does the currently open op carry an SP_KERNEL param? (keyboard gate)
static int pd_has_kernel(void){
    const studio_op_t*op=studio_op_get(pd_opi); if(!op) return 0;
    for(int k=0;k<pd_np;k++) if(op->params[k].type==SP_KERNEL) return 1;
    return 0;
}

// --- Row layout. Handle-coordinate params render as one compact hint row (the
// position is set by dragging on the canvas); the Y coord of a pair is hidden. -
static int pd_handle_role(int k){    // 0 none, 1 = X (compact hint), 2 = Y (hidden)
    const studio_op_t*op=studio_op_get(pd_opi); if(!op) return 0;
    for(int hh=0; hh<op->nhandles && hh<2; hh++){ if(k==op->handle_xp[hh]) return 1; if(k==op->handle_yp[hh]) return 2; }
    return 0;
}
static int pd_row_h_k(int k){
    const studio_op_t*op=studio_op_get(pd_opi); if(!op) return 30;
    int role=pd_handle_role(k); if(role==2) return 0; if(role==1) return 26;
    switch(op->params[k].type){ case SP_COLOR: return 46; case SP_ANGLE: return 74; case SP_CURVE: return 244; case SP_KERNEL: return 180; default: return 30; }
}
// Vertical space the preview pane consumes at the top of the dialog (0 if none).
// pd_pv_h for the pane + a hint line + a zoom/pan control row.
static int pd_pane_area(void){ return (pd_region && pd_pv_h>0) ? pd_pv_h + 50 : 0; }
static int pd_h(void){
    const studio_op_t*op=studio_op_get(pd_opi); if(!op) return 120;
    int h=54 + pd_pane_area(); for(int k=0;k<pd_np;k++) h+=pd_row_h_k(k); h+=44; return h;
}
static int pd_has_handles(void){ const studio_op_t*op=studio_op_get(pd_opi); return op && op->nhandles>0; }
static int pd_bx(void){
    if(pd_has_handles()){ int x=g_w-DOCK_W-360-16; if(x<cv_x()+8) x=cv_x()+8; return x; }
    return (g_w-360)/2;
}
static int pd_by(void){ int by=(g_h-pd_h())/2; if(by<MENU_H+OPT_H+4) by=MENU_H+OPT_H+4; return by; }
// Screen y (row baseline) of param k, matching the draw loop.
static int pd_row_top(int k){ int y=pd_by()+40+pd_pane_area(); for(int i=0;i<k && i<pd_np;i++) y+=pd_row_h_k(i); return y; }
// Geometry of the SP_ANGLE dial / SP_CURVE graph for a given param row.
static void pd_dial_geom(int k,int*cx,int*cy,int*rad){ int bx=pd_bx(); *cx=bx+360-70; *cy=pd_row_top(k)+30; *rad=26; }
static void pd_curve_geom(int k,int*gx,int*gy,int*gs){ int bx=pd_bx(); *gs=196; *gx=bx+(360-*gs)/2; *gy=pd_row_top(k)+26; }
static int  pd_curve_k = -1;   // param index of the SP_CURVE currently being dragged

static void pd_backup_make(void){
    int W=g_doc.w,H=g_doc.h; pd_backup_layer=g_doc.active;
    if(pd_backup) free(pd_backup);
    pd_backup=(uint32_t*)malloc((size_t)W*H*4);
    if(pd_backup) memcpy(pd_backup, g_doc.layer[g_doc.active].px, (size_t)W*H*4);
    if(pd_maskbk){ free(pd_maskbk); pd_maskbk=0; }
    if(g_doc.layer[g_doc.active].mask){ pd_maskbk=(uint8_t*)malloc((size_t)W*H); if(pd_maskbk) memcpy(pd_maskbk,g_doc.layer[g_doc.active].mask,(size_t)W*H); }
}
static void pd_restore(void){
    int W=g_doc.w,H=g_doc.h;
    if(pd_backup && pd_backup_layer==g_doc.active) memcpy(g_doc.layer[g_doc.active].px, pd_backup, (size_t)W*H*4);
    if(pd_maskbk && g_doc.layer[g_doc.active].mask) memcpy(g_doc.layer[g_doc.active].mask,pd_maskbk,(size_t)W*H);
    g_doc.comp_dirty=1;
}
static void pd_backup_free(void){ if(pd_backup){free(pd_backup);pd_backup=0;} if(pd_maskbk){free(pd_maskbk);pd_maskbk=0;} pd_backup_layer=-1; }

// --- In-dialog preview pane (loupe) -----------------------------------------
static void pd_region_free(void){
    if(pd_region){ free(pd_region); pd_region=0; }
    if(pd_regwork){ free(pd_regwork); pd_regwork=0; }
    pd_reg_w=pd_reg_h=0;
}
static void pd_pane_free(void){
    pd_region_free();
    if(pd_pvwork){ free(pd_pvwork); pd_pvwork=0; }
    pd_pv_w=pd_pv_h=0;
}
// Fixed on-screen display box: aspect-fit the WHOLE layer inside PV_BOXW x
// PV_BOXH (so "Fit" shows the whole image undistorted). The loupe always samples
// a region of THIS box's aspect, so upscaled/downscaled views never distort.
static void pd_box_geom(int *pw,int *ph){
    int W=g_doc.w, H=g_doc.h; if(W<1)W=1; if(H<1)H=1;
    int w=PV_BOXW, h=(int)((long)H*PV_BOXW/W);
    if(h>PV_BOXH){ h=PV_BOXH; w=(int)((long)W*PV_BOXH/H); }
    if(w<1)w=1; if(w>W)w=W; if(h<1)h=1; if(h>H)h=H;
    *pw=w; *ph=h;
}
// Re-render the pane: copy the pristine region crop, route the op onto it, then
// scale (nearest) into the display box. Cheap: one small region. Never touches
// the real layer. Selection disabled so the whole window shows the effect.
static void pd_pane_render(void){
    const studio_op_t*op=studio_op_get(pd_opi);
    if(!op||!op->apply||!pd_region||!pd_regwork||!pd_pvwork) return;
    if(pd_reg_w<1||pd_reg_h<1||pd_pv_w<1||pd_pv_h<1) return;
    memcpy(pd_regwork, pd_region, (size_t)pd_reg_w*pd_reg_h*4);
    int save_sel=g_doc.sel_active; g_doc.sel_active=0;
    draw_preview_target_set(pd_regwork, pd_reg_w, pd_reg_h);
    op->apply(pd_val);
    draw_preview_target_clear();
    g_doc.sel_active=save_sel;
    // Scale the region (pd_reg_w x pd_reg_h) into the display box (pd_pv_w x
    // pd_pv_h). Upscale (zoom>1), 1:1, or downscale (Fit) all fall out of this.
    for(int y=0;y<pd_pv_h;y++){
        int sy=(int)((long)y*pd_reg_h/pd_pv_h); if(sy>=pd_reg_h)sy=pd_reg_h-1;
        const uint32_t *srow=&pd_regwork[(size_t)sy*pd_reg_w];
        uint32_t *drow=&pd_pvwork[(size_t)y*pd_pv_w];
        for(int x=0;x<pd_pv_w;x++){ int sx=(int)((long)x*pd_reg_w/pd_pv_w); if(sx>=pd_reg_w)sx=pd_reg_w-1; drow[x]=srow[sx]; }
    }
}
// Build the pristine region crop from pd_backup for the current zoom+center.
// The region is a window of pd_pv_w*100/zoom by pd_pv_h*100/zoom SOURCE pixels
// (clamped to the layer), centered on (pd_cx,pd_cy) with the center clamped so
// the window stays inside the image. (Re)allocs region buffers only when the
// region size changes; always recopies (center may have moved). Then renders.
static void pd_region_build(void){
    int W=g_doc.w, H=g_doc.h; if(W<1||H<1||pd_pv_w<1||pd_pv_h<1) return;
    if(pd_zoom<1)pd_zoom=1;
    int rw=(int)((long)pd_pv_w*100/pd_zoom); if(rw<1)rw=1; if(rw>W)rw=W;
    int rh=(int)((long)pd_pv_h*100/pd_zoom); if(rh<1)rh=1; if(rh>H)rh=H;
    // Clamp center so [cx-rw/2 .. cx+rw/2] stays in [0,W]; same for Y.
    if(rw>=W){ pd_cx=W/2; pd_reg_x=0; }
    else { if(pd_cx<rw/2)pd_cx=rw/2; if(pd_cx>W-rw/2)pd_cx=W-rw/2; pd_reg_x=pd_cx-rw/2; if(pd_reg_x<0)pd_reg_x=0; if(pd_reg_x>W-rw)pd_reg_x=W-rw; }
    if(rh>=H){ pd_cy=H/2; pd_reg_y=0; }
    else { if(pd_cy<rh/2)pd_cy=rh/2; if(pd_cy>H-rh/2)pd_cy=H-rh/2; pd_reg_y=pd_cy-rh/2; if(pd_reg_y<0)pd_reg_y=0; if(pd_reg_y>H-rh)pd_reg_y=H-rh; }
    if(rw!=pd_reg_w||rh!=pd_reg_h||!pd_region||!pd_regwork){
        pd_region_free();
        pd_region=(uint32_t*)malloc((size_t)rw*rh*4);
        pd_regwork=(uint32_t*)malloc((size_t)rw*rh*4);
        if(!pd_region||!pd_regwork){ pd_region_free(); return; }
        pd_reg_w=rw; pd_reg_h=rh;
    }
    const uint32_t *lp = pd_backup ? pd_backup : g_doc.layer[g_doc.active].px;
    for(int y=0;y<rh;y++){
        const uint32_t *srow=&lp[(size_t)(pd_reg_y+y)*W + pd_reg_x];
        memcpy(&pd_region[(size_t)y*rw], srow, (size_t)rw*4);
    }
    pd_pane_render();
}
// Set up the loupe when the dialog opens: display box, 1:1 zoom, image center.
static void pd_pane_setup(void){
    pd_pane_free();
    int W=g_doc.w, H=g_doc.h; if(W<1||H<1) return;
    if(g_doc.active<0||g_doc.active>=g_doc.nlayers) return;
    int pw,ph; pd_box_geom(&pw,&ph);
    pd_pvwork=(uint32_t*)malloc((size_t)pw*ph*4);
    if(!pd_pvwork){ pd_pane_free(); return; }
    pd_pv_w=pw; pd_pv_h=ph;
    pd_zoom=100; pd_cx=W/2; pd_cy=H/2;   // start at actual pixels, centered
    pd_loupe_drag=0;
    pd_region_build();
}
// Zoom stepping through PD_ZOOMS (dir<0 out, dir>0 in), re-centering the region.
static void pd_zoom_step(int dir){
    int z=pd_zoom, nz=z;
    if(dir<0){ nz=PD_ZOOMS[0]; for(int i=0;i<PD_NZOOM;i++) if(PD_ZOOMS[i]<z) nz=PD_ZOOMS[i]; }
    else { nz=PD_ZOOMS[PD_NZOOM-1]; for(int i=PD_NZOOM-1;i>=0;i--) if(PD_ZOOMS[i]>z) nz=PD_ZOOMS[i]; }
    pd_zoom=nz; pd_region_build();
}
// "Fit": pick the largest zoom whose region covers the whole layer (region==box
// image, i.e. the old whole-image thumbnail). "1:1": exactly actual pixels.
static void pd_zoom_fit(void){
    int W=g_doc.w; if(W<1)W=1;
    int z=(int)((long)pd_pv_w*100/W); if(z<1)z=1; if(z>100)z=100;
    pd_zoom=z; pd_region_build();
}
static void pd_zoom_actual(void){ pd_zoom=100; pd_region_build(); }
// Geometry of the pane box and the zoom/pan control row inside the dialog.
static void pd_pane_geom(int bx,int by,int bw,int *px0,int *py0){
    *px0=bx+(bw-pd_pv_w)/2; *py0=by+34;
}
static int pd_ctrl_y(int by){ return by+34+pd_pv_h+18; }  // control row top
// Hand-drag pan: pointer delta (screen px) -> source-px shift of the view. One
// screen px = pd_reg_w/pd_pv_w source px. Dragging right pushes the image right,
// so the center moves the opposite way. pd_region_build re-clamps the center.
static void pd_loupe_pan_to(int mx,int my){
    if(pd_pv_w<1||pd_pv_h<1) return;
    int dx=(int)((long)(mx-pd_drag_mx0)*pd_reg_w/pd_pv_w);
    int dy=(int)((long)(my-pd_drag_my0)*pd_reg_h/pd_pv_h);
    pd_cx=pd_drag_cx0-dx; pd_cy=pd_drag_cy0-dy;
    pd_region_build();
}

static void redraw_canvas_only(void);
static void pd_preview(void){
    const studio_op_t*op=studio_op_get(pd_opi); if(!op) return;
    pd_restore();
    op->apply(pd_val);
    g_doc.comp_dirty=1;
    pd_pane_render();
    redraw_canvas_only();
}
static void pd_ok(void){
    const studio_op_t*op=studio_op_get(pd_opi);
    if(op){ pd_restore(); undo_push(op->name); op->apply(pd_val); g_doc.comp_dirty=1; g_doc.modified=1; }
    pd_backup_free(); pd_pane_free(); pd_open=0; ui_status(op?op->name:"Applied"); ui_full_redraw();
}
static void pd_cancel(void){ pd_restore(); pd_backup_free(); pd_pane_free(); pd_open=0; ui_full_redraw(); }
// SP_COLOR now opens the shared modal picker; this fires when the modal commits
// a new colour into pd_val[] so the filter's live preview updates.
static void pd_color_committed(void){ if(pd_open) pd_preview(); }

static const uint32_t CVW_CHCOL[4] = { 0x00C0C0C0, 0x00E05050, 0x0050D050, 0x005080E0 };
static void draw_param_dialog(void){
    if(!pd_open) return;
    const studio_op_t*op=studio_op_get(pd_opi); if(!op) return;
    int bw=360, bx=pd_bx(), by=pd_by(), bh=pd_h();
    R(bx-2,by-2,bw+4,bh+4,C_LINE); R(bx,by,bw,bh,C_PANEL); OUT(bx,by,bw,bh,C_ACCENT);
    T(bx+14,by+12, op->name, C_ACCENT2);
    // Live preview LOUPE (#541): a zoomable/pannable window into the layer, in
    // addition to the full-canvas live preview.
    if(pd_pane_area()>0 && pd_pvwork){
        int pvw=pd_pv_w, pvh=pd_pv_h, px0,py0; pd_pane_geom(bx,by,bw,&px0,&py0);
        R(px0-2,py0-2,pvw+4,pvh+4,C_PANEL2);
        win_draw_image(g_win, px0, py0, pvw, pvh, pd_pvwork);
        OUT(px0-1,py0-1,pvw+2,pvh+2,C_LINE);
        OUT(px0-2,py0-2,pvw+4,pvh+4, pd_loupe_drag?C_ACCENT2:C_ACCENT);
        // Zoom/pan control row.
        int cy=pd_ctrl_y(by); char zb[16];
        button(bx+14,cy,26,20,"-",0,0);
        R(bx+44,cy,54,20,C_PANEL2); OUT(bx+44,cy,54,20,C_LINE);
        snprintf(zb,sizeof(zb),"%d%%",pd_zoom); gui_text_ttf_centered(g_win,bx+44,cy,54,20,zb,C_TEXT,11);
        button(bx+102,cy,26,20,"+",0,0);
        button(bx+134,cy,38,20,"1:1",0,pd_zoom==100);
        button(bx+176,cy,38,20,"Fit",0,0);
        Ts(bx+222,cy+4,"drag pane to pan",C_DIM);
    }
    int y=by+40+pd_pane_area();
    for(int k=0;k<pd_np;k++){
        const sparam_t*p=&op->params[k];
        int role=pd_handle_role(k);
        if(role==2){ continue; }                 // Y coord of a handle pair: hidden
        if(role==1){                              // X coord: compact on-canvas hint
            T(bx+14,y, p->name, C_TEXT);
            uint32_t hc=(op->nhandles>1 && k==op->handle_xp[1])?0x0060E0FF:0x00FFE060;
            R(bx+bw-24,y-1,12,12,hc); OUT(bx+bw-24,y-1,12,12,C_LINE);
            Ts(bx+bw-150,y+1,"drag marker on canvas",C_DIM);
            y+=pd_row_h_k(k); continue;
        }
        T(bx+14,y, p->name, C_TEXT);
        char vb[24];
        if(p->type==SP_SLIDER||p->type==SP_INT){
            button(bx+bw-118,y-3,20,20,"-",0,0);
            int tx=bx+bw-96, tw=64;
            R(tx,y-3,tw,20,C_PANEL2); OUT(tx,y-3,tw,20,C_LINE);
            int rng=p->max-p->min; if(rng<1)rng=1;
            int fillw=(pd_val[k]-p->min)*tw/rng; if(fillw<0)fillw=0; if(fillw>tw)fillw=tw;
            R(tx,y-3,fillw,20,C_ACCENT);
            snprintf(vb,sizeof(vb),"%d",pd_val[k]); Ts(tx+tw/2-6,y+1,vb,C_TEXT);
            button(bx+bw-28,y-3,20,20,"+",0,0);
        }else if(p->type==SP_ENUM){
            int ec=enum_count(p->choices); int cur=pd_val[k]; if(cur<0)cur=0; if(cur>=ec)cur=ec-1;
            char ch[32]; enum_choice(p->choices,cur,ch,sizeof(ch));
            button(bx+bw-118,y-3,20,20,"<",0,0);
            R(bx+bw-96,y-3,64,20,C_PANEL2); OUT(bx+bw-96,y-3,64,20,C_LINE);
            Ts(bx+bw-92,y+1,ch,C_TEXT);
            button(bx+bw-28,y-3,20,20,">",0,0);
        }else if(p->type==SP_CHECK){
            R(bx+bw-40,y-3,20,20, pd_val[k]?C_ACCENT:C_PANEL2); OUT(bx+bw-40,y-3,20,20,C_LINE);
            if(pd_val[k]) T(bx+bw-36,y-3,"x",C_TEXT);
        }else if(p->type==SP_COLOR){
            uint32_t cv=(uint32_t)pd_val[k]&0xFFFFFF;
            R(bx+bw-40,y-3,24,20,cv); OUT(bx+bw-40,y-3,24,20,C_LINE);
            button(bx+14,y+18,90,20,"Change...",0,0);   // opens the shared modal picker
        }else if(p->type==SP_ANGLE){
            int cx,cy,rad; pd_dial_geom(k,&cx,&cy,&rad);
            int ang=pd_val[k];
            R(cx-rad-1,cy-rad-1,(rad+1)*2,(rad+1)*2,C_PANEL2);
            ui_circle(cx,cy,rad,C_LINE);
            for(int a=0;a<360;a+=45){ int tx=cx+ui_cos(a)*(rad-3)/4096, ty=cy+ui_sin(a)*(rad-3)/4096; R(tx,ty,2,2,C_DIM); }
            int hx=cx+ui_cos(ang)*(rad-4)/4096, hy=cy+ui_sin(ang)*(rad-4)/4096;
            ui_line(cx,cy,hx,hy,C_ACCENT); R(hx-3,hy-3,6,6,C_ACCENT2); OUT(hx-3,hy-3,6,6,C_LINE);
            R(cx-2,cy-2,4,4,C_TEXT);
            snprintf(vb,sizeof(vb),"%d deg",ang); Ts(bx+14,y+22,vb,C_ACCENT2);
        }else if(p->type==SP_CURVE){
            const char* TN[4]={"RGB","R","G","B"};
            for(int t=0;t<4;t++) button(bx+14+t*46,y+2,42,18,TN[t], cvw_chan==t, cvw_chan==t);
            int gx,gy,gs; pd_curve_geom(k,&gx,&gy,&gs);
            R(gx,gy,gs,gs,C_PANEL2); OUT(gx-1,gy-1,gs+2,gs+2,C_LINE);
            for(int q=1;q<4;q++){ R(gx+gs*q/4,gy,1,gs,mix(C_LINE,C_PANEL,90)); R(gx,gy+gs*q/4,gs,1,mix(C_LINE,C_PANEL,90)); }
            ui_line(gx,gy+gs,gx+gs,gy, mix(C_LINE,C_PANEL,140));
            int lut[256]; curve_chan_lut(cvw_chan,lut);
            uint32_t cc=CVW_CHCOL[cvw_chan&3];
            for(int x=0;x<gs;x++){ int dv=x*255/(gs>1?gs-1:1); int yv=lut[clampi(dv,0,255)]; int py=gy+(255-yv)*gs/255; R(gx+x,py-1,1,2,cc); }
            int c=cvw_chan;
            for(int i=0;i<cvw_n[c];i++){ int px=gx+cvw_px[c][i]*gs/255, py=gy+(255-cvw_py[c][i])*gs/255;
                uint32_t pc=(i==cvw_sel)?C_ACCENT:C_TEXT; R(px-3,py-3,6,6,pc); OUT(px-3,py-3,6,6,C_LINE); }
            Ts(bx+14,gy+gs+3,"Click add | drag move | Alt-click delete",C_DIM);
        }else if(p->type==SP_KERNEL){
            if(!kw_inited) kernel_reset();
            static const char*KPN[5]={"Sharp","Edge","Emb","Blur","Outl"};
            for(int t=0;t<5;t++) button(bx+14+t*64,y+18,60,18,KPN[t],0,0);
            int gx=bx+14, gy2=y+40;
            for(int r=0;r<5;r++) for(int c=0;c<5;c++){
                int ci=r*5+c, cxp=gx+c*44, cyp=gy2+r*22;
                R(cxp,cyp,42,20, ci==kw_sel?C_ACCENT:C_PANEL2); OUT(cxp,cyp,42,20,C_LINE);
                snprintf(vb,sizeof(vb),"%d",kw_cell[ci]); Ts(cxp+4,cyp+3,vb, ci==kw_sel?C_TEXT:C_DIM);
            }
            button(bx+242,gy2+22,24,20,"-",0,0);
            button(bx+300,gy2+22,24,20,"+",0,0);
            snprintf(vb,sizeof(vb),"%d",kw_cell[kw_sel]); Ts(bx+270,gy2+26,vb,C_ACCENT2);
            Ts(bx+14,gy2+114,"Click cell then +/- | drag | type digits",C_DIM);
        }
        y+=pd_row_h_k(k);
    }
    button(bx+14,by+bh-32,74,24,"Reset",0,0);
    button(bx+bw-170,by+bh-32,74,24,"Cancel",0,0);
    button(bx+bw-88,by+bh-32,74,24,"OK",0,0);
}
// On-canvas positional handle markers (drawn over the canvas during preview).
static void draw_pd_handles(void){
    if(!pd_open) return; const studio_op_t*op=studio_op_get(pd_opi); if(!op||op->nhandles<=0) return;
    int z=zoom(), W=g_doc.w, H=g_doc.h; if(W<1||H<1) return;
    for(int hh=0; hh<op->nhandles && hh<2; hh++){
        int xp=op->handle_xp[hh], yp=op->handle_yp[hh]; if(xp<0||xp>=pd_np||yp<0||yp>=pd_np) continue;
        int cxp=pd_val[xp]*W/1000, cyp=pd_val[yp]*H/1000;
        int sx=cv_x()+(cxp-pan_x)*z/100, sy=cv_y()+(cyp-pan_y)*z/100;
        if(!IN(sx,sy,cv_x(),cv_y(),cv_w(),cv_h())) continue;
        uint32_t col=(hh==0)?0x00FFE060:0x0060E0FF;
        for(int a=0;a<360;a+=20){ int rx=sx+ui_cos(a)*11/4096, ry=sy+ui_sin(a)*11/4096; Rclip(rx,ry,2,2,col); }
        Rclip(sx-10,sy,21,1,col); Rclip(sx,sy-10,1,21,col);
        Rclip(sx-2,sy-2,4,4,0x00202020); Rclip(sx-1,sy-1,2,2,col);
    }
}

// Set an SP_ANGLE from a pointer position over its dial.
static void pd_dial_set(int k,int mx,int my){
    const studio_op_t*op=studio_op_get(pd_opi); if(!op) return; const sparam_t*p=&op->params[k];
    int cx,cy,rad; (void)rad; pd_dial_geom(k,&cx,&cy,&rad);
    int ang=ui_atan2(my-cy,mx-cx);
    if(ang<p->min)ang=p->min; if(ang>p->max)ang=p->max;
    pd_val[k]=ang; pd_preview();
}
// Move the selected SP_CURVE control point to a pointer position over its graph.
static void pd_curve_drag_to(int mx,int my){
    if(pd_curve_k<0 || cvw_sel<0) return;
    int gx,gy,gs; pd_curve_geom(pd_curve_k,&gx,&gy,&gs);
    int c=cvw_chan, n=cvw_n[c];
    int dx=clampi((mx-gx)*255/(gs>1?gs-1:1),0,255), dy=clampi(255-(my-gy)*255/(gs>1?gs-1:1),0,255);
    if(cvw_sel==0) dx=0; else if(cvw_sel==n-1) dx=255;
    else { int lo=cvw_px[c][cvw_sel-1]+1, hi=cvw_px[c][cvw_sel+1]-1; if(lo>hi){lo=hi=cvw_px[c][cvw_sel];} dx=clampi(dx,lo,hi); }
    cvw_px[c][cvw_sel]=dx; cvw_py[c][cvw_sel]=dy; pd_preview();
}
// Click inside an SP_CURVE row: tab switch, point select/insert/delete. ret 1 consumed.
static int pd_curve_click(int k,int mx,int my){
    int bx=pd_bx();
    for(int t=0;t<4;t++){ if(IN(mx,my,bx+14+t*46, pd_row_top(k)+2, 42,18)){ cvw_chan=t; cvw_sel=-1; pd_preview(); return 1; } }
    int gx,gy,gs; pd_curve_geom(k,&gx,&gy,&gs);
    if(!IN(mx,my,gx-6,gy-6,gs+12,gs+12)) return 0;
    int c=cvw_chan, n=cvw_n[c];
    int best=-1, bestd=1<<30;
    for(int i=0;i<n;i++){ int px=gx+cvw_px[c][i]*gs/255, py=gy+(255-cvw_py[c][i])*gs/255;
        int d=(mx-px)*(mx-px)+(my-py)*(my-py); if(d<bestd){bestd=d;best=i;} }
    if(best>=0 && bestd<=12*12){
        if(mod_alt && best>0 && best<n-1){ for(int i=best;i<n-1;i++){cvw_px[c][i]=cvw_px[c][i+1];cvw_py[c][i]=cvw_py[c][i+1];} cvw_n[c]--; cvw_sel=-1; pd_preview(); return 1; }
        cvw_sel=best; pd_curve_k=k; pd_cvdrag=k; pd_curve_drag_to(mx,my); return 1;
    }
    if(n<CV_MAXPTS){
        int dx=clampi((mx-gx)*255/(gs>1?gs-1:1),0,255), dy=clampi(255-(my-gy)*255/(gs>1?gs-1:1),0,255);
        int ins=0; while(ins<n && cvw_px[c][ins]<dx) ins++;
        for(int i=n;i>ins;i--){cvw_px[c][i]=cvw_px[c][i-1];cvw_py[c][i]=cvw_py[c][i-1];}
        cvw_px[c][ins]=dx; cvw_py[c][ins]=dy; cvw_n[c]++;
        cvw_sel=ins; pd_curve_k=k; pd_cvdrag=k; pd_preview(); return 1;
    }
    return 1;
}
// On-canvas positional handles.
static int pd_handle_hit(int mx,int my){    // returns handle index (0..1) or -1
    const studio_op_t*op=studio_op_get(pd_opi); if(!op||op->nhandles<=0) return -1;
    int z=zoom(), W=g_doc.w, H=g_doc.h; if(W<1||H<1) return -1;
    for(int hh=0; hh<op->nhandles && hh<2; hh++){
        int xp=op->handle_xp[hh], yp=op->handle_yp[hh]; if(xp<0||xp>=pd_np||yp<0||yp>=pd_np) continue;
        int cxp=pd_val[xp]*W/1000, cyp=pd_val[yp]*H/1000;
        int sx=cv_x()+(cxp-pan_x)*z/100, sy=cv_y()+(cyp-pan_y)*z/100;
        if((mx-sx)*(mx-sx)+(my-sy)*(my-sy) <= 14*14) return hh;
    }
    return -1;
}
static void pd_handle_setxy(int hh,int mx,int my){
    const studio_op_t*op=studio_op_get(pd_opi); if(!op) return;
    int xp=op->handle_xp[hh], yp=op->handle_yp[hh];
    int cx,cy; screen_to_canvas(mx,my,&cx,&cy);
    int W=g_doc.w?g_doc.w:1, H=g_doc.h?g_doc.h:1;
    pd_val[xp]=clampi(cx*1000/W,0,1000); pd_val[yp]=clampi(cy*1000/H,0,1000);
    pd_preview();
}

// Hit test for param dialog. Returns 1 if consumed.
static int click_param_dialog(int mx,int my){
    if(!pd_open) return 0;
    const studio_op_t*op=studio_op_get(pd_opi); if(!op){ pd_open=0; return 1; }
    int bw=360, bx=pd_bx(), by=pd_by(), bh=pd_h();
    if(IN(mx,my,bx+14,by+bh-32,74,24)){    // Reset all params to defaults
        for(int k=0;k<pd_np;k++){ pd_val[k]=op->params[k].def; if(op->params[k].type==SP_CURVE) curve_reset_all(); if(op->params[k].type==SP_KERNEL) kernel_reset(); }
        cvw_sel=-1; pd_preview(); return 1;
    }
    if(IN(mx,my,bx+bw-170,by+bh-32,74,24)){ pd_cancel(); return 1; }
    if(IN(mx,my,bx+bw-88,by+bh-32,74,24)){ pd_ok(); return 1; }
    // Loupe: zoom/pan controls + drag-inside-pane to pan.
    if(pd_pane_area()>0 && pd_pvwork){
        int px0,py0; pd_pane_geom(bx,by,bw,&px0,&py0);
        int cy=pd_ctrl_y(by);
        if(IN(mx,my,bx+14,cy,26,20)){ pd_zoom_step(-1); redraw_canvas_only(); return 1; }
        if(IN(mx,my,bx+102,cy,26,20)){ pd_zoom_step(1); redraw_canvas_only(); return 1; }
        if(IN(mx,my,bx+134,cy,38,20)){ pd_zoom_actual(); redraw_canvas_only(); return 1; }
        if(IN(mx,my,bx+176,cy,38,20)){ pd_zoom_fit(); redraw_canvas_only(); return 1; }
        if(IN(mx,my,px0,py0,pd_pv_w,pd_pv_h)){
            pd_loupe_drag=1; pd_drag_mx0=mx; pd_drag_my0=my; pd_drag_cx0=pd_cx; pd_drag_cy0=pd_cy;
            redraw_canvas_only(); return 1;
        }
    }
    int y=by+40+pd_pane_area();
    for(int k=0;k<pd_np;k++){
        const sparam_t*p=&op->params[k];
        int role=pd_handle_role(k);
        if(role){ y+=pd_row_h_k(k); continue; }   // handle coords are set on the canvas
        if(p->type==SP_SLIDER||p->type==SP_INT){
            if(IN(mx,my,bx+bw-118,y-3,20,20)){ pd_val[k]=clampi(pd_val[k]-1,p->min,p->max); pd_preview(); return 1; }
            if(IN(mx,my,bx+bw-28,y-3,20,20)){ pd_val[k]=clampi(pd_val[k]+1,p->min,p->max); pd_preview(); return 1; }
            int tx=bx+bw-96, tw=64;
            if(IN(mx,my,tx,y-3,tw,20)){ int rng=p->max-p->min; pd_val[k]=clampi(p->min+(mx-tx)*rng/tw,p->min,p->max); pd_drag=k; pd_preview(); return 1; }
        }else if(p->type==SP_ENUM){
            int ec=enum_count(p->choices); if(ec<1)ec=1;
            if(IN(mx,my,bx+bw-118,y-3,20,20)){ pd_val[k]=(pd_val[k]-1+ec)%ec; pd_preview(); return 1; }
            if(IN(mx,my,bx+bw-28,y-3,20,20)){ pd_val[k]=(pd_val[k]+1)%ec; pd_preview(); return 1; }
        }else if(p->type==SP_CHECK){
            if(IN(mx,my,bx+bw-40,y-3,20,20)){ pd_val[k]=!pd_val[k]; pd_preview(); return 1; }
        }else if(p->type==SP_COLOR){
            if(IN(mx,my,bx+14,y+18,90,20)){ cp_open_modal_for_value((uint32_t)pd_val[k]&0xFFFFFF, &pd_val[k], pd_color_committed); return 1; }
        }else if(p->type==SP_ANGLE){
            int cx,cy,rad; pd_dial_geom(k,&cx,&cy,&rad);
            if((mx-cx)*(mx-cx)+(my-cy)*(my-cy) <= (rad+6)*(rad+6)){ pd_dialdrag=k; pd_dial_set(k,mx,my); return 1; }
        }else if(p->type==SP_CURVE){
            if(pd_curve_click(k,mx,my)) return 1;
        }else if(p->type==SP_KERNEL){
            if(!kw_inited) kernel_reset();
            // preset row seeds the grid (and its Scale/Offset companions, so
            // e.g. Blur lands with Scale 9 like the retired Convolution Matrix)
            for(int t=0;t<5;t++) if(IN(mx,my,bx+14+t*64,y+18,60,18)){
                kernel_seed_preset(t);
                for(int j=0;j<pd_np;j++){
                    if(op->params[j].type!=SP_INT) continue;
                    if(!strcmp(op->params[j].name,"Scale"))  pd_val[j]=KW_PDIV[t];
                    if(!strcmp(op->params[j].name,"Offset")) pd_val[j]=KW_POFF[t];
                }
                pd_preview(); return 1;
            }
            int gx=bx+14, gy2=y+40;
            if(IN(mx,my,bx+242,gy2+22,24,20)){ kw_cell[kw_sel]=clampi(kw_cell[kw_sel]-1,-999,999); pd_preview(); return 1; }
            if(IN(mx,my,bx+300,gy2+22,24,20)){ kw_cell[kw_sel]=clampi(kw_cell[kw_sel]+1,-999,999); pd_preview(); return 1; }
            if(IN(mx,my,gx,gy2,220,110)){
                int c=(mx-gx)/44, r=(my-gy2)/22; if(c>4)c=4; if(r>4)r=4;
                kw_sel=r*5+c; pd_kdrag=1; kw_drag_y0=my; kw_drag_v0=kw_cell[kw_sel];
                redraw_canvas_only(); return 1;
            }
        }
        y+=pd_row_h_k(k);
    }
    return 1; // consume dialog area
}

// ---------------------------------------------------------------------------
// Modal overlays (text/message)
// ---------------------------------------------------------------------------
static void draw_modal(void){
    if(!modal) return;
    int bw=520, bh=(modal==2)?260:120;
    int bx=(g_w-bw)/2, by=(g_h-bh)/2;
    R(bx-2,by-2,bw+4,bh+4,C_LINE); R(bx,by,bw,bh,C_PANEL); OUT(bx,by,bw,bh,C_ACCENT);
    if(modal==1){
        T(bx+16,by+16, modal_prompt, C_TEXT);
        R(bx+16,by+44,bw-32,26,C_PANEL2); OUT(bx+16,by+44,bw-32,26,C_LINE);
        T(bx+22,by+49, modal_buf, C_TEXT);
        int cw=gui_string_width(modal_buf); R(bx+22+cw,by+47,2,20,C_ACCENT2);
        Ts(bx+16,by+90,"Enter = OK    Esc = Cancel", C_DIM);
        button(bx+bw-172,by+bh-34,76,24,"Cancel",0,0);
        button(bx+bw-88, by+bh-34,76,24,"OK",0,0);
    }else{
        T(bx+16,by+14, modal_prompt, C_ACCENT2);
        int y=by+40, x=bx+16, maxw=bw-32; const char *p=modal_msg; char line[80];
        while(*p){ int ll=0;
            while(*p && *p!='\n' && gui_string_width(line)<maxw-8 && ll<78){ line[ll++]=*p++; line[ll]=0; }
            if(*p=='\n') p++;
            T(x,y,line,C_TEXT); y+=18; if(y>by+bh-40) break;
        }
        button(bx+bw-90,by+bh-34,74,24,"OK",0,0);
    }
}

// ---------------------------------------------------------------------------
// Full redraw
// ---------------------------------------------------------------------------
void ui_full_redraw(void){
    g_selbb_dirty=1;                 // recompute cached selection bbox this frame
    R(0,0,g_w,g_h,C_BG);
    blit_canvas();
    draw_rulers();
    draw_overlays();
    draw_pd_handles();
    draw_menubar();
    draw_optbar();
    draw_strip();
    draw_dock();
    draw_status();
    draw_dropdown();
    draw_blend_pop();
    draw_param_dialog();
    draw_modal();
    draw_filebrowser();
    draw_print_preview();
    cp_draw_modal();          // color-picker modal draws on top of the param dialog
    draw_guard();
    win_invalidate(g_win);
}
static void redraw_canvas_only(void){
    blit_canvas();
    draw_rulers();
    draw_overlays();
    draw_pd_handles();
    if(pd_open) draw_param_dialog();
    win_invalidate(g_win);
}
void ui_status(const char *msg){
    int i=0; if(msg){ while(msg[i] && i<(int)sizeof(g_status)-1){ g_status[i]=msg[i]; i++; } }
    g_status[i]=0; draw_status(); win_invalidate(g_win);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------
static void open_modal_text(int purpose,const char*prompt,const char*deflt){
    modal=1; modal_purpose=purpose;
    int i=0; while(prompt[i]&&i<63){modal_prompt[i]=prompt[i];i++;} modal_prompt[i]=0;
    i=0; if(deflt){ while(deflt[i]&&i<159){modal_buf[i]=deflt[i];i++;} } modal_buf[i]=0;
    tf_init(&modal_tf, modal_buf, sizeof(modal_buf));
}
static void open_msg(const char*title,const char*body){
    modal=2;
    int i=0; while(title[i]&&i<63){modal_prompt[i]=title[i];i++;} modal_prompt[i]=0;
    i=0; if(body){ while(body[i]&&i<511){modal_msg[i]=body[i];i++;} } modal_msg[i]=0;
}
static int parse_int(const char*s){ int v=0,neg=0,i=0; if(s[0]=='-'){neg=1;i=1;} while(s[i]>='0'&&s[i]<='9'){v=v*10+s[i]-'0';i++;} return neg?-v:v; }

static void run_op(int i){
    const studio_op_t*op=studio_op_get(i); if(!op) return;
    if(op->nparams<=0){ undo_push(op->name); if(op->apply) op->apply((const int*)0); g_doc.comp_dirty=1; g_doc.modified=1; ui_status(op->name); ui_full_redraw(); return; }
    pd_open=1; pd_opi=i; pd_np=op->nparams; if(pd_np>STUDIO_MAX_PARAMS)pd_np=STUDIO_MAX_PARAMS;
    pd_drag=pd_dialdrag=pd_cvdrag=pd_hdrag=-1; pd_kdrag=0;
    for(int k=0;k<pd_np;k++){ pd_val[k]=op->params[k].def; if(op->params[k].type==SP_CURVE) curve_reset_all(); if(op->params[k].type==SP_KERNEL) kernel_reset(); }
    pd_backup_make(); pd_pane_setup(); pd_preview(); ui_full_redraw();
}

// --- Clipboard / fill / stroke (Edit menu) ---------------------------------
static void clip_free(void){ if(g_clip){ free(g_clip); g_clip=0; } g_clip_w=g_clip_h=0; }
// Bounding box of the active selection (whole doc if none). ret 1 if a real
// selection bbox was found.
static int sel_bbox(int*x0,int*y0,int*x1,int*y1){
    if(!g_doc.sel_active || !g_doc.sel){ *x0=0;*y0=0;*x1=g_doc.w-1;*y1=g_doc.h-1; return 0; }
    int minx=g_doc.w,miny=g_doc.h,maxx=-1,maxy=-1;
    for(int y=0;y<g_doc.h;y++){ uint8_t*r=g_doc.sel+(size_t)y*g_doc.w;
        for(int x=0;x<g_doc.w;x++) if(r[x]){ if(x<minx)minx=x; if(x>maxx)maxx=x; if(y<miny)miny=y; if(y>maxy)maxy=y; } }
    if(maxx<minx){ *x0=0;*y0=0;*x1=g_doc.w-1;*y1=g_doc.h-1; return 0; }
    *x0=minx;*y0=miny;*x1=maxx;*y1=maxy; return 1;
}
static void clip_copy(void){
    layer_t*L=&g_doc.layer[g_doc.active]; if(!L->px) return;
    int x0,y0,x1,y1; sel_bbox(&x0,&y0,&x1,&y1);
    int w=x1-x0+1,h=y1-y0+1; if(w<1||h<1) return;
    clip_free(); g_clip=(uint32_t*)malloc((size_t)w*h*4); if(!g_clip) return;
    g_clip_w=w; g_clip_h=h; g_clip_x=x0; g_clip_y=y0;
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){
        int sx=x0+x, sy=y0+y; uint32_t p=L->px[(size_t)sy*g_doc.w+sx];
        int a=px_a(p)*sel_at(sx,sy)/255;
        g_clip[y*w+x]=argb(a,px_r(p),px_g(p),px_b(p));
    }
}
static void clip_cut(void){
    undo_push("Cut"); clip_copy();
    layer_t*L=&g_doc.layer[g_doc.active];
    if(L->px) for(int y=0;y<g_doc.h;y++) for(int x=0;x<g_doc.w;x++){
        int cov=sel_at(x,y); if(cov<=0) continue;
        uint32_t p=L->px[(size_t)y*g_doc.w+x];
        L->px[(size_t)y*g_doc.w+x]=argb(px_a(p)*(255-cov)/255,px_r(p),px_g(p),px_b(p));
    }
    g_doc.comp_dirty=1; g_doc.modified=1;
}
static void clip_paste(void){
    if(!g_clip) return;
    undo_push("Paste");
    int idx=layer_add("Pasted", 0x00000000); if(idx<0) return;
    layer_t*L=&g_doc.layer[idx];
    if(L->px) for(int y=0;y<g_clip_h;y++) for(int x=0;x<g_clip_w;x++){
        int dx=g_clip_x+x, dy=g_clip_y+y;
        if(dx<0||dy<0||dx>=g_doc.w||dy>=g_doc.h) continue;
        L->px[(size_t)dy*g_doc.w+dx]=g_clip[y*g_clip_w+x];
    }
    g_doc.comp_dirty=1; g_doc.modified=1;
}
static void do_fill(uint32_t col){
    undo_push("Fill");
    layer_t*L=&g_doc.layer[g_doc.active];
    int fr=px_r(col),fg=px_g(col),fb=px_b(col);
    if(L->px) for(int y=0;y<g_doc.h;y++) for(int x=0;x<g_doc.w;x++){
        int a=sel_at(x,y); if(a<=0) continue;
        uint32_t d=L->px[(size_t)y*g_doc.w+x];
        int rr=(fr*a+px_r(d)*(255-a))/255, gg=(fg*a+px_g(d)*(255-a))/255, bb=(fb*a+px_b(d)*(255-a))/255;
        int da=px_a(d), na=da+(255-da)*a/255;
        L->px[(size_t)y*g_doc.w+x]=argb(na,rr,gg,bb);
    }
    g_doc.comp_dirty=1; g_doc.modified=1;
}
static void do_stroke(void){
    if(!g_doc.sel_active || !g_doc.sel) return;
    undo_push("Stroke");
    layer_t*L=&g_doc.layer[g_doc.active]; if(!L->px){ return; }
    uint32_t col=argb(255,px_r(g_tool.fg),px_g(g_tool.fg),px_b(g_tool.fg));
    for(int y=0;y<g_doc.h;y++) for(int x=0;x<g_doc.w;x++){
        if(g_doc.sel[(size_t)y*g_doc.w+x]<=127) continue;
        if(sel_at(x-1,y)<=127||sel_at(x+1,y)<=127||sel_at(x,y-1)<=127||sel_at(x,y+1)<=127)
            L->px[(size_t)y*g_doc.w+x]=col;
    }
    g_doc.comp_dirty=1; g_doc.modified=1;
}

// Forward decls for the file-browser + unsaved-changes guard (defined below).
static void fb_start(int purpose);
static void guard_open_for(int action);
static int  g_want_quit=0, g_in_guarded=0;

static void do_action(int a){
    // Unsaved-changes guard (item 3): intercept destructive actions.
    if((a==A_NEW||a==A_OPEN||a==A_REVERT||a==A_QUIT) && g_doc.modified && !g_in_guarded){
        guard_open_for(a); return;
    }
    if(a>=A_OP_BASE){ run_op(a-A_OP_BASE); return; }
    switch(a){
        case A_NEW: open_modal_text(MP_NEWSZ,"New image size (WxH):","960x600"); break;
        case A_OPEN: fb_start(MP_OPEN); break;
        case A_SAVE:
            if(g_doc.path[0]){ if(io_save(g_doc.path)==0){ g_doc.modified=0; ui_status("Saved"); } else ui_status("Save failed"); }
            else fb_start(MP_SAVEAS);
            break;
        case A_SAVEAS: fb_start(MP_SAVEAS); break;
        case A_EXPORTPNG: fb_start(MP_EXPORT); break;
        case A_EXPORTBMP: fb_start(MP_EXPORTBMP); break;
        case A_PRINT: open_print_preview(); break;
        case A_REVERT:
            if(g_doc.path[0] && io_load(g_doc.path)==0){ g_doc.comp_dirty=1; g_doc.modified=0; ui_status("Reverted"); }
            else ui_status("Revert failed");
            ui_full_redraw(); break;
        case A_CUT: clip_cut(); ui_status("Cut"); ui_full_redraw(); break;
        case A_COPY: clip_copy(); ui_status("Copied"); ui_full_redraw(); break;
        case A_PASTE: clip_paste(); ui_status("Pasted"); ui_full_redraw(); break;
        case A_FILLFG: do_fill(g_tool.fg); ui_status("Filled with FG"); ui_full_redraw(); break;
        case A_FILLBG: do_fill(g_tool.bg); ui_status("Filled with BG"); ui_full_redraw(); break;
        case A_STROKE: do_stroke(); ui_status("Stroked selection"); ui_full_redraw(); break;
        case A_QUIT: modal=0; menu_open=-1; g_want_quit=1; break;
        case A_UNDO: if(undo_undo()){ g_doc.comp_dirty=1; ui_status("Undo"); } ui_full_redraw(); break;
        case A_REDO: if(undo_redo()){ g_doc.comp_dirty=1; ui_status("Redo"); } ui_full_redraw(); break;
        case A_FLATTEN: undo_push("Flatten"); doc_flatten(); ui_status("Flattened"); ui_full_redraw(); break;
        case A_LNEW: undo_push("New Layer"); layer_add("Layer", 0x00000000); ui_full_redraw(); break;
        case A_LDUP: undo_push("Duplicate"); layer_dup(g_doc.active); ui_full_redraw(); break;
        case A_LDEL: undo_push("Delete Layer"); layer_del(g_doc.active); ui_full_redraw(); break;
        case A_LUP:  undo_push("Raise"); layer_move(g_doc.active,+1); ui_full_redraw(); break;
        case A_LDOWN:undo_push("Lower"); layer_move(g_doc.active,-1); ui_full_redraw(); break;
        case A_LMERGE: undo_push("Merge Down"); layer_merge_down(g_doc.active); ui_full_redraw(); break;
        case A_LMASKADD: undo_push("Add Mask"); layer_mask_add(g_doc.active,0); ui_status("Mask added"); ui_full_redraw(); break;
        case A_LMASKAPPLY: undo_push("Apply Mask"); layer_mask_apply(g_doc.active); ui_status("Mask applied"); ui_full_redraw(); break;
        case A_LMASKDEL: undo_push("Delete Mask"); layer_mask_delete(g_doc.active); ui_full_redraw(); break;
        case A_LMASKTOG: { layer_t*L=&g_doc.layer[g_doc.active]; if(L->mask){ L->mask_active=!L->mask_active; ui_status(L->mask_active?"Editing mask":"Editing layer"); } ui_full_redraw(); } break;
        case A_LLOCKA: { layer_t*L=&g_doc.layer[g_doc.active]; L->lock_alpha=!L->lock_alpha; ui_full_redraw(); } break;
        case A_LBLENDPOP: { int dx=g_w-DOCK_W; blend_pop=1; blend_pop_x=dx+52; blend_pop_y=LMODE_y+20; ui_full_redraw(); } break;
        case A_LOPUP: { layer_t*L=&g_doc.layer[g_doc.active]; layer_set_opacity(g_doc.active,clampi(L->opacity+26,0,255)); g_doc.comp_dirty=1; ui_full_redraw(); } break;
        case A_LOPDN: { layer_t*L=&g_doc.layer[g_doc.active]; layer_set_opacity(g_doc.active,clampi(L->opacity-26,0,255)); g_doc.comp_dirty=1; ui_full_redraw(); } break;
        case A_SELALL: sel_all(); ui_status("Select All"); ui_full_redraw(); break;
        case A_SELNONE: sel_clear(); ui_status("Select None"); ui_full_redraw(); break;
        case A_SELINV: sel_invert(); ui_status("Invert Selection"); ui_full_redraw(); break;
        case A_SGROW: open_modal_text(MP_SGROW,"Grow selection by (px):","4"); break;
        case A_SSHRINK: open_modal_text(MP_SSHRINK,"Shrink selection by (px):","4"); break;
        case A_SBORDER: open_modal_text(MP_SBORDER,"Border width (px):","4"); break;
        case A_SFEATHER: open_modal_text(MP_SFEATHER,"Feather radius (px):","6"); break;
        case A_SROUND: open_modal_text(MP_SROUND,"Corner radius (px):","12"); break;
        case A_CHSAVE: if(channel_save_selection("Selection")>=0) ui_status("Saved selection to channel"); else ui_status("Channel save failed"); ui_full_redraw(); break;
        case A_IFLIPH: undo_push("Flip Horizontal"); layer_flip(g_doc.active,1); g_doc.comp_dirty=1; ui_full_redraw(); break;
        case A_IFLIPV: undo_push("Flip Vertical"); layer_flip(g_doc.active,0); g_doc.comp_dirty=1; ui_full_redraw(); break;
        case A_IROT90CW: undo_push("Rotate 90 CW"); layer_rotate90(g_doc.active,1); g_doc.comp_dirty=1; ui_full_redraw(); break;
        case A_IROT90CCW: undo_push("Rotate 90 CCW"); layer_rotate90(g_doc.active,0); g_doc.comp_dirty=1; ui_full_redraw(); break;
        case A_IROTARB: open_modal_text(MP_ROT,"Rotate layer (degrees):","15"); break;
        case A_ISCALE: open_modal_text(MP_SCALE,"Scale layer to (WxH):","480x300"); break;
        case A_ICROP: undo_push("Crop"); doc_crop_to_selection(); g_zoom_pct=100; pan_x=pan_y=0; g_doc.comp_dirty=1; ui_status("Cropped"); ui_full_redraw(); break;
        case A_ICANVAS: open_modal_text(MP_CANVAS,"Canvas size (WxH):","960x600"); break;
        case A_ZIN: zoom_step(+1); ui_full_redraw(); break;
        case A_ZOUT: zoom_step(-1); ui_full_redraw(); break;
        case A_ZFIT: g_zoom_pct=zoom_fit_pct(); pan_x=pan_y=0; ui_full_redraw(); break;
        case A_VGRID: grid_on=!grid_on; ui_full_redraw(); break;
        case A_VGRIDSP: open_modal_text(MP_GRIDSP,"Grid spacing (px):","32"); break;
        case A_VGCLEAR: nguide_v=nguide_h=0; ui_full_redraw(); break;
        case A_AICMD:
            if(!ai_available()){ open_msg("AI","AI is unavailable. Place a key at /CONFIG/KIMI.KEY and connect to a network."); ui_full_redraw(); }
            else open_modal_text(MP_AICMD,"Describe an edit:","warmer and higher contrast");
            break;
        case A_AIPAL:
            if(!ai_available()){ open_msg("AI","AI is unavailable. Place a key at /CONFIG/KIMI.KEY and connect to a network."); ui_full_redraw(); }
            else open_modal_text(MP_AIPAL,"Palette prompt:","autumn forest");
            break;
        case A_ABOUT:
            open_msg("Maytera Studio",
                "Maytera Studio - GIMP-class layered image editor.\n"
                "Layers with masks and blend modes, selections,\n"
                "a registry of Colors adjustments and Filters,\n"
                "generators, channels, paths, histogram, and native AI.\n"
                "Filters menu is built live from the op registry.");
            ui_full_redraw(); break;
        default: break;
    }
}

static void modal_confirm(void){
    char buf[160]; int i=0; while(modal_buf[i]&&i<159){buf[i]=modal_buf[i];i++;} buf[i]=0;
    int purpose=modal_purpose; modal=0;
    switch(purpose){
        case MP_NEWSZ: case MP_CANVAS: {
            int w=0,h=0,s=0; while(buf[s]>='0'&&buf[s]<='9'){w=w*10+buf[s]-'0';s++;}
            if(buf[s]=='x'||buf[s]=='X'){ s++; while(buf[s]>='0'&&buf[s]<='9'){h=h*10+buf[s]-'0';s++;} }
            if(w<1||h<1){w=STUDIO_DEF_W;h=STUDIO_DEF_H;}
            w=clampi(w,1,STUDIO_MAX_W); h=clampi(h,1,STUDIO_MAX_H);
            if(purpose==MP_NEWSZ){ doc_new(w,h,argb(255,255,255,255)); }
            else { undo_push("Canvas Size"); doc_resize(w,h,0); }
            g_zoom_pct=100; pan_x=pan_y=0; ui_status("Resized");
        } break;
        case MP_OPEN:
            if(io_load(buf)==0){ int j=0; while(buf[j]&&j<STUDIO_PATH_LEN-1){g_doc.path[j]=buf[j];j++;} g_doc.path[j]=0; g_doc.comp_dirty=1; g_zoom_pct=100; pan_x=pan_y=0; ui_status("Opened"); }
            else ui_status("Open failed");
            break;
        case MP_SAVEAS:
            if(io_save(buf)==0){ int j=0; while(buf[j]&&j<STUDIO_PATH_LEN-1){g_doc.path[j]=buf[j];j++;} g_doc.path[j]=0; g_doc.modified=0; ui_status("Saved"); }
            else ui_status("Save failed");
            break;
        case MP_EXPORT: if(io_save(buf)==0) ui_status("Exported PNG"); else ui_status("Export failed"); break;
        case MP_SGROW: undo_push("Grow"); sel_grow(parse_int(buf)); ui_status("Grew selection"); break;
        case MP_SSHRINK: undo_push("Shrink"); sel_shrink(parse_int(buf)); ui_status("Shrank selection"); break;
        case MP_SBORDER: undo_push("Border"); sel_border(parse_int(buf)); ui_status("Border"); break;
        case MP_SFEATHER: undo_push("Feather"); sel_feather(parse_int(buf)); ui_status("Feathered"); break;
        case MP_SROUND: undo_push("Rounded"); sel_round(parse_int(buf)); ui_status("Rounded"); break;
        case MP_ROT: undo_push("Rotate"); layer_rotate_arbitrary(g_doc.active,parse_int(buf)); g_doc.comp_dirty=1; ui_status("Rotated"); break;
        case MP_SCALE: { int w=0,h=0,s=0; while(buf[s]>='0'&&buf[s]<='9'){w=w*10+buf[s]-'0';s++;} if(buf[s]=='x'||buf[s]=='X'){s++; while(buf[s]>='0'&&buf[s]<='9'){h=h*10+buf[s]-'0';s++;}} if(w>0&&h>0){ undo_push("Scale Layer"); layer_scale(g_doc.active,w,h); g_doc.comp_dirty=1; ui_status("Scaled"); } } break;
        case MP_GRIDSP: { int v=parse_int(buf); if(v>=2) grid_sp=v; grid_on=1; ui_status("Grid set"); } break;
        case MP_LRENAME: { layer_t *L=&g_doc.layer[g_doc.active]; int i=0;
            for(; buf[i] && i<STUDIO_NAME_LEN-1; i++) L->name[i]=buf[i]; L->name[i]=0;
            ui_status("Renamed layer"); } break;
        case MP_AICMD: {
            ui_status("Asking AI...");
            char reply[256]; int rc=ai_command(buf, reply, sizeof(reply)); g_doc.comp_dirty=1;
            if(rc==0) open_msg("AI applied", reply[0]?reply:"Edit applied.");
            else if(rc==-1) open_msg("AI","Network or key error.");
            else open_msg("AI","Could not turn that into an edit. Try rephrasing.");
        } break;
        case MP_AIPAL: {
            ui_status("Asking AI...");
            uint32_t pal[8]; int n=ai_palette(buf, pal, 8);
            if(n>0){ cp_load_ai_palette(buf, pal, n); char s[48]; snprintf(s,sizeof(s),"AI palette: %d colors",n); open_msg("AI palette",s); }
            else open_msg("AI","No palette returned.");
        } break;
        default: break;
    }
    ui_full_redraw();
}

// ===========================================================================
// File browser modal (item 3): list dir, image-type filter, thumbnail preview,
// pick. Open / Save As / Export all route through it.
// ===========================================================================
#define FB_MAX 200
#define FBT    112                          // thumbnail preview edge (px)
#define FB_SBW 10                           // list scrollbar width (px)
static int   fb_open=0;
static int   fb_purpose=0;                  // MP_OPEN / MP_SAVEAS / MP_EXPORT
static char  fb_dir[128]="/HOME";
static int   fb_n=0, fb_selrow=-1, fb_scroll=0;
static int   fb_sb_drag=0;                  // 1 while dragging the list scrollbar thumb
static char  fb_name[FB_MAX][48];
static uint8_t fb_isdir[FB_MAX];
static char  fb_fname[96];                  // filename field (save/export/typed pick)
static textfield_t fb_ftf;
static uint32_t *fb_thumb=0;                // FBT*FBT preview buffer
static int   fb_thumb_ok=0;

static void sset(char*d,int cap,const char*s){ int i=0; if(s) while(s[i]&&i<cap-1){d[i]=s[i];i++;} d[i]=0; }
static int  fb_ext_ok(const char*n){
    int L=0; while(n[L])L++;
    for(int i=L-1;i>=0 && i>=L-6;i--){ if(n[i]=='.'){
        const char*e=&n[i+1]; char x[8]; int j=0; while(e[j]&&j<7){ char c=e[j]; if(c>='A'&&c<='Z')c+=32; x[j]=c; j++; } x[j]=0;
        return !strcmp(x,"bmp")||!strcmp(x,"dib")||!strcmp(x,"mstu")||!strcmp(x,"png")||!strcmp(x,"jpg")||!strcmp(x,"jpeg");
    } }
    return 0;
}
static void path_join(char*out,int cap,const char*dir,const char*name){
    int i=0; while(dir[i]&&i<cap-1){out[i]=dir[i];i++;}
    if(i>0 && out[i-1]!='/' && i<cap-1) out[i++]='/';
    int j=0; while(name[j]&&i<cap-1) out[i++]=name[j++];
    out[i]=0;
}
static void path_up(char*dir){
    int n=0; while(dir[n])n++;
    if(n>1 && dir[n-1]=='/') n--;
    while(n>1 && dir[n-1]!='/') n--;
    while(n>1 && dir[n-1]=='/') n--;
    dir[n]=0;
    if(dir[0]==0){ dir[0]='/'; dir[1]=0; }
}
static void fb_load_dir(void){
    fb_n=0; fb_selrow=-1; fb_scroll=0; fb_thumb_ok=0;
    if(fb_n<FB_MAX){ sset(fb_name[fb_n],48,".."); fb_isdir[fb_n]=1; fb_n++; }
    DIR*d=opendir(fb_dir);
    if(d){ struct dirent*e;
        while((e=readdir(d)) && fb_n<FB_MAX){
            if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
            int isdir=(e->d_type==DT_DIR);
            if(e->d_type==DT_UNKNOWN){ int hasdot=0; for(int i=0;e->d_name[i];i++) if(e->d_name[i]=='.'){hasdot=1;break;} isdir=!hasdot; }
            if(!isdir && !fb_ext_ok(e->d_name)) continue;
            sset(fb_name[fb_n],48,e->d_name); fb_isdir[fb_n]=isdir?1:0; fb_n++;
        }
        closedir(d);
    }
    // dirs first, then files; simple selection sort keeping the ".." at row 0
    for(int i=1;i<fb_n;i++) for(int j=i+1;j<fb_n;j++){
        int swap=0;
        if(fb_isdir[j] && !fb_isdir[i]) swap=1;
        else if(fb_isdir[j]==fb_isdir[i] && strcmp(fb_name[j],fb_name[i])<0) swap=1;
        if(swap){ char t[48]; sset(t,48,fb_name[i]); sset(fb_name[i],48,fb_name[j]); sset(fb_name[j],48,t);
                  uint8_t td=fb_isdir[i]; fb_isdir[i]=fb_isdir[j]; fb_isdir[j]=td; }
    }
}
static void fb_load_thumb(void){
    fb_thumb_ok=0;
    if(fb_fname[0]==0) return;
    if(!fb_thumb){ fb_thumb=(uint32_t*)malloc((size_t)FBT*FBT*4); if(!fb_thumb) return; }
    char full[192]; path_join(full,sizeof full,fb_dir,fb_fname);
    fb_thumb_ok=(io_thumb(full,fb_thumb,FBT,FBT)==0);
}
static void fb_start(int purpose){
    fb_purpose=purpose; fb_open=1;
    if(purpose==MP_EXPORT) sset(fb_fname,sizeof fb_fname,"IMAGE.PNG");
    else if(purpose==MP_EXPORTBMP) sset(fb_fname,sizeof fb_fname,"IMAGE.BMP");
    else if(purpose==MP_SAVEAS) sset(fb_fname,sizeof fb_fname,"DRAW.MSTU");
    else fb_fname[0]=0;
    tf_init(&fb_ftf, fb_fname, sizeof fb_fname);
    fb_load_dir();
    ui_full_redraw();
}
static void fb_enter(int row){
    if(row<0||row>=fb_n) return;
    if(fb_isdir[row]){
        if(!strcmp(fb_name[row],"..")) path_up(fb_dir);
        else { char nd[128]; path_join(nd,sizeof nd,fb_dir,fb_name[row]); sset(fb_dir,sizeof fb_dir,nd); }
        fb_load_dir(); ui_full_redraw();
    }else{
        fb_selrow=row; sset(fb_fname,sizeof fb_fname,fb_name[row]);
        tf_init(&fb_ftf, fb_fname, sizeof fb_fname);
        fb_load_thumb(); ui_full_redraw();
    }
}
// Open a path directly (used by main() when Studio is launched with a file, and
// anywhere a one-shot load is needed). Mirrors the file-browser Open branch.
void ui_open_path(const char *path){
    if(!path||!path[0]) return;
    if(io_load(path)==0){ sset(g_doc.path,sizeof g_doc.path,path); g_doc.comp_dirty=1; g_zoom_pct=100; pan_x=pan_y=0; ui_status("Opened"); }
    else ui_status("Open failed");
    ui_full_redraw();
}
static void fb_geom(int*bx,int*by,int*bw,int*bh){ *bw=620; *bh=452; *bx=(g_w-*bw)/2; *by=(g_h-*bh)/2; }
static void fb_commit(void){
    if(fb_fname[0]==0){ fb_open=0; ui_full_redraw(); return; }
    char full[192]; path_join(full,sizeof full,fb_dir,fb_fname);
    int purpose=fb_purpose; fb_open=0;
    if(purpose==MP_OPEN){
        if(io_load(full)==0){ sset(g_doc.path,sizeof g_doc.path,full); g_doc.comp_dirty=1; g_zoom_pct=100; pan_x=pan_y=0; ui_status("Opened"); }
        else ui_status("Open failed");
    }else{
        if(io_save(full)==0){ sset(g_doc.path,sizeof g_doc.path,full); g_doc.modified=0; ui_status((purpose==MP_EXPORT||purpose==MP_EXPORTBMP)?"Exported":"Saved"); }
        else ui_status("Save failed");
    }
    ui_full_redraw();
}
// List scrollbar geometry. Returns the max scroll (fb_n - visible rows), or 0
// when the list fits and no scrollbar is needed. Fills the track rect (sx..sh)
// and the proportional thumb (tby/tbh).
static int fb_scrollbar(int*sx,int*sy,int*sw,int*sh,int*tby,int*tbh){
    int bx,by,bw,bh; fb_geom(&bx,&by,&bw,&bh);
    int lx=bx+14, ly=by+60, lw=bw-28-FBT-24, lh=bh-60-84;
    int rowh=20, vis=lh/rowh;
    int maxs=fb_n-vis; if(maxs<1) return 0;
    *sx=lx+lw-FB_SBW; *sy=ly; *sw=FB_SBW; *sh=lh;
    int th=lh*vis/(fb_n>0?fb_n:1); if(th<16) th=16; if(th>lh) th=lh;
    int sc=clampi(fb_scroll,0,maxs);
    int ty=ly + (lh-th)*sc/maxs;
    if(ty<ly) ty=ly; if(ty>ly+lh-th) ty=ly+lh-th;
    *tby=ty; *tbh=th;
    return maxs;
}
// Map a pointer Y (during a track click/drag) to a clamped fb_scroll value.
static void fb_sb_to(int my){
    int sx,sy,sw,sh,tby,tbh; int maxs=fb_scrollbar(&sx,&sy,&sw,&sh,&tby,&tbh);
    if(maxs<=0) return;
    int denom=sh-tbh; if(denom<1) denom=1;
    fb_scroll=clampi((my - sy - tbh/2)*maxs/denom, 0, maxs);
}
static void draw_filebrowser(void){
    if(!fb_open) return;
    int bx,by,bw,bh; fb_geom(&bx,&by,&bw,&bh);
    R(bx-2,by-2,bw+4,bh+4,C_LINE); R(bx,by,bw,bh,C_PANEL); OUT(bx,by,bw,bh,C_ACCENT);
    const char*title=(fb_purpose==MP_OPEN)?"Open Image":(fb_purpose==MP_EXPORT)?"Export PNG":(fb_purpose==MP_EXPORTBMP)?"Export BMP":"Save As";
    T(bx+14,by+12,title,C_ACCENT2);
    R(bx+14,by+32,bw-28,20,C_PANEL2); OUT(bx+14,by+32,bw-28,20,C_LINE);
    Ts(bx+20,by+37,fb_dir,C_DIM);
    int lx=bx+14, ly=by+60, lw=bw-28-FBT-24, lh=bh-60-84;
    R(lx,ly,lw,lh,C_PANEL2); OUT(lx,ly,lw,lh,C_LINE);
    int sbx,sby,sbw,sbh,tby,tbh; int sbmax=fb_scrollbar(&sbx,&sby,&sbw,&sbh,&tby,&tbh);
    int iw=lw-2-(sbmax>0?FB_SBW:0);    // row highlight width, clearing the scrollbar gutter
    int rowh=20, vis=lh/rowh;
    for(int r=0;r<vis;r++){ int i=fb_scroll+r; if(i>=fb_n) break;
        int ry=ly+r*rowh;
        if(i==fb_selrow) vgrad(lx+1,ry,iw,rowh,C_ACCENT2,C_ACCENT);
        else if(IN(g_mx,g_my,lx+1,ry,iw,rowh)) R(lx+1,ry,iw,rowh,C_BTN_HOV);
        uint32_t tc=(i==fb_selrow)?0x00ffffff:C_TEXT;
        if(fb_isdir[i]){ R(lx+8,ry+6,11,8,0x00d8b45a); R(lx+8,ry+4,6,3,0x00d8b45a); }
        else { R(lx+9,ry+4,8,12,mix(C_DIM,0x00ffffff,20)); R(lx+9,ry+4,8,2,0x00ffffff); R(lx+13,ry+4,4,4,C_PANEL); }
        T(lx+26,ry+3,fb_name[i],tc);
    }
    if(sbmax>0){                        // track + proportional thumb
        R(sbx,sby,sbw,sbh,C_FIELD); OUT(sbx,sby,sbw,sbh,C_LINE);
        int hov=IN(g_mx,g_my,sbx,tby,sbw,tbh)||fb_sb_drag;
        vgrad(sbx+1,tby,sbw-2,tbh, mix(hov?C_BTN_HOV:C_BTN,0x00ffffff,24), hov?C_BTN:C_PANEL2);
        OUT(sbx+1,tby,sbw-2,tbh,C_LINE);
    }
    // preview panel
    int pxp=lx+lw+12, pyp=ly;
    R(pxp,pyp,FBT,FBT,C_CANVASBG); OUT(pxp,pyp,FBT,FBT,C_LINE);
    if(fb_thumb_ok && fb_thumb){
        for(int yy=0;yy<FBT;yy++) for(int xx=0;xx<FBT;xx++)
            win_draw_pixel(g_win,pxp+xx,pyp+yy, fb_thumb[yy*FBT+xx]|0xFF000000u);
    }else gui_text_ttf_centered(g_win,pxp,pyp+FBT/2-8,FBT,16,"No preview",C_DIM,12);
    // filename field
    int fy=by+bh-58;
    Ts(bx+14,fy-14,"File name:",C_DIM);
    R(bx+14,fy,bw-28,24,C_PANEL2); OUT(bx+14,fy,bw-28,24,C_LINE);
    T(bx+20,fy+4,fb_fname,C_TEXT);
    int cw=gui_string_width(fb_fname); R(bx+20+cw,fy+3,2,18,C_ACCENT2);
    button(bx+bw-172,by+bh-30,76,24,"Cancel",0,0);
    button(bx+bw-88,by+bh-30,76,24,(fb_purpose==MP_OPEN)?"Open":"Save",0,0);
}
static int click_filebrowser(int mx,int my){
    if(!fb_open) return 0;
    int bx,by,bw,bh; fb_geom(&bx,&by,&bw,&bh);
    if(IN(mx,my,bx+bw-172,by+bh-30,76,24)){ fb_open=0; ui_full_redraw(); return 1; }
    if(IN(mx,my,bx+bw-88,by+bh-30,76,24)){ fb_commit(); return 1; }
    // scrollbar track: click/drag jumps the list (handle before the row hit-test)
    int sbx,sby,sbw,sbh,tby,tbh; int sbmax=fb_scrollbar(&sbx,&sby,&sbw,&sbh,&tby,&tbh);
    if(sbmax>0 && IN(mx,my,sbx,sby,sbw,sbh)){ fb_sb_drag=1; fb_sb_to(my); ui_full_redraw(); return 1; }
    int lx=bx+14, ly=by+60, lw=bw-28-FBT-24, lh=bh-60-84;
    if(IN(mx,my,lx,ly,lw,lh)){ int r=(my-ly)/20; fb_enter(fb_scroll+r); return 1; }
    return 1;   // consume all clicks inside the modal region
}

// ===========================================================================
// Print / Preview modal. Shows the flattened document laid out on a page, then
// submits it over IPP (io_print) or hands off to Export for print-to-file. All
// pixels reuse the shared blit path (win_draw_image); nothing bespoke.
// ===========================================================================
static void pr_geom(int*bx,int*by,int*bw,int*bh){
    int w=600, h=560;
    if(w>g_w-20) w=g_w-20;
    if(h>g_h-20) h=g_h-20;
    *bw=w; *bh=h; *bx=(g_w-w)/2; *by=(g_h-h)/2;
}
static void open_print_preview(void){
    doc_composite();
    pr_have=io_printer_default(pr_printer,sizeof pr_printer);
    pr_open=1; menu_open=-1; ui_full_redraw();
}
static void draw_print_preview(void){
    if(!pr_open) return;
    doc_composite();
    int bx,by,bw,bh; pr_geom(&bx,&by,&bw,&bh);
    R(bx-2,by-2,bw+4,bh+4,C_LINE); R(bx,by,bw,bh,C_PANEL); OUT(bx,by,bw,bh,C_ACCENT);
    T(bx+16,by+12,"Print / Preview",C_ACCENT2);
    // Grey pasteboard behind the "paper".
    int pax=bx+20, pay=by+40, paw=bw-40, pah=bh-150;
    R(pax,pay,paw,pah,0x00585858);
    int dw=g_doc.w, dh=g_doc.h; if(dw<1)dw=1; if(dh<1)dh=1;
    int availw=paw-56, availh=pah-56;
    int rw=availw, rh=(int)((long)availw*dh/dw);
    if(rh>availh){ rh=availh; rw=(int)((long)availh*dw/dh); }
    if(rw<1)rw=1; if(rh<1)rh=1;
    int px0=pax+(paw-rw)/2, py0=pay+(pah-rh)/2;
    R(px0+6,py0+6,rw,rh,0x00202020);              // page drop shadow
    R(px0-8,py0-8,rw+16,rh+16,0x00ffffff);        // white paper + margin
    uint32_t *comp=g_doc.comp;
    if(comp && (long)rw*rh<=(long)STUDIO_MAX_W*STUDIO_MAX_H){
        uint32_t *buf=(uint32_t*)malloc((size_t)rw*rh*4);
        if(buf){
            for(int yy=0;yy<rh;yy++){ int sy=yy*dh/rh; if(sy>=dh)sy=dh-1;
                for(int xx=0;xx<rw;xx++){ int sx=xx*dw/rw; if(sx>=dw)sx=dw-1;
                    buf[yy*rw+xx]=comp[(long)sy*dw+sx]; } }
            win_draw_image(g_win,px0,py0,rw,rh,buf);
            free(buf);
        }
    }
    char info[128];
    snprintf(info,sizeof info,"Image %dx%d px    Fit to page    Printer: %s",
             g_doc.w,g_doc.h, pr_have?pr_printer:"none configured");
    Ts(bx+20,by+bh-96,info,C_DIM);
    if(!pr_have)
        Ts(bx+20,by+bh-78,"Add a printer in Settings > Devices, or use Print to File.",C_DIM);
    button(bx+20,     by+bh-44,120,28,"Print",0,0);
    button(bx+152,    by+bh-44,132,28,"Print to File..",0,0);
    button(bx+bw-108, by+bh-44,92,28,"Close",0,0);
}
static int click_print_preview(int mx,int my){
    if(!pr_open) return 0;
    int bx,by,bw,bh; pr_geom(&bx,&by,&bw,&bh);
    if(IN(mx,my,bx+20,by+bh-44,120,28)){
        if(pr_have){
            ui_status("Sending to printer...");
            int rc=io_print(0);
            pr_open=0; ui_full_redraw();
            ui_status(rc==0?"Sent to printer":"Print failed");
        }else{
            ui_status("No printer configured - use Print to File");
        }
        return 1;
    }
    if(IN(mx,my,bx+152,by+bh-44,132,28)){ pr_open=0; fb_start(MP_EXPORT); return 1; }
    if(IN(mx,my,bx+bw-108,by+bh-44,92,28)){ pr_open=0; ui_full_redraw(); return 1; }
    return 1;   // modal: consume all clicks
}

// ===========================================================================
// Unsaved-changes guard (item 3): Save / Discard / Cancel before a destructive
// action (New / Open / Revert / Quit).
// ===========================================================================
static int guard_open=0, guard_action=0;
static void guard_open_for(int action){ guard_open=1; guard_action=action; menu_open=-1; ui_full_redraw(); }
static void guard_geom(int*bx,int*by,int*bw,int*bh){ *bw=420; *bh=150; *bx=(g_w-*bw)/2; *by=(g_h-*bh)/2; }
static void draw_guard(void){
    if(!guard_open) return;
    int bx,by,bw,bh; guard_geom(&bx,&by,&bw,&bh);
    R(bx-2,by-2,bw+4,bh+4,C_LINE); R(bx,by,bw,bh,C_PANEL); OUT(bx,by,bw,bh,C_ACCENT);
    T(bx+16,by+16,"Unsaved changes",C_ACCENT2);
    Ts(bx+16,by+46,"This image has unsaved changes.",C_TEXT);
    Ts(bx+16,by+64,"Save them before continuing?",C_TEXT);
    button(bx+16,      by+bh-38,110,26,"Save",0,0);
    button(bx+bw/2-55, by+bh-38,110,26,"Discard",0,0);
    button(bx+bw-126,  by+bh-38,110,26,"Cancel",0,0);
}
static void guard_proceed(void){
    int act=guard_action; guard_open=0; g_in_guarded=1; do_action(act); g_in_guarded=0;
}
static int click_guard(int mx,int my){
    if(!guard_open) return 0;
    int bx,by,bw,bh; guard_geom(&bx,&by,&bw,&bh);
    if(IN(mx,my,bx+16,by+bh-38,110,26)){                 // Save
        guard_open=0;
        if(g_doc.path[0]){ if(io_save(g_doc.path)==0) g_doc.modified=0; guard_proceed(); }
        else { fb_start(MP_SAVEAS); }                    // no path: get one, drop pending
        return 1;
    }
    if(IN(mx,my,bx+bw/2-55,by+bh-38,110,26)){ guard_proceed(); return 1; }   // Discard
    if(IN(mx,my,bx+bw-126,by+bh-38,110,26)){ guard_open=0; g_want_quit=0; ui_full_redraw(); return 1; } // Cancel
    return 1;
}

// ---------------------------------------------------------------------------
// Chrome hit testing
// ---------------------------------------------------------------------------
static int click_optbar(int mx,int my){
    int y0=MENU_H; if(my<y0||my>=y0+OPT_H) return 0;
    if(bp_shown){
        if(IN(mx,my,bp_x0,bp_y,bp_sz,bp_sz)){ g_tool.hardness=255; draw_optbar(); win_invalidate(g_win); return 1; }
        if(IN(mx,my,bp_x1,bp_y,bp_sz,bp_sz)){ g_tool.hardness=60;  draw_optbar(); win_invalidate(g_win); return 1; }
    }
    for(int i=0;i<g_noc;i++){
        optctl_t*o=&g_oc[i];
        if(!IN(mx,my,o->x,o->y,o->w,o->h)) continue;
        if(o->type==OC_SLIDER && o->val){
            int v=(mx-o->x)*o->vmax/(o->w>0?o->w:1);
            *o->val=clampi(v,0,o->vmax); opt_drag=i;
            draw_optbar(); win_invalidate(g_win); return 1;
        }else if(o->type==OC_CYCLE && o->val){
            *o->val=(*o->val+1)%(o->nch>0?o->nch:1);
            draw_optbar(); win_invalidate(g_win); return 1;
        }else if(o->type==OC_TOGGLE && o->val){
            *o->val = !*o->val;
            draw_optbar(); win_invalidate(g_win); return 1;
        }else if(o->type==OC_SWAP){
            uint32_t t=g_tool.fg; g_tool.fg=g_tool.bg; g_tool.bg=t;
            draw_optbar(); draw_dock(); win_invalidate(g_win); return 1;
        }else if(o->type==OC_DEFCOL){
            g_tool.fg=argb(255,0,0,0); g_tool.bg=argb(255,255,255,255);
            draw_optbar(); draw_dock(); win_invalidate(g_win); return 1;
        }
    }
    return 1;   // consume any click on the options bar
}
// Granular zoom control in the status bar (item 2): readout, -/+ steppers,
// continuous slider, and Fit / 1:1 quick buttons.
static int click_status(int mx,int my){
    if(my<g_h-STATUS_H) return 0;
    if(IN(mx,my,zc_minus_x,zc_y,zc_step_w,zc_h)){ zoom_step(-1); ui_full_redraw(); return 1; }
    if(IN(mx,my,zc_plus_x,zc_y,zc_step_w,zc_h)){ zoom_step(+1); ui_full_redraw(); return 1; }
    if(IN(mx,my,zc_fit_x,zc_y,zc_btn_w,zc_h)){ g_zoom_pct=zoom_fit_pct(); pan_x=pan_y=0; ui_full_redraw(); return 1; }
    if(IN(mx,my,zc_100_x,zc_y,zc_btn_w,zc_h)){ zoom_to_center(100); ui_full_redraw(); return 1; }
    if(IN(mx,my,zc_slider_x,zc_y,zc_slider_w,zc_h)){
        int sw=zc_slider_w>10?zc_slider_w-8:1;
        int np=ZOOM_MIN+(mx-zc_slider_x)*(ZOOM_MAX-ZOOM_MIN)/sw;
        zoom_slider_drag=1; zoom_to_center(clampi(np,ZOOM_MIN,ZOOM_MAX)); ui_full_redraw(); return 1;
    }
    return 1;   // consume clicks anywhere on the status bar
}
static int click_strip(int mx,int my){
    if(mx>=STRIP_W || my<MENU_H+OPT_H) return 0;
    int bw=20,bh=20,x0=3,y0=MENU_H+OPT_H+4;
    for(int i=0;i<TL_COUNT;i++){
        int col=i&1,row=i>>1; int x=x0+col*(bw+2), y=y0+row*(bh+2);
        if(IN(mx,my,x,y,bw,bh)){ g_tool.id=(tool_id_t)i; draw_strip(); draw_optbar(); win_invalidate(g_win); return 1; }
    }
    return 1;
}
static int click_dock(int mx,int my){
    int dx=g_w-DOCK_W; if(mx<dx) return 0;
    // section header toggles (collapse/expand)
    for(int s=0;s<SEC_N;s++)
        if(IN(mx,my,dx+4,sec_hdr_y[s],DOCK_W-8,18)){ SEC_COL[s]=!SEC_COL[s]; ui_full_redraw(); return 1; }

    // ===== Layers =====
    if(!SEC_COL[SEC_LAYERS]){
        const int acts[6]={A_LNEW,A_LDUP,A_LDEL,A_LUP,A_LDOWN,A_LMERGE};
        for(int i=0;i<6;i++){ if(IN(mx,my,dx+8+i*29,LP_y,26,20)){ do_action(acts[i]); return 1; } }
        int y=LP_y+24, maxrows=g_doc.nlayers, shown=0;
        for(int li=g_doc.nlayers-1; li>=0 && shown<maxrows; li--, shown++){
            int rh=38, ry=y;
            if(IN(mx,my,dx+50,ry+3,16,16)){ layer_t*L=&g_doc.layer[li]; L->visible=!L->visible; g_doc.comp_dirty=1; ui_full_redraw(); return 1; }
            if(IN(mx,my,dx+8,ry,DOCK_W-16,rh)){
                if(g_doc.active==li && mx>=dx+88)         // click active layer's name -> rename
                    open_modal_text(MP_LRENAME,"Rename layer:",g_doc.layer[li].name);
                else { g_doc.active=li; ui_full_redraw(); }
                return 1;
            }
            y+=rh+3;
        }
        // opacity slider (click or begin drag) -> layer_set_opacity
        if(IN(mx,my,dx+56,LO_y-4,DOCK_W-64,24)){
            int sx=dx+62, sw=DOCK_W-100; int v=clampi((mx-sx)*255/(sw>12?sw-12:1),0,255);
            layer_set_opacity(g_doc.active,v); g_doc.comp_dirty=1; lop_drag=1; ui_full_redraw(); return 1;
        }
        // blend mode ("Mode:") control + mask row
        if(IN(mx,my,dx+52,LMODE_y,DOCK_W-62,18)){ do_action(A_LBLENDPOP); return 1; }
        if(IN(mx,my,dx+90,LM_y,24,18)){ do_action(A_LMASKADD); return 1; }
        if(IN(mx,my,dx+116,LM_y,24,18)){ do_action(A_LMASKAPPLY); return 1; }
        if(IN(mx,my,dx+142,LM_y,24,18)){ do_action(A_LMASKDEL); return 1; }
        if(IN(mx,my,dx+168,LM_y,18,18)){ do_action(A_LMASKTOG); return 1; }
        if(IN(mx,my,dx+8,LM_y+20,46,16)){ do_action(A_LLOCKA); return 1; }
    }
    // ===== Channels =====
    if(!SEC_COL[SEC_CHANNELS]){
        for(int ci=0; ci<4; ci++){
            if(IN(mx,my,dx+6,CH_y+ci*19,DOCK_W-12,18)){ g_chan_vis[ci]=!g_chan_vis[ci]; ui_full_redraw(); return 1; }
        }
        if(IN(mx,my,dx+DOCK_W-58,CHSB_y,50,16)){ do_action(A_CHSAVE); return 1; }
        int cy=CHS_y, nc=channel_count();
        for(int i=0;i<nc && i<3;i++){ if(IN(mx,my,dx+14,cy,DOCK_W-20,14)){ channel_load_selection(i); ui_status("Loaded channel"); ui_full_redraw(); return 1; } cy+=14; }
    }
    // ===== Paths =====
    if(!SEC_COL[SEC_PATHS]){
        if(IN(mx,my,dx+8,PA_y,40,16)){ path_reset(); ui_status("Path cleared"); ui_full_redraw(); return 1; }
        if(IN(mx,my,dx+52,PA_y,50,16)){ undo_push("Stroke Path"); path_stroke(g_tool.size, g_tool.fg); g_doc.comp_dirty=1; ui_full_redraw(); return 1; }
        if(IN(mx,my,dx+8,PA_y+18,60,16)){ path_to_selection(0); ui_status("Path to selection"); ui_full_redraw(); return 1; }
    }
    // ===== Histogram =====
    if(!SEC_COL[SEC_HIST]){
        if(IN(mx,my,dx+DOCK_W-108,HI_y,32,16)){ hist_log=!hist_log; ui_full_redraw(); return 1; }
        for(int i=0;i<4;i++){ if(IN(mx,my,dx+DOCK_W-70+i*17,HI_y,15,16)){ hist_ch=i; ui_full_redraw(); return 1; } }
    }
    // ===== Color (professional picker) =====
    if(!SEC_COL[SEC_COLOR]){
        if(cp_click_dock(dx,CP_y,mx,my,mod_shift,mod_alt)) return 1;
    }
    // ===== Swatches / palettes =====
    if(!SEC_COL[SEC_SWATCHES]){
        int r=cp_click_pal(dx,SW_y,mx,my,mod_shift,mod_alt);
        if(r==2){ do_action(A_AIPAL); return 1; }
        if(r) return 1;
    }
    // ===== Brushes =====
    if(!SEC_COL[SEC_BRUSHES]){
        int total=1+brush_count();
        for(int c=0;c<total;c++){
            int cxp=dx+8+(c%BPCOLS)*BPSTRIDE, cyp=BR_y+(c/BPCOLS)*BPSTRIDE;
            if(IN(mx,my,cxp,cyp,BPCELL,BPCELL)){ brush_set(c-1); draw_optbar(); ui_full_redraw(); return 1; }
        }
    }
    // ===== Patterns =====
    if(!SEC_COL[SEC_PATTERNS]){
        int total=1+pattern_count();
        for(int c=0;c<total;c++){
            int cxp=dx+8+(c%BPCOLS)*BPSTRIDE, cyp=PT_y+(c/BPCOLS)*BPSTRIDE;
            if(IN(mx,my,cxp,cyp,BPCELL,BPCELL)){
                pattern_set(c-1);
                g_brush_pattern_fill = (c-1>=0) ? 1 : 0;   // pattern active -> bucket uses pattern
                ui_full_redraw(); return 1;
            }
        }
    }
    return 1;
}
static int click_menubar(int mx,int my){
    if(my>=MENU_H) return 0;
    for(int i=0;i<NMENU;i++){ if(IN(mx,my,menu_x(i),0,menu_w(i),MENU_H)){ menu_open=(menu_open==i)?-1:i; submenu_cat=-1; ui_full_redraw(); return 1; } }
    menu_open=-1; return 0;
}

// Dropdown click. Returns action, A_NONE, or -2 (kept open).
static int click_dropdown(int mx,int my){
    if(menu_open<0) return A_NONE;
    menu_t*m=&MENUS[menu_open];
    int n=m->n, x=menu_x(menu_open), y=MENU_H;
    int w=dropdown_w(menu_open); int h=n*20+6;
    // Filters submenu region
    if(m->kind==MK_FILTERS && submenu_cat>=0 && submenu_cat<g_ncats){
        const char*cat=g_cats[submenu_cat]; int cn=cat_op_count(cat);
        int sx=x+w+2, sy=y+submenu_cat*20;
        int sw=0; for(int i=0;i<cn;i++){ int gi=nth_cat_op(cat,i); const studio_op_t*o=gi>=0?studio_op_get(gi):0; if(o){int lw=gui_string_width(o->name); if(lw>sw)sw=lw;} }
        sw+=24; if(sw<120)sw=120; int sh=cn*20+6;
        if(IN(mx,my,sx,sy,sw,sh)){
            int idx=(my-(sy+4))/20; if(idx<0||idx>=cn) return -2;
            int gi=nth_cat_op(cat,idx); menu_open=-1; submenu_cat=-1;
            return (gi>=0)? (A_OP_BASE+gi) : A_NONE;
        }
    }
    if(!IN(mx,my,x,y,w,h)){ menu_open=-1; submenu_cat=-1; return A_NONE; }
    int idx=(my-(y+4))/20; if(idx<0||idx>=n){ return -2; }
    if(m->kind==MK_STATIC){ int act=m->items[idx].action; if(act==A_SEP||action_disabled(act)) return -2; menu_open=-1; return act; }
    if(m->kind==MK_COLORS){ int gi=nth_color_op(idx); menu_open=-1; return (gi>=0)?(A_OP_BASE+gi):A_NONE; }
    // MK_FILTERS: open the submenu for this category
    submenu_cat=idx; ui_full_redraw(); return -2;
}

// ---------------------------------------------------------------------------
// Canvas interaction
// ---------------------------------------------------------------------------
static int is_shape_tool(tool_id_t t){
    return t==TL_LINE||t==TL_RECT||t==TL_ELLIPSE||t==TL_GRADIENT||
           t==TL_SEL_RECT||t==TL_SEL_ELLIPSE||t==TL_CROP;
}
// Selection-mode combine (item 1): New replaces, Add/Subtract/Intersect merge
// the new shape with the pre-existing selection captured in sel_combine_before.
// Effective selection combine mode for the in-progress op: the optbar Mode,
// overridden live by Shift (add) / Alt (subtract) / Shift+Alt (intersect).
static uint8_t *g_sel_snap=0; static int g_sel_snap_cap=0;
static int g_sel_eff_mode=0;
static int sel_effective_mode(void){
    if(mod_shift && mod_alt) return 3;   // intersect
    if(mod_shift)            return 1;   // add
    if(mod_alt)              return 2;   // subtract
    return g_tool.sel_mode;
}
static void sel_combine_before(void){
    g_sel_eff_mode=sel_effective_mode();
    if(g_sel_eff_mode==0) return;
    int n=g_doc.w*g_doc.h; if(n<1) return;
    if(n>g_sel_snap_cap){ if(g_sel_snap) free(g_sel_snap); g_sel_snap=(uint8_t*)malloc(n); g_sel_snap_cap=g_sel_snap?n:0; }
    if(!g_sel_snap) return;
    if(g_doc.sel_active && g_doc.sel) memcpy(g_sel_snap,g_doc.sel,n); else memset(g_sel_snap,0,n);
}
static void sel_combine_after(void){
    if(g_sel_eff_mode==0) return;
    if(!g_sel_snap || !g_doc.sel) return;
    int n=g_doc.w*g_doc.h;
    for(int i=0;i<n;i++){ int a=g_sel_snap[i], b=g_doc.sel[i], r;
        switch(g_sel_eff_mode){ case 1: r=a>b?a:b; break; case 2: r=a>b?a-b:0; break; case 3: r=a<b?a:b; break; default: r=b; }
        g_doc.sel[i]=(uint8_t)r; }
    g_doc.sel_active=1;
}
// Commit the text currently being typed (if any) onto the active layer at its
// anchor, then leave typing mode. tool_begin/tool_end route TL_TEXT through the
// TTF text_commit() with a single undo step.
static void text_finish(void){
    if(text_typing && g_tool.text[0]){
        tool_begin(text_ax,text_ay); tool_end(text_ax,text_ay);
        g_doc.comp_dirty=1;
    }
    text_typing=0; g_tool.text[0]=0;
}
static void canvas_down(int mx,int my){
    int cx,cy; screen_to_canvas(mx,my,&cx,&cy);
    tool_id_t t=g_tool.id;
    if(t==TL_TEXT){
        // Commit any in-progress text, then drop a new caret here and type.
        if(text_typing && g_tool.text[0]){ tool_begin(text_ax,text_ay); tool_end(text_ax,text_ay); g_doc.comp_dirty=1; }
        text_typing=1; text_ax=cx; text_ay=cy; g_tool.text[0]=0;
        tf_init(&g_text_tf, g_tool.text, (int)sizeof(g_tool.text));
        ui_full_redraw(); return;
    }
    if(t==TL_SEL_LASSO||t==TL_PATH){ shaping=0; if(t==TL_SEL_LASSO){ sel_combine_before(); sel_lasso_begin(cx,cy);} else { path_reset(); path_add_node(cx,cy,0,0,0,0);} painting=2; return; }
    if(t==TL_SEL_WAND){ sel_combine_before(); sel_wand(cx,cy,g_tool.wand_tolerance?g_tool.wand_tolerance:32); sel_combine_after(); ui_full_redraw(); return; }
    if(t==TL_SEL_BYCOLOR){ sel_combine_before(); sel_by_color(cx,cy,g_tool.wand_tolerance?g_tool.wand_tolerance:32); sel_combine_after(); ui_full_redraw(); return; }
    if(is_shape_tool(t)){ shaping=1; sh_x0=sh_x1=cx; sh_y0=sh_y1=cy; return; }
    undo_push(tool_name(t));
    tool_begin(cx,cy); painting=1; g_doc.comp_dirty=1; redraw_canvas_only();
}
static void canvas_move(int mx,int my){
    int cx,cy; screen_to_canvas(mx,my,&cx,&cy);
    if(painting==2){ if(g_tool.id==TL_PATH) path_add_node(cx,cy,0,0,0,0); else sel_lasso_point(cx,cy); return; }
    if(shaping){ sh_x1=cx; sh_y1=cy; redraw_canvas_only(); return; }
    if(painting==1){ tool_drag(cx,cy); g_doc.comp_dirty=1; redraw_canvas_only(); }
}
static void canvas_up(int mx,int my){
    int cx,cy; screen_to_canvas(mx,my,&cx,&cy);
    tool_id_t t=g_tool.id;
    if(painting==2){ if(t==TL_PATH){ path_close(); ui_status("Path closed"); } else { sel_lasso_end(); sel_combine_after(); } painting=0; ui_full_redraw(); return; }
    if(shaping){
        shaping=0;
        if(t==TL_SEL_RECT){ sel_combine_before(); sel_rect(sh_x0,sh_y0,cx,cy,g_tool.feather); sel_combine_after(); }
        else if(t==TL_SEL_ELLIPSE){ sel_combine_before(); sel_ellipse(sh_x0,sh_y0,cx,cy,g_tool.feather); sel_combine_after(); }
        else if(t==TL_CROP){ sel_rect(sh_x0,sh_y0,cx,cy,0); undo_push("Crop"); doc_crop_to_selection(); g_zoom_pct=100; pan_x=pan_y=0; }
        else{ undo_push(tool_name(t)); tool_begin(sh_x0,sh_y0); tool_drag(cx,cy); tool_end(cx,cy); g_doc.comp_dirty=1; }
        ui_full_redraw(); return;
    }
    if(painting==1){ tool_end(cx,cy); painting=0; g_doc.comp_dirty=1; ui_full_redraw(); }
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
static int handle_key(gui_event_t *e){
    char ch=e->key_char;
    if(ch==26){ do_action(A_UNDO); return 1; }
    if(ch==25){ do_action(A_REDO); return 1; }
    if(ch==1){ do_action(A_SELALL); return 1; }
    if(ch==4){ do_action(A_SELNONE); return 1; }
    if(ch==9){ do_action(A_SELINV); return 1; }
    if(ch==24){ do_action(A_CUT); return 1; }
    if(ch==3){ do_action(A_COPY); return 1; }
    if(ch==22){ do_action(A_PASTE); return 1; }
    if(ch==27){ if(menu_open>=0||blend_pop){menu_open=-1;blend_pop=0;submenu_cat=-1; ui_full_redraw(); return 1;} return 0; }
    switch(ch){
        case 'b': g_tool.id=TL_BRUSH; break;
        case 'p': g_tool.id=TL_PENCIL; break;
        case 'e': g_tool.id=TL_ERASER; break;
        case 'a': g_tool.id=TL_AIRBRUSH; break;
        case 'n': g_tool.id=TL_CLONE; break;
        case 's': g_tool.id=TL_SMUDGE; break;
        case 'u': g_tool.id=TL_BLUR; break;
        case 'f': g_tool.id=TL_FILL; break;
        case 'g': g_tool.id=TL_GRADIENT; break;
        case 'l': g_tool.id=TL_LINE; break;
        case 'r': g_tool.id=TL_RECT; break;
        case 'o': g_tool.id=TL_ELLIPSE; break;
        case 't': g_tool.id=TL_TEXT; break;
        case 'm': g_tool.id=TL_MOVE; break;
        case 'k': g_tool.id=TL_PICK; break;
        case 'h': g_tool.id=TL_HEAL; break;
        case 'd': g_tool.id=TL_DODGE; break;
        case 'c': g_tool.id=TL_CROP; break;
        case 'w': g_tool.id=TL_SEL_WAND; break;
        case 'q': g_tool.id=TL_SEL_RECT; break;
        case 'x': { uint32_t t=g_tool.fg; g_tool.fg=g_tool.bg; g_tool.bg=t; } break;
        case '+': case '=': zoom_step(+1); break;
        case '-': case '_': zoom_step(-1); break;
        case '[': g_tool.size=clampi(g_tool.size-1,1,64); break;
        case ']': g_tool.size=clampi(g_tool.size+1,1,64); break;
        default: return 1;
    }
    ui_full_redraw(); return 1;
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------
void ui_init(int win_handle, int win_w, int win_h){
    g_win=win_handle; g_w=win_w; g_h=win_h;
    theme_sync();   // chrome palette follows the active OS style engine
    cats_init();
    menus_init();
    g_tool.id=TL_BRUSH; g_tool.fg=0x00101010; g_tool.bg=0x00ffffff;
    g_tool.size=8; g_tool.opacity=255; g_tool.hardness=200; g_tool.wand_tolerance=32;
    g_tool.flow=255; g_tool.sel_mode=0; g_tool.feather=0;
    g_tool.grad_blend=BLEND_NORMAL; grad_mode_idx=0;
    g_tool.text[0]=0;
    g_zoom_pct=100; pan_x=pan_y=0;
    cp_init(win_handle);   // color picker + palette engine (loads /CONFIG/PALETTE.GPL)
}

int ui_handle_event(void *evp){
    gui_event_t *e=(gui_event_t*)evp;
    switch(e->type){
        case EVENT_RESIZE:
            if(e->mouse_x>200 && e->mouse_y>200){ g_w=e->mouse_x; g_h=e->mouse_y; }
            ui_full_redraw(); break;
        case EVENT_REDRAW: ui_full_redraw(); break;
        case EVENT_WINDOW_CLOSE: return 0;
        case EVENT_KEY_UP:
            if(e->keycode==0x2A||e->keycode==0x36) mod_shift=0;
            if(e->keycode==0x38) mod_alt=0;
            if(e->key_char==' '){ space_down=0; } break;
        case EVENT_KEY_DOWN:
            if(e->keycode==0x2A||e->keycode==0x36){ mod_shift=1; return 1; }
            if(e->keycode==0x38){ mod_alt=1; return 1; }
            if(cp_modal_open()){ cp_key_modal(e); return 1; }
            if(cp_key_dock(e)) return 1;   // dock hex field, when focused
            if(guard_open){ if(e->key_char==27){ guard_open=0; g_want_quit=0; ui_full_redraw(); } return 1; }
            if(fb_open){ char ch=e->key_char;
                if(ch=='\n'||ch=='\r'||e->keycode==0x1C){ fb_commit(); return g_want_quit?0:1; }
                if(ch==27){ fb_open=0; ui_full_redraw(); return 1; }
                tf_handle_key(&fb_ftf,e); fb_selrow=-1; fb_thumb_ok=0; draw_filebrowser(); win_invalidate(g_win); return 1; }
            if(pr_open){ if(e->key_char==27){ pr_open=0; ui_full_redraw(); } return 1; }
            if(pd_open){ char ch=e->key_char;
                if(ch=='\n'||ch=='\r'||e->keycode==0x1C){ pd_ok(); return 1; }
                if(ch==27){ pd_cancel(); return 1; }
                if(pd_has_kernel()){                     // type into the selected SP_KERNEL cell
                    int v=kw_cell[kw_sel], changed=0;
                    if(ch>='0'&&ch<='9'){ int d=ch-'0'; v=(v<0)?v*10-d:v*10+d; v=clampi(v,-999,999); changed=1; }
                    else if(ch=='-'){ v=-v; changed=1; }
                    else if(ch==8||ch==127){ v/=10; changed=1; }
                    if(changed){ kw_cell[kw_sel]=v; pd_preview(); }
                }
                return 1; }
            if(modal==1){ char ch=e->key_char;
                if(ch=='\n'||ch=='\r'||e->keycode==0x1C){ modal_confirm(); return 1; }
                if(ch==27){ modal=0; ui_full_redraw(); return 1; }
                tf_handle_key(&modal_tf, e); draw_modal(); win_invalidate(g_win); return 1; }
            if(modal==2){ if(e->key_char=='\n'||e->key_char==27){ modal=0; ui_full_redraw(); } return 1; }
            if(text_typing){
                char tch=e->key_char; unsigned int tkc=e->keycode;
                if(tch=='\n'||tch=='\r'||tkc==0x1C){ text_finish(); ui_full_redraw(); return 1; }
                if(tch==27){ text_typing=0; g_tool.text[0]=0; ui_full_redraw(); return 1; }
                // #542 OS-wide standard editing: caret movement, select-all,
                // copy/cut/paste (system-wide clipboard) and undo/redo, all via
                // the shared text-field widget bound to g_tool.text.
                tf_handle_key(&g_text_tf, e);
                ui_full_redraw();
                return 1;   // swallow all other keys while typing
            }
            if(e->key_char==' '){ space_down=1; return 1; }
            if(!handle_key(e)) return 0;
            return 1;
        case EVENT_MOUSE_DOWN: {
            int mx=e->mouse_x, my=e->mouse_y;
            g_mx=mx; g_my=my;
            if(!(e->mouse_buttons & MOUSE_BUTTON_LEFT)) break;
            g_mdown=1;
            if(cp_modal_open()){ cp_click_modal(mx,my); return 1; }
            if(guard_open){ click_guard(mx,my); if(g_want_quit) return 0; return 1; }
            if(fb_open){ click_filebrowser(mx,my); if(g_want_quit) return 0; return 1; }
            if(pr_open){ click_print_preview(mx,my); return 1; }
            if(pd_open){ int hh=pd_handle_hit(mx,my); if(hh>=0){ pd_hdrag=hh; pd_handle_setxy(hh,mx,my); return 1; } click_param_dialog(mx,my); return 1; }
            if(modal==2){ int bw=520,bh=260,bx=(g_w-bw)/2,by=(g_h-bh)/2; if(IN(mx,my,bx+bw-90,by+bh-34,74,24)){ modal=0; ui_full_redraw(); } break; }
            if(modal==1){ int bw=520,bh=120,bx=(g_w-bw)/2,by=(g_h-bh)/2;
                if(IN(mx,my,bx+bw-88,by+bh-34,76,24)){ modal_confirm(); return 1; }   // OK
                if(IN(mx,my,bx+bw-172,by+bh-34,76,24)){ modal=0; ui_full_redraw(); return 1; } // Cancel
                break; }
            if(blend_pop){
                int x,y,w,h; int rowh=blend_pop_geom(&x,&y,&w,&h);
                if(IN(mx,my,x,y,w,h)){
                    int ry=y+3;
                    for(int g=0; g<BLEND_NGROUPS; g++){
                        ry+=rowh;   // group header row (not selectable)
                        for(int i=0; i<BLEND_GROUPS[g].n; i++){
                            if(IN(mx,my,x,ry,w,rowh)){ layer_set_blend(g_doc.active,BLEND_GROUPS[g].modes[i]); g_doc.comp_dirty=1; blend_pop=0; ui_full_redraw(); return 1; }
                            ry+=rowh;
                        }
                    }
                }
                blend_pop=0; ui_full_redraw(); return 1;
            }
            if(menu_open>=0){
                int a=click_dropdown(mx,my);
                if(a==-2){ return 1; }
                if(a!=A_NONE){ do_action(a); if(g_want_quit) return 0; return 1; }
            }
            if(click_menubar(mx,my)) return 1;
            // ruler drag -> new guide
            if(my>=cv_y() && my<cv_y()+cv_h() && mx>=cv_x()-RULER_Y && mx<cv_x()){ // left ruler
                int cx,cy; screen_to_canvas(cv_x(),my,&cx,&cy); dragging_guide=2; drag_guide_pos=cy; ui_full_redraw(); return 1; }
            if(mx>=cv_x() && mx<cv_x()+cv_w() && my>=cv_y()-RULER && my<cv_y()){ // top ruler
                int cx,cy; screen_to_canvas(mx,cv_y(),&cx,&cy); dragging_guide=1; drag_guide_pos=cx; ui_full_redraw(); return 1; }
            if(space_down && pt_in_canvas(mx,my)){ panning=1; pan_mx=mx; pan_my=my; return 1; }
            if(click_optbar(mx,my)) return 1;
            if(click_strip(mx,my)) return 1;
            // Status bar spans the full width and is drawn ON TOP of the dock's
            // bottom edge, so it must claim its clicks BEFORE click_dock (which
            // otherwise consumes everything in the dock x-column, including the
            // zoom cluster that lives at the bottom-right).
            if(my>=g_h-STATUS_H){ click_status(mx,my); return 1; }
            if(click_dock(mx,my)) return 1;
            if(pt_in_canvas(mx,my)) canvas_down(mx,my);
            break;
        }
        case EVENT_MOUSE_MOVE: {
            int mx=e->mouse_x, my=e->mouse_y;
            g_mx=mx; g_my=my;
            if(cp_modal_open()){ cp_drag_modal(mx,my); return 1; }
            if(cp_drag_dock(mx,my)) return 1;
            if(fb_open){ if(fb_sb_drag) fb_sb_to(my); draw_filebrowser(); win_invalidate(g_win); return 1; }
            if(pr_open){ return 1; }   // static preview; ignore hover churn
            if(guard_open){ draw_guard(); win_invalidate(g_win); return 1; }
            if(pd_open){
                if(pd_loupe_drag){ pd_loupe_pan_to(mx,my); redraw_canvas_only(); }
                else if(pd_hdrag>=0){ pd_handle_setxy(pd_hdrag,mx,my); }
                else if(pd_dialdrag>=0){ pd_dial_set(pd_dialdrag,mx,my); }
                else if(pd_cvdrag>=0){ pd_curve_drag_to(mx,my); }
                else if(pd_kdrag){ kw_cell[kw_sel]=clampi(kw_drag_v0+(kw_drag_y0-my)/2,-999,999); pd_preview(); }
                else if(pd_drag>=0){ const studio_op_t*op=studio_op_get(pd_opi); if(op){ const sparam_t*p=&op->params[pd_drag]; int bx=pd_bx(),bw=360; int tx=bx+bw-96,tw=64; int rng=p->max-p->min; pd_val[pd_drag]=clampi(p->min+(mx-tx)*rng/tw,p->min,p->max); pd_preview(); } }
                else { draw_param_dialog(); win_invalidate(g_win); }
                return 1;
            }
            if(opt_drag>=0 && opt_drag<g_noc){ optctl_t*o=&g_oc[opt_drag];
                if(o->type==OC_SLIDER && o->val){ int v=(mx-o->x)*o->vmax/(o->w>0?o->w:1); *o->val=clampi(v,0,o->vmax); draw_optbar(); win_invalidate(g_win); }
                return 1; }
            if(zoom_slider_drag){
                int sw=zc_slider_w>10?zc_slider_w-8:1;
                int np=ZOOM_MIN+(mx-zc_slider_x)*(ZOOM_MAX-ZOOM_MIN)/sw;
                zoom_to_center(clampi(np,ZOOM_MIN,ZOOM_MAX));
                ui_full_redraw(); return 1;
            }
            if(lop_drag){ int sx=g_w-DOCK_W+62, sw=DOCK_W-100; int v=clampi((mx-sx)*255/(sw>12?sw-12:1),0,255); layer_set_opacity(g_doc.active,v); g_doc.comp_dirty=1; ui_full_redraw(); return 1; }
            if(dragging_guide){ int cx,cy; screen_to_canvas(mx,my,&cx,&cy); drag_guide_pos=(dragging_guide==1)?cx:cy; redraw_canvas_only(); return 1; }
            if(panning){ int z=zoom(); pan_x -= (mx-pan_mx)*100/z; pan_y -= (my-pan_my)*100/z; pan_mx=mx; pan_my=my; redraw_canvas_only(); return 1; }
            if(modal){ draw_modal(); win_invalidate(g_win); return 1; }
            if(menu_open>=0){
                // Hover-switch between top-level menus while one is already open.
                if(my<MENU_H){
                    for(int i=0;i<NMENU;i++) if(IN(mx,my,menu_x(i),0,menu_w(i),MENU_H)){
                        if(menu_open!=i){ menu_open=i; submenu_cat=-1; } break; }
                }
                // Hover-open the Filters category submenu under the pointer.
                if(MENUS[menu_open].kind==MK_FILTERS){
                    int x=menu_x(menu_open), y=MENU_H, w=dropdown_w(menu_open), n=MENUS[menu_open].n;
                    if(IN(mx,my,x,y,w,n*20+6)){
                        int idx=(my-(y+4))/20;
                        if(idx>=0 && idx<n && idx!=submenu_cat) submenu_cat=idx;
                    }
                }
                draw_menubar(); draw_dropdown(); win_invalidate(g_win); return 1;
            }
            if(blend_pop){ draw_blend_pop(); win_invalidate(g_win); return 1; }
            int nh=-1;
            if(mx<STRIP_W && my>=MENU_H+OPT_H){
                int bw=20,bh=20,x0=3,y0=MENU_H+OPT_H+4;
                for(int i=0;i<TL_COUNT;i++){ int col=i&1,row=i>>1; if(IN(mx,my,x0+col*(bw+2),y0+row*(bh+2),bw,bh)){ nh=i; break; } }
            }
            if(nh!=hover_tool){ hover_tool=nh; draw_strip(); win_invalidate(g_win); }
            if(painting||shaping) canvas_move(mx,my);
            // hover repaint of the chrome panel under the pointer (button hover/press)
            if(my<MENU_H){ g_last_over_grid=0; draw_menubar(); win_invalidate(g_win); }
            else if(my<MENU_H+OPT_H){ g_last_over_grid=0; draw_optbar(); win_invalidate(g_win); }
            else if(my>=g_h-STATUS_H){ g_last_over_grid=0; draw_status(); win_invalidate(g_win); }
            else if(mx>=g_w-DOCK_W){
                // The Brushes/Patterns cell grids have no hover feedback, so skip
                // the whole-dock repaint while the pointer merely tracks over them
                // (this, plus the cached cells, kills the Patterns-panel flashing).
                int over_grid = (mx>=g_w-DOCK_W+8) &&
                    ((BRG_y1>BRG_y0 && my>=BRG_y0 && my<BRG_y1) ||
                     (PTG_y1>PTG_y0 && my>=PTG_y0 && my<PTG_y1));
                if(!over_grid || !g_last_over_grid){ draw_dock(); win_invalidate(g_win); }
                g_last_over_grid = over_grid;
            }
            else g_last_over_grid=0;
            if(pt_in_canvas(mx,my)){
                int cx,cy; screen_to_canvas(mx,my,&cx,&cy);
                int inb=(g_doc.comp && cx>=0&&cy>=0&&cx<g_doc.w&&cy<g_doc.h);
                uint32_t under=inb?g_doc.comp[cy*g_doc.w+cx]:0;
                g_cur_valid=1; g_cur_x=cx; g_cur_y=cy;
                g_cur_col=inb?(under|0xFF000000u):C_PANEL2;
                if(!painting && !shaping && tool_is_brush(g_tool.id)) redraw_canvas_only();  // move the ring cursor
                draw_status(); win_invalidate(g_win);          // center zone only; left message untouched
            }else if(g_cur_valid){ g_cur_valid=0; if(tool_is_brush(g_tool.id)) redraw_canvas_only(); draw_status(); win_invalidate(g_win); }
            break;
        }
        case EVENT_MOUSE_UP: {
            int mx=e->mouse_x, my=e->mouse_y;
            g_mx=mx; g_my=my; g_mdown=0;
            if(cp_modal_open()){ cp_modal_release(); return 1; }
            cp_dock_release();
            if(fb_open){ fb_sb_drag=0; return 1; }
            if(guard_open) return 1;
            if(zoom_slider_drag){ zoom_slider_drag=0; return 1; }
            if(opt_drag>=0){ opt_drag=-1; draw_optbar(); win_invalidate(g_win); return 1; }
            if(lop_drag){ lop_drag=0; ui_full_redraw(); return 1; }
            if(pd_open){ int wasl=pd_loupe_drag; pd_drag=-1; pd_dialdrag=-1; pd_cvdrag=-1; pd_hdrag=-1; pd_kdrag=0; pd_loupe_drag=0; if(wasl) redraw_canvas_only(); return 1; }
            if(dragging_guide){ int cx,cy; screen_to_canvas(mx,my,&cx,&cy);
                if(dragging_guide==1 && nguide_v<MAX_GUIDES && cx>=0 && cx<g_doc.w) guide_v[nguide_v++]=cx;
                if(dragging_guide==2 && nguide_h<MAX_GUIDES && cy>=0 && cy<g_doc.h) guide_h[nguide_h++]=cy;
                dragging_guide=0; ui_full_redraw(); return 1; }
            if(panning){ panning=0; return 1; }
            if(painting||shaping){ canvas_up(mx,my); break; }
            // release over chrome: repaint so the pressed look clears
            if(my<MENU_H){ draw_menubar(); win_invalidate(g_win); }
            else if(my<MENU_H+OPT_H){ draw_optbar(); win_invalidate(g_win); }
            else if(mx>=g_w-DOCK_W){ draw_dock(); win_invalidate(g_win); }
            break;
        }
        case EVENT_MOUSE_SCROLL:
            if(e->mouse_x>0||e->mouse_y>0){ g_mx=e->mouse_x; g_my=e->mouse_y; }
            if(fb_open){
                if(e->scroll_delta>0) fb_scroll-=1; else if(e->scroll_delta<0) fb_scroll+=1;
                if(fb_scroll<0) fb_scroll=0;
                int maxs=fb_n-8; if(maxs<0) maxs=0; if(fb_scroll>maxs) fb_scroll=maxs;
                ui_full_redraw(); break;
            }
            if(guard_open) break;
            if(g_mx>=g_w-DOCK_W && g_my>=MENU_H){
                int step=48;
                if(e->scroll_delta>0) dock_scroll-=step; else if(e->scroll_delta<0) dock_scroll+=step;
                if(dock_scroll<0) dock_scroll=0;
                if(dock_scroll>dock_max_scroll) dock_scroll=dock_max_scroll;
                ui_full_redraw();
            } else {
                // Scroll-wheel zoom snaps to the next/prev nice stop. When the
                // pointer is over the canvas, zoom to the cursor (keep the point
                // under it fixed); otherwise zoom about the viewport centre.
                int cur=g_zoom_pct, np=cur;
                if(e->scroll_delta>0){ np=ZOOM_MAX; for(int i=0;i<NZSTOP;i++) if(ZSTOPS[i]>cur){np=ZSTOPS[i];break;} }
                else if(e->scroll_delta<0){ np=ZOOM_MIN; for(int i=NZSTOP-1;i>=0;i--) if(ZSTOPS[i]<cur){np=ZSTOPS[i];break;} }
                if(np!=cur){
                    if(pt_in_canvas(g_mx,g_my)) zoom_to(np,g_mx,g_my);
                    else zoom_to_center(np);
                }
                ui_full_redraw();
            }
            break;
        default: break;
    }
    return 1;
}
