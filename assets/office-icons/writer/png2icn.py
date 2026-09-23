#!/usr/bin/env python3
"""png2icn.py: pack an 8-bit RGBA PNG into the compositor's MICO .ICN format.

Standard library only (zlib + struct), so it runs on the build container,
which has rsvg-convert but no PIL. Pairs with the icon README's rasterize step:

    rsvg-convert -w 64 -h 64 --background-color=none in.svg -o g.png
    png2icn.py g.png /path/to/NAME.ICN

MICO layout (matches userland/apps/compositor/icons.c icon_load_color() and
userland/apps/files/main.c mico_get(), read from the C, not from prose):
    'MICO' | u32 LE width | u32 LE height | width*height * u32 LE 0xAARRGGBB
A little-endian 0xAARRGGBB is the byte sequence B,G,R,A on disk.

Only the PNG shape rsvg-convert emits for a transparent-background icon is
accepted (8-bit, colour type 6 RGBA, non-interlaced); anything else fails
loudly rather than packing garbage. `--self-test` round-trips a synthetic
image through hand-built PNGs (filter 0 and filter 2) and asserts the packed
bytes are exact, including the on-disk byte order.
"""
import struct, sys, zlib


def read_png_rgba(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG")
    pos, idat, ihdr = 8, [], None
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat.append(body)
        elif typ == b"IEND":
            break
    if ihdr is None:
        raise SystemExit("PNG without IHDR")
    w, h, depth, ctype, comp, filt, interlace = ihdr
    if depth != 8 or ctype != 6 or interlace != 0:
        raise SystemExit("need 8-bit RGBA non-interlaced PNG (rsvg-convert output); "
                         "got depth=%d ctype=%d interlace=%d" % (depth, ctype, interlace))
    raw = zlib.decompress(b"".join(idat))
    bpp, stride = 4, w * 4
    if len(raw) != h * (stride + 1):
        raise SystemExit("PNG raw length mismatch")
    prev = bytearray(stride)
    rows = []
    for y in range(h):
        ft = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ft == 0: pred = 0
            elif ft == 1: pred = a
            elif ft == 2: pred = b
            elif ft == 3: pred = (a + b) // 2
            elif ft == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            else:
                raise SystemExit("bad PNG filter type %d" % ft)
            line[i] = (line[i] + pred) & 0xFF
        rows.append(bytes(line))
        prev = line
    return w, h, rows


def pack_mico(w, h, rows):
    out = bytearray(b"MICO" + struct.pack("<II", w, h))
    for line in rows:
        for x in range(w):
            r, g, b, a = line[x * 4:x * 4 + 4]
            out += bytes((b, g, r, a))
    return bytes(out)


def _write_png_rgba(w, h, rows, filt):
    raw = bytearray()
    prev = bytes(w * 4)
    for r in rows:
        raw.append(filt)
        raw += r if filt == 0 else bytes((r[i] - prev[i]) & 0xFF for i in range(w * 4))
        prev = r
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))


def self_test():
    w, h = 5, 3
    rows = [b"".join(bytes((x * 40, y * 90, (x + y) * 7, 255 - x * 30)) for x in range(w))
            for y in range(h)]
    for filt in (0, 2):
        png = _write_png_rgba(w, h, rows, filt)
        pw, ph, prow = read_png_rgba(png)
        assert (pw, ph) == (w, h) and prow == rows, "filter %d round-trip" % filt
    m = pack_mico(w, h, rows)
    assert m[:4] == b"MICO" and struct.unpack("<II", m[4:12]) == (w, h)
    # pixel (1,0): r=40 g=0 b=7 a=225 -> on disk B,G,R,A
    assert m[12 + 4:12 + 8] == bytes((7, 0, 40, 225)), "byte order"
    assert len(m) == 12 + w * h * 4
    print("png2icn self-test OK")


def main(argv):
    if len(argv) == 2 and argv[1] == "--self-test":
        self_test(); return
    if len(argv) != 3:
        raise SystemExit("usage: png2icn.py in.png out.ICN | --self-test")
    with open(argv[1], "rb") as f:
        w, h, rows = read_png_rgba(f.read())
    with open(argv[2], "wb") as f:
        f.write(pack_mico(w, h, rows))
    print("%s: %dx%d -> %s (%d bytes)" % (argv[1], w, h, argv[2], 12 + w * h * 4))


if __name__ == "__main__":
    main(sys.argv)
