#include <ntifs.h>

#include "common/pml4.h"
#include "driver.h"
#include "kpti.h"
#include "physmem.h"

UINT64 SmKptiResolveShadowPhys(PEPROCESS Process,
                               const SM_KERNEL_OFFSETS* Offsets) {
    UINT64 shadowVa = 0;
    __try {
        shadowVa = *(volatile UINT64*)((UINT8*)Process + Offsets->VmOffset +
                                       Offsets->MmSupportSharedOffset +
                                       Offsets->ShadowMappingOffset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (shadowVa < SM_KERNEL_VA_BASE) {
        return 0;
    }
    UINT64 shadowPhys = 0;
    __try {
        PHYSICAL_ADDRESS pa = MmGetPhysicalAddress((PVOID)shadowVa);
        shadowPhys = (UINT64)pa.QuadPart & ~0xFFFull;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (shadowPhys == 0 || !SmPhysInRam(shadowPhys)) {
        return 0;
    }
    SM_LOGD("kpti: shadowva=0x%I64x shadowphys=0x%I64x -> DUAL-WRITE", shadowVa,
            shadowPhys);
    return shadowPhys;
}

VOID SmKptiFlushAfterWrite(BOOLEAN DualWrite) {
    if (!DualWrite) {
        __writecr3(__readcr3());
        return;
    }
    static PVOID flushTb;
    if (flushTb == NULL) {
        UNICODE_STRING name;
        RtlInitUnicodeString(&name, L"KeFlushEntireTb");
        flushTb = MmGetSystemRoutineAddress(&name);
    }
    if (flushTb != NULL) {
        ((VOID(NTAPI*)(BOOLEAN, BOOLEAN))flushTb)(TRUE, TRUE);
    } else {
        SM_LOGE("kpti: KeFlushEntireTb unresolved, stale TLB risk");
    }
}
