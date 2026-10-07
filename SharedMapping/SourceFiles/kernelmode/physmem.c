#include <ntifs.h>

#include "common/pml4.h"
#include "driver.h"
#include "physmem.h"

// Lazily opened \Device\PhysicalMemory section for the fallback path.
static HANDLE g_PhysSection;

NTSTATUS SmPhysInit(VOID) {
    g_PhysSection = NULL;
    return STATUS_SUCCESS;
}

VOID SmPhysShutdown(VOID) {
    if (g_PhysSection != NULL) {
        ZwClose(g_PhysSection);
        g_PhysSection = NULL;
    }
}

BOOLEAN SmPhysInRam(UINT64 Phys) {
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

// SMAP toggles: clang-cl has no MSVC __stac/__clac intrinsics; the
// instructions are the documented SMAP toggles (SDM Vol. 2A: STAC/CLAC).
static __inline VOID SmStac(VOID) {
    __asm__ volatile("stac" ::: "memory");
}

static __inline VOID SmClac(VOID) {
    __asm__ volatile("clac" ::: "memory");
}

typedef struct {
    PVOID Va;
    PMDL Mdl;  // non-NULL when the MDL path succeeded
} SM_PAGE_MAP;

// MdlBuf must be sizeof(MDL)+sizeof(PFN_NUMBER) bytes, caller-owned.
static BOOLEAN SmPhysMapPage(UINT64 PagePhys, PUCHAR MdlBuf, SM_PAGE_MAP* Map) {
    Map->Va = NULL;
    Map->Mdl = (PMDL)MdlBuf;
    MmInitializeMdl(Map->Mdl, NULL, PAGE_SIZE);
    Map->Mdl->MdlFlags |= MDL_PAGES_LOCKED;
    MmGetMdlPfnArray(Map->Mdl)[0] =
        (PFN_NUMBER)(SmCr3ToPhys(PagePhys) >> PAGE_SHIFT);
    Map->Va = MmMapLockedPagesSpecifyCache(Map->Mdl, KernelMode, MmCached, NULL,
                                           FALSE, NormalPagePriority);
    if (Map->Va != NULL) {
        return TRUE;
    }
    Map->Mdl = NULL;
    if (g_PhysSection == NULL) {
        UNICODE_STRING name;
        RtlInitUnicodeString(&name, L"\\Device\\PhysicalMemory");
        OBJECT_ATTRIBUTES oa;
        InitializeObjectAttributes(
            &oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
        HANDLE section = NULL;
        if (!NT_SUCCESS(ZwOpenSection(
                &section, SECTION_MAP_READ | SECTION_MAP_WRITE, &oa))) {
            SM_LOGE("PhysicalMemory open failed");
            return FALSE;
        }
        g_PhysSection = section;
    }
    LARGE_INTEGER offset;
    offset.QuadPart = (LONGLONG)SmCr3ToPhys(PagePhys);
    SIZE_T viewSize = PAGE_SIZE;
    const NTSTATUS st = ZwMapViewOfSection(
        g_PhysSection, ZwCurrentProcess(), &Map->Va, 0, PAGE_SIZE, &offset,
        &viewSize, ViewUnmap, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(st)) {
        SM_LOGE("section map failed for phys 0x%I64x: 0x%lx",
                (UINT64)offset.QuadPart, (ULONG)st);
        Map->Va = NULL;
        return FALSE;
    }
    return TRUE;
}

static VOID SmPhysUnmapPage(SM_PAGE_MAP* Map) {
    if (Map->Va == NULL) {
        return;
    }
    if (Map->Mdl != NULL) {
        MmUnmapLockedPages(Map->Va, Map->Mdl);
    } else {
        ZwUnmapViewOfSection(ZwCurrentProcess(), Map->Va);
    }
    Map->Va = NULL;
}

BOOLEAN SmPhysReadEntry(UINT64 TablePhys, ULONG Index, UINT64* Value) {
    UCHAR mdlBuf[sizeof(MDL) + sizeof(PFN_NUMBER)] = {0};
    SM_PAGE_MAP map;
    if (!SmPhysMapPage(TablePhys, mdlBuf, &map)) {
        return FALSE;
    }
    const BOOLEAN acWasSet = (__readeflags() & SM_EFLAGS_AC) != 0;
    if (!acWasSet) {
        SmStac();
    }
    __try {
        *Value = ((volatile UINT64*)map.Va)[Index];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *Value = 0;
    }
    if (!acWasSet) {
        SmClac();
    }
    SmPhysUnmapPage(&map);
    return TRUE;
}

BOOLEAN SmPhysWriteEntry(UINT64 TablePhys, ULONG Index, UINT64 Value,
                         UINT64* Written) {
    UCHAR mdlBuf[sizeof(MDL) + sizeof(PFN_NUMBER)] = {0};
    SM_PAGE_MAP map;
    if (!SmPhysMapPage(TablePhys, mdlBuf, &map)) {
        return FALSE;
    }
    const BOOLEAN acWasSet = (__readeflags() & SM_EFLAGS_AC) != 0;
    if (!acWasSet) {
        SmStac();
    }
    __try {
        InterlockedExchange64(&((volatile LONG64*)map.Va)[Index],
                              (LONG64)Value);
        *Written = ((volatile UINT64*)map.Va)[Index];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *Written = 0;
    }
    if (!acWasSet) {
        SmClac();
    }
    SmPhysUnmapPage(&map);
    return TRUE;
}

BOOLEAN SmPhysReadEntries(UINT64 TablePhys, ULONG Start, ULONG Count,
                          UINT64* Out) {
    UCHAR mdlBuf[sizeof(MDL) + sizeof(PFN_NUMBER)] = {0};
    SM_PAGE_MAP map;
    if (!SmPhysMapPage(TablePhys, mdlBuf, &map)) {
        return FALSE;
    }
    const BOOLEAN acWasSet = (__readeflags() & SM_EFLAGS_AC) != 0;
    if (!acWasSet) {
        SmStac();
    }
    BOOLEAN ok = TRUE;
    __try {
        RtlCopyMemory(Out, (UINT8*)map.Va + (UINT64)Start * sizeof(UINT64),
                      Count * sizeof(UINT64));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = FALSE;
    }
    if (!acWasSet) {
        SmClac();
    }
    SmPhysUnmapPage(&map);
    return ok;
}

BOOLEAN SmPhysWritePair(UINT64 TablePhysA, ULONG IndexA, UINT64 TablePhysB,
                        ULONG IndexB, UINT64 ValueA, UINT64 ValueB,
                        UINT64* WrittenA, UINT64* WrittenB) {
    UCHAR mdlBufA[sizeof(MDL) + sizeof(PFN_NUMBER)] = {0};
    UCHAR mdlBufB[sizeof(MDL) + sizeof(PFN_NUMBER)] = {0};
    SM_PAGE_MAP mapA;
    SM_PAGE_MAP mapB;
    if (!SmPhysMapPage(TablePhysA, mdlBufA, &mapA)) {
        return FALSE;
    }
    if (!SmPhysMapPage(TablePhysB, mdlBufB, &mapB)) {
        SmPhysUnmapPage(&mapA);
        return FALSE;
    }
    const BOOLEAN acWasSet = (__readeflags() & SM_EFLAGS_AC) != 0;
    if (!acWasSet) {
        SmStac();
    }
    __try {
        InterlockedExchange64(&((volatile LONG64*)mapA.Va)[IndexA],
                              (LONG64)ValueA);
        InterlockedExchange64(&((volatile LONG64*)mapB.Va)[IndexB],
                              (LONG64)ValueB);
        *WrittenA = ((volatile UINT64*)mapA.Va)[IndexA];
        *WrittenB = ((volatile UINT64*)mapB.Va)[IndexB];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *WrittenA = 0;
        *WrittenB = 0;
    }
    if (!acWasSet) {
        SmClac();
    }
    SmPhysUnmapPage(&mapB);
    SmPhysUnmapPage(&mapA);
    return TRUE;
}
