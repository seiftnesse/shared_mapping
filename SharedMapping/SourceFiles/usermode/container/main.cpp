#include <cstdio>
#include <windows.h>
#include <winioctl.h>
#include <cstring>

#include "common/driver_protocol.h"
#include "common/pml4.h"

static BOOL AttachAndPrint(ULONG pid) {
    SM_ATTACH_IN in;
    SM_ATTACH_OUT out;
    DWORD returned = 0;

    const HANDLE device =
        CreateFileW(SM_DOS_DEVICE_NAME, GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        wprintf(L"CreateFile(%s) failed: %lu (driver loaded? admin?)\n",
                SM_DOS_DEVICE_NAME, GetLastError());
        return FALSE;
    }

    memset(&in, 0, sizeof(in));
    memset(&out, 0, sizeof(out));
    in.TargetPid = pid;
    if (!DeviceIoControl(device, IOCTL_SM_ATTACH, &in, sizeof(in), &out,
                         sizeof(out), &returned, 0)) {
        wprintf(L"IOCTL_SM_ATTACH failed: %lu\n", GetLastError());
        CloseHandle(device);
        return FALSE;
    }

    wprintf(
        L"attach: windows=%u flags=0x%x kdtb=0x%llx (pml4 0x%llx) "
        L"udtb=0x%llx\n",
        out.WindowCount, out.Flags, out.TargetKernelDtb,
        SmCr3ToPhys(out.TargetKernelDtb), out.TargetUserDtb);
    if (out.TargetUserDtb != 0) {
        wprintf(L"  (KVA shadow: user DTB is active for the target)\n");
    }
    for (unsigned i = 0; i < out.WindowCount && i < SM_USER_SLOT_COUNT; ++i) {
        const long long delta = ((long long)out.Windows[i].ContainerSlot -
                                 (long long)out.Windows[i].TargetSlot) *
                                (long long)SM_SLOT_SIZE;
        wprintf(
            L"  window %2u: target slot %3u -> container slot %3u "
            L"(va %+lld)\n",
            i, out.Windows[i].TargetSlot, out.Windows[i].ContainerSlot, delta);
    }

    DeviceIoControl(device, IOCTL_SM_DETACH, nullptr, 0, nullptr, 0, &returned,
                    nullptr);
    CloseHandle(device);
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    ULONG pid = 0;

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
            pid = (ULONG)wcstoul(argv[++i], nullptr, 0);
        }
    }
    if (pid == 0) {
        wprintf(L"usage: container --pid <pid>\n");
        return 2;
    }
    return AttachAndPrint(pid) ? 0 : 1;
}
