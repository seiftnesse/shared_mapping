#ifndef PFN_H_
#define PFN_H_

#include "common/kernel_offsets.h"
#include "driver.h"

// Resolves the PFN database and validates the layout constants.
NTSTATUS SmPfnInit(const SM_KERNEL_OFFSETS* Offsets);

VOID SmPfnShutdown(VOID);

// Adjusts the packed share count of the page-table page at `PhysPage` by
// +1 / -1
NTSTATUS SmPfnBumpShareCount(UINT64 PhysPage);
NTSTATUS SmPfnDropShareCount(UINT64 PhysPage);

// Bumps minus drops; must return to zero when the mirror is down.
LONG SmPfnBalance(VOID);

#endif  // PFN_H_
