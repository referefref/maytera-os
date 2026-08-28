// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// mtool.c - implementation of the shared command-line-tool spine. See mtool.h
//           for WHY this exists; this file is the how.
//
// KEEP ONLY mtool_* FUNCTIONS IN THIS FILE (see the note at the end of
// mtool.h). It is linked by a dozen tiny apps and must not drag anything else
// out of the archive with it.
#include <stdarg.h>

#include "types.h"
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "fcntl.h"
#include "mtool.h"

static const char *g_prog = "mtool";

void mtool_setprog(const char *argv0)
{
	if (!argv0 || !*argv0) return;
	const char *base = argv0;
	for (const char *p = argv0; *p; p++)
		if (*p == '/' || *p == '\\') base = p + 1;
	if (*base) g_prog = base;
}

const char *mtool_prog(void) { return g_prog; }

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------
int mtool_wall(int fd, const void *buf, size_t len)
{
	const char *p = (const char *)buf;
	size_t off = 0;
	while (off < len) {
		long w = write(fd, p + off, len - off);
		// w == 0 is not progress. On this kernel a full pipe ring returns 0
		// and a dead reader returns -1; treating 0 as "keep going" is how a
		// producer spins at 100% CPU, so only a negative result is fatal but
		// a zero must not be counted as bytes written.
		if (w < 0) return -1;
		if (w == 0) continue;
		off += (size_t)w;
	}
	return 0;
}

int mtool_wfmt(int fd, const char *fmt, ...)
{
	char b[1024];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	if (n < 0) return -1;
	if (n > (int)sizeof b - 1) n = (int)sizeof b - 1;
	return mtool_wall(fd, b, (size_t)n);
}

// ---------------------------------------------------------------------------
// diagnostics: fd 2, always. Never stdout - a diagnostic on stdout corrupts
// the pipeline this tool may be a stage of.
// ---------------------------------------------------------------------------
static void vwarn(const char *fmt, va_list ap)
{
	char b[1024];
	int n = snprintf(b, sizeof b, "%s: ", g_prog);
	if (n < 0) n = 0;
	int m = vsnprintf(b + n, sizeof b - (size_t)n, fmt, ap);
	if (m < 0) m = 0;
	int total = n + m;
	if (total > (int)sizeof b - 2) total = (int)sizeof b - 2;
	b[total] = '\n';
	b[total + 1] = '\0';
	(void)mtool_wall(2, b, (size_t)total + 1);
}

void mtool_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vwarn(fmt, ap);
	va_end(ap);
}

void mtool_die(int status, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vwarn(fmt, ap);
	va_end(ap);
	_exit(status);
	for (;;) { }
}

void mtool_refuse(const char *what, const char *why)
{
	if (why && *why)
		mtool_warn("%s is not implemented: %s", what, why);
	else
		mtool_warn("%s is not implemented", what);
	// The second line is the contract this ticket is enforcing, spelled out
	// once here rather than in a dozen apps: the tool did NOT do a fraction of
	// the job and exit 0.
	mtool_warn("refusing rather than answering a different question (exit %d)",
	           MTOOL_EX_REFUSE);
	_exit(MTOOL_EX_REFUSE);
	for (;;) { }
}

void mtool_bad_option(const char *opt)
{
	if (opt && *opt)
		mtool_warn("unrecognised option '%s'", opt);
	else
		mtool_warn("unrecognised option");
	mtool_warn("this tool refuses options it does not implement rather than "
	           "ignoring them (exit %d)", MTOOL_EX_REFUSE);
	_exit(MTOOL_EX_REFUSE);
	for (;;) { }
}

// ---------------------------------------------------------------------------
// the operand loop - the whole reason this file exists
// ---------------------------------------------------------------------------
int mtool_each_operand(int argc, char **argv, int first,
                       mtool_operand_fn fn, void *ctx, const char *missing)
{
	if (first >= argc) {
		if (missing) mtool_die(MTOOL_EX_FAIL, "%s", missing);
		// POSIX: no file operands means standard input, exactly once.
		return fn("-", ctx) == 0 ? MTOOL_EX_OK : MTOOL_EX_FAIL;
	}
	int status = MTOOL_EX_OK;
	for (int i = first; i < argc; i++) {
		// Keep going after a failure. `rm a missing c` must still remove c,
		// and must still exit non-zero. Stopping at the first error is a
		// different bug from only looking at the first operand, and it is just
		// as surprising.
		if (fn(argv[i], ctx) != 0) status = MTOOL_EX_FAIL;
	}
	return status;
}

// ---------------------------------------------------------------------------
// paths
//
// The kernel STORES a cwd (sys_chdir) and NOTHING READS IT: open() takes the
// path it is given. That is why tac and less each hand-rolled a getcwd()+join
// and why every other tool here silently only worked on absolute paths. One
// definition, here.
// ---------------------------------------------------------------------------
// #745 local 108, THIRD batch: NORMALISE, do not merely join.
//
// The join alone produced "<cwd>/." for the single most common relative operand
// there is, and "<cwd>/../x" for the second. Neither is a path this kernel's
// open()/opendir() resolves: there is no path canonicaliser under them, so a
// "." component is looked up as a directory entry literally named ".". ls asks
// for "." on every bare `ls`, which is how this surfaced; every other caller of
// mtool_resolve() has the same latent hole for any operand a user types with a
// leading ./ or ../.
//
// EXTENDED HERE RATHER THAN WORKED AROUND IN ls, per the owner rule: a private
// "." special case in ls would have been the second copy, and the next tool
// would have needed a third. All fourteen existing callers were re-run against
// the corpus afterwards (see tools/coreutils-oracle).
//
// The rule is purely lexical, which is correct on this OS precisely BECAUSE it
// has no symbolic links (kernel/fs/ext2.c has zero hits for S_IFLNK), so
// "a/../b" and "b" cannot denote different objects here.
#define MTOOL_MAX_COMPONENTS 64

static int normalise(char *s)
{
	char  *comp[MTOOL_MAX_COMPONENTS];
	size_t clen[MTOOL_MAX_COMPONENTS];
	int n = 0;
	char *p = s + 1;            // s is absolute; the leading '/' is kept

	while (*p) {
		char *start = p;
		while (*p && *p != '/') p++;
		size_t len = (size_t)(p - start);
		if (len == 0) { }                                        // "//"
		else if (len == 1 && start[0] == '.') { }                 // "/./"
		else if (len == 2 && start[0] == '.' && start[1] == '.') { if (n) n--; }
		else {
			// A depth bound that silently DROPPED components would be the same
			// silent-cap defect this whole ticket is about, so it fails instead.
			if (n == MTOOL_MAX_COMPONENTS) return -1;
			comp[n] = start; clen[n] = len; n++;
		}
		if (*p == '/') p++;
	}

	// Writes strictly left of where it reads, in order, so no component is
	// overwritten before it is copied.
	char *w = s + 1;
	for (int i = 0; i < n; i++) {
		if (i) *w++ = '/';
		for (size_t k = 0; k < clen[i]; k++) *w++ = comp[i][k];
	}
	*w = '\0';
	if (w == s + 1) { s[0] = '/'; s[1] = '\0'; }   // everything cancelled: root
	return 0;
}

static int resolve(const char *path, char *out, size_t outsz)
{
	if (!path || !*path) return -1;
	if (path[0] == '/') {
		size_t n = strlen(path);
		if (n + 1 > outsz) return -1;
		memcpy(out, path, n + 1);
		return normalise(out);
	}
	char cwd[512];
	if (!getcwd(cwd, sizeof cwd)) { cwd[0] = '/'; cwd[1] = '\0'; }
	size_t c = strlen(cwd);
	if (c == 0) { cwd[0] = '/'; cwd[1] = '\0'; c = 1; }
	size_t p = strlen(path);
	size_t need = c + 1 + p + 1;
	if (need > outsz) return -1;
	memcpy(out, cwd, c);
	if (out[c - 1] != '/') out[c++] = '/';
	memcpy(out + c, path, p + 1);
	return normalise(out);
}

int mtool_resolve(const char *path, char *out, size_t outsz)
{
	return resolve(path, out, outsz);
}

int mtool_open_read(const char *operand)
{
	if (operand && operand[0] == '-' && operand[1] == '\0') return 0;
	char full[1024];
	if (resolve(operand, full, sizeof full) != 0) {
		mtool_warn("%s: path too long", operand ? operand : "(null)");
		return -1;
	}
	int fd = open(full, O_RDONLY);
	if (fd < 0) {
		mtool_warn("%s: cannot open (error %d)", operand, fd);
		return -1;
	}
	return fd;
}

void mtool_close_read(int fd)
{
	if (fd > 0) close(fd);
}

// ---------------------------------------------------------------------------
// slurp - no cap, and a failure is a failure
// ---------------------------------------------------------------------------
char *mtool_slurp_fd(int fd, size_t *len_out)
{
	size_t cap = 65536, len = 0;
	char *buf = (char *)malloc(cap);
	if (!buf) { mtool_warn("out of memory"); return NULL; }
	for (;;) {
		if (len + 4096 + 1 > cap) {
			size_t ncap = cap * 2;
			char *nb = (char *)realloc(buf, ncap);
			if (!nb) {
				free(buf);
				// The whole point of this function: it does NOT hand back the
				// first 64 KB and let the caller print a confident wrong
				// answer, which is exactly what tail/tac/sort/less did.
				mtool_warn("out of memory after %lu bytes; refusing to answer "
				           "from a truncated read", (unsigned long)len);
				return NULL;
			}
			buf = nb;
			cap = ncap;
		}
		long n = read(fd, buf + len, cap - len - 1);
		if (n < 0) { free(buf); mtool_warn("read error"); return NULL; }
		if (n == 0) break;
		len += (size_t)n;
	}
	buf[len] = '\0';
	if (len_out) *len_out = len;
	return buf;
}

size_t *mtool_index_lines(const char *buf, size_t len, size_t *count_out)
{
	if (count_out) *count_out = 0;
	if (!buf || len == 0) return NULL;
	size_t cap = 256, n = 0;
	size_t *off = (size_t *)malloc(cap * sizeof(size_t));
	if (!off) { mtool_warn("out of memory"); return NULL; }
	off[n++] = 0;
	for (size_t i = 0; i < len; i++) {
		if (buf[i] == '\n' && i + 1 < len) {
			if (n == cap) {
				size_t ncap = cap * 2;
				size_t *no = (size_t *)realloc(off, ncap * sizeof(size_t));
				if (!no) { free(off); mtool_warn("out of memory"); return NULL; }
				off = no;
				cap = ncap;
			}
			off[n++] = i + 1;
		}
	}
	if (count_out) *count_out = n;
	return off;
}

// ---------------------------------------------------------------------------
// the obsolete "-NUM" count syntax
// ---------------------------------------------------------------------------
static int all_digits(const char *s)
{
	if (!*s) return 0;
	for (const char *p = s; *p; p++)
		if (*p < '0' || *p > '9') return 0;
	return 1;
}

#define MTOOL_MAXARGV 256

char **mtool_expand_count_opts(int argc, char **argv, int *out_argc, int allow_plus,
                               const char *argopts)
{
	// A word that is the ARGUMENT of an option is never a count word of its
	// own. Without this, "tail -n +5" became "tail -n -n 5".
	int consumed[MTOOL_MAXARGV];
	int nmark = argc < MTOOL_MAXARGV ? argc : MTOOL_MAXARGV;
	for (int i = 0; i < nmark; i++) consumed[i] = 0;
	for (int i = 1; i + 1 < nmark; i++) {
		const char *a = argv[i];
		if (a[0] != '-' || a[1] == '\0' || a[2] != '\0') continue;
		if (argopts && strchr(argopts, a[1])) consumed[i + 1] = 1;
	}

	int hits = 0;
	for (int i = 1; i < argc; i++) {
		if (i < nmark && consumed[i]) continue;
		if (argv[i][0] == '-' && all_digits(argv[i] + 1)) hits++;
		else if (allow_plus && argv[i][0] == '+' && all_digits(argv[i] + 1)) hits++;
	}
	if (hits == 0) { *out_argc = argc; return argv; }

	char **out = (char **)malloc((size_t)(argc + hits + 1) * sizeof(char *));
	if (!out) { *out_argc = argc; return argv; }   // degrade to refusing, not to guessing
	int n = 0;
	out[n++] = argv[0];
	for (int i = 1; i < argc; i++) {
		int skip = (i < nmark && consumed[i]);
		int dash = (!skip && argv[i][0] == '-' && all_digits(argv[i] + 1));
		int plus = (!skip && allow_plus && argv[i][0] == '+' && all_digits(argv[i] + 1));
		if (dash) {
			out[n++] = (char *)"-n";
			out[n++] = argv[i] + 1;
		} else if (plus) {
			out[n++] = (char *)"-n";
			out[n++] = argv[i];        // keep the '+': "-n +5" is a real form
		} else {
			out[n++] = argv[i];
		}
	}
	out[n] = NULL;
	*out_argc = n;
	return out;
}

// ---------------------------------------------------------------------------
// numeric option arguments
// ---------------------------------------------------------------------------
long mtool_count_arg(const char *opt, const char *s)
{
	if (!s || !*s) mtool_die(MTOOL_EX_FAIL, "%s requires a number", opt);
	const char *p = s;
	long v = 0;
	while (*p) {
		if (*p < '0' || *p > '9')
			// atoi() answers 0 for "x" and 12 for "12x", and every tool here
			// used atoi(). `head -n x` printed nothing and exited 0.
			mtool_die(MTOOL_EX_FAIL, "invalid number of %s: '%s'", opt, s);
		v = v * 10 + (*p - '0');
		if (v > 1000000000L) v = 1000000000L;   // clamp, do not wrap
		p++;
	}
	return v;
}
