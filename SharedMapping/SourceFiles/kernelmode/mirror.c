#include <intrin.h>

#include "common/paging_entry.h"
#include "common/pml4.h"
#include "mirror.h"
#include "target_process.h"

typedef struct {
    FAST_MUTEX Lock;
    LONG Attached;
    HANDLE SelfPid;
    HANDLE TargetPid;
    PEPROCESS TargetProcess;  // referenced for the lifetime of the attach
    UINT64 TargetKernelDtb;
    UINT64 TargetUserDtb;
    UINT64 SelfKernelDtb;
    UINT64 SelfUserDtb;
    ULONG WindowCount;
    SM_WINDOW_MAPPING Windows[SM_USER_SLOT_COUNT];
} MIRROR_STATE;

static MIRROR_STATE g_Mirror;
static const SM_KERNEL_OFFSETS* g_Offsets;

static BOOLEAN PhysInRam(UINT64 Phys) {
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    if (ranges == NULL) {
        return FALSE;
    }
    for (ULONG i = 0; ranges[i].BaseAddress.QuadPart != 0 ||
                      ranges[i].NumberOfBytes.QuadPart != 0;
         ++i) {
        UINT64 base = (UINT64)ranges[i].BaseAddress.QuadPart;
        UINT64 end = base + (UINT64)ranges[i].NumberOfBytes.QuadPart;
        if (Phys >= base && Phys < end) {
            ExFreePool(ranges);
            return TRUE;
        }
    }
    ExFreePool(ranges);
    return FALSE;
}

// DTB -> kernel VA of the PML4 page via the direct map, with a physical
// range check: a bogus DTB (offset drift) must not turn into a wild read.
static UINT64* Pml4OfDtb(UINT64 Dtb) {
    UINT64 page = SmCr3ToPhys(Dtb);
    if (page == 0 || !PhysInRam(page)) {
        return NULL;
    }
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)page;
    return (UINT64*)MmGetVirtualForPhysical(pa);
}

NTSTATUS SmMirrorInit(const SM_KERNEL_OFFSETS* Offsets) {
    RtlZeroMemory(&g_Mirror, sizeof(g_Mirror));
    ExInitializeFastMutex(&g_Mirror.Lock);
    g_Offsets = Offsets;
    return STATUS_SUCCESS;
}

VOID SmMirrorShutdown(VOID) {
    if (g_Mirror.Attached) {
        SmMirrorDetach();
    }
}

static NTSTATUS BuildWindowPlan(UINT64* TargetPml4, UINT64* SelfPml4Kernel,
                                UINT64* SelfPml4User, SM_ATTACH_OUT* Out) {
    BOOLEAN used[SM_USER_SLOT_COUNT] = {FALSE};
    NTSTATUS st = STATUS_SUCCESS;
    ULONG w = 0;
    ULONG next = 0;

    __try {
        for (ULONG i = 0; i < SM_USER_SLOT_COUNT; ++i) {
            used[i] =
                SmEntryIsPresent(SelfPml4Kernel[i]) ||
                (SelfPml4User != NULL && SmEntryIsPresent(SelfPml4User[i]));
        }
        for (ULONG i = 0; i < SM_USER_SLOT_COUNT; ++i) {
            UINT64 entry = TargetPml4[i];
            if (!SmEntryIsPresent(entry)) {
                continue;
            }
            while (next < SM_USER_SLOT_COUNT && used[next]) {
                ++next;
            }
            if (next >= SM_USER_SLOT_COUNT) {
                st = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            used[next] = TRUE;
            Out->Windows[w].TargetSlot = (uint16_t)i;
            Out->Windows[w].ContainerSlot = (uint16_t)next;
            ++w;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = STATUS_ACCESS_VIOLATION;
    }
    Out->WindowCount = w;
    return st;
}

NTSTATUS SmMirrorAttach(const SM_ATTACH_IN* In, SM_ATTACH_OUT* Out) {
    if (In == NULL || Out == NULL || In->TargetPid == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireFastMutex(&g_Mirror.Lock);
    if (g_Mirror.Attached) {
        ExReleaseFastMutex(&g_Mirror.Lock);
        return STATUS_DEVICE_BUSY;
    }
    if (g_Offsets == NULL || (__readcr4() & SM_CR4_LA57)) {
        ExReleaseFastMutex(&g_Mirror.Lock);
        return STATUS_NOT_SUPPORTED;
    }

    const ULONG pid = In->TargetPid;
    PEPROCESS target = NULL;
    NTSTATUS st = SmLookupTargetProcess(pid, &target);
    if (!NT_SUCCESS(st)) {
        ExReleaseFastMutex(&g_Mirror.Lock);
        return st;
    }

    PEPROCESS self = PsGetCurrentProcess();
    if (PsGetProcessId(self) == (HANDLE)(ULONG_PTR)pid) {
        st = STATUS_INVALID_PARAMETER;
        goto Cleanup;
    }

    SmReadTargetDtbs(target, g_Offsets, &g_Mirror.TargetKernelDtb,
                     &g_Mirror.TargetUserDtb);
    SmReadTargetDtbs(self, g_Offsets, &g_Mirror.SelfKernelDtb,
                     &g_Mirror.SelfUserDtb);

    UINT64* targetPml4 = Pml4OfDtb(g_Mirror.TargetKernelDtb);
    UINT64* selfPml4Kernel = Pml4OfDtb(g_Mirror.SelfKernelDtb);
    UINT64* selfPml4User =
        (g_Mirror.SelfUserDtb != 0) ? Pml4OfDtb(g_Mirror.SelfUserDtb) : NULL;
    if (targetPml4 == NULL || selfPml4Kernel == NULL) {
        st = STATUS_UNSUCCESSFUL;
        goto Cleanup;
    }

    RtlZeroMemory(Out, sizeof(*Out));
    Out->TargetKernelDtb = g_Mirror.TargetKernelDtb;
    Out->TargetUserDtb = g_Mirror.TargetUserDtb;
    st = BuildWindowPlan(targetPml4, selfPml4Kernel, selfPml4User, Out);
    if (!NT_SUCCESS(st)) {
        goto Cleanup;
    }
    Out->Flags = SM_FLAG_DRY_RUN;

    g_Mirror.SelfPid = PsGetProcessId(self);
    g_Mirror.TargetPid = (HANDLE)(ULONG_PTR)pid;
    g_Mirror.TargetProcess = target;
    g_Mirror.WindowCount = Out->WindowCount;
    RtlCopyMemory(g_Mirror.Windows, Out->Windows, sizeof(g_Mirror.Windows));
    g_Mirror.Attached = 1;
    target = NULL;
    SM_LOG("dry-run plan: pid=%u windows=%u kdtb=0x%I64x udtb=0x%I64x", pid,
           Out->WindowCount, Out->TargetKernelDtb, Out->TargetUserDtb);

Cleanup:
    if (target != NULL) {
        ObDereferenceObject(target);
    }
    ExReleaseFastMutex(&g_Mirror.Lock);
    return st;
}

NTSTATUS SmMirrorDetach(VOID) {
    ExAcquireFastMutex(&g_Mirror.Lock);
    if (g_Mirror.Attached) {
        ObDereferenceObject(g_Mirror.TargetProcess);
        g_Mirror.TargetProcess = NULL;
        g_Mirror.Attached = 0;
        SM_LOG0("detach");
    }
    ExReleaseFastMutex(&g_Mirror.Lock);
    return STATUS_SUCCESS;
}

NTSTATUS SmMirrorGetInfo(SM_INFO_OUT* Out) {
    if (Out == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Out, sizeof(*Out));
    ExAcquireFastMutex(&g_Mirror.Lock);
    Out->Attached = (uint32_t)g_Mirror.Attached;
    Out->TargetPid = (uint32_t)(ULONG_PTR)g_Mirror.TargetPid;
    Out->WindowCount = g_Mirror.WindowCount;
    Out->TargetKernelDtb = g_Mirror.TargetKernelDtb;
    Out->TargetUserDtb = g_Mirror.TargetUserDtb;
    ExReleaseFastMutex(&g_Mirror.Lock);
    return STATUS_SUCCESS;
}

// Unlocked state read: a race with attach is acceptable at research level.
VOID SmOnProcessNotify(HANDLE ParentId, HANDLE ProcessId, BOOLEAN Create) {
    UNREFERENCED_PARAMETER(ParentId);
    if (Create) {
        return;
    }
    if (!g_Mirror.Attached) {
        return;
    }
    if (ProcessId == g_Mirror.TargetPid ||
        (g_Mirror.SelfPid != NULL && ProcessId == g_Mirror.SelfPid)) {
        SmMirrorDetach();
    }
}
