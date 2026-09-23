// du - estimate file space usage
// Usage: du [-a|-s] [-b|-h|-k] [-c] [-d N] [FILE...]
//
// Until this tool existed the image had no du(1) at all: userland/apps had
// cat, ls, stat, wc and the rest, and the only "how big is this folder" code
// anywhere was Settings' non-recursive four-folder cache sum. The GUI sibling
// is userland/apps/diskuse (Disk Usage in the System menu).
//
// SIZES ARE APPARENT SIZES. Every figure is the byte length the directory
// entry reports (SYS_OPEN + SYS_READDIR, the same numbers ls prints), summed
// over the tree. This kernel exposes no per-file allocated-block count to
// userland, so GNU du's default "blocks actually allocated" is not available
// and is NOT approximated: the default unit is 1K units of the apparent size,
// rounded up, which is exactly `du --apparent-size` on GNU. --apparent-size
// is therefore accepted and is a no-op. Nothing here guesses a cluster size.
//
//   -a            write counts for all files, not just directories
//   -s            display only a total for each operand (same as -d 0)
//   -c            produce a grand total
//   -d N          print entries at most N levels below each operand
//   -b            bytes            -k  1K units (default)   -h  human readable
//   --max-depth=N --all --summarize --total --bytes --human-readable
//   --apparent-size (no-op, see above)   --help
//
// The walk reads a whole directory, closes it, then descends, so it never
// holds more than one directory fd at a time regardless of depth. Directory
// entries named "." and ".." are skipped. An unreadable directory is reported
// on stderr, counted as 0, and makes the exit status 1, like du(1).
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "getopt.h"
#include "mtool.h"
#include "syscall.h"
#include "sys/stat.h"

#define PATHMAX 4096

typedef struct {
	int all, human, bytes, total;
	long max_depth;        // -1 = unlimited
	unsigned long long grand;
	int failed;
} opts_t;

static void emit(opts_t *o, unsigned long long bytes, const char *path)
{
	char num[40];
	if (o->bytes) {
		snprintf(num, sizeof num, "%llu", bytes);
	} else if (o->human) {
		static const char *u[] = { "", "K", "M", "G", "T" };
		int ui = 0;
		unsigned long long whole = bytes, frac = 0;
		while (whole >= 1024 && ui < 4) {
			frac = (whole % 1024) * 10 / 1024;
			whole /= 1024;
			ui++;
		}
		// GNU prints one decimal below 10 and rounds up ("1.1K", "12M").
		if (ui == 0) snprintf(num, sizeof num, "%llu", whole);
		else if (whole < 10) snprintf(num, sizeof num, "%llu.%llu%s", whole,
		                              frac + ((bytes % 1024) && !frac ? 1 : 0), u[ui]);
		else snprintf(num, sizeof num, "%llu%s", whole + (frac ? 1 : 0), u[ui]);
	} else {
		snprintf(num, sizeof num, "%llu", (bytes + 1023) / 1024);
	}
	if (mtool_wfmt(1, "%s\t%s\n", num, path) < 0) o->failed = 1;
}

// Entry list for one directory, collected before descending.
typedef struct { char *name; unsigned int size; unsigned char is_dir; } ent_t;

static int read_entries(const char *path, ent_t **out, int *count)
{
	*out = NULL; *count = 0;
	int fd = sys_open(path, 0);
	if (fd < 0) return -1;
	ent_t *arr = NULL; int n = 0, cap = 0, rc = 0;
	dirent_t de;
	for (;;) {
		int r = sys_readdir_raw(fd, &de);
		if (r > 0) break;              // end of directory
		if (r < 0) { rc = -1; break; } // read error mid-directory
		if (de.name[0] == '.' && (de.name[1] == 0 ||
		    (de.name[1] == '.' && de.name[2] == 0)))
			continue;
		if (n == cap) {
			int nc = cap ? cap * 2 : 32;
			ent_t *na = (ent_t *)realloc(arr, (size_t)nc * sizeof(ent_t));
			if (!na) { rc = -1; break; }
			arr = na; cap = nc;
		}
		size_t nl = strlen(de.name);
		arr[n].name = (char *)malloc(nl + 1);
		if (!arr[n].name) { rc = -1; break; }
		memcpy(arr[n].name, de.name, nl + 1);
		arr[n].size = de.size;
		arr[n].is_dir = DIRENT_IS_DIR(de) ? 1 : 0;
		n++;
	}
	sys_close(fd);
	*out = arr; *count = n;
	return rc;
}

static void free_entries(ent_t *arr, int n)
{
	for (int i = 0; i < n; i++) free(arr[i].name);
	free(arr);
}

// Walk the directory at path (plen chars, NUL-terminated, writable up to
// PATHMAX) at the given depth below the operand. Returns apparent bytes.
static unsigned long long walk_dir(opts_t *o, char *path, int plen, long depth)
{
	ent_t *ents; int n;
	if (read_entries(path, &ents, &n) < 0) {
		mtool_warn("cannot read directory '%s'", path);
		o->failed = 1;
		// Whatever was read before the error still counts; du does the same.
	}
	unsigned long long total = 0;
	for (int i = 0; i < n; i++) {
		int nl = (int)strlen(ents[i].name);
		int sep = !(plen == 1 && path[0] == '/');
		if (plen + sep + nl + 1 > PATHMAX) {
			mtool_warn("path too long under '%s'", path);
			o->failed = 1;
			continue;
		}
		int np = plen;
		if (sep) path[np++] = '/';
		memcpy(path + np, ents[i].name, (size_t)nl + 1);
		np += nl;
		if (ents[i].is_dir) {
			total += walk_dir(o, path, np, depth + 1);
		} else {
			total += ents[i].size;
			if (o->all && (o->max_depth < 0 || depth + 1 <= o->max_depth))
				emit(o, ents[i].size, path);
		}
		path[plen] = 0;
	}
	free_entries(ents, n);
	if (o->max_depth < 0 || depth <= o->max_depth) emit(o, total, path);
	return total;
}

static int do_operand(const char *operand, void *ctx)
{
	opts_t *o = (opts_t *)ctx;
	static char path[PATHMAX];
	if (mtool_resolve(operand, path, sizeof path) < 0) {
		mtool_warn("%s: path too long", operand);
		o->failed = 1;
		return 1;
	}
	int plen = (int)strlen(path);
	while (plen > 1 && path[plen - 1] == '/') path[--plen] = 0;

	struct stat st;
	if (stat(path, &st) < 0) {
		mtool_warn("cannot access '%s'", operand);
		o->failed = 1;
		return 1;
	}
	int before = o->failed;
	if (S_ISDIR(st.st_mode)) {
		o->grand += walk_dir(o, path, plen, 0);
	} else {
		unsigned long long b = st.st_size < 0 ? 0ULL : (unsigned long long)st.st_size;
		o->grand += b;
		emit(o, b, path);   // an explicit file operand is always printed
	}
	return o->failed != before;
}

static void usage(void)
{
	mtool_wfmt(1, "Usage: du [-a|-s] [-b|-h|-k] [-c] [-d N] [FILE...]\n"
	              "Summarize apparent disk usage of each FILE, recursively for directories.\n"
	              "  -a, --all            write counts for all files, not just directories\n"
	              "  -s, --summarize      display only a total for each argument\n"
	              "  -c, --total          produce a grand total\n"
	              "  -d, --max-depth=N    print entries at most N levels below each argument\n"
	              "  -b, --bytes          print sizes in bytes\n"
	              "  -k                   print sizes in 1K units (default)\n"
	              "  -h, --human-readable print sizes like 1K 234M 2G\n"
	              "      --apparent-size  accepted; sizes are always apparent on MayteraOS\n");
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	opts_t o;
	memset(&o, 0, sizeof o);
	o.max_depth = -1;

	static const struct option longopts[] = {
		{ "all",            no_argument,       0, 'a' },
		{ "summarize",      no_argument,       0, 's' },
		{ "total",          no_argument,       0, 'c' },
		{ "max-depth",      required_argument, 0, 'd' },
		{ "bytes",          no_argument,       0, 'b' },
		{ "human-readable", no_argument,       0, 'h' },
		{ "apparent-size",  no_argument,       0, 'A' },
		{ "help",           no_argument,       0, 'H' },
		{ 0, 0, 0, 0 }
	};
	int c, saw_s = 0;
	while ((c = getopt_long(argc, argv, "ascd:bkh", longopts, NULL)) != -1) {
		switch (c) {
		case 'a': o.all = 1; break;
		case 's': saw_s = 1; break;
		case 'c': o.total = 1; break;
		case 'd': o.max_depth = mtool_count_arg("-d", optarg); break;
		case 'b': o.bytes = 1; o.human = 0; break;
		case 'k': o.bytes = 0; o.human = 0; break;
		case 'h': o.human = 1; o.bytes = 0; break;
		case 'A': break;                       // always apparent, see header
		case 'H': usage(); return MTOOL_EX_OK;
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			mtool_bad_option(b);
		}
		}
	}
	if (saw_s && o.all)
		mtool_die(MTOOL_EX_FAIL, "cannot both summarize and show all entries");
	if (saw_s) o.max_depth = 0;

	if (optind >= argc) {
		do_operand(".", &o);
	} else {
		mtool_each_operand(argc, argv, optind, do_operand, &o, NULL);
	}
	if (o.total) emit(&o, o.grand, "total");
	return o.failed ? MTOOL_EX_FAIL : MTOOL_EX_OK;
}
