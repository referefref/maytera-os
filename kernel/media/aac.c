// aac.c - MayteraOS AAC / M4A decoder (real fixed-point decode via vendored faad2)
//
// Implements the audio_codec_ops_t interface from audio_decode.h using faad2
// 2.7 (libfaad) built in INTEGER / fixed-point mode (FIXED_POINT, LC_ONLY_DECODER;
// the kernel is -mno-sse / -mno-sse2, no runtime FPU). faad2's FIXED_POINT path
// (real_t = int32_t, integer iquant/pow/sqrt tables) yields native-endian
// interleaved signed 16-bit PCM, which is what this codec exposes. SBR / PS /
// SSR / MAIN / LTP are excluded (float-heavy), so this is a pure AAC-LC decoder.
//
// Two containers are supported:
//   - ADTS (.aac): raw AAC with self-framing ADTS headers. faad2 NeAACDecInit()
//     syncs to the first ADTS frame; NeAACDecDecode() walks frame by frame.
//   - MP4 / M4A (.m4a): AAC in an ISO-BMFF / MP4 container. A small atom parser
//     (below, no external lib) descends ftyp/moov/trak/mdia/minf/stbl, reads the
//     AudioSpecificConfig from stsd->mp4a->esds, and the per-sample offsets/sizes
//     from stsz + stco/co64 + stsc, then feeds each raw AAC frame to faad2 via
//     NeAACDecInit2(ASC) + NeAACDecDecode().
//
// Vendored sources live in media/faad2/ (faad2 2.7 libfaad LC-only fixed-point
// subset + compat shims). Built with relaxed warnings and the fixed-point config;
// objdump-verified to contain ZERO floating-point / SIMD instructions. Decoding
// works off an in-memory buffer (no file I/O).
//
// License: faad2 is GPLv2 (Copyright (c) Nero AG, www.nero.com). "Code from
// FAAD2 is copyright (c) Nero AG, www.nero.com." See media/faad2/COPYING and
// CHANGELOG.md. Source: faad2-2.7 (sourceforge.net/projects/faac). #331.

#include "audio_decode.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"

#include "faad2/neaacdec.h"

#define AAC_MAX_CHANNELS   8
// AAC-LC frame is 1024 samples/channel; allow headroom (e.g. 960/1024).
#define AAC_FRAME_SAMPLES  2048

typedef struct {
    uint32_t offset;   // absolute byte offset of the AAC frame within buf
    uint32_t size;     // frame size in bytes
} aac_frame_t;

typedef struct {
    uint8_t        *buf;            // owned copy of the whole input
    long            buf_size;

    NeAACDecHandle  dec;
    int             channels;
    int             sample_rate;
    uint64_t        total_samples;  // per channel (estimate; 0 if unknown)
    uint64_t        current_sample; // per channel emitted so far

    bool            is_mp4;

    // ADTS streaming cursor (raw .aac):
    long            pos;            // next byte offset to feed to faad2

    // MP4 frame table (.m4a):
    aac_frame_t    *frames;
    int             nframes;
    int             frame_idx;

    // Leftover interleaved int16 PCM ring (one decoded AAC frame at a time):
    int16_t        *pcm;
    int             pcm_len;        // valid interleaved samples
    int             pcm_pos;        // next interleaved sample to emit
    bool            eof;
    bool            init_done;
} aac_ctx_t;

// ============================================================================
// Big-endian readers for the MP4 / ISO-BMFF container
// ============================================================================

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

// Find a child atom named `type` within [start,end). On success sets *body /
// *body_len to the atom payload (after the 8- or 16-byte header) and returns 1.
static int mp4_find(const uint8_t *d, long start, long end, const char *type,
                    long *body, long *body_len) {
    long i = start;
    while (i + 8 <= end) {
        uint64_t sz = be32(d + i);
        long hdr = 8;
        if (sz == 1) {                       // 64-bit extended size
            if (i + 16 > end) break;
            sz = be64(d + i + 8);
            hdr = 16;
        } else if (sz == 0) {
            sz = (uint64_t)(end - i);        // extends to end of container
        }
        if (sz < (uint64_t)hdr || i + (long)sz > end) break;
        if (memcmp(d + i + 4, type, 4) == 0) {
            *body = i + hdr;
            *body_len = (long)sz - hdr;
            return 1;
        }
        i += (long)sz;
    }
    return 0;
}

// Decode an MPEG-4 expandable descriptor length (each byte: high bit = more).
static uint32_t desc_len(const uint8_t *d, long *p, long end) {
    uint32_t len = 0;
    int n = 0;
    while (*p < end && n < 4) {
        uint8_t b = d[(*p)++];
        len = (len << 7) | (b & 0x7F);
        n++;
        if (!(b & 0x80)) break;
    }
    return len;
}

// Extract the AudioSpecificConfig from an esds box payload [start,end).
// esds layout: version/flags(4) ES_Descr(0x03){ ES_ID(2) flags(1)
// DecoderConfigDescr(0x04){ objType(1) streamType(1) buf(3) maxBR(4) avgBR(4)
// DecoderSpecificInfo(0x05){ <ASC bytes> } } }
static int mp4_extract_asc(const uint8_t *d, long start, long end,
                           const uint8_t **asc, long *asc_len) {
    long p = start + 4;                       // skip version/flags
    if (p >= end || d[p++] != 0x03) return 0; // ES_Descriptor
    desc_len(d, &p, end);
    p += 3;                                   // ES_ID(2) + flags(1)
    if (p >= end || d[p++] != 0x04) return 0; // DecoderConfigDescriptor
    desc_len(d, &p, end);
    p += 1 + 1 + 3 + 4 + 4;                   // objType,streamType,buf,maxBR,avgBR
    if (p >= end || d[p++] != 0x05) return 0; // DecoderSpecificInfo (= ASC)
    uint32_t l = desc_len(d, &p, end);
    if (l == 0 || p + (long)l > end) return 0;
    *asc = d + p;
    *asc_len = (long)l;
    return 1;
}

// ============================================================================
// #404 / #505 Phase Z: MP4/ISO-BMFF sample-table parse seam (mp4_parse_c/_rs).
//
// mp4_parse_c below is the VERBATIM reference (kept for rollback + differential).
// mp4_parse_rs (rustkern.rs) is the same descent + frame-table build with every
// sample-table read bounds-checked, so a crafted stsz/stco/stsc whose declared
// count (nsamp / nchunks / nstsc) exceeds its actual table can no longer walk
// stsz_tab/stco_tab/stsc_tab past the file buffer (the reachable heap OOB READ,
// MAYTERA-SEC-2026-0009). Under -DRUST_MP4 the live aac_create() path parses
// through the Rust. The two-phase call (NULL probe for the count, then a fill
// pass with that exact capacity) mirrors the C's kmalloc(nsamp) then fill.
// Mp4ParseResult mirrors the Rust #[repr(C)] out-struct; the _Static_assert
// locks the FFI layout so it can never silently drift.
typedef struct {
    uint32_t nframes;
    uint32_t asc_off;   // byte offset of the ASC within buf
    uint32_t asc_len;
    uint32_t _pad;
} mp4_parse_result_t;
_Static_assert(sizeof(mp4_parse_result_t) == 16, "mp4_parse_result_t must be 16 bytes for the Rust FFI");
_Static_assert(sizeof(aac_frame_t) == 8, "aac_frame_t must be 8 bytes for the Rust FFI");

extern int mp4_parse_rs(const uint8_t *buf, size_t buf_size,
                        aac_frame_t *frames, uint32_t frames_cap,
                        mp4_parse_result_t *out);

// Parse the MP4 sample tables and build c->frames + the ASC. Returns the ASC
// (pointer into c->buf) via *asc/*asc_len, or 0 on failure.
static int mp4_parse_c(aac_ctx_t *c, const uint8_t **asc, long *asc_len) {
    const uint8_t *d = c->buf;
    long n = c->buf_size;

    long moov, moov_len;
    if (!mp4_find(d, 0, n, "moov", &moov, &moov_len)) return 0;

    // Descend the first audio trak: moov/trak/mdia/minf/stbl. There may be
    // multiple traks; pick the first whose stbl yields an esds (AAC) ASC.
    long search = moov, search_end = moov + moov_len;
    long stbl = -1, stbl_len = 0, found_asc = 0;
    long trak, trak_len;
    long tpos = search;
    while (mp4_find(d, tpos, search_end, "trak", &trak, &trak_len)) {
        long mdia, mdia_len, minf, minf_len, sb, sb_len;
        if (mp4_find(d, trak, trak + trak_len, "mdia", &mdia, &mdia_len) &&
            mp4_find(d, mdia, mdia + mdia_len, "minf", &minf, &minf_len) &&
            mp4_find(d, minf, minf + minf_len, "stbl", &sb, &sb_len)) {
            // Look for stsd->...->esds in this stbl.
            long stsd, stsd_len;
            if (mp4_find(d, sb, sb + sb_len, "stsd", &stsd, &stsd_len)) {
                // stsd: version/flags(4) entry_count(4) then entries. Search the
                // whole stsd payload for an esds (it sits inside mp4a/esds).
                long esds, esds_len;
                if (mp4_find(d, stsd + 8, stsd + stsd_len, "mp4a", &esds, &esds_len)) {
                    // mp4a is a sample entry: 6 reserved + 2 data-ref + 8*2 ver/rev/vendor
                    // + 2 ch + 2 bits + 2 + 2 + 4 samplerate = 28 bytes, then child boxes.
                    long ce, ce_len;
                    if (mp4_find(d, esds + 28, esds + esds_len, "esds", &ce, &ce_len) &&
                        mp4_extract_asc(d, ce, ce + ce_len, asc, asc_len)) {
                        stbl = sb; stbl_len = sb_len; found_asc = 1;
                        break;
                    }
                } else {
                    // Some files put esds directly findable in the subtree.
                    long ce, ce_len;
                    if (mp4_find(d, stsd + 8, stsd + stsd_len, "esds", &ce, &ce_len) &&
                        mp4_extract_asc(d, ce, ce + ce_len, asc, asc_len)) {
                        stbl = sb; stbl_len = sb_len; found_asc = 1;
                        break;
                    }
                }
            }
        }
        tpos = trak + trak_len;
    }
    if (!found_asc || stbl < 0) return 0;

    // --- sample sizes (stsz) ---
    long stsz, stsz_len;
    if (!mp4_find(d, stbl, stbl + stbl_len, "stsz", &stsz, &stsz_len)) return 0;
    uint32_t uniform = be32(d + stsz + 4);
    uint32_t nsamp = be32(d + stsz + 8);
    if (nsamp == 0 || nsamp > 10000000) return 0;

    // --- chunk offsets (stco / co64) ---
    long stco, stco_len; int is64 = 0;
    if (!mp4_find(d, stbl, stbl + stbl_len, "stco", &stco, &stco_len)) {
        if (!mp4_find(d, stbl, stbl + stbl_len, "co64", &stco, &stco_len)) return 0;
        is64 = 1;
    }
    uint32_t nchunks = be32(d + stco + 4);
    if (nchunks == 0 || nchunks > 10000000) return 0;

    // --- sample-to-chunk (stsc) ---
    long stsc, stsc_len;
    if (!mp4_find(d, stbl, stbl + stbl_len, "stsc", &stsc, &stsc_len)) return 0;
    uint32_t nstsc = be32(d + stsc + 4);

    // Build per-sample (offset,size). Walk chunks, distributing samples per the
    // stsc run-length table.
    c->frames = kmalloc((size_t)nsamp * sizeof(aac_frame_t));
    if (!c->frames) return 0;
    c->nframes = 0;

    uint32_t s = 0;            // global sample index
    long stsz_tab = stsz + 12; // per-sample size table (if uniform==0)
    long stco_tab = stco + 8;  // chunk offset table
    long stsc_tab = stsc + 8;  // stsc entries: first_chunk(4) spc(4) sdi(4)

    for (uint32_t chunk = 0; chunk < nchunks && s < nsamp; chunk++) {
        // samples-per-chunk for chunk index (1-based in stsc)
        uint32_t spc = 1;
        for (uint32_t e = 0; e < nstsc; e++) {
            uint32_t first = be32(d + stsc_tab + e * 12);
            uint32_t this_spc = be32(d + stsc_tab + e * 12 + 4);
            uint32_t next_first = (e + 1 < nstsc)
                ? be32(d + stsc_tab + (e + 1) * 12) : 0xFFFFFFFF;
            if (chunk + 1 >= first && chunk + 1 < next_first) { spc = this_spc; break; }
        }
        uint64_t off = is64 ? be64(d + stco_tab + (long)chunk * 8)
                            : be32(d + stco_tab + (long)chunk * 4);
        for (uint32_t k = 0; k < spc && s < nsamp; k++, s++) {
            uint32_t sz = uniform ? uniform : be32(d + stsz_tab + (long)s * 4);
            if (off + sz > (uint64_t)c->buf_size) { sz = 0; }
            c->frames[c->nframes].offset = (uint32_t)off;
            c->frames[c->nframes].size = sz;
            c->nframes++;
            off += sz;
        }
    }
    if (c->nframes == 0) { kfree(c->frames); c->frames = NULL; return 0; }
    return 1;
}

// Live dispatcher. With -DRUST_MP4 (set in the Makefile) the untrusted-.m4a
// sample-table parse runs in Rust (mp4_parse_rs, rustkern.rs) which CONFINES the
// reachable over-read; without it, the verbatim mp4_parse_c above. Two-phase:
// probe (frames==NULL) for the frame count, kmalloc that exact table, then fill.
static int mp4_parse(aac_ctx_t *c, const uint8_t **asc, long *asc_len) {
#ifdef RUST_MP4
    mp4_parse_result_t r;
    r.nframes = r.asc_off = r.asc_len = r._pad = 0;
    if (!mp4_parse_rs(c->buf, (size_t)c->buf_size, NULL, 0, &r)) return 0;
    if (r.nframes == 0) return 0;
    c->frames = kmalloc((size_t)r.nframes * sizeof(aac_frame_t));
    if (!c->frames) return 0;
    mp4_parse_result_t r2;
    r2.nframes = r2.asc_off = r2.asc_len = r2._pad = 0;
    if (!mp4_parse_rs(c->buf, (size_t)c->buf_size, c->frames, r.nframes, &r2) ||
        r2.nframes == 0) {
        kfree(c->frames); c->frames = NULL; return 0;
    }
    c->nframes = (int)r2.nframes;
    *asc = c->buf + r2.asc_off;
    *asc_len = (long)r2.asc_len;
    return 1;
#else
    return mp4_parse_c(c, asc, asc_len);
#endif
}

// ============================================================================
// ADTS detection
// ============================================================================

// A valid ADTS frame starts with syncword 0xFFF and a non-reserved layer (00).
static int is_adts(const uint8_t *b, size_t size) {
    if (size < 7) return 0;
    if (b[0] != 0xFF) return 0;
    if ((b[1] & 0xF6) != 0xF0) return 0;   // 1111 0xx0: sync(12) + layer(00)
    return 1;
}

// ============================================================================
// Codec interface
// ============================================================================

static bool aac_can_decode(const void *data, size_t size) {
    if (!data || size < 8) return false;
    const uint8_t *b = (const uint8_t *)data;
    // MP4 / M4A: an 'ftyp' box at offset 4 (size at 0). Accept generic MP4.
    if (memcmp(b + 4, "ftyp", 4) == 0) return true;
    // Raw ADTS .aac
    if (is_adts(b, size)) return true;
    return false;
}

static void aac_destroy(void *context);

static NeAACDecHandle aac_open_dec(void) {
    NeAACDecHandle h = NeAACDecOpen();
    if (!h) return NULL;
    NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(h);
    if (cfg) {
        cfg->outputFormat = FAAD_FMT_16BIT;
        cfg->downMatrix = 0;
        NeAACDecSetConfiguration(h, cfg);
    }
    return h;
}

static void *aac_create(const void *data, size_t size) {
    if (!data || size < 8) return NULL;

    aac_ctx_t *c = kzalloc(sizeof(aac_ctx_t));
    if (!c) return NULL;
    c->buf_size = (long)size;
    c->buf = kmalloc(size);
    if (!c->buf) { kfree(c); return NULL; }
    memcpy(c->buf, data, size);

    c->dec = aac_open_dec();
    if (!c->dec) { aac_destroy(c); return NULL; }

    unsigned long rate = 0;
    unsigned char ch = 0;

    if (memcmp(c->buf + 4, "ftyp", 4) == 0) {
        // --- MP4 / M4A container ---
        c->is_mp4 = true;
        const uint8_t *asc = NULL; long asc_len = 0;
        if (!mp4_parse(c, &asc, &asc_len)) {
            kprintf("[AAC] MP4 parse failed (no AAC track / sample table)\n");
            aac_destroy(c);
            return NULL;
        }
        char rc = NeAACDecInit2(c->dec, (unsigned char *)asc, (unsigned long)asc_len,
                                &rate, &ch);
        if (rc < 0) {
            kprintf("[AAC] NeAACDecInit2 failed (%d)\n", (int)rc);
            aac_destroy(c);
            return NULL;
        }
        // Estimate total per-channel samples from the frame count (LC = 1024).
        c->total_samples = (uint64_t)c->nframes * 1024;
        kprintf("[AAC] faad2 MP4/M4A: %lu Hz, %u ch, %d frames, ~%llu samples\n",
                rate, (unsigned)ch, c->nframes,
                (unsigned long long)c->total_samples);
    } else {
        // --- Raw ADTS .aac ---
        c->is_mp4 = false;
        long off = NeAACDecInit(c->dec, c->buf, (unsigned long)c->buf_size,
                                &rate, &ch);
        if (off < 0) {
            kprintf("[AAC] NeAACDecInit (ADTS) failed (%ld)\n", off);
            aac_destroy(c);
            return NULL;
        }
        c->pos = off;
        c->total_samples = 0;   // unknown for raw ADTS
        kprintf("[AAC] faad2 ADTS: %lu Hz, %u ch, header offset=%ld\n",
                rate, (unsigned)ch, off);
    }

    c->sample_rate = (int)rate;
    c->channels = (int)ch;
    if (c->channels < 1 || c->channels > AAC_MAX_CHANNELS) {
        kprintf("[AAC] bad channel count %d\n", c->channels);
        aac_destroy(c);
        return NULL;
    }

    c->pcm = kmalloc((size_t)AAC_FRAME_SAMPLES * AAC_MAX_CHANNELS * sizeof(int16_t));
    if (!c->pcm) { aac_destroy(c); return NULL; }
    c->init_done = true;
    return c;
}

static void aac_destroy(void *context) {
    aac_ctx_t *c = (aac_ctx_t *)context;
    if (!c) return;
    if (c->dec)    NeAACDecClose(c->dec);
    if (c->frames) kfree(c->frames);
    if (c->pcm)    kfree(c->pcm);
    if (c->buf)    kfree(c->buf);
    kfree(c);
}

static int aac_get_info(void *context, audio_info_t *info) {
    aac_ctx_t *c = (aac_ctx_t *)context;
    if (!c || !info) return DECODE_ERR_INVALID_DATA;
    info->sample_rate     = (uint32_t)c->sample_rate;
    info->channels        = (uint32_t)c->channels;
    info->bits_per_sample = 16;
    info->total_samples   = c->total_samples;
    info->bitrate         = 0;
    info->codec           = AUDIO_CODEC_AAC;
    info->duration_ms     = (c->total_samples && c->sample_rate)
        ? (uint32_t)(c->total_samples * 1000 / (uint64_t)c->sample_rate) : 0;
    return DECODE_OK;
}

// Decode the next AAC frame into the leftover ring. Returns false at EOF.
static bool aac_fill(aac_ctx_t *c) {
    for (;;) {
        NeAACDecFrameInfo fi;
        memset(&fi, 0, sizeof(fi));
        void *out;

        if (c->is_mp4) {
            if (c->frame_idx >= c->nframes) { c->eof = true; return false; }
            aac_frame_t *f = &c->frames[c->frame_idx++];
            if (f->size == 0) continue;
            out = NeAACDecDecode(c->dec, &fi, c->buf + f->offset, f->size);
        } else {
            if (c->pos >= c->buf_size) { c->eof = true; return false; }
            out = NeAACDecDecode(c->dec, &fi, c->buf + c->pos,
                                 (unsigned long)(c->buf_size - c->pos));
            if (fi.bytesconsumed == 0 && fi.error == 0) { c->eof = true; return false; }
            c->pos += (long)fi.bytesconsumed;
        }

        if (fi.error != 0) {
            // Skip a bad frame and keep going (ADTS resync / MP4 next sample).
            if (!c->is_mp4 && fi.bytesconsumed == 0) { c->eof = true; return false; }
            continue;
        }
        if (!out || fi.samples == 0) continue;   // header-only / silent frame

        int n = (int)fi.samples;                 // interleaved int16 count
        int cap = AAC_FRAME_SAMPLES * AAC_MAX_CHANNELS;
        if (n > cap) n = cap;
        memcpy(c->pcm, out, (size_t)n * sizeof(int16_t));
        c->pcm_len = n;
        c->pcm_pos = 0;
        return true;
    }
}

static int aac_decode_read(void *context, int16_t *samples, int max_samples) {
    aac_ctx_t *c = (aac_ctx_t *)context;
    if (!c || !samples || max_samples <= 0) return DECODE_ERR_INVALID_DATA;

    int written = 0;
    while (written < max_samples) {
        if (c->pcm_pos < c->pcm_len) {
            int avail = c->pcm_len - c->pcm_pos;
            int want  = max_samples - written;
            int n = (want < avail) ? want : avail;
            memcpy(&samples[written], &c->pcm[c->pcm_pos], (size_t)n * sizeof(int16_t));
            c->pcm_pos += n;
            written += n;
            if (c->channels > 0) c->current_sample += n / c->channels;
        } else {
            if (c->eof) break;
            if (!aac_fill(c)) { if (c->eof) break; }
        }
    }
    return written;
}

// Best-effort seek for MP4 (frame table is random-access); ADTS not seekable.
static void aac_seek(void *context, uint64_t sample) {
    aac_ctx_t *c = (aac_ctx_t *)context;
    if (!c || !c->is_mp4) return;
    int target_frame = (int)(sample / 1024);
    if (target_frame < 0) target_frame = 0;
    if (target_frame > c->nframes) target_frame = c->nframes;
    c->frame_idx = target_frame;
    c->pcm_len = c->pcm_pos = 0;
    c->eof = false;
    c->current_sample = (uint64_t)target_frame * 1024;
    NeAACDecPostSeekReset(c->dec, target_frame);
}

static bool aac_can_seek_impl(void *context) {
    aac_ctx_t *c = (aac_ctx_t *)context;
    return c && c->is_mp4;
}

static uint64_t aac_tell(void *context) {
    aac_ctx_t *c = (aac_ctx_t *)context;
    return c ? c->current_sample : 0;
}

// ============================================================================
// Registration
// ============================================================================

const audio_codec_ops_t aac_codec_ops = {
    .codec      = AUDIO_CODEC_AAC,
    .name       = "AAC",
    .extensions = "aac,m4a",
    .can_decode = aac_can_decode,
    .create     = aac_create,
    .destroy    = aac_destroy,
    .get_info   = aac_get_info,
    .decode     = aac_decode_read,
    .seek       = aac_seek,
    .can_seek   = aac_can_seek_impl,
    .tell       = aac_tell,
};

// ============================================================================
// #404 / #505 Phase Z boot-time self-test: prove mp4_parse_rs (Rust, live under
// -DRUST_MP4) == mp4_parse_c (verbatim reference) on the agreement domain
// (well-formed .m4a atom trees + reject cases both handle identically),
// demonstrate the REACHABLE sample-table over-read the Rust confines, and
// micro-benchmark. LIGHT (#426, bounded, runs once): ~256 differential vectors +
// ~300 crafted over-read vectors + a ~3k-iter RDTSC bench. The heavy fuzz (1M+
// vectors + ASan on the C reference proving the real heap OOB) is the OFFLINE
// pre-flight. One [RUST-DIFF] mp4, one [RUST-SEC] mp4, one [RUST-PERF] mp4 line.
#include "mp4_test_build.h"

extern void bootlog_write(const char *fmt, ...);

static inline uint64_t mp4_tsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("xor %%eax,%%eax\n\tcpuid" ::: "eax", "ebx", "ecx", "edx");
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

// Run mp4_parse_c and the mp4_parse_rs two-phase path on the SAME bytes; return
// 1 if the accept/reject verdict + emitted frame table + ASC (offset,len) agree.
static int mp4_diff_eq(const uint8_t *buf, long len) {
    aac_ctx_t cc;
    memset(&cc, 0, sizeof(cc));
    cc.buf = (uint8_t *)buf;
    cc.buf_size = len;
    const uint8_t *asc_c = NULL; long asclen_c = 0;
    int rc_c = mp4_parse_c(&cc, &asc_c, &asclen_c);

    mp4_parse_result_t r; r.nframes = r.asc_off = r.asc_len = r._pad = 0;
    int rc_r = mp4_parse_rs(buf, (size_t)len, NULL, 0, &r);
    aac_frame_t *rf = NULL;
    if (rc_r && r.nframes) {
        rf = kmalloc((size_t)r.nframes * sizeof(aac_frame_t));
        if (rf) {
            mp4_parse_result_t r2; r2.nframes = r2.asc_off = r2.asc_len = r2._pad = 0;
            if (!mp4_parse_rs(buf, (size_t)len, rf, r.nframes, &r2) || r2.nframes == 0) {
                kfree(rf); rf = NULL; rc_r = 0;
            } else {
                r.nframes = r2.nframes; r.asc_off = r2.asc_off; r.asc_len = r2.asc_len;
            }
        } else {
            rc_r = 0;
        }
    }

    int ok = 1;
    if ((rc_c != 0) != (rc_r != 0)) {
        ok = 0;
    } else if (rc_c) {
        if ((int)r.nframes != cc.nframes) ok = 0;
        if (ok && asclen_c != (long)r.asc_len) ok = 0;
        if (ok && (long)(asc_c - buf) != (long)r.asc_off) ok = 0;
        if (ok && rf) {
            for (int i = 0; i < cc.nframes; i++) {
                if (cc.frames[i].offset != rf[i].offset || cc.frames[i].size != rf[i].size) { ok = 0; break; }
            }
        }
    }
    if (cc.frames) { kfree(cc.frames); cc.frames = NULL; }
    if (rf) kfree(rf);
    return ok;
}

void mp4_rust_selftest(void) {
    // 16 KiB arena: the MP4 tree is < 1 KiB and the Part-2 over-read stays inside
    // OUR arena poison tail (never faults; the real heap OOB is proven offline).
    static uint8_t arena[16384];
    uint32_t seed = 0x4d503421; // 'MP4!'
    uint32_t vectors = 0, mism = 0;
    int first_bad = -1;

    // Force-reference the Rust symbol so its archive member always links.
    { mp4_parse_result_t t; t.nframes = 0; mp4_parse_rs(arena, 0, NULL, 0, &t); }

    // Part 1: agreement domain. Well-formed .m4a trees (mixed stco/co64, uniform
    // and per-sample stsz), plus reject cases both handle identically (garbage,
    // too-short, no-moov).
    for (uint32_t iter = 0; iter < 256; iter++) {
        int len;
        uint32_t k = iter & 3;
        if (k == 3) {
            // pure garbage / too short -> both reject
            len = (int)(mp4t_rng(&seed) % 48);
            for (int i = 0; i < len; i++) arena[i] = (uint8_t)(mp4t_rng(&seed) & 0xFF);
        } else if (k == 2) {
            // valid ftyp but truncated so no moov -> both reject
            mp4_off_t o; len = mp4_build_valid(arena, (int)sizeof(arena), &seed, &o);
            if (len > 40) len = 20 + (int)(mp4t_rng(&seed) % 16); // cut inside ftyp/before moov
        } else {
            mp4_off_t o; len = mp4_build_valid(arena, (int)sizeof(arena), &seed, &o);
        }
        vectors++;
        if (!mp4_diff_eq(arena, len)) {
            mism++;
            if (first_bad < 0) first_bad = (int)iter;
        }
    }

    const char *verdict = (mism == 0) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] mp4: %u vectors, %u mismatches -> %s\n", vectors, mism, verdict);
    bootlog_write("[RUST-DIFF] mp4: %u vectors, %u mismatches -> %s", vectors, mism, verdict);
    if (mism != 0) {
        kprintf("[RUST-DIFF] mp4 FIRST MISMATCH iter=%d\n", first_bad);
        bootlog_write("[RUST-DIFF] mp4 FIRST MISMATCH iter=%d", first_bad);
    }

    // Part 2: SECURITY posture - the REACHABLE sample-table over-read. Build a
    // valid tree, then inflate the stco nchunks + stsz nsamp counts FAR beyond
    // the tables actually present (leaving buf_size = the real built length). The
    // verbatim C's chunk/sample walk ignores buf_size: it reads stco_tab/stsz_tab
    // past the received bytes and manufactures frames from the arena poison; the
    // Rust, slicing exactly buf_size, stops at the real table end. Count the
    // observable divergences (the over-read manifesting as extra/garbage frames).
    {
        uint32_t sec_n = 0, overreads = 0;
        uint32_t s2 = 0x51ee7c33;
        for (uint32_t r = 0; r < 300; r++) {
            mp4_off_t o;
            int len = mp4_build_valid(arena, (int)sizeof(arena), &s2, &o);
            if (len <= 0 || o.stco_count_off < 0 || o.stsz_count_off < 0) continue;
            // Poison the arena tail (distinctive nonzero) so the C over-read picks
            // up bytes that could ONLY come from beyond the received length.
            for (int i = len; i < len + 4096 && i < (int)sizeof(arena); i++)
                arena[i] = (uint8_t)(0xA5 ^ (i & 0xFF));
            // Inflate the declared counts well past the real tables (bounded so
            // the over-read stays inside the arena poison, never faults).
            uint32_t bump = 200 + (mp4t_rng(&s2) % 200);
            uint32_t old_nchunks = ((uint32_t)arena[o.stco_count_off] << 24) |
                                   ((uint32_t)arena[o.stco_count_off + 1] << 16) |
                                   ((uint32_t)arena[o.stco_count_off + 2] << 8) |
                                   (uint32_t)arena[o.stco_count_off + 3];
            uint32_t old_nsamp = ((uint32_t)arena[o.stsz_count_off] << 24) |
                                 ((uint32_t)arena[o.stsz_count_off + 1] << 16) |
                                 ((uint32_t)arena[o.stsz_count_off + 2] << 8) |
                                 (uint32_t)arena[o.stsz_count_off + 3];
            mp4_poke_be32(arena, o.stco_count_off, old_nchunks + bump);
            mp4_poke_be32(arena, o.stsz_count_off, old_nsamp + bump * 4);
            sec_n++;
            // Over-read manifests iff the two parsed views now DISAGREE (the C
            // read beyond the received bytes; the Rust confined to them).
            if (!mp4_diff_eq(arena, len)) overreads++;
        }
        kprintf("[RUST-SEC] mp4: verbatim C over-read past buf_size on %u/%u crafted inflated-count .m4a "
                "(stco/stsz walk ignores buf_size -> arena bytes parsed as frames); Rust confined %u/%u\n",
                overreads, sec_n, sec_n, sec_n);
        bootlog_write("[RUST-SEC] mp4: C over-read past buf_size on %u/%u crafted inflated-count .m4a; "
                      "Rust confined %u/%u (REACHABLE OOB removed, MAYTERA-SEC-2026-0009)",
                      overreads, sec_n, sec_n, sec_n);
    }

    // Part 3: RDTSC micro-benchmark over a fixed well-formed .m4a. LIGHT: 3k iters.
    {
        const int iters = 3000;
        uint32_t s3 = 0x6f2a13b8;
        mp4_off_t o;
        int len = mp4_build_valid(arena, (int)sizeof(arena), &s3, &o);
        mp4_parse_result_t rr; rr.nframes = 0;
        aac_ctx_t cc;

        for (int i = 0; i < 200; i++) {
            memset(&cc, 0, sizeof(cc)); cc.buf = arena; cc.buf_size = len;
            const uint8_t *a = NULL; long al = 0;
            if (mp4_parse_c(&cc, &a, &al) && cc.frames) { kfree(cc.frames); }
            mp4_parse_rs(arena, (size_t)len, NULL, 0, &rr);
        }

        uint64_t t0 = mp4_tsc();
        for (int i = 0; i < iters; i++) {
            memset(&cc, 0, sizeof(cc)); cc.buf = arena; cc.buf_size = len;
            const uint8_t *a = NULL; long al = 0;
            if (mp4_parse_c(&cc, &a, &al) && cc.frames) { kfree(cc.frames); }
        }
        uint64_t t1 = mp4_tsc();
        for (int i = 0; i < iters; i++) { mp4_parse_rs(arena, (size_t)len, NULL, 0, &rr); }
        uint64_t t2 = mp4_tsc();

        uint64_t c_cyc = (t1 - t0) / iters;
        uint64_t r_cyc = (t2 - t1) / iters;
        uint64_t ratio100 = (c_cyc != 0) ? (r_cyc * 100ULL / c_cyc) : 0;
        kprintf("[RUST-PERF] mp4: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu\n",
                (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
        bootlog_write("[RUST-PERF] mp4: C=%llu cyc/op RS=%llu cyc/op ratio=%llu.%02llu",
                      (unsigned long long)c_cyc, (unsigned long long)r_cyc,
                      (unsigned long long)(ratio100 / 100), (unsigned long long)(ratio100 % 100));
    }
}
