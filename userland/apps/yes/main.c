// yes - write a string over and over until the write fails
//
// #745 (local 108, THIRD batch). The whole program was
//
//     for (;;) { write(1, str, len); write(1, "\n", 1); }
//
// with BOTH results discarded, and it only ever used argv[1].
//
// WHY DISCARDING THE RESULT IS A HANG AND NOT A STYLE PROBLEM. There is no
// SIGPIPE on MayteraOS: kernel/fs/pipe.c's pipe_write_fn() returns -1 when the
// last reader is gone and nothing raises a signal (ticket 111, unfixed). The
// POSIX contract that makes `producer | head -1` a safe thing to type therefore
// does not exist here, and whether a pipeline TERMINATES is decided entirely by
// the producer's error handling. `cat` checks write() and stops, which is why
// `cat big | head -1` ends. `yes` did not, so `yes | head -1` spun for ever at
// 100% CPU on every image this OS has ever shipped. It was two lines from
// correct.
//
// THE FIX IS TO CHECK THE WRITE, NOT TO WAIT FOR A SIGNAL THAT NEVER ARRIVES.
// mtool_wall() is that check, and it is the same one head, tail, cat and tee
// use, so there is one definition of "did my consumer go away".
//
// WHAT A FAILED WRITE MEANS, AND HOW THIS TELLS THE TWO CASES APART. A failed
// write is ambiguous on this OS: it is either "the consumer has gone" (the
// normal, expected end of yes) or "the device is full" (a real error worth
// reporting). errno cannot separate them - the libc syscall wrapper turns the
// kernel's -1 into errno 1 - so this asks the FILE DESCRIPTOR what it is
// instead: lseek() on a pipe fails, because kernel/fs/pipe.c's pipe_write_ops
// has `.seek = NULL`, and succeeds on a regular file. That is the same "ask the
// object, do not infer from the data" probe the pipeline verification uses
// (blame.md, local 108). So `yes | head -1` ends silently with status 0, and
// `yes > /full/disk` reports a write error and exits 1.
//
// OPTIONS: GNU yes has NONE beyond --help/--version, which this build does not
// carry. Every argument is an OPERAND, and they are joined with single spaces,
// so `yes -x` prints "-x" and `yes a b` prints "a b". Refusing "-x" here would
// itself be a wrong answer, which is why there is no getopt in this file. The
// one exception is a leading `--`, which GNU's option parser consumes as the
// end-of-options marker before ever looking at the operands, so `yes --` prints
// "y" and not "--". That was measured against GNU yes 9.1, not assumed: the
// first version of this file printed "--" and the oracle said so.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "mtool.h"

// One write per line is two syscalls per line for no reason. Fill a buffer with
// whole copies of the line and write THAT; the observable output is identical
// and a `yes | wc -l` costs a few thousand times fewer syscalls. The buffer is
// never larger than the pipe ring, so a partial write is still a partial write
// and mtool_wall's loop still handles it.
#define YES_BUF 8192

static char line[4096];
static char blk[YES_BUF];

// Build the repeated line from the operands. Never truncates: a line that does
// not fit is a refusal, not a shorter answer (blame.md, local 108: "a silent cap
// is not repaired by a larger silent cap").
static size_t build_line(int argc, char **argv)
{
	size_t len = 0;
	int first = 1;
	if (argc > 1 && strcmp(argv[1], "--") == 0) first = 2;   // end of options
	if (argc <= first) { line[0] = 'y'; line[1] = '\n'; return 2; }
	for (int i = first; i < argc; i++) {
		size_t n = strlen(argv[i]);
		size_t need = (i > first ? 1u : 0u) + n + 1;
		if (len + need >= sizeof line)
			mtool_die(MTOOL_EX_FAIL,
			          "the joined operands do not fit in %lu bytes",
			          (unsigned long)sizeof line);
		if (i > first) line[len++] = ' ';
		memcpy(line + len, argv[i], n);
		len += n;
	}
	line[len++] = '\n';
	return len;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	size_t len = build_line(argc, argv);

	// Whole copies only: a partial trailing copy would be correct output but it
	// makes the buffer's contents depend on its size, which is the kind of thing
	// that is right until someone changes the constant.
	size_t blen = 0;
	if (len <= sizeof blk) {
		while (blen + len <= sizeof blk) { memcpy(blk + blen, line, len); blen += len; }
	} else {
		// A line longer than the block: write the line itself.
		memcpy(blk, line, len < sizeof blk ? len : sizeof blk);
		blen = len;
	}

	const char *buf = (len <= sizeof blk) ? blk : line;
	size_t sz = (len <= sizeof blk) ? blen : len;

	for (;;) {
		if (mtool_wall(1, buf, sz) != 0) {
			// See the header: a pipe cannot be seeked on this kernel, a file can.
			if (lseek(1, 0, 1 /* SEEK_CUR */) >= 0) {
				mtool_warn("write error on standard output");
				return MTOOL_EX_FAIL;
			}
			// The consumer is gone. That is how yes is supposed to end, and it
			// is the case that never terminated before this change.
			return MTOOL_EX_OK;
		}
	}
}
