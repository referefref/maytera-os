// intel_gpu.h - Intel Integrated Graphics Driver for MayteraOS
//
// Supports Intel HD Graphics (Gen 5+), including Ironlake, Sandy Bridge,
// Ivy Bridge, Haswell, Broadwell, Skylake, Kaby Lake, Coffee Lake, etc.
//
// Features:
// - PCI detection for Intel GPU (vendor 0x8086)
// - MMIO register access via GTT (Graphics Translation Table)
// - Native resolution detection from EDID/VBT
// - Framebuffer setup with proper stride
// - Basic 2D acceleration (blitter engine)
//
// Based on Intel Open Source Graphics Driver documentation and i915 Linux driver.

#ifndef INTEL_GPU_H
#define INTEL_GPU_H

#include "../types.h"
#include "../boot_info.h"

// ============================================================================
// PCI Identification
// ============================================================================

#define INTEL_GPU_VENDOR_ID     0x8086

// PCI Device Class
#define PCI_CLASS_DISPLAY       0x03
#define PCI_SUBCLASS_VGA        0x00

// Intel GPU Device IDs - Common generations
// Gen 5 - Ironlake
#define INTEL_DEV_ILK_D         0x0042
#define INTEL_DEV_ILK_M         0x0046

// Gen 6 - Sandy Bridge
#define INTEL_DEV_SNB_GT1       0x0102
#define INTEL_DEV_SNB_GT2       0x0112
#define INTEL_DEV_SNB_GT2_PLUS  0x0122
#define INTEL_DEV_SNB_M_GT1     0x0106
#define INTEL_DEV_SNB_M_GT2     0x0116
#define INTEL_DEV_SNB_M_GT2_PLUS 0x0126

// Gen 7 - Ivy Bridge
#define INTEL_DEV_IVB_GT1       0x0152
#define INTEL_DEV_IVB_GT2       0x0162
#define INTEL_DEV_IVB_M_GT1     0x0156
#define INTEL_DEV_IVB_M_GT2     0x0166

// Gen 7.5 - Haswell
#define INTEL_DEV_HSW_GT1       0x0402
#define INTEL_DEV_HSW_GT2       0x0412
#define INTEL_DEV_HSW_GT3       0x0422
#define INTEL_DEV_HSW_ULT_GT1   0x0A02
#define INTEL_DEV_HSW_ULT_GT2   0x0A12
#define INTEL_DEV_HSW_ULT_GT3   0x0A22
#define INTEL_DEV_HSW_ULX_GT1   0x0A0E
#define INTEL_DEV_HSW_ULX_GT2   0x0A1E

// Gen 8 - Broadwell
#define INTEL_DEV_BDW_GT1       0x1602
#define INTEL_DEV_BDW_GT2       0x1612
#define INTEL_DEV_BDW_GT3       0x1622
#define INTEL_DEV_BDW_ULT_GT1   0x1606
#define INTEL_DEV_BDW_ULT_GT2   0x1616
#define INTEL_DEV_BDW_ULT_GT3   0x1626
#define INTEL_DEV_BDW_ULX_GT1   0x160E
#define INTEL_DEV_BDW_ULX_GT2   0x161E

// Gen 9 - Skylake
#define INTEL_DEV_SKL_GT1       0x1902
#define INTEL_DEV_SKL_GT2       0x1912
#define INTEL_DEV_SKL_GT3       0x1922
#define INTEL_DEV_SKL_GT4       0x1932
#define INTEL_DEV_SKL_ULT_GT1   0x1906
#define INTEL_DEV_SKL_ULT_GT2   0x1916
#define INTEL_DEV_SKL_ULT_GT3   0x1926
#define INTEL_DEV_SKL_ULX_GT1   0x190E
#define INTEL_DEV_SKL_ULX_GT2   0x191E

// Gen 9.5 - Kaby Lake / Coffee Lake
#define INTEL_DEV_KBL_GT1       0x5902
#define INTEL_DEV_KBL_GT2       0x5912
#define INTEL_DEV_KBL_GT3       0x5922
#define INTEL_DEV_KBL_ULT_GT1   0x5906
#define INTEL_DEV_KBL_ULT_GT2   0x5916
#define INTEL_DEV_KBL_ULT_GT3   0x5926
#define INTEL_DEV_CFL_GT2       0x3E92
#define INTEL_DEV_CFL_H_GT2     0x3E9B
#define INTEL_DEV_CFL_S_GT2     0x3E91

// Gen 11 - Ice Lake
#define INTEL_DEV_ICL_GT2       0x8A52
#define INTEL_DEV_ICL_GT1_5     0x8A5C
#define INTEL_DEV_ICL_GT1       0x8A5A
#define INTEL_DEV_ICL_GT0_5     0x8A56

// Gen 12 - Tiger Lake
#define INTEL_DEV_TGL_GT2       0x9A49
#define INTEL_DEV_TGL_GT1       0x9A40

// ============================================================================
// GPU Generation Enum
// ============================================================================

typedef enum {
    INTEL_GEN_UNKNOWN = 0,
    INTEL_GEN_5,        // Ironlake
    INTEL_GEN_6,        // Sandy Bridge
    INTEL_GEN_7,        // Ivy Bridge
    INTEL_GEN_7_5,      // Haswell
    INTEL_GEN_8,        // Broadwell
    INTEL_GEN_9,        // Skylake
    INTEL_GEN_9_5,      // Kaby Lake / Coffee Lake
    INTEL_GEN_11,       // Ice Lake
    INTEL_GEN_12,       // Tiger Lake
} intel_gpu_gen_t;

// ============================================================================
// Memory-Mapped Register Definitions
// ============================================================================

// PCI Configuration Registers
#define INTEL_PCI_GMADR         0x18    // Graphics Memory Aperture
#define INTEL_PCI_GTTADR        0x1C    // GTT Base (Gen 6+: in GTTMMADR)
#define INTEL_PCI_MSAC          0x62    // Memory Size Allocation Control
#define INTEL_PCI_MGGC          0x50    // Graphics Mode Select / GC

// MMIO Base Offsets (from BAR0)
#define INTEL_MMIO_SIZE         (2 * MB)    // MMIO region size

// Display Engine Registers (offset from MMIO base)
#define INTEL_REG_VGA_CONTROL   0x41000     // VGA Control
#define INTEL_REG_VGACNTRL      0x71400     // VGA Control (alternate)

// Pipe/CRTC Registers - Pipe A
#define INTEL_REG_PIPEA_CONF    0x70008     // Pipe A Configuration
#define INTEL_REG_PIPEA_STAT    0x70024     // Pipe A Status
#define INTEL_REG_HTOTAL_A      0x60000     // Horizontal Total A
#define INTEL_REG_HBLANK_A      0x60004     // Horizontal Blank A
#define INTEL_REG_HSYNC_A       0x60008     // Horizontal Sync A
#define INTEL_REG_VTOTAL_A      0x6000C     // Vertical Total A
#define INTEL_REG_VBLANK_A      0x60010     // Vertical Blank A
#define INTEL_REG_VSYNC_A       0x60014     // Vertical Sync A
#define INTEL_REG_PIPEASRC      0x6001C     // Pipe A Source Image Size
#define INTEL_REG_PIPEAFRAMEH   0x70040     // Pipe A Frame High
#define INTEL_REG_PIPEAFRAMEL   0x70044     // Pipe A Frame Low

// Display Plane Registers - Primary Plane A
#define INTEL_REG_DSPACNTR      0x70180     // Display Plane A Control
#define INTEL_REG_DSPALINOFF    0x70184     // Display Plane A Linear Offset
#define INTEL_REG_DSPASTRIDE    0x70188     // Display Plane A Stride
#define INTEL_REG_DSPASURF      0x7019C     // Display Plane A Surface Base
#define INTEL_REG_DSPATILEOFF   0x701A4     // Display Plane A Tiled Offset
#define INTEL_REG_DSPAADDR      0x70184     // Display Plane A Base Address (legacy)

// Cursor Registers
#define INTEL_REG_CURACNTR      0x70080     // Cursor A Control
#define INTEL_REG_CURABASE      0x70084     // Cursor A Base
#define INTEL_REG_CURAPOS       0x70088     // Cursor A Position

// FDI/Panel Fitter (Ironlake+)
#define INTEL_REG_PF_CTL_A      0x68080     // Panel Fitter Control A
#define INTEL_REG_PF_WIN_SZ_A   0x68074     // Panel Fitter Window Size A
#define INTEL_REG_PF_WIN_POS_A  0x68070     // Panel Fitter Window Position A

// DPLL (Clock) Registers
#define INTEL_REG_DPLL_A        0x6014      // DPLL A Control
#define INTEL_REG_DPLL_A_MD     0x601C      // DPLL A Divisor MD
#define INTEL_REG_FPA0          0x6040      // FP0 Divisor A
#define INTEL_REG_FPA1          0x6044      // FP1 Divisor A

// Sprite Plane Registers
#define INTEL_REG_DVSCNTR       0x72180     // Sprite Control
#define INTEL_REG_DVSLINOFF     0x72184     // Sprite Linear Offset
#define INTEL_REG_DVSSTRIDE     0x72188     // Sprite Stride
#define INTEL_REG_DVSPOS        0x7218C     // Sprite Position
#define INTEL_REG_DVSSIZE       0x72190     // Sprite Size
#define INTEL_REG_DVSKEYVAL     0x72194     // Sprite Key Value
#define INTEL_REG_DVSKEYMSK     0x72198     // Sprite Key Mask
#define INTEL_REG_DVSSURF       0x7219C     // Sprite Surface Base
#define INTEL_REG_DVSKEYMAX     0x721A0     // Sprite Key Max
#define INTEL_REG_DVSTILEOFF    0x721A4     // Sprite Tile Offset
#define INTEL_REG_DVSSCALE      0x72204     // Sprite Scale

// BLT Engine Registers (Gen 6+)
#define INTEL_REG_BLT_RING_BASE     0x22000
#define INTEL_REG_BLT_RING_HEAD     0x22034
#define INTEL_REG_BLT_RING_TAIL     0x22030
#define INTEL_REG_BLT_RING_CTL      0x2203C

// Render Engine Registers
#define INTEL_REG_RENDER_RING_BASE  0x02000
#define INTEL_REG_RENDER_RING_HEAD  0x02034
#define INTEL_REG_RENDER_RING_TAIL  0x02030
#define INTEL_REG_RENDER_RING_CTL   0x0203C

// Display Status Registers
#define INTEL_REG_DERRMR        0x44050     // Display Error Mask
#define INTEL_REG_DEIIR         0x44008     // Display Engine Interrupt Identity
#define INTEL_REG_DEIMR         0x4400C     // Display Engine Interrupt Mask
#define INTEL_REG_DEIER         0x44004     // Display Engine Interrupt Enable

// GTT Registers
#define INTEL_REG_PGTBL_CTL     0x02020     // Page Table Control (Gen 2-4)

// Power/PM Registers
#define INTEL_REG_PWRCTX        0xA010      // Power Context
#define INTEL_REG_FWBLC         0xA00C      // Framebuffer Window Block

// ============================================================================
// Display Plane Control Bits
// ============================================================================

// DSPCNTR - Display Plane Control
#define INTEL_DSPCNTR_ENABLE        (1 << 31)
#define INTEL_DSPCNTR_GAMMA_ENABLE  (1 << 30)
#define INTEL_DSPCNTR_FORMAT_MASK   (0xF << 26)
#define INTEL_DSPCNTR_FORMAT_8BPP   (2 << 26)   // Indexed 8-bit
#define INTEL_DSPCNTR_FORMAT_RGB565 (5 << 26)   // RGB 5:6:5
#define INTEL_DSPCNTR_FORMAT_XRGB   (6 << 26)   // xRGB 8:8:8:8
#define INTEL_DSPCNTR_FORMAT_XBGR   (0xE << 26) // xBGR 8:8:8:8
#define INTEL_DSPCNTR_FORMAT_ARGB   (7 << 26)   // ARGB 8:8:8:8
#define INTEL_DSPCNTR_PIPE_SELECT   (1 << 24)   // 0=Pipe A, 1=Pipe B
#define INTEL_DSPCNTR_TILED         (1 << 10)   // Tiled mode
#define INTEL_DSPCNTR_TRICKLE_FEED  (1 << 14)   // Trickle feed disable

// PIPE_CONF bits
#define INTEL_PIPECONF_ENABLE       (1 << 31)
#define INTEL_PIPECONF_STATE        (1 << 30)
#define INTEL_PIPECONF_DOUBLE_WIDE  (1 << 30)
#define INTEL_PIPECONF_PROGRESSIVE  (0 << 21)
#define INTEL_PIPECONF_INTERLACE    (1 << 21)
#define INTEL_PIPECONF_BPC_MASK     (7 << 5)
#define INTEL_PIPECONF_BPC_6        (2 << 5)
#define INTEL_PIPECONF_BPC_8        (0 << 5)
#define INTEL_PIPECONF_BPC_10       (1 << 5)
#define INTEL_PIPECONF_BPC_12       (3 << 5)

// VGA Control
#define INTEL_VGACNTRL_DISABLE      (1 << 31)

// Cursor Control
#define INTEL_CURCNTR_ENABLE        (1 << 0)
#define INTEL_CURCNTR_ARGB          (1 << 5)
#define INTEL_CURCNTR_64X64         (0x07)

// ============================================================================
// BLT Commands
// ============================================================================

#define INTEL_BLT_OPCODE_COLOR_BLT      0x40
#define INTEL_BLT_OPCODE_SRC_COPY_BLT   0x43
#define INTEL_BLT_OPCODE_PAT_BLT        0x41

#define INTEL_BLT_WRITE_ALPHA           (1 << 21)
#define INTEL_BLT_WRITE_RGB             (1 << 20)
#define INTEL_BLT_ROP_SRC_COPY          (0xCC << 16)
#define INTEL_BLT_ROP_PAT_COPY          (0xF0 << 16)

// Color depth codes for BLT
#define INTEL_BLT_COLOR_8BPP            0
#define INTEL_BLT_COLOR_16BPP_565       1
#define INTEL_BLT_COLOR_32BPP           3

// ============================================================================
// EDID/DDC Definitions
// ============================================================================

#define INTEL_EDID_BLOCK_SIZE   128
#define INTEL_EDID_EXTENSIONS   0x7E

// GMBUS (I2C) Registers for DDC
#define INTEL_REG_GMBUS0        0x5100      // GMBUS Control
#define INTEL_REG_GMBUS1        0x5104      // GMBUS Command/Status
#define INTEL_REG_GMBUS2        0x5108      // GMBUS Status
#define INTEL_REG_GMBUS3        0x510C      // GMBUS Data
#define INTEL_REG_GMBUS4        0x5110      // GMBUS Interrupt Mask
#define INTEL_REG_GMBUS5        0x5120      // GMBUS 2-byte index

// GMBUS Rate Select
#define INTEL_GMBUS_RATE_100K   (0 << 8)
#define INTEL_GMBUS_RATE_50K    (1 << 8)
#define INTEL_GMBUS_RATE_400K   (2 << 8)
#define INTEL_GMBUS_RATE_1M     (3 << 8)

// GMBUS Pin Pair Select
#define INTEL_GMBUS_PIN_DPC     (4 << 0)    // HDMI-C/DPD
#define INTEL_GMBUS_PIN_DPB     (5 << 0)    // DP-B/HDMI-B
#define INTEL_GMBUS_PIN_DPD     (6 << 0)    // DP-D/HDMI-D
#define INTEL_GMBUS_PIN_VGA     (2 << 0)    // VGA DDC

// ============================================================================
// GPU State Structure
// ============================================================================

typedef struct {
    // PCI Information
    uint8_t  pci_bus;
    uint8_t  pci_slot;
    uint8_t  pci_func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  revision;

    // GPU Generation
    intel_gpu_gen_t gen;
    const char *gen_name;

    // Memory Mapped I/O
    volatile uint8_t *mmio;
    uint64_t mmio_phys;
    uint32_t mmio_size;

    // Graphics Memory Aperture (GTT mapped VRAM)
    uint64_t gtt_phys;
    uint64_t gtt_size;

    // Stolen Memory (pre-allocated VRAM in system memory)
    uint64_t stolen_base;
    uint64_t stolen_size;

    // Framebuffer Configuration
    uint64_t fb_phys;           // Physical address of framebuffer
    void    *fb_virt;           // Virtual address (if mapped)
    uint32_t fb_size;           // Framebuffer size in bytes
    uint32_t fb_width;          // Width in pixels
    uint32_t fb_height;         // Height in pixels
    uint32_t fb_pitch;          // Bytes per line
    uint32_t fb_bpp;            // Bits per pixel
    uint32_t fb_format;         // Pixel format

    // Display Configuration
    bool     display_enabled;
    bool     vga_disabled;
    uint32_t h_active;          // Horizontal active pixels
    uint32_t v_active;          // Vertical active lines
    uint32_t pixel_clock;       // Pixel clock in kHz

    // EDID Data
    uint8_t  edid[256];         // Extended EDID data
    bool     edid_valid;

    // BLT Engine State (2D acceleration)
    volatile uint32_t *blt_ring;
    uint64_t blt_ring_phys;
    uint32_t blt_ring_size;
    uint32_t blt_ring_tail;
    bool     blt_enabled;

    // Cursor State
    volatile uint32_t *cursor_buffer;
    uint64_t cursor_phys;
    int32_t  cursor_x;
    int32_t  cursor_y;
    bool     cursor_enabled;

    // Driver State
    bool     initialized;
    bool     owns_display;      // True if we control the display (vs UEFI)

    // Statistics
    uint64_t blt_ops;
    uint64_t vblank_count;
} intel_gpu_state_t;

// ============================================================================
// Display Mode Structure
// ============================================================================

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t refresh;           // Refresh rate in Hz
    uint32_t pixel_clock;       // Pixel clock in kHz

    // Horizontal timing
    uint32_t h_total;
    uint32_t h_blank_start;
    uint32_t h_blank_end;
    uint32_t h_sync_start;
    uint32_t h_sync_end;

    // Vertical timing
    uint32_t v_total;
    uint32_t v_blank_start;
    uint32_t v_blank_end;
    uint32_t v_sync_start;
    uint32_t v_sync_end;

    bool     interlaced;
} intel_display_mode_t;

// ============================================================================
// Error Codes
// ============================================================================

#define INTEL_GPU_OK                0
#define INTEL_GPU_ERR_NO_DEVICE     -1
#define INTEL_GPU_ERR_NOT_INTEL     -2
#define INTEL_GPU_ERR_UNSUPPORTED   -3
#define INTEL_GPU_ERR_NO_MEMORY     -4
#define INTEL_GPU_ERR_TIMEOUT       -5
#define INTEL_GPU_ERR_EDID          -6
#define INTEL_GPU_ERR_MODE_SET      -7
#define INTEL_GPU_ERR_NOT_INIT      -8

// ============================================================================
// Public API - Initialization
// ============================================================================

// Initialize Intel GPU driver
// Detects Intel GPU via PCI, sets up MMIO, and configures display
// Returns: INTEL_GPU_OK on success, error code on failure
int intel_gpu_init(void);

// Shutdown Intel GPU driver
void intel_gpu_shutdown(void);

// Check if Intel GPU is available and initialized
bool intel_gpu_is_available(void);

// Get driver state (for debugging)
intel_gpu_state_t *intel_gpu_get_state(void);

// ============================================================================
// Public API - Display Control
// ============================================================================

// Get current display mode
int intel_gpu_get_mode(intel_display_mode_t *mode);

// Set display mode (if supported)
// Note: Mode setting on real hardware requires careful DPLL/FDI configuration
int intel_gpu_set_mode(const intel_display_mode_t *mode);

// Get native resolution from EDID
int intel_gpu_get_native_resolution(uint32_t *width, uint32_t *height);

// Enable/disable display output
int intel_gpu_enable_display(bool enable);

// ============================================================================
// Public API - Framebuffer Access
// ============================================================================

// Get framebuffer information
// Returns framebuffer physical address and dimensions
int intel_gpu_get_framebuffer(uint64_t *phys_addr, uint32_t *width,
                               uint32_t *height, uint32_t *pitch, uint32_t *bpp);

// Set framebuffer base address (for double buffering)
int intel_gpu_set_framebuffer_base(uint64_t phys_addr);

// Update display from framebuffer (synchronize with vsync)
void intel_gpu_flip(void);

// Wait for vertical blank
void intel_gpu_wait_vblank(void);

// ============================================================================
// Public API - 2D Acceleration (BLT Engine)
// ============================================================================

// Initialize BLT engine for 2D acceleration
int intel_gpu_blt_init(void);

// Fill rectangle with solid color
// Uses hardware BLT for acceleration
int intel_gpu_blt_fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        uint32_t color);

// Copy rectangle (blit)
int intel_gpu_blt_copy(uint32_t src_x, uint32_t src_y,
                        uint32_t dst_x, uint32_t dst_y,
                        uint32_t width, uint32_t height);

// Copy from system memory to VRAM
int intel_gpu_blt_upload(const void *src, uint32_t src_pitch,
                          uint32_t dst_x, uint32_t dst_y,
                          uint32_t width, uint32_t height);

// Wait for BLT operations to complete
void intel_gpu_blt_wait(void);

// ============================================================================
// Public API - Cursor
// ============================================================================

// Set cursor image (64x64 ARGB)
int intel_gpu_set_cursor(const uint32_t *image, uint32_t hotspot_x, uint32_t hotspot_y);

// Move cursor
void intel_gpu_move_cursor(int32_t x, int32_t y);

// Show/hide cursor
void intel_gpu_show_cursor(bool show);

// ============================================================================
// Public API - Integration with Existing Framebuffer
// ============================================================================

// Use UEFI/bootloader framebuffer
// Call this if you want to use the framebuffer set up by UEFI GOP
// instead of doing full mode setting
int intel_gpu_use_uefi_fb(framebuffer_info_t *fb_info);

// Check if we should fall back to simple framebuffer
// Returns true if Intel GPU detected but mode setting not recommended
bool intel_gpu_should_fallback(void);

// ============================================================================
// Debug/Diagnostics
// ============================================================================

// Print Intel GPU information
void intel_gpu_print_info(void);

// Dump MMIO registers (for debugging)
void intel_gpu_dump_registers(void);

// Read EDID from display
int intel_gpu_read_edid(uint8_t *buffer, uint32_t size);

#endif // INTEL_GPU_H
