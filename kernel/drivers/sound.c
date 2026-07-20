// sound.c - Sound Blaster 16 audio driver implementation
#include "sound.h"
#include "../serial.h"
#include "../string.h"
#include "../gui/syslog.h"

// Global sound driver state
static sound_state_t sound_state = {0};

// DMA buffer (must be in low memory < 16MB for ISA DMA)
// For simplicity, we use a static buffer at a known address
#define DMA_BUFFER_SIZE     32768
#define DMA_BUFFER_ADDR     0x10000  // 64KB mark - safe for DMA

// PC Speaker port for fallback tone generation
#define PC_SPEAKER_PORT     0x61
#define PIT_CHANNEL2_PORT   0x42
#define PIT_COMMAND_PORT    0x43
#define PIT_FREQUENCY       1193182  // PIT oscillator frequency

// Base ports to probe for Sound Blaster
static const uint16_t sb_base_ports[] = {
    SB_BASE_220, SB_BASE_240, SB_BASE_260, SB_BASE_280
};

// Forward declarations
static int dsp_reset(uint16_t base);
static void dsp_write(uint16_t base, uint8_t value);
static uint8_t dsp_read(uint16_t base);
static void dma_setup_8bit(uint32_t address, uint16_t length);
static void dma_setup_16bit(uint32_t address, uint16_t length);

// Delay loop (approximately microseconds)
static void delay_us(uint32_t us) {
    for (uint32_t i = 0; i < us * 10; i++) {
        io_wait();
    }
}

// Reset DSP
// Returns 0 on success, -1 on failure
static int dsp_reset(uint16_t base) {
    // 1. Write 1 to reset port
    outb(base + SB_DSP_RESET, 1);

    // 2. Wait at least 3 microseconds
    delay_us(10);

    // 3. Write 0 to reset port
    outb(base + SB_DSP_RESET, 0);

    // 4. Wait for DSP to be ready (poll read status)
    //    Should return 0xAA within about 100 microseconds
    for (int i = 0; i < 1000; i++) {
        // Check if data is available
        if (inb(base + SB_DSP_RSTAT) & 0x80) {
            // Read the data
            if (inb(base + SB_DSP_READ) == 0xAA) {
                return 0;  // Success
            }
        }
        delay_us(10);
    }

    return -1;  // Timeout/failure
}

// Write a byte to DSP
static void dsp_write(uint16_t base, uint8_t value) {
    // Wait until DSP is ready to accept data (bit 7 clear)
    for (int i = 0; i < 1000; i++) {
        if ((inb(base + SB_DSP_WSTAT) & 0x80) == 0) {
            outb(base + SB_DSP_WRITE, value);
            return;
        }
        delay_us(1);
    }
    kprintf("[SOUND] DSP write timeout\n");
}

// Read a byte from DSP
static uint8_t dsp_read(uint16_t base) {
    // Wait until data is available (bit 7 set)
    for (int i = 0; i < 1000; i++) {
        if (inb(base + SB_DSP_RSTAT) & 0x80) {
            return inb(base + SB_DSP_READ);
        }
        delay_us(1);
    }
    kprintf("[SOUND] DSP read timeout\n");
    return 0;
}

// Get DSP version
static void dsp_get_version(uint16_t base, uint8_t *major, uint8_t *minor) {
    dsp_write(base, DSP_CMD_GET_VERSION);
    *major = dsp_read(base);
    *minor = dsp_read(base);
}

// Write to mixer register
static void mixer_write(uint16_t base, uint8_t reg, uint8_t value) {
    outb(base + SB_MIXER_ADDR, reg);
    io_wait();
    outb(base + SB_MIXER_DATA, value);
}

// Read from mixer register
static uint8_t mixer_read(uint16_t base, uint8_t reg) {
    outb(base + SB_MIXER_ADDR, reg);
    io_wait();
    return inb(base + SB_MIXER_DATA);
}

// Setup 8-bit DMA channel 1
static void dma_setup_8bit(uint32_t address, uint16_t length) {
    // Disable DMA channel 1
    outb(DMA1_MASK_REG, 0x05);  // Mask channel 1 (bit 0 = channel, bit 2 = disable)

    // Clear flip-flop
    outb(DMA1_CLEAR_FF, 0x00);

    // Set mode: single mode, address increment, auto-init, read (memory -> I/O)
    outb(DMA1_MODE_REG, 0x59);  // Channel 1, single, auto-init, read

    // Set address (low byte first, then high byte)
    outb(DMA1_CH1_ADDR, address & 0xFF);
    outb(DMA1_CH1_ADDR, (address >> 8) & 0xFF);

    // Set page register (bits 16-23 of address)
    outb(DMA1_CH1_PAGE, (address >> 16) & 0xFF);

    // Set count (length - 1, low byte first)
    uint16_t count = length - 1;
    outb(DMA1_CH1_COUNT, count & 0xFF);
    outb(DMA1_CH1_COUNT, (count >> 8) & 0xFF);

    // Enable DMA channel 1
    outb(DMA1_MASK_REG, 0x01);  // Unmask channel 1
}

// Setup 16-bit DMA channel 5
static void __attribute__((unused)) dma_setup_16bit(uint32_t address, uint16_t length) {
    // For 16-bit DMA, address and length are in words (2-byte units)
    uint32_t word_addr = address >> 1;
    uint16_t word_count = (length >> 1) - 1;

    // Disable DMA channel 5
    outb(DMA2_MASK_REG, 0x05);  // Mask channel 5 (channel 1 of DMA2)

    // Clear flip-flop
    outb(DMA2_CLEAR_FF, 0x00);

    // Set mode: single mode, address increment, auto-init, read
    outb(DMA2_MODE_REG, 0x59);  // Channel 5, single, auto-init, read

    // Set address
    outb(DMA2_CH5_ADDR, word_addr & 0xFF);
    outb(DMA2_CH5_ADDR, (word_addr >> 8) & 0xFF);

    // Set page register
    outb(DMA2_CH5_PAGE, (address >> 16) & 0xFF);

    // Set count
    outb(DMA2_CH5_COUNT, word_count & 0xFF);
    outb(DMA2_CH5_COUNT, (word_count >> 8) & 0xFF);

    // Enable DMA channel 5
    outb(DMA2_MASK_REG, 0x01);  // Unmask channel 5
}

// Initialize sound driver
int sound_init(void) {
    LOG_INFO("[Sound] Initializing Sound Blaster driver");
    kprintf("[SOUND] Initializing Sound Blaster driver...\n");

    // Clear state
    memset(&sound_state, 0, sizeof(sound_state));

    // Probe for Sound Blaster at standard ports
    for (int i = 0; i < (int)(sizeof(sb_base_ports) / sizeof(sb_base_ports[0])); i++) {
        uint16_t base = sb_base_ports[i];

        kprintf("[SOUND] Probing port 0x%x... ", base);

        if (dsp_reset(base) == 0) {
            // Card found!
            sound_state.base_port = base;
            sound_state.detected = true;

            // Get DSP version
            dsp_get_version(base, &sound_state.dsp_version_major,
                           &sound_state.dsp_version_minor);

            kprintf("Found! DSP version %u.%u\n",
                    sound_state.dsp_version_major,
                    sound_state.dsp_version_minor);

            // Determine card type and set defaults
            if (sound_state.dsp_version_major >= 4) {
                // SB16 or newer
                sound_state.irq = 5;  // Common default
                sound_state.dma_8bit = 1;
                sound_state.dma_16bit = 5;

                // Try to read IRQ/DMA from mixer
                uint8_t irq_reg = mixer_read(base, MIXER_IRQ_SELECT);
                uint8_t dma_reg = mixer_read(base, MIXER_DMA_SELECT);

                // Decode IRQ (bits 0=IRQ2, 1=IRQ5, 2=IRQ7, 3=IRQ10)
                if (irq_reg & 0x01) sound_state.irq = 2;
                else if (irq_reg & 0x02) sound_state.irq = 5;
                else if (irq_reg & 0x04) sound_state.irq = 7;
                else if (irq_reg & 0x08) sound_state.irq = 10;

                // Decode DMA (bits 0=DMA0, 1=DMA1, 3=DMA3 for 8-bit)
                // (bits 5=DMA5, 6=DMA6, 7=DMA7 for 16-bit)
                if (dma_reg & 0x01) sound_state.dma_8bit = 0;
                else if (dma_reg & 0x02) sound_state.dma_8bit = 1;
                else if (dma_reg & 0x08) sound_state.dma_8bit = 3;

                if (dma_reg & 0x20) sound_state.dma_16bit = 5;
                else if (dma_reg & 0x40) sound_state.dma_16bit = 6;
                else if (dma_reg & 0x80) sound_state.dma_16bit = 7;

            } else if (sound_state.dsp_version_major >= 3) {
                // SBPro
                sound_state.irq = 5;
                sound_state.dma_8bit = 1;
                sound_state.dma_16bit = 0;  // No 16-bit DMA
            } else {
                // Original SB or SB 2.0
                sound_state.irq = 7;
                sound_state.dma_8bit = 1;
                sound_state.dma_16bit = 0;
            }

            // Turn on speaker
            dsp_write(base, DSP_CMD_SPEAKER_ON);

            // Set default volume (75%)
            sound_set_volume(192);

            kprintf("[SOUND] Card configured: IRQ %u, DMA8 %u, DMA16 %u\n",
                    sound_state.irq, sound_state.dma_8bit, sound_state.dma_16bit);

            return SOUND_STATUS_OK;
        } else {
            kprintf("Not found\n");
        }
    }

    LOG_WARNING("[Sound] No Sound Blaster card detected");
    kprintf("[SOUND] No Sound Blaster card detected\n");
    kprintf("[SOUND] PC Speaker fallback available for tones\n");
    return SOUND_STATUS_NOT_FOUND;
}

// Play a tone using PC Speaker (fallback) or direct DAC
void sound_play_tone(uint32_t frequency, uint32_t duration_ms) {
    if (frequency < 20 || frequency > 20000) {
        kprintf("[SOUND] Frequency out of range: %u Hz\n", frequency);
        return;
    }

    if (sound_state.detected) {
        // Use Sound Blaster direct DAC output for simple tones
        uint16_t base = sound_state.base_port;

        // Generate a simple square wave using direct DAC
        // Calculate samples per cycle
        uint32_t sample_rate = 22050;  // 22.05 kHz
        uint32_t samples_per_cycle = sample_rate / frequency;
        uint32_t total_samples = (sample_rate * duration_ms) / 1000;

        // Turn on speaker
        dsp_write(base, DSP_CMD_SPEAKER_ON);

        // Output samples using direct DAC (slow but simple)
        uint32_t sample = 0;
        for (uint32_t i = 0; i < total_samples; i++) {
            // Generate square wave: high for half cycle, low for other half
            uint8_t value = (sample < samples_per_cycle / 2) ? 0xFF : 0x00;

            // Send to DAC
            dsp_write(base, DSP_CMD_8BIT_OUT);
            dsp_write(base, value);

            sample++;
            if (sample >= samples_per_cycle) {
                sample = 0;
            }

            // Simple timing delay (very approximate)
            delay_us(45);  // ~22kHz
        }

        // Turn off speaker
        dsp_write(base, DSP_CMD_SPEAKER_OFF);

    } else {
        // Use PC Speaker as fallback
        kprintf("[SOUND] Using PC Speaker for tone\n");

        // Calculate PIT divisor for desired frequency
        uint32_t divisor = PIT_FREQUENCY / frequency;

        // Set PIT channel 2 to square wave mode
        outb(PIT_COMMAND_PORT, 0xB6);  // Channel 2, lobyte/hibyte, mode 3, binary

        // Set frequency divisor
        outb(PIT_CHANNEL2_PORT, divisor & 0xFF);
        outb(PIT_CHANNEL2_PORT, (divisor >> 8) & 0xFF);

        // Enable speaker (connect PIT to speaker)
        uint8_t tmp = inb(PC_SPEAKER_PORT);
        outb(PC_SPEAKER_PORT, tmp | 0x03);  // Enable speaker and PIT gate

        // Wait for duration
        for (uint32_t i = 0; i < duration_ms; i++) {
            delay_us(1000);  // 1ms delay
        }

        // Disable speaker
        tmp = inb(PC_SPEAKER_PORT);
        outb(PC_SPEAKER_PORT, tmp & 0xFC);  // Disable speaker and PIT gate
    }
}

// Play PCM audio buffer using DMA
int sound_play_buffer(const void *data, uint32_t size, uint32_t sample_rate) {
    if (!sound_state.detected) {
        LOG_WARNING("[Sound] Playback requested but no sound card");
        kprintf("[SOUND] No sound card detected\n");
        return SOUND_STATUS_NOT_FOUND;
    }

    if (data == NULL || size == 0) {
        LOG_ERROR("[Sound] Invalid playback buffer");
        return SOUND_STATUS_DMA_ERROR;
    }

    LOG_INFO("[Sound] Starting PCM playback");

    // Limit size to DMA buffer
    if (size > DMA_BUFFER_SIZE) {
        size = DMA_BUFFER_SIZE;
    }

    uint16_t base = sound_state.base_port;

    // Copy data to DMA buffer (must be < 16MB for ISA DMA)
    uint8_t *dma_buffer = (uint8_t *)DMA_BUFFER_ADDR;
    memcpy(dma_buffer, data, size);

    // Turn on speaker
    dsp_write(base, DSP_CMD_SPEAKER_ON);

    // Setup DMA transfer
    dma_setup_8bit(DMA_BUFFER_ADDR, size);

    // For SB16 (DSP version >= 4.0), use SB16 programming method
    if (sound_state.dsp_version_major >= 4) {
        // Set sample rate (SB16 method)
        dsp_write(base, DSP_CMD_SET_SAMPLE_RATE);
        dsp_write(base, (sample_rate >> 8) & 0xFF);
        dsp_write(base, sample_rate & 0xFF);

        // Start 8-bit single-cycle DMA output
        // Command: 0xC0 + mode bits
        // Mode: bit 4 = signed, bit 5 = stereo
        dsp_write(base, DSP_CMD_8BIT_DMA_OUT_SB16);  // 8-bit output
        dsp_write(base, DSP_MODE_MONO_UNSIGNED);     // Mono, unsigned

        // Length - 1 (low byte, high byte)
        uint16_t length = size - 1;
        dsp_write(base, length & 0xFF);
        dsp_write(base, (length >> 8) & 0xFF);

    } else {
        // For older Sound Blasters, use time constant
        // Time constant = 256 - (1000000 / sample_rate)
        uint8_t time_constant = 256 - (1000000 / sample_rate);

        dsp_write(base, DSP_CMD_SET_TIME_CONST);
        dsp_write(base, time_constant);

        // Set transfer length
        uint16_t length = size - 1;

        // Start 8-bit DMA output
        dsp_write(base, DSP_CMD_8BIT_DMA_OUT);
        dsp_write(base, length & 0xFF);
        dsp_write(base, (length >> 8) & 0xFF);
    }

    sound_state.playing = true;

    return SOUND_STATUS_OK;
}

// Stop playback
void sound_stop(void) {
    if (!sound_state.detected) {
        return;
    }

    uint16_t base = sound_state.base_port;

    // Stop DMA
    dsp_write(base, DSP_CMD_STOP_8BIT);

    // Turn off speaker
    dsp_write(base, DSP_CMD_SPEAKER_OFF);

    sound_state.playing = false;
}

// Set master volume
void sound_set_volume(uint8_t volume) {
    if (!sound_state.detected) {
        return;
    }

    uint16_t base = sound_state.base_port;

    // For SB16, volume is 0-255 for left and right channels
    // Lower nibble = right, upper nibble = left
    if (sound_state.dsp_version_major >= 4) {
        // SB16 master volume register
        mixer_write(base, 0x30, volume);  // Master left
        mixer_write(base, 0x31, volume);  // Master right
        mixer_write(base, 0x32, volume);  // Voice left
        mixer_write(base, 0x33, volume);  // Voice right
    } else {
        // Older cards: combined L/R in one byte
        uint8_t vol = ((volume >> 4) << 4) | (volume >> 4);
        mixer_write(base, MIXER_MASTER_VOL, vol);
        mixer_write(base, MIXER_VOICE_VOL, vol);
    }
}

// Get sound driver state
sound_state_t *sound_get_state(void) {
    return &sound_state;
}

// Check if sound card is detected
bool sound_is_detected(void) {
    return sound_state.detected;
}

// Check if currently playing
bool sound_is_playing(void) {
    return sound_state.playing;
}

// Print sound card info
void sound_print_info(void) {
    kprintf("\n[SOUND] Sound Card Information:\n");

    if (!sound_state.detected) {
        kprintf("  Status: No Sound Blaster detected\n");
        kprintf("  PC Speaker fallback available\n");
        return;
    }

    kprintf("  Status:      Detected\n");
    kprintf("  Base Port:   0x%x\n", sound_state.base_port);
    kprintf("  DSP Version: %u.%u\n",
            sound_state.dsp_version_major, sound_state.dsp_version_minor);

    // Determine card type from DSP version
    const char *card_type;
    if (sound_state.dsp_version_major >= 4) {
        card_type = "Sound Blaster 16";
    } else if (sound_state.dsp_version_major == 3) {
        card_type = "Sound Blaster Pro";
    } else if (sound_state.dsp_version_major == 2) {
        card_type = "Sound Blaster 2.0";
    } else {
        card_type = "Sound Blaster 1.x";
    }
    kprintf("  Card Type:   %s\n", card_type);

    kprintf("  IRQ:         %u\n", sound_state.irq);
    kprintf("  8-bit DMA:   %u\n", sound_state.dma_8bit);
    if (sound_state.dma_16bit > 0) {
        kprintf("  16-bit DMA:  %u\n", sound_state.dma_16bit);
    }
    kprintf("  Playing:     %s\n", sound_state.playing ? "Yes" : "No");
}
