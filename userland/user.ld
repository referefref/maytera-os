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

    /* AssaultCube bring-up (2026-07): C++ global/static objects with dynamic
       initializers (every AssaultCube COMMAND/VAR/VARP macro expands to one,
       see source/src/command.h, 838 call sites in the upstream engine) need
       their .init_array entries run before main(). Previously this section
       was not emitted at all by this script, so GNU ld silently orphan-placed
       it and crt0 never ran it: any C++ port with non-trivial global init
       would silently no-op. __init_array_start/__init_array_end are consumed
       by libc/libc_init.c's __libc_init(). KEEP prevents a future
       --gc-sections from discarding entries with no visible referrer.
       Existing C apps are unaffected: they have zero .init_array entries, so
       __init_array_start == __init_array_end and the run loop iterates zero
       times. */
    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    } :load

    .data : {
        *(.data*)
    } :load

    .bss : {
        *(.bss*)
        *(COMMON)
    } :load
}
