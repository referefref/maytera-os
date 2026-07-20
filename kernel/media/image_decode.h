// image_decode.h - Unified Image Decoder API for MayteraOS
//
// Provides a unified interface for decoding various image formats:
// - BMP (Windows Bitmap)
// - PNG (Portable Network Graphics)
// - JPEG (Joint Photographic Experts Group)
// - WebP (Google WebP)
//
// Usage:
//   image_t img;
//   int ret = image_decode(file_data, file_size, &img);
//   if (ret == IMAGE_OK) {
//       // Use img.pixels, img.width, img.height
//       image_free(&img);
//   }

#ifndef MEDIA_IMAGE_DECODE_H
#define MEDIA_IMAGE_DECODE_H

#include "../types.h"

// ============================================================================
// Image Structure
// ============================================================================

typedef struct {
    uint32_t width;      // Image width in pixels
    uint32_t height;     // Image height in pixels
    uint32_t *pixels;    // Pixel data in BGRA format (compatible with framebuffer)
                         // Format: 0xAARRGGBB
} image_t;

// ============================================================================
// Error Codes
// ============================================================================

#define IMAGE_OK                 0
#define IMAGE_ERR_NULL_PTR      -1
#define IMAGE_ERR_INVALID_SIG   -2
#define IMAGE_ERR_UNSUPPORTED   -3
#define IMAGE_ERR_NOMEM         -4
#define IMAGE_ERR_CORRUPT       -5
#define IMAGE_ERR_TOO_SMALL     -6
#define IMAGE_ERR_DECODE        -7

// ============================================================================
// Image Format Detection
// ============================================================================

typedef enum {
    IMAGE_FORMAT_UNKNOWN = 0,
    IMAGE_FORMAT_BMP,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_JPEG,
    IMAGE_FORMAT_WEBP,
    IMAGE_FORMAT_GIF,   // Future
    IMAGE_FORMAT_TIFF,  // Future
    IMAGE_FORMAT_ICO,   // Future
} image_format_t;

/**
 * Detect image format from data
 *
 * @param data    Pointer to file data
 * @param size    Size of data in bytes
 * @return        Detected format, or IMAGE_FORMAT_UNKNOWN
 */
image_format_t image_detect_format(const void *data, uint32_t size);

/**
 * Get human-readable name for image format
 */
const char *image_format_name(image_format_t format);

/**
 * Get file extension for image format
 */
const char *image_format_extension(image_format_t format);

// ============================================================================
// Image Decoding API
// ============================================================================

/**
 * Decode an image from memory (auto-detect format)
 *
 * @param data    Pointer to image file data
 * @param size    Size of data in bytes
 * @param img     Output image structure
 * @return        IMAGE_OK on success, negative error code on failure
 *
 * The resulting pixel buffer uses BGRA format.
 * Caller is responsible for freeing with image_free().
 */
int image_decode(const void *data, uint32_t size, image_t *img);

/**
 * Decode an image with explicit format specification
 *
 * @param data    Pointer to image file data
 * @param size    Size of data in bytes
 * @param format  Image format to decode as
 * @param img     Output image structure
 * @return        IMAGE_OK on success, negative error code on failure
 */
int image_decode_format(const void *data, uint32_t size,
                        image_format_t format, image_t *img);

/**
 * Get image info without fully decoding
 *
 * @param data    Pointer to image data
 * @param size    Size of data
 * @param width   Output width (may be NULL)
 * @param height  Output height (may be NULL)
 * @param format  Output format (may be NULL)
 * @return        IMAGE_OK on success
 */
int image_get_info(const void *data, uint32_t size,
                   uint32_t *width, uint32_t *height,
                   image_format_t *format);

/**
 * Free an image's pixel buffer
 *
 * @param img     Image to free (may be NULL)
 *
 * After calling, img->pixels will be NULL.
 */
void image_free(image_t *img);

/**
 * Get error string for error code
 */
const char *image_error_string(int err);

// ============================================================================
// Image Manipulation
// ============================================================================

/**
 * Create a scaled copy of an image (nearest neighbor)
 *
 * @param src     Source image
 * @param dst     Destination image (will be allocated)
 * @param width   Target width
 * @param height  Target height
 * @return        IMAGE_OK on success
 */
int image_scale(const image_t *src, image_t *dst,
                uint32_t width, uint32_t height);

/**
 * Create a thumbnail of an image
 *
 * @param src       Source image
 * @param thumb     Output thumbnail (will be allocated)
 * @param max_size  Maximum dimension (width or height)
 * @return          IMAGE_OK on success
 */
int image_thumbnail(const image_t *src, image_t *thumb, uint32_t max_size);

/**
 * Copy image data
 *
 * @param src     Source image
 * @param dst     Destination (will be allocated)
 * @return        IMAGE_OK on success
 */
int image_copy(const image_t *src, image_t *dst);

/**
 * Flip image vertically
 *
 * @param img     Image to flip (in place)
 */
void image_flip_vertical(image_t *img);

/**
 * Flip image horizontally
 *
 * @param img     Image to flip (in place)
 */
void image_flip_horizontal(image_t *img);

// ============================================================================
// Image Display
// ============================================================================

/**
 * Draw image to framebuffer
 *
 * @param img     Image to draw
 * @param x       X position (can be negative for clipping)
 * @param y       Y position (can be negative for clipping)
 */
void image_blit(const image_t *img, int x, int y);

/**
 * Draw image region to framebuffer
 *
 * @param img     Source image
 * @param dx, dy  Destination position
 * @param sx, sy  Source offset
 * @param sw, sh  Source region size
 */
void image_blit_region(const image_t *img, int dx, int dy,
                       uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh);

/**
 * Draw image scaled to destination size
 *
 * @param img     Source image
 * @param dx, dy  Destination position
 * @param dw, dh  Destination size
 */
void image_blit_scaled(const image_t *img, int dx, int dy,
                       uint32_t dw, uint32_t dh);

/**
 * Draw image with alpha blending
 *
 * @param img     Source image (with alpha channel)
 * @param x, y    Destination position
 */
void image_blit_alpha(const image_t *img, int x, int y);

// ============================================================================
// BMP-Specific Functions
// ============================================================================

// BMP header structures (for direct access if needed)
#pragma pack(push, 1)
typedef struct {
    uint16_t signature;
    uint32_t file_size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t data_offset;
} bmp_file_header_t;

typedef struct {
    uint32_t header_size;
    int32_t  width;
    int32_t  height;
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint32_t image_size;
    int32_t  x_ppm;
    int32_t  y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
} bmp_info_header_t;
#pragma pack(pop)

/**
 * Decode BMP image specifically
 */
int image_decode_bmp(const void *data, uint32_t size, image_t *img);

#endif // MEDIA_IMAGE_DECODE_H
