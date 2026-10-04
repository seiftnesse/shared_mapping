#ifndef TARGET_PROCESS_H_
#define TARGET_PROCESS_H_

#include "driver.h"

// Takes a reference on success; release with SmReleaseTargetProcess.
NTSTATUS SmLookupTargetProcess(ULONG Pid, PEPROCESS* Process);

VOID SmReleaseTargetProcess(PEPROCESS Process);

// Reads DirectoryTableBase / UserDirectoryTableBase through the per-build
// offset table. The user DTB is 0 when KVA shadow is off for the process.
VOID SmReadTargetDtbs(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                      UINT64* KernelDtb, UINT64* UserDtb);

#endif  // TARGET_PROCESS_H_
