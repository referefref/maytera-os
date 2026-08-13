/*
 * dom_hubbub_bind.h - MayteraOS hubbub->libdom tree-construction binding.
 * Licensed under the MIT License.
 *
 * NetSurf's own libdom_hubbub binding is GPL and is NOT shipped with the
 * core libraries, so this is an independent MIT reimplementation that wires
 * hubbub's tree_handler callbacks onto the libdom core/HTML API.
 */
#ifndef MAYTERA_DOM_HUBBUB_BIND_H
#define MAYTERA_DOM_HUBBUB_BIND_H

#include <dom/dom.h>

/* Opaque parse context. */
typedef struct mdb_parser mdb_parser;

/*
 * Create an HTML parse context (document + hubbub parser + tree binding).
 * Returns NULL on failure.
 */
mdb_parser *mdb_create(void);

/* Feed a chunk of (UTF-8) HTML source to the parser. 0 on success. */
int mdb_parse_chunk(mdb_parser *p, const unsigned char *data, unsigned long len);

/* Signal end of input and run final tree construction. 0 on success. */
int mdb_parse_complete(mdb_parser *p);

/* Get the libdom document built so far (borrowed; valid until mdb_destroy). */
dom_document *mdb_document(mdb_parser *p);

/* Tear down parser + document. */
void mdb_destroy(mdb_parser *p);

#endif
