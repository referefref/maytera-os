// diskmgr / DISKUSE - MayteraOS Disk Manager suite (#404, Stage 6)
//
// WHAT IT IS. A TABBED disk suite grown out of the Disk Usage analyzer. Five
// tabs share one window:
//   Usage      - the original ncdu-style per-folder analyzer (unchanged idiom).
//   Partitions - SYS_BLK_ENUM device list + a GPT partition editor driven by the
//                two-phase SYS_PART_PREPARE -> SYS_PART_WRITE handshake.
//   Format     - pick a partition + filesystem + label -> SYS_MKFS.
//   Mount      - SYS_MOUNT_LIST table; mount at /MNT/<name> / unmount.
//   Removable  - SYS_VOL_LIST devices + safe-eject (SYS_VOL_BUSY then SYS_VOL_EJECT).
//
// SAFETY (this app can DESTROY data). Belt and braces with the kernel:
//   1. The BOOT disk is badged and every destructive control on it is greyed.
//      The kernel ALSO refuses (BLKMGR_E_BOOTDISK); the UI never trusts itself.
//   2. Every destructive action (partition write, format) requires a TYPE-TO-
//      CONFIRM modal: the exact device, the consequence, and the user must type
//      the disk model before the danger button enables, PLUS a settle timer so a
//      buffered keystroke cannot land on the button the instant it opens.
//   3. Partition writes go through PREPARE (mint a nonce bound to device+layout+
//      current-table hash) then APPLY the nonce, so a stale UI cannot write.
//   4. Long/destructive ops (mkfs, partition write, eject, mount) run on a
//      WORKER thread, never the UI/draw thread (#211/#212/#426). The UI polls
//      an atomically-published op state and shows progress.
//
// REAL DATA ONLY. Every number and every device comes from the kernel syscalls;
// nothing is invented. Test target is the RAM-backed scratch device (kind
// BLK_KIND_SCRATCH), armed by placing /DISKMGR.TST on the ESP; a normal golden
// exposes no scratch device and the destructive tabs simply find nothing to act
// on except real disks, where the boot disk is locked out.
#include "../../libc/maytera.h"
#include "../../libc/gui.h"
#include "../../libc/gui_style.h"
#include "../../libc/theme.h"
#include "../../libc/syscall.h"
#include "../../libc/stdio.h"
#include "../../libc/stdlib.h"
#include "../../libc/string.h"
#include "../../libc/pthread.h"
#include "../../libc/keys.h"

#define WIN_W     940
#define WIN_H     650
#define PAD       10
#define HDR_H     34
#define TABBAR_H  30
#define STATUS_H  24
#define PATHMAX   1024
#define LINEW     256
#define ROW_H     24
#define HDRROW_H  22
#define DBLCLICK_MS 450
#define SECTOR_BYTES 512ULL
#define ALIGN_SECTORS 2048ULL           // 1 MiB partition alignment
#define PARTS_MAX 64

static int win = -1, DW = WIN_W, DH = WIN_H;

// ---------------------------------------------------------------------------
// Tabs
// ---------------------------------------------------------------------------
enum { TAB_USAGE = 0, TAB_PART = 1, TAB_FORMAT = 2, TAB_MOUNT = 3, TAB_REMOV = 4, TAB_COUNT = 5 };
static int g_tab = TAB_USAGE;
static const char *g_tab_name[TAB_COUNT] = { "Usage", "Partitions", "Format", "Mount", "Removable" };

// ---------------------------------------------------------------------------
// Palette / style (same derivation devmgr/svcmgr/diskuse use)
// ---------------------------------------------------------------------------
static unsigned int C_BG, C_CARD, C_FIELD, C_BORDER, C_INK, C_DIM, C_ACC, C_SEL, C_SELTX, C_TRACK, C_WARN, C_OK;

static unsigned int lum_ink(unsigned int bg) {
    int r = (bg >> 16) & 255, g = (bg >> 8) & 255, b = bg & 255;
    return ((r * 30 + g * 59 + b * 11) / 100) > 140 ? 0x00181818u : 0x00F0F0F0u;
}
static unsigned int dim_ink(unsigned int bg) {
    unsigned int k = lum_ink(bg);
    int ir = (k >> 16) & 255, ig = (k >> 8) & 255, ib = k & 255;
    int br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    return (((ir + br) / 2) << 16) | (((ig + bgc) / 2) << 8) | ((ib + bb) / 2);
}
static unsigned int tint(unsigned int base, unsigned int acc, int pct) {
    int br = (base >> 16) & 255, bg = (base >> 8) & 255, bb = base & 255;
    int ar = (acc >> 16) & 255, ag = (acc >> 8) & 255, ab = acc & 255;
    return ((((br * (100 - pct) + ar * pct) / 100) & 255) << 16) |
           ((((bg * (100 - pct) + ag * pct) / 100) & 255) << 8) |
           (((bb * (100 - pct) + ab * pct) / 100) & 255);
}
static void apply_style(void) {
    int tid = theme_get_active();
    gui_set_style(tid == 4 ? GUI_STYLE_CLASSIC : GUI_STYLE_MODERN);
    unsigned int wb = theme_color(THEME_COLOR_WINDOW_BG);
    int r = (wb >> 16) & 255, g = (wb >> 8) & 255, b = wb & 255;
    int dark = ((r * 30 + g * 59 + b * 11) / 100) < 128;
    C_ACC   = theme_color(THEME_COLOR_ACCENT);
    C_BG    = tint(dark ? 0x00262A30 : 0x00F5F6F8, C_ACC, 5);
    C_CARD  = tint(dark ? 0x002C313B : 0x00EDEFF3, C_ACC, 6);
    C_FIELD = dark ? 0x00333A45 : 0x00FFFFFF;
    C_BORDER= dark ? 0x003A424F : 0x00CDD3DB;
    C_INK = lum_ink(C_BG); C_DIM = dim_ink(C_BG); C_SEL = C_ACC; C_SELTX = lum_ink(C_ACC);
    C_TRACK = tint(C_CARD, C_ACC, 18);
    C_WARN  = theme_color(THEME_COLOR_WARNING);
    C_OK    = 0x00329B58;
    gui_palette_t p;
    p.surface = C_BG; p.surface_raised = C_CARD; p.ink = C_INK; p.ink_dim = C_DIM;
    p.accent = C_ACC; p.accent_hover = gui_lighten(C_ACC, 24); p.border = C_BORDER;
    p.field_bg = C_FIELD; p.field_border = C_BORDER; p.track = C_TRACK;
    gui_set_palette(&p);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static long now_ms(void) { return (long)uptime_ms(); }

static void fmt_bytes(uint64_t b, char *out, int cap) {
    static const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    int ui = 0; uint64_t whole = b, frac = 0;
    while (whole >= 1024 && ui < 4) { frac = (whole % 1024) * 10 / 1024; whole /= 1024; ui++; }
    if (ui == 0) snprintf(out, (size_t)cap, "%lu B", (unsigned long)whole);
    else snprintf(out, (size_t)cap, "%lu.%lu %s", (unsigned long)whole, (unsigned long)frac, u[ui]);
}
static void fmt_mb(long mb, char *out, int cap) {
    if (mb >= 1024) snprintf(out, (size_t)cap, "%ld.%ld GB", mb / 1024, (mb % 1024) * 10 / 1024);
    else snprintf(out, (size_t)cap, "%ld MB", mb);
}
static int permille(uint64_t part, uint64_t whole) {
    if (!whole) return 0;
    if (part >= whole) return 1000;
    return (int)((part * 1000) / whole);
}
static void fit_text(const char *s, int sz, int max_w, char *out, int cap) {
    int n = (int)strlen(s);
    if (n > cap - 3) n = cap - 3;
    memcpy(out, s, (size_t)n); out[n] = 0;
    int w = gui_ttf_width(out, sz);
    if (w <= max_w || n == 0) return;
    int keep = (int)((long)n * max_w / (w > 0 ? w : 1)) - 2;
    if (keep < 1) keep = 1;
    if (keep >= n) keep = n - 1;
    for (;;) {
        memcpy(out, s, (size_t)keep);
        out[keep] = '.'; out[keep + 1] = '.'; out[keep + 2] = 0;
        if (keep <= 1 || gui_ttf_width(out, sz) <= max_w) return;
        keep -= (keep > 12 ? 3 : 1);
    }
}
static uint32_t rd32le(const unsigned char *p, int off) {
    return (uint32_t)p[off] | ((uint32_t)p[off+1] << 8) | ((uint32_t)p[off+2] << 16) | ((uint32_t)p[off+3] << 24);
}
static uint64_t rd64le(const unsigned char *p, int off) {
    return (uint64_t)rd32le(p, off) | ((uint64_t)rd32le(p, off+4) << 32);
}

// Fixed partition type GUIDs (match kernel blkmgr.rs).
static const unsigned char GUID_DATA[16] = {
    0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7 };
static const unsigned char GUID_LINUX[16] = {
    0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4 };
static int guid_is_zero(const unsigned char *g) {
    for (int i = 0; i < 16; i++) { if (g[i]) return 0; }
    return 1;
}

static const char *kind_name(int k) {
    switch (k) { case BLK_KIND_ATA: return "ATA"; case BLK_KIND_AHCI: return "AHCI";
                 case BLK_KIND_USB: return "USB"; case BLK_KIND_SCRATCH: return "SCRATCH";
                 default: return "?"; }
}

// ===========================================================================
// Shared cross-tab device / partition / mount / volume model
// ===========================================================================
static blk_dev_t g_disks[16]; static int g_ndisks = 0; static int g_disk_sel = 0;

typedef struct {
    unsigned char type_guid[16];
    uint64_t start_lba, size_lba;
    char name[40];
    int slot;                 // index in the GPT entry array (for SYS_MKFS)
} part_t;
static part_t g_parts[PARTS_MAX]; static int g_nparts = 0; static int g_part_sel = 0;
static int g_gpt_ok = 0;      // selected disk carries a parseable GPT
static char g_part_err[96];   // why the table could not be read/parsed

static mount_ent_t g_mounts[32]; static int g_nmounts = 0; static int g_mount_sel = 0;
static sc_volume_t g_vols[SC_VOL_MAX]; static int g_nvols = 0; static int g_vol_sel = 0;

// Proposed layout the Partitions editor manipulates (starts from on-disk parts).
static part_spec_t g_layout[PARTS_MAX]; static int g_nlayout = 0; static int g_lay_sel = 0;
static int g_dirty = 0;       // proposed layout differs from disk

// A one-line status banner shown at the bottom of the window.
static char g_banner[160]; static long g_banner_ms = 0; static unsigned int g_banner_col;
static void banner(const char *s, unsigned int col) {
    snprintf(g_banner, sizeof g_banner, "%s", s);
    g_banner_col = col; g_banner_ms = now_ms();
}

static blk_dev_t *cur_disk(void) { return (g_disk_sel >= 0 && g_disk_sel < g_ndisks) ? &g_disks[g_disk_sel] : NULL; }

// ---------------------------------------------------------------------------
// Enumerate disks (cheap: one syscall, no device I/O in the enum path)
// ---------------------------------------------------------------------------
static void refresh_disks(void) {
    int n = blk_enum(g_disks, 16);
    if (n < 0) n = 0;
    g_ndisks = n;
    if (g_disk_sel >= g_ndisks) g_disk_sel = g_ndisks - 1;
    if (g_disk_sel < 0) g_disk_sel = 0;
}

// Parse the selected disk's GPT via raw sector reads (LBA 1 header + entries).
static void parse_parts(void) {
    g_nparts = 0; g_gpt_ok = 0; g_part_err[0] = 0;
    blk_dev_t *d = cur_disk();
    if (!d) { snprintf(g_part_err, sizeof g_part_err, "no disk selected"); return; }
    unsigned char hdr[512];
    int r = blk_read_lba(d->kind, d->index, 1, 1, hdr);
    if (r != 1) { snprintf(g_part_err, sizeof g_part_err, "cannot read LBA1 (rc=%d)", r); return; }
    if (memcmp(hdr, "EFI PART", 8) != 0) {
        snprintf(g_part_err, sizeof g_part_err, "no GPT (blank or MBR disk)");
        return;
    }
    uint64_t entry_lba = rd64le(hdr, 72);
    uint32_t num = rd32le(hdr, 80);
    uint32_t esz = rd32le(hdr, 84);
    if (esz < 128 || esz > 512 || num == 0) { snprintf(g_part_err, sizeof g_part_err, "malformed GPT header"); return; }
    if (num > 128) num = 128;
    uint32_t bytes = num * esz;
    uint32_t secs = (bytes + 511) / 512; if (secs > 32) secs = 32;
    unsigned char *arr = (unsigned char *)malloc((size_t)secs * 512);
    if (!arr) { snprintf(g_part_err, sizeof g_part_err, "out of memory"); return; }
    r = blk_read_lba(d->kind, d->index, entry_lba, secs, arr);
    if (r != (int)secs) { free(arr); snprintf(g_part_err, sizeof g_part_err, "cannot read entry array (rc=%d)", r); return; }
    g_gpt_ok = 1;
    for (uint32_t i = 0; i < num && g_nparts < PARTS_MAX; i++) {
        const unsigned char *e = arr + (size_t)i * esz;
        if (guid_is_zero(e)) continue;              // unused entry
        part_t *p = &g_parts[g_nparts];
        memcpy(p->type_guid, e, 16);
        uint64_t first = rd64le(e, 32), last = rd64le(e, 40);
        p->start_lba = first;
        p->size_lba  = (last >= first) ? (last - first + 1) : 0;
        p->slot = (int)i;
        // UTF-16LE name at offset 56 (36 chars); take low bytes into ASCII.
        int o = 0;
        for (int c = 0; c < 36 && o < 39; c++) {
            unsigned char lo = e[56 + c * 2], hi = e[56 + c * 2 + 1];
            if (lo == 0 && hi == 0) break;
            p->name[o++] = (hi == 0 && lo >= 32 && lo < 127) ? (char)lo : '?';
        }
        p->name[o] = 0;
        g_nparts++;
    }
    free(arr);
    if (g_part_sel >= g_nparts) g_part_sel = g_nparts - 1;
    if (g_part_sel < 0) g_part_sel = 0;
}

// Rebuild the proposed layout from the on-disk partitions.
static void layout_from_disk(void) {
    g_nlayout = 0; g_dirty = 0; g_lay_sel = 0;
    for (int i = 0; i < g_nparts && g_nlayout < PARTS_MAX; i++) {
        part_spec_t *s = &g_layout[g_nlayout++];
        memset(s, 0, sizeof *s);
        memcpy(s->type_guid, g_parts[i].type_guid, 16);
        s->start_lba = g_parts[i].start_lba;
        s->size_lba  = g_parts[i].size_lba;
        int n = (int)strlen(g_parts[i].name); if (n > 39) n = 39;
        memcpy(s->name, g_parts[i].name, (size_t)n); s->name[n] = 0;
    }
}

static void refresh_mounts(void) {
    int n = mount_list(g_mounts, 32);
    if (n < 0) n = 0;
    g_nmounts = n;
    if (g_mount_sel >= g_nmounts) g_mount_sel = g_nmounts - 1;
    if (g_mount_sel < 0) g_mount_sel = 0;
}
static void refresh_vols(void) {
    int n = vol_list(g_vols, SC_VOL_MAX);
    if (n < 0) n = 0;
    g_nvols = n;
    if (g_vol_sel >= g_nvols) g_vol_sel = g_nvols - 1;
    if (g_vol_sel < 0) g_vol_sel = 0;
}

// ---------------------------------------------------------------------------
// Layout-edit helpers (all produce VALID layouts by construction)
// ---------------------------------------------------------------------------
static uint64_t align_up(uint64_t v, uint64_t a) { return ((v + a - 1) / a) * a; }
// End LBA (exclusive) of proposed partition i.
static uint64_t lay_end(int i) { return g_layout[i].start_lba + g_layout[i].size_lba; }
// First free sector after all proposed partitions.
static uint64_t lay_free_start(void) {
    uint64_t s = ALIGN_SECTORS;
    for (int i = 0; i < g_nlayout; i++) { uint64_t e = lay_end(i); if (e > s) s = e; }
    return align_up(s, ALIGN_SECTORS);
}
static void layout_new_part(int use_all) {
    blk_dev_t *d = cur_disk(); if (!d) return;
    if (g_nlayout >= PARTS_MAX) { banner("Partition limit reached", C_WARN); return; }
    uint64_t last_usable = (d->sectors > 34) ? d->sectors - 34 : 0;
    uint64_t start = lay_free_start();
    if (start + ALIGN_SECTORS > last_usable) { banner("No free space for a new partition", C_WARN); return; }
    uint64_t avail = last_usable - start;
    uint64_t size = use_all ? avail : (avail / 2);
    size = (size / ALIGN_SECTORS) * ALIGN_SECTORS;
    if (size < ALIGN_SECTORS) size = ALIGN_SECTORS;
    part_spec_t *s = &g_layout[g_nlayout];
    memset(s, 0, sizeof *s);
    memcpy(s->type_guid, GUID_DATA, 16);
    s->start_lba = start; s->size_lba = size;
    snprintf(s->name, sizeof s->name, "PART%d", g_nlayout + 1);
    g_lay_sel = g_nlayout; g_nlayout++;
    g_dirty = 1;
    banner("New partition added to the proposed layout (not yet written)", C_DIM);
}
static void layout_delete(void) {
    if (g_lay_sel < 0 || g_lay_sel >= g_nlayout) return;
    for (int i = g_lay_sel; i < g_nlayout - 1; i++) g_layout[i] = g_layout[i + 1];
    g_nlayout--;
    if (g_lay_sel >= g_nlayout) g_lay_sel = g_nlayout - 1;
    if (g_lay_sel < 0) g_lay_sel = 0;
    g_dirty = 1;
    banner("Partition removed from the proposed layout (not yet written)", C_DIM);
}
static void layout_resize(int grow) {
    blk_dev_t *d = cur_disk(); if (!d) return;
    if (g_lay_sel < 0 || g_lay_sel >= g_nlayout) return;
    part_spec_t *s = &g_layout[g_lay_sel];
    uint64_t step = 4 * ALIGN_SECTORS;   // 4 MiB
    if (grow) {
        // Grow into free space before the next partition (or the disk end).
        uint64_t limit = (d->sectors > 34) ? d->sectors - 34 : 0;
        for (int i = 0; i < g_nlayout; i++)
            if (i != g_lay_sel && g_layout[i].start_lba >= lay_end(g_lay_sel) && g_layout[i].start_lba < limit)
                limit = g_layout[i].start_lba;
        uint64_t maxsz = (limit > s->start_lba) ? limit - s->start_lba : s->size_lba;
        uint64_t nsz = s->size_lba + step; if (nsz > maxsz) nsz = maxsz;
        if (nsz == s->size_lba) { banner("Cannot grow: no free space after this partition", C_WARN); return; }
        s->size_lba = nsz;
    } else {
        uint64_t nsz = (s->size_lba > step) ? s->size_lba - step : ALIGN_SECTORS;
        if (nsz < ALIGN_SECTORS) nsz = ALIGN_SECTORS;
        s->size_lba = nsz;
    }
    g_dirty = 1;
}
static void layout_toggle_type(void) {
    if (g_lay_sel < 0 || g_lay_sel >= g_nlayout) return;
    part_spec_t *s = &g_layout[g_lay_sel];
    if (memcmp(s->type_guid, GUID_LINUX, 16) == 0) memcpy(s->type_guid, GUID_DATA, 16);
    else memcpy(s->type_guid, GUID_LINUX, 16);
    g_dirty = 1;
}

// ===========================================================================
// Worker thread for long / destructive ops (never on the UI thread, #426)
// ===========================================================================
enum { OPS_IDLE = 0, OPS_RUNNING = 1, OPS_DONE = 2 };
enum { OP_PARTWRITE = 1, OP_MKFS = 2, OP_MOUNT = 3, OP_UMOUNT = 4, OP_EJECT = 5 };
static volatile int g_ops = OPS_IDLE;
static pthread_t g_opth; static int g_opth_live = 0;
static int g_op_kind = 0;
static volatile int g_op_rc = 0;
static char g_op_msg[128];
// op parameters
static int g_op_dk, g_op_di, g_op_pi, g_op_fs, g_op_vi;
static char g_op_label[36], g_op_path[40];
static unsigned char g_op_nonce[16];
static part_spec_t g_op_layout[PARTS_MAX]; static int g_op_nparts;

static const char *blkmgr_err(int rc) {
    switch (rc) {
        case -700: return "refused: boot disk is protected";
        case -701: return "refused: device is busy";
        case -702: return "refused: bad/expired confirmation nonce";
        case -703: return "refused: layout changed since confirm";
        case -704: return "refused: device changed under us";
        case -705: return "refused: out of range";
        case -706: return "no such device";
        case -1:   return "refused: permission denied";
        case -22:  return "refused: invalid argument";
        default:   return "failed";
    }
}

static void *op_thread(void *arg) {
    (void)arg;
    int rc = -1; char msg[128];
    switch (g_op_kind) {
        case OP_PARTWRITE:
            rc = part_write(g_op_dk, g_op_di, g_op_layout, g_op_nparts, g_op_nonce);
            if (rc >= 0) snprintf(msg, sizeof msg, "Partition table written (%d partitions)", g_op_nparts);
            else snprintf(msg, sizeof msg, "Partition write %s", blkmgr_err(rc));
            break;
        case OP_MKFS:
            rc = mkfs(g_op_dk, g_op_di, g_op_pi, g_op_fs, g_op_label[0] ? g_op_label : (char *)0);
            if (rc >= 0) snprintf(msg, sizeof msg, "Formatted partition %d as %s", g_op_pi, g_op_fs == MKFS_EXT2 ? "ext2" : "FAT");
            else snprintf(msg, sizeof msg, "Format %s", blkmgr_err(rc));
            break;
        case OP_MOUNT:
            rc = mount_at(g_op_dk, g_op_di, g_op_pi, g_op_path);
            if (rc >= 0) snprintf(msg, sizeof msg, "Mounted at %s", g_op_path);
            else snprintf(msg, sizeof msg, "Mount %s", blkmgr_err(rc));
            break;
        case OP_UMOUNT:
            rc = umount_at(g_op_path);
            if (rc >= 0) snprintf(msg, sizeof msg, "Unmounted %s", g_op_path);
            else snprintf(msg, sizeof msg, "Unmount %s", blkmgr_err(rc));
            break;
        case OP_EJECT: {
            int busy = vol_busy(g_op_vi);
            if (busy > 0) { rc = -701; snprintf(msg, sizeof msg, "Cannot eject: %d open handle(s)", busy); }
            else {
                rc = vol_eject(g_op_vi);
                if (rc >= 0) snprintf(msg, sizeof msg, "Safe to remove the device");
                else snprintf(msg, sizeof msg, "Eject failed (rc=%d)", rc);
            }
            break;
        }
        default: snprintf(msg, sizeof msg, "unknown op"); break;
    }
    g_op_rc = rc;
    memcpy(g_op_msg, msg, sizeof g_op_msg);
    __atomic_store_n(&g_ops, OPS_DONE, __ATOMIC_RELEASE);
    return NULL;
}
static int op_running(void) { return __atomic_load_n(&g_ops, __ATOMIC_ACQUIRE) == OPS_RUNNING; }
static void op_start(void) {
    if (op_running()) return;
    if (g_opth_live) { pthread_join(g_opth, NULL); g_opth_live = 0; }
    __atomic_store_n(&g_ops, OPS_RUNNING, __ATOMIC_RELEASE);
    if (pthread_create(&g_opth, NULL, op_thread, NULL) != 0) {
        __atomic_store_n(&g_ops, OPS_IDLE, __ATOMIC_RELEASE);
        banner("Could not start worker thread", C_WARN);
        return;
    }
    g_opth_live = 1;
}

// ===========================================================================
// Type-to-confirm modal (self-contained; reuses the confirm SAFETY properties:
// a settle timer so a buffered key cannot fire, and an explicit typed match for
// the destructive variant).
// ===========================================================================
enum { MODAL_NONE = 0, MODAL_TYPECONFIRM = 1, MODAL_CONFIRM = 2, MODAL_NOTICE = 3 };
#define SETTLE_MS 500
static int g_modal = MODAL_NONE;
static char g_modal_title[64];
static char g_modal_line[3][110]; static int g_modal_nlines = 0;
static char g_confirm_expect[48];      // string the user must type (TYPECONFIRM)
static char g_confirm_input[48]; static int g_confirm_len = 0;
static long g_modal_shown = 0;
static int g_modal_pending_op = 0;     // OP_* to run on confirm

static void modal_open(int kind, const char *title, const char *l0, const char *l1, const char *l2,
                       const char *expect, int pending_op) {
    g_modal = kind;
    snprintf(g_modal_title, sizeof g_modal_title, "%s", title ? title : "");
    g_modal_nlines = 0;
    if (l0) { snprintf(g_modal_line[0], sizeof g_modal_line[0], "%s", l0); g_modal_nlines = 1; }
    if (l1) { snprintf(g_modal_line[1], sizeof g_modal_line[1], "%s", l1); g_modal_nlines = 2; }
    if (l2) { snprintf(g_modal_line[2], sizeof g_modal_line[2], "%s", l2); g_modal_nlines = 3; }
    g_confirm_expect[0] = 0; g_confirm_input[0] = 0; g_confirm_len = 0;
    if (expect) snprintf(g_confirm_expect, sizeof g_confirm_expect, "%s", expect);
    g_modal_shown = now_ms();
    g_modal_pending_op = pending_op;
}
static void modal_close(void) { g_modal = MODAL_NONE; g_modal_pending_op = 0; }
static int modal_settled(void) { return now_ms() - g_modal_shown >= SETTLE_MS; }
static int modal_can_confirm(void) {
    if (!modal_settled()) return 0;
    if (g_modal == MODAL_TYPECONFIRM) return strcmp(g_confirm_input, g_confirm_expect) == 0;
    return 1;
}

// Modal geometry.
static int modal_w(void) { return 460; }
static int modal_h(void) { return g_modal == MODAL_TYPECONFIRM ? 246 : 190; }
static int modal_x(void) { return (DW - modal_w()) / 2; }
static int modal_y(void) { return (DH - modal_h()) / 2; }

static void modal_render(void) {
    if (!g_modal) return;
    // Scrim (interlaced scanlines confined to the window) then the card.
    for (int y = 0; y < DH; y += 2) win_draw_rect(win, 0, y, DW, 1, 0x00000000u);
    int mw = modal_w(), mh = modal_h(), mx = modal_x(), my = modal_y();
    gui_card(win, mx, my, mw, mh);
    unsigned int cink = lum_ink(C_CARD), cdim = dim_ink(C_CARD);
    int x = mx + 20, y = my + 18;
    unsigned int tcol = (g_modal == MODAL_NOTICE) ? cink : C_WARN;
    win_draw_text_ttf(win, x, y, g_modal_title, 16, tcol);
    y += 30;
    win_draw_rect(win, mx + 16, y - 6, mw - 32, 1, C_BORDER);
    for (int i = 0; i < g_modal_nlines; i++) {
        char fit[128]; fit_text(g_modal_line[i], 13, mw - 40, fit, sizeof fit);
        win_draw_text_ttf(win, x, y, fit, 13, cink); y += 22;
    }
    if (g_modal == MODAL_TYPECONFIRM) {
        y += 4;
        char prompt[96]; snprintf(prompt, sizeof prompt, "Type  %s  to confirm:", g_confirm_expect);
        char fit[110]; fit_text(prompt, 12, mw - 40, fit, sizeof fit);
        win_draw_text_ttf(win, x, y, fit, 12, cdim); y += 20;
        gui_textfield_tf(win, x, y, mw - 40, 28, g_confirm_input, g_confirm_len, g_confirm_len, -1, 1, NULL);
        y += 36;
    } else {
        y += 6;
    }
    // Buttons at the bottom.
    int by = my + mh - 44, bw = 120, bh = 30;
    if (g_modal == MODAL_NOTICE) {
        gui_button(win, mx + mw - 20 - bw, by, bw, bh, "OK", GUI_BTN_PRIMARY, GUI_ST_NORMAL);
    } else {
        gui_button(win, mx + 20, by, bw, bh, "Cancel", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
        int can = modal_can_confirm();
        const char *lbl = (g_modal == MODAL_TYPECONFIRM || g_modal_pending_op == OP_PARTWRITE || g_modal_pending_op == OP_MKFS)
                            ? "Apply" : "Confirm";
        gui_button(win, mx + mw - 20 - bw, by, bw, bh, lbl, GUI_BTN_DANGER,
                   can ? GUI_ST_NORMAL : GUI_ST_DISABLED);
        if (!modal_settled())
            win_draw_text_ttf(win, mx + 20, by - 20, "Read the warning above...", 11, cdim);
    }
}

// Run the pending destructive op once confirmed. Returns 1 if it started.
static void modal_do_confirm(void) {
    int op = g_modal_pending_op;
    modal_close();
    if (!op) return;
    g_op_kind = op;
    op_start();
}

// ---------------------------------------------------------------------------
// Usage tab (the original diskuse analyzer, worker-thread scan)
// ---------------------------------------------------------------------------
#define U_SIDE_W  260
#define U_HDR     22
typedef struct node {
    struct node  *parent;
    struct node **kids;
    int           nkids, capkids;
    char         *name;
    uint64_t      bytes;
    uint32_t      files, dirs;
    uint8_t       is_dir, skipped, err;
} node_t;

static node_t *node_new(node_t *parent, const char *name, int is_dir) {
    node_t *n = (node_t *)malloc(sizeof(node_t));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    size_t nl = strlen(name);
    n->name = (char *)malloc(nl + 1);
    if (!n->name) { free(n); return NULL; }
    memcpy(n->name, name, nl + 1);
    n->parent = parent;
    n->is_dir = (uint8_t)(is_dir ? 1 : 0);
    if (parent) {
        if (parent->nkids == parent->capkids) {
            int nc = parent->capkids ? parent->capkids * 2 : 16;
            node_t **nk = (node_t **)realloc(parent->kids, (size_t)nc * sizeof(node_t *));
            if (!nk) { free(n->name); free(n); return NULL; }
            parent->kids = nk; parent->capkids = nc;
        }
        parent->kids[parent->nkids++] = n;
    }
    return n;
}
static void node_free(node_t *n) {
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    free(n->kids); free(n->name); free(n);
}
static int node_path(const node_t *n, char *out, int cap) {
    if (!n->parent) {
        int l = (int)strlen(n->name);
        if (l + 1 > cap) return -1;
        memcpy(out, n->name, (size_t)l + 1);
        return l;
    }
    int l = node_path(n->parent, out, cap);
    if (l < 0) return -1;
    int nl = (int)strlen(n->name);
    int need = l + (l > 0 && out[l - 1] != '/' ? 1 : 0) + nl + 1;
    if (need > cap) return -1;
    if (l > 0 && out[l - 1] != '/') out[l++] = '/';
    memcpy(out + l, n->name, (size_t)nl + 1);
    return l + nl;
}

enum { ST_IDLE = 0, ST_SCANNING = 1, ST_DONE = 2 };
static node_t *g_root = NULL;
static volatile int g_state = ST_IDLE;
static volatile int g_cancel = 0;
static volatile uint64_t g_p_bytes = 0;
static volatile uint32_t g_p_files = 0, g_p_dirs = 0;
static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static char g_p_path[PATHMAX];
static char g_scan_root[PATHMAX] = "/";
static long g_scan_t0 = 0, g_scan_ms = 0;
static uint32_t g_err_count = 0, g_skip_count = 0;
static pthread_t g_thread = 0; static int g_thread_live = 0;
static node_t *g_cur = NULL; static int g_sel = 0, g_scroll = 0;
static long g_last_click_ms = 0; static int g_last_click_row = -1;
static long g_disk_total_mb = -1, g_disk_free_mb = -1;
static int g_files_spawn_status = 0;

static int ci_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}
static int is_pruned_root_child(const char *name) {
    static const char *skip[] = { "DEV", "SMB", "NFS", "NET", "GRAPHFS" };
    for (unsigned i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (ci_eq(name, skip[i])) return 1;
    return 0;
}
static void scan_dir(node_t *n, char *path, int plen, int depth, int root_is_slash) {
    if (g_cancel) return;
    pthread_mutex_lock(&g_plock);
    memcpy(g_p_path, path, (size_t)plen + 1);
    pthread_mutex_unlock(&g_plock);
    int fd = sys_open(path, 0);
    if (fd < 0) { n->err = 1; return; }
    dirent_t de;
    uint64_t direct_bytes = 0; uint32_t direct_files = 0;
    for (;;) {
        int r = sys_readdir_raw(fd, &de);
        if (r != 0) { if (r < 0) n->err = 1; break; }
        if (de.name[0] == '.' && (de.name[1] == 0 || (de.name[1] == '.' && de.name[2] == 0))) continue;
        node_t *k = node_new(n, de.name, DIRENT_IS_DIR(de));
        if (!k) { n->err = 1; break; }
        if (!k->is_dir) { k->bytes = de.size; k->files = 1; direct_bytes += de.size; direct_files++; }
        else if (depth == 0 && root_is_slash && is_pruned_root_child(de.name)) k->skipped = 1;
    }
    sys_close(fd);
    __atomic_fetch_add(&g_p_bytes, direct_bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_p_files, direct_files, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_p_dirs, 1u, __ATOMIC_RELAXED);
    for (int i = 0; i < n->nkids; i++) {
        node_t *k = n->kids[i];
        if (k->is_dir && !k->skipped && !g_cancel) {
            int nl = (int)strlen(k->name), np = plen;
            int need = np + (plen == 1 && path[0] == '/' ? 0 : 1) + nl + 1;
            if (need > PATHMAX) k->err = 1;
            else {
                if (!(plen == 1 && path[0] == '/')) path[np++] = '/';
                memcpy(path + np, k->name, (size_t)nl + 1); np += nl;
                scan_dir(k, path, np, depth + 1, root_is_slash);
                path[plen] = 0;
            }
        }
        n->bytes += k->bytes; n->files += k->files; n->dirs += k->dirs + (k->is_dir ? 1u : 0u);
    }
}
static int cmp_kids(const void *a, const void *b) {
    const node_t *x = *(node_t *const *)a, *y = *(node_t *const *)b;
    if (x->bytes != y->bytes) return x->bytes < y->bytes ? 1 : -1;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    return strcmp(x->name, y->name);
}
static void sort_tree(node_t *n) {
    if (n->nkids > 1) qsort(n->kids, (size_t)n->nkids, sizeof(node_t *), cmp_kids);
    for (int i = 0; i < n->nkids; i++) if (n->kids[i]->is_dir) sort_tree(n->kids[i]);
}
static void count_flags(const node_t *n, uint32_t *errs, uint32_t *skips) {
    if (n->err) (*errs)++;
    if (n->skipped) (*skips)++;
    for (int i = 0; i < n->nkids; i++) count_flags(n->kids[i], errs, skips);
}
static void *scan_thread(void *arg) {
    (void)arg;
    char path[PATHMAX];
    int plen = (int)strlen(g_scan_root);
    memcpy(path, g_scan_root, (size_t)plen + 1);
    node_t *root = node_new(NULL, g_scan_root, 1);
    if (root) {
        scan_dir(root, path, plen, 0, (plen == 1 && path[0] == '/'));
        if (!g_cancel) { sort_tree(root); uint32_t e = 0, s = 0; count_flags(root, &e, &s); g_err_count = e; g_skip_count = s; }
    }
    g_root = root;
    g_scan_ms = now_ms() - g_scan_t0;
    __atomic_store_n(&g_state, ST_DONE, __ATOMIC_RELEASE);
    return NULL;
}
static void scan_stop_and_free(void) {
    if (g_thread_live) { g_cancel = 1; pthread_join(g_thread, NULL); g_thread_live = 0; }
    node_free(g_root); g_root = NULL; g_state = ST_IDLE;
}
static int scan_start(void) {
    scan_stop_and_free();
    g_cancel = 0; g_p_bytes = 0; g_p_files = 0; g_p_dirs = 0;
    g_err_count = 0; g_skip_count = 0; g_p_path[0] = 0;
    g_scan_t0 = now_ms(); g_scan_ms = 0;
    __atomic_store_n(&g_state, ST_SCANNING, __ATOMIC_RELEASE);
    pthread_attr_t at; pthread_attr_init(&at); at.stack_size = 256 * 1024;
    int rc = pthread_create(&g_thread, &at, scan_thread, NULL);
    pthread_attr_destroy(&at);
    if (rc != 0) { g_state = ST_IDLE; return -1; }
    g_thread_live = 1;
    return 0;
}
static void adopt_tree_if_done(void) {
    if (!g_cur && __atomic_load_n(&g_state, __ATOMIC_ACQUIRE) == ST_DONE) {
        if (g_thread_live) { pthread_join(g_thread, NULL); g_thread_live = 0; }
        g_cur = g_root; g_sel = 0; g_scroll = 0;
    }
}

// ---------------------------------------------------------------------------
// Common layout: header + tab bar + content rect
// ---------------------------------------------------------------------------
static int content_y(void) { return HDR_H + TABBAR_H + 6; }
static int content_x(void) { return PAD; }
static int content_w(void) { return DW - 2 * PAD; }
static int content_h(void) { return DH - content_y() - PAD - STATUS_H; }

static int tab_w(void) { return (DW - 2 * PAD) / TAB_COUNT; }
static int tab_at(int mx, int my) {
    if (my < HDR_H || my >= HDR_H + TABBAR_H) return -1;
    int i = (mx - PAD) / tab_w();
    if (i < 0 || i >= TAB_COUNT) return -1;
    return i;
}

// A simple beveled list row helper used across the data tabs.
static int list_rows(int listh) { int n = (listh - HDRROW_H - 6) / ROW_H; return n < 1 ? 1 : n; }

// ---------------------------------------------------------------------------
// Draw: Usage tab
// ---------------------------------------------------------------------------
#define U_BTN_H 26
static int u_btn_rescan_x(void) { return DW - PAD - 96; }
static int u_btn_up_x(void)     { return u_btn_rescan_x() - 8 - 60; }
static int u_btn_files_x(void)  { return u_btn_up_x() - 8 - 68; }
static int u_btn_y(void)        { return content_y() + 4; }
static int u_list_x(void) { return content_x(); }
static int u_list_y(void) { return content_y() + U_BTN_H + 12 + 22; }
static int u_list_w(void) { int w = content_w() - U_SIDE_W - PAD; return w < 200 ? 200 : w; }
static int u_list_h(void) { int h = DH - u_list_y() - PAD - STATUS_H; return h < 60 ? 60 : h; }
static int u_side_x(void) { return u_list_x() + u_list_w() + PAD; }
static int u_rows_vis(void) { int n = (u_list_h() - U_HDR - 10) / ROW_H; return n < 1 ? 1 : n; }

static void u_clamp(void) {
    if (!g_cur) { g_sel = 0; g_scroll = 0; return; }
    int n = g_cur->nkids;
    if (g_sel >= n) g_sel = n - 1;
    if (g_sel < 0) g_sel = 0;
    int vis = u_rows_vis();
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + vis) g_scroll = g_sel - vis + 1;
    if (g_scroll > n - vis) g_scroll = n - vis;
    if (g_scroll < 0) g_scroll = 0;
}
static void draw_glyph(int x, int y, int is_dir, unsigned int col, unsigned int bg) {
    if (is_dir) { gui_fill_rounded_aa(win, x, y + 3, 14, 10, 2, col, bg); gui_fill_rounded_aa(win, x, y + 1, 7, 4, 1, col, bg); }
    else gui_rounded_border(win, x + 2, y, 10, 13, 1, col);
}
static void draw_usage(void) {
    int st = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
    // toolbar row
    char sub[LINEW];
    if (g_disk_total_mb > 0 && g_disk_free_mb >= 0) {
        char t[32], f[32]; fmt_mb(g_disk_total_mb, t, sizeof t); fmt_mb(g_disk_free_mb, f, sizeof f);
        long used = g_disk_total_mb - g_disk_free_mb; if (used < 0) used = 0;
        snprintf(sub, LINEW, "Root volume: %s total, %s free (%ld%% used)", t, f, used * 100 / g_disk_total_mb);
    } else snprintf(sub, LINEW, "Root volume: size unavailable");
    win_draw_text_ttf(win, content_x() + 2, content_y() + 8, sub, 12, C_DIM);
    int can_up = (st == ST_DONE && g_cur && g_cur->parent);
    gui_button(win, u_btn_files_x(), u_btn_y(), 68, U_BTN_H, "Files", GUI_BTN_SECONDARY, (st == ST_DONE && g_cur) ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(win, u_btn_up_x(), u_btn_y(), 60, U_BTN_H, "Up", GUI_BTN_SECONDARY, can_up ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(win, u_btn_rescan_x(), u_btn_y(), 96, U_BTN_H, st == ST_SCANNING ? "Cancel" : "Rescan", GUI_BTN_PRIMARY, GUI_ST_NORMAL);

    // breadcrumb
    char path[PATHMAX];
    if (st == ST_DONE && g_cur) { if (node_path(g_cur, path, sizeof path) < 0) snprintf(path, sizeof path, "(path too long)"); }
    else snprintf(path, sizeof path, "%s", g_scan_root);
    char fit[LINEW]; fit_text(path, 13, content_w() - 4, fit, sizeof fit);
    win_draw_text_ttf(win, content_x() + 2, content_y() + U_BTN_H + 14, fit, 13, C_INK);

    // list card
    int lx = u_list_x(), ly = u_list_y(), lw = u_list_w(), lh = u_list_h();
    gui_card(win, lx, ly, lw, lh);
    unsigned int cink = lum_ink(C_CARD), cdim = dim_ink(C_CARD);
    if (st != ST_DONE || !g_cur) {
        char l1[LINEW], l2[LINEW];
        if (st == ST_SCANNING) {
            char b[32]; fmt_bytes(__atomic_load_n(&g_p_bytes, __ATOMIC_RELAXED), b, sizeof b);
            snprintf(l1, LINEW, "Scanning %s", g_scan_root);
            snprintf(l2, LINEW, "%u folders, %u files, %s so far", (unsigned)__atomic_load_n(&g_p_dirs, __ATOMIC_RELAXED),
                     (unsigned)__atomic_load_n(&g_p_files, __ATOMIC_RELAXED), b);
        } else { snprintf(l1, LINEW, "No scan"); snprintf(l2, LINEW, "Press Rescan (R) to scan %s", g_scan_root); }
        int cy = ly + lh / 2 - 20;
        gui_text_ttf_centered(win, lx, cy, lw, 20, l1, cink, 15);
        gui_text_ttf_centered(win, lx, cy + 26, lw, 18, l2, cdim, 12);
    } else {
        int hx = lx + 12, hy = ly + 6, right = lx + lw - 12;
        int x_items = right - 60, x_share = x_items - 120, x_size = x_share - 80;
        win_draw_text_ttf(win, hx + 24, hy + 2, "Name", 12, cdim);
        win_draw_text_ttf(win, x_size, hy + 2, "Size", 12, cdim);
        win_draw_text_ttf(win, x_share, hy + 2, "Share", 12, cdim);
        win_draw_text_ttf(win, x_items, hy + 2, "Items", 12, cdim);
        win_draw_rect(win, lx + 8, hy + U_HDR - 2, lw - 16, 1, C_BORDER);
        int vis = u_rows_vis(), n = g_cur->nkids;
        if (n == 0) gui_text_ttf_centered(win, lx, ly + lh / 2 - 10, lw, 20, "(empty folder)", cdim, 13);
        int name_w = x_size - (hx + 24) - 10;
        for (int rr = 0; rr < vis && rr + g_scroll < n; rr++) {
            int idx = rr + g_scroll; node_t *k = g_cur->kids[idx];
            int ry = hy + U_HDR + rr * ROW_H; int selrow = (idx == g_sel);
            unsigned int rowbg = C_CARD;
            if (selrow) { gui_fill_rounded_aa(win, lx + 4, ry, lw - 8, ROW_H - 2, 4, C_SEL, C_CARD); rowbg = C_SEL; }
            unsigned int ink = selrow ? C_SELTX : cink, dink = selrow ? C_SELTX : cdim;
            draw_glyph(hx, ry + 4, k->is_dir, k->is_dir ? (selrow ? C_SELTX : C_ACC) : dink, rowbg);
            char nm[LINEW]; fit_text(k->name, 13, name_w, nm, sizeof nm);
            win_draw_text_ttf(win, hx + 24, ry + 4, nm, 13, ink);
            char sz[32]; fmt_bytes(k->bytes, sz, sizeof sz);
            win_draw_text_ttf(win, x_size, ry + 5, sz, 12, ink);
            if (k->skipped) win_draw_text_ttf(win, x_share, ry + 5, "not scanned", 11, selrow ? C_SELTX : C_WARN);
            else {
                int pm = permille(k->bytes, g_cur->bytes); int bw = 120 - 52, fw = (bw * pm) / 1000;
                win_draw_rect(win, x_share, ry + 8, bw, 8, selrow ? tint(C_SEL, C_SELTX, 25) : C_TRACK);
                if (fw > 0) win_draw_rect(win, x_share, ry + 8, fw, 8, selrow ? C_SELTX : C_ACC);
                char pc[16]; snprintf(pc, sizeof pc, "%d.%d%%", pm / 10, pm % 10);
                win_draw_text_ttf(win, x_share + bw + 6, ry + 5, pc, 11, dink);
            }
            char it[32];
            if (k->is_dir) snprintf(it, sizeof it, "%lu", (unsigned long)(k->files + k->dirs)); else snprintf(it, sizeof it, "-");
            win_draw_text_ttf(win, x_items, ry + 5, it, 12, dink);
        }
    }
    // side card
    int sx = u_side_x(), sy = ly, sw = U_SIDE_W, sh = lh;
    gui_card(win, sx, sy, sw, sh);
    unsigned int sink = lum_ink(C_CARD), sdim = dim_ink(C_CARD);
    int x = sx + 14, y = sy + 12, lhh = 19; char b[LINEW], b2[64];
    win_draw_text_ttf(win, x, y, "This folder", 14, sink); y += 24;
    win_draw_rect(win, sx + 12, y - 4, sw - 24, 1, C_BORDER);
    if (st == ST_DONE && g_cur) {
        fmt_bytes(g_cur->bytes, b2, sizeof b2);
        snprintf(b, LINEW, "Total:   %s", b2); win_draw_text_ttf(win, x, y, b, 12, sink); y += lhh;
        snprintf(b, LINEW, "Files:   %lu", (unsigned long)g_cur->files); win_draw_text_ttf(win, x, y, b, 12, sdim); y += lhh;
        snprintf(b, LINEW, "Folders: %lu", (unsigned long)g_cur->dirs); win_draw_text_ttf(win, x, y, b, 12, sdim); y += lhh;
        y += 6;
        win_draw_text_ttf(win, x, y, "Scan", 14, sink); y += 24;
        win_draw_rect(win, sx + 12, y - 4, sw - 24, 1, C_BORDER);
        snprintf(b, LINEW, "Took %ld ms", g_scan_ms); win_draw_text_ttf(win, x, y, b, 12, sdim); y += lhh;
        if (g_skip_count) { snprintf(b, LINEW, "Not scanned: %u", (unsigned)g_skip_count); win_draw_text_ttf(win, x, y, b, 11, C_WARN); y += lhh; }
        if (g_err_count) { snprintf(b, LINEW, "Unreadable: %u", (unsigned)g_err_count); win_draw_text_ttf(win, x, y, b, 11, C_WARN); y += lhh; }
    } else if (st == ST_SCANNING) {
        char sz[32]; fmt_bytes(__atomic_load_n(&g_p_bytes, __ATOMIC_RELAXED), sz, sizeof sz);
        snprintf(b, LINEW, "Scanning..."); win_draw_text_ttf(win, x, y, b, 12, sink); y += lhh;
        snprintf(b, LINEW, "Folders: %u", (unsigned)__atomic_load_n(&g_p_dirs, __ATOMIC_RELAXED)); win_draw_text_ttf(win, x, y, b, 12, sdim); y += lhh;
        snprintf(b, LINEW, "Files:   %u", (unsigned)__atomic_load_n(&g_p_files, __ATOMIC_RELAXED)); win_draw_text_ttf(win, x, y, b, 12, sdim); y += lhh;
        snprintf(b, LINEW, "Bytes:   %s", sz); win_draw_text_ttf(win, x, y, b, 12, sdim); y += lhh;
    }
}
static int u_row_at(int lx, int ly) {
    int x0 = u_list_x(), y0 = u_list_y() + 6 + U_HDR;
    if (lx < x0 || lx >= x0 + u_list_w()) return -1;
    if (ly < y0) return -1;
    int rr = (ly - y0) / ROW_H;
    if (rr >= u_rows_vis()) return -1;
    int idx = rr + g_scroll;
    if (!g_cur || idx >= g_cur->nkids) return -1;
    return idx;
}
static void u_go_into(void) { if (!g_cur || g_cur->nkids == 0) return; node_t *k = g_cur->kids[g_sel]; if (!k->is_dir || k->skipped) return; g_cur = k; g_sel = 0; g_scroll = 0; }
static void u_go_up(void) { if (!g_cur || !g_cur->parent) return; node_t *was = g_cur; g_cur = g_cur->parent; g_sel = 0; for (int i = 0; i < g_cur->nkids; i++) if (g_cur->kids[i] == was) { g_sel = i; break; } g_scroll = 0; }
static void u_open_files(void) { if (!g_cur) return; char path[PATHMAX]; if (node_path(g_cur, path, sizeof path) < 0) return; char *argv[2]; argv[0] = (char *)"FILES"; argv[1] = path; g_files_spawn_status = (sys_spawn_args("/APPS/FILES", argv, 2) >= 0) ? 1 : -1; }
static void u_rescan(void) { g_cur = NULL; g_sel = 0; g_scroll = 0; g_files_spawn_status = 0; scan_start(); }

// ---------------------------------------------------------------------------
// Draw: Partitions tab
// ---------------------------------------------------------------------------
#define P_DISK_W 320
static int p_disk_x(void) { return content_x(); }
static int p_disk_y(void) { return content_y() + 30; }
static int p_disk_h(void) { return content_h() - 30; }
static int p_pane_x(void) { return content_x() + P_DISK_W + PAD; }
static int p_pane_w(void) { return content_w() - P_DISK_W - PAD; }

static void draw_partitions(void) {
    unsigned int cink = lum_ink(C_CARD), cdim = dim_ink(C_CARD);
    win_draw_text_ttf(win, content_x() + 2, content_y() + 6, "Block devices", 13, C_INK);
    gui_button(win, content_x() + content_w() - 90, content_y() + 2, 90, 22, "Refresh", GUI_BTN_SECONDARY, GUI_ST_NORMAL);

    // Disk list card
    int dx = p_disk_x(), dy = p_disk_y(), dw = P_DISK_W, dh = p_disk_h();
    gui_card(win, dx, dy, dw, dh);
    if (g_ndisks == 0) gui_text_ttf_centered(win, dx, dy + dh / 2 - 10, dw, 20, "No block devices", cdim, 13);
    int drh = 46;
    for (int i = 0; i < g_ndisks; i++) {
        int ry = dy + 8 + i * drh; if (ry + drh > dy + dh) break;
        int sel = (i == g_disk_sel);
        if (sel) gui_fill_rounded_aa(win, dx + 4, ry, dw - 8, drh - 4, 5, tint(C_CARD, C_ACC, 22), C_CARD);
        unsigned int ink = cink;
        char l1[96], l2[96], sz[32];
        fmt_bytes(g_disks[i].sectors * SECTOR_BYTES, sz, sizeof sz);
        char model[41]; memcpy(model, g_disks[i].model, 40); model[40] = 0;
        snprintf(l1, sizeof l1, "%s %d  %s", kind_name(g_disks[i].kind), g_disks[i].index, model[0] ? model : "(disk)");
        char fit[96]; fit_text(l1, 13, dw - 24 - 60, fit, sizeof fit);
        win_draw_text_ttf(win, dx + 12, ry + 6, fit, 13, ink);
        snprintf(l2, sizeof l2, "%s  %s%s", sz, g_disks[i].is_removable ? "removable" : "fixed",
                 g_disks[i].kind == BLK_KIND_SCRATCH ? "  (scratch)" : "");
        win_draw_text_ttf(win, dx + 12, ry + 25, l2, 11, cdim);
        if (g_disks[i].is_boot) {
            int bw = 52; gui_fill_rounded_aa(win, dx + dw - bw - 12, ry + 8, bw, 18, 4, C_WARN, C_CARD);
            gui_text_ttf_centered(win, dx + dw - bw - 12, ry + 9, bw, 16, "BOOT", 0x00FFFFFFu, 11);
        }
    }

    // Partition pane
    int px = p_pane_x(), py = p_disk_y(), pw = p_pane_w(), ph = p_disk_h();
    blk_dev_t *d = cur_disk();
    int boot = (d && d->is_boot);
    // action buttons row
    int by = py, bh = 26, bx = px, bstep = 92;
    int can_edit = (d && !boot);
    gui_button(win, bx, by, 84, bh, "New", GUI_BTN_SECONDARY, can_edit ? GUI_ST_NORMAL : GUI_ST_DISABLED); bx += bstep;
    gui_button(win, bx, by, 84, bh, "Delete", GUI_BTN_SECONDARY, (can_edit && g_nlayout > 0) ? GUI_ST_NORMAL : GUI_ST_DISABLED); bx += bstep;
    gui_button(win, bx, by, 40, bh, "-", GUI_BTN_SECONDARY, (can_edit && g_nlayout > 0) ? GUI_ST_NORMAL : GUI_ST_DISABLED); bx += 48;
    gui_button(win, bx, by, 40, bh, "+", GUI_BTN_SECONDARY, (can_edit && g_nlayout > 0) ? GUI_ST_NORMAL : GUI_ST_DISABLED); bx += 48;
    gui_button(win, px + pw - 180, by, 84, bh, "Revert", GUI_BTN_SECONDARY, (can_edit && g_dirty) ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    gui_button(win, px + pw - 90, by, 90, bh, "Apply", GUI_BTN_DANGER, (can_edit && g_dirty && g_nlayout > 0) ? GUI_ST_NORMAL : GUI_ST_DISABLED);

    int ly = py + bh + 8;
    int lh = ph - bh - 8;
    gui_card(win, px, ly, pw, lh);
    if (boot) {
        gui_text_ttf_centered(win, px, ly + 20, pw, 20, "This is the BOOT disk", C_WARN, 14);
        gui_text_ttf_centered(win, px, ly + 46, pw, 18, "Partitioning is disabled to protect the running system.", cdim, 12);
        gui_text_ttf_centered(win, px, ly + 66, pw, 18, "(The kernel also refuses destructive writes to it.)", cdim, 11);
    }
    // header
    int hx = px + 12, hy = ly + 6;
    win_draw_text_ttf(win, hx, hy + 2, "#", 11, cdim);
    win_draw_text_ttf(win, hx + 28, hy + 2, "Name", 11, cdim);
    win_draw_text_ttf(win, hx + 150, hy + 2, "Start (LBA)", 11, cdim);
    win_draw_text_ttf(win, hx + 260, hy + 2, "Size", 11, cdim);
    win_draw_text_ttf(win, hx + 360, hy + 2, "Type", 11, cdim);
    win_draw_rect(win, px + 8, hy + HDRROW_H - 2, pw - 16, 1, C_BORDER);
    if (!d) { gui_text_ttf_centered(win, px, ly + lh / 2, pw, 20, "Select a disk", cdim, 13); }
    else if (g_nlayout == 0) {
        char msg[128];
        if (!g_gpt_ok) snprintf(msg, sizeof msg, "%s - New will create a fresh GPT", g_part_err[0] ? g_part_err : "no partitions");
        else snprintf(msg, sizeof msg, "No partitions (empty GPT)");
        gui_text_ttf_centered(win, px, ly + lh / 2, pw, 20, boot ? "" : msg, cdim, 12);
    } else {
        int vis = list_rows(lh);
        for (int i = 0; i < g_nlayout && i < vis; i++) {
            int ry = hy + HDRROW_H + i * ROW_H; int sel = (i == g_lay_sel);
            unsigned int ink = cink, dink = cdim;
            if (sel && !boot) { gui_fill_rounded_aa(win, px + 4, ry, pw - 8, ROW_H - 2, 4, C_SEL, C_CARD); ink = C_SELTX; dink = C_SELTX; }
            char b[64];
            snprintf(b, sizeof b, "%d", i); win_draw_text_ttf(win, hx, ry + 5, b, 12, dink);
            char nm[40]; int nl = 0; for (int c = 0; c < 39 && g_layout[i].name[c]; c++) nm[nl++] = g_layout[i].name[c]; nm[nl] = 0;
            char fit[64]; fit_text(nm[0] ? nm : "(unnamed)", 12, 116, fit, sizeof fit);
            win_draw_text_ttf(win, hx + 28, ry + 5, fit, 12, ink);
            snprintf(b, sizeof b, "%lu", (unsigned long)g_layout[i].start_lba); win_draw_text_ttf(win, hx + 150, ry + 5, b, 12, dink);
            char sz[32]; fmt_bytes(g_layout[i].size_lba * SECTOR_BYTES, sz, sizeof sz);
            win_draw_text_ttf(win, hx + 260, ry + 5, sz, 12, ink);
            const char *ty = (memcmp(g_layout[i].type_guid, GUID_LINUX, 16) == 0) ? "Linux" : "Data";
            win_draw_text_ttf(win, hx + 360, ry + 5, ty, 12, dink);
        }
        char hint[128];
        blk_dev_t *dd = cur_disk();
        char tot[32]; fmt_bytes((dd ? dd->sectors : 0) * SECTOR_BYTES, tot, sizeof tot);
        snprintf(hint, sizeof hint, "Disk %s  |  arrows: Left/Right disk, Up/Down layout  |  -/+ resize 4 MiB  |  dbl-click row to change type", tot);
        win_draw_text_ttf(win, px + 12, ly + lh - 20, hint, 11, cdim);
    }
    if (g_dirty && !boot)
        win_draw_text_ttf(win, px + 12, by + bh + 2 - 20 + lh, "", 11, cdim);
}
static int p_disk_row_at(int mx, int my) {
    int dx = p_disk_x(), dy = p_disk_y(), dw = P_DISK_W, dh = p_disk_h(), drh = 46;
    if (mx < dx || mx >= dx + dw) return -1;
    int i = (my - dy - 8) / drh;
    if (i < 0 || i >= g_ndisks) return -1;
    if (dy + 8 + i * drh + drh > dy + dh) return -1;
    return i;
}
static int p_part_row_at(int mx, int my) {
    int px = p_pane_x(), py = p_disk_y(), pw = p_pane_w();
    int ly = py + 26 + 8, hy = ly + 6, y0 = hy + HDRROW_H;
    if (mx < px || mx >= px + pw) return -1;
    if (my < y0) return -1;
    int i = (my - y0) / ROW_H;
    if (i < 0 || i >= g_nlayout) return -1;
    return i;
}
// PREPARE then open the type-to-confirm modal for a partition write.
static void partitions_apply(void) {
    blk_dev_t *d = cur_disk();
    if (!d || d->is_boot || g_nlayout == 0) return;
    part_token_t tok;
    int rc = part_prepare(d->kind, d->index, g_layout, g_nlayout, &tok);
    if (rc < 0) { banner(blkmgr_err(rc), C_WARN); return; }
    memcpy(g_op_nonce, tok.nonce, 16);
    g_op_dk = d->kind; g_op_di = d->index;
    memcpy(g_op_layout, g_layout, sizeof(part_spec_t) * (size_t)g_nlayout);
    g_op_nparts = g_nlayout;
    char model[41]; memcpy(model, d->model, 40); model[40] = 0;
    char *cf = model[0] ? model : (char *)kind_name(d->kind);
    char l0[110], l1[110], l2[110];
    snprintf(l0, sizeof l0, "Overwrite the partition table of %s %d (%s).", kind_name(d->kind), d->index, cf);
    snprintf(l1, sizeof l1, "Writes %d partition(s). ALL DATA on removed/changed", g_nlayout);
    snprintf(l2, sizeof l2, "partitions is PERMANENTLY LOST.");
    modal_open(MODAL_TYPECONFIRM, "Write partition table?", l0, l1, l2, cf, OP_PARTWRITE);
}

// ---------------------------------------------------------------------------
// Draw: Format tab
// ---------------------------------------------------------------------------
static int g_fmt_fs = MKFS_FAT;      // MKFS_FAT / MKFS_EXT2
static char g_fmt_label[36] = ""; static int g_fmt_label_len = 0;
static int g_fmt_focus = 0;          // 0 none, 1 label field

static void draw_format(void) {
    unsigned int cink = lum_ink(C_CARD), cdim = dim_ink(C_CARD);
    int cx = content_x(), cy = content_y() + 6, cw = content_w();
    gui_button(win, cx + cw - 90, cy - 4, 90, 22, "Refresh", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    win_draw_text_ttf(win, cx + 2, cy, "Create a filesystem on a partition", 13, C_INK);

    int cardx = cx, cardy = cy + 26, cardw = cw, cardh = content_h() - 26;
    gui_card(win, cardx, cardy, cardw, cardh);
    int x = cardx + 18, y = cardy + 16;
    blk_dev_t *d = cur_disk();
    int boot = (d && d->is_boot);

    char b[128];
    if (!d) { win_draw_text_ttf(win, x, y, "No disk selected (use the Partitions tab).", 13, cdim); return; }
    char model[41]; memcpy(model, d->model, 40); model[40] = 0;
    snprintf(b, sizeof b, "Disk:  %s %d  %s", kind_name(d->kind), d->index, model[0] ? model : "");
    win_draw_text_ttf(win, x, y, b, 13, cink);
    if (boot) { int bw = 52; gui_fill_rounded_aa(win, x + 340, y - 2, bw, 18, 4, C_WARN, C_CARD); gui_text_ttf_centered(win, x + 340, y - 1, bw, 16, "BOOT", 0x00FFFFFFu, 11); }
    y += 30;

    win_draw_text_ttf(win, x, y, "Partition:", 12, cdim); y += 22;
    if (g_nparts == 0) { win_draw_text_ttf(win, x + 12, y, g_gpt_ok ? "(no partitions - create some first)" : "(no GPT - use Partitions tab)", 12, cdim); y += 24; }
    for (int i = 0; i < g_nparts; i++) {
        int ry = y + i * ROW_H; int sel = (i == g_part_sel);
        if (sel) gui_fill_rounded_aa(win, x + 8, ry, cardw - 60, ROW_H - 2, 4, C_SEL, C_CARD);
        unsigned int ink = sel ? C_SELTX : cink;
        char sz[32]; fmt_bytes(g_parts[i].size_lba * SECTOR_BYTES, sz, sizeof sz);
        snprintf(b, sizeof b, "  [%d] %-10s  %s  start %lu", g_parts[i].slot, g_parts[i].name[0] ? g_parts[i].name : "(unnamed)", sz, (unsigned long)g_parts[i].start_lba);
        win_draw_text_ttf(win, x + 12, ry + 5, b, 12, ink);
    }
    y += (g_nparts > 0 ? g_nparts * ROW_H : 24) + 14;

    win_draw_text_ttf(win, x, y, "Filesystem:", 12, cdim);
    gui_button(win, x + 110, y - 4, 90, 26, "FAT", g_fmt_fs == MKFS_FAT ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    gui_button(win, x + 208, y - 4, 90, 26, "ext2", g_fmt_fs == MKFS_EXT2 ? GUI_BTN_PRIMARY : GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    y += 40;

    win_draw_text_ttf(win, x, y, "Label:", 12, cdim);
    gui_textfield_tf(win, x + 110, y - 4, 200, 28, g_fmt_label, g_fmt_label_len, g_fmt_label_len, -1, g_fmt_focus == 1, "(optional)");
    y += 42;

    int can = (d && !boot && g_nparts > 0);
    gui_button(win, x, y, 160, 32, "Format partition", GUI_BTN_DANGER, can ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    if (boot) win_draw_text_ttf(win, x + 176, y + 8, "Boot disk is protected", 12, C_WARN);
    else if (g_nparts == 0) win_draw_text_ttf(win, x + 176, y + 8, "Create a partition first (Partitions tab)", 12, cdim);
}
static int fmt_part_row_at(int mx, int my) {
    (void)mx;
    int cardy = content_y() + 6 + 26;
    int y0 = cardy + 16 + 30 + 22;   // matches draw_format partition list start
    int i = (my - y0) / ROW_H;
    if (i < 0 || i >= g_nparts) return -1;
    return i;
}
static void format_apply(void) {
    blk_dev_t *d = cur_disk();
    if (!d || d->is_boot || g_nparts == 0) return;
    if (g_part_sel < 0 || g_part_sel >= g_nparts) return;
    g_op_dk = d->kind; g_op_di = d->index; g_op_pi = g_parts[g_part_sel].slot; g_op_fs = g_fmt_fs;
    snprintf(g_op_label, sizeof g_op_label, "%s", g_fmt_label);
    char model[41]; memcpy(model, d->model, 40); model[40] = 0;
    char *cf = model[0] ? model : (char *)kind_name(d->kind);
    char l0[110], l1[110], l2[110];
    char sz[32]; fmt_bytes(g_parts[g_part_sel].size_lba * SECTOR_BYTES, sz, sizeof sz);
    snprintf(l0, sizeof l0, "Format partition %d (%s) on %s %d", g_parts[g_part_sel].slot, sz, kind_name(d->kind), d->index);
    snprintf(l1, sizeof l1, "as %s. This ERASES everything on that", g_fmt_fs == MKFS_EXT2 ? "ext2" : "FAT");
    snprintf(l2, sizeof l2, "partition permanently.");
    modal_open(MODAL_TYPECONFIRM, "Format partition?", l0, l1, l2, cf, OP_MKFS);
}

// ---------------------------------------------------------------------------
// Draw: Mount tab
// ---------------------------------------------------------------------------
static void draw_mount(void) {
    unsigned int cink = lum_ink(C_CARD), cdim = dim_ink(C_CARD);
    int cx = content_x(), cy = content_y() + 6, cw = content_w();
    gui_button(win, cx + cw - 90, cy - 4, 90, 22, "Refresh", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    win_draw_text_ttf(win, cx + 2, cy, "Mount table", 13, C_INK);

    int cardx = cx, cardy = cy + 26, cardw = cw, cardh = content_h() - 26 - 44;
    gui_card(win, cardx, cardy, cardw, cardh);
    int hx = cardx + 12, hy = cardy + 6;
    win_draw_text_ttf(win, hx, hy + 2, "Mount point", 11, cdim);
    win_draw_text_ttf(win, hx + 160, hy + 2, "Type", 11, cdim);
    win_draw_text_ttf(win, hx + 240, hy + 2, "Size", 11, cdim);
    win_draw_text_ttf(win, hx + 340, hy + 2, "Free", 11, cdim);
    win_draw_text_ttf(win, hx + 440, hy + 2, "Flags", 11, cdim);
    win_draw_rect(win, cardx + 8, hy + HDRROW_H - 2, cardw - 16, 1, C_BORDER);
    if (g_nmounts == 0) gui_text_ttf_centered(win, cardx, cardy + cardh / 2, cardw, 20, "No mounts", cdim, 13);
    int vis = list_rows(cardh);
    for (int i = 0; i < g_nmounts && i < vis; i++) {
        mount_ent_t *m = &g_mounts[i];
        int ry = hy + HDRROW_H + i * ROW_H; int sel = (i == g_mount_sel);
        unsigned int ink = cink, dink = cdim;
        if (sel) { gui_fill_rounded_aa(win, cardx + 4, ry, cardw - 8, ROW_H - 2, 4, C_SEL, C_CARD); ink = C_SELTX; dink = C_SELTX; }
        char fit[64]; fit_text(m->path, 12, 150, fit, sizeof fit);
        win_draw_text_ttf(win, hx, ry + 5, fit, 12, ink);
        win_draw_text_ttf(win, hx + 160, ry + 5, m->fstype, 12, dink);
        char sz[32];
        if (m->total_bytes) { fmt_bytes(m->total_bytes, sz, sizeof sz); win_draw_text_ttf(win, hx + 240, ry + 5, sz, 12, dink); }
        if (m->free_bytes) { fmt_bytes(m->free_bytes, sz, sizeof sz); win_draw_text_ttf(win, hx + 340, ry + 5, sz, 12, dink); }
        char fl[48]; fl[0] = 0;
        if (m->flags & MNT_F_ROOT) strcat(fl, "root ");
        else if (m->flags & MNT_F_BOOT) strcat(fl, "boot ");
        if (m->flags & MNT_F_DYNAMIC) strcat(fl, "dyn ");
        if (m->flags & MNT_F_RO) strcat(fl, "ro");
        win_draw_text_ttf(win, hx + 440, ry + 5, fl, 11, dink);
    }
    // action row
    int ay = cardy + cardh + 10;
    blk_dev_t *d = cur_disk();
    int can_mount = (d && g_nparts > 0 && g_part_sel >= 0 && g_part_sel < g_nparts);
    char mp[64];
    if (can_mount) snprintf(mp, sizeof mp, "Mount %s %d part %d at /MNT/S%dP%d", kind_name(d->kind), d->index, g_parts[g_part_sel].slot, d->index, g_parts[g_part_sel].slot);
    else snprintf(mp, sizeof mp, "Select a disk+partition on the Partitions tab to mount");
    win_draw_text_ttf(win, cx + 2, ay + 6, mp, 12, cdim);
    gui_button(win, cx + cw - 210, ay, 100, 30, "Mount", GUI_BTN_SECONDARY, can_mount ? GUI_ST_NORMAL : GUI_ST_DISABLED);
    int sel_dyn = (g_mount_sel >= 0 && g_mount_sel < g_nmounts && (g_mounts[g_mount_sel].flags & MNT_F_DYNAMIC) && !(g_mounts[g_mount_sel].flags & MNT_F_BOOT));
    gui_button(win, cx + cw - 100, ay, 100, 30, "Unmount", GUI_BTN_SECONDARY, sel_dyn ? GUI_ST_NORMAL : GUI_ST_DISABLED);
}
static int mount_row_at(int mx, int my) {
    int cardy = content_y() + 6 + 26, hy = cardy + 6, y0 = hy + HDRROW_H;
    (void)mx;
    if (my < y0) return -1;
    int i = (my - y0) / ROW_H;
    if (i < 0 || i >= g_nmounts) return -1;
    return i;
}
static void mount_do(void) {
    blk_dev_t *d = cur_disk();
    if (!d || g_nparts == 0 || g_part_sel < 0 || g_part_sel >= g_nparts) return;
    g_op_dk = d->kind; g_op_di = d->index; g_op_pi = g_parts[g_part_sel].slot;
    snprintf(g_op_path, sizeof g_op_path, "/MNT/S%dP%d", d->index, g_parts[g_part_sel].slot);
    g_op_kind = OP_MOUNT; op_start();
}
static void umount_do(void) {
    if (g_mount_sel < 0 || g_mount_sel >= g_nmounts) return;
    mount_ent_t *m = &g_mounts[g_mount_sel];
    if (!(m->flags & MNT_F_DYNAMIC) || (m->flags & MNT_F_BOOT)) return;
    snprintf(g_op_path, sizeof g_op_path, "%s", m->path);
    g_op_kind = OP_UMOUNT; op_start();
}

// ---------------------------------------------------------------------------
// Draw: Removable tab
// ---------------------------------------------------------------------------
static void draw_removable(void) {
    unsigned int cink = lum_ink(C_CARD), cdim = dim_ink(C_CARD);
    int cx = content_x(), cy = content_y() + 6, cw = content_w();
    gui_button(win, cx + cw - 90, cy - 4, 90, 22, "Refresh", GUI_BTN_SECONDARY, GUI_ST_NORMAL);
    win_draw_text_ttf(win, cx + 2, cy, "Removable devices", 13, C_INK);

    int cardx = cx, cardy = cy + 26, cardw = cw, cardh = content_h() - 26 - 44;
    gui_card(win, cardx, cardy, cardw, cardh);
    int hx = cardx + 12, hy = cardy + 6;
    win_draw_text_ttf(win, hx, hy + 2, "Device", 11, cdim);
    win_draw_text_ttf(win, hx + 260, hy + 2, "Mount", 11, cdim);
    win_draw_text_ttf(win, hx + 360, hy + 2, "FS", 11, cdim);
    win_draw_text_ttf(win, hx + 430, hy + 2, "Size", 11, cdim);
    win_draw_text_ttf(win, hx + 540, hy + 2, "State", 11, cdim);
    win_draw_rect(win, cardx + 8, hy + HDRROW_H - 2, cardw - 16, 1, C_BORDER);
    if (g_nvols == 0) gui_text_ttf_centered(win, cardx, cardy + cardh / 2, cardw, 20, "No removable devices", cdim, 13);
    int vis = list_rows(cardh);
    for (int i = 0; i < g_nvols && i < vis; i++) {
        sc_volume_t *v = &g_vols[i];
        int ry = hy + HDRROW_H + i * ROW_H; int sel = (i == g_vol_sel);
        unsigned int ink = cink, dink = cdim;
        if (sel) { gui_fill_rounded_aa(win, cardx + 4, ry, cardw - 8, ROW_H - 2, 4, C_SEL, C_CARD); ink = C_SELTX; dink = C_SELTX; }
        char fit[80]; fit_text(v->name, 12, 250, fit, sizeof fit);
        win_draw_text_ttf(win, hx, ry + 5, fit, 12, ink);
        win_draw_text_ttf(win, hx + 260, ry + 5, v->mount, 12, dink);
        win_draw_text_ttf(win, hx + 360, ry + 5, v->fsname, 12, dink);
        char sz[32];
        if (v->total_bytes) { fmt_bytes(v->total_bytes, sz, sizeof sz); win_draw_text_ttf(win, hx + 430, ry + 5, sz, 12, dink); }
        const char *state = (v->flags & MOSVOL_MOUNTED) ? "mounted" : "present";
        win_draw_text_ttf(win, hx + 540, ry + 5, state, 12, dink);
    }
    // action row
    int ay = cardy + cardh + 10;
    sc_volume_t *v = (g_vol_sel >= 0 && g_vol_sel < g_nvols) ? &g_vols[g_vol_sel] : NULL;
    int can_eject = (v && (v->flags & MOSVOL_REMOVABLE));
    if (v) { char b[96]; snprintf(b, sizeof b, "Selected: %s", v->name); char fit[96]; fit_text(b, 12, cw - 260, fit, sizeof fit); win_draw_text_ttf(win, cx + 2, ay + 6, fit, 12, cdim); }
    else win_draw_text_ttf(win, cx + 2, ay + 6, "No device selected", 12, cdim);
    gui_button(win, cx + cw - 130, ay, 130, 30, "Safe eject", GUI_BTN_PRIMARY, can_eject ? GUI_ST_NORMAL : GUI_ST_DISABLED);
}
static int vol_row_at(int mx, int my) {
    int cardy = content_y() + 6 + 26, hy = cardy + 6, y0 = hy + HDRROW_H;
    (void)mx;
    if (my < y0) return -1;
    int i = (my - y0) / ROW_H;
    if (i < 0 || i >= g_nvols) return -1;
    return i;
}
static void eject_do(void) {
    sc_volume_t *v = (g_vol_sel >= 0 && g_vol_sel < g_nvols) ? &g_vols[g_vol_sel] : NULL;
    if (!v || !(v->flags & MOSVOL_REMOVABLE)) return;
    g_op_vi = v->index;
    char l0[110]; snprintf(l0, sizeof l0, "Flush and eject %s?", v->name);
    modal_open(MODAL_CONFIRM, "Safe eject", l0, "The device will be unmounted and flushed.", NULL, NULL, OP_EJECT);
}

// ---------------------------------------------------------------------------
// Master draw
// ---------------------------------------------------------------------------
static void draw(void) {
    apply_style();
    win_get_size(win, &DW, &DH);
    if (DW < 640) DW = WIN_W;
    if (DH < 400) DH = WIN_H;
    u_clamp();

    win_draw_rect(win, 0, 0, DW, DH, C_BG);
    win_draw_text_ttf(win, PAD + 2, 9, "Disk Manager", 17, C_INK);

    // Tab bar
    int tw = tab_w();
    for (int i = 0; i < TAB_COUNT; i++) {
        int tx = PAD + i * tw;
        int active = (i == g_tab);
        if (active) gui_fill_rounded_aa(win, tx + 2, HDR_H + 2, tw - 4, TABBAR_H - 2, 5, C_CARD, C_BG);
        gui_text_ttf_centered(win, tx, HDR_H + 8, tw, 18, g_tab_name[i], active ? C_INK : C_DIM, 13);
        if (active) win_draw_rect(win, tx + 2, HDR_H + TABBAR_H - 1, tw - 4, 2, C_ACC);
    }
    win_draw_rect(win, 0, HDR_H + TABBAR_H, DW, 1, C_BORDER);

    switch (g_tab) {
        case TAB_USAGE:  draw_usage(); break;
        case TAB_PART:   draw_partitions(); break;
        case TAB_FORMAT: draw_format(); break;
        case TAB_MOUNT:  draw_mount(); break;
        case TAB_REMOV:  draw_removable(); break;
    }

    // Status / banner line
    const char *hint = "";
    if (op_running()) hint = "Working...";
    else if (g_banner[0] && now_ms() - g_banner_ms < 6000) hint = g_banner;
    else switch (g_tab) {
        case TAB_USAGE:  hint = "Enter/double-click: open   Backspace: up   R: rescan"; break;
        case TAB_PART:   hint = "Select a disk, edit the layout, then Apply. Boot disk is locked."; break;
        case TAB_FORMAT: hint = "Pick a partition + filesystem, then Format (type-to-confirm)."; break;
        case TAB_MOUNT:  hint = "Mount the Partitions-tab selection at /MNT/, or unmount a dynamic mount."; break;
        case TAB_REMOV:  hint = "Select a removable device and Safe eject."; break;
    }
    unsigned int hc = (g_banner[0] && now_ms() - g_banner_ms < 6000 && !op_running()) ? g_banner_col : C_DIM;
    char fit[LINEW]; fit_text(hint, 11, DW - 2 * PAD, fit, sizeof fit);
    win_draw_text_ttf(win, PAD + 2, DH - STATUS_H + 5, fit, 11, hc);

    if (g_modal) modal_render();
    win_invalidate(win);
}

// ---------------------------------------------------------------------------
// Tab switching (loads that tab's data)
// ---------------------------------------------------------------------------
static void enter_tab(int t) {
    g_tab = t;
    switch (t) {
        case TAB_PART:
            refresh_disks(); parse_parts(); layout_from_disk(); break;
        case TAB_FORMAT:
            refresh_disks(); parse_parts(); break;
        case TAB_MOUNT:
            refresh_mounts(); break;
        case TAB_REMOV:
            refresh_vols(); break;
        default: break;
    }
}

// consume a finished worker op
static void consume_op(void) {
    if (__atomic_load_n(&g_ops, __ATOMIC_ACQUIRE) != OPS_DONE) return;
    if (g_opth_live) { pthread_join(g_opth, NULL); g_opth_live = 0; }
    __atomic_store_n(&g_ops, OPS_IDLE, __ATOMIC_RELEASE);
    banner(g_op_msg, g_op_rc >= 0 ? C_OK : C_WARN);
    // Refresh whatever the op affected.
    switch (g_op_kind) {
        case OP_PARTWRITE: refresh_disks(); parse_parts(); layout_from_disk(); break;
        case OP_MKFS:      parse_parts(); break;
        case OP_MOUNT:
        case OP_UMOUNT:    refresh_mounts(); break;
        case OP_EJECT:     refresh_vols(); break;
    }
    g_op_kind = 0;
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
static void modal_key(gui_event_t *ev) {
    char c = ev->key_char; uint32_t kc = ev->keycode;
    if (c == 27) { modal_close(); return; }
    if (g_modal == MODAL_NOTICE) { if (c == '\n' || c == '\r' || kc == GUI_KEY_ENTER) modal_close(); return; }
    if (g_modal == MODAL_TYPECONFIRM) {
        if (c == 8 || kc == GUI_KEY_BKSP) { if (g_confirm_len > 0) g_confirm_input[--g_confirm_len] = 0; return; }
        if (c == '\n' || c == '\r' || kc == GUI_KEY_ENTER) { if (modal_can_confirm()) modal_do_confirm(); return; }
        if (c >= 32 && c < 127 && g_confirm_len < (int)sizeof(g_confirm_input) - 1) { g_confirm_input[g_confirm_len++] = c; g_confirm_input[g_confirm_len] = 0; }
        return;
    }
    // MODAL_CONFIRM
    if (c == '\n' || c == '\r' || kc == GUI_KEY_ENTER) { if (modal_can_confirm()) modal_do_confirm(); }
}
static void modal_mouse(int mx, int my) {
    int mw = modal_w(), mh = modal_h(), mmx = modal_x(), mmy = modal_y();
    int by = mmy + mh - 44, bw = 120, bh = 30;
    if (my < by || my >= by + bh) return;
    if (g_modal == MODAL_NOTICE) { if (mx >= mmx + mw - 20 - bw && mx < mmx + mw - 20) modal_close(); return; }
    if (mx >= mmx + 20 && mx < mmx + 20 + bw) { modal_close(); return; }        // Cancel
    if (mx >= mmx + mw - 20 - bw && mx < mmx + mw - 20) { if (modal_can_confirm()) modal_do_confirm(); } // Action
}

static void handle_key(gui_event_t *ev) {
    if (g_modal) { modal_key(ev); return; }
    char c = ev->key_char; uint32_t kc = ev->keycode;
    if (c == 27) { /* Esc: quit only from Usage; elsewhere ignore to avoid accidental close */ }
    // Global tab nav: Ctrl not tracked; use Tab key to cycle.
    if (kc == GUI_KEY_TAB || c == '\t') { enter_tab((g_tab + 1) % TAB_COUNT); return; }
    if (c >= '1' && c <= '5') { enter_tab(c - '1'); return; }
    switch (g_tab) {
        case TAB_USAGE:
            if (c == 27) { /* quit handled in loop */ break; }
            if (c == 'r' || c == 'R') { u_rescan(); break; }
            if (c == 'f' || c == 'F') { u_open_files(); break; }
            if (!g_cur) break;
            if (kc == GUI_KEY_UP) g_sel--;
            else if (kc == GUI_KEY_DOWN) g_sel++;
            else if (kc == GUI_KEY_PGUP) g_sel -= u_rows_vis();
            else if (kc == GUI_KEY_PGDN) g_sel += u_rows_vis();
            else if (kc == GUI_KEY_HOME) g_sel = 0;
            else if (kc == GUI_KEY_END) g_sel = g_cur->nkids - 1;
            else if (kc == GUI_KEY_ENTER || c == '\n' || c == '\r' || kc == GUI_KEY_RIGHT) u_go_into();
            else if (c == 8 || kc == GUI_KEY_LEFT) u_go_up();
            break;
        case TAB_PART:
            // Up/Down move within the selected disk's layout; Left/Right (or
            // PageUp/PageDown) switch which disk is selected. Disk selection was
            // mouse-only, and headless/VM mouse clicks do not land (#334), so
            // keyboard disk selection is required to drive this tab (2026-09-23).
            if (kc == GUI_KEY_UP && g_lay_sel > 0) g_lay_sel--;
            else if (kc == GUI_KEY_DOWN && g_lay_sel < g_nlayout - 1) g_lay_sel++;
            else if ((kc == GUI_KEY_LEFT || kc == GUI_KEY_PGUP) && g_disk_sel > 0) {
                g_disk_sel--; parse_parts(); layout_from_disk();
            } else if ((kc == GUI_KEY_RIGHT || kc == GUI_KEY_PGDN) && g_disk_sel < g_ndisks - 1) {
                g_disk_sel++; parse_parts(); layout_from_disk();
            }
            break;
        case TAB_FORMAT:
            if (g_fmt_focus == 1) {
                if (c == 8 || kc == GUI_KEY_BKSP) { if (g_fmt_label_len > 0) g_fmt_label[--g_fmt_label_len] = 0; }
                else if (c >= 32 && c < 127 && g_fmt_label_len < (int)sizeof(g_fmt_label) - 1) { g_fmt_label[g_fmt_label_len++] = c; g_fmt_label[g_fmt_label_len] = 0; }
            } else {
                if (kc == GUI_KEY_UP && g_part_sel > 0) g_part_sel--;
                else if (kc == GUI_KEY_DOWN && g_part_sel < g_nparts - 1) g_part_sel++;
            }
            break;
        case TAB_MOUNT:
            if (kc == GUI_KEY_UP && g_mount_sel > 0) g_mount_sel--;
            else if (kc == GUI_KEY_DOWN && g_mount_sel < g_nmounts - 1) g_mount_sel++;
            break;
        case TAB_REMOV:
            if (kc == GUI_KEY_UP && g_vol_sel > 0) g_vol_sel--;
            else if (kc == GUI_KEY_DOWN && g_vol_sel < g_nvols - 1) g_vol_sel++;
            break;
    }
}

static void handle_mouse_down(gui_event_t *ev) {
    int mx = ev->mouse_x, my = ev->mouse_y;
    if (g_modal) { modal_mouse(mx, my); return; }
    // tab bar
    int t = tab_at(mx, my);
    if (t >= 0) { enter_tab(t); return; }
    switch (g_tab) {
        case TAB_USAGE: {
            if (my >= u_btn_y() && my < u_btn_y() + U_BTN_H) {
                if (mx >= u_btn_rescan_x() && mx < u_btn_rescan_x() + 96) { u_rescan(); return; }
                if (mx >= u_btn_up_x() && mx < u_btn_up_x() + 60) { u_go_up(); return; }
                if (mx >= u_btn_files_x() && mx < u_btn_files_x() + 68) { u_open_files(); return; }
            }
            int idx = u_row_at(mx, my);
            if (idx >= 0) {
                long now = now_ms();
                if (idx == g_last_click_row && now - g_last_click_ms < DBLCLICK_MS) { g_sel = idx; u_go_into(); g_last_click_row = -1; }
                else { g_sel = idx; g_last_click_row = idx; g_last_click_ms = now; }
            }
            break;
        }
        case TAB_PART: {
            if (my >= content_y() + 2 && my < content_y() + 24 && mx >= content_x() + content_w() - 90) { refresh_disks(); parse_parts(); layout_from_disk(); return; }
            int di = p_disk_row_at(mx, my);
            if (di >= 0) { if (di != g_disk_sel) { g_disk_sel = di; parse_parts(); layout_from_disk(); } return; }
            // action buttons
            blk_dev_t *d = cur_disk(); int boot = (d && d->is_boot);
            int by = p_disk_y(), bh = 26;
            if (my >= by && my < by + bh && d && !boot) {
                int px = p_pane_x(), pw = p_pane_w(), bx = px;
                if (mx >= bx && mx < bx + 84) { layout_new_part(1); return; } bx += 92;
                if (mx >= bx && mx < bx + 84) { layout_delete(); return; } bx += 92;
                if (mx >= bx && mx < bx + 40) { layout_resize(0); return; } bx += 48;
                if (mx >= bx && mx < bx + 40) { layout_resize(1); return; }
                if (mx >= px + pw - 180 && mx < px + pw - 96) { layout_from_disk(); banner("Reverted to on-disk layout", C_DIM); return; }
                if (mx >= px + pw - 90 && mx < px + pw && g_dirty && g_nlayout > 0) { partitions_apply(); return; }
            }
            int pi = p_part_row_at(mx, my);
            if (pi >= 0) {
                long now = now_ms();
                if (pi == g_last_click_row && now - g_last_click_ms < DBLCLICK_MS && !boot) { g_lay_sel = pi; layout_toggle_type(); g_last_click_row = -1; }
                else { g_lay_sel = pi; g_last_click_row = pi; g_last_click_ms = now; }
                return;
            }
            break;
        }
        case TAB_FORMAT: {
            int cx = content_x(), cy = content_y() + 6, cw = content_w();
            if (my >= cy - 4 && my < cy + 18 && mx >= cx + cw - 90) { refresh_disks(); parse_parts(); return; }
            int cardx = cx, cardy = cy + 26; int x = cardx + 18;
            // fs buttons and label field and format button need y computed like draw_format
            int y = cardy + 16 + 30 + 22; // partition list start
            int listend = y + (g_nparts > 0 ? g_nparts * ROW_H : 24) + 14;
            int pi = fmt_part_row_at(mx, my);
            if (pi >= 0 && my < listend) { g_part_sel = pi; g_fmt_focus = 0; return; }
            int fsy = listend;                 // filesystem row (buttons at fsy-4)
            if (my >= fsy - 4 && my < fsy + 22) {
                if (mx >= x + 110 && mx < x + 200) { g_fmt_fs = MKFS_FAT; return; }
                if (mx >= x + 208 && mx < x + 298) { g_fmt_fs = MKFS_EXT2; return; }
            }
            int laby = fsy + 40;               // label field at laby-4
            if (my >= laby - 4 && my < laby + 24 && mx >= x + 110 && mx < x + 310) { g_fmt_focus = 1; return; }
            g_fmt_focus = 0;
            int fmty = laby + 42;              // format button
            if (my >= fmty && my < fmty + 32 && mx >= x && mx < x + 160) { format_apply(); return; }
            break;
        }
        case TAB_MOUNT: {
            int cx = content_x(), cy = content_y() + 6, cw = content_w();
            if (my >= cy - 4 && my < cy + 18 && mx >= cx + cw - 90) { refresh_mounts(); return; }
            int mi = mount_row_at(mx, my);
            if (mi >= 0) { g_mount_sel = mi; return; }
            int cardh = content_h() - 26 - 44, cardy = cy + 26;
            int ay = cardy + cardh + 10;
            if (my >= ay && my < ay + 30) {
                if (mx >= cx + cw - 210 && mx < cx + cw - 110) { mount_do(); return; }
                if (mx >= cx + cw - 100 && mx < cx + cw) { umount_do(); return; }
            }
            break;
        }
        case TAB_REMOV: {
            int cx = content_x(), cy = content_y() + 6, cw = content_w();
            if (my >= cy - 4 && my < cy + 18 && mx >= cx + cw - 90) { refresh_vols(); return; }
            int vi = vol_row_at(mx, my);
            if (vi >= 0) { g_vol_sel = vi; return; }
            int cardh = content_h() - 26 - 44, cardy = cy + 26;
            int ay = cardy + cardh + 10;
            if (my >= ay && my < ay + 30 && mx >= cx + cw - 130 && mx < cx + cw) { eject_do(); return; }
            break;
        }
    }
}

static void handle_scroll(gui_event_t *ev) {
    if (g_modal) return;
    if (g_tab == TAB_USAGE && g_cur) {
        int d = ev->scroll_delta;
        g_scroll += (d > 0) ? 3 : -3;
        int vis = u_rows_vis();
        if (g_scroll > g_cur->nkids - vis) g_scroll = g_cur->nkids - vis;
        if (g_scroll < 0) g_scroll = 0;
        if (g_sel < g_scroll) g_sel = g_scroll;
        if (g_sel >= g_scroll + vis) g_sel = g_scroll + vis - 1;
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && argv[1] && argv[1][0] == '/') {
        size_t l = strlen(argv[1]);
        if (l < PATHMAX) memcpy(g_scan_root, argv[1], l + 1);
        size_t rl = strlen(g_scan_root);
        while (rl > 1 && g_scan_root[rl - 1] == '/') g_scan_root[--rl] = 0;
    }
    win = win_create("Disk Manager", 90, 50, WIN_W, WIN_H);
    if (win < 0) return 1;
    g_disk_total_mb = sys_get_disk_total();
    g_disk_free_mb  = sys_get_disk_free();
    g_banner[0] = 0; g_banner_col = C_DIM;
    scan_start();
    apply_style();
    draw();

    int running = 1;
    while (running) {
        gui_event_t ev;
        int scanning = (__atomic_load_n(&g_state, __ATOMIC_ACQUIRE) == ST_SCANNING);
        int busy = op_running() || (g_modal && !modal_settled());
        int timeout = (scanning || busy) ? 120 : 4000;
        int et = win_get_event(win, &ev, timeout);
        adopt_tree_if_done();
        consume_op();
        if (et == 0) { draw(); continue; }
        switch (ev.type) {
            case EVENT_REDRAW:
            case EVENT_RESIZE: draw(); break;
            case EVENT_WINDOW_CLOSE: running = 0; break;
            case EVENT_KEY_DOWN:
                if (!g_modal && g_tab == TAB_USAGE && ev.key_char == 27) { running = 0; break; }
                handle_key(&ev); draw(); break;
            case EVENT_MOUSE_DOWN: handle_mouse_down(&ev); draw(); break;
            case EVENT_MOUSE_SCROLL: handle_scroll(&ev); draw(); break;
            default: break;
        }
    }
    if (g_opth_live) pthread_join(g_opth, NULL);
    scan_stop_and_free();
    win_destroy(win);
    return 0;
}
