/*
 * layout.h - MayteraOS minimal block+inline+text layout (MIT, our own code).
 *
 * Walks a styled libdom tree and produces positioned text runs for rendering
 * into the browser viewport. Handles block stacking, inline text wrapping,
 * heading sizes/weight, paragraph margins, links in an accent colour. Floats,
 * tables, flex are intentionally ignored. No libm: integer math only.
 */
#ifndef MAYTERA_LAYOUT_H
#define MAYTERA_LAYOUT_H

#include <stdint.h>
#include <dom/dom.h>
#include "css_select_bind.h"

#define LAYOUT_MAX_ITEMS 4096
#define LAYOUT_RUN_MAX   256
#define LAYOUT_HREF_MAX  512

typedef struct layout_item {
	int kind;          /* 0 = text run, 1 = box (bg/border) */
	int w;             /* box width  (kind 1) */
	int h;             /* box height (kind 1) */
	uint32_t bg;       /* box background 0x00RRGGBB (kind 1) */
	int has_bg;
	uint32_t border_col;
	int border_w;
	int x;            /* relative to content origin */
	int y;            /* relative to content top (pre-scroll) */
	int size;         /* font size px */
	uint32_t color;   /* 0x00RRGGBB */
	int bold;
	int italic;
	int underline;
	char text[LAYOUT_RUN_MAX];
	char href[LAYOUT_HREF_MAX];  /* link target, or form action for a control */
	int form_kind;     /* 0 none, 1 text field, 2 submit/button */
	char field_name[64];  /* <input name=> */
} layout_item;

typedef struct layout_result {
	layout_item *items;
	int n_items;
	int content_height; /* total laid-out height in px */
} layout_result;

/*
 * Lay out the document tree into `out`. `content_width` is the wrap width in px.
 * `measure` measures a string at a size and returns pixel width.
 * Returns 0 on success.
 */
int layout_document(mcs_ctx *css, dom_document *doc, int content_width,
		int (*measure)(const char *s, int size),
		layout_result *out);

/* #245 perf: count of measure() (SYS_MEASURE_TTF) calls made by layout. */
extern unsigned long g_layout_measure_calls;
extern unsigned long g_layout_measure_words;
/* #245 profiling buckets, in TSC cycles, reset at each layout_document(). */
extern unsigned long long g_lp_style, g_lp_text, g_lp_flow, g_lp_meas;
extern unsigned long long g_lp_attr, g_lp_walk;
extern unsigned long g_lp_nelem, g_lp_ntext;

#endif
