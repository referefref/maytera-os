/*
 * css_select_bind.h - MayteraOS libcss<->libdom selection binding (MIT).
 * Independent implementation of the css_select_handler callbacks plus helpers
 * to build a select context from UA + document stylesheets and to compute a
 * css_computed_style for a libdom element.
 */
#ifndef MAYTERA_CSS_SELECT_BIND_H
#define MAYTERA_CSS_SELECT_BIND_H

#include <dom/dom.h>
#include <libcss/libcss.h>

typedef struct mcs_ctx mcs_ctx;

/* Create a selection context seeded with a built-in UA stylesheet. */
mcs_ctx *mcs_create(void);

/* Append author CSS source text (e.g. from a <style> element). 0 on success. */
int mcs_add_author_css(mcs_ctx *c, const char *css, unsigned long len);

/* Compute the style for a libdom element node. Caller must
 * css_computed_style_destroy() the returned style (or via mcs results). The
 * parent_style may be NULL for the root; it is used for inheritance. */
css_computed_style *mcs_compute_style(mcs_ctx *c, dom_element *node,
		const css_computed_style *parent_style);

void mcs_destroy(mcs_ctx *c);

/* Expose the select handler/pw for callers that want to call css_select_style
 * directly. */
css_select_handler *mcs_handler(void);

#endif
