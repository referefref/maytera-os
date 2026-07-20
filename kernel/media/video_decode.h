// video_decode.h - Video Decoder API for MayteraOS
// Unified video decoding interface supporting MPEG-1, AVI containers
#ifndef VIDEO_DECODE_H
#define VIDEO_DECODE_H

#include "../types.h"

// Video format identifiers
#define VIDEO_FORMAT_UNKNOWN    0
#define VIDEO_FORMAT_MPEG1      1   // MPEG Program Stream (.mpg, .mpeg)
#define VIDEO_FORMAT_AVI        2   // AVI container (.avi)
#define VIDEO_FORMAT_MP4        3   // MP4/MOV container (basic support)

// Error codes
#define VIDEO_SUCCESS           0
#define VIDEO_ERR_NULL_PTR     -1
#define VIDEO_ERR_INVALID_FMT  -2
#define VIDEO_ERR_NOMEM        -3
#define VIDEO_ERR_CORRUPT      -4
#define VIDEO_ERR_UNSUPPORTED  -5
#define VIDEO_ERR_EOF          -6
#define VIDEO_ERR_NO_FRAME     -7
#define VIDEO_ERR_NO_AUDIO     -8

// Maximum dimensions for sanity checking
#define VIDEO_MAX_WIDTH         1920
#define VIDEO_MAX_HEIGHT        1080

// Video information structure
typedef struct {
    int width;              // Frame width in pixels
    int height;             // Frame height in pixels
    int fps_num;            // Frame rate numerator (e.g., 30000)
    int fps_den;            // Frame rate denominator (e.g., 1001 for 29.97 fps)
    int has_audio;          // Non-zero if audio track present
    int audio_sample_rate;  // Audio sample rate in Hz (e.g., 44100)
    int audio_channels;     // Audio channels (1=mono, 2=stereo)
    int audio_bits;         // Audio bits per sample (8 or 16)
    double duration;        // Total duration in seconds (0 if unknown)
    int format;             // VIDEO_FORMAT_xxx
} video_info_t;

// Opaque decoder handle
typedef struct video_decoder video_decoder_t;

// ============================================
// Core Decoding API
// ============================================

/**
 * Open a video decoder for the given data
 *
 * @param data      Pointer to video file data in memory
 * @param size      Size of the data in bytes
 * @return          Decoder handle, or NULL on failure
 *
 * Automatically detects video format and initializes the appropriate decoder.
 * Supported formats: MPEG-1 Program Stream, AVI
 */
video_decoder_t *video_decode_open(const void *data, size_t size);

/**
 * Get information about the video
 *
 * @param dec       Decoder handle
 * @param info      Output video information structure
 * @return          VIDEO_SUCCESS on success, negative error on failure
 */
int video_decode_info(video_decoder_t *dec, video_info_t *info);

/**
 * Decode the next video frame
 *
 * @param dec           Decoder handle
 * @param rgb_buffer    Output buffer for RGB pixels (must be width*height*4 bytes)
 *                      Pixels are in BGRA format for direct framebuffer blitting
 * @return              VIDEO_SUCCESS on success,
 *                      VIDEO_ERR_EOF at end of video,
 *                      VIDEO_ERR_NO_FRAME if no frame available yet
 *
 * The caller must ensure rgb_buffer has sufficient space.
 * Get required size with: width * height * 4 bytes
 */
int video_decode_frame(video_decoder_t *dec, uint32_t *rgb_buffer);

/**
 * Decode audio samples
 *
 * @param dec           Decoder handle
 * @param samples       Output buffer for signed 16-bit PCM samples
 * @param max_samples   Maximum number of samples to decode (per channel)
 * @return              Number of samples decoded (per channel),
 *                      VIDEO_ERR_EOF at end of audio,
 *                      VIDEO_ERR_NO_AUDIO if no audio track
 *
 * For stereo audio, samples are interleaved (L, R, L, R, ...).
 * Buffer size needed: max_samples * channels * sizeof(int16_t)
 */
int video_decode_audio(video_decoder_t *dec, int16_t *samples, int max_samples);

/**
 * Seek to a position in the video
 *
 * @param dec       Decoder handle
 * @param seconds   Time in seconds to seek to
 *
 * Seeks to the nearest keyframe at or before the specified time.
 * For MPEG, this may be imprecise due to GOP structure.
 */
void video_decode_seek(video_decoder_t *dec, double seconds);

/**
 * Get current playback position
 *
 * @param dec       Decoder handle
 * @return          Current position in seconds
 */
int64_t video_decode_get_time(video_decoder_t *dec);

/**
 * Check if video has ended
 *
 * @param dec       Decoder handle
 * @return          Non-zero if at end of video
 */
int video_decode_has_ended(video_decoder_t *dec);

/**
 * Rewind to beginning of video
 *
 * @param dec       Decoder handle
 */
void video_decode_rewind(video_decoder_t *dec);

/**
 * Close the decoder and free resources
 *
 * @param dec       Decoder handle (safe to call with NULL)
 */
void video_decode_close(video_decoder_t *dec);

// ============================================
// Format Detection
// ============================================

/**
 * Detect video format from data
 *
 * @param data      Pointer to file data
 * @param size      Size of data in bytes
 * @return          VIDEO_FORMAT_xxx constant
 */
int video_detect_format(const void *data, size_t size);

/**
 * Get format name string
 *
 * @param format    VIDEO_FORMAT_xxx constant
 * @return          Human-readable format name
 */
const char *video_format_name(int format);

/**
 * Get error string
 *
 * @param error     Error code
 * @return          Human-readable error message
 */
const char *video_error_string(int error);

// ============================================
// Utility Functions
// ============================================

/**
 * Calculate buffer size needed for frames
 *
 * @param info      Video info structure
 * @return          Bytes needed for one frame's RGB buffer
 */
static inline size_t video_frame_size(const video_info_t *info) {
    return (size_t)info->width * (size_t)info->height * 4;
}

/**
 * Calculate frame duration in milliseconds
 *
 * @param info      Video info structure
 * @return          Milliseconds per frame
 */
static inline int video_frame_duration_ms(const video_info_t *info) {
    if (info->fps_num == 0) return 33;  // Default ~30fps
    return (info->fps_den * 1000) / info->fps_num;
}

#endif // VIDEO_DECODE_H
