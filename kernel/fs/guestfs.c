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

// FLOOD CONTROL THAT CANNOT GO SILENT (#dosperm, 2026-09-03).
//
// This used to be a hard cap: log the first 100 denials, print one "further
// denials silent" line, and never say anything again for the rest of the boot.
// The flood-control half of that reasoning is right and is kept. The "never
// again" half hid the exact failure it was built for.
//
// MEASURED, Red Alert on the pinned image, in-kernel route: exactly 100
// [GUESTFS-DENY] lines for /DOS/RA/REDALERT.INI, then the cap line, then
// nothing for 173 more seconds while the guest retired ~5,000 kinsn/s into a
// black, unchanging window. A previous investigation read that shape as "the
// in-kernel guest gives up after ~100 attempts and proceeds", and wrote it up
// as a behavioural DIFFERENCE from the Ring-3 route (which logs its own
// denials from a different site and so kept printing). It was not a
// behavioural difference. It was this cap. A log that stops is indistinguish-
// able from a problem that stops, and that ambiguity cost a whole
// investigation the wrong conclusion.
//
// So: COUNT ALWAYS, PRINT RARELY. The first GUESTFS_DENY_LOG_MAX denials print
// in full. After that the volume falls to a geometric schedule (every doubling
// of the running total), which is at most ~20 further lines for a boot that
// denies four billion times, and each carries the running total. A guest stuck
// in a retry loop therefore stays visible forever at negligible cost.
//
// And the stuck case gets its OWN line, because it is the actionable one: the
// same slot denying the same access on the same path over and over is a guest
// that cannot make progress, which is never something a person wants to
// discover by counting log lines.
#define GUESTFS_DENY_LOG_MAX 100u
static unsigned g_deny_logged;      // how many full lines have been printed
static unsigned g_deny_total;       // every denial, always counted
static unsigned g_deny_next_milestone = 2u * GUESTFS_DENY_LOG_MAX;
// Consecutive-repeat detection. Deliberately a cheap FNV-1a fingerprint over
// (slot, access, path) rather than a stored path: this runs on a
// denial path that a guest can drive at hundreds of hertz, and a strcmp
// against a 256-byte buffer per denial is a cost with no reader.
static uint32_t g_last_fp;
static unsigned g_repeat;
static unsigned g_repeat_next = 1000u;

static uint32_t deny_fp(uint32_t slot, int access, const char *p) {
    uint32_t h = 2166136261u ^ (slot * 16777619u) ^ ((uint32_t)access << 24);
    if (p) for (const char *q = p; *q; q++) h = (h ^ (uint8_t)*q) * 16777619u;
    return h;
}

static const char *slot_name(uint32_t slot) {
    switch (slot) {
    case GUESTFS_SLOT_DOS:   return "dos";
    case GUESTFS_SLOT_WIN16: return "win16";
    case GUESTFS_SLOT_DPMI:  return "dpmi";
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

    g_deny_total++;

    // Same guest, same access, same path as last time? Then the guest is
    // retrying something it will never be allowed to do. Say so, on a
    // schedule that is loud enough to find and quiet enough to live with.
    uint32_t fp = deny_fp(slot, access, native_path);
    if (fp == g_last_fp) {
        g_repeat++;
        if (g_repeat >= g_repeat_next) {
            kprintf("[GUESTFS-STUCK] guest=%s op=%s path=%s DENIED %u times in a "
                    "row; the guest is retrying an access it can never be "
                    "granted and is making no progress\n",
                    slot_name(slot), what ? what : "?",
                    native_path ? native_path : "(null)", g_repeat);
            g_repeat_next *= 10u;
        }
    } else {
        g_last_fp = fp;
        g_repeat = 1;
        g_repeat_next = 1000u;
    }

    int print_full = (g_deny_logged < GUESTFS_DENY_LOG_MAX);
    int print_milestone = (!print_full && g_deny_total >= g_deny_next_milestone);
    if (print_full || print_milestone) {
        if (print_full) g_deny_logged++;
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
        if (print_milestone) {
            kprintf("[GUESTFS-DENY] running total %u denials this boot (full "
                    "logging stopped at %u; this line repeats on every "
                    "doubling, so a denial storm is never silent)\n",
                    g_deny_total, (unsigned)GUESTFS_DENY_LOG_MAX);
            g_deny_next_milestone = g_deny_total * 2u;
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
