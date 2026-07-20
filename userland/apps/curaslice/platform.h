// platform.h - MayteraOS glue for the vendored CuraEngine 15.04 sources.
//
// Included ONLY by the handful of upstream files that use std::string, so that
// we can substitute std::string -> pstring (a malloc-backed mstring) without
// dragging libstdc++'s basic_string / iostream / locale runtime into a
// freestanding -nostdlib link (see CURAENGINE_PORT_PLAN.md section 3.4).
//
// It also pulls the MayteraOS libc headers (resolved first via -I../../libc)
// and provides tiny compile-time compat, keeping the upstream .cpp/.h edits to
// a single "#include" line plus the mechanical std::string -> pstring rename.
#ifndef CURASLICE_PLATFORM_H
#define CURASLICE_PLATFORM_H

// Header strategy (PORT-STATUS.md): C++ translation units compile against the
// HOST toolchain headers (glibc prototypes + libstdc++ templates) because
// libstdc++'s <cstdlib> etc. include_next into the host C headers, which
// conflict with the MayteraOS libc declarations if both are visible. The
// prototypes for everything CuraEngine calls (malloc/free, str*, mem*, printf
// family, fopen/fwrite, math) are ABI-identical, and the -nostdlib link against
// MayteraOS libc.a resolves the actual symbols. Any glibc-only symbol leaking
// through (checked-printf, _IO_*, errno location) fails the LINK loudly and is
// then stubbed or patched at the source, never silently.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// Malloc-backed std::string stand-in and the project-wide alias used by the
// substituted sources.
#include "mstring.h"
typedef mstring pstring;

// c++11 dropped the M_PI guarantee; libc math.h defines it, but keep a guard in
// case a future libc trims it. intpoint.h has an identical fallback.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#endif // CURASLICE_PLATFORM_H
