#include <ntddk.h>

static VOID SharedMappingUnload(PDRIVER_OBJECT DriverObject) {
    UNREFERENCED_PARAMETER(DriverObject);
    DbgPrint("SharedMapping: goodbye from kernel mode\n");
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT DriverObject,
                           PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    DbgPrint("SharedMapping: hello from kernel mode\n");

    DriverObject->DriverUnload = SharedMappingUnload;
    return STATUS_SUCCESS;
}
