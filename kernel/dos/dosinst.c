// dosinst.c - per-DOS-instance registry. See dosinst.h.
//
// WHY C, NOT RUST (CLAUDE.md Rust-first policy): this table is read on the DOS
// interpreter's hot path from dosexec.c, a C+asm subsystem, and it is keyed off
// dos_task_t*, a file-local C type. Exposing that layout across a #[repr(C)] FFI
// for a bounded, behavior-neutral first step would add risk with no benefit. The
// DOS interpreter's C+asm entanglement is the stated reason.
//
// CONCURRENCY: guarded by the canonical spinlock primitive
// (spinlock_acquire_irqsave / spinlock_release_irqrestore); NO hand-rolled wait
// or poll (#426 / concurrency-lint). The table scan is a bounded fixed loop over
// DOS_MAX_INSTANCES, not a busy-wait on a condition.
#include "dosinst.h"
#include "../sync/spinlock.h"
#include "../serial.h"

// Headroom for the future multi-instance switch; today at most slot 0 is live.
#define DOS_MAX_INSTANCES 8

typedef struct { uint32_t pid; void *task; } dos_inst_ent_t;
static dos_inst_ent_t g_dos_inst[DOS_MAX_INSTANCES];
static spinlock_t     g_dos_inst_lock = SPINLOCK_INIT;

// (dosconc4) The focus-ownership token for the shared host singletons. A single
// aligned 32-bit volatile: reads/writes are atomic on x86-64, so the input/audio
// hot path reads it WITHOUT taking g_dos_inst_lock (owner brief: no new spin on
// the input/audio path). Written only at the rare DOS focus edge and at
// unregister. 0 = no DOS owner.
static volatile uint32_t g_dos_inst_focus_owner = 0;

void dos_inst_register(uint32_t pid, void *task) {
    if (!pid || !task) return;
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_inst_lock);
    int free_slot = -1;
    for (int i = 0; i < DOS_MAX_INSTANCES; i++) {
        if (g_dos_inst[i].pid == pid) {
            g_dos_inst[i].task = task;
            spinlock_release_irqrestore(&g_dos_inst_lock, fl);
            return;
        }
        if (free_slot < 0 && g_dos_inst[i].pid == 0) free_slot = i;
    }
    if (free_slot >= 0) {
        g_dos_inst[free_slot].pid = pid;
        g_dos_inst[free_slot].task = task;
    } else {
        kprintf("[DOS-INST] register: table full (pid=%u)\n", pid);
    }
    spinlock_release_irqrestore(&g_dos_inst_lock, fl);
}

void dos_inst_unregister(uint32_t pid) {
    if (!pid) return;
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_inst_lock);
    for (int i = 0; i < DOS_MAX_INSTANCES; i++)
        if (g_dos_inst[i].pid == pid) {
            g_dos_inst[i].pid = 0;
            g_dos_inst[i].task = 0;
        }
    // (dosconc4) A guest that exits while owning the host singletons must release
    // them, so a stale pid cannot keep the keyboard tap / FM sink hostage.
    if (g_dos_inst_focus_owner == pid) g_dos_inst_focus_owner = 0;
    spinlock_release_irqrestore(&g_dos_inst_lock, fl);
}

void *dos_inst_lookup(uint32_t pid) {
    if (!pid) return 0;
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_inst_lock);
    void *r = 0;
    for (int i = 0; i < DOS_MAX_INSTANCES; i++)
        if (g_dos_inst[i].pid == pid) { r = g_dos_inst[i].task; break; }
    spinlock_release_irqrestore(&g_dos_inst_lock, fl);
    return r;
}

int dos_inst_count(void) {
    uint64_t fl = spinlock_acquire_irqsave(&g_dos_inst_lock);
    int n = 0;
    for (int i = 0; i < DOS_MAX_INSTANCES; i++) if (g_dos_inst[i].pid) n++;
    spinlock_release_irqrestore(&g_dos_inst_lock, fl);
    return n;
}

// (dosconc4) Focus-ownership accessors. Lockless by construction (single aligned
// 32-bit volatile); see the declaration comment in dosinst.h.
void dos_inst_set_focus_owner(uint32_t pid) {
    if (pid) g_dos_inst_focus_owner = pid;
}
void dos_inst_clear_focus_owner(uint32_t pid) {
    if (pid && g_dos_inst_focus_owner == pid) g_dos_inst_focus_owner = 0;
}
uint32_t dos_inst_focus_owner(void) {
    return g_dos_inst_focus_owner;
}
int dos_inst_may_drive_host(uint32_t pid) {
    uint32_t o = g_dos_inst_focus_owner;
    return (o == 0) || (o == pid);
}
