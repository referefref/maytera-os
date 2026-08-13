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

// Maximum memory map entries
#define MAX_MEMORY_MAP_ENTRIES 256

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
    UINT64 reserved[8];
} __attribute__((packed)) boot_info_t;

// Kernel entry point type
typedef void (*kernel_entry_t)(boot_info_t *boot_info);

// Global boot info and memory map
static boot_info_t g_boot_info;
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

// Initialize framebuffer info from GOP
EFI_STATUS init_framebuffer(void) {
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

    // Get current mode info
    mode_info = gop->Mode->Info;

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

    Print(L"      Framebuffer: %ux%u at 0x%lx\r\n",
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
            Print(L"      ACPI 2.0 RSDP at 0x%lx\r\n", g_boot_info.acpi.rsdp_address);
            return;
        }
    }

    // Fall back to ACPI 1.0
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *table = &ST->ConfigurationTable[i];

        if (CompareMem(&table->VendorGuid, &Acpi10TableGuid, sizeof(EFI_GUID)) == 0) {
            g_boot_info.acpi.rsdp_address = (UINT64)table->VendorTable;
            g_boot_info.acpi.rsdp_version = 1;
            Print(L"      ACPI 1.0 RSDP at 0x%lx\r\n", g_boot_info.acpi.rsdp_address);
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
    UINT64 total_memory = 0;
    UINT8 *ptr = (UINT8 *)uefi_map;

    for (UINTN offset = 0; offset < map_size && entry_count < MAX_MEMORY_MAP_ENTRIES;
         offset += descriptor_size) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)(ptr + offset);

        g_memory_map[entry_count].base = desc->PhysicalStart;
        g_memory_map[entry_count].length = desc->NumberOfPages * 4096;
        g_memory_map[entry_count].type = convert_memory_type(desc->Type);
        g_memory_map[entry_count].attributes = (UINT32)desc->Attribute;

        // Count usable memory
        if (g_memory_map[entry_count].type == MEMORY_TYPE_USABLE) {
            total_memory += g_memory_map[entry_count].length;
        }

        entry_count++;
    }

    g_boot_info.memory_map_address = (UINT64)g_memory_map;
    g_boot_info.memory_map_entries = entry_count;
    g_boot_info.memory_map_entry_size = sizeof(memory_map_entry_t);
    g_boot_info.total_memory = total_memory;

    Print(L"      Memory map: %u entries, %lu MB usable\r\n",
          entry_count, total_memory / (1024 * 1024));
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
    if (EFI_ERROR(status)) { Print(L"[chime] alloc failed; skipping\r\n"); return; }
    UINTN pcm_pages = (CHIME_PCM_BYTES + 4095) / 4096;
    status = uefi_call_wrapper(BS->AllocatePages, 4, AllocateAnyPages,
                               EfiBootServicesData, pcm_pages, &pcm_pg);
    if (EFI_ERROR(status)) {
        uefi_call_wrapper(BS->FreePages, 2, ctrl_pg, 1);
        Print(L"[chime] PCM alloc failed; skipping\r\n");
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
        Print(L"[chime] no HD Audio controller found; skipping\r\n");
        goto done;
    }
    if (best_score < 0) {
        Print(L"[chime] no usable output path on %d controller(s); skipping\r\n", controllers);
        goto done;
    }

    Print(L"[chime] HDA codec %04x:%04x (%s) DAC=%d PIN=%d on %02x:%02x.%x\r\n",
          best_codec.vendor_id, best_codec.device_id,
          best_codec.is_analog ? L"analog" : L"digital",
          best_codec.dac_nid, best_codec.out_pin_nid, best_bus, best_slot, best_func);

    // Finalize on the winning controller: bring it back up, configure the codec
    // route, set the converter format, and start the output stream.
    pci_enable_bm(best_bus, best_slot, best_func);
    if (!hda_bring_up(best_bar, (UINT64)(UINTN)g_corb, (UINT64)(UINTN)g_rirb)) {
        Print(L"[chime] winner re-init failed; skipping\r\n");
        goto done;
    }
    hda_w16(HDA_REG_STATESTS, hda_r16(HDA_REG_STATESTS));

    UINT16 gcap = hda_r16(HDA_REG_GCAP);
    UINT8 num_oss = (gcap >> 12) & 0xF;
    UINT8 num_iss = (gcap >> 8) & 0xF;
    if (num_oss == 0) { Print(L"[chime] no output streams; skipping\r\n"); goto done; }
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
    Print(L"[chime] playing bong: LPIB %u -> %u (%s)\r\n",
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
    Print(L"[chime] done\r\n");

done:
    (void)pcm;
    uefi_call_wrapper(BS->FreePages, 2, pcm_pg, pcm_pages);
    uefi_call_wrapper(BS->FreePages, 2, ctrl_pg, 1);
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

    Print(L"========================================\r\n");
    Print(L"  MayteraOS UEFI Bootloader v3.1 (boot chime)\r\n");
    Print(L"========================================\r\n\r\n");

    // Pre-kernel boot chime (macOS-style bong). Best-effort: always returns and
    // never blocks the boot, even if no HD Audio hardware is present.
    play_boot_chime();
    Print(L"\r\n");

    // Open root filesystem
    Print(L"[1/8] Opening root filesystem...\r\n");
    status = open_root_dir(ImageHandle, &root);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Failed to open root directory (0x%x)\r\n", status);
        goto error;
    }
    Print(L"      Root filesystem opened successfully\r\n\r\n");

    // Load kernel file
    Print(L"[2/8] Loading /boot/kernel.elf...\r\n");
    status = load_file(root, L"boot\\kernel.elf", &kernel_buffer, &kernel_size);
    if (EFI_ERROR(status)) {
        Print(L"ERROR: Failed to load kernel.elf (0x%x)\r\n", status);
        Print(L"       Make sure /boot/kernel.elf exists on the disk\r\n");
        goto error;
    }
    Print(L"      Kernel loaded: %d bytes\r\n\r\n", kernel_size);

    // Verify ELF header
    Print(L"[3/8] Parsing ELF header...\r\n");
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
    Print(L"      ELF header valid\r\n");
    Print(L"      Entry point: 0x%lx\r\n", kernel_entry);
    Print(L"      Program headers: %d\r\n\r\n", elf_header->e_phnum);

    // Load program segments
    Print(L"[4/8] Loading kernel segments...\r\n");
    program_headers = (Elf64_Phdr*)((UINT8*)kernel_buffer + elf_header->e_phoff);

    UINT64 kernel_min_addr = 0xFFFFFFFFFFFFFFFF;
    UINT64 kernel_max_addr = 0;

    for (int i = 0; i < elf_header->e_phnum; i++) {
        if (program_headers[i].p_type == PT_LOAD) {
            UINT64 vaddr = program_headers[i].p_vaddr;
            UINT64 memsz = program_headers[i].p_memsz;
            UINT64 filesz = program_headers[i].p_filesz;
            UINT64 offset = program_headers[i].p_offset;

            Print(L"      Segment %d: vaddr=0x%lx size=%d bytes\r\n", i, vaddr, memsz);

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
    Print(L"      All segments loaded\r\n\r\n");

    // Initialize framebuffer
    Print(L"[5/8] Initializing framebuffer...\r\n");
    init_framebuffer();
    Print(L"\r\n");

    // Find ACPI RSDP
    Print(L"[6/8] Locating ACPI tables...\r\n");
    find_acpi_rsdp();
    Print(L"\r\n");

    // Get memory map
    Print(L"[7/8] Getting memory map...\r\n");
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
    Print(L"\r\n");

    // Exit boot services
    Print(L"[8/8] Exiting UEFI boot services...\r\n");

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
            // Can't print anymore after failed ExitBootServices
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
    Print(L"\r\nPress any key to exit...\r\n");
    EFI_INPUT_KEY key;
    while (uefi_call_wrapper(ST->ConIn->ReadKeyStroke, 2, ST->ConIn, &key) == EFI_NOT_READY);
    return status;
}
