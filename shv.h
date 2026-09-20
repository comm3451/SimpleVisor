/*++

Copyright (c) Alex Ionescu.  All rights reserved.
Copyright (c) SimpleVisor contributors.  AMD64 SVM port.

Header Name:

    shv.h

Abstract:

    This header defines the structures and functions of the Simple Hyper Visor
    (AMD64 / SVM edition).

Environment:

    Kernel mode only.

--*/

#pragma once
#pragma warning(disable:4201)
#pragma warning(disable:4214)

#ifndef __BASE_H__
#include <basetsd.h>
#endif
#define _INC_MALLOC
#include <intrin.h>
#include "ntint.h"
#include "shv_x.h"

typedef struct _SHV_CALLBACK_CONTEXT
{
    UINT64 Cr3;
    volatile long InitCount;
    INT32 FailedCpu;
    INT32 FailureStatus;
} SHV_CALLBACK_CONTEXT, *PSHV_CALLBACK_CONTEXT;

SHV_CPU_CALLBACK ShvVpLoadCallback;
SHV_CPU_CALLBACK ShvVpUnloadCallback;

//
// Virtual processor management (shvvp.c)
//
PSHV_VP_DATA
ShvVpAllocateData (
    _In_ UINT32 CpuCount
    );

VOID
ShvVpFreeData (
    _In_ PSHV_VP_DATA Data,
    _In_ UINT32 CpuCount
    );

DECLSPEC_NORETURN
VOID
ShvVpRestoreAfterLaunch (
    VOID
    );

//
// AMD SVM engine (shvsvm.c / shvsvmhv.c)
//
UINT8
ShvSvmProbe (
    VOID
    );

INT32
ShvSvmLaunchOnVp (
    _In_ PSHV_VP_DATA VpData
    );

VOID
ShvSvmSetupVmcb (
    _In_ PSHV_VP_DATA VpData
    );

VOID
ShvSvmNptInitialize (
    _In_ PSHV_VP_DATA VpData
    );

VOID
ShvSvmFillSegment (
    _Out_ PVMCB_SEGMENT Segment,
    _In_ VOID* GdtBase,
    _In_ UINT16 Selector
    );

DECLSPEC_NORETURN
VOID
ShvSvmVmexitLoop (
    _In_ PSHV_VP_DATA VpData
    );

VOID
ShvSvmHandleExit (
    _In_ PSHV_VP_DATA VpData
    );

//
// AMD SVM assembly support (shvsvmx64.asm)
//
DECLSPEC_NORETURN
VOID
ShvSvmLaunch (
    _In_ PSHV_VP_DATA VpData
    );

VOID
ShvSvmRun (
    _In_ PSHV_GUEST_REGISTERS Regs,
    _In_ UINT64 GuestVmcbPa,
    _In_ UINT64 HostVmcbPa
    );

VOID
ShvSvmVmsave (
    _In_ UINT64 VmcbPa
    );

VOID
ShvSvmVmload (
    _In_ UINT64 VmcbPa
    );

VOID
ShvSvmStgi (
    VOID
    );

//
// OS Layer
//
DECLSPEC_NORETURN
VOID
__cdecl
ShvOsRestoreContext (
    _In_ PCONTEXT ContextRecord
    );

VOID
ShvOsCaptureContext (
    _In_ PCONTEXT ContextRecord
    );

VOID
ShvOsUnprepareProcessor (
    _In_ PSHV_VP_DATA VpData
    );

INT32
ShvOsPrepareProcessor (
    _In_ PSHV_VP_DATA VpData
    );

INT32
ShvOsGetActiveProcessorCount (
    VOID
    );

INT32
ShvOsGetCurrentProcessorNumber (
    VOID
    );

VOID
ShvOsFreeContiguousAlignedMemory (
    _In_ VOID* BaseAddress,
    _In_ size_t Size
    );

VOID*
ShvOsAllocateContigousAlignedMemory (
    _In_ size_t Size
    );

UINT64
ShvOsGetPhysicalAddress (
    _In_ VOID* BaseAddress
    );

VOID
ShvOsDebugPrint (
    _In_ const char* Format,
    ...
    );

VOID
ShvOsRunCallbackOnProcessors (
    _In_ PSHV_CPU_CALLBACK Routine,
    _In_opt_ VOID* Context
    );
