#include "driver.h"
#include "common/driver_protocol.h"
#include "mirror.h"

static const WCHAR SM_DEVICE_NAME[] = L"\\Device\\SharedMapping";
static const WCHAR SM_DOS_NAME[] = L"\\DosDevices\\SharedMapping";

static NTSTATUS ReadUbr(PULONG Ubr) {
    static const WCHAR SM_CV_KEY[] =
        L"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

    UNICODE_STRING keyName;
    UNICODE_STRING valueName;
    RtlInitUnicodeString(&keyName, SM_CV_KEY);
    RtlInitUnicodeString(&valueName, L"UBR");

    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &keyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL,
                               NULL);

    HANDLE key;
    NTSTATUS st = ZwOpenKey(&key, KEY_QUERY_VALUE, &attributes);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    ULONG length;
    st = ZwQueryValueKey(key, &valueName, KeyValuePartialInformation, buffer,
                         sizeof(buffer), &length);
    ZwClose(key);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    PKEY_VALUE_PARTIAL_INFORMATION info =
        (PKEY_VALUE_PARTIAL_INFORMATION)buffer;
    if (info->Type != REG_DWORD || info->DataLength != sizeof(ULONG)) {
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *Ubr = *(ULONG*)info->Data;
    return STATUS_SUCCESS;
}

static const SM_KERNEL_OFFSETS* ResolveOffsets(VOID) {
    RTL_OSVERSIONINFOW version = {0};
    version.dwOSVersionInfoSize = sizeof(version);

    ULONG ubr = 0;
    if (!NT_SUCCESS(RtlGetVersion(&version)) || !NT_SUCCESS(ReadUbr(&ubr))) {
        SM_LOG0("failed to read the build key");
        return NULL;
    }

    const SM_KERNEL_OFFSETS* offsets =
        SmFindKernelOffsets(version.dwBuildNumber, ubr);
    if (offsets == NULL) {
        SM_LOGE("unsupported build %u.%u -- add a kernel_offsets.csv row",
                version.dwBuildNumber, ubr);
    } else {
        SM_LOG("offsets for build %u.%u found", version.dwBuildNumber, ubr);
    }
    return offsets;
}

NTSTATUS SmIoCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS SmIoDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
    UNREFERENCED_PARAMETER(DeviceObject);

    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(Irp);
    PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS st;
    ULONG_PTR information = 0;

    // Everything below needs PASSIVE_LEVEL (fast mutex, process lookup).
    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        st = STATUS_INVALID_DEVICE_STATE;
        goto Finish;
    }

    switch (io->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_SM_GETINFO:
            if (io->Parameters.DeviceIoControl.OutputBufferLength <
                sizeof(SM_INFO_OUT)) {
                st = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            st = SmMirrorGetInfo((SM_INFO_OUT*)buffer);
            information = sizeof(SM_INFO_OUT);
            break;

        case IOCTL_SM_ATTACH:
            if (io->Parameters.DeviceIoControl.InputBufferLength <
                    sizeof(SM_ATTACH_IN) ||
                io->Parameters.DeviceIoControl.OutputBufferLength <
                    sizeof(SM_ATTACH_OUT)) {
                st = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            st = SmMirrorAttach((SM_ATTACH_IN*)buffer, (SM_ATTACH_OUT*)buffer);
            information = NT_SUCCESS(st) ? sizeof(SM_ATTACH_OUT) : 0;
            break;

        case IOCTL_SM_DETACH:
            st = SmMirrorDetach();
            break;

        case IOCTL_SM_WARMUP:
            if (io->Parameters.DeviceIoControl.InputBufferLength <
                sizeof(SM_WARMUP_IN)) {
                st = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            st = SmMirrorWarmup((SM_WARMUP_IN*)buffer);
            break;

        default:
            st = STATUS_INVALID_DEVICE_REQUEST;
            break;
    }

Finish:
    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = information;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

VOID SmDriverUnload(PDRIVER_OBJECT DriverObject) {
    PsSetCreateProcessNotifyRoutine(SmOnProcessNotify, TRUE);
    SmMirrorShutdown();

    UNICODE_STRING dosName;
    RtlInitUnicodeString(&dosName, SM_DOS_NAME);
    IoDeleteSymbolicLink(&dosName);
    IoDeleteDevice(DriverObject->DeviceObject);
    SM_LOG0("unloaded");
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject,
                     PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(RegistryPath);

    NTSTATUS st = SmMirrorInit(ResolveOffsets());
    if (!NT_SUCCESS(st)) {
        return st;
    }

    UNICODE_STRING deviceName;
    UNICODE_STRING dosName;
    RtlInitUnicodeString(&deviceName, SM_DEVICE_NAME);
    RtlInitUnicodeString(&dosName, SM_DOS_NAME);

    PDEVICE_OBJECT device;
    st = IoCreateDevice(DriverObject, 0, &deviceName, FILE_DEVICE_UNKNOWN,
                        FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = IoCreateSymbolicLink(&dosName, &deviceName);
    if (!NT_SUCCESS(st)) {
        IoDeleteDevice(device);
        return st;
    }

    DriverObject->MajorFunction[IRP_MJ_CREATE] = SmIoCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = SmIoCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP] = SmIoCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = SmIoDeviceControl;
    DriverObject->DriverUnload = SmDriverUnload;

    st = PsSetCreateProcessNotifyRoutine(SmOnProcessNotify, FALSE);
    if (!NT_SUCCESS(st)) {
        IoDeleteSymbolicLink(&dosName);
        IoDeleteDevice(device);
        return st;
    }

    device->Flags &= ~DO_DEVICE_INITIALIZING;
    SM_LOG0("loaded (dry-run)");
    return STATUS_SUCCESS;
}
