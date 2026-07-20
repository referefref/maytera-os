// bc - arbitrary-precision integer calculator for MayteraOS (#354)
//
// Supports + - * / % ^ , parentheses, unary minus, and arbitrarily large
// integers (base-10, one digit per byte). Reads expressions one per line from
// stdin (interactive or piped). `quit` / `q` / `exit` ends the session.
//
// Honest scope: INTEGER arithmetic (scale = 0). Division truncates toward zero
// like C; there is no fractional `scale` as in GNU bc. This is enough for the
// classic "arbitrary precision" demos (factorials, 2^4096, huge products).
#include "../../libc/maytera.h"
#include "../../libc/syscall.h"
#include "../../libc/string.h"
#include "../../libc/stdlib.h"
#include "../../libc/stdio.h"

// ---------------------------------------------------------------------------
// Bignum: sign * (d[0] + d[1]*10 + d[2]*100 + ...), d holds decimal digits 0..9
// little-endian. Zero is {sign=0, n=1, d="\0"} (a single 0 digit).
// ---------------------------------------------------------------------------
typedef struct { int sign; int n; unsigned char *d; } BN;

static int g_err = 0;
static void die(const char *m) { (void)m; g_err = 1; }

static BN bn_zero(void) {
    BN b; b.sign = 0; b.n = 1; b.d = (unsigned char *)malloc(1); if (b.d) b.d[0] = 0; return b;
}
static void bn_free(BN *b) { if (b->d) free(b->d); b->d = 0; b->n = 0; b->sign = 0; }

static void bn_norm(BN *b) {
    while (b->n > 1 && b->d[b->n - 1] == 0) b->n--;
    if (b->n == 1 && b->d[0] == 0) b->sign = 0;
    else if (b->sign == 0) b->sign = 1;   // nonzero must have a sign
}

static BN bn_from_str(const char *s, int len) {
    BN b; b.sign = 1;
    b.d = (unsigned char *)malloc(len ? len : 1);
    if (!b.d) { die("oom"); return bn_zero(); }
    b.n = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (s[i] < '0' || s[i] > '9') { die("badnum"); bn_free(&b); return bn_zero(); }
        b.d[b.n++] = (unsigned char)(s[i] - '0');
    }
    if (b.n == 0) { b.d[0] = 0; b.n = 1; }
    bn_norm(&b);
    return b;
}

static BN bn_copy(const BN *a) {
    BN b; b.sign = a->sign; b.n = a->n; b.d = (unsigned char *)malloc(a->n ? a->n : 1);
    if (!b.d) { die("oom"); return bn_zero(); }
    memcpy(b.d, a->d, a->n); return b;
}

// compare magnitudes: -1,0,1
static int cmp_abs(const BN *a, const BN *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

// magnitude add: |a|+|b|
static BN add_abs(const BN *a, const BN *b) {
    int n = (a->n > b->n ? a->n : b->n) + 1;
    BN r; r.sign = 1; r.n = n; r.d = (unsigned char *)malloc(n);
    if (!r.d) { die("oom"); return bn_zero(); }
    int carry = 0;
    for (int i = 0; i < n; i++) {
        int s = carry;
        if (i < a->n) s += a->d[i];
        if (i < b->n) s += b->d[i];
        r.d[i] = (unsigned char)(s % 10); carry = s / 10;
    }
    bn_norm(&r); return r;
}

// magnitude subtract: |a|-|b|, requires |a| >= |b|
static BN sub_abs(const BN *a, const BN *b) {
    BN r; r.sign = 1; r.n = a->n; r.d = (unsigned char *)malloc(a->n);
    if (!r.d) { die("oom"); return bn_zero(); }
    int borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int s = a->d[i] - borrow - (i < b->n ? b->d[i] : 0);
        if (s < 0) { s += 10; borrow = 1; } else borrow = 0;
        r.d[i] = (unsigned char)s;
    }
    bn_norm(&r); return r;
}

static BN bn_add(const BN *a, const BN *b);
static BN bn_neg(const BN *a) { BN r = bn_copy(a); if (r.sign) r.sign = -r.sign; return r; }

static BN bn_sub(const BN *a, const BN *b) {  // a - b
    BN nb = bn_neg(b); BN r = bn_add(a, &nb); bn_free(&nb); return r;
}

static BN bn_add(const BN *a, const BN *b) {  // signed add
    if (a->sign == 0) return bn_copy(b);
    if (b->sign == 0) return bn_copy(a);
    if (a->sign == b->sign) { BN r = add_abs(a, b); r.sign = a->sign; bn_norm(&r); return r; }
    int c = cmp_abs(a, b);
    if (c == 0) return bn_zero();
    if (c > 0) { BN r = sub_abs(a, b); r.sign = a->sign; bn_norm(&r); return r; }
    BN r = sub_abs(b, a); r.sign = b->sign; bn_norm(&r); return r;
}

static BN bn_mul(const BN *a, const BN *b) {
    if (a->sign == 0 || b->sign == 0) return bn_zero();
    int n = a->n + b->n;
    unsigned int *tmp = (unsigned int *)malloc(sizeof(unsigned int) * n);
    if (!tmp) { die("oom"); return bn_zero(); }
    for (int i = 0; i < n; i++) tmp[i] = 0;
    for (int i = 0; i < a->n; i++)
        for (int j = 0; j < b->n; j++)
            tmp[i + j] += (unsigned int)a->d[i] * (unsigned int)b->d[j];
    BN r; r.sign = a->sign * b->sign; r.n = n; r.d = (unsigned char *)malloc(n);
    if (!r.d) { free(tmp); die("oom"); return bn_zero(); }
    unsigned int carry = 0;
    for (int i = 0; i < n; i++) { unsigned int s = tmp[i] + carry; r.d[i] = (unsigned char)(s % 10); carry = s / 10; }
    free(tmp);
    bn_norm(&r); return r;
}

// truncated division toward zero: q = a/b, r = a%b (r has sign of a). b != 0.
static void bn_divmod(const BN *a, const BN *b, BN *q, BN *r) {
    if (b->sign == 0) { die("div0"); *q = bn_zero(); *r = bn_zero(); return; }
    // work on magnitudes
    BN rem = bn_zero();
    unsigned char *qd = (unsigned char *)malloc(a->n);
    if (!qd) { die("oom"); *q = bn_zero(); *r = bn_zero(); return; }
    int qn = 0;
    // process from most significant digit down
    for (int i = a->n - 1; i >= 0; i--) {
        // rem = rem*10 + a->d[i]
        BN t; t.sign = 1; t.n = rem.n + 1; t.d = (unsigned char *)malloc(t.n);
        if (!t.d) { die("oom"); break; }
        t.d[0] = a->d[i];
        for (int k = 0; k < rem.n; k++) t.d[k + 1] = rem.d[k];
        bn_free(&rem); rem = t; bn_norm(&rem);
        // find how many times |b| fits (0..9) by repeated subtraction
        int qdig = 0;
        BN babs = bn_copy(b); babs.sign = 1;
        while (cmp_abs(&rem, &babs) >= 0) { BN s = sub_abs(&rem, &babs); bn_free(&rem); rem = s; qdig++; }
        bn_free(&babs);
        qd[a->n - 1 - i] = (unsigned char)qdig; qn++;
    }
    // qd is most-significant-first; reverse into a BN
    BN Q; Q.sign = 1; Q.n = qn ? qn : 1; Q.d = (unsigned char *)malloc(Q.n);
    if (!Q.d) { die("oom"); free(qd); *q = bn_zero(); *r = rem; return; }
    for (int i = 0; i < qn; i++) Q.d[i] = qd[qn - 1 - i];
    free(qd);
    bn_norm(&Q);
    Q.sign = (Q.n == 1 && Q.d[0] == 0) ? 0 : a->sign * b->sign;
    rem.sign = (rem.n == 1 && rem.d[0] == 0) ? 0 : a->sign;
    bn_norm(&Q); bn_norm(&rem);
    *q = Q; *r = rem;
}

// a ^ e (e via BN; must be non-negative and fit a reasonable exponent).
static BN bn_pow(const BN *a, const BN *e) {
    if (e->sign < 0) { die("negexp"); return bn_zero(); }
    if (e->sign == 0) { BN one = bn_from_str("1", 1); return one; }
    // convert exponent to a long; cap to avoid runaway memory
    long ex = 0;
    for (int i = e->n - 1; i >= 0; i--) {
        ex = ex * 10 + e->d[i];
        if (ex > 1000000) { die("bigexp"); return bn_zero(); }
    }
    BN result = bn_from_str("1", 1);
    BN base = bn_copy(a);
    while (ex > 0) {
        if (ex & 1) { BN t = bn_mul(&result, &base); bn_free(&result); result = t; }
        ex >>= 1;
        if (ex) { BN t = bn_mul(&base, &base); bn_free(&base); base = t; }
    }
    bn_free(&base);
    return result;
}

static char *bn_to_str(const BN *b) {
    int len = b->n + (b->sign < 0 ? 1 : 0) + 1;
    char *s = (char *)malloc(len);
    if (!s) { die("oom"); return 0; }
    int p = 0;
    if (b->sign < 0) s[p++] = '-';
    for (int i = b->n - 1; i >= 0; i--) s[p++] = (char)('0' + b->d[i]);
    s[p] = 0;
    return s;
}

// ---------------------------------------------------------------------------
// Recursive-descent parser over a global cursor.
// ---------------------------------------------------------------------------
static const char *P;
static void skip_ws(void) { while (*P == ' ' || *P == '\t') P++; }

static BN parse_expr(void);

static BN parse_primary(void) {
    skip_ws();
    if (*P == '(') {
        P++;
        BN v = parse_expr();
        skip_ws();
        if (*P == ')') P++; else die("expected )");
        return v;
    }
    if (*P >= '0' && *P <= '9') {
        const char *start = P;
        while (*P >= '0' && *P <= '9') P++;
        return bn_from_str(start, (int)(P - start));
    }
    die("expected number");
    return bn_zero();
}

static BN parse_unary(void) {
    skip_ws();
    if (*P == '-') { P++; BN v = parse_unary(); BN r = bn_neg(&v); bn_free(&v); return r; }
    if (*P == '+') { P++; return parse_unary(); }
    return parse_primary();
}

static BN parse_power(void) {   // right-associative ^
    BN base = parse_unary();
    skip_ws();
    if (*P == '^') { P++; BN e = parse_power(); BN r = bn_pow(&base, &e); bn_free(&base); bn_free(&e); return r; }
    return base;
}

static BN parse_term(void) {
    BN v = parse_power();
    for (;;) {
        skip_ws();
        char op = *P;
        if (op != '*' && op != '/' && op != '%') break;
        P++;
        BN rhs = parse_power();
        if (op == '*') { BN t = bn_mul(&v, &rhs); bn_free(&v); v = t; }
        else {
            BN q, r; bn_divmod(&v, &rhs, &q, &r);
            bn_free(&v);
            if (op == '/') { v = q; bn_free(&r); } else { v = r; bn_free(&q); }
        }
        bn_free(&rhs);
        if (g_err) break;
    }
    return v;
}

static BN parse_expr(void) {
    BN v = parse_term();
    for (;;) {
        skip_ws();
        char op = *P;
        if (op != '+' && op != '-') break;
        P++;
        BN rhs = parse_term();
        BN t = (op == '+') ? bn_add(&v, &rhs) : bn_sub(&v, &rhs);
        bn_free(&v); bn_free(&rhs); v = t;
        if (g_err) break;
    }
    return v;
}

static void trim(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
}

static void eval_line(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    if (*s == 0) return;
    g_err = 0;
    P = s;
    BN v = parse_expr();
    skip_ws();
    if (!g_err && *P != 0) die("trailing junk");
    if (g_err) { printf("syntax error\n"); }
    else {
        char *out = bn_to_str(&v);
        if (out) { printf("%s\n", out); free(out); }
    }
    bn_free(&v);
}

int main(int argc, char **argv) {
    // If given args, evaluate them as a single expression and exit (handy for
    // scripts and the remote `run` bridge, which cannot feed stdin). Otherwise
    // read expressions one per line from stdin (interactive or piped).
    if (argc > 1) {
        char expr[1024]; int p = 0;
        for (int i = 1; i < argc && p < (int)sizeof(expr) - 1; i++) {
            if (i > 1 && p < (int)sizeof(expr) - 1) expr[p++] = ' ';
            const char *a = argv[i];
            while (*a && p < (int)sizeof(expr) - 1) expr[p++] = *a++;
        }
        expr[p] = 0;
        eval_line(expr);
        return 0;
    }
    char line[1024];
    // Small banner only for an interactive tty is hard to detect here; keep it
    // quiet so piped use is clean. Type an expression, or quit.
    while (fgets(line, sizeof(line), stdin)) {
        trim(line);
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == 0) continue;
        if (strcmp(s, "quit") == 0 || strcmp(s, "q") == 0 || strcmp(s, "exit") == 0) break;
        if (s[0] == '#') continue;   // comment line
        eval_line(s);
    }
    return 0;
}
