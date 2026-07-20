// viz.h - MilkDrop-1-style TinyGL audio visualizer window for the Maytera HiFi
// player. Runs INSIDE the player process as a second window with its own libgl
// (TinyGL) surface. The player feeds it the live spectrum bars + real DAC
// playback position each frame; the viz drives the classic non-shader MilkDrop
// technique: a texture-feedback WARP mesh (previous frame copied to a GL texture,
// redrawn warped + decayed so trails/tunnels form) with live WAVEFORM + SPECTRUM
// overlays and an energy-based BEAT pulse. Several built-in presets cycle with a
// key/button. (hifi-milkdrop-viz)
#ifndef MP_VIZ_H
#define MP_VIZ_H

int  viz_open(void);          // create the viz window + GL context. 0 ok, <0 fail
int  viz_is_open(void);
void viz_close(void);
void viz_toggle(void);        // open if closed, close if open
void viz_next_preset(void);
int  viz_preset(void);
const char *viz_preset_name(void);

// Feed one frame of audio state: spectrum bars (each 0..63), how many, the
// playing flag (1 while the DAC is actively streaming), and the real DAC
// position in ms (SYS_AUDIO_POS_MS, -1 if idle). The position delta drives the
// animation phase so the visuals advance with the ACTUAL audio, not wall time.
void viz_set_audio(const int *bars, int nbars, int playing, long pos_ms);

// Render + blit one frame and pump the viz window's own events (close, preset
// keys). Marks the viz closed internally when its window is closed.
void viz_frame(void);

#endif
