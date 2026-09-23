// gldemo.h - shared 3D demo render cores (spinning textured cube + 3D matrix
// rain, plus ten psychedelic/geometric screensaver cores added in #560) built
// on TinyGL. Used by the glcube/glmatrix userland apps AND by the compositor
// GL screensavers. One TinyGL context per process (gl_ctx is global), so only
// one gldemo can be active at a time. (#319, extended #560)
#ifndef _GLDEMO_H
#define _GLDEMO_H
#include <stdint.h>

#define GLDEMO_CUBE        0
#define GLDEMO_MATRIX      1
// --- #560: ten psychedelic/geometric screensaver cores ---
#define GLDEMO_TUNNEL      2   // rainbow ring tunnel flythrough
#define GLDEMO_KALEIDO     3   // mirrored rotating kaleidoscope, 8-fold symmetry
#define GLDEMO_PLATONIC    4   // tetra/cube/octa/icosahedron morph cycle
#define GLDEMO_LORENZ      5   // Lorenz strange attractor, rainbow trail
#define GLDEMO_MOBIUS      6   // twisting Mobius strip, hue along its length
#define GLDEMO_WAVEMESH    7   // grid deformed by interfering sine waves
#define GLDEMO_SPIROGRAPH  8   // 3D harmonograph / epitrochoid sweep
#define GLDEMO_HYPERCUBE   9   // rotating tesseract (4D->3D->2D projection)
#define GLDEMO_VORTEX      10  // swirling particle vortex, point sprites
#define GLDEMO_LAVA        11  // drifting alpha-blended low-poly lava blobs
#define GLDEMO_LAVALAMP    12  // proper 3D lava lamp: metaball wax, glass vessel (lavalamp)
#define GLDEMO_AQUARIUM    13  // 3D reef aquarium: fish species, boids school, kelp (aquarium)

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

// (aquarium) GLDEMO_AQUARIUM runtime options. Set with
// gldemo_aquarium_option(opt, val); the value is clamped and applied (a
// structural change, e.g. population, reseeds the scene on the next frame)
// and the applied value is returned, or -1 for an unknown opt. Safe to call
// before or after gldemo_init(). The screensaver path uses the defaults;
// the aquarium app maps keyboard keys onto these.
#define AQOPT_POPULATION 0   // 0 sparse / 1 normal / 2 busy
#define AQOPT_SPECIES    1   // bitmask: 1 clown, 2 tang, 4 angel, 8 goby, 16 school
#define AQOPT_TINT       2   // 0 clear blue / 1 lagoon green / 2 deep ocean / 3 sunset
#define AQOPT_LIGHT      3   // 0 dim / 1 normal / 2 bright
#define AQOPT_CAUSTICS   4   // 0/1: sand caustic shimmer + god rays
#define AQOPT_BUBBLES    5   // 0 off / 1 normal / 2 dense
#define AQOPT_CAMERA     6   // 0 slow orbit / 1 fixed / 2 drifting glide
#define AQOPT_PLANTS     7   // 0 few / 1 normal / 2 lush
#define AQOPT_QUALITY    8   // 0 performance / 1 quality
int gldemo_aquarium_option(int opt, int val);

#endif
