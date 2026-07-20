// wav.c - MayteraOS WAV (RIFF/WAVE) decoder.
//
// Implements the audio_codec_ops_t interface from audio_decode.h for
// uncompressed PCM WAVE files. No vendored library is needed: this parses the
// RIFF/WAVE header (fmt + data chunks) and converts the stored PCM to native
// interleaved signed 16-bit, channel count passed through (mono/stereo/multi),
// exactly like the MP3/OGG/FLAC wrappers expose it.
//
// Supported sample formats (all integer, no FPU, matching the kernel's
// -mno-sse / -mno-sse2 build):
//   - 8-bit  unsigned PCM        -> (s - 128) << 8
//   - 16-bit signed LE PCM       -> passthrough
//   - 24-bit signed LE PCM       -> top 16 bits
//   - 32-bit signed LE PCM       -> top 16 bits
// WAVE_FORMAT_PCM (1) and WAVE_FORMAT_EXTENSIBLE (0xFFFE) with a PCM subformat
// are accepted. IEEE float (format 3) is rejected (would require FP).
//
// License: MayteraOS (no third-party code). #331.

#include "audio_decode.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"

#define WAV_FMT_PCM         0x0001
#define WAV_FMT_IEEE_FLOAT  0x0003
#define WAV_FMT_EXTENSIBLE  0xFFFE

typedef struct {
    uint8_t  *buf;            // owned copy of the input
    size_t    buf_size;

    uint32_t  data_off;       // byte offset of PCM data within buf
    uint32_t  data_size;      // PCM data size in bytes

    int       sample_rate;
    int       channels;
    int       bits_per_sample;     // 8/16/24/32 (container bits)
    uint32_t  container_bytes;     // bits_per_sample / 8
    uint32_t  frame_bytes;         // container_bytes * channels

    uint64_t  total_samples;       // per channel (frames)
    uint64_t  current_sample;      // per channel
    uint32_t  pos;                 // current byte offset within data
} wav_ctx_t;

static inline uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ============================================================================
// #404 batch-2: RIFF/WAVE header-parse seam (strangler flag -DRUST_WAV_PARSE).
// The pure container walk (fmt/data chunk reads + validation + data-size clamp)
// is extracted from wav_create into wav_parse_header_c, and (under the flag)
// routed to the Rust port wav_parse_header_rs (rustkern.rs). C is kept as the
// verbatim rollback reference; a boot [RUST-DIFF] wav self-test re-proves rs==c
// on this build. Latent/defense-in-depth: the C walk was already bounded.
// ============================================================================

// #[repr(C)] out-struct shared with the Rust seam. sizeof-locked == 20.
typedef struct {
    uint16_t format;           // resolved WAVE format tag (1 = PCM on success)
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;  // 8/16/24/32
    uint16_t reserved;         // explicit pad, always 0
    uint32_t data_off;         // byte offset of PCM data within buf
    uint32_t data_size;        // PCM data size in bytes (clamped to buffer)
} WavInfo;
_Static_assert(sizeof(WavInfo) == 20, "WavInfo must be 20 bytes");

#define WAV_OK                      0
#define WAV_ERR_NULL               -1   // null data/out
#define WAV_ERR_TOO_SMALL          -2   // len < 44
#define WAV_ERR_NOT_WAVE           -3   // RIFF/WAVE magic mismatch
#define WAV_ERR_MISSING_CHUNK      -4   // no fmt or no data chunk
#define WAV_ERR_FLOAT              -5   // IEEE float PCM (needs FPU)
#define WAV_ERR_UNSUPPORTED_FMT    -6   // format != PCM
#define WAV_ERR_UNSUPPORTED_PARAMS -7   // bad channels/rate/bits

// Verbatim-faithful C reference: exactly the wav_create() RIFF walk + validation
// (INCLUDING the data-size-vs-buffer clamp), restructured only to fill a WavInfo
// and return status codes instead of allocating a ctx.
static int wav_parse_header_c(const uint8_t *data, uint32_t len, WavInfo *out) {
    if (!data || !out) return WAV_ERR_NULL;
    for (size_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

    size_t size = (size_t)len;
    if (size < 44) return WAV_ERR_TOO_SMALL;
    const uint8_t *b = data;
    if (!(b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' &&
          b[8] == 'W' && b[9] == 'A' && b[10] == 'V' && b[11] == 'E'))
        return WAV_ERR_NOT_WAVE;

    uint16_t fmt = 0;
    int channels = 0, sample_rate = 0, bits = 0;
    uint32_t data_off = 0, data_size = 0;
    int have_fmt = 0, have_data = 0;

    size_t off = 12;  // skip "RIFF" + size + "WAVE"
    while (off + 8 <= size) {
        const uint8_t *ch = b + off;
        uint32_t csize = rd_le32(ch + 4);
        size_t body = off + 8;
        if (ch[0] == 'f' && ch[1] == 'm' && ch[2] == 't' && ch[3] == ' ') {
            if (body + 16 <= size) {
                fmt         = rd_le16(b + body + 0);
                channels    = rd_le16(b + body + 2);
                sample_rate = rd_le32(b + body + 4);
                bits        = rd_le16(b + body + 14);
                if (fmt == WAV_FMT_EXTENSIBLE && body + 26 <= size) {
                    uint16_t cb = rd_le16(b + body + 16);
                    if (cb >= 2 && body + 24 + 2 <= size) {
                        fmt = rd_le16(b + body + 24);
                    }
                }
                have_fmt = 1;
            }
        } else if (ch[0] == 'd' && ch[1] == 'a' && ch[2] == 't' && ch[3] == 'a') {
            data_off  = (uint32_t)body;
            data_size = csize;
            if ((size_t)data_off + data_size > size) {
                data_size = (uint32_t)(size - data_off);   // clamp (truncated file)
            }
            have_data = 1;
        }
        off = body + csize + (csize & 1);   // word-aligned chunk advance
    }

    if (!have_fmt || !have_data) return WAV_ERR_MISSING_CHUNK;
    if (fmt == WAV_FMT_IEEE_FLOAT)  return WAV_ERR_FLOAT;
    if (fmt != WAV_FMT_PCM)         return WAV_ERR_UNSUPPORTED_FMT;
    if (channels <= 0 || sample_rate <= 0 ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return WAV_ERR_UNSUPPORTED_PARAMS;

    out->format          = fmt;
    out->channels        = (uint16_t)channels;
    out->sample_rate     = (uint32_t)sample_rate;
    out->bits_per_sample = (uint16_t)bits;
    out->reserved        = 0;
    out->data_off        = data_off;
    out->data_size       = data_size;
    return WAV_OK;
}

// Rust port (rustkern.rs, #404). Routed in under -DRUST_WAV_PARSE.
extern int wav_parse_header_rs(const uint8_t *data, uint32_t len, WavInfo *out);

#ifdef RUST_WAV_PARSE
#define wav_parse_header wav_parse_header_rs
#else
#define wav_parse_header wav_parse_header_c
#endif

// ============================================================================
// Codec interface
// ============================================================================

static bool wav_can_decode(const void *data, size_t size) {
    if (!data || size < 12) return false;
    const uint8_t *b = (const uint8_t *)data;
    return b[0] == 'R' && b[1] == 'I' && b[2] == 'F' && b[3] == 'F' &&
           b[8] == 'W' && b[9] == 'A' && b[10] == 'V' && b[11] == 'E';
}

static void *wav_create(const void *data, size_t size) {
    if (!data || size < 44) return NULL;
    const uint8_t *b = (const uint8_t *)data;
    if (!wav_can_decode(data, size)) return NULL;

    // Walk the RIFF chunks looking for "fmt " and "data" via the parse seam
    // (Rust under -DRUST_WAV_PARSE, else the verbatim C reference). The seam
    // fills a WavInfo and returns WAV_OK or a negative WAV_ERR_* code; the
    // diagnostics below map each code to the original kprintf message.
    WavInfo wi;
    int prc = wav_parse_header(b, (uint32_t)size, &wi);
    if (prc != WAV_OK) {
        switch (prc) {
            case WAV_ERR_MISSING_CHUNK:
                kprintf("[WAV] missing fmt/data chunk\n"); break;
            case WAV_ERR_FLOAT:
                kprintf("[WAV] IEEE float PCM not supported (no FPU)\n"); break;
            case WAV_ERR_UNSUPPORTED_FMT:
                kprintf("[WAV] unsupported format\n"); break;
            case WAV_ERR_UNSUPPORTED_PARAMS:
                kprintf("[WAV] unsupported params (ch/rate/bits)\n"); break;
            default: break;  // TOO_SMALL / NOT_WAVE / NULL: caller already guarded
        }
        return NULL;
    }
    int channels = (int)wi.channels;
    int sample_rate = (int)wi.sample_rate;
    int bits = (int)wi.bits_per_sample;
    uint32_t data_off = wi.data_off, data_size = wi.data_size;

    wav_ctx_t *ctx = kzalloc(sizeof(wav_ctx_t));
    if (!ctx) return NULL;
    ctx->buf_size = size;
    ctx->buf = kmalloc(size);
    if (!ctx->buf) { kfree(ctx); return NULL; }
    memcpy(ctx->buf, data, size);

    ctx->data_off        = data_off;
    ctx->data_size       = data_size;
    ctx->sample_rate     = sample_rate;
    ctx->channels        = channels;
    ctx->bits_per_sample = bits;
    ctx->container_bytes = (uint32_t)bits / 8;
    ctx->frame_bytes     = ctx->container_bytes * (uint32_t)channels;
    ctx->total_samples   = ctx->frame_bytes ? (data_size / ctx->frame_bytes) : 0;
    ctx->current_sample  = 0;
    ctx->pos             = 0;

    kprintf("[WAV] PCM: %d Hz, %d ch, %d-bit, %llu frames\n",
            sample_rate, channels, bits,
            (unsigned long long)ctx->total_samples);
    return ctx;
}

static void wav_destroy(void *context) {
    wav_ctx_t *ctx = (wav_ctx_t *)context;
    if (!ctx) return;
    if (ctx->buf) kfree(ctx->buf);
    kfree(ctx);
}

static int wav_get_info(void *context, audio_info_t *info) {
    wav_ctx_t *ctx = (wav_ctx_t *)context;
    if (!ctx || !info) return DECODE_ERR_INVALID_DATA;
    info->sample_rate     = ctx->sample_rate;
    info->channels        = ctx->channels;
    info->bits_per_sample = 16;   // we always emit 16-bit PCM
    info->total_samples   = ctx->total_samples;
    info->bitrate = (uint32_t)(ctx->sample_rate * ctx->channels * ctx->bits_per_sample);
    info->codec   = AUDIO_CODEC_WAV;
    info->duration_ms = (ctx->sample_rate && ctx->total_samples)
        ? (uint32_t)(ctx->total_samples * 1000 / ctx->sample_rate) : 0;
    return DECODE_OK;
}

// Convert one stored container sample (little-endian) to int16.
static inline int16_t wav_sample_to_s16(const uint8_t *p, uint32_t bytes) {
    switch (bytes) {
        case 1: // 8-bit unsigned
            return (int16_t)(((int)p[0] - 128) << 8);
        case 2: // 16-bit signed LE
            return (int16_t)(p[0] | (p[1] << 8));
        case 3: { // 24-bit signed LE -> top 16 bits
            int32_t v = (int32_t)((uint32_t)p[0] << 8 |
                                  (uint32_t)p[1] << 16 |
                                  (uint32_t)p[2] << 24);
            return (int16_t)(v >> 16);
        }
        case 4: { // 32-bit signed LE -> top 16 bits
            int32_t v = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                                  (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
            return (int16_t)(v >> 16);
        }
        default:
            return 0;
    }
}

// Read up to max_samples interleaved int16 samples. Returns count, 0 at EOF.
static int wav_decode(void *context, int16_t *samples, int max_samples) {
    wav_ctx_t *ctx = (wav_ctx_t *)context;
    if (!ctx || !samples || max_samples <= 0) return DECODE_ERR_INVALID_DATA;
    uint32_t cb = ctx->container_bytes;
    if (cb == 0) return 0;

    int written = 0;
    while (written < max_samples) {
        if (ctx->pos + cb > ctx->data_size) break;  // EOF
        const uint8_t *p = ctx->buf + ctx->data_off + ctx->pos;
        samples[written++] = wav_sample_to_s16(p, cb);
        ctx->pos += cb;
    }
    if (ctx->channels > 0) {
        ctx->current_sample = (uint64_t)ctx->pos / ctx->frame_bytes;
    }
    return written;
}

static void wav_seek(void *context, uint64_t sample) {
    wav_ctx_t *ctx = (wav_ctx_t *)context;
    if (!ctx) return;
    uint64_t byte = sample * ctx->frame_bytes;
    if (byte > ctx->data_size) byte = ctx->data_size;
    ctx->pos = (uint32_t)byte;
    ctx->current_sample = ctx->frame_bytes ? (byte / ctx->frame_bytes) : 0;
}

static bool wav_can_seek_impl(void *context) {
    (void)context;
    return true;
}

static uint64_t wav_tell(void *context) {
    wav_ctx_t *ctx = (wav_ctx_t *)context;
    return ctx ? ctx->current_sample : 0;
}

// ============================================================================
// Registration
// ============================================================================

const audio_codec_ops_t wav_codec_ops = {
    .codec      = AUDIO_CODEC_WAV,
    .name       = "WAV",
    .extensions = "wav",
    .can_decode = wav_can_decode,
    .create     = wav_create,
    .destroy    = wav_destroy,
    .get_info   = wav_get_info,
    .decode     = wav_decode,
    .seek       = wav_seek,
    .can_seek   = wav_can_seek_impl,
    .tell       = wav_tell,
};

// ============================================================================
// #404 batch-2 boot-time differential self-test. Proves wav_parse_header_rs ==
// wav_parse_header_c on THIS build over well-formed AND malformed RIFF/WAVE
// headers, comparing the WavInfo out-struct FIELD-BY-FIELD (not memcmp: C-vs-Rust
// tail padding can differ) plus the return code. Bounded, runs once at boot
// before any WAV is decoded; logs one [RUST-DIFF] wav line to serial + /BOOTLOG.
// Latent/defense-in-depth: the C walk is already bounded (no advisory).
// ============================================================================
static inline void wav_st_le16(uint8_t *p, uint16_t v) { p[0]=v&0xff; p[1]=(v>>8)&0xff; }
static inline void wav_st_le32(uint8_t *p, uint32_t v) {
    p[0]=v&0xff; p[1]=(v>>8)&0xff; p[2]=(v>>16)&0xff; p[3]=(v>>24)&0xff;
}

// Build a minimal RIFF/WAVE into buf; returns total length. fmt_tag: 1=PCM,
// 3=float, 0xFFFE=extensible(PCM subformat). data_csize is the declared "data"
// chunk size (may exceed the real bytes to exercise the clamp path).
static uint32_t wav_st_build(uint8_t *buf, uint16_t fmt_tag, uint16_t ch,
                             uint32_t rate, uint16_t bits, uint32_t data_csize,
                             uint32_t data_real) {
    int ext = (fmt_tag == WAV_FMT_EXTENSIBLE);
    uint16_t block_align = (uint16_t)(ch * (bits / 8));
    uint32_t off = 0;
    memcpy(buf + off, "RIFF", 4); off += 4;
    uint32_t riff_pos = off; wav_st_le32(buf + off, 0); off += 4;
    memcpy(buf + off, "WAVE", 4); off += 4;
    memcpy(buf + off, "fmt ", 4); off += 4;
    uint32_t fmt_body = ext ? 40 : 16;
    wav_st_le32(buf + off, fmt_body); off += 4;
    uint32_t fb = off;
    wav_st_le16(buf + fb + 0, fmt_tag);
    wav_st_le16(buf + fb + 2, ch);
    wav_st_le32(buf + fb + 4, rate);
    wav_st_le32(buf + fb + 8, rate * block_align);
    wav_st_le16(buf + fb + 12, block_align);
    wav_st_le16(buf + fb + 14, bits);
    if (ext) {
        wav_st_le16(buf + fb + 16, 22);
        wav_st_le16(buf + fb + 18, bits);
        wav_st_le32(buf + fb + 20, 0);
        wav_st_le16(buf + fb + 24, 1);   // SubFormat = PCM
        for (int i = 26; i < 40; i++) buf[fb + i] = 0;
    }
    off += fmt_body;
    memcpy(buf + off, "data", 4); off += 4;
    wav_st_le32(buf + off, data_csize); off += 4;
    for (uint32_t i = 0; i < data_real; i++) buf[off + i] = (uint8_t)(i * 7 + 1);
    off += data_real;
    wav_st_le32(buf + riff_pos, off - 8);
    return off;
}

void wav_rust_selftest(void) {
    extern void bootlog_write(const char *fmt, ...);
    uint8_t buf[512];
    int total = 0, mism = 0;

    struct { uint16_t tag, ch, bits; uint32_t rate, dcsize, dreal; } vecs[] = {
        { WAV_FMT_PCM,        2, 16, 44100,   8,   8 },  // valid stereo 16-bit
        { WAV_FMT_PCM,        1,  8,  8000,  16,  16 },  // valid mono 8-bit
        { WAV_FMT_PCM,        2, 24, 48000,  24,  24 },  // valid 24-bit
        { WAV_FMT_PCM,        1, 32, 96000,  16,  16 },  // valid 32-bit
        { WAV_FMT_EXTENSIBLE, 2, 16, 44100,  12,  12 },  // EXTENSIBLE -> PCM
        { WAV_FMT_IEEE_FLOAT, 2, 32, 44100,   8,   8 },  // float -> reject (agree)
        { 0x0055,             2, 16, 44100,   8,   8 },  // MP3 tag -> reject (agree)
        { WAV_FMT_PCM,        2, 16, 44100, 0xFFFFFFFFu, 8 }, // huge data -> clamp
        { WAV_FMT_PCM,        3, 16, 44100,   6,   6 },  // 3 channels, odd data
    };
    for (unsigned v = 0; v < sizeof(vecs)/sizeof(vecs[0]); v++) {
        uint32_t len = wav_st_build(buf, vecs[v].tag, vecs[v].ch, vecs[v].rate,
                                    vecs[v].bits, vecs[v].dcsize, vecs[v].dreal);
        WavInfo ic, ir;
        int rc = wav_parse_header_c (buf, len, &ic);
        int rr = wav_parse_header_rs(buf, len, &ir);
        total++;
        int bad = (rc != rr);
        if (rc == WAV_OK && rr == WAV_OK) {
            // FIELD-BY-FIELD (padding may differ, so no whole-struct memcmp).
            if (ic.format != ir.format || ic.channels != ir.channels ||
                ic.sample_rate != ir.sample_rate ||
                ic.bits_per_sample != ir.bits_per_sample ||
                ic.reserved != ir.reserved ||
                ic.data_off != ir.data_off || ic.data_size != ir.data_size)
                bad = 1;
        }
        if (bad) mism++;
    }

    // Explicit malformed cases (too-short + bad-magic): both must reject alike.
    {
        WavInfo ic, ir;
        int rc = wav_parse_header_c (buf, 20, &ic);       // too short
        int rr = wav_parse_header_rs(buf, 20, &ir);
        total++; if (rc != rr) mism++;
        uint8_t bogus[64]; for (int i = 0; i < 64; i++) bogus[i] = (uint8_t)(i * 3);
        rc = wav_parse_header_c (bogus, sizeof(bogus), &ic); // bad magic
        rr = wav_parse_header_rs(bogus, sizeof(bogus), &ir);
        total++; if (rc != rr) mism++;
    }

    kprintf("[RUST-DIFF] wav: %d vectors, mism=%d %s (LIVE=%s)\n",
            total, mism, mism ? "*** MISMATCH ***" : "MATCH",
#ifdef RUST_WAV_PARSE
            "rust");
#else
            "c");
#endif
    bootlog_write("[RUST-DIFF] wav: %d vectors mism=%d %s", total, mism,
                  mism ? "MISMATCH" : "MATCH");
    kprintf("[RUST-SEC]  wav: RIFF walk confined to len by construction "
            "(defense-in-depth; C already bounded)\n");
}
