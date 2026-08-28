// spawnrace - a FAST reproducer for local ticket 116: "about one captured
// command output per boot comes back empty, a different one every time".
//
// WHAT IT REPRODUCES, AND WHY IT IS NOT THE TOOL CORPUS. toolchk's
// run_capture() is the pattern under suspicion:
//
//     unlink(out);
//     posix_spawn(&pid, prog, {addopen(1, out, O_WRONLY|O_CREAT|O_TRUNC)}, argv);
//     waitpid(pid, &st, 0);
//     read_file(out, ...);
//
// The corpus wraps that around ~300 different programs and scores one loss per
// BOOT, so any investigation paced by the corpus is paced by boots. This app
// strips the corpus away and hammers ONLY the capture pattern, against a child
// whose output is byte-exact and known in advance, thousands of times in one
// boot. Nothing here tests a tool; a tool that is wrong would show up as the
// SAME wrong answer every iteration, which is the one shape this classifier
// reports separately.
//
// THE CLASSIFIER IS THE POINT. A count of failures cannot tell three different
// races apart, so every loss is classified by WHERE the bytes went missing:
//
//   PREFIX  the captured bytes are a proper SUFFIX of what the child wrote:
//           the first N writes are gone and everything after them is intact.
//           That is a write that went somewhere else BEFORE the capture file
//           was in place - it cannot be a flush, a truncation or a short read,
//           because those cannot delete the FRONT of a file and keep the rest.
//   EMPTY   nothing at all landed (the degenerate PREFIX case: all of it).
//   TAIL    the captured bytes are a proper PREFIX of the expectation: the
//           file lost its END. That is what a lost flush / a reader racing the
//           writer's close looks like.
//   HOLE    bytes missing from the middle, or bytes that are not ours at all.
//
// `wc a b` losing its FIRST line while its total stayed arithmetically correct
// is a PREFIX loss, so this classifier is what turns that one observation into
// a measurement.
//
// MODES
//   (no args)        run the capture loop, default 2000 iterations
//   <n>              run n iterations
//   --emit           child mode: write EMIT_LINES fixed-width lines, one
//                    write(2) call each, then exit 0
//
// The child is THIS SAME BINARY so the corpus's tools, their argument parsing
// and their buffering are all out of the picture: if a line is missing, this
// program did call write() for it.

#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "fcntl.h"
#include "spawn.h"
#include "sys/wait.h"
#include "syscall.h"   // uptime_ms()

#define SELF_PATH  "/APPS/SPAWNRAC"
#define OUT_PATH   "/SPAWNRAC.OUT"

// Eight writes of 24 bytes. Eight so a prefix loss has room to be PARTIAL
// (losing 1 of 8 is the `wc` shape; losing 8 of 8 is the `uname -r` shape),
// fixed width so "how many were lost" is a division and not a parse.
#define EMIT_LINES  8
#define LINE_BYTES  24
#define TOTAL_BYTES (EMIT_LINES * LINE_BYTES)

static void line(const char *s) { write(2, s, strlen(s)); }

// Build line i, exactly LINE_BYTES bytes including the newline.
static void mkline(int i, char *out)
{
	// "SPAWNRACE-LINE-NN------\n" == 15 + 2 + 6 + 1 == 24
	snprintf(out, LINE_BYTES + 1, "SPAWNRACE-LINE-%02d------\n", i);
}

static int mode_emit(void)
{
	char b[LINE_BYTES + 1];
	for (int i = 0; i < EMIT_LINES; i++) {
		mkline(i, b);
		// One write() per line, deliberately. A single big write could only
		// ever be all-or-nothing, which would hide exactly the partial-prefix
		// loss this app exists to measure.
		long w = write(1, b, LINE_BYTES);
		if (w != LINE_BYTES) return 3;
	}
	return 0;
}

static void build_expect(char *e)
{
	char b[LINE_BYTES + 1];
	for (int i = 0; i < EMIT_LINES; i++) { mkline(i, b); memcpy(e + i * LINE_BYTES, b, LINE_BYTES); }
	e[TOTAL_BYTES] = '\0';
}

static int read_file(const char *path, char *out, int outsz)
{
	int fd = open(path, O_RDONLY, 0);
	if (fd < 0) return -1;
	int total = 0;
	long n;
	while (total < outsz - 1 && (n = read(fd, out + total, (size_t)(outsz - 1 - total))) > 0)
		total += (int)n;
	close(fd);
	out[total] = '\0';
	return total;
}

// Classify a capture against the expectation. Returns a one-word verdict and
// fills `lost` with how many BYTES went missing off the front (PREFIX) or the
// back (TAIL).
static const char *classify(const char *got, int n, const char *exp, int *lost)
{
	*lost = 0;
	if (n == TOTAL_BYTES && memcmp(got, exp, TOTAL_BYTES) == 0) return "OK";
	if (n <= 0) { *lost = TOTAL_BYTES; return "EMPTY"; }
	// PREFIX loss: what landed is the TAIL of the expectation.
	if (n < TOTAL_BYTES && memcmp(got, exp + (TOTAL_BYTES - n), (size_t)n) == 0) {
		*lost = TOTAL_BYTES - n;
		return "PREFIX";
	}
	// TAIL loss: what landed is the HEAD of the expectation.
	if (n < TOTAL_BYTES && memcmp(got, exp, (size_t)n) == 0) {
		*lost = TOTAL_BYTES - n;
		return "TAIL";
	}
	*lost = TOTAL_BYTES - n;
	return "HOLE";
}

static void escape(const char *s, int n, char *out, int outsz)
{
	int o = 0;
	for (int i = 0; i < n && o < outsz - 5; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\n')      { out[o++] = '\\'; out[o++] = 'n'; }
		else if (c >= 32 && c < 127) out[o++] = (char)c;
		else { o += snprintf(out + o, (size_t)(outsz - o), "\\%03o", c); }
	}
	out[o] = '\0';
}

int main(int argc, char **argv)
{
	char b[512];

	if (argc >= 2 && !strcmp(argv[1], "--emit")) return mode_emit();

	int iters = 2000;
	if (argc >= 2) { int v = atoi(argv[1]); if (v > 0) iters = v; }

	static char expect[TOTAL_BYTES + 1];
	static char got[TOTAL_BYTES + 512];
	build_expect(expect);

	snprintf(b, sizeof b, "[SPAWNRACE] start: %d iterations of "
	                      "spawn -> %d writes -> exit -> read, expecting %d bytes\n",
	         iters, EMIT_LINES, TOTAL_BYTES);
	line(b);

	int n_ok = 0, n_empty = 0, n_prefix = 0, n_tail = 0, n_hole = 0, n_spawnfail = 0;
	int lost_bytes_total = 0;
	unsigned long t0 = uptime_ms();

	char *av[3];
	av[0] = "spawnrace"; av[1] = "--emit"; av[2] = 0;

	for (int it = 0; it < iters; it++) {
		posix_spawn_file_actions_t fa;
		pid_t pid = 0;
		int st = 0, rc;

		unlink(OUT_PATH);
		posix_spawn_file_actions_init(&fa);
		posix_spawn_file_actions_addopen(&fa, 1, OUT_PATH,
		                                 O_WRONLY | O_CREAT | O_TRUNC, 0644);
		rc = posix_spawn(&pid, SELF_PATH, &fa, NULL, av, NULL);
		posix_spawn_file_actions_destroy(&fa);
		if (rc != 0) {
			n_spawnfail++;
			snprintf(b, sizeof b, "[SPAWNRACE] iter %d: spawn failed rc=%d\n", it, rc);
			line(b);
			continue;
		}
		waitpid(pid, &st, 0);

		int n = read_file(OUT_PATH, got, sizeof got);
		int lost = 0;
		const char *v = classify(got, n, expect, &lost);

		if      (!strcmp(v, "OK"))     n_ok++;
		else if (!strcmp(v, "EMPTY"))  n_empty++;
		else if (!strcmp(v, "PREFIX")) n_prefix++;
		else if (!strcmp(v, "TAIL"))   n_tail++;
		else                            n_hole++;

		if (strcmp(v, "OK") != 0) {
			char eg[400];
			lost_bytes_total += lost;
			escape(got, n > 0 ? (n < 120 ? n : 120) : 0, eg, sizeof eg);
			// ONE write() per record: an autorun-launched process emits one
			// serial record per write(), so a split line would interleave.
			snprintf(b, sizeof b,
			         "[SPAWNRACE] LOSS iter=%d verdict=%s pid=%d exit=%d got=%d/%d "
			         "lost=%d bytes (%d lines off the %s) t=%lums data=\"%s\"\n",
			         it, v, (int)pid, st, n, TOTAL_BYTES, lost, lost / LINE_BYTES,
			         (!strcmp(v, "TAIL")) ? "end" : "front",
			         uptime_ms() - t0, eg);
			line(b);
		}

		if (((it + 1) % 250) == 0) {
			snprintf(b, sizeof b,
			         "[SPAWNRACE] progress %d/%d ok=%d empty=%d prefix=%d tail=%d hole=%d "
			         "spawnfail=%d elapsed=%lums\n",
			         it + 1, iters, n_ok, n_empty, n_prefix, n_tail, n_hole,
			         n_spawnfail, uptime_ms() - t0);
			line(b);
		}
	}

	unsigned long ms = uptime_ms() - t0;
	int bad = n_empty + n_prefix + n_tail + n_hole;
	snprintf(b, sizeof b,
	         "[SPAWNRACE] DONE iters=%d ok=%d LOSSES=%d (empty=%d prefix=%d tail=%d hole=%d) "
	         "spawnfail=%d lostbytes=%d elapsed=%lums rate=%d losses/1000-captures\n",
	         iters, n_ok, bad, n_empty, n_prefix, n_tail, n_hole, n_spawnfail,
	         lost_bytes_total, ms, iters ? (bad * 1000) / iters : 0);
	line(b);
	line("[SPAWNRACE] END\n");
	return bad ? 1 : 0;
}
