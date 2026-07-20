/* gfx.h - Maytera Chess shared app state + small GL math. */
#ifndef MAYTERA_CHESS_GFX_H
#define MAYTERA_CHESS_GFX_H
#include "chess.h"

/* app screens */
enum { SC_MENU = 0, SC_PLAY, SC_PROMOTE, SC_GAMEOVER, SC_SETTINGS, SC_HELP };
/* game mode */
enum { MODE_HOTSEAT = 0, MODE_VS_CPU };

typedef struct { float m[16]; } Mat4;   /* column-major, GL layout */

typedef struct {
    Game    game;
    int     screen;
    int     mode;
    int     difficulty;          /* 0 easy .. 2 hard */
    int     human_color;         /* which side the human plays vs CPU (WHITE) */
    int     sel;                 /* selected square, or -1 */
    int     cursor;              /* keyboard board cursor square 0..63 */
    int     legal_to[64];        /* legal destinations from sel (bitmap-ish: 1 = legal) */
    int     nlegal;
    int     last_from, last_to;  /* last move highlight */
    int     result;              /* RES_* when game over */
    int     flip;                /* board orientation flip (black at bottom) */
    int     show_coords;

    /* pending promotion */
    int     promo_from, promo_to;

    /* move animation */
    int     anim_active;
    int     anim_piece;          /* piece code being animated */
    float   anim_t;              /* 0..1 */
    float   anim_fx, anim_fz, anim_tx, anim_tz;
    Move    anim_move;
    int     anim_apply_pending;

    /* AI worker */
    volatile int ai_thinking;
    volatile int ai_ready;
    Move    ai_move;

    /* camera */
    float   cam_yaw;             /* orbit angle degrees */
    float   cam_pitch;

    int     mouse_x, mouse_y;
    int     vw, vh;              /* current viewport (GL render size) */
} App;

/* render.c */
void gfx_init(int w, int h);
void gfx_resize(int w, int h);
void gfx_render(App *a, unsigned int *dst, int pitch);  /* 3D scene into ARGB dst */
int  gfx_pick_square(App *a, int mx, int my);           /* screen -> square 0..63 or -1 */
int  gfx_project(App *a, float wx, float wy, float wz, int *sx, int *sy); /* world -> screen px */
void gfx_load_assets(void);

/* overlay.c - 2D drawing into an ARGB buffer (pitch = pixels/row) */
void ov_text(unsigned int *buf, int bw, int bh, int x, int y, const char *s, unsigned int col, int scale);
void ov_text_c(unsigned int *buf, int bw, int bh, int cx, int y, const char *s, unsigned int col, int scale);
int  ov_text_w(const char *s, int scale);
void ov_rect(unsigned int *buf, int bw, int bh, int x, int y, int w, int h, unsigned int col);
void ov_rect_a(unsigned int *buf, int bw, int bh, int x, int y, int w, int h, unsigned int col, int alpha);
void ov_frame(unsigned int *buf, int bw, int bh, int x, int y, int w, int h, unsigned int col);
const unsigned int *ov_load_bg(const char *path, int *w, int *h);  /* logo/backdrop BMP loader */

#endif
