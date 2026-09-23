// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// See docs/OFFICE_SUITE_ARCHITECTURE.md for the module map and contracts.
// NOTE: officelib headers are SELF-SUFFICIENT (raw C types only). Do NOT add
//       #include "types.h" or any libc header here: libc's bool/off_t/pid_t
//       leak into apps carrying legacy compat typedefs and break the userland.
#ifndef OFFICE_FORMATS_H
#define OFFICE_FORMATS_H
// Uniform import/export entry points. Each returns 0 on success, <0 on error.
// load: parse in-memory file bytes into a model (caller owns *out, model_free).
// save: serialize a model into a freshly malloc'd buffer (*out/*out_len).
#include "docmodel.h"
#include "sheetmodel.h"
#include "slidemodel.h"

// OOXML (agents 4,6,7)
int docx_load(const unsigned char *b, unsigned long n, document **out);
int docx_save(const document *d, unsigned char **out, unsigned long *out_len);
int xlsx_load(const unsigned char *b, unsigned long n, workbook **out);
int xlsx_save(const workbook *w, unsigned char **out, unsigned long *out_len);
int pptx_load(const unsigned char *b, unsigned long n, presentation **out);
int pptx_save(const presentation *p, unsigned char **out, unsigned long *out_len);

// ODF (agents 5,6,7)
int odt_load(const unsigned char *b, unsigned long n, document **out);
int odt_save(const document *d, unsigned char **out, unsigned long *out_len);
int ods_load(const unsigned char *b, unsigned long n, workbook **out);
int ods_save(const workbook *w, unsigned char **out, unsigned long *out_len);
int odp_load(const unsigned char *b, unsigned long n, presentation **out);
int odp_save(const presentation *p, unsigned char **out, unsigned long *out_len);

// Legacy OLE2, read-only (agent 8)
int doc_ole_load(const unsigned char *b, unsigned long n, document **out);     // .doc
int xls_ole_load(const unsigned char *b, unsigned long n, workbook **out);     // .xls
int ppt_ole_load(const unsigned char *b, unsigned long n, presentation **out); // .ppt

// Top-level dispatcher by extension + magic. kind: 1=doc 2=sheet 3=pres.
// Fills exactly one of the out params matching kind. *readonly set for legacy.
int office_detect(const char *path, const unsigned char *b, unsigned long n); // returns kind or <0
int office_open_document(const char *path, document **out, int *readonly);
int office_open_workbook(const char *path, workbook **out, int *readonly);
int office_open_presentation(const char *path, presentation **out, int *readonly);
#endif
