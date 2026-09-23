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

/*
 * Tell the context how big the viewport is, in CSS px. This is NOT cosmetic:
 * it is what @media conditions are evaluated against and what vw/vh resolve to.
 * It was hardcoded to 1024x768 while the browser laid out at ~760, so a page
 * with a responsive breakpoint was styled for a viewport the user did not have.
 * Call this BEFORE adding any author CSS; the custom-property resolver needs it.
 */
void mcs_set_viewport(mcs_ctx *c, int w, int h);

/*
 * Append author CSS source text (e.g. from a <style> element). 0 on success.
 *
 * The text is run through cssvar_preprocess() first, which resolves CSS custom
 * properties and the clamp()/min()/max()/calc() math functions. libcss supports
 * none of those, and on a modern stylesheet they carry most of the palette and
 * the whole spacing scale. See cssvar.h.
 */
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
