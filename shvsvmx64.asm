;++
;
; Copyright (c) Alex Ionescu.  All rights reserved.
; Copyright (c) SimpleVisor contributors.  AMD64 SVM port.
;
; Module:
;
;    shvsvmx64.asm
;
; Abstract:
;
;    This module implements the AMD64 (AMD-V / SVM) assembly support: the stack
;    switch into the VM-exit loop, the VMRUN round-trip that preserves the
;    guest general purpose registers, and a VMSAVE helper.
;
;    The SVM instructions are emitted as raw bytes so the module assembles with
;    any version of ml64:
;        VMRUN  rax  = 0F 01 D8
;        VMLOAD rax  = 0F 01 DA
;        VMSAVE rax  = 0F 01 DB
;
; Environment:
;
;    Kernel mode only.
;
;--

; Must match KERNEL_STACK_SIZE in ntint.h (24 * 1024).
KERNEL_STACK_SIZE   equ 06000h

; Offsets into SHV_GUEST_REGISTERS (svm.h). Must stay in sync with that struct.
GrRbx   equ 000h
GrRcx   equ 008h
GrRdx   equ 010h
GrRbp   equ 018h
GrRsi   equ 020h
GrRdi   equ 028h
GrR8    equ 030h
GrR9    equ 038h
GrR10   equ 040h
GrR11   equ 048h
GrR12   equ 050h
GrR13   equ 058h
GrR14   equ 060h
GrR15   equ 068h

    .code

    extern ShvSvmVmexitLoop:proc

;
; VOID ShvSvmLaunch(PSHV_VP_DATA VpData);
;
; Switches to the top of the per-VP hypervisor stack (ShvStackLimit is the
; first member of SHV_VP_DATA, at offset 0) and enters the VM-exit loop. Does
; not return.
;
    ShvSvmLaunch PROC
    lea     rax, [rcx + KERNEL_STACK_SIZE]  ; top of the hypervisor stack
    and     rax, 0FFFFFFFFFFFFFFF0h         ; keep it 16-byte aligned
    mov     rsp, rax
    sub     rsp, 20h                        ; home space for the callee
    ; rcx still holds VpData
    call    ShvSvmVmexitLoop                ; never returns
    int     3                               ; trap if it ever does
    ShvSvmLaunch ENDP

;
; VOID ShvSvmRun(PSHV_GUEST_REGISTERS Regs, UINT64 GuestVmcbPa, UINT64 HostVmcbPa);
;   rcx = Regs, rdx = GuestVmcbPa, r8 = HostVmcbPa
;
; Loads the guest system state and general purpose registers, runs the guest
; with VMRUN, and on the resulting VM exit saves the guest registers back and
; reloads the host system state. Host non-volatile registers are preserved so
; this behaves as an ordinary function call to the C loop.
;
    ShvSvmRun PROC
    push    rbx
    push    rbp
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    push    r8                      ; [rsp+10h] = HostVmcbPa
    push    rdx                     ; [rsp+08h] = GuestVmcbPa
    push    rcx                     ; [rsp+00h] = Regs

    ; Load the guest FS/GS/TR/LDTR and SYSCALL/SYSENTER MSRs.
    mov     rax, rdx                ; GuestVmcbPa
    db      0Fh, 01h, 0DAh          ; vmload rax

    ; Load the guest general purpose registers (rcx currently = Regs).
    mov     rbx, [rcx+GrRbx]
    mov     rbp, [rcx+GrRbp]
    mov     rsi, [rcx+GrRsi]
    mov     rdi, [rcx+GrRdi]
    mov     r8,  [rcx+GrR8]
    mov     r9,  [rcx+GrR9]
    mov     r10, [rcx+GrR10]
    mov     r11, [rcx+GrR11]
    mov     r12, [rcx+GrR12]
    mov     r13, [rcx+GrR13]
    mov     r14, [rcx+GrR14]
    mov     r15, [rcx+GrR15]
    mov     rdx, [rcx+GrRdx]
    mov     rax, [rsp+08h]          ; GuestVmcbPa for VMRUN
    mov     rcx, [rcx+GrRcx]        ; guest rcx last (destroys the Regs pointer)

    db      0Fh, 01h, 0D8h          ; vmrun rax

    ; VMRUN restores the host RSP on exit, so [rsp] still points at Regs.
    mov     rax, [rsp]              ; Regs
    mov     [rax+GrRbx], rbx
    mov     [rax+GrRcx], rcx        ; guest rcx
    mov     [rax+GrRdx], rdx        ; guest rdx
    mov     [rax+GrRbp], rbp
    mov     [rax+GrRsi], rsi
    mov     [rax+GrRdi], rdi
    mov     [rax+GrR8],  r8
    mov     [rax+GrR9],  r9
    mov     [rax+GrR10], r10
    mov     [rax+GrR11], r11
    mov     [rax+GrR12], r12
    mov     [rax+GrR13], r13
    mov     [rax+GrR14], r14
    mov     [rax+GrR15], r15

    ; Save the guest system state, then reload the host system state.
    mov     rax, [rsp+08h]          ; GuestVmcbPa
    db      0Fh, 01h, 0DBh          ; vmsave rax
    mov     rax, [rsp+10h]          ; HostVmcbPa
    db      0Fh, 01h, 0DAh          ; vmload rax

    add     rsp, 18h                ; drop Regs / GuestVmcbPa / HostVmcbPa
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rdi
    pop     rsi
    pop     rbp
    pop     rbx
    ret
    ShvSvmRun ENDP

;
; VOID ShvSvmVmsave(UINT64 VmcbPa);
;   rcx = physical address of a VMCB
;
; Saves the current FS/GS/TR/LDTR and SYSCALL/SYSENTER MSRs into the VMCB.
;
    ShvSvmVmsave PROC
    mov     rax, rcx
    db      0Fh, 01h, 0DBh          ; vmsave rax
    ret
    ShvSvmVmsave ENDP

    end
