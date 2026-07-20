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
#define MAX_MEMORY_MAP_ENTRIES 256

// Memory map entry structure
typedef struct {
    uint64_t base;      // Physical base address
    uint64_t length;    // Length in bytes
    uint32_t type;      // Memory type
    uint32_t attributes; // Additional attributes
} __attribute__((packed)) memory_map_entry_t;

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

    // Reserved for future use
    uint64_t reserved[8];
} __attribute__((packed)) boot_info_t;

// Global boot info pointer (set by kernel entry)
extern boot_info_t *g_boot_info;

// Boot info functions
void boot_info_init(boot_info_t *info);
void boot_info_print(void);
uint64_t boot_info_get_total_memory(void);
memory_map_entry_t* boot_info_get_memory_map(uint32_t *count);
framebuffer_info_t* boot_info_get_framebuffer(void);

#endif // BOOT_INFO_H
