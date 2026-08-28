// mkdir - create directories
// Usage: mkdir [-p] [-v] DIRECTORY...
//
// #745 (local 108, second batch). Was `mkdir(argv[1])` and exit 0, so
// `mkdir a b c` created a and silently did not create b or c. See rm/main.c
// for the shared loop this now uses.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "getopt.h"
#include "mtool.h"
#include "sys/stat.h"

typedef struct {
	int parents;   // -p
	int verbose;   // -v
} opts_t;

static int is_dir(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int make_one(const char *full, const char *shown, opts_t *o, int leaf)
{
	int r = mkdir(full, 0755);
	if (r == 0) {
		if (o->verbose) mtool_wfmt(1, "created directory '%s'\n", shown);
		return 0;
	}
	// -p: an existing directory is success, and only for -p. Without it,
	// mkdir(1) must report the collision - a silent success here would be the
	// same class of lie the rest of this ticket is about.
	if (o->parents && is_dir(full)) return 0;
	if (!leaf && is_dir(full)) return 0;
	mtool_warn("cannot create directory '%s': error %d", shown, r);
	return 1;
}

static int do_one(const char *operand, void *ctx)
{
	opts_t *o = (opts_t *)ctx;
	char full[1024];
	if (mtool_resolve(operand, full, sizeof full) != 0) {
		mtool_warn("%s: path too long", operand);
		return 1;
	}
	if (!o->parents) return make_one(full, operand, o, 1);

	// -p: create every missing component. Walk forward turning each '/' into a
	// NUL in turn, so no second buffer and no recursion.
	size_t n = strlen(full);
	for (size_t i = 1; i < n; i++) {
		if (full[i] != '/') continue;
		full[i] = '\0';
		if (full[1] && make_one(full, full, o, 0) != 0) { full[i] = '/'; return 1; }
		full[i] = '/';
	}
	return make_one(full, operand, o, 1);
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	opts_t o = { 0, 0 };
	int c;
	while ((c = getopt(argc, argv, "pv")) != -1) {
		switch (c) {
		case 'p': o.parents = 1; break;
		case 'v': o.verbose = 1; break;
		default: {
			// -m MODE is the one worth naming: MayteraOS permissions are not
			// the ext2 mode bits (blame.md #739), so accepting -m and ignoring
			// it would tell the user their directory is 0700 when it is not.
			char b[4] = { '-', (char)optopt, 0, 0 };
			if (optopt == 'm')
				mtool_refuse("mkdir -m MODE",
				             "MayteraOS permissions are not the ext2 mode bits, "
				             "so a mode given here would not be the mode applied");
			mtool_bad_option(b);
		}
		}
	}
	return mtool_each_operand(argc, argv, optind, do_one, &o, "missing operand");
}
