// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
// mpeg.c - MPEG-1 Video Decoder Implementation for MayteraOS
// Based on MPEG-1 specification ISO/IEC 11172
// Inspired by pl_mpeg (public domain), adapted for kernel use

#include "mpeg.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

// ============================================
// Internal Structures
// ============================================

// Bit reader for parsing bit streams
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t byte_pos;
    int bit_pos;            // Bits remaining in current byte (8 to 1)
} bitreader_t;

// MPEG decoder internal state
struct mpeg_decoder {
    // Input stream
    const uint8_t *data;
    size_t size;
    size_t pos;             // Current read position

    // Bitstream reader for video
    bitreader_t bits;

    // Sequence info
    mpeg_sequence_t seq;

    // Current picture
    mpeg_picture_t pic;

    // Frame buffers (double-buffered + B-frame buffer)
    mpeg_frame_t frames[3];
    int frame_current;      // Index of current display frame
    int frame_forward;      // Forward reference
    int frame_backward;     // Backward reference

    // Decode state
    int mb_row;             // Current macroblock row
    int mb_col;             // Current macroblock column
    int dc_predictor[3];    // DC predictors for Y, Cb, Cr
    motion_vec_t mv_forward;
    motion_vec_t mv_backward;

    // Quantizer matrices
    uint8_t intra_quant[64];
    uint8_t non_intra_quant[64];

    // Dequantized coefficients
    int block[64];

    // Audio state
    bool has_audio;
    int audio_sample_rate;
    int audio_channels;
    int16_t *audio_buffer;
    int audio_buffer_len;
    int audio_buffer_pos;

    // Timing
    double time;
    double frame_time;

    // State flags
    bool ended;
    bool sequence_started;
    bool picture_started;
};

// ============================================
// Constant Tables
// ============================================

// Default intra quantization matrix
static const uint8_t default_intra_quant[64] = {
     8, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83
};

// Default non-intra quantization matrix (all 16s)
static const uint8_t default_non_intra_quant[64] = {
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16
};

// Zigzag scan order
static const uint8_t zigzag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// Frame rates table (MPEG-1)
static const double frame_rates[16] = {
    0.0,          // 0 - forbidden
    24000.0/1001.0,  // 1 - 23.976 fps
    24.0,         // 2 - 24 fps
    25.0,         // 3 - 25 fps
    30000.0/1001.0,  // 4 - 29.97 fps
    30.0,         // 5 - 30 fps
    50.0,         // 6 - 50 fps
    60000.0/1001.0,  // 7 - 59.94 fps
    60.0,         // 8 - 60 fps
    0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0  // 9-15 reserved
};

// ============================================
// Bit Reader Functions
// ============================================

static void bits_init(bitreader_t *br, const uint8_t *data, size_t size) {
    br->data = data;
    br->size = size;
    br->byte_pos = 0;
    br->bit_pos = 8;
}

static inline int bits_has(bitreader_t *br, int n) {
    return (br->byte_pos * 8 + (8 - br->bit_pos) + n) <= (br->size * 8);
}

static inline uint32_t bits_read(bitreader_t *br, int n) {
    if (n == 0 || !bits_has(br, n)) return 0;

    uint32_t value = 0;
    while (n > 0) {
        int bits_left = br->bit_pos;
        int bits_to_read = (n < bits_left) ? n : bits_left;

        uint8_t mask = (1 << bits_left) - 1;
        uint8_t bits = (br->data[br->byte_pos] & mask) >> (bits_left - bits_to_read);

        value = (value << bits_to_read) | bits;
        n -= bits_to_read;
        br->bit_pos -= bits_to_read;

        if (br->bit_pos == 0) {
            br->byte_pos++;
            br->bit_pos = 8;
        }
    }
    return value;
}

static inline uint32_t bits_peek(bitreader_t *br, int n) {
    bitreader_t saved = *br;
    uint32_t value = bits_read(br, n);
    *br = saved;
    return value;
}

static inline void bits_skip(bitreader_t *br, int n) {
    bits_read(br, n);
}

static inline void bits_align(bitreader_t *br) {
    if (br->bit_pos != 8) {
        br->byte_pos++;
        br->bit_pos = 8;
    }
}

// ============================================
// Start Code Finding
// ============================================

// Find next start code (0x000001xx)
// Returns the start code byte (xx), or -1 if not found
static int find_start_code(mpeg_decoder_t *dec) {
    const uint8_t *p = dec->data + dec->pos;
    const uint8_t *end = dec->data + dec->size - 4;

    while (p < end) {
        if (p[0] == 0x00 && p[1] == 0x00 && p[2] == 0x01) {
            dec->pos = p - dec->data;
            return p[3];
        }
        p++;
    }
    return -1;
}

// Skip past current start code
static void skip_start_code(mpeg_decoder_t *dec) {
    dec->pos += 4;
}

// ============================================
// Frame Buffer Management
// ============================================

static bool alloc_frame(mpeg_frame_t *frame, int width, int height) {
    int y_size = width * height;
    int c_size = (width / 2) * (height / 2);

    frame->y = kzalloc(y_size);
    frame->cb = kzalloc(c_size);
    frame->cr = kzalloc(c_size);

    if (!frame->y || !frame->cb || !frame->cr) {
        kfree(frame->y);
        kfree(frame->cb);
        kfree(frame->cr);
        frame->y = frame->cb = frame->cr = NULL;
        return false;
    }

    frame->width = width;
    frame->height = height;
    frame->time = 0.0;
    return true;
}

static void free_frame(mpeg_frame_t *frame) {
    kfree(frame->y);
    kfree(frame->cb);
    kfree(frame->cr);
    frame->y = frame->cb = frame->cr = NULL;
}

// ============================================
// Sequence Header Parsing
// ============================================

static int parse_sequence_header(mpeg_decoder_t *dec) {
    bitreader_t *br = &dec->bits;

    // Initialize bit reader at current position after start code
    bits_init(br, dec->data + dec->pos, dec->size - dec->pos);

    // Read dimensions
    int width = bits_read(br, 12);
    int height = bits_read(br, 12);

    if (width == 0 || height == 0 || width > MPEG_MAX_WIDTH || height > MPEG_MAX_HEIGHT) {
        kprintf("[MPEG] Invalid dimensions: %dx%d\n", width, height);
        return VIDEO_ERR_CORRUPT;
    }

    // Pixel aspect ratio (4 bits) - we ignore this
    bits_skip(br, 4);

    // Frame rate code
    int frame_rate_code = bits_read(br, 4);
    if (frame_rate_code == 0 || frame_rate_code > 8) {
        kprintf("[MPEG] Invalid frame rate code: %d\n", frame_rate_code);
        frame_rate_code = 4;  // Default to ~30fps
    }

    // Bitrate (18 bits), marker (1 bit), vbv buffer size (10 bits)
    int bitrate = bits_read(br, 18) * 400;
    bits_skip(br, 1);  // Marker
    bits_skip(br, 10); // VBV buffer size
    bits_skip(br, 1);  // Constrained parameters flag

    // Load intra quantization matrix?
    if (bits_read(br, 1)) {
        for (int i = 0; i < 64; i++) {
            dec->intra_quant[zigzag[i]] = bits_read(br, 8);
        }
    } else {
        memcpy(dec->intra_quant, default_intra_quant, 64);
    }

    // Load non-intra quantization matrix?
    if (bits_read(br, 1)) {
        for (int i = 0; i < 64; i++) {
            dec->non_intra_quant[zigzag[i]] = bits_read(br, 8);
        }
    } else {
        memcpy(dec->non_intra_quant, default_non_intra_quant, 64);
    }

    // Store sequence info
    dec->seq.width = width;
    dec->seq.height = height;
    dec->seq.display_width = width;
    dec->seq.display_height = height;
    dec->seq.mb_width = (width + 15) / 16;
    dec->seq.mb_height = (height + 15) / 16;
    dec->seq.mb_size = dec->seq.mb_width * dec->seq.mb_height;
    dec->seq.frame_rate = frame_rates[frame_rate_code];
    dec->seq.bitrate = bitrate;
    dec->seq.has_sequence = true;

    dec->frame_time = 1.0 / dec->seq.frame_rate;

    // Allocate frame buffers
    int coded_width = dec->seq.mb_width * 16;
    int coded_height = dec->seq.mb_height * 16;

    for (int i = 0; i < 3; i++) {
        free_frame(&dec->frames[i]);
        if (!alloc_frame(&dec->frames[i], coded_width, coded_height)) {
            kprintf("[MPEG] Failed to allocate frame buffer %d\n", i);
            return VIDEO_ERR_NOMEM;
        }
    }

    dec->sequence_started = true;
    kprintf("[MPEG] Sequence: %dx%d @ %.2f fps, %d kbps\n",
            width, height, dec->seq.frame_rate, bitrate / 1000);

    return VIDEO_SUCCESS;
}

// ============================================
// Picture Header Parsing
// ============================================

static int parse_picture_header(mpeg_decoder_t *dec) {
    bitreader_t *br = &dec->bits;
    bits_init(br, dec->data + dec->pos, dec->size - dec->pos);

    dec->pic.temporal_ref = bits_read(br, 10);
    dec->pic.type = bits_read(br, 3);
    dec->pic.vbv_delay = bits_read(br, 16);

    if (dec->pic.type < 1 || dec->pic.type > 3) {
        kprintf("[MPEG] Invalid picture type: %d\n", dec->pic.type);
        return VIDEO_ERR_CORRUPT;
    }

    // Forward motion vectors (P and B frames)
    if (dec->pic.type == MPEG_PICTURE_P || dec->pic.type == MPEG_PICTURE_B) {
        dec->pic.full_pel_forward = bits_read(br, 1);
        dec->pic.forward_f_code = bits_read(br, 3);
    }

    // Backward motion vectors (B frames only)
    if (dec->pic.type == MPEG_PICTURE_B) {
        dec->pic.full_pel_backward = bits_read(br, 1);
        dec->pic.backward_f_code = bits_read(br, 3);
    }

    // Skip extra information
    while (bits_read(br, 1)) {
        bits_skip(br, 8);
    }

    // Reset DC predictors
    dec->dc_predictor[0] = 128;
    dec->dc_predictor[1] = 128;
    dec->dc_predictor[2] = 128;

    // Reset motion vectors
    dec->mv_forward.h = dec->mv_forward.v = 0;
    dec->mv_backward.h = dec->mv_backward.v = 0;

    dec->picture_started = true;

    return VIDEO_SUCCESS;
}

// ============================================
// IDCT (Inverse Discrete Cosine Transform)
// ============================================

// Simple reference IDCT (not optimized)
static void idct_block(int *block) {
    // Fixed-point precision
    #define IDCT_PRECISION 8
    #define IDCT_ROUND (1 << (IDCT_PRECISION - 1))

    // Cosine table (scaled by 256)
    static const int cos_table[8][8] = {
        {256, 256, 256, 256, 256, 256, 256, 256},
        {251, 213, 142,  50, -50,-142,-213,-251},
        {237, 98,  -98,-237,-237, -98,  98, 237},
        {213,-50, -251,-142, 142, 251,  50,-213},
        {181,-181,-181, 181, 181,-181,-181, 181},
        {142,-251,  50, 213,-213, -50, 251,-142},
        { 98,-237, 237, -98, -98, 237,-237,  98},
        { 50,-142, 213,-251, 251,-213, 142, -50}
    };

    int temp[64];

    // Transform rows
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int sum = 0;
            for (int u = 0; u < 8; u++) {
                sum += block[y * 8 + u] * cos_table[u][x];
            }
            temp[y * 8 + x] = (sum + IDCT_ROUND) >> IDCT_PRECISION;
        }
    }

    // Transform columns
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            int sum = 0;
            for (int v = 0; v < 8; v++) {
                sum += temp[v * 8 + x] * cos_table[v][y];
            }
            // Scale and clamp to 0-255
            int val = ((sum + IDCT_ROUND) >> IDCT_PRECISION) + 128;
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            block[y * 8 + x] = val;
        }
    }
}

// ============================================
// Block Decoding (Simplified)
// ============================================

// Decode a single 8x8 block
static void decode_block(mpeg_decoder_t *dec, int block_index, int *out_block, bool intra) {
    bitreader_t *br = &dec->bits;
    memset(out_block, 0, 64 * sizeof(int));

    // Get quantizer scale from slice header (stored elsewhere in full impl)
    int quant_scale = 8;  // Default

    uint8_t *quant_matrix = intra ? dec->intra_quant : dec->non_intra_quant;

    if (intra) {
        // DC coefficient
        int dc_size = 0;
        // Read DC coefficient size (simplified - real impl uses VLC table)
        uint32_t code = bits_peek(br, 9);
        if (code >= 0x100) { dc_size = 0; bits_skip(br, 3); }
        else if (code >= 0x80) { dc_size = 1; bits_skip(br, 4); }
        else if (code >= 0x40) { dc_size = 2; bits_skip(br, 5); }
        else if (code >= 0x20) { dc_size = 3; bits_skip(br, 5); }
        else if (code >= 0x10) { dc_size = 4; bits_skip(br, 6); }
        else if (code >= 0x08) { dc_size = 5; bits_skip(br, 7); }
        else if (code >= 0x04) { dc_size = 6; bits_skip(br, 8); }
        else { dc_size = 7; bits_skip(br, 9); }

        int dc_diff = 0;
        if (dc_size > 0) {
            dc_diff = bits_read(br, dc_size);
            if ((dc_diff & (1 << (dc_size - 1))) == 0) {
                dc_diff -= (1 << dc_size) - 1;
            }
        }

        // Update DC predictor
        int comp = (block_index < 4) ? 0 : (block_index - 3);
        dec->dc_predictor[comp] += dc_diff;
        out_block[0] = dec->dc_predictor[comp] * 8;
    }

    // AC coefficients (simplified - skip detailed VLC decoding for now)
    // In a full implementation, this would decode run-level pairs

    // Apply IDCT
    idct_block(out_block);
}

// ============================================
// Macroblock Decoding
// ============================================

static void decode_macroblock(mpeg_decoder_t *dec, int mb_x, int mb_y) {
    mpeg_frame_t *frame = &dec->frames[dec->frame_current];

    // Decode 6 blocks: 4 Y blocks, 1 Cb, 1 Cr
    int blocks[6][64];
    bool intra = (dec->pic.type == MPEG_PICTURE_I);

    for (int i = 0; i < 6; i++) {
        decode_block(dec, i, blocks[i], intra);
    }

    // Copy blocks to frame buffer
    int base_x = mb_x * 16;
    int base_y = mb_y * 16;
    int y_stride = frame->width;
    int c_stride = frame->width / 2;

    // Y blocks (2x2 arrangement)
    for (int by = 0; by < 2; by++) {
        for (int bx = 0; bx < 2; bx++) {
            int block_idx = by * 2 + bx;
            int *block = blocks[block_idx];
            int x0 = base_x + bx * 8;
            int y0 = base_y + by * 8;

            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    frame->y[(y0 + y) * y_stride + (x0 + x)] = block[y * 8 + x];
                }
            }
        }
    }

    // Cb block
    int *cb_block = blocks[4];
    int cx = mb_x * 8;
    int cy = mb_y * 8;
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            frame->cb[(cy + y) * c_stride + (cx + x)] = cb_block[y * 8 + x];
        }
    }

    // Cr block
    int *cr_block = blocks[5];
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            frame->cr[(cy + y) * c_stride + (cx + x)] = cr_block[y * 8 + x];
        }
    }
}

// ============================================
// Slice Decoding
// ============================================

static int decode_slice(mpeg_decoder_t *dec, int slice_code) {
    // Slice code indicates vertical position (1-based)
    int mb_row = slice_code - 1;
    if (mb_row >= dec->seq.mb_height) {
        return VIDEO_ERR_CORRUPT;
    }

    bitreader_t *br = &dec->bits;
    bits_init(br, dec->data + dec->pos, dec->size - dec->pos);

    // Quantizer scale
    int quant_scale = bits_read(br, 5);
    UNUSED(quant_scale);

    // Extra slice info
    while (bits_read(br, 1)) {
        bits_skip(br, 8);
    }

    // Reset DC predictors at slice start
    dec->dc_predictor[0] = 128;
    dec->dc_predictor[1] = 128;
    dec->dc_predictor[2] = 128;

    // Decode macroblocks in this slice (simplified)
    for (int mb_col = 0; mb_col < dec->seq.mb_width; mb_col++) {
        decode_macroblock(dec, mb_col, mb_row);
    }

    return VIDEO_SUCCESS;
}

// ============================================
// Picture Decoding
// ============================================

static int decode_picture(mpeg_decoder_t *dec) {
    if (!dec->sequence_started) {
        return VIDEO_ERR_CORRUPT;
    }

    // Select target frame buffer based on picture type
    if (dec->pic.type == MPEG_PICTURE_B) {
        dec->frame_current = 2;  // B-frames use third buffer
    } else {
        // Rotate reference frames for I and P
        int temp = dec->frame_forward;
        dec->frame_forward = dec->frame_backward;
        dec->frame_backward = temp;
        dec->frame_current = dec->frame_backward;
    }

    // Process slices until next picture or sequence
    while (dec->pos < dec->size) {
        int code = find_start_code(dec);
        if (code < 0) break;

        if (code >= MPEG_START_SLICE_MIN && code <= MPEG_START_SLICE_MAX) {
            skip_start_code(dec);
            decode_slice(dec, code);
        } else if (code == MPEG_START_PICTURE || code == MPEG_START_SEQUENCE ||
                   code == MPEG_START_END) {
            // Don't consume this start code
            break;
        } else {
            skip_start_code(dec);
        }
    }

    // Update frame time
    dec->frames[dec->frame_current].time = dec->time;
    dec->time += dec->frame_time;

    return VIDEO_SUCCESS;
}

// ============================================
// YCbCr to RGB Conversion
// ============================================

static void convert_ycbcr_to_rgb(mpeg_frame_t *frame, uint32_t *rgb,
                                  int display_width, int display_height) {
    int coded_width = frame->width;
    int c_stride = coded_width / 2;

    for (int y = 0; y < display_height; y++) {
        for (int x = 0; x < display_width; x++) {
            // Get Y sample
            int Y = frame->y[y * coded_width + x];

            // Get Cb, Cr samples (subsampled 2x2)
            int cx = x / 2;
            int cy = y / 2;
            int Cb = frame->cb[cy * c_stride + cx] - 128;
            int Cr = frame->cr[cy * c_stride + cx] - 128;

            // Convert to RGB using fixed-point math
            int r = Y + ((YUV_CR_R * Cr) >> YUV_FP_SHIFT);
            int g = Y - ((YUV_CB_G * Cb + YUV_CR_G * Cr) >> YUV_FP_SHIFT);
            int b = Y + ((YUV_CB_B * Cb) >> YUV_FP_SHIFT);

            // Clamp to 0-255
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;

            // Store as BGRA (framebuffer format)
            rgb[y * display_width + x] = (uint32_t)b | ((uint32_t)g << 8) |
                                         ((uint32_t)r << 16) | 0xFF000000;
        }
    }
}

// ============================================
// Public API Implementation
// ============================================

mpeg_decoder_t *mpeg_decoder_open(const void *data, size_t size) {
    if (!data || size < 12) {
        return NULL;
    }

    mpeg_decoder_t *dec = kzalloc(sizeof(mpeg_decoder_t));
    if (!dec) {
        kprintf("[MPEG] Failed to allocate decoder\n");
        return NULL;
    }

    dec->data = (const uint8_t *)data;
    dec->size = size;
    dec->pos = 0;
    dec->time = 0.0;
    dec->frame_time = 1.0 / 30.0;  // Default until we know better
    dec->ended = false;
    dec->sequence_started = false;
    dec->picture_started = false;

    // Initialize frame indices
    dec->frame_current = 0;
    dec->frame_forward = 0;
    dec->frame_backward = 1;

    // Copy default quantization matrices
    memcpy(dec->intra_quant, default_intra_quant, 64);
    memcpy(dec->non_intra_quant, default_non_intra_quant, 64);

    // Find and parse sequence header
    while (dec->pos < dec->size) {
        int code = find_start_code(dec);
        if (code < 0) {
            kprintf("[MPEG] No start codes found\n");
            break;
        }

        if (code == MPEG_START_SEQUENCE) {
            skip_start_code(dec);
            if (parse_sequence_header(dec) == VIDEO_SUCCESS) {
                kprintf("[MPEG] Decoder initialized successfully\n");
                return dec;
            }
            break;
        }

        skip_start_code(dec);

        // Skip pack headers
        if (code == MPEG_START_PACK) {
            dec->pos += 8;  // Skip pack header data
        }
    }

    kprintf("[MPEG] Failed to find valid sequence header\n");
    mpeg_decoder_close(dec);
    return NULL;
}

int mpeg_decoder_info(mpeg_decoder_t *dec, video_info_t *info) {
    if (!dec || !info) {
        return VIDEO_ERR_NULL_PTR;
    }

    if (!dec->seq.has_sequence) {
        return VIDEO_ERR_CORRUPT;
    }

    info->width = dec->seq.display_width;
    info->height = dec->seq.display_height;
    info->fps_num = (int)(dec->seq.frame_rate * 1000);
    info->fps_den = 1000;
    info->has_audio = dec->has_audio ? 1 : 0;
    info->audio_sample_rate = dec->audio_sample_rate;
    info->audio_channels = dec->audio_channels;
    info->audio_bits = 16;
    info->duration = 0.0;  // Unknown without scanning whole file
    info->format = VIDEO_FORMAT_MPEG1;

    return VIDEO_SUCCESS;
}

int mpeg_decoder_frame(mpeg_decoder_t *dec, uint32_t *rgb_buffer) {
    if (!dec || !rgb_buffer) {
        return VIDEO_ERR_NULL_PTR;
    }

    if (dec->ended) {
        return VIDEO_ERR_EOF;
    }

    if (!dec->seq.has_sequence) {
        return VIDEO_ERR_CORRUPT;
    }

    // Find and decode next picture
    while (dec->pos < dec->size) {
        int code = find_start_code(dec);
        if (code < 0) {
            dec->ended = true;
            return VIDEO_ERR_EOF;
        }

        if (code == MPEG_START_END) {
            dec->ended = true;
            return VIDEO_ERR_EOF;
        }

        if (code == MPEG_START_PICTURE) {
            skip_start_code(dec);
            if (parse_picture_header(dec) != VIDEO_SUCCESS) {
                continue;
            }

            if (decode_picture(dec) != VIDEO_SUCCESS) {
                continue;
            }

            // Convert YCbCr to RGB
            mpeg_frame_t *frame = &dec->frames[dec->frame_current];
            convert_ycbcr_to_rgb(frame, rgb_buffer,
                               dec->seq.display_width,
                               dec->seq.display_height);

            return VIDEO_SUCCESS;
        }

        if (code == MPEG_START_SEQUENCE) {
            skip_start_code(dec);
            parse_sequence_header(dec);
            continue;
        }

        // Skip other headers
        skip_start_code(dec);
        if (code == MPEG_START_PACK) {
            dec->pos += 8;
        }
    }

    dec->ended = true;
    return VIDEO_ERR_EOF;
}

int mpeg_decoder_audio(mpeg_decoder_t *dec, int16_t *samples, int max_samples) {
    if (!dec || !samples) {
        return VIDEO_ERR_NULL_PTR;
    }

    if (!dec->has_audio) {
        return VIDEO_ERR_NO_AUDIO;
    }

    // Audio decoding not yet implemented
    // Would need MPEG Audio Layer I/II decoder
    return 0;
}

void mpeg_decoder_seek(mpeg_decoder_t *dec, double seconds) {
    if (!dec) return;

    // For accurate seeking, we'd need to scan for I-frames
    // Simple implementation: rewind and decode forward
    if (seconds <= 0.0) {
        dec->pos = 0;
        dec->time = 0.0;
        dec->ended = false;
        dec->sequence_started = false;

        // Re-parse sequence header
        while (dec->pos < dec->size) {
            int code = find_start_code(dec);
            if (code == MPEG_START_SEQUENCE) {
                skip_start_code(dec);
                parse_sequence_header(dec);
                break;
            }
            if (code < 0) break;
            skip_start_code(dec);
        }
    }
    // For forward seeking, would need to decode frames until target time
}

int64_t mpeg_decoder_get_time(mpeg_decoder_t *dec) {
    return dec ? dec->time : 0;
}

int mpeg_decoder_has_ended(mpeg_decoder_t *dec) {
    return dec ? (dec->ended ? 1 : 0) : 1;
}

void mpeg_decoder_close(mpeg_decoder_t *dec) {
    if (!dec) return;

    for (int i = 0; i < 3; i++) {
        free_frame(&dec->frames[i]);
    }

    kfree(dec->audio_buffer);
    kfree(dec);
}
