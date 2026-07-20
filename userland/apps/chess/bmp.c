/* bmp.c - minimal uncompressed 24/32-bit BMP loader for Maytera Chess.
 * Returns malloc'd 0xAARRGGBB pixels (top-down), alpha 0 for magenta
 * (#FF00FF) color-key texels, 0xFF otherwise. NULL if missing/unsupported. */
#include "gfx.h"
#include <stdlib.h>
#include "syscall.h"
#include "fcntl.h"

static unsigned rd32(const unsigned char *p){return (unsigned)p[0]|((unsigned)p[1]<<8)|((unsigned)p[2]<<16)|((unsigned)p[3]<<24);}
static unsigned rd16(const unsigned char *p){return (unsigned)p[0]|((unsigned)p[1]<<8);}

static unsigned char *read_whole(const char *path, long *len) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return 0;
    long cap = 65536, n = 0;
    unsigned char *b = (unsigned char *)malloc(cap);
    if (!b) { sys_close(fd); return 0; }
    for (;;) {
        if (n == cap) { unsigned char *nb = realloc(b, cap*2); if (!nb){free(b);sys_close(fd);return 0;} b=nb; cap*=2; }
        long r = sys_read(fd, b+n, cap-n);
        if (r < 0) { free(b); sys_close(fd); return 0; }
        if (r == 0) break;
        n += r;
    }
    sys_close(fd); *len = n; return b;
}

unsigned int *cbmp_load(const char *path, int *ow, int *oh) {
    long len = 0;
    unsigned char *f = read_whole(path, &len);
    if (!f) return 0;
    if (len < 54 || f[0] != 'B' || f[1] != 'M') { free(f); return 0; }
    unsigned off = rd32(f+10);
    int w = (int)rd32(f+18), hh = (int)rd32(f+22), bpp = (int)rd16(f+28);
    unsigned comp = rd32(f+30);
    int topdown = hh < 0, h = topdown ? -hh : hh;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || (bpp != 24 && bpp != 32) || (comp != 0 && comp != 3)) { free(f); return 0; }
    long stride = ((long)w * (bpp/8) + 3) & ~3L;
    if ((long)off + stride*h > len) { free(f); return 0; }
    unsigned int *px = (unsigned int *)malloc((size_t)w*h*4);
    if (!px) { free(f); return 0; }
    for (int y = 0; y < h; y++) {
        const unsigned char *row = f + off + stride*(topdown ? y : (h-1-y));
        unsigned int *d = px + (long)y*w;
        for (int x = 0; x < w; x++) {
            const unsigned char *s = row + (long)x*(bpp/8);
            unsigned int b = s[0], g = s[1], r = s[2];
            unsigned int a = 0xFF;
            if (r == 255 && g == 0 && b == 255) a = 0;   /* magenta color-key */
            d[x] = (a<<24)|(r<<16)|(g<<8)|b;
        }
    }
    free(f);
    *ow = w; *oh = h;
    return px;
}

const unsigned int *ov_load_bg(const char *path, int *w, int *h) {
    return cbmp_load(path, w, h);
}
