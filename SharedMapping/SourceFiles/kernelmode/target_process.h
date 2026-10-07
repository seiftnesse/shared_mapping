#ifndef TARGET_PROCESS_H_
#define TARGET_PROCESS_H_

#include "driver.h"

// Takes a reference on success; release with SmReleaseTargetProcess.
NTSTATUS SmLookupTargetProcess(ULONG Pid, PEPROCESS* Process);

VOID SmReleaseTargetProcess(PEPROCESS Process);

// KVA-shadow marker bits (IDA, 19041): DTB bit 1 is the SwapContext
// shadow-path marker, AddressPolicy bit 0 the per-CPU bookkeeping one.
#define SM_DTB_KERNEL_SHADOW (1ull << 1)
#define SM_DTB_USER_SHADOW (1ull << 0)

// Reads DirectoryTableBase / UserDirectoryTableBase through the per-build
// offset table. The user DTB is 0 when KVA shadow is off for the process.
VOID SmReadTargetDtbs(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                      UINT64* KernelDtb, UINT64* UserDtb);

// The KVA-shadow markers live INSIDE the DTBs.
// SwapContext (19041 build) tests bit 1 of KPROCESS.DirectoryTableBase to
// decide the shadow path and bit 0 of KPROCESS.AddressPolicy (+0x390) for
// the per-CPU bookkeeping; UserDirectoryTableBase feeds the user CR3 only
// on the shadow path. Zeroing UserDirectoryTableBase alone therefore
// triple-faults a live process (user CR3 loads 0) -- the whole marker set
// must be cleared instead.
typedef struct SM_SHADOW_SAVE {
    UINT64 RawDtb;
    UINT64 UserDtb;
    UINT8 AddressPolicy;
    UINT64 ShadowDtbPointer;
} SM_SHADOW_SAVE;

VOID SmSaveShadowState(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                       SM_SHADOW_SAVE* Out);

// Clears the shadow markers: kernel DTB bit 1, UserDirectoryTableBase,
// AddressPolicy bit 0. Takes effect at the next context switch.
VOID SmClearShadowState(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets);

VOID SmRestoreShadowState(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                          const SM_SHADOW_SAVE* Saved);

// Diagnostic read of the shadow markers without modifying anything.
VOID SmReadShadowMarkers(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                         UINT8* AddressPolicy, UINT64* ShadowDtbPointer);

#endif  // TARGET_PROCESS_H_
