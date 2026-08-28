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
#include "../fs/bootlog.h"      // #152: audio diagnostics MUST reach /BOOTLOG.TXT
#include "../cpu/mono.h"       // #71: mono_us() - the ONLY clock allowed to time a DMA probe
#include "../sync/noblock.h"   // #71: wq_may_block() before the probe sleeps

// ============================================================================
// #152 LOGGING RULE FOR THIS FILE - READ BEFORE ADDING A DIAGNOSTIC
// ============================================================================
// The ONE machine this driver exists to support, the owner's iMac14,4 with the
// Cirrus CS4208, HAS NO SERIAL CONSOLE. kprintf() goes to serial. Therefore
// every kprintf() in this file was, on the target hardware, a diagnostic that
// produced NOTHING.
//
// This was not a theoretical problem. The real-hardware capture taken on
// 2026-08-14 (the build host:<workspace>, build 1902,
// 287 lines) contains xHCI, HID, ASIX, USB-hub, auth and session lines and
// ZERO lines about audio - not the controller probe, not STATESTS, not the
// codec identity, not the output-DMA check. Whether HDA even ran on that boot
// was unknowable from the only log the machine produces. The identical trap is
// documented for cpu/smp.c (40 kprintf, 0 bootlog_write).
//
// So: DECISION-PATH diagnostics in this file go through bootlog_write(), which
// per fs/bootlog.h ALSO mirrors every line to the serial kprintf log. It is a
// strict superset of kprintf, not a replacement for it, and it takes no
// trailing "\n" (bootlog_write appends one).
//
// The budget matters: bootlog_write() rewrites the whole growing /BOOTLOG.TXT
// on every armed call, so this is for CHECKPOINTS, not per-widget tracing. The
// always-on audio path below emits on the order of ten lines. Per-widget graph
// dumping stays where it already is: behind the /CONFIG/AUDIODMP.CFG opt-in
// gate, writing to the separate /AUDIOLOG.TXT.
#define hda_log(...) bootlog_write(__VA_ARGS__)

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

// #189: the ONE sine table in the driver, published rather than copied. The
// tail probe in audio.c needs a tone and the project rule is to reuse the
// existing primitive, not to grow a second table that can drift from this one.
const int16_t *hda_sine64_table(void) { return hda_sine64; }

// ============================================================================
// #189: the FFI to the starve-silence arithmetic (rustkern/hdastarve.rs).
//
// The DECISIONS are Rust; the MMIO reads and the memset/memcpy are C, which is
// the same split hda_dma_verdict_rs() and hda_start_verdict_rs() already use in
// this file. Nothing here is a float and nothing here allocates.
// ============================================================================
#ifdef RUST_HDA_STARVE
#define HDA_STARVE_ZERO_PER_PASS 4u
extern uint64_t hda_starve_advance_rs(uint64_t dma_slot, uint32_t last_slot,
                                      uint32_t cur_slot, uint32_t nbdl);
extern uint32_t hda_starve_zero_count_rs(uint64_t sil_slot, uint64_t dma_slot,
                                         uint32_t max_per_pass);
extern uint64_t hda_starve_sil_floor_rs(uint64_t sil_slot, uint64_t dma_slot,
                                        uint32_t nbdl);
extern uint64_t hda_starve_resync_rs(uint64_t wr_slot, uint64_t dma_slot);
extern uint32_t hda_starve_avail_slots_rs(uint64_t wr_slot, uint64_t dma_slot,
                                          uint64_t sil_slot, uint32_t nbdl);
extern uint32_t hda_starve_selftest_rs(void);
// AUDLEAD: starvation accounting (pure, same counters the repair path uses).
extern uint64_t hda_starve_deficit_rs(uint64_t wr_slot, uint64_t dma_slot);
extern uint64_t hda_starve_lead_rs(uint64_t wr_slot, uint64_t dma_slot);
extern uint32_t hda_starve_stat_selftest_rs(void);
#endif

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

    hda_log("[HDA] Controller reset failed");
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
    hda_log("[HDA] CORB/RIRB setup complete");
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
    // Pass 1b (#71): a machine with internal speakers almost always ALSO has a
    // headphone jack, and until now the fixed-speaker early-return meant the HP
    // pin was never powered, never output-enabled, never EAPD'd, never unmuted,
    // and - fatally - its DAC was never given a stream tag. Plugging headphones
    // into the iMac would therefore have produced exactly the same silence, for
    // exactly the same class of reason: a second silent path, the same bug
    // twice. So append every jack-connected HP-Out pin as a further parallel
    // route. hda_route_start_channel() gives every route past the first the
    // copy-front channel 0 on a 2-channel stream (#152), so the headphone DAC
    // gets the same stereo pair the speakers do.
    //
    // Deliberately additive: this branch is only reachable when pass 1 already
    // found fixed speakers, i.e. never on the QEMU codec (which has no
    // Speaker/Fixed pin at all and falls through to pass 2 unchanged).
    if (nroutes > 0) {
        for (int nid = c->start_nid;
             nid < c->start_nid + c->num_nodes && nroutes < HDA_MAX_OUT_ROUTES; nid++) {
            if (hda_widget_type(cad, nid) != HDA_WIDGET_TYPE_PIN) continue;
            uint32_t pincap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_PIN_CAP);
            if (pincap == 0xFFFFFFFF || !(pincap & HDA_PINCAP_OUT)) continue;
            uint32_t cfg = hda_codec_command(cad, nid, HDA_VERB_GET_CONFIG_DEF);
            uint8_t conn = (cfg >> 30) & 0x3;                     // 1 == No physical connection
            uint8_t dev  = (cfg >> 20) & 0xF;                     // 2 == HP-Out
            if (dev != 0x2 || conn == 0x1) continue;              // real headphone jacks only
            int dup = 0;
            for (int k = 0; k < nroutes; k++) if (c->route_pin[k] == (uint8_t)nid) dup = 1;
            if (dup) continue;
            uint8_t dac, pin_conn, mix_nid, mix_conn, mix_is_sel;
            if (!hda_resolve_pin_route(cad, (uint8_t)nid, &dac, &pin_conn,
                                       &mix_nid, &mix_conn, &mix_is_sel))
                continue;
            hda_store_route(c, nroutes, (uint8_t)nid, dev, dac, pin_conn,
                            mix_nid, mix_conn, mix_is_sel);
            nroutes++;
        }
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

// #152: which stream channel does route `r` start at?
//
// This replaces `channel = r`, which was wrong for the exact hardware it was
// written for. Per the HDA spec the Set-Converter-Stream/Channel payload's
// channel field is the FIRST channel of the stream that this converter
// consumes, and the converter then consumes as many consecutive channels as its
// Converter Format says. Every route DAC is programmed with the SAME format as
// the stream (2 channels), so telling route 1 to start at channel 1 asked it for
// channels 1 and 2 of a stream that only has channels 0 and 1. Channel 2 does
// not exist.
//
// Linux does not do that. sound/pci/hda/hda_codec.c
// snd_hda_multi_out_analog_prepare() assigns the secondary DACs like this:
//
//     for (i = 1; i < mout->num_dacs; i++) {
//         if (chs >= (i + 1) * 2)  // enough channels for a real independent out
//             snd_hda_codec_setup_stream(codec, nids[i], stream_tag, i * 2, format);
//         else                     // copy front
//             snd_hda_codec_setup_stream(codec, nids[i], stream_tag, 0, format);
//     }
//
// so on a 2-channel stream with two DACs, BOTH get channel 0 and the full stereo
// format ("copy front"), and both speakers reproduce the same stereo pair. The
// i*2 branch only engages once the stream genuinely carries 4+ channels.
//
// Consequence for the iMac14,4: routes 0 and 1 (speaker pins nid29/nid30, DACs
// nid10/nid11) both take channel 0. This is a behaviour change from #390's
// stereo split; #390's intent was right but its encoding asked the codec for a
// channel outside the stream. Restoring per-speaker stereo separation, if the
// hardware really is two mono speakers, is a 4-channel-stream question, not a
// start-channel question, and is deliberately NOT attempted here.
static uint8_t hda_route_start_channel(int r) {
    if (r <= 0) return 0;
    uint32_t chs = hda_state.channels ? hda_state.channels : 2;
    if (chs >= (uint32_t)(r + 1) * 2) return (uint8_t)(r * 2);
    return 0;   // copy front
}

// #152: program one route DAC's Converter Stream/Channel, and enable the
// converter's digital output if the widget declares itself digital.
//
// The digital half is new and is a direct consequence of reading the real
// hardware dump rather than the QEMU one. The iMac's two internal-speaker
// converters report wcap=0x00046631, and bit 9 of Audio Widget Capabilities is
// the Digital bit. A digital converter's output is gated by DigEn in Digital
// Converter Control 1, which nothing in this driver had ever sent, so the
// converters were being handed a stream tag and a format while their output
// stayed switched off. The QEMU codec's DACs are not digital, which is why no
// VM could ever have shown this.
//
// Issued on the capability bit, not on the vendor ID, because that is what the
// bit means; on a codec whose converters are analog (QEMU, AC97-era parts) the
// wcap test is false and nothing extra is sent, so the known-good path is byte
// for byte unchanged.
// `do_log` is 0 on the runtime reconfigure path (hda_configure(), reached on
// every tone/format change) so that a re-rate cannot turn /BOOTLOG.TXT into a
// rewrite-per-call treadmill; the boot path logs once per route, which is the
// evidence we actually need.
static void hda_setup_route_converter(uint8_t cad, int r, uint8_t dac, int do_log) {
    uint8_t ch = hda_route_start_channel(r);
    hda_codec_command(cad, dac, HDA_VERB_SET_STREAM | (1 << 4) | (ch & 0x0F));

    uint32_t wcap = hda_codec_command(cad, dac,
                                      HDA_VERB_GET_PARAM | HDA_PARAM_AUDIO_WIDGET_CAP);
    int digital = (wcap != 0xFFFFFFFF) && (wcap & HDA_WCAP_DIGITAL);
    if (digital) {
        hda_codec_command(cad, dac, HDA_VERB_SET_DIGI_CONV1 | HDA_DIG1_ENABLE);
    }
    if (!do_log) return;
    hda_log("[HDA] route %d DAC=%d: stream=1 channel=%d wcap=0x%08x%s%s%s",
            r, dac, ch, wcap,
            digital ? " DIGITAL(DigEn sent)" : " analog",
            (wcap & HDA_WCAP_OUT_AMP) ? " out-amp" : " no-out-amp",
            (wcap & HDA_WCAP_STEREO)  ? " stereo"  : " mono");
}

// ============================================================================
// #71 THE ANALOGUE ENABLES: amp gain, and EAPD. READ THIS BEFORE EDITING.
// ============================================================================
//
// The iMac14,4 /AUDIOLOG.TXT capture (golden 1925, Cirrus CS4208) showed the
// whole digital path healthy - codec picked at score 100, both speaker routes
// resolved, converter format set, stream tagged, LPIB advancing - and both
// analogue enables reading zero:
//
//   route 0: DAC=10 PIN=29 dev=Speaker  EAPD=0x00(off) ... DACamp gain=0x00 ...
//
// Two distinct things are being reported there, and they fail for different
// reasons, so they get separate helpers with separate readbacks.
//
// GAIN. Verb 0x300's gain field is an INDEX into the widget's amp ladder, not a
// linear volume, and index 0 is FULL ATTENUATION. "Unmuted at gain 0" is
// silence. The correct index is not a magic number and must never be one: the
// Output Amplifier Capabilities parameter (0x12) reports Offset, the index the
// part defines as 0 dB, and Num Steps, the top of the ladder. We program
// Offset when the codec publishes one (that is the level the silicon was
// calibrated for), and Num Steps when it publishes Offset == 0 (a ladder with
// no 0 dB reference tops out at unity). Worked example from this codec's
// DACs 2..5: cap 0x80017f7f -> Num Steps 0x7f, Offset 0x7f -> gain 0x7f.
//
// A widget with no Amp Param Override reports its caps as the FUNCTION GROUP's
// defaults; some codecs return 0 for the widget's own query in that case, and
// the old code then returned without writing anything, leaving the amp at
// whatever the reset value was. So we fall back to the AFG's caps rather than
// giving up.
//
// A widget with the out-amp bit CLEAR in its Audio Widget Capabilities has no
// amp at all. There is nothing to set and nothing to read; a 0 in the log for
// such a widget is a correct report of "no gain control here", not a fault.
// Distinguishing those two cases is the whole point of the return code.
//
// EAPD. External Amplifier Power Down. The bit is "amp POWERED", so clear
// means the external amplifier is shut down. It is implemented per PIN and
// ONLY on pins that advertise Pin Capabilities bit 16. Sending it to a pin
// without that bit is a no-op the codec is free to ignore, which is precisely
// why a blanket send followed by no readback taught us nothing for a year.
//
// Every one of these helpers WRITES THEN READS BACK. A verb this driver sends
// and never reads is not evidence of anything.

static const char *hda_default_device_name(uint8_t d);   // defined with the devlog dump

// Fetch a widget's effective output-amp capabilities, following the HDA rule
// that a widget without Amp Param Override inherits the function group's.
// Returns 0 if the widget has no output amp at all.
static uint32_t hda_out_amp_cap(uint8_t cad, uint8_t nid, uint8_t fg_nid) {
    uint32_t wcap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AUDIO_WIDGET_CAP);
    if (wcap == 0xFFFFFFFF || !(wcap & HDA_WCAP_OUT_AMP)) return 0;
    uint32_t cap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AMP_OUT_CAP);
    if (cap == 0xFFFFFFFF) cap = 0;
    if (cap == 0 && fg_nid) {   // no override published: inherit the AFG default
        cap = hda_codec_command(cad, fg_nid, HDA_VERB_GET_PARAM | HDA_PARAM_AMP_OUT_CAP);
        if (cap == 0xFFFFFFFF) cap = 0;
    }
    return cap;
}

// Gain index for a capability word: the codec's own 0 dB offset, else the top
// of its ladder. Never a literal.
static uint8_t hda_amp_gain_from_cap(uint32_t cap) {
    uint8_t nsteps = (uint8_t)HDA_AMPCAP_NUMSTEPS(cap);
    uint8_t offset = (uint8_t)HDA_AMPCAP_OFFSET(cap);
    uint8_t gain   = offset ? offset : nsteps;
    if (gain > nsteps) gain = nsteps;
    return gain;
}

// Set a widget's OUTPUT amp unmuted at its calibrated 0 dB index, then READ IT
// BACK. *cap/*gain/*mute are always written (zeroed when absent) so the caller
// can log the truth either way.
// Returns HDA_AMP_R_ABSENT / HDA_AMP_R_MISMATCH / HDA_AMP_R_OK.
static int hda_set_out_amp_rb(uint8_t cad, uint8_t nid, uint8_t fg_nid,
                              uint32_t *cap_out, uint8_t *gain_out, uint8_t *mute_out) {
    if (cap_out) *cap_out = 0;
    if (gain_out) *gain_out = 0;
    if (mute_out) *mute_out = 0;

    uint32_t cap = hda_out_amp_cap(cad, nid, fg_nid);
    if (cap_out) *cap_out = cap;
    if (cap == 0) return HDA_AMP_R_ABSENT;

    uint8_t gain = hda_amp_gain_from_cap(cap);
    hda_codec_command(cad, nid, HDA_VERB_SET_AMP_GAIN |
        HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | gain);

    uint32_t rb = hda_codec_command(cad, nid, HDA_VERB_GET_AMP_GAIN | 0x8000); // out, left
    if (rb == 0xFFFFFFFF) rb = 0;
    uint8_t rg = rb & HDA_AMP_GAIN_MASK, rm = (rb >> 7) & 1;
    if (gain_out) *gain_out = rg;
    if (mute_out) *mute_out = rm;
    return (rg == gain && !rm) ? HDA_AMP_R_OK : HDA_AMP_R_MISMATCH;
}

// Thin wrapper for callers that do not want the readback detail.
static void hda_set_out_amp(uint8_t cad, uint8_t nid) {
    hda_set_out_amp_rb(cad, nid, hda_state.codec.fg_nid, NULL, NULL, NULL);
}

// Set a widget's INPUT amp (at connection index) unmuted at its 0 dB index.
static void hda_set_in_amp(uint8_t cad, uint8_t nid, uint8_t index) {
    uint32_t wcap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AUDIO_WIDGET_CAP);
    if (wcap == 0xFFFFFFFF || !(wcap & HDA_WCAP_IN_AMP)) return;
    uint32_t cap = hda_codec_command(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AMP_IN_CAP);
    if (cap == 0xFFFFFFFF) cap = 0;
    if (cap == 0 && hda_state.codec.fg_nid) {
        cap = hda_codec_command(cad, hda_state.codec.fg_nid,
                                HDA_VERB_GET_PARAM | HDA_PARAM_AMP_IN_CAP);
        if (cap == 0xFFFFFFFF) cap = 0;
    }
    if (cap == 0) return;
    hda_codec_command(cad, nid, HDA_VERB_SET_AMP_GAIN |
        HDA_AMP_SET_INPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
        ((index << 8) & HDA_AMP_SET_INDEX_MASK) | hda_amp_gain_from_cap(cap));
}

// #71 Power the external amplifier on ONE pin, capability-gated, with readback.
//
//   returns  1  pin is EAPD-capable, verb sent, readback confirms EAPD ON
//            0  pin is EAPD-capable, verb sent, readback still shows it OFF
//           -1  pin is NOT EAPD-capable (Pin Capabilities bit 16 clear): no
//               verb sent, and the 0 a later GET returns is not a fault
//
// The BTL and L/R-swap bits in the same payload are read-modify-written rather
// than cleared, because clearing a codec's balanced-output configuration while
// trying to switch its amp on would be a new bug wearing the fix's clothes.
static int hda_pin_eapd_on(uint8_t cad, uint8_t nid, uint32_t pincap, uint8_t *rb_out) {
    if (rb_out) *rb_out = 0;
    if (!(pincap & HDA_PINCAP_EAPD)) return -1;

    uint32_t cur = hda_codec_command(cad, nid, HDA_VERB_GET_EAPD);
    if (cur == 0xFFFFFFFF) cur = 0;
    uint32_t want = (cur & HDA_EAPD_MASK) | HDA_EAPD_ENABLE;
    hda_codec_command(cad, nid, HDA_VERB_SET_EAPD | want);

    uint32_t rb = hda_codec_command(cad, nid, HDA_VERB_GET_EAPD);
    if (rb == 0xFFFFFFFF) rb = 0;
    if (rb_out) *rb_out = (uint8_t)(rb & 0xFF);
    return (rb & HDA_EAPD_ENABLE) ? 1 : 0;
}

// ============================================================================
// #71 THE ANALOGUE HALF, IN ONE PLACE, ALWAYS WITH READBACK
// ============================================================================
//
// Everything between "the DMA engine is running" and "a speaker moves": pin
// output enable, headphone drive amp, EAPD external-amplifier power, output amp
// gain/mute on every widget of every route, and the Cirrus GPIO0 speaker-amp
// gate. It is idempotent and cheap, so both the boot configure path AND the
// /CONFIG/AUDIOTONE.CFG boot tone call it: the owner's audible test therefore
// exercises exactly the code being fixed, and reports what it found.
//
// `do_log` writes one bootlog line per route. Callers on the per-tone/per-format
// reconfigure path pass 0 so a re-rate cannot turn /BOOTLOG.TXT into a
// rewrite-per-call treadmill (bootlog_write rewrites the whole file each call).
static void hda_analog_enables_apply(int do_log) {
    hda_codec_t *c = &hda_state.codec;
    uint8_t cad = c->cad;
    int nroutes = c->num_out_routes ? c->num_out_routes : 1;

    for (int r = 0; r < nroutes; r++) {
        uint8_t dac = c->route_dac[r];
        uint8_t pin = c->route_pin[r];
        uint8_t mix = c->route_mixn[r];
        uint8_t dev = c->route_dev[r];

        uint32_t pincap = hda_codec_command(cad, pin, HDA_VERB_GET_PARAM | HDA_PARAM_PIN_CAP);
        if (pincap == 0xFFFFFFFF) pincap = 0;

        uint8_t pinctl = HDA_PIN_OUT_EN;                     // bit 6 output-enable
        // #71: drive the headphone amp on any pin that HAS one, not only on one
        // whose config-default device happens to say HP-Out. The two are
        // independent: pincap bit 3 is the pin's own statement that it has a
        // headphone drive amplifier.
        if (dev == 0x2 || (pincap & HDA_PINCAP_HP_DRV)) pinctl |= HDA_PIN_HP_EN;
        hda_codec_command(cad, pin, HDA_VERB_SET_PIN_CTL | pinctl);
        uint32_t pinctl_rb = hda_codec_command(cad, pin, HDA_VERB_GET_PIN_CTL);
        if (pinctl_rb == 0xFFFFFFFF) pinctl_rb = 0;

        uint8_t eapd_rb = 0;
        int eapd_r = hda_pin_eapd_on(cad, pin, pincap, &eapd_rb);

        if (mix) hda_set_out_amp(cad, mix);
        uint32_t dcap = 0, pcap_amp = 0;
        uint8_t dg = 0, dm = 0, pg = 0, pm = 0;
        int damp = hda_set_out_amp_rb(cad, dac, c->fg_nid, &dcap, &dg, &dm);
        int pamp = hda_set_out_amp_rb(cad, pin, c->fg_nid, &pcap_amp, &pg, &pm);

        if (!do_log) continue;

        // OBSERVABILITY. This line is the whole reason the fault survived a
        // year: nothing in any log distinguished "playing into a powered-down
        // amp" from "playing". Every value here is a READBACK taken from the
        // codec after the write, not a restatement of what we asked for, and it
        // goes through bootlog_write because the iMac has no serial console.
        hda_log("[HDA] route %d %s PIN=%d pincap=0x%08x pinctl=0x%02x(out=%d,hp=%d) "
                "EAPD=%s(0x%02x) DAC=%d dacamp=%s(0x%02x%s) pinamp=%s(0x%02x%s)",
                r, hda_default_device_name(dev), pin, pincap,
                pinctl_rb & 0xFF, (pinctl_rb & HDA_PIN_OUT_EN) ? 1 : 0,
                (pinctl_rb & HDA_PIN_HP_EN) ? 1 : 0,
                eapd_r < 0 ? "NOT-CAPABLE" : (eapd_r ? "ON" : "SENT-BUT-OFF"), eapd_rb,
                dac,
                damp < 0 ? "no-amp" : (damp ? "ok" : "MISMATCH"), dg, dm ? " MUTED" : "",
                pamp < 0 ? "no-amp" : (pamp ? "ok" : "MISMATCH"), pg, pm ? " MUTED" : "");

        if (damp == HDA_AMP_R_ABSENT && pamp == HDA_AMP_R_ABSENT) {
            // Not a failure. Some parts put no gain control on either widget of a
            // path and run it at a fixed level. Say so, or the two 0x00s above
            // read as the bug they are not.
            hda_log("[HDA] route %d: neither DAC %d nor PIN %d has an output amp: "
                    "no gain control on this path, it runs at its fixed level",
                    r, dac, pin);
        }
    }

    // #71 Cirrus CS4208 (Apple iMac14,4 internal speakers): on this codec NO pin
    // implements EAPD (every pin reports Pin Capabilities bit 16 clear), and the
    // internal-speaker power amplifier is gated by a codec GPIO instead.
    // Mirroring Linux sound/pci/hda/patch_cirrus.c cs4208_fixup_gpio0()
    // (gpio_eapd_speaker = bit0, gpio_mask = gpio_dir = data = 0x01), drive
    // GPIO0 high on the AFG root node. QEMU's emulated codec needs none of this,
    // so it stays gated on the Cirrus vendor ID and the known-good VM path is
    // byte for byte unchanged.
    //
    // The values are now READ BACK. The old line printed the constant 0x01 it
    // had just asked for, which is a restatement of intent, not a witness. With
    // no pin EAPD anywhere on this part, this readback is the single most
    // load-bearing number in the entire analogue path.
    if (c->vendor_id == HDA_VENDOR_CIRRUS) {
        uint32_t gpc = hda_codec_command(cad, c->fg_nid,
                                         HDA_VERB_GET_PARAM | HDA_PARAM_GPIO_COUNT);
        hda_codec_command(cad, c->fg_nid, HDA_VERB_SET_GPIO_MASK | 0x01);
        hda_codec_command(cad, c->fg_nid, HDA_VERB_SET_GPIO_DIR  | 0x01);
        hda_codec_command(cad, c->fg_nid, HDA_VERB_SET_GPIO_DATA | 0x01);
        hda_delay(1000);
        uint32_t gm = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_GPIO_MASK);
        uint32_t gd = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_GPIO_DIR);
        uint32_t gv = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_GPIO_DATA);
        if (do_log) {
            hda_log("[HDA] Cirrus %04x:%04x GPIO0 speaker-amp: count=0x%08x "
                    "readback mask=0x%02x dir=0x%02x data=0x%02x -> amp %s",
                    c->vendor_id, c->device_id, gpc,
                    gm & 0xFF, gd & 0xFF, gv & 0xFF,
                    ((gm & 1) && (gd & 1) && (gv & 1)) ? "POWERED" : "NOT POWERED");
        }
    }
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

        // Power the widgets on this path (D0).
        hda_codec_command(cad, dac, HDA_VERB_SET_PS | 0x00);
        hda_codec_command(cad, pin, HDA_VERB_SET_PS | 0x00);
        if (mix) hda_codec_command(cad, mix, HDA_VERB_SET_PS | 0x00);
        hda_delay(1000);

        // DAC: same stream tag (1); start channel per the Linux copy-front rule,
        // plus DigEn for a digital converter (#152). Unmute amp.
        // (Converter format is set on every route DAC in hda_init.)
        hda_setup_route_converter(cad, r, dac, 1);
        hda_set_out_amp(cad, dac);

        // Intermediate mixer/selector: route to the DAC and unmute.
        if (mix) {
            if (c->route_mixsel[r])
                hda_codec_command(cad, mix, HDA_VERB_SET_CONN_SELECT | c->route_mixc_[r]);
            hda_set_in_amp(cad, mix, c->route_mixc_[r]);
            hda_set_out_amp(cad, mix);
        }

        // Pin: select which of its inputs feeds it. Everything else about the pin
        // (output enable, EAPD, amps) is the ANALOGUE half and lives in
        // hda_analog_enables_apply() below, so there is exactly one copy of it
        // and the boot tone can re-assert and re-report it.
        hda_codec_command(cad, pin, HDA_VERB_SET_CONN_SELECT | c->route_pinc[r]);
    }

    hda_analog_enables_apply(1);

    hda_log("[HDA] Configured %d output route(s) (%s score=%d)",
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

    hda_log("[HDA] DMA buffers allocated: BDL@0x%llx, Buffer@0x%llx size=%u",
            hda_state.bdl_phys, hda_state.dma_buffer_phys,
            hda_state.dma_buffer_size);

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

#ifdef RUST_HDA_STARVE
    // #189: SRST has just reset the descriptor, so LPIB is back at 0 and every
    // free-running counter derived from it is now describing a ring that no
    // longer exists. Reset them together with the hardware, or the first
    // hda_starve_advance_rs() after this would read the drop from slot 31 to
    // slot 0 as a lap and the accounting would be a whole ring out.
    hda_state.dma_slot       = 0;
    hda_state.wr_slot        = 0;
    hda_state.sil_slot       = 0;
    hda_state.last_dma_slot  = 0;
    // AUDLEAD: per-stream starvation accounting. lead_min_slots starts at the
    // maximum so the first healthy observation replaces it; a stream that never
    // ran leaves it at UINT64_MAX, which the reporter prints as "no data"
    // rather than as a perfect score.
    hda_state.starve_armed   = false;
    hda_state.starve_events  = 0;
    hda_state.starve_slots   = 0;
    hda_state.lead_obs       = 0;
    hda_state.lead_min_slots = 0xFFFFFFFFFFFFFFFFULL;
    hda_state.loop_mode      = false;
#endif

    // #173: this function has just cleared RUN twice over (explicitly, then
    // again via the SRST cycle, which resets the descriptor to defaults). The
    // stream is definitively STOPPED on exit, so the shadow must say so. It
    // used to be left alone, and any caller that reconfigured a running stream
    // left `playing` true over a stopped engine - after which hda_start()
    // returned early and never re-asserted RUN. See hda_start().
    hda_state.playing = false;

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

#ifdef RUST_HDA_STARVE
// #189: sample LPIB and carry the free-running slot counter across ring laps.
// MUST be called with g_hda_svc_lock held, and MUST be reached more often than
// once per ring lap (~0.7 s here) or a whole lap would be lost silently. Both
// service paths run at >= 100 Hz, so that holds by construction.
static void hda_starve_track_locked(void) {
    if (hda_state.bdl_buffer_size == 0) return;
    uint32_t lpib = hda_sd_read32(hda_state.out_stream_idx, HDA_SD_LPIB);
    uint32_t cur  = lpib / hda_state.bdl_buffer_size;
    if (cur >= HDA_NUM_BDL) cur = HDA_NUM_BDL - 1;   // LPIB >= CBL: clamp, do
                                                     // not wrap into lap 0
    hda_state.dma_slot = hda_starve_advance_rs(hda_state.dma_slot,
                                               hda_state.last_dma_slot,
                                               cur, HDA_NUM_BDL);
    hda_state.last_dma_slot = cur;
}

// #189: THE FIX. Zero the slots the DMA has finished, so the ring plays each
// buffer ONCE instead of forever.
//
// WHY THIS AND NOT "STOP THE ENGINE ON UNDERRUN": stopping is a state change
// that every future sound then has to undo, and the restart is neither free nor
// silent (SDnCTL RUN toggling, and on this driver a start also carries the #173
// verdict/recovery path). It would also fire on a momentary scheduler hiccup
// mid-song, turning a 10 ms late producer into an audible stop/start. Zeroing
// costs one bounded memset per service pass, is invisible during healthy
// playback (the slots being zeroed have already been played), and leaves the
// engine free-running so the next sound starts within one slot.
//
// WHY NOT "ZERO THE WHOLE RING WHEN A STARVE IS DETECTED": that needs a
// starvation EDGE to be detected reliably, and a 128 KB memset with interrupts
// off. This form needs no edge at all - it is unconditional and therefore has
// no state machine to get wrong - and its work per pass is bounded.
//
// MUST be called with g_hda_svc_lock held. Bounded to HDA_STARVE_ZERO_PER_PASS
// slots because the interrupt handler reaches this too.
static void hda_starve_silence_locked(void) {
    if (!hda_state.dma_buffer || hda_state.bdl_buffer_size == 0) return;
    if (hda_state.loop_mode) return;      // a test tone is MEANT to repeat

    uint64_t floored = hda_starve_sil_floor_rs(hda_state.sil_slot,
                                               hda_state.dma_slot, HDA_NUM_BDL);
    if (floored != hda_state.sil_slot) {
        hda_state.silence_skips += (floored - hda_state.sil_slot);
        hda_state.sil_slot = floored;
    }

    uint32_t n = hda_starve_zero_count_rs(hda_state.sil_slot, hda_state.dma_slot,
                                          HDA_STARVE_ZERO_PER_PASS);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t s = (uint32_t)(hda_state.sil_slot % HDA_NUM_BDL);
        memset((uint8_t *)hda_state.dma_buffer + (size_t)s * hda_state.bdl_buffer_size,
               0, hda_state.bdl_buffer_size);
        hda_state.sil_slot++;
        hda_state.silenced_slots++;
    }
}
#endif

#ifdef RUST_HDA_STARVE
// AUDLEAD: RECORD WHAT THE REPAIR PATH ALREADY KNEW.
//
// Call this with g_hda_svc_lock held, IMMEDIATELY BEFORE
// hda_starve_resync_rs(), because resync is what destroys the evidence: it
// moves wr_slot back in front of the head, after which nothing can tell that
// the producer was ever overrun.
//
// WHY THIS IS NOT FOLDED INTO resync: resync is called on paths where being
// behind the head is normal and not a fault (the first write of a stream, and
// any write after a stop), so the repair must stay unconditional while the
// STATISTIC must not. hda_state.starve_armed is that distinction: nothing is
// counted until a producer has actually written a slot into this stream.
//
// WHY IT COUNTS A HEALTHY LEAD TOO: an underrun count on its own cannot tell
// "we have 250 ms of margin and lost it" from "we never had any margin". The
// minimum lead is what says which, and it is the number a buffering change is
// supposed to move.
static void hda_starve_account_locked(void) {
    if (!hda_state.starve_armed) return;
    // ONLY WHILE THE ENGINE IS ACTUALLY CONSUMING. Two windows would otherwise
    // pollute the minimum-lead figure with leads that carry no risk at all:
    //
    //   - the PREFILL ramp, where the producer is deliberately filling a
    //     STOPPED ring from empty, so the lead legitimately passes through 1, 2,
    //     3 ... slots on its way up and nothing is being played;
    //   - the tail after audio_stop(), where the ring is meant to drain.
    //
    // Reporting either as "minimum lead" would understate the margin by an order
    // of magnitude and make a real regression indistinguishable from a normal
    // start. A statistic that is misleading in the ordinary case is the same
    // defect this accounting was added to fix.
    if (!hda_state.playing) return;

    uint64_t deficit = hda_starve_deficit_rs(hda_state.wr_slot, hda_state.dma_slot);
    if (deficit) {
        hda_state.starve_events++;
        hda_state.starve_slots += deficit;
        hda_state.lead_min_slots = 0;
    } else {
        uint64_t lead = hda_starve_lead_rs(hda_state.wr_slot, hda_state.dma_slot);
        if (lead < hda_state.lead_min_slots) hda_state.lead_min_slots = lead;
    }
    hda_state.lead_obs++;
}
#endif

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

#ifdef RUST_HDA_STARVE
    // #189: unconditional, on BOTH service paths. Deliberately not gated on
    // BCIS: on the real iMac the MSI never arms and BCIS is only ever observed
    // by the 10 ms poll worker, so anything gated on the interrupt is gated on
    // the one machine that matters not having it.
    hda_starve_track_locked();
    hda_starve_silence_locked();
#endif

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
#ifdef RUST_HDA_STARVE
        // #189: like hda_selftest_tone(), this fills the whole ring on purpose
        // and relies on the engine repeating it. Stand the starve silencer down
        // until a real producer writes.
        hda_state.loop_mode = true;
#endif
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
        hda_log("[HDA] #71: could not re-find winning controller for MSI setup");
        return;
    }

    idt_register_handler(HDA_MSI_VECTOR, hda_msi_isr);

    uint32_t dest = lapic_get_id();   // target the BSP
    if (pci_enable_msi(dev, HDA_MSI_VECTOR, dest)) {
        hda_state.msi_enabled = true;
        hda_state.msi_vector = HDA_MSI_VECTOR;
        hda_log("[HDA] #71: MSI armed on vector 0x%02x -> LAPIC %u "
                "(controller %04x:%04x, legacy PCI IRQ line was %u)",
                HDA_MSI_VECTOR, dest, hda_state.vendor_id, hda_state.device_id,
                hda_state.irq);
    } else {
        hda_log("[HDA] #71: controller has no MSI capability; relying on "
                "LPIB poll worker only (legacy PCI IRQ line=%u)", hda_state.irq);
    }
}

// ============================================================================
// Public API
// ============================================================================

// Point the register helpers at a controller and bring CORB/RIRB up on it.
// Returns AUDIO_OK if the controller reset and its command ring is running.
// #152: Intel PCH host-controller PCI-config quirks, mirroring Linux
// sound/pci/hda/hda_intel.c intel_init_pci(). See the register comments in
// hda.h for why each one matters. Both registers live in PCI CONFIG space, not
// in the MMIO BAR, so no amount of work on the register-poking path in this
// file could ever have reached them.
//
// Why this is a live suspect for the iMac silence rather than mere box-ticking:
// the 2026-07-11 real-hardware AUDIOLOG proves the CS4208 codec side is fully
// and correctly configured (routes found, pins output-enabled, DACs stream-
// tagged, GPIO0 speaker amp powered), which leaves the CONTROLLER-to-memory DMA
// path as the remaining place for the audio to disappear. NO SNOOP is precisely
// a "DMA runs, LPIB advances, the bytes fetched are stale" failure, and it is
// invisible under QEMU, whose emulated DMA is always coherent.
//
// Safe on non-Intel and on QEMU: gated on the Intel vendor ID, and both writes
// are idempotent read-modify-writes of documented bits.
static void hda_intel_pci_quirks(pci_device_t *dev) {
    if (dev->vendor_id != HDA_PCI_VENDOR_INTEL) return;

    uint8_t tcsel = pci_read8(dev->bus, dev->slot, dev->func, HDA_PCI_TCSEL);
    if (tcsel & HDA_PCI_TCSEL_MASK) {
        pci_write8(dev->bus, dev->slot, dev->func, HDA_PCI_TCSEL,
                   (uint8_t)(tcsel & ~HDA_PCI_TCSEL_MASK));
    }

    uint16_t devc = pci_read16(dev->bus, dev->slot, dev->func, HDA_PCI_DEVC);
    if (devc & HDA_PCI_DEVC_NOSNOOP) {
        pci_write16(dev->bus, dev->slot, dev->func, HDA_PCI_DEVC,
                    (uint16_t)(devc & ~HDA_PCI_DEVC_NOSNOOP));
    }

    // Read BOTH back. A register that does not accept the write is a different
    // fault from one that was already correct, and only the read-back can tell
    // them apart on a machine we cannot attach a debugger to.
    uint8_t  tcsel_after = pci_read8(dev->bus, dev->slot, dev->func, HDA_PCI_TCSEL);
    uint16_t devc_after  = pci_read16(dev->bus, dev->slot, dev->func, HDA_PCI_DEVC);
    hda_log("[HDA] Intel PCI quirks %02x:%02x.%x: TCSEL 0x%02x->0x%02x  "
            "DEVC 0x%04x->0x%04x (NOSNOOP %s)",
            dev->bus, dev->slot, dev->func, tcsel, tcsel_after, devc, devc_after,
            (devc_after & HDA_PCI_DEVC_NOSNOOP) ? "STILL SET - DMA may read stale cache"
                                                : "clear - DMA snoops CPU caches");
}

static int hda_bring_up_controller(pci_device_t *dev) {
    pci_enable_bus_master(dev);
    hda_intel_pci_quirks(dev);
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
#ifdef RUST_HDA_STARVE
    // #189: the ring is still the kzalloc'd zeros here, so there is nothing to
    // silence; only the counters need to agree with the register.
    hda_state.dma_slot = hda_state.wr_slot = hda_state.sil_slot = 0;
    hda_state.last_dma_slot = 0;
    hda_state.starve_armed = false;     // AUDLEAD: a probe is not a producer
#endif
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
    // #173: this touched RUN behind hda_start()'s back. Keep the shadow honest.
    hda_state.playing = false;
    // #152: THIS is the line that has been missing from every real-hardware
    // capture. The 2026-07-11 CS4208 analysis named "did the output DMA
    // actually advance?" as the single remaining unknown for #71 and pointed at
    // BOOTLOG for the answer; the answer was being kprintf'd to a serial port
    // the iMac does not have, so it was never once observed on that machine.
    hda_log("[HDA] output DMA check: LPIB %u -> %u : %s (stream=%u fmt=0x%04x)",
            lp0, lpn, moved ? "RUNNING (DMA advances)" : "STALLED",
            s, hda_state.stream_format);
    return moved;
}

// #173: CAN THE OUTPUT STREAM BE STARTED A SECOND TIME?
//
// hda_check_output_dma() above proves the engine runs ONCE, from a freshly
// configured descriptor, with nothing else having touched it. That is the
// question #71 asked, and on the owner's iMac14,4 it answered RUNNING while the
// machine was silent, because the failing question was the next one: after that
// check stopped the stream, could anything start it again? It could not, and
// nothing in the boot said so. One line of the same boot reported
// "output DMA check: ... RUNNING (DMA advances) (stream=4 fmt=0x0011)" and a
// later line reported "TONE 660Hz: ... DMA NOT-STARTED (stream=4 fmt=0x0011
// STS=0x00)" on that same stream.
//
// So this asks it directly, at init, on every machine, deterministically. It is
// not a race and it does not depend on the gated boot tone: the two shapes that
// produced the silence are constructed on purpose and required to recover.
//
//   CHECK 1, end to end: start the stream, then reconfigure it the way EVERY
//   second audio client does (audio_open -> hda_configure ->
//   hda_configure_output_stream, which clears RUN and drives SRST), then start
//   again. RUN must be set at the end. This is exactly what the boot chime and
//   the boot self-tone did to each other.
//
//   CHECK 2, the specific mechanism: plant the precise pre-fix state, hardware
//   RUN clear with the playing-shadow claiming PLAYING, and require hda_start()
//   to recover from it. Before #173 that combination made hda_start() return
//   AUDIO_OK without writing RUN.
//
// The stream is silent throughout (the ring holds whatever the liveness check
// left, and this runs for microseconds), and it is left stopped. Cost is two
// short DMA starts and one SRST, the same order of cost as the check above it,
// which is why it is always on rather than gated: the machine that needs the
// answer is the one with no serial console, and hda_log() reaches its
// /BOOTLOG.TXT.
static void hda_restart_selftest(void) {
    uint8_t s = hda_state.out_stream_idx;
    unsigned mask = 0;

    // ---- CHECK 1: restart across a reconfigure -----------------------------
    hda_start();
    if ((hda_sd_read32(s, HDA_SD_CTL) & HDA_SD_CTL_RUN) == 0) mask |= 1u << 0;
    hda_configure_output_stream();
    // A configure that did NOT stop the engine would make check 1 vacuous:
    // it would pass without ever testing a restart. Assert the precondition.
    if ((hda_sd_read32(s, HDA_SD_CTL) & HDA_SD_CTL_RUN) != 0) mask |= 1u << 1;
    hda_start();
    if ((hda_sd_read32(s, HDA_SD_CTL) & HDA_SD_CTL_RUN) == 0) mask |= 1u << 2;
    hda_stop();

    // ---- CHECK 2: restart out of a stale playing-shadow ---------------------
    hda_state.playing = true;
    if ((hda_sd_read32(s, HDA_SD_CTL) & HDA_SD_CTL_RUN) != 0) mask |= 1u << 3;
    hda_start();
    if ((hda_sd_read32(s, HDA_SD_CTL) & HDA_SD_CTL_RUN) == 0) mask |= 1u << 4;
    hda_stop();

    hda_log("[HDA] #173 restart self-test %s mask=0x%02x (bit0 first start; "
            "bit1 configure did not stop the engine, so bit2 would be vacuous; "
            "bit2 RESTART AFTER RECONFIGURE FAILED; bit3 precondition; "
            "bit4 STALE PLAYING-SHADOW BLOCKED THE RUN WRITE. bit2 or bit4 set "
            "means #173 is live on this machine and audio will go silent for "
            "the rest of the session after the first stream is reconfigured.)",
            mask ? "FAIL" : "PASS", mask);
}

int hda_init(void) {
    LOG_INFO("[HDA] Initializing Intel HDA driver");
    hda_log("[HDA] Scanning ALL HD Audio controllers (class 04:03)...");

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

        hda_log("[HDA] Controller #%d %04x:%04x at %02x:%02x.%x IRQline=%u",
                controllers, dev->vendor_id, dev->device_id, dev->bus, dev->slot,
                dev->func, dev->interrupt_line);
        // #205: WHICH CONTROLLER, durably, unconditionally. The owner's ASUS
        // has TWO (an HDMI/DP codec and the Realtek analog one with the actual
        // speakers), and "the stream is healthy" and "the stream is audible"
        // are different questions on such a machine. A stream on the digital
        // codec is perfectly healthy and completely silent.
        audiolog_write("[HDA] controller #%d %04x:%04x at %02x:%02x.%x IRQline=%u",
                       controllers, dev->vendor_id, dev->device_id, dev->bus,
                       dev->slot, dev->func, dev->interrupt_line);

        if (hda_bring_up_controller(dev) != AUDIO_OK) {
            hda_log("[HDA]   bring-up failed; skipping");
            continue;
        }

        uint16_t statests = hda_read16(HDA_REG_STATESTS);
        hda_write16(HDA_REG_STATESTS, statests);
        hda_log("[HDA]   STATESTS=0x%04x (bit N set = a codec answered at address N)", statests);
        if (statests == 0) continue;

        for (int cad = 0; cad < 15; cad++) {
            if (!(statests & (1 << cad))) continue;
            hda_codec_t c;
            int s = hda_parse_codec(cad, &c);
            // #390: a codec that gives no VendorID response (the iMac GPU-HDA
            // controller #1's codec@0 does exactly this) is skipped so we fall
            // through to controller #2's real Cirrus CS4208 codec.
            if (!c.vendor_id) {
                hda_log("[HDA]   codec@%d: no VendorID response, skipping", cad);
                continue;
            }
            codecs_seen++;
            audiolog_write("[HDA]   codec@%d %04x:%04x -> %s (score %d, %s dev=0x%x "
                           "routes=%d)  [higher score wins; analog beats digital]",
                           cad, c.vendor_id, c.device_id,
                           s >= 0 ? "OUTPUT PATH" : "no output", s,
                           c.is_analog ? "analog" : "digital", c.default_device,
                           c.num_out_routes);
            hda_log("[HDA]   codec@%d %04x:%04x -> %s (score %d, %s dev=0x%x routes=%d)",
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
        hda_log("[HDA] No HD Audio controller present");
        return AUDIO_ERR_NO_DEVICE;
    }
    if (best_score < 0) {
        hda_log("[HDA] No usable output path on any of %d controller(s), %d codec(s)",
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
        hda_log("[HDA] Winning controller re-init failed");
        return AUDIO_ERR_NO_DEVICE;
    }
    hda_write16(HDA_REG_STATESTS, hda_read16(HDA_REG_STATESTS));

    uint16_t gcap = hda_read16(HDA_REG_GCAP);
    hda_state.supports_64bit = (gcap & HDA_GCAP_64OK) != 0;
    hda_state.num_oss = (gcap >> 12) & 0xF;
    hda_state.num_iss = (gcap >> 8) & 0xF;
    hda_state.num_bss = (gcap >> 3) & 0x1F;
    hda_log("[HDA] Winner %04x:%04x GCAP=0x%04x OSS=%d ISS=%d codec %04x:%04x",
            hda_state.vendor_id, hda_state.device_id, gcap,
            hda_state.num_oss, hda_state.num_iss,
            best_codec.vendor_id, best_codec.device_id);

    if (hda_state.num_oss == 0) {
        hda_log("[HDA] No output streams available");
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

    // #173 core proof: confirm it can be started AGAIN after a stop.
    hda_restart_selftest();

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

    // #189: SAY WHICH ARM THIS BINARY IS. The bug this guards against is
    // inaudible in a log and only visible in a host-side capture, so a capture
    // has to be attributable to a build without trusting a filename or a
    // timestamp. The self-test result is included because arithmetic that is
    // compiled in but wrong would reproduce #189 exactly, with the fix
    // apparently present.
#ifdef RUST_HDA_STARVE
    {
        uint32_t __st = hda_starve_selftest_rs();
        uint32_t __ss = hda_starve_stat_selftest_rs();   // AUDLEAD
        hda_log("[HDA] AUDLEAD starve-accounting: selftest mask=0x%04x %s "
                "(non-zero = the underrun counter does not measure the event "
                "the repair path repairs, so every buffering number below it "
                "is unreadable)", __ss, __ss ? "FAIL" : "PASS");
        hda_log("[HDA] #189 starve-silence: ARM=SILENCE-ON-STARVE (COMPILED IN); "
                "ring=%u bytes / %u slots of %u; selftest mask=0x%04x %s "
                "(non-zero = the slot arithmetic is wrong and the ring WILL "
                "replay its tail)",
                hda_state.dma_buffer_size, (unsigned)HDA_NUM_BDL,
                hda_state.bdl_buffer_size, __st, __st ? "FAIL" : "PASS");
    }
#else
    hda_log("[HDA] #189 starve-silence: ARM=LEGACY (NOT COMPILED IN); "
            "ring=%u bytes / %u slots of %u; a producer that stops feeding "
            "WILL leave this ring replaying its last lap until the stream is "
            "stopped",
            hda_state.dma_buffer_size, (unsigned)HDA_NUM_BDL,
            hda_state.bdl_buffer_size);
#endif

    LOG_INFO("[HDA] Initialization complete");
    hda_log("[HDA] Init complete: %s output, codec %04x:%04x DAC=%d PIN=%d",
            hda_state.codec.is_analog ? "ANALOG" : "digital",
            hda_state.codec.vendor_id, hda_state.codec.device_id,
            hda_state.codec.dac_nid, hda_state.codec.out_pin_nid);
    // #205: THE SELECTION, AND WHY. This is the line that answers "is anything
    // this machine plays going to come out of a speaker?".
    audiolog_write("[HDA] SELECTED controller %04x:%04x, codec %04x:%04x, %s output, "
                   "DAC nid=%d PIN nid=%d, %u Hz %u ch. %s",
                   hda_state.vendor_id, hda_state.device_id,
                   hda_state.codec.vendor_id, hda_state.codec.device_id,
                   hda_state.codec.is_analog ? "ANALOG" : "DIGITAL (HDMI/DP)",
                   hda_state.codec.dac_nid, hda_state.codec.out_pin_nid,
                   hda_state.sample_rate, hda_state.channels,
                   hda_state.codec.is_analog
                     ? "Analog: this is the speaker/headphone path."
                     : "DIGITAL: audio here is inaudible unless an HDMI/DP sink "
                       "is attached and selected. If the machine has speakers, "
                       "the wrong codec won the scoring.");

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
            hda_setup_route_converter(hda_state.codec.cad,
                                      r, hda_state.codec.route_dac[r], 0);
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

// #173: what a start attempt actually achieved, decided in rustkern/hdadma.rs
// from the shadow flag and the SDnCTL read before and after the RUN write. The
// MMIO sequencing stays in C (see the CHANGELOG note on entanglement); the
// DECISION is pure integer logic and lives in Rust with the rest of the HDA
// verdicts, so the log line and the semantics cannot drift apart.
#define HDA_START_ALREADY_RUNNING 0
#define HDA_START_OK              1
#define HDA_START_SHADOW_STALE    2
#define HDA_START_REFUSED         3
// #205: RUN read clear but LPIB advanced -> the engine is running and the
// register is wrong. See rustkern/hdadma.rs for the measurement this is from.
#define HDA_START_RUNNING_UNREPORTED 4
extern int      hda_start_verdict_lpib_rs(uint32_t shadow_playing,
                                          uint32_t ctl_before, uint32_t ctl_after,
                                          uint32_t lpib0, uint32_t lpib1,
                                          uint32_t cbl);
extern const char *hda_start_verdict_name_rs(int verdict);

// #173: START THE STREAM AGAINST THE HARDWARE, NOT AGAINST A SHADOW BOOL.
//
// THE BUG THIS REPLACES. hda_state.playing was treated as the authority on
// whether the output stream was running, and the first line of this function
// was `if (hda_state.playing) return AUDIO_OK;`. But `playing` is only a
// SHADOW of SDnCTL.RUN, and three separate code paths clear RUN in HARDWARE
// without touching the shadow:
//
//   - hda_configure_output_stream(), which explicitly clears RUN and then
//     drives a full SRST stream reset (every hda_configure() reaches it);
//   - hda_check_output_dma(), the init-time liveness proof, which starts and
//     then stops the engine directly;
//   - hda_devlog_scan()'s restore path, which does a whole-controller GCTL
//     reset with a stream possibly running.
//
// Once any of those ran while `playing` was true, the shadow said "already
// playing" for a stopped engine, this function returned WITHOUT writing RUN,
// and the machine was silent for the rest of the boot with no error anywhere.
// That is the real iMac14,4 fault in #173: the boot chime worker
// (audio_boot_sound_worker) had set playing=true, then the gated boot
// diagnostics reconfigured the same single output stream, and the boot tone's
// hda_start() no-opped. The captured /BOOTLOG.TXT holds the contradiction in
// two lines of one boot: the init-time check reported "RUNNING (DMA advances)"
// on stream 4, and the tone minutes later reported "DMA NOT-STARTED" on the
// same stream 4 with the same format and STS=0x00.
//
// So: RUN is read from the register, not remembered. The shadow is now written
// FROM the readback, never trusted as an input. A start is idempotent across
// any stop, by anybody, in any order.
// #205: ONE start attempt, judged on TWO witnesses.
//
// WHAT THIS REPLACES, and why the old shape was wrong on real hardware. It was:
//
//     hda_sd_write32(stream, HDA_SD_CTL, ctl_before | HDA_SD_CTL_RUN);
//     ctl_after = hda_sd_read32(stream, HDA_SD_CTL);      // one instruction later
//
// with a comment asserting that a PCI read flushes the posted write ahead of
// it, so the readback must observe the latched bit. On the owner's iMac14,4
// (Lynx Point-LP 8086:9c20) that assertion is FALSE, and the build-2007
// /BOOTLOG.TXT contains the contradiction eight times over: this function
// reported "REFUSED-BY-CONTROLLER" and the next line, from hda_selftest_tone()
// writing the SAME bit at the SAME address with no readback, reported
// "LPIB 0 -> 19468 ... 100.0% of rate : DMA RUNNING".
//
// So: give the bit TIME (a bounded settle, the same 100-iteration idiom every
// other wait in this file uses), and if it still will not say RUN, ask the
// witness that cannot lie. LPIB is the controller's own link position; a
// stopped DMA engine does not advance it. The DECISION is in Rust
// (hda_start_verdict_lpib_rs); only the MMIO sequencing is here.
static int hda_try_start_once(uint8_t stream, uint32_t shadow,
                              uint32_t *out_before, uint32_t *out_after) {
    uint32_t ctl_before = hda_sd_read32(stream, HDA_SD_CTL);
    uint32_t ctl_after  = ctl_before;
    uint32_t lpib0 = 0, lpib1 = 0;

    if ((ctl_before & HDA_SD_CTL_RUN) == 0) {
        lpib0 = hda_sd_read32(stream, HDA_SD_LPIB);
        hda_sd_write32(stream, HDA_SD_CTL, ctl_before | HDA_SD_CTL_RUN);

        // Bounded settle: 20 x hda_delay(100), about 20 ms worst case, and it
        // leaves on the FIRST pass on every controller that answers promptly
        // (every VM measured, and the healthy path on real hardware too).
        for (int i = 0; i < 20; i++) {
            ctl_after = hda_sd_read32(stream, HDA_SD_CTL);
            if (ctl_after & HDA_SD_CTL_RUN) break;
            hda_delay(100);
        }

        if ((ctl_after & HDA_SD_CTL_RUN) == 0) {
            // The register has had its chance. Sample the link position across
            // a short window; hda_delay(2000) is the same unit hda_check_
            // output_dma() already uses for exactly this question.
            hda_delay(2000);
            lpib1 = hda_sd_read32(stream, HDA_SD_LPIB);
        } else {
            lpib1 = lpib0;
        }
    }

    *out_before = ctl_before;
    *out_after  = ctl_after;
    return hda_start_verdict_lpib_rs(shadow, ctl_before, ctl_after,
                                     lpib0, lpib1, hda_state.dma_buffer_size);
}

int hda_start(void) {
    if (!hda_state.initialized) return AUDIO_ERR_NOT_INITIALIZED;

    uint8_t stream = hda_state.out_stream_idx;
    uint32_t shadow = hda_state.playing ? 1u : 0u;

    uint32_t ctl_before = 0, ctl_after = 0;
    int verdict = hda_try_start_once(stream, shadow, &ctl_before, &ctl_after);

    if (verdict == HDA_START_RUNNING_UNREPORTED) {
        // Log ONCE per boot, not once per start: on the machine where this
        // fires it fires on every start, and this goes to the persistent boot
        // log, which is disk I/O.
        if (!hda_state.run_readback_unreliable) {
            hda_state.run_readback_unreliable = true;
            hda_log("[HDA] #205: stream %d SDnCTL.RUN reads CLEAR (ctl 0x%08x -> "
                    "0x%08x) but LPIB ADVANCED across the settle window, so the "
                    "engine IS running and the RUN bit is not evidence on this "
                    "controller. Treating the start as SUCCESSFUL. Before #205 "
                    "this was reported REFUSED-BY-CONTROLLER, the descriptor was "
                    "SRST-reset out from under a stream that had just started, "
                    "and the machine went silent with every log claiming the "
                    "hardware had refused. RUN is now ignored for the rest of "
                    "this boot; hda_out_stopped() answers from the shadow.",
                    stream, ctl_before, ctl_after);
        }
    }

    // #173 RECOVERY, bounded and one-shot (not a poll loop): if the controller
    // did not latch RUN, the descriptor is the only thing that can be wrong at
    // this point, so re-program it once and try again. Two attempts, no loop.
    if (verdict == HDA_START_REFUSED) {
        hda_log("[HDA] #173 start REFUSED on stream %d: ctl 0x%08x -> 0x%08x "
                "(CBL=%u LVI=%u FMT=0x%04x BDPL=0x%08x STS=0x%02x); "
                "re-programming the descriptor and retrying once",
                stream, ctl_before, ctl_after,
                hda_sd_read32(stream, HDA_SD_CBL),
                hda_sd_read16(stream, HDA_SD_LVI),
                hda_sd_read16(stream, HDA_SD_FMT),
                hda_sd_read32(stream, HDA_SD_BDPL),
                hda_sd_read8(stream, HDA_SD_STS));
        hda_configure_output_stream();
        // #205: the retry is judged by the same two witnesses as the first
        // attempt. It used to re-read SDnCTL one instruction after the write,
        // i.e. it re-ran the exact measurement that had just been wrong.
        verdict = hda_try_start_once(stream, 0u, &ctl_before, &ctl_after);
        hda_log("[HDA] #173 start retry on stream %d: ctl 0x%08x -> 0x%08x : %s",
                stream, ctl_before, ctl_after, hda_start_verdict_name_rs(verdict));
    } else if (verdict == HDA_START_SHADOW_STALE) {
        // The exact #173 signature. Loud, once per occurrence, into the
        // persistent bootlog: the iMac has no serial console, so a kprintf
        // here would be invisible on the one machine that needs it.
        hda_log("[HDA] #173: stale playing-shadow caught on stream %d "
                "(shadow said PLAYING, hardware RUN was clear). Before this fix "
                "the stream would have stayed stopped and the machine silent. "
                "RUN re-asserted: ctl 0x%08x -> 0x%08x",
                stream, ctl_before, ctl_after);
    }

    // The shadow is a RECORD of the hardware, never an input to it - but the
    // record is now the VERDICT, not one register bit, because #205 measured a
    // controller on which that bit disagrees with the engine it describes.
    hda_state.playing = (verdict == HDA_START_OK ||
                         verdict == HDA_START_SHADOW_STALE ||
                         verdict == HDA_START_ALREADY_RUNNING ||
                         verdict == HDA_START_RUNNING_UNREPORTED);

    LOG_INFO("[HDA] Playback started");
    return hda_state.playing ? AUDIO_OK : AUDIO_ERR_DMA_ERROR;
}

// #190: is the ONE hardware output stream actually running RIGHT NOW?
//
// Read from SDnCTL, never from hda_state.playing, for the #173 reason: the
// shadow is a record of what this driver last did and three code paths clear
// RUN in hardware without going through hda_stop(). A caller asking this
// question is asking about the ENGINE.
//
// One MMIO read, no locks, no allocation, cannot block: safe to evaluate inside
// a wait_event() condition, which is exactly where audio_pcm.c uses it.
bool hda_out_stopped(void) {
    if (!hda_state.initialized) return true;
    // #205: on a controller where hda_start() has MEASURED SDnCTL.RUN reading
    // clear over a demonstrably advancing LPIB, this register is not evidence
    // and reading it here makes this predicate PERMANENTLY TRUE. That matters
    // more than it looks: audio_pcm.c evaluates this inside a wait_event()
    // condition, and a condition that is always true is not a wait, it is a
    // 100%-CPU spin that plays nothing. Fall back to the shadow, which
    // hda_start()/hda_stop() both write from a measured verdict.
    if (hda_state.run_readback_unreliable) return !hda_state.playing;
    return (hda_sd_read32(hda_state.out_stream_idx, HDA_SD_CTL) & HDA_SD_CTL_RUN) == 0;
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

    // #173: same rule as hda_start() - the shadow records what the register
    // says, it does not assert what we intended.
    // #205: except on a controller whose RUN bit has already been caught
    // disagreeing with LPIB. There the register cannot be believed in either
    // direction, and believing it here would leave playing=true after a stop
    // and make hda_out_stopped() answer "running" for a stopped engine.
    hda_state.playing = hda_state.run_readback_unreliable
                      ? false
                      : ((hda_sd_read32(stream, HDA_SD_CTL) & HDA_SD_CTL_RUN) != 0);
    LOG_INFO("[HDA] Playback stopped");

    return AUDIO_OK;
}

#ifdef RUST_HDA_STARVE
// #189: RESERVE slots under the service lock, then copy with the lock dropped.
//
// The reservation is what makes the unlocked copy safe: the slots handed back
// by hda_starve_avail_slots_rs() are, by that function's anti-aliasing bound,
// guaranteed not to be in the silencer's window, so no interrupt can zero
// bytes this function is writing. That bound is the whole reason the counters
// are free-running rather than modular.
//
// The critical section is a handful of integer operations and one MMIO read;
// the memcpy, which can be the length of the whole ring, is outside it.
int hda_write(const void *buffer, uint32_t frames) {
    if (!hda_state.initialized || !buffer) return AUDIO_ERR_INVALID_PARAM;
    if (!hda_state.dma_buffer || hda_state.bdl_buffer_size == 0) {
        return AUDIO_ERR_INVALID_PARAM;
    }

    uint32_t bits = 16;
    if (hda_state.format == AUDIO_FORMAT_S24_LE || hda_state.format == AUDIO_FORMAT_S32_LE) {
        bits = 32;
    }
    uint32_t bytes_per_frame = (bits / 8) * hda_state.channels;
    if (bytes_per_frame == 0) return AUDIO_ERR_INVALID_PARAM;

    uint32_t slot_bytes = hda_state.bdl_buffer_size;
    uint32_t bytes = frames * bytes_per_frame;
    if (bytes == 0) return 0;
    uint32_t want = (bytes + slot_bytes - 1) / slot_bytes;

    uint64_t start;
    uint32_t take;

    uint64_t fl = spinlock_acquire_irqsave(&g_hda_svc_lock);
    // A real producer owns the ring from here on, so it is no longer a
    // deliberately repeating test tone.
    hda_state.loop_mode = false;
    hda_starve_track_locked();
    hda_state.sil_slot = hda_starve_sil_floor_rs(hda_state.sil_slot,
                                                 hda_state.dma_slot, HDA_NUM_BDL);
    // If the engine has caught up with or passed the frontier, the queue is
    // empty: put the new audio just past the play head, not a whole lap behind
    // it where it would not be heard for 0.7 s.
    hda_starve_account_locked();     // AUDLEAD: BEFORE the repair erases the evidence
    hda_state.wr_slot = hda_starve_resync_rs(hda_state.wr_slot, hda_state.dma_slot);
    take = hda_starve_avail_slots_rs(hda_state.wr_slot, hda_state.dma_slot,
                                     hda_state.sil_slot, HDA_NUM_BDL);
    if (take > want) take = want;
    start = hda_state.wr_slot;
    hda_state.wr_slot += take;
    hda_state.write_index = (uint8_t)(hda_state.wr_slot % HDA_NUM_BDL);
    spinlock_release_irqrestore(&g_hda_svc_lock, fl);

    if (take == 0) return 0;
    // AUDLEAD: a producer has now committed audio to this stream, so from here on
    // "the engine reached a slot nobody wrote" is a real fault and not just the
    // start of a stream.
    hda_state.starve_armed = true;

    uint32_t written_frames = 0;
    const uint8_t *src = (const uint8_t *)buffer;
    for (uint32_t i = 0; i < take && bytes > 0; i++) {
        uint32_t n = (bytes > slot_bytes) ? slot_bytes : bytes;
        uint8_t *dst = (uint8_t *)hda_state.dma_buffer +
                       (size_t)((start + i) % HDA_NUM_BDL) * slot_bytes;
        memcpy(dst, src, n);
        // #189: a short final chunk used to leave the REST of the slot holding
        // whatever was there last lap, which the engine then played as a stale
        // fragment. Zero it, for the same reason the silencer exists.
        if (n < slot_bytes) memset(dst + n, 0, slot_bytes - n);
        src   += n;
        bytes -= n;
        written_frames += n / bytes_per_frame;
    }

    hda_state.frames_played += written_frames;

    return (int)written_frames;
}
#else
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
#endif

#ifdef RUST_HDA_STARVE
// #189: the same accounting hda_write() reserves against, so a waiter that is
// told there is room and then writes cannot be refused. Answering this question
// from the old modular arithmetic while hda_write() used the free-running kind
// would let audio_avail() promise space the writer would then decline, which is
// a hang in a wait_event condition, not a cosmetic disagreement.
int hda_avail(void) {
    if (!hda_state.initialized || hda_state.bdl_buffer_size == 0) return 0;

    uint64_t fl = spinlock_acquire_irqsave(&g_hda_svc_lock);
    hda_starve_track_locked();
    hda_state.sil_slot = hda_starve_sil_floor_rs(hda_state.sil_slot,
                                                 hda_state.dma_slot, HDA_NUM_BDL);
    hda_starve_account_locked();     // AUDLEAD: hda_avail() is polled far more often
                                     // than hda_write() is called, so this is the
                                     // site that actually SEES a starve begin.
    hda_state.wr_slot = hda_starve_resync_rs(hda_state.wr_slot, hda_state.dma_slot);
    hda_state.write_index = (uint8_t)(hda_state.wr_slot % HDA_NUM_BDL);
    uint32_t slots = hda_starve_avail_slots_rs(hda_state.wr_slot, hda_state.dma_slot,
                                               hda_state.sil_slot, HDA_NUM_BDL);
    spinlock_release_irqrestore(&g_hda_svc_lock, fl);

    uint32_t bits = 16;
    if (hda_state.format == AUDIO_FORMAT_S24_LE || hda_state.format == AUDIO_FORMAT_S32_LE) {
        bits = 32;
    }
    uint32_t bytes_per_frame = (bits / 8) * hda_state.channels;
    if (bytes_per_frame == 0) return 0;

    return (int)(((uint64_t)slots * hda_state.bdl_buffer_size) / bytes_per_frame);
}
#else
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
#endif

// AUDLEAD: publish the starvation accounting.
//
// Slots are converted to milliseconds HERE rather than at the call sites,
// because the conversion needs the format the driver actually programmed and
// nothing above this layer knows it. `lead_min_ms` is UINT32_MAX when no
// healthy observation was ever taken, which the caller must print as "no data":
// a stream that never played must not report a perfect minimum lead.
void hda_starve_stats(uint64_t *events, uint64_t *starved_ms,
                      uint32_t *lead_min_ms, uint64_t *obs) {
#ifdef RUST_HDA_STARVE
    uint32_t bits = 16;
    if (hda_state.format == AUDIO_FORMAT_S24_LE || hda_state.format == AUDIO_FORMAT_S32_LE)
        bits = 32;
    uint32_t bpf = (bits / 8) * hda_state.channels;
    uint32_t rate = hda_state.sample_rate ? hda_state.sample_rate : 48000;
    uint64_t fl = spinlock_acquire_irqsave(&g_hda_svc_lock);
    uint64_t ev = hda_state.starve_events;
    uint64_t sl = hda_state.starve_slots;
    uint64_t lm = hda_state.lead_min_slots;
    uint64_t ob = hda_state.lead_obs;
    uint32_t sb = hda_state.bdl_buffer_size;
    spinlock_release_irqrestore(&g_hda_svc_lock, fl);

    uint64_t slot_us = 0;
    if (bpf && rate) slot_us = ((uint64_t)sb * 1000000ULL) / ((uint64_t)rate * bpf);

    if (events)     *events = ev;
    if (starved_ms) *starved_ms = (sl * slot_us) / 1000ULL;
    if (obs)        *obs = ob;
    if (lead_min_ms) {
        *lead_min_ms = (lm == 0xFFFFFFFFFFFFFFFFULL)
                     ? 0xFFFFFFFFu
                     : (uint32_t)((lm * slot_us) / 1000ULL);
    }
#else
    if (events) *events = 0;
    if (starved_ms) *starved_ms = 0;
    if (obs) *obs = 0;
    if (lead_min_ms) *lead_min_ms = 0xFFFFFFFFu;
#endif
}

// AUDLEAD: the ring's total playable depth in milliseconds, for a producer that
// wants to size its lead against the buffer it actually has rather than against
// a constant someone chose once.
uint32_t hda_ring_ms(void) {
#ifdef RUST_HDA_STARVE
    uint32_t bits = 16;
    if (hda_state.format == AUDIO_FORMAT_S24_LE || hda_state.format == AUDIO_FORMAT_S32_LE)
        bits = 32;
    uint32_t bpf = (bits / 8) * hda_state.channels;
    uint32_t rate = hda_state.sample_rate ? hda_state.sample_rate : 48000;
    if (!bpf || !rate) return 0;
    return (uint32_t)(((uint64_t)hda_state.dma_buffer_size * 1000ULL) /
                      ((uint64_t)rate * bpf));
#else
    return 0;
#endif
}

// #71: volume/mute now (a) scale into the widget's OWN ladder instead of
// assuming a 0..0x7F one - pin 16 on this codec has Num Steps 0x42, so a
// literal 0x7F is out of range there - and (b) apply to EVERY output route,
// not only route 0's DAC. Applying to route 0 alone left the second speaker and
// the headphone path at whatever gain they happened to hold.
static void hda_apply_out_gain_scaled(uint8_t cad, uint8_t nid, uint8_t pct127, int mute) {
    uint32_t cap = hda_out_amp_cap(cad, nid, hda_state.codec.fg_nid);
    if (cap == 0) return;                                  // no amp on this widget
    uint8_t nsteps = (uint8_t)HDA_AMPCAP_NUMSTEPS(cap);
    if (pct127 > 127) pct127 = 127;
    uint8_t g = (uint8_t)(((uint32_t)pct127 * nsteps) / 127);
    hda_codec_command(cad, nid, HDA_VERB_SET_AMP_GAIN |
        HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
        (mute ? HDA_AMP_MUTE : 0) | g);
}

void hda_set_volume(uint8_t left, uint8_t right) {
    if (!hda_state.initialized) return;
    hda_codec_t *c = &hda_state.codec;
    int nr = c->num_out_routes ? c->num_out_routes : 1;
    // One value per channel is not expressible through the scaled helper, so
    // send left and right separately where the widget has an amp.
    for (int r = 0; r < nr; r++) {
        for (int which = 0; which < 2; which++) {
            uint8_t nid = which ? c->route_pin[r] : c->route_dac[r];
            uint32_t cap = hda_out_amp_cap(c->cad, nid, c->fg_nid);
            if (cap == 0) continue;
            uint8_t nsteps = (uint8_t)HDA_AMPCAP_NUMSTEPS(cap);
            uint8_t gl = (uint8_t)(((uint32_t)(left  > 127 ? 127 : left)  * nsteps) / 127);
            uint8_t gr = (uint8_t)(((uint32_t)(right > 127 ? 127 : right) * nsteps) / 127);
            hda_codec_command(c->cad, nid, HDA_VERB_SET_AMP_GAIN |
                              HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT  | gl);
            hda_codec_command(c->cad, nid, HDA_VERB_SET_AMP_GAIN |
                              HDA_AMP_SET_OUTPUT | HDA_AMP_SET_RIGHT | gr);
        }
    }
}

void hda_mute(bool mute) {
    if (!hda_state.initialized) return;
    hda_codec_t *c = &hda_state.codec;
    int nr = c->num_out_routes ? c->num_out_routes : 1;
    for (int r = 0; r < nr; r++) {
        hda_apply_out_gain_scaled(c->cad, c->route_dac[r], 127, mute ? 1 : 0);
        hda_apply_out_gain_scaled(c->cad, c->route_pin[r], 127, mute ? 1 : 0);
    }
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
#ifdef RUST_HDA_STARVE
    kprintf("  #189 silenced slots: %llu (given up on: %llu)\n",
            hda_state.silenced_slots, hda_state.silence_skips);
#endif
    kprintf("  Frames:       %llu\n", hda_state.frames_played);
    kprintf("  Underruns:    %llu\n", hda_state.underruns);
}

bool hda_is_analog_output(void) {
    return hda_state.initialized && hda_state.codec.is_analog;
}

// #71: peak absolute 16-bit sample currently in the DMA ring, sampled sparsely
// (every 16th frame) so it stays cheap on a 128KB buffer.
//
// This exists because LPIB advancing was mistaken for "the digital path is
// fine" for a year. LPIB advancing proves the DMA engine walked the buffer. It
// says nothing whatsoever about what was IN the buffer. Without this number,
// "DMA RUNNING and no sound" cannot distinguish a dead analogue path from a
// perfectly working one being fed silence.
static int hda_dma_buffer_peak(void) {
    if (!hda_state.dma_buffer || hda_state.dma_buffer_size < 4) return -1;
    const int16_t *b = (const int16_t *)hda_state.dma_buffer;
    uint32_t n = hda_state.dma_buffer_size / 2;
    int peak = 0;
    for (uint32_t i = 0; i < n; i += 16) {
        int v = b[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    return peak;
}

// ============================================================================
// #71: OUTPUT-DMA LIVENESS PROBE
// ============================================================================
//
// THE MISTAKE THIS REPLACES. Build 1932's real-iMac /AUDIOLOG.TXT reported
// "OUT stream 4: ... RUN=0 ... LPIB=0" and "LPIB poll worker: NOT running" and
// "MSI=NOT armed", and those three lines were read as the fault. Every one of
// them was guaranteed by WHERE the dump is emitted: drivers/audio.c runs it
// between audio_init() and the calls that start the poll worker, arm the MSI,
// and play the tone. The snapshot could not have said anything else on any
// machine, VM or iMac. A register read taken before the thing it describes has
// been set up is not evidence.
//
// A single LPIB read is unfalsifiable in the same way: "0" is equally
// consistent with a dead engine, a healthy engine at the top of its ring, and
// looking too early. So the sanctioned question is a DELTA over a known window
// of REAL time, with the RUN bit captured alongside the first read so the
// window cannot be reported as a failure when nobody had asked for playback.
//
// The clock is mono_us() (TSC-backed, cpu/mono.h), never timer_ticks: blame.md
// records that timer_ticks is not a wall clock because KVM replays lost ticks
// in bursts, which would make a rate computed from it report whatever the
// hypervisor felt like. The arithmetic and the verdict are in
// rustkern/hdadma.rs (new kernel code is Rust); only the MMIO reads and the
// sleep are here, because they are memory-mapped register access.
extern uint32_t hda_dma_bytes_advanced_rs(uint32_t lpib0, uint32_t lpib1, uint32_t cbl);
extern uint64_t hda_dma_expected_bytes_rs(uint32_t rate, uint32_t ch, uint32_t bits, uint64_t us);
extern uint64_t hda_dma_bytes_per_sec_rs(uint32_t rate, uint32_t ch, uint32_t bits);
extern uint32_t hda_dma_rate_permille_rs(uint32_t delta, uint64_t expected);
extern int      hda_dma_verdict_rs(uint32_t run, uint32_t lpib0, uint32_t lpib1,
                                   uint32_t cbl, uint64_t elapsed_us, uint64_t bps);
extern const char *hda_dma_verdict_name_rs(int verdict);


static hda_dma_probe_t g_hda_last_probe;

// How long the boot tone's probe watches the engine. 100 ms is ~19200 bytes at
// 48 kHz stereo 16-bit, comfortably above any plausible read jitter and well
// under one lap of the 128 KiB ring (~682 ms), so the delta is unambiguous.
#define HDA_PROBE_MS 100

// The gap between the two reads. A one-shot sleep, not a poll loop: there is no
// condition being spun on here, the elapsed time IS the measurement. Falls back
// to the calibrated busy delay only where blocking is forbidden (ISR/IRQs-off),
// which wq_assert_may_block() would otherwise catch at the wait-queue chokepoint.
static void hda_probe_gap(uint32_t ms) {
    if (wq_may_block()) {
        proc_sleep(ms);
    } else {
        hda_delay(ms * 1000);
    }
}

static uint32_t hda_format_bits(void) {
    if (hda_state.format == AUDIO_FORMAT_S24_LE || hda_state.format == AUDIO_FORMAT_S32_LE)
        return 32;
    return 16;
}

int hda_dma_probe(hda_dma_probe_t *p, uint32_t gap_ms) {
    if (!p) return HDA_DMA_UNKNOWN;
    for (unsigned i = 0; i < sizeof(*p); i++) ((uint8_t *)p)[i] = 0;
    p->verdict = HDA_DMA_UNKNOWN;
    p->peak = -1;
    if (!hda_state.initialized) return HDA_DMA_UNKNOWN;
    if (gap_ms == 0) gap_ms = HDA_PROBE_MS;

    uint8_t s = hda_state.out_stream_idx;
    p->sample_rate = hda_state.sample_rate;
    p->channels    = hda_state.channels;
    p->bits        = hda_format_bits();
    p->cbl         = hda_sd_read32(s, HDA_SD_CBL);

    // RUN is sampled WITH the first position, so a stream started or stopped
    // underneath us cannot be reported as this window's behaviour.
    p->ctl   = hda_sd_read32(s, HDA_SD_CTL);
    uint64_t t0 = mono_us();
    p->lpib0 = hda_sd_read32(s, HDA_SD_LPIB);

    hda_probe_gap(gap_ms);

    p->lpib1 = hda_sd_read32(s, HDA_SD_LPIB);
    uint64_t t1 = mono_us();
    p->elapsed_us = (t1 >= t0) ? (t1 - t0) : 0;
    p->sts = hda_sd_read8(s, HDA_SD_STS);
    p->peak = hda_dma_buffer_peak();

    uint64_t bps = hda_dma_bytes_per_sec_rs(p->sample_rate, p->channels, p->bits);
    p->delta    = hda_dma_bytes_advanced_rs(p->lpib0, p->lpib1, p->cbl);
    p->expected = hda_dma_expected_bytes_rs(p->sample_rate, p->channels, p->bits, p->elapsed_us);
    p->permille = hda_dma_rate_permille_rs(p->delta, p->expected);
    p->verdict  = hda_dma_verdict_rs((p->ctl & HDA_SD_CTL_RUN) ? 1u : 0u,
                                     p->lpib0, p->lpib1, p->cbl, p->elapsed_us, bps);

    // #71: THE SLEEP CAN OVERSHOOT, AND ON THE TARGET MACHINE IT MAY OVERSHOOT
    // BADLY. proc_sleep() is driven by the periodic tick, and the same iMac14,4
    // this ticket exists for is the machine where #62/tickwatch was written
    // because the owner reports "if i stop moving the mouse the timers all
    // hang". A 100 ms request that returns after a second would put the window
    // past one lap of the ring (~682 ms at 48 kHz stereo 16-bit over 128 KiB),
    // the wrap guard would correctly refuse to interpret the delta, and the log
    // would say NOT-MEASURED -- which is honest but is exactly the outcome this
    // whole change exists to stop producing. Measured on the verification VM:
    // a 100 ms request came back at 204 ms even on an idle boot.
    //
    // So on a wrap-ambiguous result, re-measure ONCE across a short calibrated
    // busy delay instead. hda_delay() does not depend on the tick, and 20 ms is
    // 34x inside the lap. This is a bounded one-shot delay, not a poll loop:
    // nothing is being spun on, the elapsed time IS the measurement.
    if (p->verdict == HDA_DMA_UNKNOWN && (p->ctl & HDA_SD_CTL_RUN)) {
        p->busy_gap = 1;
        p->ctl   = hda_sd_read32(s, HDA_SD_CTL);
        t0       = mono_us();
        p->lpib0 = hda_sd_read32(s, HDA_SD_LPIB);
        hda_delay(20000);
        p->lpib1 = hda_sd_read32(s, HDA_SD_LPIB);
        t1       = mono_us();
        p->elapsed_us = (t1 >= t0) ? (t1 - t0) : 0;
        p->sts   = hda_sd_read8(s, HDA_SD_STS);
        p->delta    = hda_dma_bytes_advanced_rs(p->lpib0, p->lpib1, p->cbl);
        p->expected = hda_dma_expected_bytes_rs(p->sample_rate, p->channels, p->bits, p->elapsed_us);
        p->permille = hda_dma_rate_permille_rs(p->delta, p->expected);
        p->verdict  = hda_dma_verdict_rs((p->ctl & HDA_SD_CTL_RUN) ? 1u : 0u,
                                         p->lpib0, p->lpib1, p->cbl, p->elapsed_us, bps);
    }
    p->measured = 1;
    g_hda_last_probe = *p;
    return p->verdict;
}

int hda_dma_probe_auto(hda_dma_probe_t *p, uint32_t gap_ms) {
    if (!p) return HDA_DMA_UNKNOWN;
    if (!hda_state.initialized) { hda_dma_probe(p, gap_ms); return HDA_DMA_UNKNOWN; }
    uint8_t s = hda_state.out_stream_idx;
    int was_running = (hda_sd_read32(s, HDA_SD_CTL) & HDA_SD_CTL_RUN) != 0;
    if (!was_running) hda_start();
    int v = hda_dma_probe(p, gap_ms);
    if (!was_running) hda_stop();
    p->started_for_probe = !was_running;
    g_hda_last_probe = *p;
    return v;
}

const hda_dma_probe_t *hda_last_dma_probe(void) { return &g_hda_last_probe; }

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

    // #71: the tone is the owner's ONLY audible test, so it must exercise the
    // path this ticket fixed rather than assume boot left it enabled. Re-assert
    // and re-report the analogue enables (pin output, headphone drive, EAPD
    // where the pin implements it, amp gains, Cirrus GPIO0) immediately before
    // making a sound. Idempotent, so on a healthy machine this changes nothing
    // and only logs.
    hda_analog_enables_apply(1);

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
#ifdef RUST_HDA_STARVE
    // #189: this tone IS the loop. The whole ring has just been filled with one
    // continuous waveform and the caller holds it audible by letting the engine
    // repeat it, so the starve silencer must stand down or the tone would cut
    // to silence after a single 0.7 s lap. Cleared again by the first
    // hda_write() from a real producer, and by the next stream reconfigure.
    hda_state.loop_mode = true;
#endif
    // Sample the peak HERE, immediately after filling, not after playback: by
    // the time the stream has run, the 10ms poll worker has refilled the ring
    // from the mixer and the peak would describe that, not the tone.
    int tone_peak = hda_dma_buffer_peak();

    hda_start();
    // #71: MEASURE the engine, do not peek at it. The old code here was a
    // bounded hda_delay() busy loop that broke out on the first LPIB change and
    // reported only "moved / did not move" -- so a stream limping at a tenth of
    // its rate and a stream running perfectly produced the same log line, and a
    // stream that had not been started yet produced the same line as a dead one.
    // hda_dma_probe() takes two reads across a known window of real time and
    // returns a verdict that separates all four cases. It also replaces a
    // busy-wait with a single sleep (#426).
    hda_dma_probe(&g_hda_last_probe, HDA_PROBE_MS);
    const hda_dma_probe_t *pr = &g_hda_last_probe;
    uint8_t s = hda_state.out_stream_idx;
    uint32_t lp0 = pr->lpib0, lpn = pr->lpib1;
    int moved = (pr->verdict == HDA_DMA_RUNNING || pr->verdict == HDA_DMA_SLOW);
    // #152 logging rule: this was a kprintf, i.e. invisible on the one machine
    // the tone exists for. It also now reports the buffer's PEAK SAMPLE, because
    // "LPIB advanced" only proves the engine walked the buffer, never that the
    // buffer held anything but zeroes: a peak of 0 with DMA running is silence
    // the driver caused, a peak of ~8192 with DMA running is silence the
    // analogue path caused, and those two need completely different fixes.
    hda_log("[HDA] TONE %uHz: LPIB %u -> %u advanced %u of an expected %u bytes "
            "in %u us (%u.%u%% of rate) : DMA %s (stream=%d fmt=0x%04x STS=0x%02x "
            "tone-peak-written=%d/32767, expect ~8192)",
            freq_hz, lp0, lpn, pr->delta, (uint32_t)pr->expected,
            (uint32_t)pr->elapsed_us, pr->permille / 10u, pr->permille % 10u,
            hda_dma_verdict_name_rs(pr->verdict),
            s, hda_state.stream_format, pr->sts, tone_peak);

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
    // #173: `saved` was captured before this scan reset every controller on the
    // machine, so its `playing` flag describes a stream that a GCTL reset has
    // since stopped. Restoring it verbatim resurrects a shadow that outlives
    // the hardware state it shadows - which is how a boot-chime-in-progress
    // left `playing` true over a dead engine and silenced everything after it.
    hda_state.playing = false;
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
        // #71: the interrupt state and the poll-worker state USED TO BE REPORTED
        // HERE, and reporting them here is what wasted a round of real-hardware
        // analysis. This function runs before either of them is set up (see the
        // call site in drivers/audio.c), so it read "MSI=NOT armed" and "poll
        // worker: NOT running" on EVERY machine including a healthy VM, and the
        // build-1932 iMac capture was read as if those were findings about the
        // hardware. They now live in hda_audiolog_runtime_report(), which runs
        // after the poll worker and the MSI have actually had their turn.
        snprintf(line, sizeof(line),
                 "Winning codec@%d: vendor:device %04x:%04x  AFG nid=%d  %s output  routes=%d  score=%d",
                 c->cad, c->vendor_id, c->device_id, c->fg_nid,
                 c->is_analog ? "ANALOG" : "digital",
                 c->num_out_routes, c->route_score);
        emit(line);
        if (c->vendor_id == HDA_VENDOR_CIRRUS) {
            snprintf(line, sizeof(line),
                     "  -> Cirrus Logic codec (CS4208 expected on iMac14,4); its "
                     "speaker amp gate is GPIO0, NOT pin EAPD (see live state below)");
            emit(line);
        }
    }

    // Full output-relevant widget graph for every controller/codec.
    emit("--- HD Audio widget graph (all controllers/codecs) ---");
    hda_devlog_scan(emit);

    if (!hda_state.initialized) return;

    emit("--- end of static widget graph. Live output-path state, the measured");
    emit("    output-DMA verdict and the pass/fail block follow BELOW, emitted");
    emit("    later in boot once the poll worker, the MSI and the tone have run. ---");
}

// ============================================================================
// #71: the RUNTIME half of the AUDIOLOG dump.
//
// Everything in here is meaningless until after the poll worker is started, the
// MSI is armed and the boot tone (if requested) has played, which is why it is
// a separate function called separately and LAST. The static widget graph above
// has the opposite constraint -- it must run BEFORE the poll worker so the two
// do not race for codec access -- and collapsing the two into one dump is
// exactly what produced build 1932's three tautological lines.
//
// The other change of principle here: this driver's own rule for the analogue
// half is "every one of these helpers WRITES THEN READS BACK; a verb this
// driver sends and never reads is not evidence of anything". That rule was only
// ever applied to the analogue enables. The DIGITAL half -- widget power state,
// converter stream/channel, converter format, digital-converter enable -- was
// sent blind. All four are read back here, per route.
// ============================================================================
static void hda_audiolog_runtime_body(void (*emit)(const char *line)) {
    char line[196];
    if (!emit) return;
    if (!hda_state.initialized) {
        emit("--- live output-path state: SKIPPED, HDA never initialized ---");
        emit("=== end AUDIOLOG ===");
        return;
    }

    emit("--- live output-path state (poll worker + MSI armed, tone done) ---");

    hda_codec_t *c = &hda_state.codec;
    uint8_t cad = c->cad;

    if (hda_state.msi_enabled) {
        snprintf(line, sizeof(line),
                 "Interrupt: legacy PCI IRQ line=%u (0 = not routed by firmware)  "
                 "MSI=armed on vector 0x%02x",
                 hda_state.irq, hda_state.msi_vector);
    } else {
        snprintf(line, sizeof(line),
                 "Interrupt: legacy PCI IRQ line=%u (0 = not routed by firmware)  "
                 "MSI=NOT armed (controller exposes no MSI capability)",
                 hda_state.irq);
    }
    emit(line);
    snprintf(line, sizeof(line),
             "LPIB poll worker: %s (services the stream every %ums independent of "
             "any interrupt; this is the wake source output does not depend on MSI for)",
             g_hda_poll_started ? "RUNNING" : "NOT RUNNING", (unsigned)HDA_POLL_MS);
    emit(line);

    // Codec GPIO (CS4208 speaker-amp power gate). GPIO verbs target the AFG node.
    uint32_t gpc  = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_PARAM | HDA_PARAM_GPIO_COUNT);
    uint32_t gm   = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_GPIO_MASK);
    uint32_t gd   = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_GPIO_DIR);
    uint32_t gda  = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_GPIO_DATA);
    uint32_t afgps = hda_codec_command(cad, c->fg_nid, HDA_VERB_GET_PS);
    // #71: the number of GPIOs the codec actually HAS decides whether any of
    // this means anything. GPIO Count bits 7:0 are the count; a codec that
    // reports 0 has no gate to assert, and printing "GATE NOT ASSERTED" for it
    // is the same class of mistake this whole change is about -- a reading that
    // could not have said anything else, presented as a finding. Only the
    // Cirrus part on the iMac gates its speaker amp this way, and it reports
    // count=0x...06.
    unsigned ngpio = (gpc == 0xFFFFFFFF) ? 0 : (gpc & 0xFF);
    int gpio_applicable = (ngpio > 0) && (c->vendor_id == HDA_VENDOR_CIRRUS);
    int gpio_ok = (gm & 1) && (gd & 1) && (gda & 1);
    snprintf(line, sizeof(line),
             "AFG nid %d: GPIO count=0x%08x (%u GPIO) mask=0x%02x dir=0x%02x data=0x%02x -> %s"
             " | power state: set D0, reads 0x%02x (act=D%u)",
             c->fg_nid, gpc, ngpio, gm & 0xFF, gd & 0xFF, gda & 0xFF,
             gpio_applicable ? (gpio_ok ? "speaker amp GATE ASSERTED"
                                        : "speaker amp GATE NOT ASSERTED")
                             : "no GPIO speaker-amp gate on this codec (n/a)",
             afgps & 0xFF, (afgps >> 4) & 0x3);
    emit(line);

    int nr = c->num_out_routes ? c->num_out_routes : 1;
    int routes_live = 0;
    int routes_bound = 0;
    for (int r = 0; r < nr; r++) {
        uint8_t dac = c->route_dac[r];
        uint8_t pin = c->route_pin[r];
        uint8_t dev = c->route_dev[r];
        uint32_t eapd   = hda_codec_command(cad, pin, HDA_VERB_GET_EAPD);
        uint32_t pinctl = hda_codec_command(cad, pin, HDA_VERB_GET_PIN_CTL);
        uint32_t cfg    = hda_codec_command(cad, pin, HDA_VERB_GET_CONFIG_DEF);
        uint32_t pincap = hda_codec_command(cad, pin, HDA_VERB_GET_PARAM | HDA_PARAM_PIN_CAP);
        if (pincap == 0xFFFFFFFF) pincap = 0;
        uint32_t dcap   = hda_out_amp_cap(cad, dac, c->fg_nid);
        uint32_t pcap   = hda_out_amp_cap(cad, pin, c->fg_nid);
        uint8_t dacg = 0, dacm = 0, ping = 0, pinm = 0;
        hda_audiolog_amp_out(cad, dac, &dacg, &dacm);
        hda_audiolog_amp_out(cad, pin, &ping, &pinm);
        snprintf(line, sizeof(line),
                 "route %d: DAC=%d PIN=%d dev=%s cfg=0x%08x  EAPD=0x%02x(%s) pinctl=0x%02x(out=%d hp=%d)  "
                 "DACamp gain=0x%02x %s  PINamp gain=0x%02x %s",
                 r, dac, pin, hda_default_device_name(dev), cfg,
                 eapd & 0xFF,
                 (pincap & HDA_PINCAP_EAPD) ? ((eapd & HDA_EAPD_ENABLE) ? "ON" : "OFF-AND-CAPABLE")
                                            : "pin has no EAPD",
                 pinctl & 0xFF, (pinctl & HDA_PIN_OUT_EN) ? 1 : 0,
                 (pinctl & HDA_PIN_HP_EN) ? 1 : 0,
                 dacg, dacm ? "MUTED" : "unmuted",
                 ping, pinm ? "MUTED" : "unmuted");
        emit(line);
        snprintf(line, sizeof(line),
                 "   caps: pincap=0x%08x EAPD-capable=%s HPdrv=%s | DAC%d out-amp %s | PIN%d out-amp %s",
                 pincap,
                 (pincap & HDA_PINCAP_EAPD) ? "YES" : "no",
                 (pincap & HDA_PINCAP_HP_DRV) ? "yes" : "no",
                 dac, dcap ? "present" : "ABSENT (no gain control on this widget)",
                 pin, pcap ? "present" : "ABSENT (no gain control on this widget)");
        emit(line);
        if (dcap || pcap) {
            snprintf(line, sizeof(line),
                     "   ampcap: DAC cap=0x%08x nsteps=0x%02x offset(0dB)=0x%02x | "
                     "PIN cap=0x%08x nsteps=0x%02x offset(0dB)=0x%02x",
                     dcap, (unsigned)HDA_AMPCAP_NUMSTEPS(dcap), (unsigned)HDA_AMPCAP_OFFSET(dcap),
                     pcap, (unsigned)HDA_AMPCAP_NUMSTEPS(pcap), (unsigned)HDA_AMPCAP_OFFSET(pcap));
            emit(line);
        }

        // #71 THE DIGITAL HALF, READ BACK. Every verb below is one this driver
        // SENDS during hda_configure_codec() and had never once read. A DAC
        // that silently kept stream tag 0 would leave the controller's DMA
        // running with nothing consuming the stream: LPIB advances, the
        // analogue path reads perfectly live, and the machine is silent. That
        // failure mode is indistinguishable from a healthy machine in every
        // capture taken before this line existed.
        uint32_t dacps  = hda_codec_command(cad, dac, HDA_VERB_GET_PS);
        uint32_t pinps  = hda_codec_command(cad, pin, HDA_VERB_GET_PS);
        uint32_t conv   = hda_codec_command(cad, dac, HDA_VERB_GET_CONV);
        uint32_t cfmt   = hda_codec_command(cad, dac, HDA_VERB_GET_CONV_FMT);
        uint32_t wcap   = hda_codec_command(cad, dac, HDA_VERB_GET_PARAM | HDA_PARAM_AUDIO_WIDGET_CAP);
        int is_digital  = (wcap != 0xFFFFFFFF) && (wcap & HDA_WCAP_DIGITAL);
        uint32_t dig1   = is_digital ? hda_codec_command(cad, dac, HDA_VERB_GET_DIGI_CONV1) : 0;
        uint8_t  strm   = (uint8_t)((conv >> 4) & 0x0F);
        uint8_t  chan   = (uint8_t)(conv & 0x0F);
        uint8_t  sdstrm = (uint8_t)((hda_sd_read32(hda_state.out_stream_idx, HDA_SD_CTL) >> 20) & 0xF);
        uint16_t sdfmt  = hda_sd_read16(hda_state.out_stream_idx, HDA_SD_FMT);
        int bound = (strm != 0) && (strm == sdstrm) &&
                    ((cfmt & 0xFFFF) == sdfmt) &&
                    (!is_digital || (dig1 & HDA_DIG1_ENABLE));
        if (bound) routes_bound++;
        snprintf(line, sizeof(line),
                 "   readback: DAC%d power=D%u PIN%d power=D%u | conv stream=%u chan=%u "
                 "(descriptor STRM=%u) | conv fmt=0x%04x (descriptor FMT=0x%04x)%s -> %s",
                 dac, (dacps >> 4) & 0x3, pin, (pinps >> 4) & 0x3,
                 strm, chan, sdstrm, (unsigned)(cfmt & 0xFFFF), sdfmt,
                 is_digital ? ((dig1 & HDA_DIG1_ENABLE) ? " | digital DigEn=1"
                                                        : " | digital DigEn=0 (OUTPUT GATED OFF)")
                            : " | analog converter",
                 bound ? "CONVERTER BOUND TO THE STREAM"
                       : "CONVERTER NOT BOUND (this alone is silence)");
        emit(line);

        {
            int gain_ok = (!dcap || (dacg && !dacm)) && (!pcap || (ping && !pinm));
            int eapd_ok = !(pincap & HDA_PINCAP_EAPD) || (eapd & HDA_EAPD_ENABLE);
            int out_ok  = (pinctl & HDA_PIN_OUT_EN) ? 1 : 0;
            int live    = out_ok && eapd_ok && gain_ok;
            if (live) routes_live++;
            snprintf(line, sizeof(line),
                     "   ANALOGUE PATH route %d: pin-output %s, EAPD %s, gain %s -> %s",
                     r, out_ok ? "ENABLED" : "DISABLED",
                     eapd_ok ? "ok" : "POWERED DOWN",
                     gain_ok ? "ok" : "AT ZERO (silence)",
                     live ? "LIVE" : "NOT LIVE");
            emit(line);
        }
    }

    // ---------------------------------------------------------------- DMA ---
    // The measurement, not a snapshot. If the boot tone ran it already probed
    // while the tone was audible, which is the reading we most want; report
    // THAT rather than re-measuring after the fact. Otherwise probe now (the
    // stream is started for ~100 ms and stopped again).
    const hda_dma_probe_t *pr = hda_last_dma_probe();
    hda_dma_probe_t local;
    int from_tone = pr->measured;
    if (!from_tone) {
        hda_dma_probe_auto(&local, HDA_PROBE_MS);
        pr = &local;
    }
    emit("--- output DMA liveness (MEASURED: two LPIB reads a known time apart) ---");
    snprintf(line, sizeof(line),
             "probe source: %s | stream %d fmt=0x%04x %uHz %uch %ubit CBL=%u",
             from_tone ? "the boot self-tone, sampled WHILE IT WAS PLAYING"
                       : "a silent 100ms diagnostic run (no AUDIOTONE.CFG present)",
             hda_state.out_stream_idx, hda_state.stream_format,
             pr->sample_rate, pr->channels, pr->bits, pr->cbl);
    emit(line);
    snprintf(line, sizeof(line),
             "RUN=%d STS=0x%02x  LPIB %u -> %u  advanced %u bytes of an expected %u "
             "in %u us%s  (%u.%u%% of the format's rate)",
             (pr->ctl & HDA_SD_CTL_RUN) ? 1 : 0, pr->sts, pr->lpib0, pr->lpib1,
             pr->delta, (uint32_t)pr->expected, (uint32_t)pr->elapsed_us,
             pr->busy_gap ? " (re-measured on a calibrated delay: the tick-driven"
                            " sleep overshot past one ring lap)" : "",
             pr->permille / 10u, pr->permille % 10u);
    emit(line);
    snprintf(line, sizeof(line),
             "ring peak sample at probe time: %d / 32767  (0 with DMA running means the "
             "driver fed silence; a real peak means the samples were there)", pr->peak);
    emit(line);

    // ------------------------------------------------------------ verdict ---
    // The point of this block: the owner should not have to decode hex to learn
    // whether the fault is ours. Each line names one stage and passes or fails
    // it, and the closing line says which side of the codec to look at next.
    emit("=== AUDIO VERDICT (read this, not the hex above) ===");
    snprintf(line, sizeof(line), "  OUTPUT DMA ......... %s", hda_dma_verdict_name_rs(pr->verdict));
    emit(line);
    snprintf(line, sizeof(line), "  RING CONTENT ....... %s (peak %d/32767)",
             (pr->peak > 64) ? "NON-SILENT" : (pr->peak < 0 ? "NO BUFFER" : "SILENT"), pr->peak);
    emit(line);
    snprintf(line, sizeof(line), "  CONVERTER BINDING .. %d of %d route(s) bound to the stream",
             routes_bound, nr);
    emit(line);
    snprintf(line, sizeof(line), "  ANALOGUE PATH ...... %d of %d route(s) LIVE", routes_live, nr);
    emit(line);
    if (gpio_applicable) {
        snprintf(line, sizeof(line), "  SPEAKER AMP GATE ... GPIO0 %s",
                 gpio_ok ? "ASSERTED" : "NOT ASSERTED");
        emit(line);
    }
    if (pr->verdict == HDA_DMA_NOT_STARTED || pr->verdict == HDA_DMA_UNKNOWN) {
        emit("  => NOT MEASURED. The engine was not running when it was sampled, so");
        emit("     nothing here says whether it can run. This is NOT a fault report.");
    } else if (pr->verdict == HDA_DMA_STALLED) {
        emit("  => FAULT IS THE STREAM ENGINE. RUN was set and the link position did");
        emit("     not move. Nothing downstream of the controller matters yet.");
    } else if (pr->verdict == HDA_DMA_SLOW) {
        emit("  => The engine runs but well under rate: expect glitching, not silence.");
    } else if (pr->peak <= 64) {
        emit("  => DMA runs and the ring is SILENT: the driver fed zeroes. The fault is");
        emit("     the mixer/feed path, not the hardware.");
    } else if (routes_bound < nr || routes_live < nr) {
        emit("  => DMA runs with real samples, but a codec route is not fully set up");
        emit("     (see CONVERTER BINDING / ANALOGUE PATH above). Fault is in the codec");
        emit("     programming, which is ours to fix.");
    } else if (gpio_applicable && !gpio_ok) {
        emit("  => DMA runs with real samples and every route is live, but the codec's");
        emit("     GPIO0 speaker-amp gate is NOT asserted, so the external amplifier is");
        emit("     powered down. That gate is ours to set: fault is in the driver.");
    } else {
        emit("  => Every stage this OS controls passed: real samples, DMA at rate,");
        emit("     converters bound to the stream, pins enabled, widgets in D0.");
        emit("     If you still heard nothing, the fault is BEYOND the codec pins");
        emit("     (external amp, output routing, or a vendor-specific init verb");
        emit("     this driver does not send).");
    }
    emit("=== end AUDIOLOG ===");
}

// Bounded wrapper, same contract as hda_audiolog_report(): arm the short
// per-command timeout + total-timeout budget so a silent codec cannot freeze
// the box (b730/b733), run, restore.
void hda_audiolog_runtime_report(void (*emit)(const char *line)) {
    int save_iters = g_hda_cmd_max_iters;
    g_hda_cmd_max_iters = 200;
    g_hda_diag_budget   = 60;
    g_hda_diag_active   = 1;
    hda_audiolog_runtime_body(emit);
    g_hda_diag_active   = 0;
    g_hda_diag_budget   = 0;
    g_hda_cmd_max_iters = save_iters;
}
