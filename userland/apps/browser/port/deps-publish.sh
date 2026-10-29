#!/usr/bin/env bash
# deps-publish.sh - rebuild the browser engine's third-party dependency bundles
# from UPSTREAM, and publish them to the MayteraOS update server.
#
# WHAT PROBLEM THIS SOLVES
#
# The browser links five NetSurf core libraries and Duktape. None of that source
# is in git (59 MB of unmodified third-party code), and until now it existed in
# exactly ONE place in the world: the build container:<workspace>{netsurf-port,duktape}.
# `git ls-files | grep -cE 'duktape\.c|duk_config\.h'` returned 0. So the golden
# built only because one machine happened to hold files nothing tracked, and if
# that machine were rebuilt the browser would become unbuildable and the loss
# would be silent. That is the same class as the incident where nine shipping
# apps had no source in git.
#
# The fix the owner asked for is to distribute those two dependencies from the
# update server (the build container). This script is the PRODUCER side of that: it rebuilds
# each bundle from the real upstream, at the pinned revision, and records the
# sha256 that fetch-upstream.sh will then demand. It needs INTERNET and is
# therefore run by hand, rarely: whenever a pin moves. Nothing in a normal build
# runs it.
#
# WHY THE BUNDLES ARE PRISTINE UPSTREAM AND NOTHING ELSE
#
# It would have been easier to tar up the live trees. That would have been wrong
# twice over. The live trees contain (a) build outputs, which is exactly the
# "prebuilt binary with no build rule" that made this unbuildable in the first
# place, and (b) copies of MayteraOS-authored glue that IS in git, so the bundle
# would become a second copy of tracked source and could drift from it. #386,
# #650 and #659 are all that shape. The bundles are therefore upstream source
# only; our glue comes from the repo and our changes to upstream come from the
# committed patches in patches/.
#
# USAGE
#   deps-publish.sh build [--out <dir>]      build every bundle from upstream
#   deps-publish.sh build --fix-pins         ... and write the sha256s into upstream-pins.tsv
#   deps-publish.sh verify-live [--ns <dir>] [--duk <dir>]
#                                            PROVE a live tree is pristine-upstream-at-pin
#                                            plus the committed patches, and nothing else
#   deps-publish.sh publish-cmds [--out <dir>]
#                                            print the exact host commands that publish
#                                            the built bundles through the sanctioned gate
#
# Exit: 0 ok, 1 a check failed, 2 cannot proceed (fail closed).
set -uo pipefail

HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd) || { echo "cannot locate self" >&2; exit 2; }
PINS="$HERE/upstream-pins.tsv"
OUT=${DEPS_OUT:-/root/deps-bundles}
STAGE=${DEPS_STAGE:-/root/deps-stage}
NS_LIVE=${MAYTERA_NS_ROOT:-<workspace>}
DUK_LIVE=${MAYTERA_DUK_ROOT:-<workspace>}
FIX=0

say()  { printf 'deps-publish: %s\n' "$*"; }
die1() { printf 'deps-publish: FAIL: %s\n' "$*" >&2; exit 1; }
die2() { printf 'deps-publish: CANNOT PROCEED: %s\n' "$*" >&2; exit 2; }

[ -s "$PINS" ] || die2 "no pin table at $PINS"

# Read the table once. Blank lines and #-comments are skipped. A row that does
# not have exactly 8 fields is a hard error rather than something to work around,
# because every consumer of this file indexes the columns positionally.
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

need() { command -v "$1" >/dev/null 2>&1 || die2 "$1 is required and is not on PATH"; }

# ---------------------------------------------------------------- build --------
cmd_build() {
    need git; need curl; need tar; need gzip; need sha256sum
    mkdir -p "$OUT" "$STAGE" || die2 "cannot create $OUT / $STAGE"
    say "stage $STAGE -> bundles $OUT"
    local row name tree kind url pin bundle sha
    for row in "${ROWS[@]}"; do
        name=$(field "$row" 1); tree=$(field "$row" 2); kind=$(field "$row" 3)
        url=$(field "$row" 4);  pin=$(field "$row" 5); bundle=$(field "$row" 6)
        case "$kind" in
        git)
            local d="$STAGE/$name"
            if [ -d "$d/.git" ]; then
                say "$name: stage present"
            else
                git clone --quiet --filter=blob:none --no-checkout "$url" "$d" \
                    || die1 "clone failed: $url"
            fi
            # Fetch the pin explicitly: the default branch may have moved past
            # it, and a shallow clone may not contain it at all.
            git -C "$d" fetch --quiet origin "$pin" 2>/dev/null \
                || git -C "$d" fetch --quiet origin || die1 "fetch failed: $url"
            git -C "$d" cat-file -e "$pin^{commit}" 2>/dev/null \
                || die1 "$name: upstream does not contain pinned commit $pin"
            # SOURCE ONLY. A full archive of these repositories is NOT
            # redistributable, and that is a measurement, not caution. The
            # licence audit of 2026-10-20 found, in the TEST TREES only:
            #   libcss/test/data/css/allzengarden.css  139 blocks under CC
            #     BY-NC-SA 1.0 (NonCommercial) across ~211 designers, with an
            #     express "You may not reproduce it elsewhere". No notice file
            #     can cure a NonCommercial term.
            #   libhubbub/test/data/html/*  nine real third-party web pages
            #     saved as crash fixtures (Coriell, Direct Line, UBC and six
            #     more): all-rights-reserved, no grant of any kind.
            #   libdom/test/testcases/  13 MB of the W3C DOM Test Suite under
            #     the W3C Software Licence (plus 38 Oracle and 34 X-Hive
            #     files), with no licence text present anywhere in the tree.
            #   libdom/test/dom*-interface*.xml  five files under the W3C
            #     DOCUMENT Licence, which does not permit derivatives.
            # None of it is compiled, linked or shipped by MayteraOS. Dropping
            # the test trees removes every non-MIT item in one step, instead of
            # shipping four more licence texts for fixtures we never use.
            #
            # build/ STAYS, and that matters: it holds the code generators
            # build-all.sh runs (make-aliases.pl, make-entities.pl, the Entities
            # table, mkprops.pl). Without it the libraries cannot be built at
            # all. The audit's suggestion to drop build/ was aimed at the 458
            # stale .o files in the LIVE tree; a git archive contains none of
            # those, because they were never committed.
            #
            # The list is EXCLUSIONS of top-level entries, resolved against what
            # the pin actually contains, so a pin that adds a directory gets it
            # included rather than silently dropped. COPYING is asserted below:
            # a bundle we redistribute without its licence text is not shippable
            # and must fail here, not be noticed later.
            local keep=() e
            while IFS= read -r e; do
                case "$e" in
                    test|examples|perf|docs|gdb|bindings|.github|.clang-format|.gitignore|.gitattributes) continue ;;
                esac
                keep+=("$e")
            done < <(git -C "$d" ls-tree --name-only "$pin")
            printf '%s\n' "${keep[@]}" | grep -qx COPYING \
                || die1 "$name: the pin has no top-level COPYING. We redistribute this source; refusing to build a bundle with no licence text."
            # From the OBJECT DATABASE, never the working tree: a dirty checkout
            # at a shared path is how the ClassiCube vendoring once absorbed
            # another agent's edit without a word (see classicube/fetch-upstream.sh).
            # gzip -n so the bundle carries no timestamp and rebuilds identically.
            git -C "$d" archive --format=tar "$pin" -- "${keep[@]}" > "$OUT/.$bundle.tar" \
                || die1 "$name: git archive $pin failed"
            gzip -n -9 -c < "$OUT/.$bundle.tar" > "$OUT/$bundle" \
                || die1 "$name: gzip failed"
            rm -f "$OUT/.$bundle.tar"
            ;;
        tarball)
            # Byte-identical upstream release artifact. We do NOT repack it: the
            # sha256 below is then the same hash anyone else downloading from
            # upstream would compute, which makes the pin independently checkable
            # rather than a number only we can produce.
            if [ -s "$OUT/$bundle" ]; then
                say "$name: $bundle already built"
            else
                curl -fsSL --max-time 300 -o "$OUT/.$bundle.part" "$url" \
                    || die1 "download failed: $url"
                mv -f "$OUT/.$bundle.part" "$OUT/$bundle" || die1 "rename failed"
            fi
            ;;
        *) die2 "$name: unknown kind '$kind'" ;;
        esac
        sha=$(sha256sum -- "$OUT/$bundle" | cut -d' ' -f1)
        printf '  %-16s %-44s %s  %s\n' "$name" "$bundle" "$sha" \
               "$(stat -c %s -- "$OUT/$bundle") bytes"
        if [ "$FIX" = 1 ]; then
            # Rewrite column 7 of this row in place. Matched on column 6 (the
            # bundle name), which carries the pin and is therefore unique.
            BUNDLE="$bundle" SHA="$sha" python3 - "$PINS" <<'PY' || die2 "could not update $PINS"
import os, sys
path = sys.argv[1]
bundle, sha = os.environ["BUNDLE"], os.environ["SHA"]
out, hit = [], 0
for line in open(path).read().splitlines(True):
    if line.startswith("#") or "\t" not in line:
        out.append(line); continue
    f = line.rstrip("\n").split("\t")
    if len(f) == 8 and f[5] == bundle:
        f[6] = sha; hit += 1
        line = "\t".join(f) + "\n"
    out.append(line)
if hit != 1:
    sys.stderr.write("expected exactly 1 row for %s, matched %d\n" % (bundle, hit))
    sys.exit(1)
open(path, "w").write("".join(out))
PY
        fi
    done
    ( cd "$OUT" && sha256sum -- $(for row in "${ROWS[@]}"; do field "$row" 6; printf ' '; done) > SHA256SUMS ) \
        || die1 "could not write $OUT/SHA256SUMS"
    say "wrote $OUT/SHA256SUMS"
    [ "$FIX" = 1 ] && say "pins updated in $PINS (commit it)"
    return 0
}

# ------------------------------------------------------------ verify-live ------
# The claim being proved: the live trees that every golden has been built from
# really are pristine upstream at the pins, plus the committed patches, and
# nothing else. If that is false then the bundles do NOT reproduce the shipping
# browser and this whole change would be a quiet regression. Measure it, do not
# assume it.
cmd_verify_live() {
    need git; need diff
    local rc=0 row name tree kind pin d
    for row in "${ROWS[@]}"; do
        name=$(field "$row" 1); tree=$(field "$row" 2)
        kind=$(field "$row" 3); pin=$(field "$row" 5)
        [ "$kind" = git ] || continue
        d="$NS_LIVE/$name"
        if [ ! -d "$d/.git" ]; then
            printf '  %-16s NO LIVE CLONE at %s (skipped)\n' "$name" "$d"; continue
        fi
        local head dirty
        head=$(git -C "$d" rev-parse HEAD 2>/dev/null)
        if [ "$head" != "$pin" ]; then
            printf '  %-16s HEAD %s != pin %s\n' "$name" "${head:-<none>}" "$pin"; rc=1; continue
        fi
        # Uncommitted changes in a pristine third-party clone must be exactly
        # what our committed patches produce. Compare the live diff against the
        # patch set rather than merely counting files.
        dirty=$(git -C "$d" diff --name-only | tr '\n' ' ')
        local expect="" p
        for p in "$HERE"/patches/*"$name"*.patch; do
            [ -e "$p" ] || continue
            expect="$expect $(basename "$p")"
        done
        if [ -z "$dirty" ] && [ -z "$expect" ]; then
            printf '  %-16s PRISTINE at pin, no patches expected\n' "$name"; continue
        fi
        # Re-derive: pristine checkout of the pin in a temp worktree, apply the
        # committed patches, diff against the live tree's tracked files.
        local tmp; tmp=$(mktemp -d) || die2 "mktemp failed"
        git -C "$d" archive --format=tar "$pin" | tar -x -C "$tmp" || { rm -rf "$tmp"; die1 "$name: archive failed"; }
        for p in "$HERE"/patches/*"$name"*.patch; do
            [ -e "$p" ] || continue
            ( cd "$tmp" && git apply "$p" ) || { rm -rf "$tmp"; die1 "$name: committed patch $(basename "$p") does not apply to pin $pin"; }
        done
        # Only tracked files are compared: the live tree is also a build
        # directory, full of .o and generated sources, and those are outputs.
        #
        # A tracked file can be legitimately ABSENT from the archive: `git
        # archive` honours the export-ignore attribute, and all five libs mark
        # .gitignore and .gitattributes with it. Treating that as divergence
        # made this check cry wolf on its first run, and a check that cries wolf
        # is one somebody eventually stops reading. So ask git whether the file
        # is export-ignored, and fail only when it is not.
        local bad=0 f
        while IFS= read -r f; do
            if [ ! -f "$tmp/$f" ]; then
                if git -C "$d" check-attr export-ignore -- "$f" 2>/dev/null | grep -q ': export-ignore: set$'; then
                    continue
                fi
                # Excluded from the redistributed bundle on licence grounds (see
                # cmd_build). Still present in the live clone, which is a full
                # checkout; its absence from the bundle is the intended state.
                case "$f" in
                    test/*|examples/*|perf/*|docs/*|gdb/*|bindings/*|.github/*|.clang-format) continue ;;
                esac
                printf '  %-16s file in live tree missing from pin: %s\n' "$name" "$f"; bad=1; continue
            fi
            cmp -s "$tmp/$f" "$d/$f" || { printf '  %-16s DIFFERS from pin+patches: %s\n' "$name" "$f"; bad=1; }
        done < <(git -C "$d" ls-files)
        rm -rf "$tmp"
        if [ "$bad" = 0 ]; then
            printf '  %-16s = upstream %s + %s\n' "$name" "${pin:0:12}" "${expect:-no patches}"
        else
            rc=1
        fi
    done
    # Duktape: the three files we compile must be byte-identical to the upstream
    # distribution. This is the whole of our Duktape provenance claim.
    local bundle="" sha=""
    for row in "${ROWS[@]}"; do
        [ "$(field "$row" 3)" = tarball ] || continue
        bundle=$(field "$row" 6)
    done
    if [ -n "$bundle" ] && [ -s "$OUT/$bundle" ]; then
        local tmp; tmp=$(mktemp -d) || die2 "mktemp failed"
        tar -xJf "$OUT/$bundle" -C "$tmp" || { rm -rf "$tmp"; die1 "cannot unpack $OUT/$bundle"; }
        local top; top=$(ls -1 "$tmp" | head -1)
        local f
        for f in duktape.c duktape.h duk_config.h; do
            if [ ! -f "$DUK_LIVE/$f" ]; then
                printf '  %-16s live tree has no %s\n' duktape "$f"; rc=1; continue
            fi
            if cmp -s "$tmp/$top/src/$f" "$DUK_LIVE/$f"; then
                printf '  %-16s %s = upstream distribution\n' duktape "$f"
            else
                printf '  %-16s %s DIFFERS from the upstream distribution\n' duktape "$f"; rc=1
            fi
        done
        rm -rf "$tmp"
    else
        printf '  %-16s no built bundle at %s (run: %s build)\n' duktape "$OUT/${bundle:-?}" "$0"
    fi
    [ "$rc" = 0 ] && say "LIVE TREES ARE PRISTINE-UPSTREAM-AT-PIN (+ committed patches)" \
                  || say "LIVE TREES DIVERGE from the pins; the bundles would NOT reproduce them"
    return "$rc"
}

# ----------------------------------------------------------- publish-cmds ------
# Publishing goes through server/appstore/publish-from-host.sh, which runs the
# release secret gate on the artifact and materialises the gate out of the
# commit. Hand-pushing into the docroot with `pct push` bypasses that, and the
# one time the gate was bypassed a live credential was published. So this
# command prints, it does not push.
cmd_publish_cmds() {
    local row b
    echo "# Run these ON THE the build host HOST, as root, after committing the pin table."
    echo "# Each one runs the release secret gate on the artifact before it lands."
    for row in "${ROWS[@]}"; do
        b=$(field "$row" 6)
        echo "<repo>/server/appstore/publish-from-host.sh $OUT/$b deps/browser/$b"
    done
    echo "<repo>/server/appstore/publish-from-host.sh $OUT/SHA256SUMS deps/browser/SHA256SUMS"
}

cmd=${1:-}; shift 2>/dev/null || true
while [ $# -gt 0 ]; do
    case "$1" in
        --out)      OUT=${2:?}; shift 2 ;;
        --stage)    STAGE=${2:?}; shift 2 ;;
        --ns)       NS_LIVE=${2:?}; shift 2 ;;
        --duk)      DUK_LIVE=${2:?}; shift 2 ;;
        --fix-pins) FIX=1; shift ;;
        *) die2 "unknown argument: $1" ;;
    esac
done

case "$cmd" in
    build)        cmd_build ;;
    verify-live)  cmd_verify_live ;;
    publish-cmds) cmd_publish_cmds ;;
    ''|-h|--help) grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die2 "unknown command '$cmd' (build | verify-live | publish-cmds)" ;;
esac
