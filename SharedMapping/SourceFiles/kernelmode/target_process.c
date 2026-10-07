#include <ntifs.h>

#include "target_process.h"

NTSTATUS SmLookupTargetProcess(ULONG Pid, PEPROCESS* Process) {
    return PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)Pid, Process);
}

VOID SmReleaseTargetProcess(PEPROCESS Process) {
    ObDereferenceObject(Process);
}

VOID SmReadTargetDtbs(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                      UINT64* KernelDtb, UINT64* UserDtb) {
    volatile UINT64* field =
        (volatile UINT64*)((UINT8*)Process + Offsets->DirectoryTableBaseOffset);
    *KernelDtb = *field;
    field = (volatile UINT64*)((UINT8*)Process +
                               Offsets->UserDirectoryTableBaseOffset);
    *UserDtb = *field;
}

VOID SmWriteUserDtb(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                    UINT64 Value) {
    volatile UINT64* field =
        (volatile UINT64*)((UINT8*)Process +
                           Offsets->UserDirectoryTableBaseOffset);
    *field = Value;
}

static volatile UINT8* AddressPolicyOf(PEPROCESS Process,
                                       const SM_KERNEL_OFFSETS* Offsets) {
    return (volatile UINT8*)((UINT8*)Process + Offsets->AddressPolicyOffset);
}

VOID SmSaveShadowState(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                       SM_SHADOW_SAVE* Out) {
    SmReadTargetDtbs(Process, Offsets, &Out->RawDtb, &Out->UserDtb);
    Out->AddressPolicy = *AddressPolicyOf(Process, Offsets);
    Out->ShadowDtbPointer =
        *(volatile UINT64*)((UINT8*)Process + Offsets->ShadowDtbPointerOffset);
}

// Read-only view of the KVA-shadow markers for logging: AddressPolicy bit 0
// and the shadow-PML4 pointer at +0x400 (the fields SmClearShadowState
// manipulates).
VOID SmReadShadowMarkers(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                         UINT8* AddressPolicy, UINT64* ShadowDtbPointer) {
    *AddressPolicy = *AddressPolicyOf(Process, Offsets);
    *ShadowDtbPointer =
        *(volatile UINT64*)((UINT8*)Process + Offsets->ShadowDtbPointerOffset);
}

VOID SmClearShadowState(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets) {
    volatile UINT64* dtb =
        (volatile UINT64*)((UINT8*)Process + Offsets->DirectoryTableBaseOffset);
    *dtb &= ~SM_DTB_KERNEL_SHADOW;
    SmWriteUserDtb(Process, Offsets, 0);
    *AddressPolicyOf(Process, Offsets) &= ~(UINT8)SM_DTB_USER_SHADOW;
    // The real shadow-PML4 pointer (PDB "Spare2"): NULL makes
    // MiCheckProcessShadow skip its audit (decompiled gate v5 == 0).
    *(volatile UINT64*)((UINT8*)Process + Offsets->ShadowDtbPointerOffset) = 0;
}

VOID SmRestoreShadowState(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                          const SM_SHADOW_SAVE* Saved) {
    volatile UINT64* dtb =
        (volatile UINT64*)((UINT8*)Process + Offsets->DirectoryTableBaseOffset);
    *dtb =
        (*dtb & ~SM_DTB_KERNEL_SHADOW) | (Saved->RawDtb & SM_DTB_KERNEL_SHADOW);
    SmWriteUserDtb(Process, Offsets, Saved->UserDtb);
    *AddressPolicyOf(Process, Offsets) =
        (*AddressPolicyOf(Process, Offsets) & ~(UINT8)SM_DTB_USER_SHADOW) |
        (Saved->AddressPolicy & SM_DTB_USER_SHADOW);
    *(volatile UINT64*)((UINT8*)Process + Offsets->ShadowDtbPointerOffset) =
        Saved->ShadowDtbPointer;
}
