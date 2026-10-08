#ifndef MIRROR_H_
#define MIRROR_H_

#include "common/driver_protocol.h"
#include "driver.h"

NTSTATUS SmMirrorInit(const SM_KERNEL_OFFSETS* Offsets);

VOID SmMirrorShutdown(VOID);

NTSTATUS SmMirrorAttach(const SM_ATTACH_IN* In, SM_ATTACH_OUT* Out);

NTSTATUS SmMirrorDetach(VOID);

// Residency audit for the target range: every page is resolved through
// the truth path. A foreign page cannot be faulted in without entering
// the owner's context (pitfall P3), so a non-resident page is reported
// and the target must touch its own buffer. Succeeds only when every
// page of the range is resident.
NTSTATUS SmMirrorWarmup(const SM_WARMUP_IN* In);

NTSTATUS SmMirrorGetInfo(SM_INFO_OUT* Out);

#endif  // MIRROR_H_
