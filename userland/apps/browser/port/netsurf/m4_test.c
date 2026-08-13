/* M4 layout test: parse+style+layout an HTML page, dump positioned runs. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dom/dom.h>
#include "dom_hubbub_bind.h"
#include "css_select_bind.h"
#include "layout.h"
#include "../active-code/source/userland/libc/syscall.h"

static const char *HTML =
	"<!DOCTYPE html><html><head><title>T</title>"
	"<style>p{color:#202020;} a{color:#0040c0;}</style></head><body>"
	"<h1>MayteraOS Browser</h1>"
	"<p>This is the first paragraph of body text that should wrap across "
	"several lines when the viewport is narrow enough to force wrapping.</p>"
	"<h2>Section</h2>"
	"<p>Second paragraph with a <a href=\"http://example.com\">hyperlink</a> "
	"and some <b>bold</b> and <i>italic</i> words.</p>"
	"<ul><li>first item</li><li>second item</li></ul>"
	"</body></html>";

static int meas(const char *s, int size) { return ttf_measure(s, size); }

static layout_item items[LAYOUT_MAX_ITEMS];

int main(void)
{
	char out[2048]; int n = 0;
	mdb_parser *p = mdb_create();
	mcs_ctx *css = mcs_create();
	if (!p || !css) { n += snprintf(out+n,sizeof(out)-n,"init FAIL\n"); goto done; }

	mdb_parse_chunk(p, (const unsigned char*)HTML, strlen(HTML));
	mdb_parse_complete(p);
	mcs_add_author_css(css, "p{color:#202020;} a{color:#0040c0;}",
			(unsigned long)strlen("p{color:#202020;} a{color:#0040c0;}"));

	layout_result res; res.items = items;
	int r = layout_document(css, mdb_document(p), 760, meas, &res);

	n += snprintf(out+n,sizeof(out)-n,
		"layout rc=%d items=%d height=%d\n", r, res.n_items, res.content_height);
	for (int i = 0; i < res.n_items && i < 14; i++) {
		layout_item *it = &res.items[i];
		n += snprintf(out+n,sizeof(out)-n,
			"[%2d] x=%3d y=%3d sz=%2d b=%d u=%d col=%06x '%s'\n",
			i, it->x, it->y, it->size, it->bold, it->underline,
			(unsigned)it->color, it->text);
	}
	n += snprintf(out+n,sizeof(out)-n,"M4 OK\n");
done:
	printf("%s", out);
	{ FILE *f=fopen("/M4RESULT.TXT","w"); if(f){fwrite(out,1,n,f);fclose(f);} }
	return 0;
}
