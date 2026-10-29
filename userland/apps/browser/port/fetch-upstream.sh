#!/bin/bash
# fetch-upstream.sh - stage the browser engine's third-party trees on a machine
# that does not have them.
#
# WHAT THE BROWSER NEEDS AND WHERE IT COMES FROM
#
# The browser links five NetSurf CORE LIBRARIES and the Duktape JavaScript
# engine. All six are PRISTINE upstream: no MayteraOS change is made inside any
# of them. Our glue sits on top and IS in this repo, next to this script; our
# changes to upstream are committed patches in patches/, applied here. So this
# script fetches third-party source, and nothing of ours comes over the network.
#
# WHY IT FETCHES FROM OUR OWN UPDATE SERVER BY DEFAULT
#
# Until 2026-10-20 those two trees existed in exactly ONE place in the world,
# the build container:<workspace>{netsurf-port,duktape}, outside version control. The
# golden built because that one machine happened to hold 59 MB that nothing
# tracked; rebuild it and the browser becomes unbuildable, silently. Nobody,
# including us, could build the browser from a clean checkout. This script
# existed, but it cloned from github.com and downloaded from duktape.org, so it
# was also a bet that six third-party hosts stay up and keep serving the same
# bytes forever. Neither property is one we control.
#
# The sources are therefore MIRRORED on the MayteraOS update server (the build container,
# http://192.0.2.1 on the LAN, https://updates.maytera.net externally), which
# already serves the App Store. They are small: 1.5 MB for all six, because the
# 59 MB in the live trees is mostly .git objects and build output, not source.
#
# THE NETSURF BUNDLES ARE SOURCE ONLY, AND THAT IS A LICENCE REQUIREMENT, not
# tidiness. The test trees of these repositories are not redistributable: see the
# measured list in deps-publish.sh (a NonCommercial CSS fixture, nine
# all-rights-reserved web pages saved as crash cases, and 13 MB of W3C-licensed
# test suite with no licence text). MayteraOS compiles none of it. What the
# bundles DO carry is every lib's COPYING, asserted below on extraction.
#
# EVERY BUNDLE IS PINNED BY CONTENT HASH. A fetch that can silently return
# something different would be WORSE than the state this replaces, which at
# least failed loudly. upstream-pins.tsv carries the sha256 of each bundle, it
# is checked after download and BEFORE extraction, and a mismatch aborts. The
# pins also mean the plain-HTTP LAN URL is fine for integrity: a tampered or
# truncated body cannot get past the hash.
#
# The upstream URLs stay in the table and `--from-upstream` still uses them, so
# the mirror is never the only account of where this code came from, and
# `deps-publish.sh verify-live` can re-derive the whole tree from upstream and
# prove the mirror honest.
#
# USAGE
#   fetch-upstream.sh [<dest>]                  fetch from the update server (default)
#   fetch-upstream.sh [<dest>] --from-upstream   clone/download from the real upstreams
#                                                (needs internet; used to re-pin)
#   fetch-upstream.sh [<dest>] --check-tools     only report the host prerequisites
# <dest> defaults to <workspace>, giving <dest>/netsurf-port and <dest>/duktape.
#
# AFTER THIS SCRIPT, the libraries still have to be COMPILED:
#   <dest>/netsurf-port/build-all.sh      (the five .a archives the browser links)
# build/build-golden.sh does both automatically when the trees are absent.
#
# Exit: 0 ok, 1 a fetch or a hash check failed, 2 cannot proceed (fail closed).
set -uo pipefail

HERE=$(cd -- "$(dirname -- "$0")" && pwd) || { echo "cannot locate self" >&2; exit 2; }
PINS="$HERE/upstream-pins.tsv"

DEST=<workspace>
MODE=mirror
CHECK_TOOLS_ONLY=0
for a in "$@"; do
    case "$a" in
        --from-upstream) MODE=upstream ;;
        --from-mirror)   MODE=mirror ;;
        --check-tools)   CHECK_TOOLS_ONLY=1 ;;
        -h|--help) grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "fetch-upstream: unknown option: $a" >&2; exit 2 ;;
        *)  DEST=$a ;;
    esac
done

say()  { printf 'fetch-upstream: %s\n' "$*"; }
die1() { printf 'fetch-upstream: FAIL: %s\n' "$*" >&2; exit 1; }
die2() { printf 'fetch-upstream: CANNOT PROCEED: %s\n' "$*" >&2; exit 2; }

[ -s "$PINS" ] || die2 "no pin table at $PINS"

# The bundle mirror. First match wins; each is tried in turn on a failure, so a
# machine on the LAN works with no internet and a machine off the LAN works with
# no VPN. Override with one URL via MAYTERA_DEPS_URL.
if [ -n "${MAYTERA_DEPS_URL:-}" ]; then
    DEPS_URLS="$MAYTERA_DEPS_URL"
else
    DEPS_URLS="http://192.0.2.1/deps/browser
https://updates.maytera.net/deps/browser"
fi
# Downloaded bundles are kept, so a second build on the same host does not
# re-fetch. The cache cannot poison anything: every file is re-hashed against the
# pin on every single use, including a cache hit.
CACHE=${MAYTERA_DEPS_CACHE:-<workspace>}

# ---- host prerequisites -----------------------------------------------------
# These are needed to COMPILE the libraries, not to fetch them, but reporting a
# missing gperf now is worth a great deal more than discovering it 500 lines into
# build-all.sh, where it fails inside a subshell and the build carries on with a
# missing generated source. Name the package, not just the binary.
check_tools() {
    local missing=0 t pkg
    for t in curl:curl tar:tar gzip:gzip xz:xz-utils sha256sum:coreutils \
             perl:perl python3:python3 gperf:gperf cc:gcc ar:binutils; do
        pkg=${t##*:}; t=${t%%:*}
        if ! command -v "$t" >/dev/null 2>&1; then
            printf 'fetch-upstream:   MISSING %-10s (package: %s)\n' "$t" "$pkg"
            missing=$((missing+1))
        fi
    done
    [ "$MODE" = upstream ] && { command -v git >/dev/null 2>&1 || { echo "fetch-upstream:   MISSING git (package: git)"; missing=$((missing+1)); }; }
    if [ "$missing" != 0 ]; then
        say "$missing host prerequisite(s) missing."
        say "perl, python3, gperf and a host cc generate sources inside the NetSurf"
        say "libraries (charset aliases, HTML entities, the element-type perfect hash,"
        say "and the libcss property parsers). Without them the libraries cannot be"
        say "built from source at all, so this is reported here rather than as a"
        say "confusing failure deep inside build-all.sh."
        return 1
    fi
    say "host prerequisites OK"
    return 0
}

if [ "$CHECK_TOOLS_ONLY" = 1 ]; then check_tools; exit $?; fi
check_tools || die2 "install the missing prerequisites and re-run"

mkdir -p "$DEST/netsurf-port" "$DEST/duktape" "$CACHE" || die2 "cannot create $DEST / $CACHE"

ROWS=()
_lineno=0
while IFS= read -r _l; do
    _lineno=$((_lineno+1))
    case "$_l" in ''|'#'*) continue ;; esac
    _n=$(printf '%s' "$_l" | awk -F'\t' '{print NF}')
    [ "$_n" = 8 ] || die2 "$PINS line $_lineno has $_n tab-separated fields, expected 8"
    ROWS+=("$_l")
done < "$PINS"
[ ${#ROWS[@]} -gt 0 ] || die2 "$PINS has no rows"
field() { printf '%s' "$1" | cut -d"$(printf '\t')" -f"$2"; }

# ---- get one bundle into the cache, hash-verified ---------------------------
# A download that does not match the pin is DELETED, not left in the cache for
# the next run to trip over, and it is never extracted. The order is download ->
# hash -> extract, never download -> extract -> hash.
#
# EVERY PROGRESS LINE IN HERE GOES TO STDERR. This function RETURNS the cached
# path on stdout, and the caller reads it with a command substitution, so one
# `say` on stdout silently becomes part of the returned filename. That is not a
# hypothetical: it is what the first end-to-end run of this script actually did,
# and tar then reported "Cannot open" on a path with the progress message
# embedded in it, which reads like a missing file rather than a plumbing bug.
get_bundle() { # $1=bundle $2=sha256 -> prints the cached path on STDOUT, progress on STDERR
    local bundle=$1 want=$2 got base url tmp
    if [ -s "$CACHE/$bundle" ]; then
        got=$(sha256sum -- "$CACHE/$bundle" | cut -d' ' -f1)
        if [ "$got" = "$want" ]; then printf '%s' "$CACHE/$bundle"; return 0; fi
        say "cached $bundle has the wrong hash; discarding it" >&2
        rm -f -- "$CACHE/$bundle"
    fi
    for base in $DEPS_URLS; do
        url="$base/$bundle"
        tmp="$CACHE/.$bundle.part.$$"
        say "GET $url" >&2
        if curl -fsSL --max-time 300 -o "$tmp" "$url"; then
            got=$(sha256sum -- "$tmp" | cut -d' ' -f1)
            if [ "$got" = "$want" ]; then
                mv -f -- "$tmp" "$CACHE/$bundle" || { rm -f -- "$tmp"; return 1; }
                printf '%s' "$CACHE/$bundle"; return 0
            fi
            printf 'fetch-upstream: HASH MISMATCH for %s\n' "$url" >&2
            printf 'fetch-upstream:   expected %s\n' "$want" >&2
            printf 'fetch-upstream:   got      %s\n' "$got" >&2
            printf 'fetch-upstream: NOT extracting it. Either the mirror is serving the wrong\n' >&2
            printf 'fetch-upstream: bytes, or a pin was moved without re-publishing the bundle.\n' >&2
            rm -f -- "$tmp"
            return 1
        fi
        rm -f -- "$tmp"
    done
    return 1
}

# ---- stage the sources ------------------------------------------------------
stage_from_mirror() {
    local row name tree kind pin bundle sha path tmp top
    for row in "${ROWS[@]}"; do
        name=$(field "$row" 1); tree=$(field "$row" 2); kind=$(field "$row" 3)
        pin=$(field "$row" 5);  bundle=$(field "$row" 6); sha=$(field "$row" 7)
        case "$sha" in
            [0-9a-f]*) [ "${#sha}" = 64 ] || die2 "$name: sha256 column is not 64 hex chars: $sha" ;;
            *) die2 "$name: sha256 column is '$sha'. The pin table has not been published yet; run deps-publish.sh build --fix-pins." ;;
        esac
        path=$(get_bundle "$bundle" "$sha") || die1 "could not fetch $bundle from any mirror ($(printf '%s' "$DEPS_URLS" | tr '\n' ' '))"
        case "$kind" in
        git)
            # A git-archive bundle has no top-level directory, so it extracts
            # straight into the library directory. Wipe it first: a half-extracted
            # or older tree merged under a new one is how you get a build that
            # compiles files from two different revisions.
            rm -rf -- "$DEST/netsurf-port/$name"
            mkdir -p -- "$DEST/netsurf-port/$name"
            tar -xzf "$path" -C "$DEST/netsurf-port/$name" || die1 "$name: extract failed"
            [ -f "$DEST/netsurf-port/$name/COPYING" ] \
                || die1 "$name: the extracted tree has no COPYING. We redistribute this source, so its licence text travelling with it is not optional."
            say "$name staged at pin ${pin:0:12} ($(find "$DEST/netsurf-port/$name" -type f | wc -l) files)"
            ;;
        tarball)
            # The upstream Duktape distribution, unmodified. Only the three files
            # we compile plus the licence/authors files are taken out of it; the
            # rest of the distribution is tools and tests we do not build. The
            # full bundle stays in the cache, so the complete corresponding source
            # is on the machine either way.
            tmp=$(mktemp -d) || die2 "mktemp failed"
            tar -xJf "$path" -C "$tmp" || { rm -rf "$tmp"; die1 "duktape: extract failed"; }
            top=$(ls -1 "$tmp" | head -1)
            [ -d "$tmp/$top/src" ] || { rm -rf "$tmp"; die1 "duktape: no src/ in the distribution"; }
            local f
            for f in duktape.c duktape.h duk_config.h; do
                cp -f -- "$tmp/$top/src/$f" "$DEST/duktape/$f" || { rm -rf "$tmp"; die1 "duktape: cannot place $f"; }
            done
            # LICENSE.txt and AUTHORS.rst: Duktape's MIT licence requires the
            # notice to travel with the software, and the live tree had NEITHER
            # of them for months while the browser shipped. Place them, and fail
            # if the distribution does not carry them, rather than shipping a
            # binary whose notice file we never copied.
            for f in LICENSE.txt AUTHORS.rst; do
                [ -f "$tmp/$top/$f" ] || { rm -rf "$tmp"; die1 "duktape: the distribution has no $f"; }
                cp -f -- "$tmp/$top/$f" "$DEST/duktape/$f" || { rm -rf "$tmp"; die1 "duktape: cannot place $f"; }
            done
            # The upstream distribution's licenses/ directory. duktape.c embeds
            # several third-party ALGORITHMS, credited by URL only in the source:
            # MurmurHash2 (Austin Appleby, public domain), xoroshiro128+ and
            # SplitMix64 (Blackman and Vigna, CC0), a Lua-derived string hash and
            # a CommonJS reference. None of those carries an attribution
            # obligation, but upstream ships a notice for each and the audit of
            # 2026-10-20 found we had copied none of them. They cost 5 small
            # files; omitting them bought nothing.
            [ -d "$tmp/$top/licenses" ] || { rm -rf "$tmp"; die1 "duktape: the distribution has no licenses/ directory"; }
            rm -rf -- "$DEST/duktape/licenses"
            cp -a -- "$tmp/$top/licenses" "$DEST/duktape/licenses" || { rm -rf "$tmp"; die1 "duktape: cannot place licenses/"; }
            # THE VERSION ASSERTION READS duktape.h, NOT duk_config.h. The
            # previous version of this script grepped duk_config.h for
            # 'DUK_VERSION 20700L' and printed a WARNING when it was not there.
            # It is not there and never was: measured on the shipping tree,
            # duk_config.h contains ZERO occurrences of 20700L and duktape.h
            # defines it at line 185. So that check had been reporting the wrong
            # answer for its whole existence, and reporting it as a warning
            # nobody reads. It is fatal here, because a silent version check is
            # indistinguishable from no version check.
            _dukver=$(grep -cE '^#define[[:space:]]+DUK_VERSION[[:space:]]+20700L' "$DEST/duktape/duktape.h")
            [ "$_dukver" = 1 ] \
                || die1 "duktape: duktape.h does not define DUK_VERSION 20700L (found $_dukver times), so the staged engine is not $pin"
            rm -rf "$tmp"
            say "duktape $pin staged (duktape.c, duktape.h, duk_config.h, LICENSE.txt, AUTHORS.rst)"
            ;;
        esac
    done
}

stage_from_upstream() {
    command -v git >/dev/null 2>&1 || die2 "git is required for --from-upstream"
    local row name kind url pin tmp top f
    for row in "${ROWS[@]}"; do
        name=$(field "$row" 1); kind=$(field "$row" 3)
        url=$(field "$row" 4);  pin=$(field "$row" 5)
        case "$kind" in
        git)
            local d="$DEST/netsurf-port/$name"
            if [ -d "$d/.git" ]; then
                say "$name: clone present, checking out $pin"
            else
                rm -rf -- "$d"
                git clone --quiet "$url" "$d" || die1 "clone failed: $url"
            fi
            git -C "$d" fetch --quiet origin "$pin" 2>/dev/null || git -C "$d" fetch --quiet origin || true
            git -C "$d" checkout -q "$pin" || die1 "$name: cannot check out $pin"
            say "$name -> $(git -C "$d" rev-parse HEAD)"
            ;;
        tarball)
            tmp=$(mktemp -d) || die2 "mktemp failed"
            curl -fsSL --max-time 300 -o "$tmp/d.tar.xz" "$url" || { rm -rf "$tmp"; die1 "download failed: $url"; }
            tar -xJf "$tmp/d.tar.xz" -C "$tmp" || { rm -rf "$tmp"; die1 "duktape: extract failed"; }
            top=$(ls -1d "$tmp"/duktape-* 2>/dev/null | head -1)
            for f in src/duktape.c src/duktape.h src/duk_config.h LICENSE.txt AUTHORS.rst; do
                cp -f -- "$top/$f" "$DEST/duktape/$(basename "$f")" || { rm -rf "$tmp"; die1 "duktape: cannot place $f"; }
            done
            rm -rf "$tmp"
            say "duktape $pin staged from $url"
            ;;
        esac
    done
}

case "$MODE" in
    mirror)   say "source: the MayteraOS update server"; stage_from_mirror ;;
    upstream) say "source: the real upstreams (needs internet)"; stage_from_upstream ;;
esac

# ---- local patches to the pristine third-party trees ------------------------
# The staged trees are upstream at the pins. Anything we must change in them is a
# committed patch in patches/, applied here. Without this step a re-fetch
# silently reverts the change and the golden ships the unpatched engine, which is
# exactly the divergence build/browser-port-drift.sh exists to catch (it asserts
# the markers are present, so a skipped patch fails the build).
#
# `git apply` is used OUTSIDE a git repository on purpose: it operates on files
# and needs no index, so the same invocation works whether the tree came from a
# tarball (no .git at all) or from a clone.
if [ -d "$HERE/patches" ]; then
    for p in "$HERE"/patches/*.patch; do
        [ -e "$p" ] || continue
        case $(basename "$p") in
            *libhubbub*) tgt=$DEST/netsurf-port/libhubbub ;;
            *libdom*)    tgt=$DEST/netsurf-port/libdom ;;
            *libcss*)    tgt=$DEST/netsurf-port/libcss ;;
            *libparserutils*) tgt=$DEST/netsurf-port/libparserutils ;;
            *libwapcaplet*)   tgt=$DEST/netsurf-port/libwapcaplet ;;
            *)  echo "patch $(basename "$p"): no target library in its name" >&2
                exit 1 ;;
        esac
        [ -d "$tgt" ] || die1 "patch $(basename "$p") targets $tgt, which was not staged"
        if ( cd "$tgt" && git apply --check "$p" ) 2>/dev/null; then
            ( cd "$tgt" && git apply "$p" ) || die1 "applying $(basename "$p") to $tgt failed after its own --check passed"
            say "applied $(basename "$p") to $tgt"
        elif ( cd "$tgt" && git apply --reverse --check "$p" ) 2>/dev/null; then
            say "already applied: $(basename "$p")"
        else
            echo "FAILED to apply $(basename "$p") to $tgt" >&2
            echo "(the pin may have moved; re-base the patch, do not drop it)" >&2
            exit 1
        fi
    done
fi

# ---- MayteraOS glue, from this repo ----------------------------------------
cp -a "$HERE/netsurf/." "$DEST/netsurf-port/" || die1 "could not copy the netsurf glue"
cp -a "$HERE/duktape/." "$DEST/duktape/"      || die1 "could not copy the duktape glue"

# duktape/build.sh lives at the REPO ROOT, not under this directory, so the two
# copies above never brought it, and the browser Makefile names
# $(DUK)/build.sh as the rule that produces the three Duktape objects. Without
# it the browser build dies with "No rule to make target .../duktape/build.sh",
# which is what the first clean-room run of this script actually did.
#
# THE ROOT IS LOCATED, NEVER COUNTED (#721). A hand-written '../../../..' is
# unsound by construction: move this script one directory, or run it from a
# `git archive` materialisation, and a level count silently resolves somewhere
# else. Walk up for marker files instead, honouring the existing $MAYTERA_REPO
# override so there is one convention and not two.
#
# The markers are duktape/build.sh (the file we are here to fetch) and
# build/build-golden.sh. Deliberately NOT build/buildq-guard.sh, which is the
# marker the build ENTRY POINTS use: this script is not an entry point and does
# not source it, and build/buildq-entrypoint-check.sh reasonably objects to a
# file that names buildq-guard.sh without carrying the preamble. Borrowing
# another mechanism's marker makes a file look like something it is not.
_repo_root=''
_root_has() { [ -r "$1/duktape/build.sh" ] && [ -r "$1/build/build-golden.sh" ]; }
if [ -n "${MAYTERA_REPO:-}" ] && _root_has "$MAYTERA_REPO"; then
    _repo_root=$MAYTERA_REPO
else
    _d=$HERE
    while [ -n "$_d" ] && [ "$_d" != / ]; do
        if _root_has "$_d"; then _repo_root=$_d; break; fi
        _d=$(dirname -- "$_d")
    done
fi
[ -n "$_repo_root" ] || die2 "cannot locate the repo root from $HERE (looked for duktape/build.sh + build/build-golden.sh in every parent, and \$MAYTERA_REPO). duktape/build.sh is the rule that compiles the three Duktape objects; the browser cannot be built without it."
cp -a "$_repo_root/duktape/build.sh" "$DEST/duktape/build.sh" \
    || die1 "could not copy duktape/build.sh from $_repo_root"
say "glue copied from $HERE into $DEST (plus duktape/build.sh from $_repo_root)"
echo
say "now compile the libraries:"
say "  MAYTERA_NS_ROOT=$DEST/netsurf-port $DEST/netsurf-port/build-all.sh"
