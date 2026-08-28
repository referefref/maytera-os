#!/usr/bin/env python3
"""
#232: enumerate and triage every strncpy() call site in the kernel tree.

Triage rule (from #231): the pre-fix primitive writes n+1 bytes whenever the
source is NUL-terminated within n, i.e. for every normal string. So a site is
UNCONDITIONALLY IMMUNE when n is one less than the destination's capacity,
because the extra byte lands on the byte the caller already reserved.

This prints the sites where n is NOT of that shape; those are the ones that
must be traced by hand against the actual layout (objdump/nm, not source order).

Run:  python3 232-triage.py <kernel-dir>
"""
import re, os, sys

root = sys.argv[1] if len(sys.argv) > 1 else "."
EXCLUDE_FILES = {"./string.c", "./string.h"}
EXCLUDE_DIRS = ("tests/strncpy_231", "tests/strncpy_232", "compat")

def split_args(body):
    args, depth, cur = [], 0, ""
    for ch in body:
        if ch == "(": depth += 1
        elif ch == ")": depth -= 1
        if ch == "," and depth == 0:
            args.append(cur.strip()); cur = ""
        else:
            cur += ch
    args.append(cur.strip())
    return args

os.chdir(root)
hits = []
for dp, ds, fs in os.walk("."):
    if any(x in dp for x in EXCLUDE_DIRS):
        continue
    for f in sorted(fs):
        if not f.endswith((".c", ".h")):
            continue
        p = os.path.join(dp, f)
        if p in EXCLUDE_FILES:
            continue
        txt = open(p, errors="replace").read()
        for m in re.finditer(r"(?<![A-Za-z0-9_])strncpy\s*\(", txt):
            i, depth = m.end(), 1
            while i < len(txt) and depth:
                if txt[i] == "(": depth += 1
                elif txt[i] == ")": depth -= 1
                i += 1
            call = " ".join(txt[m.start():i].split())
            ln = txt.count("\n", 0, m.start()) + 1
            args = split_args(call[call.index("(") + 1:-1])
            hits.append((p, ln, args[0], args[-1]))

immune = [h for h in hits if re.search(r"-\s*1\s*$", h[3])]
manual = [h for h in hits if not re.search(r"-\s*1\s*$", h[3])]

print("total kernel strncpy call sites : %d" % len(hits))
print("  n == <capacity> - 1 (immune)  : %d" % len(immune))
print("  needs hand tracing            : %d" % len(manual))
print()
for p, ln, d, n in manual:
    print("  %-28s dest=%-46s n=%s" % ("%s:%d" % (p, ln), d, n))
