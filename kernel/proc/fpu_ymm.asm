; fpu_ymm.asm - #745 (local 107): YMM oracle helpers. DEBUG BUILDS ONLY.
;
; The whole file is assembled ONLY under `make YMMTEST=1` / `make YMMTEST=red`
; (which pass nasm -dFPU_YMM_SELFTEST). A shipping kernel therefore contains no
; VEX-encoded instruction at all, which is a property worth keeping: it is
; exactly what the local-107 disassembly survey measured of build 1880.
;
; These two routines exist because the kernel is built -mno-sse -mno-sse2 and
; cannot be made to emit a YMM access from C at all. They are the ONLY hand
; -written AVX in the tree.
%ifdef FPU_YMM_SELFTEST

section .text
global ymm_fill
global ymm_dump

; void ymm_fill(const void *pat32)
;   RDI = 32 bytes. Loaded into every one of ymm0..ymm15, so a switch that
;   drops the upper 128 bits of ANY of them is visible.
ymm_fill:
    vmovdqu ymm0,  [rdi]
    vmovdqu ymm1,  [rdi]
    vmovdqu ymm2,  [rdi]
    vmovdqu ymm3,  [rdi]
    vmovdqu ymm4,  [rdi]
    vmovdqu ymm5,  [rdi]
    vmovdqu ymm6,  [rdi]
    vmovdqu ymm7,  [rdi]
    vmovdqu ymm8,  [rdi]
    vmovdqu ymm9,  [rdi]
    vmovdqu ymm10, [rdi]
    vmovdqu ymm11, [rdi]
    vmovdqu ymm12, [rdi]
    vmovdqu ymm13, [rdi]
    vmovdqu ymm14, [rdi]
    vmovdqu ymm15, [rdi]
    ret

; void ymm_dump(void *out)
;   RDI = 16 * 32 bytes, receives ymm0..ymm15 in order.
ymm_dump:
    vmovdqu [rdi + 0*32],  ymm0
    vmovdqu [rdi + 1*32],  ymm1
    vmovdqu [rdi + 2*32],  ymm2
    vmovdqu [rdi + 3*32],  ymm3
    vmovdqu [rdi + 4*32],  ymm4
    vmovdqu [rdi + 5*32],  ymm5
    vmovdqu [rdi + 6*32],  ymm6
    vmovdqu [rdi + 7*32],  ymm7
    vmovdqu [rdi + 8*32],  ymm8
    vmovdqu [rdi + 9*32],  ymm9
    vmovdqu [rdi + 10*32], ymm10
    vmovdqu [rdi + 11*32], ymm11
    vmovdqu [rdi + 12*32], ymm12
    vmovdqu [rdi + 13*32], ymm13
    vmovdqu [rdi + 14*32], ymm14
    vmovdqu [rdi + 15*32], ymm15
    ret

%endif ; FPU_YMM_SELFTEST
