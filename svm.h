/*++

Copyright (c) Alex Ionescu.  All rights reserved.
Copyright (c) SimpleVisor contributors.  AMD64 SVM port.

Header Name:

    svm.h

Abstract:

    This header defines the MSRs, VMCB layout and helper structures required
    for AMD64 (AMD-V / SVM) support in SimpleVisor. The VMCB field offsets are
    taken directly from the AMD64 Architecture Programmer's Manual, Volume 2,
    Appendix B. Structural static-asserts on the sub-structure sizes guarantee
    that every field lands at its architecturally required offset.

Environment:

    Kernel mode only.

--*/

#pragma once
#pragma warning(disable:4201)
#pragma warning(disable:4214)

//
// Generic x64 / selector helpers
//
#define DPL_USER                3
#define DPL_SYSTEM              0
#define RPL_MASK                3
#define SELECTOR_TABLE_INDEX    0x04
#define EFLAGS_ALIGN_CHECK      0x40000
#ifndef PAGE_SIZE
#define PAGE_SIZE               4096
#endif

//
// Architectural MSRs used by the hypervisor
//
#define MSR_GS_BASE             0xC0000101
#define MSR_DEBUG_CTL           0x1D9
#define MSR_PAT                 0x00000277

//
// AMD SVM specific MSRs and bits
//
#define MSR_EFER                0xC0000080
#define EFER_SVME               (1ULL << 12)
#define MSR_VM_CR               0xC0010114
#define VM_CR_SVMDIS            (1ULL << 4)
#define MSR_VM_HSAVE_PA         0xC0010117

//
// CPUID leaves / bits for SVM feature detection
//
#define CPUID_EXTENDED_FEATURES 0x80000001  // ECX bit 2 == SVM
#define CPUID_EXT_FEATURE_SVM   (1 << 2)
#define CPUID_SVM_FEATURES      0x8000000A  // EDX carries SVM sub-features

//
// Hyper-V style enlightenment CPUID interface, reused to advertise SHV
//
#define HYPERV_CPUID_INTERFACE          0x40000001
#define HYPERV_HYPERVISOR_PRESENT_BIT   0x80000000

//
// VMCB intercept vector 4 (offset 0x00C). CPUID lives at bit 18.
//
#define SVM_INTERCEPT_CPUID     (1UL << 18)

//
// VMCB intercept vector 5 (offset 0x010). AMD requires the VMRUN intercept
// (bit 0) to be set for VMRUN to be legal.
//
#define SVM_INTERCEPT_VMRUN     (1UL << 0)

//
// The subset of #VMEXIT codes SimpleVisor cares about. The full list is in the
// APM; with only CPUID (and the mandatory VMRUN) intercepted we should never
// observe anything else.
//
#define VMEXIT_CPUID            0x72
#define VMEXIT_VMRUN            0x80
#define VMEXIT_NPF              0x400   // Nested page fault (used by NPT hooks)
#define VMEXIT_INVALID          (-1)

//
// Nested paging (NPT). AMD nested page tables use the ordinary x64 page-table
// format, so the identity map below is built from plain PML4/PDPT/PD entries.
//
#define CPUID_SVM_FEATURE_NP    (1 << 0)    // CPUID 8000000Ah, EDX bit 0
#define SVM_NP_ENABLE           (1ULL << 0) // VMCB ControlArea.NpEnable bit 0

#define _2MB                    (2 * 1024 * 1024)
#define NPT_PML4E_COUNT         512
#define NPT_PDPTE_COUNT         512
#define NPT_PDE_COUNT           512

#define NPT_PAGE_PRESENT        (1ULL << 0)
#define NPT_PAGE_WRITE          (1ULL << 1)
#define NPT_PAGE_USER           (1ULL << 2)
#define NPT_PAGE_LARGE          (1ULL << 7)  // 2MB page (in a PDE)
#define NPT_PAGE_NX             (1ULL << 63) // No-execute

typedef struct _KDESCRIPTOR
{
    UINT16 Pad[3];
    UINT16 Limit;
    void* Base;
} KDESCRIPTOR, *PKDESCRIPTOR;

typedef union _KGDTENTRY64
{
    struct
    {
        UINT16 LimitLow;
        UINT16 BaseLow;
        union
        {
            struct
            {
                UINT8 BaseMiddle;
                UINT8 Flags1;
                UINT8 Flags2;
                UINT8 BaseHigh;
            } Bytes;
            struct
            {
                UINT32 BaseMiddle : 8;
                UINT32 Type : 5;
                UINT32 Dpl : 2;
                UINT32 Present : 1;
                UINT32 LimitHigh : 4;
                UINT32 System : 1;
                UINT32 LongMode : 1;
                UINT32 DefaultBig : 1;
                UINT32 Granularity : 1;
                UINT32 BaseHigh : 8;
            } Bits;
        };
        UINT32 BaseUpper;
        UINT32 MustBeZero;
    };
    struct
    {
        INT64 DataLow;
        INT64 DataHigh;
    };
} KGDTENTRY64, *PKGDTENTRY64;

//
// A VMCB segment slot: selector, packed attributes, limit and base. AMD packs
// the descriptor attribute bits as bits [55:52,47:40] of the GDT descriptor,
// i.e. the low byte is descriptor byte 5 and the high nibble is the high
// nibble of descriptor byte 6.
//
typedef struct _VMCB_SEGMENT
{
    UINT16 Selector;
    UINT16 Attrib;
    UINT32 Limit;
    UINT64 Base;
} VMCB_SEGMENT, *PVMCB_SEGMENT;
C_ASSERT(sizeof(VMCB_SEGMENT) == 16);

//
// VMCB control area (bytes 0x000 - 0x3FF). Reserved arrays keep every named
// field at its APM offset; the size assert below proves it.
//
typedef struct _VMCB_CONTROL_AREA
{
    UINT16 InterceptCrRead;                 // 0x000
    UINT16 InterceptCrWrite;                // 0x002
    UINT16 InterceptDrRead;                 // 0x004
    UINT16 InterceptDrWrite;                // 0x006
    UINT32 InterceptException;              // 0x008
    UINT32 InterceptVector4;                // 0x00C (CPUID = bit 18)
    UINT32 InterceptVector5;                // 0x010 (VMRUN = bit 0)
    UINT8  Reserved1[0x03C - 0x014];        // 0x014
    UINT16 PauseFilterThreshold;            // 0x03C
    UINT16 PauseFilterCount;                // 0x03E
    UINT64 IopmBasePa;                      // 0x040
    UINT64 MsrpmBasePa;                     // 0x048
    UINT64 TscOffset;                       // 0x050
    UINT32 GuestAsid;                       // 0x058
    UINT8  TlbControl;                      // 0x05C
    UINT8  Reserved2[3];                    // 0x05D
    UINT64 VIntr;                           // 0x060
    UINT64 InterruptShadow;                 // 0x068
    UINT64 ExitCode;                        // 0x070
    UINT64 ExitInfo1;                       // 0x078
    UINT64 ExitInfo2;                       // 0x080
    UINT64 ExitIntInfo;                     // 0x088
    UINT64 NpEnable;                        // 0x090
    UINT64 AvicApicBar;                     // 0x098
    UINT64 GuestPaOfGhcb;                   // 0x0A0
    UINT64 EventInj;                        // 0x0A8
    UINT64 NCr3;                            // 0x0B0
    UINT64 LbrVirtualizationEnable;         // 0x0B8
    UINT64 VmcbClean;                       // 0x0C0
    UINT64 NRip;                            // 0x0C8
    UINT8  NumOfBytesFetched;               // 0x0D0
    UINT8  GuestInstructionBytes[15];       // 0x0D1
    UINT8  Reserved3[0x400 - 0x0E0];        // 0x0E0
} VMCB_CONTROL_AREA, *PVMCB_CONTROL_AREA;
C_ASSERT(sizeof(VMCB_CONTROL_AREA) == 0x400);

//
// VMCB state save area (starts at VMCB offset 0x400; offsets below are relative
// to that start). VMSAVE/VMLOAD populate FS/GS/TR/LDTR plus the SYSCALL and
// SYSENTER MSRs; the remaining fields are filled by software.
//
typedef struct _VMCB_STATE_SAVE_AREA
{
    VMCB_SEGMENT Es;                        // 0x000
    VMCB_SEGMENT Cs;                        // 0x010
    VMCB_SEGMENT Ss;                        // 0x020
    VMCB_SEGMENT Ds;                        // 0x030
    VMCB_SEGMENT Fs;                        // 0x040
    VMCB_SEGMENT Gs;                        // 0x050
    VMCB_SEGMENT Gdtr;                      // 0x060
    VMCB_SEGMENT Ldtr;                      // 0x070
    VMCB_SEGMENT Idtr;                      // 0x080
    VMCB_SEGMENT Tr;                        // 0x090
    UINT8  Reserved1[0x0CB - 0x0A0];        // 0x0A0
    UINT8  Cpl;                             // 0x0CB
    UINT32 Reserved2;                       // 0x0CC
    UINT64 Efer;                            // 0x0D0
    UINT8  Reserved3[0x148 - 0x0D8];        // 0x0D8
    UINT64 Cr4;                             // 0x148
    UINT64 Cr3;                             // 0x150
    UINT64 Cr0;                             // 0x158
    UINT64 Dr7;                             // 0x160
    UINT64 Dr6;                             // 0x168
    UINT64 Rflags;                          // 0x170
    UINT64 Rip;                             // 0x178
    UINT8  Reserved4[0x1D8 - 0x180];        // 0x180
    UINT64 Rsp;                             // 0x1D8
    UINT8  Reserved5[0x1F8 - 0x1E0];        // 0x1E0
    UINT64 Rax;                             // 0x1F8
    UINT64 Star;                            // 0x200
    UINT64 Lstar;                           // 0x208
    UINT64 Cstar;                           // 0x210
    UINT64 Sfmask;                          // 0x218
    UINT64 KernelGsBase;                    // 0x220
    UINT64 SysenterCs;                      // 0x228
    UINT64 SysenterEsp;                     // 0x230
    UINT64 SysenterEip;                     // 0x238
    UINT64 Cr2;                             // 0x240
    UINT8  Reserved6[0x268 - 0x248];        // 0x248
    UINT64 GPat;                            // 0x268
    UINT64 DbgCtl;                          // 0x270
    UINT64 BrFrom;                          // 0x278
    UINT64 BrTo;                            // 0x280
    UINT64 LastExcpFrom;                    // 0x288
    UINT64 LastExcpTo;                      // 0x290
} VMCB_STATE_SAVE_AREA, *PVMCB_STATE_SAVE_AREA;
C_ASSERT(sizeof(VMCB_STATE_SAVE_AREA) == 0x298);

//
// The full 4KB VMCB: control area, then state save area, then padding.
//
typedef struct DECLSPEC_ALIGN(PAGE_SIZE) _VMCB
{
    VMCB_CONTROL_AREA ControlArea;                                  // 0x000
    VMCB_STATE_SAVE_AREA StateSaveArea;                             // 0x400
    UINT8 Reserved[PAGE_SIZE - 0x400 - sizeof(VMCB_STATE_SAVE_AREA)];
} VMCB, *PVMCB;
C_ASSERT(sizeof(VMCB) == PAGE_SIZE);

//
// General purpose registers preserved by software around VMRUN. RAX and RSP
// are carried in the VMCB state save area, so they are not stored here. The
// field order must match the offsets used by shvsvmx64.asm.
//
typedef struct _SHV_GUEST_REGISTERS
{
    UINT64 Rbx;     // 0x00
    UINT64 Rcx;     // 0x08
    UINT64 Rdx;     // 0x10
    UINT64 Rbp;     // 0x18
    UINT64 Rsi;     // 0x20
    UINT64 Rdi;     // 0x28
    UINT64 R8;      // 0x30
    UINT64 R9;      // 0x38
    UINT64 R10;     // 0x40
    UINT64 R11;     // 0x48
    UINT64 R12;     // 0x50
    UINT64 R13;     // 0x58
    UINT64 R14;     // 0x60
    UINT64 R15;     // 0x68
} SHV_GUEST_REGISTERS, *PSHV_GUEST_REGISTERS;
C_ASSERT(sizeof(SHV_GUEST_REGISTERS) == 0x70);
