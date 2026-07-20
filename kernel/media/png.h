// png.h - PNG Image Decoder for MayteraOS
// Enhanced PNG decoder with Adam7 interlacing and 16-bit support
//
// Features:
// - 8-bit and 16-bit RGB/RGBA
// - 8-bit and 16-bit Grayscale with optional alpha
// - Indexed color (palette-based)
// - All PNG filter types (None, Sub, Up, Average, Paeth)
// - Adam7 interlacing support
// - CRC validation
// - zlib/DEFLATE decompression

#ifndef MEDIA_PNG_H
#define MEDIA_PNG_H

#include "../types.h"
#include "image_decode.h"

// Forward declare image_t if not including image.h

// ============================================================================
// PNG Error Codes
// ============================================================================

#define PNG_OK                   0
#define PNG_ERR_NULL_PTR        -1
#define PNG_ERR_INVALID_SIG     -2
#define PNG_ERR_UNSUPPORTED     -3
#define PNG_ERR_NOMEM           -4
#define PNG_ERR_CORRUPT         -5
#define PNG_ERR_TOO_SMALL       -6
#define PNG_ERR_CRC             -7
#define PNG_ERR_INFLATE         -8
#define PNG_ERR_INVALID_IHDR    -9
#define PNG_ERR_MISSING_PLTE   -10
#define PNG_ERR_CHUNK_SIZE     -11

// ============================================================================
// PNG Constants
// ============================================================================

// PNG signature bytes
#define PNG_SIGNATURE_SIZE      8

// PNG color types
#define PNG_COLOR_GRAYSCALE     0   // Grayscale
#define PNG_COLOR_RGB           2   // Truecolor RGB
#define PNG_COLOR_INDEXED       3   // Indexed-color (palette)
#define PNG_COLOR_GRAYSCALE_A   4   // Grayscale with alpha
#define PNG_COLOR_RGBA          6   // Truecolor RGBA

// PNG filter types
#define PNG_FILTER_NONE         0
#define PNG_FILTER_SUB          1
#define PNG_FILTER_UP           2
#define PNG_FILTER_AVERAGE      3
#define PNG_FILTER_PAETH        4

// PNG interlace methods
#define PNG_INTERLACE_NONE      0
#define PNG_INTERLACE_ADAM7     1

// Maximum dimensions
#define PNG_MAX_WIDTH       16384
#define PNG_MAX_HEIGHT      16384
#define PNG_MAX_PALETTE       256

// ============================================================================
// PNG Structures
// ============================================================================

// IHDR chunk data
typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t  bit_depth;      // 1, 2, 4, 8, or 16
    uint8_t  color_type;     // PNG_COLOR_*
    uint8_t  compression;    // Always 0
    uint8_t  filter;         // Always 0
    uint8_t  interlace;      // PNG_INTERLACE_*
} png_ihdr_t;

// PNG decoder context
typedef struct {
    // Image header info
    png_ihdr_t ihdr;

    // Palette for indexed color
    uint8_t palette[PNG_MAX_PALETTE][3];
    uint8_t palette_alpha[PNG_MAX_PALETTE];
    int palette_count;
    int has_trns;

    // Transparency for non-indexed
    uint16_t trns_gray;
    uint16_t trns_rgb[3];

    // Background color
    uint8_t background[4];
    int has_background;

    // Gamma
    uint32_t gamma;
    int has_gamma;

    // Input data
    const uint8_t *data;
    uint32_t size;

    // Compressed IDAT data buffer
    uint8_t *compressed;
    uint32_t compressed_len;
    uint32_t compressed_cap;

    // Decompressed data
    uint8_t *raw;
    uint32_t raw_len;

    // Output pixels
    uint32_t *pixels;
} png_decoder_t;

// ============================================================================
// PNG API
// ============================================================================

/**
 * Check if data appears to be a PNG image
 *
 * @param data    Pointer to file data
 * @param size    Size of data in bytes
 * @return        1 if PNG signature matches, 0 otherwise
 */
int png_is_png(const void *data, uint32_t size);

/**
 * Load a PNG image from memory
 *
 * @param data    Pointer to PNG file data
 * @param size    Size of the PNG data in bytes
 * @param img     Output image structure
 * @return        PNG_OK on success, negative error code on failure
 *
 * Supports:
 * - 8-bit and 16-bit per channel
 * - Grayscale, RGB, RGBA, Grayscale+Alpha, Indexed color
 * - Adam7 interlacing
 * - All PNG filter types
 *
 * The resulting pixel buffer uses BGRA format compatible with the framebuffer.
 * Caller is responsible for freeing img->pixels with kfree().
 */
int png_decode(const void *data, uint32_t size, image_t *img);

/**
 * Get error string for PNG error code
 */
const char *png_error_string(int err);

/**
 * Get PNG image info without fully decoding
 *
 * @param data    Pointer to PNG data
 * @param size    Size of data
 * @param width   Output width (may be NULL)
 * @param height  Output height (may be NULL)
 * @param depth   Output bit depth (may be NULL)
 * @param color   Output color type (may be NULL)
 * @return        PNG_OK on success, error code on failure
 */
int png_get_info(const void *data, uint32_t size,
                 uint32_t *width, uint32_t *height,
                 uint8_t *depth, uint8_t *color);

// ============================================================================
// Internal Functions (exported for advanced use)
// ============================================================================

// zlib/DEFLATE decompression
int png_inflate(const uint8_t *src, uint32_t src_len,
                uint8_t *dst, uint32_t dst_cap, uint32_t *dst_len);

// CRC32 calculation
uint32_t png_crc32(const uint8_t *data, uint32_t len);

// Paeth predictor
uint8_t png_paeth(int a, int b, int c);

#endif // MEDIA_PNG_H
