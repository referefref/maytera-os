/* M2 test: feed real HTML through libparserutils+hubbub+binding into libdom,
 * walk the tree, count elements/text nodes, print some tag names. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dom/dom.h>
#include "dom_hubbub_bind.h"

static const char *HTML =
	"<!DOCTYPE html>\n"
	"<html><head><title>Hello MayteraOS</title></head>\n"
	"<body>\n"
	"  <h1>Welcome</h1>\n"
	"  <p>This is a <a href=\"http://example.com\">link</a> in a paragraph.</p>\n"
	"  <p>Second paragraph with <b>bold</b> text.</p>\n"
	"  <ul><li>one</li><li>two</li><li>three</li></ul>\n"
	"</body></html>\n";

static int n_elem, n_text, n_other;
static char names[64][32];
static int n_names;

static void walk(dom_node *node, int depth)
{
	dom_node *child = NULL;
	dom_node_type type;

	if (node == NULL)
		return;
	if (dom_node_get_node_type(node, &type) != DOM_NO_ERR)
		return;

	if (type == DOM_ELEMENT_NODE) {
		n_elem++;
		if (n_names < 64) {
			dom_string *nm = NULL;
			if (dom_node_get_node_name(node, &nm) == DOM_NO_ERR && nm) {
				const char *d = dom_string_data(nm);
				size_t l = dom_string_byte_length(nm);
				if (l > 31) l = 31;
				memcpy(names[n_names], d, l);
				names[n_names][l] = '\0';
				n_names++;
				dom_string_unref(nm);
			}
		}
	} else if (type == DOM_TEXT_NODE) {
		n_text++;
	} else {
		n_other++;
	}

	(void) depth;
	if (dom_node_get_first_child(node, &child) == DOM_NO_ERR) {
		while (child != NULL) {
			dom_node *next = NULL;
			walk(child, depth + 1);
			dom_node_get_next_sibling(child, &next);
			dom_node_unref(child);
			child = next;
		}
	}
}

int main(void)
{
	char out[1024];
	int n = 0;
	mdb_parser *p = mdb_create();
	if (p == NULL) {
		printf("mdb_create FAILED\n");
		FILE *f = fopen("/M2RESULT.TXT", "w");
		if (f) { fputs("mdb_create FAILED\n", f); fclose(f); }
		return 1;
	}

	mdb_parse_chunk(p, (const unsigned char *) HTML, strlen(HTML));
	mdb_parse_complete(p);

	dom_document *doc = mdb_document(p);
	dom_node *root = NULL;
	dom_document_get_document_element(doc, &root);

	walk(root, 0);
	if (root) dom_node_unref(root);

	n += snprintf(out + n, sizeof(out) - n,
		"elements=%d text=%d other=%d\n", n_elem, n_text, n_other);
	n += snprintf(out + n, sizeof(out) - n, "tags:");
	for (int i = 0; i < n_names && i < 20; i++)
		n += snprintf(out + n, sizeof(out) - n, " %s", names[i]);
	n += snprintf(out + n, sizeof(out) - n, "\nM2 OK\n");

	printf("%s", out);
	FILE *f = fopen("/M2RESULT.TXT", "w");
	if (f) { fwrite(out, 1, n, f); fclose(f); }

	mdb_destroy(p);
	return 0;
}
