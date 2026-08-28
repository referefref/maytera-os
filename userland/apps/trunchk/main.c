// trunchk - proof, on a booted MayteraOS, of what O_TRUNC actually does to a
// file on each of the two shipping filesystems (#745 local 109).
//
// WHY THIS APP EXISTS. blame.md (local 103) recorded that a file opened
// O_TRUNC whose writer then produces ZERO bytes keeps its old contents, while
// a writer that produces ANY byte truncates correctly. That is a claim about
// two filesystems, two open paths (sys_open and the spawn redirect) and a
// deferred flush, and none of it can be settled by reading code: the ext2 and
// FAT open paths BOTH read as correct. So this measures it on the machine.
//
// WHAT IT COVERS, and every one of these has to be reported both before and
// after any fix, because a fix shown only passing is indistinguishable from no
// fix:
//   A  zero-byte O_TRUNC   open, close, no write  -> the file must read EMPTY
//   B  short-after-long    O_TRUNC then 2 bytes   -> exactly the 2 bytes
//   C  append              O_APPEND, no O_TRUNC   -> old + new, nothing lost
//   D  trunc-then-write    O_TRUNC then new text  -> new text ONLY, no tail
//   E  spawn redirect, child writes ZERO bytes    -> the file must read EMPTY
//   F  spawn redirect, child writes a short line  -> exactly that line
//   G  ftruncate(fd, 0)    what the libc stub actually does
//
// A, B, C and D run TWICE: once on the ext2 root ("/T109E.TXT") and once on
// the FAT ESP ("/boot/T109F.TXT"). path_root_ext2() in the kernel routes every
// "/" path to ext2 EXCEPT /boot and /EFI, so those two paths are how a Ring-3
// program addresses one filesystem or the other by name. A defect present in
// one and not the other is itself the finding.
//
// SEEDING IS DONE WITH unlink() FIRST, NEVER WITH O_TRUNC. The bug under test
// is in O_TRUNC, so using it to construct the precondition would make every
// case depend on the thing being measured. An assertion about absence needs the
// absence to have been constructed, not inherited.
//
// OUTPUT DISCIPLINE. Launched from /CONFIG/AUTORUN.CFG. An autorun-launched
// process emits ONE SERIAL RECORD PER write(), so every line is formatted into
// a buffer and issued as exactly one write(2, ...).

#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "fcntl.h"
#include "spawn.h"
#include "sys/wait.h"

#define P_EXT2 "/T109E.TXT"
#define P_FAT  "/boot/T109F.TXT"
#define P_RED  "/T109R.TXT"

#define LONGTEXT "LONG-PREVIOUS-CONTENTS-0123456789"
#define SHORTTXT "hi"
#define NEWTEXT  "NEW"

static int g_pass = 0, g_fail = 0;

static void line(const char *s) { write(2, s, strlen(s)); }

static void ok(int cond, const char *what)
{
	char b[320];
	if (cond) g_pass++; else g_fail++;
	snprintf(b, sizeof b, "[TRUNCHK] %s %s\n", cond ? "PASS" : "FAIL", what);
	line(b);
}

// Read PATH whole. Returns bytes read (0 for an empty file), or -1 if the file
// could not be opened at all. OUT is always NUL-terminated.
static int slurp(const char *path, char *out, int outsz)
{
	int fd = open(path, O_RDONLY, 0);
	if (fd < 0) { out[0] = '\0'; return -1; }
	int total = 0;
	for (;;) {
		int n = (int)read(fd, out + total, (size_t)(outsz - 1 - total));
		if (n <= 0) break;
		total += n;
		if (total >= outsz - 1) break;
	}
	close(fd);
	out[total] = '\0';
	return total;
}

// Put exactly TEXT into PATH, without going anywhere near O_TRUNC.
static int seed(const char *path, const char *text)
{
	unlink(path);
	int fd = open(path, O_WRONLY | O_CREAT, 0644);
	if (fd < 0) return -1;
	int n = (int)write(fd, text, strlen(text));
	close(fd);
	return n == (int)strlen(text) ? 0 : -1;
}

// Report a case that compares the whole file against WANT ("" means empty).
static void expect(const char *what, const char *path, const char *want)
{
	char got[512], b[768];
	int n = slurp(path, got, sizeof got);
	int good = (n >= 0) && (n == (int)strlen(want)) && strcmp(got, want) == 0;
	ok(good, what);
	if (!good) {
		snprintf(b, sizeof b, "[TRUNCHK]      %s: got %d bytes '%s', wanted %d bytes '%s'\n",
			 path, n, got, (int)strlen(want), want);
		line(b);
	}
}

// ---------------------------------------------------------------------------
// A-D, run once per filesystem.
// ---------------------------------------------------------------------------
static void fs_cases(const char *fsname, const char *path)
{
	char what[160];

	// A: THE DEFECT. O_TRUNC, then close with nothing written.
	if (seed(path, LONGTEXT) != 0) {
		snprintf(what, sizeof what, "%s A zero-byte O_TRUNC: could not seed %s", fsname, path);
		ok(0, what);
	} else {
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0) {
			snprintf(what, sizeof what, "%s A zero-byte O_TRUNC: open failed", fsname);
			ok(0, what);
		} else {
			close(fd);
			snprintf(what, sizeof what, "%s A zero-byte O_TRUNC leaves the file EMPTY", fsname);
			expect(what, path, "");
		}
	}

	// B: short after long. The control that says truncation works at all.
	if (seed(path, LONGTEXT) == 0) {
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			write(fd, SHORTTXT, strlen(SHORTTXT));
			close(fd);
		}
		snprintf(what, sizeof what, "%s B short-after-long still truncates", fsname);
		expect(what, path, SHORTTXT);
	}

	// C: append must NOT truncate.
	if (seed(path, "AAA") == 0) {
		int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			write(fd, "BBB", 3);
			close(fd);
		}
		snprintf(what, sizeof what, "%s C O_APPEND does NOT truncate", fsname);
		expect(what, path, "AAABBB");
	}

	// D: O_TRUNC then a normal write leaves ONLY the new content.
	if (seed(path, LONGTEXT) == 0) {
		int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			write(fd, NEWTEXT, strlen(NEWTEXT));
			close(fd);
		}
		snprintf(what, sizeof what, "%s D O_TRUNC then write holds ONLY the new content", fsname);
		expect(what, path, NEWTEXT);
	}
}

// ---------------------------------------------------------------------------
// E-F: the shape local 103 actually hit. posix_spawn maps an addopen on fd 1
// to SYS_SPAWN_REDIR, so the child's stdout is a kernel-side file description
// opened O_WRONLY|O_CREAT|O_TRUNC that the child may never write to.
// ---------------------------------------------------------------------------
static int run_to_file(const char *arg, const char *outfile)
{
	posix_spawn_file_actions_t fa;
	pid_t pid = 0;
	int st = 0, rc;
	char *argv[3];
	argv[0] = (char *)"/APPS/TRUNCHK";
	argv[1] = (char *)arg;
	argv[2] = NULL;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, outfile,
					 O_WRONLY | O_CREAT | O_TRUNC, 0644);
	rc = posix_spawn(&pid, "/APPS/TRUNCHK", &fa, NULL, argv, NULL);
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0) return -rc;
	waitpid(pid, &st, 0);
	return st;
}

static void spawn_cases(void)
{
	if (seed(P_RED, LONGTEXT) == 0) {
		run_to_file("--silent", P_RED);
		expect("E spawn redirect, child writes ZERO bytes -> file EMPTY", P_RED, "");
	} else {
		ok(0, "E spawn redirect: could not seed " P_RED);
	}

	if (seed(P_RED, LONGTEXT) == 0) {
		run_to_file("--emit", P_RED);
		expect("F spawn redirect, child writes a short line -> ONLY that line",
		       P_RED, "OUT\n");
	} else {
		ok(0, "F spawn redirect: could not seed " P_RED);
	}
}

// ---------------------------------------------------------------------------
// G: ftruncate. Reported, not asserted against a policy, because the point is
// to record what the shipped libc does rather than to assume it. It is run on
// the FAT ESP deliberately: on the ext2 root the H case below shows that a
// plain O_WRONLY open already empties the file by itself, which would make a
// no-op ftruncate() look like a working one.
// ---------------------------------------------------------------------------
static void ftruncate_case(void)
{
	char got[512], b[768];
	if (seed(P_FAT, LONGTEXT) != 0) { ok(0, "G ftruncate: could not seed"); return; }
	int fd = open(P_FAT, O_WRONLY, 0);
	if (fd < 0) { ok(0, "G ftruncate: open failed"); return; }
	int rc = ftruncate(fd, 0);
	close(fd);
	int n = slurp(P_FAT, got, sizeof got);
	snprintf(b, sizeof b, "[TRUNCHK] INFO ftruncate(fd,0) returned %d; file is now %d bytes\n", rc, n);
	line(b);
	ok(rc == 0 && n == 0, "G ftruncate(fd,0) reports success AND empties the file");
}

// ---------------------------------------------------------------------------
// H-I: O_WRONLY WITHOUT O_TRUNC. POSIX says this does not shorten anything: a
// write of 3 bytes into a 33-byte file replaces the first 3 and leaves the
// other 30, and closing without writing at all leaves the file exactly as it
// was. This is the same question as O_TRUNC ("when does this kernel decide a
// file is now shorter") asked from the other side, so it belongs in the same
// pass rather than in a follow-up nobody files.
// ---------------------------------------------------------------------------
static void nontrunc_cases(const char *fsname, const char *path)
{
	char what[160];

	// H: open for writing, write nothing, close. The file must be untouched.
	if (seed(path, LONGTEXT) == 0) {
		int fd = open(path, O_WRONLY, 0);
		if (fd >= 0) close(fd);
		snprintf(what, sizeof what, "%s H O_WRONLY with NO write leaves the file untouched", fsname);
		expect(what, path, LONGTEXT);
	}

	// I: overwrite the head in place. The tail must survive.
	if (seed(path, LONGTEXT) == 0) {
		int fd = open(path, O_WRONLY, 0);
		if (fd >= 0) { write(fd, NEWTEXT, strlen(NEWTEXT)); close(fd); }
		char want[128];
		snprintf(want, sizeof want, "%s%s", NEWTEXT, LONGTEXT + strlen(NEWTEXT));
		snprintf(what, sizeof what, "%s I O_WRONLY partial overwrite keeps the tail", fsname);
		expect(what, path, want);
	}

	// K: THE busybox vi SAVE, byte for byte. vi opens without O_TRUNC on
	// purpose ("might reduce amount of data lost on power fail"), writes the
	// new text over the old, and calls ftruncate() to cut whatever is left.
	// Saving a file that got SHORTER is therefore correct only if BOTH halves
	// are correct, which is why H, I and G cannot be fixed independently.
	if (seed(path, LONGTEXT) == 0) {
		int fd = open(path, O_WRONLY | O_CREAT, 0644);
		if (fd >= 0) {
			int n = (int)write(fd, NEWTEXT, strlen(NEWTEXT));
			ftruncate(fd, n);
			close(fd);
		}
		snprintf(what, sizeof what, "%s K the vi save shape (write then ftruncate) shortens the file", fsname);
		expect(what, path, NEWTEXT);
	}
}

// ---------------------------------------------------------------------------
// J: the LITERAL local-103 repro, with the real shipped /APPS/SED.
//
// Case E above uses this app as its own silent child and passes, so the
// difference that matters has to be reproduced exactly rather than modelled:
// the stale bytes local 103 read back were written by a PREVIOUS CHILD through
// its own redirect fd, not by the parent, and the sequence is
// producing-run then refusing-run onto the SAME path with no unlink between.
// ---------------------------------------------------------------------------
#define P_SEDIN  "/T109SIN.TXT"
#define P_SEDOUT "/T109SOU.TXT"

static int run_sed(char *const argv[], const char *outfile)
{
	posix_spawn_file_actions_t fa;
	pid_t pid = 0;
	int st = 0, rc;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, outfile,
					 O_WRONLY | O_CREAT | O_TRUNC, 0644);
	rc = posix_spawn(&pid, "/APPS/SED", &fa, NULL, argv, NULL);
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0) return -rc;
	waitpid(pid, &st, 0);
	return st;
}

static void sed_repro(void)
{
	char got[1024], b[1400];

	if (seed(P_SEDIN, "foo bar\nalpha 12 beta\nabab\na+b\nSTART\nmiddle\nEND\nxyz\n") != 0) {
		ok(0, "J sed repro: could not seed the corpus");
		return;
	}
	unlink(P_SEDOUT);

	// J1: a run that PRODUCES output, exactly as the case before the refusals
	// did. Its content is what must not survive the next run.
	{ char *a[] = { (char *)"sed", (char *)"s/xyz/ZZZ/", (char *)P_SEDIN, 0 };
	  int st = run_sed(a, P_SEDOUT);
	  int n = slurp(P_SEDOUT, got, sizeof got);
	  snprintf(b, sizeof b, "[TRUNCHK] INFO J1 producing sed run: status=%d, %d bytes on %s\n",
		   st, n, P_SEDOUT);
	  line(b);
	  ok(n > 0, "J1 the producing sed run left output to be clobbered");
	}

	// J2: a run that REFUSES and writes nothing, onto the same path, with no
	// unlink. This is the exact assertion local 103 saw fail.
	{ char *a[] = { (char *)"sed", (char *)"h", (char *)P_SEDIN, 0 };
	  int st = run_sed(a, P_SEDOUT);
	  int n = slurp(P_SEDOUT, got, sizeof got);
	  snprintf(b, sizeof b, "[TRUNCHK] INFO J2 refusing sed run: status=%d, %d bytes on %s: '%s'\n",
		   st, n, P_SEDOUT, got);
	  line(b);
	  ok(st != 0, "J2 the refusing sed run exited non-zero");
	  ok(n == 0, "J2 a refusing child leaves the redirect target EMPTY");
	}
	unlink(P_SEDIN);
	unlink(P_SEDOUT);
}

// ---------------------------------------------------------------------------
// L: `> file` AT THE SHELL, which is the sentence the ticket is named after.
// msh parses the redirection into cmd.outfile and then, when the command has no
// words, threw the whole parse away. Driven through `msh -c`, which is the only
// way to hand this shell a line (its line editor reads a tty; a script file is
// not a way in, see msh's own main()).
// ---------------------------------------------------------------------------
#define P_MSH "/T109M.TXT"
#define P_MSH2 "/T109M2.TXT"

static int run_msh(const char *line_in)
{
	posix_spawn_file_actions_t fa;
	pid_t pid = 0;
	int st = 0, rc;
	char *argv[4];
	argv[0] = (char *)"msh";
	argv[1] = (char *)"-c";
	argv[2] = (char *)line_in;
	argv[3] = NULL;
	posix_spawn_file_actions_init(&fa);
	rc = posix_spawn(&pid, "/APPS/MSH", &fa, NULL, argv, NULL);
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0) return -rc;
	waitpid(pid, &st, 0);
	return st;
}

static void msh_cases(void)
{
	char got[512], b[768];

	// L1: the headline. `> file` must empty a file that has contents.
	if (seed(P_MSH, LONGTEXT) == 0) {
		int st = run_msh("> " P_MSH);
		int n = slurp(P_MSH, got, sizeof got);
		snprintf(b, sizeof b, "[TRUNCHK] INFO L1 msh -c '> file': status=%d, %d bytes left\n", st, n);
		line(b);
		expect("L1 msh `> file` empties an existing file", P_MSH, "");
	} else {
		ok(0, "L1 msh: could not seed " P_MSH);
	}

	// L2: `> file` on a path that does not exist must CREATE it empty, which
	// is the other half of what the idiom is used for.
	unlink(P_MSH2);
	run_msh("> " P_MSH2);
	expect("L2 msh `> file` creates a missing file, empty", P_MSH2, "");

	// L3: `>> file` must NOT truncate. Same code path, opposite flag, and the
	// case that would catch a fix that emptied everything it touched.
	if (seed(P_MSH, "AAA") == 0) {
		run_msh(">> " P_MSH);
		expect("L3 msh `>> file` leaves an existing file alone", P_MSH, "AAA");
	}

	// L4: a real command with a redirection still works, so L1 did not steal
	// the ordinary path on its way past.
	if (seed(P_MSH, LONGTEXT) == 0) {
		run_msh("echo hi > " P_MSH);
		expect("L4 msh `echo hi > file` still writes only the new content", P_MSH, "hi\n");
	}
	unlink(P_MSH);
	unlink(P_MSH2);
}

int main(int argc, char **argv)
{
	char b[256];

	// Child modes. These are the fixtures for E and F, so they must produce
	// EXACTLY what their names say and nothing else on fd 1.
	if (argc >= 2 && strcmp(argv[1], "--silent") == 0) {
		// Deliberately writes NOTHING to stdout, then exits non-zero, which
		// is what a tool that correctly refuses its input does.
		return 1;
	}
	if (argc >= 2 && strcmp(argv[1], "--emit") == 0) {
		write(1, "OUT\n", 4);
		return 0;
	}

	line("[TRUNCHK] start: O_TRUNC behaviour on ext2 root and FAT ESP (#745 local 109)\n");

	fs_cases("ext2", P_EXT2);
	fs_cases("FAT ", P_FAT);
	nontrunc_cases("ext2", P_EXT2);
	nontrunc_cases("FAT ", P_FAT);
	spawn_cases();
	ftruncate_case();
	sed_repro();
	msh_cases();

	unlink(P_EXT2);
	unlink(P_FAT);
	unlink(P_RED);

	snprintf(b, sizeof b, "[TRUNCHK] DONE pass=%d fail=%d\n", g_pass, g_fail);
	line(b);
	return g_fail ? 1 : 0;
}
