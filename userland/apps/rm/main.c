// rm - remove files and directories
// Usage: rm [-f] [-r|-R] [-v] FILE...
//
// #745 (local 108, second batch). THE DEFECT THIS REPLACES: the whole program
// was `if (argc < 2) usage; unlink(argv[1]);`, so `rm a b c` removed a, said
// nothing about b or c, and exited 0. The same three lines were in mkdir,
// rmdir and touch, and the same first-operand-only shape was in wc, head,
// tail, tee and cp. The loop is now ONE definition in userland/libc/mtool.c
// (mtool_each_operand) rather than eight copies of the same fix.
//
// It also only ever worked on ABSOLUTE paths: the kernel stores a cwd and
// nothing reads it, so `cd /HOME; rm notes.txt` unlinked /notes.txt or
// nothing. mtool_resolve() is the one place that join now lives.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "getopt.h"
#include "dirent.h"
#include "mtool.h"
#include "sys/stat.h"

typedef struct {
	int force;       // -f: a missing operand is not an error
	int recursive;   // -r/-R
	int verbose;     // -v
} opts_t;

static int rm_tree(const char *full, const char *shown, opts_t *o);

// Remove the CONTENTS of a directory, then the directory. Depth first, and
// every failure is reported and counted rather than aborting the walk.
static int rm_dir_contents(const char *full, const char *shown, opts_t *o)
{
	DIR *d = opendir(full);
	if (!d) {
		mtool_warn("cannot read directory '%s'", shown);
		return 1;
	}
	int bad = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		char child[1024], childshown[1024];
		int n = snprintf(child, sizeof child, "%s/%s", full, e->d_name);
		if (n < 0 || n >= (int)sizeof child) {
			mtool_warn("%s/%s: path too long", shown, e->d_name);
			bad = 1;
			continue;
		}
		snprintf(childshown, sizeof childshown, "%s/%s", shown, e->d_name);
		if (rm_tree(child, childshown, o) != 0) bad = 1;
	}
	closedir(d);
	return bad;
}

static int rm_tree(const char *full, const char *shown, opts_t *o)
{
	struct stat st;
	if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
		if (!o->recursive) {
			mtool_warn("cannot remove '%s': is a directory (use -r)", shown);
			return 1;
		}
		int bad = rm_dir_contents(full, shown, o);
		int r = rmdir(full);
		if (r < 0) {
			mtool_warn("cannot remove directory '%s': error %d", shown, r);
			return 1;
		}
		if (o->verbose) mtool_wfmt(1, "removed directory '%s'\n", shown);
		return bad;
	}
	int r = unlink(full);
	if (r < 0) {
		if (o->force) return 0;
		mtool_warn("cannot remove '%s': error %d", shown, r);
		return 1;
	}
	if (o->verbose) mtool_wfmt(1, "removed '%s'\n", shown);
	return 0;
}

static int do_one(const char *operand, void *ctx)
{
	opts_t *o = (opts_t *)ctx;
	char full[1024];
	if (mtool_resolve(operand, full, sizeof full) != 0) {
		mtool_warn("%s: path too long", operand);
		return 1;
	}
	return rm_tree(full, operand, o);
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	opts_t o = { 0, 0, 0 };
	int c;
	while ((c = getopt(argc, argv, "frRv")) != -1) {
		switch (c) {
		case 'f': o.force = 1; break;
		case 'r': case 'R': o.recursive = 1; break;
		case 'v': o.verbose = 1; break;
		default: {
			// LOUD. The old tools skipped an option they did not know and
			// carried on, which is the silent-wrong-answer shape this whole
			// ticket is about. -i in particular would be actively dangerous
			// to ignore: the user asked to be prompted before each deletion.
			char b[4] = { '-', (char)optopt, 0, 0 };
			mtool_bad_option(b);
		}
		}
	}
	// -f with no operands is not an error, exactly as rm(1) specifies.
	if (optind >= argc && o.force) return MTOOL_EX_OK;
	return mtool_each_operand(argc, argv, optind, do_one, &o, "missing operand");
}
