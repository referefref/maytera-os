// usb_audio.c - USB Audio Class 1.0 (UAC1) output driver
// #323: real sound out through a passed-through NuForce uDAC-3 (262a:10aa).
//
// The DAC is a full-speed UAC1 device. Audio interface 3, alt 1 carries a
// 16-bit stereo PCM isochronous OUT endpoint (0x03). We:
//   1. SET_CONFIGURATION 1
//   2. configure the iso OUT endpoint on the xHCI slot (CONFIG_EP)
//   3. SET_INTERFACE alt 1 on the audio streaming interface
//   4. set the sampling frequency (UAC1 endpoint control) to 48000 Hz
//   5. stream signed-16 stereo PCM frames as isochronous TDs
#include "usb_audio.h"
#include "usb.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../proc/process.h"

// --------------------------------------------------------------------------
// xHCI iso helpers implemented in xhci.c
// --------------------------------------------------------------------------
extern int xhci_configure_iso_out(xhci_controller_t *xhc, int slot_id,
                                  int ep_addr, int max_packet, int interval);
extern int xhci_iso_submit(xhci_controller_t *xhc, int slot_id, int dci,
                           uint64_t buf_phys, uint32_t total_bytes,
                           uint32_t pkt_bytes);

// #323: PIT tick counter for the continuous-stream backstop timer.
// The PIT runs at ~250 Hz (see project notes).
extern volatile uint64_t timer_ticks;
#define UAC_TICKS_PER_SEC 250u

// --------------------------------------------------------------------------
// Driver state
// --------------------------------------------------------------------------
static struct {
    int          ready;
    xhci_controller_t *xhc;
    int          slot_id;
    int          if_num;        // audio streaming interface number (3)
    int          alt;           // alternate setting with the iso EP (1)
    int          ep_addr;       // endpoint address (0x03)
    int          dci;           // device context index of the iso EP (6)
    uint32_t     max_packet;    // wMaxPacketSize (400)
    uint32_t     interval;      // bInterval (1)
    uint32_t     rate;          // configured sample rate (Hz)
} uac;

// --------------------------------------------------------------------------
// Integer sine table, one full cycle, amplitude pre-scaled to ~0.30 full scale
// (peak ~9830). The kernel builds with -mno-sse/-mno-sse2, so no floating point
// is permitted; this table is precomputed and we synthesize tones with a
// fixed-point phase accumulator and pure integer math.
// --------------------------------------------------------------------------
#define UAC_TABLE_SIZE 256
static const int16_t uac_sine_tab[UAC_TABLE_SIZE] = {
         0,   241,   482,   723,   964,  1203,  1442,  1681,  1918,  2154,  2389,  2622,  2854,  3084,  3312,  3538,
      3762,  3984,  4203,  4420,  4634,  4845,  5054,  5259,  5461,  5660,  5856,  6048,  6236,  6421,  6601,  6778,
      6951,  7119,  7284,  7443,  7599,  7750,  7896,  8037,  8173,  8305,  8432,  8553,  8669,  8780,  8886,  8987,
      9082,  9171,  9255,  9334,  9407,  9474,  9536,  9591,  9641,  9685,  9724,  9756,  9783,  9803,  9818,  9827,
      9830,  9827,  9818,  9803,  9783,  9756,  9724,  9685,  9641,  9591,  9536,  9474,  9407,  9334,  9255,  9171,
      9082,  8987,  8886,  8780,  8669,  8553,  8432,  8305,  8173,  8037,  7896,  7750,  7599,  7443,  7284,  7119,
      6951,  6778,  6601,  6421,  6236,  6048,  5856,  5660,  5461,  5259,  5054,  4845,  4634,  4420,  4203,  3984,
      3762,  3538,  3312,  3084,  2854,  2622,  2389,  2154,  1918,  1681,  1442,  1203,   964,   723,   482,   241,
         0,  -241,  -482,  -723,  -964, -1203, -1442, -1681, -1918, -2154, -2389, -2622, -2854, -3084, -3312, -3538,
     -3762, -3984, -4203, -4420, -4634, -4845, -5054, -5259, -5461, -5660, -5856, -6048, -6236, -6421, -6601, -6778,
     -6951, -7119, -7284, -7443, -7599, -7750, -7896, -8037, -8173, -8305, -8432, -8553, -8669, -8780, -8886, -8987,
     -9082, -9171, -9255, -9334, -9407, -9474, -9536, -9591, -9641, -9685, -9724, -9756, -9783, -9803, -9818, -9827,
     -9830, -9827, -9818, -9803, -9783, -9756, -9724, -9685, -9641, -9591, -9536, -9474, -9407, -9334, -9255, -9171,
     -9082, -8987, -8886, -8780, -8669, -8553, -8432, -8305, -8173, -8037, -7896, -7750, -7599, -7443, -7284, -7119,
     -6951, -6778, -6601, -6421, -6236, -6048, -5856, -5660, -5461, -5259, -5054, -4845, -4634, -4420, -4203, -3984,
     -3762, -3538, -3312, -3084, -2854, -2622, -2389, -2154, -1918, -1681, -1442, -1203,  -964,  -723,  -482,  -241,
};

// --------------------------------------------------------------------------
// Configuration descriptor parser: find the audio streaming interface alt
// setting that carries an isochronous OUT endpoint, preferring 16-bit PCM.
// --------------------------------------------------------------------------
// #404 driver tier: the pure descriptor walk is a strangler seam. The input is
// UNTRUSTED (whatever bytes the attached USB device returns; drivers/xhci.c
// stages it in a 512-byte static buffer and passes the device-declared
// wTotalLength, clamped to 512, as len). The parse now lives behind
// uac_parse_config_seam(); the xHCI control transfers, SET_INTERFACE and the
// kprintf report stay in C below.

// Pure out-struct for the seam: descriptor bytes + len -> selection | reject.
// Field order/types mirror the live `uac` globals above, plus best_bits which
// the kprintf reports as "%d-bit". Must stay layout-identical to the Rust
// #[repr(C)] UsbUacSel in rustkern.rs.
typedef struct {
    int32_t  if_num;
    int32_t  alt;
    int32_t  ep_addr;
    uint32_t max_packet;
    uint32_t interval;
    int32_t  best_bits;
} usb_uac_sel_t;

_Static_assert(sizeof(usb_uac_sel_t) == 24, "usb_uac_sel_t must be 24 bytes");
_Static_assert(_Alignof(usb_uac_sel_t) == 4, "usb_uac_sel_t must be 4-aligned");

// VERBATIM C reference, kept for rollback + the boot differential. The ONLY
// edits vs the original in-place parse: writes to the file-static `uac` struct
// become writes through `out` (same fields, same order, same conditions) so the
// seam is pure, and the trailing kprintf moves to the caller. The walk, the
// guards and every cfg[] index are byte-for-byte the original.
//
// KNOWN, DELIBERATELY PRESERVED WEAKNESS (this is the reference, not the fix):
// cfg[i+2], cfg[i+3], cfg[i+6] (INTERFACE) and cfg[i+2..i+6] (ENDPOINT) are
// guarded ONLY by `blen >= 2`, so a crafted descriptor with a lying 2-byte
// INTERFACE/ENDPOINT reads up to 5 bytes past len. See the -DRUST_USB_DESC
// block in the Makefile for why that is real but not advisory-grade.
static int uac_parse_config_c(const uint8_t *cfg, int len, usb_uac_sel_t *out) {
    int cur_if = -1, cur_alt = -1;
    int cur_subclass = -1;
    int cur_bits = 0;           // bBitResolution from FORMAT_TYPE descriptor
    int found = 0;
    int best_bits = 0;

    int i = 0;
    while (i + 2 <= len) {
        int blen = cfg[i];
        int btype = cfg[i + 1];
        if (blen < 2 || i + blen > len) break;

        if (btype == 0x04) {            // INTERFACE descriptor
            cur_if = cfg[i + 2];
            cur_alt = cfg[i + 3];
            cur_subclass = cfg[i + 6]; // bInterfaceSubClass (2 = AudioStreaming)
            cur_bits = 0;
        } else if (btype == 0x24) {     // CS_INTERFACE
            int subtype = cfg[i + 2];
            if (subtype == 0x02 && blen >= 7) {   // FORMAT_TYPE
                cur_bits = cfg[i + 6];            // bBitResolution
            }
        } else if (btype == 0x05) {     // ENDPOINT descriptor
            int ep_addr = cfg[i + 2];
            int attrs = cfg[i + 3];
            int mps = cfg[i + 4] | (cfg[i + 5] << 8);
            int interval = cfg[i + 6];
            int is_iso = (attrs & 0x03) == 0x01;
            int is_out = (ep_addr & 0x80) == 0;
            if (is_iso && is_out && cur_subclass == 0x02 && cur_alt != 0) {
                // candidate: prefer 16-bit, else first iso OUT found
                int better = (!found) ||
                             (cur_bits == 16 && best_bits != 16);
                if (better) {
                    out->if_num = cur_if;
                    out->alt = cur_alt;
                    out->ep_addr = ep_addr;
                    out->max_packet = mps & 0x7FF;
                    out->interval = interval;
                    out->best_bits = cur_bits;
                    best_bits = cur_bits;
                    found = 1;
                }
            }
        }
        i += blen;
    }
    return found ? 0 : -1;
}

// Rust port (rustkern.rs). Bounds-checked by construction.
extern int uac_parse_config_rs(const uint8_t *cfg, int len, usb_uac_sel_t *out);

// Live strangler seam. With -DRUST_USB_DESC (set in the Makefile CFLAGS) the
// Rust port is the live parser; without it, the C twin above is. Rollback =
// drop the one flag and rebuild. The boot-time [RUST-DIFF] usb_desc self-test
// compares the two impls regardless of the flag.
static inline int uac_parse_config_seam(const uint8_t *cfg, int len,
                                        usb_uac_sel_t *out) {
#ifdef RUST_USB_DESC
    return uac_parse_config_rs(cfg, len, out);
#else
    return uac_parse_config_c(cfg, len, out);
#endif
}

// Returns 0 on success and fills if_num/alt/ep_addr/max_packet/interval.
static int uac_parse_config(const uint8_t *cfg, int len) {
    usb_uac_sel_t sel;
    memset(&sel, 0, sizeof(sel));
    if (uac_parse_config_seam(cfg, len, &sel) != 0) return -1;

    uac.if_num     = sel.if_num;
    uac.alt        = sel.alt;
    uac.ep_addr    = sel.ep_addr;
    uac.max_packet = sel.max_packet;
    uac.interval   = sel.interval;

    kprintf("[UAC] AS interface %d alt %d, EP 0x%02x, mps %u, bInterval %u, %d-bit\n",
            uac.if_num, uac.alt, uac.ep_addr, uac.max_packet,
            uac.interval, sel.best_bits);
    return 0;
}

// --------------------------------------------------------------------------
// #404 boot-time differential self-test for the usb_desc seam.
// Proves uac_parse_config_rs == uac_parse_config_c on THIS build, over
// well-formed UAC configuration descriptors, regardless of which one the
// -DRUST_USB_DESC flag makes live. Bounded, runs once (#426): no allocation,
// no blocking, no I/O beyond the two log lines.
//
// Vectors are generated well-formed ONLY (every bLength equals the
// descriptor's real size and the buffer is exactly the sum), because that is
// the set on which the C is memory-safe and the two impls are contractually
// required to agree byte-for-byte. On malformed input the two DELIBERATELY
// diverge (the C over-reads and keeps walking; the Rust stops), so counting a
// "mismatch" there would be counting the fix as a defect. The malformed-input
// behaviour is proven separately offline (300,000 vectors: 100,188 over-read
// in the C, 0 in the Rust) and reported as [RUST-SEC] below.
// --------------------------------------------------------------------------
void usb_desc_rust_selftest(void);
void usb_desc_rust_selftest(void) {
    static uint8_t buf[512];
    uint32_t seed = 0x5EED1234u;
    int vectors = 0, mism = 0;

    for (int iter = 0; iter < 2000; iter++) {
        // xorshift32, self-contained so the test cannot perturb kernel RNG state
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        uint32_t r = seed;
        int n = 0;

        // CONFIGURATION header (bLength 9)
        buf[n+0] = 9; buf[n+1] = 0x02; buf[n+2] = 0; buf[n+3] = 0;
        buf[n+4] = (uint8_t)(1 + (r % 3)); buf[n+5] = 1; buf[n+6] = 0;
        buf[n+7] = 0x80; buf[n+8] = 50;
        n += 9;

        int nif = 1 + (int)((r >> 3) % 4);
        for (int k = 0; k < nif && n + 9 <= (int)sizeof(buf); k++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            uint32_t q = seed;
            // INTERFACE (bLength 9)
            buf[n+0] = 9; buf[n+1] = 0x04;
            buf[n+2] = (uint8_t)(q % 4);
            buf[n+3] = (uint8_t)((q >> 2) % 3);
            buf[n+4] = (uint8_t)((q >> 4) % 3);
            buf[n+5] = 0x01;
            buf[n+6] = ((q >> 6) & 1) ? 0x02 : (uint8_t)((q >> 7) % 4);
            buf[n+7] = 0; buf[n+8] = 0;
            n += 9;

            // optional CS_INTERFACE FORMAT_TYPE (bLength 7..11)
            if ((q >> 9) & 1) {
                int cl = 7 + (int)((q >> 10) % 5);
                if (n + cl <= (int)sizeof(buf)) {
                    static const uint8_t bits[] = { 8, 16, 16, 24, 32 };
                    buf[n+0] = (uint8_t)cl; buf[n+1] = 0x24; buf[n+2] = 0x02;
                    buf[n+3] = 1; buf[n+4] = 2; buf[n+5] = 2;
                    buf[n+6] = bits[(q >> 13) % 5];
                    for (int z = 7; z < cl; z++) buf[n+z] = (uint8_t)(q >> z);
                    n += cl;
                }
            }
            // 0..2 ENDPOINT descriptors (bLength 7 or 9)
            int nep = (int)((q >> 16) % 3);
            for (int e = 0; e < nep; e++) {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                uint32_t p = seed;
                int el = (p & 1) ? 7 : 9;
                if (n + el > (int)sizeof(buf)) break;
                buf[n+0] = (uint8_t)el; buf[n+1] = 0x05;
                buf[n+2] = (uint8_t)(((p >> 1) & 1 ? 0x00 : 0x80) | (1 + ((p >> 2) % 7)));
                buf[n+3] = (uint8_t)((p >> 5) % 4);
                uint32_t mps = (p >> 7) % 1024;
                buf[n+4] = (uint8_t)(mps & 0xFF);
                buf[n+5] = (uint8_t)((mps >> 8) & 0xFF);
                buf[n+6] = (uint8_t)(1 + ((p >> 17) % 4));
                for (int z = 7; z < el; z++) buf[n+z] = (uint8_t)(p >> z);
                n += el;
            }
        }

        usb_uac_sel_t a, b;
        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        int ra = uac_parse_config_c(buf, n, &a);
        int rb = uac_parse_config_rs(buf, n, &b);
        vectors++;
        if (ra != rb || memcmp(&a, &b, sizeof(a)) != 0) mism++;
    }

    kprintf("[RUST-DIFF] usb_desc: %d vectors, %d mismatches -> %s\n",
            vectors, mism, mism == 0 ? "PASS" : "*** MISMATCH ***");
    bootlog_write("[RUST-DIFF] usb_desc: %d vectors, %d mismatches -> %s",
                  vectors, mism, mism == 0 ? "PASS" : "*** MISMATCH ***");
    kprintf("[RUST-SEC] usb_desc: C over-reads up to 5B past len on a lying "
            "2-byte INTERFACE/ENDPOINT (blen>=2 is the only guard); Rust "
            "confines by construction. LIVE=%s\n",
#ifdef RUST_USB_DESC
            "rust");
#else
            "c");
#endif
    bootlog_write("[RUST-SEC] usb_desc: C over-read class confined by Rust; LIVE=%s",
#ifdef RUST_USB_DESC
                  "rust");
#else
                  "c");
#endif
}

// --------------------------------------------------------------------------
// Set the UAC1 sampling frequency (endpoint control, 3-byte little-endian).
// --------------------------------------------------------------------------
static int uac_set_rate(uint32_t rate) {
    static uint8_t rb[4] __attribute__((aligned(16)));
    rb[0] = rate & 0xFF;
    rb[1] = (rate >> 8) & 0xFF;
    rb[2] = (rate >> 16) & 0xFF;
    // bmRequestType 0x22 (Host->Dev, Class, Endpoint), bRequest 0x01 SET_CUR
    // wValue = SAMPLING_FREQ_CONTROL (0x01) << 8, wIndex = endpoint address
    int r = xhci_control_transfer(uac.xhc, uac.slot_id,
                                  0x22, 0x01, 0x0100, uac.ep_addr,
                                  rb, 3);
    return (r == CC_SUCCESS || r == CC_SHORT_PACKET) ? 0 : -1;
}

static int uac_set_interface(int if_num, int alt) {
    // bmRequestType 0x01 (Host->Dev, Standard, Interface), bRequest 0x0B
    int r = xhci_control_transfer(uac.xhc, uac.slot_id,
                                  0x01, 0x0B, alt, if_num, NULL, 0);
    return (r == CC_SUCCESS || r == CC_SHORT_PACKET) ? 0 : -1;
}

// --------------------------------------------------------------------------
// Public: probe / setup
// --------------------------------------------------------------------------
int uac_probe(xhci_controller_t *xhc, int slot_id,
              uint16_t vid, uint16_t pid,
              const uint8_t *config, int config_len) {
    if (uac.ready) return 0;  // already have an output device

    kprintf("[UAC] Probing audio device %04x:%04x slot %d\n", vid, pid, slot_id);

    memset(&uac, 0, sizeof(uac));
    uac.xhc = xhc;
    uac.slot_id = slot_id;

    if (uac_parse_config(config, config_len) != 0) {
        kprintf("[UAC] No isochronous OUT endpoint found, not an output DAC\n");
        return -1;
    }

    // 1. SET_CONFIGURATION 1
    int r = xhci_control_transfer(xhc, slot_id, 0x00, 0x09, 1, 0, NULL, 0);
    if (r != CC_SUCCESS && r != CC_SHORT_PACKET) {
        kprintf("[UAC] SET_CONFIGURATION failed (cc=%d)\n", r);
        return -1;
    }
    kprintf("[UAC] Configuration 1 set\n");

    // 2. configure the isochronous OUT endpoint on the xHCI slot.
    int dci = xhci_configure_iso_out(xhc, slot_id, uac.ep_addr,
                                     uac.max_packet, uac.interval);
    if (dci < 0) {
        kprintf("[UAC] Iso endpoint configure (CONFIG_EP) failed\n");
        return -1;
    }
    uac.dci = dci;
    kprintf("[UAC] Iso OUT endpoint configured, DCI %d\n", dci);

    // 3. SET_INTERFACE to the alt setting that activates the iso EP.
    if (uac_set_interface(uac.if_num, uac.alt) != 0) {
        kprintf("[UAC] SET_INTERFACE alt %d failed\n", uac.alt);
        return -1;
    }
    kprintf("[UAC] alt%d set on interface %d\n", uac.alt, uac.if_num);

    // 4. set the sampling frequency.
    uac.rate = 48000;
    if (uac_set_rate(uac.rate) != 0) {
        kprintf("[UAC] set rate %u failed, trying 44100\n", uac.rate);
        uac.rate = 44100;
        if (uac_set_rate(uac.rate) != 0) {
            kprintf("[UAC] set sample rate failed\n");
            // not fatal on all DACs; many default to 48000 anyway
            uac.rate = 48000;
        }
    }
    kprintf("[UAC] sample rate %u Hz\n", uac.rate);

    uac.ready = 1;
    kprintf("[UAC] USB Audio Class DAC ready, slot %d\n", slot_id);
    return 0;
}

int uac_is_ready(void) { return uac.ready; }
uint32_t uac_sample_rate(void) { return uac.ready ? uac.rate : 0; }

// #335: frames submitted to the DAC in the current stream (for elapsed time).
static volatile uint64_t g_uac_frames_streamed = 0;
uint64_t uac_frames_streamed(void) { return g_uac_frames_streamed; }

// #336: software master gain + mute for the USB DAC. The DAC exposes no mixer
// register we can poke, so the tray Volume slider and Mute must scale the PCM
// samples themselves. g_uac_gain_q8 is Q8 fixed-point 0..256 (256 == unity /
// 100%); g_uac_muted forces silence. Both are read live by the streaming loop,
// so slider / mute changes take effect within one batch (~50 ms).
static volatile int g_uac_gain_q8 = 256;   // default unity (100%)
static volatile int g_uac_muted   = 0;
void uac_set_volume(int vol0_100) {
    if (vol0_100 < 0)   vol0_100 = 0;
    if (vol0_100 > 100) vol0_100 = 100;
    g_uac_gain_q8 = (vol0_100 * 256) / 100;
    kprintf("[UAC] software master gain -> %d%% (q8=%d)\n", vol0_100, g_uac_gain_q8);
}
void uac_set_mute(int mute) {
    g_uac_muted = mute ? 1 : 0;
    kprintf("[UAC] software master mute -> %s\n", g_uac_muted ? "ON" : "OFF");
}
int uac_get_gain_q8(void) { return g_uac_muted ? 0 : g_uac_gain_q8; }

// #336: scale a batch of interleaved S16 samples by the current master gain,
// or zero it when muted. Called by every DAC streaming loop just before submit
// so the tray Volume slider / Mute change what the DAC actually outputs.
static void uac_apply_gain(int16_t *sp, uint32_t nsamples) {
    int g = g_uac_muted ? 0 : g_uac_gain_q8;
    if (g == 256) return;                     // unity: no work
    if (g == 0) {
        for (uint32_t i = 0; i < nsamples; i++) sp[i] = 0;
    } else {
        for (uint32_t i = 0; i < nsamples; i++)
            sp[i] = (int16_t)(((int32_t)sp[i] * g) >> 8);
    }
}

// #329: set the DAC output rate for the next stream. The NuForce uDAC-3 is
// verified at 44100 and 48000; snap anything else to the nearest of those so
// direct-copy playback keeps correct pitch for the common MP3 rates.
int uac_set_output_rate(uint32_t rate) {
    if (!uac.ready) return -1;
    uint32_t r = rate;
    if (r != 44100 && r != 48000) r = (rate <= 46050) ? 44100 : 48000;
    if (uac_set_rate(r) == 0) { uac.rate = r; kprintf("[UAC] output rate -> %u Hz\n", r); return 0; }
    // If the SET_CUR failed the device likely stays at its default; still record
    // our intended rate so packet sizing matches what we stream.
    uac.rate = r;
    return -1;
}

// --------------------------------------------------------------------------
// PCM streaming. Builds a DMA buffer, splits it into per-frame iso packets
// (1 ms each at the device rate) and hands the batch to the controller.
// --------------------------------------------------------------------------
int uac_play_pcm(const int16_t *data, uint32_t frames) {
    if (!uac.ready) return -1;

    uint32_t bytes_per_frame = 2 * 2;                 // S16 stereo
    uint32_t pkt_frames = uac.rate / 1000;            // frames per 1ms packet
    if (pkt_frames == 0) pkt_frames = 48;
    uint32_t pkt_bytes = pkt_frames * bytes_per_frame;
    if (pkt_bytes > uac.max_packet) pkt_bytes = uac.max_packet;

    uint32_t total_bytes = frames * bytes_per_frame;

    // Allocate a contiguous DMA buffer (identity mapped).
    uint32_t npages = (total_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages == 0) npages = 1;
    uint64_t buf_phys = pmm_alloc_pages(npages);
    if (buf_phys == 0) {
        kprintf("[UAC] PCM buffer alloc failed (%u pages)\n", npages);
        return -1;
    }
    memcpy((void *)buf_phys, data, total_bytes);

    kprintf("[UAC] streaming %u frames (%u bytes, %u/pkt)\n",
            frames, total_bytes, pkt_bytes);

    int r = xhci_iso_submit(uac.xhc, uac.slot_id, uac.dci,
                            buf_phys, total_bytes, pkt_bytes);

    // Let the audio drain (1ms per packet) before freeing the buffer.
    uint32_t ms = total_bytes / pkt_bytes + 50;
    proc_sleep(ms);

    pmm_free_pages(buf_phys, npages);
    return r;
}

int uac_tone(uint32_t freq, uint32_t duration_ms) {
    if (!uac.ready) return -1;

    uint32_t rate = uac.rate;
    uint32_t frames = (rate * duration_ms) / 1000;
    if (frames == 0) return -1;

    // Generate sine into a temporary DMA buffer using the integer table
    // (no FPU here, so preemption mid-loop is harmless).
    uint32_t bytes = frames * 4;
    uint32_t npages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(npages);
    if (phys == 0) {
        kprintf("[UAC] tone buffer alloc failed\n");
        return -1;
    }
    int16_t *buf = (int16_t *)phys;

    // 16.16 fixed-point phase accumulator stepping through the sine table.
    uint32_t phase = 0;
    uint32_t step = (uint32_t)(((uint64_t)freq * UAC_TABLE_SIZE << 16) / rate);
    for (uint32_t n = 0; n < frames; n++) {
        int16_t s = uac_sine_tab[(phase >> 16) & (UAC_TABLE_SIZE - 1)];
        buf[n * 2 + 0] = s;     // L
        buf[n * 2 + 1] = s;     // R
        phase += step;
    }

    kprintf("[UAC] tone playing: %u Hz for %u ms (%u frames @ %u Hz)\n",
            freq, duration_ms, frames, rate);

    uint32_t pkt_frames = rate / 1000;
    if (pkt_frames == 0) pkt_frames = 48;
    uint32_t pkt_bytes = pkt_frames * 4;
    if (pkt_bytes > uac.max_packet) pkt_bytes = uac.max_packet;

    int r = xhci_iso_submit(uac.xhc, uac.slot_id, uac.dci,
                            phys, bytes, pkt_bytes);

    proc_sleep(duration_ms + 80);

    // Drain the completion event from the last TD so serial shows the iso
    // transfer completion code (proof the controller serviced the ring).
    xhci_poll_events(uac.xhc);

    pmm_free_pages(phys, npages);
    return r;
}

// --------------------------------------------------------------------------
// Continuous isochronous streaming engine (#323).
//
// A reusable kernel-side streaming loop that keeps the iso OUT ring topped up
// so audio plays gaplessly with no ring-underrun stalls. A source callback
// fills S16 stereo frames on demand; the first source is the continuous test
// tone, but the same loop is the foundation for real WAV playback later.
//
// Flow control is drift-free: every batch flags IOC on its last iso TD, the
// xHCI driver counts those transfer events (xhci_iso_xfer_events), and we keep
// exactly UAC_TARGET_BATCHES batches in flight. We never enqueue more than
// UAC_TARGET_BATCHES * UAC_BATCH_PKTS TRBs ahead of the controller, which is
// far inside the 1024-entry iso ring, so we never overwrite an unconsumed TD.
// --------------------------------------------------------------------------
#define UAC_BATCH_PKTS     50    // iso packets (1 ms each) per submit batch
#define UAC_NSLOTS         8     // DMA batch buffers in the pool (must exceed
                                 // UAC_TARGET_BATCHES so a slot is free on reuse)
#define UAC_TARGET_BATCHES 4     // batches kept in flight (~lookahead window)

// Source callback type uac_src_fn is declared in usb_audio.h (#329).

// Continuous tone source: a fixed-point phase accumulator walking the integer
// sine table. The phase persists across batches so the tone is perfectly
// continuous with no loop seam. Amplitude is doubled to ~0.6 full scale.
typedef struct {
    uint32_t phase;   // 16.16 fixed-point index into the sine table
    uint32_t step;    // phase increment per frame
} uac_tone_src_t;

static int uac_tone_fill(int16_t *dst, uint32_t frames, void *ctx) {
    uac_tone_src_t *t = (uac_tone_src_t *)ctx;
    uint32_t phase = t->phase;
    uint32_t step = t->step;
    for (uint32_t n = 0; n < frames; n++) {
        // table peak ~9830 (0.30 FS); x2 -> ~19660 (~0.60 FS), no clipping.
        int32_t s = (int32_t)uac_sine_tab[(phase >> 16) & (UAC_TABLE_SIZE - 1)] * 2;
        dst[n * 2 + 0] = (int16_t)s;   // L
        dst[n * 2 + 1] = (int16_t)s;   // R
        phase += step;
    }
    t->phase = phase;
    return 0;
}

// Run a streaming source for duration_ms milliseconds, then stop cleanly.
static int uac_stream_run(uac_src_fn src, void *ctx, uint32_t duration_ms) {
    if (!uac.ready) return -1;

    uint32_t pkt_frames = uac.rate / 1000;
    if (pkt_frames == 0) pkt_frames = 48;
    uint32_t pkt_bytes = pkt_frames * 4;             // S16 stereo
    if (pkt_bytes > uac.max_packet) {
        pkt_bytes = uac.max_packet & ~3u;            // keep frame-aligned
        pkt_frames = pkt_bytes / 4;
    }
    uint32_t slot_frames = pkt_frames * UAC_BATCH_PKTS;
    uint32_t slot_bytes  = slot_frames * 4;
    uint32_t pool_bytes  = slot_bytes * UAC_NSLOTS;
    uint32_t npages = (pool_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages == 0) npages = 1;

    uint64_t pool = pmm_alloc_pages(npages);
    if (pool == 0) {
        kprintf("[UAC] stream: DMA pool alloc failed (%u pages)\n", npages);
        return -1;
    }

    xhci_ring_t *ring = uac.xhc->transfer_rings[uac.slot_id - 1][uac.dci];
    if (!ring) {
        kprintf("[UAC] stream: no iso ring for DCI %d\n", uac.dci);
        pmm_free_pages(pool, npages);
        return -1;
    }

    uint32_t submitted = 0;
    uint32_t slot_idx = 0;
    xhci_iso_xfer_events = 0;       // reset flow-control counter
    xhci_iso_quiet = 1;            // suppress per-batch transfer-event spam

    uint64_t start = timer_ticks;
    uint64_t end   = start + (uint64_t)duration_ms * UAC_TICKS_PER_SEC / 1000;
    uint64_t hb    = start + UAC_TICKS_PER_SEC * 30;   // heartbeat every 30 s

    kprintf("[UAC] continuous tone worker started (%u-frame batches, %u pkt/batch, %u in flight, %u Hz, gain q8=%d mute=%d)\n",
            slot_frames, (uint32_t)UAC_BATCH_PKTS, (uint32_t)UAC_TARGET_BATCHES, uac.rate,
            g_uac_gain_q8, g_uac_muted);

    while (timer_ticks < end) {
        xhci_poll_events(uac.xhc);                 // drain events, update counter
        uint32_t completed = xhci_iso_xfer_events;
        // Clamp: iso Ring-Underrun/Overrun events also tick the counter, so
        // completed can momentarily exceed submitted; treat that as 0 in flight
        // (we fell behind) and refill, which self-heals via the SIA bit.
        uint32_t in_flight = (submitted > completed) ? (submitted - completed) : 0;

        // Top the ring back up to the in-flight target.
        while (in_flight < (uint32_t)UAC_TARGET_BATCHES) {
            uint64_t base = pool + (uint64_t)slot_idx * slot_bytes;
            src((int16_t *)base, slot_frames, ctx);
            uac_apply_gain((int16_t *)base, slot_frames * 2u);   // #336

            for (uint32_t pk = 0; pk < UAC_BATCH_PKTS; pk++) {
                xhci_trb_t *trb = xhci_ring_enqueue(ring);
                trb->parameter = base + (uint64_t)pk * pkt_bytes;
                trb->status = pkt_bytes & 0x1FFFF;
                uint32_t ctrl = XHCI_TRB_TYPE(TRB_ISOCH) | (1u << 31) | ring->cycle_bit;
                if (pk == UAC_BATCH_PKTS - 1) ctrl |= TRB_IOC;   // 1 event/batch
                trb->control = ctrl;
            }
            __asm__ volatile("mfence" ::: "memory");
            xhci_ring_doorbell(uac.xhc, uac.slot_id, uac.dci);

            submitted++;
            slot_idx = (slot_idx + 1) % UAC_NSLOTS;
            completed = xhci_iso_xfer_events;
            in_flight = (submitted > completed) ? (submitted - completed) : 0;
        }

        if (timer_ticks >= hb) {
            kprintf("[UAC] continuous tone streaming: %u batches submitted, %u completed (gain q8=%d mute=%d)\n",
                    submitted, completed, g_uac_gain_q8, g_uac_muted);
            hb += UAC_TICKS_PER_SEC * 30;
        }
        proc_sleep(8);                              // ~2 PIT ticks; do not peg CPU
    }

    xhci_iso_quiet = 0;
    kprintf("[UAC] continuous tone worker stopping (backstop reached after %u ms)\n",
            duration_ms);
    proc_sleep(300);                                // let in-flight TDs drain
    xhci_poll_events(uac.xhc);
    pmm_free_pages(pool, npages);
    return 0;
}

// Public: play a steady continuous tone for duration_ms (safety backstop).
int uac_play_tone_continuous(uint32_t freq, uint32_t duration_ms) {
    if (!uac.ready) return -1;
    uac_tone_src_t src;
    src.phase = 0;
    src.step = (uint32_t)(((uint64_t)freq * UAC_TABLE_SIZE << 16) / uac.rate);
    return uac_stream_run(uac_tone_fill, &src, duration_ms);
}

// --------------------------------------------------------------------------
// #329: finite PCM streaming. Same gapless ring-refill engine as the tone
// worker, but the source callback returns the number of frames it produced,
// so an arbitrary-length track (e.g. decoded MP3) plays to its natural end and
// then stops cleanly. max_ms is a safety backstop only. This is the path the
// music player's decoded PCM flows through (audio_play_file -> here -> DAC).
// --------------------------------------------------------------------------
int uac_stream_source(uac_src_fn src, void *ctx, uint32_t max_ms) {
    if (!uac.ready) return -1;

    uint32_t pkt_frames = uac.rate / 1000;
    if (pkt_frames == 0) pkt_frames = 48;
    uint32_t pkt_bytes = pkt_frames * 4;             // S16 stereo
    if (pkt_bytes > uac.max_packet) {
        pkt_bytes = uac.max_packet & ~3u;            // keep frame-aligned
        pkt_frames = pkt_bytes / 4;
    }
    uint32_t slot_frames = pkt_frames * UAC_BATCH_PKTS;
    uint32_t slot_bytes  = slot_frames * 4;
    uint32_t pool_bytes  = slot_bytes * UAC_NSLOTS;
    uint32_t npages = (pool_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages == 0) npages = 1;

    uint64_t pool = pmm_alloc_pages(npages);
    if (pool == 0) {
        kprintf("[UAC] pcm stream: DMA pool alloc failed (%u pages)\n", npages);
        return -1;
    }

    xhci_ring_t *ring = uac.xhc->transfer_rings[uac.slot_id - 1][uac.dci];
    if (!ring) {
        kprintf("[UAC] pcm stream: no iso ring for DCI %d\n", uac.dci);
        pmm_free_pages(pool, npages);
        return -1;
    }

    uint32_t submitted = 0;
    uint32_t slot_idx = 0;
    int eof = 0;
    xhci_iso_xfer_events = 0;
    xhci_iso_quiet = 1;
    g_uac_frames_streamed = 0;

    uint64_t start = timer_ticks;
    uint64_t end   = start + (uint64_t)max_ms * UAC_TICKS_PER_SEC / 1000;

    kprintf("[UAC] pcm stream worker started (%u-frame batches, %u pkt/batch, %u in flight, %u Hz, gain q8=%d mute=%d)\n",
            slot_frames, (uint32_t)UAC_BATCH_PKTS, (uint32_t)UAC_TARGET_BATCHES, uac.rate,
            g_uac_gain_q8, g_uac_muted);

    while (!eof && timer_ticks < end) {
        xhci_poll_events(uac.xhc);
        uint32_t completed = xhci_iso_xfer_events;
        uint32_t in_flight = (submitted > completed) ? (submitted - completed) : 0;

        while (!eof && in_flight < (uint32_t)UAC_TARGET_BATCHES) {
            uint64_t base = pool + (uint64_t)slot_idx * slot_bytes;
            int got = src((int16_t *)base, slot_frames, ctx);
            if (got < 0) got = 0;
            if ((uint32_t)got < slot_frames) {
                // last batch: pad the unfilled tail with silence, then finish.
                memset((void *)(base + (uint64_t)got * 4), 0,
                       (size_t)(slot_frames - (uint32_t)got) * 4);
                eof = 1;
            }
            g_uac_frames_streamed += (uint32_t)got;
            uac_apply_gain((int16_t *)base, slot_frames * 2u);   // #336: master gain/mute

            for (uint32_t pk = 0; pk < UAC_BATCH_PKTS; pk++) {
                xhci_trb_t *trb = xhci_ring_enqueue(ring);
                trb->parameter = base + (uint64_t)pk * pkt_bytes;
                trb->status = pkt_bytes & 0x1FFFF;
                uint32_t ctrl = XHCI_TRB_TYPE(TRB_ISOCH) | (1u << 31) | ring->cycle_bit;
                if (pk == UAC_BATCH_PKTS - 1) ctrl |= TRB_IOC;
                trb->control = ctrl;
            }
            __asm__ volatile("mfence" ::: "memory");
            xhci_ring_doorbell(uac.xhc, uac.slot_id, uac.dci);

            submitted++;
            slot_idx = (slot_idx + 1) % UAC_NSLOTS;
            completed = xhci_iso_xfer_events;
            in_flight = (submitted > completed) ? (submitted - completed) : 0;
        }
        proc_sleep(8);
    }

    xhci_iso_quiet = 0;
    kprintf("[UAC] pcm stream stopping (%s, %u batches submitted, %llu frames)\n",
            eof ? "end of track" : "backstop",
            submitted, (unsigned long long)g_uac_frames_streamed);
    proc_sleep(300);                                // let in-flight TDs drain
    xhci_poll_events(uac.xhc);
    pmm_free_pages(pool, npages);
    return 0;
}

// --------------------------------------------------------------------------
// Boot self-test: deferred kernel worker that plays an audible tone once the
// scheduler is running so the user can physically confirm the DAC works.
// --------------------------------------------------------------------------
extern fat_fs_t g_fat_fs;

static void uac_boot_worker(void *arg) {
    (void)arg;
    proc_sleep(4000);   // let boot settle a few seconds (net, desktop start)
    if (!uac_is_ready()) {
        kprintf("[UAC] boot self-test: no DAC present, skipping tone\n");
        return;
    }
    // #329: the DAC is now the system audio sink (music player streams through
    // it), so the diagnostic 440 Hz tone must NOT auto-play and hog the ring.
    // It is gated behind /CONFIG/UACTONE.CFG for hardware bring-up only.
    uint32_t sz = 0;
    void *cfg = fat_read_file(&g_fat_fs, "/CONFIG/UACTONE.CFG", &sz);
    if (!cfg) {
        kprintf("[UAC] boot self-test: DAC ready; tone gated off (no /CONFIG/UACTONE.CFG), sink free for playback\n");
        return;
    }
    kfree(cfg);
    kprintf("[UAC] boot self-test: starting CONTINUOUS 440 Hz tone (~0.6 amplitude, 15 min backstop)\n");
    uac_play_tone_continuous(440, 15u * 60u * 1000u);
    kprintf("[UAC] boot self-test complete\n");
}

void uac_start_boot_selftest(void) {
    proc_create("uac_tone", uac_boot_worker, NULL, PRIO_NORMAL);
}
