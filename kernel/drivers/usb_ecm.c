// usb_ecm.c - CDC-ECM USB Ethernet class driver + shared USB-NIC core (#362).
//
// Core model (mirrors the USB HID non-blocking pattern from #307):
//   RX: exactly one bulk-IN TD is kept outstanding. usb_eth_receive() polls
//       its completion via the shared per-(slot,DCI) table (xhci_int_in_poll),
//       parses the completed buffer into a small frame FIFO (one ECM transfer
//       is one Ethernet frame; ASIX transfers can carry several framed
//       packets), then resubmits. The net stack's nic_receive() pops frames
//       from the FIFO, exactly like e1000_receive() pops its RX ring.
//   TX: copy the frame into a DMA bounce buffer, submit one TD on the bulk
//       OUT ring and wait (bounded, ~40ms) on the same completion table.
//       On timeout the frame is dropped - Ethernet is lossy, TCP retransmits.
//
// All waits use xhci_delay() (PIT-calibrated, works with interrupts off), so
// TX is safe both before the scheduler starts (DHCP during boot) and inside
// the net_lock()/cli critical section later on.
#include "usb_net.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/pmm.h"
#include "../fs/bootlog.h"

usbnet_dev_t g_usbnet;

// =============================================================================
// Frame FIFO (RX side, between the bulk-IN parser and nic_receive())
// =============================================================================

#define USBNET_FIFO_SLOTS  16
#define USBNET_FRAME_MAX   1520

static uint8_t  fifo_frame[USBNET_FIFO_SLOTS][USBNET_FRAME_MAX];
static uint16_t fifo_len[USBNET_FIFO_SLOTS];
static int fifo_head = 0;   // next slot to pop
static int fifo_tail = 0;   // next slot to fill
static int fifo_count = 0;

void usbnet_fifo_push(const uint8_t *frame, uint32_t len) {
    if (len < 14 || len > USBNET_FRAME_MAX) return;   // runt / oversize
    if (fifo_count >= USBNET_FIFO_SLOTS) return;      // overflow: drop
    memcpy(fifo_frame[fifo_tail], frame, len);
    fifo_len[fifo_tail] = (uint16_t)len;
    fifo_tail = (fifo_tail + 1) % USBNET_FIFO_SLOTS;
    fifo_count++;
}

static int usbnet_fifo_pop(uint8_t *out, uint32_t out_size) {
    if (fifo_count == 0) return 0;
    uint32_t len = fifo_len[fifo_head];
    if (len > out_size) len = out_size;
    memcpy(out, fifo_frame[fifo_head], len);
    fifo_head = (fifo_head + 1) % USBNET_FIFO_SLOTS;
    fifo_count--;
    return (int)len;
}

// =============================================================================
// Shared helpers
// =============================================================================

int usbnet_alloc_buffers(usbnet_dev_t *d, uint32_t rx_len, uint32_t tx_len) {
    // Page allocations are identity-mapped (phys == virt). One page each is
    // enough for ECM/AX88772 (<= 2KB per transfer); AX88179 asks for more.
    uint32_t rx_pages = (rx_len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t tx_pages = (tx_len + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t rx_phys = pmm_alloc_pages(rx_pages);
    uint64_t tx_phys = pmm_alloc_pages(tx_pages);
    if (!rx_phys || !tx_phys) {
        kprintf("[USB-NET] DMA buffer allocation failed\n");
        return -1;
    }
    d->rx_buf = (uint8_t *)rx_phys;
    d->rx_buf_len = rx_len;
    d->tx_buf = (uint8_t *)tx_phys;
    d->tx_buf_len = tx_len;
    return 0;
}

int usbnet_config_bulk_eps(usbnet_dev_t *d, int ep_in, int in_mps,
                           int ep_out, int out_mps) {
    int in_dci = xhci_configure_endpoint_ep(d->xhc, d->slot_id, ep_in,
                                            EP_TYPE_BULK_IN, in_mps, 0, d->speed);
    if (in_dci < 0) return -1;
    int out_dci = xhci_configure_endpoint_ep(d->xhc, d->slot_id, ep_out,
                                             EP_TYPE_BULK_OUT, out_mps, 0, d->speed);
    if (out_dci < 0) return -1;
    d->in_dci = in_dci;
    d->out_dci = out_dci;
    d->in_mps = in_mps;
    d->out_mps = out_mps;
    return 0;
}

// Diagnostics: log the outcome of the first few TX/RX transfers to the boot
// log (real-hardware debugging aid; silent afterwards).
static uint32_t g_tx_diag = 0;
static uint32_t g_rx_diag = 0;

int usbnet_bulk_out(usbnet_dev_t *d, const void *data, uint32_t len,
                    uint32_t timeout_ms, int send_zlp) {
    if (!d->active) return -1;
    if (len > d->tx_buf_len) return -1;

    // Consume any stale completion left on the OUT DCI (e.g. a previous
    // timed-out TX that completed late) so it can't satisfy this wait.
    uint32_t scrap = 0;
    (void)xhci_int_in_poll(d->xhc, d->slot_id, d->out_dci, &scrap, 0);

    if (data != d->tx_buf) memcpy(d->tx_buf, data, len);
    if (xhci_int_in_submit(d->xhc, d->slot_id, d->out_dci,
                           (uint64_t)d->tx_buf, len) != 0) {
        return -1;
    }
    // Wait using the PIT-calibrated wall-clock delay (same discipline as
    // xhci_wait_transfer): 1ms per iteration, timeout_ms iterations.
    for (uint32_t i = 0; i < timeout_ms; i++) {
        uint32_t done = 0;
        int r = xhci_int_in_poll(d->xhc, d->slot_id, d->out_dci, &done, len);
        if (r > 0) {
            if (g_tx_diag < 3) {
                g_tx_diag++;
                bootlog_write("[USB-NET] TX #%u OK (%u bytes, ~%ums)",
                              g_tx_diag, len, i);
            }
            if (send_zlp) {
                // Terminating zero-length packet (ECM frames that are an
                // exact multiple of the endpoint max packet size).
                if (xhci_int_in_submit(d->xhc, d->slot_id, d->out_dci,
                                       (uint64_t)d->tx_buf, 0) != 0) return -1;
                for (uint32_t j = 0; j < timeout_ms; j++) {
                    int rz = xhci_int_in_poll(d->xhc, d->slot_id, d->out_dci,
                                              &done, 0);
                    if (rz > 0) return 0;
                    if (rz < 0) return -1;
                    xhci_delay_ms(1);
                }
                return -1;
            }
            return 0;
        }
        if (r < 0) {
            if (g_tx_diag < 6) {
                g_tx_diag++;
                bootlog_write("[USB-NET] TX ERROR on DCI %d (%u bytes)",
                              d->out_dci, len);
            }
            return -1;   // STALL/error
        }
        xhci_delay_ms(1);
    }
    if (g_tx_diag < 6) {
        g_tx_diag++;
        bootlog_write("[USB-NET] TX TIMEOUT on DCI %d (%u bytes, %ums budget)",
                      d->out_dci, len, timeout_ms);
    }
    return -1;   // timeout: drop the frame
}

static int usbnet_rx_submit(usbnet_dev_t *d) {
    return xhci_int_in_submit(d->xhc, d->slot_id, d->in_dci,
                              (uint64_t)d->rx_buf, d->rx_buf_len);
}

// =============================================================================
// Backend API for net/net.c
// =============================================================================

int usb_eth_present(void) {
    return g_usbnet.active;
}

const char *usb_eth_name(void) {
    switch (g_usbnet.type) {
        case USBNET_TYPE_ECM:     return "CDC-ECM";
        case USBNET_TYPE_AX88772: return "AX88772";
        case USBNET_TYPE_AX88179: return "AX88179";
        default:                  return "none";
    }
}

int usb_eth_start(void) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active) return -1;
    if (d->started) return 0;
    // Quiet the per-transfer-event serial spam: from here on RX/TX completions
    // arrive continuously (the HID poll worker sets this too, but the NIC may
    // start first when DHCP runs during boot).
    xhci_iso_quiet = 1;
    if (usbnet_rx_submit(d) != 0) {
        kprintf("[USB-NET] initial bulk-IN submit failed\n");
        return -1;
    }
    d->started = 1;
    kprintf("[USB-NET] %s started: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            usb_eth_name(), d->mac[0], d->mac[1], d->mac[2],
            d->mac[3], d->mac[4], d->mac[5]);
    bootlog_write("[USB-NET] %s active as NIC, MAC %02x:%02x:%02x:%02x:%02x:%02x",
                  usb_eth_name(), d->mac[0], d->mac[1], d->mac[2],
                  d->mac[3], d->mac[4], d->mac[5]);
    return 0;
}

void usb_eth_get_mac(uint8_t *mac) {
    if (mac) memcpy(mac, g_usbnet.mac, 6);
}

// #381: CHEAP, NON-BLOCKING carrier read. Returns the CACHED link state only,
// never issuing a USB control transfer on the caller's thread. nic_link_up() /
// net_is_up() are called from the compositor + net_poll hot path (the latter
// under net_lock == interrupts off); a cable-less ASIX PHY read there stalled
// the whole UI ~3s per cycle. The actual (possibly slow) PHY read now happens
// exclusively on the background net worker via usb_eth_poll_link().
int usb_eth_link_up(void) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active) return 0;
    if (d->type == USBNET_TYPE_ECM) return 1;   // no PHY to poll; bus-powered link
    return d->link;                             // cached; refreshed by net worker
}

// #381: active PHY link poll, called ONLY by the background net worker
// (net/net.c). Does the real double-read PHY query OFF the UI/net_poll path and
// updates the cached d->link that usb_eth_link_up() returns.
int usb_eth_poll_link(void) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active) return 0;
    if (d->type == USBNET_TYPE_ECM) { d->link = 1; return 1; }
    return usb_asix_poll_link(d);
}

int usb_eth_send(const void *data, uint16_t length) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active || !data || length == 0) return -1;
    if (!d->started && usb_eth_start() != 0) return -1;

    switch (d->type) {
        case USBNET_TYPE_ECM: {
            int zlp = ((length % d->out_mps) == 0);
            return usbnet_bulk_out(d, data, length, 40, zlp);
        }
        case USBNET_TYPE_AX88772: {
            // 4-byte header: u16 length (LE), u16 ~length. If the padded
            // transfer would be an exact multiple of the max packet size,
            // append the magic pad marker instead of a ZLP (per the chip's
            // framing; matches the Linux asix driver's tx_fixup).
            uint32_t len = length;
            if (len + 4 > d->tx_buf_len) return -1;
            d->tx_buf[0] = (uint8_t)(len & 0xFF);
            d->tx_buf[1] = (uint8_t)((len >> 8) & 0xFF);
            d->tx_buf[2] = (uint8_t)(~len & 0xFF);
            d->tx_buf[3] = (uint8_t)((~len >> 8) & 0xFF);
            memcpy(d->tx_buf + 4, data, len);
            uint32_t total = len + 4;
            if ((total % d->out_mps) == 0 && total + 4 <= d->tx_buf_len) {
                d->tx_buf[total + 0] = 0x00;
                d->tx_buf[total + 1] = 0x00;
                d->tx_buf[total + 2] = 0xFF;
                d->tx_buf[total + 3] = 0xFF;
                total += 4;
            }
            return usbnet_bulk_out(d, d->tx_buf, total, 40, 0);
        }
        case USBNET_TYPE_AX88179: {
            // 8-byte header: u32 packet length (LE), u32 flags (0).
            uint32_t len = length;
            if (len + 8 > d->tx_buf_len) return -1;
            d->tx_buf[0] = (uint8_t)(len & 0xFF);
            d->tx_buf[1] = (uint8_t)((len >> 8) & 0xFF);
            d->tx_buf[2] = (uint8_t)((len >> 16) & 0xFF);
            d->tx_buf[3] = 0;
            memset(d->tx_buf + 4, 0, 4);
            memcpy(d->tx_buf + 8, data, len);
            uint32_t total = len + 8;
            int zlp = ((total % d->out_mps) == 0);
            return usbnet_bulk_out(d, d->tx_buf, total, 40, zlp);
        }
    }
    return -1;
}

int usb_eth_receive(void *buffer, uint16_t buffer_size) {
    usbnet_dev_t *d = &g_usbnet;
    if (!d->active || !buffer) return 0;
    if (!d->started && usb_eth_start() != 0) return 0;

    // Reap a completed bulk-IN (if any), parse it, resubmit.
    uint32_t got = 0;
    int r = xhci_int_in_poll(d->xhc, d->slot_id, d->in_dci, &got, d->rx_buf_len);
    if (r > 0) {
        if (g_rx_diag < 3 && got > 0) {
            g_rx_diag++;
            bootlog_write("[USB-NET] RX #%u completed (%u bytes)", g_rx_diag, got);
        }
        if (got > 0) {
            switch (d->type) {
                case USBNET_TYPE_ECM:
                    // One transfer == one Ethernet frame (short packet /
                    // ZLP terminated).
                    usbnet_fifo_push(d->rx_buf, got);
                    break;
                case USBNET_TYPE_AX88772:
                case USBNET_TYPE_AX88179:
                    usb_asix_rx_fixup(d, d->rx_buf, got);
                    break;
            }
        }
        usbnet_rx_submit(d);
    } else if (r < 0) {
        // Endpoint error/STALL: try to keep the pipe alive by resubmitting.
        usbnet_rx_submit(d);
    }

    // Lazy PHY link refresh for ASIX parts (cheap counter-gated inside).
    if (d->type != USBNET_TYPE_ECM) usb_asix_refresh_link(d);

    return usbnet_fifo_pop((uint8_t *)buffer, buffer_size);
}

// =============================================================================
// CDC-ECM attach
// =============================================================================

// GET_DESCRIPTOR(STRING) -> parse 12 UTF-16LE hex digits into a MAC address.
static int ecm_read_mac_string(xhci_controller_t *xhc, int slot_id,
                               int str_idx, uint8_t *mac) {
    static uint8_t sbuf[64] __attribute__((aligned(64)));
    if (str_idx <= 0) return -1;
    memset(sbuf, 0, sizeof(sbuf));
    int cc = xhci_control_transfer(xhc, slot_id, 0x80, 0x06,
                                   (uint16_t)((0x03 << 8) | str_idx), 0x0409,
                                   sbuf, sizeof(sbuf));
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) return -1;
    int blen = sbuf[0];
    if (blen > (int)sizeof(sbuf)) blen = sizeof(sbuf);
    int mi = 0, nhex = 0;
    uint8_t val = 0;
    for (int i = 2; i + 1 < blen && mi < 6; i += 2) {
        char c = (char)sbuf[i];
        int h;
        if (c >= '0' && c <= '9') h = c - '0';
        else if (c >= 'a' && c <= 'f') h = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') h = c - 'A' + 10;
        else if (c == ':' || c == '-') continue;   // tolerate separators
        else return -1;
        val = (uint8_t)((val << 4) | h);
        if (++nhex == 2) { mac[mi++] = val; val = 0; nhex = 0; }
    }
    return (mi == 6) ? 0 : -1;
}

// Does this configuration descriptor contain a CDC-ECM control interface?
static int cfg_find_ecm_iface(uint8_t *cfg, int total) {
    int i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9 &&
            cfg[i + 5] == 0x02 && cfg[i + 6] == 0x06) {
            return cfg[i + 2];   // bInterfaceNumber
        }
        i += blen;
    }
    return -1;
}

int usb_ecm_attach_cfg(xhci_controller_t *xhc, int slot_id, int speed,
                       uint8_t *cfg, int total) {
    int ctrl_if = cfg_find_ecm_iface(cfg, total);
    if (ctrl_if < 0) return -1;
    int cfg_value = (total >= 6) ? cfg[5] : 1;

    // Walk the CDC functional descriptors (type 0x24): union (subtype 0x06)
    // names the data interface; the Ethernet Networking functional descriptor
    // (subtype 0x0F) carries the MAC address string index.
    int data_if = ctrl_if + 1;   // spec default if no union descriptor
    int imac = -1;
    int i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x24 && blen >= 4) {
            int subtype = cfg[i + 2];
            if (subtype == 0x06 && blen >= 5) data_if = cfg[i + 4];  // union: bSubordinateInterface0
            if (subtype == 0x0F && blen >= 13) imac = cfg[i + 3];    // ether: iMACAddress
        }
        i += blen;
    }

    // Find the data interface alternate setting that carries the bulk pair
    // (alt 0 has zero endpoints per the ECM spec; QEMU follows this).
    int alt = -1, ep_in = -1, ep_out = -1, in_mps = 0, out_mps = 0;
    i = 0;
    int cur_if = -1, cur_alt = -1;
    int t_in = -1, t_out = -1, t_in_mps = 0, t_out_mps = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9) {
            // close out the previous alt setting
            if (cur_if == data_if && t_in >= 0 && t_out >= 0 && alt < 0) {
                alt = cur_alt; ep_in = t_in; ep_out = t_out;
                in_mps = t_in_mps; out_mps = t_out_mps;
            }
            cur_if = cfg[i + 2];
            cur_alt = cfg[i + 3];
            t_in = t_out = -1; t_in_mps = t_out_mps = 0;
        } else if (btype == 0x05 && blen >= 7 && cur_if == data_if) {
            int eaddr = cfg[i + 2];
            int eattr = cfg[i + 3] & 0x03;
            int emps  = cfg[i + 4] | (cfg[i + 5] << 8);
            if (eattr == 0x02) {   // bulk
                if (eaddr & 0x80) { t_in = eaddr; t_in_mps = emps; }
                else              { t_out = eaddr; t_out_mps = emps; }
            }
        }
        i += blen;
    }
    if (cur_if == data_if && t_in >= 0 && t_out >= 0 && alt < 0) {
        alt = cur_alt; ep_in = t_in; ep_out = t_out;
        in_mps = t_in_mps; out_mps = t_out_mps;
    }
    if (ep_in < 0 || ep_out < 0) {
        kprintf("[USB-ECM] no bulk endpoint pair on data interface %d\n", data_if);
        return -1;
    }

    kprintf("[USB-ECM] ctrl if %d data if %d alt %d bulk-in 0x%02x/%d bulk-out 0x%02x/%d cfg %d\n",
            ctrl_if, data_if, alt, ep_in, in_mps, ep_out, out_mps, cfg_value);

    usbnet_dev_t *d = &g_usbnet;
    memset(d, 0, sizeof(*d));
    d->xhc = xhc;
    d->slot_id = slot_id;
    d->speed = speed;
    d->type = USBNET_TYPE_ECM;

    // MAC address from the Ethernet functional descriptor's string.
    if (ecm_read_mac_string(xhc, slot_id, imac, d->mac) != 0) {
        // Fall back to a locally administered address rather than failing:
        // a NIC with a random MAC still gets a DHCP lease.
        kprintf("[USB-ECM] iMACAddress string unreadable, using fallback MAC\n");
        d->mac[0] = 0x02; d->mac[1] = 0x36; d->mac[2] = 0x32;
        d->mac[3] = 0x4D; d->mac[4] = 0x4F; d->mac[5] = 0x53;
    }

    if (xhci_set_configuration(xhc, slot_id, (uint8_t)cfg_value) < 0) {
        kprintf("[USB-ECM] SET_CONFIGURATION %d failed\n", cfg_value);
        return -1;
    }
    if (alt > 0) {
        // SET_INTERFACE(data_if, alt) selects the endpoint-bearing setting.
        int cc = xhci_control_transfer(xhc, slot_id, 0x01, 0x0B,
                                       (uint16_t)alt, (uint16_t)data_if, NULL, 0);
        if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) {
            kprintf("[USB-ECM] SET_INTERFACE(%d, alt %d) failed cc=%d (continuing)\n",
                    data_if, alt, cc);
        }
    }
    if (usbnet_config_bulk_eps(d, ep_in, in_mps, ep_out, out_mps) != 0) {
        kprintf("[USB-ECM] endpoint configuration failed\n");
        return -1;
    }
    if (usbnet_alloc_buffers(d, 2048, 2048) != 0) return -1;

    // SET_ETHERNET_PACKET_FILTER: directed + broadcast + all-multicast.
    // Some real ECM devices keep RX gated until this arrives; QEMU accepts it.
    (void)xhci_control_transfer(xhc, slot_id, 0x21, 0x43, 0x000F,
                                (uint16_t)ctrl_if, NULL, 0);

    d->link = 1;
    d->active = 1;
    kprintf("[USB-ECM] attached: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    bootlog_write("[USB-ECM] CDC-ECM NIC attached (slot %d), MAC %02x:%02x:%02x:%02x:%02x:%02x",
                  slot_id, d->mac[0], d->mac[1], d->mac[2],
                  d->mac[3], d->mac[4], d->mac[5]);
    return 0;
}

// =============================================================================
// Probe entry (called from xhci_enumerate_devices)
// =============================================================================

// ASIX VID/PID match table. is179 selects the AX88179 (USB3) register model.
static const struct { uint16_t vid, pid; int is179; } asix_ids[] = {
    { 0x0B95, 0x7720, 0 },   // AX88772
    { 0x0B95, 0x772A, 0 },   // AX88772A
    { 0x0B95, 0x772B, 0 },   // AX88772B
    { 0x0B95, 0x7E2B, 0 },   // AX88772B
    { 0x05AC, 0x1402, 0 },   // Apple USB Ethernet Adapter (AX88772/A)
    { 0x2001, 0x3C05, 0 },   // D-Link DUB-E100 rev B
    { 0x0DF6, 0x061C, 0 },   // Sitecom LN-028
    { 0x17EF, 0x7203, 0 },   // Lenovo U2L100P
    { 0x0B95, 0x1790, 1 },   // AX88179
    { 0x0B95, 0x178A, 1 },   // AX88178A
    { 0x04B4, 0x3610, 1 },   // Cypress GX3 (AX88179 core)
    { 0x2001, 0x4A00, 1 },   // D-Link DUB-1312
};

int usb_net_probe(xhci_controller_t *xhc, int slot_id, int speed,
                  uint16_t vid, uint16_t pid,
                  uint8_t *cfg, int cfg_total, uint8_t num_configs) {
    if (g_usbnet.active) return 0;   // one USB NIC supported

    // 1) ASIX vendor match (vendor-specific class; must key on VID/PID).
    for (unsigned k = 0; k < sizeof(asix_ids) / sizeof(asix_ids[0]); k++) {
        if (asix_ids[k].vid == vid && asix_ids[k].pid == pid) {
            kprintf("[USB-NET] ASIX %s dongle %04x:%04x detected\n",
                    asix_ids[k].is179 ? "AX88179" : "AX88772", vid, pid);
            return usb_asix_attach(xhc, slot_id, speed, vid, pid,
                                   asix_ids[k].is179, cfg, cfg_total) == 0;
        }
    }

    // 2) CDC-ECM in the already-fetched configuration (index 0).
    if (cfg_find_ecm_iface(cfg, cfg_total) >= 0) {
        return usb_ecm_attach_cfg(xhc, slot_id, speed, cfg, cfg_total) == 0;
    }

    // 3) Search the device's OTHER configurations for an ECM function.
    // QEMU's usb-net exposes RNDIS as config descriptor index 0 and the
    // CDC-ECM function in the other configuration; real combo devices do
    // similar. Only bother for Communications-class-plausible devices
    // (multi-config), and cap the search.
    if (num_configs > 1) {
        static uint8_t cfg2[512] __attribute__((aligned(64)));
        for (int ci = 1; ci < (int)num_configs && ci < 4; ci++) {
            memset(cfg2, 0, sizeof(cfg2));
            int cc = xhci_control_transfer(xhc, slot_id, 0x80, 0x06,
                                           (uint16_t)((0x02 << 8) | ci), 0,
                                           cfg2, 9);
            if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) continue;
            int total = cfg2[2] | (cfg2[3] << 8);
            if (total > (int)sizeof(cfg2)) total = sizeof(cfg2);
            if (total < 9) continue;
            memset(cfg2, 0, sizeof(cfg2));
            cc = xhci_control_transfer(xhc, slot_id, 0x80, 0x06,
                                       (uint16_t)((0x02 << 8) | ci), 0,
                                       cfg2, (uint16_t)total);
            if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) continue;
            if (cfg_find_ecm_iface(cfg2, total) >= 0) {
                kprintf("[USB-NET] CDC-ECM function found in config index %d\n", ci);
                return usb_ecm_attach_cfg(xhc, slot_id, speed, cfg2, total) == 0;
            }
        }
    }
    return 0;
}
