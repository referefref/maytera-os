/* Linker script for the #427 relocation-engine PIE test binary.
 *
 * MayteraOS's ELF loader (exec/elf.c) copies each PT_LOAD segment's file
 * data into freshly-mapped user pages via the segment's *final* page
 * permissions (see user.ld's comment: it must be WRITABLE at copy time, so
 * ld's normal split into separate R/RX/RW segments page-faults the loader).
 * Force a SINGLE RWX PT_LOAD covering everything (matches user.ld's existing
 * fixed-base convention), but do NOT fix the base address here - this must
 * link as a real ET_DYN/PIE image with its own PT_DYNAMIC + DT_RELA table,
 * which is exactly what the #427 relocation engine is being tested against.
 */

ENTRY(_start)

PHDRS {
    load    PT_LOAD    FLAGS(7);   /* 7 = PF_R | PF_W | PF_X, single segment */
    dynamic PT_DYNAMIC FLAGS(6);   /* 6 = PF_R | PF_W, describes .dynamic */
}

SECTIONS
{
    . = 0x1000;

    .hash     : { *(.hash) *(.gnu.hash) }                                   :load
    .dynsym   : { *(.dynsym) }                                              :load
    .dynstr   : { *(.dynstr) }                                              :load
    .rela.dyn : { *(.rela.dyn) *(.rela.data*) *(.rela.rodata*)
                  *(.rela.text*) *(.rela.bss*) *(.rela.got) }               :load
    .text     : { *(.text*) }                                              :load
    .rodata   : { *(.rodata*) }                                            :load
    .dynamic  : { *(.dynamic) }                                            :load :dynamic
    .data     : { *(.data*) }                                              :load
    .bss      : { *(.bss*) *(COMMON) }                                     :load
}
