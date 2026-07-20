// World Clock + Calendar (#96) - replaces the old single Clock app that hung.
// Shows six time zones (analog + digital + city) and a month calendar. Reads
// the RTC; each city applies a whole-hour UTC offset. Day/week/quarter views,
// custom events and a weather overlay are planned follow-ups.

#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/syscall.h"

#define WIN_W 820
#define WIN_H 540
static int win = -1;

// sin(pos*6deg) * 1000 for pos 0..59.
static const int SIN[60] = {
      0,  105,  208,  309,  407,  500,  588,  669,  743,  809,
    866,  914,  951,  978,  995, 1000,  995,  978,  951,  914,
    866,  809,  743,  669,  588,  500,  407,  309,  208,  105,
      0, -105, -208, -309, -407, -500, -588, -669, -743, -809,
   -866, -914, -951, -978, -995,-1000, -995, -978, -951, -914,
   -866, -809, -743, -669, -588, -500, -407, -309, -208, -105,
};
static int s_sin(int p){ p %= 60; if (p < 0) p += 60; return SIN[p]; }
static int s_cos(int p){ return s_sin(p + 15); }

typedef struct { const char *name; int off; } city_t;
// UTC offsets (whole hours). RTC is treated as UTC for relative display.
static city_t cities[6] = {
    {"London (UTC)",  0}, {"New York",   -5}, {"Sao Paulo",  -3},
    {"Tokyo",        +9}, {"Sydney",    +10}, {"Los Angeles",-8},
};

static const char *MON[12] = {"January","February","March","April","May","June",
    "July","August","September","October","November","December"};

static void two(char *b, int v){ b[0]='0'+(v/10)%10; b[1]='0'+v%10; b[2]=0; }

static void line(int x0,int y0,int x1,int y1,uint32_t c){
    int dx=x1-x0, dy=y1-y0;
    int adx=dx<0?-dx:dx, ady=dy<0?-dy:dy;
    int sx=dx<0?-1:1, sy=dy<0?-1:1, err=adx-ady;
    for(;;){ win_draw_pixel(win,x0,y0,c);
        if(x0==x1&&y0==y1)break;
        int e2=2*err; if(e2>-ady){err-=ady;x0+=sx;} if(e2<adx){err+=adx;y0+=sy;} }
}
static void hand(int cx,int cy,int pos,int len,int thick,uint32_t c){
    int ex=cx+len*s_sin(pos)/1000, ey=cy-len*s_cos(pos)/1000;
    line(cx,cy,ex,ey,c);
    for(int t=1;t<thick;t++){ line(cx+t,cy,ex+t,ey,c); line(cx,cy+t,ex,ey+t,c); }
}

// Cheap midpoint-circle outline (avoids per-pixel filled-disc syscall storm).
static void circle(int cx,int cy,int r,uint32_t c){
    int x=0,y=r,d=1-r;
    while(x<=y){
        win_draw_pixel(win,cx+x,cy+y,c); win_draw_pixel(win,cx-x,cy+y,c);
        win_draw_pixel(win,cx+x,cy-y,c); win_draw_pixel(win,cx-x,cy-y,c);
        win_draw_pixel(win,cx+y,cy+x,c); win_draw_pixel(win,cx-y,cy+x,c);
        win_draw_pixel(win,cx+y,cy-x,c); win_draw_pixel(win,cx-y,cy-x,c);
        if(d<0)d+=2*x+3; else {d+=2*(x-y)+5;y--;} x++;
    }
}
static void clock_face(int cx,int cy,int r,int h,int m,int s,const char *name){
    circle(cx,cy,r,   THEME_WINDOW_BORDER);
    circle(cx,cy,r-1, THEME_TEXT_SECONDARY);
    for(int i=0;i<12;i++){
        int p=i*5, ir=r-9, orr=r-3;
        line(cx+ir*s_sin(p)/1000, cy-ir*s_cos(p)/1000,
             cx+orr*s_sin(p)/1000, cy-orr*s_cos(p)/1000, THEME_TEXT_PRIMARY);
    }
    hand(cx,cy,(h%12)*5+m/12, r-26, 2, THEME_TEXT_PRIMARY);   // hour
    hand(cx,cy,m,             r-14, 2, THEME_TEXT_PRIMARY);   // minute
    hand(cx,cy,s,             r-10, 1, THEME_ERROR);          // second
    for(int dy=-2;dy<=2;dy++) for(int dx=-2;dx<=2;dx++)
        win_draw_pixel(win,cx+dx,cy+dy,THEME_ACCENT);
    int nw = gui_string_width(name);
    win_draw_text(win, cx-nw/2, cy-r-14, name, THEME_TEXT_PRIMARY);
    char t[6]; char hh[3],mm[3]; two(hh,h); two(mm,m);
    t[0]=hh[0];t[1]=hh[1];t[2]=':';t[3]=mm[0];t[4]=mm[1];t[5]=0;
    int tw = gui_string_width(t);
    win_draw_text(win, cx-tw/2, cy+r+4, t, THEME_ACCENT);
}

static int dim(int m,int y){ static const int d[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    if(m==2&&((y%4==0&&y%100!=0)||y%400==0))return 29; if(m<1||m>12)return 30; return d[m-1]; }
static int dow(int d,int m,int y){ if(m<3){m+=12;y-=1;} int K=y%100,J=y/100;
    int h=(d+13*(m+1)/5+K+K/4+J/4+5*J)%7; return (h+6)%7; }

static void calendar(int x,int y,int w,int dd,int mm,int yy){
    int cell=w/7;
    win_draw_rect(win,x,y,w,236,THEME_BG_SECONDARY);
    gui_draw_rect_outline(win,x,y,w,236,THEME_WINDOW_BORDER);
    char title[24]; const char*mn=MON[(mm>=1&&mm<=12)?mm-1:0]; int i=0;
    while(mn[i]){title[i]=mn[i];i++;} title[i++]=' ';
    title[i++]='0'+(yy/1000)%10; title[i++]='0'+(yy/100)%10; title[i++]='0'+(yy/10)%10; title[i++]='0'+yy%10; title[i]=0;
    int tw=gui_string_width(title);
    win_draw_text(win, x+(w-tw)/2, y+6, title, THEME_ACCENT);
    static const char*wd[7]={"Su","Mo","Tu","We","Th","Fr","Sa"};
    int gy=y+28;
    for(int c=0;c<7;c++){ uint32_t wc=(c==0||c==6)?THEME_ERROR:THEME_TEXT_SECONDARY;
        win_draw_text(win, x+c*cell+(cell-gui_string_width(wd[c]))/2, gy, wd[c], wc); }
    int first=dow(1,mm,yy), nd=dim(mm,yy), gy0=gy+20, rh=28;
    for(int day=1;day<=nd;day++){
        int idx=first+day-1, col=idx%7, row=idx/7;
        int cx=x+col*cell, cyy=gy0+row*rh;
        char nb[3]; if(day<10){nb[0]='0'+day;nb[1]=0;} else two(nb,day);
        uint32_t tc=(col==0||col==6)?THEME_ERROR:THEME_TEXT_PRIMARY;
        if(day==dd){ win_draw_rect(win,cx+2,cyy-2,cell-4,rh-2,THEME_ACCENT); tc=THEME_TEXT_PRIMARY; }
        win_draw_text(win, cx+(cell-gui_string_width(nb))/2, cyy, nb, tc);
    }
}

static void draw_all(void){
    int h,m,s,dd,mm,yy;
    get_rtc_time(&h,&m,&s); get_rtc_date(&dd,&mm,&yy);
    if(mm<1||mm>12)mm=1; if(yy<1970||yy>3000)yy=2026;
    win_draw_rect(win,0,0,WIN_W,WIN_H,THEME_BG_PRIMARY);
    win_draw_text(win,14,10,"World Clock",THEME_ACCENT);
    int r=58, cols=3, area=560, gx=area/cols;
    for(int i=0;i<6;i++){
        int col=i%3, row=i/3;
        int cx=col*gx+gx/2, cy=58+r + row*(2*r+44);
        int ch=(h+cities[i].off+24)%24;
        clock_face(cx,cy,r,ch,m,s,cities[i].name);
    }
    calendar(area+8, 44, WIN_W-area-20, dd, mm, yy);
    win_invalidate(win);
}

int main(int argc,char**argv){
    (void)argc;(void)argv;
    win=win_create("World Clock",120,80,WIN_W,WIN_H);
    if(win<0)return 1;
    draw_all();
    gui_event_t ev; int running=1; uint64_t last=sys_clock();
    while(running){
        int t=win_get_event(win,&ev,200);
        if(t>0){
            if(ev.type==EVENT_WINDOW_CLOSE) running=0;
            else if(ev.type==EVENT_REDRAW) draw_all();
        }
        uint64_t now=sys_clock();
        if(now-last>=1000){ last=now; draw_all(); }  // tick ~1/s
    }
    win_destroy(win);
    return 0;
}
