// Suppress warnings
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-function-declaration"
// video_decode.c - Video Decoder Implementation for MayteraOS
// Format detection and common decoder dispatch
#include "video_decode.h"
#include "mpeg.h"
#include "avi.h"
#include "../mm/heap.h"
#include "../string.h"
#include "../serial.h"

// Internal decoder structure
struct video_decoder {
    int format;                     // VIDEO_FORMAT_xxx
    const void *data;               // Original data pointer
    size_t size;                    // Data size

    // Format-specific decoder
    union {
        mpeg_decoder_t *mpeg;       // MPEG-1 decoder
        avi_decoder_t *avi;         // AVI decoder
    } ctx;

    // Cached video info
    video_info_t info;
    bool info_valid;

    // Playback state
    double current_time;
    bool ended;
};

// Format signatures
#define MPEG_PACK_START     0x000001BA
#define MPEG_SYSTEM_START   0x000001BB
#define MPEG_VIDEO_START    0x000001E0
#define AVI_RIFF_SIG        0x46464952  // "RIFF"
#define AVI_AVI_SIG         0x20495641  // "AVI "
#define MP4_FTYP_SIG        0x70797466  // "ftyp"

// Helper: read big-endian 32-bit value
static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Helper: read little-endian 32-bit value
static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Detect video format from data
int video_detect_format(const void *data, size_t size) {
    if (!data || size < 12) {
        return VIDEO_FORMAT_UNKNOWN;
    }

    const uint8_t *p = (const uint8_t *)data;

    // Check for MPEG Program Stream (starts with pack header 0x000001BA)
    uint32_t start_code = read_be32(p);
    if (start_code == MPEG_PACK_START || start_code == MPEG_SYSTEM_START) {
        return VIDEO_FORMAT_MPEG1;
    }

    // Also check for raw MPEG video stream (starts with sequence header 0x000001B3)
    if ((start_code & 0xFFFFFF00) == 0x00000100) {
        uint8_t type = start_code & 0xFF;
        if (type == 0xB3 || type == 0xBA || type == 0xBB || type == 0xE0) {
            return VIDEO_FORMAT_MPEG1;
        }
    }

    // Check for AVI container ("RIFF" followed by "AVI ")
    uint32_t riff = read_le32(p);
    if (riff == AVI_RIFF_SIG && size >= 12) {
        uint32_t avi = read_le32(p + 8);
        if (avi == AVI_AVI_SIG) {
            return VIDEO_FORMAT_AVI;
        }
    }

    // Check for MP4/MOV (ftyp box)
    // "ftyp" at offset 4 or size at offset 0 followed by "ftyp"
    if (size >= 8) {
        uint32_t box_type = read_le32(p + 4);
        // Note: MP4 uses big-endian, but we read "ftyp" which happens to work
        if (p[4] == 'f' && p[5] == 't' && p[6] == 'y' && p[7] == 'p') {
            return VIDEO_FORMAT_MP4;  // Detected but not fully supported yet
        }
    }

    return VIDEO_FORMAT_UNKNOWN;
}

// Get format name string
const char *video_format_name(int format) {
    switch (format) {
        case VIDEO_FORMAT_MPEG1:    return "MPEG-1 Program Stream";
        case VIDEO_FORMAT_AVI:      return "AVI Container";
        case VIDEO_FORMAT_MP4:      return "MP4/MOV Container";
        default:                    return "Unknown Format";
    }
}

// Get error string
const char *video_error_string(int error) {
    switch (error) {
        case VIDEO_SUCCESS:         return "Success";
        case VIDEO_ERR_NULL_PTR:    return "Null pointer";
        case VIDEO_ERR_INVALID_FMT: return "Invalid format";
        case VIDEO_ERR_NOMEM:       return "Out of memory";
        case VIDEO_ERR_CORRUPT:     return "Corrupt data";
        case VIDEO_ERR_UNSUPPORTED: return "Unsupported feature";
        case VIDEO_ERR_EOF:         return "End of file";
        case VIDEO_ERR_NO_FRAME:    return "No frame available";
        case VIDEO_ERR_NO_AUDIO:    return "No audio track";
        default:                    return "Unknown error";
    }
}

// Open video decoder
video_decoder_t *video_decode_open(const void *data, size_t size) {
    if (!data || size < 12) {
        kprintf("[VIDEO] video_decode_open: invalid parameters\n");
        return NULL;
    }

    // Detect format
    int format = video_detect_format(data, size);
    if (format == VIDEO_FORMAT_UNKNOWN) {
        kprintf("[VIDEO] video_decode_open: unknown format\n");
        return NULL;
    }

    kprintf("[VIDEO] Detected format: %s\n", video_format_name(format));

    // Allocate decoder structure
    video_decoder_t *dec = kzalloc(sizeof(video_decoder_t));
    if (!dec) {
        kprintf("[VIDEO] video_decode_open: out of memory\n");
        return NULL;
    }

    dec->format = format;
    dec->data = data;
    dec->size = size;
    dec->current_time = 0.0;
    dec->ended = false;
    dec->info_valid = false;

    // Initialize format-specific decoder
    int result = VIDEO_ERR_UNSUPPORTED;

    switch (format) {
        case VIDEO_FORMAT_MPEG1:
            dec->ctx.mpeg = mpeg_decoder_open(data, size);
            if (dec->ctx.mpeg) {
                result = VIDEO_SUCCESS;
            }
            break;

        case VIDEO_FORMAT_AVI:
            dec->ctx.avi = avi_decoder_open(data, size);
            if (dec->ctx.avi) {
                result = VIDEO_SUCCESS;
            }
            break;

        case VIDEO_FORMAT_MP4:
            kprintf("[VIDEO] MP4 format not yet fully supported\n");
            result = VIDEO_ERR_UNSUPPORTED;
            break;

        default:
            break;
    }

    if (result != VIDEO_SUCCESS) {
        kfree(dec);
        return NULL;
    }

    kprintf("[VIDEO] Decoder opened successfully\n");
    return dec;
}

// Get video information
int video_decode_info(video_decoder_t *dec, video_info_t *info) {
    if (!dec || !info) {
        return VIDEO_ERR_NULL_PTR;
    }

    // Return cached info if available
    if (dec->info_valid) {
        memcpy(info, &dec->info, sizeof(video_info_t));
        return VIDEO_SUCCESS;
    }

    int result = VIDEO_ERR_UNSUPPORTED;

    switch (dec->format) {
        case VIDEO_FORMAT_MPEG1:
            if (dec->ctx.mpeg) {
                result = mpeg_decoder_info(dec->ctx.mpeg, info);
            }
            break;

        case VIDEO_FORMAT_AVI:
            if (dec->ctx.avi) {
                result = avi_decoder_info(dec->ctx.avi, info);
            }
            break;

        default:
            break;
    }

    if (result == VIDEO_SUCCESS) {
        info->format = dec->format;
        memcpy(&dec->info, info, sizeof(video_info_t));
        dec->info_valid = true;
    }

    return result;
}

// Decode next video frame
int video_decode_frame(video_decoder_t *dec, uint32_t *rgb_buffer) {
    if (!dec || !rgb_buffer) {
        return VIDEO_ERR_NULL_PTR;
    }

    if (dec->ended) {
        return VIDEO_ERR_EOF;
    }

    int result = VIDEO_ERR_UNSUPPORTED;

    switch (dec->format) {
        case VIDEO_FORMAT_MPEG1:
            if (dec->ctx.mpeg) {
                result = mpeg_decoder_frame(dec->ctx.mpeg, rgb_buffer);
                if (result == VIDEO_SUCCESS) {
                    dec->current_time = mpeg_decoder_get_time(dec->ctx.mpeg);
                } else if (result == VIDEO_ERR_EOF) {
                    dec->ended = true;
                }
            }
            break;

        case VIDEO_FORMAT_AVI:
            if (dec->ctx.avi) {
                result = avi_decoder_frame(dec->ctx.avi, rgb_buffer);
                if (result == VIDEO_SUCCESS) {
                    dec->current_time = avi_decoder_get_time(dec->ctx.avi);
                } else if (result == VIDEO_ERR_EOF) {
                    dec->ended = true;
                }
            }
            break;

        default:
            break;
    }

    return result;
}

// Decode audio samples
int video_decode_audio(video_decoder_t *dec, int16_t *samples, int max_samples) {
    if (!dec || !samples) {
        return VIDEO_ERR_NULL_PTR;
    }

    if (!dec->info_valid) {
        video_info_t info;
        video_decode_info(dec, &info);
    }

    if (!dec->info.has_audio) {
        return VIDEO_ERR_NO_AUDIO;
    }

    int result = VIDEO_ERR_UNSUPPORTED;

    switch (dec->format) {
        case VIDEO_FORMAT_MPEG1:
            if (dec->ctx.mpeg) {
                result = mpeg_decoder_audio(dec->ctx.mpeg, samples, max_samples);
            }
            break;

        case VIDEO_FORMAT_AVI:
            if (dec->ctx.avi) {
                result = avi_decoder_audio(dec->ctx.avi, samples, max_samples);
            }
            break;

        default:
            break;
    }

    return result;
}

// Seek to position
void video_decode_seek(video_decoder_t *dec, double seconds) {
    if (!dec) return;

    if (seconds < 0) seconds = 0;

    dec->ended = false;

    switch (dec->format) {
        case VIDEO_FORMAT_MPEG1:
            if (dec->ctx.mpeg) {
                mpeg_decoder_seek(dec->ctx.mpeg, seconds);
                dec->current_time = seconds;
            }
            break;

        case VIDEO_FORMAT_AVI:
            if (dec->ctx.avi) {
                avi_decoder_seek(dec->ctx.avi, seconds);
                dec->current_time = seconds;
            }
            break;

        default:
            break;
    }
}

// Get current time
int64_t video_decode_get_time(video_decoder_t *dec) {
    if (!dec) return 0.0;
    return dec->current_time;
}

// Check if ended
int video_decode_has_ended(video_decoder_t *dec) {
    if (!dec) return 1;
    return dec->ended ? 1 : 0;
}

// Rewind to beginning
void video_decode_rewind(video_decoder_t *dec) {
    video_decode_seek(dec, 0.0);
}

// Close decoder
void video_decode_close(video_decoder_t *dec) {
    if (!dec) return;

    kprintf("[VIDEO] Closing decoder\n");

    switch (dec->format) {
        case VIDEO_FORMAT_MPEG1:
            if (dec->ctx.mpeg) {
                mpeg_decoder_close(dec->ctx.mpeg);
            }
            break;

        case VIDEO_FORMAT_AVI:
            if (dec->ctx.avi) {
                avi_decoder_close(dec->ctx.avi);
            }
            break;

        default:
            break;
    }

    kfree(dec);
}
