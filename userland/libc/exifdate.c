// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// exifdate.c - minimal, bounds-safe EXIF DateTimeOriginal reader (#713).
// See exifdate.h. Userland C: no in-kernel float constraint applies and no new
// Rust requirement (this is a Ring-3 libc helper, not new kernel code). The
// parser is pure integer pointer arithmetic with a bounds check on EVERY
// file-driven offset, because the input is untrusted file data.
// EXIFDATE_HOST_TEST lets the host unit test (tests/exifdate_test.c) compile
// this file with host libc + AddressSanitizer to prove the parser is bounds-safe
// on untrusted input. In that mode the test supplies malloc/free and the sys_*
// file primitives; the shipped build takes the MayteraOS libc headers below.
#ifndef EXIFDATE_HOST_TEST
#include "syscall.h"    // sys_open / sys_read / sys_close
#include "stdlib.h"     // malloc / free
#endif
#include "exifdate.h"

// ---------------------------------------------------------------------------
// Endian-aware, bounds-checked field reads. `off` is an ABSOLUTE offset into
// buf; every read refuses to touch a byte at or past `len`. Offsets originate
// from 32-bit file fields, so on a 64-bit unsigned long `off + N` cannot wrap
// (off <= ~4G + a small base, N <= 4), and the comparison against the small
// `len` (we only ever read a bounded prefix) rejects anything out of range.
// ---------------------------------------------------------------------------
static int rd16(const unsigned char *b, unsigned long len, unsigned long off,
                int be, unsigned int *out) {
    if (off + 2 > len || off > len) return 0;
    if (be) *out = ((unsigned int)b[off] << 8) | b[off + 1];
    else    *out = ((unsigned int)b[off + 1] << 8) | b[off];
    return 1;
}
static int rd32(const unsigned char *b, unsigned long len, unsigned long off,
                int be, unsigned long *out) {
    if (off + 4 > len || off > len) return 0;
    if (be) *out = ((unsigned long)b[off] << 24) | ((unsigned long)b[off + 1] << 16) |
                   ((unsigned long)b[off + 2] << 8) | (unsigned long)b[off + 3];
    else    *out = ((unsigned long)b[off + 3] << 24) | ((unsigned long)b[off + 2] << 16) |
                   ((unsigned long)b[off + 1] << 8) | (unsigned long)b[off];
    return 1;
}

// Parse an EXIF ASCII "YYYY:MM:DD HH:MM:SS" starting at absolute `off`. Only the
// first 10 chars (the date) are needed; each is bounds-checked. EXIF uses ':'
// separators; a few cameras emit '-', accepted here as harmless leniency.
static int parse_datestr(const unsigned char *b, unsigned long len,
                         unsigned long off, int *y, int *m, int *d) {
    if (off + 10 > len || off > len) return 0;
    for (int i = 0; i < 10; i++) {
        unsigned char ch = b[off + i];
        if (i == 4 || i == 7) { if (ch != ':' && ch != '-') return 0; }
        else                  { if (ch < '0' || ch > '9')   return 0; }
    }
    int yy = (b[off] - '0') * 1000 + (b[off + 1] - '0') * 100 +
             (b[off + 2] - '0') * 10 + (b[off + 3] - '0');
    int mm = (b[off + 5] - '0') * 10 + (b[off + 6] - '0');
    int dd = (b[off + 8] - '0') * 10 + (b[off + 9] - '0');
    if (yy < 1 || mm < 1 || mm > 12 || dd < 1 || dd > 31) return 0;
    *y = yy; *m = mm; *d = dd;
    return 1;
}

// TIFF type byte sizes, indexed by type id 1..12 (0 = unknown/invalid).
static unsigned int tiff_type_size(unsigned int type) {
    static const unsigned char tsz[13] = { 0,1,1,2,4,8,1,1,2,4,8,4,8 };
    return (type >= 1 && type <= 12) ? tsz[type] : 0;
}

// Search the IFD at absolute offset `ifd_abs` for `want_tag`. On a match, set
// *out_valoff to the ABSOLUTE offset of the tag's value bytes (inline when the
// value fits in 4 bytes, otherwise the pointer field is a tiff-relative offset)
// and *out_type/*out_count to the type and count. Returns 1 if found, else 0.
// Every entry access is bounds-checked; a bogus count/offset just fails.
static int ifd_find(const unsigned char *b, unsigned long len, unsigned long tiff,
                    unsigned long ifd_abs, int be, unsigned int want_tag,
                    unsigned int *out_type, unsigned long *out_count,
                    unsigned long *out_valoff) {
    unsigned int nent;
    if (!rd16(b, len, ifd_abs, be, &nent)) return 0;
    unsigned long e = ifd_abs + 2;
    for (unsigned int i = 0; i < nent; i++, e += 12) {
        if (e + 12 > len) return 0;                  // entry runs past the buffer
        unsigned int tag, type;
        unsigned long count;
        if (!rd16(b, len, e,     be, &tag))   return 0;
        if (!rd16(b, len, e + 2, be, &type))  return 0;
        if (!rd32(b, len, e + 4, be, &count)) return 0;
        if (tag != want_tag) continue;

        unsigned int esz = tiff_type_size(type);
        if (esz == 0) return 0;
        // count is untrusted: cap so esz*count cannot overflow before compare.
        if (count > 0xffffffUL) return 0;
        unsigned long bytes = (unsigned long)esz * count;
        if (bytes <= 4) {
            *out_valoff = e + 8;                     // value stored inline
        } else {
            unsigned long ptr;
            if (!rd32(b, len, e + 8, be, &ptr)) return 0;
            *out_valoff = tiff + ptr;                // tiff-relative pointer
        }
        *out_type = type; *out_count = count;
        return 1;
    }
    return 0;
}

// Parse a TIFF block whose header starts at absolute offset `tiff`.
static int parse_tiff(const unsigned char *b, unsigned long len, unsigned long tiff,
                      int *y, int *m, int *d) {
    if (tiff + 8 > len || tiff > len) return 0;      // need the 8-byte header
    int be;
    if      (b[tiff] == 0x49 && b[tiff + 1] == 0x49) be = 0;   // "II"
    else if (b[tiff] == 0x4D && b[tiff + 1] == 0x4D) be = 1;   // "MM"
    else return 0;
    unsigned int magic;
    if (!rd16(b, len, tiff + 2, be, &magic) || magic != 42) return 0;
    unsigned long ifd0_off;
    if (!rd32(b, len, tiff + 4, be, &ifd0_off)) return 0;

    // IFD0 -> ExifIFD pointer (tag 0x8769, a LONG offset relative to tiff).
    unsigned int type; unsigned long count, valoff;
    if (!ifd_find(b, len, tiff, tiff + ifd0_off, be, 0x8769,
                  &type, &count, &valoff)) return 0;
    unsigned long exif_ptr;
    if (!rd32(b, len, valoff, be, &exif_ptr)) return 0;

    // ExifIFD -> DateTimeOriginal (tag 0x9003, ASCII).
    if (!ifd_find(b, len, tiff, tiff + exif_ptr, be, 0x9003,
                  &type, &count, &valoff)) return 0;
    return parse_datestr(b, len, valoff, y, m, d);
}

int exifdate_parse(const unsigned char *buf, unsigned long len,
                   int *y, int *m, int *d) {
    if (!buf || !y || !m || !d || len < 4) return 0;
    if (buf[0] != 0xFF || buf[1] != 0xD8) return 0;  // JPEG SOI

    unsigned long p = 2;
    // Walk the JPEG marker segments looking for APP1 (0xFFE1) "Exif\0\0".
    while (p + 4 <= len) {
        if (buf[p] != 0xFF) return 0;                // a marker must start 0xFF
        unsigned char marker = buf[p + 1];
        // Standalone markers (no length payload): SOI/EOI, RSTn, TEM.
        if (marker == 0xD8 || marker == 0xD9 ||
            (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) {
            p += 2; continue;
        }
        if (marker == 0xDA) return 0;                // start of scan: no more meta
        unsigned int seglen;
        if (!rd16(buf, len, p + 2, 1, &seglen)) return 0;  // seg length is BE
        if (seglen < 2) return 0;                    // includes its own 2 bytes
        unsigned long seg_payload = p + 4;           // after marker + length
        if (marker == 0xE1) {                        // APP1
            if (seg_payload + 6 <= len &&
                buf[seg_payload]     == 'E' && buf[seg_payload + 1] == 'x' &&
                buf[seg_payload + 2] == 'i' && buf[seg_payload + 3] == 'f' &&
                buf[seg_payload + 4] == 0   && buf[seg_payload + 5] == 0) {
                if (parse_tiff(buf, len, seg_payload + 6, y, m, d)) return 1;
            }
            // A non-EXIF APP1 (e.g. XMP): keep scanning later segments.
        }
        p = p + 2 + seglen;                          // next marker
    }
    return 0;
}

int exifdate_from_file(const char *path, int *y, int *m, int *d) {
    if (!path || !y || !m || !d) return 0;
    int fd = sys_open(path, 0);
    if (fd < 0) return 0;
    // The EXIF APP1 segment lives at the very start of a JPEG; a 64K prefix
    // covers it with margin. Bounded read: never slurp an arbitrarily large file.
    const unsigned long CAP = 65536;
    unsigned char *buf = malloc(CAP);
    if (!buf) { sys_close(fd); return 0; }
    unsigned long got = 0;
    while (got < CAP) {
        long r = sys_read(fd, buf + got, CAP - got);
        if (r <= 0) break;
        got += (unsigned long)r;
    }
    sys_close(fd);
    int ok = exifdate_parse(buf, got, y, m, d);
    free(buf);
    return ok;
}
