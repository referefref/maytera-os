// security/selftest_registry.c - #PERMSKIP: the C face of the not-run register.
//
// The storage and the string handling live in Rust (rustkern/selftestreg.rs);
// CLAUDE.md's rule is that new kernel code is Rust unless there is a stated
// performance reason, and there is none here. What stays in C is exactly the
// part that cannot be anything else: the calls into the kernel's variadic
// bootlog_write() formatter, which has no Rust binding in this tree.
//
// The wording of the loud line lives HERE, in one place, rather than at each
// call site. That is deliberate. The bug being fixed is that each caller chose
// its own wording and its own sink, and the wording it chose ("SKIP ...") read
// like a pass and the sink it chose (kprintf) is invisible on the two machines
// that matter. One wording and one sink means a future caller cannot get
// either wrong by being terse.

#include "selftest_registry.h"
#include "../fs/bootlog.h"

// rustkern/selftestreg.rs
extern int  selftest_notrun_rs(const char *name, const char *reason);
extern void selftest_ran_rs(const char *name);
extern unsigned int selftest_notrun_count_rs(void);
extern unsigned int selftest_notrun_overflow_rs(void);
extern unsigned int selftest_ran_count_rs(void);
extern int  selftest_notrun_entry_rs(unsigned int idx,
                                     char *name_out, unsigned int name_cap,
                                     char *reason_out, unsigned int reason_cap);

void selftest_ran(const char *group) {
    // The name is not decoration: it RETRACTS any earlier not-run entry for the
    // same group. A group that declined at one point in the boot and ran later
    // (perms/traversal during the OOBE bootstrap session, then again at the
    // first real login) must not be reported as not-run at the end of it.
    selftest_ran_rs(group);
}

void selftest_notrun(const char *group, const char *reason) {
    int stored = selftest_notrun_rs(group, reason);
    // "***" and "DID NOT RUN" so that a reader scanning a 383 KB /BOOTLOG.TXT
    // sees the difference between this and a pass without reading the sentence.
    bootlog_write("[SELFTEST] *** DID NOT RUN: %s *** %s",
                  group ? group : "?", reason ? reason : "(no reason given)");
    if (!stored) {
        bootlog_write("[SELFTEST] (register full; this group is counted but "
                      "not named in the summary)");
    }
}

void selftest_notrun_report(void) {
    unsigned int nr = selftest_notrun_count_rs();
    unsigned int ov = selftest_notrun_overflow_rs();
    unsigned int ran = selftest_ran_count_rs();

    if (nr == 0 && ov == 0) {
        bootlog_write("[SELFTEST] %u group(s) ran, 0 declined.", ran);
        return;
    }
    bootlog_write("[SELFTEST] *** %u GROUP(S) DID NOT RUN *** (%u ran%s). "
                  "Something this kernel is supposed to verify about ITSELF "
                  "was not verified on this boot:",
                  nr + ov, ran,
                  ov ? ", register overflowed" : "");
    for (unsigned int i = 0; i < nr; i++) {
        char name[48], reason[160];
        if (selftest_notrun_entry_rs(i, name, sizeof(name),
                                     reason, sizeof(reason)) != 0) continue;
        bootlog_write("[SELFTEST]   %u/%u %s: %s", i + 1, nr, name, reason);
    }
}
