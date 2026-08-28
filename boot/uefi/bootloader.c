// bootloader.c - Full UEFI Bootloader for MayteraOS
// Loads kernel.elf from /boot/ directory and executes it
// Passes boot_info structure with memory map, framebuffer, and ACPI info

#include <efi.h>
#include <efilib.h>

// Boot info magic (matches kernel/boot_info.h)
#define BOOT_INFO_MAGIC 0x4D415954455241ULL  // "MAYTERA"

// Memory types
#define MEMORY_TYPE_USABLE           1
#define MEMORY_TYPE_RESERVED         2
#define MEMORY_TYPE_ACPI_RECLAIMABLE 3
#define MEMORY_TYPE_ACPI_NVS         4
#define MEMORY_TYPE_BAD              5
#define MEMORY_TYPE_BOOTLOADER       6
#define MEMORY_TYPE_KERNEL           7
#define MEMORY_TYPE_FRAMEBUFFER      8

// Pixel formats
#define PIXEL_FORMAT_RGB  0
#define PIXEL_FORMAT_BGR  1
#define PIXEL_FORMAT_MASK 2

// Maximum memory map entries.
//
// ASUS bring-up: this was 256 and build_memory_map() dropped everything past it
// SILENTLY (the cap was in the for-loop guard, so the loop simply stopped and
// nothing counted or reported the remainder). A modern laptop's UEFI memory map
// is routinely larger than a VM's, and the entries that come LAST are as likely
// to be usable RAM as anything else, so a silent truncation is lost memory the
// kernel never knew existed. 512 entries costs sizeof(memory_map_entry_t) * 512
// = 24 * 512 = 12288 bytes of static storage in the loader, which is nothing.
// The truncation is now also COUNTED and reported (see build_memory_map).
#define MAX_MEMORY_MAP_ENTRIES 512

// ELF64 header structures
#define EI_NIDENT 16
#define EI_CLASS 4
#define ELFCLASS64 2
#define PT_LOAD 1

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} Elf64_Phdr;

// Memory map entry (matches kernel/boot_info.h)
typedef struct {
    UINT64 base;
    UINT64 length;
    UINT32 type;
    UINT32 attributes;
} __attribute__((packed)) memory_map_entry_t;

// Framebuffer info (matches kernel/boot_info.h)
typedef struct {
    UINT64 address;
    UINT32 width;
    UINT32 height;
    UINT32 pitch;
    UINT32 bpp;
    UINT32 pixel_format;
    UINT32 red_mask;
    UINT32 green_mask;
    UINT32 blue_mask;
    UINT32 reserved_mask;
} __attribute__((packed)) framebuffer_info_t;

// ACPI info (matches kernel/boot_info.h)
typedef struct {
    UINT64 rsdp_address;
    UINT32 rsdp_version;
    UINT32 reserved;
} __attribute__((packed)) acpi_info_t;

// Boot info structure (matches kernel/boot_info.h)
typedef struct {
    UINT64 magic;
    UINT64 memory_map_address;
    UINT32 memory_map_entries;
    UINT32 memory_map_entry_size;
    UINT64 total_memory;
    framebuffer_info_t framebuffer;
    acpi_info_t acpi;
    UINT64 kernel_physical_base;
    UINT64 kernel_virtual_base;
    UINT64 kernel_size;
    // #745 (local 102): display rotation - see kernel/boot_info.h for the
    // authoritative comment. Keep this struct byte-identical to that one.
    UINT64 display_rotation;
    // #ASUSDIAG: descriptors the loop below could NOT record - see
    // kernel/boot_info.h for the authoritative comment. Keep this struct
    // byte-identical to that one.
    UINT64 memory_map_dropped;
    // #QUIETBOOT: the boot-diagnostic arming word - see kernel/boot_info.h for
    // the authoritative comment. Keep this struct byte-identical to that one.
    UINT64 diag_flags;

    // Boot-time video mode: what the GOP enumeration and the \boot\MODE.TXT
    // selection did. See kernel/boot_info.h for the authoritative comment. Keep
    // this struct byte-identical to that one.
    UINT64 video_mode_count;
    UINT64 video_mode_current;
    UINT64 video_mode_status;

    // #ASUSMODE: physical address of the enumerated GOP mode list, and the
    // in-band tag that says the address is real. These occupy what was
    // reserved[2], at the SAME offsets, so sizeof(boot_info_t) is unchanged.
    // See kernel/boot_info.h for the authoritative comment, in particular WHY
    // the tag is load-bearing: an older loader leaves BSS garbage (not zero) at
    // these offsets, so the kernel must never dereference the address without
    // it. Keep this struct byte-identical to that one.
    UINT64 video_mode_list;
    UINT64 video_mode_list_tag;
} __attribute__((packed)) boot_info_t;

// boot_info.video_mode_status. Kept in step with kernel/boot_info.h.
#define MODESEL_NONE       0   // no \boot\MODE.TXT: the firmware mode was kept
#define MODESEL_APPLIED    1   // a selection was honoured
#define MODESEL_REFUSED    2   // a selection was present but rejected as unsafe
                               // or unusable; the firmware mode was kept
#define MODESEL_SETFAILED  3   // SetMode returned an error; firmware kept its mode
// #DIAGLOG: see kernel/boot_info.h for why absent and unreadable are separate.
#define MODESEL_READERR    5   // \boot\MODE.TXT EXISTS but could not be read, or
                               // read as empty; the firmware mode was kept
#define MODESEL_REVERTED   4   // SetMode succeeded but produced an unusable
                               // display, and the original mode was set back
#define MODESEL_NO_TARGET  0xFFFFFFFFu   // "no candidate", not a mode number

// #ASUSMODE: the enumerated-mode-list blob handed to the kernel. DEFINED in
// kernel/boot_info.h; this is the loader's copy and must be kept BYTE-IDENTICAL
// to it, exactly like boot_info_t above (the two files share no header, so
// nothing but this instruction and the fixed-width packed layout keeps them in
// step). Rationale for the whole mechanism lives on boot_info.video_mode_list
// in kernel/boot_info.h; the short version is that an ASUS panel came up
// 3840x2160 with "1920x1080" in \boot\MODE.TXT and the enumerated list, which
// is the one piece of evidence that would settle it, reached the UEFI console
// only and so was never captured.
#define MODELIST_MAGIC 0x4D4F44454C535431ULL   // "MODELST1"
#define MODELIST_MAX   512                     // count ceiling the kernel trusts
#define MODELIST_NO_REQ 0xFFFFFFFFu            // req_idx: not the bare-index form
#define MODELIST_F_CURRENT   0x1u              // this index was Mode->Mode on entry
#define MODELIST_F_QUERYFAIL 0x2u              // QueryMode failed; w/h are 0

typedef struct {
    UINT64 magic;      // MODELIST_MAGIC
    UINT32 count;      // entries that follow; equals video_mode_count
    UINT32 cur_idx;    // gop->Mode->Mode as it was on ENTRY to gop_select_mode
    UINT32 req_w;      // MODE.TXT WxH request, 0 if absent or unparseable
    UINT32 req_h;
    UINT32 req_idx;    // MODE.TXT bare-index request, else MODELIST_NO_REQ
    UINT32 status;     // MODESEL_*, always the SAME value as video_mode_status
} __attribute__((packed)) modelist_hdr_t;      // 32 bytes

typedef struct {
    UINT32 width;         // 0 when MODELIST_F_QUERYFAIL is set
    UINT32 height;
    UINT32 pixel_format;  // raw EFI_GRAPHICS_PIXEL_FORMAT value, NOT remapped
    UINT32 flags;         // MODELIST_F_*
} __attribute__((packed)) modelist_ent_t;      // 16 bytes

// Kernel entry point type
typedef void (*kernel_entry_t)(boot_info_t *boot_info);

// Global boot info and memory map
static boot_info_t g_boot_info;

// #QUIETBOOT: the pre-kernel console gate. DECLARED HERE, AT THE TOP, ON PURPOSE.
//
// These live above every user rather than beside read_boot_markers() at the
// bottom, because the first DPRINT() in this file is in gop_select_mode() around
// line 470 and the marker reader is around line 1700. With the macro defined at
// the point of the reader, that first use compiled as an IMPLICIT DECLARATION of
// a function called DPRINT: only a warning, and `ld -shared` does not require
// undefined symbols to resolve, so BOOTX64.EFI LINKED CLEANLY and would have
// carried an unresolved relocation into the firmware. "It built" would have been
// the last good news of that boot. Definitions go above their users.
//
// See read_boot_markers() further down for what the two markers mean and for the
// rule about which lines are gated (progress and success) and which are not
// (anything that failed, was refused, was reverted, was truncated, or warns).

// \boot\DIAG.TXT present: show the loader console AND arm the kernel's on-screen
// boot diagnostics. Default 0, so a path that somehow skips the read is quiet
// rather than noisy, which is the direction a default should fail in.
static int g_diag_console = 0;
// \boot\NOCHIME.TXT present: skip the pre-kernel boot chime.
static int g_nochime      = 0;
// Has the banner been printed? The failure path prints it if the quiet boot did
// not, so an error line is never the first thing on an otherwise blank screen
// with nothing to say which program produced it.
static int g_banner_shown = 0;

// Every progress/success line in this file goes through this. Failures do not.
#define DPRINT(...) do { if (g_diag_console) Print(__VA_ARGS__); } while (0)

static memory_map_entry_t g_memory_map[MAX_MEMORY_MAP_ENTRIES];

// ACPI GUIDs (these are not declared in efilib.h)
static EFI_GUID Acpi20TableGuid = ACPI_20_TABLE_GUID;
static EFI_GUID Acpi10TableGuid = ACPI_TABLE_GUID;
// Note: gEfiGraphicsOutputProtocolGuid is already declared extern in efilib.h

// Helper: Open root directory
EFI_STATUS open_root_dir(EFI_HANDLE ImageHandle, EFI_FILE_PROTOCOL **root) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *file_system;
    EFI_GUID loaded_image_protocol = LOADED_IMAGE_PROTOCOL;
    EFI_GUID simple_file_system_protocol = SIMPLE_FILE_SYSTEM_PROTOCOL;

    // Get loaded image protocol from our image handle
    status = uefi_call_wrapper(BS->HandleProtocol, 3,
                               ImageHandle, &loaded_image_protocol, (void**)&loaded_image);
    if (EFI_ERROR(status)) {
        return status;
    }

    // Get file system protocol from the device we loaded from
    status = uefi_call_wrapper(BS->HandleProtocol, 3,
                               loaded_image->DeviceHandle, &simple_file_system_protocol, (void**)&file_system);
    if (EFI_ERROR(status)) {
        return status;
    }

    // Open root volume
    status = uefi_call_wrapper(file_system->OpenVolume, 2, file_system, root);
    return status;
}

// Helper: Load file into memory
EFI_STATUS load_file(EFI_FILE_PROTOCOL *root, CHAR16 *path, void **buffer, UINTN *size) {
    EFI_STATUS status;
    EFI_FILE_PROTOCOL *file;
    EFI_FILE_INFO *file_info;
    UINTN info_size;
    EFI_GUID file_info_id = EFI_FILE_INFO_ID;

    // Open file
    status = uefi_call_wrapper(root->Open, 5, root, &file, path,
                               EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(status)) {
        return status;
    }

    // Get file info to determine size
    info_size = sizeof(EFI_FILE_INFO) + 512;
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, info_size, (void**)&file_info);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    status = uefi_call_wrapper(file->GetInfo, 4, file, &file_info_id, &info_size, file_info);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePool, 1, file_info);
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    *size = file_info->FileSize;
    uefi_call_wrapper(BS->FreePool, 1, file_info);

    // Allocate buffer
    status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData, *size, buffer);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(file->Close, 1, file);
        return status;
    }

    // Read file
    status = uefi_call_wrapper(file->Read, 3, file, size, *buffer);
    uefi_call_wrapper(file->Close, 1, file);

    return status;
}

// ---------------------------------------------------------------------------
// #modeset: GOP mode ENUMERATION and boot-time mode SELECTION.
//
// WHY THIS EXISTS. Until now the framebuffer was simply whatever mode the
// firmware happened to leave the panel in: init_framebuffer() read
// gop->Mode->Info and never asked what else was on offer. UEFI GOP has always
// exposed the whole list (Mode->MaxMode plus QueryMode) and a SetMode to pick
// one, and this loader called neither, so the OS had no resolution choice at
// all on any machine. We had never even RECORDED what a given panel supports.
//
// TWO SEPARATE JOBS, and the first is unconditional:
//
//  1. ALWAYS enumerate and print every mode the firmware offers, marking the
//     current one. This runs on every boot whether or not a selection exists,
//     because the list is diagnostic gold in its own right and we have never
//     captured it from any real machine. On a laptop with no serial port the
//     UEFI console is the only channel there is, and /BOOTLOG.TXT does not
//     exist yet at this point in the boot.
//
//  2. Optionally apply \boot\MODE.TXT. Same \boot spelling and the same
//     best-effort discipline as \boot\ROTATE.TXT above, for the same reason:
//     the kernel-side writer goes through fat_write_file(), which silently
//     redirects any path that is not /boot or /EFI to the ext2 root volume, so
//     \boot is the one spelling both sides agree on.
//
// THE SAFETY RULE, WHICH OUTRANKS THE FEATURE: a machine that will not display
// anything is unrecoverable for a user with no serial port. So every failure
// path leaves the FIRMWARE'S OWN MODE untouched, and there are five of them,
// each logged distinctly rather than collapsed into one "failed":
//
//   - no file, or an unreadable or empty one -> keep firmware mode
//   - content that does not parse             -> refuse, keep firmware mode
//   - a mode index out of range, or a WxH the -> refuse, keep firmware mode
//     firmware does not offer
//   - the target mode is PixelBltOnly         -> REFUSE. This is the important
//     one. PixelBltOnly means there IS no linear framebuffer, so every write
//     the kernel makes goes nowhere and the screen stays black after
//     ExitBootServices. Honouring such a request would produce exactly the
//     unrecoverable outcome the rule above exists to prevent.
//   - SetMode returns an error                -> refuse. A failed SetMode
//     leaves the previous mode in effect, which is the specified behaviour.
//
// And one path that is not a refusal but a REVERT: if SetMode SUCCEEDS and the
// resulting mode is unusable anyway (no framebuffer base, a zero dimension, or
// a format with no linear framebuffer), set the original mode number back. A
// successful call that yields an unusable display is the one case a
// before-the-fact check cannot catch, so it is caught after the fact.
//
// RECOVERY, stated plainly because it is the honest answer to "what if the
// panel goes black anyway": \boot\MODE.TXT is on the FAT ESP, so deleting it
// needs nothing but that disk or USB stick in any other computer. That real
// escape hatch is why a FILE, and not a compiled-in constant, is the right
// mechanism for this.
//
// ACCEPTED CONTENT, first line only, leading blanks skipped:
//   1920x1080  a resolution. Preferred spelling: it means the same thing on
//              every machine, whereas a mode NUMBER is firmware-specific and
//              is not stable across a firmware update.
//   7          a raw GOP mode index, for when two modes share a resolution and
//              differ only in pixel format.
//   anything else, an empty file included, is refused and logged as refused.
//
// THE MARKER-FILE TRAP THIS DELIBERATELY AVOIDS (measured in this tree):
// diskimg_boot_harness() and img_shadow_selftest() are compiled, linked and
// CALLED every boot, and both return at their first line because their marker
// files exist on no image and in no asset manifest. Every dead-code check
// passes because the CALLER runs; what is missing is a FILE. So this function
// prints a [modeset] line saying what it did on EVERY boot, armed or not.
// "No line" is then a real signal that the code did not run, instead of being
// indistinguishable from "no selection is configured".
// ---------------------------------------------------------------------------

// Parse the first line of a MODE.TXT buffer.
//   returns 1 and sets *w,*h  for the WxH form
//   returns 2 and sets *idx   for the bare-index form
//   returns 0 for anything that does not parse, which the caller REFUSES.
// Deliberately strict: trailing garbage on the line is a parse failure, not
// something to shrug off, because "1920x10 80" silently becoming 1920x10 is
// how a user ends up staring at a panel they cannot read.
static int modesel_parse(const CHAR8 *b, UINTN n, UINT32 *w, UINT32 *h, UINT32 *idx) {
    UINTN i = 0;
    UINT64 a = 0, c = 0;
    UINTN da = 0, dc = 0;

    while (i < n && (b[i] == ' ' || b[i] == '\t')) i++;

    while (i < n && b[i] >= '0' && b[i] <= '9') {
        a = a * 10 + (UINT64)(b[i] - '0');
        if (a > 100000) return 0;          // absurd: refuse rather than wrap
        i++; da++;
    }
    if (da == 0) return 0;

    if (i < n && (b[i] == 'x' || b[i] == 'X')) {
        i++;
        while (i < n && b[i] >= '0' && b[i] <= '9') {
            c = c * 10 + (UINT64)(b[i] - '0');
            if (c > 100000) return 0;
            i++; dc++;
        }
        if (dc == 0) return 0;
        if (i < n && b[i] != '\r' && b[i] != '\n' && b[i] != ' ' && b[i] != '\t') return 0;
        if (a == 0 || c == 0) return 0;
        *w = (UINT32)a; *h = (UINT32)c;
        return 1;
    }

    if (i < n && b[i] != '\r' && b[i] != '\n' && b[i] != ' ' && b[i] != '\t') return 0;
    *idx = (UINT32)a;
    return 2;
}

// #ASUSMODE: record a MODESEL_* outcome in BOTH places, always together.
//
// gop_select_mode() has eleven distinct return paths and the mode-list blob
// carries its own copy of the final status so that a kernel reading the blob
// sees the same verdict as boot_info.video_mode_status. Two copies that can be
// written independently is exactly how a log starts lying, so they are never
// written independently: this is the only writer of either.
static void modesel_status(modelist_hdr_t *ml, UINT32 st) {
    g_boot_info.video_mode_status = (UINT64)st;
    if (ml) ml->status = st;
}

static void gop_select_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL *gop, EFI_FILE_PROTOCOL *root) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi = NULL;
    UINTN misz = 0;
    void *buf = NULL;
    UINTN sz = 0;
    UINT32 want_w = 0, want_h = 0, want_idx = 0;
    UINT32 target = MODESEL_NO_TARGET;
    UINT32 any_match = MODESEL_NO_TARGET;
    UINT32 odd_fmt = 0, odd_blt = 0, odd_first = MODESEL_NO_TARGET;
    UINT32 t_w = 0, t_h = 0;
    int t_blt = 0;
    int kind;
    UINT32 m;
    UINT32 max_mode = gop->Mode->MaxMode;
    UINT32 cur_mode = gop->Mode->Mode;
    modelist_hdr_t *ml = NULL;
    modelist_ent_t *ment = NULL;

    // #ASUSMODE: allocate the mode-list blob BEFORE the enumeration loop, so the
    // loop below fills it as it goes and there is no second QueryMode pass. Two
    // passes could disagree (QueryMode is firmware code and is not promised to
    // be stable across calls), and a list that disagrees with what was printed
    // is worse than no list.
    //
    // EfiLoaderData ON PURPOSE: that allocation SURVIVES ExitBootServices, and
    // the kernel reads this blob very early, in print_video_mode_info(), long
    // before pmm_init() exists to own memory. It is therefore NEVER FreePool()d;
    // freeing it would hand the kernel a dangling physical address.
    //
    // A FAILED ALLOCATION MUST NOT BE ABLE TO BREAK A BOOT. This is a
    // diagnostic; if AllocatePool fails, or the firmware reports an absurd mode
    // count, ml stays NULL, video_mode_list and video_mode_list_tag stay zero,
    // and everything below behaves exactly as it did before this existed.
    if (max_mode > 0 && max_mode <= (UINT32)MODELIST_MAX) {
        UINTN mlsz = sizeof(modelist_hdr_t) + (UINTN)max_mode * sizeof(modelist_ent_t);
        EFI_STATUS mlst = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                            mlsz, (void**)&ml);
        if (EFI_ERROR(mlst) || !ml) {
            ml = NULL;
        } else {
            SetMem(ml, mlsz, 0);
            ml->magic   = MODELIST_MAGIC;
            ml->count   = max_mode;
            ml->cur_idx = cur_mode;
            ml->req_w   = 0;
            ml->req_h   = 0;
            ml->req_idx = MODELIST_NO_REQ;
            ml->status  = MODESEL_NONE;
            ment = (modelist_ent_t *)(ml + 1);
        }
    }

    g_boot_info.video_mode_count   = (UINT64)max_mode;
    g_boot_info.video_mode_current = (UINT64)cur_mode;
    modesel_status(ml, MODESEL_NONE);

    // ---- job 1: enumerate, unconditionally -----------------------------------
    // COMPACT ON PURPOSE. `index:WxH` tokens, six to a line, so a 30-mode
    // firmware costs five lines instead of thirty. #QUIETBOOT has just made the
    // rest of the boot deliberately quiet, and a wall of mode detail would undo
    // that; but the list itself must NOT be hidden behind \boot\DIAG.TXT,
    // because reading it is how you find out what to put in \boot\MODE.TXT in
    // the first place, and needing a second marker file to learn the contents of
    // the first is a chicken-and-egg nobody should have to solve on a laptop
    // with no serial port.
    //
    // Pixel format is NOT printed per mode, for the same reason: it is the same
    // value for every mode on every machine seen so far, so printing it thirty
    // times is thirty times nothing. What matters is the EXCEPTION, so any mode
    // that is not the ordinary 8-8-8-8 kind gets its own line below the list.
    // PixelBltOnly especially: it has no linear framebuffer, so it is the one
    // value that turns a mode into a guaranteed black screen.
    DPRINT(L"      [modeset] %d modes (* = current):\r\n", (int)max_mode);
    for (m = 0; m < max_mode; m++) {
        if ((m % 6) == 0) DPRINT(L"        ");
        status = uefi_call_wrapper(gop->QueryMode, 4, gop, m, &misz, &mi);
        if (EFI_ERROR(status) || !mi) {
            DPRINT(L"%d:query-failed ", (int)m);
            if (ment) {
                // Width/height stay 0; the flag is what tells the kernel that
                // 0x0 here means "the firmware would not say" and not "a mode
                // of zero size".
                ment[m].width        = 0;
                ment[m].height       = 0;
                ment[m].pixel_format = 0;
                ment[m].flags        = MODELIST_F_QUERYFAIL |
                                       ((m == cur_mode) ? MODELIST_F_CURRENT : 0u);
            }
        } else {
            DPRINT(L"%d%s:%dx%d ", (int)m, (m == cur_mode) ? L"*" : L"",
                  (int)mi->HorizontalResolution, (int)mi->VerticalResolution);
            if (ment) {
                ment[m].width        = mi->HorizontalResolution;
                ment[m].height       = mi->VerticalResolution;
                ment[m].pixel_format = (UINT32)mi->PixelFormat;
                ment[m].flags        = (m == cur_mode) ? MODELIST_F_CURRENT : 0u;
            }
            if (mi->PixelFormat != PixelRedGreenBlueReserved8BitPerColor &&
                mi->PixelFormat != PixelBlueGreenRedReserved8BitPerColor) {
                odd_fmt++;
                odd_first = (odd_first == MODESEL_NO_TARGET) ? m : odd_first;
                odd_blt  += (mi->PixelFormat == PixelBltOnly) ? 1 : 0;
            }
            uefi_call_wrapper(BS->FreePool, 1, mi);
            mi = NULL;
        }
        if ((m % 6) == 5) DPRINT(L"\r\n");
    }
    if ((max_mode % 6) != 0) DPRINT(L"\r\n");
    if (odd_fmt) {
        DPRINT(L"      [modeset] %d mode(s) are not 8-8-8-8 (first is %d); %d are PixelBltOnly\r\n",
              (int)odd_fmt, (int)odd_first, (int)odd_blt);
        if (odd_blt) DPRINT(L"                A PixelBltOnly mode has NO linear framebuffer and is refused.\r\n");
    }

    // #ASUSMODE: the blob is complete, so publish it. Everything from here down
    // only ever updates ml->status (via modesel_status), which the kernel reads
    // from the same allocation, so publishing now rather than at each return
    // keeps every one of the eleven exits below identical in this respect.
    if (ml) {
        g_boot_info.video_mode_list     = (UINT64)(UINTN)ml;
        g_boot_info.video_mode_list_tag = MODELIST_MAGIC;
    }

    // ---- job 2: the selection ----------------------------------------------
    if (!root) {
        Print(L"      [modeset] no root volume; keeping firmware mode %d\r\n", (int)cur_mode);
        return;
    }

    status = load_file(root, L"boot\\MODE.TXT", &buf, &sz);
    if (EFI_ERROR(status) || !buf || sz == 0) {
        if (buf) uefi_call_wrapper(BS->FreePool, 1, buf);
        // #DIAGLOG: ABSENT and UNREADABLE are different answers and both used to
        // record as MODESEL_NONE. A person who has just written MODE.TXT onto a
        // stick and still booted at the firmware resolution needs to know which
        // of the two happened; "no such file" sends them to check the spelling,
        // "cannot read it" sends them to check the filesystem.
        if (status == EFI_NOT_FOUND) {
            DPRINT(L"      [modeset] no \\boot\\MODE.TXT; keeping firmware mode %d (%dx%d)\r\n",
                  (int)cur_mode,
                  (int)gop->Mode->Info->HorizontalResolution,
                  (int)gop->Mode->Info->VerticalResolution);
        } else {
            modesel_status(ml, MODESEL_READERR);
            Print(L"      [modeset] \\boot\\MODE.TXT PRESENT but unusable (status 0x%x, %d bytes);\r\n",
                  status, (int)sz);
            Print(L"                keeping firmware mode %d.\r\n", (int)cur_mode);
        }
        return;
    }

    kind = modesel_parse((const CHAR8 *)buf, sz, &want_w, &want_h, &want_idx);
    uefi_call_wrapper(BS->FreePool, 1, buf);

    // #ASUSMODE: the REQUEST, recorded before it is judged. The enumeration
    // above ran before MODE.TXT was even read, so these three are necessarily
    // filled in here rather than at allocation time. kind == 0 is a parse
    // failure and leaves all three at their "no request" values, which is the
    // honest answer: nothing was asked for that could be acted on.
    if (ml) {
        ml->req_w   = (kind == 1) ? want_w : 0;
        ml->req_h   = (kind == 1) ? want_h : 0;
        ml->req_idx = (kind == 2) ? want_idx : MODELIST_NO_REQ;
    }

    if (kind == 0) {
        modesel_status(ml, MODESEL_REFUSED);
        Print(L"      [modeset] REFUSED: \\boot\\MODE.TXT does not parse. Keeping mode %d.\r\n", (int)cur_mode);
        Print(L"                Write \"1920x1080\" or a mode index such as \"7\".\r\n");
        return;
    }

    if (kind == 2) {
        if (want_idx >= max_mode) {
            modesel_status(ml, MODESEL_REFUSED);
            Print(L"      [modeset] REFUSED: mode index %d is out of range (firmware has 0..%d).\r\n",
                  (int)want_idx, (int)max_mode - 1);
            Print(L"                Keeping mode %d.\r\n", (int)cur_mode);
            return;
        }
        target = want_idx;
    } else {
        // Prefer a match that HAS a linear framebuffer. Remember any match too,
        // so a resolution that exists only as PixelBltOnly can be refused with
        // a message that says which of the two things went wrong.
        for (m = 0; m < max_mode; m++) {
            status = uefi_call_wrapper(gop->QueryMode, 4, gop, m, &misz, &mi);
            if (EFI_ERROR(status) || !mi) continue;
            if (mi->HorizontalResolution == want_w && mi->VerticalResolution == want_h) {
                if (any_match == MODESEL_NO_TARGET) any_match = m;
                if (mi->PixelFormat != PixelBltOnly && target == MODESEL_NO_TARGET) target = m;
            }
            uefi_call_wrapper(BS->FreePool, 1, mi);
            mi = NULL;
        }
        if (target == MODESEL_NO_TARGET) {
            modesel_status(ml, MODESEL_REFUSED);
            if (any_match != MODESEL_NO_TARGET) {
                Print(L"      [modeset] REFUSED: %dx%d exists only as PixelBltOnly (mode %d), which has\r\n",
                      (int)want_w, (int)want_h, (int)any_match);
                Print(L"                NO linear framebuffer. Keeping mode %d.\r\n", (int)cur_mode);
            } else {
                Print(L"      [modeset] REFUSED: this firmware does not offer %dx%d. Pick one of the\r\n",
                      (int)want_w, (int)want_h);
                Print(L"                modes listed above. Keeping mode %d.\r\n", (int)cur_mode);
            }
            return;
        }
    }

    // Verify the TARGET before committing to it.
    status = uefi_call_wrapper(gop->QueryMode, 4, gop, target, &misz, &mi);
    if (EFI_ERROR(status) || !mi) {
        modesel_status(ml, MODESEL_REFUSED);
        Print(L"      [modeset] REFUSED: QueryMode(%d) failed (0x%x). Keeping mode %d.\r\n",
              (int)target, status, (int)cur_mode);
        return;
    }
    t_w = mi->HorizontalResolution;
    t_h = mi->VerticalResolution;
    t_blt = (mi->PixelFormat == PixelBltOnly);
    uefi_call_wrapper(BS->FreePool, 1, mi);
    mi = NULL;

    if (t_blt) {
        modesel_status(ml, MODESEL_REFUSED);
        Print(L"      [modeset] REFUSED: mode %d (%dx%d) is PixelBltOnly. It has NO linear\r\n",
              (int)target, (int)t_w, (int)t_h);
        Print(L"                framebuffer, so honouring it would leave a BLACK SCREEN after\r\n");
        Print(L"                boot services exit. Keeping mode %d.\r\n", (int)cur_mode);
        return;
    }
    if (t_w == 0 || t_h == 0) {
        modesel_status(ml, MODESEL_REFUSED);
        Print(L"      [modeset] REFUSED: mode %d reports %dx%d. Keeping mode %d.\r\n",
              (int)target, (int)t_w, (int)t_h, (int)cur_mode);
        return;
    }

    if (target == cur_mode) {
        modesel_status(ml, MODESEL_APPLIED);
        DPRINT(L"      [modeset] mode %d (%dx%d) is already current; nothing to do.\r\n",
              (int)target, (int)t_w, (int)t_h);
        return;
    }

    DPRINT(L"      [modeset] applying mode %d (%dx%d) from \\boot\\MODE.TXT ...\r\n",
          (int)target, (int)t_w, (int)t_h);
    status = uefi_call_wrapper(gop->SetMode, 2, gop, target);
    if (EFI_ERROR(status)) {
        modesel_status(ml, MODESEL_SETFAILED);
        Print(L"      [modeset] SetMode(%d) FAILED (0x%x). Firmware keeps mode %d.\r\n",
              (int)target, status, (int)cur_mode);
        return;
    }

    // After the fact: a SUCCESSFUL SetMode can still leave an unusable display.
    if (gop->Mode->FrameBufferBase == 0 ||
        gop->Mode->Info->HorizontalResolution == 0 ||
        gop->Mode->Info->VerticalResolution == 0 ||
        gop->Mode->Info->PixelFormat == PixelBltOnly) {
        Print(L"      [modeset] mode %d was SET but is UNUSABLE (base=0x%lx %dx%d fmt=%d).\r\n",
              (int)target, (UINT64)gop->Mode->FrameBufferBase,
              (int)gop->Mode->Info->HorizontalResolution,
              (int)gop->Mode->Info->VerticalResolution,
              (int)gop->Mode->Info->PixelFormat);
        Print(L"      [modeset] REVERTING to mode %d.\r\n", (int)cur_mode);
        uefi_call_wrapper(gop->SetMode, 2, gop, cur_mode);
        modesel_status(ml, MODESEL_REVERTED);
        g_boot_info.video_mode_current = (UINT64)gop->Mode->Mode;
        return;
    }

    modesel_status(ml, MODESEL_APPLIED);
    g_boot_info.video_mode_current = (UINT64)gop->Mode->Mode;
    DPRINT(L"      [modeset] mode %d APPLIED: %dx%d fb=0x%lx\r\n",
          (int)gop->Mode->Mode,
          (int)gop->Mode->Info->HorizontalResolution,
          (int)gop->Mode->Info->VerticalResolution,
          (UINT64)gop->Mode->FrameBufferBase);
}

// Initialize framebuffer info from GOP
EFI_STATUS init_framebuffer(EFI_FILE_PROTOCOL *root) {
    EFI_STATUS status;
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mode_info;

    // Get Graphics Output Protocol
    status = uefi_call_wrapper(BS->LocateProtocol, 3,
                               &gEfiGraphicsOutputProtocolGuid, NULL, (void**)&gop);
    if (EFI_ERROR(status)) {
        Print(L"      Warning: No GOP found, framebuffer unavailable\r\n");
        return status;
    }

    // Enumerate every mode the firmware offers and, if \boot\MODE.TXT asks for
    // one, apply it. MUST happen BEFORE mode_info is read below, because a
    // successful SetMode changes both Mode->Info and Mode->FrameBufferBase, and
    // boot_info has to describe the mode actually in effect at handover rather
    // than the one that was. Every failure path inside leaves the firmware's own
    // mode untouched.
    gop_select_mode(gop, root);

    // Get current mode info
    mode_info = gop->Mode->Info;

    // ASUS bring-up: report what the FIRMWARE actually gave us, before anything
    // is assumed about it. On a laptop with no serial port, and before the
    // kernel exists, the UEFI console is the ONLY diagnostic channel there is,
    // and this is the difference between "the firmware handed us an unusable
    // graphics mode" and "the kernel died on entry", which look identical from
    // a black screen.
    //
    // NOTE: the kernel's 32bpp assumption is NOT changed here. bpp is still
    // forced to 32 and pitch to PixelsPerScanLine * 4 below, exactly as before.
    // This block only makes the assumption VISIBLE when it is wrong.
    DPRINT(L"      GOP PixelFormat=%d PixelsPerScanLine=%d\r\n",
          (int)mode_info->PixelFormat, (int)mode_info->PixelsPerScanLine);
    DPRINT(L"      GOP Resolution=%dx%d\r\n",
          (int)mode_info->HorizontalResolution,
          (int)mode_info->VerticalResolution);
    DPRINT(L"      GOP FrameBufferBase=0x%lx FrameBufferSize=%lu\r\n",
          (UINT64)gop->Mode->FrameBufferBase,
          (UINT64)gop->Mode->FrameBufferSize);

    if (mode_info->PixelFormat == PixelBltOnly) {
        // There is NO linear framebuffer in this mode: FrameBufferBase and
        // FrameBufferSize are meaningless, and every write the kernel makes to
        // that address goes nowhere (or somewhere worse). The machine will show
        // nothing at all once the firmware console is gone.
        Print(L"\r\n");
        Print(L"  *** WARNING: GOP PixelFormat is PixelBltOnly ***\r\n");
        Print(L"  There is NO linear framebuffer in this mode.\r\n");
        Print(L"  FrameBufferBase is meaningless and the screen will stay blank\r\n");
        Print(L"  after boot services exit. This is a FIRMWARE mode problem,\r\n");
        Print(L"  not a kernel crash.\r\n");
        Print(L"\r\n");
    } else if (mode_info->PixelFormat == PixelBitMask) {
        // A linear framebuffer exists, but the channel layout is described by
        // masks rather than being one of the two 8-8-8-8 orders the kernel
        // knows. The masks ARE captured into boot_info below; what is not
        // handled is a pixel that is not 32 bits wide, or channels that are not
        // byte-aligned, which show up as wrong colours or a skewed image.
        Print(L"\r\n");
        Print(L"  *** WARNING: GOP PixelFormat is PixelBitMask ***\r\n");
        Print(L"  R=0x%x G=0x%x B=0x%x Rsvd=0x%x\r\n",
              (int)mode_info->PixelInformation.RedMask,
              (int)mode_info->PixelInformation.GreenMask,
              (int)mode_info->PixelInformation.BlueMask,
              (int)mode_info->PixelInformation.ReservedMask);
        Print(L"  The kernel assumes 32bpp with byte-aligned channels; expect\r\n");
        Print(L"  wrong colours or a skewed image if that does not hold.\r\n");
        Print(L"\r\n");
    }

    // Fill framebuffer info
    g_boot_info.framebuffer.address = gop->Mode->FrameBufferBase;
    g_boot_info.framebuffer.width = mode_info->HorizontalResolution;
    g_boot_info.framebuffer.height = mode_info->VerticalResolution;
    g_boot_info.framebuffer.pitch = mode_info->PixelsPerScanLine * 4;  // Assume 32-bit
    g_boot_info.framebuffer.bpp = 32;

    // Determine pixel format
    switch (mode_info->PixelFormat) {
        case PixelRedGreenBlueReserved8BitPerColor:
            g_boot_info.framebuffer.pixel_format = PIXEL_FORMAT_RGB;
            g_boot_info.framebuffer.red_mask = 0x000000FF;
            g_boot_info.framebuffer.green_mask = 0x0000FF00;
            g_boot_info.framebuffer.blue_mask = 0x00FF0000;
            g_boot_info.framebuffer.reserved_mask = 0xFF000000;
            break;
        case PixelBlueGreenRedReserved8BitPerColor:
            g_boot_info.framebuffer.pixel_format = PIXEL_FORMAT_BGR;
            g_boot_info.framebuffer.red_mask = 0x00FF0000;
            g_boot_info.framebuffer.green_mask = 0x0000FF00;
            g_boot_info.framebuffer.blue_mask = 0x000000FF;
            g_boot_info.framebuffer.reserved_mask = 0xFF000000;
            break;
        case PixelBitMask:
            g_boot_info.framebuffer.pixel_format = PIXEL_FORMAT_MASK;
            g_boot_info.framebuffer.red_mask = mode_info->PixelInformation.RedMask;
            g_boot_info.framebuffer.green_mask = mode_info->PixelInformation.GreenMask;
            g_boot_info.framebuffer.blue_mask = mode_info->PixelInformation.BlueMask;
            g_boot_info.framebuffer.reserved_mask = mode_info->PixelInformation.ReservedMask;
            break;
        default:
            g_boot_info.framebuffer.pixel_format = PIXEL_FORMAT_BGR;
            break;
    }

    DPRINT(L"      Framebuffer: %ux%u at 0x%lx\r\n",
          g_boot_info.framebuffer.width,
          g_boot_info.framebuffer.height,
          g_boot_info.framebuffer.address);

    return EFI_SUCCESS;
}

// Find ACPI RSDP
void find_acpi_rsdp(void) {
    // First try ACPI 2.0
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *table = &ST->ConfigurationTable[i];

        if (CompareMem(&table->VendorGuid, &Acpi20TableGuid, sizeof(EFI_GUID)) == 0) {
            g_boot_info.acpi.rsdp_address = (UINT64)table->VendorTable;
            g_boot_info.acpi.rsdp_version = 2;
            DPRINT(L"      ACPI 2.0 RSDP at 0x%lx\r\n", g_boot_info.acpi.rsdp_address);
            return;
        }
    }

    // Fall back to ACPI 1.0
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *table = &ST->ConfigurationTable[i];

        if (CompareMem(&table->VendorGuid, &Acpi10TableGuid, sizeof(EFI_GUID)) == 0) {
            g_boot_info.acpi.rsdp_address = (UINT64)table->VendorTable;
            g_boot_info.acpi.rsdp_version = 1;
            DPRINT(L"      ACPI 1.0 RSDP at 0x%lx\r\n", g_boot_info.acpi.rsdp_address);
            return;
        }
    }

    Print(L"      Warning: No ACPI RSDP found\r\n");
}

// Convert UEFI memory type to our memory type
UINT32 convert_memory_type(UINT32 uefi_type) {
    switch (uefi_type) {
        case EfiConventionalMemory:
        case EfiBootServicesCode:
        case EfiBootServicesData:
            return MEMORY_TYPE_USABLE;
        case EfiLoaderCode:
        case EfiLoaderData:
            return MEMORY_TYPE_BOOTLOADER;
        case EfiACPIReclaimMemory:
            return MEMORY_TYPE_ACPI_RECLAIMABLE;
        case EfiACPIMemoryNVS:
            return MEMORY_TYPE_ACPI_NVS;
        case EfiUnusableMemory:
            return MEMORY_TYPE_BAD;
        default:
            return MEMORY_TYPE_RESERVED;
    }
}

// Build memory map from UEFI memory map
void build_memory_map(EFI_MEMORY_DESCRIPTOR *uefi_map, UINTN map_size,
                      UINTN descriptor_size) {
    UINT32 entry_count = 0;
    UINT32 descriptor_count = 0;
    UINT32 dropped_count = 0;
    UINT64 dropped_usable = 0;
    UINT64 total_memory = 0;
    UINT8 *ptr = (UINT8 *)uefi_map;

    // ASUS bring-up: walk the WHOLE map, always. The cap now controls what gets
    // RECORDED, not where the walk stops, so anything that does not fit can be
    // counted and reported instead of vanishing.
    for (UINTN offset = 0; offset < map_size; offset += descriptor_size) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(ptr + offset);
        descriptor_count++;

        if (entry_count >= MAX_MEMORY_MAP_ENTRIES) {
            dropped_count++;
            if (convert_memory_type(desc->Type) == MEMORY_TYPE_USABLE) {
                dropped_usable += desc->NumberOfPages * 4096;
            }
            continue;
        }

        g_memory_map[entry_count].base = desc->PhysicalStart;
        g_memory_map[entry_count].length = desc->NumberOfPages * 4096;
        g_memory_map[entry_count].type = convert_memory_type(desc->Type);
        g_memory_map[entry_count].attributes = (UINT32)desc->Attribute;

        // Count usable memory. Deliberately counts only RECORDED entries, so
        // total_memory always describes the map the kernel actually receives
        // rather than a total it has no entries for. Dropped usable memory is
        // reported separately below.
        if (g_memory_map[entry_count].type == MEMORY_TYPE_USABLE) {
            total_memory += g_memory_map[entry_count].length;
        }

        entry_count++;
    }

    g_boot_info.memory_map_address = (UINT64)g_memory_map;
    g_boot_info.memory_map_entries = entry_count;
    g_boot_info.memory_map_entry_size = sizeof(memory_map_entry_t);
    g_boot_info.total_memory = total_memory;
    g_boot_info.memory_map_dropped = (UINT64)dropped_count;

    DPRINT(L"      Memory map: %u entries, %lu MB usable\r\n",
          entry_count, total_memory / (1024 * 1024));
    DPRINT(L"      UEFI descriptors: %d  recorded: %d  cap: %d\r\n",
          (int)descriptor_count, (int)entry_count, (int)MAX_MEMORY_MAP_ENTRIES);

    if (dropped_count > 0) {
        // The kernel CANNOT be told: boot_info_t is kept in sync with
        // kernel/boot_info.h only by convention, with no shared header, so
        // claiming one of the reserved[] slots means editing both files in
        // lockstep or corrupting every field after it. The UEFI console before
        // ExitBootServices is the one channel available here that costs nothing
        // and cannot break the boot, so the truncation is at least LOUD.
        Print(L"\r\n");
        Print(L"  *** WARNING: UEFI MEMORY MAP TRUNCATED ***\r\n");
        Print(L"  %d of %d descriptors were DROPPED (cap %d).\r\n",
              (int)dropped_count, (int)descriptor_count,
              (int)MAX_MEMORY_MAP_ENTRIES);
        Print(L"  %lu MB of USABLE memory is invisible to the kernel.\r\n",
              dropped_usable / (1024 * 1024));
        Print(L"  Raise MAX_MEMORY_MAP_ENTRIES in uefi/bootloader.c.\r\n");
        Print(L"\r\n");
    }
}

// ============================================================================
// Pre-kernel boot "bong" (macOS-style power-on chime)
//
// Self-contained HD Audio output driver that runs INSIDE the UEFI boot-services
// environment, BEFORE the kernel is loaded, so a chime sounds right at power-on
// like a Mac. It is a faithful port of the kernel's generic HDA driver
// (drivers/hda.c, v1.79, including the #71 SET_STREAM=0x706<<8 fix) with these
// substitutions for the loader environment:
//   * HDA controller discovery via DIRECT PCI config I/O (ports 0xCF8/0xCFC),
//     since there is no PCI subsystem in the loader.
//   * DMA buffers (CORB/RIRB/BDL/PCM) via BS->AllocatePages (identity-mapped in
//     EFI, so phys == virt).
//   * All timing via BS->Stall(microseconds) (no kernel timer).
// The whole thing is best-effort: ANY failure (no controller, no codec, no
// output path, timeout) skips cleanly and boot proceeds to load the kernel. A
// broken chime must NEVER prevent the OS from booting.
// ============================================================================

// ---- Port I/O (ring 0 in EFI) ----
static inline void chime_outl(UINT16 port, UINT32 val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline UINT32 chime_inl(UINT16 port) {
    UINT32 r;
    __asm__ volatile("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

// ---- PCI config space ----
static UINT32 pci_cfg_read(UINT8 bus, UINT8 slot, UINT8 func, UINT8 off) {
    UINT32 addr = 0x80000000u | ((UINT32)bus << 16) | ((UINT32)slot << 11) |
                  ((UINT32)func << 8) | (off & 0xFC);
    chime_outl(0xCF8, addr);
    return chime_inl(0xCFC);
}
static void pci_cfg_write(UINT8 bus, UINT8 slot, UINT8 func, UINT8 off, UINT32 val) {
    UINT32 addr = 0x80000000u | ((UINT32)bus << 16) | ((UINT32)slot << 11) |
                  ((UINT32)func << 8) | (off & 0xFC);
    chime_outl(0xCF8, addr);
    chime_outl(0xCFC, val);
}

// ---- HDA register / verb constants (subset of drivers/hda.h) ----
#define HDA_REG_GCTL        0x08
#define HDA_REG_STATESTS    0x0E
#define HDA_REG_INTCTL      0x20
#define HDA_REG_GCAP        0x00
#define HDA_REG_CORBLBASE   0x40
#define HDA_REG_CORBUBASE   0x44
#define HDA_REG_CORBWP      0x48
#define HDA_REG_CORBRP      0x4A
#define HDA_REG_CORBCTL     0x4C
#define HDA_REG_CORBSIZE    0x4E
#define HDA_REG_RIRBLBASE   0x50
#define HDA_REG_RIRBUBASE   0x54
#define HDA_REG_RIRBWP      0x58
#define HDA_REG_RINTCNT     0x5A
#define HDA_REG_RIRBCTL     0x5C
#define HDA_REG_RIRBSTS     0x5D
#define HDA_REG_RIRBSIZE    0x5E
#define HDA_REG_SD_BASE     0x80
#define HDA_REG_SD_SIZE     0x20
#define HDA_SD_CTL          0x00
#define HDA_SD_STS          0x03
#define HDA_SD_LPIB         0x04
#define HDA_SD_CBL          0x08
#define HDA_SD_LVI          0x0C
#define HDA_SD_FMT          0x12
#define HDA_SD_BDPL         0x18
#define HDA_SD_BDPU         0x1C

#define HDA_GCTL_CRST       (1u << 0)
#define HDA_CORBCTL_RUN     (1u << 1)
#define HDA_RIRBCTL_RUN     (1u << 1)
#define HDA_SD_CTL_SRST     (1u << 0)
#define HDA_SD_CTL_RUN      (1u << 1)
#define HDA_SD_CTL_IOCE     (1u << 2)
#define HDA_SD_STS_BCIS     (1u << 2)
#define HDA_SD_STS_FIFOE    (1u << 3)
#define HDA_SD_STS_DESE     (1u << 4)
#define HDA_SD_CTL_STRM_MASK (0xFu << 20)

#define HDA_VERB_GET_PARAM          (0xF00u << 8)
#define HDA_VERB_SET_CONV_FMT       (0x200u << 8)
#define HDA_VERB_SET_CONN_SELECT    (0x701u << 8)
#define HDA_VERB_GET_CONN_LIST      (0xF02u << 8)
#define HDA_VERB_SET_PS             (0x705u << 8)
#define HDA_VERB_SET_PIN_CTL        (0x707u << 8)
#define HDA_VERB_GET_CONFIG_DEF     (0xF1Cu << 8)
#define HDA_VERB_SET_AMP_GAIN       (0x300u << 8)
#define HDA_VERB_SET_EAPD           (0x70Cu << 8)
// #71 FIX: 12-bit verb 0x706 (payload [7:4]=stream tag, [3:0]=channel). The old
// bogus 0x20000<<8 corrupted the NID field so the DAC stream tag was never set
// and output DMA never ran. This is the core fix, preserved here in the loader.
#define HDA_VERB_SET_STREAM         (0x706u << 8)

#define HDA_PARAM_VENDOR_ID         0x00
#define HDA_PARAM_NODE_COUNT        0x04
#define HDA_PARAM_FG_TYPE           0x05
#define HDA_PARAM_AUDIO_WIDGET_CAP  0x09
#define HDA_PARAM_PIN_CAP           0x0C
#define HDA_PARAM_AMP_IN_CAP        0x0D
#define HDA_PARAM_AMP_OUT_CAP       0x12
#define HDA_PARAM_CONN_LIST_LEN     0x0E

#define HDA_WIDGET_TYPE_MASK        (0xFu << 20)
#define HDA_WIDGET_TYPE_OUTPUT      (0x0u << 20)
#define HDA_WIDGET_TYPE_MIXER       (0x2u << 20)
#define HDA_WIDGET_TYPE_SELECTOR    (0x3u << 20)
#define HDA_WIDGET_TYPE_PIN         (0x4u << 20)

#define HDA_PIN_OUT_EN              (1u << 6)
#define HDA_AMP_SET_OUTPUT          (1u << 15)
#define HDA_AMP_SET_INPUT           (1u << 14)
#define HDA_AMP_SET_LEFT            (1u << 13)
#define HDA_AMP_SET_RIGHT           (1u << 12)
#define HDA_AMP_SET_INDEX_MASK      (0xFu << 8)

#define CHIME_CORB_SIZE   256
#define CHIME_RIRB_SIZE   256
#define CHIME_NUM_BDL     32
// 1.5 s of 48 kHz / 16-bit / stereo PCM = 72000 frames * 4 bytes = 288000 bytes.
#define CHIME_FRAMES      72000u
#define CHIME_PCM_BYTES   (CHIME_FRAMES * 4u)

// Parsed output route for the winning codec.
typedef struct {
    UINT8  cad;
    UINT16 vendor_id, device_id;
    UINT8  fg_nid, start_nid, num_nodes;
    UINT8  dac_nid, out_pin_nid;
    UINT8  route_mix_nid, route_mix_conn, route_pin_conn, route_mix_is_sel;
    UINT8  default_device, is_analog;
    int    route_score;
    // #390 CS4208 stereo: parallel output routes (the two fixed speaker pins on
    // the Apple Cirrus codec: DAC10->Pin29 left, DAC11->Pin30 right). Route 0 is
    // mirrored into the single fields above; the DMA stream/format loop drives
    // every route DAC. Single-output codecs (QEMU line-out) have num_out_routes=1.
#define CHIME_MAX_ROUTES 4
    UINT8  num_out_routes;
    UINT8  route_dac[CHIME_MAX_ROUTES];
    UINT8  route_pin[CHIME_MAX_ROUTES];
    UINT8  route_mixn[CHIME_MAX_ROUTES];
    UINT8  route_mixc[CHIME_MAX_ROUTES];
    UINT8  route_pinc[CHIME_MAX_ROUTES];
    UINT8  route_mixsel[CHIME_MAX_ROUTES];
    UINT8  route_dev[CHIME_MAX_ROUTES];
} chime_codec_t;

// ---- Loader-global HDA state ----
static volatile UINT8 *g_hda_mmio;
static UINT32 *g_corb;              // CORB (256 x 4 bytes)
static void   *g_rirb;              // RIRB (256 x 8 bytes)
static UINT16  g_corb_wp;
static UINT16  g_rirb_rp;

#define STALL(us) uefi_call_wrapper(BS->Stall, 1, (UINTN)(us))

static inline UINT8  hda_r8(UINT32 o)  { return g_hda_mmio[o]; }
static inline UINT16 hda_r16(UINT32 o) { return *(volatile UINT16 *)(g_hda_mmio + o); }
static inline UINT32 hda_r32(UINT32 o) { return *(volatile UINT32 *)(g_hda_mmio + o); }
static inline void hda_w8(UINT32 o, UINT8 v)   { g_hda_mmio[o] = v; }
static inline void hda_w16(UINT32 o, UINT16 v) { *(volatile UINT16 *)(g_hda_mmio + o) = v; }
static inline void hda_w32(UINT32 o, UINT32 v) { *(volatile UINT32 *)(g_hda_mmio + o) = v; }

static inline UINT32 sd_off(UINT8 s) { return HDA_REG_SD_BASE + (s * HDA_REG_SD_SIZE); }

// ---- Controller reset ----
static int hda_reset_controller(void) {
    UINT32 gctl = hda_r32(HDA_REG_GCTL);
    gctl &= ~HDA_GCTL_CRST;
    hda_w32(HDA_REG_GCTL, gctl);
    for (int i = 0; i < 100; i++) {
        if ((hda_r32(HDA_REG_GCTL) & HDA_GCTL_CRST) == 0) break;
        STALL(100);
    }
    gctl |= HDA_GCTL_CRST;
    hda_w32(HDA_REG_GCTL, gctl);
    for (int i = 0; i < 100; i++) {
        if (hda_r32(HDA_REG_GCTL) & HDA_GCTL_CRST) { STALL(1000); return 1; }
        STALL(100);
    }
    return 0;
}

// ---- CORB/RIRB command ring ----
static int hda_setup_corb_rirb(UINT64 corb_phys, UINT64 rirb_phys) {
    hda_w8(HDA_REG_CORBCTL, 0);
    for (int i = 0; i < 100 && (hda_r8(HDA_REG_CORBCTL) & HDA_CORBCTL_RUN); i++) STALL(100);
    hda_w8(HDA_REG_RIRBCTL, 0);
    for (int i = 0; i < 100 && (hda_r8(HDA_REG_RIRBCTL) & HDA_RIRBCTL_RUN); i++) STALL(100);

    hda_w32(HDA_REG_CORBLBASE, (UINT32)corb_phys);
    hda_w32(HDA_REG_CORBUBASE, (UINT32)(corb_phys >> 32));
    hda_w32(HDA_REG_RIRBLBASE, (UINT32)rirb_phys);
    hda_w32(HDA_REG_RIRBUBASE, (UINT32)(rirb_phys >> 32));

    hda_w8(HDA_REG_CORBSIZE, (hda_r8(HDA_REG_CORBSIZE) & 0xFC) | 0x02);
    hda_w8(HDA_REG_RIRBSIZE, (hda_r8(HDA_REG_RIRBSIZE) & 0xFC) | 0x02);

    hda_w16(HDA_REG_CORBRP, 0x8000);
    for (int i = 0; i < 100; i++) { if (hda_r16(HDA_REG_CORBRP) & 0x8000) break; STALL(100); }
    hda_w16(HDA_REG_CORBRP, 0);
    for (int i = 0; i < 100; i++) { if ((hda_r16(HDA_REG_CORBRP) & 0x8000) == 0) break; STALL(100); }

    hda_w16(HDA_REG_CORBWP, 0);
    g_corb_wp = 0;
    hda_w16(HDA_REG_RIRBWP, 0x8000);
    g_rirb_rp = 0;
    hda_w16(HDA_REG_RINTCNT, 0xFF);

    hda_w8(HDA_REG_CORBCTL, HDA_CORBCTL_RUN);
    hda_w8(HDA_REG_RIRBCTL, HDA_RIRBCTL_RUN);
    STALL(100);
    return 1;
}

// Send a codec command and read its response (DMA CORB/RIRB path; QEMU-safe).
static UINT32 hda_cmd(UINT8 cad, UINT8 nid, UINT32 verb) {
    UINT32 cmd = ((UINT32)cad << 28) | ((UINT32)nid << 20) | verb;
    UINT16 wp = (g_corb_wp + 1) % CHIME_CORB_SIZE;
    g_corb[wp] = cmd;
    g_corb_wp = wp;
    hda_w16(HDA_REG_CORBWP, wp);
    for (int i = 0; i < 1000; i++) {
        UINT16 rwp = hda_r16(HDA_REG_RIRBWP);
        if (rwp != g_rirb_rp) {
            g_rirb_rp = (g_rirb_rp + 1) % CHIME_RIRB_SIZE;
            UINT32 resp = ((volatile UINT32 *)g_rirb)[g_rirb_rp * 2];
            hda_w8(HDA_REG_RIRBSTS, 0x05);
            return resp;
        }
        STALL(10);
    }
    return 0xFFFFFFFF;
}

// ---- Generic widget-graph parser (port of hda.c #71) ----
static int hda_get_connections(UINT8 cad, UINT8 nid, UINT8 *out, int max) {
    UINT32 p = hda_cmd(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_CONN_LIST_LEN);
    if (p == 0xFFFFFFFF) return 0;
    int len = p & 0x7F;
    int longform = (p & 0x80) != 0;
    int count = 0, prev = -1;
    for (int i = 0; i < len && count < max; ) {
        UINT32 resp = hda_cmd(cad, nid, HDA_VERB_GET_CONN_LIST | (i & 0xFF));
        int per = longform ? 2 : 4;
        for (int k = 0; k < per && i < len && count < max; k++, i++) {
            UINT32 ent = longform ? ((resp >> (k * 16)) & 0xFFFF) : ((resp >> (k * 8)) & 0xFF);
            int range = longform ? (ent & 0x8000) : (ent & 0x80);
            int val   = longform ? (ent & 0x7FFF) : (ent & 0x7F);
            if (range && prev >= 0) {
                for (int v = prev + 1; v <= val && count < max; v++) out[count++] = (UINT8)v;
            } else {
                out[count++] = (UINT8)val;
            }
            prev = val;
        }
    }
    return count;
}

static UINT32 hda_widget_type(UINT8 cad, UINT8 nid) {
    return hda_cmd(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AUDIO_WIDGET_CAP) & HDA_WIDGET_TYPE_MASK;
}

static int hda_pin_output_score(UINT8 cad, UINT8 nid, UINT8 *out_dev, UINT8 *out_analog) {
    UINT32 pincap = hda_cmd(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_PIN_CAP);
    if (pincap == 0xFFFFFFFF) return -1;
    if (!(pincap & 0x10)) return -1;                 // bit 4 = Output Capable
    UINT32 cfg = hda_cmd(cad, nid, HDA_VERB_GET_CONFIG_DEF);
    UINT8 conn = (cfg >> 30) & 0x3;
    UINT8 dev  = (cfg >> 20) & 0xF;
    *out_dev = dev;
    int score;
    switch (dev) {
        case 0x1: score = 100; *out_analog = 1; break; // Speaker
        case 0x0: score = 90;  *out_analog = 1; break; // Line Out
        case 0x2: score = 80;  *out_analog = 1; break; // HP Out
        case 0x4: score = 45;  *out_analog = 0; break; // SPDIF Out
        case 0x5: score = 40;  *out_analog = 0; break; // Digital Other Out
        default:  score = 20;  *out_analog = 0; break;
    }
    if (conn == 1) score -= 50;
    return score;
}

// Resolve pin -> DAC (direct or via one selector/mixer). Returns 1 on success.
static int chime_resolve_route(UINT8 cad, UINT8 pin, UINT8 *dac, UINT8 *pin_conn,
                               UINT8 *mix_nid, UINT8 *mix_conn, UINT8 *mix_is_sel) {
    UINT8 conns[16];
    int n = hda_get_connections(cad, pin, conns, 16);
    for (int i = 0; i < n; i++) {
        if (hda_widget_type(cad, conns[i]) == HDA_WIDGET_TYPE_OUTPUT) {
            *dac = conns[i]; *pin_conn = (UINT8)i; *mix_nid = 0; *mix_conn = 0; *mix_is_sel = 0;
            return 1;
        }
    }
    for (int i = 0; i < n; i++) {
        UINT32 t = hda_widget_type(cad, conns[i]);
        if (t != HDA_WIDGET_TYPE_MIXER && t != HDA_WIDGET_TYPE_SELECTOR) continue;
        UINT8 sub[16];
        int m = hda_get_connections(cad, conns[i], sub, 16);
        for (int j = 0; j < m; j++) {
            if (hda_widget_type(cad, sub[j]) == HDA_WIDGET_TYPE_OUTPUT) {
                *dac = sub[j]; *pin_conn = (UINT8)i; *mix_nid = conns[i];
                *mix_conn = (UINT8)j; *mix_is_sel = (t == HDA_WIDGET_TYPE_SELECTOR) ? 1 : 0;
                return 1;
            }
        }
    }
    return 0;
}

static void chime_store_route(chime_codec_t *c, int idx, UINT8 pin, UINT8 dev,
                              UINT8 dac, UINT8 pin_conn, UINT8 mix_nid,
                              UINT8 mix_conn, UINT8 mix_is_sel) {
    c->route_pin[idx] = pin;   c->route_dev[idx] = dev;   c->route_dac[idx] = dac;
    c->route_pinc[idx] = pin_conn; c->route_mixn[idx] = mix_nid;
    c->route_mixc[idx] = mix_conn; c->route_mixsel[idx] = mix_is_sel;
    if (idx == 0) {
        c->dac_nid = dac; c->out_pin_nid = pin; c->default_device = dev;
        c->route_pin_conn = pin_conn; c->route_mix_nid = mix_nid;
        c->route_mix_conn = mix_conn; c->route_mix_is_sel = mix_is_sel;
    }
}

// #390 CS4208 stereo: prefer fixed-function SPEAKER pins (device=Speaker,
// connectivity=Fixed) and drive ALL of them for stereo; ignore the codec's many
// dead conn=None Line-Out pins. Fall back to the single best-scoring pin only if
// no fixed speakers exist (QEMU line-out).
static int hda_parse_codec(UINT8 cad, chime_codec_t *c) {
    SetMem(c, sizeof(*c), 0);
    c->cad = cad;
    c->route_score = -1;

    UINT32 vendor = hda_cmd(cad, 0, HDA_VERB_GET_PARAM | HDA_PARAM_VENDOR_ID);
    if (vendor == 0 || vendor == 0xFFFFFFFF) return -1;   // no VendorID: skip codec
    c->vendor_id = (vendor >> 16) & 0xFFFF;
    c->device_id = vendor & 0xFFFF;

    UINT32 nc = hda_cmd(cad, 0, HDA_VERB_GET_PARAM | HDA_PARAM_NODE_COUNT);
    UINT8 fg_start = (nc >> 16) & 0xFF;
    UINT8 fg_count = nc & 0xFF;

    UINT8 afg = 0;
    for (int i = 0; i < fg_count && i < 8; i++) {
        UINT8 fgn = fg_start + i;
        UINT32 fgt = hda_cmd(cad, fgn, HDA_VERB_GET_PARAM | HDA_PARAM_FG_TYPE);
        if ((fgt & 0x7F) == 0x01) { afg = fgn; break; }
    }
    if (afg == 0) afg = fg_start;
    c->fg_nid = afg;

    UINT32 wnc = hda_cmd(cad, afg, HDA_VERB_GET_PARAM | HDA_PARAM_NODE_COUNT);
    c->start_nid = (wnc >> 16) & 0xFF;
    c->num_nodes = wnc & 0xFF;
    if (c->num_nodes == 0 || c->num_nodes > 128) return -1;

    // Pass 1: collect fixed-function speaker pins.
    int nroutes = 0;
    for (int nid = c->start_nid;
         nid < c->start_nid + c->num_nodes && nroutes < CHIME_MAX_ROUTES; nid++) {
        if (hda_widget_type(cad, nid) != HDA_WIDGET_TYPE_PIN) continue;
        UINT32 pincap = hda_cmd(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_PIN_CAP);
        if (pincap == 0xFFFFFFFF || !(pincap & 0x10)) continue;
        UINT32 cfg = hda_cmd(cad, nid, HDA_VERB_GET_CONFIG_DEF);
        UINT8 conn = (cfg >> 30) & 0x3;
        UINT8 dev  = (cfg >> 20) & 0xF;
        if (!(dev == 0x1 && conn == 0x2)) continue;      // only Speaker + Fixed
        UINT8 dac, pin_conn, mix_nid, mix_conn, mix_is_sel;
        if (!chime_resolve_route(cad, (UINT8)nid, &dac, &pin_conn, &mix_nid, &mix_conn, &mix_is_sel))
            continue;
        chime_store_route(c, nroutes, (UINT8)nid, dev, dac, pin_conn, mix_nid, mix_conn, mix_is_sel);
        nroutes++;
    }
    if (nroutes > 0) {
        c->num_out_routes = (UINT8)nroutes;
        c->is_analog = 1;
        c->route_score = 100;
        return c->route_score;
    }

    // Pass 2 (fallback): single best-scoring pin.
    int best_pin = -1, best_score = -1;
    UINT8 best_dev = 0, best_analog = 0;
    for (int nid = c->start_nid; nid < c->start_nid + c->num_nodes; nid++) {
        if (hda_widget_type(cad, nid) != HDA_WIDGET_TYPE_PIN) continue;
        UINT8 dev = 0, analog = 0;
        int s = hda_pin_output_score(cad, nid, &dev, &analog);
        if (s > best_score) { best_score = s; best_pin = nid; best_dev = dev; best_analog = analog; }
    }
    if (best_pin < 0) return -1;

    UINT8 dac, pin_conn, mix_nid, mix_conn, mix_is_sel;
    if (!chime_resolve_route(cad, (UINT8)best_pin, &dac, &pin_conn, &mix_nid, &mix_conn, &mix_is_sel))
        return -1;
    chime_store_route(c, 0, (UINT8)best_pin, best_dev, dac, pin_conn, mix_nid, mix_conn, mix_is_sel);
    c->num_out_routes = 1;
    c->is_analog = best_analog;
    c->route_score = best_score;
    return best_score;
}

static void hda_set_out_amp(UINT8 cad, UINT8 nid) {
    UINT32 cap = hda_cmd(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AMP_OUT_CAP);
    if (cap == 0 || cap == 0xFFFFFFFF) return;
    UINT8 nsteps = (cap >> 8) & 0x7F;
    UINT8 offset = cap & 0x7F;
    UINT8 gain = offset ? offset : nsteps;
    if (gain > nsteps) gain = nsteps;
    hda_cmd(cad, nid, HDA_VERB_SET_AMP_GAIN |
            HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT | gain);
}

static void hda_set_in_amp(UINT8 cad, UINT8 nid, UINT8 index) {
    UINT32 cap = hda_cmd(cad, nid, HDA_VERB_GET_PARAM | HDA_PARAM_AMP_IN_CAP);
    if (cap == 0 || cap == 0xFFFFFFFF) return;
    UINT8 nsteps = (cap >> 8) & 0x7F;
    UINT8 offset = cap & 0x7F;
    UINT8 gain = offset ? offset : nsteps;
    if (gain > nsteps) gain = nsteps;
    hda_cmd(cad, nid, HDA_VERB_SET_AMP_GAIN |
            HDA_AMP_SET_INPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
            ((index << 8) & HDA_AMP_SET_INDEX_MASK) | gain);
}

static void hda_configure_codec(chime_codec_t *cc) {
    UINT8 cad = cc->cad;
    int nroutes = cc->num_out_routes ? cc->num_out_routes : 1;

    hda_cmd(cad, cc->fg_nid, HDA_VERB_SET_PS | 0x00);
    STALL(10000);

    for (int r = 0; r < nroutes; r++) {
        UINT8 dac = cc->route_dac[r], pin = cc->route_pin[r];
        UINT8 mix = cc->route_mixn[r], dev = cc->route_dev[r];

        hda_cmd(cad, dac, HDA_VERB_SET_PS | 0x00);
        hda_cmd(cad, pin, HDA_VERB_SET_PS | 0x00);
        if (mix) hda_cmd(cad, mix, HDA_VERB_SET_PS | 0x00);
        STALL(1000);

        // Same stream tag (1), one stereo channel per route (0=left, 1=right).
        hda_cmd(cad, dac, HDA_VERB_SET_STREAM | (1 << 4) | (r & 0x0F));
        hda_set_out_amp(cad, dac);

        if (mix) {
            if (cc->route_mixsel[r])
                hda_cmd(cad, mix, HDA_VERB_SET_CONN_SELECT | cc->route_mixc[r]);
            hda_set_in_amp(cad, mix, cc->route_mixc[r]);
            hda_set_out_amp(cad, mix);
        }

        hda_cmd(cad, pin, HDA_VERB_SET_CONN_SELECT | cc->route_pinc[r]);
        {
            UINT8 pinctl = HDA_PIN_OUT_EN;
            if (dev == 0x2) pinctl |= 0x80;   // HP amp enable
            hda_cmd(cad, pin, HDA_VERB_SET_PIN_CTL | pinctl);
        }
        hda_cmd(cad, pin, HDA_VERB_SET_EAPD | 0x02);
        hda_set_out_amp(cad, pin);
    }
}

// ---- Bong synthesis: decaying major chord (warm, Mac-like) ----
static double chime_sin(double x) {
    const double PI = 3.14159265358979323846;
    // Range-reduce to [-PI, PI].
    while (x >  PI) x -= 2.0 * PI;
    while (x < -PI) x += 2.0 * PI;
    double x2 = x * x;
    // Taylor series to x^11 (accurate over [-PI, PI]).
    return x * (1.0 + x2 * (-1.0/6.0 + x2 * (1.0/120.0 + x2 * (-1.0/5040.0 +
               x2 * (1.0/362880.0 + x2 * (-1.0/39916800.0))))));
}

static void chime_synth(INT16 *buf, UINT32 frames, UINT32 fs) {
    const double PI = 3.14159265358979323846;
    // A warm C major chord: root, major third, fifth, octave, plus a soft
    // upper fifth for shimmer. Frequencies in Hz.
    double freqs[5] = { 130.81, 164.81, 196.00, 261.63, 392.00 };
    double gains[5] = { 1.00,   0.90,   0.80,   0.70,   0.40   };
    double phase[5] = { 0, 0, 0, 0, 0 };
    double dphi[5];
    for (int p = 0; p < 5; p++) dphi[p] = 2.0 * PI * freqs[p] / (double)fs;

    double env = 0.0;                                // start silent -> soft attack
    double attack_inc = 1.0 / (0.030 * (double)fs);  // ~30 ms attack ramp
    double decay = 0.99996;                          // ~0.5 s exponential decay tail
    int attacking = 1;

    for (UINT32 i = 0; i < frames; i++) {
        if (attacking) { env += attack_inc; if (env >= 1.0) { env = 1.0; attacking = 0; } }
        else env *= decay;

        double s = 0.0;
        for (int p = 0; p < 5; p++) {
            s += gains[p] * chime_sin(phase[p]);
            phase[p] += dphi[p];
            if (phase[p] > PI) phase[p] -= 2.0 * PI;
        }
        double v = env * s * 0.22;                   // headroom below full scale
        if (v >  1.0) v =  1.0;
        if (v < -1.0) v = -1.0;
        INT16 sample = (INT16)(v * 30000.0);
        buf[2 * i]     = sample;
        buf[2 * i + 1] = sample;
    }
}

// Enable PCI memory space + bus mastering on an HDA controller.
static void pci_enable_bm(UINT8 bus, UINT8 slot, UINT8 func) {
    UINT32 cmd = pci_cfg_read(bus, slot, func, 0x04);
    pci_cfg_write(bus, slot, func, 0x04, cmd | 0x06);   // bit1 mem space, bit2 bus master
}

// Read BAR0 MMIO base of an HDA controller (handles 32/64-bit memory BARs).
static UINT64 pci_hda_bar0(UINT8 bus, UINT8 slot, UINT8 func) {
    UINT32 bar0 = pci_cfg_read(bus, slot, func, 0x10);
    if (bar0 & 0x1) return 0;                            // I/O BAR: not MMIO
    UINT64 base = bar0 & ~0xFULL;
    if (((bar0 >> 1) & 0x3) == 0x2) {                    // 64-bit BAR
        UINT32 bar1 = pci_cfg_read(bus, slot, func, 0x14);
        base |= ((UINT64)bar1) << 32;
    }
    return base;
}

// Bring a controller up on g_hda_mmio and start its command ring.
static int hda_bring_up(UINT64 bar, UINT64 corb_phys, UINT64 rirb_phys) {
    if (bar == 0) return 0;
    g_hda_mmio = (volatile UINT8 *)(UINTN)bar;
    if (!hda_reset_controller()) return 0;
    if (!hda_setup_corb_rirb(corb_phys, rirb_phys)) return 0;
    STALL(1000);
    return 1;
}

// Program + start the output stream over the PCM cyclic buffer.
static void hda_start_stream(UINT8 stream, UINT64 bdl_phys) {
    UINT32 ctl = hda_r32(sd_off(stream) + HDA_SD_CTL);
    ctl &= ~HDA_SD_CTL_RUN;
    hda_w32(sd_off(stream) + HDA_SD_CTL, ctl);
    for (int i = 0; i < 100; i++) { if ((hda_r32(sd_off(stream) + HDA_SD_CTL) & HDA_SD_CTL_RUN) == 0) break; STALL(100); }

    ctl |= HDA_SD_CTL_SRST;
    hda_w32(sd_off(stream) + HDA_SD_CTL, ctl);
    STALL(100);
    for (int i = 0; i < 100; i++) { if (hda_r32(sd_off(stream) + HDA_SD_CTL) & HDA_SD_CTL_SRST) break; STALL(100); }
    ctl &= ~HDA_SD_CTL_SRST;
    hda_w32(sd_off(stream) + HDA_SD_CTL, ctl);
    for (int i = 0; i < 100; i++) { if ((hda_r32(sd_off(stream) + HDA_SD_CTL) & HDA_SD_CTL_SRST) == 0) break; STALL(100); }

    hda_w8(sd_off(stream) + HDA_SD_STS, HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE | HDA_SD_STS_DESE);
    hda_w16(sd_off(stream) + HDA_SD_FMT, 0x0011);        // 48kHz, 16-bit, 2ch
    hda_w32(sd_off(stream) + HDA_SD_CBL, CHIME_PCM_BYTES);
    hda_w16(sd_off(stream) + HDA_SD_LVI, CHIME_NUM_BDL - 1);
    hda_w32(sd_off(stream) + HDA_SD_BDPL, (UINT32)bdl_phys);
    hda_w32(sd_off(stream) + HDA_SD_BDPU, (UINT32)(bdl_phys >> 32));

    ctl = HDA_SD_CTL_IOCE | ((1u << 20) & HDA_SD_CTL_STRM_MASK);
    hda_w32(sd_off(stream) + HDA_SD_CTL, ctl);
    // Go.
    ctl |= HDA_SD_CTL_RUN;
    hda_w32(sd_off(stream) + HDA_SD_CTL, ctl);
}

// Guarded, best-effort boot chime. Always returns; never blocks the boot.
static void play_boot_chime(void) {
    EFI_STATUS status;
    EFI_PHYSICAL_ADDRESS ctrl_pg = 0, pcm_pg = 0;

    // Allocate control structures (CORB 1KB + RIRB 2KB + BDL 512B fit in 1 page,
    // each 128-byte aligned within the page-aligned allocation) and the PCM DMA
    // buffer. Identity-mapped in EFI, so the returned address is the DMA phys.
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                               EfiBootServicesData, 1, &ctrl_pg);
    if (EFI_ERROR(status)) { DPRINT(L"[chime] alloc failed; skipping\r\n"); return; }
    UINTN pcm_pages = (CHIME_PCM_BYTES + 4095) / 4096;
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                               EfiBootServicesData, pcm_pages, &pcm_pg);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePages, 2, ctrl_pg, 1);
        DPRINT(L"[chime] PCM alloc failed; skipping\r\n");
        return;
    }

    g_corb = (UINT32 *)(UINTN)ctrl_pg;               // offset 0
    g_rirb = (void *)(UINTN)(ctrl_pg + 1024);        // offset 1024
    UINT64 bdl_phys = ctrl_pg + 3072;                // offset 3072
    void *pcm = (void *)(UINTN)pcm_pg;
    SetMem((void *)(UINTN)ctrl_pg, 4096, 0);
    SetMem(pcm, pcm_pages * 4096, 0);

    // Build the BDL: split the cyclic buffer across CHIME_NUM_BDL entries.
    UINT32 seg = CHIME_PCM_BYTES / CHIME_NUM_BDL;     // 288000/32 = 9000 (mult of 4)
    struct { UINT64 addr; UINT32 len; UINT32 ioc; } __attribute__((packed)) *bdl =
        (void *)(UINTN)bdl_phys;
    for (int i = 0; i < CHIME_NUM_BDL; i++) {
        bdl[i].addr = pcm_pg + (UINT64)i * seg;
        bdl[i].len  = seg;
        bdl[i].ioc  = 1;
    }

    // Synthesize the bong into the PCM buffer.
    chime_synth((INT16 *)pcm, CHIME_FRAMES, 48000);

    // Scan PCI for ALL HD Audio controllers (class 0x04, subclass 0x03) and pick
    // the one whose codec has the best analog output path (speaker > line-out >
    // HP > digital). This drives the iMac's Cirrus on the PCH controller rather
    // than the GPU HDMI codec.
    int best_score = -1;
    UINT64 best_bar = 0;
    UINT8  best_bus = 0, best_slot = 0, best_func = 0;
    chime_codec_t best_codec;
    SetMem(&best_codec, sizeof(best_codec), 0);
    int controllers = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            for (int func = 0; func < 8; func++) {
                UINT32 vd = pci_cfg_read(bus, slot, func, 0x00);
                if ((vd & 0xFFFF) == 0xFFFF) { if (func == 0) break; else continue; }
                UINT32 cc = pci_cfg_read(bus, slot, func, 0x08);
                UINT8 cls = (cc >> 24) & 0xFF, sub = (cc >> 16) & 0xFF;
                if (cls != 0x04 || sub != 0x03) {
                    if (func == 0) {
                        UINT32 hdr = pci_cfg_read(bus, slot, func, 0x0C);
                        if (!((hdr >> 16) & 0x80)) break;   // not multifunction
                    }
                    continue;
                }
                controllers++;
                pci_enable_bm(bus, slot, func);
                UINT64 bar = pci_hda_bar0(bus, slot, func);
                if (!hda_bring_up(bar, (UINT64)(UINTN)g_corb, (UINT64)(UINTN)g_rirb))
                    continue;
                UINT16 statests = hda_r16(HDA_REG_STATESTS);
                hda_w16(HDA_REG_STATESTS, statests);
                if (statests == 0) continue;
                for (int cad = 0; cad < 15; cad++) {
                    if (!(statests & (1 << cad))) continue;
                    chime_codec_t c;
                    int s = hda_parse_codec((UINT8)cad, &c);
                    // #390: skip a codec with no VendorID (the iMac GPU-HDA
                    // controller's codec) so we fall through to the PCH Cirrus.
                    if (!c.vendor_id) continue;
                    if (s > best_score) {
                        best_score = s; best_codec = c;
                        best_bar = bar; best_bus = bus; best_slot = slot; best_func = func;
                    }
                }
            }
        }
    }

    if (controllers == 0) {
        DPRINT(L"[chime] no HD Audio controller found; skipping\r\n");
        goto done;
    }
    if (best_score < 0) {
        DPRINT(L"[chime] no usable output path on %d controller(s); skipping\r\n", controllers);
        goto done;
    }

    DPRINT(L"[chime] HDA codec %04x:%04x (%s) DAC=%d PIN=%d on %02x:%02x.%x\r\n",
          best_codec.vendor_id, best_codec.device_id,
          best_codec.is_analog ? L"analog" : L"digital",
          best_codec.dac_nid, best_codec.out_pin_nid, best_bus, best_slot, best_func);

    // Finalize on the winning controller: bring it back up, configure the codec
    // route, set the converter format, and start the output stream.
    pci_enable_bm(best_bus, best_slot, best_func);
    if (!hda_bring_up(best_bar, (UINT64)(UINTN)g_corb, (UINT64)(UINTN)g_rirb)) {
        DPRINT(L"[chime] winner re-init failed; skipping\r\n");
        goto done;
    }
    hda_w16(HDA_REG_STATESTS, hda_r16(HDA_REG_STATESTS));

    UINT16 gcap = hda_r16(HDA_REG_GCAP);
    UINT8 num_oss = (gcap >> 12) & 0xF;
    UINT8 num_iss = (gcap >> 8) & 0xF;
    if (num_oss == 0) { DPRINT(L"[chime] no output streams; skipping\r\n"); goto done; }
    UINT8 stream = num_iss;                              // first output stream index

    hda_configure_codec(&best_codec);
    // Converter format (required for the voice to open) on EVERY route DAC, then
    // start the stream. #390: both Cirrus speaker DACs get the format.
    {
        int nr = best_codec.num_out_routes ? best_codec.num_out_routes : 1;
        for (int r = 0; r < nr; r++)
            hda_cmd(best_codec.cad, best_codec.route_dac[r], HDA_VERB_SET_CONV_FMT | 0x0011);
    }
    hda_start_stream(stream, bdl_phys);

    // Confirm DMA is advancing (LPIB moves), for the no-host-audio case.
    UINT32 lp0 = hda_r32(sd_off(stream) + HDA_SD_LPIB);
    UINT32 lpn = lp0;
    for (int i = 0; i < 40; i++) { STALL(3000); lpn = hda_r32(sd_off(stream) + HDA_SD_LPIB); if (lpn != lp0) break; }
    DPRINT(L"[chime] playing bong: LPIB %u -> %u (%s)\r\n",
          lp0, lpn, (lpn != lp0) ? L"DMA RUNNING" : L"DMA STALLED");

    // Let the chime play (bounded). ~1.45 s so the decayed tail finishes before
    // the cyclic buffer wraps and re-triggers the attack.
    STALL(1450000);

    // Stop the stream cleanly before handing off to the kernel.
    {
        UINT32 ctl = hda_r32(sd_off(stream) + HDA_SD_CTL);
        ctl &= ~HDA_SD_CTL_RUN;
        hda_w32(sd_off(stream) + HDA_SD_CTL, ctl);
        for (int i = 0; i < 100; i++) { if ((hda_r32(sd_off(stream) + HDA_SD_CTL) & HDA_SD_CTL_RUN) == 0) break; STALL(100); }
        hda_w32(HDA_REG_INTCTL, 0);
    }
    DPRINT(L"[chime] done\r\n");

done:
    (void)pcm;
    uefi_call_wrapper(BS->FreePages, 2, pcm_pg, pcm_pages);
    uefi_call_wrapper(BS->FreePages, 2, ctrl_pg, 1);
}

// #QUIETBOOT: THE TWO PRE-KERNEL MARKER FILES, READ ONCE, BEFORE ANYTHING PRINTS.
//
// A NORMAL BOOT IS SILENT. The owner's complaint was that the boot is ugly, and
// the loader's own console is part of what he was looking at: the banner, the
// nine [N/8] step headings and their success lines, the GOP report, the ACPI and
// memory-map lines, the chime brackets and the display-mode enumeration. That is
// roughly thirty-five lines, and it is genuinely on screen: the firmware console
// is the glass until the kernel takes the framebuffer, verified on a screendump
// of a VM boot, not assumed. All of it is now gated behind the SAME
// \boot\DIAG.TXT marker that arms the kernel's boot-stage instrumentation, so
// one file on the ESP turns on both layers and no file turns on neither. A
// second marker for the loader would have been a second thing to forget.
//
// WHAT IS *NOT* GATED, AND THE RULE IS SIMPLE: QUIET MEANS QUIET ON SUCCESS.
// Anything that FAILED, was REFUSED, was REVERTED, was TRUNCATED, or is a
// WARNING prints unconditionally, because at that moment there may be no kernel
// coming to write a log and the screen is the only channel left. That is the
// same principle as boot_stage_report_forever() arming the screen on demand when
// no filesystem mounted: failure outranks quiet. Use DPRINT() for progress and
// success; use Print() for everything that means something is wrong.
//
// WHY BOTH MARKERS ARE READ HERE, TOGETHER, AND EARLY. The banner and the chime
// both happen before [1/8] open_root_dir(), so the DIAG verdict has to exist
// before either. Reading both markers in one place also collapses what used to
// be two independent OpenVolume calls into one, which is the shared-primitive
// rule doing its job rather than a second copy of the same twelve lines.
//
// TWO deliberate properties, both inherited from the ROTATE.TXT precedent:
//
//  1. It opens its OWN volume handle rather than reusing efi_main()'s `root`,
//     because `root` is produced by [1/8] open_root_dir(), which runs after
//     this. A second OpenVolume is legal and the handle is closed again before
//     returning, so nothing is left open across the boot.
//  2. Presence is tested with root->Open, NOT load_file(). A marker file is very
//     likely to be created with `touch`, and load_file() on a 0-byte file ends
//     up calling AllocatePool(0), whose behaviour is not guaranteed across
//     firmwares. A zero-byte marker MUST count as present.
//
// Any error at all leaves both flags at 0, which is the chime playing as usual
// and a quiet console. A firmware quirk can never make a machine noisy; only a
// file someone deliberately put there can.


static int marker_present(EFI_FILE_PROTOCOL *root, CHAR16 *name) {
    EFI_FILE_PROTOCOL *file = NULL;
    EFI_STATUS status = uefi_call_wrapper(root->Open, 5, root, &file, name,
                                          EFI_FILE_MODE_READ, 0);
    int present = (!EFI_ERROR(status) && file) ? 1 : 0;
    if (present) uefi_call_wrapper(file->Close, 1, file);
    return present;
}

static void read_boot_markers(EFI_HANDLE ImageHandle) {
    EFI_FILE_PROTOCOL *root = NULL;
    EFI_STATUS status = open_root_dir(ImageHandle, &root);
    if (EFI_ERROR(status) || !root) return;    // cannot tell -> quiet, chime as usual
    g_diag_console = marker_present(root, L"boot\\DIAG.TXT");
    g_nochime      = marker_present(root, L"boot\\NOCHIME.TXT");
    uefi_call_wrapper(root->Close, 1, root);
}

// Main UEFI entry point
EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    EFI_FILE_PROTOCOL *root;
    void *kernel_buffer;
    UINTN kernel_size;
    Elf64_Ehdr *elf_header;
    Elf64_Phdr *program_headers;
    UINT64 kernel_entry;
    UINTN memory_map_size = 0;
    EFI_MEMORY_DESCRIPTOR *memory_map = NULL;
    UINTN map_key, descriptor_size;
    UINT32 descriptor_version;

    InitializeLib(ImageHandle, SystemTable);

    // Clear boot info
    SetMem(&g_boot_info, sizeof(boot_info_t), 0);
    g_boot_info.magic = BOOT_INFO_MAGIC;

    // #QUIETBOOT: BEFORE THE FIRST Print(). Everything below is gated on the
    // verdict this produces, so it has to exist first. See read_boot_markers().
    read_boot_markers(ImageHandle);

    DPRINT(L"========================================\r\n");
    DPRINT(L"  MayteraOS UEFI Bootloader v3.1 (boot chime)\r\n");
    DPRINT(L"========================================\r\n\r\n");
    if (g_diag_console) g_banner_shown = 1;

    // Pre-kernel boot chime (macOS-style bong). Best-effort: always returns and
    // never blocks the boot, even if no HD Audio hardware is present.
    //
    // ASUS bring-up: gated by \boot\NOCHIME.TXT (see boot_chime_disabled()
    // above) and BRACKETED by markers. The brackets are the point: this runs
    // before [1/8], so without them a machine that dies inside the chime and a
    // machine that never reached [1/8] at all look identical, namely a banner
    // and then nothing. With them, the last line on screen says which.
    if (g_nochime) {
        DPRINT(L"[0/8] boot chime: skipped (NOCHIME.TXT present)\r\n");
    } else {
        DPRINT(L"[0/8] boot chime: starting\r\n");
        play_boot_chime();
        DPRINT(L"[0/8] boot chime: done\r\n");
    }
    DPRINT(L"\r\n");

    // Open root filesystem
    DPRINT(L"[1/8] Opening root filesystem...\r\n");
    status = open_root_dir(ImageHandle, &root);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Failed to open root directory (0x%x)\r\n", status);
        goto error;
    }
    DPRINT(L"      Root filesystem opened successfully\r\n\r\n");

    // #745 (local 102): display rotation. Best-effort, NOT one of the
    // numbered [N/8] steps below - a missing file is the normal case (no
    // rotation configured) and must never fail the boot. \boot\ROTATE.TXT
    // (NOT bare ESP-root \ROTATE.TXT - see blame.md "local 102 /boot/ redirect
    // trap"): on a two-partition ext2-root golden, the KERNEL-side writer
    // (SYS_SET_ROTATION, proc/syscall.c) goes through fat_write_file(), which
    // silently redirects any path that is NOT /boot or /EFI to the ext2 root
    // volume (fs/fat.c fat_path_on_ext2()) - the exact same defect class that
    // once broke /PANIC.TXT and /STAGE.TXT (see fs/panic.c). Living under
    // \boot, the same directory as \boot\kernel.elf below, is the one
    // spelling guaranteed to stay on the FAT ESP on BOTH the UEFI read here and
    // the kernel write, so a value the user sets in Settings is actually the
    // value this loader finds next boot. A single ASCII digit '0'-'3';
    // anything else (missing file, empty file, garbage) is treated as '0' (no
    // rotation) by the kernel too (see fb_init()), so a corrupt marker
    // degrades to "unrotated", never to an undefined rotation.
    g_boot_info.display_rotation = 0;
    {
        void *rbuf = NULL;
        UINTN rsize = 0;
        EFI_STATUS rstatus = load_file(root, L"boot\\ROTATE.TXT", &rbuf, &rsize);
        if (!EFI_ERROR(rstatus) && rbuf && rsize > 0) {
            CHAR8 c = ((CHAR8 *)rbuf)[0];
            if (c >= '0' && c <= '3') {
                g_boot_info.display_rotation = (UINT64)(c - '0');
                DPRINT(L"      Display rotation: %d\r\n\r\n", (int)g_boot_info.display_rotation);
            }
            uefi_call_wrapper(BS->FreePool, 1, rbuf);
        }
    }

    // #QUIETBOOT: hand the arming verdict to the kernel.
    //
    // The kernel takes the firmware framebuffer in the FIRST executable statement
    // of kernel_main, long before any filesystem is mounted, so it cannot read a
    // marker file in time to decide whether to paint on it. The loader can, and
    // did, in read_boot_markers() above; this carries the answer across.
    //
    // The loader no longer PRINTS its verdict on a quiet boot, and that is not a
    // hole in the self-reporting. The kernel's "[DIAG] ..." line in /BOOTLOG.TXT
    // is written from boot_info->diag_flags, i.e. from exactly the value the
    // loader sent, and the kernel then cross-checks that value against the file
    // it can see with its OWN FAT driver once the ESP is mounted. Two independent
    // readers still disagree loudly if the hand-off breaks; what was dropped was
    // a third statement of the same fact, on the one channel that scrolls away.
    g_boot_info.diag_flags = g_diag_console ? 0x1ULL : 0ULL;   // BOOT_DIAG_SCREEN
    DPRINT(L"      Boot diagnostics: %s\r\n\r\n",
           g_diag_console ? L"ARMED (\\boot\\DIAG.TXT present)"
                          : L"quiet (no \\boot\\DIAG.TXT)");

    // Load kernel file
    DPRINT(L"[2/8] Loading /boot/kernel.elf...\r\n");
    status = load_file(root, L"boot\\kernel.elf", &kernel_buffer, &kernel_size);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Failed to load kernel.elf (0x%x)\r\n", status);
        Print(L"       Make sure /boot/kernel.elf exists on the disk\r\n");
        goto error;
    }
    DPRINT(L"      Kernel loaded: %d bytes\r\n\r\n", kernel_size);

    // Verify ELF header
    DPRINT(L"[3/8] Parsing ELF header...\r\n");
    elf_header = (Elf64_Ehdr*)kernel_buffer;

    if (elf_header->e_ident[0] != 0x7F ||
        elf_header->e_ident[1] != 'E' ||
        elf_header->e_ident[2] != 'L' ||
        elf_header->e_ident[3] != 'F') {
        Print(L"ERROR: Not a valid ELF file\r\n");
        status = EFI_INVALID_PARAMETER;
        goto error;
    }

    if (elf_header->e_ident[EI_CLASS] != ELFCLASS64) {
        Print(L"ERROR: Not a 64-bit ELF file\r\n");
        status = EFI_INVALID_PARAMETER;
        goto error;
    }

    kernel_entry = elf_header->e_entry;
    DPRINT(L"      ELF header valid\r\n");
    DPRINT(L"      Entry point: 0x%lx\r\n", kernel_entry);
    DPRINT(L"      Program headers: %d\r\n\r\n", elf_header->e_phnum);

    // Load program segments
    DPRINT(L"[4/8] Loading kernel segments...\r\n");
    program_headers = (Elf64_Phdr*)((UINT8*)kernel_buffer + elf_header->e_phoff);

    UINT64 kernel_min_addr = 0xFFFFFFFFFFFFFFFF;
    UINT64 kernel_max_addr = 0;

    for (int i = 0; i < elf_header->e_phnum; i++) {
        if (program_headers[i].p_type == PT_LOAD) {
            UINT64 vaddr = program_headers[i].p_vaddr;
            UINT64 memsz = program_headers[i].p_memsz;
            UINT64 filesz = program_headers[i].p_filesz;
            UINT64 offset = program_headers[i].p_offset;

            DPRINT(L"      Segment %d: vaddr=0x%lx size=%d bytes\r\n", i, vaddr, memsz);

            // Track kernel memory range
            if (vaddr < kernel_min_addr) kernel_min_addr = vaddr;
            if (vaddr + memsz > kernel_max_addr) kernel_max_addr = vaddr + memsz;

            // Allocate memory at virtual address
            EFI_PHYSICAL_ADDRESS phys_addr = vaddr;
            UINTN pages = (memsz + 4095) / 4096;
            status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAddress,
                                      EfiLoaderData, pages, &phys_addr);
            if (EFI_ERROR(status)) {
                // Try allocating anywhere
                status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                                          EfiLoaderData, pages, &phys_addr);
                if (EFI_ERROR(status)) {
                    Print(L"ERROR: Failed to allocate memory for segment\r\n");
                    goto error;
                }
                Print(L"      (Allocated at 0x%lx instead)\r\n", phys_addr);
            }

            // Copy segment data
            CopyMem((void*)phys_addr, (UINT8*)kernel_buffer + offset, filesz);

            // Zero BSS if memsz > filesz
            if (memsz > filesz) {
                SetMem((void*)(phys_addr + filesz), memsz - filesz, 0);
            }
        }
    }

    g_boot_info.kernel_physical_base = kernel_min_addr;
    g_boot_info.kernel_virtual_base = kernel_min_addr;
    g_boot_info.kernel_size = kernel_max_addr - kernel_min_addr;
    DPRINT(L"      All segments loaded\r\n\r\n");

    // Initialize framebuffer
    DPRINT(L"[5/8] Initializing framebuffer...\r\n");
    init_framebuffer(root);
    DPRINT(L"\r\n");

    // Find ACPI RSDP
    DPRINT(L"[6/8] Locating ACPI tables...\r\n");
    find_acpi_rsdp();
    DPRINT(L"\r\n");

    // Get memory map
    DPRINT(L"[7/8] Getting memory map...\r\n");
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size,
                              memory_map, &map_key, &descriptor_size, &descriptor_version);
    if (status == EFI_BUFFER_TOO_SMALL) {
        memory_map_size += 2 * descriptor_size;
        status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                  memory_map_size, (void**)&memory_map);
        if (EFI_ERROR(status)) {
            Print(L"ERROR: Failed to allocate memory map buffer\r\n");
            goto error;
        }

        status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size,
                                  memory_map, &map_key, &descriptor_size, &descriptor_version);
    }
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Failed to get memory map (0x%x)\r\n", status);
        goto error;
    }

    // Build our memory map
    build_memory_map(memory_map, memory_map_size, descriptor_size);
    DPRINT(L"\r\n");

    // Exit boot services
    DPRINT(L"[8/8] Exiting UEFI boot services...\r\n");

    // We need to get the memory map again right before ExitBootServices
    // The size may have changed, so query the size first
    memory_map_size = 0;
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size,
                              NULL, &map_key, &descriptor_size, &descriptor_version);

    if (status == EFI_BUFFER_TOO_SMALL) {
        // Free old buffer and allocate new one with extra space
        if (memory_map) {
            uefi_call_wrapper(BS->FreePool, 1, memory_map);
        }
        memory_map_size += 4 * descriptor_size;  // Extra space for changes
        status = uefi_call_wrapper(BS->AllocatePool, 3, EfiLoaderData,
                                  memory_map_size, (void**)&memory_map);
        if (EFI_ERROR(status)) {
            Print(L"ERROR: Failed to allocate memory map buffer\r\n");
            goto error;
        }
    }

    // Now get the actual memory map
    status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size,
                              memory_map, &map_key, &descriptor_size, &descriptor_version);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Failed to get final memory map (0x%x)\r\n", status);
        goto error;
    }

    status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
    if (EFI_ERROR(status)) {
        // Memory map may have changed, try again
        memory_map_size += 4 * descriptor_size;
        status = uefi_call_wrapper(BS->GetMemoryMap, 5, &memory_map_size,
                                  memory_map, &map_key, &descriptor_size, &descriptor_version);
        if (!EFI_ERROR(status)) {
            status = uefi_call_wrapper(BS->ExitBootServices, 2, ImageHandle, map_key);
        }
        if (EFI_ERROR(status)) {
            // #QUIETBOOT: SAY SO. The old comment here read "Can't print anymore
            // after failed ExitBootServices", and that is backwards: if
            // ExitBootServices FAILED then boot services are still up, which is
            // precisely why Print() still works. The one path where printing is
            // genuinely impossible is the SUCCESS path, and that one does not
            // come here.
            //
            // It matters much more now that a normal boot is silent. Before, a
            // failure here at least left nine [N/8] headings on screen, so you
            // could see it had got to the last one. On a quiet boot the screen
            // is the firmware's until this moment, so without this line a failed
            // ExitBootServices is indistinguishable from a machine that never
            // ran our loader at all.
            Print(L"ERROR: ExitBootServices FAILED twice (0x%x).\r\n", status);
            Print(L"       The memory map changed under us and re-fetching it did\r\n");
            Print(L"       not help. The kernel was NOT started.\r\n");
            goto error;
        }
    }

    // Jump to kernel with boot_info pointer in RDI
    // (System V AMD64 ABI: first argument in RDI)
    kernel_entry_t entry = (kernel_entry_t)kernel_entry;
    entry(&g_boot_info);

    // Should never reach here
    for (;;) {
        __asm__ volatile("hlt");
    }

error:
    // #QUIETBOOT: ON A QUIET BOOT THE ERROR LINE ABOVE IS THE FIRST THING ON THE
    // SCREEN, so print the banner underneath it now. Without this, a user
    // photographing a failed boot sends back one bare "ERROR: ..." line with
    // nothing to say which program produced it or how far it got, which is a
    // worse bug report than the noisy boot was a nuisance. Costs three lines,
    // and only on a boot that has already failed.
    if (!g_banner_shown) {
        Print(L"\r\n========================================\r\n");
        Print(L"  MayteraOS UEFI Bootloader v3.1\r\n");
        Print(L"  Boot FAILED. Put an empty file at \\boot\\DIAG.TXT on this\r\n");
        Print(L"  disk and boot again for the full step-by-step console.\r\n");
        Print(L"========================================\r\n");
        g_banner_shown = 1;
    }
    Print(L"\r\nPress any key to exit...\r\n");
    EFI_INPUT_KEY key;
    while (uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key) == EFI_NOT_READY);
    return status;
}
