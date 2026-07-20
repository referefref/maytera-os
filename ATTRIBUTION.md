# Third-Party Asset Attribution

## Icons (SVG Repo, Creative Commons Attribution)

MayteraOS desktop, Start menu, and system-tray icons are derived from SVG icons
obtained from **SVG Repo** (https://www.svgrepo.com). The icons used are
distributed under the **Creative Commons Attribution (CC BY)** license.

License reference: https://www.svgrepo.com/page/licensing/#CC%20Attribution

The SVGs were recolored white and rendered to the internal `.ICN` (MICO) icon
format used by the compositor; the source artwork is unmodified in form.

### Icons used

| Use | SVG Repo ID | Name |
|-----|-------------|------|
| Computer | 533134 | monitor-alt-4 |
| Browser | 443597 | browser-general |
| Recycle Bin | (custom) | hand-drawn lineart trash can - `assets/icons/recycle.svg`, not third-party |
| Terminal | 444669 | terminal |
| Settings | 527439 | settings |
| IRC | 529474 | chat-dots |
| Editor | 532786 | file-pencil-alt |
| Calculator | 533417 | calculator |
| Image Viewer | 532570 | image-pen |
| Audio Player | 527259 | music-library |
| Media Player | 532727 | video |
| Clock | 532125 | clock-two |
| Files | 532810 | folder |
| Network | 532872 | network-wired |

If any icon above is republished, retain attribution to SVG Repo and the
CC Attribution license linked above.

The DOOM launcher's icon is id Software's DOOM logo (used to launch the bundled
DOOM engine), not an SVG Repo icon; see the DOOM licensing note below.

## Desktop pet

The "sheep" desktop pet in MayteraOS is drawn procedurally from primitives by
the compositor; it does not embed third-party sprite artwork.

## Fonts

`FONT.TTF` on the boot image is **DejaVu Sans** (from the DejaVu fonts project,
derived from Bitstream Vera). Bitstream Vera is distributed under the Bitstream
Vera license (a permissive, redistributable license); DejaVu's own changes are
released into the public domain. Both permit redistribution.

## Vendored open-source libraries

MayteraOS adapts the following open-source projects. Each retains its own
license text in its source subdirectory (e.g. `COPYING`, `LICENSE`, `AUTHORS`):

| Component | Location | License | License file |
|-----------|----------|---------|--------------|
| libmad (MP3 decode) | `kernel/media/libmad` | GPLv2+ | per-file headers (add `COPYING`) |
| faad2 (AAC decode) | `kernel/media/faad2` | GPLv2 | `COPYING` + `AUTHORS` |
| Tremor / libogg (Vorbis) | `kernel/media/tremor` | BSD-style (Xiph) | `COPYING`(+`.libogg`) |
| Opus | `kernel/media/opus` | BSD-style (Xiph) | `COPYING` + `AUTHORS` |
| dr_flac | `kernel/media/dr_flac` | public domain / MIT-0 | `COPYING` |
| TinyGL | `userland/libgl` | zlib-style | `src/LICENSE` |
| GNU regex (grep port) | `userland/apps/grep-gnu/lib` | LGPLv3-or-later | per-file headers |
| MicroPython port glue | `userland/python/micropython/ports/maytera` | MIT (port glue only) | upstream |
| Mozilla CA bundle | `kernel/fs/CERTS/ca-bundle.crt` | MPL 2.0 | in-file note |
| Realtek 88x2bu register tables | `kernel/drivers/net/wifi` | GPL (register facts) | in-file note |
| Nova prompt-injection ruleset | `kernel/security/nova.c` | MIT | in-file note |

The AI layer's LLM prompt-injection protection uses the **Nova** open ruleset by
**Thomas Roccia** ([@fr0gger_](https://github.com/fr0gger/nova-framework)),
(c) 2025, MIT License. The keyword layer is adapted from Nova's
`llm01_promptinject`, `jailbreak` and `injection` rules; retain this credit if
you redistribute `kernel/security/nova.c`.

Because the kernel statically links GPLv2 components (libmad, faad2), the
combined MayteraOS binary is distributed under **GPLv2-or-later**. The
permissively-licensed components above remain under their own terms as source.

The in-house decoders (`jpeg.c`, `png.c`, `webp.c`, `wav.c`, `mpeg.c`) and the
archiver (`userland/libarchive`) are original MayteraOS code.

> **DOOM (id Software):** the DOOM engine source under `kernel/games/doom` (and
> the derived `d_*/i_*/r_*/p_*/w_*/z_*` files) is covered by id Software's own
> *DOOM Source Code License*, carried in every source-file header. It is a
> **separate license, not the GPL**, and permits non-commercial redistribution
> under id's terms. See `kernel/games/doom/DOOMLICENSE.md`. It is carved out of
> this repository's blanket GPLv2-or-later; MayteraOS's own code remains GPL.

## Photography (boot splash / wallpapers)

The boot-splash background (`kernel/boot.bmp`, `kernel/boot_splash.jpg`,
`kernel/video/boot_image_data.c`) and the bundled wallpapers are edited from
**Pexels** stock photography, used under the [Pexels License](https://www.pexels.com/license/)
(free for commercial and non-commercial use, no attribution required). The
MayteraOS lighthouse mark composited onto the splash is original artwork.

If you redistribute any component, retain its in-tree license file and this
attribution.
