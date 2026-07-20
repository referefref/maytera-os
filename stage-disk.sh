#!/bin/bash
#
# stage-disk.sh - Assemble a bootable MayteraOS FAT32 disk image
#
# Takes the outputs of build.sh and lays them out on a GPT disk with a
# single FAT32 EFI System Partition in the shape the firmware + bootloader
# expect:
#
#   /EFI/BOOT/BOOTX64.EFI      <- boot/uefi/BOOTX64.EFI
#   /boot/kernel.elf           <- kernel/kernel.elf
#   /APPS/                     <- every built userland app ELF
#   /GAMES/DOOM/DOOM.ELF       <- userland/apps/doom/DOOM.ELF
#   /GAMES/DOOM/DOOM1.WAD      <- YOUR copy, if $DOOM_WAD is set
#   /CONFIG/MENU.CFG           <- disk/CONFIG/MENU.CFG
#   /THEMES/                   <- disk/THEMES/*
#   /BOOT.BMP, /BACK.BMP, ...  <- $WALLPAPERS_DIR/*.BMP (optional)
#
# Wallpapers are not shipped in the repo (they're large binary assets).
# Set WALLPAPERS_DIR=/path/to/bmps to have this script copy them into the
# FAT32 root. See disk/WALLPAPERS/README.md for the list of filenames the
# kernel looks for.
#
# Requires root (mount/losetup). Usage:
#
#   sudo ./stage-disk.sh                        # writes ./boot_disk.img (256 MiB)
#   sudo IMG=/path/to/out.img SIZE_MB=512 ./stage-disk.sh
#   sudo DOOM_WAD=/path/to/DOOM1.WAD ./stage-disk.sh
#   sudo WALLPAPERS_DIR=/path/to/bmps ./stage-disk.sh
#
# Boot with:
#
#   qemu-system-x86_64 -machine pc,accel=kvm -cpu host -m 2G \
#       -bios /usr/share/OVMF/OVMF_CODE.fd \
#       -drive file=boot_disk.img,format=raw,if=ide \
#       -serial stdio

set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

IMG="${IMG:-${ROOT_DIR}/boot_disk.img}"
SIZE_MB="${SIZE_MB:-256}"
MNT="${MNT:-${ROOT_DIR}/.stage-mnt}"

BOOTX64="${ROOT_DIR}/boot/uefi/BOOTX64.EFI"
KERNEL_ELF="${ROOT_DIR}/kernel/kernel.elf"
DOOM_ELF="${ROOT_DIR}/userland/apps/doom/DOOM.ELF"
DISK_TEMPLATE="${ROOT_DIR}/disk"

if [ "$(id -u)" -ne 0 ]; then
    echo "error: stage-disk.sh needs root (loop mount + losetup)" >&2
    exit 1
fi

for dep in parted mkfs.fat losetup mount umount; do
    if ! command -v "$dep" >/dev/null 2>&1; then
        echo "error: missing required tool: $dep" >&2
        exit 1
    fi
done

if [ ! -f "$BOOTX64" ]; then
    echo "error: $BOOTX64 not found. Run ./build.sh first." >&2
    exit 1
fi
if [ ! -f "$KERNEL_ELF" ]; then
    echo "error: $KERNEL_ELF not found. Run ./build.sh first." >&2
    exit 1
fi

echo "Creating ${SIZE_MB} MiB disk image at ${IMG}..."
rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$SIZE_MB" status=none
parted -s "$IMG" mklabel gpt
parted -s "$IMG" mkpart ESP fat32 1MiB 100%
parted -s "$IMG" set 1 esp on

LOOP=$(losetup -fP --show "$IMG")
trap 'umount "$MNT" 2>/dev/null || true; losetup -d "$LOOP" 2>/dev/null || true; rmdir "$MNT" 2>/dev/null || true' EXIT

mkfs.fat -F32 "${LOOP}p1" >/dev/null

mkdir -p "$MNT"
mount "${LOOP}p1" "$MNT"

echo "Installing bootloader + kernel..."
mkdir -p "$MNT/EFI/BOOT" "$MNT/boot"
cp "$BOOTX64"   "$MNT/EFI/BOOT/BOOTX64.EFI"
cp "$KERNEL_ELF" "$MNT/boot/kernel.elf"

echo "Installing disk template (CONFIG, THEMES)..."
if [ -d "${DISK_TEMPLATE}/CONFIG" ]; then
    cp -r "${DISK_TEMPLATE}/CONFIG" "$MNT/"
fi
if [ -d "${DISK_TEMPLATE}/THEMES" ]; then
    cp -r "${DISK_TEMPLATE}/THEMES" "$MNT/"
fi

echo "Installing userland apps..."
mkdir -p "$MNT/APPS"
shopt -s nullglob
for app_dir in "${ROOT_DIR}"/userland/apps/*/; do
    name="$(basename "$app_dir")"
    [ "$name" = "doom" ] && continue
    # Derive the install name from the BINARY, never from the directory.
    # An app's Makefile TARGET often differs from its directory name, and
    # the kernel spawns the binary name: userland/apps/compositor/ builds
    # COMPOSIT, which the kernel launches as /APPS/COMPOSIT. Matching on
    # the directory name silently skipped it and the desktop never started.
    # So: install every ELF *executable* in the app dir, whatever it is called.
    for candidate in "${app_dir}"*; do
        [ -f "$candidate" ] || continue
        case "$candidate" in
            *.o|*.a|*.c|*.h|*.rs|*.asm|*.ld|*.md|*.yml|*.yaml|*.txt|*.TXT|*Makefile) continue ;;
        esac
        # ELF magic + type 2 (ET_EXEC); skip relocatables and shared objects.
        read -r -n 18 magic < "$candidate" 2>/dev/null || true
        if head -c 4 "$candidate" 2>/dev/null | grep -qP '^\x7fELF' \
           && [ "$(od -An -tu1 -j16 -N1 "$candidate" 2>/dev/null | tr -d ' ')" = "2" ]; then
            cp "$candidate" "$MNT/APPS/"
            echo "  APPS/$(basename "$candidate")"
        fi
    done
done

echo "Installing DOOM..."
mkdir -p "$MNT/GAMES/DOOM"
if [ -f "$DOOM_ELF" ]; then
    cp "$DOOM_ELF" "$MNT/GAMES/DOOM/DOOM.ELF"
else
    echo "  (no DOOM.ELF yet; run: make -C userland/apps/doom)"
fi
if [ -n "${DOOM_WAD:-}" ] && [ -f "$DOOM_WAD" ]; then
    cp "$DOOM_WAD" "$MNT/GAMES/DOOM/$(basename "$DOOM_WAD")"
    echo "  GAMES/DOOM/$(basename "$DOOM_WAD")"
else
    echo "  (no WAD copied. Set DOOM_WAD=/path/to/DOOM1.WAD to include one.)"
fi

echo "Installing wallpapers..."
if [ -n "${WALLPAPERS_DIR:-}" ] && [ -d "$WALLPAPERS_DIR" ]; then
    count=0
    shopt -s nullglob nocaseglob
    for bmp in "$WALLPAPERS_DIR"/*.bmp; do
        cp "$bmp" "$MNT/$(basename "$bmp" | tr '[:lower:]' '[:upper:]')"
        count=$((count + 1))
    done
    shopt -u nocaseglob
    echo "  copied $count wallpaper BMP(s) from $WALLPAPERS_DIR"
    if [ "$count" -eq 0 ]; then
        echo "  (WALLPAPERS_DIR set but no .bmp files found)"
    fi
else
    echo "  (no wallpapers copied. Set WALLPAPERS_DIR=/path/to/bmps to include them."
    echo "   See disk/WALLPAPERS/README.md for the expected filenames.)"
fi

sync
umount "$MNT"
losetup -d "$LOOP"
rmdir "$MNT"
trap - EXIT

echo
echo "Done. Bootable image: $IMG"
echo
echo "Test with:"
echo "  qemu-system-x86_64 -machine pc,accel=kvm -cpu host -m 2G \\"
echo "      -bios /usr/share/OVMF/OVMF_CODE.fd \\"
echo "      -drive file=${IMG},format=raw,if=ide \\"
echo "      -serial stdio"
