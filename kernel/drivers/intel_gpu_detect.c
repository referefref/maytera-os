// intel_gpu_detect.c - Intel integrated GPU DETECTION ONLY.
//
// ===========================================================================
// READ THIS BEFORE ADDING ANYTHING TO THIS FILE
// ---------------------------------------------------------------------------
// This file identifies the Intel GPU and prints what it found. That is ALL it
// does, and that is deliberate. It performs NO MMIO access, maps no BAR, and
// writes no display register. It cannot change what is on screen, because it
// never touches anything that decides what is on screen.
//
// That restraint is the whole point. The system's display today is the UEFI
// GOP linear framebuffer handed over in boot_info (physical == virtual,
// identity mapped); the userland compositor composites into a kernel back
// buffer and SYS_FB_FLIP memcpys dirty rows to it. Nothing here interposes on
// that path. If this code is wrong, the worst outcome is a wrong log line.
//
// The tempting next step is to call intel_gpu_init() from drivers/intel_gpu.c,
// which looks like a ready-made driver. DO NOT. See the audit block at the top
// of that file: as written it does not terminate on a machine whose first
// display device is not Intel (i.e. every QEMU VM), and its PCH-relative
// register offsets are wrong for every generation it claims to support.
//
// ===========================================================================
// WHY THE WALK IS C AND THE LOGIC IS RUST
// ---------------------------------------------------------------------------
// New kernel code must be Rust unless there is a stated reason. The reason
// here is a boundary one, not a performance one: the PCI walk is an EXISTING
// shared primitive (pci_find_vendor_class(), already in pci.c and already
// doing exactly this job), and reusing it beats reimplementing it. Consuming
// it from Rust would mean mirroring the C `pci_device_t` struct behind a
// repr(C) + sizeof lock for a struct this change does not own and that other
// subsystems keep extending, which is more risk than the thirty lines of glue
// it would replace. So the walk stays on the existing C primitive and every
// line of NEW logic (classification, naming, the invariants) is Rust in
// rustkern/intelgpu.rs. Nothing here is a re-implementation of anything.

#include "pci.h"
#include "../serial.h"
#include "../gui/syslog.h"

// rustkern/intelgpu.rs. Pure functions, no hardware access.
extern const char *intelgpu_ident_rs(uint16_t device_id, uint32_t *out_gen);
extern int32_t     intelgpu_selftest_rs(uint32_t *out_checks);

#define INTEL_VENDOR_ID   0x8086
#define PCI_CLASS_DISPLAY_CTRL 0x03
#define PCI_SUBCLASS_VGA_COMPAT 0x00

// Human-readable generation names, indexed by the GEN_* codes in
// rustkern/intelgpu.rs. Kept next to nothing else so the two cannot drift far.
static const char *intel_gen_label(uint32_t gen) {
    switch (gen) {
        case 1:  return "Gen5 Ironlake";
        case 2:  return "Gen6 Sandy Bridge";
        case 3:  return "Gen7 Ivy Bridge";
        case 4:  return "Gen7.5 Haswell";
        case 5:  return "Gen8 Broadwell";
        case 6:  return "Gen9 Skylake";
        case 7:  return "Gen9.5 Kaby/Coffee Lake";
        case 8:  return "Gen11 Ice Lake";
        case 9:  return "Gen12 Tiger Lake";
        default: return "unknown generation";
    }
}

// Detect and report the Intel integrated GPU, if any. Safe to call on any
// machine: on a VM with a non-Intel display adapter it says so and returns.
// Called once from main.c immediately after pci_init().
void intel_gpu_detect(void) {
    // The classifier's own invariants, re-proven on this exact build before
    // its answer is trusted. On a VM this is the ONLY part of the Intel GPU
    // work that can be exercised at all, because QEMU never presents an Intel
    // iGPU: it proves the TABLE knows the owner's iMac14,4 part (8086:0a26),
    // NOT that any hardware responded. Those are different claims.
    uint32_t checks = 0;
    int32_t fails = intelgpu_selftest_rs(&checks);
    kprintf("[INTELGPU] classifier self-test: %u checks, %d failures%s\n",
            checks, (int)fails, fails == 0 ? " (PASS)" : " (FAIL)");
    if (fails != 0) {
        syslog_log(LOG_WARNING, "Intel GPU classifier self-test FAILED");
    }

    pci_device_t *dev = pci_find_vendor_class(INTEL_VENDOR_ID,
                                              PCI_CLASS_DISPLAY_CTRL,
                                              PCI_SUBCLASS_VGA_COMPAT);
    if (!dev) {
        // Say what IS there, so the log distinguishes "no Intel GPU" from
        // "no display device found at all". On a QEMU VM this is the expected
        // path and it is not an error.
        pci_device_t *other = pci_find_class(PCI_CLASS_DISPLAY_CTRL,
                                             PCI_SUBCLASS_VGA_COMPAT);
        if (other) {
            kprintf("[INTELGPU] no Intel GPU; display adapter is %04x:%04x "
                    "at %02x:%02x.%x (using UEFI GOP framebuffer)\n",
                    other->vendor_id, other->device_id,
                    other->bus, other->slot, other->func);
        } else {
            kprintf("[INTELGPU] no VGA-class display device on the PCI bus "
                    "(using UEFI GOP framebuffer)\n");
        }
        return;
    }

    uint32_t gen = 0;
    const char *name = intelgpu_ident_rs(dev->device_id, &gen);

    kprintf("[INTELGPU] %04x:%04x at %02x:%02x.%x rev %02x\n",
            dev->vendor_id, dev->device_id,
            dev->bus, dev->slot, dev->func, dev->revision);
    kprintf("[INTELGPU]   %s (%s)\n", name, intel_gen_label(gen));

    // BAR0 = GTTMMADR (MMIO + GTT window), BAR2 = GMADR (the aperture). Both
    // are 64-bit BARs on Gen6+, so each consumes the following BAR slot too;
    // pci_get_bar_address() already folds in the high dword. These are READ
    // and PRINTED only. Nothing maps them.
    uint64_t bar0 = pci_get_bar_address(dev, 0);
    uint32_t bar0_size = pci_get_bar_size(dev, 0);
    uint64_t bar2 = pci_get_bar_address(dev, 2);
    uint32_t bar2_size = pci_get_bar_size(dev, 2);

    kprintf("[INTELGPU]   GTTMMADR (BAR0) 0x%llx size %u KB\n",
            (unsigned long long)bar0, bar0_size / 1024);
    kprintf("[INTELGPU]   GMADR    (BAR2) 0x%llx size %u MB\n",
            (unsigned long long)bar2, bar2_size / (1024 * 1024));

    // A 64-bit BAR placed above the identity-mapped range is the #125 class of
    // problem (an xHCI BAR above 512GB was absent from every address space).
    // Flag it rather than assume, because the answer differs per firmware.
    if (bar0 >= 0x100000000ULL) {
        kprintf("[INTELGPU]   NOTE: BAR0 is above 4GB; verify it is mapped "
                "before any MMIO access (cf. #125)\n");
    }

    // Deliberately NOT calling pci_mark_claimed(): nothing here brings the
    // device up. Per #418 a device is claimed on successful bring-up, not on
    // an id match, and identifying a GPU is not bringing it up. Claiming it
    // would tell every other subsystem a driver owns this device when none
    // does.

    syslog_log(LOG_INFO, "Intel GPU detected (identification only)");
}
