// boot_info.h - Boot information structure passed from bootloader to kernel
#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include "types.h"

// Magic number to verify boot info integrity
#define BOOT_INFO_MAGIC 0x4D415954455241ULL  // "MAYTERA" in ASCII

// Memory types (compatible with UEFI memory map)
#define MEMORY_TYPE_USABLE           1
#define MEMORY_TYPE_RESERVED         2
#define MEMORY_TYPE_ACPI_RECLAIMABLE 3
#define MEMORY_TYPE_ACPI_NVS         4
#define MEMORY_TYPE_BAD              5
#define MEMORY_TYPE_BOOTLOADER       6
#define MEMORY_TYPE_KERNEL           7
#define MEMORY_TYPE_FRAMEBUFFER      8

// Maximum memory map entries
// DESCRIPTIVE ONLY. The UEFI bootloader owns the real cap (uefi/bootloader.c)
// and nothing in the kernel sizes an array by this, grep-verified. It is kept in
// step with the loader as documentation, but the AUTHORITATIVE answer to "was
// the map truncated" is boot_info.memory_map_dropped, which the loader measures.
#define MAX_MEMORY_MAP_ENTRIES 512

// Memory map entry structure
typedef struct {
    uint64_t base;      // Physical base address
    uint64_t length;    // Length in bytes
    uint32_t type;      // Memory type
    uint32_t attributes; // Additional attributes
} __attribute__((packed)) memory_map_entry_t;

// #QUIETBOOT: bits of boot_info_t.diag_flags. See the field's comment below.
#define BOOT_DIAG_SCREEN  0x1ULL   // on-screen boot diagnostics armed

// Framebuffer pixel format
#define PIXEL_FORMAT_RGB  0  // Red-Green-Blue (8 bits each)
#define PIXEL_FORMAT_BGR  1  // Blue-Green-Red (8 bits each)
#define PIXEL_FORMAT_MASK 2  // Bitmask format

// Framebuffer information
typedef struct {
    uint64_t address;       // Physical address of framebuffer
    uint32_t width;         // Width in pixels
    uint32_t height;        // Height in pixels
    uint32_t pitch;         // Bytes per scanline
    uint32_t bpp;           // Bits per pixel
    uint32_t pixel_format;  // Pixel format
    uint32_t red_mask;      // Red mask (for bitmask format)
    uint32_t green_mask;    // Green mask
    uint32_t blue_mask;     // Blue mask
    uint32_t reserved_mask; // Reserved mask
} __attribute__((packed)) framebuffer_info_t;

// ACPI information
typedef struct {
    uint64_t rsdp_address;  // Physical address of RSDP
    uint32_t rsdp_version;  // RSDP version (1 or 2)
    uint32_t reserved;
} __attribute__((packed)) acpi_info_t;

// Boot information structure
typedef struct {
    uint64_t magic;         // Magic number for verification

    // Memory information
    uint64_t memory_map_address;    // Physical address of memory map array
    uint32_t memory_map_entries;    // Number of entries in memory map
    uint32_t memory_map_entry_size; // Size of each entry
    uint64_t total_memory;          // Total usable memory in bytes

    // Framebuffer information
    framebuffer_info_t framebuffer;

    // ACPI information
    acpi_info_t acpi;

    // Kernel information
    uint64_t kernel_physical_base;  // Physical address where kernel is loaded
    uint64_t kernel_virtual_base;   // Virtual address of kernel
    uint64_t kernel_size;           // Size of kernel in bytes

    // #745 (local 102): display rotation, read by the UEFI bootloader from
    // \ROTATE.TXT on the ESP (FAT, root) BEFORE ExitBootServices, so it is
    // known to the kernel at the very first console_init()/fb_init() call -
    // before any FAT mount, before gfx_boot_splash(). 0=none 1=90cw 2=180
    // 3=270cw. Any other value is treated as 0 by the kernel (see fb_init()).
    // A NAMED field, not a reserved[] index: this struct has no shared header
    // between uefi/bootloader.c and the kernel (each keeps its own "matches
    // kernel/boot_info.h" copy by convention), so a name that greps is safer
    // than a silently-agreed index.
    uint64_t display_rotation;

    // #ASUSDIAG: how many UEFI memory-map descriptors the bootloader could NOT
    // record because g_memory_map[] was full. Before this the truncation was
    // reported only on the UEFI console, which nobody is watching on an
    // unattended boot, and the kernel had to INFER it from "entries equals a
    // cap I happen to know about" - an inference that is wrong for any map that
    // legitimately lands on exactly the cap, and that needs re-editing every
    // time either copy of the cap moves. Zero means nothing was dropped.
    //
    // A NAMED field, not a reserved[] index, for the same reason as
    // display_rotation above: this struct has no shared header between
    // uefi/bootloader.c and here (each keeps its own copy by convention), so a
    // name that greps is far safer than a silently-agreed index. It consumes
    // one reserved slot, so sizeof(boot_info_t) is UNCHANGED.
    uint64_t memory_map_dropped;

    // #QUIETBOOT: the boot-diagnostic arming word, read by the UEFI bootloader
    // from \boot\DIAG.TXT on the ESP (FAT, root) BEFORE ExitBootServices, so
    // it is known to the kernel at the FIRST executable statement of
    // kernel_main, before any filesystem mount and before early_fb_init()
    // decides whether to take the screen.
    //
    // Bit 0 (BOOT_DIAG_SCREEN) turns ON the on-screen boot diagnostics: the
    // early framebuffer page, the numbered stage list, the large stage ordinal,
    // the bottom banner, and the routing of stage checkpoints into the boot-log
    // console. ZERO, the value on every normal image, is a CLEAN QUIET BOOT.
    //
    // IT DOES NOT GATE ANY PERSISTENT LOG. /BOOTLOG.TXT, /DEVLOG.TXT,
    // /boot/STAGE.TXT, /boot/PANIC.TXT, the raw-LBA flight recorder and the
    // serial mirror are written on EVERY boot, armed or quiet. They are
    // invisible to the user and they are what makes the next unknown-machine
    // failure diagnosable, so they are not something to switch off.
    //
    // Nor does it gate the no-root-filesystem hardware report: that report can
    // only ever appear on a boot that has already failed to find a filesystem,
    // so it arms the screen on demand (early_fb_force_arm()) rather than being
    // silenced. A quiet boot that fails completely must still leave evidence.
    //
    // A NAMED field, not a reserved[] index, for the same reason as
    // display_rotation and memory_map_dropped above: this struct has no shared
    // header between uefi/bootloader.c and here (each keeps its own copy by
    // convention), so a name that greps is far safer than a silently-agreed
    // index. It consumes one reserved slot, so sizeof(boot_info_t) is UNCHANGED.
    uint64_t diag_flags;

    // Boot-time video mode. The UEFI loader enumerates every mode the firmware
    // offers (GOP Mode->MaxMode / QueryMode) and, if \boot\MODE.TXT names one,
    // applies it with SetMode before ExitBootServices. It ALWAYS fills these,
    // armed or not, so "no selection configured" and "the code never ran" stay
    // distinguishable in the kernel and not only on a UEFI console nobody is
    // watching on an unattended boot.
    //   video_mode_count    Mode->MaxMode: how many modes the firmware offers
    //   video_mode_current  the GOP mode number in effect at handover
    //   video_mode_status   MODESEL_* below
    //
    // READ THE ZERO CORRECTLY. A count of 0 means the BOOTLOADER predates this
    // field, NOT that the firmware offers no modes. That distinction is not
    // hypothetical: until #QUIETBOOT added the bootloader build step, every
    // golden shipped an 18-July BOOTX64.EFI from the asset base which left
    // display_rotation, memory_map_dropped and anything else added after it at
    // the SetMem zero. build/invariant-gate.sh now refuses an image whose ESP
    // loader is not the one the build produced, so on a gated image a zero here
    // is a genuine anomaly rather than the normal case.
    //
    // NAMED fields, not reserved[] indices, matching display_rotation,
    // memory_map_dropped and diag_flags above: this struct has no shared header
    // between uefi/bootloader.c and here (each keeps its own copy by
    // convention), so a name that greps is far safer than a silently-agreed
    // index. They consume three reserved slots, so sizeof(boot_info_t) is
    // UNCHANGED.
    uint64_t video_mode_count;
    uint64_t video_mode_current;
    uint64_t video_mode_status;

    // #ASUSMODE: the ENUMERATED GOP mode list, carried out of the loader as a
    // side blob and referenced from here by physical address.
    //
    // WHY THIS EXISTS. MEASURED: a user booted this OS from a USB stick on an
    // ASUS laptop with "1920x1080" in \boot\MODE.TXT and got a 3840x2160
    // framebuffer. The three fields above record only the OUTCOME (how many
    // modes, which one is live, MODESEL_*), so "gop_select_mode() refused the
    // request because the panel never offers 1920x1080" and "something else
    // happened" are INDISTINGUISHABLE from the kernel side. The loader does
    // Print() the whole enumerated list, but only to the UEFI console, which a
    // laptop user with no serial port cannot capture. The enumerated list is
    // the input needed to answer the question and it has never once been
    // captured from that hardware.
    //
    // WHY A BLOB AND NOT MORE FIELDS: the list is variable length (Mode->
    // MaxMode is whatever the firmware says), so it cannot live inside a struct
    // that uefi/bootloader.c and this header must keep byte-identical by hand.
    // The loader AllocatePool()s it as EfiLoaderData, which survives
    // ExitBootServices, and puts its physical address here. sizeof(boot_info_t)
    // is therefore UNCHANGED at 180 and every existing field offset is unmoved:
    // these two slots are the former reserved[2], at the same offsets (164 and
    // 172).
    //
    //   video_mode_list      physical address of a modelist_hdr_t, or 0
    //   video_mode_list_tag  MODELIST_MAGIC when video_mode_list is valid
    //
    // THE TAG IS LOAD-BEARING, NOT DECORATION. A reserved slot is NOT reliably
    // zero. MEASURED: g_boot_info in uefi/bootloader.c is a static in BSS with
    // g_memory_map[] declared immediately after it, and this struct carries no
    // version field and no size handshake, only BOOT_INFO_MAGIC. So an OLDER
    // loader paired with a NEWER kernel leaves whatever follows the struct in
    // the loader's BSS at these two offsets, which is arbitrary non-zero data,
    // not zero. A kernel that treated a non-zero video_mode_list as a pointer
    // would dereference a memory-map entry. The kernel MUST therefore check
    // video_mode_list_tag == MODELIST_MAGIC BEFORE touching video_mode_list,
    // and, having done so, must still re-check hdr->magic, hdr->count against
    // video_mode_count, and the MODELIST_MAX ceiling before walking the array.
    // See print_video_mode_info() in kernel/main.c, which does all four.
    uint64_t video_mode_list;
    uint64_t video_mode_list_tag;
} __attribute__((packed)) boot_info_t;

// boot_info.video_mode_status. Kept in step with the loader's own copy of these
// defines in uefi/bootloader.c.
#define MODESEL_NONE       0   // no \boot\MODE.TXT: the firmware mode was kept
#define MODESEL_APPLIED    1   // a selection was honoured
#define MODESEL_REFUSED    2   // a selection was present but rejected as unsafe
                               // or unusable; the firmware mode was kept
#define MODESEL_SETFAILED  3   // SetMode returned an error; firmware kept its mode
// #DIAGLOG: MODESEL_NONE used to mean BOTH "there is no such file" and "the
// file is there and I could not read it", which are opposite answers to the one
// question a person who just wrote MODE.TXT onto a stick is asking. An ASUS
// panel booted 3840x2160 with the loader reporting MODESEL_NONE, and nothing in
// the record could say whether the file had been absent or unreadable.
#define MODESEL_READERR    5   // \boot\MODE.TXT EXISTS but could not be read, or
                               // read as empty; the firmware mode was kept
#define MODESEL_REVERTED   4   // SetMode succeeded but produced an unusable
                               // display, and the original mode was set back

// ===========================================================================
// #ASUSMODE: the GOP mode list the loader enumerated, carried to the kernel by
// physical address in boot_info.video_mode_list (see the comment on that field
// for why it is a side blob and why the tag must be checked first).
//
// DEFINED ONCE, HERE. uefi/bootloader.c keeps a copy that must be BYTE-
// IDENTICAL to this one, exactly as it does for boot_info_t itself: the loader
// is built by a separate UEFI toolchain and shares no header with the kernel,
// so the only thing keeping the two in step is this instruction and the fact
// that both are packed and fixed-width.
// ===========================================================================
#define MODELIST_MAGIC 0x4D4F44454C535431ULL   // "MODELST1"

// The ceiling the kernel refuses to read past. No firmware seen so far offers
// anything remotely close to this; the number exists so that a count which is
// garbage (a stale loader, a truncated blob) can never walk memory. A count
// over the ceiling is reported as REJECTED, never trusted.
#define MODELIST_MAX 512

// req_idx when \boot\MODE.TXT was not the bare-index form. Shares the value of
// MODESEL_NO_TARGET in the loader, deliberately: both mean "no such index".
#define MODELIST_NO_REQ 0xFFFFFFFFu

// modelist_ent_t.flags
#define MODELIST_F_CURRENT   0x1u  // this index was gop->Mode->Mode on entry
#define MODELIST_F_QUERYFAIL 0x2u  // QueryMode() failed; width/height are 0

typedef struct {
    uint64_t magic;      // MODELIST_MAGIC
    uint32_t count;      // entries that follow; must equal video_mode_count
    uint32_t cur_idx;    // gop->Mode->Mode as it was on ENTRY to gop_select_mode
    uint32_t req_w;      // MODE.TXT WxH request, 0 if absent or unparseable
    uint32_t req_h;
    uint32_t req_idx;    // MODE.TXT bare-index request, else MODELIST_NO_REQ
    uint32_t status;     // MODESEL_*, always the SAME value as video_mode_status
} __attribute__((packed)) modelist_hdr_t;      // 32 bytes

typedef struct {
    uint32_t width;         // 0 when MODELIST_F_QUERYFAIL is set
    uint32_t height;
    uint32_t pixel_format;  // raw EFI_GRAPHICS_PIXEL_FORMAT value, NOT remapped:
                            // 0=RGBX8888 1=BGRX8888 2=bitmask 3=BltOnly. Kept raw
                            // because the whole point is to see what the firmware
                            // actually reported, including a value we do not know.
    uint32_t flags;         // MODELIST_F_*
} __attribute__((packed)) modelist_ent_t;      // 16 bytes

// Global boot info pointer (set by kernel entry)
extern boot_info_t *g_boot_info;

// Boot info functions
void boot_info_init(boot_info_t *info);
void boot_info_print(void);
uint64_t boot_info_get_total_memory(void);
memory_map_entry_t* boot_info_get_memory_map(uint32_t *count);
framebuffer_info_t* boot_info_get_framebuffer(void);

#endif // BOOT_INFO_H
