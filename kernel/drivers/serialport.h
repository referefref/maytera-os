// serialport.h - the mediated serial-port gateway (Stage 2, serial.port).
//
// docs/SYSTEM_CAPABILITY_API.md sections 1.5, 4.2 and Stage 4. Modelled on
// dos/diskimg.h dimg_vol_t: an app names a PORT from a kernel-published list,
// never a base address and never a raw /dev path. The kernel answers with a
// class, flags and one opaque name; there is no COM base, no channel, no device
// identity the app can express. The only verb it gets is SERIAL_OPEN(name), and
// that verb is gated by the serial.port capability at the dispatcher chokepoint.
//
// WHY THIS FILE IS C, not Rust (the 2026-07-16 rule). It is glue over existing
// C hardware primitives: legacy port I/O (serial.c inb/outb / serial_write),
// the file_ops_t/file_alloc description model, and dev.c open factories. It
// makes no policy decision; the capability policy lives in rustkern/caps.rs.
#ifndef DRIVERS_SERIALPORT_H
#define DRIVERS_SERIALPORT_H

#include "../types.h"
#include "dev.h"      // dev_open_fn

struct file;

// --- classes / flags -------------------------------------------------------
#define SERIALPORT_CLS_NONE      0u
#define SERIALPORT_CLS_UART16550 1u   // a legacy 16550 UART (COM2+; COM1 is the console)
#define SERIALPORT_CLS_USB_CDC   2u   // a USB CDC-ACM adapter (/dev/ttyACM0 backend)

#define SERIALPORT_F_NONE     0u
#define SERIALPORT_F_READONLY 1u

#define SERIALPORT_NAME_MAX  16
#define SERIALPORT_MAX        4        // published ports; small and fixed

// ABI struct written into a Ring-3 buffer by SYS_SERIAL_LIST. It names a port,
// its class and its flags. There is deliberately NO base address, NO /dev path
// and NO device handle: the only noun that crosses is the name. Size locked in
// proc/syscall_argtab_lock.c against SZ_SERIAL_PUB in rustkern/argtab.rs.
typedef struct {
    char     name[SERIALPORT_NAME_MAX];  // the noun the app names back to SERIAL_OPEN
    uint32_t cls;                        // SERIALPORT_CLS_*
    uint32_t flags;                      // SERIALPORT_F_*
} serial_pub_t;

// Register a USB CDC-ACM style port backed by a dev_open factory (the same
// factory /dev/<name> uses). Called by the CDC-ACM driver when a device
// attaches. Returns 0, or -1 (dup / table full).
int serialport_register_dev(const char *name, uint32_t cls, dev_open_fn open);

// Probe a legacy 16550 UART at base with a self-contained loopback test that
// NEVER touches serial.c's g_serial_present global (which is the COM1 console
// verdict), and if present register it under name. Returns 0 if registered,
// -1 if absent/dup/full. COM1 must NOT be passed: it is the kernel console.
int serialport_register_uart(const char *name, uint16_t base);

// Is name a currently-published port? The kernel-owned enumeration the scope
// is validated against (design 4.3): a consent prompt is refused for a name the
// kernel does not publish, so an app cannot be granted a port that is not there.
int serialport_is_published(const char *name);

// Copy up to max published ports into out (a kernel buffer). Returns count.
uint32_t serialport_list(serial_pub_t *out, uint32_t max);

// Open a published port. Returns a refcounted struct file*, or NULL. The caller
// (sys_serial_open) has already proven the serial.port grant covers name.
struct file *serialport_open(const char *name, int flags);

// Boot init: publishes COM2 as "ttyS1" if a UART is actually present there.
void serialport_init(void);

#endif // DRIVERS_SERIALPORT_H
