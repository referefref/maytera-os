// usb_audio.h - USB Audio Class 1.0 (UAC1) output driver
// #323: drives a passed-through NuForce USB DAC (262a:10aa) for real sound out.
#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include "../types.h"
#include "xhci.h"

// Called by xHCI enumeration when an Audio class interface is detected on a
// device. config is the full configuration descriptor blob (config_len bytes).
// Returns 0 if the device was claimed as a UAC output device.
int uac_probe(xhci_controller_t *xhc, int slot_id,
              uint16_t vid, uint16_t pid,
              const uint8_t *config, int config_len);

// True once a UAC output device has been set up and is ready to stream.
int uac_is_ready(void);

// Play a 440-style sine tone of freq Hz for duration_ms milliseconds (blocking-ish).
int uac_tone(uint32_t freq, uint32_t duration_ms);

// Play raw interleaved signed-16 stereo PCM at the configured device rate.
// data points to little-endian S16 stereo frames; frames is the frame count.
int uac_play_pcm(const int16_t *data, uint32_t frames);

// Configured device output sample rate (Hz), 0 if not ready.
uint32_t uac_sample_rate(void);

// #323: play a steady CONTINUOUS tone of freq Hz for duration_ms ms via the
// gapless iso ring-refill streaming worker. Returns 0 on a clean stop.
int uac_play_tone_continuous(uint32_t freq, uint32_t duration_ms);

// Spawn the deferred boot self-test worker (plays a tone if a DAC is present).
void uac_start_boot_selftest(void);

// #329: source callback for the gapless iso streaming engine. Fill up to
// `frames` interleaved S16 STEREO frames into dst at the DAC's output rate.
// Return the number of frames actually produced; a return < `frames` (or 0)
// signals end-of-stream and the streamer stops after the final (padded) batch.
typedef int (*uac_src_fn)(int16_t *dst, uint32_t frames, void *ctx);

// #329: stream arbitrary PCM from a source callback through the gapless
// ring-refill engine until the source signals EOF or max_ms elapses (backstop).
// This is the engine the music player's decoded PCM feeds through. Returns 0
// on a clean stop.
int uac_stream_source(uac_src_fn src, void *ctx, uint32_t max_ms);

// #329: set the DAC output sample rate for the next stream (snaps to a rate the
// NuForce accepts: 44100 or 48000). Returns 0 on success.
int uac_set_output_rate(uint32_t rate);

// #336: software master gain + mute for the USB DAC (the DAC has no mixer
// register, so the tray Volume slider / Mute scale the PCM in the stream path).
void uac_set_volume(int vol0_100);   // 0..100
void uac_set_mute(int mute);         // 0/1
int  uac_get_gain_q8(void);          // current effective gain (0 when muted)

// #335: total S16 stereo frames submitted to the DAC since the current stream
// started (for deriving elapsed playback time = frames / sample_rate).
uint64_t uac_frames_streamed(void);

#endif // USB_AUDIO_H
