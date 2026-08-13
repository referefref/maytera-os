/*
 * Audio_Maytera.c - ClassiCube audio backend for MayteraOS (task #28).
 *
 * WHAT SHIPPED, STATED UP FRONT SO NOBODY HAS TO GUESS:
 *   - STREAM path (music): REAL. It pushes 16-bit PCM into the kernel's
 *     Ring-3 PCM ring via SYS_AUDIO_PCM_OPEN/WRITE/CLOSE (315/316/317).
 *     UNVERIFIED END TO END at the time of writing: there is currently NO
 *     other userland caller of those three syscalls anywhere in the tree
 *     (kernel/version.h names a music player that does not in fact call
 *     them), so this is their first Ring-3 user and it has not been heard
 *     playing. It fails CLOSED: every error path returns a cc_result and
 *     ClassiCube simply stops the music, so the worst case is silence.
 *   - SOUND path (dig/step one-shots): DELIBERATELY SILENT, and silent by
 *     construction rather than by lying. AudioBackend_LoadSounds is a no-op,
 *     so no sound data is ever loaded, so Soundboard_PickRandom always
 *     returns NULL and Sounds_Play returns before it ever reaches the audio
 *     API. Nothing reports a fake success.
 *
 * WHY SOUNDS CANNOT WORK YET, which is a property of the OS and not a
 * shortcut here. kernel/drivers/audio_pcm.c defines PCM_MAX_STREAMS as 1 and
 * enforces one owning process per stream. There is no mixer anywhere below
 * Ring 3. ClassiCube's sound pool (AudioPool_Play in _AudioBase.h) wants up
 * to 8 contexts playing concurrently, and dig/step sounds routinely overlap.
 * With one non-mixing stream, the honest options were "no sounds" or "sounds
 * that cut the music dead and each other dead". The upgrade path is a real
 * one: either raise PCM_MAX_STREAMS and mix in the kernel pump, or add a
 * small userland mixer that owns the single stream and accepts N voices.
 * Until one of those exists, this file does not pretend.
 *
 * BLOCKING BEHAVIOUR. sys_audio_pcm_write() blocks when the kernel ring is
 * full and wakes from the pump's wait queue (it is a proper #426 wait, not a
 * spin), and sys_audio_pcm_close() blocks until the ring has drained. Both
 * are therefore only safe off the render thread. That is exactly where they
 * happen: ClassiCube runs music on its own thread (Music_PlayOgg, started by
 * Music_Start), and this backend is never touched from the game loop because
 * the sound path is disabled. StreamContext_Update always reports 0 buffers
 * in use, which is the truth for this design (a chunk handed to Enqueue has
 * already been copied into the kernel ring and is free for reuse); the
 * decoder is then paced naturally by Enqueue blocking when the ring is full,
 * so there is no poll loop anywhere in this file.
 *
 * BUILD INTEGRATION: this is a real ClassiCube audio backend and carries its
 * own id, CC_AUD_BACKEND_MAYTERA, added to the STAGED Core.h by
 * engine-patches/coreh-maytera.py and made the MayteraOS default there. The
 * guard below is what makes Audio_Null.c collapse to an empty translation unit
 * instead of defining every Audio_* symbol a second time. Needs
 * -I<ClassiCube>/src and -I<userland>/libc.
 */
#include "Core.h"
#if CC_AUD_BACKEND == CC_AUD_BACKEND_MAYTERA

struct AudioContext {
	/* Non-zero once Audio_Init has run. _AudioBase.h's AudioPool_Play uses
	 * this to decide whether a pool slot still needs initialising. */
	int count;
	int channels;
	int sampleRate;
	int volume;
	int handle;   /* kernel PCM stream handle, >= 1 when open */
};

/* No sound pool support: this backend supplies its own (empty)
 * AudioBackend_LoadSounds instead of the base one that loads the sound zip. */
#define AUDIO_OVERRIDE_SOUNDS
#include "_AudioBase.h"

/* MayteraOS userland libc: sys_audio_pcm_open/write/close. */
#include "syscall.h"

/* kernel/drivers/audio.h AUDIO_FORMAT_S16_LE. audio_pcm.h states plainly that
 * "format must be AUDIO_FORMAT_S16_LE", and ClassiCube decodes to signed
 * 16-bit host-endian (x86-64 is little-endian), so this is the only value
 * this backend can pass. Mirrored here because the kernel header is not
 * exported to Ring 3; kept next to the reason so it cannot drift silently. */
#define MAYT_AUDIO_FORMAT_S16_LE 0x0002

/* Negative returns from the PCM syscalls (kernel/drivers/audio_pcm.h). */
#define MAYT_PCM_EINVAL (-1)
#define MAYT_PCM_EBUSY  (-2)
#define MAYT_PCM_ENOMEM (-3)
#define MAYT_PCM_EINTR  (-4)
#define MAYT_PCM_EPERM  (-5)
#define MAYT_PCM_ENODEV (-6)

/* cc_result values this backend reports. 0x4D41 is 'M','A'. */
#define MAUD_ERR_OPEN_FAILED  0x4D410001UL
#define MAUD_ERR_NO_DEVICE    0x4D410002UL
#define MAUD_ERR_DEVICE_BUSY  0x4D410003UL
#define MAUD_ERR_WRITE_FAILED 0x4D410004UL
#define MAUD_ERR_NO_STREAM    0x4D410005UL
#define MAUD_ERR_NO_SOUNDS    0x4D410006UL

static cc_result Mayt_MapPcmError(int rc) {
	switch (rc) {
	case MAYT_PCM_ENODEV: return MAUD_ERR_NO_DEVICE;
	case MAYT_PCM_EBUSY:  return MAUD_ERR_DEVICE_BUSY;
	case MAYT_PCM_ENOMEM: return ERR_OUT_OF_MEMORY;
	case MAYT_PCM_EPERM:  return MAUD_ERR_OPEN_FAILED;
	case MAYT_PCM_EINTR:  return MAUD_ERR_WRITE_FAILED;
	case MAYT_PCM_EINVAL: return ERR_INVALID_ARGUMENT;
	}
	return MAUD_ERR_OPEN_FAILED;
}

static void Mayt_CloseStream(struct AudioContext* ctx) {
	if (ctx->handle > 0) sys_audio_pcm_close(ctx->handle);
	ctx->handle     = 0;
	ctx->channels   = 0;
	ctx->sampleRate = 0;
}


/*########################################################################################################################*
*-------------------------------------------------------Backend hooks-----------------------------------------------------*
*#########################################################################################################################*/
/* True, so ClassiCube keeps its music subsystem alive. Whether a PCM device
 * actually exists is discovered by the open in StreamContext_SetFormat, which
 * is a real answer rather than a guess; probing here would mean opening and
 * immediately closing the OS's single PCM stream at startup, which can race
 * its own teardown (the kernel refuses a second open until the previous
 * stream's pump thread has exited). */
cc_bool AudioBackend_Init(void) { return true; }

void AudioBackend_Tick(void) { }
void AudioBackend_Free(void) { }

/* Deliberately empty. See the header comment: with no sound data loaded, the
 * whole one-shot sound path is never entered, so nothing has to fake a
 * success or emit a "disabling sounds" warning. */
void AudioBackend_LoadSounds(void) { }


/*########################################################################################################################*
*------------------------------------------------------Audio contexts-----------------------------------------------------*
*#########################################################################################################################*/
cc_result Audio_Init(struct AudioContext* ctx, int buffers) {
	ctx->count      = buffers;
	ctx->channels   = 0;
	ctx->sampleRate = 0;
	ctx->volume     = 100;
	ctx->handle     = 0;
	return 0;
}

void Audio_Close(struct AudioContext* ctx) {
	Mayt_CloseStream(ctx);
	ctx->count = 0;
}

void Audio_SetVolume(struct AudioContext* ctx, int volume) {
	ctx->volume = volume;
}

cc_bool Audio_DescribeError(cc_result res, cc_string* dst) {
	switch (res) {
	case MAUD_ERR_OPEN_FAILED:
		String_AppendConst(dst, "Could not open the audio device");
		return true;
	case MAUD_ERR_NO_DEVICE:
		String_AppendConst(dst, "No audio output device is available");
		return true;
	case MAUD_ERR_DEVICE_BUSY:
		String_AppendConst(dst, "The audio device is in use by another program");
		return true;
	case MAUD_ERR_WRITE_FAILED:
		String_AppendConst(dst, "Writing audio to the device failed");
		return true;
	case MAUD_ERR_NO_STREAM:
		String_AppendConst(dst, "No audio stream has been opened");
		return true;
	case MAUD_ERR_NO_SOUNDS:
		String_AppendConst(dst, "MayteraOS cannot mix sound effects yet");
		return true;
	}
	return false;
}


/*########################################################################################################################*
*------------------------------------------------------Stream context-----------------------------------------------------*
*#########################################################################################################################*/
cc_result StreamContext_SetFormat(struct AudioContext* ctx, int channels, int sampleRate, int playbackRate) {
	int rate = Audio_AdjustSampleRate(sampleRate, playbackRate);
	int h;

	if (ctx->handle > 0 && ctx->channels == channels && ctx->sampleRate == rate) return 0;
	Mayt_CloseStream(ctx);

	h = sys_audio_pcm_open((unsigned)rate, (unsigned)channels, MAYT_AUDIO_FORMAT_S16_LE);
	if (h < 1) return Mayt_MapPcmError(h);

	ctx->handle     = h;
	ctx->channels   = channels;
	ctx->sampleRate = rate;
	return 0;
}

cc_result StreamContext_Enqueue(struct AudioContext* ctx, struct AudioChunk* chunk) {
	cc_int16* samples = (cc_int16*)chunk->data;
	int numSamples, frames, i, n;

	if (ctx->handle < 1)  return MAUD_ERR_NO_STREAM;
	if (!chunk->size)     return 0;

	numSamples = (int)(chunk->size / sizeof(cc_int16));
	frames     = numSamples / ctx->channels;
	if (frames <= 0) return 0;

	/* Volume is applied here because the kernel PCM stream has no per-stream
	 * gain control. Scaling in place is safe for this caller: Music_Buffer
	 * decodes fresh samples into the chunk before every Enqueue, and this
	 * backend has finished with the buffer by the time the syscall returns
	 * (the kernel copies the frames into its own ring). */
	if (ctx->volume < 100) {
		for (i = 0; i < numSamples; i++)
		{
			samples[i] = (cc_int16)((samples[i] * ctx->volume) / 100);
		}
	}

	/* Blocks only while the kernel ring is full, on ClassiCube's own music
	 * thread, waking from the PCM pump's wait queue. */
	n = sys_audio_pcm_write(ctx->handle, samples, (unsigned)frames);
	if (n < 0) return Mayt_MapPcmError(n);
	return 0;
}

/* The stream starts playing as soon as it is opened (the kernel spawns its
 * pump thread there), so there is nothing to start. */
cc_result StreamContext_Play(struct AudioContext* ctx) {
	return ctx->handle > 0 ? 0 : MAUD_ERR_NO_STREAM;
}

/* No pause in the kernel PCM API. Reported as unsupported rather than
 * silently ignored; only the mobile builds call it. */
cc_result StreamContext_Pause(struct AudioContext* ctx) {
	(void)ctx;
	return ERR_NOT_SUPPORTED;
}

cc_result StreamContext_Update(struct AudioContext* ctx, int* inUse) {
	/* Zero is the truth here, not a stub: a chunk passed to Enqueue has been
	 * copied into the kernel ring by the time that call returns, so no chunk
	 * this side of the syscall is still in use. Pacing comes from Enqueue
	 * blocking on a full ring, not from this count. */
	(void)ctx;
	*inUse = 0;
	return 0;
}


/*########################################################################################################################*
*------------------------------------------------------Sound context------------------------------------------------------*
*#########################################################################################################################*/
/* These three exist only to satisfy the link: with AudioBackend_LoadSounds
 * empty, no sound is ever loaded, so AudioPool_Play is never reached. If a
 * future change does load sounds, PlayData returns a described error exactly
 * once and ClassiCube turns sounds off, which is the correct visible outcome
 * for an OS that cannot mix them. */
cc_bool SoundContext_FastPlay(struct AudioContext* ctx, struct AudioData* data) {
	(void)ctx; (void)data;
	return true;
}

cc_result SoundContext_PlayData(struct AudioContext* ctx, struct AudioData* data) {
	(void)ctx; (void)data;
	return MAUD_ERR_NO_SOUNDS;
}

cc_result SoundContext_PollBusy(struct AudioContext* ctx, cc_bool* isBusy) {
	(void)ctx;
	*isBusy = false;
	return 0;
}

#endif /* CC_AUD_BACKEND == CC_AUD_BACKEND_MAYTERA */
