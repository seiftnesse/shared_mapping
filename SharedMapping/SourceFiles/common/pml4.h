// Sources for the layout facts used here (verified against SDM revision
// 093, document 325462-093):
//  - Intel SDM Vol. 3A, sec. 5.5.4 "Linear-Address Translation with 4-Level
//    Paging and 5-Level Paging"
//    https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html
//    (https://cdrdv2.intel.com/v1/dl/getContent/671200)
//  - Canonical addresses (user half of the VA space): SDM Vol. 1,
//    sec. 3.3.7.1 "Canonical Addressing"; matches the Windows x64 layout
//    (Windows Internals, 7th ed., Part 2, ch. 5).

#ifndef PML4_H_
#define PML4_H_

#include <stdint.h>

// Slot geometry per SDM Vol. 3A sec. 5.5.4 (Table 5-15: a PML4E controls a
// 512-GByte region, selected by VA bits 47:39; 512 entries per table):
// one slot spans 2^39 = 512 GiB; canonical user addresses (Vol. 1,
// sec. 3.3.7.1) cover exactly the lower 256 slots.
#define SM_PML4_ENTRIES 512u
#define SM_USER_SLOT_COUNT 256u
#define SM_SLOT_SHIFT 39u
#define SM_SLOT_SIZE (1ull << SM_SLOT_SHIFT)

// 4-level paging only: with 5-level paging enabled (CPUID.(EAX=7, ECX=0)
// :ECX[16]; "CR4.LA57 ... (bit 12 of CR4)", SDM Vol. 3A sec. 2.5) VA bits
// 52:48 select a PML5E and the user half outgrows 256 slots, invalidating
// the constants above. The driver must check LA57 before attaching and
// refuse otherwise.
#define SM_CR4_LA57 (1ull << 12)

// CR3 (a.k.a. DTB, KPROCESS.DirectoryTableBase) layout for 4-/5-level
// paging (SDM Vol. 3A sec. 5.5.2, p. 5-21): bits 51:12 hold the physical
// address of the PML4 (PML5) table. With CR4.PCIDE = 1 (bit 17), bits 11:0
// hold the PCID (sec. 5.10.1), and bit 63 of a mov-to-CR3 source operand is
// NOFLUSH: a plain CR3 reload (NOFLUSH = 0) invalidates only the current
// PCID's non-global TLB entries (sec. 5.10.4.1).
#define SM_CR3_PHYS_MASK 0x000FFFFFFFFFF000ull
#define SM_CR3_PCID_MASK 0xFFFull
#define SM_CR3_NOFLUSH (1ull << 63)

// Returns the PML4-slot-aligned CR3 page address of a raw DTB read.
static inline uint64_t SmCr3ToPhys(uint64_t dtb) {
    return dtb & SM_CR3_PHYS_MASK;
}

// Returns the PML4 slot index (VA bits 47:39) of `va`.
static inline uint32_t SmPml4Index(uint64_t va) {
    return (uint32_t)(va >> SM_SLOT_SHIFT) & 0x1FFu;
}

// Returns the base address of the 512-GiB window `slot`.
static inline uint64_t SmSlotBase(uint32_t slot) {
    return (uint64_t)slot << SM_SLOT_SHIFT;
}

// Returns the same va expressed inside `to_slot`; 0 when va does not live
// in `from_slot`. Note the collision: va == 0 remapped into slot 0 also
// returns 0 -- callers always operate on non-null target addresses.
static inline uint64_t SmRemapVa(uint64_t va, uint32_t from_slot,
                                 uint32_t to_slot) {
    if (SmPml4Index(va) != from_slot) {
        return 0;
    }
    return SmSlotBase(to_slot) | (va & (SM_SLOT_SIZE - 1u));
}

#endif  // PML4_H_
