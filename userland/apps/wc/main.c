// wc - count lines, words and bytes
// Usage: wc [-c] [-l] [-w] [-L] [FILE...]
//
// #745 (local 108, second batch). Was `if (argc >= 2) open(argv[1])` and then
// one set of counts, so `wc a b` counted a, printed a's name, and exited 0.
// It also accepted no options at all: `wc -l f` opened a file called "-l",
// failed, and exited 1 - which at least was loud, unlike its siblings.
//
// Counts are `long long` because the old ones were `int`: a 2 GB file made the
// byte count negative, silently.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "getopt.h"
#include "mtool.h"

typedef struct {
	int want_lines, want_words, want_bytes, want_maxlen;
	int nfiles;
	long long tl, tw, tb;
	long long tmax;
} opts_t;

static void emit(opts_t *o, long long l, long long w, long long b, long long m,
                 const char *name)
{
	char out[1100];
	int n = 0;
	// GNU right-aligns these in a width it computes from the input size. That
	// column width is cosmetic and deliberately not reproduced; the oracle
	// (tools/coreutils-oracle) collapses runs of spaces for wc and says so.
	if (o->want_lines)  n += snprintf(out + n, sizeof out - (size_t)n, "%s%lld", n ? " " : "", l);
	if (o->want_words)  n += snprintf(out + n, sizeof out - (size_t)n, "%s%lld", n ? " " : "", w);
	if (o->want_bytes)  n += snprintf(out + n, sizeof out - (size_t)n, "%s%lld", n ? " " : "", b);
	if (o->want_maxlen) n += snprintf(out + n, sizeof out - (size_t)n, "%s%lld", n ? " " : "", m);
	if (name && *name)  n += snprintf(out + n, sizeof out - (size_t)n, " %s", name);
	n += snprintf(out + n, sizeof out - (size_t)n, "\n");
	mtool_wall(1, out, (size_t)n);
}

static int do_one(const char *operand, void *ctx)
{
	opts_t *o = (opts_t *)ctx;
	int fd = mtool_open_read(operand);
	if (fd < 0) return 1;

	long long lines = 0, words = 0, bytes = 0, maxlen = 0, curlen = 0;
	int in_word = 0;
	char buf[8192];
	long n;
	int failed = 0;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		bytes += n;
		for (long i = 0; i < n; i++) {
			char c = buf[i];
			if (c == '\n') { lines++; if (curlen > maxlen) maxlen = curlen; curlen = 0; }
			else curlen++;
			if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
			    c == '\f' || c == '\v') in_word = 0;
			else { if (!in_word) words++; in_word = 1; }
		}
	}
	if (n < 0) { mtool_warn("%s: read error", operand); failed = 1; }
	if (curlen > maxlen) maxlen = curlen;
	mtool_close_read(fd);

	o->tl += lines; o->tw += words; o->tb += bytes;
	if (maxlen > o->tmax) o->tmax = maxlen;
	// "-" as an operand prints as "-", exactly as wc(1) does; standard input
	// with no operand at all prints no name.
	emit(o, lines, words, bytes, maxlen, o->nfiles ? operand : "");
	return failed;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	opts_t o;
	memset(&o, 0, sizeof o);
	int c, any = 0;
	while ((c = getopt(argc, argv, "clwL")) != -1) {
		switch (c) {
		case 'l': o.want_lines = 1; any = 1; break;
		case 'w': o.want_words = 1; any = 1; break;
		case 'c': o.want_bytes = 1; any = 1; break;
		case 'L': o.want_maxlen = 1; any = 1; break;
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			if (optopt == 'm')
				// -m is CHARACTERS. With no multibyte support in this libc it
				// would be identical to -c, which is right in the C locale and
				// silently wrong for any UTF-8 input - the exact shape this
				// ticket exists to remove.
				mtool_refuse("wc -m (character counts)",
				             "this libc has no multibyte support, so -m could "
				             "only ever return the byte count; use -c");
			mtool_bad_option(b);
		}
		}
	}
	if (!any) { o.want_lines = o.want_words = o.want_bytes = 1; }
	o.nfiles = argc - optind;
	int rc = mtool_each_operand(argc, argv, optind, do_one, &o, NULL);
	if (o.nfiles > 1) emit(&o, o.tl, o.tw, o.tb, o.tmax, "total");
	return rc;
}
