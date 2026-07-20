// avi.h - AVI Container Parser for MayteraOS
// Parses AVI files and provides access to video/audio streams
#ifndef AVI_H
#define AVI_H

#include "../types.h"
#include "video_decode.h"

// AVI FourCC codes
#define AVI_FOURCC(a, b, c, d) \
    (((uint32_t)(a)) | ((uint32_t)(b) << 8) | \
     ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define AVI_RIFF        AVI_FOURCC('R', 'I', 'F', 'F')
#define AVI_AVI         AVI_FOURCC('A', 'V', 'I', ' ')
#define AVI_LIST        AVI_FOURCC('L', 'I', 'S', 'T')
#define AVI_HDRL        AVI_FOURCC('h', 'd', 'r', 'l')
#define AVI_AVIH        AVI_FOURCC('a', 'v', 'i', 'h')
#define AVI_STRL        AVI_FOURCC('s', 't', 'r', 'l')
#define AVI_STRH        AVI_FOURCC('s', 't', 'r', 'h')
#define AVI_STRF        AVI_FOURCC('s', 't', 'r', 'f')
#define AVI_MOVI        AVI_FOURCC('m', 'o', 'v', 'i')
#define AVI_IDX1        AVI_FOURCC('i', 'd', 'x', '1')

// Stream types
#define AVI_VIDS        AVI_FOURCC('v', 'i', 'd', 's')
#define AVI_AUDS        AVI_FOURCC('a', 'u', 'd', 's')

// Video codecs
#define AVI_MJPG        AVI_FOURCC('M', 'J', 'P', 'G')
#define AVI_MJPG_LC     AVI_FOURCC('m', 'j', 'p', 'g')
#define AVI_DIB         AVI_FOURCC('D', 'I', 'B', ' ')
#define AVI_RGB         AVI_FOURCC('R', 'G', 'B', ' ')
#define AVI_RAW         0  // Uncompressed RGB

// Audio formats
#define AVI_WAVE_PCM    0x0001
#define AVI_WAVE_ADPCM  0x0002
#define AVI_WAVE_MP3    0x0055

// Maximum streams
#define AVI_MAX_STREAMS     16

// AVI main header (avih chunk)
typedef struct __attribute__((packed)) {
    uint32_t microsec_per_frame;    // Frame time in microseconds
    uint32_t max_bytes_per_sec;     // Max data rate
    uint32_t padding_granularity;   // Pad to multiple of this
    uint32_t flags;                 // Flags
    uint32_t total_frames;          // Total number of frames
    uint32_t initial_frames;        // Initial frames (for interleaved)
    uint32_t streams;               // Number of streams
    uint32_t suggested_buffer;      // Suggested buffer size
    uint32_t width;                 // Video width
    uint32_t height;                // Video height
    uint32_t reserved[4];           // Reserved
} avi_main_header_t;

// AVI flags
#define AVI_FLAG_HASINDEX       0x00000010
#define AVI_FLAG_MUSTUSEINDEX   0x00000020
#define AVI_FLAG_ISINTERLEAVED  0x00000100
#define AVI_FLAG_TRUSTCKTYPE    0x00000800
#define AVI_FLAG_WASCAPTUREFILE 0x00010000
#define AVI_FLAG_COPYRIGHTED    0x00020000

// Stream header (strh chunk)
typedef struct __attribute__((packed)) {
    uint32_t type;              // 'vids' or 'auds'
    uint32_t handler;           // FourCC codec
    uint32_t flags;
    uint16_t priority;
    uint16_t language;
    uint32_t initial_frames;
    uint32_t scale;             // Time scale
    uint32_t rate;              // Rate / scale = samples/sec
    uint32_t start;             // Start time
    uint32_t length;            // Stream length (in scale units)
    uint32_t suggested_buffer;
    uint32_t quality;
    uint32_t sample_size;
    int16_t  left, top;
    int16_t  right, bottom;
} avi_stream_header_t;

// BITMAPINFOHEADER for video format
typedef struct __attribute__((packed)) {
    uint32_t size;              // Size of this structure
    int32_t  width;             // Width in pixels
    int32_t  height;            // Height (positive = bottom-up)
    uint16_t planes;            // Must be 1
    uint16_t bit_count;         // Bits per pixel
    uint32_t compression;       // Compression FourCC
    uint32_t size_image;        // Image size in bytes
    int32_t  x_pels_per_meter;
    int32_t  y_pels_per_meter;
    uint32_t clr_used;
    uint32_t clr_important;
} avi_bitmap_info_t;

// WAVEFORMATEX for audio format
typedef struct __attribute__((packed)) {
    uint16_t format_tag;        // Format type (1 = PCM)
    uint16_t channels;          // Number of channels
    uint32_t samples_per_sec;   // Sample rate
    uint32_t avg_bytes_per_sec; // Average data rate
    uint16_t block_align;       // Block alignment
    uint16_t bits_per_sample;   // Bits per sample
    // Additional fields may follow for non-PCM
} avi_wave_format_t;

// Index entry (idx1 chunk)
typedef struct __attribute__((packed)) {
    uint32_t chunk_id;          // Stream identifier (e.g., '00dc')
    uint32_t flags;
    uint32_t offset;            // Offset from movi start
    uint32_t size;              // Chunk size
} avi_index_entry_t;

// Index flags
#define AVI_IDX_KEYFRAME    0x00000010

// Stream info (internal)
typedef struct {
    uint32_t type;              // AVI_VIDS or AVI_AUDS
    uint32_t codec;             // Codec FourCC
    int width, height;          // For video
    int bit_depth;              // Bits per pixel/sample
    int sample_rate;            // For audio
    int channels;               // For audio
    uint32_t scale, rate;       // Timing
    uint32_t length;            // Total chunks
} avi_stream_info_t;

// Opaque decoder structure
typedef struct avi_decoder avi_decoder_t;

// ============================================
// AVI Decoder API
// ============================================

/**
 * Open AVI file for decoding
 *
 * @param data      AVI file data in memory
 * @param size      Size of data in bytes
 * @return          Decoder handle or NULL on failure
 */
avi_decoder_t *avi_decoder_open(const void *data, size_t size);

/**
 * Get video information
 */
int avi_decoder_info(avi_decoder_t *dec, video_info_t *info);

/**
 * Decode next video frame to RGB
 *
 * @param dec           Decoder handle
 * @param rgb_buffer    Output buffer (width * height * 4 bytes, BGRA format)
 * @return              VIDEO_SUCCESS, VIDEO_ERR_EOF, or error code
 */
int avi_decoder_frame(avi_decoder_t *dec, uint32_t *rgb_buffer);

/**
 * Decode audio samples
 *
 * @param dec           Decoder handle
 * @param samples       Output buffer for 16-bit signed PCM
 * @param max_samples   Maximum samples to decode
 * @return              Number of samples decoded, or error code
 */
int avi_decoder_audio(avi_decoder_t *dec, int16_t *samples, int max_samples);

/**
 * Seek to time position
 */
void avi_decoder_seek(avi_decoder_t *dec, double seconds);

/**
 * Get current playback time
 */
int avi_decoder_get_time_ms(avi_decoder_t *dec);

/**
 * Check if at end of stream
 */
int avi_decoder_has_ended(avi_decoder_t *dec);

/**
 * Close decoder and free resources
 */
void avi_decoder_close(avi_decoder_t *dec);

#endif // AVI_H
