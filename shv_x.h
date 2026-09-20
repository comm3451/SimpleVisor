/*++

Copyright (c) Alex Ionescu.  All rights reserved.
Copyright (c) SimpleVisor contributors.  AMD64 SVM port.

Header Name:

    shv_x.h

Abstract:

    This header defines the externally visible structures and functions of the
    Simple Hyper Visor which are shared between the OS layer and SimpleVisor.

Environment:

    Kernel mode only.

--*/

#pragma once

#include "svm.h"

#define SHV_STATUS_SUCCESS          0
#define SHV_STATUS_NOT_AVAILABLE    -1
#define SHV_STATUS_NO_RESOURCES     -2
#define SHV_STATUS_NOT_PRESENT      -3

struct _SHV_CALLBACK_CONTEXT;

typedef
void
SHV_CPU_CALLBACK (
    _In_ struct _SHV_CALLBACK_CONTEXT* Context
    );
typedef SHV_CPU_CALLBACK *PSHV_CPU_CALLBACK;

typedef struct _SHV_SPECIAL_REGISTERS
{
    UINT64 Cr0;
    UINT64 Cr3;
    UINT64 Cr4;
    UINT64 MsrGsBase;
    UINT16 Tr;
    UINT16 Ldtr;
    UINT64 DebugControl;
    UINT64 KernelDr7;
    KDESCRIPTOR Idtr;
    KDESCRIPTOR Gdtr;
} SHV_SPECIAL_REGISTERS, *PSHV_SPECIAL_REGISTERS;

typedef struct _SHV_VP_DATA
{
    //
    // The per-VP hypervisor stack. It overlaps the register capture area,
    // which is only needed while building the VMCB (before the stack is used).
    //
    union
    {
        DECLSPEC_ALIGN(PAGE_SIZE) UINT8 ShvStackLimit[KERNEL_STACK_SIZE];
        struct
        {
            SHV_SPECIAL_REGISTERS SpecialRegisters;
            CONTEXT ContextFrame;
        };
    };

    //
    // SVM control structures. These live outside the stack union so the host
    // stack cannot clobber them at runtime.
    //
    DECLSPEC_ALIGN(PAGE_SIZE) VMCB GuestVmcb;
    DECLSPEC_ALIGN(PAGE_SIZE) VMCB HostVmcb;
    DECLSPEC_ALIGN(PAGE_SIZE) UINT8 HostStateArea[PAGE_SIZE];

    //
    // Nested page tables: a 512GB identity map built from 2MB pages. These use
    // the ordinary x64 page-table format. NptPde is 2MB of tables, so this is
    // the bulk of the per-VP allocation.
    //
    DECLSPEC_ALIGN(PAGE_SIZE) UINT64 NptPml4[NPT_PML4E_COUNT];
    DECLSPEC_ALIGN(PAGE_SIZE) UINT64 NptPdpt[NPT_PDPTE_COUNT];
    DECLSPEC_ALIGN(PAGE_SIZE) UINT64 NptPde[NPT_PDPTE_COUNT][NPT_PDE_COUNT];

    SHV_GUEST_REGISTERS GuestRegs;

    UINT64 GuestVmcbPa;
    UINT64 HostVmcbPa;
    UINT64 HostStateAreaPa;
    UINT64 NptPml4Pa;
} SHV_VP_DATA, *PSHV_VP_DATA;

VOID
_sldt (
    _In_ UINT16* Ldtr
    );

VOID
_str (
    _In_ UINT16* Tr
    );

VOID
__lgdt (
    _In_ VOID* Gdtr
    );

INT32
ShvLoad (
    VOID
    );

VOID
ShvUnload (
    VOID
    );
