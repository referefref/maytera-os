/* overlay.c - Maytera Chess 2D overlay drawing into the ARGB frame buffer:
 * text (public-domain 8x8 font), filled/outlined/alpha rects. Used for the
 * main menu, HUD, move list, promotion picker, and banners on top of the GL
 * scene. */
#include "gfx.h"
typedef signed char GLbyte;   /* font8x8.h declares its table as GLbyte[256][8] */
#include "font8x8.h"

static inline void px_blend(unsigned int *p, unsigned int col, int a){
    if (a >= 255){ *p = 0xFF000000u | col; return; }
    if (a <= 0) return;
    unsigned int d = *p;
    int dr=(d>>16)&0xFF, dg=(d>>8)&0xFF, db=d&0xFF;
    int sr=(col>>16)&0xFF, sg=(col>>8)&0xFF, sb=col&0xFF;
    int rr=(sr*a+dr*(255-a))/255, rg=(sg*a+dg*(255-a))/255, rb=(sb*a+db*(255-a))/255;
    *p = 0xFF000000u | (rr<<16)|(rg<<8)|rb;
}

void ov_rect(unsigned int *buf, int bw, int bh, int x, int y, int w, int h, unsigned int col){
    for (int j=y;j<y+h;j++){ if(j<0||j>=bh)continue; unsigned int *row=buf+(long)j*bw;
        for(int i=x;i<x+w;i++){ if(i<0||i>=bw)continue; row[i]=0xFF000000u|col; } }
}
void ov_rect_a(unsigned int *buf, int bw, int bh, int x, int y, int w, int h, unsigned int col, int alpha){
    for (int j=y;j<y+h;j++){ if(j<0||j>=bh)continue; unsigned int *row=buf+(long)j*bw;
        for(int i=x;i<x+w;i++){ if(i<0||i>=bw)continue; px_blend(&row[i],col,alpha); } }
}
void ov_frame(unsigned int *buf, int bw, int bh, int x, int y, int w, int h, unsigned int col){
    ov_rect(buf,bw,bh,x,y,w,1,col); ov_rect(buf,bw,bh,x,y+h-1,w,1,col);
    ov_rect(buf,bw,bh,x,y,1,h,col); ov_rect(buf,bw,bh,x+w-1,y,1,h,col);
}

static void glyph(unsigned int *buf, int bw, int bh, int x, int y, char c, unsigned int col, int scale){
    unsigned char uc=(unsigned char)c; if(uc>=128)uc='?';
    const signed char *g=font8x8_basic[uc];
    for(int gy=0;gy<8;gy++){ int bits=(unsigned char)g[gy];
        for(int gx=0;gx<8;gx++){ if(!(bits&(1<<gx)))continue;
            for(int sy=0;sy<scale;sy++)for(int sx=0;sx<scale;sx++){
                int px=x+gx*scale+sx, py=y+gy*scale+sy;
                if(px<0||px>=bw||py<0||py>=bh)continue;
                buf[(long)py*bw+px]=0xFF000000u|col;
            } } }
}

int ov_text_w(const char *s, int scale){ int n=0; while(*s++)n++; return n*8*scale; }

void ov_text(unsigned int *buf, int bw, int bh, int x, int y, const char *s, unsigned int col, int scale){
    int cx=x;
    for(; *s; s++){ if(*s=='\n'){ y+=8*scale+2; cx=x; continue; } glyph(buf,bw,bh,cx,y,*s,col,scale); cx+=8*scale; }
}
void ov_text_c(unsigned int *buf, int bw, int bh, int cx, int y, const char *s, unsigned int col, int scale){
    ov_text(buf,bw,bh,cx-ov_text_w(s,scale)/2,y,s,col,scale);
}
