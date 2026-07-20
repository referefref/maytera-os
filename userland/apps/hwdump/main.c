// hwdump - headless real-hardware diagnostic: dumps the PCI device list (and
// IRQ vectors) via the #325 devinfo syscalls, then sleeps a few seconds so the
// RemoteCtrl `shell` pump on OLD kernels (which drop a fast child's final
// output) streams it out while the process is still alive. Used over the mdev
// bridge to inspect real iMac14,4 hardware for #71 (audio) / #383 (WiFi) / #307.
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"

#define SYS_DEV_PCI_LIST 272
#define SYS_DEV_IRQ_LIST 274

typedef struct {
    unsigned char  bus, slot, func, class_code, subclass, prog_if, revision, irq_line;
    unsigned short vendor_id, device_id;
    unsigned int   bar[6];
    char           class_name[40];
} devinfo_pci_t;

static devinfo_pci_t pci[64];

int main(void) {
    int n = (int)syscall2(SYS_DEV_PCI_LIST, (long)pci, 64);
    printf("HWDUMP-PCI count=%d\n", n);
    for (int i = 0; i < n && i < 64; i++) {
        devinfo_pci_t *d = &pci[i];
        printf("%02x:%02x.%x %04x:%04x cls=%02x sub=%02x if=%02x irq=%d bar0=%08x [%s]\n",
               d->bus, d->slot, d->func, d->vendor_id, d->device_id,
               d->class_code, d->subclass, d->prog_if, d->irq_line,
               d->bar[0], d->class_name);
    }
    printf("HWDUMP-END\n");
    sys_sleep(4000);   // hold output long enough for the old-kernel shell pump
    return 0;
}
