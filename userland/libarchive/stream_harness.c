// stream_harness.c - hosted tests for the #613 streaming tar.gz extractor.
// Build:  gcc -DARC_HOST -O2 -o /tmp/arcstream arc.c stream_harness.c
// Run:    /tmp/arcstream [path-to-a-real.mpkg]
//
// Covers the two things the buffered extractor never had to care about:
//   1. correctness - the streamed member set/content matches the buffered
//      arc_targz_extract() output byte for byte;
//   2. hardening - because members now go STRAIGHT to a destination path, a
//      crafted archive must be REFUSED, not extracted (absolute paths, ".."
//      traversal, symlink/hardlink/device/fifo member types, over-long names,
//      and a truncated member whose declared size never arrives).
#include "arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
                              else { printf("ok  : %s\n", msg); } } while (0)

// ---- a pull reader over a memory buffer ---------------------------------
typedef struct { const uint8_t *p; size_t n, off; size_t slice; } memrd;
static int memrd_fn(void *ctx, uint8_t *buf, size_t cap) {
    memrd *m = (memrd *)ctx;
    size_t want = m->n - m->off;
    if (want == 0) return 0;
    if (want > cap) want = cap;
    if (m->slice && want > m->slice) want = m->slice;   // exercise short reads
    memcpy(buf, m->p + m->off, want);
    m->off += want;
    return (int)want;
}

// ---- a sink that records what it was told --------------------------------
#define MAXM 64
typedef struct {
    char   name[MAXM][ARC_MEMBER_NAME_MAX];
    size_t size[MAXM];
    uint8_t *data[MAXM];
    size_t   fill[MAXM];
    int    n;
    int    open;          // index of the member currently being written
    int    aborted;
    size_t peak_chunk;
} rec_sink;

static int rs_member(void *ctx, const arc_member *m) {
    rec_sink *r = (rec_sink *)ctx;
    if (m->is_dir) return 1;
    if (r->n >= MAXM) return -1;
    strcpy(r->name[r->n], m->name);
    r->size[r->n] = (size_t)m->size;
    r->data[r->n] = (uint8_t *)malloc(m->size ? (size_t)m->size : 1);
    r->fill[r->n] = 0;
    r->open = r->n;
    r->n++;
    return 0;
}
static int rs_data(void *ctx, const uint8_t *d, size_t len) {
    rec_sink *r = (rec_sink *)ctx;
    int i = r->open;
    if (len > r->peak_chunk) r->peak_chunk = len;
    if (r->fill[i] + len > r->size[i]) return -1;
    memcpy(r->data[i] + r->fill[i], d, len);
    r->fill[i] += len;
    return 0;
}
static int rs_end(void *ctx, const arc_member *m) {
    rec_sink *r = (rec_sink *)ctx;
    return (r->fill[r->open] == (size_t)m->size) ? 0 : -1;
}
static void rs_abort(void *ctx) { ((rec_sink *)ctx)->aborted = 1; }
static const arc_sink REC_SINK = { rs_member, rs_data, rs_end, rs_abort };

// ---- hand-built tar blocks (so we can craft hostile ones) ----------------
static void put_oct(char *f, int width, unsigned long v) {
    for (int i = width - 2; i >= 0; i--) { f[i] = (char)('0' + (v & 7)); v >>= 3; }
    f[width - 1] = ' ';
}
static void tar_hdr(uint8_t *h, const char *name, size_t size, char type) {
    memset(h, 0, 512);
    strncpy((char *)h, name, 99);
    put_oct((char *)h + 100, 8, 0644);
    put_oct((char *)h + 108, 8, 0);
    put_oct((char *)h + 116, 8, 0);
    put_oct((char *)h + 124, 12, (unsigned long)size);
    put_oct((char *)h + 136, 12, 0);
    memset(h + 148, ' ', 8);          // checksum field reads as spaces while summing
    h[156] = (uint8_t)type;
    memcpy(h + 257, "ustar  ", 8);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += h[i];
    // ustar convention: 6 octal digits, NUL, space
    for (int i = 5; i >= 0; i--) { h[148 + i] = (uint8_t)('0' + (sum & 7)); sum >>= 3; }
    h[154] = 0;
    h[155] = ' ';
}

// Build a tar.gz whose single member has the given name/type/payload.
static uint8_t *make_gz(const char *name, char type, const char *payload,
                        size_t plen, size_t *out_len, int truncate_payload) {
    size_t blocks = 1 + (plen + 511) / 512 + 2;
    size_t tlen = blocks * 512;
    uint8_t *tar = (uint8_t *)calloc(tlen, 1);
    tar_hdr(tar, name, plen, type);
    if (payload && plen) memcpy(tar + 512, payload, truncate_payload ? plen / 2 : plen);
    if (truncate_payload) tlen = 512 + 512;   // header + one short data block, no end
    return arc_gzip_compress(tar, tlen, "t.tar", out_len);
}

// counting-only sink used for the timed real-package run
struct cnt { uint64_t total; int members; };
static int cnt_member(void *ctx, const arc_member *m) { (void)m; ((struct cnt *)ctx)->members++; return 0; }
static int cnt_data(void *ctx, const uint8_t *d, size_t l) { (void)d; ((struct cnt *)ctx)->total += l; return 0; }
static int cnt_end(void *ctx, const arc_member *m) { (void)ctx; (void)m; return 0; }
static const arc_sink CNT_SINK = { cnt_member, cnt_data, cnt_end, 0 };

static int stream_rc(const uint8_t *gz, size_t n, rec_sink *r, size_t slice) {
    memrd m = { gz, n, 0, slice };
    memset(r, 0, sizeof(*r));
    return arc_targz_extract_stream(memrd_fn, &m, &REC_SINK, r, 1);
}

int main(int argc, char **argv) {
    printf("arc streaming working set: %zu bytes\n", arc_stream_workmem());

    // ---------------------------------------------------------------- 1
    printf("\n==== correctness: streamed == buffered ====\n");
    {
        int NE = 5;
        arc_entry *e = (arc_entry *)calloc(NE, sizeof(arc_entry));
        size_t sizes[5] = { 0, 1, 511, 512, 200000 };
        const char *names[5] = { "pkg/EMPTY", "pkg/ONE", "pkg/A511", "pkg/A512", "pkg/files/big.bin" };
        for (int i = 0; i < NE; i++) {
            strcpy(e[i].name, names[i]);
            e[i].size = sizes[i];
            e[i].data = (uint8_t *)malloc(sizes[i] ? sizes[i] : 1);
            for (size_t j = 0; j < sizes[i]; j++)
                e[i].data[j] = (uint8_t)((j % 7 == 0) ? (j * 2654435761u >> 13) : ('A' + (j % 26)));
            e[i].mode = 0644;
        }
        size_t gzn = 0;
        uint8_t *gz = arc_targz_create(e, NE, &gzn);
        CHECK(gz != NULL, "arc_targz_create built a test archive");

        rec_sink r;
        int rc = stream_rc(gz, gzn, &r, 0);
        CHECK(rc == ARC_OK, "stream extract returns ARC_OK");
        CHECK(r.n == NE, "stream saw every regular member");
        int same = 1;
        for (int i = 0; i < r.n && i < NE; i++) {
            if (strcmp(r.name[i], names[i]) != 0) same = 0;
            if (r.fill[i] != sizes[i]) same = 0;
            else if (sizes[i] && memcmp(r.data[i], e[i].data, sizes[i]) != 0) same = 0;
        }
        CHECK(same, "streamed member names + bytes match the source exactly");
        CHECK(r.peak_chunk <= 32768, "no data chunk exceeded the 32KB window");

        // and with a deliberately tiny input slice (1 byte at a time)
        rc = stream_rc(gz, gzn, &r, 1);
        CHECK(rc == ARC_OK && r.n == NE, "same result when input arrives 1 byte at a time");
        free(gz);
    }

    // ---------------------------------------------------------------- 2
    printf("\n==== hardening: hostile archives must be REFUSED ====\n");
    {
        struct { const char *name; char type; const char *what; int expect; } cases[] = {
            { "../evil",                    '0', "parent-traversal name '../evil'",        ARC_E_UNSAFE },
            { "pkg/../../etc/passwd",       '0', "embedded '..' traversal",                ARC_E_UNSAFE },
            { "/etc/passwd",                '0', "absolute path '/etc/passwd'",            ARC_E_UNSAFE },
            { "pkg/link",                   '2', "symlink member type",                    ARC_E_UNSAFE },
            { "pkg/hard",                   '1', "hardlink member type",                   ARC_E_UNSAFE },
            { "pkg/dev",                    '3', "character-device member type",           ARC_E_UNSAFE },
            { "pkg/blk",                    '4', "block-device member type",               ARC_E_UNSAFE },
            { "pkg/fifo",                   '6', "fifo member type",                       ARC_E_UNSAFE },
            { "pkg/ok",                     '0', "(control) a plain safe member",          ARC_OK       },
        };
        const char *pl = "payload-bytes";
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            size_t gzn = 0;
            uint8_t *gz = make_gz(cases[i].name, cases[i].type, pl, strlen(pl), &gzn, 0);
            rec_sink r;
            int rc = stream_rc(gz, gzn, &r, 0);
            char msg[160];
            snprintf(msg, sizeof(msg), "%s -> %s (rc=%d)", cases[i].what,
                     cases[i].expect == ARC_OK ? "extracted" : "REFUSED", rc);
            CHECK(rc == cases[i].expect, msg);
            if (cases[i].expect != ARC_OK)
                CHECK(r.n == 0 || r.aborted, "  ...and nothing was left written");
            free(gz);
        }
        // over-long member name, delivered the only way a tar CAN carry one:
        // a GNU 'L' long-name record ahead of the real header.
        {
            char big[400];
            memset(big, 'a', sizeof(big) - 1); big[sizeof(big) - 1] = 0;
            size_t nlen = strlen(big) + 1;
            size_t tlen = 512 + ((nlen + 511) / 512) * 512 + 512 + 512 + 1024;
            uint8_t *tar = (uint8_t *)calloc(tlen, 1);
            tar_hdr(tar, "././@LongLink", nlen, 'L');
            memcpy(tar + 512, big, nlen);
            size_t off = 512 + ((nlen + 511) / 512) * 512;
            tar_hdr(tar + off, "shortname", strlen(pl), '0');
            memcpy(tar + off + 512, pl, strlen(pl));
            size_t gzn = 0;
            uint8_t *gz = arc_gzip_compress(tar, tlen, "t.tar", &gzn);
            rec_sink r; int rc = stream_rc(gz, gzn, &r, 0);
            CHECK(rc == ARC_E_UNSAFE, "over-long GNU long-name member refused");
            free(gz); free(tar);
        }
        // a GNU long-name record carrying a TRAVERSAL name must also be caught
        {
            const char *evil = "../../../APPS/COMPOSIT";
            size_t nlen = strlen(evil) + 1;
            size_t tlen = 512 + 512 + 512 + 512 + 1024;
            uint8_t *tar = (uint8_t *)calloc(tlen, 1);
            tar_hdr(tar, "././@LongLink", nlen, 'L');
            memcpy(tar + 512, evil, nlen);
            tar_hdr(tar + 1024, "harmless", strlen(pl), '0');
            memcpy(tar + 1536, pl, strlen(pl));
            size_t gzn = 0;
            uint8_t *gz = arc_gzip_compress(tar, tlen, "t.tar", &gzn);
            rec_sink r; int rc = stream_rc(gz, gzn, &r, 0);
            CHECK(rc == ARC_E_UNSAFE, "traversal hidden in a GNU long-name record refused");
            free(gz); free(tar);
        }
        // ustar prefix field must not be able to smuggle an absolute path
        {
            size_t tlen = 512 + 512 + 1024;
            uint8_t *tar = (uint8_t *)calloc(tlen, 1);
            tar_hdr(tar, "file", strlen(pl), '0');
            memcpy(tar + 345, "..", 3);            // prefix = ".."
            unsigned sum = 0;
            memset(tar + 148, ' ', 8);
            for (int i = 0; i < 512; i++) sum += tar[i];
            for (int i = 5; i >= 0; i--) { tar[148 + i] = (uint8_t)('0' + (sum & 7)); sum >>= 3; }
            tar[154] = 0; tar[155] = ' ';
            memcpy(tar + 512, pl, strlen(pl));
            size_t gzn = 0;
            uint8_t *gz = arc_gzip_compress(tar, tlen, "t.tar", &gzn);
            rec_sink r; int rc = stream_rc(gz, gzn, &r, 0);
            CHECK(rc == ARC_E_UNSAFE, "'..' smuggled in the ustar prefix field refused");
            free(gz); free(tar);
        }
        // truncated member: the declared size never arrives
        {
            size_t gzn = 0;
            char payload[1200];
            memset(payload, 'z', sizeof(payload));
            uint8_t *gz = make_gz("pkg/short", '0', payload, sizeof(payload), &gzn, 1);
            rec_sink r; int rc = stream_rc(gz, gzn, &r, 0);
            CHECK(rc != ARC_OK, "truncated member (declared size never arrives) refused");
            CHECK(r.aborted == 1, "  ...and the sink was told to drop its partial output");
            free(gz);
        }
        // flipped byte in the compressed body -> CRC/format failure
        {
            int NE = 1;
            arc_entry e; memset(&e, 0, sizeof(e));
            strcpy(e.name, "pkg/x"); e.size = 40000;
            e.data = (uint8_t *)malloc(e.size);
            for (size_t j = 0; j < e.size; j++) e.data[j] = (uint8_t)(j * 31);
            size_t gzn = 0; uint8_t *gz = arc_targz_create(&e, NE, &gzn);
            gz[gzn / 2] ^= 0x40;
            rec_sink r; int rc = stream_rc(gz, gzn, &r, 0);
            CHECK(rc != ARC_OK, "corrupted compressed body refused (crc/format)");
            free(gz);
        }
    }

    // ---------------------------------------------------------------- 3
    if (argc > 1) {
        printf("\n==== real package: %s ====\n", argv[1]);
        FILE *f = fopen(argv[1], "rb");
        if (!f) { printf("cannot open\n"); return fails ? 1 : 0; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = (uint8_t *)malloc(n);
        if (fread(b, 1, n, f) != (size_t)n) { printf("short read\n"); return 1; }
        fclose(f);
        printf("package %ld bytes\n", n);

        struct timespec t0, t1;
        // streaming pass, counting bytes only (no member buffering at all)
        struct cnt c = { 0, 0 };
        memrd m = { b, (size_t)n, 0, 0 };
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int rc = arc_targz_extract_stream(memrd_fn, &m, &CNT_SINK, &c, 1);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("STREAM  rc=%d members=%d uncompressed=%llu  %.2f s  (%.1f MB/s out)\n",
               rc, c.members, (unsigned long long)c.total, ds, c.total / 1048576.0 / ds);

        clock_gettime(CLOCK_MONOTONIC, &t0);
        int bc = 0;
        arc_entry *ents = arc_targz_extract(b, (size_t)n, &bc);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double db = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        uint64_t btot = 0;
        for (int i = 0; i < bc; i++) btot += ents[i].size;
        printf("BUFFER  count=%d uncompressed=%llu  %.2f s\n",
               bc, (unsigned long long)btot, db);
        CHECK(rc == ARC_OK, "real package streams cleanly");
        CHECK(c.total == btot, "streamed byte total equals the buffered extractor's");
    }

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
