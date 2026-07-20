/* render.c - Maytera Chess 3D renderer (TinyGL). Draws a lit 3D board with
 * procedurally-modelled pieces (surfaces of revolution + detail solids), move
 * highlights, an optional textured backdrop, and provides screen->square
 * picking by projecting the 64 square centers with the same matrices GL uses. */
#include "gfx.h"
#include <GL/gl.h>
#include <zbuffer.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* immediate-mode entrypoints (real functions in libgl/src/api.c) */
void glVertex3f(float,float,float);
void glNormal3f(float,float,float);
void glColor3f(float,float,float);
void glColor4f(float,float,float,float);
void glTexCoord2f(float,float);

unsigned int *cbmp_load(const char *path, int *w, int *h);

#define TILE 1.0f

static ZBuffer *g_zb;
static int g_w, g_h;
static int g_inited;

/* asset textures (GL texture ids; 0 = not loaded) */
static GLuint g_tex_bg;
static int g_have_bg;

/* last projection matrices for picking (row math done in C) */
static Mat4 g_view, g_proj;

/* ---------------- Mat4 (column-major, GL layout) ---------------- */
static Mat4 mat_identity(void){Mat4 m; for(int i=0;i<16;i++)m.m[i]=0; m.m[0]=m.m[5]=m.m[10]=m.m[15]=1; return m;}
static Mat4 mat_mul(Mat4 a, Mat4 b){ Mat4 r; for(int c=0;c<4;c++)for(int rr=0;rr<4;rr++){float s=0;for(int k=0;k<4;k++)s+=a.m[k*4+rr]*b.m[c*4+k];r.m[c*4+rr]=s;}return r;}
static Mat4 mat_frustum(float l,float rt,float b,float t,float n,float f){Mat4 m;for(int i=0;i<16;i++)m.m[i]=0;
    m.m[0]=2*n/(rt-l); m.m[5]=2*n/(t-b); m.m[8]=(rt+l)/(rt-l); m.m[9]=(t+b)/(t-b);
    m.m[10]=-(f+n)/(f-n); m.m[11]=-1; m.m[14]=-2*f*n/(f-n); return m;}
static void v3norm(float*x,float*y,float*z){float l=sqrtf(*x**x+*y**y+*z**z); if(l>1e-6f){*x/=l;*y/=l;*z/=l;}}
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

/* board file/rank -> world x,z (y is up). flip swaps orientation. */
static void world_of(const App *a, int f, int r, float *wx, float *wz){
    float x = (f - 3.5f)*TILE, z = (3.5f - r)*TILE;
    if (a->flip){ x = -x; z = -z; }
    *wx = x; *wz = z;
}

/* ---------------- camera ---------------- */
static void camera_eye(const App *a, float *ex,float *ey,float *ez){
    float yaw = a->cam_yaw * 3.14159265f/180.0f;
    float pit = a->cam_pitch * 3.14159265f/180.0f;
    float R = 11.5f;
    *ex = R*sinf(yaw)*cosf(pit);
    *ey = R*sinf(pit);
    *ez = R*cosf(yaw)*cosf(pit);
}

/* ---------------- GL init ---------------- */
void gfx_init(int w, int h){
    if (w<64)w=64; if(h<64)h=64;
    if (g_inited) return;
    g_w=w; g_h=h;
    g_zb = ZB_open(w,h,ZB_MODE_RGBA,0);
    if (!g_zb) return;
    glInit(g_zb);
    g_inited=1;
    glShadeModel(GL_SMOOTH);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_NORMALIZE);
    GLfloat amb[4]={0.35f,0.35f,0.40f,1};
    glLightModelfv(GL_LIGHT_MODEL_AMBIENT, amb);
    GLfloat ld[4]={1.0f,0.98f,0.92f,1}; GLfloat la[4]={0.25f,0.25f,0.25f,1};
    glLightfv(GL_LIGHT0, GL_DIFFUSE, ld);
    glLightfv(GL_LIGHT0, GL_AMBIENT, la);
    glEnable(GL_LIGHT0);
}
void gfx_resize(int w,int h){
    if (w<64)w=64; if(h<64)h=64;
    if (g_inited){ glClose(); ZB_close(g_zb); g_inited=0; }
    gfx_init(w,h);
}

/* ---------------- textures ---------------- */
static GLuint upload_rgb(const unsigned int *px, int w, int h){
    long n=(long)w*h; unsigned char *rgb=(unsigned char*)malloc(n*3); if(!rgb)return 0;
    for(long i=0;i<n;i++){unsigned int p=px[i]; rgb[i*3]=(p>>16)&0xFF; rgb[i*3+1]=(p>>8)&0xFF; rgb[i*3+2]=p&0xFF;}
    GLuint t=0; glGenTextures(1,&t); glBindTexture(GL_TEXTURE_2D,(GLint)t);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D,0,3,w,h,0,GL_RGB,GL_UNSIGNED_BYTE,rgb);
    free(rgb); return t;
}
void gfx_load_assets(void){
    int w,h; unsigned int *px;
    /* The board checker is drawn as per-square coloured geometry (draw_board),
     * NOT as a single photographic board texture: a one-quad CHESSBRD.BMP was
     * unreadable on real hardware (texture heap fallback / lighting wash-out).
     * Only the ambient backdrop is textured here. */
    px = cbmp_load("/CHESS/BG.BMP",&w,&h);
    if (px){ g_tex_bg = upload_rgb(px,w,h); g_have_bg = (g_tex_bg != 0); free(px); }
}

/* ---------------- background ---------------- */
static void draw_backdrop(void){
    glDisable(GL_DEPTH_TEST); glDisable(GL_LIGHTING);
    Mat4 o=mat_identity(); /* ortho 0..1 */
    o.m[0]=2; o.m[12]=-1; o.m[5]=2; o.m[13]=-1; o.m[10]=-1;
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(o.m);
    glMatrixMode(GL_MODELVIEW); Mat4 id=mat_identity(); glLoadMatrixf(id.m);
    if (g_have_bg){
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D,(GLint)g_tex_bg); glColor3f(1,1,1);
        glBegin(GL_QUADS);
        glTexCoord2f(0,1); glVertex3f(0,0,0); glTexCoord2f(1,1); glVertex3f(1,0,0);
        glTexCoord2f(1,0); glVertex3f(1,1,0); glTexCoord2f(0,0); glVertex3f(0,1,0);
        glEnd(); glDisable(GL_TEXTURE_2D);
    } else {
        glBegin(GL_QUADS);
        glColor3f(0.10f,0.12f,0.16f); glVertex3f(0,0,0); glVertex3f(1,0,0);
        glColor3f(0.20f,0.24f,0.32f); glVertex3f(1,1,0); glVertex3f(0,1,0);
        glEnd();
    }
    glEnable(GL_DEPTH_TEST);
}

/* ---------------- board ---------------- */
static void draw_board(const App *a){
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_LIGHTING); glEnable(GL_COLOR_MATERIAL); glColorMaterial(GL_FRONT_AND_BACK,GL_AMBIENT_AND_DIFFUSE);
    /* base slab */
    glColor3f(0.20f,0.14f,0.09f);
    float B=4.4f, yb=-0.35f;
    glBegin(GL_QUADS);
    glNormal3f(0,1,0);
    glVertex3f(-B,0.0f,-B); glVertex3f(B,0.0f,-B); glVertex3f(B,0.0f,B); glVertex3f(-B,0.0f,B);
    glEnd();
    /* sides */
    glColor3f(0.14f,0.10f,0.06f);
    glBegin(GL_QUADS);
    glNormal3f(0,0,1);  glVertex3f(-B,yb,B); glVertex3f(B,yb,B); glVertex3f(B,0,B); glVertex3f(-B,0,B);
    glNormal3f(0,0,-1); glVertex3f(B,yb,-B); glVertex3f(-B,yb,-B); glVertex3f(-B,0,-B); glVertex3f(B,0,-B);
    glNormal3f(1,0,0);  glVertex3f(B,yb,B); glVertex3f(B,yb,-B); glVertex3f(B,0,-B); glVertex3f(B,0,B);
    glNormal3f(-1,0,0); glVertex3f(-B,yb,-B); glVertex3f(-B,yb,B); glVertex3f(-B,0,B); glVertex3f(-B,0,-B);
    glEnd();
    /* 64 squares as individual quads in ALTERNATING light/dark colours, drawn
     * exactly like the lit base slab (GL_LIGHTING + GL_COLOR_MATERIAL, which is
     * the rendering path proven to be visible), just one per board cell with a
     * per-square colour. They sit a clear step ABOVE the slab (y=0.05) so they
     * never lose a z-fight with the coplanar slab, which had left the checker
     * invisible before. Standard orientation: a1 (f=0,r=0) is dark, so each
     * side has a light square on its right. The 0.4 slab rim around the squares
     * remains as a subtle board frame. */
    glDisable(GL_TEXTURE_2D);
    for (int r=0;r<8;r++) for(int f=0;f<8;f++){
        int light=((f+r)&1);
        if (light) glColor3f(0.85f,0.75f,0.55f);   /* warm cream  -> ~ivory when lit */
        else       glColor3f(0.34f,0.22f,0.12f);   /* dark walnut -> deep brown when lit */
        float wx,wz; world_of(a,f,r,&wx,&wz);
        float h=TILE*0.5f; float y=0.05f;
        glBegin(GL_QUADS); glNormal3f(0,1,0);
        glVertex3f(wx-h,y,wz-h); glVertex3f(wx+h,y,wz-h);
        glVertex3f(wx+h,y,wz+h); glVertex3f(wx-h,y,wz+h);
        glEnd();
    }
}

/* translucent-looking highlight tile (emissive flat, depth-tested) */
/* Highlights are drawn on the LIT path (GL_LIGHTING + GL_COLOR_MATERIAL, both
 * already enabled here). The unlit flat-colour path does not rasterise in this
 * TinyGL build, so an unlit tile was invisible; a lit tile with a top normal
 * renders bright and reliably. They sit at y=0.10, above the y=0.05 squares. */
static void hl_tile(const App *a, int sq, float r,float g,float b){
    int f=sq&7, rk=sq>>3; float wx,wz; world_of(a,f,rk,&wx,&wz);
    glColor3f(r,g,b); float h=TILE*0.46f; float y=0.10f;
    glBegin(GL_QUADS); glNormal3f(0,1,0);
    glVertex3f(wx-h,y,wz-h); glVertex3f(wx+h,y,wz-h); glVertex3f(wx+h,y,wz+h); glVertex3f(wx-h,y,wz+h);
    glEnd();
}
static void hl_dot(const App *a, int sq){
    int f=sq&7, rk=sq>>3; float wx,wz; world_of(a,f,rk,&wx,&wz);
    glColor3f(0.20f,0.70f,0.35f); float y=0.11f; float rad=0.16f;
    glBegin(GL_QUADS); glNormal3f(0,1,0);
    glVertex3f(wx-rad,y,wz-rad); glVertex3f(wx+rad,y,wz-rad); glVertex3f(wx+rad,y,wz+rad); glVertex3f(wx-rad,y,wz+rad);
    glEnd();
}

/* ---------------- procedural pieces (surfaces of revolution) ---------------- */
#define SEG 18
static void lathe(const float prof[][2], int n){
    for (int i=0;i<n-1;i++){
        float r0=prof[i][0], y0=prof[i][1], r1=prof[i+1][0], y1=prof[i+1][1];
        float dr=r1-r0, dy=y1-y0; float nl=sqrtf(dr*dr+dy*dy); if(nl<1e-5f)nl=1;
        float nr=dy/nl, ny=-dr/nl;   /* profile normal in r,y plane */
        glBegin(GL_QUADS);
        for (int s=0;s<SEG;s++){
            float a0=(float)s/SEG*6.2831853f, a1=(float)(s+1)/SEG*6.2831853f;
            float c0=cosf(a0),si0=sinf(a0),c1=cosf(a1),si1=sinf(a1);
            glNormal3f(nr*c0,ny,nr*si0); glVertex3f(r0*c0,y0,r0*si0);
            glNormal3f(nr*c1,ny,nr*si1); glVertex3f(r0*c1,y0,r0*si1);
            glNormal3f(nr*c1,ny,nr*si1); glVertex3f(r1*c1,y1,r1*si1);
            glNormal3f(nr*c0,ny,nr*si0); glVertex3f(r1*c0,y1,r1*si0);
        }
        glEnd();
    }
}
static void disc_top(float r, float y){
    glBegin(GL_TRIANGLES);
    for (int s=0;s<SEG;s++){
        float a0=(float)s/SEG*6.2831853f, a1=(float)(s+1)/SEG*6.2831853f;
        glNormal3f(0,1,0);
        glVertex3f(0,y,0); glVertex3f(r*cosf(a0),y,r*sinf(a0)); glVertex3f(r*cosf(a1),y,r*sinf(a1));
    }
    glEnd();
}
static void small_box(float cx,float cy,float cz,float sx,float sy,float sz){
    float x0=cx-sx,x1=cx+sx,y0=cy-sy,y1=cy+sy,z0=cz-sz,z1=cz+sz;
    glBegin(GL_QUADS);
    glNormal3f(0,0,1);  glVertex3f(x0,y0,z1);glVertex3f(x1,y0,z1);glVertex3f(x1,y1,z1);glVertex3f(x0,y1,z1);
    glNormal3f(0,0,-1); glVertex3f(x1,y0,z0);glVertex3f(x0,y0,z0);glVertex3f(x0,y1,z0);glVertex3f(x1,y1,z0);
    glNormal3f(1,0,0);  glVertex3f(x1,y0,z1);glVertex3f(x1,y0,z0);glVertex3f(x1,y1,z0);glVertex3f(x1,y1,z1);
    glNormal3f(-1,0,0); glVertex3f(x0,y0,z0);glVertex3f(x0,y0,z1);glVertex3f(x0,y1,z1);glVertex3f(x0,y1,z0);
    glNormal3f(0,1,0);  glVertex3f(x0,y1,z1);glVertex3f(x1,y1,z1);glVertex3f(x1,y1,z0);glVertex3f(x0,y1,z0);
    glNormal3f(0,-1,0); glVertex3f(x0,y0,z0);glVertex3f(x1,y0,z0);glVertex3f(x1,y0,z1);glVertex3f(x0,y0,z1);
    glEnd();
}

/* profiles: {radius, y}. base at y=0. */
static void model_pawn(void){
    static const float p[][2]={{0.28f,0},{0.30f,0.05f},{0.20f,0.10f},{0.16f,0.22f},{0.13f,0.30f},
        {0.20f,0.36f},{0.14f,0.42f},{0.19f,0.50f},{0.0f,0.66f}};
    lathe(p,9);
}
static void model_rook(void){
    static const float p[][2]={{0.32f,0},{0.34f,0.06f},{0.22f,0.12f},{0.20f,0.55f},{0.28f,0.62f},{0.30f,0.74f},{0.30f,0.80f}};
    lathe(p,7); disc_top(0.30f,0.80f);
    for(int i=0;i<4;i++){float a=i*1.5708f; small_box(0.24f*cosf(a),0.86f,0.24f*sinf(a),0.07f,0.06f,0.07f);}
}
static void model_knight(void){
    static const float p[][2]={{0.32f,0},{0.34f,0.06f},{0.22f,0.12f},{0.20f,0.30f},{0.22f,0.40f}};
    lathe(p,5); disc_top(0.22f,0.40f);
    /* stylised horse head: angled body block + snout + ears */
    small_box(0.0f,0.58f,0.02f,0.13f,0.20f,0.15f);
    small_box(0.0f,0.66f,0.16f,0.10f,0.09f,0.12f);   /* snout forward (+z) */
    small_box(-0.05f,0.80f,-0.06f,0.03f,0.07f,0.03f);/* ear */
    small_box(0.05f,0.80f,-0.06f,0.03f,0.07f,0.03f); /* ear */
}
static void model_bishop(void){
    static const float p[][2]={{0.30f,0},{0.32f,0.06f},{0.20f,0.12f},{0.15f,0.40f},{0.22f,0.48f},{0.13f,0.56f},
        {0.15f,0.66f},{0.10f,0.74f},{0.0f,0.82f}};
    lathe(p,9);
    small_box(0.0f,0.86f,0.0f,0.04f,0.05f,0.04f);   /* mitre knob */
}
static void model_queen(void){
    static const float p[][2]={{0.34f,0},{0.36f,0.06f},{0.22f,0.12f},{0.17f,0.55f},{0.26f,0.66f},{0.20f,0.74f},{0.24f,0.86f}};
    lathe(p,7); disc_top(0.24f,0.86f);
    for(int i=0;i<8;i++){float a=i*0.7854f; small_box(0.20f*cosf(a),0.92f,0.20f*sinf(a),0.035f,0.05f,0.035f);}
    small_box(0,0.99f,0,0.05f,0.05f,0.05f);
}
static void model_king(void){
    static const float p[][2]={{0.35f,0},{0.37f,0.06f},{0.23f,0.12f},{0.18f,0.60f},{0.27f,0.72f},{0.21f,0.80f},{0.25f,0.92f}};
    lathe(p,7); disc_top(0.25f,0.92f);
    small_box(0,1.02f,0,0.05f,0.10f,0.05f);   /* cross vertical */
    small_box(0,1.06f,0,0.10f,0.04f,0.04f);   /* cross horizontal */
}
static void draw_piece_model(int type){
    switch(type){
        case PAWN: model_pawn(); break; case ROOK: model_rook(); break;
        case KNIGHT: model_knight(); break; case BISHOP: model_bishop(); break;
        case QUEEN: model_queen(); break; case KING: model_king(); break;
    }
}
static void set_piece_material(int color){
    if (color==WHITE){ glColor3f(0.90f,0.87f,0.78f); }
    else { glColor3f(0.16f,0.15f,0.18f); }
}

/* place & draw one piece at world x,z */
static void draw_piece_at(int piece, float wx, float wz){
    int type = piece<0?-piece:piece; int color = piece<0?BLACK:WHITE;
    glPushMatrix();
    glTranslatef(wx,0.0f,wz);
    /* knights face the opponent: rotate black knight 180 */
    if (type==KNIGHT && color==BLACK) glRotatef(180,0,1,0);
    set_piece_material(color);
    draw_piece_model(type);
    glPopMatrix();
}

/* ---------------- main render ---------------- */
void gfx_render(App *a, unsigned int *dst, int pitch){
    if (!g_inited) return;
    a->vw=g_w; a->vh=g_h;
    glViewport(0,0,g_w,g_h);
    glClearColor(0.08f,0.09f,0.12f,0);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);

    draw_backdrop();
    glClear(GL_DEPTH_BUFFER_BIT);   /* backdrop quad wrote depth; reset so the 3D scene isn't culled */

    /* perspective */
    float aspect=(float)g_w/(float)g_h, nh=0.5f;
    g_proj = mat_frustum(-nh*aspect,nh*aspect,-nh,nh,1.0f,60.0f);
    glMatrixMode(GL_PROJECTION); glLoadMatrixf(g_proj.m);
    float ex,ey,ez; camera_eye(a,&ex,&ey,&ez);
    g_view = mat_lookat(ex,ey,ez, 0,0,0, 0,1,0);
    glMatrixMode(GL_MODELVIEW); glLoadMatrixf(g_view.m);

    GLfloat lp[4]={4.0f,10.0f,6.0f,1.0f}; glLightfv(GL_LIGHT0,GL_POSITION,lp);

    draw_board(a);

    /* highlights */
    if (a->last_from>=0) hl_tile(a,a->last_from,0.55f,0.50f,0.20f);
    if (a->last_to>=0)   hl_tile(a,a->last_to,0.70f,0.62f,0.22f);
    if (a->sel>=0)       hl_tile(a,a->sel,0.20f,0.55f,0.30f);
    /* check indicator */
    if (a->screen==SC_PLAY || a->screen==SC_PROMOTE){
        int side=a->game.pos.side;
        if (chess_in_check(&a->game.pos,side)){
            signed char k=(side==WHITE)?KING:-KING;
            for(int i=0;i<64;i++) if(a->game.pos.board[i]==k) hl_tile(a,i,0.75f,0.18f,0.15f);
        }
    }
    if (a->sel>=0) for(int i=0;i<64;i++) if(a->legal_to[i]) hl_dot(a,i);
    /* keyboard cursor */
    if (a->screen==SC_PLAY && a->cursor>=0 && a->cursor<64) hl_tile(a,a->cursor,0.25f,0.65f,0.80f);

    /* pieces */
    glEnable(GL_LIGHTING); glEnable(GL_COLOR_MATERIAL);
    for (int sq=0; sq<64; sq++){
        signed char pc=a->game.pos.board[sq];
        if (!pc) continue;
        if (a->anim_active && sq==a->anim_move.to && a->anim_apply_pending) continue; /* moving piece drawn separately (already at dest in board?) */
        if (a->anim_active && sq==a->anim_move.from) continue;
        int f=sq&7, r=sq>>3; float wx,wz; world_of(a,f,r,&wx,&wz);
        draw_piece_at(pc,wx,wz);
    }
    /* animated piece */
    if (a->anim_active){
        float t=a->anim_t; if(t<0)t=0; if(t>1)t=1;
        float wx=a->anim_fx+(a->anim_tx-a->anim_fx)*t;
        float wz=a->anim_fz+(a->anim_tz-a->anim_fz)*t;
        float lift = sinf(t*3.14159f)*0.25f;
        glPushMatrix(); glTranslatef(0,lift,0);
        draw_piece_at(a->anim_piece,wx,wz);
        glPopMatrix();
    }

    ZB_copyFrameBuffer(g_zb, dst, pitch*(int)sizeof(unsigned int));
    for (int y=0;y<g_h;y++){ unsigned int *row=dst+(long)y*pitch; for(int x=0;x<g_w;x++) row[x]|=0xFF000000u; }
}

/* project a world point to screen pixel coords using the matrices from the last
 * gfx_render(); returns 1 if the point is in front of the camera. Used by the
 * 2D overlay to place rank/file coordinate labels along the board edges. */
int gfx_project(App *a, float wx, float wy, float wz, int *sx, int *sy){
    (void)a;
    if (!g_inited) return 0;
    Mat4 vp = mat_mul(g_proj, g_view);
    float X=vp.m[0]*wx+vp.m[4]*wy+vp.m[8]*wz+vp.m[12];
    float Y=vp.m[1]*wx+vp.m[5]*wy+vp.m[9]*wz+vp.m[13];
    float W=vp.m[3]*wx+vp.m[7]*wy+vp.m[11]*wz+vp.m[15];
    if (W<=0.0001f) return 0;
    float ndcx=X/W, ndcy=Y/W;
    if (sx) *sx=(int)((ndcx*0.5f+0.5f)*g_w);
    if (sy) *sy=(int)((1.0f-(ndcy*0.5f+0.5f))*g_h);
    return 1;
}

/* ---------------- picking ---------------- */
int gfx_pick_square(App *a, int mx, int my){
    if (!g_inited) return -1;
    Mat4 vp = mat_mul(g_proj, g_view);
    int best=-1; float bestd=1e9f;
    for (int sq=0; sq<64; sq++){
        int f=sq&7, r=sq>>3; float wx,wz; world_of(a,f,r,&wx,&wz);
        float X=vp.m[0]*wx+vp.m[4]*0+vp.m[8]*wz+vp.m[12];
        float Y=vp.m[1]*wx+vp.m[5]*0+vp.m[9]*wz+vp.m[13];
        float W=vp.m[3]*wx+vp.m[7]*0+vp.m[11]*wz+vp.m[15];
        if (W<=0.0001f) continue;
        float ndcx=X/W, ndcy=Y/W;
        float sx=(ndcx*0.5f+0.5f)*g_w;
        float sy=(1.0f-(ndcy*0.5f+0.5f))*g_h;
        float dx=sx-mx, dy=sy-my; float d=dx*dx+dy*dy;
        if (d<bestd){ bestd=d; best=sq; }
    }
    /* reject clicks well off the board (roughly one tile in screen space) */
    float tilepx = (float)g_h/12.0f;
    if (bestd > (tilepx*1.4f)*(tilepx*1.4f)) return -1;
    return best;
}
