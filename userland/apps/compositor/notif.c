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

/* #683: per-user spool, resolved through libc/userconf.c. The Ring-0 poster
   (kernel/security/seclog.c) and every app poster (libc/notify.c) resolve the
   same path, so there is one spool. */
#include "userconf.h"
#define SPOOL_NAME  "NOTIFY.TXT"
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
    int      rx, ry, rw, rh;   // last drawn rect (for hit-testing AND #585 damage tracking)
    int      settled;         // #585: 1 once the slide-in animation has finished
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
    // #585: rw=0 tells notif_collect_damage() this slot has no "last drawn"
    // rect yet, so its very first damage covers only the new position (no
    // stale union from whatever toast previously occupied this slot).
    t->rx=t->ry=t->rw=t->rh=0; t->settled=0;
}

// Consume the fixed spool: read the whole file, process complete "\n"-terminated
// records past the consumed byte offset. Producers do read-modify-write so the
// already-consumed prefix stays byte-identical; partial trailing records are
// left for the next poll. notif_init() clears the spool at session start so old
// records never replay across reboots.
static long g_off = 0;
void notif_init(void) {
    { char _sp[256];       // fresh spool each session; old records already shown
      if (userconf_path(SPOOL_NAME, _sp, sizeof(_sp)) == 0) sys_unlink(_sp); }
    g_off = 0;
}
static void poll_spool(void) {
    int fd = userconf_open_read(SPOOL_NAME, 0);   // #683: no legacy fallback, see notify.c
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

// #585: notif_tick() used to force a FULL-screen g_needs_redraw on every
// single tick for as long as ANY toast was on screen ("if(active||...)
// g_needs_redraw=true"), regardless of whether the toast was actually
// animating. On a weak/high-res target (real iMac, 1920x1080, ~10ms/frame
// software compositor) that made a long-lived toast (e.g. a persistent
// network-fault warning) pin the compositor to a full-screen composite every
// tick for as long as it stayed up - the dominant real-hardware lag cause.
// Fixed: a toast merely SITTING on screen, unchanged, now forces nothing at
// all (the idle gate, #564, stays engaged). Only genuine STATE CHANGES get a
// one-shot g_needs_redraw (new arrival in poll_spool() above, a toast
// finishing its slide-in here, or a toast expiring/vacating its rect here) -
// these are rare, one-time events, not a per-tick cost. While the toast is
// merely animating (still sliding) or static, notif_collect_damage() (called
// from main's idle path alongside widgets_collect_damage()/
// taskbar_collect_damage(), same #379 mechanism) marks damage for ONLY the
// toast's own rect, so the composite+present stays scoped to that small
// region instead of the whole screen.
#define TOAST_W 320
#define TOAST_H 64
#define TOAST_MARGIN 12
#define TOAST_GAP 8
#define TOAST_LINE_H 16
#define TOAST_MAX_LINES 3

// #762: word-wrap `body` into up to `max_lines` lines that each fit `max_w`
// pixels at TTF size `size`, measured with the REAL glyph metrics
// (text_width_ttf) - a proportional TTF face has no fixed char width, so
// guessing "N chars per line" is wrong for any string with wide glyphs (this
// is what let "N color pair(s) were too low-contrast..." run off the right
// edge of the toast). Breaks at the last space that still fits; a single
// word wider than max_w is hard-broken so it can never leave the box. Any
// text left over once max_lines is reached is dropped and the last line
// ellipsized (real-measured, not a fixed suffix count) so it still fits.
static int wrap_text_ttf(const char *body, int size, int max_w, int max_lines,
                         char out[][NBODY]) {
    int nlines = 0;
    const char *p = body;
    while (*p && nlines < max_lines) {
        int len = 0, last_space = -1;
        char probe[NBODY];
        while (p[len]) {
            if (p[len] == ' ' && len > 0) last_space = len;
            int pl = len + 1; if (pl > NBODY - 1) pl = NBODY - 1;
            memcpy(probe, p, pl); probe[pl] = 0;
            if (text_width_ttf(probe, size) > max_w) break;
            if (pl >= NBODY - 1) { len = pl; break; }
            len++;
        }
        int cut;
        if (!p[len]) {
            cut = len;                 // rest of the string fits on this line
        } else if (last_space > 0) {
            cut = last_space;          // break at the last word boundary that fit
        } else {
            cut = (len > 0) ? len : 1; // single word wider than max_w: hard-break it
        }
        int cl = cut; if (cl > NBODY - 1) cl = NBODY - 1;
        memcpy(out[nlines], p, cl); out[nlines][cl] = 0;
        nlines++;
        p += cut;
        while (*p == ' ') p++;
    }
    if (*p && nlines > 0) {
        // Text remains beyond max_lines: ellipsize the last line, measuring
        // the actual "..." glyph width rather than assuming a fixed suffix.
        char *last = out[nlines - 1];
        int ellw = text_width_ttf("...", size);
        int l = (int)strlen(last);
        while (l > 0 && text_width_ttf(last, size) + ellw > max_w) last[--l] = 0;
        if (l + 3 < NBODY) { last[l]='.'; last[l+1]='.'; last[l+2]='.'; last[l+3]=0; }
    }
    if (nlines < 1) nlines = 1, out[0][0] = 0;
    return nlines;
}

// Truncate a single line to fit max_w, appending a real-measured ellipsis.
// Used where growing the box is not an option (fixed-height history rows).
static void ellipsize_ttf(const char *src, int size, int max_w, char *out, int outcap) {
    int n = 0; while (src[n] && n < outcap - 1) { out[n] = src[n]; n++; } out[n] = 0;
    if (text_width_ttf(out, size) <= max_w) return;
    int ellw = text_width_ttf("...", size);
    while (n > 0 && text_width_ttf(out, size) + ellw > max_w) out[--n] = 0;
    if (n + 3 < outcap) { out[n]='.'; out[n+1]='.'; out[n+2]='.'; out[n+3]=0; }
}

// Available width for toast body text: box width minus the left icon/text
// inset (matches the x+50 draw origin in draw_toast()) and a right margin
// clear of the close "x" glyph.
#define TOAST_BODY_MAXW (TOAST_W - 50 - 16)

// Wrapped line count -> resulting box height for a toast's body. Called from
// notif_render()/notif_collect_damage()/draw_toast() so the three can never
// disagree about how tall a given toast is (same mirroring discipline as the
// existing tx/ty stacking formula these functions already share, #585).
static int toast_height_for(const char *body) {
    char lines[TOAST_MAX_LINES][NBODY];
    int n = wrap_text_ttf(body, 13, TOAST_BODY_MAXW, TOAST_MAX_LINES, lines);
    int h = 33 + n * TOAST_LINE_H + 12;   // body-start y + lines + bottom pad
    return h > TOAST_H ? h : TOAST_H;
}

void notif_tick(void){
    unsigned long now=(unsigned long)sys_clock();
    static unsigned long last=0, plast=0;
    if(now-plast>250){ load_prefs(); plast=now; }     // reload prefs ~1s
    if(now-last>=12){ last=now; poll_spool(); }        // poll spool ~50ms
    uint64_t t=uptime_ms(); uint64_t life=(uint64_t)p_duration*1000;
    for(int i=0;i<MAX_TOASTS;i++) if(g_toasts[i].used){
        toast_t*ts=&g_toasts[i];
        if(t-ts->born_ms>life){
            // Expiring: the slot goes unused, so notif_collect_damage()'s
            // per-toast layout loop will never revisit it to erase the rect
            // it vacates (and the remaining stacked toasts above the gap need
            // to be recomposited to reflow into it too). One-shot only.
            ts->used=0; g_needs_redraw=true;
        } else if(!ts->settled && t-ts->born_ms>=220){
            // Slide-in just finished. One more forced frame guarantees the
            // toast reaches its true resting position even when nothing else
            // is triggering a render this tick (e.g. an app window is open
            // and idle, so the #379 idle dirty-rect path below is not
            // reached at all - see main.c's "Pure idle" branch, #564).
            ts->settled=1; g_needs_redraw=true;
        }
    }
    if(g_center_open) g_needs_redraw=true;   // open modal panel: same as any other menu
}

// #585: per-toast damage for the idle dirty-rect path (#379). Mirrors
// notif_render()'s own stacking layout exactly (same tx/ty formula, same
// `shown` bookkeeping) so the two can never disagree about where a toast
// belongs. A toast that has not moved since notif_render() last actually drew
// it (t->rx/ry) contributes NO damage at all - this is what lets a long-lived
// STATIC toast sit on screen at zero per-tick cost, instead of the old "any
// toast visible -> full-screen redraw every tick" behavior.
void notif_collect_damage(void){
    uint64_t now=uptime_ms();
    int y_off=TOAST_MARGIN;
    for(int i=0;i<MAX_TOASTS;i++){
        if(!g_toasts[i].used) continue;
        toast_t*t=&g_toasts[i];
        int th=toast_height_for(t->body);
        int tx=g_fb_width-TOAST_W-TOAST_MARGIN;
        int ty=y_off;
        uint64_t age=now-t->born_ms;
        if(age<220){ int off=(int)((220-age)*(TOAST_W+TOAST_MARGIN)/220); tx+=off; }
        int have_prev = t->rw>0;
        if(have_prev && tx==t->rx && ty==t->ry && th==t->rh){ y_off+=th+TOAST_GAP; continue; }  // static: nothing changed
        int ux0 = have_prev ? (tx<t->rx?tx:t->rx) : tx;
        int uy0 = have_prev ? (ty<t->ry?ty:t->ry) : ty;
        int ux1 = have_prev ? ((tx+TOAST_W)>(t->rx+t->rw)?(tx+TOAST_W):(t->rx+t->rw)) : (tx+TOAST_W);
        int uy1 = have_prev ? ((ty+th)>(t->ry+t->rh)?(ty+th):(t->ry+t->rh)) : (ty+th);
        damage_add(ux0,uy0,ux1-ux0,uy1-uy0);
        y_off+=th+TOAST_GAP;
    }
}

// ---- rendering ------------------------------------------------------------
// (TOAST_W/H/MARGIN/GAP now defined above, ahead of notif_collect_damage())
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
    char title_line[NTITLE];
    ellipsize_ttf(t->title[0]?t->title:sev_word(t->severity), 15, w-50-16, title_line, sizeof(title_line));
    draw_text_ttf(x+50,y+9,title_line,15,ink);
    char body_lines[TOAST_MAX_LINES][NBODY];
    int body_n = wrap_text_ttf(t->body, 13, TOAST_BODY_MAXW, TOAST_MAX_LINES, body_lines);
    for (int li = 0; li < body_n; li++)
        draw_text_ttf(x+50, y+33+li*TOAST_LINE_H, body_lines[li], 13, dim);
    draw_text_ttf(x+w-15,y+5,"x",13,dim);
}
void notif_render(void){
    int y_off=TOAST_MARGIN; uint64_t now=uptime_ms();
    for(int i=0;i<MAX_TOASTS;i++){
        if(!g_toasts[i].used) continue;
        toast_t*t=&g_toasts[i];
        int th=toast_height_for(t->body);
        int tx=g_fb_width-TOAST_W-TOAST_MARGIN;
        int ty=y_off;
        uint64_t age=now-t->born_ms;
        if(age<220){ int off=(int)((220-age)*(TOAST_W+TOAST_MARGIN)/220); tx+=off; }
        t->rx=tx; t->ry=ty; t->rw=TOAST_W; t->rh=th;
        draw_toast(t);
        y_off+=th+TOAST_GAP;
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
            char hist_title[NTITLE], hist_body[NBODY];
            ellipsize_ttf(hh->title[0]?hh->title:sev_word(hh->severity), 13, (x+w-16)-(x+46), hist_title, sizeof(hist_title));
            ellipsize_ttf(hh->body, 12, (x+w-16)-(x+46), hist_body, sizeof(hist_body));
            draw_text_ttf(x+46,ry+4,hist_title,13,ink);
            draw_text_ttf(x+46,ry+22,hist_body,12,dim);
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
