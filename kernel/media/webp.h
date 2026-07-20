// webp.h - WebP Image Decoder for MayteraOS
// Simplified VP8 lossy and VP8L lossless decoder
//
// Features:
// - VP8 lossy decoding (basic support)
// - VP8L lossless decoding (basic support)
// - Extended WebP format detection (VP8X)
// - Alpha channel support
//
// Limitations:
// - No animation support
// - Limited color transform options in VP8L

#ifndef MEDIA_WEBP_H
#define MEDIA_WEBP_H

#include "../types.h"

// Forward declare image_t
#ifndef IMAGE_H
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} image_t;
#endif

// ============================================================================
// WebP Error Codes
// ============================================================================

#define WEBP_OK                  0
#define WEBP_ERR_NULL_PTR       -1
#define WEBP_ERR_INVALID_SIG    -2
#define WEBP_ERR_UNSUPPORTED    -3
#define WEBP_ERR_NOMEM          -4
#define WEBP_ERR_CORRUPT        -5
#define WEBP_ERR_TOO_SMALL      -6
#define WEBP_ERR_VP8_DECODE     -7
#define WEBP_ERR_VP8L_DECODE    -8
#define WEBP_ERR_HUFFMAN        -9
#define WEBP_ERR_TRANSFORM     -10

// ============================================================================
// WebP Constants
// ============================================================================

// RIFF chunk IDs
#define WEBP_FOURCC_RIFF    0x46464952  // "RIFF"
#define WEBP_FOURCC_WEBP    0x50424557  // "WEBP"
#define WEBP_FOURCC_VP8     0x20385056  // "VP8 "
#define WEBP_FOURCC_VP8L    0x4C385056  // "VP8L"
#define WEBP_FOURCC_VP8X    0x58385056  // "VP8X"
#define WEBP_FOURCC_ALPH    0x48504C41  // "ALPH"
#define WEBP_FOURCC_ANIM    0x4D494E41  // "ANIM"
#define WEBP_FOURCC_ANMF    0x464D4E41  // "ANMF"

// VP8L signature
#define VP8L_SIGNATURE      0x2F

// Maximum dimensions
#define WEBP_MAX_WIDTH      16384
#define WEBP_MAX_HEIGHT     16384

// ============================================================================
// WebP API
// ============================================================================

/**
 * Check if data appears to be a WebP image
 *
 * @param data    Pointer to file data
 * @param size    Size of data in bytes
 * @return        1 if WebP signature matches, 0 otherwise
 */
int webp_is_webp(const void *data, uint32_t size);

/**
 * Load a WebP image from memory
 *
 * @param data    Pointer to WebP file data
 * @param size    Size of the WebP data in bytes
 * @param img     Output image structure
 * @return        WEBP_OK on success, negative error code on failure
 *
 * Supports:
 * - Simple lossy WebP (VP8)
 * - Simple lossless WebP (VP8L)
 * - Extended WebP with alpha (VP8X)
 *
 * Does NOT support:
 * - Animated WebP
 * - ICC profiles
 * - EXIF/XMP metadata processing
 *
 * The resulting pixel buffer uses BGRA format compatible with the framebuffer.
 * Caller is responsible for freeing img->pixels with kfree().
 */
int webp_decode(const void *data, uint32_t size, image_t *img);

/**
 * Get error string for WebP error code
 */
const char *webp_error_string(int err);

/**
 * Get WebP image info without fully decoding
 *
 * @param data    Pointer to WebP data
 * @param size    Size of data
 * @param width   Output width (may be NULL)
 * @param height  Output height (may be NULL)
 * @param lossy   Output: 1 if lossy, 0 if lossless (may be NULL)
 * @param alpha   Output: 1 if has alpha, 0 otherwise (may be NULL)
 * @return        WEBP_OK on success, error code on failure
 */
int webp_get_info(const void *data, uint32_t size,
                  uint32_t *width, uint32_t *height,
                  int *lossy, int *alpha);

#endif // MEDIA_WEBP_H
