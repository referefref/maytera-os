// wav.c - WAV/RIFF Audio File Parser Implementation

#include "wav.h"
#include "audio.h"
#include "../serial.h"
#include "../string.h"

// ============================================================================
// Helper Functions
// ============================================================================

static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ============================================================================
// WAV Parser Implementation
// ============================================================================

int wav_parse(const void *file_data, uint32_t file_size, wav_info_t *info) {
    if (!file_data || !info || file_size < 44) {
        if (info) {
            info->valid = false;
            info->error = "Invalid parameters";
        }
        return AUDIO_ERR_INVALID_PARAM;
    }

    const uint8_t *data = (const uint8_t *)file_data;

    // Initialize info
    memset(info, 0, sizeof(wav_info_t));
    info->file_data = data;
    info->file_size = file_size;

    // Check RIFF header
    if (read_le32(data) != WAV_RIFF_ID) {
        info->error = "Not a RIFF file";
        return AUDIO_ERR_INVALID_FORMAT;
    }

    uint32_t riff_size = read_le32(data + 4);
    if (riff_size + 8 > file_size) {
        info->error = "Truncated RIFF file";
        return AUDIO_ERR_INVALID_FORMAT;
    }

    // Check WAVE format
    if (read_le32(data + 8) != WAV_WAVE_ID) {
        info->error = "Not a WAVE file";
        return AUDIO_ERR_INVALID_FORMAT;
    }

    // Parse chunks
    uint32_t offset = 12;  // After RIFF header
    bool found_fmt = false;
    bool found_data = false;

    while (offset + 8 <= file_size) {
        uint32_t chunk_id = read_le32(data + offset);
        uint32_t chunk_size = read_le32(data + offset + 4);

        if (offset + 8 + chunk_size > file_size) {
            info->error = "Chunk extends beyond file";
            return AUDIO_ERR_INVALID_FORMAT;
        }

        if (chunk_id == WAV_FMT_ID) {
            // Format chunk
            if (chunk_size < 16) {
                info->error = "Format chunk too small";
                return AUDIO_ERR_INVALID_FORMAT;
            }

            const uint8_t *fmt = data + offset + 8;
            info->audio_format = read_le16(fmt);
            info->num_channels = read_le16(fmt + 2);
            info->sample_rate = read_le32(fmt + 4);
            info->byte_rate = read_le32(fmt + 8);
            info->block_align = read_le16(fmt + 12);
            info->bits_per_sample = read_le16(fmt + 14);

            found_fmt = true;
        } else if (chunk_id == WAV_DATA_ID) {
            // Data chunk
            info->data_ptr = data + offset + 8;
            info->data_size = chunk_size;
            found_data = true;
        }
        // Skip other chunks (fact, LIST, etc.)

        // Move to next chunk (aligned to 2 bytes)
        offset += 8 + chunk_size;
        if (chunk_size & 1) offset++;  // Pad byte
    }

    if (!found_fmt) {
        info->error = "No format chunk found";
        return AUDIO_ERR_INVALID_FORMAT;
    }

    if (!found_data) {
        info->error = "No data chunk found";
        return AUDIO_ERR_INVALID_FORMAT;
    }

    // Validate format
    if (info->audio_format != WAV_FORMAT_PCM && 
        info->audio_format != WAV_FORMAT_IEEE_FLOAT) {
        info->error = "Unsupported audio format (only PCM supported)";
        return AUDIO_ERR_NOT_SUPPORTED;
    }

    if (info->num_channels == 0 || info->num_channels > 8) {
        info->error = "Invalid channel count";
        return AUDIO_ERR_INVALID_CHANNELS;
    }

    if (info->bits_per_sample != 8 && info->bits_per_sample != 16 &&
        info->bits_per_sample != 24 && info->bits_per_sample != 32) {
        info->error = "Unsupported bits per sample";
        return AUDIO_ERR_INVALID_FORMAT;
    }

    // Calculate derived values
    info->audio_format_api = wav_format_to_audio_format(info->audio_format, 
                                                         info->bits_per_sample);

    uint32_t bytes_per_sample = info->bits_per_sample / 8;
    uint32_t bytes_per_frame = bytes_per_sample * info->num_channels;

    if (bytes_per_frame > 0) {
        info->total_frames = info->data_size / bytes_per_frame;
        info->total_samples = info->total_frames;
    }

    if (info->sample_rate > 0) {
        info->duration_ms = (info->total_frames * 1000) / info->sample_rate;
    }

    info->valid = true;
    info->error = NULL;

    return AUDIO_OK;
}

bool wav_validate(const wav_info_t *info) {
    if (!info || !info->valid) {
        return false;
    }

    // Check for supported formats
    if (info->audio_format != WAV_FORMAT_PCM) {
        return false;
    }

    if (info->bits_per_sample != 8 && info->bits_per_sample != 16 &&
        info->bits_per_sample != 24 && info->bits_per_sample != 32) {
        return false;
    }

    if (info->sample_rate < 8000 || info->sample_rate > 192000) {
        return false;
    }

    if (info->num_channels == 0 || info->num_channels > 8) {
        return false;
    }

    return true;
}

uint32_t wav_format_to_audio_format(uint16_t wav_format, uint16_t bits_per_sample) {
    if (wav_format == WAV_FORMAT_IEEE_FLOAT) {
        return AUDIO_FORMAT_FLOAT32;
    }

    // PCM format
    switch (bits_per_sample) {
        case 8:
            return AUDIO_FORMAT_U8;
        case 16:
            return AUDIO_FORMAT_S16_LE;
        case 24:
            return AUDIO_FORMAT_S24_LE;
        case 32:
            return AUDIO_FORMAT_S32_LE;
        default:
            return AUDIO_FORMAT_S16_LE;
    }
}

int wav_get_audio_config(const wav_info_t *info, audio_config_t *config) {
    if (!info || !config || !info->valid) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    config->format = info->audio_format_api;
    config->sample_rate = info->sample_rate;
    config->channels = info->num_channels;
    config->buffer_size = 0;  // Auto
    config->period_size = 0;  // Auto

    return AUDIO_OK;
}

int wav_play(const void *file_data, uint32_t file_size) {
    wav_info_t info;

    // Parse WAV file
    int ret = wav_parse(file_data, file_size, &info);
    if (ret != AUDIO_OK) {
        kprintf("[WAV] Parse error: %s\n", info.error);
        return ret;
    }

    // Validate format
    if (!wav_validate(&info)) {
        kprintf("[WAV] Unsupported format\n");
        return AUDIO_ERR_NOT_SUPPORTED;
    }

    kprintf("[WAV] Playing: %u Hz, %u-bit, %u channels, %u ms\n",
            info.sample_rate, info.bits_per_sample, 
            info.num_channels, info.duration_ms);

    // Get audio configuration
    audio_config_t config = {0};
    wav_get_audio_config(&info, &config);

    // Play using audio subsystem
    return audio_play_buffer(info.data_ptr, info.data_size,
                             config.format, config.sample_rate, config.channels);
}

void wav_print_info(const wav_info_t *info) {
    if (!info) {
        kprintf("[WAV] NULL info\n");
        return;
    }

    kprintf("\n[WAV] File Information:\n");
    kprintf("  Valid:        %s\n", info->valid ? "Yes" : "No");

    if (!info->valid) {
        kprintf("  Error:        %s\n", info->error ? info->error : "Unknown");
        return;
    }

    kprintf("  Format:       ");
    switch (info->audio_format) {
        case WAV_FORMAT_PCM: kprintf("PCM\n"); break;
        case WAV_FORMAT_IEEE_FLOAT: kprintf("IEEE Float\n"); break;
        case WAV_FORMAT_ALAW: kprintf("A-law\n"); break;
        case WAV_FORMAT_MULAW: kprintf("Mu-law\n"); break;
        default: kprintf("Unknown (0x%04x)\n", info->audio_format); break;
    }

    kprintf("  Channels:     %u\n", info->num_channels);
    kprintf("  Sample Rate:  %u Hz\n", info->sample_rate);
    kprintf("  Bits/Sample:  %u\n", info->bits_per_sample);
    kprintf("  Byte Rate:    %u bytes/sec\n", info->byte_rate);
    kprintf("  Block Align:  %u bytes\n", info->block_align);
    kprintf("  Data Size:    %u bytes\n", info->data_size);
    kprintf("  Duration:     %u.%03u seconds\n", 
            info->duration_ms / 1000, info->duration_ms % 1000);
    kprintf("  Total Frames: %u\n", info->total_frames);
}
