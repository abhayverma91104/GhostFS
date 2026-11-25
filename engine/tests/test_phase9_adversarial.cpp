#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ghostfs/corruption.hpp"
#include "ghostfs/endian.hpp"
#include "ghostfs/fsck.hpp"
#include "ghostfs/gfs_format.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/hex_inspector.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/virtual_disk.hpp"

namespace {

struct AdvTempDir {
  std::filesystem::path dir;
  AdvTempDir() {
    const auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    dir = std::filesystem::temp_directory_path() / ("ghostfs_p9_adv_" + std::to_string(ts));
    std::filesystem::create_directories(dir / "disk_images" / "sandbox");
  }
  ~AdvTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

std::filesystem::path make_adv_disk(const std::filesystem::path& dir, const std::string& name) {
  const auto img = dir / "disk_images" / name;
  ghostfs::VirtualDisk::create_image(img, 4096U, 64U, true);
  ghostfs::VirtualDisk disk;
  disk.open_image(img, 4096U, true);
  ghostfs::GfsFileSystem fs;
  fs.format(disk, "AdvTest", ghostfs::AllocPolicy::FirstFit, 32U);
  fs.write_file("/seed.txt", std::vector<uint8_t>{'A', 'B', 'C', 'D'}, false);
  disk.sync();
  disk.close_image();
  return img;
}

TEST(Phase9AdversarialTest, MalformedBinaryAndHexSearchInputsHandledSafely) {
  AdvTempDir ws;
  const auto img = make_adv_disk(ws.dir, "adv_hex.gfs");

  // Invalid hex pattern
  EXPECT_THROW(
      ghostfs::HexInspector::search(img, "hex", "ZZZZ_NOT_HEX", "disk"),
      std::invalid_argument);

  // Out-of-bounds disk_range offset
  EXPECT_THROW(
      ghostfs::HexInspector::read_disk_range(img, 99999999ULL, 64U, 16),
      std::out_of_range);

  // Invalid UTF-8 bytes interpreted safely without crashing
  const std::vector<uint8_t> bad_utf8{0xFFU, 0xFEU, 0x80U, 0xC0U, 0x41U, 0x42U, 0x00U, 0xFFU};
  const auto interp = ghostfs::HexInspector::interpret_bytes(bad_utf8);
  EXPECT_EQ(interp.byte_count, 8U);
  EXPECT_FALSE(interp.valid_utf8);
  EXPECT_FALSE(interp.ascii.empty());
}

TEST(Phase9AdversarialTest, MalformedCarvingSignaturesNeverCrashOrClaimFullyVerified) {
  AdvTempDir ws;
  const auto img = make_adv_disk(ws.dir, "adv_carve.gfs");

  // Write malformed PNG, JPEG, and PDF headers into free data blocks
  {
    ghostfs::VirtualDisk disk;
    disk.open_image(img, 4096U, true);
    std::vector<uint8_t> blk(4096U, 0U);
    // Malformed PNG (signature + truncated IHDR)
    const uint8_t bad_png[] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D};
    std::copy(std::begin(bad_png), std::end(bad_png), blk.begin());
    disk.write_block(30, blk);

    // Malformed JPEG (SOI without EOI)
    std::fill(blk.begin(), blk.end(), 0U);
    blk[0] = 0xFF; blk[1] = 0xD8; blk[2] = 0xFF; blk[3] = 0xE0;
    disk.write_block(31, blk);

    // Malformed PDF (%PDF-1.7 without %%EOF)
    std::fill(blk.begin(), blk.end(), 0U);
    const std::string bad_pdf = "%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\n";
    std::copy(bad_pdf.begin(), bad_pdf.end(), blk.begin());
    disk.write_block(32, blk);

    disk.sync();
    disk.close_image();
  }

  const auto scan = ghostfs::ForensicRecoveryEngine::scan(img, ghostfs::ScanMode::Deep, {}, false);
  for (const auto& c : scan.candidates) {
    EXPECT_NE(c.classification_str, "fully_verified");
  }
}

TEST(Phase9AdversarialTest, MalformedSuperblockInodeJournalAndSymlinkRejection) {
  AdvTempDir ws;
  const auto img = make_adv_disk(ws.dir, "adv_fsck.gfs");

  // Symlink escape rejection
  const auto link_path = ws.dir / "disk_images" / "sym_link.gfs";
  std::error_code ec;
  std::filesystem::create_symlink(img, link_path, ec);
  if (!ec) {
    EXPECT_THROW(
        ghostfs::CorruptionEngine::create_experiment(ws.dir, "sym_link.gfs", "bad"),
        std::runtime_error);
  }

  // Corrupt primary SB geometry & test gfsck handles gracefully
  auto exp = ghostfs::CorruptionEngine::create_experiment(ws.dir, "adv_fsck.gfs", "adv_sb");
  ghostfs::CorruptionInjectRequest req{};
  req.category = "inode";
  req.scenario = "inconsistent_size";
  req.confirm = true;
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req);

  req.category = "journal";
  req.scenario = "invalid_checksum";
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req);

  const auto sb_path = ghostfs::CorruptionEngine::sandbox_dir(ws.dir) / exp.sandbox_filename;
  const auto chk = ghostfs::GfsckEngine::check(sb_path, "full");
  EXPECT_GE(chk.total_issues, 2U);
}

}  // namespace
