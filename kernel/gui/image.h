// image.h - Image loading library for MayteraOS
#ifndef IMAGE_H
#define IMAGE_H

#include "../types.h"

// Image structure
typedef struct {
    uint32_t width;     // Image width in pixels
    uint32_t height;    // Image height in pixels
    uint32_t *pixels;   // Pixel data in BGRA format (compatible with framebuffer)
} image_t;

// Error codes
#define IMAGE_SUCCESS           0
#define IMAGE_ERR_NULL_PTR     -1
#define IMAGE_ERR_INVALID_SIG  -2
#define IMAGE_ERR_UNSUPPORTED  -3
#define IMAGE_ERR_NOMEM        -4
#define IMAGE_ERR_CORRUPT      -5
#define IMAGE_ERR_TOO_SMALL    -6

// BMP header structures (packed for direct memory mapping)
#pragma pack(push, 1)

// BMP file header (14 bytes)
typedef struct {
    uint16_t signature;     // "BM" (0x4D42 in little-endian)
    uint32_t file_size;     // Total file size
    uint16_t reserved1;     // Reserved (unused)
    uint16_t reserved2;     // Reserved (unused)
    uint32_t data_offset;   // Offset to pixel data
} bmp_file_header_t;

// BMP info header (BITMAPINFOHEADER - 40 bytes)
typedef struct {
    uint32_t header_size;   // Size of this header (40 bytes)
    int32_t  width;         // Image width (can be negative for top-down)
    int32_t  height;        // Image height (negative = top-down DIB)
    uint16_t planes;        // Number of color planes (must be 1)
    uint16_t bpp;           // Bits per pixel (24 or 32 for this implementation)
    uint32_t compression;   // Compression method (0 = BI_RGB uncompressed)
    uint32_t image_size;    // Size of raw pixel data (may be 0 for uncompressed)
    int32_t  x_ppm;         // Horizontal resolution (pixels per meter)
    int32_t  y_ppm;         // Vertical resolution (pixels per meter)
    uint32_t colors_used;   // Number of colors in palette (0 = default)
    uint32_t colors_important; // Number of important colors (0 = all)
} bmp_info_header_t;

#pragma pack(pop)

// BMP compression types
#define BMP_COMPRESSION_RGB       0   // Uncompressed
#define BMP_COMPRESSION_RLE8      1   // 8-bit RLE (not supported)
#define BMP_COMPRESSION_RLE4      2   // 4-bit RLE (not supported)
#define BMP_COMPRESSION_BITFIELDS 3   // Bit field masks (limited support)

// BMP signature
#define BMP_SIGNATURE 0x4D42  // "BM" in little-endian

/**
 * Load a BMP image from memory
 *
 * @param data    Pointer to BMP file data in memory
 * @param size    Size of the BMP data in bytes
 * @param img     Output image structure (will be filled on success)
 * @return        IMAGE_SUCCESS on success, negative error code on failure
 *
 * Supports:
 * - 24-bit uncompressed BMPs (BI_RGB)
 * - 32-bit uncompressed BMPs (BI_RGB)
 * - Both bottom-up and top-down DIB formats
 *
 * The resulting pixel buffer uses BGRA format compatible with the framebuffer.
 * Caller is responsible for freeing the image with image_free().
 */
int image_load_bmp(void *data, uint32_t size, image_t *img);

/**
 * Pure BMP decode seam (#404 Phase U). Parses + validates the BMP headers and
 * decodes 24/32bpp uncompressed pixels (bottom-up or top-down) into a caller-
 * bounded buffer. No allocation, no GUI. If out_px is NULL, only validates and
 * reports the dimensions (out_w/out_h). Returns IMAGE_SUCCESS or IMAGE_ERR_*.
 *   bmp_decode   - live dispatcher (routes to the Rust port under -DRUST_BMP)
 *   bmp_decode_c - the verbatim C reference (kept for rollback + differential)
 */
int bmp_decode(const uint8_t *buf, uint32_t len,
               uint32_t *out_px, uint32_t out_cap_px,
               uint32_t *out_w, uint32_t *out_h);
int bmp_decode_c(const uint8_t *buf, uint32_t len,
                 uint32_t *out_px, uint32_t out_cap_px,
                 uint32_t *out_w, uint32_t *out_h);

/** #404 Phase U boot-time [RUST-DIFF]/[RUST-SEC]/[RUST-PERF] self-test. */
void bmp_rust_selftest(void);

/**
 * Free an image's pixel buffer
 *
 * @param img     Image to free
 *
 * After calling this function, img->pixels will be NULL.
 * Safe to call with NULL img or img with NULL pixels.
 */
void image_free(image_t *img);

/**
 * Draw an image to the framebuffer
 *
 * @param img     Image to draw
 * @param x       X position on screen (left edge of image)
 * @param y       Y position on screen (top edge of image)
 *
 * Clips the image to screen boundaries.
 * Does nothing if img is NULL or has no pixel data.
 */
void image_blit(image_t *img, int x, int y);

/**
 * Draw a portion of an image to the framebuffer
 *
 * @param img     Source image
 * @param dx      Destination X position on screen
 * @param dy      Destination Y position on screen
 * @param sx      Source X offset within image
 * @param sy      Source Y offset within image
 * @param sw      Width of region to copy
 * @param sh      Height of region to copy
 */
void image_blit_region(image_t *img, int dx, int dy,
                       uint32_t sx, uint32_t sy, uint32_t sw, uint32_t sh);

/**
 * Draw an image scaled to fit destination dimensions
 *
 * Uses nearest-neighbor scaling for performance.
 *
 * @param img     Source image
 * @param dx      Destination X position on screen
 * @param dy      Destination Y position on screen
 * @param dw      Destination width (scaled)
 * @param dh      Destination height (scaled)
 */
void image_blit_scaled(image_t *img, int dx, int dy, uint32_t dw, uint32_t dh);

/**
 * Get error string for error code
 *
 * @param err     Error code from image_load_bmp
 * @return        Human-readable error string
 */
const char *image_error_string(int err);

/**
 * Load an image from memory, auto-detecting format
 *
 * @param data    Pointer to image file data
 * @param size    Size of the data in bytes
 * @param img     Output image structure
 * @return        IMAGE_SUCCESS on success, negative error code on failure
 *
 * Supports: BMP, PNG, JPEG
 */
int image_load(const void *data, uint32_t size, image_t *img);

/**
 * Check image format
 *
 * @param data    Pointer to image file data
 * @param size    Size of data
 * @return        Format identifier: 'B'=BMP, 'P'=PNG, 'J'=JPEG, 0=unknown
 */
int image_detect_format(const void *data, uint32_t size);

#endif // IMAGE_H
