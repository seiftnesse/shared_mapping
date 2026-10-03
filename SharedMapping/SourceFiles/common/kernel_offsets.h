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
} SM_KERNEL_OFFSETS;

#include "kernel_offsets_table.gen.h"

static inline const SM_KERNEL_OFFSETS* SmFindKernelOffsets(uint32_t os_build,
                                                           uint32_t ubr) {
    for (const auto & i : SM_KernelOffsetTable) {
        if (i.OsBuild == os_build &&
            i.Ubr == ubr) {
            return &i;
        }
    }
    return nullptr;
}

#endif  // KERNEL_OFFSETS_H_
