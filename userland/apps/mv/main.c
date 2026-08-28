// mv - move or rename files
// Usage: mv [-f|-n] [-v] [-T] SOURCE DEST
//        mv [-f|-n] [-v] SOURCE... DIRECTORY
//        mv [-f|-n] [-v] -t DIRECTORY SOURCE...
//
// #108 follow-up. mv was NOT in local 103's survey of the twenty tools in the
// class "named after a standard utility, implements a fraction, REPORTS NO
// ERROR" - not in tier 1, not in tier 2, and not in the checked-and-honest
// list. It ships as /APPS/MV and it belonged in tier 1, which makes the survey
// an ENUMERATION that missed a member rather than a triage that judged one
// leniently. It is fixed here to the same standard as its siblings.
//
// WHAT IT DID, and why it is the worst of the family rather than another
// also-ran. The whole program was rename(argv[1], argv[2]) with a copy+unlink
// fallback, and:
//
//  1. IT DESTROYED DATA ON THE MOST ORDINARY MULTI-FILE FORM OF ITSELF.
//     `mv a.txt b.txt dest` is "move both files into dest". This mv renamed
//     a.txt ONTO b.txt - obliterating b.txt, which the user never asked to
//     touch - ignored the dest operand entirely, and exited 0. Its siblings
//     (rm, cp, mkdir...) merely acted on the first operand and ignored the
//     rest; mv used operand 2 as a DESTINATION, so the extra operands did not
//     just go unprocessed, they inverted the meaning of the command.
//
//  2. THE COPY FALLBACK LOST BYTES AND THEN DELETED THE ORIGINAL. copy_file()
//     tested `written < 0` only, so a SHORT write counted as success; control
//     then fell through to unlink(src). A partial copy followed by removing the
//     source is unrecoverable data loss, reported as exit 0.
//
//  3. It could not move a file INTO a directory (`mv f dir/` produced whatever
//     the kernel made of open("dir", O_WRONLY|O_CREAT|O_TRUNC)), had no
//     options at all - so `mv -f a b` looked for a file literally named "-f" -
//     and printed its errors to STDOUT.
//
// WHY THE SAME-FILE CHECK IS LEXICAL. mtool_resolve() normalises, and this OS
// has no symbolic links (kernel/fs/ext2.c has zero hits for S_IFLNK), so two
// paths denote the same object exactly when their normalised forms are equal.
// That is the same reasoning mtool.c states for normalise() itself, not a
// weaker shortcut taken here; st_ino is real since #115 but is 0 on FAT, so it
// would answer "same file" for every pair of files on the ESP.
//
// WHAT IS REFUSED, and why each is a refusal and not a gap:
//   -i  there is no prompt facility for a tool that may run as a pipeline
//       stage with stdin redirected; rm refuses it for the same reason, and
//       silently NOT prompting is what this ticket exists to stop.
//   -u  "move only if newer" needs both timestamps, and st_mtime is 0 on FAT
//       (kernel/proc/syscall.c sys_stat_path), so on the ESP every file would
//       compare equal and -u would silently move nothing.
//   -b/-S  no backup suffix machinery here; accepting it would mean
//       overwriting the file the user asked to have preserved.
// A cross-filesystem DIRECTORY move is refused too: rename() handles the
// same-filesystem case, and the fallback would have to copy a tree and then
// delete it, where a failure halfway leaves the tree in both places minus the
// parts already removed. cp -r followed by rm -r is the honest way to ask.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "getopt.h"
#include "mtool.h"
#include "sys/stat.h"

typedef struct { int force, noclobber, verbose; } opts_t;

static int is_dir(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0;
}

static const char *base_name(const char *p)
{
	const char *b = p;
	for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
	return b;
}

// The copy+unlink fallback, for a cross-filesystem move of a REGULAR file.
// The ordering is the whole point: the source is removed ONLY after every byte
// has been written and the descriptor has closed cleanly. The pre-fix version
// removed it after a loop that could not tell a short write from a complete
// one.
static int move_by_copy(const char *src, const char *dst,
                        const char *ssh, const char *dsh)
{
	int in = open(src, O_RDONLY);
	if (in < 0) { mtool_warn("cannot open '%s': error %d", ssh, in); return 1; }
	int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		mtool_warn("cannot create '%s': error %d", dsh, out);
		close(in);
		return 1;
	}
	char buf[8192];
	long n;
	int rc = 0;
	while ((n = read(in, buf, sizeof buf)) > 0) {
		if (mtool_wall(out, buf, (size_t)n) != 0) {
			mtool_warn("write error on '%s'", dsh);
			rc = 1;
			break;
		}
	}
	if (n < 0) { mtool_warn("read error on '%s'", ssh); rc = 1; }
	close(in);
	close(out);
	if (rc != 0) {
		// The copy did not complete. The source is the only intact copy left,
		// so it stays, and the partial destination is removed rather than left
		// looking like a successful move.
		unlink(dst);
		return 1;
	}
	if (unlink(src) < 0) {
		mtool_warn("copied to '%s' but cannot remove '%s'", dsh, ssh);
		return 1;
	}
	return 0;
}

static int move_one(const char *src, const char *dst,
                    const char *ssh, const char *dsh, opts_t *o)
{
	if (strcmp(src, dst) == 0) {
		mtool_warn("'%s' and '%s' are the same file", ssh, dsh);
		return 1;
	}
	if (!exists(src)) {
		mtool_warn("cannot stat '%s': no such file or directory", ssh);
		return 1;
	}
	if (exists(dst)) {
		if (o->noclobber) {
			if (o->verbose) mtool_wfmt(1, "skipped '%s'\n", dsh);
			return 0;                       // -n: not an error, per mv(1)
		}
		// Moving a plain file onto a directory (or the reverse) is refused by
		// mv(1) rather than silently doing the other thing.
		if (is_dir(dst) && !is_dir(src)) {
			mtool_warn("cannot overwrite directory '%s' with non-directory", dsh);
			return 1;
		}
		if (!is_dir(dst) && is_dir(src)) {
			mtool_warn("cannot overwrite non-directory '%s' with directory", dsh);
			return 1;
		}
	}

	if (rename(src, dst) == 0) {
		// GNU mv's -v line is "renamed 'a' -> 'b'", where cp's is "'a' -> 'b'".
		// Taken from the oracle's host arm, not from memory.
		if (o->verbose) mtool_wfmt(1, "renamed '%s' -> '%s'\n", ssh, dsh);
		return 0;
	}

	// rename() failed. On this OS that means the two paths are on different
	// filesystems (the FAT ESP and the ext2 root are the everyday case).
	if (is_dir(src))
		mtool_refuse("mv of a DIRECTORY across filesystems",
		             "rename() cannot span the FAT ESP and the ext2 root, and "
		             "copying a tree then deleting it leaves the tree in both "
		             "places if it fails halfway; use cp -r then rm -r");

	if (move_by_copy(src, dst, ssh, dsh) != 0) return 1;
	if (o->verbose) mtool_wfmt(1, "renamed '%s' -> '%s'\n", ssh, dsh);
	return 0;
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	opts_t o = { 0, 0, 0 };
	const char *target_dir = NULL;   // -t
	int no_target_dir = 0;           // -T
	int c;

	while ((c = getopt(argc, argv, "fnvTt:")) != -1) {
		switch (c) {
		case 'f': o.force = 1; o.noclobber = 0; break;
		case 'n': o.noclobber = 1; o.force = 0; break;
		case 'v': o.verbose = 1; break;
		case 'T': no_target_dir = 1; break;
		case 't': target_dir = optarg; break;
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			if (optopt == 'i')
				mtool_refuse("mv -i (prompt before overwrite)",
				             "there is no prompt facility for a tool that may "
				             "run as a pipeline stage with stdin redirected; "
				             "silently NOT prompting would overwrite the file "
				             "you asked to be asked about. Use -n to refuse "
				             "every overwrite");
			if (optopt == 'u')
				mtool_refuse("mv -u (move only when the source is newer)",
				             "st_mtime is 0 for every file on a FAT volume "
				             "(kernel/proc/syscall.c sys_stat_path), so on the "
				             "ESP every comparison would tie and -u would "
				             "silently move nothing");
			if (optopt == 'b' || optopt == 'S')
				mtool_refuse("mv -b / -S (backup suffix)",
				             "no backup machinery exists here, and accepting "
				             "the option would overwrite the very file it was "
				             "asked to preserve");
			mtool_bad_option(b);
		}
		}
	}

	if (target_dir && no_target_dir)
		mtool_die(MTOOL_EX_FAIL, "cannot combine -t and -T");

	int nop = argc - optind;
	if (nop == 0) mtool_die(MTOOL_EX_FAIL, "missing file operand");
	if (!target_dir && nop == 1)
		mtool_die(MTOOL_EX_FAIL, "missing destination file operand after '%s'",
		          argv[optind]);

	// Resolve the destination once. With -t it is the option argument and every
	// operand is a source; otherwise it is the LAST operand.
	const char *dst_arg = target_dir ? target_dir : argv[argc - 1];
	int last_src = target_dir ? argc : argc - 1;

	char dst[1024];
	if (mtool_resolve(dst_arg, dst, sizeof dst) != 0)
		mtool_die(MTOOL_EX_FAIL, "%s: path too long", dst_arg);

	int dst_is_dir = is_dir(dst);

	if (target_dir && !dst_is_dir)
		mtool_die(MTOOL_EX_FAIL, "target directory '%s' is not a directory", dst_arg);

	// THE CASE THE OLD mv GOT CATASTROPHICALLY WRONG. Three or more operands
	// means "move all of these into the last one", and if the last one is not a
	// directory that is an error - NOT a licence to rename operand 1 onto
	// operand 2 and drop the rest.
	if (!target_dir && nop > 2 && !dst_is_dir)
		mtool_die(MTOOL_EX_FAIL, "target '%s' is not a directory", dst_arg);

	if (no_target_dir && nop != 2)
		mtool_die(MTOOL_EX_FAIL, "-T accepts exactly two operands");

	int treat_dst_as_dir = dst_is_dir && !no_target_dir;

	int status = MTOOL_EX_OK;
	for (int i = optind; i < last_src; i++) {
		char src[1024];
		if (mtool_resolve(argv[i], src, sizeof src) != 0) {
			mtool_warn("%s: path too long", argv[i]);
			status = MTOOL_EX_FAIL;
			continue;
		}
		char target[1024], tshown[1024];
		if (treat_dst_as_dir) {
			if (snprintf(target, sizeof target, "%s/%s", dst, base_name(argv[i]))
			    >= (int)sizeof target) {
				mtool_warn("%s: path too long", argv[i]);
				status = MTOOL_EX_FAIL;
				continue;
			}
			snprintf(tshown, sizeof tshown, "%s/%s", dst_arg, base_name(argv[i]));
		} else {
			memcpy(target, dst, strlen(dst) + 1);
			snprintf(tshown, sizeof tshown, "%s", dst_arg);
		}
		if (move_one(src, target, argv[i], tshown, &o) != 0) status = MTOOL_EX_FAIL;
	}
	return status;
}
