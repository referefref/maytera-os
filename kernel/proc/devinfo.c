// devinfo.c - MayteraOS Device Manager kernel backend (#325).
//
// Read-only syscalls that let userland enumerate the system's hardware by
// exposing the tables the kernel ALREADY builds at boot:
//   SYS_DEV_PCI_LIST  - PCI devices       (drivers/pci.c)
//   SYS_DEV_USB_LIST  - USB controllers + enumerated devices (drivers/usb.c,
//                       drivers/xhci.c; the passed-through NuForce DAC shows up)
//   SYS_DEV_IRQ_LIST  - populated IDT vectors / legacy IRQ lines (cpu/idt.c)
//   SYS_SYSINFO       - CPU count/vendor/brand/features, real PMM + heap memory
//                       (also satisfies #235), timer Hz, ACPI, uptime
//
// None of these re-enumerate hardware; they copy snapshots of existing kernel
// state into a caller-provided buffer (clamped to a caller-specified count) and
// return the number of records produced. No side effects. Also feeds #326.

#include "../devinfo.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../fs/fat.h"
#include "../drivers/pci.h"
#include "../drivers/usb.h"
#include "../drivers/xhci.h"
#include "../cpu/idt.h"
#include "../drivers/acpi.h"

// Globals owned by other subsystems.
extern volatile uint64_t timer_ticks;        // cpu/isr.h
extern uint32_t g_timer_hz;                   // cpu/pic.c
extern usb_controller_t usb_controllers[4];   // drivers/usb.h
extern int usb_controller_count;
extern fat_fs_t g_fat_fs;
extern int  proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                           int priority, uint32_t stack_size);
extern void proc_sleep(uint32_t ms);
extern void kprintf_set_dual_output(int on);
extern uint32_t smp_get_cpu_count(void);
extern uint32_t smp_get_online_count(void);

// ===========================================================================
// PCI list
// ===========================================================================
int64_t devinfo_pci_list(devinfo_pci_t *out, int max) {
    if (!out || max <= 0) return 0;
    int total = pci_get_device_count();
    int n = 0;
    for (int i = 0; i < total && n < max; i++) {
        pci_device_t *d = pci_get_device(i);
        if (!d) continue;
        devinfo_pci_t *r = &out[n++];
        memset(r, 0, sizeof(*r));
        r->bus        = d->bus;
        r->slot       = d->slot;
        r->func       = d->func;
        r->class_code = d->class_code;
        r->subclass   = d->subclass;
        r->prog_if    = d->prog_if;
        r->revision   = d->revision;
        r->irq_line   = d->interrupt_line;
        r->vendor_id  = d->vendor_id;
        r->device_id  = d->device_id;
        for (int b = 0; b < 6; b++) r->bar[b] = d->bar[b];
        const char *cn = pci_get_class_name(d->class_code, d->subclass);
        if (cn) {
            int j = 0;
            for (; cn[j] && j < (int)sizeof(r->class_name) - 1; j++) r->class_name[j] = cn[j];
            r->class_name[j] = '\0';
        }
    }
    return n;
}

// ===========================================================================
// USB list: host controllers + enumerated devices (legacy HCs and xHCI).
// ===========================================================================
int64_t devinfo_usb_list(devinfo_usb_t *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;

    // Legacy controllers (UHCI/OHCI/EHCI) registered in usb_controllers[].
    for (int c = 0; c < usb_controller_count && c < 4 && n < max; c++) {
        usb_controller_t *ctrl = &usb_controllers[c];
        if (!ctrl->initialized) continue;
        devinfo_usb_t *r = &out[n++];
        memset(r, 0, sizeof(*r));
        r->controller_type = (uint8_t)ctrl->type;
        r->is_controller   = 1;
        r->bus_port        = (uint8_t)c;
        for (int dvi = 0; dvi < ctrl->num_devices && n < max; dvi++) {
            usb_device_t *dv = &ctrl->devices[dvi];
            devinfo_usb_t *dr = &out[n++];
            memset(dr, 0, sizeof(*dr));
            dr->controller_type = (uint8_t)ctrl->type;
            dr->is_controller   = 0;
            dr->bus_port        = (uint8_t)dv->port;
            dr->speed           = (uint8_t)dv->speed;
            dr->address         = (uint8_t)dv->slot;
            dr->dev_class       = dv->desc.bDeviceClass;
            dr->subclass        = dv->desc.bDeviceSubClass;
            dr->protocol        = dv->desc.bDeviceProtocol;
            dr->num_interfaces  = dv->num_interfaces;
            dr->num_endpoints   = (uint8_t)dv->num_endpoints;
            dr->vendor_id       = dv->vendor_id;
            dr->product_id      = dv->product_id;
        }
    }

    // xHCI controller rows: this kernel already registers the xHCI host
    // controller in usb_controllers[] (emitted above), so only fall back to the
    // dedicated xHCI registry when usb_controllers[] is empty, to avoid listing
    // the controller twice.
    if (usb_controller_count == 0) {
        int xc = xhci_get_controller_count();
        for (int c = 0; c < xc && n < max; c++) {
            devinfo_usb_t *r = &out[n++];
            memset(r, 0, sizeof(*r));
            r->controller_type = DEVINFO_USB_XHCI;
            r->is_controller   = 1;
            r->bus_port        = (uint8_t)c;
        }
    }
    // xHCI enumerated devices (recorded during xhci_enumerate_devices).
    int xn = xhci_get_enum_count();
    for (int i = 0; i < xn && n < max; i++) {
        const xhci_enum_dev_t *d = xhci_get_enum_device(i);
        if (!d) continue;
        // Normalize xHCI port-speed codes (1=full,2=low,3=high,4=super,5=super+)
        // to the devinfo speed convention (0=full,1=low,2=high,3=super).
        uint8_t spd = 0;
        switch (d->speed) {
            case 1: spd = 0; break;  // full
            case 2: spd = 1; break;  // low
            case 3: spd = 2; break;  // high
            case 4: spd = 3; break;  // super
            case 5: spd = 3; break;  // super+
            default: spd = 0; break;
        }
        devinfo_usb_t *r = &out[n++];
        memset(r, 0, sizeof(*r));
        r->controller_type = DEVINFO_USB_XHCI;
        r->is_controller   = 0;
        r->bus_port        = (uint8_t)d->port;
        r->speed           = spd;
        r->address         = (uint8_t)d->address;
        r->dev_class       = d->dev_class;
        r->subclass        = d->dev_subclass;
        r->protocol        = d->dev_protocol;
        r->num_interfaces  = d->num_interfaces;
        r->vendor_id       = d->vendor_id;
        r->product_id      = d->product_id;
    }
    return n;
}

// ===========================================================================
// IRQ / IDT vector list: only populated (present) vectors are reported.
// ===========================================================================
int64_t devinfo_irq_list(devinfo_irq_t *out, int max) {
    if (!out || max <= 0) return 0;
    int n = 0;
    for (int v = 0; v < IDT_ENTRIES && n < max; v++) {
        uint8_t type_attr = 0;
        int has_handler = 0;
        int present = idt_get_vector_info(v, &type_attr, &has_handler);
        if (!present) continue;
        devinfo_irq_t *r = &out[n++];
        memset(r, 0, sizeof(*r));
        r->vector      = (uint16_t)v;
        r->present     = 1;
        r->has_handler = (uint8_t)has_handler;
        r->type_attr   = type_attr;
        if (v >= IRQ_BASE && v < IRQ_BASE + 16) {
            r->is_irq   = 1;
            r->irq_line = (int8_t)(v - IRQ_BASE);
        } else {
            r->is_irq   = 0;
            r->irq_line = -1;
        }
        r->count = 0;  // per-vector counts not tracked kernel-wide yet
    }
    return n;
}

// ===========================================================================
// System info: CPU / memory / timers / ACPI / uptime.
// ===========================================================================
static void devinfo_fill_cpu(devinfo_sysinfo_t *si) {
    uint32_t a, b, c, d;

    // Vendor string from CPUID leaf 0 (EBX, EDX, ECX).
    cpuid(0, &a, &b, &c, &d);
    memcpy(&si->cpu_vendor[0], &b, 4);
    memcpy(&si->cpu_vendor[4], &d, 4);
    memcpy(&si->cpu_vendor[8], &c, 4);
    si->cpu_vendor[12] = '\0';

    // Feature flags from leaf 1 (EDX + ECX).
    cpuid(1, &a, &b, &c, &d);
    uint32_t f = 0;
    if (d & (1u << 0))  f |= DEVINFO_FEAT_FPU;
    if (d & (1u << 4))  f |= DEVINFO_FEAT_TSC;
    if (d & (1u << 5))  f |= DEVINFO_FEAT_MSR;
    if (d & (1u << 6))  f |= DEVINFO_FEAT_PAE;
    if (d & (1u << 9))  f |= DEVINFO_FEAT_APIC;
    if (d & (1u << 24)) f |= DEVINFO_FEAT_FXSR;
    if (d & (1u << 25)) f |= DEVINFO_FEAT_SSE;
    if (d & (1u << 26)) f |= DEVINFO_FEAT_SSE2;
    if (d & (1u << 28)) f |= DEVINFO_FEAT_HTT;
    if (c & (1u << 0))  f |= DEVINFO_FEAT_SSE3;
    if (c & (1u << 9))  f |= DEVINFO_FEAT_SSSE3;
    if (c & (1u << 19)) f |= DEVINFO_FEAT_SSE41;
    if (c & (1u << 20)) f |= DEVINFO_FEAT_SSE42;
    if (c & (1u << 28)) f |= DEVINFO_FEAT_AVX;

    // AVX2 from leaf 7 (EBX bit 5).
    cpuid(7, &a, &b, &c, &d);
    if (b & (1u << 5)) f |= DEVINFO_FEAT_AVX2;

    // Long mode from extended leaf 0x80000001 (EDX bit 29).
    cpuid(0x80000000, &a, &b, &c, &d);
    uint32_t maxext = a;
    if (maxext >= 0x80000001) {
        cpuid(0x80000001, &a, &b, &c, &d);
        if (d & (1u << 29)) f |= DEVINFO_FEAT_LM;
    }
    si->features = f;

    // Brand string from extended leaves 0x80000002..0x80000004.
    si->cpu_brand[0] = '\0';
    if (maxext >= 0x80000004) {
        int off = 0;
        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
            cpuid(leaf, &a, &b, &c, &d);
            memcpy(&si->cpu_brand[off + 0],  &a, 4);
            memcpy(&si->cpu_brand[off + 4],  &b, 4);
            memcpy(&si->cpu_brand[off + 8],  &c, 4);
            memcpy(&si->cpu_brand[off + 12], &d, 4);
            off += 16;
        }
        si->cpu_brand[48] = '\0';
    }
}

int64_t devinfo_sysinfo(devinfo_sysinfo_t *si) {
    if (!si) return -1;
    memset(si, 0, sizeof(*si));

    si->cpu_count  = smp_get_cpu_count();
    si->cpu_online = smp_get_online_count();
    if (si->cpu_count == 0)  si->cpu_count = 1;
    if (si->cpu_online == 0) si->cpu_online = 1;
    devinfo_fill_cpu(si);

    uint64_t total_pages = pmm_get_total_pages();
    uint64_t used_pages  = pmm_get_used_pages();
    uint64_t free_pages  = pmm_get_free_pages();
    si->mem_total = total_pages * 4096ULL;
    si->mem_used  = used_pages  * 4096ULL;
    si->mem_free  = free_pages  * 4096ULL;

    si->heap_total = heap_get_total_size();
    si->heap_used  = heap_get_used_size();
    si->heap_free  = heap_get_free_size();

    si->timer_hz      = g_timer_hz;
    si->acpi_present  = acpi_is_initialized() ? 1 : 0;
    si->uptime_ticks  = timer_ticks;
    si->uptime_ms     = (g_timer_hz > 0) ? (timer_ticks * 1000ULL) / g_timer_hz : 0;
    return 0;
}

// ===========================================================================
// Gated serial self-test (#325). No-op unless /CONFIG/DEVTEST.CFG exists.
// Prints an lspci/lsusb-style device tree to serial as headless evidence.
// ===========================================================================
static const char *devinfo_usb_speed_name(int s) {
    switch (s) {
        case 0: return "full";
        case 1: return "low";
        case 2: return "high";
        case 3: return "super";
        default: return "?";
    }
}

static void devinfo_selftest_worker(void *arg) {
    (void)arg;
    proc_sleep(11000);   // let drivers + desktop settle
    uint32_t cfgsz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/DEVTEST.CFG", &cfgsz);
    if (!cfg) return;    // no flag -> silent no-op
    kfree(cfg);

    kprintf_set_dual_output(1);
    kprintf("\n========== DEVICE MANAGER SELFTEST (#325) ==========\n");

    // ---- PCI ----
    static devinfo_pci_t pci[64];
    int pn = (int)devinfo_pci_list(pci, 64);
    kprintf("[DEVTEST] PCI devices: %d\n", pn);
    for (int i = 0; i < pn; i++) {
        kprintf("  %02x:%02x.%x  %04x:%04x  class %02x:%02x pif %02x irq %u  %s\n",
                pci[i].bus, pci[i].slot, pci[i].func,
                pci[i].vendor_id, pci[i].device_id,
                pci[i].class_code, pci[i].subclass, pci[i].prog_if,
                pci[i].irq_line, pci[i].class_name);
    }

    // ---- USB ----
    static devinfo_usb_t usb[64];
    int un = (int)devinfo_usb_list(usb, 64);
    kprintf("[DEVTEST] USB entries: %d\n", un);
    int dac_found = 0;
    for (int i = 0; i < un; i++) {
        if (usb[i].is_controller) {
            kprintf("  HC[%u] type=%u (%s)\n", usb[i].bus_port, usb[i].controller_type,
                    usb_type_name(usb[i].controller_type));
        } else {
            kprintf("  dev %04x:%04x port %u addr %u speed %s class %02x:%02x:%02x ifaces %u\n",
                    usb[i].vendor_id, usb[i].product_id, usb[i].bus_port,
                    usb[i].address, devinfo_usb_speed_name(usb[i].speed),
                    usb[i].dev_class, usb[i].subclass, usb[i].protocol,
                    usb[i].num_interfaces);
            if (usb[i].vendor_id == 0x262a && usb[i].product_id == 0x10aa) dac_found = 1;
        }
    }
    kprintf("[DEVTEST] NuForce DAC 262a:10aa %s\n", dac_found ? "PRESENT" : "not found");

    // ---- IRQ ----
    static devinfo_irq_t irq[256];
    int in = (int)devinfo_irq_list(irq, 256);
    kprintf("[DEVTEST] populated IDT vectors: %d\n", in);
    int shown = 0;
    for (int i = 0; i < in; i++) {
        if (irq[i].is_irq) {
            kprintf("  IRQ %d -> vector %u (handler %s)\n",
                    irq[i].irq_line, irq[i].vector,
                    irq[i].has_handler ? "yes" : "no");
            shown++;
        }
    }
    if (!shown) kprintf("  (no legacy IRQ vectors populated)\n");

    // ---- SYSINFO ----
    devinfo_sysinfo_t si;
    devinfo_sysinfo(&si);
    kprintf("[DEVTEST] CPU: count=%u online=%u vendor=%s\n",
            si.cpu_count, si.cpu_online, si.cpu_vendor);
    kprintf("[DEVTEST] CPU brand: %s\n", si.cpu_brand);
    kprintf("[DEVTEST] CPU features: 0x%08x  sse=%d sse2=%d avx=%d avx2=%d fxsr=%d apic=%d lm=%d\n",
            si.features,
            !!(si.features & DEVINFO_FEAT_SSE),  !!(si.features & DEVINFO_FEAT_SSE2),
            !!(si.features & DEVINFO_FEAT_AVX),  !!(si.features & DEVINFO_FEAT_AVX2),
            !!(si.features & DEVINFO_FEAT_FXSR), !!(si.features & DEVINFO_FEAT_APIC),
            !!(si.features & DEVINFO_FEAT_LM));
    kprintf("[DEVTEST] MEM total=%lu MB used=%lu MB free=%lu MB\n",
            (unsigned long)(si.mem_total >> 20), (unsigned long)(si.mem_used >> 20),
            (unsigned long)(si.mem_free >> 20));
    kprintf("[DEVTEST] MEM total=%lu used=%lu free=%lu bytes\n",
            (unsigned long)si.mem_total, (unsigned long)si.mem_used,
            (unsigned long)si.mem_free);
    kprintf("[DEVTEST] HEAP total=%lu KB used=%lu KB free=%lu KB\n",
            (unsigned long)(si.heap_total >> 10), (unsigned long)(si.heap_used >> 10),
            (unsigned long)(si.heap_free >> 10));
    kprintf("[DEVTEST] timer=%u Hz  acpi=%d  uptime=%lu ticks (%lu ms)\n",
            si.timer_hz, si.acpi_present,
            (unsigned long)si.uptime_ticks, (unsigned long)si.uptime_ms);

    kprintf("========== DEVICE MANAGER SELFTEST: DONE ==========\n");
    kprintf_set_dual_output(0);
}

void devinfo_start_deferred_selftest(void) {
    proc_create_ex("devtest", devinfo_selftest_worker, 0, 1, 256 * 1024);
}
