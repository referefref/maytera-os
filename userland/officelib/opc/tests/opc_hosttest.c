// opc_hosttest.c - hosted unit test for the OPC package layer (agent 1).
// Build+run on the userland CT with the host gcc, no VM needed:
//   gcc -DARC_HOST -I<officelib/include> -I<libarchive> \
//       opc.c <libarchive>/arc.c opc_hosttest.c -o opctest
//   ./opctest <corpus samples dir> <scratch out dir>
// Exercises the real logic against the QA corpus and a write round-trip.
#define ARC_HOST
#include "opc.h"
#include "arc.h"
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

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    const char *out = argc > 2 ? argv[2] : ".";
    char path[1024];

    printf("==== sample.docx (OOXML) ====\n");
    {
        snprintf(path, sizeof(path), "%s/sample.docx", dir);
        unsigned long len = 0;
        unsigned char *bytes = readfile(path, &len);
        CHECK(bytes != NULL, "read sample.docx bytes");
        opc_pkg *p = opc_open_mem(bytes, len);
        CHECK(p != NULL, "opc_open_mem(sample.docx)");
        if (p) {
            CHECK(opc_count(p) > 0, "docx has >0 parts");
            opc_part *ct = opc_get(p, "[Content_Types].xml");
            CHECK(ct && ct->size > 0, "[Content_Types].xml present and non-empty");
            opc_part *doc = opc_get(p, "word/document.xml");
            CHECK(doc && doc->size > 0, "word/document.xml present and non-empty");

            // Find the first rId in the document .rels, then resolve it.
            opc_part *rels = opc_get(p, "word/_rels/document.xml.rels");
            CHECK(rels && rels->size > 0, "word/_rels/document.xml.rels present");
            if (rels) {
                char rid[256] = {0};
                // pull the first Id="..." out of the rels text for the test
                const char *h = (const char *)rels->data;
                const char *m = NULL;
                for (unsigned long i = 0; i + 4 <= rels->size; i++)
                    if (memcmp(h + i, "Id=\"", 4) == 0) { m = h + i + 4; break; }
                CHECK(m != NULL, "rels contains an Id attribute");
                if (m) {
                    int k = 0;
                    while (*m && *m != '"' && k < 255) rid[k++] = *m++;
                    rid[k] = 0;
                    char tgt[512] = {0};
                    const char *r = opc_rel_target(p, "word/_rels/document.xml.rels",
                                                   rid, tgt, sizeof(tgt));
                    printf("      resolved %s -> %s\n", rid, r ? tgt : "(null)");
                    CHECK(r != NULL && tgt[0] != 0, "opc_rel_target resolves the rId");
                }
            }
            opc_free(p);
        }
        free(bytes);
    }

    printf("==== sample.odt (ODF) ====\n");
    {
        snprintf(path, sizeof(path), "%s/sample.odt", dir);
        unsigned long len = 0;
        unsigned char *bytes = readfile(path, &len);
        CHECK(bytes != NULL, "read sample.odt bytes");
        opc_pkg *p = opc_open_mem(bytes, len);
        CHECK(p != NULL, "opc_open_mem(sample.odt)");
        if (p) {
            opc_part *mt = opc_get(p, "mimetype");
            CHECK(mt && mt->size > 0, "mimetype part present");
            const char *want = "application/vnd.oasis.opendocument.text";
            CHECK(mt && mt->size == strlen(want) &&
                  memcmp(mt->data, want, mt->size) == 0,
                  "mimetype == application/vnd.oasis.opendocument.text");
            opc_free(p);
        }
        free(bytes);
    }

    printf("==== write round-trip ====\n");
    {
        const char *n1 = "hello.txt";
        const unsigned char d1[] = "Hi MayteraOS OPC";
        const char *n2 = "word/document.xml";
        const unsigned char d2[] = "<?xml version=\"1.0\"?><doc><p>round</p></doc>";
        unsigned long s1 = (unsigned long)strlen((const char *)d1);
        unsigned long s2 = (unsigned long)strlen((const char *)d2);

        opc_pkg *w = opc_new();
        CHECK(w != NULL, "opc_new");
        CHECK(opc_put(w, n1, d1, s1) == 0, "opc_put part 1");
        CHECK(opc_put(w, n2, d2, s2) == 0, "opc_put part 2");
        CHECK(opc_count(w) == 2, "write pkg has 2 parts");

        unsigned long zl = 0;
        unsigned char *z = opc_save_mem(w, &zl);
        CHECK(z != NULL && zl > 0, "opc_save_mem produced a zip");

        // also drop it to disk so the runner can `unzip -l` it
        snprintf(path, sizeof(path), "%s/roundtrip.zip", out);
        CHECK(opc_save_file(w, path) == 0, "opc_save_file wrote roundtrip.zip");

        opc_pkg *r = opc_open_mem(z, zl);
        CHECK(r != NULL, "reopen the saved zip");
        if (r) {
            CHECK(opc_count(r) == 2, "reopened pkg has 2 parts");
            opc_part *p1 = opc_get(r, n1);
            opc_part *p2 = opc_get(r, n2);
            CHECK(p1 && p1->size == s1 && memcmp(p1->data, d1, s1) == 0,
                  "part 1 bytes identical after round-trip");
            CHECK(p2 && p2->size == s2 && memcmp(p2->data, d2, s2) == 0,
                  "part 2 bytes identical after round-trip");
            opc_free(r);
        }
        free(z);
        opc_free(w);
    }

    printf("\n==== %s (%d failure%s) ====\n",
           fails ? "SOME TESTS FAILED" : "ALL TESTS PASSED",
           fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
