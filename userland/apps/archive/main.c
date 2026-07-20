// archive - MayteraOS terminal archiver tool (#321).
// Create, extract and list .gz .tar .tar.gz/.tgz .zip archives.
//
//   archive c <archive> <path>...     create (format from extension)
//   archive x <archive> [destdir]     extract (default: current dir)
//   archive l <archive>               list contents
//
#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/fcntl.h"
#include "../../libc/dirent.h"
#include "../../libc/sys/stat.h"
#include "arc.h"

enum { FMT_GZ, FMT_TAR, FMT_TGZ, FMT_ZIP, FMT_UNKNOWN };

static int ends_with(const char *s, const char *suf) {
    size_t ls = strlen(s), lf = strlen(suf);
    if (lf > ls) return 0;
    for (size_t i = 0; i < lf; i++) {
        char a = s[ls - lf + i], b = suf[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

static int detect_fmt(const char *name) {
    if (ends_with(name, ".tar.gz") || ends_with(name, ".tgz")) return FMT_TGZ;
    if (ends_with(name, ".tar")) return FMT_TAR;
    if (ends_with(name, ".zip")) return FMT_ZIP;
    if (ends_with(name, ".gz"))  return FMT_GZ;
    return FMT_UNKNOWN;
}

// ---- whole-file read into malloc'd buffer ----
static uint8_t *read_whole(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    long sz = lseek(fd, 0, SEEK_END);
    if (sz < 0) { close(fd); return 0; }
    lseek(fd, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(sz ? (size_t)sz : 1);
    if (!buf) { close(fd); return 0; }
    long off = 0;
    while (off < sz) {
        long n = read(fd, buf + off, sz - off);
        if (n <= 0) break;
        off += n;
    }
    close(fd);
    *len = (size_t)off;
    return buf;
}

static int write_whole(const char *path, const uint8_t *buf, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { printf("archive: cannot create %s (err %d)\n", path, fd); return -1; }
    size_t off = 0;
    while (off < len) {
        long n = write(fd, buf + off, len - off);
        if (n <= 0) { close(fd); return -1; }
        off += (size_t)n;
    }
    close(fd);
    return 0;
}

// ---- create: gather entries (recurse dirs) ----
typedef struct { arc_entry *v; int n, cap; } elist;

static void el_push(elist *l, arc_entry e) {
    if (l->n >= l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = (arc_entry *)realloc(l->v, sizeof(arc_entry) * l->cap);
    }
    l->v[l->n++] = e;
}

static void gather(elist *l, const char *path, const char *arcname) {
    struct stat st;
    if (stat(path, &st) != 0) { printf("archive: cannot stat %s\n", path); return; }
    arc_entry e;
    memset(&e, 0, sizeof(e));
    strncpy(e.name, arcname, sizeof(e.name) - 2);
    e.mode = st.st_mode & 07777;
    if (S_ISDIR(st.st_mode)) {
        size_t nl = strlen(e.name);
        if (nl == 0 || e.name[nl - 1] != '/') { e.name[nl] = '/'; e.name[nl + 1] = 0; }
        e.is_dir = 1; e.data = 0; e.size = 0;
        el_push(l, e);
        DIR *d = opendir(path);
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char cpath[512], cname[256];
            int pn = strlen(path);
            int slash = (pn > 0 && path[pn - 1] == '/');
            // build child path and child arcname
            int i = 0; for (; path[i] && i < 500; i++) cpath[i] = path[i];
            if (!slash && i < 500) cpath[i++] = '/';
            for (int k = 0; de->d_name[k] && i < 511; k++) cpath[i++] = de->d_name[k];
            cpath[i] = 0;
            int j = 0;
            for (; arcname[j] && j < 250; j++) cname[j] = arcname[j];
            if (j > 0 && cname[j - 1] != '/' && j < 250) cname[j++] = '/';
            for (int k = 0; de->d_name[k] && j < 255; k++) cname[j++] = de->d_name[k];
            cname[j] = 0;
            gather(l, cpath, cname);
        }
        closedir(d);
    } else {
        size_t len = 0;
        e.data = read_whole(path, &len);
        e.size = len;
        e.is_dir = 0;
        if (!e.data) { printf("archive: cannot read %s\n", path); return; }
        el_push(l, e);
    }
}

// strip leading "./" from a user path for the in-archive name
static const char *arcname_of(const char *p) {
    while (p[0] == '.' && p[1] == '/') p += 2;
    while (p[0] == '/') p++;
    return p;
}

// ---- extract: mkdir -p for the parent dirs of a path ----
static void mkdirs(const char *destdir, const char *name) {
    char path[640];
    int p = 0;
    for (int i = 0; destdir[i] && p < 500; i++) path[p++] = destdir[i];
    if (p > 0 && path[p - 1] != '/') path[p++] = '/';
    int base = p;
    for (int i = 0; name[i] && p < 639; i++) {
        if (name[i] == '/') {
            path[p] = 0;
            if (p > base) sys_mkdir(path, 0755);
        }
        path[p++] = name[i];
    }
    path[p] = 0;
}

static void join(char *out, int outsz, const char *destdir, const char *name) {
    int p = 0;
    for (int i = 0; destdir[i] && p < outsz - 1; i++) out[p++] = destdir[i];
    if (p > 0 && out[p - 1] != '/' && p < outsz - 1) out[p++] = '/';
    for (int i = 0; name[i] && p < outsz - 1; i++) out[p++] = name[i];
    out[p] = 0;
}

static int do_extract(const char *archive, const char *destdir) {
    int fmt = detect_fmt(archive);
    size_t len = 0;
    uint8_t *buf = read_whole(archive, &len);
    if (!buf) { printf("archive: cannot read %s\n", archive); return 1; }

    if (fmt == FMT_GZ) {
        size_t out = 0;
        uint8_t *data = arc_gzip_decompress(buf, len, &out);
        free(buf);
        if (!data) { printf("archive: gzip decode failed\n"); return 1; }
        // output name = archive without .gz, placed in destdir
        char base[256]; int b = 0;
        const char *an = arcname_of(archive);
        for (int i = 0; an[i] && b < 250; i++) base[b++] = an[i];
        if (b > 3 && base[b-3]=='.' && (base[b-2]=='g'||base[b-2]=='G') && (base[b-1]=='z'||base[b-1]=='Z')) b -= 3;
        base[b] = 0;
        char path[640]; join(path, sizeof(path), destdir, base);
        int rc = write_whole(path, data, out);
        free(data);
        printf("extracted %s (%u bytes)\n", path, (unsigned)out);
        return rc ? 1 : 0;
    }

    int cnt = 0; arc_entry *ents = 0;
    if (fmt == FMT_TAR) ents = arc_tar_extract(buf, len, &cnt);
    else if (fmt == FMT_TGZ) ents = arc_targz_extract(buf, len, &cnt);
    else if (fmt == FMT_ZIP) ents = arc_zip_extract(buf, len, &cnt);
    else { printf("archive: unknown format for %s\n", archive); free(buf); return 1; }
    free(buf);
    if (!ents) { printf("archive: extract failed (corrupt or unsupported)\n"); return 1; }

    sys_mkdir(destdir, 0755);
    for (int i = 0; i < cnt; i++) {
        char path[640];
        mkdirs(destdir, ents[i].name);
        join(path, sizeof(path), destdir, ents[i].name);
        if (ents[i].is_dir) {
            sys_mkdir(path, ents[i].mode ? ents[i].mode : 0755);
            printf("  dir   %s\n", ents[i].name);
        } else {
            if (write_whole(path, ents[i].data, ents[i].size) == 0)
                printf("  write %s (%u bytes)\n", ents[i].name, (unsigned)ents[i].size);
        }
    }
    arc_free_entries(ents, cnt);
    return 0;
}

static int do_list(const char *archive) {
    int fmt = detect_fmt(archive);
    size_t len = 0;
    uint8_t *buf = read_whole(archive, &len);
    if (!buf) { printf("archive: cannot read %s\n", archive); return 1; }
    if (fmt == FMT_GZ) {
        size_t out = 0;
        uint8_t *data = arc_gzip_decompress(buf, len, &out);
        free(buf);
        if (!data) { printf("archive: gzip decode failed\n"); return 1; }
        printf("%-40s %u bytes (gzip)\n", arcname_of(archive), (unsigned)out);
        free(data);
        return 0;
    }
    int cnt = 0; arc_entry *ents = 0;
    if (fmt == FMT_TAR) ents = arc_tar_extract(buf, len, &cnt);
    else if (fmt == FMT_TGZ) ents = arc_targz_extract(buf, len, &cnt);
    else if (fmt == FMT_ZIP) ents = arc_zip_extract(buf, len, &cnt);
    else { printf("archive: unknown format\n"); free(buf); return 1; }
    free(buf);
    if (!ents) { printf("archive: list failed\n"); return 1; }
    for (int i = 0; i < cnt; i++) {
        if (ents[i].is_dir) printf("  %-40s <dir>\n", ents[i].name);
        else printf("  %-40s %u bytes\n", ents[i].name, (unsigned)ents[i].size);
    }
    printf("%d entries\n", cnt);
    arc_free_entries(ents, cnt);
    return 0;
}

static int do_create(const char *archive, int npaths, char **paths) {
    int fmt = detect_fmt(archive);
    if (fmt == FMT_GZ) {
        if (npaths != 1) { printf("archive: .gz takes exactly one file\n"); return 1; }
        size_t len = 0;
        uint8_t *data = read_whole(paths[0], &len);
        if (!data) { printf("archive: cannot read %s\n", paths[0]); return 1; }
        size_t out = 0;
        uint8_t *gz = arc_gzip_compress(data, len, arcname_of(paths[0]), &out);
        free(data);
        if (!gz) { printf("archive: gzip failed\n"); return 1; }
        int rc = write_whole(archive, gz, out);
        printf("created %s (%u -> %u bytes)\n", archive, (unsigned)len, (unsigned)out);
        free(gz);
        return rc ? 1 : 0;
    }

    elist l; l.v = 0; l.n = 0; l.cap = 0;
    for (int i = 0; i < npaths; i++)
        gather(&l, paths[i], arcname_of(paths[i]));
    if (l.n == 0) { printf("archive: nothing to add\n"); return 1; }

    size_t out = 0; uint8_t *ar = 0;
    if (fmt == FMT_TAR) ar = arc_tar_create(l.v, l.n, &out);
    else if (fmt == FMT_TGZ) ar = arc_targz_create(l.v, l.n, &out);
    else if (fmt == FMT_ZIP) ar = arc_zip_create(l.v, l.n, 1, &out);
    else { printf("archive: unknown output format (use .gz/.tar/.tar.gz/.tgz/.zip)\n"); return 1; }

    // free file data we read
    for (int i = 0; i < l.n; i++) if (l.v[i].data) free(l.v[i].data);
    free(l.v);

    if (!ar) { printf("archive: create failed\n"); return 1; }
    int rc = write_whole(archive, ar, out);
    printf("created %s (%d entries, %u bytes)\n", archive, l.n, (unsigned)out);
    free(ar);
    return rc ? 1 : 0;
}

static void usage(void) {
    printf("Usage:\n");
    printf("  archive c <archive> <path>...   create  (.gz .tar .tar.gz .tgz .zip)\n");
    printf("  archive x <archive> [destdir]   extract (default: .)\n");
    printf("  archive l <archive>             list\n");
}

int main(int argc, char **argv) {
    if (argc < 3) { usage(); return 1; }
    const char *cmd = argv[1];
    if ((cmd[0] == 'c') && cmd[1] == 0) {
        if (argc < 4) { usage(); return 1; }
        return do_create(argv[2], argc - 3, &argv[3]);
    } else if (cmd[0] == 'x' && cmd[1] == 0) {
        const char *dest = (argc >= 4) ? argv[3] : ".";
        return do_extract(argv[2], dest);
    } else if (cmd[0] == 'l' && cmd[1] == 0) {
        return do_list(argv[2]);
    }
    usage();
    return 1;
}
