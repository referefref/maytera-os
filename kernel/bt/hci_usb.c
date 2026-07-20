// hci_usb.c - USB HCI transport driver for the MayteraOS Bluetooth stack (#372).
// TRANSPORT-agent owned. This is the LOWEST layer: it enumerates a USB
// Bluetooth dongle on the existing xHCI stack, claims the standard Bluetooth
// USB transport endpoints, and pumps HCI over them (Bluetooth Core spec Vol 4
// Part B, "HCI USB Transport Layer"):
//
//   - EP0 control (bmRequestType 0x20, bRequest 0x00): HCI COMMAND packets.
//   - interrupt-IN endpoint:                           HCI EVENT packets.
//   - bulk IN / bulk OUT endpoints:                    ACL data packets.
//   - (isochronous SCO endpoints on interface 1 are ignored: audio only.)
//
// USB puts each HCI packet class on its own endpoint, so - unlike a UART/H4
// transport - there is NO 1-byte packet-type indicator on the wire. We send the
// raw HCI command (starting at the opcode) and receive raw events/ACL.
//
// Transfer discipline mirrors usb_ecm.c / usb_hid.c: all completions are reaped
// through the shared xhci per-(slot,DCI) completion tables
// (xhci_int_in_submit/poll + xhci_control_transfer). No new event-ring consumer
// is introduced (see blame.md #307/#348 event-stealing history).
//
// Contract (bt_transport.h): bt_hci_usb_probe() registers the bt_transport_ops
// (send_cmd / send_acl / poll / get_bdaddr) with the HCI layer; poll() forwards
// inbound events/ACL via bt_transport_deliver_event/acl(). Enumeration
// (bt_usb_probe, called from drivers/xhci.c) matches the dongle, claims the
// endpoints, and registers the transport.
//
// Standalone transport proof (gated behind g_bt_enable): when Bluetooth is
// enabled, a one-shot self-test drives HCI_Reset -> Read_BD_ADDR (which it
// LOGS: proof the transport works end to end) -> a bounded Classic inquiry + LE
// scan that surfaces nearby device addresses. This proves the transport even
// while hci.c (protocol layer) is still a stub. Nothing here runs unless
// g_bt_enable == 1, so an odd/absent dongle can never affect boot.
#include "bt_transport.h"
#include "bt.h"
#include "hci.h"
#include "hci_defs.h"
#include "../drivers/xhci.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/pmm.h"
#include "../mm/heap.h"
#include "../fs/bootlog.h"
#include "../fs/fat.h"
#include "../proc/process.h"

extern fat_fs_t g_fat_fs;

// -----------------------------------------------------------------------------
// Enumerated controller context (one dongle supported, matching the real
// deployment). Filled by bt_usb_probe(); bt_hci_usb_probe() marks it present
// and registers the transport ops.
// -----------------------------------------------------------------------------
typedef struct {
    int present;
    xhci_controller_t *xhc;
    int slot_id;
    int speed;
    uint16_t vid, pid;

    int evt_dci;            // interrupt-IN (HCI events)
    int evt_mps;
    int acl_in_dci;         // bulk-IN  (ACL)  (-1 if absent)
    int acl_out_dci;        // bulk-OUT (ACL)  (-1 if absent)
    int acl_in_mps, acl_out_mps;

    int evt_pending;        // one interrupt-IN TD outstanding
    int acl_in_pending;     // one bulk-IN TD outstanding

    // DMA buffers (identity-mapped: phys == virt).
    uint8_t *evt_buf;       // interrupt-IN landing buffer
    uint8_t *cmd_buf;       // HCI command scratch (control OUT data stage)
    uint8_t *acl_in_buf;    // bulk-IN landing buffer
    uint8_t *acl_out_buf;   // bulk-OUT bounce buffer

    int      brought_up;    // one-shot self-test guard
    int      have_bdaddr;
    bt_addr_t bdaddr;       // cached from Read_BD_ADDR
} bt_usb_ctx_t;

static bt_usb_ctx_t g_ctx;

#define BT_EVT_BUF_LEN  260     // max HCI event = 2 hdr + 255 params
#define BT_ACL_BUF_LEN  2048

// -----------------------------------------------------------------------------
// Chipset classification (firmware-upload honesty).
// -----------------------------------------------------------------------------
typedef enum { CHIP_GENERIC, CHIP_CSR, CHIP_REALTEK, CHIP_INTEL, CHIP_BROADCOM } bt_chip_t;

// Known Realtek-based USB BT dongles that are NOT under Realtek's own 0x0BDA
// VID (rebadged RTL8761BU parts). The TP-Link UB500 (2357:0604) on the build server is one.
static int bt_is_realtek(uint16_t vid, uint16_t pid) {
    if (vid == 0x0BDA) return 1;            // Realtek's own VID (RTL8761/8821/8852)
    if (vid == 0x2357 && pid == 0x0604) return 1;   // TP-Link UB500 (RTL8761BU)
    if (vid == 0x0BDA && pid == 0x8771) return 1;   // RTL8761BU
    if (vid == 0x2550 && pid == 0x8761) return 1;   // generic RTL8761B
    return 0;
}

static bt_chip_t bt_classify(uint16_t vid, uint16_t pid) {
    if (bt_is_realtek(vid, pid)) return CHIP_REALTEK; // firmware upload required
    switch (vid) {
        case 0x0A12: return CHIP_CSR;       // CSR8510: plain HCI, no firmware
        case 0x8087: return CHIP_INTEL;     // Intel: firmware upload
        case 0x0A5C: return CHIP_BROADCOM;  // Broadcom/Cypress: patchram
        default:     return CHIP_GENERIC;
    }
}

static const char *bt_chip_name(bt_chip_t c) {
    switch (c) {
        case CHIP_CSR:      return "CSR (plain-HCI)";
        case CHIP_REALTEK:  return "Realtek (needs firmware upload)";
        case CHIP_INTEL:    return "Intel (needs firmware upload)";
        case CHIP_BROADCOM: return "Broadcom (may need patchram)";
        default:            return "generic";
    }
}

// -----------------------------------------------------------------------------
// Event RX plumbing (interrupt-IN). Keep exactly one TD outstanding.
// -----------------------------------------------------------------------------
static void bt_evt_submit(void) {
    if (!g_ctx.present || g_ctx.evt_pending) return;
    if (xhci_int_in_submit(g_ctx.xhc, g_ctx.slot_id, g_ctx.evt_dci,
                           (uint64_t)g_ctx.evt_buf, BT_EVT_BUF_LEN) == 0) {
        g_ctx.evt_pending = 1;
    }
}

// Reap a completed HCI event into out[] (up to cap). Returns event length (>0),
// 0 if none ready, <0 on endpoint error. Re-arms the interrupt-IN each call.
static int bt_evt_reap(uint8_t *out, int cap) {
    if (!g_ctx.present) return -1;
    bt_evt_submit();
    uint32_t got = 0;
    int r = xhci_int_in_poll(g_ctx.xhc, g_ctx.slot_id, g_ctx.evt_dci, &got,
                             BT_EVT_BUF_LEN);
    if (r > 0) {
        g_ctx.evt_pending = 0;
        int n = (int)got;
        if (n > cap) n = cap;
        if (n > 0) memcpy(out, g_ctx.evt_buf, n);
        bt_evt_submit();
        return n;
    }
    if (r < 0) {
        g_ctx.evt_pending = 0;
        bt_evt_submit();
        return -1;
    }
    return 0;
}

static void bt_acl_in_submit(void) {
    if (!g_ctx.present || g_ctx.acl_in_dci < 0 || g_ctx.acl_in_pending) return;
    if (xhci_int_in_submit(g_ctx.xhc, g_ctx.slot_id, g_ctx.acl_in_dci,
                           (uint64_t)g_ctx.acl_in_buf, BT_ACL_BUF_LEN) == 0) {
        g_ctx.acl_in_pending = 1;
    }
}

// -----------------------------------------------------------------------------
// Transport downcalls (HCI -> USB). Registered via bt_transport_ops.
// -----------------------------------------------------------------------------
// Send one raw HCI COMMAND packet (opcode_lo, opcode_hi, plen, params...) on
// EP0 as the class-specific control request per the USB BT transport spec.
static int hci_usb_send_cmd(const uint8_t *data, uint16_t len) {
    if (!g_ctx.present) return BT_ERR_NODEV;
    if (!data || len < 3 || len > 258) return BT_ERR_PARAM;
    memcpy(g_ctx.cmd_buf, data, len);
    int cc = xhci_control_transfer(g_ctx.xhc, g_ctx.slot_id,
                                   0x20 /* Host->Dev|Class|Device */, 0x00,
                                   0, 0, g_ctx.cmd_buf, len);
    return (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? BT_OK : BT_ERR_TIMEOUT;
}

// Send one ACL data packet over bulk-OUT (bounded ~40ms wait).
static int hci_usb_send_acl(const uint8_t *data, uint16_t len) {
    if (!g_ctx.present) return BT_ERR_NODEV;
    if (g_ctx.acl_out_dci < 0) return BT_ERR_NODEV;
    if (!data || len == 0 || len > BT_ACL_BUF_LEN) return BT_ERR_PARAM;
    uint32_t scrap = 0;
    (void)xhci_int_in_poll(g_ctx.xhc, g_ctx.slot_id, g_ctx.acl_out_dci, &scrap, 0);
    memcpy(g_ctx.acl_out_buf, data, len);
    if (xhci_int_in_submit(g_ctx.xhc, g_ctx.slot_id, g_ctx.acl_out_dci,
                           (uint64_t)g_ctx.acl_out_buf, len) != 0) return BT_ERR_TIMEOUT;
    for (int i = 0; i < 40; i++) {
        uint32_t done = 0;
        int r = xhci_int_in_poll(g_ctx.xhc, g_ctx.slot_id, g_ctx.acl_out_dci,
                                 &done, len);
        if (r > 0) return BT_OK;
        if (r < 0) return BT_ERR_HCI;
        xhci_delay_ms(1);
    }
    return BT_ERR_TIMEOUT;
}

// Forward-declares for the one-shot self-test invoked from poll().
static void bt_usb_selftest(void);

// Pump the RX endpoints: forward any completed HCI event / ACL packet to the
// registered upper-layer (hci.c) via bt_transport_deliver_*. Called from
// bt_poll() on the bt worker thread.
static void hci_usb_poll(void) {
    if (!g_ctx.present) return;

    // One-shot transport self-test the first time we are pumped with Bluetooth
    // enabled. Proves Reset + Read_BD_ADDR + scan even while hci.c is a stub.
    if (g_bt_enable && !g_ctx.brought_up) {
        g_ctx.brought_up = 1;
        bt_usb_selftest();
        return;   // next poll resumes normal delivery
    }

    uint8_t evt[BT_EVT_BUF_LEN];
    int n = bt_evt_reap(evt, sizeof(evt));
    if (n > 0) bt_transport_deliver_event(evt, (uint16_t)n);

    bt_acl_in_submit();
    uint32_t got = 0;
    int r = xhci_int_in_poll(g_ctx.xhc, g_ctx.slot_id, g_ctx.acl_in_dci, &got,
                             BT_ACL_BUF_LEN);
    if (g_ctx.acl_in_dci >= 0) {
        if (r > 0) {
            g_ctx.acl_in_pending = 0;
            if (got > 0) bt_transport_deliver_acl(g_ctx.acl_in_buf, (uint16_t)got);
            bt_acl_in_submit();
        } else if (r < 0) {
            g_ctx.acl_in_pending = 0;
            bt_acl_in_submit();
        }
    }
}

// Controller BD_ADDR, if the self-test has read it. Otherwise BT_ERR_NODEV so
// the HCI layer knows to issue HCI_READ_BD_ADDR itself.
static int hci_usb_get_bdaddr(bt_addr_t *out) {
    if (!g_ctx.present) return BT_ERR_NODEV;
    if (!g_ctx.have_bdaddr) return BT_ERR_NODEV;
    if (out) *out = g_ctx.bdaddr;
    return BT_OK;
}

static const bt_transport_ops_t g_usb_transport_ops = {
    .name       = "usb-hci",
    .send_cmd   = hci_usb_send_cmd,
    .send_acl   = hci_usb_send_acl,
    .poll       = hci_usb_poll,
    .get_bdaddr = hci_usb_get_bdaddr,
};

// -----------------------------------------------------------------------------
// Self-test helpers (transport proof). These reap events DIRECTLY (bypassing
// deliver) because they run synchronously to completion as a one-shot before
// normal RX delivery begins; there is no overlap with hci.c.
// -----------------------------------------------------------------------------
static int bt_send_op(uint16_t opcode, const uint8_t *params, int plen) {
    if (plen < 0 || plen > 255) return BT_ERR_PARAM;
    uint8_t pkt[258];
    pkt[0] = (uint8_t)(opcode & 0xFF);
    pkt[1] = (uint8_t)(opcode >> 8);
    pkt[2] = (uint8_t)plen;
    if (plen && params) memcpy(pkt + 3, params, plen);
    return hci_usb_send_cmd(pkt, (uint16_t)(plen + 3));
}

static void bt_log_bdaddr(const char *what, const uint8_t *le6) {
    // BD_ADDR is little-endian on the wire; print MSB first.
    bootlog_write("[BT] %s %02x:%02x:%02x:%02x:%02x:%02x", what,
                  le6[5], le6[4], le6[3], le6[2], le6[1], le6[0]);
    kprintf("[BT] %s %02x:%02x:%02x:%02x:%02x:%02x\n", what,
            le6[5], le6[4], le6[3], le6[2], le6[1], le6[0]);
}

static int g_found_classic;
static int g_found_le;

static void bt_log_scan_event(const uint8_t *e, int n) {
    if (n < 2) return;
    uint8_t code = e[0];
    if (code == HCI_EVT_INQUIRY_RESULT && n >= 3) {
        int num = e[2];
        for (int i = 0; i < num; i++) {
            int off = 3 + i * 6;
            if (off + 6 <= n) { bt_log_bdaddr("Classic device", e + off); g_found_classic++; }
        }
    } else if (code == HCI_EVT_INQUIRY_RESULT_RSSI && n >= 3) {
        int num = e[2];
        for (int i = 0; i < num; i++) {
            int off = 3 + i * 14;   // BD_ADDR(6)+PSRM(1)+res(1)+COD(3)+clk(2)+rssi(1)
            if (off + 6 <= n) { bt_log_bdaddr("Classic device (rssi)", e + off); g_found_classic++; }
        }
    } else if (code == HCI_EVT_EXT_INQUIRY_RESULT && n >= 9) {
        bt_log_bdaddr("Classic device (ext)", e + 3);
        g_found_classic++;
    } else if (code == HCI_EVT_INQUIRY_COMPLETE) {
        bootlog_write("[BT] Classic inquiry complete (%d found)", g_found_classic);
        kprintf("[BT] Classic inquiry complete (%d found)\n", g_found_classic);
    } else if (code == HCI_EVT_LE_META && n >= 4 && e[2] == HCI_LE_SUBEVT_ADV_REPORT) {
        int num = e[3];
        int off = 4;
        for (int i = 0; i < num && off + 8 <= n; i++) {
            // evt_type(1) addr_type(1) addr(6) len(1) data(len) rssi(1)
            bt_log_bdaddr("LE device", e + off + 2);
            g_found_le++;
            int dlen = e[off + 8];
            off += 9 + dlen + 1;
        }
    }
}

// Send a command and wait (bounded) for its Command Complete (0x0E) or Command
// Status (0x0F), surfacing any other events (inquiry/LE results) to the scan
// logger. Returns the HCI status byte (0 = success), or negative on transport
// failure / timeout. On Command Complete, up to *rlen return-parameter bytes
// (after the status byte) are copied to resp.
static int bt_cmd_sync(uint16_t opcode, const uint8_t *params, int plen,
                       uint8_t *resp, int *rlen, uint32_t timeout_ms) {
    if (bt_send_op(opcode, params, plen) != BT_OK) return BT_ERR_TIMEOUT;
    uint8_t e[BT_EVT_BUF_LEN];
    for (uint32_t i = 0; i < timeout_ms; i++) {
        int n = bt_evt_reap(e, sizeof(e));
        if (n >= 2) {
            uint8_t code = e[0];
            if (code == HCI_EVT_CMD_COMPLETE && n >= 6) {
                uint16_t op = (uint16_t)(e[3] | (e[4] << 8));
                if (op == opcode) {
                    uint8_t status = e[5];
                    if (resp && rlen) {
                        int avail = n - 6;
                        if (avail < 0) avail = 0;
                        if (avail > *rlen) avail = *rlen;
                        if (avail > 0) memcpy(resp, e + 6, avail);
                        *rlen = avail;
                    }
                    return status;
                }
            } else if (code == HCI_EVT_CMD_STATUS && n >= 6) {
                uint16_t op = (uint16_t)(e[4] | (e[5] << 8));
                if (op == opcode) return e[2];
            }
            bt_log_scan_event(e, n);
        }
        xhci_delay_ms(1);
    }
    return BT_ERR_TIMEOUT;
}

// -----------------------------------------------------------------------------
// Realtek RTL8761B/BU firmware download (BT-IMPLEMENTATION-NOTES.md section 3).
// The RTL8761BU ROM bootloader answers vendor commands but will NOT do real HCI
// until a firmware patch + config blob are downloaded via vendor opcode 0xFC20
// in 252-byte fragments. Firmware blobs are staged on the FAT disk as
// /FW/RTLBTFW.BIN and /FW/RTLBTCFG.BIN (linux-firmware rtl_bt/*).
// Runs on the bt worker thread, so the FAT filesystem is mounted.
// -----------------------------------------------------------------------------
#define RTL_READ_ROM_VERSION   0xFC6D
#define RTL_DOWNLOAD_FW        0xFC20
#define RTL_FRAG_LEN           252
#define HCI_CMD_READ_LOCAL_VER 0x1001

// Read the ROM/eco version (selects which patch in the fw file applies).
static int rtl_read_rom_version(uint8_t *out_ver) {
    uint8_t resp[4]; int rlen = sizeof(resp);
    int st = bt_cmd_sync(RTL_READ_ROM_VERSION, NULL, 0, resp, &rlen, 1500);
    if (st != 0 || rlen < 1) return -1;
    *out_ver = resp[0];
    return 0;
}

// Returns 0 on success (controller patched + rebooted into firmware), <0 else.
static int rtl_load_firmware(void) {
    // Best-effort initial reset (ROM bootloader accepts it; ignore failure).
    (void)bt_cmd_sync(HCI_CMD_RESET, NULL, 0, NULL, NULL, 1500);

    // Log the local version for identification (RTL8761B: lmp_subver ~0x8761).
    uint8_t lv[8]; int lvl = sizeof(lv);
    if (bt_cmd_sync(HCI_CMD_READ_LOCAL_VER, NULL, 0, lv, &lvl, 1500) == 0 && lvl >= 8) {
        uint16_t hci_rev  = (uint16_t)(lv[1] | (lv[2] << 8));
        uint16_t lmp_sub  = (uint16_t)(lv[6] | (lv[7] << 8));
        bootlog_write("[BT-RTL] local version: hci_ver=0x%02x hci_rev=0x%04x lmp_subver=0x%04x",
                      lv[0], hci_rev, lmp_sub);
    }

    uint8_t rom = 0;
    if (rtl_read_rom_version(&rom) != 0) {
        bootlog_write("[BT-RTL] Read_ROM_Version (0xFC6D) failed - not a patchable ROM?");
        return -1;
    }
    bootlog_write("[BT-RTL] ROM version = 0x%02x", rom);
    kprintf("[BT-RTL] ROM version = 0x%02x\n", rom);

    uint32_t fwsz = 0, cfgsz = 0;
    // Try the FAT root first (where the GUI reliably loads files), then a /FW
    // subdirectory. Root placement avoids any subdir-traversal quirk.
    uint8_t *fw = (uint8_t *)fat_read_file(&g_fat_fs, "/RTLBTFW.BIN", &fwsz);
    if (!fw || fwsz < 14) {
        if (fw) { kfree(fw); fw = NULL; }
        fw = (uint8_t *)fat_read_file(&g_fat_fs, "/FW/RTLBTFW.BIN", &fwsz);
    }
    if (!fw || fwsz < 14) {
        bootlog_write("[BT-RTL] firmware RTLBTFW.BIN missing or short (%u) at / or /FW", fwsz);
        if (fw) kfree(fw);
        return -1;
    }
    bootlog_write("[BT-RTL] firmware loaded: %u bytes", fwsz);
    uint8_t *cfg = (uint8_t *)fat_read_file(&g_fat_fs, "/RTLBTCFG.BIN", &cfgsz);
    if (!cfg) cfg = (uint8_t *)fat_read_file(&g_fat_fs, "/FW/RTLBTCFG.BIN", &cfgsz);

    // Parse V1 rtl_epatch header ("Realtech" signature).
    if (memcmp(fw, "Realtech", 8) != 0) {
        bootlog_write("[BT-RTL] unsupported firmware signature (want V1 'Realtech')");
        kfree(fw); if (cfg) kfree(cfg);
        return -1;
    }
    uint32_t fw_version = (uint32_t)(fw[8] | (fw[9] << 8) | (fw[10] << 16) | ((uint32_t)fw[11] << 24));
    uint16_t num = (uint16_t)(fw[12] | (fw[13] << 8));
    uint32_t chip_arr = 14;
    uint32_t plen_arr = chip_arr + (uint32_t)num * 2;
    uint32_t poff_arr = plen_arr + (uint32_t)num * 2;
    if (poff_arr + (uint32_t)num * 4 > fwsz || num == 0) {
        bootlog_write("[BT-RTL] firmware header malformed (num_patches=%u)", num);
        kfree(fw); if (cfg) kfree(cfg);
        return -1;
    }
    // Select the patch whose chip_id == rom_version + 1 (Linux btrtl rule).
    int sel = -1;
    for (int i = 0; i < num; i++) {
        uint16_t cid = (uint16_t)(fw[chip_arr + i * 2] | (fw[chip_arr + i * 2 + 1] << 8));
        if (cid == (uint16_t)(rom + 1)) { sel = i; break; }
    }
    if (sel < 0) sel = num - 1;   // fall back to last patch
    uint16_t patch_len = (uint16_t)(fw[plen_arr + sel * 2] | (fw[plen_arr + sel * 2 + 1] << 8));
    uint32_t patch_off = (uint32_t)(fw[poff_arr + sel * 4] | (fw[poff_arr + sel * 4 + 1] << 8) |
                                    (fw[poff_arr + sel * 4 + 2] << 16) | ((uint32_t)fw[poff_arr + sel * 4 + 3] << 24));
    if (patch_off + patch_len > fwsz || patch_len < 4) {
        bootlog_write("[BT-RTL] patch %d out of range (off=%u len=%u fwsz=%u)",
                      sel, patch_off, patch_len, fwsz);
        kfree(fw); if (cfg) kfree(cfg);
        return -1;
    }

    // Build the download image: patch payload (last 4 bytes := fw_version) +
    // the whole config file appended.
    uint32_t img_len = (uint32_t)patch_len + cfgsz;
    uint8_t *img = (uint8_t *)kmalloc(img_len);
    if (!img) { kfree(fw); if (cfg) kfree(cfg); return -1; }
    memcpy(img, fw + patch_off, patch_len);
    memcpy(img + patch_len - 4, &fw_version, 4);
    if (cfg && cfgsz) memcpy(img + patch_len, cfg, cfgsz);

    bootlog_write("[BT-RTL] uploading patch %d: %u fw + %u cfg = %u bytes in %u frags",
                  sel, (uint32_t)patch_len, cfgsz, img_len,
                  (img_len + RTL_FRAG_LEN - 1) / RTL_FRAG_LEN);

    // Download in <=252-byte fragments via 0xFC20. Index low 7 bits = sequence
    // (0,1,..0x7F then wraps to 0x01); the final fragment sets bit7.
    uint32_t off = 0;
    uint8_t seq = 0;
    int nfrag = 0;
    while (off < img_len) {
        uint32_t frag = img_len - off;
        if (frag > RTL_FRAG_LEN) frag = RTL_FRAG_LEN;
        int last = (off + frag >= img_len);
        uint8_t param[1 + RTL_FRAG_LEN];
        param[0] = (uint8_t)(seq | (last ? 0x80 : 0x00));
        memcpy(param + 1, img + off, frag);
        uint8_t re[4]; int rl = sizeof(re);
        int st = bt_cmd_sync(RTL_DOWNLOAD_FW, param, (int)(1 + frag), re, &rl, 2000);
        if (st != 0) {
            bootlog_write("[BT-RTL] fragment %d (seq 0x%02x) FAILED status=%d",
                          nfrag, param[0], st);
            kfree(fw); if (cfg) kfree(cfg); kfree(img);
            return -1;
        }
        off += frag;
        nfrag++;
        seq++;
        if (seq > 0x7F) seq = 1;
    }
    kfree(fw); if (cfg) kfree(cfg); kfree(img);

    // Reboot into the patched firmware.
    (void)bt_cmd_sync(HCI_CMD_RESET, NULL, 0, NULL, NULL, 2000);
    bootlog_write("[BT-RTL] firmware download complete (%d fragments); controller re-reset",
                  nfrag);
    kprintf("[BT-RTL] firmware download complete (%d fragments)\n", nfrag);
    return 0;
}

// Milestone: [RTL firmware upload] -> HCI_Reset -> Read_BD_ADDR (LOG) ->
// Classic inquiry + LE scan.
static void bt_usb_selftest(void) {
    if (!g_ctx.present) return;
    bt_chip_t chip = bt_classify(g_ctx.vid, g_ctx.pid);
    bootlog_write("[BT] bring-up: %04x:%04x chipset %s", g_ctx.vid, g_ctx.pid,
                  bt_chip_name(chip));
    kprintf("[BT] bring-up: %04x:%04x chipset %s\n", g_ctx.vid, g_ctx.pid,
            bt_chip_name(chip));

    if (chip == CHIP_REALTEK) {
        // The RTL8761BU needs its firmware before HCI works. Upload it now.
        if (rtl_load_firmware() == 0) {
            bootlog_write("[BT] Realtek firmware uploaded OK; proceeding to HCI bring-up");
        } else {
            bootlog_write("[BT] Realtek firmware upload FAILED - HCI_Reset/Read_BD_ADDR "
                          "below will likely fail (firmware is the blocker)");
        }
    } else if (chip == CHIP_INTEL || chip == CHIP_BROADCOM) {
        bootlog_write("[BT] NOTE: %s typically requires a vendor firmware upload "
                      "before HCI is functional; attempting plain HCI_Reset anyway",
                      bt_chip_name(chip));
    }

    // --- MILESTONE 1: HCI_Reset ---
    int st = bt_cmd_sync(HCI_CMD_RESET, NULL, 0, NULL, NULL, 2000);
    if (st != 0) {
        bootlog_write("[BT] HCI_Reset FAILED (status/timeout=%d). If this is a "
                      "firmware-upload chipset, that upload is the blocker.", st);
        kprintf("[BT] HCI_Reset failed (%d)\n", st);
        bt_set_state(BT_STATE_ERROR);
        return;
    }
    bootlog_write("[BT] HCI_Reset OK (Command Complete, status 0)");
    kprintf("[BT] HCI_Reset OK\n");

    // --- MILESTONE 2: Read_BD_ADDR ---
    uint8_t resp[8]; int rlen = sizeof(resp);
    st = bt_cmd_sync(HCI_CMD_READ_BD_ADDR, NULL, 0, resp, &rlen, 2000);
    if (st == 0 && rlen >= 6) {
        bt_log_bdaddr("BD_ADDR =", resp);          // <<< the transport proof
        memcpy(g_ctx.bdaddr.b, resp, 6);
        g_ctx.have_bdaddr = 1;
    } else {
        bootlog_write("[BT] Read_BD_ADDR failed (status/timeout=%d)", st);
        kprintf("[BT] Read_BD_ADDR failed (%d)\n", st);
        return;
    }

    // Transport is proven end to end (firmware upload + HCI_Reset + BD_ADDR).
    // Hand off to the HCI PROTOCOL layer (hci.c), which from here owns ALL HCI
    // traffic: the full async bring-up (reset -> BD_ADDR -> event masks -> SSP ->
    // class/name/scan -> LE host support/event mask/buffer size -> ready), then
    // scan/connect/pair/GATT/HOGP. The transport now only pumps RX (events + ACL)
    // via bt_transport_deliver_*. This is the fix for the two halves fighting
    // over the interrupt-IN event stream: exactly one consumer after this point.
    bootlog_write("[BT] transport ready; handing off to HCI protocol layer");
    kprintf("[BT] transport ready; handing off to HCI protocol layer\n");
    hci_start_bringup();
}

// -----------------------------------------------------------------------------
// Contract registration entry (bt_transport.h). Marks the controller bound and
// registers the transport ops. Called by bt_usb_probe() once the endpoints are
// claimed. `controller` is the xhci_controller_t*; the endpoint arguments are
// the xHCI DCIs from xhci_configure_endpoint_ep().
// -----------------------------------------------------------------------------
int bt_hci_usb_probe(void *controller, uint8_t slot_id,
                     uint8_t ep_event_in, uint8_t ep_acl_in, uint8_t ep_acl_out) {
    g_ctx.present     = 1;
    g_ctx.xhc         = (xhci_controller_t *)controller;
    g_ctx.slot_id     = slot_id;
    g_ctx.evt_dci     = ep_event_in;
    g_ctx.acl_in_dci  = (ep_acl_in  == 0xFF) ? -1 : ep_acl_in;
    g_ctx.acl_out_dci = (ep_acl_out == 0xFF) ? -1 : ep_acl_out;
    kprintf("[BT-USB] controller bound: slot %u (evt-in dci %u, acl-in dci %u, acl-out dci %u)\n",
            slot_id, ep_event_in, ep_acl_in, ep_acl_out);
    xhci_iso_quiet = 1;   // quiet per-completion serial spam once events flow
    bt_evt_submit();      // arm the first interrupt-IN
    return bt_transport_register(&g_usb_transport_ops);
}

// -----------------------------------------------------------------------------
// Config-descriptor parsing: find the primary HCI controller interface and its
// interrupt-IN + bulk IN/OUT endpoints. Per the USB BT transport spec the
// primary interface is class 0xE0 / subclass 0x01 / protocol 0x01, alt 0; some
// dongles report the class only at the device level, so fall back to interface
// 0's endpoint mix. Do NOT hardcode endpoint addresses - read them.
// -----------------------------------------------------------------------------
static int bt_parse_endpoints(uint8_t *cfg, int total, int *out_cfgval,
                              int *evt_ep, int *evt_mps,
                              int *acl_in, int *acl_in_mps,
                              int *acl_out, int *acl_out_mps) {
    *evt_ep = *acl_in = *acl_out = -1;
    *evt_mps = *acl_in_mps = *acl_out_mps = 0;
    *out_cfgval = (total >= 6) ? cfg[5] : 1;

    int i = 0, cur_if = -1, cur_alt = -1, want_if = -1;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9) {           // INTERFACE
            cur_if  = cfg[i + 2];
            cur_alt = cfg[i + 3];
            int cclass = cfg[i + 5], csub = cfg[i + 6], cproto = cfg[i + 7];
            if (cur_alt == 0 && cclass == 0xE0 && csub == 0x01 &&
                cproto == 0x01 && want_if < 0) {
                want_if = cur_if;
            }
        } else if (btype == 0x05 && blen >= 7) {     // ENDPOINT
            int eaddr = cfg[i + 2];
            int eattr = cfg[i + 3] & 0x03;
            int emps  = cfg[i + 4] | (cfg[i + 5] << 8);
            int match_if = (want_if >= 0) ? (cur_if == want_if)
                                          : (cur_if == 0 && cur_alt == 0);
            if (match_if) {
                if (eattr == 0x03 && (eaddr & 0x80)) {          // interrupt-IN
                    if (*evt_ep < 0) { *evt_ep = eaddr; *evt_mps = emps; }
                } else if (eattr == 0x02) {                     // bulk
                    if (eaddr & 0x80) { if (*acl_in < 0)  { *acl_in = eaddr;  *acl_in_mps = emps; } }
                    else              { if (*acl_out < 0) { *acl_out = eaddr; *acl_out_mps = emps; } }
                }
            }
        }
        i += blen;
    }
    return (*evt_ep >= 0) ? 0 : -1;   // interrupt-IN is mandatory
}

// -----------------------------------------------------------------------------
// Enumeration entry: called from drivers/xhci.c's xhci_probe_device() after the
// device + config descriptors are read. Matches a USB Bluetooth dongle, claims
// its endpoints, and registers the transport. Returns 1 if claimed. Registering
// is unconditional (independent of g_bt_enable) so the transport is present
// when the user later enables Bluetooth (bt_power(1) -> bt_init); no HCI traffic
// is sent here, so boot is unaffected. The self-test runs later, gated behind
// g_bt_enable, from hci_usb_poll() on the bt worker thread.
// -----------------------------------------------------------------------------
int bt_usb_probe(xhci_controller_t *xhc, int slot_id, int speed,
                 uint16_t vid, uint16_t pid, uint8_t dev_class,
                 uint8_t dev_subclass, uint8_t dev_proto,
                 uint8_t *cfg, int total) {
    if (g_ctx.present) return 0;   // one dongle supported

    // Match: device-level wireless/BT class, OR a wireless/RF/BT interface.
    int is_bt = (dev_class == 0xE0 && dev_subclass == 0x01 && dev_proto == 0x01);
    if (!is_bt) {
        int i = 0;
        while (i + 2 <= total) {
            int blen = cfg[i], btype = cfg[i + 1];
            if (blen < 2 || i + blen > total) break;
            if (btype == 0x04 && blen >= 9 &&
                cfg[i + 5] == 0xE0 && cfg[i + 6] == 0x01 && cfg[i + 7] == 0x01) {
                is_bt = 1; break;
            }
            i += blen;
        }
    }
    if (!is_bt) return 0;

    int cfgval, evt_ep, evt_mps, acl_in, acl_in_mps, acl_out, acl_out_mps;
    if (bt_parse_endpoints(cfg, total, &cfgval, &evt_ep, &evt_mps,
                           &acl_in, &acl_in_mps, &acl_out, &acl_out_mps) != 0) {
        kprintf("[BT] %04x:%04x looks like BT but has no interrupt-IN endpoint\n",
                vid, pid);
        bootlog_write("[BT] %04x:%04x: no interrupt-IN endpoint, not claiming", vid, pid);
        return 0;
    }

    kprintf("[BT] dongle %04x:%04x (%s): evt-in 0x%02x/%d bulk-in 0x%02x/%d bulk-out 0x%02x/%d cfg %d\n",
            vid, pid, bt_chip_name(bt_classify(vid, pid)),
            evt_ep, evt_mps, acl_in, acl_in_mps, acl_out, acl_out_mps, cfgval);
    bootlog_write("[BT] dongle %04x:%04x detected (%s): evt-in 0x%02x bulk-in 0x%02x bulk-out 0x%02x",
                  vid, pid, bt_chip_name(bt_classify(vid, pid)), evt_ep, acl_in, acl_out);

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.speed = speed;
    g_ctx.vid = vid;
    g_ctx.pid = pid;
    g_ctx.evt_mps = evt_mps;
    g_ctx.acl_in_mps = acl_in_mps;
    g_ctx.acl_out_mps = acl_out_mps;

    if (xhci_set_configuration(xhc, slot_id, (uint8_t)cfgval) < 0) {
        kprintf("[BT] SET_CONFIGURATION %d failed\n", cfgval);
        bootlog_write("[BT] SET_CONFIGURATION %d failed", cfgval);
        return 0;
    }

    int evt_dci = xhci_configure_endpoint_ep(xhc, slot_id, evt_ep,
                                             EP_TYPE_INTERRUPT_IN, evt_mps, 1, speed);
    if (evt_dci < 0) {
        kprintf("[BT] interrupt-IN endpoint config failed\n");
        bootlog_write("[BT] interrupt-IN endpoint config failed");
        return 0;
    }

    int acl_in_dci = -1, acl_out_dci = -1;
    if (acl_in >= 0)
        acl_in_dci = xhci_configure_endpoint_ep(xhc, slot_id, acl_in,
                                                EP_TYPE_BULK_IN, acl_in_mps, 0, speed);
    if (acl_out >= 0)
        acl_out_dci = xhci_configure_endpoint_ep(xhc, slot_id, acl_out,
                                                 EP_TYPE_BULK_OUT, acl_out_mps, 0, speed);

    // DMA buffers (identity-mapped pages).
    uint64_t p_evt  = pmm_alloc_pages(1);
    uint64_t p_cmd  = pmm_alloc_pages(1);
    uint64_t p_ain  = pmm_alloc_pages(1);
    uint64_t p_aout = pmm_alloc_pages(1);
    if (!p_evt || !p_cmd || !p_ain || !p_aout) {
        kprintf("[BT] DMA buffer allocation failed\n");
        bootlog_write("[BT] DMA buffer allocation failed");
        return 0;
    }
    g_ctx.evt_buf     = (uint8_t *)p_evt;
    g_ctx.cmd_buf     = (uint8_t *)p_cmd;
    g_ctx.acl_in_buf  = (uint8_t *)p_ain;
    g_ctx.acl_out_buf = (uint8_t *)p_aout;

    // Register the transport (marks g_ctx.present, arms interrupt-IN).
    bt_hci_usb_probe(xhc, (uint8_t)slot_id, (uint8_t)evt_dci,
                     (acl_in_dci  < 0) ? 0xFF : (uint8_t)acl_in_dci,
                     (acl_out_dci < 0) ? 0xFF : (uint8_t)acl_out_dci);

    kprintf("[BT] USB HCI transport up (slot %d)\n", slot_id);
    bootlog_write("[BT] USB HCI transport up (slot %d), Bluetooth %s",
                  slot_id, g_bt_enable ? "enabled (self-test will run)"
                                       : "disabled (enable to bring up)");
    return 1;
}

// -----------------------------------------------------------------------------
// Bluetooth worker thread (INTEGRATION.md phase 1). Pumps bt_poll(), which fans
// out to transport->poll (our hci_usb_poll, incl. the one-shot self-test) and
// hci_poll(). Only does work while g_bt_enable and the stack is not OFF.
// Started from main.c right after net/heartbeat workers.
// -----------------------------------------------------------------------------
static void bt_worker(void *arg) {
    (void)arg;
    // Post-boot enable decision (FAT is mounted here, unlike the boot path under
    // the #375 to-RAM root). Replaces the old forced g_bt_enable=1 in main.c.
    extern int bt_autostart(void);
    bt_autostart();

    extern int  bt_scan_start(void);
    extern void bt_debug_scan_summary(void);
    extern int  bt_debug_try_connect(void);

    int scan_kicked = 0, ticks = 0, connect_tried = 0;
    for (;;) {
        bt_poll();
        // Once the controller finishes bring-up, start an LE scan so HOGP
        // keyboards/mice are discovered and auto-connected (gatt HID target).
        if (!scan_kicked && g_bt_enable && hci_is_ready()) {
            bt_scan_start();
            scan_kicked = 1;
            ticks = 0;
            kprintf("[BT] controller ready; LE scan started for HID devices\n");
        }
        if (scan_kicked) {
            ticks++;
            if (ticks % 60 == 0 && ticks <= 360) bt_debug_scan_summary();  // ~3s cadence
            // ~10s in: if no HID device appeared, exercise the connect/ATT path.
            if (!connect_tried && ticks == 200) { connect_tried = 1; bt_debug_try_connect(); }
        }
        proc_sleep(50);
    }
}

void bt_start_worker(void) {
    int pid = proc_create("btmon", bt_worker, NULL, PRIO_NORMAL);
    kprintf("[BT] background bt worker started, pid=%d\n", pid);
}
