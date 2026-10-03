#include "gtest/gtest.h"

#include "common/pml4.h"

namespace {

TEST(Pml4, SlotSizeIs512Gib) {
    EXPECT_EQ(SM_SLOT_SIZE, 0x0000'0080'0000'0000ull);
    EXPECT_EQ(SM_SLOT_SIZE, 512ull * 1024 * 1024 * 1024);
}

TEST(Pml4, Pml4IndexOfCanonicalUserVa) {
    // Typical image/TEB region on Win10/11 lives in the top user slot.
    EXPECT_EQ(SmPml4Index(0x0000'7FF6'F123'4567ull), 255u);
    // User-space ceiling.
    EXPECT_EQ(SmPml4Index(0x0000'7FFF'FFFF'FFFFull), 255u);
    // Low addresses (null page region) fall into slot 0.
    EXPECT_EQ(SmPml4Index(0x0000'0000'0001'0000ull), 0u);
    // First kernel slot starts at bit 47.
    EXPECT_EQ(SmPml4Index(0x0000'8000'0000'0000ull), 256u);
    EXPECT_EQ(SmPml4Index(0xFFFF'FFFF'FFFF'FFFFull), 511u);
}

TEST(Pml4, SlotBaseRoundTrip) {
    for (uint32_t slot = 0; slot < SM_USER_SLOT_COUNT; ++slot) {
        const uint64_t base = SmSlotBase(slot);
        EXPECT_EQ(SmPml4Index(base), slot);
        EXPECT_EQ(base & (SM_SLOT_SIZE - 1u), 0u);
    }
}

TEST(Pml4, RemapPreservesSlotOffset) {
    const uint64_t va = 0x0000'0100'1234'5678ull;  // slot 2
    const uint64_t remapped = SmRemapVa(va, 2u, 40u);
    EXPECT_EQ(remapped, 0x0000'1400'1234'5678ull);
    EXPECT_EQ(SmPml4Index(remapped), 40u);
    // The lower 39 bits must survive untouched.
    EXPECT_EQ(remapped & (SM_SLOT_SIZE - 1u), va & (SM_SLOT_SIZE - 1u));
}

TEST(Pml4, RemapSlotBoundaries) {
    const uint64_t first = SmSlotBase(7u);
    const uint64_t last = SmSlotBase(7u) + SM_SLOT_SIZE - 1u;
    EXPECT_EQ(SmRemapVa(first, 7u, 9u), SmSlotBase(9u));
    EXPECT_EQ(SmRemapVa(last, 7u, 9u), SmSlotBase(9u) + SM_SLOT_SIZE - 1u);
}

TEST(Pml4, RemapRejectsWrongSourceSlot) {
    const uint64_t va = SmSlotBase(3u) + 0x1000ull;
    EXPECT_EQ(SmRemapVa(va, 4u, 10u), 0u);
    EXPECT_EQ(SmRemapVa(va, 3u, 10u), SmSlotBase(10u) + 0x1000ull);
}

TEST(Pml4, UserSlotCountMatchesHardwareSplit) {
    EXPECT_EQ(SM_USER_SLOT_COUNT, SM_PML4_ENTRIES / 2u);
}

}  // namespace
