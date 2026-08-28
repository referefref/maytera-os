// sort - sort lines of text
// Usage: sort [-b] [-c] [-f] [-n] [-r] [-u] [-o FILE] [FILE...]
//
// #745 (local 108, second batch). TWO SILENT CAPS, both of which produced a
// confidently wrong answer with exit 0:
//   * MAX_LINES 1000: line 1001 onwards was DISCARDED. `sort bigfile` printed
//     a sorted first thousand lines and said nothing.
//   * MAX_LINE_LEN 256: a longer line was SPLIT into two lines mid-word, which
//     were then sorted separately.
// It also took exactly one file, had no options at all (every option was
// treated as a filename, which is at least loud), and used a bubble sort, so
// a 1000-line input was a million comparisons.
//
// Now: no caps (the input is read with mtool_slurp_fd and indexed with
// mtool_index_lines, both of which grow and both of which FAIL rather than
// truncate), the libc's qsort, and the options people actually type. Options
// this does not implement are REFUSED by name rather than ignored.
//
// COMPARISON ORDER, which is the one place a "simple" sort quietly disagrees
// with sort(1): the default comparison is a byte comparison (the C locale) and
// a key comparison that reports equality falls back to comparing whole lines,
// which is sort(1)'s "last-resort comparison". -u turns that fall-back OFF, so
// `sort -nu` keeps one line per numeric key, exactly like GNU. The oracle runs
// with LC_ALL=C because any other locale changes GNU's answer, not ours.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "ctype.h"
#include "getopt.h"
#include "mtool.h"

typedef struct {
	int numeric, reverse, fold, ignore_blanks, unique, check;
} sopts_t;

static sopts_t g;

static const char *skip_blanks(const char *s)
{
	while (*s == ' ' || *s == '\t') s++;
	return s;
}

// The numeric key: optional blanks, optional sign, digits, optional fraction.
// A line with no leading number has key 0, exactly as sort -n does.
static double numkey(const char *s)
{
	s = skip_blanks(s);
	int neg = 0;
	if (*s == '-') { neg = 1; s++; }
	else if (*s == '+') s++;
	double v = 0;
	while (*s >= '0' && *s <= '9') v = v * 10.0 + (*s++ - '0');
	if (*s == '.') {
		s++;
		double scale = 0.1;
		while (*s >= '0' && *s <= '9') { v += (*s++ - '0') * scale; scale *= 0.1; }
	}
	return neg ? -v : v;
}

static int cmp_text(const char *a, const char *b)
{
	if (g.ignore_blanks) { a = skip_blanks(a); b = skip_blanks(b); }
	if (!g.fold) return strcmp(a, b);
	for (;;) {
		unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
		unsigned char la = (unsigned char)tolower(ca), lb = (unsigned char)tolower(cb);
		if (la != lb) return la < lb ? -1 : 1;
		if (!ca) return 0;
	}
}

// The KEY comparison alone: what -u dedupes on.
static int cmp_key(const char *a, const char *b)
{
	if (g.numeric) {
		double x = numkey(a), y = numkey(b);
		if (x < y) return -1;
		if (x > y) return 1;
		return 0;
	}
	return cmp_text(a, b);
}

static int cmp_full(const char *a, const char *b)
{
	int r = cmp_key(a, b);
	// The last-resort comparison. Without it two lines with the same numeric
	// key come out in whatever order qsort happened to leave them, which is a
	// different answer from sort(1) on the same input.
	if (r == 0 && !g.unique) r = strcmp(a, b);
	return r;
}

static int cmp_qsort(const void *pa, const void *pb)
{
	const char *a = *(const char *const *)pa;
	const char *b = *(const char *const *)pb;
	int r = cmp_full(a, b);
	return g.reverse ? -r : r;
}

typedef struct {
	char **line;
	size_t n, cap;
	char **blocks;      // the slurped buffers, kept alive; lines point into them
	size_t nblocks, bcap;
} lines_t;

static void push_block(lines_t *L, char *b)
{
	if (L->nblocks == L->bcap) {
		size_t nc = L->bcap ? L->bcap * 2 : 8;
		char **nb = (char **)realloc(L->blocks, nc * sizeof(char *));
		if (!nb) mtool_die(MTOOL_EX_FAIL, "out of memory");
		L->blocks = nb; L->bcap = nc;
	}
	L->blocks[L->nblocks++] = b;
}

static void push_line(lines_t *L, char *s)
{
	if (L->n == L->cap) {
		size_t nc = L->cap ? L->cap * 2 : 1024;
		char **nl = (char **)realloc(L->line, nc * sizeof(char *));
		if (!nl) mtool_die(MTOOL_EX_FAIL, "out of memory");
		L->line = nl; L->cap = nc;
	}
	L->line[L->n++] = s;
}

static int read_into(const char *operand, void *ctx)
{
	lines_t *L = (lines_t *)ctx;
	int fd = mtool_open_read(operand);
	if (fd < 0) return 1;
	size_t len = 0;
	char *buf = mtool_slurp_fd(fd, &len);
	mtool_close_read(fd);
	if (!buf) return 1;
	push_block(L, buf);
	// Split in place: every newline becomes a NUL, so each line is a C string
	// pointing into the block. No per-line copy and no length limit.
	size_t start = 0;
	for (size_t i = 0; i <= len; i++) {
		if (i == len) {
			if (i > start) push_line(L, buf + start);   // no trailing newline
			break;
		}
		if (buf[i] == '\n') {
			buf[i] = '\0';
			push_line(L, buf + start);
			start = i + 1;
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	memset(&g, 0, sizeof g);
	const char *outfile = NULL;
	int c;
	while ((c = getopt(argc, argv, "bcfnruo:")) != -1) {
		switch (c) {
		case 'b': g.ignore_blanks = 1; break;
		case 'c': g.check = 1; break;
		case 'f': g.fold = 1; break;
		case 'n': g.numeric = 1; break;
		case 'r': g.reverse = 1; break;
		case 'u': g.unique = 1; break;
		case 'o': outfile = optarg; break;
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			if (optopt == 'k' || optopt == 't')
				mtool_refuse("sort -k / -t (sort by a field)",
				             "only whole-line keys are implemented; -t alone "
				             "does nothing without -k, so accepting it would be "
				             "the silent no-op this ticket exists to remove");
			if (optopt == 'M' || optopt == 'V' || optopt == 'h' || optopt == 'R')
				mtool_refuse("sort -M/-V/-h/-R (month, version, human, random "
				             "orderings)", "not implemented");
			if (optopt == 's')
				mtool_refuse("sort -s (stable)",
				             "the sort under this is not stable, so -s could "
				             "only be accepted and ignored");
			mtool_bad_option(b);
		}
		}
	}

	lines_t L;
	memset(&L, 0, sizeof L);
	int rc = mtool_each_operand(argc, argv, optind, read_into, &L, NULL);

	if (g.check) {
		for (size_t i = 1; i < L.n; i++) {
			int r = cmp_full(L.line[i - 1], L.line[i]);
			if (g.reverse) r = -r;
			if (r > 0) {
				mtool_warn("disorder at line %lu: %s", (unsigned long)(i + 1), L.line[i]);
				return MTOOL_EX_FAIL;
			}
		}
		return rc;
	}

	if (L.n > 1) qsort(L.line, L.n, sizeof(char *), cmp_qsort);

	int out = 1;
	if (outfile) {
		char full[1024];
		if (mtool_resolve(outfile, full, sizeof full) != 0)
			mtool_die(MTOOL_EX_FAIL, "%s: path too long", outfile);
		out = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) mtool_die(MTOOL_EX_FAIL, "%s: cannot open for writing (error %d)",
		                       outfile, out);
	}

	for (size_t i = 0; i < L.n; i++) {
		if (g.unique && i > 0 && cmp_key(L.line[i - 1], L.line[i]) == 0) continue;
		size_t len = strlen(L.line[i]);
		if (mtool_wall(out, L.line[i], len) != 0 || mtool_wall(out, "\n", 1) != 0) {
			mtool_warn("write error");
			return MTOOL_EX_FAIL;
		}
	}
	if (out != 1) close(out);
	return rc;
}
