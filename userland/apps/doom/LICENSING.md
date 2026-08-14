# DOOM port: licensing and provenance

## What is here

`userland/apps/doom` is the MayteraOS **source port** of the id Software DOOM
engine. It contains engine source only: 63 `.c` files, 70 `.h` files and one
`Makefile`.

**No game data ships here, and none ever will.** There is no `.wad`, no `.lmp`
and no extracted asset in this directory. The engine is useless without game
data, which is separately copyrighted and is not ours to redistribute. Supply
your own:

```sh
sudo DOOM_WAD=/path/to/DOOM1.WAD ./stage-disk.sh
```

The MayteraOS-specific platform layer is `i_maytera.c`, `i_maytera.h`,
`i_video.c`, `i_sound.c`, `i_system.c`, `i_net.c` and `doom_stubs.c`. Those are
first-party glue against the MayteraOS libc and compositor. Everything else is
upstream engine source with local build fixes.

## Licence

The engine source is covered by the **GNU General Public License, version 2**.
The authoritative text is in `LICENSE.TXT` beside this file.

`LICENSE.TXT` was **fetched verbatim from id Software's own source release**
and committed unmodified. It was not retyped and not reconstructed from memory:

| | |
|---|---|
| Upstream | `https://github.com/id-Software/DOOM`, path `LICENSE.TXT` |
| Git blob sha1 | `d60c31a97a544b53039088d14fe9114583c0efc3` |
| sha256 | `32b1062f7da84967e7019d01ab805935caa7ab7321a7ced0e30ebe75e5df1670` |
| Size | 17992 bytes |
| Retrieved | 2026-08-14 |

Re-verify at any time with `git hash-object LICENSE.TXT`, which must print the
blob sha1 above.

## Read this before citing the per-file headers

Every engine file carries a 1997-vintage id Software header that says the
source is available "only under the terms of the DOOM Source Code License".
**Those headers are stale upstream, and they are not the controlling licence.**

id Software released the DOOM source in December 1997 under the non-commercial
DOOM Source Code License, then **relicensed it under the GPL v2 in 1999**. id
updated the licence document in its own repository and never went back to
rewrite the 121 per-file banners. That is why the upstream release ships a
GPLv2 `LICENSE.TXT` next to source files that still name the older licence, and
it is the same basis on which every mainstream DOOM source port
(Chocolate Doom, PrBoom, Crispy Doom and the rest) is distributed under GPLv2.

An earlier revision of this project's `ATTRIBUTION.md` recorded DOOM as being
under "id Software DOOM Source Code License (NOT the GPL)" with "no licence
document in the tree". That entry read the per-file headers and did not read
the upstream licence document. It has been corrected.

GPLv2 is the same licence MayteraOS itself uses, so this port raises no
compatibility question against the rest of the repository.

## Scope of this note

This is a **notice and provenance analysis** based on reading the upstream
licence document and the per-file headers, in the same spirit as the rest of
`ATTRIBUTION.md`. It is not legal advice.
