#!/bin/bash
# jsdom-native-diff.sh - #245/#275: does a change to duk_dom.c change what a
# page DOES?
#
# Builds the shipped engine twice from the same NetSurf + Duktape roots, once
# with a REFERENCE duk_dom.c and once with the one under test, runs the DOM
# fixtures in this directory through both, and compares byte for byte.
#
# IT ALSO BUILDS A THIRD, DELIBERATELY BROKEN ARM and exits non-zero if that arm
# does NOT diverge. A differential that has never been shown capable of failing
# is not evidence, and this tree has shipped more than one check that could not
# fail (#514's lint that never ran, #663/#664's gate guarded by a skipping test).
#
# NOT WIRED INTO THE GOLDEN BUILD, and that is a deliberate limit rather than an
# oversight: it compiles the whole engine twice, which is minutes, and the
# golden build already has two blocking gates on this directory
# (browser-port-drift.sh, twice). Run it by hand when touching the binding.
#
# Usage:
#   NS=<netsurf-port root> DUK=<duktape root> RIG=<scratch dir> \
#   CC_HARNESS=<harness.c> ./jsdom-native-diff.sh [reference-duk_dom.c]
#
# The reference defaults to the committed copy one directory up. The rig needs a
# host gcc and the same generated NetSurf sources build-all.sh produces.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REF=${1:-$HERE/../duktape/duk_dom.c}
NS=${NS:-<workspace>}
DUK=${DUK:-<workspace>}
RIG=${RIG:-}
[ -n "$RIG" ] || { echo "set RIG=<scratch dir holding build.sh + harness.c>"; exit 2; }
[ -f "$RIG/build.sh" ] || { echo "no $RIG/build.sh"; exit 2; }
[ -f "$REF" ] || { echo "no reference duk_dom.c at $REF"; exit 2; }

TESTED=$DUK/duk_dom.c
SAVE=$(mktemp); cp "$TESTED" "$SAVE"
restore() { cp "$SAVE" "$TESTED"; rm -f "$SAVE"; }
trap restore EXIT

build() {  # $1 = source to install as duk_dom.c, $2 = output name
  cp "$1" "$TESTED"
  rm -f "$RIG/obj/duk_dom.o"
  ( cd "$RIG" && OUTNAME="$2" ./build.sh ) >/dev/null 2>&1 || { echo "build $2 FAILED"; exit 3; }
}

# THE BROKEN ARM. The same source under test with ONE thing wrong:
# document.createElement is replaced by null, immediately after the real one is
# installed. The break is deliberately NARROW rather than catastrophic: a build
# that fails to start at all would make every fixture differ and would prove
# only that the rig can tell a working binary from a broken one. This one must
# make exactly the fixtures that CREATE ELEMENTS differ, and leave the one that
# does not alone, which is a much sharper statement about what the comparison
# can see. dom1 and dom2 create elements; dom3 does not.
BROKEN=$(mktemp --suffix=.c)
python3 - "$SAVE" "$BROKEN" <<'PY'
import sys
src, dst = sys.argv[1], sys.argv[2]
s = open(src).read()
# Anchor on the line that installs `document` on the global object. The stack is
# [global] immediately after it, which is what the injected block assumes.
needle = '    duk_put_prop_string(ctx, -2, "document");'
assert needle in s, "document-install anchor missing from duk_dom.c"
s = s.replace(needle, needle + """
    /* RED ARM ONLY, injected by jsdom-native-diff.sh: break createElement. */
    duk_get_prop_string(ctx, -1, "document");
    duk_push_null(ctx);
    duk_put_prop_string(ctx, -2, "createElement");
    duk_pop(ctx);""", 1)
open(dst, "w").write(s)
PY

build "$REF"    harness-ref
build "$SAVE"   harness-test
build "$BROKEN" harness-broken
rm -f "$BROKEN"

same=0; diff_green=0; red_seen=0; red_total=0
for f in "$HERE"/dom*.html; do
  a=$("$RIG/harness-ref"    file "$f" 2>&1 | sed 's/.*log=//')
  b=$("$RIG/harness-test"   file "$f" 2>&1 | sed 's/.*log=//')
  c=$("$RIG/harness-broken" file "$f" 2>&1 | sed 's/.*log=//')
  if [ "$a" = "$b" ]; then same=$((same+1)); r="SAME"; else diff_green=$((diff_green+1)); r="DIFFER"; fi
  red_total=$((red_total+1))
  if [ "$a" = "$c" ]; then rr="RED-SAME"; else rr="RED-DIFFER"; red_seen=$((red_seen+1)); fi
  echo "$(basename "$f"): ref-vs-test=$r  ref-vs-BROKEN=$rr"
done

echo "GREEN: identical=$same differing=$diff_green of $red_total"
echo "RED  : the broken arm diverged on $red_seen of $red_total fixtures"
rc=0
[ "$diff_green" -eq 0 ] || { echo "FAIL: the source under test changed what a page does"; rc=1; }
[ "$red_seen" -gt 0 ]   || { echo "FAIL: the broken arm did NOT diverge, so this comparison proves nothing"; rc=1; }
# A broken arm that breaks EVERYTHING would also satisfy the line above while
# saying much less. Exactly one fixture here (dom3) never calls createElement,
# so a correctly narrow break leaves at least one fixture matching.
[ "$red_seen" -lt "$red_total" ] || { echo "FAIL: the broken arm diverged on EVERY fixture, including the one that never calls createElement. The injection is too blunt to be evidence about createElement specifically."; rc=1; }
[ "$rc" -eq 0 ] && echo "RESULT: PASS"
exit $rc
