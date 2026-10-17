# Publishing the Angband App Store package

Angband is distributed as a signed App Store package, NOT baked into the golden
image, for the reason the owner stated: the games "are meant to be served by the
update server / app repo". Both the binary and its 1.4 MB lib data come from here.

## Why there was no package until #gamedata2

MEASURED against the shipping `pkgdest_confine()`: `/ANGBAND/...` is REFUSED
("destination is not an installable directory"), because `userland/libc/pkgdest.c`
permits only `/APPS`, `/GAMES` and `/THEMES`. The port compiled its
`DEFAULT_CONFIG_PATH`/`DEFAULT_LIB_PATH` as `/ANGBAND/`, so the data had to be
baked in. It is now `/GAMES/ANGBAND/lib/`, a `lib/` subdirectory of the writable
save root this port already owned, which keeps the installed payload and the
player's save tree separable rather than intermixed.

## What is in this directory

- `INSTALL` - one TREE rule for the lib data and one EXACT rule for the binary.
  Declares `#!mpkg-min-client 2`.
- `manifest-entry.json` - the `manifest.json` `"packages"` entry.
- `build-package.sh` - packs the mports lib data tree plus the ELF. It REFUSES to
  pack a binary that does not contain the string `/GAMES/ANGBAND/lib/`, because a
  binary predating the data-root move would not look where this package installs.
  Takes no build slot.

## Rebuild recipe

1. `buildq run <agent-id> angband -- pct exec <ct> -- bash -lc 'cd <repo>/userland/ports && bash mports.sh build angband'`
   then build `userland/apps/angband`. A golden build does both, and
   `build-package.sh` prefers the golden build tree's copies.
2. `bash userland/ports/angband/appstore-package/build-package.sh`

## Publish steps

See `userland/apps/openarena/appstore-package/APPSTORE-PUBLISH.md`. Do not keep a
second copy of that list in sync by hand.

## Licence

GPL-2.0-only. Upstream ships no standalone COPYING file; every source file
carries a dual grant and we redistribute under option (a), the GPL. The PORT file
plus `patches/` plus the exact upstream tarball are the corresponding source, as
recorded in ATTRIBUTION.md. The lib data is part of the same GPL work, so unlike
AssaultCube's content there is no separate asset licence to track.
