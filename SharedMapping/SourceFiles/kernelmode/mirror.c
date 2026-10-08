#include <ia32intrin.h>
#include <intrin.h>
#include <ntifs.h>

#include "common/paging_entry.h"
#include "common/pml4.h"
#include "kpti.h"
#include "mirror.h"
#include "pfn.h"
#include "physmem.h"
#include "plan.h"
#include "selftest.h"
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

#ifndef SM_ENABLE_SELFTEST
#define SM_ENABLE_SELFTEST 1
#endif

// NX bit set on both container copies of the mirrored entry (VM dump,
// finding 25): Mm keeps NX on user-slot entries of the ShadowMapping copy
// and the audit's masks preserve bit 63, so a kernel-side NX=0 vs shadow
// NX=1 mismatch is exactly bugcheck 1a/0x3600. Execution through the
// mirror is barred (data only), read/write unaffected.
#define SM_MIRROR_ENTRY_NX 0x8000000000000000ull

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
    // PML4E values captured at plan time; the share counts are held on the
    // page-table pages they reference (P2). The target layout can change
    // under the mirror (P1 note), so drops always use these snapshots.
    UINT64 TargetEntry[SM_USER_SLOT_COUNT];
    // Container shadow-PML4 physical address when KPTI is active (0 when
    // off). Outside the write gate: the plan's occupied-slot union
    // consults it in dry builds too.
    UINT64 SelfShadowPhys;
#if SM_ENABLE_WRITE
    // Live-mirror bookkeeping: the one window whose entry was written into
    // the container PML4s, the referenced self process (detach context),
    // the pinned page, and the watchdog stop event.
    BOOLEAN MirroredActive;
    ULONG MirroredWindow;
    PEPROCESS SelfProcess;
    UINT64* PinnedPfns;
    ULONG PinnedCount;
    KEVENT WatchdogStop;
#endif
} MIRROR_STATE;

static MIRROR_STATE g_Mirror;
static const SM_KERNEL_OFFSETS* g_Offsets;

NTSTATUS SmMirrorInit(const SM_KERNEL_OFFSETS* Offsets) {
    RtlZeroMemory(&g_Mirror, sizeof(g_Mirror));
    ExInitializeFastMutex(&g_Mirror.Lock);
    g_Offsets = Offsets;
    return SmPhysInit();
}

VOID SmMirrorShutdown(VOID) {
    if (g_Mirror.Attached) {
        SmMirrorDetach();
    }
    SmPhysShutdown();
}

#if SM_ENABLE_WRITE
// Watchdog: bounds the exposure of a live mirror. The thread is spawned
// detached (its handle is closed right away, nobody ever waits on it, so
// there is no leak and no deadlock); on wake it detaches unless the client
// already did.
//
// It waits on BOTH the stop event and the client process object: the
// process object is signaled at termination, while the destroy notify
// (SmOnProcessNotify) fires only in PspProcessDelete, AFTER
// MmCleanProcessAddressSpace has already walked and deleted the VADs
// (bugcheck 21 QUOTA_UNDERFLOW in MiRemoveVadCharges ran before our notify
// could clear the mirrored PML4E). Waiting on the referenced EPROCESS is
// safe: our own reference keeps the pointer valid.
static VOID WatchdogThread(PVOID Context) {
    UNREFERENCED_PARAMETER(Context);

    PVOID waits[2] = {&g_Mirror.WatchdogStop, g_Mirror.SelfProcess};
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)SM_WATCHDOG_SECONDS * 10 * 1000 * 1000;
    KWAIT_BLOCK waitBlock;
    const NTSTATUS st = KeWaitForMultipleObjects(
        2, waits, WaitAny, Executive, KernelMode, FALSE, &due, &waitBlock);
    if (st == STATUS_WAIT_1) {
        SM_LOG("watchdog: client died detaching");
    } else {
        SM_LOG("watchdog: detaching (exposure limit %us)", SM_WATCHDOG_SECONDS);
    }
    SmMirrorDetach();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// Writes the captured PML4E of the ONE window containing TargetVa into the
// container kernel PML4 and (under KPTI) its ShadowMapping copy, with NX
// set on both (see SM_MIRROR_ENTRY_NX). The entry is re-read through the
// truth path first: Mm rewrites the target PML4E during the attach, so the
// plan snapshot may be stale. Readbacks verify both writes.
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
    UINT64 live = 0;
    if (SmPhysReadEntry(g_Mirror.TargetKernelDtb, targetSlot, &live) &&
        SmEntryIsPresent(live)) {
        SM_LOGD("mirror: entry live=0x%I64x captured=0x%I64x (%s)", live, entry,
                live == entry ? "match" : "STALE-CAPTURE");
        if (live != entry) {
            entry = live;
            g_Mirror.TargetEntry[w] = live;
        }
    } else {
        SM_LOGD("mirror: live re-read unavailable, keeping captured");
    }

    if (!SmEntryUserAccessible(entry)) {
        // Refuse to project kernel-private regions into the user half.
        return STATUS_ACCESS_DENIED;
    }

    const ULONG slot = Out->Windows[w].ContainerSlot;
    const UINT64 mirrorEntry = entry | SM_MIRROR_ENTRY_NX;
    // Under KPTI the two writes must not be separable by an Mm operation:
    // the section-view unmap in between walks VADs and runs the shadow
    // audit, which bugchecks on any kernel-vs-shadow disagreement (the
    // SM_MIRROR_ENTRY pair write keeps both mappings up before the stores).
    UINT64 kernelWritten = 0;
    UINT64 shadowWritten = 0;
    if (g_Mirror.SelfShadowPhys != 0) {
        if (!SmPhysWritePair(g_Mirror.SelfKernelDtb, slot,
                             g_Mirror.SelfShadowPhys, slot, mirrorEntry,
                             mirrorEntry, &kernelWritten, &shadowWritten)) {
            SM_LOGE("mirror write: container PML4 not mappable");
            return STATUS_UNSUCCESSFUL;
        }
        if (kernelWritten != mirrorEntry || shadowWritten != mirrorEntry) {
            SM_LOGE(
                "mirror: PXE[%u] WRITE-LOST (kernel=0x%I64x "
                "shadow=0x%I64x want=0x%I64x)",
                slot, kernelWritten, shadowWritten, mirrorEntry);
            return STATUS_UNSUCCESSFUL;
        }
        SM_LOGD("mirror: PXE[%u] kernel+shadow verified live=0x%I64x", slot,
                kernelWritten);
    } else {
        kernelWritten = mirrorEntry;
        if (!SmPhysWriteEntry(g_Mirror.SelfKernelDtb, slot, mirrorEntry,
                              &kernelWritten)) {
            SM_LOGE("mirror write: container PML4 not mappable");
            return STATUS_UNSUCCESSFUL;
        }
        if (kernelWritten != mirrorEntry) {
            SM_LOGE("mirror: PXE[%u] WRITE-LOST (back=0x%I64x want=0x%I64x)",
                    slot, kernelWritten, mirrorEntry);
            return STATUS_UNSUCCESSFUL;
        }
        SM_LOGD("mirror: PXE[%u] kernel verified live=0x%I64x", slot,
                kernelWritten);
    }

    // Visibility: a stale not-present walk entry survives INVLPG (finding
    // 5f), and under KPTI the user CR3 carries its own PCID, so the dual
    // path needs a broadcast flush.
    SmKptiFlushAfterWrite(g_Mirror.SelfShadowPhys != 0);
    g_Mirror.MirroredActive = TRUE;
    g_Mirror.MirroredWindow = w;
    Out->Flags |= SM_FLAG_WRITE_ENABLED;
    SM_LOGD(
        "mirror ON: window %u (target slot %u -> container slot %u), "
        "watchdog %us",
        w, targetSlot, slot, SM_WATCHDOG_SECONDS);
    return STATUS_SUCCESS;
}
#endif  // SM_ENABLE_WRITE

NTSTATUS SmMirrorAttach(const SM_ATTACH_IN* In, SM_ATTACH_OUT* Out) {
    if (In == NULL || Out == NULL || In->TargetPid == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    // METHOD_BUFFERED: In and Out share one SystemBuffer, so later Out
    // writes clobber the input fields; capture them first. The pin length
    // is clamped to one page when zero (legacy single-page contract).
    const UINT64 targetVa = In->TargetVa;
    ULONG targetLength = In->TargetLength;
    if (targetLength == 0) {
        targetLength = PAGE_SIZE;
    } else if (targetLength > SM_MAX_PIN_LENGTH) {
        targetLength = SM_MAX_PIN_LENGTH;
    }

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
#if SM_ENABLE_WRITE
    // A zero chain offset means a truncated offset table: the shadow-PML4
    // reads below would land on the EPROCESS header at +0.
    if (targetVa != 0 &&
        (g_Offsets->VmOffset == 0 || g_Offsets->MmSupportSharedOffset == 0 ||
         g_Offsets->ShadowMappingOffset == 0)) {
        SM_LOGE(
            "attach refused: shadow chain offsets are zero "
            "(truncated offset table?)");
        st = STATUS_NOT_SUPPORTED;
        goto ReleaseAndExit;
    }
#endif

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

    SM_LOGD("attach: pid=%u TargetVa=0x%I64x", pid, targetVa);
    SmReadTargetDtbs(target, g_Offsets, &g_Mirror.TargetKernelDtb,
                     &g_Mirror.TargetUserDtb);
    SmReadTargetDtbs(self, g_Offsets, &g_Mirror.SelfKernelDtb,
                     &g_Mirror.SelfUserDtb);
    // Sanity: both PML4 pages must be real RAM (all page-table access goes
    // through the truth path, no direct-map aliases are taken).
    if (!SmPhysInRam(SmCr3ToPhys(g_Mirror.TargetKernelDtb)) ||
        !SmPhysInRam(SmCr3ToPhys(g_Mirror.SelfKernelDtb))) {
        st = STATUS_UNSUCCESSFUL;
        goto Cleanup;
    }
    g_Mirror.SelfShadowPhys =
        SmKptiResolveShadowPhys(self, g_Offsets);  // 0 when KPTI is off
    SM_LOGD("kpti: self udtb=0x%I64x -> %s", g_Mirror.SelfUserDtb,
            g_Mirror.SelfShadowPhys != 0 ? "DUAL-WRITE" : "kernel-only");

    // Container slots occupied by EITHER the kernel PML4 or the shadow
    // copy; under KPTI the plan union consults the shadow table itself.
    const UINT64 selfUserTable = g_Mirror.SelfShadowPhys != 0
                                     ? g_Mirror.SelfShadowPhys
                                     : g_Mirror.SelfUserDtb;

#if SM_ENABLE_WRITE
    UINT64* pinnedPfns = NULL;
    ULONG pinnedCount = 0;
    if (targetVa != 0) {
        // Everything below runs on physical memory. The pin resolves each
        // page of the range through the truth path and takes a share-count
        // hold on the data frame (the same primitive MmProbeAndLockPages
        // applies internally), so the trimmer cannot touch it while the
        // mirror is live (P3). No process attach is involved. A
        // not-present leaf means the page is not resident: fail cleanly
        // (the client can use --warmup, which faults pages through the
        // target's VADs, or the target can simply touch its buffer).
        const ULONG pages = targetLength / PAGE_SIZE;
        pinnedPfns =
            ExAllocatePoolWithTag(NonPagedPool, pages * sizeof(UINT64), 'SMpn');
        if (pinnedPfns == NULL) {
            st = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }

        // Plan first: the mirrored window must exist in the target PML4.
        const ULONG targetSlot = SmPml4Index(targetVa);
        RtlZeroMemory(Out, sizeof(*Out));
        Out->TargetKernelDtb = g_Mirror.TargetKernelDtb;
        Out->TargetUserDtb = g_Mirror.TargetUserDtb;
        st = SmBuildWindowPlan(g_Mirror.TargetKernelDtb, g_Mirror.SelfKernelDtb,
                               selfUserTable, Out, g_Mirror.TargetEntry);
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
        if (!found) {
            st = STATUS_INVALID_PARAMETER;
#if SM_ENABLE_SELFTEST
            SmSelfTestPlanMiss(g_Mirror.TargetKernelDtb, targetSlot, targetVa);
#endif
            goto Cleanup;
        }

        for (ULONG page = 0; page < pages; ++page) {
            const UINT64 pageVa = targetVa + (UINT64)page * PAGE_SIZE;
            UINT64 phys = 0;
            if (!SmResolveLeafPfn(g_Mirror.TargetKernelDtb, pageVa, &phys)) {
                SM_LOGE("pin: page %u of range 0x%I64x not resident", page,
                        targetVa);
                st = STATUS_INVALID_PARAMETER;
                goto Cleanup;
            }
            st = SmPfnBumpShareCount(phys);
            if (!NT_SUCCESS(st)) {
                SM_LOGE("pin: share-count hold failed for phys 0x%I64x", phys);
                goto Cleanup;
            }
            pinnedPfns[pinnedCount++] = phys;
        }
    } else
#endif
    {
        RtlZeroMemory(Out, sizeof(*Out));
        Out->TargetKernelDtb = g_Mirror.TargetKernelDtb;
        Out->TargetUserDtb = g_Mirror.TargetUserDtb;
        st = SmBuildWindowPlan(g_Mirror.TargetKernelDtb, g_Mirror.SelfKernelDtb,
                               selfUserTable, Out, g_Mirror.TargetEntry);
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
    // Initialized on every attach: detach may signal it for plan-only
    // attaches too (the watchdog thread itself only exists while a
    // mirror is live).
    KeInitializeEvent(&g_Mirror.WatchdogStop, NotificationEvent, FALSE);
    g_Mirror.MirroredActive = FALSE;
    if (targetVa == 0) {
        // Protocol: TargetVa == 0 means plan only. Mirroring an arbitrary
        // slot (index 0 of a null VA) would be a silent mistake.
        SM_LOG("write build: no TargetVa given plan only");
    } else {
        st = MirrorSingleWindow(targetVa, Out);
        if (!NT_SUCCESS(st)) {
            // The share-count bumps above are already held: roll them back
            // before bailing out (balance invariant, ground rule 4).
            for (ULONG i = 0; i < Out->WindowCount; ++i) {
                SmPfnDropShareCount(SmEntryPhys(g_Mirror.TargetEntry[i]));
            }
            goto Cleanup;
        }
#if SM_ENABLE_SELFTEST
        ULONG mw = 0;
        for (ULONG k = 0; k < Out->WindowCount; ++k) {
            if (Out->Windows[k].TargetSlot == SmPml4Index(targetVa)) {
                mw = k;
                break;
            }
        }
        const SM_SELFTEST_CTX ctx = {g_Mirror.TargetKernelDtb,
                                     g_Mirror.SelfKernelDtb,
                                     g_Mirror.TargetEntry[mw]};
        SmSelfTestRun(targetVa, Out, pinnedPfns[0], &ctx);
#endif
    }
#endif

    g_Mirror.SelfPid = PsGetProcessId(self);
    g_Mirror.TargetPid = (HANDLE)(ULONG_PTR)pid;
    g_Mirror.TargetProcess = target;
#if SM_ENABLE_WRITE
    g_Mirror.SelfProcess = self;
    ObReferenceObject(self);
    g_Mirror.PinnedPfns = pinnedPfns;
    g_Mirror.PinnedCount = pinnedCount;
    pinnedPfns = NULL;
#endif
    g_Mirror.WindowCount = Out->WindowCount;
    RtlCopyMemory(g_Mirror.Windows, Out->Windows, sizeof(g_Mirror.Windows));
    g_Mirror.Attached = 1;
    target = NULL;

#if SM_ENABLE_WRITE
    // The watchdog bounds LIVE-MIRROR exposure only. Plan-only attaches
    // hold just share counts: the client detach and the process-exit
    // notify already cover their cleanup, exactly like dry builds.
    if (g_Mirror.MirroredActive) {
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
    if (pinnedPfns != NULL) {
        // Attach failed with some holds already taken: release them.
        for (ULONG i = 0; i < pinnedCount; ++i) {
            SmPfnDropShareCount(pinnedPfns[i]);
        }
        ExFreePoolWithTag(pinnedPfns, 'SMpn');
    }
#endif
    ExReleaseFastMutex(&g_Mirror.Lock);
    return st;

ReleaseAndExit:
    ExReleaseFastMutex(&g_Mirror.Lock);
    return st;
}

NTSTATUS SmMirrorDetach(VOID) {
    ExAcquireFastMutex(&g_Mirror.Lock);
    if (g_Mirror.Attached) {
#if SM_ENABLE_WRITE
        if (g_Mirror.MirroredActive) {
            // Rollback: zero the mirrored slot in the container's REAL
            // PML4 page(s) through the truth path (physical, works from
            // any context: watchdog, process notify). Threads on other
            // CPUs may still hold stale translations and AV by design
            // (P5, research scope).
            const ULONG slot =
                g_Mirror.Windows[g_Mirror.MirroredWindow].ContainerSlot;
            UINT64 zero = 0;
            UINT64 shadowZero = 0;
            if (g_Mirror.SelfShadowPhys != 0) {
                // Pair-zeroed for the same reason the writes are paired:
                // the audit must never see one side cleared and the other
                // still holding the foreign entry.
                if (!SmPhysWritePair(g_Mirror.SelfKernelDtb, slot,
                                     g_Mirror.SelfShadowPhys, slot, 0, 0, &zero,
                                     &shadowZero)) {
                    SM_LOGE("detach zero: container PML4 not mappable");
                }
            } else if (!SmPhysWriteEntry(g_Mirror.SelfKernelDtb, slot, 0,
                                         &zero)) {
                SM_LOGE("detach zero: container PML4 not mappable");
            }
            SmKptiFlushAfterWrite(g_Mirror.SelfShadowPhys != 0);
            g_Mirror.MirroredActive = FALSE;
        }
        if (g_Mirror.PinnedPfns != NULL) {
            for (ULONG i = 0; i < g_Mirror.PinnedCount; ++i) {
                SmPfnDropShareCount(g_Mirror.PinnedPfns[i]);
            }
            ExFreePoolWithTag(g_Mirror.PinnedPfns, 'SMpn');
            g_Mirror.PinnedPfns = NULL;
            g_Mirror.PinnedCount = 0;
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
    const UINT64 targetDtb = g_Mirror.TargetKernelDtb;
    ExReleaseFastMutex(&g_Mirror.Lock);

    // Attach-free residency audit: every page of the range is resolved
    // through the truth path. A foreign page cannot be faulted in without
    // entering its context (fundamental), so a non-resident page is
    // reported and the target must touch its own buffer.
    ULONG resident = 0;
    const ULONG pages = In->Size / PAGE_SIZE;
    for (ULONG page = 0; page < pages; ++page) {
        UINT64 phys = 0;
        if (SmResolveLeafPfn(targetDtb, In->Address + (UINT64)page * PAGE_SIZE,
                             &phys)) {
            ++resident;
        }
    }
    SM_LOGD("warmup: %u/%u pages resident", resident, pages);
    return resident == pages ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
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
