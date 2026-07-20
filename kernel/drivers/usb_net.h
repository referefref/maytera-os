// usb_net.h - USB network device support (#362).
//
// Two driver families feed one shared "USB NIC" core:
//   - CDC-ECM class driver (USB class 0x02 / subclass 0x06), usb_ecm.c.
//     This is what QEMU's usb-net (CDC configuration) and many generic
//     dongles speak: raw Ethernet frames over a bulk IN/OUT pair.
//   - ASIX AX88772 (USB2) / AX88179 (USB3) vendor drivers, usb_asix.c.
//     These are the chips inside Apple's own USB Ethernet adapter and most
//     common third-party dongles. Vendor control requests bring up the
//     PHY/MAC; frames carry small vendor headers on the bulk pipes.
//
// The core exposes the same backend API shape as e1000/virtio-net so
// net/net.c can select it as the active NIC when no PCI NIC is present
// (VMs keep using e1000/virtio unchanged; the iMac uses the dongle).
//
// Event-ring discipline: ALL completions are consumed through the existing
// xhci per-(slot,DCI) completion tables via xhci_int_in_submit/poll and
// xhci_control_transfer. No new event-ring consumer is introduced (see
// blame.md #307/#348 for the event-stealing history).
#ifndef USB_NET_H
#define USB_NET_H

#include "../types.h"
#include "xhci.h"

// Device flavor
#define USBNET_TYPE_NONE    0
#define USBNET_TYPE_ECM     1   // CDC-ECM class device
#define USBNET_TYPE_AX88772 2   // ASIX AX88772/A/B (USB2)
#define USBNET_TYPE_AX88179 3   // ASIX AX88179 / AX88178A (USB3)

// Singleton USB NIC state (one USB network device supported, which matches
// the real deployment: one dongle on the iMac).
typedef struct {
    xhci_controller_t *xhc;
    int slot_id;
    int speed;              // XHCI_SPEED_*
    int type;               // USBNET_TYPE_*
    int active;             // attach fully succeeded, device is usable
    int started;            // RX armed (first bulk-IN submitted)
    int link;               // last known PHY/link state (ECM: 1 once active)
    int in_dci, out_dci;    // bulk endpoint DCIs
    int in_mps, out_mps;    // bulk endpoint max packet sizes
    uint8_t mac[6];
    // RX: one permanently outstanding bulk-IN into this DMA buffer.
    uint8_t *rx_buf;
    uint32_t rx_buf_len;    // bytes submitted per bulk-IN
    // TX bounce buffer (frame + any vendor header)
    uint8_t *tx_buf;
    uint32_t tx_buf_len;
} usbnet_dev_t;

extern usbnet_dev_t g_usbnet;

// Called from xhci_enumerate_devices() once the device + config descriptors
// have been read. Claims the device if it is a supported USB NIC (ASIX by
// VID/PID, otherwise CDC-ECM by interface class, searching the device's
// OTHER configurations too - QEMU's usb-net puts RNDIS in config index 0 and
// the CDC-ECM function in another configuration). Returns 1 if claimed.
int usb_net_probe(xhci_controller_t *xhc, int slot_id, int speed,
                  uint16_t vid, uint16_t pid,
                  uint8_t *cfg, int cfg_total, uint8_t num_configs);

// Backend API for net/net.c (mirrors e1000_* / virtio_net_*).
int  usb_eth_present(void);
int  usb_eth_start(void);                       // arm RX; 0 on success
void usb_eth_get_mac(uint8_t *mac);
int  usb_eth_send(const void *data, uint16_t length);
int  usb_eth_receive(void *buffer, uint16_t buffer_size);
int  usb_eth_link_up(void);                     // #381: cheap, cached (no ctrl xfer)
int  usb_eth_poll_link(void);                   // #381: active PHY read (worker only)
const char *usb_eth_name(void);                 // "CDC-ECM" / "AX88772" / "AX88179"

// ---- internal, shared between usb_ecm.c (core) and usb_asix.c ----
int usbnet_alloc_buffers(usbnet_dev_t *d, uint32_t rx_len, uint32_t tx_len);
int usbnet_config_bulk_eps(usbnet_dev_t *d, int ep_in, int in_mps,
                           int ep_out, int out_mps);
// Blocking-with-short-timeout bulk OUT through the shared completion tables.
int usbnet_bulk_out(usbnet_dev_t *d, const void *data, uint32_t len,
                    uint32_t timeout_ms, int send_zlp);
void usbnet_fifo_push(const uint8_t *frame, uint32_t len);

int usb_ecm_attach_cfg(xhci_controller_t *xhc, int slot_id, int speed,
                       uint8_t *cfg, int cfg_total);
int usb_asix_attach(xhci_controller_t *xhc, int slot_id, int speed,
                    uint16_t vid, uint16_t pid, int is179,
                    uint8_t *cfg, int cfg_total);
// RX-framing parsers (called by the core when a bulk-IN completes)
void usb_asix_rx_fixup(usbnet_dev_t *d, uint8_t *buf, uint32_t len);
// Lazy PHY link refresh for ASIX parts (no-op for ECM)
void usb_asix_refresh_link(usbnet_dev_t *d);
// Active, real-time (double-read, short-cached) PHY link query for ASIX parts.
int usb_asix_link_up_cached(usbnet_dev_t *d);
// #381: active PHY link poll for the background net worker (no cache gate).
int usb_asix_poll_link(usbnet_dev_t *d);

#endif // USB_NET_H
