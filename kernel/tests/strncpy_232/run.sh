#!/bin/bash
# #232: two-arm execution proof for proc/services.c:209.
# Arm A = frozen pre-#231 strncpy (must show exec[] corrupted).
# Arm B = live fixed strncpy (must show exec[] intact).
set -u
cd "$(dirname "$0")"
FL="-O2 -fstack-protector-strong -Wall -Wextra"
echo "===== ARM A: PRE-#231 primitive (expected RED) ====="
gcc $FL -o /dev/shm/svc_prefix svc_layout_test.c arm_prefix.c || exit 2
/dev/shm/svc_prefix; A=$?
echo
echo "===== ARM B: LIVE primitive (expected GREEN) ====="
gcc $FL -o /dev/shm/svc_live svc_layout_test.c arm_live.c || exit 2
/dev/shm/svc_live; B=$?
echo
if [ $A -ne 0 ] && [ $B -eq 0 ]; then echo "GATE OK: red on pre-fix, green on live"; exit 0; fi
echo "GATE INCONCLUSIVE: A=$A B=$B"; exit 1
