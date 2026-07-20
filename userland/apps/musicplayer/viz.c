// viz.c - MilkDrop-1-style TinyGL audio visualizer. See viz.h.
//
// Technique (authentic non-shader MilkDrop 1.x):
//   1. Each frame the PREVIOUS frame is held in a GL texture and drawn back as a
//      warped mesh (per-vertex zoom / rotate / translate / sinusoidal warp) and
//      colour-modulated by a decay factor < 1. That recursive feedback is what
//      makes the tunnels / swirls / trails.
//   2. A live WAVEFORM (bright additive line) and a SPECTRUM (additive bars /
//      radial rays) are drawn on top, driven by the player's spectrum + the real
//      DAC playback position.
//   3. An energy-based BEAT detector pulses the zoom + brightness.
//   4. The finished frame is copied back into the feedback texture for next time.
//
// Rendered at a fixed internal RES x RES (= TinyGL's texture dimension so the
// feedback texture captures the whole frame with no scaling), then nearest-
// scaled to the window on blit.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/string.h"
#include "../include/GL/gl.h"
#include "../include/zbuffer.h"
#include <math.h>
#include "viz.h"

#ifndef GL_UNSIGNED_BYTE
#define GL_UNSIGNED_BYTE 0x1401
#endif

#define RES        256            // == TGL_FEATURE_TEXTURE_DIM (256)
#define MESH       20             // warp mesh resolution (MESH x MESH cells)
#define NW         96             // waveform sample count
#define VIZ_MAXW   720
#define VIZ_MAXH   540
#define VIZ_REQ_W  520
#define VIZ_REQ_H  400
#define PI         3.14159265358979

#define NPRESET    3
enum { P_TUNNEL = 0, P_SWIRL, P_STARBURST };
static const char *g_pnames[NPRESET] = { "TUNNEL", "SWIRL", "STARBURST" };

static int      g_open = 0;
static int      g_win = -1;
static ZBuffer *g_zb = 0;
static GLuint   g_tex = 0;
static int      g_cw = VIZ_REQ_W, g_ch = VIZ_REQ_H;
static int      g_preset = P_TUNNEL;

// feedback / blit buffers
static uint32_t g_argb[RES * RES];        // rendered frame (0x00RRGGBB from TinyGL)
static unsigned char g_texrgb[RES * RES * 3];
static uint32_t g_blit[VIZ_MAXW * VIZ_MAXH];
static int      g_tex_primed = 0;

// audio-driven state
static int      g_bars[32];
static int      g_nbars = 0;
static int      g_playing = 0;
static long     g_last_pos = -1;
static double   g_phase = 0.0;             // pos-driven animation phase
static double   g_rot_acc = 0.0;           // accumulated rotation
static double   g_energy = 0.0;            // instantaneous spectrum energy
static double   g_avg = 1.0;               // running-average energy
static double   g_beat = 0.0;              // smoothed beat pulse 0..~1.5
static double   g_wave[NW];
static unsigned g_frames = 0;

// ---------------------------------------------------------------------------
static void build_wave(void) {
    // hifi-vizreact: scale the whole scope by the live spectrum energy + beat so
    // it visibly swells and jumps with the music instead of idling as a fixed line.
    double amp = g_playing ? 1.0 : 0.14;
    double e = g_energy / ((g_nbars ? g_nbars : 1) * 40.0);   // 0..~1.6
    if (e > 1.6) e = 1.6;
    for (int i = 0; i < NW; i++) {
        double x = (double)i / (double)NW;
        double w = 0.42 * sin(x * 8.0 * PI + g_phase * 2.0) * (0.5 + 0.8 * e)
                 + 0.30 * sin(x * 17.0 * PI - g_phase * 3.1) * (0.4 + e)
                 + 0.18 * sin(x * 29.0 * PI + g_phase * 5.3)
                 + 0.10 * sin(x * 53.0 * PI - g_phase * 7.7) * e;
        g_wave[i] = w * amp * (0.55 + 0.9 * g_beat);
    }
}

// map a destination screen coord (u,v in [0,1]) to a source texcoord for the
// warp feedback, using the current preset's parameters.
static double s_rot, s_zoom, s_warp, s_wfreq, s_dx, s_dy, s_decay;
static void set_preset_params(void) {
    double b = g_beat;
    g_rot_acc += (g_playing ? 1.0 : 0.15) * (0.0);   // per-preset below sets speed
    switch (g_preset) {
    case P_TUNNEL:
        s_zoom  = 1.028 + 0.045 * b;
        g_rot_acc += 0.0040 + 0.010 * b;
        s_rot   = g_rot_acc;
        s_warp  = 0.010 + 0.010 * b;
        s_wfreq = 5.0;
        s_dx = 0.0; s_dy = 0.0;
        s_decay = 0.955;
        break;
    case P_SWIRL:
        s_zoom  = 1.015 + 0.030 * b;
        g_rot_acc += 0.020 + 0.020 * b;
        s_rot   = g_rot_acc;
        s_warp  = 0.030 + 0.020 * b;
        s_wfreq = 6.0;
        s_dx = 0.010 * sin(g_phase * 0.7);
        s_dy = 0.010 * cos(g_phase * 0.9);
        s_decay = 0.935;
        break;
    default: /* P_STARBURST */
        s_zoom  = 0.982 - 0.030 * b;    // < 1 => explode outward
        g_rot_acc += -0.012 - 0.008 * b;
        s_rot   = g_rot_acc;
        s_warp  = 0.018 + 0.012 * b;
        s_wfreq = 4.0;
        s_dx = 0.0; s_dy = 0.0;
        s_decay = 0.930;
        break;
    }
}

static void mapuv(double u, double v, double *su, double *sv) {
    double dx = u - 0.5, dy = v - 0.5;
    double c = cos(s_rot), s = sin(s_rot);
    double rx = dx * c - dy * s;
    double ry = dx * s + dy * c;
    rx /= s_zoom; ry /= s_zoom;
    rx += s_warp * sin(s_wfreq * v + g_phase);
    ry += s_warp * cos(s_wfreq * u + g_phase * 1.3);
    rx += s_dx; ry += s_dy;
    double a = 0.5 + rx, b = 0.5 + ry;
    if (a < 0.0) a = 0.0; else if (a > 1.0) a = 1.0;
    if (b < 0.0) b = 0.0; else if (b > 1.0) b = 1.0;
    *su = a; *sv = b;
}

// ---------------------------------------------------------------------------
static void gl_setup_2d(void) {
    // identity projection + modelview => vertices in NDC [-1,1] map straight to
    // the viewport (glOrtho is a no-op stub in this TinyGL, so we do it directly)
    glViewport(0, 0, RES, RES);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
}

static void warp_pass(void) {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    double d = s_decay;
    glColor3f((float)d, (float)d, (float)d);
    for (int gy = 0; gy < MESH; gy++) {
        double v0 = (double)gy / MESH, v1 = (double)(gy + 1) / MESH;
        for (int gx = 0; gx < MESH; gx++) {
            double u0 = (double)gx / MESH, u1 = (double)(gx + 1) / MESH;
            double su, sv;
            glBegin(GL_QUADS);
            mapuv(u0, v0, &su, &sv); glTexCoord2f((float)su, (float)sv); glVertex3f((float)(u0*2-1), (float)(v0*2-1), 0.0f);
            mapuv(u1, v0, &su, &sv); glTexCoord2f((float)su, (float)sv); glVertex3f((float)(u1*2-1), (float)(v0*2-1), 0.0f);
            mapuv(u1, v1, &su, &sv); glTexCoord2f((float)su, (float)sv); glVertex3f((float)(u1*2-1), (float)(v1*2-1), 0.0f);
            mapuv(u0, v1, &su, &sv); glTexCoord2f((float)su, (float)sv); glVertex3f((float)(u0*2-1), (float)(v1*2-1), 0.0f);
            glEnd();
        }
    }
}

// per-preset overlay colour (waveform / rays)
static void preset_color(double t, float *r, float *g, float *b) {
    double bright = 0.7 + 0.5 * g_beat;
    if (bright > 1.0) bright = 1.0;
    switch (g_preset) {
    case P_TUNNEL:
        *r = (float)(0.30 * bright); *g = (float)(0.95 * bright); *b = (float)(1.00 * bright); break;
    case P_SWIRL:
        *r = (float)(1.00 * bright); *g = (float)((0.30 + 0.5 * t) * bright); *b = (float)(0.90 * bright); break;
    default:
        *r = (float)(1.00 * bright); *g = (float)((0.55 + 0.4 * t) * bright); *b = (float)(0.10 * bright); break;
    }
}

static void overlay_pass(void) {
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);          // additive glow
    glBlendEquation(GL_FUNC_ADD);

    float r, g, b;
    if (g_preset == P_STARBURST) {
        // radial rays: one per spectrum bar, length driven by that bar
        int n = g_nbars ? g_nbars : 1;
        for (int i = 0; i < n; i++) {
            double ang = (double)i / n * 2.0 * PI + g_rot_acc;
            double len = 0.12 + (g_bars[i] / 63.0) * 0.85;
            preset_color((double)i / n, &r, &g, &b);
            glColor3f(r, g, b);
            glBegin(GL_LINES);
            glVertex3f(0.0f, 0.0f, 0.0f);
            glVertex3f((float)(cos(ang) * len), (float)(sin(ang) * len), 0.0f);
            glEnd();
        }
        // plus a small central waveform ring
        preset_color(0.5, &r, &g, &b);
        glColor3f(r, g, b);
        for (int i = 0; i < NW; i++) {
            double a0 = (double)i / NW * 2.0 * PI;
            double a1 = (double)(i + 1) / NW * 2.0 * PI;
            double r0 = 0.18 + g_wave[i] * 0.12;
            double r1 = 0.18 + g_wave[(i + 1) % NW] * 0.12;
            glBegin(GL_LINES);
            glVertex3f((float)(cos(a0) * r0), (float)(sin(a0) * r0), 0.0f);
            glVertex3f((float)(cos(a1) * r1), (float)(sin(a1) * r1), 0.0f);
            glEnd();
        }
    } else {
        // horizontal waveform across the middle (classic MilkDrop scope)
        for (int i = 0; i + 1 < NW; i++) {
            double x0 = -0.9 + 1.8 * ((double)i / (NW - 1));
            double x1 = -0.9 + 1.8 * ((double)(i + 1) / (NW - 1));
            double y0 = g_wave[i] * 0.6;
            double y1 = g_wave[i + 1] * 0.6;
            preset_color((double)i / NW, &r, &g, &b);
            glColor3f(r, g, b);
            glBegin(GL_LINES);
            glVertex3f((float)x0, (float)y0, 0.0f);
            glVertex3f((float)x1, (float)y1, 0.0f);
            glEnd();
        }
        // spectrum bars along the bottom edge (additive)
        int n = g_nbars ? g_nbars : 1;
        double bw = 1.9 / n;
        for (int i = 0; i < n; i++) {
            double h = (g_bars[i] / 63.0) * 0.9;
            double x0 = -0.95 + i * bw, x1 = x0 + bw * 0.8;
            double yb = -0.98, yt = -0.98 + h;
            preset_color((double)i / n, &r, &g, &b);
            glColor3f(r * 0.8f, g * 0.8f, b * 0.8f);
            glBegin(GL_QUADS);
            glVertex3f((float)x0, (float)yb, 0.0f);
            glVertex3f((float)x1, (float)yb, 0.0f);
            glVertex3f((float)x1, (float)yt, 0.0f);
            glVertex3f((float)x0, (float)yt, 0.0f);
            glEnd();
        }
    }
    glDisable(GL_BLEND);
}

// copy the rendered frame into the feedback texture for the next frame
static void capture_feedback(void) {
    ZB_copyFrameBuffer(g_zb, g_argb, RES * (int)sizeof(uint32_t));
    for (int i = 0; i < RES * RES; i++) {
        uint32_t p = g_argb[i];
        g_texrgb[i * 3 + 0] = (unsigned char)((p >> 16) & 0xFF);
        g_texrgb[i * 3 + 1] = (unsigned char)((p >> 8) & 0xFF);
        g_texrgb[i * 3 + 2] = (unsigned char)(p & 0xFF);
    }
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, 3, RES, RES, 0, GL_RGB, GL_UNSIGNED_BYTE, g_texrgb);
    g_tex_primed = 1;
}

static void blit_to_window(void) {
    int cw = g_cw, ch = g_ch;
    if (cw > VIZ_MAXW) cw = VIZ_MAXW;
    if (ch > VIZ_MAXH) ch = VIZ_MAXH;
    for (int y = 0; y < ch; y++) {
        int sy = y * RES / ch;
        uint32_t *dst = g_blit + (long)y * cw;
        const uint32_t *src = g_argb + (long)sy * RES;
        for (int x = 0; x < cw; x++)
            dst[x] = src[x * RES / cw] | 0xFF000000u;
    }
    syscall5(SYS_WIN_BLIT, g_win, 0, 0, (cw & 0xFFFF) | ((ch & 0xFFFF) << 16), (long)g_blit);
    win_invalidate(g_win);
}

// ---------------------------------------------------------------------------
int viz_open(void) {
    if (g_open) return 0;
    g_win = win_create("Maytera Viz - MilkDrop", 220, 70, VIZ_REQ_W, VIZ_REQ_H);
    if (g_win < 0) return -1;
    g_cw = VIZ_REQ_W; g_ch = VIZ_REQ_H;
    if (win_get_size(g_win, &g_cw, &g_ch) != 0 || g_cw <= 0 || g_ch <= 0) { g_cw = VIZ_REQ_W; g_ch = VIZ_REQ_H; }
    if (g_cw > VIZ_MAXW) g_cw = VIZ_MAXW;
    if (g_ch > VIZ_MAXH) g_ch = VIZ_MAXH;

    g_zb = ZB_open(RES, RES, ZB_MODE_RGBA, 0);
    if (!g_zb) { win_destroy(g_win); g_win = -1; return -1; }
    glInit(g_zb);
    glShadeModel(GL_SMOOTH);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    // prime the feedback texture to black so the first warp pass is defined
    for (int i = 0; i < RES * RES * 3; i++) g_texrgb[i] = 0;
    glTexImage2D(GL_TEXTURE_2D, 0, 3, RES, RES, 0, GL_RGB, GL_UNSIGNED_BYTE, g_texrgb);
    g_tex_primed = 1;
    gl_setup_2d();

    g_phase = 0.0; g_rot_acc = 0.0; g_beat = 0.0; g_avg = 1.0;
    g_last_pos = -1; g_frames = 0;
    g_open = 1;
    return 0;
}

int  viz_is_open(void) { return g_open; }
int  viz_preset(void)  { return g_preset; }
const char *viz_preset_name(void) { return g_pnames[g_preset]; }

void viz_close(void) {
    if (!g_open) return;
    glClose();
    if (g_zb) { ZB_close(g_zb); g_zb = 0; }
    if (g_win >= 0) { win_destroy(g_win); g_win = -1; }
    g_open = 0;
    g_tex_primed = 0;
}

void viz_toggle(void) { if (g_open) viz_close(); else viz_open(); }
void viz_next_preset(void) { g_preset = (g_preset + 1) % NPRESET; }

void viz_set_audio(const int *bars, int nbars, int playing, long pos_ms) {
    if (nbars > 32) nbars = 32;
    g_nbars = nbars;
    double e = 0.0;
    for (int i = 0; i < nbars; i++) { g_bars[i] = bars[i]; e += bars[i]; }
    g_energy = e;
    g_playing = playing;

    // running-average energy + beat pulse. hifi-vizreact: stronger, punchier
    // response so the visuals CLEARLY move with the audio (was too subtle).
    g_avg = g_avg * 0.92 + e * 0.08;
    double beat = (g_avg > 1.0) ? (e - g_avg) / g_avg : 0.0;
    beat *= 2.4;                                   // amplify the audio->motion map
    if (beat < 0.0) beat = 0.0; if (beat > 2.5) beat = 2.5;
    g_beat = g_beat * 0.55 + beat * 0.45;          // faster attack = a punchy pulse

    // advance the animation phase by the REAL DAC position delta so the visuals
    // track the actual audio; add a small idle drift so it never fully freezes.
    if (pos_ms >= 0 && g_last_pos >= 0 && pos_ms >= g_last_pos) {
        double dms = (double)(pos_ms - g_last_pos);
        if (dms > 200.0) dms = 200.0;      // clamp seeks
        g_phase += dms * 0.0016 * (0.5 + 0.5 * (e / ((nbars ? nbars : 1) * 40.0)));
    }
    g_phase += playing ? 0.02 : 0.004;
    g_last_pos = pos_ms;
}

void viz_frame(void) {
    if (!g_open) return;

    // pump this window's own events (close, preset cycling)
    gui_event_t ev;
    while (win_get_event(g_win, &ev, 0) > 0) {
        if (ev.type == EVENT_WINDOW_CLOSE) { viz_close(); return; }
        if (ev.type == EVENT_RESIZE) {
            int nw = ev.mouse_x, nh = ev.mouse_y;
            if (nw > 0 && nh > 0) {
                if (nw > VIZ_MAXW) nw = VIZ_MAXW;
                if (nh > VIZ_MAXH) nh = VIZ_MAXH;
                g_cw = nw; g_ch = nh;
            } else if (win_get_size(g_win, &g_cw, &g_ch) != 0) { g_cw = VIZ_REQ_W; g_ch = VIZ_REQ_H; }
            if (g_cw > VIZ_MAXW) g_cw = VIZ_MAXW;
            if (g_ch > VIZ_MAXH) g_ch = VIZ_MAXH;
        }
        if (ev.type == EVENT_KEY_DOWN) {
            if (ev.keycode == 0x01) { viz_close(); return; }             // Esc closes
            if (ev.key_char == 'p' || ev.key_char == 'P' ||
                ev.key_char == ' ' || ev.keycode == 0x1C) viz_next_preset();
            if (ev.keycode == 0x4D || ev.key_char == ']') viz_next_preset();
            if (ev.keycode == 0x4B || ev.key_char == '[') g_preset = (g_preset + NPRESET - 1) % NPRESET;
        }
        if (ev.type == EVENT_MOUSE_DOWN) viz_next_preset();              // click = next preset
    }
    if (!g_open) return;

    set_preset_params();
    build_wave();

    // on the very first frame there is nothing fed back yet -> just clear
    if (!g_tex_primed) glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    warp_pass();       // draw previous frame warped + decayed (the feedback)
    overlay_pass();    // draw live waveform + spectrum on top
    capture_feedback();// stash this frame as the texture for next time
    blit_to_window();
    g_frames++;
}
