/* M3 test: parse HTML, style the tree, read computed display/font-size/color/
 * margin for selected elements. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dom/dom.h>
#include <libcss/libcss.h>
#include <libcss/computed.h>
#include <libcss/fpmath.h>
#include <libcss/properties.h>
#include "dom_hubbub_bind.h"
#include "css_select_bind.h"
extern int g_mcs_last_err, g_mcs_stage;

static const char *HTML =
	"<!DOCTYPE html><html><head>"
	"<style>p { color: #ff0000; margin: 5px; } .big { font-size: 30px; }</style>"
	"</head><body>"
	"<h1>Title</h1>"
	"<p>Para</p>"
	"<p class=\"big\">Big para</p>"
	"<a href=\"http://x\">link</a>"
	"</body></html>";

static char OUT[2048];
static int OL;
#define EMIT(...) OL += snprintf(OUT + OL, sizeof(OUT) - OL, __VA_ARGS__)

static const char *display_name(uint8_t d)
{
	switch (d) {
	case CSS_DISPLAY_INLINE: return "inline";
	case CSS_DISPLAY_BLOCK: return "block";
	case CSS_DISPLAY_LIST_ITEM: return "list-item";
	case CSS_DISPLAY_NONE: return "none";
	case CSS_DISPLAY_INLINE_BLOCK: return "inline-block";
	case CSS_DISPLAY_TABLE: return "table";
	default: return "other";
	}
}

static void report(mcs_ctx *css, dom_element *el, const css_computed_style *parent,
		const char *label)
{
	css_computed_style *s = mcs_compute_style(css, el, parent);
	if (!s) { EMIT("%s: <no style> err=%d stage=%d\n", label, g_mcs_last_err, g_mcs_stage); return; }

	uint8_t disp = css_computed_display(s, false);

	css_fixed fs = 0; css_unit fu = CSS_UNIT_PX;
	css_computed_font_size(s, &fs, &fu);

	css_color col = 0;
	css_computed_color(s, &col);

	css_fixed mt = 0; css_unit mu = CSS_UNIT_PX;
	css_computed_margin_top(s, &mt, &mu);

	EMIT("%s: display=%s font-size=%dpx color=#%06x margin-top=%dpx\n",
		label, display_name(disp),
		(int) FIXTOINT(fs),
		(unsigned)(col & 0xffffff),
		(int) FIXTOINT(mt));

	css_computed_style_destroy(s);
}

/* find first element by lowercase tag name (DFS) */
static dom_element *find_tag(dom_node *node, const char *tag, const char *cls)
{
	dom_node *child = NULL;
	dom_node_type t;
	if (!node) return NULL;
	if (dom_node_get_node_type(node, &t) == DOM_NO_ERR && t == DOM_ELEMENT_NODE) {
		dom_string *nm = NULL;
		bool tag_ok = false, cls_ok = (cls == NULL);
		if (dom_node_get_node_name(node, &nm) == DOM_NO_ERR && nm) {
			const char *d = dom_string_data(nm);
			size_t l = dom_string_byte_length(nm);
			if (l == strlen(tag)) {
				size_t i; tag_ok = true;
				for (i = 0; i < l; i++) {
					char c = d[i];
					if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
					if (c != tag[i]) { tag_ok = false; break; }
				}
			}
			dom_string_unref(nm);
		}
		if (tag_ok && cls) {
			dom_string *an = NULL, *cv = NULL;
			if (dom_string_create((const uint8_t*)"class",5,&an)==DOM_NO_ERR && an){
				dom_element_get_attribute((dom_element*)node, an, &cv);
				dom_string_unref(an);
			}
			if (cv) {
				if (strstr(dom_string_data(cv), cls)) cls_ok = true;
				dom_string_unref(cv);
			}
		}
		if (tag_ok && cls_ok) { dom_node_ref(node); return (dom_element*)node; }
	}
	if (dom_node_get_first_child(node, &child) == DOM_NO_ERR) {
		while (child) {
			dom_node *next = NULL;
			dom_element *r = find_tag(child, tag, cls);
			if (r) { dom_node_unref(child); return r; }
			dom_node_get_next_sibling(child, &next);
			dom_node_unref(child);
			child = next;
		}
	}
	return NULL;
}

int main(void)
{
	mdb_parser *p = mdb_create();
	if (!p) { EMIT("mdb_create FAIL\n"); goto done; }
	mdb_parse_chunk(p, (const unsigned char*)HTML, strlen(HTML));
	mdb_parse_complete(p);
	dom_document *doc = mdb_document(p);

	mcs_ctx *css = mcs_create();
	if (!css) { EMIT("mcs_create FAIL\n"); goto done; }
	mcs_add_author_css(css,
		"p { color: #ff0000; margin: 5px; } .big { font-size: 30px; }",
		(unsigned long) strlen(
		"p { color: #ff0000; margin: 5px; } .big { font-size: 30px; }"));

	dom_node *root = NULL;
	dom_document_get_document_element(doc, &root);

	dom_element *body = find_tag(root, "body", NULL);
	dom_element *h1 = find_tag(root, "h1", NULL);
	dom_element *para = find_tag(root, "p", NULL);
	dom_element *big = find_tag(root, "p", "big");
	dom_element *a = find_tag(root, "a", NULL);

	/* root html style first (parent chain) */
	css_computed_style *html_style = mcs_compute_style(css, (dom_element*)root, NULL);
	css_computed_style *body_style = body ? mcs_compute_style(css, body, html_style) : NULL;

	EMIT("html_style=%p body=%p body_style=%p err=%d stage=%d\n", (void*)html_style,(void*)body,(void*)body_style,g_mcs_last_err,g_mcs_stage);

	report(css, h1, body_style, "h1");
	report(css, para, body_style, "p");
	report(css, big, body_style, "p.big");
	report(css, a, body_style, "a");
	if (body) report(css, body, html_style, "body");

	if (html_style) css_computed_style_destroy(html_style);
	if (body_style) css_computed_style_destroy(body_style);
	EMIT("M3 OK\n");

done:
	printf("%s", OUT);
	{ FILE *f = fopen("/M3RESULT.TXT", "w"); if (f){ fwrite(OUT,1,OL,f); fclose(f);} }
	return 0;
}
