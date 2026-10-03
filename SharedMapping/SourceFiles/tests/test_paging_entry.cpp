#include "gtest/gtest.h"

#include "common/paging_entry.h"
#include "common/pml4.h"

namespace {

// Two independently sourced facts must agree: the slot geometry in
// pml4.h and the region sizes stated by SDM Tables 5-15..5-20.
TEST(PagingEntry, RegionSizesMatchSlotGeometry) {
    EXPECT_EQ(SM_REGION_PML4E, SM_SLOT_SIZE);
    EXPECT_EQ(SM_REGION_PML4E, SM_PML4_ENTRIES * SM_REGION_PDPTE);
    EXPECT_EQ(SM_REGION_PDPTE, SM_PML4_ENTRIES * SM_REGION_PDE);
    EXPECT_EQ(SM_REGION_PDE, SM_PML4_ENTRIES * SM_REGION_PTE);
    EXPECT_EQ(SM_REGION_PML4E, 512ull * 1024 * 1024 * 1024);
    EXPECT_EQ(SM_REGION_PDPTE, 1024ull * 1024 * 1024);
    EXPECT_EQ(SM_REGION_PDE, 2ull * 1024 * 1024);
    EXPECT_EQ(SM_REGION_PTE, 4ull * 1024);
}

TEST(PagingEntry, PhysExtractionKeepsBits51To12Only) {
    const uint64_t phys = 0x0000'9ABC'DEF0'1000ull;
    const uint64_t entry = phys | SM_ENTRY_PRESENT | SM_ENTRY_USER |
                           SM_ENTRY_RW | SM_ENTRY_XD | SM_ENTRY_PKEY_MASK;
    EXPECT_EQ(SmEntryPhys(entry), phys);
    // The physical-address mask must not leak flag, pkey or XD bits.
    EXPECT_EQ(SmEntryPhys(SM_ENTRY_PHYS_MASK), SM_ENTRY_PHYS_MASK);
    EXPECT_EQ(SmEntryPhys(~SM_ENTRY_PHYS_MASK), 0u);
}

TEST(PagingEntry, FlagHelpers) {
    const uint64_t leaf = 0x0000'1234'5000'0000ull | SM_ENTRY_PRESENT |
                          SM_ENTRY_USER | SM_ENTRY_RW | SM_ENTRY_LARGE;
    EXPECT_TRUE(SmEntryIsPresent(leaf));
    EXPECT_TRUE(SmEntryUserAccessible(leaf));
    EXPECT_TRUE(SmEntryWritable(leaf));
    EXPECT_TRUE(SmEntryIsLarge(leaf));

    const uint64_t table_ref = SM_ENTRY_PRESENT;  // PML4E: U/S=0, R/W=0
    EXPECT_TRUE(SmEntryIsPresent(table_ref));
    EXPECT_FALSE(SmEntryUserAccessible(table_ref));
    EXPECT_FALSE(SmEntryWritable(table_ref));
    EXPECT_FALSE(SmEntryIsLarge(table_ref));

    EXPECT_FALSE(SmEntryIsPresent(0));  // not-present entry
}

// Bits 51:12 hold the physical address, bits 11:0 the flags: setting the
// low flag bits must never change the extracted physical address.
TEST(PagingEntry, FlagsDoNotAffectPhys) {
    const uint64_t phys = 0x0000'0002'8000'0000ull;
    for (uint64_t flags = 0; flags < 0x1000ull; ++flags) {
        EXPECT_EQ(SmEntryPhys(phys | flags), phys);
    }
}

// CR3 (sec. 5.5.2) and the paging entries (Tables 5-15..5-20) place the
// physical address in the same bits 51:12, per two different tables.
TEST(PagingEntry, Cr3PhysMaskMatchesEntryPhysMask) {
    EXPECT_EQ(SM_CR3_PHYS_MASK, SM_ENTRY_PHYS_MASK);
}

// A raw DTB may carry a PCID in bits 11:0; it must be stripped before the
// value is used as a physical address.
TEST(Pml4, Cr3ToPhysStripsPcid) {
    const uint64_t dtb = 0x0000'01A2'B000'0000ull | 0x1ADu  // PCID
                         | SM_CR3_NOFLUSH;
    EXPECT_EQ(SmCr3ToPhys(dtb), 0x0000'01A2'B000'0000ull);
    EXPECT_EQ(SmCr3ToPhys(0), 0u);
}

}  // namespace
