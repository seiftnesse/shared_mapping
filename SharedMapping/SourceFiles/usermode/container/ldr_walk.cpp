#include <cstddef>
#include <cstdio>
#include <type_traits>

#include "common/pml4.h"
#include "ldr_walk.h"
#include "mirror_view.h"

namespace {

// Minimal mirrors of the loader structures the walk touches (x64). The
// layouts are verified against the real PDBs (_PEB.Ldr +0x18 from
// ntdll.pdb, the rest from the ntoskrnl PDB) and pinned by the
// static_asserts below. Independent cross-reference agreeing on every
// field: the ReactOS loader definitions (x64 prefix layout identical to
// the real one) - a witness only, not a copy source: their headers are
// GPL, and their full _PEB is WS2003-era, shorter than the real Win10
// one (reading sizeof(full struct) through the mirror could hit a
// non-resident tail). Every pointer field below holds a TARGET-space
// address; nothing here is dereferenced directly.

// https://doxygen.reactos.org/d0/d53/struct__PEB.html
struct ListEntry {
    unsigned long long Flink;
    unsigned long long Blink;
};

struct UnicodeStringView {  // _UNICODE_STRING
    uint16_t Length;        // in bytes
    uint16_t MaximumLength;
    uint32_t pad0;
    unsigned long long Buffer;  // target-space
};

struct PebView {  // _PEB prefix
    uint8_t InheritedAddressSpace;
    uint8_t ReadImageFileExecOptions;
    uint8_t BeingDebugged;
    uint8_t pad0[5];
    unsigned long long Mutant;
    unsigned long long ImageBaseAddress;  // cross-check vs the driver
    unsigned long long Ldr;               // -> PEB_LDR_DATA, target-space
};

struct PebLdrDataView {  // _PEB_LDR_DATA prefix
    uint32_t Length;
    uint8_t Initialized;
    uint8_t pad0[3];
    unsigned long long SsHandle;
    ListEntry InLoadOrderModuleList;  // the list head
};

struct LdrEntryView {                      // _LDR_DATA_TABLE_ENTRY prefix
    ListEntry InLoadOrderLinks;            // +0x00: list links ARE the entry
    ListEntry InMemoryOrderLinks;          // +0x10, unused
    ListEntry InInitializationOrderLinks;  // +0x20, unused
    unsigned long long DllBase;            // +0x30
    unsigned long long EntryPoint;         // +0x38, unused
    uint32_t SizeOfImage;                  // +0x40
    uint32_t pad1;                         // +0x44
    UnicodeStringView FullDllName;         // +0x48, unused
    UnicodeStringView BaseDllName;         // +0x58
};

static_assert(offsetof(PebView, ImageBaseAddress) == 0x10, "PEB layout pin");
static_assert(offsetof(PebView, Ldr) == 0x18, "PEB.Ldr pin (ntdll PDB)");
static_assert(offsetof(PebLdrDataView, InLoadOrderModuleList) == 0x10,
              "PEB_LDR_DATA pin (ntoskrnl PDB)");
static_assert(offsetof(LdrEntryView, DllBase) == 0x30,
              "LDR_DATA_TABLE_ENTRY pin (ntoskrnl PDB)");
static_assert(offsetof(LdrEntryView, SizeOfImage) == 0x40,
              "LDR_DATA_TABLE_ENTRY pin (ntoskrnl PDB)");
static_assert(offsetof(LdrEntryView, BaseDllName) == 0x58,
              "LDR_DATA_TABLE_ENTRY pin (ntoskrnl PDB)");
static_assert(sizeof(UnicodeStringView) == 16, "UNICODE_STRING pin");

constexpr unsigned kMaxModules = 256;
constexpr unsigned kNameChars = 64;

// Reads one trivially-copyable object of TARGET address space through
// the mirror. False when no window covers the va or the page faults.
template <typename T>
bool ReadTarget(const SM_ATTACH_OUT& out, unsigned long long targetVa,
                T* value) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "foreign-memory reads must be raw bytes");
    const unsigned long long va = TranslateVa(out, targetVa);
    return va != 0 &&
           SafeRead(reinterpret_cast<const void*>(va), value, sizeof(T));
}

bool ReadBaseDllName(const SM_ATTACH_OUT& out, const UnicodeStringView& str,
                     wchar_t* name) {
    if (str.Buffer == 0 || str.Length == 0 ||
        str.Length > (kNameChars - 1) * 2) {
        return false;
    }
    const unsigned long long va = TranslateVa(out, str.Buffer);
    if (va == 0 ||
        !SafeRead(reinterpret_cast<const void*>(va), name, str.Length)) {
        return false;
    }
    name[str.Length / 2] = L'\0';
    return true;
}

}  // namespace

unsigned LdrWalkModules(const SM_ATTACH_OUT& out) {
    if (out.TargetPeb == 0) {
        return 0;
    }
    PebView peb;
    if (!ReadTarget(out, out.TargetPeb, &peb) || peb.Ldr == 0) {
        wprintf(L"ldr: PEB->Ldr unreadable through the mirror\n");
        return 0;
    }
    if (peb.ImageBaseAddress != 0) {
        wprintf(
            L"ldr: PEB.ImageBaseAddress 0x%llx %ls driver-reported "
            L"0x%llx\n",
            peb.ImageBaseAddress,
            peb.ImageBaseAddress == out.TargetImageBase ? L"==" : L"!=",
            out.TargetImageBase);
    }

    PebLdrDataView ldrData;
    if (!ReadTarget(out, peb.Ldr, &ldrData)) {
        wprintf(L"ldr: PEB_LDR_DATA unreadable through the mirror\n");
        return 0;
    }
    // Entries link back to the head inside PEB_LDR_DATA; that address
    // ends the walk.
    const unsigned long long head =
        peb.Ldr + offsetof(PebLdrDataView, InLoadOrderModuleList);

    wprintf(L"ldr: target modules (via PEB 0x%llx):\n", out.TargetPeb);
    unsigned long long flink = ldrData.InLoadOrderModuleList.Flink;
    unsigned count = 0;
    while (flink != head && flink != 0 && count < kMaxModules) {
        LdrEntryView entry;
        if (!ReadTarget(out, flink, &entry)) {
            wprintf(L"ldr: entry %u unreadable, stopping\n", count);
            break;
        }
        wchar_t name[kNameChars] = L"?";
        ReadBaseDllName(out, entry.BaseDllName, name);
        const unsigned long long here = TranslateVa(out, entry.DllBase);
        wprintf(L"  [%2u] %-24ls base 0x%llx -> 0x%llx size 0x%x\n", count,
                name, entry.DllBase, here, entry.SizeOfImage);
        flink = entry.InLoadOrderLinks.Flink;
        ++count;
    }
    if (count == kMaxModules) {
        wprintf(L"ldr: hit the %u-module cap, list truncated\n", kMaxModules);
    }
    return count;
}
