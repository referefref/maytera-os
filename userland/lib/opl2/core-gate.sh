#!/bin/bash
# core-gate.sh - #182: THERE IS EXACTLY ONE FM SYNTHESISER, AND THIS IS WHY IT
# STAYS THAT WAY.
#
# The whole architectural decision in #182 is "one synthesiser core, in
# userland, with two consumers". A sentence cannot enforce that. This project's
# signature failure is a second implementation appearing next to the first
# because it was quicker than reusing it, and then the two drifting:
#
#   two Task Managers                one a never-firing kernel fallback
#   two g_wallpapers[] arrays        the desktop and the settings app disagreed
#   five version.h files             nobody could say which one shipped
#   msh and terminal (#112)          a private env table each
#   FIVE CPU-ranking copies (#178)   one defect fixed twice and missed once
#
# In every one of those the review that would have caught it was a person
# noticing. So this is a MECHANISM: it FAILS THE BUILD when a second FM core
# appears, and it goes RED destructively on demand so that nobody has to take
# its word for it.
#
#   ./core-gate.sh <repo-root>     0 = one core, 1 = a second one appeared
#   ./core-gate.sh --self-test     PROVES it goes RED on a planted duplicate
#
# WHAT IT LOOKS FOR, and why these three markers and not a filename check:
#
#   1. THE LOG-SINE TABLE. Any second OPL implementation must have one, because
#      the chip cannot be emulated without it. The signature is logsin[0] and
#      logsin[1] ADJACENT: "2137" followed by "1731". A bare 2137 is NOT enough
#      and this gate said so the hard way: on its first run it flagged
#      kernel/rustkern/winbuf.rs and userland/apps/doom/tables.c, both of which
#      simply contain the number 2137 in an unrelated table. A gate with false
#      positives gets disabled, so the marker is the PAIR, which no unrelated
#      table has any reason to contain in that order.
#   2. THE OPERATOR OFFSET MAP. The 0x00,0x01,0x02,0x08,0x09,0x0A,0x10,0x11,0x12
#      sequence is the OPL2's channel-to-operator map and appears in every
#      register decoder ever written for it.
#   3. THE MULT TABLE. 1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30 with its
#      giveaway duplicated entries.
#
# A file is allowed to contain these ONLY if it is the sanctioned core or its
# generator. Anything else is a second implementation by definition.

set -uo pipefail

SELFTEST=0
ROOT="."
for a in "$@"; do
    case "$a" in
        --self-test) SELFTEST=1 ;;
        -h|--help) sed -n '2,40p' "$0"; exit 0 ;;
        *) ROOT="$a" ;;
    esac
done

# The ONE core, plus the generator and gate that legitimately mention it.
ALLOWED_RE='^(userland/lib/opl2/(opl2_tables\.rs|opl2core\.rs|gen_tables\.py|core-gate\.sh|table-gate\.sh)|docs/OPL2_FM_CORE\.md)$'

scan() {
    local root="$1"
    local bad=0
    local hits=""

    # Marker 1: logsin[0]. Written as a bare 2137 next to a table or a formula.
    hits=$(grep -rIlzE '2137,[[:space:]]*1731' "$root" \
             --include='*.rs' --include='*.c' --include='*.h' --include='*.py' \
             --exclude-dir=.git --exclude-dir=vendor --exclude-dir=port 2>/dev/null || true)
    # Marker 2: the operator offset map.
    hits="$hits
$(grep -rIlE '0x00, *0x01, *0x02, *0x08, *0x09, *0x0A' "$root" \
             --include='*.rs' --include='*.c' --include='*.h' --include='*.py' \
             --exclude-dir=.git --exclude-dir=vendor --exclude-dir=port 2>/dev/null || true)"
    # Marker 3: the MULT table, with its duplicated 20,20 / 24,24 / 30,30.
    hits="$hits
$(grep -rIlE '20, *20, *24, *24, *30, *30' "$root" \
             --include='*.rs' --include='*.c' --include='*.h' --include='*.py' \
             --exclude-dir=.git --exclude-dir=vendor --exclude-dir=port 2>/dev/null || true)"

    for f in $(printf '%s\n' $hits | sed "s#^$root/##" | sort -u); do
        [ -n "$f" ] || continue
        if ! printf '%s' "$f" | grep -qE "$ALLOWED_RE"; then
            echo "[opl-core-gate] FAIL: $f carries an OPL2 core marker but is not the one core."
            echo "                 The one core is userland/lib/opl2/. If you need FM"
            echo "                 synthesis, include it; do not write a second one."
            bad=1
        fi
    done
    return $bad
}

if [ "$SELFTEST" -eq 1 ]; then
    # A gate that has never been seen to fail is not a gate. Plant a second
    # core in a scratch tree and require RED, then remove it and require GREEN.
    TMP=$(mktemp -d)
    trap 'rm -rf "$TMP"' EXIT
    mkdir -p "$TMP/userland/lib/opl2" "$TMP/userland/apps/rogueapp"
    echo 'pub static LOGSIN: [u16; 2] = [2137, 0];' > "$TMP/userland/lib/opl2/opl2_tables.rs"

    echo "[opl-core-gate] self-test 1/2: clean tree must be GREEN"
    if scan "$TMP"; then
        echo "  GREEN as required"
    else
        echo "  BROKEN: the gate failed a tree with only the sanctioned core in it."
        exit 1
    fi

    echo "[opl-core-gate] self-test 2/2: a planted second core must be RED"
    cat > "$TMP/userland/apps/rogueapp/mysynth.rs" <<'ROGUE'
// A plausible second OPL core: someone needed FM in their app and it was
// quicker to write one than to find the shared one.
const LOGSIN: [u16; 4] = [2137, 1731, 1543, 1419];
const SLOTS: [u8; 9] = [0x00, 0x01, 0x02, 0x08, 0x09, 0x0A, 0x10, 0x11, 0x12];
ROGUE
    if scan "$TMP"; then
        echo "  BROKEN: the gate PASSED a tree containing a second FM core."
        echo "  It would not have caught the thing it exists to catch."
        exit 1
    else
        echo "  RED as required"
    fi
    echo "[opl-core-gate] SELF-TEST PASSED (2/2): proven RED on a duplicate, GREEN without."
    exit 0
fi

if scan "$ROOT"; then
    echo "[opl-core-gate] OK: exactly one FM synthesis core."
    exit 0
else
    exit 1
fi
