// tac - concatenate and print files, last line first
// Usage: tac [FILE...]
//
// #745 (local 108, second batch). TWO SILENT CAPS: a 64 KB whole-file buffer
// and a 4096-entry line index. Past either one, tac printed a reversed
// FRAGMENT and exited 0 - and the 64 KB truncation took the END of the file,
// which is the half tac is asked for.
//
// The read and the line index now come from userland/libc/mtool.c, which grows
// and which fails rather than truncating, so this file has no cap of its own
// and neither do tail, sort and less.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "getopt.h"
#include "mtool.h"

static int do_one(const char *operand, void *ctx)
{
	(void)ctx;
	int fd = mtool_open_read(operand);
	if (fd < 0) return 1;
	size_t len = 0;
	char *buf = mtool_slurp_fd(fd, &len);
	mtool_close_read(fd);
	if (!buf) return 1;
	if (len == 0) { free(buf); return 0; }

	size_t nlines = 0;
	size_t *off = mtool_index_lines(buf, len, &nlines);
	if (!off) { free(buf); return 1; }

	int rc = 0;
	for (size_t i = nlines; i > 0; i--) {
		size_t start = off[i - 1];
		size_t end = (i < nlines) ? off[i] : len;
		// Each record carries its OWN separator or none at all, and nothing is
		// added. MEASURED against GNU tac 9.1 rather than assumed: `printf
		// 'a\nb' | tac` is "ba\n", not "b\na\n" - the incomplete final record
		// stays incomplete and simply moves to the front. The tac this replaces
		// appended a newline here, so it disagreed with tac(1) on every file
		// that does not end in one.
		if (mtool_wall(1, buf + start, end - start) != 0) { rc = 1; break; }
	}
	if (rc) mtool_warn("write error");
	free(off);
	free(buf);
	return rc;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	int c;
	while ((c = getopt(argc, argv, "")) != -1) {
		char b[4] = { '-', (char)optopt, 0, 0 };
		(void)c;
		if (optopt == 'r' || optopt == 's')
			mtool_refuse("tac -r / -s (a custom or regular-expression separator)",
			             "only newline-separated records are implemented");
		if (optopt == 'b')
			mtool_refuse("tac -b (separator before the record)",
			             "not implemented");
		mtool_bad_option(b);
	}
	return mtool_each_operand(argc, argv, optind, do_one, NULL, NULL);
}
