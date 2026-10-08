#ifndef CONTAINER_LDR_WALK_H_
#define CONTAINER_LDR_WALK_H_

#include "common/driver_protocol.h"

// Walks the target's PEB->Ldr->InLoadOrderModuleList through the mirror
// and prints every module (name, target base, translated container base,
// image size). Returns the module count; 0 when the walk cannot start.
unsigned LdrWalkModules(const SM_ATTACH_OUT& out);

#endif  // CONTAINER_LDR_WALK_H_
