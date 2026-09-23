// pci.c - PCI Bus driver implementation
#include "pci.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/bootlog.h"   // ASUS bring-up: LOUD overflow, on disk as well as serial

// Maximum PCI functions to track.
//
// This was 64, and overflow was SILENT: pci_check_device() simply returned
// once the table was full, so every function past the 64th was invisible to
// EVERY subsystem (storage, USB, network, audio) and nothing anywhere said so.
// On unfamiliar hardware, "the controller we needed was the 65th function" is
// precisely the evidence we would need and would not have had. A modern laptop
// chipset plus its PCIe root ports, bridges and multi-function devices gets
// close enough to 64 that this is not a theoretical margin.
//
// 256 costs sizeof(pci_device_t) * 256 = 60 * 256 = 15360 bytes of .bss
// (up from 3840, so +11520 bytes), which is nothing against the kernel's
// existing static footprint, and is a fixed cost with no allocator involved,
// so it cannot fail at the point of use the way a kmalloc'd table could.
#define MAX_PCI_DEVICES 256

// Discovered devices
static pci_device_t pci_devices[MAX_PCI_DEVICES];
static int pci_device_count = 0;
// Functions seen by the scan but NOT recorded because the table was full.
// Counted rather than merely detected, so the report can say HOW MUCH was lost.
static int pci_dropped_count = 0;

// Forward declaration
static const char *pci_class_name(uint8_t class_code, uint8_t subclass);

// Build PCI address
static uint32_t pci_address(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    return (1U << 31) |  // Enable bit
           ((uint32_t)bus << 16) |
           ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) |
           (offset & 0xFC);
}

// Read 8-bit value from PCI config space
uint8_t pci_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    return inb(PCI_CONFIG_DATA + (offset & 3));
}

// Read 16-bit value from PCI config space
uint16_t pci_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    return inw(PCI_CONFIG_DATA + (offset & 2));
}

// Read 32-bit value from PCI config space
uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    return inl(PCI_CONFIG_DATA);
}

// Write 8-bit value to PCI config space
void pci_write8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint8_t value) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    outb(PCI_CONFIG_DATA + (offset & 3), value);
}

// Write 16-bit value to PCI config space
void pci_write16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint16_t value) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    outw(PCI_CONFIG_DATA + (offset & 2), value);
}

// Write 32-bit value to PCI config space
void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDR, pci_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

// Check and add a PCI device
static void pci_check_device(uint8_t bus, uint8_t slot, uint8_t func) {
    uint16_t vendor_id = pci_read16(bus, slot, func, PCI_VENDOR_ID);

    // Check if device exists
    if (vendor_id == 0xFFFF) return;

    if (pci_device_count >= MAX_PCI_DEVICES) {
        // Count it. Deliberately no kprintf here: this is inside the scan loop
        // and a machine that overflows could emit hundreds of lines. pci_init()
        // reports the total once, loudly, on both serial and /BOOTLOG.TXT.
        pci_dropped_count++;
        return;
    }

    pci_device_t *dev = &pci_devices[pci_device_count++];

    dev->bus = bus;
    dev->slot = slot;
    dev->func = func;
    dev->vendor_id = vendor_id;
    dev->device_id = pci_read16(bus, slot, func, PCI_DEVICE_ID);
    dev->class_code = pci_read8(bus, slot, func, PCI_CLASS);
    dev->subclass = pci_read8(bus, slot, func, PCI_SUBCLASS);
    dev->prog_if = pci_read8(bus, slot, func, PCI_PROG_IF);
    dev->revision = pci_read8(bus, slot, func, PCI_REVISION_ID);
    dev->header_type = pci_read8(bus, slot, func, PCI_HEADER_TYPE);
    dev->interrupt_line = pci_read8(bus, slot, func, PCI_INTERRUPT_LINE);
    dev->interrupt_pin = pci_read8(bus, slot, func, PCI_INTERRUPT_PIN);

    // Read BARs (for type 0 headers only)
    if ((dev->header_type & 0x7F) == 0) {
        for (int i = 0; i < 6; i++) {
            dev->bar[i] = pci_read32(bus, slot, func, PCI_BAR0 + i * 4);
        }
        // #imacnic: subsystem identity, type-0 header only. See pci.h for why
        // reading these offsets on a bridge would produce a confident wrong
        // answer rather than an obviously missing one.
        dev->subsys_vendor = pci_read16(bus, slot, func, PCI_SUBSYS_VENDOR);
        dev->subsys_id     = pci_read16(bus, slot, func, PCI_SUBSYS_ID);
    }
}

// Scan PCI bus
static void pci_scan_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        uint16_t vendor_id = pci_read16(bus, slot, 0, PCI_VENDOR_ID);
        if (vendor_id == 0xFFFF) continue;

        pci_check_device(bus, slot, 0);

        // Check if multi-function device
        uint8_t header_type = pci_read8(bus, slot, 0, PCI_HEADER_TYPE);
        if (header_type & 0x80) {
            for (uint8_t func = 1; func < 8; func++) {
                pci_check_device(bus, slot, func);
            }
        }
    }
}

// Initialize PCI driver
void pci_init(void) {
    kprintf("[PCI] Scanning PCI bus...\n");

    pci_device_count = 0;

    // Scan bus 0 and check for additional buses
    for (int bus = 0; bus < 256; bus++) {
        pci_scan_bus(bus);
    }

    // Greppable and unconditional: the count is evidence in its own right on a
    // machine nobody has booted before, whether or not anything overflowed.
    kprintf("[PCI] Found %d devices (table cap %d, dropped %d)\n",
            pci_device_count, MAX_PCI_DEVICES, pci_dropped_count);
    bootlog_write("[PCI] found=%d cap=%d dropped=%d",
                  pci_device_count, MAX_PCI_DEVICES, pci_dropped_count);

    if (pci_dropped_count > 0) {
        // This is a correctness failure, not a curiosity: those functions are
        // invisible to every driver, so a missing storage/USB controller here
        // looks exactly like "no driver for it". Say so in both places, because
        // a stick-booted laptop has no serial port and /BOOTLOG.TXT is the only
        // surviving channel.
        kprintf("[PCI] BUG: PCI device table FULL - %d function(s) NOT recorded "
                "and invisible to all drivers; raise MAX_PCI_DEVICES\n",
                pci_dropped_count);
        bootlog_write("[PCI] BUG: device table FULL - %d function(s) NOT "
                      "recorded and invisible to all drivers (cap %d)",
                      pci_dropped_count, MAX_PCI_DEVICES);
    }

    // #imacnic: and now the table itself, durably. See pci_bootlog_inventory().
    // Unconditional: a device inventory that only exists on a serial port the
    // machine does not have is not an inventory.
    pci_bootlog_inventory();
}

// ---------------------------------------------------------------------------
// #imacnic: DURABLE PCI INVENTORY
// ---------------------------------------------------------------------------

// Render BAR `i` of `dev` into `out`, and return how many BAR slots it consumed
// (2 for a 64-bit BAR, 1 otherwise) so the caller never prints the upper half of
// a 64-bit BAR as though it were a separate region. Writes an empty string for
// an unimplemented (zero) BAR.
//
// DELIBERATELY DOES NOT CALL pci_get_bar_size(). Sizing a BAR means writing
// all-ones into it and reading the mask back, which momentarily aims a live
// device's address decode at 0xFFFFFFFF before the original value is restored.
// Doing that to every function of an unfamiliar machine during boot, purely to
// make a log line more informative, is a bad trade against the thing we are
// trying to protect: the boot. The base address and the BAR type are enough to
// write a driver against, and a driver that needs the size asks at bring-up,
// once, for the one device it owns.
static int pci_bar_render(pci_device_t *dev, int i, char *out, size_t outsz) {
    uint32_t bar = dev->bar[i];
    out[0] = 0;
    if (bar == 0) return 1;

    if (bar & PCI_BAR_IO) {
        snprintf(out, outsz, " bar%d=io:0x%04x", i, (unsigned)(bar & ~0x3u));
        return 1;
    }
    // Bit 3 is prefetchable; bits 2:1 are the type (00 = 32-bit, 10 = 64-bit).
    const char *pf = (bar & 0x8) ? "pf" : "";
    if ((bar & 0x6) == PCI_BAR_MEM_64 && i < 5) {
        uint64_t addr = ((uint64_t)dev->bar[i + 1] << 32) | (uint64_t)(bar & ~0xFu);
        snprintf(out, outsz, " bar%d=mem64%s:0x%llx", i, pf,
                 (unsigned long long)addr);
        return 2;
    }
    snprintf(out, outsz, " bar%d=mem32%s:0x%08x", i, pf, (unsigned)(bar & ~0xFu));
    return 1;
}

// Append `src` at *off within `dst` (capacity dstsz), keeping *off correct even
// when snprintf reports the length it WOULD have written. One helper rather
// than the same three-line clamp copied at each call site.
static void pci_str_append(char *dst, size_t dstsz, size_t *off, const char *src) {
    if (*off + 1 >= dstsz) return;
    int n = snprintf(dst + *off, dstsz - *off, "%s", src);
    if (n < 0) return;
    size_t room = dstsz - *off - 1;
    *off += ((size_t)n > room) ? room : (size_t)n;
}

// Format the subsystem identity, or say plainly that this header type does not
// have one, rather than printing 0000:0000 and letting a reader take it for a
// real answer from a real register.
static void pci_subsys_str(pci_device_t *d, char *out, size_t outsz) {
    if ((d->header_type & 0x7F) == 0)
        snprintf(out, outsz, "%04x:%04x", d->subsys_vendor, d->subsys_id);
    else
        snprintf(out, outsz, "n/a-hdr%02x", d->header_type & 0x7F);
}

// One durable line per function. Called at the end of pci_init(), which runs at
// boot stage 20, long before /DEVLOG.TXT (stage 38) exists: a boot that hangs
// anywhere in USB, storage or SMP bring-up still leaves the full inventory.
//
// COST. 23 functions on the owner's iMac at roughly 200 bytes each is ~4.6 KB of
// the 96 KB in-RAM bootlog buffer, spent at the earliest point in boot, so these
// lines are appended to /BOOTLOG.TXT before anything else competes for the
// buffer. That is the right place to spend it: an inventory you cannot read is
// worth nothing, and every later line in the file presupposes knowing what
// machine produced it.
void pci_bootlog_inventory(void) {
    // ONE DEVICE WRITE, NOT TWENTY-FOUR. /BOOTLOG.TXT is rewritten (or appended
    // to) on every bootlog_write(), so a burst of lines is a burst of writes
    // over whatever the root device happens to be, which on the #307 USB-MSC
    // path is slow enough to have wedged a boot before (#373). The defer window
    // keeps the serial mirror and the RAM buffer and suppresses only the device
    // flush; the closing line after bootlog_defer_end() carries the whole
    // accumulated delta down in a single transaction.
    //
    // In practice this window costs nothing at all today, because pci_init()
    // runs at boot stage 20 and bootlog_arm() does not happen until stage 37:
    // there is no medium yet, so nothing would have been flushed regardless.
    // The window is here so that stays true if pci_init() ever moves later, and
    // so a reader does not have to reconstruct that argument to be sure.
    bootlog_defer_begin();

    bootlog_write("[PCI] full device table follows, one line per function (%d of them). "
                  "Fields: bus:slot.func vendor:device sub=subsystem-vendor:subsystem-device "
                  "class=class:subclass:progif rev hdr irq=line/pin cmd=command-register, "
                  "then every non-zero BAR with its type, then the class name. "
                  "cmd bit0=IO-space bit1=MEM-space bit2=BUS-MASTER bit10=INTx-disabled. "
                  "irq pin '-' means the function asserts no legacy INTx at all.",
                  pci_device_count);

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t *d = &pci_devices[i];

        char bars[224];
        size_t off = 0;
        bars[0] = 0;
        for (int b = 0; b < 6; ) {
            char one[48];
            int used = pci_bar_render(d, b, one, sizeof(one));
            if (one[0]) pci_str_append(bars, sizeof(bars), &off, one);
            b += used;
        }
        if (off == 0) pci_str_append(bars, sizeof(bars), &off, " (no BARs)");

        char sub[16];
        pci_subsys_str(d, sub, sizeof(sub));

        // Read the command register live rather than caching it at scan time:
        // this runs microseconds later, but stating what it IS is honest and
        // costs one config read.
        uint16_t cmd = pci_read16(d->bus, d->slot, d->func, PCI_COMMAND);
        char pin = (d->interrupt_pin <= 4) ? "-ABCD"[d->interrupt_pin] : '?';

        bootlog_write("[PCI] %02x:%02x.%x %04x:%04x sub=%s class=%02x:%02x:%02x "
                      "rev=%02x hdr=%02x irq=%u/%c cmd=0x%04x%s \"%s\"",
                      d->bus, d->slot, d->func,
                      d->vendor_id, d->device_id, sub,
                      d->class_code, d->subclass, d->prog_if,
                      d->revision, d->header_type,
                      (unsigned)d->interrupt_line, pin, (unsigned)cmd,
                      bars,
                      pci_class_name(d->class_code, d->subclass));
    }

    bootlog_defer_end();
    // The closing line is what actually persists everything above it.
    bootlog_write("[PCI] end of device table (%d function(s) listed, %d dropped). "
                  "This table is written at boot stage 20, so it survives a hang "
                  "anywhere later; /DEVLOG.TXT at stage 38 carries the same data "
                  "plus the memory map, ACPI tables and USB descriptor trees.",
                  pci_device_count, pci_dropped_count);
}

// Is this a class a user would expect to be driven? Bridges and host bridges are
// excluded on purpose: they are legitimately unclaimed on every machine, and
// listing twelve of them would bury the one storage or network function that
// actually is missing a driver.
static int pci_class_is_interesting(uint8_t class_code) {
    switch (class_code) {
        case PCI_CLASS_STORAGE:
        case PCI_CLASS_NETWORK:
        case PCI_CLASS_DISPLAY:
        case PCI_CLASS_MULTIMEDIA:
        case PCI_CLASS_SERIAL:
        case 0x0D:              // Wireless (WiFi / Bluetooth)
            return 1;
        default:
            return 0;
    }
}

void pci_bootlog_claims(void) {
    // Same one-flush discipline as pci_bootlog_inventory(), and here it is NOT
    // free: this runs after bootlog_arm(), so without the window each line
    // below would be its own write to the root device.
    bootlog_defer_begin();

    int claimed = 0, unclaimed_interesting = 0, unclaimed_other = 0;
    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t *d = &pci_devices[i];
        if (d->claimed) { claimed++; continue; }
        if (pci_class_is_interesting(d->class_code)) unclaimed_interesting++;
        else unclaimed_other++;
    }

    bootlog_write("[PCI] driver claim summary: %d of %d function(s) claimed by a driver; "
                  "%d unclaimed in a class a user would expect to work (listed below); "
                  "%d unclaimed bridges/host-bridges/other (expected, not listed). "
                  "A function is 'claimed' only when a driver called pci_mark_claimed() "
                  "after SUCCESSFUL bring-up, so unclaimed means 'not working', not "
                  "merely 'not recognised'.",
                  claimed, pci_device_count, unclaimed_interesting, unclaimed_other);

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t *d = &pci_devices[i];
        if (d->claimed || !pci_class_is_interesting(d->class_code)) continue;

        char sub[16];
        pci_subsys_str(d, sub, sizeof(sub));

        // The network case gets named explicitly. "No driver for the Ethernet
        // controller" and "no Ethernet controller" produce identical symptoms
        // from userland, and only one of them is fixable by writing code.
        const char *note = "";
        if (d->class_code == PCI_CLASS_NETWORK)
            note = "  <== NETWORK CLASS: this machine HAS this Ethernet/network "
                   "controller and this kernel does NOT drive it";
        else if (d->class_code == PCI_CLASS_STORAGE)
            note = "  <== STORAGE CLASS: a disk behind this controller is invisible";

        bootlog_write("[PCI] UNCLAIMED %02x:%02x.%x %04x:%04x sub=%s class=%02x:%02x:%02x "
                      "\"%s\"%s",
                      d->bus, d->slot, d->func, d->vendor_id, d->device_id, sub,
                      d->class_code, d->subclass, d->prog_if,
                      pci_class_name(d->class_code, d->subclass), note);
    }

    bootlog_defer_end();
    bootlog_write("[PCI] end of claim summary (%d claimed / %d total).", claimed,
                  pci_device_count);
}

// ASUS bring-up: how many PCI functions the scan found but could NOT record.
// Non-zero means the inventory in /DEVLOG.TXT is INCOMPLETE and any
// "no such device" conclusion drawn from it is unsound. See pci.h.
int pci_get_dropped_count(void) {
    return pci_dropped_count;
}

// #418: record that a driver successfully claimed this PCI function. See
// pci.h for why this exists (closing out "no driver for this device" with
// certainty in /DEVLOG.TXT rather than an absence-of-evidence argument).
void pci_mark_claimed(pci_device_t *dev, const char *driver_name) {
    if (!dev) return;
    dev->claimed = 1;
    int i = 0;
    if (driver_name) {
        for (; i < (int)sizeof(dev->claimed_by) - 1 && driver_name[i]; i++) {
            dev->claimed_by[i] = driver_name[i];
        }
    }
    dev->claimed_by[i] = 0;
}

// Find device by vendor/device ID
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].device_id == device_id) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

// Find device by class/subclass
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

// Find device by vendor ID only
pci_device_t *pci_find_vendor(uint16_t vendor_id) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

// Find device by vendor ID and class
pci_device_t *pci_find_vendor_class(uint16_t vendor_id, uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < pci_device_count; i++) {
        if (pci_devices[i].vendor_id == vendor_id &&
            pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass) {
            return &pci_devices[i];
        }
    }
    return NULL;
}

// Find next device matching class (for iterating)
int pci_find_next_class(uint8_t class_code, uint8_t subclass, int start_idx) {
    for (int i = start_idx + 1; i < pci_device_count; i++) {
        if (pci_devices[i].class_code == class_code &&
            pci_devices[i].subclass == subclass) {
            return i;
        }
    }
    return -1;
}

// Enable bus mastering
void pci_enable_bus_master(pci_device_t *dev) {
    uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER | PCI_CMD_MEMORY | PCI_CMD_IO;
    pci_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);
}

// Get BAR address
uint64_t pci_get_bar_address(pci_device_t *dev, int bar_num) {
    if (bar_num < 0 || bar_num > 5) return 0;

    uint32_t bar = dev->bar[bar_num];

    if (bar & PCI_BAR_IO) {
        // I/O BAR
        return bar & ~0x3;
    } else {
        // Memory BAR
        if ((bar & 0x6) == PCI_BAR_MEM_64 && bar_num < 5) {
            // 64-bit BAR
            uint64_t addr = (bar & ~0xF);
            addr |= ((uint64_t)dev->bar[bar_num + 1] << 32);
            return addr;
        } else {
            // 32-bit BAR
            return bar & ~0xF;
        }
    }
}

// #71: walk the PCI capabilities list (PCI_STATUS bit 4 gates whether the
// Capabilities Pointer at PCI_CAP_PTR is even valid). Each capability is a
// {id, next-pointer} pair; next==0 terminates the list. Bounded to 48 hops so
// a malformed/cyclic list can never hang boot.
uint8_t pci_find_capability(pci_device_t *dev, uint8_t cap_id) {
    if (!dev) return 0;

    uint16_t status = pci_read16(dev->bus, dev->slot, dev->func, PCI_STATUS);
    if (!(status & PCI_STATUS_CAPLIST)) return 0;

    uint8_t ptr = pci_read8(dev->bus, dev->slot, dev->func, PCI_CAP_PTR) & 0xFC;
    for (int guard = 0; ptr != 0 && guard < 48; guard++) {
        uint8_t id = pci_read8(dev->bus, dev->slot, dev->func, ptr);
        if (id == cap_id) return ptr;
        ptr = pci_read8(dev->bus, dev->slot, dev->func, ptr + 1) & 0xFC;
    }
    return 0;
}

// #71: program + enable MSI on a device (see pci.h for the full rationale).
// Message Address = 0xFEE00000 | (dest_apic_id << 12) targets the Local APIC
// directly (RH=0 no redirection hint, DM=0 physical destination mode); Message
// Data = vector (delivery mode 0 = Fixed, trigger = edge). Handles both the
// 32-bit-only and 64-bit-capable MSI capability layouts. Also sets
// PCI_CMD_INT_DISABLE so the device does not ALSO keep asserting its (likely
// unrouted) legacy INTx line once MSI is live.
int pci_enable_msi(pci_device_t *dev, uint8_t vector, uint32_t dest_apic_id) {
    uint8_t cap = pci_find_capability(dev, PCI_CAP_ID_MSI);
    if (!cap) return 0;

    uint16_t msgctl = pci_read16(dev->bus, dev->slot, dev->func, cap + 2);
    int is64bit = (msgctl & (1 << 7)) != 0;
    int per_vector_mask = (msgctl & (1 << 8)) != 0;

    uint32_t addr_lo = 0xFEE00000u | ((dest_apic_id & 0xFF) << 12);
    uint16_t data = vector;   // fixed delivery (bits 10:8 = 0), edge trigger

    pci_write32(dev->bus, dev->slot, dev->func, cap + 4, addr_lo);
    if (is64bit) {
        pci_write32(dev->bus, dev->slot, dev->func, cap + 8, 0);           // address hi
        pci_write16(dev->bus, dev->slot, dev->func, cap + 12, data);
        if (per_vector_mask) {
            pci_write32(dev->bus, dev->slot, dev->func, cap + 16, 0);      // unmask vector 0
        }
    } else {
        pci_write16(dev->bus, dev->slot, dev->func, cap + 8, data);
        if (per_vector_mask) {
            pci_write32(dev->bus, dev->slot, dev->func, cap + 12, 0);      // unmask vector 0
        }
    }

    // Multiple Message Enable = 0 (we only ever use a single vector), MSI Enable = 1.
    msgctl &= (uint16_t)~(0x7 << 4);
    msgctl |= 0x1;
    pci_write16(dev->bus, dev->slot, dev->func, cap + 2, msgctl);

    uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, PCI_COMMAND);
    cmd |= PCI_CMD_INT_DISABLE;
    pci_write16(dev->bus, dev->slot, dev->func, PCI_COMMAND, cmd);

    return 1;
}

// Get BAR size (by writing all 1s and reading back)
uint32_t pci_get_bar_size(pci_device_t *dev, int bar_num) {
    if (bar_num < 0 || bar_num > 5) return 0;

    uint8_t offset = PCI_BAR0 + bar_num * 4;
    uint32_t original = pci_read32(dev->bus, dev->slot, dev->func, offset);

    // Write all 1s
    pci_write32(dev->bus, dev->slot, dev->func, offset, 0xFFFFFFFF);
    uint32_t size_mask = pci_read32(dev->bus, dev->slot, dev->func, offset);

    // Restore original value
    pci_write32(dev->bus, dev->slot, dev->func, offset, original);

    if (original & PCI_BAR_IO) {
        size_mask &= ~0x3;
    } else {
        size_mask &= ~0xF;
    }

    return ~size_mask + 1;
}

// Get total device count
int pci_get_device_count(void) {
    return pci_device_count;
}

// Get device by index
pci_device_t *pci_get_device(int index) {
    if (index < 0 || index >= pci_device_count) return NULL;
    return &pci_devices[index];
}

// Get class name (public version)
const char *pci_get_class_name(uint8_t class_code, uint8_t subclass) {
    return pci_class_name(class_code, subclass);
}

// Get class name (internal)
static const char *pci_class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
        case 0x01:  // Storage
            switch (subclass) {
                case 0x00: return "SCSI Controller";
                case 0x01: return "IDE Controller";
                case 0x05: return "ATA Controller";
                case 0x06: return "SATA Controller";
                case 0x08: return "NVMe Controller";
                default: return "Storage Controller";
            }
        case 0x02:  // Network
            switch (subclass) {
                case 0x00: return "Ethernet Controller";
                case 0x80: return "Network Controller";
                default: return "Network Controller";
            }
        case 0x03:  // Display
            switch (subclass) {
                case 0x00: return "VGA Controller";
                case 0x01: return "XGA Controller";
                case 0x02: return "3D Controller";
                case 0x80: return "Display Controller";
                default: return "Display Controller";
            }
        case 0x04:  // Multimedia
            switch (subclass) {
                case 0x00: return "Video Controller";
                case 0x01: return "Audio Controller";
                case 0x02: return "Telephony";
                case 0x03: return "HD Audio Controller";
                default: return "Multimedia Controller";
            }
        case 0x05:  // Memory
            switch (subclass) {
                case 0x00: return "RAM Controller";
                case 0x01: return "Flash Controller";
                default: return "Memory Controller";
            }
        case 0x06:  // Bridge
            switch (subclass) {
                case 0x00: return "Host Bridge";
                case 0x01: return "ISA Bridge";
                case 0x02: return "EISA Bridge";
                case 0x03: return "MCA Bridge";
                case 0x04: return "PCI-PCI Bridge";
                case 0x05: return "PCMCIA Bridge";
                case 0x06: return "NuBus Bridge";
                case 0x07: return "CardBus Bridge";
                case 0x80: return "Other Bridge";
                default: return "Bridge Device";
            }
        case 0x07:  // Communication
            switch (subclass) {
                case 0x00: return "Serial Controller";
                case 0x01: return "Parallel Controller";
                case 0x03: return "Modem";
                default: return "Communication Controller";
            }
        case 0x08:  // System
            switch (subclass) {
                case 0x00: return "PIC";
                case 0x01: return "DMA Controller";
                case 0x02: return "Timer";
                case 0x03: return "RTC";
                case 0x80: return "System Peripheral";
                default: return "System Peripheral";
            }
        case 0x0C:  // Serial Bus
            switch (subclass) {
                case 0x00: return "FireWire Controller";
                case 0x03: return "USB Controller";
                case 0x05: return "SMBus Controller";
                case 0x07: return "IPMI";
                default: return "Serial Bus Controller";
            }
        case 0x0D:  // Wireless
            switch (subclass) {
                case 0x00: return "iRDA Controller";
                case 0x11: return "Bluetooth Controller";
                case 0x20: return "WiFi Controller";
                default: return "Wireless Controller";
            }
        default: return "Unknown Device";
    }
}

// Print all PCI devices
void pci_print_devices(void) {
    kprintf("\n[PCI] Device List:\n");
    kprintf("  Bus:Slot.Func  Vendor:Device  Class    Description\n");
    kprintf("  -------------  -------------  ------   -----------\n");

    for (int i = 0; i < pci_device_count; i++) {
        pci_device_t *dev = &pci_devices[i];
        kprintf("  %02x:%02x.%x        %04x:%04x      %02x.%02x    %s\n",
                dev->bus, dev->slot, dev->func,
                dev->vendor_id, dev->device_id,
                dev->class_code, dev->subclass,
                pci_class_name(dev->class_code, dev->subclass));
    }
}
