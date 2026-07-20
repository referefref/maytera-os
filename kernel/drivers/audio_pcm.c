// audio_pcm.c - Ring-3 PCM push (#426-clean). Phase 1 of the Ring-0 media exit.
// See audio_pcm.h for the design, the reuse argument, and the #426 wake table.

#include "audio_pcm.h"
#include "../security/validate.h"   // #503: U/S-bit validation, not an address range
#include "audio.h"
#include "usb_audio.h"
#include "hda.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"
#include "../sync/waitq.h"
#include "../proc/process.h"

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

// Phase 1 serves one stream at a time. This is not a new restriction: the music
// player already guarantees EXACTLY ONE --play helper (kill + waitpid before
// spawn), and the USB DAC iso ring is a single sink anyway.
#define PCM_MAX_STREAMS     1

// ============================================================================
// Stream state
// ============================================================================

typedef struct {
    volatile int      in_use;       // slot allocated (cleared LAST, by the pump)
    volatile int      pump_live;    // pump thread created and not yet exited
    uint32_t          owner_pid;
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
    wait_queue_head_t wq_data;      // non-UAC pump sleeps here; writer wakes it
    wait_queue_head_t wq_done;      // closer sleeps here; pump wakes it on exit

    volatile int      drain_req;    // close(): finish once the ring empties
    volatile int      stop;         // teardown: release every waiter now
    volatile int      pump_done;    // pump has left; ring freed; safe to reuse

    uint64_t          underruns;
    uint64_t          frames_pushed;
} pcm_stream_t;

// Statically allocated so that a closer blocked on wq_done can never touch
// freed memory: only s->ring is heap, and only the pump frees it.
static pcm_stream_t g_pcm[PCM_MAX_STREAMS];

static inline uint32_t pcm_used(pcm_stream_t *s)  { return s->w - s->r; }
static inline uint32_t pcm_space(pcm_stream_t *s) { return s->cap_frames - (s->w - s->r); }

// Handle <-> slot. Handles are 1-based so 0 is never a valid handle.
static pcm_stream_t *pcm_lookup(int handle) {
    if (handle < 1 || handle > PCM_MAX_STREAMS) return NULL;
    pcm_stream_t *s = &g_pcm[handle - 1];
    if (!s->in_use) return NULL;
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
// The pull callback (UAC sink)
//
// Shaped exactly like uac_file_fill() in audio.c, and consumed by the exact
// same uac_stream_source(). The ONLY difference: this pulls from a Ring-3-fed
// ring, where uac_file_fill() pulls from an in-kernel audio_decoder_t. That IS
// the Ring-0 exit, expressed in one function.
//
// CRITICAL, and easy to get wrong: uac_stream_source() treats a SHORT return as
// end-of-track (it pads the tail with silence and sets eof=1, ending the
// stream). So a momentary underrun MUST NOT return short, or the track would
// simply stop the first time the decoder was late. On starvation we pad silence
// ourselves and return a FULL batch; only a genuine drain/stop returns short.
//
// Runs in the pump thread (uac_stream_source calls src() inline), NOT in an
// interrupt handler, so wake_up_all() here is a normal thread-context wake.
// It never blocks: the iso ring must keep being refilled.
// ============================================================================
static int pcm_ring_fill(int16_t *dst, uint32_t frames, void *vctx) {
    pcm_stream_t *s = (pcm_stream_t *)vctx;
    uint32_t got = 0;

    while (got < frames) {
        uint32_t used = pcm_used(s);
        if (used == 0) break;

        uint32_t idx = s->r & (s->cap_frames - 1u);
        uint32_t run = s->cap_frames - idx;          // to the end of the ring
        if (run > used) run = used;
        if (run > frames - got) run = frames - got;

        const int16_t *src = s->ring + (size_t)idx * s->ch;
        for (uint32_t f = 0; f < run; f++) {
            int16_t l = src[(size_t)f * s->ch + 0];
            int16_t rr = (s->ch >= 2) ? src[(size_t)f * s->ch + 1] : l;
            dst[(size_t)(got + f) * 2 + 0] = l;      // uac sink is S16 stereo
            dst[(size_t)(got + f) * 2 + 1] = rr;     // mono is duplicated, as
        }                                            // uac_file_fill() does
        s->r += run;
        got += run;
    }

    // The writer's wake source. Space just freed -> release a blocked
    // sys_audio_pcm_write().
    if (got > 0) wake_up_all(&s->wq_space);

    if (got < frames) {
        if (s->drain_req || s->stop) {
            return (int)got;            // genuine EOF: short return ends stream
        }
        // Underrun, NOT eof: pad silence, report a full batch, keep streaming.
        memset(dst + (size_t)got * 2, 0,
               (size_t)(frames - got) * 2u * sizeof(int16_t));
        s->underruns++;
        return (int)frames;
    }
    return (int)got;
}

// ============================================================================
// The pump (non-UAC sink: HDA / AC97 / SB16)
//
// Uses the existing audio_open/start/write/drain/close stream API verbatim.
// ============================================================================
static void pcm_pump_generic(pcm_stream_t *s) {
    audio_config_t cfg = {
        .format      = s->format,
        .sample_rate = s->rate,
        .channels    = s->ch,
        .buffer_size = 0,
        .period_size = 0
    };
    audio_stream_t *as = audio_open(&cfg);
    if (!as) {
        kprintf("[PCM] audio_open failed; stream aborted\n");
        return;
    }

    audio_device_info_t di;
    int use_hda_wq = (audio_get_device_info(&di) == AUDIO_OK &&
                      di.type == AUDIO_DEVICE_HDA);

    int16_t *batch = (int16_t *)kmalloc((size_t)PCM_HDA_BATCH * s->ch * sizeof(int16_t));
    if (!batch) { audio_close(as); return; }

    audio_start(as);

    for (;;) {
        if (s->stop) break;

        uint32_t used = pcm_used(s);
        if (used == 0) {
            if (s->drain_req) break;                 // ring empty + drain = done
            // #426: BLOCK for data. Wake source: sys_audio_pcm_write() after it
            // copies frames in; sys_audio_pcm_close()/audio_pcm_proc_exit()
            // when they set drain_req/stop. No poll, no yield-spin.
            wait_event_interruptible(&s->wq_data,
                                     pcm_used(s) > 0 || s->drain_req || s->stop);
            continue;
        }

        uint32_t n = (used > PCM_HDA_BATCH) ? PCM_HDA_BATCH : used;

        // Copy out of the ring (handles the wrap) into a linear batch.
        uint32_t idx = s->r & (s->cap_frames - 1u);
        uint32_t run = s->cap_frames - idx;
        if (run > n) run = n;
        memcpy(batch, s->ring + (size_t)idx * s->ch,
               (size_t)run * s->ch * sizeof(int16_t));
        if (run < n) {
            memcpy(batch + (size_t)run * s->ch, s->ring,
                   (size_t)(n - run) * s->ch * sizeof(int16_t));
        }

        if (use_hda_wq) {
            // #426: BLOCK for DAC space. Wake source: hda_service_stream(),
            // reached from BOTH hda_msi_isr() (real BCIS completion interrupt)
            // and the pre-existing 10 ms hda_poll_worker(). Two independent
            // sources means no lost wakeup can hang us for more than one 10 ms
            // service pass, even where MSI never arms (the real iMac case).
            wait_event_interruptible(hda_space_wq(),
                                     audio_avail(as) >= (int)n || s->stop);
            if (s->stop) break;
        } else {
            // AC97/SB16 expose NO completion event to wait on. Pace to real
            // time with the SAME proc_sleep() pacing audio_play_file() already
            // uses for this sink (#331/#347): a real-time-derived sleep, not a
            // spin and not an unbounded poll. HONEST LIMIT of Phase 1: making
            // this fully event-driven needs an ac97_space_wq() fed from the
            // AC97 BDL completion interrupt, mirroring what hda_space_wq() now
            // does. Filed as follow-up; the HDA and USB DAC sinks (i.e. the
            // real hardware target and the golden image) are event-driven.
            if (audio_avail(as) < (int)n) {
                uint32_t ms = (n * 1000u) / (s->rate ? s->rate : 48000u);
                proc_sleep(ms ? ms : 1);
                continue;
            }
        }

        int wr = audio_write(as, batch, n);
        if (wr > 0) {
            s->r += (uint32_t)wr;
            wake_up_all(&s->wq_space);               // the writer's wake source
        }
    }

    audio_drain(as);
    audio_close(as);
    kfree(batch);
}

// ============================================================================
// Pump thread entry
// ============================================================================
static void pcm_pump_worker(void *arg) {
    pcm_stream_t *s = (pcm_stream_t *)arg;

    if (uac_is_ready()) {
        // The USB DAC path: identical plumbing to audio_play_file_uac(), only
        // the source callback differs. Rate snap + gain/mute are unchanged.
        uac_set_output_rate(s->rate);
        kprintf("[PCM] Ring-3 PCM -> USB DAC: %u Hz, %u ch (DAC %u Hz)\n",
                s->rate, s->ch, uac_sample_rate());
        uac_stream_source(pcm_ring_fill, s, PCM_MAX_STREAM_MS);
    } else {
        kprintf("[PCM] Ring-3 PCM -> HDA/AC97/SB16 sink: %u Hz, %u ch\n",
                s->rate, s->ch);
        pcm_pump_generic(s);
    }

    kprintf("[PCM] stream ended (%llu frames pushed, %llu underruns)\n",
            (unsigned long long)s->frames_pushed,
            (unsigned long long)s->underruns);

    // Teardown, in an order that is safe for every possible waiter:
    // 1. stop -> unblocks a writer whose condition is (space || stop)
    // 2. free the ring (nobody can be reading it: the pump IS the only reader
    //    and we have left both pump loops)
    // 3. pump_done + wake -> releases a closer blocked on wq_done
    // 4. in_use cleared LAST, so a fresh open() cannot race a live teardown.
    s->stop = 1;
    wake_up_all(&s->wq_space);
    wake_up_all(&s->wq_data);

    if (s->ring) { kfree(s->ring); s->ring = NULL; }

    s->pump_live = 0;
    s->pump_done = 1;
    wake_up_all(&s->wq_done);
    s->in_use = 0;
}

// ============================================================================
// Ring-3 entry points
// ============================================================================

int64_t audio_pcm_open(uint32_t rate, uint32_t channels, uint32_t format) {
    if (format == 0) format = AUDIO_FORMAT_S16_LE;
    if (format != AUDIO_FORMAT_S16_LE)                     return AUDIO_PCM_EINVAL;
    if (channels < 1 || channels > AUDIO_PCM_MAX_CHANNELS) return AUDIO_PCM_EINVAL;
    if (rate < AUDIO_PCM_MIN_RATE || rate > AUDIO_PCM_MAX_RATE) return AUDIO_PCM_EINVAL;

    // A sink must exist, or the pump would spin up with nowhere to send PCM.
    if (!uac_is_ready() && !audio_is_available()) return AUDIO_PCM_ENODEV;

    process_t *cur = proc_current();
    if (!cur) return AUDIO_PCM_EPERM;

    pcm_stream_t *s = NULL;
    for (int i = 0; i < PCM_MAX_STREAMS; i++) {
        if (!g_pcm[i].in_use && !g_pcm[i].pump_live) { s = &g_pcm[i]; break; }
    }
    // EBUSY also covers "the previous stream's pump has not finished tearing
    // down yet". The caller (music player) already serialises with waitpid, so
    // this is a guard, not an expected path.
    if (!s) return AUDIO_PCM_EBUSY;

    int slot = (int)(s - g_pcm);

    s->ring = (int16_t *)kmalloc((size_t)PCM_RING_FRAMES * channels * sizeof(int16_t));
    if (!s->ring) return AUDIO_PCM_ENOMEM;
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
    s->owner_pid     = cur->pid;

    // Re-init in place on every open: the previous pump cleared every waiter
    // before it exited, so the lists are empty, but re-initialising here means a
    // reopen can never inherit a stale entry pointer.
    wait_queue_head_init(&s->wq_space);
    wait_queue_head_init(&s->wq_data);
    wait_queue_head_init(&s->wq_done);

    s->in_use    = 1;
    s->pump_live = 1;

    if (proc_create("pcmpump", pcm_pump_worker, s, PRIO_NORMAL) < 0) {
        s->pump_live = 0;
        s->in_use    = 0;
        kfree(s->ring);
        s->ring = NULL;
        return AUDIO_PCM_ENOMEM;
    }

    return (int64_t)(slot + 1);         // 1-based handle
}

int64_t audio_pcm_write(int handle, const void *ubuf, uint32_t frames) {
    pcm_stream_t *s = pcm_lookup(handle);
    if (!s) return AUDIO_PCM_EINVAL;
    if (frames == 0) return 0;

    if (!pcm_user_range_ok(ubuf, (uint64_t)frames * s->ch * sizeof(int16_t)))
        return AUDIO_PCM_EINVAL;

    const int16_t *src = (const int16_t *)ubuf;
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
            if (rc == WAIT_EINTR)
                return (done > 0) ? (int64_t)done : (int64_t)AUDIO_PCM_EINTR;
            continue;
        }

        uint32_t n = frames - done;
        if (n > space) n = space;

        uint32_t idx = s->w & (s->cap_frames - 1u);
        uint32_t run = s->cap_frames - idx;
        if (run > n) run = n;

        memcpy(s->ring + (size_t)idx * s->ch,
               src + (size_t)done * s->ch,
               (size_t)run * s->ch * sizeof(int16_t));
        if (run < n) {
            memcpy(s->ring,
                   src + (size_t)(done + run) * s->ch,
                   (size_t)(n - run) * s->ch * sizeof(int16_t));
        }

        s->w += n;
        done += n;
        s->frames_pushed += n;

        // The non-UAC pump's wake source (harmless no-op for the UAC pump,
        // which is paced by uac_stream_source and never waits on wq_data).
        wake_up_all(&s->wq_data);
    }

    return (int64_t)done;
}

int64_t audio_pcm_close(int handle) {
    pcm_stream_t *s = pcm_lookup(handle);
    if (!s) return AUDIO_PCM_EINVAL;

    s->drain_req = 1;
    wake_up_all(&s->wq_data);           // let a data-starved pump see drain_req

    // #426: BLOCK until the pump has drained and exited. Wake source:
    // pcm_pump_worker() sets pump_done + wake_up_all(&wq_done) on EVERY exit
    // path, including uac_stream_source()'s max_ms backstop, so this terminates.
    int rc = wait_event_interruptible(&s->wq_done, s->pump_done);
    if (rc == WAIT_EINTR) {
        // We were signalled mid-close (the music player SIGKILLs this helper on
        // a manual track switch, so this is a real path). We must STILL join the
        // pump: it is reading s->ring and owns freeing it. Force EOF, then join
        // uninterruptibly. Bounded: s->stop makes pcm_ring_fill() return short
        // on its next call, which ends uac_stream_source() within one batch.
        s->stop = 1;
        wake_up_all(&s->wq_data);
        wait_event(&s->wq_done, s->pump_done);
    }
    return 0;
}

void audio_pcm_proc_exit(uint32_t pid) {
    // Called from proc_exit() under cli(). MUST NOT block: it only sets flags
    // and wakes. The pump thread does the actual teardown and frees the ring.
    for (int i = 0; i < PCM_MAX_STREAMS; i++) {
        pcm_stream_t *s = &g_pcm[i];
        if (s->in_use && s->owner_pid == pid) {
            s->stop      = 1;
            s->drain_req = 1;
            wake_up_all(&s->wq_space);
            wake_up_all(&s->wq_data);
        }
    }
}
