// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// pcmown - artefact-level verification of PCM STREAM OWNERSHIP (#217).
//
// WHY IT EXISTS. #217's subject is that there is exactly ONE Ring-3 PCM stream
// (drivers/audio_pcm.c, PCM_MAX_STREAMS == 1) and that apps used to hold it for
// their whole lifetime even while silent, so nothing else on the machine could
// play. A kernel self-test cannot prove that: the question is about TWO Ring-3
// processes contending for one slot, about what the SECOND one is told, and
// about what happens when the FIRST one dies without closing. Only real
// processes making real syscalls can answer it, which is what this does.
//
// A GUARD THAT NEVER FIRES AND AN ABSENT GUARD LOOK IDENTICAL. Every check here
// therefore has a RED arm: the test deliberately CREATES the contended state and
// FAILS if the kernel does not refuse, before it ever checks that a fix works.
//
// Modes (argv):
//   pcmown hold <ms>   child: open the PCM stream, write NOTHING, sit idle for
//                      <ms>, close, exit. This is exactly the shape of the
//                      reported fault: a live holder that is making no sound.
//   pcmown midi <ms>   spawn /APPS/MIDIPLAY, wait <ms>, then try to open.
//   pcmown             run the whole suite (the AUTORUN.CFG entry point).
//
// Launched via /CONFIG/AUTORUN.CFG on a throwaway VM. Verification aid, not a
// shipped app.
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "syscall.h"
#include "fcntl.h"

#define SIGKILL 9

#define RATE     44100
#define CHANNELS 2
#define FMT_S16  0x0002     /* AUDIO_FORMAT_S16_LE, drivers/audio.h:21. NOT 1: the kernel
                             refuses any other value with EINVAL(-1), which is
                             how the first run of this harness correctly aborted
                             rather than reporting a false pass. */

/* Mirrors drivers/audio_pcm.h. Negative, so a handle is never confusable. */
#define PCM_EBUSY  (-2)

static int g_fail = 0;
static int g_checks = 0;

static void check(const char *name, int ok, const char *detail, long v) {
    g_checks++;
    if (!ok) g_fail++;
    printf("[PCMOWN] %-4s %-34s %s (rc=%ld)\n",
           ok ? "PASS" : "FAIL", name, detail ? detail : "", v);
}

static int pcm_open(void)  { return (int)syscall3(SYS_AUDIO_PCM_OPEN,
                                                  RATE, CHANNELS, FMT_S16); }
static int pcm_close(int h){ return (int)syscall1(SYS_AUDIO_PCM_CLOSE, h); }
static int kill9(int pid)  { return (int)syscall2(SYS_KILL, pid, SIGKILL); }

/* Try to open, bounded, until it succeeds or the budget runs out.
   Returns the handle, or the LAST error seen. Never spins: sys_sleep()
   is the kernel's timed sleep, not a yield loop. */
static int open_until_ok(int budget_ms, int *waited_ms) {
    int waited = 0;
    for (;;) {
        int h = pcm_open();
        if (h >= 1) { if (waited_ms) *waited_ms = waited; return h; }
        if (waited >= budget_ms) { if (waited_ms) *waited_ms = waited; return h; }
        sys_sleep(50);
        waited += 50;
    }
}

/* THE HOLDER'S READINESS FLAG, AND WHY IT IS A FILE AND NOT A PROBE.
   The first version of this harness established "the child has the stream" by
   opening the stream itself in a retry loop until it got EBUSY. That is a probe
   that PERTURBS the thing being measured, and it did: on the very first run the
   parent's probe held the one slot at the instant the child called open, the
   CHILD got the -2, the child exited, and T3/T4 then measured a machine with no
   holder at all. The kernel was behaving perfectly and the harness reported a
   FAIL. (blame.md, #745 task 36: a leak test that measures "can I still get a
   slot" is measuring three subsystems.)
   The parent now touches the PCM stream exactly ONCE in T3, and learns the child
   is ready out-of-band. */
#define HELD_FLAG "/PCMHELD.TXT"

static int held_flag_present(void) {
    int fd = open(HELD_FLAG, O_RDONLY);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

/* Wait, bounded, for the child to announce it holds the stream. Returns 1 if it
   did. NEVER calls pcm_open(). */
static int wait_for_holder(int budget_ms) {
    int waited = 0;
    for (;;) {
        if (held_flag_present()) return 1;
        if (waited >= budget_ms) return 0;
        sys_sleep(50);
        waited += 50;
    }
}

static int mode_hold(int ms) {
    int h = pcm_open();
    printf("[PCMOWN-HOLD] pcm_open -> %d\n", h);
    if (h < 1) return 1;
    /* Announce, so the parent never has to probe the stream to find out. */
    int fd = open(HELD_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, "held\n", 5); close(fd); }
    /* STREAM, so the kill in T4 lands MID-PLAYBACK and not on an idle pump.
       Silence is the right payload: the point is that the DMA engine is running
       and the kernel pump is actively consuming, not what it sounds like.
       This loop does not spin: sys_audio_pcm_write() blocks on the ring's
       wait queue once the ring is full, which is what paces it. */
    static int16_t blk[1024 * CHANNELS];   /* .bss, not the stack */
    long blocks = ((long)ms * RATE) / (1000L * 1024L);
    if (blocks < 1) blocks = 1;
    printf("[PCMOWN-HOLD] streaming %ld blocks (handle %d)\n", blocks, h);
    for (long i = 0; i < blocks; i++) {
        int w = (int)syscall3(SYS_AUDIO_PCM_WRITE, h, (long)blk, 1024);
        if (w < 0) { printf("[PCMOWN-HOLD] write -> %d, stopping\n", w); break; }
    }
    pcm_close(h);
    printf("[PCMOWN-HOLD] closed\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "hold") == 0) {
        return mode_hold(argc >= 3 ? atoi(argv[2]) : 5000);
    }

    printf("\n[PCMOWN] #217 PCM stream ownership suite starting\n");
    unlink(HELD_FLAG);              /* a stale flag from a previous boot would
                                       make T3 believe a holder exists */

    /* ---- T1: baseline. Is there a stream to contend for at all? -------- */
    int h = pcm_open();
    check("T1 baseline open", h >= 1, "a sink exists and the slot is free", h);
    if (h < 1) {
        printf("[PCMOWN] no PCM sink on this machine; the rest of the suite "
               "cannot say anything. ABORTING (not a pass).\n");
        return 2;
    }
    int rc = pcm_close(h);
    check("T1 baseline close", rc == 0, "close joins the pump", rc);

    /* ---- T2: close-then-reopen, 20x. -----------------------------------
       Lazy open/close (#217's fix) makes "close, then open again a moment
       later" the NORMAL path, where before it never happened at all. If the
       teardown publishes "done" before it releases the slot, this is where it
       shows up, as an intermittent EBUSY against nobody.                    */
    int reopen_fail = 0, worst = 0;
    for (int i = 0; i < 20; i++) {
        int hh = pcm_open();
        if (hh < 1) { reopen_fail++; worst = hh; continue; }
        pcm_close(hh);
    }
    check("T2 close->reopen x20", reopen_fail == 0,
          reopen_fail ? "a close raced its own reopen" : "no self-inflicted EBUSY",
          reopen_fail ? worst : 0);

    /* ---- T3: THE REPORTED FAILURE. A silent holder locks the machine. --- */
    char ms[16];
    strcpy(ms, "9000");
    char *hargv[3]; hargv[0] = "/APPS/PCMOWN"; hargv[1] = "hold"; hargv[2] = ms;
    int child = sys_spawn_args("/APPS/PCMOWN", hargv, 3);
    check("T3 spawn silent holder", child > 0, "child pid", child);
    if (child <= 0) goto done;

    int ready = wait_for_holder(6000);
    check("T3 holder acquired the stream", ready,
          ready ? "child announced it holds the stream"
                : "the child never got the stream; T3/T4 below say NOTHING",
          ready);
    if (!ready) goto killchild;

    int busy = pcm_open();          /* EXACTLY ONE attempt, no probing */
    check("T3 second opener REFUSED", busy == PCM_EBUSY,
          busy == PCM_EBUSY
            ? "EBUSY while the holder is SILENT: this is #217"
            : "the kernel admitted a second opener into a 1-slot table",
          busy);
    if (busy >= 1) pcm_close(busy);

killchild:;

    /* ---- T4: release on death. ------------------------------------------
       #36/#37 found this exact class in the fetch-slot table: no release when
       the owner died, and six dead owners exhausted it until reboot. Kill the
       holder WITHOUT letting it close, and require the slot back.           */
    rc = kill9(child);
    check("T4 SIGKILL the holder MID-STREAM", rc == 0,
          rc == 0 ? "killed while its pump was actively consuming"
                  : "kill failed (-3 = ESRCH: the child had already exited, so "
                    "T4 below proves NOTHING)",
          rc);
    int waited = 0;
    h = open_until_ok(5000, &waited);
    check("T4 slot returns after death", h >= 1,
          h >= 1 ? "the dead owner's stream was reclaimed"
                 : "STREAM LEAKED: a dead process still owns the only stream",
          h >= 1 ? waited : h);
    if (h >= 1) pcm_close(h);

    /* ---- T5: THE ACTUAL SUBJECT OF #217. -------------------------------
       /APPS/MIDIPLAY, sitting at its opening screen having been asked to play
       NOTHING. Before #217 it opened the stream in main() and held it until it
       exited, so this open is refused. After #217 it opens on Play, so this
       open succeeds. Same harness, same machine, same sequence: the only
       difference is which MIDIPLAY binary is on the disk.                    */
    char *margv[1]; margv[0] = "/APPS/MIDIPLAY";
    int mp = sys_spawn_args("/APPS/MIDIPLAY", margv, 1);
    check("T5 spawn /APPS/MIDIPLAY", mp > 0, "midi player pid", mp);
    if (mp > 0) {
        sys_sleep(6000);            /* window up, demo loaded, sitting IDLE */
        int hm = pcm_open();
        printf("[PCMOWN] T5 with an IDLE /APPS/MIDIPLAY running, pcm_open -> %d\n", hm);
        printf("[PCMOWN] T5 %s: %s\n",
               hm >= 1 ? "STREAM AVAILABLE" : "STREAM HELD",
               hm >= 1 ? "the idle player is NOT holding the stream (#217 fixed)"
                       : "the idle player IS holding the stream (#217 unfixed)");
        if (hm >= 1) pcm_close(hm);
        else if (hm != PCM_EBUSY)
            printf("[PCMOWN] T5 note: refusal was %d, not EBUSY(-2)\n", hm);
        /* Leave the player running: the GUI half of the verification drives it
           with real keystrokes and watches the kernel's own [PCM] lines. */
        printf("[PCMOWN] T5 leaving /APPS/MIDIPLAY (pid %d) running for the GUI phase\n", mp);
    }

done:
    printf("[PCMOWN] SUITE: %d checks, %d FAILING\n", g_checks, g_fail);
    if (g_fail == 0) printf("[PCMOWN] SUITE PASS\n");
    else             printf("[PCMOWN] SUITE FAIL <<<<\n");
    return g_fail ? 1 : 0;
}
