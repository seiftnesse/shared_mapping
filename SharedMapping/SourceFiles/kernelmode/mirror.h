#ifndef MIRROR_H_
#define MIRROR_H_

#include "common/driver_protocol.h"
#include "driver.h"

NTSTATUS SmMirrorInit(const SM_KERNEL_OFFSETS* Offsets);

VOID SmMirrorShutdown(VOID);

NTSTATUS SmMirrorAttach(const SM_ATTACH_IN* In, SM_ATTACH_OUT* Out);

NTSTATUS SmMirrorDetach(VOID);

// Faults the target range in through the target's own VAD tree (pitfall
// P3) so the pages become reachable via the mirror. Attaches to the target
// process; SMAP-safe around the probes.
NTSTATUS SmMirrorWarmup(const SM_WARMUP_IN* In);

NTSTATUS SmMirrorGetInfo(SM_INFO_OUT* Out);

#endif  // MIRROR_H_
