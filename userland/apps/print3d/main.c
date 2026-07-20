// 3D Print - MayteraOS userland app (#396) for the M3D Micro printer.
//
// Phase 1: host-side M3D g-code preprocessing (faithful port of M33-Fio) plus a
// transport layer whose phase-1 backend is a "virtual printer" that logs the
// produced M3D command stream to a file. No real USB / hardware yet (phase 2).
//
// UI follows the Settings/Files design language: the shared style engine
// primitives (gui_card/gui_button/gui_progress/...), antialiased TTF text, and
// the runtime theme color system so it recolors with the active theme.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/fcntl.h"
#include "m3d_gcode.h"
#include "m3d_preprocess.h"
#include "m3d_transport.h"
#include "stlview.h"
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"

// Route all in-window text through the antialiased TrueType path (as Settings).
#define win_draw_text(h, x, y, s, c)       win_draw_text_ttf((h), (x), (y), (s), 14, (c))
#define win_draw_text_small(h, x, y, s, c) win_draw_text_ttf((h), (x), (y), (s), 11, (c))

#define WIN_W 840
#define WIN_H 600
#define SIDEBAR_W 230
#define CONTENT_X (SIDEBAR_W + 1)
#define PAD 18

static int win = -1;

// ---- theme colors (queried from the active theme; recolors live) ----
static uint32_t COL_CONTENT_BG, COL_SIDEBAR_BG, COL_CARD_BG, COL_TEXT_PRIMARY,
    COL_TEXT_SECONDARY, COL_ACCENT, COL_ACCENT_HOVER, COL_INPUT_BG, COL_INPUT_BORDER,
    COL_BUTTON_BG, COL_SLIDER_TRACK, COL_SUCCESS, COL_WARNING, COL_ERROR, COL_SEP;

static void refresh_theme(void) {
    COL_CONTENT_BG    = theme_color(THEME_COLOR_WINDOW_BG);
    COL_TEXT_PRIMARY  = theme_color(THEME_COLOR_LABEL_TEXT);
    COL_ACCENT        = theme_color(THEME_COLOR_ACCENT);
    COL_INPUT_BG      = theme_color(THEME_COLOR_TEXTBOX_BG);
    COL_INPUT_BORDER  = theme_color(THEME_COLOR_TEXTBOX_BORDER);
    COL_BUTTON_BG     = theme_color(THEME_COLOR_BUTTON_FACE);
    COL_CARD_BG       = gui_lighten(COL_CONTENT_BG, 12);
    COL_SIDEBAR_BG    = gui_darken(COL_CONTENT_BG, 10);
    COL_TEXT_SECONDARY= gui_mix(COL_TEXT_PRIMARY, COL_CONTENT_BG, 96);
    COL_ACCENT_HOVER  = gui_lighten(COL_ACCENT, 26);
    COL_SLIDER_TRACK  = gui_mix(COL_INPUT_BG, COL_CONTENT_BG, 128);
    COL_SEP           = gui_mix(COL_TEXT_PRIMARY, COL_CONTENT_BG, 40);
    COL_SUCCESS = 0x0066BB66; COL_WARNING = 0x00DDAA44; COL_ERROR = 0x00DD5555;

    int base = (get_theme() == 0) ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN;
    gui_set_style(base);
    gui_palette_t pal;
    pal.surface = COL_CONTENT_BG; pal.surface_raised = COL_CARD_BG;
    pal.ink = COL_TEXT_PRIMARY; pal.ink_dim = COL_TEXT_SECONDARY;
    pal.accent = COL_ACCENT; pal.accent_hover = COL_ACCENT_HOVER;
    pal.border = COL_INPUT_BORDER; pal.field_bg = COL_INPUT_BG;
    pal.field_border = COL_INPUT_BORDER; pal.track = COL_SLIDER_TRACK;
    gui_set_palette(&pal);
}

// ---- clickable regions (rebuilt each draw, like Settings' focus list) ----
enum { BTN_PRINT=1, BTN_PAUSE, BTN_CANCEL, BTN_HOME, BTN_PREHEAT, BTN_EXTRUDE,
       BTN_RETRACT, BTN_TUP, BTN_TDN,
       // Filament-change wizard
       BTN_FILAMENT, BTN_FC_BACK, BTN_FC_HEAT, BTN_FC_PRIME, BTN_FC_UNLOAD,
       BTN_FC_SWAP, BTN_FC_LOAD, BTN_FC_FINISH,
       // Slice an STL model to g-code (CuraEngine)
       BTN_SLICE,
       // Slicer settings steppers (STL preview panel)
       BTN_LH_DN, BTN_LH_UP, BTN_WALL_DN, BTN_WALL_UP,
       BTN_INF_DN, BTN_INF_UP, BTN_SUP_TOGGLE,
       FILE_BASE=1000 };
typedef struct { int x, y, w, h, id; } hit_t;
static hit_t g_hits[80]; static int g_nhits;
static void hit_reset(void) { g_nhits = 0; }
static void hit_add(int x, int y, int w, int h, int id) {
    if (g_nhits < 80) { g_hits[g_nhits].x=x; g_hits[g_nhits].y=y; g_hits[g_nhits].w=w; g_hits[g_nhits].h=h; g_hits[g_nhits].id=id; g_nhits++; }
}
static int hit_test(int mx, int my) {
    for (int i = g_nhits - 1; i >= 0; i--)
        if (mx>=g_hits[i].x && mx<g_hits[i].x+g_hits[i].w && my>=g_hits[i].y && my<g_hits[i].y+g_hits[i].h) return g_hits[i].id;
    return 0;
}

// ---- file discovery ----
typedef struct { char path[160]; char name[96]; unsigned int size; int is_stl; } gfile_t;
static gfile_t g_files[64]; static int g_nfiles;
static int g_sel = -1;

// ---- STL 3D preview state ----
#define PREVIEW_W 344
#define PREVIEW_H 270
static unsigned int g_preview[PREVIEW_W * PREVIEW_H];   // offscreen ARGB panel
static int   g_stl_ok = 0;      // model loaded for the current selection
static int   g_stl_tris = 0;    // triangle count of the loaded model
static float g_yaw = 32.0f;     // preview orbit angle (degrees)
// #545: the STL preview used to orbit forever at ~5 Hz, so the app kept
// software-rendering the mesh every 200 ms and pegged ~10% of a core for as
// long as it was open (even backgrounded / not focused). Bound the orbit to a
// one-time reveal on select, then hold a static view so idle CPU drops to ~0.
#define ORBIT_REVEAL_TICKS 40   // ~one reveal spin (40 * 200 ms = 8 s) then stop
static int   g_orbit_ticks = 0; // >0 while the reveal spin is still running

// ---- slicer settings (passed to CuraEngine as -s overrides) ----
static const float LH_OPT[4] = { 0.10f, 0.15f, 0.20f, 0.30f };
static int g_lh_idx  = 2;       // -> 0.20 mm (default)
static int g_walls   = 2;       // perimeters, 1..4
static const int INF_OPT[6] = { 0, 10, 20, 40, 60, 100 };
static int g_inf_idx = 2;       // -> 20% (default)
static int g_supports = 0;      // 0 = off, 1 = on

// A .stl model needs slicing (via CuraEngine) before it can be printed; a
// g-code file can be printed directly. ends_stl() flags the former so the UI
// offers Slice instead of Print for it.
static int ends_stl(const char *n) {
    int len = (int)strlen(n);
    if (len < 4) return 0;
    const char *e = n + len - 4;
    char c0 = e[1], c1 = e[2], c2 = e[3];
    if (c0>='A'&&c0<='Z') c0=(char)(c0-'A'+'a');
    if (c1>='A'&&c1<='Z') c1=(char)(c1-'A'+'a');
    if (c2>='A'&&c2<='Z') c2=(char)(c2-'A'+'a');
    return e[0]=='.' && c0=='s' && c1=='t' && c2=='l';
}

static int ends_gcode(const char *n) {
    int len = (int)strlen(n);
    const char *exts[] = { ".gcode", ".gco", ".gc", ".g", 0 };
    for (int e = 0; exts[e]; e++) {
        int el = (int)strlen(exts[e]);
        if (len >= el) {
            int ok = 1;
            for (int i = 0; i < el; i++) {
                char a = n[len-el+i], b = exts[e][i];
                if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
                if (a != b) { ok = 0; break; }
            }
            if (ok) return 1;
        }
    }
    return 0;
}
// Index of an already-listed file by path, or -1. Lets the bundled built-in
// test prints and the directory scan coexist without listing a file twice.
static int find_by_path(const char *path) {
    for (int i = 0; i < g_nfiles; i++) if (!strcmp(g_files[i].path, path)) return i;
    return -1;
}
// Add a bundled built-in entry with a friendly display name (e.g. the shipped
// 3DBenchy test print). Shown even before the directory scan finds the file.
static void add_builtin(const char *name, const char *path) {
    if (g_nfiles >= 64 || find_by_path(path) >= 0) return;
    gfile_t *f = &g_files[g_nfiles++];
    strncpy(f->path, path, sizeof(f->path)-1); f->path[sizeof(f->path)-1]=0;
    strncpy(f->name, name, sizeof(f->name)-1); f->name[sizeof(f->name)-1]=0;
    f->size = 0; f->is_stl = ends_stl(path);
}
static void scan_dir(const char *dir) {
    dirent_t e;
    for (int i = 0; i < 512 && g_nfiles < 64; i++) {
        if (sys_readdir(dir, i, &e) != 0) break;
        if (DIRENT_IS_DIR(e)) continue;
        int stl = ends_stl(e.name);
        if (!ends_gcode(e.name) && !stl) continue;
        // Build the full path first so we can skip a file already listed as a
        // bundled built-in (keeps its friendly name; just fills in the size).
        char full[160]; int dl = (int)strlen(dir);
        strncpy(full, dir, sizeof(full)-1); full[sizeof(full)-1]=0;
        if (dl && dir[dl-1] != '/' && dl < (int)sizeof(full)-1) { full[dl]='/'; full[dl+1]=0; }
        strncat(full, e.name, sizeof(full)-strlen(full)-1);
        int ex = find_by_path(full);
        if (ex >= 0) { g_files[ex].size = e.size; g_files[ex].is_stl = stl; continue; }
        gfile_t *f = &g_files[g_nfiles++];
        strncpy(f->path, full, sizeof(f->path)-1); f->path[sizeof(f->path)-1]=0;
        strncpy(f->name, e.name, sizeof(f->name)-1); f->name[sizeof(f->name)-1]=0;
        f->size = e.size; f->is_stl = stl;
    }
}
static void scan_files(void) {
    g_nfiles = 0;
    // Bundled built-in test prints (sliced for the M3D Micro). These ship on the
    // boot disk under /GCODE so the user can pick a known-good print directly.
    add_builtin("3DBenchy (test)", "/GCODE/BENCHY.GCODE");
    add_builtin("Calibration cube 20mm (test)", "/GCODE/CUBE.GCODE");
    // Bundled STL model: slice it on-device with CuraEngine to prove the pipeline.
    add_builtin("Calibration cube 20mm (STL, slice me)", "/CUBE.STL");
    scan_dir("/");
    scan_dir("/GCODE");
    scan_dir("/MODELS");
    scan_dir("/HOME");
}

// ---- loaded g-code ----
static char  *g_buf;            // file contents
static char **g_lines;          // line pointers into g_buf
static int    g_nlines;
static m3d_ctx_t g_ctx;
// info
static int    g_layers;
static double g_filament_mm;
static double g_minx, g_maxx, g_miny, g_maxy, g_minz, g_maxz;
static int    g_centered;
static int    g_valid;
static int    g_est_min;

static void free_loaded(void) {
    if (g_lines) { free(g_lines); g_lines = 0; }
    if (g_buf)   { free(g_buf); g_buf = 0; }
    g_nlines = 0;
}

// Count printed layers + filament + travel (mirrors the reference's
// layer-detection heuristic) for the file-info panel.
static void compute_info(void) {
    m3d_gcode_t g; double cz = 0, ce = 0; int rel = 0;
    double layerZ[4096]; int nl = 0;
    double travel = 0, px = 0, py = 0; int havep = 0;
    g_filament_mm = 0;
    for (int i = 0; i < g_nlines; i++) {
        if (!m3d_gcode_parse(&g, g_lines[i])) continue;
        if (!m3d_gcode_has_value(&g, 'G')) continue;
        int gg = m3d_atoi_g(m3d_gcode_get_value(&g, 'G'));
        if (gg == 90) rel = 0; else if (gg == 91) rel = 1;
        else if (gg == 0 || gg == 1) {
            double ne = ce;
            if (m3d_gcode_has_value(&g,'Z')) cz = rel ? cz + m3d_atod(m3d_gcode_get_value(&g,'Z')) : m3d_atod(m3d_gcode_get_value(&g,'Z'));
            if (m3d_gcode_has_value(&g,'E')) ne = rel ? ne + m3d_atod(m3d_gcode_get_value(&g,'E')) : m3d_atod(m3d_gcode_get_value(&g,'E'));
            if (m3d_gcode_has_value(&g,'X') && m3d_gcode_has_value(&g,'Y')) {
                double x = m3d_atod(m3d_gcode_get_value(&g,'X')), y = m3d_atod(m3d_gcode_get_value(&g,'Y'));
                if (havep) { double dx=x-px, dy=y-py; travel += (dx<0?-dx:dx)+(dy<0?-dy:dy); }
                px = x; py = y; havep = 1;
            }
            // Only a real printing move (one that also moves X or Y) counts
            // toward filament + layer detection. A positive-E move with no X/Y
            // is a deretraction prime that just recovers a prior retract;
            // counting it double-counts filament. Real slicers (e.g. PrusaSlicer
            // with its G92 E0 + prime pattern) otherwise inflate the total ~2x.
            int moved_xy = m3d_gcode_has_value(&g,'X') || m3d_gcode_has_value(&g,'Y');
            if (ne > ce && moved_xy) {
                g_filament_mm += ne - ce;
                int found = 0; for (int k=0;k<nl;k++) if (layerZ[k]==cz){found=1;break;}
                if (!found && nl < 4096) layerZ[nl++] = cz;
            }
            ce = ne;
        } else if (gg == 92) { if (m3d_gcode_has_value(&g,'E')) ce = m3d_atod(m3d_gcode_get_value(&g,'E')); }
    }
    g_layers = nl;
    g_est_min = (int)(travel / 1500.0) + 1;   // rough: total travel at ~F1500
}

static void load_file(int idx) {
    free_loaded();
    stl_free();               // loading g-code releases any previewed STL model
    g_stl_ok = 0; g_stl_tris = 0;
    g_sel = idx;
    g_layers = 0; g_valid = 0; g_centered = 0; g_filament_mm = 0; g_est_min = 0;
    int fd = sys_open(g_files[idx].path, O_RDONLY);
    if (fd < 0) return;
    long cap = 0; long n = 0; char tmp[4096];
    for (;;) {
        long r = sys_read(fd, tmp, sizeof(tmp));
        if (r <= 0) break;
        char *nb = realloc(g_buf, n + r + 1);
        if (!nb) break;
        g_buf = nb; memcpy(g_buf + n, tmp, r); n += r; cap = n;
        if (n > 4*1024*1024) break;   // cap for phase 1
    }
    sys_close(fd);
    if (!g_buf) return;
    g_buf[n] = 0;
    // split into lines
    int lc = 0; for (long i = 0; i < n; i++) if (g_buf[i] == '\n') lc++;
    g_lines = malloc(sizeof(char*) * (lc + 2));
    g_nlines = 0;
    char *p = g_buf;
    while (*p) {
        g_lines[g_nlines++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = 0; p = nl + 1;
    }
    (void)cap;

    // dimensions + validity via the ported collector
    m3d_ctx_defaults(&g_ctx);
    m3d_reset_settings(&g_ctx);
    m3d_set_filament_type(&g_ctx, "PLA");
    m3d_set_firmware_type(&g_ctx, "iMe");
    g_ctx.useCenterModelPreprocessor = true;
    g_ctx.useValidationPreprocessor = true;
    g_ctx.usePreparationPreprocessor = true;
    g_ctx.useWaveBondingPreprocessor = true;
    g_ctx.useThermalBondingPreprocessor = true;
    g_valid = m3d_collect_print_information(&g_ctx, (const char *const *)g_lines, g_nlines, true) ? 1 : 0;
    g_centered = g_ctx.objectSuccessfullyCentered ? 1 : 0;
    g_minx = g_ctx.minXExtruderLow; g_maxx = g_ctx.maxXExtruderLow;
    g_miny = g_ctx.minYExtruderLow; g_maxy = g_ctx.maxYExtruderLow;
    g_minz = g_ctx.minZExtruder;    g_maxz = g_ctx.maxZExtruder;
    compute_info();
}

// Select an STL entry: mark it current and load its geometry for the 3D preview.
static void select_stl(int idx) {
    free_loaded();
    g_sel = idx; g_layers = 0; g_valid = 0; g_centered = 0; g_filament_mm = 0; g_est_min = 0;
    g_yaw = 32.0f;
    g_orbit_ticks = ORBIT_REVEAL_TICKS;   // arm a brief reveal spin for the new model
    g_stl_ok = (stl_load(g_files[idx].path) > 0);
    g_stl_tris = stl_count();
}

// ---- printer / transport state ----
static m3d_transport_t g_tx;
static int  g_tx_ok = 0;
static int  g_target_temp = 215;
static int  g_cur_temp = 24;
static int  g_heating = 0;
static int  g_printing = 0;
static int  g_progress = 0;         // 0..100
static unsigned long g_sent_this = 0;
static char g_status[96] = "Select a .gcode file, then Print.";

// ---- filament-change wizard state ----
enum { VIEW_PRINT = 0, VIEW_FILAMENT = 1 };
static int g_view      = VIEW_PRINT;
// Guided step index into FC_STEP_NAME[]: 0 heat, 1 prime, 2 unload, 3 swap,
// 4 load/purge, 5 finish. Tracks the furthest step reached (for sidebar marks).
static int g_fc_step   = 0;
static int g_fc_purges = 0;   // number of completed 100 mm load/purge passes
static int g_heat_ready = 0;  // nozzle >= FC_MIN_EXTRUDE_TEMP
static int g_tick      = 0;   // idle-tick counter, paces the M105 temperature poll
static int g_last_theme = -1; // #545: last seen theme id (get_theme()); used to
                              // ignore the redraw-feedback echo (see the loop)
// The M3D firmware rejects a cold extrude (error 1001 "cannot cold extrude"); the
// heater must be up before any G0 E move. 170 C is a safe floor for soft PLA.
#define FC_MIN_EXTRUDE_TEMP 170
#define FC_PLA_TEMP         215   // M33-Fio's PLA filament-change temperature
static const char *FC_STEP_NAME[6] = {
    "Heat nozzle", "Prime old strand", "Unload old",
    "Swap filament", "Load & purge new", "Finish & cool"
};

static unsigned long g_emit_count;
static void print_emit(void *user, const char *line) {
    (void)user;
    m3d_transport_send_line(&g_tx, line);
    g_emit_count++;
}

static void set_status(const char *s) { strncpy(g_status, s, sizeof(g_status)-1); g_status[sizeof(g_status)-1]=0; }

static void do_print(void) {
    if (g_sel < 0 || !g_lines) { set_status("No file selected."); return; }
    if (!g_tx_ok) { set_status("Virtual printer log unavailable."); return; }
    // re-run collect (settings may matter) then preprocess -> transport
    m3d_reset_settings(&g_ctx);
    m3d_collect_print_information(&g_ctx, (const char *const *)g_lines, g_nlines, true);
    m3d_reset_settings(&g_ctx);
    g_emit_count = 0;
    m3d_preprocess(&g_ctx, (const char *const *)g_lines, g_nlines, print_emit, NULL);
    g_sent_this = g_emit_count;
    g_printing = 1; g_progress = 100; g_heating = 1; g_target_temp = g_ctx.filamentTemperature;
    char b[96]; char nb[24]; m3d_ltoa((long)g_emit_count, nb);
    strcpy(b, "Sent "); strcat(b, nb); strcat(b, " M3D commands to virtual printer.");
    set_status(b);
}

static void send_manual(const char *cmd, const char *note) {
    if (!g_tx_ok) { set_status("Virtual printer log unavailable."); return; }
    m3d_transport_send_line(&g_tx, cmd);
    set_status(note);
}

// ---- drawing helpers (style-engine) ----
static void draw_kv(int x, int y, const char *k, const char *v) {
    win_draw_text(win, x, y, k, COL_TEXT_SECONDARY);
    win_draw_text(win, x + 150, y, v, COL_TEXT_PRIMARY);
}
static void num(char *b, long v) { m3d_ltoa(v, b); }
static void numd(char *b, double v) { m3d_dtoa(v, b); }

// ---- filament-change command helpers (absolute-E, M33-Fio framing) ----
//
// Emit a forward (target_mm > 0) or reverse (target_mm < 0) extrude using the
// exact framing that worked live: G90 (absolute) + G92 E0 (zero the axis), then
// a run of G0 E<abs> F<feed> moves capped at 2 mm each. Stepping in small
// increments is M33-Fio's anti-stall pattern for a long-idle M3D whose gears
// otherwise strip against a clogged strand.
static void fc_extrude(int target_mm, int feed) {
    if (!g_tx_ok) { set_status("Virtual printer log unavailable."); return; }
    m3d_transport_send_line(&g_tx, "G90");
    m3d_transport_send_line(&g_tx, "G92 E0");
    int fwd = (target_mm >= 0);
    int mag = fwd ? target_mm : -target_mm;
    char cmd[48], a[24], b[24];
    for (int e = 2; e <= mag; e += 2) {
        int ee = fwd ? e : -e;
        m3d_ltoa((long)ee, a); m3d_ltoa((long)feed, b);
        strcpy(cmd, "G0 E"); strcat(cmd, a); strcat(cmd, " F"); strcat(cmd, b);
        m3d_transport_send_line(&g_tx, cmd);
    }
    if (mag % 2) {   // exact final step for odd targets (e.g. the 15 mm prime)
        int ee = fwd ? mag : -mag;
        m3d_ltoa((long)ee, a); m3d_ltoa((long)feed, b);
        strcpy(cmd, "G0 E"); strcat(cmd, a); strcat(cmd, " F"); strcat(cmd, b);
        m3d_transport_send_line(&g_tx, cmd);
    }
}

// Poll the nozzle temperature (M105). On the real USB backend this parses the
// firmware's "T:<n>" reply; the phase-1 virtual backend has no reply, so the
// simulated warm-up in the idle loop drives the readout instead.
static void fc_poll_temp(void) {
    if (!g_tx_ok) return;
    m3d_transport_send_line(&g_tx, "M105");
    char rb[128];
    long r = m3d_transport_read(&g_tx, rb, (int)sizeof(rb) - 1);
    if (r <= 0) return;
    rb[r] = 0;
    for (int i = 0; i + 1 < (int)r; i++) {
        if (rb[i] == 'T' && rb[i+1] == ':') {
            int t = 0, j = i + 2, any = 0;
            while (rb[j] >= '0' && rb[j] <= '9') { t = t*10 + (rb[j]-'0'); j++; any = 1; }
            if (any && t > 0) g_cur_temp = t;
            break;
        }
    }
}

// A labelled -/+ stepper occupying `area_w` px: [-] on the left, [+] flush
// right, the current value between them. Used by the STL slicer-settings panel.
static void draw_stepper(int x, int y, int area_w, const char *label,
                         const char *val, int id_dn, int id_up) {
    win_draw_text_small(win, x, y, label, COL_TEXT_SECONDARY);
    int ry = y + 18;
    gui_button(win, x, ry, 26, 24, "-", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    hit_add(x, ry, 26, 24, id_dn);
    int upx = x + area_w - 26;
    gui_button(win, upx, ry, 26, 24, "+", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    hit_add(upx, ry, 26, 24, id_up);
    win_draw_text(win, x + 40, ry + 3, val, COL_TEXT_PRIMARY);
}

// STL selection view: a lit 3D preview of the model (blitted from an offscreen
// TinyGL render) on the left, an editable slicer-settings panel on the right,
// and the model summary + Slice action below. Assumes draw_print() already drew
// the sidebar (hit list) and header; this only adds the content-area regions.
static void draw_stl_view(int cx, int iy, int cw) {
    char b[96], nb[32];
    // ---------- preview card ----------
    int pcw = PREVIEW_W + 16, pch = PREVIEW_H + 16;
    gui_card(win, cx, iy, pcw, pch);
    if (g_stl_ok) {
        stl_render(g_preview, PREVIEW_W, PREVIEW_H, g_yaw);
        syscall5(SYS_WIN_BLIT, win, cx + 8, iy + 8,
                 (PREVIEW_W & 0xFFFF) | ((PREVIEW_H & 0xFFFF) << 16), (long)g_preview);
    } else {
        win_draw_text(win, cx + 20, iy + pch/2 - 8, "Could not load model.", COL_TEXT_SECONDARY);
    }

    // ---------- slicer settings card ----------
    int sx = cx + pcw + 14;
    int sw = cw - pcw - 14;
    gui_card(win, sx, iy, sw, pch);
    int x0 = sx + 12, aw = sw - 24;
    win_draw_text(win, x0, iy + 14, "Slice Settings", COL_TEXT_PRIMARY);
    int y = iy + 44;
    numd(nb, (double)LH_OPT[g_lh_idx]); strcpy(b, nb); strcat(b, " mm");
    draw_stepper(x0, y, aw, "Layer height", b, BTN_LH_DN, BTN_LH_UP); y += 52;
    num(nb, (long)g_walls);
    draw_stepper(x0, y, aw, "Walls (perimeters)", nb, BTN_WALL_DN, BTN_WALL_UP); y += 52;
    num(nb, (long)INF_OPT[g_inf_idx]); strcpy(b, nb); strcat(b, "%");
    draw_stepper(x0, y, aw, "Infill", b, BTN_INF_DN, BTN_INF_UP); y += 52;
    win_draw_text_small(win, x0, y, "Supports", COL_TEXT_SECONDARY);
    gui_button(win, x0, y + 18, aw, 26, g_supports ? "On" : "Off",
               g_supports ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    hit_add(x0, y + 18, aw, 26, BTN_SUP_TOGGLE);

    // ---------- model summary + slice action ----------
    int by = iy + pch + 14;
    win_draw_text(win, cx, by, g_files[g_sel].name, COL_TEXT_PRIMARY);
    if (g_stl_ok) {
        num(nb, (long)g_stl_tris);
        strcpy(b, "3D model (STL) - "); strcat(b, nb); strcat(b, " triangles, not yet sliced.");
    } else {
        strcpy(b, "3D model (STL) - preview unavailable.");
    }
    win_draw_text_small(win, cx, by + 22, b, COL_TEXT_SECONDARY);
    { char lh[16], wl[8], inf[8];
      numd(lh, (double)LH_OPT[g_lh_idx]); num(wl, (long)g_walls); num(inf, (long)INF_OPT[g_inf_idx]);
      strcpy(b, "Profile: "); strcat(b, lh); strcat(b, " mm, "); strcat(b, wl);
      strcat(b, " walls, "); strcat(b, inf); strcat(b, "% infill, supports ");
      strcat(b, g_supports ? "on." : "off."); }
    win_draw_text_small(win, cx, by + 40, b, COL_TEXT_SECONDARY);
    gui_button(win, cx, by + 64, 200, 36, "Slice to G-code", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    hit_add(cx, by + 64, 200, 36, BTN_SLICE);

    // ---------- status bar ----------
    win_draw_rect(win, CONTENT_X, WIN_H - 30, WIN_W - CONTENT_X, 1, COL_SEP);
    uint32_t sc = g_tx_ok ? COL_TEXT_SECONDARY : COL_WARNING;
    win_draw_text_small(win, cx, WIN_H - 22, g_status, sc);
}

static void draw_print(void) {
    hit_reset();
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, COL_CONTENT_BG);

    // ---------- sidebar: file list ----------
    win_draw_rect(win, 0, 0, SIDEBAR_W, WIN_H, COL_SIDEBAR_BG);
    win_draw_text(win, 16, 16, "Models & G-code", COL_TEXT_PRIMARY);
    win_draw_rect(win, 16, 40, SIDEBAR_W - 32, 1, COL_SEP);
    if (g_nfiles == 0) {
        win_draw_text_small(win, 16, 52, "No models or g-code found.", COL_TEXT_SECONDARY);
        win_draw_text_small(win, 16, 70, "Put a .stl or .gcode at / or /GCODE.", COL_TEXT_SECONDARY);
    }
    for (int i = 0; i < g_nfiles; i++) {
        int ry = 50 + i * 30;
        int sel = (i == g_sel);
        if (sel) gui_fill_rounded(win, 8, ry, SIDEBAR_W - 16, 26, 5, COL_ACCENT);
        win_draw_text(win, 18, ry + 5, g_files[i].name, sel ? gui_ink_on(COL_ACCENT) : COL_TEXT_PRIMARY);
        hit_add(8, ry, SIDEBAR_W - 16, 26, FILE_BASE + i);
    }

    // ---------- header ----------
    int cx = CONTENT_X + PAD;
    win_draw_text_ttf(win, cx, 16, "3D Print", 20, COL_TEXT_PRIMARY);
    win_draw_text_small(win, cx + 130, 24, "M3D Micro", COL_TEXT_SECONDARY);

    // ---------- file info card ----------
    int cw = WIN_W - CONTENT_X - 2 * PAD;
    int iy = 48;
    if (g_sel >= 0 && g_files[g_sel].is_stl) {
        // ===== STL selected: 3D preview + slicer settings =====
        draw_stl_view(cx, iy, cw);
        return;
    }
    gui_card(win, cx, iy, cw, 170);
    if (g_sel < 0) {
        win_draw_text(win, cx + 16, iy + 20, "No file loaded.", COL_TEXT_SECONDARY);
        win_draw_text_small(win, cx + 16, iy + 44, "Select a model or g-code file on the left.", COL_TEXT_SECONDARY);
    } else {
        char b[64];
        win_draw_text(win, cx + 16, iy + 14, g_files[g_sel].name, COL_TEXT_PRIMARY);
        int col1 = cx + 16, col2 = cx + cw/2 + 8, ry = iy + 44;
        num(b, (long)g_files[g_sel].size);            draw_kv(col1, ry, "Size (bytes)", b);
        num(b, (long)g_nlines);                        draw_kv(col2, ry, "Lines", b); ry += 26;
        num(b, (long)g_layers);                        draw_kv(col1, ry, "Layers", b);
        numd(b, g_filament_mm);                        draw_kv(col2, ry, "Filament (mm)", b); ry += 26;
        { char lo[24], hi[24]; numd(lo,g_minz); numd(hi,g_maxz); strcpy(b,lo); strcat(b," - "); strcat(b,hi); }
        draw_kv(col1, ry, "Z range", b);
        num(b, (long)g_est_min);                       draw_kv(col2, ry, "Est. print (min)", b); ry += 26;
        draw_kv(col1, ry, "In-bounds", g_valid ? "Yes" : "No (out of bed)");
        draw_kv(col2, ry, "Auto-centered", g_centered ? "Yes" : "Partial");
    }

    // ---------- temperature + progress card ----------
    int ty = iy + 186;
    gui_card(win, cx, ty, cw, 120);
    win_draw_text(win, cx + 16, ty + 12, "Printer", COL_TEXT_PRIMARY);
    char tb[64], nb[24];
    num(nb, g_cur_temp);  strcpy(tb, "Nozzle: "); strcat(tb, nb); strcat(tb, " C");
    win_draw_text(win, cx + 16, ty + 40, tb, COL_TEXT_SECONDARY);
    num(nb, g_target_temp); strcpy(tb, "Target: "); strcat(tb, nb); strcat(tb, " C");
    win_draw_text(win, cx + 180, ty + 40, tb, COL_TEXT_SECONDARY);
    // temp +/- buttons
    gui_button(win, cx + 320, ty + 34, 30, 24, "-", GUI_BTN_SECONDARY, GUI_ST_NORMAL); hit_add(cx+320, ty+34, 30, 24, BTN_TDN);
    gui_button(win, cx + 356, ty + 34, 30, 24, "+", GUI_BTN_SECONDARY, GUI_ST_NORMAL); hit_add(cx+356, ty+34, 30, 24, BTN_TUP);
    // progress
    win_draw_text_small(win, cx + 16, ty + 70, "Progress", COL_TEXT_SECONDARY);
    gui_progress(win, cx + 16, ty + 88, cw - 120, 16, g_progress);
    num(nb, g_progress); strcpy(tb, nb); strcat(tb, "%");
    win_draw_text(win, cx + cw - 90, ty + 84, tb, COL_TEXT_PRIMARY);

    // ---------- controls ----------
    int by = ty + 138;
    win_draw_text(win, cx, by, "Print", COL_TEXT_SECONDARY);
    int r1 = by + 22;
    int can_print = (g_sel >= 0 && !g_files[g_sel].is_stl);
    gui_button(win, cx,        r1, 120, 32, "Print",  GUI_BTN_PRIMARY,   can_print?GUI_ST_NORMAL:GUI_ST_DISABLED); hit_add(cx,       r1,120,32,BTN_PRINT);
    gui_button(win, cx + 132,  r1, 110, 32, "Pause",  GUI_BTN_SECONDARY, GUI_ST_NORMAL); hit_add(cx+132, r1,110,32,BTN_PAUSE);
    gui_button(win, cx + 252,  r1, 110, 32, "Cancel", GUI_BTN_SECONDARY, GUI_ST_NORMAL); hit_add(cx+252, r1,110,32,BTN_CANCEL);

    int my = r1 + 46;
    win_draw_text(win, cx, my, "Manual control", COL_TEXT_SECONDARY);
    int r2 = my + 22;
    gui_button(win, cx,        r2, 100, 30, "Home",    GUI_BTN_SECONDARY, GUI_ST_NORMAL); hit_add(cx,       r2,100,30,BTN_HOME);
    gui_button(win, cx + 110,  r2, 100, 30, "Preheat", GUI_BTN_SECONDARY, GUI_ST_NORMAL); hit_add(cx+110, r2,100,30,BTN_PREHEAT);
    gui_button(win, cx + 220,  r2, 100, 30, "Extrude", GUI_BTN_SECONDARY, GUI_ST_NORMAL); hit_add(cx+220, r2,100,30,BTN_EXTRUDE);
    gui_button(win, cx + 330,  r2, 100, 30, "Retract", GUI_BTN_SECONDARY, GUI_ST_NORMAL); hit_add(cx+330, r2,100,30,BTN_RETRACT);

    // Entry to the guided filament-change / load wizard.
    int r3 = r2 + 40;
    gui_button(win, cx, r3, 230, 32, "Change / Load Filament...", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    hit_add(cx, r3, 230, 32, BTN_FILAMENT);

    // ---------- status bar ----------
    win_draw_rect(win, CONTENT_X, WIN_H - 30, WIN_W - CONTENT_X, 1, COL_SEP);
    uint32_t sc = g_tx_ok ? COL_TEXT_SECONDARY : COL_WARNING;
    win_draw_text_small(win, cx, WIN_H - 22, g_status, sc);
}

// ---- guided filament-change / load wizard ----
//
// One panel modeling the exact sequence that worked live: heat -> prime the old
// strand FORWARD (frees a clog so it will reverse) -> unload -> swap -> load /
// purge (repeatable) -> cool. Extrude actions are gated on a hot nozzle because
// the firmware blocks a cold extrude.
static void fc_row(int x, int y, int w, int id, const char *label, int enabled,
                   int active, const char *desc) {
    gui_button(win, x, y, w, 30, label, active ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY,
               enabled ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    if (enabled) hit_add(x, y, w, 30, id);
    win_draw_text_small(win, x + w + 14, y + 8, desc,
                        active ? COL_TEXT_PRIMARY : COL_TEXT_SECONDARY);
}

static void draw_filament(void) {
    hit_reset();
    win_draw_rect(win, 0, 0, WIN_W, WIN_H, COL_CONTENT_BG);

    // ---------- sidebar: guided step checklist ----------
    win_draw_rect(win, 0, 0, SIDEBAR_W, WIN_H, COL_SIDEBAR_BG);
    win_draw_text(win, 16, 16, "Filament Change", COL_TEXT_PRIMARY);
    win_draw_rect(win, 16, 40, SIDEBAR_W - 32, 1, COL_SEP);
    for (int i = 0; i < 6; i++) {
        int ry = 54 + i * 40;
        int active = (i == g_fc_step);
        int done   = (i < g_fc_step);
        uint32_t bg = done ? COL_SUCCESS : (active ? COL_ACCENT : COL_SLIDER_TRACK);
        gui_fill_rounded(win, 16, ry, 24, 24, 12, bg);
        char nb[4]; nb[0] = (char)('1' + i); nb[1] = 0;
        win_draw_text_small(win, 16 + 8, ry + 6, nb, gui_ink_on(bg));
        win_draw_text_small(win, 50, ry + 6, FC_STEP_NAME[i],
                            (active || done) ? COL_TEXT_PRIMARY : COL_TEXT_SECONDARY);
    }
    gui_button(win, 16, WIN_H - 46, SIDEBAR_W - 32, 32, "< Back to Print",
               GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    hit_add(16, WIN_H - 46, SIDEBAR_W - 32, 32, BTN_FC_BACK);

    int cx = CONTENT_X + PAD;
    int cw = WIN_W - CONTENT_X - 2 * PAD;

    // ---------- header ----------
    win_draw_text_ttf(win, cx, 16, "Change / Load Filament", 20, COL_TEXT_PRIMARY);

    // ---------- safety strip ----------
    int sy = 46;
    gui_fill_rounded(win, cx, sy, cw, 28, 5, gui_mix(COL_WARNING, COL_CONTENT_BG, 200));
    win_draw_text_small(win, cx + 12, sy + 7,
        "Hot nozzle: the tip reaches ~215 C. Keep fingers clear - hold the filament, not the head.",
        COL_TEXT_PRIMARY);

    // ---------- heat card (step 1) ----------
    int ty = sy + 40;
    gui_card(win, cx, ty, cw, 100);
    win_draw_text(win, cx + 16, ty + 12, "1. Heat nozzle", COL_TEXT_PRIMARY);
    char tb[64], nb[24];
    num(nb, g_cur_temp);   strcpy(tb, "Nozzle: "); strcat(tb, nb); strcat(tb, " C");
    win_draw_text(win, cx + 16, ty + 40, tb, g_heat_ready ? COL_SUCCESS : COL_TEXT_SECONDARY);
    num(nb, g_target_temp); strcpy(tb, "Target: "); strcat(tb, nb); strcat(tb, " C");
    win_draw_text(win, cx + 170, ty + 40, tb, COL_TEXT_SECONDARY);
    gui_button(win, cx + 300, ty + 34, 30, 24, "-", GUI_BTN_SECONDARY, GUI_ST_NORMAL); hit_add(cx+300, ty+34,30,24,BTN_TDN);
    gui_button(win, cx + 336, ty + 34, 30, 24, "+", GUI_BTN_SECONDARY, GUI_ST_NORMAL); hit_add(cx+336, ty+34,30,24,BTN_TUP);
    gui_button(win, cx + 16, ty + 64, 150, 26, g_heating ? "Heating..." : "Heat nozzle",
               GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    hit_add(cx + 16, ty + 64, 150, 26, BTN_FC_HEAT);
    win_draw_text_small(win, cx + 178, ty + 70,
        g_heat_ready ? "Ready - safe to extrude." : "Warming up... extrude stays blocked until hot.",
        g_heat_ready ? COL_SUCCESS : COL_WARNING);

    // ---------- action list (steps 2-6) ----------
    int ay = ty + 116;
    gui_card(win, cx, ay, cw, 220);
    int bx = cx + 16, bw = 210, y = ay + 14;
    int hot = g_heat_ready;
    fc_row(bx, y, bw, BTN_FC_PRIME,  "2. Prime old (+15 mm)", hot, g_fc_step == 1,
           "Feed the OLD strand FORWARD first to soften/free a clog so it can reverse.");
    y += 38;
    fc_row(bx, y, bw, BTN_FC_UNLOAD, "3. Unload old (-100 mm)", hot, g_fc_step == 2,
           "Reverse the strand out, then pull it clear of the feeder by hand.");
    y += 38;
    fc_row(bx, y, bw, BTN_FC_SWAP,   "4. Filament swapped >", 1, g_fc_step == 3,
           "Insert the fresh spool into the feed tube, then confirm here.");
    y += 38;
    { char d[96], cnt[24];
      if (g_fc_purges > 0) { num(cnt, (long)(g_fc_purges * 100));
          strcpy(d, "Push new filament until the old colour is gone. Purged ");
          strcat(d, cnt); strcat(d, " mm."); }
      else strcpy(d, "Push new filament until the old colour is gone. Press again to purge more.");
      fc_row(bx, y, bw, BTN_FC_LOAD, "5. Load / purge (+100 mm)", hot, g_fc_step == 4, d);
    }
    y += 38;
    fc_row(bx, y, bw, BTN_FC_FINISH, "6. Finish & cool", 1, g_fc_step == 5,
           "Heater off (M104 S0) and motors released (M18). Nozzle stays hot a while.");

    // ---------- status bar ----------
    win_draw_rect(win, CONTENT_X, WIN_H - 30, WIN_W - CONTENT_X, 1, COL_SEP);
    uint32_t sc = g_tx_ok ? COL_TEXT_SECONDARY : COL_WARNING;
    win_draw_text_small(win, cx, WIN_H - 22, g_status, sc);
}

static void draw_all(void) {
    if (g_view == VIEW_FILAMENT) draw_filament();
    else                         draw_print();
    win_invalidate(win);
}

// Where CuraEngine writes the sliced g-code. Kept short (8.3-safe) so it lands
// cleanly on both the FAT ESP and the ext2 root.
#define SLICE_OUT "/SLICED.GCO"

// Slice the selected STL model to g-code with the CuraEngine port (/APPS/CURASLIC),
// then load the result so it can be printed. CuraEngine has a compiled-in default
// profile (0.2 mm layers, 2 walls, 20% infill); we pass only the output path and
// the model. The wait uses the kernel child-exit wait-queue (proc_wait): the
// slicer runs while this app sleeps, so no CPU is spun. The app window does not
// animate for the (bounded, user-initiated) slice, matching how the shell and the
// Win16 launcher already run a child to completion.
static void do_slice(void) {
    if (g_sel < 0 || !g_files[g_sel].is_stl) return;
    // Clear any stale output so a failed slice cannot look like a success.
    sys_unlink(SLICE_OUT);
    set_status("Slicing model with CuraEngine, please wait...");
    draw_all();

    // Translate the UI settings into CuraEngine -s key=value overrides. curaslice
    // applies its built-in preset first, then these override it. Lengths are in
    // microns; each "-s" and its "key=value" are SEPARATE argv tokens because
    // curaslice reads "-s" then consumes the following token.
    int lh_um = (int)(LH_OPT[g_lh_idx] * 1000.0f + 0.5f);   // 0.20 mm -> 200
    int infill = INF_OPT[g_inf_idx];
    int line_dist;                                          // sparse-infill line spacing (um)
    if (infill <= 0) line_dist = 100000;                   // effectively no infill
    else             line_dist = 400 * 100 / infill;       // 400 um extrusion width; 20% -> 2000
    int sup_angle = g_supports ? 50 : -1;
    int sup_every = g_supports ? 1 : 0;

    static char b_lt[40], b_ilt[40], b_ins[24], b_inf[48], b_sa[24], b_se[24];
    snprintf(b_lt,  sizeof b_lt,  "layerThickness=%d", lh_um);
    snprintf(b_ilt, sizeof b_ilt, "initialLayerThickness=%d", lh_um);
    snprintf(b_ins, sizeof b_ins, "insetCount=%d", g_walls);
    snprintf(b_inf, sizeof b_inf, "sparseInfillLineDistance=%d", line_dist);
    snprintf(b_sa,  sizeof b_sa,  "supportAngle=%d", sup_angle);
    snprintf(b_se,  sizeof b_se,  "supportEverywhere=%d", sup_every);

    char *av[24]; int n = 0;
    av[n++] = (char *)"curaslice";
    av[n++] = (char *)"-v";
    av[n++] = (char *)"-s"; av[n++] = b_lt;
    av[n++] = (char *)"-s"; av[n++] = b_ilt;
    av[n++] = (char *)"-s"; av[n++] = b_ins;
    av[n++] = (char *)"-s"; av[n++] = b_inf;
    av[n++] = (char *)"-s"; av[n++] = b_sa;
    av[n++] = (char *)"-s"; av[n++] = b_se;
    av[n++] = (char *)"-o"; av[n++] = (char *)SLICE_OUT;
    av[n++] = g_files[g_sel].path;
    av[n] = 0;
    int pid = sys_spawn_args("/APPS/CURASLIC", av, n);
    if (pid < 0) { set_status("Slice failed: could not start CuraEngine (/APPS/CURASLIC)."); return; }
    int st = 0;
    sys_waitpid(pid, &st, 0);   // blocks on the child-exit wait-queue

    // Verify CuraEngine produced g-code.
    int fd = sys_open(SLICE_OUT, O_RDONLY);
    if (fd < 0) { set_status("Slice produced no g-code (CuraEngine could not read the model?)."); return; }
    sys_close(fd);

    // Insert the sliced g-code into the file list and load it for printing.
    int idx = find_by_path(SLICE_OUT);
    if (idx < 0) {
        if (g_nfiles >= 64) { set_status("Sliced OK, but the file list is full."); return; }
        idx = g_nfiles++;
        strncpy(g_files[idx].path, SLICE_OUT, sizeof(g_files[idx].path)-1);
        g_files[idx].path[sizeof(g_files[idx].path)-1] = 0;
        strncpy(g_files[idx].name, "Sliced model (g-code)", sizeof(g_files[idx].name)-1);
        g_files[idx].name[sizeof(g_files[idx].name)-1] = 0;
        g_files[idx].size = 0; g_files[idx].is_stl = 0;
    }
    load_file(idx);
    set_status("Sliced to g-code. Review the details, then Print.");
}

static void handle_click(int id) {
    if (id >= FILE_BASE) {
        int fi = id - FILE_BASE;
        if (g_files[fi].is_stl) {
            // An STL cannot be printed directly; select it, load the 3D preview,
            // and offer Slice.
            select_stl(fi);
            set_status(g_stl_ok ? "STL model loaded. Adjust settings, then Slice."
                                : "STL selected (preview unavailable). Click Slice.");
        } else {
            load_file(fi); set_status("File loaded.");
        }
        draw_all(); return;
    }
    char cmd[32], nb[24];
    switch (id) {
        case BTN_SLICE:  do_slice(); break;
        case BTN_PRINT:  do_print(); break;
        case BTN_PAUSE:  send_manual("M25", "Paused (M25 sent to virtual printer)."); break;
        case BTN_CANCEL:
            send_manual("M104 S0", "Cancelled: heater off, motors released.");
            m3d_transport_send_line(&g_tx, "M18");
            g_printing = 0; g_progress = 0; g_heating = 0; break;
        case BTN_HOME:   send_manual("G28", "Homing (G28 sent)."); break;
        case BTN_PREHEAT:
            num(nb, g_target_temp); strcpy(cmd, "M104 S"); strcat(cmd, nb);
            g_heating = 1; send_manual(cmd, "Preheating nozzle."); break;
        case BTN_EXTRUDE:
            m3d_transport_send_line(&g_tx, "G91");
            send_manual("G0 E5 F300", "Extruding 5 mm.");
            m3d_transport_send_line(&g_tx, "G90"); break;
        case BTN_RETRACT:
            m3d_transport_send_line(&g_tx, "G91");
            send_manual("G0 E-5 F300", "Retracting 5 mm.");
            m3d_transport_send_line(&g_tx, "G90"); break;
        case BTN_TUP: if (g_target_temp < 315) g_target_temp += 5; break;
        case BTN_TDN: if (g_target_temp > 0) g_target_temp -= 5; break;

        // ----- slicer settings steppers -----
        case BTN_LH_DN:   if (g_lh_idx > 0) g_lh_idx--; set_status("Layer height changed."); break;
        case BTN_LH_UP:   if (g_lh_idx < 3) g_lh_idx++; set_status("Layer height changed."); break;
        case BTN_WALL_DN: if (g_walls > 1) g_walls--; set_status("Wall count changed."); break;
        case BTN_WALL_UP: if (g_walls < 4) g_walls++; set_status("Wall count changed."); break;
        case BTN_INF_DN:  if (g_inf_idx > 0) g_inf_idx--; set_status("Infill changed."); break;
        case BTN_INF_UP:  if (g_inf_idx < 5) g_inf_idx++; set_status("Infill changed."); break;
        case BTN_SUP_TOGGLE: g_supports = !g_supports;
            set_status(g_supports ? "Supports on." : "Supports off."); break;

        // ----- filament-change wizard -----
        case BTN_FILAMENT:
            g_view = VIEW_FILAMENT;
            if (g_fc_step == 0 && g_heat_ready) g_fc_step = 1;
            set_status("Filament change: heat the nozzle, then follow the numbered steps.");
            break;
        case BTN_FC_BACK:
            g_view = VIEW_PRINT;
            set_status("Select a .gcode file, then Print.");
            break;
        case BTN_FC_HEAT:
            if (g_target_temp < FC_MIN_EXTRUDE_TEMP) g_target_temp = FC_PLA_TEMP;
            m3d_transport_send_line(&g_tx, "M106");         // fan on (as M33-Fio)
            num(nb, g_target_temp); strcpy(cmd, "M104 S"); strcat(cmd, nb);
            m3d_transport_send_line(&g_tx, cmd);            // heat, do not block
            g_heating = 1;
            set_status("Heating nozzle - watch the temperature climb (M105).");
            break;
        case BTN_FC_PRIME:
            if (!g_heat_ready) { set_status("Too cold to extrude - heat the nozzle first."); break; }
            fc_extrude(15, 200);                            // FORWARD: free the clog
            g_fc_step = 2;
            set_status("Primed old filament forward 15 mm. Now Unload.");
            break;
        case BTN_FC_UNLOAD:
            if (!g_heat_ready) { set_status("Too cold to extrude - heat the nozzle first."); break; }
            fc_extrude(-100, 200);                          // REVERSE: pull old out
            g_fc_step = 3;
            set_status("Unloaded 100 mm. Pull the old strand clear, then Swap.");
            break;
        case BTN_FC_SWAP:
            g_fc_step = 4;
            set_status("Insert fresh filament into the feed, then Load / purge.");
            break;
        case BTN_FC_LOAD:
            if (!g_heat_ready) { set_status("Too cold to extrude - heat the nozzle first."); break; }
            fc_extrude(100, 150);                           // FORWARD: load & purge new
            g_fc_purges++;
            if (g_fc_step < 4) g_fc_step = 4;
            set_status("Purged new filament. Purge more until clean, or Finish & cool.");
            break;
        case BTN_FC_FINISH:
            m3d_transport_send_line(&g_tx, "M104 S0");      // heater off
            m3d_transport_send_line(&g_tx, "M18");          // motors off
            m3d_transport_send_line(&g_tx, "M107");         // fan off
            g_heating = 0; g_fc_step = 5;
            set_status("Done - heater off, motors released. Nozzle stays hot; let it cool.");
            break;
    }
    draw_all();
}

int main(int argc, char **argv) {
    win = win_create("3D Print", 90, 60, WIN_W, WIN_H);
    if (win < 0) { printf("3D Print: cannot create window\n"); return 1; }
    refresh_theme();

    // open the phase-1 virtual printer log
    g_tx_ok = (m3d_transport_open(&g_tx, M3D_BACKEND_VIRTUAL, "/M3DOUT.TXT") == 0);
    if (!g_tx_ok) set_status("Could not open /M3DOUT.TXT (virtual printer).");

    scan_files();
    if (argc > 1 && argv[1] && argv[1][0]) {
        // Launched with a file path (e.g. Files "Open with"). Locate it in the
        // list, adding it if the scan did not reach its directory.
        int idx = -1;
        for (int i = 0; i < g_nfiles; i++) if (!strcmp(g_files[i].path, argv[1])) { idx = i; break; }
        if (idx < 0 && g_nfiles < 64) {
            idx = g_nfiles++;
            strncpy(g_files[idx].path, argv[1], sizeof(g_files[idx].path)-1);
            g_files[idx].path[sizeof(g_files[idx].path)-1] = 0;
            const char *bn = argv[1]; for (const char *p = argv[1]; *p; p++) if (*p=='/') bn = p+1;
            strncpy(g_files[idx].name, bn, sizeof(g_files[idx].name)-1);
            g_files[idx].name[sizeof(g_files[idx].name)-1] = 0;
            g_files[idx].size = 0; g_files[idx].is_stl = ends_stl(argv[1]);
        }
        if (idx >= 0) {
            if (g_files[idx].is_stl) {
                // Open-with-an-STL: load its preview, then slice straight away.
                select_stl(idx); do_slice();
            } else {
                load_file(idx);
            }
        }
    }
    if (g_sel < 0 && g_nfiles > 0) {
        // Prefer a printable g-code that ACTUALLY exists on disk; otherwise fall
        // back to the first STL so its 3D preview is shown (e.g. the bundled
        // calibration cube). This avoids auto-selecting a bundled-but-absent
        // g-code entry, and makes the model preview the default on a model-only
        // disk.
        int chosen = -1;
        for (int i = 0; i < g_nfiles; i++) {
            if (g_files[i].is_stl) continue;
            int fd = sys_open(g_files[i].path, O_RDONLY);
            if (fd >= 0) { sys_close(fd); chosen = i; break; }
        }
        if (chosen >= 0) { load_file(chosen); }
        else {
            int si = -1; for (int i = 0; i < g_nfiles; i++) if (g_files[i].is_stl) { si = i; break; }
            if (si >= 0) { select_stl(si); set_status(g_stl_ok ? "STL model loaded. Adjust settings, then Slice."
                                                               : "STL model selected. Click Slice to generate g-code."); }
            else load_file(0);
        }
    }

    draw_all();
    g_last_theme = get_theme();

    gui_event_t ev;
    int running = 1;
    while (running) {
        // Block until a real event when there is nothing to animate, so an idle
        // 3D Print window costs ~0 CPU. Wake on the 200 ms tick ONLY while
        // something must actually move: the reveal orbit, the nozzle-temp
        // easing toward target, or (real printer) the wizard M105 poll. When
        // none hold, pass -1 so win_get_event sleeps on its wait-queue (#453),
        // instead of the old unconditional 5 Hz poll that never blocked.
        int stl_orbiting = (g_orbit_ticks > 0 && g_view == VIEW_PRINT && g_sel >= 0 &&
                            g_files[g_sel].is_stl && g_stl_ok &&
                            g_stl_tris > 0 && g_stl_tris < 20000);
        int temp_easing  = (g_heating && g_cur_temp != g_target_temp);
        int wizard_poll  = (g_view == VIEW_FILAMENT && g_heating);
        int want_anim    = (stl_orbiting || temp_easing || wizard_poll);
        int et = win_get_event(win, &ev, want_anim ? 200 : -1);
        if (et == 0) {
            g_tick++;
            int dirty = 0;
            // In the wizard, poll the real nozzle temp ~1 s (M105); on the virtual
            // backend there is no reply, so the simulation below drives the readout.
            if (g_view == VIEW_FILAMENT && g_heating && (g_tick % 5 == 0)) fc_poll_temp();
            // Ease the nozzle temperature toward target while heating.
            if (g_heating && g_cur_temp != g_target_temp) {
                int d = g_target_temp - g_cur_temp;
                g_cur_temp += (d > 0) ? (d > 4 ? 4 : d) : (d < -4 ? -4 : d);
                dirty = 1;
            }
            int ready = (g_cur_temp >= FC_MIN_EXTRUDE_TEMP);
            if (ready != g_heat_ready) {
                g_heat_ready = ready;
                if (ready && g_view == VIEW_FILAMENT && g_fc_step == 0) g_fc_step = 1;
                dirty = 1;
            }
            // Orbit the STL preview for a BOUNDED reveal, then stop and hold a
            // static view. Perpetual orbiting re-rendered the mesh 5x/s forever
            // (~10% of a core while the app was merely open), the exact idle
            // busy-work the wait-queue rule forbids (#211/#347/#453 class).
            // Large meshes never orbit (the per-frame software render is heavy).
            if (g_orbit_ticks > 0 && g_view == VIEW_PRINT && g_sel >= 0 &&
                g_files[g_sel].is_stl && g_stl_ok &&
                g_stl_tris > 0 && g_stl_tris < 20000) {
                g_yaw += 3.0f; if (g_yaw >= 360.0f) g_yaw -= 360.0f;
                g_orbit_ticks--;
                dirty = 1;
            }
            if (dirty) draw_all();
            continue;
        }
        switch (ev.type) {
            case EVENT_REDRAW:
                // Repaint ONLY on a real theme change; a REDRAW with the same
                // theme is just the echo of our own win_invalidate() and must
                // be ignored, or present->REDRAW->present spins a core forever.
                {
                    int th = get_theme();
                    if (th != g_last_theme) { g_last_theme = th; refresh_theme(); draw_all(); }
                }
                break;
            case EVENT_WINDOW_CLOSE: running = 0; break;
            case EVENT_KEY_DOWN: if (ev.key_char == 27) running = 0; break;
            case EVENT_MOUSE_DOWN:
                if (ev.mouse_buttons & MOUSE_BUTTON_LEFT) {
                    int id = hit_test(ev.mouse_x, ev.mouse_y);
                    if (id) handle_click(id);
                }
                break;
            default: break;
        }
    }
    m3d_transport_close(&g_tx);
    free_loaded();
    stl_free();
    win_destroy(win);
    return 0;
}
