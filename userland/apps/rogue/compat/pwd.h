
#ifndef COMPAT_PWD_H
#define COMPAT_PWD_H

struct passwd {
    char *pw_name;
    char *pw_dir;
    char *pw_shell;
    int   pw_uid;
};

static inline struct passwd *getpwuid(int uid) { (void)uid; return ((void*)0); }

#endif
