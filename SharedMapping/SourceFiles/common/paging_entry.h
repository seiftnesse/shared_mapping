// Hardware formats of x86-64 paging-structure entries (4-level paging).
//
// Source: Intel SDM Vol. 3A, sec. 5.5.4 "Linear-Address Translation with
// 4-Level Paging and 5-Level Paging", Tables 5-15..5-20 (verified against
// revision 093, document 325462-093):
// https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
// (https://cdrdv2.intel.com/v1/dl/getContent/671200)
//
// Windows caveat: the architecturally "ignored" bits are repurposed by the
// Windows memory manager as software PTE state (transition, prototype,
// working-set index, ...). Entries must be copied whole; only the flags
// below have stable hardware meaning (Windows Internals, 7th ed., Part 2,
// ch. 5).

#ifndef PAGING_ENTRY_H_
#define PAGING_ENTRY_H_

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif

#include "pml4.h"

// Flag bits shared by the paging-structure entries (Tables 5-15..5-20).
// Bit 7 is dual-purpose: PS on PDPTE/PDE (selects a 1-GByte/2-MByte page,
// Tables 5-16/5-18; must be 0 on table references) and PAT on 4-KByte PTEs
// (Table 5-20). D and G are defined for leaf entries only. Bit 11 (R)
// exists for HLAT paging only and is ignored otherwise.
#define SM_ENTRY_PRESENT (1ull << 0)   // P
#define SM_ENTRY_RW (1ull << 1)        // R/W: 0 => writes not allowed
#define SM_ENTRY_USER (1ull << 2)      // U/S: 0 => supervisor only
#define SM_ENTRY_PWT (1ull << 3)       // page-level write-through
#define SM_ENTRY_PCD (1ull << 4)       // page-level cache disable
#define SM_ENTRY_ACCESSED (1ull << 5)  // A
#define SM_ENTRY_DIRTY (1ull << 6)     // D
#define SM_ENTRY_LARGE (1ull << 7)     // PS / PAT
#define SM_ENTRY_GLOBAL (1ull << 8)    // G
#define SM_ENTRY_RESTART (1ull << 11)  // R (HLAT)

// Physical address of the referenced table/page: bits 51:12. The top usable
// bit depends on MAXPHYADDR (table notes: CPUID.80000008H:EAX[7:0]); bits
// 51:M are reserved and must be 0 in well-formed entries, so masking 51:12
// is lossless.
#define SM_ENTRY_PHYS_SHIFT 12u
#define SM_ENTRY_PHYS_MASK 0x000FFFFFFFFFF000ull

// Protection key: bits 62:59, leaf entries only (Tables 5-16/5-18/5-20),
// active with CR4.PKE/CR4.PKS.
#define SM_ENTRY_PKEY_SHIFT 59u
#define SM_ENTRY_PKEY_MASK (0xFull << SM_ENTRY_PKEY_SHIFT)

#define SM_ENTRY_XD (1ull << 63)  // execute disable when IA32_EFER.NXE = 1

// Region controlled by one entry (Tables 5-15..5-20): PML4E -> 512 GByte,
// PDPTE -> 1 GByte, PDE -> 2 MByte, PTE -> 4 KByte.
#define SM_REGION_PML4E SM_SLOT_SIZE
#define SM_REGION_PDPTE (1ull << 30)
#define SM_REGION_PDE (1ull << 21)
#define SM_REGION_PTE (1ull << 12)

static inline int SmEntryIsPresent(uint64_t entry) {
    return (entry & SM_ENTRY_PRESENT) != 0;
}

static inline uint64_t SmEntryPhys(uint64_t entry) {
    return entry & SM_ENTRY_PHYS_MASK;
}

// PS bit as seen on PDPTE/PDE: 1 => the entry maps a 1-GByte/2-MByte page.
static inline int SmEntryIsLarge(uint64_t entry) {
    return (entry & SM_ENTRY_LARGE) != 0;
}

static inline int SmEntryUserAccessible(uint64_t entry) {
    return (entry & SM_ENTRY_USER) != 0;
}

static inline int SmEntryWritable(uint64_t entry) {
  return (entry & SM_ENTRY_RW) != 0;
}

#endif  // PAGING_ENTRY_H_
