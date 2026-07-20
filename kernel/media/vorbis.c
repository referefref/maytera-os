// vorbis.c - MayteraOS OGG Vorbis decoder (real decode via Tremor / libvorbisidec)
//
// Implements the audio_codec_ops_t interface from audio_decode.h using Tremor
// (libvorbisidec), the INTEGER / fixed-point Vorbis decoder, plus libogg for the
// Ogg container layer. The kernel is built -mno-sse / -mno-sse2 (no FPU), so the
// decoder must avoid all floating point; Tremor's integer core (and libogg) do,
// and Tremor's ov_read() yields native-endian interleaved signed 16-bit PCM
// directly, which is exactly what this codec exposes.
//
// Vendored sources live in media/tremor/ (Tremor core + libogg bitwise.c/
// framing.c) and are compiled with relaxed warnings + compat shims. Decoding
// works off an in-memory buffer through ov_open_callbacks (no file I/O).
//
// License: Tremor and libogg are BSD-style (Xiph.Org Foundation). See
// media/tremor/COPYING (Xiph) and CHANGELOG.md. Sources: gitlab.xiph.org
// xiph/tremor and xiph/ogg (master).

#include "audio_decode.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"

// Tremor public API. ivorbisfile.h pulls <ogg/ogg.h> (stddef include is guarded
// for the kernel since types.h already defines size_t) and the compat <stdio.h>.
#include "tremor/ivorbiscodec.h"
#include "tremor/ivorbisfile.h"

// PCM scratch: 8192 interleaved int16 samples (16 KiB) read per ov_read fill.
#define VORBIS_PCM_SAMPLES 8192

typedef struct {
    const uint8_t *data;   // borrowed copy of the compressed stream
    long           size;
    long           pos;
} vorbis_memsrc_t;

typedef struct {
    uint8_t        *buf;            // owned copy of the input
    long            buf_size;

    vorbis_memsrc_t src;            // in-memory datasource for ov_callbacks
    OggVorbis_File  vf;             // Tremor file handle
    bool            opened;

    int             sample_rate;
    int             channels;
    int             bitrate;        // nominal bits/sec (0 if unknown)
    uint64_t        total_samples;  // per channel
    uint64_t        current_sample; // per channel

    int16_t         pcm[VORBIS_PCM_SAMPLES]; // interleaved leftover
    int             pcm_len;        // valid interleaved samples
    int             pcm_pos;        // next interleaved sample to emit
    bool            eof;
} vorbis_ctx_t;

// ============================================================================
// In-memory ov_callbacks (mirror stdio fread/fseek/fclose/ftell semantics)
// ============================================================================

static size_t v_read(void *ptr, size_t size, size_t nmemb, void *ds) {
    vorbis_memsrc_t *m = (vorbis_memsrc_t *)ds;
    long want = (long)(size * nmemb);
    long avail = m->size - m->pos;
    if (want > avail) want = avail;
    if (want < 0) want = 0;
    if (want) memcpy(ptr, m->data + m->pos, (size_t)want);
    m->pos += want;
    return size ? (size_t)(want / (long)size) : 0;
}

static int v_seek(void *ds, ogg_int64_t off, int whence) {
    vorbis_memsrc_t *m = (vorbis_memsrc_t *)ds;
    long np;
    if (whence == 0)      np = (long)off;            // SEEK_SET
    else if (whence == 1) np = m->pos + (long)off;   // SEEK_CUR
    else                  np = m->size + (long)off;   // SEEK_END
    if (np < 0 || np > m->size) return -1;
    m->pos = np;
    return 0;
}

static int v_close(void *ds) { (void)ds; return 0; }

static long v_tell(void *ds) { return ((vorbis_memsrc_t *)ds)->pos; }

// ============================================================================
// Codec interface
// ============================================================================

static bool vorbis_can_decode(const void *data, size_t size) {
    if (!data || size < 4) return false;
    const uint8_t *b = (const uint8_t *)data;
    return b[0] == 'O' && b[1] == 'g' && b[2] == 'g' && b[3] == 'S';
}

static void *vorbis_create(const void *data, size_t size) {
    if (!data || size == 0) return NULL;

    vorbis_ctx_t *ctx = kzalloc(sizeof(vorbis_ctx_t));
    if (!ctx) return NULL;

    ctx->buf_size = (long)size;
    ctx->buf = kmalloc(size);
    if (!ctx->buf) { kfree(ctx); return NULL; }
    memcpy(ctx->buf, data, size);

    ctx->src.data = ctx->buf;
    ctx->src.size = ctx->buf_size;
    ctx->src.pos  = 0;

    ov_callbacks cb;
    cb.read_func  = v_read;
    cb.seek_func  = v_seek;
    cb.close_func = v_close;
    cb.tell_func  = v_tell;

    int r = ov_open_callbacks(&ctx->src, &ctx->vf, NULL, 0, cb);
    if (r < 0) {
        kprintf("[Vorbis] ov_open_callbacks failed (%d)\n", r);
        kfree(ctx->buf);
        kfree(ctx);
        return NULL;
    }
    ctx->opened = true;

    vorbis_info *vi = ov_info(&ctx->vf, -1);
    if (!vi) {
        ov_clear(&ctx->vf);
        kfree(ctx->buf);
        kfree(ctx);
        return NULL;
    }
    ctx->sample_rate = (int)vi->rate;
    ctx->channels    = vi->channels;
    ctx->bitrate     = (int)vi->bitrate_nominal;

    ogg_int64_t total = ov_pcm_total(&ctx->vf, -1);
    ctx->total_samples = (total > 0) ? (uint64_t)total : 0;

    kprintf("[Vorbis] Tremor: %d Hz, %d ch, %d kbps (nominal)\n",
            ctx->sample_rate, ctx->channels, ctx->bitrate / 1000);
    return ctx;
}

static void vorbis_destroy(void *context) {
    vorbis_ctx_t *ctx = (vorbis_ctx_t *)context;
    if (!ctx) return;
    if (ctx->opened) ov_clear(&ctx->vf);
    if (ctx->buf) kfree(ctx->buf);
    kfree(ctx);
}

static int vorbis_get_info(void *context, audio_info_t *info) {
    vorbis_ctx_t *ctx = (vorbis_ctx_t *)context;
    if (!ctx || !info) return DECODE_ERR_INVALID_DATA;
    info->sample_rate     = ctx->sample_rate;
    info->channels        = ctx->channels;
    info->bits_per_sample = 16;
    info->total_samples   = ctx->total_samples;
    info->bitrate         = ctx->bitrate;
    info->codec           = AUDIO_CODEC_OGG_VORBIS;
    info->duration_ms = (ctx->sample_rate && ctx->total_samples)
        ? (uint32_t)(ctx->total_samples * 1000 / ctx->sample_rate) : 0;
    return DECODE_OK;
}

// Read up to max_samples interleaved int16 samples. Returns count, 0 at EOF.
static int vorbis_decode(void *context, int16_t *samples, int max_samples) {
    vorbis_ctx_t *ctx = (vorbis_ctx_t *)context;
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
            int bitstream = 0;
            long bytes = ov_read(&ctx->vf, (char *)ctx->pcm,
                                 VORBIS_PCM_SAMPLES * (int)sizeof(int16_t),
                                 &bitstream);
            if (bytes <= 0) { ctx->eof = true; break; }  // 0=EOF, <0=error
            ctx->pcm_len = (int)(bytes / (long)sizeof(int16_t));
            ctx->pcm_pos = 0;
            if (ctx->channels > 0)
                ctx->current_sample += ctx->pcm_len / ctx->channels;
        }
    }
    return written;
}

static void vorbis_seek(void *context, uint64_t sample) {
    vorbis_ctx_t *ctx = (vorbis_ctx_t *)context;
    if (!ctx || !ctx->opened) return;
    if (ov_pcm_seek(&ctx->vf, (ogg_int64_t)sample) == 0) {
        ctx->current_sample = sample;
        ctx->pcm_len = 0;
        ctx->pcm_pos = 0;
        ctx->eof = false;
    }
}

static bool vorbis_can_seek_impl(void *context) {
    vorbis_ctx_t *ctx = (vorbis_ctx_t *)context;
    return ctx && ctx->opened && ov_seekable(&ctx->vf);
}

static uint64_t vorbis_tell(void *context) {
    vorbis_ctx_t *ctx = (vorbis_ctx_t *)context;
    return ctx ? ctx->current_sample : 0;
}

// ============================================================================
// Registration
// ============================================================================

const audio_codec_ops_t vorbis_codec_ops = {
    .codec      = AUDIO_CODEC_OGG_VORBIS,
    .name       = "OGG Vorbis",
    .extensions = "ogg",
    .can_decode = vorbis_can_decode,
    .create     = vorbis_create,
    .destroy    = vorbis_destroy,
    .get_info   = vorbis_get_info,
    .decode     = vorbis_decode,
    .seek       = vorbis_seek,
    .can_seek   = vorbis_can_seek_impl,
    .tell       = vorbis_tell,
};
