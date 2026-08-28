// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// term_emu.c - MayteraOS Terminal emulation core. See term_emu.h for WHY.
//
// No syscalls, no allocation, no GUI. Compiles with a hosted gcc so
// tests/term_emu_test.c can drive it directly.

#include "term_emu.h"

// ---------------------------------------------------------------------------
// States. This is Paul Williams' VT500 parser (https://vt100.net/emu/dec_ansi_parser)
// with the DEC-specific DCS device-control states collapsed into a single
// "collect the payload, hand it over at ST" state, because nothing in this OS
// consumes a DCS yet and the ONLY property that matters today is that a DCS
// cannot leak its payload onto the screen (which is exactly what the old
// parser did: it consumed the `P` and printed everything after it).
enum {
    S_GROUND = 0,
    S_ESCAPE,
    S_ESC_INTER,
    S_CSI_ENTRY,
    S_CSI_PARAM,
    S_CSI_INTER,
    S_CSI_IGNORE,
    S_OSC,
    S_DCS_ENTRY,
    S_DCS_PARAM,
    S_DCS_INTER,
    S_DCS_PASS,
    S_DCS_IGNORE,
    S_SOS_PM_APC,    // SOS/PM/APC: swallow until ST, hand nothing over
    S_STR_ESC        // saw ESC inside a string state; '\' completes a 7-bit ST
};

static void seq_clear(term_parser_t *p) {
    p->seq.final = 0;
    p->seq.priv = 0;
    p->seq.ninter = 0;
    p->seq.inter[0] = 0;
    p->seq.inter[1] = 0;
    p->seq.inter[2] = 0;
    p->seq.nparams = 0;
    p->cur_param = 0;
    p->cur_has_digit = 0;
    for (int i = 0; i < TE_MAX_PARAMS; i++) { p->seq.params[i] = -1; p->seq.sep[i] = ';'; }
    p->str_len = 0;
    p->str_trunc = 0;
}

void term_emu_reset(term_parser_t *p) {
    p->state = S_GROUND;
    seq_clear(p);
    p->u_cp = 0; p->u_need = 0; p->u_seen = 0; p->u_min = 0;
}

int term_emu_param(const term_seq_t *s, int i, int def) {
    if (i < 0 || i >= s->nparams) return def;
    return s->params[i] < 0 ? def : s->params[i];
}

// ---------------------------------------------------------------------------
// Parameter accumulation.
//
// The old parser did `escape_params[n] = escape_params[n]*10 + d` with NO
// overflow guard, so `ESC[99999999999999m` wrapped through INT_MIN and was
// then used as an array index in the SGR switch. Clamp instead: every real
// terminal caps parameters (xterm at 65535), and a clamped parameter is a
// wrong colour, while an overflowed one is a memory-safety bug.
#define TE_PARAM_MAX 65535

static void param_digit(term_parser_t *p, int d) {
    if (p->cur_param >= TE_MAX_PARAMS) return;   // excess params are dropped, not folded into the last
    int v = p->seq.params[p->cur_param];
    if (v < 0) v = 0;
    if (v <= (TE_PARAM_MAX - d) / 10) v = v * 10 + d;
    else v = TE_PARAM_MAX;
    p->seq.params[p->cur_param] = v;
    p->cur_has_digit = 1;
    if (p->cur_param + 1 > p->seq.nparams) p->seq.nparams = p->cur_param + 1;
}

static void param_sep(term_parser_t *p, char sep) {
    if (p->cur_param < TE_MAX_PARAMS) {
        p->seq.sep[p->cur_param] = sep;
        // An empty slot stays -1 ("omitted"), which is what makes `ESC[;5H`
        // mean "row default, column 5" rather than "row 0, column 5".
        if (p->cur_param + 1 > p->seq.nparams) p->seq.nparams = p->cur_param + 1;
    }
    p->cur_param++;
    p->cur_has_digit = 0;
    if (p->cur_param < TE_MAX_PARAMS && p->cur_param + 1 > p->seq.nparams)
        p->seq.nparams = p->cur_param + 1;
}

static void seq_finish_params(term_parser_t *p) {
    // `ESC[m` has ONE omitted parameter, not zero: SGR's default is 0, and a
    // caller that sees nparams==0 has to special-case it. Normalising here
    // means term_emu_param(s,0,def) is correct for every sequence shape.
    if (p->seq.nparams == 0) p->seq.nparams = 1;
    if (p->seq.nparams > TE_MAX_PARAMS) p->seq.nparams = TE_MAX_PARAMS;
}

static void str_push(term_parser_t *p, unsigned char c) {
    if (p->str_len < TE_MAX_STR - 1) p->str[p->str_len++] = (char)c;
    else p->str_trunc = 1;
}

// ---------------------------------------------------------------------------
// UTF-8 incremental decode.
//
// Rejects, rather than accepts, the three classes of malformed input that
// every "just mask the low bits" decoder lets through and that turn into
// grid corruption: overlong encodings (a 2-byte encoding of '/' defeats a
// path check), surrogates D800-DFFF (not scalar values), and anything above
// 10FFFF. A rejected sequence emits U+FFFD ONCE and resynchronises on the
// offending byte rather than swallowing it, so a single bad byte costs one
// replacement character, not the rest of the line.
#define TE_REPLACEMENT 0xFFFDu

static void utf8_flush_bad(term_parser_t *p, const term_cb_t *cb, void *ctx) {
    p->u_need = 0; p->u_seen = 0; p->u_cp = 0; p->u_min = 0;
    if (cb && cb->print) cb->print(ctx, TE_REPLACEMENT);
}

// Returns 1 if the byte was consumed by the decoder, 0 if the caller should
// process it as a fresh byte in the current state.
static int utf8_feed(term_parser_t *p, unsigned char c,
                     const term_cb_t *cb, void *ctx) {
    if (p->u_need > 0) {
        if ((c & 0xC0) != 0x80) {
            // Truncated sequence. Emit U+FFFD and REPROCESS this byte: it may
            // be an ESC that starts a real sequence, and swallowing it is how
            // a decoder eats the escape that would have fixed the screen.
            utf8_flush_bad(p, cb, ctx);
            return 0;
        }
        p->u_cp = (p->u_cp << 6) | (uint32_t)(c & 0x3F);
        p->u_seen++;
        if (--p->u_need == 0) {
            uint32_t cp = p->u_cp;
            p->u_cp = 0; p->u_seen = 0;
            if (cp < p->u_min || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu)
                cp = TE_REPLACEMENT;
            if (cb && cb->print) cb->print(ctx, cp);
        }
        return 1;
    }
    if (c < 0x80) return 0;                       // ASCII: not our business
    if ((c & 0xE0) == 0xC0) { p->u_cp = c & 0x1F; p->u_need = 1; p->u_min = 0x80u;    p->u_seen = 1; return 1; }
    if ((c & 0xF0) == 0xE0) { p->u_cp = c & 0x0F; p->u_need = 2; p->u_min = 0x800u;   p->u_seen = 1; return 1; }
    if ((c & 0xF8) == 0xF0) { p->u_cp = c & 0x07; p->u_need = 3; p->u_min = 0x10000u; p->u_seen = 1; return 1; }
    // 0x80-0xBF stray continuation, or 0xF8-0xFF which UTF-8 never uses.
    if (cb && cb->print) cb->print(ctx, TE_REPLACEMENT);
    return 1;
}

// ---------------------------------------------------------------------------
// Dispatch helpers

static void do_csi(term_parser_t *p, char final, const term_cb_t *cb, void *ctx) {
    seq_finish_params(p);
    p->seq.final = final;
    if (cb && cb->csi) cb->csi(ctx, &p->seq);
}

static void do_esc(term_parser_t *p, char final, const term_cb_t *cb, void *ctx) {
    p->seq.final = final;
    p->seq.nparams = 0;
    if (cb && cb->esc) cb->esc(ctx, &p->seq);
}

static void do_osc(term_parser_t *p, const term_cb_t *cb, void *ctx) {
    p->str[p->str_len] = 0;
    if (cb && cb->osc) cb->osc(ctx, p->str, p->str_len);
}

static void do_dcs(term_parser_t *p, const term_cb_t *cb, void *ctx) {
    p->str[p->str_len] = 0;
    seq_finish_params(p);
    if (cb && cb->dcs) cb->dcs(ctx, &p->seq, p->str, p->str_len);
}

// C0 controls that execute in ANY state without terminating the sequence in
// progress (the VT500 machine's "anywhere" execute set). ESC, CAN, SUB and
// the string terminators are handled separately.
static int is_exec_c0(unsigned char c) {
    return (c <= 0x17) || c == 0x19 || (c >= 0x1C && c <= 0x1F);
}

void term_emu_feed(term_parser_t *p, const unsigned char *buf, int len,
                   const term_cb_t *cb, void *ctx) {
    for (int i = 0; i < len; i++) {
        unsigned char c = buf[i];

        // --- bytes that reset from ANY state --------------------------------
        if (c == 0x1B) {                          // ESC
            if (p->u_need) utf8_flush_bad(p, cb, ctx);
            // Inside a string (OSC/DCS/APC), an ESC is only meaningful as the
            // first half of a 7-bit ST (ESC \). It is NOT resolved by peeking
            // at the next byte: a pty read can end exactly between the ESC and
            // the backslash, and a lookahead that runs off the end of the
            // buffer would drop the title of every prompt that happened to
            // straddle a read boundary - an intermittent bug that only appears
            // under load. Remember the string state instead and decide on the
            // NEXT byte, whenever it arrives.
            if (p->state == S_OSC || p->state == S_DCS_PASS ||
                p->state == S_SOS_PM_APC || p->state == S_DCS_IGNORE) {
                p->str_from = p->state;
                p->state = S_STR_ESC;
                continue;
            }
            p->state = S_ESCAPE;
            seq_clear(p);
            continue;
        }
        if (c == 0x18 || c == 0x1A) {             // CAN / SUB: abort, execute
            if (p->u_need) utf8_flush_bad(p, cb, ctx);
            p->state = S_GROUND;
            seq_clear(p);
            if (cb && cb->execute) cb->execute(ctx, c);
            continue;
        }
        if (c == 0x9C) {                          // 8-bit ST
            if (p->state == S_OSC) { do_osc(p, cb, ctx); p->state = S_GROUND; seq_clear(p); continue; }
            if (p->state == S_DCS_PASS) { do_dcs(p, cb, ctx); p->state = S_GROUND; seq_clear(p); continue; }
            if (p->state == S_SOS_PM_APC || p->state == S_DCS_IGNORE) { p->state = S_GROUND; seq_clear(p); continue; }
        }

        switch (p->state) {

        // -------------------------------------------------------------------
        case S_GROUND:
            if (utf8_feed(p, c, cb, ctx)) continue;
            if (is_exec_c0(c)) { if (cb && cb->execute) cb->execute(ctx, c); continue; }
            if (c == 0x7F) continue;              // DEL is discarded, per DEC
            if (c < 0x20) { if (cb && cb->execute) cb->execute(ctx, c); continue; }
            if (cb && cb->print) cb->print(ctx, (uint32_t)c);
            continue;

        // -------------------------------------------------------------------
        case S_ESCAPE:
            if (is_exec_c0(c)) { if (cb && cb->execute) cb->execute(ctx, c); continue; }
            if (c == 0x7F) continue;
            if (c == '[') { p->state = S_CSI_ENTRY; continue; }
            if (c == ']') { p->state = S_OSC; p->str_len = 0; p->str_trunc = 0; continue; }
            if (c == 'P') { p->state = S_DCS_ENTRY; continue; }
            if (c == 'X' || c == '^' || c == '_') { p->state = S_SOS_PM_APC; continue; }
            if (c >= 0x20 && c <= 0x2F) {         // intermediate: ( ) * + # SP ! " $ %
                if (p->seq.ninter < TE_MAX_INTER) p->seq.inter[p->seq.ninter++] = (char)c;
                p->state = S_ESC_INTER;
                continue;
            }
            if (c >= 0x30 && c <= 0x7E) {         // final
                do_esc(p, (char)c, cb, ctx);
                p->state = S_GROUND;
                seq_clear(p);
                continue;
            }
            p->state = S_GROUND;
            continue;

        // -------------------------------------------------------------------
        // THIS STATE IS THE FIX FOR THE `ESC ( B` LEAK. The old parser hit its
        // `default:` on the '(' and returned to GROUND, so the 'B' printed. The
        // comment above that default said "Consumed, not leaked" - it consumed
        // exactly one byte of a two-byte sequence. Charset designation is still
        // not MODELLED (nothing here switches to the DEC graphics set), but it
        // is now correctly SWALLOWED, which is the difference between "line
        // drawing is unsupported" and "the letter B appears in your prompt".
        case S_ESC_INTER:
            if (is_exec_c0(c)) { if (cb && cb->execute) cb->execute(ctx, c); continue; }
            if (c == 0x7F) continue;
            if (c >= 0x20 && c <= 0x2F) {
                if (p->seq.ninter < TE_MAX_INTER) p->seq.inter[p->seq.ninter++] = (char)c;
                continue;
            }
            if (c >= 0x30 && c <= 0x7E) {
                do_esc(p, (char)c, cb, ctx);
                p->state = S_GROUND;
                seq_clear(p);
                continue;
            }
            p->state = S_GROUND;
            continue;

        // -------------------------------------------------------------------
        case S_CSI_ENTRY:
        case S_CSI_PARAM:
            if (is_exec_c0(c)) { if (cb && cb->execute) cb->execute(ctx, c); continue; }
            if (c == 0x7F) continue;
            if (c >= '0' && c <= '9') { param_digit(p, c - '0'); p->state = S_CSI_PARAM; continue; }
            if (c == ';' || c == ':') { param_sep(p, (char)c); p->state = S_CSI_PARAM; continue; }
            if (p->state == S_CSI_ENTRY && c >= 0x3C && c <= 0x3F) {   // < = > ?
                p->seq.priv = (char)c;
                p->state = S_CSI_PARAM;
                continue;
            }
            if (c >= 0x3C && c <= 0x3F) { p->state = S_CSI_IGNORE; continue; }  // a marker after a param is malformed
            if (c >= 0x20 && c <= 0x2F) {         // intermediate: SP ! " # $ % & ' ( ) * + , - . /
                if (p->seq.ninter < TE_MAX_INTER) p->seq.inter[p->seq.ninter++] = (char)c;
                else { p->state = S_CSI_IGNORE; continue; }
                p->state = S_CSI_INTER;
                continue;
            }
            if (c >= 0x40 && c <= 0x7E) {         // final
                do_csi(p, (char)c, cb, ctx);
                p->state = S_GROUND;
                seq_clear(p);
                continue;
            }
            p->state = S_CSI_IGNORE;
            continue;

        // -------------------------------------------------------------------
        // THIS STATE IS THE FIX FOR `ESC[ SP q` (DECSCUSR, the cursor-shape
        // sequence every modern vim emits) AND `ESC[ ! p` (DECSTR soft reset).
        // The old parser treated the intermediate as the final byte, then
        // printed the real final byte: a stray `q` or `p` in the output.
        case S_CSI_INTER:
            if (is_exec_c0(c)) { if (cb && cb->execute) cb->execute(ctx, c); continue; }
            if (c == 0x7F) continue;
            if (c >= 0x20 && c <= 0x2F) {
                if (p->seq.ninter < TE_MAX_INTER) p->seq.inter[p->seq.ninter++] = (char)c;
                else { p->state = S_CSI_IGNORE; continue; }
                continue;
            }
            if (c >= 0x30 && c <= 0x3F) { p->state = S_CSI_IGNORE; continue; }
            if (c >= 0x40 && c <= 0x7E) {
                do_csi(p, (char)c, cb, ctx);
                p->state = S_GROUND;
                seq_clear(p);
                continue;
            }
            p->state = S_CSI_IGNORE;
            continue;

        // -------------------------------------------------------------------
        case S_CSI_IGNORE:
            if (is_exec_c0(c)) { if (cb && cb->execute) cb->execute(ctx, c); continue; }
            if (c >= 0x40 && c <= 0x7E) { p->state = S_GROUND; seq_clear(p); }
            continue;

        // -------------------------------------------------------------------
        // OSC. `ESC ] 0 ; some title BEL` and the ST-terminated form.
        //
        // The old parser had NO OSC state at all: it consumed the ']' in its
        // ESC default branch and printed `0;some title` on the screen. Every
        // shell that sets a window title (bash's PROMPT_COMMAND, zsh, vim, ssh)
        // did this on EVERY prompt.
        case S_OSC:
            if (c == 0x07) { do_osc(p, cb, ctx); p->state = S_GROUND; seq_clear(p); continue; }
            if (c == 0x00) continue;
            str_push(p, c);
            continue;

        // -------------------------------------------------------------------
        case S_DCS_ENTRY:
        case S_DCS_PARAM:
            if (c == 0x7F) continue;
            if (c >= '0' && c <= '9') { param_digit(p, c - '0'); p->state = S_DCS_PARAM; continue; }
            if (c == ';' || c == ':') { param_sep(p, (char)c); p->state = S_DCS_PARAM; continue; }
            if (p->state == S_DCS_ENTRY && c >= 0x3C && c <= 0x3F) { p->seq.priv = (char)c; p->state = S_DCS_PARAM; continue; }
            if (c >= 0x20 && c <= 0x2F) {
                if (p->seq.ninter < TE_MAX_INTER) p->seq.inter[p->seq.ninter++] = (char)c;
                p->state = S_DCS_INTER;
                continue;
            }
            if (c >= 0x40 && c <= 0x7E) { p->seq.final = (char)c; p->str_len = 0; p->state = S_DCS_PASS; continue; }
            p->state = S_DCS_IGNORE;
            continue;

        case S_DCS_INTER:
            if (c >= 0x20 && c <= 0x2F) {
                if (p->seq.ninter < TE_MAX_INTER) p->seq.inter[p->seq.ninter++] = (char)c;
                continue;
            }
            if (c >= 0x40 && c <= 0x7E) { p->seq.final = (char)c; p->str_len = 0; p->state = S_DCS_PASS; continue; }
            p->state = S_DCS_IGNORE;
            continue;

        case S_DCS_PASS:
            str_push(p, c);
            continue;

        case S_DCS_IGNORE:
        case S_SOS_PM_APC:
            continue;

        // The byte after an ESC that arrived inside a string.
        case S_STR_ESC:
            if (c == '\\') {                      // ST: the string is complete
                if (p->str_from == S_OSC) do_osc(p, cb, ctx);
                else if (p->str_from == S_DCS_PASS) do_dcs(p, cb, ctx);
                p->state = S_GROUND;
                seq_clear(p);
                continue;
            }
            // Not an ST. The application abandoned the string mid-way; the ESC
            // starts a real sequence. Drop the string and REPROCESS this byte
            // in S_ESCAPE, so `ESC ] junk ESC [ 2 J` still clears the screen.
            seq_clear(p);
            p->state = S_ESCAPE;
            i--;                                  // re-read c as the byte after ESC
            continue;

        default:
            p->state = S_GROUND;
            continue;
        }
    }
}

// ---------------------------------------------------------------------------
// wcwidth
//
// Ranges are Unicode East_Asian_Width W/F for the wide set and the General
// Category Mn/Me/Cf blocks that a terminal actually meets for the zero-width
// set. This is a SUBSET of the full Unicode property tables, and it is
// deliberately a subset: shipping the full tables would add ~12 KB of data to
// every terminal for scripts this OS has no font coverage for. What it does
// cover is stated so nobody has to guess: Latin/Greek/Cyrillic combining
// marks, Hebrew points, Arabic diacritics, the Indic and Thai mark blocks,
// Hangul jamo medial/final, the format characters (ZWSP/ZWNJ/ZWJ/LRM/RLM),
// variation selectors, and the CJK/Hangul/fullwidth/emoji wide blocks.
//
// Anything outside the tables gets width 1, which is the safe default: a
// mark rendered one cell wide misaligns one column, whereas a wide character
// treated as narrow misaligns the whole rest of the row AND leaves the grid
// disagreeing with the application about where the cursor is.

typedef struct { uint32_t lo, hi; } te_range_t;

static const te_range_t k_zero[] = {
    { 0x00AD, 0x00AD },   // SOFT HYPHEN
    { 0x0300, 0x036F }, { 0x0483, 0x0489 },
    { 0x0591, 0x05BD }, { 0x05BF, 0x05BF }, { 0x05C1, 0x05C2 },
    { 0x05C4, 0x05C5 }, { 0x05C7, 0x05C7 },
    { 0x0610, 0x061A }, { 0x064B, 0x065F }, { 0x0670, 0x0670 },
    { 0x06D6, 0x06DC }, { 0x06DF, 0x06E4 }, { 0x06E7, 0x06E8 },
    { 0x06EA, 0x06ED }, { 0x0711, 0x0711 }, { 0x0730, 0x074A },
    { 0x07A6, 0x07B0 }, { 0x07EB, 0x07F3 },
    { 0x0816, 0x0819 }, { 0x081B, 0x0823 }, { 0x0825, 0x0827 },
    { 0x0829, 0x082D }, { 0x0859, 0x085B },
    { 0x08E3, 0x0902 }, { 0x093A, 0x093A }, { 0x093C, 0x093C },
    { 0x0941, 0x0948 }, { 0x094D, 0x094D }, { 0x0951, 0x0957 },
    { 0x0962, 0x0963 }, { 0x0981, 0x0981 }, { 0x09BC, 0x09BC },
    { 0x09C1, 0x09C4 }, { 0x09CD, 0x09CD }, { 0x09E2, 0x09E3 },
    { 0x0A01, 0x0A02 }, { 0x0A3C, 0x0A3C }, { 0x0A41, 0x0A42 },
    { 0x0A47, 0x0A48 }, { 0x0A4B, 0x0A4D }, { 0x0A70, 0x0A71 },
    { 0x0A81, 0x0A82 }, { 0x0ABC, 0x0ABC }, { 0x0AC1, 0x0AC5 },
    { 0x0AC7, 0x0AC8 }, { 0x0ACD, 0x0ACD }, { 0x0AE2, 0x0AE3 },
    { 0x0B01, 0x0B01 }, { 0x0B3C, 0x0B3C }, { 0x0B3F, 0x0B3F },
    { 0x0B41, 0x0B44 }, { 0x0B4D, 0x0B56 }, { 0x0B62, 0x0B63 },
    { 0x0B82, 0x0B82 }, { 0x0BC0, 0x0BC0 }, { 0x0BCD, 0x0BCD },
    { 0x0C00, 0x0C00 }, { 0x0C3E, 0x0C40 }, { 0x0C46, 0x0C56 },
    { 0x0C62, 0x0C63 }, { 0x0C81, 0x0C81 }, { 0x0CBC, 0x0CBC },
    { 0x0CBF, 0x0CBF }, { 0x0CC6, 0x0CC6 }, { 0x0CCC, 0x0CCD },
    { 0x0CE2, 0x0CE3 }, { 0x0D01, 0x0D01 }, { 0x0D41, 0x0D44 },
    { 0x0D4D, 0x0D4D }, { 0x0D62, 0x0D63 }, { 0x0DCA, 0x0DCA },
    { 0x0DD2, 0x0DD6 }, { 0x0E31, 0x0E31 }, { 0x0E34, 0x0E3A },
    { 0x0E47, 0x0E4E }, { 0x0EB1, 0x0EB1 }, { 0x0EB4, 0x0EBC },
    { 0x0EC8, 0x0ECD }, { 0x0F18, 0x0F19 }, { 0x0F35, 0x0F35 },
    { 0x0F37, 0x0F37 }, { 0x0F39, 0x0F39 }, { 0x0F71, 0x0F7E },
    { 0x0F80, 0x0F84 }, { 0x0F86, 0x0F87 }, { 0x0F8D, 0x0FBC },
    { 0x0FC6, 0x0FC6 }, { 0x102D, 0x1030 }, { 0x1032, 0x1037 },
    { 0x1039, 0x103A }, { 0x103D, 0x103E }, { 0x1058, 0x1059 },
    { 0x105E, 0x1060 }, { 0x1071, 0x1074 }, { 0x1082, 0x1082 },
    { 0x1085, 0x1086 }, { 0x108D, 0x108D }, { 0x109D, 0x109D },
    { 0x135D, 0x135F }, { 0x1712, 0x1714 }, { 0x1732, 0x1734 },
    { 0x1752, 0x1753 }, { 0x1772, 0x1773 }, { 0x17B4, 0x17B5 },
    { 0x17B7, 0x17BD }, { 0x17C6, 0x17C6 }, { 0x17C9, 0x17D3 },
    { 0x17DD, 0x17DD }, { 0x180B, 0x180E }, { 0x18A9, 0x18A9 },
    { 0x1920, 0x1922 }, { 0x1927, 0x1928 }, { 0x1932, 0x1932 },
    { 0x1939, 0x193B }, { 0x1A17, 0x1A18 }, { 0x1A60, 0x1A60 },
    { 0x1A75, 0x1A7F }, { 0x1AB0, 0x1AFF }, { 0x1B00, 0x1B03 },
    { 0x1B34, 0x1B34 }, { 0x1B36, 0x1B3A }, { 0x1B3C, 0x1B3C },
    { 0x1B42, 0x1B42 }, { 0x1B6B, 0x1B73 }, { 0x1DC0, 0x1DFF },
    { 0x200B, 0x200F },   // ZWSP ZWNJ ZWJ LRM RLM
    { 0x202A, 0x202E }, { 0x2060, 0x2064 }, { 0x206A, 0x206F },
    { 0x20D0, 0x20F0 }, { 0x2CEF, 0x2CF1 }, { 0x2D7F, 0x2D7F },
    { 0x2DE0, 0x2DFF }, { 0x302A, 0x302D }, { 0x3099, 0x309A },
    { 0xA66F, 0xA672 }, { 0xA674, 0xA67D }, { 0xA69E, 0xA69F },
    { 0xA806, 0xA806 }, { 0xA80B, 0xA80B }, { 0xA825, 0xA826 },
    { 0xA8C4, 0xA8C5 }, { 0xA8E0, 0xA8F1 }, { 0xA926, 0xA92D },
    { 0xA947, 0xA951 }, { 0xA980, 0xA982 }, { 0xA9B3, 0xA9B3 },
    { 0xAAB0, 0xAAB0 }, { 0xAAB2, 0xAAB4 }, { 0xAAB7, 0xAAB8 },
    { 0xAABE, 0xAABF }, { 0xAAC1, 0xAAC1 },
    { 0xFB1E, 0xFB1E }, { 0xFE00, 0xFE0F },   // variation selectors
    { 0xFE20, 0xFE2F }, { 0xFEFF, 0xFEFF },   // BOM / ZWNBSP
    { 0xFFF9, 0xFFFB },
    { 0x101FD, 0x101FD }, { 0x10A01, 0x10A0F }, { 0x11046, 0x11046 },
    { 0x1D165, 0x1D169 }, { 0x1D16D, 0x1D182 }, { 0x1D185, 0x1D18B },
    { 0x1D1AA, 0x1D1AD }, { 0x1D242, 0x1D244 },
    { 0xE0001, 0xE0001 }, { 0xE0020, 0xE007F }, { 0xE0100, 0xE01EF },
};

static const te_range_t k_wide[] = {
    { 0x1100, 0x115F },   // Hangul Jamo initial consonants
    { 0x231A, 0x231B }, { 0x2329, 0x232A },
    { 0x23E9, 0x23EC }, { 0x23F0, 0x23F0 }, { 0x23F3, 0x23F3 },
    { 0x25FD, 0x25FE }, { 0x2614, 0x2615 }, { 0x2648, 0x2653 },
    { 0x267F, 0x267F }, { 0x2693, 0x2693 }, { 0x26A1, 0x26A1 },
    { 0x26AA, 0x26AB }, { 0x26BD, 0x26BE }, { 0x26C4, 0x26C5 },
    { 0x26CE, 0x26CE }, { 0x26D4, 0x26D4 }, { 0x26EA, 0x26EA },
    { 0x26F2, 0x26F3 }, { 0x26F5, 0x26F5 }, { 0x26FA, 0x26FA },
    { 0x26FD, 0x26FD }, { 0x2705, 0x2705 }, { 0x270A, 0x270B },
    { 0x2728, 0x2728 }, { 0x274C, 0x274C }, { 0x274E, 0x274E },
    { 0x2753, 0x2755 }, { 0x2757, 0x2757 }, { 0x2795, 0x2797 },
    { 0x27B0, 0x27B0 }, { 0x27BF, 0x27BF }, { 0x2B1B, 0x2B1C },
    { 0x2B50, 0x2B50 }, { 0x2B55, 0x2B55 },
    { 0x2E80, 0x303E },   // CJK radicals .. ideographic marks (0x303F is NARROW)
    { 0x3041, 0x33FF },   // kana, bopomofo, Hangul compat jamo, CJK squared
    { 0x3400, 0x4DBF },   // CJK ext A
    { 0x4E00, 0x9FFF },   // CJK unified
    { 0xA000, 0xA4CF },   // Yi
    { 0xA960, 0xA97F },   // Hangul Jamo ext A
    { 0xAC00, 0xD7A3 },   // Hangul syllables
    { 0xF900, 0xFAFF },   // CJK compatibility ideographs
    { 0xFE10, 0xFE19 }, { 0xFE30, 0xFE6F },
    { 0xFF00, 0xFF60 },   // fullwidth forms
    { 0xFFE0, 0xFFE6 },
    { 0x16FE0, 0x16FE4 }, { 0x17000, 0x18AFF },
    { 0x1B000, 0x1B2FF },
    { 0x1F004, 0x1F004 }, { 0x1F0CF, 0x1F0CF },
    { 0x1F18E, 0x1F18E }, { 0x1F191, 0x1F19A },
    { 0x1F200, 0x1F320 }, { 0x1F32D, 0x1F335 }, { 0x1F337, 0x1F37C },
    { 0x1F37E, 0x1F393 }, { 0x1F3A0, 0x1F3CA }, { 0x1F3CF, 0x1F3D3 },
    { 0x1F3E0, 0x1F3F0 }, { 0x1F3F4, 0x1F3F4 }, { 0x1F3F8, 0x1F43E },
    { 0x1F440, 0x1F440 }, { 0x1F442, 0x1F4FC }, { 0x1F4FF, 0x1F53D },
    { 0x1F54B, 0x1F54E }, { 0x1F550, 0x1F567 }, { 0x1F57A, 0x1F57A },
    { 0x1F595, 0x1F596 }, { 0x1F5A4, 0x1F5A4 }, { 0x1F5FB, 0x1F64F },
    { 0x1F680, 0x1F6C5 }, { 0x1F6CC, 0x1F6CC }, { 0x1F6D0, 0x1F6D2 },
    { 0x1F6EB, 0x1F6EC }, { 0x1F6F4, 0x1F6F9 },
    { 0x1F910, 0x1F9FF },
    { 0x20000, 0x2FFFD }, { 0x30000, 0x3FFFD },
};

static int in_ranges(uint32_t cp, const te_range_t *t, int n) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < t[mid].lo) hi = mid - 1;
        else if (cp > t[mid].hi) lo = mid + 1;
        else return 1;
    }
    return 0;
}

int term_emu_wcwidth(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return -1;   // C0 / DEL / C1
    if (cp < 0xA0) return 1;                                  // fast path: Latin-1 printables
    if (in_ranges(cp, k_zero, (int)(sizeof(k_zero) / sizeof(k_zero[0])))) return 0;
    if (in_ranges(cp, k_wide, (int)(sizeof(k_wide) / sizeof(k_wide[0])))) return 2;
    return 1;
}

// ---------------------------------------------------------------------------
// SGR

void term_emu_sgr_reset(term_sgr_t *pen) {
    pen->fg = TE_COL_DEFAULT;
    pen->bg = TE_COL_DEFAULT;
    pen->attr = 0;
}

// Parse an extended colour starting at params[i] (which is 38 or 48).
// Returns the index of the LAST parameter consumed, and writes the colour to
// *out. On anything malformed it consumes what it can and leaves *out alone -
// the one thing it must never do is fall through and let 5, 2 or a raw RGB
// component be interpreted as an SGR code of its own (which is precisely how a
// terminal that mishandles 38 turns a colour request into random bold/italic).
static int sgr_ext_color(const term_seq_t *s, int i, uint32_t *out) {
    // Colon form: 38:2:...:r:g:b or 38:5:n. Detected by the separator that
    // ended params[i] being ':'.
    if (i < s->nparams && s->sep[i] == ':') {
        int j = i + 1;
        int group_end = j;
        while (group_end < s->nparams && s->sep[group_end] == ':') group_end++;
        // group_end is the last member of the colon group (its sep is ';' or
        // it is the final parameter).
        int nsub = group_end - i;            // members after params[i]
        int mode = term_emu_param(s, j, -1);
        if (mode == 5 && nsub >= 1) {
            int n = term_emu_param(s, j + 1, 0);
            if (n >= 0 && n <= 255) *out = TE_COL_IDX(n);
        } else if (mode == 2) {
            // 38:2:<cs>:r:g:b is 5 members after the 38; 38:2:r:g:b is 4.
            // Distinguishing by COUNT is what libvte does and is the only way,
            // since the colour-space id is frequently written empty.
            int base = (nsub >= 5) ? j + 2 : j + 1;
            int r = term_emu_param(s, base, 0);
            int g = term_emu_param(s, base + 1, 0);
            int b = term_emu_param(s, base + 2, 0);
            *out = TE_COL_RGB(r, g, b);
        }
        return group_end;
    }
    // Semicolon form: 38;5;n or 38;2;r;g;b.
    int mode = term_emu_param(s, i + 1, -1);
    if (mode == 5) {
        int n = term_emu_param(s, i + 2, 0);
        if (n >= 0 && n <= 255) *out = TE_COL_IDX(n);
        return i + 2;
    }
    if (mode == 2) {
        int r = term_emu_param(s, i + 2, 0);
        int g = term_emu_param(s, i + 3, 0);
        int b = term_emu_param(s, i + 4, 0);
        *out = TE_COL_RGB(r, g, b);
        return i + 4;
    }
    // Unknown extended-colour mode: consume only the mode selector so the rest
    // is re-examined as ordinary SGR codes rather than silently swallowed.
    return i + 1;
}

void term_emu_sgr(term_sgr_t *pen, const term_seq_t *s) {
    for (int i = 0; i < s->nparams; i++) {
        int p = term_emu_param(s, i, 0);
        switch (p) {
        case 0:  term_emu_sgr_reset(pen); break;
        case 1:  pen->attr |= TE_ATTR_BOLD; break;
        case 2:  pen->attr |= TE_ATTR_DIM; break;
        case 3:  pen->attr |= TE_ATTR_ITALIC; break;
        case 4:
            // 4:0 is "underline off"; 4:1..4:5 are underline STYLES (straight,
            // double, curly, dotted, dashed). This terminal draws one style, so
            // every non-zero sub-value means "underline on" - but 4:0 MUST turn
            // it off, or a program using the extended form can never clear it.
            if (s->sep[i] == ':' && term_emu_param(s, i + 1, 1) == 0) {
                pen->attr &= (uint8_t)~TE_ATTR_UNDERLINE;
                i++;
            } else {
                pen->attr |= TE_ATTR_UNDERLINE;
                if (s->sep[i] == ':') i++;
            }
            break;
        case 5:
        case 6:  pen->attr |= TE_ATTR_BLINK; break;
        case 7:  pen->attr |= TE_ATTR_REVERSE; break;
        case 8:  pen->attr |= TE_ATTR_HIDDEN; break;
        case 9:  pen->attr |= TE_ATTR_STRIKE; break;
        case 21: // ECMA-48 says double-underline, xterm says bold-off. Every
                 // terminfo that emits 21 means bold-off; follow the practice.
        case 22: pen->attr &= (uint8_t)~(TE_ATTR_BOLD | TE_ATTR_DIM); break;
        case 23: pen->attr &= (uint8_t)~TE_ATTR_ITALIC; break;
        case 24: pen->attr &= (uint8_t)~TE_ATTR_UNDERLINE; break;
        case 25: pen->attr &= (uint8_t)~TE_ATTR_BLINK; break;
        case 27: pen->attr &= (uint8_t)~TE_ATTR_REVERSE; break;
        case 28: pen->attr &= (uint8_t)~TE_ATTR_HIDDEN; break;
        case 29: pen->attr &= (uint8_t)~TE_ATTR_STRIKE; break;
        case 38: i = sgr_ext_color(s, i, &pen->fg); break;
        case 39: pen->fg = TE_COL_DEFAULT; break;
        case 48: i = sgr_ext_color(s, i, &pen->bg); break;
        case 49: pen->bg = TE_COL_DEFAULT; break;
        default:
            if (p >= 30 && p <= 37)        pen->fg = TE_COL_IDX(p - 30);
            else if (p >= 40 && p <= 47)   pen->bg = TE_COL_IDX(p - 40);
            else if (p >= 90 && p <= 97)   pen->fg = TE_COL_IDX(p - 90 + 8);
            else if (p >= 100 && p <= 107) pen->bg = TE_COL_IDX(p - 100 + 8);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Mouse encoders

static int put_u(char *out, int n, int v) {
    char tmp[12];
    int t = 0;
    if (v == 0) tmp[t++] = '0';
    while (v > 0) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0) out[n++] = tmp[--t];
    return n;
}

// ---------------------------------------------------------------------------
// The single copy of the DEC private-mode state (see term_emu.h).
term_modes_t g_term_modes = { 0, 0, 0, 0 };

void term_emu_modes_reset(void) {
    g_term_modes.mouse_mode = 0;
    g_term_modes.mouse_sgr = 0;
    g_term_modes.bracketed_paste = 0;
    g_term_modes.focus_events = 0;
}

int term_emu_mouse_x10(char *out, int btn, int col, int row) {
    // CSI M Cb Cx Cy, each biased by 32. A coordinate above 222 would need a
    // byte above 255, so the legacy encoding simply CANNOT address it; return
    // 0 and let the caller stay silent rather than send a wrong position.
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col > 222 || row > 222) return 0;
    out[0] = 0x1B; out[1] = '['; out[2] = 'M';
    out[3] = (char)(32 + btn);
    out[4] = (char)(32 + col + 1);
    out[5] = (char)(32 + row + 1);
    return 6;
}

int term_emu_tables_sorted(void) {
    const te_range_t *t; int n;
    t = k_zero; n = (int)(sizeof(k_zero) / sizeof(k_zero[0]));
    for (int i = 0; i < n; i++) {
        if (t[i].lo > t[i].hi) return 0;
        if (i && t[i].lo <= t[i - 1].hi) return 0;
    }
    t = k_wide; n = (int)(sizeof(k_wide) / sizeof(k_wide[0]));
    for (int i = 0; i < n; i++) {
        if (t[i].lo > t[i].hi) return 0;
        if (i && t[i].lo <= t[i - 1].hi) return 0;
    }
    return 1;
}

int term_emu_mouse_sgr(char *out, int btn, int col, int row, int release) {
    // CSI < Cb ; Cx ; Cy (M|m). No coordinate limit, and press and release are
    // distinguishable, which the legacy encoding cannot do (it reports every
    // release as button 3, so an application cannot tell WHICH button was let
    // go). This is why every modern TUI asks for ?1006.
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    int n = 0;
    out[n++] = 0x1B; out[n++] = '['; out[n++] = '<';
    n = put_u(out, n, btn);
    out[n++] = ';';
    n = put_u(out, n, col + 1);
    out[n++] = ';';
    n = put_u(out, n, row + 1);
    out[n++] = release ? 'm' : 'M';
    return n;
}
