// audio_pcm.c - Ring-3 PCM push (#426-clean). Phase 1 of the Ring-0 media exit.
// See audio_pcm.h for the design, the reuse argument, and the #426 wake table.

#include "audio_pcm.h"
#include "../security/validate.h"   // #503: U/S-bit validation, not an address range
#include "audio.h"
#include "usb_audio.h"
#include "hda.h"       // #190: hda_out_stopped(), hda_space_wq()
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sync/waitq.h"
#include "../proc/process.h"
#include "../security/uaccess_smap.h"  // #19/#645: AC bracket on the caller-buffer copy
#include "../fs/bootlog.h"             // #205: name the refusal where the owner can read it

// ============================================================================
// Tunables
// ============================================================================

// Ring capacity in frames. MUST be a power of two (the index math relies on
// masking, and the free-running r/w counters rely on well-defined uint32 wrap).
// 32768 frames = ~0.68 s @ 48 kHz, 256 KB at S16 stereo: deep enough that a
// userland decoder scheduled behind the compositor never underruns, small
// enough that a manual track-switch drops < 1 s of audio.
#define PCM_RING_FRAMES     32768u

// Frames the non-UAC pump moves per audio_write(). One HDA BDL buffer is
// comfortably larger than this; keeping it small bounds the latency between
// s->stop being set and the pump noticing it.
#define PCM_HDA_BATCH       1024u

// Hard backstop on a single stream, handed to uac_stream_source() exactly as
// audio_play_file_uac() does. A stream whose Ring-3 owner vanishes without
// closing (SIGKILL) self-terminates here rather than living forever.
#define PCM_MAX_STREAM_MS   (30u * 60u * 1000u)

// #205: HOW MANY THINGS MAY MAKE A SOUND AT ONCE.
//
// This was 1, and the comment it carried argued that one hardware DAC means one
// stream. That is true of the DAC and false of the operating system: turning N
// producers into one hardware stream is called MIXING, and refusing the second
// opener does not avoid the work, it moves the failure onto the user.
//
// MEASURED, on the owner's ASUS i7-4720HQ (build 2219, <workspace>
// BOOTLOG.TXT): /APPS/FMSYNTH held the single stream from the first DOS game of
// the session, and /APPS/MIDIPLAY's Play button was answered with EBUSY. The
// refusal line said in as many words "Nothing else on this machine can play
// until that holder closes", which is an accurate description of a design
// defect, not a hardware limit. The same limit also means a DOS game cannot
// have FM music and digitised speech at the same time: dosexec.c's Sound
// Blaster pump and /APPS/FMSYNTH are two separate PCM producers.
//
// Four is not a hardware number. It is the count of producers this OS can
// plausibly have going at once (FM synth, DOS SB16 DMA, MIDI player, one app)
// plus the honest admission that a fifth would still be refused, which is why
// the refusal path below now names every holder rather than just the first.
#define PCM_MAX_STREAMS     4

// ============================================================================
// Stream state
// ============================================================================

typedef struct {
    volatile int      in_use;       // slot allocated (cleared LAST, by the pump)
    volatile int      pump_live;    // pump thread created and not yet exited
    // #181: 1 = opened by the kernel (the DOS Sound Blaster emulation), so
    // there is no owning process. Kept as a separate flag rather than
    // owner_pid == 0 because pid 0 is a real process here (the idle task) and
    // "owned by pid 0" and "owned by no process" must not be the same value.
    volatile int      kernel_src;
    uint32_t          owner_pid;
    char              owner_name[32];   // #205: so EBUSY can name the holder
    uint32_t          rate;
    uint32_t          ch;
    uint32_t          format;

    int16_t          *ring;         // PCM_RING_FRAMES * ch samples; pump frees it
    uint32_t          cap_frames;

    // Free-running frame counters. used = w - r, space = cap - used. Only the
    // writer advances w; only the pump advances r. Single producer, single
    // consumer, so the counters need no lock: each side publishes its own
    // counter and reads the other's. uint32 wraparound is well-defined and the
    // subtraction stays correct across it.
    volatile uint32_t r;
    volatile uint32_t w;

    wait_queue_head_t wq_space;     // writer sleeps here; pump wakes it
    wait_queue_head_t wq_done;      // closer sleeps here; pump wakes it on exit
    volatile int      wq_inited;    // #217: heads inited once per slot, not per open

    volatile int      drain_req;    // close(): finish once the ring empties
    volatile int      stop;         // teardown: release every waiter now
    volatile int      pump_done;    // pump has left; ring freed; safe to reuse

    uint64_t          underruns;
    uint64_t          frames_pushed;

    // #205: per-stream resampler state, owned by the mixer thread alone.
    // rs_dev_rate is the sink rate these were computed against; when the mixer
    // opens a session at a different rate they are recomputed, which is the
    // only way this state is ever invalidated.
    uint32_t          rs_dev_rate;
    uint64_t          rs_step;      // Q32 source frames per output frame
    uint64_t          rs_phase;     // Q32 position between rs_prev and ring[r]
    int32_t           rs_prev[2];   // last source frame consumed, L/R

    // #205: bumped every time the slot is released. audio_pcm_close() captures
    // it before waiting, so a close that races a release-then-reopen of the
    // same slot cannot end up waiting on the NEW stream's pump_done.
    volatile uint32_t gen;
} pcm_stream_t;

// Statically allocated so that a closer blocked on wq_done can never touch
// freed memory: only s->ring is heap, and only the pump frees it.
static pcm_stream_t g_pcm[PCM_MAX_STREAMS];

// #217: THE FREE-SLOT SCAN AND THE CLAIM MUST BE ONE ATOMIC STEP.
//
// pcm_open_common() used to scan g_pcm[] for a slot with (!in_use &&
// !pump_live) and then mark it taken ~30 lines later, after a kmalloc and a
// dozen field writes. Nothing marked the slot taken in between, so two cores
// calling sys_audio_pcm_open() at the same time both passed the test and both
// claimed the SAME pcm_stream_t: two owners, one ring pointer (the first
// allocation leaked), two pump threads consuming one s->r, and the first
// opener's own pcm_lookup() then failing because owner_pid had been overwritten
// by the second. This is exactly the shape blame.md records at #745/#75, where
// the AP idle task and the seclog thread got the same process_t.
//
// It was never REACHED before, and that is the whole reason it survived review:
// an app opened once at startup and closed once at exit, so two opens were
// seconds or minutes apart. #217's lazy open/close makes an open happen on
// every Play, on every DOS guest that starts making noise, and on every track
// change, which is precisely the traffic that turns a latent test-and-claim
// race into a reachable one. Fixing it is part of the same change, not a
// separate cleanup.
//
// STATIC INITIALISER, NOT A LAZY ONE. #444's ATA-DMA zero-filled-chunk defect
// was in part a TOCTOU lazy spinlock init; a lock that is created on first use
// has the same race as the thing it is protecting.
static spinlock_t g_pcm_slot_lock = SPINLOCK_INIT;

static int pcm_active_count(void) {
    int n = 0;
    for (int i = 0; i < PCM_MAX_STREAMS; i++) if (g_pcm[i].in_use) n++;
    return n;
}

static inline uint32_t pcm_used(pcm_stream_t *s)  { return s->w - s->r; }
static inline uint32_t pcm_space(pcm_stream_t *s) { return s->cap_frames - (s->w - s->r); }

// Handle <-> slot. Handles are 1-based so 0 is never a valid handle.
// #181: the kernel-source form. Same slot table, same handles; the ownership
// gate is "opened by the kernel" instead of "opened by this process". A Ring-3
// handle is refused here and a kernel handle is refused by pcm_lookup(), so
// neither side can reach the other's stream.
static pcm_stream_t *pcm_lookup_k(int handle) {
    if (handle < 1 || handle > PCM_MAX_STREAMS) return NULL;
    pcm_stream_t *s = &g_pcm[handle - 1];
    if (!s->in_use) return NULL;
    if (!s->kernel_src) return NULL;
    return s;
}

static pcm_stream_t *pcm_lookup(int handle) {
    if (handle < 1 || handle > PCM_MAX_STREAMS) return NULL;
    pcm_stream_t *s = &g_pcm[handle - 1];
    if (!s->in_use) return NULL;
    if (s->kernel_src) return NULL;              // #181: not a Ring-3 stream
    // Ownership gate: a stream belongs to the process that opened it. Without
    // this, any Ring-3 process could write into or close another's stream.
    // SYS_PLAY_WAV has no such notion because it owns nothing; this does.
    process_t *cur = proc_current();
    if (!cur || cur->pid != s->owner_pid) return NULL;
    return s;
}

// Bound-check a Ring-3 buffer.
//
// #503 / MAYTERA-SEC-2026-0016: THIS USED TO BE AN ADDRESS-RANGE TEST, AND IT
// DID NOT WORK. It read:
//
//     if (a < USER_SPACE_START) return 0;    // USER_SPACE_START == 0x400000
//
// and its comment correctly stated the intent ("reject anything outside the
// user half"), but on this OS that test cannot express it: USER_SPACE_START is
// 0x400000, which is THE KERNEL'S OWN LOAD ADDRESS (linker.ld), and
// vmm_create_user_space() copies PML4[0] into every user CR3, so kernel text is
// mapped and present in the caller's own address space at an address ABOVE
// USER_SPACE_START. A Ring-3 caller passing ubuf = 0x400000 therefore PASSED
// this check and had kernel text read straight into the audio ring: an info
// leak of kernel memory out through the speakers, or into anything else that
// can observe the PCM stream.
//
// No address range can decide this question here. The only ground truth is the
// hardware's own U/S bit in the CALLER's page tables, which is what
// validate_user_ptr() reads (and which also validates the WHOLE range and
// refuses base+len overflow, replacing the hand-rolled arithmetic below).
// Reusing the shared validator rather than hand-rolling a fourth copy is the
// point: the primitive DOES exist now (security/validate.c), the comment above
// was written when it did not.
static int pcm_user_range_ok(const void *ubuf, uint64_t len) {
    if (!ubuf || len == 0) return 0;
    if (len > (uint64_t)(size_t)-1) return 0;
    return validate_user_ptr(ubuf, (size_t)len, ACCESS_READ_USER) == VALIDATE_OK;
}

// ============================================================================
// THE MIXER
// ============================================================================
//
// ONE thread, ONE sink, N producers summed into it. This replaces the previous
// one-pump-thread-per-stream arrangement, which could only ever work because
// PCM_MAX_STREAMS was 1.
//
// WHY A THREAD THAT NEVER EXITS. Start/stop races on a shared worker are the
// expensive kind: the last close begins tearing the mixer down, a new open
// arrives, and either it attaches to a dying thread or it starts a second one.
// The mixer therefore behaves exactly like hda_poll_worker(): created once, and
// from then on asleep on g_mix_wq whenever no stream is open. It costs one
// blocked kernel thread. It buys the complete absence of that race.
//
// THE SINK IS STILL OPENED AND CLOSED AROUND ACTIVITY. The mixer holds
// audio_open()/audio_close() only while at least one stream is open, so the
// SYS_PLAY_WAV path (the boot chime, MUSICPLR's --play helper) is no worse off
// than before: it does not have to contend with a permanently-held engine.
//
// A LATE PRODUCER MUST NEVER STALL THE MIX. If one stream's ring is empty when
// a block is assembled, that stream contributes silence for the missing frames
// and gets an underrun counted; the block still goes out. The alternative -
// waiting for the slow producer - is the single-holder bug rebuilt inside the
// mixer, where one process could again silence the machine.
//
// PACING IS UNCHANGED AND STILL COMES FROM THE SINK. Each writer blocks in
// audio_pcm_write() when ITS OWN ring is full, and the mixer wakes that stream's
// wq_space every time it consumes from it. So a producer is still paced by the
// DAC, per stream, exactly as before.
// ============================================================================

// rustkern/pcmmix.rs. The arithmetic - resample, accumulate, saturate - lives
// in Rust; the rings, locks, threads and wait queues stay here. Sizes are
// primitive types only, so there is no struct ABI to lock with _Static_assert.
extern uint64_t pcm_mix_add_rs(int32_t *acc, uint32_t out_frames,
                               const int16_t *src, uint32_t src_frames,
                               uint32_t src_ch, uint64_t step_q32,
                               uint64_t *phase_q32, int32_t *prev);
extern void     pcm_mix_finish_rs(const int32_t *acc, int16_t *out, uint32_t samples);
extern uint64_t pcm_mix_step_rs(uint32_t src_rate, uint32_t dev_rate);
extern uint32_t pcm_mix_selftest_rs(void);

// (#231r) rustkern/pcmeq.rs. The 5-band graphic EQ's ACTUAL per-band DSP:
// five cascaded fixed-point RBJ biquads per channel, applied to the SUMMED
// accumulator between the mix and the saturating clamp, i.e. a MASTER EQ.
//
// #231 deleted the tray EQ because its faders "write a static int g_eq[5]
// that only the fader itself reads" - a control that moved and changed
// nothing. This is the half that was missing. The same split as the mixer
// above applies: the arithmetic is Rust, the rings/locks/threads stay here.
//
// pcm_eq_process_rs() returns 0 and touches NOTHING when every band is flat,
// which is what makes the EQ-disabled path bit-identical rather than merely
// similar.
extern uint32_t pcm_eq_process_rs(int32_t *acc, uint32_t frames);
extern void     pcm_eq_session_start_rs(uint32_t rate);
extern int32_t  pcm_eq_get_rs(int32_t band);
extern int32_t  pcm_eq_db10_rs(int32_t band);
extern int32_t  pcm_eq_freq_rs(int32_t band);
extern int32_t  pcm_eq_bands_rs(void);
extern int32_t  pcm_eq_active_rs(void);
extern uint64_t pcm_eq_frames_rs(void);
extern uint64_t pcm_eq_peak_state_rs(void);
extern uint32_t pcm_eq_selftest_rs(int32_t *out25, uint64_t *out_peak);
extern uint64_t pcm_eq_bench_rs(uint32_t frames);

// The mixer sleeps here when there is nothing to do. Woken by: every
// audio_pcm_write() that adds frames, every open, every close, and
// audio_pcm_proc_exit(). Deliberately NOT initialised lazily: g_mix_wq is BSS,
// so head == NULL and locked == 0, which is exactly what
// wait_queue_head_init() would write, and a lazily-initialised lock has the
// same race as the thing it protects (#444).
static wait_queue_head_t g_mix_wq;

static volatile int g_mix_started = 0;      // the thread exists
static uint32_t     g_mix_rate = 0;         // rate the sink is programmed at
static uint32_t     g_mix_sessions = 0;
static uint64_t     g_mix_blocks = 0;       // device frames handed to the sink
static uint64_t     g_mix_short_writes = 0;
static uint32_t     g_mix_selftest = 0xFFFFFFFFu;   // 0 = PASS, ~0 = not run
static uint32_t     g_eq_selftest  = 0xFFFFFFFFu;   // (#231r) same convention

// Scratch, allocated once with the first session and kept. Sized in device
// frames for the accumulator/output, and in SOURCE frames for the de-wrap
// buffer, which is the larger of the two whenever a producer runs faster than
// the sink (a 48000 Hz source on a 44100 Hz sink consumes more than it emits).
#define MIX_BATCH        1024u   // device frames per audio_write()
#define MIX_LIN_FRAMES   4096u   // source frames de-wrapped per pull

// #426 LIVENESS BACKSTOP, NOT PACING. Read this before changing it.
//
// The DAC-space wait keeps BOTH of hda_space_wq()'s always-armed wake sources
// (the BCIS MSI ISR and the 10 ms poll worker); the timeout does not replace a
// wake we were supposed to arm, and it is not how the loop is paced. It exists
// because #190 measured a wait whose CONDITION was permanently false - somebody
// else had stopped the shared output engine - so the waiter woke at 100 Hz,
// correctly found its condition false every time, and hung for ever with the
// stream held and no error anywhere. #205 added a bounded guard for the ONE
// cause known at the time (hda_out_stopped()). This generalises it: any future
// cause of a permanently-false condition now ends as a LOGGED, bounded,
// stream-ending failure instead of an invisible silence, and the log line
// carries the measured state so the next person does not have to guess.
#define MIX_DAC_WAIT_MS      200u
#define MIX_DAC_STALL_MS     3000u

// Blocks of MIX_BATCH written into the hardware ring before the engine is
// started. The HDA ring is 32 slots of 4096 bytes = 32768 frames at S16 stereo,
// so 16 blocks of 1024 is half of it: a real head start, and still leaves the
// driver's own #189 silencer window free.
#define HDA_PREFILL_BLOCKS   16u
// How long to wait for the first producer to hand over anything at all before
// starting the engine anyway. Not pacing: the producer's own wake is armed, and
// starting silent is the correct outcome if it genuinely has nothing yet.
#define MIX_PREFILL_WAIT_MS  200u

static int32_t  *g_mix_acc = NULL;   // MIX_BATCH * 2 int32
static int16_t  *g_mix_out = NULL;   // MIX_BATCH * 2 int16
static int16_t  *g_mix_lin = NULL;   // MIX_LIN_FRAMES * 2 int16

static int mix_alloc(void) {
    if (g_mix_acc && g_mix_out && g_mix_lin) return 0;
    if (!g_mix_acc) g_mix_acc = (int32_t *)kmalloc((size_t)MIX_BATCH * 2 * sizeof(int32_t));
    if (!g_mix_out) g_mix_out = (int16_t *)kmalloc((size_t)MIX_BATCH * 2 * sizeof(int16_t));
    if (!g_mix_lin) g_mix_lin = (int16_t *)kmalloc((size_t)MIX_LIN_FRAMES * 2 * sizeof(int16_t));
    return (g_mix_acc && g_mix_out && g_mix_lin) ? 0 : -1;
}

static int mix_any_active(void) {
    for (int i = 0; i < PCM_MAX_STREAMS; i++)
        if (g_pcm[i].in_use) return 1;
    return 0;
}

// "There is a block worth assembling, or a stream needs releasing."
static int mix_has_work(void) {
    for (int i = 0; i < PCM_MAX_STREAMS; i++) {
        pcm_stream_t *s = &g_pcm[i];
        if (!s->in_use) continue;
        if (pcm_used(s) > 0 || s->drain_req || s->stop) return 1;
    }
    return 0;
}

// Release every stream that has finished: drained, stopped, or orphaned by
// audio_pcm_proc_exit(). This is the ONLY place a ring is freed and the ONLY
// place a slot is handed back, so the ordering argument lives in one function.
static void mix_release_finished(void) {
    for (int i = 0; i < PCM_MAX_STREAMS; i++) {
        pcm_stream_t *s = &g_pcm[i];
        if (!s->in_use) continue;
        if (!s->stop && !(s->drain_req && pcm_used(s) == 0)) continue;

        audiolog_write("[PCM] close: handle=%d pid=%u '%s' %u Hz %u ch  "
                       "pushed=%llu frames  underruns=%llu  reason=%s",
                       i + 1, s->owner_pid, s->owner_name, s->rate, s->ch,
                       (unsigned long long)s->frames_pushed,
                       (unsigned long long)s->underruns,
                       s->stop ? "stopped/orphaned" : "drained");

        s->stop = 1;
        wake_up_all(&s->wq_space);        // release any writer still inside

        if (s->ring) { kfree(s->ring); s->ring = NULL; }

        // #217's ordering, unchanged in spirit: publish every field a future
        // opener inspects BEFORE the wake that can release the closer, and
        // clear in_use before that wake, so a closer that has returned has
        // provably seen a free slot. All volatile; x86-TSO does not reorder
        // store-with-store, so program order is the order another core sees.
        s->gen++;
        s->pump_done = 1;
        s->in_use    = 0;
        s->pump_live = 0;
        wake_up_all(&s->wq_done);
    }
}

// Assemble ONE block of `frames` device frames from every active stream.
// Always writes exactly frames*2 samples into dst (silence where a stream had
// nothing), so the caller never has to reason about a partial block.
static void mix_render(int16_t *dst, uint32_t frames) {
    memset(g_mix_acc, 0, (size_t)frames * 2 * sizeof(int32_t));

    for (int i = 0; i < PCM_MAX_STREAMS; i++) {
        pcm_stream_t *s = &g_pcm[i];
        if (!s->in_use || !s->ring) continue;

        // Recompute the resampling step lazily rather than at open, because the
        // sink's rate is only known once a session starts and can differ
        // between sessions. Equal rates give exactly 1<<32, which pcm_mix_add_rs
        // turns into a bit-exact straight accumulate.
        if (s->rs_dev_rate != g_mix_rate) {
            s->rs_dev_rate = g_mix_rate;
            s->rs_step     = pcm_mix_step_rs(s->rate, g_mix_rate);
            s->rs_phase    = 0;
            s->rs_prev[0]  = 0;
            s->rs_prev[1]  = 0;
        }

        uint32_t done = 0;
        while (done < frames) {
            uint32_t avail = pcm_used(s);
            if (avail == 0) break;
            if (avail > MIX_LIN_FRAMES) avail = MIX_LIN_FRAMES;

            // De-wrap into the linear scratch the Rust kernel expects.
            uint32_t idx = s->r & (s->cap_frames - 1u);
            uint32_t run = s->cap_frames - idx;
            if (run > avail) run = avail;
            memcpy(g_mix_lin, s->ring + (size_t)idx * s->ch,
                   (size_t)run * s->ch * sizeof(int16_t));
            if (run < avail) {
                memcpy(g_mix_lin + (size_t)run * s->ch, s->ring,
                       (size_t)(avail - run) * s->ch * sizeof(int16_t));
            }

            uint64_t rc = pcm_mix_add_rs(g_mix_acc + (size_t)done * 2,
                                         frames - done,
                                         g_mix_lin, avail, s->ch,
                                         s->rs_step, &s->rs_phase, s->rs_prev);
            uint32_t used = (uint32_t)(rc & 0xFFFFFFFFu);
            uint32_t prod = (uint32_t)(rc >> 32);

            if (used) {
                s->r += used;
                wake_up_all(&s->wq_space);   // the writer's wake source
            }
            done += prod;
            if (prod == 0) break;            // no progress: dry
        }

        if (done < frames) s->underruns++;
    }

    // (#231r) THE EQ GOES HERE, and this position is the whole design decision.
    //
    // Post-mix, pre-clamp: the accumulator now holds the sum of every open
    // producer at full i32 precision, which is exactly what a MASTER
    // equaliser is meant to act on. Filtering per-stream instead would mean
    // PCM_MAX_STREAMS filter chains on a machine whose userland runs on one
    // core, and would give a different answer depending on how many things
    // happened to be playing.
    //
    // A flat EQ returns 0 without reading or writing a single sample, so the
    // default path through this function is byte-for-byte what it was before
    // the EQ existed. A boosted band can push the sum further into
    // pcm_mix_finish_rs's saturating clamp, which is the correct place for
    // that to be handled and was already carrying exactly that job.
    pcm_eq_process_rs(g_mix_acc, frames);

    pcm_mix_finish_rs(g_mix_acc, dst, frames * 2);
    g_mix_blocks += frames;
}

// (#231r) One line, all five bands, into the ONE file the owner is asked to
// send back. #231's complaint was that the EQ was invisible everywhere except
// on its own faceplate; a boosted band is now visible in /AUDIOLOG.TXT.
// Gains are printed in TENTHS of a dB rather than as a formatted decimal
// because audiolog_write has no float and a hand-split "%d.%d" prints -0.5 dB
// as "+0.5".
static void mix_log_eq(const char *when) {
    if (!pcm_eq_active_rs()) {
        audiolog_write("[EQ] %s: FLAT (all %d bands at 0.0 dB). The filter is "
                       "bypassed entirely and the mixed PCM reaches the sink "
                       "bit-identical to the pre-EQ path.",
                       when, pcm_eq_bands_rs());
        return;
    }
    audiolog_write("[EQ] %s: ACTIVE. %d/%d/%d/%d/%d Hz, gains "
                   "%d/%d/%d/%d/%d tenths of a dB (faders %d/%d/%d/%d/%d "
                   "of 100, 50 = flat).",
                   when,
                   pcm_eq_freq_rs(0), pcm_eq_freq_rs(1), pcm_eq_freq_rs(2),
                   pcm_eq_freq_rs(3), pcm_eq_freq_rs(4),
                   pcm_eq_db10_rs(0), pcm_eq_db10_rs(1), pcm_eq_db10_rs(2),
                   pcm_eq_db10_rs(3), pcm_eq_db10_rs(4),
                   pcm_eq_get_rs(0), pcm_eq_get_rs(1), pcm_eq_get_rs(2),
                   pcm_eq_get_rs(3), pcm_eq_get_rs(4));
}

// ---------------------------------------------------------------------------
// UAC sink. Shaped exactly like the old pcm_ring_fill(): uac_stream_source()
// treats a SHORT return as end-of-track, so an underrun must still return a
// FULL batch (mix_render() has already padded it with silence) and only a
// genuine "nothing is open any more" returns short.
// ---------------------------------------------------------------------------
static int mix_fill_uac(int16_t *dst, uint32_t frames, void *vctx) {
    (void)vctx;
    mix_release_finished();
    if (!mix_any_active()) return 0;         // real EOF: ends the session
    if (frames > MIX_BATCH) frames = MIX_BATCH;
    mix_render(dst, frames);
    return (int)frames;
}

// ---------------------------------------------------------------------------
// HDA / AC97 / SB16 sink.
// ---------------------------------------------------------------------------
static void mix_session_generic(void) {
    audio_config_t cfg = {
        .format      = AUDIO_FORMAT_S16_LE,
        .sample_rate = g_mix_rate,
        .channels    = 2,          // the mixer always emits stereo; mono
        .buffer_size = 0,          // sources are duplicated by pcm_mix_add_rs
        .period_size = 0
    };
    audio_stream_t *as = audio_open(&cfg);
    if (!as) {
        audiolog_write("[PCM] mixer: audio_open(%u Hz, 2ch) FAILED; no sound this "
                       "session. Every open stream is being released.", g_mix_rate);
        for (int i = 0; i < PCM_MAX_STREAMS; i++)
            if (g_pcm[i].in_use) g_pcm[i].stop = 1;
        mix_release_finished();
        return;
    }

    audio_device_info_t di;
    int use_hda_wq = (audio_get_device_info(&di) == AUDIO_OK &&
                      di.type == AUDIO_DEVICE_HDA);

    // 2026-08-26's STUTTER FIX, APPLIED AT THE ONE WRITER. audio_start() over
    // an empty ring makes the cyclic DMA engine consume a kzalloc-ed lap of
    // zeros while the first producer is still decoding, and the producer then
    // spends the whole track chasing a head start it gave away. Every dropout
    // measured that day landed in the first ~600 ms. Fill first, then start.
    //
    // Bounded three ways so it cannot become a wait: by the ring (audio_avail
    // stops handing out slots), by a block count, and by a short wait for the
    // first producer to have anything at all. A silent start is correct if it
    // has nothing to prefill with.
    {
        uint32_t pre = 0;
        while (pre < HDA_PREFILL_BLOCKS && mix_any_active()) {
            if (!mix_has_work()) {
                if (wait_event_timeout(&g_mix_wq,
                                       mix_has_work() || !mix_any_active(),
                                       wq_ms_to_ticks(MIX_PREFILL_WAIT_MS)) != WAIT_OK)
                    break;
                continue;
            }
            if (audio_avail(as) < (int)MIX_BATCH) break;
            mix_render(g_mix_out, MIX_BATCH);
            if (audio_write(as, g_mix_out, MIX_BATCH) <= 0) break;
            pre++;
        }
        audiolog_write("[PCM] mixer: prefilled %u block(s) of %u frames before "
                       "starting the engine", pre, MIX_BATCH);
    }

    audio_start(as);

    int      rearmed  = 0;
    uint32_t stall_ms = 0;

    for (;;) {
        mix_release_finished();
        if (!mix_any_active()) break;

        if (!mix_has_work()) {
            // #426: block for data. Wake source: every audio_pcm_write(), every
            // open/close, audio_pcm_proc_exit(). No poll, no yield-spin.
            wait_event_interruptible(&g_mix_wq, mix_has_work() || !mix_any_active());
            continue;
        }

        uint32_t n = MIX_BATCH;

        if (use_hda_wq) {
            int w = wait_event_timeout(hda_space_wq(),
                                       audio_avail(as) >= (int)n ||
                                       !mix_any_active() ||
                                       hda_out_stopped(),
                                       wq_ms_to_ticks(MIX_DAC_WAIT_MS));
            if (!mix_any_active()) break;

            if (hda_out_stopped()) {
                if (!rearmed) {
                    rearmed = 1;
                    audiolog_write("[PCM] #190: the hardware output stream was "
                                   "stopped underneath the mixer; re-arming");
                    audio_start(as);
                    continue;
                }
                audiolog_write("[PCM] #205: HDA output STILL reported stopped after "
                               "a re-arm. Ending this mix session rather than "
                               "waiting on a condition that can never go false. "
                               "avail=%d needed=%u", audio_avail(as), n);
                break;
            }
            rearmed = 0;

            if (w == WAIT_TIMEOUT) {
                // The engine claims to be running and space never appears. This
                // is the shape that hung the owner's laptop with FMSYNTH holding
                // the only stream, so SAY SO rather than wait for ever.
                stall_ms += MIX_DAC_WAIT_MS;
                if (stall_ms >= MIX_DAC_STALL_MS) {
                    uint64_t ev = 0, sms = 0, obs = 0; uint32_t lead = 0;
                    hda_starve_stats(&ev, &sms, &lead, &obs);
                    audiolog_write("[PCM] STALL: the HDA output reports RUNNING but "
                                   "freed no space for %u ms (avail=%d needed=%u). "
                                   "starve events=%llu starved=%llu ms obs=%llu. "
                                   "Ending the session so the streams are released "
                                   "instead of hanging their writers for ever.",
                                   stall_ms, audio_avail(as), n,
                                   (unsigned long long)ev,
                                   (unsigned long long)sms,
                                   (unsigned long long)obs);
                    break;
                }
                continue;
            }
            stall_ms = 0;
        } else {
            // AC97/SB16 expose no completion event. Same real-time-derived
            // proc_sleep() pacing audio_play_file() has always used for this
            // sink (#331/#347): not a spin, and not an unbounded poll.
            if (audio_avail(as) < (int)n) {
                uint32_t ms = (n * 1000u) / (g_mix_rate ? g_mix_rate : 48000u);
                proc_sleep(ms ? ms : 1);
                continue;
            }
        }

        mix_render(g_mix_out, n);
        int wr = audio_write(as, g_mix_out, n);
        if (wr < (int)n) {
            // The frames the sink declined are GONE: the mixer has already
            // consumed them from the source rings and cannot un-consume them.
            // Count it, do not hide it.
            g_mix_short_writes++;
        }
    }

    audio_drain(as);
    audio_close(as);
}

static void pcm_mixer_worker(void *arg) {
    (void)arg;

    if (mix_alloc() != 0) {
        audiolog_write("[PCM] mixer: could not allocate mix buffers; audio is "
                       "unavailable for this boot");
        g_mix_started = 0;
        return;
    }

    g_mix_selftest = pcm_mix_selftest_rs();
    audiolog_write("[PCM] mixer: up. PCM_MAX_STREAMS=%d, batch=%u frames. "
                   "pcm_mix_selftest_rs mask=0x%x %s", PCM_MAX_STREAMS,
                   MIX_BATCH, g_mix_selftest,
                   g_mix_selftest ? "<<<< FAIL: the mix arithmetic is wrong"
                                  : "PASS");

    // (#231r) THE EQ SELF-TEST IS A SPECTRAL MEASUREMENT, NOT A PLUMBING
    // CHECK, and that is the entire point of it.
    //
    // #231 deleted the previous EQ for being convincing and inert, so "the
    // fader moved" and "the syscall returned 0" are both worthless as
    // evidence here. pcm_eq_selftest_rs boosts each band in turn and measures
    // the output/input energy ratio at EVERY band's centre frequency, through
    // the real pcm_eq_process_rs entry point, and hands back a 5x5 matrix in
    // tenths of a dB. That matrix is printed below on every boot, so anybody
    // holding an /AUDIOLOG.TXT can see whether this machine's EQ actually
    // filters, per band, in dB - without a scope, a capture, or a claim.
    //
    // It runs here (mixer-thread start, before the first block is assembled)
    // rather than at kernel init because it needs no hardware and must not be
    // on the boot critical path. It costs ~250k stereo frames of filtering,
    // tens of milliseconds, once, against a 372 ms prefill.
    {
        int32_t  m[25];
        uint64_t peak = 0;
        for (int i = 0; i < 25; i++) m[i] = 0;
        g_eq_selftest = pcm_eq_selftest_rs(m, &peak);
        audiolog_write("[EQ] pcm_eq_selftest_rs mask=0x%x %s  (bit0 flat-is-"
                       "untouched, bit1 boost reaches its band, bit2 other "
                       "bands substantially unchanged, bit3 cut mirrors "
                       "boost, bit4 no overflow, bit5 control surface)",
                       g_eq_selftest,
                       g_eq_selftest ? "<<<< FAIL: the EQ does not filter as "
                                       "specified on this build"
                                     : "PASS");
        audiolog_write("[EQ] measured response, tenths of a dB. Row = the band "
                       "boosted to +12.0 dB, column = probe frequency. The two "
                       "END bands are SHELVES, whose corner frequency is by "
                       "definition the HALF-gain point, so +60 on their "
                       "diagonal is correct and +120 is reached out on the "
                       "plateau.");
        audiolog_write("[EQ]            60Hz   250Hz    1kHz    4kHz   12kHz");
        for (int r = 0; r < 5; r++) {
            audiolog_write("[EQ]  +%5d Hz %6d  %6d  %6d  %6d  %6d",
                           pcm_eq_freq_rs(r),
                           m[r * 5 + 0], m[r * 5 + 1], m[r * 5 + 2],
                           m[r * 5 + 3], m[r * 5 + 4]);
        }
        audiolog_write("[EQ] headroom probe: largest |filter state| word "
                       "reached with every band at maximum boost driven by a "
                       "4x-full-scale square = %llu. i64 wraps at "
                       "9223372036854775807; the design argument in "
                       "rustkern/pcmeq.rs says this must stay far below it.",
                       (unsigned long long)peak);
        // Cost, measured on THIS machine rather than estimated. MIX_BATCH
        // frames of the real filter with all five bands active.
        uint64_t ck = pcm_eq_bench_rs(256);
        audiolog_write("[EQ] cost: %llu cycles per 1000 stereo frames with all "
                       "5 bands active (10 biquad sections per frame). At "
                       "44100 Hz that is %llu cycles/second of filtering.",
                       (unsigned long long)ck,
                       (unsigned long long)((ck * 441ull) / 10ull));
    }

    for (;;) {
        // Idle until somebody opens a stream. This is where the thread spends
        // essentially all of its life.
        wait_event_interruptible(&g_mix_wq, mix_any_active());

        mix_release_finished();
        if (!mix_any_active()) continue;

        // Program the sink at the rate of the stream that woke us. Every other
        // producer is resampled into it by pcm_mix_add_rs(). Choosing the first
        // opener's rate rather than a fixed one means the overwhelmingly common
        // case (everything at 44100) does no resampling at all.
        uint32_t rate = 0;
        for (int i = 0; i < PCM_MAX_STREAMS; i++)
            if (g_pcm[i].in_use) { rate = g_pcm[i].rate; break; }
        if (!rate) continue;

        g_mix_rate = rate;
        g_mix_sessions++;

        // (#231r) Design the filters for THIS sink's rate and zero every
        // filter state word. Without the reset the first block of a new
        // session inherits the tail of the previous one, which is audible
        // exactly once per track as a thump - the defect the ticket named.
        pcm_eq_session_start_rs(g_mix_rate);
        mix_log_eq("session start");

        if (uac_is_ready()) {
            uac_set_output_rate(g_mix_rate);
            audiolog_write("[PCM] mixer session %u: sink=USB DAC, %u Hz requested, "
                           "DAC at %u Hz, stereo", g_mix_sessions, g_mix_rate,
                           uac_sample_rate());
            uac_stream_source(mix_fill_uac, NULL, PCM_MAX_STREAM_MS);
        } else {
            audiolog_write("[PCM] mixer session %u: sink=HDA/AC97/SB16, %u Hz, stereo",
                           g_mix_sessions, g_mix_rate);
            mix_session_generic();
        }

        // The sink is gone. Anything still open would block its writer for
        // ever, so end it here rather than leave a holder nobody can see.
        for (int i = 0; i < PCM_MAX_STREAMS; i++)
            if (g_pcm[i].in_use) g_pcm[i].stop = 1;
        mix_release_finished();

        audiolog_write("[PCM] mixer session %u ended: %llu device frames mixed, "
                       "%llu short writes to the sink", g_mix_sessions,
                       (unsigned long long)g_mix_blocks,
                       (unsigned long long)g_mix_short_writes);
        audiolog_write("[EQ] session %u ended: %llu frames actually filtered "
                       "(0 means the EQ was flat for the whole session and "
                       "the samples were passed through untouched), peak "
                       "|state| %llu", g_mix_sessions,
                       (unsigned long long)pcm_eq_frames_rs(),
                       (unsigned long long)pcm_eq_peak_state_rs());
        g_mix_rate = 0;
    }
}

// ============================================================================
// Ring-3 entry points
// ============================================================================

// (#231r) Ring 3 entry points for the EQ that are not pure pcmeq.rs calls.
//
// audio_pcm_eq_log_now() is called by SYS_AUDIO_EQ/AEQ_LOG. The tray calls it
// ONCE, when a fader drag ENDS - never during the drag. audiolog_write()
// rewrites the whole growing file on every call (see fs/bootlog.c), so a
// per-sample log during a 50-event drag would be genuinely expensive; the
// decision about when a change is "settled" belongs to the UI that knows the
// mouse button came up, not to a throttle guessed at down here.
// BOUNDED BY CONSTRUCTION, not by a convention in the caller.
//
// audiolog_write() rewrites the whole growing file on every call, and this is
// the first audiolog caller Ring 3 can drive at will. The comment above asks
// the UI to call it once per drag-release, and the UI does - but "the caller
// is well behaved" is a convention, and #205 pass 2 is a whole CHANGELOG entry
// about replacing conventions with things that cannot be broken.
//
// Two independent bounds, neither needing a clock (timer_ticks is not a wall
// clock: it loses and replays ticks in bursts, so a tick-derived rate limit
// admits a burst exactly when the machine is already loaded):
//   1. nothing is written unless the EQ state actually CHANGED since the last
//      line, so a caller looping on AEQ_LOG writes nothing at all;
//   2. a hard budget of Ring-3-driven lines per boot, after which it says so
//      once and stops. A real user dragging faders never approaches it.
#define EQ_LOG_BUDGET 200
static uint32_t g_eq_log_used  = 0;
static int32_t  g_eq_log_last[8];
static int      g_eq_log_seen  = 0;

void audio_pcm_eq_log_now(void) {
    int n = pcm_eq_bands_rs();
    if (n > 8) n = 8;
    int changed = !g_eq_log_seen;
    for (int i = 0; i < n; i++) {
        int32_t v = pcm_eq_get_rs(i);
        if (g_eq_log_last[i] != v) changed = 1;
        g_eq_log_last[i] = v;
    }
    g_eq_log_seen = 1;
    if (!changed) return;

    if (g_eq_log_used >= EQ_LOG_BUDGET) return;
    g_eq_log_used++;
    if (g_eq_log_used == EQ_LOG_BUDGET) {
        audiolog_write("[EQ] this boot has logged %u EQ changes on request from "
                       "Ring 3; that is the budget. Further AEQ_LOG calls are "
                       "ignored until reboot. The EQ still applies; only this "
                       "log line stops.", (unsigned)EQ_LOG_BUDGET);
        return;
    }
    mix_log_eq("setting changed");
}

// The boot spectral self-test's result, for AEQ_SELFTEST. 0 = PASS,
// 0xFFFFFFFF = the mixer thread has not started yet so it has not run.
uint32_t audio_pcm_eq_selftest_mask(void) {
    return g_eq_selftest;
}

// #181: ONE open body for both doors. The Ring-3 and kernel entry points differ
// only in who is recorded as the owner; everything else - validation, the sink
// check, the ring allocation, the wait-queue init and the pump thread - is
// shared, so the two can never drift apart.
static int64_t pcm_open_common(uint32_t rate, uint32_t channels, uint32_t format,
                               uint32_t owner_pid, int kernel_src) {
    if (format == 0) format = AUDIO_FORMAT_S16_LE;
    if (format != AUDIO_FORMAT_S16_LE)                     return AUDIO_PCM_EINVAL;
    if (channels < 1 || channels > AUDIO_PCM_MAX_CHANNELS) return AUDIO_PCM_EINVAL;
    if (rate < AUDIO_PCM_MIN_RATE || rate > AUDIO_PCM_MAX_RATE) return AUDIO_PCM_EINVAL;

    // A sink must exist, or the pump would spin up with nowhere to send PCM.
    if (!uac_is_ready() && !audio_is_available()) {
        // #205: NAME THE REFUSAL WHERE THE OWNER CAN READ IT.
        //
        // Every Ring-3 audio client funnels through this one function, and
        // until #205 it answered with a bare negative integer. /APPS/MIDIPLAY
        // renders that as "No audio device (pcm_open returned -6)" whether the
        // machine has no sound card at all or simply has a busy one, and
        // nothing anywhere recorded which. The owner's report for #205 was
        // exactly that string, and it cost a full investigation to establish
        // which of six returns had produced it. One line, on the failing path
        // only, into the persistent boot log the iMac can actually produce.
        bootlog_write("[PCM] #205: open REFUSED, ENODEV: no audio sink on this "
                      "machine (uac_is_ready=0, audio_is_available=0). rate=%u "
                      "ch=%u fmt=0x%x owner_pid=%u kernel_src=%d",
                      rate, channels, format, owner_pid, kernel_src);
        // #205: and into the audio log too, so the ONE file the owner is told
        // to send back carries both halves of the story: what the machine
        // selected, and every request that was then turned away.
        audiolog_write("[PCM] open REFUSED (ENODEV, no audio sink on this machine): "
                       "pid=%u rate=%u ch=%u fmt=0x%x kernel_src=%d "
                       "(uac_is_ready=0, audio_is_available=0)",
                       owner_pid, rate, channels, format, kernel_src);
        return AUDIO_PCM_ENODEV;
    }

    // #217: test AND claim under one lock. The claim is pump_live, NOT in_use:
    // pump_live alone already excludes every other opener (the predicate is
    // !in_use && !pump_live), while leaving in_use == 0 keeps pcm_lookup()
    // refusing this handle until the ring is allocated and every field is set,
    // which is still done outside the lock. So no lookup can ever reach a
    // half-built stream, and no allocation happens with a lock held.
    //
    // #205: the same lock now also decides who creates the mixer thread, so
    // two simultaneous first-opens cannot create two of them.
    pcm_stream_t *s = NULL;
    int need_mixer = 0;
    uint64_t __sf = spinlock_acquire_irqsave(&g_pcm_slot_lock);
    for (int i = 0; i < PCM_MAX_STREAMS; i++) {
        if (!g_pcm[i].in_use && !g_pcm[i].pump_live) {
            s = &g_pcm[i];
            s->pump_live = 1;          // CLAIMED, atomically with the test above
            break;
        }
    }
    if (s && !g_mix_started) { g_mix_started = 1; need_mixer = 1; }
    spinlock_release_irqrestore(&g_pcm_slot_lock, __sf);

    if (!s) {
        // #205: this used to be the NORMAL outcome, because PCM_MAX_STREAMS was
        // 1 and one long-lived holder therefore silenced the whole machine. With
        // a mixer it is a genuine exhaustion of every slot, so name ALL of them
        // rather than only g_pcm[0], which is what the old line did and what
        // made the owner's report read as though there were only ever one.
        for (int i = 0; i < PCM_MAX_STREAMS; i++) {
            bootlog_write("[PCM] #205: open REFUSED, EBUSY: all %d PCM streams are "
                          "in use. Slot %d held by pid %u '%s' (kernel_src=%d, "
                          "in_use=%d pump_live=%d, %u Hz %u ch). Requesting pid=%u "
                          "rate=%u ch=%u.",
                          PCM_MAX_STREAMS, i, g_pcm[i].owner_pid, g_pcm[i].owner_name,
                          g_pcm[i].kernel_src, g_pcm[i].in_use, g_pcm[i].pump_live,
                          g_pcm[i].rate, g_pcm[i].ch, owner_pid, rate, channels);
            audiolog_write("[PCM] open REFUSED (EBUSY, all %d slots busy): slot %d "
                           "held by pid %u '%s' %u Hz %u ch; requester pid=%u "
                           "rate=%u ch=%u", PCM_MAX_STREAMS, i, g_pcm[i].owner_pid,
                           g_pcm[i].owner_name, g_pcm[i].rate, g_pcm[i].ch,
                           owner_pid, rate, channels);
        }
        return AUDIO_PCM_EBUSY;
    }

    int slot = (int)(s - g_pcm);

    s->ring = (int16_t *)kmalloc((size_t)PCM_RING_FRAMES * channels * sizeof(int16_t));
    if (!s->ring) {
        s->pump_live = 0;              // #217: give the claim back, or the one
        return AUDIO_PCM_ENOMEM;       // slot is lost until reboot
    }
    s->cap_frames    = PCM_RING_FRAMES;
    s->rate          = rate;
    s->ch            = channels;
    s->format        = format;
    s->r = s->w      = 0;
    s->drain_req     = 0;
    s->stop          = 0;
    s->pump_done     = 0;
    s->underruns     = 0;
    s->frames_pushed = 0;
    s->owner_pid     = owner_pid;
    s->kernel_src    = kernel_src;
    s->rs_dev_rate   = 0;              // forces the mixer to compute the step
    s->rs_step       = 0;
    s->rs_phase      = 0;
    s->rs_prev[0]    = 0;
    s->rs_prev[1]    = 0;
    {
        // #205: snapshot the holder's name at open. Looking it up at EBUSY time
        // would race the holder exiting, and would name whatever process had
        // since inherited the pid.
        const char *nm = "kernel";
        if (!kernel_src) {
            process_t *cur = proc_current();
            if (cur && cur->name[0]) nm = cur->name;
            else nm = "?";
        }
        uint32_t i = 0;
        while (i < sizeof(s->owner_name) - 1 && nm[i]) { s->owner_name[i] = nm[i]; i++; }
        s->owner_name[i] = '\0';
    }

    // #217: INITIALISE THE QUEUE HEADS EXACTLY ONCE PER SLOT, NOT ONCE PER OPEN.
    //
    // This used to re-init all three on every open, justified as "a reopen can
    // never inherit a stale entry pointer". The stale-pointer worry is already
    // answered by the pump: it wake_up_all()s all three queues on its way out,
    // and wake_up_all() empties the list under the queue lock, so the heads are
    // provably NULL by the time the slot is free. What the re-init ADDED was a
    // hazard, and only #217's lazy open/close can reach it. wait_queue_head_init()
    // calls spinlock_init(), i.e. it writes lock->locked = 0 with no lock held,
    // and the outgoing pump's final wake_up_all(&wq_done) HOLDS that same lock.
    // Make close-then-immediately-open a normal path and a new opener can now run
    // spinlock_init() on a lock a dying pump is still inside, dropping it on the
    // floor mid-critical-section.
    //
    // Once per slot is the correct lifetime for a head that lives in static
    // storage. g_pcm is BSS (head = NULL, locked = 0), so this is belt on braces,
    // but it is stated rather than assumed.
    if (!s->wq_inited) {
        wait_queue_head_init(&s->wq_space);
        wait_queue_head_init(&s->wq_done);
        s->wq_inited = 1;
    }

    // pump_live was already set by the claim above; in_use is the LAST thing to
    // go up, because it is what makes the handle resolvable from Ring 3.
    s->in_use    = 1;

    // #205: ONE mixer thread for the whole machine, created on the first open
    // this boot and never retired. See the comment above pcm_mixer_worker().
    if (need_mixer) {
        if (proc_create("pcmmix", pcm_mixer_worker, NULL, PRIO_NORMAL) < 0) {
            g_mix_started = 0;
            s->in_use    = 0;
            s->pump_live = 0;
            kfree(s->ring);
            s->ring = NULL;
            audiolog_write("[PCM] open FAILED (ENOMEM): could not create the mixer "
                           "thread. pid=%u rate=%u ch=%u", owner_pid, rate, channels);
            return AUDIO_PCM_ENOMEM;
        }
    }

    // THE GRANT, LOGGED WHERE THE OWNER CAN READ IT. A denied open was already
    // recorded; a granted one was not, so a log showing no refusal could not be
    // told apart from a log showing no attempt.
    audiolog_write("[PCM] open GRANTED: handle=%d pid=%u '%s' %u Hz %u ch fmt=0x%x "
                   "kernel_src=%d (sink %s, %d of %d slots now in use)",
                   slot + 1, owner_pid, s->owner_name, rate, channels, format,
                   kernel_src,
                   uac_is_ready() ? "USB DAC" : (audio_is_available() ? "HDA/AC97/SB16"
                                                                     : "none"),
                   pcm_active_count(), PCM_MAX_STREAMS);

    wake_up_all(&g_mix_wq);             // tell the mixer it has work
    return (int64_t)(slot + 1);         // 1-based handle
}

int64_t audio_pcm_open(uint32_t rate, uint32_t channels, uint32_t format) {
    process_t *cur = proc_current();
    if (!cur) return AUDIO_PCM_EPERM;
    return pcm_open_common(rate, channels, format, cur->pid, 0);
}

// #181: the kernel door. No proc_current() requirement: the DOS DMA pump is a
// kernel thread and its stream outlives no user process.
int64_t audio_pcm_open_kernel(uint32_t rate, uint32_t channels, uint32_t format) {
    return pcm_open_common(rate, channels, format, 0, 1);
}

// #181: ONE write body for both doors. `from_user` decides only whether the
// source range is validated and copied under an AC bracket; the ring
// arithmetic, the #426 block and the wake are shared.
static int64_t pcm_write_common(pcm_stream_t *s, const int16_t *src,
                                uint32_t frames, int from_user) {
    uint32_t done = 0;

    while (done < frames) {
        if (s->stop) break;             // pump gone: stop accepting

        uint32_t space = pcm_space(s);
        if (space == 0) {
            // #426: the whole point. BLOCK on the wait queue; do NOT poll the
            // ring, do NOT proc_yield(). Wake source: pcm_ring_fill() (UAC) or
            // pcm_pump_generic() (HDA/AC97/SB16) calling wake_up_all(&wq_space)
            // after consuming frames, and pcm_pump_worker() on every exit path
            // (which also sets s->stop, so a dead pump cannot strand us).
            int rc = wait_event_interruptible(&s->wq_space,
                                              pcm_space(s) > 0 || s->stop);
            // Wake source: mix_render() calls wake_up_all(&s->wq_space) every
            // time it consumes from THIS stream, and mix_release_finished()
            // does on every teardown path (where it also sets s->stop), so a
            // dead mixer cannot strand a writer.
            if (rc == WAIT_EINTR)
                return (done > 0) ? (int64_t)done : (int64_t)AUDIO_PCM_EINTR;
            continue;
        }

        uint32_t n = frames - done;
        if (n > space) n = space;

        uint32_t idx = s->w & (s->cap_frames - 1u);
        uint32_t run = s->cap_frames - idx;
        if (run > n) run = n;

        // #19/#645: for a RING-3 source, `src` was proved ACCESS_READ_USER by
        // pcm_user_range_ok(), i.e. U/S=1, i.e. a certain #PF without AC. The
        // bracket is inside the loop and around the two copies ONLY: the
        // wait_event_interruptible() earlier in this loop must never run with
        // AC set.
        //
        // #181: for a KERNEL source the bracket must NOT be taken. AC is a
        // permission to touch USER pages; setting it around a copy from kernel
        // memory would be a no-op at best and is a lie about where the bytes
        // came from at worst. The branch is on the flag, not on the address,
        // because an address test is exactly the kind of range check #503
        // proved cannot express this question on this OS.
        if (from_user) {
            uaccess_ac_t __ac = uaccess_begin();
            memcpy(s->ring + (size_t)idx * s->ch,
                   src + (size_t)done * s->ch,
                   (size_t)run * s->ch * sizeof(int16_t));
            if (run < n) {
                memcpy(s->ring,
                       src + (size_t)(done + run) * s->ch,
                       (size_t)(n - run) * s->ch * sizeof(int16_t));
            }
            uaccess_end(__ac);
        } else {
            memcpy(s->ring + (size_t)idx * s->ch,
                   src + (size_t)done * s->ch,
                   (size_t)run * s->ch * sizeof(int16_t));
            if (run < n) {
                memcpy(s->ring,
                       src + (size_t)(done + run) * s->ch,
                       (size_t)(n - run) * s->ch * sizeof(int16_t));
            }
        }

        s->w += n;
        done += n;
        s->frames_pushed += n;

        // #205: the MIXER's wake source. One queue for the whole mixer rather
        // than one per stream, because the mixer assembles a block from every
        // stream at once and so has exactly one thing to wait for.
        wake_up_all(&g_mix_wq);
    }

    return (int64_t)done;
}

int64_t audio_pcm_write(int handle, const void *ubuf, uint32_t frames) {
    pcm_stream_t *s = pcm_lookup(handle);
    if (!s) return AUDIO_PCM_EINVAL;
    if (frames == 0) return 0;
    if (!pcm_user_range_ok(ubuf, (uint64_t)frames * s->ch * sizeof(int16_t)))
        return AUDIO_PCM_EINVAL;
    return pcm_write_common(s, (const int16_t *)ubuf, frames, 1);
}

int64_t audio_pcm_write_kernel(int handle, const int16_t *kbuf, uint32_t frames) {
    pcm_stream_t *s = pcm_lookup_k(handle);
    if (!s) return AUDIO_PCM_EINVAL;
    if (frames == 0) return 0;
    if (!kbuf) return AUDIO_PCM_EINVAL;
    return pcm_write_common(s, kbuf, frames, 0);
}

uint32_t audio_pcm_consumed_kernel(int handle) {
    pcm_stream_t *s = pcm_lookup_k(handle);
    if (!s) return 0;
    return s->r;
}

uint64_t audio_pcm_underruns_kernel(int handle) {
    pcm_stream_t *s = pcm_lookup_k(handle);
    if (!s) return 0;
    return s->underruns;
}

uint32_t audio_pcm_queued_kernel(int handle) {
    pcm_stream_t *s = pcm_lookup_k(handle);
    if (!s) return 0;
    return pcm_used(s);
}

int audio_pcm_wait_below_kernel(int handle, uint32_t max_used, uint32_t ms) {
    pcm_stream_t *s = pcm_lookup_k(handle);
    if (!s) return WAIT_TIMEOUT;
    // #426: the wake source is the pump's wake_up_all(&s->wq_space), fired on
    // EVERY consume and on every teardown path, so the timeout is a backstop
    // for a dead sink and not the mechanism.
    return wait_event_timeout(&s->wq_space,
                              pcm_used(s) <= max_used || s->stop,
                              wq_ms_to_ticks(ms));
}

int audio_pcm_wait_consumed_kernel(int handle, uint32_t target, uint32_t ms) {
    pcm_stream_t *s = pcm_lookup_k(handle);
    if (!s) return WAIT_TIMEOUT;
    // Signed difference: r is free-running and wraps, and `r >= target` would
    // be wrong for exactly one window of 2^31 frames after every wrap.
    return wait_event_timeout(&s->wq_space,
                              (int32_t)(s->r - target) >= 0 || s->stop,
                              wq_ms_to_ticks(ms));
}

// #205: the close no longer JOINS a per-stream pump, because there is no
// per-stream pump any more. It asks the shared mixer to drain and release this
// slot, and waits for that to happen.
//
// `mygen` is why the wait is safe against the slot being reused underneath it:
// mix_release_finished() bumps gen before it clears in_use, so if a fresh open
// takes this slot and resets pump_done to 0, the generation has already moved
// and the closer still terminates.
static int64_t pcm_close_common(pcm_stream_t *s, int interruptible) {
    uint32_t mygen = s->gen;
    s->drain_req = 1;
    wake_up_all(&g_mix_wq);

    if (interruptible) {
        int rc = wait_event_interruptible(&s->wq_done,
                                          s->pump_done || s->gen != mygen);
        if (rc == WAIT_EINTR) {
            // Signalled mid-close (the music player SIGKILLs its --play helper
            // on a manual track switch, so this is a NORMAL path). The mixer
            // still owns s->ring, so we must not return until it has let go.
            // Bounded: s->stop makes mix_release_finished() free it on the
            // mixer's next pass, which is at most one block away.
            s->stop = 1;
            wake_up_all(&g_mix_wq);
            wait_event(&s->wq_done, s->pump_done || s->gen != mygen);
        }
    } else {
        wait_event(&s->wq_done, s->pump_done || s->gen != mygen);
    }
    return 0;
}

int64_t audio_pcm_close_kernel(int handle) {
    pcm_stream_t *s = pcm_lookup_k(handle);
    if (!s) return AUDIO_PCM_EINVAL;
    return pcm_close_common(s, 0);
}

int64_t audio_pcm_close(int handle) {
    pcm_stream_t *s = pcm_lookup(handle);
    if (!s) return AUDIO_PCM_EINVAL;
    return pcm_close_common(s, 1);
}

void audio_pcm_proc_exit(uint32_t pid) {
    // Called from proc_exit() under cli(). MUST NOT block: it only sets flags
    // and wakes. The pump thread does the actual teardown and frees the ring.
    for (int i = 0; i < PCM_MAX_STREAMS; i++) {
        pcm_stream_t *s = &g_pcm[i];
        // #181: a kernel-owned stream has no owning process and must not be
        // torn down when some unrelated pid exits. Without this guard, pid 0
        // exiting (or any pid matching the zeroed owner_pid) would kill the
        // DOS guest's audio.
        if (s->kernel_src) continue;
        if (s->in_use && s->owner_pid == pid) {
            s->stop      = 1;
            s->drain_req = 1;
            wake_up_all(&s->wq_space);
        }
    }
    // The mixer does the teardown and the kfree; all this path may do is ask.
    wake_up_all(&g_mix_wq);
}
