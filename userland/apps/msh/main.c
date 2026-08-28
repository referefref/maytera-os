// msh - MayteraOS Shell
// A POSIX-like shell with pipes, redirection, environment variables, and globbing
#include "../../libc/maytera.h"
#include "../../libc/unistd.h"
#include "../../libc/pwd.h"
#include "../../libc/userconf.h"   // #745: <home>/APPS on PATH
#include "../../libc/termios.h"
#include "../../libc/aiclient.h"
#include "../../libc/pipeline.h"   // #745 local 108: the ONE pipeline runner
#include "../../libc/stdio.h"      // stderr: a pipeline's errors must not go
                                   // into the pipe it failed to build

// Shell limits
#define MAX_LINE        1024
#define MAX_ARGS        64
// One definition: a pipeline's stage cap is the shared runner's cap. Two
// numbers that must agree is a bug waiting for the day they do not.
#define MAX_PIPES       MPIPE_MAX_STAGES
#define PATH_MAX        512
#define HISTORY_SIZE    64

// #112: THE SHELL'S ENVIRONMENT IS THE PROCESS'S ENVIRONMENT NOW.
//
// This used to be a private 64 x (64 + 256) table, and it was a FORKED COPY of
// a primitive libc already had. That mattered for one reason: `export FOO=bar`
// wrote into this table, the table was never carried across a spawn, and the
// child therefore never saw FOO. The shell reported success and the variable
// went nowhere, which is the exact defect #112 is about, one layer up from the
// kernel.
//
// The storage is gone. env_get/env_set/env_unset below are now three-line
// adapters over getenv/setenv/unsetenv, so what `export` writes is the same
// `environ` that libc's spawn wrappers hand to the kernel. MAX_ENV_VARS and
// MAX_VAR_LEN are gone with it: libc mallocs each entry, and the real caps are
// the kernel block's (64 entries, 511 bytes each), enforced where the block is
// actually built rather than restated here.

// Current working directory
static char cwd[PATH_MAX] = "/";

// Shell running flag
static int running = 1;

// Last exit status
static int last_status = 0;

// Command history
static char hist_buf[HISTORY_SIZE][MAX_LINE];
static int hist_count = 0;

// ============================================================================
// String utilities
// ============================================================================

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int str_starts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static void str_cat(char *dst, const char *src, int max) {
    int len = str_len(dst);
    int i = 0;
    while (src[i] && len + i < max - 1) { dst[len + i] = src[i]; i++; }
    dst[len + i] = '\0';
}

// ============================================================================
// History management
// ============================================================================

static void hist_add(const char *line) {
    if (!line || !line[0]) return;
    // Skip duplicate of last entry
    if (hist_count > 0) {
        int last = (hist_count - 1) % HISTORY_SIZE;
        if (str_eq(hist_buf[last], line)) return;
    }
    int idx = hist_count % HISTORY_SIZE;
    str_copy(hist_buf[idx], line, MAX_LINE);
    hist_count++;
}

static void builtin_history(void) {
    int start = hist_count > HISTORY_SIZE ? hist_count - HISTORY_SIZE : 0;
    for (int i = start; i < hist_count; i++) {
        int idx = i % HISTORY_SIZE;
        printf("  %d  %s\n", i + 1, hist_buf[idx]);
    }
}

// Return history entry by 1-based number (as shown by `history`), or NULL if it
// is out of range or has scrolled out of the ring buffer.
static const char *hist_get(int number) {
    if (number < 1 || number > hist_count) return NULL;
    if (number <= hist_count - HISTORY_SIZE) return NULL;  // evicted from ring
    return hist_buf[(number - 1) % HISTORY_SIZE];
}

// Most recent command (what `!!` expands to).
static const char *hist_last(void) {
    return hist_get(hist_count);
}

// Most recent command starting with `prefix` (what `!prefix` expands to).
static const char *hist_find_prefix(const char *prefix) {
    int start = hist_count > HISTORY_SIZE ? hist_count - HISTORY_SIZE : 0;
    for (int i = hist_count - 1; i >= start; i--) {
        const char *e = hist_buf[i % HISTORY_SIZE];
        if (str_starts(e, prefix)) return e;
    }
    return NULL;
}

// Bash-style history expansion: !!, !n, !-n, !prefix. Writes the expanded line
// to `out`. Returns 1 if any expansion happened, 0 if unchanged, -1 if a
// referenced event was not found (a message is printed in that case).
static int expand_history(const char *in, char *out, int max) {
    int oi = 0, changed = 0, sq = 0, dq = 0;
    for (int i = 0; in[i]; ) {
        char c = in[i];
        if (oi >= max - 1) break;
        if (c == '\'' && !dq) { sq = !sq; out[oi++] = c; i++; continue; }
        if (c == '"'  && !sq) { dq = !dq; out[oi++] = c; i++; continue; }
        if (c == '\\' && in[i + 1] == '!') { out[oi++] = '!'; i += 2; changed = 1; continue; }
        if (c != '!' || sq) { out[oi++] = c; i++; continue; }

        // c == '!' (and not in single quotes): try history expansion.
        char nx = in[i + 1];
        if (nx == 0 || nx == ' ' || nx == '\t' || nx == '=' || nx == '(') {
            out[oi++] = '!'; i++; continue;  // literal '!'
        }
        const char *ev = NULL;
        int consumed = 1;
        if (nx == '!') {
            ev = hist_last(); consumed = 2;
        } else if (nx >= '0' && nx <= '9') {
            int num = 0, k = i + 1;
            while (in[k] >= '0' && in[k] <= '9') { num = num * 10 + (in[k] - '0'); k++; }
            ev = hist_get(num); consumed = k - i;
        } else if (nx == '-') {
            int num = 0, k = i + 2;
            while (in[k] >= '0' && in[k] <= '9') { num = num * 10 + (in[k] - '0'); k++; }
            ev = (num > 0) ? hist_get(hist_count - num + 1) : NULL; consumed = k - i;
        } else {
            int k = i + 1;
            while (in[k] && in[k] != ' ' && in[k] != '\t') k++;
            char pfx[MAX_LINE];
            int pl = k - (i + 1);
            if (pl >= MAX_LINE) pl = MAX_LINE - 1;
            for (int j = 0; j < pl; j++) pfx[j] = in[i + 1 + j];
            pfx[pl] = 0;
            ev = hist_find_prefix(pfx); consumed = k - i;
        }
        if (!ev) {
            printf("msh: %.*s: event not found\n", consumed, in + i);
            return -1;
        }
        for (int j = 0; ev[j] && oi < max - 1; j++) out[oi++] = ev[j];
        i += consumed; changed = 1;
    }
    out[oi] = 0;
    return changed;
}

// ============================================================================
// Environment variable management
// ============================================================================

static const char *env_get(const char *key) {
    return getenv(key);
}

static void env_set(const char *key, const char *value) {
    setenv(key, value, 1);
}

static void env_unset(const char *key) {
    unsetenv(key);
}

static void env_init(void) {
    // #745: "<home>/APPS:/APPS", the per-user application directory first. See
    // the block comment at the top of this patch hunk's changelog entry: an app
    // a user installed for themselves lives ONLY in <home>/APPS, so without
    // this it cannot be run by name. Deduped when they are the same string,
    // which is exactly the root case (root's home is "/").
    {
        char hp[192];
        char pathv[400];
        if (userhome_path(0, "APPS", hp, sizeof(hp)) == 0 && !str_eq(hp, "/APPS")) {
            int o = 0;
            for (int i = 0; hp[i] && o < (int)sizeof(pathv) - 8; i++) pathv[o++] = hp[i];
            pathv[o++] = ':';
            const char *sysdir = "/APPS";
            for (int i = 0; sysdir[i] && o < (int)sizeof(pathv) - 1; i++) pathv[o++] = sysdir[i];
            pathv[o] = '\0';
            env_set("PATH", pathv);
        } else {
            env_set("PATH", "/APPS");
        }
    }
    env_set("SHELL", "/APPS/MSH");
    env_set("PS1", "\\u@maytera:\\w$ ");

    uid_t uid = getuid();
    struct passwd *pw = getpwuid(uid);

    if (pw && pw->pw_name[0]) {
        env_set("USER", pw->pw_name);
        if (pw->pw_dir && pw->pw_dir[0]) {
            env_set("HOME", pw->pw_dir);
            int i = 0;
            while (pw->pw_dir[i] && i < PATH_MAX - 1) {
                cwd[i] = pw->pw_dir[i];
                i++;
            }
            cwd[i] = '\0';
        } else {
            env_set("HOME", "/");
        }
    } else {
        env_set("USER", "user");
        env_set("HOME", "/");
    }

    syscall1(SYS_CHDIR, (long)cwd);
    env_set("PWD", cwd);
}

// ============================================================================
// Variable expansion: replace $VAR and ${VAR} in a string
// ============================================================================

static void expand_variables(const char *src, char *dst, int max) {
    int di = 0;
    int si = 0;

    while (src[si] && di < max - 1) {
        if (src[si] == '$') {
            si++;
            if (src[si] == '?') {
                si++;
                char num[16];
                int n = last_status;
                int ni = 0;
                if (n < 0) { dst[di++] = '-'; n = -n; }
                if (n == 0) { num[ni++] = '0'; }
                else { while (n > 0 && ni < 15) { num[ni++] = '0' + (n % 10); n /= 10; } }
                for (int j = ni - 1; j >= 0 && di < max - 1; j--) dst[di++] = num[j];
                continue;
            }
            int braced = 0;
            if (src[si] == '{') { braced = 1; si++; }

            char varname[64];
            int vi = 0;
            while (vi < 63) {
                char c = src[si];
                if (braced) {
                    if (c == '}' || c == '\0') break;
                } else {
                    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_')) break;
                }
                varname[vi++] = c;
                si++;
            }
            varname[vi] = '\0';
            if (braced && src[si] == '}') si++;

            const char *val = env_get(varname);
            if (val) {
                while (*val && di < max - 1) dst[di++] = *val++;
            }
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
}

// ============================================================================
// Glob expansion (basic * and ? patterns)
// ============================================================================

static int glob_match(const char *pattern, const char *name) {
    while (*pattern && *name) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
            while (*name) {
                if (glob_match(pattern, name)) return 1;
                name++;
            }
            return glob_match(pattern, name);
        } else if (*pattern == '?') {
            pattern++;
            name++;
        } else {
            if (*pattern != *name) return 0;
            pattern++;
            name++;
        }
    }
    while (*pattern == '*') pattern++;
    return (*pattern == '\0' && *name == '\0');
}

// ============================================================================
// Command parsing
// ============================================================================

typedef struct {
    char *args[MAX_ARGS];
    int argc;
    char *infile;
    char *outfile;
    int append;
} command_t;

static int parse_command(char *str, command_t *cmd) {
    cmd->argc = 0;
    cmd->infile = NULL;
    cmd->outfile = NULL;
    cmd->append = 0;

    char *p = str;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (*p == '<') {
            p++;
            while (*p == ' ') p++;
            cmd->infile = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
            continue;
        }
        if (*p == '>') {
            p++;
            if (*p == '>') { cmd->append = 1; p++; }
            while (*p == ' ') p++;
            cmd->outfile = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
            continue;
        }

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            if (cmd->argc < MAX_ARGS - 1) {
                cmd->args[cmd->argc++] = p;
            }
            while (*p && *p != quote) p++;
            if (*p) *p++ = '\0';
            continue;
        }

        if (cmd->argc < MAX_ARGS - 1) {
            cmd->args[cmd->argc++] = p;
        }
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }

    cmd->args[cmd->argc] = NULL;
    return cmd->argc;
}

// ============================================================================
// Built-in commands
// ============================================================================

static int builtin_cd(command_t *cmd) {
    const char *dir = cmd->argc > 1 ? cmd->args[1] : env_get("HOME");
    if (!dir) dir = "/";

    char old_cwd[PATH_MAX];
    str_copy(old_cwd, cwd, PATH_MAX);

    if (*dir == '/') {
        str_copy(cwd, dir, PATH_MAX);
    } else if (str_eq(dir, "..")) {
        int len = str_len(cwd);
        if (len > 1) {
            len--;
            while (len > 0 && cwd[len] != '/') len--;
            if (len == 0) len = 1;
            cwd[len] = '\0';
        }
    } else {
        int len = str_len(cwd);
        if (len > 1) { cwd[len] = '/'; cwd[len + 1] = '\0'; }
        str_cat(cwd, dir, PATH_MAX);
    }

    long r = syscall1(SYS_CHDIR, (long)cwd);
    if (r < 0) {
        str_copy(cwd, old_cwd, PATH_MAX);
        printf("msh: cd: %s: No such file or directory\n", dir);
        return 1;
    }

    env_set("PWD", cwd);
    return 0;
}

static int builtin_export(command_t *cmd) {
    for (int i = 1; i < cmd->argc; i++) {
        char *arg = cmd->args[i];
        char *eq = arg;
        while (*eq && *eq != '=') eq++;
        if (*eq == '=') {
            *eq = '\0';
            env_set(arg, eq + 1);
        }
    }
    return 0;
}

static int builtin_unset(command_t *cmd) {
    for (int i = 1; i < cmd->argc; i++) {
        env_unset(cmd->args[i]);
    }
    return 0;
}

static int builtin_env(void) {
    // The real environment, in the order libc holds it. Before #112 this
    // printed a table that only this process could see.
    if (!environ) return 0;
    for (int i = 0; environ[i]; i++) printf("%s\n", environ[i]);
    return 0;
}

static int builtin_pwd(void) {
    printf("%s\n", cwd);
    return 0;
}


static int builtin_exit(command_t *cmd) {
    int code = cmd->argc > 1 ? atoi(cmd->args[1]) : 0;
    running = 0;
    return code;
}

// screenshot [path]: request a remote screen capture. msh cannot map the
// composited framebuffer itself (only the compositor owns it), so this simply
// drops a /SCREENSHOT.REQ trigger that the compositor services once per frame.
// With no arg the compositor writes /SCREENSHOT.BMP; an absolute path arg
// (e.g. `screenshot /MYSHOT.BMP`) is passed through as the output target.
static int builtin_screenshot(command_t *cmd) {
    const char *body = "1\n";
    if (cmd->argc > 1 && cmd->args[1][0] == '/') body = cmd->args[1];
    int fd = sys_open("/SCREENSHOT.REQ", 0x41 | 0x200 /*O_WRONLY|O_CREAT|O_TRUNC*/);
    if (fd < 0) {
        printf("screenshot: could not create /SCREENSHOT.REQ\n");
        return 1;
    }
    unsigned long len = 0;
    while (body[len]) len++;
    sys_write(fd, body, len);
    sys_close(fd);
    printf("screenshot: requested -> %s\n",
           (cmd->argc > 1 && cmd->args[1][0] == '/') ? cmd->args[1] : "/SCREENSHOT.BMP");
    return 0;
}

// Forward declarations for scripting support
static int execute_line(char *line);

// #745 local 108: ONE builtin table, looked up by id.
//
// This used to be a chain of str_eq() calls inside try_builtin(). A pipeline
// has to ask "is this stage a builtin?" BEFORE it decides whether to spawn it,
// and answering that with a SECOND list of the same names is how two lists
// drift apart. builtin_id() is the only place the names live; try_builtin()
// and the pipeline runner both go through it.
enum {
    BI_NONE = 0, BI_CD, BI_EXPORT, BI_UNSET, BI_ENV, BI_PWD, BI_EXIT,
    BI_HISTORY, BI_SCREENSHOT, BI_TRUE, BI_FALSE
};

static int builtin_id(const command_t *cmd) {
    if (cmd->argc == 0) return BI_NONE;
    const char *name = cmd->args[0];
    if (str_eq(name, "cd"))         return BI_CD;
    if (str_eq(name, "export"))     return BI_EXPORT;
    if (str_eq(name, "unset"))      return BI_UNSET;
    // #745 local 108: `env` is a builtin ONLY with no arguments, where it
    // prints this shell's own variable table. With arguments it must reach
    // /APPS/ENV: the old builtin ignored argv entirely, so `env FOO=bar prog`
    // printed the shell's table and exited 0 without running prog - the same
    // silent-wrong-answer shape /APPS/ENV itself had.
    if (str_eq(name, "env") && cmd->argc == 1) return BI_ENV;
    if (str_eq(name, "pwd"))        return BI_PWD;
    if (str_eq(name, "exit"))       return BI_EXIT;
    if (str_eq(name, "history"))    return BI_HISTORY;
    if (str_eq(name, "screenshot")) return BI_SCREENSHOT;
    if (str_eq(name, "true"))       return BI_TRUE;
    if (str_eq(name, "false"))      return BI_FALSE;
    return BI_NONE;
}

// Run a builtin and return its exit status. stdout is whatever the caller has
// pointed fd 1 at, which is how a builtin works as a pipeline stage.
static int run_builtin(int id, command_t *cmd) {
    switch (id) {
        case BI_CD:         return builtin_cd(cmd);
        case BI_EXPORT:     return builtin_export(cmd);
        case BI_UNSET:      return builtin_unset(cmd);
        case BI_ENV:        return builtin_env();
        case BI_PWD:        return builtin_pwd();
        case BI_EXIT:       return builtin_exit(cmd);
        case BI_HISTORY:    builtin_history(); return 0;
        case BI_SCREENSHOT: return builtin_screenshot(cmd);
        case BI_TRUE:       return 0;
        case BI_FALSE:      return 1;
        default:            return 0;
    }
}

// Check if command is a builtin; returns 1 if handled, 0 if not
static int try_builtin(command_t *cmd) {
    if (cmd->argc == 0) return 1;
    int id = builtin_id(cmd);
    if (id == BI_NONE) return 0;
    last_status = run_builtin(id, cmd);
    return 1;
}

// ============================================================================
// External command execution
// ============================================================================

static int resolve_path(const char *name, char *out, int max) {
    if (name[0] == '/') {
        str_copy(out, name, max);
        return 0;
    }

    const char *path = env_get("PATH");
    if (!path) path = "/APPS";

    const char *p = path;
    while (*p) {
        char dir[PATH_MAX];
        int di = 0;
        while (*p && *p != ':' && di < PATH_MAX - 1) dir[di++] = *p++;
        dir[di] = '\0';
        if (*p == ':') p++;

        str_copy(out, dir, max);
        str_cat(out, "/", max);
        str_cat(out, name, max);

        int fd = open(out, 0);
        if (fd >= 0) {
            close(fd);
            return 0;
        }

        // Try uppercase (FAT filesystem convention)
        char upper_name[64];
        int ni = 0;
        while (name[ni] && ni < 63) {
            char c = name[ni];
            if (c >= 'a' && c <= 'z') c -= 32;
            upper_name[ni] = c;
            ni++;
        }
        upper_name[ni] = '\0';

        str_copy(out, dir, max);
        str_cat(out, "/", max);
        str_cat(out, upper_name, max);

        fd = open(out, 0);
        if (fd >= 0) {
            close(fd);
            return 0;
        }

        // Try with a ".ELF" extension (exact + uppercased), so an app
        // installed as /APPS/foo.ELF runs by typing "foo".
        str_copy(out, dir, max);
        str_cat(out, "/", max);
        str_cat(out, name, max);
        str_cat(out, ".ELF", max);
        fd = open(out, 0);
        if (fd >= 0) { close(fd); return 0; }

        str_copy(out, dir, max);
        str_cat(out, "/", max);
        str_cat(out, upper_name, max);
        str_cat(out, ".ELF", max);
        fd = open(out, 0);
        if (fd >= 0) { close(fd); return 0; }
    }

    // Per-game nested convention: /GAMES/<UPPER>/<UPPER>.ELF (games ship in
    // their own directory beside their data files).
    {
        char up[64];
        int ni = 0;
        while (name[ni] && ni < 63) {
            char c = name[ni];
            if (c >= 'a' && c <= 'z') c -= 32;
            up[ni] = c; ni++;
        }
        up[ni] = '\0';
        str_copy(out, "/GAMES/", max);
        str_cat(out, up, max);
        str_cat(out, "/", max);
        str_cat(out, up, max);
        str_cat(out, ".ELF", max);
        int fd = open(out, 0);
        if (fd >= 0) { close(fd); return 0; }
    }

    return -1;
}

static int has_glob(const char *s) {
    while (*s) {
        if (*s == '*' || *s == '?') return 1;
        s++;
    }
    return 0;
}

static void expand_globs(command_t *cmd) {
    char *new_args[MAX_ARGS];
    int new_argc = 0;

    for (int i = 0; i < cmd->argc && new_argc < MAX_ARGS - 1; i++) {
        if (!has_glob(cmd->args[i])) {
            new_args[new_argc++] = cmd->args[i];
            continue;
        }

        int fd = open(cwd, 0);
        if (fd < 0) {
            new_args[new_argc++] = cmd->args[i];
            continue;
        }

        int matched = 0;
        dirent_t de;
        while (sys_readdir_raw(fd, &de) == 0 && new_argc < MAX_ARGS - 1) {
            if (de.name[0] == '.' && cmd->args[i][0] != '.') continue;
            if (glob_match(cmd->args[i], de.name)) {
                static char glob_buf[MAX_ARGS][256];
                static int glob_idx = 0;
                str_copy(glob_buf[glob_idx % MAX_ARGS], de.name, 256);
                new_args[new_argc++] = glob_buf[glob_idx % MAX_ARGS];
                glob_idx++;
                matched++;
            }
        }
        close(fd);

        if (!matched) {
            new_args[new_argc++] = cmd->args[i];
        }
    }

    for (int i = 0; i < new_argc; i++) cmd->args[i] = new_args[i];
    cmd->args[new_argc] = NULL;
    cmd->argc = new_argc;
}

static void execute_external(command_t *cmd) {
    if (cmd->argc == 0) return;

    // #279: `runap <path> [args]` launches the program on an application
    // processor (the GUI-terminal twin of the RC `launchap` command). Set
    // the kernel one-shot "next proc on an AP" flag, then drop the `runap`
    // prefix and spawn the rest of the line normally.
    if (str_eq(cmd->args[0], "runap")) {
        if (cmd->argc < 2) { printf("usage: runap <path> [args]\n"); last_status = 2; return; }
        sys_run_next_on_ap();
        for (int i = 0; i < cmd->argc - 1; i++) cmd->args[i] = cmd->args[i + 1];
        cmd->args[cmd->argc - 1] = 0;
        cmd->argc--;
    }

    expand_globs(cmd);

    char path[PATH_MAX];
    if (resolve_path(cmd->args[0], path, PATH_MAX) != 0) {
        printf("msh: %s: command not found\n", cmd->args[0]);
        last_status = 127;
        return;
    }

    // Build argv array: argv[0] = resolved path, argv[1..] = args
    char *argv[MAX_ARGS + 1];
    argv[0] = path;
    for (int i = 1; i < cmd->argc; i++) argv[i] = cmd->args[i];
    argv[cmd->argc] = NULL;

    // Create the child with argv. If the command has I/O redirection (> >> <),
    // use sys_spawn_redir so the kernel wires the child's stdout/stdin to files.
    int pid;
    if (cmd->infile || cmd->outfile) {
        pid = sys_spawn_redir(path, argv, cmd->argc, cmd->infile, cmd->outfile, cmd->append);
    } else {
        pid = sys_spawn_args(path, argv, cmd->argc);
    }
    if (pid < 0) {
        printf("msh: failed to spawn %s\n", path);
        last_status = 1;
        return;
    }

    // Wait for child
    int status = 0;
    sys_waitpid(pid, &status, 0);
    last_status = status;
}

// ============================================================================
// Pipelines (#745 local 108)
//
// WHAT THIS REPLACED. execute_pipeline() split the line on '|' and then ran
// the stages SEQUENTIALLY AND UNCONNECTED, with the comment "run sequentially
// for now (proper pipe fds need kernel pipe support)". `ls | wc -l` printed
// the listing and then blocked reading the terminal, exit 0. The premise in
// that comment had been false for a long time: kernel/fs/pipe.c is a real
// 64 KB ring with a blocking read and a proper EOF, SYS_PIPE/SYS_DUP2 are
// dispatched, and spawn_impl() gives the child the caller's fds[0..2]. The
// Terminal app had been running two-stage pipelines on exactly that mechanism.
//
// So this does not invent a mechanism; it calls the shared one. The Terminal's
// working implementation moved into userland/libc/pipeline.c, grew N stages,
// and both programs now go through mpipe_run(). See pipeline.h for the two
// kernel gaps that remain (no blocking write, no SIGPIPE).
//
// A BUILTIN stage runs in this process, because there is no fork() here. That
// is a real semantic difference from a POSIX shell (`cd` in a pipeline changes
// the shell's directory) and is documented rather than hidden.
// ============================================================================

static char *strip(char *s);   // defined with the scripting helpers below

static command_t  g_pl_cmd[MAX_PIPES];
static char       g_pl_path[MAX_PIPES][PATH_MAX];
static char      *g_pl_argv[MAX_PIPES][MAX_ARGS + 1];

static int pipeline_builtin_thunk(void *ctx) {
    command_t *c = (command_t *)ctx;
    int rc = run_builtin(builtin_id(c), c);
    // The stdout stream is line-buffered whenever fd 1 is not a tty, which is
    // exactly the case here: fd 1 is a pipe. Anything still in the buffer when
    // mpipe_run() puts fd 1 back would be flushed into the RESTORED
    // destination, i.e. into the terminal instead of into the next stage.
    fflush(stdout);
    return rc;
}

// Does LINE contain a '|' outside quotes? Cheap enough to ask of every line.
static int has_pipe(const char *s) {
    int sq = 0, dq = 0;
    for (; *s; s++) {
        if (*s == '\'' && !dq) sq = !sq;
        else if (*s == '"' && !sq) dq = !dq;
        else if (*s == '|' && !sq && !dq) return 1;
    }
    return 0;
}

// Execute "a | b | c". Returns the exit status of the LAST stage, as POSIX
// specifies, or a non-zero status after reporting a failure on stderr.
//
// Every stage is parsed and RESOLVED before a single descriptor is touched, so
// that "command not found" cannot be written into the pipe instead of to the
// user. That ordering is deliberate.
static int run_pipeline(char *line) {
    char *segments[MAX_PIPES + 1];
    mpipe_stage_t st[MAX_PIPES];
    int statuses[MAX_PIPES];
    int seg_count = 0;
    int sq = 0, dq = 0;
    char *p = line;

    // Anything already buffered belongs to the CURRENT fd 1, not to the pipe
    // we are about to point it at.
    fflush(stdout);

    segments[seg_count++] = p;
    for (; *p; p++) {
        if (*p == '\'' && !dq) sq = !sq;
        else if (*p == '"' && !sq) dq = !dq;
        else if (*p == '|' && !sq && !dq) {
            if (seg_count >= MAX_PIPES) {
                fprintf(stderr, "msh: too many pipeline stages (max %d)\n", MAX_PIPES);
                return (last_status = 2);
            }
            *p = '\0';
            segments[seg_count++] = p + 1;
        }
    }

    for (int i = 0; i < seg_count; i++) {
        char *seg = strip(segments[i]);
        command_t *c = &g_pl_cmd[i];

        st[i].path = 0; st[i].argv = 0; st[i].argc = 0;
        st[i].infile = 0; st[i].outfile = 0; st[i].append = 0;
        st[i].builtin = 0; st[i].builtin_ctx = 0;
        statuses[i] = -1;

        if (parse_command(seg, c) <= 0) {
            // An empty stage is `a || b`, a trailing '|', or a leading one.
            // A shell that silently runs the non-empty half of that is
            // guessing at what was meant.
            fprintf(stderr, "msh: syntax error near unexpected token `|'\n");
            return (last_status = 2);
        }

        int bid = builtin_id(c);
        if (bid != BI_NONE) {
            if (c->infile || c->outfile) {
                // mpipe_run() expresses a per-stage redirection through
                // sys_spawn_redir, which only exists for a SPAWNED stage.
                // Accepting it here and ignoring it is the defect this ticket
                // is about, so it is refused instead.
                fprintf(stderr, "msh: %s: redirection on a builtin inside a pipeline "
                                "is not implemented\n", c->args[0]);
                return (last_status = 2);
            }
            st[i].builtin = pipeline_builtin_thunk;
            st[i].builtin_ctx = c;
            continue;
        }

        expand_globs(c);
        if (resolve_path(c->args[0], g_pl_path[i], PATH_MAX) != 0) {
            fprintf(stderr, "msh: %s: command not found\n", c->args[0]);
            return (last_status = 127);
        }
        g_pl_argv[i][0] = g_pl_path[i];
        for (int k = 1; k < c->argc; k++) g_pl_argv[i][k] = c->args[k];
        g_pl_argv[i][c->argc] = 0;

        st[i].path   = g_pl_path[i];
        st[i].argv   = g_pl_argv[i];
        st[i].argc   = c->argc;
        st[i].infile = c->infile;
        st[i].outfile= c->outfile;
        st[i].append = c->append;
    }

    if (seg_count == 1) {
        // Not a pipeline after all (has_pipe() gates this, so this is only
        // reachable if a caller asks for it directly).
        if (st[0].builtin) return (last_status = run_builtin(builtin_id(&g_pl_cmd[0]), &g_pl_cmd[0]));
    }

    int rc = mpipe_run(st, seg_count, -1, -1, statuses);
    if (rc < 0) {
        fprintf(stderr, "msh: pipeline: %s\n", mpipe_error());
        return (last_status = 1);
    }
    return (last_status = rc);
}

// ============================================================================
// Scripting constructs: if/then/elif/else/fi, while/do/done, for/in/do/done
// ============================================================================

// Tiny token scanner for compound commands. We scan semicolons as statement
// separators and recognize the keywords: if, then, elif, else, fi, while,
// do, done, for, in.

// Strip leading/trailing whitespace in-place, return pointer to first non-ws char
static char *strip(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    int len = str_len(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t')) { s[--len] = '\0'; }
    return s;
}

// Execute a simple (non-compound) line and return its exit status.
// This is the recursive entry point for compound statements.
static int execute_simple_line(char *line);

// Split 'line' by semicolons (respecting quotes) and execute sequentially.
static int execute_line(char *line) {
    // Check for compound statements
    char *s = strip(line);
    if (!*s) return 0;

    // if ... then ... [elif ... then ...] [else ...] fi
    if (str_starts(s, "if ") || str_starts(s, "if\t")) {
        // Collect the full compound statement from the line
        // Simple single-line form: if CMD; then BODY; [elif CMD; then BODY;] [else BODY;] fi
        // We look for then/elif/else/fi tokens separated by semicolons.

        // Tokenize by semicolons
        char *tokens[64];
        int ntokens = 0;
        char *p = s;
        tokens[ntokens++] = p;
        while (*p) {
            if (*p == ';') {
                *p = '\0';
                p++;
                while (*p == ' ') p++;
                if (*p) tokens[ntokens++] = p;
            } else {
                p++;
            }
        }

        // Parse: if CMD; then BODY...; [elif CMD; then BODY...;] [else BODY...;] fi
        int pos = 0;
        int result = 0;
        int branch_taken = 0;

        while (pos < ntokens) {
            char *tok = strip(tokens[pos]);

            if (str_starts(tok, "if ") || str_starts(tok, "elif ")) {
                // Extract condition command
                char *cond = tok;
                if (str_starts(tok, "if ")) cond = tok + 3;
                else cond = tok + 5;  // elif
                cond = strip(cond);
                pos++;

                // Execute condition
                int cond_status = execute_simple_line(cond);

                // Expect 'then'
                if (pos < ntokens && str_eq(strip(tokens[pos]), "then")) {
                    pos++;
                }

                if (cond_status == 0 && !branch_taken) {
                    // Condition true: execute body until elif/else/fi
                    branch_taken = 1;
                    while (pos < ntokens) {
                        char *bt = strip(tokens[pos]);
                        if (str_starts(bt, "elif ") || str_eq(bt, "else") || str_eq(bt, "fi")) break;
                        result = execute_simple_line(bt);
                        pos++;
                    }
                } else {
                    // Skip body
                    while (pos < ntokens) {
                        char *bt = strip(tokens[pos]);
                        if (str_starts(bt, "elif ") || str_eq(bt, "else") || str_eq(bt, "fi")) break;
                        pos++;
                    }
                }
            } else if (str_eq(tok, "else")) {
                pos++;
                if (!branch_taken) {
                    branch_taken = 1;
                    while (pos < ntokens) {
                        char *bt = strip(tokens[pos]);
                        if (str_eq(bt, "fi")) break;
                        result = execute_simple_line(bt);
                        pos++;
                    }
                } else {
                    while (pos < ntokens) {
                        char *bt = strip(tokens[pos]);
                        if (str_eq(bt, "fi")) break;
                        pos++;
                    }
                }
            } else if (str_eq(tok, "fi")) {
                pos++;
                break;
            } else if (str_eq(tok, "then")) {
                pos++;  // skip stray then
            } else {
                // Not part of if/elif/else/fi, just execute
                result = execute_simple_line(tok);
                pos++;
            }
        }
        return result;
    }

    // while CMD; do BODY...; done
    if (str_starts(s, "while ") || str_starts(s, "while\t")) {
        char *tokens[64];
        int ntokens = 0;
        char *p = s;
        tokens[ntokens++] = p;
        while (*p) {
            if (*p == ';') {
                *p = '\0';
                p++;
                while (*p == ' ') p++;
                if (*p) tokens[ntokens++] = p;
            } else {
                p++;
            }
        }

        // Find condition, do, body, done
        char *cond = strip(tokens[0]) + 6;  // skip "while "
        cond = strip(cond);
        int do_pos = 1;
        if (do_pos < ntokens && str_eq(strip(tokens[do_pos]), "do")) do_pos++;

        int body_start = do_pos;
        int body_end = ntokens;
        for (int i = body_start; i < ntokens; i++) {
            if (str_eq(strip(tokens[i]), "done")) { body_end = i; break; }
        }

        int result = 0;
        int iterations = 0;
        // Make copies of condition and body since we modify in place
        char cond_copy[MAX_LINE];
        char body_copies[32][MAX_LINE];
        int nbody = body_end - body_start;
        str_copy(cond_copy, cond, MAX_LINE);
        for (int i = 0; i < nbody && i < 32; i++) {
            str_copy(body_copies[i], strip(tokens[body_start + i]), MAX_LINE);
        }

        while (iterations < 1000) {
            char cond_tmp[MAX_LINE];
            str_copy(cond_tmp, cond_copy, MAX_LINE);
            if (execute_simple_line(cond_tmp) != 0) break;
            for (int i = 0; i < nbody && i < 32; i++) {
                char tmp[MAX_LINE];
                str_copy(tmp, body_copies[i], MAX_LINE);
                result = execute_simple_line(tmp);
            }
            iterations++;
        }
        return result;
    }

    // for VAR in WORDS...; do BODY...; done
    if (str_starts(s, "for ") || str_starts(s, "for\t")) {
        char *tokens[64];
        int ntokens = 0;
        char *p = s;
        tokens[ntokens++] = p;
        while (*p) {
            if (*p == ';') {
                *p = '\0';
                p++;
                while (*p == ' ') p++;
                if (*p) tokens[ntokens++] = p;
            } else {
                p++;
            }
        }

        // Parse: for VAR in WORD1 WORD2 ...
        char *for_header = strip(tokens[0]) + 4;  // skip "for "
        for_header = strip(for_header);

        // Extract variable name
        char varname[64];
        int vi = 0;
        while (for_header[vi] && for_header[vi] != ' ' && vi < 63) {
            varname[vi] = for_header[vi];
            vi++;
        }
        varname[vi] = '\0';

        // Skip "in "
        char *words_str = for_header + vi;
        words_str = strip(words_str);
        if (str_starts(words_str, "in ")) words_str += 3;
        words_str = strip(words_str);

        // Parse words
        char *words[64];
        int nwords = 0;
        p = words_str;
        while (*p && nwords < 64) {
            while (*p == ' ') p++;
            if (!*p) break;
            words[nwords++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }

        // Find do/done
        int do_pos = 1;
        if (do_pos < ntokens && str_eq(strip(tokens[do_pos]), "do")) do_pos++;
        int body_start = do_pos;
        int body_end = ntokens;
        for (int i = body_start; i < ntokens; i++) {
            if (str_eq(strip(tokens[i]), "done")) { body_end = i; break; }
        }
        int nbody = body_end - body_start;

        // Copy body templates
        char body_copies[32][MAX_LINE];
        for (int i = 0; i < nbody && i < 32; i++) {
            str_copy(body_copies[i], strip(tokens[body_start + i]), MAX_LINE);
        }

        int result = 0;
        for (int w = 0; w < nwords; w++) {
            env_set(varname, words[w]);
            for (int i = 0; i < nbody && i < 32; i++) {
                char tmp[MAX_LINE];
                str_copy(tmp, body_copies[i], MAX_LINE);
                // Expand variables in body
                char expanded[MAX_LINE];
                expand_variables(tmp, expanded, MAX_LINE);
                result = execute_simple_line(expanded);
            }
        }
        return result;
    }

    // Not a compound statement: split by semicolons and execute each
    char *p = s;
    int result = 0;
    char *seg_start = p;
    while (1) {
        if (*p == ';' || *p == '\0') {
            int at_end = (*p == '\0');
            *p = '\0';
            char *seg = strip(seg_start);
            if (*seg) {
                result = execute_simple_line(seg);
            }
            if (at_end) break;
            p++;
            seg_start = p;
        } else {
            p++;
        }
    }
    return result;
}

// Execute a simple (non-compound) line
static int execute_simple_line(char *line) {
    command_t cmd;

    // #745 local 108: a pipeline is decided HERE rather than above execute_line(),
    // which is where the old execute_pipeline() sat. Splitting on '|' before
    // splitting on ';' meant `a | b; c` was cut into "a" and "b; c" and the
    // whole compound-statement layer (if/while/for) could never contain a
    // pipeline at all. Asking at the simple-command level makes both work.
    if (has_pipe(line)) return run_pipeline(line);

    if (parse_command(line, &cmd) <= 0) {
        // #745 local 109: A REDIRECTION WITH NO COMMAND IS A COMMAND.
        // `> file` is one of the most reflexive things anyone types at a
        // shell, and this line used to throw the whole parse away whenever
        // argc was 0 - the redirection had already been parsed into
        // cmd.outfile and was then discarded, so the shell silently did
        // nothing at all. There is no kernel defect behind it: O_TRUNC empties
        // a file correctly on both filesystems (measured, #745 local 109); the
        // shell simply never asked.
        //
        // POSIX: the redirections of a command with no words are still
        // performed, in order, in a subshell environment. That is exactly an
        // open() and a close() here.
        if (cmd.outfile && cmd.outfile[0]) {
            int fd = open(cmd.outfile,
                          O_WRONLY | O_CREAT | (cmd.append ? O_APPEND : O_TRUNC),
                          0644);
            if (fd < 0) {
                fprintf(stderr, "msh: %s: cannot open\n", cmd.outfile);
                return (last_status = 1);
            }
            close(fd);
        }
        if (cmd.infile && cmd.infile[0]) {
            // `< file` performs the redirection too, which for a command with
            // no words means only that the file must be openable. Reporting
            // that is the whole observable effect.
            int fd = open(cmd.infile, O_RDONLY, 0);
            if (fd < 0) {
                fprintf(stderr, "msh: %s: No such file or directory\n", cmd.infile);
                return (last_status = 1);
            }
            close(fd);
        }
        if (cmd.outfile || cmd.infile) return (last_status = 0);
        return 0;
    }

    // Try builtins first
    if (try_builtin(&cmd)) return last_status;

    // External command
    execute_external(&cmd);
    return last_status;
}

// ============================================================================
// Main shell loop
// ============================================================================

static void print_prompt(void) {
    const char *user = env_get("USER");
    if (!user || !user[0]) user = "user";
    printf("\033[32m%s@maytera\033[0m:\033[34m%s\033[0m$ ", user, cwd);
}

// ---------------------------------------------------------------------------
// Raw-mode line editor (readline-lite): arrow-key history, cursor movement.
//
// The tty is switched to raw mode only while editing a line, then restored to
// canonical mode before a command runs (so child programs see a normal tty).
// In canonical mode the line discipline buffers whole lines and arrow keys only
// arrive as literal ESC[ sequences after Enter, so interactive editing requires
// raw mode. msh does all echoing itself while raw.
// ---------------------------------------------------------------------------
static struct termios g_saved_termios;
static int g_raw_active = 0;
static int g_have_tty   = 0;

static int term_enter_raw(void) {
    struct termios t;
    if (tcgetattr(0, &t) != 0) { g_have_tty = 0; return -1; }
    g_have_tty = 1;
    g_saved_termios = t;
    t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(IXON | ICRNL | INLCR);
    t.c_oflag &= ~OPOST;                 // we emit explicit \r\n while raw
    t.c_cc[VMIN]  = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0) { g_have_tty = 0; return -1; }
    g_raw_active = 1;
    return 0;
}

static void term_restore(void) {
    if (g_raw_active && g_have_tty) {
        tcsetattr(0, TCSANOW, &g_saved_termios);
        g_raw_active = 0;
    }
}

// Repaint the whole input line (prompt + buffer) and place the cursor at `cur`.
// Reprinting the prompt each edit keeps positioning simple despite its ANSI
// color codes; lines are short so the cost over SSH is negligible.
static void editor_redraw(const char *buf, int len, int cur) {
    putchar('\r');
    printf("\033[K");          // clear from cursor to end of line
    print_prompt();
    for (int i = 0; i < len; i++) putchar(buf[i]);
    for (int i = cur; i < len; i++) putchar('\b');
}

static int read_line(char *buf, int max) {
    int len = 0, cur = 0;
    int h_pos = hist_count;           // history browse position
    char saved[MAX_LINE];             // in-progress line stashed when browsing up
    saved[0] = 0;
    buf[0] = 0;

    print_prompt();

    // Dumb-terminal fallback: no tty / termios -> simple canonical read.
    if (term_enter_raw() != 0) {
        int pos = 0;
        while (pos < max - 1) {
            int c = getchar();
            if (c < 0) { if (pos == 0) return -1; break; }
            if (c == '\n' || c == '\r') break;
            if (c == 4) { if (pos == 0) return -1; break; }
            if (c == '\b' || c == 127) { if (pos > 0) pos--; continue; }
            if (c >= ' ' && c < 127) buf[pos++] = (char)c;
        }
        buf[pos] = 0;
        return pos;
    }

    for (;;) {
        int c = getchar();
        if (c < 0) {                  // stream closed
            term_restore();
            if (len == 0) return -1;
            buf[len] = 0;
            return len;
        }

        if (c == '\r' || c == '\n') {
            putchar('\r'); putchar('\n');
            buf[len] = 0;
            term_restore();
            return len;
        }
        if (c == 3) {                 // Ctrl+C: abandon the line
            printf("^C\r\n");
            term_restore();
            buf[0] = 0;
            return 0;
        }
        if (c == 4) {                 // Ctrl+D: EOF on empty line, else delete-fwd
            if (len == 0) { putchar('\r'); putchar('\n'); term_restore(); return -1; }
            if (cur < len) {
                for (int i = cur; i < len - 1; i++) buf[i] = buf[i + 1];
                len--; editor_redraw(buf, len, cur);
            }
            continue;
        }
        if (c == 1) { cur = 0; editor_redraw(buf, len, cur); continue; }      // ^A home
        if (c == 5) { cur = len; editor_redraw(buf, len, cur); continue; }    // ^E end
        if (c == 21) { len = 0; cur = 0; buf[0] = 0; editor_redraw(buf, len, cur); continue; } // ^U clear
        if (c == 11) { len = cur; buf[len] = 0; editor_redraw(buf, len, cur); continue; }      // ^K kill-to-end
        if (c == 23) {                // ^W delete previous word
            int e = cur;
            while (cur > 0 && buf[cur - 1] == ' ') cur--;
            while (cur > 0 && buf[cur - 1] != ' ') cur--;
            int removed = e - cur;
            if (removed > 0) {
                for (int i = cur; i + removed <= len; i++) buf[i] = buf[i + removed];
                len -= removed;
                editor_redraw(buf, len, cur);
            }
            continue;
        }

        if (c == '\033') {            // escape sequence
            int c2 = getchar();
            if (c2 != '[' && c2 != 'O') continue;
            int c3 = getchar();
            if (c3 == 'A' || c3 == 'B') {           // up / down: history
                if (c3 == 'A') {
                    if (h_pos > (hist_count > HISTORY_SIZE ? hist_count - HISTORY_SIZE : 0)) {
                        if (h_pos == hist_count) { buf[len] = 0; str_copy(saved, buf, MAX_LINE); }
                        h_pos--;
                        str_copy(buf, hist_buf[h_pos % HISTORY_SIZE], max);
                        len = str_len(buf); cur = len;
                        editor_redraw(buf, len, cur);
                    }
                } else {
                    if (h_pos < hist_count) {
                        h_pos++;
                        if (h_pos == hist_count) str_copy(buf, saved, max);
                        else str_copy(buf, hist_buf[h_pos % HISTORY_SIZE], max);
                        len = str_len(buf); cur = len;
                        editor_redraw(buf, len, cur);
                    }
                }
            } else if (c3 == 'C') {                 // right
                if (cur < len) { cur++; putchar(buf[cur - 1]); }
            } else if (c3 == 'D') {                 // left
                if (cur > 0) { cur--; putchar('\b'); }
            } else if (c3 == 'H') {                 // home
                cur = 0; editor_redraw(buf, len, cur);
            } else if (c3 == 'F') {                 // end
                cur = len; editor_redraw(buf, len, cur);
            } else if (c3 >= '0' && c3 <= '9') {    // ESC[n~  (Del=3, Home=1, End=4)
                int code = c3 - '0';
                int t;
                while ((t = getchar()) >= 0 && t != '~') {
                    if (t >= '0' && t <= '9') code = code * 10 + (t - '0');
                }
                if (code == 3 && cur < len) {        // Delete
                    for (int i = cur; i < len - 1; i++) buf[i] = buf[i + 1];
                    len--; editor_redraw(buf, len, cur);
                } else if (code == 1 || code == 7) { cur = 0; editor_redraw(buf, len, cur); }
                else if (code == 4 || code == 8) { cur = len; editor_redraw(buf, len, cur); }
            }
            continue;
        }

        if (c == '\b' || c == 127) {  // backspace
            if (cur > 0) {
                for (int i = cur - 1; i < len - 1; i++) buf[i] = buf[i + 1];
                cur--; len--;
                editor_redraw(buf, len, cur);
            }
            continue;
        }

        if (c >= ' ' && c < 127 && len < max - 1) {  // insert printable at cursor
            for (int i = len; i > cur; i--) buf[i] = buf[i - 1];
            buf[cur] = (char)c;
            len++; cur++;
            if (cur == len) putchar((char)c);        // fast path: append
            else editor_redraw(buf, len, cur);
        }
    }
}

// ---------------------------------------------------------------------------
// AI prefix (#224/#292): a line beginning with '?' sends the remainder to the
// built-in Kimi assistant via the SHARED aiclient module (the SAME tools and
// permission gate as the aichat widget; one source of truth, no drift). The
// conversation persists across '?' lines for this shell session, so the AI's
// two-step confirm flow for high-risk writes works: it asks, you answer, it acts.
//   ? what files are in /APPS
//   ? launch doom
//   ? how much disk is free
// High-risk writes (settings.set, ...) are permission-gated: the AI must ask and
// you must reply with a confirming '?' line (e.g. "? yes") before they apply.
// The call is bounded (the loop caps tool actions and the HTTPS POST has a kernel
// deadline) so it can never wedge the shell.
// ---------------------------------------------------------------------------
static int g_ai_ready = 0;   // 0=uninit, 1=ready, -1=no key

static void ai_handle(const char *rest) {
    while (*rest == ' ' || *rest == '\t') rest++;
    if (!*rest) {
        printf("Usage: ? <question or command for the AI>\n");
        return;
    }
    if (g_ai_ready == 0) {
        printf("\033[2mAI: connecting...\033[0m\n");
        g_ai_ready = aiclient_init() ? 1 : -1;
        if (g_ai_ready == 1) aiclient_reset();   // seed system prompt once per session
    }
    if (g_ai_ready != 1) {
        printf("Set your API key in Settings > AI.\n");
        return;
    }
    aiclient_add(0, rest);
    static char answer[8192];
    printf("\033[2mAI: thinking...\033[0m\n");
    int rc = aiclient_run_turn(answer, sizeof(answer), 0);
    if (rc != 0)
        printf("\033[31mAI error:\033[0m %s\n", answer);
    else
        printf("\033[36mAI:\033[0m %s\n", answer);
}

int main(int argc, char **argv) {
    env_init();

    // #745 local 108: `msh -c '<line>'`. Two reasons it exists.
    //
    //  1. It is how a pipeline can be VERIFIED. msh's line editor reads a tty;
    //     with stdin redirected from a file, getchar() finds no termios on fd 0
    //     and falls back to the kernel's global keyboard buffer (see
    //     userland/libc/stdio.c), so a script file is not a way in. Without -c
    //     the only way to drive this shell is to type at it.
    //  2. It is the shape every caller already expects (`sh -c`), including
    //     libc's system(), which today refuses honestly because no command
    //     processor could be handed a string. Wiring system() to this is a
    //     separate change and is NOT done here.
    //
    // It runs ONE line, with the same variable expansion and the same
    // execute_line() the interactive loop uses, and exits with its status.
    if (argc >= 2 && str_eq(argv[1], "-c")) {
        char cline[MAX_LINE], cexp[MAX_LINE];
        if (argc < 3) {
            fprintf(stderr, "msh: -c: option requires an argument\n");
            return 2;
        }
        str_copy(cline, argv[2], MAX_LINE);
        expand_variables(cline, cexp, MAX_LINE);
        execute_line(cexp);
        return last_status;
    }
    if (argc >= 2) {
        // A script-file operand would be the obvious next thing and is NOT
        // implemented. Saying so beats treating the argument as noise.
        fprintf(stderr, "msh: %s: script files are not implemented; use -c '<line>'\n", argv[1]);
        return 2;
    }

    printf("\033[1;36mMayteraOS Shell (msh)\033[0m\n");
    printf("Type 'exit' to quit.\n\n");

    char line[MAX_LINE];
    char expanded[MAX_LINE];

    while (running) {
        int n = read_line(line, MAX_LINE);   // prints its own prompt
        if (n < 0) {
            printf("\n");
            break;
        }
        if (n == 0) continue;

        // AI prefix: a line starting with '?' goes to the built-in assistant.
        {
            const char *q = line;
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '?') { ai_handle(q + 1); continue; }
        }

        // History expansion (!!, !n, !-n, !prefix). Bash echoes the expanded
        // command before running it, and stores the expanded form in history.
        char histexp[MAX_LINE];
        int hr = expand_history(line, histexp, MAX_LINE);
        if (hr < 0) continue;                 // event not found (already reported)
        if (hr > 0) { printf("%s\n", histexp); str_copy(line, histexp, MAX_LINE); }

        // Add to history before variable expansion
        hist_add(line);

        // Expand variables
        expand_variables(line, expanded, MAX_LINE);

        // Execute. execute_line() splits on ';' and handles the compound
        // statements; a pipeline is recognised inside it, per simple command.
        execute_line(expanded);
    }

    return last_status;
}
