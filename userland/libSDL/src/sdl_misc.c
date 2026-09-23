/* sdl_misc.c - the small, honestly-deferred corners of the SDL 1.2 API:
 * CPU feature queries, dynamic library loading, joystick, CD-ROM, iconv, and
 * the SDL_syswm introspection call. Part of the MayteraOS SDL 1.2 backend
 * (task #745, docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md Tier 2 #7).
 *
 * "Honestly deferred" means: every one of these reports zero devices / no
 * capability / a real SDL_GetError() string, rather than a fabricated
 * success. A game that checks SDL_NumJoysticks() == 0 and disables gamepad
 * support behaves correctly; a game that got told "1 joystick" and then
 * dereferenced a fake one would not.
 */
#include "sdl_priv.h"
#include <string.h>

/* ---------------------------------------------------------------------
 * CPU feature queries. Userland on MayteraOS is REAL hardware SSE2 (the
 * ABI baseline for x86-64), which is the one fact worth being accurate
 * about; RDTSC/MMX/3DNow/AltiVec are not probed (no CPUID wrapper exposed to
 * userland today) so they report false rather than a guess.
 * ------------------------------------------------------------------- */
SDL_bool SDL_HasRDTSC(void)   { return SDL_FALSE; }
SDL_bool SDL_HasMMX(void)     { return SDL_FALSE; }
SDL_bool SDL_HasMMXExt(void)  { return SDL_FALSE; }
SDL_bool SDL_Has3DNow(void)   { return SDL_FALSE; }
SDL_bool SDL_Has3DNowExt(void){ return SDL_FALSE; }
SDL_bool SDL_HasSSE(void)     { return SDL_TRUE; }
SDL_bool SDL_HasSSE2(void)    { return SDL_TRUE; }
SDL_bool SDL_HasAltiVec(void) { return SDL_FALSE; }

/* ---------------------------------------------------------------------
 * Dynamic library loading: no dlopen()-equivalent syscall exists on
 * MayteraOS (userland/ports/ MPORTS.md's execve gap, 2.3, is the same class
 * of missing OS surface). A game that plugin-loads via SDL_LoadObject is
 * uncommon in the class-E corpus; refuse cleanly.
 * ------------------------------------------------------------------- */
void *SDL_LoadObject(const char *sofile) { (void)sofile; sdlpriv_set_error("Dynamic loading is not supported on MayteraOS"); return 0; }
void *SDL_LoadFunction(void *handle, const char *name) { (void)handle; (void)name; sdlpriv_set_error("Dynamic loading is not supported on MayteraOS"); return 0; }
void SDL_UnloadObject(void *handle) { (void)handle; }

/* ---------------------------------------------------------------------
 * Joystick: zero devices, honestly. No joystick/gamepad driver exists in
 * MayteraOS's input stack yet (PS/2 keyboard+mouse and USB HID keyboard are
 * the input surface today).
 * ------------------------------------------------------------------- */
int SDL_NumJoysticks(void) { return 0; }
const char *SDL_JoystickName(int device_index) { (void)device_index; return 0; }
SDL_Joystick *SDL_JoystickOpen(int device_index) { (void)device_index; sdlpriv_set_error("No joystick devices"); return 0; }
int SDL_JoystickOpened(int device_index) { (void)device_index; return 0; }
int SDL_JoystickIndex(SDL_Joystick *joystick) { (void)joystick; return -1; }
int SDL_JoystickNumAxes(SDL_Joystick *joystick) { (void)joystick; return 0; }
int SDL_JoystickNumBalls(SDL_Joystick *joystick) { (void)joystick; return 0; }
int SDL_JoystickNumHats(SDL_Joystick *joystick) { (void)joystick; return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *joystick) { (void)joystick; return 0; }
void SDL_JoystickUpdate(void) { }
int SDL_JoystickEventState(int state) { return (state == SDL_QUERY) ? SDL_IGNORE : state; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *joystick, int axis) { (void)joystick; (void)axis; return 0; }
Uint8 SDL_JoystickGetHat(SDL_Joystick *joystick, int hat) { (void)joystick; (void)hat; return SDL_HAT_CENTERED; }
int SDL_JoystickGetBall(SDL_Joystick *joystick, int ball, int *dx, int *dy) { (void)joystick; (void)ball; if (dx) *dx = 0; if (dy) *dy = 0; return -1; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *joystick, int button) { (void)joystick; (void)button; return 0; }
void SDL_JoystickClose(SDL_Joystick *joystick) { (void)joystick; }

/* ---------------------------------------------------------------------
 * CD-ROM: zero drives, honestly (no optical-drive driver exists).
 * ------------------------------------------------------------------- */
int SDL_CDNumDrives(void) { return 0; }
const char *SDL_CDName(int drive) { (void)drive; return 0; }
SDL_CD *SDL_CDOpen(int drive) { (void)drive; sdlpriv_set_error("No CD-ROM devices"); return 0; }
CDstatus SDL_CDStatus(SDL_CD *cdrom) { (void)cdrom; return CD_TRAYEMPTY; }
int SDL_CDPlayTracks(SDL_CD *cdrom, int start_track, int start_frame, int ntracks, int nframes) { (void)cdrom; (void)start_track; (void)start_frame; (void)ntracks; (void)nframes; return -1; }
int SDL_CDPlay(SDL_CD *cdrom, int start, int length) { (void)cdrom; (void)start; (void)length; return -1; }
int SDL_CDPause(SDL_CD *cdrom) { (void)cdrom; return -1; }
int SDL_CDResume(SDL_CD *cdrom) { (void)cdrom; return -1; }
int SDL_CDStop(SDL_CD *cdrom) { (void)cdrom; return -1; }
int SDL_CDEject(SDL_CD *cdrom) { (void)cdrom; return -1; }
void SDL_CDClose(SDL_CD *cdrom) { (void)cdrom; }

/* ---------------------------------------------------------------------
 * iconv: no iconv on MayteraOS. SDL_stdinc.h always declares these three
 * (they are only #defined to the real libc when HAVE_ICONV is set, which
 * userland/libSDL/include/SDL/SDL_config.h deliberately does not set), so a
 * real (if minimal) implementation is required for anything that includes
 * SDL_stdinc.h to link. UTF-8-to-UTF-8 (a no-op "conversion", which is also
 * the single most common real call: `SDL_iconv_utf8_locale()` on a UTF-8
 * host) is supported; anything else fails cleanly rather than mis-converting.
 * ------------------------------------------------------------------- */
struct _SDL_iconv_t { int noop; };
static struct _SDL_iconv_t g_iconv_noop = { 1 };
static int names_equal_ci(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}
SDL_iconv_t SDL_iconv_open(const char *tocode, const char *fromcode) {
    if (names_equal_ci(tocode, fromcode) ||
        (names_equal_ci(tocode, "UTF-8") && names_equal_ci(fromcode, "")) ||
        (names_equal_ci(fromcode, "UTF-8") && names_equal_ci(tocode, ""))) {
        return (SDL_iconv_t)&g_iconv_noop;
    }
    sdlpriv_set_error("SDL_iconv_open: only identity/UTF-8 passthrough is supported on MayteraOS");
    return (SDL_iconv_t)-1;
}
int SDL_iconv_close(SDL_iconv_t cd) { (void)cd; return 0; }
size_t SDL_iconv(SDL_iconv_t cd, const char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft) {
    if (cd == (SDL_iconv_t)-1 || cd == 0) return SDL_ICONV_EILSEQ;
    size_t n = (*inbytesleft < *outbytesleft) ? *inbytesleft : *outbytesleft;
    if (n > 0) SDL_memcpy(*outbuf, *inbuf, n);
    *inbuf += n; *inbytesleft -= n;
    *outbuf += n; *outbytesleft -= n;
    return (*inbytesleft == 0) ? 0 : SDL_ICONV_E2BIG;
}
char *SDL_iconv_string(const char *tocode, const char *fromcode, const char *inbuf, size_t inbytesleft) {
    SDL_iconv_t cd = SDL_iconv_open(tocode, fromcode);
    if (cd == (SDL_iconv_t)-1) return 0;
    char *out = (char *)SDL_malloc(inbytesleft + 1);
    if (!out) { SDL_iconv_close(cd); return 0; }
    SDL_memcpy(out, inbuf, inbytesleft);
    out[inbytesleft] = 0;
    SDL_iconv_close(cd);
    return out;
}

/* ---------------------------------------------------------------------
 * SDL_syswm: no native window handle to hand out (MayteraOS window handles
 * are small integers meaningful only via SYS_WIN_*, not a pointer any
 * upstream code could dereference). Report failure rather than a fabricated
 * handle.
 * ------------------------------------------------------------------- */
int SDL_GetWMInfo(SDL_SysWMinfo *info) {
    if (info) info->data = -1;
    sdlpriv_set_error("SDL_GetWMInfo is not supported on MayteraOS");
    return 0;
}
SDL_Window *SDL12COMPAT_GetWindow(void) { return 0; }
