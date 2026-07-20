// devinfo.h - Shared hardware-enumeration structures for the MayteraOS
// Device Manager (#325). These structs are the wire format for the read-only
// device-info syscalls (SYS_DEV_PCI_LIST, SYS_DEV_USB_LIST, SYS_DEV_IRQ_LIST,
// SYS_SYSINFO). The future userland Device Manager GUI app and libc reuse this
// exact header, so keep the layout stable. Also feeds #326 (AI driver
// generation needs concrete device specifics).
//
// Read-only, no side effects. Each list syscall copies up to a caller-specified
// count of records into a user buffer and returns the actual number copied.
#ifndef DEVINFO_H
#define DEVINFO_H

#include "types.h"

// --- PCI device record (mirrors drivers/pci.h pci_device_t, plus class name) -
typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  irq_line;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar[6];
    char     class_name[40];   // human-readable, e.g. "Network controller"
} devinfo_pci_t;

// USB controller type codes. These mirror drivers/usb.h USB_TYPE_* so the
// value in devinfo_usb_t.controller_type resolves through usb_type_name().
#define DEVINFO_USB_UHCI   0x00
#define DEVINFO_USB_OHCI   0x10
#define DEVINFO_USB_EHCI   0x20
#define DEVINFO_USB_XHCI   0x30

// --- USB record: a row is either a host controller or an enumerated device ---
typedef struct {
    uint8_t  controller_type;  // DEVINFO_USB_* (which HC this row belongs to)
    uint8_t  is_controller;    // 1 = host controller row, 0 = device row
    uint8_t  bus_port;         // root-hub port (device) / controller index (HC)
    uint8_t  speed;            // 0=full 1=low 2=high 3=super
    uint8_t  address;          // USB address / xHCI slot id
    uint8_t  dev_class;        // bDeviceClass (or interface class fallback)
    uint8_t  subclass;         // bDeviceSubClass
    uint8_t  protocol;         // bDeviceProtocol
    uint8_t  num_interfaces;
    uint8_t  num_endpoints;
    uint8_t  reserved[2];
    uint16_t vendor_id;        // idVendor
    uint16_t product_id;       // idProduct
} devinfo_usb_t;

// --- IRQ / IDT vector record ------------------------------------------------
typedef struct {
    uint16_t vector;        // IDT vector 0-255
    uint8_t  present;       // gate present bit (P) set
    uint8_t  has_handler;   // a C interrupt handler is registered
    uint8_t  type_attr;     // raw gate type/attr byte
    uint8_t  is_irq;        // vector is in the legacy IRQ range (32-47)
    int8_t   irq_line;      // legacy IRQ number (vector-32), or -1
    uint8_t  reserved;
    uint64_t count;         // interrupts counted (0 if not tracked)
} devinfo_irq_t;

// CPU feature bitmask values for devinfo_sysinfo_t.features.
#define DEVINFO_FEAT_FPU    (1u << 0)
#define DEVINFO_FEAT_SSE    (1u << 1)
#define DEVINFO_FEAT_SSE2   (1u << 2)
#define DEVINFO_FEAT_SSE3   (1u << 3)
#define DEVINFO_FEAT_SSSE3  (1u << 4)
#define DEVINFO_FEAT_SSE41  (1u << 5)
#define DEVINFO_FEAT_SSE42  (1u << 6)
#define DEVINFO_FEAT_AVX    (1u << 7)
#define DEVINFO_FEAT_AVX2   (1u << 8)
#define DEVINFO_FEAT_FXSR   (1u << 9)
#define DEVINFO_FEAT_APIC   (1u << 10)
#define DEVINFO_FEAT_HTT    (1u << 11)
#define DEVINFO_FEAT_TSC    (1u << 12)
#define DEVINFO_FEAT_PAE    (1u << 13)
#define DEVINFO_FEAT_MSR    (1u << 14)
#define DEVINFO_FEAT_LM     (1u << 15)  // long mode (extended leaf)

// --- System info (CPU / memory / timers / ACPI / uptime) --------------------
typedef struct {
    uint32_t cpu_count;        // total detected logical CPUs
    uint32_t cpu_online;       // CPUs currently online
    char     cpu_vendor[16];   // e.g. "GenuineIntel" / "AuthenticAMD"
    char     cpu_brand[52];    // CPUID brand string (NUL-terminated)
    uint32_t features;         // DEVINFO_FEAT_* bitmask
    uint32_t timer_hz;         // PIT/scheduler tick frequency
    uint8_t  acpi_present;     // ACPI initialized
    uint8_t  reserved[7];
    uint64_t mem_total;        // physical RAM, bytes (PMM)
    uint64_t mem_used;         // used physical RAM, bytes (PMM)
    uint64_t mem_free;         // free physical RAM, bytes (PMM)
    uint64_t heap_total;       // kernel heap, bytes
    uint64_t heap_used;
    uint64_t heap_free;
    uint64_t uptime_ticks;     // raw timer ticks since boot
    uint64_t uptime_ms;        // uptime in milliseconds
} devinfo_sysinfo_t;

#endif // DEVINFO_H
