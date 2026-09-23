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

// (dosconc5, Stage 5) The pids of the Ring-3 DOS hosts THIS layer started, one
// per slot, 0 = free. WAS a single `int g_dosroute_r3_pid`: one guest at a time.
// It is now a BOUNDED set so up to DOSROUTE_MAX_RING3 guests run concurrently,
// the owner's requirement. Each Ring-3 host is a separate process, so its whole
// DOS guest state (g_dos, MCB/XMS/EMS, PSP/DTA, the file-handle table, the LDT
// and conventional-memory arena, diskimg drive maps, dospath CWD, doslinger's
// CLOSE_REQ/HOLD_UNTIL_MS) is a PRIVATE per-process copy: isolation by address
// space, not by refactor. The only genuinely shared host singletons (the raw
// keyboard tap and the OPL2/FM sink) follow compositor FOCUS, not this table
// (rustkern/rawsc.rs's single subscriber and the dos_inst focus-owner token).
//
// Each pid is re-validated through proc_get() rather than trusted, because slots
// are REUSED (a stale pid could match an unrelated process that landed in the
// same slot later); checking the state as well as the pointer is what makes a
// zombie DOSUSER (exited, not yet reaped) count as gone rather than as live.
//
// IDENTITY-PRESERVING: with a lone guest exactly one slot is ever occupied and
// the gate refuses nothing it refused before; the relaxation is observable only
// once a SECOND guest is asked for while a first is live.
#define DOSROUTE_MAX_RING3 2
static volatile int g_dosroute_r3_pid[DOSROUTE_MAX_RING3];

// ---------------------------------------------------------------------------
// Cross-path arbitration.
//
// g_dos_busy has always guarded the in-kernel path against a second in-kernel
// guest (that path shares one static g_dos and the raw-scancode ISR tap, so it
// stays one-at-a-time until the heap-allocated-per-guest switch, plan Stage 6).
// The RING-3 path is different: each host is its own address space, so N of them
// are isolated by construction, and Stage 5 lets up to DOSROUTE_MAX_RING3 run at
// once. What must still hold across BOTH paths is that an in-kernel guest and any
// Ring-3 guest do not run together (they would fight over the ISR scancode tap
// and host focus), which is why dosroute_launch() still refuses an in-kernel
// launch while any Ring-3 host lives and refuses a Ring-3 launch while the
// in-kernel guest is busy.
//
// dosroute_ring3_count() returns the number of LIVE Ring-3 hosts, reaping any
// slot whose pid is gone. A pid is re-validated through proc_get() rather than
// trusted, because slots are REUSED: a stale pid could match an unrelated
// process that landed in the same slot later. Checking the state as well as the
// pointer is what makes a zombie DOSUSER (exited, not yet reaped) count as gone
// rather than as live. The scan is a bounded fixed loop over DOSROUTE_MAX_RING3,
// not a wait on a condition (#426 / concurrency-lint).
// ---------------------------------------------------------------------------
static int dosroute_ring3_count(void) {
    int n = 0;
    for (int i = 0; i < DOSROUTE_MAX_RING3; i++) {
        int pid = g_dosroute_r3_pid[i];
        if (pid <= 0) continue;
        process_t *p = proc_get((uint32_t)pid);
        if (!p || p->state == PROC_STATE_UNUSED || p->state == PROC_STATE_ZOMBIE) {
            g_dosroute_r3_pid[i] = 0;   // reap a gone host so its slot frees up
            continue;
        }
        n++;
    }
    return n;
}

// Record a freshly-started Ring-3 host pid in a free slot. Reaps first, so a
// slot left by an exited host is reused. Returns 0 on success, -1 if the table
// is full (the caller has already refused in that case: belt-and-braces).
static int dosroute_ring3_record(int pid) {
    if (pid <= 0) return -1;
    (void)dosroute_ring3_count();   // reap dead slots before searching for a free one
    for (int i = 0; i < DOSROUTE_MAX_RING3; i++) {
        if (g_dosroute_r3_pid[i] <= 0) { g_dosroute_r3_pid[i] = pid; return 0; }
    }
    return -1;
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

    // (dosconc5) Cap concurrent Ring-3 hosts. Enforced HERE, the one definition of
    // "spawn a Ring-3 DOS host", so both the routed SYS_DOS_RUN path and the
    // /CONFIG/DOSRING3.CFG boot harness respect it. With a lone guest live is 0.
    {
        int live = dosroute_ring3_count();
        if (live >= DOSROUTE_MAX_RING3) {
            kprintf("[DOSROUTE] refusing Ring-3 host '%s': %d already running (max %d)\n",
                    line, live, DOSROUTE_MAX_RING3);
            return -1;
        }
    }

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
    dosroute_ring3_record(pid);
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

    // (dosconc5, Stage 5) Cross-path arbitration, now that up to DOSROUTE_MAX_RING3
    // Ring-3 hosts may coexist (each its own address space). See dosroute_ring3_count.
    int r3live = dosroute_ring3_count();

    if (route != DOSROUTE_RING3) {
        // In-kernel route. It shares the single static g_dos and the ISR scancode
        // tap, so it must not run beside a Ring-3 DOS host. UNCHANGED rule: with a
        // lone guest r3live is 0 and this is exactly the old path.
        if (r3live > 0) {
            kprintf("[DOSROUTE] busy: %d Ring-3 DOS host(s) running; in-kernel launch refused\n",
                    r3live);
            return -1;
        }
        return dos_launch(line);
    }

    // Ring-3 route. Refuse beside an in-kernel guest (mirror of the check above),
    // and cap concurrent Ring-3 hosts at DOSROUTE_MAX_RING3. dosroute_spawn_ring3
    // enforces the cap too (one definition), so a config-driven launch respects it
    // as well; this check keeps the routed path's diagnostic specific.
    if (dos_is_busy()) {
        kprintf("[DOSROUTE] busy: an in-kernel DOS task is already running\n");
        return -1;
    }
    if (r3live >= DOSROUTE_MAX_RING3) {
        kprintf("[DOSROUTE] busy: %d Ring-3 DOS host(s) already running (max %d)\n",
                r3live, DOSROUTE_MAX_RING3);
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
