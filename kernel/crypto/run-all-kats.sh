#!/usr/bin/env bash
#
# run-all-kats.sh - run every known-answer oracle in the kernel tree.
#
# WHY THIS EXISTS, and it is the whole point:
# On 2026-09-29 there were five carefully written KAT oracles in this directory
# (pbkdf2_shadow, ed25519_verify, rsa_verify, chacha20_poly1305, ecdsa_verify)
# and NOTHING IN THE REPOSITORY INVOKED ANY OF THEM. Not the kernel Makefile,
# not build-golden.sh, not the pre-land gates. They were written, they were
# correct, and they were dead.
#
# The cost was not hypothetical. ecdsa_verify_kat.sh had been BROKEN for an
# unknown length of time: rsa.c gained `#include "fs/bootlog.h"` and the oracle,
# which stages only crypto/ into a temp dir, died with
# "fatal error: fs/bootlog.h: No such file or directory". Nobody noticed,
# because noticing requires running it. That is #514 in a different costume: an
# inert gate and no gate are indistinguishable from the outside.
#
# So: one runner, wired into mwt's pre-land gates. A new oracle dropped in this
# directory is picked up automatically by the glob, which means the next person
# to write one does not also have to remember to wire it.
#
# Exit 0 iff EVERY oracle passes. Any failure names the oracle and its first
# error line. Both *_kat.sh and *-oracle.sh are found; see the glob below.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
KROOT=$(cd "$HERE/.." && pwd)
cd "$KROOT" || exit 1

# #kfirewall: THE GLOB NOW COVERS THE WHOLE KERNEL TREE, NOT JUST crypto/.
#
# The header above argues that a new oracle dropped in "this directory" is picked
# up automatically "which means the next person to write one does not also have
# to remember to wire it". That argument is right and the glob was one directory
# too narrow to honour it. MEASURED 2026-10-01:
# kernel/net/tls/ecdhkat_x25519_kat.sh, a real GREEN/RED oracle for the x25519
# ladder (RFC 7748 5.2 + 6.1 with negatives), was invoked by NOTHING. grep found
# it referenced only in CHANGELOG.md and blame.md prose. It PASSES, and it had
# been sitting dead outside the glob of the very runner written to stop oracles
# being dead. That is this file's own founding fault, one directory over.
#
# So the search is the kernel tree. An oracle anywhere under kernel/ named
# *_kat.sh runs, which is what the header always claimed.
shopt -s nullglob
# BOTH NAMING CONVENTIONS, because the tree uses both (#deadtests, 2026-10-02).
# This glob was '*_kat.sh' only. The header above says a new oracle "is picked up
# automatically by the glob, which means the next person to write one does not
# also have to remember to wire it" - and then two oracles were written with a
# DIFFERENT NAME and were therefore not picked up by anything at all:
#
#   kernel/fs/ext2oob/ext2oob-oracle.sh   #476 ext2 directory-walk over-read
#   kernel/gui/imgoob/imgoob-oracle.sh    BMP decode over-read
#
# Both are real GREEN/RED oracles over SHIPPING parsers of untrusted input, both
# prove their guards are load-bearing, both PASS, and between them they had ZERO
# callers of any kind. That is this file's own documented failure mode repeating
# one level up: the glob was not one directory too narrow this time, it was one
# SUFFIX too narrow. Measured cost of adding them: 3s for the pair.
mapfile -t KATS < <(find . -type f \( -name '*_kat.sh' -o -name '*-oracle.sh' \) \
                    | sed 's|^\./||' | sort)
if [ ${#KATS[@]} -eq 0 ]; then
    echo "run-all-kats: no *_kat.sh or *-oracle.sh found anywhere under $KROOT"
    echo "That is itself suspicious: this tree is where the oracles live."
    exit 1
fi

rc=0
pass=0
fail=0
for k in "${KATS[@]}"; do
    printf '  %-42s ' "$k"
    log=$(mktemp)
    start=$(date +%s)
    # Each oracle resolves its sources relative to ITSELF (crypto ones stage
    # crypto/, the tls one seds a span out of tls13.c, the net one includes
    # ../rustkern/fwfilter.rs), so bash is given the path and they each compute
    # their own HERE. Nothing here cd's on their behalf.
    if timeout 600 bash "$k" >"$log" 2>&1; then
        printf 'PASS  (%ss)\n' "$(( $(date +%s) - start ))"
        pass=$((pass + 1))
    else
        # KAT_DIAG: CAPTURE THE STATUS FIRST. Inside the else branch of an `if
        # cmd; then`, $? is still the condition's status, and the very next
        # command would overwrite it.
        krc=$?
        printf 'FAIL  (%ss)\n' "$(( $(date +%s) - start ))"

        # ------------------------------------------------------------------
        # KAT_DIAG. WHY THIS IS NOT A KEYWORD GREP ANY MORE.
        #
        # This used to be the single line:
        #
        #     grep -iE 'fatal|error|FAIL' "$log" | head -3
        #
        # which prints NOTHING when the failing oracle's output happens to
        # contain none of those three words. "No output" then looks exactly
        # like "the log is empty", and an empty log looks exactly like "the
        # command never ran". MEASURED CONSEQUENCE, 2026-10-03: six `mwt land`
        # attempts were refused by this gate, each reporting four KATs as
        # `FAIL (0s)` with nothing underneath. That silence was read as
        # evidence the scripts died before producing output, which sent a whole
        # investigation after `mktemp` and resource exhaustion. The scripts may
        # have been talking the entire time; this line was not listening for
        # the right words.
        #
        # A failure report must distinguish FOUR different things that all
        # printed identically before:
        #   1. the oracle ran and said why it failed   -> show its tail
        #   2. the oracle ran and said nothing         -> say the log is EMPTY
        #   3. the log file is missing entirely        -> say so: that means
        #      the redirect failed, i.e. mktemp gave us nothing usable, and
        #      the oracle NEVER RAN
        #   4. the oracle was killed by the timeout    -> rc 124, say it
        # Conflating any of these with "the oracle failed" is how a gate sends
        # someone to read a diff that was never at fault.
        # ------------------------------------------------------------------
        if [ ! -e "$log" ]; then
            printf '        KAT_DIAG: rc=%s and THE LOG FILE DOES NOT EXIST (%s).\n' "$krc" "$log"
            printf '        KAT_DIAG: so the redirect failed and this oracle NEVER RAN.\n'
            printf '        KAT_DIAG: that is an ENVIRONMENT fault (mktemp/disk/descriptors),\n'
            printf '        KAT_DIAG: not a fault in the code under test. mktemp rc check:\n'
            if _probe=$(mktemp 2>&1); then
                printf '        KAT_DIAG:   mktemp works right now (%s); the failure was transient.\n' "$_probe"
                rm -f "$_probe"
            else
                printf '        KAT_DIAG:   mktemp IS FAILING NOW: %s\n' "$_probe"
            fi
        elif [ ! -s "$log" ]; then
            printf '        KAT_DIAG: rc=%s and the log is EMPTY (0 bytes).\n' "$krc"
            printf '        KAT_DIAG: the oracle produced no output at all before failing.\n'
            [ "$krc" = 124 ] && printf '        KAT_DIAG: rc=124 is the 600s timeout killing it.\n'
        else
            printf '        KAT_DIAG: rc=%s, log %s bytes. LAST 12 LINES, unfiltered:\n' \
                   "$krc" "$(wc -c < "$log" | tr -d ' ')"
            tail -12 "$log" | sed 's/^/        | /'
            # The keyword lines too, when there are any: they are usually the
            # point, they are just no longer the ONLY thing reported.
            if grep -qiE 'fatal|error|FAIL' "$log"; then
                printf '        KAT_DIAG: keyword hits:\n'
                grep -iE 'fatal|error|FAIL' "$log" | head -3 | sed 's/^/        > /'
            fi
        fi
        fail=$((fail + 1))
        rc=1
    fi
    rm -f "$log"
done

echo
echo "run-all-kats: $pass passed, $fail failed, ${#KATS[@]} total"
exit $rc
