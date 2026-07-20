# Disk template

Files that get copied to the root of the FAT32 boot partition alongside
the kernel and bootloader. The kernel reads these at runtime through the
FAT32 driver (see `kernel/fs/fat.c`).

## Contents

```
disk/
├── CONFIG/
│   └── MENU.CFG            # file-browser / menu defaults
└── THEMES/
    ├── classic/theme.ini   # Windows 95/98 style
    ├── dark/theme.ini      # dark theme
    ├── highcontrast/theme.ini
    ├── light/theme.ini
    └── retro-unix/         # default theme: CDE/Motif/NeXTSTEP inspired
        ├── colors/
        ├── cursors/
        ├── icons/
        ├── README.md
        └── theme.ini
```

## What is intentionally NOT here

- **Wallpapers (`*.BMP`).** The production disk carries 50+ BMP wallpapers
  at the root of the FAT partition. They are not committed here because
  they are large binary assets. The kernel's wallpaper picker
  (`kernel/gui/desktop.c`) silently falls back to a gradient when a
  wallpaper filename is not found, so the desktop still boots fine
  without them.
- **The `retro-unix/WALLPAPERS/` subdirectory.** Same reasoning. The
  retro-unix theme works without its two 6 MiB reference wallpapers.
- **`/EFI/BOOT/BOOTX64.EFI`.** Build it from `boot/uefi/`.
- **`/APPS/` and `/GAMES/`.** Build the user-mode apps and DOOM from
  `userland/`, then stage them into `APPS/` and `GAMES/DOOM/` when you
  assemble the boot partition.
- **`DOOM1.WAD` / `DOOM.WAD`.** Not GPL, not in this repository. Supply
  your own; see `userland/apps/doom/COPYING`.

## Staging the boot partition

Building MayteraOS gives you four sets of outputs:

| Source                   | Output                        | Destination on the FAT32 partition |
|--------------------------|-------------------------------|------------------------------------|
| `boot/uefi/`             | `BOOTX64.EFI`                 | `/EFI/BOOT/BOOTX64.EFI`            |
| `kernel/`                | `kernel.elf`                  | `/boot/kernel.elf`                 |
| `userland/apps/*/`       | `*.ELF`                       | `/APPS/`                           |
| `userland/apps/doom/`    | `DOOM.ELF` (+ your own WAD)   | `/GAMES/DOOM/`                     |
| `disk/`                  | `CONFIG/`, `THEMES/`          | `/CONFIG/`, `/THEMES/`             |

See the top-level `README.md` for a full walk-through.
