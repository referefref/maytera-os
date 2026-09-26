/*
 * cssvar.c - CSS custom-property and math-function resolver (MayteraOS, MIT).
 * See cssvar.h for why this exists and, importantly, what it deliberately is
 * not. Integer arithmetic only; no libm, no float.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "cssvar.h"

/* ------------------------------------------------------------------ */
/* growable output buffer                                             */
/* ------------------------------------------------------------------ */
typedef struct { char *p; size_t n, cap; int oom; } sbuf;

static void sb_init(sbuf *b) { b->p = NULL; b->n = 0; b->cap = 0; b->oom = 0; }

static int sb_grow(sbuf *b, size_t need)
{
	size_t nc;
	char *np;
	if (b->oom) return 0;
	if (b->n + need + 1 <= b->cap) return 1;
	nc = b->cap ? b->cap : 1024;
	while (nc < b->n + need + 1) {
		if (nc > (size_t) 1 << 28) { b->oom = 1; return 0; }
		nc *= 2;
	}
	np = realloc(b->p, nc);
	if (!np) { b->oom = 1; return 0; }
	b->p = np; b->cap = nc;
	return 1;
}

static void sb_putn(sbuf *b, const char *s, size_t n)
{
	if (n == 0) return;
	if (!sb_grow(b, n)) return;
	memcpy(b->p + b->n, s, n);
	b->n += n;
	b->p[b->n] = '\0';
}

static void sb_putc(sbuf *b, char c)
{
	if (!sb_grow(b, 1)) return;
	b->p[b->n++] = c;
	b->p[b->n] = '\0';
}

static void sb_puts(sbuf *b, const char *s) { sb_putn(b, s, strlen(s)); }

static void sb_puti(sbuf *b, long v)
{
	char t[24];
	int i = 0, neg = 0;
	if (v < 0) { neg = 1; v = -v; }
	if (v == 0) t[i++] = '0';
	while (v > 0) { t[i++] = (char) ('0' + (v % 10)); v /= 10; }
	if (neg) sb_putc(b, '-');
	while (i > 0) sb_putc(b, t[--i]);
}

/* ------------------------------------------------------------------ */
/* character classes                                                  */
/* ------------------------------------------------------------------ */
static int is_space(char c)
{ return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* An identifier character, for deciding whether "min(" is the min() function or
 * the tail of a property name like "min-height". Getting this wrong would
 * rewrite "min-height: 3.2rem" into nonsense, so it is tested. */
static int is_ident(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       is_digit(c) || c == '-' || c == '_';
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c; }

/* Case-insensitive compare of src[i..] against a lowercase literal. */
static int match_ci(const char *s, size_t len, size_t i, const char *lit)
{
	size_t k = 0;
	while (lit[k]) {
		if (i + k >= len) return 0;
		if (lower(s[i + k]) != lit[k]) return 0;
		k++;
	}
	return 1;
}

/* ------------------------------------------------------------------ */
/* variable table                                                     */
/* ------------------------------------------------------------------ */
#define CV_MAX_VARS 512
#define CV_NAME_MAX 80
#define CV_VAL_MAX  512

typedef struct {
	char name[CV_NAME_MAX];   /* including the leading "--" */
	char val[CV_VAL_MAX];
} cvar;

typedef struct { cvar v[CV_MAX_VARS]; int n; } cvtab;

static int cv_find(const cvtab *t, const char *name)
{
	int i;
	for (i = 0; i < t->n; i++)
		if (strcmp(t->v[i].name, name) == 0) return i;
	return -1;
}

/* Last definition wins, which matches the cascade for the flat ":root palette"
 * case this collector restricts itself to. */
static void cv_set(cvtab *t, const char *name, const char *val, size_t vlen)
{
	int i = cv_find(t, name);
	if (i < 0) {
		if (t->n >= CV_MAX_VARS) return;
		i = t->n++;
		strncpy(t->v[i].name, name, CV_NAME_MAX - 1);
		t->v[i].name[CV_NAME_MAX - 1] = '\0';
	}
	if (vlen > CV_VAL_MAX - 1) vlen = CV_VAL_MAX - 1;
	memcpy(t->v[i].val, val, vlen);
	t->v[i].val[vlen] = '\0';
}

/* ------------------------------------------------------------------ */
/* source scanning primitives                                         */
/* ------------------------------------------------------------------ */

/* Advance past a /..../ comment starting at i (s[i]=='/' && s[i+1]=='*').
 * Returns the index just past the closing marker. */
static size_t skip_comment(const char *s, size_t len, size_t i)
{
	i += 2;
	while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++;
	return (i + 1 < len) ? i + 2 : len;
}

/* Advance past a quoted string starting at i. Returns index just past the
 * closing quote (or len). */
static size_t skip_string(const char *s, size_t len, size_t i)
{
	char q = s[i++];
	while (i < len) {
		if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
		if (s[i] == q) return i + 1;
		i++;
	}
	return len;
}

/* Index of the ')' matching an open paren whose '(' is at i. Returns len if
 * unbalanced. */
static size_t match_paren(const char *s, size_t len, size_t i)
{
	int d = 0;
	while (i < len) {
		if (s[i] == '"' || s[i] == '\'') { i = skip_string(s, len, i); continue; }
		if (s[i] == '/' && i + 1 < len && s[i + 1] == '*') { i = skip_comment(s, len, i); continue; }
		if (s[i] == '(') d++;
		else if (s[i] == ')') { d--; if (d == 0) return i; }
		i++;
	}
	return len;
}

/* ------------------------------------------------------------------ */
/* pass 1: collect custom properties from global rules                */
/* ------------------------------------------------------------------ */

/*
 * A selector qualifies as "the global palette" if any of its comma-separated
 * parts is :root (possibly with attached qualifiers), html, body or *.
 * Anything else is deliberately ignored: a --x defined on .card is genuinely
 * scoped to .card, and pretending otherwise would paint the wrong colours in
 * the wrong places, which is worse than painting none.
 */
static int selector_is_global(const char *s, size_t len)
{
	size_t i = 0;
	while (i < len) {
		size_t a, b;
		while (i < len && (is_space(s[i]) || s[i] == ',')) i++;
		a = i;
		while (i < len && s[i] != ',') i++;
		b = i;
		while (b > a && is_space(s[b - 1])) b--;
		if (b > a) {
			size_t n = b - a;
			if (n == 4 && match_ci(s, len, a, "html")) return 1;
			if (n == 4 && match_ci(s, len, a, "body")) return 1;
			if (n == 1 && s[a] == '*') return 1;
			/* :root, :root[data-theme=...], html:root, ... */
			{
				size_t k;
				for (k = a; k + 5 <= b; k++)
					if (match_ci(s, len, k, ":root")) return 1;
			}
		}
	}
	return 0;
}

#define CV_MAX_DEPTH 32

static void collect_vars(const char *s, size_t len, cvtab *t)
{
	size_t i = 0;
	size_t prelude_start = 0;
	int depth = 0;
	/* kind[d] for the block opened at depth d+1: 1 = plain rule, 0 = at-rule */
	int kind[CV_MAX_DEPTH];
	int is_global[CV_MAX_DEPTH];

	memset(kind, 0, sizeof(kind));
	memset(is_global, 0, sizeof(is_global));

	while (i < len) {
		char c = s[i];
		if (c == '/' && i + 1 < len && s[i + 1] == '*') { i = skip_comment(s, len, i); continue; }
		if (c == '"' || c == '\'') { i = skip_string(s, len, i); continue; }

		if (c == '{') {
			size_t a = prelude_start, b = i;
			int at = 0;
			while (a < b && is_space(s[a])) a++;
			if (a < b && s[a] == '@') at = 1;
			if (depth < CV_MAX_DEPTH) {
				kind[depth] = at ? 0 : 1;
				is_global[depth] = (!at && depth == 0)
					? selector_is_global(s + a, b - a) : 0;
			}
			depth++;
			i++;
			prelude_start = i;
			continue;
		}
		if (c == '}') {
			if (depth > 0) depth--;
			i++;
			prelude_start = i;
			continue;
		}
		if (c == ';' && depth == 0) { i++; prelude_start = i; continue; }

		/*
		 * A custom-property declaration. Only accepted at depth 1 (directly
		 * inside a rule) from a rule that is itself top-level (not nested in
		 * an @media / @supports whose condition we have not evaluated) and
		 * whose selector is a global one.
		 */
		if (depth == 1 && c == '-' && i + 1 < len && s[i + 1] == '-' &&
		    (i == 0 || !is_ident(s[i - 1]))) {
			size_t ns = i, ne, vs, ve;
			int accept = kind[0] == 1 && is_global[0];
			ne = ns;
			while (ne < len && s[ne] != ':' && s[ne] != ';' && s[ne] != '}') ne++;
			if (ne >= len || s[ne] != ':') { i++; continue; }
			vs = ne + 1;
			{
				int pd = 0;
				size_t j = vs;
				while (j < len) {
					if (s[j] == '"' || s[j] == '\'') { j = skip_string(s, len, j); continue; }
					if (s[j] == '/' && j + 1 < len && s[j + 1] == '*') { j = skip_comment(s, len, j); continue; }
					if (s[j] == '(') pd++;
					else if (s[j] == ')') { if (pd > 0) pd--; }
					else if (pd == 0 && (s[j] == ';' || s[j] == '}')) break;
					j++;
				}
				ve = j;
			}
			if (accept) {
				char nm[CV_NAME_MAX];
				size_t nlen = ne;
				size_t a = vs, b = ve;
				while (nlen > ns && is_space(s[nlen - 1])) nlen--;
				nlen -= ns;
				if (nlen > CV_NAME_MAX - 1) nlen = CV_NAME_MAX - 1;
				memcpy(nm, s + ns, nlen);
				nm[nlen] = '\0';
				while (a < b && is_space(s[a])) a++;
				while (b > a && is_space(s[b - 1])) b--;
				cv_set(t, nm, s + a, b - a);
			}
			i = ve;
			continue;
		}
		i++;
	}
}

/* ------------------------------------------------------------------ */
/* pass 2: expand var() and drop custom-property declarations         */
/* ------------------------------------------------------------------ */

static void expand_text(sbuf *out, const char *s, size_t len,
		const cvtab *t, int depth);

/*
 * Emit the substitution for one var(...) whose '(' is at `open`. Returns the
 * index just past the matching ')'.
 */
static size_t expand_var(sbuf *out, const char *s, size_t len, size_t open,
		const cvtab *t, int depth)
{
	size_t close = match_paren(s, len, open);
	size_t a = open + 1, b = close;
	size_t ne, fb_s = 0, fb_e = 0;
	char nm[CV_NAME_MAX];
	size_t nlen;
	int idx;

	if (close >= len) { sb_putn(out, s + open, len - open); return len; }

	while (a < b && is_space(s[a])) a++;
	/* name runs to the first top-level comma */
	{
		int pd = 0;
		size_t j = a;
		ne = b;
		while (j < b) {
			if (s[j] == '(') pd++;
			else if (s[j] == ')') pd--;
			else if (s[j] == ',' && pd == 0) { ne = j; fb_s = j + 1; fb_e = b; break; }
			j++;
		}
		if (ne == b) { fb_s = fb_e = 0; }
	}
	{
		size_t e = ne;
		while (e > a && is_space(s[e - 1])) e--;
		nlen = e - a;
	}
	if (nlen > CV_NAME_MAX - 1) nlen = CV_NAME_MAX - 1;
	memcpy(nm, s + a, nlen);
	nm[nlen] = '\0';

	idx = cv_find(t, nm);
	if (idx >= 0 && depth < 12) {
		expand_text(out, t->v[idx].val, strlen(t->v[idx].val), t, depth + 1);
	} else if (fb_e > fb_s && depth < 12) {
		size_t fa = fb_s, fbb = fb_e;
		while (fa < fbb && is_space(s[fa])) fa++;
		while (fbb > fa && is_space(s[fbb - 1])) fbb--;
		expand_text(out, s + fa, fbb - fa, t, depth + 1);
	}
	/* else: emit nothing. The declaration becomes invalid and libcss drops
	 * it, which is exactly what the spec requires for an unresolvable var. */
	return close + 1;
}

/* Copy `s` to `out`, expanding var() and honouring strings. Used both for the
 * document body and for a variable's own value (which may itself contain
 * var()). Does not touch declarations; the caller handles those. */
static void expand_text(sbuf *out, const char *s, size_t len,
		const cvtab *t, int depth)
{
	size_t i = 0;
	while (i < len) {
		char c = s[i];
		if (c == '"' || c == '\'') {
			size_t e = skip_string(s, len, i);
			sb_putn(out, s + i, e - i);
			i = e;
			continue;
		}
		if (c == '/' && i + 1 < len && s[i + 1] == '*') { i = skip_comment(s, len, i); continue; }
		if ((c == 'v' || c == 'V') && match_ci(s, len, i, "var") &&
		    (i == 0 || !is_ident(s[i - 1]))) {
			size_t j = i + 3;
			while (j < len && is_space(s[j])) j++;
			if (j < len && s[j] == '(') {
				i = expand_var(out, s, len, j, t, depth);
				continue;
			}
		}
		sb_putc(out, c);
		i++;
	}
}

static void substitute(const char *s, size_t len, const cvtab *t, sbuf *out)
{
	size_t i = 0;
	int depth = 0;

	while (i < len) {
		char c = s[i];
		if (c == '/' && i + 1 < len && s[i + 1] == '*') { i = skip_comment(s, len, i); continue; }
		if (c == '"' || c == '\'') {
			size_t e = skip_string(s, len, i);
			sb_putn(out, s + i, e - i);
			i = e;
			continue;
		}
		if (c == '{') { depth++; sb_putc(out, c); i++; continue; }
		if (c == '}') { if (depth > 0) depth--; sb_putc(out, c); i++; continue; }

		/* Drop a custom-property declaration wherever it appears. libcss
		 * cannot parse one, and leaving it in only costs parse errors. */
		if (depth > 0 && c == '-' && i + 1 < len && s[i + 1] == '-' &&
		    (i == 0 || !is_ident(s[i - 1]))) {
			size_t j = i;
			int pd = 0;
			int is_decl = 0;
			while (j < len && s[j] != ':' && s[j] != ';' && s[j] != '}' && s[j] != '{') j++;
			if (j < len && s[j] == ':') is_decl = 1;
			if (is_decl) {
				while (j < len) {
					if (s[j] == '"' || s[j] == '\'') { j = skip_string(s, len, j); continue; }
					if (s[j] == '/' && j + 1 < len && s[j + 1] == '*') { j = skip_comment(s, len, j); continue; }
					if (s[j] == '(') pd++;
					else if (s[j] == ')') { if (pd > 0) pd--; }
					else if (pd == 0 && s[j] == ';') { j++; break; }
					else if (pd == 0 && s[j] == '}') break;
					j++;
				}
				i = j;
				continue;
			}
		}

		if ((c == 'v' || c == 'V') && match_ci(s, len, i, "var") &&
		    (i == 0 || !is_ident(s[i - 1]))) {
			size_t j = i + 3;
			while (j < len && is_space(s[j])) j++;
			if (j < len && s[j] == '(') {
				i = expand_var(out, s, len, j, t, 0);
				continue;
			}
		}
		sb_putc(out, c);
		i++;
	}
}

/* ------------------------------------------------------------------ */
/* pass 3: evaluate clamp() / min() / max() / calc()                  */
/* ------------------------------------------------------------------ */

/*
 * A value carried through the evaluator. `milli` is thousandths, either of a
 * CSS pixel (when px == 1) or of a plain number (px == 0). Keeping the two
 * apart is what lets calc(2rem * 2) work and calc(2rem * 2px) fail.
 */
typedef struct {
	long milli;
	int  px;   /* 1 = a resolved length in px, 0 = unitless number */
	int  ok;
} cval;

typedef struct {
	const char *s;
	size_t len, i;
	int vw, vh;   /* viewport, CSS px */
} cparse;

static cval cv_bad(void) { cval v; v.milli = 0; v.px = 0; v.ok = 0; return v; }

static void cp_ws(cparse *p) { while (p->i < p->len && is_space(p->s[p->i])) p->i++; }

static cval parse_expr(cparse *p);

/* number [unit] */
static cval parse_number(cparse *p)
{
	cval v;
	long ip = 0, fr = 0, scale = 1;
	int neg = 0, any = 0;
	size_t us, ue;

	v.ok = 1; v.px = 0; v.milli = 0;
	cp_ws(p);
	if (p->i < p->len && (p->s[p->i] == '+' || p->s[p->i] == '-')) {
		neg = (p->s[p->i] == '-');
		p->i++;
	}
	while (p->i < p->len && is_digit(p->s[p->i])) { ip = ip * 10 + (p->s[p->i] - '0'); p->i++; any = 1; }
	if (p->i < p->len && p->s[p->i] == '.') {
		p->i++;
		while (p->i < p->len && is_digit(p->s[p->i])) {
			if (scale < 1000) { fr = fr * 10 + (p->s[p->i] - '0'); scale *= 10; }
			p->i++;
			any = 1;
		}
	}
	if (!any) return cv_bad();
	v.milli = ip * 1000 + (fr * 1000) / scale;
	if (neg) v.milli = -v.milli;

	us = p->i;
	while (p->i < p->len && ((p->s[p->i] >= 'a' && p->s[p->i] <= 'z') ||
	                         (p->s[p->i] >= 'A' && p->s[p->i] <= 'Z'))) p->i++;
	ue = p->i;
	if (ue == us) {
		if (p->i < p->len && p->s[p->i] == '%') return cv_bad();  /* unresolvable here */
		return v;   /* unitless */
	}
	{
		size_t n = ue - us;
		const char *u = p->s + us;
		char b[8];
		size_t k;
		if (n >= sizeof(b)) return cv_bad();
		for (k = 0; k < n; k++) b[k] = lower(u[k]);
		b[n] = '\0';
		v.px = 1;
		if      (strcmp(b, "px") == 0)   { /* as-is */ }
		else if (strcmp(b, "rem") == 0)  v.milli = v.milli * 16;
		/* em without an element context: the root font size is the only
		 * defensible stand-in, and it is right for the top-level spacing
		 * and type scale this is used for. Stated, not hidden. */
		else if (strcmp(b, "em") == 0)   v.milli = v.milli * 16;
		else if (strcmp(b, "pt") == 0)   v.milli = (v.milli * 96) / 72;
		else if (strcmp(b, "pc") == 0)   v.milli = (v.milli * 16);
		else if (strcmp(b, "in") == 0)   v.milli = v.milli * 96;
		else if (strcmp(b, "cm") == 0)   v.milli = (v.milli * 96) / 254 * 10;
		else if (strcmp(b, "mm") == 0)   v.milli = (v.milli * 96) / 254;
		else if (strcmp(b, "ch") == 0)   v.milli = (v.milli * 8);
		else if (strcmp(b, "ex") == 0)   v.milli = (v.milli * 8);
		else if (strcmp(b, "vw") == 0)   v.milli = (v.milli * p->vw) / 100;
		else if (strcmp(b, "vh") == 0)   v.milli = (v.milli * p->vh) / 100;
		else if (strcmp(b, "vmin") == 0) v.milli = (v.milli * (p->vw < p->vh ? p->vw : p->vh)) / 100;
		else if (strcmp(b, "vmax") == 0) v.milli = (v.milli * (p->vw > p->vh ? p->vw : p->vh)) / 100;
		else return cv_bad();
	}
	return v;
}

/* Collect the comma-separated arguments of a function whose '(' is at p->i. */
static int parse_args(cparse *p, cval *out, int max)
{
	int n = 0;
	if (p->i >= p->len || p->s[p->i] != '(') return -1;
	p->i++;
	for (;;) {
		cval v;
		if (n >= max) return -1;
		v = parse_expr(p);
		if (!v.ok) return -1;
		out[n++] = v;
		cp_ws(p);
		if (p->i < p->len && p->s[p->i] == ',') { p->i++; continue; }
		if (p->i < p->len && p->s[p->i] == ')') { p->i++; return n; }
		return -1;
	}
}

/* Two operands are compatible if both are lengths or both are plain numbers. */
static int compat(cval a, cval b) { return a.px == b.px; }

static cval parse_factor(cparse *p)
{
	cp_ws(p);
	if (p->i >= p->len) return cv_bad();
	if (p->s[p->i] == '(') {
		cval v;
		p->i++;
		v = parse_expr(p);
		cp_ws(p);
		if (p->i < p->len && p->s[p->i] == ')') { p->i++; return v; }
		return cv_bad();
	}
	if (match_ci(p->s, p->len, p->i, "calc") && !is_ident(p->i ? p->s[p->i - 1] : ' ')) {
		cval args[1];
		p->i += 4;
		cp_ws(p);
		if (parse_args(p, args, 1) != 1) return cv_bad();
		return args[0];
	}
	if (match_ci(p->s, p->len, p->i, "clamp")) {
		cval a[3];
		p->i += 5;
		cp_ws(p);
		if (parse_args(p, a, 3) != 3) return cv_bad();
		if (!compat(a[0], a[1]) || !compat(a[1], a[2])) return cv_bad();
		{
			cval v = a[1];
			if (v.milli < a[0].milli) v = a[0];
			if (v.milli > a[2].milli) v = a[2];
			/* A clamp whose max is below its min resolves to the min. */
			if (a[2].milli < a[0].milli) v = a[0];
			return v;
		}
	}
	if (match_ci(p->s, p->len, p->i, "min") || match_ci(p->s, p->len, p->i, "max")) {
		int want_min = (lower(p->s[p->i + 1]) == 'i');
		cval a[8];
		int n, k;
		cval v;
		p->i += 3;
		cp_ws(p);
		n = parse_args(p, a, 8);
		if (n < 1) return cv_bad();
		v = a[0];
		for (k = 1; k < n; k++) {
			if (!compat(v, a[k])) return cv_bad();
			if (want_min ? (a[k].milli < v.milli) : (a[k].milli > v.milli)) v = a[k];
		}
		return v;
	}
	return parse_number(p);
}

static cval parse_term(cparse *p)
{
	cval a = parse_factor(p);
	if (!a.ok) return a;
	for (;;) {
		cp_ws(p);
		if (p->i < p->len && p->s[p->i] == '*') {
			cval b;
			p->i++;
			b = parse_factor(p);
			if (!b.ok) return cv_bad();
			if (a.px && b.px) return cv_bad();   /* px * px is not a length */
			a.milli = (a.milli * b.milli) / 1000;
			a.px = a.px | b.px;
			continue;
		}
		if (p->i < p->len && p->s[p->i] == '/') {
			cval b;
			p->i++;
			b = parse_factor(p);
			if (!b.ok || b.px || b.milli == 0) return cv_bad();
			a.milli = (a.milli * 1000) / b.milli;
			continue;
		}
		return a;
	}
}

static cval parse_expr(cparse *p)
{
	cval a = parse_term(p);
	if (!a.ok) return a;
	for (;;) {
		cp_ws(p);
		if (p->i < p->len && (p->s[p->i] == '+' || p->s[p->i] == '-')) {
			/*
			 * A sign is only an operator if it is separated from what
			 * follows the way calc() requires. "10px -5px" is two values,
			 * not a subtraction, and this is inside a comma-delimited
			 * argument list where both shapes occur.
			 */
			int sub = (p->s[p->i] == '-');
			size_t save = p->i;
			cval b;
			if (p->i == 0 || !is_space(p->s[p->i - 1])) return a;
			if (p->i + 1 >= p->len || !is_space(p->s[p->i + 1])) { (void) save; return a; }
			p->i++;
			b = parse_term(p);
			if (!b.ok || !compat(a, b)) return cv_bad();
			a.milli = sub ? a.milli - b.milli : a.milli + b.milli;
			continue;
		}
		return a;
	}
}

static long milli_round(long m)
{
	return (m >= 0) ? (m + 500) / 1000 : -((-m + 500) / 1000);
}

/*
 * Rewrite math functions. Anything that does not fully resolve is copied
 * through byte for byte, so the worst case is exactly today's behaviour.
 */
static void eval_math(const char *s, size_t len, int vw, int vh, sbuf *out)
{
	size_t i = 0;
	while (i < len) {
		char c = s[i];
		if (c == '"' || c == '\'') {
			size_t e = skip_string(s, len, i);
			sb_putn(out, s + i, e - i);
			i = e;
			continue;
		}
		if ((i == 0 || !is_ident(s[i - 1])) &&
		    (match_ci(s, len, i, "clamp(") || match_ci(s, len, i, "calc(") ||
		     match_ci(s, len, i, "min(")   || match_ci(s, len, i, "max("))) {
			cparse p;
			cval v;
			size_t open = i;
			while (open < len && s[open] != '(') open++;
			p.s = s; p.len = len; p.i = i; p.vw = vw; p.vh = vh;
			v = parse_factor(&p);
			if (v.ok) {
				sb_puti(out, milli_round(v.milli));
				if (v.px) sb_puts(out, "px");
				i = p.i;
				continue;
			}
			{
				size_t close = match_paren(s, len, open);
				size_t e = (close < len) ? close + 1 : len;
				sb_putn(out, s + i, e - i);
				i = e;
				continue;
			}
		}
		sb_putc(out, c);
		i++;
	}
}

/* ------------------------------------------------------------------ */
/* pass 4: carry grid track counts through properties libcss DOES have */
/* ------------------------------------------------------------------ */

/*
 * libcss has no grid support at all: `grid-template-columns` and `gap` are
 * parse errors and vanish, so a grid container reaches layout with no way to
 * know how many columns it has. Adding grid properties to libcss means new
 * property parsers, new computed-style storage and a new bytecode opcode, in a
 * pinned third-party tree that the browser and the CSS conformance work both
 * depend on. That is a fork, not a fix.
 *
 * What libcss DOES have, fully plumbed through parse, cascade and computed
 * style, is CSS multi-column: `column-count`, `column-width` and `column-gap`.
 * Those carry exactly the three numbers a simple grid needs, and they mean
 * nearly the same thing: "this many equal columns", "columns at least this
 * wide, fit as many as you can", "this much between them". So the track list is
 * REDUCED HERE, in text, to whichever of those two properties expresses it, and
 * layout.c reads them back for a `display: grid` element.
 *
 * It is a carrier, and it is worth being clear that it is one: a grid whose
 * track list is not uniform (`200px 1fr 3em`) cannot be expressed this way and
 * gets its column COUNT only, so the tracks come out equal. Named lines, areas,
 * spans, `grid-auto-flow` and explicit row tracks are not represented at all.
 * The alternative was no columns whatsoever.
 *
 * `gap` is translated for flex containers too, where it means the same thing
 * and libcss drops it for the same reason.
 *
 * THE ROW GAP NEEDS A SECOND CARRIER (#245 enggap). column-gap is ONE number
 * and CSS has two: `gap: <row> <column>`, plus the `row-gap` longhand. Until
 * now only the column component survived and layout.c spent it on BOTH axes,
 * so `gap: 24px 8px` put 8px between the flex lines and a bare `column-gap`
 * opened a row gap the author never wrote.
 *
 * The row component rides `clip`, chosen on the same three properties the
 * radius carrier was: it is NOT INHERITED (libcss has it in the uncommon
 * group and css__compose_clip only follows the parent on an explicit
 * `inherit`, so a row gap cannot leak into a nested flex container); it is
 * FOUR LENGTHS with a full parse/cascade/computed path, of which we spend one;
 * and it is inert here, because this engine never clips anything. Its initial
 * value is CSS_CLIP_AUTO, which is DISTINGUISHABLE from a carried
 * CSS_CLIP_RECT, so an unstyled element cannot be mistaken for a zero gap.
 *
 * THE COLLISION IS REAL AND IS THE PRICE, and it is a cheap one: `clip` is a
 * deprecated property whose one surviving idiom on the modern web is
 * `.sr-only { clip: rect(0 0 0 0) }`, which carries 0 and is therefore inert.
 * Only a plain non-negative LENGTH is carried: a percentage row gap has
 * nothing to resolve against at this stage and `normal` is not a length, so
 * both are dropped and the row gap stays 0. libcss rejects a percentage in a
 * clip rect anyway, so the guard here and the parser agree.
 *
 * IT RIDES THE THIRD COMPONENT (`bottom`), NOT THE FIRST, because libcss's
 * generated get_clip() reads the TOP and RIGHT units with a `&`/`>>`
 * precedence bug and reports both as em. Measured: a 10px row gap in the top
 * slot laid out as 150px. The full diagnosis is in the read site in layout.c;
 * the two ends of the carrier must agree on the slot, so do not move one
 * without the other.
 *
 * BORDER-RADIUS RIDES THE SAME MECHANISM, for the same reason: libcss has no
 * radius properties at all (there is no CSS_PROP_BORDER_*_RADIUS in
 * properties.h and no getter in computed.h), so `border-radius: 10px` is a
 * parse error and every card, pill, chip and code well on a modern page comes
 * out as a rectangle. maytera.net uses it 31 times.
 *
 * The carrier is `column-rule-width`. It was chosen on three properties, in
 * this order: it is NOT INHERITED (so a radius cannot leak into children, which
 * rules out border-spacing, letter-spacing, word-spacing and text-indent); it
 * is a LENGTH with a full parse/cascade/computed path; and it is inert here,
 * because a column rule only paints inside a multi-column container and this
 * engine implements none, so nothing is lost by spending it.
 *
 * THE COLLISION IS REAL AND IS THE PRICE: a page that genuinely sets
 * `column-rule-width` will have it read back as a corner radius. That is one
 * property, on a feature this engine does not render, against every rounded
 * corner on the modern web.
 *
 * Only the FIRST radius component is carried. `border-radius: 4px 4px 0 0` and
 * the `/` elliptical form both collapse to their first value, so a partly
 * rounded box comes out uniformly rounded. A percentage becomes a very large
 * pixel value, which layout clamps to half the shorter side: that is exactly
 * what `border-radius: 50%` means for a square and a reasonable answer
 * otherwise.
 */

/*
 * Emit the ROW-GAP CARRIER for the value s[a..b), preceded by a `;` when sep is
 * set and something is actually emitted (#245 enggap). See the carrier note
 * above. Emits nothing at all for anything that is not a plain non-negative
 * length, which leaves the row gap at its pre-enggap inert 0.
 */
static void emit_row_gap(sbuf *out, const char *s, size_t a, size_t b, int sep)
{
	size_t j;

	while (a < b && is_space(s[a])) a++;
	while (b > a && is_space(s[b - 1])) b--;
	if (b <= a || b - a > 24) return;
	if (!is_digit(s[a]) && s[a] != '.') return;
	for (j = a; j < b; j++) {
		/* a percentage has no basis here; a space, a paren or a comma
		 * means this is not the single length it has to be. */
		if (s[j] == '%' || is_space(s[j]) || s[j] == '(' ||
				s[j] == ')' || s[j] == ',')
			return;
	}
	if (sep) sb_putc(out, ';');
	/* rect(top, right, BOTTOM, left): the third, for the reason above. */
	sb_puts(out, "clip:rect(0px,0px,");
	sb_putn(out, s + a, b - a);
	sb_puts(out, ",0px)");
}

/* Reduce a grid-template-columns value to a column count and/or a minimum
 * column width. Returns 1 if it produced anything. */
static int grid_tracks(const char *v, size_t len, long *count, long *minpx)
{
	size_t i = 0;
	*count = 0;
	*minpx = 0;

	while (i < len && is_space(v[i])) i++;
	if (i >= len) return 0;
	if (match_ci(v, len, i, "none") || match_ci(v, len, i, "inherit")) return 0;

	if (match_ci(v, len, i, "repeat")) {
		size_t open = i;
		size_t close, a;
		while (open < len && v[open] != '(') open++;
		if (open >= len) return 0;
		close = match_paren(v, len, open);
		a = open + 1;
		while (a < close && is_space(v[a])) a++;
		/* repeat(<N>, ...) is an explicit count. */
		if (is_digit(v[a])) {
			long n = 0;
			while (a < close && is_digit(v[a])) { n = n * 10 + (v[a] - '0'); a++; }
			if (n > 0) { *count = n; return 1; }
			return 0;
		}
		/* repeat(auto-fill|auto-fit, minmax(<len>, ...)) is a minimum width. */
		if (match_ci(v, len, a, "auto-fill") || match_ci(v, len, a, "auto-fit")) {
			size_t m = a;
			while (m < close && !match_ci(v, len, m, "minmax(")) m++;
			if (m >= close) return 0;
			m += 7;
			while (m < close && is_space(v[m])) m++;
			{
				long px = 0;
				int any = 0;
				while (m < close && is_digit(v[m])) { px = px * 10 + (v[m] - '0'); any = 1; m++; }
				if (!any || px <= 0) return 0;
				*minpx = px;
				return 1;
			}
		}
		return 0;
	}

	/* An explicit track list: count the top-level components. */
	{
		long n = 0;
		int in_tok = 0;
		while (i < len) {
			if (v[i] == '(') { i = match_paren(v, len, i); if (i < len) i++; in_tok = 1; continue; }
			if (is_space(v[i])) { if (in_tok) { n++; in_tok = 0; } i++; continue; }
			in_tok = 1;
			i++;
		}
		if (in_tok) n++;
		if (n > 0) { *count = n; return 1; }
	}
	return 0;
}

/* ================================================================== */
/* CSS gradient parsing (#245 enggrad).                               */
/*
 * A source-rewrite pass that turns a `background`/`background-image` gradient
 * into a table entry plus an outline-color carrier. See the long note in
 * cssvar.h. This is a bounded common-case parser, NOT a spec implementation:
 *   - linear-gradient: `to <side[ side]>` or `<angle>` (deg/turn/grad/rad or a
 *     bare number = deg), else the default `to bottom`; then 2..8 color stops
 *     each `<color> [<pct>%]`. Corner directions use 45-degree diagonals, not
 *     the aspect-corrected magic corner.
 *   - radial-gradient: `circle` or `ellipse` (default ellipse); size and
 *     position beyond center are ignored (extent is farthest-side for the
 *     ellipse, farthest-corner for the circle, decided in main.c); then stops.
 *   - color: #rgb, #rgba, #rrggbb, #rrggbbaa (alpha dropped), rgb()/rgba()
 *     (alpha dropped) and a small set of named colors.
 * DEFERRED (see the plan doc): conic-gradient, repeating-* gradients, color
 * interpolation hints (`<pct>` with no color), multiple layered backgrounds,
 * and inline style="" gradients (the inline path does not run this pass).
 */
#define CV_MAX_GRADS 64

static cssvar_gradient g_grads[CV_MAX_GRADS];
static int g_grad_n = 0;

void cssvar_gradients_reset(void) { g_grad_n = 0; }
int  cssvar_gradient_count(void) { return g_grad_n; }
const cssvar_gradient *cssvar_gradient_get(int idx)
{
	if (idx < 0 || idx >= g_grad_n) return NULL;
	return &g_grads[idx];
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	c = lower(c);
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/* Named colors this bounded parser recognises. Kept small on purpose; the
 * overwhelming majority of gradient stops on real pages are hex or rgb(). */
struct namedcol { const char *name; unsigned int rgb; };
static const struct namedcol g_named[] = {
	{ "black",   0x000000 }, { "white",  0xFFFFFF }, { "red",     0xFF0000 },
	{ "green",   0x008000 }, { "lime",   0x00FF00 }, { "blue",    0x0000FF },
	{ "yellow",  0xFFFF00 }, { "cyan",   0x00FFFF }, { "aqua",    0x00FFFF },
	{ "magenta", 0xFF00FF }, { "fuchsia",0xFF00FF }, { "silver",  0xC0C0C0 },
	{ "gray",    0x808080 }, { "grey",   0x808080 }, { "maroon",  0x800000 },
	{ "olive",   0x808000 }, { "purple", 0x800080 }, { "teal",    0x008080 },
	{ "navy",    0x000080 }, { "orange", 0xFFA500 }, { "pink",    0xFFC0CB },
	{ "gold",    0xFFD700 }, { "coral",  0xFF7F50 }, { "salmon",  0xFA8072 },
	{ "tomato",  0xFF6347 }, { "indigo", 0x4B0082 }, { "violet",  0xEE82EE },
	{ "brown",   0xA52A2A }, { "tan",    0xD2B48C }, { "beige",   0xF5F5DC },
	{ "ivory",   0xFFFFF0 }, { "khaki",  0xF0E68C }, { "crimson", 0xDC143C },
	{ "skyblue", 0x87CEEB }, { "steelblue", 0x4682B4 }, { "dodgerblue", 0x1E90FF },
	{ "royalblue", 0x4169E1 }, { "midnightblue", 0x191970 },
	{ "turquoise", 0x40E0D0 }, { "seagreen", 0x2E8B57 },
	{ "forestgreen", 0x228B22 }, { "limegreen", 0x32CD32 },
	{ "darkgreen", 0x006400 }, { "lightgreen", 0x90EE90 },
	{ "darkred", 0x8B0000 }, { "firebrick", 0xB22222 },
	{ "hotpink", 0xFF69B4 }, { "deeppink", 0xFF1493 },
	{ "lightblue", 0xADD8E6 }, { "lightgray", 0xD3D3D3 },
	{ "lightgrey", 0xD3D3D3 }, { "darkgray", 0xA9A9A9 },
	{ "darkgrey", 0xA9A9A9 }, { "slategray", 0x708090 },
	{ "lavender", 0xE6E6FA }, { "plum", 0xDDA0DD }, { "orchid", 0xDA70D6 },
	{ "chocolate", 0xD2691E }, { "sienna", 0xA0522D }, { "goldenrod", 0xDAA520 },
	{ "transparent", 0x000000 },
};

/* Parse a single CSS color from v[a..end). Returns 1 and *out set on success,
 * *consumed set to the byte after the color token. */
static int parse_color(const char *v, size_t a, size_t end,
		unsigned int *out, size_t *consumed)
{
	while (a < end && is_space(v[a])) a++;
	if (a >= end) return 0;

	if (v[a] == '#') {
		size_t h = a + 1, k = 0;
		int d[8];
		while (h < end && k < 8) {
			int hv = hexval(v[h]);
			if (hv < 0) break;
			d[k++] = hv; h++;
		}
		if (k == 3 || k == 4) {          /* #rgb / #rgba */
			*out = ((unsigned)(d[0] * 17) << 16) |
			       ((unsigned)(d[1] * 17) << 8) |
			        (unsigned)(d[2] * 17);
			*consumed = h; return 1;
		}
		if (k == 6 || k == 8) {          /* #rrggbb / #rrggbbaa */
			*out = ((unsigned)(d[0] * 16 + d[1]) << 16) |
			       ((unsigned)(d[2] * 16 + d[3]) << 8) |
			        (unsigned)(d[4] * 16 + d[5]);
			*consumed = h; return 1;
		}
		return 0;
	}

	if (match_ci(v, end, a, "rgb(") || match_ci(v, end, a, "rgba(")) {
		size_t p = a;
		while (p < end && v[p] != '(') p++;
		if (p >= end) return 0;
		{
			size_t close = match_paren(v, end, p);
			int comp[4], nc = 0;
			size_t q = p + 1;
			while (q < close && nc < 4) {
				long val = 0; int any = 0, isneg = 0;
				while (q < close && is_space(v[q])) q++;
				if (q < close && v[q] == '-') { isneg = 1; q++; }
				while (q < close && is_digit(v[q])) { val = val * 10 + (v[q]-'0'); any = 1; q++; }
				/* percentage component: scale 0..100 to 0..255 */
				if (q < close && v[q] == '%') { val = val * 255 / 100; q++; }
				if (isneg) val = 0;
				if (any) comp[nc++] = (int) val;
				while (q < close && (is_space(v[q]) || v[q] == ',' || v[q] == '/')) q++;
				if (q < close && v[q] == ')') break;
			}
			if (nc >= 3) {
				int r = comp[0], g = comp[1], b = comp[2];
				if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
				*out = ((unsigned) r << 16) | ((unsigned) g << 8) | (unsigned) b;
				*consumed = (close < end) ? close + 1 : end;
				return 1;
			}
		}
		return 0;
	}

	/* named color: read the ident, look it up */
	{
		size_t e = a;
		char nm[24]; size_t k = 0;
		while (e < end && (is_ident(v[e])) && k < sizeof(nm) - 1) { nm[k++] = lower(v[e]); e++; }
		nm[k] = '\0';
		if (k > 0) {
			size_t j;
			for (j = 0; j < sizeof(g_named) / sizeof(g_named[0]); j++) {
				if (strcmp(nm, g_named[j].name) == 0) {
					*out = g_named[j].rgb;
					*consumed = e; return 1;
				}
			}
		}
	}
	return 0;
}

/* Parse an integer or decimal number from v[a..end); returns milli-units
 * (e.g. 45.5 -> 45500) and advances *consumed. Sign handled. */
static long parse_num_milli(const char *v, size_t a, size_t end, size_t *consumed)
{
	long ip = 0, fp = 0, scale = 1;
	int neg = 0, any = 0;
	while (a < end && is_space(v[a])) a++;
	if (a < end && (v[a] == '-' || v[a] == '+')) { neg = (v[a] == '-'); a++; }
	while (a < end && is_digit(v[a])) { ip = ip * 10 + (v[a]-'0'); a++; any = 1; }
	if (a < end && v[a] == '.') {
		a++;
		while (a < end && is_digit(v[a]) && scale < 1000) {
			fp = fp * 10 + (v[a]-'0'); scale *= 10; a++; any = 1;
		}
	}
	*consumed = a;
	if (!any) return 0;
	{
		long milli = ip * 1000 + (fp * 1000) / scale;
		return neg ? -milli : milli;
	}
}

/* Parse a linear-gradient direction (angle text) into CSS-convention degrees.
 * Returns 1 if v[a..end) is a direction, setting *deg; else 0 (it is a stop). */
static int parse_direction(const char *v, size_t a, size_t end, int *deg)
{
	while (a < end && is_space(v[a])) a++;
	if (match_ci(v, end, a, "to")) {
		int top = 0, bot = 0, left = 0, right = 0;
		size_t p = a + 2;
		while (p < end) {
			while (p < end && is_space(v[p])) p++;
			if (match_ci(v, end, p, "top"))        { top = 1; p += 3; }
			else if (match_ci(v, end, p, "bottom")) { bot = 1; p += 6; }
			else if (match_ci(v, end, p, "left"))   { left = 1; p += 4; }
			else if (match_ci(v, end, p, "right"))  { right = 1; p += 5; }
			else break;
		}
		if (top && right)      *deg = 45;
		else if (bot && right) *deg = 135;
		else if (bot && left)  *deg = 225;
		else if (top && left)  *deg = 315;
		else if (top)          *deg = 0;
		else if (right)        *deg = 90;
		else if (bot)          *deg = 180;
		else if (left)         *deg = 270;
		else return 0;
		return 1;
	}
	/* An angle: <number>[deg|turn|grad|rad], bare number = deg. Only if it
	 * starts with a digit or sign and is followed by an angle unit or end of
	 * the argument (a color never looks like this). */
	if (a < end && (is_digit(v[a]) || v[a] == '-' || v[a] == '+' ||
			(v[a] == '.' && a + 1 < end && is_digit(v[a+1])))) {
		size_t c = a;
		long milli = parse_num_milli(v, a, end, &c);
		long d;
		while (c < end && is_space(v[c])) c++;
		if (match_ci(v, end, c, "turn"))      d = milli * 360 / 1000;
		else if (match_ci(v, end, c, "grad")) d = milli * 9 / 10 / 1000;
		else if (match_ci(v, end, c, "rad"))  d = milli * 180 / 3142 ; /* /pi, milli cancels ~ */
		else                                  d = milli / 1000;         /* deg (or bare) */
		/* normalise to [0,360) */
		d %= 360; if (d < 0) d += 360;
		*deg = (int) d;
		return 1;
	}
	return 0;
}

/* Split gradient arguments on top-level commas and fill one gradient.
 * `inner` is the text between the outer parentheses. Returns the table index,
 * or -1 on failure / table full. */
static int parse_gradient_body(int type, const char *inner, size_t len)
{
	cssvar_gradient g;
	size_t i = 0;
	size_t args[16]; size_t arge[16]; int na = 0;
	int stopi = 0;

	if (g_grad_n >= CV_MAX_GRADS) return -1;

	/* split into top-level comma-separated arguments */
	{
		size_t start = 0; int depth = 0;
		for (i = 0; i <= len; i++) {
			if (i == len || (depth == 0 && inner[i] == ',')) {
				if (na < 16) {
					size_t a = start, b = i;
					while (a < b && is_space(inner[a])) a++;
					while (b > a && is_space(inner[b-1])) b--;
					args[na] = a; arge[na] = b; na++;
				}
				start = i + 1;
			} else if (inner[i] == '(') depth++;
			else if (inner[i] == ')') { if (depth > 0) depth--; }
		}
	}
	if (na < 2) return -1;   /* need at least a direction/first-stop + one more */

	g.type = (unsigned char) type;
	g.shape = 0;             /* ellipse default */
	g.angle = 180;           /* linear default: to bottom */
	g.nstops = 0;

	{
		int firstarg_is_stop = 1;
		int ai = 0;
		if (type == 0) {     /* linear: first arg may be a direction */
			int deg;
			if (parse_direction(inner, args[0], arge[0], &deg)) {
				g.angle = deg; ai = 1; firstarg_is_stop = 0;
			}
		} else {             /* radial: first arg may be shape/size/position */
			size_t p = args[0];
			unsigned int probe; size_t pc;
			/* If the first arg parses as a color it is a stop, else it is the
			 * shape/position preamble which we read for `circle`/`ellipse`. */
			if (!parse_color(inner, p, arge[0], &probe, &pc)) {
				size_t q;
				for (q = args[0]; q < arge[0]; q++) {
					if (match_ci(inner, arge[0], q, "circle")) { g.shape = 1; break; }
					if (match_ci(inner, arge[0], q, "ellipse")) { g.shape = 0; break; }
				}
				ai = 1; firstarg_is_stop = 0;
			}
		}
		(void) firstarg_is_stop;

		/* remaining args are color stops */
		for (; ai < na && stopi < CSSVAR_GRAD_MAX_STOPS; ai++) {
			unsigned int col; size_t c;
			if (!parse_color(inner, args[ai], arge[ai], &col, &c)) continue;
			g.stop_col[stopi] = col;
			g.stop_pos[stopi] = -1;
			/* optional position: a percentage after the color */
			while (c < arge[ai] && is_space(inner[c])) c++;
			if (c < arge[ai] && (is_digit(inner[c]) || inner[c] == '-' ||
					inner[c] == '.')) {
				size_t pc;
				long m = parse_num_milli(inner, c, arge[ai], &pc);
				while (pc < arge[ai] && is_space(inner[pc])) pc++;
				if (pc < arge[ai] && inner[pc] == '%') {
					long permille = m / 100;   /* percent(milli) -> per-mille */
					if (permille < 0) permille = 0;
					if (permille > 1000) permille = 1000;
					g.stop_pos[stopi] = (int) permille;
				}
			}
			stopi++;
		}
	}

	g.nstops = stopi;
	if (g.nstops < 2) return -1;   /* a gradient needs at least two stops */

	g_grads[g_grad_n] = g;
	return g_grad_n++;
}

/* If v[vs..ve) is (or contains) a bounded gradient function, parse it, append
 * to the table, and return its index; else -1. Only the FIRST gradient in the
 * value is taken (layered backgrounds are deferred). Repeating and conic
 * gradients are deliberately NOT matched, so they fall through unchanged and
 * libcss drops them exactly as today. */
static int gradient_from_value(const char *v, size_t vs, size_t ve)
{
	size_t i = vs;
	int type = -1;
	size_t fn = 0;
	for (i = vs; i < ve; i++) {
		int identboundary = (i == vs || !is_ident(v[i-1]));
		if (!identboundary) continue;
		if (match_ci(v, ve, i, "linear-gradient(")) { type = 0; fn = i + 15; break; }
		if (match_ci(v, ve, i, "radial-gradient(")) { type = 1; fn = i + 15; break; }
	}
	if (type < 0) return -1;
	/* fn points at the '(' */
	{
		size_t open = fn;
		size_t close = match_paren(v, ve, open);
		if (close <= open + 1 || close > ve) return -1;
		return parse_gradient_body(type, v + open + 1, close - open - 1);
	}
}

static void grid_rewrite(const char *s, size_t len, sbuf *out)
{
	size_t i = 0;
	int depth = 0;

	while (i < len) {
		char c = s[i];
		if (c == '"' || c == '\'') {
			size_t e = skip_string(s, len, i);
			sb_putn(out, s + i, e - i);
			i = e;
			continue;
		}
		if (c == '{') { depth++; sb_putc(out, c); i++; continue; }
		if (c == '}') { if (depth > 0) depth--; sb_putc(out, c); i++; continue; }

		if (depth > 0 && (i == 0 || !is_ident(s[i - 1]))) {
			/*
			 * #245 enggrad: a `background` shorthand or `background-image`
			 * whose value is a gradient function is parsed into the table and
			 * rewritten onto the outline-color carrier (0xC5<index>). Any
			 * other background value (url, solid color, none) is left exactly
			 * as-is so the stream is byte-identical (AE=0). background-color,
			 * -position, -repeat and the other longhands start with
			 * "background-" and are excluded here.
			 */
			int is_bg_img = match_ci(s, len, i, "background-image");
			int is_bg_sh  = !is_bg_img && match_ci(s, len, i, "background") &&
					(i + 10 >= len || !is_ident(s[i + 10]));
			if (is_bg_img || is_bg_sh) {
				size_t k = i, vs, ve, vss;
				while (k < len && s[k] != ':' && s[k] != ';' && s[k] != '}') k++;
				if (k < len && s[k] == ':') {
					vs = k + 1;
					{
						int pd = 0; size_t j = vs;
						while (j < len) {
							if (s[j] == '(') pd++;
							else if (s[j] == ')') { if (pd > 0) pd--; }
							else if (pd == 0 && (s[j] == ';' || s[j] == '}')) break;
							j++;
						}
						ve = j;
					}
					vss = vs;
					while (vss < ve && is_space(s[vss])) vss++;
					{
						int gi = gradient_from_value(s, vss, ve);
						if (gi >= 0) {
							unsigned int cval = 0xC50000u |
								(unsigned) (gi & 0xFFFF);
							int sh;
							static const char hx[] = "0123456789ABCDEF";
							sb_puts(out, "outline-color:#");
							for (sh = 20; sh >= 0; sh -= 4)
								sb_putc(out, hx[(cval >> sh) & 0xF]);
							i = ve;
							continue;
						}
					}
				}
				/* not a gradient: fall through and emit verbatim */
			}
			int is_gtc = match_ci(s, len, i, "grid-template-columns");
			int is_gap = match_ci(s, len, i, "gap") || match_ci(s, len, i, "grid-gap");
			/* #245 enggap: the row-gap LONGHAND, which libcss drops
			 * entirely, onto the clip carrier. Checked separately from
			 * is_gap: match_ci anchors at a property-name start, so
			 * "row-gap" never matched "gap" and the two cannot collide. */
			int is_rgap = match_ci(s, len, i, "row-gap") ||
					match_ci(s, len, i, "grid-row-gap");
			/* Only the shorthand. The per-corner longhands
			 * (border-top-left-radius and friends) are left alone:
			 * there is one carrier and four of them, so taking the
			 * last one seen would be arbitrary. */
			int is_rad = match_ci(s, len, i, "border-radius");
			if (is_gtc || is_gap || is_rad || is_rgap) {
				size_t k = i;
				size_t vs, ve;
				while (k < len && s[k] != ':' && s[k] != ';' && s[k] != '}') k++;
				if (k >= len || s[k] != ':') { sb_putc(out, c); i++; continue; }
				vs = k + 1;
				{
					int pd = 0;
					size_t j = vs;
					while (j < len) {
						if (s[j] == '(') pd++;
						else if (s[j] == ')') { if (pd > 0) pd--; }
						else if (pd == 0 && (s[j] == ';' || s[j] == '}')) break;
						j++;
					}
					ve = j;
				}
				while (vs < ve && is_space(s[vs])) vs++;
				{
					size_t e2 = ve;
					while (e2 > vs && is_space(s[e2 - 1])) e2--;
					ve = e2;
				}
				if (is_rad) {
					/* First component only; a `/` starts the
					 * vertical radii, which we do not carry. */
					size_t a = vs, b = ve, j;
					int pct = 0;
					for (j = vs; j < ve; j++) {
						if (is_space(s[j]) || s[j] == '/') break;
						if (s[j] == '%') pct = 1;
					}
					b = j;
					sb_puts(out, "column-rule-width:");
					if (pct) {
						/* layout clamps to min(w,h)/2, which is
						 * what a percentage radius means here */
						sb_puts(out, "9999px");
					} else if (b > a) {
						sb_putn(out, s + a, b - a);
					} else {
						sb_puts(out, "0px");
					}
					/* libcss computes a rule width of 0 when the rule
					 * style is none, and none is the initial value, so
					 * the width would be discarded without this. */
					sb_puts(out, ";column-rule-style:solid");
				} else if (is_gtc) {
					long cnt = 0, minpx = 0;
					if (grid_tracks(s + vs, ve - vs, &cnt, &minpx)) {
						if (cnt > 0) { sb_puts(out, "column-count:"); sb_puti(out, cnt); }
						else { sb_puts(out, "column-width:"); sb_puti(out, minpx); sb_puts(out, "px"); }
					} else {
						sb_puts(out, "column-count:1");
					}
				} else if (is_rgap) {
					/* The row-gap longhand sets only the cross axis,
					 * so it emits the carrier and NO column-gap. */
					emit_row_gap(out, s, vs, ve, 0);
				} else {
					/* `gap: <row> <column>`; one value sets both. The
					 * COLUMN component goes to column-gap, which libcss
					 * has, and the ROW component to the clip carrier
					 * (#245 enggap). Before that, only the column
					 * component survived and layout spent it on both
					 * axes. */
					size_t a = vs, b = ve, sp = 0, j;
					for (j = vs; j < ve; j++) if (is_space(s[j])) sp = j;
					if (sp) { a = sp + 1; while (a < b && is_space(s[a])) a++; }
					sb_puts(out, "column-gap:");
					sb_putn(out, s + a, b - a);
					/* The row component is everything up to the LAST
					 * space, or the whole value when there is none, so a
					 * one-value gap still sets both axes to it. */
					emit_row_gap(out, s, vs, sp ? sp : ve, 1);
				}
				i = ve;
				continue;
			}
		}
		sb_putc(out, c);
		i++;
	}
}

/* ------------------------------------------------------------------ */
/* entry point                                                        */
/* ------------------------------------------------------------------ */
char *cssvar_preprocess(const char *src, size_t len,
		int viewport_w, int viewport_h, size_t *out_len)
{
	static cvtab tab;   /* 300KB; a page load is single-threaded here */
	sbuf a, b;

	if (out_len) *out_len = 0;
	if (!src) return NULL;
	if (viewport_w <= 0) viewport_w = 1024;
	if (viewport_h <= 0) viewport_h = 768;

	tab.n = 0;
	collect_vars(src, len, &tab);

	sb_init(&a);
	substitute(src, len, &tab, &a);
	if (a.oom) { free(a.p); return NULL; }

	sb_init(&b);
	eval_math(a.p ? a.p : "", a.n, viewport_w, viewport_h, &b);
	free(a.p);
	if (b.oom) { free(b.p); return NULL; }

	{
		sbuf g;
		sb_init(&g);
		grid_rewrite(b.p ? b.p : "", b.n, &g);
		free(b.p);
		if (g.oom) { free(g.p); return NULL; }
		b = g;
	}

	if (!b.p) {
		b.p = malloc(1);
		if (!b.p) return NULL;
		b.p[0] = '\0';
		b.n = 0;
	}
	if (out_len) *out_len = b.n;
	return b.p;
}

/* ------------------------------------------------------------------ */
/* self-test                                                          */
/* ------------------------------------------------------------------ */

/* Does `hay` contain `needle`? */
static int has(const char *hay, const char *needle)
{
	return hay && needle && strstr(hay, needle) != NULL;
}

struct cvcase {
	const char *name;
	const char *in;
	int vw, vh;
	const char *want;      /* must be present, or NULL */
	const char *want_not;  /* must be absent, or NULL */
};

int cssvar_selftest(void (*report)(const char *line))
{
	static const struct cvcase cases[] = {
	  { "root palette substitutes",
	    ":root{--bg:#0c1312}body{background:var(--bg)}", 800, 600,
	    "background:#0c1312", "var(" },
	  { "nested var resolves",
	    ":root{--a:#ffffff;--b:var(--a)}p{color:var(--b)}", 800, 600,
	    "color:#ffffff", "var(" },
	  { "fallback used when undefined",
	    "a{color:var(--nope,#123456)}", 800, 600,
	    "color:#123456", "var(" },
	  { "undefined with no fallback leaves an invalid declaration",
	    "a{color:var(--nope)}", 800, 600,
	    "color:}", "#" },
	  { "non-global scope is NOT collected",
	    ".card{--y:red}p{color:var(--y)}", 800, 600,
	    "color:}", "red" },
	  { "at-rule scope is NOT collected",
	    "@media (min-width:900px){:root{--z:red}}p{color:var(--z)}", 800, 600,
	    "color:}", "red" },
	  { "clamp picks the middle",
	    ".s{padding:clamp(1rem,4vw,2.2rem)}", 776, 600,
	    "padding:31px", "clamp" },
	  { "clamp picks the lower bound",
	    ".s{padding:clamp(1rem,4vw,2.2rem)}", 200, 600,
	    "padding:16px", "clamp" },
	  { "clamp picks the upper bound",
	    ".s{padding:clamp(1rem,4vw,2.2rem)}", 2000, 600,
	    "padding:35px", "clamp" },
	  { "min-height is a property, not the min() function",
	    ".t{min-height:3.2rem}", 800, 600,
	    "min-height:3.2rem", "px}" },
	  { "max() of two lengths",
	    ".s{width:max(10px,3rem)}", 800, 600,
	    "width:48px", "max(" },
	  { "calc with compatible units",
	    ".s{margin:calc(2rem + 4px)}", 800, 600,
	    "margin:36px", "calc" },
	  { "calc with a percentage is left alone",
	    ".s{width:calc(100% - 20px)}", 800, 600,
	    "calc(100% - 20px)", NULL },
	  { "two clamps in one declaration",
	    ".s{padding:clamp(1rem,4vw,2.2rem) clamp(1rem,2vw,2rem)}", 776, 600,
	    "padding:31px 16px", "clamp" },
	  { "custom property declarations are removed",
	    ":root{--bg:#000000;--fg:#ffffff}", 800, 600,
	    ":root{}", "--bg" },
	  { "a comment containing a brace does not break block tracking",
	    ":root{--c:#abcdef}/* } */ p{color:var(--c)}", 800, 600,
	    "color:#abcdef", "var(" },
	  { "a string containing var( is not substituted",
	    "p{content:\"var(--x)\"}", 800, 600,
	    "\"var(--x)\"", NULL },
	  { "var inside a function argument",
	    ":root{--l:#24403d}p{border:1px solid var(--l)}", 800, 600,
	    "solid #24403d", "var(" },
	  { "font stack through a var",
	    ":root{--m:\"IBM Plex Mono\",monospace}code{font-family:var(--m)}", 800, 600,
	    "font-family:\"IBM Plex Mono\",monospace", "var(" },
	  { "vw resolves against the given viewport",
	    ".s{font-size:clamp(1rem,5vw,3rem)}", 640, 480,
	    "font-size:32px", "clamp" },
	  { "auto-fill grid becomes a minimum column width",
	    ".g{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr))}",
	    800, 600, "column-width:280px", "grid-template" },
	  { "repeat(N) grid becomes a column count",
	    ".g{grid-template-columns:repeat(3,1fr)}", 800, 600,
	    "column-count:3", "grid-template" },
	  { "an explicit track list is counted",
	    ".g{grid-template-columns:minmax(0,10fr) minmax(0,9fr)}", 800, 600,
	    "column-count:2", "grid-template" },
	  { "gap becomes column-gap",
	    ".g{gap:1rem}", 800, 600, "column-gap:1rem", "grid-template" },
	  { "a one-value gap also carries the SAME row gap",
	    ".g{gap:1rem}", 800, 600, "clip:rect(0px,0px,1rem,0px)", NULL },
	  { "gap with two values takes the column component",
	    ".g{gap:2rem 12px}", 800, 600, "column-gap:12px", NULL },
	  { "gap with two values carries the ROW component separately",
	    ".g{gap:2rem 12px}", 800, 600, "clip:rect(0px,0px,2rem,0px)", NULL },
	  { "row-gap is not mistaken for gap",
	    ".g{row-gap:4px}", 800, 600, "clip:rect(0px,0px,4px,0px)", "column-gap" },
	  { "the row-gap longhand survives as the carrier, not verbatim",
	    ".g{row-gap:4px}", 800, 600, NULL, "row-gap:4px" },
	  { "grid-row-gap is carried too",
	    ".g{grid-row-gap:9px}", 800, 600, "clip:rect(0px,0px,9px,0px)", NULL },
	  { "the carrier rides the THIRD rect component, never the first",
	    ".g{row-gap:4px}", 800, 600, NULL, "rect(4px" },
	  { "a percentage row gap is dropped, not carried",
	    ".g{row-gap:10%}", 800, 600, NULL, "clip:rect" },
	  { "a bare column-gap opens no row gap",
	    ".g{column-gap:7px}", 800, 600, "column-gap:7px", "clip:rect" },
	  { "a clamped gap resolves before it is carried",
	    ".g{gap:clamp(1rem,4vw,2rem)}", 776, 600, "column-gap:31px", "clamp" },
	  { "border-radius rides column-rule-width",
	    ".c{border-radius:10px}", 800, 600,
	    "column-rule-width:10px", "border-radius" },
	  { "the carried radius arms the rule style, or libcss drops the width",
	    ".c{border-radius:6px}", 800, 600, "column-rule-style:solid", NULL },
	  { "a radius through a var is resolved first",
	    ":root{--rad:10px}.c{border-radius:var(--rad)}", 800, 600,
	    "column-rule-width:10px", "var(" },
	  { "only the first radius component is carried",
	    ".c{border-radius:4px 4px 0 0}", 800, 600,
	    "column-rule-width:4px", "0 0" },
	  { "the elliptical slash form takes the horizontal radius",
	    ".c{border-radius:12px/6px}", 800, 600,
	    "column-rule-width:12px", "/6px" },
	  { "a percentage radius becomes the clamp-to-half sentinel",
	    ".c{border-radius:50%}", 800, 600,
	    "column-rule-width:9999px", "50%" },
	  { "a pill radius survives as a large length",
	    ".p{border-radius:999px}", 800, 600,
	    "column-rule-width:999px", "border-radius" },
	  { "the per-corner longhand is left alone, not half-carried",
	    ".c{border-top-left-radius:8px}", 800, 600,
	    "border-top-left-radius:8px", "column-rule-width" },
	  /* #245 enggrad: gradient shorthand/image rides the outline-color carrier */
	  { "a linear-gradient background becomes the outline-color carrier",
	    ".g{background:linear-gradient(to right,#f00,#00f)}", 800, 600,
	    "outline-color:#C5", "linear-gradient" },
	  { "background-image radial-gradient rides the same carrier",
	    ".g{background-image:radial-gradient(circle,#fff,#000)}", 800, 600,
	    "outline-color:#C5", "radial-gradient" },
	  { "a plain background color is NOT touched (no false carrier)",
	    ".g{background:#123456}", 800, 600,
	    "background:#123456", "outline-color" },
	  { "a background url is NOT touched",
	    ".g{background:url(x.png) no-repeat}", 800, 600,
	    "url(x.png)", "outline-color" },
	  { "background-color longhand is NOT mistaken for the shorthand",
	    ".g{background-color:linear-gradient(#f00,#00f)}", 800, 600,
	    "background-color", "outline-color" },
	};
	int n = (int) (sizeof(cases) / sizeof(cases[0]));
	int fails = 0, i;

	for (i = 0; i < n; i++) {
		size_t ol = 0;
		char *got = cssvar_preprocess(cases[i].in, strlen(cases[i].in),
				cases[i].vw, cases[i].vh, &ol);
		int ok = (got != NULL);
		if (ok && cases[i].want && !has(got, cases[i].want)) ok = 0;
		if (ok && cases[i].want_not && has(got, cases[i].want_not)) ok = 0;
		if (!ok) fails++;
		if (report) {
			char line[640];
			size_t k = 0;
			const char *tag = ok ? "PASS " : "FAIL ";
			while (tag[k] && k < 8) { line[k] = tag[k]; k++; }
			{
				const char *nm = cases[i].name;
				size_t j = 0;
				while (nm[j] && k < 260) line[k++] = nm[j++];
			}
			line[k++] = ' '; line[k++] = '=';line[k++] = '>'; line[k++] = ' ';
			{
				const char *g = got ? got : "(null)";
				size_t j = 0;
				while (g[j] && k < 600) line[k++] = g[j++];
			}
			line[k] = '\0';
			report(line);
		}
		free(got);
	}
	return fails;
}
