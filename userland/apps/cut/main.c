// cut - select fields or byte ranges from each line
// Usage: cut -f LIST [-d DELIM] [-s] [FILE...]
//        cut -b LIST [FILE...]
//        cut -c LIST [FILE...]
//
// #745 (local 108, second batch). THE DEFECT: -f was `atoi(arg)`, so
// `cut -f1,3` silently produced field 1 and `cut -f2-4` silently produced
// field 2 - a wrong answer with exit 0, on the exact syntax that makes cut
// worth using. There was no -c and no -b at all, and an unrecognised option
// was SKIPPED: `cut -s -f2` set no flag and carried on.
//
// It also read lines into a fixed 1024-byte buffer and, on a longer line,
// SPLIT it silently and cut both halves.
//
// -c AND -b ARE THE SAME THING HERE, and that is stated rather than hidden:
// this libc has no multibyte support, so a "character" is a byte. That makes
// both correct for single-byte data and makes -c differ from GNU cut under a
// UTF-8 locale. The oracle runs with LC_ALL=C for this reason.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "getopt.h"
#include "mtool.h"

#define MAX_RANGES 128

typedef struct { long lo, hi; } range_t;   // 1-based, inclusive; hi == 0 means "to end"

typedef struct {
	range_t r[MAX_RANGES];
	int     n;
	int     by_field;      // -f
	char    delim;         // -d
	int     only_delim;    // -s
} opts_t;

static int selected(const opts_t *o, long idx)
{
	for (int i = 0; i < o->n; i++)
		if (idx >= o->r[i].lo && (o->r[i].hi == 0 || idx <= o->r[i].hi)) return 1;
	return 0;
}

// LIST is N, N-M, N-, -M, joined by commas. Anything else is a REFUSAL, not a
// best guess: atoi() answering 1 for "1,3" is what this replaces.
static void parse_list(opts_t *o, const char *spec, const char *what)
{
	const char *p = spec;
	o->n = 0;
	if (!*p) mtool_die(MTOOL_EX_FAIL, "%s: empty list", what);
	while (*p) {
		if (o->n >= MAX_RANGES) mtool_die(MTOOL_EX_FAIL, "%s: too many ranges", what);
		long lo = 0, hi = 0;
		int have_lo = 0;
		while (*p >= '0' && *p <= '9') { lo = lo * 10 + (*p++ - '0'); have_lo = 1; }
		if (*p == '-') {
			p++;
			int have_hi = 0;
			while (*p >= '0' && *p <= '9') { hi = hi * 10 + (*p++ - '0'); have_hi = 1; }
			if (!have_lo) lo = 1;                 // "-M"
			if (!have_hi) hi = 0;                 // "N-"
			if (!have_lo && !have_hi)
				mtool_die(MTOOL_EX_FAIL, "invalid range in %s: '%s'", what, spec);
		} else {
			if (!have_lo) mtool_die(MTOOL_EX_FAIL, "invalid list in %s: '%s'", what, spec);
			hi = lo;
		}
		if (lo == 0) mtool_die(MTOOL_EX_FAIL, "%s are numbered from 1", what);
		if (hi != 0 && hi < lo)
			mtool_die(MTOOL_EX_FAIL, "invalid decreasing range in %s: '%s'", what, spec);
		o->r[o->n].lo = lo;
		o->r[o->n].hi = hi;
		o->n++;
		if (*p == ',') { p++; continue; }
		if (*p) mtool_die(MTOOL_EX_FAIL, "invalid character '%c' in %s: '%s'", *p, what, spec);
	}
}

static void emit_line(const opts_t *o, const char *line, size_t len)
{
	char out[8192];
	size_t n = 0;
	if (!o->by_field) {
		for (size_t i = 0; i < len && n + 1 < sizeof out; i++)
			if (selected(o, (long)i + 1)) out[n++] = line[i];
	} else {
		// Split on the delimiter. A line with NO delimiter is passed through
		// whole, unless -s, exactly as cut(1) specifies.
		int has = 0;
		for (size_t i = 0; i < len; i++) if (line[i] == o->delim) { has = 1; break; }
		if (!has) {
			if (o->only_delim) return;
			mtool_wall(1, line, len);
			mtool_wall(1, "\n", 1);
			return;
		}
		long f = 1;
		size_t start = 0;
		int wrote = 0;
		for (size_t i = 0; i <= len; i++) {
			if (i == len || line[i] == o->delim) {
				if (selected(o, f)) {
					if (wrote && n + 1 < sizeof out) out[n++] = o->delim;
					for (size_t k = start; k < i && n + 1 < sizeof out; k++)
						out[n++] = line[k];
					wrote = 1;
				}
				f++;
				start = i + 1;
			}
		}
	}
	out[n] = '\0';
	mtool_wall(1, out, n);
	mtool_wall(1, "\n", 1);
}

static int do_one(const char *operand, void *ctx)
{
	opts_t *o = (opts_t *)ctx;
	int fd = mtool_open_read(operand);
	if (fd < 0) return 1;
	size_t len = 0;
	char *buf = mtool_slurp_fd(fd, &len);        // no silent line-length cap
	mtool_close_read(fd);
	if (!buf) return 1;
	size_t start = 0;
	for (size_t i = 0; i <= len; i++) {
		if (i == len || buf[i] == '\n') {
			if (i == len && i == start) break;    // trailing newline: no empty last line
			emit_line(o, buf + start, i - start);
			start = i + 1;
		}
	}
	free(buf);
	return 0;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	opts_t o;
	memset(&o, 0, sizeof o);
	o.delim = '\t';
	int mode = 0;    // 'f', 'c' or 'b'
	int c;
	while ((c = getopt(argc, argv, "f:c:b:d:s")) != -1) {
		switch (c) {
		case 'f': case 'c': case 'b':
			if (mode) mtool_die(MTOOL_EX_FAIL,
			                    "only one of -b, -c or -f may be given");
			mode = c;
			o.by_field = (c == 'f');
			parse_list(&o, optarg, c == 'f' ? "field list" : "byte list");
			break;
		case 'd':
			if (optarg[0] == '\0' || optarg[1] != '\0')
				mtool_refuse("cut -d with a multi-character delimiter",
				             "the delimiter is one byte, as in cut(1)");
			o.delim = optarg[0];
			break;
		case 's': o.only_delim = 1; break;
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			mtool_bad_option(b);
		}
		}
	}
	if (!mode)
		mtool_die(MTOOL_EX_FAIL, "you must specify a list of bytes (-b), "
		                         "characters (-c) or fields (-f)");
	if (!o.by_field && (o.only_delim || o.delim != '\t'))
		mtool_refuse("cut -d/-s with -b or -c",
		             "a delimiter only means something for -f");
	return mtool_each_operand(argc, argv, optind, do_one, &o, NULL);
}
