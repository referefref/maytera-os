// uniq - filter adjacent matching lines
// Usage: uniq [-c] [-d] [-u] [-i] [INPUT [OUTPUT]]
//
// #745 (local 108, THIRD batch). WHAT WAS WRONG, and the headline is not the
// missing flags:
//
//  1. A 1024-BYTE LINE BUFFER THAT SPLIT LONGER LINES AND THEN DE-DUPED THE
//     HALVES. The old loop appended bytes to `line[1024]` and, on reaching
//     `line_pos >= sizeof(line) - 1`, treated that as END OF LINE. So a 3000-
//     character line became three records, and if two such lines shared a
//     1023-byte prefix - which any two long lines from the same generator do -
//     uniq DELETED one of the halves. That is worse than truncating: truncation
//     loses data visibly, this INVENTS adjacency that was never in the input and
//     then acts on it. Fixed by not having a line buffer at all: the input is
//     read whole with mtool_slurp_fd() (no cap, and it FAILS rather than
//     returning a fragment) and indexed with mtool_index_lines().
//
//  2. THE SECOND OPERAND WAS IGNORED. `uniq in out` is uniq(1)'s documented
//     form and it wrote to STDOUT, leaving `out` untouched. A user who typed it
//     got an empty output file and no error.
//
//  3. NO -c, -d, -u, -i, and an unrecognised option was silently taken as the
//     INPUT FILENAME: `uniq -c f` tried to open a file called "-c".
//
// WHAT IS DELIBERATELY REFUSED: -f (skip fields), -s (skip chars), -w (compare
// at most N chars), -D (print all of each duplicate group) and -z. Each is a
// real feature with a real definition and no partial version of it is useful;
// they are named individually so the message tells the user what to type
// instead of what not to.
//
// THE TRAILING NEWLINE WAS AN EXPECTATION I GOT WRONG, AND THE ORACLE IS WHAT
// SAID SO. The first version of this file emitted each record's bytes INCLUDING
// its '\n' when it had one, reasoning that uniq writes back what it read, so
// `printf 'no trailing newline' | uniq` produced no final newline. GNU uniq
// prints one. That is a difference no amount of reading POSIX would have
// settled with confidence, and it is the same lesson this tree has now paid for
// five times (blame.md: a hand-written expectation for a standard tool is a
// guess with a straight face). Every emitted record is therefore terminated,
// which is a no-op for the records that already were.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "getopt.h"
#include "mtool.h"

static int opt_count, opt_dup_only, opt_uniq_only, opt_ignore_case;

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

// Compare two lines EXCLUDING their trailing newline, so "a\n" at the end of a
// file equals "a" at the end of one that stops short.
static int same_line(const char *buf, size_t a, size_t alen, size_t b, size_t blen)
{
	if (alen && buf[a + alen - 1] == '\n') alen--;
	if (blen && buf[b + blen - 1] == '\n') blen--;
	if (alen != blen) return 0;
	for (size_t i = 0; i < alen; i++) {
		char ca = buf[a + i], cb = buf[b + i];
		if (opt_ignore_case) { ca = (char)lower((unsigned char)ca); cb = (char)lower((unsigned char)cb); }
		if (ca != cb) return 0;
	}
	return 1;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);

	int c;
	while ((c = getopt(argc, argv, "cduif:s:w:Dz")) != -1) {
		switch (c) {
		case 'c': opt_count = 1; break;
		case 'd': opt_dup_only = 1; break;
		case 'u': opt_uniq_only = 1; break;
		case 'i': opt_ignore_case = 1; break;
		case 'f': mtool_refuse("uniq -f N (skip the first N fields)",
		                       "field splitting is not implemented; pipe through "
		                       "cut -f to drop the fields first");
		case 's': mtool_refuse("uniq -s N (skip the first N characters)",
		                       "not implemented; pipe through cut -c to drop them first");
		case 'w': mtool_refuse("uniq -w N (compare no more than N characters)",
		                       "not implemented; pipe through cut -c to shorten the lines first");
		case 'D': mtool_refuse("uniq -D (print every line of a duplicate group)",
		                       "not implemented; -d prints one line per duplicate group");
		case 'z': mtool_refuse("uniq -z (NUL-terminated lines)",
		                       "this build has no NUL-delimited input support");
		default:  mtool_bad_option(argv[optind - 1]);
		}
	}

	if (argc - optind > 2)
		mtool_die(MTOOL_EX_FAIL, "extra operand '%s' (usage: uniq [-cdui] [INPUT [OUTPUT]])",
		          argv[optind + 2]);

	const char *inop  = (optind < argc)     ? argv[optind]     : "-";
	const char *outop = (optind + 1 < argc) ? argv[optind + 1] : NULL;

	int in = mtool_open_read(inop);
	if (in < 0) return MTOOL_EX_FAIL;

	int out = 1;
	if (outop) {
		char full[1024];
		if (mtool_resolve(outop, full, sizeof full) != 0)
			mtool_die(MTOOL_EX_FAIL, "%s: path too long", outop);
		out = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) mtool_die(MTOOL_EX_FAIL, "%s: cannot create (error %d)", outop, out);
	}

	size_t len = 0;
	char *buf = mtool_slurp_fd(in, &len);
	mtool_close_read(in);
	if (!buf) return MTOOL_EX_FAIL;

	size_t nlines = 0;
	size_t *off = mtool_index_lines(buf, len, &nlines);
	if (!off && nlines) return MTOOL_EX_FAIL;

	int rc = MTOOL_EX_OK;
	for (size_t i = 0; i < nlines; ) {
		size_t start = off[i];
		size_t end   = (i + 1 < nlines) ? off[i + 1] : len;
		size_t j = i + 1;
		while (j < nlines) {
			size_t s2 = off[j], e2 = (j + 1 < nlines) ? off[j + 1] : len;
			if (!same_line(buf, start, end - start, s2, e2 - s2)) break;
			j++;
		}
		unsigned long n = (unsigned long)(j - i);

		// -d and -u together select nothing, exactly as uniq(1) does.
		int show = 1;
		if (opt_dup_only  && n < 2) show = 0;
		if (opt_uniq_only && n > 1) show = 0;

		if (show) {
			size_t body = end - start;
			if (body && buf[end - 1] == '\n') body--;   // terminate it ourselves
			// uniq(1)'s -c format is a 7-wide right-aligned count and one space.
			if (opt_count && mtool_wfmt(out, "%7lu ", n) != 0) { rc = MTOOL_EX_FAIL; break; }
			if (mtool_wall(out, buf + start, body) != 0) { rc = MTOOL_EX_FAIL; break; }
			if (mtool_wall(out, "\n", 1) != 0) { rc = MTOOL_EX_FAIL; break; }
		}
		i = j;
	}
	if (rc != MTOOL_EX_OK) mtool_warn("write error");
	if (out != 1) close(out);
	return rc;
}
