// ls - list directory contents (columns, -l, -a, -1, -r, -S, -h)
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "errno.h"
#include "dirent.h"
#include "sys/stat.h"

#define MAX_ENTRIES 512
#define TERM_COLS   80

typedef struct {
    char name[64];
    int  isdir;
    long size;
} entry_t;

static entry_t g_ents[MAX_ENTRIES];
static int g_n;

static int opt_l, opt_a, opt_1, opt_r, opt_S;

static void wstr(const char *s) { int n = 0; while (s[n]) n++; if (n) write(1, s, n); }

// Join dir + "/" + name into out.
static void join(char *out, int outsz, const char *dir, const char *name) {
    int j = 0;
    for (int i = 0; dir[i] && j < outsz - 1; i++) out[j++] = dir[i];
    if (j > 0 && out[j - 1] != '/' && j < outsz - 1) out[j++] = '/';
    for (int i = 0; name[i] && j < outsz - 1; i++) out[j++] = name[i];
    out[j] = 0;
}

// Case-insensitive name compare.
static int namecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return (int)ca - (int)cb;
        a++; b++;
    }
    return (int)*a - (int)*b;
}

// Returns <0 if a should come before b (before reverse is applied).
static int entcmp(const entry_t *a, const entry_t *b) {
    if (opt_S) {
        if (a->size != b->size) return (a->size < b->size) ? 1 : -1;  // largest first
    }
    return namecmp(a->name, b->name);
}

static void sort_entries(void) {
    for (int i = 1; i < g_n; i++) {
        entry_t key = g_ents[i];
        int j = i - 1;
        while (j >= 0 && entcmp(&g_ents[j], &key) > 0) {
            g_ents[j + 1] = g_ents[j];
            j--;
        }
        g_ents[j + 1] = key;
    }
    if (opt_r) {
        for (int i = 0, k = g_n - 1; i < k; i++, k--) {
            entry_t t = g_ents[i]; g_ents[i] = g_ents[k]; g_ents[k] = t;
        }
    }
}

static void human(long sz, char *out) {
    if (sz < 1024)            { sprintf(out, "%ld", sz); return; }
    if (sz < 1024L * 1024)    { sprintf(out, "%ldK", (sz + 512) / 1024); return; }
    if (sz < 1024L*1024*1024) { sprintf(out, "%ldM", (sz + 512L*1024) / (1024*1024)); return; }
    sprintf(out, "%ldG", sz / (1024L*1024*1024));
}

static void print_long(void) {
    char num[24];
    for (int i = 0; i < g_n; i++) {
        char line[128];
        if (g_ents[i].isdir) {
            sprintf(line, "d        - %s\n", g_ents[i].name);
        } else {
            human(g_ents[i].size, num);
            sprintf(line, "-  %7s %s\n", num, g_ents[i].name);
        }
        wstr(line);
    }
}

static void print_columns(void) {
    if (opt_1 || g_n == 0) {
        for (int i = 0; i < g_n; i++) {
            wstr(g_ents[i].name);
            if (g_ents[i].isdir) wstr("/");
            wstr("\n");
        }
        return;
    }
    // Compute widest display name (+1 for trailing '/').
    int maxw = 1;
    for (int i = 0; i < g_n; i++) {
        int w = (int)strlen(g_ents[i].name) + (g_ents[i].isdir ? 1 : 0);
        if (w > maxw) maxw = w;
    }
    int colw = maxw + 2;
    int cols = TERM_COLS / colw;
    if (cols < 1) cols = 1;
    int rows = (g_n + cols - 1) / cols;
    // Column-major (down then across), like GNU ls.
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = c * rows + r;
            if (idx >= g_n) continue;
            char cell[80];
            int p = 0;
            const char *nm = g_ents[idx].name;
            for (int k = 0; nm[k] && p < (int)sizeof(cell) - 2; k++) cell[p++] = nm[k];
            if (g_ents[idx].isdir && p < (int)sizeof(cell) - 1) cell[p++] = '/';
            int last = (c == cols - 1) || (idx + rows >= g_n);
            if (!last) { while (p < colw && p < (int)sizeof(cell) - 1) cell[p++] = ' '; }
            cell[p] = 0;
            wstr(cell);
        }
        wstr("\n");
    }
}

static int list_dir(const char *path) {
    char target[320];
    if (path[0] != '/') {
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
        if (strcmp(path, ".") == 0) {
            int i = 0; while (cwd[i] && i < (int)sizeof(target) - 1) { target[i] = cwd[i]; i++; } target[i] = 0;
        } else {
            join(target, sizeof(target), cwd, path);
        }
    } else {
        int i = 0; while (path[i] && i < (int)sizeof(target) - 1) { target[i] = path[i]; i++; } target[i] = 0;
    }

    DIR *d = opendir(target);
    if (!d) {
        // Not a directory: maybe a single file argument.
        struct stat fst;
        if (stat(target, &fst) == 0) {
            g_n = 1;
            int k = 0; while (path[k] && k < 63) { g_ents[0].name[k] = path[k]; k++; }
            g_ents[0].name[k] = 0;
            g_ents[0].isdir = 0;
            g_ents[0].size = fst.st_size;
            if (opt_l) print_long(); else print_columns();
            return 0;
        }
        perror(path); return 1;
    }

    g_n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0 && g_n < MAX_ENTRIES) {
        if (!opt_a && e->d_name[0] == '.') continue;
        entry_t *en = &g_ents[g_n];
        int k = 0; while (e->d_name[k] && k < 63) { en->name[k] = e->d_name[k]; k++; } en->name[k] = 0;
        en->isdir = (e->d_type == DT_DIR);
        en->size = 0;
        if ((opt_l || opt_S) && !en->isdir) {
            char fp[384]; join(fp, sizeof(fp), target, en->name);
            struct stat st;
            if (stat(fp, &st) == 0) en->size = st.st_size;
        }
        g_n++;
    }
    closedir(d);

    sort_entries();
    if (opt_l) print_long(); else print_columns();
    return 0;
}

int main(int argc, char **argv) {
    const char *paths[64];
    int np = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (int c = 1; argv[i][c]; c++) {
                switch (argv[i][c]) {
                    case 'l': opt_l = 1; break;
                    case 'a': opt_a = 1; break;
                    case '1': opt_1 = 1; break;
                    case 'r': opt_r = 1; break;
                    case 'S': opt_S = 1; break;
                    case 'h': break;  // human sizes always on in -l
                    case 't': break;  // mtime unavailable; ignored
                    default: break;
                }
            }
        } else if (np < 64) {
            paths[np++] = argv[i];
        }
    }

    if (np == 0) return list_dir(".");
    int rc = 0;
    for (int i = 0; i < np; i++) {
        if (np > 1) { if (i) write(1, "\n", 1); wstr(paths[i]); wstr(":\n"); }
        if (list_dir(paths[i]) != 0) rc = 1;
    }
    return rc;
}
