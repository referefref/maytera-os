// syscall.c - System call implementation for MayteraOS
#include "syscall.h"
#include "../dos/diskimg.h"   // #739 SYS_DISKIMG
#include "../fs/graphfs/journal.h"   // #711 SYS_GFS_VERIFY
#include "../fs/graphfs/fold.h"      // #711 slice 2 SYS_GFS_QUERY
#include "procinfo.h"   // #487: Ring-3 process introspection backends
#include "syscall_argtab.h"  // #503: central pointer-arg validation
#ifdef SECTEST_SYSCALL
extern int64_t validate_selftest(void *ubuf, uint64_t ubuf_len);
#endif
#include "../gui/image.h"

int64_t sys_decode_image(const void *, uint32_t, uint32_t, void *, uint32_t, int *);
int64_t sys_win_draw_image(int, int, int, int, int, uint32_t *);
#include "process.h"
#include "../net/http_progress.h"   // #25: real fetch progress
#include "../sync/waitq.h"   // #453: wait-queue for win_get_event blocking
#include "../drivers/audio_pcm.h"  // Ring-3 PCM push (Ring-0 media exit, phase 1)
#include "../security/validate.h" // #500: spawn_impl argv two-level deref + SYS_IOCTL boundary
#include "../security/aiguard.h"  // #745: LLM prompt-injection screen (nova.c + aiguard.rs)
#include "../version.h"
#include "../serial.h"
#include "../string.h"
#include "../cpu/gdt.h"
#include "../cpu/isr.h"
#include "../mm/vmm.h"
#include "../mm/heap.h"
#include "../mm/demand.h"   // #510/#511: mm_prefault_range() for sys_read
#include "../sync/spinlock.h"
#include "../fs/fat.h"
#include "../fs/ext2.h"
#include "../fs/perms.h"
#include "../fs/guestfs.h"   // #708: DOS/Win16 guest fs gate
#include "../fs/bootlog.h"
#include "users.h"
#include "elevate.h"   // #745 elevation syscall bodies
#include "fetchown.h"  // #745 (task #36): async HTTP job slot ownership (rustkern/fetchown.rs)
#include "pwpolicy.h"
#include "../gui/window.h"
#include "../gui/ttf.h"
#include "../gui/syslog.h"
#include "../video/framebuffer.h"
#include "../video/font.h"
#include "../net/net.h"
#include "../net/dhcp.h"
#include "../net/udp.h"
#include "../net/tcp.h"
#include "../net/socket.h"   // #524 BSD sockets
#include "../net/icmp.h"
#include "../net/sntp.h"   // #797: SNTP client (validation lives in rustkern/sntp.rs)
#include "../net/smb.h"
#include "../net/nfs.h"
#include "../ipc/msg.h"
#include "../ipc/shm.h"
#include "../gui/fb_syscall.h"
#include "../gui/installer.h"
#include "../exec/elf.h"
#include "../exec/ne.h"
#include "../fs/vfs.h"
#include "services.h"
#include "cron.h"
#include "../devinfo.h"
#include "../cpu/dlprof.h"
#include "../security/seclog.h"   /* #653: security event producer */
#include "syscall_path.h"  // #746b: SC_PATH_MAX + the shared path predicates
#include "../security/uaccess_smap.h"  // #19/#645: uaccess_begin/end for the copy-engine calls
#include "fdlayer.h"       // #746b: the legacy fd layer (proc/fdlayer.c)

// WM blit debug log toggle (see user_window_draw_handler). Default OFF.
volatile int g_wm_blit_debug = 0;

// ============================================================================
// #567: fault-safe (#509) user<->kernel copy helpers used by the handlers this
// task drained off the copy-user-lint KNOWN_GAP ledger. Every one routes the
// actual user access through strncpy_from_user / strnlen_user / copy_*_user,
// which do an ATOMIC entry-check-AND-copy with an exception-table fixup
// (mm/fault.c): a sibling thread that remaps the page mid-copy faults to
// -EFAULT instead of the kernel dereferencing a freed/remapped frame. They
// never raw-deref the destination kernel buffer, so they stay lint-clean.
// Justified-C (not Rust): these are in-place conversions of existing C handlers
// to the existing C copy_*_user primitives; no new subsystem, no hot FP path.
// ============================================================================
// #745: the bound on a bounced Ring-3 CREDENTIAL. sys_authenticate() had
// this inlined as a bare 128; sys_su()/sys_passwd_change() now bounce too,
// and three copies of an unnamed 128 is how they drift apart.
#define SC_PASSWORD_MAX 128
// #745: lock elevate.h's copy of the password bound to the real one. See the
// comment on ELEV_PASSWORD_MAX; this is the only translation unit where both
// names are visible, so it is the only place the duplication CAN be checked.
_Static_assert(ELEV_PASSWORD_MAX == SC_PASSWORD_MAX,
               "#745: ELEV_PASSWORD_MAX (proc/elevate.h) must equal SC_PASSWORD_MAX");


// Bounce a user C string into the fixed kernel buffer `dst` (cap bytes).
// strncpy_from_user always NUL-terminates within [0,cap-1]. Returns 0 on
// success, -14 (EFAULT) on a NULL/bad user pointer or a mid-copy fault.
static int sc_bounce_str(const char *usrc, char *dst, size_t cap) {
    if (!usrc || cap == 0) return -14;
    return (strncpy_from_user(dst, usrc, cap) < 0) ? -14 : 0;
}

// Duplicate a user C string into a freshly kmalloc'd kernel buffer (up to
// maxcap+1 bytes incl. NUL; caller kfree's). Returns NULL on NULL/bad pointer,
// a mid-copy fault, or OOM.
static char *sc_dup_user_str(const char *usrc, size_t maxcap) {
    if (!usrc) return 0;
    ssize_t n = strnlen_user(usrc, maxcap);
    if (n < 0) return 0;
    char *k = (char *)kmalloc((size_t)n + 1);
    if (!k) return 0;
    if (strncpy_from_user(k, usrc, (size_t)n + 1) < 0) { kfree(k); return 0; }
    return k;
}

static int64_t sys_spawn_args(const char *path, char **argv, int argc);
static int64_t sys_spawn_redir(const char *path, char **argv, int argc,
                               const char *infile, const char *outfile, int append);
// VFS file backings used for shell redirection (open a path as a struct file_t
// so it can be installed directly into a child's stdin/stdout fd).
extern file_t *fat_vfs_open(const char *path, int flags);
extern file_t *ext2_vfs_open(const char *path, int flags);
// ext2 root-cutover path helpers (defined alongside sys_open below).
// #317 pass 2: SMB network mount control syscalls (defined below).
int64_t sys_net_mount(const char *server, const char *share,
                      const char *user, const char *pass);
int64_t sys_net_list_shares(const char *server, char *ubuf, uint32_t maxlen);
int64_t sys_net_unmount(const char *server, const char *share);

// External filesystem
extern fat_fs_t g_fat_fs;

// External timer
extern volatile uint64_t timer_ticks;

// MSR addresses for SYSCALL/SYSRET
#define MSR_EFER            0xC0000080
#define MSR_STAR            0xC0000081
#define MSR_LSTAR           0xC0000082
#define MSR_CSTAR           0xC0000083  // 32-bit SYSCALL (not used)
#define MSR_SFMASK          0xC0000084

// #668: IA32_FMASK selects which RFLAGS bits SYSCALL CLEARS on entry. Two bits
// beyond IF/TF matter and were both missing.
//
//   DF - THE LIVE BUG. The SysV ABI requires DF=0 on entry and gcc emits
//        `rep movsb`/`rep stosb` on that assumption. DF is freely writable from
//        CPL 3, so Ring 3 could execute `std` and then issue a syscall (or just
//        wait for a timer interrupt) and kernel string operations would run
//        BACKWARDS from there. Ring-3-triggerable Ring-0 memory corruption, no
//        SMAP involved. An IDT gate does not clear DF either, which is why
//        cpu/idt.asm also gets a `cld` at isr_common.
//   AC - the SMAP override, also writable from CPL 3. Latent while SMAP is off,
//        but without this bit `pushfq; or $1<<18; popfq; syscall` would run a
//        whole syscall with SMAP disabled, making SMAP worthless (#645).
#define SFMASK_TF           0x100      // trap flag
#define SFMASK_IF           0x200      // interrupts
#define SFMASK_DF           0x400      // direction
#define SFMASK_AC           0x40000    // alignment check == SMAP override

// EFER bits
#define EFER_SCE            (1 << 0)    // SYSCALL enable

// Forward declaration of assembly entry point
extern void syscall_entry(void);

// Set kernel stack for syscall handling
extern void syscall_set_kernel_stack(uint64_t stack_top);

// Note: rdmsr/wrmsr are already defined in types.h

// ============================================================================
// Syscall Initialization
// ============================================================================

void syscall_init(void) {
    kprintf("[SYSCALL] Initializing syscall mechanism...\n");

    // Enable SYSCALL/SYSRET in EFER
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    // Set up STAR register:
    // Bits 63:48 = SYSRET CS and SS (user mode selectors)
    // Bits 47:32 = SYSCALL CS and SS (kernel mode selectors)
    // SYSRET loads CS from bits 63:48 + 16 for 64-bit, SS from bits 63:48 + 8
    // For user mode: CS = 0x1B (GDT_USER_CODE | 3), SS = 0x23 (GDT_USER_DATA | 3)
    // But SYSRET adds 16 to get 64-bit CS, so we use base selector 0x18
    // Kernel: CS = 0x08, SS = 0x10
    uint64_t star = ((uint64_t)(GDT_USER_CODE - 16) << 48) |  // User base (SYSRET adds 16)
                    ((uint64_t)GDT_KERNEL_CODE << 32);         // Kernel CS
    wrmsr(MSR_STAR, star);

    // Set LSTAR to syscall entry point (64-bit)
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    // Set SFMASK - flags to clear on syscall. #668: DF and AC added; see the
    // comment on the SFMASK_* defines for why omitting DF was exploitable.
    wrmsr(MSR_SFMASK, SFMASK_IF | SFMASK_TF | SFMASK_DF | SFMASK_AC);

    kprintf("[SYSCALL] SYSCALL/SYSRET enabled\n");
    kprintf("[SYSCALL] LSTAR = 0x%lx\n", (uint64_t)syscall_entry);
}

// ============================================================================
// Syscall Dispatcher
// ============================================================================

// Forward declarations for functions defined later in this file
int64_t sys_win_blit(int handle, int x, int y, int src_w, int src_h, uint32_t *src_buffer);
int64_t sys_win_draw_text_small(int handle, int x, int y, const char *text, uint32_t color);
int64_t sys_win_draw_text_ttf(int handle, int x, int y, const char *text, uint32_t color, int size);
int64_t sys_win_draw_text_ttf_ex(int handle, int x, int y, const char *text, uint32_t color, int size, int face, int style);
int64_t sys_win_get_pos(int handle, int *x, int *y);
int64_t sys_win_move(int handle, int x, int y);
int64_t sys_win_move_by(int handle, int dx, int dy);

// Network TX from a user-process syscall context runs on the process CR3, which
// only copies the kernel's UPPER-half PML4 entries (256-511). The NIC's MMIO
// registers and DMA ring buffers live in the kernel's LOWER-half identity map
// (entries 0-255), which is absent from the process CR3, so a raw eth_send from
// here silently fails (no packet reaches the wire, not even ARP requests).
// kprintf still works because serial uses port I/O, which needs no mapping.
//
// Fix: switch CR3 to the kernel master page table (vmm_get_pml4(), which maps
// BOTH kernel code/stack upper-half AND the NIC lower-half) for the duration of
// the send, with interrupts disabled, then restore the caller's CR3. This
// mirrors the proven scheduler pattern in process.c (mov %cr3). RX replies are
// drained by net_poll() running on the kernel CR3 in the desktop loop while this
// process sleeps between retries, so the ARP cache gets populated out of band.
extern uint64_t vmm_get_pml4(void);

static inline uint64_t net_cr3_enter(void) {
    uint64_t saved;
    __asm__ volatile("mov %%cr3, %0" : "=r"(saved));
    uint64_t kcr3 = vmm_get_pml4();
    __asm__ volatile("mov %0, %%cr3" : : "r"(kcr3) : "memory");
    return saved;
}

static inline void net_cr3_exit(uint64_t saved) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(saved) : "memory");
}

// Non-blocking DNS for userland (poll-split, like TCP connect/state).
// User-info struct as seen by userland (must match libc user_info_t).
typedef struct {
    char     username[64];
    char     display_name[64];
    uint32_t uid;
    uint32_t gid;
    uint8_t  active;
    uint8_t  padding[3];
} sc_user_info_t;
// #503 argtab sizeof-lock. The Rust table (rustkern.rs, SZ_SC_USER_INFO)
// validates SYS_LIST_USERS as max*140 writable bytes. If this struct grows and
// that constant does not, the tail of every row the kernel writes goes
// unvalidated. Grow one, grow the other.
_Static_assert(sizeof(sc_user_info_t) == 140,
               "#503 argtab: SZ_SC_USER_INFO in rustkern.rs is stale");

// Wallpaper index shared with the userland compositor: Settings sets it via
// SYS_SET_WALLPAPER, the compositor polls SYS_GET_WALLPAPER each frame and
// reloads the desktop background when it changes.
static int g_wallpaper_idx = 0;
// Desktop icon size (0=Small,1=Medium,2=Large); compositor polls it. (#63)
static int g_icon_size = 1;
// Display effects: brightness 0-100 (100=normal), night-light 0-100 (0=off).
// Compositor polls SYS_GET_DISPLAY_FX and post-processes the framebuffer. (#57)
static int g_brightness = 100;
static int g_nightlight = 0;
// System font size index (0=Small,1=Medium,2=Large,3=X-Large); compositor polls. (#58)
static int g_font_size = 1;
static int g_screensaver_type = 2;
// #652: 600, not 120. This is the AUTHORITATIVE default: get_ss_delay() returns
// it on a fresh boot, and because 120 >= 5 the compositor's own
// SS_DEFAULT_TIMEOUT fallback never fired, so anyone "fixing" the default in
// compositor.h alone changed nothing observable. Two minutes also sat at the
// very bottom of the Settings slider's 1-60 minute range. There are THREE
// copies of this default (here, userland/apps/compositor/compositor.h, and
// userland/apps/settings/main.c); they must agree.
static int g_screensaver_delay = 600;  // (#115/#652) activation delay, seconds
int g_win_blit_suppressed = 0;   // set by compositor while the screensaver owns the FB
// (#116) Live mouse-cursor style/size. Settings sets these via SYS_SET_CURSOR; the
// compositor reads them every frame via SYS_GET_CURSOR (same live-apply pattern as
// theme/opacity), so changing the cursor in Settings updates it without a reboot.
static int g_cursor_style = 0;   // 0=Light, 1=Dark, 2=Glow
static int g_cursor_size  = 100; // percent (100 = 1.0x)
static int g_settings_tab = -1;  // #74 one-shot: panel for the next Settings launch
static int g_ss_test = 0; // one-shot screensaver test trigger // SS_STARFIELD; compositor polls (#screensaver)

// Win16 launcher (#144): the interpreter runs in its own kernel process (the
// proven proc_create path, same primitive RemoteCtrl/terminal use) so the caller
// (compositor or RC) is never blocked and the desktop keeps drawing. One Win16
// app at a time (the interpreter uses global state).
static volatile int g_win16_busy = 0;
static char         g_win16_path[128];
static void win16_proc_entry(void *arg) {
    (void)arg;
    win16_run_file(g_win16_path);
    // #708: disarm the Win16 slot HERE, not in win16_api_end(). MEASURED: ne.c
    // calls win16_api_end() only "if (is_ne)", so after a .COM run through the
    // Win16 layer the teardown never fired and the slot stayed ARMED with the
    // last guest's identity. Nothing could exploit that today (every launch
    // re-arms, and re-arming disarms first), but "disarmed at teardown" has to
    // be TRUE, not nearly true. This is the single place every Win16 run
    // returns to, whatever the binary format, which is the same reason the DOS
    // side disarms in dos_proc_entry() rather than inside dos_run_file().
    guestfs_finish(GUESTFS_SLOT_WIN16);
    g_win16_busy = 0;
}
// Defined in dos/dosexec.c: launch an MS-DOS program in its own kernel proc +
// host window (non-blocking). Declared here so the SYS_DOS_RUN dispatch (#208)
// can call it without pulling in dos/dosexec.h.
int dos_launch(const char *path);

int win16_launch(const char *upath) {
    if (g_win16_busy) return -1;
    // #567: bounce the user path fault-safe into the kernel g_win16_path buffer.
    if (sc_bounce_str(upath, g_win16_path, sizeof(g_win16_path)) != 0) return -1;
    if (!g_win16_path[0]) return -1;
    // #708: capture the guest's identity HERE, while we are still on the
    // calling process's syscall stack. Once win16_proc_entry is running,
    // proc_current() is a kernel thread whose uid is 0 by construction, which
    // is exactly the identity a guest must not get. Fail the launch outright
    // rather than starting a guest that would be denied every file it opens.
    if (guestfs_arm_caller(GUESTFS_SLOT_WIN16) != 0) {
        kprintf("[win16] launch of '%s' REFUSED: no usable identity for the guest\n",
                g_win16_path);
        return -1;
    }
    g_win16_busy = 1;
    if (proc_create("win16", win16_proc_entry, NULL, PRIO_NORMAL) < 0) {
        g_win16_busy = 0;
        guestfs_disarm_rs(GUESTFS_SLOT_WIN16);
        return -1;
    }
    return 0;
}

// Disk info struct as seen by userland (must match libc disk_info_t).
typedef struct {
    uint8_t  present;
    uint8_t  type;        // 0=ATA, 1=ATAPI
    int8_t   smart;       // 1=ok, 0=failing, -1=unknown
    uint8_t  pad;
    uint32_t size_mb;
    char     model[41];
    char     serial[21];
    char     pad2[2];
} sc_disk_info_t;
// #503: SYS_GET_DISK_INFO's descriptor (rustkern.rs SZ_SC_DISK_INFO) hardcodes
// this size, because Rust cannot see a C struct private to this TU. If a field
// is added here and the table is not updated, the validator would prove fewer
// bytes writable than sys_get_disk_info() zeroes and fills, and the tail of
// every one of those writes would go unchecked. Fail the build instead. Value is
// compiler ground truth (nm -S on a probe TU with the real kernel CFLAGS).
_Static_assert(sizeof(sc_disk_info_t) == 72, "#503 argtab: SZ_SC_DISK_INFO in rustkern.rs is stale");
int64_t sys_get_disk_info(int idx, void *buf);

int64_t sys_list_users(sc_user_info_t *ubuf, int max) {
    if (!ubuf || max <= 0) return -1;
    int n = 0;
    user_entry_t *t = users_all(&n);
    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        if (!t[i].active) continue;
        // #567: build each entry in a kernel-local struct (zeroed, so no kernel
        // stack bytes leak into userland) then copy it out fault-safe.
        sc_user_info_t ke;
        memset(&ke, 0, sizeof(ke));
        int k;
        for (k = 0; k < 63 && t[i].username[k]; k++) ke.username[k] = t[i].username[k];
        ke.username[k] = '\0';
        for (k = 0; k < 63 && t[i].display_name[k]; k++) ke.display_name[k] = t[i].display_name[k];
        ke.display_name[k] = '\0';
        ke.uid = t[i].uid;
        ke.gid = t[i].gid;
        ke.active = 1;
        if (copy_to_user(&ubuf[out], &ke, sizeof(ke)) != 0) return -14;
        out++;
    }
    return out;
}

// #745 followup (Security): an authentication ATTEMPT against an account
// must not be mountable by a THIRD party. sys_su()/sys_authenticate() route
// through the shared per-account lockout inside users_authenticate(), and the
// kernel login gate (gui/login.c), the lock screen and the SSH server all read
// and enforce that SAME shadow_entry_t.failed_attempts/lockout_until_ms. So an
// unprivileged process calling these syscalls for an account that is NOT its
// own could (a) use the syscall as a slow password oracle and, worse, (b) drive
// the victim's failed-attempt counter and lock the real owner out of login,
// unlock and ssh: a no-privilege denial of service against the machine owner.
//
// The boundary is the one this file already uses for sys_passwd_change() and
// login_cfg_authorize(): root (the login authority) may authenticate anyone; a
// non-root caller may authenticate ONLY its own account. The test is by NAME
// against the caller's OWN record and never looks up the requested name, so it
// discloses nothing about which accounts exist. Callers must return BEFORE
// users_authenticate() on a deny so the victim's lockout counter is untouched.
static int caller_may_authenticate(const char *target) {
    process_t *p = proc_current();
    if (!p) return 0;                       // no caller identity: deny
    if (p->euid == 0) return 1;             // root: may authenticate anyone
    user_entry_t *self = user_lookup_uid(p->euid);
    if (!self) return 0;                    // caller has no account: deny
    return (strcmp(target, self->username) == 0) ? 1 : 0;
}

int64_t sys_authenticate(const char *u_uname, const char *u_upass) {
    // #567: bounce both credentials fault-safe into kernel buffers before the
    // (rate-limited) authenticator ever reads them.
    char uname[USERNAME_MAX], upass[SC_PASSWORD_MAX];
    if (sc_bounce_str(u_uname, uname, sizeof(uname)) != 0) return -1;
    if (sc_bounce_str(u_upass, upass, sizeof(upass)) != 0) return -1;
    if (upass[0] == '\0') return -1;   // #566: empty password never authenticates
    // #745 followup: non-root may authenticate only its OWN account. Deny
    // BEFORE users_authenticate() so a cross-account guess never touches the
    // victim's shared lockout counter (Claim 1 oracle + Claim 2 login DoS).
    if (!caller_may_authenticate(uname)) {
        process_t *pa = proc_current();
        bootlog_write("[AUTH] authenticate DENIED (not own account): uid=%u -> '%s' (#745)",
                      (unsigned)(pa ? pa->euid : 0), uname);
        return -1;   // EPERM; victim's lockout state untouched
    }
    int r = users_authenticate(uname, upass);  // rate-limited + escalating lockout
    if (r != 0) return r;               // -1 bad credentials, -2 locked out
    user_entry_t *u = user_lookup_name(uname);
    return u ? (int64_t)u->uid : -1;
}

int64_t sys_delete_user(const char *uname) {
    process_t *p = proc_current();
    if (p && p->euid != 0) return -1;   // root only
    if (!uname) return -1;
    return user_delete_by_name(uname);
}

// ============================================================================
// #566 Secure session lock / unlock and autologin policy.
// The kernel is the sole authority for "is this session locked" (gui/desktop.c
// g_session_locked). A Ring-3 app can only READ it and can only CLEAR it via
// SYS_SESSION_UNLOCK, which itself runs the rate-limited users_authenticate().
// ============================================================================
extern fat_fs_t g_fat_fs;
extern void desktop_set_locked(int locked);
extern int  desktop_is_locked(void);
extern uint32_t desktop_get_session_uid(void);

// Bounded copy of a userspace C string into a fixed kernel buffer (NUL-safe).
static void sc_copy_str(const char *usrc, char *dst, int cap) {
    int i = 0;
    if (usrc) { for (; i < cap - 1 && usrc[i]; i++) dst[i] = usrc[i]; }
    dst[i] = '\0';
}

// #19/#645: the USER-POINTER twin of sc_copy_str(). sc_copy_str() reads its
// source byte-by-byte with a plain Ring-0 load, which is a #PF under CR4.SMAP
// and, worse, was the tree's documented "GAP-2" hole: a Ring-3 string read two
// call levels below a dispatcher case, with no validation at all. It is kept
// (one caller still passes a KERNEL string, session_user()->username) and every
// call whose source is a Ring-3 pointer now goes through this one, which is a
// thin wrapper over the canonical strncpy_from_user primitive: U/S-checked,
// fault-fixed-up and AC-bracketed. A NULL or unreadable source yields "",
// preserving sc_copy_str()'s contract exactly.
static void sc_copy_str_user(const char *usrc, char *dst, int cap) {
    if (!dst || cap <= 0) return;
    dst[0] = '\0';
    if (!usrc) return;
    if (strncpy_from_user(dst, usrc, (size_t)cap) < 0) dst[0] = '\0';
}

// Read the configured boot-autologin username from /CONFIG/LOGIN.CFG into `out`
// (empty string if none / unreadable). Extracted so the LOGIN.CFG parse lives in
// exactly ONE place, shared by sys_get_autologin() and the session-lock policy.
static void autologin_configured_user(char *out, int cap) {
    if (out && cap > 0) out[0] = '\0';
    if (!out || cap <= 0 || !g_fat_fs.mounted) return;
    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, "/CONFIG/LOGIN.CFG", &size);
    if (!data || size == 0) { if (data) kfree(data); return; }
    const char *src = (const char *)data; const char *end = src + size;
    while (src < end) {
        while (src < end && (*src==' '||*src=='\n'||*src=='\r'||*src=='\t')) src++;
        if (src >= end) break;
        if (strncmp(src, "autologin=", 10) == 0) {
            src += 10; int i = 0;
            while (src < end && *src!='\n' && *src!='\r' && i < cap-1)
                out[i++] = *src++;
            out[i] = '\0';
            break;
        }
        while (src < end && *src != '\n') src++;
    }
    kfree(data);
}

// #566/macOS-style autologin: is boot autologin enabled FOR THE CURRENT SESSION
// USER? Used to keep an auto-logged-in session from locking itself out on idle.
// Returns 0 for any non-autologin session (autologin disabled entirely, or a
// Switch-User to a DIFFERENT account than the configured autologin one), so a
// real multi-user login box is never bypassed.
static int session_autologin_active(void) {
    char name[USERNAME_MAX];
    autologin_configured_user(name, sizeof(name));
    if (!name[0]) return 0;
    user_entry_t *su = user_lookup_uid(desktop_get_session_uid());
    if (!su) return 0;
    return strcmp(name, su->username) == 0 ? 1 : 0;
}

// #745: resolve the CURRENT SESSION's account. The kernel owns this fact
// (gui/desktop.c g_session_uid, set by the login gate); nothing in Ring 3 gets
// to name it. Every session-scoped operation below goes through here, so the
// session identity has exactly one definition.
static user_entry_t *session_user(void) {
    return user_lookup_uid(desktop_get_session_uid());
}

// #745 lock policy decision, in rustkern/sessionid.rs. Returns 0 = allow,
// 1 = deny (session user cannot authenticate), 2 = deny (autologin + idle).
extern uint32_t session_lock_decide_rs(uint32_t reason, uint32_t autologin_active,
                                       uint32_t session_can_auth);

int64_t sys_session_lock(int reason) {
    // #745. Two things changed here and they are opposites, which is why they
    // had to land together:
    //
    //  - An EXPLICIT lock (Start Menu > Lock, Super+L) is now HONOURED on an
    //    autologin box. It used to be silently discarded along with the idle
    //    lock, which meant the entire lock/unlock path was dead code on the
    //    shipping image and nothing downstream of it had ever been exercised.
    //
    //  - A session whose user CANNOT AUTHENTICATE is now never locked, for any
    //    reason. Making explicit locks work without this would have handed the
    //    user a way to brick their own session: an account with no usable
    //    credential (no shadow record, or "*") locks and can never unlock, and
    //    the only recovery is a power cycle. The shipped `ref` account is such
    //    an account today.
    //
    // The decision itself is in Rust with a boot self-test; this function only
    // gathers the facts and applies the verdict.
    user_entry_t *su = session_user();
    uint32_t can_auth = (su && users_can_authenticate(su->username)) ? 1u : 0u;
    uint32_t auto_on  = session_autologin_active() ? 1u : 0u;
    uint32_t verdict  = session_lock_decide_rs((uint32_t)reason, auto_on, can_auth);

    if (verdict != 0) {
        // Never silent. A refused lock must be visible in the bootlog, because
        // "I pressed Lock and nothing happened" is otherwise indistinguishable
        // from a hung compositor.
        bootlog_write("[SESSION] LOCK DECLINED (reason=%d verdict=%u) uid=%u user='%s' "
                      "autologin=%u can_auth=%u",
                      reason, verdict, desktop_get_session_uid(),
                      su ? su->username : "?", auto_on, can_auth);
        kprintf("[SESSION] lock declined: %s\n",
                verdict == 1 ? "session user has no usable password"
                             : "autologin session, idle lock suppressed");
        return -1;
    }

    desktop_set_locked(1);
    process_t *p = proc_current();
    bootlog_write("[SESSION] LOCK ok (reason=%d) session uid=%u user='%s', requested by uid=%u",
                  reason, desktop_get_session_uid(), su ? su->username : "?",
                  p ? p->euid : 0);
    return 0;
}

int64_t sys_session_is_locked(void) {
    return desktop_is_locked() ? 1 : 0;
}

int64_t sys_session_unlock(const char *uuser, const char *upass) {
    if (!desktop_is_locked()) return 0;          // already unlocked
    char user[USERNAME_MAX]; char pass[128];
    sc_copy_str_user(uuser, user, sizeof(user));   // #19/#645: Ring-3 strings
    sc_copy_str_user(upass, pass, sizeof(pass));
    // Unlock resumes the SAME session: the password must be the session user's.
    // Switching users is a logout back to the login gate, not an unlock.
    user_entry_t *su = session_user();
    // #745: AN EMPTY/NULL USERNAME MEANS "THE SESSION USER", and that is now
    // the sanctioned way to call this. The compositor used to pass a hardcoded
    // "root"; at uid 1000 the strcmp below never matched and the session could
    // not be unlocked except by rebooting. The deeper problem is that the name
    // was a Ring-3 input at all: the kernel already knows who the session is,
    // and it checks the supplied name against that, so the name could only ever
    // agree or cause a failure. Removing it from the caller removes the entire
    // class, including the next caller that gets it wrong. A non-empty name is
    // still accepted and still must match, so existing callers keep working.
    int64_t rc;
    if (!su) { rc = -1; }
    else if (user[0] != '\0' && strcmp(user, su->username) != 0) {
        // Wrong account named: still burn a failed attempt on the session user
        // so this cannot be used to probe other accounts without penalty.
        (void)users_authenticate(su->username, pass);
        rc = -1;
    } else {
        int r = users_authenticate(su->username, pass);
        if (r == 0) {
            desktop_set_locked(0);
            bootlog_write("[SESSION] UNLOCK ok for '%s'", su->username);
            rc = 0;
        } else {
            bootlog_write("[SESSION] UNLOCK failed for '%s' (r=%d)", su->username, r);
            rc = r;                                // -1 bad, -2 locked out
        }
    }
    memset(pass, 0, sizeof(pass));
    return rc;
}

int64_t sys_auth_lockout(const char *uuser) {
    char user[USERNAME_MAX];
    sc_copy_str_user(uuser, user, sizeof(user));   // #19/#645: Ring-3 string
    // #745: same rule as sys_session_unlock - empty means the session user, so
    // the lock screen never has to know a name to report its own lockout.
    if (user[0] == '\0') {
        user_entry_t *su = session_user();
        if (!su) return 0;
        sc_copy_str(su->username, user, sizeof(user));
    }
    return users_get_lockout(user);
}

// Enable/disable boot autologin for an account (#566 decision 1: secure,
// macOS-style opt-in). Stored in /CONFIG/LOGIN.CFG, force-set root-only 0600 so
// a non-root Ring-3 process cannot rewrite it via sys_open(). Authorization:
// root sets it for anyone; a non-root caller may only set it for THEIR OWN
// account and must prove the account password.
// #745: THE ONE AUTHORIZATION GATE for /CONFIG/LOGIN.CFG.
//
// Root may set these for anyone; a non-root caller may set them only for THEIR
// OWN account, and must prove that account's password. This was inline in
// sys_set_autologin(); it is a function now because a second syscall writes the
// same file, and two copies of an authorization rule is how one of them ends up
// weaker than the other after a later edit to only one.
//
// Returns the target account, or NULL if the caller is refused. The password
// copy is scrubbed on every path, including the refusals.
static user_entry_t *login_cfg_authorize(const char *uuser, const char *upass) {
    char user[USERNAME_MAX]; char pass[128];
    sc_copy_str_user(uuser, user, sizeof(user));   // #19/#645: Ring-3 strings
    sc_copy_str_user(upass, pass, sizeof(pass));
    process_t *p = proc_current();
    int is_root = (p && p->euid == 0);
    user_entry_t *target = user_lookup_name(user);
    if (!target) { memset(pass, 0, sizeof(pass)); return NULL; }
    if (!is_root) {
        if (!p || target->uid != p->euid ||
            users_authenticate(user, pass) != 0) {
            memset(pass, 0, sizeof(pass));
            return NULL;
        }
    }
    memset(pass, 0, sizeof(pass));
    return target;
}

// #745 LOGIN.CFG parse + compose live in rustkern/loginmode.rs (new kernel code
// = Rust per the 2026-07-16 rule). The C below does the FAT I/O and nothing
// else: it decides no bytes and parses no keys.
extern uint32_t login_mode_parse_rs(const void *buf, uint32_t len);
extern int32_t  login_cfg_compose_rs(const void *old, uint32_t old_len,
                                     const char *autologin, int32_t mode,
                                     void *out, uint32_t out_cap);

// Whole-file read, bounded-retry (the same primitive gui/login.c uses: real
// USB-MSC/ATA hardware returns a transient NULL that a plain read would report
// as "no config"). Caller kfree()s. NULL / *size_out = 0 on any failure.
static void *login_cfg_read(uint32_t *size_out) {
    if (size_out) *size_out = 0;
    if (!g_fat_fs.mounted) return NULL;
    uint32_t sz = 0;
    void *d = fat_read_file_retry(&g_fat_fs, "/CONFIG/LOGIN.CFG", &sz);
    if (!d) return NULL;
    if (sz == 0) { kfree(d); return NULL; }
    if (size_out) *size_out = sz;
    return d;
}

// #745: the configured sign-in screen mode. NON-STATIC on purpose - the boot
// gate (gui/login.c) calls it, so this key has ONE file-read path and ONE
// parse, shared by the kernel reader and by SYS_GET_LOGIN_MODE. Any failure to
// read resolves to LOGIN_MODE_TYPED, which is the non-disclosing direction.
int login_mode_configured(void) {
    uint32_t sz = 0;
    void *d = login_cfg_read(&sz);
    if (!d) return LOGIN_MODE_TYPED;
    int m = (int)login_mode_parse_rs(d, sz);
    kfree(d);
    return m;
}

// Rewrite /CONFIG/LOGIN.CFG. `autologin` NULL preserves the existing autologin
// line, "" disables it, anything else enables it for that name; `mode` < 0
// preserves the existing login_mode line. The composer reads the file it is
// about to replace, so writing one key can never erase the other - that erase
// is exactly what the previous single-snprintf implementation would have done
// to login_mode on every startup-mode change.
static int login_cfg_write(const char *autologin, int mode) {
    if (!g_fat_fs.mounted) return -1;
    uint32_t sz = 0;
    void *old = login_cfg_read(&sz);
    char buf[192];
    int32_t n = login_cfg_compose_rs(old, sz, autologin, (int32_t)mode,
                                     buf, (uint32_t)sizeof(buf));
    if (old) kfree(old);
    if (n <= 0) return -1;
    if (fat_write_file(&g_fat_fs, "/CONFIG/LOGIN.CFG", buf, (int)n) != 0) return -1;
    // Force root-only 0600 after every write so a non-root Ring-3 process can
    // never rewrite this file through sys_open().
    perms_set("/CONFIG/LOGIN.CFG", 0, 0, 0600);
    if (perms_sync() != 0)
        kprintf("[SESSION] perms sync failed after LOGIN.CFG write\n");
    return 0;
}

int64_t sys_set_autologin(const char *uuser, const char *upass, int enable) {
    user_entry_t *target = login_cfg_authorize(uuser, upass);
    if (!target) return -1;
    process_t *p = proc_current();
    // #693: a failed persist is reported to Ring 3 in BOTH directions. Enabling
    // autologin that never reached the disk silently does nothing at the next
    // boot; DISABLING it that never reached the disk is worse, because the
    // machine keeps auto-logging in and nobody is told.
    if (login_cfg_write(enable ? target->username : "", -1) != 0) {
        kprintf("[SESSION] FAILED to persist /CONFIG/LOGIN.CFG (autologin %s)\n",
                enable ? "ENABLE" : "DISABLE");
        return -1;
    }
    if (enable)
        bootlog_write("[SESSION] autologin ENABLED for '%s' by uid=%u",
                      target->username, p ? p->euid : 0);
    else
        bootlog_write("[SESSION] autologin DISABLED by uid=%u", p ? p->euid : 0);
    return 0;
}

// #745: the sign-in screen mode. Same file, same gate, same composer as
// autologin above; the ONLY difference is which key this caller owns.
int64_t sys_set_login_mode(int mode, const char *uuser, const char *upass) {
    if (mode != LOGIN_MODE_LIST && mode != LOGIN_MODE_TYPED) return -1;
    user_entry_t *target = login_cfg_authorize(uuser, upass);
    if (!target) return -1;
    process_t *p = proc_current();
    if (login_cfg_write(NULL, mode) != 0) {
        kprintf("[SESSION] FAILED to persist /CONFIG/LOGIN.CFG (login_mode)\n");
        return -1;
    }
    bootlog_write("[SESSION] login_mode set to '%s' by uid=%u",
                  mode == LOGIN_MODE_LIST ? "list" : "typed", p ? p->euid : 0);
    return 0;
}

// Report the configured sign-in screen mode. Ungated, exactly as
// SYS_GET_AUTOLOGIN is: the lock screen runs as the session user (non-root) and
// has to know which screen to draw, and the answer is not itself a secret - the
// disclosure is the list of NAMES, which this does not return.
int64_t sys_get_login_mode(void) {
    return (int64_t)login_mode_configured();
}

// Report the configured autologin username (empty -> 0). Not sensitive: the
// design wants autologin state visible. Kernel reads via fat directly (not
// perms-gated) so Settings can display it regardless of caller euid.
int64_t sys_get_autologin(char *ubuf, int cap) {
    if (!ubuf || cap <= 0) return 0;
    char name[USERNAME_MAX];
    autologin_configured_user(name, sizeof(name));   // shared LOGIN.CFG parse
    if (!name[0]) return 0;
    // #567: truncate into a kernel buffer, then a single fault-safe copy_to_user.
    char out[USERNAME_MAX];
    int i = 0; for (; i < cap-1 && i < (int)sizeof(out)-1 && name[i]; i++) out[i] = name[i];
    out[i] = '\0';
    if (copy_to_user(ubuf, out, (size_t)i + 1) != 0) return -14;
    return i;
}

int64_t sys_dns_start(const char *uhost, uint32_t *uip) {
    extern int dns_resolve_start(const char *hostname, uint32_t *ip_out);
    extern int dns_resolve_check(uint32_t *ip_out);
    extern void net_poll(void);
    if (!uhost || !uip) return -1;
    // #567: bounce the hostname fault-safe into a kernel buffer before the
    // resolver reads it, and write the result back via copy_to_user.
    char host[256];
    if (sc_bounce_str(uhost, host, sizeof(host)) != 0) return -1;
    uint32_t ip = 0;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    int rc = dns_resolve_start(host, &ip);
    net_poll();
    if (rc == 0 && dns_resolve_check(&ip) == 1) rc = 1;
    net_cr3_exit(saved);
    if (flags & 0x200) __asm__ volatile("sti");
    if (rc == 1 && copy_to_user(uip, &ip, sizeof(ip)) != 0) return -14;
    return rc;
}

int64_t sys_dns_poll(uint32_t *uip) {
    extern int dns_resolve_check(uint32_t *ip_out);
    extern void net_poll(void);
    uint32_t ip = 0;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    net_poll();
    int rc = dns_resolve_check(&ip);
    net_cr3_exit(saved);
    if (flags & 0x200) __asm__ volatile("sti");
    // #567: fault-safe write-back of the resolved IP.
    if (rc == 1 && uip && copy_to_user(uip, &ip, sizeof(ip)) != 0) return -14;
    return rc;
}

extern int eth_receive(void);

// Send an ICMP echo request AND drain the NIC RX ring, all on the kernel address
// space. We cannot rely on the desktop loop's net_poll() to process the ARP/echo
// replies while a user ping process spins, so we both transmit and receive here:
//   - icmp_ping() fires the echo (or an ARP request if the next hop is unresolved)
//   - eth_receive() processes any pending RX frames (an ARP reply caches the MAC,
//     an ICMP echo reply sets the ping-reply flag)
// Both TX and RX touch the NIC MMIO/DMA in the kernel lower-half identity map,
// which is absent from the user process CR3, so the whole window runs on the
// kernel pml4 with interrupts disabled. Returns icmp_ping()'s result.
static int icmp_ping_kcr3(uint32_t dest_ip) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    int rc = icmp_ping(dest_ip);
    for (int i = 0; i < 64; i++) {
        if (!eth_receive())
            break;
    }
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

// Drain only the NIC RX ring on the kernel address space (used while waiting for
// the echo reply, so we do not keep retransmitting).
static void net_rx_drain_kcr3(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    for (int i = 0; i < 64; i++) {
        if (!eth_receive())
            break;
    }
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
}

// ---------------------------------------------------------------------------
// (#745) SYS_NET_PROBE shims. THREE lines of real code; they exist only to own
// the ADDRESS-SPACE + INTERRUPT window (cli + net_cr3_enter/net_cr3_exit)
// around NIC MMIO/DMA, which is irreducibly entangled with paging and inline
// asm and is therefore the stated reason this half stays C while the probe
// logic itself is Rust (rustkern/netstat.rs). They reuse the two static
// helpers already written for SYS_PING rather than duplicating a CR3 switch,
// so the rule has ONE definition.
//
// NONE of these blocks. net_probe_tx_c() puts at most one echo request on the
// wire and returns; net_probe_rx_c() drains the RX ring and returns;
// net_dhcp_restart_c() kicks a DORA and returns. The deadline lives in the
// caller, by design.
int  net_probe_tx_c(uint32_t dest_ip) { return icmp_ping_kcr3(dest_ip); }
void net_probe_rx_c(void)             { net_rx_drain_kcr3(); }

int net_dhcp_restart_c(void) {
    extern int dhcp_discover(void);
    extern void net_clear_fault(void);
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    net_clear_fault();               // #549: an explicit retry clears the fault
    int rc = dhcp_discover();        // non-blocking: sends DISCOVER, returns
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

// ---------------------------------------------------------------------------
// TCP syscall wrappers (used by userland network apps, e.g. /APPS/irc)
//
// Same root cause as ping: a TCP TX from a user-process syscall runs on the
// process CR3, which lacks the kernel lower-half identity map where the NIC
// MMIO/DMA live, so the SYN / data / FIN never reach the wire. We run every TCP
// operation on the kernel pml4 with interrupts disabled, and also drain the RX
// ring there so the handshake and incoming data make progress in-band rather
// than relying on the desktop poll loop being scheduled. tcp_send / tcp_recv
// take user pointers, which are NOT mapped under the kernel CR3, so we bounce
// the payload through a kernel-stack buffer while the user CR3 is still active.
extern void tcp_timer(void);

static inline void net_rx_drain_locked(void) {
    for (int i = 0; i < 64; i++) {
        if (!eth_receive())
            break;
    }
    tcp_timer();
}

static int tcp_connect_kcr3(int sock, uint32_t ip, uint16_t port) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    int rc = tcp_connect(sock, ip, port);
    net_rx_drain_locked();
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

static int tcp_send_kcr3(int sock, const void *ubuf, uint16_t len) {
    uint8_t kbuf[1600];
    if (len > sizeof(kbuf))
        len = sizeof(kbuf);
    // #567: fault-safe copy from user space while the process CR3 is still
    // active (was a raw memcpy: TOCTOU-unsafe if a sibling remaps the page).
    if (len && copy_from_user(kbuf, ubuf, len) != 0)
        return -14;
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    int rc = tcp_send(sock, kbuf, len);
    net_rx_drain_locked();
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

static int tcp_recv_kcr3(int sock, void *ubuf, uint16_t len) {
    uint8_t kbuf[1600];
    if (len > sizeof(kbuf))
        len = sizeof(kbuf);
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    net_rx_drain_locked();
    int rc = tcp_recv(sock, kbuf, len);
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    // #567: fault-safe copy to user space after the process CR3 is restored.
    if (rc > 0 && copy_to_user(ubuf, kbuf, (size_t)rc) != 0)
        return -14;
    return rc;
}

static int tcp_close_kcr3(int sock) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    int rc = tcp_close(sock);
    net_rx_drain_locked();
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

// tcp_get_state itself touches no NIC, but the userland app polls it in a loop
// waiting for the handshake to complete. We must run the full RX drain AND
// tcp_timer() here on the kernel CR3:
//   - eth_receive() processes the ARP reply (caches the next-hop MAC) and any
//     SYN-ACK, advancing the state and sending the final ACK.
//   - tcp_timer() retransmits the SYN. The very first SYN is dropped because the
//     next-hop MAC is not yet resolved (tcp_connect only fires the ARP request);
//     without a retransmit the handshake never starts. tcp_timer is time-based
//     (TCP_RETRANSMIT_TIMEOUT ticks), so calling it on every poll is safe and the
//     SYN goes back out once the ARP cache is populated.
// Both touch the NIC MMIO/DMA in the kernel lower-half identity map, absent from
// the user process CR3, so the whole window runs on the kernel pml4 with
// interrupts disabled.
static int tcp_state_kcr3(int sock) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    net_rx_drain_locked();
    int rc = (int)tcp_get_state(sock);
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

// #443: bind()+listen() combined into one syscall (this codebase never exposed
// a standalone SYS_BIND to userland). Neither tcp_bind() nor tcp_listen() sends
// anything on the wire -- they only update the local connection table entry
// (same as tcp_socket()/tcp_close(), which are also called directly below with
// no CR3 switch) -- so no kcr3 wrapping is needed here.
static int tcp_listen_bind(int sock, uint16_t port, int backlog) {
    int r = tcp_bind(sock, port);
    if (r < 0) return r;
    return tcp_listen(sock, backlog);
}

// #443: accept() must let a pending SYN's handshake actually complete (SYN ->
// SYN-ACK -> ACK -> ESTABLISHED) so tcp_accept() has something to return, which
// requires draining the NIC RX ring and running the retransmit timer -- the
// same reasoning as tcp_state_kcr3 above, which a polling tcp_connect() loop
// relies on for the same purpose. A userland server calling accept() in a loop
// must not depend on some other process's desktop loop happening to be
// scheduled to drive net_poll(); do it here so accept() makes progress on its
// own regardless of what else is running.
static int tcp_accept_kcr3(int sock) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    uint64_t saved = net_cr3_enter();
    net_rx_drain_locked();
    int rc = tcp_accept(sock);
    net_cr3_exit(saved);
    if (flags & 0x200)
        __asm__ volatile("sti");
    return rc;
}

// ICMP ping: send one echo request to dest_ip (host byte order) and wait up to
// timeout_ms for a reply. Returns approximate round-trip ms (>=0) on reply,
// -1 on send failure or timeout.
//
// icmp_ping() -> ip_send() returns <0 when the next hop's MAC is not yet in the
// ARP cache; ip_send() fires an ARP request in that case and expects the caller
// to retry once the reply arrives. We retry the send (each retry also drains RX
// so the ARP reply is cached in the same syscall), then poll the reply flag.
static int64_t sys_ping(uint32_t dest_ip, int timeout_ms) {
    if (timeout_ms <= 0)
        timeout_ms = 1000;
    int waited = 0;

    // Phase 1: get the echo request onto the wire (resolving ARP if needed).
    int sent = 0;
    while (waited < timeout_ms) {
        if (icmp_ping_kcr3(dest_ip) >= 0) {
            sent = 1;
            break;
        }
        proc_sleep(20);
        waited += 20;
    }
    if (!sent)
        return -1;

    // Phase 2: wait for the matching echo reply, draining RX as we go.
    while (waited < timeout_ms) {
        net_rx_drain_kcr3();
        uint32_t src = 0;
        uint16_t seq = 0, rtt = 0;
        if (icmp_get_ping_reply(&src, &seq, &rtt))
            return (int)rtt;
        proc_sleep(5);
        waited += 5;
    }
    return -1;
}

// #430: signal engine (proc/signal.c), threads (proc/process.c) and futex
// (sync/futex.c). Declared here so the dispatcher can reach them.
extern int64_t sys_kill(int pid, int signo);
extern int64_t sys_sigaction(int signo, const void *new_act, void *old_act);
extern int64_t sys_sigprocmask(int how, const uint64_t *set, uint64_t *oldset);
extern int64_t sys_rt_sigreturn(void);
extern int64_t sys_alarm(uint32_t seconds);
extern int64_t sys_pause(void);
extern int proc_clone(uint32_t flags, void *user_stack, uint32_t *parent_tid,
                      uint32_t *child_tid, void *tls);
extern uint32_t proc_gettid(void);
extern uint32_t proc_set_tid_address(uint32_t *tidptr);
extern int64_t sys_futex(uint32_t *addr, int op, uint32_t val, uint64_t timeout,
                         uint32_t *addr2, uint32_t val3);

// ===========================================================================
// #700 B1: SYS_PKG_WRITE authorization. THE decision, in ONE place.
//
// WHAT WAS WRONG. SYS_PKG_WRITE (301) took an arbitrary path and an arbitrary
// buffer and wrote them, with no check of any kind. Measured on golden 1025
// from a Ring-3 process at uid 1000: pkg_write("/CONFIG/AUTHKEYS", ...) returned
// 0 and the root-owned 0600 SSH authorized-keys file came off the disk holding
// the caller's 29 chosen bytes. The same call on "/boot/kernel.elf" defeats the
// entire signed-OTA design in selfupdate.c: an attacker who can replace the
// kernel image never needs a signature, a capability, or Win16.
//
// TWO INDEPENDENT CONTROLS, deliberately, because either one alone has a way to
// fail:
//
//  1. A HARD REFUSAL of the boot paths for EVERY Ring-3 caller including root.
//     Not a permission decision: there is no legitimate SYS_PKG_WRITE to the
//     kernel image (selfupdate.c writes it from Ring 0, after verifying a
//     signature), so "root asked" is not an argument. This is the control that
//     still holds when an attacker has become root but not Ring 0. The streaming
//     path at sys_pkg_write_stream() already excluded /boot and /EFI; that
//     exclusion existed only because those paths are not on ext2, and the
//     buffered fallback, which IS the FAT ESP writer, excluded nothing at all.
//
//  2. THE ORDINARY POSIX RULE, the same shape sys_open_k() and
//     open_redir_file() already use (#676): creating a NAME is a write to the
//     PARENT DIRECTORY; overwriting an EXISTING file is a write to that file.
//     Same helper (sc_parent_of), same access bits (W_OK|X_OK on the parent),
//     so there is one rule for "may this uid put bytes at this path", not a
//     second one that lives only in the package manager.
//
// Neither control is applied to Ring-0 callers: perms_check() is consulted only
// for PRIV_USER, exactly as everywhere else in this file.
// ===========================================================================

// Case-insensitive match of `pfx` at the head of `p`, requiring the match to end
// on a path boundary ('/' or end of string) so "/BOOTLOG.TXT" is not read as a
// hit on "/BOOT". FAT is case-insensitive and both spellings reach this syscall
// ("/boot/kernel.elf" from the OTA client, "/BOOT/KERNEL.ELF" from the deploy),
// so a case-sensitive compare would be a bypass by typing.
static int pkg_pfx_ci(const char *p, const char *pfx) {
    unsigned i = 0;
    for (; pfx[i]; i++) {
        char a = p[i], b = pfx[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return 0;
    }
    return (p[i] == 0 || p[i] == '/');
}

// Control 1. Boot-critical paths, refused for every Ring-3 caller.
static int pkg_path_is_boot(const char *kp) {
    if (pkg_pfx_ci(kp, "/BOOT")) return 1;          // /boot, /BOOT/..., /boot/kernel.elf
    if (pkg_pfx_ci(kp, "/EFI"))  return 1;          // /EFI/BOOT/BOOTX64.EFI and siblings
    if (pkg_pfx_ci(kp, "/KERNEL.ELF")) return 1;    // the ESP-root copy
    return 0;
}

// Controls 1 + 2 together. Returns 0 to allow, -1 to refuse.
//
// ARRAY parameter, not a pointer, for the same two reasons as
// sys_pkg_write_stream(): it says in the type that this takes a full-size
// KERNEL buffer and never the caller's pointer, and it keeps copy-user-lint's
// "every pointer param of a function a dispatcher case calls is a user pointer"
// default from firing on a path that has already been bounced.
static int pkg_write_permit(const char kp[SC_PATH_MAX]) {
    if (!kp || kp[0] != '/') return -1;

    process_t *p = proc_current();
    if (!p || p->privilege != PRIV_USER) return 0;   // Ring 0: unchanged

    if (pkg_path_is_boot(kp)) {
        kprintf("[PKGWRITE] DENIED uid=%u path=%s (boot path is never writable "
                "via SYS_PKG_WRITE; the signed OTA path in selfupdate.c is)\n",
                p->euid, kp);
        return -1;
    }

    extern fat_fs_t g_fat_fs;
    if (!fat_exists(&g_fat_fs, kp)) {
        char parent[SC_PATH_MAX];
        sc_parent_of(kp, parent, sizeof(parent));
        if (perms_check(parent, p->euid, p->egid, W_OK | X_OK) != 0) return -1;
    } else if (perms_check(kp, p->euid, p->egid, W_OK) != 0) {
        return -1;
    }
    return 0;
}

// #689: after a successful install, give the file a DELIBERATE owner and mode
// instead of leaving it with no PERMS.DB entry at all (which is how a file the
// caller had just created came back root-owned and un-chmod-able: measured, the
// probe's own staged copy could not chmod itself).
//
// An ELF gets 0555. The installing user owns it and can delete and replace it
// (that is a write to the parent directory, which they already passed), but
// cannot MUTATE IN PLACE the bytes of a binary that may already have been
// launched and blessed. Root is unaffected: perms_check() short-circuits on
// uid 0, so a root-run App Store re-installs over it normally.
// perms_on_create() never overwrites an existing entry, so an operator chmod
// and the perms_system_seed[] pins both survive a rewrite.
static void pkg_write_stamp(const char kp[SC_PATH_MAX], int is_elf) {
    process_t *p = proc_current();
    if (!p || p->privilege != PRIV_USER) return;
    uint32_t u = 0, g = 0; uint16_t m = 0;
    int had_entry = (perms_get(kp, &u, &g, &m) == 0);
    perms_on_create(kp, p->euid, p->egid, 0);
    if (!had_entry && is_elf) perms_set(kp, p->euid, p->egid, 0555);
}

// #572: stream a SYS_PKG_WRITE payload straight to the ext2 root in bounded,
// block-flushed chunks instead of one giant kmalloc + whole-file write, so a
// 100MB+ package payload needs only ~block_size of kernel heap. Returns 0 on
// success, or -1 if the target is not an ext2-root path (caller then falls back
// to the whole-buffer path) or on a write error. `udata` is a user pointer and
// is read exclusively through copy_from_user (this function is defined in
// syscall.c so copy-user-lint traces into it from the case).
//
// #700 B1: `kp` is now a KERNEL copy made by the caller, not a user pointer.
// The authorization decision moved to the SYS_PKG_WRITE case so that it runs
// once, before EITHER path. It could not stay here: this function returns -1
// both for "not an ext2 target, use the other path" and for a hard error, so a
// refusal expressed as -1 would have fallen straight through to the buffered
// writer, which is the one that had no checks at all.
//
// The path parameter is declared as an ARRAY, not a pointer, and that is
// deliberate on two counts. It states in the signature that the caller must
// hand over a full-size KERNEL buffer, not a Ring-3 string; and copy-user-lint
// treats every pointer parameter of a function reached from a dispatcher case
// as a user pointer, which is the right default and the wrong answer here. The
// alternative was a manifest exemption for the whole syscall, which would have
// switched the lint off for the data buffer too. Making the type say what is
// true beats annotating around it. Re-copying the path inside would have been
// worse still: the path authorized in the case and the path written here would
// then be two separate reads of Ring-3 memory, which is the #509 check-and-use
// hazard the bounce exists to remove.
static int64_t sys_pkg_write_stream(const char kp[SC_PATH_MAX], const void *udata, uint32_t len) {
    extern int g_root_ext2;
    if (!kp || !g_root_ext2 || kp[0] != '/') return -1;
    // /boot and /EFI live on the FAT ESP, never on ext2.
    if (kp[1]=='b'&&kp[2]=='o'&&kp[3]=='o'&&kp[4]=='t'&&(kp[5]=='/'||kp[5]==0)) return -1;
    if (kp[1]=='E'&&kp[2]=='F'&&kp[3]=='I'&&(kp[4]=='/'||kp[4]==0)) return -1;
    // Strip an explicit "/ext2" mount prefix (kernel-internal callers may pass it).
    const char *rel = kp;
    if (kp[1]=='e'&&kp[2]=='x'&&kp[3]=='t'&&kp[4]=='2'&&(kp[5]=='/'||kp[5]==0))
        rel = (kp[5]==0) ? "/" : (kp+5);
    uint32_t bs = ext2_block_size();
    if (!bs) return -1;
    ext2_wstream_t ws;
    if (ext2_wstream_begin(rel, &ws) != 0) return -1;
    uint8_t *sb = (uint8_t *)kmalloc(bs);
    if (!sb) { ext2_wstream_abort(&ws); return -1; }
    uint32_t off = 0; int rc = 0;
    while (off < len) {
        uint32_t take = (len - off < bs) ? (len - off) : bs;   // final chunk may be < bs
        if (copy_from_user(sb, (const uint8_t *)udata + off, take) != 0) { rc = -1; break; }
        if (ext2_wstream_block(&ws, sb, take) != 0) { rc = -1; break; }
        off += take;
    }
    kfree(sb);
    if (rc != 0) { ext2_wstream_abort(&ws); return -1; }
    return (ext2_wstream_finish(&ws) == 0) ? 0 : -1;
}


// ===========================================================================
// #739 SYS_DISKIMG: mount / eject / query a disk image on a DOS drive letter.
//
// ONE multiplexer rather than three syscalls because the three share one
// permission story and one table, and because a number is the scarcest thing
// in this file: 300-360 is contiguous and nothing in the build fails for a
// duplicate.
//
// PERMISSION. Mounting names a PATH and makes the kernel read it, which is the
// same shape as sys_print_image() (#700 B3), where the measured finding was
// that the kernel had already read /CONFIG/KIMI.KEY before the decode failed.
// Here the read would be worse than a failed decode: a mounted image is a
// browsable filesystem. So:
//
//   * the path is bounced into a KERNEL buffer first, and every later use is of
//     that copy. Checking the user copy and then opening the user copy again is
//     a time-of-check/time-of-use gap; rustkern/argtab.rs is explicit that its
//     entry validation is not TOCTOU-safe.
//   * perms_check(kpath, euid, egid, R_OK) must pass for a Ring-3 caller. Fail
//     closed: any non-zero answer refuses.
//   * drvmap_path_ok_rs() (called inside diskimg_mount_idx) rejects relative
//     paths, "..", backslashes and control characters, so the string
//     perms_check() approved and the string that gets opened are provably the
//     same string rather than arguably the same path.
//
// HONEST LIMIT, because it should be written down rather than discovered:
// perms_check()'s no-entry default is world-readable (fs/perms.c
// perms_check_leaf), so this gate means "the caller may read this file" and NOT
// "this file is not a secret". That is the right gate for the requirement (a
// user may mount what they may read), and /CONFIG is protected two ways (per
// file 0600 seeds plus the directory at 0711), but do not read more into it.
//
// EJECT AND QUERY take no path and no privilege beyond being able to call it:
// they only affect drives this machine's own user mounted, and query is the
// information the UI is for.
// ===========================================================================
#define DIMG_CMD_INFO   0   // letter -> *out
#define DIMG_CMD_MOUNT  1   // path, letter (or -1 auto) -> letter index
#define DIMG_CMD_EJECT  2   // letter -> 0
#define DIMG_CMD_MAX_MOUNTS 3   // -> DISKIMG_MAX_MOUNTS

static int64_t sys_diskimg(long cmd, const char *upath, void *uout, long letter) {
    switch (cmd) {
    case DIMG_CMD_MAX_MOUNTS:
        return DISKIMG_MAX_MOUNTS;

    case DIMG_CMD_INFO: {
        diskimg_info_t info;
        if (diskimg_query((int)letter, &info) != 0) return -1;
        if (!uout) return -1;
        // #567/#509: copy_to_user, never a direct deref. The argtab-361 entry
        // check proved the address at entry; only copy_to_user is atomic
        // check-and-use against a sibling thread remapping the page.
        if (copy_to_user(uout, &info, sizeof(info)) != 0) return -14;
        return 0;
    }

    case DIMG_CMD_MOUNT: {
        char kpath[SC_PATH_MAX];
        if (sc_bounce_str(upath, kpath, sizeof(kpath)) != 0) return -14;
        process_t *p = proc_current();
        if (p && p->privilege == PRIV_USER &&
            perms_check(kpath, p->euid, p->egid, R_OK) != 0) {
            kprintf("[diskimg] mount of %s refused: uid %u may not read it\n",
                    kpath, (unsigned)p->euid);
            return -13;   // EACCES. MOS_EACCES is #defined further down this file, after this handler.
        }
        int want = (letter < 0) ? DISKIMG_LETTER_AUTO : (int)letter;
        return diskimg_mount_idx(want, kpath);
    }

    case DIMG_CMD_EJECT: {
        if (letter < 0 || letter > 25) return -1;
        if (!diskimg_is_mounted((char)('A' + letter))) return -1;
        diskimg_eject_idx((int)letter);
        return 0;
    }

    default:
        return -1;
    }
}

int64_t syscall_dispatch(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5,
                         uint64_t arg6) {
    // #67 pass 5: tag the in-kernel reason for BKL hold attribution.
    { extern void bkl_set_reason(uint32_t r); bkl_set_reason(0x0200u | (uint32_t)(num & 0xFF)); }

    // #503 / MAYTERA-SEC-2026-0016: THE CHOKE POINT.
    //
    // Every user pointer this kernel is handed arrives here, once, before any
    // handler sees it. That placement is the entire design: #500 found 171
    // user-pointer arguments across 239 dispatcher cases and exactly five
    // validated call sites tree-wide, because a per-handler rule is a rule that
    // ~110 authors each had to remember and all but five forgot. Fixing the
    // handlers fixes tonight; the next syscall reopens the hole. Fixing the
    // dispatcher fixes the shape.
    //
    // Reachable ONLY from proc/syscall.asm, i.e. the SYSCALL instruction, i.e.
    // Ring 3. No kernel caller exists (kernel subsystems call the sys_* handlers
    // directly), so this cannot reject a legitimate kernel pointer. Verified by
    // grep across the tree before wiring: syscall.asm is the only caller.
    //
    // Deliberately NOT gated on proc_current()->privilege: gating would make a
    // syscall's validation depend on process state, which is the trust-a-flag
    // shape this task exists to remove. Ring 3 is Ring 3.
    //
    // A syscall with no descriptor is NOT validated and returns 0 here, so the
    // table rolls out incrementally. tools/syscall-ptr-lint holds the ledger of
    // what is still undeclared and fails the build if that set ever grows.
    {
        int64_t vrc = (int64_t)syscall_validate_args(num, arg1, arg2, arg3,
                                                     arg4, arg5, arg6);
        if (vrc != 0) {
            // #616: a silently-rejected syscall argument cost two separate
            // multi-hour investigations (#615, #616). Say so on serial, bounded
            // so a misbehaving app cannot flood the console.
            static int sysarg_reports = 0;
            if (sysarg_reports < 32) {
                sysarg_reports++;
                kprintf("[SYSARG] num=%lu REJECTED rc=%ld a1=%lx a2=%lx a3=%lx\n",
                        (unsigned long)num, (long)vrc, (unsigned long)arg1,
                        (unsigned long)arg2, (unsigned long)arg3);
            }

            /* #653: this is a REAL security event - a process handed the kernel
             * a pointer it is not allowed to name - so record it as one. It
             * reaches /CONFIG/SECURITY.LOG and, being a WARNING, raises a
             * desktop notification.
             *
             * Before this call security_audit() had ZERO callers anywhere in
             * the kernel: the audit ring, its severity classification and its
             * whole API were reachable-but-unreached. A sink with no producer
             * is decorative (#514/#624/#646 class), so this is the wiring that
             * makes the security log actually contain something.
             *
             * security_audit() is non-blocking by construction (ring + kprintf
             * + a wake), which is what makes it safe to call from a syscall
             * entry path. Do NOT put I/O here.
             *
             * Rate-limited on the SAME counter as the serial print: a process
             * spraying bad pointers must not be able to fill the security log
             * or the notification spool. That bound is the point. */
            if (sysarg_reports <= 32) {
                char sd[64];
                const char *pre = "syscall ";
                int di = 0;
                while (*pre) sd[di++] = *pre++;
                unsigned long n = (unsigned long)num;
                char nd[24]; int nl = 0;
                if (n == 0) nd[nl++] = '0';
                while (n > 0) { nd[nl++] = (char)('0' + (n % 10)); n /= 10; }
                while (nl > 0) sd[di++] = nd[--nl];
                const char *suf = " rejected: bad user pointer";
                while (*suf && di < (int)sizeof(sd) - 1) sd[di++] = *suf++;
                sd[di] = '\0';
                seclog_report_bad_user_ptr(
                    proc_current() ? (unsigned int)proc_current()->pid : 0u, sd);
            }
            return vrc;   /* -EFAULT */
        }
    }

    switch (num) {
#ifdef SECTEST_SYSCALL
        // #500 / #503 MAYTERA-SEC-2026-0016 negative-control battery. DEBUG
        // BUILDS ONLY, never shipped: it deliberately hands the validator and
        // the dispatcher argtab kernel addresses in order to prove they are
        // rejected. Shipping it would hand Ring 3 a syscall whose whole job is
        // to dereference attacker-named addresses.
        //
        // NOTE this case sits ABOVE the argtab check?  No - deliberately NOT.
        // It is inside the switch, so syscall_validate_args() has already run
        // on it. SYS_SECTEST has no descriptor, so it passes through
        // unvalidated, which is exactly what this test needs: it must receive
        // its ubuf pointer untouched in order to build the boundary cases.
        case SYS_SECTEST:
            return validate_selftest((void *)arg1, arg2);
#endif
        // Networking (TCP + ICMP)
        case SYS_SOCKET:
            return tcp_socket();
        case SYS_CONNECT:
            return tcp_connect_kcr3((int)arg1, (uint32_t)arg2, (uint16_t)arg3);
        case SYS_SEND:
            return tcp_send_kcr3((int)arg1, (const void *)arg2, (uint16_t)arg3);
        case SYS_RECV:
            return tcp_recv_kcr3((int)arg1, (void *)arg2, (uint16_t)arg3);
        case SYS_TCP_CLOSE:
            return tcp_close_kcr3((int)arg1);
        case SYS_TCP_STATE:
            return (int64_t)tcp_state_kcr3((int)arg1);
        case SYS_LISTEN:
            return tcp_listen_bind((int)arg1, (uint16_t)arg2, (int)arg3);
        case SYS_ACCEPT:
            return tcp_accept_kcr3((int)arg1);
        case SYS_PING:
            return sys_ping((uint32_t)arg1, (int)arg2);

        // #524 - BSD sockets (fd-integrated, blocking-capable). Pointer args are
        // validated by the argtab descriptors (rustkern/argtab.rs) before the
        // handler runs; handlers copy via copy_*_user. See net/socket.c.
        case SYS_SOCK_OPEN:
            return sys_sock_open((int)arg1, (int)arg2, (int)arg3);
        case SYS_SOCK_BIND:
            return sys_sock_bind((int)arg1, (const void *)arg2, (int)arg3);
        case SYS_SOCK_CONNECT:
            return sys_sock_connect((int)arg1, (const void *)arg2, (int)arg3);
        case SYS_SOCK_LISTEN:
            return sys_sock_listen((int)arg1, (int)arg2);
        case SYS_SOCK_ACCEPT:
            return sys_sock_accept((int)arg1, (void *)arg2, (void *)arg3);
        case SYS_SOCK_SEND:
            return sys_sock_send((int)arg1, (const void *)arg2, (uint64_t)arg3, (int)arg4);
        case SYS_SOCK_RECV:
            return sys_sock_recv((int)arg1, (void *)arg2, (uint64_t)arg3, (int)arg4);
        case SYS_SOCK_SENDTO:
            return sys_sock_sendto((int)arg1, (const void *)arg2, (uint64_t)arg3,
                                   (int)arg4, (const void *)arg5, (int)arg6);
        case SYS_SOCK_RECVFROM:
            return sys_sock_recvfrom((int)arg1, (void *)arg2, (uint64_t)arg3,
                                     (int)arg4, (void *)arg5, (void *)arg6);
        case SYS_SOCK_SETOPT:
            return sys_sock_setsockopt((int)arg1, (int)arg2, (int)arg3,
                                       (const void *)arg4, (int)arg5);
        case SYS_SOCK_GETOPT:
            return sys_sock_getsockopt((int)arg1, (int)arg2, (int)arg3,
                                       (void *)arg4, (void *)arg5);
        case SYS_SOCK_SELECT:
            return sys_sock_select((int)arg1, (void *)arg2, (void *)arg3,
                                   (void *)arg4, (void *)arg5);
        case SYS_SOCK_SHUTDOWN:
            return sys_sock_shutdown((int)arg1, (int)arg2);

        // Process control
        case SYS_EXIT:
            return sys_exit((int)arg1);
        case SYS_FORK:
            return sys_fork();
        case SYS_EXEC:
            return sys_exec((const char *)arg1);
        case SYS_SPAWN:
            return sys_spawn_args((const char *)arg1, NULL, 0);
        case SYS_SPAWN_ARGS:
            return sys_spawn_args((const char *)arg1, (char **)arg2, (int)arg3);
        case SYS_SPAWN_REDIR:
            return sys_spawn_redir((const char *)arg1, (char **)arg2, (int)arg3,
                                   (const char *)arg4, (const char *)arg5, (int)arg6);
        case SYS_WIN16_RUN:
            // Non-blocking launch in a dedicated kernel process (#144).
            return win16_launch((const char *)arg1);
        case SYS_DOS_RUN: {
            // Non-blocking MS-DOS launch in its own kernel proc + window (#208).
            // #692 (found in passing): this used to hand the raw Ring-3
            // pointer to dos_launch(), which dereferences it immediately, so
            // Ring 3 could point it at kernel memory. Bounce it fault-safe,
            // exactly as SYS_WIN16_RUN above already did.
            char kdospath[256];
            if (sc_bounce_str((const char *)arg1, kdospath, sizeof(kdospath)) != 0)
                return -1;
            if (!kdospath[0]) return -1;
            return dos_launch(kdospath);
        }
        case SYS_WIN16_ACTIVE: {
            // (#200 SkiFree) Report whether a Win16 app currently owns the
            // keyboard/foreground. The compositor uses this to treat a running
            // Win16 game as continuous activity so its idle screensaver does not
            // black out the game while the Win16 message pump is the sole key
            // consumer (SYS_GET_KEYBOARD returns -1 to the compositor in that
            // state, so the compositor would otherwise never see input).
            extern volatile int g_win16_owns_screen;
            return g_win16_owns_screen ? 1 : 0;
        }
        case SYS_WAIT:
            return (int64_t)proc_wait(-1, (int *)arg1);
        // #503: this was `case 98: /* SYS_WAITPID */`, a BARE NUMERIC label, and
        // that is why it is worth a comment. syscall-ptr-lint keys on
        // `case SYS_<NAME>:`, so a numeric label makes the case INVISIBLE to the
        // ledger: this syscall hands Ring 3 an `int *` out-param and was not in
        // the 109 the inventory counted, purely because of how its label was
        // spelled. The lint's unit of measurement was a naming convention. A
        // sweep of the dispatcher found exactly one such label out of 239; using
        // the name (which proc/syscall.h has always defined) is the fix, and the
        // count is now 110. Do not reintroduce a numeric case label here.
        case SYS_WAITPID:
            return (int64_t)proc_wait((int)arg1, (int *)arg2);
        case SYS_GETPID:
            return sys_getpid();
        case SYS_GETPPID:
            return sys_getppid();
        case SYS_YIELD:
            return sys_yield();
        case SYS_SLEEP:
            return sys_sleep((uint32_t)arg1);

        // File I/O
        case SYS_OPEN:
            return sys_open((const char *)arg1, (int)arg2);
        case SYS_FCNTL:
            return sys_fcntl((int)arg1, (int)arg2, (long)arg3);
        case SYS_CLOSE:
            return sys_close((int)arg1);
        case SYS_FSYNC:
            return sys_fsync((int)arg1);
        case SYS_READ:
            return sys_read((int)arg1, (void *)arg2, (size_t)arg3);
        case SYS_WRITE:
            return sys_write((int)arg1, (const void *)arg2, (size_t)arg3);
        case SYS_SEEK:
            return sys_seek((int)arg1, (int64_t)arg2, (int)arg3);
        case SYS_STAT:
            return sys_stat_path((const char *)arg1, (void *)arg2);
        case SYS_HTTP_FETCH:
            return sys_http_fetch((const char *)arg1, (char *)arg2, (uint32_t)arg3, (uint32_t *)arg4, (int *)arg5);
        case SYS_HTTP_FETCH_HDR:
            return sys_http_fetch_hdr((const char *)arg1, (const char *)arg2, (char *)arg3, (uint32_t)arg4, (uint32_t *)arg5, (int *)arg6);
        case SYS_HTTP_POST:
            return sys_http_post((const char *)arg1, (const char *)arg2, (const char *)arg3,
                                 (char *)arg4, (uint32_t)arg5, (int *)arg6);
        case SYS_HTTP_FETCH_START:
            return sys_http_fetch_start((const char *)arg1);
        case SYS_HTTP_FETCH_POLL:
            return sys_http_fetch_poll((int)arg1, (int *)arg2, (uint32_t *)arg3);
        case SYS_HTTP_FETCH_READ:
            return sys_http_fetch_read((int)arg1, (char *)arg2, (uint32_t)arg3);
        case SYS_HTTP_FETCH_CANCEL:
            return sys_http_fetch_cancel((int)arg1);
        case SYS_HTTP_FETCH_PROGRESS:
            return sys_http_fetch_progress((int)arg1, (int *)arg2, (uint32_t *)arg3, (uint32_t *)arg4);
        case SYS_HTTP_POST_START:
            return sys_http_post_start((const char *)arg1, (const char *)arg2, (const char *)arg3);
        case SYS_AI_SCAN:   // #745
            return sys_ai_scan((const char *)arg1, (void *)arg2);
        case SYS_HTTP_POST_POLL:
            return sys_http_post_poll((int)arg1, (int *)arg2, (uint32_t *)arg3);
        case SYS_HTTP_POST_READ:
            return sys_http_post_read((int)arg1, (char *)arg2, (uint32_t)arg3);
        case SYS_HTTP_POST_CANCEL:
            return sys_http_post_cancel((int)arg1);

        // Directory / filesystem ops
        case SYS_MKDIR:
            return sys_mkdir((const char *)arg1, (int)arg2);
        case SYS_RMDIR:
            return sys_rmdir((const char *)arg1);
        case SYS_UNLINK:
            return sys_unlink((const char *)arg1);
        case SYS_READDIR:
            return sys_readdir((int)arg1, (void *)arg2);
        case SYS_RENAME:
            return sys_rename((const char *)arg1, (const char *)arg2);

        // POSIX process groups / sessions (#745 local 82).
        case SYS_SETSID:
            return sys_setsid();
        case SYS_SETPGID:
            return sys_setpgid((int64_t)arg1, (int64_t)arg2);
        case SYS_GETPGID:
            return sys_getpgid((int64_t)arg1);
        case SYS_GETSID:
            return sys_getsid((int64_t)arg1);
        // POSIX poll(2) (#745 local 82). The handler is Rust.
        case SYS_POLL:
            return sys_poll_rs((void *)arg1, (uint64_t)arg2, (int64_t)arg3);

        case SYS_GETCWD:
            return sys_getcwd((char *)arg1, (uint64_t)arg2);
        case SYS_CHDIR:
            return sys_chdir((const char *)arg1);

        // Memory
        case SYS_BRK:
            return sys_brk(arg1);
        case SYS_MMAP:
            return sys_mmap(arg1, arg2, (int)arg3, (int)arg4);
        case SYS_MUNMAP:
            return sys_munmap(arg1, arg2);

        // Console
        case SYS_PUTCHAR:
            return sys_putchar((int)arg1);
        case SYS_GETCHAR:
            return sys_getchar();

        // #739 disk images
        case SYS_DISKIMG:
            return sys_diskimg((long)arg1, (const char *)arg2,
                               (void *)arg3, (long)arg4);

        // Time
        case SYS_GFS_VERIFY: {
            // #711: verify the GraphFS journal against its seal AND against
            // the kernel's own in-memory chain head. arg1 is validated by
            // syscall_validate_args() (rustkern/argtab.rs descriptor 360,
            // 80 writable bytes) before we get here.
            gfsj_verify_t v;
            int grc = gfs_journal_verify(&v);
            if (!arg1) return -1;
            // #567/#509: bounce through copy_to_user, never a direct deref of a
            // user pointer. The entry check (argtab 360) proves the address was
            // valid AT ENTRY; only copy_to_user is atomic check-and-use, and a
            // sibling thread can remap the page in between.
            if (copy_to_user((void *)arg1, &v, sizeof(v)) != 0) return -14;
            return grc;
        }
        case SYS_TIME:
            return sys_time();
        case SYS_GFS_QUERY:
            // #711 slice 2: READ-ONLY view of the contract graph. There is
            // deliberately no mutating counterpart (design section 8). arg4 is
            // validated by syscall_validate_args() (rustkern/argtab.rs
            // descriptor 363: arg5 writable bytes) before we get here, and the
            // handler bounces its result through copy_to_user, which is the
            // only atomic check-and-use.
            return sys_gfs_query((int)arg1, (uint32_t)arg2, (uint32_t)arg3,
                                 (void *)arg4, (int)arg5, (uint32_t)arg6);
        case SYS_GET_TICKS:
            return (int64_t)timer_ticks;   // 250Hz monotonic ticks (4ms each)
        case SYS_UPTIME_MS: {
            // Monotonic milliseconds since boot. The kernel is the single
            // authority on the tick rate (g_timer_hz), so userland never has
            // to divide raw ticks by a guessed frequency.
            extern uint32_t g_timer_hz;
            uint32_t _hz = g_timer_hz ? g_timer_hz : 250;
            return (int64_t)((uint64_t)timer_ticks * 1000ULL / _hz);
        }
        case SYS_FSCK: {
            // #610: the on-device filesystem checker, exposed to userland.
            // READ-ONLY. It scans the whole ext2 metadata, which costs real
            // I/O, so it is a deliberate user action (the Terminal `fsck`
            // command), never something an app does in a loop.
            extern int      ext2_is_mounted(void);
            // (declared in fs/ext2.h, included above)
            extern uint32_t ext2_fsck_needed(void);
            extern uint16_t ext2_state_at_mount(void);
            extern uint16_t ext2_mnt_count(void);
            void *ub = (void *)arg1;
            uint32_t ulen = (uint32_t)arg2;
            int mode = (int)arg3;
            if (!ub || ulen < 200) return -1;
            if (!ext2_is_mounted()) return -19;
            if (mode == 1) {
                // State-only query: no scan, no I/O beyond what mount cached.
                return (int64_t)((uint32_t)ext2_state_at_mount()
                                 | ((uint32_t)ext2_mnt_count() << 8)
                                 | (ext2_fsck_needed() << 24));
            }
            // Build in kernel memory then ONE copy_to_user (#509 TOCTOU-safe):
            // never let the checker write through a user pointer.
            uint8_t *krep = (uint8_t *)kmalloc(200);
            if (!krep) return -12;
            int rc = ext2_fsck_run((ext2_fsck_report_t *)krep);
            if (rc == 0 && copy_to_user(ub, krep, 200) != 0) rc = -14;
            kfree(krep);
            return rc;
        }
        case SYS_GET_VERSION: {
            // Copy "X.Y.Z (build N)" into the user buffer (arg1=buf, arg2=len).
            #define SYSV_S2(x) #x
            #define SYSV_S(x)  SYSV_S2(x)
            static const char *v = MAYTERA_VERSION_STRING " (build " SYSV_S(MAYTERA_BUILD_NUMBER) ")";
            char *ub = (char *)arg1; int ulen = (int)arg2;
            if (!ub || ulen <= 0) return -1;
            // #566: build in a kernel buffer then copy_to_user once (#509
            // TOCTOU-safe) instead of writing ub[i] through the user pointer.
            char vkbuf[96];
            int i = 0; while (v[i] && i < ulen - 1 && i < (int)sizeof(vkbuf) - 1) { vkbuf[i] = v[i]; i++; }
            vkbuf[i] = 0;
            if (copy_to_user(ub, vkbuf, (size_t)i + 1) != 0) return -14;
            return i;
        }
        case SYS_CLOCK:
            return sys_clock();

        // Window/Graphics syscalls
        case SYS_WIN_CREATE:
            return sys_win_create((const char *)arg1, (int)arg2, (int)arg3, (int)arg4, (int)arg5);
        case SYS_WIN_DESTROY:
            return sys_win_destroy((int)arg1);
        case SYS_WIN_DRAW_RECT:
            return sys_win_draw_rect((int)arg1, (int)arg2, (int)arg3, (int)arg4, (int)arg5, (uint32_t)arg6);
        case SYS_WIN_DRAW_TEXT:
            return sys_win_draw_text((int)arg1, (int)arg2, (int)arg3, (const char *)arg4, (uint32_t)arg5);
        case SYS_WIN_DRAW_TEXT_SMALL:
            return sys_win_draw_text_small((int)arg1, (int)arg2, (int)arg3, (const char *)arg4, (uint32_t)arg5);
        case SYS_WIN_DRAW_TTF:
            // size is packed into the top byte of the color argument (RGB is 24-bit).
            return sys_win_draw_text_ttf((int)arg1, (int)arg2, (int)arg3, (const char *)arg4,
                                         (uint32_t)arg5 & 0xFFFFFF,
                                         (int)(((uint32_t)arg5 >> 24) & 0xFF));
        case SYS_WIN_DRAW_PIXEL:
            return sys_win_draw_pixel((int)arg1, (int)arg2, (int)arg3, (uint32_t)arg4);
        case SYS_WIN_BLIT: {
            int src_w = (int)(arg4 & 0xFFFF);
            int src_h = (int)((arg4 >> 16) & 0xFFFF);
            return sys_win_blit((int)arg1, (int)arg2, (int)arg3, src_w, src_h, (uint32_t *)arg5);
        }
        case SYS_WIN_GET_EVENT:
            return sys_win_get_event((int)arg1, (void *)arg2, (int)arg3);
        case SYS_WIN_INVALIDATE:
            return sys_win_invalidate((int)arg1);
        case SYS_WIN_GET_SIZE:
            return sys_win_get_size((int)arg1, (int *)arg2, (int *)arg3);

        // User identity
        case SYS_GETUID:
            return sys_getuid();
        case SYS_SETUID:
            return sys_setuid((uint32_t)arg1);
        case SYS_GETGID:
            return sys_getgid();
        case SYS_SETGID:
            return sys_setgid((uint32_t)arg1);
        case SYS_GETEUID:
            return sys_geteuid();
        case SYS_GETEGID:
            return sys_getegid();
        case SYS_SETEUID:
            return sys_seteuid((uint32_t)arg1);
        case SYS_SETEGID:
            return sys_setegid((uint32_t)arg1);
        case SYS_CHMOD:
            return sys_chmod((const char *)arg1, (uint16_t)arg2);
        case SYS_CHOWN:
            return sys_chown((const char *)arg1, (uint32_t)arg2, (uint32_t)arg3);
        case SYS_FS_PERM_INFO:
            return sys_fs_perm_info((const char *)arg1, (int)arg2, (void *)arg3);
        case SYS_THEME_LOAD_FILE:
            return sys_theme_load_file((const char *)arg1);
        case SYS_THEME_CONTRAST_CORRECTIONS:
            return sys_theme_contrast_corrections((int64_t)arg1);
        case SYS_PASSWD_CHANGE:
            return sys_passwd_change((const char *)arg1, (const char *)arg2, (const char *)arg3);
        case SYS_SU:
            return sys_su((const char *)arg1, (const char *)arg2);
        case SYS_ADDUSER:
            return sys_adduser((const char *)arg1, (uint32_t)arg2, (uint32_t)arg3,
                              (const char *)arg4, (const char *)arg5);
        case SYS_USER_CREATE_PW:
            return sys_user_create_pw((const char *)arg1, (const char *)arg2,
                                      (uint32_t)arg3, (uint32_t)arg4,
                                      (const char *)arg5);
        case SYS_PW_CHECK:
            return sys_pw_check((const char *)arg1, (const char *)arg2);
        case SYS_FIRSTBOOT_ADMIN:
            return sys_firstboot_admin((const char *)arg1, (const char *)arg2,
                                       (const char *)arg3);
        case SYS_INST_ENUM:
            return sys_inst_enum((void *)arg1, (int)arg2, (int)arg3);
        case SYS_INST_INSTALL:
            return sys_inst_install((int)arg1, (int)arg2);
        case SYS_SET_THEME:
            return sys_set_theme((int)arg1);
        case SYS_GET_THEME:
            return sys_get_theme();
        case SYS_THEME_COLOR: {
            extern uint32_t theme_get_color_by_id(int theme_id, int color_id);
            return (int64_t)(uint32_t)theme_get_color_by_id((int)arg1, (int)arg2);
        }
        // (#711) mtheme v2: the same live theme table, read as an INTEGER.
        // Colours already came from the file via SYS_THEME_COLOR; this is the
        // metric/radius/decor/type half, so a userland widget's geometry is
        // data too. Bounds-checked kernel-side (theme_get_metric_by_id).
        case SYS_THEME_METRIC: {
            extern int32_t theme_get_metric_by_id(int theme_id, int metric_id);
            return (int64_t)theme_get_metric_by_id((int)arg1, (int)arg2);
        }
        // (#704) See sys_wm_force_redraw_all() for why this exists: neither an
        // explicit theme switch nor a live .mtheme file reload today reaches
        // an already-open app's EVENT_REDRAW handler, so its content area
        // keeps showing stale colours until the window is resized or
        // recreated. Called by the compositor only, edge-triggered on an
        // actual theme change.
        case SYS_WM_FORCE_REDRAW_ALL:
            return sys_wm_force_redraw_all();
        case SYS_PRINT_LIST:
            return sys_print_list((void *)arg1, (int)arg2);
        case SYS_PRINT_JOB:
            return sys_print_job((const char *)arg1, (const char *)arg2, (const char *)arg3);
        case SYS_PRINT_ADD:
            return sys_print_add((const char *)arg1, (const char *)arg2, (int)arg3,
                                 (const char *)arg4, (int)arg5);
        case SYS_PRINT_REMOVE:
            return sys_print_remove((const char *)arg1);
        case SYS_PRINT_IMAGE:
            return sys_print_image((const char *)arg1, (const char *)arg2);
        case SYS_PRINT_SCREEN:
            return sys_print_screen((const char *)arg1);
        case SYS_BOOTLOG_WRITE:
            return sys_bootlog_write((const char *)arg1);
        case SYS_SET_VOLUME:
            return sys_set_volume((int)arg1);
        case SYS_GET_VOLUME:
            return sys_get_volume();
        case SYS_SET_MUTE:
            return sys_set_mute((int)arg1);
        case SYS_GET_DISK_TOTAL:
            return sys_get_disk_total();
        case SYS_GET_DISK_FREE:
            return sys_get_disk_free();
        case SYS_SET_MOUSE_SPEED:
            return sys_set_mouse_speed((int)arg1);
        case SYS_GET_MOUSE_SPEED:
            return sys_get_mouse_speed();
        case SYS_GET_RTC_TIME:
            return sys_get_rtc_time();
        case SYS_GET_RTC_DATE:
            return sys_get_rtc_date();
        case SYS_SET_RTC_TIME: return sys_set_rtc_time(arg1);
        case SYS_SET_RTC_DATE: return sys_set_rtc_date(arg1);
        case SYS_GET_NET_INFO: return sys_get_net_info((void *)arg1, arg2);
        case SYS_NET_SET_STATIC: return sys_net_set_static((const char *)arg1, (const char *)arg2, (const char *)arg3);
        case SYS_NET_DHCP: { extern int dhcp_discover_blocking(void); extern void net_clear_fault(void); net_clear_fault(); return (int64_t)dhcp_discover_blocking(); }  // #549: renew/reconnect clears fault
        case SYS_NET_IS_UP: { extern int net_is_up(void); return (int64_t)net_is_up(); }  // #374
        // (#745) Structured live IPv4 status. The Rust builder fills a
        // KERNEL-LOCAL net_status_t and this does the single fault-safe
        // copy_to_user, so no user pointer is dereferenced inside the
        // builder. arg1 is described by Desc { num: 369 } in
        // rustkern/argtab.rs, so syscall_validate_args() has already proven
        // it spans 48 writable USER bytes before we get here.
        case SYS_NET_STATUS: {
            net_status_t kns;
            // Parameter left UNNAMED deliberately: smap-uaccess-lint's B3
            // scan keys on identifiers, and a declaration parameter named
            // `out` reads to it as a Ring-0 deref of a user pointer. There
            // is no user pointer here at all - the builder writes `kns`,
            // which is on this kernel stack frame.
            extern int net_status_build_rs(void *);
            if (net_status_build_rs(&kns) != 0) return -1;
            if (copy_to_user((void *)arg1, &kns, sizeof(kns)) != 0) return -14;
            return 0;
        }
        // (#745) Non-blocking probe (ICMP echo start/poll/cancel, DHCP
        // restart). No pointer arguments; see NET_PROBE_* in syscall.h.
        case SYS_NET_PROBE: {
            extern int64_t net_probe_rs(int op, uint64_t arg);
            return net_probe_rs((int)arg1, (uint64_t)arg2);
        }
        // #745 ELEVATION. Bodies in proc/elevate.c; the trust story is in
        // proc/elevate.h. Nothing here decides anything.
        case SYS_ELEV_REQUEST:
            return sys_elev_request((const elev_request_t *)arg1);
        case SYS_ELEV_STATUS:
            return sys_elev_status((uint64_t)arg1);
        case SYS_ELEV_VIEW:
            return sys_elev_view((elev_view_t *)arg1);
        case SYS_ELEV_RESOLVE: {
            // The credential is bounced HERE, fault-safe, before the
            // authenticator can ever read Ring-3 memory, and the stack copy is
            // zeroed on the way out whatever the verdict was.
            char epw[SC_PASSWORD_MAX];
            memset(epw, 0, sizeof(epw));
            if ((int)arg2 == ELEV_ACT_SUBMIT &&
                sc_bounce_str((const char *)arg3, epw, sizeof(epw)) != 0)
                return ELEV_EARG;
            int64_t er = sys_elev_resolve((uint64_t)arg1, (int)arg2, epw);
            memset(epw, 0, sizeof(epw));
            return er;
        }
        case SYS_ELEV_MAY:
            return sys_elev_may();
        case SYS_DESKTOP_MENU_RELOAD: { extern void desktop_menu_reload(void); desktop_menu_reload(); return 0; }  // #402
        case SYS_KERNEL_SELFUPDATE: {  // #492 Stage 1b: authenticated brick-safe self-update
            extern int kernel_selfupdate_apply(const void *, uint32_t,
                                               const uint8_t *, uint32_t,
                                               const uint8_t *, uint32_t);
            // PRIVILEGE GATE: only a registered service holding SVC_PERM_SELFUPDATE
            // may install a kernel. Arbitrary Ring-3 apps (is_service==0) are
            // refused outright, so the primitive is unreachable from ordinary
            // user code. The mandatory RSA signature check inside the primitive is
            // the second, cryptographic gate.
            process_t *ku_cur = proc_current();
            if (!ku_cur || !ku_cur->is_service ||
                !(ku_cur->svc_perms & SVC_PERM_SELFUPDATE)) {
                return (int64_t)(-11) /* SELFUPD_ERR_PERM */;
            }
            uint32_t ku_len = (uint32_t)arg2;
            uint32_t ku_build = (uint32_t)arg4;
            uint32_t ku_sig_len = (uint32_t)arg6;
            // #19/#645: kernel_selfupdate_apply() hashes and writes the image
            // through the pointers it is given, and it has a KERNEL caller too
            // (selfupdate.c's boot resume path), so the bounce belongs here at
            // the Ring-3 boundary. It also removes a check-and-use race on the
            // one input whose integrity the whole brick-safe contract rests on:
            // the image the signature is verified over is now the image that
            // gets written, because Ring 3 can no longer change it in between.
            if (ku_len == 0 || ku_len > (32u * 1024u * 1024u)) return (int64_t)(-1);
            if (ku_sig_len == 0 || ku_sig_len > 512u) return (int64_t)(-1);
            uint8_t ku_ksha[32], ku_ksig[512];
            if (copy_from_user(ku_ksha, (const void *)arg3, sizeof(ku_ksha)) != 0)
                return (int64_t)(-1);
            if (copy_from_user(ku_ksig, (const void *)arg5, ku_sig_len) != 0)
                return (int64_t)(-1);
            void *ku_kimg = kmalloc(ku_len);
            if (!ku_kimg) return (int64_t)(-1);
            if (copy_from_user(ku_kimg, (const void *)arg1, ku_len) != 0) {
                kfree(ku_kimg); return (int64_t)(-1);
            }
            int ku_rc = kernel_selfupdate_apply(ku_kimg, ku_len, ku_ksha, ku_build,
                                                ku_ksig, ku_sig_len);
            kfree(ku_kimg);
            return (int64_t)ku_rc;
        }
        case SYS_OTA_VERIFY_SIG: {  // #492 Stage 1b: authenticate a signed manifest
            // Read-only signature check against the baked-in OTA pubkey. Safe for
            // any caller (no privilege needed): it grants no capability, it only
            // tells the client whether a manifest is authentic before it acts.
            extern int kernel_ota_verify_sig(const uint8_t *, const uint8_t *, uint32_t);
            // #566: a raced read of the digest or signature would corrupt a trust
            // decision, so bounce BOTH into kernel buffers via copy_from_user
            // (#509 atomic check-and-use) before the crypto ever reads them.
            uint32_t ov_len = (uint32_t)arg3;
            if (ov_len == 0 || ov_len > 512) return -1;
            uint8_t ov_dig[32], ov_sig[512];
            if (copy_from_user(ov_dig, (const void *)arg1, sizeof(ov_dig)) != 0) return -1;
            if (copy_from_user(ov_sig, (const void *)arg2, ov_len) != 0) return -1;
            return (int64_t)kernel_ota_verify_sig(ov_dig, ov_sig, ov_len);
        }
        case SYS_APP_VERIFY_SIG: {  // #563 key split: authenticate a signed APP manifest
            // Domain-separated from SYS_OTA_VERIFY_SIG: checks the baked-in APP
            // key (kernel_app_verify_sig), which cannot authorize a kernel image.
            // Read-only, grants no capability, so it needs no privilege.
            // RESTORED: this case was silently dropped by 68199dd (#565 theme
            // loader), which defeated the #563 app/kernel signing-key split
            // (calls fell through, so the App Store client used the OTA-key
            // fallback and the dedicated app-key path never ran).
            extern int kernel_app_verify_sig(const uint8_t *, const uint8_t *, uint32_t);
            // #566: bounce the digest + signature into kernel buffers via
            // copy_from_user (#509 atomic check-and-use) before the app-key crypto
            // reads them; a raced read here would corrupt an app-trust decision.
            uint32_t av_len = (uint32_t)arg3;
            if (av_len == 0 || av_len > 512) return -1;
            uint8_t av_dig[32], av_sig[512];
            if (copy_from_user(av_dig, (const void *)arg1, sizeof(av_dig)) != 0) return -1;
            if (copy_from_user(av_sig, (const void *)arg2, av_len) != 0) return -1;
            return (int64_t)kernel_app_verify_sig(av_dig, av_sig, av_len);
        }
        case SYS_PKG_WRITE: {  // #402 package manager writes to the FAT ESP (/APPS ...)
            extern fat_fs_t g_fat_fs;
            const char *pw_path = (const char *)arg1; const void *pw_data = (const void *)arg2;
            uint32_t pw_len = (uint32_t)arg3;
            if (!pw_path || (!pw_data && pw_len)) return -1;

            // #700 B1: bounce the PATH into the kernel before anything looks at
            // it. The buffered path below used to hand the caller's raw pointer
            // straight to fat_write_file(), so the filesystem walked a Ring-3
            // string that the caller could unmap or rewrite between the check
            // and the use. That is the same #509 check-and-use hazard the data
            // buffer was already protected against.
            char pw_kp[SC_PATH_MAX];
            if (strncpy_from_user(pw_kp, pw_path, sizeof(pw_kp)) < 0) return -14;

            // #700 B1: ONE authorization decision, before EITHER writer.
            if (pkg_write_permit(pw_kp) != 0) return -1;

            // #689: is the payload an executable? Decided from the first four
            // bytes of the CALLER'S data (copied in, never dereferenced in
            // place), not from the path, because "under /APPS" is a convention
            // and ELF magic is a fact.
            int pw_is_elf = 0;
            {
                uint8_t pw_hdr[4];
                if (pw_len >= 4 && copy_from_user(pw_hdr, pw_data, 4) == 0)
                    pw_is_elf = (pw_hdr[0] == 0x7F && pw_hdr[1] == 'E' &&
                                 pw_hdr[2] == 'L'  && pw_hdr[3] == 'F');
            }

            // #572: large ext2-root payloads stream to disk in bounded chunks
            // (no whole-file kmalloc). Falls back to the buffered path below when
            // the target is not on ext2 or the payload is small.
            if (pw_len > EXT2_WSPILL_BYTES &&
                sys_pkg_write_stream(pw_kp, pw_data, pw_len) == 0) {
                pkg_write_stamp(pw_kp, pw_is_elf);
                return 0;
            }
            void *pw_kb = kmalloc(pw_len ? pw_len : 1);
            if (!pw_kb) return -1;
            // #566: copy_from_user (#509 atomic check-and-use) instead of a plain
            // memcpy from the user pointer, so a racing unmap of the package data
            // faults to EFAULT rather than a Ring-0 read of a freed frame.
            if (pw_len && copy_from_user(pw_kb, pw_data, pw_len) != 0) { kfree(pw_kb); return -14; }
            int pw_rc = fat_write_file(&g_fat_fs, pw_kp, pw_kb, pw_len);
            kfree(pw_kb);
            if (pw_rc >= 0) pkg_write_stamp(pw_kp, pw_is_elf);
            return pw_rc;
        }
        case SYS_NET_MOUNT:
            return sys_net_mount((const char *)arg1, (const char *)arg2,
                                 (const char *)arg3, (const char *)arg4);
        case SYS_NET_LIST_SHARES:
            return sys_net_list_shares((const char *)arg1, (char *)arg2, (uint32_t)arg3);
        case SYS_NET_UNMOUNT:
            return sys_net_unmount((const char *)arg1, (const char *)arg2);
        case SYS_GET_DISK_INFO: return sys_get_disk_info((int)arg1, (void *)arg2);
        case SYS_NTP_SYNC:     return sys_ntp_sync();
        case SYS_NTP_SYNC_SERVER:
            return sys_ntp_sync_server((const char *)arg1, (uint32_t)arg2);
        case SYS_SET_CURSOR_THEME: {
            extern void cursor_set_theme(int theme);
            cursor_set_theme((int)arg1);
            return 0;
        }
        case SYS_GET_CURSOR_THEME: {
            extern int cursor_get_theme(void);
            return (int64_t)cursor_get_theme();
        }
        case SYS_SET_WALLPAPER:
            g_wallpaper_idx = (int)arg1;
            return 0;
        case SYS_GET_WALLPAPER:
            return (int64_t)g_wallpaper_idx;
        case SYS_SET_ICON_SIZE:
            g_icon_size = (int)arg1;
            return 0;
        case SYS_GET_ICON_SIZE:
            return (int64_t)g_icon_size;
        case SYS_SET_DISPLAY_FX:
            g_brightness = (int)arg1;
            g_nightlight = (int)arg2;
            if (g_brightness < 0) g_brightness = 0;
            if (g_brightness > 100) g_brightness = 100;
            if (g_nightlight < 0) g_nightlight = 0;
            if (g_nightlight > 100) g_nightlight = 100;
            return 0;
        case SYS_GET_DISPLAY_FX:
            return (int64_t)(g_brightness | (g_nightlight << 8));
        case SYS_DRAW_TTF: {
            extern void ttf_draw_string(int, int, const char *, int, unsigned int);
            ttf_draw_string((int)arg1, (int)arg2, (const char *)arg3, (int)arg4, (unsigned int)arg5);
            return 0;
        }
        case SYS_MEASURE_TTF: {
            extern int ttf_measure_string(const char *, int);
            return (int64_t)ttf_measure_string((const char *)arg1, (int)arg2);
        }
        case SYS_SET_FONT_SIZE:
            g_font_size = (int)arg1;
            if (g_font_size < 0) g_font_size = 0;
            if (g_font_size > 3) g_font_size = 3;
            return 0;
        case SYS_GET_FONT_SIZE:
            return (int64_t)g_font_size;

        // ---- OS-wide font registry (multi-face TrueType) ----
        case SYS_FONT_COUNT: {
            extern int ttf_face_count(void);
            return (int64_t)ttf_face_count();
        }
        case SYS_FONT_NAME: {
            extern int ttf_face_name(int, char *, int);
            char *ub = (char *)arg2; int cap = (int)arg3;
            if (!ub || cap <= 0) return -1;
            // #19/#645: ttf_face_name() writes the name straight through the
            // Ring-3 pointer. Build in a kernel buffer, then ONE copy_to_user.
            char fnk[128];
            if (cap > (int)sizeof(fnk)) cap = (int)sizeof(fnk);
            int fnr = ttf_face_name((int)arg1, fnk, cap);
            if (fnr < 0) return (int64_t)fnr;
            if (copy_to_user(ub, fnk, (size_t)cap) != 0) return -14;
            return (int64_t)fnr;
        }
        case SYS_FONT_SET_UI: {
            // System-wide UI font. Every legacy (non-_f) text path draws with
            // the active face, so this restyles the whole desktop live.
            extern int ttf_set_active_face(int);
            return (int64_t)ttf_set_active_face((int)arg1);
        }
        case SYS_FONT_GET_UI: {
            extern int ttf_get_active_face(void);
            return (int64_t)ttf_get_active_face();
        }
        case SYS_FONT_STYLE: {
            extern int ttf_face_style(int, char *, int);
            char *ub = (char *)arg2; int cap = (int)arg3;
            if (!ub || cap <= 0) return -1;
            // #19/#645: same shape as SYS_FONT_NAME above.
            char fsk[128];
            if (cap > (int)sizeof(fsk)) cap = (int)sizeof(fsk);
            int fsr = ttf_face_style((int)arg1, fsk, cap);
            if (fsr < 0) return (int64_t)fsr;
            if (copy_to_user(ub, fsk, (size_t)cap) != 0) return -14;
            return (int64_t)fsr;
        }
        case SYS_FONT_FIND: {
            extern int ttf_face_by_path(const char *);
            const char *up = (const char *)arg1;
            if (!up) return -1;
            // #19/#645: the callee walks the Ring-3 string with plain loads.
            char fpk[SC_PATH_MAX];
            if (strncpy_from_user(fpk, up, sizeof(fpk)) < 0) return -14;
            return (int64_t)ttf_face_by_path(fpk);
        }
        // #542 OS-wide system clipboard. Kernel-held bounded store so ANY Ring-3
        // app can copy/paste across apps. Backing buffer + all length clamping
        // live in Rust (rustkern/clipboard.rs); the copy is bounded there.
        case SYS_CLIP_SET: {
            extern long clip_set_rs(const unsigned char *src, unsigned long len);
            // #19/#645: clip_set_rs() is this path's copy engine and does its
            // own length clamping in Rust, so the AC window is around the copy
            // engine, which is where mm/uaccess.asm puts its own.
            uaccess_ac_t __ac = uaccess_begin();
            int64_t __r = (int64_t)clip_set_rs((const unsigned char *)arg1,
                                               (unsigned long)arg2);
            uaccess_end(__ac);
            return __r;
        }
        case SYS_CLIP_GET: {
            extern long clip_get_rs(unsigned char *dst, unsigned long cap);
            // #19/#645: see SYS_CLIP_SET.
            uaccess_ac_t __ac = uaccess_begin();
            int64_t __r = (int64_t)clip_get_rs((unsigned char *)arg1,
                                               (unsigned long)arg2);
            uaccess_end(__ac);
            return __r;
        }
        case SYS_CLIP_LEN: {
            extern long clip_len_rs(void);
            return (int64_t)clip_len_rs();
        }
        case SYS_FONT_RESCAN: {
            extern int ttf_rescan(void);
            return (int64_t)ttf_rescan();
        }
        case SYS_FONT_REMOVE: {
            extern int ttf_face_remove(int);
            return (int64_t)ttf_face_remove((int)arg1);
        }
        case SYS_FONT_GLYPH: {
            extern ttf_glyph_t *ttf_get_glyph_f(int, int, int, int);
            int face  = (int)(arg1 & 0xFF);
            int size  = (int)((arg1 >> 8) & 0xFFFF);
            int style = (int)((arg1 >> 24) & 0xFF);
            if (size < 4) size = 4;
            if (size > 128) size = 128;
            int *meta = (int *)arg3;
            uint8_t *ubmp = (uint8_t *)arg4;
            int cap = (int)arg5;
            ttf_glyph_t *g = ttf_get_glyph_f(face, (int)arg2, size, style);
            if (!g) return -1;
            // #566: copy_to_user the metadata + bitmap (#509 TOCTOU-safe) instead
            // of writing meta[]/ubmp directly through the user pointers.
            if (meta) {
                int mk[5] = { g->width, g->height, g->xoff, g->yoff, g->advance };
                if (copy_to_user(meta, mk, sizeof(mk)) != 0) return -14;
            }
            if (g->bitmap && ubmp && cap >= g->width * g->height && g->width > 0 && g->height > 0) {
                if (copy_to_user(ubmp, g->bitmap, (size_t)g->width * g->height) != 0) return -14;
            }
            return (int64_t)g->advance;
        }
        case SYS_FONT_METRICS: {
            extern void ttf_get_metrics_f(int, int, int *, int *, int *);
            int face = (int)(arg1 & 0xFF);
            int size = (int)((arg1 >> 8) & 0xFFFF);
            if (size < 4) size = 4;
            if (size > 128) size = 128;
            int *out = (int *)arg2;
            if (!out) return -1;
            // #566: fill a kernel buffer then copy_to_user (#509 TOCTOU-safe)
            // rather than letting ttf_get_metrics_f write the user pointer.
            int mk[3] = {0, 0, 0};
            ttf_get_metrics_f(face, size, &mk[0], &mk[1], &mk[2]);
            if (copy_to_user(out, mk, sizeof(mk)) != 0) return -14;
            return 0;
        }
        case SYS_FONT_KERN: {
            extern int ttf_get_kerning_f(int, int, int, int);
            int face = (int)(arg1 & 0xFF);
            int size = (int)((arg1 >> 8) & 0xFFFF);
            if (size < 4) size = 4;
            if (size > 128) size = 128;
            return (int64_t)ttf_get_kerning_f(face, (int)arg2, (int)arg3, size);
        }
        case SYS_WIN_DRAW_TTF_EX: {
            int x = (int)(arg2 & 0xFFFF);
            int y = (int)((arg2 >> 16) & 0xFFFF);
            int face  = (int)(arg4 & 0xFF);
            int size  = (int)((arg4 >> 8) & 0xFFFF);
            int style = (int)((arg4 >> 24) & 0xFF);
            return sys_win_draw_text_ttf_ex((int)arg1, x, y, (const char *)arg3,
                                            (uint32_t)arg5, size, face, style);
        }
        case SYS_SET_WIN_BLANK:
            g_win_blit_suppressed = (int)arg1;
            return 0;
        case SYS_SET_WIN_OPACITY: {
            extern void wm_set_default_opacity(int opacity);
            wm_set_default_opacity((int)arg1);
            return 0;
        }
        case SYS_GET_WIN_OPACITY: {
            extern int wm_get_default_opacity(void);
            return (int64_t)wm_get_default_opacity();
        }
        case SYS_SET_CURSOR:   // (#116) arg1=style, arg2=size%
            g_cursor_style = (int)arg1;
            g_cursor_size  = (int)arg2;
            return 0;
        case SYS_GET_CURSOR:   // packed: style (low 8) | size<<8
            return (int64_t)((g_cursor_style & 0xFF) | ((g_cursor_size & 0xFFFF) << 8));
        case SYS_SET_SCREENSAVER:
            g_screensaver_type = (int)arg1;
            return 0;
        case SYS_GET_SCREENSAVER:
            return (int64_t)g_screensaver_type;
        case SYS_SET_SS_DELAY:
            g_screensaver_delay = (int)arg1;
            return 0;
        case SYS_GET_SS_DELAY:
            return (int64_t)g_screensaver_delay;
        case SYS_SET_SETTINGS_TAB:
            g_settings_tab = (int)arg1;
            return 0;
        case SYS_GET_SETTINGS_TAB:
            return (int64_t)g_settings_tab;
        case SYS_SCREENSAVER_TEST:
            g_ss_test = 1;
            return 0;
        case SYS_GET_SS_TEST: {
            int v = g_ss_test; g_ss_test = 0; return (int64_t)v;
        }
        case SYS_PLAY_WAV:
            return sys_play_wav((const char *)arg1);
        // Ring-3 PCM push (Phase 1, Ring-0 media exit). Additive: SYS_PLAY_WAV
        // above is untouched. All argument validation, the stream-ownership
        // check and the #426 wait-queue blocking live in drivers/audio_pcm.c.
        case SYS_AUDIO_PCM_OPEN:
            return sys_audio_pcm_open((uint32_t)arg1, (uint32_t)arg2, (uint32_t)arg3);
        case SYS_AUDIO_PCM_WRITE:
            return sys_audio_pcm_write((int)arg1, (const void *)arg2, (uint32_t)arg3);
        case SYS_AUDIO_PCM_CLOSE:
            return sys_audio_pcm_close((int)arg1);
        case SYS_AUDIO_POS_MS: {
            // #335: elapsed ms of the current USB-DAC stream = frames/rate.
            extern int uac_is_ready(void);
            extern uint64_t uac_frames_streamed(void);
            extern uint32_t uac_sample_rate(void);
            if (!uac_is_ready()) return -1;
            uint32_t r = uac_sample_rate(); if (!r) return -1;
            return (int64_t)(uac_frames_streamed() * 1000ULL / r);
        }
        case SYS_HDA_DBG: {
            // #71: userland HDA audio bring-up debug. arg1=op, arg2/3/4 = a/b/c.
            extern int64_t hda_debug_op(int op, uint64_t a, uint64_t b, uint64_t c);
            return hda_debug_op((int)arg1, (uint64_t)arg2, (uint64_t)arg3, (uint64_t)arg4);
        }
        case SYS_POWEROFF: {
            extern void gfx_boot_log_clear(void);
            extern void gfx_boot_splash(void);
            extern void gfx_boot_log(const char *);
            extern void acpi_shutdown(void);
            gfx_boot_log_clear();
            gfx_boot_splash();
            gfx_boot_log("Shutting down...");
            { extern volatile uint64_t timer_ticks; extern uint32_t g_timer_hz;
              uint32_t hz = g_timer_hz ? g_timer_hz : 250; uint64_t t0 = timer_ticks;
              volatile uint64_t cap = 0;
              while ((timer_ticks - t0) < (hz * 3 / 2) && cap < 4000000000ULL) cap++; }
            acpi_shutdown();
            return 0;
        }
        case SYS_REBOOT: {
            extern void gfx_boot_log_clear(void);
            extern void gfx_boot_splash(void);
            extern void gfx_boot_log(const char *);
            extern void acpi_reboot(void);
            gfx_boot_log_clear();
            gfx_boot_splash();
            gfx_boot_log("Restarting...");
            { extern volatile uint64_t timer_ticks; extern uint32_t g_timer_hz;
              uint32_t hz = g_timer_hz ? g_timer_hz : 250; uint64_t t0 = timer_ticks;
              volatile uint64_t cap = 0;
              while ((timer_ticks - t0) < (hz * 3 / 2) && cap < 4000000000ULL) cap++; }
            acpi_reboot();
            return 0;
        }
        case SYS_WIN_GET_POS:
            return sys_win_get_pos((int)arg1, (int *)arg2, (int *)arg3);
        case SYS_WIN_MOVE:
            return sys_win_move((int)arg1, (int)arg2, (int)arg3);
        case SYS_WIN_MOVE_BY:
            return sys_win_move_by((int)arg1, (int)arg2, (int)arg3);

        // Window manager query
        case SYS_WM_GET_WINDOWS:
            return sys_wm_get_windows((wm_window_info_t *)arg1, (int)arg2);
        case SYS_WM_FOCUS_WINDOW: {
            // Apps hold the user-window SLOT handle that win_create() returns, not
            // the internal window->id that sys_wm_focus_window() matches on. When a
            // process re-asserts focus on ITS OWN window by that slot handle (what
            // fullscreen games Arena/Squadron do every frame), resolve the slot
            // directly: the slot space (0..15) overlaps low window ids, so routing
            // it through the id path silently focused the WRONG window and the
            // game's WASD/keys went nowhere. Falls back to id matching (taskbar,
            // winswitch) when the handle is not a slot the caller owns.
            extern int64_t wm_focus_user_slot(int slot);
            if (wm_focus_user_slot((int)arg1) == 0) return 0;
            return sys_wm_focus_window((int)arg1);
        }
        case SYS_WM_MINIMIZE_WINDOW:
            return sys_wm_minimize_window((int)arg1);
        case SYS_WM_MAXIMIZE_WINDOW: {
            extern void wm_toggle_maximize_focused(void);
            wm_toggle_maximize_focused();
            return 0;
        }
        // (#745) Compositor publishes the dock-style-derived work-area insets.
        case SYS_WM_SET_WORK_AREA:
            return sys_wm_set_work_area((int32_t)arg1, (int32_t)arg2,
                                        (int32_t)arg3, (int32_t)arg4);
        case SYS_RUN_NEXT_ON_AP: {
            // #279: mark the next user process this caller spawns as
            // migratable so the scheduler routes it to an application
            // processor (GUI `runap` / RC `launchap`).
            extern void proc_set_next_migratable(int v);
            proc_set_next_migratable(1);
            return 0;
        }
        case SYS_GET_MOUSE_SCROLL: {
            // OS-wide mouse wheel: return and clear the kernel scroll delta.
            extern int8_t mouse_get_scroll(void);
            return (int64_t)mouse_get_scroll();
        }
        case SYS_GET_GLOBAL_MOUSE:
            return sys_get_global_mouse((int32_t *)arg1, (int32_t *)arg2, (uint32_t *)arg3);
        case SYS_WIN_SET_NOCHROME:
            return sys_win_set_nochrome((int)arg1);
        case SYS_WIN_SET_SHADOW:
            return sys_win_set_shadow((int)arg1);
        case SYS_DNS_START:
            return sys_dns_start((const char *)arg1, (uint32_t *)arg2);
        case SYS_DNS_POLL:
            return sys_dns_poll((uint32_t *)arg1);
        case SYS_LIST_USERS:
            return sys_list_users((sc_user_info_t *)arg1, (int)arg2);
        case SYS_AUTHENTICATE:
            return sys_authenticate((const char *)arg1, (const char *)arg2);
        case SYS_DELETE_USER:
            return sys_delete_user((const char *)arg1);
        case SYS_SESSION_LOCK:
            return sys_session_lock((int)arg1);
        case SYS_SESSION_UNLOCK:
            return sys_session_unlock((const char *)arg1, (const char *)arg2);
        case SYS_SESSION_IS_LOCKED:
            return sys_session_is_locked();
        case SYS_SET_AUTOLOGIN:
            return sys_set_autologin((const char *)arg1, (const char *)arg2, (int)arg3);
        case SYS_GET_AUTOLOGIN:
            return sys_get_autologin((char *)arg1, (int)arg2);
        case SYS_SET_LOGIN_MODE:
            return sys_set_login_mode((int)arg1, (const char *)arg2, (const char *)arg3);
        case SYS_GET_LOGIN_MODE:
            return sys_get_login_mode();
        case SYS_AUTH_LOCKOUT:
            return sys_auth_lockout((const char *)arg1);
        case SYS_WM_APPS_DIRTY:
            return sys_wm_apps_dirty();

        // IPC: Message Passing
        case SYS_MSG_CREATE_CHANNEL: return sys_msg_create_channel();
        case SYS_MSG_CONNECT:        return sys_msg_connect((int)arg1);
        case SYS_MSG_SEND:           return sys_msg_send((int)arg1, (const void *)arg2, (size_t)arg3);
        case SYS_MSG_RECV:           return sys_msg_recv((int)arg1, (void *)arg2, (size_t)arg3, (int)arg4);
        case SYS_MSG_ACCEPT:         return sys_msg_accept((int)arg1, (int)arg2);
        case SYS_MSG_CLOSE:          return sys_msg_close((int)arg1);
        case SYS_MSG_DESTROY:        return sys_msg_destroy_channel((int)arg1);

        // IPC: Shared Memory
        case SYS_SHM_CREATE:  return sys_shm_create((size_t)arg1, (int)arg2);
        case SYS_SHM_MAP:     return sys_shm_map((int)arg1, (void **)arg2);
        case SYS_SHM_UNMAP:   return sys_shm_unmap((int)arg1);
        case SYS_SHM_DESTROY: return sys_shm_destroy((int)arg1);
        case SYS_SHM_INFO:    return sys_shm_info((int)arg1, (size_t *)arg2, (uint32_t *)arg3);

        // IPC: Name Service
        case SYS_IPC_REGISTER_NAME: return sys_ipc_register_name((const char *)arg1, (int)arg2);
        case SYS_IPC_LOOKUP_NAME:   return sys_ipc_lookup_name((const char *)arg1);

        // Framebuffer / Compositor
        case SYS_FB_MAP:      return sys_fb_map();
        case SYS_FB_INFO:     return sys_fb_info((fb_info_user_t *)arg1);
        case SYS_FB_FLIP:     return sys_fb_flip();
        case SYS_FB_DAMAGE:   return sys_fb_damage((int)arg1, (int)arg2, (int)arg3, (int)arg4);
        case SYS_GET_MOUSE:   return sys_get_mouse((int32_t *)arg1, (int32_t *)arg2, (uint32_t *)arg3);
        case SYS_SET_MOUSE:   return sys_set_mouse((int)arg1, (int)arg2);
        case SYS_GET_KEY:     return sys_get_key((key_event_t *)arg1);
        case SYS_GRAB_INPUT:  return sys_grab_input((int)arg1);
        case SYS_INJECT_MOUSE: return sys_inject_mouse((int32_t)arg1, (int32_t)arg2, (int32_t)arg3, (int32_t)arg4);
        case SYS_SET_MOUSE_BUTTONS: return sys_set_mouse_buttons((uint32_t)arg1);
        case SYS_DECODE_IMAGE: return sys_decode_image((const void *)arg1, (uint32_t)arg2, (uint32_t)arg3, (void *)arg4, (uint32_t)arg5, (int *)arg6);
        case SYS_WIN_DRAW_IMAGE: return sys_win_draw_image((int)arg1, (int)arg2, (int)arg3, (int)arg4, (int)arg5, (uint32_t *)arg6);

        case SYS_DUP: {
            int oldfd = (int)arg1;
            return fd_dup(oldfd, 0);
        }
        case SYS_DUP2: {
            int oldfd = (int)arg1;
            int newfd = (int)arg2;
            if (oldfd < 0 || newfd < 0 || oldfd >= MAX_FDS || newfd >= MAX_FDS) return -1;
            if (oldfd == newfd) return newfd;
            file_t *oldf = fd_get(oldfd);
            if (!oldf) return -1;
            // Close newfd if open
            if (fd_get(newfd)) fd_close(newfd);
            // Install old file at new fd
            file_get(oldf);
            fd_install(newfd, oldf);
            return newfd;
        }
        case SYS_PIPE: {
            int *user_pipefd = (int *)arg1;
            if (!user_pipefd) return -1;
            int pipefd[2];
            extern int pipe_create(int pipefd[2]);
            int ret = pipe_create(pipefd);
            if (ret == 0) {
                // #566: copy_to_user the fd pair (#509 TOCTOU-safe) instead of
                // writing user_pipefd[0]/[1] through the user pointer directly.
                if (copy_to_user(user_pipefd, pipefd, sizeof(pipefd)) != 0) return -14;
            }
            return ret;
        }
        case SYS_IOCTL: {
            // Device control on an open fd (termios TCGETS/TCSETS, winsize, ...).
            // Without this, userland tcgetattr/tcsetattr always failed, so shells
            // (msh) and editors (mvi) could never enter raw mode over a pty/tty.
            file_t *f = fd_get((int)arg1);
            if (!f) return -1;
            extern int file_ioctl(file_t *f, unsigned cmd, void *arg);
            // #500/#509: THE Ring-3 boundary for ioctl. The arg is a user
            // pointer whose size is cmd-dependent (termios/winsize/int), which
            // the flat argtab cannot describe; and file_ioctl also has
            // kernel-internal callers (gui/terminal.c, net/ssh/ssh2_server.c)
            // that legitimately pass KERNEL pointers, so the copy cannot live in
            // tty_ioctl. #509: instead of only U/S-validating at entry (still a
            // TOCTOU hole - a sibling thread could remap arg before tty_ioctl's
            // memcpy), we BOUNCE the arg through a kernel buffer here with
            // copy_*_user, which is atomic check-and-use: a racing unmap faults
            // through the copy fixup and returns EFAULT, never a Ring-0 deref of
            // a freed frame. tty_ioctl now always sees a safe kernel buffer.
            extern size_t tty_ioctl_user_desc(unsigned cmd, int *from_user, int *to_user);
            int _fu = 0, _tu = 0;
            size_t _sz = tty_ioctl_user_desc((unsigned)arg2, &_fu, &_tu);
            if (_sz != 0 && _sz <= 128) {
                uint8_t _kbuf[128];
                if (_fu) {
                    if (copy_from_user(_kbuf, (void *)arg3, _sz) != 0) return -14;
                } else {
                    memset(_kbuf, 0, _sz);
                }
                int _r = file_ioctl(f, (unsigned)arg2, _kbuf);
                if (_r == 0 && _tu) {
                    if (copy_to_user((void *)arg3, _kbuf, _sz) != 0) return -14;
                }
                return _r;
            }
            // Cmd transfers no user memory: pass the arg through unchanged.
            return file_ioctl(f, (unsigned)arg2, (void *)arg3);
        }
        case SYS_GET_NET_BYTES: {
            extern uint64_t net_total_bytes(void);
            return (int64_t)net_total_bytes();
        }
        case SYS_SSH_CLIENT: {
            // arg1=ipv4 (host byte order, a.b.c.d packed), arg2=user, arg3=pass,
            // arg4=(cols<<16)|rows, arg5=port. Bridges the kernel SSH-2 client to
            // this process's stdin (fd0) / stdout (fd1) and blocks until it ends.
            uint32_t ip = (uint32_t)arg1;
            // #19/#645: both are Ring-3 strings the SSH client reads directly.
            // strncpy_from_user is called INLINE here rather than through the
            // sc_copy_str_user() helper, because copy-user-lint traces exactly
            // one call level from a dispatcher case and would otherwise see the
            // helper's kernel-side `dst[0] = 0` as a raw user deref.
            char shu[128], shp[128];
            shu[0] = '\0'; shp[0] = '\0';
            if (arg2 && strncpy_from_user(shu, (const char *)arg2, sizeof(shu)) < 0) return -14;
            if (arg3 && strncpy_from_user(shp, (const char *)arg3, sizeof(shp)) < 0) return -14;
            const char *user = shu;
            const char *pass = shp;
            int cols = (int)((arg4 >> 16) & 0xffff);
            int rows = (int)(arg4 & 0xffff);
            uint16_t port = (uint16_t)arg5;
            if (!ip) return -1;
            file_t *fin = fd_get(0), *fout = fd_get(1);
            if (!fin || !fout) return -1;
            extern int ssh2_run_on_fds(uint32_t ip, uint16_t port, const char *user,
                                       const char *pass, int cols, int rows,
                                       void *fin, void *fout);
            return ssh2_run_on_fds(ip, port, user, pass, cols, rows, fin, fout);
        }
        case SYS_NET_INFO: {
            char *ubuf = (char *)arg1;
            unsigned long ulen = (unsigned long)arg2;
            if (!ubuf || ulen == 0) return -1;
            extern int net_format_info(char *buf, unsigned long len);
            // #19/#645: net_format_info() formats straight into the Ring-3
            // buffer. Format into kernel memory, then ONE copy_to_user.
            if (ulen > 4096) ulen = 4096;
            char *nik = (char *)kmalloc(ulen);
            if (!nik) return -12;
            int nir = net_format_info(nik, ulen);
            if (nir >= 0 && copy_to_user(ubuf, nik, ulen) != 0) nir = -14;
            kfree(nik);
            return nir;
        }
        case SYS_SETPRIORITY: {
            // arg1 = pid (<=0 means the calling process), arg2 = level 0..4
            // (PRIO_IDLE..PRIO_REALTIME). Lets background apps/services demote
            // themselves so interactive work gets the CPU. Background kernel
            // threads (RemoteCtrl, sshd) are already PRIO_LOW.
            int pid = (int)arg1;
            int level = (int)arg2;
            if (level < PRIO_IDLE) level = PRIO_IDLE;
            if (level > PRIO_REALTIME) level = PRIO_REALTIME;
            process_t *tp = (pid <= 0) ? proc_current() : proc_get((uint32_t)pid);
            if (!tp) return -1;
            tp->priority = (process_priority_t)level;
            return 0;
        }
        case SYS_GET_CPU_USAGE: {
            extern int proc_get_cpu_usage(void);
            return proc_get_cpu_usage();
        }
        case SYS_GET_CPU_PER_CORE: {
            // Fill user buffer: [0]=core count, [1..count]=per-core CPU %% (0-100).
            extern int smp_get_core_count(void);
            extern int smp_get_core_pct(uint32_t);
            uint32_t *buf = (uint32_t *)arg1;
            if (!buf) return 0;
            int n = smp_get_core_count();
            if (n < 1) n = 1;
            if (n > 64) n = 64;
            // #566: fill a kernel buffer then copy_to_user once (#509 TOCTOU-safe).
            uint32_t ck[65];
            ck[0] = (uint32_t)n;
            for (int i = 0; i < n; i++) ck[1 + i] = (uint32_t)smp_get_core_pct((uint32_t)i);
            if (copy_to_user(buf, ck, (size_t)(n + 1) * sizeof(uint32_t)) != 0) return -14;
            return n;
        }
        case SYS_GET_MEM_INFO: {
            extern uint64_t pmm_get_total_pages(void);
            extern uint64_t pmm_get_used_pages(void);
            unsigned long *tp = (unsigned long *)arg1;
            unsigned long *up = (unsigned long *)arg2;
            // #566: copy_to_user each out-param (#509 TOCTOU-safe) instead of
            // writing *tp / *up directly through the user pointers.
            if (tp) { unsigned long t = (unsigned long)(pmm_get_total_pages() * 4096ULL);
                      if (copy_to_user(tp, &t, sizeof(t)) != 0) return -14; }
            if (up) { unsigned long u = (unsigned long)(pmm_get_used_pages() * 4096ULL);
                      if (copy_to_user(up, &u, sizeof(u)) != 0) return -14; }
            return 0;
        }

        case SYS_PROC_LIST:
        {
            // #19/#645: proc_snapshot() fills the caller's array row by row and
            // takes proc_mm_lock() between rows, so an AC window around it would
            // hold AC across a lock acquire. Snapshot into kernel memory, then
            // ONE copy_to_user. Bounded by MAX_PROCESSES (64 rows, ~4KB).
            int pl_max = (int)arg2;
            if (pl_max <= 0) return 0;
            if (pl_max > MAX_PROCESSES) pl_max = MAX_PROCESSES;
            proc_info_t *plk = (proc_info_t *)kmalloc((size_t)pl_max * sizeof(proc_info_t));
            if (!plk) return -12;
            int pln = proc_snapshot(plk, pl_max);
            if (pln > 0 &&
                copy_to_user((void *)arg1, plk, (size_t)pln * sizeof(proc_info_t)) != 0) {
                kfree(plk); return -14;
            }
            kfree(plk);
            return pln;
        }

        // #487/#349 Ring-3 process introspection. Unlike their neighbours here,
        // each of these VALIDATES its user pointer in the backend before any
        // write (see proc/procinfo.h), and every byte copied into the caller's
        // buffer is copied by a bounded Rust builder.
        case SYS_PROC_HANDLES:
            return sys_proc_handles((uint32_t)arg1, (void *)arg2, (int)arg3);

        case SYS_NET_CONNS:
            return sys_net_conns((uint32_t)arg1, (void *)arg2, (int)arg3);

        case SYS_SVC_LIST:
            return sys_svc_list((void *)arg1, (int)arg2);

        case SYS_SVC_CONTROL:
            return sys_svc_control((const char *)arg1, (int)arg2);

        case SYS_PROC_DETAIL:
            return sys_proc_detail((uint32_t)arg1, (void *)arg2);

        case SYS_COMPOSITOR_RENDER_WINDOWS:
            // Draw KWM window frames and app content onto the compositor framebuffer.
            // The compositor owns fb_back; it draws its desktop first, then we overlay
            // any open KWM windows (terminal, settings, etc.) on top.
            wm_draw_all();
            wm_draw_apps();
            wm_draw_winmenu();   // Task A: decorator popup on top of app content
            // #564: this composite is now current - clear the dirty flag
            // SYS_WM_APPS_DIRTY (sys_wm_apps_dirty()) peeks, so the compositor's
            // idle-CPU render gate stops seeing "dirty" until the next real
            // change (window move/resize/focus/create/close, or an app's own
            // win_invalidate()).
            wm_clear_dirty();
            return 0;

        case SYS_GET_KEYBOARD: {
            // While a Win16 app owns the screen, its own message pump
            // (win16_pump_input) must be the sole keyboard consumer; do not let
            // the compositor drain the key buffer or game keys are lost.
            extern volatile int g_win16_owns_screen;
            if (g_win16_owns_screen) return -1;
            return keyboard_has_char() ? keyboard_get_char() : -1;
        }

        case SYS_INJECT_KEY: {
            // Compositor forwards a raw keycode from the hardware queue to the
            // focused KWM window via wm_dispatch_event so user_window_event_handler
            // queues it into the per-window user event queue for win_get_event().
            int key = (int)arg1;
            gui_event_t ev;
            memset(&ev, 0, sizeof(ev));
            // (Word6 divergence catalog #2, Alt-menu) KEY_LSHIFT/KEY_RSHIFT
            // (0x95/0x96, moved this change off 0x87/0x88 to de-collide from
            // isr.c's KEY_F10/KEY_F1), KEY_LCTRL (0x99, #386/6a848ae),
            // KEY_ALT (0x9A, new this change) and KEY_SUPER (0x9B, #552) are
            // all PRESS codes that live at or above 0x90 because the 0x80-0x8F
            // press range is fully occupied by arrow/F-keys (see cpu/isr.h).
            // Without this explicit check they fell into the generic
            // "0x90-0x98/>0x98 => release" buckets below and were misreported
            // to non-Win16 apps as a key-UP of an unrelated keycode (e.g. a
            // Shift press looked like a release of an F-key, a Ctrl press
            // looked like a release of 0x19). Win16/Word itself never goes
            // through this path (SYS_GET_KEYBOARD's g_win16_owns_screen gate
            // above routes it to keyboard_get_char directly instead), so this
            // only affects native GUI apps reached via the compositor's
            // hardware-queue forward. The matching _UP release codes already
            // fall correctly into the buckets below (right TYPE); their
            // computed keycode does not round-trip back to the press value
            // here, a pre-existing imprecision in this scheme (true for
            // KEY_LCTRL_UP before this change too), left alone.
            if (key == 0x95 || key == 0x96 || key == 0x99 || key == 0x9A || key == 0x9B) {
                ev.type = EVENT_KEY_DOWN;
                ev.keycode = key;
            } else if (key >= 0x90 && key <= 0x98) {
                ev.type = EVENT_KEY_UP;
                ev.keycode = key - 0x10;
            } else if (key >= 0x80 && key < 0x90) {
                ev.type = EVENT_KEY_DOWN;
                ev.keycode = key;
            } else if (key > 0x98) {
                ev.type = EVENT_KEY_UP;
                ev.keycode = key & 0x7F;
            } else {
                ev.type = EVENT_KEY_DOWN;
                ev.keycode = key;
                ev.key_char = (char)key;
            }
            wm_dispatch_event(&ev);
            return 0;
        }

        case SYS_DEV_PCI_LIST: {
            extern int64_t devinfo_pci_list(devinfo_pci_t *, int);
            return devinfo_pci_list((devinfo_pci_t *)arg1, (int)arg2);
        }
        case SYS_DEV_USB_LIST: {
            extern int64_t devinfo_usb_list(devinfo_usb_t *, int);
            return devinfo_usb_list((devinfo_usb_t *)arg1, (int)arg2);
        }
        case SYS_DEV_IRQ_LIST: {
            extern int64_t devinfo_irq_list(devinfo_irq_t *, int);
            return devinfo_irq_list((devinfo_irq_t *)arg1, (int)arg2);
        }
        case SYS_SYSINFO: {
            extern int64_t devinfo_sysinfo(devinfo_sysinfo_t *);
            return devinfo_sysinfo((devinfo_sysinfo_t *)arg1);
        }

        // #265 cron-like timer/scheduler
        case SYS_CRON_ADD:
            return cron_add((const cron_job_t *)arg1);
        case SYS_CRON_LIST:
            return cron_list((cron_job_t *)arg1, (int)arg2);
        case SYS_CRON_REMOVE:
            return cron_remove((uint32_t)arg1);
        case SYS_CRON_ENABLE:
            return cron_enable((uint32_t)arg1, (int)arg2);

        // #430: Signals (implemented in proc/signal.c). The return-work hook in
        // syscall.asm already delivers pending signals on the way back to
        // userland; these cases simply install/raise/mask/return.
        case SYS_KILL:
            return sys_kill((int)arg1, (int)arg2);
        case SYS_TKILL:                 // tid == pid in our thread model
            return sys_kill((int)arg1, (int)arg2);
        case SYS_TGKILL:                // (tgid, tid, sig) -> deliver to tid
            return sys_kill((int)arg2, (int)arg3);
        case SYS_SIGACTION:
            return sys_sigaction((int)arg1, (const void *)arg2, (void *)arg3);
        case SYS_SIGPROCMASK:
            return sys_sigprocmask((int)arg1, (const uint64_t *)arg2,
                                   (uint64_t *)arg3);
        case SYS_SIGRETURN:
            return sys_rt_sigreturn();
        case SYS_ALARM:
            return sys_alarm((uint32_t)arg1);
        case SYS_PAUSE:
            return sys_pause();

        // #430: Threads + futex (proc/process.c clone, sync/futex.c futex).
        case SYS_CLONE:
            return proc_clone((uint32_t)arg1, (void *)arg2, (uint32_t *)arg3,
                              (uint32_t *)arg4, (void *)arg5);
        case SYS_GETTID:
            return (int64_t)proc_gettid();
        case SYS_SET_TID_ADDRESS:
            return (int64_t)proc_set_tid_address((uint32_t *)arg1);
        case SYS_FUTEX:
            return sys_futex((uint32_t *)arg1, (int)arg2, (uint32_t)arg3,
                             (uint64_t)arg4, (uint32_t *)arg5, (uint32_t)arg6);

                default:
            kprintf("[SYSCALL] Unknown syscall %lu\n", num);
            return -1;
    }
}

// ============================================================================
// Process syscalls
// ============================================================================

int64_t sys_exit(int exit_code) {
    kprintf("[SYSCALL] exit(%d)\n", exit_code);
    proc_exit(exit_code);
    return 0;  // Never reached
}

int64_t sys_fork(void) {
    return proc_fork();
}

// ============================================================================
// POSIX process groups and sessions (#745 local 82)
// ============================================================================
//
// THIS IS GLUE ONLY. Every rule lives in rustkern/pgrp.rs, which is pure and
// self-tested at boot. What is here is the part Rust cannot do: walking the
// process table. The split is deliberate and matches sessionid.rs, and it is
// what makes the refusals testable without a scheduler.
//
// Why these are worth having: drivers/tty.c ALREADY raises SIGINT/SIGQUIT/
// SIGTSTP at t->fg_pgrp and SIGHUP on hangup, sig_raise_pgrp() ALREADY walks
// the table matching p->pgrp, and TIOCSPGRP ALREADY sets fg_pgrp. Without
// setpgid() every process inherits its parent's group forever, so the entire
// process tree is ONE group and a Ctrl-C reaches everything descended from
// the shell. These calls are the missing half of a mechanism that is already
// wired, not a number nobody acts on.

extern int32_t pgrp_setsid_decide_rs(uint32_t caller_pid, uint32_t caller_pgrp);
extern uint32_t pgrp_resolve_pid_rs(int64_t arg_pid, uint32_t caller_pid);
extern int64_t pgrp_resolve_pgid_rs(int64_t arg_pgid, uint32_t target_pid);
extern int32_t pgrp_setpgid_decide_rs(uint32_t caller_pid, uint32_t caller_session,
                                      uint32_t target_pid, uint32_t target_exists,
                                      uint32_t target_ppid, uint32_t target_session,
                                      uint32_t pgid, uint32_t pgid_in_caller_session);

// Rust decision codes (rustkern/pgrp.rs). Mirrored, so a change there that is
// not mirrored here shows up as a wrong errno rather than a wrong behaviour.
#define PGRP_SETSID_ALLOW  0

int64_t sys_setsid(void) {
    process_t *me = proc_current();
    if (!me) return -1;   // -EPERM: no caller, no session
    if (pgrp_setsid_decide_rs(me->pid, me->pgrp) != PGRP_SETSID_ALLOW) {
        return -1;        // -EPERM: already a process group leader
    }
    me->session = me->pid;
    me->pgrp    = me->pid;
    // POSIX: the new session leader has NO controlling terminal. There is no
    // per-process controlling-tty pointer in process_t today, so there is
    // nothing to clear; the TTY's own fg_pgrp is left alone deliberately,
    // because clearing another session's foreground group from here would be
    // precisely the reach-across setpgid's rules exist to prevent.
    return (int64_t)me->pid;
}

int64_t sys_setpgid(int64_t pid_arg, int64_t pgid_arg) {
    process_t *me = proc_current();
    if (!me) return -1;
    uint32_t target_pid = pgrp_resolve_pid_rs(pid_arg, me->pid);
    int64_t  pgid_r     = pgrp_resolve_pgid_rs(pgid_arg, target_pid);
    if (pgid_r < 0) return pgid_r;   // -EINVAL
    uint32_t pgid = (uint32_t)pgid_r;

    process_t *t = proc_get(target_pid);
    uint32_t exists = 0, ppid = 0, tsession = 0;
    if (t && t->state != PROC_STATE_UNUSED) {
        exists = 1; ppid = t->ppid; tsession = t->session;
    }

    // Does group `pgid` already exist inside the caller's session? Rust needs
    // the answer but cannot walk the table. Creating a brand-new group led by
    // the target is always allowed, which is why that case short-circuits.
    uint32_t pgid_here = (pgid == target_pid) ? 1u : 0u;
    if (!pgid_here) {
        for (uint32_t p = 1; p < MAX_PROCESSES; p++) {
            process_t *q = proc_get(p);
            if (q && q->state != PROC_STATE_UNUSED && q->state != PROC_STATE_ZOMBIE &&
                q->pgrp == pgid && q->session == me->session) {
                pgid_here = 1; break;
            }
        }
    }

    int32_t rc = pgrp_setpgid_decide_rs(me->pid, me->session, target_pid, exists,
                                        ppid, tsession, pgid, pgid_here);
    if (rc != 0) return (int64_t)rc;
    t->pgrp = pgid;
    return 0;
}

int64_t sys_getpgid(int64_t pid_arg) {
    process_t *me = proc_current();
    if (!me) return -3;   // -ESRCH
    uint32_t target = pgrp_resolve_pid_rs(pid_arg, me->pid);
    process_t *t = proc_get(target);
    if (!t || t->state == PROC_STATE_UNUSED) return -3;   // -ESRCH
    return (int64_t)t->pgrp;
}

int64_t sys_getsid(int64_t pid_arg) {
    process_t *me = proc_current();
    if (!me) return -3;   // -ESRCH
    uint32_t target = pgrp_resolve_pid_rs(pid_arg, me->pid);
    process_t *t = proc_get(target);
    if (!t || t->state == PROC_STATE_UNUSED) return -3;   // -ESRCH
    return (int64_t)t->session;
}


// SYS_SPAWN_ARGS: spawn a new process with argv
// arg1 = path (user string), arg2 = argv[] (user pointer array), arg3 = argc
// Open a redirect target as a struct file_t, choosing the ext2 backing for the
// ext2 root volume (and the explicit /ext2 mount) and FAT otherwise. Returns
// NULL on error.
static file_t *open_redir_file(const char *path, int flags) {
    if (!path || !path[0]) return NULL;

    // #676 CREATE POINT 3 of 3. Shell redirection ("cmd > file") reaches the
    // filesystem here, NOT through sys_open, and this function checked nothing
    // at all: no perms_check, no ownership. A Ring-3 caller that could not
    // open() a path for writing could still redirect onto it. The rule has to
    // be the same rule, so it is the same shape as sys_open_k's: creating a
    // NAME is a write to the parent directory; writing an EXISTING file is a
    // write to that file.
    {
        process_t *rp = proc_current();
        if (rp && rp->privilege == PRIV_USER) {
            if (!fat_exists(&g_fat_fs, path)) {
                if (!(flags & O_CREAT)) return NULL;
                char parent[SC_PATH_MAX];
                sc_parent_of(path, parent, sizeof(parent));
                if (perms_check(parent, rp->euid, rp->egid, W_OK | X_OK) != 0) return NULL;
            } else if (perms_check(path, rp->euid, rp->egid, W_OK) != 0) {
                return NULL;
            }
            // #679: and the file it creates must belong to its creator, or the
            // creator cannot write to it a second time.
            if (flags & O_CREAT) perms_on_create(path, rp->euid, rp->egid, 0);
        }
    }

    if (path_is_ext2(path)) {
        return ext2_vfs_open(ext2_relpath(path), flags);
    }
    if (path_root_ext2(path)) {
        // Use ext2 if the file already exists there or we are creating it;
        // otherwise fall back to FAT (ESP-only files).
        if (ext2_resolve_path(path) != 0 || (flags & O_CREAT)) {
            return ext2_vfs_open(path, flags);
        }
    }
    return fat_vfs_open(path, flags);
}

static int64_t spawn_impl(const char *path, char **argv, int argc,
                          const char *infile, const char *outfile, int append) {
    if (!path || argc < 0) return -1;
    if (argc > 64) argc = 64;

    // #95: a service may only spawn child processes if granted SVC_PERM_SPAWN.
    // No-op for normal processes (is_service == 0).
    {
        process_t *cur = proc_current();
        if (cur && cur->is_service && !(cur->svc_perms & SVC_PERM_SPAWN)) {
            return -1;  // EPERM: service lacks spawn capability
        }
    }

    extern fat_fs_t g_fat_fs;
    if (!g_fat_fs.mounted) return -1;

    // #700 B8: EXECUTE PERMISSION, checked for the first time anywhere in this
    // kernel. Before this, X_OK appeared in exactly two expressions tree-wide,
    // both of them "W_OK | X_OK" on a PARENT DIRECTORY (search permission), and
    // no code path had ever asked whether a caller was allowed to RUN a file.
    // The mode bit was recorded by chmod, reported by the Files properties
    // panel, and enforced nowhere: an x bit that means nothing is worse than no
    // x bit, because it is a control everybody believes in.
    //
    // The rule is POSIX's: X_OK on the file, and nothing else. Not R_OK, because
    // reading the image is the kernel's job and an execute-only binary is a
    // legitimate thing to want. Nothing regresses on the shipped tree:
    // perms_check()'s no-entry default is root-owned mode 0755, so every file
    // without an explicit entry (which is nearly all of /APPS) still passes.
    // Only a file somebody deliberately chmod'ed non-executable is now refused.
    {
        process_t *sp = proc_current();
        if (sp && sp->privilege == PRIV_USER &&
            perms_check(path, sp->euid, sp->egid, X_OK) != 0)
            return -1;
    }

    // Read the ELF file from disk
    uint32_t size = 0;
    void *data = fat_read_file(&g_fat_fs, path, &size);
    if (!data || size == 0) return -1;

    // Validate ELF
    if (elf_validate(data, size) != 0) {
        kfree(data);
        return -1;
    }

    // Build kernel-side argv array by copying user strings into kernel buffers.
    // The string buffer (64 args x 256 bytes = 16KB) is allocated on the HEAP,
    // not on the kernel stack. A 16KB automatic array here previously risked
    // overflowing the 64KB kernel stack: this function keeps the buffer live
    // while calling proc_create_user() (FAT read, ELF load, address-space
    // setup, setup_user_argv), and any interrupt/scheduler nesting on top of
    // that could push the stack past its limit and corrupt adjacent kernel
    // heap memory. The corruption manifested as nondeterministic faults in the
    // *calling* process (e.g. a terminal spawning grep) after a variable number
    // of spawns.
    char (*kbuf)[256] = kmalloc(64 * 256);
    if (!kbuf) { kfree(data); return -1; }
    char *kargv[65];
    int kargc = 0;

    if (argv && argc > 0) {
        // #500: argv is a two-level user deref the flat syscall argtab cannot
        // express. Validate the pointer array itself is user-readable before we
        // read any argv[i], so a Ring-3 caller cannot point argv at kernel
        // memory and have the loop below read kernel words as argument pointers.
        // argc is already clamped to <=64 above, so this range is <=512 bytes.
        if (validate_user_ptr(argv, (size_t)argc * sizeof(char *),
                              ACCESS_READ_USER) != VALIDATE_OK) {
            kfree(kbuf); kfree(data); return -1;
        }
        // #19/#645: read the POINTER ARRAY itself into kernel memory in one
        // shot. The loop below used to re-read argv[i] straight out of Ring-3
        // memory, which is a #PF under CR4.SMAP and, independently, a
        // check-and-use gap: validate_user_ptr() above proved the array
        // readable, then every iteration read it again and a sibling thread
        // could have rewritten it in between.
        char *kargp[64];
        if (copy_from_user(kargp, argv, (size_t)argc * sizeof(char *)) != 0) {
            kfree(kbuf); kfree(data); return -1;
        }
        for (int i = 0; i < argc && i < 64; i++) {
            if (!kargp[i]) break;
            // Copy from user pointer to kernel buffer. Referenced as
            // kargp[i] throughout rather than via a local `const char *usrc`,
            // because that declaration is itself what smap-uaccess-lint's B3
            // `\*\s*usrc` pattern matches, and an EXEMPT entry for a
            // declaration is a worse answer than not writing the declaration.
            // #500: validate each element string before dereferencing it. The
            // copy below reads usrc[0..254], stopping at the first NUL;
            // validate_user_string proves every scanned byte is present and
            // user-accessible. UNTERMINATED is NOT a memory fault (all 256
            // scanned bytes were valid, there was simply no NUL): the copy
            // truncates at 255 exactly as before, so only a genuine bad page
            // (kernel/unmapped) rejects the spawn.
            {
                validate_error_t _sr = validate_user_string(kargp[i], 256);
                if (_sr != VALIDATE_OK && _sr != VALIDATE_STRING_UNTERMINATED) {
                    kfree(kbuf); kfree(data); return -1;
                }
            }
            // #19/#645: the byte loop was a raw Ring-0 walk of a Ring-3 string.
            // strncpy_from_user is the canonical primitive for exactly this: it
            // is AC-bracketed, it carries the exception fixup, and it truncates
            // at the cap the same way the loop did. An UNTERMINATED string is
            // still accepted (validate_user_string above already decided that),
            // because strncpy_from_user stops at the cap and NUL-terminates.
            if (strncpy_from_user(kbuf[i], kargp[i], 256) < 0) {
                kfree(kbuf); kfree(data); return -1;
            }
            kbuf[i][255] = '\0';
            kargv[i] = kbuf[i];
            kargc++;
        }
    }
    kargv[kargc] = NULL;

    // Extract program name from path
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') name = p + 1;
    }

    // Create the user process with argv. proc_create_user() (via
    // setup_user_argv) copies the argv strings onto the child's user stack, so
    // the kernel-side string buffer can be released as soon as it returns.
    // #692: exec runs as the CALLING Ring-3 process. proc_as_caller() is
    // refused outright if the caller is not Ring 3, so this cannot degrade
    // into inheriting a kernel thread's uid 0.
    int pid = proc_create_user_as(name, data, size, kargc > 0 ? kargv : NULL, NULL,
                                  proc_as_caller());
    kfree(data);
    kfree(kbuf);

    // Inherit stdio fds from the calling process so that the child writes
    // to the same PTY (or console) as the parent. Without this, children
    // spawned by msh in a terminal get /dev/console and their output goes
    // to serial instead of the terminal window.
    if (pid > 0 && argv) {   // only inherit stdio for argv spawns (msh/terminal); compositor/desktop apps keep their own console (#75)
        extern process_t *proc_get(uint32_t pid);
        process_t *parent = proc_current();
        process_t *child  = proc_get((uint32_t)pid);
        if (parent && child) {
            for (int fi = 0; fi < 3; fi++) {
                if (parent->fds[fi]) {
                    // Close the /dev/console fd that init_proc opened
                    if (child->fds[fi])
                        IGNORE_RESULT("execve close-on-exec sweep: no recipient exists mid-exec, and failing here would break exec on a full disk (#695)",
                                      file_put(child->fds[fi]));
                    // Replace with parent's fd (same PTY slave)
                    child->fds[fi] = parent->fds[fi];
                    file_get(parent->fds[fi]);
                }
            }
        }
    }

    // Shell I/O redirection (#redirect): replace the child's stdin/stdout with
    // file-backed struct file_t objects, overriding the inherited PTY fds. The
    // child writes/reads through these; on child exit fd_close_all() releases
    // them, which commits a buffered write to disk (ext2_write_file / FAT).
    if (pid > 0 && (infile || outfile)) {
        extern process_t *proc_get(uint32_t pid);
        process_t *child = proc_get((uint32_t)pid);
        if (child) {
            if (outfile && outfile[0]) {
                file_t *of = open_redir_file(outfile,
                                 O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
                if (of) {
                    if (child->fds[1])
                        IGNORE_RESULT("redirect install evicts the replaced description; dup2 and shell redirection must still succeed on a full disk (#695)",
                                      file_put(child->fds[1]));
                    child->fds[1] = of;
                }
            }
            if (infile && infile[0]) {
                file_t *inf = open_redir_file(infile, O_RDONLY);
                if (inf) {
                    if (child->fds[0])
                        IGNORE_RESULT("redirect install evicts the replaced description (#695)",
                                      file_put(child->fds[0]));
                    child->fds[0] = inf;
                }
            }
        }
    }

    return (int64_t)pid;
}

static int64_t sys_spawn_args(const char *path, char **argv, int argc) {
    return spawn_impl(path, argv, argc, NULL, NULL, 0);
}

static int64_t sys_spawn_redir(const char *path, char **argv, int argc,
                               const char *infile, const char *outfile, int append) {
    return spawn_impl(path, argv, argc, infile, outfile, append);
}

int64_t sys_exec(const char *path) {
    if (!path) return -1;
    extern int proc_execve_arm(const char *path, char **argv, char **envp);
    return proc_execve_arm(path, NULL, NULL);
}

int64_t sys_getpid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->pid : -1;
}

int64_t sys_getppid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->ppid : -1;
}

int64_t sys_yield(void) {
    proc_yield();
    return 0;
}

int64_t sys_sleep(uint32_t ms) {
    proc_sleep(ms);
    return 0;
}


// Kernel-side struct stat, byte-for-byte matching userland <sys/stat.h>.
typedef struct {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int  st_mode;
    unsigned int  st_nlink;
    unsigned int  st_uid;
    unsigned int  st_gid;
    unsigned long st_rdev;
    long          st_size;
    long          st_blksize;
    long          st_blocks;
    unsigned long st_atime;
    unsigned long st_mtime;
    unsigned long st_ctime;
} k_stat_t;
// #503 argtab sizeof-lock: SYS_STAT arg2 (SZ_K_STAT in rustkern.rs).
_Static_assert(sizeof(k_stat_t) == 88,
               "#503 argtab: SZ_K_STAT in rustkern.rs is stale");

// ===========================================================================
// #745 Stage 3: THE METADATA GATE. One rule, two callers (SYS_STAT and
// SYS_FS_PERM_INFO), because two copies of a permission rule is how they drift.
//
// WHAT WAS WRONG. Neither syscall performed ANY permission check. sys_stat_path
// took a path and reported existence, type and size; sys_fs_perm_info took a
// path and reported uid, gid and mode. Both answered for ANY path, from ANY
// Ring-3 caller, at any uid. #745 Stage 1 recorded this in the /CONFIG comment
// in fs/perms.c as the known limit of moving that directory to 0711, and left
// it alone deliberately because adding a check to stat has a far wider blast
// radius than one directory mode.
//
// WHAT THE RULE IS. POSIX, exactly: stat(path) requires SEARCH permission (x)
// on every directory component of `path`, and NO permission at all on the
// object itself. So the gate asks perms_check(PARENT, X_OK), and
// perms_path_check_rs() already walks and requires x on each component above
// that parent, which is the whole of the POSIX requirement in one call.
//
// Asking for R_OK on the object instead would have been the intuitive fix and
// it is WRONG in both directions: it would deny a file manager the size of a
// file it can legitimately see listed, and it would still permit stat of a
// world-readable file inside a directory the caller may not traverse.
//
// THE HONEST LIMIT, stated plainly because overstating it would be worse than
// the gap. This does NOT hide the existence of a KNOWN name under /CONFIG,
// because /CONFIG is 0711 and 0711 grants x to everyone: that is precisely what
// "traversable but not listable" means, and real UNIX behaves the same way
// (stat("/etc/shadow") succeeds for any user). 0711 defeats ENUMERATION, which
// is what #745 Stage 1 claimed for it, and nothing more. What this gate DOES
// close is every directory whose mode actually withholds search: another user's
// home at 0750, any 0700 directory, and any future secrets directory that is
// not deliberately traversable. Before it, those disclosed file existence,
// size, owner and mode to any caller who guessed a name. Measured, not
// theorised: see the nrprobe vectors for this change.
//
// The path is bounced by the CALLER before this is consulted, and that is not
// tidiness. Checking a Ring-3 pointer and then handing the SAME pointer to
// fat_open() is the #509 check-and-use gap: a sibling thread rewrites the
// buffer between the two reads and the path that was authorized is not the path
// that gets opened. Both callers previously passed the raw user pointer all the
// way down into the filesystem layer, so bouncing is a prerequisite for the
// gate to mean anything, not a separate cleanup.
//
// Ring 0 is unaffected, as everywhere else in this file: perms_check() is
// consulted only for p->privilege == PRIV_USER.
// ===========================================================================
static int sc_meta_permit(const char kpath[SC_PATH_MAX]) {
    process_t *p = proc_current();
    if (!p || p->privilege != PRIV_USER) return 0;   // Ring 0: unchanged
    // SMB/NFS enforce server-side (NTLM/RPC + share ACLs); local POSIX modes do
    // not describe them. Same carve-out sys_open_k() already makes.
    if (path_is_smb(kpath) || path_is_nfs(kpath)) return 0;
    char parent[SC_PATH_MAX];
    sc_parent_of(kpath, parent, sizeof(parent));
    return perms_check(parent, p->euid, p->egid, X_OK);
}

// O(1) stat by path. Reads the file size and type directly from the FAT
// directory entry (via fat_open, which never walks the file's cluster chain),
// so stat'ing a directory of large files is cheap. Previously userland stat()
// sized files with SEEK_END, which made fat_seek walk every cluster: `ls -la /`
// over the multi-MB kernel.elf/wallpaper files effectively hung the machine.
int64_t sys_stat_path(const char *u_path, void *ubuf) {
    if (!u_path || !ubuf) return -1;

    // #745: bounce ONCE, then gate. Every branch below reads `path`, which is
    // now the kernel copy; the Ring-3 pointer is never dereferenced again.
    char kpath[SC_PATH_MAX];
    if (sc_bounce_str(u_path, kpath, sizeof(kpath)) != 0) return -14;
    const char *path = kpath;
    if (sc_meta_permit(kpath) != 0) return -13;   // EACCES

    // #317: SMB network share. Mount on demand and stat over SMB2.
    if (path_is_smb(path)) {
        if (smb_vfs_ensure_mount(path) != 0) return -1;
        smb_dirent_t info;
        if (smb_stat(path, &info) != 0) return -1;
        k_stat_t st;   // #567: fill kernel-local, then one fault-safe copy_to_user
        memset(&st, 0, sizeof(st));
        st.st_mode    = info.is_directory ? (0040000u | 0755u) : (0100000u | 0644u);
        st.st_nlink   = 1;
        st.st_size    = (long)info.size;
        st.st_blksize = 512;
        st.st_blocks  = ((long)info.size + 511) / 512;
        if (copy_to_user(ubuf, &st, sizeof(st)) != 0) return -14;
        return 0;
    }

    // #317 pass 4: NFS export. Mount must exist; stat via NFSv3 GETATTR/LOOKUP.
    if (path_is_nfs(path)) {
        if (nfs_vfs_ensure_mount(path) != 0) return -1;
        nfs_fattr3_t attrs;
        if (nfs_getattr(path, &attrs) != 0) return -1;
        k_stat_t st;   // #567
        memset(&st, 0, sizeof(st));
        int is_dir = (attrs.type == NF3DIR);
        st.st_mode    = is_dir ? (0040000u | 0755u) : (0100000u | 0644u);
        st.st_nlink   = (attrs.nlink ? attrs.nlink : 1);
        st.st_size    = (long)attrs.size;
        st.st_blksize = 512;
        st.st_blocks  = ((long)attrs.size + 511) / 512;
        if (copy_to_user(ubuf, &st, sizeof(st)) != 0) return -14;
        return 0;
    }

    // #359 Phase 2: ext2 root/volume paths. sys_stat previously had no ext2
    // branch, so ext2 paths fell through to FAT (which fails), and libc stat()
    // then fell back to open()+SEEK_END - which always reports S_IFREG. That made
    // every ext2 DIRECTORY look like a regular file, so CPython's PathFinder
    // (_path_isdir) never built a FileFinder and NO filesystem import worked.
    // Resolve the inode and report the real type (ext2 i_mode is POSIX-layout:
    // 0x4000=dir, 0x8000=reg, + perm bits) and size.
    {
        const char *rel = 0;
        if (path_is_ext2(path))        rel = ext2_relpath(path);
        else if (path_root_ext2(path)) rel = path;
        if (rel) {
            uint32_t ino = ext2_resolve_path(rel);
            if (ino) {
                ext2_inode_t in;
                if (ext2_read_inode(ino, &in) == 0) {
                    int is_dir = ((in.i_mode & 0xF000) == 0x4000);
                    k_stat_t st;   // #567
                    memset(&st, 0, sizeof(st));
                    st.st_mode    = in.i_mode ? in.i_mode
                                     : (is_dir ? (0040000u|0755u) : (0100000u|0644u));
                    st.st_nlink   = 1;
                    st.st_size    = is_dir ? 0 : (long)in.i_size;
                    st.st_blksize = 1024;
                    st.st_blocks  = ((long)in.i_size + 511) / 512;
                    if (copy_to_user(ubuf, &st, sizeof(st)) != 0) return -14;
                    return 0;
                }
            }
            // Not present on ext2: fall through to FAT (file may be ESP-only).
        }
    }

    fat_file_t f;
    if (fat_open(&g_fat_fs, path, &f) != 0) return -1;

    int is_dir = f.is_dir || (f.attr & FAT_ATTR_DIRECTORY);
    uint32_t size = is_dir ? 0 : f.file_size;
    if (f.open) fat_close(&f);

    k_stat_t st;   // #567: fill kernel-local, then one fault-safe copy_to_user
    memset(&st, 0, sizeof(st));
    st.st_mode    = is_dir ? (0040000u | 0755u) : (0100000u | 0644u);
    st.st_nlink   = 1;
    st.st_size    = (long)size;
    st.st_blksize = 512;
    st.st_blocks  = ((long)size + 511) / 512;
    if (copy_to_user(ubuf, &st, sizeof(st)) != 0) return -14;
    return 0;
}

// ===== #317 pass 2: SMB network mount control syscalls =====================
// sys_net_mount: establish (and cache) an authenticated connection to an SMB
// share so subsequent "/SMB/<server>/<share>/..." file access reuses it. The
// Files app calls this before navigating into a saved network location so that
// per-mount credentials (not just the guest default) are used.
int64_t sys_net_mount(const char *server, const char *share,
                      const char *user, const char *pass) {
    if (!server || !share) return -1;
    return smb_vfs_mount_creds(server, share, user, pass) == 0 ? 0 : -1;
}

// sys_net_list_shares: enumerate the shares a server exports (srvsvc/IPC$).
// Writes share names newline-separated into ubuf; returns the count (>=0) or -1.
int64_t sys_net_list_shares(const char *u_server, char *ubuf, uint32_t maxlen) {
    if (!u_server || !ubuf || maxlen == 0) return -1;
    // #567: bounce the server name, build the listing in a kernel buffer, then
    // one fault-safe copy_to_user (was raw memcpy/ubuf[off++] into user memory).
    char server[256];
    if (sc_bounce_str(u_server, server, sizeof(server)) != 0) return -1;
    extern uint32_t smb_resolve_ip(const char *host);
    uint32_t ip = smb_resolve_ip(server);
    if (!ip) return -1;
    int count = 0;
    char **shares = smb_list_shares(ip, &count);
    if (!shares) return -1;
    char *kbuf = (char *)kmalloc(maxlen);
    if (!kbuf) { smb_free_shares(shares, count); return -1; }
    uint32_t off = 0;
    for (int i = 0; i < count; i++) {
        const char *nm = shares[i] ? shares[i] : "";
        uint32_t nl = (uint32_t)strlen(nm);
        if (off + nl + 1 >= maxlen) break;
        memcpy(kbuf + off, nm, nl); off += nl;
        kbuf[off++] = '\n';
    }
    uint32_t term = (off < maxlen) ? off : maxlen - 1;
    kbuf[term] = 0;
    smb_free_shares(shares, count);
    int rc = (copy_to_user(ubuf, kbuf, term + 1) != 0) ? -14 : count;
    kfree(kbuf);
    return rc;
}

// sys_net_unmount: tear down an SMB share connection.
int64_t sys_net_unmount(const char *u_server, const char *u_share) {
    if (!u_server || !u_share) return -1;
    // #567: bounce both user strings fault-safe before building the mount path.
    char server[256], share[256];
    if (sc_bounce_str(u_server, server, sizeof(server)) != 0) return -1;
    if (sc_bounce_str(u_share,  share,  sizeof(share))  != 0) return -1;
    char mp[300];
    snprintf(mp, sizeof(mp), "/SMB/%s/%s", server, share);
    return smb_unmount(mp) == 0 ? 0 : -1;
}

// HTTP/HTTPS fetch (#http): wrap the kernel https_get() so userland (widgets)
// can pull JSON from web APIs. Blocking; https.c self-pumps net_poll().
// Decode an image (BMP/PNG/JPEG) from `data`[len] and point-sample it down to fit
// the target box packed in `target` (tw<<16 | th), writing BGRA pixels to out[out_cap].
// dims[0]/dims[1] receive the produced width/height. Returns bytes written, or -1.
// Point-sampling = the progressive/cheap path (#247): big images never allocate a
// huge userland buffer and downscale fast.
// #549: feed the net connectivity circuit-breaker from every fetch/POST outcome.
// A positive HTTP status means we reached a server (uplink works) - report OK even
// on 4xx/5xx. A transport failure (r<0 and no status: DNS/connect/recv timeout,
// nobody answered) reports a reach-failure toward the NET_FAULTY trip. Neutral
// otherwise. Cheap; safe from the fetch worker threads.
static void net_fetch_report(int r, int status) {
    extern void net_report_reach_ok(void);
    extern void net_report_reach_fail(void);
    if (status > 0)  net_report_reach_ok();
    else if (r < 0)  net_report_reach_fail();
}
// #549: once the interface is NET_FAULTY, no fetch touches the wire again (this is
// what makes the USB busy-poll storm stop mid-cycle and CPU fall to ~0). Recovery
// is explicit: Settings apply static / renew DHCP, a carrier replug, or a fresh
// DHCP bind all clear the fault (see net.c / sys_net_set_static / SYS_NET_DHCP).
// #567: was net_fetch_blocked(int *ustatus) which raw-wrote *ustatus. The status
// zeroing is now the caller's job (via copy_to_user) so this never touches a user
// pointer; it just answers "is the interface tripped NET_FAULTY".
// #549 FIX (2026-08-10): ask the probe budget, not the raw flag. While the
// interface is healthy this is net_fetch_probe_take() returning 1 immediately.
// While NET_FAULTY it refuses everything EXCEPT one re-probe per 30s, so the
// stack can still obtain the completed transfer that clears the fault. Before
// this, the gate blocked that transfer too and NET_FAULTY was a one-way door.
static int net_fetch_blocked(void) {
    extern int net_fetch_probe_take(void);
    return net_fetch_probe_take() ? 0 : 1;
}

int64_t sys_http_fetch(const char *uurl, char *ubuf, uint32_t max_len, uint32_t *ubytes, int *ustatus) {
    if (!uurl || !ubuf || max_len == 0) return -1;
    if (net_fetch_blocked()) {
        if (ustatus) { int z = 0; (void)copy_to_user(ustatus, &z, sizeof(z)); }
        return -1;
    }
    // #567: bounce the url + write results back fault-safe.
    char *url = sc_dup_user_str(uurl, 8192);
    if (!url) return -1;
    extern int https_get(const char *url, uint8_t **body_out, uint32_t *body_len_out, int *status_out);
    extern int wget_fetch(const char *url, uint8_t **body_out, uint32_t *body_len_out, int *status_out);
    uint8_t *body = 0; uint32_t blen = 0; int status = 0;
    int https = (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]=='s');
    int r = https ? https_get(url, &body, &blen, &status)
                  : wget_fetch(url, &body, &blen, &status);
    net_fetch_report(r, status);
    kfree(url);
    if (r < 0 || !body) {
        if (body) kfree(body);
        if (ustatus) (void)copy_to_user(ustatus, &status, sizeof(status));
        return -1;
    }
    uint32_t n = (blen < max_len) ? blen : max_len;
    int rc = 0;
    if (n && copy_to_user(ubuf, body, n) != 0) rc = -14;
    kfree(body);
    if (rc) return rc;
    if (ubytes  && copy_to_user(ubytes,  &n,      sizeof(n))      != 0) return -14;
    if (ustatus && copy_to_user(ustatus, &status, sizeof(status)) != 0) return -14;
    return (int64_t)n;
}

// #414 Home Assistant: blocking GET with an Authorization header. Runs in the
// background haservice process the same inline way sys_http_fetch does (which
// netinfo already uses safely from Ring 3); never on the compositor UI thread.
static int64_t sys_http_fetch_hdr_inner(const char *uurl, const char *uheaders,
                                        char *ubuf, uint32_t max_len,
                                        uint32_t *ubytes, int *ustatus);
int64_t sys_http_fetch_hdr(const char *uurl, const char *uheaders, char *ubuf,
                           uint32_t max_len, uint32_t *ubytes, int *ustatus) {
    uint64_t _dp_t0 = dp_tsc();
    int64_t _dp_r = sys_http_fetch_hdr_inner(uurl, uheaders, ubuf, max_len,
                                             ubytes, ustatus);
    g_dp_fetch_cyc += dp_tsc() - _dp_t0; g_dp_fetch_calls++;
    return _dp_r;
}
static int64_t sys_http_fetch_hdr_inner(const char *uurl, const char *uheaders,
                                        char *ubuf, uint32_t max_len,
                                        uint32_t *ubytes, int *ustatus) {
    if (!uurl || !ubuf || max_len == 0) return -1;
    if (net_fetch_blocked()) {
        if (ustatus) { int z = 0; (void)copy_to_user(ustatus, &z, sizeof(z)); }
        return -1;
    }
    // #567: bounce url + headers, write results back fault-safe.
    char *url = sc_dup_user_str(uurl, 8192);
    if (!url) return -1;
    char *headers = uheaders ? sc_dup_user_str(uheaders, 16384) : 0;
    if (uheaders && !headers) { kfree(url); return -1; }
    extern int wget_fetch_hdr(const char *, const char *, uint8_t **, uint32_t *, int *);
    extern int https_get_hdr(const char *, const char *, uint8_t **, uint32_t *, int *);
    uint8_t *body = 0; uint32_t blen = 0; int status = 0;
    // #576: dispatch on the URL scheme, mirroring sys_http_fetch's https_get/
    // wget_fetch split, so an https:// Range/header fetch goes over the TLS
    // transport instead of failing on the plaintext-only wget path.
    int https = (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]=='s');
    int r = https ? https_get_hdr(url, headers ? headers : "", &body, &blen, &status)
                  : wget_fetch_hdr(url, headers ? headers : "", &body, &blen, &status);
    net_fetch_report(r, status);
    kfree(url); if (headers) kfree(headers);
    if (ustatus && copy_to_user(ustatus, &status, sizeof(status)) != 0) { if (body) kfree(body); return -14; }
    if (r < 0 || !body) { if (body) kfree(body); return -1; }
    uint32_t n = (blen < max_len) ? blen : max_len;
    int rc = 0;
    if (n && copy_to_user(ubuf, body, n) != 0) rc = -14;
    kfree(body);
    if (rc) return rc;
    if (ubytes && copy_to_user(ubytes, &n, sizeof(n)) != 0) return -14;
    return (int64_t)n;
}

// ===== Async HTTP fetch: background worker threads so the browser UI never
// blocks during a download (#277). A small fixed job table; each START spawns a
// kernel thread running the (blocking) https_get/wget_fetch into a kernel
// buffer; the caller POLLs each frame and READs the body when done. =====
#define ASYNC_FETCH_MAX 6
typedef struct {
    volatile int state;     // 0=running, 1=done, 2=error
    int status;             // HTTP status
    uint8_t *body;
    uint32_t len;
    int slot;               // own index; the worker names itself to fetchown with it
    char url[1024];
    http_progress_t prog;   // #25: live phase/bytes_recv/content_len, read by SYS_HTTP_FETCH_PROGRESS
} async_fetch_t;
static async_fetch_t g_async_fetch[ASYNC_FETCH_MAX];

// =========================================================================
// #745 (task #36): WHO OWNS A JOB SLOT, AND WHEN DOES IT COME BACK.
//
// `volatile int in_use` and `volatile int detached` used to live in the two
// job structs above and below. They are gone, and the slot lifetime is now
// one atomic word per slot in rustkern/fetchown.rs, because the C had two
// measured defects that were both properties of that bookkeeping:
//
//   * poll/read/cancel/progress checked `id in range && in_use`, and nothing
//     else. Any Ring 3 process could read another process's response body
//     (the App Store's signed manifest, every LLM POST reply) or destroy its
//     transfer by passing an index it never allocated. A missing check on an
//     INDEX does not care about uid, so autologin-as-root did not mask it.
//   * nothing released a slot when its owner died, so six crashed fetches
//     exhausted the six-slot table until reboot.
//
// The OWNER is a thread-group id, not a raw pid, so an app that starts a
// fetch on one pthread and polls it from another still owns it.
// =========================================================================

// The identity a job is stamped with, and the only identity that may touch it
// afterwards. 0 means "no process context", which fetchown_claim_rs() refuses
// outright rather than treating as a wildcard.
static uint32_t async_owner_id(void) {
    process_t *p = proc_current();
    if (!p) return 0;
    return p->tgid ? p->tgid : p->pid;
}

// THE ONE REFUSAL VALUE. "no such job", "that job is finished" and "that job
// belongs to another process" all return -1 to Ring 3, deliberately: a
// distinct EPERM would itself be an occupancy oracle, telling an attacker
// exactly which slots are in use by somebody else, which is half of what the
// ownership check is here to withhold. The kernel keeps the distinction for
// the audit line below; userland never sees it.
#define FETCHOWN_EREFUSED (-1)

// Rate-limited so a hostile app cannot turn the audit into a serial-console
// flood (the log is a shared resource; see blame.md on a rate limit that
// fixes an oracle and hands you a DoS). The first burst is verbose because
// that is what makes the guard OBSERVABLE FIRING rather than merely present.
static uint32_t g_fetchown_refusals;
static int async_job_auth(uint32_t tab, int id, int max_slots, const char *what) {
    if (id < 0 || id >= max_slots) return FETCHOWN_EREFUSED;
    uint32_t owner = async_owner_id();
    int r = fetchown_check_rs(tab, (uint32_t)id, owner);
    if (r == 0) return 0;
    if (r == -2) {
        uint32_t n = ++g_fetchown_refusals;
        if (n <= 32u || (n % 64u) == 0u) {
            kprintf("[FETCHSEC] REFUSED %s tab=%u slot=%d caller=%u owner=%u (n=%u)\n",
                    what, tab, id, owner, fetchown_owner_rs(tab, (uint32_t)id), n);
        }
    }
    return FETCHOWN_EREFUSED;
}

extern void thread_exit(int) __attribute__((noreturn));
static void async_fetch_worker(void *arg) {
    async_fetch_t *j = (async_fetch_t *)arg;
    extern int https_get(const char *, uint8_t **, uint32_t *, int *);
    extern int wget_fetch(const char *, uint8_t **, uint32_t *, int *);
    uint8_t *body = 0; uint32_t len = 0; int status = 0;
    int https = (j->url[0]=='h' && j->url[1]=='t' && j->url[2]=='t' &&
                 j->url[3]=='p' && j->url[4]=='s');
    // #25: opt this worker thread into progress reporting for the duration of
    // the blocking call below; https.c/wget.c read this back via
    // net_progress_current() and publish real phase/byte-count transitions.
    // Cleared before proc_exit so a recycled process_t never inherits it.
    proc_current()->net_progress = &j->prog;
    int r = https ? https_get(j->url, &body, &len, &status)
                  : wget_fetch(j->url, &body, &len, &status);
    proc_current()->net_progress = 0;
    net_fetch_report(r, status);   // #549 circuit-breaker
    j->status = status;
    if (r >= 0 && body) { j->body = body; j->len = len; j->state = 1; }
    else { if (body) kfree(body); j->len = 0; j->state = 2; }
    // #25: the terminal phase reflects the real outcome, set here rather than
    // inside https.c/wget.c, which have too many internal early-return error
    // paths to instrument individually.
    j->prog.phase = (r >= 0 && body) ? HTTP_PHASE_DONE : HTTP_PHASE_ERROR;
    // task #36: LAST action that touches this record. It clears the "a worker
    // is still live on this slot" bit, which is what finally returns an
    // orphaned slot to the pool, and returns 1 iff the owner went away while
    // we were running, in which case nobody is left to read the body.
    if (fetchown_worker_done_rs(FETCHOWN_TAB_FETCH, (uint32_t)j->slot)) {
        if (j->body) kfree(j->body);
        j->body = 0;
    }
    { extern void proc_exit(int); proc_exit(0); }
}

int64_t sys_http_fetch_start(const char *uurl) {
    if (!uurl) return -1;
    // #549: no wire work while NET_FAULTY except the paced re-probe. The refusal
    // is reported as NET_ERR_FAULTY, not -1, so the caller can say WHY.
    if (net_fetch_blocked()) return NET_ERR_FAULTY;
    // task #36: claim-and-stamp in ONE atomic step. The old code scanned for a
    // slot with in_use==0, then ran a faultable strncpy_from_user, and only
    // THEN set in_use, so two callers racing here could be handed the SAME
    // slot. Claiming first also means the slot is unallocatable for the whole
    // window in which it is half-built.
    int slot = fetchown_claim_rs(FETCHOWN_TAB_FETCH, async_owner_id());
    if (slot < 0) return -1;
    async_fetch_t *j = &g_async_fetch[slot];
    j->state = 0; j->status = 0; j->body = 0; j->len = 0; j->slot = slot;
    j->prog.phase = HTTP_PHASE_IDLE; j->prog.bytes_recv = 0; j->prog.content_len = 0;   // #25
    // #567: fault-safe copy of the url straight into the kernel job buffer.
    if (strncpy_from_user(j->url, uurl, sizeof(j->url)) < 0) {
        fetchown_abandon_rs(FETCHOWN_TAB_FETCH, (uint32_t)slot);
        return -1;
    }
    extern int proc_create_ex(const char *, void (*)(void *), void *, process_priority_t, uint32_t);
    int tid = proc_create_ex("httpfetch", async_fetch_worker, j, PRIO_NORMAL, 128 * 1024);  // #264 big stack for TLS/HTTPS (#277 was thread_create_kernel)
    if (tid < 0) { fetchown_abandon_rs(FETCHOWN_TAB_FETCH, (uint32_t)slot); return -1; }
    return slot;
}

int64_t sys_http_fetch_poll(int id, int *ustatus, uint32_t *ulen) {
    if (async_job_auth(FETCHOWN_TAB_FETCH, id, ASYNC_FETCH_MAX, "poll") != 0) return FETCHOWN_EREFUSED;
    async_fetch_t *j = &g_async_fetch[id];
    // #567: fault-safe out-param writes.
    if (ustatus) { int s = j->status; if (copy_to_user(ustatus, &s, sizeof(s)) != 0) return -14; }
    if (ulen)    { uint32_t l = j->len; if (copy_to_user(ulen, &l, sizeof(l)) != 0) return -14; }
    return j->state;
}

int64_t sys_http_fetch_read(int id, char *ubuf, uint32_t max) {
    if (async_job_auth(FETCHOWN_TAB_FETCH, id, ASYNC_FETCH_MAX, "read") != 0) return FETCHOWN_EREFUSED;
    async_fetch_t *j = &g_async_fetch[id];
    if (j->state == 0) return -2;   // still running
    uint32_t n = 0;
    int rc = 0;
    if (j->state == 1 && j->body && ubuf) {
        n = (j->len < max) ? j->len : max;
        // #567: fault-safe copy of the response body to userland.
        if (n && copy_to_user(ubuf, j->body, n) != 0) rc = -14;
    }
    if (j->body) kfree(j->body);
    j->body = 0;
    fetchown_release_rs(FETCHOWN_TAB_FETCH, (uint32_t)id);
    return rc ? rc : (int64_t)n;
}

int64_t sys_http_fetch_cancel(int id) {
    if (async_job_auth(FETCHOWN_TAB_FETCH, id, ASYNC_FETCH_MAX, "cancel") != 0) return FETCHOWN_EREFUSED;
    async_fetch_t *j = &g_async_fetch[id];
    // Still running: hand the record to the worker, which frees the body and
    // releases the slot when it unwinds. Already finished: no worker will ever
    // touch it again, so free it here and release immediately.
    if (j->state == 0) { fetchown_orphan_rs(FETCHOWN_TAB_FETCH, (uint32_t)id); }
    else { if (j->body) kfree(j->body); j->body = 0;
           fetchown_release_rs(FETCHOWN_TAB_FETCH, (uint32_t)id); }
    return 0;
}

// #25: read back the live phase/bytes_recv/content_len for an in-flight (or
// just-finished) job, so the browser chrome can show real progress instead of
// a fake animation. Any out pointer may be NULL. Valid for the same id range
// as poll/read/cancel above (i.e. until READ or CANCEL frees the slot).
int64_t sys_http_fetch_progress(int id, int *uphase, uint32_t *ubytes, uint32_t *ucontent_len) {
    if (async_job_auth(FETCHOWN_TAB_FETCH, id, ASYNC_FETCH_MAX, "progress") != 0) return FETCHOWN_EREFUSED;
    async_fetch_t *j = &g_async_fetch[id];
    int ph = j->prog.phase;
    uint32_t br = j->prog.bytes_recv;
    uint32_t cl = j->prog.content_len;
    if (uphase       && copy_to_user(uphase, &ph, sizeof(ph)) != 0) return -14;
    if (ubytes       && copy_to_user(ubytes, &br, sizeof(br)) != 0) return -14;
    if (ucontent_len && copy_to_user(ucontent_len, &cl, sizeof(cl)) != 0) return -14;
    return 0;
}

// HTTPS POST: url, extra headers (CRLF lines, e.g. Authorization + Content-Type),
// JSON body -> response body into ubuf (cap max_len). Returns bytes written, or
// -1. *ustatus gets the HTTP status. HTTP-only (http://) returns 501.
// ===== #264: Async HTTPS POST worker. The synchronous SYS_HTTP_POST path ran
// net code INLINE on the Ring-3 caller after juggling its CR3 to the kernel
// master; an IRQ/context-switch taken inside that borrowed-CR3 window left the
// caller running net code (and bkl_acquire) on a stale/wrong CR3, which hard-
// wedged the whole OS (CPU0 spinning in bkl_acquire, all APs HLT). The browser's
// async GET never had this because a genuine PRIV_KERNEL worker proc does ALL the
// net work on a real kernel CR3 while the user app only POLLS (non-blocking).
static char *kstrdup_opt(const char *src) {
    // #567: fault-safe copy of a user string (was strlen()+memcpy(): an unbounded
    // scan of a possibly-unterminated user string plus a TOCTOU-unsafe read).
    // Every caller is a user-only branch (kernel callers short-circuit before
    // this), so a strncpy_from_user contract is correct. NULL -> "" (preserved).
    if (!src) { char *d = (char *)kmalloc(1); if (d) d[0] = 0; return d; }
    // #616: this asked for 4MB. validate_user_string() REJECTS any max_len over
    // 1MB outright (VALIDATE_ARRAY_TOO_LARGE), so strnlen_user() returned EFAULT
    // for EVERY string ever passed here and kstrdup_opt() ALWAYS returned NULL.
    // #615 correctly identified the 8-byte scheme bounce as the POST killer and
    // replaced it with this call - swapping a bound that was too small for one
    // that was too large, so every Ring-3 https POST still failed, still
    // silently, and so did every other kstrdup_opt() caller (the sync POST path
    // and sys_print_file's printer/title). MEASURED, not reasoned: an
    // instrumented build printed "[HTTPPOST] reject: kstrdup_opt(url) failed"
    // on a real App Store install. Use the shared ceiling so the two can never
    // disagree again.
    ssize_t n = strnlen_user(src, USER_STRING_MAX);
    if (n < 0) return 0;
    char *d = (char *)kmalloc((size_t)n + 1);
    if (!d) return 0;
    if (n && strncpy_from_user(d, src, (size_t)n + 1) < 0) { kfree(d); return 0; }
    d[n] = 0;
    return d;
}
// We replicate that exactly for POST: START spawns a kernel worker that owns its
// kernel CR3 (proc->cr3==0 -> scheduler keeps it on the kernel master), runs the
// blocking https_post into kernel buffers, and the user app POLLs then READs.
// No CR3 juggling on the user side, ever.
#define ASYNC_POST_MAX 4
typedef struct {
    volatile int state;     // 0=running/queued, 1=done, 2=error
    int status;             // HTTP status
    uint8_t *body;          // response body (kfree'd by READ)
    uint32_t len;
    char *url;              // kernel copies of the request (kfree'd by worker)
    char *headers;
    char *reqbody;
} async_post_t;
static async_post_t g_async_post[ASYNC_POST_MAX];
static volatile int g_post_worker_started = 0;

static void async_post_free_req(async_post_t *j) {
    if (j->url)     { kfree(j->url);     j->url = 0; }
    if (j->headers) { kfree(j->headers); j->headers = 0; }
    if (j->reqbody) { kfree(j->reqbody); j->reqbody = 0; }
}

// #426: the worker's idle wake. File-static, so unlike a wait queue embedded in
// a kmalloc'd object it can never be freed under a waiter. Statically
// initialised (no init call, hence no init-ordering race).
//
// Exactly ONE producer feeds this worker: sys_http_post_start(), which wakes it
// after publishing j->url. That single source is sufficient (no redundancy
// needed) because it is unconditional and the check-then-park race is closed by
// wait_event()'s own re-check after __wait_prepare(). g_async_fetch is a
// SEPARATE table with a per-request worker and does not feed this queue.
static wait_queue_head_t g_post_job_wq = { .head = NULL, .lock = SPINLOCK_INIT };

// A job needs running iff in_use, still state 0, and fully populated (the START
// handler publishes url LAST, so a non-NULL url means the record is complete).
// Same predicate the worker's scan uses, so the park condition and the work
// condition can never disagree.
static int post_job_pending(void) {
    for (int i = 0; i < ASYNC_POST_MAX; i++) {
        async_post_t *j = &g_async_post[i];
        // task #36: `in_use` is gone; `url` is the same signal it was standing
        // in for, and is still published LAST by the START handler and cleared
        // by the worker, so this predicate is unchanged in meaning.
        if (j->state == 0 && j->url) return 1;
    }
    return 0;
}

// ONE persistent worker proc drains the job table forever. No per-POST
// proc_create/proc_exit churn (that churn raced the user-process spawn path and
// faulted the kernel under load, #264/#317). The worker runs as a PRIV_KERNEL
// proc, so the scheduler keeps it on the kernel CR3; it does ALL net work while
// the user app only POLLs.
static void async_post_worker(void *arg) {
    (void)arg;
    extern int https_post(const char *, const char *, const char *,
                          uint8_t **, uint32_t *, int *);
    extern void proc_sleep(uint32_t ms);
    for (;;) {
        int did = 0;
        for (int i = 0; i < ASYNC_POST_MAX; i++) {
            async_post_t *j = &g_async_post[i];
            // A job needs running iff it is still state 0 and has a request
            // buffer (the START handler publishes url last, after claiming the
            // slot from fetchown).
            if (j->state != 0 || !j->url) continue;
            did = 1;
            uint8_t *body = 0; uint32_t len = 0; int status = 0;
            int r = https_post(j->url, j->headers ? j->headers : "",
                               j->reqbody ? j->reqbody : "",
                               &body, &len, &status);
            net_fetch_report(r, status);   // #549 circuit-breaker
            j->status = status;
            if (r >= 0 && body) { j->body = body; j->len = len; }
            else { if (body) kfree(body); j->len = 0; }
            async_post_free_req(j);
            // Publish terminal state LAST so a poller never reads done/error
            // before body/len/status are settled.
            j->state = (r >= 0 && body) ? 1 : 2;
            // task #36: done with this record. Returns 1 iff the owner went
            // away while we were running, so nobody is left to read the body.
            if (fetchown_worker_done_rs(FETCHOWN_TAB_POST, (uint32_t)i)) {
                if (j->body) kfree(j->body);
                j->body = 0;
            }
        }
        // #426: was `if (!did) proc_sleep(10);`, a 10ms idle poll that woke this
        // worker 100 times a second forever to almost always find nothing, and
        // that added up to 10ms of latency to every POST. Now it BLOCKS until a
        // job is actually submitted: no timeout, because the only thing that can
        // end this wait is an event we own and always signal, and a worker with
        // no work should wait indefinitely.
        if (!did) (void)wait_event(&g_post_job_wq, post_job_pending());
    }
}

static void ensure_post_worker(void) {
    if (g_post_worker_started) return;
    extern int proc_create_ex(const char *, void (*)(void *), void *, process_priority_t, uint32_t);
    int tid = proc_create_ex("httppost", async_post_worker, 0, PRIO_NORMAL, 128 * 1024); // #264 big stack for TLS/HTTPS
    if (tid >= 0) g_post_worker_started = 1;
}

// START: copy the request OUT of user memory (we are on the caller's CR3 now, so
// user pointers are valid) into kernel buffers, queue it, and make sure the
// persistent worker exists. Returns a job id, or -1. http:// is rejected.
int64_t sys_http_post_start(const char *uurl, const char *uheaders, const char *ubody) {
    if (!uurl) { kprintf("[HTTPPOST] reject: null url\n"); return -1; }
    if (net_fetch_blocked()) { kprintf("[HTTPPOST] reject: NET_FAULTY\n"); return -1; }
    // #615: THIS CHECK KILLED EVERY RING-3 HTTPS POST IN THE OS.
    //
    // #567 replaced a raw url[0..4] read with a "fault-safe" 8-byte bounce:
    //     char scheme[8];
    //     if (strncpy_from_user(scheme, uurl, sizeof(scheme)) < 0) return -1;
    // but strncpy_from_user() -> validate_user_string() returns
    // VALIDATE_STRING_UNTERMINATED (i.e. EFAULT) when it does not find a NUL
    // inside max_len. A real URL is longer than 8 bytes, so this returned -1 for
    // EVERY well-formed request, before any network I/O and without a single log
    // line. Measured this pass: a full App Store install completed with ZERO
    // POSTs on the wire and ZERO POST lines on serial, while the identical POST
    // issued from inside the kernel (the NETTEST probe, which calls https_post()
    // directly) returned 200 from the same endpoint on the same boot. That is
    // the whole "guest-side POST silently fails" bug.
    //
    // Copy the URL FIRST with the primitive that is actually built for an
    // arbitrary-length user string (kstrdup_opt -> strnlen_user with a 4MB
    // bound), then test the scheme on the kernel copy. The https-only policy is
    // unchanged and still enforced before anything is queued.
    char *ku = kstrdup_opt(uurl);
    if (!ku) { kprintf("[HTTPPOST] reject: kstrdup_opt(url) failed\n"); return -1; }
    int https = (ku[0]=='h'&&ku[1]=='t'&&ku[2]=='t'&&ku[3]=='p'&&ku[4]=='s'&&ku[5]==':');
    if (!https) { kprintf("[HTTPPOST] reject: non-https '%s'\n", ku); kfree(ku); return -1; }
    // task #36: same atomic claim-and-stamp as the GET table.
    int slot = fetchown_claim_rs(FETCHOWN_TAB_POST, async_owner_id());
    if (slot < 0) { kfree(ku); return -1; }
    async_post_t *j = &g_async_post[slot];
    j->state = 0; j->status = 0; j->body = 0; j->len = 0;
    j->url = 0; j->headers = 0; j->reqbody = 0;
    char *kh = kstrdup_opt(uheaders);
    char *kb = kstrdup_opt(ubody);
    if (!kh || !kb) {
        fetchown_abandon_rs(FETCHOWN_TAB_POST, (uint32_t)slot);
        kfree(ku);
        if (kh) kfree(kh);
        if (kb) kfree(kb);
        return -1;
    }

    // =======================================================================
    // #745 THE PROMPT-INJECTION CHOKEPOINT.
    //
    // This is the ONE funnel every LLM client in the tree already passes
    // through (userland/libc/aiclient.c and the separate userland/apps/paint/
    // ai.c both call http_post_start), and the request body is sitting in
    // KERNEL memory here, fully assembled, before anything is queued. Screening
    // at this single point covers every existing route AND every route that
    // does not exist yet, which a per-client guard cannot: paint/ai.c is a
    // second, independent LLM client that would never have known about one.
    //
    // aiguard_screen_post_rs() returns ALLOW immediately for any POST that is
    // not an LLM request, so the App Store, the #294 build service and
    // ClassiCube pay one substring scan and nothing else.
    //
    // POLICY: HIGH severity BLOCKS. Lower severities are allowed and AUDITED.
    // Nothing here is silent in either direction: a block returns a DISTINCT
    // code (NET_ERR_AIGUARD, not the generic -1) so the client can say what
    // happened, and both outcomes write a record naming the actor pid, the
    // rule, the severity and the literal that matched.
    // =======================================================================
    {
        int blen = 0;
        while (kb[blen]) blen++;
        aiguard_verdict_t v;
        int verdict = aiguard_screen_post_rs(kb, (uint64_t)blen, &v);
        if (v.llm && verdict != AIGUARD_ALLOW) {
            process_t *ap = proc_current();
            unsigned int apid = ap ? (unsigned int)ap->pid : 0u;
            char det[160];
            snprintf(det, sizeof(det),
                     "%s llm-post rule=%s sev=%d matched=%s%s",
                     verdict == AIGUARD_BLOCK ? "BLOCKED" : "flagged",
                     v.rule, v.severity, v.matched,
                     v.truncated ? " (scan truncated)" : "");
            seclog_report_ai_injection(apid, det);
            kprintf("[AIGUARD] %s pid=%u rule=%s cat=%s sev=%d matched='%s'\n",
                    verdict == AIGUARD_BLOCK ? "BLOCK" : "annotate",
                    apid, v.rule, v.category, v.severity, v.matched);
            if (verdict == AIGUARD_BLOCK) {
                fetchown_abandon_rs(FETCHOWN_TAB_POST, (uint32_t)slot);   // task #36
                kfree(ku); kfree(kh); kfree(kb);
                return NET_ERR_AIGUARD;
            }
        }
    }

    kprintf("[HTTPPOST] queued slot=%d %s\n", slot, ku);
    j->headers = kh; j->reqbody = kb;
    j->url = ku;                 // published LAST: the worker only picks the job
                                 // up once the record is fully populated
    ensure_post_worker();
    // #426: wake the (now certainly existing) worker. Must come AFTER url is
    // published, or the worker could wake, run post_job_pending(), see an
    // incomplete record and park again with the job stranded. Harmless if the
    // worker was only just created and has not parked yet: no waiter means this
    // is a lock and a NULL check, and its first action is a full table scan.
    wake_up_all(&g_post_job_wq);
    return slot;
}

// #745 SYS_AI_SCAN: screen ONE untrusted string against the kernel-owned
// ruleset and hand the caller the rule that fired.
//
// This is deliberately INFORMATIONAL. It cannot be used to turn screening off
// and it is not what makes the guard binding: a client that never calls it is
// still blocked at sys_http_post_start(). It exists because a silent block is
// its own bug, and a client can only tell the user "a tool observation from
// files.read matched DirectPromptInjection (HIGH) on 'ignore all previous
// instructions'" if something gives it those words.
int64_t sys_ai_scan(const char *utext, void *uout) {
    if (!utext || !uout) return -1;
    char *kt = kstrdup_opt(utext);
    if (!kt) return -14;
    int len = 0;
    while (kt[len]) len++;

    aiguard_verdict_t v;
    int verdict = aiguard_screen_rs(kt, (uint64_t)len, &v);
    kfree(kt);

    if (verdict != AIGUARD_ALLOW) {
        process_t *ap = proc_current();
        unsigned int apid = ap ? (unsigned int)ap->pid : 0u;
        char det[160];
        snprintf(det, sizeof(det), "scan %s rule=%s sev=%d matched=%s",
                 verdict == AIGUARD_BLOCK ? "HIGH" : "low", v.rule,
                 v.severity, v.matched);
        seclog_report_ai_injection(apid, det);
    }
    if (copy_to_user(uout, &v, sizeof(v)) != 0) return -14;
    return verdict;
}

int64_t sys_http_post_poll(int id, int *ustatus, uint32_t *ulen) {
    if (async_job_auth(FETCHOWN_TAB_POST, id, ASYNC_POST_MAX, "post_poll") != 0) return FETCHOWN_EREFUSED;
    async_post_t *j = &g_async_post[id];
    // #567: fault-safe out-param writes.
    if (ustatus) { int s = j->status; if (copy_to_user(ustatus, &s, sizeof(s)) != 0) return -14; }
    if (ulen)    { uint32_t l = j->len; if (copy_to_user(ulen, &l, sizeof(l)) != 0) return -14; }
    return j->state;
}

int64_t sys_http_post_read(int id, char *ubuf, uint32_t max) {
    if (async_job_auth(FETCHOWN_TAB_POST, id, ASYNC_POST_MAX, "post_read") != 0) return FETCHOWN_EREFUSED;
    async_post_t *j = &g_async_post[id];
    if (j->state == 0) return -2;   // still running
    uint32_t n = 0;
    int rc = 0;
    if (j->state == 1 && j->body && ubuf) {
        n = (j->len < max) ? j->len : max;
        // #567: fault-safe copy of the response body to userland.
        if (n && copy_to_user(ubuf, j->body, n) != 0) rc = -14;
    }
    if (j->body) kfree(j->body);
    j->body = 0;
    fetchown_release_rs(FETCHOWN_TAB_POST, (uint32_t)id);
    return rc ? rc : (int64_t)n;
}

int64_t sys_http_post_cancel(int id) {
    if (async_job_auth(FETCHOWN_TAB_POST, id, ASYNC_POST_MAX, "post_cancel") != 0) return FETCHOWN_EREFUSED;
    async_post_t *j = &g_async_post[id];
    if (j->state == 0) { fetchown_orphan_rs(FETCHOWN_TAB_POST, (uint32_t)id); }
    else { if (j->body) kfree(j->body); j->body = 0;
           fetchown_release_rs(FETCHOWN_TAB_POST, (uint32_t)id); }
    return 0;
}

// =========================================================================
// #745 (task #36): PROCESS EXIT HOOK. Every termination path in this kernel
// funnels through proc_exit() (a voluntary exit, a fatal signal via
// signal.c's proc_exit(128+signo), and the crash handler in cpu/idt.c all
// call it), so this is the one place that can close the leak.
//
// Runs on the dying process's own stack, under cli(), from proc_exit(). It
// therefore must not block: it only frees kernel buffers and flips atomics,
// exactly like cleanup_user_windows_for_process() two lines above the call.
//
// A job that is STILL RUNNING cannot be freed here (its worker thread is
// inside https_get and owns the record), so it is ORPHANED: the worker frees
// the body and returns the slot when it unwinds. That is the same handover
// CANCEL has always used. A job that has already finished is freed outright.
// =========================================================================
void async_http_proc_exit(uint32_t owner) {
    if (!owner) return;
    int nf = 0, np = 0;
    for (int i = 0; i < ASYNC_FETCH_MAX; i++) {
        if (!fetchown_owned_by_rs(FETCHOWN_TAB_FETCH, (uint32_t)i, owner)) continue;
        async_fetch_t *j = &g_async_fetch[i];
        if (j->state == 0) {
            fetchown_orphan_rs(FETCHOWN_TAB_FETCH, (uint32_t)i);
        } else {
            if (j->body) kfree(j->body);
            j->body = 0;
            fetchown_release_rs(FETCHOWN_TAB_FETCH, (uint32_t)i);
        }
        fetchown_note_exit_release_rs();
        nf++;
    }
    for (int i = 0; i < ASYNC_POST_MAX; i++) {
        if (!fetchown_owned_by_rs(FETCHOWN_TAB_POST, (uint32_t)i, owner)) continue;
        async_post_t *j = &g_async_post[i];
        if (j->state == 0) {
            fetchown_orphan_rs(FETCHOWN_TAB_POST, (uint32_t)i);
        } else {
            if (j->body) kfree(j->body);
            j->body = 0;
            fetchown_release_rs(FETCHOWN_TAB_POST, (uint32_t)i);
        }
        fetchown_note_exit_release_rs();
        np++;
    }
    if (nf || np) {
        fetchown_stats_t sf, sp;
        fetchown_stats_rs(FETCHOWN_TAB_FETCH, &sf);
        fetchown_stats_rs(FETCHOWN_TAB_POST, &sp);
        kprintf("[FETCHSEC] exit owner=%u released fetch=%d post=%d "
                "(fetch owned=%u orphan=%u, post owned=%u orphan=%u, total=%u)\n",
                owner, nf, np, sf.owned, sf.orphaned, sp.owned, sp.orphaned,
                sf.exit_released);
    }
}

// Boot check: run the ownership state machine's self-test and prove the Rust
// module and these C tables agree about how many slots exist. A silent
// disagreement would make some slots unreachable (Rust smaller) or hand out
// out-of-range indices (Rust larger), so it is checked rather than commented.
void fetchown_boot_check(void) {
    int sf = fetchown_slots_rs(FETCHOWN_TAB_FETCH);
    int sp = fetchown_slots_rs(FETCHOWN_TAB_POST);
    int st = fetchown_selftest_rs();
    kprintf("[RUST-SEC] fetchown selftest=%s slots fetch=%d/%d post=%d/%d\n",
            st == 0 ? "PASS" : "FAIL", sf, ASYNC_FETCH_MAX, sp, ASYNC_POST_MAX);
    if (st != 0 || sf != ASYNC_FETCH_MAX || sp != ASYNC_POST_MAX)
        kprintf("[RUST-SEC] fetchown SELF-TEST FAILED step=%d - async HTTP job "
                "ownership is NOT trustworthy on this build\n", st);
}

// HTTPS POST: url, extra headers (CRLF lines), JSON body -> response body into
// ubuf (cap max_len). Returns bytes written, or -1; *ustatus gets HTTP status.
// #567: kernel-pointer core for the KERNEL-caller path (RC / shell / in-kernel
// tools). url/headers/body/kbuf/status_out are all KERNEL pointers and the
// caller already runs on the kernel CR3, so this runs https_post inline exactly
// as the old !from_user branch did. It is called only from the wrapper below
// (copy-user-lint trace depth 2), so its raw pointer use is out of scope; the
// user-facing wrapper bounces every user pointer before delegating.
static int64_t sys_http_post_k(const char *url, const char *headers, const char *body,
                               char *kbuf, uint32_t max_len, int *status_out) {
    extern int https_post(const char *url, const char *headers, const char *body,
                          uint8_t **body_out, uint32_t *body_len_out, int *status_out);
    extern int wget_post_hdr(const char *, const char *, const char *,
                             uint8_t **, uint32_t *, int *);   // #414 plain-HTTP POST
    if (net_fetch_blocked()) { if (status_out) *status_out = 0; return -1; }   // #549
    int https = (url[0]=='h'&&url[1]=='t'&&url[2]=='t'&&url[3]=='p'&&url[4]=='s');
    uint8_t *rbody = 0; uint32_t blen = 0; int status = 0;
    int r = https ? https_post(url, headers, body, &rbody, &blen, &status)
                  : wget_post_hdr(url, headers ? headers : "", body ? body : "", &rbody, &blen, &status);
    net_fetch_report(r, status);   // #549
    if (r < 0) { if (rbody) kfree(rbody); if (status_out) *status_out = status; return -1; }
    uint32_t n = (blen < max_len) ? blen : max_len;
    if (rbody && n) memcpy(kbuf, rbody, n);
    if (rbody) kfree(rbody);
    if (status_out) *status_out = status;
    return (int64_t)n;
}

int64_t sys_http_post(const char *uurl, const char *uheaders, const char *ubody,
                      char *ubuf, uint32_t max_len, int *ustatus) {
    if (!uurl || !ubuf || max_len == 0) return -1;

    process_t *cur = proc_current();
    int from_user = (cur && cur->privilege == PRIV_USER);

    // Kernel callers already run on the kernel CR3 with kernel pointers: delegate
    // straight to the raw core (#567: its *ustatus write is a kernel write there).
    if (!from_user) {
        return sys_http_post_k(uurl, uheaders, ubody, ubuf, max_len, ustatus);
    }

    // #567: user caller - fault-safe circuit-breaker status, then bounce inputs.
    extern int https_post(const char *url, const char *headers, const char *body,
                          uint8_t **body_out, uint32_t *body_len_out, int *status_out);
    extern int wget_post_hdr(const char *, const char *, const char *,
                             uint8_t **, uint32_t *, int *);
    if (net_fetch_blocked()) {
        if (ustatus) { int z = 0; (void)copy_to_user(ustatus, &z, sizeof(z)); }
        return -1;
    }

    // User caller: https_post() must run on the KERNEL address space (net_poll /
    // eth_receive / e1000 DMA + rings + MMIO live in the identity-mapped LOWER
    // half, absent from a Ring-3 CR3). We run it INLINE on this process after
    // switching it to the kernel address space. This avoids spawning (and tearing
    // down) a worker process per POST: that per-POST proc_create/proc_exit churn
    // is itself a known intermittent kernel-corruption trigger (#264/#317) when a
    // user app drives several POSTs. Mechanics:
    //   - copy the request out of user memory into kernel buffers first
    //   - set cur->cr3 = 0 so the scheduler keeps us on the kernel CR3 across the
    //     proc_sleep() yields inside https_post (a bare `mov cr3` would be undone
    //     by the next context switch, which reloads cur->cr3)
    //   - load the kernel CR3 now, run https_post() inline (the proven path; the
    //     #297 net_lock serializes its NIC/TCP-table access against net_poll)
    //   - restore cur->cr3 + the user CR3 before copying the reply back.
    char *kurl = kstrdup_opt(uurl);
    char *khdr = kstrdup_opt(uheaders);
    char *kbody = kstrdup_opt(ubody);
    if (!kurl || !khdr || !kbody) {
        if (kurl) kfree(kurl);
        if (khdr) kfree(khdr);
        if (kbody) kfree(kbody);
        return -1;
    }
    // #745: the same screen as the async path, and it is LOAD-BEARING, not
    // defensive. A first sweep concluded SYS_HTTP_POST had no live userland
    // caller because the async trio replaced it at #264. That was WRONG:
    // userland/apps/settings/main.c ai_test() posts
    // {"model":...,"messages":[{"role":"user","content":"ping"}]} to whichever
    // provider the user configured, through THIS entry point. Screening only
    // the async path would have left a real LLM route open while the comment
    // above it claimed a chokepoint. Same policy, same audit, same code.
    {
        int blen = 0;
        while (kbody[blen]) blen++;
        aiguard_verdict_t v;
        int verdict = aiguard_screen_post_rs(kbody, (uint64_t)blen, &v);
        if (v.llm && verdict != AIGUARD_ALLOW) {
            unsigned int apid = (unsigned int)cur->pid;
            char det[160];
            snprintf(det, sizeof(det), "%s llm-post(sync) rule=%s sev=%d matched=%s",
                     verdict == AIGUARD_BLOCK ? "BLOCKED" : "flagged",
                     v.rule, v.severity, v.matched);
            seclog_report_ai_injection(apid, det);
            kprintf("[AIGUARD] %s pid=%u rule=%s cat=%s sev=%d matched='%s'\n",
                    verdict == AIGUARD_BLOCK ? "BLOCK" : "annotate",
                    apid, v.rule, v.category, v.severity, v.matched);
            if (verdict == AIGUARD_BLOCK) {
                kfree(kurl); kfree(khdr); kfree(kbody);
                return NET_ERR_AIGUARD;
            }
        }
    }

    // Scheme check on the kernel copy (was a raw url[0..4] read).
    int https = (kurl[0]=='h'&&kurl[1]=='t'&&kurl[2]=='t'&&kurl[3]=='p'&&kurl[4]=='s');

    extern uint64_t vmm_get_pml4(void);
    uint64_t user_cr3 = cur->cr3;
    uint64_t kcr3 = vmm_get_pml4();
    uint64_t flags;

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = 0;                                   // scheduler -> kernel CR3 on resume
    __asm__ volatile("mov %0, %%cr3" :: "r"(kcr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    uint8_t *rbody = 0; uint32_t blen = 0; int status = 0;
    int r = https ? https_post(kurl, khdr, kbody, &rbody, &blen, &status)
                  : wget_post_hdr(kurl, khdr, kbody, &rbody, &blen, &status);
    net_fetch_report(r, status);   // #549

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = user_cr3;
    __asm__ volatile("mov %0, %%cr3" :: "r"(user_cr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    kfree(kurl); kfree(khdr); kfree(kbody);

    // Back on the user CR3: ubuf/ustatus are valid and copy_*_user works. #567.
    if (r < 0) {
        if (rbody) kfree(rbody);
        if (ustatus) (void)copy_to_user(ustatus, &status, sizeof(status));
        return -1;
    }
    uint32_t n = (blen < max_len) ? blen : max_len;
    int rc = 0;
    if (rbody && n && copy_to_user(ubuf, rbody, n) != 0) rc = -14;
    if (rbody) kfree(rbody);
    if (rc) return rc;
    if (ustatus && copy_to_user(ustatus, &status, sizeof(status)) != 0) return -14;
    return (int64_t)n;
}


// ============================================================================
// #318 network printing syscalls (IPP client). The actual IPP/HTTP exchange
// runs over the kernel TCP stack, so for a Ring-3 caller the network call must
// execute on the kernel address space (net_poll/e1000 DMA live in the
// identity-mapped lower half, absent from a user CR3). We mirror the proven
// sys_http_post() trick: copy args out of user memory, run the network call
// inline on the kernel CR3, then restore the user CR3.
// ============================================================================
int64_t sys_print_list(void *out, int max) {
    extern int print_list(void *out, int max);   // printer_cfg_t* erased to void*
    return print_list(out, max);
}

// #700 B5: the printer registry is SYSTEM configuration. print_add() and
// print_remove() both call printers_save(), which rewrites root-owned
// /CONFIG/PRINTERS.CFG, and neither asked who was calling. Measured on golden
// 1025: a uid-1000 process added a printer and the file came off the disk
// holding its entry.
//
// It is not merely a file write. A printer entry is a HOST AND PORT the kernel
// will later open a TCP connection to and POST document bytes at, on behalf of
// anyone who prints, so an unprivileged write here is an unprivileged
// redirection of everybody else's print jobs. Root only, like any other
// system-wide device registration.
//
// The remove side gets the same rule, not because deletion leaks anything, but
// because "add is privileged, remove is not" lets an attacker replace an entry
// in two steps instead of one.
static int print_cfg_caller_is_root(const char *what) {
    process_t *p = proc_current();
    if (!p || p->privilege != PRIV_USER) return 1;   // Ring 0
    if (p->euid == 0) return 1;
    kprintf("[PRINT] DENIED %s: uid=%u is not root; /CONFIG/PRINTERS.CFG is "
            "system configuration\n", what, p->euid);
    return 0;
}

int64_t sys_print_add(const char *name, const char *host, int port,
                      const char *queue, int make_default) {
    extern int print_add(const char *name, const char *host, uint16_t port,
                         const char *queue, int make_default);
    if (!name || !host || !queue) return -1;
    if (!print_cfg_caller_is_root("print_add")) return -1;
    return print_add(name, host, (uint16_t)port, queue, make_default);
}

int64_t sys_print_remove(const char *name) {
    extern int print_remove(const char *name);
    if (!name) return -1;
    if (!print_cfg_caller_is_root("print_remove")) return -1;
    return print_remove(name);
}

int64_t sys_print_job(const char *printer, const char *title, const char *text) {
    extern int print_job_text(const char *printer_name, const char *title, const char *text);
    if (!text) return -1;

    process_t *cur = proc_current();
    int from_user = (cur && cur->privilege == PRIV_USER);
    if (!from_user) {
        return print_job_text(printer, title, text);
    }

    char *kp = kstrdup_opt(printer);
    char *kt = kstrdup_opt(title);
    char *kx = kstrdup_opt(text);
    if (!kx) { if (kp) kfree(kp); if (kt) kfree(kt); if (kx) kfree(kx); return -1; }

    extern uint64_t vmm_get_pml4(void);
    uint64_t user_cr3 = cur->cr3;
    uint64_t kcr3 = vmm_get_pml4();
    uint64_t flags;

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = 0;
    __asm__ volatile("mov %0, %%cr3" :: "r"(kcr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    int r = print_job_text(kp, kt, kx);

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = user_cr3;
    __asm__ volatile("mov %0, %%cr3" :: "r"(user_cr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    if (kp) kfree(kp);
    if (kt) kfree(kt);
    if (kx) kfree(kx);
    return r;
}

// Print an image FILE (path on the FAT disk) to a configured printer. Same
// CR3-switch discipline as sys_print_job: the IPP/HTTP exchange runs on the
// kernel address space.
int64_t sys_print_image(const char *printer, const char *path) {
    extern int print_job_image(const char *printer_name, const char *path);
    if (!path) return -1;

    process_t *cur = proc_current();
    int from_user = (cur && cur->privilege == PRIV_USER);
    if (!from_user) return print_job_image(printer, path);

    char *kp = kstrdup_opt(printer);
    char *kpath = kstrdup_opt(path);
    if (!kpath) { if (kp) kfree(kp); return -1; }

    // #700 B3: the caller names a PATH and the kernel reads it, in Ring 0, with
    // no check, and then POSTs the bytes to a network host. That is an arbitrary
    // read joined to an exfiltration channel, and it is reachable by chaining
    // B5: add a printer pointing anywhere, then print any file to it. MEASURED
    // on golden 1025 at uid 1000: "[PRINT] image decode of /CONFIG/KIMI.KEY
    // failed (-3)" - the decode failed only because a key file is not a JPEG;
    // fat_read_file() had already handed the kernel its contents, and the
    // .jpg/.jpeg branch above ships the raw bytes with no decode at all.
    //
    // The check is on the KERNEL copy, after kstrdup_opt, so it cannot be raced
    // with the read that follows it (#509).
    {
        process_t *pi = proc_current();
        if (pi && pi->privilege == PRIV_USER &&
            perms_check(kpath, pi->euid, pi->egid, R_OK) != 0) {
            if (kp) kfree(kp);
            kfree(kpath);
            return -1;
        }
    }

    extern uint64_t vmm_get_pml4(void);
    uint64_t user_cr3 = cur->cr3;
    uint64_t kcr3 = vmm_get_pml4();
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = 0;
    __asm__ volatile("mov %0, %%cr3" :: "r"(kcr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    int r = print_job_image(kp, kpath);

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = user_cr3;
    __asm__ volatile("mov %0, %%cr3" :: "r"(user_cr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    if (kp) kfree(kp);
    kfree(kpath);
    return r;
}

// Print the current framebuffer (screen) to a configured printer.
int64_t sys_print_screen(const char *printer) {
    extern int print_job_screen(const char *printer_name);
    process_t *cur = proc_current();
    int from_user = (cur && cur->privilege == PRIV_USER);
    if (!from_user) return print_job_screen(printer);

    char *kp = kstrdup_opt(printer);

    extern uint64_t vmm_get_pml4(void);
    uint64_t user_cr3 = cur->cr3;
    uint64_t kcr3 = vmm_get_pml4();
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = 0;
    __asm__ volatile("mov %0, %%cr3" :: "r"(kcr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    int r = print_job_screen(kp);

    __asm__ volatile("pushfq; pop %0" : "=r"(flags));
    __asm__ volatile("cli");
    cur->cr3 = user_cr3;
    __asm__ volatile("mov %0, %%cr3" :: "r"(user_cr3) : "memory");
    if (flags & 0x200) __asm__ volatile("sti");

    if (kp) kfree(kp);
    return r;
}

// #307 real-hardware bring-up: let userland (the compositor's login screen,
// which is where the physical iMac14,4 "No user accounts found" bug is
// actually visible) append a line to the same persistent /BOOTLOG.TXT the
// kernel writes to. Bounded copy directly from the caller's own address
// space (same pattern as other simple string-arg syscalls; no CR3 switching
// needed since bootlog_write() never touches user memory itself, only the
// short kernel-side copy made here). Never fails loudly - logging must not
// be able to crash or block whatever is trying to log.
//
// #700 B9: this endpoint stays OPEN to every Ring-3 caller, deliberately, and
// that is a decision rather than an omission. /BOOTLOG.TXT is the only durable
// diagnostic channel on the real iMac, the login screen that needs it runs as
// whoever is logging in, and a log you must be root to write is a log that is
// empty exactly when it matters. Two things were wrong and both are fixed here:
//
//  1. NEWLINE INJECTION. The message was interpolated verbatim, so a caller
//     could embed "\n" and forge ADDITIONAL log lines that carry no [USERSPACE]
//     marker at all. MEASURED on golden 1025: a uid-1000 process wrote
//     "sgp-benign\n[KERNEL] FORGED-BY-UID-1000 all checks passed" and the file
//     came off the disk with the second line present, indistinguishable from a
//     kernel line. Every byte below 0x20 and 0x7F is now replaced, so one call
//     is one line, always.
//  2. NO ATTRIBUTION. "[USERSPACE]" says a Ring-3 process wrote it and nothing
//     more. The uid is now stamped by the KERNEL, from the calling process,
//     where the caller cannot choose it.
//
// HONEST LIMIT: this makes forged lines unforgeable and every line attributable.
// It does NOT stop an unprivileged process from appending to a root-owned file,
// and it is not a rate limit: a caller can still fill the log.
int64_t sys_bootlog_write(const char *umsg) {
    if (!umsg) return -1;
    // #567: bounce the user message fault-safe into the kernel buffer.
    char buf[200];
    if (sc_bounce_str(umsg, buf, sizeof(buf)) != 0) return -1;

    // #700 B9: one call is one line. Control characters (in particular \n and
    // \r) become '.', so the caller cannot manufacture a record boundary.
    for (int i = 0; buf[i]; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x20 || c == 0x7F) buf[i] = '.';
    }

    process_t *p = proc_current();
    uint32_t who = (p && p->privilege == PRIV_USER) ? p->euid : 0;
    bootlog_write("[USERSPACE uid=%u] %s", who, buf);
    return 0;
}

// ============================================================================
// Memory syscalls
// ============================================================================

// Default heap start: just past the ELF load region.
// The actual per-process value is in process_t.brk.
// #487: the heap base now lives in process.h as PROC_DEFAULT_BRK_START so the
// per-process memory accounting (proc/procmem.c) can size the brk heap from the
// same constant. Aliased here to keep this file's existing call sites unchanged.
#define DEFAULT_BRK_START   PROC_DEFAULT_BRK_START

// mmap region lives between heap and stack (within PDPT[2], 2-3GB range).
// Start below the user stack to avoid colliding with kernel PDPT entries
// above 3GB that share read-only UEFI page directories.
// #522 stage 2: the anonymous-mmap arena now lives in the DEDICATED USER WINDOW
// (mm/vmm.h USER_WIN_MMAP_BASE, PML4[1] +4GB), not in PDPT[2].
//
// The old value, 0xA0000000, sat 256MB INSIDE libc's declared heap maximum
// (HEAP_START 0x90000000 + HEAP_MAX_SIZE 512MB reaches 0xB0000000) and grew
// upward toward the user stack at 0xBFDF0000 with nothing to stop it. It shared
// that 1GB with the app image and, on real hardware, with the framebuffer.
// The new arena has 4GB of clear space below the stack arena and no device
// memory anywhere near it.
#define DEFAULT_MMAP_START  USER_WIN_MMAP_BASE

int64_t sys_brk(uint64_t addr) {
    process_t *p = proc_current();
    if (!p || p->privilege != PRIV_USER) {
        return -1;
    }

    if (p->brk == 0) {
        p->brk = DEFAULT_BRK_START;
    }

    if (addr == 0) {
        return (int64_t)p->brk;
    }

    // Expand/contract heap
    uint64_t old_brk = p->brk;
    uint64_t new_brk = (addr + 0xFFF) & ~0xFFF;  // Page align

    if (new_brk > old_brk) {
        // Expand: allocate new pages
        uint64_t pages = (new_brk - old_brk) / VMM_PAGE_SIZE_4K;
        if (vmm_alloc_user_pages(p->cr3, old_brk, pages, VMM_USER_RW) != 0) {
            return -1;
        }
    } else if (new_brk < old_brk) {
        // Contract: free pages
        uint64_t pages = (old_brk - new_brk) / VMM_PAGE_SIZE_4K;
        vmm_free_user_pages(p->cr3, new_brk, pages);
    }

    p->brk = new_brk;
    return (int64_t)new_brk;
}

// #429: demand-paged anonymous mmap. Instead of eagerly committing every page,
// register a lazy VMA in the process's mm and let the #PF handler fault pages
// in on first touch (real demand paging + W^X on writable data pages). Falls
// back to eager allocation if the VMA machinery is unavailable.
#ifndef VMA_READ
#define VMA_READ       (1 << 0)
#define VMA_WRITE      (1 << 1)
#define VMA_EXEC       (1 << 2)
#define VMA_PRIVATE    (1 << 4)
#define VMA_ANONYMOUS  (1 << 5)
#define VMA_LAZY       (1 << 10)
#endif
// mm_create/vma_create/vma_add/vma_find/do_munmap: real prototypes now come
// from mm/demand.h (included above for mm_prefault_range(), #510/#511). This
// used to locally re-declare all five with loose `void *` signatures because
// demand.h was not included in this file; that is gone (it conflicts with
// demand.h's real mm_struct_t*/vma_t* types) rather than kept as dead code.

int64_t sys_mmap(uint64_t addr, uint64_t len, int prot, int flags) {
    process_t *p = proc_current();
    if (!p || p->privilege != PRIV_USER) return -1;
    if (len == 0) return -1;

    if (!p->mm) p->mm = mm_create();
    if (!p->mm) return -1;

    // #629/#636: ONE implementation, in mm/demand.c. This function used to do
    // placement, the overlap decision, the VMA insert, the identity punch and
    // the cursor update HERE, taking mm_lock() TWICE with a kmalloc between the
    // two halves. Three things followed from that, all Ring-3 reachable:
    //
    //   - an explicit `addr` was used exactly as Ring 3 passed it. The ELF
    //     image, the brk heap and the user stack have no VMAs in this kernel,
    //     so vma_add() could not see them, and vmm_punch_demand_range() then
    //     cleared every leaf that is PRESENT and not USER, which is precisely
    //     what the kernel's own identity mappings are made of. One syscall,
    //     mmap() at a kernel address, and the next kernel instruction fetch in
    //     that address space faults.
    //   - `mmap_next += length` had no ceiling, so after 4GB of cumulative
    //     anonymous mmap (or one oversized request) the cursor walked out of
    //     the arena into regions with no VMAs, the insert succeeded, and the
    //     returned address aliased live memory.
    //   - placement and insertion sat in different critical sections, so every
    //     failed insert permanently burned `length` of address space and an
    //     explicit-address mapping could land inside a range the cursor had
    //     already promised.
    //
    // do_mmap() does all of it in a single mm_lock() critical section, bounds
    // the cursor to the anonymous arena, and rejects an explicit address that
    // is not a user address at all.
    uint64_t r = do_mmap((mm_struct_t *)p->mm, addr, len,
                         (uint32_t)prot, (uint32_t)flags, NULL, 0);
    return (r == (uint64_t)-1) ? -1 : (int64_t)r;
}

int64_t sys_munmap(uint64_t addr, uint64_t len) {
    process_t *p = proc_current();
    if (!p || p->privilege != PRIV_USER) {
        return -1;
    }

    if (len == 0) return -1;

    // #629(b): NO vma_find() GATE. It did two wrong things at once. It called
    // vma_find() WITHOUT the mm lock, which races a CLONE_VM sibling inside
    // do_munmap() that is kfree()ing the very node this walk is standing on
    // (interior nodes really do get freed since VMA splitting became real).
    // And vma_find() returns NULL when `addr` lands in a HOLE, so a range that
    // merely BEGINS in a hole skipped the VMA path entirely: it freed PTEs with
    // vmm_free_user_pages() (which is not USER-gated), LEFT the VMAs in the
    // list, and returned 0. Three calls reproduce it single-threaded: mmap 8
    // pages, munmap the first 2 (clipping the VMA and creating a hole at the
    // base), then munmap all 8. The surviving VMA re-faults fresh zero pages
    // for memory the app was told was unmapped, and any later mmap into that
    // range fails with "overlaps existing VMA" for the life of the process.
    //
    // do_munmap() now clips or removes every overlapping VMA AND tears down
    // pages across the whole [start, end) under the lock, COW-aware and
    // USER-gated, so it is correct for holes, for regions no VMA covers, and
    // for mixed ranges. BEHAVIOUR CHANGE: munmap really unmaps everything in
    // the requested range now, which is POSIX, but a caller passing an
    // over-long length destroys more than it used to.
    if (p->mm) return do_munmap((mm_struct_t *)p->mm, addr, len);

    uint64_t pages = (len + VMM_PAGE_SIZE_4K - 1) / VMM_PAGE_SIZE_4K;
    vmm_free_user_pages(p->cr3, addr, pages);
    return 0;
}

// ============================================================================
// Console syscalls
// ============================================================================

int64_t sys_putchar(int c) {
    // Write to process's stdout fd if available (PTY slave)
    process_t *proc = proc_current();
    if (proc && proc->fds[1]) {
        uint8_t ch = (uint8_t)c;
        // #693: if the pty write fails, fall through to the serial console
        // rather than silently swallowing the character.
        if (file_write(proc->fds[1], &ch, 1) == 1)
            return c;
    }
    kputc((char)c);
    return c;
}

int64_t sys_getchar(void) {
    // Read one byte from the current process's stdin (fd 0)
    process_t *proc = proc_current();
    if (!proc || !proc->fds[0]) return -1;

    extern int64_t file_read(struct file *f, void *buf, size_t count);
    uint8_t c = 0;
    int64_t n = file_read(proc->fds[0], &c, 1);
    if (n <= 0) return -1;
    return (int64_t)c;
}

// ============================================================================
// Time syscalls
// ============================================================================

int64_t sys_time(void) {
    // #594: return seconds since boot using the REAL timer rate. This used
    // to hardcode timer_ticks/100, but the kernel's actual tick rate is
    // g_timer_hz (250Hz, see SYS_UPTIME_MS/SYS_GET_TICKS/main.c), so the
    // old code returned a "second" counter running at 100/250 = 40% of
    // real speed. gettimeofday() (userland/libc/posixextra.c) builds
    // tv_sec from this syscall but tv_usec from SYS_UPTIME_MS (which
    // already used the correct g_timer_hz divisor) - the two fields
    // silently desynced, making the derived millisecond clock (used by
    // every Sys_Milliseconds()-driven app, e.g. OpenArena's frame timer)
    // non-monotonic: it could stall for over a second then jump forward
    // in one burst, exactly the freeze-then-burst symptom MEASURED via
    // OpenArena screendumps (#594). Match the same divisor SYS_UPTIME_MS
    // uses so both fields of gettimeofday() derive from one consistent
    // real-time rate.
    extern uint32_t g_timer_hz;
    uint32_t hz = g_timer_hz ? g_timer_hz : 250;
    return (int64_t)(timer_ticks / hz);
}

int64_t sys_clock(void) {
    return timer_ticks;
}

// ============================================================================
// Window/Graphics syscalls
// ============================================================================

// Track windows per process (simple single-window model for now)
#define MAX_USER_WINDOWS 16
#define USER_EVENT_QUEUE_SIZE 128

// Per-window state for user processes
typedef struct {
    window_t *window;
    gui_event_t events[USER_EVENT_QUEUE_SIZE];
    uint32_t event_head;
    uint32_t event_tail;
    uint32_t event_count;
    uint32_t *content_buffer;  // Per-window pixel buffer (persistent across frames)
    int content_width;
    int content_height;
    int alloc_width;           // Allocated buffer width
    int alloc_height;          // Allocated buffer height
    uint32_t owner_pid;        // PID of the process that created this window
    // #453: win_get_event() used to busy-wait with proc_yield(), pegging a core
    // for every idle/docked window. Sleepers now block on this wait queue and
    // are woken when an event is queued. redraw_pending gates the per-composite
    // REDRAW so an idle window isn't fed a continuous REDRAW stream.
    wait_queue_head_t event_wq;
    int redraw_pending;
} user_window_t;

static user_window_t user_windows[MAX_USER_WINDOWS];

// Forward declaration for event handler
static void user_window_event_handler(void *app_data, gui_event_t *event);
static void user_window_draw_handler(void *app_data);

// Queue an event for a user-space window
static void user_window_queue_event(int handle, gui_event_t *event) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) return;

    user_window_t *uw = &user_windows[handle];
    if (uw->event_count >= USER_EVENT_QUEUE_SIZE) {
        // Queue full, drop oldest event
        uw->event_head = (uw->event_head + 1) % USER_EVENT_QUEUE_SIZE;
        uw->event_count--;
    }

    uw->events[uw->event_tail] = *event;
    uw->event_tail = (uw->event_tail + 1) % USER_EVENT_QUEUE_SIZE;
    uw->event_count++;
    wake_up(&uw->event_wq);   // #453: wake a win_get_event() sleeper (IRQ-safe)

    // #745 ELEVATION, requirement 8: an app may only raise an elevation prompt
    // in RESPONSE to input the window manager actually delivered to it. This is
    // the single chokepoint where that delivery happens, so it is the only
    // place the stamp is written, and sys_elev_request() is the only reader.
    //
    // KEY DOWN and BUTTON DOWN/UP only. Pointer MOTION and REDRAW are excluded
    // deliberately: a cursor resting over a window, or a window simply being
    // repainted, would otherwise keep it permanently "recently active" with no
    // user intent behind it, and the property being defended is user intent.
    if (event->type == EVENT_KEY_DOWN ||
        event->type == EVENT_MOUSE_DOWN ||
        event->type == EVENT_MOUSE_UP) {
        extern uint64_t sched_now_ms(void);
        process_t *op = proc_get(uw->owner_pid);
        if (op) op->elev_last_input_ms = sched_now_ms();
    }
}

// Event handler for user-space windows - routes events to per-window queue
static void user_window_event_handler(void *app_data, gui_event_t *event) {
    int handle = (int)(uintptr_t)app_data;
    if (handle >= 0 && handle < MAX_USER_WINDOWS) {
        // Mouse events arrive from the compositor in ABSOLUTE screen
        // coordinates (sys_inject_mouse -> wm_inject_app_mouse passes them
        // through untranslated). Userland apps hit-test in their own content
        // space (origin 0,0 at the top-left of their drawable area), so we must
        // translate screen -> content-relative here using the same content
        // origin the draw handler blits to. Keyboard events carry no coords.
        if (event->type == EVENT_MOUSE_DOWN || event->type == EVENT_MOUSE_UP ||
            event->type == EVENT_MOUSE_MOVE || event->type == EVENT_MOUSE_SCROLL) {
            user_window_t *uw = &user_windows[handle];
            if (uw->window) {
                int32_t wx, wy, ww, wh;
                window_get_content_bounds(uw->window, &wx, &wy, &ww, &wh);
                event->mouse_x -= wx;
                event->mouse_y -= wy;
            }
        }
        user_window_queue_event(handle, event);
    }
}

// Draw handler for user-space windows - blits content buffer (frame already drawn by wm_draw_all)
static void user_window_draw_handler(void *app_data) {
    if (g_win_blit_suppressed) return;   // screensaver owns the FB; don't punch through
    int handle = (int)(uintptr_t)app_data;
    if (handle >= 0 && handle < MAX_USER_WINDOWS && user_windows[handle].window) {
        user_window_t *uw = &user_windows[handle];
        window_t *win = uw->window;

        // NOTE: Do NOT call window_draw(win) here! It clears the content area.
        // Window frame is already drawn by wm_draw_all() before this is called.

        // Blit content buffer to window's content area
        if (uw->content_buffer) {
            int32_t wx, wy, ww, wh;
            window_get_content_bounds(win, &wx, &wy, &ww, &wh);

            extern void fb_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint32_t *data);
            uint8_t op = win->opacity ? win->opacity : 255;
            // CLIP the content to the window's content rectangle so an app can
            // never draw outside its window box (right/bottom overflow).
            int bw = uw->content_width  < ww ? uw->content_width  : ww;
            int bh = uw->content_height < wh ? uw->content_height : wh;
            if (op >= 255) {
                if (uw->content_width == bw) {
                    fb_blit(wx, wy, bw, bh, uw->content_buffer);   // stride matches
                } else {
                    // buffer wider than the content area: blit row by row, clipped
                    for (int ry = 0; ry < bh; ry++)
                        fb_blit(wx, wy + ry, bw, 1, &uw->content_buffer[ry * uw->content_width]);
                }
            } else {
                // Per-window transparency: alpha-blend content over what's behind it.
                extern uint32_t fb_get_pixel(uint32_t x, uint32_t y);
                extern void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
                uint32_t inv = 255u - op;
                for (int ry = 0; ry < bh; ry++) {
                    for (int rx = 0; rx < bw; rx++) {
                        uint32_t sp = uw->content_buffer[ry * uw->content_width + rx];
                        uint32_t dp = fb_get_pixel((uint32_t)(wx + rx), (uint32_t)(wy + ry));
                        uint32_t r = (((sp >> 16) & 0xFF) * op + ((dp >> 16) & 0xFF) * inv) / 255;
                        uint32_t g = (((sp >> 8)  & 0xFF) * op + ((dp >> 8)  & 0xFF) * inv) / 255;
                        uint32_t b = (( sp        & 0xFF) * op + ( dp        & 0xFF) * inv) / 255;
                        fb_put_pixel((uint32_t)(wx + rx), (uint32_t)(wy + ry), (r << 16) | (g << 8) | b);
                    }
                }
            }

            // Log blit operation. Gated behind g_wm_blit_debug (default OFF):
            // an always-on per-frame [WM] blitted log floods the serial console
            // at ~30Hz (e.g. the aichat dock sliver) and starves the RC console.
            // Toggle on only when diagnosing blits.
            extern volatile int g_wm_blit_debug;
            static int blit_count = 0;
            if (g_wm_blit_debug && (++blit_count % 10 == 0)) {  // Log every 10th blit to avoid spam
                char log_msg[128];
                snprintf(log_msg, sizeof(log_msg), "[WM] user_window_draw_handler: handle=%d, blitted %dx%d buffer to (%d,%d) (#%d)",
                        handle, uw->content_width, uw->content_height, wx, wy, blit_count);
                syslog_log(1, log_msg);  // LOG_INFO
            }
        }

        // Send a redraw event to user-space ONLY when the app actually needs to
        // repaint: initial paint, resize, or an explicit win_invalidate() (all
        // of which set redraw_pending). The routine per-composite pass must NOT
        // re-queue a REDRAW, or the app's own blit -> recomposite -> REDRAW forms
        // a feedback loop that never lets win_get_event() sleep (#453). Still
        // coalesce so at most one REDRAW is queued at a time.
        if (uw->redraw_pending) {
            uw->redraw_pending = 0;
            int has_redraw = 0;
            uint32_t idx = uw->event_head;
            for (uint32_t i = 0; i < uw->event_count; i++) {
                if (uw->events[idx].type == EVENT_REDRAW) {
                    has_redraw = 1;
                    break;
                }
                idx = (idx + 1) % USER_EVENT_QUEUE_SIZE;
            }
            if (!has_redraw) {
                gui_event_t redraw_event;
                redraw_event.type = EVENT_REDRAW;
                redraw_event.target_id = handle;
                redraw_event.mouse_x = 0;
                redraw_event.mouse_y = 0;
                redraw_event.mouse_buttons = 0;
                redraw_event.scroll_delta = 0;
                redraw_event.keycode = 0;
                redraw_event.key_char = 0;
                user_window_queue_event(handle, &redraw_event);
            }
        }
    }
}

// Clean up all user windows owned by a process that is exiting.
// Called from proc_exit() in process.c.
void cleanup_user_windows_for_process(uint32_t pid) {
    // Destroy all windows owned by the exiting process.
    // This runs at proc_exit() time (interrupts may be disabled, so keep it simple).
    for (int i = 0; i < MAX_USER_WINDOWS; i++) {
        if (user_windows[i].window && user_windows[i].owner_pid == pid) {
            // Free content buffer
            if (user_windows[i].content_buffer) {
                kfree(user_windows[i].content_buffer);
                user_windows[i].content_buffer = NULL;
            }
            // Unregister from window manager and destroy the window object
            window_t *win = user_windows[i].window;
            user_windows[i].window = NULL;
            user_windows[i].owner_pid = 0;
            user_windows[i].event_count = 0;
            // Find and unregister the app registration
            app_registration_t *reg = wm_get_app_by_window(win);
            if (reg) {
                // Compute array index via pointer difference from base
                extern app_registration_t *wm_get_app_by_id(int app_id);
                app_registration_t *base = wm_get_app_by_id(0);
                if (base) {
                    int idx = (int)(reg - base);
                    wm_unregister_app(idx);
                }
            }
            window_destroy(win);
        }
    }
    wm_invalidate_all();
}

// Called from window_resize() in window.c to reallocate the content buffer
// and notify the user-mode app via EVENT_RESIZE.
void user_window_handle_resize(window_t *win) {
    if (!win) return;

    // Find the user_window_t that owns this window
    for (int i = 0; i < MAX_USER_WINDOWS; i++) {
        if (user_windows[i].window == win) {
            user_window_t *uw = &user_windows[i];
            int32_t cx, cy, cw, ch;
            window_get_content_bounds(win, &cx, &cy, &cw, &ch);
            if (cw <= 0 || ch <= 0) return;

            // Skip if dimensions haven't actually changed
            if (cw == uw->content_width && ch == uw->content_height) return;

            // Reallocate content buffer at the new size
            extern void *kmalloc(size_t size);
            extern void kfree(void *ptr);
            uint32_t *new_buf = kmalloc(cw * ch * sizeof(uint32_t));
            if (!new_buf) return;

            // Fill with window background color
            for (int p = 0; p < cw * ch; p++) {
                new_buf[p] = 0xFFF5F5F5;
            }

            // Copy old content (top-left aligned, clipped to new bounds)
            if (uw->content_buffer) {
                int copy_w = (cw < uw->content_width) ? cw : uw->content_width;
                int copy_h = (ch < uw->content_height) ? ch : uw->content_height;
                for (int y = 0; y < copy_h; y++) {
                    memcpy(&new_buf[y * cw],
                           &uw->content_buffer[y * uw->content_width],
                           copy_w * sizeof(uint32_t));
                }
                kfree(uw->content_buffer);
            }

            uw->content_buffer = new_buf;
            uw->content_width = cw;
            uw->content_height = ch;
            uw->alloc_width = cw;
            uw->alloc_height = ch;

            // (#200 resize-hang) If this is the Win16 host window, the Win16
            // interpreter (exec/win16api.c) still holds the OLD (now freed)
            // content buffer in g_win16_canvas with the OLD stride. Hand it the
            // new buffer + size so it does not write into freed memory (which
            // corrupted the heap and hung the OS on every Win16 window resize).
            extern void win16_host_rebind_canvas(int slot, uint32_t *new_buf,
                                                 int new_w, int new_h);
            win16_host_rebind_canvas(i, new_buf, cw, ch);

            // Queue EVENT_RESIZE so the app can redraw at the new size
            uw->redraw_pending = 1;   // #453: force a REDRAW after the resize
            gui_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = EVENT_RESIZE;
            ev.mouse_x = cw;   // new width in mouse_x field
            ev.mouse_y = ch;   // new height in mouse_y field
            user_window_queue_event(i, &ev);

            char log_msg[128];
            snprintf(log_msg, sizeof(log_msg),
                     "[WM] Resize window %d: new content %dx%d", i, cw, ch);
            syslog_log(1, log_msg);
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Win16 host window (#144/#145). The Win16 interpreter runs in a kernel thread
// (see SYS_WIN16_RUN) and paints into a normal user-window content buffer so the
// compositor composites it like any app (live desktop + taskbar button) instead
// of taking over the framebuffer. Called from exec/win16api.c.
// ---------------------------------------------------------------------------
// (#215) Titlebar CLOSE (X) handler for the Win16 host window. The kernel WM
// invokes win->on_close when the X is clicked. We just latch the close request
// into the Win16 subsystem; the Win16 message pump (running in the win16 proc)
// converts it to a WM_QUIT and tears the app down cleanly (frees canvas, host
// window, DLL module images via registry_reset, and clears g_win16_busy so a
// DIFFERENT Win16 app can then launch). We must NOT free the window or run the
// interpreter from here: this runs in the desktop/WM context, and the host
// window is destroyed by win16_host_destroy() during that teardown.
static void win16_host_close_handler(window_t *win, gui_event_t *event) {
    (void)win; (void)event;
    extern void win16_request_close(void);
    win16_request_close();
}

// (#745) Titlebar CLOSE (X) handler for a DOS host window.
//
// WHY A SECOND HANDLER EXISTS. win16_host_create() is shared by the Win16
// interpreter AND the DOS layer, but it installed the WIN16 handler on every
// window it made, because Win16 was once its only client. So the X on a DOS
// guest's window latched g_win16_close_requested, which only the Win16 message
// pump ever reads. The DOS run loop never sees it, so the guest kept
// interpreting at full speed (measured on VM 2933, golden 1848: the click
// landed on the button, the window stayed open, and the scheduler still
// reported top=dos:85,COMPOSIT:13 for the next 12 seconds) until it halted
// itself or hit the 6-hour DOS_MAX_RUN_MS cap, with g_dos_busy still set so no
// second DOS program could be launched. The comment on the Win16 install two
// screens below describes exactly this failure mode, for Win16, while it was
// live for DOS the whole time.
//
// Second defect closed here: the latch is a single global, so with a Win16 app
// ALSO up, closing the DOS window would have quit the WIN16 app instead.
static void dos_host_close_handler(window_t *win, gui_event_t *event) {
    (void)win; (void)event;
    extern void dos_request_close(void);
    dos_request_close();
}

// Re-route an already-created host window's X to the DOS teardown path. A
// slot-indexed setter rather than a create variant, so the DOS layer needs no
// window_t of its own and win16_host_create() keeps ONE definition.
void win16_host_route_close_to_dos(int slot) {
    if (slot < 0 || slot >= MAX_USER_WINDOWS) return;
    if (!user_windows[slot].window) return;
    window_set_close_handler(user_windows[slot].window, dos_host_close_handler);
}

int win16_host_create(const char *title, int x, int y, int w, int h,
                      uint32_t **out_buf, int *out_w, int *out_h,
                      window_t **out_win) {
    int slot = -1;
    for (int i = 0; i < MAX_USER_WINDOWS; i++)
        if (!user_windows[i].window) { slot = i; break; }
    if (slot < 0) return -1;

    window_t *win = window_create(title, x, y, w, h);
    if (!win) return -1;

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(win, &wx, &wy, &ww, &wh);
    if (ww <= 0) ww = 1;
    if (wh <= 0) wh = 1;

    extern void *kmalloc(size_t size);
    uint32_t *cb = kmalloc((size_t)ww * (size_t)wh * sizeof(uint32_t));
    if (!cb) { window_destroy(win); return -1; }
    for (int i = 0; i < ww * wh; i++) cb[i] = 0xFFC0C0C0;   // Win16 light gray

    user_windows[slot].window         = win;
    user_windows[slot].event_head     = 0;
    user_windows[slot].event_tail     = 0;
    user_windows[slot].event_count    = 0;
    user_windows[slot].content_buffer = cb;
    user_windows[slot].content_width  = ww;
    user_windows[slot].content_height = wh;
    user_windows[slot].alloc_width    = ww;
    user_windows[slot].alloc_height   = wh;
    user_windows[slot].owner_pid      = 0;   // kernel-owned (Win16 subsystem)
    wait_queue_head_init(&user_windows[slot].event_wq);   // #453
    user_windows[slot].redraw_pending = 1;                // #453: paint once on create

    int app_id = wm_register_app(win, (void *)(uintptr_t)slot,
                                 user_window_event_handler,
                                 user_window_draw_handler, NULL);
    if (app_id < 0) {
        extern void kfree(void *ptr);
        kfree(cb);
        window_destroy(win);
        user_windows[slot].window = NULL;
        return -1;
    }
    // (#215) Route the titlebar CLOSE (X) button to the Win16 teardown path.
    // Without this, on_close is NULL and the kernel WM just HIDES the window
    // (default behaviour), leaving the interpreter running and the single Win16
    // slot busy forever -> a second launch returns "busy".
    window_set_close_handler(win, win16_host_close_handler);

    window_show(win);
    wm_focus_window(win);
    wm_invalidate_all();

    if (out_buf) *out_buf = cb;
    if (out_w)   *out_w   = ww;
    if (out_h)   *out_h   = wh;
    if (out_win) *out_win = win;
    return slot;
}

// Report the on-screen content rect (client area) of a Win16 host window so the
// Win16 subsystem can translate the global kernel cursor (mouse_x/mouse_y, which
// are screen coords) into the window's canvas/client coords for mouse forwarding.
// Returns 0 on success.
int win16_host_content_rect(int slot, int *ox, int *oy, int *ow, int *oh) {
    if (slot < 0 || slot >= MAX_USER_WINDOWS) return -1;
    window_t *win = user_windows[slot].window;
    if (!win) return -1;
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(win, &wx, &wy, &ww, &wh);
    if (ox) *ox = wx;
    if (oy) *oy = wy;
    if (ow) *ow = ww;
    if (oh) *oh = wh;
    return 0;
}

void win16_host_destroy(int slot) {
    if (slot < 0 || slot >= MAX_USER_WINDOWS) return;
    if (!user_windows[slot].window) return;
    user_window_t *uw = &user_windows[slot];

    app_registration_t *reg = wm_get_app_by_window(uw->window);
    if (reg) {
        extern app_registration_t *wm_get_app_by_id(int app_id);
        wm_unregister_app((int)(reg - wm_get_app_by_id(0)));
    }
    extern void kfree(void *ptr);
    if (uw->content_buffer) { kfree(uw->content_buffer); uw->content_buffer = NULL; }
    window_destroy(uw->window);
    uw->window = NULL;
    uw->event_count = 0;
    wm_invalidate_all();
}

// #745: opt this window in to the compositor-drawn drop shadow. Pure flag set:
// no buffer, no geometry, no kernel drawing. The shadow lives in pixels OUTSIDE
// the window rectangle, which the kernel WM never owns; the userland compositor
// paints that region (wallpaper/icons/widgets) immediately before it calls
// SYS_COMPOSITOR_RENDER_WINDOWS, so it is the only layer that can darken them.
// wm_invalidate_all() so the very first composite after the call includes the
// band, rather than waiting for the next unrelated redraw.
int64_t sys_win_set_shadow(int handle) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window)
        return -1;
    user_windows[handle].window->flags |= WINDOW_FLAG_SHADOW;
    wm_invalidate_all();
    return 0;
}

// #185: mark an existing user window as borderless (no chrome). Sets the flag
// then reallocates the content buffer to the new (now full-window) content size
// so the app owns the entire window rectangle.
int64_t sys_win_set_nochrome(int handle) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window)
        return -1;
    user_window_t *uw = &user_windows[handle];
    window_t *win = uw->window;

    win->flags |= WINDOW_FLAG_NOCHROME;
    // borderless panels are fixed (no titlebar to drag, no grips to resize)
    win->flags &= ~(WINDOW_FLAG_RESIZABLE | WINDOW_FLAG_MOVABLE);

    int32_t wx, wy, ww, wh;
    window_get_content_bounds(win, &wx, &wy, &ww, &wh);
    if (ww <= 0 || wh <= 0) return -1;

    extern void *kmalloc(size_t size);
    extern void kfree(void *p);
    uint32_t *nb = kmalloc((size_t)ww * wh * sizeof(uint32_t));
    if (!nb) return -1;
    for (int i = 0; i < ww * wh; i++) nb[i] = 0xFFF5F5F5;
    if (uw->content_buffer) kfree(uw->content_buffer);
    uw->content_buffer = nb;
    uw->content_width  = ww;
    uw->content_height = wh;
    uw->alloc_width    = ww;
    uw->alloc_height   = wh;

    // tell the app its new drawable size so it relays out
    gui_event_t ev; memset(&ev, 0, sizeof(ev));
    ev.type = EVENT_RESIZE; ev.mouse_x = ww; ev.mouse_y = wh;
    user_window_queue_event(handle, &ev);

    // borderless panels still need keyboard focus to accept typing
    { extern void wm_focus_window(window_t *w); wm_focus_window(win); }

    wm_invalidate_all();
    return 0;
}

// Focus a user window addressed by the SLOT handle win_create() returns, but
// only when the CALLING process owns it. Returns 0 on success, -1 otherwise so
// the caller can fall back to window->id matching. This lets a fullscreen game
// (Arena/Squadron) re-assert keyboard focus on its own window every frame
// without the slot handle being misread as a window id (the two number spaces
// overlap for low values, which sent WASD to the wrong window). (#arena-wasd)
int64_t wm_focus_user_slot(int slot) {
    if (slot < 0 || slot >= MAX_USER_WINDOWS || !user_windows[slot].window) return -1;
    process_t *cur = proc_current();
    if (!cur || user_windows[slot].owner_pid != (uint32_t)cur->pid) return -1;
    extern void wm_focus_window(window_t *win);
    wm_focus_window(user_windows[slot].window);
    return 0;
}

int64_t sys_win_create(const char *utitle, int x, int y, int width, int height) {
    // #567: bounce the user title fault-safe; window_create() and the log
    // snprintf below then read a kernel pointer. NULL title stays NULL.
    char ktitle[128];
    const char *title = 0;
    if (utitle) {
        if (strncpy_from_user(ktitle, utitle, sizeof(ktitle)) < 0) return -1;
        title = ktitle;
    }
    // Find free window slot
    int slot = -1;
    for (int i = 0; i < MAX_USER_WINDOWS; i++) {
        if (!user_windows[i].window) {
            slot = i;
            break;
        }
    }
    if (slot < 0) return -1;

    // Create the window
    window_t *win = window_create(title, x, y, width, height);
    if (!win) return -1;

    // Get content area dimensions
    int32_t wx, wy, ww, wh;
    window_get_content_bounds(win, &wx, &wy, &ww, &wh);

    // Allocate content buffer for this window
    extern void *kmalloc(size_t size);
    uint32_t *content_buffer = kmalloc(ww * wh * sizeof(uint32_t));
    if (!content_buffer) {
        window_destroy(win);
        return -1;
    }

    // Clear buffer to light gray background (0xFFF5F5F5)
    for (int i = 0; i < ww * wh; i++) {
        content_buffer[i] = 0xFFF5F5F5;
    }

    // Initialize user window state
    user_windows[slot].window = win;
    user_windows[slot].event_head = 0;
    user_windows[slot].event_tail = 0;
    user_windows[slot].event_count = 0;
    user_windows[slot].content_buffer = content_buffer;
    user_windows[slot].content_width = ww;
    user_windows[slot].content_height = wh;
    user_windows[slot].alloc_width = ww;
    user_windows[slot].alloc_height = wh;
    user_windows[slot].owner_pid = proc_current() ? proc_current()->pid : 0;
    wait_queue_head_init(&user_windows[slot].event_wq);   // #453
    user_windows[slot].redraw_pending = 1;                // #453: paint once on create

    char log_msg[128];
    snprintf(log_msg, sizeof(log_msg), "[SYSCALL] Allocated content buffer for window %d: %dx%d (%zu bytes)",
            slot, ww, wh, (size_t)(ww * wh * sizeof(uint32_t)));
    syslog_log(1, log_msg);  // LOG_INFO

    // Register with window manager for event routing
    int app_id = wm_register_app(win, (void*)(uintptr_t)slot,
                                  user_window_event_handler,
                                  user_window_draw_handler,
                                  NULL);
    if (app_id < 0) {
        window_destroy(win);
        user_windows[slot].window = NULL;
        return -1;
    }

    window_show(win);
    wm_focus_window(win);  // New window gets keyboard focus immediately
    wm_invalidate_all();

    char log_msg2[128];
    snprintf(log_msg2, sizeof(log_msg2), "[SYSCALL] Created user window %d: \"%s\" at (%d,%d) %dx%d",
            slot, title, x, y, width, height);
    syslog_log(1, log_msg2);  // LOG_INFO
    return slot;
}

int64_t sys_win_destroy(int handle) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];

    // Unregister from window manager
    app_registration_t *reg = wm_get_app_by_window(uw->window);
    if (reg) {
        wm_unregister_app((int)(reg - wm_get_app_by_id(0)));  // Get index
    }

    // Free content buffer
    if (uw->content_buffer) {
        extern void kfree(void *ptr);
        kfree(uw->content_buffer);
        uw->content_buffer = NULL;

        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "[SYSCALL] Freed content buffer for window %d", handle);
        syslog_log(1, log_msg);  // LOG_INFO
    }

    window_destroy(uw->window);
    uw->window = NULL;
    uw->event_count = 0;
    wm_invalidate_all();
    return 0;
}

int64_t sys_win_draw_rect(int handle, int x, int y, int w, int h, uint32_t color) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer) {
        return -1;
    }

    // Clip to buffer bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > uw->content_width) w = uw->content_width - x;
    if (y + h > uw->content_height) h = uw->content_height - y;
    if (w <= 0 || h <= 0) return 0;

    // Draw to content buffer
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            int py = y + dy;
            if (px >= 0 && px < uw->content_width && py >= 0 && py < uw->content_height) {
                uw->content_buffer[py * uw->content_width + px] = color;
            }
        }
    }

    return 0;
}

int64_t sys_win_draw_pixel(int handle, int x, int y, uint32_t color) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer) {
        return -1;
    }

    // Clip to buffer bounds
    if (x < 0 || y < 0 || x >= uw->content_width || y >= uw->content_height) {
        return 0;
    }

    // Draw to content buffer
    uw->content_buffer[y * uw->content_width + x] = color;
    return 0;
}

int64_t sys_win_draw_text(int handle, int x, int y, const char *utext, uint32_t color) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !utext) {
        return -1;
    }

    // #567: bounce the user string fault-safe into a kernel buffer before the
    // per-glyph loop reads it byte-by-byte.
    char *text = sc_dup_user_str(utext, 4096);
    if (!text) return -1;

    // Render each character into the content_buffer using the full 8x16 bitmap font.
    // font_get_glyph returns a 16-byte array where each byte is one row, MSB = leftmost pixel.
    int cx = x;
    for (const char *p = text; *p; p++) {
        const uint8_t *glyph = font_get_glyph(*p);
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                if (bits & (0x80 >> col)) {
                    int px = cx + col;
                    int py = y + row;
                    if (px >= 0 && px < uw->content_width &&
                        py >= 0 && py < uw->content_height) {
                        uw->content_buffer[py * uw->content_width + px] = color;
                    }
                }
            }
        }
        cx += 8;  // Advance by one character width (8 pixels)
    }

    kfree(text);
    return 0;
}

// Antialiased TrueType text into a window's content buffer (window-relative,
// clipped to the content rect). y is the top of the text line. Used by apps
// that opt into TTF (e.g. Settings) via win_draw_text_ttf().
int64_t sys_win_draw_text_ttf(int handle, int x, int y, const char *utext, uint32_t color, int size) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) return -1;
    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !utext) return -1;
    char *text = sc_dup_user_str(utext, 8192);   // #567: fault-safe bounce
    if (!text) return -1;
    if (size < 6) size = 6;
    if (size > 64) size = 64;

    extern ttf_glyph_t *ttf_get_glyph(int codepoint, int size, int style);
    extern void ttf_get_metrics(int size, int *ascent, int *descent, int *line_gap);
    extern int ttf_get_advance(int codepoint, int size);
    extern int ttf_get_kerning(int cp1, int cp2, int size);
    extern int ttf_cursor_step(const ttf_glyph_t *g, int cp, int next_cp, int size);   // #589

    int ascent = size, descent = 0, line_gap = 0;
    ttf_get_metrics(size, &ascent, &descent, &line_gap);
    int baseline = y + ascent;
    int cw = uw->content_width, ch = uw->content_height;
    uint8_t cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;

    int cx = x;
    for (const char *p = text; *p; p++) {
        int c = (unsigned char)*p;
        ttf_glyph_t *g = ttf_get_glyph(c, size, 0);
        if (g && g->bitmap) {   // #589: null-glyph handled by the shared step below
            int gx = cx + g->xoff, gy = baseline + g->yoff;
            for (int row = 0; row < g->height; row++) {
                int py = gy + row; if (py < 0 || py >= ch) continue;
                for (int col = 0; col < g->width; col++) {
                    int px = gx + col; if (px < 0 || px >= cw) continue;
                    uint8_t a = g->bitmap[row * g->width + col]; if (!a) continue;
                    uint32_t *d = &uw->content_buffer[py * cw + px];
                    if (a >= 250) { *d = color; }
                    else {
                        uint32_t bg = *d; uint8_t br = (bg>>16)&0xFF, bgc = (bg>>8)&0xFF, bb = bg&0xFF;
                        uint8_t inv = 255 - a;
                        *d = (((cr*a+br*inv)/255)<<16) | (((cg*a+bgc*inv)/255)<<8) | ((cb*a+bb*inv)/255);
                    }
                }
            }
        }
        // #589: advance via the shared cursor-step helper. #575's kerning is now
        // STRUCTURAL: this same function backs the measure path and every draw
        // path, so ttf_measure() == win_draw_text_ttf() cannot drift again.
        cx += ttf_cursor_step(g, c, p[1] ? (unsigned char)p[1] : 0, size);
    }
    kfree(text);
    return 0;
}

// Face-aware antialiased TTF into a window (Font Browser previews, Studio text
// preview). Same as sys_win_draw_text_ttf but with an explicit face + style.
int64_t sys_win_draw_text_ttf_ex(int handle, int x, int y, const char *utext, uint32_t color, int size, int face, int style) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) return -1;
    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !utext) return -1;
    char *text = sc_dup_user_str(utext, 8192);   // #567: fault-safe bounce
    if (!text) return -1;
    if (size < 6) size = 6;
    if (size > 96) size = 96;

    extern ttf_glyph_t *ttf_get_glyph_f(int, int, int, int);
    extern void ttf_get_metrics_f(int, int, int *, int *, int *);
    extern int ttf_get_advance_f(int, int, int);
    extern int ttf_get_kerning_f(int, int, int, int);
    extern int ttf_cursor_step_f(int face, const ttf_glyph_t *g, int cp, int next_cp, int size);   // #589

    int ascent = size, descent = 0, line_gap = 0;
    ttf_get_metrics_f(face, size, &ascent, &descent, &line_gap);
    int baseline = y + ascent;
    int cw = uw->content_width, ch = uw->content_height;
    uint8_t cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;

    int cx = x;
    for (const char *p = text; *p; p++) {
        int c = (unsigned char)*p;
        if (c == '\n') { cx = x; baseline += ascent - descent + line_gap; continue; }
        ttf_glyph_t *g = ttf_get_glyph_f(face, c, size, style);
        if (g && g->bitmap) {   // #589: null-glyph handled by the shared step below
            int gx = cx + g->xoff, gy = baseline + g->yoff;
            for (int row = 0; row < g->height; row++) {
                int py = gy + row; if (py < 0 || py >= ch) continue;
                for (int col = 0; col < g->width; col++) {
                    int px = gx + col; if (px < 0 || px >= cw) continue;
                    uint8_t a = g->bitmap[row * g->width + col]; if (!a) continue;
                    uint32_t *d = &uw->content_buffer[py * cw + px];
                    if (a >= 250) { *d = color; }
                    else {
                        uint32_t bg = *d; uint8_t br = (bg>>16)&0xFF, bgc = (bg>>8)&0xFF, bb = bg&0xFF;
                        uint8_t inv = 255 - a;
                        *d = (((cr*a+br*inv)/255)<<16) | (((cg*a+bgc*inv)/255)<<8) | ((cb*a+bb*inv)/255);
                    }
                }
            }
        }
        // #589: shared cursor-step (advance + kerning), face-explicit variant.
        cx += ttf_cursor_step_f(face, g, c, p[1] ? (unsigned char)p[1] : 0, size);
    }
    kfree(text);
    return 0;
}

int64_t sys_win_draw_text_small(int handle, int x, int y, const char *utext, uint32_t color) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) return -1;
    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !utext) return -1;
    char *text = sc_dup_user_str(utext, 4096);   // #567: fault-safe bounce
    if (!text) return -1;
    // ~6x12 "medium" font: nearest-neighbor downscale of the 8x16 glyph, sized
    // between the 4x8 tooltip font and the 8x16 body font (for hints/captions).
    int cx = x;
    for (const char *p = text; *p; p++) {
        const uint8_t *glyph = font_get_glyph(*p);
        for (int row = 0; row < 12; row++) {
            uint8_t bits = glyph[row * 16 / 12];
            for (int col = 0; col < 6; col++) {
                if (bits & (0x80 >> (col * 8 / 6))) {
                    int px = cx + col, py = y + row;
                    if (px >= 0 && px < uw->content_width && py >= 0 && py < uw->content_height)
                        uw->content_buffer[py * uw->content_width + px] = color;
                }
            }
        }
        cx += 7;  // 6px glyph + 1px spacing
    }
    kfree(text);
    return 0;
}

int64_t sys_win_get_event(int handle, void *event_buf, int timeout) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];

    // #453: this used to busy-wait with proc_yield() until an event arrived or
    // the timeout expired. That spun a whole CPU core for every idle/docked
    // window (aichat sat at ~99%), starved PRIO_LOW threads, and violated the
    // "no hand-rolled poll loops" rule (#426). Now we park on the window's wait
    // queue and genuinely SLEEP: a queued event fires wake_up(&event_wq) to wake
    // us early, and for a finite timeout we also arm a wake_time deadline so the
    // timer tick (wake_sleeping_procs) can wake us. No busy-wait, no core pegged.
    //
    // The lost-wakeup close (park under cli so no wake_up()/timer IRQ can slip
    // between the recheck and the state transition) now lives inside
    // __wait_event_wait_deadline() rather than being open-coded here. It also
    // takes the wait-queue lock, so unlike this copy it does not depend on the
    // BSP-only assumption that cli alone serializes every possible waker.
    if (uw->event_count == 0) {
        if (timeout == 0) {
            return 0;  // No event, non-blocking
        }

        // #426: this loop used to be an open-coded copy of "sleep until cond or
        // deadline". That mechanism now lives in exactly one place,
        // sync/waitq.h, so this is a call into the shared primitive instead of a
        // second implementation of it. Semantics are unchanged: the deadline is
        // still computed once with the same ms->ticks rounding, the condition is
        // still re-checked at the loop top, signals are still ignored (we
        // re-sleep rather than returning early), and a timeout still surfaces to
        // userland as the pre-existing "no event" return of 0.
        uint64_t deadline = (timeout < 0)
            ? WAIT_DEADLINE_NEVER
            : wq_deadline_in(wq_ms_to_ticks((uint64_t)timeout));

        (void)wait_event_deadline(&uw->event_wq, uw->event_count > 0, deadline);

        if (uw->event_count == 0) {
            return 0;  // Timed out with no event
        }
    }

    // Pop event from queue. #567: copy the popped event out to userland via a
    // single fault-safe copy_to_user (was a raw *(gui_event_t *)event_buf write).
    gui_event_t kout = uw->events[uw->event_head];
    uw->event_head = (uw->event_head + 1) % USER_EVENT_QUEUE_SIZE;
    uw->event_count--;

    if (copy_to_user(event_buf, &kout, sizeof(kout)) != 0) return -14;
    return kout.type;  // Return event type (non-zero = event received)
}

// Present a KERNEL-OWNED host window (the DOS layer; the Win16 subsystem uses
// the same slots). The caller has just filled the window's content_buffer and
// wants it on screen.
//
// Why this exists rather than reusing sys_win_invalidate()'s body: a userland
// app calls win_invalidate() from ITS OWN process, and that path goes through
// window_invalidate(), which is not a mark at all, it is a synchronous
// window_draw(). The DOS layer presents from the `dos` proc, and drawing the
// window frame (titlebar text, so the shared TTF glyph cache) from there races
// the compositor: the first build that did it took a General Protection Fault
// in ttf_get_glyph_f(). So this MARKS THE REGION DIRTY AND NOTHING ELSE, and
// lets the render gate (wm_is_dirty(), read by the kernel desktop loop and by
// the userland compositor via SYS_WM_APPS_DIRTY) pick the frame up on the
// thread that owns drawing.
//
// Why it was needed AT ALL: the DOS layer painted into the same content_buffer
// a userland app uses and then called NOTHING, so nothing ever marked the WM
// dirty on its behalf. The render gate is level-triggered on that flag, so a
// DOS game's frames only reached the screen when something ELSE dirtied the
// region, and in practice the only thing that did was an input event. Measured
// on build 1732: with BATS running its game loop at 18.5 M insn/s and issuing
// ~1.5 M EGA plane writes a second, the WHOLE 1280x800 framebuffer was
// byte-identical over 3 seconds, one keystroke changed 27,456 pixels (all
// inside the DOS window), and then it froze again. The game was never stuck.
void win16_host_invalidate(int slot) {
    if (slot < 0 || slot >= MAX_USER_WINDOWS || !user_windows[slot].window) return;
    // NOT window_invalidate() (draws), NOT redraw_pending (that is the "app,
    // please repaint" direction and re-arming it is the #564 ping-pong).
    wm_invalidate_rect_async(&user_windows[slot].window->bounds);
}

int64_t sys_win_invalidate(int handle) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    // #564: do NOT set redraw_pending here (removed the #453 line that did).
    // redraw_pending exists to tell the APP "please repaint" - the KERNEL-
    // initiated direction, armed only by window create/resize (win_create()/
    // sys_win_resize(), unaffected by this change). An app calling
    // win_invalidate() is the OPPOSITE direction: it already repainted and is
    // presenting the result. Re-arming redraw_pending here queued it ANOTHER
    // EVENT_REDRAW - and since every app's EVENT_REDRAW handler ends by
    // calling win_invalidate() again to present THAT repaint, this was an
    // unconditional, permanent ping-pong for every window that ever calls
    // win_invalidate() more than once (i.e. every real window): kernel WM
    // dirty (wm_invalidate_rect below) never actually clears, defeating
    // #564's whole point (idle CPU with a static window open measured
    // UNCHANGED from before the #564 render-gate fix, root-caused to this).
    // Nothing is lost by removing it: wm_draw_apps()'s content_buffer blit
    // (user_window_draw_handler, unconditional whenever it runs) already
    // re-presents the fresh pixels on the very next composite via the
    // wm_invalidate_rect() call below - only the SPURIOUS extra EVENT_REDRAW
    // notification back to the app is removed.
    window_invalidate(user_windows[handle].window);
    wm_invalidate_rect(&user_windows[handle].window->bounds);
    return 0;
}

// (#704) Force every open app window to repaint its CONTENT, not merely
// recomposite whatever is already in its content_buffer.
//
// Measured gap this closes: neither path that changes the active theme
// reaches an app's EVENT_REDRAW handler. sys_set_theme() (explicit switch in
// Settings) calls wm_invalidate_all(), which only flips wm_state.dirty.
// full_redraw, consumed by wm_draw_all()/wm_draw_apps() - the kernel-side
// fallback compositor, which does not run while the real userland /APPS/
// COMPOSIT is up. And win_invalidate() (SYS_WIN_INVALIDATE) deliberately
// does NOT arm redraw_pending; see the comment on sys_win_invalidate() above
// (#564: re-arming it there caused a permanent EVENT_REDRAW ping-pong).
// redraw_pending was therefore only ever armed at window create and at
// resize (#453) - which is why, before this change, editing a theme file and
// waiting out the compositor's poll left every already-open window showing
// its old colours until it was resized or reopened; only the compositor's
// OWN chrome (taskbar/menus, restyled directly in compositor_apply_theme())
// and brand-new windows picked up the change live.
//
// This reuses the exact same arming site as create/resize (redraw_pending),
// so it flows through the existing coalesced "queue at most one EVENT_REDRAW"
// logic in user_window_draw_handler and cannot reintroduce the #564
// ping-pong: this function is called once per detected theme change (an
// edge, not a level) from the compositor's existing ~2s theme poll, never
// from a per-composite-frame path.
int64_t sys_wm_force_redraw_all(void) {
    int n = 0;
    for (int i = 0; i < MAX_USER_WINDOWS; i++) {
        if (user_windows[i].window) {
            user_windows[i].redraw_pending = 1;
            n++;
        }
    }
    return (int64_t)n;
}

int64_t sys_win_get_pos(int handle, int *x, int *y) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }
    window_t *win = user_windows[handle].window;
    // #567: fault-safe out-param writes.
    if (x) { int kx = (int)win->bounds.x; if (copy_to_user(x, &kx, sizeof(kx)) != 0) return -14; }
    if (y) { int ky = (int)win->bounds.y; if (copy_to_user(y, &ky, sizeof(ky)) != 0) return -14; }
    return 0;
}

// #334: reposition a user window to an absolute screen position. Mirrors the
// SYS_WM_MAXIMIZE_WINDOW pattern (change kernel window bounds, force a full
// redraw); the compositor composites app windows at their kernel-reported
// bounds (see sys_wm_get_windows), so the moved window follows on the next
// frame. This makes ANY app window movable, including borderless (nochrome)
// panels like the Maytera HiFi that have no title bar for the WM to drag.
int64_t sys_win_move(int handle, int x, int y) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window)
        return -1;
    window_t *win = user_windows[handle].window;
    extern void wm_invalidate_all(void);
    // (#745) One clamp, shared with the title-bar drag path. This used to be a
    // private copy that clamped to the SCREEN (y >= 0), which let SYS_WIN_MOVE
    // park a title bar under a top panel that wm_handle_mouse_move() would
    // have refused: two paths describing the same geometry, disagreeing.
    {
        int32_t nx = (int32_t)x, ny = (int32_t)y;
        wm_clamp_to_work_area(win, &nx, &ny);
        win->bounds.x = nx;
        win->bounds.y = ny;
    }
    wm_invalidate_all();
    return 0;
}

int64_t sys_win_move_by(int handle, int dx, int dy) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window)
        return -1;
    window_t *win = user_windows[handle].window;
    return sys_win_move(handle, (int)win->bounds.x + dx, (int)win->bounds.y + dy);
}

int64_t sys_win_get_size(int handle, int *width, int *height) {
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];
    // #567: fault-safe out-param writes.
    if (width)  { int kw = uw->content_width;  if (copy_to_user(width, &kw, sizeof(kw)) != 0) return -14; }
    if (height) { int kh = uw->content_height; if (copy_to_user(height, &kh, sizeof(kh)) != 0) return -14; }
    return 0;
}

// Fix sign-extended 32-bit user pointer (userland passes 32-bit addr, kernel zero-extends)
// #503: the prefix is USER_PTR_SX_PREFIX (proc/syscall_argtab.h) and is MIRRORED
// by sanitize_user_ptr() in rustkern.rs, because the argtab validator has to
// validate the address this macro produces rather than the raw arg Ring 3 passed.
// If you change the condition here, change it there; syscall_argtab_lock.c
// asserts the constant but cannot assert the shape of this expression.
#define SANITIZE_USER_PTR(ptr, type) do { \
    uint64_t __addr = (uint64_t)(ptr); \
    if ((__addr & USER_PTR_SX_PREFIX) == USER_PTR_SX_PREFIX) { \
        (ptr) = (type)((__addr) & 0xFFFFFFFFULL); \
    } \
} while(0)

int64_t sys_win_blit(int handle, int __attribute__((unused)) x, int __attribute__((unused)) y,
                     int src_w, int src_h, uint32_t *src_buffer) {
    SANITIZE_USER_PTR(src_buffer, uint32_t *);

    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) {
        return -1;
    }

    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !src_buffer) {
        return -1;
    }

    window_t *win = uw->window;
    // Size the content to the window's TRUE content rectangle (inside the
    // title bar + borders), so a full-window blit fills it exactly and never
    // overflows the frame. (Was bounds.width x bounds.height-30, which is too
    // wide and the wrong height -> right overflow + bottom gap.)
    int32_t cbx, cby, cbw, cbh;
    window_get_content_bounds(win, &cbx, &cby, &cbw, &cbh);
    int dst_w = cbw;
    int dst_h = cbh;
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;

    // Grow content buffer if needed
    if (dst_w > uw->content_width || dst_h > uw->content_height) {
        int new_w = (dst_w > uw->content_width)  ? dst_w : uw->content_width;
        int new_h = (dst_h > uw->content_height) ? dst_h : uw->content_height;
        uint32_t *new_buf = (uint32_t *)kmalloc(new_w * new_h * 4);
        if (new_buf) {
            for (int i = 0; i < new_w * new_h; i++) new_buf[i] = 0xFF000000;
            if (uw->content_buffer) kfree(uw->content_buffer);
            uw->content_buffer = new_buf;
            uw->content_width  = new_w;
            uw->content_height = new_h;
        }
    }

    // Nearest-neighbor scale from src (src_w x src_h) to dst (dst_w x dst_h)
    int scale_x_fp = (src_w << 8) / dst_w;
    int scale_y_fp = (src_h << 8) / dst_h;
    for (int dy = 0; dy < dst_h; dy++) {
        int sy = (dy * scale_y_fp) >> 8;
        if (sy >= src_h) sy = src_h - 1;
        uint32_t *src_row = src_buffer + sy * src_w;
        uint32_t *dst_row = uw->content_buffer + dy * dst_w;
        for (int dx = 0; dx < dst_w; dx++) {
            int sx = (dx * scale_x_fp) >> 8;
            if (sx >= src_w) sx = src_w - 1;
            dst_row[dx] = src_row[sx];
        }
    }

    window_invalidate(win);
    return 0;
}

// Blit a w*h BGRA pixel buffer into a user window's content buffer at (x,y),
// clipped. Used by the browser to place decoded inline <img> images.
int64_t sys_win_draw_image(int handle, int x, int y, int w, int h, uint32_t *src) {
    SANITIZE_USER_PTR(src, uint32_t *);
    if (handle < 0 || handle >= MAX_USER_WINDOWS || !user_windows[handle].window) return -1;
    user_window_t *uw = &user_windows[handle];
    if (!uw->content_buffer || !src || w <= 0 || h <= 0) return -1;
    // #567: bounce the whole user pixel buffer into a kernel buffer fault-safe,
    // then blit from kernel memory (was a raw src[...] read per pixel).
    if ((uint64_t)w * (uint64_t)h > 67108864ULL) return -1;   // 64M-px sanity guard
    size_t nbytes = (size_t)w * (size_t)h * 4u;
    uint32_t *ksrc = (uint32_t *)kmalloc(nbytes);
    if (!ksrc) return -1;
    if (copy_from_user(ksrc, src, nbytes) != 0) { kfree(ksrc); return -14; }
    int cw = uw->content_width, ch = uw->content_height;
    for (int ry = 0; ry < h; ry++) {
        int dy = y + ry;
        if (dy < 0 || dy >= ch) continue;
        uint32_t *drow = &uw->content_buffer[(uint32_t)dy * (uint32_t)cw];
        uint32_t *srow = &ksrc[(uint32_t)ry * (uint32_t)w];
        for (int rx = 0; rx < w; rx++) {
            int dx = x + rx;
            if (dx < 0 || dx >= cw) continue;
            drow[dx] = srow[rx];
        }
    }
    kfree(ksrc);
    window_invalidate(uw->window);
    return 0;
}

// Parallel image downscale (#279 stage 3a). One output-row range per core; the
// range fn touches only kernel memory (src image + a kernel dst), so it is safe
// to run on APs (which lack the caller's user mappings).
struct img_scale_ctx { const uint32_t *src; uint32_t *dst; int sw, sh, dw, dh; int has_alpha; };
static void img_scale_rows(int y0, int y1, void *vc) {
    struct img_scale_ctx *c = (struct img_scale_ctx *)vc;
    for (int y = y0; y < y1; y++) {
        int sy = y * c->sh / c->dh;
        const uint32_t *srow = &c->src[(uint32_t)sy * (uint32_t)c->sw];
        uint32_t *drow = &c->dst[(uint32_t)y * (uint32_t)c->dw];
        for (int x = 0; x < c->dw; x++) {
            uint32_t p = srow[x * c->sw / c->dw];
            uint32_t a = (p >> 24) & 0xFF;
            // #745: only composite when the SOURCE ACTUALLY CARRIES ALPHA. The
            // BMP decoder documents its output as 0x00RRGGBB - the alpha byte is
            // deliberately zero, not "transparent" - so treating it as alpha
            // blended EVERY pixel of EVERY BMP onto white and handed back a pure
            // white image with a success return. That is why the OOBE wallpaper
            // grid drew 20 blank cells while open/read/decode/blit all reported
            // success, and it hits every SYS_DECODE_IMAGE caller with a BMP
            // (browser <img>, Files thumbnails), not just the wizard.
            if (c->has_alpha && a < 255) {
                uint32_t ia = 255u - a;
                uint32_t r = (((p >> 16) & 0xFF) * a + 255u * ia) / 255u;
                uint32_t g = (((p >> 8)  & 0xFF) * a + 255u * ia) / 255u;
                uint32_t b = (( p        & 0xFF) * a + 255u * ia) / 255u;
                p = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            drow[x] = p;
        }
    }
}

int64_t sys_decode_image(const void *data, uint32_t len, uint32_t target,
                         void *out, uint32_t out_cap, int *dims) {
    SANITIZE_USER_PTR(data, const void *);
    SANITIZE_USER_PTR(out, void *);
    SANITIZE_USER_PTR(dims, int *);
    if (!data || !len || !out || !dims) return -1;
    int tw = (int)(target >> 16), th = (int)(target & 0xFFFF);
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    extern int image_load(const void *data, uint32_t size, image_t *img);
    extern void image_free(image_t *img);
    image_t img; img.pixels = 0; img.width = 0; img.height = 0;
    if (image_load(data, len, &img) != 0 || !img.pixels || !img.width || !img.height) {
        if (img.pixels) image_free(&img);
        return -1;
    }
    int sw = (int)img.width, sh = (int)img.height;
    int dw = sw, dh = sh;
    if (dw > tw) { dh = (int)((long)dh * tw / dw); dw = tw; }
    if (dh > th) { dw = (int)((long)dw * th / dh); dh = th; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    if ((uint32_t)(dw * dh * 4) > out_cap) {
        int maxrows = (int)(out_cap / (uint32_t)(dw * 4));
        if (maxrows < 1) { image_free(&img); return -1; }
        dh = maxrows;
    }
    // #279 stage 3a: scale in parallel across cores. APs cannot write the user
    // buffer (no user mappings). #567: always scale into a kernel temp, then do
    // one fault-safe copy_to_user (removes the single-core in-place raw write to
    // the user `out` buffer, which was TOCTOU-unsafe).
    extern int smp_get_core_count(void);
    extern void smp_parallel_for(int, int, void (*)(int, int, void *), void *);
    uint32_t outbytes = (uint32_t)dw * (uint32_t)dh * 4u;
    uint32_t *kdst = (uint32_t *)kmalloc(outbytes);
    if (!kdst) { image_free(&img); return -1; }
    // Does this source actually carry alpha? A decoder that emits 0x00RRGGBB
    // leaves every alpha byte zero; a real RGBA image essentially never has a
    // zero alpha in EVERY pixel. Sample rather than scan the whole image.
    int has_alpha = 0;
    {
        uint32_t total = (uint32_t)sw * (uint32_t)sh;
        uint32_t step = total / 256u; if (step == 0) step = 1;
        for (uint32_t i = 0; i < total; i += step) {
            if (((img.pixels[i] >> 24) & 0xFFu) != 0u) { has_alpha = 1; break; }
        }
    }
    struct img_scale_ctx ctx = { img.pixels, kdst, sw, sh, dw, dh, has_alpha };
    if (smp_get_core_count() > 1) smp_parallel_for(0, dh, img_scale_rows, &ctx);
    else                          img_scale_rows(0, dh, &ctx);
    image_free(&img);
    int drc = 0;
    if (copy_to_user(out, kdst, outbytes) != 0) drc = -14;
    kfree(kdst);
    if (drc) return drc;
    int kdims[2] = { dw, dh };
    if (copy_to_user(dims, kdims, sizeof(kdims)) != 0) return -14;
    return (int64_t)(dw * dh * 4);
}


// ============================================================================
// Filesystem syscalls (mkdir, rmdir, unlink, rename, readdir)
// ============================================================================

int64_t sys_mkdir(const char *upath, int mode) {
    (void)mode;
    if (!upath) return -1;
    // #567: bounce the user path fault-safe; every use below reads kpath, and
    // the subsystem calls (fat/smb) now receive a kernel pointer.
    char kpath[SC_PATH_MAX];
    if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0) return -14;
    const char *path = kpath;

    // #317: SMB network share.
    if (path_is_smb(path)) {
        if (smb_vfs_ensure_mount(path) != 0) return -1;
        return smb_mkdir(path) == 0 ? 0 : -1;
    }

    // Permission check: need W_OK on parent directory
    process_t *p = proc_current();
    if (p && p->privilege == PRIV_USER) {
        char parent[256];
        sc_parent_of(path, parent, sizeof(parent));   // #676: one shared helper
        if (perms_check(parent, p->euid, p->egid, W_OK) != 0) {
            return -1;  // EACCES
        }
    }

    int ret = fat_mkdir(&g_fat_fs, path);
    if (ret == 0 && p) {
        perms_set_default(path, p->euid, p->egid, 1);
    }
    return ret;
}

int64_t sys_rmdir(const char *upath) {
    if (!upath) return -1;
    // #567: bounce the user path fault-safe.
    char kpath[SC_PATH_MAX];
    if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0) return -14;
    const char *path = kpath;

    // #317: SMB network share.
    if (path_is_smb(path)) {
        if (smb_vfs_ensure_mount(path) != 0) return -1;
        return smb_rmdir(path) == 0 ? 0 : -1;
    }

    // Permission check: need W_OK on parent directory
    process_t *p = proc_current();
    if (p && p->privilege == PRIV_USER) {
        char parent[256];
        sc_parent_of(path, parent, sizeof(parent));   // #676: one shared helper
        if (perms_check(parent, p->euid, p->egid, W_OK) != 0) {
            return -1;  // EACCES
        }
    }

    int ret = fat_delete(&g_fat_fs, path);
    if (ret == 0) perms_remove(path);
    return ret;
}

int64_t sys_unlink(const char *upath) {
    if (!upath) return -1;
    // #567: bounce the user path fault-safe.
    char kpath[SC_PATH_MAX];
    if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0) return -14;
    const char *path = kpath;

    // #317: SMB network share.
    if (path_is_smb(path)) {
        if (smb_vfs_ensure_mount(path) != 0) return -1;
        return smb_delete(path) == 0 ? 0 : -1;
    }

    // Permission check: need W_OK on parent directory.
    // #676: this USED TO SIT BELOW the "/ext2/..." early return, so an explicit
    // "/ext2/"-prefixed path reached ext2_unlink() having been checked by
    // NOTHING. Any Ring-3 process could delete any file on the ext2 root, the
    // permission database and the shadow file included, purely by spelling the
    // path with the mount prefix. The check now runs FIRST, for every backend.
    process_t *p = proc_current();
    if (p && p->privilege == PRIV_USER) {
        char parent[256];
        sc_parent_of(path, parent, sizeof(parent));   // #676: one shared helper
        if (perms_check(parent, p->euid, p->egid, W_OK) != 0) {
            return -1;  // EACCES
        }
    }

    // ext2 volume (#99): delete on the mounted ext2 filesystem.
    if (path_is_ext2(path)) {
        int r = ext2_unlink(ext2_relpath(path));
        // #679: drop the policy entry with the file. A stale entry is not
        // inert: the next file created under that name would silently inherit
        // the DELETED file's owner and mode instead of its creator's, because
        // perms_on_create() declines to overwrite an entry that already exists.
        if (r == 0) perms_remove(path);
        return r == 0 ? 0 : -1;
    }

    int ret = fat_delete(&g_fat_fs, path);
    if (ret == 0) perms_remove(path);
    return ret;
}

int64_t sys_rename(const char *u_oldpath, const char *u_newpath) {
    if (!u_oldpath || !u_newpath) return -1;
    // #567: bounce both user paths fault-safe into kernel buffers.
    char kold[SC_PATH_MAX], knew[SC_PATH_MAX];
    if (strncpy_from_user(kold, u_oldpath, sizeof(kold)) < 0) return -14;
    if (strncpy_from_user(knew, u_newpath, sizeof(knew)) < 0) return -14;
    const char *oldpath = kold;
    const char *newpath = knew;

    // #317: SMB network share (both paths must be on the same share).
    if (path_is_smb(oldpath) || path_is_smb(newpath)) {
        if (!path_is_smb(oldpath) || !path_is_smb(newpath)) return -1;
        if (smb_vfs_ensure_mount(oldpath) != 0) return -1;
        return smb_rename(oldpath, newpath) == 0 ? 0 : -1;
    }

    // Permission check: need W_OK on both parent directories
    process_t *p = proc_current();
    if (p && p->privilege == PRIV_USER) {
        char parent[256];
        strncpy(parent, oldpath, sizeof(parent) - 1);
        parent[255] = '\0';
        char *last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) *last_slash = '\0';
        else { parent[0] = '/'; parent[1] = '\0'; }
        if (perms_check(parent, p->euid, p->egid, W_OK) != 0) return -1;

        strncpy(parent, newpath, sizeof(parent) - 1);
        parent[255] = '\0';
        last_slash = strrchr(parent, '/');
        if (last_slash && last_slash != parent) *last_slash = '\0';
        else { parent[0] = '/'; parent[1] = '\0'; }
        if (perms_check(parent, p->euid, p->egid, W_OK) != 0) return -1;
    }

    int ret = fat_rename(&g_fat_fs, oldpath, newpath);
    if (ret == 0) {
        uint32_t uid, gid;
        uint16_t mode;
        if (perms_get(oldpath, &uid, &gid, &mode) == 0) {
            perms_remove(oldpath);
            perms_set(newpath, uid, gid, mode);
        }
    }
    return ret;
}


/* #317 pass 4: verify the terminal cat/ls path (sys_open/sys_read/sys_readdir)
   over the persisted network mounts loaded from /CONFIG/NETMOUNTS.CFG. This is
   the exact syscall path the shell `ls`/`cat` and the Files app use, exercised
   for both /SMB and /NFS. Runs from the deferred net worker; logs to serial. */
void netfs_fdpath_selftest(void) {
    extern int net_mounts_count(void);
    extern const char *net_mounts_path(int);
    typedef struct { char name[256]; uint32_t type; uint32_t size; } dirent_t;
    int n = net_mounts_count();
    if (n <= 0) return;
    kprintf("\n========== NETFS FD-PATH (task #317 pass 4) ==========\n");
    for (int m = 0; m < n; m++) {
        const char *mp = net_mounts_path(m);
        if (!mp) continue;
        kprintf("[NETFD] --- %s ---\n", mp);
        // ls: open the mount root as a directory, enumerate via sys_readdir.
        // #567: kernel-buffer callers use the *_k cores (the user-facing wrappers
        // now copy_*_user, which would reject these kernel pointers).
        int dfd = (int)sys_open_k(mp, 0);   // O_RDONLY
        if (dfd < 0) { kprintf("[NETFD]   open dir FAILED\n"); continue; }
        int cnt = 0; dirent_t de;
        char first[256]; first[0] = 0;
        while (sys_readdir_k(dfd, (sc_dirent_t *)&de) == 0 && cnt < 64) {
            if (cnt == 0) { int i=0; while(de.name[i]&&i<255){first[i]=de.name[i];i++;} first[i]=0; }
            kprintf("[NETFD]   %s\n", de.name);
            cnt++;
        }
        sys_close(dfd);
        kprintf("[NETFD]   ls: %d entries (first=%s)\n", cnt, first);
        // cat: read TEST.TXT from the mount root via the fd path.
        char fp[320]; snprintf(fp, sizeof(fp), "%s/TEST.TXT", mp);
        int ffd = (int)sys_open_k(fp, 0);
        if (ffd >= 0) {
            char buf[256]; int64_t r = sys_read(ffd, buf, sizeof(buf) - 1);
            if (r > 0) { buf[r] = 0; kprintf("[NETFD]   cat %s (%d bytes): %s\n", fp, (int)r, buf); }
            else kprintf("[NETFD]   cat %s read=%d\n", fp, (int)r);
            sys_close(ffd);
        } else {
            kprintf("[NETFD]   (no TEST.TXT to cat at this mount)\n");
        }
    }
    kprintf("======================================================\n\n");
}

/* #308 self-test: exercise the EXACT userland directory-listing path
   (sys_open(dir, O_RDONLY) followed by sys_readdir) on the ext2 root, the way
   the Files app does it. Logs results to serial at boot so we can confirm
   directory opens + readdir work from the syscall layer (not just kernel-side
   ext2_readdir_ino). Tests "/", "/APPS", "/CONFIG". */
void ext2_dir_open_selftest(void) {
    extern int g_root_ext2;
    if (!g_root_ext2) { kprintf("[#308] ext2 not root; skipping dir-open self-test\n"); return; }
    static const char *dirs[] = { "/", "/APPS", "/CONFIG" };
    typedef struct { char name[256]; uint32_t type; uint32_t size; } dirent_t;
    for (unsigned di = 0; di < sizeof(dirs)/sizeof(dirs[0]); di++) {
        const char *path = dirs[di];
        int64_t fd = sys_open_k(path, 0);   // #567: kernel path -> *_k core
        if (fd < 0) {
            kprintf("[#308] FAIL: sys_open(%s) returned -1 (dir open broken)\n", path);
            continue;
        }
        int count = 0;
        dirent_t de;
        char first[64]; first[0] = 0;
        while (sys_readdir_k((int)fd, (sc_dirent_t *)&de) == 0) {
            if (count == 0) { int i=0; while (de.name[i] && i<63){first[i]=de.name[i];i++;} first[i]=0; }
            count++;
            if (count > 5000) break;
        }
        sys_close((int)fd);
        kprintf("[#308] sys_open(%s)=%ld, sys_readdir listed %d entries (first=%s)\n",
                path, (long)fd, count, first);
        if (count == 0)
            kprintf("[#308] FAIL: dir %s opened but listed 0 entries (readdir broken)\n", path);
        else
            kprintf("[#308] PASS: dir %s listing works from userland syscall path\n", path);
    }
}


// ============================================================================
// Working directory syscalls
// ============================================================================

int64_t sys_getcwd(char *ubuf, uint64_t size) {
    process_t *p = proc_current();
    if (!p || !ubuf || size == 0) return -1;
    // Return "/" as default if cwd has not been set
    const char *src = (p->cwd[0]) ? p->cwd : "/";
    // #567: build the (possibly truncated) result in a kernel buffer, then one
    // fault-safe copy_to_user. p->cwd is bounded to sizeof(kbuf), so capping the
    // kernel staging buffer at that size never truncates below the old behavior.
    char kbuf[sizeof(p->cwd)];
    uint64_t cap = (size < sizeof(kbuf)) ? size : sizeof(kbuf);
    uint64_t i = 0;
    while (i < cap - 1 && src[i]) { kbuf[i] = src[i]; i++; }
    kbuf[i] = '\0';
    if (copy_to_user(ubuf, kbuf, i + 1) != 0) return -14;
    return (int64_t)i;
}

// ===========================================================================
// #745 Stage 3: sys_chdir. FOUR faults, characterised before any was fixed.
//
// FAULT 1, WRONG ACCESS CLASS (the one fs/perms.c predicted). It validated by
// calling sys_open_k(resolved, 0), i.e. O_RDONLY, which asks perms_check() for
// R_OK on the target. POSIX chdir requires SEARCH (x) and explicitly NOT read.
// The two differ exactly on a 0711 directory, which is what #745 Stage 1 moved
// /CONFIG to, so after that change a non-root process could TRAVERSE /CONFIG
// but could not cd into it. Recorded there as a pre-existing chdir bug, and it
// is: it applies to every 0711 directory, not just that one.
//
// FAULT 2, NO DIRECTORY CHECK AT ALL. Nothing tested that the target was a
// directory. chdir("/CONFIG/PASSWD") on a world-readable 0644 FILE returned 0
// and set cwd to it, after which every relative path in that process resolved
// under a regular file. That is a plain correctness bug and it is independent
// of permissions: it reproduces at uid 0.
//
// FAULT 3, SILENT TRUNCATION. `resolved` was 256 bytes while the bounced path
// is SC_PATH_MAX (1024), and both branches truncated with `while (i < 255 ...)`
// and then proceeded. A path longer than 255 bytes silently became a DIFFERENT
// path, one that could name a different directory than the caller asked for.
// Truncating a path and then acting on it is never acceptable; the answer is to
// refuse.
//
// FAULT 4, NO CANONICALIZATION, which is what makes fault 3 reachable rather
// than theoretical. Nothing collapsed "." or popped "..", so cwd accumulated
// them: from /HOME/ADMIN, `cd ..` set cwd to the literal string
// "/HOME/ADMIN/..", and the next `cd ..` to "/HOME/ADMIN/../..". Each one grows
// the string by three bytes, so a user holding down `cd ..` in the shell walks
// cwd into the 256-byte truncation of fault 3 in under ninety keystrokes. It
// also meant getcwd() reported a path no human would recognise.
//
// THE FIX uses the primitives that already exist rather than new ones:
// perms_canon_rs() (rustkern/permpath.rs) is the SAME canonicalizer
// perms_check() runs before every permission decision, so the string that is
// authorized here and the string stored in p->cwd are canonical in the same
// sense; and perms_check(target, X_OK) is the POSIX rule, with the walker
// already requiring x on every component above it.
//
// Justified-C, not Rust: there is no new policy here. The one DECISION, what a
// path canonicalizes to, is already in Rust and is called; the rest is in-place
// repair of an existing C handler against existing C primitives (p->cwd,
// sys_open_k, the FAT/ext2 resolvers), which is the entanglement exemption.
// ===========================================================================

// Is `kpath` an existing DIRECTORY? 1 yes, 0 no (exists but not a directory, or
// does not exist). Local filesystems only; the caller handles SMB/NFS.
//
// Deliberately NOT a new resolver: it asks the same two backends sys_stat_path
// asks, in the same order (ext2 root first, then FAT), so chdir and stat cannot
// disagree about what a directory is.
static int sc_path_is_dir(const char *kpath) {
    if (kpath[0] == '/' && kpath[1] == '\0') return 1;   // "/" always
    {
        const char *rel = 0;
        if (path_is_ext2(kpath))        rel = ext2_relpath(kpath);
        else if (path_root_ext2(kpath)) rel = kpath;
        if (rel) {
            uint32_t ino = ext2_resolve_path(rel);
            if (ino) {
                ext2_inode_t in;
                if (ext2_read_inode(ino, &in) == 0)
                    return ((in.i_mode & 0xF000) == 0x4000) ? 1 : 0;
            }
            // Not on ext2: fall through to FAT (may be ESP-only).
        }
    }
    fat_file_t f;
    if (fat_open(&g_fat_fs, kpath, &f) != 0) return 0;
    int is_dir = f.is_dir || (f.attr & FAT_ATTR_DIRECTORY);
    if (f.open) fat_close(&f);
    return is_dir ? 1 : 0;
}

int64_t sys_chdir(const char *upath) {
    process_t *p = proc_current();
    if (!p || !upath) return -1;
    char kpath[SC_PATH_MAX];
    if (strncpy_from_user(kpath, upath, sizeof(kpath)) < 0) return -14;

    // FAULT 3: build the absolute form in a FULL-SIZE buffer and REFUSE on
    // overflow. Never truncate a path and then act on it.
    char abs[SC_PATH_MAX];
    if (kpath[0] == '/') {
        uint64_t i = 0;
        while (kpath[i]) { if (i >= sizeof(abs) - 1) return -1; abs[i] = kpath[i]; i++; }
        abs[i] = '\0';
    } else {
        const char *base = (p->cwd[0]) ? p->cwd : "/";
        uint64_t ci = 0;
        while (base[ci]) { if (ci >= sizeof(abs) - 2) return -1; abs[ci] = base[ci]; ci++; }
        if (ci > 0 && abs[ci - 1] != '/') abs[ci++] = '/';
        uint64_t pi = 0;
        while (kpath[pi]) { if (ci >= sizeof(abs) - 1) return -1; abs[ci++] = kpath[pi++]; }
        abs[ci] = '\0';
    }

    // FAULT 4: canonicalize with the SAME function perms_check() uses, so the
    // string checked and the string stored agree with the permission walker.
    extern int perms_canon_rs(const char *src, char *out, uint32_t cap);
    char canon[SC_PATH_MAX];
    if (perms_canon_rs(abs, canon, sizeof(canon)) < 0) return -1;

    // p->cwd is PROC_CWD_MAX. Refuse rather than store a truncated cwd, which
    // would make every later relative path in this process resolve elsewhere.
    uint64_t clen = 0;
    while (canon[clen]) clen++;
    if (clen >= PROC_CWD_MAX) return -1;

    // FAULT 2: it must actually BE a directory. SMB/NFS keep the existing
    // open-based validation; they are remote namespaces with server-side
    // access control and no perms.c entries.
    if (path_is_smb(canon) || path_is_nfs(canon)) {
        int fd = (int)sys_open_k(canon, 0);
        if (fd < 0) return -1;
        sys_close(fd);
    } else {
        if (!sc_path_is_dir(canon)) return -1;
        // FAULT 1: POSIX chdir needs SEARCH, not READ. perms_path_check_rs()
        // requires x on every component above `canon` as part of this call.
        if (p->privilege == PRIV_USER &&
            perms_check(canon, p->euid, p->egid, X_OK) != 0)
            return -13;   // EACCES
    }

    uint64_t i = 0;
    while (canon[i]) { p->cwd[i] = canon[i]; i++; }
    p->cwd[i] = '\0';
    return 0;
}

// ============================================================================
// User identity syscalls
// ============================================================================

int64_t sys_getuid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->uid : -1;
}

int64_t sys_setuid(uint32_t uid) {
    process_t *p = proc_current();
    if (!p) return -1;

    // Root (euid 0) can set to any UID
    if (p->euid == 0) {
        p->uid = uid;
        p->euid = uid;
        return 0;
    }

    // Non-root can only set euid back to real uid
    if (uid == p->uid) {
        p->euid = uid;
        return 0;
    }

    return -1;  // EPERM
}

int64_t sys_getgid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->gid : -1;
}

int64_t sys_setgid(uint32_t gid) {
    process_t *p = proc_current();
    if (!p) return -1;

    if (p->euid == 0) {
        p->gid = gid;
        p->egid = gid;
        return 0;
    }

    if (gid == p->gid) {
        p->egid = gid;
        return 0;
    }

    return -1;  // EPERM
}

int64_t sys_geteuid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->euid : -1;
}

int64_t sys_getegid(void) {
    process_t *p = proc_current();
    return p ? (int64_t)p->egid : -1;
}

int64_t sys_seteuid(uint32_t euid) {
    process_t *p = proc_current();
    if (!p) return -1;

    if (p->euid == 0) {
        p->euid = euid;
        return 0;
    }

    if (euid == p->uid) {
        p->euid = euid;
        return 0;
    }

    return -1;  // EPERM
}

int64_t sys_setegid(uint32_t egid) {
    process_t *p = proc_current();
    if (!p) return -1;

    if (p->euid == 0) {
        p->egid = egid;
        return 0;
    }

    if (egid == p->gid) {
        p->egid = egid;
        return 0;
    }

    return -1;  // EPERM
}

// #554: chmod/chown must behave correctly on BOTH ext2/POSIX paths (perms.c,
// unchanged below) and genuine FAT (ESP: /boot, /EFI) paths, which have no
// uid/gid/mode concept at all. The fs-type routing decision + the FAT-specific
// handling (map the write bit to the real on-disk FAT_ATTR_READ_ONLY bit;
// refuse chown outright rather than fake success) is new logic, so it lives in
// Rust per the 2026-07-16 rule (rustkern/fsperm.rs: rk_chmod_route /
// rk_chown_route). perms_chmod()/perms_set() themselves are untouched.
int64_t sys_chmod(const char *path, uint16_t mode) {
    if (!path) return -1;

    process_t *p = proc_current();
    if (!p) return -1;

    extern int64_t rk_chmod_route(const char *path, uint16_t mode, uint32_t euid);
    return rk_chmod_route(path, mode, p->euid);
}

int64_t sys_chown(const char *path, uint32_t uid, uint32_t gid) {
    if (!path) return -1;

    process_t *p = proc_current();
    if (!p) return -1;

    extern int64_t rk_chown_route(const char *path, uint32_t uid, uint32_t gid, uint32_t euid);
    return rk_chown_route(path, uid, gid, p->euid);
}

// #554: mirrors FsPermInfo in rustkern/fsperm.rs; kept here only for the
// sizeof lock (Rust owns the actual read/write of this buffer, via
// rk_fs_perm_info - see sys_fs_perm_info() below).
typedef struct {
    uint8_t  fs_type;        // 0 = ext2/POSIX (perms.c), 1 = FAT (ESP), 2 = other (SMB/NFS)
    uint8_t  is_dir;
    uint8_t  has_perm_entry; // fs_type==0 only
    uint8_t  fat_attr;       // fs_type==1 only: raw on-disk FAT_ATTR_* byte
    uint16_t mode;           // fs_type==0 only: rwxrwxrwx bits
    uint16_t reserved;
    uint32_t uid;            // fs_type==0 only
    uint32_t gid;            // fs_type==0 only
} k_fsperm_info_t;
// #554 argtab sizeof-lock: SYS_FS_PERM_INFO arg3 (SZ_FSPERM_INFO in
// rustkern/argtab.rs).
_Static_assert(sizeof(k_fsperm_info_t) == 16,
               "#554 argtab: SZ_FSPERM_INFO in rustkern/argtab.rs is stale");

// Filesystem-aware permission/attribute info: ext2/POSIX paths report the
// perms.c-backed uid/gid/mode (the SAME values sys_open()'s perms_check()
// actually enforces); genuine FAT paths report the real on-disk attribute
// byte and never a fabricated ext2-style mode. Backs the Files Properties
// permissions tab and the details-view attribute columns/filtering.
int64_t sys_fs_perm_info(const char *u_path, int reserved_unused, void *ubuf) {
    (void)reserved_unused;
    if (!u_path || !ubuf) return -1;

    // #745: bounce ONCE, then the SAME gate sys_stat_path uses. This syscall
    // reported uid, gid and mode for any path to any caller, which is a
    // strictly larger disclosure than stat's, and it was the other half of the
    // limit recorded against /CONFIG 0711 in fs/perms.c.
    char kpath[SC_PATH_MAX];
    if (sc_bounce_str(u_path, kpath, sizeof(kpath)) != 0) return -14;
    if (sc_meta_permit(kpath) != 0) return -13;   // EACCES

    int is_net = (path_is_smb(kpath) || path_is_nfs(kpath)) ? 1 : 0;
    extern int64_t rk_fs_perm_info(const char *path, int smb_or_nfs, void *out);
    return rk_fs_perm_info(kpath, is_net, ubuf);
}

// #565: parse a /THEMES/*.mtheme file and add/update it in the live theme
// table (see kernel/gui/themes.c theme_load_file_runtime()). Bounded copy of
// the path, mirroring the existing sys_bootlog_write() idiom for user
// strings elsewhere in this file.
int64_t sys_theme_load_file(const char *upath) {
    if (!upath) return -1;
    // #567: bounce the user path fault-safe into the kernel buffer.
    char buf[128];
    if (sc_bounce_str(upath, buf, sizeof(buf)) != 0) return -1;

    // #700 B7: the caller names ANY path and the kernel opens and parses it in
    // Ring 0. MEASURED on golden 1025 at uid 1000: theme_load_file_runtime()
    // was handed the root-owned 0600 /CONFIG/AUTHKEYS and returned a live theme
    // index, logging "[Themes] Loaded theme 'Untitled' as new index 12 from
    // /CONFIG/AUTHKEYS". No disclosure channel back to the caller was found
    // (the parser keeps only the fields it recognises, and a key file sets
    // none), so the measured impact today is a parser reachable on unreadable
    // input rather than a read primitive. That is an argument about the CURRENT
    // parser, not about the boundary, and it stops being true the first time
    // anyone adds a string field a theme can carry. The check is on the read
    // itself, which is where it stays correct.
    {
        process_t *p = proc_current();
        if (p && p->privilege == PRIV_USER &&
            perms_check(buf, p->euid, p->egid, R_OK) != 0)
            return -1;
    }

    extern int theme_load_file_runtime(const char *path);
    return (int64_t)theme_load_file_runtime(buf);
}

// (themes ticket, 2026-08-07) See SYS_THEME_CONTRAST_CORRECTIONS in syscall.h.
// No permission check: this only reads a small already-public integer off the
// live in-memory theme table (the same table SYS_THEME_COLOR already exposes
// color-by-color to any process), it names no path and touches no file.
int64_t sys_theme_contrast_corrections(int64_t theme_id) {
    extern int theme_get_contrast_corrections(int theme_id);
    return (int64_t)theme_get_contrast_corrections((int)theme_id);
}

// ===========================================================================
// #745 Stage 3: THE CREDENTIAL CHOKEPOINT (sys_passwd_change / sys_su).
//
// TWO faults lived in these two functions and only one of them was on the
// ticket.
//
// FAULT 1, the one that was reported: neither called users_authenticate().
// users.h states the rule in as many words, that interactive auth paths MUST
// call users_authenticate() so failed attempts are counted and lockouts
// enforced (#566). sys_su() called the raw user_verify_password() instead, so a
// Ring-3 loop over SYS_SU could try passwords for ANY account, root included,
// at syscall speed, with no attempt counter, no escalating lockout and no audit
// record. The lockout that protects the login gate and the lock screen was
// simply not in this path.
//
// Writing the rule in a header did not enforce the rule. It is now enforced by
// the COMPILER: user_verify_password() is static to proc/users.c and is no
// longer declared in users.h, so there is no second authenticator left in the
// kernel for anyone to reach for, and a future caller that tries fails to
// build. That is the same mechanism-not-memo move that mwt (#707) and buildq
// (#699) each had to make after a convention failed three times.
//
// FAULT 2, found while fixing fault 1, and considerably worse: `username` is a
// RING-3 POINTER and each function read it MORE THAN ONCE. That is the #509
// check-and-use hazard applied to identity, and it is a full local privilege
// escalation rather than a hardening nit:
//
//   sys_passwd_change() read it THREE times. user_lookup_name() resolved the
//   caller's own account, the target->uid == p->euid authorization test passed
//   on it, and then user_set_password() read the SAME pointer a third time. A
//   sibling thread that rewrites the buffer to "root" between the check and the
//   use sets ROOT'S PASSWORD to a string the unprivileged caller chose, knowing
//   only its own password. su(1) with that password is then uid 0.
//
//   sys_su() read it TWICE: verify the caller's own account, then
//   user_lookup_name() resolves "root", and the caller is handed uid 0 outright.
//
// The fix is the one this file already applies everywhere else (#567): bounce
// EVERY user string into a kernel buffer ONCE, up front, and let every later
// use read only that copy. The name that was authenticated and the name whose
// identity is granted are then provably the same BYTES rather than arguably the
// same string. The passwords are bounced too. They are read only once by
// today's callees, but "read once" is a property of the current callee, not of
// the trust boundary, and this whole comment exists because someone relied on
// exactly that kind of property.
//
// AUDIT. Both paths now write an actor-identified record through bootlog_write:
// an identity change that leaves no trace is not something an operator can ever
// reconstruct. The record names the acting uid AND the requested account, so a
// successful escalation and a failed guess are distinguishable after the fact.
// ===========================================================================

int64_t sys_passwd_change(const char *u_username, const char *u_old, const char *u_new) {
    if (!u_username || !u_new) return -1;

    process_t *p = proc_current();
    if (!p) return -1;

    // FAULT 2: bounce ONCE. Every use below reads these kernel copies only.
    char username[USERNAME_MAX];
    char old_pass[SC_PASSWORD_MAX];
    char new_pass[SC_PASSWORD_MAX];
    if (sc_bounce_str(u_username, username, sizeof(username)) != 0) return -14;
    if (sc_bounce_str(u_new, new_pass, sizeof(new_pass)) != 0) return -14;
    int have_old = (u_old && sc_bounce_str(u_old, old_pass, sizeof(old_pass)) == 0);
    if (!have_old) old_pass[0] = 0;

    user_entry_t *target = user_lookup_name(username);
    if (!target) return -1;

    // Non-root may change only its OWN password, and must prove the old one.
    if (p->euid != 0) {
        if (target->uid != p->euid) {
            bootlog_write("[AUTH] passwd DENIED: uid=%u tried to change '%s' (#745)",
                          (unsigned)p->euid, username);
            return -1;  // EPERM
        }
        if (!have_old) return -1;
        // FAULT 1: rate-limited and counted, exactly like the login gate.
        int ar = users_authenticate(username, old_pass);
        if (ar != 0) {
            bootlog_write("[AUTH] passwd DENIED: uid=%u bad old password for '%s'%s (#745)",
                          (unsigned)p->euid, username,
                          (ar == -2) ? " [LOCKED OUT]" : "");
            return (ar == -2) ? -2 : -1;
        }
    }

    // The strength policy runs inside user_set_password(), which is the one
    // chokepoint. A policy rejection comes back as PW_RC(code) and is passed
    // straight out to Ring 3, so `passwd` can print the rule instead of
    // "password NOT changed".
    int r = user_set_password(username, new_pass);
    if (PW_RC_IS_POLICY(r)) {
        bootlog_write("[AUTH] passwd REFUSED: uid=%u, '%s', %s (policy code %d)",
                      (unsigned)p->euid, username,
                      pw_policy_message(PW_RC_CODE(r)), PW_RC_CODE(r));
        return r;
    }
    bootlog_write("[AUTH] passwd %s: uid=%u changed '%s' (#745)",
                  (r == 0) ? "OK" : "FAILED", (unsigned)p->euid, username);
    return r;
}

// Returns 0 on success, -1 on bad credentials, -2 when the account is locked
// out right now. -2 is additive: every existing caller tests non-zero.
int64_t sys_su(const char *u_username, const char *u_password) {
    if (!u_username || !u_password) return -1;

    process_t *p = proc_current();
    if (!p) return -1;

    // FAULT 2: bounce ONCE, then never look at Ring-3 memory again. The whole
    // point of this function is to decide WHICH identity the caller gets, so a
    // name that can change underneath the decision is the entire vulnerability.
    char username[USERNAME_MAX];
    char password[SC_PASSWORD_MAX];
    if (sc_bounce_str(u_username, username, sizeof(username)) != 0) return -14;
    if (sc_bounce_str(u_password, password, sizeof(password)) != 0) return -14;

    // #745 followup: non-root may su only to its OWN account. su-to-root by an
    // unprivileged process is exactly the cross-account authentication we are
    // removing: it was a slow oracle against root and, via the shared lockout
    // counter, a way to lock root out of the login gate. Deny BEFORE
    // users_authenticate() so the target's counter is never touched. Root still
    // su's to anyone; the in-kernel login gate (gui/login.c) is unaffected.
    if (!caller_may_authenticate(username)) {
        bootlog_write("[AUTH] su DENIED (not own account): uid=%u -> '%s' (#745)",
                      (unsigned)p->euid, username);
        return -1;   // EPERM; target's lockout state untouched
    }

    // FAULT 1: the rate-limited authenticator, which is now the only one there
    // is. An account under lockout is refused here before any password compare.
    int ar = users_authenticate(username, password);
    if (ar != 0) {
        bootlog_write("[AUTH] su DENIED: uid=%u -> '%s'%s (#745)",
                      (unsigned)p->euid, username,
                      (ar == -2) ? " [LOCKED OUT]" : "");
        return (ar == -2) ? -2 : -1;
    }

    user_entry_t *target = user_lookup_name(username);
    if (!target) return -1;

    uint32_t from = p->euid;
    p->uid  = target->uid;
    p->gid  = target->gid;
    p->euid = target->uid;
    p->egid = target->gid;

    bootlog_write("[AUTH] su OK: uid=%u -> '%s' uid=%u (#745)",
                  (unsigned)from, username, (unsigned)target->uid);
    return 0;
}

int64_t sys_adduser(const char *u_username, uint32_t uid, uint32_t gid,
                    const char *u_home, const char *u_shell) {
    if (!u_username) return -1;

    process_t *p = proc_current();
    if (!p) return -1;

    // Only root can create users
    if (p->euid != 0) return -1;  // EPERM

    // #567: bounce the user strings fault-safe into kernel buffers before any
    // subsystem (user_create/fat_mkdir/perms_set) reads them.
    char username[USERNAME_MAX];
    char home[SC_PATH_MAX];
    char shell[SC_PATH_MAX];
    if (sc_bounce_str(u_username, username, sizeof(username)) != 0) return -1;
    int have_home  = (u_home  && sc_bounce_str(u_home,  home,  sizeof(home))  == 0);
    int have_shell = (u_shell && sc_bounce_str(u_shell, shell, sizeof(shell)) == 0);

    int ret = user_create(username, uid, gid,
                          have_home  ? home  : "/HOME",
                          have_shell ? shell : "/APPS/MSH",
                          username);
    if (ret != 0) return -1;

    // #745: this call has no password parameter and never wrote a shadow entry,
    // so the account it created landed in PASSWD with NOTHING in SHADOW. That
    // reads as "password not set yet" but is really an undefined state, and it
    // is how the shipped `ref` account (uid 1002) came to exist. Make the state
    // EXPLICIT: mark the account no-login ("*"). It still cannot authenticate,
    // which is correct for an account nobody gave a password to, but it is now a
    // deliberate no-login account rather than an absent record, so
    // users_can_authenticate() and the lock policy see the truth.
    //
    // Callers that want a usable account must use sys_user_create_pw(), which
    // sets the password in the same call or creates nothing at all.
    (void)user_set_nologin(username);
    bootlog_write("[USERS] adduser '%s' uid=%u: created NO-LOGIN (no password supplied); "
                  "use SYS_USER_CREATE_PW for a usable account (#745)",
                  username, (unsigned)uid);

    // Create home directory if specified
    if (have_home && home[0]) {
        fat_mkdir(&g_fat_fs, home);
        perms_set(home, uid, gid, 0750);
        users_make_home_skeleton(home, uid, gid);
    }

    // Persist to disk
    if (users_sync() != 0)
        kprintf("[USERS] sync failed after user change\n");

    return 0;
}

// #745: create an account and set its password, or create nothing. See the
// comment on the declaration in syscall.h for why this is one call and not two.
//
// Written in C rather than Rust, and the justification is entanglement, not
// convenience: every line here is a call into existing C subsystems that own
// mutable global state (user_table/shadow_table in proc/users.c, g_fat_fs,
// perms.c) and none of it is a decision. The two DECISIONS this path makes,
// which uid a new account gets and whether a session may lock, are both in
// rustkern/sessionid.rs with a boot self-test.
// (#306) Enumerate installable disks for Ring 3. Non-destructive, so any
// process may call it - the disk picker in /APPS/INSTALL needs it before the
// user has authenticated as root.
//
// usize is the caller's sizeof(inst_target_t). A stale userland binary built
// against a different layout would otherwise walk the array with the wrong
// stride and silently show garbage capacities in a DESTRUCTIVE picker.
int64_t sys_inst_enum(void *ubuf, int max, int usize) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (!ubuf) return -1;
    if (usize != (int)sizeof(inst_target_t)) {
        kprintf("[INSTALLER] sys_inst_enum: caller struct is %d bytes, kernel is %d\n",
                usize, (int)sizeof(inst_target_t));
        return -22;
    }
    if (max <= 0) return -1;
    if (max > INST_MAX_TARGETS) max = INST_MAX_TARGETS;

    inst_target_t t[INST_MAX_TARGETS];
    int n = inst_enumerate_targets(t, max);
    if (n < 0) return -1;
    if (n > max) n = max;
    if (n > 0 && copy_to_user(ubuf, t, (size_t)n * sizeof(inst_target_t)) != 0)
        return -14;
    return n;
}

// (#306) Install MayteraOS onto a disk. DESTRUCTIVE and root-only.
//
// This takes (kind,index) rather than a descriptor ON PURPOSE. If Ring 3 could
// hand in a full inst_target_t, it could clear is_boot and aim the installer at
// the disk we are running from, or inflate sectors past the real capacity and
// walk the clone off the end. So the kernel re-enumerates and uses ITS OWN
// descriptor; the caller only gets to say which of the kernel's disks it means.
int64_t sys_inst_install(int kind, int index) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (p->euid != 0) {
        kprintf("[INSTALLER] refusing install: uid %u is not root\n", p->euid);
        return -1;
    }
    if (kind < 0 || kind > 255 || index < 0 || index > 255) return -1;

    inst_target_t t[INST_MAX_TARGETS];
    int n = inst_enumerate_targets(t, INST_MAX_TARGETS);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        if (t[i].kind != (uint8_t)kind || t[i].index != (uint8_t)index)
            continue;
        if (t[i].is_boot) {
            kprintf("[INSTALLER] refusing install onto the boot disk\n");
            return -3;
        }
        kprintf("[INSTALLER] Ring 3 requested install to kind=%d idx=%d\n", kind, index);
        return installer_do_install_target(&t[i], NULL, NULL);
    }
    kprintf("[INSTALLER] no enumerated disk matches kind=%d idx=%d\n", kind, index);
    return -4;
}

int64_t sys_user_create_pw(const char *u_username, const char *u_password,
                           uint32_t uid, uint32_t gid, const char *u_home) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (p->euid != 0) return -1;                  // root only, same as sys_adduser

    char username[USERNAME_MAX];
    char password[128];
    char home[SC_PATH_MAX];
    if (sc_bounce_str(u_username, username, sizeof(username)) != 0) return -1;
    if (sc_bounce_str(u_password, password, sizeof(password)) != 0) return -1;

    int64_t rc = -1;
    // Same username rules the first-boot path enforces, applied here too so the
    // two account-creation paths cannot disagree about what a valid name is.
    size_t ulen = strlen(username);
    if (ulen == 0 || ulen >= USERNAME_MAX) goto out;
    for (size_t i = 0; i < ulen; i++) {
        unsigned char c = (unsigned char)username[i];
        if (c <= ' ' || c >= 127 || c == ':') goto out;
    }
    if (strcmp(username, "root") == 0) goto out;  // reserved system account
    if (user_lookup_name(username)) goto out;     // already exists

    // Strength policy, BEFORE the account is created, so a refused password
    // leaves nothing behind and Ring 3 is told WHICH rule it broke. The old
    // check here was `password[0] == '\0'`, i.e. an empty password is not a
    // password, and that was the whole of it: this syscall would happily set
    // "1" on a new account.
    {
        int pc = users_password_check(username, password);
        if (pc != PW_OK) {
            bootlog_write("[USERS] create '%s' REFUSED: %s (policy code %d)",
                          username, pw_policy_message(pc), pc);
            rc = PW_RC(pc);
            goto out;
        }
    }

    // uid 0 means ALLOCATE. Callers used to compute their own (Settings used
    // 1000 + user_count, which collides the moment an account is deleted), so
    // the allocator lives on this side of the boundary where the table is.
    if (uid == 0) {
        extern uint32_t next_user_uid_rs(const uint32_t *uids, uint32_t n);
        int n = 0;
        user_entry_t *t = users_all(&n);
        uint32_t inuse[MAX_USERS];
        uint32_t k = 0;
        for (int i = 0; i < n && k < (uint32_t)MAX_USERS; i++)
            if (t[i].active) inuse[k++] = t[i].uid;
        uid = next_user_uid_rs(inuse, k);
        if (uid == 0xFFFFFFFFu) goto out;         // uid space exhausted
    }
    if (gid == 0) gid = uid;
    if (user_lookup_uid(uid)) goto out;

    {
        int have_home = (u_home && sc_bounce_str(u_home, home, sizeof(home)) == 0
                         && home[0]);
        if (!have_home) {
            // /HOME/<NAME8>, uppercased alnum, matching users_create_first_admin.
            int hp = 0; const char *pfx = "/HOME/";
            for (int i = 0; pfx[i] && hp < (int)sizeof(home) - 1; i++) home[hp++] = pfx[i];
            int base = hp;
            for (size_t i = 0; i < ulen && (int)i < 8 && hp < (int)sizeof(home) - 1; i++) {
                char c = username[i];
                if (c >= 'a' && c <= 'z') c -= 32;
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) home[hp++] = c;
            }
            home[hp] = '\0';
            if (hp <= base) goto out;             // no usable characters for a path
        }

        if (user_create(username, uid, gid, home, "/APPS/MSH", username) != 0) goto out;
        if (user_set_password(username, password) != 0) {
            user_delete(uid);                     // roll back; nothing is synced
            goto out;
        }
        // Prove the account we just created can actually authenticate BEFORE
        // persisting it. This is cheap and it is the assertion whose absence is
        // the entire bug: the old path "succeeded" while producing an account
        // that could never log in, and nothing noticed for two releases.
        if (!users_can_authenticate(username)) {
            user_delete(uid);
            bootlog_write("[USERS] create '%s' ROLLED BACK: account would not authenticate (#745)",
                          username);
            goto out;
        }

        if (g_fat_fs.mounted) fat_mkdir(&g_fat_fs, "/HOME");
        fat_mkdir(&g_fat_fs, home);
        perms_set(home, uid, gid, 0750);
        users_make_home_skeleton(home, uid, gid);

        if (users_sync() != 0)
            kprintf("[USERS] sync failed after user create\n");
        bootlog_write("[USERS] created '%s' uid=%u gid=%u home='%s' WITH a password (#745)",
                      username, (unsigned)uid, (unsigned)gid, home);
        rc = (int64_t)uid;
    }

out:
    // Scrub the plaintext. crypto_zero is the compiler-barrier version, so it
    // is not optimised away the way a plain memset of a dead local can be.
    { extern void crypto_zero(void *ptr, size_t length);
      crypto_zero(password, sizeof(password)); }
    return rc;
}

// (#745) See syscall.h. A PURE predicate over data the caller already holds:
// it reveals nothing about any account, only whether a candidate string would
// be accepted. Deliberately NOT root-gated, because every user needs it to
// choose their own password. It does let Ring 3 probe the breached-password
// table one guess at a time; that table is a public 50k word list that ships
// inside this very image, so there is nothing there to leak.
int64_t sys_pw_check(const char *u_username, const char *u_password) {
    char username[USERNAME_MAX];
    char password[128];

    if (!u_password) return -14;
    if (sc_bounce_str(u_password, password, sizeof(password)) != 0) return -14;
    username[0] = '\0';
    if (u_username && sc_bounce_str(u_username, username, sizeof(username)) != 0) return -14;

    // The SAME call user_set_password() makes, not a re-implementation of it,
    // so a candidate this accepts cannot then be refused by the set path.
    int pc = users_password_check(username[0] ? username : NULL, password);

    { extern void crypto_zero(void *ptr, size_t length);
      crypto_zero(password, sizeof(password)); }
    return (int64_t)pc;
}

// (#745) FIRST-BOOT PROVISIONING WITH A SEPARATE ROOT PASSWORD.
//
// WHY THIS EXISTS AND SYS_USER_CREATE_PW WAS NOT ENOUGH. The setup wizard runs
// on an image that ALREADY has a root account, carrying the shipped default
// credential from the asset base. SYS_USER_CREATE_PW explicitly refuses the
// name "root" (it is a reserved system account), so the wizard could create the
// human account and had no way at all to replace root's shipped password. The
// machine finished setup with a fresh, policy-checked desktop account sitting
// next to a uid 0 whose password was a published default.
//
// The two rules that matter (both passwords pass the FULL policy, and they are
// not the same string) are enforced HERE, kernel side, before anything is
// written. They cannot be enforced by two separate syscalls, because a caller
// could simply not make the second one.
//
// The euid==0 gate is the real boundary and it is worth being honest about what
// it does and does not buy: the caller IS root already, so it can change root's
// password by other means. This call is not a privilege boundary, it is the
// only path that changes BOTH accounts atomically under one validation.
int64_t sys_firstboot_admin(const char *u_username, const char *u_user_pw,
                            const char *u_root_pw) {
    process_t *p = proc_current();
    if (!p) return -1;
    if (p->euid != 0) return -1;

    char username[USERNAME_MAX];
    char user_pw[128];
    char root_pw[128];
    int64_t rc = -14;
    if (sc_bounce_str(u_username, username, sizeof(username)) != 0) goto out;
    if (sc_bounce_str(u_user_pw,  user_pw,  sizeof(user_pw))  != 0) goto out;
    if (sc_bounce_str(u_root_pw,  root_pw,  sizeof(root_pw))  != 0) goto out;

    // BOTH passwords, BEFORE anything is created or changed. One shared rule
    // (proc/users.c) with the kernel first-boot screen, so the two paths cannot
    // disagree about what is acceptable.
    rc = (int64_t)users_check_first_boot_pair(username, user_pw, root_pw);
    if (rc != 0) {
        bootlog_write("[USERS] first-boot provisioning REFUSED for '%s' (code %d)",
                      username, (int)rc);
        goto out;
    }

    if (!user_lookup_uid(0)) {
        // A genuinely virgin account database: one function owns the whole job,
        // creating root AND the human account.
        rc = (int64_t)users_create_first_admin(username, user_pw, root_pw);
        if (rc == 0) {
            user_entry_t *u = user_lookup_name(username);
            rc = u ? (int64_t)u->uid : -1;
        }
        goto out;
    }

    // The shipped-image path. root already exists; take ownership of it, then
    // create the human account. Ordering matters: sys_user_create_pw() is what
    // calls users_sync(), so root's new hash and the new account reach the disk
    // in ONE save. If the account creation fails, root's password is put back
    // exactly as it was and nothing was synced, so the machine is unchanged and
    // setup can be retried.
    if (users_root_pw_begin(root_pw) != 0) { rc = -1; goto out; }
    rc = sys_user_create_pw(u_username, u_user_pw, 0, 0, NULL);
    if (rc < 0) {
        users_root_pw_rollback();
        bootlog_write("[USERS] first-boot: account creation failed (%d); root password ROLLED BACK (#745)",
                      (int)rc);
        goto out;
    }
    users_root_pw_commit();
    // #745: the account the wizard just called an administrator joins the admin
    // set, so it can actually be offered the elevation prompt. On a SHIPPED
    // image this is not the same thing as FIRST_ADMIN_UID: measured on a
    // throwaway VM, the golden already holds `admin` at uid 1000, so the
    // wizard's account landed at 1001 and a uid-literal admin set would have
    // refused it while the wizard told the user otherwise.
    (void)users_grant_admin((uint32_t)rc);
    IGNORE_RESULT("#745: the account and root's password were already synced by\n                  sys_user_create_pw above; this second save only persists the new\n                  admin GROUP. If it fails the machine still has a working account\n                  and a working root password, and failing setup here would be a\n                  worse outcome than an admin set that reverts to the FIRST_ADMIN_UID\n                  fallback until the next successful save.",
                  users_sync());
    bootlog_write("[USERS] first-boot: account '%s' uid=%d created; root given its OWN password (#745)",
                  username, (int)rc);

out:
    { extern void crypto_zero(void *ptr, size_t length);
      crypto_zero(user_pw, sizeof(user_pw));
      crypto_zero(root_pw, sizeof(root_pw)); }
    return rc;
}

int64_t sys_set_theme(int theme_id) {
    extern void theme_set(int theme_id);
    extern void wm_invalidate_all(void);
    theme_set(theme_id);
    wm_invalidate_all();
    return 0;
}

int64_t sys_get_theme(void) {
    extern int theme_get_current_id(void);
    return (int64_t)theme_get_current_id();
}

int64_t sys_set_volume(int volume) {
    extern int audio_set_master_volume(int volume);
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    audio_set_master_volume(volume);
    return 0;
}

int64_t sys_get_volume(void) {
    extern int audio_get_volume(void *vol);
    // audio_volume_t has master_left as first int field; read it directly
    int buf[16];
    if (audio_get_volume(buf) == 0) {
        int v = buf[0]; // master_left
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        return (int64_t)v;
    }
    return 0;
}

int64_t sys_set_mute(int mute_flag) {
    extern int audio_mute(int mute);
    audio_mute(mute_flag != 0);
    return 0;
}

int64_t sys_get_disk_total(void) {
    extern fat_fs_t g_fat_fs;
    uint64_t bytes = (uint64_t)g_fat_fs.cluster_count
                     * g_fat_fs.sectors_per_cluster
                     * g_fat_fs.bytes_per_sector;
    return (int64_t)(bytes / (1024 * 1024));
}

int64_t sys_get_disk_free(void) {
    extern fat_fs_t g_fat_fs;
    extern uint32_t fat_get_free_clusters(fat_fs_t *fs);
    uint32_t free_clusters = fat_get_free_clusters(&g_fat_fs);
    uint64_t bytes = (uint64_t)free_clusters
                     * g_fat_fs.sectors_per_cluster
                     * g_fat_fs.bytes_per_sector;
    return (int64_t)(bytes / (1024 * 1024));
}

int64_t sys_set_mouse_speed(int speed) {
    extern void mouse_set_sensitivity(int s);
    mouse_set_sensitivity(speed);
    return 0;
}

int64_t sys_get_mouse_speed(void) {
    extern int mouse_get_sensitivity(void);
    return (int64_t)mouse_get_sensitivity();
}

int64_t sys_get_rtc_time(void) {
    extern void rtc_read_time(int *hour, int *minute, int *second);
    int h = 0, m = 0, s = 0;
    rtc_read_time(&h, &m, &s);
    return (int64_t)((h << 16) | (m << 8) | s);
}

int64_t sys_get_rtc_date(void) {
    extern void rtc_read_date(int *day, int *month, int *year, int *weekday);
    int d = 1, mo = 1, y = 2026, wd = 0;
    rtc_read_date(&d, &mo, &y, &wd);
    // Pack: year in upper 16 bits, month in next 8, day in lowest 8
    return (int64_t)(((int64_t)y << 16) | ((mo & 0xFF) << 8) | (d & 0xFF));
}

// ============================================================================
// RTC write helpers (set time / set date)
// ============================================================================

static void rtc_write_reg(uint8_t reg, uint8_t val) {
    outb(0x70, reg);
    outb(0x71, val);
}

static uint8_t bin_to_bcd(uint8_t v) {
    return (uint8_t)(((v / 10) << 4) | (v % 10));
}

int64_t sys_set_rtc_time(uint64_t packed) {
    int h = (int)((packed >> 16) & 0xFF);
    int m = (int)((packed >>  8) & 0xFF);
    int s = (int)(packed & 0xFF);
    // Poll until RTC not updating
    outb(0x70, 0x0A);
    while (inb(0x71) & 0x80) { outb(0x70, 0x0A); }
    rtc_write_reg(0x00, bin_to_bcd((uint8_t)s));
    rtc_write_reg(0x02, bin_to_bcd((uint8_t)m));
    rtc_write_reg(0x04, bin_to_bcd((uint8_t)h));
    return 0;
}

int64_t sys_set_rtc_date(uint64_t packed) {
    int y  = (int)((packed >> 16) & 0xFFFF);
    int mo = (int)((packed >>  8) & 0xFF);
    int d  = (int)(packed & 0xFF);
    outb(0x70, 0x0A);
    while (inb(0x71) & 0x80) { outb(0x70, 0x0A); }
    rtc_write_reg(0x07, bin_to_bcd((uint8_t)d));
    rtc_write_reg(0x08, bin_to_bcd((uint8_t)mo));
    rtc_write_reg(0x09, bin_to_bcd((uint8_t)(y % 100)));
    return 0;
}

// ============================================================================
// Network info syscall
// ============================================================================

static void ip_to_str(const uint8_t *ip, char *out) {
    // Format 4 bytes as A.B.C.D
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t v = ip[i];
        if (v >= 100) { out[pos++] = '0' + v / 100; }
        if (v >= 10)  { out[pos++] = '0' + (v / 10) % 10; }
        out[pos++] = '0' + v % 10;
        if (i < 3) out[pos++] = '.';
    }
    out[pos] = '\0';
}

static void mac_to_str(const uint8_t *mac, char *out) {
    // Format 6 bytes as XX:XX:XX:XX:XX:XX
    const char *hex = "0123456789ABCDEF";
    int pos = 0;
    for (int i = 0; i < 6; i++) {
        out[pos++] = hex[(mac[i] >> 4) & 0xF];
        out[pos++] = hex[mac[i] & 0xF];
        if (i < 5) out[pos++] = ':';
    }
    out[pos] = '\0';
}

int64_t sys_get_net_info(void *buf, uint64_t len) {
    if (!buf || len < sizeof(net_info_t)) return -1;
    net_info_t kni;   // #567: fill kernel-local, then one fault-safe copy_to_user
    net_info_t *ni = &kni;
    // Report the IP layer's ACTUAL configured address, not the DHCP-offered one.
    // The kernel currently boots with a static IP (ip_set_address), so the DHCP
    // module's offered IP stays 0 and the panel wrongly showed "Disconnected"
    // even though networking is up. DHCP also routes through ip_set_address, so
    // this is correct for both static and DHCP configurations.
    extern uint32_t ip_get_address(void);
    extern uint32_t ip_get_gateway(void);
    extern uint32_t ip_get_netmask(void);
    // ip_to_str() reads the 4 address bytes in memory order, so the values
    // must be in network byte order; the IP layer stores them in host order.
    #define NETINFO_BSWAP(v) ((((v) & 0xFFu) << 24) | (((v) & 0xFF00u) << 8) | \
                              (((v) >> 8) & 0xFF00u) | (((v) >> 24) & 0xFFu))
    uint32_t ip  = NETINFO_BSWAP(ip_get_address());
    uint32_t gw  = NETINFO_BSWAP(ip_get_gateway());
    uint32_t nm  = NETINFO_BSWAP(ip_get_netmask());
    uint32_t dnsv = dhcp_get_dns();
    if (dnsv == 0) dnsv = ip_get_gateway();  // static config: show the gateway
    uint32_t dns = NETINFO_BSWAP(dnsv);
    #undef NETINFO_BSWAP
    uint8_t  mac[6];
    nic_get_mac(mac);
    ip_to_str((uint8_t *)&ip,  ni->ip);
    ip_to_str((uint8_t *)&gw,  ni->gateway);
    ip_to_str((uint8_t *)&nm,  ni->netmask);
    ip_to_str((uint8_t *)&dns, ni->dns);
    mac_to_str(mac, ni->mac);
    ni->connected = (ip != 0) ? 1 : 0;
    if (copy_to_user(buf, &kni, sizeof(kni)) != 0) return -14;
    return 0;
}

// Parse a dotted-quad "a.b.c.d" into the IP layer's stored representation
// ((a<<24)|(b<<16)|(c<<8)|d), the inverse of what sys_get_net_info reports.
static uint32_t settings_parse_ip(const char *s) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int pi = 0, val = 0;
    const char *p = s;
    for (;; p++) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (*p - '0');
        } else if (*p == '.' || *p == '\0') {
            if (pi < 4) parts[pi] = (uint32_t)(val & 0xFF);
            pi++; val = 0;
            if (*p == '\0') break;
        } else {
            break;
        }
    }
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

int64_t sys_net_set_static(const char *ip, const char *mask, const char *gw) {
    extern void ip_set_address(uint32_t);
    extern void ip_set_netmask(uint32_t);
    extern void ip_set_gateway(uint32_t);
    if (!ip || !mask || !gw) return -1;
    ip_set_address(settings_parse_ip(ip));
    ip_set_netmask(settings_parse_ip(mask));
    ip_set_gateway(settings_parse_ip(gw));
    { extern void net_clear_fault(void); net_clear_fault(); }   // #549: manual reconfigure = recovery
    kprintf("[NET] Static config applied: ip=%s mask=%s gw=%s\n", ip, mask, gw);
    return 0;
}

int64_t sys_get_disk_info(int idx, void *buf) {
    if (!buf || idx < 0 || idx > 3) return -1;
    extern int ata_drive_present(int);
    extern int ata_drive_type(int);
    extern int ata_drive_smart(int);
    extern unsigned long ata_drive_sectors(int);
    extern const char *ata_drive_model(int);
    extern const char *ata_drive_serial(int);
    // #567: fill a kernel-local struct, then one fault-safe copy_to_user.
    sc_disk_info_t kd;
    sc_disk_info_t *d = &kd;
    memset(&kd, 0, sizeof(kd));
    if (!ata_drive_present(idx)) return -1;
    d->present = 1;
    d->type    = (uint8_t)ata_drive_type(idx);
    d->smart   = (int8_t)ata_drive_smart(idx);
    d->size_mb = (uint32_t)((ata_drive_sectors(idx) * 512UL) / (1024UL * 1024UL));
    const char *m = ata_drive_model(idx);
    int k; for (k = 0; k < 40 && m[k]; k++) d->model[k] = m[k];
    d->model[k] = 0;
    const char *sr = ata_drive_serial(idx);
    for (k = 0; k < 20 && sr[k]; k++) d->serial[k] = sr[k];
    d->serial[k] = 0;
    if (copy_to_user(buf, &kd, sizeof(kd)) != 0) return -14;
    return 0;
}

// Play a sound file (MP3/WAV/...) by path, asynchronously (kernel thread).
// #745: THE THIRD ONE. #700 found the shape "Ring 3 names any path and the
// kernel opens and parses it in Ring 0" and fixed it in two places, B3
// (sys_print_image) and B7 (sys_theme_load_file). This syscall is the same
// shape and was missed, so it kept the property both of those were fixed for:
// a uid-1000 caller could hand /CONFIG/SSHHOST.KEY to the audio stack and have
// Ring 0 read a root-owned 0600 file it may not open.
//
// The decoder behind this is the largest single block of vendored C in the
// kernel (libmad, tremor, faad2, opus), so "the current parser probably does
// not leak anything back" is not an argument worth making. The check goes on
// the READ, where it stays correct no matter what the parser later becomes.
//
// Same access class as its two siblings: R_OK on the file being read.
int64_t sys_play_wav(const char *upath) {
    if (!upath) return -1;
    // #567: bounce the user path fault-safe into the kernel buffer.
    char path[128];
    if (sc_bounce_str(upath, path, sizeof(path)) != 0) return -1;
    {
        process_t *p = proc_current();
        if (p && p->privilege == PRIV_USER &&
            perms_check(path, p->euid, p->egid, R_OK) != 0) {
            kprintf("[AUDIO] play refused: uid %u may not read %s (#745)\n",
                    (unsigned)p->euid, path);
            return -13;   // EACCES
        }
    }
    extern int audio_play_file_async(const char *path);
    return audio_play_file_async(path);
}

// ============================================================================
// Ring-3 PCM push (Phase 1 of the Ring-0 media-decode exit).
//
// These are deliberately thin: every check (format/rate/channel validation,
// user-pointer bounds, stream ownership by pid) and all #426 wait-queue
// blocking lives in drivers/audio_pcm.c next to the state it guards, rather
// than being smeared across the dispatcher. See drivers/audio_pcm.h.
// ============================================================================

int64_t sys_audio_pcm_open(uint32_t rate, uint32_t channels, uint32_t format) {
    return audio_pcm_open(rate, channels, format);
}

int64_t sys_audio_pcm_write(int handle, const void *pcm, uint32_t frames) {
    return audio_pcm_write(handle, pcm, frames);
}

int64_t sys_audio_pcm_close(int handle) {
    return audio_pcm_close(handle);
}

// ============================================================================
// NTP sync syscall
// ============================================================================

// #797: THE NTP CLIENT MOVED. What used to sit here - ntp_udp_cb(), a private
// unix_days_to_ymd(), and a hand-rolled net_poll()+proc_sleep() loop - is now
// net/sntp.c (transport) plus rustkern/sntp.rs (validation, in Rust per the
// 2026-07-16 rule). The reasons are recorded at the top of net/sntp.c; the short
// version is that the code here hardcoded one server, could not be pointed at
// another, validated NOTHING about the reply before setting the clock, and would
// have decoded a 2036-or-later timestamp as a date around 1900.
//
// syscall 147 keeps its number, its signature and its 0/-1 contract, so the
// existing Settings date/time caller is unchanged. It is now simply the
// "use the default server" spelling of syscall 367.
int64_t sys_ntp_sync(void) {
    return (sntp_sync(NULL, 0, NULL) == SNTP_OK) ? 0 : -1;
}

// #797 SYS_NTP_SYNC_SERVER (367): sync against a CALLER-NAMED server, which is
// what the first-boot wizard's "NTP server" field needs and what syscall 147
// could never express.
//
// `userver` is a Ring-3 pointer, so it goes through sc_bounce_str() ->
// strncpy_from_user() -> validate_user_string() before it is read, and syscall
// 367 also carries a descriptor in rustkern/argtab.rs so the central entry check
// runs on it too. NULL means "the default server".
//
// Unlike 147 this returns the FULL status, not 0/-1: on failure the caller gets
// the negative SNTP_E_* code, so a wizard can distinguish "that host never
// answered" from "that host answered with a packet we refused to trust".
int64_t sys_ntp_sync_server(const char *userver, uint32_t timeout_ms) {
    char server[128];
    if (userver) {
        if (sc_bounce_str(userver, server, sizeof(server)) != 0) return -14;
    } else {
        server[0] = '\0';
    }
    return (int64_t)sntp_sync(server[0] ? server : NULL, timeout_ms, NULL);
}

// #dosverify: trusted-kernel-caller twin of win16_launch() (defined above,
// near line 316). win16_launch() exists to be reached from a SYSCALL, so it
// treats `upath` as a userland pointer and bounces it through sc_bounce_str()
// -> strncpy_from_user() -> validate_user_string(), which (correctly, since
// #500/MAYTERA-SEC-2026-0016) demands the live U/S page-table bit on every
// page touched, not just a low address. The WIN16PM.RUN boot autolauncher
// (win16_autolaunch_thread, exec/win16api.c) is KERNEL code calling
// win16_launch() with a KERNEL-stack buffer: that buffer is never U/S=user,
// so validate_user_string() correctly rejects it, sc_bounce_str() returns
// -14, and win16_launch() returns -1 before ever calling proc_create() --
// silently, because that call site never checked the return value. Net
// effect: WIN16PM.RUN has launched nothing since whichever build first
// carried the #500 hardening (a test VM's own kernel, built one day earlier,
// still had a working WIN16PM.RUN launch of Word6). That is a real,
// user-visible regression in an unrelated subsystem, found while verifying
// the #dosverify DOS-layer change did not itself break Win16 -- it did not,
// but this pre-existing break meant Word6 could not be run at all to check.
//
// The correct fix is not to weaken validate_user_string() (that would reopen
// the #500/#487 hole for every real syscall caller); it is to give
// kernel-internal callers a path that never pretends `path` is a user
// pointer in the first place, exactly like dos_launch() already does not
// bounce through a user-validated helper. `kpath` must be a NUL-terminated
// kernel string the caller owns for the duration of this call. g_win16_busy/
// g_win16_path/win16_proc_entry are the same file-static state win16_launch()
// above uses; appended here (end of file) rather than beside win16_launch()
// so every pre-existing smap-uaccess.manifest line number in this file stays
// byte-for-byte valid (#645 lint is line-pinned; inserting mid-file shifts
// every entry below it and turns 48 unrelated KNOWN_GAP lines into false
// "site moved" failures for a change that touches none of them).
int win16_launch_kernel(const char *kpath) {
    if (g_win16_busy) return -1;
    if (!kpath || !kpath[0]) return -1;
    int i = 0;
    for (; i < (int)sizeof(g_win16_path) - 1 && kpath[i]; i++) g_win16_path[i] = kpath[i];
    g_win16_path[i] = '\0';
    if (!g_win16_path[0]) return -1;
    // #708: no Ring-3 caller here (this is the /CONFIG/WIN16PM.RUN boot
    // harness), so the guest runs as the authenticated desktop session. If
    // nobody has logged in yet the identity is unresolvable and the launch is
    // REFUSED, rather than silently running the guest as root.
    if (guestfs_arm_session(GUESTFS_SLOT_WIN16) != 0) {
        kprintf("[win16] kernel launch of '%s' REFUSED: no usable identity for "
                "the guest\n", g_win16_path);
        return -1;
    }
    g_win16_busy = 1;
    if (proc_create("win16", win16_proc_entry, NULL, PRIO_NORMAL) < 0) {
        g_win16_busy = 0;
        guestfs_disarm_rs(GUESTFS_SLOT_WIN16);
        return -1;
    }
    return 0;
}


// #745 login-gate backdrop continuity. `g_wallpaper_idx` above is the LIVE
// wallpaper ordinal: the compositor's frame loop force-syncs its displayed
// wallpaper to it every frame (userland/apps/compositor/main.c, the
// SYS_GET_WALLPAPER poll), so this value IS what is on screen for whatever
// session is running, and nothing clears it on logout. The kernel login gate
// reads it when it is re-entered by Switch User / Log Out so the backdrop does
// not visibly change out from under the user. Read-only on purpose: the write
// path stays SYS_SET_WALLPAPER and nothing else.
//
// Appended at end of file deliberately: kernel/proc/syscall.c is line-anchored
// by the smap-uaccess manifest, and an insertion anywhere above shifts every
// anchor after it.
int syscall_get_wallpaper_idx(void) { return g_wallpaper_idx; }
