// jpeg.h - JPEG image decoder for MayteraOS
#ifndef JPEG_H
#define JPEG_H

#include "../types.h"
#include "image.h"

// JPEG error codes
#define JPEG_SUCCESS            0
#define JPEG_ERR_NULL_PTR      -1
#define JPEG_ERR_INVALID_SIG   -2
#define JPEG_ERR_UNSUPPORTED   -3
#define JPEG_ERR_NOMEM         -4
#define JPEG_ERR_CORRUPT       -5
#define JPEG_ERR_TOO_SMALL     -6
#define JPEG_ERR_HUFFMAN       -7
#define JPEG_ERR_MARKER        -8

// JPEG markers
#define JPEG_MARKER_SOI  0xFFD8  // Start of image
#define JPEG_MARKER_EOI  0xFFD9  // End of image
#define JPEG_MARKER_SOS  0xFFDA  // Start of scan
#define JPEG_MARKER_DQT  0xFFDB  // Define quantization table
#define JPEG_MARKER_SOF0 0xFFC0  // Start of frame (baseline DCT)
#define JPEG_MARKER_DHT  0xFFC4  // Define Huffman table
#define JPEG_MARKER_DRI  0xFFDD  // Define restart interval
#define JPEG_MARKER_APP0 0xFFE0  // Application marker

// #404 Phase W: pure JPEG header-parse seam output. Mirrors the Rust JpegHdr
// (#[repr(C)] in rustkern.rs); gui/jpeg.c locks sizeof with a _Static_assert so
// the C struct and the Rust view can never silently drift. Holds everything the
// (C) entropy decoder needs: dims, sampling, MCU grid, quant + Huffman table
// DEFINITIONS, restart interval, and entropy_pos = the byte offset where the
// scan's entropy-coded data begins. entropy_pos == 0 means "no SOS found".
typedef struct {
    uint32_t width;
    uint32_t height;
    int32_t  components;
    int32_t  comp_id[4];
    int32_t  comp_h[4];
    int32_t  comp_v[4];
    int32_t  comp_qt[4];
    int32_t  comp_dc[4];
    int32_t  comp_ac[4];
    int32_t  mcu_width;
    int32_t  mcu_height;
    int32_t  mcu_count_x;
    int32_t  mcu_count_y;
    int32_t  restart_interval;
    int32_t  quant_valid[4];
    int32_t  huff_valid[2][2];
    uint32_t entropy_pos;
    int32_t  status;
    uint8_t  quant[4][64];
    uint8_t  huff_bits[2][2][16];
    uint8_t  huff_vals[2][2][256];
} jpeg_hdr_t;

// Pure header-parse seam dispatcher (routes to jpeg_parse_headers_rs under
// -DRUST_JPEG, else jpeg_parse_headers_c). Walks SOI..SOS scan header into out.
int jpeg_parse_headers(const uint8_t *data, uint32_t len, jpeg_hdr_t *out);

/**
 * Load a JPEG image from memory
 *
 * @param data    Pointer to JPEG file data
 * @param size    Size of the JPEG data in bytes
 * @param img     Output image structure
 * @return        JPEG_SUCCESS on success, negative error code on failure
 *
 * Supports:
 * - Baseline DCT JPEG (SOF0)
 * - 8-bit precision
 * - YCbCr color space (converted to RGB)
 * - Grayscale images
 *
 * Does NOT support:
 * - Progressive JPEG
 * - Arithmetic coding
 * - 12-bit or 16-bit precision
 *
 * The resulting pixel buffer uses BGRA format compatible with the framebuffer.
 */
int image_load_jpeg(const void *data, uint32_t size, image_t *img);

/**
 * Check if data is a JPEG image
 *
 * @param data    Pointer to file data
 * @param size    Size of data
 * @return        1 if JPEG, 0 otherwise
 */
int image_is_jpeg(const void *data, uint32_t size);

/**
 * Get JPEG error string
 */
const char *jpeg_error_string(int err);

#endif // JPEG_H
