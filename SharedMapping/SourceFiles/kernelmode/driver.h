#ifndef DRIVER_H_
#define DRIVER_H_

#include <ntddk.h>

#include "common/kernel_offsets.h"

#define SM_LOG0(fmt) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[smmap] " fmt "\n")
#define SM_LOG(fmt, ...)                                                    \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, "[smmap] " fmt "\n", \
               __VA_ARGS__)
#define SM_LOGE(fmt, ...)                                                     \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[smmap]! " fmt "\n", \
               __VA_ARGS__)

// driver.c
NTSTATUS SmIoCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS SmIoDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);
VOID SmDriverUnload(PDRIVER_OBJECT DriverObject);

// mirror.c
VOID SmOnProcessNotify(HANDLE ParentId, HANDLE ProcessId, BOOLEAN Create);

#endif  // DRIVER_H_
