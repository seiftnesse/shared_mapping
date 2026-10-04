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
    // _MMPFN (stage 4).
    uint32_t MmpfnElementSize;
    uint32_t MmpfnShareCountOffset;
    // The share count occupies the bits at and above this shift within the
    // packed field (bits 63:2 => shift 2 on the 19041 line).
    uint32_t MmpfnShareCountShift;
    // RVA of the MmPfnDatabase pointer variable (ntoskrnl PDB publics;
    // PDB section map compresses away the zero-size Pad sections).
    uint32_t MmPfnDatabasePointerRva;
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
