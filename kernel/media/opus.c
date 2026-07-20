// opus.c - MayteraOS Opus decoder (real fixed-point decode via vendored libopus)
//
// Implements the audio_codec_ops_t interface from audio_decode.h using libopus
// (Xiph/Mozilla) built in INTEGER / fixed-point mode (FIXED_POINT, no FPU; the
// kernel is -mno-sse / -mno-sse2). opus_decode() yields native-endian
// interleaved signed 16-bit PCM at the fixed Opus 48 kHz output rate, which is
// exactly what this codec exposes. The Ogg-Opus container is demuxed with the
// libogg framing layer already vendored for OGG Vorbis (media/tremor, #236);
// per RFC 7845 the first packet is "OpusHead" (channel/pre-skip/mapping) and the
// second is "OpusTags".  Pre-skip is trimmed from the front and the stream is
// trimmed to the final granule position, matching libopusfile / ffmpeg output.
//
// Vendored sources live in media/opus/ (libopus 1.3.1 fixed-point decode subset:
// celt/ + silk/ + src/, plus compat shims).  Built with relaxed warnings and the
// fixed-point config; objdump-verified to contain ZERO floating-point / SIMD
// instructions.  Decoding works off an in-memory buffer (no file I/O).
//
// License: libopus is BSD (Xiph.Org Foundation / Jean-Marc Valin et al.). See
// media/opus/COPYING and CHANGELOG.md. Source: downloads.xiph.org opus-1.3.1.

#include "audio_decode.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"

// libogg framing (shared with the Vorbis/Tremor port).
#include "tremor/ogg/ogg.h"
// libopus public API.
#include "opus/include/opus.h"
#include "opus/include/opus_multistream.h"

// Opus always decodes to 48 kHz. A single packet covers at most 120 ms.
#define OPUS_RATE          48000
#define OPUS_MAX_FRAME     5760     // 120 ms @ 48 kHz, per channel
#define OPUS_PCM_FRAMES    5760     // leftover ring (per-channel frames)

typedef struct {
    uint8_t        *buf;            // owned copy of the input
    long            buf_size;

    ogg_sync_state  oy;
    ogg_stream_state os;
    bool            sync_init;
    bool            stream_init;

    OpusDecoder    *dec;            // family 0 (1-2 ch)
    OpusMSDecoder  *msdec;          // family 1 (surround)
    bool            is_ms;

    int             channels;
    int             preskip;        // samples (per channel) to drop from front
    uint64_t        total_samples;  // per channel after trimming (0 if unknown)
    uint64_t        current_sample; // per channel emitted so far

    int             preskip_left;   // remaining pre-skip frames to drop
    uint64_t        frames_emitted; // per-channel frames committed to leftover/out

    int16_t        *frame;          // scratch decode buffer (channels*MAX_FRAME)
    int16_t        *pcm;            // leftover interleaved samples
    int             pcm_len;        // valid interleaved samples
    int             pcm_pos;        // next interleaved sample to emit
    bool            eof;
} opus_ctx_t;

// ============================================================================
// Helpers
// ============================================================================

static uint64_t scan_last_granule(const uint8_t *d, long n) {
    // Walk Ogg page captures and keep the last valid (non -1) granule position.
    uint64_t last = 0;
    long i = 0;
    while (i + 27 <= n) {
        if (!(d[i] == 'O' && d[i+1] == 'g' && d[i+2] == 'g' && d[i+3] == 'S')) { i++; continue; }
        uint64_t g = 0;
        for (int k = 7; k >= 0; k--) g = (g << 8) | d[i+6+k];
        int nseg = d[i+26];
        if (i + 27 + nseg > n) break;
        long body = 0;
        for (int s = 0; s < nseg; s++) body += d[i+27+s];
        if (g != (uint64_t)-1) last = g;
        i += 27 + nseg + body;
    }
    return last;
}

// Pull the next packet from the logical stream, feeding pages as needed.
// Returns 1 on packet, 0 when no more packets are available.
static int opus_next_packet(opus_ctx_t *c, ogg_packet *op) {
    for (;;) {
        if (c->stream_init) {
            int r = ogg_stream_packetout(&c->os, op);
            if (r == 1) return 1;
            // r==0: need more page data; r==-1: gap, just try next page
        }
        ogg_page og;
        int pr = ogg_sync_pageout(&c->oy, &og);
        if (pr != 1) return 0;  // all buffered input consumed
        if (!c->stream_init) {
            ogg_stream_init(&c->os, ogg_page_serialno(&og));
            c->stream_init = true;
        }
        ogg_stream_pagein(&c->os, &og);
    }
}

// ============================================================================
// Codec interface
// ============================================================================

static bool opus_can_decode(const void *data, size_t size) {
    // Ogg container whose first BOS packet is "OpusHead".
    if (!data || size < 36) return false;
    const uint8_t *b = (const uint8_t *)data;
    if (!(b[0]=='O' && b[1]=='g' && b[2]=='g' && b[3]=='S')) return false;
    // First page header: 27 bytes + segment table. The first packet payload
    // begins right after the segment table and (for Opus) starts "OpusHead".
    int nseg = b[26];
    size_t payload = 27 + (size_t)nseg;
    if (payload + 8 > size) return false;
    const uint8_t *p = b + payload;
    return p[0]=='O'&&p[1]=='p'&&p[2]=='u'&&p[3]=='s'&&p[4]=='H'&&p[5]=='e'&&p[6]=='a'&&p[7]=='d';
}

static void opus_destroy(void *context);

static void *opus_create(const void *data, size_t size) {
    if (!data || size == 0) return NULL;

    opus_ctx_t *c = kzalloc(sizeof(opus_ctx_t));
    if (!c) return NULL;

    c->buf_size = (long)size;
    c->buf = kmalloc(size);
    if (!c->buf) { kfree(c); return NULL; }
    memcpy(c->buf, data, size);

    uint64_t last_granule = scan_last_granule(c->buf, c->buf_size);

    ogg_sync_init(&c->oy);
    c->sync_init = true;
    char *sb = ogg_sync_buffer(&c->oy, c->buf_size);
    if (!sb) { opus_destroy(c); return NULL; }
    memcpy(sb, c->buf, c->buf_size);
    ogg_sync_wrote(&c->oy, c->buf_size);

    // Header packet 0 = OpusHead, packet 1 = OpusTags.
    ogg_packet op;
    if (!opus_next_packet(c, &op) || op.bytes < 19 ||
        memcmp(op.packet, "OpusHead", 8) != 0) {
        kprintf("[Opus] missing OpusHead\n");
        opus_destroy(c);
        return NULL;
    }
    const uint8_t *h = op.packet;
    c->channels = h[9];
    c->preskip  = h[10] | (h[11] << 8);
    int gain    = (int16_t)(h[16] | (h[17] << 8));   // Q7.8 output gain
    int family  = h[18];

    if (c->channels < 1 || c->channels > 8) {
        kprintf("[Opus] unsupported channel count %d\n", c->channels);
        opus_destroy(c);
        return NULL;
    }

    int err = OPUS_OK;
    if (family == 0) {
        c->is_ms = false;
        c->dec = opus_decoder_create(OPUS_RATE, c->channels, &err);
        if (c->dec && gain) opus_decoder_ctl(c->dec, OPUS_SET_GAIN(gain));
    } else {
        // RFC 7845 mapping family 1+: explicit stream layout.
        if (op.bytes < 21 + c->channels) { opus_destroy(c); return NULL; }
        int streams = h[19];
        int coupled = h[20];
        const unsigned char *mapping = h + 21;
        c->is_ms = true;
        c->msdec = opus_multistream_decoder_create(OPUS_RATE, c->channels,
                                                   streams, coupled, mapping, &err);
        if (c->msdec && gain) opus_multistream_decoder_ctl(c->msdec, OPUS_SET_GAIN(gain));
    }
    if (err != OPUS_OK || (!c->dec && !c->msdec)) {
        kprintf("[Opus] decoder create failed (%d)\n", err);
        opus_destroy(c);
        return NULL;
    }

    // Consume OpusTags (second header packet) if present.
    ogg_packet tags;
    opus_next_packet(c, &tags);

    c->frame = kmalloc((size_t)c->channels * OPUS_MAX_FRAME * sizeof(int16_t));
    c->pcm   = kmalloc((size_t)c->channels * OPUS_PCM_FRAMES * sizeof(int16_t));
    if (!c->frame || !c->pcm) { opus_destroy(c); return NULL; }

    c->preskip_left = c->preskip;
    c->total_samples = (last_granule > (uint64_t)c->preskip)
                       ? (last_granule - (uint64_t)c->preskip) : 0;

    kprintf("[Opus] libopus: %d Hz, %d ch, preskip=%d, family=%d, frames=%llu\n",
            OPUS_RATE, c->channels, c->preskip, family,
            (unsigned long long)c->total_samples);
    return c;
}

static void opus_destroy(void *context) {
    opus_ctx_t *c = (opus_ctx_t *)context;
    if (!c) return;
    if (c->dec)   opus_decoder_destroy(c->dec);
    if (c->msdec) opus_multistream_decoder_destroy(c->msdec);
    if (c->stream_init) ogg_stream_clear(&c->os);
    if (c->sync_init)   ogg_sync_clear(&c->oy);
    if (c->frame) kfree(c->frame);
    if (c->pcm)   kfree(c->pcm);
    if (c->buf)   kfree(c->buf);
    kfree(c);
}

static int opus_get_info(void *context, audio_info_t *info) {
    opus_ctx_t *c = (opus_ctx_t *)context;
    if (!c || !info) return DECODE_ERR_INVALID_DATA;
    info->sample_rate     = OPUS_RATE;
    info->channels        = c->channels;
    info->bits_per_sample = 16;
    info->total_samples   = c->total_samples;
    info->bitrate         = 0;       // VBR; not signalled in the container
    info->codec           = AUDIO_CODEC_OPUS;
    info->duration_ms     = c->total_samples
        ? (uint32_t)(c->total_samples * 1000 / OPUS_RATE) : 0;
    return DECODE_OK;
}

// Decode one packet into the leftover ring, applying pre-skip + end trimming.
static bool opus_fill(opus_ctx_t *c) {
    ogg_packet op;
    if (!opus_next_packet(c, &op)) { c->eof = true; return false; }

    int ns;
    if (c->is_ms)
        ns = opus_multistream_decode(c->msdec, op.packet, (opus_int32)op.bytes,
                                     c->frame, OPUS_MAX_FRAME, 0);
    else
        ns = opus_decode(c->dec, op.packet, (opus_int32)op.bytes,
                         c->frame, OPUS_MAX_FRAME, 0);
    if (ns <= 0) return false;   // skip bad packet, try again next call

    int start = 0;               // frames to drop from the front (pre-skip)
    if (c->preskip_left > 0) {
        int drop = (c->preskip_left < ns) ? c->preskip_left : ns;
        start = drop;
        c->preskip_left -= drop;
    }
    int avail = ns - start;
    // End trim: never emit past the final granule position.
    if (c->total_samples) {
        uint64_t room = (c->frames_emitted < c->total_samples)
                        ? (c->total_samples - c->frames_emitted) : 0;
        if ((uint64_t)avail > room) avail = (int)room;
    }
    if (avail <= 0) return true;  // consumed entirely by trimming; keep going

    memcpy(c->pcm, &c->frame[start * c->channels],
           (size_t)avail * c->channels * sizeof(int16_t));
    c->pcm_len = avail * c->channels;
    c->pcm_pos = 0;
    c->frames_emitted += (uint64_t)avail;
    return true;
}

static int opus_decode_read(void *context, int16_t *samples, int max_samples) {
    opus_ctx_t *c = (opus_ctx_t *)context;
    if (!c || !samples || max_samples <= 0) return DECODE_ERR_INVALID_DATA;

    int written = 0;
    while (written < max_samples) {
        if (c->pcm_pos < c->pcm_len) {
            int avail = c->pcm_len - c->pcm_pos;
            int want  = max_samples - written;
            int n = (want < avail) ? want : avail;
            memcpy(&samples[written], &c->pcm[c->pcm_pos], n * sizeof(int16_t));
            c->pcm_pos += n;
            written += n;
            if (c->channels > 0) c->current_sample += n / c->channels;
        } else {
            if (c->eof) break;
            if (c->total_samples && c->frames_emitted >= c->total_samples) { c->eof = true; break; }
            if (!opus_fill(c)) { if (c->eof) break; }
        }
    }
    return written;
}

// Best-effort seek: restart the demux from the top and discard up to the target.
static void opus_seek(void *context, uint64_t sample) {
    opus_ctx_t *c = (opus_ctx_t *)context;
    if (!c) return;

    if (c->stream_init) { ogg_stream_clear(&c->os); c->stream_init = false; }
    ogg_sync_reset(&c->oy);
    char *sb = ogg_sync_buffer(&c->oy, c->buf_size);
    if (!sb) return;
    memcpy(sb, c->buf, c->buf_size);
    ogg_sync_wrote(&c->oy, c->buf_size);

    if (c->dec)   opus_decoder_ctl(c->dec, OPUS_RESET_STATE);
    if (c->msdec) opus_multistream_decoder_ctl(c->msdec, OPUS_RESET_STATE);

    c->preskip_left   = c->preskip;
    c->frames_emitted = 0;
    c->current_sample = 0;
    c->pcm_len = c->pcm_pos = 0;
    c->eof = false;

    // Skip the two header packets again.
    ogg_packet op;
    opus_next_packet(c, &op);   // OpusHead
    opus_next_packet(c, &op);   // OpusTags

    // Decode-and-discard up to the requested sample position.
    int16_t tmp[2048];
    while (c->current_sample < sample) {
        uint64_t remain = sample - c->current_sample;
        int want = (int)((remain * c->channels < 2048) ? remain * c->channels : 2048);
        if (want <= 0) break;
        int got = opus_decode_read(c, tmp, want);
        if (got <= 0) break;
    }
}

static bool opus_can_seek_impl(void *context) {
    opus_ctx_t *c = (opus_ctx_t *)context;
    return c != NULL;   // seekable via in-memory restart
}

static uint64_t opus_tell(void *context) {
    opus_ctx_t *c = (opus_ctx_t *)context;
    return c ? c->current_sample : 0;
}

// ============================================================================
// Registration
// ============================================================================

const audio_codec_ops_t opus_codec_ops = {
    .codec      = AUDIO_CODEC_OPUS,
    .name       = "Opus",
    .extensions = "opus",
    .can_decode = opus_can_decode,
    .create     = opus_create,
    .destroy    = opus_destroy,
    .get_info   = opus_get_info,
    .decode     = opus_decode_read,
    .seek       = opus_seek,
    .can_seek   = opus_can_seek_impl,
    .tell       = opus_tell,
};
