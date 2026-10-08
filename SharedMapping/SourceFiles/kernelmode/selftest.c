#include <ia32intrin.h>
#include <intrin.h>
#include <ntifs.h>

#include "common/paging_entry.h"
#include "driver.h"
#include "physmem.h"
#include "plan.h"
#include "selftest.h"

// SMAP toggles: clang-cl has no MSVC __stac/__clac intrinsics; the
// instructions are the documented SMAP toggles (SDM Vol. 2A: STAC/CLAC).
static __inline VOID SmStac(VOID) {
    __asm__ volatile("stac" ::: "memory");
}

static __inline VOID SmClac(VOID) {
    __asm__ volatile("clac" ::: "memory");
}

// Walks Va from Dtb through the truth path, logging every level.
static UINT64 SmWalkAndLog(const CHAR* Tag, UINT64 Dtb, UINT64 Va) {
    static const CHAR* names[4] = {"PXE", "PPE", "PDE", "PTE"};
    const UINT32 idx[4] = {SmPml4Index(Va), SmPdptIndex(Va), SmPdIndex(Va),
                           SmPtIndex(Va)};
    UINT64 table = Dtb;
    UINT64 leafPhys = 0;
    for (ULONG level = 0; level < 4; ++level) {
        UINT64 entry = 0;
        if (!SmPhysReadEntry(table, idx[level], &entry)) {
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

VOID SmSelfTestPlanMiss(UINT64 TargetKernelDtb, ULONG TargetSlot,
                        UINT64 TargetVa) {
    UINT64 leafPhys = 0;
    const BOOLEAN resident =
        SmResolveLeafPfn(TargetKernelDtb, TargetVa, &leafPhys);
    // The pin had already validated the page, so a miss with a resident
    // leaf means the plan walked a stale view; an unresolved leaf means
    // even the live tables do not map the VA (bad pid/VA).
    SM_LOGD("plan miss: slot %u va=0x%I64x leaf=0x%I64x (%s)", TargetSlot,
            TargetVa, resident ? leafPhys : 0,
            resident ? "resident" : "unmapped");
}

VOID SmSelfTestRun(UINT64 TargetVa, const SM_ATTACH_OUT* Out, UINT64 PinnedPhys,
                   const SM_SELFTEST_CTX* Ctx) {
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
    const UINT64 va =
        SmRemapVa(TargetVa, targetSlot, Out->Windows[w].ContainerSlot);

    // Logged before anything else: visible even when a walk aborts.
    if (PinnedPhys != 0) {
        SM_LOGD("selftest: pinned page phys 0x%I64x", PinnedPhys);
    }

    // Full level-by-level walk of the live target chain; SmWalkAndLog
    // logs every level, so its leaf needs no second walk.
    const UINT64 hwLeaf =
        SmWalkAndLog("target", Ctx->TargetKernelDtb, TargetVa);
    if (hwLeaf != 0 && PinnedPhys != 0) {
        SM_LOGD("selftest: target leaf 0x%I64x pinned 0x%I64x (%s)", hwLeaf,
                PinnedPhys,
                (hwLeaf >> 12) == (PinnedPhys >> 12) ? "match" : "MISMATCH");
    }
    UINT64 currentLeaf = 0;
    const BOOLEAN leafNow =
        SmResolveLeafPfn(Ctx->TargetKernelDtb, TargetVa, &currentLeaf);
    SM_LOGD("selftest: leaf now=0x%I64x pinned=0x%I64x (%s)",
            leafNow ? currentLeaf : 0, PinnedPhys,
            leafNow && (currentLeaf >> 12) == (PinnedPhys >> 12) ? "match"
                                                                 : "MISMATCH");

    // The plan input re-read vs the capture.
    {
        UINT64 liveNow = 0;
        if (SmPhysReadEntry(Ctx->TargetKernelDtb, targetSlot, &liveNow)) {
            SM_LOGD(
                "selftest: target PML4[%u] live=0x%I64x captured=0x%I64x "
                "(%s)",
                targetSlot, liveNow, Ctx->CapturedEntry,
                liveNow == Ctx->CapturedEntry ? "match" : "CHANGED");
        }
    }
    // Under KPTI the user CR3 walks the ShadowMapping table, resolved at
    // attach time; there is no need to enter the target context to see it.
    if (va == 0) {
        return;
    }

    // Live CR3 vs KPROCESS DTB and CR4.PCIDE decide what a CR3 reload
    // flushes (SDM Vol. 3A sec. 5.10.4.1); record them with the walk.
    SM_LOGD("selftest: cr3=0x%I64x kdtb=0x%I64x pcide=%u", __readcr3(),
            Ctx->SelfKernelDtb, (UINT32)((__readcr4() >> 17) & 1u));
    const UINT64 leafPhys = SmWalkAndLog("mirror", Ctx->SelfKernelDtb, va);
    if (PinnedPhys != 0 && leafPhys != 0) {
        SM_LOGD("selftest: mirror leaf 0x%I64x pinned 0x%I64x (%s)", leafPhys,
                PinnedPhys,
                (leafPhys >> 12) == (PinnedPhys >> 12) ? "match" : "MISMATCH");
    }

    if (leafPhys == 0) {
        // Dereferencing the mirror VA through an incomplete chain faults
        // in kernel mode inside a system service (bugcheck 3B). Only the
        // user-mode client may touch it in this state.
        SM_LOGE("selftest: kernel probe skipped (mirror chain incomplete)");
        return;
    }
    const BOOLEAN acWasSet = (__readeflags() & SM_EFLAGS_AC) != 0;
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
