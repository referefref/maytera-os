// regexchk - proof that the mports-built musl regex (userland/ports/musl-regex)
// works on a running MayteraOS, not just that it compiled.
//
// WHY THIS APP EXISTS. "A library that compiles but was never linked and run is
// not evidence." This is the evidence. It links the static libregex.a that
// userland/ports/mports.sh produced from the sha256-pinned musl 1.2.5 tarball,
// and it runs on a booted machine.
//
// WHAT IT ASSERTS, AND WHERE THE EXPECTATIONS CAME FROM. Every expected string
// below is the output of the HOST's own GNU grep, recorded by
// tools/pcre2-oracle/divergence.py, which asks `grep -oG` / `grep -oE` for the
// leftmost match of the same pattern on the same subject. Nothing here is a
// hand-reasoned expectation about somebody else's specification: that shape is
// what made a correct getopt look broken (blame.md, local 72).
//
// It exercises the path GNU grep actually takes, which is the GNU API
// (re_set_syntax + re_compile_pattern + re_search) rather than bare regcomp,
// because that adapter is the part of this port that is OURS and therefore the
// part most likely to be wrong. The dialects are the ones grep selects:
// RE_SYNTAX_GREP for -G and RE_SYNTAX_POSIX_EGREP for -E.
//
// THE TWO KNOWN DIVERGENCES ARE ASSERTED AS DIVERGENCES, not hidden. `a{,2}`
// and `\d` behave differently here than under GNU regex; the cases at the
// bottom pin the NEW behaviour so that a future change to it is a test failure
// rather than a surprise.
//
// OUTPUT DISCIPLINE. Launched via /CONFIG/AUTORUN.CFG containing
// "/APPS/REGEXCHK". An autorun-launched process emits ONE SERIAL RECORD PER
// write(), so every line here is formatted into a buffer and issued as exactly
// one write(2, ...).

#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "fcntl.h"
#include "spawn.h"
#include "sha256.h"
#include "sys/wait.h"

#include <regex.h>
#include <gnuregex.h>

static int g_pass = 0, g_fail = 0;

static void line(const char *s) { write(2, s, strlen(s)); }

static void ok(int cond, const char *what)
{
	char b[256];
	if (cond) g_pass++; else g_fail++;
	snprintf(b, sizeof b, "[REGEXCHK] %s %s\n", cond ? "PASS" : "FAIL", what);
	line(b);
}

// ---------------------------------------------------------------------------
// The grep question: leftmost match of PAT in SUBJ, as text.
// Driven through the GNU API exactly as grep's search.c drives it.
// ---------------------------------------------------------------------------
static const char *leftmost(int ere, const char *pat, const char *subj,
			    char *out, int outsz)
{
	struct re_pattern_buffer buf;
	struct re_registers regs;
	const char *err;
	int size = (int)strlen(subj);
	int start;

	memset(&buf, 0, sizeof buf);
	memset(&regs, 0, sizeof regs);
	re_set_syntax(ere ? RE_SYNTAX_POSIX_EGREP : RE_SYNTAX_GREP);
	err = re_compile_pattern(pat, strlen(pat), &buf);
	if (err) {
		snprintf(out, outsz, "ERROR:%s", err);
		return out;
	}
	start = re_search(&buf, subj, size, 0, size, &regs);
	if (start < 0 || regs.end[0] == regs.start[0]) {
		snprintf(out, outsz, "NOMATCH");
		regfree((regex_t *)&buf);
		return out;
	}
	int n = (int)(regs.end[0] - regs.start[0]);
	if (n > outsz - 1) n = outsz - 1;
	memcpy(out, subj + start, (size_t)n);
	out[n] = '\0';
	regfree((regex_t *)&buf);
	return out;
}

static void grepcase(int ere, const char *pat, const char *subj,
		     const char *want)
{
	char got[128], b[512];
	leftmost(ere, pat, subj, got, sizeof got);
	int good = strcmp(got, want) == 0;
	if (good) g_pass++; else g_fail++;
	snprintf(b, sizeof b, "[REGEXCHK] %s %-2s %-24s on %-14s -> '%s'%s\n",
		 good ? "PASS" : "FAIL", ere ? "-E" : "-G", pat, subj, got,
		 good ? "" : " WANTED-DIFFERENT");
	line(b);
	if (!good) {
		snprintf(b, sizeof b, "[REGEXCHK]      wanted '%s'\n", want);
		line(b);
	}
}


// ===========================================================================
// PHASE 2: the SHIPPED BINARIES, not the library.
//
// Everything above proves libregex.a. This part proves the two programs the
// licence retirement was actually about, by SPAWNING /APPS/GREP and /APPS/VI
// with real patterns and reading back what they produced. A library that
// links is not a grep that works, and this project's recurring defect is a
// claim about a description rather than about the artefact.
//
// Each spawned binary's sha256 is printed first, so the run can be tied to
// the exact file on the image rather than to "the build I think shipped".
// ===========================================================================

static void print_sha256(const char *path)
{
	int fd = open(path, O_RDONLY, 0);
	char b[256];
	if (fd < 0) {
		snprintf(b, sizeof b, "[REGEXCHK] SHA256 %s: CANNOT OPEN\n", path);
		line(b);
		return;
	}
	sha256_ctx_t c;
	sha256_init(&c);
	static char buf[4096];
	int n, total = 0;
	while ((n = read(fd, buf, sizeof buf)) > 0) {
		sha256_update(&c, buf, (size_t)n);
		total += n;
	}
	close(fd);
	uint8_t d[32];
	sha256_final(&c, d);
	char hex[65];
	for (int i = 0; i < 32; i++)
		snprintf(hex + i * 2, 3, "%02x", d[i]);
	snprintf(b, sizeof b, "[REGEXCHK] SHA256 %s = %s (%d bytes)\n", path, hex, total);
	line(b);
}

static int write_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return -1;
	int n = (int)write(fd, text, strlen(text));
	close(fd);
	return n == (int)strlen(text) ? 0 : -1;
}

// Read a file and return it NUL-terminated in OUT. Returns bytes read, or -1.
static int read_file(const char *path, char *out, int outsz)
{
	int fd = open(path, O_RDONLY, 0);
	if (fd < 0)
		return -1;
	int n = (int)read(fd, out, (size_t)outsz - 1);
	close(fd);
	if (n < 0)
		n = 0;
	out[n] = '\0';
	return n;
}

// Spawn PATH with ARGV, stdout redirected to OUTFILE, and wait for it.
static int run_to_file(const char *path, char *const argv[], const char *outfile)
{
	posix_spawn_file_actions_t fa;
	pid_t pid = 0;
	int st = 0, rc;

	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, outfile,
					 O_WRONLY | O_CREAT | O_TRUNC, 0644);
	rc = posix_spawn(&pid, path, &fa, NULL, argv, NULL);
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0)
		return -rc;
	waitpid(pid, &st, 0);
	return st;
}

// Trim a trailing newline so the comparison is about the match, not the EOL.
static void chomp(char *s)
{
	int n = (int)strlen(s);
	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
		s[--n] = '\0';
}

static void grep_case(const char *what, char *const argv[], const char *want)
{
	char got[512], b[768];
	int st = run_to_file("/APPS/GREP", argv, "/REGEXOUT.TXT");
	if (st < 0) {
		snprintf(b, sizeof b, "[REGEXCHK] FAIL grep %s: spawn failed (%d)\n", what, -st);
		line(b);
		g_fail++;
		return;
	}
	if (read_file("/REGEXOUT.TXT", got, sizeof got) < 0) {
		snprintf(b, sizeof b, "[REGEXCHK] FAIL grep %s: no output file\n", what);
		line(b);
		g_fail++;
		return;
	}
	chomp(got);
	int good = strcmp(got, want) == 0;
	if (good) g_pass++; else g_fail++;
	snprintf(b, sizeof b, "[REGEXCHK] %s /APPS/GREP %-26s -> '%s'%s\n",
		 good ? "PASS" : "FAIL", what, got,
		 good ? "" : " (wanted a different answer)");
	line(b);
	if (!good) {
		snprintf(b, sizeof b, "[REGEXCHK]      wanted '%s'\n", want);
		line(b);
	}
}

static void phase2(void)
{
	char b[512];

	line("[REGEXCHK] ==== phase 2: the shipped /APPS/GREP and /APPS/VI ====\n");
	print_sha256("/APPS/GREP");
	print_sha256("/APPS/VI");

	// One subject file, one line per case, so every invocation reads the
	// same bytes and a difference can only come from the pattern.
	if (write_file("/REGEXIN.TXT",
		       "abcdz\n"
		       "ab\n"
		       "a+b\n"
		       "abab\n"
		       "ab123cd\n") != 0) {
		line("[REGEXCHK] FAIL could not write /REGEXIN.TXT\n");
		g_fail++;
		return;
	}

	// The five shapes the ticket names, run through the REAL binary.
	{
		char *av1[] = { "grep", "-E", "^ab.*z$", "/REGEXIN.TXT", 0 };
		grep_case("-E anchored", av1, "abcdz");

		// Leftmost-LONGEST: POSIX must print ab where PCRE would print a.
		// The six expected lines are the host grep -oE output measured on
		// this exact file: line 3 is a+b, whose leftmost match is a bare a.
		// It is the case PCRE2 could not have satisfied at all.

		char *av2[] = { "grep", "-oE", "a|ab", "/REGEXIN.TXT", 0 };
		grep_case("-oE alternation longest", av2, "ab\nab\na\nab\nab\nab");

		// BRE: '+' is a LITERAL, so this matches the line "a+b".
		char *av3[] = { "grep", "-G", "a+b", "/REGEXIN.TXT", 0 };
		grep_case("-G literal plus", av3, "a+b");

		char *av4[] = { "grep", "-oE", "(ab)\\1", "/REGEXIN.TXT", 0 };
		grep_case("-oE backreference", av4, "abab");

		char *av5[] = { "grep", "-oE", "[[:digit:]]+", "/REGEXIN.TXT", 0 };
		grep_case("-oE character class", av5, "123");
	}

	// ---- vi -------------------------------------------------------------
	// busybox vi's :substitute is the ONLY caller of REG_STARTEND in this
	// OS, and it goes through POSIX regcomp/regexec rather than the GNU
	// adapter, so it exercises the other half of the port. -c runs a colon
	// command at startup (ENABLE_FEATURE_VI_COLON=1), which is how a
	// non-interactive check of an interactive editor is possible at all.
	if (write_file("/VITEST.TXT", "one abab two\nthree 123 four\n") == 0) {
		char *av[] = { "vi", "-c", ":%s/\\(ab\\)\\1/MATCHED/",
			       "-c", ":wq", "/VITEST.TXT", 0 };
		int st = run_to_file("/APPS/VI", av, "/VIOUT.TXT");
		char got[512];
		read_file("/VITEST.TXT", got, sizeof got);
		snprintf(b, sizeof b, "[REGEXCHK]      vi exit=%d, file now: '%s'\n", st, got);
		line(b);
		int good = strstr(got, "MATCHED") != NULL;
		if (good) g_pass++; else g_fail++;
		snprintf(b, sizeof b, "[REGEXCHK] %s /APPS/VI :s with a BRE back reference rewrote the file\n",
			 good ? "PASS" : "FAIL");
		line(b);
	} else {
		line("[REGEXCHK] FAIL could not write /VITEST.TXT\n");
		g_fail++;
	}
}


// ===========================================================================
// PHASE 3: /APPS/SED (#745 local 103).
//
// The third consumer of this same libregex.a. Until this ticket, sed was 126
// lines whose matcher was memcmp() and whose own header said "FIND is a
// literal string (no regex)": `sed 's/^foo.*bar$/x/'` did not error, it copied
// the input through unchanged and exited 0. So every case below is chosen to
// be one the OLD sed would have got wrong, not merely one the new one gets
// right: an anchored substitution, a character class, a back reference in the
// pattern, a capture group used in the REPLACEMENT, a two-regex address range,
// and the same pattern under BRE and under -E.
//
// EVERY EXPECTED STRING IS HOST `sed` OUTPUT, printed by
// tools/sed-oracle/sed-oracle.sh --table against this exact corpus and pasted
// in. None of it is reasoned from the specification: that shape is what made a
// correct getopt look broken (local 72) and cost a VM boot on grep (local 97).
//
// The last three cases assert the REFUSALS. A tool that implements a fraction
// of its specification and reports no error is the defect this ticket is
// about, so `{ }`, the hold space and `a` must exit non-zero and print
// nothing, rather than silently doing something else.
// ===========================================================================

// The oracle corpus. One definition; sed_case() rewrites it before each run so
// a `d` or `q` case cannot leak into the next one.
#define SED_CORPUS \
	"foo bar\n" \
	"alpha 12 beta\n" \
	"abab\n" \
	"a+b\n" \
	"START\n" \
	"middle\n" \
	"END\n" \
	"xyz\n"

static void sed_case(const char *what, char *const argv[], const char *want)
{
	char got[1024], b[2048];

	if (write_file("/SEDIN.TXT", SED_CORPUS) != 0) {
		snprintf(b, sizeof b, "[REGEXCHK] FAIL sed %s: could not write /SEDIN.TXT\n", what);
		line(b);
		g_fail++;
		return;
	}
	int st = run_to_file("/APPS/SED", argv, "/SEDOUT.TXT");
	if (st < 0) {
		snprintf(b, sizeof b, "[REGEXCHK] FAIL sed %s: spawn failed (%d)\n", what, -st);
		line(b);
		g_fail++;
		return;
	}
	if (read_file("/SEDOUT.TXT", got, sizeof got) < 0) {
		snprintf(b, sizeof b, "[REGEXCHK] FAIL sed %s: no output file\n", what);
		line(b);
		g_fail++;
		return;
	}
	chomp(got);
	int good = strcmp(got, want) == 0;
	if (good) g_pass++; else g_fail++;
	snprintf(b, sizeof b, "[REGEXCHK] %s /APPS/SED %-26s\n", good ? "PASS" : "FAIL", what);
	line(b);
	if (!good) {
		snprintf(b, sizeof b, "[REGEXCHK]      got    '%s'\n", got);
		line(b);
		snprintf(b, sizeof b, "[REGEXCHK]      wanted '%s'\n", want);
		line(b);
	}
}

// A refusal must be LOUD: non-zero status and no stdout. "Exited 0 having done
// nothing" and "did the right thing" are the two answers this must separate.
//
// THE UNLINK IS LOAD BEARING, and it cost a VM boot to learn. run_to_file()
// opens the output with O_WRONLY|O_CREAT|O_TRUNC, and on this kernel a file
// opened O_TRUNC that the child then NEVER WRITES TO keeps its previous
// contents on disk. Every refusal case therefore read back the PREVIOUS case's
// output and the "no stdout" half of the assertion failed on four correct
// refusals. It is specific to a zero-byte producer: the success cases below
// include short outputs following long ones (a two-byte "7" after a 52-byte
// line) and they compare exactly, so truncation does land as soon as anything
// is written. Removing the file first makes the O_CREAT produce a genuinely
// empty one, so "no output" means no output.
static void sed_refuses(const char *what, char *const argv[])
{
	char got[512], b[1024];

	unlink("/SEDOUT.TXT");
	if (write_file("/SEDIN.TXT", SED_CORPUS) != 0) {
		snprintf(b, sizeof b, "[REGEXCHK] FAIL sed %s: could not write /SEDIN.TXT\n", what);
		line(b);
		g_fail++;
		return;
	}
	int st = run_to_file("/APPS/SED", argv, "/SEDOUT.TXT");
	if (read_file("/SEDOUT.TXT", got, sizeof got) < 0)
		got[0] = '\0';
	chomp(got);
	int good = (st != 0) && got[0] == '\0';
	if (good) g_pass++; else g_fail++;
	snprintf(b, sizeof b, "[REGEXCHK] %s /APPS/SED refuses %-18s (status=%d, stdout='%s')\n",
		 good ? "PASS" : "FAIL", what, st, got);
	line(b);
}

static void phase3(void)
{
	line("[REGEXCHK] ==== phase 3: the shipped /APPS/SED ====\n");
	print_sha256("/APPS/SED");

	// --- the five shapes the fake would have passed through unchanged -------
	{ char *a[] = { "sed", "-E", "s/^a.*a$/X/", "/SEDIN.TXT", 0 };
	  sed_case("-E anchored s", a,
	           "foo bar\nX\nabab\na+b\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "s/[[:digit:]][[:digit:]]*/N/", "/SEDIN.TXT", 0 };
	  sed_case("character class", a,
	           "foo bar\nalpha N beta\nabab\na+b\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "s/\\(ab\\)\\1/DOUBLE/", "/SEDIN.TXT", 0 };
	  sed_case("BRE backref in pattern", a,
	           "foo bar\nalpha 12 beta\nDOUBLE\na+b\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "-E", "s/([a-z]+) ([a-z]+)/\\2-\\1/", "/SEDIN.TXT", 0 };
	  sed_case("group backref in REPL", a,
	           "bar-foo\nalpha 12 beta\nabab\na+b\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "-n", "/START/,/END/p", "/SEDIN.TXT", 0 };
	  sed_case("two-regex address range", a, "START\nmiddle\nEND"); }

	// --- BRE and ERE are DIFFERENT LANGUAGES on the same pattern ------------
	// In BRE `+` is a literal, so this matches the line "a+b"; under -E it is
	// a quantifier, so it matches the "ab" inside "abab".
	{ char *a[] = { "sed", "s/a+b/PLUS/", "/SEDIN.TXT", 0 };
	  sed_case("BRE: + is literal", a,
	           "foo bar\nalpha 12 beta\nabab\nPLUS\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "-E", "s/a+b/PLUS/", "/SEDIN.TXT", 0 };
	  sed_case("ERE: + is a quantifier", a,
	           "foo bar\nalpha 12 beta\nPLUSab\na+b\nSTART\nmiddle\nEND\nxyz"); }

	// --- the rest of the command set ----------------------------------------
	{ char *a[] = { "sed", "/foo/d", "/SEDIN.TXT", 0 };
	  sed_case("regex address d", a,
	           "alpha 12 beta\nabab\na+b\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "-n", "$p", "/SEDIN.TXT", 0 };
	  sed_case("$ last line", a, "xyz"); }

	{ char *a[] = { "sed", "-n", "2p", "/SEDIN.TXT", 0 };
	  sed_case("line-number address", a, "alpha 12 beta"); }

	{ char *a[] = { "sed", "s/a/A/g", "/SEDIN.TXT", 0 };
	  sed_case("s///g", a,
	           "foo bAr\nAlphA 12 betA\nAbAb\nA+b\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "s/a/A/2", "/SEDIN.TXT", 0 };
	  sed_case("s///2 nth occurrence", a,
	           "foo bar\nalphA 12 beta\nabAb\na+b\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "s/a/A/2g", "/SEDIN.TXT", 0 };
	  sed_case("s///2g", a,
	           "foo bar\nalphA 12 betA\nabAb\na+b\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "y/abc/ABC/", "/SEDIN.TXT", 0 };
	  sed_case("y transliterate", a,
	           "foo BAr\nAlphA 12 BetA\nABAB\nA+B\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "-n", "/END/=", "/SEDIN.TXT", 0 };
	  sed_case("= line number of match", a, "7"); }

	{ char *a[] = { "sed", "-n", "2,3!p", "/SEDIN.TXT", 0 };
	  sed_case("! negated range", a,
	           "foo bar\na+b\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "s/xyz/[&]/", "/SEDIN.TXT", 0 };
	  sed_case("& is the whole match", a,
	           "foo bar\nalpha 12 beta\nabab\na+b\nSTART\nmiddle\nEND\n[xyz]"); }

	{ char *a[] = { "sed", "-E", "s/(a)(b)/\\2\\1/g", "/SEDIN.TXT", 0 };
	  sed_case("ERE group swap, global", a,
	           "foo bar\nalpha 12 beta\nbaba\na+b\nSTART\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "2q", "/SEDIN.TXT", 0 };
	  sed_case("q quits early", a, "foo bar\nalpha 12 beta"); }

	{ char *a[] = { "sed", "s/START/lower/I", "/SEDIN.TXT", 0 };
	  sed_case("s///I case-insensitive", a,
	           "foo bar\nalpha 12 beta\nabab\na+b\nlower\nmiddle\nEND\nxyz"); }

	{ char *a[] = { "sed", "-n", "/abab/s//SEEN/p", "/SEDIN.TXT", 0 };
	  sed_case("s// reuses the last regex", a, "SEEN"); }

	{ char *a[] = { "sed", "s,a+b,COMMA,", "/SEDIN.TXT", 0 };
	  sed_case("arbitrary s delimiter", a,
	           "foo bar\nalpha 12 beta\nabab\nCOMMA\nSTART\nmiddle\nEND\nxyz"); }

	// --- and the loud refusals ----------------------------------------------
	{ char *a[] = { "sed", "-n", "/foo/{p}", "/SEDIN.TXT", 0 };
	  sed_refuses("{ } blocks", a); }

	{ char *a[] = { "sed", "h", "/SEDIN.TXT", 0 };
	  sed_refuses("hold space", a); }

	{ char *a[] = { "sed", "-i", "s/a/b/", "/SEDIN.TXT", 0 };
	  sed_refuses("-i in place", a); }

	{ char *a[] = { "sed", "s/[/x/", "/SEDIN.TXT", 0 };
	  sed_refuses("a bad regex", a); }
}

int main(void)
{
	char b[512];

	line("[REGEXCHK] ==== musl-regex 1.2.5 (userland/ports/musl-regex) ====\n");
	line("[REGEXCHK] expectations are host `grep -oG/-oE` output, recorded by\n");
	line("[REGEXCHK] tools/pcre2-oracle/divergence.py\n");

	// --- the family PCRE2 could not do: leftmost-LONGEST -------------------
	grepcase(1, "a|ab",        "ab",     "ab");
	grepcase(1, "foo|foobar",  "foobar", "foobar");
	grepcase(1, "x(a|ab)y?",   "xab",    "xab");

	// --- the family PCRE2 could not do: BRE is a different language --------
	grepcase(0, "a+b",         "a+b",    "a+b");
	grepcase(0, "ab?c",        "ab?c",   "ab?c");
	grepcase(0, "a|b",         "a|b",    "a|b");
	grepcase(0, "(x)",         "(x)",    "(x)");
	grepcase(0, "a{2}",        "a{2}",   "a{2}");
	grepcase(0, "\\(ab\\)*c",  "ababc",  "ababc");
	grepcase(0, "a\\{2,3\\}",  "aaaa",   "aaa");
	grepcase(0, "a\\+b",       "aaab",   "aaab");
	grepcase(0, "cat\\|dog",   "a dog",  "dog");

	// --- the shared middle, which must not have moved ----------------------
	grepcase(1, "[[:digit:]]+", "ab123cd", "123");
	grepcase(1, "^ab.*z$",     "abcdz",  "abcdz");
	grepcase(1, "[^a]+",       "aXYZ",   "XYZ");
	grepcase(1, "<.*>",        "<a><b>", "<a><b>");
	grepcase(1, "(ab)*c",      "ababc",  "ababc");
	grepcase(1, "a{2,3}",      "aaaa",   "aaa");

	// --- back references, in BOTH dialects (patch 0005 is why -E works) ----
	grepcase(1, "(ab)\\1",     "abab",   "abab");
	grepcase(0, "\\(ab\\)\\1", "abab",   "abab");
	grepcase(1, "(a(b))\\2",   "abb",    "abb");

	// --- GNU word operators, which busybox vi and grep users both type -----
	grepcase(0, "\\<cat",      "a cat",  "cat");
	grepcase(0, "cat\\>",      "cat!",   "cat");
	grepcase(1, "\\bcat\\b",   "the cat", "cat");

	// --- re_search: the START WINDOW and the BACKWARD direction ------------
	// busybox vi's char_search() is the only caller of the negative-range
	// form in this OS, and it is the form a POSIX-only engine cannot express
	// at all, so it gets its own case rather than being assumed.
	{
		struct re_pattern_buffer buf;
		struct re_registers regs;
		const char *subj = "cat dog cat dog cat";
		int size = (int)strlen(subj);
		memset(&buf, 0, sizeof buf);
		memset(&regs, 0, sizeof regs);
		re_set_syntax(RE_SYNTAX_GREP);
		ok(re_compile_pattern("cat", 3, &buf) == NULL, "re_compile_pattern('cat')");

		int fwd = re_search(&buf, subj, size, 0, size, &regs);
		snprintf(b, sizeof b, "[REGEXCHK]      forward from 0 -> %d (want 0)\n", fwd);
		line(b);
		ok(fwd == 0, "re_search forward finds the FIRST cat");

		int mid = re_search(&buf, subj, size, 1, size - 1, &regs);
		snprintf(b, sizeof b, "[REGEXCHK]      forward from 1 -> %d (want 8)\n", mid);
		line(b);
		ok(mid == 8, "re_search honours startpos");

		int back = re_search(&buf, subj, size, size, -size, &regs);
		snprintf(b, sizeof b, "[REGEXCHK]      backward from end -> %d (want 16)\n", back);
		line(b);
		ok(back == 16, "re_search backward finds the LAST cat");

		int none = re_search(&buf, subj, size, 0, 2, &regs);
		ok(none == 0, "re_search with a tiny forward range still finds a match at 0");

		int nope = re_search(&buf, subj, size, 1, 2, &regs);
		snprintf(b, sizeof b, "[REGEXCHK]      range-limited from 1 -> %d (want -1)\n", nope);
		line(b);
		ok(nope == -1, "re_search refuses a match starting outside the range");

		// re_match is ANCHORED: it must succeed at 0 and fail at 1.
		ok(re_match(&buf, subj, size, 0, &regs) == 3, "re_match at 0 -> length 3");
		ok(re_match(&buf, subj, size, 1, &regs) == -1, "re_match at 1 -> no anchored match");
		regfree((regex_t *)&buf);
	}

	// --- REG_STARTEND, which busybox vi's :substitute requires -------------
	{
		regex_t re;
		regmatch_t m[2];
		const char *subj = "one\ntwo\nthree";
		ok(regcomp(&re, "^t.*", REG_NEWLINE) == 0, "regcomp('^t.*', REG_NEWLINE)");
		// Search ONLY the middle line, bytes 4..7, of a buffer that keeps
		// going. Without REG_STARTEND the match would run past "two".
		m[0].rm_so = 4;
		m[0].rm_eo = 7;
		int rc = regexec(&re, subj, 1, m, REG_STARTEND);
		snprintf(b, sizeof b, "[REGEXCHK]      REG_STARTEND rc=%d so=%d eo=%d (want 0 4 7)\n",
			 rc, (int)m[0].rm_so, (int)m[0].rm_eo);
		line(b);
		ok(rc == 0 && m[0].rm_so == 4 && m[0].rm_eo == 7,
		   "REG_STARTEND bounds the subject and returns absolute offsets");
		regfree(&re);
	}

	// --- the POSIX surface itself ------------------------------------------
	{
		regex_t re;
		regmatch_t m[3];
		ok(regcomp(&re, "(a+)(b+)", REG_EXTENDED) == 0, "regcomp ERE with groups");
		ok(regexec(&re, "xxaaabbyy", 3, m, 0) == 0, "regexec finds it");
		ok(m[0].rm_so == 2 && m[0].rm_eo == 7, "group 0 spans aaabb");
		ok(m[1].rm_so == 2 && m[1].rm_eo == 5, "group 1 is aaa");
		ok(m[2].rm_so == 5 && m[2].rm_eo == 7, "group 2 is bb");
		ok(re.re_nsub == 2, "re_nsub counts the groups");
		regfree(&re);

		int err = regcomp(&re, "a[", REG_EXTENDED);
		char msg[128];
		regerror(err, &re, msg, sizeof msg);
		snprintf(b, sizeof b, "[REGEXCHK]      regcomp('a[') -> %d '%s'\n", err, msg);
		line(b);
		ok(err == REG_EBRACK, "an unterminated bracket is REG_EBRACK");
		ok(msg[0] != '\0', "regerror produced a message");
	}

	// --- re_set_syntax is a real setter over a real global -----------------
	{
		reg_syntax_t prev = re_set_syntax(RE_SYNTAX_POSIX_EGREP);
		ok(re_syntax_options == RE_SYNTAX_POSIX_EGREP, "re_syntax_options is the live global");
		reg_syntax_t back = re_set_syntax(prev);
		ok(back == RE_SYNTAX_POSIX_EGREP, "re_set_syntax returns the PREVIOUS value");
	}

	// --- the two measured divergences from GNU regex, pinned as behaviour --
	// Neither is a bug to fix silently: they are what this engine does, and a
	// change to either should break this app rather than surprise a user.
	{
		char got[128];
		leftmost(1, "\\d+", "a123", got, sizeof got);
		snprintf(b, sizeof b, "[REGEXCHK] DIVERGENCE \\d+ on 'a123' -> '%s' "
			 "(GNU regex: NOMATCH, it has no \\d)\n", got);
		line(b);
		ok(strcmp(got, "123") == 0, "known divergence: \\d is a digit class here");

		leftmost(1, "a{,2}", "a{,2}", got, sizeof got);
		snprintf(b, sizeof b, "[REGEXCHK] DIVERGENCE a{,2} on 'a{,2}' -> '%s' "
			 "(GNU regex: 'a', it reads {,2} as {0,2})\n", got);
		line(b);
		ok(strncmp(got, "ERROR:", 6) == 0, "known divergence: {,n} is refused here");
	}

	phase2();
	phase3();

	snprintf(b, sizeof b, "[REGEXCHK] ==== done: %d passed, %d failed ====\n",
		 g_pass, g_fail);
	line(b);
	return g_fail ? 1 : 0;
}
