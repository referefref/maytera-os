// zstdtest - running proof for the mports zstd port (userland/ports/zstd).
//
// Links the static libzstd.a that mports.sh built from the sha256-pinned upstream
// tarball and round-trips a buffer through the simple one-shot API
// (ZSTD_compress / ZSTD_decompress). Correctness is proven by comparing the
// decompressed bytes to the original and by reading the stored frame content
// size back, not by internal self-consistency; it also asserts the compressed
// form is smaller for compressible input.
//
// OUTPUT DISCIPLINE (see zlibtest): one write(2) per serial record.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "zstd.h"

static int g_pass = 0, g_fail = 0;
static void line(const char *s){ write(2, s, strlen(s)); }
static void ck(const char *what, int ok){
    char b[256]; if(ok) g_pass++; else g_fail++;
    snprintf(b,sizeof(b),"[ZSTDTEST] %s %s\n", ok?"PASS":"FAIL", what);
    line(b);
}

int main(void){
    line("[ZSTDTEST] start\n");
    char vb[64];
    snprintf(vb,sizeof(vb),"[ZSTDTEST] ZSTD_versionNumber=%u maxCLevel=%d\n",
             ZSTD_versionNumber(), ZSTD_maxCLevel());
    line(vb);

    // A compressible source: a phrase repeated many times.
    char src[8192];
    for(int i=0;i<(int)sizeof(src);i++) src[i] = "MayteraOS loves zstd "[i % 21];
    const size_t srclen = sizeof(src);

    size_t bound = ZSTD_compressBound(srclen);
    char *cbuf = (char*)malloc(bound);
    size_t clen = ZSTD_compress(cbuf, bound, src, srclen, 3);
    ck("compress not error", !ZSTD_isError(clen));
    ck("compressed smaller", !ZSTD_isError(clen) && clen < srclen);

    unsigned long long content = ZSTD_getFrameContentSize(cbuf, clen);
    ck("frame content size == srclen", content == (unsigned long long)srclen);

    char *dbuf = (char*)malloc(srclen);
    size_t dlen = ZSTD_decompress(dbuf, srclen, cbuf, clen);
    ck("decompress not error", !ZSTD_isError(dlen));
    ck("decompress full length", dlen == srclen);
    ck("round-trip identical", dlen == srclen && memcmp(src, dbuf, srclen)==0);

    char rb[160];
    snprintf(rb,sizeof(rb),"[ZSTDTEST]      src=%d comp=%d dec=%d content=%d\n",
             (int)srclen, (int)clen, (int)dlen, (int)content);
    line(rb);

    // Context-based API exercise (proves cctx/dctx lifecycle links + runs).
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    ck("cctx created", cc != NULL);
    ZSTD_DCtx *dc = ZSTD_createDCtx();
    ck("dctx created", dc != NULL);
    if(cc && dc){
        size_t cl2 = ZSTD_compressCCtx(cc, cbuf, bound, src, srclen, 5);
        size_t dl2 = ZSTD_decompressDCtx(dc, dbuf, srclen, cbuf, cl2);
        ck("cctx/dctx round-trip identical", dl2 == srclen && memcmp(src, dbuf, srclen)==0);
    }
    if(cc) ZSTD_freeCCtx(cc);
    if(dc) ZSTD_freeDCtx(dc);
    free(cbuf); free(dbuf);

    snprintf(rb,sizeof(rb),"[ZSTDTEST] done pass=%d fail=%d\n", g_pass, g_fail);
    line(rb);
    return g_fail ? 1 : 0;
}
