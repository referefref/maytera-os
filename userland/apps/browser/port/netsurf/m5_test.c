/* M5 layout test (#245 engscroll/nested-clip): dump box geometry to PROVE
 * nested overflow clipping composes to the intersection, and that an
 * overflow:scroll container shifts + clips content and emits a scrollbar. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dom/dom.h>
#include "dom_hubbub_bind.h"
#include "css_select_bind.h"
#include "layout.h"
#include "syscall.h"

/* fontmap platform hooks the browser app normally supplies (main.c). */
int fm_plat_font_count(void) { return font_count(); }
int fm_plat_font_name(int idx, char *buf, int cap) { return font_name(idx, buf, cap); }
int fm_plat_font_style(int idx, char *buf, int cap) { return font_style(idx, buf, cap); }

static int meas(const char *s, int size, int face, int style) {
	return ttf_measure_ex(s, face, size, style);
}

static layout_item items[LAYOUT_MAX_ITEMS];

/* Nested clip: inner box starts at margin-left 120 and is 300 wide (extends
 * past the 200-wide outer content box). Its red child is 260 wide. Correct
 * nesting clips red to the OUTER right edge, not the inner one. */
static const char *NEST_HTML =
	"<!DOCTYPE html><html><body><div id=o><div id=i><div id=r></div></div></div></body></html>";
static const char *NEST_CSS =
	"body{margin:0;padding:0}"
	"#o{overflow:hidden;width:200px;height:200px;background:#eeeeee}"
	"#i{overflow:hidden;width:300px;height:100px;margin-left:120px;background:#cccccc}"
	"#r{width:260px;height:60px;background:#ff0000}";

/* Scroll: a 60px-tall overflow:scroll box holding content much taller. */
static const char *SCR_HTML =
	"<!DOCTYPE html><html><body><div id=s>"
	"<p>AAAA</p><p>BBBB</p><p>CCCC</p><p>DDDD</p><p>EEEE</p>"
	"<p>FFFF</p><p>GGGG</p><p>HHHH</p><p>IIII</p><p>JJJJ</p></div></body></html>";
static const char *SCR_CSS =
	"body{margin:0;padding:0}"
	"#s{overflow:scroll;width:300px;height:60px;background:#f0f0f0}";

/* Horizontal (brhscroll #245): a 300px-wide overflow:scroll box whose child is
 * 900px wide, so content overflows the content box horizontally. A green marker
 * sits 350px in: off-screen at offset 0, revealed after scrolling right. */
static const char *HSCR_HTML =
	"<!DOCTYPE html><html><body><div id=h><div id=w>"
	"<div id=m></div></div></div></body></html>";
static const char *HSCR_CSS =
	"body{margin:0;padding:0}"
	"#h{overflow:scroll;width:300px;height:80px;background:#f0f0f0}"
	"#w{width:900px;height:40px;background:#0000ff}"
	"#m{width:24px;height:24px;margin-left:350px;background:#00ff00}";

static int dump_boxes(char *out, int n, int cap, layout_result *res) {
	for (int i = 0; i < res->n_items; i++) {
		layout_item *it = &res->items[i];
		if (it->kind != 1) continue;
		n += snprintf(out+n, cap-n,
			"  box[%2d] x=%4d y=%4d w=%4d h=%4d bg=%06x\n",
			i, it->x, it->y, it->w, it->h, (unsigned)it->bg);
	}
	return n;
}

int main(void) {
	static char out[8192]; int n = 0;

	/* ---- Test 1: nested clip intersection ---- */
	{
		mdb_parser *p = mdb_create();
		mcs_ctx *css = mcs_create();
		mcs_set_viewport(css, 760, 600);
		mdb_parse_chunk(p, (const unsigned char*)NEST_HTML, strlen(NEST_HTML));
		mdb_parse_complete(p);
		mcs_add_author_css(css, NEST_CSS, (unsigned long)strlen(NEST_CSS));
		layout_result res; memset(&res,0,sizeof res); res.items = items;
		int r = layout_document(css, mdb_document(p), 760, meas, &res);
		n += snprintf(out+n,sizeof(out)-n,"NEST rc=%d items=%d n_scrolls=%d\n", r, res.n_items, res.n_scrolls);
		n = dump_boxes(out, n, sizeof(out), &res);
	}

	/* ---- Test 2: overflow:scroll, offset 0 then 40 ---- */
	for (int pass = 0; pass < 2; pass++) {
		mdb_parser *p = mdb_create();
		mcs_ctx *css = mcs_create();
		mcs_set_viewport(css, 760, 600);
		mdb_parse_chunk(p, (const unsigned char*)SCR_HTML, strlen(SCR_HTML));
		mdb_parse_complete(p);
		mcs_add_author_css(css, SCR_CSS, (unsigned long)strlen(SCR_CSS));
		layout_reset_scroll_requests();
		if (pass == 1) layout_set_scroll_request(0, 40);
		layout_result res; memset(&res,0,sizeof res); res.items = items;
		int r = layout_document(css, mdb_document(p), 760, meas, &res);
		n += snprintf(out+n,sizeof(out)-n,
			"SCROLL pass=%d req=%d rc=%d items=%d n_scrolls=%d\n",
			pass, pass==1?40:0, r, res.n_items, res.n_scrolls);
		if (res.n_scrolls > 0) {
			scroll_box *sb = &res.scrolls[0];
			n += snprintf(out+n,sizeof(out)-n,
				"  scrolls[0] cx=%d cy=%d cw=%d ch=%d extent=%d offset=%d\n",
				sb->content_x, sb->content_y, sb->content_w, sb->content_h,
				sb->extent_h, sb->offset);
		}
		/* first text run y (proves the content shifted up) */
		for (int i = 0; i < res.n_items; i++) {
			if (res.items[i].kind == 0 && res.items[i].text[0]) {
				n += snprintf(out+n,sizeof(out)-n,
					"  first-text y=%d text=%s\n", res.items[i].y, res.items[i].text);
				break;
			}
		}
		n = dump_boxes(out, n, sizeof(out), &res);
	}

	/* ---- Test 3: overflow HORIZONTAL scroll, offset_x 0 then 120 ---- */
	for (int pass = 0; pass < 2; pass++) {
		mdb_parser *p = mdb_create();
		mcs_ctx *css = mcs_create();
		mcs_set_viewport(css, 760, 600);
		mdb_parse_chunk(p, (const unsigned char*)HSCR_HTML, strlen(HSCR_HTML));
		mdb_parse_complete(p);
		mcs_add_author_css(css, HSCR_CSS, (unsigned long)strlen(HSCR_CSS));
		layout_reset_scroll_requests();
		if (pass == 1) layout_set_scroll_request_x(0, 120);
		layout_result res; memset(&res,0,sizeof res); res.items = items;
		int r = layout_document(css, mdb_document(p), 760, meas, &res);
		n += snprintf(out+n,sizeof(out)-n,
			"HSCROLL pass=%d reqx=%d rc=%d items=%d n_scrolls=%d\n",
			pass, pass==1?120:0, r, res.n_items, res.n_scrolls);
		if (res.n_scrolls > 0) {
			scroll_box *sb = &res.scrolls[0];
			n += snprintf(out+n,sizeof(out)-n,
				"  scrolls[0] cx=%d cy=%d cw=%d ch=%d extent=%d off=%d extentw=%d offx=%d\n",
				sb->content_x, sb->content_y, sb->content_w, sb->content_h,
				sb->extent_h, sb->offset, sb->extent_w, sb->offset_x);
		}
		/* green marker (0x0000ff00): OFF-screen at offx 0, VISIBLE after scroll */
		{ int seen = 0;
		  for (int i = 0; i < res.n_items; i++)
			if (res.items[i].kind == 1 && res.items[i].bg == 0x0000ff00u &&
					res.items[i].w > 0) {
				n += snprintf(out+n,sizeof(out)-n,
					"  marker VISIBLE x=%d w=%d\n", res.items[i].x, res.items[i].w);
				seen = 1; break; }
		  if (!seen) n += snprintf(out+n,sizeof(out)-n,"  marker OFF-SCREEN\n");
		}
		n = dump_boxes(out, n, sizeof(out), &res);
	}

	n += snprintf(out+n,sizeof(out)-n,"M5 OK\n");
	printf("%s", out);
	{ FILE *f=fopen("/M5RESULT.TXT","w"); if(f){fwrite(out,1,n,f);fclose(f);} }
	return 0;
}
