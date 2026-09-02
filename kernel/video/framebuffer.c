// framebuffer.c - Linear framebuffer driver with double buffering
#include "framebuffer.h"
#include "../string.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../mm/vmm.h"      // #642: PAT / write-combining front buffer
#include "../cpu/dlprof.h"    // #642: dp_tsc() - the shared rdtsc helper
#include "fs/bootlog.h"   // #742: the owning header, NOT a private extern
#include "../drivers/pci.h" // ASUS bring-up: positively identify a VIRTUAL display
                            // adapter before touching the Bochs DISPI I/O ports

// Framebuffer state
static uint32_t *fb_front = NULL;      // Front buffer (actual display)
static uint32_t *fb_back = NULL;       // Back buffer (drawing target)
static uint32_t *fb_addr = NULL;       // Current drawing target
static uint32_t fb_width = 0;          // LOGICAL width  (post-rotation)
static uint32_t fb_height = 0;         // LOGICAL height (post-rotation)
static uint32_t fb_pitch = 0;          // LOGICAL bytes per line (fb_back's row stride)
static uint32_t fb_bpp = 0;            // Bits per pixel
static int fb_is_bgr = 0;

// ===========================================================================
// #745 (local 102): display rotation (GPD MicroPC - the panel is physically
// mounted rotated, so its native GOP scanout is portrait; Linux users on the
// same hardware fix this with an fbcon/xrandr transform, and we need the
// software equivalent since firmware cannot change how the panel is glued in).
//
// DECISION: rotate at the PRESENT chokepoint, not per-primitive. fb_width/
// fb_height/fb_pitch above (and every fb_* draw call in this file) are the
// LOGICAL screen: the console, gfx_boot_splash(), and the compositor's mapped
// back buffer all draw into fb_back as an ordinary unrotated landscape screen
// and never know rotation exists. fb_phys_* below is the raw GOP layout that
// only fb_swap_buffers()/fb_swap_dirty_rects() (video/framebuffer.c) ever
// touch, rotating each row (or each damage rect - #379/b740) as it copies
// back->front. This was chosen over transforming every draw primitive because
// the compositor already funnels 100% of its output through exactly that one
// copy (confirmed: fb_put_pixel/fb_fill_rect/fb_draw_rect/fb_blit/etc. write
// ONLY fb_addr==fb_back; nothing in this tree writes fb_front outside these
// two functions), so rotating there is free of per-primitive overhead and
// naturally covers every caller, including ones this file's own author will
// never know about (DOOM's fullscreen path, the Win16 layer, TinyGL) instead
// of requiring each of them to opt in.
//
// MEASURED COST (see the block above fb_present_rotated_copy() below): a 90/
// 270 present is a cache-hostile transpose (each written pixel lands on a new
// destination cache line - there is no reuse to exploit without block tiling,
// which is not implemented here), unlike the straight-line memcpy the NONE/
// 180 paths keep. This is real, on-hardware-relevant cost with no GPU to hide
// it behind, which is why it stays C rather than moving to Rust: it is
// entangled with the existing cli/CR3-switch/damage-lock present chokepoint
// (sys_fb_flip() in gui/fb_syscall.c) that is already hand-tuned C with its
// own TSC-cycle instrumentation (#632/#642), and reusing dp_tsc() here rather
// than introducing an FFI boundary mid-cli-region keeps that entanglement in
// one language. rotation==NONE (every machine that is not this one) takes the
// UNCHANGED pre-#102 fb_swap_buffers()/fb_swap_dirty_rects() code path with
// byte-identical behaviour - see the early-out in both functions below.
static fb_rotation_t fb_rotation   = FB_ROTATE_NONE;
static uint32_t fb_phys_width      = 0;   // native GOP width (firmware-reported)
static uint32_t fb_phys_height     = 0;   // native GOP height
static uint32_t fb_phys_pitch      = 0;   // native GOP row stride (bytes), fb_front

fb_rotation_t fb_get_rotation(void)  { return fb_rotation; }
uint32_t fb_get_phys_width(void)     { return fb_phys_width; }
uint32_t fb_get_phys_height(void)    { return fb_phys_height; }

// Rotation-copy cycle counters, read by the [FLIPPROF]-style boot log line so
// the cost is a measured number, not an assertion (mirrors #632's g_flip_*
// counters one file over). Cumulative + max, cheap enough to keep unconditional.
volatile uint64_t g_fb_rot_copy_tot_cyc = 0;
volatile uint64_t g_fb_rot_copy_max_cyc = 0;
volatile uint64_t g_fb_rot_copy_calls   = 0;
volatile uint64_t g_fb_rot_copy_px_tot  = 0;   // total pixels copied, for cyc/px

// #halfres: integer PRESENT-SCALE compositing (see the block comment above
// fb_set_present_scale() in framebuffer.h). 1 == off, byte-identical to a
// kernel that never had this feature. >1 is an exact integer replication
// factor: fb_width/fb_height/fb_pitch are reduced to fb_phys_*/n and every
// draw primitive in this file (and everything upstream of it: the
// compositor, uiscale) is none the wiser, exactly like fb_rotation above.
static int fb_present_scale_n = 1;

// Present-scale replication copy cycle counters, mirroring g_fb_rot_copy_*
// one block up. src_px is what was actually COMPOSITED (the number this
// feature exists to shrink); dst_px is physical pixels WRITTEN, which is
// src_px * n * n and does not shrink - the present still touches the whole
// panel either way. Read by main.c's [SCALEPROF] boot log line.
volatile uint64_t g_fb_scale_copy_tot_cyc = 0;
volatile uint64_t g_fb_scale_copy_max_cyc = 0;
volatile uint64_t g_fb_scale_copy_calls   = 0;
volatile uint64_t g_fb_scale_copy_src_px_tot = 0;
volatile uint64_t g_fb_scale_copy_dst_px_tot = 0;

// Global variables for external access (fb_syscall.c)
uint64_t g_fb_phys_addr = 0;
uint32_t g_fb_width = 0;
uint32_t g_fb_height = 0;
uint32_t g_fb_pitch = 0;
uint32_t g_fb_bpp = 0;
static bool fb_double_buffered = false;

int fb_get_present_scale(void) { return fb_present_scale_n; }

// #halfres: apply an integer present-scale factor. Called once at boot, AFTER
// fb_init() and AFTER the root filesystem is mounted (kernel/gui/presentscale.c
// reads its config from ext2/FAT), and BEFORE uiscale_init() so uiscale sees
// the REDUCED logical dims (kernel/gui/desktop.c calls both, in that order).
// Not supported live: like display rotation, this is a reboot-to-apply
// setting, which keeps it out of the compositor's already-complex live-resize
// path (uiscale's uw_rescale_all) for a feature that is one owner's choice
// about one physical panel, not something anyone needs to preview and back
// out of in the same session.
//
// WHY THIS DOES NOT REALLOCATE fb_back, AND WHY THAT IS NOT MERELY THE
// SIMPLER OPTION. fb_back was allocated with kmalloc_aligned(size, 4096) in
// fb_init() below, and this kernel's heap has NO kfree_aligned(): kfree()
// assumes the header sits immediately before the returned pointer, which is
// true for a plain kmalloc() but not for the >16-byte-aligned path, which
// stores the real allocation elsewhere and returns an interior pointer.
// Calling kfree() on that interior pointer would corrupt the heap the first
// time this ever ran, on a code path with zero prior callers to have caught
// it (see blame.md's "zero callers" lesson). So this REPACKS the SAME
// allocation at a smaller pitch/height instead: fb_back was sized for the
// full PHYSICAL screen, and any n>=1 view of it needs less than that, never
// more, so shrinking fb_width/fb_height/fb_pitch in place can never run past
// the allocation's end. No realloc, no free, no new failure mode.
bool fb_set_present_scale(int n) {
    if (n <= 1) {
        fb_present_scale_n = 1;
        return true;
    }
    if (!fb_back || !fb_front || fb_phys_width == 0 || fb_phys_height == 0) {
        return false;
    }
    // Validation (exact divisor, logical floor, no-rotation) is the caller's
    // job (presentscale_valid_rs in rustkern/presentscale.rs) - this function
    // trusts it and only guards against dividing into nothing.
    uint32_t new_w = fb_phys_width  / (uint32_t)n;
    uint32_t new_h = fb_phys_height / (uint32_t)n;
    if (new_w == 0 || new_h == 0) {
        return false;
    }
    uint32_t new_pitch = new_w * 4;   // tightly packed, same reasoning fb_init
                                      // uses for the 90/270 rotated back buffer

    fb_width  = new_w;
    fb_height = new_h;
    fb_pitch  = new_pitch;
    g_fb_width  = fb_width;
    g_fb_height = fb_height;
    g_fb_pitch  = fb_pitch;

    // The old (physical-sized) content is meaningless at the new, smaller
    // pitch - zero it so nothing stale from before this call can show through
    // a torn early partial present.
    memset(fb_back, 0, (size_t)fb_height * fb_pitch);
    if (fb_addr == fb_back || fb_addr != fb_front) {
        fb_addr = fb_back;
    }

    fb_present_scale_n = n;
    kprintf("[FB] present-scale %dx applied: compositing %ux%u into a "
            "physical %ux%u panel (back buffer repacked, no realloc)\n",
            n, fb_width, fb_height, fb_phys_width, fb_phys_height);
    return true;
}

void fb_scale_profile_get(uint64_t *tot_cyc, uint64_t *max_cyc,
                           uint64_t *calls, uint64_t *src_px_tot,
                           uint64_t *dst_px_tot) {
    if (tot_cyc)    *tot_cyc    = g_fb_scale_copy_tot_cyc;
    if (max_cyc)    *max_cyc    = g_fb_scale_copy_max_cyc;
    if (calls)      *calls      = g_fb_scale_copy_calls;
    if (src_px_tot) *src_px_tot = g_fb_scale_copy_src_px_tot;
    if (dst_px_tot) *dst_px_tot = g_fb_scale_copy_dst_px_tot;
}
// Bochs VGA (stdvga) DISPI register definitions
#define VBE_DISPI_IOPORT_INDEX  0x01CE
#define VBE_DISPI_IOPORT_DATA   0x01CF
#define VBE_DISPI_INDEX_XRES    0x01
#define VBE_DISPI_INDEX_YRES    0x02
#define VBE_DISPI_INDEX_BPP     0x03
#define VBE_DISPI_INDEX_ENABLE  0x04
#define VBE_DISPI_INDEX_BANK    0x05
#define VBE_DISPI_INDEX_VIRT_WIDTH  0x06
#define VBE_DISPI_INDEX_VIRT_HEIGHT 0x07
#define VBE_DISPI_INDEX_X_OFFSET    0x08
#define VBE_DISPI_INDEX_Y_OFFSET    0x09
// ASUS bring-up: the DISPI interface IDENTIFIES ITSELF through index 0. Real
// Bochs/QEMU/VirtualBox report 0xB0C0..0xB0C5 (the interface revision); a
// machine that does not decode these ports at all floats the bus and reads
// 0xFFFF. This is the POSITIVE identification the gate below needs.
#define VBE_DISPI_INDEX_ID          0x00
#define VBE_DISPI_ID0               0xB0C0
#define VBE_DISPI_ID5               0xB0C5

// Display-class PCI vendors that are virtual adapters known to implement the
// Bochs DISPI register interface. This list is a WHITELIST, deliberately: any
// machine not on it never has a byte written to ports 0x1CE/0x1CF.
#define FB_VDPY_VENDOR_QEMU     0x1234  // QEMU / Bochs stdvga (1234:1111)
#define FB_VDPY_VENDOR_QXL      0x1B36  // Red Hat QXL
#define FB_VDPY_VENDOR_VIRTIO   0x1AF4  // virtio-vga / virtio-gpu
#define FB_VDPY_VENDOR_VBOX     0x80EE  // VirtualBox VBoxVGA

static uint16_t bochs_vbe_read(uint16_t index) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    return inw(VBE_DISPI_IOPORT_DATA);
}

static void bochs_vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_DISPI_IOPORT_INDEX, index);
    outw(VBE_DISPI_IOPORT_DATA, value);
}

// ASUS bring-up: is there a VIRTUAL display adapter on the PCI bus?
//
// fb_init() is reached from console_init(), which main.c runs BEFORE pci_init(),
// so the pci.c device table does not exist yet and pci_find_vendor_class() would
// answer "no" on every machine. pci_read8/16() are pure 0xCF8/0xCFC
// configuration cycles with no initialisation of their own, so this walks bus 0
// directly. Bus 0 is sufficient and deliberate: every emulator in the whitelist
// puts its display adapter on bus 0, and widening the walk would only add ways
// to mis-identify a real GPU behind a bridge as virtual.
//
// This is a POSITIVE test. "The read was not 0xFFFF" is NOT one, and is exactly
// the reasoning that made the old unconditional fixup dangerous.
static int fb_virtual_display_present(uint16_t *out_vendor, uint16_t *out_device) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        if (pci_read16(0, slot, 0, PCI_VENDOR_ID) == 0xFFFF) continue;
        uint8_t hdr = pci_read8(0, slot, 0, PCI_HEADER_TYPE);
        uint8_t nfunc = (hdr & 0x80) ? 8 : 1;
        for (uint8_t func = 0; func < nfunc; func++) {
            uint16_t vid = pci_read16(0, slot, func, PCI_VENDOR_ID);
            if (vid == 0xFFFF) continue;
            if (pci_read8(0, slot, func, PCI_CLASS) != PCI_CLASS_DISPLAY) continue;
            if (vid == FB_VDPY_VENDOR_QEMU || vid == FB_VDPY_VENDOR_QXL ||
                vid == FB_VDPY_VENDOR_VIRTIO || vid == FB_VDPY_VENDOR_VBOX) {
                if (out_vendor) *out_vendor = vid;
                if (out_device) *out_device = pci_read16(0, slot, func, PCI_DEVICE_ID);
                return 1;
            }
        }
    }
    return 0;
}

// Read and fix Bochs VGA display alignment after fb_init
static void fb_dump_vga_crtc(void);
static void fb_fix_bochs_alignment(void) {
    // ASUS bring-up GATE. This function used to run UNCONDITIONALLY from
    // fb_init(), on every machine, and it does raw I/O to the Bochs VBE DISPI
    // ports 0x1CE/0x1CF plus (via fb_dump_vga_crtc) the legacy VGA CRTC and
    // attribute-controller ports. On a machine that does not decode those
    // ports the reads float to 0xFFFF, `virt_w != fb_phys_width` is then
    // trivially true, and the code WRITES 0xFFFF-derived values back to I/O
    // ports it does not own. fb_dump_vga_crtc() is the same story and is NOT a
    // read-only dump despite its name: it writes the attribute controller
    // (0x3C0) and CRTC start-address (0x3D4/0x3D5) whenever its own floated
    // reads come back non-zero, which on unclaimed ports they always do.
    //
    // Two POSITIVE conditions, both required, before a single byte is written:
    //   1. a display-class PCI function from a known virtual-adapter vendor;
    //   2. the DISPI interface identifying itself through index 0 with a
    //      version in 0xB0C0..0xB0C5.
    // Order matters: (1) is pure PCI config space and is safe everywhere, so a
    // machine that fails it never has the index register at 0x1CE written at
    // all, not even for the probe.
    uint16_t vdpy_vendor = 0, vdpy_device = 0;
    if (!fb_virtual_display_present(&vdpy_vendor, &vdpy_device)) {
        kprintf("[FB] Bochs VBE: NOT detected (no virtual display adapter on PCI "
                "bus 0) - DISPI + VGA CRTC fixup SKIPPED, no I/O to 0x1CE/0x1CF\n");
        bootlog_write("[FB] Bochs VBE NOT detected (no virtual display adapter) "
                      "- DISPI/VGA-CRTC fixup SKIPPED");
        return;
    }

    uint16_t dispi_id = bochs_vbe_read(VBE_DISPI_INDEX_ID);
    if (dispi_id < VBE_DISPI_ID0 || dispi_id > VBE_DISPI_ID5) {
        kprintf("[FB] Bochs VBE: adapter %04x:%04x present but DISPI id 0x%x is "
                "out of range - fixup SKIPPED\n",
                vdpy_vendor, vdpy_device, dispi_id);
        bootlog_write("[FB] Bochs VBE NOT confirmed (adapter %04x:%04x, DISPI id "
                      "0x%x) - DISPI/VGA-CRTC fixup SKIPPED",
                      vdpy_vendor, vdpy_device, dispi_id);
        return;
    }

    kprintf("[FB] Bochs VBE detected (adapter %04x:%04x, DISPI id 0x%x) - "
            "alignment fixup WILL run\n", vdpy_vendor, vdpy_device, dispi_id);
    bootlog_write("[FB] Bochs VBE detected (adapter %04x:%04x, DISPI id 0x%x) "
                  "- DISPI/VGA-CRTC fixup RUNNING",
                  vdpy_vendor, vdpy_device, dispi_id);

    uint16_t xres = bochs_vbe_read(VBE_DISPI_INDEX_XRES);
    uint16_t yres = bochs_vbe_read(VBE_DISPI_INDEX_YRES);
    uint16_t bpp = bochs_vbe_read(VBE_DISPI_INDEX_BPP);
    uint16_t enable = bochs_vbe_read(VBE_DISPI_INDEX_ENABLE);
    uint16_t virt_w = bochs_vbe_read(VBE_DISPI_INDEX_VIRT_WIDTH);
    uint16_t virt_h = bochs_vbe_read(VBE_DISPI_INDEX_VIRT_HEIGHT);
    uint16_t x_off = bochs_vbe_read(VBE_DISPI_INDEX_X_OFFSET);
    uint16_t y_off = bochs_vbe_read(VBE_DISPI_INDEX_Y_OFFSET);

    kprintf("[FB] Bochs VBE: xres=%u yres=%u bpp=%u enable=0x%x\n",
            xres, yres, bpp, enable);
    kprintf("[FB] Bochs VBE: virt_w=%u virt_h=%u x_off=%u y_off=%u\n",
            virt_w, virt_h, x_off, y_off);

    // Fix: ensure virtual width matches actual width and offsets are zero.
    // #745 (local 102): this is a Bochs/QEMU-std-vga HARDWARE register - it
    // describes the real scanout memory layout, so it must be compared/set
    // against fb_phys_width (the true GOP width), never fb_width (which is
    // the LOGICAL, possibly-rotated width and would drive this register to
    // the wrong value on a 90/270 rotated boot, corrupting the actual display
    // this function exists to fix).
    int fixed = 0;
    if (virt_w != fb_phys_width) {
        kprintf("[FB] Fixing virt_width: %u -> %u\n", virt_w, (unsigned)fb_phys_width);
        bochs_vbe_write(VBE_DISPI_INDEX_VIRT_WIDTH, (uint16_t)fb_phys_width);
        fixed = 1;
    }
    if (x_off != 0) {
        kprintf("[FB] Fixing x_offset: %u -> 0\n", x_off);
        bochs_vbe_write(VBE_DISPI_INDEX_X_OFFSET, 0);
        fixed = 1;
    }
    if (y_off != 0) {
        kprintf("[FB] Fixing y_offset: %u -> 0\n", y_off);
        bochs_vbe_write(VBE_DISPI_INDEX_Y_OFFSET, 0);
        fixed = 1;
    }
    if (fixed) {
        kprintf("[FB] Bochs VBE display alignment corrected\n");
    }

    // Also check legacy VGA CRTC registers
    fb_dump_vga_crtc();
}

// ===========================================================================
// #642: give the FRONT buffer a real write-combining memory type.
//
// The back buffer is kmalloc'd RAM and must stay write-back: the compositor
// draws into it through sys_fb_map, read-modify-writes it constantly, and WC
// reads are uncached. It is the FRONT buffer, the GOP framebuffer BAR, that is
// the problem. Firmware maps that BAR UC in the MTRRs, so the present copy
// issues one uncached PCIe transaction per 16-byte movdqu store.
//
// MTRR is deliberately NOT touched, and that is a checked claim rather than an
// optimistic one. Intel SDM Vol 3A "Effective Page-Level Memory Types" (Table
// 12-7 in current editions) and AMD APM Vol 2 Table 7-9 both give: MTRR=UC with
// PAT=WB yields UC, which is the state we are in, but MTRR=UC with PAT=WC
// yields WC. WC in the PAT is the single type no MTRR type downgrades. Linux
// relies on exactly this: ioremap_wc() touches no MTRR, and arch_phys_wc_add()
// is documented as "a no-op on PAT enabled systems". So a PAT slot plus the
// right PTE bits is sufficient, and MTRR programming would add risk for nothing.
//
// MEASURED, NOT ASSERTED, AND WITH A CONTROL. fb_enable_write_combining() times
// three full presents: a cold one, a second one with NOTHING changed (the
// control), and one after the switch. The headline ratio is control-vs-after, so
// ordinary cache/TLB warm-up cannot be miscredited to the memory type; the
// cold-vs-control ratio is printed alongside precisely so the size of that
// artifact is visible rather than hidden.
//
// THIS CANNOT BE VALIDATED IN A VM, and the numbers a VM prints must not be
// quoted as evidence for the fix. QEMU's framebuffer is ordinary host RAM, and
// KVM forces WB for guest RAM in EPT regardless of the guest PAT, so the guest
// memory type is not honoured at all. A VM boot proves the PAT write lands, the
// PTEs carry the right bits, and nothing regresses. It cannot prove the win.
// The number that matters is the one this prints on the real iMac.
// ===========================================================================
// ---------------------------------------------------------------------------
// #COMPIDLE: HOW MANY PIXELS ACTUALLY REACH THE DISPLAY, AND HOW OFTEN.
//
// The owner's report is "the compositor uses 70% of the CPU at idle" on real
// hardware, and the recorded prior failure on this exact question was that a
// VM measured ~0% and the VM number was believed. A VM cannot settle it,
// because the front buffer there is ordinary host RAM. What CAN be settled
// from any machine, including his, is the WORKLOAD: how many presents per
// second, how many of them are whole-screen, and how many bytes per second
// are written across the (on real hardware) MMIO aperture. Those three
// numbers are hardware-independent statements about what the compositor
// ASKED for, and they are what turn "70%" into a diagnosis instead of an
// impression.
//
// Counted here rather than in sys_fb_flip because this is the only place that
// knows the real byte count: sys_fb_flip sees rectangles, this sees the
// memcpy lengths, and after clamping the two are not the same number.
//
// Cost is one add per copy, on a path that already does a multi-KB memcpy.
// ---------------------------------------------------------------------------
uint64_t g_fb_front_bytes   = 0;   // bytes written back->front, lifetime
uint64_t g_fb_full_presents = 0;   // whole-screen presents
uint64_t g_fb_part_presents = 0;   // damage-rect presents
uint64_t g_fb_wc_bytes      = 0;   // front-buffer bytes actually re-typed WC
uint64_t g_fb_wc_span       = 0;   // front-buffer bytes in total

static int      fb_wc_active     = 0;
uint64_t        g_fb_wc_pre_cyc  = 0;   // cycles for one full present, before
uint64_t        g_fb_wc_pre2_cyc = 0;   // CONTROL: a second present, still before
uint64_t        g_fb_wc_post_cyc = 0;   // cycles for one full present, after

int fb_wc_is_active(void) { return fb_wc_active; }

// Time one full back->front copy. At this point in fb_init the back buffer has
// just been zeroed and the front buffer has just been cleared, so this writes
// black over black: measurable, and invisible on screen. Interrupts are masked
// so a timer tick cannot land inside the sample.
static uint64_t fb_time_one_present(void) {
    // #745 (local 102): LOGICAL size, deliberately - fb_back is allocated to
    // exactly fb_height*fb_pitch bytes (see fb_init()). Using fb_phys_height*
    // fb_phys_pitch here instead would read past the end of fb_back on a
    // 90/270 rotated boot (logical byte count is always <= physical: proof in
    // fb_init()'s comment on fb_pitch). Both buffers are freshly zeroed at
    // this point in fb_init (see caller), so this straight-line copy is a
    // faithful "N bytes, one linear memcpy" timing sample either way; on a
    // rotated boot N is the logical (smaller-or-equal) byte count, which is a
    // legitimate lower bound on the real rotated present's cost, not the
    // exact number - the real present is a transpose, strictly more
    // expensive per byte than this straight memcpy measures.
    uint64_t sz = (uint64_t)fb_height * fb_pitch;
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0" : "=r"(rflags));
    __asm__ volatile("cli");
    uint64_t t0 = dp_tsc();
    memcpy(fb_front, fb_back, sz);
    __asm__ volatile("sfence" ::: "memory");
    uint64_t d = dp_tsc() - t0;
    if (rflags & (1ULL << 9)) __asm__ volatile("sti");
    return d;
}

static void fb_enable_write_combining(void) {
    if (!fb_front) return;

    uint64_t sz = (uint64_t)fb_height * fb_pitch;
    sz = (sz + 0xFFF) & ~0xFFFULL;
    // #745 (local 102): the PAT re-type below must cover the PHYSICAL front
    // buffer (the real GOP MMIO BAR) - fb_sz below, not the LOGICAL `sz`
    // above (which fb_time_one_present() needs unchanged; see its comment).
    // On rotation NONE/180 these are numerically identical, so this is a
    // no-op change for every machine that is not rotated.
    uint64_t fb_sz = (uint64_t)fb_phys_height * fb_phys_pitch;
    fb_sz = (fb_sz + 0xFFF) & ~0xFFFULL;

    // TWO "before" samples, not one. The first present of a boot is cold in
    // every sense (source not in cache, TLB unwarmed), so a naive
    // before-then-after pair credits ordinary warm-up to the memory type and
    // reports a speedup that would appear even if the PAT write did nothing.
    // pre2 is the control: it is measured under IDENTICAL conditions to pre,
    // with nothing changed in between. Any improvement from pre to pre2 is
    // warm-up. Only the pre2 -> post difference can be attributed to WC.
    if (fb_back) {
        g_fb_wc_pre_cyc  = fb_time_one_present();
        g_fb_wc_pre2_cyc = fb_time_one_present();
    }

    if (!vmm_pat_init()) {
        kprintf("[FB] #642: no PAT, front buffer keeps its firmware memory type\n");
        return;
    }

    extern uint64_t g_kernel_cr3;
    uint64_t cr3 = g_kernel_cr3 ? g_kernel_cr3 : (read_cr3() & VMM_ADDR_MASK);

    int64_t wc_bytes = vmm_set_memtype_range(cr3, (uint64_t)fb_front, fb_sz,
                                             VMM_PAT_IDX_WC);
    if (wc_bytes <= 0) {
        kprintf("[FB] #642: WC re-type covered nothing (%ld), front buffer unchanged\n",
                (long)wc_bytes);
        return;
    }
    fb_wc_active = 1;
    g_fb_wc_bytes = (uint64_t)wc_bytes;   // #COMPIDLE: reportable at runtime
    g_fb_wc_span  = fb_sz;
    // Coverage is NOT assumed to be 100%. vmm_set_memtype_range only re-types
    // whole 2MB/1GB granules, so a framebuffer whose size is not a multiple of
    // the granule keeps its tail at the firmware type. Report the real figure,
    // because a speedup measured over a partially-converted buffer is a
    // partially-converted speedup and saying otherwise would overstate it.
    uint64_t pct = (uint64_t)wc_bytes * 100 / fb_sz;

    if (fb_back) g_fb_wc_post_cyc = fb_time_one_present();

    // Report cycles per byte rather than MB/s: it needs no TSC calibration
    // (mono_tsc_khz_rs() is not necessarily up this early) and it is the metric
    // that actually characterises the copy. Scaled by 1000 for integer output.
    uint64_t pre2_mcpb = g_fb_wc_pre2_cyc ? (g_fb_wc_pre2_cyc * 1000) / sz : 0;
    uint64_t post_mcpb = g_fb_wc_post_cyc ? (g_fb_wc_post_cyc * 1000) / sz : 0;
    // The headline ratio is CONTROL vs after, never cold-first vs after.
    uint64_t ratio_x100  = g_fb_wc_post_cyc ? (g_fb_wc_pre2_cyc * 100) / g_fb_wc_post_cyc : 0;
    // How much of a naive pre-vs-post "speedup" was really just warm-up.
    uint64_t warm_x100   = g_fb_wc_pre2_cyc ? (g_fb_wc_pre_cyc  * 100) / g_fb_wc_pre2_cyc : 0;

    kprintf("[FB] #642 WC ACTIVE on 0x%lx..0x%lx: %lu of %lu KB re-typed (%lu%%)\n",
            (uint64_t)fb_front, (uint64_t)fb_front + fb_sz,
            (uint64_t)wc_bytes / 1024, fb_sz / 1024, pct);
    kprintf("[FB] #642 present: cold=%lu  CONTROL=%lu cyc (%lu.%03lu cyc/byte)  "
            "wc=%lu cyc (%lu.%03lu cyc/byte)\n",
            g_fb_wc_pre_cyc, g_fb_wc_pre2_cyc, pre2_mcpb / 1000, pre2_mcpb % 1000,
            g_fb_wc_post_cyc, post_mcpb / 1000, post_mcpb % 1000);
    kprintf("[FB] #642 speedup=%lu.%02lux (control vs wc); warmup alone was "
            "%lu.%02lux (cold vs control)\n",
            ratio_x100 / 100, ratio_x100 % 100, warm_x100 / 100, warm_x100 % 100);

    // Durable copy: this is the number the next real-hardware boot has to
    // report, and serial is silent in GUI mode.
    bootlog_write("[FB] #642 WC: base=0x%lx size=%lu covered=%lu (%lu%%) "
                  "cold=%lu control=%lu wc=%lu cyc speedup=%lu.%02lux warmup=%lu.%02lux",
                  (uint64_t)fb_front, fb_sz, (uint64_t)wc_bytes, pct,
                  g_fb_wc_pre_cyc, g_fb_wc_pre2_cyc, g_fb_wc_post_cyc,
                  ratio_x100 / 100, ratio_x100 % 100, warm_x100 / 100, warm_x100 % 100);
}

// Initialize framebuffer
void fb_init(framebuffer_info_t *info) {
    if (!info || info->address == 0) {
        kprintf("[FB] ERROR: Invalid framebuffer info\n");
        return;
    }

    fb_front = (uint32_t *)info->address;
    fb_phys_width = info->width;
    fb_phys_height = info->height;
    fb_phys_pitch = info->pitch;
    fb_bpp = info->bpp;
    fb_is_bgr = (info->pixel_format == PIXEL_FORMAT_BGR);

    // #745 (local 102): rotation is decided HERE, once, for the whole boot
    // session (reboot-to-apply - see docs/UI_STYLE_GUIDE.md-following hint in
    // the Settings Display panel). g_boot_info was filled by the UEFI
    // bootloader from \ROTATE.TXT before ExitBootServices (uefi/bootloader.c),
    // so it is already valid at this, the very first framebuffer call of boot -
    // gfx_boot_splash() and everything else draws rotated from its first pixel.
    // Any value other than the four real rotations (a NULL g_boot_info on a
    // boot path that never set one, a stale/corrupt marker) degrades to
    // FB_ROTATE_NONE, never to an out-of-range enum.
    extern boot_info_t *g_boot_info;
    fb_rotation = FB_ROTATE_NONE;
    if (g_boot_info && g_boot_info->display_rotation <= FB_ROTATE_270) {
        fb_rotation = (fb_rotation_t)g_boot_info->display_rotation;
    }
    bool fb_swapped_dims = (fb_rotation == FB_ROTATE_90 || fb_rotation == FB_ROTATE_270);

    fb_width  = fb_swapped_dims ? fb_phys_height : fb_phys_width;
    fb_height = fb_swapped_dims ? fb_phys_width  : fb_phys_height;
    // NONE/180 keep the exact physical pitch (dims did not swap, so this is
    // the pre-#102 value, unchanged). 90/270 need a back buffer whose rows are
    // as wide as the new LOGICAL width, which fb_phys_pitch cannot express (it
    // was sized for the OTHER axis), so it is tightly repacked. Every GOP mode
    // this kernel has ever booted is 32bpp (fb_bpp asserted at 32 in the
    // rotated-copy path below), so width*4 is exact, not an approximation.
    fb_pitch = fb_swapped_dims ? (fb_width * 4) : fb_phys_pitch;

    // Set global variables for external access - LOGICAL, same contract as
    // pre-#102 (sys_fb_info() and every other reader never knew there was a
    // physical layout to begin with, and still doesn't need to).
    g_fb_phys_addr = info->address;
    g_fb_width = fb_width;
    g_fb_height = fb_height;
    g_fb_pitch = fb_pitch;
    g_fb_bpp = fb_bpp;

    // Allocate back buffer for double buffering (LOGICAL size)
    uint32_t buffer_size = fb_height * fb_pitch;
    fb_back = (uint32_t *)kmalloc_aligned(buffer_size, 4096);  // page-align so sys_fb_map mapping matches exactly (#72)

    if (fb_back) {
        // Explicitly zero the back buffer to avoid garbage data
        memset(fb_back, 0, buffer_size);
        fb_double_buffered = true;
        fb_addr = fb_back;  // Draw to back buffer
        kprintf("[FB] Double buffering enabled (%u KB back buffer)\n", buffer_size / 1024);

        // Also clear the front buffer (hardware framebuffer) to remove UEFI
        // graphics. PHYSICAL size: for FB_ROTATE_90/270 this now differs from
        // the back buffer's (logical) size, and clearing the wrong number of
        // bytes here would either under-clear the real GOP buffer (leftover
        // firmware garbage at the tail) or overrun it.
        memset(fb_front, 0, (size_t)fb_phys_height * fb_phys_pitch);
    } else {
        fb_double_buffered = false;
        fb_addr = fb_front;  // Fall back to single buffer
        kprintf("[FB] Warning: Could not allocate back buffer, using single buffer\n");
    }

    kprintf("[FB] Framebuffer initialized: logical %ux%u pitch=%u  physical %ux%u pitch=%u  "
            "rotation=%u @ 0x%lx\n",
            fb_width, fb_height, fb_pitch, fb_phys_width, fb_phys_height, fb_phys_pitch,
            (unsigned)fb_rotation, (uint64_t)fb_front);

    // #642: re-type the front buffer as write-combining. Must run AFTER
    // fb_front/fb_back/fb_pitch are set (it sizes and times a real present) and
    // after pmm/vmm/heap are up (main.c orders console_init after all three).
    fb_enable_write_combining();

    // Fix Bochs VGA display alignment (prevents left-side content appearing on right)
    fb_fix_bochs_alignment();
}

// Getters
uint32_t fb_get_width(void) { return fb_width; }
uint32_t fb_get_height(void) { return fb_height; }
uint32_t fb_get_pitch(void) { return fb_pitch; }
uint32_t *fb_get_back_buffer(void) { return fb_back; }
uint32_t fb_get_bpp(void) { return fb_bpp; }

// Put a pixel at (x, y)
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_addr || x >= fb_width || y >= fb_height) return;

    // Calculate offset (pitch is in bytes, we're using 32-bit pointer)
    uint32_t *pixel = (uint32_t *)((uint8_t *)fb_addr + y * fb_pitch + x * 4);
    *pixel = color;
}

// Write a horizontal run of `count` pixels at (x,y) into the active buffer with
// a single bounds-clamp + memcpy, instead of `count` fb_put_pixel() calls. Used
// by row-oriented blitters (scaled image blit, etc.) for a big speedup.
void fb_put_row(uint32_t x, uint32_t y, uint32_t count, const uint32_t *pixels) {
    if (!fb_addr || !pixels || y >= fb_height || x >= fb_width) return;
    if (x + count > fb_width) count = fb_width - x;
    uint32_t *dst = (uint32_t *)((uint8_t *)fb_addr + y * fb_pitch + x * 4);
    memcpy(dst, pixels, (size_t)count * 4);
}

// Get pixel at (x, y)
uint32_t fb_get_pixel(uint32_t x, uint32_t y) {
    if (!fb_addr || x >= fb_width || y >= fb_height) return 0;

    uint32_t *pixel = (uint32_t *)((uint8_t *)fb_addr + y * fb_pitch + x * 4);
    return *pixel;
}

// Clear entire screen
void fb_clear(uint32_t color) {
    if (!fb_addr) return;

    for (uint32_t y = 0; y < fb_height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)fb_addr + y * fb_pitch);
        for (uint32_t x = 0; x < fb_width; x++) {
            row[x] = color;
        }
    }
}

// Fill a rectangle
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_addr) return;

    // Handle negative coordinates (signed int32_t cast to uint32_t)
    int32_t sx = (int32_t)x, sy = (int32_t)y;
    int32_t sw = (int32_t)w, sh = (int32_t)h;
    if (sx < 0) { sw += sx; sx = 0; }
    if (sy < 0) { sh += sy; sy = 0; }
    if (sw <= 0 || sh <= 0) return;
    x = (uint32_t)sx; y = (uint32_t)sy;
    w = (uint32_t)sw; h = (uint32_t)sh;

    // Clip to screen
    if (x >= fb_width || y >= fb_height) return;
    if (x + w > fb_width) w = fb_width - x;
    if (y + h > fb_height) h = fb_height - y;

    for (uint32_t row = y; row < y + h; row++) {
        uint32_t *pixel = (uint32_t *)((uint8_t *)fb_addr + row * fb_pitch + x * 4);
        for (uint32_t col = 0; col < w; col++) {
            pixel[col] = color;
        }
    }
}

// Draw rectangle outline
void fb_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_addr || w == 0 || h == 0) return;

    // Use signed coords to handle negative positions
    int32_t sx = (int32_t)x, sy = (int32_t)y;
    int32_t sw = (int32_t)w, sh = (int32_t)h;

    int32_t x_start = sx < 0 ? 0 : sx;
    int32_t x_end = sx + sw;
    if (x_end > (int32_t)fb_width) x_end = (int32_t)fb_width;

    // Top and bottom lines
    for (int32_t i = x_start; i < x_end; i++) {
        if (sy >= 0 && sy < (int32_t)fb_height)
            fb_put_pixel(i, sy, color);
        int32_t bottom = sy + sh - 1;
        if (bottom >= 0 && bottom < (int32_t)fb_height)
            fb_put_pixel(i, bottom, color);
    }

    // Left and right lines
    int32_t y_start = sy < 0 ? 0 : sy;
    int32_t y_end = sy + sh;
    if (y_end > (int32_t)fb_height) y_end = (int32_t)fb_height;

    for (int32_t i = y_start; i < y_end; i++) {
        if (sx >= 0 && sx < (int32_t)fb_width)
            fb_put_pixel(sx, i, color);
        int32_t right = sx + sw - 1;
        if (right >= 0 && right < (int32_t)fb_width)
            fb_put_pixel(right, i, color);
    }
}

// Draw line using Bresenham's algorithm
void fb_draw_line(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color) {
    if (!fb_addr) return;

    int32_t dx = x2 > x1 ? x2 - x1 : x1 - x2;
    int32_t dy = y2 > y1 ? y2 - y1 : y1 - y2;
    int32_t sx = x1 < x2 ? 1 : -1;
    int32_t sy = y1 < y2 ? 1 : -1;
    int32_t err = dx - dy;

    while (1) {
        if (x1 >= 0 && x1 < (int32_t)fb_width &&
            y1 >= 0 && y1 < (int32_t)fb_height) {
            fb_put_pixel(x1, y1, color);
        }

        if (x1 == x2 && y1 == y2) break;

        int32_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x1 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y1 += sy;
        }
    }
}

// Scroll screen up by n lines (in pixels)
void fb_scroll(uint32_t lines, uint32_t bg_color) {
    if (!fb_addr || lines == 0 || lines >= fb_height) {
        fb_clear(bg_color);
        return;
    }

    // Move screen content up
    uint32_t bytes_to_copy = (fb_height - lines) * fb_pitch;
    memmove(fb_addr, (uint8_t *)fb_addr + lines * fb_pitch, bytes_to_copy);

    // Clear the bottom area
    fb_fill_rect(0, fb_height - lines, fb_width, lines, bg_color);
}

// Blit raw pixel data
void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *data) {
    if (!fb_addr || !data) return;

    // Handle negative coordinates (signed int32_t cast to uint32_t)
    int32_t sx = (int32_t)x, sy = (int32_t)y;
    int32_t sw = (int32_t)w, sh = (int32_t)h;
    int32_t src_x = 0, src_y = 0;

    if (sx < 0) { src_x = -sx; sw += sx; sx = 0; }
    if (sy < 0) { src_y = -sy; sh += sy; sy = 0; }
    if (sw <= 0 || sh <= 0) return;

    for (int32_t row = 0; row < sh && sy + row < (int32_t)fb_height; row++) {
        for (int32_t col = 0; col < sw && sx + col < (int32_t)fb_width; col++) {
            fb_put_pixel(sx + col, sy + row, data[(src_y + row) * (int32_t)w + (src_x + col)]);
        }
    }
}

// Swap back buffer to front buffer (present frame)
// ===========================================================================
// #745 (local 102): rotated present helpers. Entered ONLY when fb_rotation !=
// FB_ROTATE_NONE - fb_swap_buffers()/fb_swap_dirty_rects() below keep their
// exact pre-#102 memcpy code path otherwise, so an unrotated machine's present
// is byte-identical to before this ticket (proof: grep for FB_ROTATE_NONE in
// both functions - the old lines are reached with zero new code in between).
//
// A logical rect rotated by a multiple of 90 degrees DOES land on a single
// axis-aligned rectangle in physical space (rotating a rectangle by 90/180/
// 270 never produces a non-rectangular result) - so this is not "rotate the
// bounding box, then memcpy the block". For 90/270 the ACCESS PATTERN into
// that physical rectangle is a transpose: one source row (contiguous reads)
// fills one destination COLUMN (fb_phys_pitch-strided writes), which is why
// this is a per-pixel copy rather than a row-oriented memcpy, and why it is
// measurably slower than the straight-line memcpy the NONE/180 paths keep -
// see g_fb_rot_copy_* below, read by the [FLIPPROF]-style boot log line.
// ===========================================================================

// Copy one rectangle from the LOGICAL back buffer into the PHYSICAL front
// buffer, applying fb_rotation. lx,ly,lw,lh are already clamped to the
// logical screen by both call sites before this is reached.
static void fb_present_rect_rotated(int32_t lx, int32_t ly, int32_t lw, int32_t lh) {
    if (lw <= 0 || lh <= 0) return;

    uint64_t t0 = dp_tsc();

    if (fb_rotation == FB_ROTATE_180) {
        // Same dimensions as the logical screen: logical (x,y) -> physical
        // (phys_w-1-x, phys_h-1-y). Source read is row-contiguous; the
        // destination row is the mirror row, filled back-to-front.
        for (int32_t row = 0; row < lh; row++) {
            const uint32_t *src = (const uint32_t *)((const uint8_t *)fb_back +
                                    (uint32_t)(ly + row) * fb_pitch) + lx;
            uint32_t py = fb_phys_height - 1 - (uint32_t)(ly + row);
            uint32_t *dstrow = (uint32_t *)((uint8_t *)fb_front + (uint32_t)py * fb_phys_pitch);
            for (int32_t col = 0; col < lw; col++) {
                dstrow[fb_phys_width - 1 - (uint32_t)(lx + col)] = src[col];
            }
        }
    } else {
        // FB_ROTATE_90 / FB_ROTATE_270: transpose. fb_width/fb_height are the
        // LOGICAL (already-swapped-by-fb_init) dims; fb_height == the
        // physical width and fb_width == the physical height (fb_init()
        // swapped them for exactly this reason). Derivation: rotating a WxH
        // image 90 deg clockwise produces an HxW image where
        // dst(H-1-y, x) = src(x, y); 270 (90 CCW) is dst(y, W-1-x) = src(x, y).
        int rot90 = (fb_rotation == FB_ROTATE_90);
        for (int32_t row = 0; row < lh; row++) {
            int32_t srcy = ly + row;
            const uint32_t *src = (const uint32_t *)((const uint8_t *)fb_back +
                                    (uint32_t)srcy * fb_pitch) + lx;
            for (int32_t col = 0; col < lw; col++) {
                int32_t srcx = lx + col;
                uint32_t px, py;
                if (rot90) {
                    px = fb_height - 1 - (uint32_t)srcy;   // fb_height == phys width
                    py = (uint32_t)srcx;
                } else {
                    px = (uint32_t)srcy;
                    py = fb_width - 1 - (uint32_t)srcx;    // fb_width == phys height
                }
                *(uint32_t *)((uint8_t *)fb_front + py * fb_phys_pitch + px * 4) = src[col];
            }
        }
    }

    uint64_t d = dp_tsc() - t0;
    g_fb_rot_copy_tot_cyc += d;
    g_fb_rot_copy_calls++;
    g_fb_rot_copy_px_tot  += (uint64_t)lw * (uint64_t)lh;
    g_fb_front_bytes      += (uint64_t)lw * (uint64_t)lh * 4ULL;   // #COMPIDLE
    if (d > g_fb_rot_copy_max_cyc) g_fb_rot_copy_max_cyc = d;
}

// Full-screen rotated present: the first frame, a whole-screen damage rect,
// and any caller that passes full_redraw all fall back to this, exactly like
// the pre-#102 full memcpy() they replace.
static void fb_present_full_rotated(void) {
    fb_present_rect_rotated(0, 0, (int32_t)fb_width, (int32_t)fb_height);
}

// Read by main.c's boot-log heartbeat, alongside [FLIPPROF] (#632) - the same
// "measured, not asserted" posture. Cheap (a handful of divides) and called
// at heartbeat cadence, not per-frame.
void fb_rotate_profile_get(uint64_t *tot_cyc, uint64_t *max_cyc,
                            uint64_t *calls, uint64_t *px_tot) {
    if (tot_cyc) *tot_cyc = g_fb_rot_copy_tot_cyc;
    if (max_cyc) *max_cyc = g_fb_rot_copy_max_cyc;
    if (calls)   *calls   = g_fb_rot_copy_calls;
    if (px_tot)  *px_tot  = g_fb_rot_copy_px_tot;
}

// ===========================================================================
// #halfres: integer PRESENT-SCALE replication. Entered ONLY when
// fb_present_scale_n > 1 - fb_swap_buffers()/fb_swap_dirty_rects() keep their
// exact pre-existing code path otherwise (grep both for fb_present_scale_n:
// the old lines are reached with zero new code in between, same proof #745's
// rotation feature offers above).
//
// UNLIKE the 90/270 rotation transpose, this is NOT cache-hostile: it is
// EXACTLY the "build one widened row once, then memcpy it N times" trick
// dosexec.c's dos_row_reuse()/dos_xscale_t already use for the DOS presenters
// (see kernel/dos/dosexec.c's "ONE scaling/blit path" block comment) and the
// same principle this file's OWN fb_present_rect_rotated() uses for its
// FB_ROTATE_180 case (row-contiguous reads, row-oriented writes). Nothing
// here is a new algorithm; it is the smallest instance of a pattern this
// codebase already leans on twice, generalised to an arbitrary integer N
// instead of hardcoding N=2, because "reject a non-integer factor" (the
// whole point: no resampling) does not mean N is always 2 on every panel.
//
// C, NOT RUST, and that is a stated choice, not a default: this function
// shares a translation unit and a hot invariant (interrupts already off,
// the temporary kernel CR3 already live - see sys_fb_flip() in
// gui/fb_syscall.c) with fb_present_rect_rotated() immediately above, which
// was already accepted as C in THIS FILE for exactly that entanglement.
// Splitting the replication loop into Rust while leaving fb_swap_buffers()/
// fb_swap_dirty_rects() in C would add an FFI hop inside an already
// hand-tuned cli region for no measured benefit. The DECISION logic (is a
// requested factor valid for this panel) has none of those constraints and
// lives in rustkern/presentscale.rs instead, exactly mirroring the uiscale.c
// (C plumbing) / uiscale.rs (Rust arithmetic) split one file over.
//
// MEASURED (see the [SCALEPROF] boot-log line in main.c, and the CHANGELOG
// entry for a real 2x capture): building the widened row costs lw stores;
// replicating it costs n memcpy()s of lw*n*4 bytes each. Total bytes WRITTEN
// to the front buffer are the same as a straight full-resolution present
// (the panel has the pixels it has), which is exactly why this ticket's
// mandate is "do not claim a 4x speedup" - the saving is in what gets READ
// and RECOMPUTED (compositing), not in the unavoidable physical write.
// ===========================================================================

// Widest single destination (physical) row this supports, in pixels. 4096
// covers every panel this kernel has ever booted with headroom; a caller
// asking for a wider one is refused rather than overrunning this buffer.
#define FB_SCALE_SCRATCH_MAXW 4096
static uint32_t fb_scale_scratch[FB_SCALE_SCRATCH_MAXW];

// Copy one rectangle from the LOGICAL back buffer into the PHYSICAL front
// buffer, replicating each source pixel into an nxn block. lx,ly,lw,lh are
// already clamped to the logical screen by both call sites before this is
// reached (same contract fb_present_rect_rotated documents above).
static void fb_present_rect_scaled(int32_t lx, int32_t ly, int32_t lw, int32_t lh) {
    if (lw <= 0 || lh <= 0) return;
    int n = fb_present_scale_n;
    if (n <= 1) return;

    int32_t dst_w = lw * n;
    if (dst_w > FB_SCALE_SCRATCH_MAXW) {
        // Should not happen for any mode this kernel supports (checked at
        // config-apply time by presentscale_valid_rs); refuse rather than
        // overrun the scratch row.
        return;
    }

    uint64_t t0 = dp_tsc();

    for (int32_t row = 0; row < lh; row++) {
        const uint32_t *src = (const uint32_t *)((const uint8_t *)fb_back +
                                (uint32_t)(ly + row) * fb_pitch) + lx;

        // Build the widened row ONCE: each source pixel replicated n times.
        uint32_t *srow = fb_scale_scratch;
        for (int32_t col = 0; col < lw; col++) {
            uint32_t px = src[col];
            for (int k = 0; k < n; k++) *srow++ = px;
        }

        // n IDENTICAL destination rows, each a straight memcpy of the row
        // just built - the row-reuse trick, applied vertically instead of
        // detecting a repeat (there is no need to detect one: at an integer
        // replication factor every one of the n rows is a repeat, always).
        uint32_t dst_x0 = (uint32_t)lx * (uint32_t)n;
        uint32_t dst_y0 = (uint32_t)(ly + row) * (uint32_t)n;
        for (int k = 0; k < n; k++) {
            uint32_t *dstrow = (uint32_t *)((uint8_t *)fb_front +
                                 (dst_y0 + (uint32_t)k) * fb_phys_pitch) + dst_x0;
            memcpy(dstrow, fb_scale_scratch, (size_t)dst_w * 4);
        }
    }

    uint64_t d = dp_tsc() - t0;
    uint64_t src_px = (uint64_t)lw * (uint64_t)lh;
    uint64_t dst_px = (uint64_t)dst_w * (uint64_t)lh * (uint64_t)n;
    g_fb_scale_copy_tot_cyc     += d;
    g_fb_scale_copy_calls++;
    g_fb_scale_copy_src_px_tot  += src_px;
    g_fb_scale_copy_dst_px_tot  += dst_px;
    g_fb_front_bytes            += dst_px * 4ULL;   // #COMPIDLE
    if (d > g_fb_scale_copy_max_cyc) g_fb_scale_copy_max_cyc = d;
}

// Full-screen scaled present: the first frame, a whole-screen damage rect,
// and any caller that passes full_redraw all fall back to this.
static void fb_present_full_scaled(void) {
    fb_present_rect_scaled(0, 0, (int32_t)fb_width, (int32_t)fb_height);
}

void fb_swap_buffers(void) {
    if (!fb_double_buffered || !fb_back || !fb_front) {
        return;  // Nothing to swap in single buffer mode
    }

    // #745 (local 102): rotated present. Everything below this block is the
    // UNCHANGED pre-#102 code, reached exactly as before when fb_rotation is
    // FB_ROTATE_NONE (every machine that is not this one).
    if (fb_rotation != FB_ROTATE_NONE) {
        fb_present_full_rotated();
        g_fb_full_presents++;                 // #COMPIDLE (bytes counted in
        __asm__ volatile("sfence" ::: "memory");   // fb_present_rect_rotated)
        return;
    }

    // #halfres: integer present-scale replication. Mutually exclusive with
    // rotation (checked at config-apply time, not here) - reached only when
    // fb_present_scale_n > 1, which is only ever true on a machine that
    // opted in. Every other machine takes the unchanged memcpy below.
    if (fb_present_scale_n > 1) {
        fb_present_full_scaled();
        g_fb_full_presents++;                 // #COMPIDLE (bytes counted in
        __asm__ volatile("sfence" ::: "memory");   // fb_present_rect_scaled)
        return;
    }

    // Copy back buffer to front buffer
    uint32_t buffer_size = fb_height * fb_pitch;
    memcpy(fb_front, fb_back, buffer_size);
    g_fb_front_bytes += buffer_size;      // #COMPIDLE
    g_fb_full_presents++;
    // #642: WC stores are weakly ordered and sit in the write-combining buffers
    // until a line fills or a fence drains them. Without this the last partial
    // line of a present can still be in a buffer when the caller assumes the
    // frame is on screen, which shows up as an intermittent torn/partial frame
    // and reads exactly like a compositor bug. Cheap, and a no-op on WB.
    __asm__ volatile("sfence" ::: "memory");
}

// Swap only dirty rectangles (optimized partial update)
void fb_swap_dirty_rects(const void *dirty_rects, uint32_t count, bool full_redraw) {
    if (!fb_double_buffered || !fb_back || !fb_front) {
        return;
    }
    
    // If full redraw requested or no dirty rects, do full swap
    if (full_redraw || count == 0 || dirty_rects == NULL) {
        if (fb_rotation != FB_ROTATE_NONE) {          // #745 (local 102)
            fb_present_full_rotated();
            g_fb_full_presents++;                     // #COMPIDLE
            __asm__ volatile("sfence" ::: "memory");
            return;
        }
        if (fb_present_scale_n > 1) {                 // #halfres
            fb_present_full_scaled();
            g_fb_full_presents++;                     // #COMPIDLE
            __asm__ volatile("sfence" ::: "memory");
            return;
        }
        uint32_t buffer_size = fb_height * fb_pitch;
        memcpy(fb_front, fb_back, buffer_size);
        g_fb_front_bytes += buffer_size;      // #COMPIDLE
        g_fb_full_presents++;
        __asm__ volatile("sfence" ::: "memory");   // #642, see fb_swap_buffers
        return;
    }
    
    g_fb_part_presents++;   // #COMPIDLE: one damage-rect present (N rects)

    // Cast to rect structure (x, y, width, height as int32_t)
    typedef struct { int32_t x, y, width, height; } rect_t;
    const rect_t *rects = (const rect_t *)dirty_rects;
    
    // Copy only the dirty rectangles
    for (uint32_t i = 0; i < count; i++) {
        int32_t x = rects[i].x;
        int32_t y = rects[i].y;
        int32_t w = rects[i].width;
        int32_t h = rects[i].height;
        
        // Clamp to screen bounds
        if (x < 0) { w += x; x = 0; }
        if (y < 0) { h += y; y = 0; }
        if (x + w > (int32_t)fb_width) w = fb_width - x;
        if (y + h > (int32_t)fb_height) h = fb_height - y;
        
        // Skip invalid rectangles
        if (w <= 0 || h <= 0) continue;

        // #745 (local 102): the damage rect is in LOGICAL space - the
        // compositor that reported it never learns rotation exists (see the
        // block comment above fb_rotation) - so it is rotated pixel-by-pixel
        // alongside the copy here rather than as a separate "rotate the rect,
        // then memcpy" step. See the block comment above
        // fb_present_rect_rotated() for why a straight memcpy is not
        // available even though the rotated rect IS a physical rectangle.
        if (fb_rotation != FB_ROTATE_NONE) {
            fb_present_rect_rotated(x, y, w, h);
            continue;
        }
        // #halfres: same per-rect replication for a partial present as the
        // full-screen path above uses.
        if (fb_present_scale_n > 1) {
            fb_present_rect_scaled(x, y, w, h);
            continue;
        }

        // Copy each row of the dirty rectangle
        uint32_t bytes_per_row = w * 4;  // 4 bytes per pixel (BGRA)
        for (int32_t row = 0; row < h; row++) {
            uint32_t offset = ((y + row) * fb_pitch) + (x * 4);
            memcpy((uint8_t*)fb_front + offset, (uint8_t*)fb_back + offset, bytes_per_row);
        }
        g_fb_front_bytes += (uint64_t)bytes_per_row * (uint64_t)h;   // #COMPIDLE
    }
    // #642: drain the write-combining buffers once for the whole partial
    // present. This path matters MORE than the full-copy one: a damage rect is
    // usually a few hundred bytes per row, so most rows END mid-line and leave a
    // partially-filled WC buffer behind. See fb_swap_buffers.
    __asm__ volatile("sfence" ::: "memory");
}

// Set direct mode - draw directly to front buffer (for boot splash)
void fb_set_direct_mode(bool direct) {
    // Note: Direct mode was found to not work reliably. Use fb_swap_buffers() instead.
    // This function is kept for backward compatibility but does nothing useful.
    if (direct) {
        fb_addr = fb_front;
    } else {
        if (fb_double_buffered && fb_back) {
            fb_addr = fb_back;
        }
    }
}

// ============================================================================
// Alpha Blending Operations
// ============================================================================

// Blend a pixel with the background using alpha (0-255, 0=transparent, 255=opaque)
void fb_blend_pixel(uint32_t x, uint32_t y, uint32_t color, uint8_t alpha) {
    if ((int32_t)x < 0 || (int32_t)y < 0) return;
    if (x >= fb_width || y >= fb_height) return;
    if (alpha == 255) {
        fb_put_pixel(x, y, color);
        return;
    }
    if (alpha == 0) return;  // Fully transparent
    
    // Get existing pixel
    uint32_t bg = fb_get_pixel(x, y);
    
    // Extract RGB components
    uint32_t bg_r = (bg >> 16) & 0xFF;
    uint32_t bg_g = (bg >> 8) & 0xFF;
    uint32_t bg_b = bg & 0xFF;
    
    uint32_t fg_r = (color >> 16) & 0xFF;
    uint32_t fg_g = (color >> 8) & 0xFF;
    uint32_t fg_b = color & 0xFF;
    
    // Blend: result = bg * (255-alpha)/255 + fg * alpha/255
    uint32_t inv_alpha = 255 - alpha;
    uint32_t r = (bg_r * inv_alpha + fg_r * alpha) / 255;
    uint32_t g = (bg_g * inv_alpha + fg_g * alpha) / 255;
    uint32_t b = (bg_b * inv_alpha + fg_b * alpha) / 255;
    
    fb_put_pixel(x, y, (r << 16) | (g << 8) | b);
}

// Fill a rectangle with alpha blending
void fb_fill_rect_alpha(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color, uint8_t alpha) {
    if (alpha == 255) {
        fb_fill_rect(x, y, w, h, color);
        return;
    }
    if (alpha == 0) return;  // Fully transparent
    
    // Handle negative coordinates (signed int32_t cast to uint32_t)
    {
        int32_t sx = (int32_t)x, sy = (int32_t)y;
        int32_t sw = (int32_t)w, sh = (int32_t)h;
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sw <= 0 || sh <= 0) return;
        x = (uint32_t)sx; y = (uint32_t)sy;
        w = (uint32_t)sw; h = (uint32_t)sh;
    }

    // Clip to framebuffer bounds
    if (x >= fb_width || y >= fb_height) return;
    if (x + w > fb_width) w = fb_width - x;
    if (y + h > fb_height) h = fb_height - y;
    
    // Extract foreground RGB
    uint32_t fg_r = (color >> 16) & 0xFF;
    uint32_t fg_g = (color >> 8) & 0xFF;
    uint32_t fg_b = color & 0xFF;
    uint32_t inv_alpha = 255 - alpha;
    
    // Pre-multiply foreground
    fg_r = fg_r * alpha;
    fg_g = fg_g * alpha;
    fg_b = fg_b * alpha;
    
    uint32_t *fb = fb_back;  // Use back buffer
    for (uint32_t dy = 0; dy < h; dy++) {
        uint32_t *row = fb + (y + dy) * (fb_pitch / 4) + x;
        for (uint32_t dx = 0; dx < w; dx++) {
            uint32_t bg = row[dx];
            uint32_t bg_r = (bg >> 16) & 0xFF;
            uint32_t bg_g = (bg >> 8) & 0xFF;
            uint32_t bg_b = bg & 0xFF;
            
            uint32_t r = (bg_r * inv_alpha + fg_r) / 255;
            uint32_t g = (bg_g * inv_alpha + fg_g) / 255;
            uint32_t b = (bg_b * inv_alpha + fg_b) / 255;
            
            row[dx] = (r << 16) | (g << 8) | b;
        }
    }
}


// Debug: read and log VGA CRTC registers that affect display width.
// NOT read-only despite the name: it WRITES the attribute controller (0x3C0)
// and the CRTC start-address registers (0x3D4/0x3D5) when its reads come back
// non-zero, which on a machine that does not decode these legacy ports they
// always do (floating bus -> 0xFF). Its only caller is fb_fix_bochs_alignment()
// and it is reached ONLY after that function's positive Bochs identification,
// so it is gated by construction; do not call it from anywhere else without
// repeating that gate.
static void fb_dump_vga_crtc(void) {
    // Read CRTC Offset register (index 0x13) - logical line width
    outb(0x3D4, 0x13);
    uint8_t crtc_offset = inb(0x3D5);

    // Read CRTC Start Address High (0x0C) and Low (0x0D)
    outb(0x3D4, 0x0C);
    uint8_t start_hi = inb(0x3D5);
    outb(0x3D4, 0x0D);
    uint8_t start_lo = inb(0x3D5);
    uint16_t start_addr = (uint16_t)((start_hi << 8) | start_lo);

    // Read Attribute Controller Horizontal Pixel Panning (index 0x13)
    // Must reset flip-flop first by reading 0x3DA
    inb(0x3DA);
    outb(0x3C0, 0x13 | 0x20);  // Index 0x13, keep palette on
    uint8_t pixel_pan = inb(0x3C1);

    // Read CRTC H Total (0x00), H Display End (0x01)
    outb(0x3D4, 0x00);
    uint8_t h_total = inb(0x3D5);
    outb(0x3D4, 0x01);
    uint8_t h_disp_end = inb(0x3D5);

    // Read CRTC H Blank Start (0x02)
    outb(0x3D4, 0x02);
    uint8_t h_blank_start = inb(0x3D5);

    kprintf("[FB] VGA CRTC: offset=0x%02x start_addr=0x%04x pixel_pan=%u\n",
            crtc_offset, start_addr, pixel_pan);
    kprintf("[FB] VGA CRTC: h_total=%u h_disp_end=%u h_blank_start=%u\n",
            h_total, h_disp_end, h_blank_start);

    // If pixel panning is non-zero, zero it out
    if (pixel_pan != 0) {
        kprintf("[FB] Fixing pixel panning: %u -> 0\n", pixel_pan);
        inb(0x3DA);
        outb(0x3C0, 0x13 | 0x20);
        outb(0x3C0, 0x00);
    }

    // If start address is non-zero, zero it out
    if (start_addr != 0) {
        kprintf("[FB] Fixing start address: 0x%04x -> 0\n", start_addr);
        outb(0x3D4, 0x0C);
        outb(0x3D5, 0x00);
        outb(0x3D4, 0x0D);
        outb(0x3D5, 0x00);
    }
}
