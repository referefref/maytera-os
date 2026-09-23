// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// See docs/OFFICE_SUITE_ARCHITECTURE.md for the module map and contracts.
// NOTE: officelib headers are SELF-SUFFICIENT (raw C types only). Do NOT add
//       #include "types.h" or any libc header here: libc's bool/off_t/pid_t
//       leak into apps carrying legacy compat typedefs and break the userland.
#ifndef OFFICE_DOCMODEL_H
#define OFFICE_DOCMODEL_H
// Rich-text document model shared by Writer + the docx/odt/.doc importers.
// Owner: agent 3 (model/docmodel.c). Consumed by fmt_docx, fmt_odt, fmt_ole2,
// layout/, apps/writer.

typedef struct doc_image {
    char           id[64];      // relationship id or name
    unsigned char *bytes;       // encoded PNG/JPEG bytes (owned)
    unsigned long  nbytes;
    int            w, h;        // px hint (0 if unknown)
} doc_image;

typedef struct {
    int          bold, italic, underline, strike;
    int          size;          // points (0 = inherit)
    unsigned int color;         // 0xRRGGBB (0xFFFFFFFF = inherit)
    int          face;          // font registry face id (0 = default UI face)
} doc_runfmt;

typedef struct { char *text; doc_runfmt fmt; } doc_run;

typedef enum { DOC_ALIGN_L=0, DOC_ALIGN_C, DOC_ALIGN_R, DOC_ALIGN_J } doc_align;

typedef struct doc_para {
    doc_run  *runs; int nrun;
    doc_align align;
    int       style_id;         // index into document.styles, -1 = none
    int       list_level;       // -1 = not a list item, else 0..N
    int       list_ordered;     // 1 = numbered, 0 = bullet
} doc_para;

typedef struct doc_table {
    int        rows, cols;
    doc_para **cells;           // rows*cols paragraphs (row-major); may be NULL
} doc_table;

typedef enum { DBLK_PARA=0, DBLK_TABLE, DBLK_IMAGE } doc_blocktype;
typedef struct doc_block {
    doc_blocktype type;
    doc_para  *para;            // DBLK_PARA
    doc_table *table;           // DBLK_TABLE
    doc_image *image;           // DBLK_IMAGE (borrowed from document.images)
} doc_block;

typedef struct { char *name; doc_runfmt runfmt; doc_align align; } doc_style;

typedef struct document {
    doc_block *blocks; int nblk;
    doc_style *styles; int nstyle;
    doc_image *images; int nimg;
    int page_w, page_h, margin; // twips (1/1440 inch); 0 = default Letter
} document;

document *doc_new(void);
void      doc_free(document *d);
// builder helpers used by importers:
doc_block*doc_add_block(document *d, doc_blocktype t);
doc_para *doc_add_para(document *d);                    // appends a DBLK_PARA
doc_run  *doc_para_add_run(doc_para *p, const char *text, doc_runfmt fmt);
int       doc_add_style(document *d, const char *name, doc_runfmt f, doc_align a);
doc_image*doc_add_image(document *d, const char *id, const unsigned char *b, unsigned long n);
#endif
