// version.h - MayteraOS Version Information
#ifndef VERSION_H
#define VERSION_H

// Version format: MAJOR.MINOR.PATCH (e.g., 1.8.2)
#define MAYTERA_VERSION_MAJOR    1
#define MAYTERA_VERSION_MINOR    26
#define MAYTERA_VERSION_PATCH    0

// Build number (increment for each build)
#define MAYTERA_BUILD_NUMBER 311

// Version string helper macros
#define STRINGIFY(x) STRINGIFY_HELPER(x)
#define STRINGIFY_HELPER(x) #x

#define MAYTERA_VERSION_STRING "1.26.0"

// Build date (set at compile time)
#define MAYTERA_BUILD_DATE       __DATE__
#define MAYTERA_BUILD_TIME       __TIME__

// Full version string
#define MAYTERA_FULL_VERSION     "MayteraOS v" MAYTERA_VERSION_STRING

// Changelog for this version
#define MAYTERA_CHANGELOG \
    "v1.9.0 - May 2026\n" \
    "- IPC subsystem: message passing syscalls (160-166)\n" \
    "- IPC subsystem: shared memory syscalls (170-174)\n" \
    "- IPC name service for process discovery\n" \
    "- Framebuffer syscalls wired (200-213)\n" \
    "- Window manager query API (SYS_WM_GET_WINDOWS)\n" \
    "- Userland compositor infrastructure (Phase 1, idle)\n" \
    "- Exclusive mode support for compositor takeover\n" \
    "- Mouse state dedup to prevent drain loops\n" \
    "- Phase 2 compositor: real user-mode rendering\n" \
    "- Compositor renders background, taskbar, clock, cursor\n" \
    "- SYS_COMPOSITOR_RENDER_WINDOWS syscall (156)\n" \
    "- Direct framebuffer mode for compositor\n" \
    "- Input event extraction for compositor syscall\n"

#endif // VERSION_H
