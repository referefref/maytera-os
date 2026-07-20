// taskmanager - MayteraOS Task Manager (#159): real procs + style engine, Files-matched palette.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/theme.h"
#include "../../libc/syscall.h"
#include "../../libc/signal.h"

#define WIN_W 600
#define WIN_H 480
#define ROW_H 22
#define SIGKILL 9

static int win=-1, DW=WIN_W, DH=WIN_H;
static proc_info_t procs[64];
static int nproc=0, cpu_pct[64], prev_valid=0, sel_pid=-1, scroll=0, cpu_total=0;
static unsigned long long prev_ticks[64];
static unsigned long mem_total=0, mem_used=0;
static unsigned int C_BG,C_CARD,C_FIELD,C_BORDER,C_INK,C_DIM,C_ACC,C_SEL,C_SELTX;

static int sl(const char *s){int n=0;while(s&&s[n])n++;return n;}
static void u2s(unsigned long v,char *b){
    char t[24];int n=0; if(!v){b[0]=48;b[1]=0;return;}
    while(v){t[n++]=(char)(48+(v%10));v/=10;} int i=0;while(n)b[i++]=t[--n];b[i]=0;
}
static void mem_str(unsigned int kb,char *b){
    char m[16];int i=0,j=0;
    if(kb>=1024){unsigned int mb=kb/1024,fr=((kb%1024)*10)/1024; u2s(mb,m);while(m[j])b[i++]=m[j++];
        b[i++]=46;b[i++]=(char)(48+fr);b[i++]=32;b[i++]=77;b[i++]=66;b[i]=0;}
    else{u2s(kb,m);while(m[j])b[i++]=m[j++];b[i++]=32;b[i++]=75;b[i++]=66;b[i]=0;}
}
static const char* state_name(unsigned int s){
    switch(s){case 1:return "Ready";case 2:return "Running";case 3:return "Sleep";
              case 4:return "Blocked";case 5:return "Zombie";default:return "-";}
}
static unsigned int lum_ink(unsigned int bg){int r=(bg>>16)&255,g=(bg>>8)&255,b=bg&255;
    return ((r*30+g*59+b*11)/100)>140?0x00181818u:0x00F0F0F0u;}
static unsigned int dim_ink(unsigned int bg){unsigned int k=lum_ink(bg);
    int ir=(k>>16)&255,ig=(k>>8)&255,ib=k&255,br=(bg>>16)&255,bgc=(bg>>8)&255,bb=bg&255;
    return (((ir+br)/2)<<16)|(((ig+bgc)/2)<<8)|((ib+bb)/2);}
static unsigned int tint(unsigned int base,unsigned int acc,int pct){
    int br=(base>>16)&255,bg=(base>>8)&255,bb=base&255,ar=(acc>>16)&255,ag=(acc>>8)&255,ab=acc&255;
    return ((((br*(100-pct)+ar*pct)/100)&255)<<16)|((((bg*(100-pct)+ag*pct)/100)&255)<<8)|(((bb*(100-pct)+ab*pct)/100)&255);}

static void apply_style(void){
    int tid=theme_get_active();
    gui_set_style(tid==4?GUI_STYLE_CLASSIC:GUI_STYLE_MODERN);
    unsigned int wb=theme_color(THEME_COLOR_WINDOW_BG);
    int r=(wb>>16)&255,g=(wb>>8)&255,b=wb&255; int dark=((r*30+g*59+b*11)/100)<128;
    C_ACC=theme_color(THEME_COLOR_ACCENT);
    C_BG   = tint(dark?0x00262A30:0x00F5F6F8, C_ACC, 5);
    C_CARD = tint(dark?0x002C313B:0x00EDEFF3, C_ACC, 6);
    C_FIELD= dark?0x00333A45:0x00FFFFFF;
    C_BORDER=dark?0x003A424F:0x00CDD3DB;
    C_INK=lum_ink(C_BG); C_DIM=dim_ink(C_BG); C_SEL=C_ACC; C_SELTX=lum_ink(C_ACC);
    gui_palette_t p;
    p.surface=C_BG;p.surface_raised=C_CARD;p.ink=C_INK;p.ink_dim=C_DIM;
    p.accent=C_ACC;p.accent_hover=gui_lighten(C_ACC,24);p.border=C_BORDER;
    p.field_bg=C_FIELD;p.field_border=C_BORDER;p.track=tint(C_BG,C_ACC,20);
    gui_set_palette(&p);
}
static void sort_by_cpu(void){
    for(int a=0;a<nproc-1;a++)for(int b=0;b<nproc-1-a;b++){
        int sw=cpu_pct[b]<cpu_pct[b+1]||(cpu_pct[b]==cpu_pct[b+1]&&procs[b].mem_kb<procs[b+1].mem_kb);
        if(sw){proc_info_t tp=procs[b];procs[b]=procs[b+1];procs[b+1]=tp;
               int tc=cpu_pct[b];cpu_pct[b]=cpu_pct[b+1];cpu_pct[b+1]=tc;}}
}
static void refresh(void){
    nproc=sys_proc_list(procs,64); if(nproc<0)nproc=0;
    unsigned long long d[64],tot=0;
    for(int i=0;i<nproc;i++){unsigned p=procs[i].pid;unsigned long long pv=(p<64)?prev_ticks[p]:0;
        d[i]=(procs[i].cpu_ticks>=pv)?(procs[i].cpu_ticks-pv):0;tot+=d[i];}
    for(int i=0;i<nproc;i++)cpu_pct[i]=(prev_valid&&tot)?(int)((d[i]*100ULL)/tot):0;
    for(int i=0;i<64;i++)prev_ticks[i]=0;
    for(int i=0;i<nproc;i++)if(procs[i].pid<64)prev_ticks[procs[i].pid]=procs[i].cpu_ticks;
    prev_valid=1; cpu_total=sys_get_cpu_usage(); sys_get_mem_info(&mem_total,&mem_used); sort_by_cpu();
}
static void draw(void){
    apply_style();
    win_get_size(win,&DW,&DH); if(DW<200)DW=WIN_W; if(DH<200)DH=WIN_H;
    win_draw_rect(win,0,0,DW,DH,C_BG);
    int pad=12,y=10,ch=72;
    gui_card(win,pad,y,DW-2*pad,ch);
    unsigned int cink=lum_ink(C_CARD), cdim=dim_ink(C_CARD);
    win_draw_text_ttf(win,pad+12,y+8,"Performance",14,cink);
    int bx=pad+58, bw=DW-2*pad-58-110;
    win_draw_text_ttf(win,pad+12,y+30,"CPU",11,cdim);
    gui_progress(win,bx,y+31,bw,10,cpu_total);
    {char tf[16];u2s(cpu_total,tf);int l=sl(tf);tf[l]=37;tf[l+1]=0;win_draw_text_ttf(win,bx+bw+10,y+27,tf,12,cink);}
    int rpct=mem_total?(int)((unsigned long long)mem_used*100/mem_total):0;
    win_draw_text_ttf(win,pad+12,y+50,"RAM",11,cdim);
    gui_progress(win,bx,y+51,bw,10,rpct);
    {unsigned long um=mem_used/1048576UL,tm=mem_total/1048576UL;char a[16],bb[16];u2s(um,a);u2s(tm,bb);
     char rt[48];int i=0,j=0;while(a[j])rt[i++]=a[j++];rt[i++]=32;rt[i++]=47;rt[i++]=32;j=0;while(bb[j])rt[i++]=bb[j++];rt[i++]=32;rt[i++]=77;rt[i++]=66;rt[i]=0;
     win_draw_text_ttf(win,bx+bw+10,y+47,rt,11,cink);}
    y+=ch+10;
    int cName=pad+10,cPid=DW-330,cCore=DW-268,cState=DW-208,cCpu=DW-128,cMem=DW-84;
    win_draw_text_ttf(win,cName,y,"Name",11,C_DIM);
    win_draw_text_ttf(win,cPid,y,"PID",11,C_DIM);
    win_draw_text_ttf(win,cCore,y,"Core",11,C_DIM);
    win_draw_text_ttf(win,cState,y,"State",11,C_DIM);
    win_draw_text_ttf(win,cCpu,y,"CPU",11,C_DIM);
    win_draw_text_ttf(win,cMem,y,"Memory",11,C_DIM);
    y+=18; win_draw_rect(win,pad,y-2,DW-2*pad,1,C_BORDER);
    int listtop=y,listbot=DH-48,rows=(listbot-listtop)/ROW_H;
    for(int rr=0;rr<rows&&(rr+scroll)<nproc;rr++){
        int i=rr+scroll,ry=listtop+rr*ROW_H,selrow=(procs[i].pid==(unsigned)sel_pid);
        if(selrow)gui_fill_rounded_aa(win,pad,ry,DW-2*pad,ROW_H-2,4,C_SEL,C_BG);
        unsigned int tx=selrow?C_SELTX:C_INK, td=selrow?C_SELTX:C_DIM;
        win_draw_text_ttf(win,cName,ry+3,procs[i].name,12,tx);
        char nb[16];u2s(procs[i].pid,nb);win_draw_text_ttf(win,cPid,ry+3,nb,12,tx);
        { char cc[8]; int rc=procs[i].running_cpu;
          if(rc<1){cc[0]=0x2d;cc[1]=0;} else {cc[0]='A';cc[1]='P';u2s((unsigned)rc,cc+2);}
          win_draw_text_ttf(win,cCore,ry+3,cc,11,td); }
        win_draw_text_ttf(win,cState,ry+3,state_name(procs[i].state),11,td);
        char cb[8];u2s((unsigned)cpu_pct[i],cb);int cl=sl(cb);cb[cl]=37;cb[cl+1]=0;win_draw_text_ttf(win,cCpu,ry+3,cb,12,tx);
        char mf[24];mem_str(procs[i].mem_kb,mf);win_draw_text_ttf(win,cMem,ry+3,mf,11,td);
    }
    int fy=DH-40; win_draw_rect(win,pad,fy-6,DW-2*pad,1,C_BORDER);
    {char cnt[40];u2s((unsigned)nproc,cnt);int cn=sl(cnt);const char *sf=" processes";for(int k=0;sf[k];k++)cnt[cn++]=sf[k];cnt[cn]=0;
     win_draw_text_ttf(win,pad,fy+8,cnt,12,C_DIM);}
    gui_button(win,DW-128,fy,108,28,"End Task",GUI_BTN_PRIMARY,sel_pid>1?GUI_ST_NORMAL:GUI_ST_DISABLED);
    win_invalidate(win);
}
static void end_task(void){ if(sel_pid>1){kill(sel_pid,SIGKILL);sel_pid=-1;refresh();draw();} }
int main(void){
    win=win_create("Task Manager",140,90,WIN_W,WIN_H);
    if(win<0)return 1;
    refresh();draw();
    int running=1;
    while(running){
        gui_event_t ev; int et=win_get_event(win,&ev,1000);
        if(et==0){refresh();draw();continue;}
        switch(ev.type){
            case EVENT_REDRAW:draw();break;
            case EVENT_RESIZE:draw();break;
            case EVENT_WINDOW_CLOSE:running=0;break;
            case EVENT_KEY_DOWN: if(ev.key_char==27)running=0; else if(ev.key_char==127)end_task(); break;
            case EVENT_MOUSE_DOWN:{
                int wx,wy;win_get_pos(win,&wx,&wy);
                int lx=ev.mouse_x,ly=ev.mouse_y,fy=DH-40;
                if(ly>=fy&&ly<fy+28&&lx>=DW-128&&lx<DW-20){end_task();break;}
                int listtop=10+72+10+18;
                if(ly>=listtop&&ly<DH-48){int rr=(ly-listtop)/ROW_H,idx=rr+scroll;
                    if(idx>=0&&idx<nproc){sel_pid=(int)procs[idx].pid;draw();}}
                break;}
            case EVENT_MOUSE_SCROLL:
                scroll+=(ev.mouse_y>0)?1:-1; if(scroll<0)scroll=0; if(scroll>nproc-1)scroll=nproc-1; draw(); break;
            default:break;
        }
    }
    win_destroy(win);return 0;
}
