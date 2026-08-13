// fs/guestfs.c - #708: C surface + reporting for the DOS/Win16 guest filesystem
// gate. The POLICY is rustkern/guestfs.rs; nothing here decides anything.
//
// Why any C at all, given the Rust-first rule: kprintf is variadic and is
// deliberately kept off the Rust FFI surface (the #674 permpath.rs port made
// the same call). So the decision is Rust and the reporting is C, and this file
// contains no branch that can turn a DENY into an ALLOW.

#include "guestfs.h"
#include "perms.h"
#include "../serial.h"
#include "../proc/process.h"

// Bounded, not time-rate-limited, for the reason perms.c gives at its own deny
// log: a guest in a tight retry loop must not be able to flood the serial
// console (the only debug channel on real hardware) or push earlier, more
// informative lines out of a captured log. A DOS game that probes for a file
// every frame would do exactly that.
#define GUESTFS_DENY_LOG_MAX 100u
static unsigned g_deny_logged;

static const char *slot_name(uint32_t slot) {
    switch (slot) {
    case GUESTFS_SLOT_DOS:   return "dos";
    case GUESTFS_SLOT_WIN16: return "win16";
    default:                 return "?";
    }
}

static const char *reason_name(int rc) {
    switch (rc) {
    case -1: return "BAD-SLOT";
    case -2: return "NOT-ARMED(fail-closed)";
    case -3: return "PERMS";
    case -4: return "BAD-PATH";
    case -5: return "NO-SESSION";
    case -6: return "RESOLVE";
    default: return "?";
    }
}

int guestfs_allow(uint32_t slot, const char *native_path, int access, const char *what) {
    int rc = guestfs_check_rs(slot, native_path, access);
    if (rc == 0) return 1;

    if (g_deny_logged < GUESTFS_DENY_LOG_MAX) {
        g_deny_logged++;
        uint32_t uid = 0, gid = 0;
        int have = (guestfs_cred_rs(slot, &uid, &gid) == 0);
        // Name the guest, the identity it is running as, the operation and the
        // path. Without all four a denial reaches the guest as a bare DOS
        // error code and surfaces as "the game will not load its data", with
        // nothing on the console connecting it to a mode.
        kprintf("[GUESTFS-DENY] guest=%s uid=%u gid=%u want=%c%c%c op=%s reason=%s path=%s\n",
                slot_name(slot),
                have ? uid : 0xFFFFFFFFu, have ? gid : 0xFFFFFFFFu,
                (access & R_OK) ? 'r' : '-',
                (access & W_OK) ? 'w' : '-',
                (access & X_OK) ? 'x' : '-',
                what ? what : "?",
                reason_name(rc),
                native_path ? native_path : "(null)");
        if (g_deny_logged == GUESTFS_DENY_LOG_MAX) {
            kprintf("[GUESTFS-DENY] log cap (%u) reached; further denials silent\n",
                    (unsigned)GUESTFS_DENY_LOG_MAX);
        }
    }
    return 0;
}

int guestfs_arm_caller(uint32_t slot) {
    int rc = guestfs_arm_rs(slot, PROC_AS_CALLER, 0);
    if (rc != 0) {
        kprintf("[GUESTFS] %s: REFUSED to arm from caller (%s); the guest will "
                "have NO filesystem access\n", slot_name(slot), reason_name(rc));
        return rc;
    }
    uint32_t uid = 0, gid = 0;
    guestfs_cred_rs(slot, &uid, &gid);
    kprintf("[GUESTFS] %s: armed as launching user uid=%u gid=%u\n",
            slot_name(slot), uid, gid);
    return 0;
}

int guestfs_arm_session(uint32_t slot) {
    int rc = guestfs_arm_rs(slot, PROC_AS_SESSION, 0);
    if (rc != 0) {
        kprintf("[GUESTFS] %s: REFUSED to arm from session (%s); the guest will "
                "have NO filesystem access\n", slot_name(slot), reason_name(rc));
        return rc;
    }
    uint32_t uid = 0, gid = 0;
    guestfs_cred_rs(slot, &uid, &gid);
    kprintf("[GUESTFS] %s: armed as desktop session uid=%u gid=%u\n",
            slot_name(slot), uid, gid);
    return 0;
}

void guestfs_finish(uint32_t slot) {
    uint32_t uid = 0, gid = 0, checks = 0, denies = 0;
    int have = (guestfs_cred_rs(slot, &uid, &gid) == 0);
    guestfs_stats_rs(slot, &checks, &denies);
    // checks=0 means the gate performed no work for this guest, which for a
    // guest that ran at all means it is NOT WIRED IN on this build. Say so
    // rather than printing a reassuring zero-denials line.
    kprintf("[GUESTFS] %s: run finished; identity=%s uid=%u gid=%u checks=%u denies=%u%s\n",
            slot_name(slot), have ? "armed" : "UNARMED",
            have ? uid : 0xFFFFFFFFu, have ? gid : 0xFFFFFFFFu,
            checks, denies,
            (checks == 0) ? "   <-- ZERO CHECKS: the gate did not run" : "");
    guestfs_disarm_rs(slot);
}

void guestfs_boot_selftest(void) {
    uint32_t fails = guestfs_selftest_rs();
    if (fails == 0) {
        kprintf("[GUESTFS] self-test PASS (guest fs gate policy is live)\n");
    } else {
        kprintf("[GUESTFS] self-test FAIL mask=0x%x (guest fs gate policy is "
                "NOT behaving as specified)\n", fails);
    }
}
