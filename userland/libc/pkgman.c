// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// pkgman.c - INSTALL manifest resolution: exact rules, TREE rules, fan-out.
// See pkgman.h for the grammar, for why the payload representation did NOT need
// to change, and for which layer owns which guarantee.
//
// Self-contained: no string.h, no syscalls, no allocation, so the host test
// (tests/run_pkgman.sh) compiles this exact translation unit.
#include "pkgman.h"

static unsigned long pm_len(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

// ---------------------------------------------------------------------------
// Member name safety.
//
// A TREE rule appends the member's own tail to a destination prefix, so the
// member name becomes an input to the resulting path. arc_path_is_safe() in the
// archiver already refuses these, and the extractor calls it before any sink
// ever sees a member; this is the SECOND, independent check, in the translation
// unit that does the concatenating. Two copies of a containment rule is
// normally the thing to avoid, but the alternative here is a concatenation
// whose safety is a property of a different file that a future caller can
// forget to route through.
// ---------------------------------------------------------------------------
static int pm_member_ok(const char *n) {
    if (!n || !n[0]) return 0;
    if (n[0] == '/') return 0;                    // absolute
    unsigned long i = 0;
    for (; n[i]; i++) {
        unsigned char c = (unsigned char)n[i];
        if (c < 0x20 || c == 0x7F) return 0;      // control bytes
        if (c == '\\') return 0;                  // no alternate separator games
        if (c == ':') return 0;                   // no drive/volume prefixes
    }
    if (i >= PKGMAN_DEST_MAX) return 0;           // bounded
    const char *p = n;
    for (;;) {                                    // no ".." component anywhere
        const char *q = p;
        while (*q && *q != '/') q++;
        if ((unsigned long)(q - p) == 2 && p[0] == '.' && p[1] == '.') return 0;
        if (!*q) break;
        p = q + 1;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// One parsed manifest rule.
// ---------------------------------------------------------------------------
typedef struct {
    char src[PKGMAN_SRC_MAX];
    char dst[PKGMAN_DEST_MAX];
    unsigned long srclen;
    unsigned long dstlen;
    int  is_tree;
} pm_rule;

// Pull the next line out of man[*p .. mlen). Returns 0 at end of text, 1 when a
// line was produced, PKGMAN_E_RULE when the line is longer than PKGMAN_LINE_MAX
// (refused, never truncated: a truncated rule is a different rule).
static int pm_next_line(const char *man, unsigned long mlen, unsigned long *p,
                        char *line, unsigned long cap) {
    if (*p >= mlen) return 0;
    unsigned long ll = 0;
    while (*p < mlen && man[*p] != '\n') {
        if (ll + 1 >= cap) {
            // Consume the rest of the over-long line so the caller's line
            // numbering stays right, then report it.
            while (*p < mlen && man[*p] != '\n') (*p)++;
            if (*p < mlen) (*p)++;
            return PKGMAN_E_RULE;
        }
        line[ll++] = man[*p];
        (*p)++;
    }
    if (*p < mlen) (*p)++;            // step over the '\n'
    // Tolerate CRLF manifests and trailing blanks.
    while (ll > 0 && (line[ll - 1] == '\r' || line[ll - 1] == ' ' ||
                      line[ll - 1] == '\t')) ll--;
    line[ll] = 0;
    return 1;
}

// Parse one line into a rule.
// Returns 1 = a rule, 0 = blank or comment (not a rule, not an error),
// PKGMAN_E_RULE = malformed.
//
// A line with no "->" is NOT treated as an error: the shipping INSTALL files
// carry long prose comment blocks, and tolerating a stray non-rule line has
// always been this parser's behaviour. What IS now an error is a line that
// clearly means to be a rule and cannot be honoured exactly, because the failure
// mode of guessing is a file written somewhere other than where the package
// said.
static int pm_parse(const char *line, pm_rule *r) {
    if (line[0] == 0 || line[0] == '#') return 0;

    const char *arrow = 0;
    for (const char *s = line; s[0] && s[1]; s++)
        if (s[0] == '-' && s[1] == '>') { arrow = s; break; }
    if (!arrow) return 0;

    // Left-hand side: skip leading blanks, then take up to the first blank.
    const char *s = line;
    while (s < arrow && (*s == ' ' || *s == '\t')) s++;
    unsigned long si = 0;
    while (s < arrow && *s != ' ' && *s != '\t') {
        if (si + 1 >= PKGMAN_SRC_MAX) return PKGMAN_E_RULE;   // refuse, never truncate
        r->src[si++] = *s++;
    }
    r->src[si] = 0;
    // Anything other than blanks between the source token and the arrow means
    // the line is not the shape it looks like.
    while (s < arrow && (*s == ' ' || *s == '\t')) s++;
    if (s != arrow) return PKGMAN_E_RULE;
    if (si == 0) return PKGMAN_E_RULE;

    // Right-hand side.
    const char *d = arrow + 2;
    while (*d == ' ' || *d == '\t') d++;
    unsigned long di = 0;
    while (*d && *d != ' ' && *d != '\t') {
        if (di + 1 >= PKGMAN_DEST_MAX) return PKGMAN_E_RULE;
        r->dst[di++] = *d++;
    }
    r->dst[di] = 0;
    while (*d == ' ' || *d == '\t') d++;
    if (*d) return PKGMAN_E_RULE;                 // trailing junk
    if (di == 0) return PKGMAN_E_RULE;
    if (r->dst[0] != '/') return PKGMAN_E_RULE;   // destinations are absolute

    int src_slash = (si > 0 && r->src[si - 1] == '/');
    int dst_slash = (di > 0 && r->dst[di - 1] == '/');
    if (src_slash != dst_slash) return PKGMAN_E_RULE;   // half-a-tree-rule

    if (src_slash) {
        // TREE rule. Strip the marker slashes; what is kept is the prefix.
        si--; r->src[si] = 0;
        di--; r->dst[di] = 0;
        // A bare-root tree rule would map every member including INSTALL.
        if (si == 0 || di == 0) return PKGMAN_E_RULE;
        r->is_tree = 1;
    } else {
        r->is_tree = 0;
    }
    // A source that is itself unusable as a relative path is a bad rule, not a
    // rule that simply never matches. Saying so up front is the difference
    // between a named refusal and a package that installs nothing and claims
    // success.
    if (!pm_member_ok(r->src)) return PKGMAN_E_RULE;
    if (!pm_member_ok(r->dst + 1)) return PKGMAN_E_RULE;   // skip the leading '/'
    r->srclen = si;
    r->dstlen = di;
    return 1;
}

int pkgman_check_manifest(const char *man, unsigned long mlen, int *out_line) {
    if (out_line) *out_line = 0;
    if (!man) return PKGMAN_E_ARG;
    char line[PKGMAN_LINE_MAX];
    unsigned long p = 0;
    int lineno = 0;
    for (;;) {
        int lr = pm_next_line(man, mlen, &p, line, sizeof(line));
        if (lr == 0) break;
        lineno++;
        if (lr < 0) { if (out_line) *out_line = lineno; return PKGMAN_E_RULE; }
        pm_rule r;
        if (pm_parse(line, &r) < 0) {
            if (out_line) *out_line = lineno;
            return PKGMAN_E_RULE;
        }
    }
    return 0;
}

// Copy `n` bytes of src plus a NUL into dests[slot], bounds-checked.
static int pm_emit(char dests[][PKGMAN_DEST_MAX], int slot, int max_dests,
                   const char *a, unsigned long alen,
                   const char *b, unsigned long blen) {
    if (slot >= max_dests) return PKGMAN_E_TOOMANY;
    // a + optional ('/' + b) + NUL
    unsigned long need = alen + (b ? 1 + blen : 0) + 1;
    if (need > PKGMAN_DEST_MAX) return PKGMAN_E_TOOLONG;
    unsigned long o = 0;
    for (unsigned long i = 0; i < alen; i++) dests[slot][o++] = a[i];
    if (b) {
        dests[slot][o++] = '/';
        for (unsigned long i = 0; i < blen; i++) dests[slot][o++] = b[i];
    }
    dests[slot][o] = 0;
    return 0;
}

int pkgman_dest_all(const char *man, unsigned long mlen,
                    const char *pkgid, const char *member,
                    char dests[][PKGMAN_DEST_MAX], int max_dests) {
    if (!man || !pkgid || !member || !dests || max_dests < 1) return PKGMAN_E_ARG;

    // The member must live under "<pkgid>/". Checked before the name-safety
    // test so that an archive carrying unrelated members is simply skipped.
    unsigned long idl = pm_len(pkgid);
    for (unsigned long k = 0; k < idl; k++)
        if (member[k] != pkgid[k]) return PKGMAN_UNREFERENCED;
    if (member[idl] != '/') return PKGMAN_UNREFERENCED;
    const char *rel = member + idl + 1;

    // Only now, with the member established as ours, is a bad name an error
    // rather than something to ignore.
    if (!pm_member_ok(rel)) return PKGMAN_E_MEMBER;
    unsigned long rell = pm_len(rel);

    char line[PKGMAN_LINE_MAX];
    int n = 0;

    // ---- pass 1: EXACT rules win, and ALL of them apply ------------------
    unsigned long p = 0;
    for (;;) {
        int lr = pm_next_line(man, mlen, &p, line, sizeof(line));
        if (lr == 0) break;
        if (lr < 0) return PKGMAN_E_RULE;
        pm_rule r;
        int pr = pm_parse(line, &r);
        if (pr < 0) return PKGMAN_E_RULE;
        if (pr == 0 || r.is_tree) continue;
        if (r.srclen != rell) continue;
        unsigned long i = 0;
        for (; i < rell; i++) if (r.src[i] != rel[i]) break;
        if (i != rell) continue;
        int er = pm_emit(dests, n, max_dests, r.dst, r.dstlen, 0, 0);
        if (er < 0) return er;
        n++;
    }
    if (n > 0) return n;

    // ---- pass 2: TREE rules. The LONGEST matching prefix wins; every rule
    // at that same length applies (so a tree can be fanned out too). Two
    // passes over the manifest rather than one, because "longest" is not known
    // until the whole manifest has been read and the manifest is a few KB.
    unsigned long bestlen = 0;
    int have_best = 0;
    p = 0;
    for (;;) {
        int lr = pm_next_line(man, mlen, &p, line, sizeof(line));
        if (lr == 0) break;
        if (lr < 0) return PKGMAN_E_RULE;
        pm_rule r;
        int pr = pm_parse(line, &r);
        if (pr < 0) return PKGMAN_E_RULE;
        if (pr == 0 || !r.is_tree) continue;
        // rel must be "<src>/<something>". Requiring the '/' is what makes
        // "packages/" fail to match a member literally named "packagesX/y",
        // i.e. the prefix match is on COMPONENT boundaries, not on characters.
        if (rell <= r.srclen + 1) continue;
        if (rel[r.srclen] != '/') continue;
        unsigned long i = 0;
        for (; i < r.srclen; i++) if (r.src[i] != rel[i]) break;
        if (i != r.srclen) continue;
        if (!have_best || r.srclen > bestlen) { bestlen = r.srclen; have_best = 1; }
    }
    if (!have_best) return PKGMAN_UNREFERENCED;

    const char *tail = rel + bestlen + 1;         // past "<src>/"
    unsigned long taill = pm_len(tail);
    if (taill == 0) return PKGMAN_UNREFERENCED;   // the directory entry itself

    p = 0;
    for (;;) {
        int lr = pm_next_line(man, mlen, &p, line, sizeof(line));
        if (lr == 0) break;
        if (lr < 0) return PKGMAN_E_RULE;
        pm_rule r;
        int pr = pm_parse(line, &r);
        if (pr < 0) return PKGMAN_E_RULE;
        if (pr == 0 || !r.is_tree || r.srclen != bestlen) continue;
        if (rel[r.srclen] != '/') continue;
        unsigned long i = 0;
        for (; i < r.srclen; i++) if (r.src[i] != rel[i]) break;
        if (i != r.srclen) continue;
        int er = pm_emit(dests, n, max_dests, r.dst, r.dstlen, tail, taill);
        if (er < 0) return er;
        n++;
    }
    return n > 0 ? n : PKGMAN_UNREFERENCED;
}

// ---------------------------------------------------------------------------
// "#!mpkg-min-client <n>": the one directive the manifest format has.
//
// Parsed here rather than by the caller because the caller would have to
// re-implement line splitting, and a second line splitter is a second thing to
// disagree with this one about what a line is (CRLF, trailing blanks, an
// over-long line). pm_next_line() is the one answer.
//
// The directive must be the WHOLE line after the marker, leading blanks
// allowed, so "#!mpkg-min-client 2 or later" is PKGMAN_E_RULE and not a silent
// 2. A requirement that is not exactly readable is not a requirement that has
// been read.
// ---------------------------------------------------------------------------
#define PM_DIRECTIVE     "#!mpkg-min-client"
#define PM_DIRECTIVE_LEN  17
#define PM_GEN_MAX        99          // a plausible generation; above it, refuse

int pkgman_min_client(const char *man, unsigned long mlen) {
    if (!man) return PKGMAN_E_ARG;
    unsigned long p = 0;
    char line[PKGMAN_LINE_MAX];
    int found = 1;                    // no directive == generation 1
    for (;;) {
        int lr = pm_next_line(man, mlen, &p, line, sizeof(line));
        if (lr == 0) break;
        if (lr < 0) return PKGMAN_E_RULE;
        const char *q = line;
        while (*q == ' ' || *q == '\t') q++;
        unsigned long i = 0;
        for (; i < PM_DIRECTIVE_LEN; i++)
            if (q[i] != PM_DIRECTIVE[i]) break;
        if (i != PM_DIRECTIVE_LEN) continue;
        q += PM_DIRECTIVE_LEN;
        if (*q != ' ' && *q != '\t') return PKGMAN_E_RULE;  // "#!mpkg-min-clientX"
        while (*q == ' ' || *q == '\t') q++;
        if (*q < '0' || *q > '9') return PKGMAN_E_RULE;
        int v = 0;
        while (*q >= '0' && *q <= '9') {
            v = v * 10 + (*q - '0');
            if (v > PM_GEN_MAX) return PKGMAN_E_RULE;
            q++;
        }
        while (*q == ' ' || *q == '\t') q++;
        if (*q != 0) return PKGMAN_E_RULE;                   // trailing junk
        if (v < 1) return PKGMAN_E_RULE;
        // A SECOND, DIFFERENT declaration is refused rather than resolved by a
        // rule ("first wins", "highest wins"): two answers to one question in
        // one file is a package nobody can review.
        if (found != 1 && found != v) return PKGMAN_E_RULE;
        found = v;
    }
    return found;
}

int pkgman_dest_for(const char *man, unsigned long mlen,
                    const char *pkgid, const char *member,
                    char *dest, unsigned long cap) {
    if (!dest || cap < 2) return PKGMAN_E_ARG;
    dest[0] = 0;
    // Asks for the FULL fan-out and keeps the first, rather than passing
    // max_dests=1: a member that legitimately fans out would otherwise come
    // back PKGMAN_E_TOOMANY from a call that only ever wanted one answer.
    char d[PKGMAN_MAX_DESTS][PKGMAN_DEST_MAX];
    int rc = pkgman_dest_all(man, mlen, pkgid, member, d, PKGMAN_MAX_DESTS);
    if (rc <= 0) return rc;                       // UNREFERENCED or negative
    unsigned long l = pm_len(d[0]);
    if (l + 1 > cap) return PKGMAN_E_TOOLONG;
    for (unsigned long i = 0; i <= l; i++) dest[i] = d[0][i];
    return PKGMAN_MAPPED;
}

const char *pkgman_strerror(int rc) {
    switch (rc) {
    case PKGMAN_UNREFERENCED: return "not named by the INSTALL manifest";
    case PKGMAN_E_ARG:        return "bad argument";
    case PKGMAN_E_MEMBER:     return "unsafe archive member name";
    case PKGMAN_E_RULE:       return "malformed INSTALL rule";
    case PKGMAN_E_TOOLONG:    return "destination path too long";
    case PKGMAN_E_TOOMANY:    return "too many destinations for one file";
    case PKGMAN_E_NEWER:      return "this package needs a newer MayteraOS";
    default:                  return rc > 0 ? "ok" : "unknown error";
    }
}
