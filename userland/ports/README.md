# userland/ports

The mports third-party library ports tree. **The documentation is
`docs/MPORTS.md`**; this file exists only so that someone who arrives here first
does not start inventing a second mechanism.

* `ports.mk` - the one definition of the userland cross-build flags.
* `mports.sh` - the one driver. `mports.sh --self-test` proves its refusals.
* `<name>/PORT` - a recipe, in pure key=value data. No per-port scripts.
* `<name>/patches/` - our deltas to upstream, as a readable patch series.
* `out/`, `.work/` - build output, gitignored. Not source.

Upstream source is never committed and never edited in place: the tarball is
fetched at build time and refused unless its sha256 equals the pin in the
recipe.
