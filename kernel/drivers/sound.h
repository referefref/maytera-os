// sound.h - Sound Blaster 16 audio driver
#ifndef SOUND_H
#define SOUND_H

#include "../types.h"

// Sound Blaster base I/O ports to probe
#define SB_BASE_220     0x220
#define SB_BASE_240     0x240
#define SB_BASE_260     0x260
#define SB_BASE_280     0x280

// Sound Blaster register offsets (from base port)
#define SB_MIXER_ADDR   0x04    // Mixer Address
#define SB_MIXER_DATA   0x05    // Mixer Data
#define SB_DSP_RESET    0x06    // DSP Reset
#define SB_DSP_READ     0x0A    // DSP Read Data
#define SB_DSP_WRITE    0x0C    // DSP Write Data/Command
#define SB_DSP_WSTAT    0x0C    // DSP Write Status (bit 7)
#define SB_DSP_RSTAT    0x0E    // DSP Read Status (bit 7)

// DSP commands
#define DSP_CMD_SET_TIME_CONST      0x40    // Set time constant
#define DSP_CMD_SET_SAMPLE_RATE     0x41    // Set sample rate (SB16)
#define DSP_CMD_SPEAKER_ON          0xD1    // Turn speaker on
#define DSP_CMD_SPEAKER_OFF         0xD3    // Turn speaker off
#define DSP_CMD_STOP_8BIT           0xD0    // Pause 8-bit DMA
#define DSP_CMD_CONT_8BIT           0xD4    // Continue 8-bit DMA
#define DSP_CMD_STOP_16BIT          0xD5    // Pause 16-bit DMA
#define DSP_CMD_CONT_16BIT          0xD6    // Continue 16-bit DMA
#define DSP_CMD_GET_VERSION         0xE1    // Get DSP version
#define DSP_CMD_8BIT_OUT            0x10    // 8-bit direct DAC output
#define DSP_CMD_8BIT_DMA_OUT        0x14    // 8-bit single-cycle DMA output
#define DSP_CMD_8BIT_DMA_OUT_AI     0x1C    // 8-bit auto-init DMA output
#define DSP_CMD_8BIT_DMA_OUT_HS     0x91    // 8-bit high-speed DMA output
#define DSP_CMD_16BIT_DMA_OUT       0xB0    // 16-bit DMA output (SB16)
#define DSP_CMD_8BIT_DMA_OUT_SB16   0xC0    // 8-bit DMA output (SB16)

// DSP transfer modes for SB16 commands
#define DSP_MODE_MONO_UNSIGNED      0x00
#define DSP_MODE_MONO_SIGNED        0x10
#define DSP_MODE_STEREO_UNSIGNED    0x20
#define DSP_MODE_STEREO_SIGNED      0x30

// DMA channels
#define DMA_CHANNEL_8BIT            1       // 8-bit DMA channel
#define DMA_CHANNEL_16BIT           5       // 16-bit DMA channel

// DMA controller ports (8-bit, channels 0-3)
#define DMA1_MASK_REG               0x0A
#define DMA1_MODE_REG               0x0B
#define DMA1_CLEAR_FF               0x0C
#define DMA1_CH1_ADDR               0x02
#define DMA1_CH1_COUNT              0x03
#define DMA1_CH1_PAGE               0x83

// DMA controller ports (16-bit, channels 4-7)
#define DMA2_MASK_REG               0xD4
#define DMA2_MODE_REG               0xD6
#define DMA2_CLEAR_FF               0xD8
#define DMA2_CH5_ADDR               0xC4
#define DMA2_CH5_COUNT              0xC6
#define DMA2_CH5_PAGE               0x8B

// Mixer registers
#define MIXER_MASTER_VOL            0x22    // Master volume
#define MIXER_VOICE_VOL             0x04    // Voice/DAC volume
#define MIXER_IRQ_SELECT            0x80    // IRQ select
#define MIXER_DMA_SELECT            0x81    // DMA select
#define MIXER_IRQ_STATUS            0x82    // IRQ status

// Sound driver status
#define SOUND_STATUS_OK             0
#define SOUND_STATUS_NOT_FOUND      -1
#define SOUND_STATUS_RESET_FAILED   -2
#define SOUND_STATUS_DMA_ERROR      -3

// Sound driver state structure
typedef struct {
    uint16_t base_port;         // Base I/O port (0x220, 0x240, etc.)
    uint8_t  dsp_version_major; // DSP version major
    uint8_t  dsp_version_minor; // DSP version minor
    uint8_t  irq;               // IRQ number
    uint8_t  dma_8bit;          // 8-bit DMA channel
    uint8_t  dma_16bit;         // 16-bit DMA channel
    bool     detected;          // Card detected
    bool     playing;           // Currently playing
} sound_state_t;

// Initialize sound driver (detect and initialize Sound Blaster)
int sound_init(void);

// Play a simple tone (using PC speaker or direct DAC)
// frequency: tone frequency in Hz (20-20000)
// duration_ms: duration in milliseconds
void sound_play_tone(uint32_t frequency, uint32_t duration_ms);

// Play PCM audio buffer
// data: pointer to PCM data (8-bit unsigned or 16-bit signed)
// size: size of data in bytes
// sample_rate: sample rate in Hz (e.g., 8000, 11025, 22050, 44100)
// Returns 0 on success, negative on error
int sound_play_buffer(const void *data, uint32_t size, uint32_t sample_rate);

// Stop playback
void sound_stop(void);

// Set master volume (0-255)
void sound_set_volume(uint8_t volume);

// Get sound driver state
sound_state_t *sound_get_state(void);

// Check if sound card is detected
bool sound_is_detected(void);

// Check if currently playing
bool sound_is_playing(void);

// Print sound card info
void sound_print_info(void);

#endif // SOUND_H
