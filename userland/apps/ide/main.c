// Maytera Code Studio - a coding IDE with an integrated Python console (task #353)
// Userland GUI app for MayteraOS. Multi-file tabbed code editor + syntax
// highlighting + project file tree + integrated CPython (/APPS/PYTHON.ELF)
// console (Run + stateful REPL). Reuses the shared libc GUI + child-spawn/pipe
// plumbing and a worker thread so the UI stays responsive while Python runs.
#include "gui.h"
#include "syscall.h"
#include "string.h"
#include "stdlib.h"
#include "stdio.h"
#include "pthread.h"
#include "dirent.h"
#include "unistd.h"
#include "fcntl.h"

// ---------------------------------------------------------------- palette ---
#define C_BG        0x001E1E1E
#define C_PANEL     0x00252526
#define C_PANEL2    0x002D2D30
#define C_CHROME    0x00333337
#define C_BORDER    0x00141414
#define C_INK       0x00D4D4D4
#define C_DIM       0x00858585
#define C_ACCENT    0x00007ACC
#define C_ACCENT2   0x00094771
#define C_SEL       0x00264F78
#define C_CURLINE   0x00282828
#define C_GUTTER    0x00606066
#define C_TABBG     0x002D2D2D
#define C_TABACT    0x001E1E1E
// syntax
#define C_KEY   0x00569CD6
#define C_STR   0x00CE9178
#define C_COM   0x006A9955
#define C_NUM   0x00B5CEA8
#define C_DEF   0x00DCDCAA
#define C_ERRC  0x00F44747
#define C_OK    0x006A9955
#define C_WHITE 0x00FFFFFF

#define FW 8
#define FH 16

// key codes (from kernel)
#define K_UP 0x80
#define K_DOWN 0x81
#define K_LEFT 0x82
#define K_RIGHT 0x83
#define K_HOME 0x47
#define K_END 0x4F
#define K_PGUP 0x49
#define K_PGDN 0x51
#define K_DEL 0x53
#define K_BS 0x0E
#define K_ENTER 0x1C
#define K_F5 0x3F

// ---------------------------------------------------------------- buffer ----
#define MAXTABS 12
#define UNDO_MAX 64

typedef struct {
    char **lines;     // array of NUL-terminated line strings (no newline)
    int nlines, cap;
    char path[256];
    char name[64];
    int used;
    int dirty;
    int lang;         // 0 plain, 1 python, 2 c
    int cl, cc;       // cursor line, col
    int top;          // first visible line
    int leftcol;      // horizontal scroll
    int sel_on, al, ac; // selection anchor
    // undo/redo
    char *undo[UNDO_MAX]; int undo_n;
    char *redo[UNDO_MAX]; int redo_n;
    int last_type;    // coalesce consecutive typing into one undo group
} Buffer;

static Buffer tabs[MAXTABS];
static int ntabs = 0, curtab = 0;

// ---------------------------------------------------------------- globals ---
static int win = -1;
static int W = 1000, H = 680;
static int tree_visible = 1;
static int console_visible = 1;
static int console_tab = 0;       // 0 = Python Console (REPL), 1 = Output
static char *g_clip = 0;          // internal clipboard
static char status_msg[160] = "Maytera Code Studio ready";

// layout constants
#define MENUBAR_H 22
#define TOOLBAR_H 32
#define STATUS_H  22
#define TABBAR_H  24
#define TREE_W    210
#define CONSOLE_H 210

// menus
static int menu_open = -1;        // which top menu dropdown is open (-1 none)

// find bar
static int find_open = 0;
static char find_q[128] = "";

// modal prompt
static int g_modal = 0;

// ---------------------------------------------------------------- python ---
#define CAPMAX  (256*1024)
static pthread_mutex_t g_lock;
static char *g_cap;               // capture buffer for the current run
static int   g_cap_len;
static int   g_run_active = 0;
static int   g_run_pid = 0;
static int   g_run_exit = 0;
static int   g_run_done = 0;      // set by worker when finished (UI consumes)
static int   g_run_target = 0;    // 0 output pane, 1 console
static char  g_run_path[256];
static int   g_dirty = 0;         // worker asks UI to repaint

// console (REPL) persistent state
static char *g_con;               // console display text
static int   g_con_len, g_con_cap;
static char *g_sess;              // committed session source
static int   g_sess_len, g_sess_cap;
static int   g_con_prev = 0;      // baseline output length already shown
static char  g_pending_line[512]; // line awaiting commit after a console run
static int   g_out_scroll = 0;    // scroll for output/console panes
static char  con_input[512];
static int   con_input_len = 0;

// output pane text = g_cap (last run). scrollback offset shared g_out_scroll.

// ---------------------------------------------------------- small helpers --
static void set_status(const char *s){ int i=0; while(s[i]&&i<159){status_msg[i]=s[i];i++;} status_msg[i]=0; }

static void *xrealloc(void *p, int n){ void *q=realloc(p,n); return q; }

static void append_buf(char **buf, int *len, int *cap, const char *data, int n){
    if(*cap - *len < n + 1){
        int nc = (*cap? *cap:1024);
        while(nc - *len < n + 1) nc *= 2;
        *buf = xrealloc(*buf, nc); *cap = nc;
    }
    memcpy(*buf + *len, data, n); *len += n; (*buf)[*len]=0;
}

// ---------------------------------------------------------- buffer basics --
static void buf_free_lines(Buffer *b){
    if(b->lines){ for(int i=0;i<b->nlines;i++) free(b->lines[i]); free(b->lines); }
    b->lines=0; b->nlines=0; b->cap=0;
}
static void buf_add_line(Buffer *b, const char *s, int n){
    if(b->nlines>=b->cap){ b->cap = b->cap? b->cap*2:64; b->lines=xrealloc(b->lines,b->cap*sizeof(char*)); }
    char *l = malloc(n+1); memcpy(l,s,n); l[n]=0; b->lines[b->nlines++]=l;
}
static int detect_lang(const char *path){
    int n=strlen(path);
    if(n>=3 && !strcmp(path+n-3,".py")) return 1;
    if(n>=2 && (!strcmp(path+n-2,".c")||!strcmp(path+n-2,".h"))) return 2;
    return 0;
}
static const char *base_name(const char *p){ const char *s=p; for(const char*q=p;*q;q++) if(*q=='/') s=q+1; return s; }

// serialize buffer to one malloc'd string (for undo)
static char *buf_serialize(Buffer *b){
    int tot=1; for(int i=0;i<b->nlines;i++) tot+=strlen(b->lines[i])+1;
    char *s=malloc(tot); int p=0;
    for(int i=0;i<b->nlines;i++){ int ln=strlen(b->lines[i]); memcpy(s+p,b->lines[i],ln); p+=ln; if(i+1<b->nlines) s[p++]='\n'; }
    s[p]=0; return s;
}
static void buf_load_text(Buffer *b, const char *text){
    buf_free_lines(b);
    const char *p=text;
    while(1){ const char *nl=strchr(p,'\n'); if(!nl){ buf_add_line(b,p,strlen(p)); break; } buf_add_line(b,p,(int)(nl-p)); p=nl+1; }
    if(b->nlines==0) buf_add_line(b,"",0);
}

static void undo_clear(Buffer *b){
    for(int i=0;i<b->undo_n;i++) free(b->undo[i]); b->undo_n=0;
    for(int i=0;i<b->redo_n;i++) free(b->redo[i]); b->redo_n=0;
}
static void undo_push(Buffer *b){
    // free redo
    for(int i=0;i<b->redo_n;i++) free(b->redo[i]); b->redo_n=0;
    if(b->undo_n>=UNDO_MAX){ free(b->undo[0]); memmove(b->undo,b->undo+1,(UNDO_MAX-1)*sizeof(char*)); b->undo_n--; }
    b->undo[b->undo_n++]=buf_serialize(b);
}
static void do_undo(Buffer *b){
    if(b->undo_n<=0){ set_status("Nothing to undo"); return; }
    if(b->redo_n<UNDO_MAX) b->redo[b->redo_n++]=buf_serialize(b);
    char *t=b->undo[--b->undo_n];
    buf_load_text(b,t); free(t);
    if(b->cl>=b->nlines) b->cl=b->nlines-1; if(b->cc>(int)strlen(b->lines[b->cl])) b->cc=strlen(b->lines[b->cl]);
    b->dirty=1; b->last_type=0; set_status("Undo");
}
static void do_redo(Buffer *b){
    if(b->redo_n<=0){ set_status("Nothing to redo"); return; }
    if(b->undo_n<UNDO_MAX) b->undo[b->undo_n++]=buf_serialize(b);
    char *t=b->redo[--b->redo_n];
    buf_load_text(b,t); free(t);
    if(b->cl>=b->nlines) b->cl=b->nlines-1; if(b->cc>(int)strlen(b->lines[b->cl])) b->cc=strlen(b->lines[b->cl]);
    b->dirty=1; b->last_type=0; set_status("Redo");
}

// ---------------------------------------------------------- file load/save
static int load_file_into(Buffer *b, const char *path){
    int fd = sys_open(path, O_RDONLY);
    if(fd<0) return -1;
    char *data=0; int len=0, cap=0; char chunk[4096]; long n;
    while((n=sys_read(fd, chunk, sizeof chunk))>0){
        if(cap-len < (int)n+1){ cap = cap? cap*2 : 8192; while(cap-len<(int)n+1) cap*=2; data=xrealloc(data,cap); }
        memcpy(data+len,chunk,n); len+=n;
    }
    sys_close(fd);
    if(!data){ data=malloc(1); }
    data[len]=0;
    buf_free_lines(b);
    const char *p=data;
    while(1){ const char *nl=strchr(p,'\n'); if(!nl){ buf_add_line(b,p,strlen(p)); break;}
        int ln=(int)(nl-p); if(ln>0 && p[ln-1]=='\r') ln--; buf_add_line(b,p,ln); p=nl+1; }
    free(data);
    if(b->nlines==0) buf_add_line(b,"",0);
    strncpy(b->path,path,255); b->path[255]=0;
    strncpy(b->name,base_name(path),63); b->name[63]=0;
    b->lang=detect_lang(path); b->dirty=0; b->cl=b->cc=b->top=b->leftcol=0; b->sel_on=0;
    undo_clear(b);
    return 0;
}
static int save_buffer(Buffer *b){
    if(!b->path[0]) return -1;
    int fd = sys_open(b->path, O_WRONLY|O_CREAT|O_TRUNC);
    if(fd<0){ set_status("Save failed (open)"); return -1; }
    for(int i=0;i<b->nlines;i++){ int ln=strlen(b->lines[i]); if(ln) sys_write(fd,b->lines[i],ln); if(i+1<b->nlines) sys_write(fd,"\n",1); }
    sys_close(fd);
    b->dirty=0; char m[200]; snprintf(m,sizeof m,"Saved %s",b->name); set_status(m);
    return 0;
}

// ---------------------------------------------------------- tab management
static Buffer *cur(void){ return &tabs[curtab]; }
static int open_path_tab(const char *path){
    // already open?
    for(int i=0;i<ntabs;i++) if(tabs[i].used && !strcmp(tabs[i].path,path)){ curtab=i; return i; }
    if(ntabs>=MAXTABS){ set_status("Too many tabs open"); return -1; }
    Buffer *b=&tabs[ntabs]; memset(b,0,sizeof *b); b->used=1;
    if(load_file_into(b,path)<0){ set_status("Cannot open file"); return -1; }
    curtab=ntabs; ntabs++;
    char m[200]; snprintf(m,sizeof m,"Opened %s",b->name); set_status(m);
    return curtab;
}
static void new_untitled(void){
    if(ntabs>=MAXTABS){ set_status("Too many tabs open"); return; }
    Buffer *b=&tabs[ntabs]; memset(b,0,sizeof *b); b->used=1;
    buf_add_line(b,"",0); strcpy(b->name,"untitled"); b->path[0]=0; b->lang=1;
    curtab=ntabs; ntabs++; set_status("New file");
}
static void close_tab(int i){
    if(i<0||i>=ntabs) return;
    buf_free_lines(&tabs[i]); undo_clear(&tabs[i]);
    for(int j=i;j<ntabs-1;j++) tabs[j]=tabs[j+1];
    ntabs--; if(ntabs==0){ new_untitled(); } if(curtab>=ntabs) curtab=ntabs-1;
}

// ---------------------------------------------------------- file tree ------
#define MAXTREE 2048
typedef struct { char path[256]; char name[96]; int depth; int is_dir; int expanded; } TNode;
static TNode tree[MAXTREE];
static int ntree=0, tree_scroll=0;
static char proj_root[256]="/";


// insert children of node at index idx (a dir), returns count inserted
static int tree_load_children(int idx){
    TNode *n=&tree[idx];
    DIR *d=opendir(n->path); if(!d) return 0;
    // collect
    char names[256][96]; int types[256]; int cnt=0;
    struct dirent *e;
    while((e=readdir(d)) && cnt<256){
        if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
        strncpy(names[cnt],e->d_name,95); names[cnt][95]=0;
        types[cnt]= (e->d_type==DT_DIR); cnt++;
    }
    closedir(d);
    // sort: dirs first, then case-insensitive name
    for(int i=0;i<cnt;i++) for(int j=i+1;j<cnt;j++){
        int swap=0;
        if(types[j]&&!types[i]) swap=1;
        else if(types[j]==types[i]){ if(strcmp(names[j],names[i])<0) swap=1; }
        if(swap){ char tn[96]; strcpy(tn,names[i]); strcpy(names[i],names[j]); strcpy(names[j],tn); int tt=types[i];types[i]=types[j];types[j]=tt; }
    }
    if(ntree+cnt>=MAXTREE) cnt=MAXTREE-ntree-1;
    // shift down
    memmove(&tree[idx+1+cnt], &tree[idx+1], (ntree-(idx+1))*sizeof(TNode));
    for(int i=0;i<cnt;i++){
        TNode *c=&tree[idx+1+i]; memset(c,0,sizeof *c);
        c->depth=n->depth+1; c->is_dir=types[i]; c->expanded=0;
        strncpy(c->name,names[i],95);
        int pl=strlen(n->path); strncpy(c->path,n->path,255);
        if(pl && c->path[pl-1]!='/'){ c->path[pl]='/'; c->path[pl+1]=0; pl++; }
        strncat(c->path,names[i],255-strlen(c->path));
    }
    ntree+=cnt;
    return cnt;
}
static void tree_collapse(int idx){
    int d=tree[idx].depth; int j=idx+1;
    while(j<ntree && tree[j].depth>d) j++;
    int rem=j-(idx+1);
    if(rem>0){ memmove(&tree[idx+1],&tree[j],(ntree-j)*sizeof(TNode)); ntree-=rem; }
    tree[idx].expanded=0;
}
static void tree_init(const char *root){
    ntree=0; memset(&tree[0],0,sizeof(TNode));
    strncpy(tree[0].path,root,255); strncpy(tree[0].name,base_name(root),95);
    if(!tree[0].name[0]) strcpy(tree[0].name,"/");
    tree[0].depth=0; tree[0].is_dir=1; tree[0].expanded=1; ntree=1;
    tree_load_children(0);
    strncpy(proj_root,root,255);
}
static void tree_toggle(int idx){
    if(!tree[idx].is_dir) return;
    if(tree[idx].expanded) tree_collapse(idx);
    else { tree[idx].expanded=1; tree_load_children(idx); }
}

// ---------------------------------------------------------- python runner --
static void run_reset_capture(void){ g_cap_len=0; if(g_cap) g_cap[0]=0; }

static void *run_worker(void *arg){
    (void)arg;
    char *argv[3];
    argv[0]="/APPS/PYTHON.ELF"; argv[1]=g_run_path; argv[2]=0;
    int p[2];
    if(pipe(p)!=0){ pthread_mutex_lock(&g_lock); g_run_active=0; g_run_done=1; g_run_exit=-1; g_dirty=1; pthread_mutex_unlock(&g_lock); return 0; }
    int save1=dup(1), save2=dup(2);
    dup2(p[1],1); dup2(p[1],2); close(p[1]);
    int pid=sys_spawn_args("/APPS/PYTHON.ELF", argv, 2);
    dup2(save1,1); dup2(save2,2); close(save1); close(save2);
    pthread_mutex_lock(&g_lock); g_run_pid=pid; pthread_mutex_unlock(&g_lock);
    if(pid<=0){
        const char *m="[Failed to launch /APPS/PYTHON.ELF - is the Python runtime installed?]\n";
        pthread_mutex_lock(&g_lock); int ml=strlen(m); if(g_cap_len+ml<CAPMAX){memcpy(g_cap+g_cap_len,m,ml);g_cap_len+=ml;g_cap[g_cap_len]=0;} g_run_active=0; g_run_done=1; g_run_exit=-1; g_dirty=1; pthread_mutex_unlock(&g_lock);
        close(p[0]); return 0;
    }
    char buf[2048]; long n;
    while((n=read(p[0],buf,sizeof buf))>0){
        pthread_mutex_lock(&g_lock);
        if(g_cap_len + n < CAPMAX){ memcpy(g_cap+g_cap_len,buf,n); g_cap_len+=n; g_cap[g_cap_len]=0; }
        g_dirty=1;
        pthread_mutex_unlock(&g_lock);
    }
    int st=0; sys_waitpid(pid,&st,0);
    close(p[0]);
    pthread_mutex_lock(&g_lock);
    g_run_exit=st; g_run_active=0; g_run_done=1; g_dirty=1;
    pthread_mutex_unlock(&g_lock);
    return 0;
}
static void start_run(const char *path, int target){
    if(g_run_active){ set_status("A program is already running"); return; }
    run_reset_capture();
    strncpy(g_run_path,path,255); g_run_path[255]=0;
    g_run_target=target; g_run_active=1; g_run_done=0; g_run_exit=0; g_run_pid=0;
    pthread_t t; pthread_create(&t,0,run_worker,0); pthread_detach(t);
}
static void stop_run(void){
    if(!g_run_active){ return; }
    pthread_mutex_lock(&g_lock); int pid=g_run_pid; pthread_mutex_unlock(&g_lock);
    if(pid>0){ syscall2(SYS_KILL, pid, 9); set_status("Sent stop to running program"); }
}
// Run the active file with Python (Output pane)
static void run_active_file(void){
    Buffer *b=cur();
    if(!b->path[0]){ set_status("Save the file before running"); return; }
    if(b->dirty) save_buffer(b);
    if(b->lang!=1){
        run_reset_capture();
        const char *m="[Only Python files can be run on-device. MayteraOS has no C compiler yet\n (#376). This file was saved; open a .py file to Run.]\n";
        pthread_mutex_lock(&g_lock); int ml=strlen(m); if(g_cap_len+ml<CAPMAX){memcpy(g_cap+g_cap_len,m,ml);g_cap_len+=ml;g_cap[g_cap_len]=0;} pthread_mutex_unlock(&g_lock);
        console_visible=1; console_tab=1; g_out_scroll=0; set_status("Not a Python file");
        return;
    }
    console_visible=1; console_tab=1; g_out_scroll=0;
    char m[200]; snprintf(m,sizeof m,"Running %s ...",b->name); set_status(m);
    start_run(b->path, 0);
}
// Console REPL: submit a line
static void console_submit(const char *line){
    if(g_run_active){ set_status("Busy - a program is running"); return; }
    // echo prompt
    char pr[540]; snprintf(pr,sizeof pr,">>> %s\n",line);
    append_buf(&g_con,&g_con_len,&g_con_cap,pr,strlen(pr));
    // build session file = committed session + this line
    int fd=sys_open("/IDECON.PY", O_WRONLY|O_CREAT|O_TRUNC);
    if(fd<0){ append_buf(&g_con,&g_con_len,&g_con_cap,"[cannot write session file]\n",28); return; }
    if(g_sess_len) sys_write(fd,g_sess,g_sess_len);
    if(g_sess_len) sys_write(fd,"\n",1);
    sys_write(fd,line,strlen(line));
    sys_close(fd);
    strncpy(g_pending_line,line,511); g_pending_line[511]=0;
    start_run("/IDECON.PY", 1);
}
// called from UI loop when a console run finished
static void console_finish(void){
    pthread_mutex_lock(&g_lock);
    int caplen=g_cap_len; int ex=g_run_exit;
    // delta beyond baseline already shown
    int start = (g_con_prev<=caplen)? g_con_prev : 0;
    if(caplen>start) append_buf(&g_con,&g_con_len,&g_con_cap,g_cap+start,caplen-start);
    pthread_mutex_unlock(&g_lock);
    if(ex==0){
        // commit the line to the session, advance baseline
        if(g_sess_len){ append_buf(&g_sess,&g_sess_len,&g_sess_cap,"\n",1); }
        append_buf(&g_sess,&g_sess_len,&g_sess_cap,g_pending_line,strlen(g_pending_line));
        g_con_prev=caplen;
    }
    // ensure a trailing newline in display
    if(g_con_len && g_con[g_con_len-1]!='\n') append_buf(&g_con,&g_con_len,&g_con_cap,"\n",1);
    g_out_scroll=0;
}

// ---------------------------------------------------------- editing ops ----
static void ins_char(Buffer *b, char c){
    if(!b->last_type){ undo_push(b); b->last_type=1; }
    char *l=b->lines[b->cl]; int ln=strlen(l);
    char *nl=malloc(ln+2); memcpy(nl,l,b->cc); nl[b->cc]=c; memcpy(nl+b->cc+1,l+b->cc,ln-b->cc); nl[ln+1]=0;
    free(b->lines[b->cl]); b->lines[b->cl]=nl; b->cc++; b->dirty=1;
}
static void ins_newline(Buffer *b){
    undo_push(b); b->last_type=0;
    char *l=b->lines[b->cl]; int ln=strlen(l);
    // auto-indent: copy leading whitespace
    int ind=0; while(ind<b->cc && (l[ind]==' '||l[ind]=='\t')) ind++;
    char *right=malloc(ln-b->cc+ind+1);
    for(int i=0;i<ind;i++) right[i]=l[i];
    memcpy(right+ind, l+b->cc, ln-b->cc); right[ind+ln-b->cc]=0;
    char *left=malloc(b->cc+1); memcpy(left,l,b->cc); left[b->cc]=0;
    free(b->lines[b->cl]); b->lines[b->cl]=left;
    if(b->nlines>=b->cap){ b->cap=b->cap?b->cap*2:64; b->lines=xrealloc(b->lines,b->cap*sizeof(char*)); }
    memmove(&b->lines[b->cl+2], &b->lines[b->cl+1], (b->nlines-b->cl-1)*sizeof(char*));
    b->lines[b->cl+1]=right; b->nlines++;
    b->cl++; b->cc=ind; b->dirty=1;
}
static void do_backspace(Buffer *b){
    if(b->cc>0){
        if(!b->last_type){ undo_push(b); b->last_type=1; }
        char *l=b->lines[b->cl]; int ln=strlen(l);
        memmove(l+b->cc-1, l+b->cc, ln-b->cc+1); b->cc--;
    } else if(b->cl>0){
        undo_push(b); b->last_type=0;
        char *prev=b->lines[b->cl-1]; int pl=strlen(prev); char *curl=b->lines[b->cl]; int cln=strlen(curl);
        char *m=malloc(pl+cln+1); memcpy(m,prev,pl); memcpy(m+pl,curl,cln); m[pl+cln]=0;
        free(b->lines[b->cl-1]); b->lines[b->cl-1]=m; free(b->lines[b->cl]);
        memmove(&b->lines[b->cl],&b->lines[b->cl+1],(b->nlines-b->cl-1)*sizeof(char*)); b->nlines--;
        b->cl--; b->cc=pl;
    }
    b->dirty=1;
}
static void do_delete(Buffer *b){
    char *l=b->lines[b->cl]; int ln=strlen(l);
    if(b->cc<ln){ if(!b->last_type){undo_push(b);b->last_type=1;} memmove(l+b->cc,l+b->cc+1,ln-b->cc); b->dirty=1; }
    else if(b->cl<b->nlines-1){ undo_push(b); b->last_type=0;
        char *nx=b->lines[b->cl+1]; int nl=strlen(nx);
        char *m=malloc(ln+nl+1); memcpy(m,l,ln); memcpy(m+ln,nx,nl); m[ln+nl]=0;
        free(b->lines[b->cl]); b->lines[b->cl]=m; free(b->lines[b->cl+1]);
        memmove(&b->lines[b->cl+1],&b->lines[b->cl+2],(b->nlines-b->cl-2)*sizeof(char*)); b->nlines--; b->dirty=1; }
}
static void ins_text(Buffer *b, const char *t){
    undo_push(b); b->last_type=0;
    for(const char *p=t;*p;p++){ if(*p=='\n'){
        // split (reuse newline but without extra undo)
        char *l=b->lines[b->cl]; int ln=strlen(l);
        char *right=malloc(ln-b->cc+1); memcpy(right,l+b->cc,ln-b->cc); right[ln-b->cc]=0;
        char *left=malloc(b->cc+1); memcpy(left,l,b->cc); left[b->cc]=0;
        free(b->lines[b->cl]); b->lines[b->cl]=left;
        if(b->nlines>=b->cap){ b->cap=b->cap?b->cap*2:64; b->lines=xrealloc(b->lines,b->cap*sizeof(char*)); }
        memmove(&b->lines[b->cl+2],&b->lines[b->cl+1],(b->nlines-b->cl-1)*sizeof(char*));
        b->lines[b->cl+1]=right; b->nlines++; b->cl++; b->cc=0;
    } else { char *l=b->lines[b->cl]; int ln=strlen(l);
        char *nl=malloc(ln+2); memcpy(nl,l,b->cc); nl[b->cc]=*p; memcpy(nl+b->cc+1,l+b->cc,ln-b->cc); nl[ln+1]=0;
        free(b->lines[b->cl]); b->lines[b->cl]=nl; b->cc++; }
    }
    b->dirty=1;
}
// selection helpers
static void sel_bounds(Buffer *b, int *l0,int *c0,int *l1,int *c1){
    int al=b->al,ac=b->ac,bl=b->cl,bc=b->cc;
    if(al<bl || (al==bl&&ac<=bc)){ *l0=al;*c0=ac;*l1=bl;*c1=bc; } else { *l0=bl;*c0=bc;*l1=al;*c1=ac; }
}
static char *sel_extract(Buffer *b){
    int l0,c0,l1,c1; sel_bounds(b,&l0,&c0,&l1,&c1);
    char *out=0; int len=0,cap=0;
    for(int i=l0;i<=l1;i++){ char *l=b->lines[i]; int ln=strlen(l); int s=(i==l0)?c0:0; int e=(i==l1)?c1:ln;
        append_buf(&out,&len,&cap,l+s,e-s); if(i<l1) append_buf(&out,&len,&cap,"\n",1); }
    if(!out){ out=malloc(1); out[0]=0; } return out;
}
static void sel_delete(Buffer *b){
    int l0,c0,l1,c1; sel_bounds(b,&l0,&c0,&l1,&c1);
    if(l0==l1&&c0==c1) return;
    undo_push(b); b->last_type=0;
    char *ll=b->lines[l0]; int lln=strlen(ll); char *rl=b->lines[l1]; int rln=strlen(rl);
    char *m=malloc(c0 + (rln-c1) +1); memcpy(m,ll,c0); memcpy(m+c0, rl+c1, rln-c1); m[c0+rln-c1]=0;
    for(int i=l0;i<=l1;i++) free(b->lines[i]);
    b->lines[l0]=m;
    int rem=l1-l0;
    if(rem>0){ memmove(&b->lines[l0+1],&b->lines[l1+1],(b->nlines-l1-1)*sizeof(char*)); b->nlines-=rem; }
    b->cl=l0; b->cc=c0; b->sel_on=0; b->dirty=1;
}

// ---------------------------------------------------------- syntax draw ----
static int is_kw_py(const char *w,int n){
    static const char *k[]={"def","class","return","if","elif","else","for","while","break","continue",
        "import","from","as","pass","with","try","except","finally","raise","in","is","not","and","or",
        "None","True","False","lambda","global","nonlocal","yield","assert","del","await","async",0};
    for(int i=0;k[i];i++) if((int)strlen(k[i])==n && !strncmp(k[i],w,n)) return 1; return 0;
}
static int is_kw_c(const char *w,int n){
    static const char *k[]={"int","char","void","float","double","long","short","unsigned","signed",
        "struct","union","enum","typedef","static","const","return","if","else","for","while","do",
        "switch","case","break","continue","default","sizeof","goto","extern","volatile","register","inline",0};
    for(int i=0;k[i];i++) if((int)strlen(k[i])==n && !strncmp(k[i],w,n)) return 1; return 0;
}
// draw one code line with highlighting; returns nothing. x,y in pixels. clip to xmax.
static void draw_code_line(const char *l, int lang, int x, int y, int leftcol, int xmax){
    int ln=strlen(l);
    int col=0; // logical column
    char run[512]; int rl=0; uint32_t rc=C_INK; int rx=x;
    #define FLUSH() do{ if(rl>0){ run[rl]=0; gui_draw_text(win,rx,y,run,rc); } rl=0; }while(0)
    int i=0;
    // comment for whole rest?
    while(i<ln){
        char c=l[i];
        uint32_t color=C_INK;
        int adv=1; char tok[512]; int tl=0;
        if(lang==1 && c=='#'){ // comment to EOL
            FLUSH();
            int startcol=col; int px=x+(startcol-leftcol)*FW;
            if(px<xmax) gui_draw_text(win,px,y,l+i,C_COM);
            return;
        }
        if(lang==2 && c=='/'&&i+1<ln&&l[i+1]=='/'){ FLUSH(); int px=x+(col-leftcol)*FW; if(px<xmax) gui_draw_text(win,px,y,l+i,C_COM); return; }
        if(c=='"'||c=='\''){ // string
            FLUSH(); char q=c; tok[tl++]=c; i++; col++;
            while(i<ln&&tl<510){ tok[tl++]=l[i]; if(l[i]=='\\'&&i+1<ln){ i++;col++; if(tl<510)tok[tl++]=l[i]; } else if(l[i]==q){ i++;col++; break;} i++; col++; }
            tok[tl]=0; int px=x+((col-tl)-leftcol)*FW; if(px+tl*FW>=x && px<xmax) gui_draw_text(win,px,y,tok,C_STR);
            continue;
        }
        if((c>='0'&&c<='9')){ FLUSH(); int st=col; while(i<ln&&((l[i]>='0'&&l[i]<='9')||l[i]=='.'||l[i]=='x'||(l[i]>='a'&&l[i]<='f')||(l[i]>='A'&&l[i]<='F'))){ tok[tl++]=l[i]; i++; col++; } tok[tl]=0; int px=x+(st-leftcol)*FW; if(px<xmax) gui_draw_text(win,px,y,tok,C_NUM); continue; }
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'){ // word
            int st=col; int si=i; while(i<ln&&((l[i]>='a'&&l[i]<='z')||(l[i]>='A'&&l[i]<='Z')||(l[i]>='0'&&l[i]<='9')||l[i]=='_')){ i++; col++; }
            int wl=i-si; int kw = (lang==1)?is_kw_py(l+si,wl):(lang==2)?is_kw_c(l+si,wl):0;
            // def/class name after keyword
            FLUSH();
            memcpy(tok,l+si,wl); tok[wl]=0;
            int px=x+(st-leftcol)*FW; if(px<xmax){
                uint32_t wc = kw? C_KEY : C_INK;
                gui_draw_text(win,px,y,tok,wc);
            }
            (void)color; continue;
        }
        // plain char - accumulate in run
        if(col>=leftcol){ if(rl==0){ rx=x+(col-leftcol)*FW; } if(rl<510) run[rl++]=c; }
        else { FLUSH(); }
        i+=adv; col+=adv;
        (void)adv;(void)tok;(void)tl;
    }
    FLUSH();
    #undef FLUSH
}

// ---------------------------------------------------------- geometry -------
static int editor_x(void){ return tree_visible?TREE_W:0; }
static int editor_y(void){ return MENUBAR_H+TOOLBAR_H+TABBAR_H + (find_open?26:0); }
static int body_bottom(void){ return H-STATUS_H; }
static int console_y(void){ return body_bottom()-(console_visible?CONSOLE_H:0); }
static int editor_w(void){ return W-editor_x(); }
static int editor_h(void){ return console_y()-editor_y(); }
static int edit_rows(void){ int h=editor_h(); return h>0? h/FH : 1; }
static int gutter_w(void){ Buffer*b=cur(); int d=1,n=b->nlines; while(n>=10){n/=10;d++;} return (d+2)*FW; }

// ---------------------------------------------------------- rendering ------
static void draw_button(int x,int y,int w,int h,const char*label,int hot,uint32_t fg){
    gui_fill_rect(win,x,y,w,h, hot?C_ACCENT2:C_CHROME);
    gui_draw_rect(win,x,y,w,h,C_BORDER);
    int tx=x+(w-(int)strlen(label)*FW)/2; int ty=y+(h-FH)/2;
    gui_draw_text(win,tx,ty,label,fg);
}

static const char *g_menus[5]={"File","Edit","View","Run","Help"};
static void draw_menubar(void){
    gui_fill_rect(win,0,0,W,MENUBAR_H,C_CHROME);
    gui_fill_rect(win,0,MENUBAR_H-1,W,1,C_BORDER);
    int x=8;
    for(int i=0;i<5;i++){ int w=strlen(g_menus[i])*FW+16; if(menu_open==i) gui_fill_rect(win,x,0,w,MENUBAR_H,C_ACCENT2);
        gui_draw_text(win,x+8,(MENUBAR_H-FH)/2,g_menus[i],C_INK); x+=w; }
    // title on the right
    const char *t="Maytera Code Studio"; gui_draw_text(win,W-strlen(t)*FW-10,(MENUBAR_H-FH)/2,t,C_DIM);
}
// dropdown menu items per top menu
static const char *mi_file[]={"New File","Open Folder...","Save","Save As...","Close Tab","Exit",0};
static const char *mi_edit[]={"Undo","Redo","Cut","Copy","Paste","Find","Replace All",0};
static const char *mi_view[]={"Toggle File Tree","Toggle Console","Console: REPL","Console: Output",0};
static const char *mi_run[]={"Run File (F5)","Stop","Clear Console","New REPL Session",0};
static const char *mi_help[]={"About",0};
static const char **menu_items(int m){ switch(m){case 0:return mi_file;case 1:return mi_edit;case 2:return mi_view;case 3:return mi_run;default:return mi_help;} }
static int menu_x(int m){ int x=8; for(int i=0;i<m;i++) x+=strlen(g_menus[i])*FW+16; return x; }
static void draw_dropdown(void){
    if(menu_open<0) return;
    const char **it=menu_items(menu_open); int n=0; while(it[n]) n++;
    int mw=0; for(int i=0;i<n;i++){ int w=strlen(it[i])*FW; if(w>mw)mw=w; } mw+=24;
    int x=menu_x(menu_open), y=MENUBAR_H;
    gui_fill_rect(win,x,y,mw,n*20+4,C_PANEL2); gui_draw_rect(win,x,y,mw,n*20+4,C_BORDER);
    for(int i=0;i<n;i++) gui_draw_text(win,x+12,y+4+i*20+2,it[i],C_INK);
}
static void draw_toolbar(void){
    int y=MENUBAR_H; gui_fill_rect(win,0,y,W,TOOLBAR_H,C_PANEL2); gui_fill_rect(win,0,y+TOOLBAR_H-1,W,1,C_BORDER);
    int x=8, by=y+4, bh=TOOLBAR_H-8;
    draw_button(x,by,52,bh,"New",0,C_INK); x+=56;
    draw_button(x,by,56,bh,"Open",0,C_INK); x+=60;
    draw_button(x,by,56,bh,"Save",0,C_INK); x+=60;
    x+=8;
    draw_button(x,by,64,bh,"Run",0,C_OK); x+=68;
    draw_button(x,by,60,bh,"Stop",0,g_run_active?C_ERRC:C_DIM); x+=64;
    x+=8;
    draw_button(x,by,60,bh,"Find",0,C_INK); x+=64;
    // run indicator
    if(g_run_active){ const char*r="running"; gui_draw_text(win,W-strlen(r)*FW-12,by+(bh-FH)/2,r,C_OK); }
}
// toolbar hit test -> action id
static int toolbar_hit(int mx,int my){
    int y=MENUBAR_H; if(my<y+4||my>y+TOOLBAR_H-4) return -1;
    int x=8, bh=TOOLBAR_H-8; (void)bh;
    struct{int w;int id;} bs[]={{52,1},{56,-1},{56,2},{-1,-1},{56,3},{-1,-1},{64,4},{60,5},{-1,-1},{60,6}};
    // simpler explicit boxes
    int bx=8;
    if(mx>=bx&&mx<bx+52)return 1; bx+=56;
    if(mx>=bx&&mx<bx+56)return 2; bx+=60; // open
    if(mx>=bx&&mx<bx+56)return 3; bx+=60; // save
    bx+=8;
    if(mx>=bx&&mx<bx+64)return 4; bx+=68; // run
    if(mx>=bx&&mx<bx+60)return 5; bx+=64; // stop
    bx+=8;
    if(mx>=bx&&mx<bx+60)return 6; // find
    (void)bs; return -1;
}
static void draw_tabbar(void){
    int y=MENUBAR_H+TOOLBAR_H; int x=editor_x();
    gui_fill_rect(win,x,y,W-x,TABBAR_H,C_TABBG);
    for(int i=0;i<ntabs;i++){
        char lbl[80]; snprintf(lbl,sizeof lbl,"%s%s", tabs[i].name, tabs[i].dirty?" *":"");
        int w=strlen(lbl)*FW+28;
        gui_fill_rect(win,x,y,w,TABBAR_H, i==curtab?C_TABACT:C_TABBG);
        if(i==curtab) gui_fill_rect(win,x,y,w,2,C_ACCENT);
        gui_draw_text(win,x+8,y+(TABBAR_H-FH)/2,lbl, i==curtab?C_INK:C_DIM);
        gui_draw_text(win,x+w-16,y+(TABBAR_H-FH)/2,"x",C_DIM);
        gui_draw_rect(win,x,y,w,TABBAR_H,C_BORDER);
        x+=w; if(x>W) break;
    }
}
static void draw_tree(void){
    if(!tree_visible) return;
    int y0=MENUBAR_H+TOOLBAR_H; int h=body_bottom()-y0;
    gui_fill_rect(win,0,y0,TREE_W,h,C_PANEL);
    gui_fill_rect(win,TREE_W-1,y0,1,h,C_BORDER);
    gui_draw_text(win,8,y0+4,"EXPLORER",C_DIM);
    int rows=(h-24)/18; int yy=y0+24;
    for(int i=tree_scroll;i<ntree && i<tree_scroll+rows;i++){
        TNode *n=&tree[i]; int ind=8+n->depth*12;
        uint32_t col = n->is_dir?0x00C5C5A0:C_INK;
        if(n->is_dir) gui_draw_text(win,ind,yy, n->expanded?"v":">", C_DIM);
        char nm[100]; snprintf(nm,sizeof nm,"%s",n->name);
        gui_draw_text(win,ind+12,yy,nm,col);
        yy+=18;
    }
}
static int tree_row_at(int my){
    int y0=MENUBAR_H+TOOLBAR_H; int yy=y0+24; if(my<yy) return -1;
    int idx=tree_scroll+(my-yy)/18; if(idx>=0&&idx<ntree) return idx; return -1;
}
static void draw_editor(void){
    Buffer *b=cur();
    int ex=editor_x(), ey=editor_y(), ew=editor_w(), eh=editor_h();
    gui_fill_rect(win,ex,ey,ew,eh,C_BG);
    int gw=gutter_w(); int tx=ex+gw+4; int xmax=ex+ew;
    int rows=edit_rows();
    // keep cursor visible
    if(b->cl<b->top) b->top=b->cl; if(b->cl>=b->top+rows) b->top=b->cl-rows+1;
    int viscols=(ew-gw-8)/FW; if(viscols<1)viscols=1;
    if(b->cc<b->leftcol) b->leftcol=b->cc; if(b->cc>=b->leftcol+viscols) b->leftcol=b->cc-viscols+1;
    // selection bounds
    int sl0=0,sc0=0,sl1=0,sc1=0; if(b->sel_on) sel_bounds(b,&sl0,&sc0,&sl1,&sc1);
    for(int r=0;r<rows;r++){
        int li=b->top+r; if(li>=b->nlines) break;
        int y=ey+r*FH;
        if(li==b->cl) gui_fill_rect(win,ex+gw,y,ew-gw,FH,C_CURLINE);
        // selection highlight
        if(b->sel_on && li>=sl0 && li<=sl1){
            int s=(li==sl0)?sc0:0; int e=(li==sl1)?sc1:(int)strlen(b->lines[li]);
            int sx=tx+(s-b->leftcol)*FW; int w=(e-s)*FW; if(w<2 && li<sl1) w=FW; // show empty-line sel
            if(sx<xmax) gui_fill_rect(win,sx,y,w,FH,C_SEL);
        }
        // gutter number
        char ln[16]; snprintf(ln,sizeof ln,"%d",li+1);
        gui_draw_text(win,ex+gw-4-(int)strlen(ln)*FW,y,ln,C_GUTTER);
        draw_code_line(b->lines[li], b->lang, tx, y, b->leftcol, xmax);
        // cursor
        if(li==b->cl){ int cx=tx+(b->cc-b->leftcol)*FW; if(cx>=tx&&cx<xmax) gui_fill_rect(win,cx,y,2,FH,C_INK); }
    }
    // gutter divider
    gui_fill_rect(win,ex+gw,ey,1,eh,C_PANEL2);
}
static void draw_console(void){
    if(!console_visible) return;
    int x=editor_x(), y=console_y(), w=W-x, h=CONSOLE_H;
    gui_fill_rect(win,x,y,w,h,C_PANEL);
    gui_fill_rect(win,x,y,w,1,C_BORDER);
    // tabs
    const char *t0="Python Console", *t1="Output";
    int w0=strlen(t0)*FW+20, w1=strlen(t1)*FW+20;
    gui_fill_rect(win,x,y+1,w0,20, console_tab==0?C_PANEL2:C_PANEL);
    gui_fill_rect(win,x+w0,y+1,w1,20, console_tab==1?C_PANEL2:C_PANEL);
    if(console_tab==0) gui_fill_rect(win,x,y+1,w0,2,C_ACCENT); else gui_fill_rect(win,x+w0,y+1,w1,2,C_ACCENT);
    gui_draw_text(win,x+10,y+4,t0, console_tab==0?C_INK:C_DIM);
    gui_draw_text(win,x+w0+10,y+4,t1, console_tab==1?C_INK:C_DIM);
    int inputh = (console_tab==0)?FH+6:0;
    int texty=y+24, texth=h-24-inputh;
    // pick text buffer
    char *txt; int tlen;
    pthread_mutex_lock(&g_lock);
    if(console_tab==0){ txt=g_con; tlen=g_con_len; } else { txt=g_cap; tlen=g_cap_len; }
    // render last N lines fitting, honoring g_out_scroll (lines from bottom)
    int rows=texth/FH;
    // count total lines
    int total=1; for(int i=0;i<tlen;i++) if(txt&&txt[i]=='\n') total++;
    int firstline = total-rows-g_out_scroll; if(firstline<0) firstline=0;
    // walk to firstline
    int line=0; int i=0; if(txt){ while(i<tlen && line<firstline){ if(txt[i]=='\n') line++; i++; } }
    int ry=texty; int drawn=0;
    char lb[1024]; int lbl2=0;
    if(txt) for(; i<=tlen && drawn<rows; i++){
        if(i==tlen || txt[i]=='\n'){ lb[lbl2]=0; gui_draw_text(win,x+8,ry,lb,C_INK); lbl2=0; ry+=FH; drawn++; if(i==tlen) break; }
        else if(lbl2<1023 && txt[i]!='\r') lb[lbl2++]=txt[i];
    }
    pthread_mutex_unlock(&g_lock);
    // input line for REPL
    if(console_tab==0){
        int iy=y+h-inputh; gui_fill_rect(win,x,iy,w,inputh,C_BG);
        gui_draw_text(win,x+4,iy+3,">>>",C_OK);
        gui_draw_text(win,x+4+4*FW,iy+3,con_input,C_INK);
        gui_fill_rect(win,x+4+(4+con_input_len)*FW,iy+3,2,FH,C_INK);
    }
}
static void draw_statusbar(void){
    Buffer*b=cur(); int y=H-STATUS_H;
    gui_fill_rect(win,0,y,W,STATUS_H, g_run_active?C_ACCENT2:C_ACCENT);
    char left[220]; const char *lang=b->lang==1?"Python":b->lang==2?"C":"Text";
    snprintf(left,sizeof left," %s   Ln %d, Col %d   %s%s", b->name, b->cl+1, b->cc+1, lang, b->dirty?"   *modified":"");
    gui_draw_text(win,4,y+3,left,C_WHITE);
    gui_draw_text(win,W-strlen(status_msg)*FW-8,y+3,status_msg,C_WHITE);
}
static void draw_find(void){
    if(!find_open) return;
    int y=MENUBAR_H+TOOLBAR_H+TABBAR_H; int x=editor_x();
    gui_fill_rect(win,x,y,editor_w(),26,C_PANEL2); gui_fill_rect(win,x,y+25,editor_w(),1,C_BORDER);
    gui_draw_text(win,x+8,y+5,"Find:",C_DIM);
    gui_fill_rect(win,x+56,y+3,300,20,C_BG); gui_draw_rect(win,x+56,y+3,300,20,C_BORDER);
    gui_draw_text(win,x+62,y+5,find_q,C_INK);
    gui_draw_text(win,x+370,y+5,"Enter=next  Esc=close",C_DIM);
}
static void render(void){
    gui_fill_rect(win,0,0,W,H,C_BG);
    draw_tree();
    draw_editor();
    draw_find();
    draw_console();
    draw_tabbar();
    draw_toolbar();
    draw_menubar();
    draw_dropdown();
    draw_statusbar();
    gui_invalidate(win);
}

// ---------------------------------------------------------- modal prompt ---
// simple centered text-input modal; returns 1 on OK, 0 on cancel
static int prompt_modal(const char *title, const char *initial, char *out, int outsz){
    char buf[512]; strncpy(buf,initial?initial:"",511); buf[511]=0; int len=strlen(buf);
    g_modal=1;
    while(1){
        int bw=520, bh=120; int bx=(W-bw)/2, by=(H-bh)/2;
        render();
        gui_fill_rect(win,0,0,W,H,0); // dim not available; draw box over
        render();
        gui_fill_rect(win,bx,by,bw,bh,C_PANEL2); gui_draw_rect(win,bx,by,bw,bh,C_ACCENT);
        gui_draw_text(win,bx+16,by+12,title,C_INK);
        gui_fill_rect(win,bx+16,by+40,bw-32,22,C_BG); gui_draw_rect(win,bx+16,by+40,bw-32,22,C_BORDER);
        gui_draw_text(win,bx+20,by+43,buf,C_INK);
        gui_fill_rect(win,bx+20+len*FW,by+43,2,FH,C_INK);
        draw_button(bx+bw-180,by+bh-32,80,24,"OK",1,C_WHITE);
        draw_button(bx+bw-90,by+bh-32,72,24,"Cancel",0,C_INK);
        gui_invalidate(win);
        gui_event_t ev; int t=gui_get_event(win,&ev,-1);
        if(t==EVENT_KEY_DOWN){
            char c=ev.key_char; uint32_t kc=ev.keycode;
            if(c==27){ g_modal=0; return 0; }
            if(kc==K_ENTER||c=='\n'||c=='\r'){ strncpy(out,buf,outsz-1); out[outsz-1]=0; g_modal=0; return 1; }
            if(c=='\b'||kc==K_BS){ if(len>0) buf[--len]=0; }
            else if(c>=32&&c<127&&len<510){ buf[len++]=c; buf[len]=0; }
        } else if(t==EVENT_MOUSE_DOWN){
            int bw2=520,bh2=120,bx2=(W-bw2)/2,by2=(H-bh2)/2;
            if(ev.mouse_y>=by2+bh2-32&&ev.mouse_y<by2+bh2-8){
                if(ev.mouse_x>=bx2+bw2-180&&ev.mouse_x<bx2+bw2-100){ strncpy(out,buf,outsz-1); out[outsz-1]=0; g_modal=0; return 1; }
                if(ev.mouse_x>=bx2+bw2-90&&ev.mouse_x<bx2+bw2-18){ g_modal=0; return 0; }
            }
        } else if(t==EVENT_WINDOW_CLOSE){ g_modal=0; return 0; }
    }
}

// ---------------------------------------------------------- find -----------
static void find_next(void){
    Buffer*b=cur(); if(!find_q[0]) return;
    int sl=b->cl, sc=b->cc+1;
    for(int pass=0;pass<2;pass++){
        for(int li=(pass?0:sl); li<b->nlines; li++){
            const char *l=b->lines[li]; int from=(li==sl&&!pass)?sc:0;
            const char *hit=strstr(l+ (from<= (int)strlen(l)?from:strlen(l)), find_q);
            if(hit){ b->cl=li; b->cc=(int)(hit-l); b->sel_on=1; b->al=li; b->ac=b->cc; b->cc+=strlen(find_q); set_status("Found"); return; }
            if(pass && li==sl) break;
        }
        sl=0;
    }
    set_status("Not found");
}
static void replace_all(const char *from,const char *to){
    Buffer*b=cur(); if(!from[0])return; int count=0; int fl=strlen(from), tl=strlen(to);
    undo_push(b); b->last_type=0;
    for(int li=0;li<b->nlines;li++){
        char *l=b->lines[li];
        for(int i=0;(l=b->lines[li]),l[i];){
            if(!strncmp(l+i,from,fl)){ int ol=strlen(l); char *nl=malloc(ol-fl+tl+1);
                memcpy(nl,l,i); memcpy(nl+i,to,tl); strcpy(nl+i+tl,l+i+fl); free(b->lines[li]); b->lines[li]=nl; i+=tl; count++; }
            else i++;
        }
    }
    char m[80]; snprintf(m,sizeof m,"Replaced %d occurrence(s)",count); set_status(m); if(count)b->dirty=1;
}

// ---------------------------------------------------------- cursor move ----
static void clampc(Buffer*b){ if(b->cl<0)b->cl=0; if(b->cl>=b->nlines)b->cl=b->nlines-1; int ln=strlen(b->lines[b->cl]); if(b->cc>ln)b->cc=ln; if(b->cc<0)b->cc=0; }
static void move(Buffer*b,int dl,int dc,int ext){
    if(ext && !b->sel_on){ b->sel_on=1; b->al=b->cl; b->ac=b->cc; }
    if(!ext) b->sel_on=0;
    if(dc==-99){ b->cc=0; }
    else if(dc==99){ b->cc=strlen(b->lines[b->cl]); }
    else { b->cc+=dc; b->cl+=dl; }
    if(dc==-1 && b->cc<0){ if(b->cl>0){ b->cl--; b->cc=strlen(b->lines[b->cl]); } else b->cc=0; }
    if(dc==1){ int ln0= (b->cl<b->nlines)?(int)strlen(b->lines[b->cl>=0?b->cl:0]):0; if(b->cc>ln0){ if(b->cl<b->nlines-1){ b->cl++; b->cc=0; } } }
    clampc(b);
}

// ---------------------------------------------------------- menu actions ---
static void action_open_folder(void){
    char path[256]; if(prompt_modal("Open folder (path):",proj_root,path,256)){ tree_init(path); tree_visible=1; set_status("Folder opened"); }
}
static void action_save_as(void){
    char path[256]; Buffer*b=cur(); if(prompt_modal("Save As (path):", b->path[0]?b->path:"/untitled.py",path,256)){
        strncpy(b->path,path,255); strncpy(b->name,base_name(path),63); b->lang=detect_lang(path); save_buffer(b); }
}
static void action_find(void){ find_open=1; }
static void action_replace(void){
    char f[128],t[128]; if(prompt_modal("Replace - find:","",f,128)){ if(prompt_modal("Replace with:","",t,128)) replace_all(f,t); } }
static void do_menu_action(int m,int idx){
    Buffer*b=cur();
    if(m==0){ // File
        if(idx==0) new_untitled();
        else if(idx==1) action_open_folder();
        else if(idx==2){ if(!b->path[0]) action_save_as(); else save_buffer(b); }
        else if(idx==3) action_save_as();
        else if(idx==4) close_tab(curtab);
        else if(idx==5){ gui_window_destroy(win); syscall1(SYS_EXIT,0); }
    } else if(m==1){ // Edit
        if(idx==0) do_undo(b); else if(idx==1) do_redo(b);
        else if(idx==2){ if(b->sel_on){ if(g_clip)free(g_clip); g_clip=sel_extract(b); sel_delete(b);} }
        else if(idx==3){ if(b->sel_on){ if(g_clip)free(g_clip); g_clip=sel_extract(b);} set_status("Copied"); }
        else if(idx==4){ if(g_clip){ if(b->sel_on) sel_delete(b); ins_text(b,g_clip);} }
        else if(idx==5) action_find();
        else if(idx==6) action_replace();
    } else if(m==2){ // View
        if(idx==0) tree_visible=!tree_visible;
        else if(idx==1) console_visible=!console_visible;
        else if(idx==2){ console_visible=1; console_tab=0; }
        else if(idx==3){ console_visible=1; console_tab=1; }
    } else if(m==3){ // Run
        if(idx==0) run_active_file();
        else if(idx==1) stop_run();
        else if(idx==2){ pthread_mutex_lock(&g_lock); g_con_len=0; if(g_con)g_con[0]=0; g_cap_len=0; if(g_cap)g_cap[0]=0; pthread_mutex_unlock(&g_lock); }
        else if(idx==3){ g_sess_len=0; if(g_sess)g_sess[0]=0; g_con_prev=0; pthread_mutex_lock(&g_lock); g_con_len=0; if(g_con)g_con[0]=0; pthread_mutex_unlock(&g_lock); set_status("New REPL session"); }
    } else if(m==4){ // Help
        if(idx==0){ char x[8]; prompt_modal("Maytera Code Studio - Python IDE. Run+REPL via CPython. OK to close.","",x,8); }
    }
}

// ---------------------------------------------------------- input ----------
static int focus_console_input=0;  // 1 if typing goes to REPL input

static void handle_key(gui_event_t *ev){
    char c=ev->key_char; uint32_t kc=ev->keycode;
    if(g_modal) return;
    if(find_open){
        if(c==27){ find_open=0; return; }
        if(kc==K_ENTER||c=='\n'||c=='\r'){ find_next(); return; }
        if(c=='\b'||kc==K_BS){ int l=strlen(find_q); if(l)find_q[l-1]=0; return; }
        if(c>=32&&c<127){ int l=strlen(find_q); if(l<126){find_q[l++]=c;find_q[l]=0;} return; }
        return;
    }
    // REPL input focus
    if(focus_console_input && console_visible && console_tab==0){
        if(kc==K_ENTER||c=='\n'||c=='\r'){ if(con_input_len>0){ con_input[con_input_len]=0; console_submit(con_input); con_input_len=0; con_input[0]=0; } return; }
        if(c=='\b'||kc==K_BS){ if(con_input_len>0) con_input[--con_input_len]=0; return; }
        if(kc==K_UP){ g_out_scroll++; return; } if(kc==K_DOWN){ if(g_out_scroll>0)g_out_scroll--; return; }
        if(c>=32&&c<127&&con_input_len<510){ con_input[con_input_len++]=c; con_input[con_input_len]=0; return; }
        return;
    }
    // global shortcuts
    if(c==19){ if(!cur()->path[0]) action_save_as(); else save_buffer(cur()); return; }  // Ctrl+S
    if(c==14){ new_untitled(); return; }        // Ctrl+N
    if(c==6){ action_find(); return; }           // Ctrl+F
    if(c==8 && kc!=K_BS){ action_replace(); return; } // Ctrl+H
    if(c==26){ do_undo(cur()); return; }         // Ctrl+Z
    if(c==25){ do_redo(cur()); return; }         // Ctrl+Y
    if(kc==K_F5){ run_active_file(); return; }
    Buffer*b=cur();
    if(c==1){ b->sel_on=1; b->al=0;b->ac=0; b->cl=b->nlines-1; b->cc=strlen(b->lines[b->cl]); return; } // Ctrl+A select all
    if(c==3){ if(b->sel_on){ if(g_clip)free(g_clip); g_clip=sel_extract(b);} set_status("Copied"); return; } // Ctrl+C
    if(c==24){ if(b->sel_on){ if(g_clip)free(g_clip); g_clip=sel_extract(b); sel_delete(b);} return; } // Ctrl+X
    if(c==22){ if(g_clip){ if(b->sel_on) sel_delete(b); ins_text(b,g_clip);} return; } // Ctrl+V
    if(c==0){ // Ctrl+Space toggle selection anchor
        if(b->sel_on) b->sel_on=0; else { b->sel_on=1; b->al=b->cl; b->ac=b->cc; } return; }
    int ext=b->sel_on; // selection continues if active (Ctrl+Space mode)
    // navigation
    if(kc==K_LEFT){ move(b,0,-1,ext); return; }
    if(kc==K_RIGHT){ move(b,0,1,ext); return; }
    if(kc==K_UP){ if(b->cl>0){b->cl--; clampc(b);} if(!ext)b->sel_on=0; return; }
    if(kc==K_DOWN){ if(b->cl<b->nlines-1){b->cl++; clampc(b);} if(!ext)b->sel_on=0; return; }
    if(kc==K_HOME){ move(b,0,-99,ext); return; }
    if(kc==K_END){ move(b,0,99,ext); return; }
    if(kc==K_PGUP){ int r=edit_rows(); b->cl-=r; if(b->cl<0)b->cl=0; clampc(b); if(!ext)b->sel_on=0; return; }
    if(kc==K_PGDN){ int r=edit_rows(); b->cl+=r; if(b->cl>=b->nlines)b->cl=b->nlines-1; clampc(b); if(!ext)b->sel_on=0; return; }
    // editing
    if(kc==K_DEL){ if(b->sel_on) sel_delete(b); else do_delete(b); return; }
    if(c=='\b'||kc==K_BS){ if(b->sel_on) sel_delete(b); else do_backspace(b); return; }
    if(kc==K_ENTER||c=='\n'||c=='\r'){ if(b->sel_on) sel_delete(b); ins_newline(b); return; }
    if(c=='\t'){ if(b->sel_on) sel_delete(b); undo_push(b); b->last_type=0; for(int i=0;i<4;i++) ins_char(b,' '); return; }
    if(c>=32&&c<127){ if(b->sel_on) sel_delete(b); ins_char(b,c); return; }
}

static void handle_mouse_down(gui_event_t *ev){
    int mx=ev->mouse_x,my=ev->mouse_y;
    // menubar
    if(my<MENUBAR_H){ int x=8; for(int i=0;i<5;i++){ int w=strlen(g_menus[i])*FW+16; if(mx>=x&&mx<x+w){ menu_open= (menu_open==i)?-1:i; return; } x+=w; } menu_open=-1; return; }
    // dropdown
    if(menu_open>=0){
        const char **it=menu_items(menu_open); int n=0; while(it[n])n++;
        int mw=0; for(int i=0;i<n;i++){int w=strlen(it[i])*FW;if(w>mw)mw=w;} mw+=24;
        int x=menu_x(menu_open),y=MENUBAR_H;
        if(mx>=x&&mx<x+mw&&my>=y&&my<y+n*20+4){ int idx=(my-y-4)/20; if(idx>=0&&idx<n){ int m=menu_open; menu_open=-1; do_menu_action(m,idx);} return; }
        menu_open=-1;
    }
    // toolbar
    int tb=toolbar_hit(mx,my);
    if(tb>0){ switch(tb){case 1:new_untitled();break;case 2:action_open_folder();break;case 3:{Buffer*b=cur(); if(!b->path[0])action_save_as();else save_buffer(b);}break;case 4:run_active_file();break;case 5:stop_run();break;case 6:action_find();break;} return; }
    // tabbar
    int ty=MENUBAR_H+TOOLBAR_H;
    if(my>=ty&&my<ty+TABBAR_H && mx>=editor_x()){
        int x=editor_x(); for(int i=0;i<ntabs;i++){ char lbl[80]; snprintf(lbl,sizeof lbl,"%s%s",tabs[i].name,tabs[i].dirty?" *":""); int w=strlen(lbl)*FW+28;
            if(mx>=x&&mx<x+w){ if(mx>=x+w-18){ close_tab(i);} else curtab=i; return; } x+=w; }
        return;
    }
    // tree
    if(tree_visible && mx<TREE_W && my>=ty){
        int idx=tree_row_at(my); if(idx>=0){ if(tree[idx].is_dir) tree_toggle(idx); else { open_path_tab(tree[idx].path); focus_console_input=0; } }
        return;
    }
    // console tabs / input focus
    if(console_visible && my>=console_y() && my<body_bottom()){
        int x=editor_x(),y=console_y(); const char*t0="Python Console",*t1="Output"; int w0=strlen(t0)*FW+20,w1=strlen(t1)*FW+20;
        if(my<y+22){ if(mx>=x&&mx<x+w0)console_tab=0; else if(mx>=x+w0&&mx<x+w0+w1)console_tab=1; return; }
        focus_console_input=(console_tab==0); return;
    }
    // editor: place cursor
    int ex=editor_x(),ey=editor_y();
    if(mx>=ex&&my>=ey&&my<console_y()){
        focus_console_input=0; Buffer*b=cur(); int gw=gutter_w();
        int row=(my-ey)/FH; int li=b->top+row; if(li>=b->nlines)li=b->nlines-1; if(li<0)li=0;
        int col=(mx-(ex+gw+4))/FW + b->leftcol; if(col<0)col=0; int ln=strlen(b->lines[li]); if(col>ln)col=ln;
        b->cl=li; b->cc=col; b->sel_on=0; return;
    }
}

// ---------------------------------------------------------- sample project -
static void ensure_welcome(void){
    // create a welcome .py if none, so the IDE has something to show/run
    int fd=sys_open("/HELLO.PY", O_RDONLY);
    if(fd>=0){ sys_close(fd); return; }
    fd=sys_open("/HELLO.PY", O_WRONLY|O_CREAT|O_TRUNC);
    if(fd>=0){ const char *s=
        "# Welcome to Maytera Code Studio\n"
        "# Press Run (or F5) to execute this file with CPython.\n"
        "import sys\n\n"
        "def greet(name):\n"
        "    return \"Hello, \" + name + \"!\"\n\n"
        "for i in range(3):\n"
        "    print(greet(\"MayteraOS\"), i)\n\n"
        "print(\"Python\", sys.version.split()[0])\n";
        sys_write(fd,s,strlen(s)); sys_close(fd); }
}

int main(int argc, char **argv){
    pthread_mutex_init(&g_lock,0);
    g_cap=malloc(CAPMAX); g_cap[0]=0; g_cap_len=0;
    g_con=malloc(4096); g_con[0]=0; g_con_len=0; g_con_cap=4096;
    g_sess=malloc(4096); g_sess[0]=0; g_sess_len=0; g_sess_cap=4096;
    const char *welcome="Maytera Code Studio - Python IDE\nType statements below and press Enter. Open a .py and press Run.\n\n";
    append_buf(&g_con,&g_con_len,&g_con_cap,welcome,strlen(welcome));

    win=gui_window_create("Maytera Code Studio", 60,40, W,H);
    if(win<0) return 1;

    ensure_welcome();
    tree_init("/");
    // open a file passed as arg, else the welcome file
    if(argc>1 && argv[1] && argv[1][0]) open_path_tab(argv[1]);
    else { if(open_path_tab("/HELLO.PY")<0) new_untitled(); }
    focus_console_input=0;

    render();
    while(1){
        int timeout = g_run_active?30:180;
        gui_event_t ev; int t=gui_get_event(win,&ev,timeout);
        int need=0;
        if(t==EVENT_WINDOW_CLOSE){ gui_window_destroy(win); break; }
        else if(t==EVENT_RESIZE){ if(ev.mouse_x>200)W=ev.mouse_x; if(ev.mouse_y>150)H=ev.mouse_y; need=1; }
        else if(t==EVENT_KEY_DOWN){ handle_key(&ev); need=1; }
        else if(t==EVENT_MOUSE_DOWN){ handle_mouse_down(&ev); need=1; }
        else if(t==EVENT_MOUSE_SCROLL){
            if(console_visible && ev.mouse_y>=console_y()){ g_out_scroll+=ev.scroll_delta>0?-3:3; if(g_out_scroll<0)g_out_scroll=0; }
            else { Buffer*b=cur(); b->top+= ev.scroll_delta>0?-3:3; if(b->top<0)b->top=0; if(b->top>=b->nlines)b->top=b->nlines-1; }
            need=1;
        }
        // consume worker completion / streamed output
        pthread_mutex_lock(&g_lock);
        int done=g_run_done, dirty=g_dirty; g_dirty=0;
        pthread_mutex_unlock(&g_lock);
        if(done){
            pthread_mutex_lock(&g_lock); g_run_done=0; int target=g_run_target; pthread_mutex_unlock(&g_lock);
            if(target==1) console_finish();
            else { int ex; pthread_mutex_lock(&g_lock); ex=g_run_exit; pthread_mutex_unlock(&g_lock);
                char m[80]; snprintf(m,sizeof m,"Program finished (exit %d)",ex); set_status(m); }
            need=1;
        }
        if(dirty) need=1;
        if(need) render();
    }
    return 0;
}
