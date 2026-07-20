// netinfo - background internet-info fetcher service for MayteraOS (#81-83)
//
// Windowless background service (run by the #95 service manager). Periodically
// fetches weather, crypto and stock data and writes STRUCTURED, pipe-delimited
// result files that the compositor widgets parse and render as rich, multi-line
// cards. Doing the blocking network fetch here keeps the UI responsive.
//
//   /WEATHER.TXT : loc|cond|icon|now|min|max|hum|rain|mm
//   /CRYPTO.TXT  : CUR|CODE,price,chg|CODE,price,chg|...
//   /STOCKS.TXT  : CODE,price,chg|CODE,price,chg|...
//
// Config files written by the widgets' Settings dialogs:
//   /WXLOC.TXT (city[,country])   /CRYPTOID.TXT (BTC,ETH,..)   /CRYPTOCUR.TXT (USD)
//   /STOCKID.TXT (AAPL,MSFT,..)

#include "../../libc/maytera.h"

#define O_WRONLY  0x0001
#define O_CREAT   0x0040

static char http_buf[16384];

// ---- helpers ---------------------------------------------------------------
static int s_len(const char *s){ int n=0; while(s[n]) n++; return n; }
static char up1(char c){ return (c>='a'&&c<='z')?(char)(c-32):c; }
static int str_eq2(const char *a,const char *b){ int i=0; while(a[i]&&b[i]){ if(a[i]!=b[i]) return 0; i++; } return a[i]==b[i]; }

static int fetch(const char *url){
    // #374: hard network-up gate. Never attempt a fetch (DNS/TCP/TLS) when the
    // network is down - the kernel fast-fails anyway, but skipping here avoids
    // the inter-attempt sleeps and lets the service back off cleanly.
    if (!sys_net_is_up()) return -1;
    for (int a=0; a<2; a++){   // #374 2 attempts (was 4); kernel now bounds each
        if (!sys_net_is_up()) return -1;   // link may drop mid-cycle
        // Space out every attempt: the kernel TCP socket pool is small and a
        // just-closed socket lingers briefly, so back-to-back fetches otherwise
        // fail with "Failed to create socket". This also backs off rate limits.
        sys_sleep(1500);
        unsigned int bytes=0; int status=0;
        int r = sys_http_fetch(url, http_buf, sizeof(http_buf)-1, &bytes, &status);
        if (r>=0 && status==200 && bytes>0){
            if (bytes>sizeof(http_buf)-1) bytes=sizeof(http_buf)-1;
            http_buf[bytes]='\0'; return (int)bytes;
        }
    }
    return -1;
}

static void write_file(const char *path,const char *text){
    int fd=sys_open(path,O_WRONLY|O_CREAT); if(fd<0) return;
    int n=s_len(text);
    char line[256]; int i=0;
    for(; i<n && i<240; i++) line[i]=text[i];
    line[i++]='\n';
    while(i<248) line[i++]=' ';   // pad: overwrite-from-0 leaves no stale tail
    line[i++]='\n';
    sys_write(fd,line,i); sys_close(fd);
}

static const char *find_after(const char *s,const char *key){
    int kl=s_len(key);
    for(int i=0;s[i];i++){ int j=0; while(j<kl&&s[i+j]==key[j])j++; if(j==kl) return &s[i+kl]; }
    return 0;
}
static void read_cfg(const char *path,char *out,int cap,const char *def){
    out[0]='\0';
    int fd=sys_open(path,0);
    if(fd>=0){ char b[256]; long n=sys_read(fd,b,sizeof(b)-1); sys_close(fd);
        if(n>0){ int i=0; for(;i<cap-1&&i<n&&b[i]&&b[i]!='\n'&&b[i]!='\r';i++) out[i]=b[i]; while(i>0&&out[i-1]==' ')i--; out[i]='\0'; } }
    if(!out[0]){ int i=0; for(;def[i]&&i<cap-1;i++) out[i]=def[i]; out[i]='\0'; }
}

// Extract "key":"value" string. Returns 1 on success.
static int json_str(const char *s,const char *key,char *out,int cap){
    char k[48]; int n=0; k[n++]='"'; for(int i=0;key[i]&&n<45;i++)k[n++]=key[i]; k[n++]='"'; k[n++]=':'; k[n]='\0';
    const char *p=find_after(s,k); if(!p) return 0;
    while(*p==' ')p++; if(*p!='"') return 0; p++;
    int i=0; for(;*p&&*p!='"'&&i<cap-1;i++,p++) out[i]=*p; out[i]='\0'; return 1;
}
// Extract "key": NUM (also handles "key":[NUM,...]). Returns 1 on success.
static int json_num(const char *s,const char *key,char *out,int cap){
    char k[48]; int n=0; k[n++]='"'; for(int i=0;key[i]&&n<45;i++)k[n++]=key[i]; k[n++]='"'; k[n++]=':'; k[n]='\0';
    const char *p=find_after(s,k); if(!p) return 0;
    while(*p==' '||*p=='[')p++;
    int i=0,got=0; if(*p=='-'&&i<cap-1) out[i++]=*p++;
    while(((*p>='0'&&*p<='9')||*p=='.')&&i<cap-1){ out[i++]=*p; got=1; p++; }
    out[i]='\0'; return got;
}
// Truncate a numeric string to its integer part (drop the decimals).
static void int_part(char *s){ for(int i=0;s[i];i++) if(s[i]=='.'){ s[i]='\0'; return; } }
// Round a numeric string to one decimal place into out.
static void one_dp(const char *s,char *out,int cap){
    int i=0,dot=-1; for(;s[i]&&i<cap-1;i++){ out[i]=s[i]; if(s[i]=='.')dot=i; }
    out[i]='\0';
    if(dot>=0 && out[dot+1]) out[dot+2]='\0';   // keep one digit after '.'
}

// ---- crypto short ticker -> CoinGecko id -----------------------------------
static const char *sym2id(const char *sym,char *fb){
    static const char *syms[]={"BTC","ETH","USDT","USDC","BNB","XRP","SOL","ADA","DOGE","DOT","LTC","TRX","AVAX","LINK","MATIC","SHIB","UNI","ATOM"};
    static const char *ids[] ={"bitcoin","ethereum","tether","usd-coin","binancecoin","ripple","solana","cardano","dogecoin","polkadot","litecoin","tron","avalanche-2","chainlink","matic-network","shiba-inu","uniswap","cosmos"};
    for(int i=0;i<18;i++) if(str_eq2(sym,syms[i])) return ids[i];
    int k=0; for(;sym[k]&&k<38;k++) fb[k]=(sym[k]>='A'&&sym[k]<='Z')?(char)(sym[k]+32):sym[k]; fb[k]='\0'; return fb;
}

// Find the first "value":"X" string after a key (wttr.in nests as key:[{"value":X}]).
static int json_val_after(const char *s,const char *key,char *out,int cap){
    const char *p=find_after(s,key); if(!p) return 0;
    const char *v=find_after(p,"\"value\""); if(!v) return 0;
    while(*v==' '||*v==':')v++; if(*v!='"') return 0; v++;
    int i=0; for(;*v&&*v!='"'&&i<cap-1;i++,v++) out[i]=*v; out[i]='\0'; return 1;
}
static int has_kw(const char *s,const char *kw){ return find_after(s,kw)!=0; }
// Map a wttr.in weatherDesc text to a compositor icon index (0-6).
static int desc_icon(const char *d){
    if(has_kw(d,"Thunder")) return 6;
    if(has_kw(d,"Snow")||has_kw(d,"Sleet")||has_kw(d,"Blizzard")||has_kw(d,"Ice")) return 5;
    if(has_kw(d,"Rain")||has_kw(d,"Drizzle")||has_kw(d,"Shower")) return 4;
    if(has_kw(d,"Fog")||has_kw(d,"Mist")) return 3;
    if(has_kw(d,"Overcast")||has_kw(d,"Cloud")) return 2;
    if(has_kw(d,"Partly")||has_kw(d,"intervals")) return 1;
    if(has_kw(d,"Sunny")||has_kw(d,"Clear")) return 0;
    return 2;
}

// ---- weather (wttr.in j1 JSON: rich current + today's min/max/rain) --------
// Open-Meteo's TLS handshake hangs the kernel TLS stack; wttr.in works (it was
// the original source) and j1 gives full detail in one request.
static void update_weather(void){
    char loc[80]; read_cfg("/WXLOC.TXT",loc,sizeof(loc),"London");
    char name[80]; int ni=0; for(int i=0;loc[i]&&i<78;i++) name[ni++]=(loc[i]==' ')?'+':loc[i]; name[ni]='\0';
    char url[160]; int u=0;
    for(const char*p="https://wttr.in/";*p;p++) url[u++]=*p;
    for(int i=0;name[i]&&u<(int)sizeof(url)-12;i++) url[u++]=name[i];
    for(const char*p="?format=j1";*p;p++) url[u++]=*p; url[u]='\0';
    if(fetch(url)<0) return;

    char an[48],reg[48],ctry[48],cond[48];
    an[0]=reg[0]=ctry[0]=cond[0]='\0';
    json_val_after(http_buf,"\"areaName\"",an,sizeof(an));
    json_val_after(http_buf,"\"region\"",reg,sizeof(reg));
    json_val_after(http_buf,"\"country\"",ctry,sizeof(ctry));
    json_val_after(http_buf,"\"weatherDesc\"",cond,sizeof(cond));
    int icon=desc_icon(cond[0]?cond:"Cloudy");

    char now[16],hum[8],pmm[16],mx[16],mn[16],pp[8];
    now[0]=hum[0]=pmm[0]=mx[0]=mn[0]=pp[0]='\0';
    json_str(http_buf,"temp_C",now,sizeof(now));        // j1 numbers are quoted strings
    json_str(http_buf,"humidity",hum,sizeof(hum));
    json_str(http_buf,"precipMM",pmm,sizeof(pmm));
    json_str(http_buf,"maxtempC",mx,sizeof(mx));
    json_str(http_buf,"mintempC",mn,sizeof(mn));
    json_str(http_buf,"chanceofrain",pp,sizeof(pp));
    int_part(now); int_part(mx); int_part(mn); int_part(hum); int_part(pp);
    char mmd[16]; one_dp(pmm[0]?pmm:"0",mmd,sizeof(mmd));

    char out[256]; int oi=0;
    #define AP(s) do{ for(int _i=0;(s)[_i]&&oi<(int)sizeof(out)-1;_i++) out[oi++]=(s)[_i]; }while(0)
    if(an[0]) AP(an); else AP(loc);
    if(reg[0]){ AP(", "); AP(reg); } else if(ctry[0]){ AP(", "); AP(ctry); }
    AP("|"); AP(cond[0]?cond:"?"); AP("|");
    { char ib[4]; ib[0]=(char)('0'+icon); ib[1]='\0'; AP(ib); }
    AP("|"); AP(now[0]?now:"?");
    AP("|"); AP(mn[0]?mn:"?");
    AP("|"); AP(mx[0]?mx:"?");
    AP("|"); AP(hum[0]?hum:"?");
    AP("|"); AP(pp[0]?pp:"0");
    AP("|"); AP(mmd);
    out[oi]='\0';
    #undef AP
    write_file("/WEATHER.TXT",out);
}

// ---- crypto (CoinGecko: price + 24h change) --------------------------------
static void update_crypto(void){
    char syms[128]; read_cfg("/CRYPTOID.TXT",syms,sizeof(syms),"BTC,ETH");
    char cur[8];    read_cfg("/CRYPTOCUR.TXT",cur,sizeof(cur),"USD");
    char curl[8]; int ci=0; for(;cur[ci]&&ci<6;ci++) curl[ci]=(cur[ci]>='A'&&cur[ci]<='Z')?(char)(cur[ci]+32):cur[ci]; curl[ci]='\0';
    char curu[8]; int cu=0; for(;cur[cu]&&cu<6;cu++) curu[cu]=up1(cur[cu]); curu[cu]='\0';

    char idlist[220]; int il=0; char sym[16]; int si=0;
    for(int i=0;;i++){ char c=syms[i];
        if(c==','||c=='\0'){ sym[si]='\0';
            if(si>0){ char fb[40]; const char *id=sym2id(sym,fb); if(il>0&&il<(int)sizeof(idlist)-1) idlist[il++]=','; for(int j=0;id[j]&&il<(int)sizeof(idlist)-1;j++) idlist[il++]=id[j]; }
            si=0; if(c=='\0')break;
        } else if(si<(int)sizeof(sym)-1) sym[si++]=up1(c);
    }
    idlist[il]='\0';
    char url[320]; int u=0;
    for(const char*p="https://api.coingecko.com/api/v3/simple/price?include_24hr_change=true&ids=";*p;p++) url[u++]=*p;
    for(int i=0;idlist[i]&&u<(int)sizeof(url)-30;i++) url[u++]=idlist[i];
    for(const char*p="&vs_currencies=";*p;p++) url[u++]=*p;
    for(int i=0;curl[i]&&u<(int)sizeof(url)-1;i++) url[u++]=curl[i];
    url[u]='\0';
    if(fetch(url)<0) return;

    // 24h-change JSON key for this currency, e.g. "usd_24h_change" (no quotes;
    // json_num adds them). The price key is just curl, e.g. "usd"; the trailing
    // ':' that json_num appends disambiguates "usd" from "usd_24h_change".
    char chgk[24]; { int k=0; for(int j=0;curl[j];j++) chgk[k++]=curl[j]; for(const char*q="_24h_change";*q;q++) chgk[k++]=*q; chgk[k]='\0'; }
    char out[256]; int oi=0;
    #define AP(s) do{ for(int _i=0;(s)[_i]&&oi<(int)sizeof(out)-1;_i++) out[oi++]=(s)[_i]; }while(0)
    AP(curu);
    si=0;
    for(int i=0;;i++){ char c=syms[i];
        if(c==','||c=='\0'){ sym[si]='\0';
            if(si>0){
                char fb[40]; const char *id=sym2id(sym,fb);
                char key[44]; int k=0; key[k++]='"'; for(int j=0;id[j]&&k<41;j++) key[k++]=id[j]; key[k++]='"'; key[k]='\0';
                const char *at=find_after(http_buf,key);
                char price[24]="?", chg[16]="0";
                if(at){
                    char pr[24]; if(json_num(at,curl,pr,sizeof(pr))) one_dp(pr,price,sizeof(price));
                    char cg[16]; if(json_num(at,chgk,cg,sizeof(cg)))  one_dp(cg,chg,sizeof(chg));
                }
                AP("|"); AP(sym); AP(","); AP(price); AP(","); AP(chg);
            }
            si=0; if(c=='\0')break;
        } else if(si<(int)sizeof(sym)-1) sym[si++]=up1(c);
    }
    out[oi]='\0';
    #undef AP
    write_file("/CRYPTO.TXT",out);
}

// ---- stocks (Yahoo: price + change vs previous close) ----------------------
static void update_stocks(void){
    char syms[128]; read_cfg("/STOCKID.TXT",syms,sizeof(syms),"AAPL,MSFT");
    char out[256]; int oi=0;
    #define AP(s) do{ for(int _i=0;(s)[_i]&&oi<(int)sizeof(out)-1;_i++) out[oi++]=(s)[_i]; }while(0)
    char sym[16]; int si=0;
    for(int i=0;;i++){ char c=syms[i];
        if(c==','||c=='\0'){ sym[si]='\0';
            if(si>0){
                char url[160]; int u=0;
                for(const char*p="https://query1.finance.yahoo.com/v8/finance/chart/";*p;p++) url[u++]=*p;
                for(int j=0;sym[j];j++) url[u++]=sym[j];
                for(const char*p="?interval=1d";*p;p++) url[u++]=*p; url[u]='\0';
                char price[24]="?", chg[16]="0";
                if(fetch(url)>=0){
                    char pr[24],pc[24];
                    if(json_num(http_buf,"regularMarketPrice",pr,sizeof(pr))){ one_dp(pr,price,sizeof(price)); }
                    if(json_num(http_buf,"chartPreviousClose",pc,sizeof(pc)) || json_num(http_buf,"previousClose",pc,sizeof(pc))){
                        // chg% = (price-prev)/prev*100, integer-ish via *10 fixed point
                        // parse to scaled ints (x100)
                        long p100=0,c100=0; int sgn=1; const char *q=pr; if(*q=='-'){sgn=-1;q++;}
                        { long v=0,f=0,fd=0; int seen=0; for(;*q;q++){ if(*q=='.'){seen=1;continue;} if(*q<'0'||*q>'9')break; if(!seen)v=v*10+(*q-'0'); else if(fd<2){f=f*10+(*q-'0');fd++;} } while(fd<2){f*=10;fd++;} p100=sgn*(v*100+f); }
                        q=pc; sgn=1; if(*q=='-'){sgn=-1;q++;}
                        { long v=0,f=0,fd=0; int seen=0; for(;*q;q++){ if(*q=='.'){seen=1;continue;} if(*q<'0'||*q>'9')break; if(!seen)v=v*10+(*q-'0'); else if(fd<2){f=f*10+(*q-'0');fd++;} } while(fd<2){f*=10;fd++;} c100=sgn*(v*100+f); }
                        if(c100!=0){ long d=(p100-c100)*1000/c100; // pct x10
                            int neg=d<0; if(neg)d=-d; int whole=d/10,frac=d%10;
                            int k=0; if(neg)chg[k++]='-';
                            char tb[8]; int tn=0; if(whole==0)tb[tn++]='0'; while(whole){tb[tn++]=(char)('0'+whole%10);whole/=10;} while(tn) chg[k++]=tb[--tn];
                            chg[k++]='.'; chg[k++]=(char)('0'+frac); chg[k]='\0';
                        }
                    }
                }
                if(oi>0) AP("|");
                AP(sym); AP(","); AP(price); AP(","); AP(chg);
            }
            si=0; if(c=='\0')break;
        } else if(si<(int)sizeof(sym)-1) sym[si++]=up1(c);
    }
    out[oi]='\0';
    #undef AP
    write_file("/STOCKS.TXT",out);
}

// #213: refresh cadence. Each feed refreshes every 30 minutes, but the three
// are STAGGERED 10 minutes apart so they never hit the network at once (keeps
// background I/O minimal and avoids rate limits / socket-pool contention).
// Pattern after the initial fetch: +10 crypto, +20 stocks, +30 weather, then
// each repeats on a 30-min period (crypto at 10/40/70..., stocks 20/50/80...,
// weather 30/60/90...).
#define NETINFO_STAGGER_MS   600000u    // 10 minutes between staggered fetches
// (#97 future policy: update-server check every 2h. No stub exists yet, skip.)

int main(int argc,char**argv){
    (void)argc;(void)argv;
    // First pass immediately so the widget cards have data right after boot
    // (each update_* is net-up-gated internally, so this is a cheap no-op offline).
    update_crypto();
    update_stocks();
    update_weather();
    unsigned down_backoff = 0;   // #374 exponential poll backoff while offline
    for(;;){
        // #374: when the network is down, do NOT run the fetch cadence (which
        // would burn timeouts / churn every cycle and could stall the UI on a
        // link-down box). Poll for the link to return on an exponential backoff
        // (30s -> capped ~8min), attempting nothing until it is up; resume the
        // normal staggered refresh immediately once it returns.
        if(!sys_net_is_up()){
            unsigned shift = down_backoff < 4 ? down_backoff : 4;
            if(down_backoff < 4) down_backoff++;
            sys_sleep(30000u << shift);   // 30s,60s,120s,240s,480s
            continue;
        }
        down_backoff = 0;
        sys_sleep(NETINFO_STAGGER_MS);   // +10 min
        update_crypto();
        sys_sleep(NETINFO_STAGGER_MS);   // +20 min
        update_stocks();
        sys_sleep(NETINFO_STAGGER_MS);   // +30 min
        update_weather();
    }
    return 0;
}
