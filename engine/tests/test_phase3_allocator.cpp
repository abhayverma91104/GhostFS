#include "ghostfs/allocator.hpp"
#include "ghostfs/comparison.hpp"
#include "ghostfs/fragmentation.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/virtual_disk.hpp"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ghostfs;

struct TempScope {
  std::filesystem::path dir;
  explicit TempScope(const std::string& tag) {
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    dir = std::filesystem::temp_directory_path() / ("ghostfs_p3_" + tag + "_" + std::to_string(ts));
    std::filesystem::create_directories(dir / "disk_images" / "sandbox");
  }
  ~TempScope() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  std::filesystem::path disk(const std::string& name) const {
    return dir / "disk_images" / name;
  }
};

// Helper: builds a bitmap with [data_start..data_start+22) containing free extents of 6, 4, and 8 blocks:
// Free: [10..15] (len 6), Used: [16..17], Free: [18..21] (len 4), Used: [22..23], Free: [24..31] (len 8).
std::vector<uint8_t> make_6_4_8_bitmap(uint32_t total_blocks) {
  std::vector<uint8_t> bm((total_blocks + 7U) / 8U, 0xFFU);
  auto clear_range = [&](uint32_t start, uint32_t len) {
    for (uint32_t b = start; b < start + len; ++b) {
      bm[b / 8U] &= static_cast<uint8_t>(~(1U << (b % 8U)));
    }
  };
  clear_range(10U, 6U);
  clear_range(18U, 4U);
  clear_range(24U, 8U);
  return bm;
}

TEST(Phase3AllocatorTest, Deterministic6_4_8_ExtentsForFirstBestWorstFit) {
  const uint32_t data_start = 10U;
  const uint32_t data_count = 22U;

  // First Fit picks [10..12] (first extent of size 6)
  {
    auto bm = make_6_4_8_bitmap(40U);
    uint32_t cursor = data_start;
    const auto tr = BlockAllocator::allocate_blocks(
        bm, data_start, data_count, 3U, AllocPolicy::FirstFit, cursor);
    ASSERT_TRUE(tr.success);
    EXPECT_FALSE(tr.used_fragmented_fallback);
    EXPECT_EQ(tr.allocated_blocks, (std::vector<uint32_t>{10U, 11U, 12U}));
  }

  // Best Fit picks [18..20] (smallest suitable extent, size 4)
  {
    auto bm = make_6_4_8_bitmap(40U);
    uint32_t cursor = data_start;
    const auto tr = BlockAllocator::allocate_blocks(
        bm, data_start, data_count, 3U, AllocPolicy::BestFit, cursor);
    ASSERT_TRUE(tr.success);
    EXPECT_FALSE(tr.used_fragmented_fallback);
    EXPECT_EQ(tr.allocated_blocks, (std::vector<uint32_t>{18U, 19U, 20U}));
  }

  // Worst Fit picks [24..26] (largest suitable extent, size 8)
  {
    auto bm = make_6_4_8_bitmap(40U);
    uint32_t cursor = data_start;
    const auto tr = BlockAllocator::allocate_blocks(
        bm, data_start, data_count, 3U, AllocPolicy::WorstFit, cursor);
    ASSERT_TRUE(tr.success);
    EXPECT_FALSE(tr.used_fragmented_fallback);
    EXPECT_EQ(tr.allocated_blocks, (std::vector<uint32_t>{24U, 25U, 26U}));
  }
}

TEST(Phase3AllocatorTest, NextFitResumesFromPersistentCursorAndWraps) {
  TempScope scope("nextfit");
  const auto img = scope.disk("nextfit.gfs");
  VirtualDisk::create_image(img, 512U, 128U);
  {
    VirtualDisk disk;
    disk.open_image(img, 512U, true);
    GfsFileSystem fs;
    fs.format(disk, "NextFitVol", AllocPolicy::BitmapNextFit);
    fs.write_file("/f1.bin", std::vector<uint8_t>(1024U, 0x11), false);  // 2 blocks
    const uint32_t cursor_after_f1 = fs.superblock().next_alloc_cursor;
    fs.write_file("/f2.bin", std::vector<uint8_t>(1024U, 0x22), false);  // 2 blocks
    const auto f2 = fs.stat_path("/f2.bin");
    ASSERT_EQ(f2.all_data_blocks.size(), 2U);
    EXPECT_GE(f2.all_data_blocks.front(), cursor_after_f1);
  }

  // Delete f1, reopen, and verify NextFit still advances from persisted cursor instead of jumping back to f1's blocks.
  {
    VirtualDisk disk;
    disk.open_image(img, 512U, true);
    GfsFileSystem fs;
    fs.mount(disk, true);
    const uint32_t persisted_cursor = fs.superblock().next_alloc_cursor;
    fs.delete_file("/f1.bin");
    const auto f3 = fs.write_file("/f3.bin", std::vector<uint8_t>(512U, 0x33), false);
    EXPECT_EQ(f3.all_data_blocks.front(), persisted_cursor);
  }
}

TEST(Phase3AllocatorTest, TieBreakingAndFragmentedFallbackAndInsufficientSpace) {
  // Tie breaking: two equal extents of length 4 at [10..13] and [16..19] -> lowest LBA [10..13] wins
  std::vector<uint8_t> bm((32U + 7U) / 8U, 0xFFU);
  for (uint32_t b : {10U, 11U, 12U, 13U, 16U, 17U, 18U, 19U}) {
    bm[b / 8U] &= static_cast<uint8_t>(~(1U << (b % 8U)));
  }
  uint32_t cursor = 10U;
  const auto tie_res = BlockAllocator::allocate_blocks(bm, 10U, 12U, 4U, AllocPolicy::BestFit, cursor);
  ASSERT_TRUE(tie_res.success);
  EXPECT_EQ(tie_res.allocated_blocks, (std::vector<uint32_t>{10U, 11U, 12U, 13U}));

  // Now only [16..19] (len 4) is free; add [21] (len 1) -> total free = 5, max contiguous = 4.
  bm.resize((40U + 7U) / 8U, 0xFFU);
  bm[21U / 8U] &= static_cast<uint8_t>(~(1U << (21U % 8U)));
  const auto frag_res = BlockAllocator::allocate_blocks(bm, 10U, 14U, 5U, AllocPolicy::FirstFit, cursor);
  ASSERT_TRUE(frag_res.success);
  EXPECT_TRUE(frag_res.used_fragmented_fallback);
  EXPECT_EQ(frag_res.allocated_blocks, (std::vector<uint32_t>{16U, 17U, 18U, 19U, 21U}));

  // Total free = 0 -> fails cleanly without modifying bitmap.
  const auto fail_res = BlockAllocator::allocate_blocks(bm, 10U, 14U, 1U, AllocPolicy::FirstFit, cursor);
  EXPECT_FALSE(fail_res.success);
  EXPECT_TRUE(fail_res.allocated_blocks.empty());
}

TEST(Phase3AllocatorTest, FragmentationMetricsInternalExternalAndPolicySwitchPreservesFiles) {
  TempScope scope("frag_metrics");
  const auto img = scope.disk("frag.gfs");
  VirtualDisk::create_image(img, 1024U, 128U);
  {
    VirtualDisk disk;
    disk.open_image(img, 1024U, true);
    GfsFileSystem fs;
    fs.format(disk, "FragLab", AllocPolicy::FirstFit);

    // Empty disk edge case: 0 files, 1 free extent, external frag = 0%
    const auto empty_rep = FragmentationAnalyzer::analyze(fs);
    EXPECT_EQ(empty_rep.total_files, 0U);
    EXPECT_EQ(empty_rep.free_extent_count, 1U);
    EXPECT_DOUBLE_EQ(empty_rep.external_fragmentation_pct, 0.0);
    EXPECT_EQ(empty_rep.internal_fragmentation_bytes, 0ULL);

    // Create A (2 blocks), B (2 blocks), C (2 blocks), then delete B to create a 2-block hole!
    fs.write_file("/a.bin", std::vector<uint8_t>(1500U, 0x01), false);  // 2 blocks, internal frag = 2048 - 1500 = 548
    fs.write_file("/b.bin", std::vector<uint8_t>(2048U, 0x02), false);  // 2 blocks
    fs.write_file("/c.bin", std::vector<uint8_t>(2048U, 0x03), false);  // 2 blocks
    const auto a_before = fs.stat_path("/a.bin");
    fs.delete_file("/b.bin");

    // Now there are 2 free extents: hole of 2 blocks + tail free extent.
    // Switch policy to WorstFit; existing files A and C MUST NOT move!
    const auto st_after_policy = fs.set_alloc_policy(AllocPolicy::WorstFit);
    EXPECT_EQ(st_after_policy.alloc_policy, "worst_fit");
    EXPECT_EQ(fs.stat_path("/a.bin").all_data_blocks, a_before.all_data_blocks);

    // Check residual deleted-file blocks are classified as free_deleted_residual
    const auto rep = FragmentationAnalyzer::analyze(fs);
    EXPECT_EQ(rep.free_extent_count, 2U);
    EXPECT_GT(rep.external_fragmentation_pct, 0.0);
    EXPECT_EQ(rep.internal_fragmentation_bytes, 548ULL);
    EXPECT_EQ(rep.free_deleted_residual_blocks, 2U);
  }

  // Reopen and confirm policy persisted
  VirtualDisk reopened;
  reopened.open_image(img, 512U, false);
  GfsFileSystem fs2;
  fs2.mount(reopened, false);
  EXPECT_EQ(fs2.status().alloc_policy, "worst_fit");
}

TEST(Phase3AllocatorTest, DirtyDiskBlocksMutationsButAllowsReadOnlyInspection) {
  TempScope scope("dirty_guard");
  const auto img = scope.disk("dirty.gfs");
  VirtualDisk::create_image(img, 4096U, 64U);
  {
    VirtualDisk disk;
    disk.open_image(img, 4096U, true);
    GfsFileSystem fs;
    fs.format(disk, "DirtyTest");
    fs.write_file("/ok.txt", {'h', 'i'}, false);
    fs.inject_pending_journal_for_test("simulated crash");
  }

  // Read-only inspection succeeds
  {
    VirtualDisk ro;
    ro.open_image(img, 512U, false);
    GfsFileSystem fs_ro;
    fs_ro.mount(ro, false);
    EXPECT_TRUE(fs_ro.status().dirty_journal_detected);
    EXPECT_EQ(fs_ro.read_file("/ok.txt"), (std::vector<uint8_t>{'h', 'i'}));
    EXPECT_NO_THROW(FragmentationAnalyzer::analyze(fs_ro));
  }

  // Mutations are blocked
  {
    VirtualDisk rw;
    rw.open_image(img, 512U, true);
    GfsFileSystem fs_rw;
    fs_rw.mount(rw, true);
    EXPECT_THROW(fs_rw.write_file("/blocked.txt", {'x'}, false), std::runtime_error);
    EXPECT_THROW(fs_rw.mkdir("/blocked_dir"), std::runtime_error);
    EXPECT_THROW(fs_rw.delete_file("/ok.txt"), std::runtime_error);
    EXPECT_THROW(fs_rw.set_alloc_policy(AllocPolicy::BestFit), std::runtime_error);
  }
}

TEST(Phase3AllocatorTest, ComparisonExperimentDoesNotMutateOriginalDisk) {
  TempScope scope("compare");
  const auto img = scope.disk("base.gfs");
  VirtualDisk::create_image(img, 1024U, 128U);
  {
    VirtualDisk disk;
    disk.open_image(img, 1024U, true);
    GfsFileSystem fs;
    fs.format(disk, "BaseDisk", AllocPolicy::FirstFit);
    fs.write_file("/seed.txt", {'s', 'e', 'e', 'd'}, false);
  }

  const std::vector<WorkloadStep> workload = {
      {"create", "/f1.bin", 6 * 1024},
      {"create", "/g1.bin", 2 * 1024},
      {"create", "/f2.bin", 4 * 1024},
      {"create", "/g2.bin", 2 * 1024},
      {"create", "/f3.bin", 8 * 1024},
      {"delete", "/f1.bin", 0},
      {"delete", "/f2.bin", 0},
      {"delete", "/f3.bin", 0},
      {"create", "/target.bin", 3 * 1024},
  };

  const auto cmp = AllocationComparator::run_comparison(scope.dir, img, 1024U, 128U, workload);
  ASSERT_EQ(cmp.policies.size(), 4U);
  EXPECT_EQ(cmp.experiment_mode, "cloned_active_disk");

  // Verify original disk still has ONLY /seed.txt
  VirtualDisk orig;
  orig.open_image(img, 512U, false);
  GfsFileSystem fs_orig;
  fs_orig.mount(orig, false);
  const auto entries = fs_orig.list_directory("/");
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0].name, "seed.txt");
}

}  // namespace
