/* sdl_init.c - SDL_Init family, error reporting, version. Part of the
 * MayteraOS SDL 1.2 backend (task #745,
 * docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7). */
#include "sdl_priv.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* g_sdlv/g_gl_attrs defined in sdl_video.c; g_sdlmouse/g_sdlkeys in
 * sdl_events.c. Declared extern in sdl_priv.h. */

static Uint32 g_init_flags = 0;
static char g_error_buf[256] = "";

void sdlpriv_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error_buf, sizeof(g_error_buf), fmt, ap);
    va_end(ap);
}

char *SDL_GetError(void) { return g_error_buf; }
void SDL_ClearError(void) { g_error_buf[0] = 0; }
void SDL_SetError(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error_buf, sizeof(g_error_buf), fmt, ap);
    va_end(ap);
}
void SDL_Error(SDL_errorcode code) {
    switch (code) {
    case SDL_ENOMEM:    sdlpriv_set_error("Out of memory"); break;
    case SDL_EFREAD:    sdlpriv_set_error("Error reading from datastream"); break;
    case SDL_EFWRITE:   sdlpriv_set_error("Error writing to datastream"); break;
    case SDL_EFSEEK:    sdlpriv_set_error("Error seeking in datastream"); break;
    case SDL_UNSUPPORTED: sdlpriv_set_error("That operation is not supported"); break;
    default:            sdlpriv_set_error("Unknown SDL error"); break;
    }
}

int SDL_InitSubSystem(Uint32 flags) {
    /* Nothing to actually spin up per-subsystem: video is the compositor
     * (always present), timer is SYS_UPTIME_MS (always present), audio opens
     * lazily in SDL_OpenAudio, joystick/cdrom have zero devices (see
     * sdl_misc.c) so there is nothing to initialise for them either. This
     * mirrors real SDL1.2's own behaviour for platforms with a fixed set of
     * backends: the flags are bookkeeping for SDL_WasInit, not a dispatch
     * table. */
    g_init_flags |= flags;
    return 0;
}
int SDL_Init(Uint32 flags) { return SDL_InitSubSystem(flags); }
void SDL_QuitSubSystem(Uint32 flags) { g_init_flags &= ~flags; }
Uint32 SDL_WasInit(Uint32 flags) { return flags ? (g_init_flags & flags) : g_init_flags; }

void SDL_Quit(void) {
    extern void SDL_VideoQuit(void);
    extern void SDL_CloseAudio(void);
    SDL_CloseAudio();
    SDL_VideoQuit();
    g_init_flags = 0;
}

const SDL_version *SDL_Linked_Version(void) {
    static SDL_version v;
    v.major = SDL_MAJOR_VERSION;
    v.minor = SDL_MINOR_VERSION;
    v.patch = SDL_PATCHLEVEL;
    return &v;
}
