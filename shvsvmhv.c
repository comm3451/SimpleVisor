/*++

Copyright (c) Alex Ionescu.  All rights reserved.
Copyright (c) SimpleVisor contributors.  AMD64 SVM port.

Module Name:

    shvsvmhv.c

Abstract:

    This module implements the AMD64 (AMD-V / SVM) VM-exit loop and its
    handlers. Unlike Intel VT-x, a VM exit on AMD returns inline after the
    VMRUN instruction, so the host runs a small loop that re-enters the guest
    after each exit.

Environment:

    Hypervisor mode only.

--*/

#include "shv.h"

DECLSPEC_NORETURN
VOID
ShvSvmResumeGuestWithoutHypervisor (
    _In_ PSHV_VP_DATA VpData
    )
{
    PVMCB vmcb = &VpData->GuestVmcb;
    PSHV_GUEST_REGISTERS regs = &VpData->GuestRegs;
    CONTEXT context;

    //
    // Capture the current register/XMM state (the guest and host share XMM, so
    // this snapshots the guest's floating point state), then overwrite the
    // architectural registers with the values the guest expects on resume.
    //
    ShvOsCaptureContext(&context);

    context.Rbx = regs->Rbx;
    context.Rcx = regs->Rcx;
    context.Rdx = regs->Rdx;
    context.Rbp = regs->Rbp;
    context.Rsi = regs->Rsi;
    context.Rdi = regs->Rdi;
    context.R8 = regs->R8;
    context.R9 = regs->R9;
    context.R10 = regs->R10;
    context.R11 = regs->R11;
    context.R12 = regs->R12;
    context.R13 = regs->R13;
    context.R14 = regs->R14;
    context.R15 = regs->R15;
    context.Rax = vmcb->StateSaveArea.Rax;
    context.Rsp = vmcb->StateSaveArea.Rsp;
    context.Rip = vmcb->StateSaveArea.Rip;
    context.EFlags = (UINT32)vmcb->StateSaveArea.Rflags;

    //
    // Restore the descriptor table limits (which SVM does not fully account
    // for versus what PatchGuard expects), then disable SVM and hand the
    // logical processor back to the guest at the recorded RIP. With SVM
    // disabled the Global Interrupt Flag no longer applies, so masking
    // interrupts here and letting the restore re-enable them via the guest
    // RFLAGS keeps the hand-off atomic.
    //
    ShvOsUnprepareProcessor(VpData);
    _disable();
    __writemsr(MSR_EFER, __readmsr(MSR_EFER) & ~EFER_SVME);
    __writecr3(vmcb->StateSaveArea.Cr3);
    ShvOsRestoreContext(&context);
}

VOID
ShvSvmHandleCpuid (
    _In_ PSHV_VP_DATA VpData
    )
{
    PVMCB vmcb = &VpData->GuestVmcb;
    PSHV_GUEST_REGISTERS regs = &VpData->GuestRegs;
    INT32 cpuInfo[4];
    UINT64 leaf = vmcb->StateSaveArea.Rax;
    UINT64 subLeaf = regs->Rcx;

    //
    // The magic leaf, issued from ring 0, requests an unload. Return the per-VP
    // data pointer in RAX:RBX and a confirmation value in RCX, advance past the
    // CPUID, and resume the guest with the hypervisor disabled.
    //
    if ((leaf == 0x41414141) &&
        (subLeaf == 0x42424242) &&
        (vmcb->StateSaveArea.Cpl == DPL_SYSTEM))
    {
        vmcb->StateSaveArea.Rax = (uintptr_t)VpData >> 32;
        regs->Rbx = (uintptr_t)VpData & 0xFFFFFFFF;
        regs->Rcx = 0x43434343;
        vmcb->StateSaveArea.Rip += 2;
        ShvSvmResumeGuestWithoutHypervisor(VpData);
    }

    //
    // Otherwise issue the real CPUID on this processor.
    //
    __cpuidex(cpuInfo, (INT32)leaf, (INT32)subLeaf);

    //
    // Advertise the hypervisor via the reserved present bit, and answer the
    // interface query with the SimpleVisor signature.
    //
    if (leaf == 1)
    {
        cpuInfo[2] |= HYPERV_HYPERVISOR_PRESENT_BIT;
    }
    else if (leaf == HYPERV_CPUID_INTERFACE)
    {
        cpuInfo[0] = ' vhS';
    }

    //
    // Return the results. RAX lives in the VMCB; the rest live in the register
    // block restored around VMRUN.
    //
    vmcb->StateSaveArea.Rax = (UINT32)cpuInfo[0];
    regs->Rbx = (UINT32)cpuInfo[1];
    regs->Rcx = (UINT32)cpuInfo[2];
    regs->Rdx = (UINT32)cpuInfo[3];

    //
    // Advance past the two-byte CPUID instruction.
    //
    vmcb->StateSaveArea.Rip += 2;
}

VOID
ShvSvmHandleExit (
    _In_ PSHV_VP_DATA VpData
    )
{
    PVMCB vmcb = &VpData->GuestVmcb;

    //
    // Decode the exit and dispatch. With only CPUID (and the mandatory VMRUN)
    // intercepted, CPUID is the only exit we expect to service.
    //
    switch (vmcb->ControlArea.ExitCode)
    {
    case VMEXIT_CPUID:
        ShvSvmHandleCpuid(VpData);
        break;
    default:
        //
        // Anything else is a state we cannot run (most plausibly
        // VMEXIT_INVALID from a VMCB the processor rejected). Rather than spin
        // re-entering a guest that cannot run, disable SVM and resume. On the
        // very first entry this returns into ShvVpInitialize with the
        // Alignment Check flag set but the hypervisor absent, which the loader
        // detects and reports as a failure.
        //
        ShvSvmResumeGuestWithoutHypervisor(VpData);
        break;
    }
}

DECLSPEC_NORETURN
VOID
ShvSvmVmexitLoop (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // Snapshot the host's FS/GS/TR/LDTR and SYSCALL/SYSENTER MSRs so they can
    // be reloaded after each exit (VMRUN does not restore them for the host).
    //
    ShvSvmVmsave(VpData->HostVmcbPa);

    for (;;)
    {
        //
        // ShvSvmRun loads the guest system state and general purpose registers,
        // executes VMRUN, and on the resulting VM exit saves the guest state
        // back and reloads the host system state.
        //
        ShvSvmRun(&VpData->GuestRegs,
                  VpData->GuestVmcbPa,
                  VpData->HostVmcbPa);

        //
        // Service the exit. On an unload this does not return.
        //
        ShvSvmHandleExit(VpData);
    }
}
