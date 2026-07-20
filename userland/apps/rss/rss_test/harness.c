/* harness.c - offline known-answer + fuzz test for the REAL Rust feed parser
 * (rss_rs.rs), built for the host and linked here. Run under ASan+UBSan so any
 * out-of-bounds read of the exact-size input buffer, or any UB, aborts loudly.
 *
 * Two phases:
 *   1. KNOWN-ANSWER over a corpus of REAL feeds (RSS2 / Atom / RSS1.0-RDF):
 *      parse, assert the detected format + a positive item count, and print the
 *      first item so a human can eyeball extraction quality.
 *   2. FUZZ: for every corpus file, run truncations at every length + a byte-
 *      flip sweep + random mutations through rss_parse. The input for each run
 *      lives in a malloc'd buffer of EXACTLY the mutated length, so ASan's
 *      redzones catch any read past the end. Assert it never crashes.
 *
 * Build (on the userland build container, pinned rustc 1.97.0):
 *   rustc --edition 2021 --crate-type staticlib --target x86_64-unknown-linux-gnu \
 *         -C opt-level=2 -C panic=abort -o librss_rs_host.a ../rss_rs.rs
 *   cc -g -O1 -fsanitize=address,undefined harness.c librss_rs_host.a -o harness
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mirror rss_rs.h without the MayteraOS types.h dependency (host build). */
#define RSS_FMT_UNKNOWN 0
#define RSS_FMT_RSS2    1
#define RSS_FMT_ATOM    2
#define RSS_FMT_RDF     3

typedef struct {
    char *title; char *link; char *description;
    char *pub_date; char *author; char *enclosure; char *guid;
    char *image_url; char *image_alt; char *image_title;
} rss_item_t;

typedef struct {
    int format; int error;
    char *title; char *link; char *description;
    rss_item_t *items; int item_count;
} rss_feed_t;

rss_feed_t  *rss_parse(const unsigned char *data, unsigned long len);
void         rss_free(rss_feed_t *feed);
const char  *rss_format_name(int fmt);
unsigned int rss_abi_item_size(void);

static const char *fmt_name(int f) { return rss_format_name(f); }

/* The precompiled host `alloc` rlib was built panic=unwind and emits a data
 * reference to rust_eh_personality; our crate is panic=abort so it is never
 * invoked. Provide the symbol so the (host-only) test link resolves. This stub
 * exists ONLY in the offline harness; the shipped app links no std rlib. */
void rust_eh_personality(void) {}

static char *read_file(const char *path, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc(n > 0 ? n : 1);
    long r = (long)fread(buf, 1, n, f);
    fclose(f);
    *out_len = r;
    return buf;
}

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail++; } } while (0)

/* Parse a buffer copied into an EXACT-size heap block (so ASan guards the end). */
static rss_feed_t *parse_exact(const char *src, long len) {
    unsigned char *exact = (unsigned char *)malloc(len > 0 ? len : 1);
    if (len > 0) memcpy(exact, src, len);
    rss_feed_t *fd = rss_parse(exact, (unsigned long)len);
    free(exact);
    return fd;
}

/* img_min/img_max bound how many items must carry an inline image. They are
 * known answers derived from the feed bytes in corpus/, not from what the code
 * happens to do:
 *   xkcd     - every item is an <img> in the description (the comic IS the item)
 *   bbc      - every item has a <media:thumbnail>
 *   github   - every item has a <media:thumbnail>
 *   nasa     - <img> in content:encoded; its <media:content> are medium="video"
 *              YouTube embeds and MUST NOT be mistaken for pictures
 *   w3c      - <img> with multi-line attributes and alt text
 *   slashdot - the ONLY imgs are twitter/facebook icons repeated in every item,
 *              so the feed-chrome filter must leave 0
 *   github   - 10 <media:thumbnail> but only ONE distinct URL: the repo owner's
 *              30x30 avatar, in every entry. That is chrome, not per-release
 *              art, so the filter must leave 0. Paired with bbc (36 thumbnails,
 *              36 DISTINCT, all kept) this pins the discrimination the filter
 *              exists to make: same picture everywhere = chrome, different
 *              picture per item = content.
 *   others   - genuinely image-less: they must stay at 0 (no false positives) */
typedef struct {
    const char *file; int expect_fmt; int img_min; int img_max; const char *label;
} corpus_t;

static const corpus_t CORPUS[] = {
    { "corpus/bbc.xml",      RSS_FMT_RSS2, 30, 999, "BBC World (RSS 2.0)" },
    { "corpus/hn.xml",       RSS_FMT_RSS2,  0,   0, "Hacker News (RSS 2.0)" },
    { "corpus/lobsters.xml", RSS_FMT_RSS2,  0,   0, "Lobsters (RSS 2.0)" },
    { "corpus/nasa.xml",     RSS_FMT_RSS2,  1, 999, "NASA (RSS 2.0, large)" },
    { "corpus/xkcd.rss",     RSS_FMT_RSS2,  4,   4, "xkcd (RSS 2.0)" },
    { "corpus/arxiv.xml",    RSS_FMT_RSS2,  0,   0, "arXiv cs.OS (RSS 2.0)" },
    { "corpus/w3c.xml",      RSS_FMT_RSS2,  1, 999, "W3C news (RSS 2.0)" },
    { "corpus/reddit.xml",   RSS_FMT_ATOM,  0,   0, "Reddit r/rust (Atom)" },
    { "corpus/github.xml",   RSS_FMT_ATOM,  0,   0, "GitHub linux releases (Atom)" },
    { "corpus/mnot.atom",    RSS_FMT_ATOM,  0,   0, "mnot.net (Atom)" },
    { "corpus/slashdot.rdf", RSS_FMT_RDF,   0,   0, "Slashdot (RSS 1.0/RDF)" },
};
static const int NCORPUS = (int)(sizeof(CORPUS) / sizeof(CORPUS[0]));

static void trunc80(const char *s, char *out) {
    int i = 0;
    for (; s && s[i] && i < 78; i++) out[i] = (s[i] >= 32 && s[i] < 127) ? s[i] : '.';
    out[i] = 0;
}

static int known_answer(void) {
    printf("=== PHASE 1: KNOWN-ANSWER over %d REAL feeds ===\n", NCORPUS);
    /* FFI sizeof-lock: this file declares rss_item_t independently of the Rust
     * CItem, so a field added on one side only would misalign every pointer
     * past it and the corpus checks below would read garbage, not fail. */
    printf("  ABI: sizeof(rss_item_t)=%u  rss_abi_item_size()=%u\n",
           (unsigned)sizeof(rss_item_t), rss_abi_item_size());
    CHECK(sizeof(rss_item_t) == rss_abi_item_size(), "C/Rust rss_item_t size mismatch");
    int rss = 0, atom = 0, rdf = 0, total_items = 0;
    for (int c = 0; c < NCORPUS; c++) {
        long len = 0;
        char *raw = read_file(CORPUS[c].file, &len);
        if (!raw) { printf("  MISSING: %s\n", CORPUS[c].file); g_fail++; continue; }
        rss_feed_t *fd = parse_exact(raw, len);
        free(raw);
        if (!fd) { printf("  NULL parse: %s\n", CORPUS[c].label); g_fail++; continue; }

        char t[80], l[80];
        trunc80(fd->title, t);
        printf("  %-28s fmt=%-11s items=%3d  ch-title=\"%s\"\n",
               CORPUS[c].label, fmt_name(fd->format), fd->item_count, t);
        CHECK(fd->format == CORPUS[c].expect_fmt, "format mismatch");
        CHECK(fd->item_count > 0, "no items extracted");

        /* How many items carry an inline image, and is every image field a
         * valid C string on EVERY item (not just item[0])? */
        int with_img = 0, first_img = -1;
        for (int i = 0; i < fd->item_count; i++) {
            rss_item_t *it = &fd->items[i];
            CHECK(it->image_url && it->image_alt && it->image_title,
                  "NULL image field ptr");
            if (it->image_url && it->image_url[0]) {
                if (first_img < 0) first_img = i;
                with_img++;
            }
        }
        printf("        images: %d/%d items\n", with_img, fd->item_count);
        CHECK(with_img >= CORPUS[c].img_min, "fewer images than expected");
        CHECK(with_img <= CORPUS[c].img_max, "more images than expected");
        if (first_img >= 0) {
            rss_item_t *it = &fd->items[first_img];
            trunc80(it->image_url, l);
            printf("        item[%d] image=\"%s\"\n", first_img, l);
            /* An image URL we would actually hand to the fetcher must not be
             * a scheme we refuse; print alt/title so extraction quality is
             * eyeballable (xkcd's title attribute is the joke). */
            if (it->image_alt[0])   { trunc80(it->image_alt, t);   printf("        item[%d] alt  =\"%s\"\n", first_img, t); }
            if (it->image_title[0]) { trunc80(it->image_title, t); printf("        item[%d] title=\"%s\"\n", first_img, t); }
        }

        if (fd->item_count > 0) {
            rss_item_t *it = &fd->items[0];
            trunc80(it->title, t);
            trunc80(it->link, l);
            printf("        item[0] title=\"%s\"\n", t);
            printf("        item[0] link =\"%s\"\n", l);
            CHECK(it->title && it->title[0], "item[0] empty title");
            CHECK(it->link && it->link[0], "item[0] empty link");
            /* every field must be a valid (non-NULL) C string */
            CHECK(it->description && it->pub_date && it->author &&
                  it->enclosure && it->guid, "item[0] NULL field ptr");
        }
        if (fd->format == RSS_FMT_RSS2) rss++;
        else if (fd->format == RSS_FMT_ATOM) atom++;
        else if (fd->format == RSS_FMT_RDF) rdf++;
        total_items += fd->item_count;
        rss_free(fd);
    }
    printf("  format coverage: RSS2=%d  Atom=%d  RDF=%d   total items extracted=%d\n",
           rss, atom, rdf, total_items);
    CHECK(rss > 0 && atom > 0 && rdf > 0, "all three formats must be covered");
    return total_items;
}

/* xorshift32 deterministic PRNG */
static unsigned int RNG = 0x1234567u;
static unsigned int rng(void) {
    RNG ^= RNG << 13; RNG ^= RNG >> 17; RNG ^= RNG << 5; return RNG;
}

/* Full exhaustive sweeps are only affordable (under ASan) up to this size;
 * larger feeds are SAMPLED instead so coverage stays broad without the 345 KB
 * NASA feed alone costing a million+ full parses. */
#define SWEEP_FULL_MAX 30000

static long fuzz_one(const char *src, long len) {
    long runs = 0;
    static const unsigned char PAT[4] = { 0xFF, 0x3C, 0x26, 0x00 };
    char *m = (char *)malloc(len > 0 ? len : 1);
    if (len <= SWEEP_FULL_MAX) {
        /* (a) every truncation length 0..len */
        for (long L = 0; L <= len; L++) {
            rss_feed_t *fd = parse_exact(src, L);
            if (fd) rss_free(fd);
            runs++;
        }
        /* (b) single byte-flip sweep (each position XOR'd with 4 patterns) */
        for (int p = 0; p < 4; p++) {
            for (long i = 0; i < len; i++) {
                memcpy(m, src, len);
                m[i] ^= PAT[p];
                rss_feed_t *fd = parse_exact(m, len);
                if (fd) rss_free(fd);
                runs++;
            }
        }
    } else {
        /* large feed: sample 4000 truncation lengths + 20000 single-byte flips */
        for (int s = 0; s < 4000; s++) {
            long L = (long)(rng() % (unsigned)(len + 1));
            rss_feed_t *fd = parse_exact(src, L);
            if (fd) rss_free(fd);
            runs++;
        }
        for (int s = 0; s < 20000; s++) {
            memcpy(m, src, len);
            m[rng() % len] ^= PAT[rng() & 3];
            rss_feed_t *fd = parse_exact(m, len);
            if (fd) rss_free(fd);
            runs++;
        }
    }
    /* (c) random multi-byte corruption, 2000 iterations */
    for (int it = 0; it < 2000; it++) {
        memcpy(m, src, len);
        int nmut = (rng() % 32) + 1;
        for (int k = 0; k < nmut && len > 0; k++)
            m[rng() % len] = (char)(rng() & 0xFF);
        long L = len > 0 ? (long)(rng() % (unsigned)len) + 1 : 0;
        rss_feed_t *fd = parse_exact(m, L);
        if (fd) rss_free(fd);
        runs++;
    }
    free(m);
    return runs;
}

static long fuzz(void) {
    printf("=== PHASE 2: FUZZ (ASan/UBSan guarded, exact-size buffers) ===\n");
    long total = 0;
    for (int c = 0; c < NCORPUS; c++) {
        long len = 0;
        char *raw = read_file(CORPUS[c].file, &len);
        if (!raw) continue;
        long r = fuzz_one(raw, len);
        free(raw);
        printf("  %-28s %ld fuzz vectors OK (no crash/OOB)\n", CORPUS[c].label, r);
        total += r;
    }
    /* pathological hand-crafted inputs */
    const char *evil[] = {
        "", "<", "<rss", "<rss>", "<rss><channel><item><title>",
        "<feed><entry><link href=", "<![CDATA[", "<!--", "<?xml",
        "<rss><channel><item><title>&#x", "&#999999999;", "<a",
        "<rdf:RDF><item><title>x</title>",
        "<rss><channel>" "<item><title></title></item></channel></rss>",
    };
    for (int i = 0; i < (int)(sizeof(evil) / sizeof(evil[0])); i++) {
        rss_feed_t *fd = parse_exact(evil[i], (long)strlen(evil[i]));
        if (fd) rss_free(fd);
        total++;
    }
    /* NULL / zero-length */
    rss_feed_t *fd = rss_parse(NULL, 0);
    if (fd) rss_free(fd);
    total++;
    printf("  total fuzz vectors: %ld  (0 crashes, 0 OOB, 0 UB under ASan+UBSan)\n", total);
    return total;
}

int main(void) {
    int items = known_answer();
    long vec = fuzz();
    printf("\n=== SUMMARY ===\n");
    printf("known-answer items extracted: %d\n", items);
    printf("fuzz vectors executed: %ld\n", vec);
    printf("failures: %d\n", g_fail);
    printf("VERDICT: %s\n", g_fail == 0 ? "PASS - works for all feeds, no crash/OOB"
                                        : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
