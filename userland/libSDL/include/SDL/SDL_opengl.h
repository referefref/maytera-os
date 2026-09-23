/*
  SDL_opengl.h - MayteraOS redirect (docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md
  Tier 2 #7, task #745).

  Upstream's SDL_opengl.h is 7000+ lines of the full Khronos GL/GLext headers,
  written for a real desktop GL driver. MayteraOS has no such driver: 3D goes
  through the ported TinyGL software rasterizer (userland/libgl, #319), which
  implements a GL 1.x subset and nothing from GL 1.2 onward. Shipping the real
  header would let a game reference an entry point (a GL 2.x/3.x call, a
  vendor extension) that compiles clean and then fails to link, or worse,
  resolves to nothing and silently no-ops.

  So: this file is deliberately NOT a copy of upstream's. It is a thin
  redirect to the exact header every other TinyGL-backed MayteraOS app already
  builds against (userland/libgl/include/GL/gl.h), so "what a game can call"
  and "what TinyGL implements" are the same list by construction, and a
  missing entry point is a compile error here instead of a link or runtime
  surprise. A consumer's Makefile adds -I<libSDL>/../libgl/include exactly as
  glcube's and arena's already do.

  This is a deliberate scope simplification, stated as one, not a port of
  upstream content: it carries no upstream copyright text because it is not
  upstream text.
*/
#ifndef _SDL_opengl_h
#define _SDL_opengl_h

#include "GL/gl.h"

#endif /* _SDL_opengl_h */
