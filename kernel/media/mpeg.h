// mpeg.h - MPEG-1 Video/Audio Decoder for MayteraOS
// Decodes MPEG-1 video and MPEG Audio Layer II from MPEG Program Streams
#ifndef MPEG_H
#define MPEG_H

#include "../types.h"
#include "video_decode.h"

// MPEG-1 Start Codes
#define MPEG_START_PACK         0xBA
#define MPEG_START_SYSTEM       0xBB
#define MPEG_START_END          0xB9
#define MPEG_START_VIDEO        0xE0    // Video stream (E0-EF)
#define MPEG_START_AUDIO        0xC0    // Audio stream (C0-DF)
#define MPEG_START_SEQUENCE     0xB3    // Video sequence header
#define MPEG_START_EXTENSION    0xB5
#define MPEG_START_USER_DATA    0xB2
#define MPEG_START_GOP          0xB8    // Group of pictures
#define MPEG_START_PICTURE      0x00    // Picture header
#define MPEG_START_SLICE_MIN    0x01    // Slice start codes 0x01-0xAF
#define MPEG_START_SLICE_MAX    0xAF

// Picture types
#define MPEG_PICTURE_I          1       // Intra-coded
#define MPEG_PICTURE_P          2       // Predictive-coded
#define MPEG_PICTURE_B          3       // Bidirectionally-predictive

// Motion vector types
#define MPEG_MOTION_FORWARD     1
#define MPEG_MOTION_BACKWARD    2

// Macroblock types (simplified)
#define MB_INTRA                0x01
#define MB_PATTERN              0x02
#define MB_MOTION_FORWARD       0x04
#define MB_MOTION_BACKWARD      0x08
#define MB_QUANT                0x10

// YCbCr to RGB conversion constants (fixed point, 16.16)
#define YUV_FP_SHIFT    16
#define YUV_CR_R        91881   // 1.402 * 65536
#define YUV_CB_G        22554   // 0.344136 * 65536
#define YUV_CR_G        46802   // 0.714136 * 65536
#define YUV_CB_B        116130  // 1.772 * 65536

// Maximum supported dimensions
#define MPEG_MAX_WIDTH          1920
#define MPEG_MAX_HEIGHT         1080

// Buffer sizes
#define MPEG_BUFFER_SIZE        (2 * 1024 * 1024)  // 2MB decode buffer
#define MPEG_AUDIO_SAMPLES      1152    // Samples per MPEG audio frame

// MPEG Audio Layer II frame structure
typedef struct {
    int sample_rate;
    int channels;
    int bitrate;
    int frame_size;
} mpeg_audio_header_t;

// Video sequence header info
typedef struct {
    int width;
    int height;
    int mb_width;           // Width in macroblocks
    int mb_height;          // Height in macroblocks
    int mb_size;            // Total macroblocks
    int display_width;
    int display_height;
    double frame_rate;
    double pixel_aspect;
    int bitrate;
    bool has_sequence;
} mpeg_sequence_t;

// Picture header info
typedef struct {
    int type;               // I, P, or B
    int temporal_ref;       // Temporal reference
    int vbv_delay;
    bool full_pel_forward;
    bool full_pel_backward;
    int forward_f_code;
    int backward_f_code;
} mpeg_picture_t;

// Motion vectors
typedef struct {
    int h, v;               // Horizontal and vertical components
} motion_vec_t;

// Frame buffer (Y, Cb, Cr planes)
typedef struct {
    uint8_t *y;
    uint8_t *cb;
    uint8_t *cr;
    int width;
    int height;
    double time;
} mpeg_frame_t;

// Opaque decoder structure
typedef struct mpeg_decoder mpeg_decoder_t;

// ============================================
// MPEG Decoder API
// ============================================

/**
 * Open MPEG decoder
 *
 * @param data      MPEG program stream data
 * @param size      Size of data in bytes
 * @return          Decoder handle or NULL on failure
 */
mpeg_decoder_t *mpeg_decoder_open(const void *data, size_t size);

/**
 * Get video information
 */
int mpeg_decoder_info(mpeg_decoder_t *dec, video_info_t *info);

/**
 * Decode next video frame to RGB
 *
 * @param dec           Decoder handle
 * @param rgb_buffer    Output buffer (width * height * 4 bytes, BGRA format)
 * @return              VIDEO_SUCCESS, VIDEO_ERR_EOF, or error code
 */
int mpeg_decoder_frame(mpeg_decoder_t *dec, uint32_t *rgb_buffer);

/**
 * Decode audio samples
 *
 * @param dec           Decoder handle
 * @param samples       Output buffer for 16-bit signed PCM
 * @param max_samples   Maximum samples to decode
 * @return              Number of samples decoded, or error code
 */
int mpeg_decoder_audio(mpeg_decoder_t *dec, int16_t *samples, int max_samples);

/**
 * Seek to time position
 */
void mpeg_decoder_seek(mpeg_decoder_t *dec, double seconds);

/**
 * Get current playback time
 */
int64_t mpeg_decoder_get_time(mpeg_decoder_t *dec);

/**
 * Check if at end of stream
 */
int mpeg_decoder_has_ended(mpeg_decoder_t *dec);

/**
 * Close decoder and free resources
 */
void mpeg_decoder_close(mpeg_decoder_t *dec);

#endif // MPEG_H
