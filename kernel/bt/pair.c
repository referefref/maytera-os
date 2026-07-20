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
// BLE SMP (LE fixed CID 0x0006) is scaffolded but not implemented in phase 1;
// LE LTK requests are acknowledged so they do not spam the log.
#include "pair.h"
#include "hci.h"
#include "hci_defs.h"
#include "l2cap.h"
#include "../serial.h"
#include "../string.h"
#include "../crypto/crypto.h"

extern int bt_pair_confirm_policy(const bt_addr_t *addr, uint32_t passkey);

// ===========================================================================
// BLE Security Manager (SMP) - LE legacy pairing, Just Works, central role.
// Runs over L2CAP fixed CID 0x0006. Crypto: security functions e / c1 / s1 from
// Bluetooth Core Vol 3 Part H, built on the kernel AES-128 (crypto/aes.c).
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
    SMP_IDLE = 0, SMP_W_PAIR_RSP, SMP_W_SCONFIRM, SMP_W_SRAND, SMP_W_ENC,
    SMP_DONE, SMP_FAILED,
} smp_state_t;

typedef struct {
    int          active;
    hci_handle_t handle;
    bt_addr_t    peer;
    uint8_t      peer_type;
    smp_state_t  state;
    uint8_t      preq[7], pres[7];
    uint8_t      tk[16], mrand[16], srand[16], mconfirm[16], sconfirm[16], stk[16];
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
}

// Build + send the Pairing Request and record it (needed by c1).
static void smp_send_pairing_req(smp_sess_t *s) {
    uint8_t *p = s->preq;
    p[0] = SMP_PAIRING_REQUEST;
    p[1] = 0x03;   // IO capability: NoInputNoOutput -> Just Works
    p[2] = 0x00;   // OOB: not present
    p[3] = 0x01;   // AuthReq: bonding, no MITM, legacy (no SC), no keypress
    p[4] = 0x10;   // Max key size 16
    p[5] = 0x00;   // Initiator key distribution: none
    p[6] = 0x01;   // Responder key distribution: EncKey (we want its LTK)
    smp_send(s->handle, s->preq, 7);
    s->state = SMP_W_PAIR_RSP;
    kprintf("[BT-SMP] -> Pairing Request (Just Works) handle 0x%04x\n", s->handle);
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
            if (len < 7) { smp_fail(s, 0x0A); return; }
            memcpy(s->pres, data, 7);
            if (s->pres[3] & 0x04)
                kprintf("[BT-SMP] NOTE: peer AuthReq requests MITM; Just Works may be rejected\n");
            // TK = 0 for Just Works.
            memset(s->tk, 0, 16);
            rng_get_bytes(s->mrand, 16);
            const bt_addr_t *ia = hci_local_addr();
            smp_c1(s->tk, s->mrand, s->preq, s->pres,
                   0x00, ia->b, s->peer_type, s->peer.b, s->mconfirm);
            uint8_t p[17]; p[0] = SMP_PAIRING_CONFIRM; memcpy(p + 1, s->mconfirm, 16);
            smp_send(h, p, 17);
            s->state = SMP_W_SCONFIRM;
            kprintf("[BT-SMP] <- Pairing Response; -> Pairing Confirm\n");
            break;
        }
        case SMP_PAIRING_CONFIRM: {
            if (len < 17) { smp_fail(s, 0x0A); return; }
            memcpy(s->sconfirm, data + 1, 16);
            uint8_t p[17]; p[0] = SMP_PAIRING_RANDOM; memcpy(p + 1, s->mrand, 16);
            smp_send(h, p, 17);
            s->state = SMP_W_SRAND;
            kprintf("[BT-SMP] <- Pairing Confirm; -> Pairing Random\n");
            break;
        }
        case SMP_PAIRING_RANDOM: {
            if (len < 17) { smp_fail(s, 0x0A); return; }
            memcpy(s->srand, data + 1, 16);
            uint8_t check[16];
            const bt_addr_t *ia = hci_local_addr();
            smp_c1(s->tk, s->srand, s->preq, s->pres,
                   0x00, ia->b, s->peer_type, s->peer.b, check);
            if (memcmp(check, s->sconfirm, 16) != 0) {
                kprintf("[BT-SMP] Sconfirm mismatch - aborting\n");
                smp_fail(s, 0x04);   // confirm value failed
                return;
            }
            smp_s1(s->tk, s->srand, s->mrand, s->stk);
            s->state = SMP_W_ENC;
            uint8_t z8[8]; memset(z8, 0, 8);
            hci_le_start_encryption(h, z8, 0, s->stk);   // EDIV=0, Rand=0, LTK=STK
            kprintf("[BT-SMP] <- Pairing Random OK; STK derived; starting encryption\n");
            break;
        }
        case SMP_ENCRYPTION_INFO: {          // peer LTK
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
            s->state = SMP_FAILED;
            break;
        default:
            kprintf("[BT-SMP] unhandled SMP op 0x%02x\n", op);
            break;
    }
}

// Known-answer self-test of the SMP crypto (Bluetooth Core spec D.1 / D.2
// vectors) so the pairing math is proven live even with no keyboard present.
static void smp_selftest(void) {
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
    int c1_ok = (memcmp(out, exp_c1, 16) == 0);

    const uint8_t r1[16] = { 0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,
                             0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x00 };
    const uint8_t r2[16] = { 0x00,0xff,0xee,0xdd,0xcc,0xbb,0xaa,0x99,
                             0x88,0x07,0x06,0x05,0x04,0x03,0x02,0x01 };
    const uint8_t exp_s1[16] = { 0x62,0xa0,0x6d,0x79,0xae,0x16,0x42,0x5b,
                                 0x9b,0xf4,0xb0,0xe8,0xf0,0xe1,0x1f,0x9a };
    smp_s1(k, r1, r2, out);
    int s1_ok = (memcmp(out, exp_s1, 16) == 0);
    kprintf("[BT-SMP] crypto self-test: c1 %s, s1 %s\n",
            c1_ok ? "PASS" : "FAIL", s1_ok ? "PASS" : "FAIL");
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
                if (ediv == 0 && ss && ss->state == SMP_W_ENC) { memcpy(ltk, ss->stk, 16); have = 1; }
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
    kprintf("[BT-PAIR] init: SSP + SMP (Just-Works, NoInputNoOutput) registered\n");
    return BT_OK;
}
