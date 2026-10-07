#ifndef SELFTEST_H_
#define SELFTEST_H_

#include <ntifs.h>

#include "common/driver_protocol.h"

// Per-attach diagnostics, compile-gated by SM_ENABLE_SELFTEST: walks the
// target chain and the mirror chain through the truth path, cross-checks
// against the pinned page PFN and Mm's own translation, and probes the
// mirrored VA from kernel mode (only with a complete chain).
typedef struct SM_SELFTEST_CTX {
    UINT64 TargetKernelDtb;
    UINT64 SelfKernelDtb;
    UINT64 CapturedEntry;  // plan-time snapshot of the mirrored slot
} SM_SELFTEST_CTX;

VOID SmSelfTestRun(UINT64 TargetVa, const SM_ATTACH_OUT* Out, UINT64 PinnedPhys,
                   const SM_SELFTEST_CTX* Ctx);

// Attach-failure diagnostic: the pin succeeded, so the slot exists in the
// target's live tables, yet the plan missed it. Dumps the live entry, the
// target's live CR3 vs KPROCESS.DTB, and Mm's own translation.
VOID SmSelfTestPlanMiss(UINT64 TargetKernelDtb, ULONG TargetSlot,
                        UINT64 TargetVa);

#endif  // SELFTEST_H_
