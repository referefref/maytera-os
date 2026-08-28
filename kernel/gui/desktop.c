// desktop.c - Desktop manager and dock implementation for MayteraOS
#include "desktop.h"
#include "uiscale.h"
#include "../drivers/battery.h"   // #battmeter
#include "../drivers/rtc.h"   // #135: the ONE RTC driver
#include "window.h"
#include "image.h"
#include "icons.h"
#include "themes.h"
#include "ttf.h"
#include "../types.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../string.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../drivers/mouse.h"
#include "../cpu/isr.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../net/net.h"
#include "../version.h"
#include "terminal.h"
#include "settings.h"
#include "clock.h"
#include "recyclebin.h"
#include "syslog.h"
#include "screensaver.h"
// Forward declarations for app launchers
extern void filebrowser_launch(void);
extern void settings_launch(void);
#include "../exec/elf.h"
#include "../proc/process.h"
#include "../proc/signal.h"   // #126 session teardown: SIGKILL + sig_raise()

// External timer ticks from ISR
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;  // Current timer frequency
static uint64_t s_last_kb_tick = 0;  // Shared 20 Hz keyboard throttle

// CPU usage tracking
static volatile uint64_t g_cpu_busy_ticks = 0;      // Ticks spent doing work
static volatile uint64_t g_cpu_total_ticks = 0;     // Total ticks for measurement period
static volatile uint64_t g_cpu_last_tick = 0;       // Last tick count
static volatile int g_cpu_usage_percent = 0;        // Current CPU usage percentage

// Calculate CPU usage percentage (call periodically)
static void cpu_update_usage(void) {
    uint64_t current_tick = timer_ticks;
    uint64_t elapsed = current_tick - g_cpu_last_tick;

    // Update every ~18 ticks (~1 second at 18.2 Hz timer)
    if (elapsed >= g_timer_hz) {
        if (g_cpu_total_ticks > 0) {
            g_cpu_usage_percent = (g_cpu_busy_ticks * 100) / g_cpu_total_ticks;
            if (g_cpu_usage_percent > 100) g_cpu_usage_percent = 100;
        }
        // Reset counters for next period
        g_cpu_busy_ticks = 0;
        g_cpu_total_ticks = 0;
        g_cpu_last_tick = current_tick;
    }
}

// Get current CPU usage percentage
static int cpu_get_usage(void) {
    return g_cpu_usage_percent;
}

// External filesystem from main
extern fat_fs_t g_fat_fs;

// Session user identity (set by login screen before desktop_run)
static uint32_t g_session_uid = 0;
static uint32_t g_session_gid = 0;

// #566: KERNEL-OWNED session lock state. This is the single authority for
// "is the current session locked". A Ring-3 app (including the compositor)
// cannot forge "unlocked": it can only READ this via SYS_SESSION_IS_LOCKED and
// can only CLEAR it through SYS_SESSION_UNLOCK, which itself calls the
// rate-limited users_authenticate(). The compositor's own g_logged_in-style
// globals are a display cache of this fact, never the fact itself. Locking
// (setting to 1) is always allowed; only unlocking requires a verified password.
static volatile int g_session_locked = 0;
// Has any session authenticated this boot? Gates the post-login kernel_shell
// fallback out of existence (#566, decision 3): once true, a dead desktop
// returns to the login gate, never to an unauthenticated Ring-0 shell.
static volatile int g_session_authenticated = 0;

void desktop_set_session(uint32_t uid, uint32_t gid) {
    g_session_uid = uid;
    g_session_gid = gid;
    g_session_locked = 0;            // a fresh session starts unlocked
    g_session_authenticated = 1;     // an authenticated session now exists
    kprintf("[DESKTOP] Session UID=%u, GID=%u\n", uid, gid);
}

uint32_t desktop_get_session_uid(void) {
    return g_session_uid;
}

uint32_t desktop_get_session_gid(void) {
    return g_session_gid;
}

// Set/clear the kernel-owned lock flag. `locked` is treated as a boolean.
// Callers that CLEAR the flag must have already verified the password; the
// only sanctioned clear path is sys_session_unlock().
void desktop_set_locked(int locked) {
    g_session_locked = locked ? 1 : 0;
    bootlog_write("[SESSION] lock state -> %s (uid=%u)",
                  g_session_locked ? "LOCKED" : "unlocked", g_session_uid);
    kprintf("[DESKTOP] Session %s\n", g_session_locked ? "LOCKED" : "unlocked");
}

int desktop_is_locked(void) {
    return g_session_locked;
}

int desktop_session_authenticated(void) {
    return g_session_authenticated;
}

// ============================================================================
// User-space application launcher
// ============================================================================

// #COMPRESPAWN: A BARE -1 IS THE SAME DEFECT CLASS AS A SILENT FAILURE.
//
// This function had FOUR ways to return -1 and told the durable log about none
// of them: three used LOG_ERROR (gui/syslog.h), which appends to an in-RAM
// syslog ring that dies with the machine, and the fourth (proc_create_user_as)
// uses kprintf, which is SERIAL ONLY. The one machine this matters on - the
// owner's iMac14,4 - has no serial port, so /BOOTLOG.TXT carried
//
//     [SESSION] compositor launch FAILED: launch_userspace_app returned -1
//
// and nothing else. "The filesystem is unmounted", "the heap could not spare
// 1 MB", "the ELF is corrupt" and "the process table is full" left byte-for-
// byte identical evidence, and they need four completely different fixes.
//
// Every return path below now writes ONE durable line naming the reason and
// the resource numbers that would decide it. The cost is a bootlog line on a
// path that already does a megabyte of disk I/O.
static void launch_fail(const char *path, const char *why, int extra) {
    bootlog_write("[LAUNCH] FAILED %s: %s (rc=%d) heapfreeKB=%lu heapusedKB=%lu "
                  "pmmfreeKB=%lu fsmounted=%d",
                  path, why, extra,
                  (unsigned long)(heap_get_free_size() / 1024),
                  (unsigned long)(heap_get_used_size() / 1024),
                  (unsigned long)(pmm_get_free_pages() * 4),
                  g_fat_fs.mounted ? 1 : 0);
    LOG_ERROR("[UserSpace] launch failed");
}

// Launch a user-space ELF application from filesystem
static int launch_userspace_app(const char *path) {
    // #COMPRESPAWN: FREE BEFORE YOU ALLOCATE.
    //
    // A dead process returns NOTHING at the moment it dies. proc_exit() marks
    // it ZOMBIE and explicitly leaves its process slot, its 64 KB kernel stack
    // and its whole address space in place; the only thing that reclaims them
    // is reap_orphan_zombies(), whose only caller was alloc_proc_slot().
    //
    // That put the reap in the wrong ORDER for exactly this function. The
    // sequence was: kmalloc ~1 MB for the ELF, THEN proc_create_user_as(), THEN
    // (inside it) the reap. So when the compositor crashes and this runs to
    // relaunch it, the 1 MB read is asked for while the dead compositor still
    // holds ~29 MB of address space, a 64 KB kernel stack and a table slot, and
    // while every session app the teardown just SIGKILLed is still fully alive
    // (session_end_teardown() does not wait; they die at their next syscall
    // return). MEASURED on the owner's VM <vmid>, golden 2054: the compositor
    // faulted after 8.8 h and this call returned -1, leaving the machine on the
    // in-kernel fallback desktop until reboot.
    //
    // Sweeping here costs a 64-entry snapshot on a path that already reads a
    // megabyte off disk, and it is the ordinary rule, not a workaround.
    uint64_t pmm_before = pmm_get_free_pages();
    int reaped = proc_reap_orphans();
    if (reaped > 0) {
        // pmmreturnedKB IS THE NUMBER THAT MATTERS, AND IT IS WHY THIS LINE
        // EXISTS RATHER THAN A PLAIN "reaped N".
        //
        // It is how much PHYSICAL memory the reap actually gave back, differenced
        // across the one call that destroys dead processes' address spaces.
        // MEASURED 2026-08-26 on VM <vmid>: reaping NINE zombie slots, one of them
        // a compositor whose image alone is 29 MB, returned pmmreturnedKB=2048.
        // Two megabytes. A dead user process's pages are very nearly not returned
        // at all, and the free-page count fell by ~29 MB on every subsequent
        // compositor generation, which is one whole compositor image per death.
        //
        // That is a live leak on exactly this path and it is NOT fixed here: the
        // ownership rules in vmm_destroy_user_space() are the ones whose own
        // comments warn that freeing a shared kernel page table hands live tables
        // to the PMM, so it needs its own change with its own proof. This line is
        // the instrument that makes the leak visible in the durable log instead of
        // requiring someone to difference two heartbeat lines by hand.
        bootlog_write("[LAUNCH] %s: reaped %d zombie process slot(s) before "
                      "allocating (heapfreeKB=%lu pmmfreeKB=%lu pmmreturnedKB=%ld)",
                      path, reaped,
                      (unsigned long)(heap_get_free_size() / 1024),
                      (unsigned long)(pmm_get_free_pages() * 4),
                      (long)((long)pmm_get_free_pages() - (long)pmm_before) * 4);
    }

    if (!g_fat_fs.mounted) {
        launch_fail(path, "root filesystem is not mounted", 0);
        return -1;
    }

    // Read ELF file from disk
    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, path, &size);
    if (!data || size == 0) {
        // NOT NECESSARILY A MISSING FILE. On an ext2-root image this path goes
        // through ext2_read_whole(), whose kmalloc failure is indistinguishable
        // from "not present on ext2" to fat_read_file(), which then falls
        // through to the FAT ESP and reports "open failed". So a heap
        // exhaustion presents as a missing binary. fs/ext2.c now logs the OOM
        // itself; the numbers below are here so the two can be told apart even
        // if that line is lost.
        launch_fail(path, "could not read the binary (missing, or the heap "
                          "could not supply the buffer)", (int)size);
        return -1;
    }

    // Validate ELF format
    int elf_err = elf_validate(data, size);
    if (elf_err != ELF_SUCCESS) {
        launch_fail(path, "not a valid ELF", elf_err);
        kfree(data);
        return -1;
    }

    // Extract filename for process name
    const char *name = path;
    const char *p = path;
    while (*p) {
        if (*p == '/') name = p + 1;
        p++;
    }

    // Create user-space process
    // #692: the desktop launches on behalf of the logged-in session user.
    // The identity is now named AT the spawn instead of being patched onto
    // the child afterwards, so there is no window in which the child is
    // root and no second place to forget.
    int pid = proc_create_user_as(name, data, size, NULL, NULL, proc_as_session());
    if (pid > 0) {
        // #692: the session uid/gid used to be stamped onto the child HERE,
        // after it already existed. Removed: the identity is now part of the
        // spawn itself, and a second place to set it is a second place to
        // forget it.
        LOG_INFO("[UserSpace] Process created successfully");
    } else {
        launch_fail(path, "proc_create_user_as() refused the spawn (no free "
                          "process slot, no address space, no kernel stack, or "
                          "the ELF would not load)", pid);
    }

    kfree(data);
    return pid;
}

// (#182) Launch the Ring-3 FM synthesiser for the DOS OPL2 emulation.
//
// NOT static, and deliberately here rather than in kernel/dos/: this is the
// file that already has proc_create_user_as, proc_as_session and elf_validate
// correctly declared, and dosexec.c cannot include proc/process.h because it
// carries a conflicting hand-written `extern int proc_create(...)`.
//
// THE RETURN VALUE GATES opl2_installed_policy(). If this returns <= 0 the
// OPL2 reports ABSENT and every DOS guest falls back to no music exactly as it
// did before #182. That fallback is what keeps the PRESENT answer honest:
// PRESENT means "a synthesiser is running", not "a synthesiser exists in the
// source tree".
// The SAME predicate audio_pcm_open() uses to decide ENODEV
// (drivers/audio_pcm.c:331), so the kernel's answer and the synthesiser's
// answer cannot disagree about whether this machine can make a sound.
extern int uac_is_ready(void);
extern bool audio_is_available(void);   // drivers/audio.h: returns bool, not int

int fm_launch_synth(void) {
    // A SUCCESSFUL SPAWN IS NOT PROOF THAT SOUND WILL COME OUT, and this check
    // exists because the version without it was measured doing the wrong thing.
    //
    // On a machine with no audio device the spawn succeeds, this function
    // returns a valid pid, the OPL2 reports PRESENT, and /APPS/FMSYNTH then
    // gets ENODEV from sys_audio_pcm_open, correctly refuses to pretend, and
    // exits. What is left is a chip advertising itself with nothing behind it
    // and no sound: precisely the fabrication #175 refused to ship, put back by
    // the ticket that was supposed to earn the right to remove it.
    //
    // Both halves were individually right. Only the JOIN was wrong, and it was
    // only wrong on a machine without an audio device, which is not the machine
    // anyone would think to test. Measured on VM <vmid>, build 2001.
    if (!uac_is_ready() && !audio_is_available()) {
        kprintf("[dos] (#182) no audio sink on this machine (no USB DAC, no HDA/AC97): "
                "NOT launching the FM synthesiser, and the OPL2 will truthfully "
                "report ABSENT. A chip that reports PRESENT with nothing behind it "
                "is the fabrication #175 refused to ship.\n");
        return -1;
    }
    int pid = launch_userspace_app("/APPS/FMSYNTH");
    if (pid <= 0) {
        kprintf("[dos] (#182) /APPS/FMSYNTH did not launch (%d): FM synthesis is "
                "unavailable, so the OPL2 will truthfully report ABSENT.\n", pid);
        return pid;
    }
    kprintf("[dos] (#182) Ring-3 FM synthesiser running as pid %d\n", pid);
    return pid;
}

// #552: these userspace_*_launch wrappers were the kernel start menu's
// app-launch callbacks (g_apps_items / g_games_items / g_system_items,
// now removed). Marked unused rather than deleted: they are small,
// self-contained, and this fallback desktop may grow its own icon-based
// launcher for them later instead of a start menu.
// #703: the wrappers whose kernel-side fallback has been deleted (calculator,
// editor, browser, imageviewer, mediaplayer, irc, syslog viewer, solitaire,
// pong, lemmings, DOOM) went with it. Each of those apps ships as a userland
// ELF that the compositor start menu spawns; the kernel copies were reachable
// only from gui/registry.c, which had no caller.

// User-space Test launcher (Debug app with extensive logging)
__attribute__((unused)) static void userspace_test_launch(void) {
    kprintf("[TEST] Loading from /apps/test (user-space debug app)\n");
    int pid = launch_userspace_app("/apps/test");
    if (pid > 0) {
        kprintf("[TEST] User-space process started successfully, PID=%d\n", pid);
    } else {
        kprintf("[TEST] Failed to launch user-space test app, PID=%d\n", pid);
    }
}


// User-space Terminal launcher
__attribute__((unused)) static void userspace_terminal_launch(void) {
    LOG_INFO("[Terminal] Loading from /apps/terminal (user-space)");
    int pid = launch_userspace_app("/apps/terminal");
    if (pid > 0) {
        LOG_INFO("[Terminal] User-space process started successfully");
    } else {
        LOG_INFO("[Terminal] User-space failed - using kernel version");
        terminal_launch();
    }
}

// User-space Files launcher
__attribute__((unused)) static void userspace_files_launch(void) {
    LOG_INFO("[Files] Loading from /apps/files (user-space)");
    int pid = launch_userspace_app("/apps/files");
    if (pid > 0) {
        LOG_INFO("[Files] User-space process started successfully");
    } else {
        LOG_INFO("[Files] User-space failed - using kernel version");
        filebrowser_launch();
    }
}

// User-space Settings launcher
__attribute__((unused)) static void userspace_settings_launch(void) {
    LOG_INFO("[Settings] Loading from /apps/settings (user-space)");
    int pid = launch_userspace_app("/apps/settings");
    if (pid > 0) {
        LOG_INFO("[Settings] User-space process started successfully");
    } else {
        LOG_INFO("[Settings] User-space failed - using kernel version");
        settings_launch();
    }
}

// User-space App Store launcher (#402)
__attribute__((unused)) static void userspace_appstore_launch(void) {
    LOG_INFO("[AppStore] Loading from /apps/appstore (user-space)");
    launch_userspace_app("/apps/appstore");
}

// User-space Python launcher
__attribute__((unused)) static void userspace_python_launch(void) {
    LOG_INFO("[Python] Loading from /apps/python (user-space)");
    int pid = launch_userspace_app("/apps/python");
    if (pid > 0) {
        LOG_INFO("[Python] User-space process started successfully");
    } else {
        LOG_INFO("[Python] User-space failed");
    }
}

// Global desktop state

// Forward declarations for kernel-mode app launchers used as fallbacks
extern void recyclebin_launch(void);

// #694: the user-space Paint launcher wrapper is gone with the kernel Paint
// it fell back to. The shipping desktop is the userland compositor, whose
// start menu spawns /APPS/PAINT directly.

// User-space Clock launcher
__attribute__((unused)) static void userspace_clock_launch(void) {
    LOG_INFO("[Clock] Loading from /apps/clock");
    int pid = launch_userspace_app("/apps/clock");
    if (pid > 0) {
        LOG_INFO("[Clock] User-space process started successfully");
    } else {
        LOG_INFO("[Clock] User-space failed, using kernel version");
        clock_launch();
    }
}

// User-space Recycle Bin launcher
__attribute__((unused)) static void userspace_recyclebin_launch(void) {
    LOG_INFO("[RecycleBin] Loading from /apps/recycle");
    int pid = launch_userspace_app("/apps/recycle");
    if (pid > 0) {
        LOG_INFO("[RecycleBin] User-space process started successfully");
    } else {
        LOG_INFO("[RecycleBin] User-space failed, using kernel version");
        recyclebin_launch();
    }
}

// User-space Screensaver launcher
__attribute__((unused)) static void userspace_screensaver_launch(void) {
    LOG_INFO("[Screensaver] Loading from /apps/ssaver");
    int pid = launch_userspace_app("/apps/ssaver");
    if (pid > 0) {
        LOG_INFO("[Screensaver] User-space process started successfully");
    }
}

static desktop_t g_desktop;

// Background image storage
static image_t g_bg_image_data;
// #552: the kernel's own start menu (draw_start_menu and friends) has been
// removed. It only ever ran when /APPS/COMPOSIT was missing (see desktop_run
// below): the userland compositor's start menu, toggled by the taskbar start
// button and the Super/Windows key, is the only start menu in MayteraOS now.
// g_start_menu_open stays declared (and permanently false) because a few
// harmless reads of it remain below (desktop_init logging, and
// desktop_handle_right_click's "close any open menus" reset).
static bool g_start_menu_open = false;

// Menu item definition with icon support. Kept: still used by the
// right-click context menu's cascading submenus (context_item_t.submenu),
// even though the kernel start menu itself is gone.
typedef struct {
    const char *name;
    int icon_id;
    void (*action)(void);
    const char *userspace_path;  // If set, launch this ELF from disk
} menu_item_t;

// Set by the context menu's submenu click handling (get_context_menu_item_at).
static int g_clicked_item = -1;

// #552: public reload hook, called from net/remote_ctrl.c after an install
// or uninstall mutates /APPS/REGINI.CFG. It used to reload the kernel start
// menu's config-driven entries; the kernel start menu is gone, so this is
// now a no-op, kept only so remote_ctrl.c and the SYS_DESKTOP_MENU_RELOAD
// syscall do not need to change.
void desktop_menu_reload(void) {
}

// ============================================================================
// Context Menu (Right-click on desktop)
// ============================================================================
static bool g_context_menu_open = false;
static int32_t g_context_menu_x = 0;
static int32_t g_context_menu_y = 0;
static int g_context_submenu_open = -1;  // Index of open submenu, -1 = none

#define CONTEXT_MENU_WIDTH   160
#define CONTEXT_MENU_ITEM_H  24
#define CONTEXT_MENU_SEP_H   8
#define SUBMENU_ARROW_WIDTH  12

typedef struct {
    const char *name;
    void (*action)(void);
    menu_item_t *submenu;      // NULL if no submenu, otherwise points to submenu items
    int submenu_count;         // Number of items in submenu
} context_item_t;

// Forward declarations
static void open_wallpaper_picker(void);
static void close_wallpaper_picker(void);
static void open_screensaver_settings(void);
static void close_screensaver_settings(void);
static void do_refresh(void);
static void do_new_folder(void);
static void do_new_file(void);
static void do_paste(void);
static void do_display_settings(void);
static void do_properties(void);

// Context menu items (right-click on desktop)
static context_item_t g_context_items[] = {
    {"New Folder",         do_new_folder, NULL, 0},
    {"New File",           do_new_file, NULL, 0},
    {"Refresh",            do_refresh, NULL, 0},
    {"Paste",              do_paste, NULL, 0},
    {"---",                NULL, NULL, 0},  // Separator
    {"Display Settings",   do_display_settings, NULL, 0},
    {"Change Background",  open_wallpaper_picker, NULL, 0},
    {"---",                NULL, NULL, 0},  // Separator
    {"Properties",         do_properties, NULL, 0},
    {NULL, NULL, NULL, 0}
};

// Context menu action handlers
static void do_new_folder(void) {
    kprintf("[Desktop] New Folder requested\n");
    // TODO: Implement create new folder dialog
}

static void do_new_file(void) {
    kprintf("[Desktop] New File requested\n");
    // TODO: Implement create new file dialog
}

static void do_paste(void) {
    kprintf("[Desktop] Paste requested\n");
    // TODO: Implement paste from clipboard
}

static void do_display_settings(void) {
    kprintf("[Desktop] Display Settings requested\n");
    settings_launch();  // Open settings app
}

static void do_properties(void) {
    kprintf("[Desktop] Properties requested\n");
    // TODO: Implement desktop properties dialog
}

// Refresh handler - just invalidate to trigger redraw
static void do_refresh(void) {
    kprintf("[Desktop] Refresh requested\n");
    wm_invalidate_all();
}

// Forward declarations for context menu
static void draw_context_menu(void);
static int get_context_menu_item_at(int x, int y);

// ============================================================================
// Wallpaper Picker
// ============================================================================
static bool g_wallpaper_picker_open = false;
static int32_t g_picker_x = 0;
static int32_t g_picker_y = 0;

// Available wallpapers (matching files on disk)
typedef struct {
    const char *name;
    const char *filename;
} wallpaper_entry_t;

static wallpaper_entry_t g_wallpapers[] = {
    // Default background
    {"Default Blue", "BACK.BMP"},
    // Eberhard Grossgasteiger landscapes
    {"Mountain Vista 1", "EBERG01.BMP"},
    {"Mountain Vista 3", "EBERG03.BMP"},
    {"Mountain Vista 4", "EBERG04.BMP"},
    {"Mountain Vista 5", "EBERG05.BMP"},
    {"Alpine Scenery 1", "EBERG06.BMP"},
    {"Alpine Scenery 2", "EBERG07.BMP"},
    {"Alpine Scenery 3", "EBERG08.BMP"},
    {"Alpine Scenery 4", "EBERG09.BMP"},
    {"Alpine Scenery 5", "EBERG10.BMP"},
    {"Nature Landscape 1", "EBERG11.BMP"},
    {"Nature Landscape 2", "EBERG12.BMP"},
    {"Nature Landscape 3", "EBERG13.BMP"},
    {"Nature Landscape 4", "EBERG15.BMP"},
    {"Nature Landscape 5", "EBERG16.BMP"},
    {"Scenic View 1", "EBERG17.BMP"},
    {"Scenic View 2", "EBERG18.BMP"},
    {"Scenic View 3", "EBERG19.BMP"},
    {"Scenic View 4", "EBERG20.BMP"},
    {"Scenic View 5", "EBERG21.BMP"},
    {"Panorama 1", "EBERG22.BMP"},
    {"Panorama 2", "EBERG25.BMP"},
    {"Panorama 3", "EBERG26.BMP"},
    {"Panorama 4", "EBERG27.BMP"},
    {"Panorama 5", "EBERG28.BMP"},
    {"Panorama 6", "EBERG29.BMP"},
    {"Panorama 7", "EBERG30.BMP"},
    // Ocean images
    {"Ocean Waves 1", "OCEAN01.BMP"},
    {"Ocean Waves 2", "OCEAN02.BMP"},
    {"Ocean Sunset 1", "OCEAN03.BMP"},
    {"Ocean Sunset 2", "OCEAN04.BMP"},
    {"Seascape 1", "OCEAN05.BMP"},
    {"Seascape 2", "OCEAN06.BMP"},
    {"Seascape 3", "OCEAN07.BMP"},
    {"Coastal View 1", "OCEAN08.BMP"},
    {"Coastal View 2", "OCEAN10.BMP"},
    {"Beach Scene 1", "OCEAN12.BMP"},
    {"Beach Scene 2", "OCEAN13.BMP"},
    {"Beach Scene 3", "OCEAN14.BMP"},
    // Macro nature images
    {"Macro Nature 1", "MACRO01.BMP"},
    {"Macro Nature 2", "MACRO02.BMP"},
    {"Macro Nature 3", "MACRO05.BMP"},
    {"Macro Nature 4", "MACRO06.BMP"},
    {"Macro Nature 5", "MACRO07.BMP"},
    {"Macro Details 1", "MACRO08.BMP"},
    {"Macro Details 2", "MACRO11.BMP"},
    {"Macro Details 3", "MACRO12.BMP"},
    {"Macro Details 4", "MACRO13.BMP"},
    {"Macro Details 5", "MACRO14.BMP"},
    {"Close-up Nature 1", "MACRO15.BMP"},
    {"Close-up Nature 2", "MACRO16.BMP"},
    {"Close-up Nature 3", "MACRO17.BMP"},
    {"Close-up Nature 4", "MACRO19.BMP"},
    {"Close-up Nature 5", "MACRO20.BMP"},
    // Theme default wallpapers
    {"Default Theme", "DEFAULT.BMP"},
    {"Retro Unix", "RETRO.BMP"},
    {"Retro Stipple", "RETROSTIP.BMP"},
    {"Modern Light", "MODLIGHT.BMP"},
    {"Modern Dark", "MODDARK.BMP"},
    {"Fluent Light", "FLULIGHT.BMP"},
    {"Fluent Dark", "FLUDARK.BMP"},
    {"Ocean Theme", "OCEAN.BMP"},
    {"Sunset Theme", "SUNSET.BMP"},
    {"Forest Theme", "FOREST.BMP"},
    {"Classic Win95", "CLASSIC.BMP"},
    {"Dark Mode", "DARKMODE.BMP"},
    {"Light Mode", "LIGHTMODE.BMP"},
    {"High Contrast", "HIGHCON.BMP"},
    // Gradient option
    {"Gradient (Blue)", NULL},
    {NULL, NULL}
};

// Thumbnail dimensions (64x48 maintains 4:3 aspect ratio)
#define THUMB_WIDTH     64
#define THUMB_HEIGHT    48
#define THUMB_PADDING   8
#define THUMB_BORDER    2

// Grid layout: columns and spacing
#define THUMB_COLS      5
#define THUMB_CELL_W    (THUMB_WIDTH + THUMB_PADDING)
#define THUMB_CELL_H    (THUMB_HEIGHT + THUMB_PADDING + 16)  // +16 for name text below

// Picker dimensions calculated from grid
#define PICKER_WIDTH    (THUMB_COLS * THUMB_CELL_W + THUMB_PADDING * 2)
#define PICKER_HEIGHT   400
#define PICKER_ITEM_H   20
#define PICKER_TITLE_H  24
#define PICKER_CONTENT_H (PICKER_HEIGHT - PICKER_TITLE_H - 8)

// Maximum wallpapers (for cache sizing)
#define MAX_WALLPAPERS  64

// Thumbnail cache structure
typedef struct {
    uint32_t *pixels;       // Thumbnail pixel data
    uint32_t width;         // Actual thumbnail width
    uint32_t height;        // Actual thumbnail height
    bool loaded;            // Whether thumbnail is loaded
} thumbnail_t;

// Thumbnail cache
static thumbnail_t g_thumbnails[MAX_WALLPAPERS];
static bool g_thumbnails_initialized = false;
static int g_picker_scroll = 0;     // Current scroll offset in rows
static int g_picker_max_scroll = 0; // Maximum scroll offset
static int g_current_wallpaper_index = 0;  // Currently selected wallpaper index

// Forward declarations for wallpaper picker
static void draw_wallpaper_picker(void);
static int get_wallpaper_item_at(int x, int y);
static void set_wallpaper(const char *filename);
static void generate_thumbnail(int index);
static void generate_thumbnails(void);
static void free_thumbnails(void);
static uint32_t *scale_image_to_thumbnail(uint32_t *src_pixels, uint32_t src_w, uint32_t src_h,
                                          uint32_t *out_w, uint32_t *out_h);

// ============================================================================
// Screensaver Settings Picker
// ============================================================================
static bool g_ss_settings_open = false;
static int32_t g_ss_settings_x = 0;
static int32_t g_ss_settings_y = 0;
__attribute__((unused)) static int g_ss_selected_type = 0;
__attribute__((unused)) static int g_ss_selected_timeout = 2;  // Index into timeout options

#define SS_SETTINGS_WIDTH   280
#define SS_SETTINGS_HEIGHT  260
#define SS_ITEM_H           22

// Timeout options in seconds
static const uint32_t g_ss_timeouts[] = {60, 120, 300, 600, 0};  // 1, 2, 5, 10 min, Never
static const char *g_ss_timeout_names[] = {"1 minute", "2 minutes", "5 minutes", "10 minutes", "Never", NULL};

// Forward declarations for screensaver settings
static void draw_screensaver_settings(void);
static int get_ss_settings_item_at(int x, int y);

// ============================================================================
// Internal Helper Functions
// ============================================================================

// Convert ARGB color (0xAARRGGBB) to framebuffer format (BGRA)
static uint32_t argb_to_fb(uint32_t argb) {
    uint8_t a = (argb >> 24) & 0xFF;
    uint8_t r = (argb >> 16) & 0xFF;
    uint8_t g = (argb >> 8) & 0xFF;
    uint8_t b = argb & 0xFF;
    UNUSED(a);  // Alpha handled separately for blending
    // Framebuffer is BGRA: blue in low byte
    return (b << 16) | (g << 8) | r;
}

// Simple alpha blending (integer math only, no floating point)
// alpha is 0-255 where 255 is fully opaque
static uint32_t blend_color(uint32_t bg, uint32_t fg, uint8_t alpha) {
    if (alpha == 255) return fg;
    if (alpha == 0) return bg;

    uint8_t bg_r = bg & 0xFF;
    uint8_t bg_g = (bg >> 8) & 0xFF;
    uint8_t bg_b = (bg >> 16) & 0xFF;

    uint8_t fg_r = fg & 0xFF;
    uint8_t fg_g = (fg >> 8) & 0xFF;
    uint8_t fg_b = (fg >> 16) & 0xFF;

    // Integer alpha blending: out = bg + (fg - bg) * alpha / 255
    uint8_t out_r = bg_r + (((fg_r - bg_r) * alpha) >> 8);
    uint8_t out_g = bg_g + (((fg_g - bg_g) * alpha) >> 8);
    uint8_t out_b = bg_b + (((fg_b - bg_b) * alpha) >> 8);

    return (out_b << 16) | (out_g << 8) | out_r;
}

// Draw a filled rectangle with alpha blending
__attribute__((unused))
static void draw_rect_alpha(int32_t x, int32_t y, uint32_t w, uint32_t h,
                            uint32_t argb_color) {
    uint8_t alpha = (argb_color >> 24) & 0xFF;
    uint32_t fb_color = argb_to_fb(argb_color);

    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    // Clipping
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen_w) w = screen_w - x;
    if (y + h > screen_h) h = screen_h - y;

    if (alpha == 255) {
        // Fully opaque - use fast fill
        fb_fill_rect(x, y, w, h, fb_color);
    } else {
        // Semi-transparent - blend each pixel
        for (uint32_t py = 0; py < h; py++) {
            for (uint32_t px = 0; px < w; px++) {
                uint32_t bg = fb_get_pixel(x + px, y + py);
                uint32_t blended = blend_color(bg, fb_color, alpha);
                fb_put_pixel(x + px, y + py, blended);
            }
        }
    }
}

// Draw a rounded rectangle (simplified - just corners clipped)
static void draw_rounded_rect(int32_t x, int32_t y, uint32_t w, uint32_t h,
                              uint32_t radius, uint32_t argb_color) {
    uint8_t alpha = (argb_color >> 24) & 0xFF;
    uint32_t fb_color = argb_to_fb(argb_color);

    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    // Draw main body (excluding corners)
    for (uint32_t py = 0; py < h; py++) {
        for (uint32_t px = 0; px < w; px++) {
            int32_t sx = x + px;
            int32_t sy = y + py;

            // Skip if outside screen
            if (sx < 0 || sy < 0 || (uint32_t)sx >= screen_w || (uint32_t)sy >= screen_h)
                continue;

            // Check if in corner region and should be clipped
            bool in_corner = false;
            int32_t dx = 0, dy = 0;

            // Top-left corner
            if (px < radius && py < radius) {
                dx = radius - px - 1;
                dy = radius - py - 1;
                in_corner = true;
            }
            // Top-right corner
            else if (px >= w - radius && py < radius) {
                dx = px - (w - radius);
                dy = radius - py - 1;
                in_corner = true;
            }
            // Bottom-left corner
            else if (px < radius && py >= h - radius) {
                dx = radius - px - 1;
                dy = py - (h - radius);
                in_corner = true;
            }
            // Bottom-right corner
            else if (px >= w - radius && py >= h - radius) {
                dx = px - (w - radius);
                dy = py - (h - radius);
                in_corner = true;
            }

            // If in corner, check if outside the rounded corner circle
            if (in_corner) {
                // Distance squared from corner center
                uint32_t dist_sq = dx * dx + dy * dy;
                uint32_t radius_sq = radius * radius;
                if (dist_sq > radius_sq) {
                    continue;  // Outside corner, skip this pixel
                }
            }

            // Draw the pixel
            if (alpha == 255) {
                fb_put_pixel(sx, sy, fb_color);
            } else {
                uint32_t bg = fb_get_pixel(sx, sy);
                fb_put_pixel(sx, sy, blend_color(bg, fb_color, alpha));
            }
        }
    }
}

// Draw a character at position using font
static void draw_char(int32_t x, int32_t y, char c, uint32_t color) {
    if (ttf_is_ready()) {
        ttf_draw_char(x, y, (unsigned char)c, TTF_SIZE_NORMAL, TTF_STYLE_NORMAL, color);
        return;
    }
    const uint8_t *glyph = font_get_glyph(c);
    if (!glyph) return;

    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    for (int row = 0; row < FONT_HEIGHT; row++) {
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (glyph[row] & (0x80 >> col)) {
                int32_t px = x + col;
                int32_t py = y + row;
                if (px >= 0 && py >= 0 && (uint32_t)px < screen_w && (uint32_t)py < screen_h) {
                    fb_put_pixel(px, py, color);
                }
            }
        }
    }
}

// Draw a string at position
static void draw_string(int32_t x, int32_t y, const char *str, uint32_t color) {
    if (ttf_is_ready()) {
        ttf_draw_string(x, y, str, TTF_SIZE_NORMAL, color);
        return;
    }
    while (*str) {
        draw_char(x, y, *str, color);
        x += FONT_WIDTH;
        str++;
    }
}

// Get string width in pixels
static uint32_t string_width(const char *str) {
    if (ttf_is_ready()) {
        return ttf_measure_string(str, TTF_SIZE_NORMAL);
    }
    return strlen(str) * FONT_WIDTH;
}

// Calculate taskbar position and dimensions (half width, left-aligned at bottom)
static void dock_recalculate(void) {
    dock_t *dock = &g_desktop.dock;

    // Taskbar is half screen width, left-aligned at bottom
    dock->width = g_desktop.screen_width / 2;
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
    dock->height = TASKBAR_HEIGHT;
    dock->x = 0;  // Left-aligned
    dock->y = g_desktop.screen_height - dock->height;

    // App icons start after the Start button
    int32_t icon_x = START_BTN_WIDTH + TASKBAR_PADDING;
    int32_t icon_y = dock->y + (TASKBAR_HEIGHT - PROCESS_BTN_SIZE) / 2;

    // Calculate positions for each app (square buttons)
    for (uint32_t i = 0; i < DOCK_MAX_APPS; i++) {
        if (dock->apps[i].active) {
            dock->apps[i].x = icon_x;
            dock->apps[i].y = icon_y;
            icon_x += PROCESS_BTN_SIZE + TASKBAR_ICON_SPACING;
        }
    }

    kprintf("[Taskbar] Recalculated: %u apps, at (%d,%d) size %ux%u\n",
            dock->app_count, dock->x, dock->y, dock->width, dock->height);
}

// ============================================================================
// Clock Widget - CMOS RTC Functions
// ============================================================================

// #135: this file used to carry a PRIVATE FOURTH copy of the RTC decode
// (its own bcd_to_binary(), its own cmos_is_bcd(), its own port I/O with a
// different NMI-bit convention to gui/clock.c's). Four decoders and one
// encoder that disagreed with all of them is what shipped the 6h06m clock
// error. There is now one driver, drivers/rtc.c, and one codec,
// rustkern/rtcenc.rs, for both directions.
static void rtc_get_time(uint8_t *hours, uint8_t *minutes, uint8_t *seconds) {
    int h = 0, m = 0, s = 0;
    rtc_read_time(&h, &m, &s);
    if (hours)   *hours   = (uint8_t)h;
    if (minutes) *minutes = (uint8_t)m;
    if (seconds) *seconds = (uint8_t)s;
}

// Clock widget dimensions
#define CLOCK_PADDING_X     12
#define CLOCK_PADDING_Y     6
#define CLOCK_MARGIN_RIGHT  16
#define CLOCK_MARGIN_TOP    10
#define CLOCK_CORNER_RADIUS 10

// Draw the taskbar clock in the top-right corner
static void desktop_clock_draw(void) {
    uint8_t hours, minutes, seconds;
    rtc_get_time(&hours, &minutes, &seconds);

    // Format time string as HH:MM:SS
    char time_str[9];
    time_str[0] = '0' + (hours / 10);
    time_str[1] = '0' + (hours % 10);
    time_str[2] = ':';
    time_str[3] = '0' + (minutes / 10);
    time_str[4] = '0' + (minutes % 10);
    time_str[5] = ':';
    time_str[6] = '0' + (seconds / 10);
    time_str[7] = '0' + (seconds % 10);
    time_str[8] = '\0';

    // Calculate clock widget dimensions
    uint32_t text_width = string_width(time_str);
    uint32_t widget_width = text_width + (CLOCK_PADDING_X * 2);
    uint32_t widget_height = FONT_HEIGHT + (CLOCK_PADDING_Y * 2);

    // Position in top-right corner
    int32_t widget_x = g_desktop.screen_width - widget_width - CLOCK_MARGIN_RIGHT;
    int32_t widget_y = CLOCK_MARGIN_TOP;

    // Draw semi-transparent dark background pill shape
    // 0xCC = ~80% opacity, 0x222222 = dark gray
    draw_rounded_rect(widget_x, widget_y, widget_width, widget_height,
                      CLOCK_CORNER_RADIUS, 0xCC222222);

    // Draw white text
    int32_t text_x = widget_x + CLOCK_PADDING_X;
    int32_t text_y = widget_y + CLOCK_PADDING_Y;
    draw_string(text_x, text_y, time_str, argb_to_fb(0xFFFFFFFF));
}

// ============================================================================
// Desktop API Implementation
// ============================================================================

// Generate a nice gradient background (ocean/sky theme to match Pexels image)
static void generate_gradient_background(void) {
    uint32_t width = g_desktop.screen_width;
    uint32_t height = g_desktop.screen_height;

    // Allocate pixel buffer
    size_t size = width * height * sizeof(uint32_t);
    uint32_t *pixels = (uint32_t *)kmalloc(size);
    if (!pixels) {
        kprintf("[Desktop] Failed to allocate gradient buffer\n");
        return;
    }

    // Generate gradient from deep blue (top) to cyan/teal (bottom)
    // Colors matching ocean theme like the Pexels image
    for (uint32_t y = 0; y < height; y++) {
        // Interpolate from top color to bottom color
        uint32_t t = (y * 255) / height;  // 0-255 based on y position

        // Top: sky blue (RGB: 0x4a90c2)  Bottom: ocean blue (RGB: 0x1e5a8a)
        uint8_t r = 0x4a - ((0x4a - 0x1e) * t) / 255;
        uint8_t g = 0x90 - ((0x90 - 0x5a) * t) / 255;
        uint8_t b = 0xc2 - ((0xc2 - 0x8a) * t) / 255;

        // Use argb_to_fb for correct color conversion
        // argb_to_fb converts from ARGB to framebuffer format (BGRA)
        uint32_t argb = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        uint32_t fb_color = argb_to_fb(argb);

        for (uint32_t x = 0; x < width; x++) {
            pixels[y * width + x] = fb_color;
        }
    }

    // Set up image structure
    g_bg_image_data.width = width;
    g_bg_image_data.height = height;
    g_bg_image_data.pixels = pixels;
    g_desktop.bg_image = &g_bg_image_data;

    kprintf("[Desktop] Generated %ux%u gradient background\n", width, height);
}

// Load background image from filesystem
static void load_background_image(void) {
    // Try to load background image from boot disk
    extern fat_fs_t g_fat_fs;
    static fat_fs_t boot_fs;
    fat_fs_t *fs_to_use = NULL;

    // First try the globally mounted filesystem
    if (g_fat_fs.mounted) {
        fs_to_use = &g_fat_fs;
        kprintf("[Desktop] Using globally mounted FAT filesystem\n");
    }

    // If not mounted, try mounting the boot disk's GPT partition
    // Boot disk is typically on primary master (drive 0), and GPT EFI partition starts at LBA 2048
    if (!fs_to_use) {
        kprintf("[Desktop] Trying to mount boot disk GPT partition...\n");
        if (fat_mount_lba(0, 2048, &boot_fs) == 0) {
            fs_to_use = &boot_fs;
            kprintf("[Desktop] Successfully mounted boot disk\n");
        } else {
            // Also try secondary master (drive 1)
            if (fat_mount_lba(1, 2048, &boot_fs) == 0) {
                fs_to_use = &boot_fs;
                kprintf("[Desktop] Mounted from drive 1\n");
            } else {
                // Try raw mount at LBA 0 (in case it's a raw FAT disk)
                if (fat_mount_lba(0, 0, &boot_fs) == 0) {
                    fs_to_use = &boot_fs;
                }
            }
        }
    }

    if (!fs_to_use) {
        kprintf("[Desktop] No FAT filesystem available, using gradient background\n");
        generate_gradient_background();
        return;
    }

    kprintf("[Desktop] Searching for background image...\n");

    // Try different possible filenames. "/MAYTERA_MODERN.BMP" is the current
    // out-of-box default (#745, second pass - see userland/apps/compositor/
    // wallpaper.c's WP_DEFAULT_FILE); tried first so this rarely-used kernel
    // fallback desktop (see the comment above load_background_image's caller
    // chain: it only runs when /APPS/COMPOSIT is missing) still shows a real
    // wallpaper instead of falling through to the gradient. Old names kept as
    // further fallbacks.
    const char *bg_files[] = {"/MAYTERA_MODERN.BMP", "/BACK.BMP", "/BACKGROUND.BMP", "/BG.BMP", "BACK.BMP", NULL};

    for (int i = 0; bg_files[i] != NULL; i++) {
        kprintf("[Desktop] Trying %s...\n", bg_files[i]);
        uint32_t size = 0;
        void *data = fat_read_file(fs_to_use, bg_files[i], &size);
        kprintf("[Desktop] fat_read_file returned: data=%p size=%u\n", data, size);

        if (data && size > 54) {
            kprintf("[Desktop] Loading background from %s (%u bytes)...\n", bg_files[i], size);

            int result = image_load_bmp(data, size, &g_bg_image_data);
            if (result == IMAGE_SUCCESS) {
                // Convert to the desktop image structure format
                g_desktop.bg_image = (image_t *)&g_bg_image_data;
                kprintf("[Desktop] Background image loaded: %ux%u\n",
                        g_bg_image_data.width, g_bg_image_data.height);
                kfree(data);
                return;
            } else {
                kprintf("[Desktop] Failed to load BMP: %s\n", image_error_string(result));
            }
            kfree(data);
        }
    }

    kprintf("[Desktop] No background image found, using gradient\n");
    generate_gradient_background();
}

// ============================================================================
// Desktop Icons Implementation
// ============================================================================

// Calculate screen position for a desktop icon based on grid position
static void desktop_icon_get_position(int grid_x, int grid_y, int32_t *screen_x, int32_t *screen_y) {
    *screen_x = DESKTOP_ICON_MARGIN_X + grid_x * DESKTOP_ICON_SPACING_X;
    *screen_y = DESKTOP_ICON_MARGIN_Y + grid_y * DESKTOP_ICON_SPACING_Y;
}

// Add a desktop icon
int desktop_add_icon(const char *name, int icon_id, int grid_x, int grid_y, void (*launch)(void)) {
    if (g_desktop.icon_count >= DESKTOP_ICON_MAX) {
        kprintf("[Desktop] Cannot add icon: maximum reached\n");
        return -1;
    }

    // Find an empty slot
    for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
        if (!g_desktop.icons[i].active) {
            desktop_icon_t *icon = &g_desktop.icons[i];
            strncpy(icon->name, name, DESKTOP_ICON_NAME_LEN - 1);
            icon->name[DESKTOP_ICON_NAME_LEN - 1] = '\0';
            icon->icon_id = icon_id;
            icon->grid_x = grid_x;
            icon->grid_y = grid_y;
            icon->launch = launch;
            icon->active = true;
            icon->selected = false;
            g_desktop.icon_count++;
            kprintf("[Desktop] Added icon: %s at grid (%d, %d)\n", name, grid_x, grid_y);
            return i;
        }
    }
    return -1;
}

// Remove a desktop icon by index
void desktop_remove_icon(int index) {
    if (index < 0 || index >= DESKTOP_ICON_MAX) return;
    if (!g_desktop.icons[index].active) return;

    g_desktop.icons[index].active = false;
    g_desktop.icon_count--;
}

// Draw a single desktop icon with label
static void draw_desktop_icon(desktop_icon_t *icon) {
    if (!icon->active) return;

    int32_t x, y;
    desktop_icon_get_position(icon->grid_x, icon->grid_y, &x, &y);

    // Draw selection highlight if selected
    if (icon->selected) {
        fb_fill_rect(x - 4, y - 4, DESKTOP_ICON_SIZE + 8,
                     DESKTOP_ICON_SIZE + DESKTOP_ICON_LABEL_H + 8,
                     argb_to_fb(0x40FFFFFF));
    }

    // Draw the icon (scaled to DESKTOP_ICON_SIZE)
    if (icon->icon_id >= 0) {
        icon_draw_scaled(icon->icon_id, x, y, DESKTOP_ICON_SIZE, 0xFFFFFF);
    } else {
        // Fallback: draw a colored rectangle
        fb_fill_rect(x, y, DESKTOP_ICON_SIZE, DESKTOP_ICON_SIZE, argb_to_fb(0xFF808080));
    }

    // Draw icon label below the icon (centered, with shadow for readability)
    int label_len = strlen(icon->name);
    int label_width = label_len * FONT_WIDTH;
    int label_x = x + (DESKTOP_ICON_SIZE - label_width) / 2;
    int label_y = y + DESKTOP_ICON_SIZE + 4;

    // Clamp label_x to keep it on screen
    if (label_x < 2) label_x = 2;

    // Draw text shadow for readability
    draw_string(label_x + 1, label_y + 1, icon->name, argb_to_fb(0xFF000000));
    // Draw text
    draw_string(label_x, label_y, icon->name, argb_to_fb(0xFFFFFFFF));
}

// Draw all desktop icons
void desktop_draw_icons(void) {
    for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
        if (g_desktop.icons[i].active) {
            draw_desktop_icon(&g_desktop.icons[i]);
        }
    }
}

// Get desktop icon at screen coordinates
int desktop_get_icon_at(int x, int y) {
    for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
        if (!g_desktop.icons[i].active) continue;

        int32_t icon_x, icon_y;
        desktop_icon_get_position(g_desktop.icons[i].grid_x,
                                   g_desktop.icons[i].grid_y,
                                   &icon_x, &icon_y);

        // Check if point is within icon bounds (including label area)
        int total_height = DESKTOP_ICON_SIZE + DESKTOP_ICON_LABEL_H;
        if (x >= icon_x && x < icon_x + DESKTOP_ICON_SIZE &&
            y >= icon_y && y < icon_y + total_height) {
            return i;
        }
    }
    return -1;
}

// Handle double-click on desktop icon
void desktop_icon_activate(int index) {
    if (index < 0 || index >= DESKTOP_ICON_MAX) return;
    if (!g_desktop.icons[index].active) return;

    desktop_icon_t *icon = &g_desktop.icons[index];
    kprintf("[Desktop] Activating icon: %s\n", icon->name);

    if (icon->launch) {
        icon->launch();
    }
}

// Initialize default desktop icons
static void desktop_init_default_icons(void) {
    // Clear any existing icons
    for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
        g_desktop.icons[i].active = false;
    }
    g_desktop.icon_count = 0;

    // Add default desktop icons
    extern void filebrowser_launch(void);
    extern void recyclebin_launch(void);
    extern void terminal_launch(void);
    extern void settings_launch(void);

    desktop_add_icon("Computer",     ICON_FOLDER,    0, 0, filebrowser_launch);
    desktop_add_icon("Recycle Bin",  ICON_TRASH,     0, 1, recyclebin_launch);
    desktop_add_icon("Terminal",     ICON_TERMINAL,  0, 2, terminal_launch);
    desktop_add_icon("Settings",     ICON_COG,       0, 3, settings_launch);

    kprintf("[Desktop] Initialized %u default desktop icons\n", g_desktop.icon_count);
}

void desktop_init(void) {
    LOG_INFO("[Desktop] Initializing desktop manager");
    kprintf("[Desktop] g_start_menu_open at init start: %d\n", g_start_menu_open);
    g_start_menu_open = false;  // Start menu closed by default
    // Initialize TrueType font rendering
    ttf_init();
    ttf_selfcheck_digits();  // #302: verify digits 0-9 (esp. '7') render OK

    kprintf("[Desktop] Initializing desktop manager...\n");

    // THE GLOBAL UI SCALE FACTOR, BEFORE THE THEME SYSTEM.
    //
    // Order matters and is not arbitrary. theme_get_metric_by_id() applies the
    // factor to every metric it returns, and theme_init() below loads the
    // themes and takes contrast measurements over them. Bringing the scale up
    // first means there is never a window in which a metric is read at a
    // different factor than the one that is about to be in force.
    //
    // This point in boot also satisfies both of uiscale_init()'s preconditions:
    // the framebuffer is up (fb_get_width/height are real), and the root
    // filesystem is mounted, so /CONFIG/DISPLAY.CFG is readable. Reading it
    // earlier is what makes the first-run wizard readable, which is the whole
    // reported bug: the wizard runs before any USER exists, so its scale can
    // only come from a machine-wide default that is already in force by the
    // time the compositor spawns it.
    uiscale_init((int)fb_get_width(), (int)fb_get_height());

    // #battmeter: battery presence reuses uiscale_is_laptop() above it, so
    // this must run AFTER uiscale_init(). See drivers/battery.h.
    battery_init();

    // Initialize the theme system
    theme_init();

    // Clear desktop state
    memset(&g_desktop, 0, sizeof(desktop_t));
    memset(&g_bg_image_data, 0, sizeof(g_bg_image_data));

    // Get screen dimensions from framebuffer
    g_desktop.screen_width = fb_get_width();
    g_desktop.screen_height = fb_get_height();
    kprintf("[Desktop] Screen resolution: %ux%u\n", g_desktop.screen_width, g_desktop.screen_height);

    // Set default background color
    g_desktop.bg_color = DESKTOP_BG_COLOR;
    g_desktop.bg_image = NULL;

    // Try to load background image
    load_background_image();

    // Initialize dock
    g_desktop.dock.app_count = 0;
    g_desktop.dock.hover_index = -1;
    g_desktop.dock.visible = true;

    // Calculate dock position (must be done after screen dimensions are set)
    dock_recalculate();

    // Dock apps are added dynamically when launched from start menu
    // and removed when the application closes

    // Initialize desktop icons
    desktop_init_default_icons();

    // Context menu no longer needs Applications submenu (now has direct items)


    // #552: kernel start menu category init removed along with the menu.
    g_desktop.initialized = true;
    kprintf("[Desktop] g_start_menu_open at init end: %d\n", g_start_menu_open);

    kprintf("[Desktop] Desktop initialized: %ux%u, %u dock apps\n",
            g_desktop.screen_width, g_desktop.screen_height, g_desktop.dock.app_count);
}

void desktop_draw(void) {
    if (!g_desktop.initialized) {
        kprintf("[Desktop] Error: desktop not initialized\n");
        return;
    }

    // Draw background
    if (g_bg_image_data.pixels != NULL && g_bg_image_data.width > 0) {
        // Draw background image scaled to fill the entire screen
        image_blit_scaled(&g_bg_image_data, 0, 0,
                         g_desktop.screen_width, g_desktop.screen_height);
    } else if (g_desktop.bg_image && g_desktop.bg_image->pixels) {
        // Draw background image scaled to fill (legacy path)
        image_blit_scaled(g_desktop.bg_image, 0, 0,
                         g_desktop.screen_width, g_desktop.screen_height);
    } else {
        // Draw solid color background
        uint32_t fb_color = argb_to_fb(g_desktop.bg_color);
        fb_clear(fb_color);
    }

    // Draw desktop icons (before dock so they appear behind it)
    desktop_draw_icons();

    // Draw dock
    if (g_desktop.dock.visible) {
        dock_draw();
    }

    // Note: Start menu is now drawn AFTER windows (see desktop_run)
    // so it appears on top of all windows

    // Note: Context menu is now drawn AFTER windows (see desktop_run)
    // so it appears on top of all windows

    // Draw wallpaper picker if open (drawn directly, not using window manager)
    if (g_wallpaper_picker_open) {
        draw_wallpaper_picker();
    }

    // Draw screensaver settings if open
    if (g_ss_settings_open) {
        draw_screensaver_settings();
    }

    // Draw clock widget in top-right corner
    desktop_clock_draw();

    // Note: Version info moved to desktop_run() main loop so it appears on top of windows
}

void desktop_set_background_color(uint32_t color) {
    g_desktop.bg_color = color;
    kprintf("[Desktop] Background color set to 0x%08X\n", color);
}

void desktop_set_background_image(image_t *img) {
    g_desktop.bg_image = img;
    if (img) {
        kprintf("[Desktop] Background image set: %ux%u\n", img->width, img->height);
    } else {
        kprintf("[Desktop] Background image cleared\n");
    }
}

void desktop_handle_click(int x, int y) {
    if (!g_desktop.initialized) return;

    // Check if screensaver settings is open first
    if (g_ss_settings_open) {
        // Check if click is on the dialog
        if (x >= g_ss_settings_x && x < g_ss_settings_x + SS_SETTINGS_WIDTH &&
            y >= g_ss_settings_y && y < g_ss_settings_y + SS_SETTINGS_HEIGHT) {

            int item = get_ss_settings_item_at(x, y);
            if (item == -2) {
                // Close button clicked
                close_screensaver_settings();
                return;
            }
            if (item >= 0 && item < SCREENSAVER_COUNT) {
                // Type selected
                kprintf("[Desktop] Screensaver type selected: %s\n",
                        screensaver_get_type_name((screensaver_type_t)item));
                screensaver_set_type((screensaver_type_t)item);
                if (item == SCREENSAVER_NONE) {
                    screensaver_set_enabled(false);
                } else {
                    screensaver_set_enabled(true);
                }
                wm_invalidate_all();
                return;
            }
            if (item >= 10 && item < 20) {
                // Timeout selected
                int timeout_idx = item - 10;
                uint32_t timeout = g_ss_timeouts[timeout_idx];
                kprintf("[Desktop] Screensaver timeout selected: %s\n", g_ss_timeout_names[timeout_idx]);
                if (timeout == 0) {
                    screensaver_set_enabled(false);
                } else {
                    screensaver_set_timeout(timeout);
                    screensaver_set_enabled(true);
                }
                wm_invalidate_all();
                return;
            }
            // Click on dialog but not on item - do nothing
            return;
        }
        // Click outside dialog - close it
        close_screensaver_settings();
        return;
    }

    // Check if wallpaper picker is open first
    if (g_wallpaper_picker_open) {
        // Check if click is on the picker
        if (x >= g_picker_x && x < g_picker_x + PICKER_WIDTH &&
            y >= g_picker_y && y < g_picker_y + PICKER_HEIGHT) {

            int item = get_wallpaper_item_at(x, y);
            if (item == -2) {
                // Close button clicked
                close_wallpaper_picker();
                return;
            }
            if (item == -3) {
                // Scroll up
                if (g_picker_scroll > 0) {
                    g_picker_scroll--;
                    wm_invalidate_all();
                }
                return;
            }
            if (item == -4) {
                // Scroll down
                if (g_picker_scroll < g_picker_max_scroll) {
                    g_picker_scroll++;
                    wm_invalidate_all();
                }
                return;
            }
            if (item >= 0) {
                // Wallpaper selected
                kprintf("[Desktop] Wallpaper selected: %s\n", g_wallpapers[item].name);
                g_current_wallpaper_index = item;  // Track current selection
                set_wallpaper(g_wallpapers[item].filename);
                close_wallpaper_picker();
                return;
            }
            // Click on picker but not on item - do nothing
            return;
        }
        // Click outside picker - close it
        close_wallpaper_picker();
        return;
    }

    // Check if context menu is open
    if (g_context_menu_open) {
        int item = get_context_menu_item_at(x, y);
        if (item == -2) {
            // Submenu item clicked - g_clicked_item has the index
            menu_item_t *submenu = g_context_items[g_context_submenu_open].submenu;
            if (submenu && submenu[g_clicked_item].name) {
                kprintf("[Desktop] Submenu item clicked: %s\n", submenu[g_clicked_item].name);
                g_context_menu_open = false;
                g_context_submenu_open = -1;
                wm_invalidate_all();
                if (submenu[g_clicked_item].action) {
                    submenu[g_clicked_item].action();
                }
            }
            return;
        }
        if (item >= 0) {
            // Main menu item clicked
            if (g_context_items[item].submenu) {
                // Has submenu - toggle it open (already handled by get_context_menu_item_at)
                wm_invalidate_all();
                return;
            }
            kprintf("[Desktop] Context menu item clicked: %s\n", g_context_items[item].name);
            g_context_menu_open = false;
            g_context_submenu_open = -1;
            if (g_context_items[item].action) {
                g_context_items[item].action();
            }
            wm_invalidate_all();
            return;
        }
        // Click outside menu - close it
        g_context_menu_open = false;
        g_context_submenu_open = -1;
        wm_invalidate_all();
        return;
    }

    // #552: kernel start menu removed (click-on-menu-item and
    // click-on-start-button branches both gone; see the block comment
    // above g_start_menu_open near the top of this file).

    // Check if click is on dock app
    int app_index = dock_get_app_at(x, y);
    if (app_index >= 0) {
        dock_app_t *app = &g_desktop.dock.apps[app_index];
        kprintf("[Desktop] Dock app clicked: %s\n", app->name);

        if (app->launch) {
            app->launch();
        } else {
            kprintf("[Desktop] No launch handler for %s\n", app->name);
        }
        return;
    }

    // Check if click is on a desktop icon (double-click to activate)
    static uint64_t last_icon_click_time = 0;
    static int last_icon_clicked = -1;
    extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;  // Current timer frequency

    int icon_index = desktop_get_icon_at(x, y);
    if (icon_index >= 0) {
        // Deselect all icons first
        for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
            g_desktop.icons[i].selected = false;
        }

        // Select the clicked icon
        g_desktop.icons[icon_index].selected = true;

        // Check for double-click (within 500ms at 1000Hz)
        uint64_t current_time = timer_ticks;
        if (icon_index == last_icon_clicked && (current_time - last_icon_click_time) < (g_timer_hz / 2)) {
            // Double-click - activate the icon
            desktop_icon_activate(icon_index);
            g_desktop.icons[icon_index].selected = false;
            last_icon_clicked = -1;
        } else {
            // Single click - just select
            last_icon_click_time = current_time;
            last_icon_clicked = icon_index;
        }
        wm_invalidate_all();
        return;
    }

    // Clicked on empty desktop - deselect all icons
    for (int i = 0; i < DESKTOP_ICON_MAX; i++) {
        g_desktop.icons[i].selected = false;
    }
    wm_invalidate_all();
}

// Calculate context menu height accounting for separators
static int calc_context_menu_height(void) {
    int height = 8;  // Padding
    for (int i = 0; g_context_items[i].name; i++) {
        if (strcmp(g_context_items[i].name, "---") == 0) {
            height += CONTEXT_MENU_SEP_H;
        } else {
            height += CONTEXT_MENU_ITEM_H;
        }
    }
    return height;
}

// Handle right-click on desktop (context menu)
void desktop_handle_right_click(int x, int y) {
    if (!g_desktop.initialized) return;

    // Close any open menus
    g_start_menu_open = false;
    close_wallpaper_picker();
    close_screensaver_settings();

    // Don't show context menu if clicking on dock
    if (dock_hit_test(x, y)) {
        return;
    }

    // Open context menu at click position
    g_context_menu_open = true;
    g_context_submenu_open = -1;  // Reset submenu state
    g_context_menu_x = x;
    g_context_menu_y = y;

    // Make sure menu doesn't go off screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();

    int menu_height = calc_context_menu_height();

    // Clamp X position
    if (g_context_menu_x + CONTEXT_MENU_WIDTH > (int32_t)screen_w) {
        g_context_menu_x = screen_w - CONTEXT_MENU_WIDTH;
    }
    if (g_context_menu_x < 0) {
        g_context_menu_x = 0;
    }

    // Clamp Y position
    if (g_context_menu_y + menu_height > (int32_t)screen_h - TASKBAR_HEIGHT) {
        g_context_menu_y = screen_h - TASKBAR_HEIGHT - menu_height;
    }
    if (g_context_menu_y < 0) {
        g_context_menu_y = 0;
    }

    kprintf("[Desktop] Context menu opened at (%d, %d)\n", g_context_menu_x, g_context_menu_y);
    wm_invalidate_all();
}

void desktop_handle_mouse_move(int x, int y) {
    if (!g_desktop.initialized) return;

    // Handle context menu hover (for submenu expansion)
    if (g_context_menu_open) {
        int old_submenu = g_context_submenu_open;
        get_context_menu_item_at(x, y);  // This updates g_context_submenu_open
        if (old_submenu != g_context_submenu_open) {
            wm_invalidate_all();
        }
    }

    int old_hover = g_desktop.dock.hover_index;
    g_desktop.dock.hover_index = dock_get_app_at(x, y);

    // Invalidate dock area if hover state changed
    if (old_hover != g_desktop.dock.hover_index) {
        rect_t dock_rect = {
            g_desktop.dock.x,
            g_desktop.dock.y,
            (int32_t)g_desktop.dock.width,
            (int32_t)g_desktop.dock.height
        };
        wm_invalidate_rect(&dock_rect);
    }
}

// ============================================================================
// Dock API Implementation
// ============================================================================

int dock_add_app_with_icon(const char *name, uint32_t color, int icon_id, void (*launch)(void)) {
    dock_t *dock = &g_desktop.dock;

    if (dock->app_count >= DOCK_MAX_APPS) {
        kprintf("[Dock] Error: dock is full\n");
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
        return -1;
    }

    // Find first empty slot
    int index = -1;
    for (int i = 0; i < DOCK_MAX_APPS; i++) {
        if (!dock->apps[i].active) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        kprintf("[Dock] Error: no free slots\n");
        return -1;
    }

    // Initialize the app entry
    dock_app_t *app = &dock->apps[index];
    memset(app, 0, sizeof(dock_app_t));

    strncpy(app->name, name, DOCK_APP_NAME_LEN - 1);
    app->name[DOCK_APP_NAME_LEN - 1] = '\0';
    app->icon_color = color;
    app->icon_id = icon_id;
    app->launch = launch;
    app->active = true;

    dock->app_count++;

    // Recalculate dock layout
    dock_recalculate();

    kprintf("[Dock] Added app: %s (index %d, icon %d)\n", name, index, icon_id);
    return index;
}

// Backward compatible function - auto-assigns icon based on name
int dock_add_app(const char *name, uint32_t color, void (*launch)(void)) {
    int icon_id = -1;

    // Auto-assign icons based on app name
    if (strcmp(name, "Terminal") == 0) {
        icon_id = ICON_TERMINAL;
    } else if (strcmp(name, "Editor") == 0) {
        icon_id = ICON_HIGHLIGHT;
    } else if (strcmp(name, "Files") == 0) {
        icon_id = ICON_FOLDER;
    } else if (strcmp(name, "Calculator") == 0) {
        icon_id = ICON_CALCULATOR;
    } else if (strcmp(name, "Settings") == 0) {
        icon_id = ICON_COG;
    } else if (strcmp(name, "About") == 0) {
        icon_id = ICON_INFO_CIRCLE;
    }

    return dock_add_app_with_icon(name, color, icon_id, launch);
}

void dock_remove_app(int index) {
    dock_t *dock = &g_desktop.dock;

    if (index < 0 || index >= DOCK_MAX_APPS) return;
    if (!dock->apps[index].active) return;
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }

    kprintf("[Dock] Removed app: %s\n", dock->apps[index].name);

    dock->apps[index].active = false;
    dock->app_count--;

    // Recalculate dock layout
    dock_recalculate();
}

void dock_clear(void) {
    dock_t *dock = &g_desktop.dock;

    for (int i = 0; i < DOCK_MAX_APPS; i++) {
        dock->apps[i].active = false;
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
    }
    dock->app_count = 0;
    dock->hover_index = -1;

    dock_recalculate();
    kprintf("[Dock] Cleared all apps\n");
}

// #552: draw_start_menu() and its helpers (draw_start_menu_search/_recent/
// _grid/_list), get_start_menu_item_at(), and is_start_button_click() were
// removed here along with the rest of the kernel start menu. do_shutdown()
// and do_restart() were removed too: they had no caller left once the start
// menu's Power buttons were gone (the compositor has its own shutdown path).

// ============================================================================
// Context Menu Implementation
// ============================================================================

static void draw_context_menu(void) {
    if (!g_context_menu_open) return;

    // Count items and calculate height (accounting for separators)
    int item_count = 0;
    int menu_height = 8;  // Padding
    while (g_context_items[item_count].name) {
        if (strcmp(g_context_items[item_count].name, "---") == 0) {
            menu_height += CONTEXT_MENU_SEP_H;  // Separator height
        } else {
            menu_height += CONTEXT_MENU_ITEM_H;  // Normal item height
        }
        item_count++;
    }

    kprintf("[ContextMenu] Drawing context menu with %d items at (%d,%d), height=%d\n",
            item_count, g_context_menu_x, g_context_menu_y, menu_height);
    for (int i = 0; i < item_count; i++) {
        kprintf("[ContextMenu] Item %d: '%s'\n", i, g_context_items[i].name);
    }

    // Draw menu background
    fb_fill_rect(g_context_menu_x, g_context_menu_y, CONTEXT_MENU_WIDTH, menu_height, argb_to_fb(0xFF303030));
    fb_draw_rect(g_context_menu_x, g_context_menu_y, CONTEXT_MENU_WIDTH, menu_height, argb_to_fb(0xFF606060));

    // Draw items
    int y = g_context_menu_y + 4;
    for (int i = 0; g_context_items[i].name; i++) {
        // Check for separator
        if (strcmp(g_context_items[i].name, "---") == 0) {
            // Draw separator line
            fb_fill_rect(g_context_menu_x + 8, y + CONTEXT_MENU_SEP_H / 2 - 1,
                        CONTEXT_MENU_WIDTH - 16, 1, argb_to_fb(0xFF505050));
            y += CONTEXT_MENU_SEP_H;
            continue;
        }

        // Highlight if this submenu is open
        if (i == g_context_submenu_open) {
            fb_fill_rect(g_context_menu_x + 2, y, CONTEXT_MENU_WIDTH - 4, CONTEXT_MENU_ITEM_H - 2, argb_to_fb(0xFF505050));
        }

        // Draw item name
        draw_string(g_context_menu_x + 10, y + (CONTEXT_MENU_ITEM_H - FONT_HEIGHT) / 2,
                   g_context_items[i].name, argb_to_fb(0xFFFFFFFF));

        // Draw arrow indicator if has submenu
        if (g_context_items[i].submenu) {
            draw_string(g_context_menu_x + CONTEXT_MENU_WIDTH - 16, y + (CONTEXT_MENU_ITEM_H - FONT_HEIGHT) / 2,
                       ">", argb_to_fb(0xFFAAAAAA));
        }

        y += CONTEXT_MENU_ITEM_H;
    }

    // Draw submenu if open
    if (g_context_submenu_open >= 0 && g_context_items[g_context_submenu_open].submenu) {
        menu_item_t *submenu = g_context_items[g_context_submenu_open].submenu;
        int sub_count = g_context_items[g_context_submenu_open].submenu_count;

        // Position submenu to the right of main menu
        int sub_x = g_context_menu_x + CONTEXT_MENU_WIDTH - 2;
        int sub_y = g_context_menu_y + 4 + g_context_submenu_open * CONTEXT_MENU_ITEM_H;
        int sub_height = sub_count * CONTEXT_MENU_ITEM_H + 8;

        // Adjust if submenu would go off screen
        uint32_t screen_w = fb_get_width();
        uint32_t screen_h = fb_get_height();
        if (sub_x + CONTEXT_MENU_WIDTH > (int32_t)screen_w) {
            sub_x = g_context_menu_x - CONTEXT_MENU_WIDTH + 2;  // Show on left side
        }
        if (sub_y + sub_height > (int32_t)screen_h - TASKBAR_HEIGHT) {
            sub_y = screen_h - TASKBAR_HEIGHT - sub_height;
        }

        // Draw submenu background
        fb_fill_rect(sub_x, sub_y, CONTEXT_MENU_WIDTH, sub_height, argb_to_fb(0xFF303030));
        fb_draw_rect(sub_x, sub_y, CONTEXT_MENU_WIDTH, sub_height, argb_to_fb(0xFF606060));

        // Draw submenu items
        int sub_item_y = sub_y + 4;
        for (int i = 0; i < sub_count && submenu[i].name; i++) {
            // Draw icon if available
            if (submenu[i].icon_id >= 0) {
                int icon_size = 16;
                int icon_x = sub_x + 6;
                int icon_y = sub_item_y + (CONTEXT_MENU_ITEM_H - icon_size) / 2;
                icon_draw_scaled(submenu[i].icon_id, icon_x, icon_y, icon_size, 0xDDDDDD);
            }

            // Draw item name
            draw_string(sub_x + 28, sub_item_y + (CONTEXT_MENU_ITEM_H - FONT_HEIGHT) / 2,
                       submenu[i].name, argb_to_fb(0xFFFFFFFF));

            sub_item_y += CONTEXT_MENU_ITEM_H;
        }
    }
}

// Returns: -1 = outside, -2 = on submenu item (sets g_clicked_item), >= 0 = main menu item index
static int get_context_menu_item_at(int x, int y) {
    if (!g_context_menu_open) return -1;

    int menu_height = calc_context_menu_height();

    // First check if we're in an open submenu
    if (g_context_submenu_open >= 0 && g_context_items[g_context_submenu_open].submenu) {
        menu_item_t *submenu = g_context_items[g_context_submenu_open].submenu;
        int sub_count = g_context_items[g_context_submenu_open].submenu_count;

        // Calculate Y offset for submenu item (accounting for separators before it)
        int submenu_y_offset = 4;
        for (int i = 0; i < g_context_submenu_open; i++) {
            if (strcmp(g_context_items[i].name, "---") == 0) {
                submenu_y_offset += CONTEXT_MENU_SEP_H;
            } else {
                submenu_y_offset += CONTEXT_MENU_ITEM_H;
            }
        }

        // Calculate submenu position (same as in draw)
        int sub_x = g_context_menu_x + CONTEXT_MENU_WIDTH - 2;
        int sub_y = g_context_menu_y + submenu_y_offset;
        int sub_height = sub_count * CONTEXT_MENU_ITEM_H + 8;

        // Adjust if off screen
        uint32_t screen_w = fb_get_width();
        uint32_t screen_h = fb_get_height();
        if (sub_x + CONTEXT_MENU_WIDTH > (int32_t)screen_w) {
            sub_x = g_context_menu_x - CONTEXT_MENU_WIDTH + 2;
        }
        if (sub_y + sub_height > (int32_t)screen_h - TASKBAR_HEIGHT) {
            sub_y = screen_h - TASKBAR_HEIGHT - sub_height;
        }

        // Check if click is in submenu
        if (x >= sub_x && x < sub_x + CONTEXT_MENU_WIDTH &&
            y >= sub_y && y < sub_y + sub_height) {

            int sub_item_y = sub_y + 4;
            for (int i = 0; i < sub_count && submenu[i].name; i++) {
                if (y >= sub_item_y && y < sub_item_y + CONTEXT_MENU_ITEM_H) {
                    g_clicked_item = i;  // Store which submenu item was clicked
                    return -2;  // Signal that a submenu item was clicked
                }
                sub_item_y += CONTEXT_MENU_ITEM_H;
            }
            return -1;  // In submenu area but not on item
        }
    }

    // Check main menu bounds
    if (x < g_context_menu_x || x >= g_context_menu_x + CONTEXT_MENU_WIDTH) {
        // If outside main menu and not in submenu, close submenu
        g_context_submenu_open = -1;
        return -1;
    }
    if (y < g_context_menu_y || y >= g_context_menu_y + menu_height) {
        return -1;
    }

    // Find which main menu item (accounting for separators)
    int item_y = g_context_menu_y + 4;
    for (int i = 0; g_context_items[i].name; i++) {
        // Check for separator
        if (strcmp(g_context_items[i].name, "---") == 0) {
            item_y += CONTEXT_MENU_SEP_H;
            continue;  // Can't click on separator
        }

        if (y >= item_y && y < item_y + CONTEXT_MENU_ITEM_H) {
            // Open/close submenu on hover
            if (g_context_items[i].submenu) {
                g_context_submenu_open = i;
            } else {
                g_context_submenu_open = -1;
            }
            return i;
        }
        item_y += CONTEXT_MENU_ITEM_H;
    }

    return -1;
}

// ============================================================================
// Wallpaper Picker Implementation
// ============================================================================

/**
 * Draw a thumbnail image at the specified position
 */
static void draw_thumbnail(int index, int x, int y, bool selected) {
    if (index < 0 || index >= MAX_WALLPAPERS) return;

    thumbnail_t *thumb = &g_thumbnails[index];

    // Draw selection border if selected (currently set wallpaper)
    if (selected) {
        // Draw highlight border around the cell
        fb_fill_rect(x - 2, y - 2, THUMB_WIDTH + 4, THUMB_HEIGHT + 4, argb_to_fb(0xFF4080FF));
    }

    // Draw thumbnail background (dark gray if no thumbnail loaded)
    fb_fill_rect(x, y, THUMB_WIDTH, THUMB_HEIGHT, argb_to_fb(0xFF3A3A3A));

    if (thumb->loaded && thumb->pixels) {
        // Center the thumbnail if it's smaller than the cell
        int offset_x = (THUMB_WIDTH - thumb->width) / 2;
        int offset_y = (THUMB_HEIGHT - thumb->height) / 2;

        // Draw thumbnail pixels directly to framebuffer
        for (uint32_t py = 0; py < thumb->height; py++) {
            uint32_t *src_row = thumb->pixels + py * thumb->width;
            for (uint32_t px = 0; px < thumb->width; px++) {
                fb_put_pixel(x + offset_x + px, y + offset_y + py, src_row[px]);
            }
        }
    } else {
        // Draw "Loading..." text if not loaded
        draw_string_small(x + 8, y + THUMB_HEIGHT / 2 - 4, "Load...", argb_to_fb(0xFF808080));
    }

    // Draw border around thumbnail
    fb_draw_rect(x, y, THUMB_WIDTH, THUMB_HEIGHT, argb_to_fb(0xFF505050));
}

/**
 * Get a short display name for a wallpaper (truncate if needed)
 */
static void get_short_name(const char *name, char *buf, int max_len) {
    int len = 0;
    while (name[len]) len++;

    if (len <= max_len) {
        for (int i = 0; i <= len; i++) buf[i] = name[i];
    } else {
        // Truncate and add "..."
        for (int i = 0; i < max_len - 3; i++) buf[i] = name[i];
        buf[max_len - 3] = '.';
        buf[max_len - 2] = '.';
        buf[max_len - 1] = '.';
        buf[max_len] = '\0';
    }
}

static void draw_wallpaper_picker(void) {
    if (!g_wallpaper_picker_open) return;

    // Draw picker background (dark panel with border)
    fb_fill_rect(g_picker_x, g_picker_y, PICKER_WIDTH, PICKER_HEIGHT, argb_to_fb(0xFF2D2D2D));
    fb_draw_rect(g_picker_x, g_picker_y, PICKER_WIDTH, PICKER_HEIGHT, argb_to_fb(0xFF505050));

    // Draw title bar
    fb_fill_rect(g_picker_x + 2, g_picker_y + 2, PICKER_WIDTH - 4, 20, argb_to_fb(0xFF3A3A3A));
    draw_string(g_picker_x + 8, g_picker_y + 6, "Choose Wallpaper", argb_to_fb(0xFFE0E0E0));

    // Draw close button
    int close_x = g_picker_x + PICKER_WIDTH - 18;
    int close_y = g_picker_y + 4;
    fb_fill_rect(close_x, close_y, 14, 14, argb_to_fb(0xFF606060));
    draw_string(close_x + 3, close_y + 2, "X", argb_to_fb(0xFFFFFFFF));

    // Draw scroll indicators if needed
    if (g_picker_max_scroll > 0) {
        // Scroll up indicator
        if (g_picker_scroll > 0) {
            draw_string(g_picker_x + PICKER_WIDTH / 2 - 8, g_picker_y + 6, "^", argb_to_fb(0xFFFFFFFF));
        }
        // Scroll down indicator
        if (g_picker_scroll < g_picker_max_scroll) {
            draw_string(g_picker_x + PICKER_WIDTH / 2 - 8, g_picker_y + PICKER_HEIGHT - 12, "v", argb_to_fb(0xFFFFFFFF));
        }
    }

    // Content area
    int content_x = g_picker_x + THUMB_PADDING;
    int content_y = g_picker_y + PICKER_TITLE_H;

    // Count wallpapers
    int count = 0;
    for (int i = 0; g_wallpapers[i].name; i++) count++;

    // Draw thumbnails in grid
    int start_index = g_picker_scroll * THUMB_COLS;
    int visible_rows = PICKER_CONTENT_H / THUMB_CELL_H;
    int end_index = start_index + (visible_rows + 1) * THUMB_COLS;
    if (end_index > count) end_index = count;

    for (int i = start_index; i < end_index; i++) {
        int row = (i - start_index) / THUMB_COLS;
        int col = (i - start_index) % THUMB_COLS;

        int thumb_x = content_x + col * THUMB_CELL_W;
        int thumb_y = content_y + row * THUMB_CELL_H;

        // Skip if outside visible area
        if (thumb_y + THUMB_CELL_H > g_picker_y + PICKER_HEIGHT - 4) continue;

        // Check if this is the currently selected wallpaper
        bool selected = (i == g_current_wallpaper_index);

        // Draw the thumbnail
        draw_thumbnail(i, thumb_x, thumb_y, selected);

        // Draw name below thumbnail (using small font)
        char short_name[12];
        get_short_name(g_wallpapers[i].name, short_name, 11);
        int name_x = thumb_x + (THUMB_WIDTH - (int)strlen(short_name) * FONT_SMALL_WIDTH) / 2;
        draw_string_small(name_x, thumb_y + THUMB_HEIGHT + 2, short_name, argb_to_fb(0xFFCCCCCC));
    }
}

static int get_wallpaper_item_at(int x, int y) {
    if (!g_wallpaper_picker_open) return -1;

    // Check if click is on close button
    int close_x = g_picker_x + PICKER_WIDTH - 18;
    int close_y = g_picker_y + 4;
    if (x >= close_x && x < close_x + 14 && y >= close_y && y < close_y + 14) {
        return -2;  // Close button clicked
    }

    // Check scroll up area
    if (g_picker_scroll > 0 && y >= g_picker_y && y < g_picker_y + PICKER_TITLE_H) {
        if (x >= g_picker_x + PICKER_WIDTH / 2 - 20 && x < g_picker_x + PICKER_WIDTH / 2 + 20) {
            return -3;  // Scroll up
        }
    }

    // Check scroll down area
    if (g_picker_scroll < g_picker_max_scroll &&
        y >= g_picker_y + PICKER_HEIGHT - 16 && y < g_picker_y + PICKER_HEIGHT) {
        if (x >= g_picker_x + PICKER_WIDTH / 2 - 20 && x < g_picker_x + PICKER_WIDTH / 2 + 20) {
            return -4;  // Scroll down
        }
    }

    // Content area starts after title bar
    int content_x = g_picker_x + THUMB_PADDING;
    int content_y = g_picker_y + PICKER_TITLE_H;

    // Check if click is in content area
    if (x < content_x || x >= g_picker_x + PICKER_WIDTH - THUMB_PADDING ||
        y < content_y || y >= g_picker_y + PICKER_HEIGHT - 4) {
        return -1;  // Outside content area
    }

    // Calculate which grid cell was clicked
    int rel_x = x - content_x;
    int rel_y = y - content_y;

    int col = rel_x / THUMB_CELL_W;
    int row = rel_y / THUMB_CELL_H;

    // Check if click is within the thumbnail area (not in padding between cells)
    int cell_x = rel_x % THUMB_CELL_W;
    int cell_y = rel_y % THUMB_CELL_H;

    // Accept clicks on the thumbnail and the name label below it
    if (cell_x >= THUMB_CELL_W - THUMB_PADDING / 2 || cell_y >= THUMB_HEIGHT + 14) {
        return -1;  // Click in padding area
    }

    if (col < 0 || col >= THUMB_COLS) {
        return -1;  // Outside grid columns
    }

    // Calculate wallpaper index
    int index = (g_picker_scroll + row) * THUMB_COLS + col;

    // Count wallpapers to validate index
    int count = 0;
    for (int i = 0; g_wallpapers[i].name; i++) count++;

    if (index >= 0 && index < count) {
        return index;
    }

    return -1;
}

static void set_wallpaper(const char *filename) {
    kprintf("[Desktop] Setting wallpaper to: %s\n", filename ? filename : "(gradient)");

    // Free old background if exists
    if (g_bg_image_data.pixels) {
        kfree(g_bg_image_data.pixels);
        g_bg_image_data.pixels = NULL;
        g_desktop.bg_image = NULL;
    }

    // NULL filename means use gradient
    if (!filename) {
        generate_gradient_background();
        wm_invalidate_all();
        return;
    }

    // Get filesystem
    extern fat_fs_t g_fat_fs;
    fat_fs_t *fs_to_use = NULL;

    // Check if g_fat_fs is mounted
    if (g_fat_fs.bytes_per_sector > 0) {
        fs_to_use = &g_fat_fs;
    }

    if (!fs_to_use) {
        kprintf("[Desktop] No filesystem available\n");
        generate_gradient_background();
        return;
    }

    // Load the file from disk
    uint32_t size = 0;
    void *data = fat_read_file(fs_to_use, filename, &size);

    if (data && size > 54) {
        int result = image_load_bmp(data, size, &g_bg_image_data);
        if (result == IMAGE_SUCCESS) {
            g_desktop.bg_image = &g_bg_image_data;
            kprintf("[Desktop] Wallpaper loaded: %ux%u\n",
                    g_bg_image_data.width, g_bg_image_data.height);
            wm_invalidate_all();
        } else {
            kprintf("[Desktop] Failed to load BMP: %s\n", image_error_string(result));
            generate_gradient_background();
        }
        kfree(data);
    } else {
        kprintf("[Desktop] Failed to read file: %s\n", filename);
        generate_gradient_background();
    }
}

static void open_wallpaper_picker(void) {
    // Close context menu
    g_context_menu_open = false;

    // If already open, do nothing
    if (g_wallpaper_picker_open) {
        return;
    }

    // Center the picker on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    g_picker_x = (screen_w - PICKER_WIDTH) / 2;
    g_picker_y = (screen_h - PICKER_HEIGHT) / 2;

    // Reset scroll position
    g_picker_scroll = 0;

    // Generate thumbnails if not already cached
    generate_thumbnails();

    g_wallpaper_picker_open = true;
    kprintf("[Desktop] Wallpaper picker opened at %d,%d (size %dx%d)\n",
            g_picker_x, g_picker_y, PICKER_WIDTH, PICKER_HEIGHT);
    wm_invalidate_all();
}

static void close_wallpaper_picker(void) {
    g_wallpaper_picker_open = false;
    wm_invalidate_all();
}

// ============================================================================
// Thumbnail Generation Functions
// ============================================================================

/**
 * Scale an image down to thumbnail size using nearest-neighbor sampling.
 * Uses integer math only - no floating point.
 *
 * @param src_pixels Source pixel data (BGRA format)
 * @param src_w Source width
 * @param src_h Source height
 * @param out_w Output: actual thumbnail width
 * @param out_h Output: actual thumbnail height
 * @return Allocated thumbnail pixel buffer, or NULL on failure
 */
static uint32_t *scale_image_to_thumbnail(uint32_t *src_pixels, uint32_t src_w, uint32_t src_h,
                                          uint32_t *out_w, uint32_t *out_h) {
    if (!src_pixels || src_w == 0 || src_h == 0) {
        return NULL;
    }

    // Calculate scaled dimensions maintaining aspect ratio
    // Use integer math: scale = min(THUMB_WIDTH/src_w, THUMB_HEIGHT/src_h)
    // To avoid division, we compare cross-products:
    // THUMB_WIDTH/src_w < THUMB_HEIGHT/src_h  =>  THUMB_WIDTH*src_h < THUMB_HEIGHT*src_w
    uint32_t thumb_w, thumb_h;

    if (THUMB_WIDTH * src_h < THUMB_HEIGHT * src_w) {
        // Width is the limiting factor
        thumb_w = THUMB_WIDTH;
        // thumb_h = src_h * THUMB_WIDTH / src_w
        thumb_h = (src_h * THUMB_WIDTH) / src_w;
        if (thumb_h == 0) thumb_h = 1;
    } else {
        // Height is the limiting factor
        thumb_h = THUMB_HEIGHT;
        // thumb_w = src_w * THUMB_HEIGHT / src_h
        thumb_w = (src_w * THUMB_HEIGHT) / src_h;
        if (thumb_w == 0) thumb_w = 1;
    }

    // Allocate thumbnail buffer
    uint32_t *thumb_pixels = (uint32_t *)kmalloc(thumb_w * thumb_h * sizeof(uint32_t));
    if (!thumb_pixels) {
        return NULL;
    }

    // Use 16.16 fixed-point for scaling ratios
    uint32_t x_ratio = (src_w << 16) / thumb_w;
    uint32_t y_ratio = (src_h << 16) / thumb_h;

    // Nearest-neighbor scaling
    for (uint32_t y = 0; y < thumb_h; y++) {
        uint32_t src_y = (y * y_ratio) >> 16;
        if (src_y >= src_h) src_y = src_h - 1;

        uint32_t *src_row = src_pixels + src_y * src_w;
        uint32_t *dst_row = thumb_pixels + y * thumb_w;

        for (uint32_t x = 0; x < thumb_w; x++) {
            uint32_t src_x = (x * x_ratio) >> 16;
            if (src_x >= src_w) src_x = src_w - 1;
            dst_row[x] = src_row[src_x];
        }
    }

    *out_w = thumb_w;
    *out_h = thumb_h;
    return thumb_pixels;
}

/**
 * Generate thumbnail for a single wallpaper
 */
static void generate_thumbnail(int index) {
    if (index < 0 || index >= MAX_WALLPAPERS || !g_wallpapers[index].name) {
        return;
    }

    // Already loaded?
    if (g_thumbnails[index].loaded) {
        return;
    }

    const char *filename = g_wallpapers[index].filename;

    // Handle gradient (no file) - create a simple gradient thumbnail
    if (!filename) {
        uint32_t *pixels = (uint32_t *)kmalloc(THUMB_WIDTH * THUMB_HEIGHT * sizeof(uint32_t));
        if (pixels) {
            // Create a blue gradient similar to the desktop gradient
            for (uint32_t y = 0; y < THUMB_HEIGHT; y++) {
                // Gradient from dark blue (top) to lighter blue (bottom)
                uint32_t ratio = (y << 8) / THUMB_HEIGHT;  // 0-255
                uint8_t r = (uint8_t)((40 * ratio) >> 8);
                uint8_t g = (uint8_t)((80 + (40 * ratio)) >> 8);
                uint8_t b = (uint8_t)((120 + (60 * ratio)) >> 8);
                uint32_t color = (b << 16) | (g << 8) | r;  // BGRA format for framebuffer

                for (uint32_t x = 0; x < THUMB_WIDTH; x++) {
                    pixels[y * THUMB_WIDTH + x] = color;
                }
            }
            g_thumbnails[index].pixels = pixels;
            g_thumbnails[index].width = THUMB_WIDTH;
            g_thumbnails[index].height = THUMB_HEIGHT;
            g_thumbnails[index].loaded = true;
            kprintf("[Thumb] Generated gradient thumbnail for index %d\n", index);
        }
        return;
    }

    // Get filesystem
    extern fat_fs_t g_fat_fs;
    fat_fs_t *fs = NULL;
    if (g_fat_fs.bytes_per_sector > 0) {
        fs = &g_fat_fs;
    }

    if (!fs) {
        kprintf("[Thumb] No filesystem for thumbnail %d\n", index);
        return;
    }

    // Load the BMP file
    uint32_t size = 0;
    void *data = fat_read_file(fs, filename, &size);

    if (!data || size <= 54) {
        kprintf("[Thumb] Failed to read %s\n", filename);
        if (data) kfree(data);
        return;
    }

    // Parse the BMP
    image_t img;
    int result = image_load_bmp(data, size, &img);
    kfree(data);  // Free raw file data

    if (result != IMAGE_SUCCESS) {
        kprintf("[Thumb] Failed to parse BMP %s: %s\n", filename, image_error_string(result));
        return;
    }

    // Scale to thumbnail size
    uint32_t thumb_w, thumb_h;
    uint32_t *thumb_pixels = scale_image_to_thumbnail(img.pixels, img.width, img.height,
                                                       &thumb_w, &thumb_h);

    // Free full-size image
    image_free(&img);

    if (thumb_pixels) {
        g_thumbnails[index].pixels = thumb_pixels;
        g_thumbnails[index].width = thumb_w;
        g_thumbnails[index].height = thumb_h;
        g_thumbnails[index].loaded = true;
        kprintf("[Thumb] Generated %ux%u thumbnail for %s\n", thumb_w, thumb_h, filename);
    } else {
        kprintf("[Thumb] Failed to scale thumbnail for %s\n", filename);
    }
}

/**
 * Generate thumbnails for all wallpapers (called when picker opens)
 */
static void generate_thumbnails(void) {
    if (!g_thumbnails_initialized) {
        // Initialize cache
        for (int i = 0; i < MAX_WALLPAPERS; i++) {
            g_thumbnails[i].pixels = NULL;
            g_thumbnails[i].width = 0;
            g_thumbnails[i].height = 0;
            g_thumbnails[i].loaded = false;
        }
        g_thumbnails_initialized = true;
    }

    // Count wallpapers
    int count = 0;
    for (int i = 0; g_wallpapers[i].name; i++) {
        count++;
    }

    kprintf("[Thumb] Generating thumbnails for %d wallpapers...\n", count);

    // Generate thumbnails for all wallpapers
    for (int i = 0; i < count && i < MAX_WALLPAPERS; i++) {
        generate_thumbnail(i);
    }

    // Calculate max scroll
    int rows = (count + THUMB_COLS - 1) / THUMB_COLS;  // Ceiling division
    int visible_rows = PICKER_CONTENT_H / THUMB_CELL_H;
    g_picker_max_scroll = rows - visible_rows;
    if (g_picker_max_scroll < 0) g_picker_max_scroll = 0;

    kprintf("[Thumb] Thumbnail generation complete. Rows: %d, Visible: %d, MaxScroll: %d\n",
            rows, visible_rows, g_picker_max_scroll);
}

/**
 * Free all cached thumbnails (for future cleanup support)
 */
__attribute__((unused))
static void free_thumbnails(void) {
    for (int i = 0; i < MAX_WALLPAPERS; i++) {
        if (g_thumbnails[i].pixels) {
            kfree(g_thumbnails[i].pixels);
            g_thumbnails[i].pixels = NULL;
        }
        g_thumbnails[i].width = 0;
        g_thumbnails[i].height = 0;
        g_thumbnails[i].loaded = false;
    }
    kprintf("[Thumb] Thumbnails freed\n");
}

// ============================================================================
// Screensaver Settings Implementation
// ============================================================================

static void draw_screensaver_settings(void) {
    if (!g_ss_settings_open) return;

    // Get current config
    screensaver_config_t *cfg = screensaver_get_config();

    // Draw dialog background
    fb_fill_rect(g_ss_settings_x, g_ss_settings_y, SS_SETTINGS_WIDTH, SS_SETTINGS_HEIGHT, argb_to_fb(0xFF2D2D2D));
    fb_draw_rect(g_ss_settings_x, g_ss_settings_y, SS_SETTINGS_WIDTH, SS_SETTINGS_HEIGHT, argb_to_fb(0xFF505050));

    // Draw title bar
    fb_fill_rect(g_ss_settings_x + 2, g_ss_settings_y + 2, SS_SETTINGS_WIDTH - 4, 20, argb_to_fb(0xFF3A3A3A));
    draw_string(g_ss_settings_x + 8, g_ss_settings_y + 6, "Screensaver Settings", argb_to_fb(0xFFE0E0E0));

    // Draw close button
    int close_x = g_ss_settings_x + SS_SETTINGS_WIDTH - 18;
    int close_y = g_ss_settings_y + 4;
    fb_fill_rect(close_x, close_y, 14, 14, argb_to_fb(0xFF606060));
    draw_string(close_x + 3, close_y + 2, "X", argb_to_fb(0xFFFFFFFF));

    int content_y = g_ss_settings_y + 28;

    // Section: Screensaver Type
    draw_string(g_ss_settings_x + 8, content_y, "Type:", argb_to_fb(0xFF888888));
    content_y += 18;

    // Draw type options
    for (int i = SCREENSAVER_NONE; i < SCREENSAVER_COUNT; i++) {
        const char *name = screensaver_get_type_name((screensaver_type_t)i);
        bool selected = (cfg->type == (screensaver_type_t)i);

        // Highlight selected item
        if (selected) {
            fb_fill_rect(g_ss_settings_x + 6, content_y, SS_SETTINGS_WIDTH - 12, SS_ITEM_H - 2, argb_to_fb(0xFF4060A0));
        } else {
            fb_fill_rect(g_ss_settings_x + 6, content_y, SS_SETTINGS_WIDTH - 12, SS_ITEM_H - 2, argb_to_fb(0xFF404040));
        }

        // Draw radio button indicator
        uint32_t radio_color = selected ? 0xFF00CC00 : 0xFF808080;
        fb_fill_rect(g_ss_settings_x + 12, content_y + 6, 8, 8, argb_to_fb(radio_color));

        draw_string(g_ss_settings_x + 26, content_y + (SS_ITEM_H - FONT_HEIGHT) / 2, name, argb_to_fb(0xFFFFFFFF));
        content_y += SS_ITEM_H;
    }

    content_y += 10;

    // Section: Timeout
    draw_string(g_ss_settings_x + 8, content_y, "Wait:", argb_to_fb(0xFF888888));
    content_y += 18;

    // Draw timeout options
    for (int i = 0; g_ss_timeout_names[i]; i++) {
        bool selected = (cfg->timeout_seconds == g_ss_timeouts[i]) ||
                       (g_ss_timeouts[i] == 0 && !cfg->enabled);

        // Highlight selected item
        if (selected) {
            fb_fill_rect(g_ss_settings_x + 6, content_y, SS_SETTINGS_WIDTH - 12, SS_ITEM_H - 2, argb_to_fb(0xFF4060A0));
        } else {
            fb_fill_rect(g_ss_settings_x + 6, content_y, SS_SETTINGS_WIDTH - 12, SS_ITEM_H - 2, argb_to_fb(0xFF404040));
        }

        // Draw radio button indicator
        uint32_t radio_color = selected ? 0xFF00CC00 : 0xFF808080;
        fb_fill_rect(g_ss_settings_x + 12, content_y + 6, 8, 8, argb_to_fb(radio_color));

        draw_string(g_ss_settings_x + 26, content_y + (SS_ITEM_H - FONT_HEIGHT) / 2, g_ss_timeout_names[i], argb_to_fb(0xFFFFFFFF));
        content_y += SS_ITEM_H;
    }
}

// Returns: -1 = outside, -2 = close button, 0-5 = type selection, 10-14 = timeout selection
static int get_ss_settings_item_at(int x, int y) {
    if (!g_ss_settings_open) return -1;

    // Check if click is on close button
    int close_x = g_ss_settings_x + SS_SETTINGS_WIDTH - 18;
    int close_y = g_ss_settings_y + 4;
    if (x >= close_x && x < close_x + 14 && y >= close_y && y < close_y + 14) {
        return -2;  // Close button clicked
    }

    // Check bounds
    if (x < g_ss_settings_x + 6 || x >= g_ss_settings_x + SS_SETTINGS_WIDTH - 6 ||
        y < g_ss_settings_y + 28 || y >= g_ss_settings_y + SS_SETTINGS_HEIGHT) {
        return -1;
    }

    int content_y = g_ss_settings_y + 28 + 18;  // After "Type:" label

    // Check type items
    for (int i = SCREENSAVER_NONE; i < SCREENSAVER_COUNT; i++) {
        if (y >= content_y && y < content_y + SS_ITEM_H) {
            return i;  // Return type index (0-5)
        }
        content_y += SS_ITEM_H;
    }

    content_y += 10 + 18;  // After gap and "Wait:" label

    // Check timeout items
    for (int i = 0; g_ss_timeout_names[i]; i++) {
        if (y >= content_y && y < content_y + SS_ITEM_H) {
            return 10 + i;  // Return 10+ for timeout selection
        }
        content_y += SS_ITEM_H;
    }

    return -1;
}

__attribute__((unused))
static void open_screensaver_settings(void) {
    // Close context menu
    g_context_menu_open = false;
    g_context_submenu_open = -1;

    // Close wallpaper picker if open
    g_wallpaper_picker_open = false;

    // Center on screen
    uint32_t screen_w = fb_get_width();
    uint32_t screen_h = fb_get_height();
    g_ss_settings_x = (screen_w - SS_SETTINGS_WIDTH) / 2;
    g_ss_settings_y = (screen_h - SS_SETTINGS_HEIGHT) / 2;

    // Initialize selection from current config
    screensaver_config_t *cfg = screensaver_get_config();
    g_ss_selected_type = cfg->type;

    g_ss_settings_open = true;
    kprintf("[Desktop] Screensaver settings opened\n");
    wm_invalidate_all();
}

static void close_screensaver_settings(void) {
    g_ss_settings_open = false;
    wm_invalidate_all();
}

// Draw a gauge bar with label and value (uses regular 8x16 font for readability)
static void draw_gauge(int32_t x, int32_t y, int32_t width, int32_t height,
                       int percent, uint32_t color, const char *label, const char *value) {
    // Background
    fb_fill_rect(x, y, width, height, argb_to_fb(GAUGE_BG_COLOR));

    // Fill based on percentage
    int fill_width = (width * percent) / 100;
    if (fill_width > 0) {
        fb_fill_rect(x, y, fill_width, height, argb_to_fb(color));
    }

    // Border
    fb_draw_rect(x, y, width, height, argb_to_fb(0xFF606060));

    // Draw label on left, value on right (using regular font for better readability)
    int text_y = y + (height - FONT_HEIGHT) / 2;
    if (label) {
        draw_string(x + 3, text_y, label, argb_to_fb(0xFFFFFFFF));
    }
    if (value) {
        int value_width = strlen(value) * FONT_WIDTH;
        draw_string(x + width - value_width - 3, text_y, value, argb_to_fb(0xFFFFFFFF));
    }
}

void dock_draw(void) {
    dock_t *dock = &g_desktop.dock;
    if (!dock->visible) return;

    // Draw taskbar background (full width, square edges)
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
    fb_fill_rect(dock->x, dock->y, dock->width, dock->height, argb_to_fb(TASKBAR_BG_COLOR));

    // Draw top border line
    fb_fill_rect(dock->x, dock->y, dock->width, 1, argb_to_fb(0xFF606060));

    // ========== START BUTTON (with categories icon) ==========
    int32_t start_size = TASKBAR_HEIGHT - 4;  // Square button
    int32_t start_x = dock->x + TASKBAR_PADDING;
    int32_t start_y = dock->y + 2;

    // Start button background (square)
    fb_fill_rect(start_x, start_y, start_size, start_size, argb_to_fb(START_BTN_COLOR));

    // Draw categories icon (centered in button)
    int icon_size = 20;
    int icon_x = start_x + (start_size - icon_size) / 2;
    int icon_y = start_y + (start_size - icon_size) / 2;
    icon_draw_scaled(ICON_CATEGORIES, icon_x, icon_y, icon_size, 0xFFFFFF);

    // ========== APP BUTTONS (square with thumbnail) ==========
    int32_t app_x = dock->x + START_BTN_WIDTH + TASKBAR_PADDING;
    int32_t btn_y = dock->y + (TASKBAR_HEIGHT - PROCESS_BTN_SIZE) / 2;

    for (uint32_t i = 0; i < DOCK_MAX_APPS; i++) {
        dock_app_t *app = &dock->apps[i];
        if (!app->active) continue;

        bool hovered = ((int32_t)i == dock->hover_index);

        // Square app button background (acts as border/frame)
        uint32_t border_color = hovered ? TASKBAR_HOVER_COLOR : 0xFF303030;
        fb_fill_rect(app_x, btn_y, PROCESS_BTN_SIZE, PROCESS_BTN_SIZE, argb_to_fb(border_color));

        // Draw icon or colored square
        if (app->icon_id >= 0) {
            // Draw icon centered in button
            int icon_sz = 20;
            int ix = app_x + (PROCESS_BTN_SIZE - icon_sz) / 2;
            int iy = btn_y + (PROCESS_BTN_SIZE - icon_sz) / 2;
            icon_draw_scaled(app->icon_id, ix, iy, icon_sz, 0xFFFFFF);
        } else {
            // Fallback: colored square
            fb_fill_rect(app_x + 2, btn_y + 2, PROCESS_BTN_SIZE - 4, PROCESS_BTN_SIZE - 4,
                        argb_to_fb(app->icon_color));
        }

        // Update app position for hit testing
        app->x = app_x;
        app->y = btn_y;

        app_x += PROCESS_BTN_SIZE + TASKBAR_ICON_SPACING;
    }

    // ========== SYSTEM GAUGES (right side) ==========
    int32_t gauge_h = 22;  // Taller to fit regular 8x16 font
    int32_t gauge_x = dock->x + dock->width - (GAUGE_WIDTH * 4 + GAUGE_SPACING * 3 + TASKBAR_PADDING);
    int32_t gauge_y = dock->y + (TASKBAR_HEIGHT - gauge_h) / 2;
    char value_buf[16];

    // CPU gauge - real usage tracking
    int cpu_percent = cpu_get_usage();
    if (cpu_percent >= 100) {
        value_buf[0] = '1';
        value_buf[1] = '0';
        value_buf[2] = '0';
        value_buf[3] = '%';
        value_buf[4] = '\0';
    } else if (cpu_percent >= 10) {
        value_buf[0] = (cpu_percent / 10) + '0';
        value_buf[1] = (cpu_percent % 10) + '0';
        value_buf[2] = '%';
        value_buf[3] = '\0';
    } else {
        value_buf[0] = cpu_percent + '0';
        value_buf[1] = '%';
        value_buf[2] = '\0';
    }
    draw_gauge(gauge_x, gauge_y, GAUGE_WIDTH, gauge_h, cpu_percent, 0xFF00AA00, "CPU", value_buf);
    gauge_x += GAUGE_WIDTH + GAUGE_SPACING;

    // Memory gauge - use real memory stats
    uint64_t total_pages = pmm_get_total_pages();
    uint64_t used_pages = pmm_get_used_pages();
    int mem_percent = (total_pages > 0) ? (used_pages * 100 / total_pages) : 0;
    uint64_t used_mb = (used_pages * 4096) / (1024 * 1024);

    if (used_mb >= 100) {
        value_buf[0] = (used_mb / 100) + '0';
        value_buf[1] = ((used_mb / 10) % 10) + '0';
        value_buf[2] = (used_mb % 10) + '0';
        value_buf[3] = 'M';
        value_buf[4] = '\0';
    } else if (used_mb >= 10) {
        value_buf[0] = (used_mb / 10) + '0';
        value_buf[1] = (used_mb % 10) + '0';
        value_buf[2] = 'M';
        value_buf[3] = '\0';
    } else {
        value_buf[0] = used_mb + '0';
        value_buf[1] = 'M';
        value_buf[2] = '\0';
    }
    draw_gauge(gauge_x, gauge_y, GAUGE_WIDTH, gauge_h, mem_percent, 0xFF0088CC, "RAM", value_buf);
    gauge_x += GAUGE_WIDTH + GAUGE_SPACING;

    // Disk gauge - show actual filesystem usage
    int dsk_percent = 0;
    strcpy(value_buf, "N/A");
    if (g_fat_fs.mounted) {
        static uint32_t cached_free = 0;
        static uint64_t last_disk_check = 0;
        
        // Only recalculate every 5 seconds
        if (timer_ticks - last_disk_check >= 500 || cached_free == 0) {
            cached_free = fat_get_free_clusters(&g_fat_fs);
            last_disk_check = timer_ticks;
        }
        
        uint32_t total_clusters = g_fat_fs.cluster_count;
        uint32_t used_clusters = total_clusters - cached_free;
        dsk_percent = (total_clusters > 0) ? (used_clusters * 100 / total_clusters) : 0;
        
        uint32_t cluster_size = g_fat_fs.sectors_per_cluster * g_fat_fs.bytes_per_sector;
        uint32_t used_mb_disk = (used_clusters * cluster_size) / (1024 * 1024);
        if (used_mb_disk >= 100) {
            value_buf[0] = (used_mb_disk / 100) + '0';
            value_buf[1] = ((used_mb_disk / 10) % 10) + '0';
            value_buf[2] = (used_mb_disk % 10) + '0';
            value_buf[3] = 'M';
            value_buf[4] = '\0';
        } else if (used_mb_disk >= 10) {
            value_buf[0] = (used_mb_disk / 10) + '0';
            value_buf[1] = (used_mb_disk % 10) + '0';
            value_buf[2] = 'M';
            value_buf[3] = '\0';
        } else {
            value_buf[0] = used_mb_disk + '0';
            value_buf[1] = 'M';
            value_buf[2] = '\0';
        }
    }
    draw_gauge(gauge_x, gauge_y, GAUGE_WIDTH, gauge_h, dsk_percent, 0xFFCC8800, "DSK", value_buf);
    gauge_x += GAUGE_WIDTH + GAUGE_SPACING;

    // Network gauge - show link status
    int net_percent = 0;
    strcpy(value_buf, "Down");
    if (nic_link_up()) {
        net_percent = 50;
        strcpy(value_buf, "Up");
    }
    draw_gauge(gauge_x, gauge_y, GAUGE_WIDTH, gauge_h, net_percent, 0xFF8800CC, "NET", value_buf);
}

bool dock_hit_test(int x, int y) {
    dock_t *dock = &g_desktop.dock;

    return (x >= dock->x && x < (int32_t)(dock->x + dock->width) &&
            y >= dock->y && y < (int32_t)(dock->y + dock->height));
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
}

int dock_get_app_at(int x, int y) {
    dock_t *dock = &g_desktop.dock;

    // Quick bounds check for entire dock
    if (!dock_hit_test(x, y)) {
    
    // Safety check: ensure dock is properly initialized
    if (dock->y == 0 && g_desktop.screen_height > 0) {
        kprintf("[StartMenu] WARNING: dock->y is 0, recalculating dock position\n");
        dock->y = g_desktop.screen_height - TASKBAR_HEIGHT;
        dock->height = TASKBAR_HEIGHT;
        dock->width = g_desktop.screen_width / 2;
    }
        return -1;
    }

    // Check each app button (square buttons)
    for (uint32_t i = 0; i < DOCK_MAX_APPS; i++) {
        dock_app_t *app = &dock->apps[i];
        if (!app->active) continue;

        if (x >= app->x && x < app->x + (int32_t)PROCESS_BTN_SIZE &&
            y >= app->y && y < app->y + (int32_t)PROCESS_BTN_SIZE) {
            return i;
        }
    }

    return -1;
}

desktop_t *desktop_get_state(void) {
    return &g_desktop;
}

// Draw mouse cursor at position (public API)
void desktop_draw_cursor(int32_t x, int32_t y) {
    // Simple arrow cursor (12x16 pixels)
    static const uint8_t cursor[] = {
        0b10000000, 0b00000000,
        0b11000000, 0b00000000,
        0b11100000, 0b00000000,
        0b11110000, 0b00000000,
        0b11111000, 0b00000000,
        0b11111100, 0b00000000,
        0b11111110, 0b00000000,
        0b11111111, 0b00000000,
        0b11111111, 0b10000000,
        0b11111111, 0b11000000,
        0b11111100, 0b00000000,
        0b11101100, 0b00000000,
        0b11000110, 0b00000000,
        0b10000110, 0b00000000,
        0b00000011, 0b00000000,
        0b00000011, 0b00000000,
    };

    uint32_t white = 0xFFFFFF;
    uint32_t black = 0x000000;

    for (int row = 0; row < 16; row++) {
        uint16_t bits = (cursor[row*2] << 8) | cursor[row*2 + 1];
        for (int col = 0; col < 12; col++) {
            if (bits & (0x8000 >> col)) {
                fb_put_pixel(x + col, y + row, white);
                // Add black outline
                fb_put_pixel(x + col + 1, y + row, black);
                fb_put_pixel(x + col, y + row + 1, black);
            }
        }
    }
}

// Main desktop event loop (new event-driven architecture)
// #317 pass 2: optional boot auto-launch. /CONFIG/AUTORUN.CFG holds one app
// path (e.g. "/APPS/files"); launched a few seconds after boot once the
// compositor has rendered. No-op when the file is absent (kiosk/test aid).
static void autorun_worker(void *arg) {
    (void)arg;
    extern void proc_sleep(uint32_t ms);
    proc_sleep(14000);
    uint32_t sz = 0;
    char *cfg = (char *)fat_read_file(&g_fat_fs, "/CONFIG/AUTORUN.CFG", &sz);
    if (!cfg) return;
    // #595: the scan MUST be bounded by the byte count fat_read_file returned.
    // It previously stopped only at '\n'/'\r'/NUL, so an AUTORUN.CFG with no
    // trailing newline (the buffer is NOT NUL-terminated by fat_read_file) ran
    // straight off the end of the allocation into adjacent heap and appended
    // whatever bytes were there to the path. OBSERVED: a file containing
    // exactly "/APPS/OPENARENA" made the kernel try to spawn
    // "/APPS/OPENARENAjPAEH". Real out-of-bounds read + wrong-path spawn.
    char path[128];
    uint32_t i = 0;
    while (i < sz && i < sizeof(path) - 1 &&
           cfg[i] && cfg[i] != '\n' && cfg[i] != '\r') {
        path[i] = cfg[i];
        i++;
    }
    path[i] = 0;
    kfree(cfg);
    kprintf("[Autorun] AUTORUN.CFG %u bytes, path='%s'\n", sz, path);
    if (path[0] == '/') launch_userspace_app(path);
}

// ===========================================================================
// #126 SESSION TEARDOWN. A login is a boundary; a login gate that leaves the
// previous user's processes running is not one.
//
// Log Out (startmenu.c) and Switch User (lockscreen.c) both exit
// /APPS/COMPOSIT, this function's caller notices, and main.c re-runs the login
// gate. Every OTHER process of the departing session used to survive all of
// that: still scheduled, still owning kernel windows, so the next compositor
// composited them and the INCOMING user was handed a live, focusable window
// belonging to a process running as the OUTGOING user. Under the shipped
// autologin=root that is a root process inside another user's session.
//
// THE RULE ITSELF IS NOT HERE. It is rustkern/sessend.rs (new kernel logic, so
// Rust per the 2026-07-16 rule); this is the table walk, which is C because
// proc_get()/MAX_PROCESSES/process_t are C and the walk is glue, not policy.
//
// SIGKILL, not a direct teardown: sig_raise() is the existing chokepoint and
// already handles the case that matters here, a GUI app blocked in
// win_get_event() on a wait queue. wake_up_process() unlinks it with
// WAIT_EINTR, the syscall returns, and return_work_handler runs the default
// action (proc_exit). proc_exit() is also what destroys the process's windows
// (cleanup_user_windows_for_process), which is why nothing here touches the
// window manager: a second place to tear a window down is a second place to
// get it wrong.
//
// KNOWN LIMIT, stated rather than implied away: termination is asynchronous.
// A process only dies at its next syscall return, so a Ring 3 process that
// never syscalls again would linger. Every windowed app polls win_get_event()
// (that is how it draws at all), so this is not the shipped case, but it is
// not a guarantee either.
static int session_end_teardown(uint32_t leader) {
    extern process_t *proc_get(uint32_t pid);
    extern void sig_raise(struct process *target, int signo);
    extern int sessend_should_kill_rs(uint32_t leader, uint32_t pid, uint32_t session,
                                      uint32_t privilege, uint32_t state);
    int killed = 0;
    if (leader == 0) {
        // Refused, not treated as "kill everything": see sessend.rs. A zero
        // leader means the session identity was never established, and the
        // wildcard reading of that would end every Ring 3 process alive.
        kprintf("[SESSION] teardown refused: no session id\n");
        return 0;
    }
    for (uint32_t pid = 1; pid < MAX_PROCESSES; pid++) {
        process_t *p = proc_get(pid);
        if (!p) continue;
        if (!sessend_should_kill_rs(leader, p->pid, p->session,
                                    (uint32_t)p->privilege, (uint32_t)p->state)) continue;
        kprintf("[SESSION] teardown: killing pid %u '%s' (uid=%u session=%u)\n",
                p->pid, p->name, p->uid, p->session);
        bootlog_write("[SESSION] teardown killed pid %u '%s' uid=%u session=%u",
                      p->pid, p->name, p->uid, p->session);
        sig_raise(p, SIGKILL);
        killed++;
    }
    kprintf("[SESSION] teardown: session %u ended, %d process(es) signalled\n",
            leader, killed);
    bootlog_write("[SESSION] teardown: session %u ended, %d signalled", leader, killed);
    return killed;
}

void desktop_run(void) {
    // #157: this file does not pull in video/graphics.h, so declare the two
    // boot-console entry points it now uses, in the same local-extern style the
    // rest of this function already uses for gfx_boot_release_display().
    extern void gfx_boot_log(const char *message);
    extern void gfx_boot_log_replace(const char *message);

    if (!g_desktop.initialized) {
        kprintf("[Desktop] Error: desktop not initialized\n");
        return;
    }

    kprintf("[Desktop] Starting desktop environment...\n");

    // #157: the #569 release used to happen HERE, before the compositor was
    // even spawned. It was too early by exactly the interval that matters.
    //
    // Between this point and the compositor's first present the kernel
    // deliberately KEEPS THE BOOT SPLASH on screen (see the comment below and
    // #268), so releasing here meant that during the single riskiest stretch of
    // the whole boot - loading a Ring-3 ELF off the root disk, faulting in its
    // pages, running its startup, its first fb_flip - the screen was frozen and
    // the kernel had voluntarily given up its only way to say anything to
    // whoever was standing in front of the machine.
    //
    // #569's actual requirement is "once a REAL UI is on screen, late
    // background gfx_boot_log() must not repaint over it". The real UI here is
    // the compositor's first frame, and the kernel can observe that exactly:
    // g_fb_flip_count leaving 0. So the release is deferred to the wait loop
    // below, which does it the moment that counter moves. Until then the boot
    // console stays live and keeps reporting, which is what turns a frozen
    // screen into a readable one on a machine with no serial port.
    //
    // The compositor-absent fallback further down releases it explicitly before
    // it draws the kernel desktop, because that path paints a real UI too.

    // Auto-launch the userland compositor. While it starts we KEEP THE BOOT
    // SPLASH on screen (do not draw the kernel desktop) so the handoff to the
    // usermode compositor is seamless - no flash of the kernel taskbar/icons.
    // The kernel desktop is only drawn as a fallback if the compositor is absent.
    extern int g_compositor_launched;
    int compositor_pid = 0;   // #566: track compositor pid to detect its death
    // #COMPRESPAWN: bounded relaunch retry state. Armed only by a FAILED
    // launch; see the failure branch below and the deadline test in the main
    // loop. Deliberately locals of desktop_run(), so a fresh desktop_run()
    // (i.e. a fresh login-gate iteration) starts with a fresh budget.
    int      s_relaunch_left = 0;
    uint64_t s_relaunch_at   = 0;
    {
        // #430 verification gate: if /PTTEST.RUN exists on the FAT ESP, launch
        // the signals+pthreads test app INSTEAD of the compositor. This runs it
        // at the same steady-state-safe spawn site, but with no compositor (so
        // no AI-Chat pegging the CPU) - the test gets a clean core and its
        // "PTTEST:" output goes straight to the serial log. Marker absent on
        // normal boots => behavior is completely unchanged.
        {
            extern fat_fs_t g_fat_fs;
            uint32_t __pt_sz = 0;
            void *__pt_mark = fat_read_file(&g_fat_fs, "/PTTEST.RUN", &__pt_sz);
            if (__pt_mark) {
                kfree(__pt_mark);
                kprintf("[Desktop] #430 PTTEST.RUN present: running /APPS/PTTEST "
                        "instead of the compositor\n");
                int tpid = launch_userspace_app("/APPS/PTTEST");
                kprintf("[Desktop] #430 pttest launched pid=%d\n", tpid);
                // Fall through into the desktop loop with no compositor; the
                // test proc is scheduled normally and prints to serial.
                goto pttest_after_launch;
            }
        }
        // #745 task #59: ARM THE FRAMEBUFFER CLAIM BEFORE THE LAUNCH.
        // The claim window has to be open before the child can possibly run,
        // or a compositor scheduled on another core between these two lines
        // would be refused and the desktop would never come up. So: open it
        // for "the next claimant", then narrow it to the real pid the instant
        // we know it. fb_owner_arm() is a no-op once there is an owner, so if
        // the compositor claimed inside that microscopic window the second
        // call changes nothing. See gui/fbown.h.
        extern void fb_owner_arm(uint32_t pid);
        extern void fb_owner_disarm(void);
        fb_owner_arm(0);
        // #157: on screen, not just on serial. The owner's machine has no serial
        // port, so anything that is only kprintf()ed does not exist to him.
        // (#182) /CONFIG/FMTEST.CFG containing "1" runs the OPL2 FM core's
        // objective self-test in Ring 3 and prints the result to the serial
        // console. It is a DIAGNOSTIC gate of the same family as DOSDIAG.CFG /
        // DOSRING.CFG / DOSIO.CFG / DOSOPL.CFG, it is not shipped in the
        // golden, and it exists so the in-OS arm of #182's verification can be
        // RE-RUN rather than re-argued.
        //
        // It runs BEFORE the compositor so its output is not interleaved with
        // the compositor's startup, and because it needs no window: everything
        // it reports goes to serial via SYS_PUTCHAR, which falls through to
        // kputc for a process with no stdout fd.
        {   uint32_t _tz = 0;
            void *_tc = fat_read_file(&g_fat_fs, "/CONFIG/FMTEST.CFG", &_tz);
            if (_tc) {
                char c0 = _tz ? ((char *)_tc)[0] : '0';
                kfree(_tc);
                if (c0 == '1') {
                    kprintf("[BOOT] (#182) FMTEST.CFG armed: running /APPS/FMTEST\n");
                    int tpid = launch_userspace_app("/APPS/FMTEST");
                    kprintf("[BOOT] (#182) /APPS/FMTEST pid=%d\n", tpid);
                }
            }
        }
        gfx_boot_log("[BOOT] Launching compositor (/APPS/COMPOSIT)...");
        // #307 telemetry: RECORD THE ATTEMPT, NOT ONLY THE SUCCESS.
        //
        // Measured on the owner's iMac14,4, golden 2039 (/BOOTLOG.TXT recovered
        // from partition 2 of his stick, 268 KB, 15.6 h of uptime): the
        // compositor exited with code 127 twice, and after the SECOND exit the
        // session relogged in ("[LOGIN] autologin: OK as 'james'") and then NO
        // "[SESSION] compositor pid N spawned" line ever followed. The machine
        // sat on a dead desktop for the remaining ~7 minutes.
        //
        // The log could not say WHY, because the only two records of this launch
        // were the success line below and a FAILURE branch that wrote nothing
        // durable at all (gfx_boot_log + kprintf only). On a machine with no
        // serial port that failure branch is silence, so "launch_userspace_app
        // returned <= 0" and "the kernel never reached the call" left byte-for-
        // byte identical evidence. This line separates them: an attempt with no
        // following spawned/FAILED line means the loader itself did not return.
        bootlog_write("[SESSION] launching /APPS/COMPOSIT (session respawn)");
        int cpid = launch_userspace_app("/APPS/COMPOSIT");
        if (cpid > 0) {
            fb_owner_arm((uint32_t)cpid);
            g_compositor_launched = 1;
            compositor_pid = cpid;   // #566
            // #126: THE COMPOSITOR IS THE SESSION LEADER. process_t already
            // carries pgrp/session and proc_create already INHERITS them down
            // a spawn tree (proc/process.c: session = creator->session ?
            // creator->session : own pid), so the only thing missing was a
            // leader for the desktop session to hang off. This is what a login
            // manager does, and what setsid() would do if the compositor could
            // call it for itself before its first child exists.
            //
            // The pre/post values are LOGGED rather than assumed: whether the
            // compositor was already its own session leader depends on whether
            // proc_current() is non-NULL in this kernel thread, and "it must
            // already be right" is exactly the kind of claim this tree has had
            // outlive the code. Measure it on every boot instead.
            {
                extern process_t *proc_get(uint32_t pid);
                process_t *cp0 = proc_get((uint32_t)cpid);
                if (cp0) {
                    bootlog_write("[SESSION] compositor pid %d spawned with pgrp=%u session=%u",
                                  cpid, cp0->pgrp, cp0->session);
                    cp0->pgrp    = (uint32_t)cpid;
                    cp0->session = (uint32_t)cpid;
                    bootlog_write("[SESSION] compositor pid %d is now session leader (session=%u)",
                                  cpid, cp0->session);
                }
            }
            proc_create_ex("autorun", autorun_worker, 0, PRIO_LOW, 64*1024);
            kprintf("[Desktop] Compositor launched (pid %d); keeping splash until it renders\n", cpid);
            {
                char _cb[96];
                snprintf(_cb, sizeof(_cb),
                         "[BOOT] Compositor started (pid %d), waiting for its first frame...",
                         cpid);
                gfx_boot_log(_cb);
            }
        } else {
            // Nothing will claim the framebuffer, so do not leave a window open
            // for whatever Ring 3 process happens to ask first.
            fb_owner_disarm();
            kprintf("[Desktop] WARNING: compositor not found; falling back to kernel desktop\n");
            // #307: DURABLE record of the failure. See the comment at the call
            // site above for why kprintf/gfx_boot_log alone was not enough.
            bootlog_write("[SESSION] compositor launch FAILED: "
                          "launch_userspace_app(\"/APPS/COMPOSIT\") returned %d; "
                          "falling back to the in-kernel desktop", cpid);
            // #COMPRESPAWN: THE FALLBACK IS NO LONGER PERMANENT.
            //
            // Before this, one failed relaunch meant no desktop until reboot:
            // the kernel desktop was drawn and nothing ever tried again. That
            // converts every compositor fault from a blink into a dead machine,
            // which is strictly worse than the fault.
            //
            // The retry is a DEADLINE TEST inside the loop that already runs
            // below and already sleeps for its own reasons - not a new poll
            // loop and not a new sleep (#426). Bounded at three attempts,
            // because a permanently missing or corrupt binary must stop
            // retrying and say so rather than churn forever.
            s_relaunch_left = 3;
            s_relaunch_at   = (uint64_t)timer_ticks +
                              2ull * (g_timer_hz ? g_timer_hz : 100);
            // #157: this branch paints a real UI, so it takes over from the boot
            // console here (the release that used to sit unconditionally at the
            // top of desktop_run()).
            { extern void gfx_boot_release_display(void); gfx_boot_release_display(); }
            gfx_boot_log("[BOOT] compositor NOT FOUND: /APPS/COMPOSIT failed to launch; "
                         "using the in-kernel desktop");
            wm_invalidate_all();
            desktop_draw();
            wm_draw_all();
            wm_draw_apps();
            wm_draw_winmenu();   // Task A: decorator popup on top of app content
            extern void fb_swap_buffers(void);
            fb_swap_buffers();
        }
    }

pttest_after_launch:;   // #430: gate jumps here, skipping the compositor path

    // #95: start enabled+autostart background services now that the
    // compositor (the first user process) is up and the allocator is in
    // steady state. svc_init() already built the registry in main().
    {
        // #566: autostart background services ONCE per boot, so re-entering
        // desktop_run() after a Log Out / compositor restart does not spawn
        // duplicate service processes.
        static int s_svc_autostarted = 0;
        if (!s_svc_autostarted) {
            extern void svc_autostart(void);
            svc_autostart();
            s_svc_autostarted = 1;
        }
    }

    // Store last mouse position and button state
    int32_t last_mouse_x = 0, last_mouse_y = 0;
    uint8_t last_buttons = 0;
    mouse_get_position(&last_mouse_x, &last_mouse_y);

    // Initialize CPU tracking
    g_cpu_last_tick = timer_ticks;
    g_cpu_busy_ticks = 0;
    g_cpu_total_ticks = 0;

    // Main loop
    // Drain any stray scancodes left over from keyboard controller init
    // (e.g. 0xFA ACK, ESC, F12) that could trigger unwanted actions.
    {
        extern int keyboard_has_char(void);
        extern int keyboard_get_char(void);
        int drained = 0;
        while (keyboard_has_char()) {
            keyboard_get_char();
            drained++;
        }
        if (drained > 0) {
            kprintf("[Desktop] Drained %d stray boot scancodes\n", drained);
        }
    }

    int running = 1;
    while (running) {
        // #COMPRESPAWN: bounded relaunch retry. No new loop, no new sleep: this
        // is a deadline test inside the loop that already exists, and it only
        // ever does anything when a launch has actually failed.
        //
        // WHY A RETRY IS THE RIGHT SHAPE HERE AND A WAIT QUEUE IS NOT. The
        // thing we are waiting for is memory being returned by processes that
        // session_end_teardown() has SIGKILLed but which have not run yet: a
        // process only dies at its next syscall return, so there is no
        // condition anyone can wake us on. That is the "wake source is outside
        // our control" case, and a bounded, LOGGED, terminating retry is the
        // honest answer to it.
        if (!g_compositor_launched && s_relaunch_left > 0 &&
            (uint64_t)timer_ticks >= s_relaunch_at) {
            s_relaunch_left--;
            bootlog_write("[SESSION] compositor relaunch retry (%d attempt(s) "
                          "left after this one)", s_relaunch_left);
            extern void fb_owner_arm(uint32_t pid);
            int rpid = launch_userspace_app("/APPS/COMPOSIT");
            if (rpid > 0) {
                fb_owner_arm((uint32_t)rpid);
                g_compositor_launched = 1;
                compositor_pid = rpid;
                {
                    extern process_t *proc_get(uint32_t pid);
                    process_t *rp = proc_get((uint32_t)rpid);
                    if (rp) { rp->pgrp = (uint32_t)rpid; rp->session = (uint32_t)rpid; }
                }
                s_relaunch_left = 0;
                bootlog_write("[SESSION] compositor RELAUNCH SUCCEEDED on retry "
                              "(pid %d); the fallback desktop is being replaced",
                              rpid);
                wm_invalidate_all();
                continue;
            }
            if (s_relaunch_left == 0) {
                bootlog_write("[SESSION] compositor relaunch gave up after 3 "
                              "attempts; the in-kernel desktop is now permanent "
                              "for this session. The [LAUNCH] FAILED lines above "
                              "name the reason.");
            } else {
                s_relaunch_at = (uint64_t)timer_ticks +
                                2ull * (g_timer_hz ? g_timer_hz : 100);
            }
        }

        // Check if exclusive mode is active (DOOM fullscreen, etc.)
        // If so, yield CPU and skip desktop processing
        if (wm_is_exclusive_mode() || g_compositor_launched) {
            // #566 decision 3: if the compositor process has DIED (Log Out,
            // crash, kill), RETURN from desktop_run() so the kernel re-enters the
            // login gate (main.c's #566 login-gate loop), NEVER the
            // unauthenticated Ring-0 shell. Death = the pid is gone, or its slot
            // is now a zombie/unused entry.
            if (g_compositor_launched && compositor_pid > 0) {
                extern process_t *proc_get(uint32_t pid);
                process_t *cp = proc_get((uint32_t)compositor_pid);
                if (!cp || cp->state == PROC_STATE_ZOMBIE ||
                    cp->state == PROC_STATE_UNUSED) {
                    kprintf("[Desktop] Compositor (pid %d) exited; returning to "
                            "login gate (no shell fallback)\n", compositor_pid);
                    g_compositor_launched = 0;
                    // #126: end the session BEFORE returning to the gate, so
                    // the next user cannot be handed the previous user's
                    // running processes. The compositor itself is already gone
                    // and is excluded by the rule (sessend.rs).
                    session_end_teardown((uint32_t)compositor_pid);
                    return;
                }
                // #307: THE DETECTOR ABOVE POLLS EVERY 50 ms AND IT ONCE TOOK
                // 3.86 HOURS TO FIRE.
                //
                // Measured on the owner's iMac14,4, golden 2039, from the
                // /BOOTLOG.TXT his machine wrote: "[PROC] 'COMPOSIT' (PID 31)
                // exiting, code 127" at t=19,957,982 ms, and the session
                // teardown did not begin until t=33,856,092 ms. The xHCI
                // heartbeat ticked all the way through, so the scheduler was
                // alive and this loop was almost certainly running; the pid
                // therefore sat in a state that is neither ZOMBIE nor UNUSED
                // for nearly four hours while the desktop stayed dead on
                // screen. The SECOND death, six hours later, was detected in
                // the same 60-second window it happened in.
                //
                // The log could not say which state, because nothing recorded
                // it. One line a minute does, and it costs the same as the
                // xHCI heartbeat that is already there. Rate-limited off
                // timer_ticks, which is NOT a wall clock (it replays in bursts
                // under vCPU starvation) - that is fine for a log cadence and
                // would not be fine for a deadline.
                else {
                    static uint64_t s_comp_state_log = 0;
                    if ((uint64_t)timer_ticks - s_comp_state_log >= 250u * 60u) {
                        s_comp_state_log = (uint64_t)timer_ticks;
                        bootlog_write("[SESSION] compositor pid %d present: state=%d",
                                      compositor_pid, (int)cp->state);
                    }
                }
            }
            // The userland compositor owns the screen (or was just launched and is
            // about to render): idle, do NOT draw the kernel desktop, so the boot
            // splash persists seamlessly until the compositor's first frame (#268).
            // ---------------------------------------------------------------
            // #157 COMPOSITOR-HANDOFF WATCHDOG.
            //
            // Until the compositor's first present the boot console still owns
            // the display (see the long #157 comment above the launch), so this
            // is the kernel's last chance to tell a serial-less machine what is
            // happening. Three jobs, in order:
            //
            //  1. The instant g_fb_flip_count leaves 0, a real UI is on screen:
            //     hand the display over, exactly as #569 requires, and never
            //     touch it again.
            //  2. While it is still 0 past a grace period, repaint a status line
            //     that INCLUDES A LIVE SECONDS COUNTER. That counter is the
            //     whole point: a static screen cannot tell "the kernel is wedged"
            //     apart from "the kernel is fine and the compositor never drew",
            //     and those need completely different fixes. A ticking number
            //     means the kernel scheduler, timer and framebuffer are alive.
            //  3. If the compositor has produced nothing at all after a long
            //     bound, stop waiting for it and fall back to the in-kernel
            //     desktop, so the machine is usable instead of frozen. Disabled
            //     by dropping /NOKDESK.TXT on the FAT ESP.
            //
            // No spin and no poll loop is introduced (#426): this is the
            // pre-existing proc_sleep(50) idle iteration of a loop that already
            // ran, and every branch below is O(1) with no waiting of its own.
            // ---------------------------------------------------------------
            if (g_compositor_launched) {
                extern volatile uint64_t g_fb_flip_count;
                extern void gfx_boot_release_display(void);
                extern bool gfx_boot_owns_display(void);
                static uint64_t s_wait_t0 = 0;
                static uint32_t s_last_report_s = 0xFFFFFFFFu;
                static int s_gave_up = 0;
                static int s_reported_once = 0;
                if (s_wait_t0 == 0) s_wait_t0 = timer_ticks;

                if (g_fb_flip_count != 0) {
                    // (1) The compositor is on screen. Hand over once.
                    if (gfx_boot_owns_display()) {
                        gfx_boot_release_display();
                        bootlog_write("[BOOT] #157 compositor first frame seen; "
                                      "boot console released the display");
                    }
                } else if (!s_gave_up) {
                    uint32_t hz = g_timer_hz ? g_timer_hz : 100;
                    uint32_t secs = (uint32_t)((timer_ticks - s_wait_t0) / hz);
                    // (3) Long bound first, so the give-up message is the last
                    // thing the status line says rather than being overwritten.
                    if (secs >= 45) {
                        extern fat_fs_t g_fat_fs;
                        int inhibit = (g_fat_fs.mounted &&
                                       fat_exists(&g_fat_fs, "/NOKDESK.TXT"));
                        bootlog_write("[BOOT] #157 compositor pid %d produced NO frame in "
                                      "%u s; kernel-desktop fallback %s",
                                      compositor_pid, secs,
                                      inhibit ? "INHIBITED by /NOKDESK.TXT" : "ENGAGING");
                        if (!inhibit) {
                            {
                                char _fb[112];
                                snprintf(_fb, sizeof(_fb),
                                         "[BOOT] compositor produced no frame in %u s - "
                                         "falling back to the in-kernel desktop",
                                         (unsigned)secs);
                                gfx_boot_log(_fb);
                            }
                            gfx_boot_release_display();
                            g_compositor_launched = 0;
                            s_gave_up = 1;
                            continue;   // next iteration draws the kernel desktop
                        }
                        s_gave_up = 1;   // inhibited: stop reporting, keep waiting
                    } else if (secs >= 8 && secs != s_last_report_s) {
                        // (2) Live, ticking status line. Only from 8 s, so a
                        // normal boot (first frame well under that) never shows
                        // it and the handoff stays seamless (#268).
                        s_last_report_s = secs;
                        extern process_t *proc_get(uint32_t pid);
                        process_t *cp = (compositor_pid > 0)
                                        ? proc_get((uint32_t)compositor_pid) : NULL;
                        const char *st = !cp ? "GONE"
                                       : (cp->state == PROC_STATE_ZOMBIE) ? "ZOMBIE"
                                       : (cp->state == PROC_STATE_UNUSED) ? "DEAD"
                                       : "alive";
                        char _wb[112];
                        snprintf(_wb, sizeof(_wb),
                                 "[BOOT] waiting for compositor pid %d (%s) - "
                                 "%u s, 0 frames presented",
                                 compositor_pid, st, (unsigned)secs);
                        // #157: REPLACE the last console line rather than
                        // appending one per second. The point of this line is a
                        // number that visibly changes, which is what tells the
                        // person in front of the machine that the kernel is
                        // still alive; scrolling the log off the screen once a
                        // second would destroy the rest of the boot record they
                        // need to read at the same time.
                        if (s_reported_once) gfx_boot_log_replace(_wb);
                        else { gfx_boot_log(_wb); s_reported_once = 1; }
                    }
                }
            }

            // The userland compositor owns the screen. REALLY sleep (yield the
            // CPU) instead of busy-spinning a volatile delay loop - that loop ran
            // forever under the compositor and pegged pid 0 at ~100% (#180).
            extern void proc_sleep(uint32_t ms);
            proc_sleep(50);
            continue;
        }

        // Track this iteration for CPU usage
        g_cpu_total_ticks++;

        bool this_iteration_busy = false;

        // ========== 1. COLLECT INPUT (non-blocking) ==========

        // Track if there was any input this frame
        bool had_input = false;

        // Check for keyboard input - throttled to 20 Hz shared with desktop_process_tick
        uint32_t _kb_iv = (g_timer_hz >= 20) ? (g_timer_hz / 20) : 1;
        extern volatile int g_win16_owns_screen;
        if (!g_win16_owns_screen && (timer_ticks - s_last_kb_tick >= _kb_iv) && keyboard_has_char()) {
            s_last_kb_tick = timer_ticks;
            had_input = true;
            int c = keyboard_get_char();

            // Ignore ALL keypresses during first 2 seconds of boot.
            // The keyboard controller sends spurious scancodes (ESC, F12, etc.)
            // during initialization that can trigger unwanted actions.
            if (timer_ticks < 2 * g_timer_hz) {
                continue;
            }

            // #703: F12 used to launch the in-kernel DOOM. The kernel DOOM
            // port has been removed; DOOM ships as the userland ELF
            // /GAMES/DOOM/DOOM.ELF, which the compositor start menu and its
            // desktop icon spawn. This fallback desktop has no launcher for
            // it, so F12 is now unbound rather than pointing at nothing.

            // F11 toggles maximize of the focused window
            if (c == 0x85) {
                window_t *focused = window_get_focused();
                if (focused) {
                    if (focused->flags & WINDOW_FLAG_MAXIMIZED) {
                        window_restore(focused);
                    } else {
                        window_maximize(focused);
                    }
                    wm_invalidate_all();
                }
                continue;
            }

            // F6 launches Terminal (Phase J2 quick-test hotkey)
            if (c == 0x8A) {
                kprintf("[Desktop] F6 - launching Terminal (PTY+msh)\n");
                extern void terminal_launch(void);
                terminal_launch();
                continue;
            }

            // ESC exits desktop only when no window is focused
            if (c == 27 && window_get_focused() == NULL) {
                kprintf("[Desktop] Exit key pressed, exiting desktop\n");
                running = 0;
                continue;
            }

            // #552: Space "toggle start menu" shortcut removed with the
            // kernel start menu. (Space now falls through to the normal
            // key-event queue below, same as any other printable key.)

            // Queue key event for window manager
            gui_event_t key_event = {0};
            // Check if this is a key release:
            // 1. Special release codes: mapped to the matching PRESS code
            // 2. Regular ASCII with bit 7 set (>= 0x80): clear bit 7
            //
            // #232 A SECOND COPY OF THE SAME DECODE. This block is a hand copy
            // of SYS_INJECT_KEY's (proc/syscall.c), which is how it also
            // inherited SYS_INJECT_KEY's bug: a blanket `c - 0x10` over the
            // whole 0x90-0x98 band delivered KEY_LCTRL_UP/KEY_LSHIFT_UP/
            // KEY_RSHIFT_UP as keycodes 0x84/0x87/0x88, which are the PRESS
            // codes of F5, F10 and F1. Fixed here TOO rather than only in
            // syscall.c, because a fallback that decodes keys differently from
            // the live path is a bug that only ever shows up on the day the
            // fallback is the one running. See the long note at the
            // SYS_INJECT_KEY site for the reasoning; the four mappings must
            // stay identical between the two.
            if (c == 0x94) {                     // KEY_LCTRL_UP  -> KEY_LCTRL
                key_event.type = EVENT_KEY_UP;
                key_event.keycode = 0x99;
                key_event.key_char = 0;
            } else if (c == 0x97) {              // KEY_LSHIFT_UP -> KEY_LSHIFT
                key_event.type = EVENT_KEY_UP;
                key_event.keycode = 0x95;
                key_event.key_char = 0;
            } else if (c == 0x98) {              // KEY_RSHIFT_UP -> KEY_RSHIFT
                key_event.type = EVENT_KEY_UP;
                key_event.keycode = 0x96;
                key_event.key_char = 0;
            } else if (c == 0x9C) {              // KEY_ALT_UP    -> KEY_ALT
                key_event.type = EVENT_KEY_UP;
                key_event.keycode = 0x9A;
                key_event.key_char = 0;
            } else if (c >= 0x90 && c <= 0x93) {
                // Arrow releases only; here -0x10 is exact by construction.
                key_event.type = EVENT_KEY_UP;
                key_event.keycode = c - 0x10;
                key_event.key_char = 0;
            } else if (c >= 0x80 && c < 0x90) {
                // Special key press (arrows, ctrl, shift, etc.) - NOT a release
                key_event.type = EVENT_KEY_DOWN;
                key_event.keycode = c;
                key_event.key_char = 0;
            } else if (c > 0x98) {
                // Regular ASCII key release (ASCII | 0x80, range 0xA0-0xFF)
                key_event.type = EVENT_KEY_UP;
                key_event.keycode = c & 0x7F;  // Clear bit 7 to get ASCII
                key_event.key_char = 0;
            } else {
                // Regular key press (ASCII < 0x80)
                key_event.type = EVENT_KEY_DOWN;
                key_event.keycode = c;
                key_event.key_char = c;
            }
            wm_queue_event(&key_event);
        }

        // Poll mouse state
        mouse_poll();
        int32_t mouse_x, mouse_y;
        uint8_t buttons;
        mouse_get_position(&mouse_x, &mouse_y);
        buttons = mouse_get_buttons();

        // Queue mouse movement event
        if (mouse_x != last_mouse_x || mouse_y != last_mouse_y) {
            had_input = true;
            gui_event_t move_event = {0};
            move_event.type = EVENT_MOUSE_MOVE;
            move_event.mouse_x = mouse_x;
            move_event.mouse_y = mouse_y;
            move_event.mouse_buttons = buttons;
            wm_queue_event(&move_event);
        }

        // Queue mouse button down event
        if ((buttons & MOUSE_LEFT_BTN) && !(last_buttons & MOUSE_LEFT_BTN)) {
            had_input = true;
            gui_event_t down_event = {0};
            down_event.type = EVENT_MOUSE_DOWN;
            down_event.mouse_x = mouse_x;
            down_event.mouse_y = mouse_y;
            down_event.mouse_buttons = MOUSE_BUTTON_LEFT;
            wm_queue_event(&down_event);
        }

        // Queue right-click event
        if ((buttons & MOUSE_RIGHT_BTN) && !(last_buttons & MOUSE_RIGHT_BTN)) {
            gui_event_t down_event = {0};
            down_event.type = EVENT_MOUSE_DOWN;
            down_event.mouse_x = mouse_x;
            down_event.mouse_y = mouse_y;
            down_event.mouse_buttons = MOUSE_BUTTON_RIGHT;
            wm_queue_event(&down_event);
        }

        // Queue mouse button up event
        if (!(buttons & MOUSE_LEFT_BTN) && (last_buttons & MOUSE_LEFT_BTN)) {
            gui_event_t up_event = {0};
            up_event.type = EVENT_MOUSE_UP;
            up_event.mouse_x = mouse_x;
            up_event.mouse_y = mouse_y;
            up_event.mouse_buttons = MOUSE_BUTTON_LEFT;
            wm_queue_event(&up_event);
        }

        if (!(buttons & MOUSE_RIGHT_BTN) && (last_buttons & MOUSE_RIGHT_BTN)) {
            gui_event_t up_event = {0};
            up_event.type = EVENT_MOUSE_UP;
            up_event.mouse_x = mouse_x;
            up_event.mouse_y = mouse_y;
            up_event.mouse_buttons = MOUSE_BUTTON_RIGHT;
            wm_queue_event(&up_event);
        }

        // Queue mouse scroll event
        int8_t scroll = mouse_get_scroll();
        if (scroll != 0) {
            had_input = true;
            gui_event_t scroll_event = {0};
            scroll_event.type = EVENT_MOUSE_SCROLL;
            scroll_event.mouse_x = mouse_x;
            scroll_event.mouse_y = mouse_y;
            scroll_event.scroll_delta = scroll;
            wm_queue_event(&scroll_event);
        }

        // Mark iteration as busy if there was input
        if (had_input) {
            this_iteration_busy = true;
        }

        // ========== SCREENSAVER HANDLING ==========

        // Update screensaver idle tracking
        if (had_input) {
            screensaver_on_input();
        }
        screensaver_update();

        // If screensaver is active, draw it and skip normal processing
        if (screensaver_is_active()) {
            screensaver_draw();
            fb_swap_buffers();

            // Update tracking variables
            last_mouse_x = mouse_x;
            last_mouse_y = mouse_y;
            last_buttons = buttons;

            // Poll network even during screensaver (critical for connectivity)
            extern void net_poll(void);
            net_poll();
            // Sleep instead of proc_yield(): yield keeps this proc READY so it is
            // re-selected immediately (a busy spin). ~30 FPS is plenty for the saver.
            extern void proc_sleep(uint32_t ms); proc_sleep(33);
            continue;
        }

        // ========== 2. PROCESS EVENTS AND DISPATCH TO APPS ==========

        // Process queued events through window manager
        gui_event_t event;
        while (wm_poll_event(&event)) {
            switch (event.type) {
                case EVENT_MOUSE_MOVE:
                    // Let WM handle dragging/resizing
                    wm_handle_mouse_move(event.mouse_x, event.mouse_y);
                    // Update desktop hover states
                    desktop_handle_mouse_move(event.mouse_x, event.mouse_y);
                    break;

                case EVENT_MOUSE_DOWN:
                    // First check if click is on a window
                    if (window_get_at_point(event.mouse_x, event.mouse_y)) {
                        wm_handle_mouse_down(event.mouse_x, event.mouse_y, event.mouse_buttons);
                    } else {
                        // Click is on desktop
                        if (event.mouse_buttons & MOUSE_BUTTON_LEFT) {
                            desktop_handle_click(event.mouse_x, event.mouse_y);
                        } else if (event.mouse_buttons & MOUSE_BUTTON_RIGHT) {
                            desktop_handle_right_click(event.mouse_x, event.mouse_y);
                        }
                    }
                    break;
                case EVENT_MOUSE_UP:
                    wm_handle_mouse_up(event.mouse_x, event.mouse_y, event.mouse_buttons);
                    break;

                case EVENT_KEY_DOWN:
                    wm_handle_key_down(event.keycode, event.key_char);
                    break;

                case EVENT_KEY_UP:
                    wm_handle_key_up(event.keycode);
                    break;

                default:
                    break;
            }

            // Dispatch event to registered apps
            wm_dispatch_event(&event);
        }

        // ========== 3. RENDER IF DIRTY ==========

        // Check if we need to redraw (and mark iteration as busy if so)
        bool needs_redraw = wm_is_dirty();
        if (needs_redraw) {
            this_iteration_busy = true;
        }

        if (needs_redraw) {
            // For now, do full redraw (dirty rect optimization can come later)
            // Draw desktop background and dock
            desktop_draw();

            // Draw all windows (back to front)
            wm_draw_all();

            // Draw app-specific content inside windows
            wm_draw_apps();
            wm_draw_winmenu();   // Task A: decorator popup on top of app content

            // Draw version info in bottom-right corner (after windows, before context menu)
            char version_text[128];
            snprintf(version_text, sizeof(version_text), "v%s Build %d",
                     MAYTERA_VERSION_STRING, MAYTERA_BUILD_NUMBER);
            int version_x = g_desktop.screen_width - (strlen(version_text) * FONT_WIDTH) - 10;
            int version_y = g_desktop.screen_height - FONT_HEIGHT - 5;
            draw_string(version_x + 1, version_y + 1, version_text, argb_to_fb(0xFF000000)); // Shadow
            draw_string(version_x, version_y, version_text, argb_to_fb(0xFFCCCCCC)); // Light gray text

            // #552: kernel start menu draw call removed (draw_start_menu()
            // no longer exists).

            // Draw context menu AFTER windows so it appears on top
            if (g_context_menu_open) {
                draw_context_menu();
            }

            // Draw cursor on top (in back buffer)
            desktop_draw_cursor(mouse_x, mouse_y);

            // Always do a full swap to avoid dirty-rect race conditions with
            // the idle process (desktop_process_tick) clearing the dirty flag.
            extern void fb_swap_buffers(void);
            fb_swap_buffers();

            // Clear dirty state
            wm_clear_dirty();
        }

        // Update tracking variables
        last_mouse_x = mouse_x;
        last_mouse_y = mouse_y;
        last_buttons = buttons;

        // Poll network for incoming packets
        extern void net_poll(void);
        net_poll();
        extern void proc_yield(void); proc_yield();  // Allow kernel threads to run

        // ========== CPU USAGE TRACKING ==========

        // Track busy ticks if this iteration did work
        if (this_iteration_busy) {
            g_cpu_busy_ticks++;
        }

        // Update CPU usage calculation (~1 second intervals)
        cpu_update_usage();

        // Periodic taskbar refresh to update gauges (every ~120 iterations)
        static uint32_t refresh_counter = 0;
        refresh_counter++;
        if (refresh_counter >= 120) {
            refresh_counter = 0;
            // Only invalidate taskbar region, not entire screen
            rect_t taskbar_rect = {
                .x = 0,
                .y = (int32_t)(fb_get_height() - TASKBAR_HEIGHT),
                .width = (int32_t)fb_get_width(),
                .height = TASKBAR_HEIGHT
            };
            wm_invalidate_rect(&taskbar_rect);
        }

        // #102/#180: yield the CPU instead of busy-spinning on `pause` until
        // the next timer tick (that pegged a full core ~98%). proc_sleep lets the
        // idle proc HLT. Under the compositor the kernel desktop does no drawing
        // and the compositor frame already services net_poll, so sleep longer.
        {
            extern int g_compositor_launched;
            extern void proc_sleep(uint32_t ms);
            proc_sleep(g_compositor_launched ? 30 : 8);
        }
    }

    kprintf("[Desktop] Desktop environment exited\n");
}
// ============================================================================
// wm_process_input_events: collect and dispatch input for compositor syscall
// ============================================================================
// Called by sys_compositor_render_windows() so that apps remain interactive
// while the userland compositor is in exclusive mode.
void wm_process_input_events(void) {
    if (!g_desktop.initialized) return;

    // Service the network stack every compositor frame. Under the userland
    // compositor the kernel desktop_run loop (which used to call net_poll) no
    // longer runs, and desktop_process_tick early-returns in exclusive mode, so
    // without this the TCP/IP stack is never polled: no RX, no ARP replies, no
    // connectivity for IRC / remote-control / DNS while the compositor owns the
    // screen.
    { extern void net_poll(void); net_poll(); }

    // Poll mouse
    mouse_poll();
    int32_t mx, my;
    uint8_t buttons;
    mouse_get_position(&mx, &my);
    buttons = mouse_get_buttons();

    static int32_t  s_wpi_last_mx = -1;
    static int32_t  s_wpi_last_my = -1;
    static uint8_t  s_wpi_last_btn = 0;

    // Queue mouse movement
    if (mx != s_wpi_last_mx || my != s_wpi_last_my) {
        gui_event_t ev = {0};
        ev.type = EVENT_MOUSE_MOVE;
        ev.mouse_x = mx;
        ev.mouse_y = my;
        ev.mouse_buttons = buttons;
        wm_queue_event(&ev);
    }

    // Queue mouse button down (left)
    if ((buttons & MOUSE_LEFT_BTN) && !(s_wpi_last_btn & MOUSE_LEFT_BTN)) {
        gui_event_t ev = {0};
        ev.type = EVENT_MOUSE_DOWN;
        ev.mouse_x = mx;
        ev.mouse_y = my;
        ev.mouse_buttons = MOUSE_BUTTON_LEFT;
        wm_queue_event(&ev);
    }
    // Queue mouse button down (right)
    if ((buttons & MOUSE_RIGHT_BTN) && !(s_wpi_last_btn & MOUSE_RIGHT_BTN)) {
        gui_event_t ev = {0};
        ev.type = EVENT_MOUSE_DOWN;
        ev.mouse_x = mx;
        ev.mouse_y = my;
        ev.mouse_buttons = MOUSE_BUTTON_RIGHT;
        wm_queue_event(&ev);
    }
    // Queue mouse button up (left)
    if (!(buttons & MOUSE_LEFT_BTN) && (s_wpi_last_btn & MOUSE_LEFT_BTN)) {
        gui_event_t ev = {0};
        ev.type = EVENT_MOUSE_UP;
        ev.mouse_x = mx;
        ev.mouse_y = my;
        ev.mouse_buttons = MOUSE_BUTTON_LEFT;
        wm_queue_event(&ev);
    }
    // Queue mouse button up (right)
    if (!(buttons & MOUSE_RIGHT_BTN) && (s_wpi_last_btn & MOUSE_RIGHT_BTN)) {
        gui_event_t ev = {0};
        ev.type = EVENT_MOUSE_UP;
        ev.mouse_x = mx;
        ev.mouse_y = my;
        ev.mouse_buttons = MOUSE_BUTTON_RIGHT;
        wm_queue_event(&ev);
    }

    s_wpi_last_mx  = mx;
    s_wpi_last_my  = my;
    s_wpi_last_btn = buttons;

    // Poll keyboard
    extern int keyboard_has_char(void);
    extern int keyboard_get_char(void);
    while (keyboard_has_char()) {
        int c = keyboard_get_char();
        gui_event_t ev = {0};
        if (c >= 0x90 && c <= 0x98) {
            ev.type = EVENT_KEY_UP;
            ev.keycode = c - 0x10;
        } else if (c >= 0x80 && c < 0x90) {
            ev.type = EVENT_KEY_DOWN;
            ev.keycode = c;
        } else if (c > 0x98) {
            ev.type = EVENT_KEY_UP;
            ev.keycode = c & 0x7F;
        } else {
            ev.type = EVENT_KEY_DOWN;
            ev.keycode = c;
            ev.key_char = c;
        }
        wm_queue_event(&ev);
    }

    // Dispatch queued events to window manager and apps
    gui_event_t event;
    while (wm_poll_event(&event)) {
        switch (event.type) {
            case EVENT_MOUSE_MOVE:
                wm_handle_mouse_move(event.mouse_x, event.mouse_y);
                break;
            case EVENT_MOUSE_DOWN:
                if (window_get_at_point(event.mouse_x, event.mouse_y)) {
                    wm_handle_mouse_down(event.mouse_x, event.mouse_y, event.mouse_buttons);
                }
                break;
            case EVENT_MOUSE_UP:
                wm_handle_mouse_up(event.mouse_x, event.mouse_y, event.mouse_buttons);
                break;
            case EVENT_KEY_DOWN:
                wm_handle_key_down(event.keycode, event.key_char);
                break;
            case EVENT_KEY_UP:
                wm_handle_key_up(event.keycode);
                break;
            default:
                break;
        }
        wm_dispatch_event(&event);
    }
}

// Process one desktop frame (called from idle or syscall)
void desktop_process_tick(void) {
    if (!g_desktop.initialized) return;
    {
        // Win16 app owns the screen: its pump is the sole key consumer.
        extern volatile int g_win16_owns_screen;
        if (g_win16_owns_screen) return;
    }
    // Once the usermode compositor is launched it owns the screen; the kernel
    // desktop must not draw (would flash the kernel taskbar/icons over the
    // boot splash before the compositor's first frame).
    extern int g_compositor_launched;
    if (g_compositor_launched) return;
    if (wm_is_exclusive_mode()) return;
    
    // Quick mouse position update
    int32_t mouse_x, mouse_y;
    uint8_t buttons;
    mouse_get_position(&mouse_x, &mouse_y);
    buttons = mouse_get_buttons();
    
    // Check for keyboard input - throttled to 100 Hz to prevent
    // multiple events per physical keypress at 250 Hz timer rate
    extern int keyboard_has_char(void);
    extern int keyboard_get_char(void);
    uint32_t kb_interval = (g_timer_hz >= 20) ? (g_timer_hz / 20) : 1;
    if (timer_ticks - s_last_kb_tick >= kb_interval) {
        s_last_kb_tick = timer_ticks;
    while (keyboard_has_char()) {
        int c = keyboard_get_char();

        // Ignore ALL keypresses during first 2 seconds of boot.
        // The keyboard controller sends spurious scancodes (ESC, F12, etc.)
        // during initialization that can trigger unwanted actions.
        if (timer_ticks < 2 * g_timer_hz) {
            continue;
        }

        gui_event_t key_event = {0};
        // Check if this is a key release:
        // 1. Special release codes (0x90-0x98): subtract 0x10 to get press code
        // 2. Regular ASCII with bit 7 set (> 0x98): clear bit 7
        if (c >= 0x90 && c <= 0x98) {
            // Special key release (Ctrl, Shift, arrows)
            key_event.type = EVENT_KEY_UP;
            key_event.keycode = c - 0x10;
            key_event.key_char = 0;
        } else if (c >= 0x80 && c < 0x90) {
            // Special key press (arrows, ctrl, shift, etc.)
            key_event.type = EVENT_KEY_DOWN;
            key_event.keycode = c;
            key_event.key_char = 0;
        } else if (c > 0x98) {
            // Regular ASCII key release (ASCII | 0x80)
            key_event.type = EVENT_KEY_UP;
            key_event.keycode = c & 0x7F;
            key_event.key_char = 0;
        } else {
            // Regular key press (ASCII < 0x80)
            key_event.type = EVENT_KEY_DOWN;
            key_event.keycode = c;
            key_event.key_char = c;
        }
        wm_queue_event(&key_event);
    }
    // Any keyboard input dismisses screensaver
    screensaver_on_input();
    } // end kb_interval throttle

    // Check for mouse button events  
    static uint8_t last_buttons = 0;
    static int32_t last_mouse_x = -1, last_mouse_y = -1;
    if (buttons != last_buttons) {
        // Queue mouse button down event
        if ((buttons & MOUSE_LEFT_BTN) && !(last_buttons & MOUSE_LEFT_BTN)) {
            gui_event_t down_event = {0};
            down_event.type = EVENT_MOUSE_DOWN;
            down_event.mouse_x = mouse_x;
            down_event.mouse_y = mouse_y;
            down_event.mouse_buttons = MOUSE_BUTTON_LEFT;
            wm_queue_event(&down_event);
        }

        // Queue mouse button up event
        if (!(buttons & MOUSE_LEFT_BTN) && (last_buttons & MOUSE_LEFT_BTN)) {
            gui_event_t up_event = {0};
            up_event.type = EVENT_MOUSE_UP;
            up_event.mouse_x = mouse_x;
            up_event.mouse_y = mouse_y;
            up_event.mouse_buttons = MOUSE_BUTTON_LEFT;
            wm_queue_event(&up_event);
        }
        last_buttons = buttons;
        screensaver_on_input();  // Mouse button dismisses screensaver
    }

    // Check for mouse movement and update cursor
    if (mouse_x != last_mouse_x || mouse_y != last_mouse_y) {
        // Queue mouse move event
        gui_event_t move_event = {0};
        move_event.type = EVENT_MOUSE_MOVE;
        move_event.mouse_x = mouse_x;
        move_event.mouse_y = mouse_y;
        move_event.mouse_buttons = buttons;
        wm_queue_event(&move_event);
        
        last_mouse_x = mouse_x;
        last_mouse_y = mouse_y;
        
        // Force redraw for cursor update
        wm_invalidate_all();
        screensaver_on_input();  // Mouse movement dismisses screensaver
    }
    
    // Screensaver: update idle timer, activate if idle, draw if active
    screensaver_update();
    if (screensaver_is_active()) {
        screensaver_draw();
        extern void fb_swap_buffers(void);
        fb_swap_buffers();
        return;  // Skip normal WM processing while screensaver is active
    }

    // Process queued events through window manager (like desktop_run does)
    gui_event_t event;
    while (wm_poll_event(&event)) {
        switch (event.type) {
            case EVENT_MOUSE_DOWN:
                // First check if click is on a window
                if (window_get_at_point(event.mouse_x, event.mouse_y)) {
                    wm_handle_mouse_down(event.mouse_x, event.mouse_y, event.mouse_buttons);
                } else {
                    // Click is on desktop
                    if (event.mouse_buttons & MOUSE_BUTTON_LEFT) {
                        desktop_handle_click(event.mouse_x, event.mouse_y);
                    }
                }
                break;

            case EVENT_MOUSE_MOVE:
                wm_handle_mouse_move(event.mouse_x, event.mouse_y);
                break;

            case EVENT_MOUSE_UP:
                wm_handle_mouse_up(event.mouse_x, event.mouse_y, event.mouse_buttons);
                break;

            default:
                break;
        }

        // Dispatch event to registered apps
        wm_dispatch_event(&event);
    }
    
    // Redraw if dirty
    if (wm_is_dirty()) {
        desktop_draw();
        wm_draw_all();
        // Blit user-mode app content buffers into their windows.
        extern void wm_draw_apps(void);
        wm_draw_apps();
        wm_draw_winmenu();   // Task A: decorator popup on top of app content
        // Draw cursor on top
        desktop_draw_cursor(mouse_x, mouse_y);
        extern void fb_swap_buffers(void);
        fb_swap_buffers();
        wm_clear_dirty();
    }
}
