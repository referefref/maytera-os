#include "opc.h"
#include "arc.h"      // in-memory zip: arc_zip_extract/create, arc_free_entries
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Open Packaging Convention layer (agent 1). A .docx/.xlsx/.pptx or an ODF
// .odt/.ods/.odp is a ZIP of XML "parts". This wraps the in-memory zip in
// arc.h so the memory path (opc_open_mem / opc_save_mem) is freestanding-pure
// with no MayteraOS syscalls: it can be unit-tested headless on the host.
// The only officelib dependency is arc.h; .rels are parsed by a tiny local
// attribute scanner so opc pulls in no oxml.

struct opc_pkg {
    int        is_write;  // 0 = opened for read, 1 = built for write
    arc_entry *raw;       // read: owned by arc (free via arc_free_entries)
                          // write: our own growable array, each .data malloc'd
    int        raw_n;     // number of entries in raw[]
    int        raw_cap;   // capacity of raw[] (write mode only)
    opc_part  *parts;     // view over the non-directory entries
    int        n;         // number of parts exposed
};

// Rebuild the borrowed part view from the current raw[] file entries.
static int rebuild_parts(opc_pkg *p) {
    free(p->parts);
    p->parts = NULL;
    p->n = 0;
    int cnt = 0;
    for (int i = 0; i < p->raw_n; i++)
        if (!p->raw[i].is_dir) cnt++;
    if (cnt == 0) return 0;
    opc_part *pa = (opc_part *)calloc((size_t)cnt, sizeof(opc_part));
    if (!pa) return -1;
    int j = 0;
    for (int i = 0; i < p->raw_n; i++) {
        arc_entry *e = &p->raw[i];
        if (e->is_dir) continue;
        size_t nl = strlen(e->name);
        if (nl >= sizeof(pa[j].name)) nl = sizeof(pa[j].name) - 1;
        memcpy(pa[j].name, e->name, nl);
        pa[j].name[nl] = 0;
        pa[j].data = e->data;                 // borrowed
        pa[j].size = (unsigned long)e->size;
        j++;
    }
    p->parts = pa;
    p->n = cnt;
    return 0;
}

// ---- read ---------------------------------------------------------------

opc_pkg *opc_open_mem(const unsigned char *zip, unsigned long len) {
    if (!zip || !len) return NULL;
    opc_pkg *p = (opc_pkg *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    int rn = 0;
    p->raw = arc_zip_extract((const uint8_t *)zip, (size_t)len, &rn);
    if (!p->raw) { free(p); return NULL; }
    p->raw_n = rn;
    p->is_write = 0;
    if (rebuild_parts(p) != 0) {
        arc_free_entries(p->raw, p->raw_n);
        free(p);
        return NULL;
    }
    return p;
}

opc_pkg *opc_open_file(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    opc_pkg *p = NULL;
    if (got == (size_t)sz) p = opc_open_mem(buf, (unsigned long)sz);
    free(buf);
    return p;
}

int opc_count(opc_pkg *p) { return p ? p->n : 0; }

opc_part *opc_at(opc_pkg *p, int i) {
    if (!p || i < 0 || i >= p->n) return NULL;
    return &p->parts[i];
}

opc_part *opc_get(opc_pkg *p, const char *partname) {
    if (!p || !partname) return NULL;
    if (*partname == '/') partname++;   // parts carry no leading '/'
    for (int i = 0; i < p->n; i++)
        if (strcmp(p->parts[i].name, partname) == 0) return &p->parts[i];
    return NULL;
}

// ---- tiny .rels attribute scanner ---------------------------------------

static const char *find_sub(const char *h, const char *hend, const char *n) {
    size_t nl = strlen(n);
    if (nl == 0) return h;
    for (const char *q = h; q + nl <= hend; q++)
        if (memcmp(q, n, nl) == 0) return q;
    return NULL;
}

static int is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Extract attribute `name`'s quoted value from within the tag [tag,tagend).
// Requires a boundary before the name and an '=' (after optional whitespace)
// after it, so "Target" is not matched by "TargetMode". Handles ' and ".
static int get_attr(const char *tag, const char *tagend, const char *name,
                    char *out, unsigned long cap) {
    size_t nl = strlen(name);
    const char *q = tag;
    while (q < tagend) {
        const char *f = find_sub(q, tagend, name);
        if (!f) return 0;
        int okpre = (f == tag) || is_ws(f[-1]) || f[-1] == '<';
        const char *a = f + nl;
        while (a < tagend && is_ws(*a)) a++;
        if (okpre && a < tagend && *a == '=') {
            a++;
            while (a < tagend && is_ws(*a)) a++;
            if (a < tagend && (*a == '"' || *a == '\'')) {
                char quote = *a++;
                const char *v = a;
                while (a < tagend && *a != quote) a++;
                unsigned long vl = (unsigned long)(a - v);
                if (cap == 0) return 0;
                if (vl >= cap) vl = cap - 1;
                memcpy(out, v, vl);
                out[vl] = 0;
                return 1;
            }
        }
        q = f + nl;
    }
    return 0;
}

const char *opc_rel_target(opc_pkg *p, const char *rels_part,
                           const char *rid, char *out, unsigned long outcap) {
    if (!p || !rels_part || !rid || !out || outcap == 0) return NULL;
    opc_part *rp = opc_get(p, rels_part);
    if (!rp || !rp->data) return NULL;
    const char *h = (const char *)rp->data;
    const char *hend = h + rp->size;
    const char *q = h;
    char idbuf[256], tbuf[512];
    while ((q = find_sub(q, hend, "<Relationship")) != NULL) {
        const char *tagend = find_sub(q, hend, ">");
        if (!tagend) break;
        if (get_attr(q, tagend, "Id", idbuf, sizeof(idbuf)) &&
            strcmp(idbuf, rid) == 0) {
            if (get_attr(q, tagend, "Target", tbuf, sizeof(tbuf))) {
                unsigned long tl = strlen(tbuf);
                if (tl >= outcap) tl = outcap - 1;
                memcpy(out, tbuf, tl);
                out[tl] = 0;
                return out;
            }
            return NULL;   // matched the rId but it has no Target
        }
        q = tagend + 1;
    }
    return NULL;
}

// ---- write --------------------------------------------------------------

opc_pkg *opc_new(void) {
    opc_pkg *p = (opc_pkg *)calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->is_write = 1;
    return p;
}

int opc_put(opc_pkg *p, const char *partname,
            const unsigned char *data, unsigned long size) {
    if (!p || !p->is_write || !partname) return -1;
    if (*partname == '/') partname++;
    if (partname[0] == 0 || strlen(partname) >= 256) return -1;
    if (size && !data) return -1;

    // Replace an existing part with the same name (last write wins).
    for (int i = 0; i < p->raw_n; i++) {
        if (!p->raw[i].is_dir && strcmp(p->raw[i].name, partname) == 0) {
            uint8_t *nd = NULL;
            if (size) {
                nd = (uint8_t *)malloc(size);
                if (!nd) return -1;
                memcpy(nd, data, size);
            }
            free(p->raw[i].data);
            p->raw[i].data = nd;
            p->raw[i].size = size;
            return rebuild_parts(p);
        }
    }

    if (p->raw_n == p->raw_cap) {
        int nc = p->raw_cap ? p->raw_cap * 2 : 8;
        arc_entry *ne = (arc_entry *)realloc(p->raw, (size_t)nc * sizeof(arc_entry));
        if (!ne) return -1;
        p->raw = ne;
        p->raw_cap = nc;
    }
    arc_entry *e = &p->raw[p->raw_n];
    memset(e, 0, sizeof(*e));
    strcpy(e->name, partname);        // bounded by the length check above
    e->is_dir = 0;
    e->mode = 0;
    e->size = size;
    e->data = NULL;
    if (size) {
        e->data = (uint8_t *)malloc(size);
        if (!e->data) return -1;
        memcpy(e->data, data, size);
    }
    p->raw_n++;
    return rebuild_parts(p);
}

unsigned char *opc_save_mem(opc_pkg *p, unsigned long *out_len) {
    if (out_len) *out_len = 0;
    if (!p) return NULL;
    size_t zl = 0;
    // deflate (method 8): universally decompressible by any inflate.
    uint8_t *z = arc_zip_create(p->raw, p->raw_n, 1, &zl);
    if (!z) return NULL;
    if (out_len) *out_len = (unsigned long)zl;
    return z;   // caller frees with free()
}

unsigned char *opc_save_mem_odf(opc_pkg *p, unsigned long *out_len) {
    if (out_len) *out_len = 0;
    if (!p) return NULL;
    size_t zl = 0;
    // Deflate everything EXCEPT `mimetype`, which arc emits first and stored so
    // the package is a conformant ODF zip (mimetype leads, uncompressed).
    uint8_t *z = arc_zip_create_ex(p->raw, p->raw_n, 1, "mimetype", &zl);
    if (!z) return NULL;
    if (out_len) *out_len = (unsigned long)zl;
    return z;   // caller frees with free()
}

int opc_save_file(opc_pkg *p, const char *path) {
    if (!p || !path) return -1;
    unsigned long zl = 0;
    unsigned char *z = opc_save_mem(p, &zl);
    if (!z) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return -1; }
    size_t w = fwrite(z, 1, zl, f);
    fclose(f);
    free(z);
    return (w == (size_t)zl) ? 0 : -1;
}

void opc_free(opc_pkg *p) {
    if (!p) return;
    if (p->is_write) {
        for (int i = 0; i < p->raw_n; i++) free(p->raw[i].data);
        free(p->raw);
    } else if (p->raw) {
        arc_free_entries(p->raw, p->raw_n);
    }
    free(p->parts);
    free(p);
}
