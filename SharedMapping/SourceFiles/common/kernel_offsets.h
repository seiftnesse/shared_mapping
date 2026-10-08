#ifndef KERNEL_OFFSETS_H_
#define KERNEL_OFFSETS_H_

#include <stdint.h>

typedef struct SM_KERNEL_OFFSETS {
    // Runtime key: RtlGetVersion dwBuildNumber + UBR (registry).
    uint32_t OsBuild;
    uint32_t Ubr;
    // _KPROCESS (embedded at +0 of _EPROCESS).
    uint32_t DirectoryTableBaseOffset;
    uint32_t UserDirectoryTableBaseOffset;
    uint32_t KprocessSize;
    // _MMPFN
    uint32_t MmpfnElementSize;
    uint32_t MmpfnShareCountOffset;
    // The share count occupies the bits at and above this shift within the
    // packed field (bits 61:0 => shift 0 on the 19041 line, per
    // MiLockAndIncrementShareCount).
    uint32_t MmpfnShareCountShift;
    // RVA of the MmPfnDatabase pointer variable
    // (PDB section map compresses away the zero-size Pad sections).
    uint32_t MmPfnDatabasePointerRva;
    // Shadow-PML4 chain used by the KPTI dual-write: _EPROCESS.Vm
    // (_MMSUPPORT_FULL) -> .Shared (_MMSUPPORT_SHARED) -> .ShadowMapping
    // (self-map VA of the process's shadow PML4; source: live KD +
    // !pte cross-check with DirBase contents, 19041.6456).
    uint32_t VmOffset;
    uint32_t MmSupportSharedOffset;
    uint32_t ShadowMappingOffset;
    // _EPROCESS.SectionBaseAddress: the exe image base, handed to the
    // client for probing image headers through the mirror
    uint32_t SectionBaseAddressOffset;
    // _EPROCESS.Peb: the target PEB va, the entry point of the client's
    // module walk.
    uint32_t PebOffset;
} SM_KERNEL_OFFSETS;

#include "kernel_offsets_table.gen.h"

static inline const SM_KERNEL_OFFSETS* SmFindKernelOffsets(uint32_t os_build,
                                                           uint32_t ubr) {
    for (uint32_t i = 0; i < (uint32_t)(sizeof(SM_KernelOffsetTable) /
                                        sizeof(SM_KernelOffsetTable[0]));
         ++i) {
        if (SM_KernelOffsetTable[i].OsBuild == os_build &&
            SM_KernelOffsetTable[i].Ubr == ubr) {
            return &SM_KernelOffsetTable[i];
        }
    }
    return 0;
}

#endif  // KERNEL_OFFSETS_H_
