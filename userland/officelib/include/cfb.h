// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// See docs/OFFICE_SUITE_ARCHITECTURE.md for the module map and contracts.
// NOTE: officelib headers are SELF-SUFFICIENT (raw C types only). Do NOT add
//       #include "types.h" or any libc header here: libc's bool/off_t/pid_t
//       leak into apps carrying legacy compat typedefs and break the userland.
#ifndef OFFICE_CFB_H
#define OFFICE_CFB_H
// Compound File Binary (OLE2) reader: the container of legacy .doc/.xls/.ppt.
// Read-only. OURS (no reusable reader existed; the Word6 OLE2 code is the
// Win16 COM runtime, not a file parser). Owner: agent 8 (fmt_ole2/cfb.c).
typedef struct cfb cfb;
cfb                *cfb_open(const unsigned char *bytes, unsigned long len);
// Borrowed pointer to a named stream's bytes (e.g. "WordDocument","Workbook",
// "PowerPoint Document"), NULL if absent. *out_len set on success.
const unsigned char*cfb_stream(cfb *c, const char *name, unsigned long *out_len);
int                 cfb_has(cfb *c, const char *name);
void                cfb_close(cfb *c);
#endif
