#ifndef PLAN_H_
#define PLAN_H_

#include <ntifs.h>

#include "common/driver_protocol.h"

// Builds the window plan: enumerates the target's user PML4 through the
// truth path (SmPhysReadEntries), skips non-RAM entries loudly (a live
// PML4E always references a RAM page-table page), assigns each target
// slot a free container slot (free = absent from BOTH the container
// kernel PML4 and its shadow PML4 when KPTI is active; under KPTI pass
// the shadow-PML4 physical address as SelfUserDtb), and snapshots the
// captured entries for the share-count holds.
NTSTATUS SmBuildWindowPlan(UINT64 TargetDtb, UINT64 SelfKernelDtb,
                           UINT64 SelfUserDtb, SM_ATTACH_OUT* Out,
                           UINT64* Entries);

// Walks Va through Dtb (truth path) and returns the PHYSICAL address of
// the data page (4K/2M/1G leaves all handled). FALSE when any level is
// not present, i.e. the page is not resident or not mapped.
BOOLEAN SmResolveLeafPfn(UINT64 Dtb, UINT64 Va, UINT64* Phys);

#endif  // PLAN_H_
