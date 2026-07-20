// hda.h - Intel High Definition Audio Controller Driver
//
// Intel HDA (High Definition Audio) is the modern replacement for AC97.
// It provides higher quality audio with more features.
//
// Architecture:
// - HDA Controller: Memory-mapped, handles DMA and codec communication
// - HDA Codec(s): Connected via HDA Link, contain widgets (DAC, ADC, mixers, etc.)
// - Widgets: Functional units within a codec organized in a graph
//
// QEMU provides Intel HDA emulation with the ich9-intel-hda device.

#ifndef HDA_H
#define HDA_H

#include "../types.h"
#include "audio.h"

// ============================================================================
// PCI Identification
// ============================================================================

#define HDA_PCI_CLASS       0x04    // Multimedia
#define HDA_PCI_SUBCLASS    0x03    // Audio Device

// Intel HDA controller device IDs
#define HDA_VENDOR_INTEL    0x8086
#define HDA_DEVICE_ICH6     0x2668  // ICH6
#define HDA_DEVICE_ICH7     0x27D8  // ICH7
#define HDA_DEVICE_ICH8     0x284B  // ICH8
#define HDA_DEVICE_ICH9     0x293E  // ICH9
#define HDA_DEVICE_ICH10    0x3A3E  // ICH10
#define HDA_DEVICE_PCH      0x3B56  // PCH
#define HDA_DEVICE_PCHC     0x8C20  // PCH-C

// ============================================================================
// HDA Controller Registers (Memory-Mapped)
// ============================================================================

// Global Capabilities
#define HDA_REG_GCAP        0x00    // Global Capabilities (16-bit)
#define HDA_REG_VMIN        0x02    // Minor Version (8-bit)
#define HDA_REG_VMAJ        0x03    // Major Version (8-bit)
#define HDA_REG_OUTPAY      0x04    // Output Payload Capability (16-bit)
#define HDA_REG_INPAY       0x06    // Input Payload Capability (16-bit)
#define HDA_REG_GCTL        0x08    // Global Control (32-bit)
#define HDA_REG_WAKEEN      0x0C    // Wake Enable (16-bit)
#define HDA_REG_STATESTS    0x0E    // State Change Status (16-bit)
#define HDA_REG_GSTS        0x10    // Global Status (16-bit)

// Interrupt Control
#define HDA_REG_INTCTL      0x20    // Interrupt Control (32-bit)
#define HDA_REG_INTSTS      0x24    // Interrupt Status (32-bit)

// Wall Clock
#define HDA_REG_WALCLK      0x30    // Wall Clock Counter (32-bit)

// Stream Synchronization
#define HDA_REG_SSYNC       0x38    // Stream Synchronization (32-bit)

// CORB (Command Outbound Ring Buffer)
#define HDA_REG_CORBLBASE   0x40    // CORB Lower Base Address (32-bit)
#define HDA_REG_CORBUBASE   0x44    // CORB Upper Base Address (32-bit)
#define HDA_REG_CORBWP      0x48    // CORB Write Pointer (16-bit)
#define HDA_REG_CORBRP      0x4A    // CORB Read Pointer (16-bit)
#define HDA_REG_CORBCTL     0x4C    // CORB Control (8-bit)
#define HDA_REG_CORBSTS     0x4D    // CORB Status (8-bit)
#define HDA_REG_CORBSIZE    0x4E    // CORB Size (8-bit)

// RIRB (Response Inbound Ring Buffer)
#define HDA_REG_RIRBLBASE   0x50    // RIRB Lower Base Address (32-bit)
#define HDA_REG_RIRBUBASE   0x54    // RIRB Upper Base Address (32-bit)
#define HDA_REG_RIRBWP      0x58    // RIRB Write Pointer (16-bit)
#define HDA_REG_RINTCNT     0x5A    // Response Interrupt Count (16-bit)
#define HDA_REG_RIRBCTL     0x5C    // RIRB Control (8-bit)
#define HDA_REG_RIRBSTS     0x5D    // RIRB Status (8-bit)
#define HDA_REG_RIRBSIZE    0x5E    // RIRB Size (8-bit)

// Immediate Command (alternative to CORB/RIRB)
#define HDA_REG_ICOI        0x60    // Immediate Command Output Interface (32-bit)
#define HDA_REG_ICII        0x64    // Immediate Command Input Interface (32-bit)
#define HDA_REG_ICIS        0x68    // Immediate Command Status (16-bit)

// DMA Position Buffer
#define HDA_REG_DPLBASE     0x70    // DMA Position Lower Base (32-bit)
#define HDA_REG_DPUBASE     0x74    // DMA Position Upper Base (32-bit)

// Stream Descriptors (base + stream_index * 0x20)
#define HDA_REG_SD_BASE     0x80    // First stream descriptor
#define HDA_REG_SD_SIZE     0x20    // Size of each stream descriptor

// Stream Descriptor registers (offset from stream base)
#define HDA_SD_CTL          0x00    // Stream Descriptor Control (24-bit, use 32-bit access)
#define HDA_SD_STS          0x03    // Stream Descriptor Status (8-bit)
#define HDA_SD_LPIB         0x04    // Link Position in Current Buffer (32-bit)
#define HDA_SD_CBL          0x08    // Cyclic Buffer Length (32-bit)
#define HDA_SD_LVI          0x0C    // Last Valid Index (16-bit)
#define HDA_SD_FIFOW        0x0E    // FIFO Watermark (16-bit)
#define HDA_SD_FIFOS        0x10    // FIFO Size (16-bit)
#define HDA_SD_FMT          0x12    // Stream Format (16-bit)
#define HDA_SD_BDPL         0x18    // BDL Pointer Lower (32-bit)
#define HDA_SD_BDPU         0x1C    // BDL Pointer Upper (32-bit)

// ============================================================================
// Register Bit Definitions
// ============================================================================

// GCAP bits
#define HDA_GCAP_64OK       (1 << 0)    // 64-bit Address Supported
#define HDA_GCAP_NSDO_MASK  (3 << 1)    // Number of Serial Data Out signals
#define HDA_GCAP_BSS_MASK   (0x1F << 3) // Number of Bidirectional Streams
#define HDA_GCAP_ISS_MASK   (0xF << 8)  // Number of Input Streams
#define HDA_GCAP_OSS_MASK   (0xF << 12) // Number of Output Streams

// GCTL bits
#define HDA_GCTL_CRST       (1 << 0)    // Controller Reset (0=reset, 1=run)
#define HDA_GCTL_FCNTRL     (1 << 1)    // Flush Control
#define HDA_GCTL_UNSOL      (1 << 8)    // Accept Unsolicited Response Enable

// INTCTL bits
#define HDA_INTCTL_SIE_MASK 0x3FFFFFFF  // Stream Interrupt Enable (one bit per stream)
#define HDA_INTCTL_CIE      (1 << 30)   // Controller Interrupt Enable
#define HDA_INTCTL_GIE      (1 << 31)   // Global Interrupt Enable

// INTSTS bits
#define HDA_INTSTS_SIS_MASK 0x3FFFFFFF  // Stream Interrupt Status
#define HDA_INTSTS_CIS      (1 << 30)   // Controller Interrupt Status
#define HDA_INTSTS_GIS      (1 << 31)   // Global Interrupt Status

// CORBCTL bits
#define HDA_CORBCTL_RUN     (1 << 1)    // CORB DMA Engine Run
#define HDA_CORBCTL_MEIE    (1 << 0)    // CORB Memory Error Interrupt Enable

// RIRBCTL bits
#define HDA_RIRBCTL_RUN     (1 << 1)    // RIRB DMA Engine Run
#define HDA_RIRBCTL_DMAEN   (1 << 1)    // RIRB DMA Enable (alias)
#define HDA_RIRBCTL_RINTCTL (1 << 0)    // Response Interrupt Control

// Stream Descriptor Control bits
#define HDA_SD_CTL_SRST     (1 << 0)    // Stream Reset
#define HDA_SD_CTL_RUN      (1 << 1)    // Stream Run
#define HDA_SD_CTL_IOCE     (1 << 2)    // Interrupt on Completion Enable
#define HDA_SD_CTL_FEIE     (1 << 3)    // FIFO Error Interrupt Enable
#define HDA_SD_CTL_DEIE     (1 << 4)    // Descriptor Error Interrupt Enable
#define HDA_SD_CTL_STRIPE_MASK (3 << 16) // Stripe Control
#define HDA_SD_CTL_TP       (1 << 18)   // Traffic Priority
#define HDA_SD_CTL_DIR      (1 << 19)   // Bidirectional Direction (0=in, 1=out)
#define HDA_SD_CTL_STRM_MASK (0xF << 20) // Stream Number

// Stream Descriptor Status bits
#define HDA_SD_STS_BCIS     (1 << 2)    // Buffer Completion Interrupt Status
#define HDA_SD_STS_FIFOE    (1 << 3)    // FIFO Error
#define HDA_SD_STS_DESE     (1 << 4)    // Descriptor Error
#define HDA_SD_STS_FIFORDY  (1 << 5)    // FIFO Ready

// Stream Format register (HDA_SD_FMT)
#define HDA_FMT_CHAN_MASK   (0xF << 0)  // Number of channels - 1
#define HDA_FMT_BITS_MASK   (7 << 4)    // Bits per sample
#define HDA_FMT_BITS_8      (0 << 4)
#define HDA_FMT_BITS_16     (1 << 4)
#define HDA_FMT_BITS_20     (2 << 4)
#define HDA_FMT_BITS_24     (3 << 4)
#define HDA_FMT_BITS_32     (4 << 4)
#define HDA_FMT_DIV_MASK    (7 << 8)    // Stream/Sample rate divisor - 1
#define HDA_FMT_MULT_MASK   (7 << 11)   // Base rate multiplier - 1
#define HDA_FMT_BASE        (1 << 14)   // Base rate (0=48kHz, 1=44.1kHz)
#define HDA_FMT_TYPE        (1 << 15)   // Sample type (0=PCM, 1=non-PCM)

// ============================================================================
// HDA Codec Command/Response
// ============================================================================

// Codec command format (sent via CORB or Immediate Command)
// Bits 31:28 = Codec Address (CAd)
// Bits 27:20 = Node ID (NID)
// Bits 19:8  = Verb (command)
// Bits 7:0   = Payload

// Verb commands (4-bit or 12-bit)
// 4-bit verbs (bits 19:16)
#define HDA_VERB_GET_PARAM          (0xF00 << 8)   // Get Parameter
#define HDA_VERB_SET_CONV_FMT       (0x200 << 8)   // Set Converter Format (verb 0x2)
#define HDA_VERB_GET_CONN_SELECT    (0xF01 << 8)   // Get Connection Select Control
#define HDA_VERB_SET_CONN_SELECT    (0x701 << 8)   // Set Connection Select Control
#define HDA_VERB_GET_CONN_LIST      (0xF02 << 8)   // Get Connection List Entry
#define HDA_VERB_GET_PS             (0xF05 << 8)   // Get Power State
#define HDA_VERB_SET_PS             (0x705 << 8)   // Set Power State
#define HDA_VERB_GET_CONV           (0xF06 << 8)   // Get Converter Control
#define HDA_VERB_SET_CONV           (0x706 << 8)   // Set Converter Control
#define HDA_VERB_GET_PIN_CTL        (0xF07 << 8)   // Get Pin Widget Control
#define HDA_VERB_SET_PIN_CTL        (0x707 << 8)   // Set Pin Widget Control
#define HDA_VERB_GET_UNSOL          (0xF08 << 8)   // Get Unsolicited Response
#define HDA_VERB_SET_UNSOL          (0x708 << 8)   // Set Unsolicited Response
#define HDA_VERB_GET_CONFIG_DEF     (0xF1C << 8)   // Get Configuration Default
#define HDA_VERB_GET_AMP_GAIN       (0xB00 << 8)   // Get Amplifier Gain/Mute
#define HDA_VERB_SET_AMP_GAIN       (0x300 << 8)   // Set Amplifier Gain/Mute
#define HDA_VERB_GET_EAPD           (0xF0C << 8)   // Get EAPD/BTL Enable
#define HDA_VERB_SET_EAPD           (0x70C << 8)   // Set EAPD/BTL Enable
#define HDA_VERB_GET_VOL_KNOB       (0xF0F << 8)   // Get Volume Knob
#define HDA_VERB_SET_VOL_KNOB       (0x70F << 8)   // Set Volume Knob

// #71 Cirrus CS4208: the Apple codec's internal-speaker amplifier is powered by
// a codec GPIO (mirrors Linux sound/pci/hda/patch_cirrus.c cs4208_fixup_gpio0:
// gpio_eapd_speaker = bit0, so mask=dir=data=0x01 powers the speaker amp). GPIO
// verbs target the Audio Function Group / root node, not a pin widget. These are
// 12-bit verbs with an 8-bit payload (same encoding as the other verbs above).
#define HDA_VERB_GET_GPIO_DATA      (0xF15 << 8)   // Get GPIO Data
#define HDA_VERB_SET_GPIO_DATA      (0x715 << 8)   // Set GPIO Data
#define HDA_VERB_GET_GPIO_MASK      (0xF16 << 8)   // Get GPIO Enable Mask
#define HDA_VERB_SET_GPIO_MASK      (0x716 << 8)   // Set GPIO Enable Mask
#define HDA_VERB_GET_GPIO_DIR       (0xF17 << 8)   // Get GPIO Direction
#define HDA_VERB_SET_GPIO_DIR       (0x717 << 8)   // Set GPIO Direction

// Cirrus Logic HDA codec vendor ID (CS4206/CS4207/CS4208 all report 0x1013).
#define HDA_VENDOR_CIRRUS           0x1013
#define HDA_VERB_GET_STREAM         (0xF06 << 8)   // Get Stream Format
// #71 FIX: "Set Converter Stream, Channel" is 12-bit verb 0x706 (payload:
// [7:4]=stream tag, [3:0]=channel). The old value (0x20000<<8) was a bogus
// encoding that corrupted the NID field, so the DAC's stream tag was NEVER set
// and the output DMA never started (LPIB stayed at 0). This is the core fix.
#define HDA_VERB_SET_STREAM         (0x706 << 8)   // Set Converter Stream/Channel

// Parameters (for GET_PARAM verb)
#define HDA_PARAM_VENDOR_ID         0x00    // Vendor ID
#define HDA_PARAM_REVISION_ID       0x02    // Revision ID
#define HDA_PARAM_NODE_COUNT        0x04    // Subordinate Node Count
#define HDA_PARAM_FG_TYPE           0x05    // Function Group Type
#define HDA_PARAM_AUDIO_FG_CAP      0x08    // Audio Function Group Capabilities
#define HDA_PARAM_AUDIO_WIDGET_CAP  0x09    // Audio Widget Capabilities
#define HDA_PARAM_PCM               0x0A    // Supported PCM Sizes/Rates
#define HDA_PARAM_STREAM_FORMATS    0x0B    // Supported Stream Formats
#define HDA_PARAM_PIN_CAP           0x0C    // Pin Capabilities
#define HDA_PARAM_AMP_IN_CAP        0x0D    // Input Amplifier Capabilities
#define HDA_PARAM_AMP_OUT_CAP       0x12    // Output Amplifier Capabilities
#define HDA_PARAM_CONN_LIST_LEN     0x0E    // Connection List Length
#define HDA_PARAM_POWER_STATE       0x0F    // Supported Power States
#define HDA_PARAM_PROC_CAP          0x10    // Processing Capabilities
#define HDA_PARAM_GPIO_COUNT        0x11    // GPIO Count
#define HDA_PARAM_VOL_KNOB_CAP      0x13    // Volume Knob Capabilities

// Widget Capabilities
#define HDA_WIDGET_TYPE_MASK        (0xF << 20)
#define HDA_WIDGET_TYPE_OUTPUT      (0x0 << 20)  // Audio Output (DAC)
#define HDA_WIDGET_TYPE_INPUT       (0x1 << 20)  // Audio Input (ADC)
#define HDA_WIDGET_TYPE_MIXER       (0x2 << 20)  // Audio Mixer
#define HDA_WIDGET_TYPE_SELECTOR    (0x3 << 20)  // Audio Selector
#define HDA_WIDGET_TYPE_PIN         (0x4 << 20)  // Pin Complex
#define HDA_WIDGET_TYPE_POWER       (0x5 << 20)  // Power Widget
#define HDA_WIDGET_TYPE_VOL_KNOB    (0x6 << 20)  // Volume Knob
#define HDA_WIDGET_TYPE_BEEP        (0x7 << 20)  // Beep Generator
#define HDA_WIDGET_TYPE_VENDOR      (0xF << 20)  // Vendor-defined

// Pin Configuration
#define HDA_PIN_OUT_EN              (1 << 6)    // Output Enable
#define HDA_PIN_IN_EN               (1 << 5)    // Input Enable
#define HDA_PIN_VREF_MASK           (7 << 0)    // Voltage Reference

// Amplifier Gain/Mute
#define HDA_AMP_MUTE                (1 << 7)    // Mute
#define HDA_AMP_GAIN_MASK           0x7F        // Gain (0-127)
#define HDA_AMP_SET_OUTPUT          (1 << 15)   // Set output amp
#define HDA_AMP_SET_INPUT           (1 << 14)   // Set input amp
#define HDA_AMP_SET_LEFT            (1 << 13)   // Set left channel
#define HDA_AMP_SET_RIGHT           (1 << 12)   // Set right channel
#define HDA_AMP_SET_INDEX_MASK      (0xF << 8)  // Input index

// ============================================================================
// Buffer Descriptor List Entry (16 bytes)
// ============================================================================

typedef struct {
    uint64_t addr;      // Physical address of buffer
    uint32_t length;    // Buffer length in bytes
    uint32_t ioc;       // Interrupt on Completion (bit 0)
} __attribute__((packed)) hda_bdl_entry_t;

#define HDA_BDL_IOC         (1 << 0)

// Number of BDL entries (must be <= 256)
#define HDA_NUM_BDL         32

// ============================================================================
// CORB/RIRB Entry Structures
// ============================================================================

// CORB entry (4 bytes - the codec command)
typedef uint32_t hda_corb_entry_t;

// RIRB entry (8 bytes)
typedef struct {
    uint32_t response;      // Response data
    uint32_t response_ex;   // Extended response (codec address, unsol flag)
} __attribute__((packed)) hda_rirb_entry_t;

#define HDA_RIRB_UNSOL      (1 << 4)    // Unsolicited response
#define HDA_RIRB_CODEC_MASK (0xF << 0)  // Codec address

// ============================================================================
// HDA Codec Widget Info
// ============================================================================

typedef struct {
    uint8_t  nid;           // Node ID
    uint32_t widget_cap;    // Widget capabilities
    uint32_t pin_cap;       // Pin capabilities (if pin widget)
    uint32_t config_def;    // Pin config default (if pin widget)
    uint32_t amp_in_cap;    // Input amp capabilities
    uint32_t amp_out_cap;   // Output amp capabilities
    uint8_t  conn_count;    // Number of connections
    uint8_t  *conn_list;    // Connection list
} hda_widget_t;

// ============================================================================
// HDA Codec Info
// ============================================================================

typedef struct {
    uint8_t  cad;           // Codec address (0-15)
    uint16_t vendor_id;     // Vendor ID
    uint16_t device_id;     // Device ID
    uint8_t  fg_nid;        // Function group NID
    uint8_t  start_nid;     // First widget NID
    uint8_t  num_nodes;     // Number of widget nodes
    uint8_t  dac_nid;       // DAC widget NID (for output)
    uint8_t  out_pin_nid;   // Output pin NID
    // #71 generic output-path routing (auto-parser result).
    uint8_t  route_mix_nid;      // Intermediate mixer/selector NID (0 = direct)
    uint8_t  route_mix_conn;     // Connection index at the mixer/selector to the DAC
    uint8_t  route_pin_conn;     // Connection index at the pin to DAC-or-mixer
    uint8_t  route_mix_is_sel;   // 1 = selector (needs Set-Connection-Select), 0 = mixer
    uint8_t  default_device;     // Pin config-default device (0 LineOut,1 Spkr,2 HP,...)
    uint8_t  is_analog;          // 1 = analog speaker/HP/line-out path, 0 = digital/HDMI
    int      route_score;        // Selection score (higher = more preferred)
    // #390 CS4208 (Apple Cirrus) stereo: an Apple codec drives its two fixed
    // internal speakers from TWO mono DACs, one per pin (DAC10->Pin29 = left,
    // DAC11->Pin30 = right on the iMac CS4208). We keep the primary route in the
    // single fields above (route 0, used by the DMA stream/format setup) and
    // record every parallel output route here so hda_configure_codec can program
    // each DAC/pin and assign each DAC its stereo channel (route index). For a
    // single-output codec (QEMU line-out, HP-only) num_out_routes == 1 and this
    // is identical to the legacy single-route behaviour.
#define HDA_MAX_OUT_ROUTES 4
    uint8_t  num_out_routes;                        // >=1 when route_score >= 0
    uint8_t  route_dac[HDA_MAX_OUT_ROUTES];         // DAC nid per route
    uint8_t  route_pin[HDA_MAX_OUT_ROUTES];         // output pin nid per route
    uint8_t  route_mixn[HDA_MAX_OUT_ROUTES];        // intermediate mixer/selector (0=direct)
    uint8_t  route_mixc_[HDA_MAX_OUT_ROUTES];       // mixer/selector conn index to the DAC
    uint8_t  route_pinc[HDA_MAX_OUT_ROUTES];        // pin conn index to DAC-or-mixer
    uint8_t  route_mixsel[HDA_MAX_OUT_ROUTES];      // 1 = selector (needs Set-Conn-Select)
    uint8_t  route_dev[HDA_MAX_OUT_ROUTES];         // config-default device per route
} hda_codec_t;

// ============================================================================
// HDA Driver State
// ============================================================================

typedef struct {
    // PCI information
    uint8_t  pci_bus;
    uint8_t  pci_slot;
    uint8_t  pci_func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  irq;

    // Memory-mapped I/O base
    volatile uint8_t *mmio;
    uint64_t mmio_phys;
    uint32_t mmio_size;

    // Controller capabilities
    uint8_t  num_iss;       // Number of input streams
    uint8_t  num_oss;       // Number of output streams
    uint8_t  num_bss;       // Number of bidirectional streams
    bool     supports_64bit;

    // Codec info
    hda_codec_t codec;
    bool codec_found;

    // CORB/RIRB
    hda_corb_entry_t *corb;
    uint64_t corb_phys;
    hda_rirb_entry_t *rirb;
    uint64_t rirb_phys;
    uint16_t corb_wp;       // CORB write pointer
    uint16_t rirb_rp;       // RIRB read pointer

    // Output stream (for playback)
    uint8_t out_stream_idx; // Stream index (0 = first output stream)
    hda_bdl_entry_t *bdl;   // Buffer descriptor list
    uint64_t bdl_phys;
    void    *dma_buffer;
    uint64_t dma_buffer_phys;
    uint32_t dma_buffer_size;
    uint32_t bdl_buffer_size;

    // Current configuration
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
    uint16_t stream_format; // HDA stream format register value

    // State
    bool initialized;
    bool playing;
    uint8_t write_index;
    uint8_t read_index;

    // Statistics
    uint64_t frames_played;
    uint64_t underruns;

    // #71: real interrupt status (MSI). msi_enabled is set once
    // hda_setup_interrupt() has successfully programmed + enabled the MSI
    // capability on the winning controller; msi_vector is the IDT vector it
    // was armed on (HDA_MSI_VECTOR). Playback never depends on this being
    // true -- hda_poll_worker() services the stream via LPIB regardless -- it
    // exists purely so AUDIOLOG can report whether the real interrupt path is
    // also live (useful on hardware where PCI_INTERRUPT_LINE reads 0).
    bool    msi_enabled;
    uint8_t msi_vector;
} hda_state_t;

// ============================================================================
// HDA Driver API
// ============================================================================

// Initialize HDA driver
int hda_init(void);

// Shutdown HDA driver
void hda_shutdown(void);

// Check if HDA is available
bool hda_is_available(void);

// Get HDA device info
int hda_get_device_info(audio_device_info_t *info);

// Configure audio output
int hda_configure(uint32_t format, uint32_t sample_rate, uint32_t channels);

// Start playback
int hda_start(void);

// Stop playback
int hda_stop(void);

// Write audio data
int hda_write(const void *buffer, uint32_t frames);

// Get available write space
int hda_avail(void);

// Set volume
void hda_set_volume(uint8_t left, uint8_t right);

// Mute/unmute
void hda_mute(bool mute);

// Get driver state
hda_state_t *hda_get_state(void);

// Print HDA info
void hda_print_info(void);

// #71: play a sine test tone through the selected output path and confirm the
// output-stream DMA position (LPIB) advances. Fills the whole cyclic buffer so
// the tone loops for the requested duration. Returns AUDIO_OK if DMA advanced.
int hda_selftest_tone(uint32_t freq_hz, uint32_t ms);

// #71: true if the selected output path is an analog speaker/HP/line-out (vs a
// digital/HDMI converter). Used by the mixer / output-device selection.
bool hda_is_analog_output(void);

// Debug: read the output stream Link Position In Buffer register.
uint32_t hda_debug_lpib(void);

// #388 DEVLOG: bounded, best-effort HD Audio codec identification. Emits one
// line per responding codec ("HDA codec@N: vendor:device XXXX:YYYY ...") via the
// supplied callback. Never hangs boot; safe to call after audio_init.
void hda_devlog_scan(void (*emit)(const char *line));

// #71 / Cirrus CS4208 AUDIOLOG: emit the focused HD Audio audio-output
// diagnostic (controller PCI, winning codec identity, full output-relevant
// widget graph, and the LIVE post-configure output-path state: EAPD, amp
// gain/mute, pin control, codec GPIO mask/dir/data, and the output-stream
// descriptor RUN/STS/LPIB) to the supplied callback, one line at a time. Wired
// to /AUDIOLOG.TXT. Non-destructive: snapshots + restores the live audio state.
void hda_audiolog_report(void (*emit)(const char *line));

// #71 userland audio bring-up debug (SYS_HDA_DBG). See the op table in hda.c.
// Lets a Ring-3 tool drive the HDA output path (codec verbs, SDnCTL RUN, LPIB,
// GPIO, test tone, raw BAR) over the mdev bridge with no kernel reburn per try.
int64_t hda_debug_op(int op, uint64_t a, uint64_t b, uint64_t c);

// IRQ handler
void hda_irq_handler(void);

// #71: IDT vector the HDA MSI interrupt (if the controller has an MSI
// capability) is armed on. Outside the legacy 32-47 PIC IRQ range and the
// existing 128 (syscall) / 240 (SMP wake) vectors -- see cpu/idt.asm
// irq_hda_msi / cpu/idt.c idt_init().
#define HDA_MSI_VECTOR 0x50

// #71: arm the real interrupt path for the winning HDA controller (found by
// hda_init(), which must have already run). MUST be called AFTER the Local
// APIC is initialized (main.c calls this right after smp_init(), which is
// where lapic_init() runs) because MSI messages target the Local APIC
// directly. No-op if HDA never initialized, or if the winning controller has
// no MSI capability at all (some emulated HDA controllers only expose legacy
// INTx) -- in that case hda_state.msi_enabled stays false and the driver keeps
// working via hda_poll_worker()'s LPIB polling alone.
void hda_setup_interrupt(void);

// #699: start the LPIB-poll worker deferred until AFTER proc_init() has run.
// hda_init() runs long before proc_init() in the boot sequence (audio_init()
// is called from main.c right after usb_init()), so calling proc_create()
// directly from hda_init() corrupted the scheduler's ready queue (proc_init()
// later memsets the whole process table without resetting the ready-queue
// head/tail pointers left over from that pre-init process) -- this reliably
// broke sshd's per-connection worker spawn whenever a real HDA controller was
// present at boot. No-op if HDA never initialized. Call once, right after
// proc_init(); see drivers/hda.c for the full story.
void hda_start_poll_worker_deferred(void);

// #426: DAC-space wait queue. Woken from hda_service_stream(), i.e. from BOTH
// the real BCIS completion interrupt (hda_msi_isr) AND the 10 ms
// hda_poll_worker, so a waiter cannot lose a wakeup for more than one service
// pass even where MSI never arms. Wait on it with the standard pattern, always
// re-testing your own condition and including your own abort flag:
//
//   wait_event_interruptible(hda_space_wq(), audio_avail(as) >= n || stop);
//
// This exists so writers can BLOCK for DAC space instead of polling
// hda_avail(). First consumer: drivers/audio_pcm.c (Ring-3 PCM push).
struct wait_queue_head;
struct wait_queue_head *hda_space_wq(void);

#endif // HDA_H
