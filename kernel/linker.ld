/* linker.ld - Linker script for MayteraOS 64-bit kernel */

/* Entry point */
ENTRY(_start)

/* Kernel virtual base address (higher half) */
/* For now, we link at physical addresses since we're loaded by UEFI */
/* which gives us identity-mapped memory. We'll set up proper higher-half */
/* mapping in the memory manager. */

/* Kernel load address - UEFI loads us here */
KERNEL_PHYS_BASE = 0x400000;  /* 4 MB - moved from 1MB to avoid text segment crossing 0x200000 boundary */

SECTIONS
{
    /* Start at 1MB physical address */
    . = KERNEL_PHYS_BASE;

    /* Text section (code) */
    .text ALIGN(4K) :
    {
        __text_start = .;
        *(.multiboot)
        *(.text)
        *(.text.*)
        __text_end = .;
    }

    /* Read-only data */
    .rodata ALIGN(4K) :
    {
        __rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        __rodata_end = .;
    }

    /* Move data/BSS to 32MB to avoid UEFI kernel buffer overlap */
    . = 0x2000000;

    /* Read-write data */
    .data ALIGN(4K) :
    {
        __data_start = .;
        *(.data)
        *(.data.*)
        __data_end = .;
    }

    /* Uninitialized data (BSS) */
    .bss ALIGN(4K) :
    {
        __bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Kernel end marker */
    . = ALIGN(4K);
    __kernel_end = .;

    /* Calculate kernel size */
    __kernel_size = __kernel_end - KERNEL_PHYS_BASE;

    /* Discard unnecessary sections */
    /DISCARD/ :
    {
        *(.comment)
        *(.note)
        *(.note.*)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
