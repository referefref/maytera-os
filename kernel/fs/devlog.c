// devlog.c - #388 comprehensive boot-time device inventory
//
// Real-hardware diagnostic. On a physical machine the user has no serial and no
// SSH, so we need a complete, self-describing inventory of what the firmware
// handed us and what the drivers found. /BOOTLOG.TXT captures the running
// commentary; this is the structured snapshot: CPU identity, the firmware
// framebuffer, the UEFI memory map, ACPI tables, the storage picture (which
// block devices exist and which one became root), every PCI function, every
// xHCI root port, every enumerated USB device with its full descriptor tree,
// every USB hub with its downstream-port status, and optionally the HD Audio
// codec identity.
//
// TWO CALLERS, ONE BUILDER. devlog_build() renders into the static buffer and
// touches no filesystem; devlog_dump() builds then writes /DEVLOG.TXT. The
// split exists because on a machine where storage never comes up there is
// nothing to write to and the SCREEN is the only channel: that path renders
// the same bytes rather than growing a second, divergent builder.
//
// ORDERING IS DELIBERATE. Identity and summary first, verbose descriptor trees
// last, so a truncated read (buffer full, a partial file, or a screen that only
// fits the first page) still answers "what machine is this and what did we
// find" before it starts spending bytes on endpoint descriptors.
//
// WHY THIS FILE IS C AND NOT RUST (the standing Rust-first policy needs a
// stated reason): it is glue. Essentially every line reads an existing C API
// (pci.h, xhci.h, hda.h, ata.h, ahci.h, acpi.h, usb_msc.h, blockdev.h,
// boot_info.h) and formats it with the kernel's own vsnprintf. A Rust port
// would be an FFI shim over ten C headers plus a #[repr(C)] mirror of every
// struct it reads, which is more unsafe surface than the C it replaced, for a
// module that computes nothing. The two places that DO carry real off-by-one
// risk are bounded by construction and noted at their sites: the ACPI table
// walk (every dereference is gated by dl_phys_ok()) and the buffer appender
// (which reserves a tail so the truncation notice can always be written).
#include "devlog.h"
#include "../serial.h"
#include "../string.h"
#include "../version.h"
#include "../boot_info.h"
#include "../cpu/sse.h"
#include "../drivers/pci.h"
#include "../drivers/xhci.h"
#include "../drivers/hda.h"
#include "../drivers/ata.h"
#include "../drivers/ahci.h"
#include "../drivers/acpi.h"
#include "../drivers/usb_msc.h"
#include "blockdev.h"
#include "ext2.h"
#include <stdarg.h>

#define DEVLOG_PATH "/DEVLOG.TXT"

// 128 KB (was 64 KB). A full PCI function list plus a 256-entry memory map plus
// a dozen USB devices with complete configuration descriptors measured out at
// roughly 70 KB of worst case, which 64 KB does not cover; the memory map alone
// is ~22 KB. This is .bss on a machine with hundreds of MB, and the buffer is
// also what gets painted on screen when nothing mounts, so running out of room
// costs exactly the evidence we came for.
#define DEVLOG_CAP  (128 * 1024)

// Reserved tail. dl_puts() refuses to encroach on it, so the "TRUNCATED"
// notice can always be appended. Before this, the notice was written with the
// same appender it was reporting on, so on a genuinely full buffer the notice
// itself was dropped and a truncated inventory looked complete.
#define DEVLOG_TAIL 128

// Print caps. Everything is still COUNTED past the cap and the count reported;
// only the per-entry listing stops.
#define DEVLOG_MMAP_PRINT_MAX  512u
#define DEVLOG_ACPI_TABLE_MAX  64u

static char     g_devlog[DEVLOG_CAP];
static uint32_t g_devlog_len;
static int      g_devlog_full;

// #388/#427: HD Audio section is OPT-IN. See devlog.h for why.
static int      g_devlog_include_hda = 0;

void devlog_set_include_hda(int enable) { g_devlog_include_hda = enable ? 1 : 0; }
int  devlog_get_include_hda(void)       { return g_devlog_include_hda; }

// ---------------------------------------------------------------------------
// Buffer appenders
// ---------------------------------------------------------------------------

// Unreserved append, used only for the truncation notice.
static void dl_raw(const char *s) {
    if (!s) return;
    uint32_t n = (uint32_t)strlen(s);
    uint32_t room = (g_devlog_len < DEVLOG_CAP - 1u) ? (DEVLOG_CAP - 1u - g_devlog_len) : 0u;
    if (n > room) n = room;
    if (n == 0) return;
    memcpy(g_devlog + g_devlog_len, s, n);
    g_devlog_len += n;
}

static void dl_puts(const char *s) {
    // STICKY: once the buffer has overflowed, stop appending entirely. Letting
    // short strings through after a long one was dropped leaves a jumbled tail
    // of orphaned newlines and half-sections that reads like corruption; a
    // clean cut followed by the TRUNCATED notice reads like what it is.
    if (!s || g_devlog_full) return;
    uint32_t n = (uint32_t)strlen(s);
    if (g_devlog_len + n >= DEVLOG_CAP - DEVLOG_TAIL) { g_devlog_full = 1; return; }
    memcpy(g_devlog + g_devlog_len, s, n);
    g_devlog_len += n;
}

// printf-style line append (adds no newline; callers include one). Formatting
// is the shared kernel vsnprintf; this only appends.
//
// TAGGED format(printf,1,2) ON PURPOSE. This file is one long series of format
// strings with 1 to 10 arguments each, written for a boot that may only happen
// once on hardware nobody has. A miscounted argument would not be a wrong
// number on the page: the kernel's kvformat consumes an argument per
// conversion, so one missing argument garbles every field after it in that
// line. The attribute makes gcc count them at build time; it is clean under
// the kernel's own -Wall -Wextra -Werror (and under -Wformat=2), so it costs
// nothing and makes every future edit self-checking.
__attribute__((format(printf, 1, 2)))
static void dl_line(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((uint32_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    dl_puts(buf);
    // MEASURED, not theoretical: a multi-line explanatory block written as one
    // dl_line() overran the old 320-byte buffer and vsnprintf cut it mid-word
    // with NO indication, so the file read as if the sentence simply ended.
    // Long literal blocks are now dl_puts() (unbounded), and a line that still
    // fills this buffer says so instead of lying quietly.
    if ((uint32_t)n >= sizeof(buf) - 1u)
        dl_puts("\n  [DEVLOG: PREVIOUS LINE TRUNCATED AT THE dl_line BUFFER]\n");
}

// Callback handed to hda_devlog_scan(): append one codec line.
static void dl_emit_line(const char *line) {
    dl_puts("  ");
    dl_puts(line ? line : "");
    dl_puts("\n");
}

// Replace anything that is not printable ASCII. Firmware strings are not
// trustworthy and this buffer is also painted on a framebuffer.
static void dl_ascii(char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c > 0x7e) *s = '.';
    }
}

// Copy a fixed-width, possibly unterminated firmware/SCSI field into a
// NUL-terminated, printable, right-trimmed C string. dst must hold n+1 bytes.
static void dl_field(char *dst, const void *src, uint32_t n) {
    memcpy(dst, src, n);
    dst[n] = 0;
    // Stop at the first NUL first: some firmware fields are NUL-padded rather
    // than space-padded, and the right-trim below must run against the real
    // string length, not the field width.
    uint32_t l = 0;
    while (l < n && dst[l]) l++;
    dst[l] = 0;
    dl_ascii(dst);
    while (l > 0 && dst[l - 1] == ' ') dst[--l] = 0;
}

typedef struct { uint32_t mask; const char *name; } dl_bit_t;

// Emit "label: NAME NAME NAME" for every set bit in the table. One appender,
// no hand-rolled string concatenation.
static void dl_bits(const char *label, uint32_t v, const dl_bit_t *t, unsigned n) {
    dl_puts("    ");
    dl_puts(label);
    dl_puts(":");
    int any = 0;
    for (unsigned i = 0; i < n; i++) {
        if (v & t[i].mask) { dl_puts(" "); dl_puts(t[i].name); any = 1; }
    }
    if (!any) dl_puts(" (none of the decoded set)");
    dl_puts("\n");
}

// ---------------------------------------------------------------------------
// Physical-address gate for the ACPI walk
// ---------------------------------------------------------------------------
//
// The kernel is UEFI identity-mapped (physical == virtual), but only the low
// 4 GiB is safe to assume, and a firmware table pointer is just a number the
// firmware wrote. acpi_find_table() dereferences EVERY RSDT/XSDT entry before
// validating it, which is fine for acpi_init() (it stops at the FADT) but would
// have this diagnostic fault on the first bad pointer on an unknown machine.
// So we walk the tables ourselves and gate every dereference on a range that
// (a) fits under the identity-map assumption and (b) lies inside some
// memory-map entry. A table we will not read is REPORTED as unread, which is
// itself a useful line, rather than being read and taking the machine down.
#define DL_PHYS_LIMIT 0x100000000ull

static int dl_phys_ok(uint64_t addr, uint64_t len) {
    if (addr == 0 || len == 0) return 0;
    if (len > DL_PHYS_LIMIT) return 0;
    if (addr > DL_PHYS_LIMIT - len) return 0;   // no overflow, and end <= limit

    if (!g_boot_info) return 0;
    uint32_t count = g_boot_info->memory_map_entries;
    uint64_t map   = g_boot_info->memory_map_address;
    if (count == 0 || map == 0) return 1;       // no map to check against

    uint32_t esz = g_boot_info->memory_map_entry_size;
    if (esz < (uint32_t)sizeof(memory_map_entry_t)) esz = (uint32_t)sizeof(memory_map_entry_t);
    if (count > 4096u) count = 4096u;           // paranoia: bound the scan

    const uint8_t *p = (const uint8_t *)(uintptr_t)map;
    for (uint32_t i = 0; i < count; i++) {
        memory_map_entry_t e;
        memcpy(&e, p + (uint64_t)i * esz, sizeof(e));
        if (e.length == 0 || len > e.length) continue;
        // len <= e.length is checked FIRST: (e.length - len) is unsigned and
        // would wrap if it were evaluated on a too-long range, which would let
        // an out-of-region address pass.
        if (addr >= e.base && (addr - e.base) <= (e.length - len)) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// CPU identity (CPUID)
// ---------------------------------------------------------------------------
//
// cpuid() is the shared helper in kernel/types.h; it hardwires the input ECX to
// 0, which covers every leaf below except CPUID.0Bh subleaf 1. That subleaf is
// therefore not read, and the omission is stated in the output rather than
// papered over: cores-per-package is reported from CPUID.4h:EAX instead.

static char     g_cpu_vendor[13];
static char     g_cpu_brand[49];
static int      g_cpu_brand_off;
static uint32_t g_cpu_max_leaf, g_cpu_max_ext;
static uint32_t g_cpu1_eax, g_cpu1_ebx, g_cpu1_ecx, g_cpu1_edx;
static int      g_cpu_probed;

// cpu/sse.c keeps its xgetbv helper file-static, so there is no shared accessor
// to reuse. XGETBV faults unless CR4.OSXSAVE is set; every caller below gates
// on that. g_fpu_xcr0 (cpu/sse.h) is what the kernel PROGRAMMED, which is not
// the same question as what the register currently holds, so both are reported.
static uint64_t dl_xgetbv0(void) {
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return ((uint64_t)hi << 32) | lo;
}

static void dl_cpu_probe(void) {
    uint32_t a, b, c, d;

    cpuid(0, &a, &b, &c, &d);
    g_cpu_max_leaf = a;
    {
        uint32_t v[3];
        v[0] = b; v[1] = d; v[2] = c;           // EBX, EDX, ECX
        memcpy(g_cpu_vendor, v, 12);
        g_cpu_vendor[12] = 0;
        dl_ascii(g_cpu_vendor);
    }

    cpuid(1, &g_cpu1_eax, &g_cpu1_ebx, &g_cpu1_ecx, &g_cpu1_edx);

    cpuid(0x80000000u, &a, &b, &c, &d);
    g_cpu_max_ext = a;

    g_cpu_brand[0]  = 0;
    g_cpu_brand_off = 0;
    if (g_cpu_max_ext >= 0x80000004u) {
        uint32_t r[12];
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002u + i, &a, &b, &c, &d);
            r[i * 4 + 0] = a; r[i * 4 + 1] = b; r[i * 4 + 2] = c; r[i * 4 + 3] = d;
        }
        memcpy(g_cpu_brand, r, 48);
        g_cpu_brand[48] = 0;
        dl_ascii(g_cpu_brand);
        int s = 0;
        while (g_cpu_brand[s] == ' ') s++;
        int e = (int)strlen(g_cpu_brand);
        while (e > s && g_cpu_brand[e - 1] == ' ') e--;
        g_cpu_brand[e]  = 0;
        g_cpu_brand_off = s;
    }
    g_cpu_probed = 1;
}

static const char *dl_cpu_brand_str(void) {
    if (!g_cpu_probed) dl_cpu_probe();
    return g_cpu_brand[g_cpu_brand_off] ? (g_cpu_brand + g_cpu_brand_off)
                                        : "(no CPUID brand string)";
}

static const dl_bit_t k_leaf1_ecx[] = {
    { 1u <<  0, "SSE3"       }, { 1u <<  1, "PCLMULQDQ" }, { 1u <<  3, "MONITOR"  },
    { 1u <<  5, "VMX"        }, { 1u <<  9, "SSSE3"     }, { 1u << 12, "FMA"      },
    { 1u << 13, "CMPXCHG16B" }, { 1u << 17, "PCID"      }, { 1u << 19, "SSE4.1"   },
    { 1u << 20, "SSE4.2"     }, { 1u << 21, "x2APIC"    }, { 1u << 22, "MOVBE"    },
    { 1u << 23, "POPCNT"     }, { 1u << 24, "TSC-DL"    }, { 1u << 25, "AES-NI"   },
    { 1u << 26, "XSAVE"      }, { 1u << 27, "OSXSAVE"   }, { 1u << 28, "AVX"      },
    { 1u << 29, "F16C"       }, { 1u << 30, "RDRAND"    }, { 1u << 31, "HYPERVISOR" },
};

static const dl_bit_t k_leaf1_edx[] = {
    { 1u <<  0, "FPU"   }, { 1u <<  4, "TSC"    }, { 1u <<  5, "MSR"   },
    { 1u <<  6, "PAE"   }, { 1u <<  8, "CX8"    }, { 1u <<  9, "APIC"  },
    { 1u << 11, "SEP"   }, { 1u << 12, "MTRR"   }, { 1u << 13, "PGE"   },
    { 1u << 15, "CMOV"  }, { 1u << 19, "CLFSH"  }, { 1u << 23, "MMX"   },
    { 1u << 24, "FXSR"  }, { 1u << 25, "SSE"    }, { 1u << 26, "SSE2"  },
    { 1u << 28, "HTT"   },
};

static const dl_bit_t k_leaf7_ebx[] = {
    { 1u <<  0, "FSGSBASE" }, { 1u <<  2, "SGX"      }, { 1u <<  3, "BMI1"    },
    { 1u <<  4, "HLE"      }, { 1u <<  5, "AVX2"     }, { 1u <<  7, "SMEP"    },
    { 1u <<  8, "BMI2"     }, { 1u <<  9, "ERMS"     }, { 1u << 10, "INVPCID" },
    { 1u << 11, "RTM"      }, { 1u << 16, "AVX512F"  }, { 1u << 17, "AVX512DQ" },
    { 1u << 18, "RDSEED"   }, { 1u << 19, "ADX"      }, { 1u << 20, "SMAP"    },
    { 1u << 23, "CLFLUSHOPT" }, { 1u << 24, "CLWB"   }, { 1u << 28, "AVX512CD" },
    { 1u << 29, "SHA"      }, { 1u << 30, "AVX512BW" }, { 1u << 31, "AVX512VL" },
};

static const dl_bit_t k_leaf7_ecx[] = {
    { 1u <<  2, "UMIP"  }, { 1u <<  3, "PKU"    }, { 1u <<  4, "OSPKE"  },
    { 1u <<  7, "CET-SS" }, { 1u << 22, "RDPID" },
};

static const dl_bit_t k_leaf7_edx[] = {
    { 1u <<  4, "FSRM"    }, { 1u << 10, "MD_CLEAR" }, { 1u << 26, "IBRS/IBPB" },
    { 1u << 27, "STIBP"   }, { 1u << 28, "L1D_FLUSH" }, { 1u << 29, "ARCH_CAP" },
    { 1u << 31, "SSBD"    },
};

static void dl_dump_cpu(void) {
    if (!g_cpu_probed) dl_cpu_probe();

    uint32_t base_family = (g_cpu1_eax >> 8)  & 0xF;
    uint32_t base_model  = (g_cpu1_eax >> 4)  & 0xF;
    uint32_t stepping    =  g_cpu1_eax        & 0xF;
    uint32_t ext_family  = (g_cpu1_eax >> 20) & 0xFF;
    uint32_t ext_model   = (g_cpu1_eax >> 16) & 0xF;
    uint32_t family = (base_family == 0xF) ? (base_family + ext_family) : base_family;
    uint32_t model  = (base_family == 0x6 || base_family == 0xF)
                    ? (base_model + (ext_model << 4)) : base_model;

    dl_line("=== CPU IDENTITY (CPUID) ===\n");
    dl_line("  vendor: \"%s\"   max standard leaf 0x%08x   max extended leaf 0x%08x\n",
            g_cpu_vendor, (unsigned)g_cpu_max_leaf, (unsigned)g_cpu_max_ext);
    dl_line("  brand : \"%s\"\n", dl_cpu_brand_str());
    dl_line("  leaf 1 EAX=0x%08x  family %u (0x%x)  model %u (0x%02x)  stepping %u"
            "  [base family %u model %u, ext family %u model %u]\n",
            (unsigned)g_cpu1_eax, (unsigned)family, (unsigned)family,
            (unsigned)model, (unsigned)model, (unsigned)stepping,
            (unsigned)base_family, (unsigned)base_model,
            (unsigned)ext_family, (unsigned)ext_model);
    dl_line("  leaf 1 EBX=0x%08x  brand-index=%u  CLFLUSH-line=%u bytes"
            "  max-logical-per-package=%u  initial-APIC-id=%u\n",
            (unsigned)g_cpu1_ebx, (unsigned)(g_cpu1_ebx & 0xFF),
            (unsigned)(((g_cpu1_ebx >> 8) & 0xFF) * 8),
            (unsigned)((g_cpu1_ebx >> 16) & 0xFF),
            (unsigned)((g_cpu1_ebx >> 24) & 0xFF));
    dl_line("  leaf 1 ECX=0x%08x  EDX=0x%08x\n",
            (unsigned)g_cpu1_ecx, (unsigned)g_cpu1_edx);
    dl_bits("leaf 1 ECX", g_cpu1_ecx, k_leaf1_ecx,
            sizeof(k_leaf1_ecx) / sizeof(k_leaf1_ecx[0]));
    dl_bits("leaf 1 EDX", g_cpu1_edx, k_leaf1_edx,
            sizeof(k_leaf1_edx) / sizeof(k_leaf1_edx[0]));
    dl_line("  RUNNING UNDER A HYPERVISOR: %s (leaf 1 ECX bit 31)\n",
            (g_cpu1_ecx & (1u << 31)) ? "YES" : "no");

    if (g_cpu_max_leaf >= 7) {
        uint32_t a, b, c, d;
        cpuid(7, &a, &b, &c, &d);       // subleaf 0 (the shared helper forces ECX=0)
        dl_line("  leaf 7.0 EAX=0x%08x (max subleaf) EBX=0x%08x ECX=0x%08x EDX=0x%08x\n",
                (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)d);
        dl_bits("leaf 7.0 EBX", b, k_leaf7_ebx, sizeof(k_leaf7_ebx) / sizeof(k_leaf7_ebx[0]));
        dl_bits("leaf 7.0 ECX", c, k_leaf7_ecx, sizeof(k_leaf7_ecx) / sizeof(k_leaf7_ecx[0]));
        dl_bits("leaf 7.0 EDX", d, k_leaf7_edx, sizeof(k_leaf7_edx) / sizeof(k_leaf7_edx[0]));
    } else {
        dl_line("  leaf 7 not available (max standard leaf < 7): no AVX2/AVX512/SMEP/SMAP report\n");
    }

    // XSAVE state. CR4.OSXSAVE gates XGETBV; reading it without that faults.
    {
        uint64_t cr4 = read_cr4();
        int osxsave = (cr4 & CR4_OSXSAVE) && (g_cpu1_ecx & CPUID_OSXSAVE);
        dl_line("  CR0=0x%llx CR4=0x%llx  OSFXSR=%d OSXMMEXCPT=%d OSXSAVE=%d\n",
                (unsigned long long)read_cr0(), (unsigned long long)cr4,
                (cr4 & CR4_OSFXSR) ? 1 : 0, (cr4 & CR4_OSXMMEXCPT) ? 1 : 0,
                (cr4 & CR4_OSXSAVE) ? 1 : 0);
        if (osxsave) {
            uint64_t xcr0 = dl_xgetbv0();
            dl_line("  XCR0 (live) = 0x%llx   x87=%d SSE=%d AVX=%d\n",
                    (unsigned long long)xcr0,
                    (xcr0 & XCR0_X87) ? 1 : 0,
                    (xcr0 & XCR0_SSE) ? 1 : 0,
                    (xcr0 & XCR0_AVX) ? 1 : 0);
        } else
            dl_line("  XCR0 (live) not read: CR4.OSXSAVE is clear, XGETBV would fault\n");
        dl_line("  kernel: g_fpu_xcr0=0x%llx g_fpu_use_xsave=%u"
                "  (what sse_init() programmed, not necessarily the live register)\n",
                (unsigned long long)g_fpu_xcr0, (unsigned)g_fpu_use_xsave);
    }

    // Topology. CPUID.0Bh subleaf 1 would give cores*threads directly, but the
    // shared cpuid() helper forces ECX=0, so it is not read here; CPUID.4h:EAX
    // gives cores-per-package for the same answer.
    if (g_cpu_max_leaf >= 0x0B) {
        uint32_t a, b, c, d;
        cpuid(0x0B, &a, &b, &c, &d);
        dl_line("  leaf 0xB.0 EAX=0x%08x EBX=0x%08x ECX=0x%08x EDX=0x%08x"
                "  (level type %u, %u logical processor(s) at this level, x2APIC id %u)\n",
                (unsigned)a, (unsigned)b, (unsigned)c, (unsigned)d,
                (unsigned)((c >> 8) & 0xFF), (unsigned)(b & 0xFFFF), (unsigned)d);
        dl_puts("  NOTE: leaf 0xB subleaf 1 (cores x threads) NOT read; kernel/types.h\n"
                "        cpuid() hardwires input ECX=0 and there is no subleaf helper.\n");
    } else {
        dl_line("  leaf 0xB not available (max standard leaf < 0xB)\n");
    }
    if (g_cpu_max_leaf >= 4) {
        uint32_t a, b, c, d;
        cpuid(4, &a, &b, &c, &d);       // subleaf 0 = first cache level
        if (a & 0x1F)
            dl_line("  leaf 4.0 EAX=0x%08x -> %u core(s) per package,"
                    " %u max-logical sharing this cache\n",
                    (unsigned)a, (unsigned)(((a >> 26) & 0x3F) + 1),
                    (unsigned)(((a >> 14) & 0xFFF) + 1));
        else
            dl_line("  leaf 4.0 reports no cache type (non-Intel topology encoding)\n");
    }
    dl_line("  THREAD COUNT (best available): max-logical-per-package %u from leaf 1 EBX\n",
            (unsigned)((g_cpu1_ebx >> 16) & 0xFF));
    dl_puts("\n");
}

// ---------------------------------------------------------------------------
// Firmware framebuffer
// ---------------------------------------------------------------------------

static const char *dl_pixfmt_name(uint32_t f) {
    switch (f) {
        case PIXEL_FORMAT_RGB:  return "RGB (8:8:8)";
        case PIXEL_FORMAT_BGR:  return "BGR (8:8:8)";
        case PIXEL_FORMAT_MASK: return "BITMASK";
        default:                return "UNKNOWN";
    }
}

static void dl_dump_framebuffer(void) {
    dl_line("=== FIRMWARE FRAMEBUFFER (UEFI GOP, via boot_info) ===\n");
    if (!g_boot_info) {
        dl_line("  g_boot_info is NULL: no framebuffer information at all\n\n");
        return;
    }
    uint64_t addr   = g_boot_info->framebuffer.address;
    uint32_t width  = g_boot_info->framebuffer.width;
    uint32_t height = g_boot_info->framebuffer.height;
    uint32_t pitch  = g_boot_info->framebuffer.pitch;
    uint32_t bpp    = g_boot_info->framebuffer.bpp;
    uint32_t pfmt   = g_boot_info->framebuffer.pixel_format;

    dl_line("  address=0x%llx  width=%u  height=%u  pitch=%u bytes  bpp=%u\n",
            (unsigned long long)addr, (unsigned)width, (unsigned)height,
            (unsigned)pitch, (unsigned)bpp);
    dl_line("  pixel_format=%u (%s)  masks: R=0x%08x G=0x%08x B=0x%08x reserved=0x%08x\n",
            (unsigned)pfmt, dl_pixfmt_name(pfmt),
            (unsigned)g_boot_info->framebuffer.red_mask,
            (unsigned)g_boot_info->framebuffer.green_mask,
            (unsigned)g_boot_info->framebuffer.blue_mask,
            (unsigned)g_boot_info->framebuffer.reserved_mask);
    dl_line("  display_rotation=%llu (0=none 1=90cw 2=180 3=270cw)\n",
            (unsigned long long)g_boot_info->display_rotation);
    dl_line("  derived: expected pitch (width*4) = %u, framebuffer size (pitch*height) = %llu bytes\n",
            (unsigned)(width * 4u), (unsigned long long)((uint64_t)pitch * height));

    // The bootloader hardcodes bpp=32 and pitch=PixelsPerScanLine*4, and the
    // whole kernel graphics stack assumes 32bpp packed pixels. A GOP mode that
    // is not 32bpp, or a scanline that is padded, renders garbage; on an
    // unknown machine that is exactly the failure we would otherwise be
    // guessing at from a photograph of a corrupted screen.
    if (bpp != 32) {
        dl_line("  !! WARNING: bpp is %u, NOT 32.\n", (unsigned)bpp);
        dl_puts("  !!          The bootloader hardcodes bpp=32 and every kernel drawing path\n"
                "  !!          assumes 32-bit packed pixels. Expect a garbled or blank display.\n");
    }
    if (pitch != width * 4u) {
        dl_line("  !! WARNING: pitch (%u) != width*4 (%u).\n",
                (unsigned)pitch, (unsigned)(width * 4u));
        dl_puts("  !!          The scanline is padded (or the mode is not 32bpp). Any code that\n"
                "  !!          strides by width*4 instead of pitch will shear the image.\n");
    }
    if (addr == 0 || width == 0 || height == 0)
        dl_puts("  !! WARNING: framebuffer address/geometry is zero. The firmware gave us no\n"
                "  !!          usable GOP mode; there is no screen to render diagnostics on.\n");
    dl_puts("\n");
}

// ---------------------------------------------------------------------------
// Memory map
// ---------------------------------------------------------------------------

static const char *dl_mem_type_name(uint32_t t) {
    switch (t) {
        case MEMORY_TYPE_USABLE:           return "usable";
        case MEMORY_TYPE_RESERVED:         return "reserved";
        case MEMORY_TYPE_ACPI_RECLAIMABLE: return "acpi-reclaim";
        case MEMORY_TYPE_ACPI_NVS:         return "acpi-nvs";
        case MEMORY_TYPE_BAD:              return "bad";
        case MEMORY_TYPE_BOOTLOADER:       return "bootloader";
        case MEMORY_TYPE_KERNEL:           return "kernel";
        case MEMORY_TYPE_FRAMEBUFFER:      return "framebuffer";
        default:                           return "other";
    }
}

static void dl_dump_memmap(void) {
    dl_line("=== MEMORY MAP ===\n");
    if (!g_boot_info) {
        dl_line("  g_boot_info is NULL: no memory map\n\n");
        return;
    }
    uint32_t count   = g_boot_info->memory_map_entries;
    uint64_t map     = g_boot_info->memory_map_address;
    uint32_t esz     = g_boot_info->memory_map_entry_size;
    uint64_t dropped = g_boot_info->memory_map_dropped;

    dl_line("  total_memory=%llu bytes (%llu MiB)\n",
            (unsigned long long)g_boot_info->total_memory,
            (unsigned long long)(g_boot_info->total_memory >> 20));
    dl_line("  memory_map_entries=%u  dropped=%llu  entry_size=%u  map at 0x%llx\n",
            (unsigned)count, (unsigned long long)dropped,
            (unsigned)esz, (unsigned long long)map);
    dl_line("  kernel: base=0x%llx virt=0x%llx size=%llu bytes\n",
            (unsigned long long)g_boot_info->kernel_physical_base,
            (unsigned long long)g_boot_info->kernel_virtual_base,
            (unsigned long long)g_boot_info->kernel_size);
    dl_line("  sizeof(memory_map_entry_t) = %u\n",
            (unsigned)sizeof(memory_map_entry_t));

    // THE TRUNCATION CHECK, now AUTHORITATIVE rather than inferred.
    // boot_info_t::memory_map_dropped is filled by the bootloader with the
    // number of UEFI descriptors it found but could not record because its
    // fixed array was full. This file used to infer truncation from "entries
    // equals a cap I happen to know about", which is wrong for a map that
    // legitimately lands exactly on the cap, and needed re-editing every time
    // either copy of the cap moved (the two copies had in fact already
    // diverged, 256 in boot_info.h vs 512 in the loader). The kernel no longer
    // needs to know what the cap is. Zero means nothing was dropped.
    if (dropped != 0) {
        dl_line("  !! MEMORY MAP IS TRUNCATED: the bootloader DROPPED %llu region(s) because its\n"
                "  !! fixed array was full (recorded %u, dropped %llu).\n",
                (unsigned long long)dropped, (unsigned)count,
                (unsigned long long)dropped);
        dl_puts("  !! total_memory and the usable regions listed below are INCOMPLETE, and\n"
                "  !! \"this machine has no memory at address X\" cannot be concluded from them.\n"
                "  !! Raise MAX_MEMORY_MAP_ENTRIES in uefi/bootloader.c and rebuild the loader.\n");
    }

    if (count == 0 || map == 0) {
        dl_line("  (no memory map entries)\n\n");
        return;
    }
    if (esz < (uint32_t)sizeof(memory_map_entry_t)) {
        dl_line("  !! entry_size (%u) is smaller than sizeof(memory_map_entry_t) (%u);"
                " striding by the struct size instead\n",
                (unsigned)esz, (unsigned)sizeof(memory_map_entry_t));
        esz = (uint32_t)sizeof(memory_map_entry_t);
    }

    dl_line("   idx  base                 length               type\n");
    const uint8_t *p = (const uint8_t *)(uintptr_t)map;
    uint64_t usable = 0;
    uint32_t usable_entries = 0;
    uint32_t scan = count;
    if (scan > 4096u) scan = 4096u;             // paranoia: bound the scan
    for (uint32_t i = 0; i < scan; i++) {
        memory_map_entry_t e;
        memcpy(&e, p + (uint64_t)i * esz, sizeof(e));
        if (e.type == MEMORY_TYPE_USABLE) { usable += e.length; usable_entries++; }
        if (i < DEVLOG_MMAP_PRINT_MAX)
            dl_line("  %4u  0x%016llx   0x%016llx   %u (%s)\n",
                    (unsigned)i, (unsigned long long)e.base,
                    (unsigned long long)e.length,
                    (unsigned)e.type, dl_mem_type_name(e.type));
    }
    if (scan > DEVLOG_MMAP_PRINT_MAX)
        dl_line("  ... %u further entr(ies) counted but not listed (print cap %u)\n",
                (unsigned)(scan - DEVLOG_MMAP_PRINT_MAX), (unsigned)DEVLOG_MMAP_PRINT_MAX);
    dl_line("  USABLE total: %llu bytes (%llu MiB) across %u entr(ies)\n",
            (unsigned long long)usable, (unsigned long long)(usable >> 20),
            (unsigned)usable_entries);
    dl_puts("\n");
}

// ---------------------------------------------------------------------------
// ACPI
// ---------------------------------------------------------------------------

static void dl_acpi_walk(const char *which, uint64_t table_pa, int is_xsdt) {
    if (!dl_phys_ok(table_pa, sizeof(acpi_sdt_header_t))) {
        dl_line("  %s at 0x%llx: NOT READ (outside the low-4GiB identity-map assumption,\n"
                "        or outside every memory-map region). Reported, not dereferenced.\n",
                which, (unsigned long long)table_pa);
        return;
    }
    const acpi_sdt_header_t *h = (const acpi_sdt_header_t *)(uintptr_t)table_pa;
    uint32_t len = h->length;
    if (len < (uint32_t)sizeof(acpi_sdt_header_t) || len > 0x10000u ||
        !dl_phys_ok(table_pa, len)) {
        dl_line("  %s at 0x%llx: implausible length %u; not walked\n",
                which, (unsigned long long)table_pa, (unsigned)len);
        return;
    }
    uint32_t entsz = is_xsdt ? 8u : 4u;
    uint32_t n = (len - (uint32_t)sizeof(acpi_sdt_header_t)) / entsz;
    dl_line("  %s at 0x%llx: length=%u, %u table pointer(s)\n",
            which, (unsigned long long)table_pa, (unsigned)len, (unsigned)n);

    const uint8_t *ents = (const uint8_t *)h + sizeof(acpi_sdt_header_t);
    uint32_t shown = 0;
    for (uint32_t i = 0; i < n && shown < DEVLOG_ACPI_TABLE_MAX; i++, shown++) {
        uint64_t pa = 0;
        if (is_xsdt) {
            uint64_t v; memcpy(&v, ents + (uint64_t)i * 8u, 8); pa = v;
        } else {
            uint32_t v; memcpy(&v, ents + (uint64_t)i * 4u, 4); pa = v;
        }
        if (!dl_phys_ok(pa, sizeof(acpi_sdt_header_t))) {
            dl_line("    [%2u] 0x%016llx  (NOT READ: outside the readable range)\n",
                    (unsigned)i, (unsigned long long)pa);
            continue;
        }
        const acpi_sdt_header_t *t = (const acpi_sdt_header_t *)(uintptr_t)pa;
        char sig[5], oem[7], otid[9];
        dl_field(sig,  t->signature,    4);
        dl_field(oem,  t->oem_id,       6);
        dl_field(otid, t->oem_table_id, 8);
        dl_line("    [%2u] %-4s 0x%016llx  len=%-7u rev=%u  oem=\"%s\" oem_table=\"%s\"\n",
                (unsigned)i, sig, (unsigned long long)pa,
                (unsigned)t->length, (unsigned)t->revision, oem, otid);
    }
    if (n > shown)
        dl_line("    ... %u further table pointer(s) not listed (print cap %u)\n",
                (unsigned)(n - shown), (unsigned)DEVLOG_ACPI_TABLE_MAX);
}

static void dl_dump_acpi(void) {
    dl_line("=== ACPI ===\n");
    uint64_t rsdp_pa = 0;
    uint32_t rsdp_ver = 0;
    if (g_boot_info) {
        rsdp_pa  = g_boot_info->acpi.rsdp_address;
        rsdp_ver = g_boot_info->acpi.rsdp_version;
    }
    dl_line("  boot_info: rsdp_address=0x%llx  rsdp_version=%u\n",
            (unsigned long long)rsdp_pa, (unsigned)rsdp_ver);
    dl_line("  driver:    acpi_is_initialized=%d  acpi_get_revision=%u\n",
            acpi_is_initialized() ? 1 : 0, (unsigned)acpi_get_revision());

    // Every dereference below is gated on the address falling inside a
    // memory-map region. If the map itself was truncated at the bootloader cap,
    // a perfectly valid ACPI region can be missing from it and its tables will
    // read as NOT READ. Say so, so that is not misread as bad firmware.
    if (g_boot_info && g_boot_info->memory_map_dropped != 0)
        dl_puts("  NOTE: the memory map is TRUNCATED (see MEMORY MAP). Table pointers are\n"
                "        validated against that map, so a table in a region the map dropped will\n"
                "        show as NOT READ here even though it is fine.\n");

    if (!dl_phys_ok(rsdp_pa, sizeof(acpi_rsdp_t))) {
        dl_puts("  RSDP not readable; no table list. (If rsdp_address is 0 the firmware gave\n"
                "  the bootloader no ACPI pointer at all, which is itself the finding.)\n\n");
        return;
    }
    const acpi_rsdp_t *rsdp = (const acpi_rsdp_t *)(uintptr_t)rsdp_pa;
    char sig[9], oem[7];
    dl_field(sig, rsdp->signature, 8);
    dl_field(oem, rsdp->oem_id,    6);
    dl_line("  RSDP: signature=\"%s\" revision=%u oem=\"%s\" rsdt=0x%08x\n",
            sig, (unsigned)rsdp->revision, oem, (unsigned)rsdp->rsdt_address);

    uint64_t xsdt_pa = 0;
    if (rsdp->revision >= 2 && dl_phys_ok(rsdp_pa, sizeof(acpi_rsdp_ext_t))) {
        const acpi_rsdp_ext_t *ext = (const acpi_rsdp_ext_t *)(uintptr_t)rsdp_pa;
        xsdt_pa = ext->xsdt_address;
        dl_line("  RSDP 2.0+: length=%u xsdt=0x%llx\n",
                (unsigned)ext->length, (unsigned long long)xsdt_pa);
    }

    // The kernel's acpi.c exposes no enumeration of discovered tables (only
    // acpi_find_table(signature), which dereferences every entry before
    // validating it). So we walk RSDT/XSDT here with every dereference gated by
    // dl_phys_ok(). Nothing below is invented: a table we cannot safely read is
    // printed as NOT READ.
    if (xsdt_pa) dl_acpi_walk("XSDT", xsdt_pa, 1);
    if (rsdp->rsdt_address) dl_acpi_walk("RSDT", (uint64_t)rsdp->rsdt_address, 0);
    if (!xsdt_pa && !rsdp->rsdt_address)
        dl_line("  RSDP names neither an XSDT nor an RSDT\n");
    dl_puts("  NOTE: DSDT and FACS are NOT RSDT/XSDT entries; they are reached through the\n"
            "        FADT, and acpi.c does not export their addresses, so they are absent\n"
            "        from the list above by construction rather than by absence.\n");
    dl_puts("\n");
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

extern fat_fs_t g_fat_fs;   // main.c; the single mounted FAT root

// Count PCI functions matching a class/subclass, for the "is there hardware we
// have no driver for" question.
static int dl_pci_class_count(uint8_t cls, uint8_t sub) {
    int n = pci_get_device_count(), hits = 0;
    for (int i = 0; i < n; i++) {
        pci_device_t *d = pci_get_device(i);
        if (d && d->class_code == cls && d->subclass == sub) hits++;
    }
    return hits;
}

static void dl_dump_storage(void) {
    dl_line("=== STORAGE ===\n");

    int usb_root = blk_root_is_usb();
    dl_line("  Root block routing: %s\n",
            usb_root ? "USB Mass Storage (blockdev.c g_root_usb)" : "ATA/IDE (blockdev.c default)");
    if (usb_root)
        dl_line("  Root USB MSC device index: %d\n", blk_root_usb_index());
    {
        uint64_t hits = 0, misses = 0;
        int mode = 0;
        blk_cache_stats(&hits, &misses, &mode);
        dl_line("  Root cache: mode=%d (0=off 1=TO-RAM 2=demand)  hits=%llu misses=%llu\n",
                mode, (unsigned long long)hits, (unsigned long long)misses);
    }
    dl_line("  ROOT FILESYSTEM MOUNTED: %s\n",
            g_fat_fs.mounted ? "YES (FAT root, g_fat_fs.mounted)" : "*** NO ***");
    if (g_fat_fs.mounted) {
        char label[13];
        dl_field(label, g_fat_fs.volume_label, 11);
        dl_line("    label=\"%s\" bytes_per_sector=%u sectors_per_cluster=%u"
                " total_sectors=%u cluster_count=%u\n",
                label, (unsigned)g_fat_fs.bytes_per_sector,
                (unsigned)g_fat_fs.sectors_per_cluster,
                (unsigned)g_fat_fs.total_sectors, (unsigned)g_fat_fs.cluster_count);
    }
    dl_line("  ext2 root cutover (g_root_ext2) = %d\n", g_root_ext2);

    // --- ATA / IDE ---
    dl_line("  --- ATA / IDE ---\n");
    {
        int first = -1;
        for (int idx = 0; idx < 4; idx++) {
            ata_drive_t *d = ata_get_drive(idx / 2, idx % 2);
            if (!d || !d->exists) {
                dl_line("    [%d] ch%d dr%d: absent\n", idx, idx / 2, idx % 2);
                continue;
            }
            if (first < 0 && d->type == ATA_TYPE_ATA) first = idx;
            dl_line("    [%d] ch%d dr%d: type=%s dma=%d udma=%u sectors=%llu (%llu MiB)"
                    " smart=%d\n",
                    idx, (int)d->channel, (int)d->drive,
                    (d->type == ATA_TYPE_ATA) ? "ATA" : "ATAPI",
                    (int)d->dma_capable, (unsigned)d->udma_mode,
                    (unsigned long long)d->sectors,
                    (unsigned long long)((d->sectors * 512ull) >> 20),
                    (int)d->smart_status);
            {
                char model[41], serial[21];
                dl_field(model,  d->model,  40);
                dl_field(serial, d->serial, 20);
                dl_line("        model=\"%s\" serial=\"%s\"\n", model, serial);
            }
        }
        if (first >= 0)
            dl_line("    first ATA drive: index %d (channel %d, drive %d)\n",
                    first, first / 2, first % 2);
        else
            dl_line("    first ATA drive: NONE (no ATA-type drive present)\n");
    }

    // --- AHCI / SATA ---
    dl_line("  --- AHCI / SATA ---\n");
    if (!ahci_is_initialized()) {
        dl_line("    AHCI not initialized (no controller claimed, or init declined)\n");
    } else {
        int np = ahci_get_port_count();
        dl_line("    present ports: %d (any device type)\n", np);
        int sata = 0;
        for (int i = 0; i < 32; i++) {
            int port = ahci_get_nth_sata_port(i);
            if (port < 0) break;                 // no more SATA disks
            sata++;
            uint64_t sect = ahci_get_sector_count(port);
            dl_line("    port %d: SATA disk, sectors=%llu (%llu MiB) model=\"%s\" serial=\"%s\"\n",
                    port, (unsigned long long)sect,
                    (unsigned long long)((sect * 512ull) >> 20),
                    ahci_get_model(port), ahci_get_serial(port));
        }
        if (sata == 0)
            dl_line("    no SATA disks (present ports may be ATAPI/SEMB/PM, or empty)\n");
    }

    // --- USB Mass Storage ---
    dl_line("  --- USB MASS STORAGE ---\n");
    {
        int nd = usb_msc_get_device_count();
        dl_line("    usb_msc_get_device_count() = %d (high-water slot count; freed slots stay\n"
                "    allocated and answer NULL, see #250)\n", nd);
        if (nd == 0) dl_line("    (no USB storage devices found)\n");
        for (int i = 0; i < nd; i++) {
            usb_msc_device_t *d = usb_msc_get_device(i);
            if (!d) { dl_line("    [%d] slot freed / not present\n", i); continue; }
            char dv[9], dp[17];
            dl_field(dv, d->vendor,  8);
            dl_field(dp, d->product, 16);
            dl_line("    [%d] slot_id=%d iface=%d %04x:%04x vendor=\"%s\" product=\"%s\"\n",
                    i, d->slot_id, d->interface_num,
                    (unsigned)d->vendor_id, (unsigned)d->product_id, dv, dp);
            dl_line("        ready=%d removable=%d block_size=%u blocks=%llu (%llu MiB)"
                    " max_lun=%u mounted=%d%s\n",
                    d->ready, d->removable, (unsigned)d->block_size,
                    (unsigned long long)d->num_blocks,
                    (unsigned long long)((d->num_blocks * (uint64_t)d->block_size) >> 20),
                    (unsigned)d->max_lun, d->mounted,
                    (usb_root && i == blk_root_usb_index()) ? "  <== ROOT" : "");
            dl_line("        bulk_in_ep=%d bulk_out_ep=%d mps_in=%d mps_out=%d\n",
                    d->bulk_in_ep, d->bulk_out_ep, d->max_packet_in, d->max_packet_out);
            for (int l = 0; l <= (int)d->max_lun && l < USB_MSC_MAX_LUNS; l++) {
                usb_msc_lun_t *u = &d->luns[l];
                char lv[9], lp[17], lr[5];
                dl_field(lv, u->vendor,   8);
                dl_field(lp, u->product,  16);
                dl_field(lr, u->revision, 4);
                dl_line("        lun %d: ready=%d block_size=%u blocks=%llu"
                        " vendor=\"%s\" product=\"%s\" rev=\"%s\"\n",
                        l, u->ready, (unsigned)u->block_size,
                        (unsigned long long)u->num_blocks, lv, lp, lr);
            }
        }
    }

    // --- Storage controllers we have NO driver for ---
    {
        int nvme = dl_pci_class_count(0x01, 0x08);
        int raid = dl_pci_class_count(0x01, 0x04);
        int sata = dl_pci_class_count(0x01, 0x06);
        int ide  = dl_pci_class_count(0x01, 0x01);
        dl_line("  --- STORAGE CONTROLLERS ON PCI ---\n");
        dl_line("    IDE (01:01)=%d  SATA/AHCI (01:06)=%d  RAID (01:04)=%d  NVMe (01:08)=%d\n",
                ide, sata, raid, nvme);
        if (nvme > 0) {
            dl_line("    !! %d NVMe controller(s) present and THIS KERNEL HAS NO NVMe DRIVER\n", nvme);
            dl_puts("    !! (drivers/ contains no nvme.c). On an NVMe-only machine the internal\n"
                    "    !! disk is invisible and USB is the only bootable/mountable medium.\n");
        }
        if (raid > 0) {
            dl_line("    !! %d controller(s) in RAID mode (class 01:04).\n", raid);
            dl_puts("    !! Firmware set to RST/RAID rather than AHCI hides the disk from the\n"
                    "    !! AHCI driver too.\n");
        }
    }
    dl_puts("\n");
}

// ---------------------------------------------------------------------------
// PCI / USB / HDA (pre-existing sections, unchanged in substance)
// ---------------------------------------------------------------------------

static const char *speed_short(int spd) {
    switch (spd) {
        case XHCI_SPEED_FULL:       return "FS(12M)";
        case XHCI_SPEED_LOW:        return "LS(1.5M)";
        case XHCI_SPEED_HIGH:       return "HS(480M)";
        case XHCI_SPEED_SUPER:      return "SS(5G)";
        case XHCI_SPEED_SUPER_PLUS: return "SS+(10G)";
        default:                    return "?";
    }
}

static const char *ep_type_name(uint8_t bmAttributes) {
    switch (bmAttributes & 0x03) {
        case 0: return "control";
        case 1: return "isoch";
        case 2: return "bulk";
        case 3: return "interrupt";
    }
    return "?";
}

// Best-effort human name for a USB (interface/device) class code.
static const char *usb_class_name(uint8_t cls) {
    switch (cls) {
        case 0x00: return "per-interface";
        case 0x01: return "Audio";
        case 0x02: return "CDC-Comm";
        case 0x03: return "HID";
        case 0x05: return "Physical";
        case 0x06: return "Image";
        case 0x07: return "Printer";
        case 0x08: return "Mass-Storage";
        case 0x09: return "Hub";
        case 0x0A: return "CDC-Data";
        case 0x0B: return "SmartCard";
        case 0x0E: return "Video";
        case 0xE0: return "Wireless";
        case 0xEF: return "Misc";
        case 0xFF: return "Vendor";
        default:   return "?";
    }
}

// Decode a HID interface protocol into a friendly tag.
static const char *hid_proto_name(uint8_t proto) {
    switch (proto) {
        case 0:  return "none";
        case 1:  return "keyboard";
        case 2:  return "mouse";
        default: return "?";
    }
}

// Walk a stored configuration descriptor and emit its interfaces + endpoints.
static void dl_dump_config(const uint8_t *cfg, int len) {
    if (len < 9) { dl_line("    (no config descriptor captured)\n"); return; }
    int total   = cfg[2] | (cfg[3] << 8);
    if (total > len) total = len;
    int nifaces = cfg[4];
    dl_line("    CONFIG: bNumInterfaces=%d wTotalLength=%d bmAttributes=0x%02x bMaxPower=%dmA\n",
            nifaces, total, cfg[7], cfg[8] * 2);
    int i = 9;
    while (i + 2 <= total) {
        int blen = cfg[i];
        int btype = cfg[i + 1];
        if (blen < 2 || i + blen > total) break;
        if (btype == 0x04 && blen >= 9) {          // INTERFACE
            uint8_t inum = cfg[i + 2], alt = cfg[i + 3], neps = cfg[i + 4];
            uint8_t icls = cfg[i + 5], isub = cfg[i + 6], iproto = cfg[i + 7];
            if (icls == 0x03) {
                dl_line("    IFACE %d alt %d: class 0x%02x(%s) sub 0x%02x proto 0x%02x(%s) nEP=%d\n",
                        inum, alt, icls, usb_class_name(icls), isub, iproto,
                        hid_proto_name(iproto), neps);
            } else {
                dl_line("    IFACE %d alt %d: class 0x%02x(%s) sub 0x%02x proto 0x%02x nEP=%d\n",
                        inum, alt, icls, usb_class_name(icls), isub, iproto, neps);
            }
        } else if (btype == 0x05 && blen >= 7) {   // ENDPOINT
            uint8_t eaddr = cfg[i + 2], eattr = cfg[i + 3];
            int emps = cfg[i + 4] | (cfg[i + 5] << 8);
            uint8_t eintv = cfg[i + 6];
            dl_line("      EP 0x%02x %s %s mps=%d interval=%d\n",
                    eaddr, (eaddr & 0x80) ? "IN " : "OUT",
                    ep_type_name(eattr), emps, eintv);
        } else if (btype == 0x21) {                // HID descriptor
            dl_line("      (HID descriptor, %d bytes)\n", blen);
        }
        i += blen;
    }
}

static void dl_dump_pci(void) {
    int n = pci_get_device_count();
    int dropped = pci_get_dropped_count();
    dl_line("=== PCI DEVICES (%d recorded, %d dropped) ===\n", n, dropped);
    // A non-zero drop count means this list is INCOMPLETE, so nothing below can
    // support "this machine has no such device". It must never be silent: on
    // unfamiliar hardware the missing function is exactly the one being looked
    // for. See pci_get_dropped_count() in drivers/pci.h.
    if (dropped > 0) {
        dl_line("  !! PCI INVENTORY IS INCOMPLETE: %d function(s) were found by the scan but\n"
                "  !! could not be recorded (MAX_PCI_DEVICES in drivers/pci.c is full).\n",
                dropped);
        dl_puts("  !! Absence from the list below is NOT evidence of absence from the machine.\n"
                "  !! Raise MAX_PCI_DEVICES and rebuild.\n");
    }
    dl_line("  B:D.F  vendor:device  class:sub:progif  IRQ  BAR0        claimed-by       description\n");
    for (int i = 0; i < n; i++) {
        pci_device_t *d = pci_get_device(i);
        if (!d) continue;
        uint64_t bar0 = pci_get_bar_address(d, 0);
        // #418: explicit claimed-by-driver column, so the next boot's
        // /DEVLOG.TXT answers "does this device even have a driver" with
        // certainty instead of an absence-of-evidence argument built from
        // grepping source for vendor strings.
        dl_line("  %02x:%02x.%x %04x:%04x    %02x:%02x:%02x        %3d  0x%08llx  %-16s %s\n",
                d->bus, d->slot, d->func, d->vendor_id, d->device_id,
                d->class_code, d->subclass, d->prog_if, d->interrupt_line,
                (unsigned long long)bar0,
                d->claimed ? d->claimed_by : "(none)",
                pci_get_class_name(d->class_code, d->subclass));
    }
    dl_puts("\n");
}

static void dl_dump_usb(void) {
    int nctrl = xhci_get_controller_count();
    dl_line("=== USB / xHCI (%d controller(s)) ===\n", nctrl);

    for (int c = 0; c < nctrl; c++) {
        xhci_controller_t *xhc = xhci_get_controller(c);
        if (!xhc) continue;
        dl_line("xHCI controller %d: %u root port(s), %u slots\n",
                c, xhc->max_ports, xhc->max_slots);
        for (int p = 0; p < (int)xhc->max_ports; p++) {
            int conn = 0, en = 0, spd = 0;
            if (!xhci_root_port_info(xhc, p, &conn, &en, &spd)) continue;
            dl_line("  root port %d: connected=%d enabled=%d port-speed=%d\n",
                    p + 1, conn, en, spd);
        }
    }
    dl_puts("\n");

    // Enumerated device tree (root-port + behind-hub devices, in enumerate order).
    int nd = xhci_get_enum_count();
    dl_line("--- ENUMERATED USB DEVICES (%d) ---\n", nd);
    if (nd == 0) dl_line("  (no USB devices enumerated)\n");
    for (int i = 0; i < nd; i++) {
        const xhci_enum_dev_t *e = xhci_get_enum_device(i);
        if (!e) continue;
        const uint8_t *dd = e->dev_desc;
        dl_line("[%s] slot %d root-port %d route 0x%05x depth %d speed %s%s\n",
                e->label[0] ? e->label : "?", e->slot_id, e->port + 1,
                (unsigned)e->route, e->depth, speed_short(e->speed),
                e->is_hub ? "  *HUB*" : "");
        dl_line("    DEVICE: %04x:%04x class 0x%02x(%s) sub 0x%02x proto 0x%02x "
                "bcdUSB %x.%02x mps0=%d numCfg=%d\n",
                e->vendor_id, e->product_id, e->dev_class,
                usb_class_name(e->dev_class), e->dev_subclass, e->dev_protocol,
                dd[3], dd[2], dd[7], dd[17]);
        dl_dump_config(e->cfg, e->cfg_len);
    }
    dl_puts("\n");

    // Hub inventory (bNbrPorts + downstream-port status, even for empty ports).
    int nh = xhci_get_hub_count();
    dl_line("--- USB HUBS (%d) ---\n", nh);
    if (nh == 0) dl_line("  (no USB hubs found)\n");
    for (int i = 0; i < nh; i++) {
        const xhci_hub_rec_t *h = xhci_get_hub_record(i);
        if (!h) continue;
        dl_line("HUB slot %d root-port %d route 0x%05x depth %d: %d downstream port(s) "
                "wHubCharacteristics=0x%04x\n",
                h->hub_slot, h->root_port + 1, (unsigned)h->route, h->depth,
                h->nports, h->hubchar);
        for (int p = 0; p < h->nports && p < 15; p++) {
            if (!h->ports[p].valid) {
                dl_line("    down-port %d: (not probed)\n", p + 1);
                continue;
            }
            dl_line("    down-port %d: status=0x%04x change=0x%04x connected=%d speed=%s\n",
                    p + 1, h->ports[p].status, h->ports[p].change,
                    h->ports[p].connected,
                    h->ports[p].connected ? speed_short(h->ports[p].speed) : "-");
        }
    }
    dl_puts("\n");
}

// THE HD AUDIO SECTION IS THE ONE THAT GOT THIS FILE UN-WIRED. It is OFF unless
// devlog_set_include_hda(1) is called. Evidence for the gate, read out of
// drivers/hda.c in this worktree rather than assumed:
//   - hda_codec_command() (hda.c:306) IS bounded now: it spins at most
//     g_hda_cmd_max_iters iterations of hda_delay(10). So it cannot loop
//     forever. That is NOT sufficient.
//   - The default cap is 2000, i.e. ~200ms of NON-YIELDING spin per timed-out
//     verb (hda.c:302 and the #71 comment above it, which names b730/b733 and
//     the real Cirrus CS4208 explicitly). hda_devlog_scan() issues that against
//     every widget of every codec of every controller, so a silent codec still
//     costs tens of seconds of BKL-held busy-wait: a freeze, not a hang.
//   - The short cap plus the timeout BUDGET that makes the scan self-limiting
//     (g_hda_cmd_max_iters=200, g_hda_diag_budget=60) is armed ONLY by
//     hda_audiolog_report() (hda.c:2840) and hda_audiolog_runtime_report()
//     (hda.c:3160). hda_devlog_scan() does NOT arm it, and the knobs are
//     file-static in hda.c so this file cannot arm them either.
//   - hda_devlog_scan() is also DESTRUCTIVE: it GCTL-resets every HDA
//     controller on the machine and reconfigures the live one afterwards
//     (hda.c:2749+). #173 is the bug where that left a stale `playing` flag
//     over a reset engine and silenced audio for the rest of the boot.
// A boot-time inventory must never be able to wedge the boot, so the default is
// OFF and the buffer says so out loud.
static void dl_dump_hda(void) {
    dl_line("=== HD AUDIO CODEC ===\n");
    if (!g_devlog_include_hda) {
        dl_puts("  SKIPPED (devlog_set_include_hda(0), the default).\n"
                "  WHY: hda_devlog_scan() GCTL-resets every HDA controller and then sweeps the\n"
                "  full codec widget graph at the DEFAULT codec-command spin cap of ~200ms per\n"
                "  timed-out verb. hda_codec_command() is bounded (it cannot loop forever), but\n"
                "  the short cap and the timeout budget that make the sweep self-limiting are\n"
                "  armed only by hda_audiolog_report(), not by hda_devlog_scan(), and those\n"
                "  knobs are file-static in drivers/hda.c. On a codec that does not answer\n"
                "  (the real iMac14,4 Cirrus CS4208) the sweep costs tens of seconds of\n"
                "  non-yielding spin. That is what wedged b730/b733 and got this whole file\n"
                "  un-wired from main.c in b734.\n"
                "  THIS IS NOT EVIDENCE OF NO AUDIO HARDWARE. See the PCI section for HDA\n"
                "  controllers (class 04:03); /AUDIOLOG.TXT is the bounded audio diagnostic.\n");
        dl_line("  HDA controllers on PCI (class 04:03): %d\n", dl_pci_class_count(0x04, 0x03));
        dl_puts("\n");
        return;
    }
    dl_puts("  INCLUDED BY EXPLICIT REQUEST (devlog_set_include_hda(1)). This resets every HDA\n"
            "  controller and can cost tens of seconds on a non-answering codec.\n");
    hda_devlog_scan(dl_emit_line);
    dl_puts("\n");
}

// ---------------------------------------------------------------------------
// Summary (first, so a truncated read still identifies the machine)
// ---------------------------------------------------------------------------

static void dl_dump_summary(void) {
    if (!g_cpu_probed) dl_cpu_probe();

    int usb_root = blk_root_is_usb();
    int msc_present = 0;
    int nmsc = usb_msc_get_device_count();
    for (int i = 0; i < nmsc; i++) if (usb_msc_get_device(i)) msc_present++;
    int nata = 0;
    for (int idx = 0; idx < 4; idx++) {
        ata_drive_t *d = ata_get_drive(idx / 2, idx % 2);
        if (d && d->exists) nata++;
    }

    dl_line("=== SUMMARY ===\n");
    dl_line("  CPU     : %s\n", dl_cpu_brand_str());
    dl_line("  CPU id  : vendor \"%s\"  hypervisor=%s\n",
            g_cpu_vendor, (g_cpu1_ecx & (1u << 31)) ? "YES" : "no");
    if (g_boot_info) {
        dl_line("  Memory  : %llu MiB total, %u memory-map entr(ies)%s\n",
                (unsigned long long)(g_boot_info->total_memory >> 20),
                (unsigned)g_boot_info->memory_map_entries,
                (g_boot_info->memory_map_dropped != 0)
                    ? "   <== MAP TRUNCATED, see MEMORY MAP" : "");
        dl_line("  Display : %ux%u pitch=%u bpp=%u @0x%llx%s\n",
                (unsigned)g_boot_info->framebuffer.width,
                (unsigned)g_boot_info->framebuffer.height,
                (unsigned)g_boot_info->framebuffer.pitch,
                (unsigned)g_boot_info->framebuffer.bpp,
                (unsigned long long)g_boot_info->framebuffer.address,
                (g_boot_info->framebuffer.bpp != 32 ||
                 g_boot_info->framebuffer.pitch != g_boot_info->framebuffer.width * 4u)
                    ? "   <== SEE FRAMEBUFFER WARNINGS" : "");
    } else {
        dl_line("  Memory  : g_boot_info is NULL\n");
        dl_line("  Display : g_boot_info is NULL\n");
    }
    dl_line("  Buses   : PCI functions=%d%s  xHCI controllers=%d  USB devices=%d  USB hubs=%d\n",
            pci_get_device_count(),
            (pci_get_dropped_count() > 0) ? " (INCOMPLETE, see PCI)" : "",
            xhci_get_controller_count(),
            xhci_get_enum_count(), xhci_get_hub_count());
    dl_line("  Storage : ATA drives=%d  AHCI=%s  USB-MSC present=%d  NVMe-on-PCI=%d (NO DRIVER)\n",
            nata, ahci_is_initialized() ? "up" : "down", msc_present,
            dl_pci_class_count(0x01, 0x08));
    dl_line("  Root    : routing=%s  FAT mounted=%s  ext2-root=%d\n",
            usb_root ? "USB-MSC" : "ATA",
            g_fat_fs.mounted ? "YES" : "*** NO ***", g_root_ext2);
    dl_line("  HDA sec : %s\n", g_devlog_include_hda ? "INCLUDED" : "skipped (default, see below)");
    dl_puts("\n");
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

const char *devlog_build(uint32_t *out_len) {
    g_devlog_len = 0;
    g_devlog_full = 0;

    dl_line("MayteraOS device inventory (/DEVLOG.TXT) - build %s v%s\n",
            MAYTERA_BUILD_DATE, MAYTERA_VERSION_STRING);
    dl_puts("Generated at boot, no filesystem required. See /BOOTLOG.TXT for the running\n"
            "enumeration log. Order is identity-first so a truncated read still identifies\n"
            "the machine; verbose descriptor trees are last.\n\n");

    // Identity and summary first.
    dl_dump_summary();
    dl_dump_cpu();
    dl_dump_framebuffer();
    dl_dump_memmap();
    dl_dump_acpi();
    dl_dump_storage();

    // Verbose trees last.
    dl_dump_pci();
    dl_dump_usb();
    dl_dump_hda();

    if (g_devlog_full)
        dl_raw("\n[DEVLOG] NOTE: inventory TRUNCATED (buffer full)\n");

    // NUL-terminate: the no-storage path renders this buffer as a C string.
    // dl_puts()'s reserve guarantees g_devlog_len < DEVLOG_CAP.
    g_devlog[g_devlog_len] = 0;

    if (out_len) *out_len = g_devlog_len;
    return g_devlog;
}

void devlog_dump(fat_fs_t *fs) {
    uint32_t len = 0;
    const char *buf = devlog_build(&len);

    // Build FIRST, then decide about writing. The inventory is the point; the
    // file is one of two ways to read it, and on a machine where nothing
    // mounted the caller still wants the buffer to paint on screen.
    if (!fs || !fs->mounted) {
        kprintf("[DEVLOG] filesystem not mounted; %s not written"
                " (%u bytes built, render it instead)\n",
                DEVLOG_PATH, (unsigned)len);
        return;
    }

    int r = fat_write_file(fs, DEVLOG_PATH, buf, len);
    kprintf("[DEVLOG] wrote %s (%u bytes, result %d): PCI=%d USB=%d HUBS=%d MSC=%d HDA=%s%s\n",
            DEVLOG_PATH, (unsigned)len, r,
            pci_get_device_count(), xhci_get_enum_count(), xhci_get_hub_count(),
            usb_msc_get_device_count(),
            g_devlog_include_hda ? "included" : "skipped",
            g_devlog_full ? " TRUNCATED" : "");
}
