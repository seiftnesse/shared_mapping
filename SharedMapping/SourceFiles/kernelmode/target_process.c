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
