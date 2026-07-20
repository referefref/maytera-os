// ac97.c - Intel AC97 Audio Codec Driver Implementation
//
// This driver implements the AC97 audio standard, which is widely
// emulated in virtual machines (QEMU, VirtualBox, VMware).

#include "ac97.h"
#include "pci.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../cpu/idt.h"
#include "../gui/syslog.h"

// ============================================================================
// Driver State
// ============================================================================

static ac97_state_t ac97_state = {0};

// DMA buffer configuration
#define AC97_DMA_BUFFER_SIZE    (64 * 1024)     // 64KB total DMA buffer
#define AC97_BD_BUFFER_SIZE     (AC97_DMA_BUFFER_SIZE / AC97_NUM_BD)  // Per BD

// ============================================================================
// Helper Functions
// ============================================================================

// Read from Native Audio Mixer BAR
static inline uint16_t ac97_mixer_read(uint8_t reg) {
    return inw(ac97_state.nambar + reg);
}

static inline void ac97_mixer_write(uint8_t reg, uint16_t value) {
    outw(ac97_state.nambar + reg, value);
}

// Read from Native Audio Bus Master BAR
static inline uint8_t ac97_nabm_read8(uint8_t channel, uint8_t reg) {
    return inb(ac97_state.nabmbar + channel + reg);
}

static inline uint16_t ac97_nabm_read16(uint8_t channel, uint8_t reg) {
    return inw(ac97_state.nabmbar + channel + reg);
}

static inline uint32_t ac97_nabm_read32(uint8_t channel, uint8_t reg) {
    return inl(ac97_state.nabmbar + channel + reg);
}

static inline void ac97_nabm_write8(uint8_t channel, uint8_t reg, uint8_t value) {
    outb(ac97_state.nabmbar + channel + reg, value);
}

static inline void ac97_nabm_write16(uint8_t channel, uint8_t reg, uint16_t value) {
    outw(ac97_state.nabmbar + channel + reg, value);
}

static inline void ac97_nabm_write32(uint8_t channel, uint8_t reg, uint32_t value) {
    outl(ac97_state.nabmbar + channel + reg, value);
}

// Global control/status
static inline uint32_t ac97_glob_read(uint8_t reg) {
    return inl(ac97_state.nabmbar + reg);
}

static inline void ac97_glob_write(uint8_t reg, uint32_t value) {
    outl(ac97_state.nabmbar + reg, value);
}

// Delay loop
static void ac97_delay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 10; i++) {
        io_wait();
    }
}

// ============================================================================
// Codec Operations
// ============================================================================

// Wait for codec ready (read completion)
__attribute__((unused)) static bool ac97_codec_wait(void) {
    for (int i = 0; i < 1000; i++) {
        uint32_t status = ac97_glob_read(AC97_NABM_GLOB_STA);
        if (status & AC97_GS_RCS) {
            return false;  // Read error
        }
        if (status & AC97_GS_PCR) {
            return true;   // Primary codec ready
        }
        ac97_delay(10);
    }
    return false;
}

// ============================================================================
// DMA Buffer Management
// ============================================================================

static int ac97_setup_dma_buffers(void) {
    // Allocate Buffer Descriptor List (must be 8-byte aligned)
    // BD list must be below 4GB for 32-bit DMA
    size_t bd_list_size = AC97_NUM_BD * sizeof(ac97_bd_t);
    ac97_state.bd_list = (ac97_bd_t *)kzalloc_aligned(bd_list_size, 8);
    if (!ac97_state.bd_list) {
        kprintf("[AC97] Failed to allocate BD list\n");
        return AUDIO_ERR_NO_MEMORY;
    }
    ac97_state.bd_list_phys = (uint64_t)(uintptr_t)ac97_state.bd_list;

    // Allocate DMA buffer (must be contiguous, below 4GB)
    ac97_state.dma_buffer_size = AC97_DMA_BUFFER_SIZE;
    ac97_state.bd_buffer_size = AC97_BD_BUFFER_SIZE;
    ac97_state.dma_buffer = kzalloc_aligned(ac97_state.dma_buffer_size, PAGE_SIZE);
    if (!ac97_state.dma_buffer) {
        kfree(ac97_state.bd_list);
        kprintf("[AC97] Failed to allocate DMA buffer\n");
        return AUDIO_ERR_NO_MEMORY;
    }
    ac97_state.dma_buffer_phys = (uint64_t)(uintptr_t)ac97_state.dma_buffer;

    // Setup Buffer Descriptors to point to DMA buffer regions
    uint32_t buffer_addr = (uint32_t)ac97_state.dma_buffer_phys;
    for (int i = 0; i < AC97_NUM_BD; i++) {
        ac97_state.bd_list[i].addr = buffer_addr + (i * ac97_state.bd_buffer_size);
        // samples = (buffer_size / bytes_per_sample) - 1
        // For 16-bit stereo: bytes_per_sample = 4, so samples = buffer_size/4 - 1
        ac97_state.bd_list[i].samples = (ac97_state.bd_buffer_size / 2) - 1;
        ac97_state.bd_list[i].control = AC97_BD_IOC;  // Interrupt on completion
    }

    kprintf("[AC97] DMA buffers allocated: BD@0x%x, Buffer@0x%x\n",
            (uint32_t)ac97_state.bd_list_phys, (uint32_t)ac97_state.dma_buffer_phys);

    return AUDIO_OK;
}

static void ac97_free_dma_buffers(void) {
    if (ac97_state.bd_list) {
        kfree(ac97_state.bd_list);
        ac97_state.bd_list = NULL;
    }
    if (ac97_state.dma_buffer) {
        kfree(ac97_state.dma_buffer);
        ac97_state.dma_buffer = NULL;
    }
}

// ============================================================================
// Hardware Reset and Initialization
// ============================================================================

static int ac97_reset_codec(void) {
    // Perform cold reset
    uint32_t glob_cnt = ac97_glob_read(AC97_NABM_GLOB_CNT);
    glob_cnt &= ~AC97_GC_COLD_RESET;  // Clear reset bit (active low)
    ac97_glob_write(AC97_NABM_GLOB_CNT, glob_cnt);
    ac97_delay(100);

    // Release reset
    glob_cnt |= AC97_GC_COLD_RESET;
    ac97_glob_write(AC97_NABM_GLOB_CNT, glob_cnt);
    ac97_delay(100);

    // Wait for codec ready
    for (int i = 0; i < 100; i++) {
        uint32_t status = ac97_glob_read(AC97_NABM_GLOB_STA);
        if (status & AC97_GS_PCR) {
            kprintf("[AC97] Primary codec ready\n");
            return AUDIO_OK;
        }
        ac97_delay(10000);  // 10ms
    }

    kprintf("[AC97] Codec not ready after reset\n");
    return AUDIO_ERR_TIMEOUT;
}

static int ac97_init_codec(void) {
    // Reset the codec via mixer register
    ac97_mixer_write(AC97_RESET, 0);
    ac97_delay(10000);

    // Read vendor ID
    ac97_state.codec_vendor = ac97_mixer_read(AC97_VENDOR_ID1);
    ac97_state.codec_revision = ac97_mixer_read(AC97_VENDOR_ID2);

    kprintf("[AC97] Codec Vendor: 0x%04x, Revision: 0x%04x\n",
            ac97_state.codec_vendor, ac97_state.codec_revision);

    // Check extended audio capabilities
    uint16_t ext_id = ac97_mixer_read(AC97_EXT_AUDIO_ID);
    ac97_state.supports_vra = (ext_id & AC97_EXT_VRA) != 0;
    ac97_state.supports_spdif = (ext_id & AC97_EXT_SPDIF) != 0;

    kprintf("[AC97] Extended Audio ID: 0x%04x (VRA=%d, SPDIF=%d)\n",
            ext_id, ac97_state.supports_vra, ac97_state.supports_spdif);

    // Enable Variable Rate Audio if supported
    if (ac97_state.supports_vra) {
        uint16_t ext_ctrl = ac97_mixer_read(AC97_EXT_AUDIO_CTRL);
        ext_ctrl |= AC97_EXTC_VRA;
        ac97_mixer_write(AC97_EXT_AUDIO_CTRL, ext_ctrl);
        ac97_state.max_sample_rate = 48000;
    } else {
        ac97_state.max_sample_rate = 48000;  // Fixed rate
    }

    // Wait for DAC to be ready
    ac97_delay(10000);
    uint16_t powerdown = ac97_mixer_read(AC97_POWERDOWN);
    kprintf("[AC97] Powerdown status: 0x%04x\n", powerdown);

    // Power up all sections
    ac97_mixer_write(AC97_POWERDOWN, 0);
    ac97_delay(10000);

    // Set initial volumes (unmuted, reasonable level)
    ac97_mixer_write(AC97_MASTER_VOL, AC97_VOL(8, 8));      // Master at ~-12dB
    ac97_mixer_write(AC97_PCM_OUT_VOL, AC97_VOL(8, 8));     // PCM at ~-12dB
    ac97_mixer_write(AC97_AUX_OUT_VOL, AC97_VOL(8, 8));     // Headphone

    // Set default sample rate
    if (ac97_state.supports_vra) {
        ac97_mixer_write(AC97_PCM_FRONT_RATE, 48000);
    }
    ac97_state.sample_rate = 48000;
    ac97_state.channels = 2;
    ac97_state.format = AUDIO_FORMAT_S16_LE;

    return AUDIO_OK;
}

// ============================================================================
// IRQ Handler
// ============================================================================

void ac97_irq_handler(void) {
    if (!ac97_state.initialized) return;

    // Read and clear status
    uint16_t status = ac97_nabm_read16(AC97_CHAN_PCMOUT, AC97_NABM_SR);

    if (status & AC97_SR_LVBCI) {
        // Last valid buffer completed
        ac97_state.underruns++;
    }

    if (status & AC97_SR_BCIS) {
        // Buffer completion - update read index
        ac97_state.read_index = ac97_nabm_read8(AC97_CHAN_PCMOUT, AC97_NABM_CIV);
    }

    // Clear status bits by writing 1s
    ac97_nabm_write16(AC97_CHAN_PCMOUT, AC97_NABM_SR, status);
}

// ============================================================================
// Public API Implementation
// ============================================================================

int ac97_init(void) {
    LOG_INFO("[AC97] Initializing AC97 driver");
    kprintf("[AC97] Scanning for AC97 device...\n");

    if (ac97_state.initialized) {
        return AUDIO_OK;
    }

    memset(&ac97_state, 0, sizeof(ac97_state));

    // Find AC97 PCI device (Multimedia Audio Controller)
    pci_device_t *dev = pci_find_class(AC97_PCI_CLASS, AC97_PCI_SUBCLASS);
    if (!dev) {
        kprintf("[AC97] No AC97 device found\n");
        return AUDIO_ERR_NO_DEVICE;
    }

    ac97_state.pci_bus = dev->bus;
    ac97_state.pci_slot = dev->slot;
    ac97_state.pci_func = dev->func;
    ac97_state.vendor_id = dev->vendor_id;
    ac97_state.device_id = dev->device_id;
    ac97_state.irq = dev->interrupt_line;

    kprintf("[AC97] Found device %04x:%04x at %02x:%02x.%x, IRQ %d\n",
            dev->vendor_id, dev->device_id,
            dev->bus, dev->slot, dev->func, dev->interrupt_line);

    // Enable bus mastering and I/O space
    pci_enable_bus_master(dev);

    // Get BAR addresses
    ac97_state.nambar = pci_get_bar_address(dev, 0) & ~0x3;   // BAR0: Mixer
    ac97_state.nabmbar = pci_get_bar_address(dev, 1) & ~0x3;  // BAR1: Bus Master

    if (ac97_state.nambar == 0 || ac97_state.nabmbar == 0) {
        kprintf("[AC97] Invalid BAR addresses\n");
        return AUDIO_ERR_NO_DEVICE;
    }

    kprintf("[AC97] NAMBAR=0x%x, NABMBAR=0x%x\n",
            ac97_state.nambar, ac97_state.nabmbar);

    // Reset codec
    int ret = ac97_reset_codec();
    if (ret != AUDIO_OK) {
        return ret;
    }

    // Initialize codec
    ret = ac97_init_codec();
    if (ret != AUDIO_OK) {
        return ret;
    }

    // Setup DMA buffers
    ret = ac97_setup_dma_buffers();
    if (ret != AUDIO_OK) {
        return ret;
    }

    // Stop any running DMA
    ac97_nabm_write8(AC97_CHAN_PCMOUT, AC97_NABM_CR, 0);
    ac97_delay(100);

    // Reset PCM Out channel
    ac97_nabm_write8(AC97_CHAN_PCMOUT, AC97_NABM_CR, AC97_CR_RR);
    ac97_delay(100);
    ac97_nabm_write8(AC97_CHAN_PCMOUT, AC97_NABM_CR, 0);

    // Set BD list base address
    ac97_nabm_write32(AC97_CHAN_PCMOUT, AC97_NABM_BDBAR, 
                      (uint32_t)ac97_state.bd_list_phys);

    // Clear status
    ac97_nabm_write16(AC97_CHAN_PCMOUT, AC97_NABM_SR, 
                      AC97_SR_LVBCI | AC97_SR_CELV | AC97_SR_BCIS | AC97_SR_FIFOE);

    ac97_state.initialized = true;
    ac97_state.playing = false;
    ac97_state.write_index = 0;
    ac97_state.read_index = 0;

    LOG_INFO("[AC97] Initialization complete");
    kprintf("[AC97] Initialization complete\n");

    return AUDIO_OK;
}

void ac97_shutdown(void) {
    if (!ac97_state.initialized) {
        return;
    }

    // Stop playback
    ac97_stop();

    // Free DMA buffers
    ac97_free_dma_buffers();

    ac97_state.initialized = false;
    LOG_INFO("[AC97] Shutdown complete");
}

bool ac97_is_available(void) {
    return ac97_state.initialized;
}

int ac97_get_device_info(audio_device_info_t *info) {
    if (!info) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    info->type = AUDIO_DEVICE_AC97;
    info->name = "AC97 Audio Codec";
    info->description = "Intel AC97 Audio Controller";
    info->supported_formats = AUDIO_FORMAT_S16_LE;
    info->min_sample_rate = 8000;
    info->max_sample_rate = ac97_state.max_sample_rate;
    info->max_channels = 2;
    info->supports_mixing = false;
    info->supports_src = ac97_state.supports_vra;

    return AUDIO_OK;
}

int ac97_configure(uint32_t format, uint32_t sample_rate, uint32_t channels) {
    if (!ac97_state.initialized) {
        return AUDIO_ERR_NOT_INITIALIZED;
    }

    // AC97 only supports 16-bit signed LE
    if (format != AUDIO_FORMAT_S16_LE && format != 0) {
        kprintf("[AC97] Only S16_LE format supported\n");
        return AUDIO_ERR_INVALID_FORMAT;
    }

    // Channels: only mono or stereo
    if (channels > 2) {
        channels = 2;
    }
    if (channels == 0) {
        channels = 2;
    }

    // Sample rate
    if (sample_rate == 0) {
        sample_rate = 48000;
    }

    // Set sample rate if VRA is supported
    if (ac97_state.supports_vra) {
        ac97_mixer_write(AC97_PCM_FRONT_RATE, (uint16_t)sample_rate);
        // Read back actual rate
        uint16_t actual_rate = ac97_mixer_read(AC97_PCM_FRONT_RATE);
        sample_rate = actual_rate;
    } else {
        // Fixed 48kHz
        sample_rate = 48000;
    }

    ac97_state.format = AUDIO_FORMAT_S16_LE;
    ac97_state.sample_rate = sample_rate;
    ac97_state.channels = channels;

    // Update BD sample count based on format
    // For 16-bit stereo: 4 bytes per frame
    uint32_t bytes_per_sample = 2;  // 16-bit
    for (int i = 0; i < AC97_NUM_BD; i++) {
        ac97_state.bd_list[i].samples = (ac97_state.bd_buffer_size / bytes_per_sample) - 1;
    }

    kprintf("[AC97] Configured: %u Hz, %u channels\n", sample_rate, channels);

    return AUDIO_OK;
}

int ac97_start(void) {
    if (!ac97_state.initialized) {
        return AUDIO_ERR_NOT_INITIALIZED;
    }

    if (ac97_state.playing) {
        return AUDIO_OK;
    }

    // Set Last Valid Index to wrap around entire buffer
    ac97_nabm_write8(AC97_CHAN_PCMOUT, AC97_NABM_LVI, AC97_NUM_BD - 1);

    // Start DMA with interrupts
    uint8_t cr = AC97_CR_RPBM | AC97_CR_LVBIE | AC97_CR_IOCE;
    ac97_nabm_write8(AC97_CHAN_PCMOUT, AC97_NABM_CR, cr);

    ac97_state.playing = true;
    LOG_INFO("[AC97] Playback started");

    return AUDIO_OK;
}

int ac97_stop(void) {
    if (!ac97_state.initialized) {
        return AUDIO_ERR_NOT_INITIALIZED;
    }

    // Stop DMA
    ac97_nabm_write8(AC97_CHAN_PCMOUT, AC97_NABM_CR, 0);

    // Wait for DMA to stop
    for (int i = 0; i < 100; i++) {
        uint16_t status = ac97_nabm_read16(AC97_CHAN_PCMOUT, AC97_NABM_SR);
        if (status & AC97_SR_DCH) {
            break;
        }
        ac97_delay(100);
    }

    ac97_state.playing = false;
    LOG_INFO("[AC97] Playback stopped");

    return AUDIO_OK;
}

int ac97_write(const void *buffer, uint32_t frames) {
    if (!ac97_state.initialized || !buffer) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    // Calculate bytes per frame
    uint32_t bytes_per_frame = 2 * ac97_state.channels;  // 16-bit samples
    uint32_t bytes = frames * bytes_per_frame;

    // Find available buffer space
    uint8_t civ = ac97_nabm_read8(AC97_CHAN_PCMOUT, AC97_NABM_CIV);
    uint8_t write_idx = ac97_state.write_index;

    // Calculate available BDs (don't overwrite currently playing)
    int available = (int)civ - (int)write_idx;
    if (available <= 0) {
        available += AC97_NUM_BD;
    }
    available--;  // Keep one BD free

    if (available <= 0) {
        return 0;  // No space
    }

    // Write to available buffers
    uint32_t written_frames = 0;
    const uint8_t *src = (const uint8_t *)buffer;

    while (bytes > 0 && available > 0) {
        uint32_t bd_bytes = ac97_state.bd_buffer_size;
        if (bd_bytes > bytes) {
            bd_bytes = bytes;
        }

        // Copy data to DMA buffer
        uint8_t *dst = (uint8_t *)ac97_state.dma_buffer + 
                       (write_idx * ac97_state.bd_buffer_size);
        memcpy(dst, src, bd_bytes);

        // Update BD (samples field is in samples, not bytes)
        ac97_state.bd_list[write_idx].samples = (bd_bytes / 2) - 1;

        src += bd_bytes;
        bytes -= bd_bytes;
        written_frames += bd_bytes / bytes_per_frame;

        write_idx = (write_idx + 1) % AC97_NUM_BD;
        available--;
    }

    ac97_state.write_index = write_idx;

    // Update Last Valid Index
    uint8_t lvi = (write_idx == 0) ? (AC97_NUM_BD - 1) : (write_idx - 1);
    ac97_nabm_write8(AC97_CHAN_PCMOUT, AC97_NABM_LVI, lvi);

    ac97_state.frames_played += written_frames;

    return written_frames;
}

int ac97_avail(void) {
    if (!ac97_state.initialized) {
        return 0;
    }

    uint8_t civ = ac97_nabm_read8(AC97_CHAN_PCMOUT, AC97_NABM_CIV);
    uint8_t write_idx = ac97_state.write_index;

    int available = (int)civ - (int)write_idx;
    if (available <= 0) {
        available += AC97_NUM_BD;
    }
    available--;

    if (available < 0) available = 0;

    // Convert to frames
    uint32_t bytes_per_frame = 2 * ac97_state.channels;
    return (available * ac97_state.bd_buffer_size) / bytes_per_frame;
}

void ac97_set_volume(uint8_t left, uint8_t right) {
    if (!ac97_state.initialized) return;

    // AC97 volume is attenuation (0 = max, 63 = min)
    if (left > 63) left = 63;
    if (right > 63) right = 63;

    ac97_mixer_write(AC97_MASTER_VOL, AC97_VOL(left, right));
}

void ac97_set_pcm_volume(uint8_t left, uint8_t right) {
    if (!ac97_state.initialized) return;

    if (left > 63) left = 63;
    if (right > 63) right = 63;

    ac97_mixer_write(AC97_PCM_OUT_VOL, AC97_VOL(left, right));
}

void ac97_mute(bool mute) {
    if (!ac97_state.initialized) return;

    uint16_t vol = ac97_mixer_read(AC97_MASTER_VOL);
    if (mute) {
        vol |= AC97_VOL_MUTE;
    } else {
        vol &= ~AC97_VOL_MUTE;
    }
    ac97_mixer_write(AC97_MASTER_VOL, vol);

    vol = ac97_mixer_read(AC97_PCM_OUT_VOL);
    if (mute) {
        vol |= AC97_VOL_MUTE;
    } else {
        vol &= ~AC97_VOL_MUTE;
    }
    ac97_mixer_write(AC97_PCM_OUT_VOL, vol);
}

ac97_state_t *ac97_get_state(void) {
    return &ac97_state;
}

void ac97_print_info(void) {
    kprintf("\n[AC97] Driver Information:\n");
    kprintf("  Initialized:  %s\n", ac97_state.initialized ? "Yes" : "No");

    if (!ac97_state.initialized) return;

    kprintf("  PCI Device:   %04x:%04x at %02x:%02x.%x\n",
            ac97_state.vendor_id, ac97_state.device_id,
            ac97_state.pci_bus, ac97_state.pci_slot, ac97_state.pci_func);
    kprintf("  IRQ:          %u\n", ac97_state.irq);
    kprintf("  NAMBAR:       0x%x\n", ac97_state.nambar);
    kprintf("  NABMBAR:      0x%x\n", ac97_state.nabmbar);
    kprintf("  Codec:        0x%04x (rev 0x%04x)\n",
            ac97_state.codec_vendor, ac97_state.codec_revision);
    kprintf("  VRA Support:  %s\n", ac97_state.supports_vra ? "Yes" : "No");
    kprintf("  S/PDIF:       %s\n", ac97_state.supports_spdif ? "Yes" : "No");
    kprintf("  Sample Rate:  %u Hz\n", ac97_state.sample_rate);
    kprintf("  Channels:     %u\n", ac97_state.channels);
    kprintf("  Playing:      %s\n", ac97_state.playing ? "Yes" : "No");
    kprintf("  Frames:       %llu\n", ac97_state.frames_played);
    kprintf("  Underruns:    %llu\n", ac97_state.underruns);
}
