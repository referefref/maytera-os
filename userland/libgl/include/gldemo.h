// gldemo.h - shared 3D demo render cores (spinning textured cube + 3D matrix
// rain) built on TinyGL. Used by the glcube/glmatrix userland apps AND by the
// compositor GL screensavers. One TinyGL context per process (gl_ctx is global),
// so only one gldemo can be active at a time. (#319)
#ifndef _GLDEMO_H
#define _GLDEMO_H
#include <stdint.h>

#define GLDEMO_CUBE   0
#define GLDEMO_MATRIX 1

// (Re)initialize the GL context and the chosen demo at size w x h.
// Returns 1 on success, 0 on failure. Safe to call repeatedly (it tears down
// any previous context first).
int  gldemo_init(int mode, int w, int h);

// Resize the render target (re-inits the GL context, keeps the mode).
void gldemo_resize(int w, int h);

// Render the next animation frame into dst (32-bit ARGB, alpha forced 0xFF),
// where dst has dst_pitch pixels per row. Advances the animation by one step.
void gldemo_frame(uint32_t *dst, int dst_pitch);

// Tear down the GL context.
void gldemo_shutdown(void);

// Current render size (for callers that need it).
int  gldemo_width(void);
int  gldemo_height(void);

#endif
