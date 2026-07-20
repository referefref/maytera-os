// intel_gpu.c - Intel Integrated Graphics Driver for MayteraOS
//
// This driver provides basic display and 2D acceleration support for
// Intel integrated graphics. It focuses on reliability and compatibility
// rather than full feature support.
//
// Design Philosophy:
// - Prefer UEFI GOP framebuffer when available (no mode setting required)
// - Support native mode detection via EDID
// - Provide optional 2D BLT acceleration
// - Graceful fallback to VGA/simple framebuffer

#include "intel_gpu.h"
#include "pci.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../gui/syslog.h"

// ============================================================================
// Driver State
// ============================================================================

static intel_gpu_state_t gpu_state = {0};

// ============================================================================
// MMIO Access Helpers
// ============================================================================

static inline uint32_t intel_read32(uint32_t offset) {
    return *(volatile uint32_t *)(gpu_state.mmio + offset);
}

static inline void intel_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(gpu_state.mmio + offset) = value;
}

static inline uint16_t intel_read16(uint32_t offset) {
    return *(volatile uint16_t *)(gpu_state.mmio + offset);
}

static inline void intel_write16(uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(gpu_state.mmio + offset) = value;
}

static void intel_delay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 10; i++) {
        io_wait();
    }
}

// ============================================================================
// GPU Generation Detection
// ============================================================================

typedef struct {
    uint16_t device_id;
    intel_gpu_gen_t gen;
    const char *name;
} intel_device_info_t;

static const intel_device_info_t intel_devices[] = {
    // Gen 5 - Ironlake
    { 0x0042, INTEL_GEN_5, "Ironlake Desktop" },
    { 0x0046, INTEL_GEN_5, "Ironlake Mobile" },

    // Gen 6 - Sandy Bridge
    { 0x0102, INTEL_GEN_6, "Sandy Bridge GT1" },
    { 0x0112, INTEL_GEN_6, "Sandy Bridge GT2" },
    { 0x0122, INTEL_GEN_6, "Sandy Bridge GT2+" },
    { 0x0106, INTEL_GEN_6, "Sandy Bridge M GT1" },
    { 0x0116, INTEL_GEN_6, "Sandy Bridge M GT2" },
    { 0x0126, INTEL_GEN_6, "Sandy Bridge M GT2+" },

    // Gen 7 - Ivy Bridge
    { 0x0152, INTEL_GEN_7, "Ivy Bridge GT1" },
    { 0x0162, INTEL_GEN_7, "Ivy Bridge GT2" },
    { 0x0156, INTEL_GEN_7, "Ivy Bridge M GT1" },
    { 0x0166, INTEL_GEN_7, "Ivy Bridge M GT2" },

    // Gen 7.5 - Haswell
    { 0x0402, INTEL_GEN_7_5, "Haswell GT1" },
    { 0x0412, INTEL_GEN_7_5, "Haswell GT2" },
    { 0x0422, INTEL_GEN_7_5, "Haswell GT3" },
    { 0x0A02, INTEL_GEN_7_5, "Haswell ULT GT1" },
    { 0x0A12, INTEL_GEN_7_5, "Haswell ULT GT2" },
    { 0x0A22, INTEL_GEN_7_5, "Haswell ULT GT3" },
    { 0x0A0E, INTEL_GEN_7_5, "Haswell ULX GT1" },
    { 0x0A1E, INTEL_GEN_7_5, "Haswell ULX GT2" },
    { 0x0D02, INTEL_GEN_7_5, "Haswell SDV GT1" },
    { 0x0D12, INTEL_GEN_7_5, "Haswell SDV GT2" },
    { 0x0D22, INTEL_GEN_7_5, "Haswell SDV GT3" },

    // Gen 8 - Broadwell
    { 0x1602, INTEL_GEN_8, "Broadwell GT1" },
    { 0x1612, INTEL_GEN_8, "Broadwell GT2" },
    { 0x1622, INTEL_GEN_8, "Broadwell GT3" },
    { 0x1606, INTEL_GEN_8, "Broadwell ULT GT1" },
    { 0x1616, INTEL_GEN_8, "Broadwell ULT GT2" },
    { 0x1626, INTEL_GEN_8, "Broadwell ULT GT3" },
    { 0x160E, INTEL_GEN_8, "Broadwell ULX GT1" },
    { 0x161E, INTEL_GEN_8, "Broadwell ULX GT2" },

    // Gen 9 - Skylake
    { 0x1902, INTEL_GEN_9, "Skylake GT1" },
    { 0x1912, INTEL_GEN_9, "Skylake GT2" },
    { 0x1922, INTEL_GEN_9, "Skylake GT3" },
    { 0x1932, INTEL_GEN_9, "Skylake GT4" },
    { 0x1906, INTEL_GEN_9, "Skylake ULT GT1" },
    { 0x1916, INTEL_GEN_9, "Skylake ULT GT2" },
    { 0x1926, INTEL_GEN_9, "Skylake ULT GT3" },
    { 0x190E, INTEL_GEN_9, "Skylake ULX GT1" },
    { 0x191E, INTEL_GEN_9, "Skylake ULX GT2" },

    // Gen 9.5 - Kaby Lake
    { 0x5902, INTEL_GEN_9_5, "Kaby Lake GT1" },
    { 0x5912, INTEL_GEN_9_5, "Kaby Lake GT2" },
    { 0x5922, INTEL_GEN_9_5, "Kaby Lake GT3" },
    { 0x5906, INTEL_GEN_9_5, "Kaby Lake ULT GT1" },
    { 0x5916, INTEL_GEN_9_5, "Kaby Lake ULT GT2" },
    { 0x5926, INTEL_GEN_9_5, "Kaby Lake ULT GT3" },
    { 0x591B, INTEL_GEN_9_5, "Kaby Lake GT2 Halo" },

    // Gen 9.5 - Coffee Lake
    { 0x3E91, INTEL_GEN_9_5, "Coffee Lake GT2" },
    { 0x3E92, INTEL_GEN_9_5, "Coffee Lake GT2" },
    { 0x3E9B, INTEL_GEN_9_5, "Coffee Lake GT2 Halo" },
    { 0x3E98, INTEL_GEN_9_5, "Coffee Lake GT2" },
    { 0x3EA0, INTEL_GEN_9_5, "Coffee Lake GT2" },

    // Gen 11 - Ice Lake
    { 0x8A52, INTEL_GEN_11, "Ice Lake GT2" },
    { 0x8A5C, INTEL_GEN_11, "Ice Lake GT1.5" },
    { 0x8A5A, INTEL_GEN_11, "Ice Lake GT1" },
    { 0x8A56, INTEL_GEN_11, "Ice Lake GT0.5" },

    // Gen 12 - Tiger Lake
    { 0x9A49, INTEL_GEN_12, "Tiger Lake GT2" },
    { 0x9A40, INTEL_GEN_12, "Tiger Lake GT1" },
    { 0x9A60, INTEL_GEN_12, "Tiger Lake GT1" },
    { 0x9A68, INTEL_GEN_12, "Tiger Lake GT1" },
    { 0x9A70, INTEL_GEN_12, "Tiger Lake GT1" },
    { 0x9A78, INTEL_GEN_12, "Tiger Lake GT2" },

    { 0, INTEL_GEN_UNKNOWN, NULL }
};

static intel_gpu_gen_t intel_detect_generation(uint16_t device_id, const char **name) {
    for (int i = 0; intel_devices[i].name != NULL; i++) {
        if (intel_devices[i].device_id == device_id) {
            if (name) *name = intel_devices[i].name;
            return intel_devices[i].gen;
        }
    }

    // Try to detect by device ID pattern
    if ((device_id & 0xFF00) == 0x0100) {
        if (name) *name = "Sandy Bridge (unknown)";
        return INTEL_GEN_6;
    }
    if ((device_id & 0xFF00) == 0x0400 || (device_id & 0xFF00) == 0x0A00 ||
        (device_id & 0xFF00) == 0x0D00) {
        if (name) *name = "Haswell (unknown)";
        return INTEL_GEN_7_5;
    }
    if ((device_id & 0xFF00) == 0x1600) {
        if (name) *name = "Broadwell (unknown)";
        return INTEL_GEN_8;
    }
    if ((device_id & 0xFF00) == 0x1900) {
        if (name) *name = "Skylake (unknown)";
        return INTEL_GEN_9;
    }
    if ((device_id & 0xFF00) == 0x5900) {
        if (name) *name = "Kaby Lake (unknown)";
        return INTEL_GEN_9_5;
    }
    if ((device_id & 0xFF00) == 0x3E00) {
        if (name) *name = "Coffee Lake (unknown)";
        return INTEL_GEN_9_5;
    }

    if (name) *name = "Intel GPU (unknown generation)";
    return INTEL_GEN_UNKNOWN;
}

// ============================================================================
// PCI Detection
// ============================================================================

static pci_device_t *intel_find_gpu(void) {
    // Find VGA-compatible controller
    pci_device_t *dev = pci_find_class(PCI_CLASS_DISPLAY, PCI_SUBCLASS_VGA);

    while (dev) {
        if (dev->vendor_id == INTEL_GPU_VENDOR_ID) {
            return dev;
        }
        // Find next VGA device
        int count = pci_get_device_count();
        bool found_current = false;
        for (int i = 0; i < count; i++) {
            pci_device_t *d = pci_get_device(i);
            if (found_current && d->class_code == PCI_CLASS_DISPLAY &&
                d->subclass == PCI_SUBCLASS_VGA) {
                dev = d;
                break;
            }
            if (d == dev) found_current = true;
        }
        if (!found_current) break;
    }

    return NULL;
}

// ============================================================================
// GMBUS (I2C) for EDID
// ============================================================================

static int intel_gmbus_wait_hw_ready(void) {
    for (int i = 0; i < 100; i++) {
        uint32_t status = intel_read32(INTEL_REG_GMBUS2);
        if (status & (1 << 11)) {  // HW_RDY
            return 0;
        }
        if (status & (1 << 10)) {  // NAK
            return -1;
        }
        intel_delay(100);
    }
    return -1;
}

static int intel_gmbus_wait_done(void) {
    for (int i = 0; i < 100; i++) {
        uint32_t status = intel_read32(INTEL_REG_GMBUS2);
        if (status & (1 << 14)) {  // Wait done
            return 0;
        }
        intel_delay(100);
    }
    return -1;
}

static int intel_read_edid_gmbus(uint8_t *buffer, uint32_t size) {
    if (size < 128) return INTEL_GPU_ERR_EDID;

    // Reset GMBUS
    intel_write32(INTEL_REG_GMBUS1, 0);
    intel_write32(INTEL_REG_GMBUS0, 0);
    intel_delay(100);

    // Select GMBUS pin pair (VGA DDC by default, could be HDMI)
    intel_write32(INTEL_REG_GMBUS0, INTEL_GMBUS_RATE_100K | INTEL_GMBUS_PIN_VGA);
    intel_delay(100);

    // Write slave address (0x50 << 1 for write)
    intel_write32(INTEL_REG_GMBUS1, (0x50 << 1) | (1 << 30) | (1 << 25) | 1);
    intel_delay(100);

    // Write offset 0
    intel_write32(INTEL_REG_GMBUS3, 0);

    if (intel_gmbus_wait_hw_ready() < 0) {
        kprintf("[INTEL_GPU] GMBUS write failed\n");
        intel_write32(INTEL_REG_GMBUS0, 0);
        return INTEL_GPU_ERR_EDID;
    }

    // Read 128 bytes
    intel_write32(INTEL_REG_GMBUS1, (0x50 << 1) | 1 | (1 << 30) | (1 << 25) | (128 << 16));

    for (int i = 0; i < 128; i += 4) {
        if (intel_gmbus_wait_hw_ready() < 0) {
            kprintf("[INTEL_GPU] GMBUS read failed at byte %d\n", i);
            intel_write32(INTEL_REG_GMBUS0, 0);
            return INTEL_GPU_ERR_EDID;
        }

        uint32_t data = intel_read32(INTEL_REG_GMBUS3);
        buffer[i] = data & 0xFF;
        if (i + 1 < 128) buffer[i + 1] = (data >> 8) & 0xFF;
        if (i + 2 < 128) buffer[i + 2] = (data >> 16) & 0xFF;
        if (i + 3 < 128) buffer[i + 3] = (data >> 24) & 0xFF;
    }

    intel_gmbus_wait_done();
    intel_write32(INTEL_REG_GMBUS0, 0);

    // Validate EDID header
    uint8_t header[] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (memcmp(buffer, header, 8) != 0) {
        kprintf("[INTEL_GPU] Invalid EDID header\n");
        return INTEL_GPU_ERR_EDID;
    }

    // Validate checksum
    uint8_t checksum = 0;
    for (int i = 0; i < 128; i++) {
        checksum += buffer[i];
    }
    if (checksum != 0) {
        kprintf("[INTEL_GPU] EDID checksum mismatch\n");
        return INTEL_GPU_ERR_EDID;
    }

    kprintf("[INTEL_GPU] EDID read successfully\n");
    return INTEL_GPU_OK;
}

// ============================================================================
// EDID Parsing
// ============================================================================

static int intel_parse_edid_resolution(uint8_t *edid, uint32_t *width, uint32_t *height) {
    // Check for preferred timing in detailed timing blocks
    for (int i = 0; i < 4; i++) {
        uint8_t *dtd = edid + 54 + (i * 18);

        // Check if this is a detailed timing descriptor (not a display descriptor)
        if (dtd[0] == 0 && dtd[1] == 0) continue;

        // Parse horizontal/vertical active
        uint32_t h_active = dtd[2] | ((dtd[4] & 0xF0) << 4);
        uint32_t v_active = dtd[5] | ((dtd[7] & 0xF0) << 4);

        if (h_active > 0 && v_active > 0) {
            *width = h_active;
            *height = v_active;
            kprintf("[INTEL_GPU] EDID preferred mode: %ux%u\n", h_active, v_active);
            return INTEL_GPU_OK;
        }
    }

    // Fall back to established timings
    if (edid[35] & 0x80) { *width = 1280; *height = 1024; return INTEL_GPU_OK; }
    if (edid[36] & 0x01) { *width = 1024; *height = 768; return INTEL_GPU_OK; }
    if (edid[35] & 0x08) { *width = 800; *height = 600; return INTEL_GPU_OK; }
    if (edid[35] & 0x20) { *width = 640; *height = 480; return INTEL_GPU_OK; }

    return INTEL_GPU_ERR_EDID;
}

// ============================================================================
// Display Configuration
// ============================================================================

static void intel_disable_vga(void) {
    uint32_t vga_ctrl = intel_read32(INTEL_REG_VGACNTRL);
    if (!(vga_ctrl & INTEL_VGACNTRL_DISABLE)) {
        intel_write32(INTEL_REG_VGACNTRL, vga_ctrl | INTEL_VGACNTRL_DISABLE);
        intel_delay(100);
        kprintf("[INTEL_GPU] VGA disabled\n");
    }
    gpu_state.vga_disabled = true;
}

static int intel_setup_plane(uint64_t fb_phys, uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t format) {
    // Configure display plane control
    uint32_t dspcntr = INTEL_DSPCNTR_ENABLE;

    // Set pixel format
    switch (format) {
        case PIXEL_FORMAT_RGB:
            dspcntr |= INTEL_DSPCNTR_FORMAT_XRGB;
            break;
        case PIXEL_FORMAT_BGR:
            dspcntr |= INTEL_DSPCNTR_FORMAT_XBGR;
            break;
        default:
            dspcntr |= INTEL_DSPCNTR_FORMAT_XRGB;
            break;
    }

    // Write plane configuration
    intel_write32(INTEL_REG_DSPACNTR, dspcntr);
    intel_write32(INTEL_REG_DSPASTRIDE, pitch);
    intel_write32(INTEL_REG_DSPALINOFF, 0);

    // Set surface base address (triggers update)
    intel_write32(INTEL_REG_DSPASURF, (uint32_t)(fb_phys & 0xFFFFF000));

    gpu_state.fb_phys = fb_phys;
    gpu_state.fb_width = width;
    gpu_state.fb_height = height;
    gpu_state.fb_pitch = pitch;
    gpu_state.display_enabled = true;

    kprintf("[INTEL_GPU] Display plane configured: %ux%u, pitch=%u\n",
            width, height, pitch);

    return INTEL_GPU_OK;
}

// ============================================================================
// BLT Engine (2D Acceleration)
// ============================================================================

static int intel_blt_ring_init(void) {
    // Allocate ring buffer (4KB aligned, 4KB size minimum)
    gpu_state.blt_ring_size = 4096;
    gpu_state.blt_ring = (volatile uint32_t *)kzalloc_aligned(gpu_state.blt_ring_size, 4096);
    if (!gpu_state.blt_ring) {
        return INTEL_GPU_ERR_NO_MEMORY;
    }
    gpu_state.blt_ring_phys = (uint64_t)(uintptr_t)gpu_state.blt_ring;
    gpu_state.blt_ring_tail = 0;

    // Configure BLT ring
    intel_write32(INTEL_REG_BLT_RING_CTL, 0);  // Stop ring
    intel_delay(10);

    intel_write32(INTEL_REG_BLT_RING_BASE, (uint32_t)gpu_state.blt_ring_phys);
    intel_write32(INTEL_REG_BLT_RING_HEAD, 0);
    intel_write32(INTEL_REG_BLT_RING_TAIL, 0);

    // Enable ring: size = (size / 4096 - 1) | valid
    uint32_t ring_ctl = ((gpu_state.blt_ring_size / 4096 - 1) << 12) | 1;
    intel_write32(INTEL_REG_BLT_RING_CTL, ring_ctl);
    intel_delay(10);

    gpu_state.blt_enabled = true;
    kprintf("[INTEL_GPU] BLT engine initialized\n");

    return INTEL_GPU_OK;
}

static void intel_blt_ring_emit(uint32_t cmd) {
    if (!gpu_state.blt_enabled) return;

    uint32_t ring_offset = gpu_state.blt_ring_tail / 4;
    gpu_state.blt_ring[ring_offset] = cmd;
    gpu_state.blt_ring_tail = (gpu_state.blt_ring_tail + 4) % gpu_state.blt_ring_size;
}

static void intel_blt_ring_submit(void) {
    if (!gpu_state.blt_enabled) return;

    // Write tail pointer to submit commands
    intel_write32(INTEL_REG_BLT_RING_TAIL, gpu_state.blt_ring_tail);
}

// ============================================================================
// Public API Implementation
// ============================================================================

int intel_gpu_init(void) {
    LOG_INFO("[INTEL_GPU] Initializing Intel GPU driver");
    kprintf("[INTEL_GPU] Scanning for Intel GPU...\n");

    if (gpu_state.initialized) {
        return INTEL_GPU_OK;
    }

    memset(&gpu_state, 0, sizeof(gpu_state));

    // Find Intel GPU via PCI
    pci_device_t *dev = intel_find_gpu();
    if (!dev) {
        kprintf("[INTEL_GPU] No Intel GPU found\n");
        return INTEL_GPU_ERR_NO_DEVICE;
    }

    if (dev->vendor_id != INTEL_GPU_VENDOR_ID) {
        kprintf("[INTEL_GPU] Found GPU but not Intel (vendor=%04x)\n", dev->vendor_id);
        return INTEL_GPU_ERR_NOT_INTEL;
    }

    // Store PCI info
    gpu_state.pci_bus = dev->bus;
    gpu_state.pci_slot = dev->slot;
    gpu_state.pci_func = dev->func;
    gpu_state.vendor_id = dev->vendor_id;
    gpu_state.device_id = dev->device_id;
    gpu_state.revision = dev->revision;

    // Detect GPU generation
    gpu_state.gen = intel_detect_generation(dev->device_id, &gpu_state.gen_name);

    kprintf("[INTEL_GPU] Found %s (device %04x) at %02x:%02x.%x\n",
            gpu_state.gen_name, dev->device_id,
            dev->bus, dev->slot, dev->func);

    if (gpu_state.gen == INTEL_GEN_UNKNOWN) {
        kprintf("[INTEL_GPU] Unknown GPU generation, using safe defaults\n");
    }

    // Enable bus master and memory access
    pci_enable_bus_master(dev);

    // Get MMIO base from BAR0
    gpu_state.mmio_phys = pci_get_bar_address(dev, 0);
    gpu_state.mmio_size = pci_get_bar_size(dev, 0);

    if (gpu_state.mmio_phys == 0 || gpu_state.mmio_size == 0) {
        kprintf("[INTEL_GPU] Invalid MMIO BAR\n");
        return INTEL_GPU_ERR_NO_DEVICE;
    }

    gpu_state.mmio = (volatile uint8_t *)(uintptr_t)gpu_state.mmio_phys;
    kprintf("[INTEL_GPU] MMIO at 0x%llx, size %u KB\n",
            gpu_state.mmio_phys, gpu_state.mmio_size / 1024);

    // Get Graphics Aperture (GTT-mapped VRAM) from BAR2
    gpu_state.gtt_phys = pci_get_bar_address(dev, 2);
    gpu_state.gtt_size = pci_get_bar_size(dev, 2);

    if (gpu_state.gtt_phys != 0) {
        kprintf("[INTEL_GPU] Graphics aperture at 0x%llx, size %u MB\n",
                gpu_state.gtt_phys, (uint32_t)(gpu_state.gtt_size / MB));
    }

    // Disable VGA
    intel_disable_vga();

    // Try to read EDID
    int ret = intel_read_edid_gmbus(gpu_state.edid, sizeof(gpu_state.edid));
    if (ret == INTEL_GPU_OK) {
        gpu_state.edid_valid = true;

        uint32_t width, height;
        if (intel_parse_edid_resolution(gpu_state.edid, &width, &height) == INTEL_GPU_OK) {
            gpu_state.h_active = width;
            gpu_state.v_active = height;
        }
    }

    // Initialize BLT engine (optional, for 2D acceleration)
    if (gpu_state.gen >= INTEL_GEN_6) {
        intel_blt_ring_init();
    }

    gpu_state.initialized = true;
    gpu_state.owns_display = false;  // Don't take over display by default

    LOG_INFO("[INTEL_GPU] Initialization complete");
    kprintf("[INTEL_GPU] Initialization complete\n");

    return INTEL_GPU_OK;
}

void intel_gpu_shutdown(void) {
    if (!gpu_state.initialized) return;

    // Disable BLT
    if (gpu_state.blt_enabled) {
        intel_write32(INTEL_REG_BLT_RING_CTL, 0);
        if (gpu_state.blt_ring) {
            kfree((void *)gpu_state.blt_ring);
        }
    }

    // Disable cursor
    if (gpu_state.cursor_enabled) {
        intel_write32(INTEL_REG_CURACNTR, 0);
        if (gpu_state.cursor_buffer) {
            kfree((void *)gpu_state.cursor_buffer);
        }
    }

    gpu_state.initialized = false;
    LOG_INFO("[INTEL_GPU] Shutdown complete");
}

bool intel_gpu_is_available(void) {
    return gpu_state.initialized;
}

intel_gpu_state_t *intel_gpu_get_state(void) {
    return &gpu_state;
}

// ============================================================================
// Display Control
// ============================================================================

int intel_gpu_get_mode(intel_display_mode_t *mode) {
    if (!gpu_state.initialized || !mode) return INTEL_GPU_ERR_NOT_INIT;

    mode->width = gpu_state.fb_width;
    mode->height = gpu_state.fb_height;
    mode->refresh = 60;  // Assumed
    mode->pixel_clock = gpu_state.pixel_clock;

    // Read timing registers if available
    if (gpu_state.owns_display) {
        uint32_t htotal = intel_read32(INTEL_REG_HTOTAL_A);
        uint32_t vtotal = intel_read32(INTEL_REG_VTOTAL_A);

        mode->h_total = (htotal >> 16) & 0xFFFF;
        mode->v_total = (vtotal >> 16) & 0xFFFF;
    }

    return INTEL_GPU_OK;
}

int intel_gpu_set_mode(const intel_display_mode_t *mode) {
    (void)mode;
    // Full mode setting requires complex DPLL/FDI configuration
    // For now, we rely on UEFI to set the mode
    kprintf("[INTEL_GPU] Mode setting not implemented - use UEFI GOP\n");
    return INTEL_GPU_ERR_UNSUPPORTED;
}

int intel_gpu_get_native_resolution(uint32_t *width, uint32_t *height) {
    if (!gpu_state.initialized) return INTEL_GPU_ERR_NOT_INIT;

    if (gpu_state.edid_valid && gpu_state.h_active > 0 && gpu_state.v_active > 0) {
        *width = gpu_state.h_active;
        *height = gpu_state.v_active;
        return INTEL_GPU_OK;
    }

    // No EDID, return current mode if known
    if (gpu_state.fb_width > 0 && gpu_state.fb_height > 0) {
        *width = gpu_state.fb_width;
        *height = gpu_state.fb_height;
        return INTEL_GPU_OK;
    }

    return INTEL_GPU_ERR_EDID;
}

int intel_gpu_enable_display(bool enable) {
    if (!gpu_state.initialized) return INTEL_GPU_ERR_NOT_INIT;

    if (enable) {
        // Enable pipe and plane
        uint32_t pipeconf = intel_read32(INTEL_REG_PIPEA_CONF);
        pipeconf |= INTEL_PIPECONF_ENABLE;
        intel_write32(INTEL_REG_PIPEA_CONF, pipeconf);

        uint32_t dspcntr = intel_read32(INTEL_REG_DSPACNTR);
        dspcntr |= INTEL_DSPCNTR_ENABLE;
        intel_write32(INTEL_REG_DSPACNTR, dspcntr);
    } else {
        // Disable plane then pipe
        uint32_t dspcntr = intel_read32(INTEL_REG_DSPACNTR);
        dspcntr &= ~INTEL_DSPCNTR_ENABLE;
        intel_write32(INTEL_REG_DSPACNTR, dspcntr);
        intel_delay(100);

        uint32_t pipeconf = intel_read32(INTEL_REG_PIPEA_CONF);
        pipeconf &= ~INTEL_PIPECONF_ENABLE;
        intel_write32(INTEL_REG_PIPEA_CONF, pipeconf);
    }

    gpu_state.display_enabled = enable;
    return INTEL_GPU_OK;
}

// ============================================================================
// Framebuffer Access
// ============================================================================

int intel_gpu_get_framebuffer(uint64_t *phys_addr, uint32_t *width,
                               uint32_t *height, uint32_t *pitch, uint32_t *bpp) {
    if (!gpu_state.initialized) return INTEL_GPU_ERR_NOT_INIT;

    if (phys_addr) *phys_addr = gpu_state.fb_phys;
    if (width) *width = gpu_state.fb_width;
    if (height) *height = gpu_state.fb_height;
    if (pitch) *pitch = gpu_state.fb_pitch;
    if (bpp) *bpp = gpu_state.fb_bpp;

    return INTEL_GPU_OK;
}

int intel_gpu_set_framebuffer_base(uint64_t phys_addr) {
    if (!gpu_state.initialized) return INTEL_GPU_ERR_NOT_INIT;

    // Update surface base address (triggers flip on vblank)
    intel_write32(INTEL_REG_DSPASURF, (uint32_t)(phys_addr & 0xFFFFF000));
    gpu_state.fb_phys = phys_addr;

    return INTEL_GPU_OK;
}

void intel_gpu_flip(void) {
    if (!gpu_state.initialized) return;

    // Trigger surface update
    intel_write32(INTEL_REG_DSPASURF, (uint32_t)(gpu_state.fb_phys & 0xFFFFF000));
}

void intel_gpu_wait_vblank(void) {
    if (!gpu_state.initialized) return;

    // Read current frame counter
    uint32_t frame = intel_read32(INTEL_REG_PIPEAFRAMEL);

    // Wait for next frame
    for (int i = 0; i < 100000; i++) {
        if (intel_read32(INTEL_REG_PIPEAFRAMEL) != frame) {
            gpu_state.vblank_count++;
            return;
        }
    }
}

// ============================================================================
// 2D Acceleration
// ============================================================================

int intel_gpu_blt_init(void) {
    if (!gpu_state.initialized) return INTEL_GPU_ERR_NOT_INIT;

    if (gpu_state.blt_enabled) return INTEL_GPU_OK;

    return intel_blt_ring_init();
}

int intel_gpu_blt_fill(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        uint32_t color) {
    if (!gpu_state.blt_enabled) {
        // Software fallback
        return INTEL_GPU_ERR_UNSUPPORTED;
    }

    // Calculate destination address
    uint64_t dst_addr = gpu_state.fb_phys + (y * gpu_state.fb_pitch) + (x * (gpu_state.fb_bpp / 8));

    // Emit COLOR_BLT command
    uint32_t cmd = (2 << 29) | (INTEL_BLT_OPCODE_COLOR_BLT << 22) |
                   INTEL_BLT_WRITE_ALPHA | INTEL_BLT_WRITE_RGB |
                   INTEL_BLT_ROP_PAT_COPY | 5;  // 5 dwords follow

    intel_blt_ring_emit(cmd);
    intel_blt_ring_emit((INTEL_BLT_COLOR_32BPP << 24) | gpu_state.fb_pitch);
    intel_blt_ring_emit((height << 16) | (width * (gpu_state.fb_bpp / 8)));
    intel_blt_ring_emit((uint32_t)dst_addr);
    intel_blt_ring_emit((uint32_t)(dst_addr >> 32));
    intel_blt_ring_emit(color);

    // Padding (commands must be 8-byte aligned)
    intel_blt_ring_emit(0);

    intel_blt_ring_submit();
    gpu_state.blt_ops++;

    return INTEL_GPU_OK;
}

int intel_gpu_blt_copy(uint32_t src_x, uint32_t src_y,
                        uint32_t dst_x, uint32_t dst_y,
                        uint32_t width, uint32_t height) {
    if (!gpu_state.blt_enabled) {
        return INTEL_GPU_ERR_UNSUPPORTED;
    }

    uint32_t bytes_per_pixel = gpu_state.fb_bpp / 8;
    uint64_t src_addr = gpu_state.fb_phys + (src_y * gpu_state.fb_pitch) + (src_x * bytes_per_pixel);
    uint64_t dst_addr = gpu_state.fb_phys + (dst_y * gpu_state.fb_pitch) + (dst_x * bytes_per_pixel);

    // Emit SRC_COPY_BLT command
    uint32_t cmd = (2 << 29) | (INTEL_BLT_OPCODE_SRC_COPY_BLT << 22) |
                   INTEL_BLT_WRITE_ALPHA | INTEL_BLT_WRITE_RGB |
                   INTEL_BLT_ROP_SRC_COPY | 8;  // 8 dwords follow

    intel_blt_ring_emit(cmd);
    intel_blt_ring_emit((INTEL_BLT_COLOR_32BPP << 24) | gpu_state.fb_pitch);
    intel_blt_ring_emit((height << 16) | (width * bytes_per_pixel));
    intel_blt_ring_emit((uint32_t)dst_addr);
    intel_blt_ring_emit((uint32_t)(dst_addr >> 32));
    intel_blt_ring_emit(gpu_state.fb_pitch);
    intel_blt_ring_emit((uint32_t)src_addr);
    intel_blt_ring_emit((uint32_t)(src_addr >> 32));

    intel_blt_ring_submit();
    gpu_state.blt_ops++;

    return INTEL_GPU_OK;
}

int intel_gpu_blt_upload(const void *src, uint32_t src_pitch,
                          uint32_t dst_x, uint32_t dst_y,
                          uint32_t width, uint32_t height) {
    (void)src; (void)src_pitch;
    (void)dst_x; (void)dst_y;
    (void)width; (void)height;
    // This would require GTT mapping of the source buffer
    // For simplicity, we don't implement hardware upload
    return INTEL_GPU_ERR_UNSUPPORTED;
}

void intel_gpu_blt_wait(void) {
    if (!gpu_state.blt_enabled) return;

    // Wait for ring to drain
    for (int i = 0; i < 100000; i++) {
        uint32_t head = intel_read32(INTEL_REG_BLT_RING_HEAD);
        uint32_t tail = intel_read32(INTEL_REG_BLT_RING_TAIL);
        if (head == tail) return;
        intel_delay(1);
    }

    kprintf("[INTEL_GPU] BLT wait timeout\n");
}

// ============================================================================
// Cursor
// ============================================================================

int intel_gpu_set_cursor(const uint32_t *image, uint32_t hotspot_x, uint32_t hotspot_y) {
    (void)hotspot_x; (void)hotspot_y;  // TODO: Use for cursor offset
    if (!gpu_state.initialized) return INTEL_GPU_ERR_NOT_INIT;

    // Allocate cursor buffer if needed (64x64 ARGB, 16KB)
    if (!gpu_state.cursor_buffer) {
        gpu_state.cursor_buffer = (volatile uint32_t *)kzalloc_aligned(64 * 64 * 4, 16384);
        if (!gpu_state.cursor_buffer) {
            return INTEL_GPU_ERR_NO_MEMORY;
        }
        gpu_state.cursor_phys = (uint64_t)(uintptr_t)gpu_state.cursor_buffer;
    }

    // Copy cursor image
    if (image) {
        memcpy((void *)gpu_state.cursor_buffer, image, 64 * 64 * 4);
    }

    // Configure cursor
    intel_write32(INTEL_REG_CURABASE, (uint32_t)gpu_state.cursor_phys);

    uint32_t cur_ctl = INTEL_CURCNTR_ENABLE | INTEL_CURCNTR_ARGB | INTEL_CURCNTR_64X64;
    intel_write32(INTEL_REG_CURACNTR, cur_ctl);

    gpu_state.cursor_enabled = true;

    return INTEL_GPU_OK;
}

void intel_gpu_move_cursor(int32_t x, int32_t y) {
    if (!gpu_state.initialized || !gpu_state.cursor_enabled) return;

    gpu_state.cursor_x = x;
    gpu_state.cursor_y = y;

    // Cursor position format: ((x & 0xFFFF) | ((y & 0xFFFF) << 16))
    // Negative values indicate offscreen
    uint32_t pos = 0;
    if (x >= 0) {
        pos |= x & 0xFFF;
    } else {
        pos |= (1 << 15) | ((-x) & 0x3F);
    }
    if (y >= 0) {
        pos |= (y & 0xFFF) << 16;
    } else {
        pos |= (1 << 31) | (((-y) & 0x3F) << 16);
    }

    intel_write32(INTEL_REG_CURAPOS, pos);
}

void intel_gpu_show_cursor(bool show) {
    if (!gpu_state.initialized) return;

    if (show && gpu_state.cursor_buffer) {
        uint32_t cur_ctl = INTEL_CURCNTR_ENABLE | INTEL_CURCNTR_ARGB | INTEL_CURCNTR_64X64;
        intel_write32(INTEL_REG_CURACNTR, cur_ctl);
        gpu_state.cursor_enabled = true;
    } else {
        intel_write32(INTEL_REG_CURACNTR, 0);
        gpu_state.cursor_enabled = false;
    }
}

// ============================================================================
// UEFI Integration
// ============================================================================

int intel_gpu_use_uefi_fb(framebuffer_info_t *fb_info) {
    if (!fb_info) return INTEL_GPU_ERR_NOT_INIT;

    // Use the framebuffer provided by UEFI GOP
    gpu_state.fb_phys = fb_info->address;
    gpu_state.fb_width = fb_info->width;
    gpu_state.fb_height = fb_info->height;
    gpu_state.fb_pitch = fb_info->pitch;
    gpu_state.fb_bpp = fb_info->bpp;
    gpu_state.fb_format = fb_info->pixel_format;
    gpu_state.fb_size = fb_info->pitch * fb_info->height;

    kprintf("[INTEL_GPU] Using UEFI framebuffer: %ux%u, %u bpp\n",
            fb_info->width, fb_info->height, fb_info->bpp);

    // We don't own the display, just use the existing configuration
    gpu_state.owns_display = false;
    gpu_state.display_enabled = true;

    // Try to update plane registers to point to this framebuffer
    if (gpu_state.initialized && gpu_state.mmio) {
        intel_setup_plane(fb_info->address, fb_info->width, fb_info->height,
                          fb_info->pitch, fb_info->pixel_format);
    }

    return INTEL_GPU_OK;
}

bool intel_gpu_should_fallback(void) {
    // Suggest fallback if:
    // - Not an Intel GPU
    // - Unknown generation (might have incompatible registers)
    // - EDID read failed and we don't have mode info

    if (!gpu_state.initialized) return true;
    if (gpu_state.gen == INTEL_GEN_UNKNOWN) return true;

    return false;
}

// ============================================================================
// Debug/Diagnostics
// ============================================================================

void intel_gpu_print_info(void) {
    kprintf("\n[INTEL_GPU] Driver Information:\n");
    kprintf("  Initialized:   %s\n", gpu_state.initialized ? "Yes" : "No");

    if (!gpu_state.initialized) return;

    kprintf("  PCI Device:    %04x:%04x at %02x:%02x.%x\n",
            gpu_state.vendor_id, gpu_state.device_id,
            gpu_state.pci_bus, gpu_state.pci_slot, gpu_state.pci_func);
    kprintf("  GPU:           %s (Gen %d)\n", gpu_state.gen_name, gpu_state.gen);
    kprintf("  MMIO:          0x%llx, %u KB\n", gpu_state.mmio_phys, gpu_state.mmio_size / 1024);

    if (gpu_state.gtt_phys) {
        kprintf("  GTT Aperture:  0x%llx, %u MB\n", gpu_state.gtt_phys,
                (uint32_t)(gpu_state.gtt_size / MB));
    }

    kprintf("  EDID Valid:    %s\n", gpu_state.edid_valid ? "Yes" : "No");
    if (gpu_state.edid_valid) {
        kprintf("  Native Res:    %ux%u\n", gpu_state.h_active, gpu_state.v_active);
    }

    kprintf("  Framebuffer:   0x%llx, %ux%u, %u bpp\n",
            gpu_state.fb_phys, gpu_state.fb_width, gpu_state.fb_height, gpu_state.fb_bpp);
    kprintf("  Display:       %s\n", gpu_state.display_enabled ? "Enabled" : "Disabled");
    kprintf("  VGA Disabled:  %s\n", gpu_state.vga_disabled ? "Yes" : "No");
    kprintf("  BLT Engine:    %s\n", gpu_state.blt_enabled ? "Enabled" : "Disabled");
    kprintf("  Cursor:        %s\n", gpu_state.cursor_enabled ? "Enabled" : "Disabled");
    kprintf("  BLT Ops:       %llu\n", gpu_state.blt_ops);
    kprintf("  VBlank Count:  %llu\n", gpu_state.vblank_count);
}

void intel_gpu_dump_registers(void) {
    if (!gpu_state.initialized) return;

    kprintf("\n[INTEL_GPU] Register Dump:\n");
    kprintf("  VGA_CONTROL:   0x%08x\n", intel_read32(INTEL_REG_VGACNTRL));
    kprintf("  PIPEA_CONF:    0x%08x\n", intel_read32(INTEL_REG_PIPEA_CONF));
    kprintf("  DSPACNTR:      0x%08x\n", intel_read32(INTEL_REG_DSPACNTR));
    kprintf("  DSPASTRIDE:    0x%08x\n", intel_read32(INTEL_REG_DSPASTRIDE));
    kprintf("  DSPASURF:      0x%08x\n", intel_read32(INTEL_REG_DSPASURF));
    kprintf("  HTOTAL_A:      0x%08x\n", intel_read32(INTEL_REG_HTOTAL_A));
    kprintf("  VTOTAL_A:      0x%08x\n", intel_read32(INTEL_REG_VTOTAL_A));
    kprintf("  PIPEASRC:      0x%08x\n", intel_read32(INTEL_REG_PIPEASRC));

    if (gpu_state.blt_enabled) {
        kprintf("  BLT_RING_CTL:  0x%08x\n", intel_read32(INTEL_REG_BLT_RING_CTL));
        kprintf("  BLT_RING_HEAD: 0x%08x\n", intel_read32(INTEL_REG_BLT_RING_HEAD));
        kprintf("  BLT_RING_TAIL: 0x%08x\n", intel_read32(INTEL_REG_BLT_RING_TAIL));
    }
}

int intel_gpu_read_edid(uint8_t *buffer, uint32_t size) {
    if (!gpu_state.initialized) return INTEL_GPU_ERR_NOT_INIT;

    if (gpu_state.edid_valid && size >= 128) {
        memcpy(buffer, gpu_state.edid, size > 256 ? 256 : size);
        return INTEL_GPU_OK;
    }

    return intel_read_edid_gmbus(buffer, size);
}
