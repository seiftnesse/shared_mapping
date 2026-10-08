#include "common/pml4.h"
#include "mirror_view.h"

// SEH and C++ unwinding do not mix: these functions must stay free of
// objects with destructors.

unsigned long long TranslateVa(const SM_ATTACH_OUT& out,
                               unsigned long long va) {
    for (unsigned i = 0; i < out.WindowCount && i < SM_USER_SLOT_COUNT; ++i) {
        if (out.Windows[i].TargetSlot == SmPml4Index(va)) {
            return SmSlotBase(out.Windows[i].ContainerSlot) |
                   (va & (SM_SLOT_SIZE - 1));
        }
    }
    return 0;
}

bool SafeRead(const void* address, void* buffer, size_t size) {
    __try {
        memcpy(buffer, address, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SafeWrite(void* address, const void* buffer, size_t size) {
    __try {
        memcpy(address, buffer, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
