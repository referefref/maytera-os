// apps/fbprobe/main.c - #745 (task #59): NEGATIVE CONTROL for the framebuffer
// ownership latch (kernel/gui/fbown.h, rustkern/fbown.rs).
//
// WHY THIS EXISTS. The latch's whole job is to refuse sys_fb_map() to any Ring 3
// process that is not the compositor the kernel launched. The obvious wrong fix
// for "Switch User leaves a dead pid latched" is to loosen that refusal, and
// "the guard never fires" and "the guard is not there any more" produce exactly
// the same evidence: a desktop that works. So the fix ships with a probe that
// makes the guard FIRE, on purpose, from a real Ring 3 process.
//
// Launch it from /CONFIG/AUTORUN.CFG on a THROWAWAY verification VM (the kernel
// starts it ~14 s after boot, i.e. well after the compositor has claimed). It
// asks for the framebuffer several times, a few seconds apart, and reports each
// answer. The ONLY correct transcript is "REFUSED" every time, matched on the
// serial console by one "[FB] ERROR: Non-compositor process tried to map
// framebuffer" line per attempt. A single MAPPED line is a security regression.
//
// It also spans a Switch User cycle if you drive one while it runs, which
// covers the interesting new state: after the compositor exits, the claim is
// RELEASED AND DISARMED, so an unrelated process must still be refused rather
// than inheriting the freed screen.
//
// Output goes through ONE write() per line, not printf(): on the no-PTY
// autorun path this kernel emits one serial syslog record per write(), and a
// per-character printf turns the transcript into thousands of single-character
// records (blame.md, "Three deployment facts that each cost a boot").

#include "../../libc/maytera.h"

#define ATTEMPTS 24
#define GAP_MS   5000

static void say(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    sys_write(1, s, n);
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    char line[160];
    int mapped = 0, refused = 0;

    say("FBPROBE: start; a non-compositor process asking for the framebuffer\n");

    for (int i = 0; i < ATTEMPTS; i++) {
        long r = fb_map();
        if (r == 0) {
            refused++;
            snprintf(line, sizeof(line),
                     "FBPROBE: attempt %d/%d -> REFUSED (correct)\n",
                     i + 1, ATTEMPTS);
        } else {
            mapped++;
            snprintf(line, sizeof(line),
                     "FBPROBE: attempt %d/%d -> MAPPED at 0x%lx (SECURITY REGRESSION)\n",
                     i + 1, ATTEMPTS, (unsigned long)r);
        }
        say(line);
        sys_sleep(GAP_MS);
    }

    snprintf(line, sizeof(line),
             "FBPROBE: RESULT refused=%d mapped=%d verdict=%s\n",
             refused, mapped, mapped == 0 ? "PASS" : "FAIL");
    say(line);
    return mapped == 0 ? 0 : 1;
}
