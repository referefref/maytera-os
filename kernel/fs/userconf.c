// userconf.c - #683 kernel side: resolve a per-user preference path.
//
// Ring 0 bypasses perms_check() entirely, so the kernel is never DENIED one of
// these files. It still has to agree with userland about WHERE they are, or the
// relocation produces a split brain: the notification spool is the live example,
// posted to by Ring-0 seclog.c and by every app through libc/notify.c, and
// drained by the compositor. If those disagreed, security events would silently
// stop being shown while every individual component looked correct.
//
// C, not Rust, for the same reason as perms_check_leaf(): the only thing here is
// a lookup in the existing C user_entry_t table. The part with an actual failure
// mode, the bounded string join, IS in Rust (rustkern/userconf.rs).
#include "userconf.h"
#include "../proc/users.h"
#include "../gui/desktop.h"
#include "fat.h"
#include "perms.h"
#include "../mm/heap.h"
#include "../serial.h"

extern fat_fs_t g_fat_fs;

extern int userconf_join_rs(const char *home, const char *name,
                            char *out, uint32_t cap);

int userconf_kpath(const char *name, char *out, uint32_t cap) {
    uint32_t uid = desktop_get_session_uid();
    user_entry_t *u = user_lookup_uid(uid);
    const char *home = (u && u->home[0]) ? u->home : "/";
    return userconf_kpath_home(home, name, out, cap);
}

int userconf_kpath_home(const char *home, const char *name, char *out, uint32_t cap) {
    if (!home || !home[0]) return -1;
    return userconf_join_rs(home, name, out, cap) < 0 ? -1 : 0;
}

// Moved here from kernel/main.c (was static provision_ai_key(login_result_t*))
// so the cron launch path can share it. STILL C, deliberately: every line is a
// call into the C fat_read_file/fat_mkdir/fat_write_file and perms_* APIs, and
// the one piece with a real overflow shape, the path join, is already the Rust
// userconf_join_rs(). The seed copy below is bounded by `n <= 200` into a
// 256-byte buffer (8 + 200 + 1), unchanged from #684.
int ai_provision_key_for(uint32_t uid, uint32_t gid, const char *home, const char *why) {
    if (!g_fat_fs.mounted) return 0;
    if (!home || !home[0]) return 0;           // #OOBEAUTH: no home, no write
    if (!why) why = "?";

    char dest[256];
    if (userconf_kpath_home(home, "AISVC.CFG", dest, sizeof(dest)) != 0) return 0;

    // Already provisioned, or the user has their own settings: never clobber.
    uint32_t esz = 0;
    void *existing = fat_read_file(&g_fat_fs, dest, &esz);
    if (existing) { kfree(existing); return 0; }

    uint32_t ksz = 0;
    char *seed = (char *)fat_read_file(&g_fat_fs, "/CONFIG/KIMI.KEY", &ksz);
    if (!seed) {
        // The normal deployment case. The user types their key into Settings.
        return 0;
    }

    // Trim to the first line and strip trailing whitespace, matching what the
    // deleted userland load_key() did, so a hand-edited seed behaves the same.
    uint32_t n = 0;
    while (n < ksz && seed[n] != '\n' && seed[n] != '\r') n++;
    while (n > 0 && (seed[n - 1] == ' ' || seed[n - 1] == '\t')) n--;
    if (n == 0 || n > 200) { kfree(seed); return 0; }

    char buf[256];
    const char *pfx = "api_key=";
    uint32_t w = 0;
    while (pfx[w]) { buf[w] = pfx[w]; w++; }
    for (uint32_t i = 0; i < n; i++) buf[w++] = seed[i];
    buf[w++] = '\n';
    kfree(seed);

    // Ensure <home>/CONFIG exists, then write and lock the file down to the
    // user: 0600, owned by them. perms_set (not perms_on_create) because this
    // must establish the mode even though Ring 0 just created the path.
    char dir[256];
    uint32_t cut = 0;
    for (uint32_t i = 0; dest[i]; i++) if (dest[i] == '/') cut = i;
    if (cut > 0) {
        for (uint32_t i = 0; i < cut; i++) dir[i] = dest[i];
        dir[cut] = 0;
        fat_mkdir(&g_fat_fs, dir);
        perms_on_create(dir, uid, gid, 1);
    }
    if (fat_write_file(&g_fat_fs, dest, buf, w) != 0) {
        kprintf("[AI] #684: FAILED to provision %s (%s, uid=%u)\n", dest, why, uid);
        return -1;
    }
    perms_set(dest, uid, gid, 0600);
    if (perms_sync() != 0)
        kprintf("[PERMS] initial sync failed; permissions are NOT on disk\n");
    kprintf("[AI] #684: provisioned %s from the /CONFIG/KIMI.KEY seed "
            "(%s, uid=%u, mode 0600)\n", dest, why, uid);
    return 1;
}
