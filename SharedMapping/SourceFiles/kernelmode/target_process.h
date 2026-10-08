#ifndef TARGET_PROCESS_H_
#define TARGET_PROCESS_H_

#include "driver.h"

// Takes a reference on success; release with SmReleaseTargetProcess.
NTSTATUS SmLookupTargetProcess(ULONG Pid, PEPROCESS* Process);

VOID SmReleaseTargetProcess(PEPROCESS Process);

// Reads DirectoryTableBase / UserDirectoryTableBase through the per-build
// offset table. The user DTB is 0 when KVA shadow is off for the process
// (under active KPTI it holds the PCID-1 marker, not a table address).
VOID SmReadTargetDtbs(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets,
                      UINT64* KernelDtb, UINT64* UserDtb);

// Best-effort exe image base (EPROCESS.SectionBaseAddress): 0 when the
// offset table lacks the field or the read faults.
UINT64 SmReadTargetImageBase(PEPROCESS Process,
                             const SM_KERNEL_OFFSETS* Offsets);

// Best-effort PEB va (EPROCESS.Peb), the module-walk entry point.
UINT64 SmReadTargetPeb(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets);

#endif  // TARGET_PROCESS_H_
