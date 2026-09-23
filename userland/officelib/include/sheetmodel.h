// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// See docs/OFFICE_SUITE_ARCHITECTURE.md for the module map and contracts.
// NOTE: officelib headers are SELF-SUFFICIENT (raw C types only). Do NOT add
//       #include "types.h" or any libc header here: libc's bool/off_t/pid_t
//       leak into apps carrying legacy compat typedefs and break the userland.
#ifndef OFFICE_SHEETMODEL_H
#define OFFICE_SHEETMODEL_H
// Spreadsheet model shared by Calc + the xlsx/ods/.xls importers.
// Owner: agent 3 (model/sheetmodel.c). Consumed by fmt_xlsx_ods, fmt_ole2,
// model/formula.c, apps/sheets.

typedef enum { CELL_EMPTY=0, CELL_NUM, CELL_STR, CELL_FORMULA, CELL_BOOL, CELL_ERR } cell_type;

typedef struct cell {
    cell_type type;
    double    num;              // CELL_NUM / CELL_BOOL / cached formula result
    char     *str;              // CELL_STR / CELL_ERR text (owned)
    char     *formula;          // CELL_FORMULA source without '=' (owned)
    int       style_id;         // index into workbook.styles, -1 = default
} cell;

typedef struct sheet {
    char *name;
    int   nrows, ncols;         // logical extent
    cell *cells;                // dense nrows*ncols, row-major; grows on demand
} sheet;

typedef struct wb_style { char *numfmt_code; int bold, italic; unsigned int color; } wb_style;

typedef struct workbook {
    sheet    *sheets; int nsheet;
    char    **sst;    int nsst;   // shared strings (xlsx)
    wb_style *styles; int nstyle;
} workbook;

workbook *wb_new(void);
void      wb_free(workbook *w);
sheet    *wb_add_sheet(workbook *w, const char *name);
cell     *sheet_cell(sheet *s, int row, int col);   // ensure allocated + return
int       wb_add_style(workbook *w, const wb_style *st);
// A1 <-> (row,col) helpers (row,col are 0-based):
int       a1_to_rc(const char *a1, int *row, int *col);
void      rc_to_a1(int row, int col, char *out, unsigned long cap);
#endif
