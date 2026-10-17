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
#
# AN APP THAT DOES NOT BUILD STOPS THIS SCRIPT. See "the failure policy" below
# if you are wondering why your build now fails where it used to say
# "Build complete, WITH FAILURES" and exit 0.

set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

BUILD_ALL_APPS=0
DO_CLEAN=0
POLICY="$ROOT_DIR/build-apps.policy"

for arg in "$@"; do
    case "$arg" in
        --all-apps) BUILD_ALL_APPS=1 ;;
        --clean)    DO_CLEAN=1 ;;
        --help|-h)
            sed -n '2,23p' "$0"
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

# 1b. THE SHARED LIBRARIES, ALL OF THEM, BEFORE THE APP LOOP.
#
# This used to build libgl and nothing else, and the omission was the direct
# cause of two separate "the app is just not there" bugs:
#
#   - libgl itself was added after 3D apps failed with
#     "No rule to make target '../../libgl/libgl.a'".
#   - sdl12test failed in every clean tree because nothing here built
#     userland/libSDL, exactly as aichat, flowedit and flowrun failed because
#     nothing built the libyaml port. A shared library that only SOME consumer
#     happens to build is a build-order lottery, and it is won on a machine that
#     has built before and lost on a clean checkout, which is the one that
#     matters.
#
# The list is the SAME ONE build/build-golden.sh uses for the internal image
# (libc, libhelp, libgl, libSDL, libarchive), so the public build cannot be
# missing a library the internal build has. libc is step 1 above because
# everything depends on it; libarchive has no Makefile (its sources are compiled
# by the apps that use it), which is why the guard below is a guard and not an
# unconditional call.
#
# A library that FAILS here stops the build, deliberately and with no policy
# exemption available. One broken library makes every dependent app fail, and a
# list of twenty "failed" apps with one real cause is how the libyaml fault
# stayed unread for a day.
for _lib in libhelp libgl libSDL; do
    if [ -f "userland/$_lib/Makefile" ]; then
        run_make "userland/$_lib" "userland $_lib"
    else
        echo
        echo "=== Skipping userland/$_lib: no Makefile in this tree ==="
        SKIPPED_PORTS="${SKIPPED_PORTS:-} $_lib"
    fi
done

# 2. kernel
run_make kernel "kernel"

# 3. UEFI bootloader
run_make boot/uefi "UEFI bootloader"

# 4. DOOM (the headline userland app)
#
# GUARDED ON PURPOSE. This step used to be an unconditional
# `run_make userland/apps/doom`, and under `set -e` a missing directory made
# `make -C` exit 2 and took the WHOLE build down at step 4, before a single app
# was built. That is exactly what happened when a source sync dropped the port:
# both `./build.sh` and `./build.sh --all-apps` stopped here and the README's
# headline build command could not work.
#
# An optional port going missing must degrade to "skipped", never to "the build
# is dead". Same shape as the libgl guard at step 1b. Apply this to any port
# added below rather than adding another unconditional run_make.
#
# NOTE THE DIFFERENCE from an app that FAILS TO COMPILE, below. "The source is
# not in this tree" is a fact about the tree and is visible in a one-line
# listing. "The source is here and the compiler rejected it" is a broken build,
# and those are not the same event however similar the end state looks.
if [ -f userland/apps/doom/Makefile ]; then
    run_make userland/apps/doom "DOOM"
else
    echo
    echo "=== Skipping DOOM: userland/apps/doom/Makefile not present ==="
    SKIPPED_PORTS="${SKIPPED_PORTS:-} doom"
fi

# ---------------------------------------------------------------------------
# 5. (optional) every other userland app with a Makefile
#
# THE FAILURE POLICY, AND WHY IT IS NOT A SHRUG ANY MORE.
#
# This loop used to collect failures, print "Build complete, WITH FAILURES",
# and EXIT 0. On 2026-10-02 that shipped a public MayteraOS image with AI Chat
# and Maytera Flow simply absent: three apps needed a libyaml port that nothing
# built except a TEST app later in the alphabet, so on a machine that had built
# before they worked, and on a clean checkout they did not. The build said
# success. The only trace was one line in the boot log of a VM nobody had to
# boot. An app that silently does not exist is not a warning, it is a missing
# product.
#
# So a failing app now STOPS THIS SCRIPT, unless it is named in
# build-apps.policy with a CLASS and a WRITTEN REASON. That file is the only
# way through, and it is deliberately a little uncomfortable to add to:
#
#     <app-directory-name>  <CLASS>  <reason>      (TAB-separated)
#
#     TESTONLY  a harness or proof-of-life app that is never installed on an
#               image, so its failure costs a user nothing.
#     DEBT      it SHOULD build and does not. The reason is printed on every
#               build so the debt stays visible, and the entry goes STALE (and
#               fails) the moment the app builds again, so a fixed problem
#               cannot keep being apologised for.
#
# A policy line for an app directory that no longer exists also fails. The file
# is meant to shrink.
# ---------------------------------------------------------------------------
FAILED_APPS=""
BUILT_APPS=""
if [ $BUILD_ALL_APPS -eq 1 ]; then
    for app in userland/apps/*/; do
        if [ -f "${app}Makefile" ] && [ "${app}" != "userland/apps/doom/" ]; then
            name="$(basename "${app%/}")"
            if ! run_make "${app%/}" "app: ${name}"; then
                echo "!!! app '${name}' FAILED to build"
                FAILED_APPS="${FAILED_APPS} ${name}"
            else
                BUILT_APPS="${BUILT_APPS} ${name}"
            fi
        fi
    done
fi

# ---------------------------------------------------------------------------
# 5b. Judge the failures against the policy.
# ---------------------------------------------------------------------------
policy_verdict() {
    local fatal=0 excused=0 line name class reason app

    if [ -n "${FAILED_APPS:-}" ] && [ ! -f "$POLICY" ]; then
        echo
        echo "=== BUILD FAILED: app(s) did not build and there is no policy file ===" >&2
        echo "  apps that did not build:${FAILED_APPS}" >&2
        echo "  expected policy file: $POLICY" >&2
        return 1
    fi
    [ -f "$POLICY" ] || return 0

    # -- validate every policy line, and check it is not stale --------------
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue;; esac
        name=$(printf  '%s' "$line" | cut -f1)
        class=$(printf '%s' "$line" | cut -f2)
        reason=$(printf '%s' "$line" | cut -f3-)
        if [ -z "$name" ]; then
            echo "POLICY ERROR: line with no app name: $line" >&2; fatal=1; continue
        fi
        case "$class" in
            TESTONLY|DEBT) ;;
            *) echo "POLICY ERROR: '$name': unknown CLASS '$class' (TESTONLY or DEBT)" >&2; fatal=1; continue;;
        esac
        if [ -z "$(printf '%s' "$reason" | tr -d '[:space:]')" ]; then
            echo "POLICY ERROR: '$name' has NO REASON. An exemption without a written reason is a shrug with extra steps." >&2
            fatal=1; continue
        fi
        if [ ! -d "userland/apps/$name" ]; then
            echo "POLICY ERROR: STALE entry '$name': userland/apps/$name does not exist. Delete the line." >&2
            fatal=1; continue
        fi
        if [ "$class" = DEBT ] && [ $BUILD_ALL_APPS -eq 1 ]; then
            case " ${BUILT_APPS} " in
                *" $name "*)
                    echo "POLICY ERROR: STALE DEBT entry '$name': it BUILT this time, so the debt is paid. Delete the line." >&2
                    fatal=1; continue;;
            esac
        fi
    done < "$POLICY"

    # -- judge each failure -------------------------------------------------
    for app in ${FAILED_APPS:-}; do
        line=$(awk -F'\t' -v k="$app" '$1==k{print; exit}' "$POLICY")
        if [ -z "$line" ]; then
            echo "UNEXCUSED FAILURE: '$app' did not build and is not in $POLICY" >&2
            fatal=1
            continue
        fi
        class=$(printf '%s' "$line" | cut -f2)
        reason=$(printf '%s' "$line" | cut -f3-)
        echo "  tolerated ($class) ${app}: ${reason}"
        excused=$((excused + 1))
    done

    if [ "$fatal" -ne 0 ]; then
        echo >&2
        echo "=== BUILD FAILED ===" >&2
        echo "  An app that does not build is an app that is not on the disk, and" >&2
        echo "  a disk missing an app the Start menu offers is a feature that" >&2
        echo "  silently disappears. Either fix the build, or add a line to" >&2
        echo "  $POLICY naming the app, its CLASS and WHY." >&2
        return 1
    fi
    [ "$excused" -gt 0 ] && echo "  ($excused tolerated failure(s), each named in $POLICY)"
    return 0
}

echo
if [ -n "${SKIPPED_PORTS:-}" ]; then
    echo "=== Optional ports skipped (not present in this tree):${SKIPPED_PORTS} ==="
fi
if [ -n "${FAILED_APPS:-}" ]; then
    echo "=== apps that did not build:${FAILED_APPS} ==="
fi

if ! policy_verdict; then
    exit 1
fi

echo "=== Build complete ==="
echo "  kernel:     kernel/kernel.elf"
echo "  bootloader: boot/uefi/BOOTX64.EFI"
if [ -f userland/apps/doom/DOOM.ELF ]; then
    echo "  DOOM:       userland/apps/doom/DOOM.ELF"
fi
echo
echo "Assemble a bootable FAT32 image with: ./stage-disk.sh"
