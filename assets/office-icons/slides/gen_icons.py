#!/usr/bin/env python3
"""Generate the SLIDES toolbar icon SVG masters (original MayteraOS artwork).

Geometry rules (see README.md next to the output):
  viewBox 0 0 512 512, a 16x16 grid of 32-unit cells. Every line is 32 units
  wide and sits on a cell (centre at 32k+16, ends on multiples of 32, butt
  caps, mitre joins) so that draw_mico()'s nearest-sample 64->16 downscale
  hits exactly one full-coverage pixel per line. White on transparent.
"""
import os, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "slides"
os.makedirs(OUT, exist_ok=True)

HEAD = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" '
        'fill="none" stroke="#ffffff" stroke-width="32" '
        'stroke-linecap="butt" stroke-linejoin="miter">\n'
        '  <!-- MayteraOS office toolbar icon: original artwork, 32-unit grid, '
        'white on transparent -->\n')
TAIL = '</svg>\n'

def stroke(d, w=None):
    return '  <path d="%s"%s/>\n' % (d, (' stroke-width="%d"' % w) if w else '')

def fill(d):
    return '  <path d="%s" fill="#ffffff" stroke="none"/>\n' % d

def circle_fill(cx, cy, r):
    return '  <circle cx="%d" cy="%d" r="%d" fill="#ffffff" stroke="none"/>\n' % (cx, cy, r)

def circle_stroke(cx, cy, r):
    return '  <circle cx="%d" cy="%d" r="%d"/>\n' % (cx, cy, r)

def group(tx, ty, body):
    return '  <g transform="translate(%d %d)">\n' % (tx, ty) + \
           ''.join('  ' + l for l in body.splitlines(True)) + '  </g>\n'

# Shared landscape slide frame: cols 1..13, rows 3..11 (13x9 cells).
FRAME = stroke("M48 112H432V368H48Z")

icons = {}

# --- document-level -------------------------------------------------------
# new: portrait page with folded corner and a plus
icons["new"] = group(16, 0,
    stroke("M112 48H272L368 144V464H112Z") +
    stroke("M272 48V144H368") +
    stroke("M240 208V368M160 288H320"))

# open: folder with a left tab and an angled front panel
icons["open"] = group(0, -16,
    stroke("M368 240V176H240L208 144H48V400H400L464 240H112"))

# save: disk with chamfered corner, shutter slot on top, label at the bottom
icons["save"] = (
    stroke("M48 48H336L464 176V464H48Z") +
    stroke("M144 48V176H304V48") +
    stroke("M144 464V304H368V464"))

# --- slide management -----------------------------------------------------
icons["new-slide"] = group(16, 16, FRAME + stroke("M240 160V320M160 240H320"))

icons["duplicate-slide"] = group(16, 16,
    stroke("M368 160V112H48V304H96") +          # back slide, only where visible
    stroke("M112 176H432V368H112Z"))            # front slide

icons["delete-slide"] = group(16, 16, FRAME + stroke("M160 160L320 320M320 160L160 320"))

icons["slide-layout"] = group(16, 16, FRAME +
    fill("M96 160H384V192H96Z") +               # title placeholder
    fill("M96 224H224V320H96Z") +               # left content block
    fill("M256 224H384V320H256Z"))              # right content block

# --- insert ---------------------------------------------------------------
# text-box: four selection-handle corners around a T
icons["text-box"] = (
    stroke("M48 144V80H144") + stroke("M368 80H464V144") +
    stroke("M464 368V432H368") + stroke("M144 432H48V368") +
    stroke("M176 176H336M256 176V352"))

# insert-image: framed picture, filled mountains and sun
icons["insert-image"] = (
    stroke("M48 80H464V432H48Z") +
    fill("M64 416L176 272L240 352L304 288L448 416Z") +
    circle_fill(352, 160, 48))

# insert-shape: overlapping square and circle
icons["insert-shape"] = (
    stroke("M48 48H304V304H48Z") +
    circle_stroke(336, 336, 128))

# --- text formatting ------------------------------------------------------
icons["bold"] = (
    stroke("M160 80V400", 56) +
    stroke("M160 80H280A72 72 0 0 1 280 224H160", 56) +
    stroke("M280 224H304A88 88 0 0 1 304 400H160", 56))

icons["italic"] = stroke("M208 80H400M112 432H304M320 80L192 432")

icons["align-left"]   = group(0, 32, stroke("M32 80H480M32 176H320M32 272H480M32 368H320"))
icons["align-center"] = group(0, 32, stroke("M32 80H480M96 176H416M32 272H480M96 368H416"))
icons["align-right"]  = group(0, 32, stroke("M32 80H480M192 176H480M32 272H480M192 368H480"))

icons["bullet-list"] = (
    circle_fill(80, 96, 40) + circle_fill(80, 256, 40) + circle_fill(80, 416, 40) +
    stroke("M176 96H464M176 256H464M176 416H464"))

# --- navigation / presenting ---------------------------------------------
icons["prev-slide"] = group(16, 16, FRAME + stroke("M304 144L208 240L304 336"))
icons["next-slide"] = group(16, 16, FRAME + stroke("M208 144L304 240L208 336"))

# present-play: projection screen on a stand with a solid play triangle
icons["present-play"] = group(16, 16,
    stroke("M48 80H432V336H48Z") +
    fill("M192 128L320 208L192 288Z") +
    stroke("M240 352V384") +
    stroke("M128 400H352"))

# notes: slide thumbnail above three lines of speaker notes
icons["notes"] = group(16, 32,
    stroke("M112 48H368V208H112Z") +
    stroke("M96 272H416M96 336H416M96 400H288"))

ORDER = ["new", "open", "save", "new-slide", "duplicate-slide", "delete-slide",
         "slide-layout", "text-box", "insert-image", "insert-shape", "bold",
         "italic", "align-left", "align-center", "align-right", "bullet-list",
         "prev-slide", "next-slide", "present-play", "notes"]
assert sorted(ORDER) == sorted(icons), set(ORDER) ^ set(icons)

for name in ORDER:
    with open(os.path.join(OUT, name + ".svg"), "w") as f:
        f.write(HEAD + icons[name] + TAIL)
print("wrote %d SVGs to %s" % (len(ORDER), OUT))
