// usb_cdc_acm.h - USB CDC-ACM (communications / abstract control model) serial
// class driver (#396). Enumerates a CDC-ACM device (comm class 0x02 subclass
// 0x02 + data class 0x0A), does the standard ACM line-coding + control-line
// setup, claims the bulk-IN/bulk-OUT (and the interrupt notification) endpoints,
// and exposes a bounded read/write serial channel plus a /dev/ttyACM0 node.
//
// Built for the M3D Micro 3D printer (03eb:2404, an Atmel CDC-ACM device). The
// device powers up in its bootloader, which speaks a single-character command
// set (see usb_cdc_acm.c). All reads are BOUNDED (small per-call timeout) so a
// silent device can never block on a full xHCI timeout.
#ifndef DRIVERS_USB_CDC_ACM_H
#define DRIVERS_USB_CDC_ACM_H

#include "../types.h"
#include "xhci.h"

// Probe entry, called from xhci_probe_device() after usb_net_probe(). Returns 1
// if this device was claimed as a CDC-ACM serial port, 0 otherwise.
int usb_cdc_acm_probe(xhci_controller_t *xhc, int slot_id, int speed,
                      uint16_t vid, uint16_t pid, uint8_t *cfg, int total);

// Is a CDC-ACM serial port attached and ready?
int usb_cdc_acm_present(void);

// Write len bytes to the serial bulk-OUT endpoint. Bounded by timeout_ms.
// Returns the number of bytes sent (== len) on success, <0 on error/timeout.
int usb_cdc_acm_write(const void *data, uint32_t len, uint32_t timeout_ms);

// Read up to maxlen bytes from the serial bulk-IN endpoint. BOUNDED by
// timeout_ms. Returns the number of bytes read (0 on timeout with nothing
// available), <0 on endpoint error. One completed bulk-IN transfer per call.
int usb_cdc_acm_read(void *buf, uint32_t maxlen, uint32_t timeout_ms);

// Register the /dev/ttyACM0 node. Called from dev_init() at boot.
void usb_cdc_acm_dev_init(void);

// Serial-console test command ("m3d ..."), wired into main.c's shell. Safe,
// read-only subcommands plus the authorized HOME and a dry-run UNLOAD.
void usb_cdc_acm_shell(const char *args);

#endif // DRIVERS_USB_CDC_ACM_H
