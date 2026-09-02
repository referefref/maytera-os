// dosring3.c - boot gate that launches the RING-3 DOS host (#DOSRING3).
//
// WHAT THIS IS FOR. #67/#168 measured that with a DOS guest the Big Kernel Lock
// is held 93% of wall clock and 89% of that is the DOS interpreter simply BEING
// a Ring-0 kernel thread, which caps SMP at 1.07x. Moving the interpreter to
// Ring 3 lifts that ceiling to roughly 4.5x. /APPS/DOSUSER is that Ring-3 host:
// it compiles the kernel's OWN DOS sources, unmodified, into a user process.
//
// THE IN-KERNEL PATH IS UNTOUCHED AND REMAINS THE DEFAULT. This gate exists so
// the SAME guest can be run down BOTH paths on the same build and their results
// compared, which is the only way to know the port preserves behaviour.
// dos/dosexec.c prints "%u instructions retired, %u frames presented" on both,
// and that is the differential oracle.
//
//   /CONFIG/DOSRUN.CFG     -> in-kernel DOS path   (existing, unchanged)
//   /CONFIG/DOSRING3.CFG   -> Ring-3 host          (this file)
//
// Both files hold a single guest path line. Absent, this is a no-op.
//
// WHY C AND NOT RUST, since the standing rule is Rust for new kernel code: this
// is glue with no logic of its own. It reads a config line and calls three
// existing C APIs (fat_read_file, proc_as_session, proc_create_user_as). A Rust
// version would be an FFI wrapper around those three calls containing nothing
// that could be verified independently, which buys no safety and adds a
// language boundary in a boot path. The DOS ring buffer and arming policy that
// DID have logic worth isolating (rustkern/rawsc.rs) are in Rust.
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "process.h"
#include "dosroute.h"

extern fat_fs_t g_fat_fs;   // main.c; the single mounted FAT root

#define DOSRING3_CFG  "/CONFIG/DOSRING3.CFG"
#define DOSRING3_APP  "/APPS/DOSUSER"

static void dosring3_entry(void *arg) {
    (void)arg;
    // Same 3-second settle as the in-kernel harness: the compositor has to be
    // up before a window can be created, and this deliberately mirrors
    // dos_deferred_entry() so the two arms of the differential start from the
    // same machine state rather than from two different ones.
    proc_sleep(3000);

    // Same bounded retry as the in-kernel harness, and for the same measured
    // reason (#385): a FAT config read can transiently fail in a post-boot proc
    // context while widgets are touching the filesystem. Bounded, so an absent
    // file is a no-op rather than a hang.
    void *cfg = 0; uint32_t sz = 0;
    int attempts = 0;
    for (attempts = 0; attempts < 10; attempts++) {
        cfg = fat_read_file(&g_fat_fs, DOSRING3_CFG, &sz);
        if (cfg && sz > 0) break;
        if (cfg) { kfree(cfg); cfg = 0; }
        proc_sleep(500);
    }
    if (!cfg || sz == 0) {
        if (cfg) kfree(cfg);
        // SAY SO. This used to `return` silently, and the first live run could
        // not tell "not armed" (the normal state) from "armed but the read
        // failed" - the two outcomes that need completely different responses.
        // One line per boot is the price of never having to guess again; the
        // audio and DOS subsystems print exactly this kind of line for exactly
        // this reason ("a run with no [PCM] open line means nothing ever asked").
        kprintf("[DOSRING3] %s absent after %d attempt(s); Ring-3 DOS host NOT "
                "started (this is the normal state - the in-kernel DOS path is "
                "the default)\n", DOSRING3_CFG, attempts);
        return;
    }
    kprintf("[DOSRING3] %s read OK on attempt %d (%u bytes)\n",
            DOSRING3_CFG, attempts, sz);

    char guest[256];
    int n = 0;
    const char *p = (const char *)cfg;
    for (uint32_t i = 0; i < sz && n < (int)sizeof(guest) - 1; i++) {
        char ch = p[i];
        if (ch == '\r' || ch == '\n') break;
        guest[n++] = ch;
    }
    guest[n] = '\0';
    while (n > 0 && (guest[n - 1] == ' ' || guest[n - 1] == '\t')) guest[--n] = '\0';
    kfree(cfg);
    if (n == 0) {
        kprintf("[DOSRING3] %s is empty; nothing to launch\n", DOSRING3_CFG);
        return;
    }

    // (#67/#168) ONE definition of "spawn /APPS/DOSUSER", shared with the
    // routed SYS_DOS_RUN path (proc/dosroute.c). This used to be an inline
    // copy of the read-ELF / build-argv / proc_create_user_as sequence, and a
    // second copy of it was about to be written for the routing layer. #172 is
    // the ticket that exists because two copies of "what a DOS launch means"
    // drifted apart; this is the same shape, so it gets the same answer.
    //
    // #692: spawn AS THE SESSION, exactly as the desktop launcher does. This is
    // the whole security argument for the Ring-3 move: the guest runs with the
    // logged-in user's real credentials, so every file it opens is checked by
    // the kernel against those credentials. It cannot reach anything its user
    // could not reach from a shell, whatever the interpreter does, which is a
    // stronger boundary than a Ring-0 DOS thread policing itself with a
    // synthesised uid. The ROUTED path passes proc_as_caller() instead, because
    // there a real Ring-3 process asked for the guest.
    kprintf("[DOSRING3] launching Ring-3 DOS host: %s '%s'\n",
            DOSRING3_APP, guest);
    bootlog_write("[DOSRING3] launching %s '%s'", DOSRING3_APP, guest);

    int pid = dosroute_spawn_ring3(guest, proc_as_session());

    if (pid > 0) {
        kprintf("[DOSRING3] Ring-3 DOS host started as pid %d\n", pid);
        bootlog_write("[DOSRING3] host pid %d", pid);

        // THE INPUT-PATH INSTRUMENT. Without this the raw-scancode path is a
        // black box with four independent places to fail (the keyboard never
        // produced a byte; the tap was not armed so the byte was dropped; the
        // app never drained; the app drained but the guest ignored it) and the
        // symptom for all four is identical: the guest sits on "press any key".
        // Guessing between them costs a VM run each time. These counters
        // already existed in rustkern/rawsc.rs and NOTHING PRINTED THEM, which
        // is the same "collected and thrown away at the last step" fault #118
        // recorded for the BKL site table.
        //
        // irq= is the kernel's own count of scancode BYTES seen at the hardware
        // level, so a run where irq stays 0 says the KEY never arrived and no
        // amount of app-side debugging will help.
        {
            extern void rawsc_census_rs(uint32_t *pushed, uint32_t *dropped,
                                        uint32_t *drained);
            extern int  rawsc_armed_rs(void);
            extern volatile uint64_t g_kbd_irq_scancodes;
            // Six samples over 30 s, not 24 over two minutes: enough to see
            // the input path come alive on a machine the owner is driving by
            // hand, without holding a thread and a serial line for the whole
            // session.
            for (int i = 0; i < 6; i++) {
                proc_sleep(5000);
                uint32_t pu = 0, dr = 0, dn = 0;
                rawsc_census_rs(&pu, &dr, &dn);
                kprintf("[RAWSC] armed=%d pushed=%u dropped=%u drained=%u "
                        "irq_bytes=%llu\n",
                        rawsc_armed_rs(), pu, dr, dn,
                        (unsigned long long)g_kbd_irq_scancodes);
            }
        }
    } else {
        // dosroute_spawn_ring3() has already printed WHICH of the two failure
        // modes this was (host image missing, vs the process refused), so this
        // line states only that the harness arm did not start.
        kprintf("[DOSRING3] the Ring-3 differential arm did NOT start (rc=%d)\n", pid);
        bootlog_write("[DOSRING3] spawn REFUSED rc=%d", pid);
    }
}

// Called from main.c beside dos_start_deferred_launch(). No-op unless
// /CONFIG/DOSRING3.CFG exists.
void dosring3_start_deferred_launch(void) {
    proc_create("dosring3", dosring3_entry, 0, 0);
}
