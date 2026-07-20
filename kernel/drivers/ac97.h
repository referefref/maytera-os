// ac97.h - Intel AC97 Audio Codec Driver
// 
// AC97 (Audio Codec '97) is a standard for integrated audio on motherboards.
// It's widely emulated in virtual machines (QEMU, VirtualBox, etc.)
//
// Architecture:
// - AC97 Controller (on PCI bus) manages DMA and interfaces with CPU
// - AC97 Codec (connected via AC-link) contains the actual DAC/ADC
// - Communication via Bus Master registers and Mixer registers

#ifndef AC97_H
#define AC97_H

#include "../types.h"
#include "audio.h"

// ============================================================================
// PCI Identification
// ============================================================================

// AC97 PCI Class: Multimedia controller, Audio device
#define AC97_PCI_CLASS      0x04
#define AC97_PCI_SUBCLASS   0x01

// Common AC97 controller vendor/device IDs
#define AC97_VENDOR_INTEL       0x8086
#define AC97_DEVICE_ICH         0x2415  // Intel 82801AA (ICH)
#define AC97_DEVICE_ICH0        0x2425  // Intel 82801AB (ICH0)
#define AC97_DEVICE_ICH2        0x2445  // Intel 82801BA (ICH2)
#define AC97_DEVICE_ICH3        0x2485  // Intel 82801CA (ICH3)
#define AC97_DEVICE_ICH4        0x24C5  // Intel 82801DB (ICH4)
#define AC97_DEVICE_ICH5        0x24D5  // Intel 82801EB (ICH5)

// QEMU AC97 device
#define AC97_VENDOR_REALTEK     0x8086  // Actually Intel in QEMU
#define AC97_DEVICE_QEMU        0x2415  // QEMU uses ICH emulation

// ============================================================================
// AC97 Bus Master Registers (NABMBAR - Native Audio Bus Master Base Address)
// ============================================================================

// Buffer Descriptor List registers (per channel: PCM In, PCM Out, Mic)
#define AC97_NABM_BDBAR         0x00    // Buffer Descriptor Base Address (32-bit)
#define AC97_NABM_CIV           0x04    // Current Index Value (8-bit)
#define AC97_NABM_LVI           0x05    // Last Valid Index (8-bit)
#define AC97_NABM_SR            0x06    // Status Register (16-bit)
#define AC97_NABM_PICB          0x08    // Position In Current Buffer (16-bit)
#define AC97_NABM_PIV           0x0A    // Prefetched Index Value (8-bit)
#define AC97_NABM_CR            0x0B    // Control Register (8-bit)

// Channel offsets from NABMBAR
#define AC97_CHAN_PCMIN         0x00    // PCM Input (recording)
#define AC97_CHAN_PCMOUT        0x10    // PCM Output (playback)
#define AC97_CHAN_MIC           0x20    // Microphone Input

// Global Control register
#define AC97_NABM_GLOB_CNT      0x2C    // Global Control (32-bit)
#define AC97_NABM_GLOB_STA      0x30    // Global Status (32-bit)

// Status Register bits
#define AC97_SR_DCH             (1 << 0)    // DMA Controller Halted
#define AC97_SR_CELV            (1 << 1)    // Current Equals Last Valid
#define AC97_SR_LVBCI           (1 << 2)    // Last Valid Buffer Completion Interrupt
#define AC97_SR_BCIS            (1 << 3)    // Buffer Completion Interrupt Status
#define AC97_SR_FIFOE           (1 << 4)    // FIFO Error

// Control Register bits
#define AC97_CR_RPBM            (1 << 0)    // Run/Pause Bus Master
#define AC97_CR_RR              (1 << 1)    // Reset Registers
#define AC97_CR_LVBIE           (1 << 2)    // Last Valid Buffer Interrupt Enable
#define AC97_CR_FEIE            (1 << 3)    // FIFO Error Interrupt Enable
#define AC97_CR_IOCE            (1 << 4)    // Interrupt On Completion Enable

// Global Control bits
#define AC97_GC_GIE             (1 << 0)    // GPI Interrupt Enable
#define AC97_GC_COLD_RESET      (1 << 1)    // Cold Reset
#define AC97_GC_WARM_RESET      (1 << 2)    // Warm Reset
#define AC97_GC_SD              (1 << 3)    // Shut Down
#define AC97_GC_PR_RESUME       (1 << 4)    // PR Resume Interrupt Enable
#define AC97_GC_SC_RESUME       (1 << 5)    // SC Resume Interrupt Enable
#define AC97_GC_PR_DETECT       (1 << 8)    // Primary Codec Ready
#define AC97_GC_SC_DETECT       (1 << 9)    // Secondary Codec Ready
#define AC97_GC_20BIT           (0 << 20)   // 20-bit samples
#define AC97_GC_STEREO          (0 << 21)   // Stereo output

// Global Status bits
#define AC97_GS_MD3             (1 << 0)    // Modem Detect 3 (not used)
#define AC97_GS_AD3             (1 << 1)    // Audio Detect 3 (not used)
#define AC97_GS_RCS             (1 << 2)    // Read Completion Status
#define AC97_GS_B3S12           (1 << 4)    // Bit 3 of slot 12
#define AC97_GS_B2S12           (1 << 5)    // Bit 2 of slot 12
#define AC97_GS_B1S12           (1 << 6)    // Bit 1 of slot 12
#define AC97_GS_SRI             (1 << 7)    // Secondary Resume Interrupt
#define AC97_GS_PRI             (1 << 8)    // Primary Resume Interrupt
#define AC97_GS_SCR             (1 << 9)    // Secondary Codec Ready
#define AC97_GS_PCR             (1 << 10)   // Primary Codec Ready
#define AC97_GS_MINT            (1 << 11)   // Mic In Interrupt
#define AC97_GS_POINT           (1 << 12)   // PCM Out Interrupt
#define AC97_GS_PIINT           (1 << 13)   // PCM In Interrupt
#define AC97_GS_GSCI            (1 << 19)   // GPI Status Change Interrupt
#define AC97_GS_MIINT           (1 << 22)   // Modem In Interrupt
#define AC97_GS_MOINT           (1 << 23)   // Modem Out Interrupt

// ============================================================================
// AC97 Mixer Registers (NAMBAR - Native Audio Mixer Base Address)
// ============================================================================

#define AC97_RESET              0x00    // Reset (write any value)
#define AC97_MASTER_VOL         0x02    // Master Volume
#define AC97_AUX_OUT_VOL        0x04    // Aux Out Volume (Headphone)
#define AC97_MONO_VOL           0x06    // Mono Out Volume
#define AC97_MASTER_TONE        0x08    // Master Tone (bass/treble)
#define AC97_PC_BEEP_VOL        0x0A    // PC Beep Volume
#define AC97_PHONE_VOL          0x0C    // Phone Input Volume
#define AC97_MIC_VOL            0x0E    // Microphone Volume
#define AC97_LINEIN_VOL         0x10    // Line In Volume
#define AC97_CD_VOL             0x12    // CD Volume
#define AC97_VIDEO_VOL          0x14    // Video Volume
#define AC97_AUX_VOL            0x16    // Aux In Volume
#define AC97_PCM_OUT_VOL        0x18    // PCM Out Volume (DAC)
#define AC97_RECORD_SELECT      0x1A    // Record Source Select
#define AC97_RECORD_GAIN        0x1C    // Record Gain
#define AC97_RECORD_GAIN_MIC    0x1E    // Record Gain Mic
#define AC97_GENERAL_PURPOSE    0x20    // General Purpose
#define AC97_3D_CONTROL         0x22    // 3D Control
#define AC97_INT_PAGING         0x24    // Interrupt/Paging (AC97 2.3)
#define AC97_POWERDOWN          0x26    // Powerdown Control/Status
#define AC97_EXT_AUDIO_ID       0x28    // Extended Audio ID
#define AC97_EXT_AUDIO_CTRL     0x2A    // Extended Audio Control
#define AC97_PCM_FRONT_RATE     0x2C    // PCM Front DAC Rate
#define AC97_PCM_SURR_RATE      0x2E    // PCM Surround DAC Rate
#define AC97_PCM_LFE_RATE       0x30    // PCM LFE DAC Rate
#define AC97_PCM_LR_ADC_RATE    0x32    // PCM L/R ADC Rate
#define AC97_MIC_ADC_RATE       0x34    // Mic ADC Rate
#define AC97_6CH_VOL_C_LFE      0x36    // 6-ch Volume Center/LFE
#define AC97_6CH_VOL_SURR       0x38    // 6-ch Volume Surround
#define AC97_SPDIF_CTRL         0x3A    // S/PDIF Control
#define AC97_VENDOR_ID1         0x7C    // Vendor ID 1
#define AC97_VENDOR_ID2         0x7E    // Vendor ID 2

// Volume register format: Bit 15 = mute, Bits 13:8 = left, Bits 5:0 = right
// Volume is attenuation in 1.5dB steps (0 = 0dB, 63 = -94.5dB)
#define AC97_VOL_MUTE           (1 << 15)
#define AC97_VOL_LEFT(x)        (((x) & 0x3F) << 8)
#define AC97_VOL_RIGHT(x)       ((x) & 0x3F)
#define AC97_VOL(l, r)          (AC97_VOL_LEFT(l) | AC97_VOL_RIGHT(r))

// Extended Audio ID bits
#define AC97_EXT_VRA            (1 << 0)    // Variable Rate Audio
#define AC97_EXT_DRA            (1 << 1)    // Double Rate Audio
#define AC97_EXT_SPDIF          (1 << 2)    // S/PDIF
#define AC97_EXT_VRM            (1 << 3)    // Variable Rate Mic
#define AC97_EXT_DSA_MASK       (3 << 4)    // DAC Slot Assignment
#define AC97_EXT_CDAC           (1 << 6)    // Center DAC
#define AC97_EXT_SDAC           (1 << 7)    // Surround DAC
#define AC97_EXT_LDAC           (1 << 8)    // LFE DAC
#define AC97_EXT_AMAP           (1 << 9)    // Alternate channel mapping
#define AC97_EXT_REV_MASK       (3 << 10)   // AC97 Revision

// Extended Audio Control bits
#define AC97_EXTC_VRA           (1 << 0)    // Enable Variable Rate Audio
#define AC97_EXTC_DRA           (1 << 1)    // Enable Double Rate Audio
#define AC97_EXTC_SPDIF         (1 << 2)    // Enable S/PDIF
#define AC97_EXTC_VRM           (1 << 3)    // Enable Variable Rate Mic
#define AC97_EXTC_SPSA_MASK     (3 << 4)    // S/PDIF Slot Assignment
#define AC97_EXTC_CDAC          (1 << 6)    // Center DAC Ready
#define AC97_EXTC_SDAC          (1 << 7)    // Surround DAC Ready
#define AC97_EXTC_LDAC          (1 << 8)    // LFE DAC Ready
#define AC97_EXTC_MADC          (1 << 9)    // Mic ADC Ready
#define AC97_EXTC_SPCV          (1 << 10)   // S/PDIF Configuration Valid

// Powerdown bits
#define AC97_PWR_ADC            (1 << 0)    // ADC Ready
#define AC97_PWR_DAC            (1 << 1)    // DAC Ready
#define AC97_PWR_ANL            (1 << 2)    // Analog Ready
#define AC97_PWR_REF            (1 << 3)    // Vref Ready
#define AC97_PWR_PR0            (1 << 8)    // Powerdown ADC
#define AC97_PWR_PR1            (1 << 9)    // Powerdown DAC
#define AC97_PWR_PR2            (1 << 10)   // Powerdown Analog
#define AC97_PWR_PR3            (1 << 11)   // Powerdown Digital
#define AC97_PWR_PR4            (1 << 12)   // Powerdown AC-link
#define AC97_PWR_PR5            (1 << 13)   // Powerdown Clk
#define AC97_PWR_PR6            (1 << 14)   // Powerdown EAPD
#define AC97_PWR_EAPD           (1 << 15)   // External Amp Powerdown

// ============================================================================
// Buffer Descriptor Structure
// ============================================================================

// Buffer Descriptor Entry (8 bytes)
typedef struct {
    uint32_t addr;              // Physical address of buffer
    uint16_t samples;           // Number of samples (not bytes\!) - 1
    uint16_t control;           // Control bits
} __attribute__((packed)) ac97_bd_t;

// BD Control bits
#define AC97_BD_IOC             (1 << 15)   // Interrupt On Completion
#define AC97_BD_BUP             (1 << 14)   // Buffer Underrun Policy

// Number of buffer descriptors (must be power of 2, max 32)
#define AC97_NUM_BD             32

// ============================================================================
// AC97 Driver State
// ============================================================================

typedef struct {
    // PCI information
    uint8_t  pci_bus;
    uint8_t  pci_slot;
    uint8_t  pci_func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  irq;

    // I/O bases
    uint32_t nambar;            // Native Audio Mixer BAR (BAR0)
    uint32_t nabmbar;           // Native Audio Bus Master BAR (BAR1)

    // Codec information
    uint16_t codec_vendor;
    uint16_t codec_revision;
    bool     supports_vra;      // Variable Rate Audio
    bool     supports_spdif;    // S/PDIF output
    uint32_t max_sample_rate;

    // Current configuration
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;

    // DMA buffers (ring buffer of buffer descriptors)
    ac97_bd_t *bd_list;         // Buffer descriptor list (physical, aligned)
    uint64_t  bd_list_phys;     // Physical address of BD list
    void     *dma_buffer;       // DMA buffer memory
    uint64_t  dma_buffer_phys;  // Physical address of DMA buffer
    uint32_t  dma_buffer_size;  // Total DMA buffer size
    uint32_t  bd_buffer_size;   // Size per BD entry

    // Playback state
    bool     initialized;
    bool     playing;
    uint8_t  write_index;       // Next BD to fill with data
    uint8_t  read_index;        // Current BD being played

    // Statistics
    uint64_t frames_played;
    uint64_t underruns;
} ac97_state_t;

// ============================================================================
// AC97 Driver API
// ============================================================================

// Initialize AC97 driver
// Returns: AUDIO_OK on success, error code on failure
int ac97_init(void);

// Shutdown AC97 driver
void ac97_shutdown(void);

// Check if AC97 is available
bool ac97_is_available(void);

// Get AC97 device info
int ac97_get_device_info(audio_device_info_t *info);

// Configure audio output
int ac97_configure(uint32_t format, uint32_t sample_rate, uint32_t channels);

// Start playback
int ac97_start(void);

// Stop playback
int ac97_stop(void);

// Write audio data
// Returns number of frames written
int ac97_write(const void *buffer, uint32_t frames);

// Get available write space (in frames)
int ac97_avail(void);

// Set volume (0-63 for each channel, or AC97_VOL_MUTE)
void ac97_set_volume(uint8_t left, uint8_t right);

// Set PCM volume
void ac97_set_pcm_volume(uint8_t left, uint8_t right);

// Mute/unmute
void ac97_mute(bool mute);

// Get driver state (for debugging)
ac97_state_t *ac97_get_state(void);

// Print AC97 info
void ac97_print_info(void);

// IRQ handler (called from interrupt)
void ac97_irq_handler(void);

#endif // AC97_H
