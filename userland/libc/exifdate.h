// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// exifdate.h - minimal, bounds-safe EXIF capture-date reader (#713).
//
// Reads the EXIF DateTimeOriginal (TIFF tag 0x9003) out of a JPEG/JFIF file so
// the photo organiser (photorg.c) can group by the real CAPTURE date instead of
// the file modification time. This is a deliberately small parser: it walks the
// JPEG markers to the APP1 "Exif\0\0" segment, parses the TIFF header (both II
// little-endian and MM big-endian byte orders), follows IFD0 -> ExifIFD
// (0x8769) -> DateTimeOriginal (0x9003), and returns the Y/M/D.
//
// SECURITY: the input is UNTRUSTED file data. Every file-driven offset is
// bounds-checked against the buffer length before it is dereferenced; a
// malformed, truncated or hostile file MUST fail cleanly (return 0), never read
// out of bounds and never crash. Callers treat a 0 return as "no capture date"
// and fall back to mtime.
#ifndef EXIFDATE_H
#define EXIFDATE_H

// Buffer-based core. Parses buf[0..len) as the beginning of a JPEG file. On
// success fills *y (e.g. 2026), *m (1..12), *d (1..31) and returns 1. Returns 0
// for anything that is not a JPEG, has no EXIF APP1, has no DateTimeOriginal, or
// is malformed/truncated. Pure: no I/O, no allocation, safe to fuzz directly.
int exifdate_parse(const unsigned char *buf, unsigned long len,
                   int *y, int *m, int *d);

// File wrapper. Reads a bounded prefix of `path` (the EXIF APP1 segment lives
// near the start of a JPEG) and runs exifdate_parse(). Returns 1 and fills
// *y/*m/*d on success; 0 on any failure (not a JPEG, no EXIF, truncated, or an
// I/O error). Never blocks on more than a bounded read.
int exifdate_from_file(const char *path, int *y, int *m, int *d);

#endif // EXIFDATE_H
