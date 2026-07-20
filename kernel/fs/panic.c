// panic.c - #418 on-fault persistent panic log + late-boot stage breadcrumbs.
// See panic.h for the full design rationale (why a pre-allocated fixed-size
// file + raw single-sector overwrite, instead of fat_write_file_inner()'s
// delete+recreate scheme).
#include "panic.h"
#include "../serial.h"
#include "../string.h"
#include "../version.h"
#include <stdarg.h>

#define SLOT_SIZE       512   // exactly one sector on essentially all FAT media
#define PANIC_PATH      "/PANIC.TXT"
#define STAGE_PATH      "/STAGE.TXT"
#define STAGE_RING_DEPTH 6
#define STAGE_DETAIL_MAX 24

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
    fat_write_sector(slot->fs, slot->sector, sector);
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
    buf_appendf(buf, sizeof(buf), &pos, "RIP=0x%lx CR2=0x%lx ERR=0x%lx CR3=0x%lx\n",
                (unsigned long)rip, (unsigned long)cr2,
                (unsigned long)error_code, (unsigned long)cr3);
    buf_appendf(buf, sizeof(buf), &pos, "last_stage=%s%s%s\n",
                last ? stage_name(last->stage) : "NONE",
                (last && last->detail[0]) ? " " : "",
                (last && last->detail[0]) ? last->detail : "");

    fixed_slot_write(&g_panic_slot, buf, pos);
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
