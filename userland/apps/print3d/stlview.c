// stlview.c - STL loader + TinyGL 3D preview for the 3D Print app (#396).
//
// Loads a binary or ASCII STL into a flat triangle array, auto-centres and
// scales it to fit the view, then renders it lit on a print bed into an ARGB
// buffer. Follows the proven chess renderer pattern (ZB_open(ZB_MODE_RGBA) +
// glInit + lit GL_TRIANGLES + ZB_copyFrameBuffer, then force alpha to 0xFF).
// Z is treated as up (the 3D-printing convention: the bed is the XY plane).
#include "../../libc/maytera.h"
#include "../../libc/fcntl.h"
#include "stlview.h"
#include "m3d_gcode.h"     // m3d_atod(): the tree's number parser (libc has no strtof)
#include <GL/gl.h>
#include <zbuffer.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Immediate-mode entrypoints (real functions live in libgl/src/api.c and are
// not declared in <GL/gl.h>, so declare them here exactly as chess does).
void glVertex3f(float, float, float);
void glNormal3f(float, float, float);
void glColor3f(float, float, float);

#define STL_PI 3.14159265f
#define STL_MAX_TRIS 200000   // cap huge meshes; load what fits

typedef struct { float nx, ny, nz; float v[3][3]; } stl_tri_t;

// ---- model store ----
static stl_tri_t *g_tris;
static int g_ntris, g_cap;
static float g_cx, g_cy, g_cz;   // model bbox centre
static float g_scale;            // uniform fit scale (model -> ~[-0.9,0.9])
static float g_bedz;             // bed plane in scaled/centred space

// ---- cached GL framebuffer ----
static ZBuffer *g_zb;
static int g_zw, g_zh;

// ---------------- small Mat4 (column-major, GL layout) ----------------
typedef struct { float m[16]; } Mat4;
static Mat4 mat_identity(void) { Mat4 m; for (int i=0;i<16;i++) m.m[i]=0; m.m[0]=m.m[5]=m.m[10]=m.m[15]=1; return m; }
static Mat4 mat_frustum(float l,float rt,float b,float t,float n,float f){
    Mat4 m; for(int i=0;i<16;i++) m.m[i]=0;
    m.m[0]=2*n/(rt-l); m.m[5]=2*n/(t-b); m.m[8]=(rt+l)/(rt-l); m.m[9]=(t+b)/(t-b);
    m.m[10]=-(f+n)/(f-n); m.m[11]=-1; m.m[14]=-2*f*n/(f-n); return m;
}
static void v3norm(float*x,float*y,float*z){ float l=sqrtf(*x**x+*y**y+*z**z); if(l>1e-6f){*x/=l;*y/=l;*z/=l;} }
static Mat4 mat_lookat(float ex,float ey,float ez,float cx,float cy,float cz,float ux,float uy,float uz){
    float fx=cx-ex,fy=cy-ey,fz=cz-ez; v3norm(&fx,&fy,&fz);
    float sx=fy*uz-fz*uy, sy=fz*ux-fx*uz, sz=fx*uy-fy*ux; v3norm(&sx,&sy,&sz);
    float ux2=sy*fz-sz*fy, uy2=sz*fx-sx*fz, uz2=sx*fy-sy*fx;
    Mat4 m=mat_identity();
    m.m[0]=sx; m.m[4]=sy; m.m[8]=sz;
    m.m[1]=ux2; m.m[5]=uy2; m.m[9]=uz2;
    m.m[2]=-fx; m.m[6]=-fy; m.m[10]=-fz;
    m.m[12]=-(sx*ex+sy*ey+sz*ez);
    m.m[13]=-(ux2*ex+uy2*ey+uz2*ez);
    m.m[14]=(fx*ex+fy*ey+fz*ez);
    return m;
}

// ---------------- loader ----------------
static float rd_f(const unsigned char *p){ float f; memcpy(&f,p,4); return f; }
static unsigned int rd_u32(const unsigned char *p){ unsigned int v; memcpy(&v,p,4); return v; }

static void ensure_cap(int need){
    if (need <= g_cap) return;
    int nc = g_cap ? g_cap*2 : 4096;
    while (nc < need) nc *= 2;
    if (nc > STL_MAX_TRIS) nc = STL_MAX_TRIS;
    stl_tri_t *nt = realloc(g_tris, (size_t)nc * sizeof(stl_tri_t));
    if (!nt) return;
    g_tris = nt; g_cap = nc;
}

static void add_tri(const float nrm[3], float v[3][3]){
    ensure_cap(g_ntris + 1);
    if (g_ntris >= g_cap) return;   // hit the cap or allocation failed
    stl_tri_t *t = &g_tris[g_ntris++];
    float nx=nrm[0], ny=nrm[1], nz=nrm[2];
    if (nx*nx+ny*ny+nz*nz < 1e-12f) {   // degenerate STL normal -> derive it
        float ax=v[1][0]-v[0][0], ay=v[1][1]-v[0][1], az=v[1][2]-v[0][2];
        float bx=v[2][0]-v[0][0], by=v[2][1]-v[0][1], bz=v[2][2]-v[0][2];
        nx=ay*bz-az*by; ny=az*bx-ax*bz; nz=ax*by-ay*bx;
    }
    float l=sqrtf(nx*nx+ny*ny+nz*nz); if (l>1e-9f){ nx/=l; ny/=l; nz/=l; }
    t->nx=nx; t->ny=ny; t->nz=nz;
    memcpy(t->v, v, sizeof t->v);
}

// Advance past whitespace, parse one float from the next whitespace-delimited
// token, and leave p just after that token. m3d_atod stops at the trailing
// whitespace, so tokenising by whitespace is enough (no endptr needed).
static const char *read_flt(const char *p, const char *end, float *out){
    while (p<end && (unsigned char)*p<=' ') p++;
    *out = (float)m3d_atod(p);
    while (p<end && (unsigned char)*p>' ') p++;   // skip the consumed token
    return p;
}

static void parse_ascii(const char *s, const char *end){
    const char *p = s;
    float nrm[3] = {0,0,0};
    float verts[3][3]; int vi = 0;
    while (p < end) {
        while (p<end && (unsigned char)*p<=' ') p++;
        if (p>=end) break;
        const char *tk = p;
        while (p<end && (unsigned char)*p>' ') p++;
        int tl = (int)(p - tk);
        if (tl==6 && !memcmp(tk,"normal",6)) {
            for (int k=0;k<3;k++) p = read_flt(p,end,&nrm[k]);
        } else if (tl==6 && !memcmp(tk,"vertex",6)) {
            if (vi<3) { for (int k=0;k<3;k++) p = read_flt(p,end,&verts[vi][k]); vi++; }
        } else if (tl==8 && !memcmp(tk,"endfacet",8)) {
            if (vi==3) add_tri(nrm, verts);
            vi = 0;
        }
    }
}

static void compute_bounds(void){
    if (g_ntris <= 0) { g_cx=g_cy=g_cz=0; g_scale=1; g_bedz=0; return; }
    float minx=1e30f,miny=1e30f,minz=1e30f, maxx=-1e30f,maxy=-1e30f,maxz=-1e30f;
    for (int i=0;i<g_ntris;i++) for (int k=0;k<3;k++) {
        float x=g_tris[i].v[k][0], y=g_tris[i].v[k][1], z=g_tris[i].v[k][2];
        if (x<minx)minx=x; if (x>maxx)maxx=x;
        if (y<miny)miny=y; if (y>maxy)maxy=y;
        if (z<minz)minz=z; if (z>maxz)maxz=z;
    }
    g_cx=(minx+maxx)*0.5f; g_cy=(miny+maxy)*0.5f; g_cz=(minz+maxz)*0.5f;
    float dx=maxx-minx, dy=maxy-miny, dz=maxz-minz;
    float md=dx; if (dy>md)md=dy; if (dz>md)md=dz; if (md<1e-6f)md=1;
    g_scale = 1.8f / md;
    g_bedz = -(dz*0.5f)*g_scale;   // bottom of the model sits on the bed
}

int stl_load(const char *path){
    stl_free();
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long n=0; unsigned char *buf=0; char tmp[8192];
    for (;;) {
        long r = sys_read(fd, tmp, sizeof tmp);
        if (r <= 0) break;
        unsigned char *nb = realloc(buf, n + r + 1);
        if (!nb) { free(buf); sys_close(fd); return -1; }
        buf = nb; memcpy(buf+n, tmp, r); n += r;
        if (n > 32*1024*1024) break;   // sanity cap
    }
    sys_close(fd);
    if (!buf || n < 84) { free(buf); return -1; }
    buf[n] = 0;   // let strtof stop safely at the end for the ASCII path

    unsigned int nt = rd_u32(buf + 80);
    int is_bin = (84 + (long)nt*50 == n);   // exact binary size match
    if (is_bin) {
        const unsigned char *bp = buf + 84;
        for (unsigned int i=0; i<nt && i<STL_MAX_TRIS; i++) {
            if (bp + 50 > buf + n) break;
            float nrm[3] = { rd_f(bp), rd_f(bp+4), rd_f(bp+8) };
            float v[3][3];
            for (int k=0;k<3;k++) {
                v[k][0]=rd_f(bp+12+k*12); v[k][1]=rd_f(bp+16+k*12); v[k][2]=rd_f(bp+20+k*12);
            }
            add_tri(nrm, v);
            bp += 50;
        }
    } else {
        parse_ascii((const char *)buf, (const char *)buf + n);
    }
    free(buf);
    compute_bounds();
    return g_ntris;
}

int stl_count(void){ return g_ntris; }

void stl_free(void){
    if (g_tris) { free(g_tris); g_tris = 0; }
    g_ntris = 0; g_cap = 0;
    if (g_zb) { glClose(); ZB_close(g_zb); g_zb = 0; g_zw = g_zh = 0; }
}

// ---------------- GL ----------------
static void ensure_zb(int w, int h){
    if (w<16)w=16; if (h<16)h=16;
    if (g_zb && g_zw==w && g_zh==h) return;
    if (g_zb) { glClose(); ZB_close(g_zb); g_zb = 0; }
    g_zb = ZB_open(w, h, ZB_MODE_RGBA, 0);
    g_zw = w; g_zh = h;
    if (!g_zb) return;
    glInit(g_zb);
    glShadeModel(GL_SMOOTH);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE);
    GLfloat amb[4] = {0.32f,0.32f,0.36f,1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    GLfloat ld[4] = {1.0f,0.98f,0.94f,1}, la[4] = {0.22f,0.22f,0.24f,1};
    glLightfv(GL_LIGHT0, GL_DIFFUSE, ld);
    glLightfv(GL_LIGHT0, GL_AMBIENT, la);
    glEnable(GL_LIGHT0);
}

// A lit print bed (XY plane) with a light grid, so the model reads as sitting
// on a printer. Drawn on the lit + colour-material path (the reliably-visible
// path in this TinyGL build; see the chess renderer notes).
static void draw_bed(void){
    glEnable(GL_LIGHTING); glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    float b = 1.15f, z = g_bedz - 0.002f;
    glColor3f(0.19f,0.21f,0.26f);
    glBegin(GL_QUADS); glNormal3f(0,0,1);
    glVertex3f(-b,-b,z); glVertex3f(b,-b,z); glVertex3f(b,b,z); glVertex3f(-b,b,z);
    glEnd();
    glColor3f(0.33f,0.37f,0.45f);
    float gz = g_bedz + 0.0005f, t = 0.006f;
    for (int i=-4;i<=4;i++) {
        float c = i*(b/4.0f);
        glBegin(GL_QUADS); glNormal3f(0,0,1);
        glVertex3f(-b,c-t,gz); glVertex3f(b,c-t,gz); glVertex3f(b,c+t,gz); glVertex3f(-b,c+t,gz);
        glEnd();
        glBegin(GL_QUADS); glNormal3f(0,0,1);
        glVertex3f(c-t,-b,gz); glVertex3f(c+t,-b,gz); glVertex3f(c+t,b,gz); glVertex3f(c-t,b,gz);
        glEnd();
    }
}

static void draw_model(void){
    if (g_ntris <= 0) return;
    glEnable(GL_LIGHTING); glEnable(GL_COLOR_MATERIAL);
    glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
    glColor3f(0.58f,0.64f,0.74f);   // neutral slicer grey-blue
    glBegin(GL_TRIANGLES);
    for (int i=0;i<g_ntris;i++) {
        stl_tri_t *t = &g_tris[i];
        glNormal3f(t->nx, t->ny, t->nz);
        for (int k=0;k<3;k++) {
            float x=(t->v[k][0]-g_cx)*g_scale;
            float y=(t->v[k][1]-g_cy)*g_scale;
            float z=(t->v[k][2]-g_cz)*g_scale;
            glVertex3f(x,y,z);
        }
    }
    glEnd();
}

void stl_render(unsigned int *dst, int w, int h, float yaw){
    if (!dst || w<=0 || h<=0) return;
    ensure_zb(w, h);
    if (!g_zb) {   // GL unavailable: fill with the panel background
        for (long i=0;i<(long)w*h;i++) dst[i] = 0xFF1C1F26u;
        return;
    }
    glViewport(0,0,w,h);
    glClearColor(0.11f,0.12f,0.15f,0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float aspect = (float)w/(float)h, nh = 0.5f;
    Mat4 proj = mat_frustum(-nh*aspect, nh*aspect, -nh, nh, 1.0f, 60.0f);
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(proj.m);

    float el = 27.0f*STL_PI/180.0f, ya = yaw*STL_PI/180.0f, R = 3.4f;
    float ex = R*cosf(ya)*cosf(el), ey = R*sinf(ya)*cosf(el), ez = R*sinf(el);
    Mat4 view = mat_lookat(ex,ey,ez, 0,0,0.05f, 0,0,1);   // Z up
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(view.m);

    GLfloat lp[4] = {2.5f, 2.0f, 6.0f, 1.0f}; glLightfv(GL_LIGHT0, GL_POSITION, lp);

    draw_bed();
    draw_model();

    ZB_copyFrameBuffer(g_zb, dst, w*(int)sizeof(unsigned int));
    for (int y=0;y<h;y++) { unsigned int *row = dst + (long)y*w; for (int x=0;x<w;x++) row[x] |= 0xFF000000u; }
}
