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
    Print(L"  MayteraOS UEFI Bootloader v3.0\r\n");
    Print(L"========================================\r\n\r\n");

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
