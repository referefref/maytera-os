// presentscale.c - boot bring-up and config plumbing for integer PRESENT-
// SCALE compositing (#halfres). See presentscale.h for the full rationale;
// the arithmetic and its self-test live in rustkern/presentscale.rs; the
// per-pixel replication lives in video/framebuffer.c. This file mirrors
// uiscale.c on purpose (same config file, same ESP-override dance, same
// ordering constraints) rather than inventing a second shape for "where does
// a display setting come from at boot".

#include "presentscale.h"
#include "../types.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../mm/heap.h"
#include "../video/framebuffer.h"
#include "../drivers/mouse.h"

extern fat_fs_t g_fat_fs;

// Pure decision logic (rustkern/presentscale.rs). No FS I/O, no locking - see
// that file's header for why it is Rust while this plumbing stays C.
extern int32_t presentscale_valid_rs(int32_t phys_w, int32_t phys_h,
                                      int32_t n, int32_t rotation_active);
extern uint32_t presentscale_selftest_rs(void);

#define PRESENTSCALE_CFG_PATH "/CONFIG/DISPLAY.CFG"

// Same three-spelling dance uiscale.c uses for /UISCALE.TXT, and for the
// exact same reason: fat_path_on_ext2() redirects everything except /boot and
// /EFI to the ext2 root and then FALLS BACK to the FAT ESP for a path it does
// not recognise, so a root-level or uppercase /BOOT path reaches the ESP
// (proven there), while the single lowercase "/boot/..." spelling that looks
// most deliberate has no fallback at all. Reusing the proven-correct set here
// rather than rediscovering the same two wrong turns.
static const char *const g_ps_esp_paths[] = {
    "/PRESENTSCALE.TXT",
    "/BOOT/PRESENTSCALE.TXT",
    "/boot/PRESENTSCALE.TXT",
};
#define PS_ESP_PATH_COUNT \
    (int)(sizeof(g_ps_esp_paths) / sizeof(g_ps_esp_paths[0]))

static int32_t     g_active_n = 1;
static const char *g_src      = "default (off)";

int32_t presentscale_active_n(void)   { return g_active_n; }
const char *presentscale_src_name(void) { return g_src; }

// Parse "present_scale=<n>" lines only. DISPLAY.CFG is a SHARED file (uiscale
// owns "scale="); anything else on a line, including uiscale's own key, must
// be ignored rather than misread as a number for this feature.
static int32_t ps_parse_cfg(const char *buf, uint32_t sz) {
    int32_t got = -1;
    for (uint32_t i = 0; i < sz; ) {
        uint32_t j = i;
        while (j < sz && buf[j] != '\n' && buf[j] != '\r') j++;
        if (j > i && buf[i] != '#' && buf[i] != ';') {
            if (j - i > 14 && strncmp(&buf[i], "present_scale=", 14) == 0) {
                uint32_t k = i + 14;
                while (k < j && (buf[k] == ' ' || buf[k] == '\t')) k++;
                int32_t v = 0, n = 0;
                while (k < j && buf[k] >= '0' && buf[k] <= '9' && n < 2) {
                    v = v * 10 + (buf[k] - '0'); k++; n++;
                }
                if (n > 0) got = v;
            }
        }
        i = j;
        while (i < sz && (buf[i] == '\n' || buf[i] == '\r')) i++;
    }
    return got;
}

static int32_t ps_read_cfg(void) {
    if (fat_exists(&g_fat_fs, "/CONFIG") != 1) return -1;
    if (fat_exists(&g_fat_fs, PRESENTSCALE_CFG_PATH) != 1) return -1;
    uint32_t sz = 0;
    char *buf = (char *)fat_read_file(&g_fat_fs, PRESENTSCALE_CFG_PATH, &sz);
    if (!buf) return -1;
    int32_t got = ps_parse_cfg(buf, sz);
    kfree(buf);
    return got;
}

// The ESP override file's whole content is a bare number, exactly like
// /UISCALE.TXT: someone editing it from Windows/macOS should not also have
// to remember a key name.
static int32_t ps_read_esp(void) {
    for (int i = 0; i < PS_ESP_PATH_COUNT; i++) {
        const char *path = g_ps_esp_paths[i];
        if (fat_exists(&g_fat_fs, path) != 1) continue;
        uint32_t sz = 0;
        char *buf = (char *)fat_read_file(&g_fat_fs, path, &sz);
        if (!buf) continue;
        int32_t v = 0, n = 0;
        uint32_t k = 0;
        while (k < sz && (buf[k] == ' ' || buf[k] == '\t')) k++;
        while (k < sz && buf[k] >= '0' && buf[k] <= '9' && n < 2) {
            v = v * 10 + (buf[k] - '0'); k++; n++;
        }
        kfree(buf);
        if (n > 0) {
            kprintf("[PRESENTSCALE] override file %s says %dx\n", path, v);
            return v;
        }
        kprintf("[PRESENTSCALE] override file %s exists but holds no usable "
                "number; put a bare integer in it, e.g. 2\n", path);
    }
    return -1;
}

void presentscale_init(void) {
    uint32_t st = presentscale_selftest_rs();
    if (st != 0) {
        // Same posture as uiscale_init(): a validator whose own arithmetic is
        // wrong must not be trusted to say a factor is safe. Stay off.
        kprintf("[PRESENTSCALE] SELF-TEST FAILED mask=0x%x - present-scale "
                "compositing DISABLED (staying at 1x). See "
                "rustkern/presentscale.rs for what each bit means.\n", st);
        bootlog_write("[PRESENTSCALE] self-test FAILED: disabled");
        g_active_n = 1; g_src = "self-test failed";
        return;
    }

    int32_t esp = ps_read_esp();
    int32_t cfg = ps_read_cfg();
    int32_t want = 1;
    const char *src = "default (off)";
    if (esp > 0)      { want = esp; src = "pinned by a boot-disk override file"; }
    else if (cfg > 0) { want = cfg; src = "config"; }
    else if (esp == 0 || cfg == 0) { src = "config (explicitly off)"; }

    if (want <= 1) {
        g_active_n = 1;
        g_src = src;
        // Only worth a line when something actually said "0"/"1" - a machine
        // that never touched this feature at all should stay silent, exactly
        // like fb_get_rotation() == NONE stays silent in [ROTPROF].
        if (esp == 0 || esp == 1 || cfg == 0 || cfg == 1) {
            kprintf("[PRESENTSCALE] present-scale compositing OFF (%s)\n", src);
        }
        return;
    }

    uint32_t pw = fb_get_phys_width();
    uint32_t ph = fb_get_phys_height();
    int32_t rotation_active = (fb_get_rotation() != FB_ROTATE_NONE) ? 1 : 0;

    if (!presentscale_valid_rs((int32_t)pw, (int32_t)ph, want, rotation_active)) {
        const char *why = rotation_active
            ? "display rotation is active and has not been proven to compose "
              "with this"
            : "the physical panel is not an exact multiple of the requested "
              "factor, or the resulting logical screen would be too small "
              "(no resampling - see docs)";
        kprintf("[PRESENTSCALE] refusing %dx on a %ux%u panel: %s. "
                "Staying at 1x.\n", want, pw, ph, why);
        bootlog_write("[PRESENTSCALE] refused %dx on %ux%u: %s",
                      want, pw, ph, why);
        g_active_n = 1; g_src = "refused (see boot log)";
        return;
    }

    if (!fb_set_present_scale(want)) {
        kprintf("[PRESENTSCALE] %dx requested and validated, but could not be "
                "applied; staying at 1x\n", want);
        g_active_n = 1; g_src = "refused (apply failed)";
        return;
    }

    g_active_n = want;
    g_src = src;

    // #wizmouse (2026-08-28): mouse_init() (kernel/main.c, well before
    // desktop_init()/presentscale_init() here - the root filesystem this
    // reads its config from is not even mounted yet at mouse_init() time)
    // already clamped the PS/2 (and USB HID) cursor to the PHYSICAL panel
    // (mouse_max_x/max_y = fb_get_width()/fb_get_height() - 1, AS THEY STOOD
    // BEFORE this call ever ran, so the full 3840x2160 on the owner's ASUS).
    // fb_set_present_scale() above just shrank fb_get_width()/fb_get_height()
    // to the COMPOSITED logical surface (1920x1080 at 2x) that the compositor
    // actually draws into, but nothing re-synced the mouse driver's bounds to
    // match, and mouse_init()'s own centring (g_mouse.x = fb_w/2) had already
    // placed the cursor at the PHYSICAL centre (1920,1080) - which is exactly
    // one pixel outside the new logical canvas's right/bottom edge, not
    // comfortably inside it. Every PS/2 packet after that stayed clamped
    // against the STALE physical bounds, so the driver-side position can sit
    // anywhere in 0..3839, while the compositor composites and clips into a
    // 1920-wide back buffer: a cursor drawn at x>=1920 is off-canvas and never
    // painted, which is "no mouse pointer at all" from the driver's side
    // working correctly the whole time. mouse_set_bounds() re-clamps the
    // CURRENT position too (see kernel/drivers/mouse.c), so this both fixes
    // future packets and immediately pulls an already-off-canvas cursor back
    // into view on the very next present - no reboot-again needed once this
    // runs. presentscale.h's existing ordering rule ("uiscale must see the
    // REDUCED logical dimensions") turns out to have a second, unstated half:
    // so must the mouse driver, and it is the one caller that runs BEFORE
    // this function rather than after it.
    mouse_set_bounds(0, 0, (int32_t)fb_get_width() - 1, (int32_t)fb_get_height() - 1);

    kprintf("[PRESENTSCALE] ACTIVE %dx: compositing %ux%u -> presenting "
            "%ux%u (%s)\n", want, fb_get_width(), fb_get_height(), pw, ph, src);
    bootlog_write("[PRESENTSCALE] ACTIVE %dx: compositing %ux%u -> presenting "
                  "%ux%u (%s)", want, fb_get_width(), fb_get_height(), pw, ph, src);
}
