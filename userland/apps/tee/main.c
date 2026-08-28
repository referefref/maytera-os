// tee - copy standard input to standard output and to files
// Usage: tee [-a] FILE...
//
// #745 (local 108, second batch). Three defects:
//   * ONE FILE ONLY: `tee a b` opened a and silently ignored b, exit 0.
//   * `open(argv[1], 1)` is O_WRONLY with no O_CREAT and no O_TRUNC, so tee
//     could not create its output file, and writing to an existing longer file
//     left the tail of the old contents behind.
//   * every write() result was discarded, so a full disk lost data silently
//     and `producer | tee f | head -1` could never terminate (there is no
//     SIGPIPE on this OS - see mtool.h).
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "getopt.h"
#include "mtool.h"

#define MAX_OUT 32

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	int append = 0;
	int c;
	while ((c = getopt(argc, argv, "a")) != -1) {
		switch (c) {
		case 'a': append = 1; break;
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			if (optopt == 'i')
				mtool_refuse("tee -i (ignore interrupts)",
				             "signal masking is not wired up for this tool");
			mtool_bad_option(b);
		}
		}
	}

	int nout = argc - optind;
	if (nout > MAX_OUT)
		mtool_die(MTOOL_EX_FAIL, "too many output files (max %d)", MAX_OUT);

	int fds[MAX_OUT];
	const char *names[MAX_OUT];
	int n = 0, status = MTOOL_EX_OK;
	for (int i = optind; i < argc; i++) {
		char full[1024];
		if (mtool_resolve(argv[i], full, sizeof full) != 0) {
			mtool_warn("%s: path too long", argv[i]);
			status = MTOOL_EX_FAIL;
			continue;
		}
		int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
		int fd = open(full, flags, 0644);
		if (fd < 0) {
			// A file we cannot open is reported and the rest still get the
			// data, exactly as tee(1) behaves.
			mtool_warn("%s: cannot open for writing (error %d)", argv[i], fd);
			status = MTOOL_EX_FAIL;
			continue;
		}
		fds[n] = fd;
		names[n] = argv[i];
		n++;
	}

	char buf[8192];
	long r;
	while ((r = read(0, buf, sizeof buf)) > 0) {
		if (mtool_wall(1, buf, (size_t)r) != 0) {
			mtool_warn("standard output: write error");
			status = MTOOL_EX_FAIL;
			break;
		}
		for (int i = 0; i < n; i++) {
			if (fds[i] < 0) continue;
			if (mtool_wall(fds[i], buf, (size_t)r) != 0) {
				mtool_warn("%s: write error", names[i]);
				close(fds[i]);
				fds[i] = -1;
				status = MTOOL_EX_FAIL;
			}
		}
	}
	if (r < 0) { mtool_warn("standard input: read error"); status = MTOOL_EX_FAIL; }

	for (int i = 0; i < n; i++)
		if (fds[i] >= 0) close(fds[i]);
	return status;
}
