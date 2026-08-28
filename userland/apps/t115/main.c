// t115 - #115 (local 120) ON-VM PROOF that stat() reports real metadata.
//
// WHAT THIS MEASURES, AND WHY IT MEASURES IT THIS WAY.
//
// The failure mode #115 is about is a value that LOOKS measured and is not.
// A test that only checks ORDER cannot see it: if every mtime came from a
// broken clock (say seconds-since-boot, #113), the order would be perfect and
// every absolute date would be nonsense. So this harness checks BOTH
// DIRECTIONS, against a clock it reads for itself:
//
//   1. Before each write it reads the RTC (SYS_GET_RTC_DATE/TIME) and converts
//      to epoch seconds IN THIS PROGRAM, with its own arithmetic. That is the
//      independent observation. It is not the kernel's converter and does not
//      call it.
//   2. It writes three files with a real gap between them.
//   3. It stats them and checks (a) mtimes strictly increase in write order,
//      and (b) each mtime is within a tolerance of the RTC instant this program
//      observed for that write. (b) is the check that a boot-relative clock
//      fails and a real one passes.
//
// AND IT DOES ALL OF IT ON BOTH FILESYSTEMS. The ext2 root (/T115E) and the
// FAT ESP (/boot/T115F, because fat_path_on_ext2() never redirects /boot).
// The previous agent's stat fixtures all lived on ext2 and it said so; this
// one covers both rather than repeating that gap.
//
// OUTPUT DISCIPLINE. Launched from /CONFIG/AUTORUN.CFG, and an autorun-launched
// process emits ONE SERIAL RECORD PER write(), so every line is formatted into
// a buffer and issued as exactly one write(2, ...). Same rule as toolchk.
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "fcntl.h"
#include "syscall.h"
#include "sys/stat.h"
#include "utime.h"
#include "errno.h"
#include "spawn.h"
#include "sys/wait.h"

static int g_pass = 0, g_fail = 0;

static void line(const char *s) { write(2, s, strlen(s)); }

static void check(int ok, const char *what)
{
	char buf[320];
	if (ok) g_pass++; else g_fail++;
	snprintf(buf, sizeof buf, "%s %s\n", ok ? "[T115] PASS" : "[T115] FAIL", what);
	line(buf);
}

// Independent civil -> epoch. Hinnant's algorithm, written here so this program
// does NOT depend on the kernel routine it is testing. A test that calls the
// code under test to compute its own expectation proves nothing (blame.md).
static long days_from_civil(long y, long m, long d)
{
	y -= (m <= 2);
	long era = (y >= 0 ? y : y - 399) / 400;
	long yoe = y - era * 400;
	long mp  = (m + 9) % 12;
	long doy = (153 * mp + 2) / 5 + d - 1;
	long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

// Read the RTC through the syscalls the CLOCK app and Settings already use.
static long rtc_epoch(void)
{
	long t = syscall0(SYS_GET_RTC_TIME);
	long d = syscall0(SYS_GET_RTC_DATE);
	long hour = (t >> 16) & 0xFF, min = (t >> 8) & 0xFF, sec = t & 0xFF;
	long year = (d >> 16) & 0xFFFF, mon = (d >> 8) & 0xFF, day = d & 0xFF;
	if (year < 1980 || year > 2107 || mon < 1 || mon > 12 || day < 1 || day > 31)
		return -1;
	return days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
}

static int write_file(const char *path, const char *text)
{
	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
	if (fd < 0) return -1;
	int n = (int)write(fd, text, strlen(text));
	close(fd);
	return (n == (int)strlen(text)) ? 0 : -1;
}

// Spawn the SHIPPED /APPS/LS.ELF with its stdout captured, echo what it
// printed, and check the order. Spawning the real binary is deliberate: an
// in-process reimplementation of the sort would test this file, not the tool
// the user runs.
static void run_ls(const char *tag, const char *dir, const char *flag,
                   const char *want_order)
{
	char buf[768];
	const char *out = "/T115LS.TXT";
	posix_spawn_file_actions_t fa;
	pid_t pid = 0;
	char *argv[4];
	argv[0] = (char *)"ls"; argv[1] = (char *)flag; argv[2] = (char *)dir; argv[3] = 0;

	unlink(out);
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, 1, out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	int rc = posix_spawn(&pid, "/APPS/LS.ELF", &fa, NULL, argv, NULL);
	posix_spawn_file_actions_destroy(&fa);
	if (rc != 0) {
		snprintf(buf, sizeof buf, "[T115] %s ls %s: spawn failed rc=%d\n", tag, flag, rc);
		line(buf); g_fail++; return;
	}
	int st = 0;
	waitpid(pid, &st, 0);

	char body[1024];
	int fd = open(out, O_RDONLY);
	int n = (fd >= 0) ? (int)read(fd, body, sizeof body - 1) : -1;
	if (fd >= 0) close(fd);
	if (n < 0) n = 0;
	body[n] = 0;

	// Reduce the listing to the sequence of trailing letters (T115A -> A), so
	// the expectation is an ORDER and not a formatting match.
	char seq[64]; int k = 0;
	for (int i = 0; i + 8 < n && k < 60; i++) {
		if (body[i]=='T' && body[i+1]=='1' && body[i+2]=='1' && body[i+3]=='5' &&
		    (body[i+4]=='A' || body[i+4]=='B' || body[i+4]=='C')) {
			if (k) seq[k++] = ' ';
			seq[k++] = body[i+4];
		}
	}
	seq[k] = 0;
	snprintf(buf, sizeof buf, "[T115] %s ls %s %s -> order [%s] (want [%s])\n",
	         tag, flag, dir, seq, want_order);
	line(buf);
	// Flatten the newlines so the whole listing lands in ONE serial record: an
	// autorun process emits one record per write(), so a multi-line buffer is
	// split across log lines and the evidence becomes hard to read.
	for (int i = 0; i < n; i++) if (body[i] == '\n') body[i] = '|';
	snprintf(buf, sizeof buf, "[T115] %s ls %s raw: [%s]\n", tag, flag, body);
	line(buf);
	check(strcmp(seq, want_order) == 0, "ls sorted by time, newest first");
}

// One filesystem's worth of the measurement.
static void run_fs(const char *tag, const char *dir, int is_fat)
{
	char pa[256], pb[256], pc[256], buf[512];
	long obs[3], got[3];
	const char *names[3] = { "A", "B", "C" };
	char *paths[3] = { pa, pb, pc };

	snprintf(buf, sizeof buf, "\n[T115] ===== %s (%s) =====\n", tag, dir);
	line(buf);

	mkdir(dir, 0755);
	snprintf(pa, sizeof pa, "%s/T115A.TXT", dir);
	snprintf(pb, sizeof pb, "%s/T115B.TXT", dir);
	snprintf(pc, sizeof pc, "%s/T115C.TXT", dir);

	for (int i = 0; i < 3; i++) {
		obs[i] = rtc_epoch();
		if (write_file(paths[i], "t115\n") != 0) {
			snprintf(buf, sizeof buf, "[T115] FAIL %s: cannot write %s (errno %d)\n",
			         tag, paths[i], errno);
			line(buf); g_fail++; return;
		}
		snprintf(buf, sizeof buf, "[T115] wrote %s at RTC epoch %ld\n",
		         paths[i], obs[i]);
		line(buf);
		if (i < 2) sys_sleep(4000);   // a gap wider than any plausible tolerance
	}

	for (int i = 0; i < 3; i++) {
		struct stat st;
		if (stat(paths[i], &st) != 0) {
			snprintf(buf, sizeof buf, "[T115] FAIL %s: cannot stat %s\n", tag, paths[i]);
			line(buf); g_fail++; return;
		}
		got[i] = (long)st.st_mtime;
		snprintf(buf, sizeof buf,
		         "[T115] %s %s: mtime=%ld atime=%ld ctime=%ld ino=%lu nlink=%u "
		         "uid=%u gid=%u dev=%lu size=%ld blksize=%ld blocks=%ld mode=%o\n",
		         tag, names[i], (long)st.st_mtime, (long)st.st_atime,
		         (long)st.st_ctime, st.st_ino, st.st_nlink, st.st_uid, st.st_gid,
		         st.st_dev, st.st_size, st.st_blksize, st.st_blocks, st.st_mode);
		line(buf);
	}

	// --- DIRECTION 1: absolute. The check a boot-relative clock cannot pass.
	// FAT stores the modify time in TWO-second units, so the floor of the
	// tolerance is 2s there; 6s covers that plus the write itself.
	long tol = is_fat ? 6 : 4;
	for (int i = 0; i < 3; i++) {
		long delta = got[i] - obs[i];
		if (delta < 0) delta = -delta;
		snprintf(buf, sizeof buf,
		         "[T115] %s %s absolute: observed %ld, reported %ld, delta %lds "
		         "(tolerance %lds)\n", tag, names[i], obs[i], got[i], delta, tol);
		line(buf);
		check(got[i] != 0 && delta <= tol,
		      "mtime matches the RTC instant of the write (absolute, not just ordered)");
	}

	// --- DIRECTION 2: order.
	check(got[0] < got[1] && got[1] < got[2],
	      "mtimes strictly increase in write order");

	// --- The other fields.
	struct stat sa, sd;
	stat(paths[0], &sa);
	stat(dir, &sd);
	check(sa.st_ino != 0, "st_ino is non-zero (K3)");
	check(sd.st_nlink >= 2, "a directory's st_nlink is >= 2, not the constant 1 (K2)");
	check(sa.st_dev != 0 && sd.st_dev != 0, "st_dev is filled (grep -r loop check needs it)");
	check(sa.st_blksize > 0, "st_blksize is the real allocation unit");

	// --- utime(): the setter, and a read-back that proves it landed.
	long target = obs[0] - 86400 * 7;   // one week before the first write
	struct utimbuf ub;
	ub.actime = target; ub.modtime = target;
	int urc = utime(paths[2], &ub);
	if (urc != 0) {
		snprintf(buf, sizeof buf, "[T115] %s utime -> errno %d\n", tag, errno);
		line(buf);
	}
	struct stat su;
	stat(paths[2], &su);
	long udelta = (long)su.st_mtime - target;
	if (udelta < 0) udelta = -udelta;
	snprintf(buf, sizeof buf,
	         "[T115] %s utime: asked %ld, read back %ld, delta %lds\n",
	         tag, target, (long)su.st_mtime, udelta);
	line(buf);
	check(urc == 0 && udelta <= (is_fat ? 2 : 0),
	      "utime() set an explicit mtime and stat() read it back");

	// --- ls -t, the consumer the ticket names. C is now the OLDEST (utime
	// pushed it a week back), so newest-first must be B, A, C - an order that
	// is NOT name order and NOT the creation order, so a sort that quietly
	// falls back to either cannot accidentally produce it.
	run_ls(tag, dir, "-t", "B A C");
	// -r reverses the ordering. Unknowns would still sort last, but there are
	// none here, so this must be the exact mirror.
	run_ls(tag, dir, "-tr", "C A B");
}

int main(void)
{
	char buf[256];
	line("\n[T115] ===== #115 stat() metadata proof =====\n");
	long e = rtc_epoch();
	snprintf(buf, sizeof buf, "[T115] RTC now = epoch %ld\n", e);
	line(buf);
	snprintf(buf, sizeof buf, "[T115] time() (SYS_TIME, #113: seconds since BOOT) = %ld\n",
	         (long)syscall0(SYS_TIME));
	line(buf);

	run_fs("EXT2", "/T115E", 0);
	run_fs("FAT",  "/boot/T115F", 1);

	snprintf(buf, sizeof buf, "\n[T115] TOTAL pass=%d fail=%d\n", g_pass, g_fail);
	line(buf);
	line("[T115] DONE\n");
	return g_fail ? 1 : 0;
}
