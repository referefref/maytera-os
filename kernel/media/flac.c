// flac.c - MayteraOS FLAC decoder (real lossless decode via dr_flac).
//
// Implements the audio_codec_ops_t interface from audio_decode.h using dr_flac
// (mackron's single-file public-domain / MIT-0 FLAC decoder). FLAC is lossless
// and therefore integer; this wrapper uses ONLY dr_flac's integer s16 output
// path (drflac_read_pcm_frames_s16), which yields native-endian interleaved
// signed 16-bit PCM directly. The kernel is built -mno-sse / -mno-sse2 (no
// FPU), so the decoder must avoid all floating point; the vendored dr_flac is
// configured (drflac_config.h) to exclude its float (f32) output API and its
// float-using binary-search seek (DR_FLAC_NO_CRC), and to disable SSE/NEON, so
// the dr_flac objects contain zero FP/SIMD instructions (objdump-verified).
//
// Vendored sources live in media/dr_flac/ and are compiled with relaxed
// warnings + compat shims. Decoding works off an in-memory buffer via
// drflac_open_memory (no file I/O).
//
// License: dr_flac is public domain (Unlicense) or MIT-0, at your option. See
// media/dr_flac/COPYING and CHANGELOG.md. Source: dr_flac v0.13.4 from
// github.com/mackron/dr_libs.

#include "audio_decode.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"

// Pull in the dr_flac public API (declarations only; the implementation lives
// in media/dr_flac/dr_flac.c). The config header must precede dr_flac.h so the
// struct layout / codepaths match the implementation TU exactly.
#include "dr_flac/drflac_config.h"
#include "dr_flac/dr_flac.h"

// PCM scratch: 8192 interleaved int16 samples (16 KiB) per refill.
#define FLAC_PCM_SAMPLES 8192

typedef struct {
    uint8_t        *buf;            // owned copy of the compressed input
    size_t          buf_size;

    drflac         *flac;           // dr_flac decoder handle
    bool            opened;

    int             sample_rate;
    int             channels;
    int             bits_per_sample;
    uint64_t        total_samples;  // per channel (PCM frames)
    uint64_t        current_sample; // per channel

    int16_t         pcm[FLAC_PCM_SAMPLES]; // interleaved leftover
    int             pcm_len;        // valid interleaved samples
    int             pcm_pos;        // next interleaved sample to emit
    bool            eof;
} flac_ctx_t;

// ============================================================================
// Codec interface
// ============================================================================

static bool flac_can_decode(const void *data, size_t size) {
    if (!data || size < 4) return false;
    const uint8_t *b = (const uint8_t *)data;
    return b[0] == 'f' && b[1] == 'L' && b[2] == 'a' && b[3] == 'C';
}

static void *flac_create(const void *data, size_t size) {
    if (!data || size == 0) return NULL;

    flac_ctx_t *ctx = kzalloc(sizeof(flac_ctx_t));
    if (!ctx) return NULL;

    ctx->buf_size = size;
    ctx->buf = kmalloc(size);
    if (!ctx->buf) { kfree(ctx); return NULL; }
    memcpy(ctx->buf, data, size);

    ctx->flac = drflac_open_memory(ctx->buf, ctx->buf_size, NULL);
    if (!ctx->flac) {
        kprintf("[FLAC] drflac_open_memory failed\n");
        kfree(ctx->buf);
        kfree(ctx);
        return NULL;
    }
    ctx->opened = true;

    ctx->sample_rate     = (int)ctx->flac->sampleRate;
    ctx->channels        = (int)ctx->flac->channels;
    ctx->bits_per_sample = (int)ctx->flac->bitsPerSample;
    ctx->total_samples   = ctx->flac->totalPCMFrameCount;

    kprintf("[FLAC] dr_flac: %d Hz, %d ch, %d-bit, %llu frames\n",
            ctx->sample_rate, ctx->channels, ctx->bits_per_sample,
            (unsigned long long)ctx->total_samples);
    return ctx;
}

static void flac_destroy(void *context) {
    flac_ctx_t *ctx = (flac_ctx_t *)context;
    if (!ctx) return;
    if (ctx->opened) drflac_close(ctx->flac);
    if (ctx->buf) kfree(ctx->buf);
    kfree(ctx);
}

static int flac_get_info(void *context, audio_info_t *info) {
    flac_ctx_t *ctx = (flac_ctx_t *)context;
    if (!ctx || !info) return DECODE_ERR_INVALID_DATA;
    info->sample_rate     = ctx->sample_rate;
    info->channels        = ctx->channels;
    info->bits_per_sample = 16;   // we always emit 16-bit PCM
    info->total_samples   = ctx->total_samples;
    // FLAC is lossless; report the uncompressed PCM bitrate as an estimate.
    info->bitrate = (uint32_t)(ctx->sample_rate * ctx->channels * 16);
    info->codec   = AUDIO_CODEC_FLAC;
    info->duration_ms = (ctx->sample_rate && ctx->total_samples)
        ? (uint32_t)(ctx->total_samples * 1000 / ctx->sample_rate) : 0;
    return DECODE_OK;
}

// Read up to max_samples interleaved int16 samples. Returns count, 0 at EOF.
static int flac_decode(void *context, int16_t *samples, int max_samples) {
    flac_ctx_t *ctx = (flac_ctx_t *)context;
    if (!ctx || !samples || max_samples <= 0) return DECODE_ERR_INVALID_DATA;
    if (ctx->channels <= 0) return 0;

    int written = 0;
    while (written < max_samples) {
        if (ctx->pcm_pos < ctx->pcm_len) {
            int avail = ctx->pcm_len - ctx->pcm_pos;
            int want  = max_samples - written;
            int n = (want < avail) ? want : avail;
            memcpy(&samples[written], &ctx->pcm[ctx->pcm_pos], n * sizeof(int16_t));
            ctx->pcm_pos += n;
            written += n;
        } else {
            if (ctx->eof) break;
            drflac_uint64 frames_to_read = FLAC_PCM_SAMPLES / (drflac_uint64)ctx->channels;
            drflac_uint64 got = drflac_read_pcm_frames_s16(ctx->flac, frames_to_read, ctx->pcm);
            if (got == 0) { ctx->eof = true; break; }
            ctx->pcm_len = (int)(got * (drflac_uint64)ctx->channels);
            ctx->pcm_pos = 0;
            ctx->current_sample += got;
        }
    }
    return written;
}

static void flac_seek(void *context, uint64_t sample) {
    flac_ctx_t *ctx = (flac_ctx_t *)context;
    if (!ctx || !ctx->opened) return;
    if (drflac_seek_to_pcm_frame(ctx->flac, (drflac_uint64)sample)) {
        ctx->current_sample = sample;
        ctx->pcm_len = 0;
        ctx->pcm_pos = 0;
        ctx->eof = false;
    }
}

static bool flac_can_seek_impl(void *context) {
    flac_ctx_t *ctx = (flac_ctx_t *)context;
    return ctx && ctx->opened;
}

static uint64_t flac_tell(void *context) {
    flac_ctx_t *ctx = (flac_ctx_t *)context;
    return ctx ? ctx->current_sample : 0;
}

// ============================================================================
// Registration
// ============================================================================

const audio_codec_ops_t flac_codec_ops = {
    .codec      = AUDIO_CODEC_FLAC,
    .name       = "FLAC",
    .extensions = "flac",
    .can_decode = flac_can_decode,
    .create     = flac_create,
    .destroy    = flac_destroy,
    .get_info   = flac_get_info,
    .decode     = flac_decode,
    .seek       = flac_seek,
    .can_seek   = flac_can_seek_impl,
    .tell       = flac_tell,
};
