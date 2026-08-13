// sgprobe - tier-1 syscall permission-bypass probe (#700 series: B1/B3/B4/B5/B7/B8/B9).
//
// WHY IT EXISTS. Each of these bypasses is a syscall that performs a privileged
// filesystem or configuration action on behalf of whoever asks, with no
// authorization check. "The code has no check" is a reading; this app turns it
// into a measurement, from a genuine Ring-3 process at a genuine non-root uid,
// on the real artifact.
//
// IT DROPS TO uid 1000 FIRST. The shipped image autologins as root
// (build/assets/LOGIN.CFG: autologin=root), so an autorun-launched app inherits
// uid 0 and would prove nothing: root is allowed to do all of this. setuid(1000)
// is performed as the very first action and the result is asserted, because a
// failed drop would make every subsequent "bypass succeeded" line a lie.
//
// EVERYTHING GOES TO fd 2. Measured on golden 1004 and re-recorded in
// apps/canarytest: an autorun-spawned process's fd 1 does not reach the serial
// console; fd 2 does. Own formatting, straight to write(2, ...).
//
// NOTHING PRINTS FILE CONTENTS. Every assertion is on a RETURN CODE or on a
// kernel-side log line. A probe that dumps the bytes it managed to read would
// itself become the disclosure channel it is testing for.

#include "../../libc/maytera.h"

#define SYS_PKG_WRITE       301
#define SYS_CRON_ADD        276
#define SYS_PRINT_ADD       293
#define SYS_PRINT_IMAGE     296
#define SYS_THEME_LOADF     335
#define SYS_BOOTLOG_WR      298
#define SYS_SPAWN_NA        196

#define CRON_TARGET_MAX 64
#define CRON_LABEL_MAX  32
#define CRON_TYPE_ONESHOT  0
#define CRON_ACT_LAUNCH    1

typedef struct {
    unsigned int   id;
    unsigned char  type;
    unsigned char  action;
    unsigned char  enabled;
    unsigned char  weekday;
    unsigned char  hour;
    unsigned char  minute;
    unsigned char  reserved[2];
    unsigned int   interval_ms;
    unsigned int   run_count;
    unsigned long long next_fire_tick;
    char           target[CRON_TARGET_MAX];
    char           label[CRON_LABEL_MAX];
} cron_job_t;

// ---------------------------------------------------------------------------
// fd-2 output.
//
// ONE write(2, ...) PER LINE, always. Measured on the first run of this probe:
// the kernel serial logger treats every write() as its own timestamped record,
// so a line assembled from four writes came out as four fragments, three of
// them stamped and reordered against a concurrently logging kernel. The values
// were unreadable. Compose into a buffer, then emit once.
// ---------------------------------------------------------------------------
static char g_line[512];
static int  g_len;

static void put_s(const char *s) {
    while (*s && g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = *s++;
}
static void put_n(long v) {
    char b[24]; int i = 23; int neg = 0;
    b[i--] = 0;
    if (v < 0) { neg = 1; v = -v; }
    if (v == 0) b[i--] = '0';
    while (v > 0) { b[i--] = (char)('0' + (v % 10)); v /= 10; }
    if (neg) b[i--] = '-';
    put_s(&b[i + 1]);
}
static void flush_line(void) {
    if (g_len < (int)sizeof(g_line) - 1) g_line[g_len++] = '\n';
    write(2, g_line, (unsigned long)g_len);
    g_len = 0;
}

// One result line, in a shape a grep can key on without ambiguity.
static void res(const char *probe, const char *what, long rc) {
    g_len = 0;
    put_s("[SGP] "); put_s(probe); put_s(" rc="); put_n(rc); put_s("  "); put_s(what);
    flush_line();
}
static void note(const char *what, long v) {
    g_len = 0;
    put_s("[SGP] "); put_s(what); put_s(" = "); put_n(v);
    flush_line();
}
static void msg(const char *what) {
    g_len = 0; put_s("[SGP] "); put_s(what); flush_line();
}

static void sg_memset(void *d, int c, unsigned long n) {
    unsigned char *p = (unsigned char *)d; while (n--) *p++ = (unsigned char)c;
}
static unsigned long sg_strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void sg_strcpy(char *d, const char *s) { while ((*d++ = *s++)) ; }

// ---------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    msg("===== tier-1 syscall permission-bypass probe =====");

    note("uid at entry", syscall0(SYS_GETUID));
    note("setgid(1000) rc", syscall1(SYS_SETGID, 1000));
    note("setuid(1000) rc", syscall1(SYS_SETUID, 1000));
    long uid1 = syscall0(SYS_GETUID);
    note("uid now", uid1);
    if (uid1 != 1000) {
        msg("FATAL: not uid 1000; every result below would be meaningless. Aborting.");
        return 1;
    }
    msg("CONFIRMED uid=1000. Everything below is an UNPRIVILEGED request.");

    // -----------------------------------------------------------------------
    // B1: SYS_PKG_WRITE - arbitrary path, arbitrary content.
    // Targets chosen so a SUCCESS is unambiguous privilege escalation but the
    // test image still boots a second time (the cron half of this run needs
    // boot 2). /boot/kernel.elf is DELIBERATELY NOT written: it is the exact
    // path that proves the point, and bricking the artifact mid-experiment
    // would destroy the evidence for everything else. /BOOT/KERNEL.ELF.BAK is
    // the same root-owned 0600 boot-path file class (perms_system_seed[]),
    // written by selfupdate.c only, and nothing boots from it.
    // -----------------------------------------------------------------------
    {
        static const char payload[] = "SGPROBE-B1-PWNED-BY-UID-1000\n";
        unsigned len = (unsigned)(sizeof(payload) - 1);

        res("B1", "pkg_write /BOOT/KERNEL.ELF.BAK (root 0600 boot path)",
            syscall3(SYS_PKG_WRITE, (long)"/BOOT/KERNEL.ELF.BAK", (long)payload, len));
        res("B1", "pkg_write /CONFIG/AUTHKEYS (root 0600 ssh authorized keys)",
            syscall3(SYS_PKG_WRITE, (long)"/CONFIG/AUTHKEYS", (long)payload, len));
        res("B1", "pkg_write /BOOTLOG.TXT (root 0600 diagnostic)",
            syscall3(SYS_PKG_WRITE, (long)"/BOOTLOG.TXT", (long)payload, len));

        // An EXISTING root-owned file on the FAT ESP, non-load-bearing (the boot
        // splash). This is the primitive that reaches /boot/kernel.elf: same
        // partition, same writer, same absence of a check. It is used instead of
        // the kernel image itself because bricking the artifact would destroy
        // the evidence for every other probe in this run.
        res("B1", "pkg_write /BOOT.BMP (existing root-owned file on the ESP)",
            syscall3(SYS_PKG_WRITE, (long)"/BOOT.BMP", (long)payload, len));

        // The boot path proper. Also a boundary test for the fix: /BOOT.BMP must
        // NOT be caught by a "/BOOT" prefix rule, and /boot/... must be.
        res("B1", "pkg_write /boot/kernel.elf.sgp (boot path, must be HARD denied)",
            syscall3(SYS_PKG_WRITE, (long)"/boot/kernel.elf.sgp", (long)payload, len));

        // #689: install an ELF into a directory this uid owns, and do NOT touch
        // it afterwards, so /CONFIG/PERMS.DB shows the mode the KERNEL chose
        // rather than one this probe set. Expected after the fix: 1000:1000:0555
        // (owned by the installer, not writable by the installer). Before the
        // fix the expected result is no PERMS.DB entry at all, which is what
        // made the staged copy below un-chmod-able by its own creator.
        {
            static const unsigned char elfmagic[16] = {0x7F,'E','L','F',2,1,1,0,0,0,0,0,0,0,0,0};
            res("B1", "#689 pkg_write ELF /HOME/ADMIN/SGPELF (expect mode 0555 after)",
                syscall3(SYS_PKG_WRITE, (long)"/HOME/ADMIN/SGPELF", (long)elfmagic, 16));
        }

        // CONTROL: a path this uid legitimately owns (/HOME/ADMIN is 1000:1000
        // 0750). This MUST keep working after the fix. Without it, "everything
        // is denied now" is indistinguishable from "the syscall is broken".
        res("B1", "CONTROL pkg_write /HOME/ADMIN/SGPOK.TXT (own dir, must SUCCEED)",
            syscall3(SYS_PKG_WRITE, (long)"/HOME/ADMIN/SGPOK.TXT", (long)payload, len));
    }

    // -----------------------------------------------------------------------
    // B4: SYS_CRON_ADD newline injection -> a forged root job.
    // The kernel stamps OUR uid as the owner (that is #692 working). The attack
    // does not lie about the caller: it writes bytes that a LATER, legitimate
    // read of /CONFIG/CRON.CFG turns into a second job line whose 5th field
    // parses as uid=0.
    // -----------------------------------------------------------------------
    {
        cron_job_t j;

        // (a) the forged one.
        sg_memset(&j, 0, sizeof(j));
        j.type = CRON_TYPE_ONESHOT;
        j.action = CRON_ACT_LAUNCH;
        j.enabled = 1;
        j.interval_ms = 3600000;
        sg_strcpy(j.target, "X\nINTERVAL 10 launch /APPS/WHOAMI uid=0");
        sg_strcpy(j.label, "sgp-inject");
        note("B4 injected target length (limit 63)", (long)sg_strlen(j.target));
        res("B4", "cron_add with newline-injected target (job id)",
            syscall1(SYS_CRON_ADD, (long)&j));

        // (b) a legitimate control job, so the persisted file shows both an
        //     honest uid=1000 line and (if unfixed) the forged uid=0 line.
        sg_memset(&j, 0, sizeof(j));
        j.type = CRON_TYPE_ONESHOT;
        j.action = CRON_ACT_LAUNCH;
        j.enabled = 1;
        j.interval_ms = 3600000;
        sg_strcpy(j.target, "/APPS/WHOAMI");
        sg_strcpy(j.label, "sgp-legit");
        res("B4", "CONTROL cron_add plain target (must SUCCEED, owner 1000)",
            syscall1(SYS_CRON_ADD, (long)&j));
    }

    // -----------------------------------------------------------------------
    // B5: SYS_PRINT_ADD - unprivileged write of root-owned /CONFIG/PRINTERS.CFG.
    // Host is 127.0.0.1 on purpose: this must not put a packet on the LAN.
    // -----------------------------------------------------------------------
    res("B5", "print_add SGPPWN -> 127.0.0.1:631 (writes root /CONFIG/PRINTERS.CFG)",
        syscall5(SYS_PRINT_ADD, (long)"SGPPWN", (long)"127.0.0.1", 631, (long)"q", 1));

    // -----------------------------------------------------------------------
    // B3: SYS_PRINT_IMAGE - arbitrary read of any path, plus a network channel.
    // The read happens BEFORE any socket work (ipp_print_image -> fat_read_file),
    // so the kernel's own "[PRINT] direct image/jpeg: <path> (<n> bytes)" /
    // "[PRINT] cannot read" line is the evidence, independent of whether the
    // (deliberately dead) 127.0.0.1 connection ever completes.
    // The .jpg suffix selects the raw-bytes path, which is the exfil-shaped one.
    // -----------------------------------------------------------------------
    res("B3", "print_image /CONFIG/SHADOW.JPG-shaped read of /CONFIG/KIMI.KEY",
        syscall2(SYS_PRINT_IMAGE, (long)"SGPPWN", (long)"/CONFIG/KIMI.KEY"));
    res("B3", "print_image /BOOT/KERNEL.ELF",
        syscall2(SYS_PRINT_IMAGE, (long)"SGPPWN", (long)"/BOOT/KERNEL.ELF"));

    // -----------------------------------------------------------------------
    // B7: SYS_THEME_LOAD_FILE - arbitrary read. Evidence is the [PERMS-DENY]
    // line after the fix; before the fix the kernel parses a 0600 root file.
    // -----------------------------------------------------------------------
    res("B7", "theme_load_file /CONFIG/AUTHKEYS (root 0600)",
        syscall1(SYS_THEME_LOADF, (long)"/CONFIG/AUTHKEYS"));

    // -----------------------------------------------------------------------
    // B9: SYS_BOOTLOG_WRITE - chosen content into root-owned /BOOTLOG.TXT.
    // The embedded newline is the point: it forges an ADDITIONAL log line that
    // does not look like it came from a Ring-3 process at all.
    // -----------------------------------------------------------------------
    res("B9", "bootlog_write with embedded newline (log-line forgery)",
        syscall1(SYS_BOOTLOG_WR,
                 (long)"sgp-benign\n[KERNEL] FORGED-BY-UID-1000 all checks passed"));

    // -----------------------------------------------------------------------
    // B8: execute permission. Copy an ELF into a directory this uid owns, chmod
    // it 0644 (no x for anyone), then try to spawn it. If X_OK means anything,
    // the spawn must fail. Before the fix it succeeds, because X_OK is checked
    // nowhere in the kernel.
    // -----------------------------------------------------------------------
    {
        // Read the WHOLE file. The first version of this probe used a single
        // read() into a 64KB buffer, got exactly 65536 back, and staged a
        // TRUNCATED ELF: the spawn then failed elf_validate() and the -1 looked
        // like an X_OK denial that did not exist. A test that fails for the
        // wrong reason is worse than no test, because it reads as a pass.
        static char buf[192 * 1024];   // /APPS/WHOAMI is 71024 bytes on golden 1025
        int fd = open("/APPS/WHOAMI", 0);
        long n = 0;
        if (fd >= 0) {
            for (;;) {
                long got = read(fd, buf + n, (long)sizeof(buf) - n);
                if (got <= 0) break;
                n += got;
                if (n >= (long)sizeof(buf)) break;
            }
            close(fd);
        }
        note("B8 read /APPS/WHOAMI bytes (whole file)", n);
        if (n > 0) {
            long w = syscall3(SYS_PKG_WRITE, (long)"/HOME/ADMIN/SGPNOX", (long)buf, (long)n);
            res("B8", "stage copy at /HOME/ADMIN/SGPNOX", w);
            if (w >= 0) {
                res("B8", "chmod 0644 (no x for anybody)",
                    syscall2(SYS_CHMOD, (long)"/HOME/ADMIN/SGPNOX", 0644));
                res("B8", "spawn a file with mode 0644 (must FAIL if X_OK exists)",
                    syscall1(SYS_SPAWN_NA, (long)"/HOME/ADMIN/SGPNOX"));
                // POSITIVE CONTROL. An x-bit-bearing binary must still spawn,
                // or "denied" below would just mean spawn is broken.
                res("B8", "CONTROL spawn /APPS/WHOAMI (x bit set, must SUCCEED)",
                    syscall1(SYS_SPAWN_NA, (long)"/APPS/WHOAMI"));
            }
        }
    }

    msg("===== probe complete =====");
    return 0;
}
