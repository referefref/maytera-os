// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// Legacy OLE2 (binary .doc/.xls/.ppt) importers, READ-ONLY, best-effort.
// Owner: agent 8 (fmt_ole2/legacy.c). Builds on the verified CFB reader (cfb.c).
//
// Coverage today:
//   doc_ole_load  IMPLEMENTED best-effort: extracts the main-document text of
//                 Word 6/95 and Word 97-2003 .doc into paragraphs, including
//                 the fast-saved (fComplex) piece-table path. Character/paragraph
//                 formatting (bold/italic, styles, tables, images) is NOT parsed.
//   xls_ole_load  STUB: BIFF (Workbook/Book stream) parsing not yet implemented.
//   ppt_ole_load  STUB: PowerPoint record stream parsing not yet implemented.
// See docs/OFFICE_SUITE_ARCHITECTURE.md and the FIB layout in [MS-DOC].
#include "formats.h"
#include "cfb.h"
#include <stdlib.h>
#include <string.h>

// ---- little-endian readers with bounds checks ----
static unsigned int  ld16(const unsigned char *b, unsigned long n, unsigned long o){
    return (o + 2 <= n) ? ((unsigned int)b[o] | ((unsigned int)b[o+1]<<8)) : 0;
}
static unsigned int  ld32(const unsigned char *b, unsigned long n, unsigned long o){
    return (o + 4 <= n) ? ((unsigned int)b[o] | ((unsigned int)b[o+1]<<8)
                         | ((unsigned int)b[o+2]<<16) | ((unsigned int)b[o+3]<<24)) : 0;
}

// ---- growable text sink ----
typedef struct { char *p; unsigned long len, cap; int fld_suppress; } sink;
static int sink_putc(sink *s, char c){
    if (s->len + 1 > s->cap){
        unsigned long nc = s->cap ? s->cap*2 : 256;
        char *np = (char*)realloc(s->p, nc);
        if (!np) return 0;
        s->p = np; s->cap = nc;
    }
    s->p[s->len++] = c;
    return 1;
}

// Map one Word character (already narrowed to a code point) into the sink,
// turning paragraph/cell marks into '\n' and dropping control noise. Returns
// 1 normally, 0 on OOM.
static int emit_char(sink *s, unsigned int u){
    // Word field codes: 0x13 begins the (hidden) instruction, 0x14 separates
    // the (visible) result, 0x15 ends the field. Suppress the instruction so we
    // keep only what Word would display (e.g. a hyperlink's text, not its URL).
    if (u == 0x13){ s->fld_suppress = 1; return 1; }
    if (u == 0x14){ s->fld_suppress = 0; return 1; }
    if (u == 0x15){ s->fld_suppress = 0; return 1; }
    if (s->fld_suppress) return 1;
    if (u == 0x0D || u == 0x07) return sink_putc(s, '\n');   // para / cell mark
    if (u == 0x0B || u == 0x0C) return sink_putc(s, '\n');   // line / page break
    if (u == 0x09) return sink_putc(s, '\t');
    if (u == 0x00A0) return sink_putc(s, ' ');               // nbsp
    if (u == 0x2013 || u == 0x2014) return sink_putc(s, '-');// en/em dash
    if (u == 0x2018 || u == 0x2019) return sink_putc(s, '\'');
    if (u == 0x201C || u == 0x201D) return sink_putc(s, '"');
    if (u == 0x2026){ if(!sink_putc(s,'.')||!sink_putc(s,'.')) return 0; return sink_putc(s,'.'); }
    if (u < 0x20) return 1;               // drop other control chars (fields, refs)
    if (u < 0x7F) return sink_putc(s, (char)u);
    if (u < 0x100) return sink_putc(s, (char)u); // latin-1 upper: keep byte
    return sink_putc(s, '?');             // other BMP: placeholder
}

// Extract `nchars` characters starting at byte `fc` in the WordDocument stream.
// compressed=1 -> one byte per char (Windows-1252 / Latin-1); 0 -> UTF-16LE.
static int extract_piece(sink *s, const unsigned char *wd, unsigned long wdlen,
                         unsigned long fc, unsigned long nchars, int compressed){
    for (unsigned long i = 0; i < nchars; i++){
        unsigned int u;
        if (compressed){
            unsigned long o = fc + i;
            if (o >= wdlen) break;
            u = wd[o];
        } else {
            unsigned long o = fc + i*2;
            if (o + 2 > wdlen) break;
            u = (unsigned int)wd[o] | ((unsigned int)wd[o+1]<<8);
        }
        if (!emit_char(s, u)) return 0;
    }
    return 1;
}

// Turn the collected text (paragraphs separated by '\n') into the doc model.
// Tolerates NULL returns from the model builders (agent 3 may still be a stub):
// it never dereferences a NULL para/run.
static void text_to_document(document *d, const char *txt, unsigned long len){
    unsigned long start = 0;
    for (unsigned long i = 0; i <= len; i++){
        if (i == len || txt[i] == '\n'){
            unsigned long plen = i - start;
            // one paragraph per line (including empty ones, to preserve spacing)
            doc_para *p = doc_add_para(d);
            if (p){
                char *line = (char*)malloc(plen + 1);
                if (line){
                    memcpy(line, txt + start, plen);
                    line[plen] = 0;
                    doc_runfmt f; memset(&f, 0, sizeof(f));
                    f.color = 0xFFFFFFFFu; // inherit
                    doc_para_add_run(p, line, f);
                    free(line);
                }
            }
            start = i + 1;
        }
    }
}

int doc_ole_load(const unsigned char *b, unsigned long n, document **out){
    if (out) *out = 0;
    if (!b || n < 512) return -1;
    cfb *c = cfb_open(b, n);
    if (!c) return -1;

    unsigned long wdlen = 0;
    const unsigned char *wd = cfb_stream(c, "WordDocument", &wdlen);
    if (!wd || wdlen < 0x60){ cfb_close(c); return -1; }

    unsigned int wIdent = ld16(wd, wdlen, 0x00);
    if (wIdent != 0xA5EC && wIdent != 0xA5DC){ cfb_close(c); return -1; } // not a Word FIB
    unsigned int nFib    = ld16(wd, wdlen, 0x02);
    unsigned int flags   = ld16(wd, wdlen, 0x0A);
    unsigned long fcMin  = ld32(wd, wdlen, 0x18);
    unsigned long fcMac  = ld32(wd, wdlen, 0x1C);
    unsigned long ccpText= ld32(wd, wdlen, 0x4C);
    int fComplex   = (flags & 0x0004) != 0;
    int fWhichTbl  = (flags & 0x0200) != 0;   // 1 -> "1Table", 0 -> "0Table"

    sink s; s.p = 0; s.len = 0; s.cap = 0; s.fld_suppress = 0;

    if (!fComplex){
        // Single contiguous run of text at [fcMin, fcMac).
        unsigned long bytelen = (fcMac > fcMin) ? (fcMac - fcMin) : 0;
        if (nFib < 105 || wIdent == 0xA5DC){
            // Word 6/95: 8-bit text, range is byte-exact.
            extract_piece(&s, wd, wdlen, fcMin, bytelen, 1);
        } else if (ccpText && bytelen == ccpText){
            extract_piece(&s, wd, wdlen, fcMin, ccpText, 1);      // 8-bit
        } else if (ccpText && bytelen == 2*ccpText){
            extract_piece(&s, wd, wdlen, fcMin, ccpText, 0);      // 16-bit
        } else {
            // Unknown: best-effort byte range as 8-bit.
            extract_piece(&s, wd, wdlen, fcMin, bytelen, 1);
        }
    } else {
        // Fast-saved: walk the piece table (plcfPcd) in the Table stream.
        unsigned long fcClx  = ld32(wd, wdlen, 0x01A2);
        unsigned long lcbClx = ld32(wd, wdlen, 0x01A6);
        const char *tblname = fWhichTbl ? "1Table" : "0Table";
        unsigned long tlen = 0;
        const unsigned char *tb = cfb_stream(c, tblname, &tlen);
        if (!tb){ tb = cfb_stream(c, fWhichTbl ? "0Table" : "1Table", &tlen); }
        if (tb && lcbClx && fcClx + lcbClx <= tlen){
            const unsigned char *clx = tb + fcClx;
            unsigned long pos = 0, end = lcbClx;
            const unsigned char *pcdt = 0; unsigned long pcdt_lcb = 0;
            // CLX = *Prc (0x01 ...) followed by one Pcdt (0x02 lcb plcfpcd)
            while (pos < end){
                unsigned char t = clx[pos];
                if (t == 0x01){
                    if (pos + 3 > end) break;
                    unsigned int cb = ld16(clx, end, pos+1);
                    pos += 3 + cb;
                } else if (t == 0x02){
                    if (pos + 5 > end) break;
                    pcdt_lcb = ld32(clx, end, pos+1);
                    pcdt = clx + pos + 5;
                    if (pos + 5 + pcdt_lcb > end) pcdt_lcb = end - (pos + 5);
                    break;
                } else break;
            }
            if (pcdt && pcdt_lcb >= 4){
                // plcfPcd: (nPieces+1) CPs (4B) then nPieces PCDs (8B).
                unsigned long nPieces = (pcdt_lcb - 4) / 12;
                const unsigned char *cps  = pcdt;
                const unsigned char *pcds = pcdt + (nPieces + 1) * 4;
                for (unsigned long i = 0; i < nPieces; i++){
                    unsigned long cpS = ld32(cps, pcdt_lcb, i*4);
                    unsigned long cpE = ld32(cps, pcdt_lcb, (i+1)*4);
                    if (cpE <= cpS) continue;
                    unsigned long nch = cpE - cpS;
                    unsigned int fcRaw = ld32(pcds, nPieces*8, i*8 + 2);
                    int compressed = (fcRaw & 0x40000000u) != 0;
                    unsigned long fc = fcRaw & 0x3FFFFFFFu;
                    if (compressed) fc /= 2;   // 8-bit pieces address bytes at fc/2
                    if (!extract_piece(&s, wd, wdlen, fc, nch, compressed)) break;
                }
            }
        }
        // Fallback if the piece table yielded nothing.
        if (s.len == 0){
            unsigned long bytelen = (fcMac > fcMin) ? (fcMac - fcMin) : 0;
            extract_piece(&s, wd, wdlen, fcMin, bytelen, 1);
        }
    }

    cfb_close(c);

    document *d = doc_new();
    if (!d){ free(s.p); return -1; }
    if (s.p && s.len) text_to_document(d, s.p, s.len);
    free(s.p);
    *out = d;
    return 0;
}

// ---- STUBS: full BIFF / PowerPoint record parsing not yet implemented. ----
// The CFB container is fully parsed (cfb_stream("Workbook"/"Book"/"PowerPoint
// Document") returns the raw record stream); only the record-level decode is
// outstanding. Kept as honest failures so the dispatcher falls through cleanly.
int xls_ole_load(const unsigned char *b, unsigned long n, workbook **out){
    (void)b; (void)n; if (out) *out = 0; return -1;
}
int ppt_ole_load(const unsigned char *b, unsigned long n, presentation **out){
    (void)b; (void)n; if (out) *out = 0; return -1;
}
