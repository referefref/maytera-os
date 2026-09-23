# userland/libSDL - the MayteraOS SDL 1.2 compatibility backend

Task #745, Tier 2 item #7 of
`docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md`: "A real SDL 1.2 API
implemented over our SYS_WIN_* and TinyGL, replacing the two divergent
hand-written sdlshim.cpp files ... Non-trivial: it needs a genuine backend,
not a header." This is that backend.

## What it is

A first-party static library (`libSDL.a`) implementing the real SDL 1.2 API
directly over MayteraOS's compositor window syscalls (`SYS_WIN_*`,
`userland/libc/gui.h`/`syscall.h`) and TinyGL (`userland/libgl`, #319), built
against the real, pinned upstream SDL 1.2 public headers so a game compiles
unmodified against them. It is NOT sdl12-compat's own SDL2-backed
implementation ported here (this OS has no SDL2 and no real desktop GL
driver to build one over) - it is a from-scratch implementation of the SDL
1.2 ABI, using sdl12-compat's own header release as the source of those
headers (see `ATTRIBUTION.md` for the exact pin and licence).

## Layout

```
userland/libSDL/
  COPYING              sdl12-compat's Zlib licence text (covers include/SDL/*.h)
  include/SDL/*.h       pinned upstream headers (release-1.2.76), unmodified
                         except SDL_config.h and SDL_opengl.h (MayteraOS originals,
                         see the comment at the top of each)
  src/sdl_priv.h         internal shared state, not installed, not part of the API
  src/sdl_init.c         SDL_Init family, error string, SDL_Linked_Version
  src/sdl_video.c        SDL_SetVideoMode, surfaces, the software blit engine,
                         palette, GL context/attributes, window manager calls
  src/sdl_events.c       event pump, keyboard state, mouse state, cursor,
                         the GUI_KEY_* -> SDLKey table
  src/sdl_rwops.c        SDL_RWops (file/memory), endian read/write helpers
  src/sdl_timer.c        SDL_GetTicks/Delay/SetTimer/AddTimer/RemoveTimer
  src/sdl_thread.c        SDL_Thread/mutex/sem/cond over userland/libc/pthread.h
  src/sdl_audio.c        SDL_OpenAudio and friends over the SYS_AUDIO_PCM_* trio
  src/sdl_misc.c          cpuinfo, loadso, joystick, cdrom, iconv, SDL_syswm
  Makefile               same compile profile as userland/libgl (see its comment)
```

## What is covered (a real implementation, not a stub)

- `SDL_Init`/`SDL_Quit` family, error reporting, version query.
- `SDL_SetVideoMode` for both a software surface (`SDL_SWSURFACE`) and
  `SDL_OPENGL` (a real TinyGL context). One window, matching SDL 1.2's own
  single-window design and the compositor's per-app window model.
- Surfaces: `SDL_CreateRGBSurface[From]`, `SDL_FreeSurface`,
  `SDL_LockSurface`/`Unlock` (no HW surfaces, always trivially safe),
  `SDL_SetColorKey`/`SetAlpha`/`SetClipRect`, a real software blit engine
  (`SDL_UpperBlit`/`LowerBlit`, colorkey and per-surface alpha, any source
  depth (1/2/3/4 bytes, indexed or truecolor) to any destination depth via
  `SDL_GetRGBA`/`MapRGBA`), `SDL_FillRect`, `SDL_ConvertSurface`,
  `SDL_DisplayFormat[Alpha]`, `SDL_SoftStretch` (nearest-neighbour).
- 8bpp indexed surfaces with a real palette (`SDL_SetColors`/`SetPalette`,
  a default grayscale ramp on create, matching real SDL), plus 16/24/32bpp
  truecolor.
- `SDL_LoadBMP_RW` (uncompressed 8/24/32bpp `BITMAPINFOHEADER`, a small
  self-contained reader; RLE and BITMAPV4/V5 refused with a clear error, not
  a misread).
- The full event pump: `SDL_PollEvent`/`WaitEvent`/`PushEvent`/`PeepEvents`,
  translated from the compositor's `gui_event_t` via
  `userland/libc/gui_mods.h`'s `gui_mods_next_event()` (the shared, already-
  correct modifier tracker - not reinvented here), keyboard state
  (`SDL_GetKeyState`/`GetModState`), mouse state and the classic SDL 1.2
  wheel-as-button-4/5 convention.
- Real threads/mutexes/semaphores/condition variables over
  `userland/libc/pthread.h` (a genuine implementation, not the C++ bridge
  the two pre-existing `sdlshim.cpp` files needed to avoid a direct libc
  include - this backend is plain C, so there is no bridge to reinvent).
- Real audio (`SDL_OpenAudio`/`PauseAudio`/`CloseAudio`/`LockAudio`,
  `SDL_LoadWAV_RW` for uncompressed PCM WAV, `SDL_MixAudio`) over the real
  `SYS_AUDIO_PCM_OPEN/WRITE/CLOSE` trio, via a genuine callback-pump thread.
  Inherits the platform's real limits rather than hiding them: ONE PCM
  stream system-wide (`kernel/drivers/audio_pcm.c: PCM_MAX_STREAMS == 1`),
  fixed S16LE device format (other requested formats are converted in the
  pump), no resampling. See `src/sdl_audio.c`'s header comment.

## What is deliberately deferred, and why

- **`SDL_AudioCVT`/`SDL_ConvertAudio`** (format/rate conversion): refused
  with a clear error rather than a silent no-op. A real resampling filter
  chain is a separate, sizeable piece of work; every game that opens audio
  at a format/rate the device already provides works today.
- **YUV overlays** (`SDL_CreateYUVOverlay` and friends): `SDL_SetError` and
  a clean failure. No class-E SDL 1.2 game in the assessment's corpus is
  expected to need video overlay playback; this is the one part of the
  public API this backend does not implement at all.
- **CD-ROM and joystick**: zero devices, honestly (no such drivers exist in
  MayteraOS's input/storage stack today). A game that checks
  `SDL_NumJoysticks() == 0` behaves correctly.
- **`SDL_WM_GrabInput`/custom cursors/`SDL_WarpMouse`**: tracked but not
  enforced - `grab_input()`-class syscalls are compositor-only
  (`kernel/gui/fb_syscall.c`'s `is_compositor()` gate), the exact same
  documented limitation the two pre-existing `sdlshim.cpp` files carry for
  SDL2. Relative-look games still work via frame-to-frame deltas of
  absolute mouse-move events, the same technique `userland/apps/arena`'s
  real, shipping mouselook already uses.
- **Gamma control, `SDL_SaveBMP`, dynamic library loading (`SDL_LoadObject`)**:
  refused with a clear error (no gamma ramp, no BMP writer, no
  `dlopen()`-equivalent syscall on this OS).
- **Window title changes after creation**: `SDL_WM_SetCaption` is tracked,
  but there is no `SYS_WIN_SET_TITLE` syscall, so a caption set after the
  window already exists cannot be pushed to the compositor's title bar live.

## Why it replaces neither pre-existing sdlshim.cpp (yet)

`userland/apps/openarena/sdlshim.cpp` and
`userland/apps/assaultcube/sdlshim.cpp` are shims for **SDL2's** API
(`SDL_CreateWindow`, `SDL_GL_CreateContext`, multi-window bookkeeping), not
SDL 1.2's (`SDL_SetVideoMode`, one window). They are a different API surface
entirely: this backend cannot be dropped in to replace them without either
porting them to the SDL 1.2 API first (a real, separate migration for two
large third-party C++ engines) or building a small SDL2-compatibility shim
of its own on top of this SDL 1.2 backend. The doc's framing ("replacing the
two divergent hand-written sdlshim.cpp files") describes the destination for
the SDL-1.2-shaped part of the class-E corpus (the ~101 candidates named in
the assessment); OpenArena and AssaultCube are not part of that corpus, they
are the two existing SDL2 ports the doc cites as the reason a *shared*
backend is worth building at all. See the top-level task report for the
proof-of-life used instead (`userland/apps/sdl12test`).

## Verified

`userland/apps/sdl12test` (see its own header comment) compiles clean
against the pinned headers, links clean against `libSDL.a` + `libgl.a` +
`libc.a` with zero undefined symbols, and was run on a real VM: a real
compositor window, `SDL_FillRect`/`SDL_BlitSurface`/`SDL_Flip` animating a
sprite and a progress bar (two screendumps, ~20s apart, visibly different
sprite position and bar fill), a live `SDL_SetVideoMode(..., SDL_OPENGL)`
switch to a real rotating, Gouraud-shaded TinyGL triangle via
`SDL_GL_SwapBuffers` (two screendumps at different rotation angles), and
real keyboard input changing the running program's state live
(`SDLK_UP`/`SDLK_LEFT` visibly changing the window's background colour
while the sprite kept animating underneath).
