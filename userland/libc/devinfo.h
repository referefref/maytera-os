// devinfo.h - userland mirror of the kernel device-enumeration structs (#325).
// Source of truth: kernel/devinfo.h. Keep the layout byte-identical: these are
// the wire format for the read-only SYS_DEV_* / SYS_SYSINFO syscalls used by the
// Device Manager app. Additive, no side effects.
#ifndef _LIBC_DEVINFO_H
#define _LIBC_DEVINFO_H

#include "types.h"
#include "syscall.h"

// --- Syscall numbers (mirror kernel proc/syscall.h) ------------------------
#ifndef SYS_DEV_PCI_LIST
#define SYS_DEV_PCI_LIST  272  // (devinfo_pci_t *buf, int max) -> count
#endif
#ifndef SYS_DEV_USB_LIST
#define SYS_DEV_USB_LIST  273  // (devinfo_usb_t *buf, int max) -> count
#endif
#ifndef SYS_DEV_IRQ_LIST
#define SYS_DEV_IRQ_LIST  274  // (devinfo_irq_t *buf, int max) -> count
#endif
#ifndef SYS_SYSINFO
#define SYS_SYSINFO       275  // (devinfo_sysinfo_t *out) -> 0/-1
#endif

// --- PCI device record -----------------------------------------------------
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
    char     class_name[40];
} devinfo_pci_t;

// USB controller type codes (mirror DEVINFO_USB_* in kernel).
#define DEVINFO_USB_UHCI   0x00
#define DEVINFO_USB_OHCI   0x10
#define DEVINFO_USB_EHCI   0x20
#define DEVINFO_USB_XHCI   0x30

// --- USB record: a host-controller row or an enumerated-device row ---------
typedef struct {
    uint8_t  controller_type;
    uint8_t  is_controller;
    uint8_t  bus_port;
    uint8_t  speed;            // 0=full 1=low 2=high 3=super
    uint8_t  address;
    uint8_t  dev_class;
    uint8_t  subclass;
    uint8_t  protocol;
    uint8_t  num_interfaces;
    uint8_t  num_endpoints;
    uint8_t  reserved[2];
    uint16_t vendor_id;
    uint16_t product_id;
} devinfo_usb_t;

// --- IRQ / IDT vector record -----------------------------------------------
typedef struct {
    uint16_t vector;
    uint8_t  present;
    uint8_t  has_handler;
    uint8_t  type_attr;
    uint8_t  is_irq;
    int8_t   irq_line;
    uint8_t  reserved;
    uint64_t count;
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
#define DEVINFO_FEAT_LM     (1u << 15)

// --- System info -----------------------------------------------------------
typedef struct {
    uint32_t cpu_count;
    uint32_t cpu_online;
    char     cpu_vendor[16];
    char     cpu_brand[52];
    uint32_t features;
    uint32_t timer_hz;
    uint8_t  acpi_present;
    uint8_t  reserved[7];
    uint64_t mem_total;
    uint64_t mem_used;
    uint64_t mem_free;
    uint64_t heap_total;
    uint64_t heap_used;
    uint64_t heap_free;
    uint64_t uptime_ticks;
    uint64_t uptime_ms;
} devinfo_sysinfo_t;

// --- Syscall wrappers ------------------------------------------------------
static inline int sys_dev_pci_list(devinfo_pci_t *buf, int max) {
    return (int)syscall2(SYS_DEV_PCI_LIST, (long)buf, (long)max);
}
static inline int sys_dev_usb_list(devinfo_usb_t *buf, int max) {
    return (int)syscall2(SYS_DEV_USB_LIST, (long)buf, (long)max);
}
static inline int sys_dev_irq_list(devinfo_irq_t *buf, int max) {
    return (int)syscall2(SYS_DEV_IRQ_LIST, (long)buf, (long)max);
}
static inline int sys_sysinfo(devinfo_sysinfo_t *out) {
    return (int)syscall1(SYS_SYSINFO, (long)out);
}

#endif // _LIBC_DEVINFO_H
