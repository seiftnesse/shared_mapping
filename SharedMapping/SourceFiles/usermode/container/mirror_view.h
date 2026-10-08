#ifndef CONTAINER_MIRROR_VIEW_H_
#define CONTAINER_MIRROR_VIEW_H_

#include <cstddef>

#include "common/driver_protocol.h"

// Translates a TARGET-space va into the container through the window
// plan (P1 shift formula); 0 when no window covers it.
unsigned long long TranslateVa(const SM_ATTACH_OUT& out, unsigned long long va);

// SEH-guarded memory access: reading foreign pages through the mirror
// raises on non-resident data, and the walk must survive it. False on
// access violation.
bool SafeRead(const void* address, void* buffer, size_t size);
bool SafeWrite(void* address, const void* buffer, size_t size);

#endif  // CONTAINER_MIRROR_VIEW_H_
