#ifndef PHYSMEM_H_
#define PHYSMEM_H_

#include <ntifs.h>

#include "common/paging_entry.h"

// The write primitives below are compiled only in the write-gated build
// (roadmap ground rule 2: no page-table write code in dry builds).
#ifndef SM_ENABLE_WRITE
#define SM_ENABLE_WRITE 0
#endif

// Truth-path access to physical pages of page tables: the
// \Device\PhysicalMemory section view reads the LIVE page tables, while
// MmCopyMemory point reads and the direct-map alias returned stale
// entries (findings 13/19). Primary attempt per access: hand-built MDL +
// MmMapLockedPages (refused by Mm for PML4 frames on 19041). Fallback:
// transient RW view of \Device\PhysicalMemory; the view is a USER VA in
// the current process, so accesses run under STAC (SMAP) inside __try,
// and the physical access itself is context-independent.

// Opens the lazily initialized state. Call once at DriverEntry.
NTSTATUS SmPhysInit(VOID);

// Closes the section handle (SmMirrorShutdown).
VOID SmPhysShutdown(VOID);

// TRUE when Phys falls into a physical RAM range (MmGetPhysicalMemoryRanges).
BOOLEAN SmPhysInRam(UINT64 Phys);

// Reads one page-table entry. FALSE when the page is not mappable.
BOOLEAN SmPhysReadEntry(UINT64 TablePhys, ULONG Index, UINT64* Value);

// Reads Count entries starting at Start. FALSE when unreadable.
BOOLEAN SmPhysReadEntries(UINT64 TablePhys, ULONG Start, ULONG Count,
                          UINT64* Out);

#if SM_ENABLE_WRITE
// Writes one entry and reads it back through the same mapping: *Written
// holds the post-write value (the write's verdict), FALSE on map failure.
BOOLEAN SmPhysWriteEntry(UINT64 TablePhys, ULONG Index, UINT64 Value,
                         UINT64* Written);

// Writes two entries in DIFFERENT pages back to back with the mappings
// established BEFORE either store and released after both: Mm operations
// (section unmap walks VADs and runs the shadow audit) never observe the
// intermediate state where the two page tables disagree. Readbacks land
// in WrittenA/WrittenB; FALSE when either page is not mappable (nothing
// is written then).
BOOLEAN SmPhysWritePair(UINT64 TablePhysA, ULONG IndexA, UINT64 TablePhysB,
                        ULONG IndexB, UINT64 ValueA, UINT64 ValueB,
                        UINT64* WrittenA, UINT64* WrittenB);
#endif  // SM_ENABLE_WRITE

#endif  // PHYSMEM_H_
