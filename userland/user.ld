/* Linker script for MayteraOS user-space applications */
/* Base address: 0x80000000 (2GB) - VMM user space region (PDPT[2]) */
/* Sign-extension is handled by kernel syscall sanitization */

ENTRY(_start)

/* The kernel ELF loader copies each PT_LOAD into the new user address space via
   the mapped user virtual address, so every loadable segment must be WRITABLE
   during load. Force a SINGLE RWX PT_LOAD covering all sections: otherwise ld
   splits off a read-only .text/.rodata segment once the binary grows past a page
   boundary, and the loader then page-faults (CR0.WP) writing into it. Small apps
   happened to get one RWE segment by luck; this makes it guaranteed. */
PHDRS {
    load PT_LOAD FLAGS(7);   /* 7 = PF_R | PF_W | PF_X */
}

SECTIONS
{
    . = 0x80000000;

    .text : {
        *(.text*)
    } :load

    .rodata : {
        *(.rodata*)
    } :load

    .data : {
        *(.data*)
    } :load

    .bss : {
        *(.bss*)
        *(COMMON)
    } :load
}
