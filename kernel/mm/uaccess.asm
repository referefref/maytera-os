; uaccess.asm - fault-safe user-memory copy primitives (#509 TOCTOU keystone)
;
; These are the ONLY routines the kernel uses to physically touch user memory.
; Each faultable instruction region is bracketed by _ex_start/_ex_end labels and
; has a _ex_fixup label. mm/fault.c builds an exception table from these
; symbols: a #PF whose RIP lands inside a region, that mm_fault() could NOT
; resolve as a legitimate demand/COW page, is redirected to that region's fixup.
; The fixup makes the routine return a non-zero "bytes not copied" (or -1), so
; copy_*_user() return -EFAULT and the KERNEL SURVIVES a page that a racing
; sibling thread unmapped mid-copy (the #509 TOCTOU window). A legitimately-lazy
; demand page is faulted in by mm_fault() and the instruction simply re-executes.

section .text

; #645 SMAP BRACKETS.
;
; With CR4.SMAP set, the user accesses below (`rep movsb`, `rep stosb`, the byte
; loads) fault unless RFLAGS.AC is set. These macros are the ONLY place in the
; kernel that AC is raised for a normal user copy.
;
; GATED, because STAC/CLAC are #UD when CPUID.(EAX=7,ECX=0):EBX.SMAP is clear.
; g_smap_active (kernel/security/security.c) is 1 only after the CR4.SMAP write
; was READ BACK as taken, so this kernel still runs on a CPU or hypervisor that
; has no SMAP at all. The test is ONE byte load per copy CALL, deliberately
; outside the byte loops, so it never sits in an inner loop.
;
; Only FLAGS are clobbered. No live register is touched: rcx (count), al (fill
; byte / loaded byte), rsi/rdi (pointers) and r8 all survive.
;
; PLACEMENT RULES, enforced by tools/smap-uaccess-lint (rule A1):
;   * SMAP_STAC before the routine's _ex_start label.
;   * SMAP_CLAC on EVERY return path.
;   * SMAP_CLAC as the first act of EVERY _ex_fixup. mm/fault.c reaches those
;     labels by REWRITING RIP, which SKIPS the CLAC after _ex_end. Omit it and
;     every -EFAULT returns with AC still set, leaving SMAP disabled for the
;     rest of the syscall, on exactly the path an attacker provokes on purpose.
extern g_smap_active

%macro SMAP_STAC 0
    cmp     byte [rel g_smap_active], 0
    je      %%smap_off
    stac
%%smap_off:
%endmacro

%macro SMAP_CLAC 0
    cmp     byte [rel g_smap_active], 0
    je      %%smap_off
    clac
%%smap_off:
%endmacro

global __uaccess_copy
global __uaccess_copy_ex_start
global __uaccess_copy_ex_end
global __uaccess_copy_ex_fixup

global __uaccess_set
global __uaccess_set_ex_start
global __uaccess_set_ex_end
global __uaccess_set_ex_fixup

global __uaccess_strncpy
global __uaccess_strncpy_ex_start
global __uaccess_strncpy_ex_end
global __uaccess_strncpy_ex_fixup

global __uaccess_strnlen
global __uaccess_strnlen_ex_start
global __uaccess_strnlen_ex_end
global __uaccess_strnlen_ex_fixup

; size_t __uaccess_copy(void *dst /rdi/, const void *src /rsi/, size_t n /rdx/)
; Returns 0 on success, else the number of bytes NOT copied (a fault occurred).
__uaccess_copy:
    mov     rcx, rdx
    SMAP_STAC
__uaccess_copy_ex_start:
    rep movsb                       ; may #PF; on fault rcx = bytes remaining
__uaccess_copy_ex_end:
    SMAP_CLAC
    xor     rax, rax                ; full success
    ret
__uaccess_copy_ex_fixup:
    SMAP_CLAC                       ; reached by RIP rewrite: skips the CLAC above
    mov     rax, rcx                ; bytes not copied (non-zero -> EFAULT)
    ret

; size_t __uaccess_set(void *dst /rdi/, int val /esi/, size_t n /rdx/)
; Returns 0 on success, else the number of bytes NOT set.
__uaccess_set:
    mov     rcx, rdx
    mov     eax, esi                ; al = fill byte
    SMAP_STAC
__uaccess_set_ex_start:
    rep stosb                       ; may #PF; on fault rcx = bytes remaining
__uaccess_set_ex_end:
    SMAP_CLAC
    xor     rax, rax
    ret
__uaccess_set_ex_fixup:
    SMAP_CLAC                       ; reached by RIP rewrite: skips the CLAC above
    mov     rax, rcx
    ret

; ssize_t __uaccess_strncpy(char *dst /rdi/, const char *src /rsi/, size_t max /rdx/)
; Copies up to max-1 bytes until NUL, always NUL-terminates within [0,max-1].
; Returns length copied (excluding NUL), or -1 on fault. Caller guarantees max>=1.
__uaccess_strncpy:
    xor     rcx, rcx                ; i = 0
    lea     r8, [rdx - 1]           ; usable = max - 1
    SMAP_STAC                       ; brackets the whole bounded loop below
uac_scpy_loop:
    cmp     rcx, r8
    jae     uac_scpy_term
__uaccess_strncpy_ex_start:
    mov     al, [rsi + rcx]         ; user read (may #PF)
    mov     [rdi + rcx], al         ; kernel write (validated dest)
__uaccess_strncpy_ex_end:
    test    al, al
    jz      uac_scpy_done
    inc     rcx
    jmp     uac_scpy_loop
uac_scpy_term:
    mov     byte [rdi + rcx], 0     ; ran out of room: terminate
uac_scpy_done:
    SMAP_CLAC                       ; also the fallthrough path from uac_scpy_term
    mov     rax, rcx                ; length excluding NUL
    ret
__uaccess_strncpy_ex_fixup:
    SMAP_CLAC                       ; reached by RIP rewrite: skips the CLAC above
    mov     rax, -1
    ret

; ssize_t __uaccess_strnlen(const char *s /rdi/, size_t max /rsi/)
; Returns length (returns max if no NUL within max), or -1 on fault.
__uaccess_strnlen:
    xor     rcx, rcx                ; i = 0
    SMAP_STAC                       ; brackets the whole bounded loop below
uac_snlen_loop:
    cmp     rcx, rsi
    jae     uac_snlen_done
__uaccess_strnlen_ex_start:
    mov     al, [rdi + rcx]         ; user read (may #PF)
__uaccess_strnlen_ex_end:
    test    al, al
    jz      uac_snlen_done
    inc     rcx
    jmp     uac_snlen_loop
uac_snlen_done:
    SMAP_CLAC
    mov     rax, rcx
    ret
__uaccess_strnlen_ex_fixup:
    SMAP_CLAC                       ; reached by RIP rewrite: skips the CLAC above
    mov     rax, -1
    ret
