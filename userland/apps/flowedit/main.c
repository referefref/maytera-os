// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// flowedit - Maytera Flow VISUAL EDITOR, Milestone 1 (installs as /APPS/FLOW).
//
// The visual front end for the Maytera Flow workflow system. It reads and
// writes the SAME /CONFIG/WORKFLOWS/<name>.yml files the headless runner
// /APPS/FLOWRUN executes, reusing the runner's exact node+edge model and YAML
// parser (userland/apps/flowrun/flow.c + flow.h, compiled straight into this
// binary). The editor renders that graph as a pannable/zoomable canvas of
// draggable nodes joined by bezier wires between typed ports, a left palette to
// add nodes, and a right inspector to configure the selected node. Save emits
// YAML the runner can execute; Run launches /APPS/FLOWRUN on the saved file.
//
// REUSE, NEVER REINVENT (project hard rule). This app hand-rolls no primitive:
//   - windowing + event loop:  win_create / win_get_event (syscall.h), the same
//                               100ms-timeout blocking loop apps/files uses (no
//                               busy-wait; the #211/#212/#426 freeze class).
//   - widgets + palette:        gui_button / gui_card / gui_fill_rounded /
//                               gui_textfield_tf (gui_style.h), textfield_t +
//                               tf_handle_key (textfield.h).
//   - connector drawing:        gui_line (gui_style.h) composed into beziers.
//   - antialiased text:         win_draw_text_ttf (SYS_WIN_DRAW_TTF=235).
//   - theme colours:            theme_color() (theme.h).
//   - YAML load + node model:   flow_parse_file + flow_graph_t (flow.h).
//   - launching the runner:     sys_spawn_args (syscall.h).
//
// REAL vs PLACEHOLDER (kept honest, per the milestone brief):
//   - REAL: the whole graph model, the YAML load AND save (round-trips through
//     flow.c's parser), node/port typing, add/move/wire/delete, Save, Run.
//   - PLACEHOLDER: the App-node inspector's app+feature catalog is a small
//     representative table (see g_app_catalog[]), NOT a live enumeration of
//     every installed app's contract. Live enumeration would drive each app's
//     ct_contract_t through contract_invoke() / a describe verb (contract.h);
//     that is an M2 concern. The catalog rows are typed with the REAL contract.h
//     risk/access vocabulary so the model is not faked, only the data is seeded.

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"

#include "gui.h"          // brings syscall.h, gui_style.h, textfield.h, keys.h, theme.h
#include "theme.h"
#include "contract.h"     // CT_SAFE/CT_GUARDED, CT_READ/CT_WRITE vocabulary
#include "flow.h"         // the runner's node+edge model (reused, not reinvented)
#include "flowfeed.h"     // #153: flows-feed manifest model (network-independent)
#include "pkgsig.h"       // #153: App Store / updater trust anchor (SAME verify)

// ===========================================================================
// Layout constants (window is fixed for M1; content coords match draw coords).
// ===========================================================================
#define WIN_W       960
#define WIN_H       620
#define TOOLBAR_H   36
#define PALETTE_W   150
#define INSPECTOR_W 250
#define CANVAS_X    PALETTE_W
#define CANVAS_Y    TOOLBAR_H
#define CANVAS_W    (WIN_W - PALETTE_W - INSPECTOR_W)
#define CANVAS_H    (WIN_H - TOOLBAR_H)

#define NODE_W      160
#define NODE_HDR    24
#define PORT_ROW    20
#define NODE_FOOT   22
#define PORT_R      5      // port dot radius, world units at zoom 1
#define PORT_HIT    9      // port hit radius, screen px (zoom-independent)

// ===========================================================================
// Port types and their chip colours (the design's typed-port palette).
// ===========================================================================
enum {
    PT_ACTION = 0, PT_BOOL, PT_INT, PT_FLOAT, PT_ENUM, PT_STRING,
    PT_LIST, PT_OBJECT, PT_IMAGE, PT_BYTES, PT_COLOR, PT_DATETIME, PT_ANY
};
static const char *pt_short[] = {
    "act","bool","int","flt","enum","str","list","obj","img","byt","col","dt","any"
};
static const unsigned pt_col[] = {
    0x94A3B8, 0xF59E0B, 0x60A5FA, 0x38BDF8, 0xA78BFA, 0x34D399,
    0xFBBF24, 0xF472B6, 0xF87171, 0x9CA3AF, 0xE879F9, 0x2DD4BF, 0xCBD5E1
};

// ===========================================================================
// Node categories: the YAML `type` string -> a category (label + header col +
// runner-executability + risk). This is the design's category/colour map.
// ===========================================================================
typedef struct {
    const char *type;     // the YAML `type` value
    const char *label;    // display name in the header
    unsigned    header;   // category colour
    int         runnable; // 1 = /APPS/FLOWRUN executes it in M1, 0 = editor-only
    int         risk;     // CT_SAFE / CT_GUARDED (badge)
} cat_t;

static const cat_t g_cats[] = {
    { "trigger",   "Trigger",        0xF59E0B, 1, CT_SAFE    },
    { "agent",     "Agent Loop",     0xF43F5E, 0, CT_GUARDED },
    { "subloop",   "Async Sub-Loop", 0x14B8A6, 0, CT_GUARDED },
    { "foreach",   "For Each",       0x6366F1, 0, CT_SAFE    },
    // #469 AI-VISION. capture is GUARDED because photographing the screen needs
    // a consented screen.capture grant; input is GUARDED because driving
    // another app's window needs a consented input.inject grant naming it.
    { "capture",   "Capture Screen", 0x0EA5E9, 0, CT_GUARDED },
    { "input",     "Send Keys",      0xEF4444, 0, CT_GUARDED },
    { "branch",    "Branch",         0xD946EF, 0, CT_SAFE    },
    { "transform", "Transform",      0xEAB308, 1, CT_SAFE    },
    { "merge",     "Merge",          0x84CC16, 0, CT_SAFE    },
    { "app",       "App",            0x06B6D4, 0, CT_GUARDED },
    { "device",    "Device",         0x8B5CF6, 0, CT_GUARDED },
    { "service",   "Service",        0x3B82F6, 0, CT_GUARDED },
    { "llm",       "LLM Prompt",     0xEC4899, 1, CT_GUARDED },
    { "file",      "File",           0x22C55E, 1, CT_GUARDED },
    { "variable",  "Variable",       0xFB7185, 0, CT_SAFE    },
};
#define NCATS ((int)(sizeof(g_cats)/sizeof(g_cats[0])))

static const cat_t *cat_of(const char *type) {
    for (int i = 0; i < NCATS; i++)
        if (strcmp(g_cats[i].type, type) == 0) return &g_cats[i];
    return &g_cats[5]; // fall back to Transform styling for an unknown type
}

// ===========================================================================
// Editor state.
// ===========================================================================
static int   g_win = -1;
static flow_graph_t g_g;                         // THE model (reused from flow.h)
static int   g_nx[FLOW_MAX_NODES];               // node world X (parallel to g_g.nodes)
static int   g_ny[FLOW_MAX_NODES];               // node world Y
static int   g_sel = -1;                         // selected node index, or -1
static int   g_idseq = 1;                        // id generator counter
static char  g_name[FLOW_ID_MAX] = "hello";      // current workflow name
static char  g_status[128] = "";                 // toolbar status line
static int   g_dirty = 1;                        // redraw requested

// ---- description (round-tripped; additive optional top-level YAML field) ----
static char  g_desc[FLOW_VAL_MAX] = "";          // workflow description (read + saved)

// ---- Flow Library (browsable catalog of /CONFIG/WORKFLOWS) ------------------
#define LIB_MAX 128
typedef struct {
    char name[FLOW_ID_MAX];   // filename without .yml
    char path[160];           // full path
    char desc[128];           // top-level description: scalar, if any
    int  is_example;          // seeded example (forkable, read-only-ish)
} lib_ent_t;
static lib_ent_t g_lib[LIB_MAX];
static int  g_lib_n = 0;
static int  g_lib_open = 0;
static int  g_lib_sel = 0;
static int  g_lib_scroll = 0;
static int  g_lib_confirm_del = 0;
enum { LIN_NONE = 0, LIN_SAVEAS };
static int  g_lib_input = LIN_NONE;
static char g_lib_input_buf[FLOW_ID_MAX] = "";
static textfield_t g_lib_tf;
typedef struct { int x, y, w, h; } lrect_t;
static lrect_t LB_editor, LB_new, LB_tmpl, LB_saveas, LB_saveok, LB_open, LB_fork, LB_del;
static lrect_t LB_getflows;   // #153: "Get more flows" (opens the feed screen)
static void feed_open(void);  // #153: forward decl (feed block defined below)
static int LB_list_top = 94, LB_row_h = 54, LB_vis = 1;

// view transform
static float g_zoom = 1.0f;
static int   g_pan_x = 0, g_pan_y = 0;

// interaction
enum { IDLE = 0, DRAG_NODE, PAN, WIRE };
static int   g_mode = IDLE;
static int   g_drag_node = -1;
static int   g_grab_ox = 0, g_grab_oy = 0;       // grab offset (world) for DRAG_NODE
static int   g_wire_src = -1, g_wire_port = -1;   // WIRE: source node + output-port index
static int   g_lastmx = 0, g_lastmy = 0;
static int   g_curmx = 0, g_curmy = 0;

// palette
typedef struct { const char *label; const char *type; int is_header; int rx, ry, rw, rh; } pal_row_t;
static pal_row_t g_pal[] = {
    { "TRIGGERS",   0,          1, 0,0,0,0 },
    { "Trigger",    "trigger",  0, 0,0,0,0 },
    { "CONTROL",    0,          1, 0,0,0,0 },
    { "Branch",     "branch",   0, 0,0,0,0 },
    { "For Each",   "foreach",  0, 0,0,0,0 },
    { "Capture",    "capture",  0, 0,0,0,0 },
    { "Send Keys",  "input",    0, 0,0,0,0 },
    { "Merge",      "merge",    0, 0,0,0,0 },
    { "Agent Loop", "agent",    0, 0,0,0,0 },
    { "Sub-Loop",   "subloop",  0, 0,0,0,0 },
    { "INTEGRATE",  0,          1, 0,0,0,0 },
    { "App",        "app",      0, 0,0,0,0 },
    { "Device",     "device",   0, 0,0,0,0 },
    { "Service",    "service",  0, 0,0,0,0 },
    { "LLM Prompt", "llm",      0, 0,0,0,0 },
    { "DATA",       0,          1, 0,0,0,0 },
    { "Transform",  "transform",0, 0,0,0,0 },
    { "File",       "file",     0, 0,0,0,0 },
    { "Variable",   "variable", 0, 0,0,0,0 },
};
#define NPAL ((int)(sizeof(g_pal)/sizeof(g_pal[0])))

// toolbar buttons (rects filled during render)
typedef struct { const char *label; int rx, ry, rw, rh; } tbtn_t;
static tbtn_t g_tb[] = {
    { "Library",0,0,0,0 },
    { "+ Node", 0,0,0,0 },
    { "Delete", 0,0,0,0 },
    { "Save",   0,0,0,0 },
    { "Run",    0,0,0,0 },
};
#define NTB ((int)(sizeof(g_tb)/sizeof(g_tb[0])))

// ---- App-node catalog (PLACEHOLDER data, REAL contract.h typing) ----------
// A representative app+feature table so the App inspector reconfigures ports on
// selection. Live enumeration (contract_invoke over each app's ct_contract_t)
// is deferred to M2; these rows use the genuine CT_* access/risk vocabulary.
typedef struct {
    const char *app;      // dotted app id (contract.h ct_contract_t.app)
    const char *feature;  // dotted feature name (ct_item_t.name style)
    int in_type, out_type;
    int access;           // CT_READ / CT_WRITE
    int risk;             // CT_SAFE / CT_GUARDED
} appcat_t;
static const appcat_t g_app_catalog[] = {
    { "files",    "files.write_text", PT_STRING, PT_INT,    CT_WRITE, CT_GUARDED },
    { "files",    "files.list_dir",   PT_STRING, PT_LIST,   CT_READ,  CT_SAFE    },
    { "editor",   "editor.open",      PT_STRING, PT_ACTION, CT_WRITE, CT_SAFE    },
    { "terminal", "terminal.run",     PT_STRING, PT_STRING, CT_WRITE, CT_GUARDED },
    { "settings", "settings.get",     PT_STRING, PT_STRING, CT_READ,  CT_SAFE    },
    { "notes",    "notes.append",     PT_STRING, PT_ACTION, CT_WRITE, CT_GUARDED },
};
#define NAPPCAT ((int)(sizeof(g_app_catalog)/sizeof(g_app_catalog[0])))

// ===========================================================================
// Small helpers.
// ===========================================================================
static void scpy(char *dst, const char *src, int cap) {
    if (cap <= 0) return;
    int i = 0;
    if (src) for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static int in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// world -> screen and back (single transform used by both render and hit-test)
static int sx_of(int wx) { return CANVAS_X + g_pan_x + (int)(wx * g_zoom); }
static int sy_of(int wy) { return CANVAS_Y + g_pan_y + (int)(wy * g_zoom); }
static int wx_of(int sx) { return (int)((sx - CANVAS_X - g_pan_x) / g_zoom); }
static int wy_of(int sy) { return (int)((sy - CANVAS_Y - g_pan_y) / g_zoom); }
static int scale(int v)  { int r = (int)(v * g_zoom); return r < 1 ? 1 : r; }

// antialiased TTF text, size clamped to the syscall's byte range
static void ttf(int x, int y, const char *s, int size, unsigned col) {
    if (size < 8) size = 8;
    if (size > 60) size = 60;
    win_draw_text_ttf(g_win, x, y, s, size, col);
}

// ===========================================================================
// Theme palette (themed, per the brief; falls back to a dark technical set).
// ===========================================================================
static unsigned C_SURFACE, C_PANEL, C_CANVAS, C_INK, C_INK_DIM, C_ACCENT,
                C_EDGE, C_FIELD, C_FIELD_BD, C_NODE_BG, C_WIRE, C_WIRE_HOT;

static unsigned tc_or(theme_color_id_t id, unsigned fallback) {
    unsigned c = theme_color(id) & 0xFFFFFF;
    return c ? c : fallback;
}
static void load_theme(void) {
    // A dark, dense, technical surface. Pull the live theme where it maps
    // cleanly; keep node/wire/canvas tones fixed so the graph reads the same
    // across themes (the node header colours are the category identity).
    C_SURFACE = tc_or(THEME_COLOR_WINDOW_BG, 0x11151C);
    C_PANEL   = 0x171C25;
    C_CANVAS  = 0x0D1016;
    C_INK     = tc_or(THEME_COLOR_FOREGROUND, 0xE5E9F0);
    C_INK_DIM = 0x8A93A6;
    C_ACCENT  = tc_or(THEME_COLOR_ACCENT, 0x4C8BF5);
    C_EDGE    = 0x2A3140;
    C_FIELD   = 0x0F131A;
    C_FIELD_BD= 0x2E3646;
    C_NODE_BG = 0x1B2230;
    C_WIRE    = 0x5B6678;
    C_WIRE_HOT= C_ACCENT;

    gui_set_style(GUI_STYLE_MODERN);
    gui_palette_t p;
    p.surface        = C_PANEL;
    p.surface_raised = C_NODE_BG;
    p.ink            = C_INK;
    p.ink_dim        = C_INK_DIM;
    p.accent         = C_ACCENT;
    p.accent_hover   = gui_lighten(C_ACCENT, 24);
    p.border         = C_EDGE;
    p.field_bg       = C_FIELD;
    p.field_border   = C_FIELD_BD;
    p.track          = C_EDGE;
    gui_set_palette(&p);
}

// ===========================================================================
// Model helpers (find / add / delete nodes, param access) - all over flow.h.
// ===========================================================================
static int node_index(const char *id) {
    for (int i = 0; i < g_g.nnodes; i++)
        if (strcmp(g_g.nodes[i].id, id) == 0) return i;
    return -1;
}

// Return a stable pointer to node n's param buffer for `key`, creating an empty
// param if it does not exist yet. NULL only if the param table is full.
static char *param_ref(flow_node_t *n, const char *key) {
    for (int i = 0; i < n->nparams; i++)
        if (strcmp(n->params[i].key, key) == 0) return n->params[i].val;
    if (n->nparams >= FLOW_MAX_PARAMS) return 0;
    scpy(n->params[n->nparams].key, key, FLOW_KEY_MAX);
    n->params[n->nparams].val[0] = 0;
    n->nparams++;
    return n->params[n->nparams - 1].val;
}
static const char *param_get(const flow_node_t *n, const char *key) {
    for (int i = 0; i < n->nparams; i++)
        if (strcmp(n->params[i].key, key) == 0) return n->params[i].val;
    return 0;
}
static void param_set(flow_node_t *n, const char *key, const char *val) {
    char *b = param_ref(n, key);
    if (b) scpy(b, val, FLOW_VAL_MAX);
}
static void param_del(flow_node_t *n, const char *key) {
    for (int i = 0; i < n->nparams; i++)
        if (strcmp(n->params[i].key, key) == 0) {
            for (int j = i; j < n->nparams - 1; j++) n->params[j] = n->params[j + 1];
            n->nparams--;
            return;
        }
}

// A node is a loop/agent/subloop CONTAINER (owns bodies via each body's
// params.loop). Mirrors flow.c is_container_type (foreach/loop/agent/subloop).
static int node_is_container(int idx) {
    if (idx < 0 || idx >= g_g.nnodes) return 0;
    const char *t = g_g.nodes[idx].type;
    return strcmp(t, "foreach") == 0 || strcmp(t, "loop") == 0 ||
           strcmp(t, "agent") == 0 || strcmp(t, "subloop") == 0;
}
static int any_container_besides(int self) {
    for (int i = 0; i < g_g.nnodes; i++)
        if (i != self && node_is_container(i)) return 1;
    return 0;
}
// Cycle the selected node's loop membership: none -> c0 -> c1 -> ... -> none.
// Sets/clears params.loop, the exact field h_foreach/h_agent/h_subloop read.
static void loop_membership_next(void) {
    if (g_sel < 0) return;
    flow_node_t *n = &g_g.nodes[g_sel];
    const char *ids[FLOW_MAX_NODES]; int nc = 0;
    for (int i = 0; i < g_g.nnodes; i++)
        if (i != g_sel && node_is_container(i)) ids[nc++] = g_g.nodes[i].id;
    if (nc == 0) { param_del(n, "loop"); return; }
    const char *cur = param_get(n, "loop");
    int curpos = -1;
    if (cur && cur[0]) for (int i = 0; i < nc; i++) if (strcmp(ids[i], cur) == 0) { curpos = i; break; }
    int next = curpos + 1;
    if (next >= nc) { param_del(n, "loop"); scpy(g_status, "Loop: top level (no container)", sizeof(g_status)); return; }
    param_set(n, "loop", ids[next]);
    snprintf(g_status, sizeof(g_status), "Assigned to loop container %s", ids[next]);
}

static void gen_id(char *out, int cap) {
    for (;;) {
        snprintf(out, cap, "n%d", g_idseq++);
        if (node_index(out) < 0) return;
    }
}

// Add a node of `type` at world (wx, wy). Seeds sensible default params so a
// freshly-added node is valid for the runner where the runner supports it.
static int add_node(const char *type, int wx, int wy) {
    if (g_g.nnodes >= FLOW_MAX_NODES) { scpy(g_status, "Node limit reached", sizeof(g_status)); return -1; }
    int idx = g_g.nnodes++;
    flow_node_t *n = &g_g.nodes[idx];
    memset(n, 0, sizeof(*n));
    gen_id(n->id, FLOW_ID_MAX);
    scpy(n->type, type, FLOW_TYPE_MAX);
    g_nx[idx] = wx; g_ny[idx] = wy;

    if (strcmp(type, "trigger") == 0)      param_set(n, "kind", "manual");
    else if (strcmp(type, "transform") == 0){ param_set(n, "op", "constant"); param_set(n, "text", ""); }
    else if (strcmp(type, "file") == 0)    { param_set(n, "op", "write"); param_set(n, "path", "/HOME/OUT.TXT"); }
    else if (strcmp(type, "llm") == 0)     { param_set(n, "system", ""); param_set(n, "prompt", ""); param_set(n, "model", "kimi"); param_set(n, "schema", ""); }
    else if (strcmp(type, "app") == 0)     { param_set(n, "app", g_app_catalog[0].app); param_set(n, "feature", g_app_catalog[0].feature); }
    else if (strcmp(type, "service") == 0) { param_set(n, "service", "http"); param_set(n, "url", ""); }
    else if (strcmp(type, "device") == 0)  { param_set(n, "device", ""); param_set(n, "attr", ""); }
    // #469 AI-VISION. Seeded with the params the runner actually reads, and
    // with a path in the session home rather than /HOME (which is root-owned,
    // so a uid-1000 run could capture and silently never write the file).
    else if (strcmp(type, "capture") == 0) {
        param_set(n, "target", "screen");
        param_set(n, "rect", "");
        param_set(n, "max_width", "512");
        param_set(n, "max_height", "512");
        param_set(n, "quality", "70");
        param_set(n, "path", "FLOWCAP.JPG");
    }
    else if (strcmp(type, "input") == 0) {
        param_set(n, "target", "");
        param_set(n, "keys", "");
        param_set(n, "after", "keys:");
        param_set(n, "hold_ms", "60");
        param_set(n, "gap_ms", "120");
        param_set(n, "max_keys", "4");
    }
    else if (strcmp(type, "branch") == 0)  param_set(n, "cond", "");
    else if (strcmp(type, "variable") == 0){ param_set(n, "name", "var"); param_set(n, "value", ""); }
    else if (strcmp(type, "agent") == 0 || strcmp(type, "subloop") == 0) {
        // objective/success are zeroed by the memset above. Only subloop reads
        // `mode` (await | fire-and-forget); the runner ignores it for agent, so
        // seed a real subloop mode and leave agent's empty (was a bogus "react",
        // which is not a mode the runner recognizes).
        n->has_loop = 1;
        if (strcmp(type, "subloop") == 0) { scpy(n->mode, "await", FLOW_TYPE_MAX); n->every_ms = 1000; }
    }
    return idx;
}

// Remove edges that touch node id `id`.
static void drop_edges_of(const char *id) {
    int w = 0;
    for (int i = 0; i < g_g.nedges; i++) {
        flow_edge_t *e = &g_g.edges[i];
        if (strcmp(e->from_node, id) == 0 || strcmp(e->to_node, id) == 0) continue;
        if (w != i) g_g.edges[w] = *e;
        w++;
    }
    g_g.nedges = w;
}
static void delete_node(int idx) {
    if (idx < 0 || idx >= g_g.nnodes) return;
    drop_edges_of(g_g.nodes[idx].id);
    for (int i = idx; i < g_g.nnodes - 1; i++) {
        g_g.nodes[i] = g_g.nodes[i + 1];
        g_nx[i] = g_nx[i + 1];
        g_ny[i] = g_ny[i + 1];
    }
    g_g.nnodes--;
    g_sel = -1;
}

static int edge_exists(const char *fn, const char *fp, const char *tn, const char *tp) {
    for (int i = 0; i < g_g.nedges; i++) {
        flow_edge_t *e = &g_g.edges[i];
        if (strcmp(e->from_node, fn) == 0 && strcmp(e->from_port, fp) == 0 &&
            strcmp(e->to_node, tn) == 0 && strcmp(e->to_port, tp) == 0) return 1;
    }
    return 0;
}
static void add_edge(const char *fn, const char *fp, const char *tn, const char *tp) {
    if (g_g.nedges >= FLOW_MAX_EDGES) { scpy(g_status, "Edge limit reached", sizeof(g_status)); return; }
    if (edge_exists(fn, fp, tn, tp)) return;
    flow_edge_t *e = &g_g.edges[g_g.nedges++];
    scpy(e->from_node, fn, FLOW_ID_MAX); scpy(e->from_port, fp, FLOW_PORT_MAX);
    scpy(e->to_node,   tn, FLOW_ID_MAX); scpy(e->to_port,   tp, FLOW_PORT_MAX);
}

// ===========================================================================
// Port model: per node type, the input ports (left) and output ports (right),
// each a (name, type). Some are dynamic (LLM prompt placeholders + schema, App
// catalog selection).
// ===========================================================================
typedef struct { char name[FLOW_PORT_MAX]; int type; } port_t;
#define MAXP 8

static const appcat_t *app_lookup(const flow_node_t *n) {
    const char *app = param_get(n, "app");
    const char *feat = param_get(n, "feature");
    for (int i = 0; i < NAPPCAT; i++)
        if (app && feat && strcmp(g_app_catalog[i].app, app) == 0 &&
            strcmp(g_app_catalog[i].feature, feat) == 0) return &g_app_catalog[i];
    return &g_app_catalog[0];
}

// Parse "{name}" placeholders from a prompt template into input ports.
static void llm_inputs(const flow_node_t *n, port_t *in, int *nin) {
    const char *t = param_get(n, "prompt");
    int cnt = 0;
    if (t) {
        for (int i = 0; t[i] && cnt < MAXP; i++) {
            if (t[i] == '{') {
                int j = i + 1, p = 0; char nm[FLOW_PORT_MAX];
                while (t[j] && t[j] != '}' && p < FLOW_PORT_MAX - 1) nm[p++] = t[j++];
                nm[p] = 0;
                if (t[j] == '}' && p > 0) {
                    int dup = 0;
                    for (int k = 0; k < cnt; k++) if (strcmp(in[k].name, nm) == 0) dup = 1;
                    if (!dup) { scpy(in[cnt].name, nm, FLOW_PORT_MAX); in[cnt].type = PT_STRING; cnt++; }
                    i = j;
                }
            }
        }
    }
    if (cnt == 0) { scpy(in[0].name, "in", FLOW_PORT_MAX); in[0].type = PT_STRING; cnt = 1; }
    *nin = cnt;
}

// Split "a:int,b:string" schema into typed output ports (after "out").
static int type_from_word(const char *w) {
    for (int i = 0; i < (int)(sizeof(pt_short)/sizeof(pt_short[0])); i++)
        if (strcmp(pt_short[i], w) == 0) return i;
    if (strcmp(w, "string") == 0) return PT_STRING;
    if (strcmp(w, "integer") == 0) return PT_INT;
    if (strcmp(w, "boolean") == 0) return PT_BOOL;
    return PT_ANY;
}
static void llm_schema_outputs(const flow_node_t *n, port_t *out, int *nout) {
    const char *s = param_get(n, "schema");
    int cnt = *nout;
    if (!s) return;
    char field[FLOW_PORT_MAX]; char tword[16];
    int fp = 0, tp = 0, in_type = 0;
    for (int i = 0; ; i++) {
        char c = s[i];
        if (c == ':') { field[fp] = 0; in_type = 1; tp = 0; continue; }
        if (c == ',' || c == 0) {
            if (in_type) tword[tp] = 0; else field[fp] = 0;
            if (fp > 0 && cnt < MAXP) {
                scpy(out[cnt].name, field, FLOW_PORT_MAX);
                out[cnt].type = in_type ? type_from_word(tword) : PT_STRING;
                cnt++;
            }
            fp = 0; tp = 0; in_type = 0;
            if (c == 0) break;
            continue;
        }
        if (c == ' ') continue;
        if (in_type) { if (tp < 15) tword[tp++] = c; }
        else         { if (fp < FLOW_PORT_MAX - 1) field[fp++] = c; }
    }
    *nout = cnt;
}

static void get_ports(const flow_node_t *n, port_t *in, int *nin, port_t *out, int *nout) {
    *nin = 0; *nout = 0;
    const char *t = n->type;
    #define AI(nm,ty) do { if (*nin < MAXP)  { scpy(in[*nin].name,nm,FLOW_PORT_MAX);  in[*nin].type=ty;  (*nin)++; } } while(0)
    #define AO(nm,ty) do { if (*nout < MAXP) { scpy(out[*nout].name,nm,FLOW_PORT_MAX);out[*nout].type=ty;(*nout)++; } } while(0)

    if (strcmp(t, "trigger") == 0)        { AO("out", PT_BOOL); }
    else if (strcmp(t, "transform") == 0) {
        AI("in", PT_STRING);
        if (param_get(n, "op") && strcmp(param_get(n, "op"), "concat") == 0) AI("in2", PT_STRING);
        AO("out", PT_STRING);
    }
    else if (strcmp(t, "file") == 0)      { AI("text", PT_STRING); AO("out", PT_INT); }
    else if (strcmp(t, "llm") == 0)       {
        llm_inputs(n, in, nin);
        // #469 AI-VISION: the optional image input. h_llm reads the "image"
        // port and, when an edge supplies it, makes a MULTIMODAL call instead
        // of a text one. Listed unconditionally so a capture -> llm wire draws
        // whether or not the prompt happens to mention {image}.
        AI("image", PT_IMAGE);
        AO("out", PT_STRING);
        // h_llm also publishes goal_met when the reply declares one; without
        // this port the agent-loop wire that makes a vision agent terminate
        // would silently not draw.
        AO("goal_met", PT_BOOL);
        llm_schema_outputs(n, out, nout);
    }
    // #469 AI-VISION. These mirror h_capture / h_input in the runner EXACTLY.
    else if (strcmp(t, "capture") == 0)   {
        AI("in", PT_ACTION); AI("target", PT_STRING);
        AO("image", PT_IMAGE); AO("out", PT_IMAGE);
        AO("w", PT_INT); AO("h", PT_INT); AO("bytes", PT_INT);
    }
    else if (strcmp(t, "input") == 0)     {
        AI("keys", PT_STRING); AI("target", PT_STRING);
        AO("out", PT_INT); AO("sent", PT_INT); AO("skipped", PT_INT);
    }
    else if (strcmp(t, "app") == 0)       { const appcat_t *a = app_lookup(n); AI("in", a->in_type); AO("out", a->out_type); }
    else if (strcmp(t, "service") == 0)   { AI("in", PT_STRING); AO("out", PT_OBJECT); }
    else if (strcmp(t, "device") == 0)    { AI("set", PT_ANY); AO("value", PT_ANY); }
    else if (strcmp(t, "branch") == 0)    { AI("in", PT_ANY); AO("true", PT_ACTION); AO("false", PT_ACTION); }
    else if (strcmp(t, "merge") == 0)     { AI("a", PT_ANY); AI("b", PT_ANY); AO("out", PT_ANY); }
    // foreach/agent/subloop ports MUST match the runner (flow.c) exactly, or a
    // saved workflow's loop wires do not draw (draw_canvas drops any edge whose
    // port name get_ports does not list). These mirror h_foreach/h_agent/
    // h_subloop; the leading "in" action input carries the trigger edge that
    // schedules the container.
    else if (strcmp(t, "foreach") == 0)   {
        AI("in", PT_ACTION); AI("items", PT_LIST); AI("collect", PT_ANY);
        AO("item", PT_ANY); AO("index", PT_INT); AO("results", PT_LIST); AO("count", PT_INT);
    }
    else if (strcmp(t, "agent") == 0)     {
        AI("in", PT_ACTION); AI("goal_met", PT_BOOL); AI("metric", PT_INT);
        AO("index", PT_INT); AO("done", PT_ENUM); AO("iterations", PT_INT); AO("tokens", PT_INT);
    }
    else if (strcmp(t, "subloop") == 0)   {
        AI("in", PT_ACTION); AI("spawn", PT_BOOL); AI("collect", PT_ANY); AI("metric", PT_INT);
        AO("result", PT_ANY); AO("metric", PT_INT); AO("joined", PT_BOOL); AO("index", PT_INT);
    }
    else if (strcmp(t, "variable") == 0)  { AI("set", PT_ANY); AO("get", PT_ANY); }
    else                                  { AI("in", PT_ANY); AO("out", PT_ANY); }
    #undef AI
    #undef AO
}

static int node_height(const flow_node_t *n) {
    port_t in[MAXP], out[MAXP]; int ni, no;
    get_ports(n, in, &ni, out, &no);
    int rows = ni > no ? ni : no;
    if (rows < 1) rows = 1;
    return NODE_HDR + rows * PORT_ROW + NODE_FOOT;
}

// Screen position of an input/output port (its dot centre).
static void input_port_screen(int idx, int pi, int *x, int *y) {
    *x = sx_of(g_nx[idx]);
    *y = sy_of(g_ny[idx] + NODE_HDR + pi * PORT_ROW + PORT_ROW / 2);
}
static void output_port_screen(int idx, int pi, int *x, int *y) {
    *x = sx_of(g_nx[idx] + NODE_W);
    *y = sy_of(g_ny[idx] + NODE_HDR + pi * PORT_ROW + PORT_ROW / 2);
}

// ===========================================================================
// Wire drawing: a cubic bezier approximated by short gui_line segments (the
// prototype's connector look; gui_line is the shared primitive - no line
// syscall exists and none is needed).
// ===========================================================================
static void draw_wire(int x0, int y0, int x1, int y1, unsigned col) {
    int dx = x1 - x0; if (dx < 0) dx = -dx;
    int h = dx / 2; if (h < scale(40)) h = scale(40);
    int cx0 = x0 + h, cy0 = y0;
    int cx1 = x1 - h, cy1 = y1;
    const int SEG = 18;
    int px = x0, py = y0;
    for (int s = 1; s <= SEG; s++) {
        float u = (float)s / SEG, v = 1.0f - u;
        float b0 = v*v*v, b1 = 3*v*v*u, b2 = 3*v*u*u, b3 = u*u*u;
        int qx = (int)(b0*x0 + b1*cx0 + b2*cx1 + b3*x1);
        int qy = (int)(b0*y0 + b1*cy0 + b2*cy1 + b3*y1);
        gui_line(g_win, px, py, qx, qy, col);
        px = qx; py = qy;
    }
}

// ===========================================================================
// Inspector: a small field list built for the selected node. Each field is
// text (bound to a param or node buffer) or an enum (a param cycled by click).
// Field rects are stored during render and reused for hit-testing.
// ===========================================================================
enum { F_LABEL = 0, F_TEXT, F_ENUM, F_LOOP };
typedef struct {
    char        label[24];
    int         kind;
    char       *buf;       // backing buffer (param or node field)
    int         cap;
    const char *options;   // "a|b|c" for F_ENUM
    textfield_t tf;         // used when kind == F_TEXT
    int         rx, ry, rw, rh;
} field_t;
#define MAXFIELDS 12
static field_t g_fields[MAXFIELDS];
static int     g_nfields = 0;
static int     g_focus = -1;   // index into g_fields of the focused text field

static void add_text_field(const char *label, char *buf, int cap) {
    if (g_nfields >= MAXFIELDS || !buf) return;
    field_t *f = &g_fields[g_nfields++];
    scpy(f->label, label, sizeof(f->label));
    f->kind = F_TEXT; f->buf = buf; f->cap = cap; f->options = 0;
    tf_init(&f->tf, buf, cap);
}
static void add_enum_field(const char *label, char *buf, int cap, const char *options) {
    if (g_nfields >= MAXFIELDS || !buf) return;
    field_t *f = &g_fields[g_nfields++];
    scpy(f->label, label, sizeof(f->label));
    f->kind = F_ENUM; f->buf = buf; f->cap = cap; f->options = options;
}
static void add_label_field(const char *label) {
    if (g_nfields >= MAXFIELDS) return;
    field_t *f = &g_fields[g_nfields++];
    scpy(f->label, label, sizeof(f->label));
    f->kind = F_LABEL; f->buf = 0; f->cap = 0; f->options = 0;
}
static void add_loop_field(const char *label) {
    if (g_nfields >= MAXFIELDS) return;
    field_t *f = &g_fields[g_nfields++];
    scpy(f->label, label, sizeof(f->label));
    f->kind = F_LOOP; f->buf = 0; f->cap = 0; f->options = 0;
}

// Rebuild the inspector field set for the current selection.
static void build_inspector(void) {
    g_nfields = 0; g_focus = -1;
    if (g_sel < 0) return;
    flow_node_t *n = &g_g.nodes[g_sel];
    const char *t = n->type;

    if (strcmp(t, "trigger") == 0) {
        add_enum_field("kind", param_ref(n, "kind"), FLOW_VAL_MAX, "manual|cron|file_watch|interval");
        add_text_field("spec", param_ref(n, "spec"), FLOW_VAL_MAX);
    } else if (strcmp(t, "transform") == 0) {
        add_enum_field("op", param_ref(n, "op"), FLOW_VAL_MAX, "constant|upper|lower|concat");
        add_text_field("text", param_ref(n, "text"), FLOW_VAL_MAX);
        add_text_field("sep",  param_ref(n, "sep"),  FLOW_VAL_MAX);
    } else if (strcmp(t, "file") == 0) {
        add_enum_field("op", param_ref(n, "op"), FLOW_VAL_MAX, "write|append");
        add_text_field("path", param_ref(n, "path"), FLOW_VAL_MAX);
        add_text_field("text", param_ref(n, "text"), FLOW_VAL_MAX);
    } else if (strcmp(t, "llm") == 0) {
        add_text_field("system", param_ref(n, "system"), FLOW_VAL_MAX);
        add_text_field("prompt", param_ref(n, "prompt"), FLOW_VAL_MAX);
        add_enum_field("model", param_ref(n, "model"), FLOW_VAL_MAX, "kimi|gpt|claude|local");
        add_text_field("schema", param_ref(n, "schema"), FLOW_VAL_MAX);
    } else if (strcmp(t, "app") == 0) {
        add_enum_field("app", param_ref(n, "app"), FLOW_VAL_MAX,
                       "files|editor|terminal|settings|notes");
        add_text_field("feature", param_ref(n, "feature"), FLOW_VAL_MAX);
    } else if (strcmp(t, "service") == 0) {
        add_enum_field("service", param_ref(n, "service"), FLOW_VAL_MAX, "http|smb|nfs|notify");
        add_text_field("url", param_ref(n, "url"), FLOW_VAL_MAX);
        add_text_field("body", param_ref(n, "body"), FLOW_VAL_MAX);
    } else if (strcmp(t, "device") == 0) {
        add_text_field("device", param_ref(n, "device"), FLOW_VAL_MAX);
        add_text_field("attr",   param_ref(n, "attr"),   FLOW_VAL_MAX);
    } else if (strcmp(t, "branch") == 0) {
        add_text_field("cond", param_ref(n, "cond"), FLOW_VAL_MAX);
    } else if (strcmp(t, "variable") == 0) {
        add_text_field("name",  param_ref(n, "name"),  FLOW_VAL_MAX);
        add_text_field("value", param_ref(n, "value"), FLOW_VAL_MAX);
    } else if (strcmp(t, "agent") == 0 || strcmp(t, "subloop") == 0) {
        // M1: render + basic inspector only (execution is the runner's later
        // milestone). These bind to the real loop/budget fields of flow_node_t.
        add_text_field("objective", n->objective, FLOW_VAL_MAX);
        add_text_field("success",   n->success,   FLOW_VAL_MAX);
        add_text_field("mode",      n->mode,       FLOW_TYPE_MAX);
        add_label_field("budgets: edit in YAML (M1)");
    } else {
        add_label_field("no parameters (M1)");
    }

    // Assign-to-loop: membership control, shown whenever a container node
    // (foreach/agent/subloop) exists to join. Sets params.loop = <containerId>,
    // the exact field the runner's h_foreach/h_agent/h_subloop read.
    if (any_container_besides(g_sel)) add_loop_field("body of loop");
}

// Cycle an enum param buffer to its next option.
static void enum_next(field_t *f) {
    if (!f->buf || !f->options) return;
    // find current option, advance to next
    const char *o = f->options;
    char cur[FLOW_VAL_MAX]; scpy(cur, f->buf, sizeof(cur));
    // collect options
    const char *first = o; int first_len = 0;
    const char *matchnext = 0;
    const char *p = o; const char *seg = o; int seglen = 0;
    int matched = 0;
    for (;; p++) {
        if (*p == '|' || *p == 0) {
            if (seg == first) first_len = seglen;
            if (matched) { matchnext = seg; break; }
            if ((int)strlen(cur) == seglen && strncmp(cur, seg, seglen) == 0) matched = 1;
            if (*p == 0) break;
            seg = p + 1; seglen = 0;
            continue;
        }
        seglen++;
    }
    char nv[FLOW_VAL_MAX];
    if (matched && matchnext) {
        int l = 0; while (matchnext[l] && matchnext[l] != '|') l++;
        if (l > (int)sizeof(nv) - 1) l = sizeof(nv) - 1;
        memcpy(nv, matchnext, l); nv[l] = 0;
    } else {
        int l = first_len; if (l > (int)sizeof(nv) - 1) l = sizeof(nv) - 1;
        memcpy(nv, first, l); nv[l] = 0;
    }
    scpy(f->buf, nv, f->cap);
    // App node: reset feature to the first catalog row for the newly-picked app
    if (g_sel >= 0 && strcmp(g_g.nodes[g_sel].type, "app") == 0 && strcmp(f->label, "app") == 0) {
        for (int i = 0; i < NAPPCAT; i++)
            if (strcmp(g_app_catalog[i].app, nv) == 0) {
                param_set(&g_g.nodes[g_sel], "feature", g_app_catalog[i].feature);
                break;
            }
    }
}

// ===========================================================================
// YAML emit (round-trips through flow.c's parser). Double-quoted, escaped
// scalar values; bare ids/ports/keys.
// ===========================================================================
static char g_out[65536];
static int  g_outlen;
static void oclear(void) { g_outlen = 0; g_out[0] = 0; }
static void oapp(const char *s) {
    int n = (int)strlen(s);
    if (g_outlen + n >= (int)sizeof(g_out) - 1) n = (int)sizeof(g_out) - 1 - g_outlen;
    if (n <= 0) return;
    memcpy(g_out + g_outlen, s, n);
    g_outlen += n; g_out[g_outlen] = 0;
}
static void oqstr(const char *s) {
    oapp("\"");
    char tmp[FLOW_VAL_MAX * 2]; int o = 0;
    for (int i = 0; s && s[i] && o < (int)sizeof(tmp) - 2; i++) {
        char c = s[i];
        if (c == '\\' || c == '"') { tmp[o++] = '\\'; tmp[o++] = c; }
        else if (c == '\n' || c == '\r' || c == '\t') { tmp[o++] = ' '; }
        else tmp[o++] = c;
    }
    tmp[o] = 0;
    oapp(tmp);
    oapp("\"");
}

static void build_yaml(void) {
    oclear();
    oapp("name: "); oqstr(g_name[0] ? g_name : "workflow"); oapp("\n");
    if (g_desc[0]) { oapp("description: "); oqstr(g_desc); oapp("\n"); }
    oapp("nodes:\n");
    for (int i = 0; i < g_g.nnodes; i++) {
        flow_node_t *n = &g_g.nodes[i];
        oapp("  - id: "); oapp(n->id); oapp("\n");
        oapp("    type: "); oapp(n->type[0] ? n->type : "transform"); oapp("\n");
        if (n->nparams > 0) {
            oapp("    params:\n");
            for (int p = 0; p < n->nparams; p++) {
                oapp("      "); oapp(n->params[p].key); oapp(": ");
                oqstr(n->params[p].val); oapp("\n");
            }
        }
        if (n->has_loop) {
            if (n->objective[0]) { oapp("    objective: "); oqstr(n->objective); oapp("\n"); }
            if (n->success[0])   { oapp("    success: ");   oqstr(n->success);   oapp("\n"); }
            if (n->mode[0])      { oapp("    mode: ");       oqstr(n->mode);      oapp("\n"); }
            char num[32];
            if (n->max_iters)   { snprintf(num, sizeof(num), "    max_iters: %d\n",   n->max_iters);   oapp(num); }
            if (n->max_minutes) { snprintf(num, sizeof(num), "    max_minutes: %d\n", n->max_minutes); oapp(num); }
            if (n->max_tokens)  { snprintf(num, sizeof(num), "    max_tokens: %ld\n", n->max_tokens);  oapp(num); }
            if (n->every_ms)    { snprintf(num, sizeof(num), "    every_ms: %d\n",    n->every_ms);    oapp(num); }
        }
    }
    if (g_g.nedges > 0) {
        oapp("edges:\n");
        for (int i = 0; i < g_g.nedges; i++) {
            flow_edge_t *e = &g_g.edges[i];
            oapp("  - from: ["); oapp(e->from_node); oapp(", "); oapp(e->from_port); oapp("]\n");
            oapp("    to: [");   oapp(e->to_node);   oapp(", "); oapp(e->to_port);   oapp("]\n");
        }
    }
}

static int path_exists(const char *p) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

// Scan a workflow file for a top-level (column-0) `description:` scalar. The
// additive optional schema READ path: flow.c already skips unknown top-level
// keys, so this never breaks the runner parse; the editor reads it here for the
// Library catalog and round-trips it on save. Absent -> out[0] = 0.
static void read_description(const char *path, char *out, int cap) {
    if (cap > 0) out[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return;
    static char buf[4096];
    int total = 0, k;
    while (total < (int)sizeof(buf) - 1 &&
           (k = (int)read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
        total += k;
    close(fd);
    buf[total] = 0;
    int i = 0;
    while (i < total) {
        if (strncmp(buf + i, "description:", 12) == 0) {
            const char *v = buf + i + 12;
            while (*v == ' ' || *v == '\t') v++;
            char q = 0;
            if (*v == '"' || *v == '\'') { q = *v; v++; }
            int o = 0;
            while (*v && *v != '\n' && *v != '\r' && o < cap - 1) {
                if (q && *v == q) break;
                if (!q && *v == '#') break;
                out[o++] = *v++;
            }
            while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\t')) o--;
            out[o] = 0;
            return;
        }
        while (i < total && buf[i] != '\n') i++;
        if (i < total) i++;
    }
}

// Byte-for-byte copy (Library Fork), preserving every field the parser does not
// model (comments, layout, description) so a fork is a faithful copy.
static int copy_file_raw(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    char b[1024]; int k, rc = 0;
    while ((k = (int)read(in, b, sizeof(b))) > 0) {
        int w = 0;
        while (w < k) { int t = (int)write(out, b + w, (unsigned)(k - w)); if (t <= 0) { rc = -1; break; } w += t; }
        if (rc < 0) break;
    }
    close(in); close(out);
    return rc;
}

static void wf_path(const char *name, char *out, int cap) {
    if (name && strchr(name, '/')) snprintf(out, cap, "%s", name);
    else snprintf(out, cap, "/CONFIG/WORKFLOWS/%s.yml", name ? name : "workflow");
}

static int save_workflow(void) {
    sys_mkdir("/CONFIG", 0755);
    sys_mkdir("/CONFIG/WORKFLOWS", 0755);
    build_yaml();
    char path[256]; wf_path(g_name, path, sizeof(path));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { snprintf(g_status, sizeof(g_status), "Save FAILED: %s", path); return -1; }
    int w = 0;
    while (w < g_outlen) {
        int k = (int)write(fd, g_out + w, (unsigned)(g_outlen - w));
        if (k <= 0) break;
        w += k;
    }
    close(fd);
    if (w != g_outlen) { snprintf(g_status, sizeof(g_status), "Save short write: %s", path); return -1; }
    snprintf(g_status, sizeof(g_status), "Saved %s (%d bytes)", path, g_outlen);
    return 0;
}

// Auto-layout: order nodes by topological "column" (BFS depth), lay out in a
// grid so a loaded workflow renders sensibly. Positions are session layout;
// persistence of manual arrangement is a documented M1 limitation.
static void auto_layout(void) {
    int col[FLOW_MAX_NODES], indeg[FLOW_MAX_NODES];
    for (int i = 0; i < g_g.nnodes; i++) { col[i] = 0; indeg[i] = 0; }
    for (int e = 0; e < g_g.nedges; e++) {
        int ti = node_index(g_g.edges[e].to_node);
        if (ti >= 0) indeg[ti]++;
    }
    // relax: node column = 1 + max(source columns), a few passes (DAG-ish)
    for (int pass = 0; pass < g_g.nnodes; pass++) {
        int changed = 0;
        for (int e = 0; e < g_g.nedges; e++) {
            int fi = node_index(g_g.edges[e].from_node);
            int ti = node_index(g_g.edges[e].to_node);
            if (fi >= 0 && ti >= 0 && col[ti] < col[fi] + 1) { col[ti] = col[fi] + 1; changed = 1; }
        }
        if (!changed) break;
    }
    int rowcount[FLOW_MAX_NODES]; for (int i = 0; i < FLOW_MAX_NODES; i++) rowcount[i] = 0;
    for (int i = 0; i < g_g.nnodes; i++) {
        int c = col[i]; if (c < 0) c = 0; if (c >= FLOW_MAX_NODES) c = FLOW_MAX_NODES - 1;
        g_nx[i] = 40 + c * (NODE_W + 70);
        g_ny[i] = 40 + rowcount[c] * (node_height(&g_g.nodes[i]) + 30);
        rowcount[c]++;
    }
}

// ---- example workflow seeds (bundled, forkable copies of the runner's
// examples/*.yml). Embedded so the OS binary is self-contained, exactly like the
// original hello seed. Each carries an additive top-level `description:` so the
// Library shows one and the read path is exercised end to end. ----
static const char *SEED_HELLO =
    "name: hello\n"
    "description: \"Manual trigger, constant, file write (the M1 hello example)\"\n"
    "nodes:\n"
    "  - id: t1\n"
    "    type: trigger\n"
    "    params: { kind: manual }\n"
    "  - id: x1\n"
    "    type: transform\n"
    "    params: { op: constant, text: \"Maytera Flow M1 OK\" }\n"
    "  - id: f1\n"
    "    type: file\n"
    "    params: { op: write, path: /HOME/FLOWOUT.TXT }\n"
    "edges:\n"
    "  - from: [t1, out]\n"
    "    to:   [x1, in]\n"
    "  - from: [x1, out]\n"
    "    to:   [f1, text]\n";
static const char *SEED_LOOPDEMO =
    "name: loopdemo\n"
    "description: \"For-each loop that upper-cases a constant list into a file\"\n"
    "nodes:\n"
    "  - id: t1\n"
    "    type: trigger\n"
    "    params: { kind: manual }\n"
    "  - id: lp\n"
    "    type: foreach\n"
    "    params: { items: \"alpha,beta,gamma\", max_iters: 100 }\n"
    "  - id: up\n"
    "    type: transform\n"
    "    params: { op: upper, loop: lp }\n"
    "  - id: sink\n"
    "    type: file\n"
    "    params: { op: write, path: /HOME/LOOPOUT.TXT }\n"
    "edges:\n"
    "  - from: [t1, out]\n"
    "    to:   [lp, in]\n"
    "  - from: [lp, item]\n"
    "    to:   [up, in]\n"
    "  - from: [up, out]\n"
    "    to:   [lp, collect]\n"
    "  - from: [lp, results]\n"
    "    to:   [sink, text]\n";
static const char *SEED_AGENTDEMO =
    "name: agentdemo\n"
    "description: \"Bounded agent-promise loop terminating on objective\"\n"
    "nodes:\n"
    "  - id: t1\n"
    "    type: trigger\n"
    "    params: { kind: manual }\n"
    "  - id: ag\n"
    "    type: agent\n"
    "    objective: reach the goal within budget\n"
    "    success: \"metric >= 5\"\n"
    "    max_iters: 12\n"
    "    max_minutes: 5\n"
    "    max_tokens: 100\n"
    "  - id: perceive\n"
    "    type: transform\n"
    "    params: { op: concat, loop: ag }\n"
    "  - id: decide\n"
    "    type: transform\n"
    "    params: { op: gte, value: 2, loop: ag }\n"
    "  - id: act\n"
    "    type: transform\n"
    "    params: { op: constant, text: \"act: step taken\", loop: ag }\n"
    "  - id: report\n"
    "    type: file\n"
    "    params: { op: write, path: /HOME/AGENTOUT.TXT }\n"
    "edges:\n"
    "  - from: [t1, out]\n"
    "    to:   [ag, in]\n"
    "  - from: [ag, index]\n"
    "    to:   [perceive, in]\n"
    "  - from: [perceive, out]\n"
    "    to:   [ag, metric]\n"
    "  - from: [ag, index]\n"
    "    to:   [decide, in]\n"
    "  - from: [decide, out]\n"
    "    to:   [ag, goal_met]\n"
    "  - from: [ag, done]\n"
    "    to:   [report, text]\n";
static const char *SEED_SUBLOOPDEMO =
    "name: subloopdemo\n"
    "description: \"Async sub-loop in await mode writing its body result\"\n"
    "nodes:\n"
    "  - id: t1\n"
    "    type: trigger\n"
    "    params: { kind: manual }\n"
    "  - id: sl\n"
    "    type: subloop\n"
    "    mode: await\n"
    "    every_ms: 500\n"
    "    params: { max_conc: 2 }\n"
    "  - id: work\n"
    "    type: transform\n"
    "    params: { op: constant, text: \"sub-body result\", loop: sl }\n"
    "  - id: sink\n"
    "    type: file\n"
    "    params: { op: write, path: /HOME/SUBLOOPOUT.TXT }\n"
    "edges:\n"
    "  - from: [t1, out]\n"
    "    to:   [sl, in]\n"
    "  - from: [work, out]\n"
    "    to:   [sl, collect]\n"
    "  - from: [sl, result]\n"
    "    to:   [sink, text]\n";
static const char *SEED_APPDEMO =
    "name: appdemo\n"
    "description: \"App node driving the Convert app tool contract\"\n"
    "nodes:\n"
    "  - id: t1\n"
    "    type: trigger\n"
    "    params: { kind: manual }\n"
    "  - id: amount\n"
    "    type: transform\n"
    "    params: { op: constant, text: \"100\" }\n"
    "  - id: conv\n"
    "    type: app\n"
    "    params: { app: convert, feature: value, verb: call }\n"
    "  - id: sink\n"
    "    type: file\n"
    "    params: { op: write, path: /HOME/APPOUT.TXT }\n"
    "edges:\n"
    "  - from: [t1, out]\n"
    "    to:   [conv, in]\n"
    "  - from: [amount, out]\n"
    "    to:   [conv, arg]\n"
    "  - from: [conv, out]\n"
    "    to:   [sink, text]\n";

// Write `content` to `path` only if it does not already exist. The single
// generalized seeder used for hello AND every bundled example (no duplicated
// per-file seeding logic).
static void seed_if_missing(const char *path, const char *content) {
    if (path_exists(path)) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    int len = (int)strlen(content), w = 0;
    while (w < len) { int k = (int)write(fd, content + w, (unsigned)(len - w)); if (k <= 0) break; w += k; }
    close(fd);
}

// Seed the top-level default (hello) and the bundled, forkable examples set into
// /CONFIG/WORKFLOWS/examples/ on first run. Generalizes the old
// seed_hello_if_missing to the whole example catalog.
static void seed_workflows(void) {
    sys_mkdir("/CONFIG", 0755);
    sys_mkdir("/CONFIG/WORKFLOWS", 0755);
    sys_mkdir("/CONFIG/WORKFLOWS/examples", 0755);
    seed_if_missing("/CONFIG/WORKFLOWS/hello.yml", SEED_HELLO);
    seed_if_missing("/CONFIG/WORKFLOWS/examples/hello.yml", SEED_HELLO);
    seed_if_missing("/CONFIG/WORKFLOWS/examples/loopdemo.yml", SEED_LOOPDEMO);
    seed_if_missing("/CONFIG/WORKFLOWS/examples/agentdemo.yml", SEED_AGENTDEMO);
    seed_if_missing("/CONFIG/WORKFLOWS/examples/subloopdemo.yml", SEED_SUBLOOPDEMO);
    seed_if_missing("/CONFIG/WORKFLOWS/examples/appdemo.yml", SEED_APPDEMO);
}

static void load_workflow(const char *name) {
    char path[256]; wf_path(name, path, sizeof(path));
    char err[FLOW_ERR_MAX] = {0};
    memset(&g_g, 0, sizeof(g_g));
    if (flow_parse_file(path, &g_g, err, sizeof(err)) != 0) {
        // start empty on a missing/broken file (still a usable canvas)
        memset(&g_g, 0, sizeof(g_g));
        snprintf(g_status, sizeof(g_status), "New workflow (%s)", err[0] ? err : "no file");
    } else {
        snprintf(g_status, sizeof(g_status), "Loaded %s (%d nodes)", path, g_g.nnodes);
    }
    if (g_g.name[0]) scpy(g_name, g_g.name, sizeof(g_name));
    // keep g_idseq clear of any numeric ids already in the file
    for (int i = 0; i < g_g.nnodes; i++) {
        const char *id = g_g.nodes[i].id;
        if (id[0] == 'n') { int v = atoi(id + 1); if (v >= g_idseq) g_idseq = v + 1; }
    }
    read_description(path, g_desc, sizeof(g_desc));
    auto_layout();
    g_sel = -1; build_inspector();
}

static void run_workflow(void) {
    if (save_workflow() != 0) return;
    char *av[3];
    av[0] = (char *)"/APPS/FLOWRUN";
    av[1] = g_name;
    av[2] = 0;
    int r = sys_spawn_args("/APPS/FLOWRUN", av, 2);
    if (r < 0) snprintf(g_status, sizeof(g_status), "Run: spawn failed (%d)", r);
    else       snprintf(g_status, sizeof(g_status), "Launched /APPS/FLOWRUN %s", g_name);
}

// ===========================================================================
// Rendering.
// ===========================================================================
static void draw_port(int x, int y, int ptype, int is_out) {
    int r = scale(PORT_R); if (r < 3) r = 3;
    gui_fill_rounded(g_win, x - r, y - r, r * 2, r * 2, r, pt_col[ptype]);
    // small type chip label next to the dot
    int size = clampi(scale(11), 8, 16);
    if (is_out) {
        int w = (int)strlen(pt_short[ptype]) * (size / 2 + 1);
        ttf(x - r - 6 - w, y - size / 2, pt_short[ptype], size, C_INK_DIM);
    } else {
        ttf(x + r + 6, y - size / 2, pt_short[ptype], size, C_INK_DIM);
    }
}

static void draw_node(int idx, int selected) {
    flow_node_t *n = &g_g.nodes[idx];
    const cat_t *c = cat_of(n->type);
    int x = sx_of(g_nx[idx]);
    int y = sy_of(g_ny[idx]);
    int w = scale(NODE_W);
    int h = scale(node_height(n));
    // clip roughly to canvas (skip fully-offscreen nodes)
    if (x > CANVAS_X + CANVAS_W || x + w < CANVAS_X || y > CANVAS_Y + CANVAS_H || y + h < CANVAS_Y)
        return;

    int hdr = scale(NODE_HDR);
    gui_fill_rounded(g_win, x, y, w, h, scale(8), C_NODE_BG);
    gui_fill_rounded(g_win, x, y, w, hdr, scale(8), c->header);
    win_draw_rect(g_win, x, y + hdr - scale(4), w, scale(4), c->header); // square off header bottom

    int ts = clampi(scale(13), 9, 20);
    char title[40];
    scpy(title, n->id, sizeof(title));
    ttf(x + 8, y + (hdr - ts) / 2, c->label, ts, 0x101010);
    ttf(x + 8, y + hdr + 2, title, clampi(scale(11), 8, 16), C_INK_DIM);

    port_t in[MAXP], out[MAXP]; int ni, no;
    get_ports(n, in, &ni, out, &no);
    for (int i = 0; i < ni; i++) {
        int px, py; input_port_screen(idx, i, &px, &py);
        draw_port(px, py, in[i].type, 0);
        int size = clampi(scale(11), 8, 15);
        ttf(px + scale(PORT_R) + 6 + 22, py - size / 2, in[i].name, size, C_INK);
    }
    for (int i = 0; i < no; i++) {
        int px, py; output_port_screen(idx, i, &px, &py);
        draw_port(px, py, out[i].type, 1);
        int size = clampi(scale(11), 8, 15);
        int tw = (int)strlen(out[i].name) * (size / 2 + 1);
        ttf(px - scale(PORT_R) - 6 - 22 - tw, py - size / 2, out[i].name, size, C_INK);
    }

    // footer: risk badge + runner status
    int fy = y + h - scale(NODE_FOOT) + 2;
    const char *risk = (c->risk == CT_GUARDED) ? "GUARDED" : "SAFE";
    unsigned rc = (c->risk == CT_GUARDED) ? 0xF59E0B : 0x22C55E;
    int fs = clampi(scale(10), 8, 14);
    ttf(x + 8, fy, risk, fs, rc);
    const char *st = c->runnable ? "runnable" : "editor-only";
    unsigned stc = c->runnable ? C_INK_DIM : 0xF472B6;
    int stw = (int)strlen(st) * (fs / 2 + 1);
    ttf(x + w - 8 - stw, fy, st, fs, stc);

    // Body-membership: a chip when this node belongs to a loop container, and a
    // teal scope outline when its container is the currently-selected node.
    const char *lp = param_get(n, "loop");
    if (lp && lp[0] && node_is_container(node_index(lp))) {
        int cs = clampi(scale(10), 8, 13);
        char chip[40]; snprintf(chip, sizeof(chip), "in %s", lp);
        int cw2 = (int)strlen(chip) * (cs / 2 + 1) + 12;
        int chx = x + w - cw2 - 4, chy = y + 3;
        gui_fill_rounded(g_win, chx, chy, cw2, cs + 5, 3, 0x0B1220);
        ttf(chx + 6, chy + 2, chip, cs, 0xFDE68A);
        if (g_sel >= 0 && strcmp(lp, g_g.nodes[g_sel].id) == 0)
            gui_rounded_border(g_win, x - 3, y - 3, w + 6, h + 6, scale(10), 0x14B8A6);
    }

    if (selected) gui_rounded_border(g_win, x - 2, y - 2, w + 4, h + 4, scale(9), C_ACCENT);
}

static void draw_toolbar(void) {
    win_draw_rect(g_win, 0, 0, WIN_W, TOOLBAR_H, C_PANEL);
    win_draw_rect(g_win, 0, TOOLBAR_H - 1, WIN_W, 1, C_EDGE);
    // title + workflow name
    ttf(10, 8, "Maytera Flow", 16, C_INK);
    char nm[80]; snprintf(nm, sizeof(nm), "-  %s.yml", g_name);
    ttf(150, 9, nm, 13, C_INK_DIM);
    // buttons (right-aligned)
    int bw = 74, bh = 24, gap = 8, x = WIN_W - (bw + gap) * NTB;
    for (int i = 0; i < NTB; i++) {
        g_tb[i].rx = x; g_tb[i].ry = 6; g_tb[i].rw = bw; g_tb[i].rh = bh;
        gui_btn_variant_t v = GUI_BTN_SECONDARY;
        if (strcmp(g_tb[i].label, "Run") == 0)    v = GUI_BTN_PRIMARY;
        if (strcmp(g_tb[i].label, "Delete") == 0) v = GUI_BTN_DANGER;
        gui_button(g_win, x, 6, bw, bh, g_tb[i].label, v, GUI_ST_NORMAL);
        x += bw + gap;
    }
}

static void draw_palette(void) {
    win_draw_rect(g_win, 0, CANVAS_Y, PALETTE_W, CANVAS_H, C_PANEL);
    win_draw_rect(g_win, PALETTE_W - 1, CANVAS_Y, 1, CANVAS_H, C_EDGE);
    int y = CANVAS_Y + 8;
    for (int i = 0; i < NPAL; i++) {
        g_pal[i].rx = 6; g_pal[i].ry = y; g_pal[i].rw = PALETTE_W - 12;
        if (g_pal[i].is_header) {
            g_pal[i].rh = 18;
            ttf(10, y + 3, g_pal[i].label, 11, C_INK_DIM);
            y += 20;
        } else {
            g_pal[i].rh = 22;
            const cat_t *c = cat_of(g_pal[i].type);
            gui_fill_rounded(g_win, 8, y, 10, 10, 2, c->header);
            ttf(24, y - 1, g_pal[i].label, 13, C_INK);
            y += 24;
        }
    }
}

static void draw_inspector(void) {
    int ix = WIN_W - INSPECTOR_W;
    win_draw_rect(g_win, ix, CANVAS_Y, INSPECTOR_W, CANVAS_H, C_PANEL);
    win_draw_rect(g_win, ix, CANVAS_Y, 1, CANVAS_H, C_EDGE);
    int x = ix + 12, y = CANVAS_Y + 10, w = INSPECTOR_W - 24;

    if (g_sel < 0) {
        ttf(x, y, "Inspector", 15, C_INK);
        ttf(x, y + 28, "Select a node to", 12, C_INK_DIM);
        ttf(x, y + 44, "configure it.", 12, C_INK_DIM);
        return;
    }
    flow_node_t *n = &g_g.nodes[g_sel];
    const cat_t *c = cat_of(n->type);
    gui_fill_rounded(g_win, x, y, w, 26, 6, c->header);
    ttf(x + 8, y + 5, c->label, 14, 0x101010);
    y += 32;
    char idl[64]; snprintf(idl, sizeof(idl), "id: %s   type: %s", n->id, n->type);
    ttf(x, y, idl, 11, C_INK_DIM);
    y += 22;

    for (int i = 0; i < g_nfields; i++) {
        field_t *f = &g_fields[i];
        ttf(x, y, f->label, 12, C_INK_DIM);
        y += 16;
        f->rx = x; f->ry = y; f->rw = w; f->rh = 24;
        if (f->kind == F_TEXT) {
            gui_textfield_tf(g_win, x, y, w, 24, f->buf, f->tf.len, f->tf.cursor,
                             f->tf.sel_anchor, (g_focus == i), "");
            y += 30;
        } else if (f->kind == F_ENUM) {
            gui_button(g_win, x, y, w, 24, f->buf[0] ? f->buf : "(set)",
                       GUI_BTN_SECONDARY, GUI_ST_NORMAL);
            y += 30;
        } else if (f->kind == F_LOOP) {
            const char *cur = param_get(n, "loop");
            char lbl[64];
            if (cur && cur[0] && node_is_container(node_index(cur)))
                snprintf(lbl, sizeof(lbl), "in loop: %s", cur);
            else
                scpy(lbl, "(none - top level)", sizeof(lbl));
            gui_button(g_win, x, y, w, 24, lbl, GUI_BTN_SECONDARY, GUI_ST_NORMAL);
            y += 30;
        } else { // F_LABEL
            f->rh = 16;
            y += 4;
        }
        if (y > CANVAS_Y + CANVAS_H - 40) break;
    }
}

static void draw_canvas(void) {
    win_draw_rect(g_win, CANVAS_X, CANVAS_Y, CANVAS_W, CANVAS_H, C_CANVAS);
    // dot grid
    int step = scale(40); if (step < 12) step = 12;
    int ox = (g_pan_x % step + step) % step;
    int oy = (g_pan_y % step + step) % step;
    for (int gx = CANVAS_X + ox; gx < CANVAS_X + CANVAS_W; gx += step)
        for (int gy = CANVAS_Y + oy; gy < CANVAS_Y + CANVAS_H; gy += step)
            win_draw_pixel(g_win, gx, gy, C_EDGE);

    // wires under nodes
    for (int e = 0; e < g_g.nedges; e++) {
        flow_edge_t *ed = &g_g.edges[e];
        int fi = node_index(ed->from_node), ti = node_index(ed->to_node);
        if (fi < 0 || ti < 0) continue;
        port_t in[MAXP], out[MAXP]; int ni, no;
        get_ports(&g_g.nodes[fi], in, &ni, out, &no);
        int op = -1; for (int i = 0; i < no; i++) if (strcmp(out[i].name, ed->from_port) == 0) op = i;
        get_ports(&g_g.nodes[ti], in, &ni, out, &no);
        int ip = -1; for (int i = 0; i < ni; i++) if (strcmp(in[i].name, ed->to_port) == 0) ip = i;
        if (op < 0 || ip < 0) continue;
        int x0, y0, x1, y1;
        output_port_screen(fi, op, &x0, &y0);
        input_port_screen(ti, ip, &x1, &y1);
        draw_wire(x0, y0, x1, y1, C_WIRE);
    }

    // temporary wire while dragging a connection
    if (g_mode == WIRE && g_wire_src >= 0) {
        int x0, y0;
        output_port_screen(g_wire_src, g_wire_port, &x0, &y0);
        draw_wire(x0, y0, g_curmx, g_curmy, C_WIRE_HOT);
    }

    for (int i = 0; i < g_g.nnodes; i++) draw_node(i, i == g_sel);
}

// ===========================================================================
// Flow Library: a browsable catalog of /CONFIG/WORKFLOWS/*.yml (+ bundled
// examples/). Lists name + optional description, with OPEN / FORK / DELETE /
// NEW / NEW-TEMPLATE / SAVE-AS. Reuses the fs directory listing (sys_readdir)
// and unlink (sys_unlink) the Files app uses, flow.c load+save, and the shared
// gui widgets + TTF. Reachable from the toolbar "Library" button.
// ===========================================================================
static void lib_add_dir(const char *dir, int is_example) {
    dirent_t e; int idx = 0;
    while (g_lib_n < LIB_MAX) {
        if (sys_readdir(dir, idx, &e) != 0) break;
        idx++;
        if (e.type == 1) continue;                 // skip subdirectories
        int L = (int)strlen(e.name);
        if (L < 5 || strcmp(e.name + L - 4, ".yml") != 0) continue;
        lib_ent_t *le = &g_lib[g_lib_n++];
        int nl = L - 4; if (nl > FLOW_ID_MAX - 1) nl = FLOW_ID_MAX - 1;
        memcpy(le->name, e.name, nl); le->name[nl] = 0;
        snprintf(le->path, sizeof(le->path), "%s/%s", dir, e.name);
        le->is_example = is_example;
        read_description(le->path, le->desc, sizeof(le->desc));
    }
}
static void lib_refresh(void) {
    g_lib_n = 0;
    lib_add_dir("/CONFIG/WORKFLOWS", 0);
    lib_add_dir("/CONFIG/WORKFLOWS/examples", 1);
    if (g_lib_sel >= g_lib_n) g_lib_sel = g_lib_n - 1;
    if (g_lib_sel < 0) g_lib_sel = 0;
}
static void unique_wf_name(const char *base, char *out, int cap) {
    char cand[FLOW_ID_MAX], path[176];
    snprintf(cand, sizeof(cand), "%s-copy", base);
    snprintf(path, sizeof(path), "/CONFIG/WORKFLOWS/%s.yml", cand);
    int n = 2;
    while (path_exists(path) && n < 1000) {
        snprintf(cand, sizeof(cand), "%s-copy%d", base, n++);
        snprintf(path, sizeof(path), "/CONFIG/WORKFLOWS/%s.yml", cand);
    }
    scpy(out, cand, cap);
}
// Simple word-wrapped TTF text; returns the y after the last line.
static int draw_wrapped(int x, int y, int w, const char *str, int size, unsigned col) {
    int cw = size / 2 + 1, maxc = w / cw; if (maxc < 8) maxc = 8;
    char line[256]; int li = 0, i = 0;
    while (str && str[i]) {
        int ws = i; while (str[i] && str[i] != ' ' && str[i] != '\n') i++;
        int wl = i - ws;
        if (li > 0 && li + 1 + wl > maxc) { line[li] = 0; ttf(x, y, line, size, col); y += size + 4; li = 0; }
        if (li > 0 && li < (int)sizeof(line) - 1) line[li++] = ' ';
        for (int k = 0; k < wl && li < (int)sizeof(line) - 1; k++) line[li++] = str[ws + k];
        if (str[i] == '\n') { line[li] = 0; ttf(x, y, line, size, col); y += size + 4; li = 0; i++; }
        else if (str[i] == ' ') i++;
    }
    if (li > 0) { line[li] = 0; ttf(x, y, line, size, col); y += size + 4; }
    return y;
}
static void library_open_workflow(int i) {
    if (i < 0 || i >= g_lib_n) return;
    lib_ent_t *e = &g_lib[i];
    load_workflow(e->path);                 // wf_path passes a '/'-bearing path through
    scpy(g_name, e->name, sizeof(g_name));  // saves go to /CONFIG/WORKFLOWS/<name>.yml
    snprintf(g_status, sizeof(g_status), "Opened %s", e->name);
    g_lib_open = 0; g_lib_confirm_del = 0;
}
static void library_fork(int i) {
    if (i < 0 || i >= g_lib_n) return;
    lib_ent_t *e = &g_lib[i];
    char nn[FLOW_ID_MAX]; unique_wf_name(e->name, nn, sizeof(nn));
    char dst[176]; snprintf(dst, sizeof(dst), "/CONFIG/WORKFLOWS/%s.yml", nn);
    sys_mkdir("/CONFIG", 0755); sys_mkdir("/CONFIG/WORKFLOWS", 0755);
    if (copy_file_raw(e->path, dst) != 0) { snprintf(g_status, sizeof(g_status), "Fork FAILED: %s", nn); return; }
    load_workflow(dst);
    scpy(g_name, nn, sizeof(g_name)); scpy(g_g.name, nn, sizeof(g_g.name));
    snprintf(g_status, sizeof(g_status), "Forked to %s", nn);
    g_lib_open = 0; g_lib_confirm_del = 0;
}
static void library_delete(int i) {
    if (i < 0 || i >= g_lib_n) return;
    lib_ent_t *e = &g_lib[i];
    if (!g_lib_confirm_del) { g_lib_confirm_del = 1; snprintf(g_status, sizeof(g_status), "Delete %s? click Confirm", e->name); return; }
    if (sys_unlink(e->path) != 0) { snprintf(g_status, sizeof(g_status), "Delete FAILED: %s", e->name); g_lib_confirm_del = 0; return; }
    snprintf(g_status, sizeof(g_status), "Deleted %s%s", e->name, e->is_example ? " (re-seeds on next launch)" : "");
    g_lib_confirm_del = 0;
    lib_refresh();
}
static void library_new_blank(void) {
    memset(&g_g, 0, sizeof(g_g));
    g_desc[0] = 0; scpy(g_name, "untitled", sizeof(g_name)); scpy(g_g.name, "untitled", sizeof(g_g.name));
    g_idseq = 1; g_sel = -1; g_zoom = 1.0f; g_pan_x = 0; g_pan_y = 0;
    build_inspector();
    scpy(g_status, "New blank workflow", sizeof(g_status));
    g_lib_open = 0; g_lib_confirm_del = 0;
}
static void library_new_template(void) {
    memset(&g_g, 0, sizeof(g_g));
    g_desc[0] = 0; scpy(g_name, "untitled", sizeof(g_name)); scpy(g_g.name, "untitled", sizeof(g_g.name));
    g_idseq = 1; g_zoom = 1.0f; g_pan_x = 0; g_pan_y = 0;
    int t = add_node("trigger", 0, 0);
    int x = add_node("transform", 0, 0);
    int f = add_node("file", 0, 0);
    if (t >= 0 && x >= 0 && f >= 0) {
        param_set(&g_g.nodes[x], "op", "constant");
        param_set(&g_g.nodes[x], "text", "hello from Maytera Flow");
        add_edge(g_g.nodes[t].id, "out", g_g.nodes[x].id, "in");
        add_edge(g_g.nodes[x].id, "out", g_g.nodes[f].id, "text");
    }
    auto_layout(); g_sel = -1; build_inspector();
    scpy(g_status, "New workflow from template", sizeof(g_status));
    g_lib_open = 0; g_lib_confirm_del = 0;
}
static void library_do_saveas(void) {
    char nm[FLOW_ID_MAX]; scpy(nm, g_lib_input_buf[0] ? g_lib_input_buf : "untitled", sizeof(nm));
    int L = (int)strlen(nm);
    if (L > 4 && strcmp(nm + L - 4, ".yml") == 0) nm[L - 4] = 0;
    scpy(g_name, nm, sizeof(g_name)); scpy(g_g.name, nm, sizeof(g_g.name));
    save_workflow();
    g_lib_input = LIN_NONE;
    lib_refresh();
    for (int i = 0; i < g_lib_n; i++) if (!g_lib[i].is_example && strcmp(g_lib[i].name, nm) == 0) { g_lib_sel = i; break; }
}
static void draw_library(void) {
    win_draw_rect(g_win, 0, 0, WIN_W, WIN_H, C_SURFACE);
    win_draw_rect(g_win, 0, 0, WIN_W, 46, C_PANEL);
    win_draw_rect(g_win, 0, 46, WIN_W, 1, C_EDGE);
    ttf(16, 12, "Flow Library", 18, C_INK);
    char sub[80]; snprintf(sub, sizeof(sub), "%d workflow%s in /CONFIG/WORKFLOWS", g_lib_n, g_lib_n == 1 ? "" : "s");
    ttf(150, 17, sub, 12, C_INK_DIM);
    LB_editor.x = WIN_W - 96; LB_editor.y = 10; LB_editor.w = 84; LB_editor.h = 26;
    gui_button(g_win, LB_editor.x, LB_editor.y, LB_editor.w, LB_editor.h, "Editor", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    LB_getflows.w = 128; LB_getflows.h = 26; LB_getflows.x = LB_editor.x - LB_getflows.w - 8; LB_getflows.y = 10;
    gui_button(g_win, LB_getflows.x, LB_getflows.y, LB_getflows.w, LB_getflows.h, "Get more flows", GUI_BTN_PRIMARY, GUI_ST_NORMAL);

    int ay = 56, ah = 26, ax = 16;
    LB_new.w = LB_tmpl.w = LB_saveas.w = LB_saveok.w = 0;
    if (g_lib_input == LIN_SAVEAS) {
        ttf(ax, ay + 6, "Save current as:", 12, C_INK_DIM);
        int fx = ax + 120, fw = 280;
        gui_textfield_tf(g_win, fx, ay, fw, ah, g_lib_input_buf, g_lib_tf.len, g_lib_tf.cursor, g_lib_tf.sel_anchor, 1, "name");
        LB_saveok.x = fx + fw + 8; LB_saveok.y = ay; LB_saveok.w = 64; LB_saveok.h = ah;
        gui_button(g_win, LB_saveok.x, LB_saveok.y, LB_saveok.w, LB_saveok.h, "Save", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    } else {
        LB_new.x = ax; LB_new.y = ay; LB_new.w = 90; LB_new.h = ah;
        gui_button(g_win, LB_new.x, LB_new.y, LB_new.w, LB_new.h, "New Blank", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
        LB_tmpl.x = ax + 98; LB_tmpl.y = ay; LB_tmpl.w = 110; LB_tmpl.h = ah;
        gui_button(g_win, LB_tmpl.x, LB_tmpl.y, LB_tmpl.w, LB_tmpl.h, "New Template", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
        LB_saveas.x = ax + 216; LB_saveas.y = ay; LB_saveas.w = 140; LB_saveas.h = ah;
        gui_button(g_win, LB_saveas.x, LB_saveas.y, LB_saveas.w, LB_saveas.h, "Save Current As", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    }

    int lx = 16, lw = 560;
    LB_list_top = 94; LB_row_h = 54;
    int list_h = WIN_H - LB_list_top - 16;
    LB_vis = list_h / LB_row_h; if (LB_vis < 1) LB_vis = 1;
    win_draw_rect(g_win, lx, LB_list_top, lw, list_h, C_CANVAS);
    if (g_lib_scroll > g_lib_n - LB_vis) g_lib_scroll = g_lib_n - LB_vis;
    if (g_lib_scroll < 0) g_lib_scroll = 0;
    for (int r = 0; r < LB_vis; r++) {
        int i = g_lib_scroll + r; if (i >= g_lib_n) break;
        int ry = LB_list_top + r * LB_row_h;
        lib_ent_t *e = &g_lib[i];
        gui_fill_rounded(g_win, lx + 4, ry + 4, lw - 8, LB_row_h - 8, 6, (i == g_lib_sel) ? C_NODE_BG : C_PANEL);
        if (i == g_lib_sel) gui_rounded_border(g_win, lx + 4, ry + 4, lw - 8, LB_row_h - 8, 6, C_ACCENT);
        ttf(lx + 14, ry + 9, e->name, 15, C_INK);
        if (e->is_example) {
            gui_fill_rounded(g_win, lx + lw - 76, ry + 10, 62, 16, 3, 0x14B8A6);
            ttf(lx + lw - 70, ry + 11, "EXAMPLE", 10, 0x061410);
        }
        ttf(lx + 14, ry + 30, e->desc[0] ? e->desc : "(no description)", 11, C_INK_DIM);
    }

    int dx = 592, dw = WIN_W - dx - 16, dy = 94, dh = WIN_H - dy - 16;
    win_draw_rect(g_win, dx, dy, dw, dh, C_PANEL);
    win_draw_rect(g_win, dx, dy, 1, dh, C_EDGE);
    if (g_lib_n > 0 && g_lib_sel >= 0 && g_lib_sel < g_lib_n) {
        lib_ent_t *e = &g_lib[g_lib_sel];
        int px = dx + 14, py = dy + 12;
        ttf(px, py, e->name, 16, C_INK); py += 26;
        ttf(px, py, e->is_example ? "Bundled example (forkable)" : "User workflow", 11, C_INK_DIM); py += 20;
        py = draw_wrapped(px, py, dw - 28, e->desc[0] ? e->desc : "(no description)", 12, C_INK_DIM);
        py += 6;
        ttf(px, py, e->path, 10, C_INK_DIM); py += 24;
        LB_open.x = px; LB_open.y = py; LB_open.w = dw - 28; LB_open.h = 28;
        gui_button(g_win, LB_open.x, LB_open.y, LB_open.w, LB_open.h, "Open in Editor", GUI_BTN_PRIMARY, GUI_ST_NORMAL); py += 34;
        LB_fork.x = px; LB_fork.y = py; LB_fork.w = (dw - 28 - 8) / 2; LB_fork.h = 28;
        gui_button(g_win, LB_fork.x, LB_fork.y, LB_fork.w, LB_fork.h, "Fork / Copy", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
        LB_del.x = px + LB_fork.w + 8; LB_del.y = py; LB_del.w = dw - 28 - LB_fork.w - 8; LB_del.h = 28;
        gui_button(g_win, LB_del.x, LB_del.y, LB_del.w, LB_del.h, g_lib_confirm_del ? "Confirm?" : "Delete", GUI_BTN_DANGER, GUI_ST_NORMAL); py += 34;
        if (g_lib_confirm_del) ttf(px, py, "Click Confirm again to delete", 10, 0xF59E0B);
    } else {
        LB_open.w = LB_fork.w = LB_del.w = 0;
        ttf(dx + 14, dy + 14, "No workflows.", 12, C_INK_DIM);
        ttf(dx + 14, dy + 32, "Use New or New Template.", 12, C_INK_DIM);
    }
    if (g_status[0]) ttf(16, WIN_H - 14, g_status, 11, C_INK_DIM);
    win_invalidate(g_win);
}
static void on_library_click(int mx, int my) {
    if (g_lib_input == LIN_SAVEAS) {
        if (LB_saveok.w && in_rect(mx, my, LB_saveok.x, LB_saveok.y, LB_saveok.w, LB_saveok.h)) library_do_saveas();
        return;
    }
    if (LB_getflows.w && in_rect(mx, my, LB_getflows.x, LB_getflows.y, LB_getflows.w, LB_getflows.h)) { feed_open(); return; }
    if (in_rect(mx, my, LB_editor.x, LB_editor.y, LB_editor.w, LB_editor.h)) { g_lib_open = 0; g_lib_confirm_del = 0; return; }
    if (LB_new.w && in_rect(mx, my, LB_new.x, LB_new.y, LB_new.w, LB_new.h)) { library_new_blank(); return; }
    if (LB_tmpl.w && in_rect(mx, my, LB_tmpl.x, LB_tmpl.y, LB_tmpl.w, LB_tmpl.h)) { library_new_template(); return; }
    if (LB_saveas.w && in_rect(mx, my, LB_saveas.x, LB_saveas.y, LB_saveas.w, LB_saveas.h)) {
        g_lib_input = LIN_SAVEAS; scpy(g_lib_input_buf, g_name, sizeof(g_lib_input_buf));
        tf_init(&g_lib_tf, g_lib_input_buf, sizeof(g_lib_input_buf)); return;
    }
    if (mx >= 16 && mx < 16 + 560 && my >= LB_list_top && my < LB_list_top + LB_vis * LB_row_h) {
        int i = g_lib_scroll + (my - LB_list_top) / LB_row_h;
        if (i >= 0 && i < g_lib_n) { g_lib_sel = i; g_lib_confirm_del = 0; }
        return;
    }
    if (g_lib_n > 0) {
        if (LB_open.w && in_rect(mx, my, LB_open.x, LB_open.y, LB_open.w, LB_open.h)) { library_open_workflow(g_lib_sel); return; }
        if (LB_fork.w && in_rect(mx, my, LB_fork.x, LB_fork.y, LB_fork.w, LB_fork.h)) { library_fork(g_lib_sel); return; }
        if (LB_del.w && in_rect(mx, my, LB_del.x, LB_del.y, LB_del.w, LB_del.h)) { library_delete(g_lib_sel); return; }
    }
}
static void on_library_key(gui_event_t *ev) {
    if (g_lib_input == LIN_SAVEAS) {
        if (ev->keycode == GUI_KEY_ENTER) { library_do_saveas(); return; }
        if (ev->keycode == GUI_KEY_ESC) { g_lib_input = LIN_NONE; return; }
        tf_handle_key(&g_lib_tf, ev); return;
    }
    if (ev->keycode == GUI_KEY_ESC) { g_lib_open = 0; g_lib_confirm_del = 0; return; }
    if (ev->keycode == GUI_KEY_ENTER) { library_open_workflow(g_lib_sel); return; }
    if (ev->keycode == GUI_KEY_UP)   { if (g_lib_sel > 0) g_lib_sel--; g_lib_confirm_del = 0; if (g_lib_sel < g_lib_scroll) g_lib_scroll = g_lib_sel; return; }
    if (ev->keycode == GUI_KEY_DOWN) { if (g_lib_sel < g_lib_n - 1) g_lib_sel++; g_lib_confirm_del = 0; if (g_lib_sel >= g_lib_scroll + LB_vis) g_lib_scroll = g_lib_sel - LB_vis + 1; return; }
}

// ===========================================================================
// Flow feed: "Download example flows from the update server" (#153).
//
// The CLIENT side of update-server flow distribution. It REUSES the App Store /
// updater trust model WITHOUT weakening it:
//   * the flows manifest is authenticated with pkgsig_verify_manifest() - the
//     SAME detached-signature check the App Store uses, verified in the kernel
//     (SYS_OTA_VERIFY_SIG) against the public key baked into the kernel image.
//   * each flow YAML is checked against the manifest's per-entry sha256 with
//     pkgsig_verify_package() BEFORE a byte is written.
//   * the YAML is parse-validated with the runner's own flow_parse_bytes()
//     before install, so a flow that would not run is never saved.
//   * install writes to a UNIQUE /CONFIG/WORKFLOWS/<name>.yml, never
//     overwriting an existing user flow.
// Fetch is async (http_fetch_start/poll/read) and pumps the window event loop,
// so the UI stays live: the #420/#549 "never block the UI thread" rule.
// ===========================================================================
#define FEED_MANIFEST_MAX (64 * 1024)
#define FEED_YAML_MAX     (128 * 1024)

static int  g_feed_open = 0;
static int  g_feed_abort = 0;
static flowfeed_entry_t g_feed[FLOWFEED_MAX];
static int  g_feed_n = 0;
static int  g_feed_sel = 0;
static int  g_feed_scroll = 0;
// per-entry outcome: 0 none, 1 installed, 2 installed under a renamed file, 3 error
static unsigned char g_feed_stat[FLOWFEED_MAX];
static char g_feed_manifest[FEED_MANIFEST_MAX];
static uint8_t g_feed_sig[1024];
static char g_feed_yaml[FEED_YAML_MAX];
static lrect_t FD_back, FD_get, FD_all;
static int FD_list_top = 94, FD_row_h = 50, FD_vis = 1;

// Repo base, mirroring the App Store client (userland/apps/appstore/main.c):
// default is the signed updates host; an optional /APPS/STORE.SRC first line
// (http:// or https://) redirects to a test repo. The signature over the
// manifest is what gates trust, so a redirected source adds no trust surface.
#define FEED_REPO_DEFAULT "https://updates.maytera.net"
static char g_feed_repo[160] = FEED_REPO_DEFAULT;

static void feed_repo_load(void) {
    int fd = open("/APPS/STORE.SRC", O_RDONLY);
    if (fd < 0) return;
    char b[192];
    int n = (int)read(fd, b, sizeof(b) - 1);
    close(fd);
    if (n <= 0) return;
    b[n] = 0;
    int i = 0;
    while (b[i] && b[i] != '\n' && b[i] != '\r' && b[i] != ' ') i++;
    b[i] = 0;
    int is_http  = strncmp(b, "http://", 7) == 0  && i > 7;
    int is_https = strncmp(b, "https://", 8) == 0 && i > 8;
    if ((is_http || is_https) && i < (int)sizeof(g_feed_repo))
        strcpy(g_feed_repo, b);
}

// Join repo base + relative path with exactly one slash.
static void feed_repo_url(const char *rel, char *out, int cap) {
    int o = 0;
    for (const char *p = g_feed_repo; *p && o < cap - 1; p++) out[o++] = *p;
    if (o > 0 && out[o - 1] == '/') o--;
    if (o < cap - 1) out[o++] = '/';
    while (*rel == '/') rel++;
    while (*rel && o < cap - 1) out[o++] = *rel++;
    out[o] = 0;
}

static void draw_feed(void);

// Async GET that keeps the window live (spinner + event pump). Returns body
// length into buf, or -1. Mirrors the App Store client's http_get_live().
static int feed_fetch(const char *url, void *buf, int cap, const char *what) {
    int job = http_fetch_start(url);
    if (job < 0) return -1;
    static const char spin[4] = { '|', '/', '-', '\\' };
    for (int i = 0; i < 600 && !g_feed_abort; i++) {   // up to ~30s
        int status = 0; unsigned int len = 0;
        int st = http_fetch_poll(job, &status, &len);
        if (st < 0) { http_fetch_cancel(job); return -1; }
        if (st == 1) return http_fetch_read(job, (char *)buf, (unsigned)cap);
        if (st == 2) { http_fetch_read(job, (char *)buf, (unsigned)cap); return -1; }
        char s[96]; scpy(s, what, sizeof(s));
        int sl = (int)strlen(s);
        if (sl < (int)sizeof(s) - 4) { s[sl] = ' '; s[sl+1] = ' '; s[sl+2] = spin[i & 3]; s[sl+3] = 0; }
        scpy(g_status, s, sizeof(g_status));
        draw_feed();
        gui_event_t ev;
        int et = win_get_event(g_win, &ev, 50);
        if (et == EVENT_WINDOW_CLOSE) g_feed_abort = 1;
    }
    http_fetch_cancel(job);
    return -1;
}

// Fetch the signed manifest, AUTHENTICATE it (App Store trust anchor), then
// parse it. Returns 0 on success (g_feed[] populated), non-zero on any failure
// with g_status set. FAIL CLOSED: an unsigned/altered manifest is refused.
static int feed_load_manifest(void) {
    g_feed_n = 0; g_feed_abort = 0;
    feed_repo_load();
    for (int w = 0; w < 20 && !sys_net_is_up() && !g_feed_abort; w++) {
        scpy(g_status, "Waiting for network...", sizeof(g_status));
        draw_feed();
        gui_event_t ev;
        int et = win_get_event(g_win, &ev, 300);
        if (et == EVENT_WINDOW_CLOSE) g_feed_abort = 1;
    }
    if (g_feed_abort) return -1;

    char murl[240]; feed_repo_url("flows/manifest.json", murl, sizeof(murl));
    int n = feed_fetch(murl, g_feed_manifest, sizeof(g_feed_manifest) - 1, "Loading flow catalog");
    if (n <= 0) { scpy(g_status, "Couldn't reach the flow feed server", sizeof(g_status)); return -1; }
    g_feed_manifest[n] = 0;

    // ---- AUTHENTICATE THE MANIFEST BEFORE TRUSTING A SINGLE BYTE OF IT. ----
    char surl[240]; feed_repo_url("flows/manifest.json.sig", surl, sizeof(surl));
    int sn = feed_fetch(surl, g_feed_sig, (int)sizeof(g_feed_sig), "Verifying signature");
    if (sn <= 0) { scpy(g_status, "Feed signature missing - refusing manifest", sizeof(g_status)); return -1; }
    int vrc = pkgsig_verify_manifest(g_feed_manifest, (size_t)n, g_feed_sig, (size_t)sn);
    if (vrc != PKGSIG_OK) {
        snprintf(g_status, sizeof(g_status), "Feed signature REFUSED: %s", pkgsig_strerror(vrc));
        return -1;
    }

    char perr[128];
    int fn = flowfeed_parse_manifest(g_feed_manifest, n, g_feed, FLOWFEED_MAX, perr, sizeof(perr));
    if (fn < 0) { snprintf(g_status, sizeof(g_status), "Malformed feed: %s", perr); return -1; }
    g_feed_n = fn;
    for (int i = 0; i < FLOWFEED_MAX; i++) g_feed_stat[i] = 0;
    if (g_feed_n == 0) { scpy(g_status, "Feed is empty", sizeof(g_status)); return -1; }
    snprintf(g_status, sizeof(g_status), "%d example flow%s available", g_feed_n, g_feed_n == 1 ? "" : "s");
    return 0;
}

static int feed_path_exists(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd >= 0) { close(fd); return 1; }
    return 0;
}

// Non-colliding install path for `base` (already sanitised). NEVER overwrites:
// <base>.yml, else <base>-1.yml, -2, ... Returns 1 if it had to rename, 0 if
// <base>.yml was free, -1 if no free name after 1000 tries.
static int feed_unique_path(const char *base, char *out, int cap, char *finalname, int fncap) {
    snprintf(out, cap, "/CONFIG/WORKFLOWS/%s.yml", base);
    if (!feed_path_exists(out)) { scpy(finalname, base, fncap); return 0; }
    for (int k = 1; k < 1000; k++) {
        char cand[FLOW_ID_MAX + 8];
        snprintf(cand, sizeof(cand), "%s-%d", base, k);
        snprintf(out, cap, "/CONFIG/WORKFLOWS/%s.yml", cand);
        if (!feed_path_exists(out)) { scpy(finalname, cand, fncap); return 1; }
    }
    return -1;
}

// Download + verify + validate + install ONE offered flow.
static void feed_install_one(int i) {
    if (i < 0 || i >= g_feed_n) return;
    flowfeed_entry_t *e = &g_feed[i];

    char base[FLOW_ID_MAX];
    if (flowfeed_sanitize_name(e->name, base, sizeof(base)) != 0) {
        g_feed_stat[i] = 3; scpy(g_status, "Rejected unsafe flow name", sizeof(g_status)); return;
    }

    // 1. fetch the YAML.
    char url[FF_PATH_MAX + 200]; feed_repo_url(e->path, url, sizeof(url));
    char what[80]; snprintf(what, sizeof(what), "Downloading %s", e->name);
    int n = feed_fetch(url, g_feed_yaml, sizeof(g_feed_yaml) - 1, what);
    if (n <= 0) { g_feed_stat[i] = 3; snprintf(g_status, sizeof(g_status), "Download failed: %s", e->name); return; }
    g_feed_yaml[n] = 0;

    // 2. integrity: sha256 must match the SIGNED manifest (App Store check).
    int vrc = pkgsig_verify_package(g_feed_yaml, (size_t)n, e->sha256);
    if (vrc != PKGSIG_OK) {
        g_feed_stat[i] = 3;
        snprintf(g_status, sizeof(g_status), "Integrity FAILED (%s): %s", pkgsig_strerror(vrc), e->name);
        return;
    }

    // 3. parse-VALIDATE with the runner's own parser BEFORE writing.
    static flow_graph_t vg; char verr[FLOW_ERR_MAX] = {0};
    memset(&vg, 0, sizeof(vg));
    if (flow_parse_bytes(g_feed_yaml, n, &vg, verr, sizeof(verr)) != 0) {
        g_feed_stat[i] = 3;
        snprintf(g_status, sizeof(g_status), "Invalid flow, not installed: %s", verr);
        return;
    }

    // 4. install to a unique path (never overwrite a user flow).
    sys_mkdir("/CONFIG", 0755);
    sys_mkdir("/CONFIG/WORKFLOWS", 0755);
    char path[224], finalname[FLOW_ID_MAX];
    int renamed = feed_unique_path(base, path, sizeof(path), finalname, sizeof(finalname));
    if (renamed < 0) { g_feed_stat[i] = 3; scpy(g_status, "Too many name collisions", sizeof(g_status)); return; }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { g_feed_stat[i] = 3; snprintf(g_status, sizeof(g_status), "Write failed: %s", path); return; }
    int w = 0;
    while (w < n) { int k = (int)write(fd, g_feed_yaml + w, (unsigned)(n - w)); if (k <= 0) break; w += k; }
    close(fd);
    if (w != n) { g_feed_stat[i] = 3; scpy(g_status, "Short write installing flow", sizeof(g_status)); return; }

    g_feed_stat[i] = renamed ? 2 : 1;
    if (renamed) snprintf(g_status, sizeof(g_status), "Installed as %s (%s existed)", finalname, base);
    else         snprintf(g_status, sizeof(g_status), "Installed %s", finalname);
}

static void feed_install_all(void) {
    int done = 0;
    for (int i = 0; i < g_feed_n && !g_feed_abort; i++) {
        if (g_feed_stat[i] == 1 || g_feed_stat[i] == 2) continue;   // already got it this session
        feed_install_one(i);
        if (g_feed_stat[i] == 1 || g_feed_stat[i] == 2) done++;
    }
    lib_refresh();
    if (!g_feed_abort) snprintf(g_status, sizeof(g_status), "Installed %d flow%s", done, done == 1 ? "" : "s");
}

static void feed_open(void) {
    g_feed_open = 1; g_lib_open = 0; g_feed_sel = 0; g_feed_scroll = 0;
    (void)feed_load_manifest();   // errors leave g_status set; the user can go Back
}

static void draw_feed(void) {
    win_draw_rect(g_win, 0, 0, WIN_W, WIN_H, C_SURFACE);
    win_draw_rect(g_win, 0, 0, WIN_W, 46, C_PANEL);
    win_draw_rect(g_win, 0, 46, WIN_W, 1, C_EDGE);
    ttf(16, 12, "Get More Flows", 18, C_INK);
    ttf(190, 17, "signed flow catalog from the update server", 12, C_INK_DIM);

    FD_back.x = WIN_W - 96; FD_back.y = 10; FD_back.w = 84; FD_back.h = 26;
    gui_button(g_win, FD_back.x, FD_back.y, FD_back.w, FD_back.h, "Back", GUI_BTN_SECONDARY, GUI_ST_NORMAL);

    int ay = 56, ah = 26, ax = 16;
    FD_get.x = ax; FD_get.y = ay; FD_get.w = 150; FD_get.h = ah;
    gui_button(g_win, FD_get.x, FD_get.y, FD_get.w, FD_get.h, "Download selected", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    FD_all.x = ax + 158; FD_all.y = ay; FD_all.w = 120; FD_all.h = ah;
    gui_button(g_win, FD_all.x, FD_all.y, FD_all.w, FD_all.h, "Download all", GUI_BTN_SECONDARY, GUI_ST_NORMAL);

    int lx = 16, lw = WIN_W - 32;
    FD_list_top = 94; FD_row_h = 50;
    int list_h = WIN_H - FD_list_top - 34;
    FD_vis = list_h / FD_row_h; if (FD_vis < 1) FD_vis = 1;
    win_draw_rect(g_win, lx, FD_list_top, lw, list_h, C_CANVAS);
    if (g_feed_scroll > g_feed_n - FD_vis) g_feed_scroll = g_feed_n - FD_vis;
    if (g_feed_scroll < 0) g_feed_scroll = 0;
    for (int r = 0; r < FD_vis; r++) {
        int i = g_feed_scroll + r; if (i >= g_feed_n) break;
        int ry = FD_list_top + r * FD_row_h;
        flowfeed_entry_t *e = &g_feed[i];
        gui_fill_rounded(g_win, lx + 4, ry + 4, lw - 8, FD_row_h - 8, 6, (i == g_feed_sel) ? C_NODE_BG : C_PANEL);
        if (i == g_feed_sel) gui_rounded_border(g_win, lx + 4, ry + 4, lw - 8, FD_row_h - 8, 6, C_ACCENT);
        ttf(lx + 14, ry + 7, e->name, 14, C_INK);
        ttf(lx + 14, ry + 26, e->desc[0] ? e->desc : "(no description)", 11, C_INK_DIM);
        const char *tag = 0; unsigned int tc = 0, tf = 0;
        if      (g_feed_stat[i] == 1) { tag = "INSTALLED";       tc = 0x14B8A6; tf = 0x061410; }
        else if (g_feed_stat[i] == 2) { tag = "SAVED (renamed)"; tc = 0xF59E0B; tf = 0x1A1206; }
        else if (g_feed_stat[i] == 3) { tag = "ERROR";           tc = 0xEF4444; tf = 0xFFFFFF; }
        if (tag) {
            int tw = (int)strlen(tag) * 7 + 12;
            gui_fill_rounded(g_win, lx + lw - tw - 12, ry + 10, tw, 16, 3, tc);
            ttf(lx + lw - tw - 6, ry + 11, tag, 10, tf);
        }
    }
    if (g_status[0]) ttf(16, WIN_H - 16, g_status, 11, C_INK_DIM);
    win_invalidate(g_win);
}

static void on_feed_click(int mx, int my) {
    if (in_rect(mx, my, FD_back.x, FD_back.y, FD_back.w, FD_back.h)) {
        g_feed_open = 0; g_lib_open = 1; lib_refresh(); return;
    }
    if (in_rect(mx, my, FD_get.x, FD_get.y, FD_get.w, FD_get.h)) {
        if (g_feed_n > 0) { feed_install_one(g_feed_sel); lib_refresh(); }
        return;
    }
    if (in_rect(mx, my, FD_all.x, FD_all.y, FD_all.w, FD_all.h)) { feed_install_all(); return; }
    if (mx >= 16 && mx < WIN_W - 16 && my >= FD_list_top && my < FD_list_top + FD_vis * FD_row_h) {
        int i = g_feed_scroll + (my - FD_list_top) / FD_row_h;
        if (i >= 0 && i < g_feed_n) g_feed_sel = i;
    }
}

static void on_feed_key(gui_event_t *ev) {
    if (ev->keycode == GUI_KEY_ESC)   { g_feed_open = 0; g_lib_open = 1; lib_refresh(); return; }
    if (ev->keycode == GUI_KEY_ENTER) { if (g_feed_n > 0) { feed_install_one(g_feed_sel); lib_refresh(); } return; }
    if (ev->keycode == GUI_KEY_UP)    { if (g_feed_sel > 0) g_feed_sel--; if (g_feed_sel < g_feed_scroll) g_feed_scroll = g_feed_sel; return; }
    if (ev->keycode == GUI_KEY_DOWN)  { if (g_feed_sel < g_feed_n - 1) g_feed_sel++; if (g_feed_sel >= g_feed_scroll + FD_vis) g_feed_scroll = g_feed_sel - FD_vis + 1; return; }
}

static void render(void) {
    if (g_feed_open) { draw_feed(); return; }
    if (g_lib_open) { draw_library(); return; }
    win_draw_rect(g_win, 0, 0, WIN_W, WIN_H, C_SURFACE);
    draw_canvas();
    draw_toolbar();
    draw_palette();
    draw_inspector();
    // status line along the toolbar underside
    if (g_status[0]) {
        int sw = (int)strlen(g_status) * 7 + 16;
        win_draw_rect(g_win, PALETTE_W + 4, CANVAS_Y + CANVAS_H - 22, sw, 18, C_PANEL);
        ttf(PALETTE_W + 10, CANVAS_Y + CANVAS_H - 21, g_status, 12, C_INK);
    }
    char zl[24]; snprintf(zl, sizeof(zl), "%d%%", (int)(g_zoom * 100));
    ttf(WIN_W - INSPECTOR_W - 44, CANVAS_Y + CANVAS_H - 21, zl, 12, C_INK_DIM);
    win_invalidate(g_win);
}

// ===========================================================================
// Hit-testing helpers.
// ===========================================================================
// Returns node index whose header/body contains screen (mx,my), else -1.
static int node_at(int mx, int my) {
    for (int i = g_g.nnodes - 1; i >= 0; i--) {
        int x = sx_of(g_nx[i]), y = sy_of(g_ny[i]);
        int w = scale(NODE_W), h = scale(node_height(&g_g.nodes[i]));
        if (in_rect(mx, my, x, y, w, h)) return i;
    }
    return -1;
}
// Output port under (mx,my): fills *node,*port and returns 1.
static int output_port_at(int mx, int my, int *node, int *port) {
    for (int i = g_g.nnodes - 1; i >= 0; i--) {
        port_t in[MAXP], out[MAXP]; int ni, no;
        get_ports(&g_g.nodes[i], in, &ni, out, &no);
        for (int p = 0; p < no; p++) {
            int px, py; output_port_screen(i, p, &px, &py);
            int dx = mx - px, dy = my - py;
            if (dx*dx + dy*dy <= PORT_HIT*PORT_HIT) { *node = i; *port = p; return 1; }
        }
    }
    return 0;
}
static int input_port_at(int mx, int my, int *node, int *port) {
    for (int i = g_g.nnodes - 1; i >= 0; i--) {
        port_t in[MAXP], out[MAXP]; int ni, no;
        get_ports(&g_g.nodes[i], in, &ni, out, &no);
        for (int p = 0; p < ni; p++) {
            int px, py; input_port_screen(i, p, &px, &py);
            int dx = mx - px, dy = my - py;
            if (dx*dx + dy*dy <= PORT_HIT*PORT_HIT) { *node = i; *port = p; return 1; }
        }
    }
    return 0;
}

static const char *out_port_name(int node, int port) {
    static char nm[FLOW_PORT_MAX];
    port_t in[MAXP], out[MAXP]; int ni, no;
    get_ports(&g_g.nodes[node], in, &ni, out, &no);
    scpy(nm, (port >= 0 && port < no) ? out[port].name : "out", sizeof(nm));
    return nm;
}
static const char *in_port_name(int node, int port) {
    static char nm[FLOW_PORT_MAX];
    port_t in[MAXP], out[MAXP]; int ni, no;
    get_ports(&g_g.nodes[node], in, &ni, out, &no);
    scpy(nm, (port >= 0 && port < ni) ? in[port].name : "in", sizeof(nm));
    return nm;
}

// ===========================================================================
// Event handling.
// ===========================================================================
static void on_toolbar_click(int mx, int my) {
    for (int i = 0; i < NTB; i++) {
        if (!in_rect(mx, my, g_tb[i].rx, g_tb[i].ry, g_tb[i].rw, g_tb[i].rh)) continue;
        const char *l = g_tb[i].label;
        if (strcmp(l, "Library") == 0) {
            g_lib_open = 1; g_lib_confirm_del = 0; g_lib_input = LIN_NONE;
            lib_refresh();
        } else if (strcmp(l, "+ Node") == 0) {
            int wx = wx_of(CANVAS_X + CANVAS_W / 2), wy = wy_of(CANVAS_Y + CANVAS_H / 2);
            int idx = add_node("transform", wx, wy);
            if (idx >= 0) { g_sel = idx; build_inspector(); scpy(g_status, "Added transform node", sizeof(g_status)); }
        } else if (strcmp(l, "Delete") == 0) {
            if (g_sel >= 0) { delete_node(g_sel); build_inspector(); scpy(g_status, "Deleted node", sizeof(g_status)); }
        } else if (strcmp(l, "Save") == 0) {
            save_workflow();
        } else if (strcmp(l, "Run") == 0) {
            run_workflow();
        }
        return;
    }
}
static void on_palette_click(int mx, int my) {
    for (int i = 0; i < NPAL; i++) {
        if (g_pal[i].is_header) continue;
        if (!in_rect(mx, my, g_pal[i].rx, g_pal[i].ry, g_pal[i].rw, g_pal[i].rh)) continue;
        int wx = wx_of(CANVAS_X + CANVAS_W / 3), wy = wy_of(CANVAS_Y + CANVAS_H / 3);
        int idx = add_node(g_pal[i].type, wx, wy);
        if (idx >= 0) {
            g_sel = idx; build_inspector();
            snprintf(g_status, sizeof(g_status), "Added %s node", g_pal[i].label);
        }
        return;
    }
}
// Returns 1 if the inspector consumed the click.
static int on_inspector_click(int mx, int my) {
    int ix = WIN_W - INSPECTOR_W;
    if (mx < ix) return 0;
    for (int i = 0; i < g_nfields; i++) {
        field_t *f = &g_fields[i];
        if (!in_rect(mx, my, f->rx, f->ry, f->rw, f->rh)) continue;
        if (f->kind == F_TEXT) {
            g_focus = i; tf_init(&f->tf, f->buf, f->cap);
        } else if (f->kind == F_ENUM) {
            enum_next(f);
            g_focus = -1;
        } else if (f->kind == F_LOOP) {
            loop_membership_next();
            g_focus = -1;
        }
        return 1;
    }
    g_focus = -1;
    return 1; // click landed on the inspector panel
}

static void on_mouse_down(int mx, int my) {
    g_lastmx = mx; g_lastmy = my; g_curmx = mx; g_curmy = my;
    if (g_feed_open) { on_feed_click(mx, my); return; }
    if (g_lib_open) { on_library_click(mx, my); return; }
    if (my < TOOLBAR_H) { on_toolbar_click(mx, my); return; }
    if (mx < PALETTE_W) { on_palette_click(mx, my); return; }
    if (mx >= WIN_W - INSPECTOR_W) { on_inspector_click(mx, my); return; }

    // canvas
    int node, port;
    if (output_port_at(mx, my, &node, &port)) {
        g_mode = WIRE; g_wire_src = node; g_wire_port = port;
        g_sel = node; build_inspector();
        return;
    }
    int ni = node_at(mx, my);
    if (ni >= 0) {
        g_sel = ni; build_inspector();
        g_mode = DRAG_NODE; g_drag_node = ni;
        g_grab_ox = wx_of(mx) - g_nx[ni];
        g_grab_oy = wy_of(my) - g_ny[ni];
        return;
    }
    // empty canvas: pan + clear selection
    g_mode = PAN; g_sel = -1; g_focus = -1; build_inspector();
}

static void on_mouse_move(int mx, int my) {
    g_curmx = mx; g_curmy = my;
    if (g_mode == DRAG_NODE && g_drag_node >= 0) {
        g_nx[g_drag_node] = wx_of(mx) - g_grab_ox;
        g_ny[g_drag_node] = wy_of(my) - g_grab_oy;
    } else if (g_mode == PAN) {
        g_pan_x += mx - g_lastmx;
        g_pan_y += my - g_lastmy;
    }
    g_lastmx = mx; g_lastmy = my;
}

static void on_mouse_up(int mx, int my) {
    if (g_mode == WIRE && g_wire_src >= 0) {
        int node, port;
        if (input_port_at(mx, my, &node, &port) && node != g_wire_src) {
            add_edge(g_g.nodes[g_wire_src].id, out_port_name(g_wire_src, g_wire_port),
                     g_g.nodes[node].id, in_port_name(node, port));
            scpy(g_status, "Wire connected", sizeof(g_status));
        } else {
            scpy(g_status, "Wire cancelled", sizeof(g_status));
        }
    }
    g_mode = IDLE; g_drag_node = -1; g_wire_src = -1; g_wire_port = -1;
}

static void on_scroll(int mx, int my, int delta) {
    if (g_lib_open) {
        g_lib_scroll += (delta > 0 ? -1 : 1);
        if (g_lib_scroll < 0) g_lib_scroll = 0;
        return;
    }
    float wx = (float)wx_of(mx), wy = (float)wy_of(my);
    float z2 = g_zoom * (delta > 0 ? 1.1f : 0.9f);
    if (z2 < 0.4f) z2 = 0.4f;
    if (z2 > 2.4f) z2 = 2.4f;
    g_zoom = z2;
    g_pan_x = mx - CANVAS_X - (int)(wx * g_zoom);
    g_pan_y = my - CANVAS_Y - (int)(wy * g_zoom);
}

static void on_key(gui_event_t *ev) {
    if (g_feed_open) { on_feed_key(ev); return; }
    if (g_lib_open) { on_library_key(ev); return; }
    // A focused inspector text field consumes typing first.
    if (g_focus >= 0 && g_focus < g_nfields && g_fields[g_focus].kind == F_TEXT) {
        if (ev->keycode == GUI_KEY_ENTER || ev->keycode == GUI_KEY_ESC) { g_focus = -1; return; }
        tf_handle_key(&g_fields[g_focus].tf, ev);
        return;
    }
    if (ev->keycode == GUI_KEY_DEL || ev->key_char == 0x7F) {
        if (g_sel >= 0) { delete_node(g_sel); build_inspector(); scpy(g_status, "Deleted node", sizeof(g_status)); }
    } else if (ev->key_char == 's' || ev->key_char == 'S') {
        save_workflow();
    } else if (ev->key_char == '+' || ev->key_char == '=') {
        on_scroll(CANVAS_X + CANVAS_W / 2, CANVAS_Y + CANVAS_H / 2, 1);
    } else if (ev->key_char == '-' || ev->key_char == '_') {
        on_scroll(CANVAS_X + CANVAS_W / 2, CANVAS_Y + CANVAS_H / 2, -1);
    }
}

// ===========================================================================
// flow.h platform seam. flow.c (compiled into this binary for its parser +
// model) references these because it also carries the node EXECUTOR. The editor
// is NOT the executor: it never calls flow_execute(). These are therefore
// honest "not the editor's job" implementations, not faked successes - the real
// capability-gated write / aiclient LLM / contract dispatch live in
// /APPS/FLOWRUN (userland/apps/flowrun/main.c), which the Run button launches.
// ===========================================================================
int flow_plat_write_file(const char *path, const char *text, int len,
                         int append, char *err, int errcap) {
    (void)path; (void)text; (void)len; (void)append;
    snprintf(err, errcap, "editor does not execute nodes; use Run (/APPS/FLOWRUN)");
    return -1;
}
int flow_plat_llm(const char *system, const char *prompt,
                  char *out, int outcap, char *err, int errcap) {
    (void)system; (void)prompt;
    if (out && outcap > 0) out[0] = 0;
    snprintf(err, errcap, "editor does not execute nodes; use Run (/APPS/FLOWRUN)");
    return -1;
}
int flow_plat_app_invoke(const char *app, int argc, char **argv,
                         char *out, int ocap) {
    (void)app; (void)argc; (void)argv;
    if (out && ocap > 0) out[0] = 0;
    return -1;
}
// #469 AI-VISION: perceive / decide / act. Same reason as the seams above:
// capturing a window, asking a model about the picture, and pressing a key in
// another app's window are all privileged, consented operations that belong to
// the RUNNER. Refused here with a reason, never faked.
int flow_plat_capture(const char *target, int rx, int ry, int rw, int rh,
                      int max_w, int max_h, int quality, const char *path,
                      int *out_w, int *out_h, char *err, int errcap) {
    (void)target; (void)rx; (void)ry; (void)rw; (void)rh;
    (void)max_w; (void)max_h; (void)quality; (void)path;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    snprintf(err, errcap, "editor does not execute nodes; use Run (/APPS/FLOWRUN)");
    return -1;
}
int flow_plat_llm_image(const char *system, const char *prompt,
                        const char *image_path,
                        char *out, int outcap, char *err, int errcap) {
    (void)system; (void)prompt; (void)image_path;
    if (out && outcap > 0) out[0] = 0;
    snprintf(err, errcap, "editor does not execute nodes; use Run (/APPS/FLOWRUN)");
    return -1;
}
int flow_plat_input_key(const char *target, int keycode,
                        int hold_ms, int gap_ms, char *err, int errcap) {
    (void)target; (void)keycode; (void)hold_ms; (void)gap_ms;
    snprintf(err, errcap, "editor does not execute nodes; use Run (/APPS/FLOWRUN)");
    return -1;
}

unsigned long flow_plat_now_ms(void) { return 0; }

// ===========================================================================
// main
// ===========================================================================
int main(int argc, char **argv) {
    if (argc >= 2 && argv[1] && argv[1][0]) scpy(g_name, argv[1], sizeof(g_name));

    seed_workflows();

    g_win = win_create("Maytera Flow", 60, 40, WIN_W + 4, WIN_H + 24);
    if (g_win < 0) return 1;

    load_theme();
    load_workflow(g_name);

    for (;;) {
        if (g_dirty) { render(); g_dirty = 0; }
        gui_event_t ev;
        int et = win_get_event(g_win, &ev, 100);   // blocks up to 100ms (no busy-wait)
        if (et <= 0) continue;                       // timeout / no event: loop, stay idle

        switch (et) {
            case EVENT_WINDOW_CLOSE:
                win_destroy(g_win);
                return 0;
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) on_mouse_down(ev.mouse_x, ev.mouse_y);
                g_dirty = 1; break;
            case EVENT_MOUSE_MOVE:
                on_mouse_move(ev.mouse_x, ev.mouse_y);
                if (g_mode != IDLE) g_dirty = 1;
                break;
            case EVENT_MOUSE_UP:
                on_mouse_up(ev.mouse_x, ev.mouse_y);
                g_dirty = 1; break;
            case EVENT_MOUSE_SCROLL:
                on_scroll(ev.mouse_x, ev.mouse_y, ev.scroll_delta);
                g_dirty = 1; break;
            case EVENT_KEY_DOWN:
                on_key(&ev);
                g_dirty = 1; break;
            case EVENT_REDRAW:
            case EVENT_RESIZE:
            case EVENT_WINDOW_FOCUS:
                g_dirty = 1; break;
            default:
                break;
        }
    }
}
