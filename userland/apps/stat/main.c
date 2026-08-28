// stat - report what this kernel actually knows about a file
// Usage: stat [FILE...]
//
// #745 (local 108, THIRD batch).
//
// WHAT WAS WRONG. The whole program was open(path, O_RDONLY) followed by
// lseek(SEEK_END), and it printed two lines: the name it was given and that
// size. Consequences:
//
//  * IT COULD NOT STAT A DIRECTORY AT ALL. open() of a directory does not give
//    you a readable file here, so `stat /APPS` failed - the single most common
//    thing anyone runs stat on after a plain file.
//  * NO MODE, NO TYPE, NO OWNER, NO LINK COUNT. It reported one number and
//    called itself stat.
//  * SINGLE OPERAND: `stat a b` reported a and exited 0.
//  * SEEK_END on a FAT file walks the cluster chain. That is exactly the cost
//    SYS_STAT was added to avoid (see sys_stat_path's own comment: sizing files
//    with SEEK_END made `ls -la /` over the multi-MB kernel.elf effectively hang
//    the machine).
//
// WHAT THIS KERNEL CAN HONESTLY ANSWER, measured in kernel/proc/syscall.c
// (sys_stat_path) and kernel/rustkern/fsperm.rs (rk_fs_perm_info) rather than
// assumed. Two syscalls are consulted, because they answer different halves:
//
//   SYS_STAT (k_stat_t)      REAL: st_mode's TYPE bits, st_size, st_blksize,
//                                  st_blocks. On the ext2 root st_mode also
//                                  carries the real on-disk permission bits.
//                            NOT REAL: st_nlink is the literal constant 1 on
//                                  every branch; st_ino, st_dev, st_uid, st_gid
//                                  and all three timestamps are left at 0 by the
//                                  memset and never filled. On the FAT ESP the
//                                  permission bits are SYNTHESISED (0755/0644) -
//                                  FAT has no POSIX mode, so what it prints
//                                  there would be a decoration, not a fact.
//
//   SYS_FS_PERM_INFO         The filesystem-aware answer, and the one the kernel
//                            itself enforces: for an ext2/POSIX path the uid,
//                            gid and mode from fs/perms.c (the SAME values
//                            sys_open()'s perms_check() applies), and for a
//                            genuine FAT path the RAW on-disk attribute byte and
//                            deliberately no mode at all.
//
// THE RULE THIS FILE FOLLOWS: A FIELD THIS OS DOES NOT KNOW IS PRINTED AS
// "unavailable" WITH THE REASON, NEVER AS A PLAUSIBLE ZERO. `Links: 1` and
// `Modify: 1970-01-01 00:00:00` would both have been formatted from a memset,
// and a reader has no way to tell a measured 1 from a placeholder 1. That is the
// same defect as the old env printing a fabricated environment (local 108, first
// batch): every one of those lines was a guess formatted to look like a
// measurement.
//
// THE FORMAT IS OURS, NOT GNU's, ON PURPOSE. GNU stat's default block prints
// Device, Inode, Links, Access/Modify/Change on lines this OS could only fill
// with zeroes. Emitting GNU's shape and putting placeholders in it would produce
// output that DIFFS clean against a real machine while being false. So the
// layout says what is known, and says what is not.
//
// KERNEL FINDINGS RAISED BY THIS WORK, NOT FIXED HERE (another agent holds the
// kernel this session; see the CHANGELOG entry):
// #115 (local 120) UPDATE: K1, K2 and K3 BELOW ARE FIXED. sys_stat_path() now
// fills every field of k_stat_t on all four backends, and this program prints
// them. The findings are kept, not deleted, because they name the shape of the
// defect and this file is where a reader looks for it. Read them as history.
// One correction to K2 as written: NFS was ALREADY returning a real nlink
// (attrs.nlink), so "a literal 1" was true of three branches, not four.
//
//   K1  sys_stat_path zero-fills st_atime/st_mtime/st_ctime on every branch.
//       ext2 inodes carry i_atime/i_mtime/i_ctime and FAT directory entries
//       carry a write date/time, so on both filesystems the data is one struct
//       field away. This is also why `ls -t` cannot sort and why utime() is
//       written to fail with ENOSYS.
//   K2  sys_stat_path sets st_nlink = 1 unconditionally, including on the ext2
//       branch where ext2_inode_t.i_links_count has just been read.
//   K3  sys_stat_path leaves st_ino = 0 on the ext2 branch, where
//       ext2_resolve_path() has just returned the inode number.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "errno.h"
#include "getopt.h"
#include "syscall.h"
#include "sys/stat.h"
#include "mtool.h"
#include "time.h"   // #115: gmtime_r/strftime for the timestamp lines

static const char *type_name(unsigned int mode)
{
	switch (mode & S_IFMT) {
	case S_IFREG:  return "regular file";
	case S_IFDIR:  return "directory";
	case S_IFCHR:  return "character special file";
	case S_IFIFO:  return "fifo";
	default:       return "unknown type";
	}
}

static void mode_string(unsigned int mode, char *out)
{
	static const char *rwx[8] = { "---","--x","-w-","-wx","r--","r-x","rw-","rwx" };
	out[0] = ((mode & S_IFMT) == S_IFDIR) ? 'd' : '-';
	memcpy(out + 1, rwx[(mode >> 6) & 7], 3);
	memcpy(out + 4, rwx[(mode >> 3) & 7], 3);
	memcpy(out + 7, rwx[mode & 7], 3);
	out[10] = '\0';
}

static void fat_attr_string(unsigned int a, char *out, size_t n)
{
	char b[32];
	int o = 0;
	if (a & FSPERM_FAT_READONLY)  b[o++] = 'R';
	if (a & FSPERM_FAT_HIDDEN)    b[o++] = 'H';
	if (a & FSPERM_FAT_SYSTEM)    b[o++] = 'S';
	if (a & FSPERM_FAT_VOLUME_ID) b[o++] = 'V';
	if (a & FSPERM_FAT_DIRECTORY) b[o++] = 'D';
	if (a & FSPERM_FAT_ARCHIVE)   b[o++] = 'A';
	if (o == 0) b[o++] = '-';
	b[o] = '\0';
	snprintf(out, n, "%s", b);
}

// #115: format an epoch timestamp, or say plainly that there is not one.
//
// 0 IS NOT A DATE. It is the kernel's single encoding for "this filesystem does
// not know" (see the k_stat_t contract in kernel/proc/syscall.c), and nothing
// in this OS can produce a legitimate epoch 0: the converter's floor is
// 1980-01-01 and it refuses anything below rather than clamping. Printing
// "1970-01-01 00:00:00" for it would be exactly the fabricated-looking-real
// value this ticket exists to remove - and it is what gui/properties.c has been
// doing for years, rendering "1980-00-00 00:00" from a pair of zero FAT words.
static void time_string(unsigned long t, char *out, size_t n)
{
	if (t == 0) {
		snprintf(out, n, "unknown (this filesystem recorded none)");
		return;
	}
	time_t tt = (time_t)t;
	struct tm tmv;
	if (!gmtime_r(&tt, &tmv)) { snprintf(out, n, "unrepresentable (%lu)", t); return; }
	// UTC, and labelled UTC: the kernel reads the RTC as UTC and this OS has no
	// timezone database in the kernel, so pretending it is local time would be
	// an unstated offset.
	if (strftime(out, n, "%Y-%m-%d %H:%M:%S UTC", &tmv) == 0)
		snprintf(out, n, "unrepresentable (%lu)", t);
}

static int do_one(const char *operand, void *ctx)
{
	(void)ctx;
	char full[1024];
	if (mtool_resolve(operand, full, sizeof full) != 0) {
		mtool_warn("%s: path too long", operand);
		return 1;
	}

	struct stat st;
	if (stat(full, &st) != 0) {
		mtool_warn("cannot stat '%s': error %d", operand, errno);
		return 1;
	}

	char ms[12];
	mode_string(st.st_mode, ms);

	mtool_wfmt(1, "  File: %s\n", operand);
	mtool_wfmt(1, "  Size: %-12ld Blocks: %-8ld IO Block: %-6ld %s\n",
	           st.st_size, st.st_blocks, st.st_blksize, type_name(st.st_mode));

	// The permission half, from the syscall that reports what the kernel
	// ENFORCES rather than what stat synthesises.
	fsperm_info_t fp;
	memset(&fp, 0, sizeof fp);
	if (sys_fs_perm_info(full, &fp) == 0) {
		if (fp.fs_type == FSPERM_TYPE_POSIX) {
			char pms[12];
			mode_string((st.st_mode & S_IFMT) | fp.mode, pms);
			mtool_wfmt(1, "Filesys: ext2 root (POSIX permissions, fs/perms.c)\n");
			mtool_wfmt(1, "  Mode: (%04o/%s)  Uid: %u  Gid: %u\n",
			           fp.mode & 07777, pms, fp.uid, fp.gid);
		} else if (fp.fs_type == FSPERM_TYPE_FAT) {
			char as[32];
			fat_attr_string(fp.fat_attr, as, sizeof as);
			mtool_wfmt(1, "Filesys: FAT ESP\n");
			mtool_wfmt(1, "  Attr: 0x%02x (%s)\n", fp.fat_attr, as);
			mtool_wfmt(1, "  Mode: unavailable - FAT has no POSIX mode; the kernel's "
			              "stat synthesises %s and it is not a fact about the file\n", ms);
			mtool_wfmt(1, "   Uid: unavailable - FAT stores no ownership\n");
		} else {
			mtool_wfmt(1, "Filesys: network (SMB/NFS) - permissions are enforced "
			              "server-side and are not exposed here\n");
		}
	} else {
		mtool_wfmt(1, "Filesys: unavailable - SYS_FS_PERM_INFO refused or is not "
		              "implemented on this kernel\n");
	}

	// #115: these three lines used to read "unavailable", correctly, because
	// sys_stat_path filled none of them. They are real now, and each one still
	// says WHAT KIND of value it is, because two of them are not measurements.
	mtool_wfmt(1, "Device: %lu  Inode: %lu%s  Links: %u%s\n",
	           st.st_dev, st.st_ino,
	           st.st_dev == 1 ? " (synthesised from the FAT directory entry's "
	                            "location; stable until the file is renamed)"
	                          : "",
	           st.st_nlink,
	           st.st_dev == 1 ? " (a constant: FAT stores no link count)" : "");
	{
		char ts[64];
		time_string(st.st_atime, ts, sizeof ts);
		mtool_wfmt(1, "Access: %s%s\n", ts,
		           (st.st_dev == 1 && st.st_atime) ? "  (FAT stores a DATE only; "
		                                             "the time of day is midnight)" : "");
		time_string(st.st_mtime, ts, sizeof ts);
		mtool_wfmt(1, "Modify: %s\n", ts);
		time_string(st.st_ctime, ts, sizeof ts);
		mtool_wfmt(1, "Change: %s%s\n", ts,
		           st.st_dev == 1 ? "  (FAT records CREATION here, not POSIX "
		                            "status-change time)" : "");
	}
	if (st.st_mtime == 0)
		mtool_wfmt(1, "  Note: a zero timestamp means the filesystem does not "
		              "know, NOT 1970-01-01. Every file written by this OS "
		              "before task #115 is unstamped on disk.\n");
	return 0;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);

	int c;
	while ((c = getopt(argc, argv, "c:fLt")) != -1) {
		switch (c) {
		case 'c': mtool_refuse("stat -c FORMAT",
		                       "a format string would have to be able to name "
		                       "fields this kernel does not record; the fixed "
		                       "output says which those are");
		case 'f': mtool_refuse("stat -f (filesystem status)",
		                       "there is no statfs syscall on this kernel");
		case 'L': mtool_refuse("stat -L (follow symbolic links)",
		                       "kernel/fs/ext2.c has no S_IFLNK support at all, "
		                       "so there are no symbolic links to follow");
		case 't': mtool_refuse("stat -t (terse output)",
		                       "terse output is a fixed field order that includes "
		                       "fields this kernel does not record");
		default:  mtool_bad_option(argv[optind - 1]);
		}
	}

	return mtool_each_operand(argc, argv, optind, do_one, NULL,
	                          "missing operand (usage: stat FILE...)");
}
