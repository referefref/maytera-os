// audio_decode.c - MayteraOS Unified Audio Decoder Implementation
//
// Provides format detection and dispatch to codec-specific decoders

#include "audio_decode.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../drivers/audio.h"

// ============================================================================
// Forward declarations for codec implementations
// ============================================================================

extern const audio_codec_ops_t mp3_codec_ops;
extern const audio_codec_ops_t flac_codec_ops;
extern const audio_codec_ops_t vorbis_codec_ops;
extern const audio_codec_ops_t wav_codec_ops;
extern const audio_codec_ops_t opus_codec_ops;
extern const audio_codec_ops_t aac_codec_ops;

// ============================================================================
// Codec Registry
// ============================================================================

#define MAX_CODECS 8
static const audio_codec_ops_t *registered_codecs[MAX_CODECS];
static int num_codecs = 0;

// ============================================================================
// Decoder Structure
// ============================================================================

struct audio_decoder {
    const audio_codec_ops_t *ops;   // Codec operations
    void *ctx;                       // Codec-specific context
    audio_info_t info;              // Cached audio info
    bool info_valid;                // True if info has been retrieved
};

// ============================================================================
// Format Detection Magic Numbers
// ============================================================================

// MP3 frame sync word (11 bits set)
#define MP3_SYNC_WORD       0xFFE0

// FLAC magic "fLaC"
#define FLAC_MAGIC          0x664C6143

// OGG magic "OggS"
#define OGG_MAGIC           0x4F676753

// RIFF/WAV magic
#define RIFF_MAGIC          0x46464952

// ============================================================================
// Helper Functions
// ============================================================================

static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint16_t read_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

// ============================================================================
// Codec Registration
// ============================================================================

void audio_decode_register(const audio_codec_ops_t *ops) {
    if (num_codecs < MAX_CODECS && ops) {
        registered_codecs[num_codecs++] = ops;
        kprintf("[AudioDecode] Registered codec: %s\n", ops->name);
    }
}

void audio_decode_init(void) {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

    kprintf("[AudioDecode] Initializing audio decoder subsystem...\n");

    // Register built-in codecs.
    // MP3 (#236): real fixed-point decode via vendored libmad (media/libmad/),
    // FPM_64BIT integer path, verified bit-exact vs an ffmpeg/libmad reference.
    audio_decode_register(&mp3_codec_ops);
    audio_decode_register(&vorbis_codec_ops);  // OGG Vorbis via Tremor (#236)
    // #331: FLAC (lossless) via vendored dr_flac, integer s16 output path
    // (no FPU; objdump-verified zero FP/SIMD). See media/flac.c + media/dr_flac/.
    audio_decode_register(&flac_codec_ops);    // FLAC via dr_flac (#331)
    // #331: WAV (uncompressed RIFF/WAVE PCM), integer sample conversion.
    audio_decode_register(&wav_codec_ops);     // WAV (#331)
    // #331: Opus (Ogg-Opus) via vendored libopus, fixed-point integer decode
    // (no FPU; objdump-verified zero FP/SIMD). 48 kHz int16 output.
    audio_decode_register(&opus_codec_ops);    // Opus (#331)
    // #331: AAC / M4A via vendored faad2 (fixed-point, LC-only; no FPU;
    // objdump-verified zero FP/SIMD). GPLv2. ADTS .aac + MP4/M4A container.
    audio_decode_register(&aac_codec_ops);     // AAC / M4A (#331)

    kprintf("[AudioDecode] Registered %d codecs\n", num_codecs);
}

// ============================================================================
// Format Detection
// ============================================================================

audio_codec_t audio_detect_format(const void *data, size_t size) {
    if (!data || size < 4) {
        return AUDIO_CODEC_UNKNOWN;
    }

    const uint8_t *bytes = (const uint8_t *)data;

    // Check for RIFF/WAV
    if (size >= 12) {
        uint32_t riff = read_le32(bytes);
        uint32_t wave = read_le32(bytes + 8);
        if (riff == RIFF_MAGIC && wave == 0x45564157) { // "WAVE"
            return AUDIO_CODEC_WAV;
        }
    }

    // Check for FLAC ("fLaC")
    if (size >= 4) {
        if (bytes[0] == 'f' && bytes[1] == 'L' &&
            bytes[2] == 'a' && bytes[3] == 'C') {
            return AUDIO_CODEC_FLAC;
        }
    }

    // Check for OGG ("OggS"). Both Vorbis and Opus use the Ogg container, so
    // peek at the first page's BOS packet: Ogg-Opus begins with "OpusHead".
    if (size >= 4) {
        if (bytes[0] == 'O' && bytes[1] == 'g' &&
            bytes[2] == 'g' && bytes[3] == 'S') {
            if (size >= 27) {
                size_t payload = 27 + (size_t)bytes[26];
                if (payload + 8 <= size &&
                    bytes[payload+0]=='O' && bytes[payload+1]=='p' &&
                    bytes[payload+2]=='u' && bytes[payload+3]=='s' &&
                    bytes[payload+4]=='H' && bytes[payload+5]=='e' &&
                    bytes[payload+6]=='a' && bytes[payload+7]=='d') {
                    return AUDIO_CODEC_OPUS;
                }
            }
            return AUDIO_CODEC_OGG_VORBIS;
        }
    }

    // Check for MP4 / M4A (ISO-BMFF): 'ftyp' box type at offset 4. #331
    if (size >= 12) {
        if (bytes[4] == 'f' && bytes[5] == 't' &&
            bytes[6] == 'y' && bytes[7] == 'p') {
            return AUDIO_CODEC_AAC;
        }
    }

    // Check for raw ADTS AAC (.aac): syncword 0xFFF + layer 00. Must come
    // before the MP3 frame-sync test (ADTS 0xFFF1 also has 0xFFE0 set). #331
    if (size >= 7) {
        if (bytes[0] == 0xFF && (bytes[1] & 0xF6) == 0xF0) {
            return AUDIO_CODEC_AAC;
        }
    }

    // Check for MP3 (ID3 tag or frame sync)
    if (size >= 3) {
        // ID3v2 tag
        if (bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3') {
            return AUDIO_CODEC_MP3;
        }
    }

    // Check for MP3 frame sync
    if (size >= 2) {
        // Frame sync: 11 bits set (0xFF followed by 0xE0-0xFF)
        if (bytes[0] == 0xFF && (bytes[1] & 0xE0) == 0xE0) {
            // Additional validation: check for valid MPEG version and layer
            uint8_t version = (bytes[1] >> 3) & 0x03;
            uint8_t layer = (bytes[1] >> 1) & 0x03;
            // Version 01 is reserved, layer 00 is reserved
            if (version != 0x01 && layer != 0x00) {
                return AUDIO_CODEC_MP3;
            }
        }
    }

    // Check registered codecs
    for (int i = 0; i < num_codecs; i++) {
        if (registered_codecs[i]->can_decode &&
            registered_codecs[i]->can_decode(data, size)) {
            return registered_codecs[i]->codec;
        }
    }

    return AUDIO_CODEC_UNKNOWN;
}

const char *audio_codec_name(audio_codec_t codec) {
    switch (codec) {
        case AUDIO_CODEC_WAV:        return "WAV";
        case AUDIO_CODEC_MP3:        return "MP3";
        case AUDIO_CODEC_FLAC:       return "FLAC";
        case AUDIO_CODEC_OGG_VORBIS: return "OGG Vorbis";
        case AUDIO_CODEC_AAC:        return "AAC";
        case AUDIO_CODEC_OPUS:       return "Opus";
        default:                     return "Unknown";
    }
}

// ============================================================================
// Decoder Management
// ============================================================================

audio_decoder_t *audio_decode_open(const void *data, size_t size) {
    if (!data || size == 0) {
        return NULL;
    }

    // Initialize codec registry if needed
    audio_decode_init();

    // Detect format
    audio_codec_t codec = audio_detect_format(data, size);
    if (codec == AUDIO_CODEC_UNKNOWN) {
        kprintf("[AudioDecode] Unknown audio format\n");
        return NULL;
    }

    kprintf("[AudioDecode] Detected format: %s\n", audio_codec_name(codec));

    // Find matching codec
    const audio_codec_ops_t *ops = NULL;
    for (int i = 0; i < num_codecs; i++) {
        if (registered_codecs[i]->codec == codec) {
            ops = registered_codecs[i];
            break;
        }
    }

    if (!ops || !ops->create) {
        kprintf("[AudioDecode] No decoder available for %s\n", audio_codec_name(codec));
        return NULL;
    }

    // Create codec-specific context
    void *ctx = ops->create(data, size);
    if (!ctx) {
        kprintf("[AudioDecode] Failed to create decoder context\n");
        return NULL;
    }

    // Allocate decoder handle
    audio_decoder_t *dec = kzalloc(sizeof(audio_decoder_t));
    if (!dec) {
        if (ops->destroy) {
            ops->destroy(ctx);
        }
        return NULL;
    }

    dec->ops = ops;
    dec->ctx = ctx;
    dec->info_valid = false;

    return dec;
}

int audio_decode_info(audio_decoder_t *dec, audio_info_t *info) {
    if (!dec || !info) {
        return DECODE_ERR_INVALID_DATA;
    }

    // Use cached info if available
    if (dec->info_valid) {
        *info = dec->info;
        return DECODE_OK;
    }

    // Get info from codec
    if (!dec->ops->get_info) {
        return DECODE_ERR_UNSUPPORTED;
    }

    int ret = dec->ops->get_info(dec->ctx, &dec->info);
    if (ret == DECODE_OK) {
        dec->info_valid = true;
        *info = dec->info;
    }

    return ret;
}

int audio_decode_read(audio_decoder_t *dec, int16_t *samples, int max_samples) {
    if (!dec || !samples || max_samples <= 0) {
        return DECODE_ERR_INVALID_DATA;
    }

    if (!dec->ops->decode) {
        return DECODE_ERR_UNSUPPORTED;
    }

    return dec->ops->decode(dec->ctx, samples, max_samples);
}

void audio_decode_seek(audio_decoder_t *dec, uint64_t sample) {
    if (!dec || !dec->ops->seek) {
        return;
    }

    dec->ops->seek(dec->ctx, sample);
}

bool audio_decode_can_seek(audio_decoder_t *dec) {
    if (!dec || !dec->ops->can_seek) {
        return false;
    }

    return dec->ops->can_seek(dec->ctx);
}

uint64_t audio_decode_tell(audio_decoder_t *dec) {
    if (!dec || !dec->ops->tell) {
        return 0;
    }

    return dec->ops->tell(dec->ctx);
}

void audio_decode_close(audio_decoder_t *dec) {
    if (!dec) {
        return;
    }

    if (dec->ops && dec->ops->destroy && dec->ctx) {
        dec->ops->destroy(dec->ctx);
    }

    kfree(dec);
}

// ============================================================================
// Convenience Functions
// ============================================================================

int audio_decode_entire(const void *data, size_t size,
                        int16_t **out_samples, uint64_t *out_count,
                        audio_info_t *info) {
    if (!data || !out_samples || !out_count || !info) {
        return DECODE_ERR_INVALID_DATA;
    }

    *out_samples = NULL;
    *out_count = 0;

    // Open decoder
    audio_decoder_t *dec = audio_decode_open(data, size);
    if (!dec) {
        return DECODE_ERR_INVALID_DATA;
    }

    // Get info
    int ret = audio_decode_info(dec, info);
    if (ret != DECODE_OK) {
        audio_decode_close(dec);
        return ret;
    }

    // Calculate buffer size
    uint64_t total_samples = info->total_samples * info->channels;
    if (total_samples == 0) {
        // Unknown size, use streaming decode
        total_samples = info->sample_rate * info->channels * 600; // Max 10 minutes
    }

    // Allocate output buffer
    int16_t *samples = kmalloc(total_samples * sizeof(int16_t));
    if (!samples) {
        audio_decode_close(dec);
        return DECODE_ERR_NO_MEMORY;
    }

    // Decode all samples
    uint64_t decoded = 0;
    int16_t *ptr = samples;
    int remaining = (int)(total_samples - decoded);

    while (remaining > 0) {
        int chunk_size = (remaining > 8192) ? 8192 : remaining;
        int count = audio_decode_read(dec, ptr, chunk_size);

        if (count <= 0) {
            break;
        }

        decoded += count;
        ptr += count;
        remaining = (int)(total_samples - decoded);
    }

    audio_decode_close(dec);

    *out_samples = samples;
    *out_count = decoded;

    return DECODE_OK;
}

int audio_decode_play(const void *data, size_t size) {
    if (!data || size == 0) {
        return DECODE_ERR_INVALID_DATA;
    }

    // Open decoder
    audio_decoder_t *dec = audio_decode_open(data, size);
    if (!dec) {
        return DECODE_ERR_INVALID_DATA;
    }

    // Get audio info
    audio_info_t info;
    int ret = audio_decode_info(dec, &info);
    if (ret != DECODE_OK) {
        audio_decode_close(dec);
        return ret;
    }

    kprintf("[AudioDecode] Playing: %u Hz, %u channels, %u ms\n",
            info.sample_rate, info.channels, info.duration_ms);

    // Open audio stream
    audio_config_t config = {
        .format = AUDIO_FORMAT_S16_LE,
        .sample_rate = info.sample_rate,
        .channels = info.channels,
        .buffer_size = 0,
        .period_size = 0
    };

    audio_stream_t *stream = audio_open(&config);
    if (!stream) {
        audio_decode_close(dec);
        return DECODE_ERR_INTERNAL;
    }

    // Decode and play in chunks
    int16_t *buffer = kmalloc(8192 * sizeof(int16_t));
    if (!buffer) {
        audio_close(stream);
        audio_decode_close(dec);
        return DECODE_ERR_NO_MEMORY;
    }

    audio_start(stream);

    int count;
    while ((count = audio_decode_read(dec, buffer, 8192)) > 0) {
        int frames = count / info.channels;
        audio_write(stream, buffer, frames);
    }

    // Wait for playback to complete
    audio_drain(stream);

    // Cleanup
    kfree(buffer);
    audio_close(stream);
    audio_decode_close(dec);

    return DECODE_OK;
}
