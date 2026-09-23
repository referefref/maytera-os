// pngtest2 - running proof for the mports libpng port (userland/ports/libpng).
//
// Links the static libpng.a (and, transitively, the ported libz.a) that mports.sh
// built from the sha256-pinned upstream tarballs, and DECODES a small in-memory
// PNG via the libpng 1.6 simplified read API. Correctness is proven against
// external ground truth: the PNG bytes below were produced by CPython's zlib +
// hand-written chunks (a 2x2 RGB image, pixels red/green/blue/white), and the
// decode must recover exactly those dimensions and pixels. A decoder that
// diverged from the PNG spec, or a zlib that mis-inflated IDAT, would fail.
//
// OUTPUT DISCIPLINE (see zlibtest): one write(2) per serial record.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "png.h"

static int g_pass = 0, g_fail = 0;
static void line(const char *s){ write(2, s, strlen(s)); }
static void ck(const char *what, int ok){
    char b[256]; if(ok) g_pass++; else g_fail++;
    snprintf(b,sizeof(b),"[PNGTEST2] %s %s\n", ok?"PASS":"FAIL", what);
    line(b);
}

// 2x2 RGB PNG: [red,green],[blue,white]. Produced by CPython zlib on the build container.
static const unsigned char PNG_DATA[]={137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,2,0,0,0,2,8,2,0,0,0,253,212,154,115,0,0,0,18,73,68,65,84,120,218,99,248,207,192,192,0,194,12,255,129,0,0,31,238,5,251,241,171,186,119,0,0,0,0,73,69,78,68,174,66,96,130};
static const unsigned PNG_LEN=75;

int main(void){
    line("[PNGTEST2] start\n");
    char vb[96];
    snprintf(vb,sizeof(vb),"[PNGTEST2] png_access_version_number=%d ver=%s\n",
             png_access_version_number(), png_get_libpng_ver(NULL));
    line(vb);

    png_image image;
    memset(&image, 0, sizeof image);
    image.version = PNG_IMAGE_VERSION;

    int began = png_image_begin_read_from_memory(&image, PNG_DATA, PNG_LEN);
    ck("begin_read_from_memory ok", began != 0);
    ck("width == 2", image.width == 2);
    ck("height == 2", image.height == 2);

    image.format = PNG_FORMAT_RGBA;
    png_bytep buf = (png_bytep)malloc(PNG_IMAGE_SIZE(image));
    ck("pixel buffer allocated", buf != NULL);
    int fin = png_image_finish_read(&image, NULL, buf, 0, NULL);
    ck("finish_read ok", fin != 0);

    if(fin && buf){
        // Row stride = width*4 (RGBA). Pixel (0,0) red, (1,0) green,
        // (0,1) blue, (1,1) white.
        unsigned char *p = buf;
        ck("pixel(0,0)==red",   p[0]==255 && p[1]==0   && p[2]==0   && p[3]==255);
        ck("pixel(1,0)==green", p[4]==0   && p[5]==255 && p[6]==0   && p[7]==255);
        ck("pixel(0,1)==blue",  p[8]==0   && p[9]==0   && p[10]==255&& p[11]==255);
        ck("pixel(1,1)==white", p[12]==255&& p[13]==255&& p[14]==255&& p[15]==255);
        char pb[160];
        snprintf(pb,sizeof(pb),"[PNGTEST2]      p00=%d,%d,%d p11=%d,%d,%d\n",
                 p[0],p[1],p[2],p[12],p[13],p[14]);
        line(pb);
    }
    if(buf) free(buf);
    png_image_free(&image);

    char b[128];
    snprintf(b,sizeof(b),"[PNGTEST2] done pass=%d fail=%d\n", g_pass, g_fail);
    line(b);
    return g_fail ? 1 : 0;
}
