// toolchk - proof, on a booted MayteraOS, that the tools #745 local 108 fixed
// do what their names promise. Phases 1-3 are the first batch (/APPS/TR,
// /APPS/ENV, msh pipelines); phase 4 is the second (rm, mkdir, rmdir, touch,
// wc, head, tail, tee, cp, cut, sort, tac, date, less).
//
// WHY THIS APP EXISTS. Three shipped tools were silently wrong on the most
// typed form of themselves: `tr a-z A-Z` was a no-op, `env FOO=bar prog` never
// ran prog, and `a | b` ran the stages sequentially and unconnected. A hosted
// build catches most tr bugs in a second (tools/tr-oracle/tr-oracle.sh), but a
// hosted build is not the shipped binary and cannot run a pipeline at all. So
// this app SPAWNS THE FILES THAT ARE ON THE IMAGE and reads back what they
// produced, printing each one's sha256 first so a run is tied to the exact
// bytes rather than to "the build I think shipped".
//
// WHERE THE EXPECTATIONS COME FROM. Every tr expectation below was pasted from
// `tools/tr-oracle/tr-oracle.sh --table`, which is the HOST's own GNU tr 9.1
// answering the same question on the same input. Nothing was reasoned from
// POSIX: a hand-written expectation for a standard tool is a guess with a
// straight face, and this tree has paid for that three times.
//
// THE O_TRUNC TRAP, WHICH THIS HARNESS IS BUILT AROUND. On this kernel a file
// opened O_TRUNC that the child never writes to KEEPS ITS OLD CONTENTS
// (blame.md, local 103; ticket 109, unfixed). A refusal writes nothing, so
// without care every refusal case reads back the PREVIOUS case's output and
// four correct refusals score as four failures. Every capture here therefore
// unlink()s the output path first, so O_CREAT makes a genuinely empty file and
// "no output" means no output.
//
// OUTPUT DISCIPLINE. Launched from /CONFIG/AUTORUN.CFG. An autorun-launched
// process emits ONE SERIAL RECORD PER write(), so every line is formatted into
// a buffer and issued as exactly one write(2, ...).
//
// THIS APP IS ALSO ITS OWN PIPELINE FIXTURE. `--pipe-produce`,
// `--pipe-produce-full`, `--pipe-consume` and `--pipe-count` are stages msh
// runs. They are what makes the difference between a REAL pipe and a temp file
// observable: a consumer that exits early must strand its producer, and a
// producer of 200 KB must get through a 64 KB ring, neither of which a
// sequential or file-backed implementation can do.

#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "fcntl.h"
#include "spawn.h"
#include "sha256.h"
#include "sys/wait.h"
#include "signal.h"     // kill(): the watchdog that bounds `yes | head -1`
#include "pthread.h"    // waitpid() here has no WNOHANG, so the bound is a thread
#include "syscall.h"    // uptime_ms(), get_version(), sys_sleep()
#include "sys/stat.h"   // mkdir() for the phase 5 fixture directory

#define IN_PATH    "/T108IN.TXT"
#define OUT_PATH   "/T108OUT.TXT"
#define STOP_PATH  "/T108STOP.TXT"
#define DONE_PATH  "/T108DONE.TXT"

static int g_pass = 0, g_fail = 0;

static void line(const char *s) { write(2, s, strlen(s)); }

static void ok(int cond, const char *what)
{
	char b[320];
	if (cond) g_pass++; else g_fail++;
	snprintf(b, sizeof b, "[TOOLCHK] %s %s\n", cond ? "PASS" : "FAIL", what);
	line(b);
}

// Render a byte string the way the oracle does, so a mismatch can be compared
// against the table by eye without a second encoding to reason about.
static void escape(const char *s, int n, char *out, int outsz)
{
	int o = 0;
	for (int i = 0; i < n && o < outsz - 5; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\n')      { out[o++] = '\\'; out[o++] = 'n'; }
		else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
		else if (c == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
		else if (c >= 32 && c < 127) out[o++] = (char)c;
		else { o += snprintf(out + o, (size_t)(outsz - o), "\\%03o", c); }
	}
	out[o] = '\0';
}

static void print_sha256(const char *path)
{
	int fd = open(path, O_RDONLY, 0);
	char b[256];
	if (fd < 0) { snprintf(b, sizeof b, "[TOOLCHK] SHA256 %s: CANNOT OPEN\n", path); line(b); return; }
	sha256_ctx_t c;
	sha256_init(&c);
	static char buf[4096];
	int n, total = 0;
	while ((n = (int)read(fd, buf, sizeof buf)) > 0) { sha256_update(&c, buf, (size_t)n); total += n; }
	close(fd);
	uint8_t d[32];
	sha256_final(&c, d);
	char hex[65];
	for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
	snprintf(b, sizeof b, "[TOOLCHK] SHA256 %s = %s (%d bytes)\n", path, hex, total);
	line(b);
}

static int write_file(const char *path, const char *text, int len)
{
	unlink(path);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return -1;
	int off = 0;
	while (off < len) {
		long w = write(fd, text + off, (size_t)(len - off));
		if (w < 0) { close(fd); return -1; }
		off += (int)w;
	}
	close(fd);
	return 0;
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

static int file_exists(const char *path)
{
	int fd = open(path, O_RDONLY, 0);
	if (fd < 0) return 0;
	close(fd);
	return 1;
}

// Spawn PATH with ARGV; stdin from INFILE (may be NULL) and stdout to OUTFILE.
// OUTFILE is UNLINKED first - see the O_TRUNC note at the top of this file.
static int run_capture(const char *path, char *const argv[],
                       const char *infile, const char *outfile)
{
	posix_spawn_file_actions_t fa;
	pid_t pid = 0;
	int st = 0, rc;

	unlink(outfile);
	posix_spawn_file_actions_init(&fa);
	if (infile) posix_spawn_file_actions_addopen(&fa, 0, infile, O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&fa, 1, outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	rc = posix_spawn(&pid, path, &fa, NULL, argv, NULL);
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0) return -rc;
	waitpid(pid, &st, 0);
	return st;
}

// ===========================================================================
// PHASE 1: /APPS/TR
// ===========================================================================

struct trcase { const char *name; char *argv[5]; const char *in; const char *want; };

// Pasted verbatim from tools/tr-oracle/tr-oracle.sh --table (GNU coreutils 9.1).
static struct trcase TRCASES[] = {
  { "range-upper",        { "tr", "a-z", "A-Z", 0, 0 },          "Hello, World 123!\n", "HELLO, WORLD 123!\n" },
  { "range-lower",        { "tr", "A-Z", "a-z", 0, 0 },          "Hello, World 123!\n", "hello, world 123!\n" },
  { "rot13",              { "tr", "A-Za-z", "N-ZA-Mn-za-m", 0, 0 }, "Attack at dawn\n",  "Nggnpx ng qnja\n" },
  { "class-upper",        { "tr", "[:lower:]", "[:upper:]", 0, 0 }, "mixed Case 42\n",   "MIXED CASE 42\n" },
  { "literal-set",        { "tr", "abc", "xyz", 0, 0 },          "a cabbage b\n",       "x zxyyxge y\n" },
  { "pad-set2",           { "tr", "abc", "x", 0, 0 },            "a cabbage b\n",       "x xxxxxge x\n" },
  { "truncate-set1",      { "tr", "-t", "abc", "xy", 0 },        "a cabbage b\n",       "x cxyyxge y\n" },
  { "delete-digits",      { "tr", "-d", "[:digit:]", 0, 0 },     "abc123def456\n",      "abcdef\n" },
  { "squeeze-space",      { "tr", "-s", " ", 0, 0 },             "a    b   c\n",        "a b c\n" },
  { "translate-squeeze",  { "tr", "-s", "a-z", "A-Z", 0 },       "aaabbb ccc\n",        "AB C\n" },
  { "complement-delete",  { "tr", "-cd", "[:alnum:]\n", 0, 0 },  "ab!c@1 2#3\n",        "abc123\n" },
  { "delete-squeeze",     { "tr", "-ds", "aeiou", " ", 0 },      "the  quick   brown fox\n", "th qck brwn fx\n" },
  { "newline-to-space",   { "tr", "\n", " ", 0, 0 },             "one\ntwo\nthree\n",   "one two three " },
  { "octal-escape",       { "tr", "\\101", "B", 0, 0 },          "AAA BBB\n",           "BBB BBB\n" },
  { "repeat-n",           { "tr", "abc", "[x*3]", 0, 0 },        "a b c\n",             "x x x\n" },
  { "repeat-fill",        { "tr", "a-e", "x[y*]", 0, 0 },        "abcde\n",             "xyyyy\n" },
  { "equiv-class",        { "tr", "[=a=]", "X", 0, 0 },          "banana\n",            "bXnXnX\n" },
  { "squeeze-space-cls",  { "tr", "-s", "[:space:]", 0, 0 },     "a  \t b\n\n\nc\n",    "a \t b\nc\n" },
  { "delete-complement",  { "tr", "-dc", "a-z", 0, 0 },          "abc DEF 123\n",       "abc" },
  { "dash-literal",       { "tr", "a-", "X", 0, 0 },             "a-b-c\n",             "XXbXc\n" },
};

// Refusals: a non-zero exit AND nothing at all on stdout. GNU tr rejects each
// of these too; the message TEXT is deliberately not compared, because
// asserting that our wording equals GNU's is a test that can only be wrong.
static struct trcase TRREFUSE[] = {
  { "no-operand",        { "tr", 0, 0, 0, 0 },              "x\n", "" },
  { "missing-set2",      { "tr", "abc", 0, 0, 0 },          "x\n", "" },
  { "extra-operand",     { "tr", "a", "b", "c", 0 },        "x\n", "" },
  { "delete-two-sets",   { "tr", "-d", "a", "b", 0 },       "x\n", "" },
  { "reverse-range",     { "tr", "z-a", "x", 0, 0 },        "x\n", "" },
  { "bad-class",         { "tr", "[:nosuch:]", "x", 0, 0 }, "x\n", "" },
  { "bad-option",        { "tr", "-z", "a", "b", 0 },       "x\n", "" },
  { "misaligned-class",  { "tr", "a[:digit:]", "[:upper:]", 0, 0 }, "x\n", "" },
};

static void phase1(void)
{
	char got[1024], eg[2048], ew[2048], b[3072];

	line("[TOOLCHK] ==== phase 1: the shipped /APPS/TR ====\n");
	print_sha256("/APPS/TR");

	for (unsigned i = 0; i < sizeof TRCASES / sizeof TRCASES[0]; i++) {
		struct trcase *t = &TRCASES[i];
		if (write_file(IN_PATH, t->in, (int)strlen(t->in)) != 0) {
			snprintf(b, sizeof b, "[TOOLCHK] FAIL tr %s: could not write the input\n", t->name);
			line(b); g_fail++; continue;
		}
		int st = run_capture("/APPS/TR", t->argv, IN_PATH, OUT_PATH);
		int n = read_file(OUT_PATH, got, sizeof got);
		if (n < 0) n = 0;
		int good = (st == 0) && (n == (int)strlen(t->want)) && memcmp(got, t->want, (size_t)n) == 0;
		if (good) g_pass++; else g_fail++;
		escape(got, n, eg, sizeof eg);
		snprintf(b, sizeof b, "[TOOLCHK] %s tr %-20s exit=%d -> \"%s\"\n",
		         good ? "PASS" : "FAIL", t->name, st, eg);
		line(b);
		if (!good) {
			escape(t->want, (int)strlen(t->want), ew, sizeof ew);
			snprintf(b, sizeof b, "[TOOLCHK]      GNU tr said \"%s\"\n", ew);
			line(b);
		}
	}

	for (unsigned i = 0; i < sizeof TRREFUSE / sizeof TRREFUSE[0]; i++) {
		struct trcase *t = &TRREFUSE[i];
		write_file(IN_PATH, t->in, (int)strlen(t->in));
		int st = run_capture("/APPS/TR", t->argv, IN_PATH, OUT_PATH);
		int n = read_file(OUT_PATH, got, sizeof got);
		if (n < 0) n = 0;
		int good = (st != 0) && (n == 0);
		if (good) g_pass++; else g_fail++;
		escape(got, n, eg, sizeof eg);
		snprintf(b, sizeof b, "[TOOLCHK] %s tr refuses %-16s exit=%d stdout=\"%s\"\n",
		         good ? "PASS" : "FAIL", t->name, st, eg);
		line(b);
	}
}

// ===========================================================================
// PHASE 2: /APPS/ENV
// ===========================================================================

static void env_case(const char *what, char *const argv[], int want_zero_exit,
                     const char *want_out)
{
	char got[512], eg[1024], b[2048];
	int st = run_capture("/APPS/ENV", argv, IN_PATH, OUT_PATH);
	int n = read_file(OUT_PATH, got, sizeof got);
	if (n < 0) n = 0;
	int good = want_zero_exit ? (st == 0) : (st != 0);
	if (good && want_out) good = (n == (int)strlen(want_out)) && memcmp(got, want_out, (size_t)n) == 0;
	if (good) g_pass++; else g_fail++;
	escape(got, n, eg, sizeof eg);
	snprintf(b, sizeof b, "[TOOLCHK] %s env %-34s exit=%d stdout=\"%s\"\n",
	         good ? "PASS" : "FAIL", what, st, eg);
	line(b);
}

static void phase2(void)
{
	line("[TOOLCHK] ==== phase 2: the shipped /APPS/ENV ====\n");
	print_sha256("/APPS/ENV");
	write_file(IN_PATH, "", 0);

	// The half that was simply missing: env must RUN the command. If it does
	// not, stdout is empty and this fails.
	{ char *av[] = { "env", "/APPS/ECHO", "hello", "world", 0 };
	  env_case("runs the command", av, 1, "hello world\n"); }

	// #112: THESE FOUR CASES USED TO ASSERT THE OPPOSITE, and they were right
	// to at the time: there was no cross-process environment, so -i and -u were
	// no-ops satisfied by construction, NAME=VALUE was a hard refusal, and a
	// bare `env` printed nothing because the environment really was empty.
	//
	// The OS changed under them (SYS_SPAWN_ENV carries an environment now), so
	// a gate that still asserted the old behaviour would be a gate asserting a
	// lie: it would go RED on a correct system and, worse, GREEN on a
	// regression back to the old one.
	{ char *av[] = { "env", "-i", "/APPS/ECHO", "ok", 0 };
	  env_case("-i then runs the command", av, 1, "ok\n"); }

	{ char *av[] = { "env", "-u", "PATH", "/APPS/ECHO", "ok", 0 };
	  env_case("-u NAME then runs", av, 1, "ok\n"); }

	// NAME=VALUE now WORKS. The command has to run...
	{ char *av[] = { "env", "FOO=bar", "/APPS/ECHO", "hi", 0 };
	  env_case("FOO=bar runs the command", av, 1, "hi\n"); }

	// ...and the variable has to actually ARRIVE in the child. Running echo
	// only proves env did not refuse; this proves the value crossed the spawn,
	// which is the whole point of the feature.
	{ char *av[] = { "env", "FOO=bar", "/APPS/ENVPROBE", "show", "FOO", 0 };
	  env_case("FOO=bar reaches the child", av, 1, "[T112] show: FOO=bar\n"); }

	// -i really empties: the child must NOT see PATH, which its parent has.
	// envprobe's `show` exits non-zero when the variable is unset.
	{ char *av[] = { "env", "-i", "/APPS/ENVPROBE", "show", "PATH", 0 };
	  env_case("-i really empties the child env", av, 0, "[T112] show: PATH=(unset)\n"); }

	// No command, empty environment: prints nothing. Asserted with -i because
	// the INHERITED environment is not a fixed string, so an exact-output test
	// on a bare `env` would be a test of whatever the parent happened to set.
	{ char *av[] = { "env", "-i", 0 };
	  env_case("no command with -i prints nothing", av, 1, ""); }

	// A command that does not exist is 127, not 0 with no output.
	{ char *av[] = { "env", "/APPS/NOSUCHTHING", 0 };
	  env_case("missing command exits non-zero", av, 0, ""); }
}

// ===========================================================================
// PHASE 3: msh pipelines
// ===========================================================================

static int msh_run(const char *cmdline, const char *infile, char *out, int outsz)
{
	char *av[4];
	av[0] = "msh"; av[1] = "-c"; av[2] = (char *)cmdline; av[3] = 0;
	int st = run_capture("/APPS/MSH", av, infile, OUT_PATH);
	int n = read_file(OUT_PATH, out, outsz);
	if (n < 0) { out[0] = '\0'; n = 0; }
	(void)n;
	return st;
}

static void msh_case(const char *cmdline, const char *want)
{
	char got[2048], eg[3072], ew[512], b[4096];
	int st = msh_run(cmdline, IN_PATH, got, sizeof got);
	int good = (strlen(got) == strlen(want)) && memcmp(got, want, strlen(want)) == 0;
	if (good) g_pass++; else g_fail++;
	escape(got, (int)strlen(got), eg, sizeof eg);
	snprintf(b, sizeof b, "[TOOLCHK] %s msh -c '%s' exit=%d -> \"%s\"\n",
	         good ? "PASS" : "FAIL", cmdline, st, eg);
	line(b);
	if (!good) {
		escape(want, (int)strlen(want), ew, sizeof ew);
		snprintf(b, sizeof b, "[TOOLCHK]      wanted \"%s\"\n", ew);
		line(b);
	}
}

static void msh_status_case(const char *what, const char *cmdline, int want_status)
{
	char got[512], b[1024];
	int st = msh_run(cmdline, IN_PATH, got, sizeof got);
	int good = (st == want_status);
	if (good) g_pass++; else g_fail++;
	snprintf(b, sizeof b, "[TOOLCHK] %s %s: msh -c '%s' exit=%d (wanted %d)\n",
	         good ? "PASS" : "FAIL", what, cmdline, st, want_status);
	line(b);
}

static void phase3(void)
{
	char got[2048], b[3072];

	line("[TOOLCHK] ==== phase 3: msh pipelines (the shipped /APPS/MSH) ====\n");
	print_sha256("/APPS/MSH");
	print_sha256("/APPS/TOOLCHK");

	write_file(IN_PATH, "the quick brown fox\n", 20);

	// Two stages. Under the old msh this printed "hello" (stage 1's output
	// straight to the terminal) and then stage 2 sat reading the terminal.
	msh_case("echo hello | tr a-z A-Z", "HELLO\n");

	// Three stages, with stage 2 consuming stage 1 and stage 3 consuming
	// stage 2. `cat` with no operand reads its stdin, which is the pipe.
	msh_case("cat /T108IN.TXT | tr a-z A-Z | tr -d AEIOU",
	         "TH QCK BRWN FX\n");

	// A per-stage redirection inside a pipeline goes through sys_spawn_redir.
	msh_case("tr a-z A-Z < /T108IN.TXT | tr -d ' '",
	         "THEQUICKBROWNFOX\n");

	// A BUILTIN as a pipeline stage: it runs in the shell, with fd 1 pointed
	// at the pipe. `pwd` is a builtin here; `tr` is not. The `cd /` in front
	// makes the expectation independent of which user the harness runs as,
	// and it also exercises the ';'-before-'|' ordering that the old code got
	// backwards (it split on '|' first, so `a | b; c` could never work).
	msh_case("cd /; pwd | tr / :", ":\n");

	// POSIX: a pipeline's status is the LAST stage's.
	msh_status_case("status is the last stage", "true | false", 1);
	msh_status_case("status is the last stage", "false | true", 0);

	// Failures must be LOUD and must produce no output at all.
	msh_case("echo x | nosuchcommand9", "");
	msh_status_case("unknown command is 127", "echo x | nosuchcommand9", 127);
	msh_case("echo x | | tr a-z A-Z", "");
	msh_status_case("empty stage is a syntax error", "echo x | | tr a-z A-Z", 2);

	// ---- the two cases that separate a REAL pipe from a temp file ---------
	//
	// (a) A CONSUMER THAT EXITS EARLY must stop its producer short.
	//     --pipe-produce writes up to 1 MB, checking every write; --pipe-consume
	//     reads ONE line and exits. With a temp file the producer would always
	//     finish and write DONE. It also proves the consumer's stdin IS a pipe:
	//     lseek() on a pipe fails (pipe_read_ops.seek is NULL) and succeeds on a
	//     regular file.
	//
	//     #111 CHANGED HOW THE PRODUCER STOPS, AND THIS TEST CAUGHT IT. The
	//     assertion used to be `stopped && !done`: the producer was expected to
	//     SURVIVE its failed write and record a STOP file, because
	//     kernel/fs/pipe.c returned a bare -1 and raised nothing. As of build
	//     1994 the kernel raises SIGPIPE, whose default action TERMINATES the
	//     producer at syscall return, so it never reaches the code that writes
	//     STOP and the test failed on a run where everything worked correctly.
	//
	//     `!done` is the invariant that was always the real point and that
	//     survives the change: the producer did NOT run to completion, which is
	//     what separates a real pipe from a temp file. HOW it was stopped is
	//     reported rather than asserted, because both endings are legitimate:
	//     killed by SIGPIPE (the default, STOP absent) or a checked write on a
	//     process that ignores SIGPIPE (STOP present).
	unlink(STOP_PATH);
	unlink(DONE_PATH);
	{
		int st = msh_run("/APPS/TOOLCHK --pipe-produce | /APPS/TOOLCHK --pipe-consume",
		                 IN_PATH, got, sizeof got);
		int stopped = file_exists(STOP_PATH);
		int done    = file_exists(DONE_PATH);
		char stopbuf[64];
		int sn = read_file(STOP_PATH, stopbuf, sizeof stopbuf);
		if (sn < 0) { stopbuf[0] = '\0'; }
		snprintf(b, sizeof b, "[TOOLCHK]      early-exit probe: exit=%d consumer said \"%s\" "
		         "STOP=%d(%s) DONE=%d\n", st, got, stopped, stopbuf, done);
		line(b);
		ok(strncmp(got, "STDIN=PIPE", 10) == 0,
		   "the consumer's stdin is a PIPE (lseek refuses it), not a file");
		snprintf(b, sizeof b, "[TOOLCHK]      producer ended by: %s\n",
		         stopped ? "a CHECKED WRITE (recorded STOP; SIGPIPE was ignored or handled)"
		                 : "SIGPIPE (#111b default action; no STOP file recorded)");
		line(b);
		ok(!done,
		   "a consumer that exits early stops its producer short (real pipe, not a temp file)");
	}

	// (b) 200 KB through a 64 KB ring. A pipeline that only worked by buffering
	//     everything first could not do this, and an unconnected one delivers
	//     nothing at all. The producer records the exact byte count it wrote;
	//     the consumer reports the exact count it read; they must agree.
	unlink(STOP_PATH);
	unlink(DONE_PATH);
	{
		int st = msh_run("/APPS/TOOLCHK --pipe-produce-full | /APPS/TOOLCHK --pipe-count",
		                 IN_PATH, got, sizeof got);
		char donebuf[64];
		int dn = read_file(DONE_PATH, donebuf, sizeof donebuf);
		if (dn < 0) donebuf[0] = '\0';
		long produced = atol(donebuf);
		long consumed = 0;
		if (strncmp(got, "BYTES=", 6) == 0) consumed = atol(got + 6);
		snprintf(b, sizeof b, "[TOOLCHK]      streaming probe: exit=%d produced=%ld consumed=%ld "
		         "(pipe ring is 65536 bytes)\n", st, produced, consumed);
		line(b);
		ok(produced > 65536 && consumed == produced,
		   "every byte of a 200 KB stream crossed a 64 KB pipe, so stage 2 consumed "
		   "stage 1's output as it was produced");
	}
}

// ===========================================================================
// The pipeline fixture stages. These are what msh runs above.
// ===========================================================================

#define PRODUCE_LIMIT 200000L

static void write_count(const char *path, long n)
{
	char b[32];
	int len = snprintf(b, sizeof b, "%ld\n", n);
	write_file(path, b, len);
}

static int mode_produce(long limit, int expect_full)
{
	static char l[64];
	long total = 0;
	while (total < limit) {
		int n = snprintf(l, sizeof l,
		                 "%08ld ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz\n", total);
		int off = 0;
		while (off < n) {
			long w = write(1, l + off, (size_t)(n - off));
			if (w < 0) {              // no readers left: this is the EPIPE case
				write_count(STOP_PATH, total);
				return 1;
			}
			off += (int)w;            // w == 0 means the ring is full; retry
		}
		total += n;
	}
	write_count(DONE_PATH, total);
	return expect_full ? 0 : 0;
}

static int mode_consume(void)
{
	// lseek() on a pipe fails, because kernel/fs/pipe.c installs no seek op.
	// On a regular file it returns the offset. That is the whole difference
	// between a real pipe and a temp-file impersonation, asked directly.
	long pos = lseek(0, 0, SEEK_CUR);
	char buf[256];
	int got = 0;
	while (got < (int)sizeof buf - 1) {
		char ch;
		long n = read(0, &ch, 1);
		if (n <= 0) break;
		if (ch == '\n') break;
		buf[got++] = ch;
	}
	buf[got] = '\0';
	char out[320];
	int len = snprintf(out, sizeof out, "STDIN=%s first=%s\n",
	                   (pos < 0) ? "PIPE" : "SEEKABLE", buf);
	write(1, out, (size_t)len);
	return 0;                          // exit immediately, stranding the producer
}

static int mode_count(void)
{
	static char buf[4096];
	long total = 0, n;
	while ((n = read(0, buf, sizeof buf)) > 0) total += n;
	char out[64];
	int len = snprintf(out, sizeof out, "BYTES=%ld\n", total);
	write(1, out, (size_t)len);
	return 0;
}

// ===========================================================================
// PHASE 4: the SECOND BATCH of #745 local 108 - the shipped /APPS/RM,
//          /APPS/MKDIR, /APPS/RMDIR, /APPS/TOUCH, /APPS/WC, /APPS/HEAD,
//          /APPS/TAIL, /APPS/TEE, /APPS/CP, /APPS/CUT, /APPS/SORT, /APPS/TAC,
//          /APPS/DATE and /APPS/LESS.
//
// WHY THESE CASES AND NOT OTHERS. Each one is a shape that the tool it
// replaces got SILENTLY WRONG:
//   * every multi-operand case, because eight of these tools acted on argv[1]
//     and exited 0 (`rm a b c` removed a);
//   * tail and tac on a file over 64 KB, because both read a fixed buffer and
//     then answered confidently about the wrong part of the file;
//   * sort on a file over 1000 lines and with a line over 255 characters,
//     because it discarded the former and SPLIT the latter;
//   * cut -f1,3 and -f2-, because -f was an atoi() and answered "field 1";
//   * date with a format at all, because it ignored argv and printed uptime.
//
// WHERE THE EXPECTATIONS COME FROM: vmtable2.h, generated by
// tools/coreutils-oracle/coreutils-oracle.sh --vm-table from the HOST's own
// GNU coreutils. Nothing here is reasoned from POSIX.
//
// AND THE FIXTURE IS VERIFIED FIRST. The expectations are only meaningful if
// this machine's fixture is byte-identical to the one the host generated them
// against, so every fixture file's SHA-256 is checked before a single case
// runs. A drifted fixture reports ITSELF rather than reporting fourteen tool
// failures.
#include "vmtable2.h"
// WHERE PHASE 5's EXPECTATIONS COME FROM: vmtable3.h, generated by
// tools/coreutils-oracle/coreutils-oracle.sh --vm-table3 from THIS HOST's
// GNU coreutils. Same contract as vmtable2.h, including the fixture hashes.
#include "vmtable3.h"

#define F4A     "/T4A.TXT"
#define F4B     "/T4B.TXT"
#define F4T     "/T4T.TSV"
#define F4NUM   "/T4NUM.TXT"
#define F4BIG   "/T4BIG.TXT"
#define F4SORT  "/T4SORT.TXT"

static int sha256_file_hex(const char *path, char *hex)
{
	int fd = open(path, O_RDONLY, 0);
	if (fd < 0) return -1;
	sha256_ctx_t c;
	sha256_init(&c);
	static char b[4096];
	long n;
	while ((n = read(fd, b, sizeof b)) > 0) sha256_update(&c, b, (size_t)n);
	close(fd);
	uint8_t d[32];
	sha256_final(&c, d);
	for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", d[i]);
	hex[64] = '\0';
	return 0;
}

// A writer that appends, so the big fixtures do not need to exist in memory.
static int append_str(int fd, const char *s)
{
	size_t len = strlen(s);
	size_t off = 0;
	while (off < len) {
		long w = write(fd, s + off, len - off);
		if (w < 0) return -1;
		off += (size_t)w;
	}
	return 0;
}

static int build_fixture(void)
{
	char nb[32];
	if (write_file(F4A, "alpha\nbravo\ncharlie\ndelta\n", 26) != 0) return -1;
	if (write_file(F4B, "zulu\nyankee\n", 12) != 0) return -1;
	if (write_file(F4T, "x\ty\tz\n1\t2\t3\n", 12) != 0) return -1;
	if (write_file(F4NUM, "10\n9\n100\n", 9) != 0) return -1;

	// seq 1 20000, which is about 108 KB: comfortably past the 64 KB buffers
	// that tail and tac each had.
	unlink(F4BIG);
	int fd = open(F4BIG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return -1;
	for (int i = 1; i <= 20000; i++) {
		int n = snprintf(nb, sizeof nb, "%d\n", i);
		if (append_str(fd, nb) != 0) { close(fd); return -1; }
		(void)n;
	}
	close(fd);

	// 1200 lines (past sort's 1000-line table) with one 300-character line
	// (past its 256-byte per-line buffer, which SPLIT it).
	unlink(F4SORT);
	fd = open(F4SORT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return -1;
	for (int i = 1; i <= 599; i++) if (append_str(fd, "a\n") != 0) { close(fd); return -1; }
	{
		char big[302];
		for (int i = 0; i < 300; i++) big[i] = 'b';
		big[300] = '\n';
		big[301] = '\0';
		if (append_str(fd, big) != 0) { close(fd); return -1; }
	}
	for (int i = 601; i <= 1199; i++) if (append_str(fd, "a\n") != 0) { close(fd); return -1; }
	if (append_str(fd, "z\n") != 0) { close(fd); return -1; }
	close(fd);
	return 0;
}

static int verify_fixture(void)
{
	char path[64], hex[80], b[320];
	int bad = 0;
	for (unsigned i = 0; i < sizeof VMFIX / sizeof VMFIX[0]; i++) {
		snprintf(path, sizeof path, "/%s", VMFIX[i].name);
		if (sha256_file_hex(path, hex) != 0) {
			snprintf(b, sizeof b, "[TOOLCHK] FIXTURE %s: CANNOT READ\n", path);
			line(b);
			bad = 1;
			continue;
		}
		int ok_ = (strcmp(hex, VMFIX[i].sha256) == 0);
		snprintf(b, sizeof b, "[TOOLCHK] %s fixture %-12s %s\n",
		         ok_ ? "PASS" : "FAIL", VMFIX[i].name, hex);
		line(b);
		if (ok_) g_pass++; else { g_fail++; bad = 1; }
	}
	return bad;
}

// Collapse runs of whitespace, the same normalisation the oracle applies to wc
// and only to wc: GNU right-aligns its counts in a width it derives from the
// input size, and reproducing that column algorithm would prove nothing about
// whether wc counted the right things.
static int collapse_spaces(char *s, int n)
{
	int o = 0, sp = 0, bol = 1;
	for (int i = 0; i < n; i++) {
		char c = s[i];
		if (c == ' ' || c == '\t') { sp = 1; continue; }
		if (sp && !bol) s[o++] = ' ';
		sp = 0;
		bol = (c == '\n');
		s[o++] = c;
	}
	s[o] = '\0';
	return o;
}

static void phase4_cases(void)
{
	static char got[8192];
	char eg[4096], ew[4096], b[9000];

	for (unsigned i = 0; i < sizeof VMCASES / sizeof VMCASES[0]; i++) {
		const struct vmcase *t = &VMCASES[i];
		int st = run_capture(t->path, t->argv, NULL, OUT_PATH);
		int n = read_file(OUT_PATH, got, sizeof got);
		if (n < 0) n = 0;
		if (t->collapse) n = collapse_spaces(got, n);
		if (t->limit > 0 && n > t->limit) n = t->limit;
		got[n] = '\0';
		int want = (int)strlen(t->want);
		int good = (st == 0) && (n == want) && memcmp(got, t->want, (size_t)n) == 0;
		if (good) g_pass++; else g_fail++;
		escape(got, n, eg, sizeof eg);
		snprintf(b, sizeof b, "[TOOLCHK] %s %-22s exit=%d -> \"%s\"\n",
		         good ? "PASS" : "FAIL", t->name, st, eg);
		line(b);
		if (!good) {
			escape(t->want, want, ew, sizeof ew);
			snprintf(b, sizeof b, "[TOOLCHK]      GNU said \"%s\"\n", ew);
			line(b);
		}
	}

	for (unsigned i = 0; i < sizeof VMREFUSE / sizeof VMREFUSE[0]; i++) {
		const struct vmref *t = &VMREFUSE[i];
		int st = run_capture(t->path, t->argv, NULL, OUT_PATH);
		int n = read_file(OUT_PATH, got, sizeof got);
		if (n < 0) n = 0;
		int good = (st != 0) && (n == 0);
		if (good) g_pass++; else g_fail++;
		escape(got, n, eg, sizeof eg);
		snprintf(b, sizeof b, "[TOOLCHK] %s refuses %-18s exit=%d stdout=\"%s\"\n",
		         good ? "PASS" : "FAIL", t->name, st, eg);
		line(b);
	}
}

// The filesystem-effect half. stdout proves nothing for these tools - `rm a b c`
// printed nothing whether it removed one file or three - so the assertion is on
// what is left on disk afterwards. These expectations are OURS (they are
// statements about this OS's filesystem, not about GNU's output format), so
// they are written here rather than generated.
static void say(int cond, const char *what)
{
	ok(cond, what);
}

static void phase4_effects(void)
{
	char *rm3[]    = { "rm", "/T4R1", "/T4R2", "/T4R3", 0 };
	char *mk3[]    = { "mkdir", "/T4D1", "/T4D2", "/T4D3", 0 };
	char *rmd2[]   = { "rmdir", "/T4D1", "/T4D2", 0 };
	char *tou3[]   = { "touch", "/T4N1", "/T4N2", "/T4N3", 0 };
	char *tee2[]   = { "tee", "/T4O1", "/T4O2", 0 };
	char *cp3[]    = { "cp", F4A, F4B, "/T4DEST", 0 };
	char *mkdest[] = { "mkdir", "/T4DEST", 0 };

	// rm over three operands.
	write_file("/T4R1", "1\n", 2);
	write_file("/T4R2", "2\n", 2);
	write_file("/T4R3", "3\n", 2);
	run_capture("/APPS/RM", rm3, NULL, OUT_PATH);
	say(!file_exists("/T4R1") && !file_exists("/T4R2") && !file_exists("/T4R3"),
	    "rm removed ALL THREE operands (it used to remove only the first)");

	// mkdir over three operands, then rmdir over two.
	run_capture("/APPS/MKDIR", mk3, NULL, OUT_PATH);
	write_file("/T4D1/probe", "x\n", 2);
	write_file("/T4D2/probe", "x\n", 2);
	write_file("/T4D3/probe", "x\n", 2);
	say(file_exists("/T4D1/probe") && file_exists("/T4D2/probe") && file_exists("/T4D3/probe"),
	    "mkdir created ALL THREE directories");
	unlink("/T4D1/probe");
	unlink("/T4D2/probe");
	unlink("/T4D3/probe");
	run_capture("/APPS/RMDIR", rmd2, NULL, OUT_PATH);
	write_file("/T4D1/probe", "x\n", 2);
	write_file("/T4D2/probe", "x\n", 2);
	say(!file_exists("/T4D1/probe") && !file_exists("/T4D2/probe"),
	    "rmdir removed BOTH operands");
	unlink("/T4D1/probe"); unlink("/T4D2/probe");
	unlink("/T4D3/probe");

	// touch over three new files.
	unlink("/T4N1"); unlink("/T4N2"); unlink("/T4N3");
	run_capture("/APPS/TOUCH", tou3, NULL, OUT_PATH);
	say(file_exists("/T4N1") && file_exists("/T4N2") && file_exists("/T4N3"),
	    "touch created ALL THREE files");

	// tee to two files at once, and it must also still write stdout.
	unlink("/T4O1"); unlink("/T4O2");
	int st = run_capture("/APPS/TEE", tee2, F4A, OUT_PATH);
	char a[128], o1[128], o2[128], so[128];
	int na = read_file(F4A, a, sizeof a);
	int n1 = read_file("/T4O1", o1, sizeof o1);
	int n2 = read_file("/T4O2", o2, sizeof o2);
	int ns = read_file(OUT_PATH, so, sizeof so);
	say(st == 0 && n1 == na && n2 == na && ns == na &&
	    memcmp(o1, a, (size_t)na) == 0 && memcmp(o2, a, (size_t)na) == 0 &&
	    memcmp(so, a, (size_t)na) == 0,
	    "tee wrote BOTH files and stdout (it used to write one file, and could "
	    "not create it)");

	// cp of two sources INTO a directory - the form the old cp could not
	// express at all: it copied argv[1] to a FILE named argv[2].
	run_capture("/APPS/MKDIR", mkdest, NULL, OUT_PATH);
	unlink("/T4DEST/T4A.TXT");
	unlink("/T4DEST/T4B.TXT");
	run_capture("/APPS/CP", cp3, NULL, OUT_PATH);
	char c1[128], c2[128], bb[64];
	int m1 = read_file("/T4DEST/T4A.TXT", c1, sizeof c1);
	int nb2 = read_file(F4B, bb, sizeof bb);
	int m2 = read_file("/T4DEST/T4B.TXT", c2, sizeof c2);
	say(m1 == na && memcmp(c1, a, (size_t)na) == 0 &&
	    m2 == nb2 && memcmp(c2, bb, (size_t)nb2) == 0,
	    "cp copied BOTH sources into the destination DIRECTORY");

	// less on a file that fits one screen simply prints it. The regex search
	// is interactive and is proven by tools/coreutils-oracle's directed less
	// section; what this proves is that the binary that now links libregex.a
	// still runs at all on the image.
	char *lessargs[] = { "less", F4A, 0 };
	int lst = run_capture("/APPS/LESS", lessargs, NULL, OUT_PATH);
	char lo[128];
	int ln = read_file(OUT_PATH, lo, sizeof lo);
	say(lst == 0 && ln == na && memcmp(lo, a, (size_t)na) == 0,
	    "less (now linking libregex.a) prints a short file unchanged");

	// date reads the RTC, not sys_time(). The VALUE cannot be asserted - that
	// would be a test of this VM's clock - so what is asserted is that it is a
	// PLAUSIBLE WALL CLOCK rather than an uptime, which is exactly what the
	// old date printed.
	char *dy[] = { "date", "+%Y", 0 };
	int dst = run_capture("/APPS/DATE", dy, NULL, OUT_PATH);
	char yb[64];
	int yn = read_file(OUT_PATH, yb, sizeof yb);
	int year = 0;
	for (int i = 0; i < yn && yb[i] >= '0' && yb[i] <= '9'; i++)
		year = year * 10 + (yb[i] - '0');
	snprintf(bb, sizeof bb, "%d", year);
	say(dst == 0 && year >= 2020 && year <= 2100,
	    "date +%Y from the RTC is a plausible year (the old date printed uptime)");
	{
		char msg[160];
		snprintf(msg, sizeof msg, "[TOOLCHK]      date +%%Y said %d\n", year);
		line(msg);
	}
}

static void phase4(void)
{
	line("[TOOLCHK] ==== phase 4: the shipped second-batch tools ====\n");
	print_sha256("/APPS/RM");
	print_sha256("/APPS/MKDIR");
	print_sha256("/APPS/RMDIR");
	print_sha256("/APPS/TOUCH");
	print_sha256("/APPS/WC");
	print_sha256("/APPS/HEAD");
	print_sha256("/APPS/TAIL");
	print_sha256("/APPS/TEE");
	print_sha256("/APPS/CP");
	print_sha256("/APPS/CUT");
	print_sha256("/APPS/SORT");
	print_sha256("/APPS/TAC");
	print_sha256("/APPS/DATE");
	print_sha256("/APPS/LESS");

	if (build_fixture() != 0) {
		line("[TOOLCHK] FAIL phase 4: could not build the fixture\n");
		g_fail++;
		return;
	}
	if (verify_fixture() != 0) {
		line("[TOOLCHK] phase 4 ABORTED: the fixture does not match the one the\n");
		line("[TOOLCHK] expectations were generated against, so a comparison would\n");
		line("[TOOLCHK] be meaningless. Regenerate vmtable2.h.\n");
		return;
	}
	phase4_cases();
	phase4_effects();
}

// ===========================================================================
// PHASE 5: the shipped THIRD-batch tools - yes, uniq, stat, uname, sleep, ls
//
// WHAT THIS PHASE HAS THAT PHASE 4 DID NOT NEED: A CONTROL RUNNING IN THE SAME
// BOOT. Four of these six defects cannot be seen in a stdout comparison at all
// - yes's is that it never TERMINATES, sleep's is that it REFUSES a fraction,
// stat's is that it CANNOT OPEN A DIRECTORY, uname's is a hardcoded string -
// so the evidence that the fix did anything is the PRE-FIX BINARY failing the
// same assertion on the same machine, seconds apart. The overlay that prepares
// the test image writes those as /APPS/YESOLD, /APPS/UNIQOLD, /APPS/STATOLD,
// /APPS/UNAMEOLD, /APPS/SLEEPOLD and /APPS/LSOLD, built from the commit named
// in tools/coreutils-oracle (fake_ref_for). They are a MEASUREMENT, not a
// shipped artifact: nothing in build/ installs them, and if they are absent this
// phase says so LOUDLY and scores nothing rather than quietly skipping - a
// missing control that reports nothing is how a green run gets believed.
// ===========================================================================

#define F5DIR   "/T5FIX"
#define F5SUB   "/T5FIX/T5SUB"
#define F5DUP   "/T5FIX/T5DUP.TXT"
#define F5A     "/T5FIX/T5A.TXT"
#define F5B     "/T5FIX/T5B.TXT"
#define F5LONG  "/T5FIX/T5LONG.TXT"
#define F5N     "/T5FIX/T5SUB/N.TXT"

static int build_fixture5(void)
{
	// The fixture is a DIRECTORY because ls with no operand lists the current
	// one, and this machine's is the ext2 root: a hundred-odd entries that no
	// host directory can be made to match.
	// #115: THE ENTRIES ARE CREATED ONE SECOND APART, ON PURPOSE.
	//
	// Until #115 this kernel filled no timestamp, so `ls -t` was a REFUSAL case
	// here and the write order did not matter. It sorts for real now, and a
	// fixture written in a single burst would give all five entries the SAME
	// mtime (ext2 and FAT both store whole seconds), every comparison would be
	// a tie, and -t would fall back to name order - producing a green result
	// whether or not the sort works at all. A test that cannot go red is the
	// defect this ticket is about, so the gaps are load-bearing.
	//
	// THE ORDER IS DELIBERATELY NOT NAME ORDER. Oldest to newest:
	//   T5SUB, T5DUP.TXT, T5A.TXT, T5B.TXT, T5LONG.TXT
	// so `ls -t` (newest first) must print T5LONG, T5B, T5A, T5DUP, T5SUB -
	// which is neither alphabetical nor reverse-alphabetical, so neither a
	// name sort nor a reversed name sort can pass by accident.
	//
	// N.TXT is written INSIDE T5SUB first, so T5SUB's own entry is the oldest
	// thing at the top level rather than the newest.
	mkdir(F5DIR, 0755);
	mkdir(F5SUB, 0755);
	if (write_file(F5N, "nested\n", 7) != 0) return -1;
	sys_sleep(1100);
	if (write_file(F5DUP, "a\na\nb\nB\nb\nc\nc\nc\nd\n", 18) != 0) return -1;
	sys_sleep(1100);
	if (write_file(F5A, "alpha\nbravo\n", 12) != 0) return -1;
	sys_sleep(1100);
	if (write_file(F5B, "zulu\n", 5) != 0) return -1;
	sys_sleep(1100);

	// Two lines sharing an 1100-character prefix. The OLD uniq treated "the
	// 1024-byte line buffer is full" as END OF LINE, so it saw four records
	// where there are two - and the two 1023-byte halves were IDENTICAL, so it
	// DELETED one. That is invented adjacency, not truncation, which is why
	// this file is the one that matters here.
	unlink(F5LONG);
	int fd = open(F5LONG, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) return -1;
	static char pfx[1101];
	for (int i = 0; i < 1100; i++) pfx[i] = 'x';
	pfx[1100] = '\0';
	if (append_str(fd, pfx) != 0 || append_str(fd, "AAA\n") != 0 ||
	    append_str(fd, pfx) != 0 || append_str(fd, "BBB\n") != 0) { close(fd); return -1; }
	close(fd);
	return 0;
}

static int verify_fixture5(void)
{
	char path[64], hex[80], b[320];
	int bad = 0;
	for (unsigned i = 0; i < sizeof VMFIX3 / sizeof VMFIX3[0]; i++) {
		snprintf(path, sizeof path, "%s/%s", F5DIR, VMFIX3[i].name);
		if (sha256_file_hex(path, hex) != 0) {
			snprintf(b, sizeof b, "[TOOLCHK] FIXTURE %s: CANNOT READ\n", path);
			line(b); bad = 1; continue;
		}
		int good = (strcmp(hex, VMFIX3[i].sha256) == 0);
		snprintf(b, sizeof b, "[TOOLCHK] %s fixture5 %-12s %s\n",
		         good ? "PASS" : "FAIL", VMFIX3[i].name, hex);
		line(b);
		if (good) g_pass++; else { g_fail++; bad = 1; }
	}
	return bad;
}

static void phase5_cases(void)
{
	static char got[8192];
	char eg[4096], ew[4096], b[9000];

	for (unsigned i = 0; i < sizeof VMCASES3 / sizeof VMCASES3[0]; i++) {
		const struct vmcase3 *t = &VMCASES3[i];
		int st = run_capture(t->path, t->argv, NULL, OUT_PATH);
		int n = read_file(OUT_PATH, got, sizeof got);
		if (n < 0) n = 0;
		got[n] = '\0';
		int want = (int)strlen(t->want);
		int good = (st == 0) && (n == want) && memcmp(got, t->want, (size_t)n) == 0;
		if (good) g_pass++; else g_fail++;
		escape(got, n, eg, sizeof eg);
		snprintf(b, sizeof b, "[TOOLCHK] %s %-22s exit=%d -> \"%s\"\n",
		         good ? "PASS" : "FAIL", t->name, st, eg);
		line(b);
		if (!good) {
			escape(t->want, want, ew, sizeof ew);
			snprintf(b, sizeof b, "[TOOLCHK]      GNU said \"%s\"\n", ew);
			line(b);
		}
	}

	for (unsigned i = 0; i < sizeof VMREFUSE3 / sizeof VMREFUSE3[0]; i++) {
		const struct vmref3 *t = &VMREFUSE3[i];
		int st = run_capture(t->path, t->argv, NULL, OUT_PATH);
		int n = read_file(OUT_PATH, got, sizeof got);
		if (n < 0) n = 0;
		int good = (st != 0) && (n == 0);
		if (good) g_pass++; else g_fail++;
		escape(got, n, eg, sizeof eg);
		snprintf(b, sizeof b, "[TOOLCHK] %s refuses %-20s exit=%d stdout=\"%s\"\n",
		         good ? "PASS" : "FAIL", t->name, st, eg);
		line(b);
	}
}

// Run PATH with ARGV and report whether stdout contains NEEDLE (want=1) or does
// not (want=0), together with the exit status class.
static void phase5_says(const char *what, const char *path, char *const argv[],
                        const char *needle, int want_present, int want_zero_exit)
{
	static char got[8192];
	char eg[2048], b[4096];
	int st = run_capture(path, argv, NULL, OUT_PATH);
	int n = read_file(OUT_PATH, got, sizeof got);
	if (n < 0) n = 0;
	got[n] = '\0';
	int present = (needle && *needle) ? (strstr(got, needle) != 0) : 0;
	int good = ((st == 0) == (want_zero_exit != 0)) && (present == want_present);
	if (good) g_pass++; else g_fail++;
	escape(got, n > 200 ? 200 : n, eg, sizeof eg);
	snprintf(b, sizeof b, "[TOOLCHK] %s %-34s exit=%d \"%s\"%s -> \"%s\"\n",
	         good ? "PASS" : "FAIL", what, st, needle ? needle : "",
	         want_present ? " expected" : " must NOT appear", eg);
	line(b);
}

static int control_present(const char *path, const char *what)
{
	char b[320];
	if (file_exists(path)) return 1;
	// LOUD, and it costs a FAIL. A control that is silently absent turns a
	// before/after claim into an unsupported assertion, and this project has
	// shipped past a printed skip more than once.
	g_fail++;
	snprintf(b, sizeof b, "[TOOLCHK] FAIL %s: control binary %s is NOT on this image, "
	                      "so the before/after comparison did not run\n", what, path);
	line(b);
	return 0;
}

// --- the yes termination test ----------------------------------------------
//
// `yes | head -1` cannot be run unbounded, so the bound is a WATCHDOG THREAD
// that kills the shell after a deadline while the main thread does the
// blocking wait (waitpid() here has no WNOHANG; the kernel ignores the flag).
//
// #111 CHANGED THE PREMISE OF THIS TEST AND OF ITS CONTROL ARM. This comment
// used to say "there is no SIGPIPE on this OS to make it (kernel/fs/pipe.c
// returns -1 and raises nothing; ticket 111)". As of build 1994 that is FALSE:
// pipe_write_fn() raises SIGPIPE and returns -EPIPE, and the POSIX default
// action terminates the writer.
//
// So the CONTROL ARM (g_yes_expect_terminate == 0, i.e. "the pre-fix `yes`,
// which discards its write result, should NOT terminate") is now WRONG at the
// kernel level: SIGPIPE stops that binary too. If /APPS/YESOLD is ever staged
// again, this test would report FAIL for a run in which everything worked. The
// expectation is corrected below: BOTH arms must now terminate, and what
// distinguishes them is HOW - the checking binary exits 0, the ignoring one is
// killed with 128+13 = 141.
//
// The "orphaned producer keeps spinning" note in the watchdog below was also a
// consequence of the missing signal and no longer applies.
static volatile int g_yes_done;
static volatile int g_yes_pid;
static volatile int g_yes_expect_terminate;
static char g_yes_cmd[128];

// Print the run total and stop. Called from main() normally, and from the
// watchdog when the main thread can no longer get there.
static void finish_and_exit(void)
{
	char b[256];
	snprintf(b, sizeof b, "[TOOLCHK] ==== done: %d passed, %d failed ====\n", g_pass, g_fail);
	line(b);
	_exit(g_fail ? 1 : 0);
}

static void *yes_watchdog(void *arg)
{
	unsigned long limit = (unsigned long)(long)arg;
	unsigned long t0 = uptime_ms();
	while (uptime_ms() - t0 < limit) {
		if (g_yes_done) return 0;
		sys_sleep(100);
	}
	if (g_yes_done || g_yes_pid <= 0) return 0;

	kill(g_yes_pid, 9);

	// MEASURED: that does not free the main thread. waitpid() on a shell that is
	// itself blocked in waitpid() does not return when the shell is SIGKILLed,
	// so the verdict is printed HERE and the run ends here. This is the last
	// case in the suite for exactly that reason.
	//
	// #111: reaching this watchdog is now a FAILURE FOR EITHER ARM. Before
	// build 1994 a producer that ignored its write result genuinely could not
	// terminate, so "did not terminate" was the CORRECT expectation for the
	// control arm; SIGPIPE now stops that binary too, so any pipeline that is
	// still alive at the deadline is a real defect regardless of which binary
	// is being tested.
	{
		char b[512];
		int good = 0;
		if (good) g_pass++; else g_fail++;
		snprintf(b, sizeof b,
		         "[TOOLCHK] %s `%s` DID NOT TERMINATE: still running %lu ms after "
		         "its consumer exited, killed by the watchdog (expected it to %s)\n",
		         good ? "PASS" : "FAIL", g_yes_cmd, limit,
		         g_yes_expect_terminate ? "terminate" : "hang");
		line(b);
	}
	line("[TOOLCHK] (the orphaned producer is still spinning; the kernel heartbeat's\n"
	     "[TOOLCHK]  top= field names it. That is the defect, not a harness fault.)\n");
	finish_and_exit();
	return 0;
}

// Returns elapsed milliseconds, and sets *killed when the watchdog had to fire.
static unsigned long run_pipeline_bounded(const char *cmdline, unsigned long limit_ms,
                                          int *killed)
{
	posix_spawn_file_actions_t fa;
	pid_t pid = 0;
	int st = 0;
	char *av[4];
	pthread_t th;

	av[0] = "msh"; av[1] = "-c"; av[2] = (char *)cmdline; av[3] = 0;

	unlink(OUT_PATH);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, OUT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	g_yes_done = 0;
	g_yes_pid = 0;
	unsigned long t0 = uptime_ms();
	int rc = posix_spawn(&pid, "/APPS/MSH", &fa, NULL, av, NULL);
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0) { *killed = 0; return 0; }
	g_yes_pid = (int)pid;
	if (pthread_create(&th, NULL, yes_watchdog, (void *)(long)limit_ms) != 0) {
		// No watchdog means no bound, and an unbounded wait here would hang the
		// whole harness. Refuse to run the case rather than risk that.
		kill((int)pid, 9);
		*killed = -1;
		return 0;
	}
	waitpid(pid, &st, 0);
	unsigned long ms = uptime_ms() - t0;
	g_yes_done = 1;
	pthread_join(th, NULL);
	*killed = (ms >= limit_ms) ? 1 : 0;
	return ms;
}

static void phase5_yes_terminates(const char *bin_name, int expect_terminate)
{
	char cmd[128], b[512];
	int killed = 0;
	snprintf(cmd, sizeof cmd, "%s | HEAD -1", bin_name);
	snprintf(g_yes_cmd, sizeof g_yes_cmd, "%s", cmd);
	g_yes_expect_terminate = expect_terminate;
	unsigned long ms = run_pipeline_bounded(cmd, 6000, &killed);
	if (killed < 0) {
		g_fail++;
		snprintf(b, sizeof b, "[TOOLCHK] FAIL `%s`: no watchdog thread, case not run\n", cmd);
		line(b);
		return;
	}
	int terminated = (killed == 0);
	int good = (terminated == expect_terminate);
	if (good) g_pass++; else g_fail++;
	snprintf(b, sizeof b, "[TOOLCHK] %s `%s` %s after %lu ms (expected it to %s)\n",
	         good ? "PASS" : "FAIL", cmd,
	         terminated ? "TERMINATED" : "DID NOT TERMINATE, killed by the watchdog",
	         ms, expect_terminate ? "terminate" : "hang");
	line(b);
}

static void phase5_directed(void)
{
	char b[512], vbuf[96], rel[96];
	static char got[4096];
	char *av[6];

	// --- stat: the thing it could not do at all -----------------------------
	av[0] = "stat"; av[1] = F5DIR; av[2] = 0;
	phase5_says("stat can stat a DIRECTORY", "/APPS/STAT", av, "directory", 1, 1);
	av[0] = "stat"; av[1] = F5A; av[2] = 0;
	phase5_says("stat reports a regular file", "/APPS/STAT", av, "regular file", 1, 1);
	// #115: this used to assert `Times: unavailable`, which was correct while
	// sys_stat_path zero-filled every timestamp. It asserts the OPPOSITE now,
	// because the fields are real. Both halves are kept: that the value is
	// there AND that a zero is never dressed up as a date.
	phase5_says("stat reports a modification time", "/APPS/STAT", av, "Modify: 20", 1, 1);
	phase5_says("stat reports an inode number", "/APPS/STAT", av, "Inode: ", 1, 1);
	// STILL THE WHOLE POINT: no field is printed as a plausible zero. An
	// unknown timestamp prints the word "unknown", never an epoch-zero date.
	phase5_says("stat invents no timestamp", "/APPS/STAT", av, "1970-01-01", 0, 1);
	phase5_says("stat invents no FAT epoch date", "/APPS/STAT", av, "1980-01-01", 0, 1);
	av[0] = "stat"; av[1] = F5A; av[2] = F5B; av[3] = 0;
	phase5_says("stat visits every operand", "/APPS/STAT", av, "T5B.TXT", 1, 1);

	// --- uname: the version must come from the KERNEL ------------------------
	vbuf[0] = 0;
	int vl = get_version(vbuf, (int)sizeof vbuf);
	if (vl <= 0) {
		g_fail++;
		line("[TOOLCHK] FAIL uname: SYS_GET_VERSION returned nothing, so there is "
		     "no second source to check uname against\n");
	} else {
		int i = 0;
		while (vbuf[i] && vbuf[i] != ' ' && i < (int)sizeof rel - 1) { rel[i] = vbuf[i]; i++; }
		rel[i] = 0;
		snprintf(b, sizeof b, "[TOOLCHK] SYS_GET_VERSION says \"%s\"; release is \"%s\"\n", vbuf, rel);
		line(b);
		av[0] = "uname"; av[1] = "-r"; av[2] = 0;
		phase5_says("uname -r is the LIVE kernel release", "/APPS/UNAME", av, rel, 1, 1);
		av[1] = "-a";
		phase5_says("uname -a carries the live release", "/APPS/UNAME", av, rel, 1, 1);
		phase5_says("uname -a has no hardcoded 1.8.2", "/APPS/UNAME", av, "1.8.2", 0, 1);
		phase5_says("uname -a invents no hostname", "/APPS/UNAME", av, "maytera ", 0, 1);
	}

	// --- sleep: fractions, and that it really sleeps -------------------------
	{
		unsigned long t0, ms;
		int st;
		av[0] = "sleep"; av[1] = "0.5"; av[2] = 0;
		t0 = uptime_ms();
		st = run_capture("/APPS/SLEEP", av, NULL, OUT_PATH);
		ms = uptime_ms() - t0;
		int good = (st == 0) && (ms >= 400) && (ms <= 4000);
		if (good) g_pass++; else g_fail++;
		snprintf(b, sizeof b, "[TOOLCHK] %s sleep 0.5 exit=%d took %lu ms (wanted 400..4000)\n",
		         good ? "PASS" : "FAIL", st, ms);
		line(b);

		av[1] = "0";
		t0 = uptime_ms();
		st = run_capture("/APPS/SLEEP", av, NULL, OUT_PATH);
		ms = uptime_ms() - t0;
		good = (st == 0) && (ms <= 1500);
		if (good) g_pass++; else g_fail++;
		snprintf(b, sizeof b, "[TOOLCHK] %s sleep 0 exit=%d took %lu ms (wanted a prompt return)\n",
		         good ? "PASS" : "FAIL", st, ms);
		line(b);

		// An interval error must be found BEFORE anything sleeps.
		av[1] = "1"; av[2] = "x"; av[3] = 0;
		t0 = uptime_ms();
		st = run_capture("/APPS/SLEEP", av, NULL, OUT_PATH);
		ms = uptime_ms() - t0;
		int n = read_file(OUT_PATH, got, sizeof got);
		if (n < 0) n = 0;
		good = (st != 0) && (ms <= 1000) && (n == 0);
		if (good) g_pass++; else g_fail++;
		snprintf(b, sizeof b, "[TOOLCHK] %s sleep 1 x exit=%d after %lu ms, %d bytes of stdout "
		                      "(must reject without sleeping)\n",
		         good ? "PASS" : "FAIL", st, ms, n);
		line(b);
	}
}

static void phase5_controls(void)
{
	char *av[6];

	line("[TOOLCHK] ---- controls: the PRE-FIX binaries, same boot ----\n");

	if (control_present("/APPS/STATOLD", "stat")) {
		av[0] = "stat"; av[1] = F5DIR; av[2] = 0;
		// MEASURED, and it is not what I assumed: the old stat does not fail on a
		// directory, it reports one AS AN EMPTY FILE - exit 0, "Size: 0 bytes",
		// no type - because open() succeeds and lseek(SEEK_END) returns 0. That
		// is the worse failure mode of the two, and it is why the assertion is
		// "the word directory never appears" rather than "it exits non-zero".
		phase5_says("CONTROL old stat calls a directory an empty file",
		            "/APPS/STATOLD", av, "directory", 0, 1);
	}
	if (control_present("/APPS/UNAMEOLD", "uname")) {
		av[0] = "uname"; av[1] = "-a"; av[2] = 0;
		phase5_says("CONTROL old uname printed a hardcoded 1.8.2", "/APPS/UNAMEOLD", av, "1.8.2", 1, 1);
	}
	if (control_present("/APPS/SLEEPOLD", "sleep")) {
		av[0] = "sleep"; av[1] = "0.5"; av[2] = 0;
		phase5_says("CONTROL old sleep refused a fraction", "/APPS/SLEEPOLD", av, "invalid time interval", 1, 0);
	}
	if (control_present("/APPS/LSOLD", "ls")) {
		av[0] = "ls"; av[1] = "-t"; av[2] = F5DIR; av[3] = 0;
		// Exit ZERO is the defect: -t was parsed and thrown away, so the user
		// got name order and no indication that the sort had not happened.
		phase5_says("CONTROL old ls accepted -t and did not sort", "/APPS/LSOLD", av, "", 0, 1);
	}
	if (control_present("/APPS/UNIQOLD", "uniq")) {
		static char got[8192];
		char b[512], eg[1024];
		av[0] = "uniq"; av[1] = F5LONG; av[2] = 0;
		int st = run_capture("/APPS/UNIQOLD", av, NULL, OUT_PATH);
		int n = read_file(OUT_PATH, got, sizeof got);
		if (n < 0) n = 0;
		// GNU's answer for this file is the two whole lines, 2208 bytes. The old
		// uniq split each at 1023 bytes and then de-duped the two IDENTICAL
		// halves, so it emits LESS than the input.
		int good = (n != 2208);
		if (good) g_pass++; else g_fail++;
		escape(got, 40, eg, sizeof eg);
		snprintf(b, sizeof b, "[TOOLCHK] %s CONTROL old uniq mangled the long lines: "
		                      "exit=%d produced %d bytes, not GNU's 2208 (\"%s...\")\n",
		         good ? "PASS" : "FAIL", st, n, eg);
		line(b);
	}

	// LAST, because the pre-fix producer survives being orphaned.
	if (control_present("/APPS/YESOLD", "yes"))
		phase5_yes_terminates("YESOLD", 0);
}

static void phase5(void)
{
	line("[TOOLCHK] ==== phase 5: the shipped third-batch tools ====\n");
	print_sha256("/APPS/YES");
	print_sha256("/APPS/UNIQ");
	print_sha256("/APPS/STAT");
	print_sha256("/APPS/UNAME");
	print_sha256("/APPS/SLEEP");
	print_sha256("/APPS/LS.ELF");
	print_sha256("/APPS/TOOLCHK");

	if (build_fixture5() != 0) {
		line("[TOOLCHK] FAIL phase 5: could not build the fixture\n");
		g_fail++;
		return;
	}
	if (verify_fixture5() != 0) {
		line("[TOOLCHK] phase 5 ABORTED: the fixture does not match the one the\n");
		line("[TOOLCHK] expectations were generated against. Regenerate vmtable3.h.\n");
		return;
	}
	phase5_cases();
	phase5_directed();
	// The fixed yes must terminate. This is the assertion the whole ticket item
	// is about, and it is bounded so a failure cannot hang the harness.
	phase5_yes_terminates("YES", 1);
	phase5_controls();
}


// ===========================================================================
// PHASE 6: /APPS/MV  (#108 follow-up)
//
// mv was NOT in local 103's survey of the class "named after a standard
// utility, implements a fraction, REPORTS NO ERROR" - not tier 1, not tier 2,
// and not the checked-and-honest list - so it was never fixed alongside its
// siblings, and it was the most destructive member of the class. `mv a b dest`
// renamed a ONTO b, obliterating a file the user had named only as a SOURCE,
// ignored dest entirely, and exited 0.
//
// A STDOUT COMPARISON CANNOT SEE ANY OF THAT. The old mv printed nothing on
// its success path and neither does the new one, so every assertion below is
// on FILE CONTENT after the run - the same reason the hosted oracle renders a
// tree dump for the filesystem-mutating tools instead of diffing stdout.
//
// The fixture is rebuilt before every case, so no case can inherit the
// previous one's state (the O_TRUNC family of mistake, blame.md local 103).
// ===========================================================================

#define MVDIR   "/T108MV"
#define MVDEST  "/T108MV/dest"

static int mv_fixture(void)
{
	unlink(MVDIR "/a.txt");  unlink(MVDIR "/b.txt");  unlink(MVDIR "/c.txt");
	unlink(MVDIR "/renamed.txt");
	unlink(MVDEST "/a.txt"); unlink(MVDEST "/b.txt"); unlink(MVDEST "/c.txt");
	mkdir(MVDIR, 0755);
	mkdir(MVDEST, 0755);
	if (write_file(MVDIR "/a.txt", "AAA\n", 4) != 0) return -1;
	if (write_file(MVDIR "/b.txt", "BBB\n", 4) != 0) return -1;
	if (write_file(MVDIR "/c.txt", "CCC\n", 4) != 0) return -1;
	return 0;
}

// Assert a file's exact content, or its ABSENCE when want is NULL.
static void mv_content(const char *what, const char *path, const char *want)
{
	char got[256], b[640], eg[512];
	int n = read_file(path, got, sizeof got);
	int good;
	if (!want) {
		good = (n < 0);
		snprintf(b, sizeof b, "[TOOLCHK] %s %-46s (must not exist, n=%d)\n",
		         good ? "PASS" : "FAIL", what, n);
	} else {
		good = (n >= 0) && strcmp(got, want) == 0;
		escape(n > 0 ? got : "", n > 0 ? n : 0, eg, sizeof eg);
		snprintf(b, sizeof b, "[TOOLCHK] %s %-46s = \"%s\"\n",
		         good ? "PASS" : "FAIL", what, eg);
	}
	if (good) g_pass++; else g_fail++;
	line(b);
}

// The headline case, run against either the shipped mv or the pre-fix control.
// WHEN is_control IS SET THIS DELIBERATELY ASSERTS THE DEFECT. That is what a
// control arm is for and it is labelled as one; it runs only when /APPS/MVOLD
// has been staged, and it is what makes "the fix works" a before/after
// measurement in ONE boot rather than an unsupported claim.
static void phase6_two_into_dir(const char *bin, int is_control)
{
	char b[256];
	char *av[6];
	int st;

	if (mv_fixture() != 0) {
		g_fail++;
		line("[TOOLCHK] FAIL phase 6: could not build the fixture\n");
		return;
	}
	av[0] = (char *)"mv";
	av[1] = (char *)MVDIR "/a.txt";
	av[2] = (char *)MVDIR "/b.txt";
	av[3] = (char *)MVDEST;
	av[4] = 0;
	st = run_capture(bin, av, NULL, OUT_PATH);
	snprintf(b, sizeof b, "[TOOLCHK] %s `mv a.txt b.txt dest` exit=%d\n",
	         is_control ? "CONTROL" : "RUN    ", st);
	line(b);

	if (!is_control) {
		mv_content("mv a b dest: a arrived in dest",  MVDEST "/a.txt", "AAA\n");
		mv_content("mv a b dest: b arrived in dest",  MVDEST "/b.txt", "BBB\n");
		mv_content("mv a b dest: a left its old path", MVDIR "/a.txt", NULL);
		mv_content("mv a b dest: b left its old path", MVDIR "/b.txt", NULL);
		ok(st == 0, "mv a b dest exits 0");
	} else {
		mv_content("CONTROL old mv OVERWROTE b with a", MVDIR "/b.txt", "AAA\n");
		mv_content("CONTROL old mv left dest empty",    MVDEST "/a.txt", NULL);
		ok(st == 0, "CONTROL old mv exited 0 while destroying b.txt");
	}
}

static void phase6(void)
{
	char *av[6];
	int st;

	line("[TOOLCHK] ==== phase 6: /APPS/MV (#108 follow-up) ====\n");
	print_sha256("/APPS/MV");

	phase6_two_into_dir("/APPS/MV", 0);

	// A plain rename still works.
	mv_fixture();
	av[0] = (char *)"mv";
	av[1] = (char *)MVDIR "/c.txt";
	av[2] = (char *)MVDIR "/renamed.txt";
	av[3] = 0;
	st = run_capture("/APPS/MV", av, NULL, OUT_PATH);
	ok(st == 0, "mv SRC DST exits 0");
	mv_content("mv SRC DST: content arrived", MVDIR "/renamed.txt", "CCC\n");
	mv_content("mv SRC DST: source is gone",  MVDIR "/c.txt", NULL);

	// THE REFUSAL THAT MATTERS: three operands whose last is not a directory is
	// an error, and NOTHING may be moved or overwritten on the way to saying so.
	// This is the exact command that used to destroy b.txt silently.
	mv_fixture();
	av[0] = (char *)"mv";
	av[1] = (char *)MVDIR "/a.txt";
	av[2] = (char *)MVDIR "/b.txt";
	av[3] = (char *)MVDIR "/c.txt";
	av[4] = 0;
	st = run_capture("/APPS/MV", av, NULL, OUT_PATH);
	ok(st != 0, "mv a b c (c is not a directory) is REFUSED");
	mv_content("refusal moved nothing: a intact", MVDIR "/a.txt", "AAA\n");
	mv_content("refusal moved nothing: b intact", MVDIR "/b.txt", "BBB\n");
	mv_content("refusal moved nothing: c intact", MVDIR "/c.txt", "CCC\n");

	// -i is refused rather than silently not prompting.
	mv_fixture();
	av[0] = (char *)"mv";
	av[1] = (char *)"-i";
	av[2] = (char *)MVDIR "/a.txt";
	av[3] = (char *)MVDIR "/b.txt";
	av[4] = 0;
	st = run_capture("/APPS/MV", av, NULL, OUT_PATH);
	ok(st != 0, "mv -i is REFUSED, not silently non-prompting");
	mv_content("mv -i moved nothing", MVDIR "/a.txt", "AAA\n");

	// -n must not clobber, and that is not an error.
	mv_fixture();
	av[0] = (char *)"mv";
	av[1] = (char *)"-n";
	av[2] = (char *)MVDIR "/a.txt";
	av[3] = (char *)MVDIR "/b.txt";
	av[4] = 0;
	st = run_capture("/APPS/MV", av, NULL, OUT_PATH);
	ok(st == 0, "mv -n exits 0");
	mv_content("mv -n did not clobber b", MVDIR "/b.txt", "BBB\n");
	mv_content("mv -n left a in place",   MVDIR "/a.txt", "AAA\n");

	if (control_present("/APPS/MVOLD", "mv")) {
		print_sha256("/APPS/MVOLD");
		phase6_two_into_dir("/APPS/MVOLD", 1);
	}
}

int main(int argc, char **argv)
{
	if (argc >= 2) {
		if (!strcmp(argv[1], "--pipe-produce"))      return mode_produce(1000000L, 0);
		if (!strcmp(argv[1], "--pipe-produce-full")) return mode_produce(PRODUCE_LIMIT, 1);
		if (!strcmp(argv[1], "--pipe-consume"))      return mode_consume();
		if (!strcmp(argv[1], "--pipe-count"))        return mode_count();
		line("[TOOLCHK] unknown mode; run with no arguments for the test suite\n");
		return 2;
	}

	line("[TOOLCHK] ==== #745 local 108: tr, env, msh pipelines, and the\n");
	line("[TOOLCHK]      second batch of silently-wrong tools ====\n");
	line("[TOOLCHK]      plus phase 6: mv, which the survey never listed ====\n");
	phase1();
	phase2();
	phase3();
	phase4();
	phase5();
	phase6();

	finish_and_exit();
	return g_fail ? 1 : 0;
}
