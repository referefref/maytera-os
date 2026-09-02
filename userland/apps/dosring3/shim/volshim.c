// volshim.c - #VOLAPI: the Ring-3 DOS host's view of mounted virtual CD volumes.
//
// ===========================================================================
// WHAT THIS REPLACES, AND WHY THE REPLACEMENT IS THIS SMALL
// ---------------------------------------------------------------------------
// The Ring-3 host compiles the kernel's own DOS sources, and that used to
// include dos/diskimg.c, dos/imgfile.c and dos/usbvol.c. Those three want RAW
// BLOCK READS: usbvol_probe_and_mount() walks the boot device's unpartitioned
// tail, and imgfile's IMGF_KIND_BLKDEV reads device ranges through blk_read().
// blk_read() is ABSENT in Ring 3 and must stay absent, so in Ring 3 that whole
// stack could only ever produce an EMPTY mount table. MEASURED consequence:
// Red Alert ran 16,759,601,960 instructions in Ring 3, read its local install,
// and then displayed "PLEASE INSERT A RED ALERT CD INTO THE CD-ROM DRIVE".
//
// THE OBVIOUS FIX WAS THE WRONG ONE AND WAS REJECTED. Exposing blk_read() to
// Ring 3 hands the DOS host the whole disk and destroys the boundary the port
// exists to create - a boundary that is measured, not asserted: the host runs
// as uid 1000 and an adversarial probe shows /CONFIG/SHADOW, KIMI.KEY, AUTHKEYS
// and ".."-traversal variants all DENIED, with positive controls proving the
// probe means something.
//
// SO THE HOST ASKS INSTEAD OF READING. This file is the negotiated contract's
// consumer side, and it is deliberately three quarters nothing:
//
//   * METADATA comes from the kernel through kb_volinfo() -> SYS_DISKIMG
//     VOLINFO: which letters hold a disc, what CLASS they are, each disc's own
//     LABEL, and each drive's geometry. That is EXACTLY the five functions the
//     rest of the DOS sources import from diskimg.c and no more; it was
//     measured, not guessed (grep of dos/*.c and exec/*.c excluding the three
//     files above: diskimg_letter_class, diskimg_is_mounted, diskimg_geometry,
//     diskimg_volume_label, diskimg_mscdex, and nothing else).
//
//   * CONTENT does not come through here AT ALL, and that is the whole design.
//     A guest's INT 21h 3Dh/3Fh reaches fat_open()/fat_read(), which kshim.c
//     already maps onto kb_open()/kb_read() -> the kernel's own open()/read().
//     The kernel's fs/fat.c has served a mounted image's subtree at
//     /WINDIR/DRIVE_<L> since #196, so those reads ALREADY land on the disc,
//     under this process's real credentials, through the same ~94-site
//     chokepoint every other path in the system uses. Nothing had to be built
//     for file access; only for identification.
//
// WHY NOT A SECOND ISO READER. Because there must not be one. The kernel parses
// ISO 9660 in rustkern/iso9660.rs (self-tested at boot over 14,536 vectors) and
// that stays the ONE parser. This file cannot parse anything: it has no image
// bytes to parse, only answers.
//
// WHAT A COMPROMISED DOS GUEST GETS OUT OF THIS FILE. A list of drive letters,
// their labels, and a directory path per volume. No LBA, no device, no channel/
// drive pair, and no host-side image path (the kernel's mediated view drops the
// one diskimg_info_t exposes). Every path it can then name is subject to
// perms_check() at open(), and every write beneath it is refused as read-only
// media by fs/fat.c regardless of who asks.
//
// MOUNT AND EJECT ARE NOT PROVIDED, ON PURPOSE. A Ring-3 guest does not get to
// change what is in the drives. diskimg_mount()/diskimg_eject() are simply not
// linked into this binary, so a guest cannot reach them by any route rather
// than being refused by a check someone has to remember to write.
// ===========================================================================
#include "types.h"
#include "serial.h"
#include "string.h"
#include "dos/diskimg.h"
#include "kbridge.h"

// The Rust drive-letter policy is compiled into this binary too, so class
// questions keep exactly ONE answer across Ring 0 and Ring 3 rather than a
// re-typed table that would drift.
extern uint32_t drvmap_class_rs(uint32_t idx);
extern void     drvmap_mscdex_rs(uint32_t mounted_mask, mscdex_info_t *out);

// One kernel query. Returns 1 and fills *v when the letter is describable to
// this process, 0 when it is not (bad index, or credentials that may not
// traverse that volume - and those are NOT distinguished here, because for
// every caller below "you may not see it" and "it is not there" have the same
// correct consequence: the guest is told there is no such drive).
static int vol_of(int idx, dimg_vol_t *v) {
    if (idx < 0 || idx > 25 || !v) return 0;
    memset(v, 0, sizeof *v);
    return (kb_volinfo(idx, v, (unsigned long)sizeof *v) == 0) ? 1 : 0;
}

static int idx_of(char letter) {
    if (letter >= 'a' && letter <= 'z') letter = (char)(letter - 'a' + 'A');
    if (letter < 'A' || letter > 'Z') return -1;
    return letter - 'A';
}

// ---------------------------------------------------------------------------
// The five imports, and nothing else.
// ---------------------------------------------------------------------------

// Class is pure policy with no disc involved, so it is answered from the SAME
// Rust function the kernel answers it from, with no syscall at all. A CD DRIVE
// existing is a different fact from a DISC being in it, and dospath.c relies on
// the difference.
int diskimg_letter_class(int idx) {
    return (idx < 0 || idx > 25) ? DISKIMG_CLASS_NONE : (int)drvmap_class_rs((uint32_t)idx);
}

int diskimg_is_mounted(char letter) {
    dimg_vol_t v;
    if (!vol_of(idx_of(letter), &v)) return 0;
    return (v.flags & DISKIMG_F_MOUNTED) ? 1 : 0;
}

// THE FUNCTION THE WHOLE FEATURE TURNS ON. Red Alert finds its disc by matching
// a volume LABEL through INT 21h 4Eh attr 8 against its own CD1/CD2 table, not
// by drive letter, so a gateway that exposed files but not labels would not have
// answered the question the guest actually asks. int21svc.c:713 calls this.
int diskimg_volume_label(char letter, char *out, int cap) {
    if (!out || cap < 1) return 0;
    out[0] = '\0';
    dimg_vol_t v;
    if (!vol_of(idx_of(letter), &v)) return 0;
    if (!(v.flags & DISKIMG_F_MOUNTED)) return 0;
    int i = 0;
    while (i < cap - 1 && i < (int)sizeof v.label && v.label[i]) { out[i] = v.label[i]; i++; }
    out[i] = '\0';
    return out[0] ? 1 : 0;
}

// INT 21h AX=4409h ("is this drive remote/a CD") and AH=36h (free space) both
// land here. The kernel computed these from the ISO descriptor or the FAT12 BPB;
// this hands the same numbers across rather than recomputing them from bytes
// this process does not have.
int diskimg_geometry(char letter, uint32_t *bps_out, uint32_t *spc_out,
                     uint32_t *clusters_out) {
    dimg_vol_t v;
    if (!vol_of(idx_of(letter), &v)) return 0;
    if (!(v.flags & DISKIMG_F_MOUNTED)) return 0;
    if (!v.bytes_per_sector || !v.sectors_per_cluster) return 0;
    if (bps_out)      *bps_out      = v.bytes_per_sector;
    if (spc_out)      *spc_out      = v.sectors_per_cluster;
    if (clusters_out) *clusters_out = v.total_clusters;
    return 1;
}

// MSCDEX's INT 2Fh AX=1500h/150Dh answers. Derived from the live mask by the
// same Rust function the kernel uses, so the count and the first-drive number a
// guest is told cannot disagree between Ring 0 and Ring 3.
void diskimg_mscdex(mscdex_info_t *out) {
    if (!out) return;
    uint32_t mask = 0;
    for (int i = 0; i < 26; i++) {
        dimg_vol_t v;
        if (vol_of(i, &v) && (v.flags & DISKIMG_F_MOUNTED)) mask |= (1u << i);
    }
    drvmap_mscdex_rs(mask, out);
}

// ---------------------------------------------------------------------------
// Boot-time report. One line per volume this process may see, so a run's log
// says WHICH discs the guest was offered rather than leaving it to be inferred
// from whether the guest complained. Called from dosmain.c before the guest
// starts. Prints even when there is nothing, because "no volumes" is the
// finding in the failure case and a silent absence is not.
// ---------------------------------------------------------------------------
void volshim_report(void) {
    int n = 0;
    for (int i = 0; i < 26; i++) {
        dimg_vol_t v;
        if (!vol_of(i, &v)) continue;
        if (!(v.flags & DISKIMG_F_MOUNTED)) continue;
        n++;
        kprintf("[VOLAPI] %c: label='%s' class=%u fmt=%u %s root=%s size=%llu\n",
                'A' + i, v.label, (unsigned)v.cls, (unsigned)v.fmt,
                (v.flags & DISKIMG_F_READONLY) ? "ro" : "rw",
                v.root, (unsigned long long)v.size);
    }
    kprintf("[VOLAPI] %d volume(s) visible to this process\n", n);
}
