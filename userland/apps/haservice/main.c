// haservice - background Home Assistant fetcher/controller for MayteraOS (#414)
//
// Windowless background service (run by the service manager, like netinfo).
// Reads the External Services config, then periodically fetches the state of the
// entities the desktop widgets care about and writes small cache files that the
// compositor HA widget parses and renders. Doing the blocking HTTP here keeps the
// UI thread free (the #211/#212/#381/#402 rule: never fetch on the draw thread).
//
// Config (written by Settings > External Services):
//   /CONFIG/EXTSVC.CFG   url=http://host:8123
//                        token=<long-lived token>
//                        refresh=15
// Widget-owned files:
//   /HAENT.TXT    one entity_id per line (<=8) the widgets want cached
//   /HA0.TXT..    per-slot cache: entity_id|friendly|state|unit|domain
//   /HALIST.REQ   presence requests a full entity list refresh (for the picker)
//   /HALIST.TXT   compact catalog: entity_id|friendly|state  (one per line)
//   /HACMD.TXT    pending control command: domain.service|entity_id  (widget writes)
//
// SAFETY (#414): a power-controlling switch (switch.* whose id/name contains
// outlet/plug/power) is NEVER actioned here - the service refuses the POST even
// if a command file somehow names one. The widget UI also guards these.

#include "../../libc/maytera.h"
#include "../../libc/syscall.h"

#define O_WRONLY  0x0001
#define O_CREAT   0x0040

static char g_url[192];       // base, e.g. http://<HOME_ASSISTANT_HOST>:8123
static char g_token[512];
static char g_hdr[600];       // "Authorization: Bearer <token>\r\n"
static int  g_refresh = 15;

static char big[1050000];     // full /api/states (991KB) list buffer
static char small_buf[8192];  // per-entity response
static char g_s0_state[64]=""; // #419 latest slot-0 state (for the live sparkline)

// ---- tiny helpers ----------------------------------------------------------
static int slen(const char *s){ int n=0; while(s[n]) n++; return n; }
static void scopy(char *d,const char *s,int cap){ int i=0; for(;s[i]&&i<cap-1;i++) d[i]=s[i]; d[i]=0; }
static int has(const char *s,const char *kw){ int kl=slen(kw); for(int i=0;s[i];i++){int j=0;while(j<kl&&s[i+j]==kw[j])j++; if(j==kl)return 1;} return 0; }

static const char *find_after(const char *s,const char *key){
    int kl=slen(key);
    for(int i=0;s[i];i++){ int j=0; while(j<kl&&s[i+j]==key[j])j++; if(j==kl) return &s[i+kl]; }
    return 0;
}
// "key":"value"  -> value. Returns 1 on success.
static int json_str(const char *s,const char *key,char *out,int cap){
    char k[64]; int n=0; k[n++]='"'; for(int i=0;key[i]&&n<60;i++)k[n++]=key[i]; k[n++]='"'; k[n++]=':'; k[n]=0;
    const char *p=find_after(s,k); if(!p) return 0;
    while(*p==' ')p++; if(*p!='"') return 0; p++;
    int i=0; for(;*p&&*p!='"'&&i<cap-1;i++,p++){ if(*p=='\\'&&p[1]){p++;} out[i]=*p; } out[i]=0; return 1;
}

// ---- file io ---------------------------------------------------------------
static int read_file(const char *path,char *dst,int cap){
    int fd=sys_open(path,0); if(fd<0){ dst[0]=0; return -1; }
    int got=0;
    while(got<cap-1){ long n=sys_read(fd,dst+got,cap-1-got); if(n<=0) break; got+=(int)n; }
    dst[got]=0; sys_close(fd); return got;
}
static void write_file(const char *path,const char *text,int len){
    int fd=sys_open(path,O_WRONLY|O_CREAT); if(fd<0) return;
    sys_write(fd,text,len); sys_close(fd);
}
static int file_exists(const char *path){ int fd=sys_open(path,0); if(fd<0) return 0; sys_close(fd); return 1; }
static void remove_file(const char *path){ /* truncate to empty as a delete surrogate */
    int fd=sys_open(path,O_WRONLY|O_CREAT); if(fd>=0){ sys_write(fd,"",0); sys_close(fd);} }

// Parse EXTSVC.CFG (key=value lines). Returns 1 if a url+token were found.
static int load_config(void){
    char buf[1600];
    if(read_file("/CONFIG/EXTSVC.CFG",buf,sizeof(buf))<=0){
        if(read_file("/EXTSVC.CFG",buf,sizeof(buf))<=0) return 0;   // FAT-root fallback
    }
    g_url[0]=g_token[0]=0; g_refresh=15;
    int i=0;
    while(buf[i]){
        // line start
        int ls=i; while(buf[i]&&buf[i]!='\n'&&buf[i]!='\r') i++;
        int le=i; while(buf[i]=='\n'||buf[i]=='\r') i++;
        // key=value in [ls,le)
        int eq=-1; for(int j=ls;j<le;j++){ if(buf[j]=='='){eq=j;break;} }
        if(eq<0) continue;
        char key[16]; int k=0; for(int j=ls;j<eq&&k<15;j++) key[k++]=buf[j]; key[k]=0;
        // trim trailing spaces on value
        int ve=le; while(ve>eq+1&&(buf[ve-1]==' ')) ve--;
        if(has(key,"url")){ int n=0; for(int j=eq+1;j<ve&&n<(int)sizeof(g_url)-1;j++) g_url[n++]=buf[j]; g_url[n]=0; }
        else if(has(key,"token")){ int n=0; for(int j=eq+1;j<ve&&n<(int)sizeof(g_token)-1;j++) g_token[n++]=buf[j]; g_token[n]=0; }
        else if(has(key,"refresh")){ int v=0; for(int j=eq+1;j<ve;j++){ if(buf[j]>='0'&&buf[j]<='9') v=v*10+(buf[j]-'0'); } if(v>=3&&v<=3600) g_refresh=v; }
    }
    if(!g_url[0]||!g_token[0]) return 0;
    // Build the auth header once.
    int h=0; const char *pfx="Authorization: Bearer ";
    for(int j=0;pfx[j];j++) g_hdr[h++]=pfx[j];
    for(int j=0;g_token[j]&&h<(int)sizeof(g_hdr)-4;j++) g_hdr[h++]=g_token[j];
    g_hdr[h++]='\r'; g_hdr[h++]='\n'; g_hdr[h]=0;
    return 1;
}

// domain of an entity_id (text before the first '.').
static void domain_of(const char *eid,char *out,int cap){ int i=0; for(;eid[i]&&eid[i]!='.'&&i<cap-1;i++) out[i]=eid[i]; out[i]=0; }

// A power-controlling switch we must never toggle.
static int is_power_switch(const char *eid){
    char dom[16]; domain_of(eid,dom,sizeof(dom));
    if(!has(dom,"switch")) return 0;
    return has(eid,"outlet")||has(eid,"plug")||has(eid,"power");
}

// GET /api/states/<eid> -> write /HA<slot>.TXT as entity|friendly|state|unit|domain.
// The kernel TCP socket pool is small and shared with netinfo, so a single fetch
// can transiently fail under contention; we retry a few times (spaced) and, on
// total failure, KEEP the last good cache file rather than clobbering it with an
// "unavailable" placeholder (so the widget keeps showing the last live value).
static void refresh_entity(const char *eid,int slot){
    char url[320]; int u=0;
    for(int j=0;g_url[j];j++) url[u++]=g_url[j];
    const char *p="/api/states/"; for(int j=0;p[j];j++) url[u++]=p[j];
    for(int j=0;eid[j]&&u<(int)sizeof(url)-1;j++) url[u++]=eid[j]; url[u]=0;

    unsigned int bytes=0; int status=0; int r=-1;
    for(int attempt=0; attempt<3; attempt++){
        r=sys_http_fetch_hdr(url,g_hdr,small_buf,sizeof(small_buf)-1,&bytes,&status);
        if(r>=0 && status==200 && bytes>0) break;
        sys_sleep(700);            // space retries: let a just-closed socket free up
    }
    if(!(r>=0&&status==200&&bytes>0)) return;   // keep last good cache on failure

    small_buf[bytes]=0;
    char st[64],unit[24],fn[96]; st[0]=unit[0]=fn[0]=0;
    json_str(small_buf,"state",st,sizeof(st));
    json_str(small_buf,"unit_of_measurement",unit,sizeof(unit));
    json_str(small_buf,"friendly_name",fn,sizeof(fn));
    if(slot==0) scopy(g_s0_state,st,sizeof(g_s0_state));   // #419 capture for sparkline
    char dom[16]; domain_of(eid,dom,sizeof(dom));
    char out[512]; int oi=0;
    #define AP(s) do{ for(int _i=0;(s)[_i]&&oi<(int)sizeof(out)-1;_i++) out[oi++]=(s)[_i]; }while(0)
    #define APC(c) do{ if(oi<(int)sizeof(out)-1) out[oi++]=(c); }while(0)
    AP(eid); APC('|'); AP(fn[0]?fn:eid); APC('|'); AP(st[0]?st:"?"); APC('|'); AP(unit); APC('|'); AP(dom);
    APC('\n');
    #undef AP
    #undef APC
    char path[16]; path[0]='/'; path[1]='H'; path[2]='A'; path[3]=(char)('0'+slot); path[4]='.'; path[5]='T'; path[6]='X'; path[7]='T'; path[8]=0;
    write_file(path,out,oi);
}

// (#419) FALLBACK: /api/states GET -> compact /HALIST.TXT (id|friendly|state).
// Kept for HA setups where the template API is disabled; the ~1MB JSON dump is
// slow/unreliable to receive over the shared TCP stack, so this is the last
// resort only.
static void refresh_list_states(void){
    char url[256]; int u=0; for(int j=0;g_url[j];j++) url[u++]=g_url[j];
    const char *p="/api/states"; for(int j=0;p[j];j++) url[u++]=p[j]; url[u]=0;
    unsigned int bytes=0; int status=0;
    int r=sys_http_fetch_hdr(url,g_hdr,big,sizeof(big)-1,&bytes,&status);
    if(r<0||status!=200||bytes==0){ return; }   // #419 keep last good, don't clobber
    big[bytes]=0;
    int fd=sys_open("/HALIST.TXT",O_WRONLY|O_CREAT); if(fd<0) return;
    // Walk each object: at every "entity_id":"..." capture id, then the following
    // "state":"..." and "friendly_name":"..." within the same record.
    const char *s=big; char line[256];
    while((s=find_after(s,"\"entity_id\":\""))){
        char id[96]; int i=0; while(*s&&*s!='"'&&i<(int)sizeof(id)-1) id[i++]=*s++; id[i]=0;
        // state comes right after entity_id in HA's JSON
        char st[64]="?"; const char *sp=find_after(s,"\"state\":\"");
        // friendly_name is inside attributes, after state
        char fn[96]=""; const char *fp=find_after(s,"\"friendly_name\":\"");
        // bound both to before the next entity_id so we don't cross records
        const char *nxt=find_after(s,"\"entity_id\":\"");
        if(sp && (!nxt || sp<nxt)){ int j=0; while(*sp&&*sp!='"'&&j<(int)sizeof(st)-1) st[j++]=*sp++; st[j]=0; }
        if(fp && (!nxt || fp<nxt)){ int j=0; while(*fp&&*fp!='"'&&j<(int)sizeof(fn)-1) fn[j++]=*fp++; fn[j]=0; }
        int li=0; for(int j=0;id[j];j++) line[li++]=id[j]; line[li++]='|';
        for(int j=0;fn[j]&&li<(int)sizeof(line)-4;j++) line[li++]=fn[j]; line[li++]='|';
        for(int j=0;st[j]&&li<(int)sizeof(line)-2;j++) line[li++]=st[j]; line[li++]='\n';
        sys_write(fd,line,li);
    }
    sys_close(fd);
}

// (#419) Compact entity catalog for the picker, fetched in SMALL CHUNKS.
// We ask HA to render the list server-side via the template API (POST
// /api/template) so the payload is "entity_id|friendly|domain" text, and we slice
// it with `states[off:off+CHUNK]` so every response is only a few KB. This is the
// key reliability fix: the MayteraOS HTTP recv resets its ~1.6s idle-timeout only
// on incoming data, so under CPU contention (aichat) a single large (~1MB /api/
// states or ~150KB whole-catalog) response stalls past the gap and times out
// before the headers even parse - while small per-entity requests always succeed.
// Chunking keeps every request in that reliable small-response regime, and we
// accumulate the chunks into one compact /HALIST.TXT (~150KB, under the read
// ceiling).
#define HA_CHUNK 100
static char chunkbuf[24000];
static int  itoa_app(char *d,int v){ char t[12]; int n=0; if(v==0)t[n++]='0'; while(v){t[n++]=(char)('0'+v%10);v/=10;} int i=0; while(n) d[i++]=t[--n]; return i; }
static void refresh_list(void){
    char url[256]; int u=0; for(int j=0;g_url[j];j++) url[u++]=g_url[j];
    const char *p="/api/template"; for(int j=0;p[j];j++) url[u++]=p[j]; url[u]=0;
    int total=0; big[0]=0;
    for(int off=0; off<5000; off+=HA_CHUNK){
        char body[320]; int bi=0;
        const char *a="{\"template\":\"{% for s in (states|list)[";
        for(int j=0;a[j];j++) body[bi++]=a[j];
        bi+=itoa_app(body+bi,off); body[bi++]=':'; bi+=itoa_app(body+bi,off+HA_CHUNK);
        const char *b="] %}{{ s.entity_id }}|{{ s.name }}|{{ s.domain }}\\n{% endfor %}\"}";
        for(int j=0;b[j];j++) body[bi++]=b[j]; body[bi]=0;
        int status=0,n=0;
        for(int attempt=0; attempt<3; attempt++){        // per-chunk retry (contention)
            n=sys_http_post(url,g_hdr,body,chunkbuf,sizeof(chunkbuf)-1,&status);
            if(status==200) break;                        // HA responded (n may be 0 at the end)
            sys_sleep(500);
        }
        if(status!=200){                                  // this page failed after retries
            if(off==0){ refresh_list_states(); return; }  // nothing yet: try the /api/states fallback once
            break;                                        // keep the partial catalog gathered so far
        }
        if(n<=0) break;                                   // 200 + empty => past the last entity
        chunkbuf[n]=0;
        if(!has(chunkbuf,"|")) break;                     // no entities in this page => end
        for(int j=0;chunkbuf[j] && total<(int)sizeof(big)-2;j++) big[total++]=chunkbuf[j];
        // HA strips the trailing newline of a rendered template, so ensure one
        // sits between pages or the last id of a page merges with the next.
        if(total>0 && big[total-1]!='\n' && total<(int)sizeof(big)-1) big[total++]='\n';
    }
    big[total]=0;
    if(total>0) write_file("/HALIST.TXT",big,total);
}

// (#419) Sparkline series for the primary (slot 0) entity, built from LIVE
// SAMPLES rather than a history fetch. The full HA /api/history response (~200KB
// for a day) hits the same large-response timeout as the catalog, so instead we
// append each freshly-read slot-0 value to a small rolling ring and rewrite a
// tiny /HAHIST0.TXT (line 1 = entity_id, line 2 = up-to-64 normalized 0..1000
// ints). The chart fills in over the service's refresh interval using only the
// small per-entity requests that are reliable here. Non-numeric states are
// skipped so the series stays plottable.
#define HSPARK_MAX 64
// parse a decimal like "453.089" into value*1000 (long). returns 1 if numeric.
static int parse_milli(const char *s,long *out){
    int i=0,neg=0; if(s[0]=='-'){neg=1;i=1;}
    if(!(s[i]>='0'&&s[i]<='9')) return 0;
    long v=0; for(;s[i]>='0'&&s[i]<='9';i++) v=v*10+(s[i]-'0');
    long frac=0,scale=1;
    if(s[i]=='.'){ i++; int d=0; for(;s[i]>='0'&&s[i]<='9'&&d<3;i++,d++){ frac=frac*10+(s[i]-'0'); scale*=10; } }
    long m=v*1000 + frac*(1000/scale);
    *out = neg? -m : m; return 1;
}
static long g_ring[HSPARK_MAX]; static int g_ring_n=0;
static char g_ring_eid[96]="";
// Push one sample for `eid` and rewrite the normalized series file.
static void spark_push(const char *eid,long milli){
    // reset the ring if the tracked entity changed
    int same=1; for(int i=0;i<96;i++){ if(g_ring_eid[i]!=eid[i]){same=0;break;} if(!eid[i])break; }
    if(!same){ g_ring_n=0; scopy(g_ring_eid,eid,sizeof(g_ring_eid)); }
    if(g_ring_n<HSPARK_MAX) g_ring[g_ring_n++]=milli;
    else { for(int i=1;i<HSPARK_MAX;i++) g_ring[i-1]=g_ring[i]; g_ring[HSPARK_MAX-1]=milli; }
    if(g_ring_n<1) return;
    long mn=g_ring[0],mx=g_ring[0];
    for(int i=1;i<g_ring_n;i++){ if(g_ring[i]<mn)mn=g_ring[i]; if(g_ring[i]>mx)mx=g_ring[i]; }
    char out[96+HSPARK_MAX*5+8]; int oi=0;
    for(int j=0;eid[j]&&oi<95;j++) out[oi++]=eid[j]; out[oi++]='\n';
    for(int i=0;i<g_ring_n;i++){
        long norm=(mx>mn)?(g_ring[i]-mn)*1000/(mx-mn):500; if(norm<0)norm=0; if(norm>1000)norm=1000;
        char t[8]; int tn=0; long v=norm; if(v==0)t[tn++]='0'; while(v){t[tn++]=(char)('0'+v%10);v/=10;}
        while(tn) out[oi++]=t[--tn]; out[oi++]=' ';
    }
    out[oi++]='\n';
    write_file("/HAHIST0.TXT",out,oi);
}

// Execute a pending control command file (guarded).
static void run_command(void){
    char cmd[160]; if(read_file("/HACMD.TXT",cmd,sizeof(cmd))<=0) return;
    if(!cmd[0]||cmd[0]=='\n'){ return; }
    // format: domain.service|entity_id
    char svc[64],eid[96]; int i=0,si=0;
    while(cmd[i]&&cmd[i]!='|'&&cmd[i]!='\n'&&si<63) svc[si++]=cmd[i++]; svc[si]=0;
    if(cmd[i]=='|') i++;
    int ei=0; while(cmd[i]&&cmd[i]!='\n'&&cmd[i]!='\r'&&ei<95) eid[ei++]=cmd[i++]; eid[ei]=0;
    remove_file("/HACMD.TXT");                 // consume regardless
    if(!svc[0]||!eid[0]) return;
    if(is_power_switch(eid)) return;           // SAFETY: never action a power switch
    // svc "switch.toggle" -> /api/services/switch/toggle
    char dom[32],act[32]; int d=0; for(;svc[d]&&svc[d]!='.'&&d<31;d++) dom[d]=svc[d]; dom[d]=0;
    int a=0; int k=(svc[d]=='.')?d+1:d; for(;svc[k]&&a<31;k++) act[a++]=svc[k]; act[a]=0;
    if(is_power_switch(eid)) return;
    char url[320]; int u=0; for(int j=0;g_url[j];j++) url[u++]=g_url[j];
    const char *p="/api/services/"; for(int j=0;p[j];j++) url[u++]=p[j];
    for(int j=0;dom[j];j++) url[u++]=dom[j]; url[u++]='/';
    for(int j=0;act[j];j++) url[u++]=act[j]; url[u]=0;
    char body[160]; int b=0; const char *bp="{\"entity_id\":\""; for(int j=0;bp[j];j++) body[b++]=bp[j];
    for(int j=0;eid[j]&&b<(int)sizeof(body)-4;j++) body[b++]=eid[j]; body[b++]='"'; body[b++]='}'; body[b]=0;
    int status=0;
    sys_http_post(url,g_hdr,body,small_buf,sizeof(small_buf)-1,&status);
    // status is logged implicitly via a result file for debugging/verification.
    char res[8]; int rl=0; int st=status; char tmp[8]; int tn=0; if(st==0)tmp[tn++]='0'; while(st){tmp[tn++]=(char)('0'+st%10);st/=10;} while(tn) res[rl++]=tmp[--tn]; res[rl++]='\n';
    write_file("/HACMD.RES",res,rl);
}

// (#419) A request marker (e.g. /HALIST.REQ) is "pending" only if it has real
// content. remove_file() truncates to 0 bytes (it does not unlink), so the old
// file_exists() test stayed true forever and re-fetched the catalog every loop.
static int req_pending(const char *path){
    char b[8]; int fd=sys_open(path,0); if(fd<0) return 0;
    long n=sys_read(fd,b,sizeof(b)); sys_close(fd);
    if(n<=0) return 0;
    for(int i=0;i<n;i++){ if(b[i]>' ') return 1; }   // non-whitespace => pending
    return 0;
}

int main(int argc,char**argv){
    (void)argc;(void)argv;
    int have_cfg=0;
    unsigned idle=0;
    for(;;){
        if(!have_cfg){ have_cfg=load_config(); if(!have_cfg){ sys_sleep(5000); continue; } }
        if(!sys_net_is_up()){ sys_sleep(5000); continue; }

        // Picker catalog on request (compositor drops /HALIST.REQ with content).
        if(req_pending("/HALIST.REQ")){ remove_file("/HALIST.REQ"); refresh_list(); }

        // Control commands (guarded).
        run_command();

        // Per-entity cache refresh from /HAENT.TXT.
        char ents[1024]; read_file("/HAENT.TXT",ents,sizeof(ents));
        int i=0,slot=0; char first_eid[96]; first_eid[0]=0;
        while(ents[i]&&slot<8){
            char eid[96]; int e=0;
            while(ents[i]&&ents[i]!='\n'&&ents[i]!='\r'&&e<95){ if(ents[i]!=' ') eid[e++]=ents[i]; i++; }
            while(ents[i]=='\n'||ents[i]=='\r') i++;
            eid[e]=0;
            if(e>0){ refresh_entity(eid,slot); if(slot==0) scopy(first_eid,eid,sizeof(first_eid)); slot++; }
        }

        // (#419) Append the freshly-read slot-0 value to the live sparkline series.
        if(first_eid[0]){ long m; if(parse_milli(g_s0_state,&m)) spark_push(first_eid,m); }

        // Re-read config occasionally so Settings edits take effect without reboot.
        if((idle++ % 8)==7){ have_cfg=load_config(); }
        sys_sleep((unsigned)g_refresh*1000u);
    }
    return 0;
}
