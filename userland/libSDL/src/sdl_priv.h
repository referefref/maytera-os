/* sdl_priv.h - private internal state shared between the libSDL translation
 * units. Not installed; not part of the public SDL 1.2 API.
 *
 * docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7 (task #745):
 * a real SDL 1.2 backend over MayteraOS's SYS_WIN_* + TinyGL, replacing the
 * two divergent hand-written sdlshim.cpp files with one shared library.
 *
 * No em-dashes (repo writing-style rule).
 */
#ifndef MAYTERA_SDL_PRIV_H
#define MAYTERA_SDL_PRIV_H

#include "SDL.h"
#include "SDL_syswm.h"

/* MayteraOS platform headers. gui.h pulls in keys.h and gui_mods.h. */
#include "syscall.h"
#include "gui.h"

#define SDL_MAX_W 3840
#define SDL_MAX_H 2160

/* ---------------------------------------------------------------------
 * Video/window state (sdl_video.c, sdl_events.c). SDL 1.2 is single-window
 * by design, which is exactly the compositor's own per-app window model, so
 * there is no multi-window bookkeeping to invent here.
 * ------------------------------------------------------------------- */
typedef struct {
    int          win_handle;      /* compositor window handle, -1 = none */
    int          have_window;
    int          mode_w, mode_h;  /* the SDL_SetVideoMode() resolution. This
                                    * is the INTERNAL render resolution; the
                                    * compositor's SYS_WIN_BLIT scales it to
                                    * whatever the real window content size
                                    * is (kernel/proc/syscall.c sys_win_blit /
                                    * winblit_plan_rs), so games that pick a
                                    * classic low resolution (320x200,
                                    * 640x480) are upscaled for free. */
    int          mode_bpp;
    Uint32       mode_flags;
    int          is_gl;
    void        *zb;              /* ZBuffer*, GL path only */
    Uint32      *present_buf;     /* mode_w*mode_h ARGB8888, always used to
                                    * publish to the compositor regardless of
                                    * the screen surface's real bit depth. */
    SDL_Surface *screen;
    char         caption[128];
    char         icon_caption[128];
    SDL_GrabMode grab_mode;
    int          cursor_shown;
    float        gl_attrs_stub;   /* unused, keeps the struct non-empty if
                                    * every other field above is compiled out
                                    * by a future ifdef; harmless padding. */
} sdl_video_state_t;

extern sdl_video_state_t g_sdlv;

/* GL attribute store: SDL_GLattr has 17 members (SDL_GL_RED_SIZE ..
 * SDL_GL_SWAP_CONTROL). */
#define SDL_GL_ATTR_COUNT 17
extern int g_gl_attrs[SDL_GL_ATTR_COUNT];

/* ---------------------------------------------------------------------
 * Mouse tracking (sdl_events.c), read by sdl_video.c's SDL_GetMouseState.
 * ------------------------------------------------------------------- */
typedef struct {
    int    x, y;
    int    rel_x, rel_y;   /* accumulated since the last GetRelativeMouseState */
    int    have_pos;
    Uint8  buttons;        /* SDL_BUTTON(n) bits */
} sdl_mouse_state_t;
extern sdl_mouse_state_t g_sdlmouse;

/* Keyboard state array, indexed by SDLKey (SDLK_LAST entries). */
extern Uint8 g_sdlkeys[SDLK_LAST];

/* ---------------------------------------------------------------------
 * sdl_keymap.c (in sdl_events.c): GUI_KEY_* -> (SDLKey, unicode) mapping.
 * ------------------------------------------------------------------- */
void sdlpriv_map_key(unsigned int gui_keycode, char key_char,
                      SDLKey *out_sym, Uint16 *out_unicode);

/* ---------------------------------------------------------------------
 * Error string (sdl_init.c).
 * ------------------------------------------------------------------- */
void sdlpriv_set_error(const char *fmt, ...);

/* ---------------------------------------------------------------------
 * Video-mode present helper (sdl_video.c): converts g_sdlv.screen's real
 * pixels (whatever bpp it is) into g_sdlv.present_buf (always ARGB8888,
 * alpha forced opaque) and pushes it to the compositor. Shared by SDL_Flip
 * and SDL_UpdateRect(s), which (see sys_win_blit()) can only ever publish
 * the WHOLE current buffer: the kernel blit syscall has no partial x/y
 * offset (its x,y parameters are accepted but unused,
 * kernel/proc/syscall.c sys_win_blit()), so a "partial" UpdateRect and a
 * full SDL_Flip do the same work here. Documented, not hidden.
 * ------------------------------------------------------------------- */
void sdlpriv_present(void);

/* Software-surface blit engine shared by SDL_UpperBlit/SDL_LowerBlit/
 * SDL_ConvertSurface/SDL_DisplayFormat (sdl_video.c). */
int sdlpriv_blit(SDL_Surface *src, SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);

#endif /* MAYTERA_SDL_PRIV_H */
