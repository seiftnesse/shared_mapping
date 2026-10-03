#ifndef DRIVER_PROTOCOL_H_
#define DRIVER_PROTOCOL_H_

#include <stdint.h>

#include "pml4.h"

#ifndef _KERNEL_MODE
#include <windows.h>
#include <winioctl.h>
#endif

#if defined(__cplusplus)
#define SM_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define SM_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#ifndef CTL_CODE
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))
#endif

#define SM_DOS_DEVICE_NAME L"\\\\.\\SharedMapping"

#define IOCTL_SM_GETINFO \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_SM_ATTACH                                   \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, \
             FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SM_DETACH \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_SM_WARMUP \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_WRITE_ACCESS)

#define SM_FLAG_DRY_RUN 0x00000001u        // plan only, no page-table writes
#define SM_FLAG_WRITE_ENABLED 0x00000002u  // container PML4 entries rewritten

#pragma pack(push, 8)

typedef struct _SM_WINDOW_MAPPING {
    uint16_t TargetSlot;     // target PML4 index (0..255)
    uint16_t ContainerSlot;  // assigned container PML4 index
} SM_WINDOW_MAPPING;

typedef struct _SM_ATTACH_IN {
    uint32_t TargetPid;
    uint32_t Reserved;
} SM_ATTACH_IN;

typedef struct _SM_ATTACH_OUT {
    uint32_t WindowCount;
    uint32_t Flags;
    uint64_t TargetKernelDtb;
    uint64_t TargetUserDtb;  // non-zero => KVA shadow enabled for the target
    SM_WINDOW_MAPPING Windows[SM_USER_SLOT_COUNT];
} SM_ATTACH_OUT;

typedef struct _SM_WARMUP_IN {
    uint64_t Address;  // VA in the TARGET address space
    uint32_t Size;
    uint32_t Reserved;
} SM_WARMUP_IN;

typedef struct _SM_INFO_OUT {
    uint32_t Attached;
    uint32_t TargetPid;
    uint32_t WindowCount;
    uint32_t Reserved;
    uint64_t TargetKernelDtb;
    uint64_t TargetUserDtb;
} SM_INFO_OUT;

#pragma pack(pop)

SM_STATIC_ASSERT(offsetof(SM_ATTACH_OUT, Windows) == 24, "wire layout pin");

#endif  // DRIVER_PROTOCOL_H_
