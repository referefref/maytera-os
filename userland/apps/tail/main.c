// tail - print the last part of files
// Usage: tail [-n N|-n +N] [-c N|-c +N] [-q] [-v] [FILE...]
//
// #745 (local 108, second batch). THE DEFECT: this read the FIRST 64 KB of the
// file into a fixed buffer and then printed "the last ten lines" of that. On
// any file bigger than 64 KB - a log, which is what tail exists for - it
// printed ten lines from somewhere near the start, with no error and exit 0.
// It was also single-operand and parsed its count with atoi().
//
// THE FIX IS NOT A BIGGER BUFFER. It keeps a WINDOW: bytes are appended and
// the front is discarded as soon as more than the requested tail is held, so
// memory is bounded by (compaction threshold + the tail itself) and the input
// may be any size at all. A cap that is merely larger is still a cap, and it
// would still be silent.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "getopt.h"
#include "mtool.h"

#define COMPACT_AT (1u << 20)   // trim the window once it exceeds 1 MB

typedef struct {
	long count;
	int  by_bytes;      // -c
	int  from_start;    // "+N": start AT that line/byte instead of ending there
	int  headers;       // 1 always, 0 never, -2 = only when >1 file
	int  nfiles;
	int  printed_any;
} opts_t;

// Offset of the start of the last `n` lines of [buf,len). See the comment in
// the header of this file: a buffer that does not end in a newline still has a
// final line, which the old code got wrong for that case too.
static size_t tail_line_start(const char *buf, size_t len, long n)
{
	if (len == 0 || n <= 0) return len;
	long count = (buf[len - 1] != '\n') ? 1 : 0;
	for (size_t i = len; i > 0; i--) {
		if (buf[i - 1] == '\n') {
			count++;
			if (count == n + 1) return i;
		}
	}
	return 0;
}

static void hdr(opts_t *o, const char *operand)
{
	int want = (o->headers == 1) || (o->headers == -2 && o->nfiles > 1);
	if (!want) { o->printed_any = 1; return; }
	const char *name = (operand[0] == '-' && operand[1] == '\0')
	                 ? "standard input" : operand;
	if (o->printed_any) mtool_wall(1, "\n", 1);
	mtool_wfmt(1, "==> %s <==\n", name);
	o->printed_any = 1;
}

// "+N": stream straight through, skipping the first N-1 lines (or bytes).
static int tail_from_start(int fd, opts_t *o, const char *operand)
{
	char buf[8192];
	long skipped = 0;          // lines or bytes already discarded
	int emitting = (o->count <= 1);
	long n;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		long i = 0;
		if (!emitting) {
			if (o->by_bytes) {
				long want = (o->count - 1) - skipped;
				if (want > n) { skipped += n; continue; }
				i = want; skipped += want; emitting = 1;
			} else {
				for (; i < n; i++) {
					if (buf[i] == '\n') {
						skipped++;
						if (skipped == o->count - 1) { i++; emitting = 1; break; }
					}
				}
				if (!emitting) continue;
			}
		}
		if (mtool_wall(1, buf + i, (size_t)(n - i)) != 0) {
			mtool_warn("write error");
			return 1;
		}
	}
	if (n < 0) { mtool_warn("%s: read error", operand); return 1; }
	return 0;
}

static int do_one(const char *operand, void *ctx)
{
	opts_t *o = (opts_t *)ctx;
	int fd = mtool_open_read(operand);
	if (fd < 0) return 1;
	hdr(o, operand);

	int rc = 0;
	if (o->from_start) {
		rc = tail_from_start(fd, o, operand);
		mtool_close_read(fd);
		return rc;
	}

	size_t cap = 65536, len = 0;
	char *win = (char *)malloc(cap);
	if (!win) { mtool_warn("out of memory"); mtool_close_read(fd); return 1; }

	char buf[8192];
	long n;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		if (len + (size_t)n > cap) {
			size_t ncap = cap;
			while (len + (size_t)n > ncap) ncap *= 2;
			char *nw = (char *)realloc(win, ncap);
			if (!nw) { free(win); mtool_warn("out of memory"); mtool_close_read(fd); return 1; }
			win = nw; cap = ncap;
		}
		memcpy(win + len, buf, (size_t)n);
		len += (size_t)n;
		if (len > COMPACT_AT) {
			size_t keep = o->by_bytes
			            ? ((len > (size_t)o->count) ? len - (size_t)o->count : 0)
			            : tail_line_start(win, len, o->count);
			if (keep > 0) {
				memmove(win, win + keep, len - keep);
				len -= keep;
			}
		}
	}
	if (n < 0) { mtool_warn("%s: read error", operand); rc = 1; }
	mtool_close_read(fd);

	size_t start = o->by_bytes
	             ? ((len > (size_t)o->count) ? len - (size_t)o->count : 0)
	             : tail_line_start(win, len, o->count);
	if (mtool_wall(1, win + start, len - start) != 0) { mtool_warn("write error"); rc = 1; }
	free(win);
	return rc;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	int eargc;
	char **eargv = mtool_expand_count_opts(argc, argv, &eargc, 1, "nc");

	opts_t o;
	memset(&o, 0, sizeof o);
	o.count = 10;
	o.headers = -2;
	int c;
	while ((c = getopt(eargc, eargv, "n:c:qv")) != -1) {
		switch (c) {
		case 'n':
		case 'c': {
			const char *a = optarg;
			int plus = 0;
			if (*a == '+') { plus = 1; a++; }
			else if (*a == '-') a++;      // "-n -5" is the same as "-n 5"
			o.count = mtool_count_arg(c == 'n' ? "lines" : "bytes", a);
			o.by_bytes = (c == 'c');
			o.from_start = plus;
			if (plus && o.count == 0) o.count = 1;   // "+0" means the whole file
			break;
		}
		case 'q': o.headers = 0; break;
		case 'v': o.headers = 1; break;
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			if (optopt == 'f' || optopt == 'F')
				mtool_refuse("tail -f (follow)",
				             "there is no file-change notification here and a "
				             "poll loop would be the hand-rolled busy-wait the "
				             "project bans; not implemented rather than faked");
			mtool_bad_option(b);
		}
		}
	}
	o.nfiles = eargc - optind;
	return mtool_each_operand(eargc, eargv, optind, do_one, &o, NULL);
}
