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
// #205 (2026-08-26): THIS IS NOW A MIXER. It used to serve exactly one stream
// at a time (PCM_MAX_STREAMS 1) with one pump thread per stream. That made any
// long-lived producer - /APPS/FMSYNTH holds the sink for a whole DOS session -
// a machine-wide mute button for everything else, which is exactly what the
// owner hit: the MIDI player's Play was answered EBUSY and said nothing an end
// user could act on. There is now ONE mixer thread that sums every open stream
// into the single hardware stream, with fixed-point resampling per producer
// (rustkern/pcmmix.rs). See the block comment above pcm_mixer_worker().
//
// #426 (NO BUSY-WAIT) - the waits, and what wakes each:
//   1. Writer (Ring 3) with a full ring:
//        wait_event_interruptible(&s->wq_space, space || s->stop)
//      WOKEN BY: the pump thread, which calls wake_up_all(&s->wq_space) every
//      time it consumes frames, and on every exit path (where it also sets
//      s->stop). A dead pump therefore always releases the writer.
//   2. Mixer with every ring empty (HDA/AC97/SB16 sink only):
//        wait_event_interruptible(&g_mix_wq, mix_has_work() || !mix_any_active())
//      WOKEN BY: sys_audio_pcm_write() after it copies frames in, every open,
//      every close, and audio_pcm_proc_exit().
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

// ---------------------------------------------------------------------------
// #181: THE SAME STREAM, FED FROM RING 0.
//
// The DOS Sound Blaster emulation (kernel/dos/) is in-kernel by construction:
// the guest's PCM is already sitting in the interpreter's 1 MiB guest-memory
// buffer, so there is no Ring-3 process to own the stream and no user pointer
// to validate. Every other property of the stream is identical, so these
// entry points reuse the SAME ring, the SAME pump thread, the SAME UAC and
// HDA/AC97/SB16 sinks and the SAME #426 wake sources. They are a second DOOR,
// not a second audio path.
//
// The two differences, and only these two:
//   - no owner pid, so `pcm_lookup()`'s process-ownership gate does not apply
//     and a KERNEL handle is refused to a Ring-3 caller and vice versa;
//   - the source buffer is kernel memory, so no validate_user_ptr() and no
//     stac/clac bracket. Applying either to a kernel address would fail (the
//     U/S bit is clear) which is exactly why this cannot be the same call.
//
// #205: PCM_MAX_STREAMS IS NO LONGER 1, so this is no longer a competition. A
// DOS guest's Sound Blaster DMA, its OPL2 FM music and the MIDI player are three
// producers and all three now sound at once, summed by the mixer. EBUSY is still
// returned when every slot is taken, and the DOS side still treats it as "no
// sound this time" rather than waiting, which is the correct behaviour for a
// real-time producer that must keep running either way.
int64_t audio_pcm_open_kernel(uint32_t rate, uint32_t channels, uint32_t format);

// Write `frames` interleaved S16 frames from a KERNEL buffer. BLOCKS on the
// wait queue while the ring is full, exactly as the Ring-3 form does; that
// block IS the pacing for a real-time producer and must not be avoided.
int64_t audio_pcm_write_kernel(int handle, const int16_t *kbuf, uint32_t frames);

int64_t audio_pcm_close_kernel(int handle);

// Frames the pump has CONSUMED from the ring since the stream opened (a
// free-running 32-bit counter; differences are correct across wrap).
//
// This is the DOS side's clock. The emulated 8237's current word count is
// derived from it, so the count register a guest polls tracks what has actually
// been PLAYED rather than what has been queued, and the end-of-block interrupt
// fires on the same evidence.
uint32_t audio_pcm_consumed_kernel(int handle);

// Frames written but not yet consumed.
uint32_t audio_pcm_queued_kernel(int handle);

// #205: how many blocks the MIXER had to fill with silence for this stream
// because its ring was empty, i.e. how late THIS producer was. Distinct from
// hda_starve_stats(), which measures how late the MIXER was against the
// hardware. Before the mixer the two were the same event; they are not now, and
// audio_play_file() reports both separately for exactly that reason.
uint64_t audio_pcm_underruns_kernel(int handle);

// #426: block until the ring holds at most `max_used` frames, or `ms` elapse.
// Wake source: the pump's existing wake_up_all(&wq_space) on every consume,
// plus every teardown path. Returns WAIT_OK / WAIT_TIMEOUT.
int audio_pcm_wait_below_kernel(int handle, uint32_t max_used, uint32_t ms);

// #426: block until the pump has consumed at least `target` frames (compared
// as a signed difference, so it is wrap-correct), or `ms` elapse. Same wake
// source. Returns WAIT_OK / WAIT_TIMEOUT.
int audio_pcm_wait_consumed_kernel(int handle, uint32_t target, uint32_t ms);

// ---------------------------------------------------------------------------
// (#181 Ring-3 audio) SYS_AUDIO_PCM_CTL ops. The Ring-3 door to the counters
// and the two #426 waits above, for a stream the CALLING process owns.
// Scalar-only, so no argtab descriptor is needed.
#define AUDIO_PCM_CTL_CONSUMED      0   // -> frames consumed since open
#define AUDIO_PCM_CTL_QUEUED        1   // -> frames written but not consumed
#define AUDIO_PCM_CTL_UNDERRUNS     2   // -> mixer fills this stream was late for
#define AUDIO_PCM_CTL_WAIT_BELOW    3   // a=max_used frames, b=ms -> WAIT_OK/TIMEOUT
#define AUDIO_PCM_CTL_WAIT_CONSUMED 4   // a=target frames,   b=ms -> WAIT_OK/TIMEOUT
// AVAIL takes no handle: 1 when a real output device exists (codec or USB DAC),
// 0 otherwise. This is what the Ring-3 DOS host asks before it advertises a
// Sound Blaster to a guest, so it must be the KERNEL answering.
#define AUDIO_PCM_CTL_AVAIL         5

int64_t audio_pcm_ctl(int handle, uint32_t op, uint32_t a, uint32_t b);

// Called from proc_exit(): tear down any stream owned by `pid` whose owner died
// without calling close (the music player force-kills its --play helper with
// SIGKILL on a manual track switch, so this is a NORMAL path, not an edge case).
// (#181) `owner_tgid` is the exiting process's THREAD GROUP, and proc_exit()
// calls this only on a group-leader exit, exactly as it calls fdown_proc_exit()
// and async_http_proc_exit(). A stream is owned by the group (see pcm_lookup),
// so one thread of a live process exiting must NOT take its audio with it, and
// a stream opened by a worker thread must still be released when the process
// dies. A pid-exact match got both of those wrong; see the block comment on the
// definition in audio_pcm.c.
void audio_pcm_proc_exit(uint32_t owner_tgid);


// (#231r) The 5-band graphic EQ. The state and the DSP are in
// rustkern/pcmeq.rs and are reached directly by SYS_AUDIO_EQ; these two
// are the parts that need audio_pcm.c's own file-scope state.
void     audio_pcm_eq_log_now(void);      // AEQ_LOG -> /AUDIOLOG.TXT
uint32_t audio_pcm_eq_selftest_mask(void); // AEQ_SELFTEST, 0 = PASS

#endif // AUDIO_PCM_H
