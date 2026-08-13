#!/bin/bash
# fetch-upstream.sh - re-create the browser engine's third-party trees.
#
# The browser links five NetSurf core libraries and the Duktape JavaScript
# engine. All six are PRISTINE upstream: no MayteraOS patch is applied to any
# of them, which is why they are not vendored into this repo (48MB + 3.7MB of
# unmodified third-party code). The MayteraOS-authored glue that sits on top of
# them IS in this repo, next to this script.
#
# The commits below are the exact revisions the shipping browser was built
# against. They are PINS, not "latest": the shim headers in shim-include/ and
# the bindings in netsurf/ track these APIs. Cloning upstream HEAD instead may
# not compile.
#
# After this script, run netsurf/build-all.sh (needs perl, python3, gperf and a
# host cc for the code generators) and duktape/../../../../duktape/build.sh.
set -e

DEST=${1:-<workspace>}
mkdir -p "$DEST/netsurf-port" "$DEST/duktape"

# --- NetSurf core libraries, pinned -----------------------------------------
# Fetched 2026-01 to 2026-02; recorded here 2026-08-04 from the live trees.
clone_at() { # $1=repo $2=dir $3=commit
    local url=$1 dir=$2 commit=$3
    if [ -d "$DEST/netsurf-port/$dir/.git" ]; then
        echo "$dir: already present, checking out $commit"
    else
        git clone "$url" "$DEST/netsurf-port/$dir"
    fi
    git -C "$DEST/netsurf-port/$dir" checkout -q "$commit"
    echo "$dir -> $(git -C "$DEST/netsurf-port/$dir" rev-parse HEAD)"
}

NS=https://github.com/netsurf-browser
clone_at $NS/libwapcaplet.git   libwapcaplet   c7c128d3eb3223b216c974471f82e9337fbcf4ba
clone_at $NS/libparserutils.git libparserutils 6b0cbf086ca8eb8fe74b69f0c9ecf274eb2397ca
clone_at $NS/libhubbub.git      libhubbub      6651b8cf87a4aa87bcdb2ff024a02659cd3f9402
clone_at $NS/libcss.git         libcss         104d87fde48b9e022cd3cdad28aeb4d8cc0a0c5a
clone_at $NS/libdom.git         libdom         f69781e1f062444b5af3f62d431d7d94018da53b

# --- Duktape, pinned by release ---------------------------------------------
# duktape.c / duktape.h / duk_config.h are the stock 2.7.0 distribution
# (DUK_VERSION 20700L). Nothing in them is modified; all MayteraOS behaviour
# lives in duk_support.c and duk_dom.c, which ARE in this repo.
DUKV=2.7.0
if [ ! -f "$DEST/duktape/duktape.c" ]; then
    tmp=$(mktemp -d)
    curl -fsSL -o "$tmp/duktape.tar.xz" \
        "https://duktape.org/duktape-${DUKV}.tar.xz"
    tar -xJf "$tmp/duktape.tar.xz" -C "$tmp"
    cp "$tmp/duktape-${DUKV}/src/duktape.c" \
       "$tmp/duktape-${DUKV}/src/duktape.h" \
       "$tmp/duktape-${DUKV}/src/duk_config.h" "$DEST/duktape/"
    rm -rf "$tmp"
fi
grep -q 'DUK_VERSION *20700L' "$DEST/duktape/duk_config.h" \
    && echo "duktape: $DUKV confirmed" \
    || echo "WARNING: duktape version is NOT $DUKV"

# --- MayteraOS glue, from this repo -----------------------------------------
HERE=$(cd "$(dirname "$0")" && pwd)

# --- local patches to the pristine third-party trees ------------------------
# The clones above are upstream at the pins. Anything we must change in them is
# a committed patch in patches/, applied here. Without this step a re-fetch
# silently reverts the change and the golden ships the unpatched engine, which
# is exactly the divergence build/browser-port-drift.sh exists to catch (it
# asserts the markers are present, so a skipped patch fails the build).
if [ -d "$HERE/patches" ]; then
    for p in "$HERE"/patches/*.patch; do
        [ -e "$p" ] || continue
        # every patch is written against libhubbub's tree root for now; if a
        # future patch targets another library, add its dir to this case.
        case $(basename "$p") in
            *libhubbub*) tgt=$DEST/netsurf-port/libhubbub ;;
            *)  echo "patch $(basename "$p"): no target library in its name" >&2
                exit 1 ;;
        esac
        if git -C "$tgt" apply --check "$p" 2>/dev/null; then
            git -C "$tgt" apply "$p"
            echo "applied $(basename "$p") to $tgt"
        elif git -C "$tgt" apply --reverse --check "$p" 2>/dev/null; then
            echo "already applied: $(basename "$p")"
        else
            echo "FAILED to apply $(basename "$p") to $tgt" >&2
            echo "(the pin may have moved; re-base the patch, do not drop it)" >&2
            exit 1
        fi
    done
fi

cp -a "$HERE/netsurf/." "$DEST/netsurf-port/"
cp -a "$HERE/duktape/." "$DEST/duktape/"
echo
echo "Glue copied from the repo into $DEST. Now run:"
echo "  $DEST/netsurf-port/build-all.sh"
