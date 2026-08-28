#!/bin/sh
# Capture the reference TUI traces for the damage harness. See README.
# Run inside userland/apps/terminal/tests/traces.
set -e
cd "$(dirname "$0")"

python3 - <<'PY' > /tmp/vifile.txt
for i in range(1, 401):
    print("line %-4d the quick brown fox jumps over the lazy dog and back again" % i)
PY

# vi: jump to the end, back to the top, then 40 page-downs and 40 page-ups.
# Ctrl-F / Ctrl-B rather than arrow keys, because a PAGE is the interesting
# case: it changes every row, which is the WORST case for damage tracking and
# therefore the one worth measuring rather than the one worth avoiding.
cat > /tmp/vidrive.py <<'PY'
# Keystrokes must arrive with REAL GAPS. Piping them all at once puts 42 KB of
# vi output into a single 10 ms bucket, which measures one enormous frame and
# tells you nothing about what a keypress costs.
import sys, time
def k(s, d=0.12):
    sys.stdout.write(s); sys.stdout.flush(); time.sleep(d)
time.sleep(0.6)
k("G"); k("1G")
for _ in range(30): k("\x06")      # Ctrl-F: page down (every row changes)
for _ in range(30): k("\x02")      # Ctrl-B: page up
for _ in range(20): k("j")          # single-line scroll at the bottom
for _ in range(20): k("k")
k(":q!\n")
PY

rm -f vi.trace vi.tm top.trace top.tm claudeish.trace claudeish.tm
python3 /tmp/vidrive.py | COLUMNS=80 LINES=24 script -q -c "vi /tmp/vifile.txt" \
    --log-out vi.trace --log-timing vi.tm > /dev/null 2>&1 || true
COLUMNS=80 LINES=24 script -q -c "top -d 0.25 -n 20" \
    --log-out top.trace --log-timing top.tm > /dev/null 2>&1 || true
script -q -c "python3 claudeish.py" \
    --log-out claudeish.trace --log-timing claudeish.tm > /dev/null 2>&1 || true

for t in vi top claudeish; do
    [ -f "$t.tm" ] && python3 mkframes.py "$t.tm" > "$t.frames"
done
ls -l *.trace *.frames
