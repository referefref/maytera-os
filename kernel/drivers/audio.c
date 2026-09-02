// audio.c - MayteraOS Audio Subsystem Implementation
//
// This file implements the unified audio API that abstracts over
// different audio backends (AC97, HDA, Sound Blaster 16)

#include "audio.h"
#include "ac97.h"
#include "hda.h"
#include "sound.h"
#include "usb_audio.h"      // #329: prefer a passed-through USB DAC when present
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../gui/syslog.h"
#include "../media/audio_decode.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"   // #71: audiolog_write() -> /AUDIOLOG.TXT

// #205 pass 2: SINGLE-OWNER CLAIM over the one hardware output stream
// (rustkern/sinkown.rs). The PCM mixer is the only legitimate opener; every
// other producer pushes into it. This is what makes a SECOND writer into the
// BDL ring impossible to construct rather than merely absent today: a refused
// opener gets no stream, so it has nothing to write through.
extern int      sink_claim_rs(uint64_t token);
extern int      sink_release_rs(uint64_t token);
extern int      sink_is_owner_rs(uint64_t token);
extern uint64_t sink_owner_rs(void);
extern void     sink_stats_rs(uint32_t *claims, uint32_t *refusals,
                              uint32_t *releases, uint32_t *gen);
extern uint32_t sink_selftest_rs(void);
#include "../proc/thread.h"
#include "../proc/process.h"
#include "../sync/waitq.h"   // #173: boot chime waits for the boot diagnostics
#include "audio_pcm.h"       // #189: the tail probe drives the kernel PCM door

// ============================================================================
// Internal State
// ============================================================================

static struct {
    bool initialized;
    audio_device_type_t device_type;
    audio_device_info_t device_info;
} audio_state = {0};

// Stream structure (opaque to users)
struct audio_stream {
    bool active;
    audio_state_t state;
    audio_config_t config;
    audio_callback_t callback;
    void *callback_data;
    uint64_t frames_played;
    uint64_t underruns;
};

// Maximum concurrent streams
#define MAX_STREAMS 4
static struct audio_stream streams[MAX_STREAMS];

// ============================================================================
// Error Messages
// ============================================================================

static const char *error_messages[] = {
    "Success",                              // AUDIO_OK
    "Audio subsystem not initialized",     // AUDIO_ERR_NOT_INITIALIZED
    "No audio device available",           // AUDIO_ERR_NO_DEVICE
    "Invalid audio format",                // AUDIO_ERR_INVALID_FORMAT
    "Invalid sample rate",                 // AUDIO_ERR_INVALID_RATE
    "Invalid channel configuration",       // AUDIO_ERR_INVALID_CHANNELS
    "Buffer full",                         // AUDIO_ERR_BUFFER_FULL
    "Buffer empty",                        // AUDIO_ERR_BUFFER_EMPTY
    "DMA error",                          // AUDIO_ERR_DMA_ERROR
    "Operation timed out",                 // AUDIO_ERR_TIMEOUT
    "Device busy",                         // AUDIO_ERR_BUSY
    "Operation not supported",             // AUDIO_ERR_NOT_SUPPORTED
    "Out of memory",                       // AUDIO_ERR_NO_MEMORY
    "Invalid parameter",                   // AUDIO_ERR_INVALID_PARAM
};

// ============================================================================
// Initialization
// ============================================================================

int audio_init(void) {
    LOG_INFO("[Audio] Initializing audio subsystem");
    bootlog_write("[AUDIO] Initializing audio subsystem (probe order: USB-AC, HDA, AC97, SB16, PC-spk)");

    if (audio_state.initialized) {
        return AUDIO_OK;
    }

    // Clear state
    memset(&audio_state, 0, sizeof(audio_state));
    memset(streams, 0, sizeof(streams));

    // Try to initialize audio devices in order of preference:
    // 0. USB Audio Class DAC (#329: real audible output, e.g. passed-through
    //    NuForce uDAC). Enumerated by the USB stack in usb_init() which runs
    //    before audio_init(), so uac_is_ready() is already set if present.
    // 1. Intel HDA (best quality, modern)
    // 2. AC97 (widely supported, good quality)
    // 3. Sound Blaster 16 (legacy, but works)

    // #329: prefer the USB DAC. Even if this races USB enumeration, the play
    // paths re-check uac_is_ready() dynamically, so audio still routes to it.
    if (uac_is_ready()) {
        audio_state.device_type = AUDIO_DEVICE_USB;
        audio_state.device_info.type = AUDIO_DEVICE_USB;
        audio_state.device_info.name = "USB Audio DAC";
        audio_state.device_info.description = "USB Audio Class DAC (isochronous)";
        audio_state.device_info.supported_formats = AUDIO_FORMAT_S16_LE;
        audio_state.device_info.min_sample_rate = 44100;
        audio_state.device_info.max_sample_rate = 48000;
        audio_state.device_info.max_channels = 2;
        audio_state.device_info.supports_mixing = false;
        audio_state.device_info.supports_src = false;
        audio_state.initialized = true;
        bootlog_write("[AUDIO] SELECTED: USB Audio Class DAC (%u Hz)", uac_sample_rate());
        LOG_INFO("[Audio] USB Audio DAC initialized successfully");
        return AUDIO_OK;
    }

    // Try Intel HDA first
    if (hda_init() == AUDIO_OK) {
        audio_state.device_type = AUDIO_DEVICE_HDA;
        hda_get_device_info(&audio_state.device_info);
        audio_state.initialized = true;
        bootlog_write("[AUDIO] SELECTED: Intel HDA");
        LOG_INFO("[Audio] Intel HDA initialized successfully");
        return AUDIO_OK;
    }

    // Try AC97
    if (ac97_init() == AUDIO_OK) {
        audio_state.device_type = AUDIO_DEVICE_AC97;
        ac97_get_device_info(&audio_state.device_info);
        audio_state.initialized = true;
        bootlog_write("[AUDIO] SELECTED: AC97");
        LOG_INFO("[Audio] AC97 initialized successfully");
        return AUDIO_OK;
    }

    // Try Sound Blaster 16
    if (sound_init() == SOUND_STATUS_OK) {
        audio_state.device_type = AUDIO_DEVICE_SB16;
        audio_state.device_info.type = AUDIO_DEVICE_SB16;
        audio_state.device_info.name = "Sound Blaster 16";
        audio_state.device_info.description = "Creative Sound Blaster 16 compatible";
        audio_state.device_info.supported_formats = AUDIO_FORMAT_U8 | AUDIO_FORMAT_S16_LE;
        audio_state.device_info.min_sample_rate = 8000;
        audio_state.device_info.max_sample_rate = 44100;
        audio_state.device_info.max_channels = 2;
        audio_state.device_info.supports_mixing = false;
        audio_state.device_info.supports_src = false;
        audio_state.initialized = true;
        bootlog_write("[AUDIO] SELECTED: Sound Blaster 16");
        LOG_INFO("[Audio] Sound Blaster 16 initialized successfully");
        return AUDIO_OK;
    }

    // No audio device found - PC Speaker fallback for beeps only
    audio_state.device_type = AUDIO_DEVICE_PCSPK;
    audio_state.device_info.type = AUDIO_DEVICE_PCSPK;
    audio_state.device_info.name = "PC Speaker";
    audio_state.device_info.description = "PC Speaker (beep only)";
    audio_state.device_info.supported_formats = 0;
    audio_state.device_info.min_sample_rate = 0;
    audio_state.device_info.max_sample_rate = 0;
    audio_state.device_info.max_channels = 0;
    audio_state.initialized = true;

    bootlog_write("[AUDIO] SELECTED: PC Speaker fallback (no HDA/AC97/SB16/USB output path found)");
    LOG_WARNING("[Audio] No audio device found, PC Speaker only");

    return AUDIO_ERR_NO_DEVICE;
}


// #71: sink for hda_audiolog_report() -> /AUDIOLOG.TXT (one line per call). The
// caller brackets the whole report in audiolog_begin_batch()/end_batch() so the
// per-line writes accumulate in RAM and flush to disk exactly ONCE (bootlog.c),
// avoiding the O(n^2) full-file-rewrite thrash over the slow USB-MSC stack.
static void hda_audiolog_emit(const char *line) {
    audiolog_write("%s", line ? line : "");
}


// ============================================================================
// #189 TAIL PROBE - opt-in, gated on /CONFIG/HDATAIL.CFG
// ============================================================================
//
// WHAT IT IS FOR. #189 is a defect that is INAUDIBLE IN EVERY LOG: the driver
// is doing exactly what it was told, the DMA engine is healthy, LPIB advances
// at the right rate, no underrun is counted. The only place the fault exists is
// in the bytes the codec is handed, so the only honest instrument is a capture
// taken OUTSIDE the guest (QEMU's `-audiodev wav`), analysed by
// tools/midi-wav-check/wavcheck.py.
//
// A capture needs a producer that behaves the way the failing ones do, and the
// distinguishing behaviour is NOT "plays a sound" - it is "stops feeding and
// leaves the stream OPEN". A producer that closes its stream stops the DMA
// engine and is silent either way, which is why the fault survived this long:
// the boot chime, every audio_play_file() caller and every app that exits
// cleanly all close, and all of them look fine.
//
// So this probe deliberately holds a stream open and idle. It runs THREE arms
// in one boot, and each is separated by a long, quiet gap so a capture can be
// read without ambiguity:
//
//   QUIET   open a stream, write NOTHING, close.      Must be silent.
//   SHORT   write ~120 ms, then hold OPEN and IDLE.   One blip, then silence.
//   TAIL    write ~2 s, then hold OPEN and IDLE 45 s. The #189 case: on the
//           unfixed driver this is where the 0.743 s ring lap repeats ~60
//           times; on the fixed driver it is one 2 s tone and then silence.
//
// It goes through audio_pcm_open_kernel(), i.e. the SAME kernel PCM door #181's
// Sound Blaster DSP emulation and #182's OPL2 use, so what it measures is the
// path DOS guests take and not a private test path.
//
// Gated, and off on the shipping image: an unrequested tone on every boot is a
// regression in its own right. Same gate style as /CONFIG/AUDIOTONE.CFG above.
#define HDA189_RATE          44100u
#define HDA189_TONE_HZ         440u   // TAIL arm
#define HDA189_SHORT_HZ        660u   // SHORT arm: a DIFFERENT pitch, so the
                                      // analyser's OTHER arm can show it
                                      // discriminates rather than always
                                      // answering with the expected number
#define HDA189_STALL_HZ        880u   // #190 arm
#define HDA189_START_DELAY_MS 35000u   // clear of the ~14 s boot chime
#define HDA189_QUIET_HOLD_MS   8000u
#define HDA189_SHORT_MS         120u
#define HDA189_SHORT_HOLD_MS  10000u
#define HDA189_TAIL_MS         2000u
#define HDA189_TAIL_HOLD_MS   30000u
#define HDA189_POST_CLOSE_MS   9000u
#define HDA190_PRE_MS          5000u   // tone before the interloper stops the engine
#define HDA190_POST_MS        20000u   // tone that must survive it
#define HDA189_CHUNK_FRAMES    1024u

// Render `ms` of HDA189_TONE_HZ into the kernel PCM stream, in chunks.
// audio_pcm_write_kernel() blocks on the ring's wait queue when it is full
// (#426), so this paces itself against the sink and needs no sleep of its own.
static uint64_t hda189_play_ms(int h, uint32_t hz, uint32_t ms, uint32_t *phase) {
    const int16_t *sine = hda_sine64_table();
    uint32_t inc = (uint32_t)(((uint64_t)hz * 64ull * 65536ull) / HDA189_RATE);
    uint64_t total = ((uint64_t)HDA189_RATE * ms) / 1000ull;
    int16_t *chunk = (int16_t *)kmalloc(HDA189_CHUNK_FRAMES * 2 * sizeof(int16_t));
    if (!chunk) return 0;

    uint64_t done = 0;
    while (done < total) {
        uint32_t n = (uint32_t)(total - done);
        if (n > HDA189_CHUNK_FRAMES) n = HDA189_CHUNK_FRAMES;
        for (uint32_t i = 0; i < n; i++) {
            int16_t s = sine[(*phase >> 16) & 63];
            chunk[i * 2 + 0] = s;
            chunk[i * 2 + 1] = s;
            *phase += inc;
        }
        int64_t w = audio_pcm_write_kernel(h, chunk, n);
        if (w <= 0) break;              // stream gone; do not spin on it
        done += (uint64_t)w;
    }
    kfree(chunk);
    return done;
}

static void hda189_tail_probe_worker(void *arg) {
    (void)arg;
    uint32_t phase = 0;

    proc_sleep(HDA189_START_DELAY_MS);

    // Name the arm FIRST, before making any sound, so a capture can be tied to
    // a build without trusting a filename or a timestamp. hda_init() has
    // already logged whether the starve silencer is compiled in; this line
    // records that the probe that produced the capture is this one.
    bootlog_write("[HDA-189] tail probe ARMED: rate=%u tone=%uHz; arms are "
                  "QUIET (open, write nothing, close), SHORT (%u ms then hold "
                  "OPEN+IDLE %u ms) and TAIL (%u ms then hold OPEN+IDLE %u ms). "
                  "On a driver without the #189 starve silencer the SHORT and "
                  "TAIL holds replay the ring lap; with it they are silent.",
                  HDA189_RATE, HDA189_TONE_HZ, HDA189_SHORT_MS,
                  HDA189_SHORT_HOLD_MS, HDA189_TAIL_MS, HDA189_TAIL_HOLD_MS);

    // ---- ARM 1: QUIET ------------------------------------------------------
    {
        int64_t h = audio_pcm_open_kernel(HDA189_RATE, 2, AUDIO_FORMAT_S16_LE);
        if (h <= 0) {
            bootlog_write("[HDA-189] QUIET arm: audio_pcm_open_kernel failed (%d); "
                          "probe cannot run", (int)h);
            return;
        }
        audio_pcm_close_kernel((int)h);
        bootlog_write("[HDA-189] QUIET arm: opened and closed with zero frames "
                      "written; the next %u ms must be silence",
                      HDA189_QUIET_HOLD_MS);
        proc_sleep(HDA189_QUIET_HOLD_MS);
    }

    // ---- ARM 2: SHORT ------------------------------------------------------
    {
        int64_t h = audio_pcm_open_kernel(HDA189_RATE, 2, AUDIO_FORMAT_S16_LE);
        if (h > 0) {
            uint64_t f = hda189_play_ms((int)h, HDA189_SHORT_HZ, HDA189_SHORT_MS, &phase);
            bootlog_write("[HDA-189] SHORT arm: wrote %u frames (~%u ms); now "
                          "holding the stream OPEN and IDLE for %u ms",
                          (unsigned)f, HDA189_SHORT_MS, HDA189_SHORT_HOLD_MS);
            proc_sleep(HDA189_SHORT_HOLD_MS);
            audio_pcm_close_kernel((int)h);
        }
        proc_sleep(2000);
    }

    // ---- ARM 3: TAIL (the #189 case) --------------------------------------
    {
        int64_t h = audio_pcm_open_kernel(HDA189_RATE, 2, AUDIO_FORMAT_S16_LE);
        if (h > 0) {
            uint64_t f = hda189_play_ms((int)h, HDA189_TONE_HZ, HDA189_TAIL_MS, &phase);
            bootlog_write("[HDA-189] TAIL arm: wrote %u frames (~%u ms); now "
                          "holding the stream OPEN and IDLE for %u ms. THIS is "
                          "the #189 window: any tone after this line came from "
                          "the DMA ring repeating, not from a producer.",
                          (unsigned)f, HDA189_TAIL_MS, HDA189_TAIL_HOLD_MS);
            proc_sleep(HDA189_TAIL_HOLD_MS);
            bootlog_write("[HDA-189] TAIL arm: idle hold over; closing.");
            audio_pcm_close_kernel((int)h);
        }
    }

    proc_sleep(HDA189_POST_CLOSE_MS);
    bootlog_write("[HDA-189] tail probe COMPLETE (stream closed; the last %u ms "
                  "must also be silence)", HDA189_POST_CLOSE_MS);

    // ---- ARM 4: STALL (#190) ----------------------------------------------
    //
    // Reproduces, without a DOS guest, the exact sequence #187 measured: a
    // SECOND audio client finishes and its audio_drain() + audio_close() stop
    // the ONE hardware output stream a still-live stream is using. In #187 the
    // second client was the boot chime ending at HOST 88.779; here it is opened
    // and closed deliberately, so the moment is known rather than waited for.
    //
    // UNFIXED: the tone stops dead at the interloper and never resumes, because
    // the pump's wait on hda_space_wq() can never be satisfied again - and the
    // probe then blocks forever inside the second hda189_play_ms(), so the
    // "SURVIVED" line below never prints. That absence IS the RED result.
    // FIXED: the tone runs straight through and the line prints.
    {
        int64_t h = audio_pcm_open_kernel(HDA189_RATE, 2, AUDIO_FORMAT_S16_LE);
        if (h > 0) {
            hda189_play_ms((int)h, HDA189_STALL_HZ, HDA190_PRE_MS, &phase);
            // #205 pass 2: this line used to say the arm was about to open and
            // close a second stream, which is what it did when the boot chime
            // was still a separate audio_open(). It no longer is, and the
            // attempt below is expected to be REFUSED. Saying otherwise would
            // leave a reader of a real log believing the opposite of what
            // happened three lines later.
            bootlog_write("[HDA-190/205] STALL arm: %u ms of %u Hz written; now "
                          "ATTEMPTING a second audio_open(), which is what the "
                          "boot chime used to do when it ended. It must now be "
                          "REFUSED: the PCM mixer holds the single-owner claim, "
                          "so the tone below is expected to continue.",
                          HDA190_PRE_MS, HDA189_STALL_HZ);
            {
                // #205 pass 2: WHAT THIS ARM ASSERTS HAS CHANGED, AND SAYING SO
                // IS THE POINT.
                //
                // It used to open a second audio stream and prove the first one
                // SURVIVED that stream's close (#190's fix). Since the PCM mixer
                // became the only opener - audio_play_file(), audio_play_buffer()
                // and audio_decode_play() all push into it - the interloper this
                // arm imitates (the boot chime ending mid-DOS-game) no longer
                // exists as a separate audio_open() anywhere in the tree. An
                // adversarial arm for a path with no production caller proves a
                // property of nothing, which is the failure this project keeps
                // rediscovering.
                //
                // So it now asserts the STRONGER invariant that replaced it: the
                // interloper CANNOT BE CONSTRUCTED. audio_open() refuses it
                // outright, so there is no second stream to start, drain or
                // close, and the tone continues for the reason that matters.
                audio_config_t icfg = {
                    .format      = AUDIO_FORMAT_S16_LE,
                    .sample_rate = HDA189_RATE,
                    .channels    = 2,
                    .buffer_size = 0,
                    .period_size = 0
                };
                audio_stream_t *ist = audio_open(&icfg);
                if (!ist) {
                    bootlog_write("[HDA-190/205] STALL arm PASS: the interloper's "
                                  "audio_open() was REFUSED, because the PCM mixer "
                                  "holds the single-owner claim on the hardware "
                                  "stream. A second writer into the BDL ring is "
                                  "unconstructable, not merely absent.");
                } else {
                    // Reaching here means sinkown.rs did not do its job. Do NOT
                    // then exercise the interloper: that would deliberately
                    // recreate the corruption this arm exists to prove is gone.
                    bootlog_write("[HDA-190/205] STALL arm FAIL: audio_open() handed "
                                  "out a SECOND stream (%p) while the mixer holds "
                                  "the sink (owner %p). The single-owner claim is "
                                  "broken; closing it again without writing.",
                                  (void *)ist, (void *)(uintptr_t)sink_owner_rs());
                    audio_close(ist);
                }
            }
            uint64_t f2 = hda189_play_ms((int)h, HDA189_STALL_HZ, HDA190_POST_MS, &phase);
            bootlog_write("[HDA-190/205] STALL arm SURVIVED: wrote a further %u "
                          "frames (~%u ms) AFTER the refused second open. On a "
                          "kernel where a second writer CAN be constructed, that "
                          "writer's close stops the shared engine and this line "
                          "is never reached.",
                          (unsigned)f2, HDA190_POST_MS);
            audio_pcm_close_kernel((int)h);
        }
    }

    proc_sleep(4000);
    hda_print_info();
    bootlog_write("[HDA-189/190] probe COMPLETE.");
}

static void audio_init_worker(void *arg) {
    (void)arg;
    // #703 (#71 iMac): audio_init() (-> hda_init(): HDA controller reset,
    // CORB/RIRB bring-up, codec/widget-graph parse) and hda_setup_interrupt()
    // (#71 MSI arm) walk hda_delay() CPU busy-spin retry loops that, on the
    // real slow Cirrus CS4208 (which reports IRQ=0), can spin for a long time.
    // This worker runs under the whole-kernel BKL (taken in proc_wrapper), so
    // holding it across those spins serialises login_run()/desktop_run()
    // behind the audio bring-up on real hardware and can feed the #446 SMP
    // scheduler race. Drop the BKL around the slow HDA hardware bring-up (the
    // SAME pattern used around context_switch() in proc/process.c) so this
    // deferred worker never holds the BKL while busy-spinning on hardware.
    // audio_init()/hda_setup_interrupt() only touch audio_state + HDA hardware
    // (no shared scheduler state), and the poll worker (started below, after
    // audio_init()) serialises HDA access with its own lock, so releasing the
    // BKL here is safe.
    extern uint32_t bkl_release_all(void);
    extern void bkl_reacquire(uint32_t depth);
    { uint32_t __bd = bkl_release_all(); audio_init(); bkl_reacquire(__bd); }
    // #71: OPT-IN HD Audio codec/output-path diagnostic to /AUDIOLOG.TXT. This
    // walks the full codec widget graph via hda_codec_command(), whose per-verb
    // busy-wait can spin for a long time on a codec that does not answer. On the
    // real iMac Cirrus CS4208 the un-gated scan added up to a multi-second
    // BKL-held freeze that wedged the desktop (b730/b733); a NORMAL boot must
    // therefore NEVER run it. It runs ONLY if the user drops /CONFIG/AUDIODMP.CFG
    // on the boot disk, and even then hda_audiolog_report() arms a bounded mode
    // (short per-command timeout + a hard cap on total timeouts) so a silent
    // codec bails in ~1-2s. Runs on this thread right after audio_init(), BEFORE
    // the LPIB poll worker starts, so there is no concurrent HDA codec access.
    int __audiodmp = 0;
    {
        extern fat_fs_t g_fat_fs;
        uint32_t __asz = 0;
        void *__acfg = g_fat_fs.mounted
                     ? fat_read_file(&g_fat_fs, "/CONFIG/AUDIODMP.CFG", &__asz)
                     : NULL;
        if (__acfg) {
            __audiodmp = 1;
            kfree(__acfg);
            extern void hda_audiolog_report(void (*emit)(const char *line));
            uint32_t __bda = bkl_release_all();
            audiolog_begin_batch();                 // accumulate in RAM
            hda_audiolog_report(hda_audiolog_emit); // emits many lines
            audiolog_end_batch();                   // ONE flush to /AUDIOLOG.TXT
            bkl_reacquire(__bda);
        }
    }
    syslog_log(LOG_INFO, "Audio subsystem initialized (deferred)");
    // #699: start the HDA LPIB-poll worker now that both proc_init() AND
    // audio_init() have run (this worker itself only ever starts after
    // proc_init(), so that ordering guarantee still holds). No-op if HDA
    // never initialized (hda_state.initialized false, e.g. USB DAC/AC97/
    // SB16/PC-speaker was selected instead).
    extern void hda_start_poll_worker_deferred(void);
    hda_start_poll_worker_deferred();
    // Arm HDA's real MSI interrupt (needs the Local APIC, already up since
    // smp_init() ran earlier in main.c, well before this worker is started).
    // No-op if HDA never initialized or the controller has no MSI capability.
    extern void hda_setup_interrupt(void);
    { uint32_t __bd2 = bkl_release_all(); hda_setup_interrupt(); bkl_reacquire(__bd2); }

    // #152: ALWAYS-ON one-line audio verdict to /BOOTLOG.TXT.
    //
    // The whole reason #71 stalled is that on the target machine the answer to
    // "what happened to audio this boot?" was not written anywhere the machine
    // can produce. One line, unconditional, cheap, and it names the thing that
    // decides audibility rather than merely that init "completed".
    bootlog_write("[AUDIO] verdict: device=%s hda_available=%d hda_analog=%d "
                  "(drop /CONFIG/AUDIOTONE.CFG on the boot disk for an audible "
                  "boot tone + LPIB proof; /CONFIG/AUDIODMP.CFG for the full "
                  "codec graph in /AUDIOLOG.TXT)",
                  audio_state.device_info.name ? audio_state.device_info.name : "none",
                  hda_is_available() ? 1 : 0,
                  hda_is_analog_output() ? 1 : 0);

    // #205: /AUDIOLOG.TXT IS NOW WRITTEN ON EVERY BOOT, NOT ONLY UNDER A GATE.
    //
    // /boot/LOGS.TXT has advertised /AUDIOLOG.TXT as one of the files that live
    // on partition 2 for months, and the file did not exist on the owner's
    // stick, because its only writer was hda_audiolog_report() behind the
    // /CONFIG/AUDIODMP.CFG opt-in. A file the OS documents and never writes is
    // worse than no file: it makes a reader believe audio was not the problem.
    // These lines are unconditional, so the file always exists and always says
    // what the audio subsystem decided.
    audiolog_write("[AUDIO] ===== MayteraOS audio log =====");
    audiolog_write("[AUDIO] SELECTED device: %s (%s). hda_available=%d hda_analog=%d "
                   "uac_ready=%d",
                   audio_state.device_info.name ? audio_state.device_info.name : "none",
                   audio_state.device_info.description
                       ? audio_state.device_info.description : "",
                   hda_is_available() ? 1 : 0,
                   hda_is_analog_output() ? 1 : 0,
                   uac_is_ready() ? 1 : 0);
    {
        uint32_t sm = sink_selftest_rs();
        audiolog_write("[AUDIO] sink single-owner claim: sink_selftest_rs mask=0x%x "
                       "%s. The PCM mixer is the only legitimate opener of the one "
                       "hardware output stream; any second audio_open() is refused "
                       "a stream outright.", sm,
                       sm ? "<<<< FAIL: the ownership rule is wrong" : "PASS");
    }
    audiolog_write("[AUDIO] Every PCM open, grant, refusal, stream start/stop, "
                   "underrun total and FM/OPL register count is recorded below. "
                   "A run with no [PCM] open line means nothing ever asked to "
                   "play; a run with opens but no [FM] writes means the guest "
                   "never wrote a note.");

    // #152: OPT-IN audible output proof, gated on /CONFIG/AUDIOTONE.CFG.
    //
    // Two separate faults made the audible check unavailable on real hardware:
    // audio_start_hda_selftest() in this file had ZERO CALLERS, so the "boot
    // self-tone" that the 2026-07-11 CS4208 analysis nominated as the next data
    // point had never run on ANY machine; and its result was kprintf-only, so
    // even if it had run the iMac could not have reported it.
    //
    // It stays OPT-IN rather than becoming an always-on boot chime: an
    // unrequested noise on every boot is a regression in its own right, and the
    // silent hda_check_output_dma() line logged during hda_init() already gives
    // the DMA answer without making a sound. This gate is for the boot where the
    // owner wants to hear it.
    //
    // fat_read_file() routes to the ext2 root when g_root_ext2 is set (see
    // fs/fat.c), so /CONFIG/AUDIOTONE.CFG works on the shipping two-partition
    // image, not only on a single-FAT one.
    {
        extern fat_fs_t g_fat_fs;
        uint32_t __tsz = 0;
        void *__tcfg = g_fat_fs.mounted
                     ? fat_read_file(&g_fat_fs, "/CONFIG/AUDIOTONE.CFG", &__tsz)
                     : NULL;
        if (__tcfg) {
            kfree(__tcfg);
            if (audio_state.device_type == AUDIO_DEVICE_HDA) {
                uint32_t __bd3 = bkl_release_all();
                int r = hda_selftest_tone(660, 600);   // starts a looping 660 Hz tone
                bkl_reacquire(__bd3);
                proc_sleep(600);                        // yielding hold, audible
                hda_stop();
                {
                    // #71: report the MEASUREMENT the tone took while it was
                    // AUDIBLE, not a register read taken afterwards. The probe
                    // ran between hda_start() and this hda_stop(), so its
                    // numbers describe the engine during the sound. A verdict
                    // of NOT-STARTED here would mean the tone never got the
                    // stream running at all, which is a different fault from
                    // STALLED and used to look identical in this log.
                    extern const hda_dma_probe_t *hda_last_dma_probe(void);
                    extern const char *hda_dma_verdict_name_rs(int verdict);
                    const hda_dma_probe_t *__pr = hda_last_dma_probe();
                    bootlog_write("[AUDIO] AUDIOTONE selftest: 660Hz -> DMA %s "
                                  "(LPIB %u->%u, advanced %u of an expected %u bytes "
                                  "in %u us, %u.%u%% of rate, ring peak %d/32767). "
                                  "DMA RUNNING with a non-zero ring peak and no sound "
                                  "means the failure is AFTER the controller; DMA "
                                  "STALLED means the failure is the stream engine; "
                                  "NOT-STARTED means the tone never started it.",
                                  hda_dma_verdict_name_rs(__pr->verdict),
                                  __pr->lpib0, __pr->lpib1, __pr->delta,
                                  (unsigned)__pr->expected, (unsigned)__pr->elapsed_us,
                                  __pr->permille / 10u, __pr->permille % 10u,
                                  __pr->peak);
                    (void)r;
                }
            } else {
                bootlog_write("[AUDIO] AUDIOTONE selftest: skipped, HDA is not the "
                              "active audio device");
            }
        }
    }

    // #189: opt-in tail probe. Its own worker, so a 65 s test run does not hold
    // audio bring-up (and therefore the boot chime release below) behind it.
    {
        extern fat_fs_t g_fat_fs;
        uint32_t __hsz = 0;
        void *__hcfg = g_fat_fs.mounted
                     ? fat_read_file(&g_fat_fs, "/CONFIG/HDATAIL.CFG", &__hsz)
                     : NULL;
        if (__hcfg) {
            kfree(__hcfg);
            proc_create("hda189", hda189_tail_probe_worker, NULL, PRIO_NORMAL);
        }
    }

    // #71 ORDERING, THE WHOLE POINT OF THIS CHANGE. The live output-path state,
    // the interrupt/poll-worker state and the output-DMA verdict are emitted
    // HERE, at the very end of audio bring-up, and not with the widget-graph
    // dump near the top of this function.
    //
    // The widget graph must run BEFORE hda_start_poll_worker_deferred() so the
    // scan does not race the worker for codec access. Everything else in the
    // dump is the exact opposite: it is undefined until after the poll worker
    // is started, the MSI is armed and the tone has played. Emitting both at
    // the same moment meant the real iMac14,4 build-1932 /AUDIOLOG.TXT reported
    // "MSI=NOT armed", "LPIB poll worker: NOT running" and "RUN=0 LPIB=0" --
    // all three guaranteed true at that instant on EVERY machine, healthy VM
    // included, and all three read as findings about the hardware. A snapshot
    // taken before the thing it describes exists is not evidence.
    if (__audiodmp) {
        extern void hda_audiolog_runtime_report(void (*emit)(const char *line));
        uint32_t __bdr = bkl_release_all();
        audiolog_begin_batch();
        hda_audiolog_runtime_report(hda_audiolog_emit);
        audiolog_end_batch();
        bkl_reacquire(__bdr);
    }

    // #173: this worker owns the single HDA output stream for the whole of
    // audio bring-up (probe, gated codec walk, self-tone, runtime report).
    // Release the boot chime only now that it is finished with the hardware.
    // Unconditional: a boot where no audio device was found must still wake
    // the chime worker so it can skip cleanly instead of waiting out its
    // whole budget.
    extern void audio_bootdiag_signal_done(void);
    audio_bootdiag_signal_done();
}

static int g_audio_deferred_started = 0;
void audio_start_deferred_init(void) {
    if (g_audio_deferred_started) return;
    g_audio_deferred_started = 1;
    proc_create("audioinit", audio_init_worker, NULL, PRIO_LOW);
}

void audio_shutdown(void) {
    if (!audio_state.initialized) {
        return;
    }

    // Close all streams
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].active) {
            audio_close(&streams[i]);
        }
    }

    // Shutdown active driver
    switch (audio_state.device_type) {
        case AUDIO_DEVICE_HDA:
            hda_shutdown();
            break;
        case AUDIO_DEVICE_AC97:
            ac97_shutdown();
            break;
        case AUDIO_DEVICE_SB16:
            sound_stop();
            break;
        default:
            break;
    }

    audio_state.initialized = false;
    LOG_INFO("[Audio] Audio subsystem shutdown");
}

bool audio_is_available(void) {
    return audio_state.initialized && 
           audio_state.device_type != AUDIO_DEVICE_PCSPK &&
           audio_state.device_type != AUDIO_DEVICE_NONE;
}

int audio_get_device_info(audio_device_info_t *info) {
    if (!audio_state.initialized) {
        return AUDIO_ERR_NOT_INITIALIZED;
    }
    if (!info) {
        return AUDIO_ERR_INVALID_PARAM;
    }
    *info = audio_state.device_info;
    return AUDIO_OK;
}

// ============================================================================
// Stream Management
// ============================================================================

audio_stream_t *audio_open(audio_config_t *config) {
    if (!audio_state.initialized || !audio_is_available()) {
        return NULL;
    }

    if (!config) {
        return NULL;
    }

    // Find free stream slot
    audio_stream_t *stream = NULL;
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!streams[i].active) {
            stream = &streams[i];
            break;
        }
    }

    if (!stream) {
        kprintf("[AUDIO] No free stream slots\n");
        return NULL;
    }

    // #205 pass 2: TAKE THE SINK, OR HAND BACK NOTHING.
    //
    // Taken here, before a single field of the stream is filled in, so a
    // refused caller leaves with NULL and never holds a handle it could write
    // through. That is the difference between an invariant and a request:
    // the second writer is not detected, it is unconstructable.
    //
    // The token is the stream's own address. streams[] is a static array, so
    // it is stable for the life of the claim and unique among live streams.
    // sinkown.rs never dereferences it.
    if (!sink_claim_rs((uint64_t)(uintptr_t)stream)) {
        audiolog_write("[AUDIO] audio_open REFUSED: the hardware output stream is "
                       "already claimed by %p. Every producer must push into the "
                       "PCM mixer (drivers/audio_pcm.c), which is the only "
                       "legitimate opener; a second writer into the one BDL ring "
                       "is what wedged the FM synthesiser (#205).",
                       (void *)(uintptr_t)sink_owner_rs());
        kprintf("[AUDIO] audio_open REFUSED: sink already claimed by %p\n",
                (void *)(uintptr_t)sink_owner_rs());
        return NULL;
    }

    // Validate and adjust configuration
    uint32_t format = config->format;
    uint32_t sample_rate = config->sample_rate;
    uint32_t channels = config->channels;

    // Default format if not specified
    if (format == 0) {
        format = AUDIO_FORMAT_S16_LE;
    }

    // Default sample rate
    if (sample_rate == 0) {
        sample_rate = AUDIO_RATE_48000;
    }

    // Default channels
    if (channels == 0) {
        channels = AUDIO_CHANNELS_STEREO;
    }

    // Clamp sample rate to device limits
    if (sample_rate < audio_state.device_info.min_sample_rate) {
        sample_rate = audio_state.device_info.min_sample_rate;
    }
    if (sample_rate > audio_state.device_info.max_sample_rate) {
        sample_rate = audio_state.device_info.max_sample_rate;
    }

    // Clamp channels
    if (channels > audio_state.device_info.max_channels) {
        channels = audio_state.device_info.max_channels;
    }

    // Configure hardware
    int ret;
    switch (audio_state.device_type) {
        case AUDIO_DEVICE_HDA:
            ret = hda_configure(format, sample_rate, channels);
            break;
        case AUDIO_DEVICE_AC97:
            ret = ac97_configure(format, sample_rate, channels);
            break;
        case AUDIO_DEVICE_SB16:
            // SB16 has fixed format, just check compatibility
            if (format != AUDIO_FORMAT_U8 && format != AUDIO_FORMAT_S16_LE) {
                format = AUDIO_FORMAT_U8;
            }
            ret = AUDIO_OK;
            break;
        default:
            ret = AUDIO_ERR_NO_DEVICE;
            break;
    }

    if (ret != AUDIO_OK) {
        kprintf("[AUDIO] Failed to configure device: %d\n", ret);
        return NULL;
    }

    // Initialize stream
    stream->active = true;
    stream->state = AUDIO_STATE_STOPPED;
    stream->config.format = format;
    stream->config.sample_rate = sample_rate;
    stream->config.channels = channels;
    stream->config.buffer_size = config->buffer_size;
    stream->config.period_size = config->period_size;
    stream->callback = NULL;
    stream->callback_data = NULL;
    stream->frames_played = 0;
    stream->underruns = 0;

    // Update caller's config with actual values
    config->format = format;
    config->sample_rate = sample_rate;
    config->channels = channels;

    kprintf("[Audio] Opened stream: %u Hz, %u channels\n", sample_rate, channels);

    return stream;
}

void audio_close(audio_stream_t *stream) {
    if (!stream || !stream->active) {
        return;
    }

    // Stop playback
    audio_stop(stream);

    stream->active = false;
    stream->callback = NULL;

    // #205 pass 2: give the sink back LAST, after the slot is inactive, so the
    // next claimant cannot find a stream that is simultaneously free and still
    // being torn down. Only the owner may release (sinkown.rs refuses anyone
    // else), so one client's teardown cannot strand another.
    sink_release_rs((uint64_t)(uintptr_t)stream);
}

int audio_get_stream_info(audio_stream_t *stream, audio_stream_info_t *info) {
    if (!stream || !stream->active || !info) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    info->state = stream->state;
    info->format = stream->config.format;
    info->sample_rate = stream->config.sample_rate;
    info->channels = stream->config.channels;
    info->buffer_size = stream->config.buffer_size;
    info->buffer_avail = audio_avail(stream);
    info->frames_played = stream->frames_played;
    info->underruns = stream->underruns;

    // Calculate bytes per frame
    uint32_t bits = 16; // Default
    if (stream->config.format == AUDIO_FORMAT_U8) bits = 8;
    else if (stream->config.format == AUDIO_FORMAT_S24_LE) bits = 32; // 24 in 32
    else if (stream->config.format == AUDIO_FORMAT_S32_LE) bits = 32;
    info->bytes_per_frame = (bits / 8) * stream->config.channels;

    return AUDIO_OK;
}

// ============================================================================
// Playback Control
// ============================================================================

int audio_start(audio_stream_t *stream) {
    if (!stream || !stream->active) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    if (stream->state == AUDIO_STATE_PLAYING) {
        return AUDIO_OK;
    }

    int ret;
    switch (audio_state.device_type) {
        case AUDIO_DEVICE_HDA:
            ret = hda_start();
            break;
        case AUDIO_DEVICE_AC97:
            ret = ac97_start();
            break;
        case AUDIO_DEVICE_SB16:
            // SB16 starts automatically on write
            ret = AUDIO_OK;
            break;
        default:
            ret = AUDIO_ERR_NO_DEVICE;
            break;
    }

    if (ret == AUDIO_OK) {
        stream->state = AUDIO_STATE_PLAYING;
    }

    return ret;
}

// #190: IS ANY OTHER STREAM STILL PLAYING?
//
// audio.c hands out MAX_STREAMS software streams but every one of them is
// backed by the SAME single hardware output stream. audio_stop() used to stop
// that hardware unconditionally, so ONE stream finishing silenced ALL of them,
// permanently and with no error anywhere.
//
// MEASURED (#187's capture, p3/serial.A-PRESENT.log, and it is the whole of the
// fault): the boot chime /SOUNDS/BOOTSND.MP3 starts at HOST 55.376 and the DOS
// guest's OPL2 PCM pump opens its own stream at HOST 56.825. At HOST 88.779 the
// chime ends and audio_play_file()'s audio_drain() + audio_close() each call
// audio_stop() - the two "[HDA] Playback stopped" lines at the identical
// timestamp - and hda_stop() clears SDnCTL.RUN on the stream the DOS game is
// still using. From that instant LPIB is frozen, so hda_avail() returns 0
// forever, so pcm_pump_generic()'s wait on hda_space_wq() has a condition that
// can NEVER become true again, so nothing ever wakes wq_space, so the Ring-3
// synthesiser blocks forever in audio_pcm_write(). The FM event queue then sits
// at now=1024/1024 and 76-88% of the guest's register writes are discarded for
// the rest of the session.
//
// Note what this was NOT: not a lost wakeup, and not a missing second wake arm.
// hda_space_wq() already has the redundant always-armed source doctrine asks
// for (the MSI ISR plus the 10 ms poll worker) and it was firing the whole
// time. The waiter woke at 100 Hz and correctly found its condition false on
// every single pass, because the hardware it was waiting for had been switched
// off underneath it. Adding a timeout here would have converted a permanent
// silence into a permanent stutter and hidden the real fault; the fix is to
// stop destroying the thing being waited on.
//
// #205 pass 2: THIS IS NOW BELT ON BRACES AND CANNOT FIRE. The single-owner
// claim in sinkown.rs means at most ONE stream is ever active, so the loop
// below has nothing to find and always returns false. It is kept, rather than
// deleted, because deleting it would silently change audio_stop()'s behaviour
// on the exact path #190 was filed about, and because it costs one pass over a
// four-entry array. If the claim is ever removed, this becomes load-bearing
// again, which is the reason to leave it where a reader can see both.
#ifdef AUDIO_SHARED_ENGINE_FIX
static bool audio_other_stream_playing(const audio_stream_t *self) {
    for (int i = 0; i < MAX_STREAMS; i++) {
        const audio_stream_t *s = &streams[i];
        if (s == self) continue;
        if (s->active && s->state == AUDIO_STATE_PLAYING) return true;
    }
    return false;
}
#endif

int audio_stop(audio_stream_t *stream) {
    if (!stream || !stream->active) {
        return AUDIO_ERR_INVALID_PARAM;
    }

#ifdef AUDIO_SHARED_ENGINE_FIX
    // #190: this stream is done, but the hardware is shared. Leave the engine
    // alone while somebody else is still playing through it; their own
    // audio_stop() will be the one that stops it.
    if (audio_other_stream_playing(stream)) {
        stream->state = AUDIO_STATE_STOPPED;
        kprintf("[AUDIO] #190: stream stopped, hardware left RUNNING for another "
                "active stream\n");
        return AUDIO_OK;
    }
#endif

    switch (audio_state.device_type) {
        case AUDIO_DEVICE_HDA:
            hda_stop();
            break;
        case AUDIO_DEVICE_AC97:
            ac97_stop();
            break;
        case AUDIO_DEVICE_SB16:
            sound_stop();
            break;
        default:
            break;
    }

    stream->state = AUDIO_STATE_STOPPED;
    return AUDIO_OK;
}

int audio_pause(audio_stream_t *stream) {
    if (!stream || !stream->active) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    // Most hardware doesn't have true pause, so we stop
    audio_stop(stream);
    stream->state = AUDIO_STATE_PAUSED;
    return AUDIO_OK;
}

int audio_resume(audio_stream_t *stream) {
    if (!stream || !stream->active) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    if (stream->state != AUDIO_STATE_PAUSED) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    return audio_start(stream);
}

// #514: DO NOT "convert this to hda_space_wq()" without reading this first.
//
// The wait-migration plan lists this loop as the flagship #426 class-A
// conversion (bounded proc_sleep(1) poll -> untimed wait on hda_space_wq()).
// That premise does not survive contact with the code. Two independent reasons,
// both verified by reading, neither built or booted:
//
// 1. THE LOOP IS DEAD CODE. The exit test is
//       audio_avail(stream) >= (int)stream->config.buffer_size - 1
//    and buffer_size is "preferred buffer size in frames, 0 = auto"
//    (audio.h:95). audio_open() copies the caller's value VERBATIM (audio.c:380)
//    and never resolves 0 to the device's real ring size; its "update caller's
//    config with actual values" block updates format/rate/channels only. Every
//    caller in the tree passes 0: audio.c:758 (audio_play_file), audio.c:890
//    (the decode/stream path), audio_pcm.c:179 (the Ring-3 PCM pump),
//    audio_decode.c:444. So the test is `audio_avail(stream) >= -1`, which is
//    true on the first iteration for any healthy sink (audio_avail returns >= 0),
//    and audio_drain() breaks at i == 0 without ever sleeping. It does not
//    drain: it just calls audio_stop(). Converting a loop that never iterates
//    would change nothing and fix nothing, while looking like the flagship win.
//    The REAL defect here is the tail truncation that no-op drain causes, plus
//    audio_open()'s unresolved buffer_size. That is a behaviour fix to the audio
//    path, not a mechanical wait conversion, and it needs VM verification.
//
// 2. AN UNTIMED WAIT HERE WOULD HANG ON A PATH THAT ALREADY EXISTS. The plan's
//    justification for "class A, no timeout" is that the DAC always drains, so
//    hardware forward progress guarantees the event. This tree already knows
//    that is not always true: audio.c:915-941 detects a sink that will not
//    accept data (`guard > 1500` -> `stalled = 1`), prints "output not draining
//    (no audio sink consuming)", and then calls audio_drain(st) ANYWAY on that
//    very stalled stream. If the condition were fixed (1) and the wait made
//    untimed (2), that path would block forever with the BKL-releasing sleep
//    replaced by an unbounded park: a #426 freeze introduced by the #426 fix.
//
// Correct sequence, once someone owns it: resolve buffer_size in audio_open(),
// give the driver a real drained predicate (hda_avail() maxes out at
// (HDA_NUM_BDL-1) buffers, reachable only in a one-buffer-wide window per ring
// revolution, so a naive `avail >= max` samples badly), decide what drain means
// for a stalled sink, and only then wire hda_space_wq(). Left as-is: a
// bounded, BKL-releasing, non-spinning loop that is currently a no-op is the
// safest thing in the tree today. Tracked as #514.
int audio_drain(audio_stream_t *stream) {
    if (!stream || !stream->active) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    // Wait for buffer to empty (simplified - just stop)
    stream->state = AUDIO_STATE_DRAINING;

    // Simple busy wait with timeout
    for (int i = 0; i < 1000; i++) {
        if (audio_avail(stream) >= (int)stream->config.buffer_size - 1) {
            break;
        }
        // #347: yield to the scheduler instead of busy-spinning (never hold the
        // BKL spinning) while the sink drains.
        proc_sleep(1);
    }

    audio_stop(stream);
    return AUDIO_OK;
}

// ============================================================================
// Data Transfer
// ============================================================================

int audio_write(audio_stream_t *stream, const void *buffer, uint32_t frames) {
    if (!stream || !stream->active || !buffer) {
        return AUDIO_ERR_INVALID_PARAM;
    }
    // #205 pass 2: belt on braces. The claim already means no second stream
    // exists, so this can only fire for a pointer kept ACROSS a close, which is
    // the one way a stale writer could still reach the ring.
    if (!sink_is_owner_rs((uint64_t)(uintptr_t)stream)) {
        audiolog_write("[AUDIO] audio_write REFUSED: stream %p does not hold the "
                       "sink (owner %p). Writing anyway would interleave into the "
                       "one BDL ring.", (void *)stream,
                       (void *)(uintptr_t)sink_owner_rs());
        return AUDIO_ERR_INVALID_PARAM;
    }

    int written;
    switch (audio_state.device_type) {
        case AUDIO_DEVICE_HDA:
            written = hda_write(buffer, frames);
            break;
        case AUDIO_DEVICE_AC97:
            written = ac97_write(buffer, frames);
            break;
        case AUDIO_DEVICE_SB16: {
            // Calculate bytes
            uint32_t bytes_per_frame = (stream->config.format == AUDIO_FORMAT_U8) ? 1 : 2;
            bytes_per_frame *= stream->config.channels;
            uint32_t bytes = frames * bytes_per_frame;
            int ret = sound_play_buffer(buffer, bytes, stream->config.sample_rate);
            written = (ret == SOUND_STATUS_OK) ? frames : (uint32_t)ret;
            break;
        }
        default:
            written = AUDIO_ERR_NO_DEVICE;
            break;
    }

    if (written > 0) {
        stream->frames_played += written;
    }

    return written;
}

// #205 pass 2: audio_write_nonblock() DELETED. It had ZERO CALLERS in the whole
// tree (declaration + definition only, verified by grep before removal) and was
// a second, independent door onto the same BDL ring. Leaving an unused entry
// point to a resource you have just made single-owner is how the next author
// discovers a "supported" way to become the second writer. The mixer's own
// backpressure (audio_pcm_write_kernel blocking on the PCM ring) is the
// non-blocking-write use case, and it is the one every producer already uses.

int audio_avail(audio_stream_t *stream) {
    if (!stream || !stream->active) {
        return AUDIO_ERR_INVALID_PARAM;
    }
    if (!sink_is_owner_rs((uint64_t)(uintptr_t)stream)) {
        return AUDIO_ERR_INVALID_PARAM;   // see audio_write()
    }

    switch (audio_state.device_type) {
        case AUDIO_DEVICE_HDA:
            return hda_avail();
        case AUDIO_DEVICE_AC97:
            return ac97_avail();
        case AUDIO_DEVICE_SB16:
            // SB16 blocks on write, so always report buffer size
            return 4096;
        default:
            return AUDIO_ERR_NO_DEVICE;
    }
}

// ============================================================================
// Callback Mode
// ============================================================================

int audio_set_callback(audio_stream_t *stream, audio_callback_t callback, void *user_data) {
    if (!stream || !stream->active) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    stream->callback = callback;
    stream->callback_data = user_data;

    return AUDIO_OK;
}

// ============================================================================
// Volume Control
// ============================================================================

int audio_get_volume(audio_volume_t *vol) {
    if (!audio_state.initialized || !vol) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    // Return reasonable defaults - actual implementation would query hardware
    vol->master_left = 80;
    vol->master_right = 80;
    vol->pcm_left = 80;
    vol->pcm_right = 80;
    vol->master_mute = false;
    vol->pcm_mute = false;

    return AUDIO_OK;
}

int audio_set_volume(const audio_volume_t *vol) {
    if (!audio_state.initialized || !vol) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    // Convert 0-100 to hardware scale
    uint8_t master_l = (vol->master_left * 63) / 100;
    uint8_t master_r = (vol->master_right * 63) / 100;
    uint8_t pcm_l = (vol->pcm_left * 63) / 100;
    uint8_t pcm_r = (vol->pcm_right * 63) / 100;

    switch (audio_state.device_type) {
        case AUDIO_DEVICE_HDA:
            hda_set_volume(master_l, master_r);
            break;
        case AUDIO_DEVICE_AC97:
            ac97_set_volume(63 - master_l, 63 - master_r); // AC97 uses attenuation
            ac97_set_pcm_volume(63 - pcm_l, 63 - pcm_r);
            break;
        case AUDIO_DEVICE_SB16:
            sound_set_volume((vol->master_left + vol->master_right) * 255 / 200);
            break;
        default:
            break;
    }

    if (vol->master_mute || vol->pcm_mute) {
        audio_mute(true);
    }

    return AUDIO_OK;
}

int audio_set_master_volume(int volume) {
    if (volume < AUDIO_VOLUME_MIN) volume = AUDIO_VOLUME_MIN;
    if (volume > AUDIO_VOLUME_MAX) volume = AUDIO_VOLUME_MAX;

    // #336: also drive the USB DAC software gain (it has no hardware mixer, so
    // the AC97/HDA register writes below never affect the active DAC output).
    uac_set_volume(volume);

    audio_volume_t vol = {
        .master_left = volume,
        .master_right = volume,
        .pcm_left = volume,
        .pcm_right = volume,
        .master_mute = false,
        .pcm_mute = false
    };

    return audio_set_volume(&vol);
}

int audio_mute(bool mute) {
    // #336: mute the USB DAC in the software stream path too.
    uac_set_mute(mute ? 1 : 0);
    switch (audio_state.device_type) {
        case AUDIO_DEVICE_HDA:
            hda_mute(mute);
            break;
        case AUDIO_DEVICE_AC97:
            ac97_mute(mute);
            break;
        case AUDIO_DEVICE_SB16:
            if (mute) sound_set_volume(0);
            break;
        default:
            break;
    }
    return AUDIO_OK;
}

// ============================================================================
// Simple Playback Helpers
// ============================================================================

// #329: pull raw S16 PCM from an in-memory buffer for the USB DAC streamer.
typedef struct { const int16_t *pcm; uint32_t total; uint32_t pos; int ch; } uac_buf_ctx_t;
static int uac_buf_fill(int16_t *dst, uint32_t frames, void *vctx) {
    uac_buf_ctx_t *c = (uac_buf_ctx_t *)vctx;
    uint32_t got = 0;
    while (got < frames && c->pos < c->total) {
        int16_t l = c->pcm[c->pos * (uint32_t)c->ch + 0];
        int16_t r = (c->ch >= 2) ? c->pcm[c->pos * (uint32_t)c->ch + 1] : l;
        dst[got * 2 + 0] = l;
        dst[got * 2 + 1] = r;
        c->pos++; got++;
    }
    return (int)got;
}

int audio_play_buffer(const void *data, uint32_t size,
                      uint32_t format, uint32_t sample_rate, uint32_t channels) {
    // #329: route decoded S16 PCM straight to the USB DAC when one is present.
    if (uac_is_ready() && data && size &&
        (format == AUDIO_FORMAT_S16_LE || format == 0)) {
        uint32_t ch = channels ? channels : 2;
        uint32_t frames = size / (2u * ch);
        uint32_t rate = sample_rate ? sample_rate : 44100;
        uac_set_output_rate(rate);
        uac_buf_ctx_t bc = { (const int16_t *)data, frames, 0, (int)ch };
        uint32_t dur = frames * 1000u / (uac_sample_rate() ? uac_sample_rate() : rate) + 2000u;
        uac_stream_source(uac_buf_fill, &bc, dur);
        return AUDIO_OK;
    }
    if (!audio_state.initialized || !audio_is_available()) {
        return AUDIO_ERR_NOT_INITIALIZED;
    }

    // #205: THROUGH THE MIXER, like every other producer, so a Win16 app's
    // sndPlaySound cannot fight the music player for the one hardware ring.
    //
    // This also fixes two defects that were live in the version it replaces.
    // (1) It wrote the WHOLE buffer in ONE audio_write() and then drained. The
    // HDA ring holds ~0.68 s, so anything longer was silently truncated to
    // whatever fitted; audio_pcm_write_kernel() blocks and delivers all of it.
    // (2) The caller in exec/win16api.c passes `bits` (8 or 16) as `format`,
    // and AUDIO_FORMAT_U8 is 1 while AUDIO_FORMAT_S16_LE is 2, so an 8-bit
    // guest WAV arrived here as format == 8 and was then measured as 16-bit.
    // Both spellings are accepted below and anything else is REFUSED with a
    // logged reason rather than played as noise.
    uint32_t ch = channels ? channels : 2;
    if (ch > 2) ch = 2;
    uint32_t rate = sample_rate ? sample_rate : 44100;

    int is_u8  = (format == AUDIO_FORMAT_U8)     || (format == 8);
    int is_s16 = (format == AUDIO_FORMAT_S16_LE) || (format == 16) || (format == 0);
    if (!is_u8 && !is_s16) {
        audiolog_write("[AUDIO] play_buffer REFUSED: unsupported format 0x%x "
                       "(%u Hz %u ch, %u bytes). Only U8 and S16_LE are mixed.",
                       format, rate, ch, size);
        return AUDIO_ERR_INVALID_PARAM;
    }

    uint32_t bytes_per_frame = (is_u8 ? 1u : 2u) * ch;
    if (bytes_per_frame == 0) return AUDIO_ERR_INVALID_PARAM;
    uint32_t frames = size / bytes_per_frame;
    if (frames == 0) return AUDIO_ERR_INVALID_PARAM;

    int64_t bh = audio_pcm_open_kernel(rate, ch, AUDIO_FORMAT_S16_LE);
    if (bh < 1) {
        audiolog_write("[AUDIO] play_buffer: no mixer stream (%d); %u Hz %u ch "
                       "%u frames NOT played", (int)bh, rate, ch, frames);
        return AUDIO_ERR_NO_DEVICE;
    }
    audiolog_write("[AUDIO] play_buffer: %u frames %u Hz %u ch %s -> mixer stream %d",
                   frames, rate, ch, is_u8 ? "U8->S16" : "S16", (int)bh);

    int rc = AUDIO_OK;
    if (is_s16) {
        int64_t w = audio_pcm_write_kernel((int)bh, (const int16_t *)data, frames);
        if (w <= 0) rc = AUDIO_ERR_DMA_ERROR;
    } else {
        // U8 is unsigned with midpoint 128; convert in bounded chunks so a long
        // guest sample does not need a second copy of itself in the heap.
        const uint8_t *u = (const uint8_t *)data;
        int16_t *conv = (int16_t *)kmalloc(1024 * 2 * sizeof(int16_t));
        if (!conv) { audio_pcm_close_kernel((int)bh); return AUDIO_ERR_NO_MEMORY; }
        uint32_t done = 0;
        while (done < frames) {
            uint32_t m = frames - done; if (m > 1024) m = 1024;
            for (uint32_t f = 0; f < m * ch; f++)
                conv[f] = (int16_t)(((int)u[(done * ch) + f] - 128) << 8);
            int64_t w = audio_pcm_write_kernel((int)bh, conv, m);
            if (w <= 0) { rc = AUDIO_ERR_DMA_ERROR; break; }
            done += (uint32_t)w;
        }
        kfree(conv);
    }

    audio_pcm_close_kernel((int)bh);   // drains
    return rc;
}

// ============================================================================
// #329: USB DAC file playback path
//
// The music player decodes MP3/WAV/FLAC/etc. via the unified decoder and calls
// audio_play_file (SYS_PLAY_WAV). When a USB Audio Class DAC is present we feed
// the decoder's PCM through the gapless iso ring-refill streaming engine in
// usb_audio.c so the audio actually reaches the DAC (instead of an HDA null
// sink / PC-speaker fallback). A pull callback bridges the decoder's chunked
// output into the fixed-size batches the streamer requests, converting mono to
// stereo and copying straight through (the DAC rate is set to the file rate).
// ============================================================================
#define UAC_STAGE_SAMPLES 8192
typedef struct {
    audio_decoder_t *dec;
    int ch;              // source channel count
    int16_t *stage;      // decoder scratch (interleaved source samples)
    int stage_len;       // valid samples currently in stage
    int stage_pos;       // samples already consumed from stage
    int eof;             // decoder exhausted
} uac_file_ctx_t;

// Fill up to `frames` S16 stereo frames from the decoder. Returns frames
// produced; a short return tells the streamer this is the last batch.
static int uac_file_fill(int16_t *dst, uint32_t frames, void *vctx) {
    uac_file_ctx_t *c = (uac_file_ctx_t *)vctx;
    uint32_t got = 0;
    while (got < frames) {
        if (c->stage_pos >= c->stage_len) {
            if (c->eof) break;
            int n = audio_decode_read(c->dec, c->stage, UAC_STAGE_SAMPLES);
            if (n <= 0) { c->eof = 1; break; }
            c->stage_len = n;
            c->stage_pos = 0;
        }
        int avail_frames = (c->stage_len - c->stage_pos) / c->ch;
        if (avail_frames <= 0) { c->stage_pos = c->stage_len; continue; }
        uint32_t want = frames - got;
        uint32_t take = ((uint32_t)avail_frames < want) ? (uint32_t)avail_frames : want;
        for (uint32_t f = 0; f < take; f++) {
            int b = c->stage_pos + (int)f * c->ch;
            int16_t l = c->stage[b];
            int16_t r = (c->ch >= 2) ? c->stage[b + 1] : l;
            dst[(got + f) * 2 + 0] = l;
            dst[(got + f) * 2 + 1] = r;
        }
        c->stage_pos += (int)take * c->ch;
        got += take;
    }
    return (int)got;
}

static int audio_play_file_uac(audio_decoder_t *dec, int ch, uint32_t rate,
                               uint32_t duration_ms, void *fdata) {
    uac_set_output_rate(rate);                 // snaps to a DAC-supported rate
    kprintf("[AUDIO] routing to USB DAC: file %u Hz -> DAC %u Hz, %d ch\n",
            rate, uac_sample_rate(), ch);

    uac_file_ctx_t ctx;
    ctx.dec = dec; ctx.ch = ch;
    ctx.stage_len = 0; ctx.stage_pos = 0; ctx.eof = 0;
    ctx.stage = (int16_t *)kmalloc(UAC_STAGE_SAMPLES * sizeof(int16_t));
    if (!ctx.stage) { audio_decode_close(dec); kfree(fdata); return -1; }

    // Backstop only; the source EOF ends the stream at the true end of track.
    uint32_t max_ms = duration_ms ? (duration_ms + 3000) : (30u * 60u * 1000u);
    uac_stream_source(uac_file_fill, &ctx, max_ms);

    kfree(ctx.stage);
    audio_decode_close(dec);
    kfree(fdata);
    return 0;
}

// Stream-decode and play an audio file (MP3/WAV via the unified decoder). If a
// USB DAC is present the PCM streams to it (#329); otherwise it goes to the
// active HDA/AC97/SB16 backend. Synchronous (kernel threads are cooperative-only
// here). Fails fast if the sink is not consuming so it never hangs.
extern fat_fs_t g_fat_fs;
int audio_play_file(const char *path) {
    if (!g_fat_fs.mounted) return -1;
    int use_uac = uac_is_ready();     // #329: dynamic check, race-proof vs init
    if (!use_uac && (!audio_state.initialized || !audio_is_available()))
        return AUDIO_ERR_NOT_INITIALIZED;

    uint32_t fsize = 0;
    void *fdata = fat_read_file(&g_fat_fs, path, &fsize);
    if (!fdata || fsize == 0) { if (fdata) kfree(fdata); return -1; }

    audio_decoder_t *dec = audio_decode_open(fdata, fsize);
    if (!dec) { kfree(fdata); return -1; }

    audio_info_t info;
    if (audio_decode_info(dec, &info) != DECODE_OK) {
        audio_decode_close(dec); kfree(fdata); return -1;
    }
    int ch = (info.channels >= 1) ? (int)info.channels : 2;
    uint32_t rate = (info.sample_rate >= 8000) ? info.sample_rate : 44100;
    kprintf("[AUDIO] play %s: %u Hz, %d ch, %u ms%s\n", path, rate, ch,
            info.duration_ms, use_uac ? " [USB DAC]" : "");

    // #329: USB DAC route (real audible output through the iso streaming engine).
    if (use_uac)
        return audio_play_file_uac(dec, ch, rate, info.duration_ms, fdata);

    // ========================================================================
    // #205: THE MIXER IS THE ONLY WRITER TO THE HARDWARE RING.
    // ------------------------------------------------------------------------
    // This used to call audio_open()/audio_write()/audio_close() and drive
    // hda_write() directly. So did drivers/audio_pcm.c's pump. audio.c hands out
    // MAX_STREAMS software streams over ONE hardware output stream and does no
    // mixing at all, so two live producers meant two writers into one BDL ring
    // and one shared wr_slot, each reprogramming the stream format under the
    // other.
    //
    // MEASURED, 2026-08-26, VM <vmid> (golden byte copy, QEMU -audiodev wav): the
    // 18.6 s boot chime was still playing when a DOS guest's /APPS/FMSYNTH
    // opened its stream. From that point hda_avail() returned 0 to the PCM pump
    // for as long as it was asked, because the chime kept every slot the
    // arithmetic would hand out. Before this change that pump had NO bound on
    // that wait, so it blocked for ever with the stream held: the exact shape
    // the owner reported, with the FM synthesiser wedged and everything else on
    // the machine refused EBUSY. #190 fixed one instance of this (a stop that
    // killed somebody else's engine); this removes the class, by leaving exactly
    // one writer.
    //
    // So the decoder now pushes into a kernel PCM stream and the mixer carries
    // it to the hardware, summed with every other producer. The elaborate
    // prefill-and-pace logic that used to live here moves WITH the writer: the
    // mixer prefills the hardware ring before starting the engine (the ordering
    // half of 2026-08-26's stutter fix), and audio_pcm_write_kernel() blocks on
    // the PCM ring, which is the pacing half. Neither is a clock-derived sleep,
    // so timer_ticks is not consulted anywhere on this path.
    // ========================================================================
    uint32_t ring_ms = (audio_state.device_type == AUDIO_DEVICE_HDA) ? hda_ring_ms() : 0;

    int64_t sh = audio_pcm_open_kernel(rate, (uint32_t)ch, AUDIO_FORMAT_S16_LE);
    if (sh < 1) {
        // Named, durably. A file that will not play must say why in the one log
        // the owner is asked to send back.
        audiolog_write("[AUDIO] %s: NOT PLAYED, could not open a mixer stream "
                       "(%d): %u Hz %d ch", path, (int)sh, rate, ch);
        audio_decode_close(dec); kfree(fdata); return -1;
    }
    audiolog_write("[AUDIO] play %s: %u Hz %d ch %u ms -> mixer stream %d",
                   path, rate, ch, info.duration_ms, (int)sh);

    int16_t *pcm = (int16_t *)kmalloc(8192 * sizeof(int16_t));
    if (!pcm) {
        audio_pcm_close_kernel((int)sh);
        audio_decode_close(dec); kfree(fdata); return -1;
    }

    uint64_t queued_ms = 0;
    int n, stalled = 0;

    while (!stalled && (n = audio_decode_read(dec, pcm, 2048)) > 0) {
        int chunk_frames = n / ch;
        const int16_t *p = pcm;
        int frames = chunk_frames;
        while (frames > 0) {
            // BLOCKS on the PCM ring's wait queue until the mixer has consumed
            // enough to make room. That block IS the pacing, and it is the same
            // one /APPS/FMSYNTH and the DOS Sound Blaster pump have always used.
            // A negative or zero return means the stream was torn down, which is
            // the only way out other than the file ending.
            int64_t w = audio_pcm_write_kernel((int)sh, p, (uint32_t)frames);
            if (w > 0) { p += (uint32_t)w * ch; frames -= (int)w; }
            else { stalled = 1; break; }
        }
        queued_ms += (uint64_t)chunk_frames * 1000 / rate;

#ifdef AUDIO_STARVE_SELFTEST
        // AUDLEAD, `make AUDSTARVETEST=1` ONLY. Stop feeding for longer than the
        // ring can cover, exactly once, after enough audio has played.
        //
        // #205 CHANGES WHAT THIS PROVES, and the change is the point of the
        // mixer. A late producer no longer starves the HARDWARE, because the
        // mixer substitutes silence for it and keeps the engine fed; so
        // hda_starve_stats() is now the right instrument for MIXER lateness and
        // the wrong one for PRODUCER lateness. The producer's own shortfall is
        // counted per stream and reported below as PCM-UNDERRUNS. Assert on that
        // number, not on the HDA one.
        {
            static int stalled_once = 0;
            if (!stalled_once && queued_ms > 2000) {
                stalled_once = 1;
                kprintf("[AUDIO] AUDLEAD SELFTEST: withholding audio for 1500 ms "
                        "(PCM ring holds ~680 ms) - PCM-UNDERRUNS MUST be "
                        "non-zero below\n");
                proc_sleep(1500);
            }
        }
#endif
    }
    if (stalled)
        kprintf("[AUDIO] %s: mixer stream ended early (no sink consuming)\n", path);

    // AUDLEAD: REPORT THE UNDERRUNS, both kinds, and say which is which.
    //   PCM-UNDERRUNS  this decoder was late and the mixer filled with silence.
    //   HDA starve     the MIXER was late and the DMA reached an unwritten slot.
    // Before the mixer these were the same event; they are not any more, and a
    // report that conflated them would send the next person to the wrong layer.
    uint64_t pcm_under = audio_pcm_underruns_kernel((int)sh);
    {
        uint64_t ev = 0, sms = 0, obs = 0; uint32_t lmin = 0xFFFFFFFFu;
        if (audio_state.device_type == AUDIO_DEVICE_HDA) {
            hda_starve_stats(&ev, &sms, &lmin, &obs);
            if (lmin == 0xFFFFFFFFu) {
                bootlog_write("[AUDIO] AUDLEAD %s: PCM-UNDERRUNS=%lu; HDA "
                              "starve=%lu (%lu ms lost), min lead=no-data over "
                              "%lu obs", path, (unsigned long)pcm_under,
                              (unsigned long)ev, (unsigned long)sms,
                              (unsigned long)obs);
            } else {
                bootlog_write("[AUDIO] AUDLEAD %s: PCM-UNDERRUNS=%lu; HDA "
                              "starve=%lu (%lu ms lost), min lead=%u ms of a %u "
                              "ms ring over %lu obs", path,
                              (unsigned long)pcm_under,
                              (unsigned long)ev, (unsigned long)sms,
                              lmin, ring_ms, (unsigned long)obs);
            }
            audiolog_write("[AUDIO] %s finished: %lu ms queued, PCM-UNDERRUNS=%lu "
                           "(this decoder was late), HDA starve=%lu events / %lu "
                           "ms (the mixer was late), min lead=%d ms of %u ms",
                           path, (unsigned long)queued_ms,
                           (unsigned long)pcm_under, (unsigned long)ev,
                           (unsigned long)sms,
                           (lmin == 0xFFFFFFFFu) ? -1 : (int)lmin, ring_ms);
            kprintf("[AUDIO] AUDLEAD %s: pcm_underruns=%lu hda_starve=%lu "
                    "lost=%lu ms minlead=%d ms\n", path,
                    (unsigned long)pcm_under, (unsigned long)ev,
                    (unsigned long)sms,
                    (lmin == 0xFFFFFFFFu) ? -1 : (int)lmin);
        } else {
            audiolog_write("[AUDIO] %s finished: %lu ms queued, PCM-UNDERRUNS=%lu",
                           path, (unsigned long)queued_ms,
                           (unsigned long)pcm_under);
        }
    }

    // Close DRAINS: the mixer plays out whatever is still in this stream's ring
    // before the slot is released, so the tail of the file is heard.
    audio_pcm_close_kernel((int)sh);

    kfree(pcm);
    audio_decode_close(dec);
    kfree(fdata);
    return 0;
}

int audio_play_file_async(const char *path) {
    // Kernel threads here are cooperative-only, so play synchronously.
    return audio_play_file(path);
}

// #329: boot chime played on its own kernel worker so it streams to the DAC
// WITHOUT blocking desktop startup (audio_play_file is synchronous, and via a
// USB DAC it now runs for the whole clip). Doubles as the audible+serial proof
// that the decode -> UAC iso streaming path works right after boot.
//
// #702: since audio_init() itself is now started from a separate deferred
// worker (audio_start_deferred_init(), kicked off right alongside this one),
// there is no ordering guarantee that HDA/AC97/USB probing has finished by
// the time this worker would otherwise fire. Rather than a blind fixed sleep,
// cooperatively poll audio_is_available() (proc_sleep between checks, never a
// busy-spin) for up to ~8s before attempting playback; if audio never comes
// up in that window this just skips the chime exactly like "no device found"
// already does today, instead of playing into an uninitialized/PC-speaker
// fallback state.
// #173: THE BOOT CHIME AND THE BOOT DIAGNOSTICS SHARED ONE OUTPUT STREAM.
//
// There is exactly ONE HDA output stream descriptor and ONE cyclic DMA ring in
// this driver, and two independent kernel workers were driving it at once:
// this one (audio_play_file -> audio_open -> hda_configure -> audio_start ->
// hda_start) and audio_init_worker's gated diagnostics (the AUDIODMP codec
// walk, which resets every HD Audio controller on the machine, and the
// AUDIOTONE self-tone, which drives a full SRST stream reset before playing).
// Interleaved, each one stops the other's engine.
//
// The real iMac14,4 capture shows it happening: the tone filled the ring with
// a sine whose peak sample is 8192 ("tone-peak-written=8192/32767"), and the
// probe 100 ms later measured a peak of 24213 in the same ring. Nothing in the
// tone path can write 24213 - that is the chime's decoded MP3, still being fed
// into the ring the tone thought it owned.
//
// hda_start() is now idempotent against a stop (see drivers/hda.c #173), so an
// overlap no longer produces permanent silence. But an overlap is still wrong
// on its own terms: two workers cutting each other's stream in half is not
// audio. So they are now ORDERED. The chime waits until audio_init_worker has
// finished everything it does to the hardware, then plays into a stream nobody
// else is touching.
//
// The wait is a wait_event_timeout, not the proc_sleep(100) poll this used to
// be (#426: hand-rolled poll loops are banned; the timed primitive exists).
// A TIMEOUT is the correct semantics here rather than a workaround for a wake
// we forgot to arm: the wake is posted unconditionally at the end of
// audio_init_worker, but that worker walks hda_delay() busy-spin retry loops
// against a codec that may never answer, and a codec that does not answer is
// precisely "hardware that may never respond" (it wedged the box for seconds
// on b730/b733). The chime is optional, so giving up on it is a correct
// outcome, and the budget is the same one the old poll loop used, widened to
// cover the diagnostics that now run inside it.
static wait_queue_head_t g_audio_bootdiag_wq = { .head = NULL, .lock = SPINLOCK_INIT };
static volatile int g_audio_bootdiag_done = 0;

// Called at the end of audio_init_worker(), on every path, including the ones
// where no audio device was found: a chime that is never going to play must
// still be released rather than left waiting out the whole budget.
void audio_bootdiag_signal_done(void) {
    g_audio_bootdiag_done = 1;
    wake_up_all(&g_audio_bootdiag_wq);
}

#define AUDIO_BOOTDIAG_WAIT_MS 20000

static void audio_boot_sound_worker(void *arg) {
    (void)arg;
    int rc = wait_event_timeout(&g_audio_bootdiag_wq,
                                g_audio_bootdiag_done != 0,
                                wq_ms_to_ticks(AUDIO_BOOTDIAG_WAIT_MS));
    if (rc != WAIT_OK) {
        bootlog_write("[AUDIO] #173: boot chime skipped, audio init/diagnostics "
                      "did not finish within %u ms", (unsigned)AUDIO_BOOTDIAG_WAIT_MS);
        return;
    }
    if (!audio_is_available()) {
        bootlog_write("[AUDIO] #173: boot chime skipped, no audio device");
        return;
    }

    // AUDLEAD: OPTIONAL PATH OVERRIDE, so the stutter can be measured on a real
    // machine without driving the GUI.
    //
    // The reported fault is a FLAC in the music player, and the music player
    // does not decode anything: /APPS/MUSICPLR --play calls SYS_PLAY_WAV, which
    // is audio_play_file(), which is this same function. So the only thing that
    // stops the boot chime from being an exact test of the reported fault is
    // WHICH FILE it plays. Put one line in /CONFIG/AUDTEST.CFG naming a file and
    // this plays that instead, with the AUDLEAD underrun counters reported the same
    // way. A laptop with no serial port can then answer "did the buffering
    // change work" from /BOOTLOG.TXT alone, on the owner's own hardware and his
    // own file, which is the only place the answer actually counts.
    //
    // Absent (the shipping case): the chime plays exactly as before.
    const char *path = "/SOUNDS/BOOTSND.MP3";
    static char testpath[128];
    {
        extern fat_fs_t g_fat_fs;
        uint32_t sz = 0;
        void *cfg = g_fat_fs.mounted
                  ? fat_read_file(&g_fat_fs, "/CONFIG/AUDTEST.CFG", &sz) : NULL;
        if (cfg) {
            uint32_t i = 0;
            const char *c = (const char *)cfg;
            while (i < sz && i < sizeof(testpath) - 1 &&
                   c[i] != '\n' && c[i] != '\r' && c[i] != '\0') {
                testpath[i] = c[i]; i++;
            }
            testpath[i] = '\0';
            kfree(cfg);
            if (i > 1 && testpath[0] == '/') {
                path = testpath;
                bootlog_write("[AUDIO] AUDLEAD: /CONFIG/AUDTEST.CFG overrides the "
                              "boot chime with '%s'", path);
            }
        }
    }
    audio_play_file(path);
}
void audio_start_boot_sound(void) {
    proc_create("bootsnd", audio_boot_sound_worker, NULL, PRIO_NORMAL);
}

// ============================================================================
// #329: gated MP3 -> USB DAC boot self-test.
//
// No-op unless /CONFIG/UACMP3.CFG is present. The CFG's first line is the path
// of the file to play (default /HOME/SAMPLE.MP3). Proves end-to-end that a real
// MP3 decodes (libmad) and streams to the DAC via the iso streaming worker,
// visible on serial. Used to verify the #329 wiring without driving the GUI.
// ============================================================================
extern void kprintf_set_dual_output(int enable);
static void uac_mp3test_worker(void *arg) {
    (void)arg;
    proc_sleep(9000);   // let desktop + USB enumeration settle
    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/UACMP3.CFG", &sz);
    if (!cfg) return;   // gated off

    char path[128];
    int i = 0;
    while (i < (int)sz && i < 127 && cfg[i] != '\r' && cfg[i] != '\n' &&
           cfg[i] != ' ' && cfg[i] != '\t')
        { path[i] = cfg[i]; i++; }
    path[i] = 0;
    kfree(cfg);
    if (i == 0) { const char *d = "/HOME/SAMPLE.MP3"; for (i = 0; d[i]; i++) path[i] = d[i]; path[i] = 0; }

    kprintf_set_dual_output(1);
    kprintf("\n========== #329 MP3 -> USB DAC SELFTEST ==========\n");
    kprintf("[UACMP3] file: %s, DAC ready=%d, rate=%u Hz\n",
            path, uac_is_ready(), uac_sample_rate());
    if (!uac_is_ready()) {
        kprintf("[UACMP3] NO USB DAC present - would route to HDA/AC97 fallback\n");
        kprintf("========== #329 SELFTEST: NO DAC ==========\n");
        kprintf_set_dual_output(0);
        return;
    }
    int r = audio_play_file(path);
    kprintf("[UACMP3] audio_play_file returned %d, frames streamed to DAC=%llu\n",
            r, (unsigned long long)uac_frames_streamed());
    kprintf("========== #329 SELFTEST: %s ==========\n",
            (r == 0 && uac_frames_streamed() > 0) ? "PASS" : "CHECK");
    kprintf_set_dual_output(0);
}

void audio_start_deferred_uac_mp3test(void) {
    proc_create("uacmp3test", uac_mp3test_worker, NULL, PRIO_NORMAL);
}

void audio_beep(uint32_t frequency, uint32_t duration_ms) {
    // #71: route the beep to the HDA analog output when that is the active
    // device, so system sound is actually audible through the codec speaker.
    if (audio_state.device_type == AUDIO_DEVICE_HDA && hda_is_analog_output()) {
        hda_selftest_tone(frequency, duration_ms);   // starts the looping tone
        proc_sleep(duration_ms ? duration_ms : 150);  // yielding hold
        hda_stop();
        return;
    }
    // Otherwise fall back to the PC speaker (also used for SB16).
    sound_play_tone(frequency, duration_ms);
}

// ============================================================================
// #71: HDA output self-test / boot proof.
//
// When Intel HDA is the active device, play a short sine tone through the
// auto-parsed output path and log whether the output-stream DMA advanced. This
// is the audible + serial proof for the "make HDA actually play" fix. Runs on
// its own worker so it never blocks desktop startup.
// ============================================================================
static void hda_selftest_worker(void *arg) {
    (void)arg;
    proc_sleep(4000);   // let USB enumeration + desktop settle
    if (audio_state.device_type != AUDIO_DEVICE_HDA) {
        kprintf("[HDA] selftest: HDA not the active audio device; skipped\n");
        return;
    }
    kprintf_set_dual_output(1);
    kprintf("\n========== #71 HDA OUTPUT SELFTEST ==========\n");
    int r = hda_selftest_tone(660, 500);   // start looping 660 Hz tone, verify DMA
    proc_sleep(500);                        // let it play (yields CPU, audible)
    hda_stop();
    kprintf("========== #71 HDA SELFTEST: %s ==========\n",
            (r == AUDIO_OK) ? "PASS (output DMA runs)" : "CHECK (DMA did not advance)");
    kprintf_set_dual_output(0);
}
void audio_start_hda_selftest(void) {
    proc_create("hdatone", hda_selftest_worker, NULL, PRIO_NORMAL);
}

// ============================================================================
// Sample Rate Conversion (Simple Linear Interpolation)
// ============================================================================

uint32_t audio_resample(const int16_t *src_data, uint32_t src_frames, uint32_t src_rate,
                        int16_t *dst_data, uint32_t dst_rate, uint32_t channels) {
    if (!src_data || !dst_data || src_frames == 0) {
        return 0;
    }

    // Calculate output frames
    uint64_t dst_frames = ((uint64_t)src_frames * dst_rate + src_rate - 1) / src_rate;

    // Resampling ratio (fixed point 16.16)
    uint32_t ratio = ((uint64_t)src_rate << 16) / dst_rate;

    for (uint32_t i = 0; i < dst_frames; i++) {
        uint32_t src_pos = ((uint64_t)i * ratio) >> 16;
        uint32_t frac = ((uint64_t)i * ratio) & 0xFFFF;

        if (src_pos >= src_frames - 1) {
            src_pos = src_frames - 1;
            frac = 0;
        }

        for (uint32_t ch = 0; ch < channels; ch++) {
            int32_t s0 = src_data[src_pos * channels + ch];
            int32_t s1 = src_data[(src_pos + 1) * channels + ch];

            // Linear interpolation
            int32_t sample = s0 + (((s1 - s0) * (int32_t)frac) >> 16);

            dst_data[i * channels + ch] = (int16_t)sample;
        }
    }

    return (uint32_t)dst_frames;
}


// ============================================================================
// Debug/Information
// ============================================================================

void audio_print_info(void) {
    kprintf("\n[AUDIO] Audio Subsystem Information:\n");
    kprintf("  Initialized: %s\n", audio_state.initialized ? "Yes" : "No");

    if (!audio_state.initialized) {
        return;
    }

    kprintf("  Device Type: ");
    switch (audio_state.device_type) {
        case AUDIO_DEVICE_HDA:  kprintf("Intel HDA\n"); break;
        case AUDIO_DEVICE_AC97: kprintf("AC97\n"); break;
        case AUDIO_DEVICE_SB16: kprintf("Sound Blaster 16\n"); break;
        case AUDIO_DEVICE_PCSPK: kprintf("PC Speaker\n"); break;
        case AUDIO_DEVICE_USB: kprintf("USB Audio Class DAC\n"); break;
        default: kprintf("Unknown\n"); break;
    }

    kprintf("  Device Name: %s\n", audio_state.device_info.name);
    kprintf("  Description: %s\n", audio_state.device_info.description);
    kprintf("  Sample Rate: %u - %u Hz\n", 
            audio_state.device_info.min_sample_rate,
            audio_state.device_info.max_sample_rate);
    kprintf("  Max Channels: %u\n", audio_state.device_info.max_channels);
    kprintf("  Hardware Mixing: %s\n", 
            audio_state.device_info.supports_mixing ? "Yes" : "No");
    kprintf("  Hardware SRC: %s\n", 
            audio_state.device_info.supports_src ? "Yes" : "No");

    // Print device-specific info
    switch (audio_state.device_type) {
        case AUDIO_DEVICE_HDA:
            hda_print_info();
            break;
        case AUDIO_DEVICE_AC97:
            ac97_print_info();
            break;
        case AUDIO_DEVICE_SB16:
            sound_print_info();
            break;
        default:
            break;
    }
}

const char *audio_strerror(int error) {
    if (error >= 0) {
        return error_messages[0]; // Success
    }

    int idx = -error;
    if (idx < (int)(sizeof(error_messages) / sizeof(error_messages[0]))) {
        return error_messages[idx];
    }

    return "Unknown error";
}

// ============================================================================
// #162: system master volume / mute, and the deferred apply worker.
// ============================================================================
//
// The STATE lives in rustkern/sysvol.rs; this is only the plumbing that state
// needs on the C side. See drivers/sysvol.h for the context rules.
//
// WHY THE WORKER EXISTS. A media key can arrive in HARD IRQ CONTEXT (PS/2
// IRQ1 -> keyboard_process_scancode -> sysvol_key_rs), and applying a volume
// change reaches into the codec: hda_set_volume() issues verbs, uac_set_volume()
// walks the USB DAC path. That is not work an interrupt handler may do, and
// this tree has a whole family of freezes from subsystems that did it anyway.
// So the IRQ side only moves atomics and this worker does the MMIO.
//
// NO WAKE CAN BE LOST, and not because of a timeout. sysvol_key_rs() raises
// the dirty flag BEFORE sysvol_wake_apply() is called, and wait_event()
// re-tests its condition before it sleeps, so a wake that lands "too early"
// simply becomes "the condition was already true". A wait_event_timeout here
// would be a workaround for a wake we forgot to arm; we did not forget.

#include "sysvol.h"
#include "../sync/waitq.h"

// rustkern/hidrepd.rs. Declared here rather than pulling in usb_hid.h (which
// drags in xhci.h) purely so the two #162 self-tests report from one place.
extern int hidrepd_selftest_rs(uint32_t *out_checks, uint32_t *out_first_fail);

static wait_queue_head_t g_sysvol_wq;
static int g_sysvol_wq_ready = 0;

// The two hardware shims rustkern/sysvol.rs calls. They exist rather than
// letting Rust call audio_mute()/audio_set_master_volume() directly because
// audio_mute() takes a C bool, whose ABI is one byte with undefined high
// bits; handing that an i32 from Rust is the kind of quiet mismatch that
// works until it does not.
void sysvol_hw_set_level(int level) {
    audio_set_master_volume(level);
}

void sysvol_hw_set_mute(int mute) {
    audio_mute(mute ? true : false);
}

void sysvol_wake_apply(void) {
    if (!g_sysvol_wq_ready) return;
    wake_up(&g_sysvol_wq);      // irqsave inside; safe from an ISR
}

void sysvol_apply_now(void) {
    sysvol_apply_rs();
}

static void sysvol_worker(void *arg) {
    (void)arg;
    for (;;) {
        wait_event(&g_sysvol_wq, sysvol_dirty_rs());
        sysvol_apply_rs();
    }
}

static int g_sysvol_worker_started = 0;

void sysvol_start_worker(void) {
    if (g_sysvol_worker_started) return;
    g_sysvol_worker_started = 1;
    wait_queue_head_init(&g_sysvol_wq);
    g_sysvol_wq_ready = 1;
    proc_create("sysvol", sysvol_worker, NULL, PRIO_LOW);
    // Say it out loud. A deferred-apply worker that silently failed to start
    // presents as "the volume keys work on some boots", which is the worst
    // kind of bug to be handed later with no log line to check.
    bootlog_write("[SYSVOL] deferred-apply worker started; media keys apply "
                  "here, the syscall path applies inline");
}

// #162 boot proof. Both self-tests are vector tests over pure logic, so they
// cost microseconds, touch no hardware and cannot move the speaker volume.
// They exist because neither thing they test can be exercised by a VM: no
// QEMU device presents a USB consumer-control collection, and a mute/restore
// round trip is invisible without an amplifier. Without these the code would
// ship having never executed, which is the zero-callers trap this tree keeps
// rediscovering.
void sysvol_selftest(void) {
    uint32_t checks = 0;
    uint32_t fails = sysvol_selftest_rs(&checks);
    if (fails == 0) {
        kprintf("[SYSVOL] volume/mute state-machine self-test PASS (%u checks)\n",
                checks);
        bootlog_write("[SYSVOL] state-machine self-test PASS (%u checks)", checks);
    } else {
        kprintf("[SYSVOL] *** state-machine self-test FAIL: %u/%u checks failed ***\n",
                fails, checks);
        bootlog_write("[SYSVOL] *** state-machine self-test FAIL %u/%u ***",
                      fails, checks);
    }

    uint32_t hchecks = 0, hfirst = 0;
    int hfails = hidrepd_selftest_rs(&hchecks, &hfirst);
    if (hfails == 0) {
        kprintf("[HIDCONS] report-descriptor parser self-test PASS (%u checks)\n",
                hchecks);
        bootlog_write("[HIDCONS] report-descriptor parser self-test PASS (%u checks)",
                      hchecks);
    } else {
        kprintf("[HIDCONS] *** report-descriptor parser self-test FAIL: %d/%u, "
                "first failing check #%u ***\n", hfails, hchecks, hfirst);
        bootlog_write("[HIDCONS] *** report-descriptor parser self-test FAIL %d/%u, "
                      "first failing check #%u ***", hfails, hchecks, hfirst);
    }
}
