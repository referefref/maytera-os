// usb_asix.c - ASIX AX88772 (USB2) and AX88179 (USB3) USB Ethernet vendor
// drivers (#362). These are the controllers inside Apple's own USB Ethernet
// adapter (05ac:1402) and most common USB-to-Ethernet dongles.
//
// Register-level init sequences follow the Linux asix/ax88179_178a drivers
// (GPL reference used for register-level facts only). The shared RX/TX core
// lives in usb_ecm.c; this file provides:
//   - vendor bring-up (PHY select, soft reset, MAC read, medium mode, RX ctl)
//   - RX de-framing (AX88772: per-packet 32-bit length header; AX88179:
//     aggregated transfer with a trailing descriptor block)
//   - lazy PHY link polling (BMSR) for nic_link_up()
//
// NOTE: QEMU emulates neither chip, so this code is verified to compile and
// to be inert without the hardware; first live test is the real dongle on
// the iMac. The CDC-ECM path (usb_ecm.c) is the QEMU-verified path.
#include "usb_net.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/bootlog.h"

// --- AX88772 vendor requests (bRequest values) ---
#define AX_CMD_SET_SW_MII        0x06
#define AX_CMD_READ_MII_REG      0x07
#define AX_CMD_WRITE_MII_REG     0x08
#define AX_CMD_SET_HW_MII        0x0A
#define AX_CMD_READ_RX_CTL       0x0F
#define AX_CMD_WRITE_RX_CTL      0x10
#define AX_CMD_WRITE_IPG0        0x12
#define AX_CMD_READ_NODE_ID      0x13
#define AX_CMD_READ_PHY_ID       0x19
#define AX_CMD_READ_MEDIUM_MODE  0x1A
#define AX_CMD_WRITE_MEDIUM_MODE 0x1B
#define AX_CMD_WRITE_GPIOS       0x1F
#define AX_CMD_SW_RESET          0x20
#define AX_CMD_SW_PHY_SELECT     0x22
#define AX_CMD_STATMNGSTS_REG    0x09   // status/manage: low bits carry chip code

// AX88772 family chip code (masked from STATMNGSTS). The dongle on the iMac is
// an AX88772B (0b95:7e2b); reading this confirms the embedded-PHY bring-up path.
#define AX_CHIPCODE_MASK         0x70
#define AX_AX88772_CHIPCODE      0x00
#define AX_AX88772A_CHIPCODE     0x10
#define AX_AX88772B_CHIPCODE     0x20

// AX88772 bit values
#define AX_GPIO_RSE              0x80
#define AX_GPIO_GPO_2            0x20
#define AX_GPIO_GPO2EN           0x10
#define AX_SWRESET_CLEAR         0x00
#define AX_SWRESET_PRTE          0x04
#define AX_SWRESET_PRL           0x08
#define AX_SWRESET_IPRL          0x20
#define AX_SWRESET_IPPD          0x40
#define AX88772_MEDIUM_DEFAULT   0x0336   // FD|RFC|TFC|PS|AC|RE
#define AX_DEFAULT_RX_CTL        0x0088   // SO|AB (start + broadcast)
#define AX88772_IPG_DEFAULT      0x0C15   // IPG0 0x15 | IPG1 0x0C << 8
#define AX88772_IPG2_DEFAULT     0x12

// MII registers
#define MII_BMCR                 0x00
#define MII_BMSR                 0x01
#define MII_ADVERTISE            0x04
#define BMCR_RESET               0x8000
#define BMCR_ANENABLE            0x1000
#define BMCR_ANRESTART           0x0200
#define BMSR_LSTATUS             0x0004
#define ADVERTISE_ALL_CSMA       0x01E1   // 10/100 HD+FD | CSMA

// --- AX88179 vendor requests + registers ---
#define AX179_ACCESS_MAC         0x01
#define AX179_ACCESS_PHY         0x02
#define AX179_PHY_ID             0x03
#define AX179_REG_PHYSICAL_LINK  0x02     // 1 byte
#define AX179_REG_RX_CTL         0x0B     // 2 bytes
#define AX179_REG_NODE_ID        0x10     // 6 bytes
#define AX179_REG_MEDIUM_MODE    0x22     // 2 bytes
#define AX179_REG_PHYPWR_RSTCTL  0x26     // 2 bytes
#define AX179_REG_BULKIN_QCTRL   0x2E     // 5 bytes
#define AX179_REG_CLK_SELECT     0x33     // 1 byte
#define AX179_REG_RXCOE_CTL      0x34     // 1 byte
#define AX179_REG_TXCOE_CTL      0x35     // 1 byte
#define AX179_REG_PAUSE_LVL_LOW  0x54     // 1 byte
#define AX179_REG_PAUSE_LVL_HIGH 0x55     // 1 byte
#define AX179_PHYPWR_IPRL        0x0020
#define AX179_CLK_ACS_BCS        0x03
// RX_CTL: DROPCRCERR 0x0200 | IPE 0x0100 | SO 0x0080 | AB 0x0008 | AM 0x0010
#define AX179_RX_CTL_DEFAULT     0x0398
// MEDIUM: RECEIVE_EN 0x0100 | TXFLOW 0x0020 | RXFLOW 0x0010 | EN_125MHZ 0x0008
//         | FULL_DUPLEX 0x0002 | GIGAMODE 0x0001
#define AX179_MEDIUM_DEFAULT     0x013B

// Small DMA scratch for vendor control transfers (identity-mapped static).
static uint8_t vbuf[64] __attribute__((aligned(64)));

// #62: control-transfer budget for ASIX vendor register access.
//
// Bring-up runs ONCE and a cold chip legitimately takes a while, so it keeps the
// generous enumeration budget. Everything AFTER attach is a PERIODIC POLL on the
// background net worker (usb_asix_poll_link, once a second), and at the 5s
// default a dongle that stops answering costs 5 SECONDS PER REGISTER: one link
// poll is 2 MII reads of up to 3 vendor transfers each, so ~30s of blocking per
// second of wall clock, forever. Measured on the owner's iMac14,4 as 13 x
// "Transfer wait TIMEOUT slot 1 DCI 1 (5000ms budget; 5006ms real)" in one boot.
// This is the same reasoning, and the same shape of fix, as the hub class-request
// budget (#373, HUB_CTRL_TIMEOUT_MS). A healthy ASIX answers a vendor request in
// well under a millisecond, so 250ms is a ~250x margin, not a tightened race.
#define AX_INIT_CTRL_MS     5000
#define AX_RUNTIME_CTRL_MS   250
static uint32_t s_ax_ctrl_ms = AX_INIT_CTRL_MS;

// Real wall-clock delay: the ASIX reset sequence needs true waits (up to
// 150ms) with interrupts off, so use the PIT-calibrated xhci delay.
static void asix_delay_ms(uint32_t ms) {
    xhci_delay_ms(ms);
}

static int axw(usbnet_dev_t *d, uint8_t req, uint16_t value, uint16_t index,
               const void *data, uint16_t len) {
    if (len > sizeof(vbuf)) return -1;
    if (data && len) memcpy(vbuf, data, len);
    int cc = xhci_control_transfer_to(d->xhc, d->slot_id, 0x40, req, value, index,
                                      (data && len) ? vbuf : NULL, len,
                                      s_ax_ctrl_ms);
    return (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? 0 : -1;
}

static int axr(usbnet_dev_t *d, uint8_t req, uint16_t value, uint16_t index,
               void *data, uint16_t len) {
    if (len > sizeof(vbuf)) return -1;
    memset(vbuf, 0, len);
    int cc = xhci_control_transfer_to(d->xhc, d->slot_id, 0xC0, req, value, index,
                                      vbuf, len, s_ax_ctrl_ms);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) return -1;
    if (data && len) memcpy(data, vbuf, len);
    return 0;
}

// --- AX88772 MII helpers ---
static int ax772_mii_write(usbnet_dev_t *d, uint8_t phy, uint8_t reg, uint16_t val) {
    uint8_t b[2] = { (uint8_t)(val & 0xFF), (uint8_t)(val >> 8) };
    if (axw(d, AX_CMD_SET_SW_MII, 0, 0, NULL, 0) != 0) return -1;
    int r = axw(d, AX_CMD_WRITE_MII_REG, phy, reg, b, 2);
    axw(d, AX_CMD_SET_HW_MII, 0, 0, NULL, 0);
    return r;
}

static int ax772_mii_read(usbnet_dev_t *d, uint8_t phy, uint8_t reg, uint16_t *out) {
    uint8_t b[2] = { 0, 0 };
    if (axw(d, AX_CMD_SET_SW_MII, 0, 0, NULL, 0) != 0) return -1;
    int r = axr(d, AX_CMD_READ_MII_REG, phy, reg, b, 2);
    axw(d, AX_CMD_SET_HW_MII, 0, 0, NULL, 0);
    if (r != 0) return -1;
    *out = (uint16_t)(b[0] | (b[1] << 8));
    return 0;
}

static uint8_t g_ax772_phy = 0x10;   // embedded PHY address

// =============================================================================
// AX88772 bring-up (Linux asix_devices.c ax88772_hw_reset, simplified)
// =============================================================================
static int ax88772_init(usbnet_dev_t *d) {
    // 1) GPIO: enable GPO_2 (PHY power on many boards), reload EEPROM state.
    axw(d, AX_CMD_WRITE_GPIOS, AX_GPIO_RSE | AX_GPIO_GPO_2 | AX_GPIO_GPO2EN,
        0, NULL, 0);
    asix_delay_ms(25);

    // 2) PHY select: embedded PHY sits at address 0x10.
    uint8_t phyid[2] = { 0, 0 };
    if (axr(d, AX_CMD_READ_PHY_ID, 0, 0, phyid, 2) != 0) {
        kprintf("[AX88772] READ_PHY_ID failed\n");
        return -1;
    }
    g_ax772_phy = phyid[1] & 0x1F;
    int embedded = (g_ax772_phy == 0x10);
    axw(d, AX_CMD_SW_PHY_SELECT, embedded ? 0x0001 : 0x0000, 0, NULL, 0);

    // 2b) #378: read the chip code so we know we are on an AX88772B (embedded
    // PHY at 0x10) and log it. Diagnostic + confirms the bring-up path.
    uint8_t stat = 0;
    axr(d, AX_CMD_STATMNGSTS_REG, 0, 0, &stat, 1);
    uint8_t chipcode = stat & AX_CHIPCODE_MASK;
    const char *chipname = (chipcode == AX_AX88772_CHIPCODE)  ? "AX88772"  :
                           (chipcode == AX_AX88772A_CHIPCODE) ? "AX88772A" :
                           (chipcode == AX_AX88772B_CHIPCODE) ? "AX88772B" : "AX88772?";
    bootlog_write("[AX88772] chip=%s code=0x%02x (stat 0x%02x) phy=0x%02x embd=%d",
                  chipname, chipcode, stat, g_ax772_phy, embedded);

    // 3) Soft reset dance. #378: 772B embedded-PHY ordering per the Linux asix
    // driver: power the internal PHY DOWN (IPPD) with the MAC in reset (PRL),
    // then CLEAR (drops IPPD so the PHY powers back up), then assert IPRL
    // (Internal PHY Reset reLease) so the embedded PHY leaves reset and starts
    // autonegotiation. IPPD must be cleared and IPRL asserted, in this order,
    // or the embedded PHY at addr 0x10 stays powered down and never links.
    axw(d, AX_CMD_SW_RESET, AX_SWRESET_IPPD | AX_SWRESET_PRL, 0, NULL, 0);
    asix_delay_ms(150);
    axw(d, AX_CMD_SW_RESET, AX_SWRESET_CLEAR, 0, NULL, 0);   // IPPD cleared
    asix_delay_ms(150);
    axw(d, AX_CMD_SW_RESET, embedded ? AX_SWRESET_IPRL : AX_SWRESET_PRTE,
        0, NULL, 0);                                         // IPRL asserted
    asix_delay_ms(150);

    // 4) RX off during setup.
    axw(d, AX_CMD_WRITE_RX_CTL, 0x0000, 0, NULL, 0);

    // 5) MAC address from the chip's EEPROM-backed node ID.
    if (axr(d, AX_CMD_READ_NODE_ID, 0, 0, d->mac, 6) != 0) {
        kprintf("[AX88772] READ_NODE_ID failed\n");
        return -1;
    }

    // 6) PHY: reset, advertise 10/100 HD/FD, restart autonegotiation.
    ax772_mii_write(d, g_ax772_phy, MII_BMCR, BMCR_RESET);
    asix_delay_ms(30);
    ax772_mii_write(d, g_ax772_phy, MII_ADVERTISE, ADVERTISE_ALL_CSMA);
    ax772_mii_write(d, g_ax772_phy, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

    // 7) Medium mode, inter-packet gap, then RX on.
    axw(d, AX_CMD_WRITE_MEDIUM_MODE, AX88772_MEDIUM_DEFAULT, 0, NULL, 0);
    axw(d, AX_CMD_WRITE_IPG0, AX88772_IPG_DEFAULT, AX88772_IPG2_DEFAULT, NULL, 0);
    axw(d, AX_CMD_WRITE_RX_CTL, AX_DEFAULT_RX_CTL, 0, NULL, 0);

    // 8) Give autoneg a moment; record initial link state. #378: double-read
    // BMSR (latch-low), and log the first few raw reads. The full ~10s wait for
    // a real switch to autonegotiate happens later in dhcp_discover_blocking
    // (which also pumps RX); here we just give it ~4s so a fast link is already
    // up by the time DHCP starts.
    d->link = 0;
    for (int t = 0; t < 40; t++) {   // up to ~4s
        uint16_t bmsr = 0;
        ax772_mii_read(d, g_ax772_phy, MII_BMSR, &bmsr);   // discard latched-low
        if (ax772_mii_read(d, g_ax772_phy, MII_BMSR, &bmsr) == 0) {
            if (t < 6)
                bootlog_write("[AX88772] init BMSR[%d]=0x%04x", t, bmsr);
            if (bmsr & BMSR_LSTATUS) { d->link = 1; break; }
        }
        asix_delay_ms(100);
    }
    kprintf("[AX88772] init done, link %s\n", d->link ? "UP" : "down (will poll)");
    return 0;
}

// =============================================================================
// AX88179 bring-up (Linux ax88179_178a.c, simplified)
// =============================================================================
static int ax88179_init(usbnet_dev_t *d) {
    uint8_t b1;
    uint8_t b2[2];
    // 1) Power/reset: PHY power down, then IPRL (internal PHY reset release).
    b2[0] = 0; b2[1] = 0;
    axw(d, AX179_ACCESS_MAC, AX179_REG_PHYPWR_RSTCTL, 2, b2, 2);
    asix_delay_ms(200);
    b2[0] = (uint8_t)(AX179_PHYPWR_IPRL & 0xFF);
    b2[1] = (uint8_t)(AX179_PHYPWR_IPRL >> 8);
    axw(d, AX179_ACCESS_MAC, AX179_REG_PHYPWR_RSTCTL, 2, b2, 2);
    asix_delay_ms(200);

    // 2) Clock select: auto + backup clock.
    b1 = AX179_CLK_ACS_BCS;
    axw(d, AX179_ACCESS_MAC, AX179_REG_CLK_SELECT, 1, &b1, 1);
    asix_delay_ms(100);

    // 3) MAC address.
    if (axr(d, AX179_ACCESS_MAC, AX179_REG_NODE_ID, 6, d->mac, 6) != 0) {
        kprintf("[AX88179] NODE_ID read failed\n");
        return -1;
    }
    axw(d, AX179_ACCESS_MAC, AX179_REG_NODE_ID, 6, d->mac, 6);

    // 4) Bulk-IN aggregation queue control (timer/size/boundary for USB3).
    {
        uint8_t q[5] = { 0x07, 0x4F, 0x00, 0x12, 0xFF };
        axw(d, AX179_ACCESS_MAC, AX179_REG_BULKIN_QCTRL, 5, q, 5);
    }

    // 5) Pause water levels.
    b1 = 0x34; axw(d, AX179_ACCESS_MAC, AX179_REG_PAUSE_LVL_LOW, 1, &b1, 1);
    b1 = 0x52; axw(d, AX179_ACCESS_MAC, AX179_REG_PAUSE_LVL_HIGH, 1, &b1, 1);

    // 6) Checksum offload OFF both directions (we parse raw frames).
    b1 = 0; axw(d, AX179_ACCESS_MAC, AX179_REG_RXCOE_CTL, 1, &b1, 1);
    b1 = 0; axw(d, AX179_ACCESS_MAC, AX179_REG_TXCOE_CTL, 1, &b1, 1);

    // 7) PHY: restart autonegotiation (internal PHY id 3, BMCR).
    b2[0] = (uint8_t)((BMCR_ANENABLE | BMCR_ANRESTART) & 0xFF);
    b2[1] = (uint8_t)((BMCR_ANENABLE | BMCR_ANRESTART) >> 8);
    axw(d, AX179_ACCESS_PHY, AX179_PHY_ID, MII_BMCR, b2, 2);

    // 8) Medium mode + RX control on.
    b2[0] = (uint8_t)(AX179_MEDIUM_DEFAULT & 0xFF);
    b2[1] = (uint8_t)(AX179_MEDIUM_DEFAULT >> 8);
    axw(d, AX179_ACCESS_MAC, AX179_REG_MEDIUM_MODE, 2, b2, 2);
    b2[0] = (uint8_t)(AX179_RX_CTL_DEFAULT & 0xFF);
    b2[1] = (uint8_t)(AX179_RX_CTL_DEFAULT >> 8);
    axw(d, AX179_ACCESS_MAC, AX179_REG_RX_CTL, 2, b2, 2);

    // 9) Wait briefly for link (PHYSICAL_LINK_STATUS register).
    d->link = 0;
    for (int t = 0; t < 30; t++) {
        uint8_t pls = 0;
        if (axr(d, AX179_ACCESS_MAC, AX179_REG_PHYSICAL_LINK, 1, &pls, 1) == 0 &&
            (pls & 0x07)) {   // 10/100/1000 link bits
            d->link = 1;
            break;
        }
        asix_delay_ms(100);
    }
    kprintf("[AX88179] init done, link %s\n", d->link ? "UP" : "down (will poll)");
    return 0;
}

// =============================================================================
// Link detection
//
// #378: the AX88772B embedded-PHY BMSR Link-Status bit (0x0004) is LATCHING-LOW
// per IEEE 802.3: a single read returns the LATCHED (old, down) value and only
// the SECOND read reports the current link. The lazy poller here always read
// once, so on the iMac dongle it reported link DOWN forever even while RX was
// flowing, which stalled DHCP and tripped the net-up gate. Every BMSR read now
// reads TWICE and keeps the second. usb_eth_link_up() also drives an ACTIVE
// (double-)read through a short time cache, so nic_link_up() is real-time
// rather than depending on the counter-gated RX poll to have run.
// =============================================================================

// Shared cache tick so both the RX-path poll and the on-demand link_up() query
// keep d->link and the cache freshness consistent (no permanent staleness even
// if one path is quiet).
static uint64_t s_link_tick = 0;
static int      s_link_primed = 0;

// One active PHY read (double-read BMSR to clear the latch), updates d->link.
// #62: returns 0 if the PHY ANSWERED and -1 if the control transfer failed, so
// the poller above can tell "link is down" (device answered, no carrier) apart
// from "device is not answering at all". Previously it returned d->link, which
// made those two indistinguishable and let the caller poll a dead device
// forever at 5 seconds per register.
static int usb_asix_read_link_now(usbnet_dev_t *d) {
    static uint32_t diag = 0;
    if (d->type == USBNET_TYPE_AX88772) {
        uint16_t bmsr = 0;
        ax772_mii_read(d, g_ax772_phy, MII_BMSR, &bmsr);          // discard latch
        if (ax772_mii_read(d, g_ax772_phy, MII_BMSR, &bmsr) == 0) {
            d->link = (bmsr & BMSR_LSTATUS) ? 1 : 0;
            if (diag < 8) { diag++;
                bootlog_write("[AX88772] BMSR=0x%04x link=%d", bmsr, d->link); }
            return 0;
        }
        return -1;
    } else if (d->type == USBNET_TYPE_AX88179) {
        uint8_t pls = 0;
        axr(d, AX179_ACCESS_MAC, AX179_REG_PHYSICAL_LINK, 1, &pls, 1);  // discard
        if (axr(d, AX179_ACCESS_MAC, AX179_REG_PHYSICAL_LINK, 1, &pls, 1) == 0) {
            d->link = (pls & 0x07) ? 1 : 0;
            return 0;
        }
        return -1;
    }
    return -1;
}

// Lazy link refresh (called from usb_eth_receive).
//
// #381: this used to issue an active MII control-transfer read here, but
// usb_eth_receive() runs inside net_poll() under net_lock() (interrupts off),
// and a cable-less ASIX PHY can stall that read for seconds -> whole-UI freeze.
// PHY polling now happens EXCLUSIVELY on the background net worker
// (usb_asix_poll_link); this RX-path hook is a no-op so nothing on the
// net_poll/compositor path ever blocks on a USB control transfer.
void usb_asix_refresh_link(usbnet_dev_t *d) {
    (void)d;
}

// On-demand, real-time link query with a short (~200ms) cache so that a caller
// hammering nic_link_up() (e.g. the net-up gate on every DNS/HTTP call) does not
// issue a control transfer every time. Because the RX poll above also updates
// d->link + s_link_tick, this never returns a permanently stale value.
//
// #381 WARNING: this issues a (possibly multi-second, on a cable-less ASIX PHY)
// MII control-transfer read when its cache expires. It MUST NOT be called from
// the compositor / net_poll hot path (net_poll runs under net_lock == cli, so a
// stalled MII read froze the whole UI ~3s per cycle with the dongle plugged in
// but no cable). The hot path now uses the cheap cached d->link via
// usb_eth_link_up(); the actual PHY read happens ONLY on the background net
// worker via usb_asix_poll_link() below. Retained for completeness.
int usb_asix_link_up_cached(usbnet_dev_t *d) {
    extern volatile uint64_t timer_ticks;
    extern uint32_t g_timer_hz;
    uint64_t hz = g_timer_hz ? g_timer_hz : 100;
    uint64_t window = hz / 5;            // ~200ms
    if (!window) window = 1;
    uint64_t now = timer_ticks;
    if (s_link_primed && (now - s_link_tick) < window) return d->link;
    usb_asix_read_link_now(d);
    s_link_tick = now;
    s_link_primed = 1;
    return d->link;
}

// #381: active PHY link poll for the background net worker ONLY. Performs the
// double-read BMSR/PHYSICAL_LINK control transfers off the UI/net_poll path and
// updates d->link. The compositor + net_is_up() gate read the cached d->link.
// #62: consecutive-failure backoff. Even with the short runtime budget above, a
// dongle that has gone silent still costs ~0.5s of blocking per poll, once a
// second, for as long as the machine is up. A timeout is the correct SEMANTICS
// here (the wake source is a device that may never answer, CLAUDE.md rule 2) but
// repeating it every second is not: after AX_LINK_FAIL_QUARANTINE consecutive
// no-answer polls the driver stops asking and probes once per
// AX_LINK_QUARANTINE_POLLS instead, so the device can still prove it recovered
// (the #549 "NET_FAULTY must not be a one-way door" principle) without the
// carrier poll being a standing cost. Total blocking on a permanently dead
// dongle drops from ~30s per second to ~0.5s per 30s.
#define AX_LINK_FAIL_QUARANTINE   3
#define AX_LINK_QUARANTINE_POLLS 30
static uint32_t s_link_fail        = 0;
static uint32_t s_link_skip        = 0;
static int      s_link_quarantined = 0;

int usb_asix_poll_link(usbnet_dev_t *d) {
    extern volatile uint64_t timer_ticks;
    if (s_link_quarantined) {
        if (++s_link_skip < AX_LINK_QUARANTINE_POLLS) return d->link;
        s_link_skip = 0;                       // one probe per quarantine window
    }
    int answered = (usb_asix_read_link_now(d) == 0);
    if (answered) {
        if (s_link_quarantined)
            bootlog_write("[AX88772] PHY answering again after %u quiet polls; "
                          "link poll resumed (#62)", s_link_fail);
        s_link_fail = 0;
        s_link_quarantined = 0;
        s_link_skip = 0;
    } else {
        s_link_fail++;
        if (!s_link_quarantined && s_link_fail >= AX_LINK_FAIL_QUARANTINE) {
            s_link_quarantined = 1;
            s_link_skip = 0;
            bootlog_write("[AX88772] PHY did not answer %u consecutive link "
                          "polls; backing off to 1 probe per %u polls (#62)",
                          s_link_fail, (unsigned)AX_LINK_QUARANTINE_POLLS);
        }
    }
    s_link_tick = timer_ticks;
    s_link_primed = 1;
    return d->link;
}

// =============================================================================
// RX de-framing
// =============================================================================
// #362: AX88772 RX carry-over state.
//
// MEASURED on the owner's AX88772B (0b95:7e2b) passed through to a VM: the chip
// PACKS several Ethernet frames into ONE bulk-IN transfer and, when the transfer
// buffer fills, SPLITS the last frame across the transfer boundary. The boot log
// shows this directly ("rx off=0/234/298/532 ... tlen=2048": the 2048-byte
// buffer came back COMPLETELY FULL, which is exactly the case where the chip has
// more to give and cuts a frame in half). This is why the Linux asix driver
// keeps rx->remaining across URBs.
//
// The de-framer below had NO carry-over, which cost two frames per fill, not
// one: the split tail was dropped, AND the next transfer began mid-frame, so its
// first four bytes parsed as a bogus header, failed the size == ~size check, and
// `break` discarded the WHOLE next transfer. Measured effect: 45 RX frames/s
// against an 85 Hz net pump (about every second transfer thrown away) and 31
// KB/s of TCP throughput, against 7825 KB/s for e1000 on the identical image.
//
// State is file-static because there is exactly one g_usbnet device, matching
// the existing style in this file. usb_asix_rx_reset() clears it, and MUST be
// called whenever the bulk-IN pipe is restarted (start, or resubmit after an
// endpoint error), or bytes from either side of a gap would be glued together
// into one bogus frame.
static uint8_t  s_rx_part[USBNET_FRAME_MAX];
static uint32_t s_rx_have    = 0;   // bytes of the split frame already held
static uint32_t s_rx_need    = 0;   // total size of the split frame (0 = none)
static uint32_t s_rx_pad     = 0;   // pad bytes still to skip after it
static uint8_t  s_rx_hdr[4];
static uint32_t s_rx_hdrhave = 0;   // bytes held of a split 4-byte header

// Diagnostics, read off-path (these run under net_lock with interrupts off, so
// they are COUNTERS and never log lines; see the #745/#69 note in usb_ecm.c).
uint64_t g_asix_rx_split_frames = 0;   // frames reassembled across a boundary
uint64_t g_asix_rx_desync       = 0;   // transfers dropped on a bad header
uint64_t g_asix_rx_max_xfer     = 0;   // largest bulk-IN the chip has handed us

void usb_asix_rx_reset(void) {
    s_rx_have = 0; s_rx_need = 0; s_rx_pad = 0; s_rx_hdrhave = 0;
}

void usb_asix_rx_fixup(usbnet_dev_t *d, uint8_t *buf, uint32_t len) {
    if (len > g_asix_rx_max_xfer) g_asix_rx_max_xfer = len;
    if (d->type == USBNET_TYPE_AX88772) {
        // Stream of [u16 size | u16 ~size | frame...] records, each record
        // padded to a 16-bit boundary (Linux asix_rx_fixup framing). Any of the
        // three parts (header, body, pad byte) may be cut by the end of the
        // transfer and continue at offset 0 of the next one.
        uint32_t off = 0;
        static uint32_t rxdiag = 0;

        // (a) Finish a 4-byte header that was cut by the previous boundary.
        if (s_rx_hdrhave) {
            while (s_rx_hdrhave < 4 && off < len) s_rx_hdr[s_rx_hdrhave++] = buf[off++];
            if (s_rx_hdrhave < 4) return;             // still incomplete
            uint32_t hdr = (uint32_t)s_rx_hdr[0] | ((uint32_t)s_rx_hdr[1] << 8) |
                           ((uint32_t)s_rx_hdr[2] << 16) | ((uint32_t)s_rx_hdr[3] << 24);
            uint32_t size = hdr & 0x7FF;
            uint32_t chk  = (~hdr >> 16) & 0x7FF;
            s_rx_hdrhave = 0;
            if (size != chk || size == 0 || size > USBNET_FRAME_MAX) {
                g_asix_rx_desync++; usb_asix_rx_reset(); return;
            }
            s_rx_need = size; s_rx_have = 0; s_rx_pad = (size & 1u);
        }

        // (b) Finish a frame body (and its pad byte) cut by the previous
        //     boundary. This is the frame the old code silently dropped.
        if (s_rx_need) {
            uint32_t want = s_rx_need - s_rx_have;
            uint32_t avail = len - off;
            uint32_t take = (want < avail) ? want : avail;
            memcpy(s_rx_part + s_rx_have, buf + off, take);
            s_rx_have += take;
            off += take;
            if (s_rx_have < s_rx_need) return;        // still incomplete
            usbnet_fifo_push(s_rx_part, s_rx_need);
            g_asix_rx_split_frames++;
            s_rx_need = 0; s_rx_have = 0;
            while (s_rx_pad && off < len) { off++; s_rx_pad--; }
            if (s_rx_pad) return;                     // pad byte lands next time
        }

        // (c) Normal in-transfer records.
        while (off < len) {
            if (off + 4 > len) {                      // header cut by the end
                s_rx_hdrhave = 0;
                while (off < len) s_rx_hdr[s_rx_hdrhave++] = buf[off++];
                return;
            }
            uint32_t hdr = (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
                           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
            uint32_t size = hdr & 0x7FF;
            uint32_t chk  = (~hdr >> 16) & 0x7FF;
            // #378 diag: log the first few RX headers so one boot tells us
            // whether the frame is well-formed (size==chk) or the transfer is
            // being de-framed wrong (a link-up-but-no-ARP failure mode).
            if (rxdiag < 6) { rxdiag++;
                bootlog_write("[AX88772] rx off=%u hdr=0x%04x%04x size=%u chk=%u ok=%d tlen=%u",
                              off, (unsigned)(hdr >> 16), (unsigned)(hdr & 0xFFFF),
                              size, chk, (size == chk && size != 0), len); }
            if (size != chk || size == 0 || size > USBNET_FRAME_MAX) {
                // Genuinely desynced (not a boundary split): drop the rest and
                // resynchronise on the next transfer.
                g_asix_rx_desync++; usb_asix_rx_reset(); return;
            }
            off += 4;
            if (off + size > len) {                   // body cut by the end
                uint32_t avail = len - off;
                memcpy(s_rx_part, buf + off, avail);
                s_rx_have = avail; s_rx_need = size; s_rx_pad = (size & 1u);
                return;
            }
            usbnet_fifo_push(buf + off, size);
            off += size;
            if (size & 1u) {                          // pad to 16-bit boundary
                if (off < len) off++;
                else { s_rx_pad = 1; return; }
            }
        }
    } else if (d->type == USBNET_TYPE_AX88179) {
        // Aggregated transfer: N packets (each 8-byte aligned), then N u32
        // per-packet descriptors, and finally a u32 header word:
        // low 16 = packet count, high 16 = offset of the descriptor block.
        if (len < 4) return;
        uint32_t rx_hdr = (uint32_t)buf[len - 4] | ((uint32_t)buf[len - 3] << 8) |
                          ((uint32_t)buf[len - 2] << 16) | ((uint32_t)buf[len - 1] << 24);
        uint32_t pkt_cnt = rx_hdr & 0xFFFF;
        uint32_t hdr_off = rx_hdr >> 16;
        if (pkt_cnt == 0 || hdr_off + pkt_cnt * 4 > len - 4) return;
        uint8_t *desc = buf + hdr_off;
        uint32_t off = 0;
        for (uint32_t i = 0; i < pkt_cnt; i++) {
            uint32_t h = (uint32_t)desc[i * 4] | ((uint32_t)desc[i * 4 + 1] << 8) |
                         ((uint32_t)desc[i * 4 + 2] << 16) | ((uint32_t)desc[i * 4 + 3] << 24);
            uint32_t pkt_len = (h >> 16) & 0x1FFF;
            if (pkt_len < 2 || off + pkt_len > hdr_off) break;
            // 2-byte IP-alignment pad at the front of each packet (IPE set).
            usbnet_fifo_push(buf + off + 2, pkt_len - 2);
            off += (pkt_len + 7) & ~7u;
        }
    }
}

// =============================================================================
// Attach
// =============================================================================
int usb_asix_attach(xhci_controller_t *xhc, int slot_id, int speed,
                    uint16_t vid, uint16_t pid, int is179,
                    uint8_t *cfg, int total) {
    // ASIX parts are single-config, single-interface (vendor class 0xFF):
    // grab the first bulk IN/OUT pair in the configuration.
    int ep_in = -1, ep_out = -1, in_mps = 0, out_mps = 0;
    int cfg_value = (total >= 6) ? cfg[5] : 1;
    int i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x05 && blen >= 7) {
            int eaddr = cfg[i + 2];
            int eattr = cfg[i + 3] & 0x03;
            int emps  = cfg[i + 4] | (cfg[i + 5] << 8);
            if (eattr == 0x02) {
                if ((eaddr & 0x80) && ep_in < 0)  { ep_in = eaddr; in_mps = emps; }
                if (!(eaddr & 0x80) && ep_out < 0) { ep_out = eaddr; out_mps = emps; }
            }
        }
        i += blen;
    }
    if (ep_in < 0 || ep_out < 0) {
        kprintf("[USB-ASIX] no bulk endpoint pair found\n");
        return -1;
    }

    usbnet_dev_t *d = &g_usbnet;
    memset(d, 0, sizeof(*d));
    d->xhc = xhc;
    d->slot_id = slot_id;
    d->speed = speed;
    d->type = is179 ? USBNET_TYPE_AX88179 : USBNET_TYPE_AX88772;

    if (xhci_set_configuration(xhc, slot_id, (uint8_t)cfg_value) < 0) {
        kprintf("[USB-ASIX] SET_CONFIGURATION failed\n");
        return -1;
    }
    if (usbnet_config_bulk_eps(d, ep_in, in_mps, ep_out, out_mps) != 0) {
        kprintf("[USB-ASIX] endpoint configuration failed\n");
        return -1;
    }
    // AX88179 aggregates many frames per bulk-IN; give it a large buffer.
    // (Kept to 12KB: matches the programmed 0x12 boundary conservatively.)
    // #362: 16 KiB for the AX88772 (was 2048). The chip aggregates
    // frames into one bulk-IN transfer, and exactly ONE full-size
    // frame (1514 + 4 header) fits in 2048, so the old buffer filled
    // and split a frame on essentially every transfer under load. A
    // multiple of the 512-byte bulk max-packet size, and well inside
    // the TRB transfer-length field (17 bits).
    if (usbnet_alloc_buffers(d, is179 ? 12288 : 16384, 2048) != 0) return -1;

    int r = is179 ? ax88179_init(d) : ax88772_init(d);
    if (r != 0) return -1;

    // #62: bring-up is over. Every control transfer from here on is a periodic
    // poll, so drop to the runtime budget (see AX_RUNTIME_CTRL_MS above).
    s_ax_ctrl_ms = AX_RUNTIME_CTRL_MS;

    d->active = 1;
    kprintf("[USB-ASIX] %s attached (%04x:%04x): MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            is179 ? "AX88179" : "AX88772", vid, pid,
            d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    bootlog_write("[USB-ASIX] %s NIC attached (slot %d, %04x:%04x), MAC %02x:%02x:%02x:%02x:%02x:%02x link %s",
                  is179 ? "AX88179" : "AX88772", slot_id, vid, pid,
                  d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
                  d->link ? "up" : "down");
    return 0;
}
