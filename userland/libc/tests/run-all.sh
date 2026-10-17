#!/usr/bin/env bash
# userland/libc/tests/run-all.sh - #deadtests: RUN THE LIBC HOST TESTS.
#
# WHY THIS EXISTS. MEASURED on dev @b387c216, 2026-10-02: this directory holds
# SIXTEEN host unit tests of shipping libc code, and the number of them that any
# automation in this repository invoked was ZERO. Not one was named by
# build-golden.sh, by mwt's pre-land gate list, by a Makefile, or by any other
# script. Running them by hand that day:
#
#   6 of 16 were RED, and not one of the six was red about the code it tests.
#
#     run_aidev / run_aieject / run_aifs / run_escrow
#         "BROKEN - positive arm did not build": aiclient.c and aicap.c had
#         grown fourteen new environment dependencies and the harnesses never
#         followed. Fixed by ai_env_stubs.h.
#     run_posixhdr
#         did not link at all (utime.c reached for a syscall3() the harness
#         never defined) AND carried three assertions that features "must never
#         claim success" - flock (#404), utime (#115), nftw FTW_MOUNT (#120) -
#         every one of which had since been implemented.
#     run_spawn
#         exited 2 on "syscall1 issued unexpected syscall 394": posix_spawn()
#         moved to SYS_SPAWN_ENV and the kernel model did not.
#     run_exifdate (flaky, ~2 in 5)
#         AddressSanitizer died in its own startup because the host has
#         vm.mmap_rnd_bits=32. Nothing to do with the EXIF parser.
#
# EVERY ONE OF THOSE IS A REASON NOBODY WIRED THEM IN, and that is the whole
# lesson: "nobody wired it" and "it could not be wired" look identical from
# outside and have opposite remedies. So this runner exists, and it is wired.
#
# DISCOVERY IS BY GLOB, NOT BY A LIST. A list of test scripts would drift the
# way run-all-kats.sh's glob drifted (one directory too narrow for a year) and
# the way the census's SCAN_ROOTS drifted. Every run*.sh in this directory runs.
# Adding a test file is enough; forgetting to mention it here is impossible.
#
# EXEMPTIONS CARRY A REASON OR THEY ARE NOT EXEMPTIONS. EXPECT-RED.policy, TSV,
# lists any test that is KNOWN red, with a REQUIRED reason printed on every run.
# A reasonless line fails. A line naming a file that does not exist fails. And a
# listed test that starts PASSING fails too, so an exemption cannot outlive the
# thing it exempted. That is the concurrency-lint allowlist discipline.
#
# COST, MEASURED rather than asserted (the build container, 2026-10-02): 80s for all sixteen.
# run_malloc 31s, run_cpython_alloc 18s and run_malloc_scale 11s are 60s of it.
# That is the price of knowing the libc unit tests still pass, paid once per
# land instead of never.
#
# Exit 0 = every test behaved as declared. Non-zero otherwise.
set -u

# --_suite is the internal entry point --self-test uses so that the self-test
# exercises the REAL run_suite() instead of a copy of its logic. It is handled
# before argument parsing because it is not a user-facing flag.
if [ "${1:-}" = "--_suite" ]; then
    _SUITE_DIR="${2:?--_suite needs a directory}"
    _SUITE_MODE=1
else
    _SUITE_MODE=0
fi

SELFTEST=0
VERBOSE=0
[ "$_SUITE_MODE" = "1" ] && set --
for a in "$@"; do
    case "$a" in
        --self-test) SELFTEST=1 ;;
        --verbose|-v) VERBOSE=1 ;;
        *) echo "run-all.sh: unknown argument '$a'" >&2; exit 2 ;;
    esac
done

POLICY_NAME="EXPECT-RED.policy"

run_suite() {   # $1 = directory to run in. Echoes a report; returns 0/1.
    local dir="$1"
    local policy="$dir/$POLICY_NAME"
    local rc_all=0 nrun=0 npass=0 nfail=0 nred=0
    local -a scripts=()
    local s b

    for s in "$dir"/run*.sh; do
        [ -f "$s" ] || continue
        b=$(basename "$s")
        [ "$b" = "run-all.sh" ] && continue
        scripts+=( "$b" )
    done
    if [ "${#scripts[@]}" -eq 0 ]; then
        echo "run-all: NO run*.sh found in $dir. A runner that runs nothing"
        echo "         reports 'nothing failed' about nothing, so this is a failure."
        return 1
    fi

    # --- the policy, validated BEFORE anything runs -------------------------
    declare -A EXPECT_RED=()
    if [ -f "$policy" ]; then
        local line path reason
        while IFS= read -r line; do
            case "$line" in ''|'#'*) continue ;; esac
            path=${line%%$'\t'*}
            reason=${line#*$'\t'}
            if [ "$reason" = "$line" ] || [ -z "${reason//[[:space:]]/}" ]; then
                echo "run-all: FAIL  $POLICY_NAME line for '$path' has NO REASON."
                echo "         An exemption without a justification is not an exemption, it is a hole."
                rc_all=1; continue
            fi
            if [ ! -f "$dir/$path" ]; then
                echo "run-all: FAIL  $POLICY_NAME names '$path', which does not exist."
                echo "         A stale exemption outliving the thing it exempted is how an allowlist turns into a lie."
                rc_all=1; continue
            fi
            EXPECT_RED["$path"]="$reason"
        done < "$policy"
    fi

    # --- run them -----------------------------------------------------------
    local out rc t0 t1
    out=$(mktemp) || return 1
    for b in "${scripts[@]}"; do
        t0=$(date +%s)
        ( cd "$dir" && timeout 600 bash "./$b" ) >"$out" 2>&1 </dev/null
        rc=$?
        t1=$(date +%s)
        nrun=$((nrun + 1))
        if [ -n "${EXPECT_RED[$b]+set}" ]; then
            if [ "$rc" -eq 0 ]; then
                printf '  FAIL      %-24s PASSES but %s says it is known-red.\n' "$b" "$POLICY_NAME"
                printf '            Delete its line. A stale exemption hides the next real failure.\n'
                nfail=$((nfail + 1)); rc_all=1
            else
                printf '  KNOWN-RED %-24s rc=%-3s %3ds\n' "$b" "$rc" "$((t1 - t0))"
                printf '            %s\n' "${EXPECT_RED[$b]}"
                nred=$((nred + 1))
            fi
        elif [ "$rc" -eq 0 ]; then
            printf '  PASS      %-24s %3ds\n' "$b" "$((t1 - t0))"
            npass=$((npass + 1))
            [ "$VERBOSE" -eq 1 ] && sed 's/^/            /' "$out"
        else
            printf '  FAIL      %-24s rc=%-3s %3ds\n' "$b" "$rc" "$((t1 - t0))"
            grep -iE 'FAIL|BROKEN|undefined reference|error:' "$out" | head -8 | sed 's/^/            /'
            nfail=$((nfail + 1)); rc_all=1
        fi
    done
    rm -f "$out"

    echo "run-all: ran=$nrun passed=$npass failed=$nfail known-red=$nred"
    [ "$rc_all" -eq 0 ] && echo "run-all: PASS" || echo "run-all: FAIL"
    return $rc_all
}

# ------------------------------------------------------------------ self-test
if [ "$SELFTEST" -eq 1 ]; then
    SELF=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
    base=$(mktemp -d) || exit 2
    trap 'rm -rf "$base"' EXIT
    n=0; bad=0
    mk() {   # $1 = dir, $2 = name, $3 = exit code
        mkdir -p "$1"; printf '#!/bin/sh\necho "%s ran"\nexit %s\n' "$2" "$3" > "$1/$2"
        chmod +x "$1/$2"
    }
    arm() {  # $1 = want rc, $2 = dir, $3 = description
        n=$((n + 1))
        local got
        bash "$SELF" --_suite "$2" >/dev/null 2>&1
        got=$?
        if [ "$got" -eq "$1" ]; then printf '  ok   %s (rc=%s)\n' "$3" "$got"
        else printf '  FAIL %s: got rc=%s want %s\n' "$3" "$got" "$1"; bad=1; fi
    }

    # 1. all green -> 0
    d="$base/green"; mk "$d" run_a.sh 0; mk "$d" run_b.sh 0
    arm 0 "$d" "every test passing is rc 0"

    # 2. one red and NOT exempted -> 1. (If this went green the runner would be
    #    decoration: that is the whole failure mode this file is about.)
    d="$base/red"; mk "$d" run_a.sh 0; mk "$d" run_b.sh 1
    arm 1 "$d" "an unexempted failure is rc 1"

    # 3. the same red, exempted WITH a reason -> 0
    d="$base/exempt"; mk "$d" run_a.sh 0; mk "$d" run_b.sh 1
    printf 'run_b.sh\ta real stated reason\n' > "$d/EXPECT-RED.policy"
    arm 0 "$d" "a known-red with a reason is rc 0"

    # 4. an exemption with NO reason -> 1
    d="$base/noreason"; mk "$d" run_a.sh 1
    printf 'run_a.sh\n' > "$d/EXPECT-RED.policy"
    arm 1 "$d" "an exemption with no reason is rc 1"

    # 5. a STALE exemption (the listed test now PASSES) -> 1
    d="$base/stale"; mk "$d" run_a.sh 0
    printf 'run_a.sh\tsupposedly red\n' > "$d/EXPECT-RED.policy"
    arm 1 "$d" "an exemption whose test now passes is rc 1"

    # 6. an exemption naming a file that does not exist -> 1
    d="$base/ghost"; mk "$d" run_a.sh 0
    printf 'run_gone.sh\tdeleted months ago\n' > "$d/EXPECT-RED.policy"
    arm 1 "$d" "an exemption naming no file is rc 1"

    # 7. an EMPTY directory -> 1. "0 failed" over an empty set is the TESTINPUT
    #    fault: a gate handed nothing recording its correct answer about nothing.
    d="$base/empty"; mkdir -p "$d"
    arm 1 "$d" "a directory with no run*.sh is rc 1"

    echo
    if [ "$bad" -eq 0 ]; then echo "run-all.sh --self-test: PASS ($n/$n)"; exit 0
    else echo "run-all.sh --self-test: FAIL"; exit 1; fi
fi

if [ "$_SUITE_MODE" = "1" ]; then
    run_suite "$_SUITE_DIR"
    exit $?
fi

HERE=$(cd "$(dirname "$0")" && pwd)
echo "== libc host unit tests ($HERE)"
run_suite "$HERE"
exit $?
