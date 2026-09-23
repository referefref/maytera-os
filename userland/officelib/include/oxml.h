// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// See docs/OFFICE_SUITE_ARCHITECTURE.md for the module map and contracts.
// NOTE: officelib headers are SELF-SUFFICIENT (raw C types only). Do NOT add
//       #include "types.h" or any libc header here: libc's bool/off_t/pid_t
//       leak into apps carrying legacy compat typedefs and break the userland.
#ifndef OFFICE_OXML_H
#define OFFICE_OXML_H
// Small, namespace-aware XML pull-parser + writer. OURS (no third-party).
// OOXML/ODF XML is well-formed (not HTML tag soup), so a strict pull parser is
// enough and far lighter than reusing the browser's hubbub HTML5 tree builder.
// Owner: agent 2 (oxml/).

typedef enum { OXML_START=1, OXML_END, OXML_TEXT, OXML_EOF, OXML_ERR } oxml_evt;
typedef struct oxml oxml;

// The parser does NOT copy the source; keep `xml` alive for the parser's life.
oxml       *oxml_open(const char *xml, unsigned long len);
oxml_evt    oxml_next(oxml *x);              // advance to the next event
const char *oxml_local(oxml *x);             // local name of current START/END
const char *oxml_ns(oxml *x);                // resolved namespace URI or ""
const char *oxml_text(oxml *x);              // text of current OXML_TEXT (unescaped)
const char *oxml_attr(oxml *x, const char *local);              // by local name
const char *oxml_attr_ns(oxml *x, const char *ns, const char *local);
int         oxml_depth(oxml *x);
void        oxml_close(oxml *x);

// ---- writer ----
typedef struct oxml_w oxml_w;
oxml_w     *oxml_w_new(void);
void        oxml_w_decl(oxml_w *w);                       // <?xml ...?>
void        oxml_w_start(oxml_w *w, const char *qname);   // caller gives qualified name
void        oxml_w_attr(oxml_w *w, const char *k, const char *v); // value escaped
void        oxml_w_text(oxml_w *w, const char *t);        // escaped
void        oxml_w_end(oxml_w *w, const char *qname);
const char *oxml_w_cstr(oxml_w *w, unsigned long *len);   // borrowed
void        oxml_w_free(oxml_w *w);
#endif
