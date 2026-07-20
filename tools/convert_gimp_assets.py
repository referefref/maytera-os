#!/usr/bin/env python3
# Convert GIMP stock brushes (.gbr/.gih) and patterns (.pat) into C headers
# for Maytera Studio. Grayscale masks (alpha for RGBA brushes), max 64x64,
# nearest-neighbour downscale. Output: brushes_data.h, patterns_data.h.
import os, struct, sys

SRC = sys.argv[1]
OUT = sys.argv[2]
MAXDIM = 64

def rd_gbr(buf, off=0):
    # returns (name, w, h, spacing, mask_bytes) or None
    if len(buf) < off + 28: return None
    hs, ver, w, h, bpp = struct.unpack_from('>IIIII', buf, off)
    magic = buf[off+20:off+24]
    if magic != b'GIMP' or ver < 2 or hs < 28: return None
    spacing = struct.unpack_from('>I', buf, off+24)[0]
    name = buf[off+28:off+hs].split(b'\x00')[0].decode('utf-8', 'replace')
    data_off = off + hs
    n = w * h * bpp
    if len(buf) < data_off + n: return None
    raw = buf[data_off:data_off+n]
    if bpp == 1:
        mask = raw
    elif bpp == 4:
        mask = bytes(raw[i*4+3] for i in range(w*h))   # alpha as mask
    else:
        return None
    return (name, w, h, spacing, mask, data_off + n)

def rd_gih_first(buf):
    # text line 1: name, line 2: count + params; then gbr frames
    nl1 = buf.find(b'\n'); nl2 = buf.find(b'\n', nl1+1)
    if nl1 < 0 or nl2 < 0: return None
    name = buf[:nl1].decode('utf-8','replace').strip()
    r = rd_gbr(buf, nl2+1)
    if not r: return None
    _, w, h, sp, mask, _ = r
    return (name, w, h, sp, mask)

def rd_pat(buf):
    if len(buf) < 24: return None
    hs, ver, w, h, bpp = struct.unpack_from('>IIIII', buf, 0)
    if buf[20:24] != b'GPAT' or hs < 24: return None
    name = buf[24:hs].split(b'\x00')[0].decode('utf-8','replace')
    n = w*h*bpp
    if len(buf) < hs + n: return None
    raw = buf[hs:hs+n]
    if bpp == 3:
        rgb = raw
    elif bpp == 1:
        rgb = bytes(v for g in raw for v in (g,g,g))
    elif bpp == 4:
        rgb = bytes(v for i in range(w*h) for v in raw[i*4:i*4+3])
    else:
        return None
    return (name, w, h, rgb)

def scale(mask, w, h, ch):
    if w <= MAXDIM and h <= MAXDIM: return mask, w, h
    s = max(w, h) / MAXDIM
    nw, nh = max(1,int(w/s)), max(1,int(h/s))
    out = bytearray()
    for y in range(nh):
        sy = min(h-1, int(y*s))
        for x in range(nw):
            sx = min(w-1, int(x*s))
            for c in range(ch):
                out.append(mask[(sy*w+sx)*ch + c])
    return bytes(out), nw, nh

def cname(s):
    return ''.join(c if c.isalnum() else '_' for c in s)

brushes, patterns = [], []
for root, _, files in os.walk(os.path.join(SRC, 'data/brushes')):
    for f in sorted(files):
        p = os.path.join(root, f)
        buf = open(p, 'rb').read()
        r = None
        if f.endswith('.gbr'):
            g = rd_gbr(buf)
            if g: r = (g[0], g[1], g[2], g[3], g[4])
        elif f.endswith('.gih'):
            r = rd_gih_first(buf)
        if r:
            name, w, h, sp, mask = r
            mask, w, h = scale(mask, w, h, 1)
            brushes.append((name or f, w, h, max(1,sp), mask))
for root, _, files in os.walk(os.path.join(SRC, 'data/patterns')):
    for f in sorted(files):
        if not f.endswith('.pat'): continue
        buf = open(os.path.join(root, f), 'rb').read()
        r = rd_pat(buf)
        if r:
            name, w, h, rgb = r
            rgb, w, h = scale(rgb, w, h, 3)
            patterns.append((name or f, w, h, rgb))

def emit_bytes(fh, data):
    for i in range(0, len(data), 20):
        fh.write(''.join('%d,' % b for b in data[i:i+20]) + '\n')

with open(os.path.join(OUT, 'brushes_data.h'), 'w') as fh:
    fh.write('// Auto-generated from GIMP 2.10 stock brushes (GPL). Do not edit.\n')
    fh.write('// Grayscale masks, max %dpx. Generator: convert_gimp_assets.py\n' % MAXDIM)
    fh.write('#ifndef BRUSHES_DATA_H\n#define BRUSHES_DATA_H\n\n')
    fh.write('typedef struct { const char *name; int w, h, spacing; const unsigned char *mask; } studio_brush_t;\n\n')
    for i, (name, w, h, sp, mask) in enumerate(brushes):
        fh.write('static const unsigned char BR_%d_%s[%d] = {\n' % (i, cname(name)[:24], len(mask)))
        emit_bytes(fh, mask)
        fh.write('};\n')
    fh.write('\nstatic const studio_brush_t STOCK_BRUSHES[] = {\n')
    for i, (name, w, h, sp, mask) in enumerate(brushes):
        fh.write('  { "%s", %d, %d, %d, BR_%d_%s },\n' % (name.replace('"','')[:30], w, h, sp, i, cname(name)[:24]))
    fh.write('};\n#define STOCK_BRUSH_COUNT %d\n#endif\n' % len(brushes))

with open(os.path.join(OUT, 'patterns_data.h'), 'w') as fh:
    fh.write('// Auto-generated from GIMP 2.10 stock patterns (GPL). Do not edit.\n')
    fh.write('#ifndef PATTERNS_DATA_H\n#define PATTERNS_DATA_H\n\n')
    fh.write('typedef struct { const char *name; int w, h; const unsigned char *rgb; } studio_pattern_t;\n\n')
    for i, (name, w, h, rgb) in enumerate(patterns):
        fh.write('static const unsigned char PT_%d_%s[%d] = {\n' % (i, cname(name)[:24], len(rgb)))
        emit_bytes(fh, rgb)
        fh.write('};\n')
    fh.write('\nstatic const studio_pattern_t STOCK_PATTERNS[] = {\n')
    for i, (name, w, h, rgb) in enumerate(patterns):
        fh.write('  { "%s", %d, %d, PT_%d_%s },\n' % (name.replace('"','')[:30], w, h, i, cname(name)[:24]))
    fh.write('};\n#define STOCK_PATTERN_COUNT %d\n#endif\n' % len(patterns))

print('brushes:', len(brushes), 'patterns:', len(patterns))
