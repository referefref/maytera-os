/* main.c - Maytera Chess: window, event loop, screen state machine, input
 * (click-to-select / click-to-move + camera), the computer opponent worker
 * thread, and the 2D overlay (menu / HUD / move list / promotion / banners).
 * 3D scene lives in render.c; rules in engine.c; AI in ai.c. */
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "pthread.h"
#include "gfx.h"
#include <string.h>
#include <stdlib.h>

#define MAXW 1280
#define MAXH 800
static unsigned int g_blit[MAXW*MAXH];

static App A;
extern int ai_pick(const Position*,int,unsigned,Move*);

/* ---- AI worker ---- */
static volatile int g_ai_ready, g_ai_busy;
static Move g_ai_move;
static Position g_ai_pos;
static int g_ai_diff;
static unsigned int g_ai_seed = 12345;
static void *ai_thread(void *arg){
    (void)arg;
    Move m; memset(&m,0,sizeof m);
    ai_pick(&g_ai_pos, g_ai_diff, g_ai_seed, &m);
    g_ai_move = m;
    g_ai_ready = 1; g_ai_busy = 0;
    return 0;
}
static void ai_start(void){
    g_ai_pos = A.game.pos; g_ai_diff = A.difficulty; g_ai_seed = g_ai_seed*1664525u+1013904223u;
    g_ai_busy = 1; g_ai_ready = 0;
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 512*1024);
    pthread_t t;
    if (pthread_create(&t, &at, ai_thread, 0) == 0) pthread_detach(t);
    else { g_ai_busy = 0; /* fallback: compute inline */ ai_thread(0); }
}

/* ---- move list (SAN) ---- */
#define SANMAX 200
static char g_san[SANMAX][10];
static int  g_sann;
static void san_push(const char *s){ if(g_sann<SANMAX){ int i=0; while(s[i]&&i<9){g_san[g_sann][i]=s[i];i++;} g_san[g_sann][i]=0; g_sann++; } }

/* ---- selection / legal ---- */
static void recompute_legal(void){
    for (int i=0;i<64;i++) A.legal_to[i]=0;
    A.nlegal=0;
    if (A.sel<0) return;
    Move mv[MAX_MOVES]; int n=chess_gen_legal(&A.game.pos,mv);
    for (int i=0;i<n;i++) if (mv[i].from==A.sel){ A.legal_to[mv[i].to]=1; A.nlegal++; }
}

static void set_gameover_if_done(void){
    int r=chess_game_result(&A.game);
    if (r!=RES_NONE){ A.result=r; A.screen=SC_GAMEOVER; }
}

/* apply a move (from,to,promo) with slide animation */
static void do_play(int from,int to,int promo){
    Move want; want.from=(signed char)from; want.to=(signed char)to; want.promo=(signed char)promo; want.flags=0;
    /* SAN uses the exact legal move; find it for notation + flags */
    Move mv[MAX_MOVES]; int n=chess_gen_legal(&A.game.pos,mv); Move real; int found=0;
    for(int i=0;i<n;i++) if(mv[i].from==from&&mv[i].to==to&&(!(mv[i].flags&MF_PROMO)||mv[i].promo==promo)){ real=mv[i]; found=1; break; }
    if(!found) return;
    char san[10]; chess_san(&A.game.pos, real, san); san_push(san);
    /* animation setup (from pre-move board) */
    int ff=from&7, fr=from>>3, tf=to&7, tr=to>>3;
    /* local mapping identical to render.c world_of */
    #define WORLDX(f) (((f)-3.5f))
    #define WORLDZ(r) ((3.5f-(r)))
    A.anim_fx = A.flip? -WORLDX(ff): WORLDX(ff);
    A.anim_fz = A.flip? -WORLDZ(fr): WORLDZ(fr);
    A.anim_tx = A.flip? -WORLDX(tf): WORLDX(tf);
    A.anim_tz = A.flip? -WORLDZ(tr): WORLDZ(tr);
    A.anim_move = real; A.anim_active=1; A.anim_apply_pending=1; A.anim_t=0;
    chess_play(&A.game, real);
    A.anim_piece = A.game.pos.board[to];   /* landed piece (promoted if promo) */
    A.last_from=from; A.last_to=to;
    A.sel=-1; recompute_legal();
}

static int is_human_turn(void){
    if (A.mode==MODE_HOTSEAT) return 1;
    return A.game.pos.side==A.human_color;
}

/* board click during play */
static void board_click(int sq){
    if (sq<0) return;
    if (A.anim_active) return;
    if (!is_human_turn()) return;
    signed char pc=A.game.pos.board[sq];
    int me=A.game.pos.side;
    if (A.sel<0){
        if (pc!=0 && ((pc<0?BLACK:WHITE)==me)){ A.sel=sq; recompute_legal(); }
        return;
    }
    if (sq==A.sel){ A.sel=-1; recompute_legal(); return; }
    if (A.legal_to[sq]){
        /* promotion? pawn reaching last rank */
        int fr=A.sel>>3; int tr=sq>>3; signed char sp=A.game.pos.board[A.sel];
        int type = sp<0?-sp:sp;
        if (type==PAWN && (tr==7||tr==0)){ A.promo_from=A.sel; A.promo_to=sq; A.screen=SC_PROMOTE; return; }
        do_play(A.sel,sq,QUEEN);
        set_gameover_if_done();
        return;
    }
    if (pc!=0 && ((pc<0?BLACK:WHITE)==me)){ A.sel=sq; recompute_legal(); return; }
    A.sel=-1; recompute_legal();
}

/* ---- menu / overlay layout + hit testing ---- */
typedef struct { int x,y,w,h; const char *label; int id; } Btn;

/* main-menu buttons */
static int menu_btns(Btn *b){
    int cx=A.vw/2, y=A.vh/2-40, bw=280, bh=44, gap=12, n=0;
    b[n]=(Btn){cx-bw/2,y,bw,bh,"2-PLAYER",1}; n++; y+=bh+gap;
    b[n]=(Btn){cx-bw/2,y,bw,bh,"VS COMPUTER",2}; n++; y+=bh+gap;
    b[n]=(Btn){cx-bw/2,y,bw,bh,A.difficulty==0?"DIFFICULTY: EASY":A.difficulty==1?"DIFFICULTY: MEDIUM":"DIFFICULTY: HARD",3}; n++; y+=bh+gap;
    b[n]=(Btn){cx-bw/2,y,bw,bh,"HOW TO PLAY",4}; n++; y+=bh+gap;
    b[n]=(Btn){cx-bw/2,y,bw,bh,"QUIT",5}; n++;
    return n;
}
static int promo_btns(Btn *b){
    int cx=A.vw/2, y=A.vh/2-30, bw=70, bh=60, gap=14, total=4*bw+3*gap, x=cx-total/2;
    const char *L[4]={"Q","R","B","N"};
    for(int i=0;i<4;i++){ b[i]=(Btn){x+i*(bw+gap),y,bw,bh,L[i],10+i}; }
    return 4;
}
static int hit(Btn *b,int mx,int my){ return mx>=b->x&&mx<b->x+b->w&&my>=b->y&&my<b->y+b->h; }

static void draw_button(Btn *b,int hov){
    unsigned int base = hov?0x2E5C8A:0x24384C;
    ov_rect_a(g_blit,A.vw,A.vh,b->x,b->y,b->w,b->h,base,235);
    ov_frame(g_blit,A.vw,A.vh,b->x,b->y,b->w,b->h,0x5FA8D8);
    ov_text_c(g_blit,A.vw,A.vh,b->x+b->w/2,b->y+b->h/2-6,b->label,0xEAF2FA,2);
}

/* ---- overlay for each screen ---- */
static const char *res_text(int r){
    switch(r){ case RES_CHECKMATE: return "CHECKMATE"; case RES_STALEMATE: return "STALEMATE";
        case RES_DRAW_50: return "DRAW (50-MOVE)"; case RES_DRAW_MATERIAL: return "DRAW (MATERIAL)";
        case RES_DRAW_REPETITION: return "DRAW (REPETITION)"; }
    return "";
}

static void draw_hud(void){
    int w=A.vw, h=A.vh;
    /* top status bar */
    ov_rect_a(g_blit,w,h,0,0,w,30,0x101820,210);
    const char *turn = (A.game.pos.side==WHITE)?"WHITE to move":"BLACK to move";
    if (A.mode==MODE_VS_CPU) turn = (A.game.pos.side==A.human_color)?"YOUR MOVE":"COMPUTER thinking...";
    ov_text(g_blit,w,h,10,8,turn,0xE8EEF4,2);
    if (chess_in_check(&A.game.pos,A.game.pos.side))
        ov_text(g_blit,w,h,10+ov_text_w(turn,2)+16,8,"CHECK",0xFF6060,2);
    ov_text(g_blit,w,h,w-210,8,"U undo  R redo  M menu",0x9FB4C6,1);
    /* move list panel (right) */
    int px=w-180, py=40, pw=170, ph=h-52;
    ov_rect_a(g_blit,w,h,px,py,pw,ph,0x0E141C,180);
    ov_frame(g_blit,w,h,px,py,pw,ph,0x2B3A4A);
    ov_text(g_blit,w,h,px+8,py+6,"MOVES",0x8FB0CC,1);
    int start = g_sann>28? g_sann-28:0;
    int yy=py+22;
    for (int i=start;i<g_sann;i+=2){
        char line[40]; int num=i/2+1; char nb[8];
        /* "N. white black" */
        int p=0; char tmp[8]; int t=num,ti=0; if(t==0)tmp[ti++]='0'; while(t){tmp[ti++]='0'+t%10;t/=10;} while(ti)line[p++]=tmp[--ti];
        line[p++]='.'; line[p++]=' '; (void)nb;
        const char *ws=g_san[i]; for(int k=0;ws[k]&&p<38;k++)line[p++]=ws[k];
        if(i+1<g_sann){ line[p++]=' '; const char *bs=g_san[i+1]; for(int k=0;bs[k]&&p<38;k++)line[p++]=bs[k]; }
        line[p]=0;
        ov_text(g_blit,w,h,px+8,yy,line,0xD6E2EC,1);
        yy+=12; if(yy>py+ph-14)break;
    }
}

/* rank/file coordinate labels projected onto the board edges (files a-h along
 * the near edge, ranks 1-8 up the a-file side). Follows camera orbit + flip. */
static void draw_coords(void){
    if (!A.show_coords) return;
    int w=A.vw, h=A.vh;
    const unsigned int col=0xF0E4C0;   /* cream, reads on the dark board rim */
    const float EDGE=4.22f;            /* just outside the 8x8 playing area */
    for (int f=0; f<8; f++){
        float wx=(f-3.5f), wz=EDGE;
        if (A.flip){ wx=-wx; wz=-wz; }
        int sx,sy; if (gfx_project(&A,wx,0.05f,wz,&sx,&sy)){
            char s[2]={(char)('a'+f),0}; ov_text_c(g_blit,w,h,sx,sy-6,s,col,2);
        }
    }
    for (int r=0; r<8; r++){
        float wx=-EDGE, wz=(3.5f-r);
        if (A.flip){ wx=-wx; wz=-wz; }
        int sx,sy; if (gfx_project(&A,wx,0.05f,wz,&sx,&sy)){
            char s[2]={(char)('1'+r),0}; ov_text_c(g_blit,w,h,sx,sy-6,s,col,2);
        }
    }
}

static void draw_menu(void){
    int w=A.vw,h=A.vh;
    ov_rect_a(g_blit,w,h,0,0,w,h,0x0A0E14,120);
    /* logo */
    int lw,lh; const unsigned int *logo=ov_load_bg("/CHESS/LOGO.BMP",&lw,&lh);
    if (logo){
        int scale=1; while ((lw/(scale+1))> w/2 && scale<4) {} /* keep */
        int dw=lw, dh=lh; if(dw>w-80){ dh=dh*(w-80)/dw; dw=w-80; }
        int lx=(w-dw)/2, ly=h/6;
        for(int j=0;j<dh;j++){int sy=j*lh/dh; for(int i=0;i<dw;i++){int sx=i*lw/dw; unsigned int p=logo[sy*lw+sx]; if((p>>24)<128)continue; int X=lx+i,Y=ly+j; if(X>=0&&X<w&&Y>=0&&Y<h)g_blit[(long)Y*w+X]=0xFF000000u|(p&0xFFFFFF);}}
        free((void*)logo);
    } else {
        ov_text_c(g_blit,w,h,w/2,h/6,"MAYTERA CHESS",0xE8D9A8,4);
    }
    Btn b[8]; int n=menu_btns(b);
    for(int i=0;i<n;i++) draw_button(&b[i], hit(&b[i],A.mouse_x,A.mouse_y));
    ov_text_c(g_blit,w,h,w/2,h-30,"Maytera Chess  -  3D TinyGL",0x6A8098,1);
}

static void draw_promote(void){
    int w=A.vw,h=A.vh;
    ov_rect_a(g_blit,w,h,0,0,w,h,0x0A0E14,150);
    ov_text_c(g_blit,w,h,w/2,h/2-70,"PROMOTE PAWN",0xE8EEF4,2);
    Btn b[4]; int n=promo_btns(b);
    for(int i=0;i<n;i++) draw_button(&b[i], hit(&b[i],A.mouse_x,A.mouse_y));
}

static void draw_help(void){
    int w=A.vw,h=A.vh;
    ov_rect_a(g_blit,w,h,0,0,w,h,0x0A0E14,220);
    ov_text_c(g_blit,w,h,w/2,60,"HOW TO PLAY",0xE8D9A8,3);
    const char *lines[]={
        "Click one of your pieces to select it; legal moves",
        "are marked with green dots. Click a marked square to",
        "move. Pawn promotions let you pick a piece.",
        "",
        "2-PLAYER: hotseat, both sides on this machine.",
        "VS COMPUTER: you play White, the engine plays Black.",
        "  Set strength on the menu (Easy / Medium / Hard).",
        "",
        "Keys:  U undo   R redo   N new game   M menu",
        "       F flip board   A/D orbit   W/S tilt   ESC back",
        "",
        "Draws: stalemate, 50-move, threefold, insufficient",
        "material are all detected automatically.",
        0
    };
    int y=120; for(int i=0;lines[i];i++){ ov_text_c(g_blit,w,h,w/2,y,lines[i],0xC8D6E2,2); y+=26; }
    ov_text_c(g_blit,w,h,w/2,h-50,"[ Click anywhere or press ESC to return ]",0x7FA0BE,1);
}

static void draw_gameover(void){
    int w=A.vw,h=A.vh;
    draw_hud();
    int bw=440,bh=150,bx=(w-bw)/2,by=(h-bh)/2;
    ov_rect_a(g_blit,w,h,bx,by,bw,bh,0x111A24,235);
    ov_frame(g_blit,w,h,bx,by,bw,bh,0x5FA8D8);
    const char *rt=res_text(A.result);
    ov_text_c(g_blit,w,h,w/2,by+24,rt,0xF0E0B0,3);
    if (A.result==RES_CHECKMATE){
        const char *win=(A.game.pos.side==WHITE)?"BLACK WINS":"WHITE WINS";
        ov_text_c(g_blit,w,h,w/2,by+66,win,0xE8EEF4,2);
    }
    ov_text_c(g_blit,w,h,w/2,by+bh-34,"N: new game     M: menu",0xA8C0D4,2);
}

/* ---- input ---- */
static void on_click(int mx,int my){
    if (A.screen==SC_MENU){
        Btn b[8]; int n=menu_btns(b);
        for(int i=0;i<n;i++) if(hit(&b[i],mx,my)){
            switch(b[i].id){
                case 1: A.mode=MODE_HOTSEAT; chess_new(&A.game); g_sann=0; A.sel=-1; A.last_from=A.last_to=-1; A.result=0; recompute_legal(); A.screen=SC_PLAY; break;
                case 2: A.mode=MODE_VS_CPU; A.human_color=WHITE; chess_new(&A.game); g_sann=0; A.sel=-1; A.last_from=A.last_to=-1; A.result=0; recompute_legal(); A.screen=SC_PLAY; break;
                case 3: A.difficulty=(A.difficulty+1)%3; break;
                case 4: A.screen=SC_HELP; break;
                case 5: exit(0); break;
            }
            return;
        }
        return;
    }
    if (A.screen==SC_HELP){ A.screen=SC_MENU; return; }
    if (A.screen==SC_PROMOTE){
        Btn b[4]; int n=promo_btns(b);
        for(int i=0;i<n;i++) if(hit(&b[i],mx,my)){
            int pr[4]={QUEEN,ROOK,BISHOP,KNIGHT};
            A.screen=SC_PLAY;
            do_play(A.promo_from,A.promo_to,pr[b[i].id-10]);
            set_gameover_if_done();
            return;
        }
        return;
    }
    if (A.screen==SC_GAMEOVER){
        /* click board area does nothing; use keys N/M */
        return;
    }
    if (A.screen==SC_PLAY){
        int sq=gfx_pick_square(&A,mx,my);
        board_click(sq);
    }
}

static void on_key(char c, unsigned int kc){
    if (c>='A'&&c<='Z') c=c-'A'+'a';
    if (kc==27 || c==27){ /* ESC */
        if (A.screen==SC_PLAY) A.screen=SC_MENU;
        else if (A.screen==SC_HELP||A.screen==SC_PROMOTE||A.screen==SC_GAMEOVER) A.screen=SC_MENU;
        return;
    }
    if (A.screen==SC_MENU){
        /* menu keyboard shortcuts (also drivable headless) */
        if (c=='1'){ A.mode=MODE_HOTSEAT; chess_new(&A.game); g_sann=0; A.sel=-1; A.last_from=A.last_to=-1; A.result=0; recompute_legal(); A.screen=SC_PLAY; }
        else if (c=='2'){ A.mode=MODE_VS_CPU; A.human_color=WHITE; chess_new(&A.game); g_sann=0; A.sel=-1; A.last_from=A.last_to=-1; A.result=0; recompute_legal(); A.screen=SC_PLAY; }
        else if (c=='3'){ A.difficulty=(A.difficulty+1)%3; }
        else if (c=='h'){ A.screen=SC_HELP; }
        return;
    }
    if (A.screen==SC_HELP) return;
    /* keyboard board cursor: i/k = rank up/down, j/l = file left/right, space = act */
    if (A.screen==SC_PLAY){
        int f=A.cursor&7, r=A.cursor>>3;
        if (c=='i'){ if(r<7)r++; A.cursor=r*8+f; return; }
        if (c=='k'){ if(r>0)r--; A.cursor=r*8+f; return; }
        if (c=='j'){ if(f>0)f--; A.cursor=r*8+f; return; }
        if (c=='l'){ if(f<7)f++; A.cursor=r*8+f; return; }
        if (c==' ' || kc==13 || c==13){ board_click(A.cursor); set_gameover_if_done(); return; }
    }
    switch(c){
        case 'u': if(!A.anim_active){ chess_undo(&A.game); if(g_sann)g_sann--; if(A.mode==MODE_VS_CPU){chess_undo(&A.game); if(g_sann)g_sann--;} A.sel=-1; recompute_legal(); A.result=0; if(A.screen==SC_GAMEOVER)A.screen=SC_PLAY; A.last_from=A.last_to=-1; } break;
        case 'r': if(!A.anim_active){ chess_redo(&A.game); A.sel=-1; recompute_legal(); } break;
        case 'n': chess_new(&A.game); g_sann=0; A.sel=-1; A.last_from=A.last_to=-1; A.result=0; recompute_legal(); A.screen=SC_PLAY; break;
        case 'm': A.screen=SC_MENU; break;
        case 'f': A.flip^=1; break;
        case 'a': A.cam_yaw-=8; break;
        case 'd': A.cam_yaw+=8; break;
        case 'w': A.cam_pitch+=4; if(A.cam_pitch>80)A.cam_pitch=80; break;
        case 's': A.cam_pitch-=4; if(A.cam_pitch<15)A.cam_pitch=15; break;
    }
}

/* Instant loading splash, painted BEFORE the TinyGL context + board texture
 * load, so the window shows the menu backdrop immediately instead of a white
 * flash. Uses only ov_* + ov_load_bg, which do not need gfx_init.            */
static void draw_loading_splash(int w, int h){
    for(int y=0;y<h;y++){                        /* menu-tone vertical gradient */
        int t = (h>1) ? y*255/(h-1) : 0;
        int r = 0x0A + (0x16 - 0x0A)*t/255;
        int g = 0x0E + (0x20 - 0x0E)*t/255;
        int b = 0x14 + (0x2E - 0x14)*t/255;
        unsigned int c = 0xFF000000u|((unsigned)r<<16)|((unsigned)g<<8)|(unsigned)b;
        for(int x=0;x<w;x++) g_blit[(long)y*w+x]=c;
    }
    int lw,lh; const unsigned int *logo=ov_load_bg("/CHESS/LOGO.BMP",&lw,&lh);
    if(logo && lw>0 && lh>0){
        int dw=lw, dh=lh; if(dw>w-80){ dh=dh*(w-80)/dw; dw=w-80; }
        int lx=(w-dw)/2, ly=h/6;
        for(int j=0;j<dh;j++){int sy=j*lh/dh; for(int i=0;i<dw;i++){int sx=i*lw/dw;
            unsigned int p=logo[sy*lw+sx]; if((p>>24)<128)continue; int X=lx+i,Y=ly+j;
            if(X>=0&&X<w&&Y>=0&&Y<h)g_blit[(long)Y*w+X]=0xFF000000u|(p&0xFFFFFF);}}
        free((void*)logo);
    } else {
        ov_text_c(g_blit,w,h,w/2,h/6,"MAYTERA CHESS",0xE8D9A8,4);
    }
    ov_text_c(g_blit,w,h,w/2,h/2,"Loading...",0x9FB4C6,2);
}

int main(int argc, char **argv){
    (void)argc;(void)argv;
    int win = win_create("Maytera Chess", 90, 60, 900, 680);
    if (win<0) return 1;
    int cw=900, ch=680;
    if (win_get_size(win,&cw,&ch)!=0 || cw<=0) { cw=900; ch=680; }
    if (cw>MAXW)cw=MAXW; if(ch>MAXH)ch=MAXH;

    /* Show the menu backdrop instantly (no white flash) before slow asset load. */
    draw_loading_splash(cw,ch);
    syscall5(SYS_WIN_BLIT, win, 0, 0, (cw&0xFFFF)|((ch&0xFFFF)<<16), (long)g_blit);
    win_invalidate(win);

    memset(&A,0,sizeof A);
    A.screen=SC_MENU; A.mode=MODE_HOTSEAT; A.difficulty=1; A.human_color=WHITE;
    A.sel=-1; A.last_from=-1; A.last_to=-1; A.show_coords=1; A.cursor=12;
    A.cam_yaw=0; A.cam_pitch=48; A.vw=cw; A.vh=ch;
    chess_new(&A.game);

    gfx_init(cw,ch);
    gfx_load_assets();

    gui_event_t ev;
    int rmb_drag=0, last_mx=0, last_my=0;

    while (1){
        int et = win_get_event(win,&ev,16);
        if (et==EVENT_WINDOW_CLOSE) break;
        if (et==EVENT_RESIZE){
            int nw,nh;
            if (win_get_size(win,&nw,&nh)==0 && nw>0 && nh>0){
                if(nw>MAXW)nw=MAXW; if(nh>MAXH)nh=MAXH;
                A.vw=nw; A.vh=nh; gfx_resize(nw,nh); gfx_load_assets();
            }
        } else if (et==EVENT_MOUSE_MOVE){
            A.mouse_x=ev.mouse_x; A.mouse_y=ev.mouse_y;
            if (rmb_drag){ A.cam_yaw += (ev.mouse_x-last_mx)*0.4f; A.cam_pitch -= (ev.mouse_y-last_my)*0.3f;
                if(A.cam_pitch>82)A.cam_pitch=82; if(A.cam_pitch<12)A.cam_pitch=12; }
            last_mx=ev.mouse_x; last_my=ev.mouse_y;
        } else if (et==EVENT_MOUSE_DOWN){
            A.mouse_x=ev.mouse_x; A.mouse_y=ev.mouse_y; last_mx=ev.mouse_x; last_my=ev.mouse_y;
            if (ev.mouse_buttons & MOUSE_BUTTON_RIGHT) rmb_drag=1;
            else on_click(ev.mouse_x,ev.mouse_y);
        } else if (et==EVENT_MOUSE_UP){
            if (!(ev.mouse_buttons & MOUSE_BUTTON_RIGHT)) rmb_drag=0;
        } else if (et==EVENT_KEY_DOWN){
            on_key(ev.key_char, ev.keycode);
        }

        /* animation advance */
        if (A.anim_active){ A.anim_t += 0.12f; if (A.anim_t>=1.0f){ A.anim_active=0; A.anim_apply_pending=0; set_gameover_if_done(); } }

        /* consume AI result */
        if (g_ai_ready && !A.anim_active){
            g_ai_ready=0;
            if (A.screen==SC_PLAY && A.mode==MODE_VS_CPU && A.result==RES_NONE){
                Move m=g_ai_move;
                if (m.from!=m.to) { do_play(m.from,m.to,m.promo?m.promo:QUEEN); }
            }
        }
        /* start AI when it is the computer's turn */
        if (A.screen==SC_PLAY && A.mode==MODE_VS_CPU && A.result==RES_NONE &&
            !A.anim_active && !g_ai_busy && !g_ai_ready && A.game.pos.side!=A.human_color){
            ai_start();
        }

        /* render 3D */
        gfx_render(&A, g_blit, A.vw);
        /* overlays */
        if (A.screen==SC_MENU) draw_menu();
        else if (A.screen==SC_HELP) draw_help();
        else if (A.screen==SC_PROMOTE){ draw_coords(); draw_hud(); draw_promote(); }
        else if (A.screen==SC_GAMEOVER){ draw_coords(); draw_gameover(); }
        else { draw_coords(); draw_hud(); }

        int w=A.vw,h=A.vh;
        syscall5(SYS_WIN_BLIT, win, 0, 0, (w&0xFFFF)|((h&0xFFFF)<<16), (long)g_blit);
        win_invalidate(win);
    }
    win_destroy(win);
    return 0;
}
