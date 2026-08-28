// panic.c - #418 on-fault persistent panic log + late-boot stage breadcrumbs.
// See panic.h for the full design rationale (why a pre-allocated fixed-size
// file + raw single-sector overwrite, instead of fat_write_file_inner()'s
// delete+recreate scheme).
#include "panic.h"
#include "bootlog.h"   // #748: persist the heartbeat ring on panic (owning header, NOT a private extern)
#include "../serial.h"
#include "../string.h"
#include "../version.h"
#include <stdarg.h>

#define SLOT_SIZE       512   // exactly one sector on essentially all FAT media
// #670 (measured, NOT inferred): on the shipping two-partition ext2-root
// golden these slots were NEVER ARMED, so /PANIC.TXT and /STAGE.TXT have not
// been written at all since the #99 root cutover. Three real boot logs on the
// build host - <workspace> (build 1017, today), <workspace> and
// <workspace> - all show, immediately after
// "[MAIN] #99: ext2 is now the ROOT filesystem":
//     [PANIC] WARNING: could not resolve /PANIC.TXT first cluster
//     [PANIC] WARNING: could not resolve /STAGE.TXT first cluster
// while the older FAT-root log <workspace> shows "[PANIC] armed:".
//
// Cause: fixed_slot_init() uses TWO fat-layer calls that no longer agree.
// fat_write_file() redirects a normal "/" path to the ext2 volume
// (fat_path_on_ext2 in fs/fat.c), but fat_open() has NO such redirect - it only
// ever sees the FAT ESP. So the pre-allocation landed on ext2 and the
// first-cluster lookup then searched the ESP, found nothing, and left the slot
// disarmed. panic_log_write() and stage_flush() have been silent no-ops since.
//
// Fix: put the slots where the scheme actually works. fat_path_on_ext2()
// explicitly excludes /boot and /EFI (the firmware must boot from the ESP), so
// a /boot/-prefixed path takes the FAT path in BOTH calls and the raw
// single-sector overwrite is valid again. That also keeps the #418 property
// that matters on the real iMac: the record lands on the FAT ESP, which any
// machine can read when the stick is pulled.
//
// This is the ONE behaviour change in this file that a boot must confirm:
// the serial log must now say "[PANIC] armed: /boot/PANIC.TXT ..." instead of
// the WARNING above. Reverting these two lines restores the previous (broken)
// paths without touching anything else.
#define PANIC_PATH      "/boot/PANIC.TXT"
#define STAGE_PATH      "/boot/STAGE.TXT"
#define STAGE_RING_DEPTH 6
#define STAGE_DETAIL_MAX 24

// ---------------------------------------------------------------------------
// #670 KERNEL ADDRESS DISCLOSURE
// ---------------------------------------------------------------------------
// The panic record is PERSISTENT and, unlike the serial console, it is an
// ORDINARY FILE that a Ring-3 process can open. It must therefore not carry a
// raw kernel .text pointer. It still has to carry enough to diagnose a fault
// from a stick pulled out of a machine with no serial cable, which is the
// entire point of #418, so the fix is a RELATIVE address, not a deleted field:
// the record now names the image and an offset INTO that image, which is what
// addr2line wants anyway.
//
// The raw absolutes are UNCHANGED on the serial console (cpu/idt.c and
// kpanic() below still kprintf them). Serial is a physically-privileged
// channel that Ring 3 cannot read, so a developer with a cable loses nothing.
//
// HONEST LIMIT, do not overstate this. This kernel is linked NON-PIE at a
// FIXED base (linker.ld: KERNEL_PHYS_BASE = 0x400000) and there is no KASLR,
// so anyone who knows the build can add the base back. Note also that this
// kernel is identity-mapped LOW and never runs on the upper half, so a
// 0xFFFF8... test would be wrong here; the bounds below come from the linker
// script, not from an assumed layout. What this change actually buys is
// (a) no ready-made, copy-pasteable live pointer in a world-persisted file,
// and (b) a record that stays non-disclosing on the day a slide does land.
// The control that CLOSES the disclosure today is the perms.db entry added in
// fs/perms.c (0600 root); this is defense in depth behind it.
//
// NOT fixed here, deliberately: CR3 is still recorded raw. It is a PHYSICAL
// page-table frame address, so it is a disclosure of the same family. It is
// also the only field that answers "which address space" for someone reading
// the file off a stick with no serial log to correlate against, and unlike RIP
// there is no representation that keeps that use and drops the value. It is
// protected by the perms entry alone.
// ---------------------------------------------------------------------------

// Bounds of the linked kernel image (linker.ld defines both).
extern char __text_start[];
extern char __kernel_end[];

// User images link at this base (userland/user.ld: ". = 0x80000000").
#define USER_IMAGE_BASE   0x80000000UL

// Render ONE faulting address as an image-relative offset. Never emits an
// absolute. `out` must be at least 24 bytes.
//   K+0x<off>  inside the linked kernel image;
//              addr2line -e kernel.dbg.elf $((0x400000 + off))
//   U+0x<off>  user-mode fault at/above the user image base; addr2line against
//              the faulting app's ELF at (0x80000000 + off)
//   NULL       a null dereference, the most common single case, named outright
//   ?+0x<off>  anything else (kernel heap, MMIO, a wild or attacker-supplied
//              value): still a delta from the kernel base, so no absolute is
//              written, and the magnitude still says which region it hit
static void fmt_reladdr(char *out, uint32_t out_size, uint64_t addr, int user_mode) {
    uint64_t kbase = (uint64_t)(unsigned long)__text_start;
    uint64_t kend  = (uint64_t)(unsigned long)__kernel_end;

    if (addr == 0) {
        snprintf(out, out_size, "NULL");
    } else if (addr >= kbase && addr < kend) {
        snprintf(out, out_size, "K+0x%lx", (unsigned long)(addr - kbase));
    } else if (user_mode && addr >= USER_IMAGE_BASE) {
        snprintf(out, out_size, "U+0x%lx", (unsigned long)(addr - USER_IMAGE_BASE));
    } else {
        snprintf(out, out_size, "?+0x%lx", (unsigned long)(addr - kbase));
    }
}

// A "fixed slot" is a pre-allocated, fixed-size file plus the cached sector
// number of its first (and only) cluster, so later writes are a single raw
// fat_write_sector() call with no lock, no fat_open(), no directory walk.
typedef struct {
    fat_fs_t *fs;
    uint32_t  sector;   // partition-relative; only meaningful when armed
    int       armed;
} fixed_slot_t;

static fixed_slot_t g_panic_slot;
static fixed_slot_t g_stage_slot;

// Pre-allocate (or reuse) a fixed-size file via the ORDINARY locked path.
// Only ever called once, early in boot, never from fault context - the
// non-atomicity of fat_write_file_inner()'s delete+recreate sequence does not
// matter here because nothing depends on THIS specific write surviving an
// interrupted reset; it is the START of the file's life, not a panic record.
// Returns 1 on success (slot armed), 0 on failure.
static int fixed_slot_init(fixed_slot_t *slot, fat_fs_t *fs, const char *path) {
    if (fs->bytes_per_sector != 0 && fs->bytes_per_sector > SLOT_SIZE) {
        // Defensive: this whole scheme assumes one fat_write_sector() call
        // (SLOT_SIZE bytes) covers no more than one physical sector. Every
        // FAT medium this kernel targets uses 512-byte sectors; refuse to arm
        // rather than silently write a partial/misaligned sector on hardware
        // where that assumption is wrong.
        kprintf("[PANIC] %s: sector size %u > %u, breadcrumb/panic log disabled\n",
                path, (unsigned)fs->bytes_per_sector, (unsigned)SLOT_SIZE);
        return 0;
    }

    static const char zero[SLOT_SIZE] = {0};
    if (fat_write_file(fs, path, zero, SLOT_SIZE) != 0) {
        kprintf("[PANIC] WARNING: could not pre-allocate %s\n", path);
        return 0;
    }

    fat_file_t f;
    if (fat_open(fs, path, &f) != 0 || f.first_cluster < 2) {
        kprintf("[PANIC] WARNING: could not resolve %s first cluster\n", path);
        return 0;
    }
    slot->sector = fat_cluster_to_sector(fs, f.first_cluster);
    fat_close(&f);
    slot->fs = fs;
    slot->armed = 1;
    kprintf("[PANIC] armed: %s pre-allocated (%u bytes)\n", path, (unsigned)SLOT_SIZE);
    return 1;
}

// Raw, unlocked, single-sector overwrite. Safe to call from exception
// context: no fat_lock(), no fat_open(), no heap allocation.
static void fixed_slot_write(fixed_slot_t *slot, const void *data, uint32_t size) {
    if (!slot->armed) return;
    uint8_t sector[SLOT_SIZE];
    memset(sector, 0, sizeof(sector));
    uint32_t n = size;
    if (n > SLOT_SIZE) n = SLOT_SIZE;
    memcpy(sector, data, n);
    // #693: we are already panicking, so there is no caller to inform and no
    // retry that could help. Serial is the only channel that still works.
    if (fat_write_sector(slot->fs, slot->sector, sector) != 0)
        kprintf("[PANIC] FAILED to write the panic record to disk; the reason "
                "for this panic will NOT survive the reboot\n");
}

// printf-style append into a caller-owned buffer, tracking position - same
// pattern as devlog.c's dl_line(), just parameterized on the buffer instead
// of a module-global one.
static void buf_appendf(char *buf, uint32_t bufsize, uint32_t *pos, const char *fmt, ...) {
    if (*pos >= bufsize) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, bufsize - *pos, fmt, ap);
    va_end(ap);
    if (n > 0) *pos += (uint32_t)n;
}

// ---------------------------------------------------------------------------
// Stage breadcrumbs
// ---------------------------------------------------------------------------

typedef struct {
    boot_stage_t stage;
    char         detail[STAGE_DETAIL_MAX];
} stage_entry_t;

static stage_entry_t g_stage_ring[STAGE_RING_DEPTH];
static int g_stage_count = 0;   // entries filled, capped at STAGE_RING_DEPTH
static int g_stage_next  = 0;   // next write slot (circular)

static const char *stage_name(boot_stage_t s) {
    switch (s) {
        case STAGE_FS_MOUNTED:         return "FS_MOUNTED";
        case STAGE_DEVLOG_WRITTEN:     return "DEVLOG_WRITTEN";
        case STAGE_NIC_INIT_DONE:      return "NIC_INIT_DONE";
        case STAGE_LOGIN_START:        return "LOGIN_START";
        case STAGE_LOGIN_DONE:         return "LOGIN_DONE";
        case STAGE_SVC_REGISTRY_BUILT: return "SVC_REGISTRY_BUILT";
        case STAGE_COMPOSITOR_LAUNCH:  return "COMPOSITOR_LAUNCH";
        case STAGE_COMPOSITOR_UP:      return "COMPOSITOR_UP";
        case STAGE_SVC_SPAWN:          return "SVC_SPAWN";
        case STAGE_DESKTOP_READY:      return "DESKTOP_READY";
        default:                       return "NONE";
    }
}

// Re-render the whole ring into /STAGE.TXT's one sector: oldest-first, one
// line per stage, so the LAST line is always the most recent stage reached.
static void stage_flush(void) {
    if (!g_stage_slot.armed) return;
    char buf[SLOT_SIZE];
    uint32_t pos = 0;
    int start = (g_stage_count < STAGE_RING_DEPTH) ? 0 : g_stage_next;
    for (int i = 0; i < g_stage_count; i++) {
        int idx = (start + i) % STAGE_RING_DEPTH;
        stage_entry_t *e = &g_stage_ring[idx];
        if (e->detail[0]) {
            buf_appendf(buf, sizeof(buf), &pos, "%s %s\n", stage_name(e->stage), e->detail);
        } else {
            buf_appendf(buf, sizeof(buf), &pos, "%s\n", stage_name(e->stage));
        }
    }
    fixed_slot_write(&g_stage_slot, buf, pos);
}

void stage_set(boot_stage_t stage, const char *detail) {
    stage_entry_t *e = &g_stage_ring[g_stage_next];
    e->stage = stage;
    e->detail[0] = 0;
    if (detail) {
        int i = 0;
        for (; i < STAGE_DETAIL_MAX - 1 && detail[i]; i++) e->detail[i] = detail[i];
        e->detail[i] = 0;
    }
    g_stage_next = (g_stage_next + 1) % STAGE_RING_DEPTH;
    if (g_stage_count < STAGE_RING_DEPTH) g_stage_count++;

    kprintf("[STAGE] %s%s%s\n", stage_name(stage), detail ? " " : "", detail ? detail : "");
    stage_flush();
}

// ---------------------------------------------------------------------------
// Init + on-fault write
// ---------------------------------------------------------------------------

void panic_log_init(fat_fs_t *fs) {
    if (!fs) return;
    fixed_slot_init(&g_panic_slot, fs, PANIC_PATH);
    if (fixed_slot_init(&g_stage_slot, fs, STAGE_PATH)) {
        // Flush whatever stage_set() calls happened before the FAT root
        // existed (mirrors bootlog_arm()'s initial flush of pre-mount
        // content).
        stage_flush();
    }
}

void panic_log_write(uint64_t rip, uint64_t cr2, uint64_t error_code,
                      uint64_t cr3, const char *exception_name, int user_mode) {
    if (!g_panic_slot.armed) return;

    const stage_entry_t *last = NULL;
    if (g_stage_count > 0) {
        int idx = (g_stage_next - 1 + STAGE_RING_DEPTH) % STAGE_RING_DEPTH;
        last = &g_stage_ring[idx];
    }

    char buf[SLOT_SIZE];
    uint32_t pos = 0;
    buf_appendf(buf, sizeof(buf), &pos, "MayteraOS PANIC v%s build %u (%s)\n",
                MAYTERA_VERSION_STRING, (unsigned)MAYTERA_BUILD_NUMBER, MAYTERA_BUILD_DATE);
    buf_appendf(buf, sizeof(buf), &pos, "mode=%s exception=%s\n",
                user_mode ? "USER" : "KERNEL", exception_name ? exception_name : "?");
    // #670: RIP and CR2 are recorded IMAGE-RELATIVE (see fmt_reladdr above).
    // The legend line deliberately does NOT print the base value: a developer
    // reads it from linker.ld, a Ring-3 reader is not handed it.
    char rip_s[24], cr2_s[24];
    fmt_reladdr(rip_s, sizeof(rip_s), rip, user_mode);
    fmt_reladdr(cr2_s, sizeof(cr2_s), cr2, user_mode);
    buf_appendf(buf, sizeof(buf), &pos, "RIP=%s CR2=%s ERR=0x%lx CR3=0x%lx\n",
                rip_s, cr2_s,
                (unsigned long)error_code, (unsigned long)cr3);
    buf_appendf(buf, sizeof(buf), &pos,
                "addr=image-relative (K=kernel, U=user; addr2line base+off)\n");
    buf_appendf(buf, sizeof(buf), &pos, "last_stage=%s%s%s\n",
                last ? stage_name(last->stage) : "NONE",
                (last && last->detail[0]) ? " " : "",
                (last && last->detail[0]) ? last->detail : "");

    fixed_slot_write(&g_panic_slot, buf, pos);

    // #748: the /HEARTBEAT.TXT ring is now RAM-resident between its 30-minute
    // flushes, so a panic would otherwise discard up to 30 minutes of the beats
    // leading up to it - which is exactly the window a reader wants. Persist it
    // here, AFTER the panic record itself is safely down, so this larger and
    // more failure-prone write can never cost us the record that matters most.
    // Its return is deliberately consumed and reported rather than ignored: a
    // deferred write is the one write whose failure nobody would otherwise see.
    {
        int hb = bootlog_heartbeat_flush();
        if (hb != 0)
            kprintf("[PANIC] heartbeat ring could NOT be persisted (rc=%d); the "
                    "beats before this panic exist only on serial\n", hb);
    }
}

// ---------------------------------------------------------------------------
// #480 Canonical kernel panic primitive (see panic.h for the rationale).
// ---------------------------------------------------------------------------

// The one shared terminal halt tail. Release the whole-kernel BKL (a dead CPU
// must not keep every other CPU spinning for a lock it will never release -
// same reasoning as cpu/idt.c's kernel-fault branch, now centralized here),
// then permanently cli+hlt. This is a terminal idle halt, NOT a busy-wait
// (#426): each iteration parks the CPU in hlt until the next (masked) event.
void kpanic_halt(void) {
    __asm__ volatile("cli");
    // #745 (task #70): FLUSH THE CONSOLE RING FIRST. kprintf is asynchronous
    // once the drain thread is up, so a panic banner printed a microsecond ago
    // may still be sitting in the ring with no thread left to write it. This is
    // the ONE tail every panic path reaches - kpanic() below AND cpu/idt.c's
    // kernel-fault branch - so flushing here covers both without either needing
    // to know the console has a buffer. It takes no locks (the console lock's
    // holder may be the CPU that just died) and it latches the console back to
    // synchronous, so everything printed after this point goes straight out.
    // An unflushed buffer at a panic would be a worse bug than the latency this
    // ticket set out to remove.
    console_panic_flush();
    // Declared locally (matches cpu/smp.h) to avoid an fs -> cpu include edge.
    extern uint32_t bkl_release_all(void);
    bkl_release_all();
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void kpanic(const char *fmt, ...) {
    // Stop the world first: no preemption / no interrupt can perturb the
    // banner or the /PANIC.TXT record we are about to write.
    __asm__ volatile("cli");

    // #745 (task #70): and stop the console buffering, first thing. The two
    // kprintf() banners below would otherwise be queued into the console ring
    // by a CPU that is about to halt, and the wake they raise would reach the
    // scheduler from a context whose invariants are exactly what just failed.
    // Flushing here makes every print from this point synchronous and in order.
    // kpanic_halt() flushes again on the way out, for the fault paths in
    // cpu/idt.c that reach it without coming through here.
    console_panic_flush();

    // Caller of kpanic(), for context in both the banner and the record. This
    // is the diagnosis anchor for a logic-abort panic (no CPU fault frame).
    void *caller = __builtin_return_address(0);

    // Reuse the shared vsnprintf/va_list - do NOT hand-roll formatting.
    char msg[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    // Loud banner on serial (+ dual output if enabled), the same "[PANIC] "
    // prefix the existing fault path uses so log scrapers see one shape.
    kprintf("\n[PANIC] %s\n", msg);
    kprintf("[PANIC] caller=%p CR3=0x%lx  Halting CPU.\n",
            caller, (unsigned long)read_cr3());

    // Persist via the existing raw-sector writer. rip=caller carries the
    // context; cr2/error_code are 0 (a kpanic is a logic abort, not a CPU
    // fault, so there is no faulting address or hardware error code). No-ops
    // safely if the panic slot was never armed (early boot).
    panic_log_write((uint64_t)(uintptr_t)caller, 0, 0, read_cr3(), "KPANIC", 0);

    kpanic_halt();
}
