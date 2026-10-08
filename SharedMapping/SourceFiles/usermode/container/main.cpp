#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstring>

#include "common/driver_protocol.h"
#include "common/pml4.h"
#include "ldr_walk.h"
#include "mirror_view.h"

static volatile const void* SmExpectedVa;

static LONG WINAPI SmVectoredLogger(struct _EXCEPTION_POINTERS* Info) {
    const ULONG_PTR* info = Info->ExceptionRecord->ExceptionInformation;
    fprintf(stderr,
            "exc: code=0x%08lX at=%p access=%s data_va=0x%p (expected %p)\n",
            (unsigned long)Info->ExceptionRecord->ExceptionCode,
            Info->ExceptionRecord->ExceptionAddress,
            info[0] == 0 ? "READ" : (info[0] == 1 ? "WRITE" : "OTHER"),
            (const void*)info[1], SmExpectedVa);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

static unsigned long long BenchRead(const volatile unsigned long long* Address,
                                    unsigned long long Iterations) {
    unsigned long long sum = 0;
    __try {
        for (unsigned long long i = 0; i < Iterations; ++i) {
            sum += Address[i & 0xF];  // stay within a couple of pages
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ~0ull;
    }
    return sum;
}

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
    wprintf(L"warmup 0x%llx+%llu: OK\n", va, bytes);
    return TRUE;
}

static void PrintBalance(HANDLE device) {
    SM_INFO_OUT info;
    DWORD returned = 0;
    memset(&info, 0, sizeof(info));
    if (DeviceIoControl(device, IOCTL_SM_GETINFO, nullptr, 0, &info,
                        sizeof(info), &returned, 0)) {
        wprintf(L"pfn balance after detach: %d (expected 0)\n",
                info.PfnBalance);
    }
}

// Translates a TARGET-space va into the container through the window
// plan; 0 when no window covers it (mirror_view).
static BOOL RunMirror(HANDLE device, SM_ATTACH_OUT* out, unsigned long long va,
                      BOOL doWrite, unsigned long long bench) {
    const unsigned long long translated = TranslateVa(*out, va);
    if (translated == 0) {
        wprintf(L"translate: no window for slot %u\n", SmPml4Index(va));
        return FALSE;
    }
    wprintf(L"mirror: target va 0x%llx -> container va 0x%llx\n", va,
            translated);
    SmExpectedVa = (const void*)translated;

    char buf[65];
    memset(buf, 0, sizeof(buf));
    if (SafeRead((const void*)translated, buf, 64)) {
        wprintf(L"read : \"%hs\"\n", buf);
    } else {
        wprintf(L"read : ACCESS VIOLATION (page not resident? try warmup)\n");
    }

    if (doWrite) {
        static const char marker[] = "SM-WRITE-FROM-CONTAINER";
        if (SafeWrite((void*)translated, marker, sizeof(marker))) {
            wprintf(L"write: OK (check the target with [p])\n");
        } else {
            wprintf(L"write: ACCESS VIOLATION (read-only page?)\n");
        }
    }

    if (bench > 0) {
        LARGE_INTEGER freq, t0, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        const unsigned long long sum =
            BenchRead((const volatile unsigned long long*)translated, bench);
        QueryPerformanceCounter(&t1);
        if (sum == ~0ull) {
            wprintf(L"bench: AV during reads\n");
        } else {
            wprintf(L"bench: %llu reads, %.2f ns/read\n", bench,
                    (double)(t1.QuadPart - t0.QuadPart) * 1e9 /
                        (double)freq.QuadPart / (double)bench);
        }
    }
    return TRUE;
}

// Reads a foreign (non-pinned) target va through the mirror. The page is
// NOT share-count pinned: a non-resident page faults fatally in the
// VAD-less container (finding 5), so probe only addresses that are
// almost certainly resident (e.g. image headers).
static void ProbeWindow(const SM_ATTACH_OUT* out, unsigned long long va) {
    const unsigned long long translated = TranslateVa(*out, va);
    if (translated == 0) {
        wprintf(L"probe: no window for slot %u\n", SmPml4Index(va));
        return;
    }
    wprintf(L"probe: target va 0x%llx -> container va 0x%llx\n", va,
            translated);
    SmExpectedVa = (const void*)translated;

    char buf[17];
    memset(buf, 0, sizeof(buf));
    if (SafeRead((const void*)translated, buf, 16)) {
        wprintf(L"probe: \"%hs\"\n", buf);
    } else {
        wprintf(L"probe: ACCESS VIOLATION (page not resident?)\n");
    }
}

// Baseline: the same 8-byte stride pattern as BenchRead, but through
// ReadProcessMemory on the ORIGINAL target va (no mirror involved) --
// one command then yields both H1 numbers under identical residency.
static void BenchRpm(ULONG pid, unsigned long long targetVa,
                     unsigned long long iterations) {
    const HANDLE process = OpenProcess(PROCESS_VM_READ, FALSE, pid);
    if (process == NULL) {
        wprintf(
            L"bench-rpm: OpenProcess(%lu) failed: %lu (elevation "
            L"mismatch with the target? run both from the same "
            L"console level)\n",
            pid, GetLastError());
        return;
    }
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    unsigned long long value = 0;
    SIZE_T got = 0;
    QueryPerformanceCounter(&t0);
    for (unsigned long long i = 0; i < iterations; ++i) {
        if (!ReadProcessMemory(
                process,
                (LPCVOID)(targetVa + (i & 0xF) * sizeof(unsigned long long)),
                &value, sizeof(value), &got)) {
            wprintf(L"bench-rpm: ReadProcessMemory failed at %llu: %lu\n", i,
                    GetLastError());
            CloseHandle(process);
            return;
        }
    }
    QueryPerformanceCounter(&t1);
    wprintf(L"bench-rpm: %llu reads, %.2f ns/read\n", iterations,
            (double)(t1.QuadPart - t0.QuadPart) * 1e9 / (double)freq.QuadPart /
                (double)iterations);
    CloseHandle(process);
}

// One launch's parameters, filled by wmain: keeps the function signatures
// small and the flag parsing local to one place.
struct RunOptions {
    ULONG pid = 0;
    unsigned long long va = 0;
    unsigned long long pinLength = 4096;
    unsigned long long warmupBytes = 0;  // >0: run the residency-audit IOCTL
    unsigned long long bench = 0;
    unsigned long long benchRpm = 0;  // >0: ReadProcessMemory baseline (H1)
    unsigned long long probe = 0;
    bool write = false;
    bool hold = false;
    unsigned long long holdMs = 0;  // >0: unattended --hold (soak script)
};

static BOOL AttachAndRun(const RunOptions& opt) {
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
    in.TargetPid = opt.pid;
    in.TargetVa = opt.va;
    in.TargetLength = (uint32_t)opt.pinLength;
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
        wprintf(L" (KVA shadow: user DTB is active for the target)\n");
    }
    if (out.TargetImageBase != 0) {
        wprintf(L" image : 0x%llx (target exe base, from the driver)\n",
                out.TargetImageBase);
    }
    for (unsigned i = 0; i < out.WindowCount && i < SM_USER_SLOT_COUNT; ++i) {
        const long long delta = ((long long)out.Windows[i].ContainerSlot -
                                 (long long)out.Windows[i].TargetSlot) *
                                (long long)SM_SLOT_SIZE;
        wprintf(
            L" window %2u: target slot %3u -> container slot %3u "
            L"(va %+lld)\n",
            i, out.Windows[i].TargetSlot, out.Windows[i].ContainerSlot, delta);
    }

    if (out.Flags & SM_FLAG_WRITE_ENABLED) {
        if (opt.va != 0) {
            if (opt.warmupBytes != 0) {
                Warmup(device, opt.va, opt.warmupBytes);
            }
            RunMirror(device, &out, opt.va, opt.write, opt.bench);
            if (opt.probe != 0) {
                ProbeWindow(&out, opt.probe);
            } else if (out.TargetImageBase != 0) {
                wprintf(L"probe: no --probe given, using the image base\n");
                ProbeWindow(&out, out.TargetImageBase);
            }
            if (out.TargetPeb != 0) {
                LdrWalkModules(out);
            }
            if (opt.benchRpm > 0) {
                BenchRpm(opt.pid, opt.va, opt.benchRpm);
            }
        } else {
            wprintf(L"write build active but no va given: plan only\n");
        }
    } else if (opt.va != 0 && opt.warmupBytes != 0) {
        Warmup(device, opt.va, opt.warmupBytes);
    }

    if (opt.hold) {
        wprintf(
            L"mirror live exit/kill the target now, then press Enter "
            L"to detach\n");
        getchar();
    } else if (opt.holdMs > 0) {
        wprintf(L"mirror live for %llu ms (kill the target now)\n", opt.holdMs);
        Sleep((DWORD)opt.holdMs);
    }

    DeviceIoControl(device, IOCTL_SM_DETACH, nullptr, 0, nullptr, 0, &returned,
                    nullptr);
    PrintBalance(device);
    CloseHandle(device);
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    AddVectoredExceptionHandler(1, SmVectoredLogger);
    wprintf(L"client pid=%lu\n", GetCurrentProcessId());
    RunOptions opt;

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--pid") == 0 && i + 1 < argc) {
            opt.pid = (ULONG)wcstoul(argv[++i], nullptr, 0);
        } else if (wcscmp(argv[i], L"--va") == 0 && i + 1 < argc) {
            opt.va = _wcstoui64(argv[++i], nullptr, 16);
        } else if (wcscmp(argv[i], L"--write") == 0) {
            opt.write = true;
        } else if (wcscmp(argv[i], L"--bench") == 0 && i + 1 < argc) {
            opt.bench = _wcstoui64(argv[++i], nullptr, 10);
        } else if (wcscmp(argv[i], L"--warmup") == 0 && i + 1 < argc) {
            opt.warmupBytes = _wcstoui64(argv[++i], nullptr, 10);
        } else if (wcscmp(argv[i], L"--len") == 0 && i + 1 < argc) {
            opt.pinLength = _wcstoui64(argv[++i], nullptr, 10);
        } else if (wcscmp(argv[i], L"--probe") == 0 && i + 1 < argc) {
            opt.probe = _wcstoui64(argv[++i], nullptr, 16);
        } else if (wcscmp(argv[i], L"--bench-rpm") == 0 && i + 1 < argc) {
            opt.benchRpm = _wcstoui64(argv[++i], nullptr, 10);
        } else if (wcscmp(argv[i], L"--hold") == 0) {
            opt.hold = true;
        } else if (wcscmp(argv[i], L"--hold-ms") == 0 && i + 1 < argc) {
            opt.holdMs = _wcstoui64(argv[++i], nullptr, 10);
        } else {
            wprintf(L"unknown flag: %s\n", argv[i]);
        }
    }
    if (opt.pid == 0) {
        wprintf(
            L"usage: container --pid <pid> [--va <hex-va>] [--write] "
            L"[--bench N] [--bench-rpm N] [--len bytes] [--warmup bytes] "
            L"[--probe <hex-va>] [--hold | --hold-ms N]\n");
        return 2;
    }
    return AttachAndRun(opt) ? 0 : 1;
}
