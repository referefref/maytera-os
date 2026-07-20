// audio.h - MayteraOS Audio Subsystem API
// Unified audio interface supporting AC97, Intel HDA, and Sound Blaster backends
//
// Design Philosophy:
// - Clean abstraction over different audio hardware
// - DMA-based buffer management for efficient streaming
// - Sample rate conversion for compatibility
// - Production-grade error handling

#ifndef AUDIO_H
#define AUDIO_H

#include "../types.h"

// ============================================================================
// Audio Format Definitions
// ============================================================================

// Sample formats (bitmask)
#define AUDIO_FORMAT_U8       0x0001  // Unsigned 8-bit
#define AUDIO_FORMAT_S16_LE   0x0002  // Signed 16-bit little-endian
#define AUDIO_FORMAT_S16_BE   0x0004  // Signed 16-bit big-endian
#define AUDIO_FORMAT_S24_LE   0x0008  // Signed 24-bit little-endian (in 32-bit container)
#define AUDIO_FORMAT_S32_LE   0x0010  // Signed 32-bit little-endian
#define AUDIO_FORMAT_FLOAT32  0x0020  // 32-bit floating point

// Common sample rates
#define AUDIO_RATE_8000       8000
#define AUDIO_RATE_11025      11025
#define AUDIO_RATE_16000      16000
#define AUDIO_RATE_22050      22050
#define AUDIO_RATE_32000      32000
#define AUDIO_RATE_44100      44100
#define AUDIO_RATE_48000      48000
#define AUDIO_RATE_96000      96000

// Channel configurations
#define AUDIO_CHANNELS_MONO   1
#define AUDIO_CHANNELS_STEREO 2
#define AUDIO_CHANNELS_QUAD   4
#define AUDIO_CHANNELS_51     6

// ============================================================================
// Audio Device Types
// ============================================================================

typedef enum {
    AUDIO_DEVICE_NONE = 0,
    AUDIO_DEVICE_AC97,          // Intel AC97 codec
    AUDIO_DEVICE_HDA,           // Intel High Definition Audio
    AUDIO_DEVICE_SB16,          // Sound Blaster 16 (legacy)
    AUDIO_DEVICE_PCSPK,         // PC Speaker (beep only)
    AUDIO_DEVICE_USB            // #329: USB Audio Class DAC (NuForce, iso stream)
} audio_device_type_t;

// ============================================================================
// Error Codes
// ============================================================================

#define AUDIO_OK                    0
#define AUDIO_ERR_NOT_INITIALIZED   -1
#define AUDIO_ERR_NO_DEVICE         -2
#define AUDIO_ERR_INVALID_FORMAT    -3
#define AUDIO_ERR_INVALID_RATE      -4
#define AUDIO_ERR_INVALID_CHANNELS  -5
#define AUDIO_ERR_BUFFER_FULL       -6
#define AUDIO_ERR_BUFFER_EMPTY      -7
#define AUDIO_ERR_DMA_ERROR         -8
#define AUDIO_ERR_TIMEOUT           -9
#define AUDIO_ERR_BUSY              -10
#define AUDIO_ERR_NOT_SUPPORTED     -11
#define AUDIO_ERR_NO_MEMORY         -12
#define AUDIO_ERR_INVALID_PARAM     -13

// ============================================================================
// Audio Stream State
// ============================================================================

typedef enum {
    AUDIO_STATE_CLOSED = 0,
    AUDIO_STATE_STOPPED,
    AUDIO_STATE_PLAYING,
    AUDIO_STATE_PAUSED,
    AUDIO_STATE_DRAINING
} audio_state_t;

// ============================================================================
// Audio Stream Configuration
// ============================================================================

typedef struct {
    uint32_t format;        // Sample format (AUDIO_FORMAT_*)
    uint32_t sample_rate;   // Sample rate in Hz
    uint32_t channels;      // Number of channels
    uint32_t buffer_size;   // Preferred buffer size in frames (0 = auto)
    uint32_t period_size;   // Preferred period size in frames (0 = auto)
} audio_config_t;

// ============================================================================
// Audio Stream Information
// ============================================================================

typedef struct {
    audio_state_t state;
    uint32_t format;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t buffer_size;       // Total buffer size in frames
    uint32_t buffer_avail;      // Available space in frames
    uint32_t bytes_per_frame;   // Bytes per complete frame (all channels)
    uint64_t frames_played;     // Total frames played since open
    uint64_t underruns;         // Number of buffer underruns
} audio_stream_info_t;

// ============================================================================
// Audio Device Information
// ============================================================================

typedef struct {
    audio_device_type_t type;
    const char *name;
    const char *description;
    uint32_t supported_formats;    // Bitmask of supported formats
    uint32_t min_sample_rate;
    uint32_t max_sample_rate;
    uint32_t max_channels;
    bool supports_mixing;          // Hardware mixing support
    bool supports_src;             // Hardware sample rate conversion
} audio_device_info_t;

// ============================================================================
// Volume Control
// ============================================================================

// Volume is 0-100 (percentage), or AUDIO_VOLUME_MUTE
#define AUDIO_VOLUME_MIN    0
#define AUDIO_VOLUME_MAX    100
#define AUDIO_VOLUME_MUTE   (-1)

typedef struct {
    int master_left;    // Master volume left (0-100, or MUTE)
    int master_right;   // Master volume right
    int pcm_left;       // PCM/DAC volume left
    int pcm_right;      // PCM/DAC volume right
    bool master_mute;
    bool pcm_mute;
} audio_volume_t;

// ============================================================================
// Audio Stream Handle
// ============================================================================

// Opaque stream handle
typedef struct audio_stream audio_stream_t;

// ============================================================================
// Callback Types
// ============================================================================

// Called when more data is needed (can be called from interrupt context!)
// Return: number of frames actually written to buffer
typedef uint32_t (*audio_callback_t)(void *buffer, uint32_t frames, void *user_data);

// ============================================================================
// Public API - Initialization
// ============================================================================

// Initialize the audio subsystem
// Probes for available audio hardware and initializes the best available device
// Returns: AUDIO_OK on success, error code on failure
int audio_init(void);

// #702: start audio_init() (+ #71 HDA MSI arming) from a low-priority
// background worker instead of calling audio_init() synchronously. Safe to
// call once, right after proc_init()/the scheduler are up; idempotent. See
// the contract comment above audio_init_worker() in audio.c.
void audio_start_deferred_init(void);

// Shutdown the audio subsystem
void audio_shutdown(void);

// Check if audio is available
bool audio_is_available(void);

// Get information about the current audio device
int audio_get_device_info(audio_device_info_t *info);

// ============================================================================
// Public API - Stream Management
// ============================================================================

// Open an audio stream for playback
// config: desired configuration (will be adjusted to supported values)
// Returns: stream handle on success, NULL on failure
audio_stream_t *audio_open(audio_config_t *config);

// Close an audio stream
void audio_close(audio_stream_t *stream);

// Get stream information
int audio_get_stream_info(audio_stream_t *stream, audio_stream_info_t *info);

// ============================================================================
// Public API - Playback Control
// ============================================================================

// Start playback (after data has been written)
int audio_start(audio_stream_t *stream);

// Stop playback immediately
int audio_stop(audio_stream_t *stream);

// Pause playback (can be resumed)
int audio_pause(audio_stream_t *stream);

// Resume paused playback
int audio_resume(audio_stream_t *stream);

// Drain: wait for all buffered data to play, then stop
int audio_drain(audio_stream_t *stream);

// ============================================================================
// Public API - Data Transfer
// ============================================================================

// Write audio data to stream (blocking)
// buffer: PCM audio data in stream format
// frames: number of complete frames to write
// Returns: number of frames written, or negative error code
int audio_write(audio_stream_t *stream, const void *buffer, uint32_t frames);

// Write audio data (non-blocking)
// Returns immediately, may write fewer frames than requested
int audio_write_nonblock(audio_stream_t *stream, const void *buffer, uint32_t frames);

// Get number of frames that can be written without blocking
int audio_avail(audio_stream_t *stream);

// ============================================================================
// Public API - Callback Mode
// ============================================================================

// Set a callback for automatic buffer refill
// The callback will be called when the hardware needs more data
// callback: function to call, or NULL to disable
// user_data: passed to callback
int audio_set_callback(audio_stream_t *stream, audio_callback_t callback, void *user_data);

// ============================================================================
// Public API - Volume Control
// ============================================================================

// Get current volume levels
int audio_get_volume(audio_volume_t *vol);

// Set volume levels
int audio_set_volume(const audio_volume_t *vol);

// Simple volume control (sets both master and PCM, both channels)
int audio_set_master_volume(int volume);  // 0-100

// Mute/unmute
int audio_mute(bool mute);

// ============================================================================
// Public API - Simple Playback Helpers
// ============================================================================

// Play a buffer of PCM data (blocks until complete)
// This is a convenience wrapper that opens a stream, writes all data, and closes
int audio_play_file(const char *path);
int audio_play_file_async(const char *path);
int audio_play_buffer(const void *data, uint32_t size, 
                      uint32_t format, uint32_t sample_rate, uint32_t channels);

// Play a simple beep/tone using the best available method
// frequency: Hz (20-20000)
// duration_ms: milliseconds
void audio_beep(uint32_t frequency, uint32_t duration_ms);

// ============================================================================
// Public API - Sample Rate Conversion
// ============================================================================

// Convert sample rate (software resampling)
// src_data: source buffer
// src_frames: number of source frames
// src_rate: source sample rate
// dst_data: destination buffer (must be pre-allocated)
// dst_rate: destination sample rate
// channels: number of channels
// Returns: number of frames written to dst_data
uint32_t audio_resample(const int16_t *src_data, uint32_t src_frames, uint32_t src_rate,
                        int16_t *dst_data, uint32_t dst_rate, uint32_t channels);

// ============================================================================
// Debug/Information
// ============================================================================

// Print audio subsystem information
void audio_print_info(void);

// Get error string for error code
const char *audio_strerror(int error);

#endif // AUDIO_H
