# build/assets/startmenu/system.d/

The Start menu's all-users ("system") config layer, git-tracked so a build
reproduces the default menu byte-for-byte. `build/build-golden.sh` overlays
every `*.MENU` file here onto the golden's ext2 root at
`/CONFIG/STARTMENU/SYSTEM.D/`, where
`userland/apps/compositor/startmenu.c` reads it (via
`userland/apps/compositor/startmenu_model.rs`, the Rust content model) on
every boot and on a live throttled poll.

Filenames are numbered (`00-`, `01-`, ...) because fragments in a directory
are loaded in filename-sorted order, and that order is also the menu's
category display order (first occurrence wins position; see
`startmenu_model.rs`'s header comment for the full merge contract). The
numbers are a convenience for reading/editing this directory - they are not
otherwise significant, and an installed app's fragment (written at install
time by `userland/libc/startmenu_reg.c`, not shipped from here) has no such
prefix.

There is no compiled-in fallback if these files are missing, empty, or fail
to parse: the Start menu comes up with whatever the system layer (this
directory) plus the per-user layer (`/CONFIG/STARTMENU/USER/<user>/`, never
shipped from the repo) produce, and nothing else. See
`userland/apps/compositor/startmenu_model.rs` for why that is the point, not
a bug.
