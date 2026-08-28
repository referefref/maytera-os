// uiscale.c - boot bring-up, persistence and live application of THE global UI
// scale factor. The factor, its arithmetic and its property self-test live in
// rustkern/uiscale.rs; read that header first. This file is the plumbing:
// where the default comes from, where the override is stored, and what has to
// happen when the value changes on a running machine.

#include "uiscale.h"
#include "themes.h"
#include "window.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../mm/heap.h"
#include "../drivers/acpi.h"

extern fat_fs_t g_fat_fs;
extern uint32_t amlhid_count_eisaid_rs(const uint8_t *base, uint32_t len,
                                       uint8_t a, uint8_t b, uint8_t c, uint8_t d);
extern uint32_t amlhid_selftest_rs(void);
// Implemented in proc/syscall.c, which owns the user-window table. Resizes
// every scale-transparent window so its LOGICAL size is unchanged, and tells
// each app to repaint. Returns how many windows it touched.
extern int uw_rescale_all(int32_t old_pct, int32_t new_pct);
// The SAME force-repaint a THEME change uses (#704). A scale change has the
// identical requirement - every open window's cached geometry is now stale and
// only a repaint renews it - so it reuses that path rather than inventing a
// second one.
extern int64_t sys_wm_force_redraw_all(void);

#define UISCALE_CFG_PATH "/CONFIG/DISPLAY.CFG"

// ---------------------------------------------------------------------------
// THE RECOVERY PATH, AND WHY IT IS ON THE FAT BOOT PARTITION.
//
// This whole feature exists because a machine came back with a UI too small to
// read. On such a machine "change it in Settings" is useless advice: the
// setting is inside the thing that cannot be operated. So there has to be a
// way to set the scale WITHOUT a working GUI, and it has to work from ANOTHER
// computer.
//
// That decides the partition. /CONFIG lives on the ext2 root, which Windows and
// macOS cannot mount, so a user with a stick and a laptop cannot edit it. The
// FAT ESP mounts everywhere. This follows the shape the tree already uses for
// exactly this kind of out-of-band switch (/TESTINPUT.TXT, /NOSMEP.TXT): a file
// at the root of the boot partition, whose whole content is the value.
//
// IT WINS OVER EVERYTHING, INCLUDING THE SAVED SETTING, and that is the point:
// it is the escape hatch, and an escape hatch that a stale saved preference can
// override is not one. Settings SAYS SO when the file is present rather than
// silently losing the user's choice at the next boot - a control whose effect
// quietly evaporates is worse than one that explains why it is pinned.
//
// WHICH SPELLING ACTUALLY REACHES THE FAT PARTITION - MEASURED, NOT ASSUMED.
//
// This took two wrong answers, and both were wrong for reasons that read as
// correct:
//
//  1. "/UISCALE.TXT" at the root, following /TESTINPUT.TXT and /NOSMEP.TXT.
//     The modeset agent pointed out that fs/fat.c fat_path_on_ext2() keeps ONLY
//     "/boot" and "/EFI" on the FAT ESP and redirects everything else to the
//     ext2 root - which would send this to the one partition a Windows or macOS
//     machine cannot mount, i.e. useless as a recovery path.
//  2. So it moved to "/boot/UISCALE.TXT", matching the video-mode override.
//     MEASURED on a booted VM with the file demonstrably present on the ESP at
//     /boot/UISCALE.TXT, 144 bytes: the kernel reported esp=-1. The lowercase
//     "/boot" spelling keeps fat_path_on_ext2() from redirecting, and then goes
//     STRAIGHT to fat_exists_inner() with no ext2 attempt and no fallback - and
//     that FAT walk does not match the uppercase 8.3 directory name on disk.
//
// What is actually true, from reading fat_exists() rather than reasoning about
// it: a path that fat_path_on_ext2() sends to ext2 is tried on ext2 and then
// FALLS BACK to the FAT ESP. So a root-level path DOES reach the ESP - which is
// why /TESTINPUT.TXT has always worked - and an uppercase "/BOOT/..." reaches
// it the same way. Only the lowercase "/boot/..." spelling, the one that looks
// most deliberate, has no fallback at all.
//
// So all three are tried, cheapest first, and the reason is written down here
// rather than left as a single spelling nobody dares change. A recovery path
// that works only if the user guesses the same spelling the author did is not
// a recovery path.
// ---------------------------------------------------------------------------
static const char *const g_uiscale_esp_paths[] = {
    "/UISCALE.TXT",         // ESP root: where every other out-of-band switch in
                            // this OS lives, and the easiest thing to tell a
                            // user over the phone
    "/BOOT/UISCALE.TXT",    // beside \\boot\\MODE.TXT, uppercase so the ext2
                            // miss falls back to the FAT ESP
    "/boot/UISCALE.TXT",    // the lowercase spelling, tried last: see above
};
#define UISCALE_ESP_PATH_COUNT \
    (int)(sizeof(g_uiscale_esp_paths) / sizeof(g_uiscale_esp_paths[0]))

static int32_t g_auto_pct = 100;   // what auto-detect said, for Settings to show
static int32_t g_laptop   = -1;    // 1 yes, 0 no, -1 could not ask
static int32_t g_max_pct  = 100;   // the cap this framebuffer imposes

int32_t uiscale_auto_pct(void)   { return g_auto_pct; }
int32_t uiscale_max_pct(void)    { return g_max_pct; }
int32_t uiscale_is_laptop(void)  { return g_laptop; }

const char *uiscale_src_name(void) {
    switch (uiscale_src_rs()) {
        case UI_SRC_AUTO:   return "auto";
        case UI_SRC_CONFIG: return "config";
        case UI_SRC_USER:   return "user";
        case UI_SRC_ESP:    return "pinned by a boot-disk override file";
        default:            return "default";
    }
}

// ---------------------------------------------------------------------------
// IS THIS A LAPTOP?
//
// The question auto-detection actually wants is PHYSICAL PANEL SIZE, and this
// kernel cannot read it: EDID would need either the UEFI EDID protocol (the
// bootloader is not built by build-golden.sh, so that is a change to a
// different artefact) or GMBUS (drivers/intel_gpu.c, whose own audit header
// says its register offsets are wrong and its probe loop does not terminate).
// See rustkern/uiscale.rs for the full statement of that limit.
//
// A battery is the one usable proxy that is a MEASUREMENT rather than a guess.
// A machine with a battery is a laptop; a laptop running 1920x1080 or better
// has a 13"-17" panel, i.e. 130-170 PPI, against the ~100 PPI the whole UI was
// drawn for. A machine WITHOUT a battery at 1920x1080 is a desktop or a VM at
// about 100 PPI, which is exactly what the current sizes already suit.
//
// THREE-VALUED, AND THE THIRD VALUE MATTERS. Returns 1 (battery declared),
// 0 (none declared) or -1 (could not ask: no validated DSDT, or the scanner's
// own positive control failed). -1 is NOT folded into 0 here; the caller is
// what decides, and it decides "behave as not-a-laptop", so a measurement that
// did not happen can never make the UI bigger. An unknown silently promoted to
// a yes is how a heuristic becomes a bug report.
// ---------------------------------------------------------------------------
static int32_t uiscale_probe_laptop(void) {
    if (amlhid_selftest_rs() != 0) {
        kprintf("[UISCALE] ACPI scanner self-test FAILED; the laptop "
                            "probe is not trustworthy and is not used.\n");
        return -1;
    }

    uint64_t base = 0; uint32_t len = 0;
    if (!acpi_get_dsdt(&base, &len)) {
        kprintf("[UISCALE] no validated DSDT: cannot ask whether this "
                            "machine has a battery.\n");
        return -1;
    }

    uint32_t bat = 0, lid = 0, ac = 0, pci = 0;
    // PNP0C0A control-method battery, PNP0C0D lid, PNP0A03 PCI root bus.
    // ACPI0003 (AC adapter) is a plain ASCII _HID, not an EISAID, so it is not
    // counted here; the battery is the decisive one and one signal that is
    // right beats three that need weighing.
    bat += amlhid_count_eisaid_rs((const uint8_t *)(uintptr_t)base, len, 0x41, 0xD0, 0x0C, 0x0A);
    lid += amlhid_count_eisaid_rs((const uint8_t *)(uintptr_t)base, len, 0x41, 0xD0, 0x0C, 0x0D);
    pci += amlhid_count_eisaid_rs((const uint8_t *)(uintptr_t)base, len, 0x41, 0xD0, 0x0A, 0x03);
    (void)ac;
    // Real firmware splits device declarations across several SSDTs, and
    // acpi_find_table() returns only the first match, so they are enumerated.
    // Bounded at 32 for the same reason drivers/mouse.c bounds it there.
    for (int i = 0; i < 32; i++) {
        uint64_t sb = 0; uint32_t sl = 0;
        if (!acpi_get_ssdt(i, &sb, &sl)) break;
        bat += amlhid_count_eisaid_rs((const uint8_t *)(uintptr_t)sb, sl, 0x41, 0xD0, 0x0C, 0x0A);
        lid += amlhid_count_eisaid_rs((const uint8_t *)(uintptr_t)sb, sl, 0x41, 0xD0, 0x0C, 0x0D);
        pci += amlhid_count_eisaid_rs((const uint8_t *)(uintptr_t)sb, sl, 0x41, 0xD0, 0x0A, 0x03);
    }

    // POSITIVE CONTROL. Every x86 firmware declares a PCI root bus. Zero of
    // them means the SCAN is broken, not that the machine is strange, and a
    // broken scan must report "could not ask" rather than "no battery" - the
    // two look identical downstream and only one of them is true.
    if (pci == 0) {
        kprintf("[UISCALE] ACPI scan POSITIVE CONTROL FAILED (no PNP0A03 "
                            "anywhere): the battery result is meaningless, ignoring it.\n");
        return -1;
    }

    kprintf("[UISCALE] ACPI chassis probe: battery(PNP0C0A)=%u lid(PNP0C0D)=%u "
            "pci-root(PNP0A03)=%u -> %s\n", bat, lid, pci,
            bat ? "LAPTOP" : "not a laptop");
    return bat ? 1 : 0;
}

// ---------------------------------------------------------------------------
// /CONFIG/DISPLAY.CFG. One key today: `scale=<percent>`.
//
// WHY A MACHINE-WIDE FILE ON THE EXT2 ROOT AND NOT A PER-USER SETTING. The
// first-run wizard has to be READABLE BEFORE THERE IS A USER, which is the
// exact surface the owner reported as unreadable. A per-user preference cannot
// help the screen on which the first user is created. Per-user scale can be
// layered on later; machine-wide has to exist first or the reported bug is not
// actually fixed.
// ---------------------------------------------------------------------------
// Parse `scale=<n>` lines, or a bare number on a line of its own. Both forms
// are accepted from both files: the ESP recovery file is meant to be editable
// by someone in a hurry on an unfamiliar computer, and refusing "200" because
// it lacked a "scale=" prefix would be pedantry at exactly the wrong moment.
static int32_t uiscale_parse(const char *buf, uint32_t sz) {
    int32_t got = -1;
    for (uint32_t i = 0; i < sz; ) {
        uint32_t j = i;
        while (j < sz && buf[j] != '\n' && buf[j] != '\r') j++;
        if (j > i && buf[i] != '#' && buf[i] != ';') {
            uint32_t k = i;
            if (j - i > 6 && strncmp(&buf[i], "scale=", 6) == 0) k = i + 6;
            while (k < j && (buf[k] == ' ' || buf[k] == '\t')) k++;
            int32_t v = 0, n = 0;
            while (k < j && buf[k] >= '0' && buf[k] <= '9' && n < 4) {
                v = v * 10 + (buf[k] - '0'); k++; n++;
            }
            if (n > 0) got = v;
        }
        i = j;
        while (i < sz && (buf[i] == '\n' || buf[i] == '\r')) i++;
    }
    return got;
}

// The FAT-ESP recovery file. fat_read_file() routes ext2-root-first then the
// ESP, and nothing puts a /UISCALE.TXT on the ext2 root, so this resolves to
// the boot partition exactly the way /TESTINPUT.TXT does.
static int32_t uiscale_read_esp(void) {
    for (int i = 0; i < UISCALE_ESP_PATH_COUNT; i++) {
        const char *path = g_uiscale_esp_paths[i];
        if (fat_exists(&g_fat_fs, path) != 1) continue;
        uint32_t sz = 0;
        char *buf = (char *)fat_read_file(&g_fat_fs, path, &sz);
        if (!buf) continue;
        int32_t got = uiscale_parse(buf, sz);
        kfree(buf);
        if (got > 0) {
            kprintf("[UISCALE] override file %s says %d%%\n", path, got);
            return got;
        }
        kprintf("[UISCALE] override file %s exists but holds no usable number; "
                "put a bare percent in it, e.g. 200\n", path);
    }
    return -1;
}

static int32_t uiscale_read_cfg(void) {
    if (fat_exists(&g_fat_fs, "/CONFIG") != 1) return -1;   // storage not up yet
    if (fat_exists(&g_fat_fs, UISCALE_CFG_PATH) != 1) return -1;

    uint32_t sz = 0;
    char *buf = (char *)fat_read_file(&g_fat_fs, UISCALE_CFG_PATH, &sz);
    if (!buf) return -1;

    int32_t got = uiscale_parse(buf, sz);
    kfree(buf);
    return got;
}

int uiscale_save(void) {
    char out[64];
    int32_t pct = uiscale_pct_rs();
    int n = 0;
    const char *hdr = "# MayteraOS display settings. scale is a PERCENT: 100 = 1x.\nscale=";
    for (const char *p = hdr; *p && n < (int)sizeof(out) - 8; p++) out[n++] = *p;
    if (pct >= 100) { out[n++] = (char)('0' + (pct / 100) % 10); }
    out[n++] = (char)('0' + (pct / 10) % 10);
    out[n++] = (char)('0' + pct % 10);
    out[n++] = '\n';
    out[n] = 0;
    return fat_write_file(&g_fat_fs, UISCALE_CFG_PATH, out, (uint32_t)n);
}

// ---------------------------------------------------------------------------
// Apply a new factor to a RUNNING machine.
//
// The point of making this work live rather than at next boot is that the
// owner has to be able to try 125, 150 and 175 and LOOK at them. A scale
// setting that needs a reboot to evaluate is a setting nobody tunes.
//
// Three things have to happen, in this order:
//   1. adopt the value, so every subsequent theme metric read scales;
//   2. resize every scale-transparent user window, so an app that was handed a
//      640x480 LOGICAL canvas still has one - its own layout maths is
//      unchanged and it does not need to know anything happened;
//   3. force a repaint, reusing the exact path a THEME change already uses
//      (SYS_WM_FORCE_REDRAW_ALL / wm_force_redraw_all), because a scale change
//      and a theme change have the same requirement: every open window's
//      cached geometry is now stale and only a repaint can renew it.
// ---------------------------------------------------------------------------
int32_t uiscale_apply(int32_t pct, int32_t src) {
    int32_t old = uiscale_pct_rs();
    int32_t adopted = uiscale_set_pct_rs(pct, src);
    if (adopted == old) return adopted;

    int touched = uw_rescale_all(old, adopted);
    (void)sys_wm_force_redraw_all();
    wm_invalidate_all();

    kprintf("[UISCALE] scale %d%% -> %d%% (%s), %d window(s) rescaled\n",
            old, adopted, uiscale_src_name(), touched);
    return adopted;
}

void uiscale_init(int fb_w, int fb_h) {
    uint32_t st = uiscale_selftest_rs();
    if (st != 0) {
        // A scale factor whose arithmetic is wrong does not merely look odd: a
        // control drawn from one rounding and hit-tested against another is a
        // DEAD control. If the properties do not hold, do not scale at all.
        kprintf("[UISCALE] SELF-TEST FAILED mask=0x%x - UI scaling DISABLED "
                "(staying at 100%%). See rustkern/uiscale.rs for what each bit means.\n", st);
        bootlog_write("[UISCALE] self-test FAILED: scaling disabled\n");
        (void)uiscale_set_pct_rs(100, UI_SRC_DEFAULT);
        return;
    }
    kprintf("[UISCALE] self-test PASS (identity at 1x, no hairline lost, "
                        "adjacent boxes share an edge, physical->logical is the exact "
                        "inverse, monotonic, logical floor holds)\n");

    g_max_pct = uiscale_max_pct_rs(fb_w, fb_h);
    g_laptop  = uiscale_probe_laptop();
    g_auto_pct = uiscale_auto_pct_rs(fb_w, fb_h, g_laptop);

    int32_t esp = uiscale_read_esp();
    int32_t cfg = uiscale_read_cfg();
    int32_t want, src;
    if (esp > 0) {
        want = esp; src = UI_SRC_ESP;
        kprintf("[UISCALE] an override file on the boot partition pins the scale "
                "at %d%% (this OVERRIDES the saved setting; delete the file to "
                "choose in Settings again)\n", esp);
    } else if (cfg > 0) {
        want = cfg; src = UI_SRC_CONFIG;
    } else {
        want = g_auto_pct; src = UI_SRC_AUTO;
    }

    // THE CAP WINS OVER THE FILE. A user who hand-edits scale=300 on a
    // 1366x768 panel has asked for a logical screen of 455x256, in which the
    // Settings window alone does not fit. Obeying that leaves them with an
    // unusable machine and no way back, so the cap is enforced on every path
    // and the reason is said out loud rather than silently ignored.
    if (want > g_max_pct) {
        kprintf("[UISCALE] requested %d%% exceeds what a %dx%d display can carry "
                "(logical screen must stay at least 1024x600); using %d%%\n",
                want, fb_w, fb_h, g_max_pct);
        want = g_max_pct;
    }

    int32_t adopted = uiscale_set_pct_rs(want, src);
    kprintf("[UISCALE] fb %dx%d, laptop=%s, auto=%d%%, max=%d%%, cfg=%d, esp=%d "
            "-> ACTIVE %d%% (%s)\n",
            fb_w, fb_h,
            g_laptop == 1 ? "yes" : (g_laptop == 0 ? "no" : "unknown"),
            g_auto_pct, g_max_pct, cfg, esp, adopted, uiscale_src_name());
}
