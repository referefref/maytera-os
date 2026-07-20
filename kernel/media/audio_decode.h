// audio_decode.h - MayteraOS Unified Audio Decoder API
//
// Provides a common interface for decoding various audio formats:
// - MP3 (MPEG Audio Layer III)
// - FLAC (Free Lossless Audio Codec)
// - OGG Vorbis
// - WAV (for consistency, though it's uncompressed)
//
// Usage:
//   audio_decoder_t *dec = audio_decode_open(file_data, file_size);
//   if (dec) {
//       audio_info_t info;
//       audio_decode_info(dec, &info);
//       int16_t samples[4096];
//       int count;
//       while ((count = audio_decode_read(dec, samples, 4096)) > 0) {
//           // Process samples...
//       }
//       audio_decode_close(dec);
//   }

#ifndef AUDIO_DECODE_H
#define AUDIO_DECODE_H

#include "../types.h"

// ============================================================================
// Audio Format Detection
// ============================================================================

typedef enum {
    AUDIO_CODEC_UNKNOWN = 0,
    AUDIO_CODEC_WAV,        // RIFF/WAVE
    AUDIO_CODEC_MP3,        // MPEG Audio Layer III
    AUDIO_CODEC_FLAC,       // Free Lossless Audio Codec
    AUDIO_CODEC_OGG_VORBIS, // OGG container with Vorbis audio
    AUDIO_CODEC_AAC,        // Advanced Audio Coding (future)
    AUDIO_CODEC_OPUS        // Opus codec (future)
} audio_codec_t;

// ============================================================================
// Audio Information Structure
// ============================================================================

typedef struct {
    uint32_t sample_rate;       // Sample rate in Hz (e.g., 44100, 48000)
    uint32_t channels;          // Number of channels (1=mono, 2=stereo)
    uint32_t bits_per_sample;   // Bits per sample (typically 16)
    uint64_t total_samples;     // Total samples per channel (0 if unknown)
    uint32_t bitrate;           // Bitrate in bits/second (for compressed formats)
    uint32_t duration_ms;       // Duration in milliseconds (0 if unknown)
    audio_codec_t codec;        // Detected codec type
} audio_info_t;

// ============================================================================
// Decoder Handle (Opaque)
// ============================================================================

typedef struct audio_decoder audio_decoder_t;

// ============================================================================
// Decoder Error Codes
// ============================================================================

#define DECODE_OK                    0
#define DECODE_ERR_INVALID_DATA     -1
#define DECODE_ERR_UNSUPPORTED      -2
#define DECODE_ERR_NO_MEMORY        -3
#define DECODE_ERR_CORRUPT          -4
#define DECODE_ERR_END_OF_STREAM    -5
#define DECODE_ERR_INTERNAL         -6

// ============================================================================
// Format Detection
// ============================================================================

// Detect audio format from file data
// data: Pointer to file data
// size: Size of data in bytes
// Returns: Detected codec type, or AUDIO_CODEC_UNKNOWN
audio_codec_t audio_detect_format(const void *data, size_t size);

// Get human-readable name for codec
const char *audio_codec_name(audio_codec_t codec);

// ============================================================================
// Decoder API
// ============================================================================

// Open a decoder for audio data
// data: Pointer to compressed audio data
// size: Size of data in bytes
// Returns: Decoder handle on success, NULL on failure
audio_decoder_t *audio_decode_open(const void *data, size_t size);

// Get audio information
// dec: Decoder handle
// info: Output structure for audio info
// Returns: DECODE_OK on success, error code on failure
int audio_decode_info(audio_decoder_t *dec, audio_info_t *info);

// Read decoded samples
// dec: Decoder handle
// samples: Output buffer for interleaved 16-bit samples
// max_samples: Maximum number of samples to read (total, not per channel)
// Returns: Number of samples read (may be less than max_samples),
//          0 at end of stream, negative error code on failure
int audio_decode_read(audio_decoder_t *dec, int16_t *samples, int max_samples);

// Seek to sample position
// dec: Decoder handle
// sample: Target sample position (per channel)
// Note: Not all codecs support seeking
void audio_decode_seek(audio_decoder_t *dec, uint64_t sample);

// Check if seeking is supported
bool audio_decode_can_seek(audio_decoder_t *dec);

// Get current sample position
uint64_t audio_decode_tell(audio_decoder_t *dec);

// Close decoder and free resources
void audio_decode_close(audio_decoder_t *dec);

// ============================================================================
// Convenience Functions
// ============================================================================

// Decode entire file to buffer
// data: Pointer to compressed audio data
// size: Size of data in bytes
// out_samples: Output pointer to allocated sample buffer (caller must free)
// out_count: Output number of samples decoded
// info: Output audio information
// Returns: DECODE_OK on success, error code on failure
int audio_decode_entire(const void *data, size_t size,
                        int16_t **out_samples, uint64_t *out_count,
                        audio_info_t *info);

// Play audio data using the audio subsystem
// data: Pointer to compressed audio data
// size: Size of data in bytes
// Returns: DECODE_OK on success, error code on failure
int audio_decode_play(const void *data, size_t size);

// ============================================================================
// Codec Registration (Internal)
// ============================================================================

// Codec-specific decoder interface
typedef struct {
    audio_codec_t codec;
    const char *name;
    const char *extensions;  // e.g., "mp3"

    // Check if this codec can decode the data
    bool (*can_decode)(const void *data, size_t size);

    // Create decoder instance
    void *(*create)(const void *data, size_t size);

    // Destroy decoder instance
    void (*destroy)(void *ctx);

    // Get audio info
    int (*get_info)(void *ctx, audio_info_t *info);

    // Decode samples (returns count or negative error)
    int (*decode)(void *ctx, int16_t *samples, int max_samples);

    // Seek to sample position
    void (*seek)(void *ctx, uint64_t sample);

    // Check if seeking is supported
    bool (*can_seek)(void *ctx);

    // Get current position
    uint64_t (*tell)(void *ctx);
} audio_codec_ops_t;

// Register a codec implementation
void audio_decode_register(const audio_codec_ops_t *ops);

// Initialize all built-in codecs
void audio_decode_init(void);

#endif // AUDIO_DECODE_H
