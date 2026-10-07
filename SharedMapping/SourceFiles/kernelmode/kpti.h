#ifndef KPTI_H_
#define KPTI_H_

#include <ntifs.h>

#include "common/kernel_offsets.h"

// Resolves the physical address of the process's shadow PML4 through the
// EPROCESS -> Vm -> Shared -> ShadowMapping chain (offsets from the
// per-build table; the field holds a self-map VA of the neighboring-frame
// PML4 copy that MiCheckProcessShadow audits and SwapContext loads as the
// user CR3). Returns 0 when KPTI is off or the chain does not resolve.
UINT64 SmKptiResolveShadowPhys(PEPROCESS Process,
                               const SM_KERNEL_OFFSETS* Offsets);

// Makes the freshly written slot visible: with KPTI the user CR3 carries
// its own PCID that a local CR3 reload cannot touch, so the dual-write
// path broadcasts KeFlushEntireTb(TRUE, TRUE); otherwise a local CR3
// reload suffices.
VOID SmKptiFlushAfterWrite(BOOLEAN DualWrite);

#endif  // KPTI_H_
