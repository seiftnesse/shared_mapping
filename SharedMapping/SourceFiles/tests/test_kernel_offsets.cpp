#include "gtest/gtest.h"

#include "common/kernel_offsets.h"

namespace {

// Pins the known rows so the CSV, the generated table and this test cannot
// drift apart silently. Both UBRs run the same ntoskrnl image.
TEST(KernelOffsets, KnownHostRowMatchesPdb) {
    const SM_KERNEL_OFFSETS* row = SmFindKernelOffsets(19045u, 6466u);
    ASSERT_NE(row, nullptr);
    const SM_KERNEL_OFFSETS* row2 = SmFindKernelOffsets(19045u, 6456u);
    ASSERT_NE(row2, nullptr);
    EXPECT_EQ(row->DirectoryTableBaseOffset, 0x28u);
    EXPECT_EQ(row->UserDirectoryTableBaseOffset, 0x388u);
    EXPECT_EQ(row->KprocessSize, 0x438u);
    EXPECT_EQ(row->MmpfnElementSize, 0x30u);
    EXPECT_EQ(row->MmpfnShareCountOffset, 0x18u);
    EXPECT_EQ(row->MmpfnShareCountShift, 0u);
    EXPECT_EQ(row->MmPfnDatabasePointerRva, 0xcfc510u);
    EXPECT_EQ(row->VmOffset, 0x680u);
    EXPECT_EQ(row->MmSupportSharedOffset, 0xc0u);
    EXPECT_EQ(row->ShadowMappingOffset, 0x48u);
    EXPECT_EQ(row->SectionBaseAddressOffset, 0x520u);
    EXPECT_EQ(row->PebOffset, 0x550u);
}

TEST(KernelOffsets, UnknownBuildIsRejected) {
    EXPECT_EQ(SmFindKernelOffsets(22000u, 1u), nullptr);
    EXPECT_EQ(SmFindKernelOffsets(19045u, 9999u), nullptr);
}

// Structural sanity of every row: offsets inside their structures, 8-byte
// alignment where the hardware requires it, unique runtime keys. The
// shadow-PML4 chain (Vm -> Shared -> ShadowMapping) offsets come from the
// PDB (llvm-pdbutil, see tools/dump-offsets.ps1); their exact values are
// per-build and pinned per row in KnownHostRowMatchesPdb.
TEST(KernelOffsets, RowsAreSane) {
    constexpr uint32_t count = std::size(SM_KernelOffsetTable);
    ASSERT_GT(count, 0u);
    for (uint32_t i = 0; i < count; ++i) {
        const SM_KERNEL_OFFSETS& row = SM_KernelOffsetTable[i];
        EXPECT_LT(row.DirectoryTableBaseOffset, row.KprocessSize);
        EXPECT_LT(row.UserDirectoryTableBaseOffset, row.KprocessSize);
        EXPECT_EQ(row.DirectoryTableBaseOffset % 8u, 0u);
        EXPECT_EQ(row.UserDirectoryTableBaseOffset % 8u, 0u);
        EXPECT_LT(row.MmpfnShareCountOffset, row.MmpfnElementSize);
        EXPECT_LT(row.MmpfnShareCountShift, 64u);
        EXPECT_GT(row.MmPfnDatabasePointerRva, 0u);
        // The attach path refuses a zero shadow-chain offset (truncated
        // table), so a zero in any row makes the row unusable.
        EXPECT_GT(row.VmOffset, 0u);
        EXPECT_GT(row.MmSupportSharedOffset, 0u);
        EXPECT_GT(row.ShadowMappingOffset, 0u);
        EXPECT_GT(row.SectionBaseAddressOffset, 0u);
        EXPECT_EQ(row.SectionBaseAddressOffset % 8u, 0u);
        EXPECT_GT(row.PebOffset, 0u);
        EXPECT_EQ(row.PebOffset % 8u, 0u);
        for (uint32_t j = i + 1; j < count; ++j) {
            EXPECT_FALSE(SM_KernelOffsetTable[j].OsBuild == row.OsBuild &&
                         SM_KernelOffsetTable[j].Ubr == row.Ubr)
                << "duplicate runtime key at rows " << i << " and " << j;
        }
    }
}

}  // namespace
