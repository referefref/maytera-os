#!/bin/bash
#
# build.sh - Top-level MayteraOS build orchestrator
#
# Builds every component needed to assemble a bootable MayteraOS system:
#
#   1. userland/libc         -> crt0.o + libc.a
#   2. kernel                -> kernel.elf
#   3. boot/uefi             -> BOOTX64.EFI
#   4. userland/apps/doom    -> DOOM.ELF   (and other userland apps on request)
#
# Usage:
#   ./build.sh                 # build libc, kernel, bootloader, doom (minimum)
#   ./build.sh --all-apps      # also build every userland app under userland/apps/
#   ./build.sh --clean         # make clean in every component, then build
#   ./build.sh --help
#
# After building, see stage-disk.sh to assemble a bootable FAT32 image.

set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

BUILD_ALL_APPS=0
DO_CLEAN=0

for arg in "$@"; do
    case "$arg" in
        --all-apps) BUILD_ALL_APPS=1 ;;
        --clean)    DO_CLEAN=1 ;;
        --help|-h)
            sed -n '2,19p' "$0"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg" >&2
            exit 1
            ;;
    esac
done

run_make() {
    local dir="$1"
    local label="$2"
    echo
    echo "=== Building ${label} (${dir}) ==="
    if [ $DO_CLEAN -eq 1 ]; then
        make -C "$dir" clean || true
    fi
    make -C "$dir"
}

# 1. libc (must come first; apps and doom depend on crt0.o and libc.a)
run_make userland/libc "userland libc"

# 1b. libgl (TinyGL). Apps that render 3D link ../../libgl/libgl.a, so this
#     must be built before the app loop or those apps fail with
#     "No rule to make target '../../libgl/libgl.a'".
if [ -f userland/libgl/Makefile ]; then
    run_make userland/libgl "userland libgl (TinyGL)"
fi

# 2. kernel
run_make kernel "kernel"

# 3. UEFI bootloader
run_make boot/uefi "UEFI bootloader"

# 4. DOOM (the headline userland app)
run_make userland/apps/doom "DOOM"

# 5. (optional) every other userland app with a Makefile
# One optional app must not abort the whole build: some apps need extra
# toolchains (e.g. a pinned rustc for the Rust apps) that not every builder
# has. Record failures and report them at the end instead of exiting.
FAILED_APPS=""
if [ $BUILD_ALL_APPS -eq 1 ]; then
    for app in userland/apps/*/; do
        if [ -f "${app}Makefile" ] && [ "${app}" != "userland/apps/doom/" ]; then
            name="$(basename "${app%/}")"
            if ! run_make "${app%/}" "app: ${name}"; then
                echo "!!! app '${name}' FAILED to build, continuing"
                FAILED_APPS="${FAILED_APPS} ${name}"
            fi
        fi
    done
fi

echo
if [ -n "${FAILED_APPS:-}" ]; then
    echo "=== Build complete, WITH FAILURES ==="
    echo "  apps that did not build:${FAILED_APPS}"
    echo "  (these are optional; the kernel, bootloader and remaining apps are usable)"
else
    echo "=== Build complete ==="
fi
echo "  kernel:     kernel/kernel.elf"
echo "  bootloader: boot/uefi/BOOTX64.EFI"
echo "  DOOM:       userland/apps/doom/DOOM.ELF"
echo
echo "Assemble a bootable FAT32 image with: ./stage-disk.sh"
