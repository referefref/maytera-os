// intel_hda.c - Intel High Definition Audio Controller Driver (Enhanced)
//
// Enhanced driver for Intel HD Audio on real Intel hardware.
// Features improved codec discovery, path tracing, and better compatibility
// with common codecs like Realtek ALC series.

#include "intel_hda.h"
#include "pci.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../gui/syslog.h"

// Include base HDA definitions from existing driver
#include "hda.h"

// ============================================================================
// Driver State
// ============================================================================

static intel_hda_state_t ihda_state = {0};

// DMA Configuration
#define IHDA_DMA_BUFFER_SIZE    (128 * 1024)    // 128KB
#define IHDA_BDL_ENTRIES        32
#define IHDA_BDL_BUFFER_SIZE    (IHDA_DMA_BUFFER_SIZE / IHDA_BDL_ENTRIES)
#define IHDA_CORB_SIZE          256
#define IHDA_RIRB_SIZE          256

// ============================================================================
// MMIO Access
// ============================================================================

static inline uint8_t ihda_read8(uint32_t offset) {
    return ihda_state.mmio[offset];
}

static inline uint16_t ihda_read16(uint32_t offset) {
    return *(volatile uint16_t *)(ihda_state.mmio + offset);
}

static inline uint32_t ihda_read32(uint32_t offset) {
    return *(volatile uint32_t *)(ihda_state.mmio + offset);
}

static inline void ihda_write8(uint32_t offset, uint8_t value) {
    ihda_state.mmio[offset] = value;
}

static inline void ihda_write16(uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(ihda_state.mmio + offset) = value;
}

static inline void ihda_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(ihda_state.mmio + offset) = value;
}

static uint32_t ihda_stream_offset(uint8_t stream_idx) {
    return HDA_REG_SD_BASE + (stream_idx * HDA_REG_SD_SIZE);
}

static void ihda_delay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 10; i++) {
        io_wait();
    }
}

// ============================================================================
// Controller Name Lookup
// ============================================================================

static const char *ihda_get_controller_name(uint16_t device_id) {
    switch (device_id) {
        case INTEL_HDA_ICH6:     return "ICH6";
        case INTEL_HDA_ICH7:     return "ICH7";
        case INTEL_HDA_ICH8:     return "ICH8";
        case INTEL_HDA_ICH9:
        case INTEL_HDA_ICH9_2:   return "ICH9";
        case INTEL_HDA_ICH10:
        case INTEL_HDA_ICH10_2:  return "ICH10";
        case INTEL_HDA_PCH:
        case INTEL_HDA_PCH_2:    return "PCH (Ibex Peak)";
        case INTEL_HDA_CPT:      return "Cougar Point";
        case INTEL_HDA_PPT:      return "Panther Point";
        case INTEL_HDA_LPT:      return "Lynx Point";
        case INTEL_HDA_LPT_LP:   return "Lynx Point LP";
        case INTEL_HDA_WPT:      return "Wildcat Point";
        case INTEL_HDA_WPT_LP:   return "Wildcat Point LP";
        case INTEL_HDA_SPT:      return "Sunrise Point";
        case INTEL_HDA_SPT_LP:   return "Sunrise Point LP";
        case INTEL_HDA_KBP:      return "Kaby Point";
        case INTEL_HDA_CNP:      return "Cannon Point";
        case INTEL_HDA_CNP_LP:   return "Cannon Point LP";
        case INTEL_HDA_CMP:
        case INTEL_HDA_CMP_H:    return "Comet Point";
        case INTEL_HDA_TGP:
        case INTEL_HDA_TGP_LP:   return "Tiger Point";
        case INTEL_HDA_ADP:
        case INTEL_HDA_ADP_P:
        case INTEL_HDA_ADP_M:    return "Alder Point";
        default:                 return "Unknown Intel HDA";
    }
}

static const char *ihda_get_codec_name(uint16_t vendor, uint16_t device) {
    if (vendor == CODEC_VENDOR_REALTEK) {
        switch (device) {
            case REALTEK_ALC882: return "Realtek ALC882";
            case REALTEK_ALC883: return "Realtek ALC883";
            case REALTEK_ALC885: return "Realtek ALC885";
            case REALTEK_ALC887: return "Realtek ALC887";
            case REALTEK_ALC888: return "Realtek ALC888";
            case REALTEK_ALC889: return "Realtek ALC889";
            case REALTEK_ALC892: return "Realtek ALC892";
            case REALTEK_ALC897: return "Realtek ALC897";
            case REALTEK_ALC256: return "Realtek ALC256";
            case REALTEK_ALC269: return "Realtek ALC269";
            case REALTEK_ALC274: return "Realtek ALC274";
            case REALTEK_ALC282: return "Realtek ALC282";
            case REALTEK_ALC287: return "Realtek ALC287";
            case REALTEK_ALC290: return "Realtek ALC290";
            case REALTEK_ALC292: return "Realtek ALC292";
            case REALTEK_ALC293: return "Realtek ALC293";
            case REALTEK_ALC295: return "Realtek ALC295";
            case REALTEK_ALC298: return "Realtek ALC298";
            case REALTEK_ALC700: return "Realtek ALC700";
            default: return "Realtek (unknown)";
        }
    }
    if (vendor == CODEC_VENDOR_CONEXANT) return "Conexant";
    if (vendor == CODEC_VENDOR_IDT) return "IDT/Tempo";
    if (vendor == CODEC_VENDOR_SIGMATEL) return "Sigmatel";
    if (vendor == CODEC_VENDOR_CIRRUS) return "Cirrus Logic";
    if (vendor == CODEC_VENDOR_INTEL_HDMI) return "Intel HDMI/DP Audio";
    if (vendor == CODEC_VENDOR_ANALOG) return "Analog Devices";
    return "Unknown Codec";
}

// ============================================================================
// Controller Operations
// ============================================================================

static int ihda_reset_controller(void) {
    // Put controller in reset
    uint32_t gctl = ihda_read32(HDA_REG_GCTL);
    gctl &= ~HDA_GCTL_CRST;
    ihda_write32(HDA_REG_GCTL, gctl);

    // Wait for reset to take effect
    for (int i = 0; i < 100; i++) {
        if ((ihda_read32(HDA_REG_GCTL) & HDA_GCTL_CRST) == 0) break;
        ihda_delay(100);
    }

    ihda_delay(1000);

    // Exit reset
    gctl |= HDA_GCTL_CRST;
    ihda_write32(HDA_REG_GCTL, gctl);

    // Wait for controller to come out of reset
    for (int i = 0; i < 100; i++) {
        if (ihda_read32(HDA_REG_GCTL) & HDA_GCTL_CRST) {
            ihda_delay(1000);
            return INTEL_HDA_OK;
        }
        ihda_delay(100);
    }

    kprintf("[INTEL_HDA] Controller reset timeout\n");
    return INTEL_HDA_ERR_TIMEOUT;
}

// ============================================================================
// CORB/RIRB Setup
// ============================================================================

static int ihda_setup_corb_rirb(void) {
    // Allocate CORB (256 x 4 bytes = 1KB)
    ihda_state.corb = (volatile uint32_t *)kzalloc_aligned(IHDA_CORB_SIZE * 4, 128);
    if (!ihda_state.corb) return INTEL_HDA_ERR_NO_MEMORY;
    ihda_state.corb_phys = (uint64_t)(uintptr_t)ihda_state.corb;

    // Allocate RIRB (256 x 8 bytes = 2KB)
    ihda_state.rirb = (volatile uint64_t *)kzalloc_aligned(IHDA_RIRB_SIZE * 8, 128);
    if (!ihda_state.rirb) {
        kfree((void *)ihda_state.corb);
        return INTEL_HDA_ERR_NO_MEMORY;
    }
    ihda_state.rirb_phys = (uint64_t)(uintptr_t)ihda_state.rirb;

    // Stop CORB and RIRB
    ihda_write8(HDA_REG_CORBCTL, 0);
    ihda_write8(HDA_REG_RIRBCTL, 0);
    ihda_delay(100);

    // Set CORB base address
    ihda_write32(HDA_REG_CORBLBASE, (uint32_t)ihda_state.corb_phys);
    ihda_write32(HDA_REG_CORBUBASE, (uint32_t)(ihda_state.corb_phys >> 32));

    // Set RIRB base address
    ihda_write32(HDA_REG_RIRBLBASE, (uint32_t)ihda_state.rirb_phys);
    ihda_write32(HDA_REG_RIRBUBASE, (uint32_t)(ihda_state.rirb_phys >> 32));

    // Set sizes to 256 entries
    ihda_write8(HDA_REG_CORBSIZE, (ihda_read8(HDA_REG_CORBSIZE) & 0xFC) | 0x02);
    ihda_write8(HDA_REG_RIRBSIZE, (ihda_read8(HDA_REG_RIRBSIZE) & 0xFC) | 0x02);

    // Reset CORB read pointer
    ihda_write16(HDA_REG_CORBRP, 0x8000);
    for (int i = 0; i < 100 && !(ihda_read16(HDA_REG_CORBRP) & 0x8000); i++) {
        ihda_delay(100);
    }
    ihda_write16(HDA_REG_CORBRP, 0);
    for (int i = 0; i < 100 && (ihda_read16(HDA_REG_CORBRP) & 0x8000); i++) {
        ihda_delay(100);
    }

    // Reset pointers
    ihda_write16(HDA_REG_CORBWP, 0);
    ihda_state.corb_wp = 0;
    ihda_write16(HDA_REG_RIRBWP, 0x8000);
    ihda_state.rirb_rp = 0;

    // Set interrupt count
    ihda_write16(HDA_REG_RINTCNT, 1);

    // Start CORB and RIRB
    ihda_write8(HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    ihda_write8(HDA_REG_RIRBCTL, HDA_RIRBCTL_RUN);
    ihda_delay(100);

    kprintf("[INTEL_HDA] CORB/RIRB initialized\n");
    return INTEL_HDA_OK;
}

// ============================================================================
// Codec Command Interface
// ============================================================================

static uint32_t ihda_codec_cmd(uint8_t cad, uint8_t nid, uint32_t verb) {
    uint32_t cmd = ((uint32_t)cad << 28) | ((uint32_t)nid << 20) | verb;

    // Write to CORB
    uint16_t wp = (ihda_state.corb_wp + 1) % IHDA_CORB_SIZE;
    ihda_state.corb[wp] = cmd;
    ihda_state.corb_wp = wp;
    ihda_write16(HDA_REG_CORBWP, wp);

    // Wait for response in RIRB
    for (int i = 0; i < 1000; i++) {
        uint16_t rirb_wp = ihda_read16(HDA_REG_RIRBWP);
        if (rirb_wp != ihda_state.rirb_rp) {
            ihda_state.rirb_rp = (ihda_state.rirb_rp + 1) % IHDA_RIRB_SIZE;
            return (uint32_t)ihda_state.rirb[ihda_state.rirb_rp];
        }
        ihda_delay(10);
    }

    kprintf("[INTEL_HDA] Command timeout: cad=%d nid=%d verb=0x%x\n", cad, nid, verb);
    return 0xFFFFFFFF;
}

// Convenience macros for common commands
#define IHDA_GET_PARAM(cad, nid, param) \
    ihda_codec_cmd(cad, nid, HDA_VERB_GET_PARAM | (param))

#define IHDA_GET_CONN_LIST(cad, nid, idx) \
    ihda_codec_cmd(cad, nid, HDA_VERB_GET_CONN_LIST | (idx))

#define IHDA_GET_CONN_SELECT(cad, nid) \
    ihda_codec_cmd(cad, nid, HDA_VERB_GET_CONN_SELECT)

#define IHDA_SET_CONN_SELECT(cad, nid, idx) \
    ihda_codec_cmd(cad, nid, HDA_VERB_SET_CONN_SELECT | (idx))

#define IHDA_GET_PIN_CTL(cad, nid) \
    ihda_codec_cmd(cad, nid, HDA_VERB_GET_PIN_CTL)

#define IHDA_SET_PIN_CTL(cad, nid, val) \
    ihda_codec_cmd(cad, nid, HDA_VERB_SET_PIN_CTL | (val))

#define IHDA_GET_CONFIG_DEFAULT(cad, nid) \
    ihda_codec_cmd(cad, nid, HDA_VERB_GET_CONFIG_DEF)

#define IHDA_SET_POWER_STATE(cad, nid, state) \
    ihda_codec_cmd(cad, nid, HDA_VERB_SET_PS | (state))

#define IHDA_SET_AMP_GAIN(cad, nid, flags) \
    ihda_codec_cmd(cad, nid, HDA_VERB_SET_AMP_GAIN | (flags))

#define IHDA_SET_CONV_STREAM(cad, nid, stream, channel) \
    ihda_codec_cmd(cad, nid, HDA_VERB_SET_STREAM | ((stream) << 4) | (channel))

#define IHDA_SET_EAPD(cad, nid, val) \
    ihda_codec_cmd(cad, nid, HDA_VERB_SET_EAPD | (val))

// ============================================================================
// Codec Discovery
// ============================================================================

static void ihda_read_widget_info(intel_hda_codec_t *codec, uint8_t nid) {
    uint8_t idx = nid - codec->start_nid;
    if (idx >= 64) return;

    intel_hda_widget_t *w = &codec->widgets[idx];
    w->nid = nid;

    // Get widget capabilities
    w->widget_cap = IHDA_GET_PARAM(codec->cad, nid, HDA_PARAM_AUDIO_WIDGET_CAP);
    w->type = (w->widget_cap >> 20) & 0xF;
    w->has_input_amp = (w->widget_cap >> 1) & 1;
    w->has_output_amp = (w->widget_cap >> 2) & 1;
    w->amp_override = (w->widget_cap >> 3) & 1;

    // Get connection list
    uint32_t conn_len = IHDA_GET_PARAM(codec->cad, nid, HDA_PARAM_CONN_LIST_LEN);
    w->conn_count = conn_len & 0x7F;
    bool long_form = (conn_len >> 7) & 1;

    for (int i = 0; i < w->conn_count && i < 32; i++) {
        uint32_t conn = IHDA_GET_CONN_LIST(codec->cad, nid, i);
        if (long_form) {
            w->conn_list[i] = (conn >> ((i % 2) * 16)) & 0xFF;
        } else {
            w->conn_list[i] = (conn >> ((i % 4) * 8)) & 0xFF;
        }
    }

    // Get current connection selection
    if (w->conn_count > 1) {
        w->selected_conn = IHDA_GET_CONN_SELECT(codec->cad, nid) & 0xFF;
    }

    // Get pin-specific info
    if (w->type == HDA_WIDGET_AUDIO_OUTPUT || w->type == HDA_WIDGET_AUDIO_INPUT ||
        w->type == HDA_WIDGET_MIXER) {
        if (w->has_input_amp) {
            w->amp_in_cap = IHDA_GET_PARAM(codec->cad, nid, HDA_PARAM_AMP_IN_CAP);
        }
        if (w->has_output_amp) {
            w->amp_out_cap = IHDA_GET_PARAM(codec->cad, nid, HDA_PARAM_AMP_OUT_CAP);
        }
    }

    if (w->type == HDA_WIDGET_PIN) {
        w->pin_cap = IHDA_GET_PARAM(codec->cad, nid, HDA_PARAM_PIN_CAP);
        w->config_default = IHDA_GET_CONFIG_DEFAULT(codec->cad, nid);
    }
}

static void ihda_find_output_paths(intel_hda_codec_t *codec) {
    // Find all output pins and trace back to DACs
    codec->num_output_paths = 0;

    for (int i = 0; i < codec->num_widgets && codec->num_output_paths < 4; i++) {
        intel_hda_widget_t *w = &codec->widgets[i];
        if (w->type != HDA_WIDGET_PIN) continue;

        // Check pin connectivity and type
        uint32_t connectivity = (w->config_default >> 30) & 0x03;
        uint32_t device = (w->config_default >> 20) & 0x0F;

        // Skip pins with no physical connection
        if (connectivity == 1) continue;

        // We want output devices
        if (device != 0 && device != 1 && device != 2 && device != 5) continue;

        // Trace back to find a DAC
        intel_hda_path_t *path = &codec->output_paths[codec->num_output_paths];
        path->pin_nid = w->nid;
        path->path[0] = w->nid;
        path->path_len = 1;

        // Set output type
        path->is_speaker = (device == 1);
        path->is_headphone = (device == 2);
        path->is_line_out = (device == 0);

        // Follow connections back to find DAC
        uint8_t current = w->nid;
        bool found_dac = false;

        for (int depth = 0; depth < 5 && !found_dac; depth++) {
            intel_hda_widget_t *cw = &codec->widgets[current - codec->start_nid];

            if (cw->conn_count == 0) break;

            // Try each connection
            uint8_t next = cw->conn_list[0];
            if (cw->conn_count > 1 && cw->selected_conn < cw->conn_count) {
                next = cw->conn_list[cw->selected_conn];
            }

            if (next < codec->start_nid || next >= codec->start_nid + codec->num_widgets) break;

            intel_hda_widget_t *nw = &codec->widgets[next - codec->start_nid];

            if (nw->type == HDA_WIDGET_AUDIO_OUTPUT) {
                path->dac_nid = next;
                path->path[path->path_len++] = next;
                found_dac = true;
            } else if (nw->type == HDA_WIDGET_MIXER) {
                path->mixer_nid = next;
                path->path[path->path_len++] = next;
                current = next;
            } else if (nw->type == HDA_WIDGET_SELECTOR) {
                path->path[path->path_len++] = next;
                current = next;
            } else {
                break;
            }
        }

        if (found_dac) {
            codec->num_output_paths++;
            kprintf("[INTEL_HDA] Found output path: DAC %d -> PIN %d (%s)\n",
                    path->dac_nid, path->pin_nid,
                    path->is_speaker ? "speaker" :
                    path->is_headphone ? "headphone" : "line out");
        }
    }
}

static int ihda_probe_codec(uint8_t cad) {
    // Get vendor ID
    uint32_t vendor = IHDA_GET_PARAM(cad, 0, HDA_PARAM_VENDOR_ID);
    if (vendor == 0xFFFFFFFF || vendor == 0) return INTEL_HDA_ERR_NO_CODEC;

    intel_hda_codec_t *codec = &ihda_state.codecs[ihda_state.num_codecs];
    memset(codec, 0, sizeof(*codec));

    codec->cad = cad;
    codec->vendor_id = (vendor >> 16) & 0xFFFF;
    codec->device_id = vendor & 0xFFFF;
    codec->name = ihda_get_codec_name(codec->vendor_id, codec->device_id);

    // Get revision
    codec->revision = IHDA_GET_PARAM(cad, 0, HDA_PARAM_REVISION_ID) & 0xFFFF;

    kprintf("[INTEL_HDA] Codec %d: %s (vendor=%04x device=%04x rev=%04x)\n",
            cad, codec->name, codec->vendor_id, codec->device_id, codec->revision);

    // Get function group info
    uint32_t node_count = IHDA_GET_PARAM(cad, 0, HDA_PARAM_NODE_COUNT);
    codec->afg_nid = (node_count >> 16) & 0xFF;

    // Get widgets in the Audio Function Group
    node_count = IHDA_GET_PARAM(cad, codec->afg_nid, HDA_PARAM_NODE_COUNT);
    codec->start_nid = (node_count >> 16) & 0xFF;
    codec->num_widgets = node_count & 0xFF;

    kprintf("[INTEL_HDA] AFG NID=%d, widgets %d-%d (%d total)\n",
            codec->afg_nid, codec->start_nid,
            codec->start_nid + codec->num_widgets - 1, codec->num_widgets);

    // Read all widget info
    for (int i = 0; i < codec->num_widgets && i < 64; i++) {
        ihda_read_widget_info(codec, codec->start_nid + i);
    }

    // Find output paths
    ihda_find_output_paths(codec);

    if (codec->num_output_paths > 0) {
        // Use first output path as default
        codec->active_dac = codec->output_paths[0].dac_nid;
        codec->active_pin = codec->output_paths[0].pin_nid;
    }

    ihda_state.num_codecs++;
    return INTEL_HDA_OK;
}

static int ihda_probe_codecs(void) {
    uint16_t statests = ihda_read16(HDA_REG_STATESTS);
    kprintf("[INTEL_HDA] STATESTS: 0x%04x\n", statests);

    // Clear status
    ihda_write16(HDA_REG_STATESTS, statests);

    ihda_state.num_codecs = 0;
    ihda_state.primary_codec = 0xFF;

    for (int cad = 0; cad < 15; cad++) {
        if (statests & (1 << cad)) {
            if (ihda_probe_codec(cad) == INTEL_HDA_OK) {
                intel_hda_codec_t *codec = &ihda_state.codecs[ihda_state.num_codecs - 1];

                // Prefer non-HDMI codec as primary
                if (ihda_state.primary_codec == 0xFF ||
                    codec->vendor_id != CODEC_VENDOR_INTEL_HDMI) {
                    ihda_state.primary_codec = ihda_state.num_codecs - 1;
                }
            }
        }
    }

    if (ihda_state.num_codecs == 0) {
        kprintf("[INTEL_HDA] No codecs found\n");
        return INTEL_HDA_ERR_NO_CODEC;
    }

    kprintf("[INTEL_HDA] Found %d codec(s), primary=%d\n",
            ihda_state.num_codecs, ihda_state.primary_codec);

    return INTEL_HDA_OK;
}

// ============================================================================
// Codec Configuration
// ============================================================================

static void ihda_configure_output_path(intel_hda_codec_t *codec, intel_hda_path_t *path) {
    uint8_t cad = codec->cad;

    // Power on all nodes in the path
    IHDA_SET_POWER_STATE(cad, codec->afg_nid, 0);  // D0
    ihda_delay(10000);

    for (int i = 0; i < path->path_len; i++) {
        IHDA_SET_POWER_STATE(cad, path->path[i], 0);
    }
    ihda_delay(1000);

    // Configure DAC
    uint8_t dac = path->dac_nid;
    IHDA_SET_CONV_STREAM(cad, dac, 1, 0);  // Stream 1, channel 0

    // Set DAC amplifier to max
    intel_hda_widget_t *dac_w = &codec->widgets[dac - codec->start_nid];
    if (dac_w->has_output_amp) {
        IHDA_SET_AMP_GAIN(cad, dac,
            HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | 0x7F);
    }

    // Configure mixer if present
    if (path->mixer_nid) {
        intel_hda_widget_t *mixer_w = &codec->widgets[path->mixer_nid - codec->start_nid];
        if (mixer_w->has_output_amp) {
            IHDA_SET_AMP_GAIN(cad, path->mixer_nid,
                HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | 0x7F);
        }
    }

    // Configure output pin
    uint8_t pin = path->pin_nid;
    intel_hda_widget_t *pin_w = &codec->widgets[pin - codec->start_nid];

    // Enable output
    uint32_t pin_ctl = HDA_PIN_OUT_EN;
    if (path->is_headphone && (pin_w->pin_cap & (1 << 3))) {  // HP drive capable
        pin_ctl |= (1 << 7);  // HP enable
    }
    IHDA_SET_PIN_CTL(cad, pin, pin_ctl);

    // Enable EAPD if present
    if (pin_w->widget_cap & (1 << 16)) {  // EAPD capable
        IHDA_SET_EAPD(cad, pin, 0x02);
    }

    // Set pin amplifier if present
    if (pin_w->has_output_amp) {
        IHDA_SET_AMP_GAIN(cad, pin,
            HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | 0x7F);
    }

    kprintf("[INTEL_HDA] Output path configured: DAC %d -> PIN %d\n", dac, pin);
}

static void ihda_configure_codec(void) {
    if (ihda_state.primary_codec >= ihda_state.num_codecs) return;

    intel_hda_codec_t *codec = &ihda_state.codecs[ihda_state.primary_codec];

    if (codec->num_output_paths == 0) {
        kprintf("[INTEL_HDA] No output paths found\n");
        return;
    }

    // Configure all output paths
    for (int i = 0; i < codec->num_output_paths; i++) {
        ihda_configure_output_path(codec, &codec->output_paths[i]);
    }
}

// ============================================================================
// Stream Setup
// ============================================================================

static int ihda_setup_stream_buffers(void) {
    // Allocate BDL (32 entries x 16 bytes = 512 bytes)
    ihda_state.bdl = kzalloc_aligned(IHDA_BDL_ENTRIES * 16, 128);
    if (!ihda_state.bdl) return INTEL_HDA_ERR_NO_MEMORY;
    ihda_state.bdl_phys = (uint64_t)(uintptr_t)ihda_state.bdl;

    // Allocate DMA buffer
    ihda_state.dma_buffer_size = IHDA_DMA_BUFFER_SIZE;
    ihda_state.bdl_buffer_size = IHDA_BDL_BUFFER_SIZE;
    ihda_state.dma_buffer = kzalloc_aligned(ihda_state.dma_buffer_size, PAGE_SIZE);
    if (!ihda_state.dma_buffer) {
        kfree((void *)ihda_state.bdl);
        return INTEL_HDA_ERR_NO_MEMORY;
    }
    ihda_state.dma_buffer_phys = (uint64_t)(uintptr_t)ihda_state.dma_buffer;

    // Setup BDL entries
    volatile uint32_t *bdl = (volatile uint32_t *)ihda_state.bdl;
    for (int i = 0; i < IHDA_BDL_ENTRIES; i++) {
        uint64_t addr = ihda_state.dma_buffer_phys + (i * ihda_state.bdl_buffer_size);
        bdl[i * 4 + 0] = (uint32_t)addr;
        bdl[i * 4 + 1] = (uint32_t)(addr >> 32);
        bdl[i * 4 + 2] = ihda_state.bdl_buffer_size;
        bdl[i * 4 + 3] = 1;  // IOC
    }

    kprintf("[INTEL_HDA] Stream buffers allocated\n");
    return INTEL_HDA_OK;
}

static uint16_t ihda_calc_stream_format(uint32_t sample_rate, uint32_t channels, uint32_t bits) {
    uint16_t fmt = (channels - 1) & 0xF;

    // Bits per sample
    switch (bits) {
        case 8:  fmt |= (0 << 4); break;
        case 16: fmt |= (1 << 4); break;
        case 20: fmt |= (2 << 4); break;
        case 24: fmt |= (3 << 4); break;
        case 32: fmt |= (4 << 4); break;
        default: fmt |= (1 << 4); break;
    }

    // Sample rate
    switch (sample_rate) {
        case 8000:   fmt |= (0 << 8) | (5 << 11); break;  // 48/6
        case 11025:  fmt |= (1 << 14) | (0 << 8) | (3 << 11); break;  // 44.1/4
        case 16000:  fmt |= (0 << 8) | (2 << 11); break;  // 48/3
        case 22050:  fmt |= (1 << 14) | (0 << 8) | (1 << 11); break;  // 44.1/2
        case 32000:  fmt |= (0 << 8) | (2 << 11); break;  // Special
        case 44100:  fmt |= (1 << 14); break;  // 44.1 base
        case 48000:  break;  // 48 base (default)
        case 88200:  fmt |= (1 << 14) | (1 << 11); break;  // 44.1*2
        case 96000:  fmt |= (1 << 11); break;  // 48*2
        case 176400: fmt |= (1 << 14) | (3 << 11); break;  // 44.1*4
        case 192000: fmt |= (3 << 11); break;  // 48*4
        default: break;
    }

    return fmt;
}

static int ihda_configure_output_stream(void) {
    uint8_t stream = ihda_state.out_stream_idx;
    uint32_t sd_base = ihda_stream_offset(stream);

    // Stop stream
    uint32_t ctl = ihda_read32(sd_base + HDA_SD_CTL);
    ctl &= ~HDA_SD_CTL_RUN;
    ihda_write32(sd_base + HDA_SD_CTL, ctl);

    for (int i = 0; i < 100 && (ihda_read32(sd_base + HDA_SD_CTL) & HDA_SD_CTL_RUN); i++) {
        ihda_delay(100);
    }

    // Reset stream
    ctl |= HDA_SD_CTL_SRST;
    ihda_write32(sd_base + HDA_SD_CTL, ctl);
    ihda_delay(100);

    for (int i = 0; i < 100 && !(ihda_read32(sd_base + HDA_SD_CTL) & HDA_SD_CTL_SRST); i++) {
        ihda_delay(100);
    }

    ctl &= ~HDA_SD_CTL_SRST;
    ihda_write32(sd_base + HDA_SD_CTL, ctl);

    for (int i = 0; i < 100 && (ihda_read32(sd_base + HDA_SD_CTL) & HDA_SD_CTL_SRST); i++) {
        ihda_delay(100);
    }

    // Clear status
    ihda_write8(sd_base + HDA_SD_STS, 0x1C);

    // Configure stream
    ihda_write16(sd_base + HDA_SD_FMT, ihda_state.stream_format);
    ihda_write32(sd_base + HDA_SD_CBL, ihda_state.dma_buffer_size);
    ihda_write16(sd_base + HDA_SD_LVI, IHDA_BDL_ENTRIES - 1);
    ihda_write32(sd_base + HDA_SD_BDPL, (uint32_t)ihda_state.bdl_phys);
    ihda_write32(sd_base + HDA_SD_BDPU, (uint32_t)(ihda_state.bdl_phys >> 32));

    // Set stream number and enable IOC
    ctl = HDA_SD_CTL_IOCE | ((1 << 20) & HDA_SD_CTL_STRM_MASK);
    ihda_write32(sd_base + HDA_SD_CTL, ctl);

    kprintf("[INTEL_HDA] Stream %d configured, format=0x%04x\n",
            stream, ihda_state.stream_format);

    return INTEL_HDA_OK;
}

// ============================================================================
// Public API
// ============================================================================

int intel_hda_init(void) {
    LOG_INFO("[INTEL_HDA] Initializing Intel HD Audio driver");
    kprintf("[INTEL_HDA] Scanning for Intel HDA controller...\n");

    if (ihda_state.initialized) return INTEL_HDA_OK;

    memset(&ihda_state, 0, sizeof(ihda_state));

    // Find HDA controller
    pci_device_t *dev = pci_find_class(HDA_PCI_CLASS, HDA_PCI_SUBCLASS);
    if (!dev) {
        kprintf("[INTEL_HDA] No HDA controller found\n");
        return INTEL_HDA_ERR_NO_DEVICE;
    }

    // Store PCI info
    ihda_state.pci_bus = dev->bus;
    ihda_state.pci_slot = dev->slot;
    ihda_state.pci_func = dev->func;
    ihda_state.vendor_id = dev->vendor_id;
    ihda_state.device_id = dev->device_id;
    ihda_state.irq = dev->interrupt_line;

    const char *controller_name = "HDA Controller";
    if (dev->vendor_id == INTEL_HDA_VENDOR_ID) {
        controller_name = ihda_get_controller_name(dev->device_id);
    }

    kprintf("[INTEL_HDA] Found %s (%04x:%04x) at %02x:%02x.%x, IRQ %d\n",
            controller_name, dev->vendor_id, dev->device_id,
            dev->bus, dev->slot, dev->func, dev->interrupt_line);

    // Enable bus mastering
    pci_enable_bus_master(dev);

    // Get MMIO
    ihda_state.mmio_phys = pci_get_bar_address(dev, 0);
    ihda_state.mmio_size = pci_get_bar_size(dev, 0);

    if (ihda_state.mmio_phys == 0) {
        kprintf("[INTEL_HDA] Invalid MMIO BAR\n");
        return INTEL_HDA_ERR_NO_DEVICE;
    }

    ihda_state.mmio = (volatile uint8_t *)(uintptr_t)ihda_state.mmio_phys;
    kprintf("[INTEL_HDA] MMIO at 0x%llx, size %u\n", ihda_state.mmio_phys, ihda_state.mmio_size);

    // Read capabilities
    uint16_t gcap = ihda_read16(HDA_REG_GCAP);
    ihda_state.supports_64bit = (gcap & 1) != 0;
    ihda_state.num_oss = (gcap >> 12) & 0xF;
    ihda_state.num_iss = (gcap >> 8) & 0xF;
    ihda_state.num_bss = (gcap >> 3) & 0x1F;

    kprintf("[INTEL_HDA] GCAP=0x%04x: 64bit=%d, OSS=%d, ISS=%d, BSS=%d\n",
            gcap, ihda_state.supports_64bit, ihda_state.num_oss,
            ihda_state.num_iss, ihda_state.num_bss);

    if (ihda_state.num_oss == 0) {
        kprintf("[INTEL_HDA] No output streams\n");
        return INTEL_HDA_ERR_NO_DEVICE;
    }

    ihda_state.out_stream_idx = ihda_state.num_iss;

    // Reset controller
    int ret = ihda_reset_controller();
    if (ret != INTEL_HDA_OK) return ret;

    // Setup CORB/RIRB
    ret = ihda_setup_corb_rirb();
    if (ret != INTEL_HDA_OK) return ret;

    // Probe codecs
    ret = ihda_probe_codecs();
    if (ret != INTEL_HDA_OK) return ret;

    // Configure primary codec
    ihda_configure_codec();

    // Setup stream buffers
    ret = ihda_setup_stream_buffers();
    if (ret != INTEL_HDA_OK) return ret;

    // Default configuration
    ihda_state.sample_rate = 48000;
    ihda_state.channels = 2;
    ihda_state.format = AUDIO_FORMAT_S16_LE;
    ihda_state.stream_format = ihda_calc_stream_format(48000, 2, 16);
    ihda_state.volume_left = 127;
    ihda_state.volume_right = 127;

    // Configure stream
    ihda_configure_output_stream();

    // Enable interrupts
    uint32_t intctl = HDA_INTCTL_GIE | HDA_INTCTL_CIE | (1 << ihda_state.out_stream_idx);
    ihda_write32(HDA_REG_INTCTL, intctl);

    ihda_state.initialized = true;
    LOG_INFO("[INTEL_HDA] Initialization complete");
    kprintf("[INTEL_HDA] Initialization complete\n");

    return INTEL_HDA_OK;
}

void intel_hda_shutdown(void) {
    if (!ihda_state.initialized) return;

    intel_hda_stop();

    // Free buffers
    if (ihda_state.bdl) kfree((void *)ihda_state.bdl);
    if (ihda_state.dma_buffer) kfree(ihda_state.dma_buffer);
    if (ihda_state.corb) kfree((void *)ihda_state.corb);
    if (ihda_state.rirb) kfree((void *)ihda_state.rirb);

    ihda_state.initialized = false;
    LOG_INFO("[INTEL_HDA] Shutdown complete");
}

bool intel_hda_is_available(void) {
    return ihda_state.initialized && ihda_state.num_codecs > 0;
}

intel_hda_state_t *intel_hda_get_state(void) {
    return &ihda_state;
}

int intel_hda_get_device_info(audio_device_info_t *info) {
    if (!info) return INTEL_HDA_ERR_INVALID_PARAM;

    info->type = AUDIO_DEVICE_HDA;
    info->name = "Intel HD Audio";
    info->description = "Intel High Definition Audio Controller";
    info->supported_formats = AUDIO_FORMAT_S16_LE | AUDIO_FORMAT_S24_LE | AUDIO_FORMAT_S32_LE;
    info->min_sample_rate = 8000;
    info->max_sample_rate = 192000;
    info->max_channels = 8;
    info->supports_mixing = true;
    info->supports_src = true;

    return INTEL_HDA_OK;
}

int intel_hda_configure(uint32_t format, uint32_t sample_rate, uint32_t channels) {
    if (!ihda_state.initialized) return INTEL_HDA_ERR_NOT_INIT;

    uint32_t bits = 16;
    if (format == AUDIO_FORMAT_S24_LE) bits = 24;
    else if (format == AUDIO_FORMAT_S32_LE) bits = 32;
    else format = AUDIO_FORMAT_S16_LE;

    if (sample_rate == 0) sample_rate = 48000;
    if (channels == 0) channels = 2;
    if (channels > 8) channels = 8;

    ihda_state.format = format;
    ihda_state.sample_rate = sample_rate;
    ihda_state.channels = channels;
    ihda_state.stream_format = ihda_calc_stream_format(sample_rate, channels, bits);

    // Update DAC stream configuration
    if (ihda_state.primary_codec < ihda_state.num_codecs) {
        intel_hda_codec_t *codec = &ihda_state.codecs[ihda_state.primary_codec];
        if (codec->active_dac) {
            IHDA_SET_CONV_STREAM(codec->cad, codec->active_dac, 1, 0);
        }
    }

    ihda_configure_output_stream();

    kprintf("[INTEL_HDA] Configured: %u Hz, %u channels, %u bits\n",
            sample_rate, channels, bits);

    return INTEL_HDA_OK;
}

int intel_hda_start(void) {
    if (!ihda_state.initialized) return INTEL_HDA_ERR_NOT_INIT;
    if (ihda_state.playing) return INTEL_HDA_OK;

    uint32_t sd_base = ihda_stream_offset(ihda_state.out_stream_idx);
    uint32_t ctl = ihda_read32(sd_base + HDA_SD_CTL);
    ctl |= HDA_SD_CTL_RUN;
    ihda_write32(sd_base + HDA_SD_CTL, ctl);

    ihda_state.playing = true;
    LOG_INFO("[INTEL_HDA] Playback started");

    return INTEL_HDA_OK;
}

int intel_hda_stop(void) {
    if (!ihda_state.initialized) return INTEL_HDA_ERR_NOT_INIT;

    uint32_t sd_base = ihda_stream_offset(ihda_state.out_stream_idx);
    uint32_t ctl = ihda_read32(sd_base + HDA_SD_CTL);
    ctl &= ~HDA_SD_CTL_RUN;
    ihda_write32(sd_base + HDA_SD_CTL, ctl);

    for (int i = 0; i < 100 && (ihda_read32(sd_base + HDA_SD_CTL) & HDA_SD_CTL_RUN); i++) {
        ihda_delay(100);
    }

    ihda_state.playing = false;
    LOG_INFO("[INTEL_HDA] Playback stopped");

    return INTEL_HDA_OK;
}

int intel_hda_write(const void *buffer, uint32_t frames) {
    if (!ihda_state.initialized || !buffer) return INTEL_HDA_ERR_INVALID_PARAM;

    uint32_t bits = 16;
    if (ihda_state.format == AUDIO_FORMAT_S24_LE || ihda_state.format == AUDIO_FORMAT_S32_LE) {
        bits = 32;
    }
    uint32_t bytes_per_frame = (bits / 8) * ihda_state.channels;
    uint32_t bytes = frames * bytes_per_frame;

    uint32_t sd_base = ihda_stream_offset(ihda_state.out_stream_idx);
    uint32_t lpib = ihda_read32(sd_base + HDA_SD_LPIB);
    uint32_t current_bdl = lpib / ihda_state.bdl_buffer_size;

    int available = (int)current_bdl - (int)ihda_state.write_index;
    if (available <= 0) available += IHDA_BDL_ENTRIES;
    available--;

    if (available <= 0) return 0;

    uint32_t written_frames = 0;
    const uint8_t *src = (const uint8_t *)buffer;

    while (bytes > 0 && available > 0) {
        uint32_t bdl_bytes = ihda_state.bdl_buffer_size;
        if (bdl_bytes > bytes) bdl_bytes = bytes;

        uint8_t *dst = (uint8_t *)ihda_state.dma_buffer +
                       (ihda_state.write_index * ihda_state.bdl_buffer_size);
        memcpy(dst, src, bdl_bytes);

        src += bdl_bytes;
        bytes -= bdl_bytes;
        written_frames += bdl_bytes / bytes_per_frame;

        ihda_state.write_index = (ihda_state.write_index + 1) % IHDA_BDL_ENTRIES;
        available--;
    }

    ihda_state.frames_played += written_frames;
    return written_frames;
}

int intel_hda_avail(void) {
    if (!ihda_state.initialized) return 0;

    uint32_t sd_base = ihda_stream_offset(ihda_state.out_stream_idx);
    uint32_t lpib = ihda_read32(sd_base + HDA_SD_LPIB);
    uint32_t current_bdl = lpib / ihda_state.bdl_buffer_size;

    int available = (int)current_bdl - (int)ihda_state.write_index;
    if (available <= 0) available += IHDA_BDL_ENTRIES;
    available--;
    if (available < 0) available = 0;

    uint32_t bits = 16;
    if (ihda_state.format == AUDIO_FORMAT_S24_LE || ihda_state.format == AUDIO_FORMAT_S32_LE) {
        bits = 32;
    }
    uint32_t bytes_per_frame = (bits / 8) * ihda_state.channels;

    return (available * ihda_state.bdl_buffer_size) / bytes_per_frame;
}

// ============================================================================
// Volume Control
// ============================================================================

void intel_hda_set_volume(uint8_t left, uint8_t right) {
    if (!ihda_state.initialized) return;

    if (left > 127) left = 127;
    if (right > 127) right = 127;

    ihda_state.volume_left = left;
    ihda_state.volume_right = right;

    if (ihda_state.primary_codec >= ihda_state.num_codecs) return;
    intel_hda_codec_t *codec = &ihda_state.codecs[ihda_state.primary_codec];

    uint8_t gain_l = ihda_state.muted ? HDA_AMP_MUTE : left;
    uint8_t gain_r = ihda_state.muted ? HDA_AMP_MUTE : right;

    // Set DAC volume
    if (codec->active_dac) {
        IHDA_SET_AMP_GAIN(codec->cad, codec->active_dac,
            HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | gain_l);
        IHDA_SET_AMP_GAIN(codec->cad, codec->active_dac,
            HDA_AMP_SET_OUTPUT | HDA_AMP_SET_RIGHT | gain_r);
    }
}

void intel_hda_get_volume(uint8_t *left, uint8_t *right) {
    if (left) *left = ihda_state.volume_left;
    if (right) *right = ihda_state.volume_right;
}

void intel_hda_mute(bool mute) {
    ihda_state.muted = mute;
    intel_hda_set_volume(ihda_state.volume_left, ihda_state.volume_right);
}

bool intel_hda_is_muted(void) {
    return ihda_state.muted;
}

// ============================================================================
// Output Selection
// ============================================================================

int intel_hda_select_output(uint8_t output_type) {
    if (!ihda_state.initialized) return INTEL_HDA_ERR_NOT_INIT;
    if (ihda_state.primary_codec >= ihda_state.num_codecs) return INTEL_HDA_ERR_NO_CODEC;

    intel_hda_codec_t *codec = &ihda_state.codecs[ihda_state.primary_codec];

    // Find matching output path
    for (int i = 0; i < codec->num_output_paths; i++) {
        intel_hda_path_t *path = &codec->output_paths[i];
        bool match = false;

        switch (output_type) {
            case INTEL_HDA_OUTPUT_SPEAKER:   match = path->is_speaker; break;
            case INTEL_HDA_OUTPUT_HEADPHONE: match = path->is_headphone; break;
            case INTEL_HDA_OUTPUT_LINEOUT:   match = path->is_line_out; break;
            case INTEL_HDA_OUTPUT_AUTO:      match = true; break;
        }

        if (match) {
            codec->active_dac = path->dac_nid;
            codec->active_pin = path->pin_nid;
            ihda_configure_output_path(codec, path);
            return INTEL_HDA_OK;
        }
    }

    return INTEL_HDA_ERR_INVALID_PARAM;
}

// ============================================================================
// IRQ Handler
// ============================================================================

void intel_hda_irq_handler(void) {
    if (!ihda_state.initialized) return;

    uint32_t intsts = ihda_read32(HDA_REG_INTSTS);
    ihda_state.interrupts++;

    if (intsts & HDA_INTSTS_GIS) {
        uint32_t sd_base = ihda_stream_offset(ihda_state.out_stream_idx);
        uint8_t sd_sts = ihda_read8(sd_base + HDA_SD_STS);

        if (sd_sts & HDA_SD_STS_BCIS) {
            ihda_state.read_index = (ihda_state.read_index + 1) % IHDA_BDL_ENTRIES;
        }

        if (sd_sts & HDA_SD_STS_FIFOE) {
            ihda_state.underruns++;
        }

        ihda_write8(sd_base + HDA_SD_STS, sd_sts);
    }
}

// ============================================================================
// Debug/Info
// ============================================================================

void intel_hda_print_info(void) {
    kprintf("\n[INTEL_HDA] Driver Information:\n");
    kprintf("  Initialized:  %s\n", ihda_state.initialized ? "Yes" : "No");

    if (!ihda_state.initialized) return;

    kprintf("  Controller:   %04x:%04x at %02x:%02x.%x\n",
            ihda_state.vendor_id, ihda_state.device_id,
            ihda_state.pci_bus, ihda_state.pci_slot, ihda_state.pci_func);
    kprintf("  IRQ:          %u\n", ihda_state.irq);
    kprintf("  MMIO:         0x%llx (size %u)\n", ihda_state.mmio_phys, ihda_state.mmio_size);
    kprintf("  Streams:      OSS=%u, ISS=%u, BSS=%u\n",
            ihda_state.num_oss, ihda_state.num_iss, ihda_state.num_bss);
    kprintf("  64-bit:       %s\n", ihda_state.supports_64bit ? "Yes" : "No");
    kprintf("  Codecs:       %d\n", ihda_state.num_codecs);

    for (int i = 0; i < ihda_state.num_codecs; i++) {
        intel_hda_codec_t *codec = &ihda_state.codecs[i];
        kprintf("    [%d] %s (%04x:%04x)%s\n",
                codec->cad, codec->name, codec->vendor_id, codec->device_id,
                i == ihda_state.primary_codec ? " *primary*" : "");
    }

    kprintf("  Sample Rate:  %u Hz\n", ihda_state.sample_rate);
    kprintf("  Channels:     %u\n", ihda_state.channels);
    kprintf("  Playing:      %s\n", ihda_state.playing ? "Yes" : "No");
    kprintf("  Volume:       L=%u R=%u %s\n",
            ihda_state.volume_left, ihda_state.volume_right,
            ihda_state.muted ? "(muted)" : "");
    kprintf("  Frames:       %llu\n", ihda_state.frames_played);
    kprintf("  Underruns:    %llu\n", ihda_state.underruns);
    kprintf("  Interrupts:   %llu\n", ihda_state.interrupts);
}

void intel_hda_print_codec_info(uint8_t codec_idx) {
    if (codec_idx >= ihda_state.num_codecs) return;

    intel_hda_codec_t *codec = &ihda_state.codecs[codec_idx];

    kprintf("\n[INTEL_HDA] Codec %d: %s\n", codec->cad, codec->name);
    kprintf("  Vendor:       0x%04x\n", codec->vendor_id);
    kprintf("  Device:       0x%04x\n", codec->device_id);
    kprintf("  Revision:     0x%04x\n", codec->revision);
    kprintf("  AFG NID:      %d\n", codec->afg_nid);
    kprintf("  Widgets:      %d-%d (%d total)\n",
            codec->start_nid, codec->start_nid + codec->num_widgets - 1, codec->num_widgets);
    kprintf("  Output Paths: %d\n", codec->num_output_paths);

    for (int i = 0; i < codec->num_output_paths; i++) {
        intel_hda_path_t *path = &codec->output_paths[i];
        kprintf("    [%d] DAC %d -> PIN %d (%s)\n",
                i, path->dac_nid, path->pin_nid,
                path->is_speaker ? "speaker" :
                path->is_headphone ? "headphone" : "line out");
    }

    kprintf("  Active DAC:   %d\n", codec->active_dac);
    kprintf("  Active PIN:   %d\n", codec->active_pin);
}

void intel_hda_print_widgets(uint8_t codec_idx) {
    if (codec_idx >= ihda_state.num_codecs) return;

    intel_hda_codec_t *codec = &ihda_state.codecs[codec_idx];

    kprintf("\n[INTEL_HDA] Codec %d Widgets:\n", codec->cad);

    const char *type_names[] = {
        "DAC", "ADC", "Mixer", "Selector", "Pin", "Power", "Volume", "Beep"
    };

    for (int i = 0; i < codec->num_widgets; i++) {
        intel_hda_widget_t *w = &codec->widgets[i];
        const char *type_name = w->type < 8 ? type_names[w->type] : "Unknown";

        kprintf("  NID %2d: %-8s conn=%d", w->nid, type_name, w->conn_count);

        if (w->type == HDA_WIDGET_PIN) {
            uint32_t conn = (w->config_default >> 30) & 0x03;
            uint32_t dev = (w->config_default >> 20) & 0x0F;

            const char *conn_str = conn == 0 ? "jack" : conn == 1 ? "none" :
                                   conn == 2 ? "fixed" : "both";
            kprintf(" [%s]", conn_str);

            if (dev == 0) kprintf(" line-out");
            else if (dev == 1) kprintf(" speaker");
            else if (dev == 2) kprintf(" hp");
            else if (dev == 8) kprintf(" line-in");
            else if (dev == 0xA) kprintf(" mic");
        }

        kprintf("\n");
    }
}
