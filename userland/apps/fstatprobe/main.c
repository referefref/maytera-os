// fstatprobe - #120 both-directions measurement for fstat().
//
// WHY THIS EXISTS AND WHY IT RUNS BOTH IMPLEMENTATIONS AT ONCE.
//
// The claim under test is "fstat() used to fabricate S_IFREG|0644 and now
// reports the truth". Proving that with two separate runs of two different
// binaries compares two things that differ in more than the fix. So this probe
// carries a VERBATIM COPY of the deleted implementation (old_fstat below,
// character-for-character what userland/libc/sys/stat.c contained at commit
// c9ae854, the commit golden build 1901 was built from) and calls BOTH against
// the SAME descriptor in the SAME process on the SAME kernel. The difference in
// the two output lines is then attributable to nothing but the implementation.
//
// The SAME binary is also run against the UNMODIFIED build-1901 kernel, where
// SYS_FSTAT does not exist. That run is the second control: it shows the
// syscall genuinely returning -1 from the dispatcher default, which is what
// makes "there was no fstat syscall" a measurement rather than a claim.
//
// Output goes to the persistent /BOOTLOG.TXT via SYS_BOOTLOG_WRITE, because
// serial is silent in GUI mode and driving the GUI is unreliable (#334).

#include "../../libc/syscall.h"
#include "../../libc/sys/stat.h"
#include "../../libc/string.h"
#include "../../libc/stdio.h"
#include "../../libc/unistd.h"
#include "../../libc/ftw.h"
#include "../../libc/sys/socket.h"

// ---- the DELETED implementation, pinned. Do not "improve" this. ------------
static int old_fstat(int fd, struct stat *st) {
    memset(st, 0, sizeof(*st));
    long cur = syscall3(SYS_SEEK, fd, 0, 1);  // SEEK_CUR
    long end = syscall3(SYS_SEEK, fd, 0, 2);  // SEEK_END
    if (cur >= 0 && end >= 0) {
        syscall3(SYS_SEEK, fd, cur, 0);       // restore
        st->st_size = end;
    }
    st->st_mode  = S_IFREG | 0644;
    st->st_nlink = 1;
    return 0;
}

static void say(const char *s) { syscall1(SYS_BOOTLOG_WRITE, (long)s); }

static const char *typestr(unsigned m) {
    switch (m & S_IFMT) {
        case S_IFREG:  return "REG";
        case S_IFDIR:  return "DIR";
        case S_IFCHR:  return "CHR";
        case S_IFIFO:  return "FIFO";
        case S_IFSOCK: return "SOCK";
        case 0:        return "UNKNOWN";
        default:       return "other";
    }
}

static int g_ftw_seen = 0;
static int g_ftw_efi_children = 0;
static int g_ftw_efi_seen = 0;

static int ftw_cb(const char *path, const struct stat *st, int type, struct FTW *f) {
    (void)st; (void)type; (void)f;
    g_ftw_seen++;
    // Anything BELOW /EFI is on the other filesystem and must not be visited
    // when FTW_MOUNT is set.
    if (path[0]=='/' && path[1]=='E' && path[2]=='F' && path[3]=='I') {
        if (path[4]=='/')      g_ftw_efi_children++;   // BELOW the boundary
        else if (path[4]=='\0') g_ftw_efi_seen++;      // the boundary itself
    }
    if (g_ftw_seen > 200000) return 1;   // bound the walk; this is a probe
    return 0;
}

static char buf[512];

// EVERY line carries a sequence number. The first version of this probe emitted
// one line per target and the FIRST one never appeared in /BOOTLOG.TXT, with no
// error anywhere - which is indistinguishable from "the probe skipped that
// case". A gap in a counted sequence is visible; a missing line is not. Lines
// are also kept well under BOOTLOG_LINE_MAX (256, fs/bootlog.c), which is why
// OLD and NEW are reported separately instead of on one long line.
static int seq = 0;

static void probe(const char *label, int fd) {
    struct stat o, n;
    int orc = old_fstat(fd, &o);
    long raw = syscall2(SYS_FSTAT, fd, (long)&n);   // the syscall, unwrapped
    int nrc = fstat(fd, &n);                        // and through libc

    snprintf(buf, sizeof(buf),
             "[F120] %03d %s fd=%d OLD rc=%d type=%s mode=%o size=%ld nlink=%u",
             ++seq, label, fd, orc, typestr(o.st_mode), o.st_mode,
             (long)o.st_size, o.st_nlink);
    say(buf);
    snprintf(buf, sizeof(buf),
             "[F120] %03d %s raw=%ld rc=%d type=%s mode=%o size=%ld ino=%lu dev=%lu",
             ++seq, label, raw, nrc, typestr(n.st_mode), n.st_mode,
             (long)n.st_size, (unsigned long)n.st_ino, (unsigned long)n.st_dev);
    say(buf);
    snprintf(buf, sizeof(buf),
             "[F120] %03d %s nlink=%u uid=%u gid=%u rdev=%lu blksz=%ld blocks=%ld mtime=%lu",
             ++seq, label, n.st_nlink, n.st_uid, n.st_gid,
             (unsigned long)n.st_rdev, (long)n.st_blksize, (long)n.st_blocks,
             (unsigned long)n.st_mtime);
    say(buf);
}

// ===========================================================================
// #120: stat() AND fstat() MUST AGREE ON THE SAME FILE.
//
// This is a separate assertion from "fstat reports the truth", and it is the
// one worth singling out, because stat-vs-fstat disagreement is exactly the
// class of bug #58 fixed for open-vs-stat: two syscalls that answer the same
// question about the same file, and answer it differently. The whole reason
// sc_stat_fill() exists is that ONE function answers for both, so if these ever
// disagree on the metadata fields, the reuse has been broken and a second
// implementation has grown back. This probe is the thing that would notice.
//
// WHICH FIELDS MUST AGREE, AND WHICH NEED NOT.
//   MUST: st_dev, st_ino, st_mode, st_nlink, st_uid, st_gid, st_rdev, st_mtime.
//         These describe the FILE and cannot depend on how you named it.
//   NEED NOT: st_size, on a descriptor carrying buffered unflushed writes. That
//         is the deliberate live-size behaviour, where fstat reports the
//         DESCRIPTION and is more current than the directory entry. So size is
//         compared only on a freshly opened, unwritten fd, which is what this
//         helper uses, and a difference THERE is a real defect.
//
// Reported as an explicit AGREE/DISAGREE verdict plus a per-field bitmask, not
// as two lines a reader has to diff by eye. A verdict nobody computes is a
// verdict nobody checks.
// ===========================================================================
static void probe_agree(const char *label, const char *path) {
    struct stat sp, sf;
    int src = stat(path, &sp);
    int fd  = (int)syscall2(SYS_OPEN, (long)path, 0);
    if (src != 0 || fd < 0) {
        snprintf(buf, sizeof(buf), "[F120A] %03d %s SKIP stat_rc=%d open_rc=%d path=%s",
                 ++seq, label, src, fd, path);
        say(buf);
        if (fd >= 0) syscall1(SYS_CLOSE, fd);
        return;
    }
    int frc = fstat(fd, &sf);
    syscall1(SYS_CLOSE, fd);

    unsigned diff = 0;
    if (sp.st_dev   != sf.st_dev)   diff |= 1u << 0;
    if (sp.st_ino   != sf.st_ino)   diff |= 1u << 1;
    if (sp.st_mode  != sf.st_mode)  diff |= 1u << 2;
    if (sp.st_nlink != sf.st_nlink) diff |= 1u << 3;
    if (sp.st_uid   != sf.st_uid)   diff |= 1u << 4;
    if (sp.st_gid   != sf.st_gid)   diff |= 1u << 5;
    if (sp.st_rdev  != sf.st_rdev)  diff |= 1u << 6;
    if (sp.st_mtime != sf.st_mtime) diff |= 1u << 7;
    if (sp.st_size  != sf.st_size)  diff |= 1u << 8;   // unwritten fd: must match

    snprintf(buf, sizeof(buf),
             "[F120A] %03d %s %s frc=%d diffmask=0x%x stat(mode=%o ino=%lu dev=%lu size=%ld)",
             ++seq, label, diff ? "DISAGREE" : "AGREE", frc, diff,
             sp.st_mode, (unsigned long)sp.st_ino, (unsigned long)sp.st_dev, (long)sp.st_size);
    say(buf);
    snprintf(buf, sizeof(buf),
             "[F120A] %03d %s     fstat(mode=%o ino=%lu dev=%lu size=%ld) mtime s=%lu f=%lu",
             ++seq, label, sf.st_mode, (unsigned long)sf.st_ino,
             (unsigned long)sf.st_dev, (long)sf.st_size,
             (unsigned long)sp.st_mtime, (unsigned long)sf.st_mtime);
    say(buf);
}

static void probe_path(const char *label, const char *path) {
    int fd = (int)syscall2(SYS_OPEN, (long)path, 0);
    if (fd < 0) {
        snprintf(buf, sizeof(buf), "[F120] %03d %s OPEN-FAILED path=%s rc=%d", ++seq, label, path, fd);
        say(buf);
        return;
    }
    probe(label, fd);
    syscall1(SYS_CLOSE, fd);
}

int main(void) {
    say("[F120] ==== fstatprobe start (build under test) ====");

    // ext2 root volume (the shipping userland lives here).
    probe_path("ext2-FILE", "/APPS/CAT");
    probe_path("ext2-DIR ", "/APPS");
    probe_path("ext2-DIR2", "/");

    // FAT ESP. A path is only reached on FAT if the ext2 branch MISSES it, so
    // the target must exist on p1 and NOT on p2. Checked by mounting both, not
    // assumed: the first version of this probe used /BUILDINFO.TXT, which
    // exists on BOTH (299 bytes on the ESP, 81 on the ext2 root), so the "FAT"
    // case was silently answered by ext2 and reported st_dev=2. The st_dev
    // field is what made that visible, which is itself a #120 result: before
    // this change st_dev was always 0 and the mistake would have been invisible.
    probe_path("fat-FILE ", "/FONT.TTF");            // ESP only, 759720 bytes
    probe_path("fat-FIL2 ", "/EFI/BOOT/BOOTX64.EFI");// ESP only, nested
    probe_path("fat-DIR  ", "/EFI");
    probe_path("fat-DIR2 ", "/EFI/BOOT");

    // A device. The old implementation could not represent this at all.
    probe_path("device   ", "/dev/null");

    // A pipe: two fds, neither of which has a path to stat.
    {
        int pfd[2];
        long rc = syscall1(SYS_PIPE, (long)pfd);
        if (rc == 0) { probe("pipe-RD  ", pfd[0]); probe("pipe-WR  ", pfd[1]);
                       syscall1(SYS_CLOSE, pfd[0]); syscall1(SYS_CLOSE, pfd[1]); }
        else { snprintf(buf, sizeof(buf), "[F120] pipe() failed rc=%ld", rc); say(buf); }
    }

    // THE STDIO DESCRIPTORS. These are the fds real code fstat()s most often
    // (an isatty-style "am I on a terminal" check is exactly an fstat for
    // S_IFCHR), and they are the three every process is born holding. They are
    // /dev/console or a /dev/pts/N slave, so the truthful answer is CHR, and
    // the fabrication called all three REG. Not previously probed.
    probe("stdin    ", 0);
    probe("stdout   ", 1);
    probe("stderr   ", 2);

    // A SOCKET. Sockets were the one fd family that recorded no path at all,
    // so before this change the kernel could not name one either. No connect()
    // is needed: an unconnected socket is still a socket, and S_IFSOCK is a
    // property of the description, not of its connection state.
    {
        int sfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sfd >= 0) { probe("socket   ", sfd); syscall1(SYS_CLOSE, sfd); }
        else { snprintf(buf, sizeof(buf), "[F120] %03d socket() failed rc=%d", ++seq, sfd); say(buf); }
    }

    // A descriptor that is not open. The old implementation returned SUCCESS
    // here, with size 0 and mode S_IFREG|0644.
    probe("badfd    ", 99);
    probe("negfd    ", -1);

    // THE LIVE-SIZE CASE. Write to a new file and fstat it BEFORE closing: the
    // directory entry still says 0, so a path-based stat would report 0 and be
    // a regression against the seek trick. Run on both filesystems.
    {
        int fd = (int)syscall2(SYS_OPEN, (long)"/TMP120.TXT", 0x40 | 0x1 | 0x200); // O_CREAT|O_WRONLY|O_TRUNC
        if (fd >= 0) {
            const char *msg = "0123456789ABCDEF";
            syscall3(SYS_WRITE, fd, (long)msg, 16);
            probe("livesize ", fd);
            syscall1(SYS_CLOSE, fd);
        } else { snprintf(buf, sizeof(buf), "[F120] livesize open failed rc=%d", fd); say(buf); }
    }

    probe_path("ext2-FILE2", "/APPS/CAT");   // repeat: a first-line loss must be visible

    // #120: FTW_MOUNT, which used to be refused with EINVAL because st_dev was
    // always 0. A walk of "/" crosses a real boundary on this image: /EFI is on
    // the FAT ESP (st_dev 1) and everything else is on the ext2 root (st_dev 2).
    // Without the flag the walk enters /EFI; with it, /EFI is REPORTED and not
    // descended into. The two counts differing is the whole proof.
    {
        int rc_no, rc_yes;
        g_ftw_seen = 0; g_ftw_efi_children = 0; g_ftw_efi_seen = 0;
        rc_no = nftw("/", ftw_cb, 8, 0);
        snprintf(buf, sizeof(buf), "[F120] %03d ftw NOFLAG rc=%d seen=%d efi_seen=%d efi_children=%d",
                 ++seq, rc_no, g_ftw_seen, g_ftw_efi_seen, g_ftw_efi_children);
        say(buf);
        g_ftw_seen = 0; g_ftw_efi_children = 0; g_ftw_efi_seen = 0;
        rc_yes = nftw("/", ftw_cb, 8, FTW_MOUNT);
        snprintf(buf, sizeof(buf), "[F120] %03d ftw MOUNT  rc=%d seen=%d efi_seen=%d efi_children=%d",
                 ++seq, rc_yes, g_ftw_seen, g_ftw_efi_seen, g_ftw_efi_children);
        say(buf);
    }
    // The agreement assertion, on every target that HAS a path to compare with.
    probe_agree("agr-ext2-FILE", "/APPS/CAT");
    probe_agree("agr-ext2-DIR ", "/APPS");
    probe_agree("agr-ext2-ROOT", "/");
    probe_agree("agr-fat-FILE ", "/FONT.TTF");
    probe_agree("agr-fat-FIL2 ", "/EFI/BOOT/BOOTX64.EFI");
    probe_agree("agr-fat-DIR  ", "/EFI");
    probe_agree("agr-fat-DIR2 ", "/EFI/BOOT");
    probe_agree("agr-device   ", "/dev/null");

    say("[F120] ==== fstatprobe end ====");
    return 0;
}
