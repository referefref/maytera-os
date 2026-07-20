; Optimized memcpy for userland - uses REP MOVSB (fast on modern CPUs with ERMSB)
; and SSE for very large copies

global memcpy_fast
global memset_fast
global memmove_fast

section .text

; void *memcpy_fast(void *dest, const void *src, size_t n)
memcpy_fast:
    push rbp
    mov rbp, rsp
    push rdi
    push rsi
    
    mov rax, rdi        ; return value = dest
    mov rcx, rdx        ; count
    
    test rcx, rcx
    jz .done
    
    ; Use REP MOVSB - fast on modern CPUs with ERMSB
    cld
    rep movsb
    
.done:
    pop rsi
    pop rdi
    pop rbp
    ret

; void *memset_fast(void *dest, int c, size_t n)
memset_fast:
    push rbp
    mov rbp, rsp
    push rdi
    
    mov rax, rdi        ; return value = dest
    mov rcx, rdx        ; count
    mov al, sil         ; byte to set
    
    test rcx, rcx
    jz .done
    
    ; Use REP STOSB
    cld
    rep stosb
    
    mov rax, [rbp-8]    ; restore return value (dest)
    
.done:
    pop rdi
    pop rbp
    ret

; void *memmove_fast(void *dest, const void *src, size_t n)
memmove_fast:
    push rbp
    mov rbp, rsp
    push rdi
    push rsi
    
    mov rax, rdi        ; return value = dest
    mov rcx, rdx        ; count
    
    test rcx, rcx
    jz .done
    
    ; Check for overlap: if dest > src && dest < src+n, copy backwards
    cmp rdi, rsi
    jbe .forward        ; dest <= src, safe to copy forward
    
    mov r8, rsi
    add r8, rcx
    cmp rdi, r8
    jae .forward        ; dest >= src+n, safe to copy forward
    
    ; Copy backwards
    std                 ; set direction flag for backwards
    add rsi, rcx
    add rdi, rcx
    dec rsi
    dec rdi
    rep movsb
    cld                 ; clear direction flag
    jmp .done
    
.forward:
    cld
    rep movsb
    
.done:
    pop rsi
    pop rdi
    pop rbp
    ret
