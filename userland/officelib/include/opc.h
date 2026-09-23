// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// See docs/OFFICE_SUITE_ARCHITECTURE.md for the module map and contracts.
// NOTE: officelib headers are SELF-SUFFICIENT (raw C types only). Do NOT add
//       #include "types.h" or any libc header here: libc's bool/off_t/pid_t
//       leak into apps carrying legacy compat typedefs and break the userland.
#ifndef OFFICE_OPC_H
#define OFFICE_OPC_H
// Open Packaging Convention layer: a .docx/.xlsx/.pptx OR an ODF .odt/.ods/.odp
// is a ZIP of XML "parts". This wraps userland/libarchive/arc.h in-memory zip.
// Owner: agent 1 (opc/). Backed by arc_zip_extract/arc_zip_create.

typedef struct opc_part {
    char           name[256];   // part path inside the package, no leading '/'
    unsigned char *data;        // borrowed: owned by the opc_pkg
    unsigned long  size;
} opc_part;

typedef struct opc_pkg opc_pkg; // opaque

// ---- read ----
opc_pkg  *opc_open_mem(const unsigned char *zip, unsigned long len);
opc_pkg  *opc_open_file(const char *path);
int       opc_count(opc_pkg *p);
opc_part *opc_at(opc_pkg *p, int i);                 // borrowed
opc_part *opc_get(opc_pkg *p, const char *partname); // borrowed, NULL if absent
// Resolve a relationship Id (rId) declared in a .rels part to its Target part.
// rels_part e.g. "word/_rels/document.xml.rels". Writes into out (<=outcap),
// returns out on success or NULL.
const char *opc_rel_target(opc_pkg *p, const char *rels_part,
                           const char *rid, char *out, unsigned long outcap);

// ---- write ----
opc_pkg  *opc_new(void);
int       opc_put(opc_pkg *p, const char *partname,
                  const unsigned char *data, unsigned long size); // copies
unsigned char *opc_save_mem(opc_pkg *p, unsigned long *out_len);  // deflate zip
// Strict-ODF save: identical to opc_save_mem, except the `mimetype` part (if
// present) is written as the FIRST zip entry and STORED (uncompressed), as the
// OpenDocument spec requires so a reader can sniff the media type at a fixed
// offset. Use this for .odt/.ods/.odp; opc_save_mem stays deflate-all for OOXML.
unsigned char *opc_save_mem_odf(opc_pkg *p, unsigned long *out_len);
int       opc_save_file(opc_pkg *p, const char *path);

void      opc_free(opc_pkg *p);
#endif
