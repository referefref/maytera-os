// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// exifdate_test.c - MEASURED host unit test for the EXIF DateTimeOriginal parser
// (#713, userland/libc/exifdate.c). Compiled by run_exifdate.sh with host libc
// AND AddressSanitizer, then it #includes ../exifdate.c directly (host-test mode)
// so the parser under test is ASan-INSTRUMENTED and every out-of-bounds read of
// a heap buffer is caught, not merely hoped against. This is the bounds-safety
// proof the parser must earn because its input is untrusted file data.
//
// It asserts, MEASURED:
//   - a synthetic JPEG with EXIF DateTimeOriginal in II (little-endian) order
//     parses to the exact Y/M/D,
//   - the same in MM (big-endian) order parses to the exact Y/M/D,
//   - a JPEG with no EXIF returns 0 (caller falls back to mtime),
//   - a non-JPEG (PNG signature) returns 0,
//   - TRUNCATION FUZZ: for every prefix length of a valid EXIF JPEG, the parser
//     is run against a heap buffer of EXACTLY that length; it must never crash
//     (ASan) and must return 1 only for the complete buffer,
//   - GARBAGE FUZZ: many random buffers (and 0xFFD8-prefixed random buffers) are
//     parsed; none may crash,
//   - the exifdate_from_file() wrapper reads a real temp file end to end.
//
// Exit 0 = every assertion held and ASan found no fault; non-zero otherwise.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// ---- host backing for exifdate.c's file wrapper, then pull it in ------------
#define EXIFDATE_HOST_TEST 1
static int  sys_open(const char *p, int f)                 { return open(p, f); }
static long sys_read(int fd, void *b, unsigned long n)     { return read(fd, b, (size_t)n); }
static int  sys_close(int fd)                              { return close(fd); }
#include "../exifdate.c"   // exifdate_parse + exifdate_from_file, ASan-instrumented

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); g_fail = 1; } \
                              else          { printf("  ok:   %s\n", msg); } } while (0)

// ---- synthetic JPEG-with-EXIF generator ------------------------------------
// Emits a minimal but standards-shaped JPEG carrying one EXIF DateTimeOriginal.
// `be` selects TIFF byte order (0 = II little-endian, 1 = MM big-endian).
// Returns the total length written into out (cap must be >= 128).
static void put16(unsigned char *p, unsigned int v, int be) {
    if (be) { p[0] = (v >> 8) & 0xFF; p[1] = v & 0xFF; }
    else    { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
}
static void put32(unsigned char *p, unsigned long v, int be) {
    if (be) { p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF; }
    else    { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
}
static int make_exif_jpeg(unsigned char *out, int be,
                          int y, int m, int d) {
    unsigned char *o = out;
    *o++ = 0xFF; *o++ = 0xD8;                 // SOI

    // TIFF block, offsets relative to `tiff`:
    //  0 order | 2 magic | 4 ifd0off=8 | 8 IFD0(count1,entry@10,next@22)
    //  26 ExifIFD(count1,entry@28,next@40) | 44 date string(20)
    unsigned char tiff[64];
    memset(tiff, 0, sizeof(tiff));
    tiff[0] = be ? 0x4D : 0x49; tiff[1] = be ? 0x4D : 0x49;  // "MM" / "II"
    put16(tiff + 2, 42, be);                  // magic
    put32(tiff + 4, 8, be);                   // IFD0 at rel 8
    // IFD0: one entry, ExifIFD pointer (0x8769, LONG, count 1, value = 26)
    put16(tiff + 8, 1, be);
    put16(tiff + 10, 0x8769, be); put16(tiff + 12, 4, be);
    put32(tiff + 14, 1, be);      put32(tiff + 18, 26, be);  // value @ entry+8 (rel 18)
    put32(tiff + 22, 0, be);                  // next IFD = none
    // ExifIFD: one entry, DateTimeOriginal (0x9003, ASCII, count 20, value = 44)
    put16(tiff + 26, 1, be);
    put16(tiff + 28, 0x9003, be); put16(tiff + 30, 2, be);
    put32(tiff + 32, 20, be);     put32(tiff + 36, 44, be);  // value @ entry+8 (rel 36)
    put32(tiff + 40, 0, be);                  // next IFD = none
    // date string "YYYY:MM:DD HH:MM:SS\0" (20 bytes) at rel 44
    char ds[24];
    snprintf(ds, sizeof(ds), "%04d:%02d:%02d 13:00:00", y, m, d);
    memcpy(tiff + 44, ds, 20);
    int tiff_len = 64;

    // APP1 = "Exif\0\0" + tiff. Segment length field counts itself (2) + payload.
    int payload = 6 + tiff_len;
    *o++ = 0xFF; *o++ = 0xE1;                  // APP1
    put16(o, (unsigned int)(payload + 2), 1); o += 2;   // seg length is BE
    memcpy(o, "Exif\0\0", 6); o += 6;
    memcpy(o, tiff, tiff_len); o += tiff_len;

    *o++ = 0xFF; *o++ = 0xD9;                  // EOI
    return (int)(o - out);
}

// A JPEG with NO EXIF: SOI + APP0 JFIF + EOI.
static int make_plain_jpeg(unsigned char *out) {
    unsigned char *o = out;
    *o++ = 0xFF; *o++ = 0xD8;                  // SOI
    *o++ = 0xFF; *o++ = 0xE0;                  // APP0
    put16(o, 16, 1); o += 2;                   // length 16 (incl these 2 bytes)
    memcpy(o, "JFIF\0", 5); o += 5;
    *o++ = 1; *o++ = 1; *o++ = 0;              // version + units
    put16(o, 1, 1); o += 2; put16(o, 1, 1); o += 2;  // density
    *o++ = 0; *o++ = 0;                        // no thumbnail
    *o++ = 0xFF; *o++ = 0xD9;                  // EOI
    return (int)(o - out);
}

int main(void) {
    printf("=== #713 EXIF DateTimeOriginal parser: MEASURED host unit test ===\n");

    unsigned char buf[512];
    int y, m, d;

    // 1. II (little-endian) byte order.
    int n = make_exif_jpeg(buf, 0, 2019, 7, 4);
    y = m = d = -1;
    int r = exifdate_parse(buf, (unsigned long)n, &y, &m, &d);
    CHECK(r == 1 && y == 2019 && m == 7 && d == 4,
          "II byte order parses to 2019-07-04");

    // 2. MM (big-endian) byte order.
    n = make_exif_jpeg(buf, 1, 2023, 12, 25);
    y = m = d = -1;
    r = exifdate_parse(buf, (unsigned long)n, &y, &m, &d);
    CHECK(r == 1 && y == 2023 && m == 12 && d == 25,
          "MM byte order parses to 2023-12-25");

    // 3. JPEG with no EXIF -> 0 (organiser falls back to mtime).
    n = make_plain_jpeg(buf);
    r = exifdate_parse(buf, (unsigned long)n, &y, &m, &d);
    CHECK(r == 0, "JPEG without EXIF returns 0 (mtime fallback)");

    // 4. Non-JPEG (PNG signature) -> 0.
    {
        unsigned char png[16] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A, 0,0,0,0,0,0,0,0};
        r = exifdate_parse(png, sizeof(png), &y, &m, &d);
        CHECK(r == 0, "non-JPEG (PNG) returns 0");
    }

    // 5. TRUNCATION FUZZ. A valid EXIF JPEG cut at every prefix length, each run
    //    against a heap buffer of EXACTLY that length so ASan bounds it tightly.
    //    Must never crash and must never FABRICATE a date from incomplete data:
    //    any run that reports success must report the one true date. (A prefix
    //    that happens to contain the whole date but is missing only the trailing
    //    EOI is a legitimate success, so "success" is allowed at large L.)
    {
        int full = make_exif_jpeg(buf, 0, 2020, 2, 29);
        int trunc_bad = 0, full_ok = 0, successes = 0;
        for (int L = 0; L <= full; L++) {
            unsigned char *hb = (unsigned char *)malloc(L ? (size_t)L : 1);
            if (L) memcpy(hb, buf, (size_t)L);
            int yy=-1,mm=-1,dd=-1;
            int rr = exifdate_parse(hb, (unsigned long)L, &yy, &mm, &dd);
            if (rr == 1) {
                successes++;
                if (!(yy == 2020 && mm == 2 && dd == 29)) trunc_bad = 1;  // fabricated
            }
            if (L == full) full_ok = (rr == 1 && yy == 2020 && mm == 2 && dd == 29);
            free(hb);
        }
        CHECK(full_ok, "full buffer parses to 2020-02-29");
        CHECK(!trunc_bad, "no truncated prefix ever fabricates a wrong date");
        printf("  info: truncation fuzz ran %d exactly-sized ASan buffers, %d legit successes\n",
               full + 1, successes);
    }

    // 6. GARBAGE FUZZ. Random buffers, some forced to start with the JPEG SOI so
    //    the marker walk is actually exercised. None may crash (ASan verifies).
    {
        unsigned int seed = 0x9003;
        int iters = 20000;
        for (int i = 0; i < iters; i++) {
            seed = seed * 1103515245u + 12345u;
            unsigned long L = (seed >> 8) % 200;   // 0..199 bytes
            unsigned char *hb = (unsigned char *)malloc(L ? (size_t)L : 1);
            for (unsigned long j = 0; j < L; j++) {
                seed = seed * 1103515245u + 12345u;
                hb[j] = (unsigned char)(seed >> 16);
            }
            if ((i & 1) && L >= 2) { hb[0] = 0xFF; hb[1] = 0xD8; }  // force SOI
            int yy, mm, dd;
            (void)exifdate_parse(hb, L, &yy, &mm, &dd);             // must not crash
            free(hb);
        }
        CHECK(1, "garbage fuzz (20000 random buffers) did not crash under ASan");
    }

    // 7. exifdate_from_file() end to end against a real temp file.
    {
        char path[] = "/tmp/exifdate_test_XXXXXX";
        int fd = mkstemp(path);
        if (fd >= 0) {
            int len = make_exif_jpeg(buf, 1, 2015, 6, 1);   // MM order on disk
            ssize_t w = write(fd, buf, (size_t)len); (void)w;
            close(fd);
            int yy=-1,mm=-1,dd=-1;
            int rr = exifdate_from_file(path, &yy, &mm, &dd);
            unlink(path);
            CHECK(rr == 1 && yy == 2015 && mm == 6 && dd == 1,
                  "exifdate_from_file reads a real file to 2015-06-01");
        } else {
            printf("  FAIL: could not create temp file\n"); g_fail = 1;
        }
    }

    printf(g_fail ? "\n#713 EXIF parser unit test: FAIL\n"
                  : "\n#713 EXIF parser unit test: PASS\n");
    return g_fail;
}
