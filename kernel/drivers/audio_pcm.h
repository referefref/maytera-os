// audio_pcm.h - Ring-3 PCM push interface (#426-clean), Phase 1 of the Ring-0
// media-decode exit.
//
// WHY THIS EXISTS
// ---------------
// Today the ONLY way Ring 3 can make a sound from a media file is
// SYS_PLAY_WAV -> sys_play_wav -> audio_play_file -> audio_decode_open ->
// {faad2, opus, tremor, dr_flac, libmad}. That drags ~121,000 lines of
// vendored decoder C, parsing an ATTACKER-CONTROLLED file, into Ring 0.
// MAYTERA-SEC-2026-0009 (CWE-125 heap OOB read in media/aac.c mp4_parse, via a
// crafted .m4a) is exactly that class of bug, and it is reachable from Ring 3.
//
// This interface breaks the dependency: userland decodes, and pushes raw PCM.
// A decoder bug then kills ONE Ring-3 process instead of reading kernel heap.
//
// THIS IS NOT A PARALLEL AUDIO PATH. It feeds the SAME sinks, through the SAME
// primitives, as audio_play_file():
//   - USB DAC present -> uac_stream_source(), the same gapless iso ring-refill
//     streamer, with a fill callback shaped exactly like uac_file_fill(). The
//     ONLY difference is that the callback pulls from a Ring-3-fed ring instead
//     of from an in-kernel audio_decoder_t. Rate snap, master gain and mute
//     (uac_apply_gain) are untouched and still apply.
//   - Otherwise -> audio_open()/audio_start()/audio_write()/audio_drain()/
//     audio_close(), the existing stream API, unchanged.
//
// #426 (NO BUSY-WAIT) - the three waits, and what wakes each:
//   1. Writer (Ring 3) with a full ring:
//        wait_event_interruptible(&s->wq_space, space || s->stop)
//      WOKEN BY: the pump thread, which calls wake_up_all(&s->wq_space) every
//      time it consumes frames, and on every exit path (where it also sets
//      s->stop). A dead pump therefore always releases the writer.
//   2. Pump with an empty ring (HDA/AC97/SB16 sink only):
//        wait_event_interruptible(&s->wq_data, used || drain_req || stop)
//      WOKEN BY: sys_audio_pcm_write() after it copies frames in, and by
//      sys_audio_pcm_close()/audio_pcm_proc_exit() when they request drain.
//   3. Pump waiting for DAC space (HDA sink only):
//        wait_event_interruptible(hda_space_wq(), audio_avail() >= n || stop)
//      WOKEN BY: hda_service_stream(), i.e. the real BCIS buffer-completion
//      interrupt via hda_msi_isr(), AND the pre-existing 10 ms hda_poll_worker.
//      Two independent sources, so a wake cannot be lost for >10 ms even on a
//      controller where MSI never arms.
// The UAC pull callback NEVER blocks: on starvation it pads silence and reports
// a FULL batch (an underrun is not an EOF), so the iso ring is never stalled.
// See the comment on pcm_ring_fill() in audio_pcm.c for why that matters.
//
// Nothing here polls, spins, or proc_yield()s. The one paced-sleep fallback
// (AC97/SB16, which expose no completion event) is documented at its call site.

#ifndef AUDIO_PCM_H
#define AUDIO_PCM_H

#include "../types.h"

// Only S16_LE is accepted in Phase 1: it is what every decoder in media/ emits
// and what uac_stream_source() consumes. Anything else is rejected rather than
// silently mis-played.
#define AUDIO_PCM_MAX_CHANNELS   2
#define AUDIO_PCM_MIN_RATE       8000
#define AUDIO_PCM_MAX_RATE       96000

// Errors (negative; mirror the AUDIO_ERR_* spirit but are syscall-visible).
#define AUDIO_PCM_EINVAL   (-1)
#define AUDIO_PCM_EBUSY    (-2)
#define AUDIO_PCM_ENOMEM   (-3)
#define AUDIO_PCM_EINTR    (-4)
#define AUDIO_PCM_EPERM    (-5)
#define AUDIO_PCM_ENODEV   (-6)

// Open a PCM playback stream. format must be AUDIO_FORMAT_S16_LE.
// Returns a handle >= 1, or a negative AUDIO_PCM_* error.
int64_t audio_pcm_open(uint32_t rate, uint32_t channels, uint32_t format);

// Write `frames` interleaved S16 frames from the Ring-3 buffer `ubuf`.
// BLOCKS (wait-queue, never a spin) while the ring is full. Returns the number
// of frames accepted, or a negative AUDIO_PCM_* error.
int64_t audio_pcm_write(int handle, const void *ubuf, uint32_t frames);

// Request drain, join the pump thread, release the stream. Returns 0 or < 0.
int64_t audio_pcm_close(int handle);

// Called from proc_exit(): tear down any stream owned by `pid` whose owner died
// without calling close (the music player force-kills its --play helper with
// SIGKILL on a manual track switch, so this is a NORMAL path, not an edge case).
void audio_pcm_proc_exit(uint32_t pid);

#endif // AUDIO_PCM_H
