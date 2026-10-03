// Pure address-translation math shared by the driver, the client and the
// tests. Must stay free of any Windows/kernel headers.
//
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

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif

// Slot geometry per SDM Vol. 3A sec. 5.5.4 (Table 5-15: a PML4E controls a
// 512-GByte region, selected by VA bits 47:39; 512 entries per table):
// one slot spans 2^39 = 512 GiB; canonical user addresses (Vol. 1,
// sec. 3.3.7.1) cover exactly the lower 256 slots.
#define SM_PML4_ENTRIES 512u
#define SM_USER_SLOT_COUNT 256u
#define SM_SLOT_SHIFT 39u
#define SM_SLOT_SIZE (1ull << SM_SLOT_SHIFT)

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
