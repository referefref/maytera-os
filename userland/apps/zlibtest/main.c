// zlibtest - proof that the mports-built real zlib (userland/ports/zlib) works
// on a running MayteraOS, not just that it compiled.
//
// WHY THIS APP EXISTS. "A library that compiles but was never linked and run is
// not evidence." This is the evidence. It links the static libz.a that
// userland/ports/mports.sh produced from the sha256-pinned upstream tarball,
// and it runs six checks on a booted machine.
//
// THREE OF THE CHECKS ARE AGAINST EXTERNAL GROUND TRUTH, not against
// ourselves, because a codec that is merely self-consistent is exactly the trap
// the two hand-written zlib_shim.c files fell into (see blame.md: "a shim that
// consumes all input every call can defeat a caller's own refill gate", and the
// 16x-ratio buffer heuristic that produced a permanent zero-progress busy loop
// presenting as a blank white window):
//
//   * crc32("123456789")   == 0xCBF43926, the CRC-32 catalogue check value.
//   * adler32("123456789") == 0x091E01DE, the Adler-32 check value.
//   * a byte-for-byte zlib stream produced by CPython's zlib on the build host
//     is decompressed here, and the result's CRC is compared against the CRC
//     the host computed. If our inflate diverged from RFC1950/1951 in any way,
//     this fails. Round-tripping our own compress() through our own uncompress()
//     could not catch that.
//
// Check 5 is the shim-bug regression: a streaming deflate/inflate through a
// deliberately TINY 64-byte output window, so the caller must loop. Both shims
// mishandled precisely this, one of them shipping a hang.
//
// OUTPUT DISCIPLINE. Launched via /CONFIG/AUTORUN.CFG containing "/APPS/ZLIBTEST".
// An autorun-launched process emits ONE SERIAL RECORD PER write(), each wrapped
// in the kernel syslog prefix, so a line assembled from several write() calls
// arrives as several interleaved fragments (blame.md, #700). Every line here is
// formatted into a buffer and issued as exactly one write(2, ...).

#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "zlib.h"

static int g_pass = 0, g_fail = 0;

static void line(const char *s) {
    write(2, s, strlen(s));
}

static void ck(const char *what, int ok) {
    char b[256];
    if (ok) g_pass++; else g_fail++;
    snprintf(b, sizeof(b), "[ZLIBTEST] %s %s\n", ok ? "PASS" : "FAIL", what);
    line(b);
}

static void note(const char *fmt_what, unsigned long a, unsigned long b) {
    char buf[256];
    snprintf(buf, sizeof(buf), "[ZLIBTEST]      %s got=0x%lx want=0x%lx\n", fmt_what, a, b);
    line(buf);
}

// 380 bytes: the string below repeated four times.
static const char *UNIT =
    "MayteraOS mports zlib interop vector: the quick brown fox jumps over the lazy dog. 0123456789. ";

// A REAL RFC1950 zlib stream of those 380 bytes, produced by CPython's zlib
// (level 6) on the the build container build container. Not produced by this program, and
// not produced by our libz: that is the entire point of check 3.
static const unsigned char HOST_STREAM[] = {
    120,156,243,77,172,44,73,45,74,244,15,86,200,45,200,47,42,41,86,168,
    202,201,76,82,200,204,3,10,230,23,40,148,165,38,151,228,23,89,41,148,
    100,164,42,20,150,102,38,103,43,36,21,229,151,231,41,164,229,87,40,100,
    149,230,22,20,43,228,151,165,22,129,165,115,18,171,42,21,82,242,211,245,
    20,12,12,141,140,77,76,205,204,45,44,245,20,124,71,141,199,109,60,0,
    145,83,131,109
};
#define HOST_STREAM_LEN (sizeof(HOST_STREAM))
#define HOST_PLAIN_LEN  380u
#define HOST_PLAIN_CRC  0x7a245aa6UL

int main(void) {
    char b[256];
    static unsigned char plain[HOST_PLAIN_LEN + 1];
    static unsigned char comp[4096];
    static unsigned char back[8192];
    unsigned i;

    line("[ZLIBTEST] ==== mports zlib proof, start ====\n");
    snprintf(b, sizeof(b), "[ZLIBTEST] zlibVersion()=%s ZLIB_VERSION=%s\n",
             zlibVersion(), ZLIB_VERSION);
    line(b);
    ck("zlibVersion() reports 1.3.1", strcmp(zlibVersion(), "1.3.1") == 0);

    // --- 1/2: checksums against the published check values -------------------
    {
        const Bytef *v = (const Bytef *)"123456789";
        uLong c = crc32(0L, Z_NULL, 0);
        uLong a = adler32(0L, Z_NULL, 0);
        c = crc32(c, v, 9);
        a = adler32(a, v, 9);
        ck("crc32(\"123456789\") == 0xCBF43926 (catalogue check value)", c == 0xCBF43926UL);
        if (c != 0xCBF43926UL) note("crc32", (unsigned long)c, 0xCBF43926UL);
        ck("adler32(\"123456789\") == 0x091E01DE (published check value)", a == 0x091E01DEUL);
        if (a != 0x091E01DEUL) note("adler32", (unsigned long)a, 0x091E01DEUL);
    }

    // --- build the 380-byte plaintext ---------------------------------------
    {
        unsigned n = (unsigned)strlen(UNIT);
        unsigned o = 0;
        for (i = 0; i < 4; i++) { memcpy(plain + o, UNIT, n); o += n; }
        plain[o] = 0;
        ck("test vector is 380 bytes", o == HOST_PLAIN_LEN);
        if (o != HOST_PLAIN_LEN) note("plainlen", o, HOST_PLAIN_LEN);
    }

    // --- 3: decompress a stream a FOREIGN zlib produced ----------------------
    {
        uLongf outlen = sizeof(back);
        int rc = uncompress(back, &outlen, HOST_STREAM, (uLong)HOST_STREAM_LEN);
        uLong c = crc32(crc32(0L, Z_NULL, 0), back, (uInt)outlen);
        snprintf(b, sizeof(b), "[ZLIBTEST]      foreign stream: %u bytes in, rc=%d, %lu bytes out\n",
                 (unsigned)HOST_STREAM_LEN, rc, (unsigned long)outlen);
        line(b);
        ck("uncompress() of a CPython-produced zlib stream returns Z_OK", rc == Z_OK);
        ck("its length matches what the host compressed", outlen == HOST_PLAIN_LEN);
        ck("its CRC-32 matches the host's CRC-32 of the original", c == HOST_PLAIN_CRC);
        if (c != HOST_PLAIN_CRC) note("foreign crc", (unsigned long)c, HOST_PLAIN_CRC);
        ck("and the bytes equal our locally-built plaintext",
           outlen == HOST_PLAIN_LEN && memcmp(back, plain, HOST_PLAIN_LEN) == 0);
    }

    // --- 4: our own round trip, and it must be a REAL zlib stream ------------
    {
        uLongf clen = sizeof(comp);
        uLongf ulen = sizeof(back);
        int rc = compress2(comp, &clen, plain, HOST_PLAIN_LEN, 6);
        ck("compress2() returns Z_OK", rc == Z_OK);
        snprintf(b, sizeof(b), "[ZLIBTEST]      compress2: %u -> %lu bytes, CMF=0x%02x FLG=0x%02x\n",
                 HOST_PLAIN_LEN, (unsigned long)clen, comp[0], comp[1]);
        line(b);
        ck("output has a valid RFC1950 header (CMF low nibble 8 = deflate)",
           clen > 2 && (comp[0] & 0x0f) == 8);
        ck("output header checksum ((CMF<<8|FLG) % 31 == 0)",
           clen > 2 && ((unsigned)comp[0] * 256 + comp[1]) % 31 == 0);
        ck("compressBound() is not less than the real output",
           compressBound(HOST_PLAIN_LEN) >= clen);
        rc = uncompress(back, &ulen, comp, clen);
        ck("uncompress() of our own output returns Z_OK", rc == Z_OK);
        ck("round trip is byte-identical",
           ulen == HOST_PLAIN_LEN && memcmp(back, plain, HOST_PLAIN_LEN) == 0);
    }

    // --- 5: the shim regression: STREAMING through a 64-byte window ----------
    // Both hand-written zlib_shim.c copies got this wrong. One only worked when
    // the caller's avail_out already fitted the entire output; the other spun
    // forever making no progress. A real zlib must make progress every call.
    {
        static unsigned char win[64];
        static unsigned char acc[8192];
        z_stream zs;
        unsigned accn = 0, rounds = 0;
        int rc, ok = 1;

        memset(&zs, 0, sizeof(zs));
        rc = deflateInit(&zs, 6);
        ck("deflateInit() returns Z_OK", rc == Z_OK);
        zs.next_in = plain; zs.avail_in = HOST_PLAIN_LEN;
        do {
            zs.next_out = win; zs.avail_out = (uInt)sizeof(win);
            rc = deflate(&zs, Z_FINISH);
            if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) { ok = 0; break; }
            unsigned produced = (unsigned)(sizeof(win) - zs.avail_out);
            if (produced == 0 && rc != Z_STREAM_END) { ok = 0; break; } // zero progress
            if (accn + produced > sizeof(acc)) { ok = 0; break; }
            memcpy(acc + accn, win, produced); accn += produced;
            if (++rounds > 512) { ok = 0; break; }
        } while (rc != Z_STREAM_END);
        deflateEnd(&zs);
        snprintf(b, sizeof(b), "[ZLIBTEST]      streaming deflate: %u rounds of a 64-byte window, %u bytes out\n",
                 rounds, accn);
        line(b);
        ck("streaming deflate through a 64-byte window reaches Z_STREAM_END",
           ok && rc == Z_STREAM_END);
        ck("it needed more than one round (the window really was too small)", rounds > 1);

        // ...and inflate it back, also through a tiny window.
        memset(&zs, 0, sizeof(zs));
        rc = inflateInit(&zs);
        ck("inflateInit() returns Z_OK", rc == Z_OK);
        zs.next_in = acc; zs.avail_in = accn;
        unsigned outn = 0; rounds = 0; ok = 1;
        do {
            zs.next_out = win; zs.avail_out = (uInt)sizeof(win);
            rc = inflate(&zs, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) { ok = 0; break; }
            unsigned produced = (unsigned)(sizeof(win) - zs.avail_out);
            if (produced == 0 && rc != Z_STREAM_END) { ok = 0; break; }
            if (outn + produced > sizeof(back)) { ok = 0; break; }
            memcpy(back + outn, win, produced); outn += produced;
            if (++rounds > 512) { ok = 0; break; }
        } while (rc != Z_STREAM_END);
        inflateEnd(&zs);
        snprintf(b, sizeof(b), "[ZLIBTEST]      streaming inflate: %u rounds, %u bytes out\n",
                 rounds, outn);
        line(b);
        ck("streaming inflate reaches Z_STREAM_END", ok && rc == Z_STREAM_END);
        ck("streamed round trip is byte-identical",
           outn == HOST_PLAIN_LEN && memcmp(back, plain, HOST_PLAIN_LEN) == 0);
    }

    // --- 6: the gzip wrapper still works (it lives in deflate.c/inflate.c,
    //         not in the excluded gz*.c file layer) -----------------------------
    {
        z_stream zs;
        uLongf ulen;
        int rc;
        memset(&zs, 0, sizeof(zs));
        rc = deflateInit2(&zs, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
        ck("deflateInit2(windowBits=31) selects the gzip wrapper", rc == Z_OK);
        zs.next_in = plain; zs.avail_in = HOST_PLAIN_LEN;
        zs.next_out = comp; zs.avail_out = (uInt)sizeof(comp);
        rc = deflate(&zs, Z_FINISH);
        uLong glen = sizeof(comp) - zs.avail_out;
        deflateEnd(&zs);
        ck("gzip deflate reaches Z_STREAM_END", rc == Z_STREAM_END);
        ck("output starts with the gzip magic 1f 8b", glen > 2 && comp[0] == 0x1f && comp[1] == 0x8b);

        memset(&zs, 0, sizeof(zs));
        rc = inflateInit2(&zs, 15 + 16);
        zs.next_in = comp; zs.avail_in = (uInt)glen;
        zs.next_out = back; zs.avail_out = (uInt)sizeof(back);
        rc = inflate(&zs, Z_FINISH);
        ulen = sizeof(back) - zs.avail_out;
        inflateEnd(&zs);
        ck("gzip inflate round trip is byte-identical",
           rc == Z_STREAM_END && ulen == HOST_PLAIN_LEN &&
           memcmp(back, plain, HOST_PLAIN_LEN) == 0);
    }

    snprintf(b, sizeof(b), "[ZLIBTEST] ==== RESULT pass=%d fail=%d ====\n", g_pass, g_fail);
    line(b);
    return g_fail == 0 ? 0 : 1;
}
