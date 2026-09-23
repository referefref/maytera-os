// pair.c - Bluetooth pairing (#372, PROTOCOL agent).
//
// CLASSIC Secure Simple Pairing (SSP), Just-Works association, driven by HCI
// events fanned out from hci.c:
//   IO capability request   -> reply NoInputNoOutput, general bonding
//   User confirmation req    -> consult UI policy (auto-accept if none), confirm
//   Link key request         -> answer from the in-memory bond store (or neg)
//   Link key notification    -> store the link key (device is now bonded)
//   PIN code request         -> legacy fallback, reply "0000"
//   Simple pairing complete  -> mark bonded / failed
//
// BLE SMP (LE fixed CID 0x0006), central/initiator role:
//   - LEGACY Just Works (c1/s1, TK=0) for older peripherals.
//   - LE SECURE CONNECTIONS (LESC) Just Works / Numeric-Comparison for modern
//     BT5.0 HID (the K68 keyboard refuses legacy). P-256 ECDH DHKey (reuses
//     crypto/ecdsa.c), and the f4/f5/f6/g2 SMP functions on AES-CMAC
//     (crypto/aes.c). See #372 (btlesc).
#include "pair.h"
#include "hci.h"
#include "hci_defs.h"
#include "l2cap.h"
#include "../serial.h"
#include "../string.h"
#include "../crypto/crypto.h"
#include "../crypto/ecdsa.h"
#include "../fs/bootlog.h"

extern int bt_pair_confirm_policy(const bt_addr_t *addr, uint32_t passkey);

// ===========================================================================
// BLE Security Manager (SMP) - LE pairing, Just Works, central role.
// Runs over L2CAP fixed CID 0x0006. Legacy crypto: security functions e/c1/s1
// from Bluetooth Core Vol 3 Part H, built on the kernel AES-128 (crypto/aes.c).
// LESC crypto: f4/f5/f6/g2 on AES-CMAC + P-256 ECDH (crypto/ecdsa.c).
// ===========================================================================
#define SMP_PAIRING_REQUEST     0x01
#define SMP_PAIRING_RESPONSE    0x02
#define SMP_PAIRING_CONFIRM     0x03
#define SMP_PAIRING_RANDOM      0x04
#define SMP_PAIRING_FAILED      0x05
#define SMP_ENCRYPTION_INFO     0x06   // LTK
#define SMP_MASTER_IDENT        0x07   // EDIV + Rand
#define SMP_IDENTITY_INFO       0x08   // IRK
#define SMP_IDENTITY_ADDR_INFO  0x09
#define SMP_SIGNING_INFO        0x0A
#define SMP_SECURITY_REQUEST    0x0B
#define SMP_PAIRING_PUBLIC_KEY  0x0C   // LESC: 64-byte X||Y (little-endian)
#define SMP_PAIRING_DHKEY_CHECK 0x0D   // LESC: 16-byte Ea/Eb
#define SMP_KEYPRESS_NOTIF      0x0E

// AuthReq bits.
#define SMP_AUTH_BONDING   0x01
#define SMP_AUTH_MITM      0x04
#define SMP_AUTH_SC        0x08   // Secure Connections

// SMP Pairing Failed reason codes.
#define SMP_ERR_CONFIRM_FAILED   0x04
#define SMP_ERR_INVALID_PARAMS   0x0A
#define SMP_ERR_DHKEY_CHECK      0x0B
#define SMP_ERR_NUMERIC_FAILED   0x0C

// e(): the AES-128 "security function e". BT arrays are little-endian (wire
// order); AES treats byte 0 as most-significant, so swap key/in/out around it.
static void smp_e(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t k[16], d[16], o[16];
    for (int i = 0; i < 16; i++) { k[i] = key[15 - i]; d[i] = in[15 - i]; }
    aes_ctx_t c;
    aes_set_encrypt_key(&c, k, 128);
    aes_encrypt_block(&c, d, o);
    for (int i = 0; i < 16; i++) out[i] = o[15 - i];
}

static void xor128(const uint8_t *a, const uint8_t *b, uint8_t *o) {
    for (int i = 0; i < 16; i++) o[i] = (uint8_t)(a[i] ^ b[i]);
}

// Reverse n bytes (little-endian wire order <-> big-endian crypto order). src
// and dst must not overlap.
static void smp_rev(uint8_t *dst, const uint8_t *src, int n) {
    for (int i = 0; i < n; i++) dst[i] = src[n - 1 - i];
}

// c1 confirm-value function. preq/pres are the 7-byte Pairing Request/Response
// PDUs; ia/ra are the initiator/responder addresses, iat/rat their types.
static void smp_c1(const uint8_t k[16], const uint8_t r[16],
                   const uint8_t preq[7], const uint8_t pres[7],
                   uint8_t iat, const uint8_t ia[6],
                   uint8_t rat, const uint8_t ra[6], uint8_t out[16]) {
    uint8_t p1[16], p2[16], t[16];
    p1[0] = iat; p1[1] = rat;
    memcpy(p1 + 2, preq, 7);
    memcpy(p1 + 9, pres, 7);
    memcpy(p2, ra, 6);
    memcpy(p2 + 6, ia, 6);
    memset(p2 + 12, 0, 4);
    xor128(r, p1, t);
    smp_e(k, t, t);
    xor128(t, p2, t);
    smp_e(k, t, out);
}

// s1 STK generation. r' = r1'(MSB half) || r2'(LSB half); STK = e(TK, r').
static void smp_s1(const uint8_t k[16], const uint8_t r1[16],
                   const uint8_t r2[16], uint8_t out[16]) {
    uint8_t r[16];
    memcpy(r, r2, 8);       // low half of r' = r2'
    memcpy(r + 8, r1, 8);   // high half of r' = r1'
    smp_e(k, r, out);
}

// ---------------------------------------------------------------------------
// LE Secure Connections crypto (Bluetooth Core Vol 3 Part H 2.2.5-2.2.9).
//
// All wrappers take LITTLE-ENDIAN inputs (SMP wire order, i.e. how the fields
// are stored/transmitted) and produce little-endian outputs. Internally they
// swap to the MSB-first order that AES-CMAC (RFC 4493) expects, exactly like
// smp_e() does around raw AES. iocap is passed in message order already
// {AuthReq, OOB, IOcap} because that is how it is naturally read out of a
// Pairing Request/Response PDU (pdu[3], pdu[2], pdu[1]).
// ---------------------------------------------------------------------------

// f4(U, V, X, Z) = AES-CMAC_X(U || V || Z). U,V are 256-bit public-key X
// coords; X is the 128-bit key (a nonce); Z is 8 bits. Used for the confirm
// value Cb = f4(PKbx, PKax, Nb, 0).
static void smp_f4(const uint8_t U[32], const uint8_t V[32], const uint8_t X[16],
                   uint8_t Z, uint8_t out[16]) {
    uint8_t m[65], k[16], mac[16];
    smp_rev(m, U, 32);
    smp_rev(m + 32, V, 32);
    m[64] = Z;
    smp_rev(k, X, 16);
    aes_cmac(k, m, 65, mac);
    smp_rev(out, mac, 16);
}

// f5(W, N1, N2, A1, A2) -> MacKey || LTK.
//   T      = AES-CMAC_SALT(W)
//   MacKey = AES-CMAC_T(0x00 || "btle" || N1 || N2 || A1 || A2 || 0x0100)
//   LTK    = AES-CMAC_T(0x01 || "btle" || N1 || N2 || A1 || A2 || 0x0100)
// W = DHKey (256-bit). A1/A2 = addr_type(1) || addr(6), initiator then resp.
static void smp_f5(const uint8_t W[32], const uint8_t N1[16], const uint8_t N2[16],
                   uint8_t at1, const uint8_t A1[6],
                   uint8_t at2, const uint8_t A2[6],
                   uint8_t mackey[16], uint8_t ltk[16]) {
    static const uint8_t salt[16] = {
        0x6c,0x88,0x83,0x91,0xaa,0xf5,0xa5,0x38,
        0x60,0x37,0x0b,0xdb,0x5a,0x60,0x83,0xbe
    };
    uint8_t ws[32], t[16], m[53], macbe[16];
    smp_rev(ws, W, 32);
    aes_cmac(salt, ws, 32, t);   // T = AES-CMAC_SALT(W)

    m[1] = 0x62; m[2] = 0x74; m[3] = 0x6c; m[4] = 0x65;   // keyID = "btle"
    smp_rev(m + 5,  N1, 16);
    smp_rev(m + 21, N2, 16);
    m[37] = at1; smp_rev(m + 38, A1, 6);
    m[44] = at2; smp_rev(m + 45, A2, 6);
    m[51] = 0x01; m[52] = 0x00;                           // Length = 256

    m[0] = 0x00; aes_cmac(t, m, 53, macbe); smp_rev(mackey, macbe, 16);
    m[0] = 0x01; aes_cmac(t, m, 53, macbe); smp_rev(ltk,    macbe, 16);
}

// f6(W, N1, N2, R, IOcap, A1, A2) = AES-CMAC_W(N1||N2||R||IOcap||A1||A2).
// W = MacKey (128-bit). R = 0 for Just Works / Numeric Comparison.
static void smp_f6(const uint8_t W[16], const uint8_t N1[16], const uint8_t N2[16],
                   const uint8_t R[16], const uint8_t iocap[3],
                   uint8_t at1, const uint8_t A1[6],
                   uint8_t at2, const uint8_t A2[6], uint8_t out[16]) {
    uint8_t m[65], k[16], mac[16];
    smp_rev(m,      N1, 16);
    smp_rev(m + 16, N2, 16);
    smp_rev(m + 32, R,  16);
    m[48] = iocap[0]; m[49] = iocap[1]; m[50] = iocap[2];   // already msg order
    m[51] = at1; smp_rev(m + 52, A1, 6);
    m[58] = at2; smp_rev(m + 59, A2, 6);
    smp_rev(k, W, 16);
    aes_cmac(k, m, 65, mac);
    smp_rev(out, mac, 16);
}

// g2(U, V, X, Y) = AES-CMAC_X(U || V || Y) taken mod 2^32 (the low 32 bits).
// Returns that 32-bit value; the 6-digit compare number is (value % 1000000).
static uint32_t smp_g2(const uint8_t U[32], const uint8_t V[32],
                       const uint8_t X[16], const uint8_t Y[16]) {
    uint8_t m[80], k[16], mac[16];
    smp_rev(m,      U, 32);
    smp_rev(m + 32, V, 32);
    smp_rev(m + 64, Y, 16);
    smp_rev(k, X, 16);
    aes_cmac(k, m, 80, mac);
    // mod 2^32 = least significant 32 bits = last four MSB-first bytes.
    return ((uint32_t)mac[12] << 24) | ((uint32_t)mac[13] << 16) |
           ((uint32_t)mac[14] << 8)  |  (uint32_t)mac[15];
}

// ---------------------------------------------------------------------------
// In-memory bond store (link keys). Not yet persisted to disk (phase 2).
// ---------------------------------------------------------------------------
#define PAIR_MAX_BONDS 8
typedef struct { bt_addr_t addr; uint8_t key[16]; uint8_t valid; } bond_t;
static bond_t g_bonds[PAIR_MAX_BONDS];

#define PAIR_MAX_STATE 4
typedef struct { bt_addr_t addr; bt_pair_state_t state; uint8_t used; } pstate_t;
static pstate_t g_pstate[PAIR_MAX_STATE];

static void set_state(const bt_addr_t *a, bt_pair_state_t s) {
    for (int i = 0; i < PAIR_MAX_STATE; i++)
        if (g_pstate[i].used && bt_addr_eq(&g_pstate[i].addr, a)) { g_pstate[i].state = s; return; }
    for (int i = 0; i < PAIR_MAX_STATE; i++)
        if (!g_pstate[i].used) { g_pstate[i].used = 1; g_pstate[i].addr = *a; g_pstate[i].state = s; return; }
}

static int bt_pair_store_link_key(const bt_addr_t *addr, const uint8_t key[16]) {
    for (int i = 0; i < PAIR_MAX_BONDS; i++)
        if (g_bonds[i].valid && bt_addr_eq(&g_bonds[i].addr, addr)) {
            memcpy(g_bonds[i].key, key, 16); return BT_OK;
        }
    for (int i = 0; i < PAIR_MAX_BONDS; i++)
        if (!g_bonds[i].valid) {
            g_bonds[i].addr = *addr; memcpy(g_bonds[i].key, key, 16); g_bonds[i].valid = 1;
            return BT_OK;
        }
    return BT_ERR_NOMEM;
}

static int bt_pair_find_link_key(const bt_addr_t *addr, uint8_t key_out[16]) {
    for (int i = 0; i < PAIR_MAX_BONDS; i++)
        if (g_bonds[i].valid && bt_addr_eq(&g_bonds[i].addr, addr)) {
            if (key_out) memcpy(key_out, g_bonds[i].key, 16);
            return BT_OK;
        }
    return BT_ERR_NODEV;
}

int pair_is_bonded(const bt_addr_t *addr) {
    return bt_pair_find_link_key(addr, NULL) == BT_OK;
}

int pair_forget(const bt_addr_t *addr) {
    for (int i = 0; i < PAIR_MAX_BONDS; i++)
        if (g_bonds[i].valid && bt_addr_eq(&g_bonds[i].addr, addr)) {
            g_bonds[i].valid = 0; return BT_OK;
        }
    return BT_ERR_NODEV;
}

bt_pair_state_t pair_state(const bt_addr_t *addr) {
    for (int i = 0; i < PAIR_MAX_STATE; i++)
        if (g_pstate[i].used && bt_addr_eq(&g_pstate[i].addr, addr)) return g_pstate[i].state;
    return pair_is_bonded(addr) ? BT_PAIR_BONDED : BT_PAIR_IDLE;
}

// ---------------------------------------------------------------------------
// LE bond store (LTK + EDIV + Rand for reconnection). In-memory (phase 1).
// ---------------------------------------------------------------------------
#define LE_MAX_BONDS 8
typedef struct {
    bt_addr_t addr; uint8_t addr_type;
    uint8_t ltk[16]; uint8_t ediv[2]; uint8_t rand[8]; uint8_t valid;
} le_bond_t;
static le_bond_t g_le_bonds[LE_MAX_BONDS];

static le_bond_t *le_bond_get(const bt_addr_t *a) {
    for (int i = 0; i < LE_MAX_BONDS; i++)
        if (g_le_bonds[i].valid && bt_addr_eq(&g_le_bonds[i].addr, a)) return &g_le_bonds[i];
    for (int i = 0; i < LE_MAX_BONDS; i++)
        if (!g_le_bonds[i].valid) { memset(&g_le_bonds[i], 0, sizeof(g_le_bonds[i])); return &g_le_bonds[i]; }
    return NULL;
}

// ---------------------------------------------------------------------------
// SMP session state (central / initiator)
// ---------------------------------------------------------------------------
typedef enum {
    SMP_IDLE = 0, SMP_W_PAIR_RSP,
    // legacy
    SMP_W_SCONFIRM, SMP_W_SRAND,
    // LESC
    SMP_SC_W_PUBKEY, SMP_SC_W_CONFIRM, SMP_SC_W_RANDOM, SMP_SC_W_DHK_CHECK,
    SMP_W_ENC, SMP_DONE, SMP_FAILED,
} smp_state_t;

typedef struct {
    int          active;
    hci_handle_t handle;
    bt_addr_t    peer;
    uint8_t      peer_type;
    smp_state_t  state;
    uint8_t      preq[7], pres[7];
    // legacy
    uint8_t      tk[16], mrand[16], srand[16], mconfirm[16], sconfirm[16], stk[16];
    // LESC
    uint8_t      sc;              // 1 = LE Secure Connections negotiated
    uint8_t      priv[32];        // our P-256 private scalar (big-endian)
    uint8_t      pka[64];         // our public key on the wire: X_le || Y_le
    uint8_t      pkb[64];         // peer public key on the wire: X_le || Y_le
    uint8_t      dhkey[32];       // ECDH shared X-coord (little-endian)
    uint8_t      na[16], nb[16];  // nonces (little-endian, wire order)
    uint8_t      mackey[16], ltk[16];
    uint8_t      peer_confirm[16];
} smp_sess_t;
static smp_sess_t g_smp[2];

static smp_sess_t *smp_by_handle(hci_handle_t h) {
    for (int i = 0; i < 2; i++) if (g_smp[i].active && g_smp[i].handle == h) return &g_smp[i];
    return NULL;
}
static smp_sess_t *smp_alloc(hci_handle_t h) {
    smp_sess_t *s = smp_by_handle(h);
    if (s) return s;
    for (int i = 0; i < 2; i++) if (!g_smp[i].active) {
        memset(&g_smp[i], 0, sizeof(g_smp[i])); g_smp[i].active = 1; g_smp[i].handle = h; return &g_smp[i];
    }
    return NULL;
}

static void smp_send(hci_handle_t h, const uint8_t *pdu, uint16_t len) {
    l2cap_send_fixed(h, L2CAP_CID_LE_SMP, pdu, len);
}

static void smp_fail(smp_sess_t *s, uint8_t reason) {
    uint8_t p[2] = { SMP_PAIRING_FAILED, reason };
    smp_send(s->handle, p, 2);
    s->state = SMP_FAILED;
    kprintf("[BT-SMP] pairing FAILED (reason 0x%02x)\n", reason);
    bootlog_write("[BT-SMP] pairing FAILED reason=0x%02x handle=0x%04x", reason, s->handle);
}

// Build + send the Pairing Request and record it (needed by c1 / feature match).
// We advertise Secure Connections + Bonding, IO cap NoInputNoOutput (Just
// Works), MITM not required. Modern BT5.0 HID refuse without the SC bit.
static void smp_send_pairing_req(smp_sess_t *s) {
    uint8_t *p = s->preq;
    p[0] = SMP_PAIRING_REQUEST;
    p[1] = 0x03;   // IO capability: NoInputNoOutput -> Just Works
    p[2] = 0x00;   // OOB: not present
    p[3] = SMP_AUTH_BONDING | SMP_AUTH_SC;   // 0x09: bonding + Secure Connections
    p[4] = 0x10;   // Max encryption key size 16
    p[5] = 0x00;   // Initiator key distribution: none
    p[6] = 0x03;   // Responder key distribution: EncKey + IdKey (LESC: LTK is
                   // computed, but request keys the peer may still distribute)
    smp_send(s->handle, s->preq, 7);
    s->state = SMP_W_PAIR_RSP;
    kprintf("[BT-SMP] -> Pairing Request (SC + Just Works) handle 0x%04x\n", s->handle);
    bootlog_write("[BT-SMP] -> Pairing Request (SC + Just Works) handle=0x%04x", s->handle);
}

// LESC: build our ephemeral P-256 keypair and send the Public Key PDU (X||Y in
// little-endian wire order). Returns 0 on success.
static int smp_send_public_key(smp_sess_t *s) {
    uint8_t pub[65]; size_t publen = 0;
    if (ecdh_generate_keypair(ECDSA_CURVE_P256, s->priv, sizeof(s->priv),
                              pub, sizeof(pub), &publen) != 0 || publen != 65) {
        kprintf("[BT-SMP] LESC keypair generation FAILED\n");
        bootlog_write("[BT-SMP] LESC keypair generation FAILED handle=0x%04x", s->handle);
        return -1;
    }
    // pub = 0x04 || X(32, big-endian) || Y(32, big-endian). Wire wants LE.
    smp_rev(s->pka,      pub + 1,      32);   // X_le
    smp_rev(s->pka + 32, pub + 1 + 32, 32);   // Y_le
    uint8_t pdu[65];
    pdu[0] = SMP_PAIRING_PUBLIC_KEY;
    memcpy(pdu + 1, s->pka, 64);
    smp_send(s->handle, pdu, 65);
    s->state = SMP_SC_W_PUBKEY;
    kprintf("[BT-SMP] LESC -> Public Key (64B) handle 0x%04x\n", s->handle);
    bootlog_write("[BT-SMP] LESC -> Public Key (64B) handle=0x%04x", s->handle);
    return 0;
}

// IOcap message field {AuthReq, OOB, IOcap} read out of a 7-byte pairing PDU.
static void smp_iocap_from_pdu(const uint8_t pdu[7], uint8_t iocap[3]) {
    iocap[0] = pdu[3];   // AuthReq
    iocap[1] = pdu[2];   // OOB data flag
    iocap[2] = pdu[1];   // IO capability
}

void smp_input(hci_handle_t h, const uint8_t *data, uint16_t len) {
    if (len < 1) return;
    uint8_t op = data[0];
    smp_sess_t *s = smp_by_handle(h);

    if (op == SMP_SECURITY_REQUEST) {
        // Peripheral asks us (central) to start security. Begin pairing if we
        // have no bond, else start encryption with the stored LTK.
        if (!s) s = smp_alloc(h);
        if (!s) return;
        hci_conn_t *c = hci_conn_by_handle(h);
        if (c) { s->peer = c->peer; s->peer_type = c->peer_addr_type; }
        le_bond_t *b = NULL;
        for (int i = 0; i < LE_MAX_BONDS; i++)
            if (g_le_bonds[i].valid && bt_addr_eq(&g_le_bonds[i].addr, &s->peer)) b = &g_le_bonds[i];
        if (b) {
            kprintf("[BT-SMP] Security Request: encrypting with stored LTK\n");
            hci_le_start_encryption(h, b->rand, (uint16_t)(b->ediv[0] | (b->ediv[1] << 8)), b->ltk);
        } else {
            smp_send_pairing_req(s);
        }
        return;
    }
    if (!s || !s->active) {
        kprintf("[BT-SMP] SMP op 0x%02x with no session (handle 0x%04x)\n", op, h);
        return;
    }

    switch (op) {
        case SMP_PAIRING_RESPONSE: {
            if (len < 7) { smp_fail(s, SMP_ERR_INVALID_PARAMS); return; }
            memcpy(s->pres, data, 7);
            // LESC only if BOTH sides set the SC bit.
            s->sc = (s->preq[3] & SMP_AUTH_SC) && (s->pres[3] & SMP_AUTH_SC);
            if (s->sc) {
                kprintf("[BT-SMP] <- Pairing Response (SC agreed); LESC public-key exchange\n");
                bootlog_write("[BT-SMP] <- Pairing Response: SC agreed (LESC), public-key exchange handle=0x%04x", h);
                if (smp_send_public_key(s) != 0) smp_fail(s, SMP_ERR_INVALID_PARAMS);
                break;
            }
            // ---- LEGACY Just Works path ----
            if (s->pres[3] & SMP_AUTH_MITM)
                kprintf("[BT-SMP] NOTE: peer AuthReq requests MITM; Just Works may be rejected\n");
            memset(s->tk, 0, 16);   // TK = 0 for Just Works.
            rng_get_bytes(s->mrand, 16);
            const bt_addr_t *ia = hci_local_addr();
            smp_c1(s->tk, s->mrand, s->preq, s->pres,
                   0x00, ia->b, s->peer_type, s->peer.b, s->mconfirm);
            uint8_t p[17]; p[0] = SMP_PAIRING_CONFIRM; memcpy(p + 1, s->mconfirm, 16);
            smp_send(h, p, 17);
            s->state = SMP_W_SCONFIRM;
            kprintf("[BT-SMP] <- Pairing Response (legacy); -> Pairing Confirm\n");
            bootlog_write("[BT-SMP] <- Pairing Response: legacy Just Works handle=0x%04x", h);
            break;
        }
        case SMP_PAIRING_PUBLIC_KEY: {
            if (!s->sc || s->state != SMP_SC_W_PUBKEY) { smp_fail(s, SMP_ERR_INVALID_PARAMS); return; }
            if (len < 65) { smp_fail(s, SMP_ERR_INVALID_PARAMS); return; }
            memcpy(s->pkb, data + 1, 64);
            // Build the peer point (0x04 || X_be || Y_be) and compute DHKey.
            uint8_t peer_pt[65]; peer_pt[0] = 0x04;
            smp_rev(peer_pt + 1,      s->pkb,      32);   // X_be
            smp_rev(peer_pt + 1 + 32, s->pkb + 32, 32);   // Y_be
            uint8_t dh_be[32];
            if (ecdh_compute_shared(ECDSA_CURVE_P256, s->priv, sizeof(s->priv),
                                    peer_pt, sizeof(peer_pt), dh_be, sizeof(dh_be)) != 0) {
                kprintf("[BT-SMP] LESC DHKey / peer public key INVALID\n");
                bootlog_write("[BT-SMP] LESC DHKey / peer public key INVALID handle=0x%04x", h);
                smp_fail(s, SMP_ERR_DHKEY_CHECK);
                return;
            }
            smp_rev(s->dhkey, dh_be, 32);   // store little-endian
            s->state = SMP_SC_W_CONFIRM;    // responder sends its Confirm next
            kprintf("[BT-SMP] LESC <- Public Key; DHKey computed; awaiting Confirm\n");
            bootlog_write("[BT-SMP] LESC <- Public Key; DHKey computed handle=0x%04x", h);
            break;
        }
        case SMP_PAIRING_CONFIRM: {
            if (len < 17) { smp_fail(s, SMP_ERR_INVALID_PARAMS); return; }
            if (s->sc) {
                // LESC Just Works: responder's Cb. Reply with our nonce Na.
                if (s->state != SMP_SC_W_CONFIRM) { smp_fail(s, SMP_ERR_INVALID_PARAMS); return; }
                memcpy(s->peer_confirm, data + 1, 16);
                rng_get_bytes(s->na, 16);
                uint8_t p[17]; p[0] = SMP_PAIRING_RANDOM; memcpy(p + 1, s->na, 16);
                smp_send(h, p, 17);
                s->state = SMP_SC_W_RANDOM;
                kprintf("[BT-SMP] LESC <- Confirm; -> Random (Na)\n");
                break;
            }
            // ---- LEGACY ----
            memcpy(s->sconfirm, data + 1, 16);
            uint8_t p[17]; p[0] = SMP_PAIRING_RANDOM; memcpy(p + 1, s->mrand, 16);
            smp_send(h, p, 17);
            s->state = SMP_W_SRAND;
            kprintf("[BT-SMP] <- Pairing Confirm; -> Pairing Random\n");
            break;
        }
        case SMP_PAIRING_RANDOM: {
            if (len < 17) { smp_fail(s, SMP_ERR_INVALID_PARAMS); return; }
            if (s->sc) {
                // LESC: responder's Nb. Verify its earlier Confirm, then run
                // f5/f6 and send our DHKey Check (Ea).
                if (s->state != SMP_SC_W_RANDOM) { smp_fail(s, SMP_ERR_INVALID_PARAMS); return; }
                memcpy(s->nb, data + 1, 16);
                // Cb = f4(PKbx, PKax, Nb, 0). PKbx = pkb X-coord, PKax = pka X-coord.
                uint8_t cb[16];
                smp_f4(s->pkb, s->pka, s->nb, 0x00, cb);
                if (memcmp(cb, s->peer_confirm, 16) != 0) {
                    kprintf("[BT-SMP] LESC confirm (f4) mismatch - aborting\n");
                    bootlog_write("[BT-SMP] LESC confirm f4 MISMATCH - aborting handle=0x%04x", h);
                    smp_fail(s, SMP_ERR_CONFIRM_FAILED);
                    return;
                }
                // Numeric-comparison value g2 (auto-accepted for NoInputNoOutput).
                uint32_t g = smp_g2(s->pka, s->pkb, s->na, s->nb);
                kprintf("[BT-SMP] LESC f4 OK; numeric value %06u (Just Works auto-accept)\n",
                        g % 1000000u);
                bootlog_write("[BT-SMP] LESC f4 OK; numeric %06u (Just Works) handle=0x%04x",
                              g % 1000000u, h);
                // f5: MacKey || LTK = f5(DHKey, Na, Nb, A, B).
                const bt_addr_t *ia = hci_local_addr();
                smp_f5(s->dhkey, s->na, s->nb,
                       0x00, ia->b, s->peer_type, s->peer.b, s->mackey, s->ltk);
                // Ea = f6(MacKey, Na, Nb, 0, IOcapA, A, B).
                uint8_t r0[16]; memset(r0, 0, 16);
                uint8_t iocap_a[3]; smp_iocap_from_pdu(s->preq, iocap_a);
                uint8_t ea[16];
                smp_f6(s->mackey, s->na, s->nb, r0, iocap_a,
                       0x00, ia->b, s->peer_type, s->peer.b, ea);
                uint8_t p[17]; p[0] = SMP_PAIRING_DHKEY_CHECK; memcpy(p + 1, ea, 16);
                smp_send(h, p, 17);
                s->state = SMP_SC_W_DHK_CHECK;
                kprintf("[BT-SMP] LESC <- Random (Nb); LTK derived; -> DHKey Check (Ea)\n");
                bootlog_write("[BT-SMP] LESC <- Random (Nb); LTK derived; -> DHKey Check (Ea) handle=0x%04x", h);
                break;
            }
            // ---- LEGACY ----
            memcpy(s->srand, data + 1, 16);
            uint8_t check[16];
            const bt_addr_t *ia = hci_local_addr();
            smp_c1(s->tk, s->srand, s->preq, s->pres,
                   0x00, ia->b, s->peer_type, s->peer.b, check);
            if (memcmp(check, s->sconfirm, 16) != 0) {
                kprintf("[BT-SMP] Sconfirm mismatch - aborting\n");
                bootlog_write("[BT-SMP] legacy Sconfirm MISMATCH - aborting handle=0x%04x", h);
                smp_fail(s, SMP_ERR_CONFIRM_FAILED);
                return;
            }
            smp_s1(s->tk, s->srand, s->mrand, s->stk);
            memcpy(s->ltk, s->stk, 16);
            s->state = SMP_W_ENC;
            uint8_t z8[8]; memset(z8, 0, 8);
            hci_le_start_encryption(h, z8, 0, s->stk);   // EDIV=0, Rand=0, LTK=STK
            kprintf("[BT-SMP] <- Pairing Random OK; STK derived; starting encryption\n");
            bootlog_write("[BT-SMP] legacy <- Pairing Random OK; STK derived; encrypting handle=0x%04x", h);
            break;
        }
        case SMP_PAIRING_DHKEY_CHECK: {
            if (!s->sc || s->state != SMP_SC_W_DHK_CHECK) { smp_fail(s, SMP_ERR_INVALID_PARAMS); return; }
            if (len < 17) { smp_fail(s, SMP_ERR_INVALID_PARAMS); return; }
            // Eb = f6(MacKey, Nb, Na, 0, IOcapB, B, A).
            uint8_t r0[16]; memset(r0, 0, 16);
            uint8_t iocap_b[3]; smp_iocap_from_pdu(s->pres, iocap_b);
            const bt_addr_t *ia = hci_local_addr();
            uint8_t eb_calc[16];
            smp_f6(s->mackey, s->nb, s->na, r0, iocap_b,
                   s->peer_type, s->peer.b, 0x00, ia->b, eb_calc);
            if (memcmp(eb_calc, data + 1, 16) != 0) {
                kprintf("[BT-SMP] LESC DHKey Check (Eb) mismatch - aborting\n");
                bootlog_write("[BT-SMP] LESC DHKey Check (Eb) MISMATCH - aborting handle=0x%04x", h);
                smp_fail(s, SMP_ERR_DHKEY_CHECK);
                return;
            }
            // Store the LESC bond (EDIV=0, Rand=0) for reconnection.
            le_bond_t *b = le_bond_get(&s->peer);
            if (b) {
                b->addr = s->peer; b->addr_type = s->peer_type;
                memcpy(b->ltk, s->ltk, 16);
                memset(b->ediv, 0, 2); memset(b->rand, 0, 8);
                b->valid = 1;
            }
            set_state(&s->peer, BT_PAIR_BONDED);
            s->state = SMP_W_ENC;
            uint8_t z8[8]; memset(z8, 0, 8);
            hci_le_start_encryption(h, z8, 0, s->ltk);   // EDIV=0, Rand=0, LTK
            kprintf("[BT-SMP] LESC <- DHKey Check OK; pairing complete; starting encryption\n");
            bootlog_write("[BT-SMP] LESC DHKey Check OK; PAIRING COMPLETE; encrypting handle=0x%04x", h);
            break;
        }
        case SMP_ENCRYPTION_INFO: {          // peer LTK (legacy key distribution)
            if (len < 17) return;
            le_bond_t *b = le_bond_get(&s->peer);
            if (b) {
                b->addr = s->peer; b->addr_type = s->peer_type;
                memcpy(b->ltk, data + 1, 16);
                b->valid = 1;   // mark now so MASTER_IDENT lands in the SAME slot
            }
            break;
        }
        case SMP_MASTER_IDENT: {             // EDIV + Rand -> completes the bond
            if (len < 11) return;
            le_bond_t *b = le_bond_get(&s->peer);
            if (b) {
                memcpy(b->ediv, data + 1, 2);
                memcpy(b->rand, data + 3, 8);
                b->valid = 1;
                kprintf("[BT-SMP] LE bond stored (LTK+EDIV+Rand) - device bonded\n");
                bootlog_write("[BT-SMP] LE bond stored (LTK+EDIV+Rand) - device BONDED handle=0x%04x", h);
            }
            s->state = SMP_DONE;
            break;
        }
        case SMP_IDENTITY_INFO:
        case SMP_IDENTITY_ADDR_INFO:
        case SMP_SIGNING_INFO:
            break;   // consumed
        case SMP_PAIRING_FAILED:
            kprintf("[BT-SMP] peer sent Pairing Failed reason=0x%02x\n", len >= 2 ? data[1] : 0);
            bootlog_write("[BT-SMP] peer sent Pairing Failed reason=0x%02x handle=0x%04x", len >= 2 ? data[1] : 0, h);
            s->state = SMP_FAILED;
            break;
        default:
            kprintf("[BT-SMP] unhandled SMP op 0x%02x\n", op);
            break;
    }
}

// Known-answer self-test of the SMP crypto so the pairing math is proven live
// even with no keyboard present. Legacy: c1/s1 (Core spec D.1/D.2). AES-CMAC:
// RFC 4493. LESC: f4/f5/f6/g2 (Core spec D.2-D.5 worked examples). The spec
// worked examples are printed MSB-first; the LESC wrappers take little-endian
// wire order, so each vector is reversed into LE before the call and the LE
// result reversed back to MSB-first for the compare.
static int mac_eq(const uint8_t *a, const uint8_t *b, int n) { return memcmp(a, b, n) == 0; }

static void smp_selftest(void) {
    // ---- legacy c1 / s1 ----
    uint8_t k[16]; memset(k, 0, 16);
    const uint8_t r[16] = { 0xe0,0x2e,0x70,0xc6,0x4e,0x27,0x88,0x63,
                            0x0e,0x6f,0xad,0x56,0x21,0xd5,0x83,0x57 };
    const uint8_t preq[7] = { 0x01,0x01,0x00,0x00,0x10,0x07,0x07 };
    const uint8_t pres[7] = { 0x02,0x03,0x00,0x00,0x08,0x00,0x05 };
    const uint8_t ia[6] = { 0xa6,0xa5,0xa4,0xa3,0xa2,0xa1 };
    const uint8_t ra[6] = { 0xb6,0xb5,0xb4,0xb3,0xb2,0xb1 };
    const uint8_t exp_c1[16] = { 0x86,0x3b,0xf1,0xbe,0xc5,0x4d,0xa7,0xd2,
                                 0xea,0x88,0x89,0x87,0xef,0x3f,0x1e,0x1e };
    uint8_t out[16];
    smp_c1(k, r, preq, pres, 0x01, ia, 0x00, ra, out);
    int c1_ok = mac_eq(out, exp_c1, 16);

    const uint8_t r1[16] = { 0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
                             0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x00 };
    const uint8_t r2[16] = { 0x00,0xff,0xee,0xdd,0xcc,0xbb,0xaa,0x99,
                             0x88,0x07,0x06,0x05,0x04,0x03,0x02,0x01 };
    const uint8_t exp_s1[16] = { 0x62,0xa0,0x6d,0x79,0xae,0x16,0x42,0x5b,
                                 0x9b,0xf4,0xb0,0xe8,0xf0,0xe1,0x1f,0x9a };
    smp_s1(k, r1, r2, out);
    int s1_ok = mac_eq(out, exp_s1, 16);

    // ---- AES-CMAC (RFC 4493) ----
    const uint8_t cmac_key[16] = { 0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
                                   0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
    const uint8_t cmac_msg[64] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
        0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
        0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
        0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10 };
    const uint8_t cmac_e0[16] = { 0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,
                                  0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46 };
    const uint8_t cmac_e16[16] = { 0x07,0x0a,0x16,0xb4,0x6b,0x4d,0x41,0x44,
                                   0xf7,0x9b,0xdd,0x9d,0xd0,0x4a,0x28,0x7c };
    const uint8_t cmac_e40[16] = { 0xdf,0xa6,0x67,0x47,0xde,0x9a,0xe6,0x30,
                                   0x30,0xca,0x32,0x61,0x14,0x97,0xc8,0x27 };
    const uint8_t cmac_e64[16] = { 0x51,0xf0,0xbe,0xbf,0x7e,0x3b,0x9d,0x92,
                                   0xfc,0x49,0x74,0x17,0x79,0x36,0x3c,0xfe };
    uint8_t mac[16]; int cmac_ok = 1;
    aes_cmac(cmac_key, cmac_msg, 0,  mac); cmac_ok &= mac_eq(mac, cmac_e0, 16);
    aes_cmac(cmac_key, cmac_msg, 16, mac); cmac_ok &= mac_eq(mac, cmac_e16, 16);
    aes_cmac(cmac_key, cmac_msg, 40, mac); cmac_ok &= mac_eq(mac, cmac_e40, 16);
    aes_cmac(cmac_key, cmac_msg, 64, mac); cmac_ok &= mac_eq(mac, cmac_e64, 16);

    // ---- LESC f4/f5/f6/g2 (Core spec D.2-D.5), fed as reversed LE ----
    // Public-key X coords (MSB-first as printed in the spec).
    const uint8_t U_be[32] = {
        0x20,0xb0,0x03,0xd2,0xf2,0x97,0xbe,0x2c,0x5e,0x2c,0x83,0xa7,0xe9,0xf9,0xa5,0xb9,
        0xef,0xf4,0x91,0x11,0xac,0xf4,0xfd,0xdb,0xcc,0x03,0x01,0x48,0x0e,0x35,0x9d,0xe6 };
    const uint8_t V_be[32] = {
        0x55,0x18,0x8b,0x3d,0x32,0xf6,0xbb,0x9a,0x90,0x0a,0xfc,0xfb,0xee,0xd4,0xe7,0x2a,
        0x59,0xcb,0x9a,0xc2,0xf1,0x9d,0x7c,0xfb,0x6b,0x4f,0xdd,0x49,0xf4,0x7f,0xc5,0xfd };
    const uint8_t X_be[16] = { 0xd5,0xcb,0x84,0x54,0xd1,0x77,0x73,0x3e,
                               0xff,0xff,0xb2,0xec,0x71,0x2b,0xae,0xab };   // Na
    const uint8_t Y_be[16] = { 0xa6,0xe8,0xe7,0xcc,0x25,0xa7,0x5f,0x6e,
                               0x21,0x65,0x83,0xf7,0xff,0x3d,0xc4,0xcf };   // Nb
    uint8_t U_le[32], V_le[32], X_le[16], Y_le[16];
    smp_rev(U_le, U_be, 32); smp_rev(V_le, V_be, 32);
    smp_rev(X_le, X_be, 16); smp_rev(Y_le, Y_be, 16);

    // f4
    const uint8_t f4_exp_be[16] = { 0xf2,0xc9,0x16,0xf1,0x07,0xa9,0xbd,0x1c,
                                    0xf1,0xed,0xa1,0xbe,0xa9,0x74,0x87,0x2d };
    uint8_t f4_le[16], f4_be[16];
    smp_f4(U_le, V_le, X_le, 0x00, f4_le);
    smp_rev(f4_be, f4_le, 16);
    int f4_ok = mac_eq(f4_be, f4_exp_be, 16);

    // f5: W(DHKey), N1=Na, N2=Nb, A1=00||56 12 37 37 bf ce, A2=00||a7 13 70 2d cf c1
    const uint8_t W_be[32] = {
        0xec,0x02,0x34,0xa3,0x57,0xc8,0xad,0x05,0x34,0x10,0x10,0xa6,0x0a,0x39,0x7d,0x9b,
        0x99,0x79,0x6b,0x13,0xb4,0xf8,0x66,0xf1,0x86,0x8d,0x34,0xf3,0x73,0xbf,0xa6,0x98 };
    const uint8_t A1_be[6] = { 0x56,0x12,0x37,0x37,0xbf,0xce };
    const uint8_t A2_be[6] = { 0xa7,0x13,0x70,0x2d,0xcf,0xc1 };
    const uint8_t mackey_exp_be[16] = { 0x29,0x65,0xf1,0x76,0xa1,0x08,0x4a,0x02,
                                        0xfd,0x3f,0x6a,0x20,0xce,0x63,0x6e,0x20 };
    const uint8_t ltk_exp_be[16] = { 0x69,0x86,0x79,0x11,0x69,0xd7,0xcd,0x23,
                                     0x98,0x05,0x22,0xb5,0x94,0x75,0x0a,0x38 };
    uint8_t W_le[32], A1_le[6], A2_le[6];
    smp_rev(W_le, W_be, 32); smp_rev(A1_le, A1_be, 6); smp_rev(A2_le, A2_be, 6);
    uint8_t mk_le[16], ltk_le[16], mk_be[16], ltk_be[16];
    smp_f5(W_le, X_le, Y_le, 0x00, A1_le, 0x00, A2_le, mk_le, ltk_le);
    smp_rev(mk_be, mk_le, 16); smp_rev(ltk_be, ltk_le, 16);
    int f5_ok = mac_eq(mk_be, mackey_exp_be, 16) && mac_eq(ltk_be, ltk_exp_be, 16);

    // f6: W=MacKey, N1=Na, N2=Nb, R, IOcap=010102, A1, A2
    const uint8_t R_be[16] = { 0x12,0xa3,0x34,0x3b,0xb4,0x53,0xbb,0x54,
                               0x08,0xda,0x42,0xd2,0x0c,0x2d,0x0f,0xc8 };
    const uint8_t iocap[3] = { 0x01,0x01,0x02 };
    const uint8_t f6_exp_be[16] = { 0xe3,0xc4,0x73,0x98,0x9c,0xd0,0xe8,0xc5,
                                    0xd2,0x6c,0x0b,0x09,0xda,0x95,0x8f,0x61 };
    uint8_t R_le[16]; smp_rev(R_le, R_be, 16);
    uint8_t f6_le[16], f6_be[16];
    smp_f6(mk_le, X_le, Y_le, R_le, iocap, 0x00, A1_le, 0x00, A2_le, f6_le);
    smp_rev(f6_be, f6_le, 16);
    int f6_ok = mac_eq(f6_be, f6_exp_be, 16);

    // g2: U, V, X=Na, Y=Nb -> compare value 0x2f9ed5ba
    uint32_t g = smp_g2(U_le, V_le, X_le, Y_le);
    int g2_ok = (g == 0x2f9ed5bau);

    int lesc_ok = cmac_ok && f4_ok && f5_ok && f6_ok && g2_ok;
    kprintf("[BT-SMP] legacy crypto self-test: c1 %s, s1 %s\n",
            c1_ok ? "PASS" : "FAIL", s1_ok ? "PASS" : "FAIL");
    kprintf("[BT-SMP] LESC self-test: %s (aes-cmac %s, f4 %s, f5 %s, f6 %s, g2 %s) "
            "[RFC 4493 + Core Vol 3 Part H D.2-D.5]\n",
            lesc_ok ? "PASS" : "FAIL",
            cmac_ok ? "PASS" : "FAIL", f4_ok ? "PASS" : "FAIL",
            f5_ok ? "PASS" : "FAIL", f6_ok ? "PASS" : "FAIL",
            g2_ok ? "PASS" : "FAIL");
    bootlog_write("[BT-SMP] crypto self-test: legacy(c1 %s, s1 %s) LESC %s",
                  c1_ok ? "OK" : "FAIL", s1_ok ? "OK" : "FAIL", lesc_ok ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// SSP event handling (HCI observer)
// ---------------------------------------------------------------------------
static int pair_hci_event(uint8_t evt, const uint8_t *params, uint8_t plen) {
    bt_addr_t addr;
    switch (evt) {
        case HCI_EVT_IO_CAP_REQUEST: {           // params: bdaddr(6)
            if (plen < 6) return 0;
            memcpy(addr.b, params, 6);
            set_state(&addr, BT_PAIR_SSP_IN_PROGRESS);
            uint8_t p[9];
            memcpy(p, addr.b, 6);
            p[6] = BT_IO_CAP_NO_IO;   // 0x03 NoInputNoOutput -> Just Works
            p[7] = 0x00;              // no OOB data
            p[8] = 0x04;              // general bonding, MITM not required
            hci_send_cmd(HCI_CMD_IO_CAP_REQ_REPLY, p, 9);
            return 1;
        }
        case HCI_EVT_IO_CAP_RESPONSE:
            return 1;                 // informational
        case HCI_EVT_USER_CONFIRM_REQUEST: {     // params: bdaddr(6)+numeric(4)
            if (plen < 6) return 0;
            memcpy(addr.b, params, 6);
            uint32_t numeric = 0;
            if (plen >= 10) numeric = (uint32_t)(params[6] | (params[7] << 8) |
                                                 (params[8] << 16) | (params[9] << 24));
            int accept = bt_pair_confirm_policy(&addr, numeric);
            uint8_t p[6];
            memcpy(p, addr.b, 6);
            hci_send_cmd(accept ? HCI_CMD_USER_CONFIRM_REPLY : HCI_CMD_USER_CONFIRM_NEG, p, 6);
            return 1;
        }
        case HCI_EVT_USER_PASSKEY_REQUEST: {     // NoIO: best-effort reply 0
            if (plen < 6) return 0;
            memcpy(addr.b, params, 6);
            uint8_t p[10];
            memcpy(p, addr.b, 6);
            p[6] = p[7] = p[8] = p[9] = 0;       // passkey 0
            hci_send_cmd(HCI_CMD_USER_PASSKEY_REPLY, p, 10);
            return 1;
        }
        case HCI_EVT_LINK_KEY_REQUEST: {         // params: bdaddr(6)
            if (plen < 6) return 0;
            memcpy(addr.b, params, 6);
            uint8_t key[16];
            if (bt_pair_find_link_key(&addr, key) == BT_OK) {
                uint8_t p[22];
                memcpy(p, addr.b, 6);
                memcpy(p + 6, key, 16);
                hci_send_cmd(HCI_CMD_LINK_KEY_REPLY, p, 22);
            } else {
                uint8_t p[6];
                memcpy(p, addr.b, 6);
                hci_send_cmd(HCI_CMD_LINK_KEY_NEG_REPLY, p, 6);
            }
            return 1;
        }
        case HCI_EVT_LINK_KEY_NOTIFICATION: {    // params: bdaddr(6)+key(16)+type(1)
            if (plen < 22) return 0;
            memcpy(addr.b, params, 6);
            bt_pair_store_link_key(&addr, params + 6);
            set_state(&addr, BT_PAIR_BONDED);
            kprintf("[BT-PAIR] link key stored, device bonded\n");
            bootlog_write("[BT-PAIR] Classic link key stored - device BONDED %02x:%02x:%02x:%02x:%02x:%02x",
                          addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0]);
            return 1;
        }
        case HCI_EVT_PIN_CODE_REQUEST: {         // legacy fallback: "0000"
            if (plen < 6) return 0;
            memcpy(addr.b, params, 6);
            uint8_t p[23];
            memset(p, 0, sizeof(p));
            memcpy(p, addr.b, 6);
            p[6] = 4;
            p[7] = '0'; p[8] = '0'; p[9] = '0'; p[10] = '0';
            hci_send_cmd(HCI_CMD_PIN_CODE_REPLY, p, 23);
            return 1;
        }
        case HCI_EVT_SIMPLE_PAIRING_COMPLETE: {  // params: status(1)+bdaddr(6)
            if (plen < 7) return 0;
            uint8_t status = params[0];
            memcpy(addr.b, params + 1, 6);
            set_state(&addr, status == HCI_SUCCESS ? BT_PAIR_BONDED : BT_PAIR_FAILED);
            kprintf("[BT-PAIR] simple pairing complete status=0x%02x\n", status);
            bootlog_write("[BT-PAIR] simple pairing complete status=0x%02x %02x:%02x:%02x:%02x:%02x:%02x",
                          status, addr.b[5], addr.b[4], addr.b[3], addr.b[2], addr.b[1], addr.b[0]);
            return 1;
        }
        case HCI_EVT_AUTH_COMPLETE:
        case HCI_EVT_ENCRYPT_CHANGE:
            return 1;
        case HCI_EVT_LE_META:
            if (plen >= 13 && params[0] == HCI_LE_SUBEVT_LTK_REQUEST) {
                // LE Long Term Key Request (raised when WE are the peripheral of
                // the encryption procedure). params: sub(1) handle(2) rand(8) ediv(2).
                uint16_t h = (uint16_t)(params[1] | (params[2] << 8));
                const uint8_t *rnd = params + 3;
                uint16_t ediv = (uint16_t)(params[11] | (params[12] << 8));
                uint8_t ltk[16]; int have = 0;
                smp_sess_t *ss = smp_by_handle(h);
                // Freshly-paired session (legacy STK or LESC LTK), EDIV/Rand 0.
                if (ediv == 0 && ss && ss->state == SMP_W_ENC) { memcpy(ltk, ss->ltk, 16); have = 1; }
                if (!have) {
                    for (int i = 0; i < LE_MAX_BONDS; i++)
                        if (g_le_bonds[i].valid &&
                            memcmp(g_le_bonds[i].ediv, &ediv, 2) == 0 &&
                            memcmp(g_le_bonds[i].rand, rnd, 8) == 0) {
                            memcpy(ltk, g_le_bonds[i].ltk, 16); have = 1; break;
                        }
                }
                if (have) {
                    uint8_t p[18]; p[0] = (uint8_t)h; p[1] = (uint8_t)(h >> 8); memcpy(p + 2, ltk, 16);
                    hci_send_cmd(HCI_CMD_LE_LTK_REQ_REPLY, p, 18);
                } else {
                    uint8_t p[2] = { (uint8_t)h, (uint8_t)(h >> 8) };
                    hci_send_cmd(HCI_CMD_LE_LTK_REQ_NEG_REPLY, p, 2);
                }
                return 1;
            }
            return 0;
        default:
            return 0;
    }
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
int pair_start_classic(hci_handle_t h, const bt_addr_t *addr) {
    if (addr) set_state(addr, BT_PAIR_SSP_IN_PROGRESS);
    uint8_t p[2] = { (uint8_t)(h & 0xFF), (uint8_t)((h >> 8) & 0xFF) };
    return hci_send_cmd(HCI_CMD_AUTH_REQUESTED, p, 2);
}

int pair_start_le(hci_handle_t h, const bt_addr_t *addr) {
    smp_sess_t *s = smp_alloc(h);
    if (!s) return BT_ERR_NOMEM;
    hci_conn_t *c = hci_conn_by_handle(h);
    if (addr) s->peer = *addr;
    else if (c) s->peer = c->peer;
    s->peer_type = c ? c->peer_addr_type : 0;
    if (addr) set_state(addr, BT_PAIR_SMP_IN_PROGRESS);
    smp_send_pairing_req(s);
    return BT_OK;
}

int pair_user_confirm(const bt_addr_t *addr, int accept) {
    (void)accept;
    return bt_pair_confirm_policy(addr, 0);
}

void pair_poll(void) { /* no SMP timers yet */ }

int pair_init(void) {
    memset(g_bonds, 0, sizeof(g_bonds));
    memset(g_pstate, 0, sizeof(g_pstate));
    memset(g_le_bonds, 0, sizeof(g_le_bonds));
    memset(g_smp, 0, sizeof(g_smp));
    hci_add_evt_observer(pair_hci_event);
    smp_selftest();
    kprintf("[BT-PAIR] init: SSP + SMP (LESC + legacy Just-Works, NoInputNoOutput) registered\n");
    return BT_OK;
}
