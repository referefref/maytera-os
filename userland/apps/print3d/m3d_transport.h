// m3d_transport.h - MayteraOS "3D Print" (#396) transport abstraction.
//
// A processed g-code command is framed into an M3D binary packet
// (m3d_gcode_get_binary) and handed to a backend. PHASE 1 provides a VIRTUAL
// PRINTER backend that logs every command (ASCII + hex packet) to a file so the
// produced M3D stream can be inspected and diffed against M33-Fio. PHASE 2 will
// add an M3D_BACKEND_USB backend (CDC-ACM over the real USB serial device) with
// NO change to the app: it uses only the functions below.
#ifndef M3D_TRANSPORT_H
#define M3D_TRANSPORT_H

#include "m3d_gcode.h"

typedef enum {
    M3D_BACKEND_VIRTUAL = 0,   // phase 1: log the produced stream to a file
    M3D_BACKEND_USB     = 1,   // M3D Micro: CDC-ACM USB serial, Micro binary framing
    M3D_BACKEND_USB_PRO = 2    // M3D Pro (#406): CDC-ACM USB serial, Repetier Binary V2
} m3d_backend_t;

// Connection mode reported by the M115 handshake (m3d_transport_connect).
typedef enum {
    M3D_MODE_UNKNOWN    = 0,
    M3D_MODE_BOOTLOADER = 1,   // reply matched "B<digits>" (e.g. Pro "B008")
    M3D_MODE_FIRMWARE   = 2    // reply contained "ok" (firmware running)
} m3d_mode_t;

typedef struct {
    m3d_backend_t backend;
    int           fd;            // virtual: log file; usb: device fd
    int           open;
    unsigned long sent_commands;
    unsigned long sent_bytes;    // total binary bytes framed
    char          target[96];    // log path (virtual) or device name (usb)
    // --- populated by m3d_transport_connect() (USB backends) ---
    m3d_mode_t    mode;          // bootloader vs firmware
    int           boot_version;  // bootloader interface version (if bootloader)
    int           is_pro;        // banner looked like an M3D Pro
} m3d_transport_t;

// Open the transport. `target` is the log-file path for the virtual backend, or
// the device name for the USB backend. Returns 0 on success, <0 on error.
int  m3d_transport_open(m3d_transport_t *t, m3d_backend_t backend, const char *target);

// Frame a parsed command into its M3D binary packet and send it. Returns the
// packet length (>=0) or <0 on error.
int  m3d_transport_send(m3d_transport_t *t, const m3d_gcode_t *g);

// Convenience: parse an ASCII command line then send it.
int  m3d_transport_send_line(m3d_transport_t *t, const char *ascii_line);

// Read a response from the printer (phase 2). Phase 1 virtual backend returns 0.
long m3d_transport_read(m3d_transport_t *t, char *buf, int maxlen);

// Perform the M115 connect handshake on a USB backend (the kernel CDC-ACM open
// already asserted DTR|RTS, which the Pro requires before it will talk). Sends
// "M115\r\n" (retried), scans the reply, and fills t->mode / t->boot_version /
// t->is_pro. Returns the detected mode. The virtual backend returns
// M3D_MODE_FIRMWARE without I/O. Non-destructive: read-only handshake, no flash.
m3d_mode_t m3d_transport_connect(m3d_transport_t *t);

void m3d_transport_close(m3d_transport_t *t);

#endif // M3D_TRANSPORT_H
