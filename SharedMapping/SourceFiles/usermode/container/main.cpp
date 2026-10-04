#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstring>

#include "common/driver_protocol.h"
#include "common/pml4.h"

static BOOL Warmup(HANDLE device, unsigned long long va,
                   unsigned long long bytes) {
    SM_WARMUP_IN in;
    DWORD returned = 0;
    memset(&in, 0, sizeof(in));
    in.Address = va;
    in.Size = (uint32_t)bytes;
    if (!DeviceIoControl(device, IOCTL_SM_WARMUP, &in, sizeof(in), nullptr, 0,
                         &returned, 0)) {
        wprintf(L"warmup 0x%llx+%llu failed: %lu\n", va, bytes, GetLastError());
        return FALSE;
    }
    wprintf(L"warmup 0x%llx+%llu: OK (pages faulted in via the target VAD)\n",
            va, bytes);
    return TRUE;
}

static BOOL AttachAndPrint(ULONG pid, unsigned long long warmupVa,
                           unsigned long long warmupBytes) {
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

    if (warmupVa != 0 && warmupBytes != 0) {
        Warmup(device, warmupVa, warmupBytes);
    }

    DeviceIoControl(device, IOCTL_SM_DETACH, nullptr, 0, nullptr, 0, &returned,
                    nullptr);
    CloseHandle(device);
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    ULONG pid = 0;
    unsigned long long warmupVa = 0;
    unsigned long long warmupBytes = 0;

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
            pid = (ULONG)wcstoul(argv[++i], nullptr, 0);
        } else if (wcscmp(argv[i], L"--warmup") == 0 && i + 1 < argc) {
            // hex VA, decimal byte count: --warmup 0x1EAA5320000 4096
            warmupVa = _wcstoui64(argv[++i], nullptr, 16);
            if (i + 1 < argc) {
                warmupBytes = _wcstoui64(argv[++i], nullptr, 10);
            }
        }
    }
    if (pid == 0) {
        wprintf(L"usage: container --pid <pid> [--warmup <hex-va> <bytes>]\n");
        return 2;
    }
    return AttachAndPrint(pid, warmupVa, warmupBytes) ? 0 : 1;
}
