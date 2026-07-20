// ssh2.h - SSH-2 client for MayteraOS (RFC 4251/4253/4252/4254)
//
// A real, interoperable SSH-2 client built on the kernel's live crypto:
//   KEX     : curve25519-sha256            (net/tls x25519_*)
//   hostkey : rsa-sha2-256 / ssh-rsa       (crypto/rsa rsa_verify_pkcs1_sha256)
//   cipher  : aes256-ctr                   (crypto aes_encrypt_block, CTR mode)
//   MAC     : hmac-sha2-256                (crypto hmac_sha256)
//
// The legacy net/ssh/{sshd,ssh_transport,...}.c files are dead scaffolding (fake
// crypto) and are NOT compiled; this is a from-scratch, correct implementation.
#ifndef SSH2_H
#define SSH2_H

#include "../../types.h"
#include "../../crypto/crypto.h"
#include "../tls/tls13.h"          // x25519_keypair_t, x25519_*

#define SSH2_BLOCK        16          // aes block size
#define SSH2_MAC_LEN      32          // hmac-sha2-256
#define SSH2_KEY_LEN      32          // aes256 key
#define SSH2_IV_LEN       16          // ctr iv / counter
#define SSH2_RECV_CAP     65536
#define SSH2_MAX_PACKET   65536

// Output callback: receives decrypted shell/channel bytes (stdout+stderr).
typedef void (*ssh2_data_cb)(void *ctx, const uint8_t *data, int len);

typedef struct ssh2_client {
    int sock;
    int connected;          // TCP + transport up
    int authed;             // userauth succeeded
    int chan_ready;         // session channel open + shell started
    int closed;             // peer closed / error
    int verify_host;        // 1 = enforce host-key signature, 0 = log only
    char err[128];

    // version strings (no trailing CR/LF), used in the exchange hash
    char v_c[128];
    char v_s[256];

    // our ephemeral X25519 keypair
    x25519_keypair_t eph;

    // saved KEXINIT payloads (I_C, I_S) for the exchange hash
    uint8_t *i_c; int i_c_len;
    uint8_t *i_s; int i_s_len;

    // exchange hash / session id
    uint8_t session_id[32];
    int have_session_id;

    // directional keys + CTR state (c2s = our outgoing, s2c = incoming)
    aes_ctx_t enc_aes;  uint8_t enc_ctr[SSH2_IV_LEN]; uint8_t enc_mac[SSH2_KEY_LEN];
    aes_ctx_t dec_aes;  uint8_t dec_ctr[SSH2_IV_LEN]; uint8_t dec_mac[SSH2_KEY_LEN];
    uint64_t send_seq, recv_seq;
    int encrypting;         // outbound encrypted (after we send NEWKEYS)
    int decrypting;         // inbound encrypted (after server NEWKEYS)

    // raw inbound byte buffer (ciphertext accumulates here)
    uint8_t rbuf[SSH2_RECV_CAP];
    int rlen;

    // scratch: outbound framing buffer, and decrypted-payload buffer
    uint8_t sbuf[SSH2_MAX_PACKET + 64];
    uint8_t inbuf[SSH2_MAX_PACKET];

    // channel state
    uint32_t local_chan, remote_chan;
    uint32_t local_window, remote_window, remote_maxpkt;

    // terminal geometry (for pty-req)
    int term_cols, term_rows;

    char khost[24];        // "a.b.c.d" host string for known_hosts pinning

    // output sink
    ssh2_data_cb on_data;
    void *cb_ctx;
} ssh2_client_t;

// Connect + full handshake + password auth + open an interactive shell channel.
// Returns 0 on success; on failure returns <0 and fills cli->err.
int ssh2_connect(ssh2_client_t *cli, uint32_t ip, uint16_t port,
                 const char *user, const char *password,
                 int cols, int rows, ssh2_data_cb on_data, void *cb_ctx);

// Non-blocking: pump inbound packets, dispatch channel data to on_data.
// Returns >0 if work was done, 0 if idle, <0 on close/error.
int ssh2_pump(ssh2_client_t *cli);

// Send keystrokes / input bytes to the remote shell.
int ssh2_send_input(ssh2_client_t *cli, const void *data, int len);

// Send a window-size change (SIGWINCH) to the remote pty.
int ssh2_window_change(ssh2_client_t *cli, int cols, int rows);

// Route stage/debug logs to a sink (e.g. an RC session) instead of serial.
void ssh2_set_log(void (*fn)(void *, const char *), void *ctx);

// Close the channel + connection.
void ssh2_close(ssh2_client_t *cli);

// Run a full interactive client bridged to a process's stdin/stdout (open
// files, passed as void* to avoid VFS headers here). Blocks until the remote
// session closes. Backs the userland `ssh` command via SYS_SSH_CLIENT.
int ssh2_run_on_fds(uint32_t ip, uint16_t port, const char *user, const char *pass,
                    int cols, int rows, void *fin, void *fout);

#endif // SSH2_H
