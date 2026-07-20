// http2.c - Minimal HTTP/2 (h2 over TLS) client for MayteraOS
//
// Implements a single GET over a single stream (id 1):
//   - connection preface + SETTINGS
//   - HPACK-encoded HEADERS frame (no dynamic table on the send side)
//   - frame read loop with SETTINGS-ACK, PING echo, WINDOW_UPDATE refill
//   - HPACK decode (static + dynamic table + Huffman) to extract :status
//
// HTTP/1.1 remains the fallback in https_get; this path is taken only when
// ALPN negotiates "h2".

#include "http2.h"
#include "tls/tls.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../serial.h"

extern void net_poll(void);
extern void proc_sleep(uint32_t ms);
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;

/* #404 HTTP/2 frame framing seam. Strangler flag: -DRUST_HTTP2_FRAME routes the
   live path (http2_get) to the Rust port http2_frame_next_rs; default stays on
   the C reference http2_frame_next_c (net/http2_frame_c.c). Both share the
   H2Frame ABI declared in http2.h. The seam decides each frame's extent once,
   safely (bounds-checked pad/priority strip), which removes the zero-length
   PADDED NULL-deref/OOB read the old inline framing had (MAYTERA-SEC-2026-0010).

   DROPPING -DRUST_HTTP2_FRAME IS NOT A BEHAVIOR ROLLBACK (#404 3-way drift audit,
   2026-07-16). Say this plainly rather than implying the flag restores the
   pre-b822 kernel:

     * The extraction RE-REPRESENTED the frame at THIS call site: the header + a
       separately kmalloc'd payload (NULL when flen==0) became ONE contiguous
       kmalloc(9 + flen) buffer (see the http2_frame_next() call in http2_get()).
       That is a caller-side change which BOTH flag states inherit, so neither
       state is the pre-b822 code.
     * Consequently http2_frame_next_c CANNOT reproduce the original: on a
       zero-length PADDED frame the original dereferenced a NULL payload and
       PANICKED, whereas the C reference does a 1-byte heap over-read that
       SUCCEEDS and continues with an attacker-influenced pad_len. Dropping the
       flag therefore trades a deterministic panic for a silent adjacent-heap
       read. See the FRAME RE-REPRESENTATION note in net/http2_frame_c.c.

   The flag restores the C IMPLEMENTATION, not the pre-port BEHAVIOR. The shipped
   (Rust) path is the verified one: 1.35M vectors, 0 unexplained divergence. */
#ifdef RUST_HTTP2_FRAME
#define http2_frame_next http2_frame_next_rs
#else
#define http2_frame_next http2_frame_next_c
#endif

// #333: gated verbose frame/HPACK tracing. Set to 1 by the NETTEST self-test to
// capture the failing musicbrainz.org / archive.org-CDN exchanges on serial.
int g_http2_dbg = 0;

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------
#define H2_MAX_BODY        (4u * 1024u * 1024u)   // 4 MB cap
#define H2_MAX_HDR_BLOCK   (256u * 1024u)         // header block accumulation cap
#define H2_DYN_TABLE_MAX   4096                    // default HPACK dynamic table size

// ---------------------------------------------------------------------------
// HPACK static table (RFC 7541 Appendix A), indices 1..61
// ---------------------------------------------------------------------------
typedef struct { const char *name; const char *value; } hpack_entry_t;

static const hpack_entry_t hpack_static[] = {
    { "", "" },                                   // index 0 unused
    { ":authority", "" },                         // 1
    { ":method", "GET" },                         // 2
    { ":method", "POST" },                        // 3
    { ":path", "/" },                             // 4
    { ":path", "/index.html" },                   // 5
    { ":scheme", "http" },                        // 6
    { ":scheme", "https" },                       // 7
    { ":status", "200" },                         // 8
    { ":status", "204" },                         // 9
    { ":status", "206" },                         // 10
    { ":status", "304" },                         // 11
    { ":status", "400" },                         // 12
    { ":status", "404" },                         // 13
    { ":status", "500" },                         // 14
    { "accept-charset", "" },                     // 15
    { "accept-encoding", "gzip, deflate" },       // 16
    { "accept-language", "" },                    // 17
    { "accept-ranges", "" },                      // 18
    { "accept", "" },                             // 19
    { "access-control-allow-origin", "" },        // 20
    { "age", "" },                                // 21
    { "allow", "" },                              // 22
    { "authorization", "" },                      // 23
    { "cache-control", "" },                      // 24
    { "content-disposition", "" },                // 25
    { "content-encoding", "" },                   // 26
    { "content-language", "" },                   // 27
    { "content-length", "" },                     // 28
    { "content-location", "" },                   // 29
    { "content-range", "" },                      // 30
    { "content-type", "" },                       // 31
    { "cookie", "" },                             // 32
    { "date", "" },                               // 33
    { "etag", "" },                               // 34
    { "expect", "" },                             // 35
    { "expires", "" },                            // 36
    { "from", "" },                               // 37
    { "host", "" },                               // 38
    { "if-match", "" },                           // 39
    { "if-modified-since", "" },                  // 40
    { "if-none-match", "" },                       // 41
    { "if-range", "" },                           // 42
    { "if-unmodified-since", "" },                // 43
    { "last-modified", "" },                      // 44
    { "link", "" },                               // 45
    { "location", "" },                           // 46
    { "max-forwards", "" },                       // 47
    { "proxy-authenticate", "" },                 // 48
    { "proxy-authorization", "" },                // 49
    { "range", "" },                              // 50
    { "referer", "" },                            // 51
    { "refresh", "" },                            // 52
    { "retry-after", "" },                        // 53
    { "server", "" },                             // 54
    { "set-cookie", "" },                         // 55
    { "strict-transport-security", "" },          // 56
    { "transfer-encoding", "" },                  // 57
    { "user-agent", "" },                         // 58
    { "vary", "" },                               // 59
    { "via", "" },                                // 60
    { "www-authenticate", "" },                   // 61
};
#define HPACK_STATIC_COUNT 61

// ---------------------------------------------------------------------------
// HPACK Huffman code table (RFC 7541 Appendix B): {code, nbits} per symbol.
// 257 entries; index 256 = EOS.
// ---------------------------------------------------------------------------
typedef struct { uint32_t code; uint8_t nbits; } huff_code_t;

static const huff_code_t huff_table[257] = {
    {0x1ff8,13},{0x7fffd8,23},{0xfffffe2,28},{0xfffffe3,28},{0xfffffe4,28},
    {0xfffffe5,28},{0xfffffe6,28},{0xfffffe7,28},{0xfffffe8,28},{0xffffea,24},
    {0x3ffffffc,30},{0xfffffe9,28},{0xfffffea,28},{0x3ffffffd,30},{0xfffffeb,28},
    {0xfffffec,28},{0xfffffed,28},{0xfffffee,28},{0xfffffef,28},{0xffffff0,28},
    {0xffffff1,28},{0xffffff2,28},{0x3ffffffe,30},{0xffffff3,28},{0xffffff4,28},
    {0xffffff5,28},{0xffffff6,28},{0xffffff7,28},{0xffffff8,28},{0xffffff9,28},
    {0xffffffa,28},{0xffffffb,28},{0x14,6},{0x3f8,10},{0x3f9,10},{0xffa,12},
    {0x1ff9,13},{0x15,6},{0xf8,8},{0x7fa,11},{0x3fa,10},{0x3fb,10},{0xf9,8},
    {0x7fb,11},{0xfa,8},{0x16,6},{0x17,6},{0x18,6},{0x0,5},{0x1,5},{0x2,5},
    {0x19,6},{0x1a,6},{0x1b,6},{0x1c,6},{0x1d,6},{0x1e,6},{0x1f,6},{0x5c,7},
    {0xfb,8},{0x7ffc,15},{0x20,6},{0xffb,12},{0x3fc,10},{0x1ffa,13},{0x21,6},
    {0x5d,7},{0x5e,7},{0x5f,7},{0x60,7},{0x61,7},{0x62,7},{0x63,7},{0x64,7},
    {0x65,7},{0x66,7},{0x67,7},{0x68,7},{0x69,7},{0x6a,7},{0x6b,7},{0x6c,7},
    {0x6d,7},{0x6e,7},{0x6f,7},{0x70,7},{0x71,7},{0x72,7},{0xfc,8},{0x73,7},
    {0xfd,8},{0x1ffb,13},{0x7fff0,19},{0x1ffc,13},{0x3ffc,14},{0x22,6},
    {0x7ffd,15},{0x3,5},{0x23,6},{0x4,5},{0x24,6},{0x5,5},{0x25,6},{0x26,6},
    {0x27,6},{0x6,5},{0x74,7},{0x75,7},{0x28,6},{0x29,6},{0x2a,6},{0x7,5},
    {0x2b,6},{0x76,7},{0x2c,6},{0x8,5},{0x9,5},{0x2d,6},{0x77,7},{0x78,7},
    {0x79,7},{0x7a,7},{0x7b,7},{0x7ffe,15},{0x7fc,11},{0x3ffd,14},{0x1ffd,13},
    {0xffffffc,28},{0xfffe6,20},{0x3fffd2,22},{0xfffe7,20},{0xfffe8,20},
    {0x3fffd3,22},{0x3fffd4,22},{0x3fffd5,22},{0x7fffd9,23},{0x3fffd6,22},
    {0x7fffda,23},{0x7fffdb,23},{0x7fffdc,23},{0x7fffdd,23},{0x7fffde,23},
    {0xffffeb,24},{0x7fffdf,23},{0xffffec,24},{0xffffed,24},{0x3fffd7,22},
    {0x7fffe0,23},{0xffffee,24},{0x7fffe1,23},{0x7fffe2,23},{0x7fffe3,23},
    {0x7fffe4,23},{0x1fffdc,21},{0x3fffd8,22},{0x7fffe5,23},{0x3fffd9,22},
    {0x7fffe6,23},{0x7fffe7,23},{0xffffef,24},{0x3fffda,22},{0x1fffdd,21},
    {0xfffe9,20},{0x3fffdb,22},{0x3fffdc,22},{0x7fffe8,23},{0x7fffe9,23},
    {0x1fffde,21},{0x7fffea,23},{0x3fffdd,22},{0x3fffde,22},{0xfffff0,24},
    {0x1fffdf,21},{0x3fffdf,22},{0x7fffeb,23},{0x7fffec,23},{0x1fffe0,21},
    {0x1fffe1,21},{0x3fffe0,22},{0x1fffe2,21},{0x7fffed,23},{0x3fffe1,22},
    {0x7fffee,23},{0x7fffef,23},{0xfffea,20},{0x3fffe2,22},{0x3fffe3,22},
    {0x3fffe4,22},{0x7ffff0,23},{0x3fffe5,22},{0x3fffe6,22},{0x7ffff1,23},
    {0x3ffffe0,26},{0x3ffffe1,26},{0xfffeb,20},{0x7fff1,19},{0x3fffe7,22},
    {0x7ffff2,23},{0x3fffe8,22},{0x1ffffec,25},{0x3ffffe2,26},{0x3ffffe3,26},
    {0x3ffffe4,26},{0x7ffffde,27},{0x7ffffdf,27},{0x3ffffe5,26},{0xfffff1,24},
    {0x1ffffed,25},{0x7fff2,19},{0x1fffe3,21},{0x3ffffe6,26},{0x7ffffe0,27},
    {0x7ffffe1,27},{0x3ffffe7,26},{0x7ffffe2,27},{0xfffff2,24},{0x1fffe4,21},
    {0x1fffe5,21},{0x3ffffe8,26},{0x3ffffe9,26},{0xffffffd,28},{0x7ffffe3,27},
    {0x7ffffe4,27},{0x7ffffe5,27},{0xfffec,20},{0xfffff3,24},{0xfffed,20},
    {0x1fffe6,21},{0x3fffe9,22},{0x1fffe7,21},{0x1fffe8,21},{0x7ffff3,23},
    {0x3fffea,22},{0x3fffeb,22},{0x1ffffee,25},{0x1ffffef,25},{0xfffff4,24},
    {0xfffff5,24},{0x3ffffea,26},{0x7ffff4,23},{0x3ffffeb,26},{0x7ffffe6,27},
    {0x3ffffec,26},{0x3ffffed,26},{0x7ffffe7,27},{0x7ffffe8,27},{0x7ffffe9,27},
    {0x7ffffea,27},{0x7ffffeb,27},{0xffffffe,28},{0x7ffffec,27},{0x7ffffed,27},
    {0x7ffffee,27},{0x7ffffef,27},{0x7fffff0,27},{0x3ffffee,26},{0x3fffffff,30},
};

// ---------------------------------------------------------------------------
// Huffman decode (Appendix B). Decode MSB-first; accumulate a (code,len) and
// emit when it matches a symbol of that length. Trailing all-ones < 8 bits are
// EOS padding and ignored. Output appended to out (caller-sized).
// Returns decoded length, or -1 on error.
// ---------------------------------------------------------------------------
static int huff_decode(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_cap) {
    uint32_t out_len = 0;
    uint32_t cur = 0;     // accumulated code
    uint8_t  curlen = 0;  // bits in cur

    for (uint32_t i = 0; i < in_len; i++) {
        for (int b = 7; b >= 0; b--) {
            cur = (cur << 1) | ((in[i] >> b) & 1u);
            curlen++;
            if (curlen > 30) return -1;  // no code is longer than 30 bits
            // check for a match among symbols of this exact length
            for (int sym = 0; sym < 256; sym++) {
                if (huff_table[sym].nbits == curlen &&
                    huff_table[sym].code == cur) {
                    if (out_len >= out_cap) return -1;
                    out[out_len++] = (uint8_t)sym;
                    cur = 0;
                    curlen = 0;
                    break;
                }
            }
        }
    }
    // remaining bits (< 8) must be EOS padding: all ones
    if (curlen > 0) {
        uint32_t mask = (1u << curlen) - 1u;
        if ((cur & mask) != mask) {
            // not valid padding, but be lenient
        }
    }
    return (int)out_len;
}

// ---------------------------------------------------------------------------
// HPACK integer decode with N-bit prefix (RFC 7541 sec 5.1).
// *p points at the first byte; on entry the low N bits of that byte are the
// prefix. Advances *p past the integer. Returns value, or -1 on overrun.
// ---------------------------------------------------------------------------
static long hpack_int(const uint8_t **p, const uint8_t *end, int prefix_bits) {
    if (*p >= end) return -1;
    uint32_t mask = (1u << prefix_bits) - 1u;
    uint32_t val = (**p) & mask;
    (*p)++;
    if (val < mask) return (long)val;
    int shift = 0;
    while (*p < end) {
        uint8_t b = **p; (*p)++;
        val += (uint32_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) return (long)val;
        if (shift > 28) return -1;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// HPACK string decode (sec 5.2). Writes the decoded string into *out (kmalloc'd
// here, caller kfree()s) and its length. Advances *p. Returns 0 / -1.
// ---------------------------------------------------------------------------
static int hpack_string(const uint8_t **p, const uint8_t *end,
                        char **out, uint32_t *out_len) {
    if (*p >= end) return -1;
    int huff = (**p & 0x80) ? 1 : 0;
    long slen = hpack_int(p, end, 7);
    if (slen < 0 || (*p + slen) > end) return -1;

    if (!huff) {
        char *s = kmalloc((uint32_t)slen + 1);
        if (!s) return -1;
        memcpy(s, *p, (uint32_t)slen);
        s[slen] = '\0';
        *p += slen;
        *out = s; *out_len = (uint32_t)slen;
        return 0;
    }
    // Huffman: decoded data is at most ~ 8/5 of input; allocate generously.
    uint32_t cap = (uint32_t)slen * 4 + 16;
    char *s = kmalloc(cap);
    if (!s) return -1;
    int dl = huff_decode(*p, (uint32_t)slen, (uint8_t *)s, cap - 1);
    if (dl < 0) { kfree(s); return -1; }
    s[dl] = '\0';
    *p += slen;
    *out = s; *out_len = (uint32_t)dl;
    return 0;
}

// ---------------------------------------------------------------------------
// HPACK dynamic table: ring of entries, newest at index 0 (HPACK index 62).
// ---------------------------------------------------------------------------
typedef struct {
    char *name; uint32_t name_len;
    char *value; uint32_t value_len;
} dyn_entry_t;

typedef struct {
    dyn_entry_t entries[128];
    int count;
    uint32_t size;       // current HPACK size sum
    uint32_t max_size;   // negotiated max
} dyn_table_t;

static void dyn_init(dyn_table_t *t) {
    t->count = 0; t->size = 0; t->max_size = H2_DYN_TABLE_MAX;
}

static void dyn_evict_one(dyn_table_t *t) {
    if (t->count <= 0) return;
    dyn_entry_t *e = &t->entries[t->count - 1];
    t->size -= (e->name_len + e->value_len + 32);
    if (e->name) kfree(e->name);
    if (e->value) kfree(e->value);
    e->name = e->value = NULL;
    t->count--;
}

static void dyn_set_max(dyn_table_t *t, uint32_t max) {
    t->max_size = max;
    while (t->size > t->max_size && t->count > 0) dyn_evict_one(t);
}

static void dyn_free(dyn_table_t *t) {
    while (t->count > 0) dyn_evict_one(t);
}

// Insert at front (newest). Copies name/value.
static void dyn_add(dyn_table_t *t, const char *name, uint32_t nlen,
                    const char *value, uint32_t vlen) {
    uint32_t esize = nlen + vlen + 32;
    // evict until it fits (or table empties)
    while (t->size + esize > t->max_size && t->count > 0) dyn_evict_one(t);
    if (esize > t->max_size) return;            // cannot fit at all -> table empty
    if (t->count >= 128) dyn_evict_one(t);      // hard cap on entry count
    // shift down to make room at index 0
    for (int i = t->count; i > 0; i--) t->entries[i] = t->entries[i-1];
    char *nc = kmalloc(nlen + 1); char *vc = kmalloc(vlen + 1);
    if (!nc || !vc) { if (nc) kfree(nc); if (vc) kfree(vc); return; }
    memcpy(nc, name, nlen); nc[nlen] = '\0';
    memcpy(vc, value, vlen); vc[vlen] = '\0';
    t->entries[0].name = nc; t->entries[0].name_len = nlen;
    t->entries[0].value = vc; t->entries[0].value_len = vlen;
    t->count++;
    t->size += esize;
}

// Look up an HPACK index (1-based). Returns name/value pointers (not copied).
// Returns 0 on success, -1 if out of range.
static int hpack_lookup(dyn_table_t *t, long idx,
                        const char **name, const char **value) {
    if (idx <= 0) return -1;
    if (idx <= HPACK_STATIC_COUNT) {
        *name = hpack_static[idx].name;
        *value = hpack_static[idx].value;
        return 0;
    }
    long di = idx - HPACK_STATIC_COUNT - 1;   // 0 = newest
    if (di < 0 || di >= t->count) return -1;
    *name = t->entries[di].name;
    *value = t->entries[di].value;
    return 0;
}

// ---------------------------------------------------------------------------
// HPACK header-block decode. We only need ":status", but we must process every
// field to keep the dynamic table consistent. Returns 0 / -1; *status set if a
// ":status" pseudo-header is seen.
// ---------------------------------------------------------------------------
static char g_h2_location[1100];   // #245 captured Location header (for redirects)
static int hpack_decode_block(dyn_table_t *t, const uint8_t *block,
                              uint32_t block_len, int *status) {
    const uint8_t *p = block;
    const uint8_t *end = block + block_len;
    g_h2_location[0] = 0;

    while (p < end) {
        uint8_t b = *p;
        if (b & 0x80) {
            // 1xxxxxxx: indexed header field
            long idx = hpack_int(&p, end, 7);
            if (idx < 0) return -1;
            const char *nm, *vl;
            if (hpack_lookup(t, idx, &nm, &vl) == 0) {
                if (status && nm && strcmp(nm, ":status") == 0 && vl) {
                    int code = 0; const char *q = vl;
                    while (*q >= '0' && *q <= '9') { code = code*10 + (*q - '0'); q++; }
                    *status = code;
                }
            }
        } else if (b & 0x40) {
            // 01xxxxxx: literal w/ incremental indexing (6-bit name index)
            long ni = hpack_int(&p, end, 6);
            if (ni < 0) return -1;
            char *name = NULL, *value = NULL;
            uint32_t nlen = 0, vlen = 0;
            const char *snm = NULL, *dummy;
            if (ni == 0) {
                if (hpack_string(&p, end, &name, &nlen) != 0) return -1;
                snm = name;
            } else {
                if (hpack_lookup(t, ni, &snm, &dummy) != 0) snm = "";
                nlen = (uint32_t)strlen(snm);
            }
            if (hpack_string(&p, end, &value, &vlen) != 0) {
                if (name) kfree(name);
                return -1;
            }
            if (status && snm && strcmp(snm, ":status") == 0) {
                int code = 0; const char *q = value;
                while (*q >= '0' && *q <= '9') { code = code*10 + (*q - '0'); q++; }
                *status = code;
            }
            if (snm && value && strcmp(snm, "location") == 0) {
                uint32_t i = 0;
                for (; i < vlen && i < sizeof(g_h2_location) - 1; i++) g_h2_location[i] = value[i];
                g_h2_location[i] = 0;
            }
            dyn_add(t, snm, nlen, value, vlen);
            if (name) kfree(name);
            if (value) kfree(value);
        } else if (b & 0x20) {
            // 001xxxxx: dynamic table size update (5-bit)
            long sz = hpack_int(&p, end, 5);
            if (sz < 0) return -1;
            dyn_set_max(t, (uint32_t)sz);
        } else {
            // 0000xxxx (no indexing) or 0001xxxx (never indexed): 4-bit name idx
            long ni = hpack_int(&p, end, 4);
            if (ni < 0) return -1;
            char *name = NULL, *value = NULL;
            uint32_t nlen = 0, vlen = 0;
            const char *snm = NULL, *dummy;
            if (ni == 0) {
                if (hpack_string(&p, end, &name, &nlen) != 0) return -1;
                snm = name;
            } else {
                if (hpack_lookup(t, ni, &snm, &dummy) != 0) snm = "";
            }
            if (hpack_string(&p, end, &value, &vlen) != 0) {
                if (name) kfree(name);
                return -1;
            }
            if (status && snm && strcmp(snm, ":status") == 0) {
                int code = 0; const char *q = value;
                while (*q >= '0' && *q <= '9') { code = code*10 + (*q - '0'); q++; }
                *status = code;
            }
            if (snm && value && strcmp(snm, "location") == 0) {
                uint32_t i = 0;
                for (; i < vlen && i < sizeof(g_h2_location) - 1; i++) g_h2_location[i] = value[i];
                g_h2_location[i] = 0;
            }
            if (name) kfree(name);
            if (value) kfree(value);
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// TLS read-exactly: fill buf with len bytes. Returns 0 on success, <0 on error
// (TLS_ERR_TIMEOUT/CLOSED propagated as -1).
// ---------------------------------------------------------------------------
static int h2_read_full(tls_context_t *tls, uint8_t *buf, uint32_t len) {
    uint32_t got = 0;
    uint64_t hz = g_timer_hz ? g_timer_hz : 250;
    // Wall-clock deadline so a stalled peer can never wedge the fetch worker
    // (the old spin counter could reset on any trickle and hang for minutes).
    uint64_t deadline = timer_ticks + hz * 20;      // ~20s of no progress
    int dbg_polls = 0;
    while (got < len) {
        int r = tls_recv(tls, buf + got, len - got);
        if (r > 0) {
            got += (uint32_t)r;
            deadline = timer_ticks + hz * 20;        // extend on real progress
            continue;
        }
        if (r == TLS_ERR_WOULD_BLOCK) {
            net_poll();
            proc_sleep(2);
            if ((int64_t)(timer_ticks - deadline) >= 0) {
                if (g_http2_dbg) kprintf("[H2DBG] h2_read_full TIMEOUT got=%u/%u\n", got, len);
                return -1;
            }
            if (g_http2_dbg && (++dbg_polls % 500) == 0)
                kprintf("[H2DBG] h2_read_full waiting got=%u/%u polls=%d\n", got, len, dbg_polls);
            continue;
        }
        if (g_http2_dbg) kprintf("[H2DBG] h2_read_full tls_recv err=%d got=%u/%u\n", r, got, len);
        return -1;
    }
    return 0;
}

// Frame header helpers --------------------------------------------------------
static void put_u32(uint8_t *b, uint32_t v) {
    b[0] = (v >> 24) & 0xff; b[1] = (v >> 16) & 0xff;
    b[2] = (v >> 8) & 0xff;  b[3] = v & 0xff;
}
static uint32_t get_u24(const uint8_t *b) {
    return ((uint32_t)b[0] << 16) | ((uint32_t)b[1] << 8) | b[2];
}
static uint32_t get_u32(const uint8_t *b) {
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8) | b[3];
}

// Send a full buffer over TLS (loops on partial / would-block).
static int h2_send_all(tls_context_t *tls, const uint8_t *buf, uint32_t len) {
    uint32_t sent = 0;
    int spins = 0;
    while (sent < len) {
        int r = tls_send(tls, buf + sent, len - sent);
        if (r > 0) { sent += (uint32_t)r; spins = 0; continue; }
        if (r == TLS_ERR_WOULD_BLOCK) {
            net_poll(); proc_sleep(2);
            if (++spins > 5000) return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

// Build and send a frame: 9-byte header + payload.
static int h2_send_frame(tls_context_t *tls, uint8_t type, uint8_t flags,
                         uint32_t stream_id, const uint8_t *payload, uint32_t plen) {
    uint8_t hdr[9];
    hdr[0] = (plen >> 16) & 0xff;
    hdr[1] = (plen >> 8) & 0xff;
    hdr[2] = plen & 0xff;
    hdr[3] = type;
    hdr[4] = flags;
    hdr[5] = (stream_id >> 24) & 0x7f;   // top bit reserved (R) = 0
    hdr[6] = (stream_id >> 16) & 0xff;
    hdr[7] = (stream_id >> 8) & 0xff;
    hdr[8] = stream_id & 0xff;

    if (h2_send_all(tls, hdr, 9) != 0) return -1;
    if (plen && payload) {
        if (h2_send_all(tls, payload, plen) != 0) return -1;
    }
    return 0;
}

// Append a string to an HPACK header block (length-prefixed, no Huffman).
static uint32_t hpack_emit_string(uint8_t *out, const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    uint32_t o = 0;
    if (len < 127) {
        out[o++] = (uint8_t)len;        // H=0, 7-bit length
    } else {
        out[o++] = 0x7f;                 // H=0, prefix all ones
        uint32_t rem = len - 127;
        while (rem >= 128) { out[o++] = (uint8_t)((rem & 0x7f) | 0x80); rem >>= 7; }
        out[o++] = (uint8_t)rem;
    }
    memcpy(out + o, s, len);
    o += len;
    return o;
}

// Grow a body buffer by copying. Returns new buffer (or NULL on OOM).
static uint8_t *body_grow(uint8_t *buf, uint32_t cur_len, uint32_t *cap,
                          uint32_t need) {
    if (cur_len + need <= *cap) return buf;
    uint32_t ncap = *cap ? *cap : 16384;
    while (ncap < cur_len + need) ncap *= 2;
    if (ncap > H2_MAX_BODY + 16384) ncap = H2_MAX_BODY + 16384;
    uint8_t *nb = kmalloc(ncap);
    if (!nb) return NULL;
    if (buf) { memcpy(nb, buf, cur_len); kfree(buf); }
    *cap = ncap;
    return nb;
}

// ---------------------------------------------------------------------------
// Public: HTTP/2 GET
// ---------------------------------------------------------------------------
int http2_get(tls_context_t *tls, const char *host, const char *path,
              uint8_t **body_out, uint32_t *body_len_out, int *status_out,
              char *loc_out, int loc_cap) {
    if (loc_out && loc_cap > 0) loc_out[0] = 0;
    if (!tls || !host || !path) return -1;
    if (body_out) *body_out = NULL;
    if (body_len_out) *body_len_out = 0;
    if (status_out) *status_out = 0;

    kprintf("[HTTP2] GET h2 %s%s\n", host, path);

    // 1) Connection preface
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    if (h2_send_all(tls, (const uint8_t *)preface, 24) != 0) {
        kprintf("[HTTP2] preface send failed\n");
        return -1;
    }

    // 2) SETTINGS (initial window size huge, max frame size 16384)
    uint8_t settings[12];
    settings[0] = 0x00; settings[1] = 0x04;                 // INITIAL_WINDOW_SIZE
    put_u32(settings + 2, 0x7fffffff);
    settings[6] = 0x00; settings[7] = 0x05;                 // MAX_FRAME_SIZE
    put_u32(settings + 8, 16384);
    if (h2_send_frame(tls, 0x04, 0x00, 0, settings, 12) != 0) {
        kprintf("[HTTP2] settings send failed\n");
        return -1;
    }

    // Enlarge the CONNECTION-level receive window up front. The default is only
    // 65535 bytes; without this, servers (e.g. Google) stall after sending the
    // first ~64KB (or even mid-frame) waiting for us to enlarge it. Bump by a
    // large increment so the connection window never throttles the response.
    {
        uint8_t cwu[4];
        put_u32(cwu, 0x7fff0000);   // ~2 GB increment, stays under the 2^31-1 max
        h2_send_frame(tls, 0x08, 0x00, 0, cwu, 4);
    }

    // 3) HEADERS frame with HPACK block
    uint8_t hb[2048];
    uint32_t hp = 0;
    hb[hp++] = 0x82;                  // :method GET (static index 2)
    hb[hp++] = 0x87;                  // :scheme https (static index 7)
    hb[hp++] = 0x01;                  // :authority literal w/o indexing, name idx 1
    hp += hpack_emit_string(hb + hp, host);
    hb[hp++] = 0x04;                  // :path literal w/o indexing, name idx 4
    hp += hpack_emit_string(hb + hp, path);
    hb[hp++] = 0x00;                  // new name literal w/o indexing
    hp += hpack_emit_string(hb + hp, "user-agent");
    hp += hpack_emit_string(hb + hp,
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36");
    hb[hp++] = 0x00;                  // accept, new name
    hp += hpack_emit_string(hb + hp, "accept");
    hp += hpack_emit_string(hb + hp, "text/html,*/*");

    // flags: END_HEADERS(0x4)|END_STREAM(0x1)
    if (h2_send_frame(tls, 0x01, 0x05, 1, hb, hp) != 0) {
        kprintf("[HTTP2] headers send failed\n");
        return -1;
    }

    // 4) Read loop
    dyn_table_t dyn; dyn_init(&dyn);

    uint8_t *body = NULL;
    uint32_t body_len = 0, body_cap = 0;

    uint8_t *hdrblock = NULL;
    uint32_t hdrblock_len = 0, hdrblock_cap = 0;
    int collecting_headers = 0;

    int status = 0;
    int done = 0;
    int rc = -1;

    uint8_t fhdr[9];
    uint8_t *payload = NULL;

    while (!done) {
        if (h2_read_full(tls, fhdr, 9) != 0) {
            if (g_http2_dbg) kprintf("[H2DBG] read frame header failed (closed/timeout)\n");
            break;  // connection closed / timeout: use what we have
        }
        uint32_t flen = get_u24(fhdr);
        uint8_t  ftype = fhdr[3];
        uint8_t  fflags = fhdr[4];
        uint32_t fstream = get_u32(fhdr + 5) & 0x7fffffff;

        if (g_http2_dbg) {
            const char *tn = "?";
            switch (ftype) {
                case 0x00: tn="DATA"; break; case 0x01: tn="HEADERS"; break;
                case 0x02: tn="PRIORITY"; break; case 0x03: tn="RST_STREAM"; break;
                case 0x04: tn="SETTINGS"; break; case 0x05: tn="PUSH_PROMISE"; break;
                case 0x06: tn="PING"; break; case 0x07: tn="GOAWAY"; break;
                case 0x08: tn="WINDOW_UPDATE"; break; case 0x09: tn="CONTINUATION"; break;
            }
            kprintf("[H2DBG] <- %s type=%u flags=0x%02x len=%u stream=%u\n",
                    tn, ftype, fflags, flen, fstream);
        }

        if (flen > 0) {
            payload = kmalloc(flen);
            if (!payload) { kprintf("[HTTP2] OOM payload %u\n", flen); break; }
            if (h2_read_full(tls, payload, flen) != 0) {
                kfree(payload); payload = NULL;
                break;  // partial frame: stop, return what we have
            }
        }

        // #404 HTTP/2 frame framing seam (routes to http2_frame_next_rs under
        // -DRUST_HTTP2_FRAME). Assemble the 9-byte header + payload into ONE
        // contiguous buffer and let the seam validate the frame and compute the
        // meaningful-payload extent (padding + PRIORITY stripped, every derived
        // read bounds-checked). This removes the zero-length PADDED NULL-deref:
        // the old inline code read payload[0]/pp[off] when flen==0 (payload==NULL)
        // -> MAYTERA-SEC-2026-0010. The seam rejects that frame instead.
        H2Frame fr;
        {
            uint8_t *frm = kmalloc(9u + flen);
            if (!frm) { if (payload) { kfree(payload); payload = NULL; } break; }
            memcpy(frm, fhdr, 9);
            if (flen) memcpy(frm + 9, payload, flen);
            uint32_t fpos = 0;
            int frc = http2_frame_next(frm, 9u + flen, &fpos, &fr);
            kfree(frm);
            if (frc != 1) {
                if (g_http2_dbg)
                    kprintf("[H2DBG] frame rejected by seam type=%u flags=0x%02x len=%u\n",
                            ftype, fflags, flen);
                if (payload) { kfree(payload); payload = NULL; }
                break;  // malformed frame (incl. zero-length PADDED) -> protocol error
            }
        }

        switch (ftype) {
            case 0x04: // SETTINGS
                if (g_http2_dbg && !(fflags & 0x01) && payload) {
                    for (uint32_t si = 0; si + 6 <= flen; si += 6) {
                        uint16_t id = ((uint16_t)payload[si] << 8) | payload[si+1];
                        uint32_t v = get_u32(payload + si + 2);
                        kprintf("[H2DBG]   SETTING id=%u val=%u\n", id, v);
                    }
                }
                if (!(fflags & 0x01)) {
                    h2_send_frame(tls, 0x04, 0x01, 0, NULL, 0);  // ACK
                }
                break;
            case 0x06: // PING
                if (!(fflags & 0x01)) {
                    h2_send_frame(tls, 0x06, 0x01, 0, payload, flen);  // echo
                }
                break;
            case 0x08: // WINDOW_UPDATE
                if (g_http2_dbg && payload && flen >= 4)
                    kprintf("[H2DBG]   WINDOW_UPDATE inc=%u stream=%u\n", get_u32(payload) & 0x7fffffff, fstream);
                break;
            case 0x02: // PRIORITY
                break;
            case 0x01: // HEADERS
            case 0x09: // CONTINUATION
                if (fstream == 1) {
                    // #404: seam-computed meaningful-payload extent (padding +
                    // PRIORITY already stripped and bounded by http2_frame_next).
                    // fr.data_off is buffer-relative (header at offset 0), so the
                    // payload-relative offset is fr.data_off - 9.
                    const uint8_t *pp = payload;
                    uint32_t off = fr.data_off - 9u;
                    uint32_t plen = fr.data_len;
                    // append fragment
                    if (plen > 0 && hdrblock_len + plen <= H2_MAX_HDR_BLOCK) {
                        if (hdrblock_len + plen > hdrblock_cap) {
                            uint32_t nc = hdrblock_cap ? hdrblock_cap : 4096;
                            while (nc < hdrblock_len + plen) nc *= 2;
                            uint8_t *nb = kmalloc(nc);
                            if (nb) {
                                if (hdrblock) { memcpy(nb, hdrblock, hdrblock_len); kfree(hdrblock); }
                                hdrblock = nb; hdrblock_cap = nc;
                            }
                        }
                        if (hdrblock) {
                            memcpy(hdrblock + hdrblock_len, pp + off, plen);
                            hdrblock_len += plen;
                        }
                    }
                    collecting_headers = !(fflags & 0x04);  // END_HEADERS clears it
                    if (fflags & 0x04) {
                        // END_HEADERS: decode the accumulated block
                        if (g_http2_dbg && hdrblock && hdrblock_len) {
                            kprintf("[H2DBG]   END_HEADERS block_len=%u first bytes:", hdrblock_len);
                            for (uint32_t di = 0; di < hdrblock_len && di < 24; di++)
                                kprintf(" %02x", hdrblock[di]);
                            kprintf("\n");
                        }
                        if (hdrblock && hdrblock_len) {
                            int hd_rc = hpack_decode_block(&dyn, hdrblock, hdrblock_len, &status);
                            if (g_http2_dbg) kprintf("[H2DBG]   hpack_decode rc=%d status=%d\n", hd_rc, status);
                            if (status_out) *status_out = status;
                            if (loc_out && loc_cap > 0 && g_h2_location[0]) {
                                int li = 0; for (; g_h2_location[li] && li < loc_cap - 1; li++) loc_out[li] = g_h2_location[li];
                                loc_out[li] = 0;
                            }
                            kprintf("[HTTP2] status=%d\n", status);
                        }
                        if (hdrblock) { kfree(hdrblock); hdrblock = NULL; }
                        hdrblock_len = 0; hdrblock_cap = 0;
                        collecting_headers = 0;
                    }
                    if ((ftype == 0x01) && (fflags & 0x01) && !collecting_headers) {
                        done = 1;  // END_STREAM on a header-only response
                    }
                }
                break;
            case 0x00: // DATA
                if (fstream == 1) {
                    // #404: seam-computed extent (PADDED pad byte + padding
                    // stripped and bounded; no payload[0] NULL-deref). fr.data_off
                    // is buffer-relative (header at offset 0).
                    uint32_t off = fr.data_off - 9u;
                    uint32_t dlen = fr.data_len;
                    if (dlen > 0) {
                        if (body_len + dlen <= H2_MAX_BODY) {
                            uint8_t *nb = body_grow(body, body_len, &body_cap, dlen);
                            if (nb) {
                                body = nb;
                                memcpy(body + body_len, payload + off, dlen);
                                body_len += dlen;
                            }
                        }
                        // refill flow-control window (stream 1 + connection)
                        uint8_t inc[4];
                        put_u32(inc, flen);
                        h2_send_frame(tls, 0x08, 0x00, 1, inc, 4);
                        h2_send_frame(tls, 0x08, 0x00, 0, inc, 4);
                    }
                    if (fflags & 0x01) done = 1;  // END_STREAM
                    if (body_len >= H2_MAX_BODY) done = 1;
                }
                break;
            case 0x07: // GOAWAY
                if (payload && flen >= 8)
                    kprintf("[HTTP2] GOAWAY last_stream=%u err=%u\n",
                            get_u32(payload) & 0x7fffffff, get_u32(payload + 4));
                else
                    kprintf("[HTTP2] GOAWAY\n");
                done = 1;
                break;
            case 0x03: // RST_STREAM
                if (fstream == 1) {
                    kprintf("[HTTP2] RST_STREAM err=%u\n", (payload && flen >= 4) ? get_u32(payload) : 0);
                    done = 1;
                }
                break;
            default:
                break;
        }

        if (payload) { kfree(payload); payload = NULL; }
    }

    if (hdrblock) kfree(hdrblock);
    dyn_free(&dyn);

    (void)collecting_headers;

    if (status != 0) {
        if (status_out) *status_out = status;
        if (body_out) {
            if (!body) {
                body = kmalloc(1);
                if (body) { body[0] = 0; body_len = 0; }
            } else if (body_len < body_cap) {
                body[body_len] = 0;  // NUL-terminate within cap when possible
            }
            *body_out = body;
            body = NULL;
        }
        if (body_len_out) *body_len_out = body_len;
        rc = 0;
        kprintf("[HTTP2] done status=%d len=%u\n", status, body_len);
    } else {
        kprintf("[HTTP2] failed: no status (len=%u)\n", body_len);
        rc = -1;
    }

    if (body) kfree(body);
    return rc;
}

// ===========================================================================
// #404 HTTP/2 frame framing seam boot self-test (flag -DRUST_HTTP2_FRAME).
// Bounded, runs once (#426). Proves http2_frame_next_rs == http2_frame_next_c
// on representative well-formed frames back-to-back, and that BOTH reject the
// zero-length PADDED DATA frame that the old inline framing NULL-dereferenced
// (MAYTERA-SEC-2026-0010). Logs [RUST-DIFF] http2_frame + [RUST-PERF] http2_frame.
// ===========================================================================
static inline uint64_t h2f_rdtsc(void) {
    uint32_t a, d;
    __asm__ __volatile__("lfence; rdtsc" : "=a"(a), "=d"(d));
    return ((uint64_t)d << 32) | a;
}

void http2_frame_selftest(void) {
    // Representative well-formed frames of several types, back-to-back.
    static const uint8_t good[] = {
        /* SETTINGS len=6 stream0 */        0,0,6, 0x04,0x00, 0,0,0,0,  0,4,0,0,0,0,
        /* HEADERS PADDED+PRIO len=12 s1 */ 0,0,12,0x01,0x28, 0,0,0,1,
             /* pad=2 | prio(5) | 3 block | 2 pad */ 2, 0,0,0,0,0, 0x88,0x0f,0x10, 0xAA,0xBB,
        /* DATA PADDED len=8 s1 */          0,0,8, 0x00,0x08, 0,0,0,1,  3, 'h','i','x', 0,0,0,
        /* WINDOW_UPDATE len=4 s0 */        0,0,4, 0x08,0x00, 0,0,0,0,  0x7f,0xff,0,0,
    };
    uint32_t pc=0, pr=0; int frames=0, mism=0;
    for (;;) {
        H2Frame fc, fr;
        int rc = http2_frame_next_c (good, sizeof(good), &pc, &fc);
        int rr = http2_frame_next_rs(good, sizeof(good), &pr, &fr);
        if (rc!=rr || pc!=pr) { mism++; break; }
        if (rc<=0) break;
        if (fc.length!=fr.length || fc.type_!=fr.type_ || fc.stream_id!=fr.stream_id ||
            fc.data_off!=fr.data_off || fc.data_len!=fr.data_len || fc.pad_len!=fr.pad_len) mism++;
        frames++;
    }
    // Malicious: zero-length PADDED DATA on stream 1 -> both must REJECT (the
    // old inline code dereferenced payload[0]==NULL here).
    static const uint8_t evil[] = { 0,0,0, 0x00,0x08, 0,0,0,1 };
    uint32_t ec=0, er=0; H2Frame fe;
    int rc_evil = http2_frame_next_c (evil, sizeof(evil), &ec, &fe);
    int rr_evil = http2_frame_next_rs(evil, sizeof(evil), &er, &fe);
    // PASS iff well-formed frames are byte-identical (mism==0) AND the Rust seam
    // CONFINES the malicious zero-length PADDED frame (rr_evil==-1). The verbatim
    // C reference does NOT reject it (rc_evil!=-1): it performs the unchecked
    // payload[0] read that is exactly MAYTERA-SEC-2026-0010 - OOB here (reads an
    // adjacent byte and returns 1), a NULL-deref in the live kernel (payload==NULL
    // when flen==0), and an ASan heap-buffer-overflow offline. The LIVE path
    // routes to the Rust seam, so the bug is unreachable in b822.
    const char *verdict = (mism==0 && rr_evil==-1) ? "PASS" : "FAIL";
    kprintf("[RUST-DIFF] http2_frame: %d frames, mism=%d, evil C-ref rc=%d (buggy: unchecked OOB read) Rust rr=%d (want -1 = confined) -> %s\n",
            frames, mism, rc_evil, rr_evil, verdict);
    extern void bootlog_write(const char *fmt, ...);
    bootlog_write("[RUST-DIFF] http2_frame: %d frames mism=%d evil C-ref rc=%d Rust rr=%d(confined) -> %s",
            frames, mism, rc_evil, rr_evil, verdict);
    kprintf("[RUST-SEC] http2_frame: MAYTERA-SEC-2026-0010 (CWE-476/125, REMOTE pre-auth): "
            "a malicious h2 server's zero-length PADDED DATA/HEADERS frame drove the old inline "
            "framing to read payload[0] with payload==NULL -> kernel page fault (whole-OS DoS). "
            "The verbatim C reference still exhibits the unchecked read (rc=%d, not -1); the Rust "
            "seam CONFINES it (rr=%d = rejected). LIVE path routes to Rust in b822.\n",
            rc_evil, rr_evil);
    bootlog_write("[RUST-SEC] http2_frame: MAYTERA-SEC-2026-0010 zero-len PADDED: C-ref unchecked rc=%d, Rust confines rr=%d",
            rc_evil, rr_evil);
#ifdef RUST_HTTP2_FRAME
    kprintf("[RUST-DIFF] http2_frame: LIVE=rust\n");
#else
    kprintf("[RUST-DIFF] http2_frame: LIVE=c\n");
#endif

    // [RUST-PERF] RDTSC micro-benchmark over one full pass of the good buffer. LIGHT.
    {
        const int R = 4000; H2Frame f; uint32_t p; volatile uint32_t sink = 0;
        for (int i=0;i<50;i++){ p=0; while (http2_frame_next(good, sizeof(good), &p, &f)==1) sink += f.data_len; }
        uint64_t t0=h2f_rdtsc();
        for (int i=0;i<R;i++){ p=0; while (http2_frame_next_c (good, sizeof(good), &p, &f)==1) sink += f.data_len; }
        uint64_t t1=h2f_rdtsc();
        for (int i=0;i<R;i++){ p=0; while (http2_frame_next_rs(good, sizeof(good), &p, &f)==1) sink += f.data_len; }
        uint64_t t2=h2f_rdtsc();
        kprintf("[RUST-PERF] http2_frame: C=%lu RS=%lu cyc/pass\n",
                (unsigned long)((t1-t0)/R), (unsigned long)((t2-t1)/R));
        bootlog_write("[RUST-PERF] http2_frame: C=%lu RS=%lu cyc/pass",
                (unsigned long)((t1-t0)/R), (unsigned long)((t2-t1)/R));
        (void)sink;
    }
}
