// pptx_odp_hosttest.c - hosted unit test for the PPTX + ODP format layer.
// Format agent 7 (officepptx). Build+run on the host gcc, no VM:
//   gcc -DARC_HOST -I<officelib/include> -I<libarchive> \
//       fmt_pptx_odp/pptx.c fmt_pptx_odp/odp.c opc/opc.c oxml/oxml.c \
//       model/slidemodel.c model/docmodel.c <libarchive>/arc.c \
//       fmt_pptx_odp/tests/pptx_odp_hosttest.c -o pptxtest
//   ./pptxtest <corpus samples dir>
// Values mirror tools/office-corpus/oracle.json (sample.pptx / sample.odp).
#define ARC_HOST
#include "formats.h"
#include "opc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); fails++; } \
    else         { printf("ok:   %s\n", (msg)); } } while (0)

static unsigned char *readfile(const char *path, unsigned long *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = (unsigned char *)malloc(n ? (size_t)n : 1);
    size_t got = fread(b, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(b); return NULL; }
    *len = (unsigned long)n;
    return b;
}

// find the SHP_TITLE / SHP_TEXTBOX in a slide; return body textbox shape*.
static const shape *find_body(const slide *s) {
    for (int i = 0; i < s->nshape; i++)
        if (s->shapes[i].type == SHP_TEXTBOX) return &s->shapes[i];
    return NULL;
}

static void check_model(presentation *p, const char *tag) {
    char m[128];
    snprintf(m, sizeof(m), "%s: 2 slides", tag);
    CHECK(p && p->nslide == 2, m);
    if (!p || p->nslide < 2) return;

    snprintf(m, sizeof(m), "%s: slide1 title == Maytera Office", tag);
    CHECK(p->slides[0].title && strcmp(p->slides[0].title, "Maytera Office") == 0, m);

    // slide 1 subtitle body
    const shape *b1 = find_body(&p->slides[0]);
    snprintf(m, sizeof(m), "%s: slide1 has subtitle body 'Presentation subtitle'", tag);
    CHECK(b1 && b1->npara >= 1 && b1->paras[0].nrun >= 1 &&
          strcmp(b1->paras[0].runs[0].text, "Presentation subtitle") == 0, m);

    snprintf(m, sizeof(m), "%s: slide2 title == Agenda", tag);
    CHECK(p->slides[1].title && strcmp(p->slides[1].title, "Agenda") == 0, m);

    const shape *b2 = find_body(&p->slides[1]);
    snprintf(m, sizeof(m), "%s: slide2 body has 3 paragraphs", tag);
    CHECK(b2 && b2->npara == 3, m);
    if (b2 && b2->npara == 3) {
        snprintf(m, sizeof(m), "%s: slide2 body is a bullet list (list_level>=0)", tag);
        CHECK(b2->paras[0].list_level >= 0 && b2->paras[1].list_level >= 0 &&
              b2->paras[2].list_level >= 0, m);
        const char *want[3] = { "First topic", "Second topic", "Third topic" };
        int ok = 1;
        for (int i = 0; i < 3; i++)
            if (!(b2->paras[i].nrun >= 1 && strcmp(b2->paras[i].runs[0].text, want[i]) == 0)) ok = 0;
        snprintf(m, sizeof(m), "%s: slide2 bullets = First/Second/Third topic", tag);
        CHECK(ok, m);
    }
}

static int is_zip(const unsigned char *b, unsigned long n) {
    return n >= 4 && b[0] == 'P' && b[1] == 'K' && b[2] == 3 && b[3] == 4;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    unsigned long len;

    printf("==== sample.pptx (PresentationML) ====\n");
    snprintf(path, sizeof(path), "%s/sample.pptx", dir);
    {
        unsigned char *buf = readfile(path, &len);
        CHECK(buf != NULL, "pptx: read corpus file");
        if (buf) {
            presentation *p = NULL;
            int rc = pptx_load(buf, len, &p);
            CHECK(rc == 0 && p != NULL, "pptx: pptx_load ok");
            free(buf);
            check_model(p, "pptx-load");

            // round-trip
            unsigned char *outb = NULL; unsigned long outl = 0;
            int src = pptx_save(p, &outb, &outl);
            CHECK(src == 0 && outb && outl, "pptx: pptx_save ok");
            CHECK(outb && is_zip(outb, outl), "pptx: export is a valid zip (PK magic)");
            if (outb) {
                opc_pkg *pk = opc_open_mem(outb, outl);
                CHECK(pk != NULL, "pptx: export re-opens as OPC package");
                if (pk) {
                    CHECK(opc_get(pk, "ppt/presentation.xml") != NULL, "pptx: export has ppt/presentation.xml");
                    CHECK(opc_get(pk, "[Content_Types].xml") != NULL, "pptx: export has [Content_Types].xml");
                    opc_free(pk);
                }
                presentation *p2 = NULL;
                int rc2 = pptx_load(outb, outl, &p2);
                CHECK(rc2 == 0 && p2, "pptx: reload of export ok");
                check_model(p2, "pptx-roundtrip");
                if (p2) pres_free(p2);
                free(outb);
            }
            if (p) pres_free(p);
        }
    }

    printf("==== sample.odp (OpenDocument) ====\n");
    snprintf(path, sizeof(path), "%s/sample.odp", dir);
    {
        unsigned char *buf = readfile(path, &len);
        CHECK(buf != NULL, "odp: read corpus file");
        if (buf) {
            presentation *p = NULL;
            int rc = odp_load(buf, len, &p);
            CHECK(rc == 0 && p != NULL, "odp: odp_load ok");
            free(buf);
            check_model(p, "odp-load");

            unsigned char *outb = NULL; unsigned long outl = 0;
            int src = odp_save(p, &outb, &outl);
            CHECK(src == 0 && outb && outl, "odp: odp_save ok");
            CHECK(outb && is_zip(outb, outl), "odp: export is a valid zip (PK magic)");
            if (outb) {
                opc_pkg *pk = opc_open_mem(outb, outl);
                CHECK(pk != NULL, "odp: export re-opens as OPC package");
                if (pk) {
                    CHECK(opc_get(pk, "content.xml") != NULL, "odp: export has content.xml");
                    CHECK(opc_get(pk, "mimetype") != NULL, "odp: export has mimetype");
                    opc_part *mt = opc_get(pk, "mimetype");
                    CHECK(mt && mt->size == strlen("application/vnd.oasis.opendocument.presentation") &&
                          memcmp(mt->data, "application/vnd.oasis.opendocument.presentation", mt->size) == 0,
                          "odp: mimetype content correct");
                    opc_free(pk);
                }
                presentation *p2 = NULL;
                int rc2 = odp_load(outb, outl, &p2);
                CHECK(rc2 == 0 && p2, "odp: reload of export ok");
                check_model(p2, "odp-roundtrip");
                if (p2) pres_free(p2);
                free(outb);
            }
            if (p) pres_free(p);
        }
    }

    printf("\n==== %s (%d failure%s) ====\n", fails ? "FAILURES" : "ALL PASS",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
