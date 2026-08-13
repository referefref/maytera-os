/* user-pie.ld - #640 stage 1: linker script for POSITION-INDEPENDENT MayteraOS
   user applications (ET_DYN). The kernel chooses the load base; the binary
   declares none.

   WHY PIE AND NOT A DIFFERENT FIXED BASE (#640, and the reason this file exists
   rather than a second copy of user.ld with a bigger number in it):

   Userland has to leave the 2-3GB window, because that window also holds device
   MMIO whose address is chosen by FIRMWARE (MEASURED on the user's iMac14,4: GOP
   framebuffer at 0x90000000, which is simultaneously PIE_USER_BASE and libc's
   old HEAP_START). A fixed HIGH base would force every app to -mcmodel=large,
   because small-model R_X86_64_32S cannot reach past 2GB. 34 of 141 app
   Makefiles do NOT pass -mcmodel=large today, so that is 34 code-model changes
   plus a base change. -fPIE is RIP-relative: position-independent AND
   code-model-agnostic, so it is ONE change instead of two. This is not a
   prediction: `init` failed to link at 0x80000000 with exactly
   "relocation truncated to fit: R_X86_64_32S against `.data'" until
   -mcmodel=large was added (#633), and `python` sits on the golden build's
   KNOWN_FAIL list for the mirror-image failure, R_X86_64_32S overflow UNDER
   -mcmodel. Both are the same wall from opposite sides.

   =========================================================================
   WHY THERE IS NO `PHDRS { ... }` BLOCK HERE, unlike user.ld. READ THIS
   BEFORE "fixing" it back.

   MEASURED on GNU ld 2.40 (Debian, the build container's linker): a script
   containing an explicit PHDRS block makes ld emit **ET_EXEC even under -pie**.
   Same objects, same flags, only the script differs:

       ld -pie -T <script with PHDRS>    -> Type: EXEC   (NOT position-independent)
       ld -pie -T <same script, no PHDRS>-> Type: DYN    (correct)

   ET_EXEC is silently fatal to the whole exercise: kernel/exec/elf.c decides
   `is_pie` from `e_type == ET_DYN`, so an ET_EXEC "PIE" is loaded at its
   declared base with relocation_offset 0 and the relocation engine, though it
   runs, has nothing to do. The binary would look plausible and prove nothing.

   user.ld needs its PHDRS block to FORCE a single RWX PT_LOAD, because the
   kernel loader writes each segment through the target's user mapping and a
   read-only segment used to fault at CPL 0. As of #633 the loader maps every
   load destination WRITABLE regardless of the segment's PF_W, so that
   requirement is gone and dropping PHDRS is safe. Without PHDRS, ld's own
   layout for these objects is in fact a single RWX PT_LOAD plus PT_DYNAMIC,
   which is exactly what is wanted. If a future app grows a layout ld splits
   into several PT_LOADs, that is now also fine: the loader accepts any number
   of PT_LOADs inside the userland window (#633 pre-flight).

   --no-dynamic-linker suppresses the PT_INTERP that -pie otherwise emits
   ("/lib/ld64.so.1"). MayteraOS has no dynamic linker: the KERNEL applies the
   relocations. The loader ignores PT_INTERP either way, but shipping a request
   for an interpreter that does not exist is a lie in the binary.
   =========================================================================

   .data.rel.ro IS NOT OPTIONAL AND IS EASY TO MISS: a `static const char *tab[]`
   lands there, not in .rodata and not in .data. Omit it from this script and ld
   orphan-places it, which happens to work until it does not. The same class of
   silent-orphan bug already cost this project .init_array (see user.ld). */

ENTRY(_start)

SECTIONS
{
    /* Base 0: this is a PIE. elf_load_user() adds the runtime base. */
    . = SIZEOF_HEADERS;

    .hash          : { *(.hash) }
    .gnu.hash      : { *(.gnu.hash) }
    .dynsym        : { *(.dynsym) }
    .dynstr        : { *(.dynstr) }
    .gnu.version   : { *(.gnu.version) }
    .gnu.version_r : { *(.gnu.version_r) }
    .rela.dyn      : { *(.rela.init) *(.rela.text*) *(.rela.rodata*)
                       *(.rela.data*) *(.rela.got) *(.rela.bss*)
                       *(.rela.ldata*) *(.rela.dyn) }
    .rela.plt      : { *(.rela.plt) }

    .text          : { *(.text*) }
    .plt           : { *(.plt) *(.plt.*) }
    .rodata        : { *(.rodata*) }
    .eh_frame      : { *(.eh_frame) }

    /* #640 leg 4: THE W^X BOUNDARY.
       Everything above is read-only or executable; everything below is
       writable. Without a page boundary here ld merges the lot into ONE
       PT_LOAD whose flags are the union - RWX - which is what every app
       shipped until now. This single ALIGN is what makes ld emit two
       segments: R+E for code and constants, RW for data.

       CONSTANT(MAXPAGESIZE), not a literal, so it tracks -z max-page-size.
       Every app Makefile passes -z max-page-size=0x1000; without that the
       default is 2MB and this ALIGN would pad a 2MB hole into every binary.

       The kernel loader currently maps every load destination WRITABLE
       regardless of PF_W (#633), so this changes the BINARY only, not the
       runtime. Making the loader honour these flags is the next step, and it
       must run map-writable -> copy -> relocate -> protect: relocations
       silently skip a non-writable target, so protecting early yields apps
       that load, run, and are quietly wrong. */
    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .init_array    : {
        __init_array_start = .;
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        __init_array_end = .;
    }

    .dynamic       : { *(.dynamic) }

    .data.rel.ro   : { *(.data.rel.ro*) }
    .got           : { *(.got) *(.got.plt) }
    .data          : { *(.data*) }
    .bss           : { *(.bss*) *(COMMON) }
}
