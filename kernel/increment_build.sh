#!/bin/bash
# increment_build.sh - build-number sanity check (#514).
#
# HISTORY / WHY THIS IS NOT AN INCREMENT. This script was literally
#
#     #!/bin/bash
#     exit 0
#
# for months while `make all` depended on a target called `increment_build` and
# CLAUDE.md told everyone to "always increment MAYTERA_BUILD_NUMBER before
# building". A guardrail named after a job it does not do is worse than no
# guardrail, because people stop checking (#514).
#
# The build number is NOT this script's job any more, and must not be: the
# golden is built ONLY by build/build-golden.sh, which computes the next number
# as max(state file, version.h) + 1 and seds it into the checkout it just made
# with `git archive`. If this script also incremented, every golden would jump
# by two and every developer `make` would dirty a tracked file.
#
# So this does the thing it CAN honestly do, and it can FAIL: prove version.h
# actually carries a parseable MAYTERA_BUILD_NUMBER. A missing or malformed
# define is silent otherwise (the desktop version string just renders wrong),
# and it would defeat invariant-gate.sh's "strictly greater than the previous
# build" check, which reads the number out of the image.
#
# NO em-dashes anywhere in this file (project style rule).

set -u

VERSION_H="$(dirname "$0")/version.h"

if [ ! -f "$VERSION_H" ]; then
    echo "increment_build: ERROR: $VERSION_H not found." >&2
    echo "  The build number lives there and invariant-gate.sh reads it out of" >&2
    echo "  the image to enforce monotonicity. Refusing to build blind." >&2
    exit 1
fi

BUILD=$(grep -oE '^[[:space:]]*#define[[:space:]]+MAYTERA_BUILD_NUMBER[[:space:]]+[0-9]+' \
        "$VERSION_H" | grep -oE '[0-9]+$' | head -1)

if [ -z "${BUILD:-}" ]; then
    echo "increment_build: ERROR: no parseable MAYTERA_BUILD_NUMBER in $VERSION_H." >&2
    echo "  Expected a line like: #define MAYTERA_BUILD_NUMBER 981" >&2
    echo "  build-golden.sh seds this value and invariant-gate.sh enforces that" >&2
    echo "  it strictly increases; an unparseable define breaks both." >&2
    exit 1
fi

echo "increment_build: version.h build number = $BUILD (the increment itself is"
echo "                 owned by build/build-golden.sh, not by make)."
exit 0
