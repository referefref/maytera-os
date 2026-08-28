// tr - translate, squeeze and delete characters.
//
// ============================================================================
// WHAT THIS REPLACED, AND WHY IT MATTERED (#745 local 108)
// ============================================================================
//
// The previous /APPS/TR was 36 lines. It walked SET1 one character at a time
// and compared each input byte against those literals, so `a`, `-` and `z` in
// `tr a-z A-Z` were THREE LITERAL CHARACTERS: an input byte only changed if it
// was literally 'a', '-' or 'z'. The single most-typed tr idiom in existence
// copied its input through unchanged and exited 0. There were no ranges, no
// [:class:] expressions, and no -d, -s, -c or -t; the options were not
// rejected either, they became SET1.
//
// That is the defect class this ticket is about: a tool named after a standard
// utility that implements a fraction and REPORTS NO ERROR. A tool that refuses
// a feature is debuggable in one command; a tool that silently answers a
// different question teaches the user that the data is empty.
//
// ============================================================================
// WHAT IS IMPLEMENTED
// ============================================================================
//
//   tr [-Ccdst] SET1 [SET2]
//
//   -c, -C, --complement       use the complement of SET1
//   -d, --delete               delete characters in SET1
//   -s, --squeeze-repeats      squeeze repeats of the last-specified set
//   -t, --truncate-set1        truncate SET1 to the length of SET2
//   --                         end of options
//
// SET syntax: literals; \\ \a \b \f \n \r \t \v and \NNN octal escapes;
// ranges A-Z; [:alnum:] [:alpha:] [:blank:] [:cntrl:] [:digit:] [:graph:]
// [:lower:] [:print:] [:punct:] [:space:] [:upper:] [:xdigit:]; [=c=]
// equivalence classes (one character each, this is a byte/C-locale tr); and
// the [c*n] / [c*] repeat constructs.
//
// SET2 is padded to SET1's length with its own last character unless -t is
// given, which is GNU's behaviour and the one every script depends on.
//
// ============================================================================
// WHAT IS REFUSED, LOUDLY, WITH A NON-ZERO EXIT AND NOTHING ON STDOUT
// ============================================================================
//
//   * an unknown option, instead of silently treating it as SET1;
//   * a missing or extra operand for the option combination given;
//   * an empty SET2 when translating;
//   * a reversed range (`tr z-a x`);
//   * an unknown [:class:] name, an unterminated [: :] / [= =] / [c*n];
//   * a [:class:] in SET2 while translating, unless both sets are exactly one
//     class construct and it is [:upper:] or [:lower:] (POSIX allows no other
//     alignment, and accepting one silently would produce a wrong table).
//
// KNOWN LIMIT, STATED RATHER THAN HIDDEN: this is a BYTE tr in the C locale.
// [=c=] therefore contains exactly the byte c, and the character classes are
// the ASCII ones. That is the same locale model the rest of this userland uses
// (see userland/libc/wctype.h), not a shortcut taken here.
//
// Expectations for every case in the test suite come from the HOST's own GNU
// tr via tools/tr-oracle/tr-oracle.sh. Nothing here was reasoned from POSIX: a
// hand-written expectation for a standard tool is a guess with a straight face.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"

#define SETMAX 1024

static const char *PROG = "tr";

static void die(const char *msg, const char *arg)
{
    if (arg) fprintf(stderr, "%s: %s: %s\n", PROG, msg, arg);
    else     fprintf(stderr, "%s: %s\n", PROG, msg);
    exit(1);
}

static void usage_die(const char *msg, const char *arg)
{
    if (arg) fprintf(stderr, "%s: %s: %s\n", PROG, msg, arg);
    else     fprintf(stderr, "%s: %s\n", PROG, msg);
    fprintf(stderr, "Usage: %s [-Ccdst] SET1 [SET2]\n", PROG);
    exit(1);
}

// ---------------------------------------------------------------------------
// Set expansion
// ---------------------------------------------------------------------------

typedef struct {
    unsigned char c[SETMAX];
    int  n;
    int  fill_at;          // index at which a [c*] "fill to length" appeared
    unsigned char fill_ch;
    int  nclasses;         // number of [:class:] constructs seen
    int  class_only;       // the whole set is exactly one [:class:] construct
    int  class_upper_lower;// that one class was [:upper:] or [:lower:]
} set_t;

static void set_add(set_t *s, unsigned c)
{
    if (s->n >= SETMAX) die("set too large", NULL);
    s->c[s->n++] = (unsigned char)c;
}

// One character of a set, honouring backslash escapes. Advances *i.
static unsigned char one_char(const char *s, int *i)
{
    unsigned char c = (unsigned char)s[*i];
    if (c != '\\') { (*i)++; return c; }
    (*i)++;
    c = (unsigned char)s[*i];
    if (c == '\0') return '\\';        // trailing backslash is a literal
    (*i)++;
    switch (c) {
        case 'a': return 7;
        case 'b': return 8;
        case 'f': return 12;
        case 'n': return 10;
        case 'r': return 13;
        case 't': return 9;
        case 'v': return 11;
        case '\\': return '\\';
        default: break;
    }
    if (c >= '0' && c <= '7') {
        int v = c - '0', k = 1;
        while (k < 3 && s[*i] >= '0' && s[*i] <= '7') { v = v * 8 + (s[*i] - '0'); (*i)++; k++; }
        return (unsigned char)(v & 0xFF);
    }
    return c;                          // \q is a literal q (GNU warns; we accept)
}

static int class_member(const char *name, int c)
{
    if (!strcmp(name, "alpha"))  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (!strcmp(name, "digit"))  return c >= '0' && c <= '9';
    if (!strcmp(name, "alnum"))  return class_member("alpha", c) || class_member("digit", c);
    if (!strcmp(name, "lower"))  return c >= 'a' && c <= 'z';
    if (!strcmp(name, "upper"))  return c >= 'A' && c <= 'Z';
    if (!strcmp(name, "space"))  return c == ' ' || (c >= 9 && c <= 13);
    if (!strcmp(name, "blank"))  return c == ' ' || c == '\t';
    if (!strcmp(name, "cntrl"))  return c < 32 || c == 127;
    if (!strcmp(name, "print"))  return c >= 32 && c < 127;
    if (!strcmp(name, "graph"))  return c > 32 && c < 127;
    if (!strcmp(name, "punct"))  return class_member("graph", c) && !class_member("alnum", c);
    if (!strcmp(name, "xdigit")) return class_member("digit", c) ||
                                        (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    return -1;                          // unknown class name
}

// Expand SRC into DST. Returns on success; dies with a message on any
// malformed construct rather than silently treating it as literal text.
static void parse_set(const char *src, set_t *dst)
{
    int i = 0, first = 1;
    dst->n = 0;
    dst->fill_at = -1;
    dst->fill_ch = 0;
    dst->nclasses = 0;
    dst->class_only = 0;
    dst->class_upper_lower = 0;

    while (src[i]) {
        // ---- [:class:] , [=c=] , [c*n] -----------------------------------
        if (src[i] == '[' && src[i + 1] == ':') {
            char name[16];
            int j = i + 2, k = 0;
            while (src[j] && src[j] != ':' && k < 15) name[k++] = src[j++];
            name[k] = '\0';
            if (src[j] != ':' || src[j + 1] != ']')
                die("unterminated character class in set", src);
            for (int c = 0; c < 256; c++) {
                int m = class_member(name, c);
                if (m < 0) die("invalid character class", name);
                if (m) set_add(dst, (unsigned)c);
            }
            dst->nclasses++;
            if (first && src[j + 2] == '\0') {
                dst->class_only = 1;
                dst->class_upper_lower = (!strcmp(name, "upper") || !strcmp(name, "lower"));
            }
            i = j + 2;
            first = 0;
            continue;
        }
        if (src[i] == '[' && src[i + 1] == '=') {
            int j = i + 2;
            unsigned char c = one_char(src, &j);
            if (src[j] != '=' || src[j + 1] != ']')
                die("unterminated equivalence class in set", src);
            set_add(dst, c);
            i = j + 2;
            first = 0;
            continue;
        }
        if (src[i] == '[') {
            // [c*n] / [c*] - scan ahead; if it does not parse as a repeat, the
            // '[' is an ordinary character (which is what GNU does too).
            int j = i + 1;
            unsigned char c = one_char(src, &j);
            if (src[j] == '*') {
                j++;
                if (src[j] == ']') {
                    if (dst->fill_at >= 0) die("only one [c*] is allowed in a set", src);
                    dst->fill_at = dst->n;
                    dst->fill_ch = c;
                    i = j + 1;
                    first = 0;
                    continue;
                }
                int base = (src[j] == '0') ? 8 : 10;
                long count = 0;
                int digits = 0;
                while ((src[j] >= '0' && src[j] <= '7') ||
                       (base == 10 && src[j] >= '0' && src[j] <= '9')) {
                    count = count * base + (src[j] - '0');
                    j++; digits++;
                    if (count > SETMAX) die("repeat count too large in set", src);
                }
                if (!digits || src[j] != ']')
                    die("unterminated repeat construct in set", src);
                for (long q = 0; q < count; q++) set_add(dst, c);
                i = j + 1;
                first = 0;
                continue;
            }
            // not a repeat: fall through and take '[' literally
        }

        // ---- literal, possibly the start of a range -----------------------
        {
            int j = i;
            unsigned char lo = one_char(src, &j);
            if (src[j] == '-' && src[j + 1] != '\0') {
                int k = j + 1;
                unsigned char hi = one_char(src, &k);
                if (hi < lo) die("range-endpoints are in reverse collating sequence order", src);
                for (int c = lo; c <= (int)hi; c++) set_add(dst, (unsigned)c);
                i = k;
            } else {
                set_add(dst, lo);
                i = j;
            }
            first = 0;
        }
    }
}

// Pad SET2 out to TARGET characters: expand a [c*] in place if there was one,
// otherwise repeat the last character. Both are GNU's rules.
static void set_pad(set_t *s, int target)
{
    if (s->n >= target) return;
    int need = target - s->n;
    if (need + s->n > SETMAX) die("set too large after padding", NULL);
    if (s->fill_at >= 0) {
        for (int i = s->n - 1; i >= s->fill_at; i--) s->c[i + need] = s->c[i];
        for (int i = 0; i < need; i++) s->c[s->fill_at + i] = s->fill_ch;
        s->n += need;
    } else {
        unsigned char last = s->c[s->n - 1];
        while (s->n < target) s->c[s->n++] = last;
    }
}

// ---------------------------------------------------------------------------
// I/O. write() to a pipe can be short (kernel/fs/pipe.c writes what fits), so
// every write loops. A negative return is a real error and is reported; it is
// how `cat | head -1` terminates on this OS, which has no SIGPIPE.
// ---------------------------------------------------------------------------
static unsigned char obuf[8192];
static int olen = 0;

static void out_flush(void)
{
    int off = 0;
    while (off < olen) {
        long w = write(1, obuf + off, (size_t)(olen - off));
        if (w < 0) { fprintf(stderr, "%s: write error\n", PROG); exit(1); }
        off += (int)w;
    }
    olen = 0;
}

static void out_put(unsigned char c)
{
    obuf[olen++] = c;
    if (olen == (int)sizeof obuf) out_flush();
}

int main(int argc, char **argv)
{
    int c_flag = 0, d_flag = 0, s_flag = 0, t_flag = 0;
    const char *op[4];
    int nop = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == '-' && a[2] == '\0') { i++; break; }
        if (a[0] == '-' && a[1] == '-') {
            if (!strcmp(a, "--complement"))          c_flag = 1;
            else if (!strcmp(a, "--delete"))         d_flag = 1;
            else if (!strcmp(a, "--squeeze-repeats"))s_flag = 1;
            else if (!strcmp(a, "--truncate-set1"))  t_flag = 1;
            else if (!strcmp(a, "--help"))           { usage_die("usage", NULL); }
            else usage_die("unrecognized option", a);
            continue;
        }
        if (a[0] == '-' && a[1] != '\0') {
            for (int k = 1; a[k]; k++) {
                switch (a[k]) {
                    case 'c': case 'C': c_flag = 1; break;
                    case 'd': d_flag = 1; break;
                    case 's': s_flag = 1; break;
                    case 't': t_flag = 1; break;
                    default: {
                        char b[3] = { '-', a[k], 0 };
                        usage_die("invalid option", b);
                    }
                }
            }
            continue;
        }
        if (nop < 4) op[nop] = a;
        nop++;
    }
    for (; i < argc; i++) { if (nop < 4) op[nop] = argv[i]; nop++; }

    // ---- operand count: refuse every combination that is not meaningful ----
    if (nop == 0) usage_die("missing operand", NULL);
    if (nop > 2)  usage_die("extra operand", op[2]);
    if (d_flag && !s_flag && nop != 1)
        usage_die("extra operand after SET1 (-d takes one set)", op[1]);
    if (d_flag && s_flag && nop != 2)
        usage_die("-d -s needs SET1 (delete) and SET2 (squeeze)", NULL);
    if (!d_flag && !s_flag && nop != 2)
        usage_die("missing operand after SET1 (translation needs SET2)", op[0]);

    set_t s1, s2;
    memset(&s1, 0, sizeof s1);
    memset(&s2, 0, sizeof s2);
    s1.fill_at = s2.fill_at = -1;
    parse_set(op[0], &s1);
    if (nop == 2) parse_set(op[1], &s2);

    if (c_flag) {                       // complement SET1, ascending
        unsigned char in1[256];
        memset(in1, 0, sizeof in1);
        for (int k = 0; k < s1.n; k++) in1[s1.c[k]] = 1;
        s1.n = 0;
        for (int ch = 0; ch < 256; ch++) if (!in1[ch]) set_add(&s1, (unsigned)ch);
        s1.class_only = 0;
    }

    int translating = (!d_flag && nop == 2);

    if (translating) {
        if (s2.n == 0 && s2.fill_at < 0)
            usage_die("when not truncating SET1, SET2 must be non-empty", NULL);
        // POSIX permits a class in SET2 only when both sets are exactly one
        // class construct and it is upper/lower. Anything else would build a
        // silently wrong table, so it is refused instead.
        if (s2.nclasses > 0 &&
            !(s1.class_only && s2.class_only && s1.class_upper_lower && s2.class_upper_lower))
            die("misaligned [:upper:] and [:lower:] construct in SET2", op[1]);
        if (t_flag) { if (s1.n > s2.n) s1.n = s2.n; }
        else        { set_pad(&s2, s1.n); }
    }

    unsigned char map[256];
    unsigned char del[256], sq[256];
    memset(del, 0, sizeof del);
    memset(sq, 0, sizeof sq);
    for (int ch = 0; ch < 256; ch++) map[ch] = (unsigned char)ch;

    if (translating)
        for (int k = 0; k < s1.n && k < s2.n; k++) map[s1.c[k]] = s2.c[k];

    if (d_flag)
        for (int k = 0; k < s1.n; k++) del[s1.c[k]] = 1;

    if (s_flag) {
        // The squeeze set is SET2 when a second set was given (translate+squeeze
        // or delete+squeeze) and SET1 otherwise.
        const set_t *sqs = (nop == 2) ? &s2 : &s1;
        for (int k = 0; k < sqs->n; k++) sq[sqs->c[k]] = 1;
    }

    unsigned char ibuf[4096];
    long n;
    int last = -1;
    while ((n = read(0, ibuf, sizeof ibuf)) > 0) {
        for (long k = 0; k < n; k++) {
            unsigned char ch = ibuf[k];
            if (d_flag && del[ch]) continue;
            ch = map[ch];
            if (s_flag && sq[ch] && (int)ch == last) continue;
            last = (int)ch;
            out_put(ch);
        }
    }
    out_flush();
    if (n < 0) { fprintf(stderr, "%s: read error\n", PROG); return 1; }
    return 0;
}
