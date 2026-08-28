// net/firewall.c - #238: boot load, persistence and reporting for the packet
// filter. GLUE ONLY. Read the header block of net/firewall.h for why this is
// C and where the actual filter lives (kernel/rustkern/fwfilter.rs).
#include "firewall.h"
#include "../fs/fat.h"
#include "../fs/bootlog.h"
#include "../fs/perms.h"
#include "../mm/heap.h"
#include "../serial.h"

// #665 persist-extern-gate: these come from their OWNING headers above, never
// from a hand-written `extern` here. A private extern compiles and links
// against a signature you invented rather than the real one, and void-vs-int
// and a wrong pointer type are both ABI-compatible on x86-64 SysV, so the
// mismatch is silent. This file's first draft declared bootlog_write() and
// perms_sync() by hand and the gate caught both.
extern fat_fs_t g_fat_fs;

// Largest FW_CFG_PATH this will ever produce or accept. The serialised form
// of a maximal config (4 comment lines + on + pin + pout + 12 rules) is well
// under 512 bytes; 1024 leaves room for a hand-annotated file.
#define FW_CFG_MAX 1024

void fw_report(const char *why) {
    fw_config_t c;
    if (fw_get_config_rs(&c) != 0) return;
    kprintf("[FW] %s: %s, in=%s out=%s, %u rule%s\n",
            why,
            c.enabled ? "ENABLED" : "disabled",
            c.pol_in  == FW_ACT_DENY ? "DENY" : "ALLOW",
            c.pol_out == FW_ACT_DENY ? "DENY" : "ALLOW",
            (unsigned)c.rule_count, c.rule_count == 1 ? "" : "s");
    for (unsigned i = 0; i < c.rule_count && i < FW_MAX_RULES; i++) {
        kprintf("[FW]   %s %s %s %u\n",
                c.rules[i].dir == FW_DIR_IN ? "in " : "out",
                c.rules[i].action == FW_ACT_DENY ? "deny " : "allow",
                c.rules[i].proto == FW_PROTO_UDP ? "udp" : "tcp",
                (unsigned)c.rules[i].port);
    }
}

// Read FW_CFG_PATH and install it.
//
// FAIL-SAFE CONTRACT, and every branch of it is here in one place:
//
//   file absent        -> keep the compiled-in policy (fwfilter.rs
//                         DEFAULT_CFG). This is the virgin-image case and it
//                         is NOT "no filtering": it is inbound DENY with
//                         sshd permitted, outbound ALLOW.
//   file unreadable    -> same.
//   file unusable      -> same, and say so loudly. "Unusable" means the parser
//   (parse < 0)           found no valid directive at all, so honouring it
//                         would mean inventing a policy nobody wrote.
//   malformed LINES    -> those lines are IGNORED and COUNTED; the rest of
//   (parse > 0)           the file is installed. The count is reported, so a
//                         typo is visible rather than silently dropping a
//                         rule the user believes is in force.
//   install refused    -> keep the previous policy. fw_install_rs() validates
//                         the whole ruleset and applies all of it or none.
//
// There is no path here that turns the filter OFF. Only an explicit `off` in
// the file, or an explicit SYS_NET_FW set, does that.
int fw_reload(void) {
    if (!g_fat_fs.mounted) {
        kprintf("[FW] no filesystem yet; keeping the compiled-in policy\n");
        return -1;
    }
    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, FW_CFG_PATH, &size);
    if (!data) {
        kprintf("[FW] %s absent; keeping the compiled-in policy\n", FW_CFG_PATH);
        return -1;
    }
    if (size > FW_CFG_MAX) size = FW_CFG_MAX;

    fw_config_t cfg;
    int bad = fw_parse_cfg_rs((const uint8_t *)data, size, &cfg);
    kfree(data);

    if (bad < 0) {
        kprintf("[FW] %s carries no usable directive (rc=%d); keeping the "
                "compiled-in policy. The file is NOT being honoured.\n",
                FW_CFG_PATH, bad);
        bootlog_write("[FW] %s unusable; compiled-in policy retained", FW_CFG_PATH);
        return bad;
    }
    if (fw_install_rs(&cfg) != 0) {
        kprintf("[FW] %s parsed but REFUSED by validation; previous policy "
                "retained\n", FW_CFG_PATH);
        bootlog_write("[FW] %s refused by validation", FW_CFG_PATH);
        return -3;
    }
    if (bad > 0) {
        kprintf("[FW] %s: %d malformed line%s IGNORED - the rules they meant "
                "to express are NOT in force\n",
                FW_CFG_PATH, bad, bad == 1 ? "" : "s");
        bootlog_write("[FW] %s: %d malformed line(s) ignored", FW_CFG_PATH, bad);
    }
    return bad;
}

void fw_boot_load(void) {
    // A filter nobody has watched drop a packet is indistinguishable from one
    // that is not wired up. This drives the real decision function over 20+
    // vectors covering every branch, and restores the live policy afterwards.
    int f = fw_selftest_rs();
    kprintf("[FW] selftest: %s (%d failure%s)\n",
            f == 0 ? "PASS" : "FAIL", f, f == 1 ? "" : "s");
    bootlog_write("[FW] selftest %s (%d failures)", f == 0 ? "PASS" : "FAIL", f);

    int r = fw_reload();
    fw_report(r >= 0 ? "policy loaded from " FW_CFG_PATH
                     : "compiled-in fail-safe policy in force");
}

int fw_persist(const fw_config_t *cfg) {
    if (!cfg) return -1;
    uint8_t buf[FW_CFG_MAX];
    int n = fw_format_cfg_rs(cfg, buf, sizeof(buf));
    if (n <= 0) return -1;   // nothing partial is ever written
    if (!g_fat_fs.mounted) return -1;
    if (fat_write_file(&g_fat_fs, FW_CFG_PATH, buf, (uint32_t)n) != 0) return -1;
    // Root-owned 0644, the same defence /CONFIG/SETUPDONE gets: every reader
    // needs it, and with an explicit entry no Ring-3 process can truncate the
    // machine's firewall policy back to "unconfigured" through sys_open().
    perms_set(FW_CFG_PATH, 0, 0, 0644);
    if (perms_sync() != 0)
        kprintf("[FW] perms sync failed after writing %s\n", FW_CFG_PATH);
    return 0;
}
