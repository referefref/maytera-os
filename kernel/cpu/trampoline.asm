; trampoline.asm - AP Startup Code (Real Mode -> Long Mode transition)
; Part of Task #41 (SMP Support)
;
; This code runs when an Application Processor (AP) receives a STARTUP IPI.
; It must be located below 1MB (we use 0x8000) because APs start in real mode.
;
; The startup sequence:
; 1. Real Mode (16-bit) - Initialize segments, enable A20
; 2. Protected Mode (32-bit) - Load GDT, enable protected mode
; 3. Long Mode (64-bit) - Enable PAE, load page tables, enable long mode
; 4. Jump to C entry point

[BITS 16]

; Trampoline code starts here
; This will be copied to AP_TRAMPOLINE_ADDR (0x8000)
SECTION .text

global ap_trampoline_start
global ap_trampoline_end

ap_trampoline_start:
    ; Real mode startup
    ; CS:IP = 0x0800:0x0000 (SIPI vector 0x08 -> address 0x8000)
    
    cli                         ; Disable interrupts
    cld                         ; Clear direction flag
    
    ; Set up segments (DS = CS = 0x0800)
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7000              ; Temporary stack below trampoline
    mov dx, 0x3F8
    mov al, 0x41
    out dx, al
.flush1:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .flush1
    
    ; Get this CPU APIC ID (for debugging)
    ; CPUID.1.EBX[31:24] = APIC ID
    mov eax, 1
    cpuid
    shr ebx, 24                 ; APIC ID in BL
    
    ; Enable A20 line (fast method via port 0x92)
    in al, 0x92
    or al, 2
    and al, 0xFE                ; Make sure bit 0 is clear (no reset)
    out 0x92, al
    
    ; Load 32-bit GDT
    lgdt [trampoline_gdt_ptr - ap_trampoline_start]
    
    ; Enable protected mode
    mov eax, cr0
    or eax, 1                   ; Set PE bit
    mov cr0, eax
    
    ; Far jump to protected mode code (flush pipeline)
    jmp dword 0x08:(ap_pm_entry - ap_trampoline_start + 0x8000)

[BITS 32]
ap_pm_entry:
    ; Now in 32-bit protected mode
    
    ; Set up data segments
    mov ax, 0x10                ; Data segment selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    mov dx, 0x3F8
    mov al, 0x50
    out dx, al
.flush2:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .flush2
    ; Get trampoline data (at offset 0x100 from trampoline start)
    mov edi, 0x8E00             ; trampoline data (0x8000 + 0xE00, past code)
    
    ; Load page tables (PML4 physical address)
    mov eax, [edi]              ; tdata->pml4_phys (low 32 bits)
    mov cr3, eax
    mov dx, 0x3F8
    mov al, 0x31
    out dx, al
.flush3:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .flush3
    
    ; Enable PAE (required for long mode)
    mov eax, cr4
    or eax, (1 << 5)            ; Set PAE bit
    mov cr4, eax
    mov dx, 0x3F8
    mov al, 0x32
    out dx, al
.flush4:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .flush4
    
    ; Enable long mode via EFER MSR
    mov ecx, 0xC0000080         ; EFER MSR
    rdmsr
    or eax, (1 << 8)            ; Set LME (Long Mode Enable)
    wrmsr
    mov dx, 0x3F8
    mov al, 0x33
    out dx, al
.flush5:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .flush5
    
    ; Enable paging (enters long mode with paging)
    mov eax, cr0
    or eax, (1 << 31)           ; Set PG bit
    mov cr0, eax
    mov dx, 0x3F8
    mov al, 0x34
    out dx, al
.flush6:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .flush6
    
    ; Load 64-bit GDT
    lgdt [trampoline_gdt64_ptr - ap_trampoline_start + 0x8000]
    mov dx, 0x3F8
    mov al, 0x35
    out dx, al
.flush7:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .flush7
    
    ; Far jump to 64-bit code
    jmp dword 0x08:(ap_lm_entry - ap_trampoline_start + 0x8000)

[BITS 64]
ap_lm_entry:
    ; Now in 64-bit long mode!
    
    ; Set up data segments
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    
    mov dx, 0x3F8
    mov al, 0x4C
    out dx, al
.flush8:
    mov dx, 0x3FD
    in al, dx
    test al, 0x20
    jz .flush8
    ; Get trampoline data
    mov rdi, 0x8E00             ; trampoline data (0x8000 + 0xE00, past code)
    
    ; Load stack pointer
    mov rsp, [rdi + 8]          ; tdata->stack_top (offset 8)
    
    ; Align stack to 16 bytes
    and rsp, ~0xF
    
    ; Get C entry point
    mov rax, [rdi + 16]         ; tdata->ap_entry_addr (offset 16)
    
    ; Jump to C entry point
    ; ap_entry() takes no arguments
    xor rdi, rdi
    xor rsi, rsi
    
    ; Clear other registers for clean state
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    
    ; Call C entry point
    call rax
    
    ; Should never return, but if it does, halt
.halt:
    cli
    hlt
    jmp .halt

; Align data to avoid issues
align 16

; ============================================================================
; 32-bit GDT for transitioning to protected mode
; ============================================================================

trampoline_gdt:
    ; Null descriptor
    dq 0x0000000000000000
    
    ; Code segment: base=0, limit=4GB, 32-bit, ring 0
    dq 0x00CF9A000000FFFF
    
    ; Data segment: base=0, limit=4GB, 32-bit, ring 0
    dq 0x00CF92000000FFFF

trampoline_gdt_ptr:
    dw trampoline_gdt_ptr - trampoline_gdt - 1
    dd trampoline_gdt - ap_trampoline_start + 0x8000

; ============================================================================
; 64-bit GDT for long mode
; ============================================================================

align 16
trampoline_gdt64:
    ; Null descriptor
    dq 0x0000000000000000
    
    ; Code segment: 64-bit, ring 0
    dq 0x00AF9A000000FFFF
    
    ; Data segment: 64-bit, ring 0
    dq 0x00CF92000000FFFF

trampoline_gdt64_ptr:
    dw trampoline_gdt64_ptr - trampoline_gdt64 - 1
    dq trampoline_gdt64 - ap_trampoline_start + 0x8000

; Pad to ensure we dont overlap with data area
align 256

ap_trampoline_end:
