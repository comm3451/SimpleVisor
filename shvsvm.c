/*++

Copyright (c) Alex Ionescu.  All rights reserved.
Copyright (c) SimpleVisor contributors.  AMD64 SVM port.

Module Name:

    shvsvm.c

Abstract:

    This module implements the AMD64 (AMD-V / SVM) specific routines: feature
    detection, enabling SVM, and building the guest VMCB from the captured
    processor state.

Environment:

    Kernel mode only, IRQL DISPATCH_LEVEL.

--*/

#include "shv.h"

VOID
ShvSvmFillSegment (
    _Out_ PVMCB_SEGMENT Segment,
    _In_ VOID* GdtBase,
    _In_ UINT16 Selector
    )
{
    PKGDTENTRY64 gdtEntry;

    //
    // Record the selector directly.
    //
    Segment->Selector = Selector;

    //
    // A null selector, or one that references the LDT, is treated as unusable.
    //
    if ((Selector == 0) || ((Selector & SELECTOR_TABLE_INDEX) != 0))
    {
        Segment->Limit = 0;
        Segment->Base = 0;
        Segment->Attrib = 0;
        return;
    }

    //
    // Locate the descriptor in the GDT, masking off the RPL bits.
    //
    gdtEntry = (PKGDTENTRY64)((uintptr_t)GdtBase + (Selector & ~RPL_MASK));

    //
    // Read the segment limit through the architectural LSL helper.
    //
    Segment->Limit = __segmentlimit(Selector);

    //
    // Build the base address. System (non code/data) descriptors are 16 bytes
    // wide and carry the upper 32 bits of the base.
    //
    Segment->Base = ((gdtEntry->Bytes.BaseHigh << 24) |
                     (gdtEntry->Bytes.BaseMiddle << 16) |
                     (gdtEntry->BaseLow)) & 0xFFFFFFFF;
    if ((gdtEntry->Bits.Type & 0x10) == 0)
    {
        Segment->Base |= ((uintptr_t)gdtEntry->BaseUpper << 32);
    }

    //
    // Pack the AMD attribute field: the low byte is descriptor byte 5 and the
    // high nibble is the high nibble of descriptor byte 6.
    //
    Segment->Attrib = (UINT16)(gdtEntry->Bytes.Flags1 |
                               ((gdtEntry->Bytes.Flags2 >> 4) << 8));

    //
    // A non-present descriptor is unusable.
    //
    if (gdtEntry->Bits.Present == 0)
    {
        Segment->Attrib = 0;
    }
}

UINT8
ShvSvmProbe (
    VOID
    )
{
    INT32 cpuInfo[4];
    UINT64 vmCr;

    //
    // Is the SVM feature present on this processor?
    //
    __cpuid(cpuInfo, CPUID_EXTENDED_FEATURES);
    if ((cpuInfo[2] & CPUID_EXT_FEATURE_SVM) == 0)
    {
        return FALSE;
    }

    //
    // Has the firmware disabled SVM? If so, do not attempt to override it.
    //
    vmCr = __readmsr(MSR_VM_CR);
    if ((vmCr & VM_CR_SVMDIS) != 0)
    {
        return FALSE;
    }

    //
    // Both the hardware and the firmware allow SVM.
    //
    return TRUE;
}

VOID
ShvSvmSetupVmcb (
    _In_ PSHV_VP_DATA VpData
    )
{
    PSHV_SPECIAL_REGISTERS state = &VpData->SpecialRegisters;
    PCONTEXT context = &VpData->ContextFrame;
    PVMCB vmcb = &VpData->GuestVmcb;
    PVMCB_CONTROL_AREA control = &vmcb->ControlArea;
    PVMCB_STATE_SAVE_AREA save = &vmcb->StateSaveArea;

    //
    // Snapshot the live FS/GS/TR/LDTR segments and the SYSCALL/SYSENTER MSRs
    // into both VMCBs. This populates the guest's hidden segment state, and
    // gives the host a block to reload with after every VM exit.
    //
    ShvSvmVmsave(VpData->GuestVmcbPa);
    ShvSvmVmsave(VpData->HostVmcbPa);

    //
    // Intercept CPUID, which is used both to advertise the hypervisor and as
    // the control channel for unloading. The VMRUN intercept is mandatory on
    // AMD for VMRUN to be legal.
    //
    control->InterceptVector4 = SVM_INTERCEPT_CPUID;
    control->InterceptVector5 = SVM_INTERCEPT_VMRUN;

    //
    // A non-zero ASID is required. Nested paging is left disabled: the guest
    // keeps its own page tables and guest-physical addresses map straight
    // through to system-physical addresses.
    //
    control->GuestAsid = 1;
    control->TlbControl = 0;
    control->NpEnable = 0;
    control->VmcbClean = 0;

    //
    // Fill the guest segments that VMSAVE does not cover (ES/CS/SS/DS).
    //
    ShvSvmFillSegment(&save->Es, state->Gdtr.Base, context->SegEs);
    ShvSvmFillSegment(&save->Cs, state->Gdtr.Base, context->SegCs);
    ShvSvmFillSegment(&save->Ss, state->Gdtr.Base, context->SegSs);
    ShvSvmFillSegment(&save->Ds, state->Gdtr.Base, context->SegDs);

    //
    // The descriptor tables carry only base and limit.
    //
    save->Gdtr.Base = (uintptr_t)state->Gdtr.Base;
    save->Gdtr.Limit = state->Gdtr.Limit;
    save->Idtr.Base = (uintptr_t)state->Idtr.Base;
    save->Idtr.Limit = state->Idtr.Limit;

    //
    // Control registers, EFER (SVME must stay set for the guest), PAT, DR7 and
    // the ring level, all taken from the interrupted host.
    //
    save->Cr0 = state->Cr0;
    save->Cr3 = state->Cr3;
    save->Cr4 = state->Cr4;
    save->Efer = __readmsr(MSR_EFER) | EFER_SVME;
    save->GPat = __readmsr(MSR_PAT);
    save->Dr7 = state->KernelDr7;
    save->Cpl = 0;

    //
    // The guest resumes at ShvVpRestoreAfterLaunch, on the top of the per-VP
    // stack (biased for the CONTEXT it restores). ShvVpRestoreAfterLaunch sets
    // the Alignment Check flag and returns into ShvVpInitialize, which is how
    // the loader learns that virtualization is now live.
    //
    save->Rflags = context->EFlags;
    save->Rsp = (uintptr_t)VpData->ShvStackLimit +
                KERNEL_STACK_SIZE - sizeof(CONTEXT);
    save->Rip = (uintptr_t)ShvVpRestoreAfterLaunch;
    save->Rax = 0;
}

INT32
ShvSvmLaunchOnVp (
    _In_ PSHV_VP_DATA VpData
    )
{
    //
    // Cache the physical addresses of the per-VP SVM structures.
    //
    VpData->GuestVmcbPa = ShvOsGetPhysicalAddress(&VpData->GuestVmcb);
    VpData->HostVmcbPa = ShvOsGetPhysicalAddress(&VpData->HostVmcb);
    VpData->HostStateAreaPa = ShvOsGetPhysicalAddress(&VpData->HostStateArea);

    //
    // Enable SVM and program the host state-save area MSR.
    //
    __writemsr(MSR_EFER, __readmsr(MSR_EFER) | EFER_SVME);
    __writemsr(MSR_VM_HSAVE_PA, VpData->HostStateAreaPa);

    //
    // Build the guest VMCB from the captured processor state.
    //
    ShvSvmSetupVmcb(VpData);

    //
    // Switch to the hypervisor stack and enter the guest. On success this does
    // not return here: the guest resumes via ShvVpRestoreAfterLaunch, while
    // this logical processor stays in the VM-exit loop. A VMCB that the
    // processor rejects surfaces as VMEXIT_INVALID, which the exit handler
    // turns into a clean, reported failure.
    //
    ShvSvmLaunch(VpData);

    //
    // Unreachable.
    //
    return SHV_STATUS_NOT_AVAILABLE;
}
