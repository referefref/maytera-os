// cp - copy files and directory trees
// Usage: cp [-r|-R] [-v] SOURCE DEST
//        cp [-r|-R] [-v] SOURCE... DIRECTORY
//
// #745 (local 108, second batch). Was strictly `cp <src> <dst>`: `cp a b c/`
// copied a to a FILE named b and silently ignored c, exit 0. It could not copy
// into a directory at all, so the most common form of the command produced a
// file with the name of the second source. It also had no -r.
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "getopt.h"
#include "dirent.h"
#include "mtool.h"
#include "sys/stat.h"

typedef struct { int recursive, verbose; } opts_t;

static int is_dir(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *base_name(const char *p)
{
	const char *b = p;
	for (const char *q = p; *q; q++) if (*q == '/') b = q + 1;
	return b;
}

static int copy_file(const char *src, const char *dst,
                     const char *ssh, const char *dsh, opts_t *o)
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
	if (rc == 0 && o->verbose) mtool_wfmt(1, "'%s' -> '%s'\n", ssh, dsh);
	return rc;
}

static int copy_any(const char *src, const char *dst,
                    const char *ssh, const char *dsh, opts_t *o);

static int copy_dir(const char *src, const char *dst,
                    const char *ssh, const char *dsh, opts_t *o)
{
	if (mkdir(dst, 0755) < 0 && !is_dir(dst)) {
		mtool_warn("cannot create directory '%s'", dsh);
		return 1;
	}
	DIR *d = opendir(src);
	if (!d) { mtool_warn("cannot read directory '%s'", ssh); return 1; }
	int bad = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		char s[1024], t[1024], ss[1024], ts[1024];
		if (snprintf(s, sizeof s, "%s/%s", src, e->d_name) >= (int)sizeof s ||
		    snprintf(t, sizeof t, "%s/%s", dst, e->d_name) >= (int)sizeof t) {
			mtool_warn("%s/%s: path too long", ssh, e->d_name);
			bad = 1;
			continue;
		}
		snprintf(ss, sizeof ss, "%s/%s", ssh, e->d_name);
		snprintf(ts, sizeof ts, "%s/%s", dsh, e->d_name);
		if (copy_any(s, t, ss, ts, o) != 0) bad = 1;
	}
	closedir(d);
	return bad;
}

static int copy_any(const char *src, const char *dst,
                    const char *ssh, const char *dsh, opts_t *o)
{
	if (is_dir(src)) {
		if (!o->recursive) {
			mtool_warn("-r not specified; omitting directory '%s'", ssh);
			return 1;
		}
		return copy_dir(src, dst, ssh, dsh, o);
	}
	return copy_file(src, dst, ssh, dsh, o);
}

int main(int argc, char **argv)
{
	mtool_setprog(argv[0]);
	opts_t o = { 0, 0 };
	int c;
	while ((c = getopt(argc, argv, "rRv")) != -1) {
		switch (c) {
		case 'r': case 'R': o.recursive = 1; break;
		case 'v': o.verbose = 1; break;
		default: {
			char b[4] = { '-', (char)optopt, 0, 0 };
			if (optopt == 'p' || optopt == 'a')
				mtool_refuse("cp -p / -a (preserve attributes)",
				             "this kernel exposes no syscall that sets a file's "
				             "timestamps or ownership, so nothing could be "
				             "preserved; see userland/libc/utime.h");
			mtool_bad_option(b);
		}
		}
	}

	int nop = argc - optind;
	if (nop == 0) mtool_die(MTOOL_EX_FAIL, "missing file operand");
	if (nop == 1) mtool_die(MTOOL_EX_FAIL, "missing destination file operand after '%s'",
	                        argv[optind]);

	const char *dst_arg = argv[argc - 1];
	char dst[1024];
	if (mtool_resolve(dst_arg, dst, sizeof dst) != 0)
		mtool_die(MTOOL_EX_FAIL, "%s: path too long", dst_arg);
	int dst_is_dir = is_dir(dst);

	if (nop > 2 && !dst_is_dir)
		mtool_die(MTOOL_EX_FAIL, "target '%s' is not a directory", dst_arg);

	int status = MTOOL_EX_OK;
	for (int i = optind; i < argc - 1; i++) {
		char src[1024];
		if (mtool_resolve(argv[i], src, sizeof src) != 0) {
			mtool_warn("%s: path too long", argv[i]);
			status = MTOOL_EX_FAIL;
			continue;
		}
		char target[1024], tshown[1024];
		if (dst_is_dir) {
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
		if (copy_any(src, target, argv[i], tshown, &o) != 0) status = MTOOL_EX_FAIL;
	}
	return status;
}
