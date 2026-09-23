#!/usr/bin/env python3
"""Generate an ORIGINAL, MayteraOS-authored ClassiCube default texture pack.

WHY THIS EXISTS (#28 deliverable 2, owner-requested 2026-08-17): ClassiCube's
upstream Resources.c builds its real default.zip by downloading and patching
Mojang's own "classic jar" / "1.6.2 jar" client.jar over HTTP at
launcher.mojang.com (see vendor/ClassiCube/src/Resources.c ~L864-865). Those
textures are Mojang/Microsoft copyrighted; MayteraOS has no license to
redistribute them inside the OS image, and the owner's real iMac has no
working NIC at all (no NIC our tree drivers, Broadcom onboard unsupported),
so a first-run HTTP fetch of them can never succeed there anyway.

This script does NOT touch, download, or derive anything from Mojang assets.
Every pixel is generated here, procedurally, except default.png's glyph
shapes, which are re-rasterised from vendor/ClassiCube/src/SystemFonts.c's
own `font_bitmap[]` table (BSD-3-Clause, already vendored in this repo,
credited there as "Goodly's texture pack for ClassiCube" - i.e. it is
ClassiCube's OWN fallback font data, not Mojang's, and reusing it under the
same BSD-3 terms that already cover the file it lives in is unambiguous).

Output: default.zip (all entries TexturePack.c / Resources.c's
defaultZipEntries[] requires, so Resources_CheckExistence sees
allZipEntriesExist=true and NEVER calls Http_AsyncGetData) and a stub
classicube.zip (satisfies the separate ccTexturesExist check; content is
irrelevant on desktop builds, see Resources.c CCTextures_SelectEntry, which
only looks for touch.png, a mobile-only asset this port does not build).

License of the generated pixel data: CC0 / public domain, same terms as
other MayteraOS-authored placeholder assets. See ATTRIBUTION.md.

Reproduce: `python3 gen-default-texpack.py <outdir>`
"""
import struct, zlib, sys, os, re, zipfile, io

# --------------------------------------------------------------------------
# Minimal PNG encoder (8-bit RGBA, filter type None). No external deps.
# --------------------------------------------------------------------------
def write_png(width, height, rgba):
    def chunk(tag, data):
        c = tag + data
        return struct.pack('>I', len(data)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)
    stride = width * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw.extend(rgba[y * stride:(y + 1) * stride])
    idat = zlib.compress(bytes(raw), 9)
    return sig + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b'')


class Image:
    """Simple RGBA pixel buffer."""
    def __init__(self, w, h, fill=(0, 0, 0, 0)):
        self.w, self.h = w, h
        self.px = bytearray(fill * (w * h))

    def set(self, x, y, rgba):
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * 4
            self.px[i:i + 4] = bytes(rgba)

    def fill_rect(self, x0, y0, w, h, rgba):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                self.set(x, y, rgba)

    def blit(self, src, x0, y0):
        for y in range(src.h):
            for x in range(src.w):
                i = (y * src.w + x) * 4
                px = src.px[i:i + 4]
                if px[3]:
                    self.set(x0 + x, y0 + y, px)

    def bytes(self):
        return bytes(self.px)


def hsv_to_rgb(h, s, v):
    i = int(h * 6.0)
    f = h * 6.0 - i
    p = v * (1.0 - s)
    q = v * (1.0 - f * s)
    t = v * (1.0 - (1.0 - f) * s)
    i %= 6
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]
    return (int(r * 255), int(g * 255), int(b * 255))


# --------------------------------------------------------------------------
# terrain.png: 256x256, 16x16 grid of 16px tiles. A small set of tile
# indices get a hand-picked, semantically-themed flat colour (grass/dirt/
# stone/sand/water/wood/leaves/wool bank, all ORIGINAL colour choices, not
# sampled from any Minecraft asset); every other tile gets a deterministic
# HSV-wheel colour so the full 256-tile grid is populated and no tile is
# ever the default.zip "unable to use" case. This is a placeholder look,
# not a claim of matching the real classic tile layout pixel-for-pixel.
# --------------------------------------------------------------------------
THEMED_TILES = {
    0:  (95, 165, 60),    # grass top
    1:  (128, 128, 128),  # stone
    2:  (134, 96, 67),    # dirt
    3:  (110, 110, 110),  # cobblestone
    4:  (172, 140, 92),   # wood planks
    5:  (60, 140, 60),    # sapling (flat, no transparency games here)
    7:  (60, 60, 60),     # bedrock/adminium
    8:  (60, 100, 200),   # water
    10: (220, 120, 40),   # lava
    12: (223, 209, 145),  # sand
    13: (150, 138, 120),  # gravel
    14: (210, 190, 90),   # gold ore
    15: (190, 160, 130),  # iron ore
    16: (70, 70, 70),     # coal ore
    17: (120, 85, 50),    # log
    18: (46, 120, 46),    # leaves
    19: (200, 200, 60),   # sponge
    20: (170, 220, 220),  # glass
    45: (170, 70, 40),    # brick
    49: (60, 30, 80),     # obsidian
}
WOOL_BASE = 21
WOOL_COLORS = [
    (200, 60, 60), (220, 130, 40), (220, 210, 60), (140, 200, 60),
    (60, 160, 60), (50, 150, 130), (60, 190, 190), (60, 150, 190),
    (60, 90, 200), (90, 60, 190), (140, 60, 190), (200, 60, 190),
    (230, 150, 190), (30, 30, 30), (120, 120, 120), (235, 235, 235),
]
for i, c in enumerate(WOOL_COLORS):
    THEMED_TILES[WOOL_BASE + i] = c


def make_terrain():
    img = Image(256, 256)
    for row in range(16):
        for col in range(16):
            idx = row * 16 + col
            if idx in THEMED_TILES:
                r, g, b = THEMED_TILES[idx]
            else:
                r, g, b = hsv_to_rgb((idx * 0.13) % 1.0, 0.45, 0.75)
            img.fill_rect(col * 16, row * 16, 16, 16, (r, g, b, 255))
            # 1px border so tiles read as distinct blocks rather than one wash
            for x in range(16):
                img.set(col * 16 + x, row * 16, tuple(min(255, c + 25) for c in (r, g, b)) + (255,))
            for y in range(16):
                img.set(col * 16, row * 16 + y, tuple(max(0, c - 25) for c in (r, g, b)) + (255,))
    return img


def make_particles():
    img = Image(128, 128)
    for row in range(16):
        for col in range(16):
            r, g, b = hsv_to_rgb((row * 16 + col) * 0.09 % 1.0, 0.5, 0.9)
            cx, cy = col * 8 + 4, row * 8 + 4
            for y in range(-2, 3):
                for x in range(-2, 3):
                    if x * x + y * y <= 5:
                        img.set(cx + x, cy + y, (r, g, b, 255))
    return img


def make_clouds():
    img = Image(256, 256, fill=(255, 255, 255, 0))
    for y in range(256):
        for x in range(256):
            n = ((x * 13 + y * 7) % 97) + ((x ^ y) % 31)
            if n % 5 < 2:
                a = 160 if n % 5 == 0 else 90
                img.set(x, y, (250, 250, 255, a))
    return img


def make_rain():
    img = Image(32, 32, fill=(0, 0, 0, 0))
    for y in range(32):
        x = (y // 2) % 32
        img.set(x, y, (110, 150, 210, 200))
        img.set((x + 1) % 32, y, (140, 180, 230, 120))
    return img


def make_snow():
    img = Image(16, 16, fill=(255, 255, 255, 235))
    return img


def skin_image(base_rgb, accent_rgb):
    """64x32 classic-layout skin canvas, filled with a two-tone pattern so
    whatever UV region a mob model samples lands on a readable colour."""
    img = Image(64, 32)
    img.fill_rect(0, 0, 64, 32, base_rgb + (255,))
    for by in range(0, 32, 8):
        for bx in range(0, 64, 8):
            if ((bx // 8) + (by // 8)) % 2 == 0:
                img.fill_rect(bx, by, 8, 8, accent_rgb + (255,))
    return img


def make_gui(top_half_only):
    img = Image(256, 256, fill=(0, 0, 0, 0))
    h = 128 if top_half_only else 256
    img.fill_rect(0, 0, 256, h, (40, 40, 48, 200))
    # hotbar-ish highlight strip
    img.fill_rect(0, 0, 256, 22, (70, 70, 82, 220))
    return img


def make_icons():
    img = Image(256, 256, fill=(0, 0, 0, 0))
    # crosshair, top-left corner of the used top-quarter (256x64)
    cx, cy = 16, 16
    for i in range(-6, 7):
        img.set(cx + i, cy, (255, 255, 255, 255))
        img.set(cx, cy + i, (255, 255, 255, 255))
    # health/food icon row
    for i in range(10):
        img.fill_rect(32 + i * 18, 4, 14, 14, (200, 50, 50, 255) if i % 2 == 0 else (170, 130, 60, 255))
    return img


def make_animations():
    """16 wide x 512 tall: 32 stacked 16x16 frames, matching the shipped
    animations.txt fire entry ('6 2 0 0 16 32 0' -> frame size 16, 32
    frames). Original colour-cycling flame look, not sampled Minecraft art."""
    img = Image(16, 512)
    for frame in range(32):
        t = frame / 32.0
        for y in range(16):
            for x in range(16):
                flicker = ((x * 7 + y * 3 + frame * 5) % 11) / 11.0
                r = 220
                g = int(90 + 100 * (1 - t) * flicker)
                b = int(20 * flicker)
                img.set(x, frame * 16 + y, (r, min(255, g), b, 255))
    return img


FONT_ROW_RE = re.compile(
    r'\{\s*(0x[0-9A-Fa-f]{2}(?:\s*,\s*0x[0-9A-Fa-f]{2}){7})\s*,?\s*\}')


def parse_font_bitmap(systemfonts_c_path):
    """Extract the font_bitmap[][8] table straight out of the vendored,
    BSD-3-Clause SystemFonts.c so default.png's glyph shapes are reused
    project data, not a fresh (and error-prone) hand transcription."""
    with open(systemfonts_c_path, 'r', encoding='utf-8') as f:
        text = f.read()
    start = text.index('static const cc_uint8 font_bitmap[][CELL_SIZE] = {')
    end = text.index('};', start)
    body = text[start:end]
    rows = []
    for m in FONT_ROW_RE.finditer(body):
        vals = [int(v.strip(), 16) for v in m.group(1).split(',')]
        rows.append(vals)
    assert len(rows) == 94, 'expected 94 glyph rows (! .. ~), got %d' % len(rows)
    return rows


def make_default_png(font_rows):
    """128x128, 16x16 grid of 8x8 cells, cell = (code & 0xF, code >> 4),
    matching Drawer2D.c's Font_SetBitmapAtlas / DrawBitmappedTextCore
    indexing. Glyphs '!'..'~' (33..126) come from font_rows; every other
    cell (space, control codes, 127-255) is left fully transparent."""
    img = Image(128, 128, fill=(0, 0, 0, 0))
    for code in range(33, 127):
        rows = font_rows[code - 33]
        cx, cy = (code & 0x0F) * 8, (code >> 4) * 8
        for ry, rowbits in enumerate(rows):
            for cxp in range(8):
                if rowbits & (1 << cxp):
                    img.set(cx + cxp, cy + ry, (255, 255, 255, 255))
    return img


def build(outdir, repo_root):
    systemfonts = os.path.join(
        repo_root, 'userland/apps/classicube/vendor/ClassiCube/src/SystemFonts.c')
    font_rows = parse_font_bitmap(systemfonts)

    entries = {
        'terrain.png': write_png(256, 256, bytes(make_terrain().px)),
        'particles.png': write_png(128, 128, bytes(make_particles().px)),
        'clouds.png': write_png(256, 256, bytes(make_clouds().px)),
        'rain.png': write_png(32, 32, bytes(make_rain().px)),
        'char.png': write_png(64, 32, bytes(skin_image((190, 160, 130), (90, 110, 150)).px)),
        'default.png': write_png(128, 128, bytes(make_default_png(font_rows).px)),
        'icons.png': write_png(256, 256, bytes(make_icons().px)),
        'gui_classic.png': write_png(256, 256, bytes(make_gui(True).px)),
        'creeper.png': write_png(64, 32, bytes(skin_image((60, 150, 60), (30, 100, 30)).px)),
        'pig.png': write_png(64, 32, bytes(skin_image((230, 170, 170), (200, 130, 130)).px)),
        'sheep.png': write_png(64, 32, bytes(skin_image((235, 235, 230), (210, 210, 200)).px)),
        'sheep_fur.png': write_png(64, 32, bytes(skin_image((240, 240, 235), (220, 220, 210)).px)),
        'skeleton.png': write_png(64, 32, bytes(skin_image((210, 210, 195), (180, 180, 165)).px)),
        'spider.png': write_png(64, 32, bytes(skin_image((60, 30, 30), (30, 15, 15)).px)),
        'zombie.png': write_png(64, 32, bytes(skin_image((90, 130, 90), (60, 100, 60)).px)),
        'chicken.png': write_png(64, 32, bytes(skin_image((240, 235, 210), (220, 200, 60)).px)),
        'snow.png': write_png(16, 16, bytes(make_snow().px)),
        'gui.png': write_png(256, 256, bytes(make_gui(True).px)),
        'animations.png': write_png(16, 512, bytes(make_animations().px)),
    }

    animations_txt = (
        "# This file defines the animations used in a texture pack for ClassiCube.\r\n"
        "# Each line is in the format : <TileX> <TileY> <FrameX> <FrameY> <Frame size> <Frames count> <Tick delay>\r\n"
        "# - TileX and TileY are the coordinates of the tile in terrain.png that will be replaced by the animation frames.\r\n"
        "#     Essentially, TileX and TileY are the remainder and quotient of an ID in F10 menu divided by 16\r\n"
        "#     For instance, obsidian texture(37) has TileX of 5, and TileY of 2\r\n"
        "# - FrameX and FrameY are the pixel coordinates of the first animation frame in animations.png.\r\n"
        "# - Frame Size is the size in pixels of an animation frame.\r\n"
        "# - Frames count is the number of used frames.  The first frame is located at\r\n"
        "#     (FrameX, FrameY), the second one at (FrameX + FrameSize, FrameY) and so on.\r\n"
        "# - Tick delay is the number of ticks a frame doesn't change. For instance, delay of 0\r\n"
        "#     means that the tile would be replaced every tick, while delay of 2 means\r\n"
        "#     'replace with frame 1, don't change frame, don't change frame, replace with frame 2'.\r\n"
        "# NOTE: If a file called 'uselavaanim' is in the texture pack, the game instead generates the lava texture animation.\r\n"
        "# NOTE: If a file called 'usewateranim' is in the texture pack, the game instead generates the water texture animation.\r\n"
        "\r\n"
        "# fire\r\n"
        "6 2 0 0 16 32 0"
    )
    entries['animations.txt'] = animations_txt.encode('ascii')

    os.makedirs(outdir, exist_ok=True)
    default_zip_path = os.path.join(outdir, 'default.zip')
    with zipfile.ZipFile(default_zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in entries.items():
            zi = zipfile.ZipInfo(name, date_time=(2026, 8, 17, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_DEFLATED
            zi.external_attr = 0o644 << 16
            z.writestr(zi, data)

    # classicube.zip: satisfies Resources.c's separate File_Exists check.
    # Content is irrelevant here (only touch.png, mobile-only, is ever read
    # out of it - see CCTextures_SelectEntry), so an empty valid zip is
    # sufficient and honest: it is not standing in for real content.
    cc_zip_path = os.path.join(outdir, 'classicube.zip')
    with zipfile.ZipFile(cc_zip_path, 'w', zipfile.ZIP_DEFLATED) as z:
        pass

    return default_zip_path, cc_zip_path, list(entries.keys())


if __name__ == '__main__':
    outdir = sys.argv[1] if len(sys.argv) > 1 else '.'
    repo_root = sys.argv[2] if len(sys.argv) > 2 else '<workspace>'
    dz, cz, names = build(outdir, repo_root)
    print('wrote', dz, os.path.getsize(dz), 'bytes,', len(names), 'entries')
    print('wrote', cz, os.path.getsize(cz), 'bytes (stub)')
