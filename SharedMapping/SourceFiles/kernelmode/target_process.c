#include <ntifs.h>

#include "common/pml4.h"
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

// Guarded EPROCESS field read: best-effort metadata for the client, so a
// missing offset or a faulting page must not fail the attach.
static UINT64 SmReadProcessQword(PEPROCESS Process, uint32_t Offset) {
    if (Offset == 0) {
        return 0;
    }
    __try {
        return *(volatile UINT64*)((UINT8*)Process + Offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

UINT64 SmReadTargetImageBase(PEPROCESS Process,
                             const SM_KERNEL_OFFSETS* Offsets) {
    return SmReadProcessQword(Process, Offsets->SectionBaseAddressOffset);
}

UINT64 SmReadTargetPeb(PEPROCESS Process, const SM_KERNEL_OFFSETS* Offsets) {
    return SmReadProcessQword(Process, Offsets->PebOffset);
}
