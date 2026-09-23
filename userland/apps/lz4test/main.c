// lz4test - running proof for the mports LZ4 port (userland/ports/lz4).
//
// Links the static liblz4.a that mports.sh built from the sha256-pinned upstream
// tarball and round-trips a buffer through both the block API
// (LZ4_compress_default / LZ4_decompress_safe) and the frame API
// (LZ4F_compressFrame / LZ4F_decompress). Correctness is proven by comparing the
// decompressed bytes to the original, not by internal self-consistency; it also
// asserts the compressed form is actually smaller for compressible input.
//
// OUTPUT DISCIPLINE (see zlibtest): one write(2) per serial record.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "lz4.h"
#include "lz4frame.h"

static int g_pass = 0, g_fail = 0;
static void line(const char *s){ write(2, s, strlen(s)); }
static void ck(const char *what, int ok){
    char b[256]; if(ok) g_pass++; else g_fail++;
    snprintf(b,sizeof(b),"[LZ4TEST] %s %s\n", ok?"PASS":"FAIL", what);
    line(b);
}

int main(void){
    line("[LZ4TEST] start\n");
    char vb[64];
    snprintf(vb,sizeof(vb),"[LZ4TEST] LZ4_versionNumber=%d\n", LZ4_versionNumber());
    line(vb);

    // A compressible source: a phrase repeated many times.
    char src[4096];
    for(int i=0;i<(int)sizeof(src);i++) src[i] = "MayteraOS loves LZ4 "[i % 20];
    const int srclen = (int)sizeof(src);

    // ---- Block API round-trip ----
    int bound = LZ4_compressBound(srclen);
    char *cbuf = (char*)malloc(bound);
    int clen = LZ4_compress_default(src, cbuf, srclen, bound);
    ck("block compress produced output", clen > 0);
    ck("block compressed smaller", clen > 0 && clen < srclen);
    char *dbuf = (char*)malloc(srclen);
    int dlen = LZ4_decompress_safe(cbuf, dbuf, clen, srclen);
    ck("block decompress full length", dlen == srclen);
    ck("block round-trip identical", dlen == srclen && memcmp(src, dbuf, srclen)==0);
    char rb[128];
    snprintf(rb,sizeof(rb),"[LZ4TEST]      block src=%d comp=%d dec=%d\n", srclen, clen, dlen);
    line(rb);
    free(cbuf); free(dbuf);

    // ---- Frame API round-trip ----
    size_t fbound = LZ4F_compressFrameBound((size_t)srclen, NULL);
    char *fbuf = (char*)malloc(fbound);
    size_t flen = LZ4F_compressFrame(fbuf, fbound, src, (size_t)srclen, NULL);
    ck("frame compress ok", !LZ4F_isError(flen) && flen > 0);
    LZ4F_decompressionContext_t ctx = NULL;
    LZ4F_errorCode_t ec = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
    ck("frame ctx created", !LZ4F_isError(ec));
    char *fdst = (char*)malloc(srclen);
    size_t dstsz = (size_t)srclen, srcsz = flen;
    size_t r = LZ4F_decompress(ctx, fdst, &dstsz, fbuf, &srcsz, NULL);
    ck("frame decompress no error", !LZ4F_isError(r));
    ck("frame round-trip identical", dstsz == (size_t)srclen && memcmp(src, fdst, srclen)==0);
    snprintf(rb,sizeof(rb),"[LZ4TEST]      frame src=%d comp=%d dec=%d\n", srclen, (int)flen, (int)dstsz);
    line(rb);
    LZ4F_freeDecompressionContext(ctx);
    free(fbuf); free(fdst);

    snprintf(rb,sizeof(rb),"[LZ4TEST] done pass=%d fail=%d\n", g_pass, g_fail);
    line(rb);
    return g_fail ? 1 : 0;
}
