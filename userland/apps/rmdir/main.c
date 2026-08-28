// rmdir - remove empty directories
// Usage: rmdir [-p] [-v] DIRECTORY...
//
// #745 (local 108, second batch). Was `rmdir(argv[1])` and exit 0, so
// `rmdir a b c` removed a only. See rm/main.c for the shared loop.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "getopt.h"
#include "mtool.h"

typedef struct {
	int parents;   // -p: also remove each parent that becomes empty
	int verbose;
} opts_t;

static int do_one(const char *operand, void *ctx)
{
	opts_t *o = (opts_t *)ctx;
	char full[1024];
	if (mtool_resolve(operand, full, sizeof full) != 0) {
		mtool_warn("%s: path too long", operand);
		return 1;
	}
	int r = rmdir(full);
	if (r < 0) {
		mtool_warn("failed to remove '%s': error %d", operand, r);
		return 1;
	}
	if (o->verbose) mtool_wfmt(1, "removed directory '%s'\n", operand);
	if (!o->parents) return 0;

	// -p: walk up removing each parent, stopping at the first one that is not
	// empty. rmdir(1) does NOT treat that as an error.
	size_t n = strlen(full);
	while (n > 1) {
		while (n > 1 && full[n - 1] != '/') n--;
		if (n <= 1) break;
		full[n - 1] = '\0';
		if (rmdir(full) < 0) break;
		if (o->verbose) mtool_wfmt(1, "removed directory '%s'\n", full);
		n = strlen(full);
	}
	return 0;
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
			char b[4] = { '-', (char)optopt, 0, 0 };
			mtool_bad_option(b);
		}
		}
	}
	return mtool_each_operand(argc, argv, optind, do_one, &o, "missing operand");
}
