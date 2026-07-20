// wav.h - WAV/RIFF Audio File Parser
//
// Supports standard PCM WAV files (RIFF/WAVE format)
// Commonly used for audio samples and sound effects

#ifndef WAV_H
#define WAV_H

#include "../types.h"
#include "audio.h"

// ============================================================================
// WAV File Format Constants
// ============================================================================

// RIFF chunk IDs (little-endian ASCII)
#define WAV_RIFF_ID         0x46464952  // "RIFF"
#define WAV_WAVE_ID         0x45564157  // "WAVE"
#define WAV_FMT_ID          0x20746D66  // "fmt "
#define WAV_DATA_ID         0x61746164  // "data"
#define WAV_FACT_ID         0x74636166  // "fact"
#define WAV_LIST_ID         0x5453494C  // "LIST"

// Audio format codes
#define WAV_FORMAT_PCM          0x0001  // Linear PCM
#define WAV_FORMAT_IEEE_FLOAT   0x0003  // IEEE float
#define WAV_FORMAT_ALAW         0x0006  // A-law
#define WAV_FORMAT_MULAW        0x0007  // Mu-law
#define WAV_FORMAT_EXTENSIBLE   0xFFFE  // Extensible format

// ============================================================================
// WAV File Structures
// ============================================================================

// RIFF chunk header
typedef struct {
    uint32_t chunk_id;      // "RIFF"
    uint32_t chunk_size;    // File size - 8
    uint32_t format;        // "WAVE"
} __attribute__((packed)) wav_riff_header_t;

// Format chunk (fmt )
typedef struct {
    uint32_t chunk_id;      // "fmt "
    uint32_t chunk_size;    // Format chunk size (16 for PCM)
    uint16_t audio_format;  // Audio format (1 = PCM)
    uint16_t num_channels;  // Number of channels
    uint32_t sample_rate;   // Sample rate (Hz)
    uint32_t byte_rate;     // Bytes per second
    uint16_t block_align;   // Bytes per sample (all channels)
    uint16_t bits_per_sample; // Bits per sample
    // Extended format fields follow for non-PCM formats
} __attribute__((packed)) wav_fmt_chunk_t;

// Data chunk header
typedef struct {
    uint32_t chunk_id;      // "data"
    uint32_t chunk_size;    // Data size in bytes
} __attribute__((packed)) wav_data_chunk_t;

// ============================================================================
// WAV File Info
// ============================================================================

typedef struct {
    // Format info
    uint16_t audio_format;      // WAV_FORMAT_*
    uint16_t num_channels;      // Number of channels
    uint32_t sample_rate;       // Sample rate (Hz)
    uint32_t byte_rate;         // Bytes per second
    uint16_t block_align;       // Bytes per frame
    uint16_t bits_per_sample;   // Bits per sample

    // Derived info
    uint32_t audio_format_api;  // AUDIO_FORMAT_* equivalent
    uint32_t total_samples;     // Total samples (per channel)
    uint32_t total_frames;      // Total frames
    uint32_t duration_ms;       // Duration in milliseconds

    // Data location
    const uint8_t *data_ptr;    // Pointer to PCM data
    uint32_t data_size;         // Size of PCM data in bytes

    // File location
    const uint8_t *file_data;   // Pointer to entire file
    uint32_t file_size;         // Size of file

    // Validity
    bool valid;                 // True if file parsed successfully
    const char *error;          // Error message if not valid
} wav_info_t;

// ============================================================================
// WAV Parser API
// ============================================================================

// Parse WAV file header
// file_data: Pointer to WAV file data in memory
// file_size: Size of file data
// info: Output structure for file information
// Returns: AUDIO_OK on success, error code on failure
int wav_parse(const void *file_data, uint32_t file_size, wav_info_t *info);

// Validate WAV file format
// Returns: true if format is supported
bool wav_validate(const wav_info_t *info);

// Convert WAV format code to AUDIO_FORMAT_*
uint32_t wav_format_to_audio_format(uint16_t wav_format, uint16_t bits_per_sample);

// Get audio config suitable for audio_open()
int wav_get_audio_config(const wav_info_t *info, audio_config_t *config);

// Play WAV file (convenience function)
// Parses and plays entire WAV file
// file_data: Pointer to WAV file in memory
// file_size: Size of file
// Returns: AUDIO_OK on success, error code on failure
int wav_play(const void *file_data, uint32_t file_size);

// Print WAV file information
void wav_print_info(const wav_info_t *info);

#endif // WAV_H
