#include <ia32intrin.h>
#include <intrin.h>
#include <ntifs.h>

#include "common/paging_entry.h"
#include "common/pml4.h"
#include "mirror.h"
#include "pfn.h"
#include "target_process.h"

#ifndef SM_ENABLE_WRITE
#define SM_ENABLE_WRITE 0
#endif
#ifndef SM_ENABLE_PFN_REFCOUNT
#define SM_ENABLE_PFN_REFCOUNT 0
#endif
#if SM_ENABLE_WRITE && !SM_ENABLE_PFN_REFCOUNT
#error \
    "SM_ENABLE_WRITE without SM_ENABLE_PFN_REFCOUNT: dangling entries on target exit (pitfall P2)"
#endif

#define SM_WATCHDOG_SECONDS 30

typedef struct {
    FAST_MUTEX Lock;
    LONG Attached;
    HANDLE SelfPid;
    HANDLE TargetPid;
    PEPROCESS TargetProcess;  // referenced for the lifetime of the attach
#if SM_ENABLE_WRITE
    PEPROCESS SelfProcess;  // referenced; holds the saved user DTB (P7)
#endif
    UINT64 TargetKernelDtb;
    UINT64 TargetUserDtb;
    UINT64 SelfKernelDtb;
    UINT64 SelfUserDtb;
    ULONG WindowCount;
    SM_WINDOW_MAPPING Windows[SM_USER_SLOT_COUNT];
    // PML4E values captured at plan time; the share counts are held on the
    // page-table pages they reference (P2). The target layout can change
    // under the mirror (P1 note), so drops always use these snapshots.
    UINT64 TargetEntry[SM_USER_SLOT_COUNT];
#if SM_ENABLE_WRITE
    // Live-mirror bookkeeping: the one window whose entry was written into
    // the container PML4, the container's saved KVA-shadow markers (P7),
    // and the watchdog stop event.
    BOOLEAN MirroredActive;
    ULONG MirroredWindow;
    BOOLEAN ShadowSaved;
    SM_SHADOW_SAVE ShadowSave;
    PMDL MirrorMdl;  // pins the mirrored page for the attach duration
    KEVENT WatchdogStop;
#endif
} MIRROR_STATE;

static MIRROR_STATE g_Mirror;
static const SM_KERNEL_OFFSETS* g_Offsets;

// Lazily opened \Device\PhysicalMemory section for the fallback physical
// page access path (see SmAccessPageEntry).
static HANDLE g_PhysSection;

// All diagnostic lines print at ERROR level: after a VM snapshot restore
// the DbgPrint filter dropped INFO-level [smmap] output, and the decisive
// selftest/plan-miss lines never reached the KD paste while the ERROR
// lines did (VM-verified 2026-10-06).
#define SM_LOGD SM_LOGE

// clang-cl has no MSVC __stac/__clac intrinsics; the instructions are the
// documented SMAP toggles (SDM Vol. 2A: STAC/CLAC).
static __inline VOID SmStac(VOID) {
    __asm__ volatile("stac" ::: "memory");
}

static __inline VOID SmClac(VOID) {
    __asm__ volatile("clac" ::: "memory");
}

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
    if (g_PhysSection != NULL) {
        ZwClose(g_PhysSection);
        g_PhysSection = NULL;
    }
}

// Faults in [Address, Address+Size) of `Target` by touching every page
// while attached to it: faults resolve against the target's VAD tree (P3),
// making the pages valid for both processes. CR4.SMAP blocks supervisor
// reads of user pages unless EFLAGS.AC is set (SDM Vol. 3A sec. 5.6), so
// AC is raised around the probes only when the kernel did not enter with
// it already set.
static NTSTATUS TouchTargetRange(PEPROCESS Target, UINT64 Address, ULONG Size) {
    KAPC_STATE apcState;
    KeStackAttachProcess(Target, &apcState);

    const BOOLEAN acWasSet = (__readeflags() & 0x40000) != 0;  // EFLAGS.AC
    if (!acWasSet) {
        SmStac();
    }

    NTSTATUS st = STATUS_SUCCESS;
    UINT64 addr = Address & ~0xFFFull;
    UINT64 end = Address + Size;
    __try {
        for (; addr < end; addr += PAGE_SIZE) {
            volatile UINT8 probe = *(volatile UINT8*)addr;
            (void)probe;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Not mapped even for the target (or a kernel-mode LASS stop).
        st = GetExceptionCode();
    }

    if (!acWasSet) {
        SmClac();
    }
    KeUnstackDetachProcess(&apcState);
    return st;
}

// The \\Device\\PhysicalMemory section view reads the LIVE page tables --
// its walk completed to the pinned leaf while MmCopyMemory point reads
// AND the direct-map alias returned STALE entries whose chains were dead.
// MmCopyMemory and MmGetVirtualForPhysical stay in the code ONLY as
// comparison views in diagnostics; every decision goes through here.
// Primary attempt: hand-built MDL + MmMapLockedPages (refused by Mm for
// PML4 frames on this build, 17/17 NULL). Fallback: transient RW view of
// \\Device\\PhysicalMemory -- the view is a USER VA in the current
// process, so accesses run under STAC (SMAP) inside __try; the physical
// access itself is context-independent.
typedef struct {
    PVOID Va;
    PMDL Mdl;  // non-NULL when the MDL path succeeded
} SM_PAGE_MAP;

// MdlBuf must be sizeof(MDL)+sizeof(PFN_NUMBER) bytes, caller-owned.
static BOOLEAN SmMapPage(UINT64 PagePhys, PUCHAR MdlBuf, SM_PAGE_MAP* Map) {
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
        UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\PhysicalMemory");
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

static VOID SmUnmapPage(SM_PAGE_MAP* Map) {
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

// Reads/writes one entry; on Write *Value is stored and read back. The
// return value holds the post-action entry. TRUE on success.
static BOOLEAN SmAccessPageEntry(UINT64 TablePhys, ULONG Index, BOOLEAN Write,
                                 UINT64* Value) {
    UCHAR mdlBuf[sizeof(MDL) + sizeof(PFN_NUMBER)] = {0};
    SM_PAGE_MAP map;
    if (!SmMapPage(TablePhys, mdlBuf, &map)) {
        return FALSE;
    }
    const BOOLEAN acWasSet = (__readeflags() & 0x40000) != 0;
    if (!acWasSet) {
        SmStac();
    }
    __try {
        if (Write) {
            InterlockedExchange64(&((volatile LONG64*)map.Va)[Index],
                                  (LONG64)*Value);
        }
        *Value = ((volatile UINT64*)map.Va)[Index];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *Value = 0;
    }
    if (!acWasSet) {
        SmClac();
    }
    SmUnmapPage(&map);
    return TRUE;
}

// Reads Count entries of a page-table page through the truth path.
static BOOLEAN SmReadTableEntries(UINT64 TablePhys, ULONG Start, ULONG Count,
                                  UINT64* Out) {
    UCHAR mdlBuf[sizeof(MDL) + sizeof(PFN_NUMBER)] = {0};
    SM_PAGE_MAP map;
    if (!SmMapPage(TablePhys, mdlBuf, &map)) {
        return FALSE;
    }
    const BOOLEAN acWasSet = (__readeflags() & 0x40000) != 0;
    if (!acWasSet) {
        SmStac();
    }
    __try {
        RtlCopyMemory(Out, (UINT8*)map.Va + (UINT64)Start * sizeof(UINT64),
                      Count * sizeof(UINT64));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Count = 0;
    }
    if (!acWasSet) {
        SmClac();
    }
    SmUnmapPage(&map);
    return Count != 0;
}

static NTSTATUS BuildWindowPlan(UINT64 TargetDtb, UINT64 SelfKernelDtb,
                                UINT64 SelfUserDtb, SM_ATTACH_OUT* Out,
                                UINT64* Entries) {
    BOOLEAN used[SM_USER_SLOT_COUNT] = {FALSE};
    NTSTATUS st = STATUS_SUCCESS;
    ULONG w = 0;
    ULONG next = 0;

    for (ULONG c = 0; c < SM_USER_SLOT_COUNT; c += 128) {
        UINT64 selfK[128];
        if (!SmReadTableEntries(SelfKernelDtb, c, 128, selfK)) {
            st = STATUS_UNSUCCESSFUL;
            break;
        }
        UINT64 selfU[128];
        const BOOLEAN haveU =
            SelfUserDtb != 0 && SmReadTableEntries(SelfUserDtb, c, 128, selfU);
        for (ULONG k = 0; k < 128; ++k) {
            used[c + k] = SmEntryIsPresent(selfK[k]) ||
                          (haveU && SmEntryIsPresent(selfU[k]));
        }
    }
    for (ULONG c = 0; c < SM_USER_SLOT_COUNT && NT_SUCCESS(st); c += 128) {
        UINT64 target[128];
        if (!SmReadTableEntries(TargetDtb, c, 128, target)) {
            st = STATUS_UNSUCCESSFUL;
            break;
        }
        for (ULONG k = 0; k < 128; ++k) {
            const UINT64 entry = target[k];
            if (!SmEntryIsPresent(entry)) {
                continue;
            }
            // A real PML4E references a page-table page in RAM; an entry
            // whose phys is outside every RAM range is not a live-table
            // value. Skip it loudly: bumping or copying it downstream
            // bugchecks (0x3B, VM-verified).
            if (!PhysInRam(SmEntryPhys(entry))) {
                SM_LOGE(
                    "plan: target slot %u entry 0x%I64x phys 0x%I64x is "
                    "not RAM -- view is not the live table",
                    c + k, entry, SmEntryPhys(entry));
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
            Out->Windows[w].TargetSlot = (uint16_t)(c + k);
            Out->Windows[w].ContainerSlot = (uint16_t)next;
            Entries[w] = entry;
            ++w;
        }
    }
    Out->WindowCount = w;
    // Log the plan slots: the "buffer slot missing from the plan" question
    // needs the actual slot list per attach in the trace.
    for (ULONG k = 0; k < w; ++k) {
        SM_LOGD("plan[%u]: target slot %u", k, Out->Windows[k].TargetSlot);
    }
    return st;
}

#if SM_ENABLE_WRITE
// Watchdog: bounds the exposure of a live mirror. The thread is spawned
// detached (its handle is closed right away -- nobody ever waits on it, so
// there is no leak and no deadlock); on wake it detaches unless the client
// already did.
//
// It waits on BOTH the stop event and the client process object: the
// process object is signaled at termination, while the destroy notify
// (SmOnProcessNotify) fires only in PspProcessDelete -- AFTER
// MmCleanProcessAddressSpace has already walked and deleted the VADs
// (VM-verified: bugcheck 21 QUOTA_UNDERFLOW in MiRemoveVadCharges ran
// before our notify could clear the mirrored PML4E). Waiting on the
// referenced EPROCESS is safe: our own reference keeps the pointer valid.
static VOID WatchdogThread(PVOID Context) {
    UNREFERENCED_PARAMETER(Context);

    PVOID waits[2] = {&g_Mirror.WatchdogStop, g_Mirror.SelfProcess};
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)SM_WATCHDOG_SECONDS * 10 * 1000 * 1000;
    KWAIT_BLOCK waitBlock;
    const NTSTATUS st = KeWaitForMultipleObjects(
        2, waits, WaitAny, Executive, KernelMode, FALSE, &due, &waitBlock);
    if (st == STATUS_WAIT_1) {
        SM_LOG("watchdog: client died -- detaching");
    } else {
        SM_LOG("watchdog: detaching (exposure limit %us)", SM_WATCHDOG_SECONDS);
    }
    SmMirrorDetach();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// Windows maps the live page tables of the CURRENT CR3 into kernel VA
// space via the PTE self-map; Mm itself edits page tables through it.
// Unlike the MmGetVirtualForPhysical alias -- VM-verified 2026-10-06 to
// return stale phantom content that flip-flops between reads (plan misses,
// dead PDPTs, a mirror write into a phantom page) -- a self-map access is
// translated by the CPU through the live CR3 and cannot lie. The self-map
// slot is BOOT-RANDOMIZED on this system: the classic 0xFFFFF68000000000
// guess bugchecked 0x50 (faults inside the PTE area are not catchable,
// VM-verified 2026-10-07), so the base is DERIVED at runtime by scanning
// the live PML4 for the self-referential entry. Abandoned after three
// 0x50s; kept here only as the historical note. The truth path for all
// page-table access is SmMapPage/SmAccessPageEntry above.

// Stage 5: writes the captured PML4E of the ONE window containing
// TargetVa into the container KERNEL PML4. The container was opted out of
// KVA shadow at attach (P7: MiCheckProcessShadow bugchecks on hand-written
// shadow entries), so a single copy is authoritative and visible from user
// mode. The slot was not-present, so no TLB invalidation is needed on the
// way in (P5); the first access walks the new tables.
static NTSTATUS MirrorSingleWindow(UINT64 TargetVa, SM_ATTACH_OUT* Out) {
    const ULONG targetSlot = SmPml4Index(TargetVa);
    ULONG w;
    for (w = 0; w < Out->WindowCount; ++w) {
        if (Out->Windows[w].TargetSlot == targetSlot) {
            break;
        }
    }
    if (w >= Out->WindowCount) {
        return STATUS_INVALID_PARAMETER;  // the VA's window is not mapped
    }

    UINT64 entry = g_Mirror.TargetEntry[w];
    // Re-read the entry through the TRUTH path (section view): the plan's
    // capture and every MmCopyMemory point read may hold STALE entries
    // (VM-verified 2026-10-07: the copy view named a dead PDPT while the
    // section view named the live chain reaching the pinned leaf).
    UINT64 live = 0;
    BOOLEAN haveLive =
        SmAccessPageEntry(g_Mirror.TargetKernelDtb, targetSlot, FALSE, &live);
    if (haveLive && SmEntryIsPresent(live)) {
        SM_LOGD("mirror: entry live=0x%I64x captured=0x%I64x (%s)", live, entry,
                live == entry ? "match" : "STALE-CAPTURE");
        if (live != entry) {
            entry = live;
            g_Mirror.TargetEntry[w] = live;
        }
    } else {
        SM_LOGD("mirror: live re-read unavailable -- keeping captured");
    }

    if (!SmEntryUserAccessible(entry)) {
        // Refuse to project kernel-private regions into the user half.
        return STATUS_ACCESS_DENIED;
    }

    const ULONG slot = Out->Windows[w].ContainerSlot;
    // Write through the MDL mapping of the container's real PML4 page and
    // take the read-back through the same mapping as the verdict: the
    // direct-map alias wrote into a phantom page (finding 13), so it is
    // not used at all any more.
    NTSTATUS st = STATUS_SUCCESS;
    UINT64 written = entry;
    if (!SmAccessPageEntry(g_Mirror.SelfKernelDtb, slot, TRUE, &written)) {
        SM_LOGE("mirror write: container PML4 not mappable");
        return STATUS_UNSUCCESSFUL;
    }
    if (written != entry) {
        SM_LOGE("mirror: PXE[%u] WRITE-LOST (back=0x%I64x want=0x%I64x)", slot,
                written, entry);
        return STATUS_UNSUCCESSFUL;
    }
    SM_LOGD("mirror: PXE[%u] verified live=0x%I64x", slot, written);
    // VM-verified (data_va == expected AV in the client): the TLB can hold
    // a stale not-present WALK entry for the new slot even though no data
    // was ever accessed through it (INVLPG cannot fix this: it flushes
    // leaf translations, not upper-level walk caches). A CR3 reload on the
    // container's CPU drops the whole local TLB, making the new PML4E
    // visible to every subsequent access. The client thread performs its
    // own next access on this CPU after the IOCTL returns.
    __writecr3(__readcr3());
    g_Mirror.MirroredActive = TRUE;
    g_Mirror.MirroredWindow = w;
    Out->Flags |= SM_FLAG_WRITE_ENABLED;
    SM_LOGD(
        "mirror ON: window %u (target slot %u -> container slot %u), "
        "watchdog %us",
        w, targetSlot, slot, SM_WATCHDOG_SECONDS);
    return STATUS_SUCCESS;
}

// VM 2026-10-05 run: the mirror write lands (PXE[5] present, our captured
// entry) but the PDPT it references lacks the target's PDE (PPE[389] = 0)
// while the target itself reads the page fine (pin before the plan, warmup
// st=0x0 after the mirror went on). Our view of the target's tables
// diverges from what Mm/CPU actually walk. This self test cross-checks
// both sides in situ: live target CR3 vs KPROCESS.DTB, captured PML4E vs
// the live one, a full software walk of BOTH the target VA (through the
// target tables) and the mirror VA (through the container tables), the
// pinned PFN, and a kernel probe read of the mirrored VA.

// Walks Va from Dtb through the truth path only. The direct-map and
// MmCopyMemory comparison calls that used to run here are GONE: their
// dereference crashed with an uncatchable 0x50 in the hyperspace/system
// region (VM-verified 2026-10-07, right after the PXE line), and the
// inversion they proved (finding 19) is settled -- no diagnostic value
// left, only risk.
static UINT64 SmWalkAndLog(const CHAR* Tag, UINT64 Dtb, UINT64 Va) {
    static const CHAR* names[4] = {"PXE", "PPE", "PDE", "PTE"};
    const UINT32 idx[4] = {
        (UINT32)((Va >> 39) & 0x1FFu), (UINT32)((Va >> 30) & 0x1FFu),
        (UINT32)((Va >> 21) & 0x1FFu), (UINT32)((Va >> 12) & 0x1FFu)};
    UINT64 table = Dtb;
    UINT64 leafPhys = 0;
    for (ULONG level = 0; level < 4; ++level) {
        UINT64 entry = 0;
        if (!SmAccessPageEntry(table, idx[level], FALSE, &entry)) {
            SM_LOGE("selftest: %s chain unreadable at %s", Tag, names[level]);
            return 0;
        }
        SM_LOGD("selftest: %s %s[%u] live=0x%I64x", Tag, names[level],
                idx[level], entry);
        if (!SmEntryIsPresent(entry)) {
            SM_LOGE("selftest: %s chain not present at %s", Tag, names[level]);
            return 0;
        }
        if (level < 3) {
            if (SmEntryIsLarge(entry)) {
                // 1-GiB leaf on a PDPTE, 2-MiB on a PDE: the walk ends here.
                const UINT64 mask =
                    (level == 1) ? (SM_REGION_PDPTE - 1) : (SM_REGION_PDE - 1);
                leafPhys = SmEntryPhys(entry) + (Va & mask);
                break;
            }
            table = entry;
        } else {
            leafPhys = SmEntryPhys(entry) + (Va & (SM_REGION_PTE - 1));
        }
    }
    return leafPhys;
}

// Plan-miss diagnostic: the pin had already validated the page, so the
// requested slot exists in the target's live tables, yet the plan did not
// see it. Dumps the dual view of the requested slot plus the target's live
// CR3 vs KPROCESS.DTB -- the discriminating data arrives even when attach
// fails and the selftest never runs.
static VOID SmLogPlanMiss(ULONG TargetSlot, UINT64 TargetVa, PEPROCESS Target) {
    UINT64 live = 0;
    const BOOLEAN ok =
        SmAccessPageEntry(g_Mirror.TargetKernelDtb, TargetSlot, FALSE, &live);
    // Mm's own translation of the requested VA, attached to the target:
    // nonzero means the LIVE tables map it (the pin was right and the plan
    // missed); zero means even Mm cannot translate (bad pid/VA).
    KAPC_STATE apcState;
    KeStackAttachProcess(Target, &apcState);
    const UINT64 liveCr3 = __readcr3();
    PHYSICAL_ADDRESS mmPa;
    mmPa.QuadPart = 0;
    __try {
        mmPa = MmGetPhysicalAddress((PVOID)TargetVa);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mmPa.QuadPart = 0;
    }
    KeUnstackDetachProcess(&apcState);
    SM_LOGD(
        "plan miss: slot %u va=0x%I64x live=0x%I64x (present=%u) "
        "livecr3=0x%I64x kdtb=0x%I64x (%s) mm-pa=0x%I64x (%s)",
        TargetSlot, TargetVa, ok ? live : 0,
        ok ? (UINT32)SmEntryIsPresent(live) : 0, liveCr3,
        g_Mirror.TargetKernelDtb,
        SmCr3ToPhys(liveCr3) == SmCr3ToPhys(g_Mirror.TargetKernelDtb)
            ? "match"
            : "MISMATCH",
        (UINT64)mmPa.QuadPart, mmPa.QuadPart != 0 ? "LIVE-MAPS" : "unmapped");
}

static VOID SmMirrorSelfTest(UINT64 TargetVa, PEPROCESS Target,
                             const SM_ATTACH_OUT* Out, PMDL Mdl) {
    const ULONG targetSlot = SmPml4Index(TargetVa);
    ULONG w;
    for (w = 0; w < Out->WindowCount; ++w) {
        if (Out->Windows[w].TargetSlot == targetSlot) {
            break;
        }
    }
    if (w >= Out->WindowCount) {
        return;
    }
    const UINT64 captured = g_Mirror.TargetEntry[w];
    const UINT64 va =
        SmRemapVa(TargetVa, targetSlot, Out->Windows[w].ContainerSlot);

    // Logged before anything else: visible even when a walk aborts.
    if (Mdl != NULL) {
        SM_LOGD("selftest: pinned page phys 0x%I64x",
                (UINT64)MmGetMdlPfnArray(Mdl)[0] << PAGE_SHIFT);
    }

    // A live CR3 that differs from KPROCESS.DTB means the plan and the copy
    // are taken from a table the CPU never walks. In the same attach, ask
    // Mm itself to translate TargetVa: MmGetPhysicalAddress resolves through
    // the self-map of the LIVE CR3 -- the tables the CPU actually walks,
    // whatever page KPROCESS.DTB names (lesson from Page-Table-Injector:
    // never self-walk a DTB page when the kernel can be asked).
    KAPC_STATE apcState;
    KeStackAttachProcess(Target, &apcState);
    const UINT64 targetCr3 = __readcr3();
    PHYSICAL_ADDRESS mmPa;
    mmPa.QuadPart = 0;
    __try {
        mmPa = MmGetPhysicalAddress((PVOID)TargetVa);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        mmPa.QuadPart = 0;
    }
    // MDL-mapped walk of the live chain: the PML4E and every LOWER level
    // read through fresh Mm-built mappings of the entry pages -- the
    // tie-breaking third view against direct/copy when they disagree
    // (PPE=0 vs Mm view match, run 2026-10-07).
    UINT64 hwLeaf = 0;
    {
        static const CHAR* hn[4] = {"PXE", "PPE", "PDE", "PTE"};
        UINT64 val = 0;
        if (!SmAccessPageEntry(g_Mirror.TargetKernelDtb, targetSlot, FALSE,
                               &val)) {
            SM_LOGD("selftest: target mdl PXE unreadable");
        }
        for (ULONG lvl = 0; lvl < 4; ++lvl) {
            if (lvl > 0) {
                UINT64 scratch = val;
                if (!SmAccessPageEntry(
                        SmCr3ToPhys(scratch),
                        (UINT32)((TargetVa >> (30 - 9 * (lvl - 1))) & 0x1FFu),
                        FALSE, &val)) {
                    SM_LOGD("selftest: target mdl %s unreadable", hn[lvl]);
                    break;
                }
            }
            SM_LOGD("selftest: target mdl %s = 0x%I64x", hn[lvl], val);
            if (!SmEntryIsPresent(val)) {
                SM_LOGD("selftest: target mdl chain not present at %s",
                        hn[lvl]);
                break;
            }
            if (lvl < 3 && SmEntryIsLarge(val)) {
                const UINT64 mask =
                    (lvl == 1) ? (SM_REGION_PDPTE - 1) : (SM_REGION_PDE - 1);
                hwLeaf = SmEntryPhys(val) + (TargetVa & mask);
                break;
            }
            if (lvl == 3) {
                hwLeaf = SmEntryPhys(val) + (TargetVa & (SM_REGION_PTE - 1));
            }
        }
    }
    if (hwLeaf != 0 && Mdl != NULL) {
        const UINT64 pinnedPhys = (UINT64)MmGetMdlPfnArray(Mdl)[0]
                                  << PAGE_SHIFT;
        SM_LOGD("selftest: mdl leaf 0x%I64x pinned 0x%I64x (%s)", hwLeaf,
                pinnedPhys,
                (hwLeaf >> 12) == (pinnedPhys >> 12) ? "match" : "MISMATCH");
    }
    KeUnstackDetachProcess(&apcState);
    SM_LOGD("selftest: target cr3=0x%I64x kdtb=0x%I64x (%s)", targetCr3,
            g_Mirror.TargetKernelDtb,
            SmCr3ToPhys(targetCr3) == SmCr3ToPhys(g_Mirror.TargetKernelDtb)
                ? "match"
                : "MISMATCH");
    if (Mdl != NULL) {
        const UINT64 pinnedPhys = (UINT64)MmGetMdlPfnArray(Mdl)[0]
                                  << PAGE_SHIFT;
        SM_LOGD("selftest: Mm view pa=0x%I64x pinned=0x%I64x (%s)",
                (UINT64)mmPa.QuadPart, pinnedPhys,
                ((UINT64)mmPa.QuadPart >> 12) == (pinnedPhys >> 12) &&
                        mmPa.QuadPart != 0
                    ? "match"
                    : "MISMATCH");
    } else {
        SM_LOGD("selftest: Mm view pa=0x%I64x", (UINT64)mmPa.QuadPart);
    }

    // The plan input re-read through the truth path vs the capture.
    {
        UINT64 liveNow = 0;
        if (SmAccessPageEntry(g_Mirror.TargetKernelDtb, targetSlot, FALSE,
                              &liveNow)) {
            SM_LOGD(
                "selftest: target PML4[%u] live=0x%I64x captured=0x%I64x "
                "(%s)",
                targetSlot, liveNow, captured,
                liveNow == captured ? "match" : "CHANGED");
        }
    }
    SmWalkAndLog("target", g_Mirror.TargetKernelDtb, TargetVa);
    if (SmCr3ToPhys(targetCr3) != SmCr3ToPhys(g_Mirror.TargetKernelDtb)) {
        // kdtb names a page the CPU does not walk: walk the live one too.
        SmWalkAndLog("live", targetCr3, TargetVa);
    }
    if (va == 0) {
        return;
    }

    // Live CR3 vs KPROCESS DTB and CR4.PCIDE decide what a CR3 reload
    // flushes (SDM Vol. 3A sec. 5.10.4.1); record them with the walk.
    SM_LOGD("selftest: cr3=0x%I64x kdtb=0x%I64x pcide=%u", __readcr3(),
            g_Mirror.SelfKernelDtb, (UINT32)((__readcr4() >> 17) & 1u));
    const UINT64 leafPhys = SmWalkAndLog("mirror", g_Mirror.SelfKernelDtb, va);
    if (Mdl != NULL && leafPhys != 0) {
        const UINT64 pinnedPhys = (UINT64)MmGetMdlPfnArray(Mdl)[0]
                                  << PAGE_SHIFT;
        SM_LOGD("selftest: mirror leaf 0x%I64x pinned 0x%I64x (%s)", leafPhys,
                pinnedPhys,
                (leafPhys >> 12) == (pinnedPhys >> 12) ? "match" : "MISMATCH");
    }

    if (leafPhys == 0) {
        // Dereferencing the mirror VA through an incomplete chain faults
        // in kernel mode inside a system service -- bugcheck 3B
        // (VM-verified 2026-10-05, three crashes right after the walk
        // aborts). Only the user-mode client may touch it in this state.
        SM_LOGE("selftest: kernel probe skipped (mirror chain incomplete)");
        return;
    }
    const BOOLEAN acWasSet = (__readeflags() & 0x40000) != 0;
    if (!acWasSet) {
        SmStac();
    }
    __try {
        const volatile UINT8 probe = *(volatile UINT8*)va;
        SM_LOGD("selftest: kernel probe read OK (0x%02x)", (UINT32)probe);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        SM_LOGE("selftest: kernel probe raised 0x%lx",
                (ULONG)GetExceptionCode());
    }
    if (!acWasSet) {
        SmClac();
    }
}
#endif  // SM_ENABLE_WRITE

NTSTATUS SmMirrorAttach(const SM_ATTACH_IN* In, SM_ATTACH_OUT* Out) {
    if (In == NULL || Out == NULL || In->TargetPid == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    // METHOD_BUFFERED: In and Out share one SystemBuffer. Out writes later
    // clobber the input at offset 8, so TargetVa must be captured first.
    const UINT64 targetVa = In->TargetVa;

#if SM_ENABLE_WRITE
    // A zero marker offset means a truncated offset table, not a real
    // field: the save/clear below would read and zero the EPROCESS header
    // at +0. Refuse before touching the process.
    if (targetVa != 0 &&
        (g_Offsets == NULL || g_Offsets->AddressPolicyOffset == 0 ||
         g_Offsets->ShadowDtbPointerOffset == 0)) {
        SM_LOGE(
            "attach refused: shadow marker offsets are zero "
            "(truncated offset table?)");
        return STATUS_NOT_SUPPORTED;
    }
    // P7 opt-out, BEFORE the fast mutex: the forced context switch below
    // needs PASSIVE_LEVEL. SwapContext (IDA, 19041) gates the shadow path
    // on DTB bit 1 + AddressPolicy bit 0; clearing the whole marker set
    // makes user mode run on the kernel CR3 (zeroing UserDirectoryTableBase
    // alone triple-faults: the shadow path then loads a null user CR3).
    // The per-CPU KPTI bookkeeping in KPCR is refreshed at context switch,
    // hence the forced delay below.
    PEPROCESS selfEarly = PsGetCurrentProcess();
    BOOLEAN shadowCleared = FALSE;
    if (targetVa != 0) {
        SM_SHADOW_SAVE save;
        SmSaveShadowState(selfEarly, g_Offsets, &save);
        // One trace line with all four markers: verifies on the VM that the
        // save hits the real fields (offsets fix) and that the clear below
        // is a no-op under KPTI-off (udtb 0, bit 1 clear, +0x400 zero).
        SM_LOGD(
            "opt-out save: dtb=0x%I64x udtb=0x%I64x ap=0x%02x shadow=0x%I64x",
            save.RawDtb, save.UserDtb, (UINT32)save.AddressPolicy,
            save.ShadowDtbPointer);
        SmClearShadowState(selfEarly, g_Offsets);
        // A >0 wait is required: a zero delay may return without a switch,
        // leaving the per-CPU KPTI state (KPCR) stale -- the stale state is
        // exactly what made MiCheckProcessShadow bugcheck (VM-verified).
        // 1 ms forces the thread through Idle and back, so SwapContext
        // re-reads the cleared markers on both transitions.
        LARGE_INTEGER oneMs;
        oneMs.QuadPart = -10000;  // 1 ms, relative
        KeDelayExecutionThread(KernelMode, FALSE, &oneMs);
        g_Mirror.ShadowSave = save;
        g_Mirror.ShadowSaved = TRUE;
        shadowCleared = TRUE;
    }
#endif

    NTSTATUS st;
    ExAcquireFastMutex(&g_Mirror.Lock);
    if (g_Mirror.Attached) {
        st = STATUS_DEVICE_BUSY;
        goto ReleaseAndExit;
    }
    if (g_Offsets == NULL || (__readcr4() & SM_CR4_LA57)) {
        st = STATUS_NOT_SUPPORTED;
        goto ReleaseAndExit;
    }

    const ULONG pid = In->TargetPid;
    PEPROCESS target = NULL;
    st = SmLookupTargetProcess(pid, &target);
    if (!NT_SUCCESS(st)) {
        goto ReleaseAndExit;
    }

    PEPROCESS self = PsGetCurrentProcess();
    if (PsGetProcessId(self) == (HANDLE)(ULONG_PTR)pid) {
        st = STATUS_INVALID_PARAMETER;
        goto Cleanup;
    }

    SM_LOG("attach: pid=%u TargetVa=0x%I64x", pid, targetVa);
    SmReadTargetDtbs(target, g_Offsets, &g_Mirror.TargetKernelDtb,
                     &g_Mirror.TargetUserDtb);
    // Full marker set of the TARGET: closes the "is KPTI really off for the
    // target" question (udtb=0 alone is the strong signal; ap bit 0 and the
    // shadow pointer confirm it).
    UINT8 targetAp = 0;
    UINT64 targetShadow = 0;
    SmReadShadowMarkers(target, g_Offsets, &targetAp, &targetShadow);
    SM_LOGD("target markers: udtb=0x%I64x ap=0x%02x shadow=0x%I64x",
            g_Mirror.TargetUserDtb, (UINT32)targetAp, targetShadow);
    // Self DTBs are read after the opt-out: with the markers cleared the
    // user DTB is 0, so only the kernel PML4 will be maintained.
    SmReadTargetDtbs(self, g_Offsets, &g_Mirror.SelfKernelDtb,
                     &g_Mirror.SelfUserDtb);
    // Sanity only: both PML4 pages must be real RAM (all page-table access
    // goes through SmMapPage; no direct-map aliases are taken here any
    // more, finding 13/19).
    if (!PhysInRam(SmCr3ToPhys(g_Mirror.TargetKernelDtb)) ||
        !PhysInRam(SmCr3ToPhys(g_Mirror.SelfKernelDtb))) {
        st = STATUS_UNSUCCESSFUL;
        goto Cleanup;
    }

#if SM_ENABLE_WRITE
    PMDL mirrorMdl = NULL;
    BOOLEAN mdlCommitted = FALSE;
    if (In->TargetVa != 0) {
        // Pin the mirrored page (P3, proper fix): MmProbeAndLockPages
        // attached to the target faults it in AND locks it against
        // trimming, so the client's read never faults and never enters
        // Mm's transition-resolution path (VM-verified: that path kills
        // the VAD-less process silently -- the read must not fault at
        // all). The probe also creates the page-table page, so the
        // requested slot exists at plan time; the retry loop stays for
        // the residual trim race.
        mirrorMdl =
            IoAllocateMdl((PVOID)targetVa, PAGE_SIZE, FALSE, FALSE, NULL);
        if (mirrorMdl == NULL) {
            st = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        KAPC_STATE apcState;
        KeStackAttachProcess(target, &apcState);
        __try {
            MmProbeAndLockPages(mirrorMdl, KernelMode, IoReadAccess);
            st = STATUS_SUCCESS;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            st = GetExceptionCode();
        }
        KeUnstackDetachProcess(&apcState);
        if (!NT_SUCCESS(st)) {
            SM_LOGE("target page pin failed: 0x%lx (bad --va?)", (ULONG)st);
            IoFreeMdl(mirrorMdl);
            st = STATUS_INVALID_PARAMETER;
            goto Cleanup;
        }

        // Expect a race anyway: under trim pressure the top-level table
        // is a moving target (VM-verified: the buffer slot vanished
        // between two attaches a second apart). Touch, replan, and retry
        // until the requested slot is present (bounded).
        const ULONG targetSlot = SmPml4Index(targetVa);
        for (ULONG attempt = 0;; ++attempt) {
            RtlZeroMemory(Out, sizeof(*Out));
            Out->TargetKernelDtb = g_Mirror.TargetKernelDtb;
            Out->TargetUserDtb = g_Mirror.TargetUserDtb;
            st = BuildWindowPlan(g_Mirror.TargetKernelDtb,
                                 g_Mirror.SelfKernelDtb, g_Mirror.SelfUserDtb,
                                 Out, g_Mirror.TargetEntry);
            if (!NT_SUCCESS(st)) {
                goto Cleanup;
            }
            BOOLEAN found = FALSE;
            for (ULONG k = 0; k < Out->WindowCount; ++k) {
                if (Out->Windows[k].TargetSlot == targetSlot) {
                    found = TRUE;
                    break;
                }
            }
            if (found || attempt >= 2) {
                if (!found) {
                    st = STATUS_INVALID_PARAMETER;
                    // The pin succeeded, so the slot exists in the target's
                    // live tables: dump the view-vs-truth pair for it.
                    SmLogPlanMiss(targetSlot, targetVa, target);
                    goto Cleanup;
                }
                break;
            }
            st = TouchTargetRange(target, targetVa, PAGE_SIZE);
            if (!NT_SUCCESS(st)) {
                SM_LOGE("target page touch failed: 0x%lx", (ULONG)st);
                goto Cleanup;
            }
        }
    } else
#endif
    {
        RtlZeroMemory(Out, sizeof(*Out));
        Out->TargetKernelDtb = g_Mirror.TargetKernelDtb;
        Out->TargetUserDtb = g_Mirror.TargetUserDtb;
        st = BuildWindowPlan(g_Mirror.TargetKernelDtb, g_Mirror.SelfKernelDtb,
                             g_Mirror.SelfUserDtb, Out, g_Mirror.TargetEntry);
        if (!NT_SUCCESS(st)) {
            goto Cleanup;
        }
    }
    Out->Flags = SM_FLAG_DRY_RUN;

    // P2: hold the page-table pages the captured entries reference. All
    // bumps succeed or the partial set is rolled back and attach fails.
    ULONG bumped = 0;
    st = STATUS_SUCCESS;
    for (; bumped < Out->WindowCount; ++bumped) {
        st = SmPfnBumpShareCount(SmEntryPhys(g_Mirror.TargetEntry[bumped]));
        if (!NT_SUCCESS(st)) {
            SM_LOGE("pfn bump failed for window %u: 0x%lx", bumped, (ULONG)st);
            break;
        }
    }
    if (!NT_SUCCESS(st)) {
        for (ULONG i = 0; i < bumped; ++i) {
            SmPfnDropShareCount(SmEntryPhys(g_Mirror.TargetEntry[i]));
        }
        goto Cleanup;
    }

#if SM_ENABLE_WRITE
    g_Mirror.MirroredActive = FALSE;
    if (targetVa == 0) {
        // Protocol: TargetVa == 0 means plan only. Mirroring an arbitrary
        // slot (index 0 of a null VA) would be a silent mistake.
        SM_LOG("write build: no TargetVa given -- plan only");
    } else {
        KeInitializeEvent(&g_Mirror.WatchdogStop, NotificationEvent, FALSE);
        st = MirrorSingleWindow(targetVa, Out);
        if (!NT_SUCCESS(st)) {
            // The share-count bumps above are already held: roll them back
            // before bailing out (balance invariant, ground rule 4).
            for (ULONG i = 0; i < Out->WindowCount; ++i) {
                SmPfnDropShareCount(SmEntryPhys(g_Mirror.TargetEntry[i]));
            }
            goto Cleanup;
        }
        SmMirrorSelfTest(targetVa, target, Out, mirrorMdl);
    }
#endif

    g_Mirror.SelfPid = PsGetProcessId(self);
    g_Mirror.TargetPid = (HANDLE)(ULONG_PTR)pid;
    g_Mirror.TargetProcess = target;
#if SM_ENABLE_WRITE
    g_Mirror.SelfProcess = self;
    ObReferenceObject(self);
    g_Mirror.MirrorMdl = mirrorMdl;
    mirrorMdl = NULL;
    mdlCommitted = TRUE;
#endif
    g_Mirror.WindowCount = Out->WindowCount;
    RtlCopyMemory(g_Mirror.Windows, Out->Windows, sizeof(g_Mirror.Windows));
    g_Mirror.Attached = 1;
    target = NULL;

#if SM_ENABLE_WRITE
    HANDLE thread = NULL;
    OBJECT_ATTRIBUTES threadAttr;
    InitializeObjectAttributes(&threadAttr, NULL, OBJ_KERNEL_HANDLE, NULL,
                               NULL);
    st = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, &threadAttr, NULL,
                              NULL, WatchdogThread, NULL);
    if (NT_SUCCESS(st)) {
        ZwClose(thread);  // detached watchdog: no handle retention
        st = STATUS_SUCCESS;
    } else {
        SM_LOGE("watchdog thread failed: 0x%lx", (ULONG)st);
        st = STATUS_SUCCESS;  // mirror stays; client detach still works
    }
#endif

    SM_LOGD("plan: pid=%u windows=%u flags=0x%x kdtb=0x%I64x udtb=0x%I64x", pid,
            Out->WindowCount, Out->Flags, Out->TargetKernelDtb,
            Out->TargetUserDtb);

Cleanup:
    if (target != NULL) {
        ObDereferenceObject(target);
    }
#if SM_ENABLE_WRITE
    if (mirrorMdl != NULL) {
        // Attach failed with the page still pinned: unlock attached to the
        // owner process, then free the MDL.
        KAPC_STATE apcState;
        KeStackAttachProcess(target, &apcState);
        MmUnlockPages(mirrorMdl);
        KeUnstackDetachProcess(&apcState);
        IoFreeMdl(mirrorMdl);
    }
    if (shadowCleared) {
        // Attach failed after the opt-out: put the shadow markers back.
        SmRestoreShadowState(selfEarly, g_Offsets, &g_Mirror.ShadowSave);
        g_Mirror.ShadowSaved = FALSE;
    }
#endif
    ExReleaseFastMutex(&g_Mirror.Lock);
    return st;

ReleaseAndExit:
#if SM_ENABLE_WRITE
    if (shadowCleared) {
        SmRestoreShadowState(selfEarly, g_Offsets, &g_Mirror.ShadowSave);
        g_Mirror.ShadowSaved = FALSE;
    }
#endif
    ExReleaseFastMutex(&g_Mirror.Lock);
    return st;
}

NTSTATUS SmMirrorDetach(VOID) {
    ExAcquireFastMutex(&g_Mirror.Lock);
    if (g_Mirror.Attached) {
#if SM_ENABLE_WRITE
        if (g_Mirror.MirroredActive) {
            // Rollback: zero the mirrored slot in the container's REAL
            // PML4 page through the MDL mapping (the direct-map alias can
            // be a phantom, finding 13). Physical, so this works from any
            // context (watchdog, process notify). Threads on other CPUs
            // may still hold stale translations and AV by design (P5,
            // research scope).
            const ULONG slot =
                g_Mirror.Windows[g_Mirror.MirroredWindow].ContainerSlot;
            UINT64 zero = 0;
            if (!SmAccessPageEntry(g_Mirror.SelfKernelDtb, slot, TRUE, &zero)) {
                SM_LOGE("detach zero: container PML4 not mappable");
            }
            __writecr3(__readcr3());
            g_Mirror.MirroredActive = FALSE;
        }
        // P7: give the container its KVA shadow back (all three markers).
        if (g_Mirror.ShadowSaved && g_Mirror.SelfProcess != NULL) {
            SmRestoreShadowState(g_Mirror.SelfProcess, g_Offsets,
                                 &g_Mirror.ShadowSave);
            g_Mirror.ShadowSaved = FALSE;
        }
        if (g_Mirror.MirrorMdl != NULL) {
            KAPC_STATE apcState;
            KeStackAttachProcess(g_Mirror.TargetProcess, &apcState);
            MmUnlockPages(g_Mirror.MirrorMdl);
            KeUnstackDetachProcess(&apcState);
            IoFreeMdl(g_Mirror.MirrorMdl);
            g_Mirror.MirrorMdl = NULL;
        }
        KeSetEvent(&g_Mirror.WatchdogStop, IO_NO_INCREMENT, FALSE);
#endif
        for (ULONG i = 0; i < g_Mirror.WindowCount; ++i) {
            SmPfnDropShareCount(SmEntryPhys(g_Mirror.TargetEntry[i]));
        }
        ObDereferenceObject(g_Mirror.TargetProcess);
        g_Mirror.TargetProcess = NULL;
#if SM_ENABLE_WRITE
        ObDereferenceObject(g_Mirror.SelfProcess);
        g_Mirror.SelfProcess = NULL;
#endif
        g_Mirror.Attached = 0;
        SM_LOG("detach (pfn balance %ld)", SmPfnBalance());
    }
    ExReleaseFastMutex(&g_Mirror.Lock);
    return STATUS_SUCCESS;
}

// Faults in [Address, Address+Size) of the target by touching every page
// while attached to it: page faults resolve against the target's VAD tree
// (P3), after which the pages are valid for both processes (shared PTEs).
// CR4.SMAP blocks supervisor reads of user pages unless EFLAGS.AC is set
// (SDM Vol. 3A sec. 5.6), so AC is raised around the probes only when the
// kernel did not enter with it already set.
NTSTATUS SmMirrorWarmup(const SM_WARMUP_IN* In) {
    if (In == NULL || In->Size == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    // User half only (128 TiB); reject kernel-space addresses outright.
    if (In->Address >= SM_SLOT_SIZE * SM_USER_SLOT_COUNT) {
        return STATUS_INVALID_PARAMETER;
    }

    ExAcquireFastMutex(&g_Mirror.Lock);
    if (!g_Mirror.Attached || g_Mirror.TargetProcess == NULL) {
        ExReleaseFastMutex(&g_Mirror.Lock);
        return STATUS_INVALID_DEVICE_STATE;
    }
    PEPROCESS target = g_Mirror.TargetProcess;
    ObReferenceObject(target);
    ExReleaseFastMutex(&g_Mirror.Lock);

    const NTSTATUS st = TouchTargetRange(target, In->Address, In->Size);
    ObDereferenceObject(target);
    SM_LOGD("warmup: st=0x%lx", (ULONG)st);
    return st;
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
    Out->PfnBalance = SmPfnBalance();
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
