// ls - list directory contents
// Usage: ls [-a] [-l] [-h] [-1] [-r] [-S] [-p] [-F] [FILE...]
//
// #745 (local 108, THIRD batch). WHAT WAS WRONG, in the order it matters:
//
// #115 (local 120) UPDATE. THE REFUSAL BELOW IS NOW HISTORY, and the reason it
// existed has been fixed at the source: kernel/proc/syscall.c sys_stat_path()
// fills st_atime/st_mtime/st_ctime from each backend's real metadata, and the
// ext2 and FAT write paths stamp them on create/write. -t, -u and -c sort for
// real.
//
// ONE THING SURVIVES FROM THE REFUSAL, and it is the important part. A
// timestamp of 0 still means "THIS FILESYSTEM DOES NOT KNOW" - it is not
// 1970-01-01, and nothing in the kernel can produce a legitimate epoch 0. Every
// file written by any build of this OS BEFORE #115 is unstamped, and that is
// most of the files on any existing volume. So:
//   * if EVERY entry's time is unknown, -t still REFUSES, because sorting a
//     column of zeroes by value produces name order dressed up as time order,
//     which is the original defect wearing a fix;
//   * if SOME are unknown, they sort LAST and ls says so on stderr, naming the
//     count. Silently interleaving unknowns among real dates would put a file
//     of unknown age in a position that asserts its age.
//
//  1. `-t` WAS PARSED AND THEN IGNORED - `case 't': break;` with the comment
//     "mtime unavailable; ignored". A sort that silently does not sort is the
//     exact defect class this ticket is about: the user asked for newest-first,
//     got name order, and nothing said so. IT IS NOW A LOUD REFUSAL, and the
//     reason is not ls's: THE KERNEL NEVER FILLS st_mtime. Measured in
//     kernel/proc/syscall.c sys_stat_path(), every branch (ext2, FAT, SMB, NFS)
//     does `memset(&st, 0, sizeof(st))` and then fills mode/nlink/size/blksize/
//     blocks and NOTHING ELSE, so st_atime, st_mtime and st_ctime are zero on
//     every file on every filesystem. There is nothing to sort by. Faking one -
//     inode order, directory order, anything - would have been a sort that looks
//     right and is meaningless. The kernel half is raised as its own finding
//     (see the CHANGELOG entry): ext2 inodes carry i_mtime and FAT directory
//     entries carry a write date/time, so on both filesystems the data is one
//     struct field away from being real.
//
//  2. AN UNRECOGNISED OPTION WAS SILENTLY SKIPPED (`default: break;`), so
//     `ls -R /` listed one directory non-recursively and exited 0, and a typo
//     produced a confident wrong answer.
//
//  3. `-h` WAS ALSO PARSED AND IGNORED, because -l printed human-rounded sizes
//     UNCONDITIONALLY. So `ls -l` could not give you a byte count at all, and
//     the flag that asks for the rounding did nothing. -l now prints bytes and
//     -h asks for the rounding, which is what those two flags mean.
//
//  4. TWO SILENT CAPS. MAX_ENTRIES was 512 with `&& g_n < MAX_ENTRIES` in the
//     read loop, so a directory with more entries listed 512 of them and exited
//     0; and every name was copied into a fixed `char name[64]` with `k < 63`,
//     so a longer filename was silently TRUNCATED and could then collide with
//     another. Both are gone: the entry table grows and names are stored whole,
//     and an allocation failure is a failure rather than a short listing.
//
//  5. IT WAS NOT COMPOSABLE, in two ways that both made `ls | ...` wrong. GNU ls
//     writes ONE ENTRY PER LINE when stdout is not a terminal; this always
//     printed 80-column output, so `ls | wc -l` counted rows rather than files.
//     And it appended '/' to every directory name unconditionally, which is what
//     -p means and is not what plain ls does. Now: isatty(1) chooses columns or
//     one-per-line, and the slash needs -p or -F.
//
//  6. THE SORT WAS CASE-INSENSITIVE while every other tool here is byte order in
//     LC_ALL=C, so `ls` disagreed with `ls | sort` on any mixed-case directory.
//     It is byte order now, which is both what sort(1) does in the C locale and
//     what makes this program diffable against ls(1) at all.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "errno.h"
#include "dirent.h"
#include "sys/stat.h"
#include "mtool.h"

#define TERM_COLS 80

typedef struct {
	char *name;
	int   isdir;
	long  size;
	// #115: the sort key for -t/-u/-c, in seconds since the UNIX epoch.
	// have_time == 0 means the filesystem did not know, NOT "1970".
	long  mtime;
	int   have_time;
} entry_t;

static entry_t *g_ents;
static int g_n, g_cap;

static int opt_l, opt_a, opt_1, opt_r, opt_S, opt_h, opt_slash;
// #115: -t sorts by mtime, -u by atime, -c by ctime. At most one is in effect;
// opt_time records WHICH field, so the "unknown" accounting and the warning
// name the right one.
static int opt_time;                 // 0 = off, else 't', 'u' or 'c'
static int g_unknown_times;          // entries whose filesystem had no timestamp

static void ents_reset(void)
{
	for (int i = 0; i < g_n; i++) free(g_ents[i].name);
	g_n = 0;
}

// Grow, and FAIL rather than stop early. The old code just stopped at 512.
static entry_t *ents_add(void)
{
	if (g_n == g_cap) {
		int ncap = g_cap ? g_cap * 2 : 128;
		entry_t *ne = (entry_t *)realloc(g_ents, (size_t)ncap * sizeof(entry_t));
		if (!ne) mtool_die(MTOOL_EX_FAIL,
		                   "out of memory after %d entries; refusing to print a "
		                   "partial listing", g_n);
		g_ents = ne;
		g_cap = ncap;
	}
	return &g_ents[g_n++];
}

static char *dupstr(const char *s)
{
	size_t n = strlen(s);
	char *p = (char *)malloc(n + 1);
	if (!p) mtool_die(MTOOL_EX_FAIL, "out of memory");
	memcpy(p, s, n + 1);
	return p;
}

static void join(char *out, int outsz, const char *dir, const char *name)
{
	int j = 0;
	for (int i = 0; dir[i] && j < outsz - 1; i++) out[j++] = dir[i];
	if (j > 0 && out[j - 1] != '/' && j < outsz - 1) out[j++] = '/';
	for (int i = 0; name[i] && j < outsz - 1; i++) out[j++] = name[i];
	out[j] = 0;
}

// #115: pick the field -t/-u/-c asked for, and record UNKNOWN as unknown.
// 0 is the kernel's one encoding for "this filesystem does not know" - see the
// k_stat_t contract in kernel/proc/syscall.c - so it must never become a sort
// position. Counting them here, at the single point they are read, is what lets
// list_dir() decide between refusing and warning.
static void ent_set_time(entry_t *e, const struct stat *st)
{
	long t;
	switch (opt_time) {
	case 'u': t = (long)st->st_atime; break;
	case 'c': t = (long)st->st_ctime; break;
	default:  t = (long)st->st_mtime; break;
	}
	if (t > 0) { e->mtime = t; e->have_time = 1; }
	else       { e->mtime = 0; e->have_time = 0; if (opt_time) g_unknown_times++; }
}

// Byte order, which is what LC_ALL=C means and what sort(1) does here.
static int entcmp(const void *pa, const void *pb)
{
	const entry_t *a = (const entry_t *)pa, *b = (const entry_t *)pb;
	int c;
	if (opt_time) {
		// Newest first, which is what -t means. An entry with no timestamp
		// sorts LAST regardless of -r: -r reverses an ORDERING, and "unknown"
		// is not a position in that ordering. Putting unknowns at one end and
		// saying so is the only presentation that does not assert an age the
		// filesystem never recorded.
		if (a->have_time != b->have_time) return a->have_time ? -1 : 1;
		if (a->have_time && a->mtime != b->mtime)
			c = (a->mtime < b->mtime) ? 1 : -1;
		else
			c = strcmp(a->name, b->name);   // stable tie-break, as ls(1) does
	}
	else if (opt_S && a->size != b->size) c = (a->size < b->size) ? 1 : -1;  // largest first
	else                             c = strcmp(a->name, b->name);
	// -r reverses the WHOLE order, including the size comparison, exactly as
	// ls(1) does. The old code sorted and then reversed the array, which had the
	// same effect; doing it in the comparator keeps one rule in one place.
	return opt_r ? -c : c;
}

static void sort_entries(void)
{
	if (g_n > 1) qsort(g_ents, (size_t)g_n, sizeof(entry_t), entcmp);
}

static void human(long sz, char *out, size_t n)
{
	if (sz < 1024)              { snprintf(out, n, "%ld", sz); return; }
	if (sz < 1024L * 1024)      { snprintf(out, n, "%ldK", (sz + 512) / 1024); return; }
	if (sz < 1024L*1024*1024)   { snprintf(out, n, "%ldM", (sz + 512L*1024) / (1024*1024)); return; }
	snprintf(out, n, "%ldG", sz / (1024L*1024*1024));
}

// Render one display cell. Returns the number of bytes actually written, never
// snprintf's would-be length: a caller that used the latter as a length would
// read past the buffer on a long name.
static int suffixed(const entry_t *e, char *out, size_t n)
{
	int w = (opt_slash && e->isdir) ? snprintf(out, n, "%s/", e->name)
	                                : snprintf(out, n, "%s", e->name);
	if (w < 0) { out[0] = '\0'; return 0; }
	if ((size_t)w >= n) w = (int)n - 1;
	return w;
}

static int print_long(void)
{
	char num[32], cell[600];
	for (int i = 0; i < g_n; i++) {
		if (g_ents[i].isdir) {
			snprintf(num, sizeof num, "-");
		} else if (opt_h) {
			human(g_ents[i].size, num, sizeof num);
		} else {
			snprintf(num, sizeof num, "%ld", g_ents[i].size);
		}
		suffixed(&g_ents[i], cell, sizeof cell);
		if (mtool_wfmt(1, "%c %8s %s\n", g_ents[i].isdir ? 'd' : '-', num, cell) != 0)
			return 1;
	}
	return 0;
}

static int print_names(void)
{
	char cell[600];
	// ONE PER LINE unless stdout is a terminal, which is ls(1)'s rule and what
	// makes `ls | wc -l` mean what everyone thinks it means.
	if (opt_1 || !isatty(1) || g_n == 0) {
		for (int i = 0; i < g_n; i++) {
			suffixed(&g_ents[i], cell, sizeof cell);
			if (mtool_wfmt(1, "%s\n", cell) != 0) return 1;
		}
		return 0;
	}
	int maxw = 1;
	for (int i = 0; i < g_n; i++) {
		int w = (int)strlen(g_ents[i].name) + ((opt_slash && g_ents[i].isdir) ? 1 : 0);
		if (w > maxw) maxw = w;
	}
	int colw = maxw + 2;
	int cols = TERM_COLS / colw;
	if (cols < 1) cols = 1;
	int rows = (g_n + cols - 1) / cols;
	for (int r = 0; r < rows; r++) {
		char lineb[1024];
		int p = 0;
		for (int c = 0; c < cols; c++) {
			int idx = c * rows + r;
			if (idx >= g_n) continue;
			int used = suffixed(&g_ents[idx], cell, sizeof cell);
			int last = (c == cols - 1) || (idx + rows >= g_n);
			int pad = last ? 0 : (colw - used > 0 ? colw - used : 1);
			if (p + used + pad + 2 > (int)sizeof lineb) break;
			memcpy(lineb + p, cell, (size_t)used); p += used;
			for (int k = 0; k < pad; k++) lineb[p++] = ' ';
		}
		lineb[p++] = '\n';
		if (mtool_wall(1, lineb, (size_t)p) != 0) return 1;
	}
	return 0;
}

static int list_dir(const char *path)
{
	char target[768];
	if (mtool_resolve(path, target, sizeof target) != 0) {
		mtool_warn("%s: path too long", path);
		return 1;
	}

	DIR *d = opendir(target);
	if (!d) {
		// Not a directory: a single file operand is still a legal listing.
		struct stat fst;
		if (stat(target, &fst) == 0) {
			ents_reset();
			entry_t *e = ents_add();
			e->name = dupstr(path);
			e->isdir = 0;
			e->size = fst.st_size;
			ent_set_time(e, &fst);
			int rc = opt_l ? print_long() : print_names();
			return rc;
		}
		mtool_warn("cannot access '%s': error %d", path, errno);
		return 1;
	}

	ents_reset();
	g_unknown_times = 0;
	struct dirent *de;
	while ((de = readdir(d)) != 0) {
		if (!opt_a && de->d_name[0] == '.') continue;
		entry_t *e = ents_add();
		e->name = dupstr(de->d_name);
		e->isdir = (de->d_type == DT_DIR);
		e->size = 0;
		e->mtime = 0;
		e->have_time = 0;
		// #115: -t/-u/-c need a stat for DIRECTORIES too, which the size-only
		// path deliberately skipped. SYS_READDIR carries no time field (its
		// dirent is sizeof-locked at 264 bytes), so one stat per entry is the
		// only way to get one; that is the same cost -l already paid.
		if (opt_l || opt_S || opt_time) {
			char fp[1024];
			join(fp, sizeof fp, target, e->name);
			struct stat st;
			if (stat(fp, &st) == 0) {
				if (!e->isdir) e->size = st.st_size;
				ent_set_time(e, &st);
			}
		}
	}
	closedir(d);

	// #115: refuse rather than produce a sort that is really name order. This
	// is the ORIGINAL refusal, narrowed from "this kernel never records times"
	// to the case where it is still true: a volume written before #115, or one
	// whose backend genuinely reports none.
	if (opt_time && g_n > 0 && g_unknown_times == g_n) {
		mtool_refuse(opt_time == 'u' ? "ls -u (sort by access time)"
		           : opt_time == 'c' ? "ls -c (sort by status-change time)"
		                             : "ls -t (sort by modification time)",
		             "not one entry in this directory carries a timestamp. "
		             "Every file written by this OS before task #115 is "
		             "unstamped on disk, and 0 means UNKNOWN, not 1970-01-01. "
		             "Sorting by a column of unknowns would give you name order "
		             "labelled as time order");
	}
	if (opt_time && g_unknown_times > 0)
		mtool_warn("%d of %d entries carry no timestamp and are listed last",
		           g_unknown_times, g_n);

	sort_entries();
	return opt_l ? print_long() : print_names();
}

int main(int argc, char **argv)
{
	const char **paths;
	int np = 0;

	mtool_setprog(argv[0]);

	paths = (const char **)malloc((size_t)(argc > 0 ? argc : 1) * sizeof(char *));
	if (!paths) mtool_die(MTOOL_EX_FAIL, "out of memory");

	for (int i = 1; i < argc; i++) {
		if (argv[i][0] == '-' && argv[i][1]) {
			for (int c = 1; argv[i][c]; c++) {
				char o[3] = { '-', argv[i][c], 0 };
				switch (argv[i][c]) {
				case 'l': opt_l = 1; break;
				case 'a': opt_a = 1; break;
				case '1': opt_1 = 1; break;
				case 'r': opt_r = 1; break;
				case 'S': opt_S = 1; break;
				case 'h': opt_h = 1; break;
				case 'p': case 'F': opt_slash = 1; break;
				// #115: real sorts now. The refusal that used to live here
				// moved into list_dir(), where it fires only when the volume
				// actually has no timestamps - which is still the common case
				// on any pre-#115 volume, so it is a narrowing, not a deletion.
				case 't': opt_time = 't'; break;
				case 'u': opt_time = 'u'; break;
				case 'c': opt_time = 'c'; break;
				case 'R':
					mtool_refuse("ls -R (recurse into subdirectories)",
					             "not implemented; name the subdirectories as "
					             "operands instead");
				default:
					mtool_bad_option(o);
				}
			}
		} else {
			paths[np++] = argv[i];
		}
	}

	int rc = 0;
	if (np == 0) {
		rc = list_dir(".");
	} else {
		for (int i = 0; i < np; i++) {
			if (np > 1) {
				if (i) mtool_wall(1, "\n", 1);
				mtool_wfmt(1, "%s:\n", paths[i]);
			}
			if (list_dir(paths[i]) != 0) rc = 1;
		}
	}
	return rc ? MTOOL_EX_FAIL : MTOOL_EX_OK;
}
