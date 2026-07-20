// mp3.c - MayteraOS MP3 decoder (real decode via libmad, fixed-point/integer)
//
// Implements the audio_codec_ops_t interface declared in audio_decode.h using
// libmad (libmad 0.15.1b) configured for the FPM_64BIT integer path. The kernel
// is built -mno-sse -mno-sse2, so the decoder must avoid all floating point;
// libmad's fixed-point core satisfies that (28-bit fractional mad_fixed_t with
// a 64-bit integer multiply). The PCM conversion below is also integer-only.
//
// Vendored sources live in media/libmad/ (built with relaxed warnings). See
// media/libmad/config.h for the build configuration and the compat/ shims.
//
// License: libmad is GPLv2 (Underbit Technologies). This is a hobby OS; the
// dependency is noted in CHANGELOG.md. Source: libmad-0.15.1b from SourceForge.

#include "audio_decode.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"

// libmad public API (individual headers; config.h must precede fixed.h).
#include "libmad/config.h"
#include "libmad/version.h"
#include "libmad/fixed.h"
#include "libmad/bit.h"
#include "libmad/timer.h"
#include "libmad/stream.h"
#include "libmad/frame.h"
#include "libmad/synth.h"

// MAD_BUFFER_GUARD zero bytes must follow the last byte of the input so libmad
// can decode the final frame. libmad defines it (typically 8).
#ifndef MAD_BUFFER_GUARD
#define MAD_BUFFER_GUARD 8
#endif

#define MP3_MAX_PCM (1152 * 2)   // max samples/frame/channel * max channels

// ============================================================================
// Integer-only fixed -> int16 conversion (no floating point).
// mad_fixed_t is 28-bit fractional signed. Round, clip, then shift to int16.
// ============================================================================

static inline int16_t mad_scale_s16(mad_fixed_t sample) {
    sample += (1L << (MAD_F_FRACBITS - 16));        // round
    if (sample >= MAD_F_ONE)        sample = MAD_F_ONE - 1;
    else if (sample < -MAD_F_ONE)   sample = -MAD_F_ONE;
    return (int16_t)(sample >> (MAD_F_FRACBITS + 1 - 16));
}

// ============================================================================
// Decoder context
// ============================================================================

typedef struct {
    uint8_t *buf;            // owned padded copy of input (+ MAD_BUFFER_GUARD)
    size_t   buf_size;       // data_size + MAD_BUFFER_GUARD
    size_t   data_size;      // original compressed size

    struct mad_stream stream;
    struct mad_frame  frame;
    struct mad_synth  synth;

    int      sample_rate;
    int      channels;
    int      bitrate;        // bits/sec, from first frame header
    uint64_t total_samples;  // estimated, per channel
    uint64_t current_sample; // per channel

    int16_t  pcm[MP3_MAX_PCM]; // interleaved leftover from last synth frame
    int      pcm_len;          // valid interleaved samples
    int      pcm_pos;          // next interleaved sample to emit

    bool     eof;
} mp3_ctx_t;

// Interleave one synthesized frame into ctx->pcm. Returns false if no samples.
static bool mp3_fill_pcm(mp3_ctx_t *ctx) {
    unsigned n = ctx->synth.pcm.length;
    int ch = ctx->synth.pcm.channels;
    if (n == 0) { ctx->pcm_len = 0; ctx->pcm_pos = 0; return false; }
    int k = 0;
    for (unsigned i = 0; i < n; i++) {
        ctx->pcm[k++] = mad_scale_s16(ctx->synth.pcm.samples[0][i]);
        if (ch == 2) {
            ctx->pcm[k++] = mad_scale_s16(ctx->synth.pcm.samples[1][i]);
        }
    }
    ctx->pcm_len = k;
    ctx->pcm_pos = 0;
    ctx->current_sample += n;
    return true;
}

// Decode frames until one yields PCM (filling ctx->pcm) or stream ends.
// Returns true if PCM is available, false at end of stream.
static bool mp3_decode_one(mp3_ctx_t *ctx) {
    for (;;) {
        if (mad_frame_decode(&ctx->frame, &ctx->stream) != 0) {
            if (MAD_RECOVERABLE(ctx->stream.error)) {
                continue;  // skip bad frame, keep going
            }
            // BUFLEN means we consumed all input (EOF). Any other fatal: stop.
            ctx->eof = true;
            return false;
        }
        mad_synth_frame(&ctx->synth, &ctx->frame);
        if (ctx->channels == 0) {
            ctx->channels    = ctx->synth.pcm.channels;
            ctx->sample_rate = ctx->synth.pcm.samplerate;
        }
        if (mp3_fill_pcm(ctx)) {
            return true;
        }
        // zero-length frame; keep decoding
    }
}

// ============================================================================
// Codec interface
// ============================================================================

static bool mp3_can_decode(const void *data, size_t size) {
    if (!data || size < 4) return false;
    const uint8_t *b = (const uint8_t *)data;
    if (b[0] == 'I' && b[1] == 'D' && b[2] == '3') return true;
    if (b[0] == 0xFF && (b[1] & 0xE0) == 0xE0) {
        uint8_t version = (b[1] >> 3) & 0x03;
        uint8_t layer   = (b[1] >> 1) & 0x03;
        if (version != 0x01 && layer != 0x00) return true;
    }
    return false;
}

static void *mp3_create(const void *data, size_t size) {
    if (!data || size == 0) return NULL;

    mp3_ctx_t *ctx = kzalloc(sizeof(mp3_ctx_t));
    if (!ctx) return NULL;

    ctx->data_size = size;
    ctx->buf_size  = size + MAD_BUFFER_GUARD;
    ctx->buf = kmalloc(ctx->buf_size);
    if (!ctx->buf) { kfree(ctx); return NULL; }
    memcpy(ctx->buf, data, size);
    memset(ctx->buf + size, 0, MAD_BUFFER_GUARD);

    mad_stream_init(&ctx->stream);
    mad_frame_init(&ctx->frame);
    mad_synth_init(&ctx->synth);
    mad_stream_buffer(&ctx->stream, ctx->buf, ctx->buf_size);

    // Decode the first frame to learn rate/channels/bitrate and prime PCM.
    if (!mp3_decode_one(ctx)) {
        mad_synth_finish(&ctx->synth);
        mad_frame_finish(&ctx->frame);
        mad_stream_finish(&ctx->stream);
        kfree(ctx->buf);
        kfree(ctx);
        return NULL;
    }

    ctx->bitrate = (int)ctx->frame.header.bitrate;
    if (ctx->sample_rate == 0) ctx->sample_rate = (int)ctx->frame.header.samplerate;

    // Estimate total per-channel samples from average bitrate over the stream.
    if (ctx->bitrate > 0) {
        ctx->total_samples =
            (uint64_t)ctx->data_size * 8 * ctx->sample_rate / (uint64_t)ctx->bitrate;
    } else {
        ctx->total_samples = 0;
    }

    kprintf("[MP3] libmad: %d Hz, %d ch, %d kbps\n",
            ctx->sample_rate, ctx->channels, ctx->bitrate / 1000);
    return ctx;
}

static void mp3_destroy(void *context) {
    mp3_ctx_t *ctx = (mp3_ctx_t *)context;
    if (!ctx) return;
    mad_synth_finish(&ctx->synth);
    mad_frame_finish(&ctx->frame);
    mad_stream_finish(&ctx->stream);
    if (ctx->buf) kfree(ctx->buf);
    kfree(ctx);
}

static int mp3_get_info(void *context, audio_info_t *info) {
    mp3_ctx_t *ctx = (mp3_ctx_t *)context;
    if (!ctx || !info) return DECODE_ERR_INVALID_DATA;
    info->sample_rate     = ctx->sample_rate;
    info->channels        = ctx->channels;
    info->bits_per_sample = 16;
    info->total_samples   = ctx->total_samples;
    info->bitrate         = ctx->bitrate;
    info->codec           = AUDIO_CODEC_MP3;
    info->duration_ms = (ctx->sample_rate && ctx->total_samples)
        ? (uint32_t)(ctx->total_samples * 1000 / ctx->sample_rate) : 0;
    return DECODE_OK;
}

// Read up to max_samples interleaved int16 samples. Returns count, 0 at EOF.
static int mp3_decode(void *context, int16_t *samples, int max_samples) {
    mp3_ctx_t *ctx = (mp3_ctx_t *)context;
    if (!ctx || !samples || max_samples <= 0) return DECODE_ERR_INVALID_DATA;

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
            if (!mp3_decode_one(ctx)) break;
        }
    }
    return written;
}

static void mp3_seek(void *context, uint64_t sample) {
    mp3_ctx_t *ctx = (mp3_ctx_t *)context;
    if (!ctx) return;

    // Restart the stream and decode-skip forward to the target (no seek table).
    mad_synth_finish(&ctx->synth);
    mad_frame_finish(&ctx->frame);
    mad_stream_finish(&ctx->stream);
    mad_stream_init(&ctx->stream);
    mad_frame_init(&ctx->frame);
    mad_synth_init(&ctx->synth);
    mad_stream_buffer(&ctx->stream, ctx->buf, ctx->buf_size);

    ctx->current_sample = 0;
    ctx->pcm_len = 0;
    ctx->pcm_pos = 0;
    ctx->eof = false;

    while (ctx->current_sample < sample) {
        if (!mp3_decode_one(ctx)) break;
        // Drop the decoded frame's samples (skip) unless this is the target.
        if (ctx->current_sample < sample) {
            ctx->pcm_len = 0;
            ctx->pcm_pos = 0;
        }
    }
}

static bool mp3_can_seek_impl(void *context) {
    (void)context;
    return true;  // supported (linear, decode-forward)
}

static uint64_t mp3_tell(void *context) {
    mp3_ctx_t *ctx = (mp3_ctx_t *)context;
    return ctx ? ctx->current_sample : 0;
}

// ============================================================================
// Registration
// ============================================================================

const audio_codec_ops_t mp3_codec_ops = {
    .codec      = AUDIO_CODEC_MP3,
    .name       = "MP3",
    .extensions = "mp3",
    .can_decode = mp3_can_decode,
    .create     = mp3_create,
    .destroy    = mp3_destroy,
    .get_info   = mp3_get_info,
    .decode     = mp3_decode,
    .seek       = mp3_seek,
    .can_seek   = mp3_can_seek_impl,
    .tell       = mp3_tell,
};
