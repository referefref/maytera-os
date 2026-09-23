// imgenc.h - small image WRITERS shared by the Image Converter (imgconv) and
// Sprite Studio (sprite). Both apps need "take an ARGB buffer, write a BMP or
// a PNG file", and the tree had no shared encoder outside apps/paint/imgio.c,
// which is welded to Studio's document model. This file is the pure-function
// half of that: no document, no UI, no globals.
//
// HOME. The right home for this is userland/libc (next to the kernel-backed
// decode_image()), which the mediaapps batch was not permitted to touch. It
// lives in apps/imgconv and apps/sprite compiles it BY PATH (../imgconv/
// imgenc.c) rather than carrying a second copy, so there is exactly one
// implementation to promote when libc takes it.
//
// Pixel format everywhere: 0xAARRGGBB in a uint32_t, i.e. the framebuffer /
// SYS_DECODE_IMAGE byte order (B,G,R,A in memory). `stride` is in PIXELS.
#ifndef IMGENC_H
#define IMGENC_H

#include "../../libc/types.h"

// Write every byte or fail. Returns 0 on success, -1 on a short/failed write.
int imgenc_write_all(int fd, const void *buf, long n);

// Read a whole file into a malloc'd buffer (caller frees). Refuses files at or
// past `cap` bytes (returns -2) so a huge file cannot exhaust the heap; -1 on
// open/read failure. On success returns the byte count and sets *out.
long imgenc_read_file(const char *path, unsigned char **out, long cap);

// 24-bit bottom-up BI_RGB Windows BMP (the format every MayteraOS reader,
// including the kernel wallpaper loader, opens). Alpha is ignored; callers
// wanting a key colour flatten first. Returns 0 on success, -1 on failure.
int imgenc_write_bmp24(const char *path, const uint32_t *px, int w, int h,
                       int stride);

// PNG, 8-bit RGB (with_alpha == 0, colour type 2) or RGBA (with_alpha != 0,
// colour type 6), filter 0 on every row, zlib STORED blocks. Stored deflate
// is a valid zlib stream that any decoder (the kernel's included) reads; it
// trades file size for a dependency-free encoder with no libm and no tables
// beyond CRC32. Returns 0 on success, -1 on failure (bad args, no memory,
// write error).
int imgenc_write_png(const char *path, const uint32_t *px, int w, int h,
                     int stride, int with_alpha);

// Byte size the PNG writer will produce for w x h (to size progress text and
// to refuse absurd outputs before allocating).
long imgenc_png_size(int w, int h, int with_alpha);

#endif // IMGENC_H
