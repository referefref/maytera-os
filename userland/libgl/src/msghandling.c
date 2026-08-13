#include "msghandling.h"
#include "../include/GL/gl.h"
#include "zgl.h"
#include <stdarg.h>


#ifdef __TINYC__
#define NO_DEBUG_OUTPUT
#endif

/* AssaultCube port phase 3: gl_fatal_error() now prints unconditionally
 * (see that function below), so stdio.h is needed regardless of
 * NO_DEBUG_OUTPUT. */
#include <stdio.h>
/* Use this function to output messages when something unexpected
   happens (which might be an indication of an error). *Don't* use it
   when there's GLinternal errors in the code - these should be handled
   by asserts. */
void tgl_warning(const char* format, ...) {
#ifndef NO_DEBUG_OUTPUT
	va_list args;
	va_start(args, format);
	fprintf(stderr, "*WARNING* ");
	vfprintf(stderr, format, args);
	va_end(args);
#endif /* !NO_DEBUG_OUTPUT */
}

/* This function should be used for debug output only. */
void tgl_trace(const char* format, ...) {
#ifndef NO_DEBUG_OUTPUT
	va_list args;
	va_start(args, format);
	fprintf(stderr, "*DEBUG* ");
	vfprintf(stderr, format, args);
	va_end(args);
#endif /* !NO_DEBUG_OUTPUT */
}

/* Use this function to output info about things in the code which
   should be fixed (missing handling of special cases, important
   features not implemented, known bugs/buglets, ...). */
void tgl_fixme(const char* format, ...) {
#ifndef NO_DEBUG_OUTPUT
	va_list args;
	va_start(args, format);
	fprintf(stderr, "*FIXME* ");
	vfprintf(stderr, format, args);
	va_end(args);
#endif /* !NO_DEBUG_OUTPUT */
}

void gl_fatal_error(char* format, ...) {
	/* AssaultCube port phase 3 (docs/ASSAULTCUBE_PORT_PLAN.md): this
	 * function terminates the process either way, so gating its message on
	 * NO_DEBUG_OUTPUT (a flag meant to silence hot-path per-frame tgl_warning
	 * /tgl_trace/tgl_fixme spam, see those functions above) bought no
	 * performance and cost real debuggability: a real, standards-legal
	 * caller (AssaultCube's own createtexture(), see glopTexImage2D's fix)
	 * hit this path and the process just vanished with exit(1) and zero
	 * output, in a build that DOES define NO_DEBUG_OUTPUT (see the Makefile).
	 * A fatal exit should never be silent regardless of that flag. */
	va_list ap;
	va_start(ap, format);
#ifndef NO_DEBUG_OUTPUT
	fprintf(stderr, "TinyGL: fatal error: ");
	vfprintf(stderr, format, ap);
	fprintf(stderr, "\n");
#else
	printf("TinyGL: fatal error: ");
	vprintf(format, ap);
	printf("\n");
#endif
	va_end(ap);
	exit(1);
}
