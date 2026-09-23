// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// See docs/OFFICE_SUITE_ARCHITECTURE.md for the module map and contracts.
// NOTE: officelib headers are SELF-SUFFICIENT (raw C types only). Do NOT add
//       #include "types.h" or any libc header here: libc's bool/off_t/pid_t
//       leak into apps carrying legacy compat typedefs and break the userland.
#ifndef OFFICE_FORMULA_H
#define OFFICE_FORMULA_H
// Spreadsheet formula evaluation over a workbook/sheet. v1 core subset:
// + - * / ^ %, parens, ranges, and SUM/AVERAGE/MIN/MAX/COUNT/IF/ABS/ROUND.
// Owner: agent 3 (model/formula.c). Uses sheetmodel.h.
#include "sheetmodel.h"
typedef enum { FX_OK=0, FX_ERR_SYNTAX=-1, FX_ERR_REF=-2, FX_ERR_DIV0=-3, FX_ERR_NAME=-4 } fx_status;
// Evaluate `src` (no leading '='), with (row,col) as the cell being computed
// for relative refs, against sheet `s` in workbook `wb`. Result in *out.
fx_status fx_eval(workbook *wb, sheet *s, int row, int col,
                  const char *src, double *out);
// Recompute every CELL_FORMULA in the workbook (single pass; no cycle solver in v1).
void      fx_recalc(workbook *wb);
#endif
