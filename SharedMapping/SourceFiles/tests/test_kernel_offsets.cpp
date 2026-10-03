#include "gtest/gtest.h"

#include "common/kernel_offsets.h"

namespace {

// Pins the known row so the CSV, the generated table and this test cannot
// drift apart silently
TEST(KernelOffsets, KnownHostRowMatchesPdb) {
    const SM_KERNEL_OFFSETS* row = SmFindKernelOffsets(19045u, 6466u);
    ASSERT_NE(row, nullptr);
    EXPECT_EQ(row->DirectoryTableBaseOffset, 0x28u);
    EXPECT_EQ(row->UserDirectoryTableBaseOffset, 0x388u);
    EXPECT_EQ(row->KprocessSize, 0x438u);
    EXPECT_EQ(row->MmpfnElementSize, 0x30u);
    EXPECT_EQ(row->MmpfnShareCountOffset, 0x18u);
    EXPECT_EQ(row->MmpfnShareCountShift, 2u);
}

TEST(KernelOffsets, UnknownBuildIsRejected) {
    EXPECT_EQ(SmFindKernelOffsets(22000u, 1u), nullptr);
    EXPECT_EQ(SmFindKernelOffsets(19045u, 9999u), nullptr);
}

// Structural sanity of every row: offsets inside their structures, 8-byte
// alignment where the hardware requires it, unique runtime keys.
TEST(KernelOffsets, RowsAreSane) {
    constexpr uint32_t count = std::size(SM_KernelOffsetTable);
    ASSERT_GT(count, 0u);
    for (uint32_t i = 0; i < count; ++i) {
        const auto& [OsBuild, Ubr, DirectoryTableBaseOffset,
                     UserDirectoryTableBaseOffset, KprocessSize,
                     MmpfnElementSize, MmpfnShareCountOffset,
                     MmpfnShareCountShift] = SM_KernelOffsetTable[i];
        EXPECT_LT(DirectoryTableBaseOffset, KprocessSize);
        EXPECT_LT(UserDirectoryTableBaseOffset, KprocessSize);
        EXPECT_EQ(DirectoryTableBaseOffset % 8u, 0u);
        EXPECT_EQ(UserDirectoryTableBaseOffset % 8u, 0u);
        EXPECT_LT(MmpfnShareCountOffset, MmpfnElementSize);
        EXPECT_LT(MmpfnShareCountShift, 64u);
        for (uint32_t j = i + 1; j < count; ++j) {
            EXPECT_FALSE(SM_KernelOffsetTable[j].OsBuild == OsBuild &&
                         SM_KernelOffsetTable[j].Ubr == Ubr)
                << "duplicate runtime key at rows " << i << " and " << j;
        }
    }
}

}  // namespace
