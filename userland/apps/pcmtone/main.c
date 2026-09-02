// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// pcmtone - prove that audio is NOT fixed to a single task, with a WAVEFORM.
//
// WHY IT EXISTS. #217's pcmown answers the OWNERSHIP question (who is refused,
// what happens when a holder dies) and answers it in RETURN CODES. That is the
// right instrument for that question and the wrong one for this one. The
// question here is whether two independent producers are actually AUDIBLE AT
// THE SAME TIME, and a return code cannot answer it: this project has already
// shipped a complete DOS audio stack whose honest summary was that nobody had
// ever heard any audio, and the owner found that out by trying it. So every
// mode here emits a PURE TONE AT A KNOWN FREQUENCY, and the acceptance test is
// an FFT of a host-captured WAV showing both peaks in one recording. A log line
// saying "wrote 44100 frames" is not evidence that a sound was made.
//
// TWO TONES, NOT TWO COPIES OF ONE. 440 Hz and 1200 Hz are not adjacent, are
// not harmonically related (1200/440 = 2.727...), and neither is a harmonic of
// the other, so neither tone can be mistaken for the other's overtone in the
// spectrum. If the mixer summed only one of them, or summed one twice, the FFT
// says which.
//
// Modes (argv):
//   pcmtone tone   <hz> <ms>   open, write a <hz> sine for <ms>, close.
//                              Single thread: the case that ships today.
//   pcmtone thread <hz> <ms>   THE #181 THREAD-GROUP CASE. Thread A opens and
//                              then EXITS; thread B writes; thread C closes.
//   pcmtone                    the suite: spawn two concurrent `tone` children
//                              at 440 Hz and 1200 Hz, then a `thread` child.
//                              This is the /CONFIG/AUTORUN.CFG entry point.
//
// Verification aid, not a shipped app.
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "syscall.h"
#include "pthread.h"

#define RATE     44100
#define CHANNELS 2
// AUDIO_FORMAT_S16_LE, drivers/audio.h:21. NOT 1: the kernel refuses any other
// value with EINVAL(-1). pcmown's header comment records that mistake costing a
// whole harness run, so it is restated here rather than rediscovered.
#define FMT_S16  0x0002

// One period of a sine, 64 steps, amplitude 9000 of 32767. Deliberately well
// below full scale: four of these summed by the mixer must not clip, because a
// clipped sum spreads energy across the spectrum and would blur the very peaks
// this test reads.
static const short SINE64[64] = {
    0, 882, 1756, 2613, 3444, 4243, 5000, 5710,
    6364, 6957, 7483, 7937, 8315, 8612, 8827, 8957,
    9000, 8957, 8827, 8612, 8315, 7937, 7483, 6957,
    6364, 5710, 5000, 4243, 3444, 2613, 1756, 882,
    0, -882, -1756, -2613, -3444, -4243, -5000, -5710,
    -6364, -6957, -7483, -7937, -8315, -8612, -8827, -8957,
    -9000, -8957, -8827, -8612, -8315, -7937, -7483, -6957,
    -6364, -5710, -5000, -4243, -3444, -2613, -1756, -882
};

#define CHUNK_FRAMES 1024
static short g_buf[CHUNK_FRAMES * CHANNELS];

// Q16 phase into SINE64. Integer only: this app must not depend on the libm
// that a freestanding userland does not have, and an integer accumulator has no
// drift argument to make.
static unsigned int g_phase;
static unsigned int g_step;

static void tone_fill(void) {
    for (int i = 0; i < CHUNK_FRAMES; i++) {
        short s = SINE64[(g_phase >> 16) & 63];
        g_buf[i * 2 + 0] = s;
        g_buf[i * 2 + 1] = s;
        g_phase += g_step;
    }
}

static int pcm_open(void)  { return (int)syscall3(SYS_AUDIO_PCM_OPEN,
                                                  RATE, CHANNELS, FMT_S16); }
static int pcm_close(int h){ return (int)syscall1(SYS_AUDIO_PCM_CLOSE, h); }
static int pcm_write(int h, const short *b, unsigned n) {
    return (int)syscall3(SYS_AUDIO_PCM_WRITE, h, (long)b, (long)n);
}

// Write `ms` worth of tone through handle `h`. Returns frames accepted, or the
// first negative error. A short write is NOT an error (the ring is finite and
// the kernel is entitled to take fewer frames), but a NEGATIVE return is, and
// it is reported rather than retried: a handle that has gone invalid underneath
// a live process is precisely the #181 fault this test exists to catch.
static long tone_play(int h, int hz, int ms) {
    g_phase = 0;
    g_step  = (unsigned int)(((unsigned long long)hz * 64ULL << 16) / RATE);
    long want = (long)RATE * ms / 1000;
    long done = 0;
    while (done < want) {
        unsigned n = CHUNK_FRAMES;
        if ((long)n > want - done) n = (unsigned)(want - done);
        tone_fill();
        int w = pcm_write(h, g_buf, n);
        if (w < 0) {
            printf("[PCMTONE] WRITE FAILED rc=%d after %ld frames (hz=%d)\n",
                   w, done, hz);
            return w;
        }
        if (w == 0) { sys_sleep(2); continue; }
        done += w;
    }
    return done;
}

// ---------------------------------------------------------------------------
// The #181 thread-group case, as three DIFFERENT threads.
//
// Thread A opens and then RETURNS, i.e. that thread exits while the process
// lives on. Before #181 this was fatal twice over: pcm_lookup() scoped the
// stream to the opening THREAD so B's write was refused outright, and
// audio_pcm_proc_exit() tore the stream down on A's exit so there was nothing
// left to write to even if the lookup had passed. Either failure produces
// silence and neither names threads as the cause, which is why this is a
// separate mode with its own audible tone rather than a return-code check
// bolted onto the suite.
static volatile int g_h = -1;
static volatile int g_hz, g_ms;
static volatile long g_written = -1;

static void *th_open(void *arg)  { (void)arg; g_h = pcm_open();  return 0; }
static void *th_write(void *arg) { (void)arg; g_written = tone_play(g_h, g_hz, g_ms); return 0; }
static void *th_close(void *arg) { (void)arg; g_h = pcm_close(g_h); return 0; }

static int mode_thread(int hz, int ms) {
    pthread_t a, b, c;
    g_hz = hz; g_ms = ms;

    if (pthread_create(&a, 0, th_open, 0) != 0) {
        printf("[PCMTONE] FAIL pthread_create(open)\n"); return 1;
    }
    // Join A: it has EXITED. The stream must survive its exit.
    pthread_join(a, 0);
    printf("[PCMTONE] thread A opened handle=%d then exited\n", g_h);
    if (g_h < 1) {
        printf("[PCMTONE] FAIL open on thread A rc=%d\n", g_h);
        return 1;
    }

    if (pthread_create(&b, 0, th_write, 0) != 0) {
        printf("[PCMTONE] FAIL pthread_create(write)\n"); return 1;
    }
    pthread_join(b, 0);
    printf("[PCMTONE] thread B wrote %ld frames at %d Hz\n", g_written, hz);

    if (pthread_create(&c, 0, th_close, 0) != 0) {
        printf("[PCMTONE] FAIL pthread_create(close)\n"); return 1;
    }
    pthread_join(c, 0);
    printf("[PCMTONE] thread C closed rc=%d\n", g_h);

    int ok = (g_written > 0);
    printf("[PCMTONE] %s open(A)/write(B)/close(C) across three threads\n",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
static int mode_tone(int hz, int ms) {
    int h = pcm_open();
    printf("[PCMTONE] pid=%d open(%d Hz) handle=%d\n", getpid(), hz, h);
    if (h < 1) { printf("[PCMTONE] FAIL open rc=%d hz=%d\n", h, hz); return 1; }
    long n = tone_play(h, hz, ms);
    int rc = pcm_close(h);
    printf("[PCMTONE] pid=%d hz=%d wrote=%ld close=%d\n", getpid(), hz, n, rc);
    return (n > 0) ? 0 : 1;
}

// A tiny decimal parser: the freestanding libc's atoi is available, but the
// argv contract here is fixed and a bad argument must be LOUD rather than
// silently become 0 Hz, which would emit DC and look like a dead mixer.
static int arg_int(const char *s, int fallback) {
    if (!s || !*s) return fallback;
    int v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return fallback;
        v = v * 10 + (*p - '0');
    }
    return v;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "tone") == 0)
        return mode_tone(arg_int(argc > 2 ? argv[2] : 0, 440),
                         arg_int(argc > 3 ? argv[3] : 0, 3000));

    if (argc >= 2 && strcmp(argv[1], "thread") == 0)
        return mode_thread(arg_int(argc > 2 ? argv[2] : 0, 660),
                           arg_int(argc > 3 ? argv[3] : 0, 3000));

    // ---- the suite ----
    printf("[PCMTONE] ===== BEGIN =====\n");

    // Two INDEPENDENT PROCESSES, started as close together as spawn allows,
    // each playing a different tone for the same 4 s. If audio is fixed to one
    // task, at most one of these frequencies appears in the capture.
    char *a1[] = { "/APPS/PCMTONE", "tone", "440",  "4000" };
    char *a2[] = { "/APPS/PCMTONE", "tone", "1200", "4000" };
    int p1 = sys_spawn_args("/APPS/PCMTONE", a1, 4);
    int p2 = sys_spawn_args("/APPS/PCMTONE", a2, 4);
    printf("[PCMTONE] spawned 440Hz pid=%d and 1200Hz pid=%d\n", p1, p2);
    if (p1 <= 0 || p2 <= 0) printf("[PCMTONE] FAIL spawn\n");

    // Outlast both children, then leave a clear gap of silence before the
    // thread-group tone so the two events are separable in the recording by
    // TIME as well as by frequency.
    sys_sleep(6000);
    printf("[PCMTONE] ----- concurrent pair done, gap -----\n");
    sys_sleep(1000);

    // 660 Hz: distinct from both of the above, so the third event cannot be
    // confused with a straggler from the pair.
    int rc = mode_thread(660, 3000);

    sys_sleep(500);
    printf("[PCMTONE] ===== END rc=%d =====\n", rc);
    return rc;
}
