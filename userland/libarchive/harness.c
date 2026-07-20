// harness.c - hosted unit + interop driver for the archiver core.
#define ARC_HOST
#include "arc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } else { printf("ok: %s\n", msg); } } while(0)

static uint8_t *readfile(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n ? n : 1);
    fread(b, 1, n, f); fclose(f);
    *len = (size_t)n;
    return b;
}
static void writefile(const char *path, const uint8_t *b, size_t n) {
    FILE *f = fopen(path, "wb"); fwrite(b, 1, n, f); fclose(f);
}

int main(void) {
    // Build a known mixed-content payload: repetitive (compressible) + pseudo-random.
    size_t N = 50000;
    uint8_t *payload = malloc(N);
    for (size_t i = 0; i < N; i++) {
        if (i % 7 == 0) payload[i] = (uint8_t)(i * 2654435761u >> 13);
        else payload[i] = (uint8_t)('A' + (i % 26));
    }
    const char *text = "Hello MayteraOS archiver! The quick brown fox jumps over the lazy dog. "
                       "1234567890 The quick brown fox jumps over the lazy dog. AAAAAAAAAAAA";
    size_t tlen = strlen(text);

    printf("==== DEFLATE/INFLATE round-trip ====\n");
    {
        size_t dl; uint8_t *def = arc_deflate(payload, N, &dl);
        CHECK(def != NULL, "deflate returns buffer");
        uint8_t *back = malloc(N); size_t got;
        int r = arc_inflate(def, dl, back, N, &got);
        CHECK(r == 0 && got == N && memcmp(back, payload, N) == 0, "deflate->inflate identical");
        printf("   deflate: %zu -> %zu bytes (%.1f%%)\n", N, dl, 100.0*dl/N);
        free(def); free(back);
    }

    printf("==== GZIP round-trip + interop ====\n");
    {
        size_t gl; uint8_t *gz = arc_gzip_compress(payload, N, "payload.bin", &gl);
        CHECK(gz != NULL, "gzip_compress");
        size_t bl; uint8_t *back = arc_gzip_decompress(gz, gl, &bl);
        CHECK(back && bl == N && memcmp(back, payload, N) == 0, "gzip self round-trip");
        writefile("/tmp/arc_h/out.gz", gz, gl);
        free(gz); free(back);
        // real gunzip must extract our gz
        int rc = system("cd /tmp/arc_h && gunzip -f -k out.gz 2>/dev/null && mv out out.fromgunzip");
        (void)rc;
        size_t fl; uint8_t *fromsys = readfile("/tmp/arc_h/out.fromgunzip", &fl);
        CHECK(fromsys && fl == N && memcmp(fromsys, payload, N) == 0, "INTEROP: real gunzip extracts our .gz");
        if (fromsys) free(fromsys);
    }
    {
        // our decompressor must read a real gzip file
        writefile("/tmp/arc_h/src.bin", payload, N);
        int rc = system("cd /tmp/arc_h && gzip -f -k -9 src.bin"); (void)rc;
        size_t gl; uint8_t *gz = readfile("/tmp/arc_h/src.bin.gz", &gl);
        size_t bl; uint8_t *back = gz ? arc_gzip_decompress(gz, gl, &bl) : NULL;
        CHECK(back && bl == N && memcmp(back, payload, N) == 0, "INTEROP: we extract real gzip -9 .gz");
        if (gz) free(gz); if (back) free(back);
    }

    printf("==== TAR round-trip + interop ====\n");
    arc_entry ents[3];
    memset(ents, 0, sizeof(ents));
    strcpy(ents[0].name, "dir/"); ents[0].is_dir = 1; ents[0].mode = 0755;
    strcpy(ents[1].name, "dir/file1.txt"); ents[1].data = (uint8_t*)text; ents[1].size = tlen; ents[1].mode = 0644;
    strcpy(ents[2].name, "data.bin"); ents[2].data = payload; ents[2].size = N; ents[2].mode = 0644;
    {
        size_t tl; uint8_t *tar = arc_tar_create(ents, 3, &tl);
        CHECK(tar != NULL, "tar_create");
        int cnt; arc_entry *ex = arc_tar_extract(tar, tl, &cnt);
        CHECK(ex && cnt == 3, "tar self extract count");
        CHECK(ex && ex[1].size == tlen && memcmp(ex[1].data, text, tlen) == 0, "tar file1 contents");
        CHECK(ex && ex[2].size == N && memcmp(ex[2].data, payload, N) == 0, "tar data.bin contents");
        CHECK(ex && ex[0].is_dir, "tar dir flag");
        writefile("/tmp/arc_h/out.tar", tar, tl);
        arc_free_entries(ex, cnt); free(tar);
        // real tar must list+extract
        int rc = system("cd /tmp/arc_h && rm -rf tx && mkdir tx && tar xf out.tar -C tx 2>/tmp/arc_h/tarerr.txt"); (void)rc;
        size_t fl; uint8_t *f1 = readfile("/tmp/arc_h/tx/dir/file1.txt", &fl);
        CHECK(f1 && fl == tlen && memcmp(f1, text, tlen) == 0, "INTEROP: real tar extracts our .tar (file1)");
        size_t fl2; uint8_t *f2 = readfile("/tmp/arc_h/tx/data.bin", &fl2);
        CHECK(f2 && fl2 == N && memcmp(f2, payload, N) == 0, "INTEROP: real tar extracts our .tar (data.bin)");
        if (f1) free(f1); if (f2) free(f2);
    }
    {
        // our extractor must read a real tar
        int rc = system("cd /tmp/arc_h && rm -rf ti && mkdir -p ti/sub && cp src.bin ti/sub/a.bin && echo 'real tar entry' > ti/note.txt && tar cf realtar.tar -C ti ."); (void)rc;
        size_t tl; uint8_t *tar = readfile("/tmp/arc_h/realtar.tar", &tl);
        int cnt; arc_entry *ex = tar ? arc_tar_extract(tar, tl, &cnt) : NULL;
        int ok_note = 0, ok_bin = 0;
        for (int i = 0; ex && i < cnt; i++) {
            if (strstr(ex[i].name, "note.txt") && ex[i].size >= 14) ok_note = 1;
            if (strstr(ex[i].name, "a.bin") && ex[i].size == N && memcmp(ex[i].data, payload, N) == 0) ok_bin = 1;
        }
        CHECK(ex != NULL, "INTEROP: we parse real tar");
        CHECK(ok_note, "INTEROP: real tar note.txt found");
        CHECK(ok_bin, "INTEROP: real tar a.bin contents match");
        if (ex) arc_free_entries(ex, cnt); if (tar) free(tar);
    }

    printf("==== TAR.GZ round-trip + interop ====\n");
    {
        size_t gl; uint8_t *gz = arc_targz_create(ents, 3, &gl);
        CHECK(gz != NULL, "targz_create");
        int cnt; arc_entry *ex = arc_targz_extract(gz, gl, &cnt);
        CHECK(ex && cnt == 3 && ex[2].size == N && memcmp(ex[2].data, payload, N) == 0, "targz self round-trip");
        writefile("/tmp/arc_h/out.tgz", gz, gl);
        arc_free_entries(ex, cnt); free(gz);
        int rc = system("cd /tmp/arc_h && rm -rf gx && mkdir gx && tar xzf out.tgz -C gx 2>/dev/null"); (void)rc;
        size_t fl; uint8_t *f1 = readfile("/tmp/arc_h/gx/dir/file1.txt", &fl);
        CHECK(f1 && fl == tlen && memcmp(f1, text, tlen) == 0, "INTEROP: real tar xzf extracts our .tgz");
        if (f1) free(f1);
        // we extract a real tgz
        rc = system("cd /tmp/arc_h && tar czf realtgz.tgz -C ti ."); (void)rc;
        size_t gl2; uint8_t *rg = readfile("/tmp/arc_h/realtgz.tgz", &gl2);
        int cnt2; arc_entry *ex2 = rg ? arc_targz_extract(rg, gl2, &cnt2) : NULL;
        int found = 0;
        for (int i = 0; ex2 && i < cnt2; i++)
            if (strstr(ex2[i].name, "a.bin") && ex2[i].size == N && memcmp(ex2[i].data, payload, N) == 0) found = 1;
        CHECK(ex2 && found, "INTEROP: we extract real tar czf .tgz");
        if (ex2) arc_free_entries(ex2, cnt2); if (rg) free(rg);
    }

    printf("==== ZIP (store + deflate) round-trip + interop ====\n");
    for (int usedef = 0; usedef <= 1; usedef++) {
        size_t zl; uint8_t *zip = arc_zip_create(ents, 3, usedef, &zl);
        CHECK(zip != NULL, usedef ? "zip_create (deflate)" : "zip_create (store)");
        int cnt; arc_entry *ex = arc_zip_extract(zip, zl, &cnt);
        CHECK(ex && cnt == 3, "zip self extract count");
        int okf1 = 0, okbin = 0, okdir = 0;
        for (int i = 0; ex && i < cnt; i++) {
            if (strstr(ex[i].name, "file1.txt") && ex[i].size == tlen && memcmp(ex[i].data, text, tlen) == 0) okf1 = 1;
            if (strstr(ex[i].name, "data.bin") && ex[i].size == N && memcmp(ex[i].data, payload, N) == 0) okbin = 1;
            if (ex[i].is_dir) okdir = 1;
        }
        CHECK(okf1 && okbin && okdir, usedef ? "zip self round-trip (deflate)" : "zip self round-trip (store)");
        const char *zp = usedef ? "/tmp/arc_h/out_def.zip" : "/tmp/arc_h/out_store.zip";
        writefile(zp, zip, zl);
        arc_free_entries(ex, cnt); free(zip);
        // real unzip must extract
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "cd /tmp/arc_h && rm -rf zx && mkdir zx && unzip -o %s -d zx >/tmp/arc_h/unziplog.txt 2>&1", zp);
        int rc = system(cmd); (void)rc;
        size_t fl; uint8_t *f1 = readfile("/tmp/arc_h/zx/dir/file1.txt", &fl);
        CHECK(f1 && fl == tlen && memcmp(f1, text, tlen) == 0, usedef ? "INTEROP: real unzip extracts our deflate zip" : "INTEROP: real unzip extracts our store zip");
        size_t fl2; uint8_t *f2 = readfile("/tmp/arc_h/zx/data.bin", &fl2);
        CHECK(f2 && fl2 == N && memcmp(f2, payload, N) == 0, usedef ? "INTEROP: real unzip deflate data.bin" : "INTEROP: real unzip store data.bin");
        // unzip -t integrity (CRC) test
        snprintf(cmd, sizeof(cmd), "cd /tmp/arc_h && unzip -t %s >/dev/null 2>&1", zp);
        rc = system(cmd);
        CHECK(rc == 0, usedef ? "INTEROP: unzip -t passes (deflate zip CRC ok)" : "INTEROP: unzip -t passes (store zip CRC ok)");
        if (f1) free(f1); if (f2) free(f2);
    }
    {
        // we extract a real zip (created by Info-ZIP, which uses deflate + data descriptors)
        int rc = system("cd /tmp/arc_h && rm -f realzip.zip && rm -rf rz && mkdir -p rz/sub && cp src.bin rz/sub/a.bin && echo 'zip note' > rz/note.txt && (cd rz && zip -r -9 ../realzip.zip . >/dev/null 2>&1)"); (void)rc;
        size_t zl; uint8_t *zip = readfile("/tmp/arc_h/realzip.zip", &zl);
        int cnt; arc_entry *ex = zip ? arc_zip_extract(zip, zl, &cnt) : NULL;
        int found = 0, foundnote = 0;
        for (int i = 0; ex && i < cnt; i++) {
            if (strstr(ex[i].name, "a.bin") && ex[i].size == N && memcmp(ex[i].data, payload, N) == 0) found = 1;
            if (strstr(ex[i].name, "note.txt")) foundnote = 1;
        }
        CHECK(ex != NULL, "INTEROP: we parse real Info-ZIP zip");
        CHECK(found, "INTEROP: real zip a.bin contents match (we inflate deflate method)");
        CHECK(foundnote, "INTEROP: real zip note.txt found");
        if (ex) arc_free_entries(ex, cnt); if (zip) free(zip);
    }

    printf("==== edge cases ====\n");
    {
        // empty file gzip
        size_t gl; uint8_t *gz = arc_gzip_compress((const uint8_t*)"", 0, "empty", &gl);
        size_t bl; uint8_t *back = gz ? arc_gzip_decompress(gz, gl, &bl) : NULL;
        CHECK(back && bl == 0, "gzip empty input round-trip");
        if (gz) { writefile("/tmp/arc_h/empty.gz", gz, gl); free(gz); }
        if (back) free(back);
        int rc = system("cd /tmp/arc_h && gunzip -t empty.gz 2>/dev/null");
        CHECK(rc == 0, "INTEROP: gunzip -t passes on our empty .gz");
    }
    {
        // 1-byte file
        uint8_t one = 0x5A;
        size_t gl; uint8_t *gz = arc_gzip_compress(&one, 1, NULL, &gl);
        size_t bl; uint8_t *back = gz ? arc_gzip_decompress(gz, gl, &bl) : NULL;
        CHECK(back && bl == 1 && back[0] == 0x5A, "gzip 1-byte round-trip");
        if (gz) free(gz); if (back) free(back);
    }

    printf("\n==== RESULT: %s (%d failures) ====\n", fails ? "FAIL" : "PASS", fails);
    free(payload);
    return fails ? 1 : 0;
}
