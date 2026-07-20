// ssh2_server.c - SSH-2 server for MayteraOS. Mirror of the ssh2 client record
// layer in the server direction: curve25519-sha256 KEX, RSA host-key signature
// (rsa-sha2-256), aes256-ctr + hmac-sha2-256, password auth, one session channel
// bridged to /APPS/MSH on a fresh pty. Single concurrent session (MVP).
//
// Host key: flat /CONFIG/SSHHOST.KEY = "MSSHKEY1" then u32-len n, u32-len e,
// u32-len d (big-endian), generated offline (see backup notes).
#include "ssh2.h"                 // reuse SSH2_* sizes + ssh2_client_t layout pieces
#include "../tcp.h"
#include "../net.h"
#include "../tls/tls13.h"
#include "../../crypto/rsa.h"
#include "../../crypto/ed25519.h"
#include "../../crypto/crypto.h"
#include "../../string.h"
#include "../../mm/heap.h"
#include "../../serial.h"
#include "../../fs/fat.h"
#include "../../proc/process.h"
#include "../../drivers/tty.h"

extern void net_poll(void);
extern void tcp_timer(void);
extern void proc_sleep(uint32_t ms);
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;
extern fat_fs_t g_fat_fs;
// #297 global net serialization: the TCP connection table is also mutated by
// net_poll()->tcp_handle() (which runs under net_lock). With per-session worker
// threads (#435) several sshd threads touch the table concurrently, so every
// raw tcp_* call below is wrapped in net_lock/net_unlock. These are short,
// non-yielding critical sections (never held across proc_sleep), matching how
// net_poll() itself uses the lock.
extern void net_lock(void);
extern void net_unlock(void);

// pty / proc bridge (same as net/remote_ctrl.c rc_cmd_shell)
extern struct file *dev_open(const char *name, int flags);
extern void     file_put(struct file *f);
extern int64_t  file_read(struct file *f, void *buf, uint64_t count);
extern int64_t  file_write(struct file *f, const void *buf, uint64_t count);
extern int      file_ioctl(struct file *f, unsigned cmd, void *arg2);
extern int      file_poll(struct file *f, int events);
extern int      proc_create_user_tty(const char *name, void *data, uint64_t sz, int pts_idx);
extern process_t *proc_get(uint32_t pid);

#define MSG_DISCONNECT 1
#define MSG_IGNORE 2
#define MSG_DEBUG 4
#define MSG_SERVICE_REQUEST 5
#define MSG_SERVICE_ACCEPT 6
#define MSG_EXT_INFO 7
#define MSG_KEXINIT 20
#define MSG_NEWKEYS 21
#define MSG_ECDH_INIT 30
#define MSG_ECDH_REPLY 31
#define MSG_USERAUTH_REQUEST 50
#define MSG_USERAUTH_FAILURE 51
#define MSG_USERAUTH_SUCCESS 52
#define MSG_USERAUTH_PK_OK 60
#define MSG_GLOBAL_REQUEST 80
#define MSG_REQUEST_FAILURE 82
#define MSG_CHANNEL_OPEN 90
#define MSG_CHANNEL_OPEN_CONFIRMATION 91
#define MSG_CHANNEL_WINDOW_ADJUST 93
#define MSG_CHANNEL_DATA 94
#define MSG_CHANNEL_EOF 96
#define MSG_CHANNEL_CLOSE 97
#define MSG_CHANNEL_REQUEST 98
#define MSG_CHANNEL_SUCCESS 99
#define MSG_CHANNEL_FAILURE 100

static int g_dbg = 0;
#define DBG(...) do { if (g_dbg) kprintf(__VA_ARGS__); } while (0)

// ---- host key (loaded once) ----
static uint8_t *g_hk_n, *g_hk_e, *g_hk_d;
static int g_hk_nlen, g_hk_elen, g_hk_dlen;
static uint8_t *g_hk_p, *g_hk_q, *g_hk_dp, *g_hk_dq, *g_hk_qinv;
static int g_hk_plen, g_hk_qlen, g_hk_dplen, g_hk_dqlen, g_hk_qinvlen;
static int g_hk_crt = 0;
static uint8_t *g_ks_blob; static int g_ks_len;   // ssh-rsa public blob
static int g_hk_ready = 0;

// ---- config (user/pass) ----
static char g_user[64] = "root";
static char g_pass[128] = "maytera";

// ---- wire helpers (mirror of ssh2.c) ----
typedef struct { uint8_t *p; int len, cap; } wb_t;
static void wu8(wb_t *b, uint8_t v) { if (b->len < b->cap) b->p[b->len] = v; b->len++; }
static void wraw(wb_t *b, const void *d, int n) { if (b->len + n <= b->cap) memcpy(b->p + b->len, d, n); b->len += n; }
static void wu32(wb_t *b, uint32_t v) { uint8_t t[4] = {(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v}; wraw(b,t,4); }
static void wstr(wb_t *b, const void *d, int n) { wu32(b,(uint32_t)n); wraw(b,d,n); }
static void wcstr(wb_t *b, const char *s) { wstr(b, s, (int)strlen(s)); }
static void wbool(wb_t *b, int v) { wu8(b, v?1:0); }
static void wmpint(wb_t *b, const uint8_t *d, int n) {
    int i = 0; while (i < n && d[i] == 0) i++;
    if (i == n) { wu32(b, 0); return; }
    int z = (d[i] & 0x80) ? 1 : 0;
    wu32(b, (uint32_t)(n - i + z));
    if (z) wu8(b, 0);
    wraw(b, d + i, n - i);
}
static uint32_t ru32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

static void ctr_inc(uint8_t c[16]) { for (int i=15;i>=0;i--){ if(++c[i]) break; } }
static void ctr_xor(const aes_ctx_t *a, uint8_t ctr[16], uint8_t *data, int len) {
    uint8_t ks[16];
    for (int off=0; off+16<=len; off+=16){ aes_encrypt_block(a,ctr,ks); for(int i=0;i<16;i++) data[off+i]^=ks[i]; ctr_inc(ctr); }
}

// ---- net-lock-wrapped TCP primitives (see net_lock note above) ----
static int L_tcp_send(int s, const void *d, uint16_t n) { net_lock(); int r = tcp_send(s, d, n); net_unlock(); return r; }
static int L_tcp_recv(int s, void *d, uint16_t n)       { net_lock(); int r = tcp_recv(s, d, n); net_unlock(); return r; }
static int L_tcp_accept(int s)                          { net_lock(); int r = tcp_accept(s); net_unlock(); return r; }
static int L_tcp_close(int s)                           { net_lock(); int r = tcp_close(s); net_unlock(); return r; }
static tcp_state_t L_tcp_state(int s)                   { net_lock(); tcp_state_t r = tcp_get_state(s); net_unlock(); return r; }

// ---- per-session state ----
typedef struct {
    int sock;
    char v_c[256], v_s[64];
    x25519_keypair_t eph;
    uint8_t *i_c; int i_c_len;
    uint8_t *i_s; int i_s_len;
    uint8_t session_id[32];
    aes_ctx_t enc_aes; uint8_t enc_ctr[16]; uint8_t enc_mac[32];   // s2c (server->client)
    aes_ctx_t dec_aes; uint8_t dec_ctr[16]; uint8_t dec_mac[32];   // c2s (client->server)
    uint64_t send_seq, recv_seq;
    int encrypting, decrypting;
    uint8_t rbuf[SSH2_RECV_CAP]; int rlen;
    uint8_t sbuf[SSH2_MAX_PACKET + 64];
    uint8_t inbuf[SSH2_MAX_PACKET];
    uint32_t peer_chan, our_chan, local_window, remote_window;
    int closed;
} srv_t;

static int sock_send_all(int sock, const uint8_t *d, int len) {
    int sent = 0; uint64_t hz = g_timer_hz?g_timer_hz:250; uint64_t st = timer_ticks;
    while (sent < len) {
        int r = L_tcp_send(sock, d+sent, (uint16_t)(len-sent>4096?4096:len-sent));
        if (r > 0) { sent+=r; net_poll(); tcp_timer(); st=timer_ticks; continue; }
        if (r == TCP_ERR_WOULD_BLOCK) { net_poll(); tcp_timer(); proc_sleep(1); if (timer_ticks-st>hz*10) return -1; continue; }
        return -1;
    }
    return sent;
}
static int srv_ingest(srv_t *s) {
    if (s->rlen >= SSH2_RECV_CAP) return 0;
    int want = SSH2_RECV_CAP - s->rlen; if (want > 8192) want = 8192;
    int r = L_tcp_recv(s->sock, s->rbuf + s->rlen, (uint16_t)want);
    if (r > 0) { s->rlen += r; return r; }
    if (r == 0 || r == TCP_ERR_WOULD_BLOCK) return 0;
    return -1;
}
// payload[0] = msg type
static int srv_send(srv_t *s, const uint8_t *payload, int plen) {
    int cs = s->encrypting ? 16 : 8;
    int pad = cs - ((4 + 1 + plen) % cs); if (pad < 4) pad += cs;
    int pktlen = 1 + plen + pad; int total = 4 + pktlen;
    if (total > SSH2_MAX_PACKET) return -1;
    uint8_t *b = s->sbuf;
    b[0]=(uint8_t)(pktlen>>24); b[1]=(uint8_t)(pktlen>>16); b[2]=(uint8_t)(pktlen>>8); b[3]=(uint8_t)pktlen;
    b[4]=(uint8_t)pad; memcpy(b+5, payload, plen); rng_get_bytes(b+5+plen, pad);
    if (s->encrypting) {
        uint8_t seq[4]={(uint8_t)(s->send_seq>>24),(uint8_t)(s->send_seq>>16),(uint8_t)(s->send_seq>>8),(uint8_t)s->send_seq};
        hmac_sha256_ctx_t hc; hmac_sha256_init(&hc, s->enc_mac, 32);
        hmac_sha256_update(&hc, seq, 4); hmac_sha256_update(&hc, b, total); hmac_sha256_final(&hc, b+total);
        ctr_xor(&s->enc_aes, s->enc_ctr, b, total);
        s->send_seq++;
        return sock_send_all(s->sock, b, total + SSH2_MAC_LEN);
    }
    s->send_seq++;
    return sock_send_all(s->sock, b, total);
}
// returns 1 got packet (in inbuf, *plen incl type), 0 need more, <0 error
static int srv_extract(srv_t *s, int *plen) {
    if (s->decrypting) {
        if (s->rlen < 16) return 0;
        uint8_t first[16]; memcpy(first, s->rbuf, 16);
        uint8_t cpk[16]; memcpy(cpk, s->dec_ctr, 16);
        ctr_xor(&s->dec_aes, cpk, first, 16);
        uint32_t pktlen = ru32(first); int enc_total = 4 + (int)pktlen;
        if (pktlen < 8 || enc_total > SSH2_MAX_PACKET || (enc_total % 16)) return -1;
        if (s->rlen < enc_total + SSH2_MAC_LEN) return 0;
        memcpy(s->inbuf, s->rbuf, enc_total);
        ctr_xor(&s->dec_aes, s->dec_ctr, s->inbuf, enc_total);
        uint8_t seq[4]={(uint8_t)(s->recv_seq>>24),(uint8_t)(s->recv_seq>>16),(uint8_t)(s->recv_seq>>8),(uint8_t)s->recv_seq};
        uint8_t mac[32]; hmac_sha256_ctx_t hc; hmac_sha256_init(&hc, s->dec_mac, 32);
        hmac_sha256_update(&hc, seq, 4); hmac_sha256_update(&hc, s->inbuf, enc_total); hmac_sha256_final(&hc, mac);
        if (crypto_memcmp(mac, s->rbuf + enc_total, SSH2_MAC_LEN)) return -1;
        int pad = s->inbuf[4]; int pl = (int)pktlen - pad - 1; if (pl < 1) return -1;
        memmove(s->inbuf, s->inbuf+5, pl);
        int consumed = enc_total + SSH2_MAC_LEN;
        memmove(s->rbuf, s->rbuf+consumed, s->rlen-consumed); s->rlen -= consumed;
        s->recv_seq++; *plen = pl; return 1;
    } else {
        if (s->rlen < 4) return 0;
        uint32_t pktlen = ru32(s->rbuf); int total = 4 + (int)pktlen;
        if (pktlen < 5 || total > SSH2_MAX_PACKET) return -1;
        if (s->rlen < total) return 0;
        int pad = s->rbuf[4]; int pl = (int)pktlen - pad - 1; if (pl < 1) return -1;
        memcpy(s->inbuf, s->rbuf+5, pl);
        memmove(s->rbuf, s->rbuf+total, s->rlen-total); s->rlen -= total;
        s->recv_seq++; *plen = pl; return 1;
    }
}
static int srv_expect(srv_t *s, int want, int *plen) {
    uint64_t hz = g_timer_hz?g_timer_hz:250; uint64_t st = timer_ticks;
    for (;;) {
        int r = srv_extract(s, plen);
        if (r < 0) return -1;
        if (r == 1) {
            int t = s->inbuf[0];
            if (t == want) return 0;
            if (t == MSG_DISCONNECT) return -1;
            if (t == MSG_IGNORE || t == MSG_DEBUG) continue;
            if (t == MSG_GLOBAL_REQUEST) { if (*plen>=5){ uint32_t nl=ru32(s->inbuf+1); int wo=1+4+(int)nl; if (wo<*plen && s->inbuf[wo]){ uint8_t f=MSG_REQUEST_FAILURE; srv_send(s,&f,1);} } continue; }
            continue;
        }
        int ing = srv_ingest(s);
        if (ing < 0) return -1;
        net_poll(); tcp_timer(); proc_sleep(2);
        if (timer_ticks - st > hz*15) return -1;
    }
}
static int srv_read_version(srv_t *s) {
    uint64_t hz = g_timer_hz?g_timer_hz:250; uint64_t st = timer_ticks;
    for (;;) {
        for (int i = 0; i < s->rlen; i++) {
            if (s->rbuf[i] == '\n') {
                int ll = i; while (ll>0 && s->rbuf[ll-1]=='\r') ll--;
                if (ll >= 4 && memcmp(s->rbuf,"SSH-",4)==0) {
                    int n = ll < (int)sizeof(s->v_c)-1 ? ll : (int)sizeof(s->v_c)-1;
                    memcpy(s->v_c, s->rbuf, n); s->v_c[n]=0;
                    int c=i+1; memmove(s->rbuf, s->rbuf+c, s->rlen-c); s->rlen-=c; return 0;
                }
                int c=i+1; memmove(s->rbuf, s->rbuf+c, s->rlen-c); s->rlen-=c; i=-1; continue;
            }
        }
        if (srv_ingest(s) < 0) return -1;
        net_poll(); tcp_timer(); proc_sleep(2);
        if (timer_ticks - st > hz*10) return -1;
    }
}
static void derive(const uint8_t *kmp, int kl, const uint8_t *h, char L, const uint8_t *sid, uint8_t *out, int olen) {
    uint8_t d[32]; sha256_ctx_t c; sha256_init(&c);
    sha256_update(&c, kmp, kl); sha256_update(&c, h, 32);
    uint8_t lb=(uint8_t)L; sha256_update(&c, &lb, 1); sha256_update(&c, sid, 32);
    sha256_final(&c, d); memcpy(out, d, olen);
}

// ---- host key load + K_S blob ----
static int load_host_key(void) {
    if (g_hk_ready) return 0;
    uint32_t sz = 0;
    uint8_t *buf = (uint8_t *)fat_read_file(&g_fat_fs, "/CONFIG/SSHHOST.KEY", &sz);
    int crt = (sz >= 8 && memcmp(buf, "MSSHKEY2", 8) == 0);
    if (!buf || sz < 20 || (!crt && memcmp(buf, "MSSHKEY1", 8) != 0)) { if (buf) kfree(buf); return -1; }
    // dup a length-prefixed field at *off, advancing it; returns malloc'd copy.
    int off = 8;
    #define DUPF(dst, dstlen) do { int _l = (int)ru32(buf+off); off+=4; \
        (dst) = kmalloc(_l); memcpy((dst), buf+off, _l); (dstlen) = _l; off+=_l; } while (0)
    DUPF(g_hk_n, g_hk_nlen);
    DUPF(g_hk_e, g_hk_elen);
    DUPF(g_hk_d, g_hk_dlen);
    if (crt) {
        DUPF(g_hk_p, g_hk_plen);  DUPF(g_hk_q, g_hk_qlen);
        DUPF(g_hk_dp, g_hk_dplen); DUPF(g_hk_dq, g_hk_dqlen);
        DUPF(g_hk_qinv, g_hk_qinvlen);
        g_hk_crt = 1;
    }
    #undef DUPF
    if (off > (int)sz) { kfree(buf); return -1; }
    kfree(buf);
    // K_S = string("ssh-rsa") mpint(e) mpint(n)
    static uint8_t ksb[1024]; wb_t w = { ksb, 0, sizeof(ksb) };
    wcstr(&w, "ssh-rsa"); wmpint(&w, g_hk_e, g_hk_elen); wmpint(&w, g_hk_n, g_hk_nlen);
    g_ks_blob = ksb; g_ks_len = w.len;
    g_hk_ready = 1;
    DBG("[SSHD] host key loaded (n=%d e=%d d=%d crt=%d, K_S=%d)\n", g_hk_nlen, g_hk_elen, g_hk_dlen, g_hk_crt, g_ks_len);
    // optional /CONFIG/SSHD.CFG user=/pass=
    uint32_t csz=0; char *cf=(char*)fat_read_file(&g_fat_fs,"/CONFIG/SSHD.CFG",&csz);
    if (cf && csz) {
        char line[160]; int li=0;
        for (uint32_t i=0;i<=csz;i++){ char c=(i<csz)?cf[i]:'\n';
            if (c=='\n'||li>=(int)sizeof(line)-1){ line[li]=0; li=0;
                char *eq=0; for(char*p=line;*p;p++) if(*p=='='){eq=p;break;}
                if(eq){*eq=0; const char*k=line,*v=eq+1; int n2=0; while(v[n2]&&v[n2]!='\r'&&v[n2]!='\n') n2++; char vv[128]; int m=n2<127?n2:127; memcpy(vv,v,m); vv[m]=0;
                    if(!strcmp(k,"user")) strncpy(g_user,vv,sizeof(g_user)-1);
                    else if(!strcmp(k,"pass")) strncpy(g_pass,vv,sizeof(g_pass)-1); }
            } else line[li++]=c;
        }
    }
    if (cf) kfree(cf);
    return 0;
}

// ---- channel <-> pty pump ----
// #445: sends bytes as CHANNEL_DATA but must never push more than
// the peer's advertised channel window (RFC4254 5.2) without a WINDOW_ADJUST.
// Callers are responsible for capping `n` to s->remote_window (see the pty-read
// gate below); this just asserts that discipline and keeps the accounting in
// one place so it can't be missed if another caller is added later.
static void srv_channel_send(srv_t *s, const uint8_t *d, int n) {
    int off = 0;
    while (off < n) {
        int chunk = n - off; if (chunk > 1024) chunk = 1024;
        if ((uint32_t)chunk > s->remote_window) chunk = (int)s->remote_window;
        if (chunk <= 0) break;  // window exhausted mid-buffer; caller must stop reading until it reopens
        uint8_t buf[1200]; wb_t w = { buf, 0, sizeof(buf) };
        wu8(&w, MSG_CHANNEL_DATA); wu32(&w, s->peer_chan); wstr(&w, d+off, chunk);
        if (srv_send(s, buf, w.len) < 0) { s->closed = 1; return; }
        s->remote_window -= (uint32_t)chunk;
        off += chunk;
    }
}

// Is this client public-key blob authorized? /CONFIG/AUTHKEYS holds one
// hex(SHA256(pubkey-blob)) fingerprint per line. Generate with, on Linux:
//   awk '{print $2}' ~/.ssh/id_rsa.pub | base64 -d | sha256sum
static int authkey_allowed(const uint8_t *blob, int bl) {
    uint8_t fp[32]; sha256(blob, bl, fp);
    static const char *HX = "0123456789abcdef";
    char hex[65]; for (int i = 0; i < 32; i++) { hex[i*2]=HX[fp[i]>>4]; hex[i*2+1]=HX[fp[i]&15]; } hex[64]=0;
    uint32_t sz=0; char *f=(char*)fat_read_file(&g_fat_fs,"/CONFIG/AUTHKEYS",&sz);
    if (!f || !sz) { if (f) kfree(f); return 0; }
    int found=0; char line[96]; int li=0;
    for (uint32_t i=0;i<=sz;i++){ char c=(i<sz)?f[i]:'\n';
        if (c=='\n' || li>=(int)sizeof(line)-1){ line[li]=0; li=0;
            int n=0; while(line[n]) n++; while(n>0&&(line[n-1]==' '||line[n-1]=='\r'||line[n-1]=='\t')) line[--n]=0;
            char *p2=line; while(*p2==' '||*p2=='\t') p2++;
            if ((int)strlen(p2)>=64 && memcmp(p2,hex,64)==0) { found=1; break; }
        } else line[li++]=c;
    }
    kfree(f); return found;
}

// Verify a publickey userauth signature. Supports RSA (rsa-sha2-256) and
// Ed25519 (ssh-ed25519). The signed data is:
// string(session_id) byte(50) string(user) string("ssh-connection")
// string("publickey") bool TRUE string(pk-algo) string(pk-blob).
static int srv_pubkey_verify(srv_t *s, const char *user, const uint8_t *algo, int al,
                             const uint8_t *blob, int bl, const uint8_t *sig, int sl) {
    // The signed data is identical for all key types; build it once.
    static uint8_t db[2048]; wb_t w={db,0,sizeof(db)};
    wstr(&w, s->session_id, 32); wu8(&w, MSG_USERAUTH_REQUEST);
    wcstr(&w, user); wcstr(&w, "ssh-connection"); wcstr(&w, "publickey");
    wbool(&w, 1); wstr(&w, algo, al); wstr(&w, blob, bl);
    if (w.len > w.cap) return 0;

    // Parse the key type from the public-key blob (first string).
    const uint8_t *p=blob; int r=bl;
    if (r < 4) return 0;
    uint32_t tl=ru32(p); p+=4; r-=4;
    if ((int)tl > r) return 0;
    const uint8_t *ktype=p; p+=tl; r-=tl;

    if (tl==11 && memcmp(ktype,"ssh-ed25519",11)==0) {
        // blob: string("ssh-ed25519") string(A[32]). sig: string("ssh-ed25519") string(raw[64]).
        if (r < 4) return 0;
        uint32_t akl=ru32(p); p+=4; r-=4;
        if (akl != 32 || (int)akl > r) return 0;
        const uint8_t *A=p;
        const uint8_t *sp=sig; int sr=sl;
        if (sr < 4) return 0;
        uint32_t sal=ru32(sp); sp+=4; sr-=4;
        if (!(sal==11 && memcmp(sp,"ssh-ed25519",11)==0)) return 0;
        sp+=sal; sr-=sal;
        if (sr < 4) return 0;
        uint32_t sbl=ru32(sp); sp+=4; sr-=4;
        if (sbl != 64 || (int)sbl > sr) return 0;
        return ed25519_verify(sp, db, w.len, A);
    }

    if (tl==7 && memcmp(ktype,"ssh-rsa",7)==0) {
        // blob: string("ssh-rsa") mpint e mpint n. sig: string("rsa-sha2-256") string(sig).
        if (r < 4) return 0;
        uint32_t el=ru32(p); p+=4; r-=4;
        if ((int)el > r) return 0;
        const uint8_t *e=p; p+=el; r-=el;
        if (r < 4) return 0;
        uint32_t nl=ru32(p); p+=4; r-=4;
        if ((int)nl > r) return 0;
        const uint8_t *n=p;
        while (el>1 && e[0]==0) { e++; el--; }
        while (nl>1 && n[0]==0) { n++; nl--; }
        const uint8_t *sp=sig; int sr=sl;
        if (sr < 4) return 0;
        uint32_t sal=ru32(sp); sp+=4; sr-=4;
        if ((int)sal > sr) return 0;
        int ok256 = (sal==12 && memcmp(sp,"rsa-sha2-256",12)==0);
        sp+=sal; sr-=sal;
        if (!ok256) return 0;   // ssh-rsa (SHA1) not supported
        if (sr < 4) return 0;
        uint32_t sbl=ru32(sp); sp+=4; sr-=4;
        if ((int)sbl > sr) return 0;
        const uint8_t *sb=sp;
        uint8_t hh[32]; sha256(db, w.len, hh);
        rsa_public_key_t pub = { (uint8_t*)n, (size_t)nl, (uint8_t*)e, (size_t)el };
        return rsa_verify_pkcs1_sha256(&pub, hh, 32, sb, sbl) == 0;
    }

    return 0;  // unsupported key type
}

static void handle_session(srv_t *s) {
    int plen;
    // version exchange
    { const char *vs="SSH-2.0-MayteraOS_1.0"; strncpy(s->v_s, vs, sizeof(s->v_s)-1);
      char line[64]; int n=0; for(const char*p=vs;*p;p++) line[n++]=*p; line[n++]='\r'; line[n++]='\n'; line[n]=0;
      if (sock_send_all(s->sock,(uint8_t*)line,n)<0) return; }
    if (srv_read_version(s) != 0) { DBG("[SSHD] version read fail\n"); return; }
    DBG("[SSHD] client version: %s\n", s->v_c);

    // our KEXINIT
    { uint8_t buf[512]; wb_t w={buf,0,sizeof(buf)};
      wu8(&w, MSG_KEXINIT); uint8_t ck[16]; rng_get_bytes(ck,16); wraw(&w,ck,16);
      wcstr(&w,"curve25519-sha256,ext-info-s"); wcstr(&w,"rsa-sha2-256");
      wcstr(&w,"aes256-ctr"); wcstr(&w,"aes256-ctr");
      wcstr(&w,"hmac-sha2-256"); wcstr(&w,"hmac-sha2-256");
      wcstr(&w,"none"); wcstr(&w,"none"); wcstr(&w,""); wcstr(&w,"");
      wbool(&w,0); wu32(&w,0);
      s->i_s = kmalloc(w.len); memcpy(s->i_s, buf, w.len); s->i_s_len = w.len;
      if (srv_send(s, buf, w.len) < 0) return; }
    if (srv_expect(s, MSG_KEXINIT, &plen) != 0) { DBG("[SSHD] no client KEXINIT\n"); return; }
    s->i_c = kmalloc(plen); memcpy(s->i_c, s->inbuf, plen); s->i_c_len = plen;

    // ECDH init from client: string Q_C
    if (srv_expect(s, MSG_ECDH_INIT, &plen) != 0) { DBG("[SSHD] no ECDH_INIT\n"); return; }
    if (plen < 5) return;
    uint32_t qcl = ru32(s->inbuf+1); if (qcl != 32 || (int)qcl > plen-5) return;
    uint8_t qc[32]; memcpy(qc, s->inbuf+5, 32);

    x25519_generate_keypair(&s->eph);
    uint8_t shared[32]; x25519_shared_secret(qc, s->eph.private_key, shared);
    uint8_t kmp[40]; wb_t kw={kmp,0,sizeof(kmp)}; wmpint(&kw, shared, 32);

    // exchange hash H
    uint8_t H[32];
    { uint8_t hb[4096]; wb_t hw={hb,0,sizeof(hb)};
      wstr(&hw, s->v_c, (int)strlen(s->v_c)); wstr(&hw, s->v_s, (int)strlen(s->v_s));
      wstr(&hw, s->i_c, s->i_c_len); wstr(&hw, s->i_s, s->i_s_len);
      wstr(&hw, g_ks_blob, g_ks_len);
      wstr(&hw, qc, 32); wstr(&hw, s->eph.public_key, 32);
      wraw(&hw, kmp, kw.len);
      if (hw.len > hw.cap) return;
      sha256(hb, hw.len, H); }
    memcpy(s->session_id, H, 32);

    // sign H: sig = RSA-sign(SHA256(H)); sigblob = string("rsa-sha2-256") string(sig)
    uint8_t hh[32]; sha256(H, 32, hh);
    uint8_t sigbytes[512];
    rsa_private_key_t pk;
    memset(&pk, 0, sizeof(pk));
    pk.n = g_hk_n; pk.n_len = g_hk_nlen; pk.d = g_hk_d; pk.d_len = g_hk_dlen;
    if (g_hk_crt) {
        pk.p = g_hk_p; pk.p_len = g_hk_plen; pk.q = g_hk_q; pk.q_len = g_hk_qlen;
        pk.dp = g_hk_dp; pk.dp_len = g_hk_dplen; pk.dq = g_hk_dq; pk.dq_len = g_hk_dqlen;
        pk.qinv = g_hk_qinv; pk.qinv_len = g_hk_qinvlen;
    }
    DBG("[SSHD] signing exchange hash...\n");
    if (rsa_sign_pkcs1_sha256(&pk, hh, 32, sigbytes, sizeof(sigbytes)) != 0) { DBG("[SSHD] sign failed\n"); return; }
    // signature length = modulus size (strip leading zero of n)
    int klen = g_hk_nlen; const uint8_t *nn=g_hk_n; while (klen>1 && nn[0]==0){nn++;klen--;}
    DBG("[SSHD] signed (siglen=%d)\n", klen);

    // KEX_ECDH_REPLY: string K_S, string Q_S, string sigblob
    { uint8_t buf[2048]; wb_t w={buf,0,sizeof(buf)};
      wu8(&w, MSG_ECDH_REPLY);
      wstr(&w, g_ks_blob, g_ks_len);
      wstr(&w, s->eph.public_key, 32);
      uint8_t sb[600]; wb_t sw={sb,0,sizeof(sb)}; wcstr(&sw,"rsa-sha2-256"); wstr(&sw, sigbytes, klen);
      wstr(&w, sb, sw.len);
      if (w.len > w.cap) { DBG("[SSHD] reply too big\n"); return; }
      if (srv_send(s, buf, w.len) < 0) return; }

    // derive keys: server enc=s2c (B,D,F), dec=c2s (A,C,E)
    { uint8_t kc2s[32],ks2c[32],mc2s[32],ms2c[32],ivc2s[16],ivs2c[16];
      derive(kmp,kw.len,H,'A',s->session_id,ivc2s,16);
      derive(kmp,kw.len,H,'B',s->session_id,ivs2c,16);
      derive(kmp,kw.len,H,'C',s->session_id,kc2s,32);
      derive(kmp,kw.len,H,'D',s->session_id,ks2c,32);
      derive(kmp,kw.len,H,'E',s->session_id,mc2s,32);
      derive(kmp,kw.len,H,'F',s->session_id,ms2c,32);
      aes_set_encrypt_key(&s->enc_aes, ks2c, 256); memcpy(s->enc_ctr, ivs2c, 16); memcpy(s->enc_mac, ms2c, 32);
      aes_set_encrypt_key(&s->dec_aes, kc2s, 256); memcpy(s->dec_ctr, ivc2s, 16); memcpy(s->dec_mac, mc2s, 32); }

    { uint8_t nk=MSG_NEWKEYS; if (srv_send(s,&nk,1)<0) return; s->encrypting=1; }
    // RFC 8308 EXT_INFO: advertise server-sig-algs so the client will use
    // rsa-sha2-256 for publickey auth (modern OpenSSH refuses SHA-1 ssh-rsa,
    // and without this it says "no mutual signature algorithm" and never even
    // sends the publickey request). Sent encrypted, as the first post-NEWKEYS pkt.
    { uint8_t buf[128]; wb_t w={buf,0,sizeof(buf)};
      wu8(&w, MSG_EXT_INFO); wu32(&w, 1);
      // Advertise the client-auth signature algorithms srv_pubkey_verify can
      // check: rsa-sha2-256 (RSA) and ssh-ed25519 (Ed25519). NOT rsa-sha2-512
      // (no SHA512-RSA verify path) - if listed the client prefers it for RSA
      // keys and we could not verify.
      wcstr(&w, "server-sig-algs"); wcstr(&w, "rsa-sha2-256,ssh-ed25519");
      if (srv_send(s, buf, w.len) < 0) return; }
    if (srv_expect(s, MSG_NEWKEYS, &plen) != 0) return;
    s->decrypting = 1;
    DBG("[SSHD] keys exchanged, encrypted\n");

    // service request
    if (srv_expect(s, MSG_SERVICE_REQUEST, &plen) != 0) return;
    { uint8_t buf[64]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,MSG_SERVICE_ACCEPT); wcstr(&w,"ssh-userauth"); if (srv_send(s,buf,w.len)<0) return; }

    // userauth loop
    int authed = 0;
    for (int tries = 0; tries < 8 && !authed; tries++) {
        if (srv_expect(s, MSG_USERAUTH_REQUEST, &plen) != 0) return;
        const uint8_t *p = s->inbuf+1; int rem = plen-1;
        if (rem < 4) return;
        uint32_t ul=ru32(p); p+=4; rem-=4; char user[64]; int um=(ul<63)?(int)ul:63; if((int)ul>rem) return; memcpy(user,p,um); user[um]=0; p+=ul; rem-=ul;
        uint32_t sl=ru32(p); p+=4; rem-=4; if((int)sl>rem) return; p+=sl; rem-=sl;  // service
        uint32_t ml=ru32(p); p+=4; rem-=4; char meth[32]; int mm=(ml<31)?(int)ml:31; if((int)ml>rem) return; memcpy(meth,p,mm); meth[mm]=0; p+=ml; rem-=ml;
        if (strcmp(meth, "password") == 0 && rem >= 1) {
            p+=1; rem-=1;  // bool FALSE
            uint32_t pl2=ru32(p); p+=4; rem-=4; char pass[128]; int pm=(pl2<127)?(int)pl2:127; if((int)pl2>rem) return; memcpy(pass,p,pm); pass[pm]=0;
            if (strcmp(user, g_user)==0 && strcmp(pass, g_pass)==0) {
                uint8_t b=MSG_USERAUTH_SUCCESS; srv_send(s,&b,1); authed=1;
                DBG("[SSHD] password auth OK for %s\n", user);
            } else {
                uint8_t buf[64]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,MSG_USERAUTH_FAILURE); wcstr(&w,"publickey,password"); wbool(&w,0); srv_send(s,buf,w.len);
                DBG("[SSHD] password auth FAIL for %s\n", user);
            }
        } else if (strcmp(meth, "publickey") == 0 && rem >= 1) {
            int has_sig = p[0]; p+=1; rem-=1;
            uint32_t al=ru32(p); p+=4; rem-=4; const uint8_t *algo=p; if((int)al>rem) return; p+=al; rem-=al;
            uint32_t bl=ru32(p); p+=4; rem-=4; const uint8_t *blob=p; if((int)bl>rem) return; p+=bl; rem-=bl;
            int allowed = (strcmp(user, g_user)==0) && authkey_allowed(blob, (int)bl);
            if (!has_sig) {
                if (allowed) {  // PK_OK: echo algo + blob, client will re-send signed
                    uint8_t buf[1024]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,MSG_USERAUTH_PK_OK); wstr(&w,algo,(int)al); wstr(&w,blob,(int)bl);
                    if (w.len<=w.cap) srv_send(s,buf,w.len);
                } else {
                    uint8_t buf[64]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,MSG_USERAUTH_FAILURE); wcstr(&w,"publickey,password"); wbool(&w,0); srv_send(s,buf,w.len);
                }
            } else {
                uint32_t sgl=ru32(p); p+=4; rem-=4; const uint8_t *sg=p; if((int)sgl>rem) return;
                int vok = srv_pubkey_verify(s, user, algo, (int)al, blob, (int)bl, sg, (int)sgl);
                if (allowed && vok) {
                    uint8_t b=MSG_USERAUTH_SUCCESS; srv_send(s,&b,1); authed=1;
                    DBG("[SSHD] pubkey auth OK for %s\n", user);
                } else {
                    uint8_t buf[64]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,MSG_USERAUTH_FAILURE); wcstr(&w,"publickey,password"); wbool(&w,0); srv_send(s,buf,w.len);
                    DBG("[SSHD] pubkey auth FAIL for %s\n", user);
                }
            }
        } else {
            uint8_t buf[64]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,MSG_USERAUTH_FAILURE); wcstr(&w,"publickey,password"); wbool(&w,0); srv_send(s,buf,w.len);
        }
    }
    if (!authed) return;

    // channel open
    if (srv_expect(s, MSG_CHANNEL_OPEN, &plen) != 0) return;
    { const uint8_t *p=s->inbuf+1; uint32_t tl=ru32(p); p+=4+tl; s->peer_chan=ru32(p); s->remote_window=ru32(p+4); }
    s->our_chan = 0; s->local_window = (1u<<21);
    { uint8_t buf[64]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,MSG_CHANNEL_OPEN_CONFIRMATION);
      wu32(&w, s->peer_chan); wu32(&w, s->our_chan); wu32(&w, s->local_window); wu32(&w, 32768);
      if (srv_send(s,buf,w.len)<0) return; }
    DBG("[SSHD] channel open (peer=%u)\n", s->peer_chan);

    // spawn shell on a pty
    struct file *master = dev_open("ptmx", 0x0002 | 0x0800);
    if (!master) { DBG("[SSHD] ptmx fail\n"); return; }
    int pts_idx = -1;
    if (file_ioctl(master, 0x80045430, &pts_idx) != 0 || pts_idx < 0) { file_put(master); return; }
    uint32_t msz=0; void *mdata = fat_read_file(&g_fat_fs, "/APPS/MSH", &msz);
    if (!mdata || !msz) { if(mdata) kfree(mdata); file_put(master); return; }
    int child_started = 0;

    // channel-request loop until shell starts, then pump
    process_t *child = 0; int child_pid = -1;
    uint8_t io[512];
    uint64_t hz = g_timer_hz?g_timer_hz:250; uint64_t idle_start = timer_ticks;
    for (;;) {
        if (s->closed) break;
        tcp_state_t st = L_tcp_state(s->sock);
        if (st == TCP_STATE_CLOSED || st == TCP_STATE_CLOSE_WAIT || st == TCP_STATE_LAST_ACK) break;
        if (child_started && child) {
            if (child->state == PROC_STATE_ZOMBIE || child->state == PROC_STATE_UNUSED) {
                // #445 follow-up: don't close the instant the child exits -- once output is
                // properly window-gated (see below), a command can finish writing and exit
                // while its last bytes are still sitting unread in the pty, waiting on a
                // closed remote_window. Closing here unconditionally silently dropped that
                // tail. Only finish up once the pty truly has nothing left to drain; if it
                // still does, fall through to the normal pty-read path (which respects the
                // window) and re-check on the next iteration.
                if (!(file_poll(master, 0x01) & 0x01)) {
                    uint8_t buf[16]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,MSG_CHANNEL_EOF); wu32(&w,s->peer_chan); srv_send(s,buf,w.len);
                    wb_t w2={buf,0,sizeof(buf)}; wu8(&w2,MSG_CHANNEL_CLOSE); wu32(&w2,s->peer_chan); srv_send(s,buf,w2.len);
                    break;
                }
            }
        }
        int did = 0;
        // pty master -> channel. Gate on the peer's remote_window: without this, large
        // exec output (e.g. `cat bigfile`) got blasted onto the TCP socket regardless of
        // the SSH channel flow-control window, and a strict client (OpenSSH disconnects
        // on "rcvd too much data") would tear down the session once its advertised window
        // was exceeded -- silently truncating the command's stdout at exactly that many
        // bytes even though the underlying pty/file data was all present. Fix: never read
        // more from the pty than we currently have window to forward, and simply skip the
        // read (leaving bytes buffered in the pty) when the window is fully closed; the
        // MSG_CHANNEL_WINDOW_ADJUST handler below reopens it as the client drains its side,
        // and this same loop iteration's inbound-packet processing keeps servicing that
        // while we wait, so no extra polling/blocking primitive is needed (net_poll/
        // tcp_timer/proc_sleep below already run every iteration).
        if (child_started && s->remote_window > 0) {
            int want = (int)sizeof(io);
            if ((uint32_t)want > s->remote_window) want = (int)s->remote_window;
            int mp = file_poll(master, 0x01);
            if (mp & 0x01) { int64_t n = file_read(master, io, (size_t)want); if (n > 0) { srv_channel_send(s, io, (int)n); did=1; } }
            else if (mp & 0x10) { uint8_t buf[16]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,MSG_CHANNEL_CLOSE); wu32(&w,s->peer_chan); srv_send(s,buf,w.len); break; }
        }
        // inbound packets
        int pl2;
        int r = srv_extract(s, &pl2);
        if (r < 0) break;
        if (r == 1) {
            did = 1;
            int t = s->inbuf[0]; const uint8_t *p = s->inbuf+1;
            if (t == MSG_CHANNEL_REQUEST) {
                uint32_t rc = ru32(p); (void)rc; uint32_t tl=ru32(p+4); const char *rt=(const char*)(p+8);
                int want_reply = (8+(int)tl < pl2-1) ? p[8+tl] : 0;
                int is_shell = (tl==5 && memcmp(rt,"shell",5)==0);
                int is_exec  = (tl==4 && memcmp(rt,"exec",4)==0);
                int is_pty   = (tl==7 && memcmp(rt,"pty-req",7)==0);
                if ((is_shell || is_exec) && !child_started) {
                    child_pid = proc_create_user_tty("msh", mdata, msz, pts_idx);
                    if (child_pid > 0) {
                        child = proc_get((uint32_t)child_pid); child_started = 1;
                        DBG("[SSHD] shell pid=%d on pts/%d\n", child_pid, pts_idx);
                        if (is_exec) {
                            // #442: a plain `ssh host "cmd"` exec (no -t) expects
                            // byte-for-byte stdout, but this pty defaults to cooked
                            // mode (OPOST|ONLCR|ECHO). Left as-is, every 0x0A byte
                            // the command writes became "\r\n" AND the "cmd\nexit\n"
                            // we inject below got echoed back into the same output
                            // stream -- both corrupt the exec channel's bytes (a
                            // `cat` of a binary/random file never matched its md5,
                            // independent of the TCP-layer truncation fixed above).
                            // Drop OPOST/ONLCR/OCRNL/ONLRET + ECHO/ECHOE/ECHOK/ECHONL
                            // for this pty; keep ICANON/ISIG so msh's own line-based
                            // command read and signal handling are unaffected.
                            struct termios tio;
                            if (file_ioctl(master, TCGETS, &tio) == 0) {
                                tio.c_oflag &= ~(OPOST | ONLCR | OCRNL | ONLRET);
                                tio.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
                                file_ioctl(master, TCSETS, &tio);
                            }
                            // exec request: byte msg,u32 chan,string "exec",bool,string command
                            int co = 8 + (int)tl + 1;            // after type string + want_reply
                            if (co + 4 <= pl2 - 1) {
                                uint32_t cl = ru32(p + co); const uint8_t *cmd = p + co + 4;
                                if (cl > 0 && (int)(co + 4 + cl) <= pl2 - 1) {
                                    file_write(master, cmd, cl);
                                    file_write(master, "\nexit\n", 6);  // run it, then close the session
                                }
                            }
                        }
                    }
                }
                if (want_reply) { uint8_t b = (is_shell||is_exec||is_pty)?MSG_CHANNEL_SUCCESS:MSG_CHANNEL_FAILURE; uint8_t buf[8]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,b); wu32(&w,s->peer_chan); srv_send(s,buf,w.len); }
            } else if (t == MSG_CHANNEL_DATA) {
                uint32_t dl=ru32(p+4); const uint8_t *d=p+8; if ((int)dl > pl2-9) dl = pl2-9>0?pl2-9:0;
                if (child_started && dl) file_write(master, d, dl);
                if (s->local_window >= dl) s->local_window -= dl;
                if (s->local_window < (1u<<18)) { uint32_t add=(1u<<21)-s->local_window; uint8_t buf[16]; wb_t w={buf,0,sizeof(buf)}; wu8(&w,MSG_CHANNEL_WINDOW_ADJUST); wu32(&w,s->peer_chan); wu32(&w,add); if(srv_send(s,buf,w.len)>=0) s->local_window+=add; }
            } else if (t == MSG_CHANNEL_WINDOW_ADJUST) {
                s->remote_window += ru32(p+4);
            } else if (t == MSG_CHANNEL_EOF) {
                /* client closed stdin */
            } else if (t == MSG_CHANNEL_CLOSE || t == MSG_DISCONNECT) {
                break;
            } else if (t == MSG_GLOBAL_REQUEST) {
                uint32_t nl=ru32(p); int wo=4+(int)nl; if (wo<pl2-1 && p[wo]){ uint8_t f=MSG_REQUEST_FAILURE; srv_send(s,&f,1);}
            }
        } else {
            if (srv_ingest(s) < 0) break;
        }
        if (!did) { net_poll(); tcp_timer(); proc_sleep(8); }
        else { net_poll(); tcp_timer(); }
        if (!child_started && timer_ticks - idle_start > hz*60) break;  // no shell within 60s
    }

    // teardown
    file_put(master);
    if (mdata) kfree(mdata);
    DBG("[SSHD] session closed\n");
}

extern int  proc_create_ex(const char *name, void (*entry)(void *), void *arg,
                           process_priority_t priority, uint32_t stack_size);
extern int  proc_reap(uint32_t pid);

static int g_listen = -1;
static volatile int g_running = 0;

// #435: up to this many SSH sessions run at once, each on its OWN worker
// thread. The listener thread never blocks inside a session, so a slow or
// half-open client (e.g. a double-hopped ssh that stalls in banner/handshake)
// only holds its own worker until that worker times out; it can no longer
// wedge the listener and starve every subsequent connection. Before this fix
// the single blocking accept loop served exactly one session per boot then
// appeared dead for ~60s per stuck client.
#define SSH_MAX_SESSIONS 4
static int g_sess_pids[SSH_MAX_SESSIONS];

// Per-connection worker: runs one SSH session to completion on its own thread,
// then closes the socket. Returning falls through proc_wrapper -> proc_exit(0);
// the listener reaps the zombie slot.
static void ssh_session_worker(void *arg) {
    int c = (int)(long)arg;
    srv_t *s = (srv_t *)kmalloc(sizeof(srv_t));
    if (s) {
        memset(s, 0, sizeof(*s));
        s->sock = c;
        handle_session(s);
        if (s->i_c) kfree(s->i_c);
        if (s->i_s) kfree(s->i_s);
        kfree(s);
    }
    L_tcp_close(c);
}

static void ssh_server_thread(void *arg) {
    (void)arg;
    if (load_host_key() != 0) { kprintf("[SSHD] no /CONFIG/SSHHOST.KEY - server not started\n"); return; }
    g_listen = tcp_socket();
    if (g_listen < 0) { kprintf("[SSHD] socket fail\n"); return; }
    if (tcp_bind(g_listen, 22) < 0) { kprintf("[SSHD] bind 22 fail\n"); return; }
    if (tcp_listen(g_listen, SSH_MAX_SESSIONS) < 0) { kprintf("[SSHD] listen fail\n"); return; }
    for (int i = 0; i < SSH_MAX_SESSIONS; i++) g_sess_pids[i] = 0;
    g_running = 1;
    kprintf("[SSHD] listening on port 22 (max %d concurrent sessions)\n", SSH_MAX_SESSIONS);
    while (g_running) {
        // Reap finished workers and count how many are still live.
        int active = 0, freeslot = -1;
        for (int i = 0; i < SSH_MAX_SESSIONS; i++) {
            if (g_sess_pids[i] > 0) {
                process_t *w = proc_get((uint32_t)g_sess_pids[i]);
                if (!w || w->state == PROC_STATE_ZOMBIE || w->state == PROC_STATE_UNUSED) {
                    proc_reap((uint32_t)g_sess_pids[i]);
                    g_sess_pids[i] = 0;
                    if (freeslot < 0) freeslot = i;
                } else {
                    active++;
                }
            } else if (freeslot < 0) {
                freeslot = i;
            }
        }
        // Accept a new connection if we have capacity. On success loop straight
        // back (no sleep) to drain any further pending connections quickly.
        if (active < SSH_MAX_SESSIONS && freeslot >= 0) {
            int c = L_tcp_accept(g_listen);
            if (c >= 0) {
                // Sessions need a big kernel stack (RSA sign, x25519, sha256,
                // several KB of on-stack wire buffers) - use proc_create_ex.
                int pid = proc_create_ex("sshsess", ssh_session_worker,
                                         (void *)(long)c, PRIO_NORMAL, 96 * 1024);
                if (pid > 0) { g_sess_pids[freeslot] = pid; continue; }
                L_tcp_close(c);   // could not spawn a worker - drop, client retries
            }
        }
        // Idle (or all session slots busy): drive the net stack and yield.
        net_poll(); tcp_timer(); proc_sleep(20);
    }
}

void ssh_server_start(void) {
    if (g_running) { kprintf("[SSHD] already running\n"); return; }
    // PRIO_NORMAL (not PRIO_LOW): a CPU-busy userland app (e.g. the AI-chat
    // widget) runs at NORMAL and, under the strict-priority scheduler, would
    // starve a PRIO_LOW sshd indefinitely - the box stays alive but SSHD goes
    // silent. NORMAL lets the listener round-robin fairly and keep answering.
    proc_create("sshd", ssh_server_thread, 0, PRIO_NORMAL);
}
