// cdprobe.c - what a DOS guest can actually SEE of the mounted drives, reported
//             once at boot, plus the self-test for the 4Eh drive-root split.
//
// WHY THIS EXISTS. The owner's machine mounted five ISO 9660 data volumes on
// E:..I: (proved by the [USBVOL] lines in his /BOOTLOG.TXT) and two DOS games
// both reported they could not find their CD. Two investigations reasoned about
// the GAMES. The answer was one layer down and one function wide: a
// volume-label search spelled "E:\" never reached the label. Nothing in the
// tree reported the difference between "mounted" and "visible to a guest", so
// this does, for every letter, from the same functions INT 21h and INT 2Fh use.
#include "../types.h"
#include "diskimg.h"
#include "dospath.h"
#include "int21svc.h"   // dos_find_split: the SHIPPING split, not a copy of it
#include "../fs/bootlog.h"

extern int kprintf(const char *fmt, ...);
extern int drvmap_ioctl_attrword_rs(uint32_t cls);
extern int drvmap_ioctl_removable_rs(uint32_t cls);


static const char *clsname(int c) {
    switch (c) {
        case DISKIMG_CLASS_FLOPPY: return "FLOPPY";
        case DISKIMG_CLASS_FIXED:  return "FIXED";
        case DISKIMG_CLASS_CDROM:  return "CDROM";
        default:                   return "none";
    }
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// PROVE THE DRIVE-ROOT SPLIT, because the rule it enforces was previously only
// written down. Every vector is a path dos_resolve_path_ex() really produces
// for a spelling a DOS program really uses; the expected drive letter is what
// find_volume_step() needs in order to answer with a volume label at all.
//
// Vector 1 is the regression: "E:\" resolves to /WINDIR/DRIVE_E, and before
// 2026-08-29 this split to (/WINDIR, DRIVE_E) with no drive, which is why Red
// Alert said "PLEASE INSERT A RED ALERT CD" with the disc mounted.
static int split_selftest(int *out_total) {
    struct { const char *fp; const char *dir; const char *pat; char drv; } v[] = {
        // resolved path            expected dir          pat        drive
        { "/WINDIR/DRIVE_E",        "/WINDIR/DRIVE_E",    "",        'E' },
        { "/WINDIR/DRIVE_E/",       "/WINDIR/DRIVE_E",    "",        'E' },
        { "/WINDIR/DRIVE_H",        "/WINDIR/DRIVE_H",    "",        'H' },
        { "/WINDIR/DRIVE_I",        "/WINDIR/DRIVE_I",    "",        'I' },
        { "/WINDIR/DRIVE_E/*.*",    "/WINDIR/DRIVE_E",    "*.*",     'E' },
        { "/WINDIR/DRIVE_E/DW2",    "/WINDIR/DRIVE_E",    "DW2",     'E' },
        { "/WINDIR/DRIVE_E/DW2/*",  "/WINDIR/DRIVE_E/DW2","*",        0  },
        { "/DOS/RA/*.MIX",          "/DOS/RA",            "*.MIX",    0  },
        { "/WINDIR",                "/",                  "WINDIR",   0  },
        { "FOO.TXT",                "/",                  "FOO.TXT",  0  },
    };
    int n = (int)(sizeof(v) / sizeof(v[0]));
    int fails = 0;
    for (int i = 0; i < n; i++) {
        char d[192], p[64];
        dos_find_split(v[i].fp, d, (int)sizeof d, p, (int)sizeof p);
        char drv = dos_native_root_drive(d);
        if (!streq(d, v[i].dir) || !streq(p, v[i].pat) || drv != v[i].drv) {
            fails++;
            kprintf("[CDPROBE] split FAIL '%s' -> dir='%s' pat='%s' drive=%c "
                    "(wanted dir='%s' pat='%s' drive=%c)\n",
                    v[i].fp, d, p, drv ? drv : '-',
                    v[i].dir, v[i].pat, v[i].drv ? v[i].drv : '-');
        }
    }
    if (out_total) *out_total = n;
    return fails;
}

void cdprobe_report(void) {
    int total = 0;
    int fails = split_selftest(&total);
    kprintf("[CDPROBE] 4Eh drive-root split self-test: %d/%d vectors PASS%s\n",
            total - fails, total, fails ? "  *** FAIL ***" : "");
    bootlog_write("[CDPROBE] 4Eh drive-root split: %d/%d PASS", total - fails, total);

    mscdex_info_t mi;
    diskimg_mscdex(&mi);
    uint32_t mask = diskimg_mounted_mask();

    kprintf("[CDPROBE] mask=%08x count=%d dos_drive_count=%d "
            "MSCDEX{count=%u first=%u}\n",
            (unsigned)mask, diskimg_mount_count(), dos_drive_count(),
            (unsigned)mi.count, (unsigned)mi.first);
    bootlog_write("[CDPROBE] mask=%08x mounts=%d drivecount=%d mscdex count=%u first=%u",
                  (unsigned)mask, diskimg_mount_count(), dos_drive_count(),
                  (unsigned)mi.count, (unsigned)mi.first);

    for (int i = 0; i < 26; i++) {
        char L = (char)('A' + i);
        int cls = diskimg_letter_class(i);
        int mnt = diskimg_is_mounted(L);
        // Report the drives that EXIST plus every mounted one. An empty CD
        // letter is not interesting; a mounted one a guest cannot see is.
        if (cls == DISKIMG_CLASS_CDROM && !mnt) continue;
        if (cls == DISKIMG_CLASS_NONE && !mnt) continue;
        char lbl[16];
        if (!diskimg_volume_label(L, lbl, (int)sizeof lbl)) lbl[0] = '\0';
        kprintf("[CDPROBE] %c: class=%s mounted=%d known=%d type=%d writable=%d "
                "gen=%u label='%s' 4408h=%d 4409h=%04x\n",
                L, clsname(cls), mnt, dos_drive_known(L), dos_drive_type(L),
                dos_drive_writable(L), (unsigned)diskimg_generation(L), lbl,
                drvmap_ioctl_removable_rs((uint32_t)cls),
                (unsigned)drvmap_ioctl_attrword_rs((uint32_t)cls));
        if (mnt)
            bootlog_write("[CDPROBE] %c: %s known=%d type=%d label='%s'",
                          L, clsname(cls), dos_drive_known(L), dos_drive_type(L), lbl);
    }
}
