#include <intrin.h>
#include <ntifs.h>

#include "pfn.h"

//  - Geoff Chappell, "_MMPFN" (geoffchappell.com/studies/windows/km/ntoskrnl/inc/ntos/mi/mmpfn):
//    MmPfnDatabase is a POINTER variable; on Win10+ the
//    array address is ASLR-resolved at boot (DVRT) -- read it at runtime,
//    never hardcode.
//  - Rayanfam, "Inside Windows Page Frame Number, part 2" (rayanfam.com/topics/inside-windows-page-frame-number-part2):
//    entry i lives at poi(nt!MmPfnDatabase) + i * 0x30 (x64 _MMPFN size).
//  - The packed share count lives in bits 61:0 of the u2 field at +0x18
//    (delete flag = bit 62, per-PFN lock = bit 63); verified from the
//    official PDB of build 19041.6456 AND against the kernel's own
//    MiLockAndIncrementShareCount in IDA (adds 1 inside mask
//    0x3FFFFFFFFFFFFFFF, then clears bit 63).

static UINT8* g_PfnDatabase;  // address of the _MMPFN array
static const SM_KERNEL_OFFSETS* g_Offsets;
static volatile LONG g_Balance;

NTSYSAPI PVOID NTAPI RtlPcToFileHeader(_In_ PVOID PcValue,
                                       _Out_ PVOID* BaseOfImage);

NTSTATUS SmPfnInit(const SM_KERNEL_OFFSETS* Offsets) {
    UNICODE_STRING name;
    PVOID ntBase = NULL;

    g_Offsets = Offsets;
    g_PfnDatabase = NULL;
    g_Balance = 0;

    // One dynamic lookup stays: the address of a statically imported
    // function is the IAT thunk in OUR image (dllimport semantics), so
    // RtlPcToFileHeader would resolve our driver instead of nt. The
    // documented way to a genuine nt code address is
    // MmGetSystemRoutineAddress.
    RtlInitUnicodeString(&name, L"MmGetPhysicalMemoryRanges");
    PVOID ntFn = MmGetSystemRoutineAddress(&name);
    if (ntFn == NULL || RtlPcToFileHeader(ntFn, &ntBase) == NULL ||
        ntBase == NULL) {
        return STATUS_NOT_SUPPORTED;
    }

    // The variable at base+RVA holds the array address at runtime.
    UINT8* db = *(UINT8**)((UINT8*)ntBase + Offsets->MmPfnDatabasePointerRva);
    if ((ULONG_PTR)db < 0xFFFF800000000000ULL || ((ULONG_PTR)db & 7) != 0) {
        SM_LOGE("pfn database pointer failed sanity: %p (nt base %p)", db,
                ntBase);
        return STATUS_NOT_SUPPORTED;
    }

    g_PfnDatabase = db;
    SM_LOG("pfn database: %p (nt base %p, rva 0x%x)", db, ntBase,
           Offsets->MmPfnDatabasePointerRva);
    return STATUS_SUCCESS;
}

VOID SmPfnShutdown(VOID) {
    if (g_Balance != 0) {
        SM_LOGE("pfn share-count balance %ld at shutdown -- leaked bumps!",
                g_Balance);
    }
}

// Per-PFN lock protocol, mirroring the kernel's own MiLockPageInline /
// MiLockAndIncrementShareCount: bit 63 of the u2 field is a spin lock, and
// Mm's field updates under the lock are plain RMWs -- a lock-free atomic
// add from our side could be overwritten by them. Lock, add, unlock.
static VOID SmPfnLock(volatile LONG64* Field) {
    while (InterlockedBitTestAndSet64(Field, 63)) {
        YieldProcessor();
    }
}

static VOID SmPfnUnlock(volatile LONG64* Field) {
    InterlockedAnd64(Field, ~(1LL << 63));
}

// True when PhysPage falls into a physical RAM range. The share-count
// bump/drop computes the _MMPFN address as database + frame*0x30 without
// any other bounds knowledge: a wild entry phys (beyond the highest frame)
// would read/write past the database and bugcheck (0x3B, VM-verified
// 2026-10-05), so callers must pre-validate.
static BOOLEAN SmPhysIsRam(UINT64 Phys) {
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    if (ranges == NULL) {
        return FALSE;
    }
    BOOLEAN inRam = FALSE;
    for (ULONG i = 0; ranges[i].BaseAddress.QuadPart != 0 ||
                      ranges[i].NumberOfBytes.QuadPart != 0;
         ++i) {
        const UINT64 base = (UINT64)ranges[i].BaseAddress.QuadPart;
        const UINT64 end = base + (UINT64)ranges[i].NumberOfBytes.QuadPart;
        if (Phys >= base && Phys < end) {
            inRam = TRUE;
            break;
        }
    }
    ExFreePool(ranges);
    return inRam;
}

static NTSTATUS AdjustShareCount(UINT64 PhysPage, LONG Delta) {
    if (g_PfnDatabase == NULL || (PhysPage & 0xFFF) != 0 ||
        !SmPhysIsRam(PhysPage)) {
        return STATUS_NOT_SUPPORTED;
    }
    volatile LONG64* field =
        (volatile LONG64*)(g_PfnDatabase +
                           (PhysPage >> PAGE_SHIFT) *
                               g_Offsets->MmpfnElementSize +
                           g_Offsets->MmpfnShareCountOffset);
    // ShareCount is bits 61:0 of the packed field; bits 62 (delete) and
    // 63 (lock) must survive untouched -- an atomic +Delta on the qword
    // cannot carry into them for any realistic count.
    SmPfnLock(field);
    InterlockedAdd64(field, Delta);
    SmPfnUnlock(field);
    InterlockedAdd(&g_Balance, Delta);
    return STATUS_SUCCESS;
}

NTSTATUS SmPfnBumpShareCount(UINT64 PhysPage) {
    return AdjustShareCount(PhysPage, 1);
}

NTSTATUS SmPfnDropShareCount(UINT64 PhysPage) {
    return AdjustShareCount(PhysPage, -1);
}

LONG SmPfnBalance(VOID) {
    return g_Balance;
}
