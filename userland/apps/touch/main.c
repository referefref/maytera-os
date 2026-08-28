// touch - create empty files and set modification/access times
// Usage: touch [-a] [-c] [-m] [-r REFFILE] FILE...
//
// #745 (local 108, second batch) fixed two defects here:
//
//   1. SILENT AND MULTI-OPERAND: it was `open(argv[1], O_CREAT|O_WRONLY)`, so
//      `touch a b c` created a and exited 0. Fixed by the shared operand loop
//      (userland/libc/mtool.c).
//
//   2. SILENT AND FUNDAMENTAL: touch's actual job is to set a file's
//      modification time, and THIS KERNEL COULD NOT. So touching an existing
//      file was refused, loudly, rather than answering a different question
//      with exit 0.
//
// #115 (local 120) REMOVES THE SECOND REFUSAL BY FIXING WHAT CAUSED IT.
// SYS_UTIME exists, libc's utime()/utimes() are real, and sys_stat_path()
// reports the result, so touch does its actual job now.
//
// WHAT "NOW" MEANS, and why touch does not compute it. utime(path, NULL) asks
// the KERNEL for the current time. touch must NOT build one from time(),
// because on this OS time() and gettimeofday() return SECONDS SINCE BOOT
// (#113): a touch that sent its own clock would stamp every file with
// 1970-01-01 plus the uptime, which is a wrong value that looks like a right
// one - the whole subject of #115. The kernel reads the RTC; userland does not
// have a calendar to send.
//
// WHAT IS STILL REFUSED, and why it is a refusal and not a gap:
//   -t and -d take a DATE STRING. This libc has no strptime()/getdate(), and a
//   hand-rolled parser that accepts a format it does not fully implement would
//   silently stamp the wrong instant. -r (copy the times from a reference file)
//   does the same job with no parsing and IS implemented.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "getopt.h"
#include "mtool.h"
#include "sys/stat.h"
#include "utime.h"
#include "errno.h"

typedef struct {
	int  no_create;      // -c
	int  set_atime;      // -a
	int  set_mtime;      // -m
	int  have_ref;       // -r was given
	long ref_atime;
	long ref_mtime;
} opts_t;

// Apply the requested times to a path that is known to exist.
// Returns 0 on success, 1 on a per-operand failure.
static int apply_times(const char *operand, const char *full, const opts_t *o)
{
	// Default (neither -a nor -m): both, which is what touch(1) does.
	int want_a = o->set_atime || (!o->set_atime && !o->set_mtime);
	int want_m = o->set_mtime || (!o->set_atime && !o->set_mtime);

	struct utimbuf ub;
	const struct utimbuf *arg;
	if (o->have_ref) {
		// UTIME_KEEP is the kernel's "leave this one alone" sentinel; using it
		// keeps `touch -a -r ref f` from disturbing f's mtime.
		ub.actime  = want_a ? o->ref_atime : (long)UTIME_KEEP;
		ub.modtime = want_m ? o->ref_mtime : (long)UTIME_KEEP;
		arg = &ub;
	} else if (want_a && want_m) {
		arg = 0;                       // NULL == "now", decided by the kernel
	} else {
		ub.actime  = want_a ? (long)UTIME_NOW : (long)UTIME_KEEP;
		ub.modtime = want_m ? (long)UTIME_NOW : (long)UTIME_KEEP;
		arg = &ub;
	}

	if (utime(full, arg) == 0) return 0;
	// Report the real reason. ENOSYS here means the backend cannot do it (SMB,
	// NFS, or an RTC that has no plausible date), which is a refusal by the
	// layer that knows, not a touch bug - so name it rather than printing a
	// bare error number.
	if (errno == ENOSYS)
		mtool_warn("cannot set times on '%s': the filesystem or the clock "
		           "cannot supply them (SMB and NFS set-times are not "
		           "implemented; an unset RTC has no calendar date)", operand);
	else
		mtool_warn("cannot set times on '%s': error %d", operand, errno);
	return 1;
}

static int do_one(const char *operand, void *ctx)
{
	opts_t *o = (opts_t *)ctx;
	char full[1024];
	if (mtool_resolve(operand, full, sizeof full) != 0) {
		mtool_warn("%s: path too long", operand);
		return 1;
	}
	struct stat st;
	if (stat(full, &st) == 0) return apply_times(operand, full, o);

	if (o->no_create) return 0;      // -c: do not create, do not complain
	// O_CREAT|O_WRONLY and deliberately NOT O_TRUNC.
	int fd = open(full, O_CREAT | O_WRONLY, 0644);
	if (fd < 0) {
		mtool_warn("cannot touch '%s': error %d", operand, fd);
		return 1;
	}
	close(fd);
	// The kernel stamps a newly created file with the current time already
	// (#115, fs/ext2.c and fs/fat.c), so the plain case needs nothing more.
	// An EXPLICIT request still has to be honoured: with -r, the file must end
	// up carrying the reference file's times, not its creation time.
	if (o->have_ref) return apply_times(operand, full, o);
	return 0;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	opts_t o;
	memset(&o, 0, sizeof o);
	int c;
	while ((c = getopt(argc, argv, "acmr:")) != -1) {
		switch (c) {
		case 'c': o.no_create = 1; break;
		case 'a': o.set_atime = 1; break;
		case 'm': o.set_mtime = 1; break;
		case 'r': {
			char ref[1024];
			if (mtool_resolve(optarg, ref, sizeof ref) != 0)
				mtool_die(MTOOL_EX_FAIL, "-r %s: path too long", optarg);
			struct stat rst;
			if (stat(ref, &rst) != 0)
				mtool_die(MTOOL_EX_FAIL, "cannot stat reference '%s': error %d",
				          optarg, errno);
			// A reference file whose own times are UNKNOWN (0) cannot be
			// copied: stamping the target with 0 would write "unknown" as if
			// it were a date, and stamping it with 1970 would invent one.
			// Every file written by this OS before #115 is in this state.
			if (rst.st_mtime == 0 && rst.st_atime == 0)
				mtool_die(MTOOL_EX_FAIL,
				          "reference '%s' carries no timestamp (0 means the "
				          "filesystem does not know, not 1970-01-01); there is "
				          "nothing to copy", optarg);
			o.have_ref  = 1;
			o.ref_atime = (long)rst.st_atime;
			o.ref_mtime = (long)rst.st_mtime;
			break;
		}
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			if (optopt == 't' || optopt == 'd')
				mtool_refuse("touch -t / -d (an explicit date string)",
				             "this libc has no strptime()/getdate(), and a "
				             "partial date parser would silently stamp the "
				             "wrong instant. Use -r REFFILE, which needs no "
				             "parsing and is implemented");
			mtool_bad_option(b);
		}
		}
	}
	return mtool_each_operand(argc, argv, optind, do_one, &o, "missing file operand");
}
