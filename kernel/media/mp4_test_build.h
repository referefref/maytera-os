// mp4_test_build.h - shared ISO-BMFF / MP4 (M4A) test-vector builder for the
// #404/#505 mp4_parse Rust-port differential + security self-tests.
//
// Used by BOTH the in-kernel boot self-test (media/aac.c mp4_rust_selftest) and
// the offline pre-flight harness so the two exercise byte-identical vectors.
// Header-only, static inline, no allocations, no libc beyond memcpy semantics
// (open-coded). A tiny box-stack writer emits a minimal-but-valid AAC/M4A atom
// tree: ftyp + moov/trak/mdia/minf/stbl/{stsd/mp4a/esds, stsz, stco|co64, stsc}.
//
// License: MayteraOS (test scaffolding, no third-party code).

#ifndef MP4_TEST_BUILD_H
#define MP4_TEST_BUILD_H

#include <stdint.h>

// Byte offsets of the attacker-controlled sample-table COUNT fields, captured
// during a build so a test can inflate them past the actual table (the reachable
// over-read) without re-deriving the layout.
typedef struct {
    int stsz_count_off;   // offset of the stsz nsamp   field (u32, big-endian)
    int stco_count_off;   // offset of the stco nchunks field
    int stsc_count_off;   // offset of the stsc nstsc   field
    int stsz_body_off;    // stsz body start (for runt-atom crafting)
} mp4_off_t;

typedef struct {
    uint8_t *b;
    int      cap;
    int      p;
    int      sp;
    int      stk[40];
} mp4w_t;

static inline void mp4w_init(mp4w_t *w, uint8_t *b, int cap) {
    w->b = b; w->cap = cap; w->p = 0; w->sp = 0;
}
static inline void mp4w_u8(mp4w_t *w, uint8_t v) {
    if (w->p >= 0 && w->p < w->cap) w->b[w->p] = v;
    w->p++;
}
static inline void mp4w_u16(mp4w_t *w, uint16_t v) { mp4w_u8(w, (uint8_t)(v >> 8)); mp4w_u8(w, (uint8_t)v); }
static inline void mp4w_u32(mp4w_t *w, uint32_t v) {
    mp4w_u8(w, (uint8_t)(v >> 24)); mp4w_u8(w, (uint8_t)(v >> 16));
    mp4w_u8(w, (uint8_t)(v >> 8));  mp4w_u8(w, (uint8_t)v);
}
static inline void mp4w_bytes(mp4w_t *w, const char *d, int n) { for (int i = 0; i < n; i++) mp4w_u8(w, (uint8_t)d[i]); }
static inline void mp4w_box_begin(mp4w_t *w, const char *fourcc) {
    if (w->sp < 40) w->stk[w->sp] = w->p;
    w->sp++;
    mp4w_u32(w, 0);
    mp4w_bytes(w, fourcc, 4);
}
static inline void mp4w_box_end(mp4w_t *w) {
    w->sp--;
    if (w->sp < 0 || w->sp >= 40) return;
    int st = w->stk[w->sp];
    uint32_t sz = (uint32_t)(w->p - st);
    if (st >= 0 && st + 4 <= w->cap) {
        w->b[st + 0] = (uint8_t)(sz >> 24);
        w->b[st + 1] = (uint8_t)(sz >> 16);
        w->b[st + 2] = (uint8_t)(sz >> 8);
        w->b[st + 3] = (uint8_t)sz;
    }
}

// Deterministic LCG so the two harnesses generate identical streams.
static inline uint32_t mp4t_rng(uint32_t *s) { *s = (*s) * 1664525u + 1013904223u; return *s; }

// Build one WELL-FORMED (agreement-domain) AAC/M4A file: every declared count
// exactly matches the table actually present, so mp4_parse_c reads strictly
// in-bounds and mp4_parse_rs must emit a byte-identical frame table. Returns the
// total length. If `off` is non-null it is filled with the count-field offsets.
static inline int mp4_build_valid(uint8_t *b, int cap, uint32_t *rng, mp4_off_t *off) {
    mp4w_t W; mp4w_init(&W, b, cap); mp4w_t *w = &W;
    if (off) { off->stsz_count_off = off->stco_count_off = off->stsc_count_off = off->stsz_body_off = -1; }

    uint32_t nchunks = 1 + (mp4t_rng(rng) % 6);         // 1..6
    uint32_t spc     = 1 + (mp4t_rng(rng) % 3);         // 1..3
    int      is64    = (mp4t_rng(rng) & 8) ? 1 : 0;
    uint32_t uniform = (mp4t_rng(rng) & 16) ? (1u + (mp4t_rng(rng) % 200)) : 0u;
    uint32_t nsamp   = nchunks * spc;

    mp4w_box_begin(w, "ftyp"); mp4w_bytes(w, "isom", 4); mp4w_u32(w, 0); mp4w_bytes(w, "isom", 4); mp4w_box_end(w);

    mp4w_box_begin(w, "moov");
      mp4w_box_begin(w, "trak");
        mp4w_box_begin(w, "mdia");
          mp4w_box_begin(w, "minf");
            mp4w_box_begin(w, "stbl");
              // stsd { version/flags, entry_count=1, mp4a { 28-byte SE hdr, esds { ASC } } }
              mp4w_box_begin(w, "stsd"); mp4w_u32(w, 0); mp4w_u32(w, 1);
                mp4w_box_begin(w, "mp4a");
                  for (int i = 0; i < 28; i++) mp4w_u8(w, 0);
                  mp4w_box_begin(w, "esds"); mp4w_u32(w, 0);
                    mp4w_u8(w, 0x03); mp4w_u8(w, 0x20); mp4w_u16(w, 0); mp4w_u8(w, 0);        // ES_Descr
                    mp4w_u8(w, 0x04); mp4w_u8(w, 0x18); mp4w_u8(w, 0x40); mp4w_u8(w, 0x15);   // DecoderConfig
                    mp4w_u8(w, 0); mp4w_u8(w, 0); mp4w_u8(w, 0);                              // bufferSizeDB(3)
                    mp4w_u32(w, 0); mp4w_u32(w, 0);                                           // maxBR, avgBR
                    mp4w_u8(w, 0x05); mp4w_u8(w, 2); mp4w_u8(w, 0x12); mp4w_u8(w, 0x10);      // DSI (ASC, AAC-LC)
                  mp4w_box_end(w);
                mp4w_box_end(w);
              mp4w_box_end(w);
              // stsz { version/flags, sample_size(uniform), sample_count(nsamp), [sizes] }
              mp4w_box_begin(w, "stsz"); mp4w_u32(w, 0); mp4w_u32(w, uniform);
                if (off) off->stsz_count_off = w->p;
                mp4w_u32(w, nsamp);
                if (off) off->stsz_body_off = w->stk[w->sp - 1] + 8;   // body = box start + 8
                if (!uniform) for (uint32_t i = 0; i < nsamp; i++) mp4w_u32(w, 1u + (mp4t_rng(rng) % 300));
              mp4w_box_end(w);
              // stco/co64 { version/flags, entry_count(nchunks), [offsets] }
              if (is64) {
                mp4w_box_begin(w, "co64"); mp4w_u32(w, 0);
                if (off) off->stco_count_off = w->p;
                mp4w_u32(w, nchunks);
                for (uint32_t i = 0; i < nchunks; i++) { mp4w_u32(w, 0); mp4w_u32(w, (mp4t_rng(rng) % 64)); }
                mp4w_box_end(w);
              } else {
                mp4w_box_begin(w, "stco"); mp4w_u32(w, 0);
                if (off) off->stco_count_off = w->p;
                mp4w_u32(w, nchunks);
                for (uint32_t i = 0; i < nchunks; i++) mp4w_u32(w, (mp4t_rng(rng) % 64));
                mp4w_box_end(w);
              }
              // stsc { version/flags, entry_count=1, {first_chunk=1, spc, sdi=1} }
              mp4w_box_begin(w, "stsc"); mp4w_u32(w, 0);
                if (off) off->stsc_count_off = w->p;
                mp4w_u32(w, 1);
                mp4w_u32(w, 1); mp4w_u32(w, spc); mp4w_u32(w, 1);
              mp4w_box_end(w);
            mp4w_box_end(w);   // stbl
          mp4w_box_end(w);     // minf
        mp4w_box_end(w);       // mdia
      mp4w_box_end(w);         // trak
    mp4w_box_end(w);           // moov
    return w->p;
}

// Overwrite the big-endian u32 at byte offset `o` in buffer `b`.
static inline void mp4_poke_be32(uint8_t *b, int o, uint32_t v) {
    b[o + 0] = (uint8_t)(v >> 24); b[o + 1] = (uint8_t)(v >> 16);
    b[o + 2] = (uint8_t)(v >> 8);  b[o + 3] = (uint8_t)v;
}

#endif // MP4_TEST_BUILD_H
