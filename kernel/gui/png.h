// png.h - PNG image decoder for MayteraOS
#ifndef PNG_H
#define PNG_H

#include "../types.h"
#include "image.h"

// PNG error codes
#define PNG_SUCCESS             0
#define PNG_ERR_NULL_PTR       -1
#define PNG_ERR_INVALID_SIG    -2
#define PNG_ERR_UNSUPPORTED    -3
#define PNG_ERR_NOMEM          -4
#define PNG_ERR_CORRUPT        -5
#define PNG_ERR_TOO_SMALL      -6
#define PNG_ERR_CRC            -7
#define PNG_ERR_INFLATE        -8

// PNG signature (8 bytes)
#define PNG_SIGNATURE_SIZE     8

// PNG color types
#define PNG_COLOR_GRAYSCALE    0
#define PNG_COLOR_RGB          2
#define PNG_COLOR_INDEXED      3
#define PNG_COLOR_GRAYSCALE_A  4
#define PNG_COLOR_RGBA         6

// PNG filter types
#define PNG_FILTER_NONE        0
#define PNG_FILTER_SUB         1
#define PNG_FILTER_UP          2
#define PNG_FILTER_AVERAGE     3
#define PNG_FILTER_PAETH       4

// #404 Phase V: pure IHDR-parse seam output. Fixed #[repr(C)] layout shared with
// the Rust port png_parse_ihdr_rs (rustkern.rs); sizeof is locked by a
// _Static_assert in gui/png.c so the C struct and the Rust #[repr(C)] can never
// drift. All fields uint32 (natural 4-byte alignment, no padding).
typedef struct {
    uint32_t width;        // IHDR width  (read_be32, NOT range-checked - matches C)
    uint32_t height;       // IHDR height (read_be32, NOT range-checked - matches C)
    uint32_t bit_depth;    // must be 8
    uint32_t color_type;   // 0/2/4/6 (grayscale/RGB/gray+A/RGBA)
    uint32_t interlace;    // must be 0
    uint32_t bpp;          // bytes per pixel derived from color_type (1/2/3/4)
    uint32_t scanline_len; // width*bpp   (#500: checked u64 in BOTH _c and _rs)
    uint32_t raw_size;     // (scanline_len+1)*height (#500: checked u64 in BOTH)
} PngInfo;

// #404 Phase V pure decode seams (png.c). png_parse_ihdr validates the 13-byte
// IHDR payload and computes bpp/scanline_len/raw_size. png_defilter reconstructs
// the None/Sub/Up/Average/Paeth scanlines of the INFLATED buffer into a caller-
// BOUNDED output buffer. Both route to the Rust port under -DRUST_PNG.
int png_parse_ihdr(const uint8_t *ihdr, uint32_t len, PngInfo *out);
int png_defilter(const uint8_t *inflated, uint32_t inflated_len,
                 uint32_t width, uint32_t height, uint32_t bpp,
                 uint8_t *out, uint32_t out_cap);

/**
 * Load a PNG image from memory
 *
 * @param data    Pointer to PNG file data
 * @param size    Size of the PNG data in bytes
 * @param img     Output image structure
 * @return        PNG_SUCCESS on success, negative error code on failure
 *
 * Supports:
 * - 8-bit RGB and RGBA images
 * - 8-bit grayscale and grayscale+alpha
 * - Non-interlaced images only
 *
 * The resulting pixel buffer uses BGRA format compatible with the framebuffer.
 */
int image_load_png(const void *data, uint32_t size, image_t *img);

/**
 * Check if data is a PNG image
 *
 * @param data    Pointer to file data
 * @param size    Size of data
 * @return        1 if PNG, 0 otherwise
 */
int image_is_png(const void *data, uint32_t size);

/**
 * Get PNG error string
 */
const char *png_error_string(int err);

#endif // PNG_H
