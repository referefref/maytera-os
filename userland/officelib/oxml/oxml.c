#include "oxml.h"
#include <stdlib.h>
#include <string.h>

// Small, strict, namespace-aware XML pull-parser + writer for the office suite.
// OURS (no third-party). OOXML/ODF XML is well-formed, so a one-pass pull parser
// is enough. The parser BORROWS `xml` (never copies the whole document): only
// the current event's names/text/attribute values are materialised (unescaped)
// into an internal scratch buffer, and every returned pointer is valid until the
// next oxml_next() call. Namespace prefixes resolve to URIs honouring nesting.

// ---- growable byte buffer (scratch: append-only within one event) -----------
typedef struct { char *p; unsigned long len, cap; } sbuf;

static int sb_reserve(sbuf *s, unsigned long extra) {
    if (s->len + extra <= s->cap) return 1;
    unsigned long nc = s->cap ? s->cap : 64;
    while (nc < s->len + extra) nc *= 2;
    char *np = (char *)realloc(s->p, nc);
    if (!np) return 0;
    s->p = np; s->cap = nc; return 1;
}
static void sb_reset(sbuf *s) { s->len = 0; }
static void sb_free(sbuf *s) { free(s->p); s->p = 0; s->len = s->cap = 0; }
// append n raw bytes, return their start offset (or (unsigned long)-1 on OOM)
static unsigned long sb_add(sbuf *s, const char *d, unsigned long n) {
    if (!sb_reserve(s, n)) return (unsigned long)-1;
    unsigned long off = s->len;
    if (n) memcpy(s->p + off, d, n);
    s->len += n;
    return off;
}
static int sb_putc(sbuf *s, char c) {
    if (!sb_reserve(s, 1)) return 0;
    s->p[s->len++] = c; return 1;
}

// ---- namespace binding stack (persists across events; owns its strings) -----
typedef struct { char *prefix; char *uri; int level; } nsbind;

// ---- attribute record (offsets into scratch, resolved at accessor time) -----
typedef struct { unsigned long local_off, ns_off, val_off; } attr_t;

struct oxml {
    const char *buf;
    unsigned long len, pos;
    oxml_evt cur;
    int level;          // running nesting depth
    int depth;          // depth reported for the current event
    sbuf sc;            // per-event scratch (reset each next())
    unsigned long local_off, ns_off, text_off;
    attr_t *attrs; int nattr, cattr;
    nsbind *ns;   int nns, cns;
    // deferred END for a self-closing tag <a/>
    int pend;
    sbuf pendbuf;       // persistent copy of the pending END's local + ns
    unsigned long pend_local_off, pend_ns_off;
    int pend_level;
};

#define XML_NS   "http://www.w3.org/XML/1998/namespace"
#define XMLNS_NS "http://www.w3.org/2000/xmlns/"

static int is_ws(char c) { return c==' '||c=='\t'||c=='\n'||c=='\r'; }
static int is_name_end(char c) { return is_ws(c)||c=='>'||c=='/'||c=='='||c=='\0'; }

// ---- entity unescape: copy [src,src+n) into scratch, decoding &..; refs ------
// returns start offset (NUL-terminated) or (unsigned long)-1 on OOM
static unsigned long unescape_into(sbuf *s, const char *src, unsigned long n) {
    unsigned long off = s->len;
    unsigned long i = 0;
    while (i < n) {
        char c = src[i];
        if (c == '&') {
            // find ';'
            unsigned long j = i + 1;
            while (j < n && src[j] != ';' && (j - i) < 12) j++;
            if (j < n && src[j] == ';') {
                unsigned long elen = j - (i + 1);
                const char *e = src + i + 1;
                int handled = 1;
                if (elen == 3 && !memcmp(e, "amp", 3))      { if(!sb_putc(s,'&')) return (unsigned long)-1; }
                else if (elen == 2 && !memcmp(e, "lt", 2))  { if(!sb_putc(s,'<')) return (unsigned long)-1; }
                else if (elen == 2 && !memcmp(e, "gt", 2))  { if(!sb_putc(s,'>')) return (unsigned long)-1; }
                else if (elen == 4 && !memcmp(e, "quot", 4)){ if(!sb_putc(s,'"')) return (unsigned long)-1; }
                else if (elen == 4 && !memcmp(e, "apos", 4)){ if(!sb_putc(s,'\'')) return (unsigned long)-1; }
                else if (elen >= 2 && e[0] == '#') {
                    // numeric char ref, decimal or hex, encode as UTF-8
                    unsigned long cp = 0; int ok = 1; unsigned long k = 1;
                    if (e[1] == 'x' || e[1] == 'X') {
                        k = 2; if (k >= elen) ok = 0;
                        for (; k < elen; k++) {
                            char h = e[k]; int d;
                            if (h>='0'&&h<='9') d=h-'0';
                            else if (h>='a'&&h<='f') d=h-'a'+10;
                            else if (h>='A'&&h<='F') d=h-'A'+10;
                            else { ok=0; break; }
                            cp = cp*16 + (unsigned)d;
                        }
                    } else {
                        for (; k < elen; k++) {
                            char h = e[k];
                            if (h<'0'||h>'9') { ok=0; break; }
                            cp = cp*10 + (unsigned)(h-'0');
                        }
                    }
                    if (ok) {
                        // UTF-8 encode cp
                        if (cp < 0x80) { if(!sb_putc(s,(char)cp)) return (unsigned long)-1; }
                        else if (cp < 0x800) {
                            if(!sb_putc(s,(char)(0xC0|(cp>>6)))) return (unsigned long)-1;
                            if(!sb_putc(s,(char)(0x80|(cp&0x3F)))) return (unsigned long)-1;
                        } else if (cp < 0x10000) {
                            if(!sb_putc(s,(char)(0xE0|(cp>>12)))) return (unsigned long)-1;
                            if(!sb_putc(s,(char)(0x80|((cp>>6)&0x3F)))) return (unsigned long)-1;
                            if(!sb_putc(s,(char)(0x80|(cp&0x3F)))) return (unsigned long)-1;
                        } else {
                            if(!sb_putc(s,(char)(0xF0|(cp>>18)))) return (unsigned long)-1;
                            if(!sb_putc(s,(char)(0x80|((cp>>12)&0x3F)))) return (unsigned long)-1;
                            if(!sb_putc(s,(char)(0x80|((cp>>6)&0x3F)))) return (unsigned long)-1;
                            if(!sb_putc(s,(char)(0x80|(cp&0x3F)))) return (unsigned long)-1;
                        }
                    } else handled = 0;
                }
                else handled = 0;
                if (handled) { i = j + 1; continue; }
                // unknown entity: emit literally
            }
            // stray '&' or unknown entity: copy the '&' verbatim
            if (!sb_putc(s, '&')) return (unsigned long)-1;
            i++;
        } else {
            if (!sb_putc(s, c)) return (unsigned long)-1;
            i++;
        }
    }
    if (!sb_putc(s, '\0')) return (unsigned long)-1;
    return off;
}

// copy raw bytes (no entity decode) NUL-terminated; for names
static unsigned long raw_into(sbuf *s, const char *src, unsigned long n) {
    unsigned long off = sb_add(s, src, n);
    if (off == (unsigned long)-1) return off;
    if (!sb_putc(s, '\0')) return (unsigned long)-1;
    return off;
}

static const char *sc_str(oxml *x, unsigned long off) {
    return (off == (unsigned long)-1) ? "" : (x->sc.p + off);
}

// resolve a prefix (may be "") to a namespace URI, honouring scope (top-down)
static const char *resolve_prefix(oxml *x, const char *pfx) {
    if (pfx[0] == 'x' && pfx[1]=='m' && pfx[2]=='l' && pfx[3]=='\0') return XML_NS;
    if (!strcmp(pfx, "xmlns")) return XMLNS_NS;
    for (int i = x->nns - 1; i >= 0; i--)
        if (!strcmp(x->ns[i].prefix, pfx)) return x->ns[i].uri;
    return ""; // default ns undeclared, or unbound prefix
}

static void ns_pop_level(oxml *x, int lvl) {
    while (x->nns > 0 && x->ns[x->nns-1].level >= lvl) {
        x->nns--;
        free(x->ns[x->nns].prefix);
        free(x->ns[x->nns].uri);
    }
}

// push a binding (prefix, uri) owning fresh copies
static int ns_push(oxml *x, const char *prefix, const char *uri, int lvl) {
    if (x->nns == x->cns) {
        int nc = x->cns ? x->cns*2 : 8;
        nsbind *nb = (nsbind *)realloc(x->ns, (unsigned long)nc*sizeof(nsbind));
        if (!nb) return 0;
        x->ns = nb; x->cns = nc;
    }
    unsigned long pl = strlen(prefix)+1, ul = strlen(uri)+1;
    char *pc = (char *)malloc(pl), *uc = (char *)malloc(ul);
    if (!pc || !uc) { free(pc); free(uc); return 0; }
    memcpy(pc, prefix, pl); memcpy(uc, uri, ul);
    x->ns[x->nns].prefix = pc; x->ns[x->nns].uri = uc; x->ns[x->nns].level = lvl;
    x->nns++;
    return 1;
}

oxml *oxml_open(const char *xml, unsigned long len) {
    if (!xml) return 0;
    oxml *x = (oxml *)calloc(1, sizeof(oxml));
    if (!x) return 0;
    x->buf = xml; x->len = len; x->pos = 0;
    x->cur = OXML_EOF; x->level = 0;
    return x;
}

void oxml_close(oxml *x) {
    if (!x) return;
    ns_pop_level(x, 0);            // free all bindings
    free(x->ns);
    free(x->attrs);
    sb_free(&x->sc);
    sb_free(&x->pendbuf);
    free(x);
}

// parse one start/end tag name from source at [p, end); split prefix:local.
// stores prefix and local as separate NUL-terminated runs in scratch, returns
// via out-offsets. namelen is total qname length.
static int split_name(oxml *x, const char *name, unsigned long namelen,
                      unsigned long *pfx_off, unsigned long *loc_off) {
    unsigned long colon = namelen;
    for (unsigned long i = 0; i < namelen; i++) if (name[i]==':') { colon = i; break; }
    if (colon < namelen) {
        *pfx_off = raw_into(&x->sc, name, colon);
        *loc_off = raw_into(&x->sc, name+colon+1, namelen-colon-1);
    } else {
        *pfx_off = raw_into(&x->sc, "", 0);
        *loc_off = raw_into(&x->sc, name, namelen);
    }
    return (*pfx_off!=(unsigned long)-1 && *loc_off!=(unsigned long)-1);
}

static oxml_evt fail(oxml *x) { x->cur = OXML_ERR; return OXML_ERR; }

oxml_evt oxml_next(oxml *x) {
    if (!x) return OXML_ERR;
    if (x->cur == OXML_ERR) return OXML_ERR;

    // deferred END from a self-closing tag
    if (x->pend) {
        x->pend = 0;
        sb_reset(&x->sc);
        x->nattr = 0;
        unsigned long lo = raw_into(&x->sc, x->pendbuf.p + x->pend_local_off,
                                    strlen(x->pendbuf.p + x->pend_local_off));
        unsigned long no = raw_into(&x->sc, x->pendbuf.p + x->pend_ns_off,
                                    strlen(x->pendbuf.p + x->pend_ns_off));
        if (lo==(unsigned long)-1 || no==(unsigned long)-1) return fail(x);
        x->local_off = lo; x->ns_off = no;
        x->depth = x->pend_level;
        ns_pop_level(x, x->pend_level);
        x->level = x->pend_level - 1;
        x->cur = OXML_END;
        return OXML_END;
    }

    sb_reset(&x->sc);
    x->nattr = 0;
    const char *b = x->buf;
    unsigned long n = x->len;

    for (;;) {
        if (x->pos >= n) { x->cur = OXML_EOF; return OXML_EOF; }

        if (b[x->pos] != '<') {
            // text run up to next '<'
            unsigned long start = x->pos;
            while (x->pos < n && b[x->pos] != '<') x->pos++;
            unsigned long toff = unescape_into(&x->sc, b+start, x->pos-start);
            if (toff == (unsigned long)-1) return fail(x);
            x->text_off = toff;
            x->cur = OXML_TEXT;
            return OXML_TEXT;
        }

        // markup
        if (x->pos+1 >= n) return fail(x);
        char c1 = b[x->pos+1];

        if (c1 == '?') { // PI or XML decl: skip to "?>"
            unsigned long p = x->pos+2;
            while (p+1 < n && !(b[p]=='?'&&b[p+1]=='>')) p++;
            if (p+1 >= n) return fail(x);
            x->pos = p+2;
            continue;
        }
        if (c1 == '!') {
            if (x->pos+3 < n && b[x->pos+2]=='-' && b[x->pos+3]=='-') {
                // comment: skip to "-->"
                unsigned long p = x->pos+4;
                while (p+2 < n && !(b[p]=='-'&&b[p+1]=='-'&&b[p+2]=='>')) p++;
                if (p+2 >= n) return fail(x);
                x->pos = p+3;
                continue;
            }
            if (x->pos+8 < n && !memcmp(b+x->pos+2, "[CDATA[", 7)) {
                // CDATA: literal text to "]]>"
                unsigned long start = x->pos+9;
                unsigned long p = start;
                while (p+2 < n && !(b[p]==']'&&b[p+1]==']'&&b[p+2]=='>')) p++;
                if (p+2 >= n) return fail(x);
                unsigned long toff = raw_into(&x->sc, b+start, p-start);
                if (toff == (unsigned long)-1) return fail(x);
                x->text_off = toff;
                x->pos = p+3;
                x->cur = OXML_TEXT;
                return OXML_TEXT;
            }
            // DOCTYPE or other declaration: skip to matching '>' (naive, ignores
            // '>' inside internal subset; OOXML/ODF parts carry none)
            unsigned long p = x->pos+2;
            while (p < n && b[p] != '>') p++;
            if (p >= n) return fail(x);
            x->pos = p+1;
            continue;
        }

        if (c1 == '/') {
            // end tag
            unsigned long p = x->pos+2;
            unsigned long nstart = p;
            while (p < n && !is_name_end(b[p])) p++;
            unsigned long namelen = p - nstart;
            if (namelen == 0) return fail(x);
            while (p < n && b[p] != '>') p++;
            if (p >= n) return fail(x);
            x->pos = p+1;

            unsigned long pfx_off, loc_off;
            if (!split_name(x, b+nstart, namelen, &pfx_off, &loc_off)) return fail(x);
            const char *uri = resolve_prefix(x, sc_str(x, pfx_off));
            unsigned long no = raw_into(&x->sc, uri, strlen(uri));
            if (no == (unsigned long)-1) return fail(x);
            x->local_off = loc_off; x->ns_off = no;
            x->depth = x->level;          // report the element's own depth
            ns_pop_level(x, x->level);    // drop bindings declared at this level
            x->level--;
            x->cur = OXML_END;
            return OXML_END;
        }

        // start tag
        {
            unsigned long p = x->pos+1;
            unsigned long nstart = p;
            while (p < n && !is_name_end(b[p])) p++;
            unsigned long namelen = p - nstart;
            if (namelen == 0) return fail(x);

            // this element's nesting level
            int lvl = x->level + 1;

            // collect raw attributes into temp offsets
            // rawattr entries: {pfx_off, loc_off, val_off}
            struct { unsigned long pfx, loc, val; } raw[64];
            int nraw = 0;
            int selfclose = 0;

            for (;;) {
                while (p < n && is_ws(b[p])) p++;
                if (p >= n) return fail(x);
                if (b[p] == '/') {
                    if (p+1 < n && b[p+1] == '>') { selfclose = 1; p += 2; break; }
                    return fail(x);
                }
                if (b[p] == '>') { p++; break; }
                // attribute name
                unsigned long anstart = p;
                while (p < n && !is_name_end(b[p])) p++;
                unsigned long anlen = p - anstart;
                if (anlen == 0) return fail(x);
                while (p < n && is_ws(b[p])) p++;
                if (p >= n || b[p] != '=') return fail(x);
                p++; // skip '='
                while (p < n && is_ws(b[p])) p++;
                if (p >= n || (b[p] != '"' && b[p] != '\'')) return fail(x);
                char q = b[p++];
                unsigned long avstart = p;
                while (p < n && b[p] != q) p++;
                if (p >= n) return fail(x);
                unsigned long avlen = p - avstart;
                p++; // skip closing quote

                if (nraw >= 64) return fail(x); // strict cap; parts stay small
                unsigned long apfx, aloc;
                if (!split_name(x, b+anstart, anlen, &apfx, &aloc)) return fail(x);
                unsigned long aval = unescape_into(&x->sc, b+avstart, avlen);
                if (aval == (unsigned long)-1) return fail(x);
                raw[nraw].pfx = apfx; raw[nraw].loc = aloc; raw[nraw].val = aval;
                nraw++;
            }
            x->pos = p;

            // pass B: push namespace declarations at this level
            for (int i = 0; i < nraw; i++) {
                const char *rp = sc_str(x, raw[i].pfx);
                const char *rl = sc_str(x, raw[i].loc);
                const char *rv = sc_str(x, raw[i].val);
                if (!strcmp(rp, "xmlns")) {            // xmlns:prefix="uri"
                    if (!ns_push(x, rl, rv, lvl)) return fail(x);
                } else if (rp[0]=='\0' && !strcmp(rl, "xmlns")) { // default xmlns="uri"
                    if (!ns_push(x, "", rv, lvl)) return fail(x);
                }
            }

            // commit this element's level now that bindings are in scope
            x->level = lvl;

            // pass C: element name/ns + regular (non-xmlns) attributes
            unsigned long pfx_off, loc_off;
            if (!split_name(x, b+nstart, namelen, &pfx_off, &loc_off)) return fail(x);
            const char *euri = resolve_prefix(x, sc_str(x, pfx_off));
            unsigned long eno = raw_into(&x->sc, euri, strlen(euri));
            if (eno == (unsigned long)-1) return fail(x);
            x->local_off = loc_off; x->ns_off = eno;

            for (int i = 0; i < nraw; i++) {
                const char *rp = sc_str(x, raw[i].pfx);
                const char *rl = sc_str(x, raw[i].loc);
                if (!strcmp(rp, "xmlns")) continue;
                if (rp[0]=='\0' && !strcmp(rl, "xmlns")) continue;
                // resolve attr ns: unprefixed attrs are in NO namespace ("")
                const char *auri = (rp[0]=='\0') ? "" : resolve_prefix(x, rp);
                unsigned long ano = raw_into(&x->sc, auri, strlen(auri));
                if (ano == (unsigned long)-1) return fail(x);
                if (x->nattr == x->cattr) {
                    int nc = x->cattr ? x->cattr*2 : 8;
                    attr_t *na = (attr_t *)realloc(x->attrs, (unsigned long)nc*sizeof(attr_t));
                    if (!na) return fail(x);
                    x->attrs = na; x->cattr = nc;
                }
                x->attrs[x->nattr].local_off = raw[i].loc;
                x->attrs[x->nattr].ns_off = ano;
                x->attrs[x->nattr].val_off = raw[i].val;
                x->nattr++;
            }

            x->depth = lvl;

            if (selfclose) {
                // stash local + ns for the deferred END (scratch is reused)
                sb_reset(&x->pendbuf);
                const char *lname = sc_str(x, x->local_off);
                const char *lns   = sc_str(x, x->ns_off);
                x->pend_local_off = raw_into(&x->pendbuf, lname, strlen(lname));
                x->pend_ns_off    = raw_into(&x->pendbuf, lns, strlen(lns));
                if (x->pend_local_off==(unsigned long)-1 || x->pend_ns_off==(unsigned long)-1)
                    return fail(x);
                x->pend_level = lvl;
                x->pend = 1;
            }

            x->cur = OXML_START;
            return OXML_START;
        }
    }
}

const char *oxml_local(oxml *x) {
    if (!x || (x->cur != OXML_START && x->cur != OXML_END)) return "";
    return sc_str(x, x->local_off);
}
const char *oxml_ns(oxml *x) {
    if (!x || (x->cur != OXML_START && x->cur != OXML_END)) return "";
    return sc_str(x, x->ns_off);
}
const char *oxml_text(oxml *x) {
    if (!x || x->cur != OXML_TEXT) return "";
    return sc_str(x, x->text_off);
}
const char *oxml_attr(oxml *x, const char *local) {
    if (!x || x->cur != OXML_START || !local) return 0;
    for (int i = 0; i < x->nattr; i++)
        if (!strcmp(sc_str(x, x->attrs[i].local_off), local))
            return sc_str(x, x->attrs[i].val_off);
    return 0;
}
const char *oxml_attr_ns(oxml *x, const char *ns, const char *local) {
    if (!x || x->cur != OXML_START || !local) return 0;
    if (!ns) ns = "";
    for (int i = 0; i < x->nattr; i++)
        if (!strcmp(sc_str(x, x->attrs[i].local_off), local) &&
            !strcmp(sc_str(x, x->attrs[i].ns_off), ns))
            return sc_str(x, x->attrs[i].val_off);
    return 0;
}
int oxml_depth(oxml *x) { return x ? x->depth : 0; }

// ---------------------------------------------------------------------------
// writer
// ---------------------------------------------------------------------------
struct oxml_w {
    sbuf out;
    int in_start;   // an open start tag awaiting its '>' (so attrs can append)
    int failed;
};

oxml_w *oxml_w_new(void) {
    oxml_w *w = (oxml_w *)calloc(1, sizeof(oxml_w));
    return w;
}
static void w_raw(oxml_w *w, const char *s) {
    if (w->failed) return;
    if (sb_add(&w->out, s, strlen(s)) == (unsigned long)-1) w->failed = 1;
}
static void w_rawn(oxml_w *w, const char *s, unsigned long n) {
    if (w->failed) return;
    if (sb_add(&w->out, s, n) == (unsigned long)-1) w->failed = 1;
}
// escape for text/attr content: & < > and (for attrs) " and '
static void w_escaped(oxml_w *w, const char *s) {
    if (w->failed || !s) return;
    for (; *s; s++) {
        switch (*s) {
            case '&': w_raw(w, "&amp;"); break;
            case '<': w_raw(w, "&lt;"); break;
            case '>': w_raw(w, "&gt;"); break;
            case '"': w_raw(w, "&quot;"); break;
            case '\'': w_raw(w, "&apos;"); break;
            default: w_rawn(w, s, 1); break;
        }
    }
}
static void w_close_start(oxml_w *w) {
    if (w->in_start) { w_rawn(w, ">", 1); w->in_start = 0; }
}
void oxml_w_decl(oxml_w *w) {
    if (!w) return;
    w_close_start(w);
    w_raw(w, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
}
void oxml_w_start(oxml_w *w, const char *qname) {
    if (!w || !qname) return;
    w_close_start(w);
    w_rawn(w, "<", 1);
    w_raw(w, qname);
    w->in_start = 1;
}
void oxml_w_attr(oxml_w *w, const char *k, const char *v) {
    if (!w || !k || !w->in_start) return; // attrs only inside an open start tag
    w_rawn(w, " ", 1);
    w_raw(w, k);
    w_raw(w, "=\"");
    w_escaped(w, v);
    w_rawn(w, "\"", 1);
}
void oxml_w_text(oxml_w *w, const char *t) {
    if (!w) return;
    w_close_start(w);
    w_escaped(w, t);
}
void oxml_w_end(oxml_w *w, const char *qname) {
    if (!w || !qname) return;
    w_close_start(w);
    w_raw(w, "</");
    w_raw(w, qname);
    w_rawn(w, ">", 1);
}
const char *oxml_w_cstr(oxml_w *w, unsigned long *len) {
    if (!w || w->failed) { if (len) *len = 0; return ""; }
    w_close_start(w);
    // ensure NUL terminator without counting it in len
    if (sb_reserve(&w->out, 1)) w->out.p[w->out.len] = '\0';
    if (len) *len = w->out.len;
    return w->out.p ? w->out.p : "";
}
void oxml_w_free(oxml_w *w) {
    if (!w) return;
    sb_free(&w->out);
    free(w);
}
