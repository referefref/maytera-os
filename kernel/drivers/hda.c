// hda.c - Intel High Definition Audio Controller Driver
//
// Intel HDA is the modern standard for PC audio. It's more complex than AC97
// but provides better quality and more features.

#include "hda.h"
#include "pci.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../cpu/idt.h"
#include "../cpu/apic.h"
#include "../sync/spinlock.h"
#include "../sync/waitq.h"      // #426: hda_space_wq() DAC-space wait queue
#include "../proc/process.h"
#include "../gui/syslog.h"

// ============================================================================
// Driver State
// ============================================================================

static hda_state_t hda_state = {0};

// DMA buffer configuration
#define HDA_DMA_BUFFER_SIZE    (128 * 1024)    // 128KB total
#define HDA_BDL_BUFFER_SIZE    (HDA_DMA_BUFFER_SIZE / HDA_NUM_BDL)

// CORB/RIRB sizes
#define HDA_CORB_SIZE          256
#define HDA_RIRB_SIZE          256

// 64-entry signed sine table (amplitude ~ +/-8192) for the #71 audible test tone.
static const int16_t hda_sine64[64] = {
         0,    803,   1598,   2378,   3135,   3862,   4551,   5197,
      5793,   6333,   6811,   7225,   7568,   7839,   8035,   8153,
      8192,   8153,   8035,   7839,   7568,   7225,   6811,   6333,
      5793,   5197,   4551,   3862,   3135,   2378,   1598,    803,
         0,   -803,  -1598,  -2378,  -3135,  -3862,  -4551,  -5197,
     -5793,  -6333,  -6811,  -7225,  -7568,  -7839,  -8035,  -8153,
     -8192,  -8153,  -8035,  -7839,  -7568,  -7225,  -6811,  -6333,
     -5793,  -5197,  -4551,  -3862,  -3135,  -2378,  -1598,   -803,
};

// ============================================================================
// MMIO Access Helpers
// ============================================================================

static inline uint8_t hda_read8(uint32_t offset) {
    return hda_state.mmio[offset];
}

static inline uint16_t hda_read16(uint32_t offset) {
    return *(volatile uint16_t *)(hda_state.mmio + offset);
}

static inline uint32_t hda_read32(uint32_t offset) {
    return *(volatile uint32_t *)(hda_state.mmio + offset);
}

static inline void hda_write8(uint32_t offset, uint8_t value) {
    hda_state.mmio[offset] = value;
}

static inline void hda_write16(uint32_t offset, uint16_t value) {
    *(volatile uint16_t *)(hda_state.mmio + offset) = value;
}

static inline void hda_write32(uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(hda_state.mmio + offset) = value;
}

// Stream descriptor access
static inline uint32_t hda_stream_offset(uint8_t stream_idx) {
    return HDA_REG_SD_BASE + (stream_idx * HDA_REG_SD_SIZE);
}

static inline uint32_t hda_sd_read32(uint8_t stream, uint32_t reg) {
    return hda_read32(hda_stream_offset(stream) + reg);
}

static inline void hda_sd_write32(uint8_t stream, uint32_t reg, uint32_t value) {
    hda_write32(hda_stream_offset(stream) + reg, value);
}

static inline uint16_t hda_sd_read16(uint8_t stream, uint32_t reg) {
    return hda_read16(hda_stream_offset(stream) + reg);
}

static inline void hda_sd_write16(uint8_t stream, uint32_t reg, uint16_t value) {
    hda_write16(hda_stream_offset(stream) + reg, value);
}

static inline uint8_t hda_sd_read8(uint8_t stream, uint32_t reg) {
    return hda_read8(hda_stream_offset(stream) + reg);
}

static inline void hda_sd_write8(uint8_t stream, uint32_t reg, uint8_t value) {
    hda_write8(hda_stream_offset(stream) + reg, value);
}

// Delay
static void hda_delay(uint32_t us) {
    for (volatile uint32_t i = 0; i < us * 10; i++) {
        io_wait();
    }
}

// ============================================================================
// Controller Operations
// ============================================================================

static int hda_reset_controller(void) {
    // Clear CRST bit to enter reset
    uint32_t gctl = hda_read32(HDA_REG_GCTL);
    gctl &= ~HDA_GCTL_CRST;
    hda_write32(HDA_REG_GCTL, gctl);

    // Wait for reset
    for (int i = 0; i < 100; i++) {
        if ((hda_read32(HDA_REG_GCTL) & HDA_GCTL_CRST) == 0) {
            break;
        }
        hda_delay(100);
    }

    // Exit reset
    gctl |= HDA_GCTL_CRST;
    hda_write32(HDA_REG_GCTL, gctl);

    // Wait for controller to be ready
    for (int i = 0; i < 100; i++) {
        if (hda_read32(HDA_REG_GCTL) & HDA_GCTL_CRST) {
            hda_delay(1000);
            return AUDIO_OK;
        }
        hda_delay(100);
    }

    kprintf("[HDA] Controller reset failed\n");
    return AUDIO_ERR_TIMEOUT;
}

// ============================================================================
// CORB/RIRB Operations
// ============================================================================

static int hda_setup_corb_rirb(void) {
    // Allocate CORB/RIRB once; reuse across controllers during multi-controller
    // probing (each controller reprograms its own base-address registers below).
    if (!hda_state.corb) {
        // Allocate CORB (256 entries * 4 bytes = 1KB, 128-byte aligned)
        hda_state.corb = (hda_corb_entry_t *)kzalloc_aligned(HDA_CORB_SIZE * 4, 128);
        if (!hda_state.corb) {
            return AUDIO_ERR_NO_MEMORY;
        }
    }
    hda_state.corb_phys = (uint64_t)(uintptr_t)hda_state.corb;

    if (!hda_state.rirb) {
        // Allocate RIRB (256 entries * 8 bytes = 2KB, 128-byte aligned)
        hda_state.rirb = (hda_rirb_entry_t *)kzalloc_aligned(HDA_RIRB_SIZE * 8, 128);
        if (!hda_state.rirb) {
            return AUDIO_ERR_NO_MEMORY;
        }
    }
    hda_state.rirb_phys = (uint64_t)(uintptr_t)hda_state.rirb;

    // Stop CORB
    hda_write8(HDA_REG_CORBCTL, 0);
    for (int i = 0; i < 100 && (hda_read8(HDA_REG_CORBCTL) & HDA_CORBCTL_RUN); i++) {
        hda_delay(100);
    }

    // Stop RIRB
    hda_write8(HDA_REG_RIRBCTL, 0);
    for (int i = 0; i < 100 && (hda_read8(HDA_REG_RIRBCTL) & HDA_RIRBCTL_RUN); i++) {
        hda_delay(100);
    }

    // Set CORB base address
    hda_write32(HDA_REG_CORBLBASE, (uint32_t)hda_state.corb_phys);
    hda_write32(HDA_REG_CORBUBASE, (uint32_t)(hda_state.corb_phys >> 32));

    // Set RIRB base address
    hda_write32(HDA_REG_RIRBLBASE, (uint32_t)hda_state.rirb_phys);
    hda_write32(HDA_REG_RIRBUBASE, (uint32_t)(hda_state.rirb_phys >> 32));

    // Set CORB size to 256 entries (size code 2)
    uint8_t corbsize = hda_read8(HDA_REG_CORBSIZE);
    corbsize = (corbsize & 0xFC) | 0x02;
    hda_write8(HDA_REG_CORBSIZE, corbsize);

    // Set RIRB size to 256 entries
    uint8_t rirbsize = hda_read8(HDA_REG_RIRBSIZE);
    rirbsize = (rirbsize & 0xFC) | 0x02;
    hda_write8(HDA_REG_RIRBSIZE, rirbsize);

    // Reset CORB read pointer
    hda_write16(HDA_REG_CORBRP, 0x8000);
    for (int i = 0; i < 100; i++) {
        if (hda_read16(HDA_REG_CORBRP) & 0x8000) break;
        hda_delay(100);
    }
    hda_write16(HDA_REG_CORBRP, 0);
    for (int i = 0; i < 100; i++) {
        if ((hda_read16(HDA_REG_CORBRP) & 0x8000) == 0) break;
        hda_delay(100);
    }

    // Reset CORB write pointer
    hda_write16(HDA_REG_CORBWP, 0);
    hda_state.corb_wp = 0;

    // Reset RIRB write pointer
    hda_write16(HDA_REG_RIRBWP, 0x8000);
    hda_state.rirb_rp = 0;

    // Response interrupt count. QEMU halts the CORB DMA engine once rirb_count
    // reaches RINTCNT and waits for the guest to ACK RIRBSTS. Use a high value
    // and also clear RIRBSTS after every response (below) so commands flow.
    hda_write16(HDA_REG_RINTCNT, 0xFF);

    // Start CORB and RIRB DMA engines (QEMU only supports the DMA path).
    hda_write8(HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    hda_write8(HDA_REG_RIRBCTL, HDA_RIRBCTL_RUN);

    hda_delay(100);
    kprintf("[HDA] CORB/RIRB setup complete\n");
    return AUDIO_OK;
}

// #71: bounded-diagnostic knobs for the codec-command busy-wait. hda_delay() is
// a NON-yielding spin, and hda_codec_command() spins up to g_hda_cmd_max_iters *
// hda_delay(10) (~200ms at the default 2000) per timed-out command. The full
// widget-graph scan (hda_devlog_scan/hda_audiolog_report) issues that against
// every node; on the real Cirrus CS4208 where verbs time out, hundreds of such
// spins on the init path add up to tens of seconds of BKL-held busy-wait, i.e. a
// freeze (this is what wedged the iMac on b730/b733 - NOT an infinite loop).
//
// Normal boots leave these at their defaults (g_hda_diag_active == 0), so audio
// init is UNCHANGED. The gated AUDIOLOG dump arms them (see hda_audiolog_report)
// so a diagnostic boot self-limits: a short per-command timeout plus a hard cap
// on the number of timed-out commands bounds the whole scan to ~1-2s even if the
// codec is completely silent.
static volatile int g_hda_cmd_max_iters = 2000;  // per-command spin cap
static volatile int g_hda_diag_active   = 0;     // 1 while a bounded scan runs
static volatile int g_hda_diag_budget   = 0;     // remaining allowed timeouts

// Send a codec command via CORB and read the response from RIRB (the DMA path
// that QEMU supports; the immediate interface returns 0 under QEMU).
static uint32_t hda_codec_command(uint8_t cad, uint8_t nid, uint32_t verb) {
    uint32_t cmd = ((uint32_t)cad << 28) | ((uint32_t)nid << 20) | verb;

    // During a bounded diagnostic scan, once the timeout budget is spent, stop
    // spinning entirely: fast-fail every further command so a silent codec can
    // never turn the scan into a multi-second freeze.
    if (g_hda_diag_active && g_hda_diag_budget <= 0) return 0xFFFFFFFF;

    uint16_t wp = (hda_state.corb_wp + 1) % HDA_CORB_SIZE;
    hda_state.corb[wp] = cmd;
    hda_state.corb_wp = wp;
    hda_write16(HDA_REG_CORBWP, wp);

    int max_iters = g_hda_cmd_max_iters;
    for (int i = 0; i < max_iters; i++) {
        uint16_t rirb_wp = hda_read16(HDA_REG_RIRBWP);
        if (rirb_wp != hda_state.rirb_rp) {
            hda_state.rirb_rp = (hda_state.rirb_rp + 1) % HDA_RIRB_SIZE;
            uint32_t resp = hda_state.rirb[hda_state.rirb_rp].response;
            hda_write8(HDA_REG_RIRBSTS, 0x05);  // clear RINTFL/OIS, reset count
            return resp;
        }
        hda_delay(10);
    }

    if (g_hda_diag_active && g_hda_diag_budget > 0) g_hda_diag_budget--;
    kprintf("[HDA] codec command timeout (cmd=0x%08x)\n", cmd);
    return 0xFFFFFFFF;
}

// ============================================================================
// Codec Discovery
// ============================================================================

// ============================================================================
// #71 Generic codec widget-graph auto-parser (modeled on Linux snd-hda auto).
// ============================================================================

// Read a widget's connection list into out[] (bounded). Returns count.
static int hda_get_connections(uint8_t cad, uint8_t nid, uint8_t *out, int max) {
    uint32_t p = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_CONN_LIST_LEN);
    if (p == 0xFFFFFFFF) return 0;
    int len = p & 0x7F;
    int longform = (p & 0x80) != 0;
    int count = 0, prev = -1;
    for (int i = 0; i < len && count < max; ) {
        uint32_t resp = hda_codec_command(cad, nid, HDA_VERB_GET_CONN_LIST | (i & 0xFF));
        int per = longform ? 2 : 4;
        for (int k = 0; k < per && i < len && count < max; k++, i++) {
            uint32_t ent = longform ? ((resp >> (k * 16)) & 0xFFFF)
                                    : ((resp >> (k * 8)) & 0xFF);
            int range = longform ? (ent & 0x8000) : (ent & 0x80);
            int val   = longform ? (ent & 0x7FFF) : (ent & 0x7F);
            if (range && prev >= 0) {
                for (int v = prev + 1; v <= val && count < max; v++) out[count++] = (uint8_t)v;
            } else {
                out[count++] = (uint8_t)val;
            }
            prev = val;
        }
    }
    return count;
}

static inline uint32_t hda_widget_type(uint8_t cad, uint8_t nid) {
    uint32_t wcap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AUDIO_WIDGET_CAP);
    return wcap & HDA_WIDGET_TYPE_MASK;
}

// Score an output pin from its config-default. Higher = more preferred. Returns
// -1 if the pin is not output-capable at all.
static int hda_pin_output_score(uint8_t cad, uint8_t nid, uint8_t *out_dev, uint8_t *out_analog) {
    uint32_t pincap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_PIN_CAP);
    if (pincap == 0xFFFFFFFF) return -1;
    if (!(pincap & 0x10)) return -1;                  // bit 4 = Output Capable
    uint32_t cfg = hda_codec_command(cad, nid, HDA_VERB_GET_CONFIG_DEF);
    uint8_t conn = (cfg >> 30) & 0x3;                 // 0 jack,1 none,2 fixed,3 both
    uint8_t dev  = (cfg >> 20) & 0xF;                 // default device
    *out_dev = dev;
    int score;
    switch (dev) {
        case 0x1: score = 100; *out_analog = 1; break; // Speaker
        case 0x0: score = 90;  *out_analog = 1; break; // Line Out
        case 0x2: score = 80;  *out_analog = 1; break; // HP Out
        case 0x4: score = 45;  *out_analog = 0; break; // SPDIF Out
        case 0x5: score = 40;  *out_analog = 0; break; // Digital Other Out
        default:  score = 20;  *out_analog = 0; break; // other output-capable pin
    }
    if (conn == 1) score -= 50;                       // "no physical connection": last resort
    return score;
}

// Resolve the path from an output pin back to a DAC (direct, or through one
// selector/mixer). On success fills *dac/*pin_conn/*mix_nid/*mix_conn/*mix_is_sel
// and returns 1; returns 0 if no reachable DAC.
static int hda_resolve_pin_route(uint8_t cad, uint8_t pin,
                                 uint8_t *dac, uint8_t *pin_conn,
                                 uint8_t *mix_nid, uint8_t *mix_conn,
                                 uint8_t *mix_is_sel) {
    uint8_t conns[16];
    int n = hda_get_connections(cad, pin, conns, 16);
    for (int i = 0; i < n; i++) {
        if (hda_widget_type(cad, conns[i]) == HDA_WIDGET_TYPE_OUTPUT) {
            *dac = conns[i]; *pin_conn = (uint8_t)i;
            *mix_nid = 0; *mix_conn = 0; *mix_is_sel = 0;
            return 1;
        }
    }
    for (int i = 0; i < n; i++) {
        uint32_t t = hda_widget_type(cad, conns[i]);
        if (t != HDA_WIDGET_TYPE_MIXER && t != HDA_WIDGET_TYPE_SELECTOR) continue;
        uint8_t sub[16];
        int m = hda_get_connections(cad, conns[i], sub, 16);
        for (int j = 0; j < m; j++) {
            if (hda_widget_type(cad, sub[j]) == HDA_WIDGET_TYPE_OUTPUT) {
                *dac = sub[j]; *pin_conn = (uint8_t)i;
                *mix_nid = conns[i]; *mix_conn = (uint8_t)j;
                *mix_is_sel = (t == HDA_WIDGET_TYPE_SELECTOR) ? 1 : 0;
                return 1;
            }
        }
    }
    return 0;
}

// Record a resolved route into c->route_*[idx] and mirror route 0 into the legacy
// single-route fields (used by the DMA stream/format setup elsewhere).
static void hda_store_route(hda_codec_t *c, int idx, uint8_t pin, uint8_t dev,
                            uint8_t dac, uint8_t pin_conn, uint8_t mix_nid,
                            uint8_t mix_conn, uint8_t mix_is_sel) {
    c->route_pin[idx]    = pin;
    c->route_dev[idx]    = dev;
    c->route_dac[idx]    = dac;
    c->route_pinc[idx]   = pin_conn;
    c->route_mixn[idx]   = mix_nid;
    c->route_mixc_[idx]  = mix_conn;
    c->route_mixsel[idx] = mix_is_sel;
    if (idx == 0) {
        c->dac_nid        = dac;
        c->out_pin_nid    = pin;
        c->default_device = dev;
        c->route_pin_conn = pin_conn;
        c->route_mix_nid  = mix_nid;
        c->route_mix_conn = mix_conn;
        c->route_mix_is_sel = mix_is_sel;
    }
}

// Parse one codec: fill *c with vendor/FG and its output route(s). Returns the
// route score, or -1 if no usable output path exists on this codec.
//
// #390 CS4208 stereo: Apple's Cirrus codec exposes MANY "Line-Out" pins whose
// config-default connectivity is "no physical connection" (0x400000f0) which are
// dead, PLUS two real fixed-function speaker pins (nid 29 cfg 0x90100110 and
// nid 30 cfg 0x90100112, device=Speaker connectivity=Fixed) each fed by its own
// mono DAC (nid 10 and nid 11). We must drive BOTH fixed speakers for stereo and
// must NOT be fooled into picking a dead conn=None pin. So we first collect every
// Speaker(device=0x1) + Fixed(connectivity=0x2) pin as a parallel output route;
// if any exist we use them exclusively. Otherwise we fall back to the single
// best-scoring pin (QEMU line-out, HP-only laptops), unchanged from before.
static int hda_parse_codec(uint8_t cad, hda_codec_t *c) {
    memset(c, 0, sizeof(*c));
    c->cad = cad;
    c->route_score = -1;

    uint32_t vendor = hda_codec_command(cad, 0, HDA_VERB_GET_PARAM | HDA_PARAM_VENDOR_ID);
    if (vendor == 0 || vendor == 0xFFFFFFFF) return -1;   // no VendorID: skip this codec
    c->vendor_id = (vendor >> 16) & 0xFFFF;
    c->device_id = vendor & 0xFFFF;

    uint32_t nc = hda_codec_command(cad, 0, HDA_VERB_GET_PARAM | HDA_PARAM_NODE_COUNT);
    uint8_t fg_start = (nc >> 16) & 0xFF;
    uint8_t fg_count = nc & 0xFF;

    // Locate the audio function group.
    uint8_t afg = 0;
    for (int i = 0; i < fg_count && i < 8; i++) {
        uint8_t fgn = fg_start + i;
        uint32_t fgt = hda_codec_command(cad, fgn, HDA_VERB_GET_PARAM | HDA_PARAM_FG_TYPE);
        if ((fgt & 0x7F) == 0x01) { afg = fgn; break; }
    }
    if (afg == 0) afg = fg_start;
    c->fg_nid = afg;

    uint32_t wnc = hda_codec_command(cad, afg, HDA_VERB_GET_PARAM | HDA_PARAM_NODE_COUNT);
    c->start_nid = (wnc >> 16) & 0xFF;
    c->num_nodes = wnc & 0xFF;
    if (c->num_nodes == 0 || c->num_nodes > 128) return -1;

    // Pass 1: collect fixed-function SPEAKER pins (device=Speaker, connectivity=
    // Fixed) and route each to its DAC. These are the real internal speakers.
    int nroutes = 0;
    for (int nid = c->start_nid;
         nid < c->start_nid + c->num_nodes && nroutes < HDA_MAX_OUT_ROUTES; nid++) {
        if (hda_widget_type(cad, nid) != HDA_WIDGET_TYPE_PIN) continue;
        uint32_t pincap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_PIN_CAP);
        if (pincap == 0xFFFFFFFF || !(pincap & 0x10)) continue;   // not output-capable
        uint32_t cfg = hda_codec_command(cad, nid, HDA_VERB_GET_CONFIG_DEF);
        uint8_t conn = (cfg >> 30) & 0x3;                         // 2 == Fixed
        uint8_t dev  = (cfg >> 20) & 0xF;                         // 1 == Speaker
        if (!(dev == 0x1 && conn == 0x2)) continue;              // only fixed speakers
        uint8_t dac, pin_conn, mix_nid, mix_conn, mix_is_sel;
        if (!hda_resolve_pin_route(cad, (uint8_t)nid, &dac, &pin_conn,
                                   &mix_nid, &mix_conn, &mix_is_sel))
            continue;                                            // no reachable DAC
        hda_store_route(c, nroutes, (uint8_t)nid, dev, dac, pin_conn,
                        mix_nid, mix_conn, mix_is_sel);
        nroutes++;
    }
    if (nroutes > 0) {
        c->num_out_routes = (uint8_t)nroutes;
        c->is_analog = 1;
        c->route_score = 100;                                    // Speaker = top choice
        return c->route_score;
    }

    // Pass 2 (fallback): no fixed speakers. Pick the single best-scoring output
    // pin (HP / line-out / digital) exactly as before, skipping conn=None dead
    // pins via the score penalty. Keeps QEMU line-out and HP-only laptops working.
    int best_pin = -1, best_score = -1;
    uint8_t best_dev = 0, best_analog = 0;
    for (int nid = c->start_nid; nid < c->start_nid + c->num_nodes; nid++) {
        if (hda_widget_type(cad, nid) != HDA_WIDGET_TYPE_PIN) continue;
        uint8_t dev = 0, analog = 0;
        int s = hda_pin_output_score(cad, nid, &dev, &analog);
        if (s > best_score) { best_score = s; best_pin = nid; best_dev = dev; best_analog = analog; }
    }
    if (best_pin < 0) return -1;

    uint8_t dac, pin_conn, mix_nid, mix_conn, mix_is_sel;
    if (!hda_resolve_pin_route(cad, (uint8_t)best_pin, &dac, &pin_conn,
                               &mix_nid, &mix_conn, &mix_is_sel))
        return -1;   // output pin found but no reachable DAC
    hda_store_route(c, 0, (uint8_t)best_pin, best_dev, dac, pin_conn,
                    mix_nid, mix_conn, mix_is_sel);
    c->num_out_routes = 1;
    c->is_analog = best_analog;
    c->route_score = best_score;
    return best_score;
}

// Set a widget's OUTPUT amp to unmuted at its calibrated 0dB gain (or max if the
// codec reports no 0dB offset). No-op if the widget has no output amp.
static void hda_set_out_amp(uint8_t cad, uint8_t nid) {
    uint32_t cap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AMP_OUT_CAP);
    if (cap == 0 || cap == 0xFFFFFFFF) return;
    uint8_t nsteps = (cap >> 8) & 0x7F;
    uint8_t offset = cap & 0x7F;
    uint8_t gain = offset ? offset : nsteps;
    if (gain > nsteps) gain = nsteps;
    hda_codec_command(cad, nid, HDA_VERB_SET_AMP_GAIN |
        HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | gain);
}

// Set a widget's INPUT amp (at connection index) to unmuted 0dB. No-op if none.
static void hda_set_in_amp(uint8_t cad, uint8_t nid, uint8_t index) {
    uint32_t cap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AMP_IN_CAP);
    if (cap == 0 || cap == 0xFFFFFFFF) return;
    uint8_t nsteps = (cap >> 8) & 0x7F;
    uint8_t offset = cap & 0x7F;
    uint8_t gain = offset ? offset : nsteps;
    if (gain > nsteps) gain = nsteps;
    hda_codec_command(cad, nid, HDA_VERB_SET_AMP_GAIN |
        HDA_AMP_SET_INPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
        ((index << 8) & HDA_AMP_SET_INDEX_MASK) | gain);
}

// Program the codec verbs along every parsed output route: power, stream
// tag/channel, amps, connection-select, pin output-enable + EAPD. Codec-agnostic.
//
// #390 CS4208 stereo: when the parser found more than one route (the two fixed
// speaker pins on the Apple Cirrus codec), each route's DAC is driven from the
// SAME output stream tag but assigned a different stereo CHANNEL (route 0 -> ch 0
// = left, route 1 -> ch 1 = right), so DAC10->Pin29 plays left and DAC11->Pin30
// plays right. A single-route codec is identical to the old behaviour (one DAC,
// channel 0).
static void hda_configure_codec(void) {
    hda_codec_t *c = &hda_state.codec;
    uint8_t cad = c->cad;
    int nroutes = c->num_out_routes ? c->num_out_routes : 1;

    // Power up the AFG once (D0).
    hda_codec_command(cad, c->fg_nid, HDA_VERB_SET_PS | 0x00);
    hda_delay(10000);

    for (int r = 0; r < nroutes; r++) {
        uint8_t dac = c->route_dac[r];
        uint8_t pin = c->route_pin[r];
        uint8_t mix = c->route_mixn[r];
        uint8_t dev = c->route_dev[r];

        // Power the widgets on this path (D0).
        hda_codec_command(cad, dac, HDA_VERB_SET_PS | 0x00);
        hda_codec_command(cad, pin, HDA_VERB_SET_PS | 0x00);
        if (mix) hda_codec_command(cad, mix, HDA_VERB_SET_PS | 0x00);
        hda_delay(1000);

        // DAC: same stream tag (1), one stereo channel per route. Unmute amp.
        // (Converter format is set on every route DAC in hda_init.)
        hda_codec_command(cad, dac, HDA_VERB_SET_STREAM | (1 << 4) | (r & 0x0F));
        hda_set_out_amp(cad, dac);

        // Intermediate mixer/selector: route to the DAC and unmute.
        if (mix) {
            if (c->route_mixsel[r])
                hda_codec_command(cad, mix, HDA_VERB_SET_CONN_SELECT | c->route_mixc_[r]);
            hda_set_in_amp(cad, mix, c->route_mixc_[r]);
            hda_set_out_amp(cad, mix);
        }

        // Pin: select path input, enable output + EAPD, unmute.
        hda_codec_command(cad, pin, HDA_VERB_SET_CONN_SELECT | c->route_pinc[r]);
        {
            uint8_t pinctl = HDA_PIN_OUT_EN;                     // bit 6 output-enable
            if (dev == 0x2) pinctl |= 0x80;                     // HP amp enable
            hda_codec_command(cad, pin, HDA_VERB_SET_PIN_CTL | pinctl);
        }
        hda_codec_command(cad, pin, HDA_VERB_SET_EAPD | 0x02);  // EAPD external amp on
        hda_set_out_amp(cad, pin);

        kprintf("[HDA] Route %d: cad=%d DAC=%d %s=%d PIN=%d ch=%d (dev=0x%x)\n",
                r, cad, dac, mix ? "MIX" : "direct", mix, pin, r, dev);
    }

    // #71 Cirrus CS4208 (Apple iMac14,4 internal speakers): the EAPD verb on the
    // speaker pins (sent per-route above) is not sufficient on the Apple codec.
    // Its internal-speaker power amplifier is gated by a codec GPIO. Mirroring
    // Linux sound/pci/hda/patch_cirrus.c cs4208_fixup_gpio0() (gpio_eapd_speaker
    // = bit0, gpio_mask = gpio_dir = data = 0x01), drive GPIO0 high on the AFG
    // root node to power the speaker amp. This is what QEMU's emulated codec does
    // NOT need, so it is gated on the Cirrus vendor ID to avoid disturbing the
    // known-good QEMU/other-vendor paths. The current mask/dir/data are logged to
    // /AUDIOLOG.TXT so the effect is verifiable over SSH on the real machine.
    if (c->vendor_id == HDA_VENDOR_CIRRUS) {
        uint32_t gpc = hda_codec_command(cad, c->fg_nid,
                                         HDA_VERB_GET_PARAM | HDA_PARAM_GPIO_COUNT);
        // Enable GPIO0 as an output driven high (speaker-amp power).
        hda_codec_command(cad, c->fg_nid, HDA_VERB_SET_GPIO_MASK | 0x01);
        hda_codec_command(cad, c->fg_nid, HDA_VERB_SET_GPIO_DIR  | 0x01);
        hda_codec_command(cad, c->fg_nid, HDA_VERB_SET_GPIO_DATA | 0x01);
        hda_delay(1000);
        kprintf("[HDA] Cirrus %04x:%04x: GPIO0 speaker-amp power enabled "
                "(GPIO_COUNT=0x%08x, mask/dir/data=0x01)\n",
                c->vendor_id, c->device_id, gpc);
    }

    kprintf("[HDA] Configured %d output route(s) (%s score=%d)\n",
            nroutes, c->is_analog ? "analog" : "digital", c->route_score);
}

// ============================================================================
// DMA Buffer Setup
// ============================================================================

static int hda_setup_dma_buffers(void) {
    hda_state.bdl = (hda_bdl_entry_t *)kzalloc_aligned(HDA_NUM_BDL * sizeof(hda_bdl_entry_t), 128);
    if (!hda_state.bdl) {
        return AUDIO_ERR_NO_MEMORY;
    }
    hda_state.bdl_phys = (uint64_t)(uintptr_t)hda_state.bdl;

    hda_state.dma_buffer_size = HDA_DMA_BUFFER_SIZE;
    hda_state.bdl_buffer_size = HDA_BDL_BUFFER_SIZE;
    hda_state.dma_buffer = kzalloc_aligned(hda_state.dma_buffer_size, PAGE_SIZE);
    if (!hda_state.dma_buffer) {
        kfree(hda_state.bdl);
        return AUDIO_ERR_NO_MEMORY;
    }
    hda_state.dma_buffer_phys = (uint64_t)(uintptr_t)hda_state.dma_buffer;

    uint64_t buffer_addr = hda_state.dma_buffer_phys;
    for (int i = 0; i < HDA_NUM_BDL; i++) {
        hda_state.bdl[i].addr = buffer_addr + (i * hda_state.bdl_buffer_size);
        hda_state.bdl[i].length = hda_state.bdl_buffer_size;
        hda_state.bdl[i].ioc = HDA_BDL_IOC;
    }

    kprintf("[HDA] DMA buffers allocated: BDL@0x%llx, Buffer@0x%llx\n",
            hda_state.bdl_phys, hda_state.dma_buffer_phys);

    return AUDIO_OK;
}

static void hda_free_dma_buffers(void) {
    if (hda_state.bdl) {
        kfree(hda_state.bdl);
        hda_state.bdl = NULL;
    }
    if (hda_state.dma_buffer) {
        kfree(hda_state.dma_buffer);
        hda_state.dma_buffer = NULL;
    }
    if (hda_state.corb) {
        kfree(hda_state.corb);
        hda_state.corb = NULL;
    }
    if (hda_state.rirb) {
        kfree(hda_state.rirb);
        hda_state.rirb = NULL;
    }
}

// ============================================================================
// Stream Configuration
// ============================================================================

static uint16_t hda_calculate_format(uint32_t sample_rate, uint32_t channels, uint32_t bits) {
    uint16_t fmt = 0;

    fmt |= (channels - 1) & 0xF;

    switch (bits) {
        case 8:  fmt |= HDA_FMT_BITS_8; break;
        case 16: fmt |= HDA_FMT_BITS_16; break;
        case 20: fmt |= HDA_FMT_BITS_20; break;
        case 24: fmt |= HDA_FMT_BITS_24; break;
        case 32: fmt |= HDA_FMT_BITS_32; break;
        default: fmt |= HDA_FMT_BITS_16; break;
    }

    switch (sample_rate) {
        case 48000: fmt |= (0 << 11) | (0 << 8); break;
        case 44100: fmt |= HDA_FMT_BASE | (0 << 11) | (0 << 8); break;
        case 96000: fmt |= (1 << 11) | (0 << 8); break;
        case 192000: fmt |= (3 << 11) | (0 << 8); break;
        case 88200: fmt |= HDA_FMT_BASE | (1 << 11) | (0 << 8); break;
        case 32000: fmt |= (0 << 11) | (2 << 8); break;
        case 22050: fmt |= HDA_FMT_BASE | (0 << 11) | (1 << 8); break;
        case 16000: fmt |= (0 << 11) | (2 << 8); break;
        case 11025: fmt |= HDA_FMT_BASE | (0 << 11) | (3 << 8); break;
        case 8000: fmt |= (0 << 11) | (5 << 8); break;
        default: fmt |= (0 << 11) | (0 << 8); break;
    }

    return fmt;
}

static int hda_configure_output_stream(void) {
    uint8_t stream = hda_state.out_stream_idx;

    uint32_t ctl = hda_sd_read32(stream, HDA_SD_CTL);
    ctl &= ~HDA_SD_CTL_RUN;
    hda_sd_write32(stream, HDA_SD_CTL, ctl);

    for (int i = 0; i < 100; i++) {
        if ((hda_sd_read32(stream, HDA_SD_CTL) & HDA_SD_CTL_RUN) == 0) break;
        hda_delay(100);
    }

    ctl |= HDA_SD_CTL_SRST;
    hda_sd_write32(stream, HDA_SD_CTL, ctl);
    hda_delay(100);

    for (int i = 0; i < 100; i++) {
        if (hda_sd_read32(stream, HDA_SD_CTL) & HDA_SD_CTL_SRST) break;
        hda_delay(100);
    }

    ctl &= ~HDA_SD_CTL_SRST;
    hda_sd_write32(stream, HDA_SD_CTL, ctl);

    for (int i = 0; i < 100; i++) {
        if ((hda_sd_read32(stream, HDA_SD_CTL) & HDA_SD_CTL_SRST) == 0) break;
        hda_delay(100);
    }

    hda_sd_write8(stream, HDA_SD_STS, HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE);
    hda_sd_write16(stream, HDA_SD_FMT, hda_state.stream_format);
    hda_sd_write32(stream, HDA_SD_CBL, hda_state.dma_buffer_size);
    hda_sd_write16(stream, HDA_SD_LVI, HDA_NUM_BDL - 1);
    hda_sd_write32(stream, HDA_SD_BDPL, (uint32_t)hda_state.bdl_phys);
    hda_sd_write32(stream, HDA_SD_BDPU, (uint32_t)(hda_state.bdl_phys >> 32));

    ctl = HDA_SD_CTL_IOCE | ((1 << 20) & HDA_SD_CTL_STRM_MASK);
    hda_sd_write32(stream, HDA_SD_CTL, ctl);

    kprintf("[HDA] Output stream %d configured, format=0x%04x\n",
            stream, hda_state.stream_format);

    return AUDIO_OK;
}

// ============================================================================
// #71: IRQ handler / LPIB-poll worker
//
// The iMac's HDA controller reports PCI_INTERRUPT_LINE=0 (no legacy INTx
// routing at all), so the DMA-completion interrupt this ISR services never
// fired -- BCIS/FIFOE status bits piled up unacknowledged and were never
// turned into read_index/underrun bookkeeping. Output DMA itself does not
// need the interrupt (it is a free-running hardware ring once SD_CTL_RUN is
// set, and hda_write()/hda_avail() already recompute directly off the live
// LPIB register rather than off read_index), but leaving the status register
// unserviced forever is still wrong, and some future driver logic may come to
// depend on read_index. Two independent, redundant paths now service the
// stream, both funneling through the same idempotent core:
//   (a) hda_msi_isr(), the real interrupt, if hda_setup_interrupt() managed to
//       arm MSI for the winning controller (works regardless of the dead
//       legacy IRQ line, since MSI targets the Local APIC directly).
//   (b) hda_poll_worker(), a low-priority kernel worker that services the
//       stream on a timer via proc_sleep() -- never a busy-spin (#426) -- so
//       the driver behaves identically whether or not (a) ever fires. This is
//       the real fix for the iMac; (a) is best-effort on top of it.
// hda_service_stream() is safe to call from both without double-counting:
// the status bits it acts on are RW1C (write-1-to-clear) in hardware, so
// whichever caller observes a bit set clears it and the other caller (racing
// or on its own later pass) simply sees it already clear. g_hda_svc_lock only
// protects the read-act-clear sequence itself (interrupt context vs. the poll
// thread), not cross-caller ordering, which doesn't need protecting.
// ============================================================================

static spinlock_t g_hda_svc_lock = SPINLOCK_INIT;

// #426: DAC-space wait queue. Statically initialised (no init call, hence no
// init-ordering race: hda_init() runs long before proc_init()).
//
// This is the event source that lets a writer BLOCK for DAC space instead of
// polling hda_avail(). It is woken from hda_service_stream(), which is reached
// by BOTH redundant service paths already described above:
//   (a) hda_msi_isr()      - the real BCIS buffer-completion interrupt
//   (b) hda_poll_worker()  - the pre-existing 10 ms service worker
// so a waiter is guaranteed a wake at >= 100 Hz even on a controller where MSI
// never arms (the real iMac Cirrus CS4208 reports IRQ=0). That redundancy is
// what makes blocking here safe: a lost BCIS cannot strand a waiter for more
// than one 10 ms service pass, and every waiter re-checks its own condition.
// First consumer: drivers/audio_pcm.c's Ring-3 PCM pump.
static wait_queue_head_t g_hda_space_wq = { .head = NULL, .lock = SPINLOCK_INIT };

wait_queue_head_t *hda_space_wq(void) {
    return &g_hda_space_wq;
}

static void hda_service_stream(void) {
    if (!hda_state.initialized) return;

    uint64_t fl = spinlock_acquire_irqsave(&g_hda_svc_lock);

    uint32_t intsts = hda_read32(HDA_REG_INTSTS);
    if (intsts & HDA_INTSTS_GIS) {
        uint8_t stream = hda_state.out_stream_idx;
        uint8_t sd_sts = hda_sd_read8(stream, HDA_SD_STS);

        if (sd_sts & HDA_SD_STS_BCIS) {
            hda_state.read_index = (hda_state.read_index + 1) % HDA_NUM_BDL;
        }
        if (sd_sts & HDA_SD_STS_FIFOE) {
            hda_state.underruns++;
        }
        if (sd_sts) {
            hda_sd_write8(stream, HDA_SD_STS, sd_sts);   // RW1C: only clears set bits
        }
    }

    spinlock_release_irqrestore(&g_hda_svc_lock, fl);

    // #426: wake DAC-space waiters, AFTER dropping g_hda_svc_lock (wake_up_all
    // takes the wait queue's own lock; nesting the two would invent a lock
    // order for no reason). Woken unconditionally rather than only on BCIS:
    // this is called from the 10 ms poll worker as well as the ISR, so an
    // unconditional wake makes the wait self-healing (no lost wakeup can outlive
    // one service pass) and costs a lock + NULL check when nobody is waiting,
    // which is the overwhelmingly common case. Every waiter re-tests its own
    // condition, so a spurious wake is harmless by construction.
    // wake_up_all() is documented safe from IRQ context (sync/waitq.h).
    wake_up_all(&g_hda_space_wq);
}

// Public IRQ handler, kept for API compatibility with anything that still
// calls it directly (e.g. a legacy INTx path, if one is ever wired).
void hda_irq_handler(void) {
    hda_service_stream();
}

// Real MSI interrupt entry point (see cpu/idt.asm irq_hda_msi / HDA_MSI_VECTOR).
// MSI is Local-APIC-delivered, not routed through the 8259 PIC, so the EOI
// here is lapic_eoi(), not pic_send_eoi().
static void hda_msi_isr(interrupt_frame_t *frame) {
    (void)frame;
    hda_service_stream();
    lapic_eoi();
}

// #71 robust fix: service the stream on a timer regardless of interrupt state.
// Uses proc_sleep() (the existing cooperative-yield primitive used throughout
// audio.c), never a busy spin. 10ms keeps this comfortably ahead of anything
// that could accumulate between services while costing nothing when idle (a
// couple of MMIO reads per pass).
#define HDA_POLL_MS 10
static void hda_poll_worker(void *arg) {
    (void)arg;
    for (;;) {
        hda_service_stream();
        proc_sleep(HDA_POLL_MS);
    }
}

static int g_hda_poll_started = 0;
static void hda_start_poll_worker(void) {
    if (g_hda_poll_started) return;
    g_hda_poll_started = 1;
    proc_create("hdapoll", hda_poll_worker, NULL, PRIO_LOW);
}

// #699: public entry point main.c calls right after proc_init(). hda_init()
// (and therefore audio_init()) runs long before proc_init() in the boot
// sequence, so the worker can no longer be started directly from hda_init()
// -- see the comment at the hda_init() call site that used to start it. This
// is a no-op if HDA never initialized (no controller/codec found), and is
// idempotent (safe even if ever called more than once) via g_hda_poll_started.
void hda_start_poll_worker_deferred(void) {
    if (hda_state.initialized) {
        hda_start_poll_worker();
    }
}

// ============================================================================
// #71 userland audio bring-up debug (SYS_HDA_DBG). Lets a Ring-3 tool drive the
// whole HDA output path over the mdev bridge WITHOUT a kernel reburn per attempt:
// send codec verbs, toggle SDnCTL RUN, read LPIB, poke the winning codec GPIO,
// fill a test tone, and raw-read/write the controller BAR. Gated on
// hda_state.initialized so it is inert if no HDA came up. Returns -1 on a bad op
// or when uninitialised; otherwise the natural result (codec response, reg value,
// etc). Physically it only touches the already-owned HDA MMIO + our own DMA
// buffer, so it cannot corrupt other subsystems.
//   op 0  CODEC_VERB   a=cad b=nid c=verb            -> codec response
//   op 1  SD_CTL                                     -> SDnCTL (out stream)
//   op 2  SD_STS                                     -> SDnSTS
//   op 3  SD_LPIB                                    -> link position (advancing = DMA runs)
//   op 4  SD_RUN       a=0 stop / 1 start            -> new SDnCTL
//   op 5  GPIO_GET     a=0 mask/1 dir/2 data         -> value (winning codec AFG)
//   op 6  GPIO_SET     a=mask b=dir c=data           -> 0
//   op 7  TONE         a=period_frames (0=silence)   -> frames written (fills DMA buf, loops on RUN)
//   op 8  REG_RD32     a=offset(<0x2000)             -> BAR value
//   op 9  REG_WR32     a=offset(<0x2000) b=value     -> 0
//   op 10 INFO         -> (out_stream_idx<<24)|(cad<<16)|(fg_nid<<8)|initialized
int64_t hda_debug_op(int op, uint64_t a, uint64_t b, uint64_t c) {
    if (!hda_state.initialized && op != 10) return -1;
    hda_codec_t *cd = &hda_state.codec;
    uint8_t sd = hda_state.out_stream_idx;

    switch (op) {
    case 0:
        return (int64_t)(uint32_t)hda_codec_command((uint8_t)a, (uint8_t)b, (uint32_t)c);
    case 1: return (int64_t)hda_sd_read32(sd, HDA_SD_CTL);
    case 2: return (int64_t)hda_sd_read8(sd, HDA_SD_STS);
    case 3: return (int64_t)hda_sd_read32(sd, HDA_SD_LPIB);
    case 4: {
        uint32_t ctl = hda_sd_read32(sd, HDA_SD_CTL);
        if (a) ctl |= HDA_SD_CTL_RUN; else ctl &= ~HDA_SD_CTL_RUN;
        hda_sd_write32(sd, HDA_SD_CTL, ctl);
        return (int64_t)hda_sd_read32(sd, HDA_SD_CTL);
    }
    case 5: {
        uint32_t verb = (a == 0) ? HDA_VERB_GET_GPIO_MASK
                      : (a == 1) ? HDA_VERB_GET_GPIO_DIR
                                 : HDA_VERB_GET_GPIO_DATA;
        return (int64_t)(uint32_t)hda_codec_command(cd->cad, cd->fg_nid, verb);
    }
    case 6:
        hda_codec_command(cd->cad, cd->fg_nid, HDA_VERB_SET_GPIO_MASK | ((uint32_t)a & 0xFF));
        hda_codec_command(cd->cad, cd->fg_nid, HDA_VERB_SET_GPIO_DIR  | ((uint32_t)b & 0xFF));
        hda_codec_command(cd->cad, cd->fg_nid, HDA_VERB_SET_GPIO_DATA | ((uint32_t)c & 0xFF));
        return 0;
    case 7: {
        // Fill the DMA buffer with a signed-16 stereo square wave (integer-only;
        // the kernel builds with -mno-sse so no float). period_frames = full
        // wave period; 0 => silence. Format is 16-bit stereo (see stream_format).
        if (!hda_state.dma_buffer) return -1;
        int16_t *buf = (int16_t *)hda_state.dma_buffer;
        uint32_t frames = hda_state.dma_buffer_size / 4;   // 2ch * 2 bytes
        uint32_t period = (uint32_t)a;
        const int16_t amp = 0x2000;
        for (uint32_t i = 0; i < frames; i++) {
            int16_t s = 0;
            if (period >= 2) s = ((i % period) < (period / 2)) ? amp : (int16_t)-amp;
            buf[i * 2 + 0] = s;   // L
            buf[i * 2 + 1] = s;   // R
        }
        return (int64_t)frames;
    }
    case 8:
        if (a >= 0x2000) return -1;
        return (int64_t)hda_read32((uint32_t)a);
    case 9:
        if (a >= 0x2000) return -1;
        hda_write32((uint32_t)a, (uint32_t)b);
        return 0;
    case 10:
        return (int64_t)(((uint32_t)sd << 24) | ((uint32_t)cd->cad << 16) |
                         ((uint32_t)cd->fg_nid << 8) | (hda_state.initialized ? 1u : 0u));
    default:
        return -1;
    }
}

// #71: arm MSI on the winning controller. Must run after lapic_init() (called
// from main.c right after smp_init()); see hda.h for the full contract.
void hda_setup_interrupt(void) {
    if (!hda_state.initialized) return;

    pci_device_t *dev = NULL;
    int count = pci_get_device_count();
    for (int i = 0; i < count; i++) {
        pci_device_t *d = pci_get_device(i);
        if (d && d->bus == hda_state.pci_bus && d->slot == hda_state.pci_slot &&
            d->func == hda_state.pci_func) {
            dev = d;
            break;
        }
    }
    if (!dev) {
        kprintf("[HDA] #71: could not re-find winning controller for MSI setup\n");
        return;
    }

    idt_register_handler(HDA_MSI_VECTOR, hda_msi_isr);

    uint32_t dest = lapic_get_id();   // target the BSP
    if (pci_enable_msi(dev, HDA_MSI_VECTOR, dest)) {
        hda_state.msi_enabled = true;
        hda_state.msi_vector = HDA_MSI_VECTOR;
        kprintf("[HDA] #71: MSI armed on vector 0x%02x -> LAPIC %u "
                "(controller %04x:%04x, legacy PCI IRQ line was %u)\n",
                HDA_MSI_VECTOR, dest, hda_state.vendor_id, hda_state.device_id,
                hda_state.irq);
    } else {
        kprintf("[HDA] #71: controller has no MSI capability; relying on "
                "LPIB poll worker only (legacy PCI IRQ line=%u)\n", hda_state.irq);
    }
}

// ============================================================================
// Public API
// ============================================================================

// Point the register helpers at a controller and bring CORB/RIRB up on it.
// Returns AUDIO_OK if the controller reset and its command ring is running.
static int hda_bring_up_controller(pci_device_t *dev) {
    pci_enable_bus_master(dev);
    uint64_t bar = pci_get_bar_address(dev, 0);
    if (bar == 0) return AUDIO_ERR_NO_DEVICE;
    hda_state.mmio = (volatile uint8_t *)(uintptr_t)bar;
    hda_state.mmio_phys = bar;
    if (hda_reset_controller() != AUDIO_OK) return AUDIO_ERR_TIMEOUT;
    if (hda_setup_corb_rirb() != AUDIO_OK) return AUDIO_ERR_NO_MEMORY;
    hda_delay(1000);   // codecs report on STATESTS after CRST deasserts
    return AUDIO_OK;
}

// #71: confirm the output-stream DMA actually runs. Fills the cyclic buffer with
// the supplied content (silence if buf==NULL) and checks the LPIB advances.
// Leaves the stream stopped. Returns 1 if DMA advanced, else 0.
static int hda_check_output_dma(void) {
    uint8_t s = hda_state.out_stream_idx;
    hda_state.write_index = 0;
    uint32_t lp0 = hda_sd_read32(s, HDA_SD_LPIB);
    // Start the stream running over the (silent) cyclic buffer.
    uint32_t ctl = hda_sd_read32(s, HDA_SD_CTL);
    hda_sd_write32(s, HDA_SD_CTL, ctl | HDA_SD_CTL_RUN);
    uint32_t lpn = lp0;
    int moved = 0;
    for (int i = 0; i < 60; i++) {
        hda_delay(2000);
        lpn = hda_sd_read32(s, HDA_SD_LPIB);
        if (lpn != lp0) { moved = 1; break; }
    }
    // Stop again; normal playback re-starts it via hda_start().
    ctl = hda_sd_read32(s, HDA_SD_CTL);
    hda_sd_write32(s, HDA_SD_CTL, ctl & ~HDA_SD_CTL_RUN);
    for (int i = 0; i < 100; i++) {
        if ((hda_sd_read32(s, HDA_SD_CTL) & HDA_SD_CTL_RUN) == 0) break;
        hda_delay(100);
    }
    kprintf("[HDA] output DMA check: LPIB %u -> %u : %s\n",
            lp0, lpn, moved ? "RUNNING (DMA advances)" : "STALLED");
    return moved;
}

int hda_init(void) {
    LOG_INFO("[HDA] Initializing Intel HDA driver");
    kprintf("[HDA] Scanning ALL HD Audio controllers (class 04:03)...\n");

    if (hda_state.initialized) {
        return AUDIO_OK;
    }

    memset(&hda_state, 0, sizeof(hda_state));

    // Iterate EVERY PCI HD-Audio controller and pick the controller+codec with
    // the best output path (analog speaker/line-out/HP preferred over digital).
    // This is what lets us drive the iMac's Cirrus codec on the PCH controller
    // instead of only the first (GPU/HDMI) controller pci_find_class returns.
    int best_score = -1;
    pci_device_t best_dev;   memset(&best_dev, 0, sizeof(best_dev));
    hda_codec_t  best_codec; memset(&best_codec, 0, sizeof(best_codec));
    int controllers = 0, codecs_seen = 0;

    int count = pci_get_device_count();
    for (int idx = 0; idx < count; idx++) {
        pci_device_t *dev = pci_get_device(idx);
        if (!dev) continue;
        if (dev->class_code != HDA_PCI_CLASS || dev->subclass != HDA_PCI_SUBCLASS) continue;
        controllers++;

        kprintf("[HDA] Controller #%d %04x:%04x at %02x:%02x.%x\n",
                controllers, dev->vendor_id, dev->device_id, dev->bus, dev->slot, dev->func);

        if (hda_bring_up_controller(dev) != AUDIO_OK) {
            kprintf("[HDA]   bring-up failed; skipping\n");
            continue;
        }

        uint16_t statests = hda_read16(HDA_REG_STATESTS);
        hda_write16(HDA_REG_STATESTS, statests);
        kprintf("[HDA]   STATESTS=0x%04x\n", statests);
        if (statests == 0) continue;

        for (int cad = 0; cad < 15; cad++) {
            if (!(statests & (1 << cad))) continue;
            hda_codec_t c;
            int s = hda_parse_codec(cad, &c);
            // #390: a codec that gives no VendorID response (the iMac GPU-HDA
            // controller #1's codec@0 does exactly this) is skipped so we fall
            // through to controller #2's real Cirrus CS4208 codec.
            if (!c.vendor_id) {
                kprintf("[HDA]   codec@%d: no VendorID response, skipping\n", cad);
                continue;
            }
            codecs_seen++;
            kprintf("[HDA]   codec@%d %04x:%04x -> %s (score %d, %s dev=0x%x routes=%d)\n",
                    cad, c.vendor_id, c.device_id,
                    s >= 0 ? "OUTPUT PATH" : "no output", s,
                    c.is_analog ? "analog" : "digital", c.default_device,
                    c.num_out_routes);
            if (s > best_score) {
                best_score = s;
                best_codec = c;
                best_dev = *dev;
            }
        }
    }

    if (controllers == 0) {
        kprintf("[HDA] No HD Audio controller present\n");
        return AUDIO_ERR_NO_DEVICE;
    }
    if (best_score < 0) {
        kprintf("[HDA] No usable output path on any of %d controller(s), %d codec(s)\n",
                controllers, codecs_seen);
        hda_free_dma_buffers();
        return AUDIO_ERR_NO_DEVICE;
    }

    // Finalize on the winning controller.
    hda_state.pci_bus  = best_dev.bus;
    hda_state.pci_slot = best_dev.slot;
    hda_state.pci_func = best_dev.func;
    hda_state.vendor_id = best_dev.vendor_id;
    hda_state.device_id = best_dev.device_id;
    hda_state.irq = best_dev.interrupt_line;
    hda_state.mmio_size = pci_get_bar_size(&best_dev, 0);

    if (hda_bring_up_controller(&best_dev) != AUDIO_OK) {
        kprintf("[HDA] Winning controller re-init failed\n");
        return AUDIO_ERR_NO_DEVICE;
    }
    hda_write16(HDA_REG_STATESTS, hda_read16(HDA_REG_STATESTS));

    uint16_t gcap = hda_read16(HDA_REG_GCAP);
    hda_state.supports_64bit = (gcap & HDA_GCAP_64OK) != 0;
    hda_state.num_oss = (gcap >> 12) & 0xF;
    hda_state.num_iss = (gcap >> 8) & 0xF;
    hda_state.num_bss = (gcap >> 3) & 0x1F;
    kprintf("[HDA] Winner %04x:%04x GCAP=0x%04x OSS=%d ISS=%d codec %04x:%04x\n",
            hda_state.vendor_id, hda_state.device_id, gcap,
            hda_state.num_oss, hda_state.num_iss,
            best_codec.vendor_id, best_codec.device_id);

    if (hda_state.num_oss == 0) {
        kprintf("[HDA] No output streams available\n");
        return AUDIO_ERR_NO_DEVICE;
    }
    hda_state.out_stream_idx = hda_state.num_iss;

    hda_state.codec = best_codec;
    hda_state.codec_found = true;

    hda_configure_codec();

    int ret = hda_setup_dma_buffers();
    if (ret != AUDIO_OK) {
        return ret;
    }

    hda_state.sample_rate = 48000;
    hda_state.channels = 2;
    hda_state.format = AUDIO_FORMAT_S16_LE;
    hda_state.stream_format = hda_calculate_format(48000, 2, 16);

    // Converter format must be set on EVERY route DAC for QEMU/hw to open the
    // voice. #390: for the stereo Cirrus both speaker DACs (10 and 11) get it.
    {
        int nr = hda_state.codec.num_out_routes ? hda_state.codec.num_out_routes : 1;
        for (int r = 0; r < nr; r++) {
            hda_codec_command(hda_state.codec.cad, hda_state.codec.route_dac[r],
                              HDA_VERB_SET_CONV_FMT | hda_state.stream_format);
        }
    }

    hda_configure_output_stream();

    uint32_t intctl = HDA_INTCTL_GIE | HDA_INTCTL_CIE |
                      (1 << hda_state.out_stream_idx);
    hda_write32(HDA_REG_INTCTL, intctl);

    hda_state.initialized = true;
    hda_state.playing = false;
    hda_state.write_index = 0;
    hda_state.read_index = 0;

    // #71 core proof: confirm the output-stream DMA position advances.
    hda_check_output_dma();

    // #699 FIX: do NOT start the LPIB-poll worker here. hda_init() (called
    // from audio_init() in main.c, right after usb_init()) runs LONG BEFORE
    // proc_init() (main.c, right before interrupts are enabled). Calling
    // proc_create() this early corrupts the scheduler: proc_init() later does
    // memset(proc_table, 0, sizeof(proc_table)) to (re)initialize every slot,
    // which wipes out the very slot this pre-init worker was allocated into,
    // but does NOT reset the static ready_queue_head/ready_queue_tail
    // pointers left over from add_to_ready_queue(hdapoll) -- they keep
    // pointing at that now-zeroed (and later reused-for-idle) slot. Every
    // process created afterward gets linked into the ready queue relative to
    // that stale/aliased entry, and on real HDA hardware (or a qemu
    // ich9-intel-hda VM) this reliably starved sshd's listener thread before
    // it ever reached tcp_listen()/printed "listening on port 22" (#699).
    // hda_start_poll_worker() is now started by hda_start_poll_worker_deferred()
    // which main.c calls right after proc_init() runs; see hda.h for the
    // contract. This is purely a matter of WHEN the worker starts -- audio
    // playback itself is unaffected, since output DMA is a free-running
    // hardware ring that does not need the poll worker to have started yet.

    LOG_INFO("[HDA] Initialization complete");
    kprintf("[HDA] Init complete: %s output, codec %04x:%04x DAC=%d PIN=%d\n",
            hda_state.codec.is_analog ? "ANALOG" : "digital",
            hda_state.codec.vendor_id, hda_state.codec.device_id,
            hda_state.codec.dac_nid, hda_state.codec.out_pin_nid);

    // #418: /DEVLOG.TXT PCI-claim tracking. best_dev was a local copy (the
    // winning controller may not be pci_devices[0], see the multi-controller
    // scan above), so re-resolve the actual array entry by vendor:device
    // before marking it. Best-effort: if two identical controllers exist,
    // this marks the first match, which is still enough to answer "was this
    // vendor:device claimed by anything" for /DEVLOG.TXT's purposes.
    {
        pci_device_t *won = pci_find_device(hda_state.vendor_id, hda_state.device_id);
        if (won) pci_mark_claimed(won, "hda");
    }

    return AUDIO_OK;
}

void hda_shutdown(void) {
    if (!hda_state.initialized) return;

    hda_stop();
    hda_free_dma_buffers();

    hda_state.initialized = false;
    LOG_INFO("[HDA] Shutdown complete");
}

bool hda_is_available(void) {
    return hda_state.initialized && hda_state.codec_found;
}

int hda_get_device_info(audio_device_info_t *info) {
    if (!info) return AUDIO_ERR_INVALID_PARAM;

    info->type = AUDIO_DEVICE_HDA;
    info->name = "Intel HD Audio";
    info->description = "Intel High Definition Audio Controller";
    info->supported_formats = AUDIO_FORMAT_S16_LE | AUDIO_FORMAT_S24_LE | AUDIO_FORMAT_S32_LE;
    info->min_sample_rate = 8000;
    info->max_sample_rate = 192000;
    info->max_channels = 8;
    info->supports_mixing = true;
    info->supports_src = true;

    return AUDIO_OK;
}

int hda_configure(uint32_t format, uint32_t sample_rate, uint32_t channels) {
    if (!hda_state.initialized) return AUDIO_ERR_NOT_INITIALIZED;

    uint32_t bits = 16;
    if (format == AUDIO_FORMAT_S24_LE) bits = 24;
    else if (format == AUDIO_FORMAT_S32_LE) bits = 32;
    else format = AUDIO_FORMAT_S16_LE;

    if (sample_rate == 0) sample_rate = 48000;
    if (channels == 0) channels = 2;
    if (channels > 8) channels = 8;

    hda_state.format = format;
    hda_state.sample_rate = sample_rate;
    hda_state.channels = channels;
    hda_state.stream_format = hda_calculate_format(sample_rate, channels, bits);

    // Set the converter (DAC) stream/channel AND format on EVERY route DAC. The
    // format verb is required for QEMU to open the output voice; without it LPIB
    // never moves. #390: for the stereo Cirrus, route 0 = channel 0 (left),
    // route 1 = channel 1 (right).
    {
        int nr = hda_state.codec.num_out_routes ? hda_state.codec.num_out_routes : 1;
        for (int r = 0; r < nr; r++) {
            hda_codec_command(hda_state.codec.cad, hda_state.codec.route_dac[r],
                              HDA_VERB_SET_CONV_FMT | hda_state.stream_format);
            hda_codec_command(hda_state.codec.cad, hda_state.codec.route_dac[r],
                              HDA_VERB_SET_STREAM | (1 << 4) | (r & 0x0F));
        }
    }

    hda_configure_output_stream();

    kprintf("[HDA] Configured: %u Hz, %u channels, %u bits\n",
            sample_rate, channels, bits);

    return AUDIO_OK;
}

uint32_t hda_debug_lpib(void) {
    if (!hda_state.initialized) return 0;
    return hda_sd_read32(hda_state.out_stream_idx, HDA_SD_LPIB);
}

int hda_start(void) {
    if (!hda_state.initialized) return AUDIO_ERR_NOT_INITIALIZED;
    if (hda_state.playing) return AUDIO_OK;

    uint8_t stream = hda_state.out_stream_idx;

    uint32_t ctl = hda_sd_read32(stream, HDA_SD_CTL);
    ctl |= HDA_SD_CTL_RUN;
    hda_sd_write32(stream, HDA_SD_CTL, ctl);

    hda_state.playing = true;
LOG_INFO("[HDA] Playback started");

    return AUDIO_OK;
}

int hda_stop(void) {
    if (!hda_state.initialized) return AUDIO_ERR_NOT_INITIALIZED;

    uint8_t stream = hda_state.out_stream_idx;

    uint32_t ctl = hda_sd_read32(stream, HDA_SD_CTL);
    ctl &= ~HDA_SD_CTL_RUN;
    hda_sd_write32(stream, HDA_SD_CTL, ctl);

    for (int i = 0; i < 100; i++) {
        if ((hda_sd_read32(stream, HDA_SD_CTL) & HDA_SD_CTL_RUN) == 0) break;
        hda_delay(100);
    }

    hda_state.playing = false;
    LOG_INFO("[HDA] Playback stopped");

    return AUDIO_OK;
}

int hda_write(const void *buffer, uint32_t frames) {
    if (!hda_state.initialized || !buffer) return AUDIO_ERR_INVALID_PARAM;

    uint32_t bits = 16;
    if (hda_state.format == AUDIO_FORMAT_S24_LE || hda_state.format == AUDIO_FORMAT_S32_LE) {
        bits = 32;
    }
    uint32_t bytes_per_frame = (bits / 8) * hda_state.channels;
    uint32_t bytes = frames * bytes_per_frame;

    uint8_t stream = hda_state.out_stream_idx;
    uint32_t lpib = hda_sd_read32(stream, HDA_SD_LPIB);
    uint32_t current_bdl = lpib / hda_state.bdl_buffer_size;

    int available = (int)current_bdl - (int)hda_state.write_index;
    if (available <= 0) available += HDA_NUM_BDL;
    available--;

    if (available <= 0) return 0;

    uint32_t written_frames = 0;
    const uint8_t *src = (const uint8_t *)buffer;

    while (bytes > 0 && available > 0) {
        uint32_t bdl_bytes = hda_state.bdl_buffer_size;
        if (bdl_bytes > bytes) bdl_bytes = bytes;

        uint8_t *dst = (uint8_t *)hda_state.dma_buffer +
                       (hda_state.write_index * hda_state.bdl_buffer_size);
        memcpy(dst, src, bdl_bytes);

        src += bdl_bytes;
        bytes -= bdl_bytes;
        written_frames += bdl_bytes / bytes_per_frame;

        hda_state.write_index = (hda_state.write_index + 1) % HDA_NUM_BDL;
        available--;
    }

    hda_state.frames_played += written_frames;

    return written_frames;
}

int hda_avail(void) {
    if (!hda_state.initialized) return 0;

    uint8_t stream = hda_state.out_stream_idx;
    uint32_t lpib = hda_sd_read32(stream, HDA_SD_LPIB);
    uint32_t current_bdl = lpib / hda_state.bdl_buffer_size;

    int available = (int)current_bdl - (int)hda_state.write_index;
    if (available <= 0) available += HDA_NUM_BDL;
    available--;

    if (available < 0) available = 0;

    uint32_t bits = 16;
    if (hda_state.format == AUDIO_FORMAT_S24_LE || hda_state.format == AUDIO_FORMAT_S32_LE) {
        bits = 32;
    }
    uint32_t bytes_per_frame = (bits / 8) * hda_state.channels;

    return (available * hda_state.bdl_buffer_size) / bytes_per_frame;
}

void hda_set_volume(uint8_t left, uint8_t right) {
    if (!hda_state.initialized) return;

    if (left > 127) left = 127;
    if (right > 127) right = 127;

    hda_codec_command(hda_state.codec.cad, hda_state.codec.dac_nid,
                      HDA_VERB_SET_AMP_GAIN | HDA_AMP_SET_OUTPUT |
                      HDA_AMP_SET_LEFT | left);
    hda_codec_command(hda_state.codec.cad, hda_state.codec.dac_nid,
                      HDA_VERB_SET_AMP_GAIN | HDA_AMP_SET_OUTPUT |
                      HDA_AMP_SET_RIGHT | right);
}

void hda_mute(bool mute) {
    if (!hda_state.initialized) return;

    uint8_t gain = mute ? HDA_AMP_MUTE : 0x7F;

    hda_codec_command(hda_state.codec.cad, hda_state.codec.dac_nid,
                      HDA_VERB_SET_AMP_GAIN | HDA_AMP_SET_OUTPUT |
                      HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | gain);
}

hda_state_t *hda_get_state(void) {
    return &hda_state;
}

void hda_print_info(void) {
    kprintf("\n[HDA] Driver Information:\n");
    kprintf("  Initialized:  %s\n", hda_state.initialized ? "Yes" : "No");

    if (!hda_state.initialized) return;

    kprintf("  PCI Device:   %04x:%04x at %02x:%02x.%x\n",
            hda_state.vendor_id, hda_state.device_id,
            hda_state.pci_bus, hda_state.pci_slot, hda_state.pci_func);
    kprintf("  IRQ:          %u\n", hda_state.irq);
    kprintf("  MMIO:         0x%llx (size %u)\n", hda_state.mmio_phys, hda_state.mmio_size);
    kprintf("  Streams:      OSS=%u, ISS=%u, BSS=%u\n",
            hda_state.num_oss, hda_state.num_iss, hda_state.num_bss);
    kprintf("  64-bit:       %s\n", hda_state.supports_64bit ? "Yes" : "No");

    if (hda_state.codec_found) {
        kprintf("  Codec:        %04x:%04x at address %d\n",
                hda_state.codec.vendor_id, hda_state.codec.device_id,
                hda_state.codec.cad);
        kprintf("  DAC NID:      %d\n", hda_state.codec.dac_nid);
        kprintf("  Output PIN:   %d\n", hda_state.codec.out_pin_nid);
    }

    kprintf("  Sample Rate:  %u Hz\n", hda_state.sample_rate);
    kprintf("  Channels:     %u\n", hda_state.channels);
    kprintf("  Playing:      %s\n", hda_state.playing ? "Yes" : "No");
    kprintf("  Frames:       %llu\n", hda_state.frames_played);
    kprintf("  Underruns:    %llu\n", hda_state.underruns);
}

bool hda_is_analog_output(void) {
    return hda_state.initialized && hda_state.codec.is_analog;
}

// #71: fill the cyclic buffer with a sine tone and START the output stream,
// confirming the output-stream DMA position (LPIB) advances. Leaves the stream
// RUNNING (the buffer loops) so the caller can hold it audible via a yielding
// proc_sleep and then hda_stop(); this avoids busy-waiting the CPU. The ms hint
// is unused here (kept for API stability). Returns AUDIO_OK if DMA advanced.
int hda_selftest_tone(uint32_t freq_hz, uint32_t ms) {
    (void)ms;
    if (!hda_state.initialized) return AUDIO_ERR_NOT_INITIALIZED;
    if (freq_hz < 50) freq_hz = 440;

    hda_configure(AUDIO_FORMAT_S16_LE, 48000, 2);

    int16_t *buf = (int16_t *)hda_state.dma_buffer;
    uint32_t frames = hda_state.dma_buffer_size / 4;    // 16-bit stereo
    uint32_t inc = (uint32_t)(((uint64_t)freq_hz * 64u * 65536u) / 48000u);  // Q16 phase step
    uint32_t acc = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t sv = hda_sine64[(acc >> 16) & 63];
        buf[2 * i] = sv;
        buf[2 * i + 1] = sv;
        acc += inc;
    }
    hda_state.write_index = 0;
    hda_state.frames_played = 0;

    uint8_t s = hda_state.out_stream_idx;
    uint32_t lp0 = hda_sd_read32(s, HDA_SD_LPIB);
    hda_start();
    uint32_t lpn = lp0;
    int moved = 0;
    for (int i = 0; i < 40; i++) {
        hda_delay(3000);
        lpn = hda_sd_read32(s, HDA_SD_LPIB);
        if (lpn != lp0) { moved = 1; break; }
    }
    kprintf("[HDA] TONE %uHz: LPIB %u -> %u : %s (stream=%d fmt=0x%04x)\n",
            freq_hz, lp0, lpn, moved ? "DMA RUNNING" : "DMA STALLED",
            s, hda_state.stream_format);

    return moved ? AUDIO_OK : AUDIO_ERR_DMA_ERROR;
}

// ============================================================================
// #388 DEVLOG: HD Audio codec identification (additive PART 1 diagnostic)
// ============================================================================
//
// The PCI dump already names the HDA *controller* (class 04:03). The actual
// *codec* (the iMac14,4's is Cirrus Logic) lives on the HDA serial link, not on
// PCI, so it needs a codec query. This is a one-shot, bounded, best-effort scan
// used only to NAME the codec(s) for the audio-driver work (#71); it does not
// try to make audio work.
//
// Two paths, chosen to avoid disturbing a working audio path:
//   (1) If hda_init already identified a codec (the QEMU / working case), report
//       it straight out of hda_state - zero extra hardware access.
//   (2) Otherwise (e.g. the real iMac, where hda_init bailed before it found a
//       usable DAC/pin), do a fresh minimal probe: map BAR0, reset the
//       controller, read STATESTS, and for each responding codec read the
//       root-node VendorID / Node Count / first Function-Group type via the
//       Immediate Command interface (with a CORB/RIRB fallback). Every wait is
//       hard-bounded (~10ms x small counts) so this can never hang boot.

static const char *hda_widget_type_name(uint8_t t) {
    switch (t) {
        case 0x0: return "Audio Output/DAC";
        case 0x1: return "Audio Input/ADC";
        case 0x2: return "Audio Mixer";
        case 0x3: return "Audio Selector";
        case 0x4: return "Pin Complex";
        case 0x5: return "Power Widget";
        case 0x6: return "Volume Knob";
        case 0x7: return "Beep Generator";
        case 0xF: return "Vendor-defined";
        default:  return "Reserved";
    }
}

static const char *hda_default_device_name(uint8_t d) {
    switch (d) {
        case 0x0: return "Line-Out"; case 0x1: return "Speaker";  case 0x2: return "HP-Out";
        case 0x3: return "CD";       case 0x4: return "SPDIF-Out"; case 0x5: return "Dig-Other-Out";
        case 0x6: return "Modem-Line"; case 0x7: return "Modem-Handset"; case 0x8: return "Line-In";
        case 0x9: return "AUX";      case 0xA: return "Mic-In";    case 0xB: return "Telephony";
        case 0xC: return "SPDIF-In"; case 0xD: return "Dig-Other-In"; case 0xF: return "Other";
        default:  return "Reserved";
    }
}

// Dump the full widget node graph for one codec to the emit callback.
static void hda_devlog_dump_codec(uint8_t cad, void (*emit)(const char *line)) {
    char line[196];

    uint32_t vid = hda_codec_command(cad, 0, HDA_VERB_GET_PARAM | HDA_PARAM_VENDOR_ID);
    if (vid == 0 || vid == 0xFFFFFFFF) {
        snprintf(line, sizeof(line), "  codec@%d: present (STATESTS) but no VendorID response", cad);
        emit(line);
        return;
    }
    uint32_t rev = hda_codec_command(cad, 0, HDA_VERB_GET_PARAM | HDA_PARAM_REVISION_ID);
    snprintf(line, sizeof(line), "  codec@%d: vendor:device %04x:%04x revision=0x%08x",
             cad, (unsigned)((vid >> 16) & 0xFFFF), (unsigned)(vid & 0xFFFF), rev);
    emit(line);

    uint32_t nc = hda_codec_command(cad, 0, HDA_VERB_GET_PARAM | HDA_PARAM_NODE_COUNT);
    uint8_t fg_start = (nc >> 16) & 0xFF, fg_count = nc & 0xFF;

    for (int f = 0; f < fg_count && f < 8; f++) {
        uint8_t fg = fg_start + f;
        uint32_t fgt = hda_codec_command(cad, fg, HDA_VERB_GET_PARAM | HDA_PARAM_FG_TYPE);
        uint8_t kind = fgt & 0x7F;
        const char *kn = (kind == 0x01) ? "Audio" : (kind == 0x02) ? "Modem" : "Other";
        snprintf(line, sizeof(line), "  Function Group @%d: type=%s(0x%02x)", fg, kn, kind);
        emit(line);
        if (kind != 0x01) continue;   // only enumerate widgets under the audio FG

        uint32_t wnc = hda_codec_command(cad, fg, HDA_VERB_GET_PARAM | HDA_PARAM_NODE_COUNT);
        uint8_t start = (wnc >> 16) & 0xFF, cnt = wnc & 0xFF;
        snprintf(line, sizeof(line), "    Audio FG: %d widgets, nid %d..%d", cnt, start, start + cnt - 1);
        emit(line);
        if (cnt > 128) cnt = 128;

        for (int nid = start; nid < start + cnt; nid++) {
            uint32_t wcap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AUDIO_WIDGET_CAP);
            uint8_t type = (wcap >> 20) & 0xF;
            snprintf(line, sizeof(line), "    nid %2d: %-17s wcap=0x%08x", nid, hda_widget_type_name(type), wcap);
            emit(line);

            if (type == 0x4) {   // Pin Complex
                uint32_t pincap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_PIN_CAP);
                uint32_t cfg = hda_codec_command(cad, nid, HDA_VERB_GET_CONFIG_DEF);
                uint8_t conn = (cfg >> 30) & 0x3, dev = (cfg >> 20) & 0xF;
                const char *cn = (conn == 0) ? "Jack" : (conn == 1) ? "None" : (conn == 2) ? "Fixed" : "Both";
                snprintf(line, sizeof(line),
                         "          pin: cfg=0x%08x device=%s conn=%s outCap=%d inCap=%d hpCap=%d pincap=0x%08x",
                         cfg, hda_default_device_name(dev), cn,
                         (pincap >> 4) & 1, (pincap >> 5) & 1, (pincap >> 3) & 1, pincap);
                emit(line);
            }

            // Amp caps (report when present).
            uint32_t aoc = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AMP_OUT_CAP);
            uint32_t aic = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AMP_IN_CAP);
            if ((aoc && aoc != 0xFFFFFFFF) || (aic && aic != 0xFFFFFFFF)) {
                snprintf(line, sizeof(line), "          amp: out=0x%08x in=0x%08x", aoc, aic);
                emit(line);
            }

            // Connection list.
            if (type == 0x2 || type == 0x3 || type == 0x4 || type == 0x0 || type == 0x1) {
                uint8_t conns[16];
                int n = hda_get_connections(cad, nid, conns, 16);
                if (n > 0) {
                    char cl[160]; int p = 0;
                    p += snprintf(cl + p, sizeof(cl) - p, "          conns:");
                    for (int i = 0; i < n && p < (int)sizeof(cl) - 8; i++)
                        p += snprintf(cl + p, sizeof(cl) - p, " %d", conns[i]);
                    emit(cl);
                }
            }
        }
    }
}

// PART A: enumerate EVERY HD-Audio controller and dump each codec's full node
// graph. Bounded and non-destructive: the live audio driver's state is snapshot
// and restored (and its winning controller re-armed) so playback still works.
void hda_devlog_scan(void (*emit)(const char *line)) {
    char line[196];
    if (!emit) return;

    hda_state_t saved = hda_state;   // preserve the live audio driver's context

    int count = pci_get_device_count();
    int controllers = 0;
    for (int idx = 0; idx < count; idx++) {
        pci_device_t *dev = pci_get_device(idx);
        if (!dev) continue;
        if (dev->class_code != HDA_PCI_CLASS || dev->subclass != HDA_PCI_SUBCLASS) continue;
        controllers++;

        uint64_t bar = pci_get_bar_address(dev, 0);
        snprintf(line, sizeof(line),
                 "HDA controller #%d: %04x:%04x at %02x:%02x.%x BAR0=0x%llx",
                 controllers, dev->vendor_id, dev->device_id, dev->bus, dev->slot, dev->func, bar);
        emit(line);

        if (hda_bring_up_controller(dev) != AUDIO_OK) {
            emit("  controller reset/CORB failed; skipping");
            continue;
        }
        uint16_t statests = hda_read16(HDA_REG_STATESTS);
        hda_write16(HDA_REG_STATESTS, statests);
        snprintf(line, sizeof(line), "  STATESTS=0x%04x (bit N = codec at address N)", statests);
        emit(line);
        if (statests == 0) { emit("  no codec responded"); continue; }

        for (int cad = 0; cad < 15; cad++) {
            if (!(statests & (1 << cad))) continue;
            hda_devlog_dump_codec((uint8_t)cad, emit);
        }
    }
    if (controllers == 0) {
        emit("HDA: no HD Audio controller present (no PCI class 04:03)");
    }

    // Restore the live audio driver and re-arm its controller so audio keeps
    // working after this diagnostic scan repointed the register window.
    hda_state = saved;
    if (saved.initialized && saved.mmio) {
        hda_reset_controller();
        hda_setup_corb_rirb();
        hda_delay(1000);
        hda_write16(HDA_REG_STATESTS, hda_read16(HDA_REG_STATESTS));
        hda_configure_codec();
        {
            int nr = hda_state.codec.num_out_routes ? hda_state.codec.num_out_routes : 1;
            for (int r = 0; r < nr; r++)
                hda_codec_command(hda_state.codec.cad, hda_state.codec.route_dac[r],
                                  HDA_VERB_SET_CONV_FMT | hda_state.stream_format);
        }
        hda_configure_output_stream();
        uint32_t intctl = HDA_INTCTL_GIE | HDA_INTCTL_CIE | (1 << hda_state.out_stream_idx);
        hda_write32(HDA_REG_INTCTL, intctl);
    }
}

// ============================================================================
// #71 / Cirrus CS4208 AUDIOLOG: focused audio-output diagnostic -> /AUDIOLOG.TXT
// ============================================================================
//
// Answers, from a single file readable over SSH on the iMac:
//   - which HDA controller (PCI vendor:device) won, and its codec identity
//     (confirming Cirrus 0x1013:CS4208 vs whatever the machine reports);
//   - the full output-relevant widget graph (DAC nodes, pin-complex config-
//     defaults incl. "is it a speaker?", amp caps) via hda_devlog_scan();
//   - the LIVE post-configure state that decides audibility: per output route
//     the current EAPD, pin control, and DAC/pin output-amp gain+mute; the
//     codec GPIO mask/dir/data (the CS4208 speaker-amp power gate); and the
//     output-stream descriptor format/CTL(RUN)/STS/LPIB (did DMA actually run).
static void hda_audiolog_amp_out(uint8_t cad, uint8_t nid, uint8_t *gain, uint8_t *mute) {
    uint32_t r = hda_codec_command(cad, nid, HDA_VERB_GET_AMP_GAIN | 0x8000); // out, left
    *gain = r & 0x7F;
    *mute = (r >> 7) & 1;
}

// Bounded wrapper: arm the diagnostic caps (short per-command spin + a hard cap
// on total timeouts) so this dump can never freeze the box the way the un-gated
// scan did on the iMac (b730/b733), then run the real report, then restore.
// A responsive codec still gets fully dumped; a silent one bails in ~1-2s.
static void hda_audiolog_report_body(void (*emit)(const char *line));
void hda_audiolog_report(void (*emit)(const char *line)) {
    int save_iters = g_hda_cmd_max_iters;
    g_hda_cmd_max_iters = 200;   // ~20ms per timed-out command (was ~200ms)
    g_hda_diag_budget   = 60;    // <=60 timeouts -> <=~1.2s of pure-timeout spin
    g_hda_diag_active   = 1;
    hda_audiolog_report_body(emit);
    g_hda_diag_active   = 0;
    g_hda_diag_budget   = 0;
    g_hda_cmd_max_iters = save_iters;
}

static void hda_audiolog_report_body(void (*emit)(const char *line)) {
    char line[196];
    if (!emit) return;

    emit("=== MayteraOS HD Audio diagnostic (AUDIOLOG) ===");

    if (!hda_state.initialized) {
        emit("HDA: driver NOT initialized - no usable analog output path was");
        emit("     found on any HD Audio controller (see the widget graph below");
        emit("     for what codec(s) DID respond). Audio will be silent.");
    } else {
        hda_codec_t *c = &hda_state.codec;
        snprintf(line, sizeof(line),
                 "Controller: %04x:%04x at %02x:%02x.%x IRQ=%u MMIO=0x%llx OSS=%u ISS=%u BSS=%u",
                 hda_state.vendor_id, hda_state.device_id,
                 hda_state.pci_bus, hda_state.pci_slot, hda_state.pci_func,
                 hda_state.irq, hda_state.mmio_phys,
                 hda_state.num_oss, hda_state.num_iss, hda_state.num_bss);
        emit(line);
        // #71: real-interrupt status. legacy PCI IRQ line 0 == "no legacy INTx
        // routing"; msi_enabled tells whether hda_setup_interrupt() managed to
        // arm a real MSI interrupt as a substitute. The poll worker line right
        // after this proves output does not depend on either one.
        if (hda_state.msi_enabled) {
            snprintf(line, sizeof(line),
                     "Interrupt: legacy PCI IRQ line=%u (0 = not routed by firmware)  "
                     "MSI=armed on vector 0x%02x",
                     hda_state.irq, hda_state.msi_vector);
        } else {
            snprintf(line, sizeof(line),
                     "Interrupt: legacy PCI IRQ line=%u (0 = not routed by firmware)  "
                     "MSI=NOT armed (no MSI capability, or hda_setup_interrupt() not yet run)",
                     hda_state.irq);
        }
        emit(line);
        snprintf(line, sizeof(line),
                 "LPIB poll worker: %s (services stream every %ums independent of any interrupt)",
                 g_hda_poll_started ? "running" : "NOT running", (unsigned)HDA_POLL_MS);
        emit(line);
        snprintf(line, sizeof(line),
                 "Winning codec@%d: vendor:device %04x:%04x  AFG nid=%d  %s output  routes=%d  score=%d",
                 c->cad, c->vendor_id, c->device_id, c->fg_nid,
                 c->is_analog ? "ANALOG" : "digital",
                 c->num_out_routes, c->route_score);
        emit(line);
        if (c->vendor_id == HDA_VENDOR_CIRRUS) {
            snprintf(line, sizeof(line),
                     "  -> Cirrus Logic codec (CS4208 expected on iMac14,4); "
                     "speaker amp needs EAPD + GPIO0 (see live state below)");
            emit(line);
        }
    }

    // Full output-relevant widget graph for every controller/codec.
    emit("--- HD Audio widget graph (all controllers/codecs) ---");
    hda_devlog_scan(emit);

    if (!hda_state.initialized) return;

    // Live post-configure output-path state (hda_devlog_scan restored + re-armed
    // the winning controller, so codec commands below hit the live codec).
    emit("--- live output-path state (after codec configure + amp-enable) ---");
    hda_codec_t *c = &hda_state.codec;
    uint8_t cad = c->cad;

    // Codec GPIO (CS4208 speaker-amp power gate). GPIO verbs target the AFG node.
    uint32_t gpc  = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_PARAM | HDA_PARAM_GPIO_COUNT);
    uint32_t gm   = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_GPIO_MASK);
    uint32_t gd   = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_GPIO_DIR);
    uint32_t gda  = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_GPIO_DATA);
    snprintf(line, sizeof(line),
             "AFG nid %d GPIO: count=0x%08x  mask=0x%02x dir=0x%02x data=0x%02x  (GPIO0 bit0 = speaker amp)",
             c->fg_nid, gpc, gm & 0xFF, gd & 0xFF, gda & 0xFF);
    emit(line);

    int nr = c->num_out_routes ? c->num_out_routes : 1;
    for (int r = 0; r < nr; r++) {
        uint8_t dac = c->route_dac[r];
        uint8_t pin = c->route_pin[r];
        uint8_t dev = c->route_dev[r];
        uint32_t eapd   = hda_codec_command(cad, pin, HDA_VERB_GET_EAPD);
        uint32_t pinctl = hda_codec_command(cad, pin, HDA_VERB_GET_PIN_CTL);
        uint32_t cfg    = hda_codec_command(cad, pin, HDA_VERB_GET_CONFIG_DEF);
        uint8_t dacg = 0, dacm = 0, ping = 0, pinm = 0;
        hda_audiolog_amp_out(cad, dac, &dacg, &dacm);
        hda_audiolog_amp_out(cad, pin, &ping, &pinm);
        snprintf(line, sizeof(line),
                 "route %d: DAC=%d PIN=%d dev=%s cfg=0x%08x  EAPD=0x%02x(%s) pinctl=0x%02x(out=%d)  "
                 "DACamp gain=0x%02x %s  PINamp gain=0x%02x %s",
                 r, dac, pin, hda_default_device_name(dev), cfg,
                 eapd & 0xFF, (eapd & 0x02) ? "ON" : "off",
                 pinctl & 0xFF, (pinctl & HDA_PIN_OUT_EN) ? 1 : 0,
                 dacg, dacm ? "MUTED" : "unmuted",
                 ping, pinm ? "MUTED" : "unmuted");
        emit(line);
    }

    // Output stream descriptor: did the DMA get set up + does it run?
    uint8_t s = hda_state.out_stream_idx;
    uint32_t ctl  = hda_sd_read32(s, HDA_SD_CTL);
    uint8_t  sts  = hda_sd_read8(s, HDA_SD_STS);
    uint32_t lpib = hda_sd_read32(s, HDA_SD_LPIB);
    uint16_t fmt  = hda_sd_read16(s, HDA_SD_FMT);
    uint32_t cbl  = hda_sd_read32(s, HDA_SD_CBL);
    uint16_t lvi  = hda_sd_read16(s, HDA_SD_LVI);
    snprintf(line, sizeof(line),
             "OUT stream %d: FMT=0x%04x CTL=0x%06x RUN=%d STRM=%u  STS=0x%02x  LPIB=%u CBL=%u LVI=%d  BDL@0x%llx",
             s, fmt, ctl & 0xFFFFFF, (ctl & HDA_SD_CTL_RUN) ? 1 : 0,
             (ctl >> 20) & 0xF, sts, lpib, cbl, lvi, hda_state.bdl_phys);
    emit(line);
    snprintf(line, sizeof(line),
             "BDL set up: %s  DMA buffer @0x%llx size=%u  (RUN + LPIB advancing on the "
             "boot self-tone proves output DMA runs)",
             hda_state.bdl ? "yes" : "NO", hda_state.dma_buffer_phys, hda_state.dma_buffer_size);
    emit(line);
    emit("=== end AUDIOLOG (see /BOOTLOG.TXT for the boot self-tone DMA result) ===");
}
