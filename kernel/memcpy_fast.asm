; memcpy_fast.asm - Optimized memory copy routines for MayteraOS
; Uses rep movsb (ERMSB optimized on modern CPUs) and SSE for large copies

section .text
global memcpy_fast
global memset_fast
global memmove_fast

; void *memcpy_fast(void *dest, const void *src, size_t n)
; Uses rep movsb for all sizes - modern CPUs optimize this heavily (ERMSB)
; For very large copies (>256KB), uses SSE with non-temporal stores
memcpy_fast:
    ; Fast + safe. Bulk copies use UNALIGNED SSE (movdqu load AND store), which
    ; needs no 16-byte alignment (so no #GP like the old movntdq path) and is
    ; ~4x fewer ops than rep movsb on QEMU TCG (restores wallpaper-load speed).
    ; xmm0-3 are saved/restored so the TTF rasterizer is never clobbered. Kernel
    ; ISRs are built -mno-sse, so they cannot clobber xmm mid-copy.
    push rdi
    push rsi
    mov rax, rdi          ; Return value = dest
    mov rcx, rdx          ; Count
    cmp rcx, 128
    jb .tail             ; small copies: plain rep movsb

    sub rsp, 64
    movdqu [rsp], xmm0
    movdqu [rsp+16], xmm1
    movdqu [rsp+32], xmm2
    movdqu [rsp+48], xmm3

    mov r8, rcx
    shr r8, 6            ; number of 64-byte blocks
.blk:
    movdqu xmm0, [rsi]
    movdqu xmm1, [rsi+16]
    movdqu xmm2, [rsi+32]
    movdqu xmm3, [rsi+48]
    movdqu [rdi], xmm0
    movdqu [rdi+16], xmm1
    movdqu [rdi+32], xmm2
    movdqu [rdi+48], xmm3
    add rsi, 64
    add rdi, 64
    dec r8
    jnz .blk

    movdqu xmm0, [rsp]
    movdqu xmm1, [rsp+16]
    movdqu xmm2, [rsp+32]
    movdqu xmm3, [rsp+48]
    add rsp, 64
    and rcx, 63          ; remaining bytes < 64

.tail:
    cld
    rep movsb
    pop rsi
    pop rdi
    ret

; void *memset_fast(void *dest, int c, size_t n)
; Uses rep stosb - ERMSB optimized
memset_fast:
    push rdi
    
    mov rax, rdi          ; Save dest for return
    mov r8, rdi           ; Save dest
    mov eax, esi          ; Character in AL
    mov rcx, rdx          ; Count
    
    cld
    rep stosb
    
    mov rax, r8           ; Return original dest
    pop rdi
    ret

; void *memmove_fast(void *dest, const void *src, size_t n)
; Handles overlapping regions correctly
memmove_fast:
    push rdi
    push rsi
    
    mov rax, rdi          ; Save dest for return
    mov rcx, rdx          ; Count
    
    ; Check for overlap: if dest > src && dest < src+n, copy backwards
    cmp rdi, rsi
    jbe .forward          ; dest <= src, safe to copy forward
    
    mov r8, rsi
    add r8, rcx
    cmp rdi, r8
    jae .forward          ; dest >= src+n, safe to copy forward
    
    ; Copy backwards
    std
    add rdi, rcx
    dec rdi
    add rsi, rcx
    dec rsi
    rep movsb
    cld
    jmp .done

.forward:
    cld
    rep movsb

.done:
    pop rsi
    pop rdi
    ret
