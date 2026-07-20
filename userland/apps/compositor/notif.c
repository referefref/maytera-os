// notif.c - Notifications subsystem (#168) for the MayteraOS compositor shell.
//
// Polls the spool file /CONFIG/NOTIFY.TXT (records "S|title|body\n" posted by any
// app via libc notify_post()), shows themed TTF toast popups (top-right, slide
// in, auto-dismiss, stack, click to dismiss), keeps a history for the tray-bell
// notification center, and honors the Settings "Alerts" prefs in
// /CONFIG/ALERTS.CFG (master enable, per-severity toggles, toast duration,
// do-not-disturb). No kernel changes: pure userland file-spool + framebuffer.
#include "compositor.h"
#include "../../libc/syscall.h"

extern int g_draw_blend;   // draw.c global alpha (0-255) for translucent overlays

#define SPOOL       "/CONFIG/NOTIFY.TXT"    /* fixed-file notification spool */
#define ALERTS_CFG  "/CONFIG/ALERTS.CFG"

#define NTF_INFO     0
#define NTF_SUCCESS  1
#define NTF_WARNING  2
#define NTF_ERROR    3

#define NTITLE 48
#define NBODY  120

typedef struct {
    int      used;
    int      severity;
    char     title[NTITLE];
    char     body[NBODY];
    uint64_t born_ms;
    int      rx, ry, rw, rh;   // last drawn rect (for hit-testing)
} toast_t;
#define MAX_TOASTS 4
static toast_t g_toasts[MAX_TOASTS];

typedef struct { int severity; char title[NTITLE]; char body[NBODY]; } hist_t;
#define MAX_HIST 40
static hist_t g_hist[MAX_HIST];
static int    g_hist_n = 0;
static int    g_unread = 0;
static int    g_center_open = 0;

// Prefs (defaults: everything on, 4s toasts, DND off).
static int p_enabled = 1;
static int p_sev[4]  = { 1, 1, 1, 1 };
static int p_duration = 4;
static int p_dnd = 0;

// ---- tiny helpers ---------------------------------------------------------
static void ncpy(char *d, const char *s, int max) {
    int i = 0; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0;
}
static uint32_t sev_color(int s) {
    switch (s) {
        case NTF_SUCCESS: return 0xFF2DA44E;
        case NTF_WARNING: return 0xFFD29922;
        case NTF_ERROR:   return 0xFFCF3B3B;
        default:          return 0xFF3B82F6;   // info
    }
}
static const char *sev_icon(int s) {
    switch (s) {
        case NTF_SUCCESS: return "CCHECK";
        case NTF_WARNING: return "WARN";
        case NTF_ERROR:   return "CIRCX";
        default:          return "INFO";
    }
}
static const char *sev_word(int s) {
    switch (s) {
        case NTF_SUCCESS: return "Success";
        case NTF_WARNING: return "Warning";
        case NTF_ERROR:   return "Error";
        default:          return "Info";
    }
}

// ---- minimal MICO icon loader (ported from the Settings app) --------------
#define MICO_DIM 64
#define MICO_CACHE 6
typedef struct { char name[16]; int w, h, loaded; unsigned char px[MICO_DIM*MICO_DIM*4]; } mico_t;
static mico_t g_mico[MICO_CACHE];
static int g_mico_n = 0;
static int mstreq(const char *a, const char *b){ int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return 0; i++; } return a[i]==b[i]; }
static mico_t *mico_get(const char *name) {
    for (int i=0;i<g_mico_n;i++) if (mstreq(g_mico[i].name,name)) return &g_mico[i];
    if (g_mico_n>=MICO_CACHE) return 0;
    mico_t *ic=&g_mico[g_mico_n++];
    int n=0; while(name[n]&&n<15){ic->name[n]=name[n];n++;} ic->name[n]=0;
    ic->loaded=-1; ic->w=ic->h=0;
    char path[48]; int l=0; const char*p="/ICONS/"; while(*p)path[l++]=*p++;
    for(int i=0;name[i]&&l<40;i++)path[l++]=name[i];
    const char*e=".ICN"; while(*e)path[l++]=*e++; path[l]=0;
    int fd=sys_open(path,0); if(fd<0) return ic;
    unsigned char hdr[12];
    if (sys_read(fd,(char*)hdr,12)!=12 || hdr[0]!='M'||hdr[1]!='I'||hdr[2]!='C'||hdr[3]!='O'){ sys_close(fd); return ic; }
    int w=hdr[4]|(hdr[5]<<8)|(hdr[6]<<16)|(hdr[7]<<24);
    int h=hdr[8]|(hdr[9]<<8)|(hdr[10]<<16)|(hdr[11]<<24);
    if(w<=0||h<=0||w>MICO_DIM||h>MICO_DIM){ sys_close(fd); return ic; }
    int want=w*h*4, got=0;
    while(got<want){ long r=sys_read(fd,(char*)ic->px+got,want-got); if(r<=0) break; got+=(int)r; }
    sys_close(fd);
    if(got!=want) return ic;
    ic->w=w; ic->h=h; ic->loaded=1; return ic;
}
// Blit cached icon scaled to size x size at (x,y), recolored to tint, blended
// against bg. Returns 1 if drawn, 0 if missing.
static int mico_blit(const char *name,int x,int y,int size,uint32_t tint,uint32_t bg){
    mico_t *ic=mico_get(name);
    if(!ic||ic->loaded!=1||size<=0) return 0;
    int tr=(tint>>16)&0xFF,tg=(tint>>8)&0xFF,tb=tint&0xFF;
    int br=(bg>>16)&0xFF,bgc=(bg>>8)&0xFF,bb=bg&0xFF;
    for(int dy=0;dy<size;dy++){
        int sy=(dy*ic->h)/size; if(sy>=ic->h)sy=ic->h-1;
        for(int dx=0;dx<size;dx++){
            int sx=(dx*ic->w)/size; if(sx>=ic->w)sx=ic->w-1;
            const unsigned char*s=&ic->px[(sy*ic->w+sx)*4];
            int b=s[0],g=s[1],r=s[2],a=s[3];
            if(a==0) continue;
            int cov=(r*30+g*59+b*11)/100; a=(a*cov)/255; if(a==0) continue;
            int rr=(tr*a+br*(255-a))/255, rg=(tg*a+bgc*(255-a))/255, rb=(tb*a+bb*(255-a))/255;
            draw_putpixel(x+dx,y+dy,(uint32_t)0xFF000000|(rr<<16)|(rg<<8)|rb);
        }
    }
    return 1;
}

// ---- prefs ----------------------------------------------------------------
static int kv_int(const char *buf,const char *key,int def){
    int kl=0; while(key[kl])kl++;
    for(const char*p=buf;*p;){
        const char*ls=p; int i=0; while(key[i]&&ls[i]==key[i])i++;
        if(i==kl&&ls[kl]=='='){
            const char*v=ls+kl+1; int neg=0,val=0,any=0;
            if(*v=='-'){neg=1;v++;}
            while(*v>='0'&&*v<='9'){val=val*10+(*v-'0');v++;any=1;}
            return any?(neg?-val:val):def;
        }
        while(*p&&*p!='\n')p++; if(*p=='\n')p++;
    }
    return def;
}
static void load_prefs(void){
    int fd=sys_open(ALERTS_CFG,0); if(fd<0) return;
    char buf[512]; long n=sys_read(fd,buf,sizeof(buf)-1); sys_close(fd);
    if(n<=0) return; buf[n]=0;
    p_enabled  = kv_int(buf,"enabled",1);
    p_sev[0]   = kv_int(buf,"sev_info",1);
    p_sev[1]   = kv_int(buf,"sev_success",1);
    p_sev[2]   = kv_int(buf,"sev_warning",1);
    p_sev[3]   = kv_int(buf,"sev_error",1);
    p_duration = kv_int(buf,"duration",4);
    if(p_duration<1)p_duration=1; if(p_duration>20)p_duration=20;
    p_dnd      = kv_int(buf,"dnd",0);
}

// ---- core: log + toast ----------------------------------------------------
static void push_notification(int sev,const char*title,const char*body){
    if(sev<0||sev>3)sev=0;
    if(!p_enabled||!p_sev[sev]) return;        // severity fully muted
    // history (cap; drop oldest)
    if(g_hist_n>=MAX_HIST){ for(int i=1;i<MAX_HIST;i++) g_hist[i-1]=g_hist[i]; g_hist_n=MAX_HIST-1; }
    hist_t*h=&g_hist[g_hist_n++]; h->severity=sev; ncpy(h->title,title,NTITLE); ncpy(h->body,body,NBODY);
    g_unread++;
    if(p_dnd) return;                          // do-not-disturb: logged, no toast
    int slot=-1; uint64_t oldest=(uint64_t)-1; int oi=0;
    for(int i=0;i<MAX_TOASTS;i++){ if(!g_toasts[i].used){slot=i;break;} if(g_toasts[i].born_ms<oldest){oldest=g_toasts[i].born_ms;oi=i;} }
    if(slot<0)slot=oi;
    toast_t*t=&g_toasts[slot]; t->used=1; t->severity=sev; t->born_ms=uptime_ms();
    ncpy(t->title,title,NTITLE); ncpy(t->body,body,NBODY);
}

// Consume the fixed spool: read the whole file, process complete "\n"-terminated
// records past the consumed byte offset. Producers do read-modify-write so the
// already-consumed prefix stays byte-identical; partial trailing records are
// left for the next poll. notif_init() clears the spool at session start so old
// records never replay across reboots.
static long g_off = 0;
void notif_init(void) {
    sys_unlink(SPOOL);     // fresh spool each session; old records already shown
    g_off = 0;
}
static void poll_spool(void) {
    int fd = sys_open(SPOOL, 0);
    if (fd < 0) return;
    static char buf[8200];
    long n = sys_read(fd, buf, sizeof(buf) - 1);
    sys_close(fd);
    if (n < 0) n = 0;
    if (n < g_off) g_off = 0;          // file shrank/reset: re-sync
    if (n <= g_off) return;
    buf[(int)n] = 0;
    int i = (int)g_off, added = 0;
    while (i < n) {
        int j = i; while (j < n && buf[j] != '\n') j++;
        if (j >= n) break;             // partial trailing record: wait
        buf[j] = 0;
        const char *l = &buf[i];
        if (l[0] >= '0' && l[0] <= '3' && l[1] == '|') {
            int sev = l[0] - '0';
            char title[NTITLE], body[NBODY];
            const char *t = l + 2; int k = 0;
            while (*t && *t != '|' && k < NTITLE-1) title[k++] = *t++;
            title[k] = 0;
            if (*t == '|') t++;
            k = 0;
            while (*t && k < NBODY-1) body[k++] = *t++;
            body[k] = 0;
            push_notification(sev, title, body);
            added = 1;
        }
        i = j + 1; g_off = i;
    }
    if (added) g_needs_redraw = true;
}

void notif_tick(void){
    unsigned long now=(unsigned long)sys_clock();
    static unsigned long last=0, plast=0;
    if(now-plast>250){ load_prefs(); plast=now; }     // reload prefs ~1s
    if(now-last>=12){ last=now; poll_spool(); }        // poll spool ~50ms
    // expire toasts; keep redrawing while any are visible (smooth slide/fade)
    uint64_t t=uptime_ms(); uint64_t life=(uint64_t)p_duration*1000;
    int active=0;
    for(int i=0;i<MAX_TOASTS;i++) if(g_toasts[i].used){
        if(t-g_toasts[i].born_ms>life){ g_toasts[i].used=0; g_needs_redraw=true; }
        else active=1;
    }
    if(active||g_center_open) g_needs_redraw=true;
}

// ---- rendering ------------------------------------------------------------
#define TOAST_W 320
#define TOAST_H 64
#define TOAST_MARGIN 12
#define TOAST_GAP 8
static void draw_toast(toast_t*t){
    int x=t->rx, y=t->ry, w=t->rw, h=t->rh;
    uint32_t acc=sev_color(t->severity);
    uint32_t ink=readable_ink(CLR_MENU_BG), dim=readable_ink_dim(CLR_MENU_BG);
    int ob=g_draw_blend; g_draw_blend=70; draw_rounded_rect(x+3,y+4,w,h,9,0xFF000000); g_draw_blend=ob;
    draw_rounded_rect(x,y,w,h,9,CLR_MENU_BG);
    draw_rect_outline(x,y,w,h,CLR_MENU_BORDER);
    draw_fill_rect(x,y,4,h,acc);                          // severity color bar
    if(!mico_blit(sev_icon(t->severity),x+14,y+(h-26)/2,26,acc,CLR_MENU_BG))
        draw_circle_filled(x+27,y+h/2,11,acc);
    draw_text_ttf(x+50,y+9,t->title[0]?t->title:sev_word(t->severity),15,ink);
    draw_text_ttf(x+50,y+33,t->body,13,dim);
    draw_text_ttf(x+w-15,y+5,"x",13,dim);
}
void notif_render(void){
    int shown=0; uint64_t now=uptime_ms();
    for(int i=0;i<MAX_TOASTS;i++){
        if(!g_toasts[i].used) continue;
        toast_t*t=&g_toasts[i];
        int tx=g_fb_width-TOAST_W-TOAST_MARGIN;
        int ty=TOAST_MARGIN+shown*(TOAST_H+TOAST_GAP);
        uint64_t age=now-t->born_ms;
        if(age<220){ int off=(int)((220-age)*(TOAST_W+TOAST_MARGIN)/220); tx+=off; }
        t->rx=tx; t->ry=ty; t->rw=TOAST_W; t->rh=TOAST_H;
        draw_toast(t);
        shown++;
    }
    if(g_center_open){
        int w=360, x=g_fb_width-w-6;
        int top=44, bot=g_fb_height-TASKBAR_HEIGHT-6;
        int h=bot-top; if(h<120)h=120;
        uint32_t ink=readable_ink(CLR_MENU_BG), dim=readable_ink_dim(CLR_MENU_BG);
        int ob=g_draw_blend; g_draw_blend=70; draw_rounded_rect(x+3,top+4,w,h,10,0xFF000000); g_draw_blend=ob;
        draw_rounded_rect(x,top,w,h,10,CLR_MENU_BG);
        draw_rect_outline(x,top,w,h,CLR_MENU_BORDER);
        draw_fill_rect(x+1,top+1,w-2,30,CLR_MENU_ITEM_HOVER);
        draw_text_ttf(x+12,top+8,"Notifications",16,ink);
        // Clear all button (top-right of header)
        int cbw=78, cbx=x+w-cbw-8, cby=top+4, cbh=22;
        draw_rounded_rect(cbx,cby,cbw,cbh,5,sev_color(NTF_INFO));
        draw_text_ttf(cbx+10,cby+4,"Clear all",12,0xFFFFFFFF);
        // history rows, newest first
        int ry=top+38; int rh=46;
        for(int k=g_hist_n-1;k>=0 && ry+rh<top+h-4;k--){
            hist_t*hh=&g_hist[k];
            uint32_t acc=sev_color(hh->severity);
            draw_fill_rect(x+8,ry,w-16,rh-6,CLR_MENU_ITEM_HOVER);
            draw_fill_rect(x+8,ry,3,rh-6,acc);
            if(!mico_blit(sev_icon(hh->severity),x+16,ry+(rh-6-20)/2,20,acc,CLR_MENU_ITEM_HOVER))
                draw_circle_filled(x+26,ry+(rh-6)/2,8,acc);
            draw_text_ttf(x+46,ry+4,hh->title[0]?hh->title:sev_word(hh->severity),13,ink);
            draw_text_ttf(x+46,ry+22,hh->body,12,dim);
            ry+=rh;
        }
        if(g_hist_n==0) draw_text_ttf(x+16,top+46,"No notifications",13,dim);
    }
}

// ---- input ----------------------------------------------------------------
// Returns 1 if the click was consumed.
int notif_handle_mouse(int x,int y,int clicked){
    if(!clicked) return 0;
    if(g_center_open){
        int w=360, cx=g_fb_width-w-6;
        int top=44, bot=g_fb_height-TASKBAR_HEIGHT-6; int h=bot-top; if(h<120)h=120;
        // Clear all
        int cbw=78, cbx=cx+w-cbw-8, cby=top+4, cbh=22;
        if(x>=cbx&&x<cbx+cbw&&y>=cby&&y<cby+cbh){ g_hist_n=0; g_unread=0; g_needs_redraw=true; return 1; }
        if(x>=cx&&x<cx+w&&y>=top&&y<top+h){ g_needs_redraw=true; return 1; } // swallow inside
        g_center_open=0; g_needs_redraw=true; return 1;                       // click outside closes
    }
    // toasts: click dismisses
    for(int i=0;i<MAX_TOASTS;i++){
        if(!g_toasts[i].used) continue;
        toast_t*t=&g_toasts[i];
        if(x>=t->rx&&x<t->rx+t->rw&&y>=t->ry&&y<t->ry+t->rh){
            t->used=0; g_needs_redraw=true; return 1;
        }
    }
    return 0;
}

// ---- tray bell API --------------------------------------------------------
int  notif_unread(void){ return g_unread; }
void notif_toggle_center(void){
    g_center_open=!g_center_open;
    if(g_center_open) g_unread=0;       // opening marks all read
    g_needs_redraw=true;
}
int  notif_center_open(void){ return g_center_open; }
