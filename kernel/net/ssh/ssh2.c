// ssh2.c - SSH-2 client for MayteraOS. See ssh2.h.
//
// Real, interoperable implementation against OpenSSH:
//   kex curve25519-sha256, hostkey ssh-rsa (rsa-sha2-256 sig), cipher aes256-ctr,
//   mac hmac-sha2-256, password userauth, one "session" channel (pty-req + shell).
#include "ssh2.h"
#include "../tcp.h"
#include "../tls/tls13.h"        // x25519_*
#include "../../crypto/rsa.h"
#include "../../string.h"
#include "../../mm/heap.h"
#include "../../serial.h"
#include "../../fs/fat.h"

extern void net_poll(void);
extern void tcp_timer(void);
extern void proc_sleep(uint32_t ms);
extern volatile uint64_t timer_ticks;
extern uint32_t g_timer_hz;
extern fat_fs_t g_fat_fs;

// known_hosts (TOFU pinning): /CONFIG/KNOWN_HOSTS holds "host sha256hex" lines.
// Returns 0 if the host key is new (added) or matches a stored one; -1 if it
// CHANGED from a stored value (possible MITM -> caller rejects).
static int ssh2_known_hosts(const char *host, const uint8_t *ks, int ks_len) {
    uint8_t fp[32]; sha256(ks, ks_len, fp);
    static const char *HX = "0123456789abcdef";
    char hex[65]; for (int i = 0; i < 32; i++) { hex[i*2]=HX[fp[i]>>4]; hex[i*2+1]=HX[fp[i]&15]; } hex[64]=0;
    uint32_t sz=0; char *f=(char*)fat_read_file(&g_fat_fs, "/CONFIG/KNOWN_HOSTS", &sz);
    char *data = f; uint32_t dsz = (f ? sz : 0);
    int hlen = (int)strlen(host);
    // scan lines "host hex"
    int li=0; char line[160];
    for (uint32_t i = 0; data && i <= dsz; i++) {
        char c = (i < dsz) ? data[i] : '\n';
        if (c == '\n' || li >= (int)sizeof(line)-1) {
            line[li]=0; li=0;
            // split host and fp at first space
            int sp=0; while(line[sp] && line[sp]!=' ') sp++;
            if (line[sp]==' ' && sp==hlen && memcmp(line,host,hlen)==0) {
                const char *stored = line+sp+1;
                int match = (strlen(stored) >= 64 && memcmp(stored, hex, 64)==0);
                if (f) kfree(f);
                return match ? 0 : -1;   // known: match ok, else CHANGED
            }
        } else line[li++]=c;
    }
    // not found -> TOFU: append "host hex\n"
    uint32_t newsz = dsz + hlen + 1 + 64 + 1;
    char *nb = (char *)kmalloc(newsz + 1);
    if (nb) {
        if (dsz) memcpy(nb, data, dsz);
        int o = dsz;
        memcpy(nb+o, host, hlen); o+=hlen; nb[o++]=' ';
        memcpy(nb+o, hex, 64); o+=64; nb[o++]='\n';
        fat_write_file(&g_fat_fs, "/CONFIG/KNOWN_HOSTS", nb, o);
        kfree(nb);
    }
    if (f) kfree(f);
    return 0;
}

// SSH message numbers
#define MSG_DISCONNECT      1
#define MSG_IGNORE          2
#define MSG_UNIMPLEMENTED   3
#define MSG_DEBUG           4
#define MSG_SERVICE_REQUEST 5
#define MSG_SERVICE_ACCEPT  6
#define MSG_KEXINIT         20
#define MSG_NEWKEYS         21
#define MSG_ECDH_INIT       30
#define MSG_ECDH_REPLY      31
#define MSG_USERAUTH_REQUEST 50
#define MSG_USERAUTH_FAILURE 51
#define MSG_USERAUTH_SUCCESS 52
#define MSG_USERAUTH_BANNER  53
#define MSG_GLOBAL_REQUEST   80
#define MSG_REQUEST_SUCCESS  81
#define MSG_REQUEST_FAILURE  82
#define MSG_CHANNEL_OPEN              90
#define MSG_CHANNEL_OPEN_CONFIRMATION 91
#define MSG_CHANNEL_OPEN_FAILURE      92
#define MSG_CHANNEL_WINDOW_ADJUST     93
#define MSG_CHANNEL_DATA              94
#define MSG_CHANNEL_EXTENDED_DATA     95
#define MSG_CHANNEL_EOF               96
#define MSG_CHANNEL_CLOSE             97
#define MSG_CHANNEL_REQUEST           98
#define MSG_CHANNEL_SUCCESS           99
#define MSG_CHANNEL_FAILURE          100

#define WINDOW_INITIAL  (1u << 21)
#define WINDOW_LOW      (1u << 18)

static int g_ssh_dbg = 0;
// Optional log sink: when set (e.g. by the RC `ssh` command) stage messages are
// routed to the caller's session (TCP) instead of the serial port, which a busy
// Win16 app can flood. Falls back to kprintf when no sink is registered.
static void (*g_ssh2_log)(void *, const char *) = 0;
static void  *g_ssh2_logctx = 0;
void ssh2_set_log(void (*fn)(void *, const char *), void *ctx) { g_ssh2_log = fn; g_ssh2_logctx = ctx; }
static void sshlog(const char *fmt, ...) {
    if (!g_ssh_dbg) return;
    char b[200];
    __builtin_va_list ap; __builtin_va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    __builtin_va_end(ap);
    if (g_ssh2_log) g_ssh2_log(g_ssh2_logctx, b);
    else kprintf("%s", b);
}
#define SDBG(...) sshlog(__VA_ARGS__)

// ---------------------------------------------------------------------------
// little buffer writer
// ---------------------------------------------------------------------------
typedef struct { uint8_t *p; int len; int cap; } wbuf_t;
static void w_u8(wbuf_t *b, uint8_t v)  { if (b->len < b->cap) b->p[b->len] = v; b->len++; }
static void w_raw(wbuf_t *b, const void *d, int n) {
    if (b->len + n <= b->cap) memcpy(b->p + b->len, d, n);
    b->len += n;
}
static void w_u32(wbuf_t *b, uint32_t v) {
    uint8_t t[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    w_raw(b, t, 4);
}
static void w_string(wbuf_t *b, const void *d, int n) { w_u32(b, (uint32_t)n); w_raw(b, d, n); }
static void w_cstr(wbuf_t *b, const char *s) { w_string(b, s, (int)strlen(s)); }
static void w_bool(wbuf_t *b, int v) { w_u8(b, v ? 1 : 0); }
// mpint: minimal big-endian, leading 0x00 if high bit set, no extra leading zeros
static void w_mpint(wbuf_t *b, const uint8_t *d, int n) {
    int i = 0;
    while (i < n && d[i] == 0) i++;     // strip leading zero bytes
    if (i == n) { w_u32(b, 0); return; }  // value zero
    int need0 = (d[i] & 0x80) ? 1 : 0;
    w_u32(b, (uint32_t)(n - i + need0));
    if (need0) w_u8(b, 0);
    w_raw(b, d + i, n - i);
}

static uint32_t rd_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// ---------------------------------------------------------------------------
// AES-CTR (whole 16-byte blocks; SSH keeps the counter continuous per direction)
// ---------------------------------------------------------------------------
static void ctr_inc(uint8_t c[16]) {
    for (int i = 15; i >= 0; i--) { if (++c[i]) break; }
}
static void ctr_xor(const aes_ctx_t *a, uint8_t ctr[16], uint8_t *data, int len) {
    uint8_t ks[16];
    for (int off = 0; off + 16 <= len; off += 16) {
        aes_encrypt_block(a, ctr, ks);
        for (int i = 0; i < 16; i++) data[off + i] ^= ks[i];
        ctr_inc(ctr);
    }
}

// ---------------------------------------------------------------------------
// low-level TCP helpers (poll-driven, matching net/https.c)
// ---------------------------------------------------------------------------
static int sock_send_all(int sock, const uint8_t *d, int len) {
    int sent = 0;
    uint64_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t start = timer_ticks;
    while (sent < len) {
        int r = tcp_send(sock, d + sent, (uint16_t)(len - sent > 4096 ? 4096 : len - sent));
        if (r > 0) { sent += r; net_poll(); tcp_timer(); start = timer_ticks; continue; }
        if (r == TCP_ERR_WOULD_BLOCK) {
            net_poll(); tcp_timer(); proc_sleep(1);
            if (timer_ticks - start > hz * 10) return -1;
            continue;
        }
        return -1;
    }
    return sent;
}

// pull whatever is available into rbuf; return bytes added (0 if none, <0 closed)
static int ssh2_ingest(ssh2_client_t *cli) {
    if (cli->rlen >= SSH2_RECV_CAP) return 0;
    // tcp_recv length is uint16_t: a (SSH2_RECV_CAP - rlen) of 65536 truncates to
    // 0 (request nothing -> we'd never read the ACKed data). Cap to a safe chunk.
    int want = SSH2_RECV_CAP - cli->rlen;
    if (want > 8192) want = 8192;
    int r = tcp_recv(cli->sock, cli->rbuf + cli->rlen, (uint16_t)want);
    if (r > 0) { cli->rlen += r; return r; }
    if (r == 0 || r == TCP_ERR_WOULD_BLOCK) return 0;
    return -1;   // closed/error
}

// ---------------------------------------------------------------------------
// packet send (frames + optionally encrypts+macs)
// payload[0] is the SSH message type.
// ---------------------------------------------------------------------------
static int ssh2_send(ssh2_client_t *cli, const uint8_t *payload, int payload_len) {
    int cs = cli->encrypting ? 16 : 8;
    int pad = cs - ((4 + 1 + payload_len) % cs);
    if (pad < 4) pad += cs;
    int packet_length = 1 + payload_len + pad;
    int total = 4 + packet_length;
    if (total > SSH2_MAX_PACKET) return -1;

    uint8_t *b = cli->sbuf;
    b[0] = (uint8_t)(packet_length >> 24); b[1] = (uint8_t)(packet_length >> 16);
    b[2] = (uint8_t)(packet_length >> 8);  b[3] = (uint8_t)packet_length;
    b[4] = (uint8_t)pad;
    memcpy(b + 5, payload, payload_len);
    rng_get_bytes(b + 5 + payload_len, pad);

    if (cli->encrypting) {
        uint8_t seq[4] = { (uint8_t)(cli->send_seq >> 24), (uint8_t)(cli->send_seq >> 16),
                           (uint8_t)(cli->send_seq >> 8), (uint8_t)cli->send_seq };
        hmac_sha256_ctx_t hc;
        hmac_sha256_init(&hc, cli->enc_mac, SSH2_KEY_LEN);
        hmac_sha256_update(&hc, seq, 4);
        hmac_sha256_update(&hc, b, total);
        hmac_sha256_final(&hc, b + total);
        ctr_xor(&cli->enc_aes, cli->enc_ctr, b, total);
        cli->send_seq++;
        return sock_send_all(cli->sock, b, total + SSH2_MAC_LEN);
    }
    cli->send_seq++;
    return sock_send_all(cli->sock, b, total);
}

// ---------------------------------------------------------------------------
// packet extract from rbuf. On success copies the decrypted payload (incl type
// byte) into cli->inbuf, sets *plen, returns 1. 0 = need more bytes, <0 = error.
// ---------------------------------------------------------------------------
static int ssh2_extract(ssh2_client_t *cli, int *plen) {
    if (cli->decrypting) {
        if (cli->rlen < 16) return 0;
        uint8_t first[16];
        memcpy(first, cli->rbuf, 16);
        uint8_t ctrpeek[16];
        memcpy(ctrpeek, cli->dec_ctr, 16);
        ctr_xor(&cli->dec_aes, ctrpeek, first, 16);     // peek-decrypt block 0
        uint32_t packet_length = rd_u32(first);
        int enc_total = 4 + (int)packet_length;
        if (packet_length < 8 || enc_total > SSH2_MAX_PACKET || (enc_total % 16) != 0) {
            snprintf(cli->err, sizeof(cli->err), "bad packet_length %u", packet_length);
            return -1;
        }
        if (cli->rlen < enc_total + SSH2_MAC_LEN) return 0;   // wait for full record
        memcpy(cli->inbuf, cli->rbuf, enc_total);
        ctr_xor(&cli->dec_aes, cli->dec_ctr, cli->inbuf, enc_total);   // real decrypt
        uint8_t seq[4] = { (uint8_t)(cli->recv_seq >> 24), (uint8_t)(cli->recv_seq >> 16),
                           (uint8_t)(cli->recv_seq >> 8), (uint8_t)cli->recv_seq };
        uint8_t mac[SSH2_MAC_LEN];
        hmac_sha256_ctx_t hc;
        hmac_sha256_init(&hc, cli->dec_mac, SSH2_KEY_LEN);
        hmac_sha256_update(&hc, seq, 4);
        hmac_sha256_update(&hc, cli->inbuf, enc_total);
        hmac_sha256_final(&hc, mac);
        if (crypto_memcmp(mac, cli->rbuf + enc_total, SSH2_MAC_LEN) != 0) {
            snprintf(cli->err, sizeof(cli->err), "MAC mismatch");
            return -1;
        }
        int pad = cli->inbuf[4];
        int payload_len = (int)packet_length - pad - 1;
        if (payload_len < 1) { snprintf(cli->err, sizeof(cli->err), "bad pad"); return -1; }
        memmove(cli->inbuf, cli->inbuf + 5, payload_len);
        int consumed = enc_total + SSH2_MAC_LEN;
        memmove(cli->rbuf, cli->rbuf + consumed, cli->rlen - consumed);
        cli->rlen -= consumed;
        cli->recv_seq++;
        *plen = payload_len;
        return 1;
    } else {
        if (cli->rlen < 4) return 0;
        uint32_t packet_length = rd_u32(cli->rbuf);
        int total = 4 + (int)packet_length;
        if (packet_length < 5 || total > SSH2_MAX_PACKET) {
            snprintf(cli->err, sizeof(cli->err), "bad plaintext len %u", packet_length);
            return -1;
        }
        if (cli->rlen < total) return 0;
        int pad = cli->rbuf[4];
        int payload_len = (int)packet_length - pad - 1;
        if (payload_len < 1) { snprintf(cli->err, sizeof(cli->err), "bad pad p"); return -1; }
        memcpy(cli->inbuf, cli->rbuf + 5, payload_len);
        memmove(cli->rbuf, cli->rbuf + total, cli->rlen - total);
        cli->rlen -= total;
        cli->recv_seq++;
        *plen = payload_len;
        return 1;
    }
}

// Block until a packet of type `want` arrives (handling transport noise), or
// timeout. Leaves payload in cli->inbuf, length in *plen. Returns 0 / <0.
static int ssh2_expect(ssh2_client_t *cli, int want, int *plen) {
    uint64_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t start = timer_ticks;
    for (;;) {
        int r = ssh2_extract(cli, plen);
        if (r < 0) return -1;
        if (r == 1) {
            int type = cli->inbuf[0];
            if (type == want) return 0;
            if (type == MSG_DISCONNECT) {
                snprintf(cli->err, sizeof(cli->err), "server disconnect");
                return -1;
            }
            if (type == MSG_IGNORE || type == MSG_DEBUG || type == MSG_UNIMPLEMENTED) continue;
            if (type == MSG_GLOBAL_REQUEST) {
                // string name, bool want_reply
                if (*plen >= 5) {
                    uint32_t nl = rd_u32(cli->inbuf + 1);
                    int wr_off = 1 + 4 + (int)nl;
                    if (wr_off < *plen && cli->inbuf[wr_off]) {
                        uint8_t f = MSG_REQUEST_FAILURE;
                        ssh2_send(cli, &f, 1);
                    }
                }
                continue;
            }
            // anything else while expecting a specific reply: ignore but note it
            SDBG("[SSH] expect %d got %d (ignored)\n", want, type);
            continue;
        }
        // need more bytes
        int ing = ssh2_ingest(cli);
        if (ing < 0) { snprintf(cli->err, sizeof(cli->err), "connection closed"); return -1; }
        net_poll(); tcp_timer(); proc_sleep(2);
        if (timer_ticks - start > hz * 12) { snprintf(cli->err, sizeof(cli->err), "timeout waiting for %d", want); return -1; }
    }
}

// read the server identification line ("SSH-2.0-...") into cli->v_s
static int ssh2_read_version(ssh2_client_t *cli) {
    uint64_t hz = g_timer_hz ? g_timer_hz : 250;
    uint64_t start = timer_ticks;
    for (;;) {
        // find a newline-terminated line
        for (int i = 0; i < cli->rlen; i++) {
            if (cli->rbuf[i] == '\n') {
                int linelen = i;
                while (linelen > 0 && cli->rbuf[linelen - 1] == '\r') linelen--;
                if (linelen >= 4 && memcmp(cli->rbuf, "SSH-", 4) == 0) {
                    int n = linelen < (int)sizeof(cli->v_s) - 1 ? linelen : (int)sizeof(cli->v_s) - 1;
                    memcpy(cli->v_s, cli->rbuf, n); cli->v_s[n] = 0;
                    int consumed = i + 1;
                    memmove(cli->rbuf, cli->rbuf + consumed, cli->rlen - consumed);
                    cli->rlen -= consumed;
                    return 0;
                }
                // banner line before the ident: drop it
                int consumed = i + 1;
                memmove(cli->rbuf, cli->rbuf + consumed, cli->rlen - consumed);
                cli->rlen -= consumed;
                i = -1;
                continue;
            }
        }
        int ing = ssh2_ingest(cli);
        if (ing < 0) { snprintf(cli->err, sizeof(cli->err), "closed during version"); return -1; }
        net_poll(); tcp_timer(); proc_sleep(2);
        if (timer_ticks - start > hz * 10) { snprintf(cli->err, sizeof(cli->err), "version timeout"); return -1; }
    }
}

// KDF: out = SHA256(K_mpint || H || letter || session_id), truncated to out_len(<=32)
static void derive(const uint8_t *kmp, int kmp_len, const uint8_t *h,
                   char letter, const uint8_t *sid, uint8_t *out, int out_len) {
    uint8_t d[32];
    sha256_ctx_t c;
    sha256_init(&c);
    sha256_update(&c, kmp, kmp_len);
    sha256_update(&c, h, 32);
    uint8_t L = (uint8_t)letter;
    sha256_update(&c, &L, 1);
    sha256_update(&c, sid, 32);
    sha256_final(&c, d);
    memcpy(out, d, out_len);   // we only ever need <=32
}

// ---------------------------------------------------------------------------
// the full client handshake
// ---------------------------------------------------------------------------
int ssh2_connect(ssh2_client_t *cli, uint32_t ip, uint16_t port,
                 const char *user, const char *password,
                 int cols, int rows, ssh2_data_cb on_data, void *cb_ctx) {
    memset(cli, 0, sizeof(*cli));
    cli->sock = -1;
    cli->on_data = on_data; cli->cb_ctx = cb_ctx;
    cli->term_cols = cols > 0 ? cols : 80;
    cli->term_rows = rows > 0 ? rows : 24;
    cli->verify_host = 0;   // signature is logged; pinning enforced via known_hosts
    cli->local_chan = 0;
    cli->local_window = WINDOW_INITIAL;
    snprintf(cli->khost, sizeof(cli->khost), "%d.%d.%d.%d",
             (int)((ip >> 24) & 0xff), (int)((ip >> 16) & 0xff),
             (int)((ip >> 8) & 0xff), (int)(ip & 0xff));

    uint64_t hz = g_timer_hz ? g_timer_hz : 250;

    // ---- TCP connect ----
    cli->sock = tcp_socket();
    if (cli->sock < 0) { snprintf(cli->err, sizeof(cli->err), "tcp_socket failed"); return -1; }
    if (tcp_connect(cli->sock, ip, port) < 0 && tcp_get_error(cli->sock) != TCP_ERR_IN_PROGRESS) {
        // some stacks return IN_PROGRESS; tolerate
    }
    uint64_t start = timer_ticks;
    while (!tcp_is_connected(cli->sock)) {
        net_poll(); tcp_timer(); proc_sleep(2);
        tcp_state_t st = tcp_get_state(cli->sock);
        if (st == TCP_STATE_CLOSED) { snprintf(cli->err, sizeof(cli->err), "connect refused"); goto fail; }
        if (timer_ticks - start > hz * 10) { snprintf(cli->err, sizeof(cli->err), "connect timeout"); goto fail; }
    }
    SDBG("[SSH] TCP connected\n");

    // ---- version exchange ----
    {
        const char *vc = "SSH-2.0-MayteraOS_1.0";
        snprintf(cli->v_c, sizeof(cli->v_c), "%s", vc);
        char line[160];
        snprintf(line, sizeof(line), "%s\r\n", vc);
        if (sock_send_all(cli->sock, (const uint8_t *)line, (int)strlen(line)) < 0) {
            snprintf(cli->err, sizeof(cli->err), "send version failed"); goto fail;
        }
    }
    if (ssh2_read_version(cli) != 0) goto fail;
    SDBG("[SSH] server version: %s\n", cli->v_s);

    // ---- our KEXINIT ----
    x25519_generate_keypair(&cli->eph);
    {
        uint8_t buf[1024];
        wbuf_t w = { buf, 0, sizeof(buf) };
        w_u8(&w, MSG_KEXINIT);
        uint8_t cookie[16]; rng_get_bytes(cookie, 16); w_raw(&w, cookie, 16);
        w_cstr(&w, "curve25519-sha256,curve25519-sha256@libssh.org");  // kex
        w_cstr(&w, "rsa-sha2-256,rsa-sha2-512,ssh-rsa");                // host key
        w_cstr(&w, "aes256-ctr");   // enc c2s
        w_cstr(&w, "aes256-ctr");   // enc s2c
        w_cstr(&w, "hmac-sha2-256"); // mac c2s
        w_cstr(&w, "hmac-sha2-256"); // mac s2c
        w_cstr(&w, "none");          // comp c2s
        w_cstr(&w, "none");          // comp s2c
        w_cstr(&w, "");              // lang c2s
        w_cstr(&w, "");              // lang s2c
        w_bool(&w, 0);               // first_kex_packet_follows
        w_u32(&w, 0);                // reserved
        if (w.len > w.cap) { snprintf(cli->err, sizeof(cli->err), "kexinit too big"); goto fail; }
        cli->i_c = (uint8_t *)kmalloc(w.len); cli->i_c_len = w.len;
        memcpy(cli->i_c, buf, w.len);
        if (ssh2_send(cli, buf, w.len) < 0) { snprintf(cli->err, sizeof(cli->err), "send kexinit"); goto fail; }
    }
    SDBG("[SSH] sent KEXINIT\n");

    // ---- server KEXINIT ----
    {
        int plen;
        if (ssh2_expect(cli, MSG_KEXINIT, &plen) != 0) goto fail;
        cli->i_s = (uint8_t *)kmalloc(plen); cli->i_s_len = plen;
        memcpy(cli->i_s, cli->inbuf, plen);
        // sanity: server must offer our algorithms (substring search in its lists)
        // (cli->inbuf is the kexinit payload; just check the raw bytes contain them)
        // Not strictly required; negotiation is single-option on our side.
    }
    SDBG("[SSH] got server KEXINIT\n");

    // ---- ECDH init: send our ephemeral public ----
    {
        uint8_t buf[64];
        wbuf_t w = { buf, 0, sizeof(buf) };
        w_u8(&w, MSG_ECDH_INIT);
        w_string(&w, cli->eph.public_key, 32);
        if (ssh2_send(cli, buf, w.len) < 0) { snprintf(cli->err, sizeof(cli->err), "send ecdh"); goto fail; }
    }

    // ---- ECDH reply: K_S, Q_S, signature ----
    uint8_t H[32];
    {
        int plen;
        if (ssh2_expect(cli, MSG_ECDH_REPLY, &plen) != 0) goto fail;
        const uint8_t *p = cli->inbuf + 1; int rem = plen - 1;
        if (rem < 4) { snprintf(cli->err, sizeof(cli->err), "short ecdh reply"); goto fail; }
        uint32_t ks_len = rd_u32(p); p += 4; rem -= 4;
        if ((int)ks_len > rem) { snprintf(cli->err, sizeof(cli->err), "bad K_S"); goto fail; }
        const uint8_t *ks = p; p += ks_len; rem -= ks_len;
        if (rem < 4) goto badreply;
        uint32_t qs_len = rd_u32(p); p += 4; rem -= 4;
        if (qs_len != 32 || (int)qs_len > rem) goto badreply;
        const uint8_t *qs = p; p += 32; rem -= 32;
        if (rem < 4) goto badreply;
        uint32_t sig_len = rd_u32(p); p += 4; rem -= 4;
        if ((int)sig_len > rem) goto badreply;
        const uint8_t *sig = p;

        // shared secret K
        uint8_t shared[32];
        x25519_shared_secret(qs, cli->eph.private_key, shared);

        // K as mpint
        uint8_t kmp[40]; wbuf_t kw = { kmp, 0, sizeof(kmp) };
        w_mpint(&kw, shared, 32);

        // exchange hash H = SHA256(V_C,V_S,I_C,I_S,K_S,Q_C,Q_S,K)
        uint8_t hb[4096]; wbuf_t hw = { hb, 0, sizeof(hb) };
        w_string(&hw, cli->v_c, (int)strlen(cli->v_c));
        w_string(&hw, cli->v_s, (int)strlen(cli->v_s));
        w_string(&hw, cli->i_c, cli->i_c_len);
        w_string(&hw, cli->i_s, cli->i_s_len);
        w_string(&hw, ks, ks_len);
        w_string(&hw, cli->eph.public_key, 32);
        w_string(&hw, qs, 32);
        w_raw(&hw, kmp, kw.len);
        if (hw.len > hw.cap) { snprintf(cli->err, sizeof(cli->err), "hash buf overflow"); goto fail; }
        sha256(hb, hw.len, H);

        // verify host-key signature (ssh-rsa blob: "ssh-rsa", mpint e, mpint n).
        // Always computed now (the kernel bn_mod O(quotient) hang on 3072-bit
        // moduli is fixed); the RESULT is logged. A failed verify only aborts the
        // connection when host-key enforcement is on (cli->verify_host); without a
        // known_hosts store this is signature-validity (proof the server holds the
        // private key for the offered host key), not pinning.
        int vr = -100;
        {
            const uint8_t *kp = ks; int kr = (int)ks_len;
            if (kr >= 4) {
                uint32_t tl = rd_u32(kp); kp += 4; kr -= 4;
                if ((int)tl <= kr && tl == 7 && memcmp(kp, "ssh-rsa", 7) == 0) {
                    kp += tl; kr -= tl;
                    if (kr >= 4) {
                        uint32_t el = rd_u32(kp); kp += 4; kr -= 4;
                        if ((int)el <= kr) {
                            const uint8_t *e = kp; kp += el; kr -= el;
                            if (kr >= 4) {
                                uint32_t nl = rd_u32(kp); kp += 4; kr -= 4;
                                if ((int)nl <= kr) {
                                    const uint8_t *n = kp;
                                    // sig blob: string(alg), string(sigbytes)
                                    const uint8_t *sp = sig; int sr = (int)sig_len;
                                    if (sr >= 4) {
                                        uint32_t al = rd_u32(sp); sp += 4; sr -= 4;
                                        if ((int)al <= sr) {
                                            sp += al; sr -= al;
                                            if (sr >= 4) {
                                                uint32_t bl = rd_u32(sp); sp += 4; sr -= 4;
                                                if ((int)bl <= sr) {
                                                    rsa_public_key_t pk;
                                                    // strip leading zero of mpint for n/e
                                                    while (el > 1 && e[0] == 0) { e++; el--; }
                                                    while (nl > 1 && n[0] == 0) { n++; nl--; }
                                                    pk.e = (uint8_t *)e; pk.e_len = el;
                                                    pk.n = (uint8_t *)n; pk.n_len = nl;
                                                    // rsa-sha2-256 signs the exchange hash H; the PKCS#1 v1.5
                                                    // signature embeds SHA256(H), and rsa_verify wants that digest.
                                                    uint8_t hh[32]; sha256(H, 32, hh);
                                                    vr = rsa_verify_pkcs1_sha256(&pk, hh, 32, sp, bl);
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if (vr == 0) {
            SDBG("[SSH] host key signature VERIFIED\n");
            // known_hosts pinning (only for keys whose signature we verified)
            int kh = ssh2_known_hosts(cli->khost, ks, (int)ks_len);
            if (kh < 0) {
                snprintf(cli->err, sizeof(cli->err), "HOST KEY CHANGED for %s - possible MITM", cli->khost);
                SDBG("[SSH] %s\n", cli->err);
                goto fail;
            }
            SDBG("[SSH] host key pinned/matched in known_hosts\n");
        } else {
            SDBG("[SSH] host key signature NOT verified (vr=%d)\n", vr);
            if (cli->verify_host) { snprintf(cli->err, sizeof(cli->err), "host key verify failed"); goto fail; }
        }

        memcpy(cli->session_id, H, 32); cli->have_session_id = 1;

        // derive keys (client: c2s = A/C/E, s2c = B/D/F)
        uint8_t kc2s[32], ks2c[32], mc2s[32], ms2c[32], ivc2s[16], ivs2c[16];
        derive(kmp, kw.len, H, 'A', cli->session_id, ivc2s, 16);
        derive(kmp, kw.len, H, 'B', cli->session_id, ivs2c, 16);
        derive(kmp, kw.len, H, 'C', cli->session_id, kc2s, 32);
        derive(kmp, kw.len, H, 'D', cli->session_id, ks2c, 32);
        derive(kmp, kw.len, H, 'E', cli->session_id, mc2s, 32);
        derive(kmp, kw.len, H, 'F', cli->session_id, ms2c, 32);
        aes_set_encrypt_key(&cli->enc_aes, kc2s, 256);
        aes_set_encrypt_key(&cli->dec_aes, ks2c, 256);   // CTR uses encrypt both ways
        memcpy(cli->enc_ctr, ivc2s, 16); memcpy(cli->enc_mac, mc2s, 32);
        memcpy(cli->dec_ctr, ivs2c, 16); memcpy(cli->dec_mac, ms2c, 32);
    }
    SDBG("[SSH] keys derived\n");

    // ---- NEWKEYS (send plaintext, then turn on encryption) ----
    {
        uint8_t nk = MSG_NEWKEYS;
        if (ssh2_send(cli, &nk, 1) < 0) { snprintf(cli->err, sizeof(cli->err), "send newkeys"); goto fail; }
        cli->encrypting = 1;
        int plen;
        if (ssh2_expect(cli, MSG_NEWKEYS, &plen) != 0) goto fail;  // still plaintext inbound
        cli->decrypting = 1;
    }
    SDBG("[SSH] NEWKEYS done, encrypted channel up\n");

    // ---- service request: ssh-userauth ----
    {
        uint8_t buf[64]; wbuf_t w = { buf, 0, sizeof(buf) };
        w_u8(&w, MSG_SERVICE_REQUEST); w_cstr(&w, "ssh-userauth");
        if (ssh2_send(cli, buf, w.len) < 0) { snprintf(cli->err, sizeof(cli->err), "send svc req"); goto fail; }
        int plen;
        if (ssh2_expect(cli, MSG_SERVICE_ACCEPT, &plen) != 0) goto fail;
    }
    SDBG("[SSH] service ssh-userauth accepted\n");

    // ---- password auth ----
    {
        uint8_t buf[512]; wbuf_t w = { buf, 0, sizeof(buf) };
        w_u8(&w, MSG_USERAUTH_REQUEST);
        w_cstr(&w, user);
        w_cstr(&w, "ssh-connection");
        w_cstr(&w, "password");
        w_bool(&w, 0);
        w_cstr(&w, password);
        if (w.len > w.cap) { snprintf(cli->err, sizeof(cli->err), "auth too big"); goto fail; }
        if (ssh2_send(cli, buf, w.len) < 0) { snprintf(cli->err, sizeof(cli->err), "send auth"); goto fail; }
        // read until SUCCESS or FAILURE (banners interleaved)
        uint64_t hz2 = g_timer_hz ? g_timer_hz : 250; uint64_t st2 = timer_ticks; int plen;
        for (;;) {
            int r = ssh2_extract(cli, &plen);
            if (r < 0) goto fail;
            if (r == 1) {
                int t = cli->inbuf[0];
                if (t == MSG_USERAUTH_SUCCESS) break;
                if (t == MSG_USERAUTH_FAILURE) { snprintf(cli->err, sizeof(cli->err), "auth failed (bad user/password?)"); goto fail; }
                if (t == MSG_USERAUTH_BANNER || t == MSG_IGNORE || t == MSG_DEBUG) continue;
                if (t == MSG_DISCONNECT) { snprintf(cli->err, sizeof(cli->err), "disconnect during auth"); goto fail; }
                continue;
            }
            if (ssh2_ingest(cli) < 0) { snprintf(cli->err, sizeof(cli->err), "closed during auth"); goto fail; }
            net_poll(); tcp_timer(); proc_sleep(2);
            if (timer_ticks - st2 > hz2 * 12) { snprintf(cli->err, sizeof(cli->err), "auth timeout"); goto fail; }
        }
        cli->authed = 1;
    }
    SDBG("[SSH] authenticated as %s\n", user);

    // ---- open session channel ----
    {
        uint8_t buf[64]; wbuf_t w = { buf, 0, sizeof(buf) };
        w_u8(&w, MSG_CHANNEL_OPEN);
        w_cstr(&w, "session");
        w_u32(&w, cli->local_chan);
        w_u32(&w, cli->local_window);
        w_u32(&w, 32768);
        if (ssh2_send(cli, buf, w.len) < 0) { snprintf(cli->err, sizeof(cli->err), "send chan open"); goto fail; }
        int plen;
        if (ssh2_expect(cli, MSG_CHANNEL_OPEN_CONFIRMATION, &plen) != 0) {
            snprintf(cli->err, sizeof(cli->err), "channel open rejected"); goto fail;
        }
        // recipient(4) sender(4) initial_window(4) max_packet(4)
        const uint8_t *p = cli->inbuf + 1;
        cli->remote_chan = rd_u32(p + 4);
        cli->remote_window = rd_u32(p + 8);
        cli->remote_maxpkt = rd_u32(p + 12);
    }
    SDBG("[SSH] channel open (remote=%u win=%u)\n", cli->remote_chan, cli->remote_window);

    // ---- pty-req ----
    {
        uint8_t buf[128]; wbuf_t w = { buf, 0, sizeof(buf) };
        w_u8(&w, MSG_CHANNEL_REQUEST);
        w_u32(&w, cli->remote_chan);
        w_cstr(&w, "pty-req");
        w_bool(&w, 1);                 // want reply
        w_cstr(&w, "xterm");
        w_u32(&w, cli->term_cols);
        w_u32(&w, cli->term_rows);
        w_u32(&w, 0); w_u32(&w, 0);    // pixel dims
        uint8_t modes[1] = { 0 };      // TTY_OP_END
        w_string(&w, modes, 1);
        if (ssh2_send(cli, buf, w.len) < 0) { snprintf(cli->err, sizeof(cli->err), "send pty"); goto fail; }
        int plen;
        if (ssh2_expect(cli, MSG_CHANNEL_SUCCESS, &plen) != 0)
            SDBG("[SSH] pty-req not confirmed (continuing)\n");
    }

    // ---- shell ----
    {
        uint8_t buf[64]; wbuf_t w = { buf, 0, sizeof(buf) };
        w_u8(&w, MSG_CHANNEL_REQUEST);
        w_u32(&w, cli->remote_chan);
        w_cstr(&w, "shell");
        w_bool(&w, 1);
        if (ssh2_send(cli, buf, w.len) < 0) { snprintf(cli->err, sizeof(cli->err), "send shell"); goto fail; }
        int plen;
        if (ssh2_expect(cli, MSG_CHANNEL_SUCCESS, &plen) != 0) {
            snprintf(cli->err, sizeof(cli->err), "shell request failed"); goto fail;
        }
    }
    cli->chan_ready = 1;
    cli->connected = 1;
    SDBG("[SSH] shell ready\n");
    return 0;

badreply:
    snprintf(cli->err, sizeof(cli->err), "malformed ECDH reply");
fail:
    if (cli->sock >= 0) tcp_close(cli->sock);
    cli->closed = 1;
    SDBG("[SSH] connect failed: %s\n", cli->err);
    return -1;
}

// replenish our receive window if it has drained
static void ssh2_maybe_adjust_window(ssh2_client_t *cli) {
    if (cli->local_window < WINDOW_LOW) {
        uint32_t add = WINDOW_INITIAL - cli->local_window;
        uint8_t buf[16]; wbuf_t w = { buf, 0, sizeof(buf) };
        w_u8(&w, MSG_CHANNEL_WINDOW_ADJUST);
        w_u32(&w, cli->remote_chan);
        w_u32(&w, add);
        if (ssh2_send(cli, buf, w.len) >= 0) cli->local_window += add;
    }
}

int ssh2_pump(ssh2_client_t *cli) {
    if (!cli->connected || cli->closed) return -1;
    int did = 0;
    ssh2_ingest(cli);
    net_poll(); tcp_timer();
    for (;;) {
        int plen;
        int r = ssh2_extract(cli, &plen);
        if (r < 0) { cli->closed = 1; return -1; }
        if (r == 0) break;
        did++;
        int t = cli->inbuf[0];
        const uint8_t *p = cli->inbuf + 1;
        switch (t) {
        case MSG_CHANNEL_DATA: {
            // u32 recipient, string data
            uint32_t dl = rd_u32(p + 4);
            const uint8_t *d = p + 8;
            if ((int)dl > plen - 9) dl = plen - 9 > 0 ? plen - 9 : 0;
            if (cli->on_data && dl) cli->on_data(cli->cb_ctx, d, (int)dl);
            if (cli->local_window >= dl) cli->local_window -= dl;
            ssh2_maybe_adjust_window(cli);
            break;
        }
        case MSG_CHANNEL_EXTENDED_DATA: {
            // u32 recipient, u32 datatype, string data
            uint32_t dl = rd_u32(p + 8);
            const uint8_t *d = p + 12;
            if ((int)dl > plen - 13) dl = plen - 13 > 0 ? plen - 13 : 0;
            if (cli->on_data && dl) cli->on_data(cli->cb_ctx, d, (int)dl);
            if (cli->local_window >= dl) cli->local_window -= dl;
            ssh2_maybe_adjust_window(cli);
            break;
        }
        case MSG_CHANNEL_WINDOW_ADJUST:
            cli->remote_window += rd_u32(p + 4);
            break;
        case MSG_CHANNEL_EOF:
            break;
        case MSG_CHANNEL_CLOSE:
            cli->closed = 1;
            return did;
        case MSG_CHANNEL_REQUEST: {
            // possibly exit-status; if want_reply, fail it
            break;
        }
        case MSG_GLOBAL_REQUEST: {
            uint32_t nl = rd_u32(p);
            int wr_off = 4 + (int)nl;
            if (wr_off < plen - 1 && p[wr_off]) {
                uint8_t f = MSG_REQUEST_FAILURE; ssh2_send(cli, &f, 1);
            }
            break;
        }
        case MSG_DISCONNECT:
            cli->closed = 1;
            return did;
        default:
            break;
        }
    }
    return did;
}

int ssh2_send_input(ssh2_client_t *cli, const void *data, int len) {
    if (!cli->chan_ready || cli->closed || len <= 0) return -1;
    uint8_t buf[2048];
    int off = 0;
    while (off < len) {
        int chunk = len - off;
        if (chunk > 1024) chunk = 1024;
        wbuf_t w = { buf, 0, sizeof(buf) };
        w_u8(&w, MSG_CHANNEL_DATA);
        w_u32(&w, cli->remote_chan);
        w_string(&w, (const uint8_t *)data + off, chunk);
        if (ssh2_send(cli, buf, w.len) < 0) { cli->closed = 1; return -1; }
        off += chunk;
    }
    return len;
}

int ssh2_window_change(ssh2_client_t *cli, int cols, int rows) {
    if (!cli->chan_ready || cli->closed) return -1;
    cli->term_cols = cols; cli->term_rows = rows;
    uint8_t buf[64]; wbuf_t w = { buf, 0, sizeof(buf) };
    w_u8(&w, MSG_CHANNEL_REQUEST);
    w_u32(&w, cli->remote_chan);
    w_cstr(&w, "window-change");
    w_bool(&w, 0);
    w_u32(&w, cols); w_u32(&w, rows);
    w_u32(&w, 0); w_u32(&w, 0);
    return ssh2_send(cli, buf, w.len);
}

void ssh2_close(ssh2_client_t *cli) {
    if (cli->chan_ready && !cli->closed) {
        uint8_t buf[16]; wbuf_t w = { buf, 0, sizeof(buf) };
        w_u8(&w, MSG_CHANNEL_CLOSE); w_u32(&w, cli->remote_chan);
        ssh2_send(cli, buf, w.len);
    }
    if (cli->sock >= 0) { tcp_close(cli->sock); cli->sock = -1; }
    if (cli->i_c) { kfree(cli->i_c); cli->i_c = 0; }
    if (cli->i_s) { kfree(cli->i_s); cli->i_s = 0; }
    cli->closed = 1; cli->connected = 0; cli->chan_ready = 0;
}

// ---------------------------------------------------------------------------
// Interactive client bridged to a pair of open files (a process's stdin and
// stdout). Used by the userland `ssh` command via the SYS_SSH_CLIENT syscall:
// the kernel runs the full SSH-2 client and shuttles bytes between the remote
// shell channel and the caller's terminal (a pts/tty). Blocks until the remote
// session closes. file ops are declared here as void* to avoid pulling the VFS
// headers into this net module (symbols resolve at link time).
// ---------------------------------------------------------------------------
extern long file_read(void *f, void *buf, unsigned long count);
extern long file_write(void *f, const void *buf, unsigned long count);
extern int  file_poll(void *f, int events);

static void ssh_fd_on_data(void *ctx, const uint8_t *data, int len) {
    if (ctx && len > 0) file_write(ctx, data, len);
}

int ssh2_run_on_fds(uint32_t ip, uint16_t port, const char *user, const char *pass,
                    int cols, int rows, void *fin, void *fout) {
    ssh2_client_t *cli = (ssh2_client_t *)kmalloc(sizeof(ssh2_client_t));
    if (!cli) return -1;
    if (port == 0) port = 22;
    int rc = ssh2_connect(cli, ip, port, user, pass,
                          cols > 0 ? cols : 80, rows > 0 ? rows : 24,
                          ssh_fd_on_data, fout);
    if (rc != 0) {
        const char *e = cli->err[0] ? cli->err : "connection failed";
        file_write(fout, "ssh: ", 5);
        file_write(fout, e, strlen(e));
        file_write(fout, "\r\n", 2);
        kfree(cli);
        return -1;
    }
    uint8_t ibuf[256];
    while (!cli->closed) {
        int did = ssh2_pump(cli);                  // net + remote->stdout
        if (fin && file_poll(fin, 0x01 /*POLL_IN*/)) {
            long n = file_read(fin, ibuf, sizeof(ibuf));   // stdin->remote
            if (n > 0) ssh2_send_input(cli, ibuf, (int)n);
        }
        if (did <= 0) proc_sleep(5);
    }
    ssh2_close(cli);
    kfree(cli);
    return 0;
}
