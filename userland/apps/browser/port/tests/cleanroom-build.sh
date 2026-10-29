#!/bin/bash
# cleanroom.sh - PROVE that a clean checkout with NO access to the out-of-repo
# engine trees can fetch them from the update server and build the browser.
#
# A test that passes while <workspace>{netsurf-port,duktape} are still sitting
# there proves nothing at all: the build could be reading them and we would never
# know. So this runs inside a PRIVATE MOUNT NAMESPACE with an empty tmpfs mounted
# over <workspace> The live trees then do not exist for this process tree, and
# cannot exist, while remaining completely untouched for every other agent on the
# box. That is also why this does not rename them: renaming the shared tree is
# the race that cost two goldens in September 2026.
#
# ARM A runs FIRST and MUST FAIL. If the browser builds with the trees hidden and
# nothing fetched, then the deps were never really needed and arm B would be
# measuring nothing.
# ARM B fetches from the update server and MUST build the browser ELF.
set -u

# HOW TO RUN IT (the namespace is not optional, it is the proof):
#
#   ROOT=/root/cleanroom-browserdeps
#   rm -rf $ROOT; mkdir -p $ROOT/src
#   git -C <repo> archive <ref> | tar -x -C $ROOT/src
#   cp $ROOT/src/userland/apps/browser/port/tests/cleanroom-build.sh /root/
#   unshare -m bash -c "mount -t tmpfs tmpfs <workspace> && exec /root/cleanroom-build.sh"
#
# The copy out of $ROOT/src is so the script itself is not inside the tree it is
# about to `make clean` in, and the tmpfs is what makes the live trees genuinely
# unreachable rather than merely unused.
CR_ROOT=${CR_ROOT:-/root/cleanroom-browserdeps}
SRC=${CR_SRC:-$CR_ROOT/src}
DEPS=${CR_DEPS:-$CR_ROOT/deps}
CACHE=${CR_CACHE:-$CR_ROOT/cache}
LOG=${CR_LOG:-$CR_ROOT/log}
mkdir -p "$LOG"

banner() { printf '\n========== %s ==========\n' "$*"; }

banner "ENVIRONMENT"
echo "live trees as seen from inside this namespace:"
ls -la <workspace> 2>&1 | head -5
for d in <workspace> <workspace>; do
    if [ -e "$d" ]; then echo "  PRESENT: $d  <-- the proof is INVALID, abort"; exit 2
    else echo "  ABSENT:  $d"; fi
done
echo "checkout under test: $SRC (a git archive; it has no .git)"
test -d "$SRC/.git" && { echo "  $SRC is a git repo, expected a bare export"; exit 2; }

# ---------------------------------------------------------------- shared libc --
banner "PREREQ: build the MayteraOS libc in the clean checkout"
( cd "$SRC/userland/libc" && make clean >/dev/null 2>&1; make ) > "$LOG/libc.log" 2>&1
rc=$?
if [ "$rc" != 0 ] || [ ! -s "$SRC/userland/libc/libc.a" ]; then
    echo "libc did not build (exit $rc); the proof cannot proceed"
    tail -20 "$LOG/libc.log"; exit 2
fi
echo "libc.a built: $(stat -c %s "$SRC/userland/libc/libc.a") bytes"

# --------------------------------------------------------------------- ARM A ---
banner "ARM A: build the browser with NOTHING fetched (must FAIL)"
# Wipe the staging dirs FIRST. A previous run of this script leaves a fully
# fetched and built engine in $DEPS, and arm A points the compiler at it, so a
# re-run without this would have arm A succeed and the proof would be worthless.
# It was caught by the arm-A guard below on the second run of this script rather
# than by foresight, which is the argument for having the guard.
rm -rf "$DEPS" "$CACHE"
mkdir -p "$DEPS/netsurf-port" "$DEPS/duktape" "$CACHE"
echo "staging dirs reset: $(find "$DEPS" -type f | wc -l) files under $DEPS, $(find "$CACHE" -type f | wc -l) in the download cache"
( cd "$SRC/userland/apps/browser" && make clean >/dev/null 2>&1
  MAYTERA_NS_ROOT="$DEPS/netsurf-port" MAYTERA_DUK_ROOT="$DEPS/duktape" make ) \
    > "$LOG/armA.log" 2>&1
A_RC=$?
A_ELF=0
[ -s "$SRC/userland/apps/browser/browser" ] && A_ELF=1
echo "arm A: make exit=$A_RC  browser-binary-present=$A_ELF"
echo "arm A: first error lines:"
grep -m5 -E "No such file|error:|Error|cannot find" "$LOG/armA.log" | sed 's/^/    /'
if [ "$A_RC" = 0 ] && [ "$A_ELF" = 1 ]; then
    echo "ARM A UNEXPECTEDLY SUCCEEDED. The browser built with no engine source"
    echo "present, so this whole proof measures nothing. Investigate before"
    echo "believing arm B."
    exit 1
fi
echo "ARM A FAILED AS REQUIRED: without the engine source the browser cannot build."

# --------------------------------------------------------------------- ARM B ---
banner "ARM B: fetch from the update server, then build (must SUCCEED)"
rm -rf "$DEPS" "$CACHE"; mkdir -p "$DEPS" "$CACHE"
MAYTERA_DEPS_CACHE="$CACHE" \
  bash "$SRC/userland/apps/browser/port/fetch-upstream.sh" "$DEPS" > "$LOG/fetch.log" 2>&1
F_RC=$?
echo "fetch exit=$F_RC"
sed 's/^/    /' "$LOG/fetch.log"
[ "$F_RC" = 0 ] || { echo "ARM B: fetch failed"; exit 1; }
echo "bytes that arrived over the network:"
ls -la "$CACHE" | sed 's/^/    /'

banner "ARM B: compile the five NetSurf libraries from that source"
( cd "$DEPS/netsurf-port" && MAYTERA_NS_ROOT="$DEPS/netsurf-port" bash ./build-all.sh ) \
    > "$LOG/libbuild.log" 2>&1
L_RC=$?
NSFAIL=$(grep -c '^  FAIL ' "$LOG/libbuild.log")
echo "build-all.sh exit=$L_RC  per-file FAIL lines=$NSFAIL"
grep -E '^(lib|###|/root.*: OK=)' "$LOG/libbuild.log" | grep 'OK=' | sed 's/^/    /'
for l in libwapcaplet libparserutils libhubbub libcss libdom; do
    if [ -s "$DEPS/netsurf-port/$l.a" ]; then
        printf '    %-16s %8s bytes  %s members\n' "$l.a" \
            "$(stat -c %s "$DEPS/netsurf-port/$l.a")" "$(ar t "$DEPS/netsurf-port/$l.a" | wc -l)"
    else
        echo "    $l.a MISSING"; L_RC=1
    fi
done
[ "$L_RC" = 0 ] || { echo "ARM B: library build failed"; tail -30 "$LOG/libbuild.log"; exit 1; }

banner "ARM B: build the browser against the fetched engine"
( cd "$SRC/userland/apps/browser" && make clean >/dev/null 2>&1
  MAYTERA_NS_ROOT="$DEPS/netsurf-port" MAYTERA_DUK_ROOT="$DEPS/duktape" \
  BUILDQ_JOB="${BUILDQ_JOB:-cleanroom}" MAYTERA_REPO="$SRC" make ) \
    > "$LOG/armB.log" 2>&1
B_RC=$?
B_ELF=0
[ -s "$SRC/userland/apps/browser/browser" ] && B_ELF=1
echo "arm B: make exit=$B_RC  browser-binary-present=$B_ELF"
if [ "$B_RC" != 0 ] || [ "$B_ELF" != 1 ]; then
    echo "ARM B FAILED. Last 40 lines:"
    tail -40 "$LOG/armB.log" | sed 's/^/    /'
    exit 1
fi
echo "browser ELF: $(stat -c %s "$SRC/userland/apps/browser/browser") bytes"
file "$SRC/userland/apps/browser/browser" 2>/dev/null | sed 's/^/    /'
echo "duktape objects it linked:"
for o in duktape.o duk_support.o duk_dom.o; do
    [ -s "$DEPS/duktape/$o" ] && printf '    %-16s %8s bytes  md5 %s\n' "$o" \
        "$(stat -c %s "$DEPS/duktape/$o")" "$(md5sum "$DEPS/duktape/$o" | cut -d' ' -f1)"
done
echo "licence texts present in the staged trees:"
for f in netsurf-port/libwapcaplet/COPYING netsurf-port/libparserutils/COPYING \
         netsurf-port/libhubbub/COPYING netsurf-port/libcss/COPYING \
         netsurf-port/libdom/COPYING duktape/LICENSE.txt duktape/AUTHORS.rst; do
    [ -s "$DEPS/$f" ] && echo "    OK $f" || echo "    MISSING $f"
done
ls -1 "$DEPS/duktape/licenses" 2>/dev/null | sed 's/^/    licenses\//'

banner "RESULT"
echo "CLEANROOM_PROOF=PASS"
echo "arm A (no deps, trees hidden)   : browser build FAILED, as required"
echo "arm B (fetched from the build container)     : browser ELF built"
exit 0
