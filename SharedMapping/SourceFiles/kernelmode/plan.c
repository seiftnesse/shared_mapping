#include <ntifs.h>

#include "common/paging_entry.h"
#include "driver.h"
#include "physmem.h"
#include "plan.h"

NTSTATUS SmBuildWindowPlan(UINT64 TargetDtb, UINT64 SelfKernelDtb,
                           UINT64 SelfUserDtb, SM_ATTACH_OUT* Out,
                           UINT64* Entries) {
    BOOLEAN used[SM_USER_SLOT_COUNT] = {FALSE};
    NTSTATUS st = STATUS_SUCCESS;
    ULONG w = 0;
    ULONG next = 0;

    for (ULONG c = 0; c < SM_USER_SLOT_COUNT; c += 128) {
        UINT64 selfK[128];
        if (!SmPhysReadEntries(SelfKernelDtb, c, 128, selfK)) {
            st = STATUS_UNSUCCESSFUL;
            break;
        }
        UINT64 selfU[128];
        const BOOLEAN haveU =
            SelfUserDtb != 0 && SmPhysReadEntries(SelfUserDtb, c, 128, selfU);
        for (ULONG k = 0; k < 128; ++k) {
            used[c + k] = SmEntryIsPresent(selfK[k]) ||
                          (haveU && SmEntryIsPresent(selfU[k]));
        }
    }
    for (ULONG c = 0; c < SM_USER_SLOT_COUNT && NT_SUCCESS(st); c += 128) {
        UINT64 target[128];
        if (!SmPhysReadEntries(TargetDtb, c, 128, target)) {
            st = STATUS_UNSUCCESSFUL;
            break;
        }
        for (ULONG k = 0; k < 128; ++k) {
            const UINT64 entry = target[k];
            if (!SmEntryIsPresent(entry)) {
                continue;
            }
            // A live PML4E references a page-table page in RAM; an entry
            // whose phys is outside every RAM range is not a live-table
            // value. Skip it loudly: bumping or copying it downstream
            // bugchecks (finding 14, 0x3B).
            if (!SmPhysInRam(SmEntryPhys(entry))) {
                SM_LOGE(
                    "plan: target slot %u entry 0x%I64x phys 0x%I64x is "
                    "not RAM, view is not the live table",
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
    for (ULONG k = 0; k < w; ++k) {
        SM_LOGD("plan[%u]: target slot %u", k, Out->Windows[k].TargetSlot);
    }
    return st;
}

BOOLEAN SmResolveLeafPfn(UINT64 Dtb, UINT64 Va, UINT64* Phys) {
    static const CHAR* names[4] = {"PXE", "PPE", "PDE", "PTE"};
    UINT64 table = Dtb;
    for (ULONG level = 0; level < 4; ++level) {
        const ULONG idx = (level == 0)   ? SmPml4Index(Va)
                          : (level == 1) ? SmPdptIndex(Va)
                          : (level == 2) ? SmPdIndex(Va)
                                         : SmPtIndex(Va);
        UINT64 entry = 0;
        if (!SmPhysReadEntry(table, idx, &entry)) {
            SM_LOGE("leaf walk: %s unreadable", names[level]);
            return FALSE;
        }
        if (!SmEntryIsPresent(entry)) {
            // Not resident (transition/queued out) or not mapped: the
            // caller decides, a non-resident page cannot be pinned
            // without faulting it in through the owner's VADs.
            SM_LOGE("leaf walk: %s not present (page not resident?)",
                    names[level]);
            return FALSE;
        }
        if (level < 3 && SmEntryIsLarge(entry)) {
            const UINT64 mask =
                (level == 1) ? (SM_REGION_PDPTE - 1) : (SM_REGION_PDE - 1);
            *Phys = SmEntryPhys(entry) + (Va & mask);
            return TRUE;
        }
        if (level == 3) {
            *Phys = SmEntryPhys(entry) + (Va & (SM_REGION_PTE - 1));
            return TRUE;
        }
        table = entry;
    }
    return FALSE;
}
