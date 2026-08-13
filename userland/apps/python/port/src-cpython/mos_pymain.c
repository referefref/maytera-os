/* mos_pymain.c - #359 Phase 2 CPython launcher for MayteraOS.
 *
 * Differences from the Phase-1 main:
 *   - sys.path now points at the REAL CPython stdlib shipped on the ext2 disk
 *     (/lib/python3.11) plus the script directory (/PY), so `import json`,
 *     `import argparse`, ... load the actual .py files from disk (filesystem
 *     import), not just frozen/builtin modules.
 *   - Runs a script FILE from disk: argv[1] if given (msh `python script.py`),
 *     otherwise the well-known /PY/MAIN.PY, otherwise an embedded self-test.
 *   - Uses the kernel CSPRNG for hash randomisation (/dev/urandom) instead of a
 *     fixed hash seed.
 *   - No --wrap open/fcntl: the kernel now implements SYS_FCNTL and sys_open
 *     returns -errno, and libc open() propagates it, so plain open/fcntl work.
 */
#include "Python.h"
#include <string.h>

extern void mos_register_frozen_encodings(void);

extern long write(int, const void *, unsigned long);
extern int  open(const char *, int, ...);
extern long read(int, void *, unsigned long);
extern int  close(int);
#define RAW(x) write(1, (x), sizeof(x) - 1)

#define STDLIB_PATH   L"/lib/python3.11"
#define SCRIPT_DIR    L"/PY"
#define DEFAULT_SCRIPT "/PY/MAIN.PY"

/* Embedded self-test used only if no script file is present. It still imports
   from the on-disk stdlib so it also proves filesystem import. */
static const char *SELFTEST =
    "import sys, json, textwrap, collections\n"
    "print('[selftest] version', sys.version.split()[0])\n"
    "print('[selftest] json', json.dumps({'a':[1,2,3]}))\n"
    "c = collections.Counter('abracadabra')\n"
    "print('[selftest] counter', dict(c))\n"
    "print('[selftest] wrap', textwrap.wrap('one two three four five', 12))\n"
    "print('[selftest] OK filesystem import works')\n";

/* Read an entire file via the (now errno-correct) libc open/read into a NUL
   terminated malloc buffer. Returns NULL if the file does not exist. */
static char *slurp(const char *path) {
    int fd = open(path, 0 /*O_RDONLY*/);
    if (fd < 0) return NULL;
    size_t cap = 8192, len = 0;
    char *buf = (char *)PyMem_RawMalloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *nb = (char *)PyMem_RawRealloc(buf, cap);
            if (!nb) { PyMem_RawFree(buf); close(fd); return NULL; }
            buf = nb;
        }
        long n = read(fd, buf + len, 4096);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';
    return buf;
}

int main(int argc, char **argv) {
    RAW("[MOS] python (phase2) main entered\n");
    mos_register_frozen_encodings();

    PyConfig config;
    PyConfig_InitIsolatedConfig(&config);
    config.use_hash_seed         = 0;   /* random via /dev/urandom (kernel CSPRNG) */
    config.site_import           = 0;
    config.use_frozen_modules    = 1;
    config.pathconfig_warnings   = 0;
    config.write_bytecode        = 0;   /* do not write .pyc back to ext2 */
    config.install_signal_handlers = 0;
    config.faulthandler          = 0;
    config.parse_argv            = 0;
    config.module_search_paths_set = 1;

    /* sys.path: on-disk stdlib + script dir. */
    PyWideStringList_Append(&config.module_search_paths, STDLIB_PATH);
    PyWideStringList_Append(&config.module_search_paths, SCRIPT_DIR);

    PyConfig_SetBytesString(&config, &config.program_name,     "python");
    PyConfig_SetBytesString(&config, &config.executable,       "/APPS/PYTHON.ELF");
    PyConfig_SetBytesString(&config, &config.prefix,           "/lib/python3.11");
    PyConfig_SetBytesString(&config, &config.exec_prefix,      "/lib/python3.11");
    PyConfig_SetBytesString(&config, &config.base_prefix,      "/lib/python3.11");
    PyConfig_SetBytesString(&config, &config.base_exec_prefix, "/lib/python3.11");
    PyConfig_SetBytesString(&config, &config.base_executable,  "/APPS/PYTHON.ELF");

    /* Decide what to run and set sys.argv accordingly. */
    const char *script = NULL;
    const char *dashc  = NULL;
    if (argc >= 3 && strcmp(argv[1], "-c") == 0)      dashc  = argv[2];
    else if (argc >= 2 && argv[1][0] != '-')          script = argv[1];

    /* Build sys.argv (config.argv): argv[0] = program name; append OS args as
       wide strings (ASCII widening is sufficient for our paths/args). */
    {
        wchar_t wbuf[512];
        const char *a0 = (argc >= 1 && argv && argv[0]) ? argv[0] : "python";
        int j = 0; while (a0[j] && j < 511) { wbuf[j] = (wchar_t)(unsigned char)a0[j]; j++; } wbuf[j] = 0;
        PyWideStringList_Append(&config.argv, wbuf);
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i]; j = 0;
            while (a[j] && j < 511) { wbuf[j] = (wchar_t)(unsigned char)a[j]; j++; }
            wbuf[j] = 0;
            PyWideStringList_Append(&config.argv, wbuf);
        }
        if (argc < 2) {
            /* service-spawn path (no OS argv): pretend `python /PY/MAIN.PY`. */
            const char *m = DEFAULT_SCRIPT; j = 0;
            while (m[j] && j < 511) { wbuf[j] = (wchar_t)(unsigned char)m[j]; j++; }
            wbuf[j] = 0;
            PyWideStringList_Append(&config.argv, wbuf);
        }
    }

    PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status)) {
        RAW("[MOS] Py init FAILED\n");
        Py_ExitStatusException(status);
    }
    RAW("[MOS] Py init OK\n");

    int rc = 0;
    if (dashc) {
        rc = PyRun_SimpleString(dashc);
    } else {
        const char *path = script ? script : DEFAULT_SCRIPT;
        /* First confirm the file is reachable with the (now errno-correct)
           libc open. Then hand the actual read+run to CPython's own IO/import
           machinery (the same code path stdlib import uses), so short reads are
           handled robustly and __name__/__file__/sys.path are set correctly. */
        char *probe = slurp(path);
        if (probe) {
            RAW("[MOS] running script from disk via CPython IO\n");
            PyMem_RawFree(probe);
            /* bootstrap: read via CPython open().read(), exec as __main__. */
            static const char *LOADER =
                "import sys\n"
                "p = sys.argv[-1] if len(sys.argv) > 1 else '/PY/MAIN.PY'\n"
                "with open(p) as _f: _src = _f.read()\n"
                "print('[loader] read', len(_src), 'bytes from', p)\n"
                "exec(compile(_src, p, 'exec'), {'__name__':'__main__','__file__':p})\n";
            rc = PyRun_SimpleString(LOADER);
        } else {
            RAW("[MOS] no script on disk; running embedded self-test\n");
            rc = PyRun_SimpleString(SELFTEST);
        }
    }

    RAW("[MOS] script returned to C main\n");
    if (Py_FinalizeEx() < 0) rc = 120;
    return rc;
}
