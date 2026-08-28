// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
//
// term_emu_test.c - per-sequence evidence for the Terminal emulation core.
//
// WHY THIS EXISTS, in one sentence: the tier-1 parity pass could not claim
// insert/delete-line because it had only ever been exercised INDIRECTLY,
// through vi, and "vi looked right" is not evidence about a sequence. Every
// case below emits ONE sequence and asserts on exactly what that sequence did.
//
// It is a HOSTED build (plain gcc, no -ffreestanding, no libc.a) because
// term_emu.c makes no syscalls. Run it on the build container:
//
//     gcc -O1 -Wall -Wextra -Werror -DTERM_EMU_HOSTTEST
//         -o term_emu_test term_emu_test.c ../term_emu.c && ./term_emu_test
//
// It prints one line per case and a final count. Non-zero exit = a failure.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "../term_emu.h"

// ---------------------------------------------------------------------------
// A trace of everything the parser handed back, so a case can assert on the
// WHOLE reaction to a sequence - including the crucial negative assertion
// "and nothing was printed", which is what a leak test actually needs.

#define TR_MAX 4096
static char  g_printed[TR_MAX];     // UTF-8 re-encoding of every print() codepoint
static int   g_printed_len;
static uint32_t g_cps[TR_MAX];
static int   g_ncps;
static char  g_events[TR_MAX];      // one-line summary of each non-print event
static int   g_events_len;

static void tr_reset(void) {
    g_printed_len = 0; g_printed[0] = 0;
    g_ncps = 0;
    g_events_len = 0; g_events[0] = 0;
}

static void ev(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    g_events_len += vsnprintf(g_events + g_events_len,
                              (size_t)(TR_MAX - g_events_len), fmt, ap);
    va_end(ap);
}

static void cb_print(void *ctx, uint32_t cp) {
    (void)ctx;
    if (g_ncps < TR_MAX) g_cps[g_ncps++] = cp;
    // re-encode to UTF-8 so a case can compare against a plain string literal
    char *o = g_printed + g_printed_len;
    if (cp < 0x80) { *o++ = (char)cp; }
    else if (cp < 0x800) { *o++ = (char)(0xC0 | (cp >> 6)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { *o++ = (char)(0xE0 | (cp >> 12)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    else { *o++ = (char)(0xF0 | (cp >> 18)); *o++ = (char)(0x80 | ((cp >> 12) & 0x3F)); *o++ = (char)(0x80 | ((cp >> 6) & 0x3F)); *o++ = (char)(0x80 | (cp & 0x3F)); }
    g_printed_len = (int)(o - g_printed);
    g_printed[g_printed_len] = 0;
}

static void cb_execute(void *ctx, uint8_t c) { (void)ctx; ev("[exec %02X]", c); }

static void seq_str(const term_seq_t *s, char *out, size_t cap) {
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "final=%c", s->final);
    if (s->priv) n += (size_t)snprintf(out + n, cap - n, " priv=%c", s->priv);
    for (int i = 0; i < s->ninter; i++)
        n += (size_t)snprintf(out + n, cap - n, " int=%c", s->inter[i]);
    n += (size_t)snprintf(out + n, cap - n, " p=[");
    for (int i = 0; i < s->nparams; i++) {
        n += (size_t)snprintf(out + n, cap - n, "%d", s->params[i]);
        n += (size_t)snprintf(out + n, cap - n, "%c",
                              (i + 1 < s->nparams) ? s->sep[i] : ']');
    }
    if (s->nparams == 0) snprintf(out + n, cap - n, "]");
}

static void cb_csi(void *ctx, const term_seq_t *s) {
    (void)ctx; char b[512]; seq_str(s, b, sizeof(b)); ev("[CSI %s]", b);
}
static void cb_esc(void *ctx, const term_seq_t *s) {
    (void)ctx;
    char in[8] = {0};
    for (int i = 0; i < s->ninter; i++) in[i] = s->inter[i];
    ev("[ESC %s%c]", in, s->final);
}
static void cb_osc(void *ctx, const char *s, int len) {
    (void)ctx; (void)len; ev("[OSC %s]", s);
}
static void cb_dcs(void *ctx, const term_seq_t *s, const char *pl, int len) {
    (void)ctx; (void)len; ev("[DCS %c %s]", s->final, pl);
}

static const term_cb_t CB = { cb_print, cb_execute, cb_csi, cb_esc, cb_osc, cb_dcs };

static term_parser_t P;

static void feed(const char *bytes, int len) {
    term_emu_feed(&P, (const unsigned char *)bytes, len, &CB, NULL);
}
static void feed_s(const char *s) { feed(s, (int)strlen(s)); }

// ---------------------------------------------------------------------------
static int g_pass, g_fail;

static void check(const char *name, int ok, const char *got, const char *want) {
    if (ok) { g_pass++; printf("  PASS  %-44s %s\n", name, got); }
    else    { g_fail++; printf("  FAIL  %-44s got %s  want %s\n", name, got, want); }
}

// Run one sequence from a clean parser and assert on BOTH what was dispatched
// and what leaked to the screen.
static void case_seq(const char *name, const char *in, int inlen,
                     const char *want_events, const char *want_printed) {
    term_emu_reset(&P);
    tr_reset();
    feed(in, inlen);
    char got[TR_MAX * 2 + 64];
    snprintf(got, sizeof(got), "ev=%s print=\"%s\"", g_events, g_printed);
    char want[TR_MAX * 2 + 64];
    snprintf(want, sizeof(want), "ev=%s print=\"%s\"", want_events, want_printed);
    check(name, strcmp(g_events, want_events) == 0 && strcmp(g_printed, want_printed) == 0,
          got, want);
}
#define CASE(name, lit, ev, pr) case_seq(name, lit, (int)sizeof(lit) - 1, ev, pr)

// ---------------------------------------------------------------------------
static void check_int(const char *name, long got, long want) {
    char g[64], w[64];
    snprintf(g, sizeof(g), "%ld", got);
    snprintf(w, sizeof(w), "%ld", want);
    check(name, got == want, g, w);
}
static void check_hex(const char *name, unsigned long got, unsigned long want) {
    char g[64], w[64];
    snprintf(g, sizeof(g), "0x%08lX", got);
    snprintf(w, sizeof(w), "0x%08lX", want);
    check(name, got == want, g, w);
}

// Drive the REAL parser and let it hand the real term_seq_t to term_emu_sgr().
// A test that hand-builds a term_seq_t proves only that the interpreter agrees
// with the test author about what the tokenizer produces, which is exactly the
// "passed while broken" failure this file exists to prevent.
static term_sgr_t *g_pen_target;
static void cb_csi_sgr(void *ctx, const term_seq_t *s) {
    (void)ctx;
    if (s->final == 'm' && g_pen_target) term_emu_sgr(g_pen_target, s);
}
static const term_cb_t CB_SGR = { NULL, NULL, cb_csi_sgr, NULL, NULL, NULL };

static void sgr_of(const char *lit, term_sgr_t *pen) {
    term_emu_reset(&P);
    term_emu_sgr_reset(pen);
    g_pen_target = pen;
    term_emu_feed(&P, (const unsigned char *)lit, (int)strlen(lit), &CB_SGR, NULL);
}

// ---------------------------------------------------------------------------
int main(void) {
    printf("\n=== MayteraOS Terminal emulation core: per-sequence evidence ===\n");

    // -----------------------------------------------------------------------
    printf("\n-- 1. LEAK TESTS (each of these printed its tail as text before) --\n");

    CASE("OSC 0 title, BEL-terminated",
         "\x1b]0;my title\x07", "[OSC 0;my title]", "");
    CASE("OSC 0 title, ST-terminated",
         "\x1b]0;my title\x1b\\", "[OSC 0;my title]", "");
    CASE("OSC split across two reads (ST straddles)",
         "\x1b]2;abc", "", "");
    // continue the SAME parser: this is the read-boundary case a lookahead
    // implementation gets wrong.
    tr_reset(); feed_s("\x1b\\");
    check("OSC ST arriving in the next read", strcmp(g_events, "[OSC 2;abc]") == 0,
          g_events, "[OSC 2;abc]");

    CASE("ESC ( B  charset designation", "\x1b(B", "[ESC (B]", "");
    CASE("ESC ) 0  charset designation", "\x1b)0", "[ESC )0]", "");
    CASE("ESC # 8  DECALN",              "\x1b#8", "[ESC #8]", "");
    CASE("CSI > 4 ; 2 m  modifyOtherKeys",
         "\x1b[>4;2m", "[CSI final=m priv=> p=[4;2]]", "");
    CASE("CSI SP q  DECSCUSR (vim cursor shape)",
         "\x1b[5 q", "[CSI final=q int=  p=[5]]", "");
    CASE("CSI ! p  DECSTR soft reset",
         "\x1b[!p", "[CSI final=p int=! p=[-1]]", "");
    CASE("CSI $ p  DECRQM",
         "\x1b[?1049$p", "[CSI final=p priv=? int=$ p=[1049]]", "");
    CASE("CSI \" q  DECSCA",
         "\x1b[0\"q", "[CSI final=q int=\" p=[0]]", "");
    CASE("DCS payload is swallowed, not printed",
         "\x1bP1$r0m\x1b\\", "[DCS r 0m]", "");
    CASE("APC payload is swallowed",
         "\x1b_Gf=100;abc\x1b\\", "", "");
    CASE("abandoned OSC does not eat the next sequence",
         "\x1b]0;junk\x1b[2J", "[CSI final=J p=[2]]", "");

    // -----------------------------------------------------------------------
    printf("\n-- 2. CSI PARAMETER GRAMMAR --\n");
    CASE("CSI H  (no params -> one omitted)", "\x1b[H", "[CSI final=H p=[-1]]", "");
    CASE("CSI 5H", "\x1b[5H", "[CSI final=H p=[5]]", "");
    CASE("CSI ;5H (omitted first param)", "\x1b[;5H", "[CSI final=H p=[-1;5]]", "");
    CASE("CSI 3;7H", "\x1b[3;7H", "[CSI final=H p=[3;7]]", "");
    CASE("CSI 3;7;H (trailing separator)", "\x1b[3;7;H", "[CSI final=H p=[3;7;-1]]", "");
    CASE("CSI ?25l private", "\x1b[?25l", "[CSI final=l priv=? p=[25]]", "");
    CASE("colon subparams keep their separator",
         "\x1b[38:2:1:2:3m", "[CSI final=m p=[38:2:1:2:3]]", "");
    CASE("parameter overflow clamps, does not wrap",
         "\x1b[99999999999m", "[CSI final=m p=[65535]]", "");
    CASE("printable text around a sequence survives",
         "ab\x1b[31mcd", "[CSI final=m p=[31]]", "abcd");

    // -----------------------------------------------------------------------
    printf("\n-- 3. UTF-8 DECODING --\n");
    CASE("2-byte: U+00E9 e-acute", "\xc3\xa9", "", "\xc3\xa9");
    CASE("3-byte: U+2500 box drawing", "\xe2\x94\x80", "", "\xe2\x94\x80");
    CASE("3-byte: U+4E16 CJK", "\xe4\xb8\x96", "", "\xe4\xb8\x96");
    CASE("4-byte: U+1F600 emoji", "\xf0\x9f\x98\x80", "", "\xf0\x9f\x98\x80");
    CASE("overlong 2-byte '/' rejected", "\xc0\xaf", "", "\xef\xbf\xbd");
    CASE("surrogate D800 rejected", "\xed\xa0\x80", "", "\xef\xbf\xbd");
    CASE("stray continuation byte rejected", "\x80", "", "\xef\xbf\xbd");
    CASE("truncated sequence resyncs on 'A'", "\xe2\x94" "A", "", "\xef\xbf\xbd" "A");
    CASE("truncated sequence does not eat a following ESC",
         "\xe2\x94\x1b[2J", "[CSI final=J p=[2]]", "\xef\xbf\xbd");

    // a codepoint split across two feeds must survive
    term_emu_reset(&P); tr_reset();
    feed("\xe4\xb8", 2);
    feed("\x96", 1);
    check("codepoint split across two reads",
          g_ncps == 1 && g_cps[0] == 0x4E16, g_printed, "\xe4\xb8\x96");

    // -----------------------------------------------------------------------
    printf("\n-- 4. wcwidth --\n");
    check_int("wcwidth('A')",           term_emu_wcwidth('A'), 1);
    check_int("wcwidth(U+00E9 e-acute)",term_emu_wcwidth(0x00E9), 1);
    check_int("wcwidth(U+2500 box)",    term_emu_wcwidth(0x2500), 1);
    check_int("wcwidth(U+4E16 CJK)",    term_emu_wcwidth(0x4E16), 2);
    check_int("wcwidth(U+3042 hiragana)",term_emu_wcwidth(0x3042), 2);
    check_int("wcwidth(U+AC00 hangul)", term_emu_wcwidth(0xAC00), 2);
    check_int("wcwidth(U+FF21 fullwidth A)", term_emu_wcwidth(0xFF21), 2);
    check_int("wcwidth(U+1F600 emoji)", term_emu_wcwidth(0x1F600), 2);
    check_int("wcwidth(U+0301 comb acute)", term_emu_wcwidth(0x0301), 0);
    check_int("wcwidth(U+200B ZWSP)",   term_emu_wcwidth(0x200B), 0);
    check_int("wcwidth(U+FE0F VS16)",   term_emu_wcwidth(0xFE0F), 0);
    check_int("wcwidth(U+303F, NARROW next to a wide block)",
              term_emu_wcwidth(0x303F), 1);
    check_int("wcwidth(U+0007 BEL)",    term_emu_wcwidth(0x07), -1);
    check_int("wcwidth(U+3000 ideographic space)", term_emu_wcwidth(0x3000), 2);

    // the range tables MUST be sorted or the binary search silently misses
    check("wcwidth range tables are sorted (binary search depends on it)",
          term_emu_tables_sorted() == 1, "sorted", "sorted");

    // -----------------------------------------------------------------------
    printf("\n-- 5. SGR --\n");
    term_sgr_t pen;

    sgr_of("\x1b[0m", &pen);
    check_hex("SGR 0 -> default fg", pen.fg, TE_COL_DEFAULT);

    sgr_of("\x1b[31m", &pen);
    check_hex("SGR 31 -> indexed 1", pen.fg, TE_COL_IDX(1));
    sgr_of("\x1b[91m", &pen);
    check_hex("SGR 91 -> indexed 9", pen.fg, TE_COL_IDX(9));
    sgr_of("\x1b[44m", &pen);
    check_hex("SGR 44 -> bg indexed 4", pen.bg, TE_COL_IDX(4));

    sgr_of("\x1b[31m\x1b[39m", &pen);
    check_hex("SGR 39 restores DEFAULT fg (was a no-op)", pen.fg, TE_COL_DEFAULT);
    sgr_of("\x1b[44m\x1b[49m", &pen);
    check_hex("SGR 49 restores DEFAULT bg (was a no-op)", pen.bg, TE_COL_DEFAULT);

    sgr_of("\x1b[38;5;196m", &pen);
    check_hex("SGR 38;5;196 -> 256-colour index", pen.fg, TE_COL_IDX(196));
    sgr_of("\x1b[48;5;21m", &pen);
    check_hex("SGR 48;5;21 -> 256-colour bg", pen.bg, TE_COL_IDX(21));
    sgr_of("\x1b[38;2;10;20;30m", &pen);
    check_hex("SGR 38;2;r;g;b -> true colour", pen.fg, TE_COL_RGB(10,20,30));
    sgr_of("\x1b[48;2;255;128;0m", &pen);
    check_hex("SGR 48;2;r;g;b -> true colour bg", pen.bg, TE_COL_RGB(255,128,0));
    sgr_of("\x1b[38:2:10:20:30m", &pen);
    check_hex("SGR 38:2:r:g:b colon form", pen.fg, TE_COL_RGB(10,20,30));
    sgr_of("\x1b[38:2::10:20:30m", &pen);
    check_hex("SGR 38:2:<cs>:r:g:b colon form", pen.fg, TE_COL_RGB(10,20,30));
    sgr_of("\x1b[38:5:196m", &pen);
    check_hex("SGR 38:5:n colon form", pen.fg, TE_COL_IDX(196));

    // the bug that made 38 dangerous: its parameters must not fall through and
    // be read as SGR codes of their own.
    sgr_of("\x1b[38;2;1;2;3;1m", &pen);
    check_hex("SGR 38;2;r;g;b then 1 -> RGB set", pen.fg, TE_COL_RGB(1,2,3));
    check_int("SGR 38;2;r;g;b then 1 -> bold also set",
              (pen.attr & TE_ATTR_BOLD) ? 1 : 0, 1);

    sgr_of("\x1b[1m", &pen);
    check_int("SGR 1 sets BOLD as an attribute", (pen.attr & TE_ATTR_BOLD) ? 1 : 0, 1);
    sgr_of("\x1b[1;31m", &pen);
    check_int("SGR 1;31 keeps bold", (pen.attr & TE_ATTR_BOLD) ? 1 : 0, 1);
    check_hex("SGR 1;31 fg is index 1, NOT 9 (old code fused them)",
              pen.fg, TE_COL_IDX(1));
    sgr_of("\x1b[1m\x1b[22m", &pen);
    check_int("SGR 22 clears bold (was impossible)", (pen.attr & TE_ATTR_BOLD) ? 1 : 0, 0);

    sgr_of("\x1b[2m", &pen);  check_int("SGR 2 dim",       (pen.attr & TE_ATTR_DIM) ? 1:0, 1);
    sgr_of("\x1b[3m", &pen);  check_int("SGR 3 italic",    (pen.attr & TE_ATTR_ITALIC) ? 1:0, 1);
    sgr_of("\x1b[4m", &pen);  check_int("SGR 4 underline", (pen.attr & TE_ATTR_UNDERLINE) ? 1:0, 1);
    sgr_of("\x1b[4m\x1b[24m", &pen);
    check_int("SGR 24 underline off", (pen.attr & TE_ATTR_UNDERLINE) ? 1:0, 0);
    sgr_of("\x1b[4:3m", &pen);
    check_int("SGR 4:3 curly underline -> underline on", (pen.attr & TE_ATTR_UNDERLINE)?1:0, 1);
    sgr_of("\x1b[4:3m\x1b[4:0m", &pen);
    check_int("SGR 4:0 underline off", (pen.attr & TE_ATTR_UNDERLINE)?1:0, 0);
    sgr_of("\x1b[4:3m", &pen);
    check_int("SGR 4:3 does NOT also set italic (colon != semicolon)",
              (pen.attr & TE_ATTR_ITALIC)?1:0, 0);
    sgr_of("\x1b[9m", &pen);  check_int("SGR 9 strikethrough", (pen.attr & TE_ATTR_STRIKE)?1:0, 1);
    sgr_of("\x1b[7m", &pen);  check_int("SGR 7 reverse",    (pen.attr & TE_ATTR_REVERSE)?1:0, 1);
    sgr_of("\x1b[7m\x1b[27m", &pen);
    check_int("SGR 27 reverse off", (pen.attr & TE_ATTR_REVERSE)?1:0, 0);

    // reverse must NOT be a colour swap at parse time: a colour set AFTER 7m
    // has to land in the slot the program named.
    sgr_of("\x1b[7m\x1b[31m", &pen);
    check_hex("reverse then 31 -> fg really is index 1", pen.fg, TE_COL_IDX(1));
    check_int("reverse then 31 -> reverse still on", (pen.attr & TE_ATTR_REVERSE)?1:0, 1);

    sgr_of("\x1b[1;4;31;44m\x1b[0m", &pen);
    check_int("SGR 0 clears every attribute", pen.attr, 0);
    check_hex("SGR 0 clears fg", pen.fg, TE_COL_DEFAULT);
    check_hex("SGR 0 clears bg", pen.bg, TE_COL_DEFAULT);

    sgr_of("\x1b[m", &pen);
    check_int("bare CSI m == SGR 0", pen.attr, 0);

    // -----------------------------------------------------------------------
    printf("\n-- 6. MOUSE ENCODING --\n");
    {
        char b[64]; int n;
        n = term_emu_mouse_x10(b, 0, 10, 5);
        check("X10 press left at (10,5)",
              n == 6 && (unsigned char)b[3] == 32 && (unsigned char)b[4] == 43 && (unsigned char)b[5] == 38,
              "CSI M 32 43 38", "CSI M 32 43 38");
        n = term_emu_mouse_x10(b, 0, 230, 5);
        check_int("X10 refuses a column it cannot encode (>222)", n, 0);
        n = term_emu_mouse_sgr(b, 0, 10, 5, 0);
        b[n] = 0;
        check("SGR press left at (10,5)", strcmp(b + 1, "[<0;11;6M") == 0, b + 1, "[<0;11;6M");
        n = term_emu_mouse_sgr(b, 0, 10, 5, 1);
        b[n] = 0;
        check("SGR release is distinguishable ('m')", strcmp(b + 1, "[<0;11;6m") == 0, b + 1, "[<0;11;6m");
        n = term_emu_mouse_sgr(b, 64, 300, 200, 0);
        b[n] = 0;
        check("SGR wheel-up beyond the X10 limit", strcmp(b + 1, "[<64;301;201M") == 0,
              b + 1, "[<64;301;201M");
    }

    // -----------------------------------------------------------------------
    printf("\n-- 7. CONTROLS AND SHORT ESCAPES --\n");
    CASE("BEL is an execute, not a print", "\x07", "[exec 07]", "");
    CASE("CR LF are executes",             "\r\n", "[exec 0D][exec 0A]", "");
    CASE("DEL is discarded",               "a\x7f" "b", "", "ab");
    CASE("ESC 7 DECSC", "\x1b" "7", "[ESC 7]", "");
    CASE("ESC 8 DECRC", "\x1b" "8", "[ESC 8]", "");
    CASE("ESC M RI",    "\x1b" "M",    "[ESC M]", "");
    CASE("ESC D IND",   "\x1b" "D",    "[ESC D]", "");
    CASE("ESC c RIS",   "\x1b" "c",    "[ESC c]", "");
    CASE("ESC H HTS",   "\x1bH",    "[ESC H]", "");
    CASE("ESC = DECKPAM","\x1b=",   "[ESC =]", "");
    CASE("CAN aborts a CSI in flight", "\x1b[12\x18" "m", "[exec 18]", "m");

    // -----------------------------------------------------------------------
    printf("\n=== %d passed, %d failed ===\n\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
