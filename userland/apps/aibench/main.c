// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// aibench - the AI loop efficiency BASELINE harness (#469m).
//   docs/AI_LOOP_EFFICIENCY_BASELINE.md is the write-up this produces the
//   numbers for.
//
// WHY THIS EXISTS. The question "would a typed-decision model make AI-driven
// app interaction more efficient" cannot be answered without knowing where the
// wall clock and the tokens go today, and before #469m the client recorded
// NEITHER. This harness drives the REAL shared aiclient path (the same
// aiclient_ask() the AI Chat app and the msh "?" prefix use, with the same
// system prompt, the same tool list and the same capability gate) over a fixed
// task list, so /CONFIG/AIMETRIC.LOG ends up holding a per-phase record of a
// realistic app-driving session rather than a synthetic ping.
//
// Launched from /CONFIG/AUTORUN.CFG on a THROWAWAY verification VM, never a
// shipping golden (build/unshipped-apps.list). It needs a reachable endpoint
// and a provisioned key; with neither it says so and exits rather than
// reporting a baseline it did not take.
//
// IT NEVER PRINTS THE KEY. It never reads AISVC.CFG itself: aiclient owns the
// key and this harness only sees aiclient_have_key(), a boolean. Nothing here
// prints a request header, a request body or an endpoint URL.
//
// OUTPUT DISCIPLINE: everything goes to fd 2. An autorun-spawned process's
// fd 1 does not reliably reach the serial console (see apps/lockprobe/main.c).

#include "../../libc/syscall.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"
#include "../../libc/aiclient.h"

// MEASURED: one turn against the live endpoint costs tens of seconds on this
// stack (section 4 of docs/AI_LOOP_EFFICIENCY_BASELINE.md), so 8 tasks x 2 is
// already a ~30 minute run. Raise it with argv[1] when you want more samples.
#define REPS_DEFAULT 2

static void e(const char *s) {
    unsigned long n = 0; while (s[n]) n++;
    if (n) sys_write(2, s, n);
}

static void ef(const char *fmt, long a, long b, long c) {
    char buf[256];
    snprintf(buf, sizeof(buf), fmt, a, b, c);
    e(buf);
}

// The task list. Each entry is a natural-language request a user would
// plausibly type, chosen so the model must make the SAME KIND of decision the
// real loop makes: pick a tool from the closed list, and for an app action
// pick a verb from the app's declared contract.
//
// The Studio group is the tier2wire-proven verb set (invert / grayscale /
// add_layer / screen_check): those are the four the live-action wire was
// demonstrated on, so they are the honest thing to benchmark. The multi-step
// entries force at least two tool calls in one turn, which is where the
// per-step cost compounds and where a cheaper decision would matter most.
typedef struct { const char *group; const char *prompt; } task_t;

static const task_t g_tasks[] = {
  // --- single-step app action: the four proven Studio verbs -----------------
  { "studio",    "Invert the colours of the image open in Maytera Studio." },
  { "studio",    "Make the picture in the paint app black and white." },
  { "studio",    "Add a new layer to the image in Studio." },
  { "studio",    "Check what the Studio canvas looks like right now." },
  // --- single-step OS tool: the tool-choice decision ------------------------
  { "ostool",    "How much free disk space is there?" },
  { "ostool",    "What colour theme am I using?" },
  // --- multi-step: two decisions in one turn --------------------------------
  { "multistep", "Invert the image in Studio and then add a new layer on top." },
  { "multistep", "Tell me the current theme and how much disk space is left." },
};
#define NTASKS ((int)(sizeof(g_tasks) / sizeof(g_tasks[0])))

int main(int argc, char **argv) {
    int reps = REPS_DEFAULT;
    if (argc >= 2) {
        int v = 0; const char *p = argv[1];
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); p++; }
        if (v > 0 && v <= 50) reps = v;
    }

    e("\n[AIBENCH] ===== AI loop efficiency baseline (#469m) =====\n");

    if (!aiclient_init() || !aiclient_have_key()) {
        // Fail loudly. A baseline taken with no key is a table of network
        // errors, and reporting it as a baseline is exactly the habit this
        // project's honesty rule exists to stop.
        e("[AIBENCH] NO API KEY for this user. The per-user AISVC.CFG was not\n"
          "[AIBENCH] provisioned on this boot, so there is nothing to measure.\n"
          "[AIBENCH] RESULT: NO BASELINE TAKEN.\n");
        return 1;
    }
    e(aiclient_have_tools()
        ? "[AIBENCH] tool index loaded from /AITOOLS/INDEX.yaml\n"
        : "[AIBENCH] WARNING: no tool index; the model sees an empty tool list\n");

    // Studio must be RUNNING for the live-action path to be the one exercised;
    // without it app.action falls back to a throwaway spawn, which is a
    // different (and cheaper) measurement wearing the same name.
    int pid = sys_spawn("/APPS/PAINT");
    ef("[AIBENCH] spawned /APPS/PAINT pid=%ld; waiting for its event loop\n",
       (long)pid, 0, 0);
    sys_sleep(9000);

    ef("[AIBENCH] %ld tasks x %ld repetitions = %ld turns\n",
       (long)NTASKS, (long)reps, (long)(NTASKS * reps));
    e("[AIBENCH] per-phase timings land in /CONFIG/AIMETRIC.LOG\n");

    static char answer[8192];
    int turn = 0;
    for (int r = 0; r < reps; r++) {
        for (int i = 0; i < NTASKS; i++) {
            turn++;
            char hdr[256];
            snprintf(hdr, sizeof(hdr), "\n[AIBENCH] --- turn %d/%d [%s] %s\n",
                     turn, NTASKS * reps, g_tasks[i].group, g_tasks[i].prompt);
            e(hdr);
            answer[0] = 0;
            int rc = aiclient_ask(g_tasks[i].prompt, answer, (int)sizeof(answer), 1);
            char tail[160];
            snprintf(tail, sizeof(tail), "[AIBENCH] turn %d rc=%d\n", turn, rc);
            e(tail);
            // The answer text itself is the model's, not ours, and can be
            // long; print a bounded prefix so the serial transcript stays
            // readable and a runaway generation cannot flood it.
            if (answer[0]) {
                char pre[420];
                int o = 0;
                for (int k = 0; answer[k] && o < (int)sizeof(pre) - 2; k++) {
                    char c = answer[k];
                    pre[o++] = (c == '\n' || c == '\r') ? ' ' : c;
                }
                pre[o] = 0;
                e("[AIBENCH] answer: "); e(pre); e("\n");
            }
            // A brief gap between turns so a rate limit on the endpoint shows
            // up as an HTTP 429 record rather than as a pile of retries that
            // would misattribute OUR pacing to the model's latency.
            sys_sleep(1500);
        }
    }

    long retries = 0, aok = 0, averb = 0, aargs = 0, amal = 0;
    aiclient_metrics_summary(&retries, &aok, &averb, &aargs, &amal);
    e("\n[AIBENCH] ===== run summary =====\n");
    ef("[AIBENCH] turns=%ld  transport retries (beyond the first attempt)=%ld\n",
       (long)turn, retries, 0);
    ef("[AIBENCH] actions: ok=%ld  bad-verb=%ld  bad-args=%ld\n", aok, averb, aargs);
    ef("[AIBENCH] replies that meant an ACTION but did not parse=%ld\n", amal, 0, 0);
    e("[AIBENCH] full per-phase records: /CONFIG/AIMETRIC.LOG\n");
    e("[AIBENCH] ===== done =====\n");
    return 0;
}
