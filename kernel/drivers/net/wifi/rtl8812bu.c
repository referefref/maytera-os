// drivers/net/wifi/rtl8812bu.c - Realtek RTL8812BU / RTL88x2BU USB WiFi
// foundation driver (#383, PHASE 1).
//
// USB id 0bda:b812 is an RTL8822B-family chip (lsusb: "RTL88x2bu [AC1200]"),
// NOT the RTL8812AU. It is a vendor-class (0xFF) USB device with bulk IN/OUT
// endpoints. This file ports the phase-1 bring-up from the open-source Realtek
// "halmac" driver (github.com/morrownr/88x2bu-20210702, GPL, used for
// register-level facts only):
//
//   (a) bind 0bda:b812 on USB enumeration
//   (b) read + verify the chip version register (REG_SYS_CFG1, 0x00F0)
//   (c) MAC power-on sequence (halmac card-enable pwr-seq, USB rows)
//   (d) upload firmware from /FIRMWARE/RTL8812BU.BIN via the 8051 DDMA FIFO
//       download protocol, then poll REG_MCUFW_CTRL (0x80) for fw-ready 0xC078
//
// Every step logs to serial + the persistent boot log so a single VM boot
// shows exactly how far bring-up reached. See PLAN.md for the full phased plan.
//
// NOTE (honesty): QEMU does not emulate this chip; the dev loop passes the real
// dongle through with `-device usb-host,vendorid=0x0bda,productid=0xb812`, so
// register reads/writes hit the real silicon. The firmware DDMA/reserved-page
// TX path is the highest-risk part and may need hardware iteration; it is fully
// structured and logged so the stopping point is explicit.

#include "wifi.h"
#include "../../xhci.h"
#include "../../../serial.h"
#include "../../../string.h"
#include "../../../fs/bootlog.h"
#include "../../../fs/fat.h"
#include "../../../mm/heap.h"
#include "../../../proc/process.h"   // proc_create / proc_sleep (phase-3 scan worker)
#include "rtl8822b_tables.h"    // BB/AGC/RF/MAC register arrays (phase 2)

extern fat_fs_t g_fat_fs;

#define RTL_VID            0x0BDA
#define RTL_PID            0xB812
#define RTW_VENDOR_REQ     0x05      // REALTEK_USB_VENQT_CMD_REQ
#define RTW_REQTYPE_READ   0xC0      // REALTEK_USB_VENQT_READ
#define RTW_REQTYPE_WRITE  0x40      // REALTEK_USB_VENQT_WRITE

#ifndef BIT
#define BIT(n) (1u << (n))
#endif

// --- MAC registers (halmac_reg2.h, RTL8822B) ---
#define REG_SYS_FUNC_EN     0x0002
#define REG_SYS_CLK_CTRL    0x0008
#define REG_RSV_CTRL        0x001C
#define REG_MCUFW_CTRL      0x0080   // "0x80": FWDL_EN(b0), IMEM/DMEM ok, FW_DW_RDY(b14)
#define REG_SYS_CFG1        0x00F0   // chip version / RF type / cut
#define REG_SYS_CFG2        0x00FC
#define REG_CR              0x0100
#define REG_MACID           0x0610   // 6-byte station MAC address (post efuse-read)
#define REG_TRXFF_BNDY      0x0114   // TRX FIFO page boundary (phase-2 mac_init)
#define REG_TXDMA_PQ_MAP    0x010C
#define REG_FIFOPAGE_CTRL_2 0x0204
#define REG_TXDMA_STATUS    0x0210
#define REG_RQPN_CTRL_2     0x022C
#define REG_FIFOPAGE_INFO_1 0x0230
#define REG_FWHW_TXQ_CTRL   0x0420
#define REG_BCN_CTRL        0x0550
#define REG_CPU_DMEM_CON    0x1080
#define REG_FW_DBG6         0x10F8
#define REG_FW_DBG7         0x10FC
#define REG_DDMA_CH0SA      0x1200
#define REG_DDMA_CH0DA      0x1204
#define REG_DDMA_CH0CTRL    0x1208
#define REG_H2CQ_CSR        0x1330

// --- bits ---
#define BIT_FW_DW_RDY               BIT(14)
#define BIT_IMEM_DW_OK              BIT(3)
#define BIT_IMEM_CHKSUM_OK          BIT(4)
#define BIT_DMEM_DW_OK              BIT(5)
#define BIT_DMEM_CHKSUM_OK          BIT(6)
#define BIT_HCI_TXDMA_EN            BIT(0)
#define BIT_TXDMA_EN                BIT(2)
#define BIT_DDMACH0_OWN             BIT(31)
#define BIT_DDMACH0_CHKSUM_EN       BIT(29)
#define BIT_DDMACH0_CHKSUM_STS      BIT(27)
#define BIT_DDMACH0_RESET_CHKSUM_STS BIT(25)
#define BIT_DDMACH0_CHKSUM_CONT     BIT(24)
#define BIT_MASK_DDMACH0_DLEN       0x3ffff
#define BIT_RTL_ID_8822B            BIT(23)
#define BIT_RF_TYPE_ID_8822B        BIT(27)
#define BIT_MASK_BCN_HEAD           0x0FFF

// --- OCP base + FW header layout (halmac_88xx_cfg.h / halmac_fw_info.h) ---
#define OCPBASE_TXBUF       0x18780000u
#define OCPBASE_DMEM        0x00200000u
#define TX_DESC_SIZE        48
#define WLAN_FW_HDR_SIZE            64
#define WLAN_FW_HDR_CHKSUM_SIZE     8
#define WLAN_FW_HDR_MEM_USAGE       24
#define WLAN_FW_HDR_DMEM_ADDR       32
#define WLAN_FW_HDR_DMEM_SIZE       36
#define WLAN_FW_HDR_IMEM_SIZE       48
#define WLAN_FW_HDR_EMEM_SIZE       52
#define WLAN_FW_HDR_EMEM_ADDR       56
#define WLAN_FW_HDR_IMEM_ADDR       60
#define FW_SIGNATURE_8822B          0x8822
#define DLFW_CHUNK                  2048    // rsvd-page chunk (<= DLFW_RSVDPG_SIZE)
#define DDMA_POLL_CNT               20000

// --- Phase-2 MAC-init registers (halmac_reg2.h, RTL8822B) ---
#define REG_WMAC_FWPKT_CR   0x0601
#define REG_FWFF_PKT_INFO   0x02A0
#define REG_FWFF_CTRL       0x029C
#define REG_RX_DRVINFO_SZ   0x060F
#define REG_FIFOPAGE_INFO_2 0x0234
#define REG_FIFOPAGE_INFO_3 0x0238
#define REG_FIFOPAGE_INFO_4 0x023C
#define REG_FIFOPAGE_INFO_5 0x0240
#define REG_BCNQ_BDNY_V1    0x0424
#define REG_BCNQ1_BDNY_V1   0x0456
#define REG_RXFF_BNDY       0x011C
#define REG_AUTO_LLT_V1     0x0208
#define REG_TXDMA_OFFSET_CHK 0x020C
#define REG_H2C_HEAD        0x0244
#define REG_H2C_TAIL        0x0248
#define REG_H2C_READ_ADDR   0x024C
#define REG_H2C_INFO        0x0254
#define REG_SW_AMPDU_BURST_MODE_CTRL 0x04BC
#define REG_AMPDU_MAX_TIME_V1 0x0455
#define REG_TX_HANG_CTRL    0x045E
#define REG_PROT_MODE_CTRL  0x04C8
#define REG_BAR_MODE_CTRL   0x04CC
#define REG_FAST_EDCA_VOVI_SETTING 0x1448
#define REG_FAST_EDCA_BEBK_SETTING 0x144C
#define REG_INIRTS_RATE_SEL 0x0480
#define REG_TIMER0_SRC_SEL  0x05B4
#define REG_TXPAUSE         0x0522
#define REG_SLOT            0x051B
#define REG_PIFS            0x0512
#define REG_SIFS            0x0514
#define REG_EDCA_VO_PARAM   0x0500
#define REG_EDCA_VI_PARAM   0x0504
#define REG_RD_NAV_NXT      0x0544
#define REG_RXTSF_OFFSET_CCK 0x055E
#define REG_TBTT_PROHIBIT   0x0540
#define REG_DRVERLYINT      0x0558
#define REG_BCNDMATIM       0x0559
#define REG_TX_PTCL_CTRL    0x0520
#define REG_RXFLTMAP0       0x06A0
#define REG_RXFLTMAP2       0x06A4
#define REG_RCR             0x0608
#define REG_RX_PKT_LIMIT    0x060C
#define REG_TCR             0x0604
#define REG_WMAC_TRXPTCL_CTL 0x0668
#define REG_SND_PTCL_CTRL   0x0718
#define REG_WMAC_OPTION_FUNCTION 0x07D0
#define REG_GPIO_MUXCFG     0x0040
#define REG_EFUSE_CTRL      0x0030

// --- Phase-4 registers (MLME: join a BSS) ---
#define REG_BSSID           0x0618   // 6-byte BSSID of the joined BSS (hal_com_reg)
#define REG_MSR             0x0102   // Media Status (REG_CR+2): port0 net-type bits[1:0]
#define MSR_NOLINK          0x00     // _HW_STATE_NOLINK_
#define MSR_ADHOC           0x01     // _HW_STATE_ADHOC_
#define MSR_INFRA           0x02     // _HW_STATE_STATION_ (associated STA)
// 802.11 management-frame subtypes (type=0)
#define ST_ASSOC_REQ        0x00
#define ST_ASSOC_RESP       0x01
#define ST_AUTH             0x0B

// --- Phase-3 registers (RF power-up + channel + RX) ---
#define REG_RF_CTRL         0x001F   // RF enable: BIT0 EN, BIT1 RSTB, BIT2 SDM-RSTB
#define REG_WLRF1           0x00EC   // 32-bit: BIT24/25/26 enable RF 3-wire (read/write)
#define REG_CCK_CHECK       0x0454   // BIT7: 0=2.4G, 1=5G (halmac cfg_ch)
#define REG_DATA_SC         0x0483   // primary sub-channel index (halmac cfg_pri_ch)
#define REG_RXDMA_MODE      0x0290   // USB RX DMA mode (halmac init_usb_cfg)
#define REG_USB_USBSTAT     0xFE11   // USB speed status (HS/FS)

// --- MAC-init derived constants (halmac_init_8822b.c) ---
#define MAC_TRX_ENABLE      0xFF    // HCI_TXDMA|HCI_RXDMA|TXDMA|RXDMA|PROTOCOL|SCHEDULE|MACTX|MACRX
#define TX_FIFO_SIZE_8822B  262144
#define RX_FIFO_SIZE_8822B  24576
#define TX_PAGE_SIZE_SHIFT  7       // 128 = 2^7
#define C2H_PKT_BUF         256
#define BLK_DESC_NUM        0x3
#define RX_DESC_DUMMY_SIZE  72
// reserved-page allocation (pages)
#define RSVD_PG_DRV_NUM         16
#define RSVD_PG_H2C_EXTRAINFO   24
#define RSVD_PG_H2C_STATICINFO  8
#define RSVD_PG_H2CQ_NUM        8
#define RSVD_PG_CPU_INSTR       0
#define RSVD_PG_FW_TXBUF        4
#define RSVD_PG_CSIBUF          0
// WLAN protocol/EDCA/WMAC literal configs (halmac_init_8822b.c)
#define WLAN_SLOT_TIME      0x09
#define WLAN_PIFS_TIME      0x19
#define WLAN_SIFS_CFG       0x10100E0A
#define WLAN_VO_TXOP_LIMIT  0x0186
#define WLAN_VI_TXOP_LIMIT  0x03BC
#define WLAN_NAV_CFG        0x001B0005
#define WLAN_RX_TSF_CFG     0x00003030
#define WLAN_TBTT_TIME      0x00006404
#define WLAN_DRV_EARLY_INT  0x04
#define WLAN_BCN_DMA_TIME   0x02
#define WLAN_RX_FILTER0     0x0FFFFFFF
#define WLAN_RX_FILTER2     0xFFFF
#define WLAN_RCR_CFG        0xE400220E
#define WLAN_RXPKT_MAX_SZ_512  24     // 12288 >> 9
#define WLAN_AMPDU_MAX_TIME 0x70
#define WLAN_PROT_MODE_CFG  0x202008FF
#define WLAN_BAR_CFG        0x0801
#define WLAN_FAST_EDCA_TH   0x06
#define WLAN_TX_FUNC_CFG1   0x30
#define WLAN_TX_FUNC_CFG2   0x30
#define WLAN_MAC_OPT_NORM_FUNC1 0x98
#define WLAN_MAC_OPT_FUNC2  0x30810041
#define BIT_EN_BCN_FUNCTION BIT(3)
#define BIT_FWEN            BIT(7)
#define BIT_AUTO_INIT_LLT   BIT(0)
#define BIT_R_DIS_CHK_VHTSIGB_CRC BIT(6)
#define BIT_EN_EOF_V1       BIT(2)
// efuse (halmac_efuse_88xx.c)
#define BIT_EF_FLAG         BIT(31)
#define BIT_MASK_EF_DATA    0xFF
#define BIT_SHIFT_EF_ADDR   8
#define BIT_MASK_EF_ADDR    0x3FF
#define EFUSE_SIZE_8822B    1024
#define EEPROM_SIZE_8822B   768
#define PRTCT_EFUSE_SIZE    96
#define EEPROM_MAC_ADDR_8822BU 0x107
#define EEPROM_RFE_OPTION_8822B 0x00CA
// phydm table-condition params (phydm_types.h)
#define ODM_ITRF_USB        0x02
#define ODM_CE_PLATFORM     0x04
#define COND_ELSE           2
#define COND_ENDIF          3
#define MASKDWORD           0xFFFFFFFFu
#define RFREGOFFSETMASK     0xFFFFFu

typedef struct {
    xhci_controller_t *xhc;
    int      slot_id;
    int      speed;
    int      ep_in;      // bulk IN endpoint address
    int      ep_out;     // bulk OUT endpoint address = HIGH/BCN queue (RtOutPipe[0])
    int      ep_out_list[4];  // all bulk-OUT endpoints in descriptor order
    int      ep_out_num;      // number of bulk-OUT endpoints
    int      in_mps, out_mps;
    uint8_t  cut;        // chip cut (0=A,1=B,2=C...)
    uint8_t  is_test;
    uint8_t  rf_2t2r;
    uint8_t  present_ready;   // adapter bound + chip verified; fw upload pending
    uint8_t  scan_ready;      // RF powered up; scan worker may be spawned (phase 3)
    uint8_t  mac[6];          // station MAC (from efuse, phase 2)
    uint8_t  mac_valid;
    uint8_t  rfe_type;        // RF-frontend type from efuse (table conditionals)
    wifi_dev_t *wifi;
} rtl_dev_t;

static rtl_dev_t g_rtl;

// DMA scratch (identity-mapped statics; phys==virt in the kernel).
static uint8_t vbuf[8]  __attribute__((aligned(64)));       // reg r/w
static uint8_t txbuf[DLFW_CHUNK + TX_DESC_SIZE + 8] __attribute__((aligned(64)));

// =============================================================================
// USB vendor-request register access (Realtek: bReq 0x05, wValue = addr)
// =============================================================================
static int rtl_reg_read(rtl_dev_t *d, uint16_t addr, void *out, uint16_t len) {
    if (len > sizeof(vbuf)) return -1;
    memset(vbuf, 0, len);
    int cc = xhci_control_transfer(d->xhc, d->slot_id, RTW_REQTYPE_READ,
                                   RTW_VENDOR_REQ, addr, 0, vbuf, len);
    if (cc != CC_SUCCESS && cc != CC_SHORT_PACKET) return -1;
    memcpy(out, vbuf, len);
    return 0;
}

static int rtl_reg_write(rtl_dev_t *d, uint16_t addr, const void *in, uint16_t len) {
    if (len > sizeof(vbuf)) return -1;
    memcpy(vbuf, in, len);
    int cc = xhci_control_transfer(d->xhc, d->slot_id, RTW_REQTYPE_WRITE,
                                   RTW_VENDOR_REQ, addr, 0, vbuf, len);
    return (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) ? 0 : -1;
}

static uint8_t rtl_r8(rtl_dev_t *d, uint16_t a)  { uint8_t v = 0;  rtl_reg_read(d, a, &v, 1); return v; }
static uint16_t rtl_r16(rtl_dev_t *d, uint16_t a){ uint16_t v = 0; rtl_reg_read(d, a, &v, 2); return v; }
static uint32_t rtl_r32(rtl_dev_t *d, uint16_t a){ uint32_t v = 0; rtl_reg_read(d, a, &v, 4); return v; }
static void rtl_w8(rtl_dev_t *d, uint16_t a, uint8_t v)  { rtl_reg_write(d, a, &v, 1); }
static void rtl_w16(rtl_dev_t *d, uint16_t a, uint16_t v){ rtl_reg_write(d, a, &v, 2); }
static void rtl_w32(rtl_dev_t *d, uint16_t a, uint32_t v){ rtl_reg_write(d, a, &v, 4); }

// =============================================================================
// (b) Chip version read + verify  (halmac read_chip_version, RTL8822B)
// =============================================================================
static int rtl_read_chip_version(rtl_dev_t *d) {
    uint32_t cfg1 = rtl_r32(d, REG_SYS_CFG1);
    uint32_t cfg2 = rtl_r32(d, REG_SYS_CFG2);
    if (cfg1 == 0xFFFFFFFFu || cfg1 == 0) {
        kprintf("[RTL8812BU] chip-version read implausible (0x%08x): control xfer not reaching silicon\n", cfg1);
        bootlog_write("[RTL8812BU] CHIP VER FAIL cfg1=0x%08x (no reg access)", cfg1);
        return -1;
    }
    d->is_test = (cfg1 & BIT_RTL_ID_8822B) ? 1 : 0;
    d->cut     = (uint8_t)((cfg1 >> 12) & 0xF);
    d->rf_2t2r = (cfg1 & BIT_RF_TYPE_ID_8822B) ? 1 : 0;
    char cutc = (char)('A' + d->cut);
    kprintf("[RTL8812BU] chip=RTL8822B SYS_CFG1=0x%08x SYS_CFG2=0x%08x cut=%c(%u) %s %s\n",
            cfg1, cfg2, cutc, d->cut, d->is_test ? "TEST" : "NORMAL",
            d->rf_2t2r ? "2T2R" : "1T1R");
    bootlog_write("[RTL8812BU] CHIP VER OK: RTL8822B cut=%c %s %s (cfg1=0x%08x)",
                  cutc, d->is_test ? "TEST" : "NORMAL", d->rf_2t2r ? "2T2R" : "1T1R", cfg1);
    return 0;
}

// =============================================================================
// (c) MAC power-on: halmac card-enable power sequence (RTL8822B, USB rows).
//     Ported from halmac_pwr_seq_8822b.c (TRANS_CARDDIS_TO_CARDEMU +
//     TRANS_CARDEMU_TO_ACT) filtered to USB/ALL interface rows. Parser matches
//     pwr_sub_seq_parser_88xx: WRITE = read-modify-write byte; POLL = wait for
//     (r8(off)&msk)==val; DELAY = milliseconds in `off`.
// =============================================================================
enum { PC_WRITE, PC_POLL, PC_DELAY, PC_END };
typedef struct { uint16_t off; uint8_t cmd; uint8_t msk; uint8_t val; uint8_t cut_c_only; } pwr_cmd_t;

static const pwr_cmd_t pwr_on_seq[] = {
    // ---- CARDDIS -> CARDEMU (USB) ----
    {0x004A, PC_WRITE, BIT(0), 0, 0},
    {0x0005, PC_WRITE, BIT(3)|BIT(4)|BIT(7), 0, 0},
    // ---- CARDEMU -> ACT (USB / ALL) ----
    {0xFF0A, PC_WRITE, 0xFF, 0, 0},
    {0xFF0B, PC_WRITE, 0xFF, 0, 0},
    {0x0012, PC_WRITE, BIT(1), 0, 0},
    {0x0012, PC_WRITE, BIT(0), BIT(0), 0},
    {0x0020, PC_WRITE, BIT(0), BIT(0), 0},
    {0x0001, PC_DELAY, 1, 0, 0},              // delay 1 ms
    {0x0000, PC_WRITE, BIT(5), 0, 0},
    {0x0005, PC_WRITE, BIT(4)|BIT(3)|BIT(2), 0, 0},
    {0x0006, PC_POLL,  BIT(1), BIT(1), 0},
    {0xFF1A, PC_WRITE, 0xFF, 0, 0},
    {0x0006, PC_WRITE, BIT(0), BIT(0), 0},
    {0x0005, PC_WRITE, BIT(7), 0, 0},
    {0x0005, PC_WRITE, BIT(4)|BIT(3), 0, 0},
    {0x10C3, PC_WRITE, BIT(0), BIT(0), 0},
    {0x0005, PC_WRITE, BIT(0), BIT(0), 0},
    {0x0005, PC_POLL,  BIT(0), 0, 0},
    {0x0020, PC_WRITE, BIT(3), BIT(3), 0},
    {0x10A8, PC_WRITE, 0xFF, 0x00, 1},
    {0x10A9, PC_WRITE, 0xFF, 0xEF, 1},
    {0x10AA, PC_WRITE, 0xFF, 0x0C, 1},
    {0x0029, PC_WRITE, 0xFF, 0xF9, 0},
    {0x0024, PC_WRITE, BIT(2), 0, 0},
    {0x00AF, PC_WRITE, BIT(5), BIT(5), 0},
    {0xFFFF, PC_END,   0, 0, 0},
};

static int rtl_run_pwr_seq(rtl_dev_t *d, const pwr_cmd_t *seq) {
    for (int i = 0; ; i++) {
        const pwr_cmd_t *c = &seq[i];
        if (c->cmd == PC_END) break;
        if (c->cut_c_only && d->cut != 2 /*C*/) continue;
        switch (c->cmd) {
        case PC_WRITE: {
            uint8_t v = rtl_r8(d, c->off);
            v = (uint8_t)((v & ~c->msk) | (c->val & c->msk));
            rtl_w8(d, c->off, v);
            break;
        }
        case PC_POLL: {
            int ok = 0;
            for (int t = 0; t < 2000; t++) {
                uint8_t v = (uint8_t)(rtl_r8(d, c->off) & c->msk);
                if (v == (uint8_t)(c->val & c->msk)) { ok = 1; break; }
                xhci_delay_ms(1);
            }
            if (!ok) {
                kprintf("[RTL8812BU] pwr-seq POLL timeout off=0x%04x msk=0x%02x val=0x%02x\n",
                        c->off, c->msk, c->val);
                bootlog_write("[RTL8812BU] PWR-ON poll timeout off=0x%04x", c->off);
                return -1;
            }
            break;
        }
        case PC_DELAY:
            xhci_delay_ms(c->off ? c->off : 1);
            break;
        }
    }
    return 0;
}

static int rtl_power_on(rtl_dev_t *d) {
    kprintf("[RTL8812BU] MAC power-on sequence...\n");
    int r = rtl_run_pwr_seq(d, pwr_on_seq);
    uint8_t cr = rtl_r8(d, REG_CR);
    if (r == 0)
        bootlog_write("[RTL8812BU] PWR-ON done, REG_CR=0x%02x", cr);
    kprintf("[RTL8812BU] power-on %s, REG_CR=0x%02x\n", r == 0 ? "OK" : "INCOMPLETE", cr);
    return r;
}

// =============================================================================
// (d) Firmware download - 8051 DDMA FIFO protocol (halmac download_firmware_88xx)
// =============================================================================
static inline uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// halmac fill_txdesc_check_sum_8822b: the 8822B MAC validates every TX packet
// with a 16-bit descriptor checksum. Clear the checksum field (DW7[15:0] at byte
// 0x1C), XOR the sixteen 16-bit little-endian words that make up the first 32
// bytes of the descriptor, and store the result back in DW7[15:0]. WITHOUT this
// the MAC silently drops the reserved-page packet, so BCN-valid never asserts
// and the DDMA firmware download stalls on the first chunk (the phase-1 bug).
static void rtl_fill_txdesc_chksum(uint8_t *txdesc) {
    txdesc[0x1C] = 0; txdesc[0x1D] = 0;           // clear checksum field first
    uint16_t sum = 0;
    for (int i = 0; i < 16; i++)
        sum ^= (uint16_t)(txdesc[2 * i] | (txdesc[2 * i + 1] << 8));
    txdesc[0x1C] = (uint8_t)(sum & 0xFF);
    txdesc[0x1D] = (uint8_t)((sum >> 8) & 0xFF);
}

// Send one firmware chunk into the TX reserved page (page 0) via bulk-OUT,
// prefixed with a 48-byte TX descriptor, then wait for BCN-valid. This is the
// PLTFM_SEND_RSVD_PAGE + dl_rsvd_page_88xx equivalent. The QSEL=BEACON packet
// is routed by the HW/DMA map to the HIGH out-pipe (RtOutPipe[0] = ep_out).
static int rtl_send_rsvd_page(rtl_dev_t *d, const uint8_t *chunk, uint32_t size) {
    // BCN head -> page 0, enable BCN download, drop the "no beacon" bit.
    rtl_w16(d, REG_FIFOPAGE_CTRL_2, (uint16_t)((0 & BIT_MASK_BCN_HEAD) | BIT(15)));
    uint8_t cr1 = rtl_r8(d, REG_CR + 1);
    rtl_w8(d, REG_CR + 1, (uint8_t)(cr1 | BIT(0)));
    uint8_t txq = rtl_r8(d, REG_FWHW_TXQ_CTRL + 2);
    rtl_w8(d, REG_FWHW_TXQ_CTRL + 2, (uint8_t)(txq & ~BIT(6)));

    // USB: if (txdesc + data) is an exact multiple of the 512-byte bulk max
    // packet, the transfer needs a terminating short packet or the MAC cannot
    // find the packet boundary. Append one dummy byte (halmac send_fwpkt_88xx).
    uint32_t pad = (((TX_DESC_SIZE + size) & (512 - 1)) == 0) ? 1 : 0;
    uint32_t pktsize = size + pad;

    // Build TX descriptor. DW0: TXPKTSIZE[15:0] | OFFSET(=txdesc size)[23:16];
    // DW1: QSEL[12:8] = beacon queue (0x10). Then fill DW7 checksum.
    memset(txbuf, 0, TX_DESC_SIZE);
    uint32_t dw0 = (pktsize & 0xFFFF) | ((uint32_t)TX_DESC_SIZE << 16);
    uint32_t dw1 = (uint32_t)0x10 << 8;
    txbuf[0] = dw0 & 0xFF; txbuf[1] = (dw0 >> 8) & 0xFF; txbuf[2] = (dw0 >> 16) & 0xFF; txbuf[3] = (dw0 >> 24) & 0xFF;
    txbuf[4] = dw1 & 0xFF; txbuf[5] = (dw1 >> 8) & 0xFF; txbuf[6] = (dw1 >> 16) & 0xFF; txbuf[7] = (dw1 >> 24) & 0xFF;
    rtl_fill_txdesc_chksum(txbuf);
    memcpy(txbuf + TX_DESC_SIZE, chunk, size);
    if (pad) txbuf[TX_DESC_SIZE + size] = 0;

    int r = xhci_bulk_transfer(d->xhc, d->slot_id, d->ep_out, txbuf,
                               TX_DESC_SIZE + size + pad, 0);
    if (r < 0) {
        kprintf("[RTL8812BU] rsvd-page bulk-OUT ep=0x%02x FAILED r=%d len=%u\n",
                d->ep_out, r, TX_DESC_SIZE + size + pad);
        rtl_w8(d, REG_FWHW_TXQ_CTRL + 2, txq);
        rtl_w8(d, REG_CR + 1, cr1);
        return -1;
    }
    // Poll BCN-valid (REG_FIFOPAGE_CTRL_2+1 bit7).
    int ok = 0;
    for (int t = 0; t < 1000; t++) {
        if (rtl_r8(d, REG_FIFOPAGE_CTRL_2 + 1) & BIT(7)) { ok = 1; break; }
        xhci_delay_ms(1);
    }
    if (!ok)
        kprintf("[RTL8812BU] BCN-valid poll timeout: 0x205=0x%02x CR=0x%02x CR+1=0x%02x TXQ+2=0x%02x\n",
                rtl_r8(d, REG_FIFOPAGE_CTRL_2 + 1), rtl_r8(d, REG_CR),
                rtl_r8(d, REG_CR + 1), rtl_r8(d, REG_FWHW_TXQ_CTRL + 2));
    // Restore.
    rtl_w16(d, REG_FIFOPAGE_CTRL_2, (uint16_t)((0 & BIT_MASK_BCN_HEAD) | BIT(15)));
    rtl_w8(d, REG_FWHW_TXQ_CTRL + 2, txq);
    rtl_w8(d, REG_CR + 1, cr1);
    return ok ? 0 : -2;
}

// Trigger DDMA CH0: copy from TXBUF (OCP) to internal IMEM/DMEM with checksum.
static int rtl_iddma(rtl_dev_t *d, uint32_t src, uint32_t dest, uint32_t len, int first) {
    uint32_t cnt = DDMA_POLL_CNT;
    while (rtl_r32(d, REG_DDMA_CH0CTRL) & BIT_DDMACH0_OWN) { if (--cnt == 0) return -1; }
    uint32_t ctrl = BIT_DDMACH0_CHKSUM_EN | BIT_DDMACH0_OWN | (len & BIT_MASK_DDMACH0_DLEN);
    if (!first) ctrl |= BIT_DDMACH0_CHKSUM_CONT;
    rtl_w32(d, REG_DDMA_CH0SA, src);
    rtl_w32(d, REG_DDMA_CH0DA, dest);
    rtl_w32(d, REG_DDMA_CH0CTRL, ctrl);
    cnt = DDMA_POLL_CNT;
    while (rtl_r32(d, REG_DDMA_CH0CTRL) & BIT_DDMACH0_OWN) { if (--cnt == 0) return -2; }
    return 0;
}

// Download one memory image (dmem or imem) into the chip.
static int rtl_dlfw_to_mem(rtl_dev_t *d, const uint8_t *fw, uint32_t dest, uint32_t size) {
    uint32_t off = 0; int first = 1;
    rtl_w32(d, REG_DDMA_CH0CTRL, rtl_r32(d, REG_DDMA_CH0CTRL) | BIT_DDMACH0_RESET_CHKSUM_STS);
    while (size) {
        uint32_t pkt = size >= DLFW_CHUNK ? DLFW_CHUNK : size;
        if (rtl_send_rsvd_page(d, fw + off, pkt) != 0) {
            kprintf("[RTL8812BU] FWDL send rsvd page failed at off=%u\n", off);
            return -1;
        }
        if (rtl_iddma(d, OCPBASE_TXBUF + TX_DESC_SIZE, dest + off, pkt, first) != 0) {
            kprintf("[RTL8812BU] FWDL DDMA failed at off=%u\n", off);
            return -2;
        }
        first = 0; off += pkt; size -= pkt;
    }
    // Checksum status: BIT_DDMACH0_CHKSUM_STS set => error.
    if (rtl_r32(d, REG_DDMA_CH0CTRL) & BIT_DDMACH0_CHKSUM_STS) {
        kprintf("[RTL8812BU] FWDL checksum error (dest=0x%08x)\n", dest);
        return -3;
    }
    return 0;
}

static int rtl_download_firmware(rtl_dev_t *d, const uint8_t *fw, uint32_t fwsize) {
    if (fwsize < WLAN_FW_HDR_SIZE) { kprintf("[RTL8812BU] fw too small\n"); return -1; }
    uint16_t sig = (uint16_t)(fw[0] | (fw[1] << 8));
    uint32_t dmem_size = rd_le32(fw + WLAN_FW_HDR_DMEM_SIZE) + WLAN_FW_HDR_CHKSUM_SIZE;
    uint32_t imem_size = rd_le32(fw + WLAN_FW_HDR_IMEM_SIZE) + WLAN_FW_HDR_CHKSUM_SIZE;
    uint32_t dmem_addr = rd_le32(fw + WLAN_FW_HDR_DMEM_ADDR) & ~BIT(31);
    uint32_t imem_addr = rd_le32(fw + WLAN_FW_HDR_IMEM_ADDR) & ~BIT(31);
    uint32_t mem_usage = fw[WLAN_FW_HDR_MEM_USAGE];
    uint32_t emem_size = (mem_usage & BIT(4)) ? rd_le32(fw + WLAN_FW_HDR_EMEM_SIZE) + WLAN_FW_HDR_CHKSUM_SIZE : 0;
    kprintf("[RTL8812BU] fw sig=0x%04x size=%u dmem=%u@0x%08x imem=%u@0x%08x emem=%u\n",
            sig, fwsize, dmem_size, dmem_addr, imem_size, imem_addr, emem_size);
    bootlog_write("[RTL8812BU] FW parse sig=0x%04x size=%u dmem=%u imem=%u emem=%u",
                  sig, fwsize, dmem_size, imem_size, emem_size);
    if (sig != FW_SIGNATURE_8822B) {
        kprintf("[RTL8812BU] bad fw signature (want 0x8822)\n");
        return -1;
    }

    // Disable 8051, prepare TXDMA + FWDL enable (halmac download_firmware_88xx).
    // wlan_cpu_en(0): CPU off, then CPU-io interface disable.
    rtl_w8(d, REG_SYS_FUNC_EN + 1, (uint8_t)(rtl_r8(d, REG_SYS_FUNC_EN + 1) & ~BIT(2)));
    rtl_w8(d, REG_RSV_CTRL + 1, (uint8_t)(rtl_r8(d, REG_RSV_CTRL + 1) & ~BIT(0)));
    // DLFW uses only the HIQ: map the high queue to hi priority
    // (REG_TXDMA_PQ_MAP+1 = HALMAC_DMA_MAPPING_HIGH(=3) << 6).
    rtl_w8(d, REG_TXDMA_PQ_MAP + 1, (uint8_t)(3 << 6));
    rtl_w8(d, REG_CR, (uint8_t)(BIT_HCI_TXDMA_EN | BIT_TXDMA_EN));
    rtl_w32(d, REG_H2CQ_CSR, BIT(31));
    // Config HIQ + public priority queue page number; reload RQPN.
    rtl_w16(d, REG_FIFOPAGE_INFO_1, 0x0200);
    rtl_w32(d, REG_RQPN_CTRL_2, rtl_r32(d, REG_RQPN_CTRL_2) | BIT(31));
    // Disable beacon-related functions during download (BCN_CTRL: -EN_BCN +DIS).
    { uint8_t bc = rtl_r8(d, REG_BCN_CTRL);
      rtl_w8(d, REG_BCN_CTRL, (uint8_t)((bc & ~BIT(3)) | BIT(4))); }
    // pltfm reset (8822B clock-sync dance).
    rtl_w8(d, REG_CPU_DMEM_CON + 2, (uint8_t)(rtl_r8(d, REG_CPU_DMEM_CON + 2) & ~BIT(0)));
    rtl_w8(d, REG_SYS_CLK_CTRL + 1, (uint8_t)(rtl_r8(d, REG_SYS_CLK_CTRL + 1) & ~BIT(6)));
    rtl_w8(d, REG_CPU_DMEM_CON + 2, (uint8_t)(rtl_r8(d, REG_CPU_DMEM_CON + 2) | BIT(0)));
    rtl_w8(d, REG_SYS_CLK_CTRL + 1, (uint8_t)(rtl_r8(d, REG_SYS_CLK_CTRL + 1) | BIT(6)));

    // FWDL enable (MCUFW_CTRL bit0), preserve bits [13:11].
    rtl_w16(d, REG_MCUFW_CTRL, (uint16_t)((rtl_r16(d, REG_MCUFW_CTRL) & 0x3800) | BIT(0)));

    const uint8_t *cur = fw + WLAN_FW_HDR_SIZE;
    if (rtl_dlfw_to_mem(d, cur, dmem_addr, dmem_size) != 0) { kprintf("[RTL8812BU] DMEM download failed\n"); return -2; }
    rtl_w8(d, REG_MCUFW_CTRL, (uint8_t)(rtl_r8(d, REG_MCUFW_CTRL) | BIT_DMEM_DW_OK | BIT_DMEM_CHKSUM_OK));
    cur = fw + WLAN_FW_HDR_SIZE + dmem_size;
    if (rtl_dlfw_to_mem(d, cur, imem_addr, imem_size) != 0) { kprintf("[RTL8812BU] IMEM download failed\n"); return -3; }
    rtl_w8(d, REG_MCUFW_CTRL, (uint8_t)(rtl_r8(d, REG_MCUFW_CTRL) | BIT_IMEM_DW_OK | BIT_IMEM_CHKSUM_OK));
    bootlog_write("[RTL8812BU] FW IMEM+DMEM downloaded, 0x80=0x%04x", rtl_r16(d, REG_MCUFW_CTRL));

    // dlfw end flow: check IMEM+DMEM ok, set FW_DW_RDY, enable 8051, poll 0xC078.
    rtl_w32(d, REG_TXDMA_STATUS, BIT(2));
    uint16_t fwctrl = rtl_r16(d, REG_MCUFW_CTRL);
    if ((fwctrl & 0x50) != 0x50) {
        kprintf("[RTL8812BU] IMEM/DMEM checksum not OK (0x80=0x%04x)\n", fwctrl);
        bootlog_write("[RTL8812BU] FWDL IDMEM chksum fail 0x80=0x%04x", fwctrl);
        return -4;
    }
    rtl_w16(d, REG_MCUFW_CTRL, (uint16_t)((fwctrl | BIT_FW_DW_RDY) & ~BIT(0)));
    // enable wlan cpu
    rtl_w8(d, REG_RSV_CTRL + 1, (uint8_t)(rtl_r8(d, REG_RSV_CTRL + 1) | BIT(0)));
    rtl_w8(d, REG_SYS_FUNC_EN + 1, (uint8_t)(rtl_r8(d, REG_SYS_FUNC_EN + 1) | BIT(2)));

    int ready = 0;
    for (int t = 0; t < 5000; t++) {
        if (rtl_r16(d, REG_MCUFW_CTRL) == 0xC078) { ready = 1; break; }
        xhci_delay_ms(1);
    }
    uint16_t fin = rtl_r16(d, REG_MCUFW_CTRL);
    if (ready) {
        kprintf("[RTL8812BU] FIRMWARE READY: 0x80=0xC078 (MCU booted)\n");
        bootlog_write("[RTL8812BU] FW-READY OK 0x80=0xC078");
        return 0;
    }
    kprintf("[RTL8812BU] fw-ready poll failed, 0x80=0x%04x (want 0xC078)\n", fin);
    bootlog_write("[RTL8812BU] FW-READY FAIL 0x80=0x%04x", fin);
    return -5;
}

// =============================================================================
// Attach + probe
// =============================================================================
static int rtl_load_and_download_fw(rtl_dev_t *d) {
    // Firmware must use an 8.3-compliant FAT name (RTL8812B.BIN = 8.3). Try the
    // /FIRMWARE/ subdir first, then the FAT root as a fallback.
    static const char *paths[] = {
        "/FIRMWARE/RTL8812B.BIN",
        "/RTL8812B.BIN",
        "/FIRMWARE/RTL8812BU.BIN",
    };
    const char *path = 0;
    uint32_t sz = 0;
    void *fw = 0;
    for (unsigned k = 0; k < sizeof(paths) / sizeof(paths[0]); k++) {
        fw = fat_read_file(&g_fat_fs, paths[k], &sz);
        if (fw && sz) { path = paths[k]; break; }
        if (fw) { kfree(fw); fw = 0; }
    }
    if (!fw || sz == 0) {
        kprintf("[RTL8812BU] firmware not found (tried /FIRMWARE/RTL8812B.BIN, /RTL8812B.BIN)\n");
        bootlog_write("[RTL8812BU] FW FILE MISSING");
        return -1;
    }
    kprintf("[RTL8812BU] loaded firmware %s (%u bytes)\n", path, sz);
    bootlog_write("[RTL8812BU] FW file loaded %u bytes", sz);
    int r = rtl_download_firmware(d, (const uint8_t *)fw, sz);
    kfree(fw);
    return r;
}

int rtl8812bu_probe(xhci_controller_t *xhc, int slot_id, int speed,
                    uint16_t vid, uint16_t pid, uint8_t *cfg, int total) {
    if (vid != RTL_VID || pid != RTL_PID) return 0;   // not ours

    kprintf("[RTL8812BU] === RTL88x2BU WiFi adapter %04x:%04x (slot %d) ===\n", vid, pid, slot_id);
    bootlog_write("[RTL8812BU] PROBE %04x:%04x slot=%d speed=%d", vid, pid, slot_id, speed);

    // Walk config descriptor. The RTL8822BU (High-Speed) exposes 3 bulk-OUT
    // endpoints (0x05, 0x06, 0x08) + 1 bulk-IN (0x84) + 1 interrupt-IN (0x87).
    // The halmac queue->pipe map for 3 out-pipes routes HIGH/BCN/CMD to the
    // FIRST out-pipe (RtOutPipe[0] = 0x05), which is what the reserved-page
    // firmware download uses; BE/BK/VI/VO/MGT spread across the remaining pipes.
    int ep_in = -1, ep_out = -1, in_mps = 0, out_mps = 0;
    int ep_out_list[4]; int ep_out_num = 0;
    int cfg_value = (total >= 6) ? cfg[5] : 1;
    int i = 0;
    while (i + 2 <= total) {
        int blen = cfg[i], btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x05 && blen >= 7) {
            int eaddr = cfg[i + 2];
            int eattr = cfg[i + 3] & 0x03;
            int emps  = cfg[i + 4] | (cfg[i + 5] << 8);
            if (eattr == 0x02) {  // bulk
                if ((eaddr & 0x80) && ep_in < 0)  { ep_in = eaddr; in_mps = emps; }
                if (!(eaddr & 0x80)) {
                    if (ep_out_num < 4) ep_out_list[ep_out_num++] = eaddr;
                    if (ep_out < 0) { ep_out = eaddr; out_mps = emps; }
                }
            }
        }
        i += blen;
    }
    if (ep_in < 0 || ep_out < 0) {
        kprintf("[RTL8812BU] no bulk endpoint pair (in=%d out=%d)\n", ep_in, ep_out);
        bootlog_write("[RTL8812BU] NO BULK EPS in=%d out=%d", ep_in, ep_out);
        return 1;   // it is ours but unusable; stop here
    }
    kprintf("[RTL8812BU] endpoints: bulk-IN 0x%02x (mps %d), %d bulk-OUT: ",
            ep_in, in_mps, ep_out_num);
    for (int k = 0; k < ep_out_num; k++) kprintf("0x%02x ", ep_out_list[k]);
    kprintf("(HIGH/BCN=0x%02x mps %d)\n", ep_out, out_mps);
    bootlog_write("[RTL8812BU] EPs in=0x%02x out_num=%d out0=0x%02x", ep_in, ep_out_num, ep_out);

    rtl_dev_t *d = &g_rtl;
    memset(d, 0, sizeof(*d));
    d->xhc = xhc; d->slot_id = slot_id; d->speed = speed;
    d->ep_in = ep_in; d->ep_out = ep_out; d->in_mps = in_mps; d->out_mps = out_mps;
    d->ep_out_num = ep_out_num;
    for (int k = 0; k < ep_out_num; k++) d->ep_out_list[k] = ep_out_list[k];

    if (xhci_set_configuration(xhc, slot_id, (uint8_t)cfg_value) < 0) {
        kprintf("[RTL8812BU] SET_CONFIGURATION failed\n");
        bootlog_write("[RTL8812BU] SET_CONFIG FAIL");
        return 1;
    }
    // Configure every bulk-OUT endpoint (the composite-HID-safe CONFIG_EP
    // re-asserts prior endpoints, #389) plus the bulk-IN.
    for (int k = 0; k < ep_out_num; k++)
        xhci_configure_endpoint_ep(xhc, slot_id, ep_out_list[k], EP_TYPE_BULK_OUT, out_mps, 0, speed);
    xhci_configure_endpoint_ep(xhc, slot_id, ep_in, EP_TYPE_BULK_IN, in_mps, 0, speed);

    d->wifi = wifi_core_register(vid, pid, "RTL8822B");

    // (b) chip version
    if (rtl_read_chip_version(d) != 0) {
        kprintf("[RTL8812BU] STOP: chip-version verify failed (register access down)\n");
        return 1;
    }
    if (d->wifi) { d->wifi->cut = d->cut; d->wifi->rf_type = d->rf_2t2r;
                   memcpy(d->wifi->chip, "RTL8822B", 9); }

    // (c) power on
    rtl_power_on(d);   // best-effort; continue even if a poll times out

    // (d) firmware upload is DEFERRED: the USB probe runs inside usb_init()
    // during early boot, BEFORE the root filesystem (ext2/FAT) is mounted, so
    // fat_read_file() cannot yet load /FIRMWARE/RTL8812B.BIN here. main.c calls
    // rtl8812bu_late_init() once the filesystem is up (see below).
    d->present_ready = 1;
    kprintf("[RTL8812BU] enum+chipver OK; firmware upload deferred to late-init (FS not mounted yet)\n");
    bootlog_write("[RTL8812BU] PHASE-1 enum+chipver OK, fw deferred");
    return 1;   // claimed
}

// =============================================================================
// PHASE 2: MAC init + efuse MAC + BB/AGC/RF register tables + IQK kickoff.
// Ported from halmac (init_mac_cfg_88xx / init_*_8822b) and phydm
// (odm_read_and_config_mp_8822b_*, config_phydm_*_8822b, halrf_iqk_8822b).
// BB registers live in the same USB vendor register space as MAC registers,
// so a BB read/write is just a 32-bit MAC register access at the BB address.
// RF registers are accessed indirectly through BB "RF-direct" windows.
// =============================================================================

// BB register 32-bit access (identical transport to MAC reg access).
static inline uint32_t bb_r32(rtl_dev_t *d, uint32_t a) { return rtl_r32(d, (uint16_t)a); }
static inline void     bb_w32(rtl_dev_t *d, uint32_t a, uint32_t v) { rtl_w32(d, (uint16_t)a, v); }

static int bb_shift(uint32_t mask) {
    int i;
    for (i = 0; i < 32; i++) if (mask & (1u << i)) return i;
    return 0;
}
// Masked BB write (read-modify-write) - matches phy_set_bb_reg.
static void bb_set(rtl_dev_t *d, uint32_t addr, uint32_t mask, uint32_t data) {
    if (mask != MASKDWORD) {
        uint32_t orig = bb_r32(d, addr);
        data = (orig & ~mask) | ((data << bb_shift(mask)) & mask);
    }
    bb_w32(d, addr, data);
}
// Masked BB read (read + downshift) - matches phy_query_bb_reg / odm_get_bb_reg.
static uint32_t bb_get(rtl_dev_t *d, uint32_t addr, uint32_t mask) {
    uint32_t v = bb_r32(d, addr) & mask;
    return (mask == MASKDWORD) ? v : (v >> bb_shift(mask));
}

// RF register read via BB RF-direct window (config_phydm_read_rf_reg_8822b).
static uint32_t rf_read(rtl_dev_t *d, int path, uint32_t addr, uint32_t mask) {
    static const uint32_t off_r[2] = { 0x2800, 0x2c00 };
    if (path > 1) return 0;
    addr &= 0xFF;
    uint32_t v = bb_r32(d, off_r[path] + (addr << 2));
    return v & (mask & RFREGOFFSETMASK);
}
// RF register write via BB LSSI window (config_phydm_write_rf_reg_8822b,
// full-mask path). data_and_addr = [27:20]=addr, [19:0]=data.
static void rf_write(rtl_dev_t *d, int path, uint32_t addr, uint32_t data) {
    static const uint32_t off_w[2] = { 0xc90, 0xe90 };
    if (path > 1) return;
    addr &= 0xFF;
    uint32_t data_and_addr = ((addr << 20) | (data & 0x000fffff)) & 0x0fffffff;
    bb_w32(d, off_w[path], data_and_addr);
}

// halmac check_positive: does a conditional table entry apply to THIS adapter?
// Only cut/package/interface/rfe_type are compared (matches the reference; the
// glna/gpa/alna/apa conditions cond2..4 are never checked in check_positive).
static int rtl_check_positive(rtl_dev_t *d, uint32_t cond1) {
    uint8_t cut_para = (d->cut == 0 /*ODM_CUT_A*/) ? 15 : d->cut;
    uint8_t pkg_para = 15;              // package_type 0 -> 15
    uint32_t itf = ODM_ITRF_USB;
    uint32_t driver1 = ((uint32_t)cut_para << 24) |
                       ((itf & 0xF0) << 16) |
                       ((uint32_t)ODM_CE_PLATFORM << 16) |
                       ((uint32_t)pkg_para << 12) |
                       ((itf & 0x0F) << 8) |
                       d->rfe_type;
    if (((cond1 & 0x0F000000) != 0) && ((cond1 & 0x0F000000) != (driver1 & 0x0F000000))) return 0;
    if (((cond1 & 0x0000F000) != 0) && ((cond1 & 0x0000F000) != (driver1 & 0x0000F000))) return 0;
    if (((cond1 & 0x00000F00) != 0) && ((cond1 & 0x00000F00) != (driver1 & 0x00000F00))) return 0;
    return ((cond1 & 0xFF) == (driver1 & 0xFF)) ? 1 : 0;
}

enum { TBL_BB_PHY, TBL_BB_AGC, TBL_RF_A, TBL_RF_B, TBL_MAC };

static void tbl_apply_one(rtl_dev_t *d, int kind, uint32_t addr, uint32_t data) {
    switch (kind) {
    case TBL_BB_PHY:
        // odm_config_bb_phy_8822b: special "addr" values are delays.
        if      (addr == 0xfe) xhci_delay_ms(50);
        else if (addr == 0xfd) xhci_delay_ms(5);
        else if (addr == 0xfc) xhci_delay_ms(1);
        else if (addr == 0xfb) xhci_delay_ms(1);   // 50us -> >=1ms granularity
        else if (addr == 0xfa) xhci_delay_ms(1);
        else if (addr == 0xf9) xhci_delay_ms(1);
        else bb_w32(d, addr, data);
        break;
    case TBL_BB_AGC:
        bb_w32(d, addr, data);
        break;
    case TBL_RF_A:
    case TBL_RF_B:
        if      (addr == 0xffe) xhci_delay_ms(50);
        else if (addr == 0xfe)  xhci_delay_ms(1);  // 100us
        else    rf_write(d, kind == TBL_RF_A ? 0 : 1, addr, data);
        break;
    case TBL_MAC:
        rtl_w8(d, (uint16_t)addr, (uint8_t)data);
        break;
    }
}

// Faithful port of odm_read_and_config_mp_8822b_* (the generated conditional
// table walker). Returns the number of register writes performed.
static int rtl_apply_table(rtl_dev_t *d, int kind, const uint32_t *arr, uint32_t len) {
    uint32_t i = 0;
    int matched = 1, skipped = 0, applied = 0;
    uint32_t pre_v1 = 0;
    while ((i + 1) < len) {
        uint32_t v1 = arr[i], v2 = arr[i + 1];
        if (v1 & (BIT(31) | BIT(30))) {
            if (v1 & BIT(31)) {                 // positive condition (IF/ELSEIF/ELSE/ENDIF)
                uint8_t c = (uint8_t)((v1 & (BIT(29) | BIT(28))) >> 28);
                if (c == COND_ENDIF) { matched = 1; skipped = 0; }
                else if (c == COND_ELSE) { matched = skipped ? 0 : 1; }
                else { pre_v1 = v1; }
            } else if (v1 & BIT(30)) {          // negative condition (close of IF test)
                if (!skipped) {
                    if (rtl_check_positive(d, pre_v1)) { matched = 1; skipped = 1; }
                    else { matched = 0; skipped = 0; }
                } else {
                    matched = 0;
                }
            }
        } else {
            if (matched) { tbl_apply_one(d, kind, v1, v2); applied++; }
        }
        i += 2;
    }
    return applied;
}

// -----------------------------------------------------------------------------
// (2.1) MAC init - halmac init_mac_cfg_88xx (trx + protocol + edca + wmac),
//       specialised for the RTL8822BU NORMAL trx mode, 3 bulk-OUT endpoints.
// -----------------------------------------------------------------------------
static int rtl_mac_init(rtl_dev_t *d) {
    // ---- TRX DMA config (init_trx_cfg_8822b) ----
    // txdma_queue_mapping: NORMAL mode / 3-bulkout RQPN
    //   VO=NQ(2) VI=NQ(2) BE=LQ(1) BK=LQ(1) MG=HQ(3) HI=HQ(3)
    //   fields: VOQ<<4 VIQ<<6 BEQ<<8 BKQ<<10 MGQ<<12 HIQ<<14
    uint16_t pqmap = (uint16_t)((2u<<4)|(2u<<6)|(1u<<8)|(1u<<10)|(3u<<12)|(3u<<14));
    rtl_w16(d, REG_TXDMA_PQ_MAP, pqmap);

    uint8_t en_fwff = rtl_r8(d, REG_WMAC_FWPKT_CR) & BIT_FWEN;
    if (en_fwff) rtl_w8(d, REG_WMAC_FWPKT_CR, (uint8_t)(rtl_r8(d, REG_WMAC_FWPKT_CR) & ~BIT_FWEN));
    rtl_w8(d, REG_CR, 0);
    rtl_w16(d, REG_FWFF_CTRL, rtl_r16(d, REG_FWFF_PKT_INFO));
    rtl_w8(d, REG_CR, MAC_TRX_ENABLE);
    if (en_fwff) rtl_w8(d, REG_WMAC_FWPKT_CR, (uint8_t)(rtl_r8(d, REG_WMAC_FWPKT_CR) | BIT_FWEN));
    rtl_w32(d, REG_H2CQ_CSR, BIT(31));

    // ---- TRX FIFO page boundaries (set_trx_fifo_info + priority_queue_cfg) ----
    uint32_t tx_fifo_pg = TX_FIFO_SIZE_8822B >> TX_PAGE_SIZE_SHIFT;         // 2048
    uint32_t rsvd_pg = RSVD_PG_DRV_NUM + RSVD_PG_H2C_EXTRAINFO + RSVD_PG_H2C_STATICINFO +
                       RSVD_PG_H2CQ_NUM + RSVD_PG_CPU_INSTR + RSVD_PG_FW_TXBUF + RSVD_PG_CSIBUF; // 60
    uint32_t acq_pg = tx_fifo_pg - rsvd_pg;                                 // 1988
    uint32_t rsvd_boundary = acq_pg;                                        // 1988
    // NORMAL / 3-bulkout page numbers: hq=64 nq=64 lq=64 exq=0 gap=1
    uint32_t hq = 64, nq = 64, lq = 64, exq = 0, gap = 1;
    uint32_t pubq = acq_pg - hq - lq - nq - exq - gap;                      // 1795
    // reserved-page sub addresses (top-down)
    uint32_t cur = tx_fifo_pg;
    cur -= RSVD_PG_CSIBUF; cur -= RSVD_PG_FW_TXBUF; cur -= RSVD_PG_CPU_INSTR;
    uint32_t rsvd_h2cq_addr = (cur -= RSVD_PG_H2CQ_NUM);                    // 2036

    rtl_w16(d, REG_FIFOPAGE_INFO_1, (uint16_t)hq);
    rtl_w16(d, REG_FIFOPAGE_INFO_2, (uint16_t)lq);
    rtl_w16(d, REG_FIFOPAGE_INFO_3, (uint16_t)nq);
    rtl_w16(d, REG_FIFOPAGE_INFO_4, (uint16_t)exq);
    rtl_w16(d, REG_FIFOPAGE_INFO_5, (uint16_t)pubq);
    rtl_w32(d, REG_RQPN_CTRL_2, rtl_r32(d, REG_RQPN_CTRL_2) | BIT(31));
    rtl_w16(d, REG_FIFOPAGE_CTRL_2, (uint16_t)(rsvd_boundary & 0x0FFF));
    rtl_w8(d, REG_FWHW_TXQ_CTRL + 2, (uint8_t)(rtl_r8(d, REG_FWHW_TXQ_CTRL + 2) | BIT(4)));
    rtl_w16(d, REG_BCNQ_BDNY_V1, (uint16_t)rsvd_boundary);
    rtl_w16(d, REG_FIFOPAGE_CTRL_2 + 2, (uint16_t)rsvd_boundary);
    rtl_w16(d, REG_BCNQ1_BDNY_V1, (uint16_t)rsvd_boundary);
    rtl_w32(d, REG_RXFF_BNDY, RX_FIFO_SIZE_8822B - C2H_PKT_BUF - 1);

    // USB LLT block-desc + auto-init LLT
    { uint8_t v = rtl_r8(d, REG_AUTO_LLT_V1);
      v = (uint8_t)((v & ~(0xF << 4)) | (BLK_DESC_NUM << 4));
      rtl_w8(d, REG_AUTO_LLT_V1, v); }
    rtl_w8(d, REG_AUTO_LLT_V1 + 3, BLK_DESC_NUM);
    rtl_w8(d, REG_TXDMA_OFFSET_CHK + 1, (uint8_t)(rtl_r8(d, REG_TXDMA_OFFSET_CHK + 1) | BIT(1)));
    rtl_w8(d, REG_AUTO_LLT_V1, (uint8_t)(rtl_r8(d, REG_AUTO_LLT_V1) | BIT_AUTO_INIT_LLT));
    int llt_ok = 0;
    for (int t = 0; t < 1000; t++) {
        if (!(rtl_r8(d, REG_AUTO_LLT_V1) & BIT_AUTO_INIT_LLT)) { llt_ok = 1; break; }
        xhci_delay_ms(1);
    }
    rtl_w8(d, REG_CR + 3, 0 /*HALMAC_TRNSFER_NORMAL*/);

    // ---- H2C queue init (init_h2c_8822b) ----
    { uint32_t h2cq_addr = rsvd_h2cq_addr << TX_PAGE_SIZE_SHIFT;
      uint32_t h2cq_size = RSVD_PG_H2CQ_NUM << TX_PAGE_SIZE_SHIFT;
      rtl_w32(d, REG_H2C_HEAD, (rtl_r32(d, REG_H2C_HEAD) & 0xFFFC0000) | h2cq_addr);
      rtl_w32(d, REG_H2C_READ_ADDR, (rtl_r32(d, REG_H2C_READ_ADDR) & 0xFFFC0000) | h2cq_addr);
      rtl_w32(d, REG_H2C_TAIL, (rtl_r32(d, REG_H2C_TAIL) & 0xFFFC0000) | (h2cq_addr + h2cq_size));
      rtl_w8(d, REG_H2C_INFO, (uint8_t)((rtl_r8(d, REG_H2C_INFO) & 0xFC) | 0x01));
      rtl_w8(d, REG_H2C_INFO, (uint8_t)((rtl_r8(d, REG_H2C_INFO) & 0xFB) | 0x04));
      rtl_w8(d, REG_TXDMA_OFFSET_CHK + 1, (uint8_t)((rtl_r8(d, REG_TXDMA_OFFSET_CHK + 1) & 0x7F) | 0x80)); }
    // USB: enable TXDMA agg
    rtl_w8(d, REG_TXDMA_PQ_MAP, (uint8_t)(rtl_r8(d, REG_TXDMA_PQ_MAP) | BIT(0)));

    // ---- protocol config (init_protocol_cfg_8822b) ----
    rtl_w8(d, REG_SW_AMPDU_BURST_MODE_CTRL, (uint8_t)(rtl_r8(d, REG_SW_AMPDU_BURST_MODE_CTRL) & ~BIT(6)));
    rtl_w8(d, REG_AMPDU_MAX_TIME_V1, WLAN_AMPDU_MAX_TIME);
    rtl_w8(d, REG_TX_HANG_CTRL, (uint8_t)(rtl_r8(d, REG_TX_HANG_CTRL) | BIT_EN_EOF_V1));
    rtl_w32(d, REG_PROT_MODE_CTRL, WLAN_PROT_MODE_CFG);
    rtl_w16(d, REG_BAR_MODE_CTRL + 2, WLAN_BAR_CFG);
    rtl_w8(d, REG_FAST_EDCA_VOVI_SETTING, WLAN_FAST_EDCA_TH);
    rtl_w8(d, REG_FAST_EDCA_VOVI_SETTING + 2, WLAN_FAST_EDCA_TH);
    rtl_w8(d, REG_FAST_EDCA_BEBK_SETTING, WLAN_FAST_EDCA_TH);
    rtl_w8(d, REG_FAST_EDCA_BEBK_SETTING + 2, WLAN_FAST_EDCA_TH);
    rtl_w8(d, REG_INIRTS_RATE_SEL, (uint8_t)(rtl_r8(d, REG_INIRTS_RATE_SEL) | BIT(5)));

    // ---- EDCA config (init_edca_cfg_8822b) ----
    rtl_w8(d, REG_TIMER0_SRC_SEL, (uint8_t)(rtl_r8(d, REG_TIMER0_SRC_SEL) & ~(BIT(4)|BIT(5)|BIT(6))));
    rtl_w16(d, REG_TXPAUSE, 0x0000);
    rtl_w8(d, REG_SLOT, WLAN_SLOT_TIME);
    rtl_w8(d, REG_PIFS, WLAN_PIFS_TIME);
    rtl_w32(d, REG_SIFS, WLAN_SIFS_CFG);
    rtl_w16(d, REG_EDCA_VO_PARAM + 2, WLAN_VO_TXOP_LIMIT);
    rtl_w16(d, REG_EDCA_VI_PARAM + 2, WLAN_VI_TXOP_LIMIT);
    rtl_w32(d, REG_RD_NAV_NXT, WLAN_NAV_CFG);
    rtl_w16(d, REG_RXTSF_OFFSET_CCK, WLAN_RX_TSF_CFG);
    rtl_w8(d, REG_BCN_CTRL, (uint8_t)(rtl_r8(d, REG_BCN_CTRL) | BIT_EN_BCN_FUNCTION));
    rtl_w32(d, REG_TBTT_PROHIBIT, WLAN_TBTT_TIME);
    rtl_w8(d, REG_DRVERLYINT, WLAN_DRV_EARLY_INT);
    rtl_w8(d, REG_BCNDMATIM, WLAN_BCN_DMA_TIME);
    rtl_w8(d, REG_TX_PTCL_CTRL + 1, (uint8_t)(rtl_r8(d, REG_TX_PTCL_CTRL + 1) & ~BIT(4)));

    // ---- WMAC config (init_wmac_cfg_8822b) ----
    rtl_w32(d, REG_RXFLTMAP0, WLAN_RX_FILTER0);
    rtl_w16(d, REG_RXFLTMAP2, WLAN_RX_FILTER2);
    rtl_w32(d, REG_RCR, WLAN_RCR_CFG);
    rtl_w8(d, REG_RX_PKT_LIMIT, WLAN_RXPKT_MAX_SZ_512);
    rtl_w8(d, REG_TCR + 2, WLAN_TX_FUNC_CFG2);
    rtl_w8(d, REG_TCR + 1, WLAN_TX_FUNC_CFG1);
    rtl_w8(d, REG_WMAC_TRXPTCL_CTL + 4, (uint8_t)(rtl_r8(d, REG_WMAC_TRXPTCL_CTL + 4) | BIT(1)));
    rtl_w8(d, REG_SND_PTCL_CTRL, (uint8_t)(rtl_r8(d, REG_SND_PTCL_CTRL) | BIT_R_DIS_CHK_VHTSIGB_CRC));
    rtl_w32(d, REG_WMAC_OPTION_FUNCTION + 8, WLAN_MAC_OPT_FUNC2);
    rtl_w8(d, REG_WMAC_OPTION_FUNCTION + 4, WLAN_MAC_OPT_NORM_FUNC1);

    uint8_t cr = rtl_r8(d, REG_CR);
    kprintf("[RTL8812BU] MAC-init: CR=0x%02x (want 0xFF) pqmap=0x%04x LLT=%s rsvd_bndy=%u pubq=%u\n",
            cr, pqmap, llt_ok ? "OK" : "TIMEOUT", rsvd_boundary, pubq);
    bootlog_write("[RTL8812BU] MAC-INIT CR=0x%02x LLT=%d bndy=%u", cr, llt_ok, rsvd_boundary);
    return (cr == MAC_TRX_ENABLE) ? 0 : -1;
}

// -----------------------------------------------------------------------------
// (2.2) Efuse read + logical-map parse -> station MAC + rfe_type.
//       read_hw_efuse_88xx (physical) + eeprom_parser_88xx (logical decode).
// -----------------------------------------------------------------------------
static int rtl_read_phys_efuse(rtl_dev_t *d, uint8_t *buf, uint32_t size) {
    uint32_t v = rtl_r32(d, REG_EFUSE_CTRL);
    for (uint32_t addr = 0; addr < size; addr++) {
        v &= ~(BIT_MASK_EF_DATA | (BIT_MASK_EF_ADDR << BIT_SHIFT_EF_ADDR));
        v |= ((addr & BIT_MASK_EF_ADDR) << BIT_SHIFT_EF_ADDR);
        rtl_w32(d, REG_EFUSE_CTRL, v & ~BIT_EF_FLAG);
        // Each register read over USB is already ~0.5ms, far longer than the
        // efuse cell read time, so the ready flag is set within a few reads.
        int ok = 0; uint32_t t = 0;
        for (int c = 0; c < 100; c++) {
            t = rtl_r32(d, REG_EFUSE_CTRL);
            if (t & BIT_EF_FLAG) { ok = 1; break; }
        }
        if (!ok) return -1;
        buf[addr] = (uint8_t)(t & BIT_MASK_EF_DATA);
    }
    return 0;
}

static int rtl_efuse_read(rtl_dev_t *d) {
    uint32_t phys_size = EFUSE_SIZE_8822B - PRTCT_EFUSE_SIZE;   // 928 physical bytes
    uint8_t *phys = (uint8_t *)kmalloc(phys_size);
    uint8_t *logmap = (uint8_t *)kmalloc(EEPROM_SIZE_8822B);
    if (!phys || !logmap) { if (phys) kfree(phys); if (logmap) kfree(logmap); return -1; }

    if (rtl_read_phys_efuse(d, phys, phys_size) != 0) {
        kprintf("[RTL8812BU] efuse: physical read timeout\n");
        kfree(phys); kfree(logmap);
        return -1;
    }
    // eeprom_parser_88xx: decode packed physical map -> 0x300-byte logical map.
    memset(logmap, 0xFF, EEPROM_SIZE_8822B);
    uint32_t eidx = 0;
    while (eidx < phys_size) {
        uint8_t hdr = phys[eidx];
        uint8_t blk, word_en;
        if (hdr == 0xFF) break;
        if ((hdr & 0x1F) == 0x0F) {                 // 2-byte extended header
            eidx++;
            if (eidx >= phys_size) break;
            uint8_t hdr2 = phys[eidx];
            if (hdr2 == 0xFF) break;
            blk = (uint8_t)(((hdr2 & 0xF0) >> 1) | ((hdr >> 5) & 0x07));
            word_en = hdr2 & 0x0F;
        } else {
            blk = (hdr & 0xF0) >> 4;
            word_en = hdr & 0x0F;
        }
        eidx++;
        for (int w = 0; w < 4; w++) {
            if (((~(word_en >> w)) & 0x01) == 1) {  // word present
                uint32_t li = ((uint32_t)blk << 3) + ((uint32_t)w << 1);
                if (eidx + 1 >= phys_size || li + 1 >= EEPROM_SIZE_8822B) { eidx = phys_size; break; }
                logmap[li]     = phys[eidx++];
                logmap[li + 1] = phys[eidx++];
            }
        }
    }
    memcpy(d->mac, logmap + EEPROM_MAC_ADDR_8822BU, 6);
    d->rfe_type = logmap[EEPROM_RFE_OPTION_8822B];
    if (d->rfe_type == 0xFF) d->rfe_type = 0;       // blank efuse -> default RFE
    int allzero = 1, allff = 1;
    for (int i = 0; i < 6; i++) { if (d->mac[i]) allzero = 0; if (d->mac[i] != 0xFF) allff = 0; }
    d->mac_valid = (!allzero && !allff) ? 1 : 0;

    kprintf("[RTL8812BU] efuse MAC = %02x:%02x:%02x:%02x:%02x:%02x  rfe_type=0x%02x  %s\n",
            d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
            d->rfe_type, d->mac_valid ? "VALID" : "blank/invalid");
    bootlog_write("[RTL8812BU] EFUSE MAC %02x:%02x:%02x:%02x:%02x:%02x rfe=0x%02x %s",
                  d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
                  d->rfe_type, d->mac_valid ? "VALID" : "blank");
    if (d->mac_valid) {
        for (int i = 0; i < 6; i++) rtl_w8(d, (uint16_t)(REG_MACID + i), d->mac[i]);
        if (d->wifi) memcpy(d->wifi->mac, d->mac, 6);
    }
    kfree(phys); kfree(logmap);
    return d->mac_valid ? 0 : -2;
}

// -----------------------------------------------------------------------------
// (2.3) BB + AGC + RF register-table application (phy_bb_config / phy_rf_config).
// -----------------------------------------------------------------------------
static void rtl_enable_bb_rf(rtl_dev_t *d);
static void rf_probe(rtl_dev_t *d, const char *tag);

static void rtl_phy_config(rtl_dev_t *d) {
    // Power up BB + enable the RF 3-wire read/write window BEFORE the BB/RF
    // tables are written. The reference (rtl8822b_phy_init) does the equivalent
    // (rtw_halmac_phy_power_switch) up front; deferring it to phase-3 meant the
    // radio-table writes were issued into a 3-wire that was never enabled.
    rtl_enable_bb_rf(d);
    // MAC PHY register table (odm_config_mac_with_header_file) - byte writes.
    int n_mac = rtl_apply_table(d, TBL_MAC, rtl8822b_mac_reg,
                                sizeof(rtl8822b_mac_reg) / sizeof(uint32_t));
    // PRE setting: disable OFDM/CCK block while programming BB.
    bb_set(d, 0x808, BIT(28) | BIT(29), 0x0);
    int n_phy = rtl_apply_table(d, TBL_BB_PHY, rtl8822b_phy_reg,
                                sizeof(rtl8822b_phy_reg) / sizeof(uint32_t));
    int n_agc = rtl_apply_table(d, TBL_BB_AGC, rtl8822b_agc_tab,
                                sizeof(rtl8822b_agc_tab) / sizeof(uint32_t));
    kprintf("[RTL8812BU] BB tables applied: MAC_REG=%d PHY_REG=%d AGC_TAB=%d (rfe=0x%02x)\n",
            n_mac, n_phy, n_agc, d->rfe_type);
    bootlog_write("[RTL8812BU] BB applied MAC=%d PHY=%d AGC=%d", n_mac, n_phy, n_agc);

    int n_rfa = rtl_apply_table(d, TBL_RF_A, rtl8822b_radioa,
                                sizeof(rtl8822b_radioa) / sizeof(uint32_t));
    int n_rfb = rtl_apply_table(d, TBL_RF_B, rtl8822b_radiob,
                                sizeof(rtl8822b_radiob) / sizeof(uint32_t));
    // POST setting: re-enable OFDM/CCK block.
    bb_set(d, 0x808, BIT(28) | BIT(29), 0x3);
    // Read back a BB id register + RF path-A reg 0x0 to prove writes reached silicon.
    uint32_t bb_id = bb_r32(d, 0x800);
    uint32_t rfa0  = rf_read(d, 0, 0x00, RFREGOFFSETMASK);
    uint32_t rfb0  = rf_read(d, 1, 0x00, RFREGOFFSETMASK);
    kprintf("[RTL8812BU] RF tables applied: RADIO_A=%d RADIO_B=%d; 0x800=0x%08x RF-A[0]=0x%05x RF-B[0]=0x%05x\n",
            n_rfa, n_rfb, bb_id, rfa0, rfb0);
    bootlog_write("[RTL8812BU] RF applied A=%d B=%d 0x800=0x%08x RFA0=0x%05x", n_rfa, n_rfb, bb_id, rfa0);
    rf_probe(d, "post-phytable");   // do LSSI writes work right after the tables?
}

// -----------------------------------------------------------------------------
// (2.4) IQK kickoff (partial, honest). Ports the observable IQK entry from
//       halrf_iqk_8822b: parameter init (0x1b10), assert GNT_WL on both RF
//       paths (RF reg 0x1 |= BIT5|BIT0), then read the IQK NCTL status regs to
//       confirm the calibration block is alive on real silicon. The full
//       per-path LOK/TXK/RXK sweep is a large follow-up port; NOT done here.
// -----------------------------------------------------------------------------
static void rtl_iqk_kickoff(rtl_dev_t *d) {
    bb_w32(d, 0x1b10, 0x88011c00);              // _iq_calibrate_8822b_init param
    uint32_t nctl_ver = bb_r32(d, 0x1bf0);      // IQK NCTL FW/version window
    // GNT_WL = 1 on both paths (_iqk_start_iqk_8822b prologue)
    uint32_t a = rf_read(d, 0, 0x01, RFREGOFFSETMASK) | BIT(5) | BIT(0);
    rf_write(d, 0, 0x01, a);
    uint32_t b = rf_read(d, 1, 0x01, RFREGOFFSETMASK) | BIT(5) | BIT(0);
    rf_write(d, 1, 0x01, b);
    // Probe the NCTL one-shot registers (report-only, no full sweep).
    bb_w32(d, 0x1b00, 0xf8000008);              // select path-A NCTL
    uint32_t st = bb_r32(d, 0x1bfc);
    kprintf("[RTL8812BU] IQK kickoff (partial): 0x1b10 param set, GNT_WL A/B asserted, "
            "NCTL ver=0x%08x status=0x%08x (full LOK/TXK/RXK deferred)\n", nctl_ver, st);
    bootlog_write("[RTL8812BU] IQK-KICKOFF ver=0x%08x st=0x%08x", nctl_ver, st);
}

// =============================================================================
// PHASE 3: RF power-up -> channel set (2.4G/20MHz) -> RX enable -> passive SCAN.
// Ports the RF-on path (halmac enable_bb_rf_88xx), the phydm channel/bandwidth
// switch (config_phydm_switch_{band,channel,bandwidth}_8822b, 2.4G/20M subset),
// the WMAC RCR promiscuous config, and an 802.11 beacon/probe-response parser.
// This is the first user-visible milestone: a list of nearby SSIDs.
// =============================================================================

// Masked RF write (read-modify-write; matches config_phydm_write_rf_reg_8822b).
static void rf_write_mask(rtl_dev_t *d, int path, uint32_t addr, uint32_t mask, uint32_t data) {
    mask &= RFREGOFFSETMASK;
    if (mask != RFREGOFFSETMASK) {
        uint32_t orig = rf_read(d, path, addr, RFREGOFFSETMASK);
        int s = bb_shift(mask);
        data = (orig & ~mask) | ((data << s) & mask);
    }
    rf_write(d, path, addr, data);
}

// Faithful phydm_igi_toggle_8822b: read IGI, write IGI-2, write IGI back (both
// paths). The change (not the final value) is what makes the BB flush pending
// RF 3-wire (LSSI) writes to the radio - the BB does NOT emit 3-wire commands
// while the RX path is enabled, so every RF register write after the RX path is
// on stays queued until an IGI toggle. A fixed re-write of the same IGI value is
// a no-op and never flushes.
static void phydm_igi_toggle(rtl_dev_t *d) {
    uint32_t igi = bb_r32(d, 0xc50) & 0x7f;
    bb_set(d, 0xc50, 0x7f, igi - 2);
    bb_set(d, 0xc50, 0x7f, igi);
    bb_set(d, 0xe50, 0x7f, igi - 2);
    bb_set(d, 0xe50, 0x7f, igi);
}

// (3.0) Power up the BB + RF and enable the RF 3-wire so the RF register direct
// read/write window (BB 0x2800/0x2c00) actually works. halmac enable_bb_rf_88xx.
// PHASE 2 applied the RF tables WITHOUT this, so the RF read-back returned 0 and
// the RF LSSI writes may never have reached the radio. This is the phase-2 fix.
static void rtl_enable_bb_rf(rtl_dev_t *d) {
    rtl_w8(d, REG_SYS_FUNC_EN, (uint8_t)(rtl_r8(d, REG_SYS_FUNC_EN) | BIT(0) | BIT(1)));
    rtl_w8(d, REG_RF_CTRL, (uint8_t)(rtl_r8(d, REG_RF_CTRL) | BIT(0) | BIT(1) | BIT(2)));
    uint32_t v = rtl_r32(d, REG_WLRF1);
    v |= BIT(24) | BIT(25) | BIT(26);
    rtl_w32(d, REG_WLRF1, v);
    xhci_delay_ms(1);
}

// Small RF write/read probe used to pinpoint WHERE the LSSI write path dies.
static void rf_probe(rtl_dev_t *d, const char *tag) {
    uint32_t b18 = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
    uint32_t b00 = rf_read(d, 0, 0x00, RFREGOFFSETMASK);
    rf_write(d, 0, 0x18, 0x00407);
    uint32_t a18 = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
    uint32_t r1b00 = bb_r32(d, 0x1b00), r1bf0 = bb_r32(d, 0x1bf0);
    kprintf("[RTL8812BU]  RFPROBE %s: RF00=%05x RF18 %05x->wrote407->%05x %s  1b00=%08x 1bf0=%08x\n",
            tag, b00, b18, a18, (a18 == 0x407) ? "WRITES-WORK" : "dead", r1b00, r1bf0);
    bootlog_write("[RTL8812BU] RFPROBE %s RF18 %05x->%05x %s 1b00=%08x", tag, b18, a18,
                  (a18 == 0x407) ? "OK" : "dead", r1b00);
    // put it back to a benign channel-1 value
    rf_write(d, 0, 0x18, 0x00c01);
}

// PHASE 4c: Dump the RF synthesizer / PLL state so we can tell whether the LO
// actually relocked to the commanded channel. RF 0x18 = band+channel+bw. On the
// 8822B the synth (SX) auto-band-selects on a 0x18 write; there is no simple
// "PLL locked" status bit, so we snapshot the synth-related RF regs that DO change
// with frequency (0xb2 SX cap/CP, 0xc9 AAC/VCO band, 0x2b/0x2c VCO, 0xdf, 0x00 mode)
// alongside the channel field. A frequency that does NOT track the commanded
// channel (or a synth reg stuck across channels) is the LO-relock smoking gun.
static void rtl_synth_report(rtl_dev_t *d, const char *tag) {
    uint32_t a18 = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
    uint32_t b18 = rf_read(d, 1, 0x18, RFREGOFFSETMASK);
    uint32_t a00 = rf_read(d, 0, 0x00, RFREGOFFSETMASK);
    uint32_t ab2 = rf_read(d, 0, 0xb2, RFREGOFFSETMASK);
    uint32_t ac9 = rf_read(d, 0, 0xc9, RFREGOFFSETMASK);
    uint32_t a2b = rf_read(d, 0, 0x2b, RFREGOFFSETMASK);
    uint32_t a2c = rf_read(d, 0, 0x2c, RFREGOFFSETMASK);
    uint32_t adf = rf_read(d, 0, 0xdf, RFREGOFFSETMASK);
    kprintf("[RTL8812BU]  SYNTH %s: RFA18=%05x(ch=%u,5g=%u,bw=%u) RFB18=%05x RFA00=%05x "
            "b2=%05x c9=%05x(vco=%u) 2b=%05x 2c=%05x df=%05x\n",
            tag, a18, (unsigned)(a18 & 0xff), (unsigned)((a18 >> 16) & 1),
            (unsigned)((a18 >> 10) & 3), b18, a00, ab2, ac9,
            (unsigned)((ac9 >> 3) & 0x1f), a2b, a2c, adf);
    bootlog_write("[RTL8812BU] SYNTH %s A18=%05x B18=%05x c9=%05x b2=%05x", tag, a18, b18, ac9, ab2);
}

// PHASE 4d: full RF register dump (0x00..0xff, path A + B). The mandate asks for
// this so the +50 MHz LO source can be found by diffing our per-channel RF words
// against what phy_switch_channel_8822b / the 8822B radio tables produce. The
// radioa init table is already proven byte-identical to the reference, and RF
// 0x18 tracks the commanded channel, so anything that differs here between our
// ch11 and a known-good ch11 capture is the synth-offset suspect. Printed 8 regs
// per line to keep the serial parseable.
static void rtl_rf_fulldump(rtl_dev_t *d, const char *tag) {
    for (int path = 0; path <= (d->rf_2t2r ? 1 : 0); path++) {
        kprintf("[RTL8812BU]  RFDUMP %s path%c:\n", tag, path ? 'B' : 'A');
        for (uint32_t base = 0; base < 0x100; base += 8) {
            uint32_t v[8];
            for (int i = 0; i < 8; i++) v[i] = rf_read(d, path, base + i, RFREGOFFSETMASK);
            kprintf("[RTL8812BU]   RF[%02x]: %05x %05x %05x %05x %05x %05x %05x %05x\n",
                    (unsigned)base, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
        }
    }
    bootlog_write("[RTL8812BU] RFDUMP %s done", tag);
}

// PHASE 4c: Faithful one-shot LCK (_phy_lc_calibrate_8822b) - the VCO / LC-tank
// calibration that lets the synthesizer LOCK. The reference only runs this in MP
// mode, so it is an EXPERIMENT here: run ONCE at init, report whether the LCK
// one-shot (RF 0x18 BIT15) self-clears (converges) and whether RF 0x18 survives.
// Includes the preconditions the p4b attempt was missing: aac_check, RF0x0 standby
// on BOTH paths, and the RTK disable/enable around the cal (RF 0xc4).
//
// PHASE 4e (found by CODE INSPECTION against the reference synth path + the p4c
// synth dumps, 2026-07-05): this LCK is NOT harmless. Its aac_check reads the
// natural VCO/SX band (RF 0xc9[7:3] = 3 on this dongle), deems it "out of range"
// (<4), and FORCES it to 6 (writes RF 0xca[19]=0 + RF 0xb2[18:14]=6). The p4c
// "set_channel recovers the LCK corruption" claim is INCOMPLETE: set_channel
// re-writes RF 0x18 (clearing the spurious 5G bit) but NEVER re-writes RF 0xc9 /
// 0xb2 / 0xca, so the WHOLE scan then runs on the FORCED band 6 (evidence:
//   ch11-pre-lck  c9=1c118 vco=3 b2=22488
//   ch11-post-lck c9=1c130 vco=6 b2=1a436 ).
// A wrong VCO/SX band is a FIXED-frequency (band-quantised) LO error, which is
// exactly the shape of the observed CONSTANT +50 MHz (+10 ch) offset. Gated by
// RTL8822B_INIT_LCK below so the natural band can be A/B tested on hardware.
static void __attribute__((unused)) rtl_lck_run(rtl_dev_t *d) {
    // aac_check_8822b: if RF 0xc9[7:3] out of [4,7], fix 0xca[19]=0 + 0xb2[18:14]=6.
    uint32_t aac = (rf_read(d, 0, 0xc9, 0xf8) >> 3);
    if (aac < 4 || aac > 7) {
        rf_write_mask(d, 0, 0xca, BIT(19), 0x0);
        rf_write_mask(d, 0, 0xb2, 0x7c000, 0x6);
    }
    uint32_t before18 = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
    uint32_t c00 = bb_r32(d, 0xc00), e00 = bb_r32(d, 0xe00);
    bb_w32(d, 0xc00, 0x4);
    bb_w32(d, 0xe00, 0x4);
    rf_write(d, 0, 0x00, 0x10000);            // both paths -> standby
    rf_write(d, 1, 0x00, 0x10000);
    uint32_t lc_cal = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
    rf_write(d, 0, 0xc4, 0x01402);            // disable RTK
    rf_write(d, 0, 0x18, lc_cal | 0x08000);   // start LCK (BIT15)
    proc_sleep(100);
    int cnt = 0, cleared = 0;
    for (cnt = 0; cnt < 8; cnt++) {
        if ((rf_read(d, 0, 0x18, 0x8000) >> 15) != 0x1) { cleared = 1; break; }
        proc_sleep(10);
    }
    uint32_t during18 = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
    rf_write(d, 0, 0x18, lc_cal);             // restore channel
    rf_write(d, 0, 0xc4, 0x81402);            // enable RTK
    bb_w32(d, 0xc00, c00);
    bb_w32(d, 0xe00, e00);
    rf_write(d, 0, 0x00, 0x3ffff);            // both paths -> normal
    rf_write(d, 1, 0x00, 0x3ffff);
    uint32_t after18 = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
    uint32_t aac2 = (rf_read(d, 0, 0xc9, 0xf8) >> 3);
    kprintf("[RTL8812BU]  LCK: aac=%u(->%u) 18 before=%05x during=%05x after=%05x self-clear=%s (cnt=%d)\n",
            (unsigned)aac, (unsigned)aac2, before18, during18, after18,
            cleared ? "YES" : "TIMEOUT", cnt);
    bootlog_write("[RTL8812BU] LCK selfclear=%s during=%05x after=%05x", cleared ? "Y" : "N", during18, after18);
}

// (3.1) Set 2.4GHz channel at 20MHz. Faithful minimal subset of the phydm
// switch band/channel/bandwidth for RF_PATH_A(+B). Writes RF reg 0x18 (band +
// channel + bandwidth) from a value read back over the direct window, and does
// the RF 0xb8[19] toggle ("debug for RF register reading error") that the
// reference performs on every channel change.
static void rtl_set_channel(rtl_dev_t *d, uint8_t ch) {
    // ---- switch band -> 2.4G (config_phydm_switch_band_8822b, 2.4G head) ----
    bb_set(d, 0x808, BIT(28), 0x1);        // enable CCK block
    bb_set(d, 0x454, BIT(7),  0x0);        // disable MAC CCK check
    bb_set(d, 0xa80, BIT(18), 0x0);        // disable BB CCK check
    bb_set(d, 0x814, 0x0000FC00, 15);      // CCA mask (default)
    uint32_t rf18 = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
    rf18 &= ~(BIT(16) | BIT(9) | BIT(8));  // RF band -> 2.4G
    rf_write(d, 0, 0x18, rf18);
    if (d->rf_2t2r) rf_write(d, 1, 0x18, rf18);

    // ---- switch channel (config_phydm_switch_channel_8822b, 2.4G) ----
    rf18 = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
    rf18 &= ~(BIT(18) | BIT(17) | 0xFF);   // clear old channel + hi-band bits
    rf18 |= ch;                            // set channel number
    bb_set(d, 0x958, 0x1f, 0x0);           // AGC table index 0 (2.4G)
    bb_set(d, 0x860, 0x1ffe0000, 0x96a);   // central freq (clock offset tracking)
    if (ch == 14) { bb_w32(d, 0xa24, 0x00006577); bb_set(d, 0xa28, 0x0000FFFF, 0x0000); }
    else          { bb_w32(d, 0xa24, 0x384f6577); bb_set(d, 0xa28, 0x0000FFFF, 0x1525); }
    rf_write_mask(d, 0, 0xbe, BIT(17) | BIT(16) | BIT(15), 0x0);  // phase noise (2.4G)
    rf_write_mask(d, 0, 0xdf, BIT(18), 0x0);

    // ---- switch bandwidth -> 20MHz (config_phydm_switch_bandwidth_8822b) ----
    rf18 |= BIT(11) | BIT(10);             // RF bandwidth = 20M
    rf_write(d, 0, 0x18, rf18);            // program band+channel+bw
    if (d->rf_2t2r) rf_write(d, 1, 0x18, rf18);
    // "Debug for RF register reading error" - also re-latches the read window.
    rf_write_mask(d, 0, 0xb8, BIT(19), 0);
    rf_write_mask(d, 0, 0xb8, BIT(19), 1);

    uint32_t v8ac = bb_r32(d, 0x8ac);
    v8ac &= 0xFFCFFC00;                     // BW20: small-BW/pri-ch/rf-mode = 0
    bb_w32(d, 0x8ac, v8ac);
    bb_set(d, 0x8c4, BIT(30), 0x1);        // ADC buffer clock

    // ---- MAC side (halmac cfg_ch / cfg_bw / cfg_pri_ch) ----
    rtl_w8(d, REG_CCK_CHECK, (uint8_t)(rtl_r8(d, REG_CCK_CHECK) & ~BIT(7)));   // 2.4G
    { uint32_t t = rtl_r32(d, REG_WMAC_TRXPTCL_CTL); t &= ~(BIT(7) | BIT(8)); // BW20
      rtl_w32(d, REG_WMAC_TRXPTCL_CTL, t); }
    rtl_w8(d, REG_DATA_SC, 0x00);          // primary sub-channel index 0

    // ---- RX digital FIR by bandwidth (phydm_rxdfirpar_by_bw_8822b, BW20) ----
    // PHASE 4c: this was MISSING. The reference switch_bandwidth tail programs the
    // RX DFIR decimation/passband per BW; without it the on-channel (near-DC)
    // subcarriers can be mis-filtered. BW20: 0x948/0x94c[29:28]=2, 0xc20/0xe20[31]=1.
    bb_set(d, 0x948, BIT(29) | BIT(28), 0x2);
    bb_set(d, 0x94c, BIT(29) | BIT(28), 0x2);
    bb_set(d, 0xc20, BIT(31), 0x1);
    bb_set(d, 0xe20, BIT(31), 0x1);

    // ---- Toggle RX path to avoid RX dead-zone (switch_bandwidth tail) ----
    // Reference writes 0x808[7:0]=0 then back to (rx_ant|rx_ant<<4). rx_ant=AB=0x3.
    bb_set(d, 0x808, 0x000000FF, 0x0);
    bb_set(d, 0x808, 0x000000FF, 0x33);

    // Force a low, sensitive initial gain (IGI), then TOGGLE it so the pending RF
    // 3-wire writes above (esp. the 0x18 channel write) are actually flushed to
    // the radio. A fixed re-write of 0x20 does not flush; the toggle does.
    bb_set(d, 0xc50, 0x7f, 0x20);          // path-A IGI (low, sensitive)
    bb_set(d, 0xe50, 0x7f, 0x20);          // path-B IGI
    phydm_igi_toggle(d);                   // flush 3-wire (RX-path is enabled)
}

// (3.1a) Put both RF paths into TX/RX mode and enable the RX path (phydm
// config_phydm_trx_mode_8822b + config_rx_path_8822b, BB_PATH_AB). WITHOUT this
// the RF sits in standby (3-wire mode table not programmed, RX path bits clear)
// and the MAC never receives a frame - this is the "0 rx-xfer" root cause.
static void rtl_config_trx_mode(rtl_dev_t *d) {
    // RF 3-wire mode table (0=shutdown 1=standby 2=TX 3=RX): 0x3231 per path.
    bb_set(d, 0xc08, 0x0000FFFF, 0x3231);
    bb_set(d, 0xe08, 0x0000FFFF, 0x3231);
    // config_rx_path(BB_PATH_AB):
    bb_set(d, 0xa2c, BIT(22), 0x0);        // disable MRC for CCK CCA
    bb_set(d, 0xa2c, BIT(18), 0x0);        // disable MRC for CCK barker
    bb_set(d, 0xa04, 0x0f000000, 0x0);     // CCK RX path A
    bb_set(d, 0x808, 0x000000FF, 0x33);    // RX path enable: (AB<<4)|AB
    bb_set(d, 0x1904, BIT(16), 0x1);       // enable antenna weighting (AB)
    bb_set(d, 0x800, BIT(28), 0x1);        // htstf ant-wgt enable
    bb_set(d, 0x850, BIT(23), 0x1);        // MRC mode
    // RF mode-table program (normal mode; path A drives the synthesizer).
    rf_write(d, 0, 0xef, 0x80000);
    rf_write(d, 0, 0x33, 0x00001);
    rf_write(d, 0, 0x3e, 0x00034);
    rf_write(d, 0, 0x3f, 0x4080c);
    rf_write(d, 0, 0xef, 0x00000);
    // Toggle IGI so the RF actually enters RX mode (BB won't drive 3-wire while
    // the RX path is enabled).
    uint32_t igi = bb_r32(d, 0xc50) & 0x7f;
    bb_set(d, 0xc50, 0x7f, igi - 2); bb_set(d, 0xc50, 0x7f, igi);
    bb_set(d, 0xe50, 0x7f, igi - 2); bb_set(d, 0xe50, 0x7f, igi);
    // Re-enable the OFDM/CCK block (parameter_init POST).
    bb_set(d, 0x808, BIT(28) | BIT(29), 0x3);
}

// (3.1a2) RFE (RF front-end) pin routing. rfe_type=3 is an iFEM (internal FEM):
// route the on-chip antenna switch to RX/TX for 2.4GHz. phydm_rfe_8822b_init +
// phydm_rfe_ifem(2.4G, rx_ant=AB). Without it the antenna is not connected to
// the RX LNA, so no signal reaches the receiver.
static void rtl_rfe_setup_2g(rtl_dev_t *d) {
    // phydm_rfe_8822b_init: chip-top mux + s0/s1 source + in/out direction.
    bb_set(d, 0x64, BIT(29) | BIT(28), 0x3);
    bb_set(d, 0x4c, BIT(26) | BIT(25), 0x0);
    bb_set(d, 0x40, BIT(2), 0x1);
    bb_set(d, 0x1990, 0x3f, 0x30);
    bb_set(d, 0x1990, BIT(11) | BIT(10), 0x3);
    bb_set(d, 0x974, 0x3f, 0x3f);
    bb_set(d, 0x974, BIT(11) | BIT(10), 0x3);
    // phydm_rfe_ifem (2.4GHz): signal source.
    bb_set(d, 0xcb0, 0xffffff, 0x745774);
    bb_set(d, 0xeb0, 0xffffff, 0x745774);
    bb_set(d, 0xcb4, 0x0000FF00, 0x57);   // MASKBYTE1
    bb_set(d, 0xeb4, 0x0000FF00, 0x57);
    // inverse or not.
    bb_set(d, 0xcbc, 0x3f, 0x0);
    bb_set(d, 0xcbc, BIT(11) | BIT(10), 0x0);
    bb_set(d, 0xebc, 0x3f, 0x0);
    bb_set(d, 0xebc, BIT(11) | BIT(10), 0x0);
    // antenna switch table, 2.4GHz, rx_ant=AB (2RX): 0xa501.
    bb_set(d, 0xca0, 0x0000FFFF, 0xa501);
    bb_set(d, 0xea0, 0x0000FFFF, 0xa501);
}

// (3.1b) Enable the USB RX DMA path (halmac init_usb_cfg_88xx). WITHOUT this the
// MAC never pushes received frames out the bulk-IN endpoint, so every scan read
// times out with zero data. REG_RXDMA_MODE = DMA_MODE | burst_cnt(3) | burst_sz.
static void rtl_init_usb_rx(rtl_dev_t *d) {
    uint8_t v = (uint8_t)(BIT(1) | (0x3 << 2));    // BIT_DMA_MODE | BURST_CNT=3
    uint8_t burst;
    if (rtl_r8(d, REG_SYS_CFG2 + 3) == 0x20) {
        burst = 0;                                 // USB 3.0
    } else {
        uint8_t st = rtl_r8(d, REG_USB_USBSTAT);
        burst = ((st & 0x3) == 0x1) ? 1 : 2;       // 1=USB2.0 HS, 2=FS
    }
    v |= (uint8_t)(burst << 4);                     // BURST_SIZE
    rtl_w8(d, REG_RXDMA_MODE, v);
    // REG_TXDMA_OFFSET_CHK |= BIT_DROP_DATA_EN (BIT9).
    rtl_w16(d, REG_TXDMA_OFFSET_CHK, (uint16_t)(rtl_r16(d, REG_TXDMA_OFFSET_CHK) | BIT(9)));
    kprintf("[RTL8812BU] USB RX DMA enabled: RXDMA_MODE=0x%02x (burst=%d)\n",
            rtl_r8(d, REG_RXDMA_MODE), burst);
}

// (3.2) Configure the WMAC RX for scanning: accept management frames (beacons,
// probe responses) from ANY BSSID, and append the PHY status (for RSSI).
static void rtl_rx_config_scan(rtl_dev_t *d) {
    rtl_init_usb_rx(d);
    // RCR: AAP|APM|AM|AB (accept all/phys/mcast/bcast) | APWRMGT |
    //      ADF|ACF|AMF (data/ctrl/mgmt) | APP_PHYST_RXFF (RSSI in RXFF).
    uint32_t rcr = BIT(0) | BIT(1) | BIT(2) | BIT(3)
                 | BIT(5)
                 | BIT(11) | BIT(12) | BIT(13)
                 | BIT(28);
    rtl_w32(d, REG_RCR, rcr);
    // Management-frame subtype filter: accept all subtypes (beacon=8, prb-rsp=5).
    rtl_w16(d, REG_RXFLTMAP0, 0xFFFF);
    rtl_w16(d, 0x06A2, 0xFFFF);            // control-frame subtype map
    rtl_w16(d, REG_RXFLTMAP2, 0xFFFF);     // data-frame subtype map
}

// PHASE 4d: re-apply ONLY the RX filters (RCR + subtype maps) WITHOUT re-running
// rtl_init_usb_rx(). Re-writing REG_RXDMA_MODE on an already-running RX DMA path
// (as rtl_rx_config_scan did before the join) was the join-RX-stall root cause:
// the scan hears 95 pkts at the offset channel, but the join - after a second
// rtl_rx_config_scan - read RXPKT_NUM[31:24]=0 and every bulk-IN timed out. The
// USB RX DMA is already up from the scan; only the filters need re-asserting.
static void rtl_rx_apply_filter(rtl_dev_t *d) {
    uint32_t rcr = BIT(0) | BIT(1) | BIT(2) | BIT(3)
                 | BIT(5)
                 | BIT(11) | BIT(12) | BIT(13)
                 | BIT(28);
    rtl_w32(d, REG_RCR, rcr);
    rtl_w16(d, REG_RXFLTMAP0, 0xFFFF);
    rtl_w16(d, 0x06A2, 0xFFFF);
    rtl_w16(d, REG_RXFLTMAP2, 0xFFFF);
}

// -----------------------------------------------------------------------------
// (3.3) 802.11 beacon / probe-response parser + BSS scan-result list.
// -----------------------------------------------------------------------------
enum { SEC_OPEN = 0, SEC_WEP, SEC_WPA, SEC_WPA2, SEC_WPA3 };
static const char *sec_name(int s) {
    switch (s) { case SEC_WEP: return "WEP"; case SEC_WPA: return "WPA";
                 case SEC_WPA2: return "WPA2"; case SEC_WPA3: return "WPA3";
                 default: return "OPEN"; }
}

#define WIFI_MAX_BSS 48
typedef struct {
    uint8_t bssid[6];
    char    ssid[33];
    uint8_t ssid_len;
    uint8_t channel;
    int     rssi;      // dBm (approximate for CCK)
    uint8_t sec;
} bss_t;
static bss_t g_bss[WIFI_MAX_BSS];
static int   g_bss_count;

// RX buffer for one bulk-IN URB (zeroed before each read; the RXDESC pkt_len
// walk stops at the first all-zero descriptor, since xhci_bulk_transfer reports
// only a completion code, not the residual length).
static uint8_t rx_buf[16384] __attribute__((aligned(64)));

// RXDESC accessors (rx_desc_nic, 8822B: 24-byte descriptor, little-endian).
#define RXD_PKT_LEN(p)  (rd_le32(p) & 0x3FFF)
#define RXD_CRC32(p)    ((rd_le32(p) >> 14) & 1)
#define RXD_ICVERR(p)   ((rd_le32(p) >> 15) & 1)
#define RXD_DRVINFO(p)  (((rd_le32(p) >> 16) & 0xF) * 8)
#define RXD_SHIFT(p)    ((rd_le32(p) >> 24) & 3)
#define RXD_PHYST(p)    ((rd_le32(p) >> 26) & 1)
#define RXD_C2H(p)      ((rd_le32((p) + 8) >> 28) & 1)
#define RXD_RATE(p)     (rd_le32((p) + 12) & 0x7F)
#define RXDESC_SIZE_8822B 24

// Approximate RSSI (dBm) from the appended PHY status (phy_status_rpt_8812).
// OFDM: rx_pwr = (gain_trsw[path] & 0x7f) - 110. CCK: from cfosho[0] AGC report
// (rough; the exact CCK LNA-gain table is calibration-dependent, so this is
// labelled approximate). Enough to rank nearby APs.
static int rtl_phy_rssi(const uint8_t *phy, uint8_t rate) {
    int is_cck = (rate <= 3);   // DESC_RATE1M..11M
    if (is_cck) {
        uint8_t agc = phy[5];                 // cfosho[0] = CCK AGC report
        int lna = (agc & 0xE0) >> 5;
        int vga = (agc & 0x1F);
        // Coarse CCK power model: higher LNA index = more attenuation.
        static const int lna_gain[8] = { -6, -20, -34, -48, -62, -76, -90, -104 };
        return lna_gain[lna & 7] - (vga << 1);
    }
    int a = (int)(phy[0] & 0x7F) - 110;       // gain_trsw[0]
    int b = (int)(phy[1] & 0x7F) - 110;       // gain_trsw[1]
    return (a > b) ? a : b;
}

static bss_t *rtl_bss_find(const uint8_t *bssid) {
    for (int i = 0; i < g_bss_count; i++)
        if (memcmp(g_bss[i].bssid, bssid, 6) == 0) return &g_bss[i];
    return 0;
}

// Parse one 802.11 management frame body (beacon or probe response). Returns 1
// if it produced/updated a BSS entry, else 0.
static int g_frame_dump;
static int rtl_parse_mgmt(const uint8_t *f, uint32_t flen, int rssi, uint8_t hop_ch) {
    if (flen < 24 + 12) return 0;
    uint8_t fc0 = f[0];
    uint8_t type = (fc0 >> 2) & 0x3;
    uint8_t subtype = (fc0 >> 4) & 0xF;
    if ((type == 0) && (subtype == 8 || subtype == 5) && g_frame_dump < 16) {
        g_frame_dump++;
        kprintf("[RTL8812BU]  BEACON fc0=%02x sub=%d flen=%u:", fc0, subtype, flen);
        for (uint32_t i = 0; i < 80 && i < flen; i++) kprintf("%02x", f[i]);
        kprintf("\n");
    }
    if (type != 0) return 0;                  // not management
    if (subtype != 8 && subtype != 5) return 0;   // beacon(8) / probe-resp(5)
    const uint8_t *bssid = f + 16;            // addr3
    // capability at body offset 24+10 (timestamp[8] + beacon-interval[2]).
    uint16_t cap = (uint16_t)(f[24 + 10] | (f[24 + 11] << 8));
    int privacy = (cap & (1 << 4)) ? 1 : 0;

    // Tagged parameters begin at body offset 24+12.
    char ssid[33]; int ssid_len = 0, ssid_seen = 0; uint8_t ds_ch = 0;
    int has_rsn = 0, has_wpa = 0, has_sae = 0;
    uint32_t o = 24 + 12;
    while (o + 2 <= flen) {
        uint8_t tag = f[o], len = f[o + 1];
        if (o + 2 + len > flen) break;
        const uint8_t *v = f + o + 2;
        if (tag == 0 && !ssid_seen) {         // SSID (only the FIRST element -
            ssid_seen = 1;                    // trailing zero padding also looks
            ssid_len = len > 32 ? 32 : len;   // like tag 0 / len 0 and must not
            for (int i = 0; i < ssid_len; i++) ssid[i] = (v[i] >= 32 && v[i] < 127) ? (char)v[i] : '.';
            ssid[ssid_len] = 0;               // overwrite a real SSID back to empty.
        } else if (tag == 3 && len >= 1) {    // DS parameter set -> channel
            ds_ch = v[0];
        } else if (tag == 48) {               // RSN IE -> WPA2/WPA3
            has_rsn = 1;
            // AKM suite list: scan for SAE (00-0F-AC:8) => WPA3.
            if (len >= 8) {
                int pc = v[6] | (v[7] << 8);          // pairwise count
                int akm_off = 8 + pc * 4;
                if (akm_off + 2 <= len) {
                    int ac = v[akm_off] | (v[akm_off + 1] << 8);
                    int as = akm_off + 2;
                    for (int k = 0; k < ac && as + 4 <= len; k++, as += 4)
                        if (v[as] == 0x00 && v[as+1] == 0x0F && v[as+2] == 0xAC && v[as+3] == 8)
                            has_sae = 1;
                }
            }
        } else if (tag == 221 && len >= 4 &&  // vendor: WPA (Microsoft OUI 00-50-F2 type 1)
                   v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2 && v[3] == 0x01) {
            has_wpa = 1;
        }
        o += 2 + len;
    }
    int sec = SEC_OPEN;
    if (has_sae) sec = SEC_WPA3;
    else if (has_rsn) sec = SEC_WPA2;
    else if (has_wpa) sec = SEC_WPA;
    else if (privacy) sec = SEC_WEP;

    uint8_t ch = ds_ch ? ds_ch : hop_ch;
    // PHASE 4c LO-MAP DIAGNOSTIC: if a beacon that advertises channel ds_ch decodes
    // while we are TUNED to hop_ch and they differ, the radio's real RX frequency
    // is closer to ds_ch than to hop_ch => the LO did not relock to the commanded
    // channel (image/offset). Logging (tuned -> advertised) builds the offset map.
    if (ds_ch && hop_ch && ds_ch != hop_ch) {
        kprintf("[RTL8812BU]  LO-MAP: tuned ch%d but decoded a ch%d beacon (%02x:%02x:%02x:%02x:%02x:%02x rssi %d)\n",
                hop_ch, ds_ch, bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], rssi);
        bootlog_write("[RTL8812BU] LO-MAP tuned%d decoded%d", hop_ch, ds_ch);
    }
    bss_t *b = rtl_bss_find(bssid);
    if (!b) {
        if (g_bss_count >= WIFI_MAX_BSS) return 0;
        b = &g_bss[g_bss_count++];
        memset(b, 0, sizeof(*b));
        memcpy(b->bssid, bssid, 6);
    }
    // update (keep strongest RSSI seen)
    if (ssid_len > 0 || b->ssid_len == 0) {
        b->ssid_len = (uint8_t)ssid_len;
        memcpy(b->ssid, ssid, ssid_len + 1);
    }
    b->channel = ch;
    if (rssi > b->rssi || b->rssi == 0) b->rssi = rssi;
    b->sec = (uint8_t)sec;
    return 1;
}

// Walk one bulk-IN buffer of concatenated [RXDESC][drvinfo][shift][802.11]
// units (zero-padded tail); dispatch each management frame to the parser.
static int rtl_rx_parse_buffer(rtl_dev_t *d, const uint8_t *buf, uint32_t cap, uint8_t hop_ch) {
    (void)d;
    int parsed = 0;
    uint32_t o = 0;
    while (o + RXDESC_SIZE_8822B <= cap) {
        const uint8_t *p = buf + o;
        uint32_t pkt_len = RXD_PKT_LEN(p);
        if (pkt_len == 0) break;                       // end of received data
        uint32_t drvinfo = RXD_DRVINFO(p);
        uint32_t shift = RXD_SHIFT(p);
        uint32_t hdr = RXDESC_SIZE_8822B + drvinfo + shift;
        uint32_t total = hdr + pkt_len;
        if (o + total > cap) break;                    // truncated
        if (!RXD_C2H(p) && !RXD_CRC32(p) && !RXD_ICVERR(p)) {
            int rssi = -100;
            // drvinfo carries the PHY-status report; use it whenever present
            // (the RXDESC PHYST bit reads 0 on this cut even with drvinfo set).
            if (drvinfo >= 8)
                rssi = rtl_phy_rssi(p + RXDESC_SIZE_8822B, (uint8_t)RXD_RATE(p));
            parsed += rtl_parse_mgmt(p + hdr, pkt_len, rssi, hop_ch);
        }
        // packets are 8-byte aligned in the aggregation buffer.
        o += (total + 7) & ~7u;
    }
    return parsed;
}

// =============================================================================
// (4) PHASE 4 - 802.11 MLME: open-system authentication + association to a BSS.
//
// The reserved-page firmware download proved the 8822B TX descriptor + bulk-OUT
// path (phase 1b). Here we build REAL over-the-air 802.11 management frames (auth
// request, association request), wrap each in a 48-byte TX descriptor for the
// MGNT queue (QSEL=0x12 -> HW priority-queue map MG=HQ -> USB high out-pipe =
// ep_out_list[0], the same pipe the beacon reserved-page used), and transmit over
// bulk-OUT. Then we poll the RX path for the AP's auth/assoc responses.
//
// TXDESC bit-field offsets are the halmac SET_TX_DESC_*_8822B macros
// (halmac_tx_desc_nic.h): each is SET_BITS_TO_LE_4BYTE(txdesc+byte, shift, len).
// =============================================================================

// Set a little-endian bit-field inside the TX descriptor (halmac SET_BITS macro).
static void td_set(uint8_t *td, int byteoff, int shift, int len, uint32_t val) {
    uint32_t w = rd_le32(td + byteoff);
    uint32_t mask = ((len >= 32) ? 0xFFFFFFFFu : ((1u << len) - 1u)) << shift;
    w = (w & ~mask) | ((val << shift) & mask);
    td[byteoff + 0] = w & 0xFF;         td[byteoff + 1] = (w >> 8) & 0xFF;
    td[byteoff + 2] = (w >> 16) & 0xFF; td[byteoff + 3] = (w >> 24) & 0xFF;
}

static uint8_t g_mlme_bssid[6];

// Build the 48-byte MGNT TX descriptor in front of an 802.11 frame of length
// flen. Faithful to rtl8822bu_xmit.c update_txdesc()'s MGNT_FRAMETAG branch: a
// non-QoS management frame uses HW sequence numbering (EN_HWSEQ), a fixed 1M
// data rate (USE_RATE) with fallback disabled, and the DW7 descriptor checksum.
static void rtl_build_mgmt_txdesc(uint8_t *td, uint32_t flen) {
    memset(td, 0, TX_DESC_SIZE);
    // DW0 (0x00): pkt size, descriptor offset, last-segment, dis-QSEL-seq.
    td_set(td, 0x00,  0, 16, flen);            // TXPKTSIZE = 802.11 frame length
    td_set(td, 0x00, 16,  8, TX_DESC_SIZE);    // OFFSET = 48-byte descriptor
    td_set(td, 0x00, 26,  1, 1);               // LS (last segment)
    td_set(td, 0x00, 31,  1, 1);               // DISQSELSEQ (non-QoS -> HW seq)
    // DW1 (0x04): MACID, QSEL=MGNT, RATE_ID.
    td_set(td, 0x04,  0,  7, 0);               // MACID 0
    td_set(td, 0x04,  8,  5, 0x12);            // QSEL = QSLT_MGNT
    td_set(td, 0x04, 16,  5, 0);               // RATE_ID
    // DW3 (0x0C): use a fixed rate; disable data fallback.
    td_set(td, 0x0C,  8,  1, 1);               // USE_RATE
    td_set(td, 0x0C, 10,  1, 1);               // DISDATAFB
    // DW4 (0x10): data rate = DESC_RATE1M (0 = 1 Mbps CCK, most robust) + retry.
    td_set(td, 0x10,  0,  7, 0x00);            // DATARATE = 1M
    td_set(td, 0x10, 17,  1, 1);               // RTY_LMT_EN
    td_set(td, 0x10, 18,  6, 12);              // RTS_DATA_RTY_LMT = 12
    // DW6 (0x18): SW_DEFINE (DriverFixedRate marker, matches reference).
    td_set(td, 0x18,  0, 12, 0x01);            // SW_DEFINE |= 0x01
    // DW8 (0x20): HW sequence numbering (outside the DW0..DW7 checksum region).
    td_set(td, 0x20, 15,  1, 1);               // EN_HWSEQ
    // DW7 (0x1C): 16-bit descriptor checksum over the first 32 bytes.
    rtl_fill_txdesc_chksum(td);
}

// Transmit one 802.11 management frame over the air (MGNT queue / high out-pipe).
static int rtl_tx_mgmt(rtl_dev_t *d, const uint8_t *frame, uint32_t flen) {
    if (flen + TX_DESC_SIZE + 1 > sizeof(txbuf)) return -1;
    rtl_build_mgmt_txdesc(txbuf, flen);
    memcpy(txbuf + TX_DESC_SIZE, frame, flen);
    // USB bulk boundary: a length that is an exact multiple of 512 needs a
    // terminating short packet, so append one dummy pad byte (not counted in
    // TXPKTSIZE - the MAC uses OFFSET+TXPKTSIZE to bound the packet).
    uint32_t total = TX_DESC_SIZE + flen;
    uint32_t pad = ((total & 511) == 0) ? 1 : 0;
    if (pad) txbuf[total] = 0;
    int r = xhci_bulk_transfer(d->xhc, d->slot_id, d->ep_out, txbuf, total + pad, 0);
    kprintf("[RTL8812BU]  MGMT TX bulk-OUT ep=0x%02x len=%u -> r=%d %s\n",
            d->ep_out, total + pad, r, (r < 0) ? "FAILED" : "sent");
    return r < 0 ? -1 : 0;
}

// Build an open-system authentication request (transaction seq 1).
static uint32_t rtl_build_auth_req(uint8_t *f, const uint8_t *bssid, const uint8_t *sa) {
    uint32_t n = 0;
    f[0] = 0xB0; f[1] = 0x00;                  // FC: mgmt / auth, no flags
    f[2] = 0x00; f[3] = 0x00;                  // duration (HW fills)
    memcpy(f + 4,  bssid, 6);                  // addr1 = RA  = AP
    memcpy(f + 10, sa,    6);                  // addr2 = TA  = us
    memcpy(f + 16, bssid, 6);                  // addr3 = BSSID
    f[22] = 0x00; f[23] = 0x00;                // seq ctrl (HW fills)
    n = 24;
    f[n++] = 0x00; f[n++] = 0x00;              // auth algorithm = 0 (open system)
    f[n++] = 0x01; f[n++] = 0x00;              // transaction sequence = 1
    f[n++] = 0x00; f[n++] = 0x00;              // status code = 0
    return n;
}

// Build an association request (SSID + supported-rates + HT-cap IEs).
static uint32_t rtl_build_assoc_req(uint8_t *f, const uint8_t *bssid, const uint8_t *sa,
                                    const char *ssid, int ssid_len, int privacy) {
    uint32_t n = 0;
    f[0] = 0x00; f[1] = 0x00;                  // FC: mgmt / assoc-req
    f[2] = 0x00; f[3] = 0x00;                  // duration
    memcpy(f + 4,  bssid, 6);                  // addr1 = AP
    memcpy(f + 10, sa,    6);                  // addr2 = us
    memcpy(f + 16, bssid, 6);                  // addr3 = BSSID
    f[22] = 0x00; f[23] = 0x00;                // seq ctrl
    n = 24;
    uint16_t cap = 0x0001 | 0x0020 | 0x0400;   // ESS | ShortPreamble | ShortSlotTime
    if (privacy) cap |= 0x0010;                // Privacy (WEP/WPA APs advertise it)
    f[n++] = cap & 0xFF; f[n++] = (cap >> 8) & 0xFF;
    f[n++] = 0x0A; f[n++] = 0x00;              // listen interval = 10
    // SSID element (tag 0)
    if (ssid_len > 32) ssid_len = 32;
    f[n++] = 0; f[n++] = (uint8_t)ssid_len;
    for (int i = 0; i < ssid_len; i++) f[n++] = (uint8_t)ssid[i];
    // Supported rates (tag 1): 1,2,5.5,11 (CCK basic) + 6,9,12,18 (OFDM)
    static const uint8_t rates[8] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };
    f[n++] = 1; f[n++] = 8; memcpy(f + n, rates, 8); n += 8;
    // Extended supported rates (tag 50): 24,36,48,54
    static const uint8_t erates[4] = { 0x30, 0x48, 0x60, 0x6c };
    f[n++] = 50; f[n++] = 4; memcpy(f + n, erates, 4); n += 4;
    // HT capabilities (tag 45, len 26): advertise 802.11n, 20MHz, MCS0-15.
    f[n++] = 45; f[n++] = 26;
    f[n++] = 0x2c; f[n++] = 0x01;              // HT cap info (SGI-20, etc.)
    f[n++] = 0x1b;                             // A-MPDU params (max len, density)
    f[n++] = 0xff; f[n++] = 0xff;              // Rx MCS bitmap: MCS0..15
    for (int i = 0; i < 14; i++) f[n++] = 0;   // rest of MCS set + ext/txbf/asel
    return n;
}

// Poll the RX path for a specific management response from our BSS. Returns 1 if
// a matching frame was seen; fills *status (and *aid for assoc-resp). want_sub is
// the 802.11 subtype (ST_AUTH / ST_ASSOC_RESP).
static int g_mlme_beacons;                     // beacons heard from BSSID while waiting
static int rtl_mlme_rx_wait(rtl_dev_t *d, uint8_t want_sub, int want_authseq,
                            int *status, int *aid) {
    // Each empty bulk-IN read blocks for the xHCI 5s transfer timeout, but when we
    // are actually on the AP's frequency the RX FIFO is never empty (beacons +
    // data from a -22 dBm AP), so reads return immediately with a full 4 KB burst
    // and we get many frames per read. want_sub=0xFF is used by the caller as a
    // "prime/listen only" mode (never matches a real mgmt subtype 0..15), which
    // just drains the FIFO and populates g_mlme_beacons.
    int reads = 12;
    g_mlme_beacons = 0;
    for (int a = 0; a < reads; a++) {
        memset(rx_buf, 0, 4096);
        int r = xhci_bulk_transfer(d->xhc, d->slot_id, d->ep_in & 0x0F, rx_buf, 4096, 1);
        if (r != CC_SUCCESS && r != CC_SHORT_PACKET) continue;
        uint32_t o = 0;
        while (o + RXDESC_SIZE_8822B <= 4096) {
            const uint8_t *p = rx_buf + o;
            uint32_t pkt_len = RXD_PKT_LEN(p);
            if (pkt_len == 0) break;
            uint32_t drvinfo = RXD_DRVINFO(p), shift = RXD_SHIFT(p);
            uint32_t hdr = RXDESC_SIZE_8822B + drvinfo + shift;
            uint32_t total = hdr + pkt_len;
            if (o + total > 4096) break;
            if (!RXD_C2H(p) && !RXD_CRC32(p) && pkt_len >= 24) {
                const uint8_t *f = p + hdr;
                uint8_t fc0 = f[0];
                uint8_t type = (fc0 >> 2) & 0x3, sub = (fc0 >> 4) & 0xF;
                // Count beacons from our BSS while waiting: confirms we are on the
                // right channel and still hearing the AP during the exchange.
                if (type == 0 && sub == 8 && memcmp(f + 16, g_mlme_bssid, 6) == 0)
                    g_mlme_beacons++;
                // addr2 (source) == BSSID and addr1 (dest) == us.
                if (type == 0 && sub == want_sub && memcmp(f + 10, g_mlme_bssid, 6) == 0) {
                    if (sub == ST_AUTH) {
                        int algo = f[24] | (f[25] << 8);
                        int seq  = f[26] | (f[27] << 8);
                        int st   = f[28] | (f[29] << 8);
                        if (want_authseq == 0 || seq == want_authseq) {
                            kprintf("[RTL8812BU]  AUTH-RESP algo=%d seq=%d status=%d\n", algo, seq, st);
                            *status = st; return 1;
                        }
                    } else if (sub == ST_ASSOC_RESP) {
                        int st   = f[26] | (f[27] << 8);
                        int a_id = (f[28] | (f[29] << 8)) & 0x3FFF;
                        kprintf("[RTL8812BU]  ASSOC-RESP status=%d aid=%d\n", st, a_id);
                        *status = st; *aid = a_id; return 1;
                    }
                }
            }
            o += (total + 7) & ~7u;
        }
    }
    return 0;
}

// Run the full open-system MLME join against one BSS: set channel, program the
// BSSID + station media state, then AUTH (seq1->seq2) and ASSOC (get the AID).
// Reaching the ASSOCIATED state (AID assigned, status 0) is the phase-4 goal;
// the WPA2 4-way key exchange is the separate phase 5.
static uint8_t mlme_frame[512];
static void rtl_iqk_run(rtl_dev_t *d);   // fwd decl (defined with the IQK block)
static int rtl_mlme_connect(rtl_dev_t *d, bss_t *b) {
    memcpy(g_mlme_bssid, b->bssid, 6);
    kprintf("\n[RTL8812BU] === PHASE 4: MLME join %02x:%02x:%02x:%02x:%02x:%02x "
            "\"%s\" ch%d %s ===\n",
            b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4], b->bssid[5],
            b->ssid_len ? b->ssid : "<hidden>", b->channel, sec_name(b->sec));
    bootlog_write("[RTL8812BU] MLME JOIN %02x:%02x:%02x:%02x:%02x:%02x ch%d %s",
                  b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3], b->bssid[4],
                  b->bssid[5], b->channel, b->ssid_len ? b->ssid : "hidden");
    if (d->wifi) wifi_core_set_state(d->wifi, WIFI_STATE_AUTHENTICATING);

    // PHASE 4d WORKAROUND (clearly logged as such): the phase-4c scan proved the
    // receive LO tunes ~50 MHz (+10 channels) ABOVE the commanded channel - the
    // ch11 APs decode at FULL strength (-22 dBm, RXPKT_NUM=0x5f) ONLY when we
    // command ch1 (LO+50MHz = 2462 = ch11), and ZERO on commanded ch11. The
    // radioa RF table is byte-identical to the reference and RF 0x18 tracks the
    // commanded channel, so the true synth-register fix is a follow-up; here we
    // subtract 10 channels from the join command so BOTH TX and RX land on the
    // AP's real frequency, and give a proper post-tune dwell (the p4c attempt
    // failed only on too-short dwell + a USB-RX-DMA re-init stall).
    uint8_t cmd_ch = (b->channel > 10) ? (uint8_t)(b->channel - 10) : b->channel;
    int st0 = -1, aid0 = 0;
    kprintf("[RTL8812BU]  LO-OFFSET WORKAROUND join: target real ch%d -> commanding "
            "ch%d (radio LO sits +50MHz = on target)\n", b->channel, cmd_ch);
    bootlog_write("[RTL8812BU] JOIN target%d cmd%d (workaround -10ch)", b->channel, cmd_ch);
    rtl_set_channel(d, cmd_ch);
    xhci_delay_ms(30);
    rtl_iqk_run(d);
    rtl_config_trx_mode(d);
    phydm_igi_toggle(d);
    xhci_delay_ms(20);

    // Program the BSSID so TX frames are addressed to this BSS and the address-
    // match / ACK engine is pointed at it. Keep the scan-promiscuous RCR so the
    // AP's unicast auth/assoc responses are always delivered to our RX FIFO.
    for (int i = 0; i < 6; i++) rtl_w8(d, (uint16_t)(REG_BSSID + i), b->bssid[i]);

    // PRIME + on-frequency confirmation (BEFORE touching MSR): drain the RX FIFO
    // at the offset channel and count beacons from the target BSS. If we hear the
    // target here, the workaround tune is correct and the join RX is alive (the
    // p4c stall is gone). want_sub=0xFF => listen-only.
    { uint32_t rxpkt = rtl_r32(d, 0x0284);
      kprintf("[RTL8812BU]  join tuned cmd-ch%d: RF-A18=%05x RXPKT_NUM=0x%08x (target real ch%d)\n",
              cmd_ch, rf_read(d, 0, 0x18, RFREGOFFSETMASK), rxpkt, b->channel); }
    rtl_mlme_rx_wait(d, 0xFF, 0, &st0, &aid0);
    kprintf("[RTL8812BU]  on-freq PRIME (pre-MSR): heard %d beacon(s) from target at "
            "cmd-ch%d\n", g_mlme_beacons, cmd_ch);
    bootlog_write("[RTL8812BU] PRIME preMSR beacons=%d", g_mlme_beacons);

    // Now set port0 to STATION/infra so the HW auto-ACK engine ACKs the AP.
    uint8_t msr = (uint8_t)((rtl_r8(d, REG_MSR) & 0x0C) | MSR_INFRA);
    rtl_w8(d, REG_MSR, msr);
    // Diagnostic: confirm setting MSR did NOT stall RX (p4c suspected it).
    rtl_mlme_rx_wait(d, 0xFF, 0, &st0, &aid0);
    kprintf("[RTL8812BU]  joined: BSSID set, MSR=0x%02x (port0=STATION), src MAC "
            "%02x:%02x:%02x:%02x:%02x:%02x; post-MSR heard %d beacon(s)\n",
            rtl_r8(d, REG_MSR), d->mac[0], d->mac[1], d->mac[2], d->mac[3],
            d->mac[4], d->mac[5], g_mlme_beacons);
    bootlog_write("[RTL8812BU] PRIME postMSR beacons=%d", g_mlme_beacons);

    // ---- open-system AUTH (up to 4 attempts) ----
    int auth_ok = 0, status = -1, aid = 0;
    for (int attempt = 1; attempt <= 3 && !auth_ok; attempt++) {
        uint32_t flen = rtl_build_auth_req(mlme_frame, b->bssid, d->mac);
        kprintf("[RTL8812BU]  -> AUTH req (open, seq1) attempt %d, %u bytes\n", attempt, flen);
        if (rtl_tx_mgmt(d, mlme_frame, flen) != 0) { proc_sleep(20); continue; }
        if (rtl_mlme_rx_wait(d, ST_AUTH, 2, &status, &aid)) {
            if (status == 0) auth_ok = 1;
            else { kprintf("[RTL8812BU]  AUTH rejected status=%d\n", status); break; }
        } else {
            kprintf("[RTL8812BU]  AUTH attempt %d: no response (heard %d beacon(s) "
                    "from BSS while waiting)\n", attempt, g_mlme_beacons);
            proc_sleep(30);
        }
    }
    bootlog_write("[RTL8812BU] AUTH %s status=%d", auth_ok ? "OK" : "FAIL", status);
    if (!auth_ok) {
        kprintf("[RTL8812BU] === PHASE 4: authentication FAILED (no auth-resp / rejected) ===\n");
        if (d->wifi) wifi_core_set_state(d->wifi, WIFI_STATE_INIT);
        return -1;
    }
    kprintf("[RTL8812BU]  AUTHENTICATED (open system, status 0)\n");

    // ---- ASSOCIATION (up to 4 attempts) ----
    int assoc_ok = 0; status = -1; aid = 0;
    int privacy = (b->sec != SEC_OPEN);
    for (int attempt = 1; attempt <= 3 && !assoc_ok; attempt++) {
        uint32_t flen = rtl_build_assoc_req(mlme_frame, b->bssid, d->mac,
                                            b->ssid, b->ssid_len, privacy);
        kprintf("[RTL8812BU]  -> ASSOC req attempt %d, %u bytes (SSID \"%s\")\n",
                attempt, flen, b->ssid_len ? b->ssid : "");
        if (rtl_tx_mgmt(d, mlme_frame, flen) != 0) { proc_sleep(20); continue; }
        if (rtl_mlme_rx_wait(d, ST_ASSOC_RESP, 0, &status, &aid)) {
            if (status == 0) assoc_ok = 1;
            else { kprintf("[RTL8812BU]  ASSOC rejected status=%d\n", status); break; }
        } else {
            kprintf("[RTL8812BU]  ASSOC attempt %d: no response (heard %d beacon(s) "
                    "from BSS while waiting)\n", attempt, g_mlme_beacons);
            proc_sleep(30);
        }
    }
    bootlog_write("[RTL8812BU] ASSOC %s status=%d aid=%d", assoc_ok ? "OK" : "FAIL", status, aid);
    if (!assoc_ok) {
        kprintf("[RTL8812BU] === PHASE 4: association FAILED (auth OK, no/failed assoc-resp) ===\n");
        if (d->wifi) wifi_core_set_state(d->wifi, WIFI_STATE_INIT);
        return -2;
    }

    // ASSOCIATED. Keep the MAC in station/infra media-connect state.
    if (d->wifi) wifi_core_set_state(d->wifi, WIFI_STATE_4WAY);   // assoc done; WPA2 next
    kprintf("[RTL8812BU] === PHASE 4 SUCCESS: ASSOCIATED to \"%s\" AID=%d "
            "(media-connect STATION). %s ===\n",
            b->ssid_len ? b->ssid : "<hidden>", aid,
            (b->sec == SEC_OPEN) ? "OPEN - fully connected at L2"
                                 : "WPA2/privacy - 4-way handshake is phase 5");
    bootlog_write("[RTL8812BU] ASSOCIATED aid=%d sec=%s", aid, sec_name(b->sec));
    return 0;
}

// Choose a scan target. PHASE 4d WORKAROUND: because the radio LO tunes +50 MHz
// (+10 ch) above the commanded channel, only APs on ch 11-14 are actually
// REACHABLE - commanding (real_ch - 10) => ch 1-4 puts the LO exactly on the AP
// frequency for BOTH TX and RX. An AP on ch 1-10 cannot be tuned (it would need a
// negative command channel), so it is NOT a valid workaround target even if it is
// the strongest BSS (it only leaks into the ch1-command passband). We therefore
// require an offset-reachable channel (>=11) and, among those, prefer non-WPA3
// (open-system 802.11 AUTH reaches ASSOCIATED against OPEN/WEP/WPA2; WPA3-SAE may
// reject open auth), then strongest RSSI.
#define OFFSET_REACHABLE(ch)  ((ch) >= 11 && (ch) <= 14)
static bss_t *rtl_pick_target(void) {
    bss_t *best = 0;
    // pass 1: offset-reachable (ch11-14), non-WPA3, named SSID -> strongest
    for (int i = 0; i < g_bss_count; i++) {
        bss_t *b = &g_bss[i];
        if (OFFSET_REACHABLE(b->channel) && b->sec != SEC_WPA3 && b->ssid_len > 0)
            if (!best || b->rssi > best->rssi) best = b;
    }
    if (best) return best;
    // pass 2: any offset-reachable named BSS (incl. WPA3 - 802.11 auth may still work)
    for (int i = 0; i < g_bss_count; i++) {
        bss_t *b = &g_bss[i];
        if (OFFSET_REACHABLE(b->channel) && b->ssid_len > 0)
            if (!best || b->rssi > best->rssi) best = b;
    }
    if (best) return best;
    // pass 3: fall back to any named BSS on a valid channel (may be unreachable
    // under the +50 MHz offset, but lets the MLME path still be exercised).
    for (int i = 0; i < g_bss_count; i++) {
        bss_t *b = &g_bss[i];
        if (b->ssid_len > 0 && b->channel >= 1 && b->channel <= 14)
            if (!best || b->rssi > best->rssi) best = b;
    }
    return best;
}

// =============================================================================
// PHASE 4b: 8822B per-path IQK (IQ imbalance / LO-offset calibration).
// Faithful port of halrf_iqk_8822b.c (morrownr/88x2bu, GPL). The 8822B runs the
// LOK/TXK/RXK one-shots inside the in-BB 0x1b00 "NCTL" block (no separate ucode).
// Specialised for 2.4GHz / 20MHz (band_type=2.4G, band_width=0), 2T2R (path A+B).
// All 20000-iteration us-delay poll loops in the reference are capped to a small
// USB-realistic bound (each register access is already a >=1ms VENQT round-trip,
// so a HW cal that completes in microseconds returns after 1-3 reads; the cap
// only guards against a never-asserting status bit hanging the scan thread).
// The calibration programs the HW's internal RX/TX CFIR correction filter; the
// "apply" bits (0xc94/0xe94, set in configure_macbb, cleared per-path only on
// failure) leave that correction engaged after the BB/MAC/RF backup is restored.
// =============================================================================
#define IQK_TXIQK  0
#define IQK_RXIQK1 1
#define IQK_RXIQK2 2

typedef struct {
    uint32_t tmp_gntwl;
    uint8_t  tmp1bcc;
    uint8_t  lna_idx;
    int      isbnd;
    uint8_t  iqk_step;      // 1..7 master state
    uint8_t  rxiqk_step;    // 1..5 RXIQK sub-state
    uint8_t  kcount;
    int      lok_fail[2];
    int      iqk_fail_report[2][2];   // [path][0=TX,1=RX]
    uint8_t  retry_count[2][3];       // [path][TXIQK/RXIQK1/RXIQK2]
    uint8_t  gs_retry_count[2][2];    // [path][GS1/GS2]
    uint8_t  rxiqk_fail_code[2];      // [path]
} iqk_state_t;
static iqk_state_t g_iqk;

// LTE-coex indirect read/write (0x1700 window) - used to force GNT_WL during cal.
// Poll caps hard-bounded (no BT on this dongle => ready bit may never assert).
static uint32_t iqk_ltec_read(rtl_dev_t *d, uint16_t reg_addr) {
    int j = 0;
    bb_w32(d, 0x1700, 0x800f0000 | reg_addr);
    do { j++; } while (((rtl_r8(d, 0x1703) & BIT(5)) == 0) && (j < 48));
    return bb_r32(d, 0x1708);
}
static void iqk_ltec_write(rtl_dev_t *d, uint16_t reg_addr, uint32_t bit_mask, uint32_t reg_value) {
    int j = 0;
    if (bit_mask == 0x0) return;
    if (bit_mask == 0xffffffff) {
        bb_w32(d, 0x1704, reg_value);
        do { j++; } while (((rtl_r8(d, 0x1703) & BIT(5)) == 0) && (j < 48));
        bb_w32(d, 0x1700, 0xc00f0000 | reg_addr);
    } else {
        uint32_t val; int i, bitpos = 0;
        for (i = 0; i <= 31; i++) { if ((bit_mask >> i) & 0x1) { bitpos = i; break; } }
        val = iqk_ltec_read(d, reg_addr);
        val = (val & (~bit_mask)) | (reg_value << bitpos);
        bb_w32(d, 0x1704, val);
        do { j++; } while (((rtl_r8(d, 0x1703) & BIT(5)) == 0) && (j < 48));
        bb_w32(d, 0x1700, 0xc00f0000 | reg_addr);
    }
}

static void iqk_rf_set_check(rtl_dev_t *d, uint8_t path, uint16_t add, uint32_t data) {
    int i;
    rf_write(d, path, add, data);
    for (i = 0; i < 20; i++) {
        if (rf_read(d, path, add, RFREGOFFSETMASK) == data) break;
        rf_write(d, path, add, data);
    }
}
static void iqk_set_rf0x8(rtl_dev_t *d, uint8_t path) {
    int c = 0;
    while (c < 64) {
        rf_write(d, path, 0xef, 0x0);
        rf_write(d, path, 0x8, 0x0);
        if (rf_read(d, path, 0x8, RFREGOFFSETMASK) == 0x0) break;
        c++;
    }
}
static void iqk_rf0xb0_workaround(rtl_dev_t *d) {
    rf_write(d, 0, 0xb8, 0x00a00);
    rf_write(d, 0, 0xb8, 0x80a00);
}
static void iqk_0xc94_workaround(rtl_dev_t *d) {
    if (bb_get(d, 0xc94, BIT(0)) == 0x1) { bb_set(d, 0xc94, BIT(0), 0x0); bb_set(d, 0xc94, BIT(0), 0x1); }
    if (bb_get(d, 0xe94, BIT(0)) == 0x1) { bb_set(d, 0xe94, BIT(0), 0x0); bb_set(d, 0xe94, BIT(0), 0x1); }
}
static void iqk_agc_bnd_int(rtl_dev_t *d) {
    bb_w32(d, 0x1b00, 0xf8000008);
    bb_w32(d, 0x1b00, 0xf80a7008);
    bb_w32(d, 0x1b00, 0xf8015008);
    bb_w32(d, 0x1b00, 0xf8000008);
}
static void iqk_bb_reset(rtl_dev_t *d) {
    int cca_ing, count = 0;
    uint32_t bit_mask = (BIT(27) | BIT(26) | BIT(25) | BIT(24));
    rf_write(d, 0, 0x0, 0x10000);
    rf_write(d, 1, 0x0, 0x10000);
    bb_set(d, 0x8f8, 0x0ff00000, 0x0);
    while (1) {
        bb_w32(d, 0x8fc, 0x0);
        bb_set(d, 0x198c, 0x7, 0x7);
        cca_ing = (int)bb_get(d, 0xfa0, BIT(3));
        if (count > 80) cca_ing = 0;
        if (cca_ing) { count++; }
        else {
            rtl_w8(d, 0x808, 0x0);
            bb_set(d, 0xa04, bit_mask, 0x0);
            bb_set(d, 0x0, BIT(16), 0x0);
            bb_set(d, 0x0, BIT(16), 0x1);
            if (bb_get(d, 0x660, BIT(16))) bb_w32(d, 0x6b4, 0x89000006);
            break;
        }
    }
}
static void iqk_afe_setting(rtl_dev_t *d, int do_iqk) {
    if (do_iqk) {
        bb_w32(d, 0xc60, 0x50000000); bb_w32(d, 0xc60, 0x70070040);
        bb_w32(d, 0xe60, 0x50000000); bb_w32(d, 0xe60, 0x70070040);
        bb_w32(d, 0xc58, 0xd8000402); bb_w32(d, 0xc5c, 0xd1000120); bb_w32(d, 0xc6c, 0x00000a15);
        bb_w32(d, 0xe58, 0xd8000402); bb_w32(d, 0xe5c, 0xd1000120); bb_w32(d, 0xe6c, 0x00000a15);
        iqk_bb_reset(d);
    } else {
        bb_w32(d, 0xc60, 0x50000000); bb_w32(d, 0xc60, 0x70038040);
        bb_w32(d, 0xe60, 0x50000000); bb_w32(d, 0xe60, 0x70038040);
    }
    bb_set(d, 0x9a4, BIT(31), 0x0);
}
static void iqk_rfe_setting(rtl_dev_t *d) {
    // ext_pa off (iFEM)
    bb_w32(d, 0xcb0, 0x77777777); bb_w32(d, 0xcb4, 0x00007777); bb_w32(d, 0xcbc, 0x00000100);
    bb_w32(d, 0xeb0, 0x77777777); bb_w32(d, 0xeb4, 0x00007777); bb_w32(d, 0xebc, 0x00000100);
}
static void iqk_rf_setting(rtl_dev_t *d) {
    uint8_t path;
    uint32_t tmp;
    bb_w32(d, 0x1b00, 0xf8000008);
    bb_w32(d, 0x1bb8, 0x00000000);
    for (path = 0; path < 2; path++) {
        tmp = rf_read(d, path, 0xdf, RFREGOFFSETMASK);
        tmp = (tmp & (~BIT(4))) | BIT(1) | BIT(11);
        iqk_rf_set_check(d, path, 0xdf, tmp);
        rf_write(d, path, 0x65, 0x09000);
        // 2.4G branch
        rf_write_mask(d, path, 0xef, BIT(19), 0x1);
        rf_write(d, path, 0x33, 0x00026);
        rf_write(d, path, 0x3e, 0x00037);
        rf_write(d, path, 0x3f, 0x5efce);
        rf_write_mask(d, path, 0xef, BIT(19), 0x0);
    }
}
static void iqk_configure_macbb(rtl_dev_t *d) {
    rtl_w8(d, 0x522, 0x7f);
    bb_set(d, 0x550, BIT(11) | BIT(3), 0x0);
    bb_set(d, 0x90c, BIT(15), 0x1);
    bb_set(d, 0xc94, BIT(0), 0x1);
    bb_set(d, 0xe94, BIT(0), 0x1);
    bb_set(d, 0xc94, (BIT(11) | BIT(10)), 0x1);
    bb_set(d, 0xe94, (BIT(11) | BIT(10)), 0x1);
    bb_w32(d, 0xc00, 0x00000004);
    bb_w32(d, 0xe00, 0x00000004);
    bb_set(d, 0xb00, BIT(8), 0x0);
    bb_set(d, 0x808, BIT(28), 0x0);
    bb_set(d, 0x838, BIT(3) | BIT(2) | BIT(1), 0x7);
}
static void iqk_lok_setting(rtl_dev_t *d, uint8_t path) {
    bb_w32(d, 0x1b00, 0xf8000008 | path << 1);
    bb_w32(d, 0x1bcc, 0x9);
    rtl_w8(d, 0x1b23, 0x00);
    // 2.4G
    rtl_w8(d, 0x1b2b, 0x00);
    rf_write(d, path, 0x56, 0x50df2);
    rf_write(d, path, 0x8f, 0xadc00);
    rf_write_mask(d, path, 0xef, BIT(4), 0x1);
    rf_write_mask(d, path, 0x33, BIT(1) | BIT(0), 0x0);
}
static void iqk_txk_setting(rtl_dev_t *d, uint8_t path) {
    bb_w32(d, 0x1b00, 0xf8000008 | path << 1);
    bb_w32(d, 0x1bcc, 0x9);
    bb_w32(d, 0x1b20, 0x01440008);
    if (path == 0x0) bb_w32(d, 0x1b00, 0xf800000a);
    else             bb_w32(d, 0x1b00, 0xf8000008);
    bb_w32(d, 0x1bcc, 0x3f);
    // 2.4G
    rf_write(d, path, 0x56, 0x50df2);
    rf_write(d, path, 0x8f, 0xadc00);
    rtl_w8(d, 0x1b2b, 0x00);
}
static void iqk_rxk1_setting(rtl_dev_t *d, uint8_t path) {
    bb_w32(d, 0x1b00, 0xf8000008 | path << 1);
    // 2.4G
    rtl_w8(d, 0x1bcc, 0x9);
    rtl_w8(d, 0x1b2b, 0x00);
    bb_w32(d, 0x1b20, 0x01450008);
    bb_w32(d, 0x1b24, 0x01460c88);
    rf_write(d, path, 0x56, 0x510e0);
    rf_write(d, path, 0x8f, 0xacc00);
}
static void iqk_rxk2_setting(rtl_dev_t *d, uint8_t path, int is_gs) {
    bb_w32(d, 0x1b00, 0xf8000008 | path << 1);
    // 2.4G
    if (is_gs) g_iqk.tmp1bcc = 0x12;
    rtl_w8(d, 0x1bcc, g_iqk.tmp1bcc);
    rtl_w8(d, 0x1b2b, 0x00);
    bb_w32(d, 0x1b20, 0x01450008);
    bb_w32(d, 0x1b24, 0x01460848);
    rf_write(d, path, 0x56, 0x510e0);
    rf_write(d, path, 0x8f, 0xa9c00);
}
// Poll RF0x8 for cal-done marker 0x12345; cmd 0=LOK (always ok when ready),
// else read fail status from 0x1b08[26]. USB-capped.
static int iqk_check_cal(rtl_dev_t *d, uint8_t path, uint8_t cmd) {
    int notready = 1, fail = 1, cnt = 0;
    while (notready) {
        if (rf_read(d, path, 0x8, RFREGOFFSETMASK) == 0x12345) {
            if (cmd == 0x0) fail = 0;
            else fail = (int)bb_get(d, 0x1b08, BIT(26));
            notready = 0;
        } else {
            cnt++;
        }
        if (cnt >= 200) { fail = 1; break; }
    }
    iqk_set_rf0x8(d, path);
    return fail;
}
static int iqk_rxk_gsearch_fail(rtl_dev_t *d, uint8_t path, uint8_t step) {
    int fail = 1;
    uint32_t IQK_CMD = 0x0, rf_reg0, tmp, bb_idx;
    uint8_t IQMUX[4] = {0x9, 0x12, 0x1b, 0x24};
    uint8_t idx;
    if (step == IQK_RXIQK1) {
        IQK_CMD = 0xf8000208 | (1 << (path + 4));
        iqk_ltec_write(d, 0x38, 0xffff, 0x7700);
        bb_w32(d, 0x1b00, IQK_CMD);
        bb_w32(d, 0x1b00, IQK_CMD + 0x1);
        fail = iqk_check_cal(d, path, 0x1);
        iqk_ltec_write(d, 0x38, MASKDWORD, g_iqk.tmp_gntwl);
    } else if (step == IQK_RXIQK2) {
        for (idx = 0; idx < 4; idx++) { if (g_iqk.tmp1bcc == IQMUX[idx]) break; }
        if (idx == 4) return fail;
        bb_w32(d, 0x1b00, 0xf8000008 | path << 1);
        bb_w32(d, 0x1bcc, g_iqk.tmp1bcc);
        IQK_CMD = 0xf8000308 | (1 << (path + 4));
        iqk_ltec_write(d, 0x38, 0xffff, 0x7700);
        bb_w32(d, 0x1b00, IQK_CMD);
        bb_w32(d, 0x1b00, IQK_CMD + 0x1);
        fail = iqk_check_cal(d, path, 0x1);
        iqk_ltec_write(d, 0x38, MASKDWORD, g_iqk.tmp_gntwl);
        rf_reg0 = rf_read(d, path, 0x0, RFREGOFFSETMASK);
        bb_w32(d, 0x1b00, 0xf8000008 | path << 1);
        tmp = (rf_reg0 & 0x1fe0) >> 5;
        g_iqk.lna_idx = tmp >> 5;
        bb_idx = tmp & 0x1f;
        if (bb_idx == 0x1) {
            if (g_iqk.lna_idx != 0x0) g_iqk.lna_idx--;
            else if (idx != 3) idx++;
            else g_iqk.isbnd = 1;
            fail = 1;
        } else if (bb_idx == 0xa) {
            if (idx != 0) idx--;
            else if (g_iqk.lna_idx != 0x7) g_iqk.lna_idx++;
            else g_iqk.isbnd = 1;
            fail = 1;
        } else {
            fail = 0;
        }
        if (g_iqk.isbnd) fail = 0;
        g_iqk.tmp1bcc = IQMUX[idx];
        if (fail) {
            bb_w32(d, 0x1b00, 0xf8000008 | path << 1);
            tmp = (bb_r32(d, 0x1b24) & 0xffffe3ff) | (g_iqk.lna_idx << 10);
            bb_w32(d, 0x1b24, tmp);
        }
    }
    return fail;
}
static int iqk_lok_one_shot(rtl_dev_t *d, uint8_t path) {
    int LOK_notready;
    uint32_t IQK_CMD = 0xf8000008 | (1 << (4 + path));
    iqk_ltec_write(d, 0x38, 0xffff, 0x7700);
    bb_w32(d, 0x1b00, IQK_CMD);
    bb_w32(d, 0x1b00, IQK_CMD + 1);
    LOK_notready = iqk_check_cal(d, path, 0x0);
    iqk_ltec_write(d, 0x38, MASKDWORD, g_iqk.tmp_gntwl);
    g_iqk.lok_fail[path] = LOK_notready;
    return LOK_notready;
}
static int iqk_one_shot(rtl_dev_t *d, uint8_t path, uint8_t idx) {
    int fail = 1;
    uint32_t IQK_CMD = 0x0, tmp;
    uint16_t iqk_apply[2] = {0xc94, 0xe94};
    const uint8_t band_width = 0; // 20MHz
    if (idx == IQK_TXIQK) {
        IQK_CMD = 0xf8000008 | ((band_width + 4) << 8) | (1 << (path + 4));
    } else if (idx == IQK_RXIQK1) {
        if (band_width == 2) IQK_CMD = 0xf8000808 | (1 << (path + 4));
        else                 IQK_CMD = 0xf8000708 | (1 << (path + 4));
    } else { /* RXIQK2 */
        IQK_CMD = 0xf8000008 | ((band_width + 9) << 8) | (1 << (path + 4));
        bb_w32(d, 0x1b00, 0xf8000008 | path << 1);
        tmp = (bb_r32(d, 0x1b24) & 0xffffe3ff) | ((g_iqk.lna_idx & 0x7) << 10);
        bb_w32(d, 0x1b24, tmp);
    }
    iqk_ltec_write(d, 0x38, 0xffff, 0x7700);
    bb_w32(d, 0x1b00, IQK_CMD);
    bb_w32(d, 0x1b00, IQK_CMD + 0x1);
    fail = iqk_check_cal(d, path, 0x1);
    iqk_ltec_write(d, 0x38, MASKDWORD, g_iqk.tmp_gntwl);
    bb_w32(d, 0x1b00, 0xf8000008 | path << 1);
    if (idx == IQK_TXIQK) {
        if (fail) bb_set(d, iqk_apply[path], BIT(0), 0x0);
    }
    if (idx == IQK_RXIQK2) {
        bb_w32(d, 0x1b38, 0x20000000);
        if (fail) bb_set(d, iqk_apply[path], (BIT(11) | BIT(10)), 0x0);
    }
    if (idx == IQK_TXIQK) g_iqk.iqk_fail_report[path][0] = fail;
    else                  g_iqk.iqk_fail_report[path][1] = fail;
    return fail;
}
// RXIQK sub-state machine (rxiqk_step 1..4 -> 5=done). Returns KFAIL.
static int iqk_rx_iqk_by_path(rtl_dev_t *d, uint8_t path) {
    int KFAIL = 1, gonext;
    switch (g_iqk.rxiqk_step) {
    case 1: /* gain search RXK1 */
        iqk_rxk1_setting(d, path);
        gonext = 0;
        while (1) {
            KFAIL = iqk_rxk_gsearch_fail(d, path, IQK_RXIQK1);
            if (KFAIL && g_iqk.gs_retry_count[path][0] < 2) g_iqk.gs_retry_count[path][0]++;
            else if (KFAIL) { g_iqk.rxiqk_fail_code[path] = 0; g_iqk.rxiqk_step = 5; gonext = 1; }
            else { g_iqk.rxiqk_step++; gonext = 1; }
            if (gonext) break;
        }
        break;
    case 2: /* gain search RXK2 */
        iqk_rxk2_setting(d, path, 1);
        g_iqk.isbnd = 0;
        while (1) {
            KFAIL = iqk_rxk_gsearch_fail(d, path, IQK_RXIQK2);
            if (KFAIL && (g_iqk.gs_retry_count[path][1] < 6 /*rxiqk_gs_limit*/)) g_iqk.gs_retry_count[path][1]++;
            else { g_iqk.rxiqk_step++; break; }
        }
        break;
    case 3: /* RXK1 */
        iqk_rxk1_setting(d, path);
        gonext = 0;
        while (1) {
            KFAIL = iqk_one_shot(d, path, IQK_RXIQK1);
            if (KFAIL && g_iqk.retry_count[path][IQK_RXIQK1] < 2) g_iqk.retry_count[path][IQK_RXIQK1]++;
            else if (KFAIL) { g_iqk.rxiqk_fail_code[path] = 1; g_iqk.rxiqk_step = 5; gonext = 1; }
            else { g_iqk.rxiqk_step++; gonext = 1; }
            if (gonext) break;
        }
        break;
    case 4: /* RXK2 */
        iqk_rxk2_setting(d, path, 0);
        gonext = 0;
        while (1) {
            KFAIL = iqk_one_shot(d, path, IQK_RXIQK2);
            if (KFAIL && g_iqk.retry_count[path][IQK_RXIQK2] < 2) g_iqk.retry_count[path][IQK_RXIQK2]++;
            else if (KFAIL) { g_iqk.rxiqk_fail_code[path] = 2; g_iqk.rxiqk_step = 5; gonext = 1; }
            else { g_iqk.rxiqk_step++; gonext = 1; }
            if (gonext) break;
        }
        break;
    }
    return KFAIL;
}
static void iqk_by_path_subfunction(rtl_dev_t *d, uint8_t path) {
    while (1) {
        iqk_rx_iqk_by_path(d, path);
        if (g_iqk.rxiqk_step == 5) { g_iqk.iqk_step++; g_iqk.rxiqk_step = 1; break; }
    }
    g_iqk.kcount++;
}
static void iqk_by_path(rtl_dev_t *d) {
    int KFAIL;
    while (1) {
        switch (g_iqk.iqk_step) {
        case 1: iqk_lok_setting(d, 0); iqk_lok_one_shot(d, 0); g_iqk.iqk_step++; break;
        case 2: iqk_lok_setting(d, 1); iqk_lok_one_shot(d, 1); g_iqk.iqk_step++; break;
        case 3:
            iqk_txk_setting(d, 0);
            KFAIL = iqk_one_shot(d, 0, IQK_TXIQK); g_iqk.kcount++;
            if (KFAIL && g_iqk.retry_count[0][IQK_TXIQK] < 3) g_iqk.retry_count[0][IQK_TXIQK]++;
            else g_iqk.iqk_step++;
            break;
        case 4:
            iqk_txk_setting(d, 1);
            KFAIL = iqk_one_shot(d, 1, IQK_TXIQK); g_iqk.kcount++;
            if (KFAIL && g_iqk.retry_count[1][IQK_TXIQK] < 3) g_iqk.retry_count[1][IQK_TXIQK]++;
            else g_iqk.iqk_step++;
            break;
        case 5: iqk_by_path_subfunction(d, 0); break;
        case 6: iqk_by_path_subfunction(d, 1); break;
        }
        if (g_iqk.iqk_step == 7) {
            int i;
            for (i = 0; i < 2; i++) {
                bb_w32(d, 0x1b00, 0xf8000008 | i << 1);
                bb_w32(d, 0x1b2c, 0x7);
                bb_w32(d, 0x1bcc, 0x0);
                bb_w32(d, 0x1b38, 0x20000000);
            }
            break;
        }
    }
}
static void iqk_start(rtl_dev_t *d) {
    uint32_t tmp;
    tmp = rf_read(d, 0, 0x1, RFREGOFFSETMASK) | BIT(5) | BIT(0);
    rf_write(d, 0, 0x1, tmp);
    tmp = rf_read(d, 1, 0x1, RFREGOFFSETMASK) | BIT(5) | BIT(0);
    rf_write(d, 1, 0x1, tmp);
    iqk_by_path(d);
}
static void iqk_fill_report(rtl_dev_t *d) {
    uint32_t tmp1 = 0, tmp2 = 0, tmp3 = 0;
    int i;
    for (i = 0; i < 2; i++) {
        tmp1 += ((g_iqk.iqk_fail_report[i][0] & 1) << i);
        tmp2 += ((g_iqk.iqk_fail_report[i][1] & 1) << (i + 4));
        tmp3 += ((g_iqk.rxiqk_fail_code[i] & 0x3) << (i * 2 + 8));
    }
    bb_w32(d, 0x1b00, 0xf8000008);
    bb_set(d, 0x1bf0, 0x0000ffff, tmp1 | tmp2 | tmp3);
}

// NOTE (phase 4b): a faithful port of the 8822B LCK (_phy_lc_calibrate_8822b,
// VCO/LC-tank cal) was tried here and REVERTED. On this dongle the LCK one-shot
// (RF 0x18|BIT15) never self-cleared (timed out), corrupted the RF band bit
// (0x18 came back with BIT16 set on 2.4G), made the first per-channel IQK fail
// (TXK/RXK fail=1/1), and dropped the scan from 3 BSS to 1. LCK standalone is
// harmful without its firmware/precondition context, so it is not run. IQK
// (below) runs clean and passes; the ch11 on-channel-decode blocker is elsewhere.

// Top-level: full LOK + TXK + RXK IQK on the CURRENT channel, both paths.
// Backs up MAC/BB/RF, runs the cal, restores, leaves the CFIR correction applied.
static void rtl_iqk_run(rtl_dev_t *d) {
    uint32_t MAC_backup[2], BB_backup[21], RF_backup[5][2];
    static const uint16_t backup_mac_reg[2] = {0x520, 0x550};
    static const uint16_t backup_bb_reg[21] = {
        0x808, 0x90c, 0xc00, 0xcb0, 0xcb4, 0xcbc, 0xe00, 0xeb0, 0xeb4, 0xebc,
        0x1990, 0x9a4, 0xa04, 0xb00, 0x838, 0xc58, 0xc5c, 0xc6c, 0xe58, 0xe5c, 0xe6c};
    static const uint16_t backup_rf_reg[5] = {0xdf, 0x8f, 0x65, 0x0, 0x1};
    int i;

    memset(&g_iqk, 0, sizeof(g_iqk));
    for (i = 0; i < 2; i++) {
        g_iqk.lok_fail[i] = 1;
        g_iqk.iqk_fail_report[i][0] = 1;
        g_iqk.iqk_fail_report[i][1] = 1;
    }
    g_iqk.tmp1bcc = 0x12;
    g_iqk.iqk_step = 1;
    g_iqk.rxiqk_step = 1;
    g_iqk.kcount = 0;

    // parameter init (_iq_calibrate_8822b_init)
    bb_w32(d, 0x1b10, 0x88011c00);

    // backup GNT_WL + MAC/BB/RF
    g_iqk.tmp_gntwl = iqk_ltec_read(d, 0x38);
    for (i = 0; i < 2; i++)  MAC_backup[i] = rtl_r32(d, backup_mac_reg[i]);
    for (i = 0; i < 21; i++) BB_backup[i]  = bb_r32(d, backup_bb_reg[i]);
    for (i = 0; i < 5; i++) {
        RF_backup[i][0] = rf_read(d, 0, backup_rf_reg[i], RFREGOFFSETMASK);
        RF_backup[i][1] = rf_read(d, 1, backup_rf_reg[i], RFREGOFFSETMASK);
    }

    // configure -> calibrate
    iqk_configure_macbb(d);
    iqk_afe_setting(d, 1);
    iqk_rfe_setting(d);
    iqk_agc_bnd_int(d);
    iqk_rf_setting(d);
    iqk_start(d);
    iqk_afe_setting(d, 0);

    // restore MAC/BB
    for (i = 0; i < 2; i++)  rtl_w32(d, backup_mac_reg[i], MAC_backup[i]);
    for (i = 0; i < 21; i++) bb_w32(d, backup_bb_reg[i], BB_backup[i]);
    // restore RF (0xdf first with B4 clear, checked)
    rf_write(d, 0, 0xef, 0x0);
    rf_write(d, 1, 0xef, 0x0);
    iqk_rf_set_check(d, 0, 0xdf, RF_backup[0][0] & (~BIT(4)));
    iqk_rf_set_check(d, 1, 0xdf, RF_backup[0][1] & (~BIT(4)));
    for (i = 1; i < 5; i++) {
        rf_write(d, 0, backup_rf_reg[i], RF_backup[i][0]);
        rf_write(d, 1, backup_rf_reg[i], RF_backup[i][1]);
    }

    // finalize + report
    iqk_fill_report(d);
    iqk_rf0xb0_workaround(d);
    iqk_0xc94_workaround(d);

    uint32_t rep = bb_r32(d, 0x1bf0);
    kprintf("[RTL8812BU]  IQK done: LOK A/B fail=%d/%d  TXK A/B fail=%d/%d  "
            "RXK A/B fail=%d/%d (code %d/%d)  0x1bf0=0x%08x\n",
            g_iqk.lok_fail[0], g_iqk.lok_fail[1],
            g_iqk.iqk_fail_report[0][0], g_iqk.iqk_fail_report[1][0],
            g_iqk.iqk_fail_report[0][1], g_iqk.iqk_fail_report[1][1],
            g_iqk.rxiqk_fail_code[0], g_iqk.rxiqk_fail_code[1], rep);
    bootlog_write("[RTL8812BU] IQK LOK=%d%d TXK=%d%d RXK=%d%d 1bf0=%08x",
                  g_iqk.lok_fail[0], g_iqk.lok_fail[1],
                  g_iqk.iqk_fail_report[0][0], g_iqk.iqk_fail_report[1][0],
                  g_iqk.iqk_fail_report[0][1], g_iqk.iqk_fail_report[1][1], rep);
}

// (3.4) Passive scan worker. Runs on its own thread so the (potentially slow,
// bulk-IN reads block up to 5s on an empty channel) sweep never stalls boot.
static void rtl_scan_worker(void *arg) {
    (void)arg;
    rtl_dev_t *d = &g_rtl;
    proc_sleep(6000);   // let USB enumeration + the desktop settle

    kprintf("\n[RTL8812BU] === PHASE 3: WiFi passive SCAN (2.4GHz ch 1..13, 20MHz) ===\n");
    bootlog_write("[RTL8812BU] SCAN START");
    rtl_rx_config_scan(d);
    g_bss_count = 0;
    if (d->wifi) wifi_core_set_state(d->wifi, WIFI_STATE_SCANNING);

    for (int ch = 1; ch <= 13; ch++) {
        rtl_set_channel(d, (uint8_t)ch);
        xhci_delay_ms(8);                              // let the PLL settle
        // PHASE 4b: run the per-path IQK on THIS channel so the on-channel
        // demodulator is IQ/DC-corrected (the reference re-runs IQK on every
        // channel switch). Then re-arm the RX path + flush the 3-wire, since the
        // IQK sequence backs up / disturbs the BB RX + RF mode registers.
        rtl_iqk_run(d);
        rtl_config_trx_mode(d);
        phydm_igi_toggle(d);
        xhci_delay_ms(5);
        if (ch == 1 || ch == 6 || ch == 11) {
            uint32_t rf18 = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
            // REG_RXPKT_NUM(0x0284)[31:24] = packets pending in the MAC RX buffer;
            // BB 0xf90/0xf94 = OFDM/CCK CCA counters (>0 => RF hears energy).
            uint32_t rxpkt = rtl_r32(d, 0x0284);
            uint32_t cca_ofdm = bb_r32(d, 0x0f90), cca_cck = bb_r32(d, 0x0fcc);
            kprintf("[RTL8812BU]  ch%d tuned: RF-A[0x18]=0x%05x 0x808=0x%08x RCR=0x%08x "
                    "RXPKT_NUM=0x%08x cca_ofdm=0x%08x cca_cck=0x%08x\n",
                    ch, rf18, bb_r32(d, 0x808), rtl_r32(d, REG_RCR), rxpkt, cca_ofdm, cca_cck);
            bootlog_write("[RTL8812BU] ch%d RF18=0x%05x 0x808=0x%08x", ch, rf18, bb_r32(d, 0x808));
            { char t[8]; t[0]='c'; t[1]='h'; t[2]=(char)('0'+ch/10); t[3]=(char)('0'+ch%10); t[4]=0;
              rtl_synth_report(d, t);
              rtl_rf_fulldump(d, t); }   // PHASE 4d: full RF 0x00..0xff A+B for synth-offset diff
        }
        int frames = 0, rx_pkts = 0, rx_bytes = 0;
        // Dwell long enough to catch a beacon (APs beacon every ~100ms). Do NOT
        // stop on the FIRST idle read (beacons arrive in bursts with idle gaps),
        // but abandon a channel after a run of consecutive idle reads so a dead
        // channel does not waste dozens of slow bulk-IN timeouts.
        int attempts = 40, misses = 0;
        for (int a = 0; a < attempts; a++) {
            memset(rx_buf, 0, 4096);
            int r = xhci_bulk_transfer(d->xhc, d->slot_id, d->ep_in & 0x0F,
                                       rx_buf, 4096, 1);
            if (r == CC_SUCCESS || r == CC_SHORT_PACKET) {
                rx_pkts++; misses = 0;
                if (rd_le32(rx_buf) != 0) rx_bytes = 1;
                frames += rtl_rx_parse_buffer(d, rx_buf, 4096, (uint8_t)ch);
            } else {
                if (++misses >= 4) break;              // channel idle -> next channel
                proc_sleep(6);                         // idle gap: wait for next frame
            }
        }
        kprintf("[RTL8812BU] scan ch %2d: %d rx-xfer, data=%d, %d mgmt frame(s); %d BSS total\n",
                ch, rx_pkts, rx_bytes, frames, g_bss_count);
    }

    kprintf("[RTL8812BU] === SCAN COMPLETE: %d network(s) found ===\n", g_bss_count);
    bootlog_write("[RTL8812BU] SCAN DONE: %d networks", g_bss_count);
    for (int i = 0; i < g_bss_count; i++) {
        bss_t *b = &g_bss[i];
        kprintf("[RTL8812BU]  [%2d] %02x:%02x:%02x:%02x:%02x:%02x  ch=%2d  %4d dBm  %s  \"%s\"\n",
                i + 1, b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3],
                b->bssid[4], b->bssid[5], b->channel, b->rssi, sec_name(b->sec),
                b->ssid_len ? b->ssid : "<hidden>");
        bootlog_write("[RTL8812BU] BSS %02x:%02x:%02x:%02x:%02x:%02x ch=%d %ddBm %s %s",
                      b->bssid[0], b->bssid[1], b->bssid[2], b->bssid[3],
                      b->bssid[4], b->bssid[5], b->channel, b->rssi, sec_name(b->sec),
                      b->ssid_len ? b->ssid : "<hidden>");
    }

    // ---- PHASE 4: pick a target from the scan and try to associate ----
    bss_t *target = rtl_pick_target();
    if (!target) {
        kprintf("[RTL8812BU] PHASE 4: no target BSS (need a named SSID on a valid "
                "channel); MLME skipped\n");
        bootlog_write("[RTL8812BU] MLME SKIP: no named BSS");
        return;
    }
    // Re-assert the RX FILTERS (the sweep may have left non-target filters) then
    // join. PHASE 4d: use rtl_rx_apply_filter, NOT rtl_rx_config_scan - the latter
    // re-inits the USB RX DMA (REG_RXDMA_MODE) on an already-running path, which
    // stalled the join RX (RXPKT_NUM=0, every bulk-IN timed out) even though the
    // scan heard 95 pkts at the same offset channel.
    rtl_rx_apply_filter(d);
    rtl_mlme_connect(d, target);
}

// Diagnostic: write RF-A reg `reg` = `val` then read it back (optionally with an
// IGI toggle to flush the 3-wire). Reports the before/after so we can see on real
// silicon whether the LSSI write path reaches the radio.
static void rf_wtest(rtl_dev_t *d, uint32_t reg, uint32_t val, int toggle, const char *tag) {
    uint32_t before = rf_read(d, 0, reg, RFREGOFFSETMASK);
    rf_write(d, 0, reg, val);
    if (toggle) phydm_igi_toggle(d);
    uint32_t after = rf_read(d, 0, reg, RFREGOFFSETMASK);
    kprintf("[RTL8812BU]  RFTEST %s RF-A[0x%02x]: %05x -> wrote %05x -> read %05x %s\n",
            tag, reg, before, val, after, (after == (val & 0xfffff)) ? "MATCH" : "no-change");
    bootlog_write("[RTL8812BU] RFTEST %s RF%02x %05x->%05x=%05x %s", tag, reg, before, val, after,
                  (after == (val & 0xfffff)) ? "OK" : "NC");
}

// Public: bring RF up for RX + spawn the scan worker. Called after phase-2.
void rtl8812bu_start_scan(void) {
    rtl_dev_t *d = &g_rtl;
    if (!d->present_ready) return;

    // (3.0) power up BB/RF + enable the RF 3-wire read/write window.
    rtl_enable_bb_rf(d);

    rf_probe(d, "start-scan-entry");    // raw post-phase-2 write-path state

    // Diagnostic dump of the RF/BB power + clock state after enable_bb_rf.
    kprintf("[RTL8812BU] RF-STATE: SYS_FUNC_EN(0x02)=%02x RF_CTRL(0x1f)=%02x WLRF1(0xEC)=%08x "
            "AFE_XTAL(0x24)=%08x AFE_PLL(0x28)=%08x SYS_CFG1(0xF0)=%08x\n",
            rtl_r8(d, 0x02), rtl_r8(d, 0x1f), rtl_r32(d, 0xEC),
            rtl_r32(d, 0x24), rtl_r32(d, 0x28), rtl_r32(d, 0xF0));
    kprintf("[RTL8812BU] BB-STATE: 0x800=%08x 0x808=%08x RFIntfOE-A(0x860)=%08x "
            "RFIntfOE-B(0x864)=%08x RFIntfSW(0x870)=%08x 0xc00=%08x\n",
            bb_r32(d, 0x800), bb_r32(d, 0x808), bb_r32(d, 0x860),
            bb_r32(d, 0x864), bb_r32(d, 0x870), bb_r32(d, 0xc00));
    bootlog_write("[RTL8812BU] RF-STATE FEN=%02x RFCTRL=%02x WLRF1=%08x 808=%08x",
                  rtl_r8(d, 0x02), rtl_r8(d, 0x1f), rtl_r32(d, 0xEC), bb_r32(d, 0x808));

    // Disable OFDM/CCK block + RX path so the RF 3-wire is un-gated while we
    // (re)apply the radio tables and run the LSSI self-test (parameter_init PRE).
    bb_set(d, 0x808, BIT(28) | BIT(29), 0x0);   // disable OFDM/CCK block
    bb_set(d, 0x808, 0x000000FF, 0x0);          // RX path OFF -> 3-wire un-gated

    // Does a write to the LSSI holding register (0xc90) even reach the BB? Write
    // a marker and read it straight back (this is a plain BB reg access).
    bb_w32(d, 0xc90, 0x000ca55e);
    kprintf("[RTL8812BU]  LSSI-HOLD 0xc90 wrote 0x000ca55e read 0x%08x  0xc94=%08x 0x88c=%08x 0xc00=%08x 0xe00=%08x\n",
            bb_r32(d, 0xc90), bb_r32(d, 0xc94), bb_r32(d, 0x88c), bb_r32(d, 0xc00), bb_r32(d, 0xe00));

    // Dump the RF-A register file (direct-read window 0x2800) BEFORE any write:
    // if the radio tables landed earlier, non-default values appear here.
    kprintf("[RTL8812BU]  RF-A file[0x00..0x0f]:");
    for (uint32_t r = 0; r < 0x10; r++) kprintf(" %05x", rf_read(d, 0, r, RFREGOFFSETMASK));
    kprintf("\n[RTL8812BU]  RF-A file[0x10..0x1f]:");
    for (uint32_t r = 0x10; r < 0x20; r++) kprintf(" %05x", rf_read(d, 0, r, RFREGOFFSETMASK));
    kprintf("\n");

    // --- LSSI SELF-TEST with the RX path OFF (writes must flush immediately) ---
    rf_wtest(d, 0x18, 0x00401, 0, "rxoff");     // channel 1
    rf_wtest(d, 0x18, 0x00406, 0, "rxoff");     // channel 6
    rf_wtest(d, 0x33, 0x00001, 0, "rxoff");     // RF mode-table entry
    rf_wtest(d, 0xef, 0x80000, 0, "rxoff");     // writable scratch
    rf_wtest(d, 0xb0, 0xabcde, 0, "rxoff");     // path-A data reg
    rf_wtest(d, 0x18, 0x00401, 1, "rxoff+tgl"); // with IGI toggle flush

    // Re-apply the RF radio tables now that the 3-wire is enabled AND un-gated.
    int n_rfa = rtl_apply_table(d, TBL_RF_A, rtl8822b_radioa,
                                sizeof(rtl8822b_radioa) / sizeof(uint32_t));
    int n_rfb = rtl_apply_table(d, TBL_RF_B, rtl8822b_radiob,
                                sizeof(rtl8822b_radiob) / sizeof(uint32_t));
    uint32_t rfa0_off = rf_read(d, 0, 0x00, RFREGOFFSETMASK);
    kprintf("[RTL8812BU] radio tables (rx-off): A=%d B=%d RF-A[0]=%05x\n", n_rfa, n_rfb, rfa0_off);

    // Re-enable OFDM/CCK (parameter_init POST) then RX mode + RX path.
    bb_set(d, 0x808, BIT(28) | BIT(29), 0x3);
    // Put the RF into RX mode + enable the RX path (essential for RX).
    rtl_config_trx_mode(d);
    // Route the RFE antenna switch to RX for 2.4GHz (iFEM, rfe_type=3).
    rtl_rfe_setup_2g(d);

    // PHASE 4c EXPERIMENT: one-shot LCK (VCO/LC-tank cal) to test the LO-relock
    // hypothesis. Snapshot the synth on ch11 before + after; if the LO cannot lock
    // this is where it shows. set_channel re-writes RF 0x18 afterwards so a bad LCK
    // cannot poison the scan.
    rf_write(d, 0, 0x18, 0x0040b);              // tune ch11 for the readback
    if (d->rf_2t2r) rf_write(d, 1, 0x18, 0x0040b);
    phydm_igi_toggle(d);
    xhci_delay_ms(5);
    rtl_synth_report(d, "ch11-pre-lck");
    // PHASE 4e CANDIDATE FIX (hypothesis, hardware-untested this pass): with
    // RTL8822B_INIT_LCK==0 we SKIP the init LCK/aac so the VCO/SX band keeps its
    // NATURAL table value (RF 0xc9[7:3]=3) instead of being force-set to 6. If the
    // constant +50 MHz offset is a forced-wrong VCO band, commanded ch11 should now
    // decode ch11 (RXPKT_NUM[31:24] > 0). Set RTL8822B_INIT_LCK to 1 to reproduce
    // the b644 behaviour for an A/B comparison on the same dongle.
#ifndef RTL8822B_INIT_LCK
#define RTL8822B_INIT_LCK 0
#endif
#if RTL8822B_INIT_LCK
    rtl_lck_run(d);
#else
    kprintf("[RTL8812BU]  PHASE-4e: init LCK/aac SKIPPED (natural VCO band kept; "
            "set RTL8822B_INIT_LCK=1 to reproduce b644)\n");
    bootlog_write("[RTL8812BU] PHASE-4e init-LCK skipped (natural VCO band)");
#endif
    rf_write(d, 0, 0x18, 0x0040b);
    if (d->rf_2t2r) rf_write(d, 1, 0x18, 0x0040b);
    phydm_igi_toggle(d);
    xhci_delay_ms(5);
    rtl_synth_report(d, "ch11-post-lck");

    // --- LSSI SELF-TEST with the RX path ON (needs the IGI toggle to flush) ---
    rf_wtest(d, 0x18, 0x0040b, 1, "rxon");      // channel 11, toggle-flushed

    // Verify RF read-back is now alive (the phase-2 open item).
    uint32_t rfa0  = rf_read(d, 0, 0x00, RFREGOFFSETMASK);
    uint32_t rfa18 = rf_read(d, 0, 0x18, RFREGOFFSETMASK);
    uint32_t rfb18 = rf_read(d, 1, 0x18, RFREGOFFSETMASK);
    int rf_alive = (rfa0 != 0 && rfa0 != 0xFFFFF) || (rfa18 != 0 && rfa18 != 0xFFFFF);
    kprintf("[RTL8812BU] RF power-up: RADIO_A=%d RADIO_B=%d RF-A[0]=0x%05x "
            "RF-A[0x18]=0x%05x RF-B[0x18]=0x%05x read-back=%s\n",
            n_rfa, n_rfb, rfa0, rfa18, rfb18, rf_alive ? "ALIVE" : "still-0");
    bootlog_write("[RTL8812BU] RF-UP RFA0=0x%05x RFA18=0x%05x readback=%s",
                  rfa0, rfa18, rf_alive ? "ALIVE" : "0");
    d->scan_ready = 1;
}

// Public: spawn the passive scan worker. Called from main.c AFTER full boot
// (same call site as the audio self-tests) so the worker schedules cleanly - a
// worker spawned from the early late_init path (progress 70) never runs.
void rtl8812bu_run_scan(void) {
    rtl_dev_t *d = &g_rtl;
    if (!d->scan_ready) return;
    // PRIO_HIGH: the passive scan + MLME worker does blocking bulk-IN reads (which
    // yield), so a high priority does not hard-peg the CPU but keeps the reads from
    // being starved by a busy foreground app during the auth/assoc exchange.
    proc_create("wifiscan", rtl_scan_worker, NULL, PRIO_HIGH);
}

// =============================================================================
// (e) Phase-2 kick-off (post fw-ready). This does NOT yet do the full MAC/BB/RF
// bring-up (TRX FIFO/RQPN/CR/RCR, the multi-thousand-entry BB + AGC + RF tables,
// and the IQK/LCK/DPK/DACK/TSSI calibration) - those are the large phase-2 body.
// It performs the two safe, verifiable first steps: confirm the 8051 MCU is
// actually running the freshly downloaded firmware (halmac check_fw_status_88xx:
// FW_DBG6 must be clear and the FW program counter at FW_DBG7 must advance), and
// snapshot the station MAC-ID register. Reports exactly how far bring-up reached.
// =============================================================================
static int rtl_phase2_kickoff(rtl_dev_t *d) {
    // check_fw_status_88xx: FW_DBG6 (0x10F8) should be 0; FW PC (FW_DBG7, 0x10FC)
    // must change between reads => the MCU is executing firmware, not hung.
    uint32_t dbg6 = rtl_r32(d, REG_FW_DBG6);
    uint32_t pc0  = rtl_r32(d, REG_FW_DBG7);
    uint32_t pc1  = pc0;
    int advanced = 0;
    for (int t = 0; t < 200; t++) {
        pc1 = rtl_r32(d, REG_FW_DBG7);
        if (pc1 != pc0) { advanced = 1; break; }
        xhci_delay_ms(1);
    }
    kprintf("[RTL8812BU] fw-status: DBG6=0x%08x FW_PC %08x->%08x %s\n",
            dbg6, pc0, pc1, advanced ? "ADVANCING (MCU running)" : "STUCK");
    bootlog_write("[RTL8812BU] FW-STATUS DBG6=0x%08x PC %08x->%08x %s",
                  dbg6, pc0, pc1, advanced ? "RUN" : "STUCK");

    // Snapshot the station MAC-ID register. This is only populated once efuse is
    // read + programmed during full MAC init, so pre-mac-init it is typically
    // zero - reported honestly as a phase-2 starting point, not a real MAC yet.
    uint8_t mac[6] = {0};
    for (int i = 0; i < 6; i++) mac[i] = rtl_r8(d, (uint16_t)(REG_MACID + i));
    kprintf("[RTL8812BU] REG_MACID = %02x:%02x:%02x:%02x:%02x:%02x (efuse MAC loaded in full mac-init)\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (!advanced) {
        kprintf("[RTL8812BU] MCU not running; skipping phase-2 MAC/BB/RF bring-up\n");
        return -1;
    }

    // ---- Phase 2 stages ----
    kprintf("[RTL8812BU] === PHASE 2: MAC init -> efuse MAC -> BB/AGC/RF -> IQK ===\n");
    bootlog_write("[RTL8812BU] PHASE-2 START");

    // (2.1) MAC init (TRX FIFO/RQPN/CR/RCR/EDCA/WMAC).
    int mac_ok = rtl_mac_init(d);

    // (2.2) efuse -> real station MAC + rfe_type (drives BB/RF table conditionals).
    int ef = rtl_efuse_read(d);

    // (2.3) BB + AGC + RF register tables.
    rtl_phy_config(d);

    rf_probe(d, "pre-iqk");         // baseline: writes before the IQK kickoff
    // (2.4) IQK kickoff (partial).
    rtl_iqk_kickoff(d);
    rf_probe(d, "post-iqk");        // did the IQK kickoff kill the write path?

    if (d->wifi) wifi_core_set_state(d->wifi, WIFI_STATE_INIT);
    kprintf("[RTL8812BU] === PHASE 2 done: MAC-init=%s efuse-MAC=%s BB/RF applied, IQK kicked off ===\n",
            mac_ok == 0 ? "OK" : "partial",
            ef == 0 ? "LOADED" : (d->mac_valid ? "loaded" : "blank/invalid"));
    bootlog_write("[RTL8812BU] PHASE-2 DONE mac=%d efuse=%d", mac_ok, ef);

    // (Phase 3) power the RF up for RX and spawn the passive scan worker.
    rtl8812bu_start_scan();
    return 0;
}

// Deferred firmware upload, invoked from main.c after the root filesystem is
// mounted. Safe to call unconditionally (no-op if no adapter was bound).
int rtl8812bu_late_init(void) {
    rtl_dev_t *d = &g_rtl;
    if (!d->present_ready) return 0;   // no adapter
    kprintf("[RTL8812BU] late-init: filesystem ready, uploading firmware...\n");
    int fr = rtl_load_and_download_fw(d);
    if (fr == 0 && d->wifi) wifi_core_set_state(d->wifi, WIFI_STATE_FW_LOADED);
    kprintf("[RTL8812BU] === phase-1 bring-up %s (enum+chipver OK, fw %s) ===\n",
            fr == 0 ? "COMPLETE" : "partial", fr == 0 ? "READY" : "not-ready");
    bootlog_write("[RTL8812BU] PHASE-1 %s fw=%d", fr == 0 ? "COMPLETE" : "partial", fr);

    // Phase-2 kick-off only if the firmware is up.
    if (fr == 0) rtl_phase2_kickoff(d);
    return fr;
}
