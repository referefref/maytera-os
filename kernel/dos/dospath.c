// dospath.c - WINE-dosdevices-style drive-letter filesystem layer (#257)
// See dospath.h for the design + the dos_resolve_path contract.
#include "dospath.h"
#include "../serial.h"
#include "../string.h"
#include "../fs/fat.h"

// Root FS access (the fat_* public wrappers route "/" paths to the ext2 root on
// VM 1006; on a FAT-root system they hit FAT directly). Same handle used by the
// DOS + Win16 file code.
extern fat_fs_t g_fat_fs;
extern int   fat_mkdir(fat_fs_t *fs, const char *path);
extern int   fat_exists(fat_fs_t *fs, const char *path);
extern int   fat_write_file(fat_fs_t *fs, const char *path, const void *data, uint32_t size);

#define WINDIR_ROOT "/WINDIR"

static char g_cur_drive = 'C';   // current default DOS drive

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

int dos_drive_known(char letter) {
    char d = up(letter);
    return (d == 'A' || d == 'C' || d == 'E');
}

int dos_drive_type(char letter) {
    switch (up(letter)) {
        case 'A': return DOS_DRIVE_REMOVABLE;   // floppy
        case 'C': return DOS_DRIVE_FIXED;        // hard disk
        case 'E': return DOS_DRIVE_REMOTE;       // CD-ROM (Win16 has no CDROM type)
        default:  return DOS_DRIVE_NO_ROOT;
    }
}

int dos_drive_writable(char letter) {
    char d = up(letter);
    return (d == 'A' || d == 'C');               // E: (CD) is read-only
}

char dos_current_drive(void) { return g_cur_drive; }

void dos_set_current_drive(char letter) {
    if (dos_drive_known(letter)) g_cur_drive = up(letter);
}

int dos_drive_count(void) { return 5; }          // A..E logical span

int dos_path_writable(const char *in) {
    if (!in || !in[0]) return 1;
    if (in[0] == '/' || in[0] == '\\') return 1;     // native / root-relative
    if (in[1] == ':') return dos_drive_writable(in[0]);
    return dos_drive_writable(g_cur_drive);           // bare relative -> current
}

extern int diskimg_is_mounted(char letter);   // dos/diskimg.c (#196)
int dos_drive_image_mounted(char letter) {
    // #196: a removable drive (A:/E:) with a disk image mounted reads from the
    // image instead of its /WINDIR folder (served in fat_read_file via the
    // diskimg_try_read hook). Returns 0 when nothing is mounted -> folder-backed.
    return diskimg_is_mounted(letter);
}

// Append src to out[*pn] (bounded), converting backslashes to slashes and
// collapsing any run of slashes to a single one. Does NOT uppercase (done last).
static void append_norm(char *out, int *pn, int outsz, const char *src) {
    int n = *pn;
    for (const char *p = src; *p && n < outsz - 1; p++) {
        char c = (*p == '\\') ? '/' : *p;
        if (c == '/' && n > 0 && out[n - 1] == '/') continue;   // collapse //
        out[n++] = c;
    }
    *pn = n;
    out[n] = '\0';
}

void dos_resolve_path(const char *in, const char *reldir, char *out, int outsz) {
    if (!out || outsz <= 0) return;
    out[0] = '\0';
    if (!in) return;
    int n = 0;

    // (1) Already a native absolute path: pass through unchanged (the launcher
    //     and most kernel callers use these, e.g. /WIN16/MSEP/CHIPS.EXE).
    if (in[0] == '/' || in[0] == '\\') {
        // Treat a leading single backslash with NO drive as the legacy
        // "root-relative" path (kept byte-identical to pre-#257 behavior).
        append_norm(out, &n, outsz, in);
        for (int i = 0; out[i]; i++) out[i] = up(out[i]);
        return;
    }

    // (2) Explicit drive prefix "X:" -> map to /WINDIR/DRIVE_X (the new feature).
    char drive = 0;
    const char *rest = in;
    if (in[0] && in[1] == ':') { drive = up(in[0]); rest = in + 2; }

    if (drive) {
        // base = /WINDIR/DRIVE_X
        char base[24];
        int bn = 0;
        for (const char *p = WINDIR_ROOT "/DRIVE_"; *p; p++) base[bn++] = *p;
        base[bn++] = drive; base[bn] = '\0';
        append_norm(out, &n, outsz, base);
        if (rest[0] != '/' && rest[0] != '\\') {
            // drive-relative ("X:FOO"): we do not track a per-drive CWD, so
            // resolve against the drive root.
            if (n < outsz - 1) out[n++] = '/';
            out[n] = '\0';
        } else if (rest[0]) {
            if (n < outsz - 1) out[n++] = '/';  // ensure separator before rest
            out[n] = '\0';
            while (*rest == '/' || *rest == '\\') rest++;   // skip leading slashes
        }
        append_norm(out, &n, outsz, rest);
    } else {
        // (3) No drive letter.
        // Bare relative ("CHIPS.DAT") -> reldir (caller CWD / Win16 app dir).
        // This preserves the legacy behavior the reference games rely on.
        if (reldir && reldir[0]) {
            append_norm(out, &n, outsz, reldir);
            if (n > 0 && out[n - 1] != '/' && n < outsz - 1) { out[n++] = '/'; out[n] = '\0'; }
            append_norm(out, &n, outsz, rest);
        } else {
            // No reldir: resolve under the current drive root.
            char base[24]; int bn = 0;
            for (const char *p = WINDIR_ROOT "/DRIVE_"; *p; p++) base[bn++] = *p;
            base[bn++] = g_cur_drive; base[bn] = '\0';
            append_norm(out, &n, outsz, base);
            if (n < outsz - 1) { out[n++] = '/'; out[n] = '\0'; }
            append_norm(out, &n, outsz, rest);
        }
    }

    for (int i = 0; out[i]; i++) out[i] = up(out[i]);
}

// Create one directory if it does not already exist (best effort).
static void ensure_dir(const char *path) {
    if (!fat_exists(&g_fat_fs, path)) fat_mkdir(&g_fat_fs, path);
}

// (#133) Seed a file with default contents only if it does not already exist, so
// a user's later edits to WIN.INI / SYSTEM.INI survive reboots. fat_exists is not
// ext2-routed, so probe via the routed reader (fat_read_file -> ext2) instead.
extern void *fat_read_file(fat_fs_t *fs, const char *path, unsigned int *size_out);
extern void  kfree(void *p);
static void seed_file(const char *path, const char *contents) {
    unsigned int sz = 0;
    void *d = fat_read_file(&g_fat_fs, path, &sz);
    if (d) kfree(d);
    if (d && sz > 0) return;    // a non-empty file exists: keep the user's copy
    unsigned n = 0; while (contents[n]) n++;
    fat_write_file(&g_fat_fs, path, contents, n);
}

void dos_windir_init(void) {
    ensure_dir(WINDIR_ROOT);
    ensure_dir(WINDIR_ROOT "/DRIVE_A");
    ensure_dir(WINDIR_ROOT "/DRIVE_C");
    ensure_dir(WINDIR_ROOT "/DRIVE_E");
    // Minimal C:\WINDOWS so GetWindowsDirectory/GetSystemDirectory probes succeed.
    ensure_dir(WINDIR_ROOT "/DRIVE_C/WINDOWS");
    ensure_dir(WINDIR_ROOT "/DRIVE_C/WINDOWS/SYSTEM");
    ensure_dir(WINDIR_ROOT "/DRIVE_C/WINDOWS/TEMP");
    // (#133) Seed minimal Win3.1 .INI files so GetProfile*/GetPrivateProfile* and
    // apps that probe [windows]/[intl]/[boot] find a real, editable file.
    seed_file(WINDIR_ROOT "/DRIVE_C/WINDOWS/WIN.INI",
        "[windows]\r\nspooler=yes\r\ndevice=\r\nrun=\r\nload=\r\n"
        "[Desktop]\r\nWallpaper=(None)\r\nPattern=(None)\r\n"
        "[intl]\r\nsCountry=United States\r\niCountry=1\r\nsLanguage=enu\r\n"
        "[fonts]\r\n[extensions]\r\n[mci extensions]\r\n[sounds]\r\n");
    seed_file(WINDIR_ROOT "/DRIVE_C/WINDOWS/SYSTEM.INI",
        "[boot]\r\nshell=progman.exe\r\nsystem.drv=system.drv\r\n"
        "[keyboard]\r\n[boot.description]\r\n[386Enh]\r\n[drivers]\r\n[mci]\r\n");
    kprintf("[dospath] /WINDIR drive layer ready (A/C/E; C:\\WINDOWS + WIN.INI/SYSTEM.INI seeded)\n");
}
