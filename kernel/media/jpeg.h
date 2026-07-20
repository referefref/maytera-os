// jpeg.h - JPEG Image Decoder for MayteraOS
// Full baseline JPEG decoder with Huffman coding and IDCT
//
// Features:
// - SOI, APP0, DQT, DHT, SOF0, SOS markers
// - Huffman decoding (DC and AC coefficients)
// - Inverse Discrete Cosine Transform (IDCT)
// - YCbCr to RGB color space conversion
// - Baseline JPEG support (SOF0)
// - Grayscale and color images
// - Multiple sampling factors (4:4:4, 4:2:2, 4:2:0)

#ifndef MEDIA_JPEG_H
#define MEDIA_JPEG_H

#include "../types.h"

// Forward declare image_t if not including image.h
#ifndef IMAGE_H
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t *pixels;
} image_t;
#endif

// ============================================================================
// JPEG Error Codes
// ============================================================================

#define JPEG_OK                  0
#define JPEG_ERR_NULL_PTR       -1
#define JPEG_ERR_INVALID_SIG    -2
#define JPEG_ERR_UNSUPPORTED    -3
#define JPEG_ERR_NOMEM          -4
#define JPEG_ERR_CORRUPT        -5
#define JPEG_ERR_TOO_SMALL      -6
#define JPEG_ERR_HUFFMAN        -7
#define JPEG_ERR_MARKER         -8
#define JPEG_ERR_DQT            -9
#define JPEG_ERR_DHT           -10
#define JPEG_ERR_SOF           -11
#define JPEG_ERR_SOS           -12

// ============================================================================
// JPEG Constants
// ============================================================================

// JPEG markers
#define JPEG_MARKER_SOI     0xFFD8  // Start of image
#define JPEG_MARKER_EOI     0xFFD9  // End of image
#define JPEG_MARKER_SOS     0xFFDA  // Start of scan
#define JPEG_MARKER_DQT     0xFFDB  // Define quantization table
#define JPEG_MARKER_SOF0    0xFFC0  // Baseline DCT
#define JPEG_MARKER_SOF1    0xFFC1  // Extended sequential DCT
#define JPEG_MARKER_SOF2    0xFFC2  // Progressive DCT
#define JPEG_MARKER_DHT     0xFFC4  // Define Huffman table
#define JPEG_MARKER_DRI     0xFFDD  // Define restart interval
#define JPEG_MARKER_RST0    0xFFD0  // Restart marker 0
#define JPEG_MARKER_RST7    0xFFD7  // Restart marker 7
#define JPEG_MARKER_APP0    0xFFE0  // Application marker 0 (JFIF)
#define JPEG_MARKER_APP1    0xFFE1  // Application marker 1 (EXIF)
#define JPEG_MARKER_APP14   0xFFEE  // Application marker 14 (Adobe)
#define JPEG_MARKER_COM     0xFFFE  // Comment

// Maximum dimensions and limits
#define JPEG_MAX_WIDTH      16384
#define JPEG_MAX_HEIGHT     16384
#define JPEG_MAX_COMPONENTS     4
#define JPEG_DCT_SIZE          64

// ============================================================================
// JPEG API
// ============================================================================

/**
 * Check if data appears to be a JPEG image
 *
 * @param data    Pointer to file data
 * @param size    Size of data in bytes
 * @return        1 if JPEG signature matches, 0 otherwise
 */
int jpeg_is_jpeg(const void *data, uint32_t size);

/**
 * Load a JPEG image from memory
 *
 * @param data    Pointer to JPEG file data
 * @param size    Size of the JPEG data in bytes
 * @param img     Output image structure
 * @return        JPEG_OK on success, negative error code on failure
 *
 * Supports:
 * - Baseline DCT JPEG (SOF0)
 * - 8-bit precision
 * - YCbCr and Grayscale
 * - Various sampling factors
 *
 * Does NOT support:
 * - Progressive JPEG (SOF2)
 * - Arithmetic coding
 * - 12-bit precision
 * - CMYK color space
 *
 * The resulting pixel buffer uses BGRA format compatible with the framebuffer.
 * Caller is responsible for freeing img->pixels with kfree().
 */
int jpeg_decode(const void *data, uint32_t size, image_t *img);

/**
 * Get error string for JPEG error code
 */
const char *jpeg_error_string(int err);

/**
 * Get JPEG image info without fully decoding
 *
 * @param data    Pointer to JPEG data
 * @param size    Size of data
 * @param width   Output width (may be NULL)
 * @param height  Output height (may be NULL)
 * @param comps   Output number of components (may be NULL)
 * @return        JPEG_OK on success, error code on failure
 */
int jpeg_get_info(const void *data, uint32_t size,
                  uint32_t *width, uint32_t *height, int *comps);

#endif // MEDIA_JPEG_H
