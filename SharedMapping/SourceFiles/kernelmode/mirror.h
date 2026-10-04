#ifndef MIRROR_H_
#define MIRROR_H_

#include "common/driver_protocol.h"
#include "driver.h"

NTSTATUS SmMirrorInit(const SM_KERNEL_OFFSETS* Offsets);

VOID SmMirrorShutdown(VOID);

NTSTATUS SmMirrorAttach(const SM_ATTACH_IN* In, SM_ATTACH_OUT* Out);

NTSTATUS SmMirrorDetach(VOID);

NTSTATUS SmMirrorGetInfo(SM_INFO_OUT* Out);

#endif  // MIRROR_H_
