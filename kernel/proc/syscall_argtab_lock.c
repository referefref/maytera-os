// syscall_argtab_lock.c - #503 / MAYTERA-SEC-2026-0016
//
// The Rust argument table (rustkern.rs) hardcodes the byte size of every fixed
// -length user buffer the dispatcher validates. Rust cannot see a C struct, so
// those numbers are duplicated constants, and a duplicated constant that drifts
// is not a cosmetic problem here: if a struct GROWS and the table does not, the
// validator proves fewer bytes writable than the kernel actually writes, and
// the tail of every one of those writes goes unchecked. That is a hole created
// by a routine refactor in an unrelated file.
//
// So the numbers are locked. This translation unit contains no code: it exists
// only so that changing any validated struct FAILS THE BUILD, loudly, in the
// file that tells you exactly what to do about it. It is the same _Static_assert
// sizeof-lock pattern the rest of the Rust FFI in this tree uses.
//
// If you are here because a _Static_assert below fired: you changed a struct
// that crosses the syscall boundary. Update the matching SZ_* constant in
// rustkern.rs (the #503 argtab section) to the new size, and check whether the
// struct's new tail needs validating differently. Do NOT just bump the number
// without reading the handler.

#include "../types.h"
#include "cron.h"
#include "process.h"
#include "procinfo.h"
#include "../gui/fb_syscall.h"
#include "../gui/window.h"
#include "../devinfo.h"
#include "../net/tcp.h"
#include "../net/ipp.h"
#include "../security/validate.h"
#include "syscall.h"
#include "syscall_argtab.h"

// --- Fixed-size user buffers named by the table (SZ_* in rustkern.rs) -------
// Values are compiler ground truth: nm -S on a probe TU built with the real
// kernel CFLAGS, not hand-computed padding.
_Static_assert(sizeof(devinfo_sysinfo_t) == 160,
               "#503 argtab: SZ_DEVINFO_SYSINFO in rustkern.rs is stale");
_Static_assert(sizeof(devinfo_pci_t) == 76,
               "#503 argtab: SZ_DEVINFO_PCI in rustkern.rs is stale");
_Static_assert(sizeof(devinfo_usb_t) == 16,
               "#503 argtab: SZ_DEVINFO_USB in rustkern.rs is stale");
_Static_assert(sizeof(devinfo_irq_t) == 16,
               "#503 argtab: SZ_DEVINFO_IRQ in rustkern.rs is stale");
_Static_assert(sizeof(proc_info_t) == 64,
               "#503 argtab: SZ_PROC_INFO in rustkern.rs is stale");
_Static_assert(sizeof(wm_window_info_t) == 96,
               "#503 argtab: SZ_WM_WINDOW_INFO in rustkern.rs is stale");
_Static_assert(sizeof(cron_job_t) == 128,
               "#503 argtab: SZ_CRON_JOB in rustkern.rs is stale");
_Static_assert(sizeof(fb_info_user_t) == 24,
               "#503 argtab: SZ_FB_INFO_USER in rustkern.rs is stale");
_Static_assert(sizeof(key_event_t) == 24,
               "#503 argtab: SZ_KEY_EVENT in rustkern.rs is stale");

// --- #503 batch 2: procinfo rows (SYS_PROC_HANDLES / _DETAIL, SYS_NET_CONNS,
// SYS_SVC_LIST). procinfo.h asserts these at their definitions too; the point
// of re-asserting HERE is that this is the file naming the table that would
// silently under-validate their tails.
_Static_assert(sizeof(handle_info_t) == 112,
               "#503 argtab: SZ_HANDLE_INFO in rustkern.rs is stale");
_Static_assert(sizeof(svc_info_t) == 80,
               "#503 argtab: SZ_SVC_INFO in rustkern.rs is stale");
_Static_assert(sizeof(proc_detail_t) == 112,
               "#503 argtab: SZ_PROC_DETAIL in rustkern.rs is stale");
_Static_assert(sizeof(tcp_conn_info_t) == 24,
               "#503 argtab: SZ_TCP_CONN_INFO in rustkern.rs is stale");

// The row caps the handlers clamp to. The table validates min(argN, cap)
// elements to match the handler EXACTLY (see Len::ElemsClamped). If a cap grows
// here and not in the table, the validator would prove fewer rows writable than
// the handler writes: the tail would go unchecked. Build fails instead.
_Static_assert(PI_NAME_MAX == 32, "#503 argtab: PI_NAME_MAX in rustkern.rs is stale");
_Static_assert(TM_PI_MAX_CONNS == 64, "#503 argtab: CAP_CONNS in rustkern.rs is stale");

// --- #503 batch 3 ----------------------------------------------------------
_Static_assert(sizeof(gui_event_t) == 32,
               "#503 argtab: SZ_GUI_EVENT in rustkern.rs is stale");
_Static_assert(sizeof(net_info_t) == 88,
               "#503 argtab: SZ_NET_INFO in rustkern.rs is stale");
_Static_assert(sizeof(printer_cfg_t) == 172,
               "#503 argtab: SZ_PRINTER_CFG in rustkern.rs is stale");
_Static_assert(PRINT_MAX_PRINTERS == 8,
               "#503 argtab: CAP_PRINTERS in rustkern.rs is stale");
// PI_MAX_HANDLES (64) and PI_MAX_SVCS (32) are private to procinfo.c, so they
// are re-stated and asserted at their point of use there, not here.

// The 32-bit-compat pointer shim (SANITIZE_USER_PTR in syscall.c) is mirrored
// by sanitize_user_ptr() in rustkern.rs. If the C prefix ever changes and the
// Rust does not, the validator would prove the WRONG ADDRESS for the three
// handlers that use it, and validation would pass while the handler touched an
// address nobody checked.
_Static_assert(USER_PTR_SX_PREFIX == 0xFFFFFFFF00000000ULL,
               "#503 argtab: sanitize_user_ptr() in rustkern.rs mirrors this prefix");

// sc_user_info_t, k_stat_t and dirent_t are private to proc/syscall.c and are
// locked there, at their definitions, where a reader changing them will see it.

// --- Access-flag encoding the Rust table mirrors ----------------------------
// The table asks for ACCESS_RW_USER / ACCESS_READ_USER by numeric value across
// the FFI. If these defines are ever renumbered, Rust would keep sending the old
// bits and silently ask for the wrong access (e.g. stop demanding the W bit on
// write buffers, which is the #232-shaped failure: a check that still runs and
// still passes but no longer checks the thing).
_Static_assert(ACCESS_READ == (1 << 0), "#503 argtab: ACCESS_READ renumbered");
_Static_assert(ACCESS_WRITE == (1 << 1), "#503 argtab: ACCESS_WRITE renumbered");
_Static_assert(ACCESS_USER == (1 << 3), "#503 argtab: ACCESS_USER renumbered");
_Static_assert(ACCESS_READ_USER == 0x9, "#503 argtab: ACCESS_READ_USER changed");
_Static_assert(ACCESS_RW_USER == 0xB, "#503 argtab: ACCESS_RW_USER changed");

// VALIDATE_OK must stay 0: Rust compares the return against 0.
_Static_assert(VALIDATE_OK == 0, "#503 argtab: VALIDATE_OK must be 0");
