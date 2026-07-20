// intel_hda.h - Intel High Definition Audio Controller Driver (Enhanced)
//
// Enhanced driver specifically optimized for Intel HD Audio on real hardware.
// Supports Intel HDA controllers found in Intel chipsets:
// - ICH6/7/8/9/10
// - PCH (6/7/8 series)
// - PCH-C (100/200/300/400/500 series)
// - Sunrise Point, Cannon Point, Tiger Lake, Alder Lake
//
// Features:
// - Full CORB/RIRB command interface
// - Multi-codec support
// - Enhanced codec enumeration
// - Path discovery for audio routing
// - Better handling of Intel-specific codecs (Realtek, Conexant, IDT)
//
// Based on Intel High Definition Audio Specification Rev 1.0a and
// Linux snd-hda-intel driver.

#ifndef INTEL_HDA_H
#define INTEL_HDA_H

#include "../types.h"
#include "audio.h"

// ============================================================================
// Intel HDA Controller Device IDs
// ============================================================================

#define INTEL_HDA_VENDOR_ID     0x8086

// ICH/PCH Controller Device IDs
#define INTEL_HDA_ICH6          0x2668
#define INTEL_HDA_ICH7          0x27D8
#define INTEL_HDA_ICH8          0x284B
#define INTEL_HDA_ICH9          0x293E
#define INTEL_HDA_ICH9_2        0x293F
#define INTEL_HDA_ICH10         0x3A3E
#define INTEL_HDA_ICH10_2       0x3A6E
#define INTEL_HDA_PCH           0x3B56
#define INTEL_HDA_PCH_2         0x3B57
#define INTEL_HDA_CPT           0x1C20  // Cougar Point
#define INTEL_HDA_PPT           0x1E20  // Panther Point
#define INTEL_HDA_LPT           0x8C20  // Lynx Point
#define INTEL_HDA_LPT_LP        0x9C20  // Lynx Point LP
#define INTEL_HDA_WPT           0x9CA0  // Wildcat Point
#define INTEL_HDA_WPT_LP        0x9D70  // Wildcat Point LP
#define INTEL_HDA_SPT           0xA170  // Sunrise Point
#define INTEL_HDA_SPT_LP        0x9D71  // Sunrise Point LP
#define INTEL_HDA_KBP           0xA270  // Kaby Point
#define INTEL_HDA_CNP           0xA348  // Cannon Point
#define INTEL_HDA_CNP_LP        0x9DC8  // Cannon Point LP
#define INTEL_HDA_CMP           0x06C8  // Comet Point
#define INTEL_HDA_CMP_H         0xF0C8  // Comet Point-H
#define INTEL_HDA_TGP           0xA0C8  // Tiger Point
#define INTEL_HDA_TGP_LP        0x43C8  // Tiger Point LP
#define INTEL_HDA_JSP           0x38C8  // Jasper Point
#define INTEL_HDA_ADP           0x7AD0  // Alder Point
#define INTEL_HDA_ADP_P         0x51C8  // Alder Point-P
#define INTEL_HDA_ADP_M         0x54C8  // Alder Point-M

// Other vendor HDA controllers (for reference)
#define NVIDIA_HDA_VENDOR       0x10DE
#define AMD_HDA_VENDOR          0x1002

// ============================================================================
// Common Codec Vendor IDs
// ============================================================================

#define CODEC_VENDOR_REALTEK    0x10EC
#define CODEC_VENDOR_CONEXANT   0x14F1
#define CODEC_VENDOR_IDT        0x111D
#define CODEC_VENDOR_SIGMATEL   0x8384
#define CODEC_VENDOR_CIRRUS     0x1013
#define CODEC_VENDOR_CREATIVE   0x1102
#define CODEC_VENDOR_VIA        0x1106
#define CODEC_VENDOR_ANALOG     0x11D4
#define CODEC_VENDOR_INTEL_HDMI 0x8086

// Common Realtek Codec IDs
#define REALTEK_ALC882          0x0882
#define REALTEK_ALC883          0x0883
#define REALTEK_ALC885          0x0885
#define REALTEK_ALC887          0x0887
#define REALTEK_ALC888          0x0888
#define REALTEK_ALC889          0x0889
#define REALTEK_ALC892          0x0892
#define REALTEK_ALC897          0x0897
#define REALTEK_ALC256          0x0256
#define REALTEK_ALC269          0x0269
#define REALTEK_ALC274          0x0274
#define REALTEK_ALC282          0x0282
#define REALTEK_ALC287          0x0287
#define REALTEK_ALC290          0x0290
#define REALTEK_ALC292          0x0292
#define REALTEK_ALC293          0x0293
#define REALTEK_ALC295          0x0295
#define REALTEK_ALC298          0x0298
#define REALTEK_ALC700          0x0700

// ============================================================================
// Widget Types
// ============================================================================

typedef enum {
    HDA_WIDGET_AUDIO_OUTPUT = 0,    // DAC
    HDA_WIDGET_AUDIO_INPUT  = 1,    // ADC
    HDA_WIDGET_MIXER        = 2,    // Audio Mixer
    HDA_WIDGET_SELECTOR     = 3,    // Audio Selector
    HDA_WIDGET_PIN          = 4,    // Pin Complex
    HDA_WIDGET_POWER        = 5,    // Power Widget
    HDA_WIDGET_VOLUME_KNOB  = 6,    // Volume Knob
    HDA_WIDGET_BEEP         = 7,    // Beep Generator
    HDA_WIDGET_VENDOR       = 0xF   // Vendor Defined
} hda_widget_type_t;

// ============================================================================
// Pin Configuration (Default)
// ============================================================================

// Port Connectivity (bits 31:30)
#define PIN_CONN_JACK           (0 << 30)   // External Jack
#define PIN_CONN_NONE           (1 << 30)   // No physical connection
#define PIN_CONN_FIXED          (2 << 30)   // Fixed function (internal)
#define PIN_CONN_BOTH           (3 << 30)   // Both jack and internal

// Location (bits 29:24)
#define PIN_LOC_EXTERNAL        (0 << 24)
#define PIN_LOC_INTERNAL        (1 << 24)
#define PIN_LOC_SEPARATE        (2 << 24)
#define PIN_LOC_OTHER           (3 << 24)

// Default Device (bits 23:20)
#define PIN_DEV_LINE_OUT        (0x0 << 20)
#define PIN_DEV_SPEAKER         (0x1 << 20)
#define PIN_DEV_HP_OUT          (0x2 << 20)
#define PIN_DEV_CD              (0x3 << 20)
#define PIN_DEV_SPDIF_OUT       (0x4 << 20)
#define PIN_DEV_DIG_OUT         (0x5 << 20)
#define PIN_DEV_MODEM_LINE      (0x6 << 20)
#define PIN_DEV_MODEM_HANDSET   (0x7 << 20)
#define PIN_DEV_LINE_IN         (0x8 << 20)
#define PIN_DEV_AUX             (0x9 << 20)
#define PIN_DEV_MIC_IN          (0xA << 20)
#define PIN_DEV_TELEPHONY       (0xB << 20)
#define PIN_DEV_SPDIF_IN        (0xC << 20)
#define PIN_DEV_DIG_IN          (0xD << 20)
#define PIN_DEV_OTHER           (0xF << 20)

// Connection Type (bits 19:16)
#define PIN_CONN_TYPE_UNKNOWN   (0x0 << 16)
#define PIN_CONN_TYPE_1_8       (0x1 << 16)  // 1/8" stereo/mono
#define PIN_CONN_TYPE_1_4       (0x2 << 16)  // 1/4" stereo/mono
#define PIN_CONN_TYPE_ATAPI     (0x3 << 16)
#define PIN_CONN_TYPE_RCA       (0x4 << 16)
#define PIN_CONN_TYPE_OPTICAL   (0x5 << 16)
#define PIN_CONN_TYPE_DIG_OTHER (0x6 << 16)
#define PIN_CONN_TYPE_COMBO     (0xF << 16)

// Color (bits 15:12)
#define PIN_COLOR_UNKNOWN       (0x0 << 12)
#define PIN_COLOR_BLACK         (0x1 << 12)
#define PIN_COLOR_GREY          (0x2 << 12)
#define PIN_COLOR_BLUE          (0x3 << 12)
#define PIN_COLOR_GREEN         (0x4 << 12)
#define PIN_COLOR_RED           (0x5 << 12)
#define PIN_COLOR_ORANGE        (0x6 << 12)
#define PIN_COLOR_YELLOW        (0x7 << 12)
#define PIN_COLOR_PURPLE        (0x8 << 12)
#define PIN_COLOR_PINK          (0x9 << 12)
#define PIN_COLOR_WHITE         (0xE << 12)
#define PIN_COLOR_OTHER         (0xF << 12)

// ============================================================================
// Widget Information Structure
// ============================================================================

typedef struct {
    uint8_t  nid;               // Node ID
    uint8_t  type;              // Widget type
    bool     has_input_amp;     // Input amplifier present
    bool     has_output_amp;    // Output amplifier present
    bool     amp_override;      // Amp override capability
    uint8_t  conn_count;        // Number of connections
    uint8_t  conn_list[32];     // Connection list (max 32 connections)
    uint32_t widget_cap;        // Widget capabilities
    uint32_t pin_cap;           // Pin capabilities (if pin widget)
    uint32_t config_default;    // Default configuration (pin)
    uint32_t amp_in_cap;        // Input amplifier capabilities
    uint32_t amp_out_cap;       // Output amplifier capabilities
    uint8_t  selected_conn;     // Currently selected connection
} intel_hda_widget_t;

// ============================================================================
// Audio Path Structure
// ============================================================================

typedef struct {
    uint8_t  dac_nid;           // DAC (audio output) NID
    uint8_t  mixer_nid;         // Mixer NID (if any)
    uint8_t  pin_nid;           // Output pin NID
    uint8_t  path[8];           // Full path from DAC to pin
    uint8_t  path_len;          // Number of nodes in path
    bool     is_speaker;        // True if internal speaker
    bool     is_headphone;      // True if headphone jack
    bool     is_line_out;       // True if line out
} intel_hda_path_t;

// ============================================================================
// Codec Structure
// ============================================================================

typedef struct {
    uint8_t  cad;               // Codec address (0-15)
    uint16_t vendor_id;         // Vendor ID
    uint16_t device_id;         // Device ID (subsystem)
    uint16_t revision;          // Revision
    uint8_t  afg_nid;           // Audio Function Group NID
    uint8_t  start_nid;         // First widget NID
    uint8_t  num_widgets;       // Number of widgets

    // Widget info
    intel_hda_widget_t widgets[64];  // Widget info (indexed by NID - start_nid)

    // Discovered paths
    intel_hda_path_t output_paths[4];  // Output paths (max 4)
    uint8_t num_output_paths;

    // Active path
    uint8_t active_dac;
    uint8_t active_pin;

    // Codec name
    const char *name;
} intel_hda_codec_t;

// ============================================================================
// Controller State Structure
// ============================================================================

typedef struct {
    // PCI Information
    uint8_t  pci_bus;
    uint8_t  pci_slot;
    uint8_t  pci_func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  irq;

    // MMIO
    volatile uint8_t *mmio;
    uint64_t mmio_phys;
    uint32_t mmio_size;

    // Controller capabilities
    uint8_t  num_iss;           // Input streams
    uint8_t  num_oss;           // Output streams
    uint8_t  num_bss;           // Bidirectional streams
    bool     supports_64bit;

    // CORB/RIRB
    volatile uint32_t *corb;
    uint64_t corb_phys;
    volatile uint64_t *rirb;
    uint64_t rirb_phys;
    uint16_t corb_wp;
    uint16_t rirb_rp;

    // Codecs (max 16)
    intel_hda_codec_t codecs[16];
    uint8_t num_codecs;
    uint8_t primary_codec;      // Index of primary audio codec

    // Output stream
    uint8_t out_stream_idx;
    volatile void *bdl;
    uint64_t bdl_phys;
    void *dma_buffer;
    uint64_t dma_buffer_phys;
    uint32_t dma_buffer_size;
    uint32_t bdl_buffer_size;

    // Current configuration
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t format;
    uint16_t stream_format;

    // State
    bool initialized;
    bool playing;
    uint8_t write_index;
    uint8_t read_index;

    // Volume (0-127)
    uint8_t volume_left;
    uint8_t volume_right;
    bool muted;

    // Statistics
    uint64_t frames_played;
    uint64_t underruns;
    uint64_t interrupts;
} intel_hda_state_t;

// ============================================================================
// Error Codes
// ============================================================================

#define INTEL_HDA_OK                0
#define INTEL_HDA_ERR_NO_DEVICE     -1
#define INTEL_HDA_ERR_NOT_INTEL     -2
#define INTEL_HDA_ERR_TIMEOUT       -3
#define INTEL_HDA_ERR_NO_MEMORY     -4
#define INTEL_HDA_ERR_NO_CODEC      -5
#define INTEL_HDA_ERR_NOT_INIT      -6
#define INTEL_HDA_ERR_INVALID_PARAM -7

// ============================================================================
// Public API - Initialization
// ============================================================================

// Initialize Intel HDA driver
// Returns: 0 on success, error code on failure
int intel_hda_init(void);

// Shutdown Intel HDA driver
void intel_hda_shutdown(void);

// Check if Intel HDA is available
bool intel_hda_is_available(void);

// Get driver state
intel_hda_state_t *intel_hda_get_state(void);

// Get device info
int intel_hda_get_device_info(audio_device_info_t *info);

// ============================================================================
// Public API - Audio Configuration
// ============================================================================

// Configure audio output
// format: AUDIO_FORMAT_* constant
// sample_rate: Hz (8000-192000)
// channels: 1-8
int intel_hda_configure(uint32_t format, uint32_t sample_rate, uint32_t channels);

// Start playback
int intel_hda_start(void);

// Stop playback
int intel_hda_stop(void);

// Write audio data
// buffer: PCM data in configured format
// frames: number of complete frames
// Returns: frames written or negative error code
int intel_hda_write(const void *buffer, uint32_t frames);

// Get available write space in frames
int intel_hda_avail(void);

// ============================================================================
// Public API - Volume Control
// ============================================================================

// Set output volume (0-127)
void intel_hda_set_volume(uint8_t left, uint8_t right);

// Get current volume
void intel_hda_get_volume(uint8_t *left, uint8_t *right);

// Mute/unmute
void intel_hda_mute(bool mute);

// Check if muted
bool intel_hda_is_muted(void);

// ============================================================================
// Public API - Output Selection
// ============================================================================

// Select output (speaker, headphone, line out)
int intel_hda_select_output(uint8_t output_type);

#define INTEL_HDA_OUTPUT_AUTO       0   // Auto-detect
#define INTEL_HDA_OUTPUT_SPEAKER    1   // Internal speaker
#define INTEL_HDA_OUTPUT_HEADPHONE  2   // Headphone jack
#define INTEL_HDA_OUTPUT_LINEOUT    3   // Line out

// ============================================================================
// Public API - Information
// ============================================================================

// Print driver and codec information
void intel_hda_print_info(void);

// Print detailed codec information
void intel_hda_print_codec_info(uint8_t codec_idx);

// Print all widget information
void intel_hda_print_widgets(uint8_t codec_idx);

// ============================================================================
// Interrupt Handler
// ============================================================================

// IRQ handler (called from interrupt handler)
void intel_hda_irq_handler(void);

#endif // INTEL_HDA_H
