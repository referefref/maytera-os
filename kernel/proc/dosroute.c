// dosroute.c - C glue for the DOS kernel-or-Ring-3 routing decision (#67/#168).
//
// WHY C AND NOT RUST, since the standing rule is Rust for new kernel code, and
// justified the same way proc/dosring3.c justifies itself: this file has NO
// logic of its own. Every decision it makes it asks rustkern/dospolicy.rs for;
// what is left is fat_read_file, proc_create_user_as, and printing. A Rust
// version would be an FFI wrapper around three C calls with nothing in it that
// could be verified independently. The part that HAS logic worth isolating -
// parsing untrusted config bytes into fixed buffers and matching a path against
// them - is already in Rust, and its self-test has a deliberate RED arm.
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../dos/dosexec.h"
#include "process.h"
#include "dosroute.h"

extern fat_fs_t g_fat_fs;   // main.c; the single mounted root

#define DOSROUTE_CFG  "/CONFIG/DOSROUTE.CFG"
#define DOSROUTE_APP  "/APPS/DOSUSER"

// The pid of the Ring-3 DOS host THIS layer started, or 0. See
// dosroute_ring3_live() for why a bare pid is enough here and why it is
// re-validated rather than trusted.
static volatile int g_dosroute_r3_pid = 0;

// ---------------------------------------------------------------------------
// Cross-path mutual exclusion.
//
// g_dos_busy has always guarded the in-kernel path against a second in-kernel
// guest. It knows nothing about Ring 3, so before this existed a Ring-3 launch
// could start while an in-kernel guest was running (and vice versa) and the two
// would fight over the raw-scancode tap and the host window. One guest at a
// time is the invariant the whole DOS subsystem is written against; routing
// must not quietly break it by adding a second path that does not participate.
//
// A pid is re-validated through proc_get() rather than trusted, because slots
// are REUSED: a stale pid could match an unrelated process that landed in the
// same slot later. Checking the state as well as the pointer is what makes a
// zombie DOSUSER (exited, not yet reaped) count as gone rather than as live.
// ---------------------------------------------------------------------------
static int dosroute_ring3_live(void) {
    int pid = g_dosroute_r3_pid;
    if (pid <= 0) return 0;
    process_t *p = proc_get((uint32_t)pid);
    if (!p) { g_dosroute_r3_pid = 0; return 0; }
    if (p->state == PROC_STATE_UNUSED || p->state == PROC_STATE_ZOMBIE) {
        g_dosroute_r3_pid = 0;
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Policy load.
//
// RE-READ ON EVERY LAUNCH, not cached at boot. The owner edits this file to try
// one title on the other path and then launches it; a boot-cached policy would
// mean "I edited the config and nothing changed", which is the exact shape of
// bug that makes people conclude a feature does not work. The cost is one
// whole-file read of a file that is a few dozen bytes, once per DOS launch,
// next to loading and relocating a guest image.
//
// Every failure mode yields a ZEROED policy, which dospolicy.rs defines as a
// valid "everything in-kernel" policy. That is why there is no error path here:
// the absent-file case and the unreadable-file case are the shipping default.
// ---------------------------------------------------------------------------
static void dosroute_load(dos_policy_t *pol) {
    for (unsigned i = 0; i < sizeof(*pol); i++) ((uint8_t *)pol)[i] = 0;

    uint32_t sz = 0;
    void *cfg = fat_read_file(&g_fat_fs, DOSROUTE_CFG, &sz);
    if (!cfg || sz == 0) {
        if (cfg) kfree(cfg);
        return;   // absent == in-kernel. The normal, shipping state.
    }
    dospolicy_parse_rs((const uint8_t *)cfg, sz, pol);
    kfree(cfg);

    if (pol->n_bad > 0) {
        // SAY SO, ONCE, LOUDLY. A routing config with a typo in it must not
        // fail silently: a mistyped override is a rule the owner believes is in
        // force and is not, and the symptom (a guest on the wrong path) looks
        // identical to the feature being broken.
        kprintf("[DOSROUTE] %s: %d unusable line(s) IGNORED - check for typos\n",
                DOSROUTE_CFG, pol->n_bad);
        bootlog_write("[DOSROUTE] %s has %d bad line(s)", DOSROUTE_CFG, pol->n_bad);
    }
}

// ---------------------------------------------------------------------------
// ONE definition of "spawn /APPS/DOSUSER".
//
// Lifted out of dosring3.c so the routed launch and the DOSRING3.CFG
// differential harness cannot come to disagree about what starting the Ring-3
// host means. That is not hypothetical: #172 records the DOSRUN.CFG path and
// the syscall path drifting apart over what a launch LINE means, for exactly
// this reason - two copies of one rule.
//
// The identity is the caller\x27s to choose and cannot be omitted, which is the
// #692 contract: proc_as_caller() for a SYS_DOS_RUN launch (the guest runs as
// the Ring-3 process that asked for it), proc_as_session() for the boot
// harness (no Ring-3 caller exists, so it runs as the authenticated desktop
// session and NOT as root just because a kernel thread started it).
// ---------------------------------------------------------------------------
int dosroute_spawn_ring3(const char *line, proc_ident_t ident) {
    if (!line || !line[0]) return -1;

    uint32_t elf_sz = 0;
    void *elf = fat_read_file(&g_fat_fs, DOSROUTE_APP, &elf_sz);
    if (!elf || elf_sz == 0) {
        if (elf) kfree(elf);
        kprintf("[DOSROUTE] '%s' is missing or empty; cannot start the Ring-3 host\n",
                DOSROUTE_APP);
        return -1;
    }

    // argv[1] is the WHOLE launch line, tail included. DOSUSER splits it with
    // dos_run_line(), which uses the same dos_split_launch_line() rule the
    // in-kernel path uses, so a guest keeps its arguments on both paths.
    char *argv[3];
    argv[0] = (char *)DOSROUTE_APP;
    argv[1] = (char *)line;
    argv[2] = 0;

    int pid = proc_create_user_as("DOSUSER", elf, elf_sz, argv, NULL, ident);
    kfree(elf);
    if (pid <= 0) {
        kprintf("[DOSROUTE] proc_create_user_as refused the Ring-3 host (rc=%d)\n", pid);
        return -1;
    }
    g_dosroute_r3_pid = pid;
    return pid;
}

// ---------------------------------------------------------------------------
// THE routed launch. SYS_DOS_RUN calls this and nothing else.
// ---------------------------------------------------------------------------
int dosroute_launch(const char *line) {
    if (!line || !line[0]) return -1;

    // The routing decision is made on the PROGRAM half only. Matching the whole
    // line would make an override depend on the arguments a title happened to
    // be launched with, so `ring3=/DOS/ROGUE/ROGUE.EXE` would silently fail to
    // match `/DOS/ROGUE/ROGUE.EXE -s`. Split with the SHARED rule
    // (dos_launch_program_half), never a second copy of it.
    char prog[DOSPOL_PROG_CAP];
    dos_launch_program_half(line, prog, (int)sizeof(prog));
    if (!prog[0]) return -1;

    dos_policy_t *pol = (dos_policy_t *)kmalloc(sizeof(dos_policy_t));
    if (!pol) {
        // Out of memory deciding where to run: take the shipping default rather
        // than refusing the launch. Never fail a launch over the ROUTING.
        kprintf("[DOSROUTE] no memory for the policy; using the in-kernel default\n");
        return dos_launch(line);
    }
    dosroute_load(pol);

    int rule = -1;
    int route = dospolicy_route_rs(pol, (const uint8_t *)prog, &rule);

    // The program path is QUOTED, and that is not decoration: a launch line may
    // carry a command tail ('/DOS/STUNTS/LOAD.EXE /u MCGA'), so without
    // delimiters a reader cannot tell where the path ends. dos_launch_common()
    // quotes its own "[dos] launched '%s'" line for the same reason.
    //
    // Name the rule that decided, not just the verdict. A routing log line with
    // no provenance cannot answer the only question anyone asks of it: "why did
    // THIS guest go there?"
    char why[DOSPOL_PAT_CAP + 8];
    if (rule >= 0 && dospolicy_rule_text_rs(pol, rule, (uint8_t *)why, sizeof(why)) > 0) {
        kprintf("[DOSROUTE] '%s' -> %s (rule %d: %s=%s)\n", prog,
                route == DOSROUTE_RING3 ? "RING3" : "kernel", rule,
                pol->mode[rule] == DOSROUTE_RING3 ? "ring3" : "kernel", why);
    } else {
        kprintf("[DOSROUTE] '%s' -> %s (default=%s)\n", prog,
                route == DOSROUTE_RING3 ? "RING3" : "kernel",
                pol->default_ring3 ? "ring3" : "kernel");
    }
    kfree(pol);

    // One guest at a time, across BOTH paths (see dosroute_ring3_live).
    if (dosroute_ring3_live()) {
        kprintf("[DOSROUTE] busy: the Ring-3 DOS host (pid %d) is still running\n",
                g_dosroute_r3_pid);
        return -1;
    }

    if (route != DOSROUTE_RING3)
        return dos_launch(line);

    // Ring-3 route. Refuse rather than start a second guest beside an in-kernel
    // one; dos_launch() makes the mirror-image check for us on the other branch.
    if (dos_is_busy()) {
        kprintf("[DOSROUTE] busy: an in-kernel DOS task is already running\n");
        return -1;
    }

    int pid = dosroute_spawn_ring3(line, proc_as_caller());
    if (pid > 0) {
        kprintf("[DOSROUTE] Ring-3 DOS host started as pid %d for '%s'\n", pid, line);
        return 0;
    }

    // FAIL SAFE, AT LAUNCH TIME ONLY.
    //
    // The Ring-3 host could not be STARTED: /APPS/DOSUSER is missing, or the
    // process could not be created. Nothing has run, so falling back costs
    // nothing and the owner gets a working game instead of an error.
    //
    // There is deliberately NO fallback once the guest is running, and in
    // particular none on "it exited quickly". A guest that legitimately exits
    // fast would then be silently run a SECOND time, on a different path, with
    // whatever side effects it had already committed to its save files. That is
    // worse than the failure it would guard against, and it is unfalsifiable
    // from the outside: two runs look like one slow run.
    kprintf("[DOSROUTE] Ring-3 launch FAILED at start; falling back to the "
            "in-kernel path ONCE for '%s'\n", line);
    bootlog_write("[DOSROUTE] ring3 start failed, fell back in-kernel");
    return dos_launch(line);
}

// ---------------------------------------------------------------------------
// Boot self-test. Both arms, always.
//
// The GREEN arm alone proves nothing: "fails=0" from a suite whose checks
// cannot fail is indistinguishable from "fails=0" from a suite that works, and
// this tree has been burned by exactly that (blame.md, the BKL hold-sum
// self-test). The RED arm is a deliberately-wrong policy that MUST produce a
// non-zero count; if it ever reports 0, the self-test is the thing that is
// broken, not the code under it.
// ---------------------------------------------------------------------------
void dosroute_selftest(void) {
    uint32_t green = dospolicy_selftest_rs();
    uint32_t red   = dospolicy_selftest_red_rs();
    int ok = (green == 0) && (red > 0);
    kprintf("[DOSROUTE] policy self-test %s (green fails=%u must be 0, "
            "red fails=%u must be >0)\n", ok ? "PASS" : "FAIL", green, red);
    bootlog_write("[DOSROUTE] selftest %s green=%u red=%u",
                  ok ? "PASS" : "FAIL", green, red);
}
