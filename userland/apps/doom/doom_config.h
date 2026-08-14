// doom_config.h - Build configuration
#ifndef DOOM_CONFIG_H
#define DOOM_CONFIG_H

// Define DOOM_NODEBUG to 1 to disable all debug output
#define DOOM_NODEBUG 0

#if DOOM_NODEBUG
// Make printf a no-op in release builds
// We do this by redefining it after stdio.h is included
static inline int doom_noop_printf(const char *fmt, ...) { (void)fmt; return 0; }
#define printf doom_noop_printf
#endif

#endif
