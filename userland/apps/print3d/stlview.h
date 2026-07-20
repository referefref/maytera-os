// stlview.h - STL model loader + TinyGL 3D preview for the 3D Print app (#396).
//
// A compact binary/ASCII STL loader plus a lit, auto-fitting 3D renderer that
// draws the model on a print bed into an offscreen ARGB buffer. The 3D Print
// app blits that buffer into its preview panel so the CuraEngine pipeline has a
// visible model to slice. Uses the shared libgl (TinyGL), exactly like the
// chess app, and never allocates per-frame (the ZBuffer is cached).
#ifndef PRINT3D_STLVIEW_H
#define PRINT3D_STLVIEW_H

// Load an STL (binary or ASCII) into the module's triangle store, replacing any
// previously loaded model. Returns the number of triangles loaded (> 0 on
// success), or <= 0 on failure (missing file, not an STL, out of memory).
int  stl_load(const char *path);

// Number of triangles in the currently loaded model (0 if none).
int  stl_count(void);

// Release the loaded model (triangles) and the cached GL framebuffer.
void stl_free(void);

// Render the loaded model into a w*h ARGB buffer (pitch == w pixels) using an
// isometric-ish camera orbited to `yaw` degrees. Safe to call every frame; the
// ZBuffer is (re)opened only when the size changes. If no model is loaded the
// buffer is filled with the neutral panel background.
void stl_render(unsigned int *dst, int w, int h, float yaw);

#endif
