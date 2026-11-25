#include "ghostfs/crc32.hpp"
#include "ghostfs/gfs_format.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/sha256.hpp"
#include "ghostfs/virtual_disk.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ghostfs;

struct TempDiskScope {
  std::filesystem::path dir;
  explicit TempDiskScope(const std::string& suffix) {
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    dir = std::filesystem::temp_directory_path() / ("ghostfs_p2_" + suffix + "_" + std::to_string(ts));
    std::filesystem::create_directories(dir);
  }
  ~TempDiskScope() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  std::filesystem::path file(const std::string& name) const { return dir / name; }
};

TEST(Phase2DiskFsTest, CreateFormatAndReopenAcrossAllBlockSizes) {
  TempDiskScope scope("block_sizes");
  for (uint32_t bs : {512U, 1024U, 2048U, 4096U}) {
    const auto img = scope.file("disk_" + std::to_string(bs) + ".gfs");
    VirtualDisk::create_image(img, bs, 128U);
    {
      VirtualDisk disk;
      disk.open_image(img, bs, true);
      GfsFileSystem fs;
      fs.format(disk, "LabVol", AllocPolicy::FirstFit);
      fs.mkdir("/evidence");
      const std::string text = "hello ghostfs " + std::to_string(bs);
      fs.write_file("/evidence/note.txt", std::vector<uint8_t>(text.begin(), text.end()), false);
    }

    // Reopen cleanly with default hint 512 to confirm auto-detection from BootBlock.
    VirtualDisk reopened;
    reopened.open_image(img, 512U, false);
    GfsFileSystem fs2;
    fs2.mount(reopened, false);
    const auto st = fs2.status();
    EXPECT_EQ(st.block_size, bs);
    EXPECT_EQ(st.volume_label, "LabVol");
    EXPECT_TRUE(st.boot_crc_ok);
    EXPECT_TRUE(st.primary_sb_crc_ok);
    EXPECT_TRUE(st.backup_sb_crc_ok);
    EXPECT_FALSE(st.dirty_journal_detected);

    const auto read_bytes = fs2.read_file("/evidence/note.txt");
    const std::string got(read_bytes.begin(), read_bytes.end());
    EXPECT_EQ(got, "hello ghostfs " + std::to_string(bs));
  }
}

TEST(Phase2DiskFsTest, OutOfBoundsBlockAccessIsRejected) {
  TempDiskScope scope("oob");
  const auto img = scope.file("oob.gfs");
  VirtualDisk::create_image(img, 4096U, 64U);
  VirtualDisk disk;
  disk.open_image(img, 4096U, true);
  std::vector<uint8_t> buf(4096U, 0xAA);
  EXPECT_THROW(disk.read_block(64U, buf), std::out_of_range);
  EXPECT_THROW(disk.write_block(64U, buf), std::out_of_range);
  EXPECT_THROW(disk.read_raw(64ULL * 4096ULL, 16U, buf), std::out_of_range);
}

TEST(Phase2DiskFsTest, NestedDirectoriesEmptyMultiBlockAndBinarySha256) {
  TempDiskScope scope("nested_bin");
  const auto img = scope.file("nested.gfs");
  VirtualDisk::create_image(img, 1024U, 512U);
  {
    VirtualDisk disk;
    disk.open_image(img, 1024U, true);
    GfsFileSystem fs;
    fs.format(disk, "Forensic");
    fs.mkdir("/cases");
    fs.mkdir("/cases/2026");
    fs.mkdir("/cases/2026/alpha");

    // Empty file
    const auto empty_info = fs.write_file("/cases/2026/empty.bin", {}, false);
    EXPECT_EQ(empty_info.size_bytes, 0ULL);
    EXPECT_TRUE(empty_info.all_data_blocks.empty());

    // Multi-block binary payload (5000 bytes -> 5 blocks of 1024B)
    std::vector<uint8_t> payload(5000U);
    for (size_t i = 0; i < payload.size(); ++i) {
      payload[i] = static_cast<uint8_t>((i * 31U + 7U) & 0xFFU);
    }
    const std::string expected_sha = compute_sha256_hex(payload);
    const auto bin_info = fs.write_file("/cases/2026/alpha/dump.bin", payload, false);
    EXPECT_EQ(bin_info.all_data_blocks.size(), 5U);
    EXPECT_EQ(bin_info.indirect_block, 0U);

    const auto got = fs.read_file("/cases/2026/alpha/dump.bin");
    EXPECT_EQ(compute_sha256_hex(got), expected_sha);

    // Rename & modify
    fs.rename_path("/cases/2026/alpha/dump.bin", "/cases/2026/renamed.bin");
    EXPECT_THROW(fs.stat_path("/cases/2026/alpha/dump.bin"), std::runtime_error);
    const auto renamed = fs.read_file("/cases/2026/renamed.bin");
    EXPECT_EQ(compute_sha256_hex(renamed), expected_sha);
  }
}

TEST(Phase2DiskFsTest, SingleAndDoubleIndirectAddressingWorks) {
  TempDiskScope scope("indirect");
  const auto img = scope.file("indirect.gfs");
  // 512B blocks -> 12 direct (6 KiB), 128 single indirect (+64 KiB = 70 KiB).
  // Use 512 blocks of 512B (256 KiB total) so double indirect (> 71,680 B) fits easily.
  VirtualDisk::create_image(img, 512U, 512U);
  VirtualDisk disk;
  disk.open_image(img, 512U, true);
  GfsFileSystem fs;
  fs.format(disk, "IndirectTest");

  // Single indirect: 20 blocks = 10,240 bytes
  std::vector<uint8_t> single_payload(10240U, 0x5A);
  for (size_t i = 0; i < single_payload.size(); ++i) {
    single_payload[i] = static_cast<uint8_t>(i & 0xFFU);
  }
  const auto single_info = fs.write_file("/single.dat", single_payload, false);
  EXPECT_EQ(single_info.direct_blocks.size(), 12U);
  EXPECT_NE(single_info.indirect_block, 0U);
  EXPECT_EQ(single_info.double_indirect_block, 0U);
  EXPECT_EQ(single_info.all_data_blocks.size(), 20U);
  EXPECT_EQ(fs.read_file("/single.dat"), single_payload);

  // Double indirect: 150 blocks = 76,800 bytes (> 12 + 128 = 140 blocks)
  std::vector<uint8_t> dbl_payload(76800U);
  for (size_t i = 0; i < dbl_payload.size(); ++i) {
    dbl_payload[i] = static_cast<uint8_t>((i * 13U) & 0xFFU);
  }
  const auto dbl_info = fs.write_file("/double.dat", dbl_payload, false);
  EXPECT_NE(dbl_info.indirect_block, 0U);
  EXPECT_NE(dbl_info.double_indirect_block, 0U);
  EXPECT_EQ(dbl_info.all_data_blocks.size(), 150U);
  EXPECT_EQ(fs.read_file("/double.dat"), dbl_payload);
}

TEST(Phase2DiskFsTest, DuplicateNamesInsufficientSpaceAndInodeExhaustion) {
  TempDiskScope scope("limits");
  const auto img = scope.file("limits.gfs");
  // Small disk with only 4 total inodes (0=null, 1=root, 2 & 3 usable)
  VirtualDisk::create_image(img, 512U, 96U);
  VirtualDisk disk;
  disk.open_image(img, 512U, true);
  GfsFileSystem fs;
  fs.format(disk, "Limits", AllocPolicy::FirstFit, 4U);

  fs.write_file("/a.txt", {'A'}, false);
  EXPECT_THROW(fs.write_file("/a.txt", {'B'}, false), std::runtime_error);
  fs.write_file("/b.txt", {'B'}, false);

  // Now free_inodes == 0
  EXPECT_THROW(fs.write_file("/c.txt", {'C'}, false), std::runtime_error);

  // Overwriting existing file with more bytes than free_blocks must fail atomically
  const std::vector<uint8_t> too_big(96U * 512U, 0x11);
  EXPECT_THROW(fs.write_file("/a.txt", too_big, true), std::runtime_error);
  EXPECT_EQ(fs.read_file("/a.txt"), std::vector<uint8_t>{'A'});
}

TEST(Phase2DiskFsTest, ForensicDeletionLeavesRawBytesUntilOverwritten) {
  TempDiskScope scope("forensic_del");
  const auto img = scope.file("forensic.gfs");
  VirtualDisk::create_image(img, 4096U, 64U);
  std::vector<uint32_t> released;
  const std::string secret = "FORENSIC_EVIDENCE_MARKER_9911";

  {
    VirtualDisk disk;
    disk.open_image(img, 4096U, true);
    GfsFileSystem fs;
    fs.format(disk, "Evidence");
    fs.write_file("/secret.txt", std::vector<uint8_t>(secret.begin(), secret.end()), false);
    const auto del = fs.delete_file("/secret.txt");
    released = del.released_blocks;
    ASSERT_EQ(released.size(), 1U);
    EXPECT_THROW(fs.read_file("/secret.txt"), std::runtime_error);
  }

  // Reopen raw disk and verify the freed block STILL contains the secret marker bytes!
  VirtualDisk raw;
  raw.open_image(img, 4096U, false);
  std::vector<uint8_t> blk;
  raw.read_block(released[0], blk);
  const std::string raw_slice(blk.begin(), blk.begin() + static_cast<std::ptrdiff_t>(secret.size()));
  EXPECT_EQ(raw_slice, secret);
}

TEST(Phase2DiskFsTest, ConcurrentExclusiveLockPreventsConflictingWriteOpen) {
  TempDiskScope scope("locking");
  const auto img = scope.file("locked.gfs");
  VirtualDisk::create_image(img, 4096U, 64U);

  VirtualDisk holder;
  holder.open_image(img, 4096U, true, true);

  VirtualDisk contender;
  EXPECT_THROW(contender.open_image(img, 4096U, true, true), std::runtime_error);
  EXPECT_THROW(contender.open_image(img, 4096U, false, true), std::runtime_error);

  holder.close_image();
  EXPECT_NO_THROW(contender.open_image(img, 4096U, true, true));
}

TEST(Phase2DiskFsTest, InterruptedJournalOperationIsDetectedOnRemount) {
  TempDiskScope scope("journal_dirty");
  const auto img = scope.file("dirty.gfs");
  VirtualDisk::create_image(img, 4096U, 64U);
  {
    VirtualDisk disk;
    disk.open_image(img, 4096U, true);
    GfsFileSystem fs;
    fs.format(disk, "JournalTest");
    fs.inject_pending_journal_for_test("aborted write");
  }

  VirtualDisk reopened;
  reopened.open_image(img, 4096U, false);
  GfsFileSystem fs2;
  fs2.mount(reopened, false);
  const auto st = fs2.status();
  EXPECT_TRUE(st.dirty_journal_detected);
  EXPECT_EQ(st.fs_state, static_cast<uint8_t>(FsState::Dirty));
  ASSERT_FALSE(st.recent_journal.empty());
  EXPECT_EQ(st.recent_journal.front().status, static_cast<uint8_t>(JournalStatus::Pending));
  EXPECT_EQ(st.recent_journal.front().summary, "aborted write");
}

}  // namespace
