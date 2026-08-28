// head - print the first part of files
// Usage: head [-n N] [-c N] [-q] [-v] [FILE...]
//
// #745 (local 108, second batch). Was single-operand (`head f1 f2` printed f1
// and exited 0), had no -c, and parsed its count with atoi(), so `head -n x f`
// printed nothing and exited 0. It also never emitted the `==> name <==`
// headers, so `head *.txt` - if it had accepted more than one file - would have
// produced an unlabelled run-together blob.
//
// STREAMING, not slurping: head must not read a gigabyte to print ten lines,
// and it must stop reading so an upstream `producer | head` can find out its
// consumer has gone (there is no SIGPIPE here; see mtool.h).
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "getopt.h"
#include "mtool.h"

typedef struct {
	long count;        // lines or bytes
	int  by_bytes;     // -c
	int  headers;      // -1 always, 0 never, -2 = only when >1 file
	int  nfiles;
	int  printed_any;
} opts_t;

static int do_one(const char *operand, void *ctx)
{
	opts_t *o = (opts_t *)ctx;
	int fd = mtool_open_read(operand);
	if (fd < 0) return 1;

	int want_hdr = (o->headers == 1) || (o->headers == -2 && o->nfiles > 1);
	if (want_hdr) {
		const char *name = (operand[0] == '-' && operand[1] == '\0')
		                 ? "standard input" : operand;
		if (o->printed_any) mtool_wall(1, "\n", 1);
		mtool_wfmt(1, "==> %s <==\n", name);
	}
	o->printed_any = 1;

	char buf[8192];
	long done = 0;
	int failed = 0;
	while (done < o->count) {
		long n = read(fd, buf, sizeof buf);
		if (n < 0) { mtool_warn("%s: read error", operand); failed = 1; break; }
		if (n == 0) break;
		long emit = n;
		if (o->by_bytes) {
			if (done + emit > o->count) emit = o->count - done;
			done += emit;
		} else {
			// Find the byte just past the Nth newline in this chunk.
			long i = 0, taken = 0;
			for (; i < n && done + taken < o->count; i++)
				if (buf[i] == '\n') taken++;
			emit = i;
			done += taken;
		}
		if (mtool_wall(1, buf, (size_t)emit) != 0) {
			// The consumer went away. Say so and stop; do not spin.
			mtool_warn("write error");
			failed = 1;
			break;
		}
	}
	mtool_close_read(fd);
	return failed;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	int eargc;
	char **eargv = mtool_expand_count_opts(argc, argv, &eargc, 0, "nc");

	opts_t o;
	memset(&o, 0, sizeof o);
	o.count = 10;
	o.headers = -2;
	int c;
	while ((c = getopt(eargc, eargv, "n:c:qv")) != -1) {
		switch (c) {
		case 'n':
			if (optarg[0] == '-' || optarg[0] == '+')
				mtool_refuse("head -n with a sign (-n -N / -n +N)",
				             "only a plain count is implemented; -n -N means "
				             "'all but the last N' and needs the whole input "
				             "buffered, which this build does not do");
			o.count = mtool_count_arg("lines", optarg);
			o.by_bytes = 0;
			break;
		case 'c':
			if (optarg[0] == '-' || optarg[0] == '+')
				mtool_refuse("head -c with a sign (-c -N / -c +N)",
				             "only a plain byte count is implemented");
			o.count = mtool_count_arg("bytes", optarg);
			o.by_bytes = 1;
			break;
		case 'q': o.headers = 0; break;
		case 'v': o.headers = 1; break;
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			mtool_bad_option(b);
		}
		}
	}
	o.nfiles = eargc - optind;
	return mtool_each_operand(eargc, eargv, optind, do_one, &o, NULL);
}
