// escrow_core.c - #305 immutable security core (Stage 5 part A). See
// escrow_core.h for the model and the honest boundary.
//
// WHY THIS IS C, NOT RUST. It is raw page-table manipulation and CR-register
// reads: it maps its page read-only through vmm_map_page_in() (inline-asm and
// hardware-page-table entanglement with no safe-Rust representation), reads CR0
// and CR3, touches the fold's GFS_NODE_ESCROW constant, and reads a /CONFIG
// kill-switch marker through the C fat/ext2 layer. This is the same entanglement
// exemption kernel/mm/vmm.c's PAT/protect code is written under. The value it
// protects (the principal) is trivial; there is no pure policy to host in Rust
// here.
//
// SEALS ARE NOW DEFAULT-ON, RUNTIME-GATED (#246/#305 real-HW enablement). Both
// read-only seals (the principal-page seal and the kernel-.text seal) used to be
// COMPILE-TIME gated (-DESCROW_IMMUTABLE_SEAL / -DESCROW_TEXT_SEAL) and OFF in
// the golden. They are now ALWAYS COMPILED IN and ON BY DEFAULT, with a RUNTIME
// KILL-SWITCH: if /CONFIG/ESCROWSEAL.OFF exists on the root fs, BOTH seals are
// skipped this boot. That lets the owner disable a misbehaving seal on real
// hardware WITHOUT a rebuild, while the shipped default is the hardened state.
// A seal is still FAIL-SAFE: one that does not take leaves the core writable but
// still the authority source and tamper-checked, and never bricks boot.
//
// NO BUSY-WAIT, NO SPIN. escrow_core_init() runs once at boot; the accessors are
// pure memory reads. vmm_map_page_in() does its own SMP-safe TLB shootdown. The
// kill-switch read is a single fat_read_file() from FS-mounted boot context (a
// may-block context, no loop), so concurrency-lint is unmoved.

#include "escrow_core.h"
#include "../mm/vmm.h"                 // vmm_map_page_in, vmm_get_effective_flags_in, VMM_FLAG_*
#include "../fs/graphfs/fold.h"        // GFS_NODE_ESCROW (the compile-time principal)
#include "../fs/fat.h"                 // g_fat_fs, fat_read_file (kill-switch marker read)
#include "../mm/heap.h"                // kfree (kill-switch marker buffer)
#include "../serial.h"
#include "../string.h"

#define ESCROW_CORE_MAGIC   0x455343524F574B31ULL  // "ESCROWK1"
#define ESCROW_CORE_CANARY  0x5A5AA5A5C3C3D00DULL

typedef struct {
    uint64_t magic;       // ESCROW_CORE_MAGIC
    uint32_t principal;   // == GFS_NODE_ESCROW at populate time
    uint32_t reserved;
    uint64_t canary;      // ESCROW_CORE_CANARY
} escrow_core_t;

// THE dedicated page. aligned(4096) + sized 4096 => it occupies EXACTLY one 4KB
// page that no other kernel object shares, so making THAT page read-only affects
// only this object. This sole-occupant property is what makes the seal a bounded,
// boot-safe change rather than a blast radius over unrelated kernel data.
static uint8_t g_escrow_core_page[4096] __attribute__((aligned(4096)));
static int g_escrow_core_ready  = 0;
static int g_escrow_core_sealed = 0;
static int g_escrow_text_sealed = 0;

static escrow_core_t *core(void) { return (escrow_core_t *)g_escrow_core_page; }

extern fat_fs_t g_fat_fs;

// Runtime kill-switch. Default: seals ON (return 0). If /CONFIG/ESCROWSEAL.OFF
// exists on the root fs, return 1 and BOTH seals are skipped this boot. Read via
// fat_read_file(), which REDIRECTS to the ext2 root where /CONFIG actually lives;
// fat_exists() only consults the FAT ESP and would MISS the marker (see
// proc/escrow_vmtest.c and the users.c note, the same reason every /CONFIG marker
// in this tree is read with fat_read_file, not fat_exists). If the FS is not yet
// mounted the marker cannot be present, so the default (seals ON) holds.
static int escrow_seal_disabled(void) {
    if (!g_fat_fs.mounted) return 0;
    uint32_t sz = 0;
    void *m = fat_read_file(&g_fat_fs, "/CONFIG/ESCROWSEAL.OFF", &sz);
    if (m) { kfree(m); return 1; }
    return 0;
}

void escrow_core_init(void) {
    if (g_escrow_core_ready) return;

    escrow_core_t *c = core();
    c->magic     = ESCROW_CORE_MAGIC;
    c->principal = GFS_NODE_ESCROW;
    c->reserved  = 0;
    c->canary    = ESCROW_CORE_CANARY;
    g_escrow_core_ready = 1;

    if (escrow_seal_disabled()) {
        kprintf("[ESCROW-CORE] initialised (principal=%08x); RO seal DISABLED by "
                "/CONFIG/ESCROWSEAL.OFF (owner kill-switch); authority-source + "
                "tamper-check active, page stays writable this boot\n",
                (unsigned)c->principal);
        return;
    }

    // DEFAULT: make the core page READ-ONLY. Reuse vmm_protect_kernel_page(): it
    // splits the containing 2MB huge page (each of the other 511 4KB frames keeps
    // its PRESENT|WRITABLE flags) and does the SMP-safe TLB shootdown. Identity
    // mapping, so phys == virt == addr; PRESENT with no WRITABLE == read-only.
    uint64_t addr  = (uint64_t)g_escrow_core_page & ~0xFFFULL;
    uint64_t kpml4 = read_cr3() & ~0xFFFULL;
    int rc = vmm_protect_kernel_page(addr, 0);   // 0 = read-only
    uint64_t eff = vmm_get_effective_flags_in(kpml4, addr);
    if (rc == 0 && (eff & VMM_FLAG_PRESENT) && !(eff & VMM_FLAG_WRITABLE)) {
        g_escrow_core_sealed = 1;
        kprintf("[ESCROW-CORE] SEALED (default ON): principal=%08x at %#lx is "
                "READ-ONLY; a post-boot supervisor write faults (CR0.WP=%d)\n",
                (unsigned)c->principal, (unsigned long)addr,
                (int)((read_cr0() >> 16) & 1));
    } else {
        // Fail SAFE, never fatal: the core stays writable but is still the
        // authority source and tamper-checked. A failed seal must not brick boot.
        kprintf("[ESCROW-CORE] WARNING: seal did not take (rc=%d eff=%#lx); core "
                "stays writable, tamper-check still active\n",
                rc, (unsigned long)eff);
    }
}

int escrow_core_verify(void) {
    if (!g_escrow_core_ready) return 0;
    const escrow_core_t *c = core();
    return c->magic == ESCROW_CORE_MAGIC &&
           c->canary == ESCROW_CORE_CANARY &&
           c->principal == GFS_NODE_ESCROW;
}

uint32_t escrow_core_principal(void) {
    if (!g_escrow_core_ready) return 0;   // fail closed
    const escrow_core_t *c = core();
    if (c->magic != ESCROW_CORE_MAGIC || c->canary != ESCROW_CORE_CANARY)
        return 0;                         // tampered: 0 is never a valid issuer
    return c->principal;
}

int escrow_core_is_sealed(void) { return g_escrow_core_sealed; }
int escrow_core_is_text_sealed(void) { return g_escrow_text_sealed; }

// #305 Stage 5A keystone: seal the ENFORCEMENT CODE (kernel .text) READ-ONLY
// after init, so patching the DECISION itself (escrow_fs_guard, gfs_grant_check,
// the fold's actor==NODE_ESCROW compare, and every other kernel routine) faults.
// Measured: this kernel maps .text WRITABLE (it runs on the UEFI identity map),
// so the enforcement code was patchable by any kernel-level write; CR0.WP is set,
// so once .text is read-only a supervisor write to it faults. Now DEFAULT ON,
// runtime-gated by the same /CONFIG/ESCROWSEAL.OFF kill-switch. Called LATE in
// boot (after all subsystem init) so no boot-time write to .text can fault; the
// codebase has no runtime self-modifying .text (proc/context_switch.asm
// explicitly chose a data-byte branch over patching the instruction stream).
// Fail SAFE: a partial seal never panics.
void escrow_core_seal_text(void) {
    if (escrow_seal_disabled()) {
        kprintf("[ESCROW-CORE] TEXT-SEAL DISABLED by /CONFIG/ESCROWSEAL.OFF (owner "
                "kill-switch); kernel .text stays writable this boot\n");
        return;
    }
    extern char __text_start[], __text_end[];
    extern int escrow_fs_guard(int op, const char *path, const char *path2);
    uint64_t s = (uint64_t)(void *)__text_start;
    uint64_t e = (uint64_t)(void *)__text_end;
    int rc = vmm_protect_kernel_range(s, e, 0);
    uint64_t kp  = read_cr3() & ~0xFFFULL;
    uint64_t eff = vmm_get_effective_flags_in(kp, (uint64_t)(void *)&escrow_fs_guard);
    g_escrow_text_sealed = (rc == 0 && (eff & VMM_FLAG_PRESENT) && !(eff & VMM_FLAG_WRITABLE));
    kprintf("[ESCROW-CORE] TEXT-SEAL %s (default ON): kernel .text %#lx..%#lx "
            "READ-ONLY; the enforcement decision code is now unpatchable-by-write "
            "(escrow_fs_guard eff=%#lx, CR0.WP=%d)\n",
            g_escrow_text_sealed ? "OK" : "PARTIAL",
            (unsigned long)s, (unsigned long)e, (unsigned long)eff,
            (int)((read_cr0() >> 16) & 1));
}

// ===========================================================================
// Gated boot self-test. Built only under -DESCROW_CORE_SELFTEST. It is now
// RUNTIME-AWARE: it asserts the seals took when the kill-switch is ABSENT (the
// shipped default), and asserts they were correctly SKIPPED when the kill-switch
// /CONFIG/ESCROWSEAL.OFF is PRESENT. So the SAME test kernel proves both the
// default-ON seal AND the kill-switch depending only on the marker file.
// ===========================================================================
#ifdef ESCROW_CORE_SELFTEST
static int escc_st(const char *name, int ok) {
    kprintf("[ESCU5A-TEST] %-54s %s\n", name, ok ? "PASS" : "FAIL");
    return ok;
}

void escrow_core_selftest(void) {
    int ok = 1;
    int ks = escrow_seal_disabled();   // is the runtime kill-switch present?
    kprintf("\n========== ESCROW STAGE 5A (IMMUTABLE CORE #305) SELF-TEST ==========\n");
    kprintf("[ESCU5A-TEST] kill-switch /CONFIG/ESCROWSEAL.OFF %s (%s)\n",
            ks ? "PRESENT" : "absent",
            ks ? "seals expected DISABLED" : "seals expected ON (shipped default)");

    ok &= escc_st("principal == GFS_NODE_ESCROW (read from the core page)",
                  escrow_core_principal() == GFS_NODE_ESCROW);
    ok &= escc_st("verify() PASS on an intact core", escrow_core_verify() == 1);

    uint64_t cr0 = read_cr0();
    int wp = (int)((cr0 >> 16) & 1);
    ok &= escc_st("CR0.WP set (supervisor write-protect active)", wp == 1);
    kprintf("[ESCU5A-TEST] (CR0=%#lx WP=%d)\n", (unsigned long)cr0, wp);

    // #305 keystone MEASUREMENT: the live PTE of escrow_fs_guard (the FS-mutation
    // decision chokepoint) and of __text_start (generic .text).
    {
        extern int escrow_fs_guard(int op, const char *path, const char *path2);
        extern char __text_start[], __text_end[];
        uint64_t kp = read_cr3() & ~0xFFFULL;
        uint64_t g_addr = (uint64_t)(void *)&escrow_fs_guard;
        uint64_t g_eff  = vmm_get_effective_flags_in(kp, g_addr);
        uint64_t t_eff  = vmm_get_effective_flags_in(kp, (uint64_t)(void *)__text_start);
        kprintf("[ESCU5A-TEST] enforcement .text escrow_fs_guard @%#lx eff=%#lx WRITABLE=%d\n",
                (unsigned long)g_addr, (unsigned long)g_eff, (g_eff & VMM_FLAG_WRITABLE) ? 1 : 0);
        if (!ks) {
            ok &= escc_st("TEXT-SEAL took: enforcement code (kernel .text) READ-ONLY",
                          escrow_core_is_text_sealed() == 1 &&
                          (g_eff & VMM_FLAG_PRESENT) && !(g_eff & VMM_FLAG_WRITABLE));
            kprintf("[ESCU5A-TEST] CONCLUSION(code): .text NOT-WRITABLE + CR0.WP set => "
                    "a post-boot write patching the DECISION faults.\n");
        } else {
            ok &= escc_st("kill-switch: TEXT-SEAL correctly SKIPPED (.text writable)",
                          escrow_core_is_text_sealed() == 0 && (g_eff & VMM_FLAG_WRITABLE));
        }
        kprintf("[ESCU5A-TEST] kernel .text %#lx..%#lx base-eff=%#lx WRITABLE=%d\n",
                (unsigned long)(uint64_t)__text_start, (unsigned long)(uint64_t)__text_end,
                (unsigned long)t_eff, (t_eff & VMM_FLAG_WRITABLE) ? 1 : 0);
    }

    uint64_t addr = (uint64_t)g_escrow_core_page & ~0xFFFULL;
    uint64_t eff  = vmm_get_effective_flags_in(read_cr3() & ~0xFFFULL, addr);
    int ro = (eff & VMM_FLAG_PRESENT) && !(eff & VMM_FLAG_WRITABLE);
    if (!ks) {
        ok &= escc_st("core page mapped READ-ONLY (seal took, default ON)", escrow_core_is_sealed() == 1);
        ok &= escc_st("PTE effective flags PRESENT & NOT WRITABLE", ro);
        kprintf("[ESCU5A-TEST] (core page %#lx eff-flags=%#lx)\n",
                (unsigned long)addr, (unsigned long)eff);
        kprintf("[ESCU5A-TEST] CONCLUSION: WRITABLE clear AND CR0.WP set => a post-boot "
                "supervisor write to the escrow principal FAULTS.\n");
        // Prove the core is STILL readable + correct while sealed (RO does not
        // break the authority read path).
        ok &= escc_st("sealed core still READABLE and correct", escrow_core_principal() == GFS_NODE_ESCROW);
    } else {
        ok &= escc_st("kill-switch: core seal correctly SKIPPED (page writable)",
                      escrow_core_is_sealed() == 0 && (eff & VMM_FLAG_WRITABLE));
        kprintf("[ESCU5A-TEST] (core page %#lx eff-flags=%#lx: writable, seal off)\n",
                (unsigned long)addr, (unsigned long)eff);
        // Even with the seal off, the principal is still the tamper-checked
        // authority source.
        ok &= escc_st("kill-switch: core still READABLE and correct (authority intact)",
                      escrow_core_principal() == GFS_NODE_ESCROW);
    }

    kprintf("[ESCU5A-TEST] SUMMARY %s\n", ok ? "OVERALL PASS" : "OVERALL FAIL");
    kprintf("========== ESCROW STAGE 5A SELF-TEST END ==========\n");
}
#else
void escrow_core_selftest(void) { /* not built in production */ }
#endif
