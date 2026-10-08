#ifndef SELFTEST_H_
#define SELFTEST_H_

#include <ntifs.h>

#include "common/driver_protocol.h"

// Per-attach diagnostics, compile-gated by SM_ENABLE_SELFTEST: walks the
// target chain and the mirror chain level by level through the truth
// path, cross-checks every leaf against the pinned page PFN, and probes
// the mirrored VA from kernel mode (only with a complete chain).
typedef struct SM_SELFTEST_CTX {
    UINT64 TargetKernelDtb;
    UINT64 SelfKernelDtb;
    UINT64 CapturedEntry;  // plan-time snapshot of the mirrored slot
} SM_SELFTEST_CTX;

VOID SmSelfTestRun(UINT64 TargetVa, const SM_ATTACH_OUT* Out, UINT64 PinnedPhys,
                   const SM_SELFTEST_CTX* Ctx);

// Attach-failure diagnostic: the pin succeeded (the page is resident),
// yet the plan missed its window. Re-resolves the leaf through the truth
// path: resident means the plan walked a stale view, unresolved means
// the live tables do not map the VA at all (bad pid/VA).
VOID SmSelfTestPlanMiss(UINT64 TargetKernelDtb, ULONG TargetSlot,
                        UINT64 TargetVa);

#endif  // SELFTEST_H_
