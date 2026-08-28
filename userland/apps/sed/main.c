// sed - the POSIX stream editor, with REAL regular expressions.
//
// WHAT THIS FILE REPLACED, AND WHY IT MATTERED (#745 local 103). The previous
// /APPS/SED was 126 lines whose entire matcher was memcmp(): its own header
// comment said "FIND is a literal string (no regex)". So
// `sed 's/^foo.*bar$/x/'` did not error. It searched for the LITERAL text
// ^foo.*bar$, found none, and copied the input to the output unchanged, exit 0.
// That is the same defect local 98 removed from /APPS/GREP, in its quieter and
// more damaging form: a grep that finds nothing is visible to a human at a
// prompt, while a sed that passes text through CORRUPTS THE OUTPUT of whatever
// was built on it, and the failure surfaces later, somewhere else, in a file.
//
// THE ENGINE IS NOT OURS AND MUST NEVER BE. It is userland/ports/musl-regex,
// musl 1.2.5's MIT (TRE-derived) POSIX regex, built by the mports driver and
// linked as libregex.a. It is leftmost-LONGEST POSIX BRE and ERE, which is what
// sed is specified in terms of. PCRE2 is in the tree and is the WRONG engine
// for this: it is leftmost-first and it has no BRE mode at all, and sed is BRE
// by default (blame.md, "PCRE2 CANNOT DROP INTO GNU grep"). /APPS/GREP and
// /APPS/VI already link this same archive; this is the third consumer and it
// adds no fourth copy of anything.
//
// THE OTHER HALF OF THE FIX IS THE ERRORS. A tool that implements a fraction of
// its specification and reports NO error is the defect class this ticket is
// about, so every construct that is not implemented here is NAMED and REFUSED
// with a non-zero exit: `sed -i`, `a`, `i`, `c`, `r`, `w`, the hold space, the
// branch commands and `{}` blocks all say so on stderr. The old code also read
// at most 256 KB of input and silently discarded the rest; this one streams and
// has no input size limit at all.
//
// IMPLEMENTED: -n, -e, -f, -E/-r, --; addresses (line number, $, /BRE/, \cREc,
// two-address ranges, and ! negation) for every command; s///
// with the g, p, i/I and Nth-occurrence flags, & and \1..\9 in the replacement,
// and an arbitrary delimiter; y///; p; d; q [exit-code]; =.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "errno.h"
#include "fcntl.h"

#include <regex.h>

// ---------------------------------------------------------------------------
// Output buffering. One write(2) per 4 KB rather than per byte; an
// autorun-launched process emits one serial record per write().
// ---------------------------------------------------------------------------
static char out_buf[4096];
static int  out_len = 0;

static void out_flush(void)
{
	int off = 0;
	while (off < out_len) {
		long w = write(1, out_buf + off, out_len - off);
		if (w <= 0) break;
		off += (int)w;
	}
	out_len = 0;
}

static void out_mem(const char *s, long n)
{
	for (long i = 0; i < n; i++) {
		if (out_len >= (int)sizeof(out_buf)) out_flush();
		out_buf[out_len++] = s[i];
	}
}

static void out_str(const char *s) { out_mem(s, (long)strlen(s)); }

static void err_str(const char *s) { write(2, s, strlen(s)); }

// Every refusal in this file goes through here, so an unimplemented construct
// can never be mistaken for a construct that did nothing.
//
// noreturn is not decoration: without it gcc reports every `case X: die(...)`
// in refuse_command() as a fallthrough, and a wall of warnings is how a real
// one gets missed.
__attribute__((noreturn))
static void die(const char *what, const char *detail)
{
	char b[512];
	if (detail && *detail)
		snprintf(b, sizeof b, "sed: %s: %s\n", what, detail);
	else
		snprintf(b, sizeof b, "sed: %s\n", what);
	out_flush();
	err_str(b);
	_exit(1);
}

__attribute__((noreturn))
static void die_re(int rc, const regex_t *re, const char *pat)
{
	char msg[128], b[512];
	regerror(rc, re, msg, sizeof msg);
	snprintf(b, sizeof b, "sed: -e expression: bad regular expression `%s': %s\n", pat, msg);
	out_flush();
	err_str(b);
	_exit(1);
}

// ---------------------------------------------------------------------------
// A growable byte buffer, used for the pattern space and for the substitution
// result. No fixed cap: the old sed's silent 256 KB truncation is exactly the
// "reports no error" shape this rewrite exists to remove.
// ---------------------------------------------------------------------------
typedef struct { char *p; long len, cap; } buf_t;

static void buf_need(buf_t *b, long extra)
{
	if (b->len + extra + 1 <= b->cap) return;
	long cap = b->cap ? b->cap : 256;
	while (cap < b->len + extra + 1) cap *= 2;
	char *np = (char *)realloc(b->p, (size_t)cap);
	if (!np) die("out of memory", 0);
	b->p = np;
	b->cap = cap;
}

static void buf_add(buf_t *b, const char *s, long n)
{
	if (n <= 0) return;
	buf_need(b, n);
	memcpy(b->p + b->len, s, (size_t)n);
	b->len += n;
	b->p[b->len] = '\0';
}

static void buf_addc(buf_t *b, char c) { buf_add(b, &c, 1); }
static void buf_clear(buf_t *b) { b->len = 0; if (b->p) b->p[0] = '\0'; }

// ---------------------------------------------------------------------------
// The script
// ---------------------------------------------------------------------------
#define MAX_CMDS 64

typedef enum { A_NONE = 0, A_LINE, A_LAST, A_RE } addrtype_t;

typedef struct {
	addrtype_t type;
	long       line;
	regex_t    re;
	int        has_re;   // 0 with type==A_RE means // : reuse the last regex
} addr_t;

typedef struct {
	addr_t a1, a2;
	int    naddr;
	int    negate;
	char   cmd;

	regex_t sre;         // s///
	int     s_has_re;
	char   *repl;
	int     s_global, s_nth, s_print;

	unsigned char ymap[256];   // y///
	int           y_valid;

	long q_code;         // q

	int  active;         // range state: currently between a1 and a2
} cmd_t;

static cmd_t g_cmds[MAX_CMDS];
static int   g_ncmds = 0;
static int   g_ere = 0;      // -E / -r
static int   g_quiet = 0;    // -n

// POSIX: an empty regex means "the last regular expression APPLIED", which is a
// runtime property, not a parse-time one.
static const regex_t *g_last_re = 0;

static int is_blank(char c) { return c == ' ' || c == '\t'; }

// Copy a delimited regex or replacement out of the script.
//
// Inside /RE/, `\<delim>` is a literal delimiter and every other backslash pair
// is handed to the engine untouched. Getting this wrong is how a delimiter
// other than '/' silently changes the pattern.
static char *scan_delimited(const char **pp, char delim, const char *what)
{
	const char *p = *pp;
	buf_t b = { 0, 0, 0 };
	while (*p && *p != delim) {
		if (*p == '\n') break;
		if (*p == '\\' && p[1]) {
			if (p[1] == delim) { buf_addc(&b, delim); p += 2; continue; }
			buf_addc(&b, '\\');
			buf_addc(&b, p[1]);
			p += 2;
			continue;
		}
		buf_addc(&b, *p++);
	}
	if (*p != delim) die("unterminated expression", what);
	p++;
	*pp = p;
	if (!b.p) { b.p = (char *)malloc(1); if (!b.p) die("out of memory", 0); b.p[0] = '\0'; }
	return b.p;
}

static void compile_re(regex_t *re, const char *pat, int icase)
{
	int flags = 0;
	if (g_ere)  flags |= REG_EXTENDED;
	if (icase)  flags |= REG_ICASE;
	int rc = regcomp(re, pat, flags);
	if (rc != 0) die_re(rc, re, pat);
}

// Returns 1 if an address was present.
static int parse_addr(const char **pp, addr_t *a)
{
	const char *p = *pp;
	while (is_blank(*p)) p++;

	if (*p >= '0' && *p <= '9') {
		char *end;
		a->type = A_LINE;
		a->line = strtol(p, &end, 10);
		p = end;
		*pp = p;
		return 1;
	}
	if (*p == '$') {
		a->type = A_LAST;
		*pp = p + 1;
		return 1;
	}
	if (*p == '/' || (*p == '\\' && p[1])) {
		char delim = '/';
		if (*p == '\\') { delim = p[1]; p += 2; } else p++;
		char *pat = scan_delimited(&p, delim, "address");
		int icase = 0;
		while (*p == 'I' || *p == 'M') { if (*p == 'I') icase = 1; p++; }
		a->type = A_RE;
		if (pat[0] == '\0') {
			a->has_re = 0;          // // : reuse the last regex applied
		} else {
			compile_re(&a->re, pat, icase);
			a->has_re = 1;
		}
		free(pat);
		*pp = p;
		return 1;
	}
	return 0;
}

static void parse_s(const char **pp, cmd_t *c)
{
	const char *p = *pp;
	char delim = *p;
	if (!delim || delim == '\n' || delim == '\\')
		die("unterminated `s' command", 0);
	p++;
	char *pat  = scan_delimited(&p, delim, "s command pattern");
	char *repl = scan_delimited(&p, delim, "s command replacement");

	c->s_global = 0;
	c->s_nth    = 1;
	c->s_print  = 0;
	int icase = 0, nth_seen = 0;
	for (;;) {
		if (*p == 'g') { c->s_global = 1; p++; continue; }
		if (*p == 'p') { c->s_print  = 1; p++; continue; }
		if (*p == 'i' || *p == 'I') { icase = 1; p++; continue; }
		if (*p == 'm' || *p == 'M') {
			die("`s' flag `m'/`M' (multiline) is not implemented", "remove it");
		}
		if (*p == 'w') {
			die("`s' flag `w' (write to file) is not implemented",
			    "redirect sed's stdout instead");
		}
		if (*p == 'e') {
			die("`s' flag `e' (execute) is not implemented", 0);
		}
		if (*p >= '0' && *p <= '9') {
			if (nth_seen) die("multiple number options to `s' command", 0);
			char *end;
			c->s_nth = (int)strtol(p, &end, 10);
			if (c->s_nth < 1) die("number option to `s' command may not be zero", 0);
			nth_seen = 1;
			p = end;
			continue;
		}
		break;
	}

	if (pat[0] == '\0') {
		c->s_has_re = 0;            // s//repl/ : reuse the last regex applied
		if (icase) die("`s' flag `I' needs a pattern", "s//.../I has no regex to modify");
	} else {
		compile_re(&c->sre, pat, icase);
		c->s_has_re = 1;
	}
	free(pat);
	c->repl = repl;
	*pp = p;
}

static void parse_y(const char **pp, cmd_t *c)
{
	const char *p = *pp;
	char delim = *p;
	if (!delim || delim == '\n' || delim == '\\')
		die("unterminated `y' command", 0);
	p++;
	char *from = scan_delimited(&p, delim, "y command source");
	char *to   = scan_delimited(&p, delim, "y command destination");

	// Undo the one-level escaping scan_delimited left in place: y takes
	// characters, not a regex, so \n means newline here and \\ means backslash.
	buf_t f = { 0, 0, 0 }, t = { 0, 0, 0 };
	for (const char *s = from; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			buf_addc(&f, *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s);
		} else buf_addc(&f, *s);
	}
	for (const char *s = to; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			buf_addc(&t, *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s);
		} else buf_addc(&t, *s);
	}
	if (f.len != t.len)
		die("strings for `y' command are different lengths", 0);

	for (int i = 0; i < 256; i++) c->ymap[i] = (unsigned char)i;
	for (long i = 0; i < f.len; i++)
		c->ymap[(unsigned char)f.p[i]] = (unsigned char)t.p[i];
	c->y_valid = 1;

	free(from); free(to);
	free(f.p);  free(t.p);
	*pp = p;
}

// The refusals. Every one of these is a real sed command that this
// implementation does not have; naming it is the whole point.
static void refuse_command(char c)
{
	char one[2] = { c, 0 };
	switch (c) {
	case 'a': case 'i': case 'c':
		die("the text commands `a', `i' and `c' are not implemented", one);
	case 'r': case 'R': case 'w': case 'W':
		die("the file commands `r' and `w' are not implemented", one);
	case 'h': case 'H': case 'g': case 'G': case 'x':
		die("the hold-space commands are not implemented", one);
	case 'b': case 't': case 'T': case ':':
		die("branching and labels are not implemented", one);
	case 'n': case 'N': case 'D': case 'P':
		die("the multi-line commands `n', `N', `D' and `P' are not implemented", one);
	case '{': case '}':
		die("command blocks `{ }' are not implemented",
		    "write one -e per command with the same address");
	case 'l':
		die("the `l' command is not implemented", one);
	case 'z': case 'F': case 'e': case 'v':
		die("GNU extension commands are not implemented", one);
	default:
		die("unknown command", one);
	}
}

static void parse_script(const char *script)
{
	const char *p = script;
	for (;;) {
		while (is_blank(*p) || *p == ';' || *p == '\n') p++;
		if (*p == '\0') break;
		if (*p == '#') { while (*p && *p != '\n') p++; continue; }

		if (g_ncmds >= MAX_CMDS) die("script has too many commands", "limit is 64");
		cmd_t *c = &g_cmds[g_ncmds];
		memset(c, 0, sizeof *c);

		if (parse_addr(&p, &c->a1)) {
			c->naddr = 1;
			while (is_blank(*p)) p++;
			if (*p == ',') {
				p++;
				if (!parse_addr(&p, &c->a2))
					die("expected an address after `,'", 0);
				c->naddr = 2;
			}
		}
		while (is_blank(*p)) p++;
		while (*p == '!') { c->negate = 1; p++; while (is_blank(*p)) p++; }

		char cmd = *p;
		if (cmd == '\0') die("missing command", 0);
		p++;
		switch (cmd) {
		case 's': c->cmd = 's'; parse_s(&p, c); break;
		case 'y': c->cmd = 'y'; parse_y(&p, c); break;
		case 'p': case 'd': case '=': c->cmd = cmd; break;
		case 'q':
			c->cmd = 'q';
			while (is_blank(*p)) p++;
			if (*p >= '0' && *p <= '9') { char *e; c->q_code = strtol(p, &e, 10); p = e; }
			break;
		default:
			refuse_command(cmd);
		}
		g_ncmds++;

		while (is_blank(*p)) p++;
		if (*p != '\0' && *p != ';' && *p != '\n' && *p != '#') {
			char d[64];
			snprintf(d, sizeof d, "extra characters after command: `%.32s'", p);
			die("parse error", d);
		}
	}
	if (g_ncmds == 0) die("no commands in script", 0);
}

// ---------------------------------------------------------------------------
// Input: a line reader with ONE line of lookahead, because `$' has to know it
// is on the last line of the last file before that line is processed.
// ---------------------------------------------------------------------------
static char **g_files;
static int    g_nfiles, g_fileidx;
static int    g_fd = -1;
static char   g_rbuf[8192];
static int    g_rlen = 0, g_rpos = 0;
static int    g_rc_exit = 0;

static char *abspath_of(const char *path, char *out, int outsz)
{
	if (path[0] == '/') return (char *)path;
	char cwd[256];
	if (!getcwd(cwd, sizeof cwd)) cwd[0] = '\0';
	int j = 0;
	for (int i = 0; cwd[i] && j < outsz - 1; i++) out[j++] = cwd[i];
	if (j > 0 && out[j - 1] != '/' && j < outsz - 1) out[j++] = '/';
	for (int k = 0; path[k] && j < outsz - 1; k++) out[j++] = path[k];
	out[j] = '\0';
	return out;
}

static int open_next_file(void)
{
	while (g_fileidx < g_nfiles) {
		const char *path = g_files[g_fileidx++];
		if (strcmp(path, "-") == 0) { g_fd = 0; return 1; }
		char ab[512];
		int fd = open(abspath_of(path, ab, sizeof ab), O_RDONLY);
		if (fd < 0) {
			// Not fatal: POSIX sed reports the file and carries on, but the
			// exit status must record it. Silence here would be the same
			// defect as everything else this file removes.
			char b[512];
			snprintf(b, sizeof b, "sed: can't read %s: No such file or directory\n", path);
			out_flush();
			err_str(b);
			g_rc_exit = 2;
			continue;
		}
		g_fd = fd;
		return 1;
	}
	return 0;
}

static int rd_byte(void)
{
	for (;;) {
		if (g_rpos < g_rlen) return (unsigned char)g_rbuf[g_rpos++];
		if (g_fd < 0) {
			if (!open_next_file()) return -1;
			g_rlen = g_rpos = 0;
			continue;
		}
		long n = read(g_fd, g_rbuf, sizeof g_rbuf);
		if (n > 0) { g_rlen = (int)n; g_rpos = 0; continue; }
		if (g_fd > 0) close(g_fd);
		g_fd = -1;
		if (!open_next_file()) return -1;
		g_rlen = g_rpos = 0;
	}
}

// Reads one line (without its newline) into b. Returns 1 on success, 0 at EOF.
// *had_nl is set to whether the line was newline-terminated.
static int read_line(buf_t *b, int *had_nl)
{
	buf_clear(b);
	*had_nl = 0;
	int c = rd_byte();
	if (c < 0) return 0;
	while (c >= 0) {
		if (c == '\n') { *had_nl = 1; return 1; }
		buf_addc(b, (char)c);
		c = rd_byte();
	}
	return 1;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
static buf_t g_ps;     // pattern space
static buf_t g_sub;    // substitution scratch
static long  g_lineno = 0;

static int addr_match(addr_t *a, int is_last)
{
	switch (a->type) {
	case A_LINE: return g_lineno == a->line;
	case A_LAST: return is_last;
	case A_RE: {
		const regex_t *re = a->has_re ? &a->re : g_last_re;
		if (!re) die("no previous regular expression", 0);
		g_last_re = re;
		return regexec(re, g_ps.p ? g_ps.p : "", 0, 0, 0) == 0;
	}
	default: return 0;
	}
}

static int cmd_selects(cmd_t *c, int is_last)
{
	int sel;
	if (c->naddr == 0) {
		sel = 1;
	} else if (c->naddr == 1) {
		sel = addr_match(&c->a1, is_last);
	} else if (!c->active) {
		if (addr_match(&c->a1, is_last)) {
			c->active = 1;
			// A numeric end address at or before the start line makes the
			// range exactly one line. The end address is never tested on the
			// START line, which is why /a/,/a/ spans to the NEXT /a/.
			if (c->a2.type == A_LINE && c->a2.line <= g_lineno) c->active = 0;
			sel = 1;
		} else {
			sel = 0;
		}
	} else {
		if (addr_match(&c->a2, is_last)) c->active = 0;
		sel = 1;
	}
	if (c->negate) sel = !sel;
	return sel;
}

// & is the whole match, \1..\9 the groups, \n \t \r the usual escapes, and
// \& \\ the literals. Everything else after a backslash is that character.
static void expand_repl(buf_t *dst, const char *repl, const char *subject,
                        long base, const regmatch_t *m, size_t ngroups)
{
	for (const char *r = repl; *r; r++) {
		if (*r == '&') {
			buf_add(dst, subject + base + m[0].rm_so, m[0].rm_eo - m[0].rm_so);
			continue;
		}
		if (*r == '\\' && r[1]) {
			char e = *++r;
			if (e >= '0' && e <= '9') {
				size_t g = (size_t)(e - '0');
				if (g >= ngroups || m[g].rm_so < 0) continue;   // unmatched group -> nothing
				buf_add(dst, subject + base + m[g].rm_so, m[g].rm_eo - m[g].rm_so);
				continue;
			}
			switch (e) {
			case 'n': buf_addc(dst, '\n'); break;
			case 't': buf_addc(dst, '\t'); break;
			case 'r': buf_addc(dst, '\r'); break;
			default:  buf_addc(dst, e);    break;
			}
			continue;
		}
		buf_addc(dst, *r);
	}
}

// Returns 1 if the pattern space changed.
static int do_subst(cmd_t *c)
{
	const regex_t *re = c->s_has_re ? &c->sre : g_last_re;
	if (!re) die("no previous regular expression", 0);
	g_last_re = re;

	const char *s = g_ps.p ? g_ps.p : "";
	long slen = g_ps.len;
	regmatch_t m[10];
	long pos = 0, prev_end = -1;
	int count = 0, changed = 0, eflags = 0;

	buf_clear(&g_sub);
	for (;;) {
		if (pos > slen) break;
		if (regexec(re, s + pos, 10, m, eflags) != 0) break;
		long ms = pos + m[0].rm_so, me = pos + m[0].rm_eo;

		// A NULL match immediately after a previous match is not a match.
		// Without this rule `s/x*/-/g` on "xyz" yields "--y-z-": the engine
		// reports an empty match at the position where "x" just ended, and the
		// replacement is emitted twice in a row. MEASURED against GNU sed 4.9,
		// which gives "-y-z-"; it is the one case in the pre-flight diff that
		// separated a correct engine from a correct DRIVER of one.
		if (me == ms && ms == prev_end) {
			buf_add(&g_sub, s + pos, ms - pos);
			if (ms < slen) buf_addc(&g_sub, s[ms]);
			pos = ms + 1;
			eflags = REG_NOTBOL;
			continue;
		}

		count++;
		buf_add(&g_sub, s + pos, ms - pos);
		if (count >= c->s_nth && (c->s_global || count == c->s_nth)) {
			expand_repl(&g_sub, c->repl, s, pos, m, 10);
			changed = 1;
		} else {
			buf_add(&g_sub, s + ms, me - ms);
		}
		prev_end = me;
		if (me == ms) {
			// An empty match must not spin: emit one byte and step past it.
			if (me < slen) buf_addc(&g_sub, s[me]);
			pos = me + 1;
		} else {
			pos = me;
		}
		// Past the first iteration the subject handed to regexec starts in the
		// middle of the line, so ^ must not be allowed to match there.
		eflags = REG_NOTBOL;
		if (!c->s_global && count >= c->s_nth) break;
	}
	if (!changed) return 0;
	if (pos < slen) buf_add(&g_sub, s + pos, slen - pos);

	buf_t tmp = g_ps; g_ps = g_sub; g_sub = tmp;
	return 1;
}

static void print_ps(int had_nl)
{
	out_mem(g_ps.p ? g_ps.p : "", g_ps.len);
	if (had_nl) out_mem("\n", 1);
}

int main(int argc, char **argv)
{
	buf_t script = { 0, 0, 0 };
	int have_script = 0;
	char *files[64];
	int nfiles = 0;
	int no_more_opts = 0;

	for (int i = 1; i < argc; i++) {
		char *a = argv[i];
		if (!no_more_opts && strcmp(a, "--") == 0) { no_more_opts = 1; continue; }
		if (!no_more_opts && a[0] == '-' && a[1] == '-') {
			if (strncmp(a, "--expression=", 13) == 0) {
				if (script.len) buf_addc(&script, '\n');
				buf_add(&script, a + 13, (long)strlen(a + 13));
				have_script = 1;
				continue;
			}
			if (strcmp(a, "--quiet") == 0 || strcmp(a, "--silent") == 0) { g_quiet = 1; continue; }
			if (strcmp(a, "--regexp-extended") == 0) { g_ere = 1; continue; }
			if (strcmp(a, "--help") == 0) {
				out_str("usage: sed [-n] [-E|-r] [-e script] [-f file] [script] [file ...]\n"
				        "  -n            suppress the automatic printing of the pattern space\n"
				        "  -e SCRIPT     add SCRIPT to the commands to execute\n"
				        "  -f FILE       read the script from FILE\n"
				        "  -E, -r        use extended (ERE) instead of basic (BRE) regular expressions\n"
				        "commands: s/RE/REPL/[gpiN]  y/abc/xyz/  p  d  q[EXIT]  =\n"
				        "addresses: N  $  /RE/  \\cREc  ADDR1,ADDR2  and a leading ! to negate\n");
				out_flush();
				return 0;
			}
			if (strcmp(a, "--in-place") == 0 || strncmp(a, "--in-place=", 11) == 0)
				die("-i / --in-place is not implemented",
				    "write to a new file and rename it, so a failed edit cannot destroy the original");
			die("unknown option", a);
		}
		if (!no_more_opts && a[0] == '-' && a[1] != '\0') {
			for (int k = 1; a[k]; k++) {
				switch (a[k]) {
				case 'n': g_quiet = 1; break;
				case 'E': case 'r': g_ere = 1; break;
				case 'e': case 'f': {
					char which = a[k];
					const char *val = a[k + 1] ? a + k + 1 : 0;
					if (!val) {
						if (i + 1 >= argc) {
							char d[8] = { '-', which, 0 };
							die("option requires an argument", d);
						}
						val = argv[++i];
					}
					if (which == 'e') {
						if (script.len) buf_addc(&script, '\n');
						buf_add(&script, val, (long)strlen(val));
					} else {
						char ab[512];
						int fd = open(abspath_of(val, ab, sizeof ab), O_RDONLY);
						if (fd < 0) die("couldn't open file", val);
						char rb[4096];
						long n;
						if (script.len) buf_addc(&script, '\n');
						while ((n = read(fd, rb, sizeof rb)) > 0) buf_add(&script, rb, n);
						close(fd);
					}
					have_script = 1;
					k = (int)strlen(a) - 1;   // the value consumed the rest
					break;
				}
				case 'i':
					die("-i / --in-place is not implemented",
					    "write to a new file and rename it, so a failed edit cannot destroy the original");
				case 's':
					die("-s (treat files as separate) is not implemented", 0);
				case 'z':
					die("-z (NUL-separated lines) is not implemented", 0);
				default: {
					char d[8] = { '-', a[k], 0 };
					die("unknown option", d);
				}
				}
			}
			continue;
		}
		if (!have_script) {
			buf_add(&script, a, (long)strlen(a));
			have_script = 1;
			continue;
		}
		if (nfiles < 64) files[nfiles++] = a;
		else die("too many input files", "limit is 64");
	}

	if (!have_script) {
		err_str("usage: sed [-n] [-E|-r] [-e script] [-f file] [script] [file ...]\n");
		return 1;
	}

	parse_script(script.p ? script.p : "");

	g_files = files;
	g_nfiles = nfiles;
	g_fileidx = 0;
	if (nfiles == 0) { g_fd = 0; g_nfiles = 0; g_fileidx = 0; }
	else g_fd = -1;

	// One line of lookahead so `$' is known before the line is processed.
	buf_t cur = { 0, 0, 0 }, nxt = { 0, 0, 0 };
	int cur_nl = 0, nxt_nl = 0;
	int have_cur = read_line(&cur, &cur_nl);
	int have_nxt = have_cur ? read_line(&nxt, &nxt_nl) : 0;

	while (have_cur) {
		g_lineno++;
		int is_last = !have_nxt;

		buf_clear(&g_ps);
		buf_add(&g_ps, cur.p ? cur.p : "", cur.len);

		int deleted = 0, quit = 0;
		long qcode = 0;
		for (int i = 0; i < g_ncmds; i++) {
			cmd_t *c = &g_cmds[i];
			if (!cmd_selects(c, is_last)) continue;
			switch (c->cmd) {
			case 's':
				if (do_subst(c) && c->s_print) print_ps(cur_nl);
				break;
			case 'y':
				for (long k = 0; k < g_ps.len; k++)
					g_ps.p[k] = (char)c->ymap[(unsigned char)g_ps.p[k]];
				break;
			case 'p':
				print_ps(cur_nl);
				break;
			case 'd':
				deleted = 1;
				break;
			case '=': {
				char nb[32];
				snprintf(nb, sizeof nb, "%ld\n", g_lineno);
				out_str(nb);
				break;
			}
			case 'q':
				quit = 1;
				qcode = c->q_code;
				break;
			}
			if (deleted || quit) break;
		}

		if (!g_quiet && !deleted) print_ps(cur_nl);
		if (quit) { out_flush(); return (int)qcode; }

		buf_t t = cur; cur = nxt; nxt = t;
		cur_nl = nxt_nl;
		have_cur = have_nxt;
		have_nxt = have_cur ? read_line(&nxt, &nxt_nl) : 0;
	}

	out_flush();
	return g_rc_exit;
}
