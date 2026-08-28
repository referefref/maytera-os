// uname - print system information
// Usage: uname [-a] [-s] [-r] [-v] [-m] [-o]
//
// #745 (local 108, THIRD batch).
//
// WHAT WAS WRONG, AND IT IS THIS PROJECT'S RECURRING SHAPE. The whole program
// was two printf()s. `uname -a` printed the string literal
//
//     "MayteraOS maytera 1.8.2 x86_64"
//
// while the tree was at 2.0.1 build 1885, and EVERY other flag - -s, -r, -v, -m
// - was ignored, so `uname -r` printed "MayteraOS". Two separate defects there:
// a version that had been wrong for many releases and could only ever go
// further wrong, and a hostname ("maytera") that this OS does not have.
//
// A CORRECT SHARED IMPLEMENTATION WAS SITTING UNUSED NEXT DOOR.
// userland/libc/sys/utsname.c already asks the KERNEL for its version through
// SYS_GET_VERSION (246), splits it into release and version, and documents
// field by field where each one comes from - including why nodename is the
// empty string rather than an invented "localhost". It is compiled into libc.a
// (SYS_SRCS in userland/libc/Makefile) and had exactly one caller: the libc's
// own header test. So the shared, correct, tested implementation shipped in
// every binary on the image while the program named after it printed a
// hardcoded lie. That is the same shape as local 98 (the real GNU grep shipping
// beside a 67-line fake under the plain name) and local 108 batch 2 (a wall
// clock with four consumers while /APPS/DATE printed uptime).
//
// THE VERSION HAS EXACTLY ONE SOURCE AND THIS FILE DOES NOT ADD A SECOND.
// build/version-gate.sh FAILS the build on a second definition of
// MAYTERA_VERSION_STRING, and rightly: the kernel is the only thing that knows
// which kernel is running. This asks it at run time, the same way the
// compositor's About screen, Settings, /APPS/SYSLOG and the OTA updater all do
// (get_version() in userland/libc/syscall.h). Nothing here can go stale.
//
// NODENAME IS OMITTED, NOT INVENTED. GNU's -a prints the node name second.
// MayteraOS has no hostname: the installer asks for one and discards it, there
// is no gethostname(), no kernel field and no config key (see the provenance
// block in userland/libc/sys/utsname.h). Printing "maytera", "localhost" or an
// empty column would all be fabrications, so -a prints the four fields that are
// real and says on STDERR that it left one out and why. The note goes to fd 2
// so that `uname -a` in a pipeline is still clean, parseable output; -n itself
// is a loud refusal, because that is where somebody is actually asking for the
// thing we do not have.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "errno.h"
#include "getopt.h"
#include "sys/utsname.h"
#include "mtool.h"

int main(int argc, char **argv)
{
	int want_s = 0, want_r = 0, want_v = 0, want_m = 0, want_o = 0, want_a = 0;
	int c;

	mtool_setprog(argv[0]);

	while ((c = getopt(argc, argv, "asrvmonpi")) != -1) {
		switch (c) {
		case 'a': want_a = 1; break;
		case 's': want_s = 1; break;
		case 'r': want_r = 1; break;
		case 'v': want_v = 1; break;
		case 'm': want_m = 1; break;
		case 'o': want_o = 1; break;
		case 'n': mtool_refuse("uname -n (node name)",
		                       "MayteraOS has no hostname facility at all - no "
		                       "gethostname(), no kernel field and no config key. "
		                       "The installer asks for a name and discards it. "
		                       "Wiring one in starts at userland/libc/sys/utsname.c");
		case 'p': mtool_refuse("uname -p (processor type)",
		                       "this OS records no processor model; -m reports the "
		                       "architecture, which is x86_64");
		case 'i': mtool_refuse("uname -i (hardware platform)",
		                       "this OS records no platform identifier; -m reports "
		                       "the architecture, which is x86_64");
		default:  mtool_bad_option(argv[optind - 1]);
		}
	}
	if (optind < argc)
		mtool_die(MTOOL_EX_FAIL, "extra operand '%s'", argv[optind]);

	struct utsname u;
	if (uname(&u) != 0)
		mtool_die(MTOOL_EX_FAIL,
		          "the kernel would not report its version (SYS_GET_VERSION "
		          "failed, errno %d); refusing to print a version we do not know",
		          errno);

	if (want_a) { want_s = want_r = want_v = want_m = want_o = 1; }
	if (!want_s && !want_r && !want_v && !want_m && !want_o) want_s = 1;

	char out[320];
	int n = 0;
	// Field order is uname(1)'s: sysname, release, version, machine, then the
	// operating system. Node name would sit between sysname and release; see the
	// header for why there is nothing truthful to put there.
	if (want_s) n += snprintf(out + n, sizeof out - (size_t)n, "%s%s", n ? " " : "", u.sysname);
	if (want_r) n += snprintf(out + n, sizeof out - (size_t)n, "%s%s", n ? " " : "", u.release);
	if (want_v) n += snprintf(out + n, sizeof out - (size_t)n, "%s%s", n ? " " : "", u.version);
	if (want_m) n += snprintf(out + n, sizeof out - (size_t)n, "%s%s", n ? " " : "", u.machine);
	if (want_o) n += snprintf(out + n, sizeof out - (size_t)n, "%s%s", n ? " " : "", "MayteraOS");
	if (n < 0 || n >= (int)sizeof out) return MTOOL_EX_FAIL;

	if (want_a)
		mtool_warn("node name omitted: this OS has no hostname facility "
		           "(uname -n explains where one would be wired in)");

	if (mtool_wfmt(1, "%s\n", out) != 0) {
		mtool_warn("write error on standard output");
		return MTOOL_EX_FAIL;
	}
	return MTOOL_EX_OK;
}
