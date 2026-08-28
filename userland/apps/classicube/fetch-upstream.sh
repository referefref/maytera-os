#!/usr/bin/env bash
# fetch-upstream.sh - deterministically (re)stage and VERIFY the vendored
# ClassiCube upstream tree.
#
# The buildable engine source is ALSO vendored in-repo under
# vendor/ClassiCube/ (so a git archive of the internal source repo can rebuild ClassiCube
# with no network). This script exists to (a) record the exact upstream
# provenance as CODE rather than only as prose, and (b) PROVE the vendored copy
# still equals upstream at the pinned commit. Same convention as
# userland/apps/assaultcube/fetch-engine.sh. See PROVENANCE.md.
#
# ------------------------------------------------------------------------------
# WHY `verify` USES `git archive` AND NOT A DIFF AGAINST THE CHECKOUT
#
# It got this wrong once, on 2026-08-10, and the failure was silent: the staged
# clone lived at a SHARED path, a parallel agent working on the same ticket
# edited src/Core.h INSIDE THAT STAGE, and `cp -a stage/src .` then vendored the
# other agent's edit as if it were upstream. Nothing complained; the only reason
# it surfaced was that the Core.h patch script asserts its anchors and found the
# chain head already consumed.
#
# Two fixes, both here: STAGE defaults to a PER-CHECKOUT private directory, and
# verification reads blobs out of the OBJECT DATABASE at the pinned commit
# (`git archive <commit>`), which cannot see a dirty working tree at all.
#
# ------------------------------------------------------------------------------
# WHY `verify` NOW DEMANDS *ZERO* DIFFERENCES (#800, five-lane merge)
#
# The earlier version tolerated two expected differences: a maytera/ subdirectory
# inside the vendored tree, and one patched hunk in src/Core.h. Neither exists
# any more. The MayteraOS backends live at the top of userland/apps/classicube/,
# and the Core.h platform branch is applied by the app Makefile to a STAGED COPY
# under build/src, never to vendor/. So "vendor/ClassiCube is byte-identical to
# the pin" is now a flat, checkable statement, which is exactly what the BSD
# attribution claim in PROVENANCE.md and ATTRIBUTION.md rests on. A tolerated
# exception is how that claim would rot.
# ------------------------------------------------------------------------------
#
# Pinned provenance:
UPSTREAM_URL=https://github.com/UnknownShadow200/ClassiCube.git
UPSTREAM_COMMIT=4016a0918ba5c127d5203a4940e76b79b229d51f   # "Windows: try to support icons on ARM builds too", 2026-08-09
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
VENDOR="$HERE/vendor/ClassiCube"
# Private by default: derived from THIS checkout's path, so two worktrees never
# share a stage. Override with STAGE=... only if you know why.
STAGE=${STAGE:-"$HERE/.upstream-stage"}

cmd=${1:-fetch}

do_fetch() {
  if [ -d "$STAGE/.git" ]; then
    echo "stage exists: $STAGE (HEAD=$(git -C "$STAGE" rev-parse HEAD))"
  else
    mkdir -p "$(dirname "$STAGE")"
    git clone --filter=blob:none --no-checkout "$UPSTREAM_URL" "$STAGE"
  fi
  git -C "$STAGE" fetch origin "$UPSTREAM_COMMIT" --depth 1 2>/dev/null || git -C "$STAGE" fetch origin
  git -C "$STAGE" checkout --detach "$UPSTREAM_COMMIT"
  echo "staged ClassiCube at pinned commit $UPSTREAM_COMMIT"
}

# Extract pristine upstream src/ + license.txt + credits.txt from the object
# database and diff them against the vendored copy. ANY difference is a failure.
do_verify() {
  [ -d "$STAGE/.git" ] || { echo "no stage at $STAGE; run '$0 fetch' first" >&2; exit 2; }
  tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' EXIT
  git -C "$STAGE" archive "$UPSTREAM_COMMIT" src license.txt credits.txt readme.md | tar -x -C "$tmp"

  rc=0
  diff -ru "$tmp/src" "$VENDOR/src" > "$tmp/diff.txt" 2>&1 || rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "VENDOR TREE DIFFERS FROM UPSTREAM $UPSTREAM_COMMIT:"
    grep -E '^(diff|Only in)' "$tmp/diff.txt" | head -40
    exit 1
  fi
  for f in license.txt credits.txt; do
    cmp -s "$tmp/$f" "$VENDOR/$f" || { echo "$f differs from upstream" >&2; exit 1; }
  done
  cmp -s "$tmp/readme.md" "$VENDOR/readme.md" || { echo "readme.md differs from upstream" >&2; exit 1; }
  echo "OK: vendor/ClassiCube/src is BYTE-IDENTICAL to upstream $UPSTREAM_COMMIT ($(find "$VENDOR/src" -type f | wc -l) files)"
  echo "OK: license.txt, credits.txt and readme.md are byte-identical too"
}

case "$cmd" in
  fetch)  do_fetch ;;
  verify) do_verify ;;
  all)    do_fetch; do_verify ;;
  *) echo "usage: $0 [fetch|verify|all]" >&2; exit 2 ;;
esac
