#!/usr/bin/env python3
"""Rasterize the CALC toolbar SVG masters to MICO .ICN (and reference PNG).

Usage:  gen-icn.py [--size N] [--svg DIR] [--out DIR]
        defaults: --size 64, --svg svg/, --out . (writes icn/ and png/ under it)

Pipeline (matches assets/icons/README.md): ONE scale, straight from the
vector master to the target canvas with rsvg-convert, transparent
background, then packed to MICO exactly as userland/apps/files/main.c
mico_get() reads it: 'MICO' + u32 LE width + u32 LE height + w*h pixels of
B,G,R,A bytes (little-endian 0xAARRGGBB).

Standard library only (no PIL on the build container): the PNG rsvg-convert
emits is 8-bit RGBA, non-interlaced, which is decoded here including all five
scanline filters.
"""
import os, struct, subprocess, sys, zlib

NAMES = {   # svg basename -> /ICONS/<NAME>.ICN (uppercase, max 8 chars, unique)
    "new":                "SCNEW",
    "open":               "SCOPEN",
    "save":               "SCSAVE",
    "bold":               "SCBOLD",
    "italic":             "SCITALIC",
    "underline":          "SCUNDERL",
    "align-left":         "SCALIGNL",
    "align-center":       "SCALIGNC",
    "align-right":        "SCALIGNR",
    "format-number":      "SCFMTNUM",
    "format-currency":    "SCFMTCUR",
    "format-percent":     "SCFMTPCT",
    "format-decimal-inc": "SCDECINC",
    "format-decimal-dec": "SCDECDEC",
    "sum-sigma":          "SCSUM",
    "function-fx":        "SCFX",
    "border-all":         "SCBRDALL",
    "border-outer":       "SCBRDOUT",
    "insert-row":         "SCINSROW",
    "insert-column":      "SCINSCOL",
    "freeze-panes":       "SCFREEZE",
    "sort-asc":           "SCSORTA",
    "sort-desc":          "SCSORTD",
}

def decode_png(data):
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, ilace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and ctype == 6 and ilace == 0, (depth, ctype, ilace)
        elif typ == b"IDAT":
            idat += body
    raw = zlib.decompress(idat)
    stride = w * 4
    out = bytearray(); prev = bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]; line = bytearray(raw[p + 1:p + 1 + stride]); p += 1 + stride
        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0
            if f == 0: pred = 0
            elif f == 1: pred = a
            elif f == 2: pred = b
            elif f == 3: pred = (a + b) // 2
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            else: raise ValueError("bad filter %d" % f)
            line[i] = (line[i] + pred) & 0xFF
        out += line; prev = line
    return w, h, bytes(out)   # RGBA

def main():
    args = sys.argv[1:]
    size, svgdir, outdir = 64, "svg", "."
    while args:
        a = args.pop(0)
        if a == "--size": size = int(args.pop(0))
        elif a == "--svg": svgdir = args.pop(0)
        elif a == "--out": outdir = args.pop(0)
        else: sys.exit("unknown arg " + a)
    icn_dir = os.path.join(outdir, "icn"); png_dir = os.path.join(outdir, "png")
    os.makedirs(icn_dir, exist_ok=True); os.makedirs(png_dir, exist_ok=True)
    assert len(set(NAMES.values())) == len(NAMES)
    for base, icn in NAMES.items():
        assert icn.isupper() and len(icn) <= 8, icn
        svg = os.path.join(svgdir, base + ".svg")
        png = subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size),
                              "--background-color=none", svg],
                             check=True, capture_output=True).stdout
        with open(os.path.join(png_dir, base + ".png"), "wb") as f: f.write(png)
        w, h, rgba = decode_png(png)
        assert (w, h) == (size, size), (w, h)
        bgra = bytearray(len(rgba))
        bgra[0::4] = rgba[2::4]; bgra[1::4] = rgba[1::4]; bgra[2::4] = rgba[0::4]; bgra[3::4] = rgba[3::4]
        with open(os.path.join(icn_dir, icn + ".ICN"), "wb") as f:
            f.write(b"MICO" + struct.pack("<II", w, h) + bytes(bgra))
        opaque = sum(1 for i in range(3, len(rgba), 4) if rgba[i])
        print(f"{base:20s} -> {icn}.ICN {w}x{h}  coverage px={opaque}")

if __name__ == "__main__":
    main()
