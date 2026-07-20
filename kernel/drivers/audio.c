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
#include "../proc/thread.h"
#include "../proc/process.h"

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
    kprintf("[AUDIO] Initializing audio subsystem...\n");

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
        kprintf("[AUDIO] Using USB Audio Class DAC (%u Hz)\n", uac_sample_rate());
        LOG_INFO("[Audio] USB Audio DAC initialized successfully");
        return AUDIO_OK;
    }

    // Try Intel HDA first
    if (hda_init() == AUDIO_OK) {
        audio_state.device_type = AUDIO_DEVICE_HDA;
        hda_get_device_info(&audio_state.device_info);
        audio_state.initialized = true;
        kprintf("[AUDIO] Using Intel HDA audio\n");
        LOG_INFO("[Audio] Intel HDA initialized successfully");
        return AUDIO_OK;
    }

    // Try AC97
    if (ac97_init() == AUDIO_OK) {
        audio_state.device_type = AUDIO_DEVICE_AC97;
        ac97_get_device_info(&audio_state.device_info);
        audio_state.initialized = true;
        kprintf("[AUDIO] Using AC97 audio\n");
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
        kprintf("[AUDIO] Using Sound Blaster 16 audio\n");
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

    kprintf("[AUDIO] No audio device found, PC Speaker fallback\n");
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
    {
        extern fat_fs_t g_fat_fs;
        uint32_t __asz = 0;
        void *__acfg = g_fat_fs.mounted
                     ? fat_read_file(&g_fat_fs, "/CONFIG/AUDIODMP.CFG", &__asz)
                     : NULL;
        if (__acfg) {
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

int audio_stop(audio_stream_t *stream) {
    if (!stream || !stream->active) {
        return AUDIO_ERR_INVALID_PARAM;
    }

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

int audio_write_nonblock(audio_stream_t *stream, const void *buffer, uint32_t frames) {
    if (!stream || !stream->active || !buffer) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    // Check available space
    int avail = audio_avail(stream);
    if (avail <= 0) {
        return 0;
    }

    // Write only what fits
    uint32_t to_write = (frames < (uint32_t)avail) ? frames : (uint32_t)avail;
    return audio_write(stream, buffer, to_write);
}

int audio_avail(audio_stream_t *stream) {
    if (!stream || !stream->active) {
        return AUDIO_ERR_INVALID_PARAM;
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

    audio_config_t config = {
        .format = format,
        .sample_rate = sample_rate,
        .channels = channels,
        .buffer_size = 0,
        .period_size = 0
    };

    audio_stream_t *stream = audio_open(&config);
    if (!stream) {
        return AUDIO_ERR_NO_DEVICE;
    }

    // Calculate frame size
    uint32_t bits = 16;
    if (format == AUDIO_FORMAT_U8) bits = 8;
    else if (format == AUDIO_FORMAT_S24_LE || format == AUDIO_FORMAT_S32_LE) bits = 32;
    uint32_t bytes_per_frame = (bits / 8) * channels;
    uint32_t frames = size / bytes_per_frame;

    // Write and start
    int ret = audio_write(stream, data, frames);
    if (ret > 0) {
        audio_start(stream);
        audio_drain(stream);
    }

    audio_close(stream);
    return (ret > 0) ? AUDIO_OK : ret;
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

    audio_config_t cfg = { .format = AUDIO_FORMAT_S16_LE, .sample_rate = rate,
                           .channels = (uint32_t)ch, .buffer_size = 0, .period_size = 0 };
    audio_stream_t *st = audio_open(&cfg);
    if (!st) { audio_decode_close(dec); kfree(fdata); return -1; }

    int16_t *pcm = (int16_t *)kmalloc(8192 * sizeof(int16_t));
    if (!pcm) { audio_close(st); audio_decode_close(dec); kfree(fdata); return -1; }

    audio_start(st);
    // #331/#347 FLAC-freeze fix v2: PACE the decode to real-time. proc_sleep() takes
    // MILLISECONDS. The old loop decoded flat-out and yielded only on sink
    // backpressure; a heavy codec (dr_flac, no SIMD) with a sink that provides little
    // backpressure burned ~100% CPU holding the kernel BKL and starved the desktop
    // (fully frozen -> after a tiny yield, churning). Instead keep only ~LEAD_MS of
    // audio decoded ahead of the wall clock, then proc_sleep() the surplus so the BKL
    // is released for the compositor/input while playback catches up. Standard-rate
    // FLAC/MP3 decode far faster than realtime -> the thread sleeps most of the time.
    // (Hi-res 24/96 FLAC can still saturate the CPU: decode ~= realtime leaves no
    // slack; that needs SIMD or finer-grained locking - a separate, larger fix.)
    extern uint32_t g_timer_hz;
    extern volatile uint64_t timer_ticks;
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t start_ms = (uint64_t)timer_ticks * 1000 / hz;
    uint64_t queued_ms = 0;
    const uint32_t LEAD_MS = 250;
    int n, stalled = 0;
    while (!stalled && (n = audio_decode_read(dec, pcm, 2048)) > 0) {
        int chunk_frames = n / ch;
        const int16_t *p = pcm;
        int frames = chunk_frames;
        int guard = 0;
        while (frames > 0) {
            int w = audio_write(st, p, frames);
            if (w > 0) { p += (uint32_t)w * ch; frames -= w; guard = 0; }
            else { proc_sleep(2); if (++guard > 1500) { stalled = 1; break; } }
        }
        queued_ms += (uint64_t)chunk_frames * 1000 / rate;
        uint64_t elapsed_ms = ((uint64_t)timer_ticks * 1000 / hz) - start_ms;
        if (queued_ms > elapsed_ms + LEAD_MS) {
            uint32_t sl = (uint32_t)(queued_ms - elapsed_ms - LEAD_MS);
            if (sl > 500) sl = 500;
            proc_sleep(sl);   // pace to real-time: release the BKL for the desktop
        } else {
            proc_sleep(1);    // decode behind realtime: still yield a little
        }
    }
    if (stalled)
        kprintf("[AUDIO] %s: output not draining (no audio sink consuming)\n", path);

    audio_drain(st);
    audio_close(st);
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
static void audio_boot_sound_worker(void *arg) {
    (void)arg;
    for (int waited_ms = 0; waited_ms < 8000; waited_ms += 100) {
        if (audio_is_available()) break;
        proc_sleep(100);
    }
    audio_play_file("/SOUNDS/BOOTSND.MP3");
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
