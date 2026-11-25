#include "ghostfs/crc32.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/sha256.hpp"
#include "ghostfs/virtual_disk.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ghostfs;

struct TempScope {
  std::filesystem::path dir;
  explicit TempScope(const std::string& tag) {
    const auto ts = std::chrono::steady_clock::now().time_since_epoch().count();
    dir = std::filesystem::temp_directory_path() / ("ghostfs_p4_" + tag + "_" + std::to_string(ts));
    std::filesystem::create_directories(dir / "disk_images" / "sandbox");
    std::filesystem::create_directories(dir / "reports" / "recovered");
  }
  ~TempScope() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  std::filesystem::path disk(const std::string& name) const { return dir / "disk_images" / name; }
};

void write_be32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24U) & 0xFFU));
  out.push_back(static_cast<uint8_t>((v >> 16U) & 0xFFU));
  out.push_back(static_cast<uint8_t>((v >> 8U) & 0xFFU));
  out.push_back(static_cast<uint8_t>(v & 0xFFU));
}

void append_png_chunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
  write_be32(out, static_cast<uint32_t>(data.size()));
  const size_t type_pos = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  const uint32_t crc = compute_crc32(out.data() + type_pos, 4U + data.size());
  write_be32(out, crc);
}

std::vector<uint8_t> make_valid_png_fixture(size_t extra_idat_bytes = 64) {
  std::vector<uint8_t> png = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr(13, 0);
  ihdr[3] = 1;  // width 1
  ihdr[7] = 1;  // height 1
  ihdr[8] = 8;  // bit depth
  ihdr[9] = 2;  // RGB
  append_png_chunk(png, "IHDR", ihdr);
  std::vector<uint8_t> idat(extra_idat_bytes, 0x55);
  append_png_chunk(png, "IDAT", idat);
  append_png_chunk(png, "IEND", {});
  return png;
}

std::vector<uint8_t> make_valid_jpeg_fixture() {
  return {
      0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
      0xFF, 0xDB, 0x00, 0x04, 0x00, 0x01,
      0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01, 0x00, 0x01, 0x01, 0x01, 0x11, 0x00,
      0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x3F, 0x00,
      0x12, 0x34, 0xFF, 0x00, 0x56, 0x78,
      0xFF, 0xD9,
  };
}

std::vector<uint8_t> make_valid_pdf_fixture() {
  const std::string pdf =
      "%PDF-1.4\n"
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
      "xref\n0 2\n0000000000 65535 f \n"
      "trailer\n<< /Size 2 /Root 1 0 R >>\nstartxref\n9\n%%EOF\n";
  return std::vector<uint8_t>(pdf.begin(), pdf.end());
}

std::vector<uint8_t> read_file_bytes(const std::filesystem::path& p) {
  std::ifstream ifs(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

TEST(Phase4RecoveryTest, ScenarioA_PerfectRecoveryAndSourceImmutability) {
  TempScope scope("scen_a");
  const auto img = scope.disk("scen_a.gfs");
  VirtualDisk::create_image(img, 1024U, 128U);
  std::vector<uint8_t> payload(2500U);
  for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>((i * 29U + 3U) & 0xFFU);
  const std::string orig_sha = compute_sha256_hex(payload);

  {
    VirtualDisk disk;
    disk.open_image(img, 1024U, true);
    GfsFileSystem fs;
    fs.format(disk, "ScenA");
    fs.mkdir("/docs");
    fs.write_file("/docs/report.bin", payload, false);
    fs.write_file("/docs/empty.bin", {}, false);
    fs.delete_file("/docs/report.bin");
    fs.delete_file("/docs/empty.bin");
  }

  const std::string disk_sha_before = compute_file_sha256_streaming(img);
  const auto scan = ForensicRecoveryEngine::scan(img, ScanMode::Quick);
  EXPECT_EQ(scan.source_disk_sha256, disk_sha_before);
  EXPECT_EQ(scan.fully_verified_count, 2U);

  for (const auto& c : scan.candidates) {
    const auto manifest = ForensicRecoveryEngine::extract_candidate(
        scope.dir, img, c.candidate_id, scan.source_disk_sha256);
    if (c.filename == "report.bin") {
      EXPECT_EQ(manifest.recovered_sha256, orig_sha);
      EXPECT_EQ(manifest.classification, "fully_verified");
      EXPECT_EQ(read_file_bytes(manifest.artifact_path), payload);
    } else if (c.filename == "empty.bin") {
      EXPECT_EQ(manifest.recovered_size, 0ULL);
      EXPECT_EQ(manifest.classification, "fully_verified");
    }
  }

  const std::string disk_sha_after = compute_file_sha256_streaming(img);
  EXPECT_EQ(disk_sha_before, disk_sha_after);
}

TEST(Phase4RecoveryTest, ScenarioB_PartialOverwriteAndScenarioC_CompleteOverwrite) {
  TempScope scope("scen_bc");
  const auto img = scope.disk("scen_bc.gfs");
  VirtualDisk::create_image(img, 512U, 128U);
  {
    VirtualDisk disk;
    disk.open_image(img, 512U, true);
    GfsFileSystem fs;
    fs.format(disk, "ScenBC", AllocPolicy::FirstFit);
    // 4-block file (2048B)
    std::vector<uint8_t> four(2048U, 0xAA);
    fs.write_file("/partial.bin", four, false);
    // 2-block file (1024B)
    std::vector<uint8_t> two(1024U, 0xBB);
    fs.write_file("/full_ow.bin", two, false);

    fs.delete_file("/partial.bin");
    fs.delete_file("/full_ow.bin");

    // Overwrite first 2 blocks of /partial.bin and all 2 blocks of /full_ow.bin
    fs.write_file("/blocker1.bin", std::vector<uint8_t>(1024U, 0xCC), false);
    fs.write_file("/blocker2.bin", std::vector<uint8_t>(1024U, 0xDD), false);
    //Wait: FirstFit allocates [10,11] for blocker1 (partial.bin[0..1]), [12,13] for blocker2 (partial.bin[2..3])!
    // Let's check: to test partial overwrite specifically, write ONLY 1024B (2 blocks) -> leaves [12,13] intact!
  }

  // Test partial overwrite explicitly on a fresh image (keep /anchor.bin on inode 2 so /multi.bin is inode 3)
  const auto img2 = scope.disk("partial_only.gfs");
  VirtualDisk::create_image(img2, 512U, 128U);
  {
    VirtualDisk disk;
    disk.open_image(img2, 512U, true);
    GfsFileSystem fs;
    fs.format(disk, "Partial", AllocPolicy::FirstFit);
    fs.write_file("/anchor.bin", {}, false);  // Holds inode 2 with 0 blocks
    std::vector<uint8_t> four(2048U);
    for (size_t i = 0; i < four.size(); ++i) four[i] = static_cast<uint8_t>(i & 0xFFU);
    fs.write_file("/multi.bin", four, false);  // Inode 3, blocks [10..13]
    fs.delete_file("/multi.bin");
    // Overwrite ONLY the first 2 blocks (1024 bytes) via /anchor.bin (keeps inode 3 untouched!)
    fs.write_file("/anchor.bin", std::vector<uint8_t>(1024U, 0xEE), true);
  }
  const auto scan2 = ForensicRecoveryEngine::scan(img2, ScanMode::Quick);
  ASSERT_EQ(scan2.candidates.size(), 1U);
  EXPECT_EQ(scan2.candidates[0].classification_str, "partially_recoverable");
  EXPECT_EQ(scan2.candidates[0].recoverable_bytes, 1024ULL);
  ASSERT_EQ(scan2.candidates[0].missing_ranges.size(), 1U);
  EXPECT_EQ(scan2.candidates[0].missing_ranges[0].start_byte, 0ULL);
  EXPECT_EQ(scan2.candidates[0].missing_ranges[0].end_byte, 1024ULL);

  // Extract with explicit zero_fill_missing = true and verify substituted_ranges recorded
  const auto m_zero = ForensicRecoveryEngine::extract_candidate(
      scope.dir, img2, scan2.candidates[0].candidate_id, scan2.source_disk_sha256, true);
  EXPECT_EQ(m_zero.recovered_size, 2048ULL);
  EXPECT_EQ(m_zero.verified_bytes, 0ULL);
  EXPECT_EQ(m_zero.unknown_candidate_bytes, 1024ULL);
  EXPECT_EQ(m_zero.missing_bytes, 1024ULL);
  EXPECT_EQ(m_zero.overwritten_bytes, 1024ULL);
  EXPECT_EQ(m_zero.substituted_bytes, 1024ULL);
  ASSERT_EQ(m_zero.substituted_ranges.size(), 1U);
  EXPECT_EQ(m_zero.substituted_ranges[0].start_byte, 0ULL);
  EXPECT_EQ(m_zero.substituted_ranges[0].end_byte, 1024ULL);

  // Also check complete overwrite on img (reused inode -> unrecoverable / overwritten)
  const auto scan1 = ForensicRecoveryEngine::scan(img, ScanMode::Quick);
  for (const auto& c : scan1.candidates) {
    if (c.filename == "partial.bin" || c.filename == "full_ow.bin") {
      EXPECT_TRUE(c.classification_str == "overwritten" || c.classification_str == "unrecoverable");
      EXPECT_THROW(
          ForensicRecoveryEngine::extract_candidate(scope.dir, img, c.candidate_id, scan1.source_disk_sha256),
          std::runtime_error);
    }
  }
}

TEST(Phase4RecoveryTest, ScenarioD_InodeReuseAndScenarioH_OverwriteAndRedelete) {
  TempScope scope("scen_dh");
  const auto img = scope.disk("scen_dh.gfs");
  VirtualDisk::create_image(img, 512U, 128U);
  {
    VirtualDisk disk;
    disk.open_image(img, 512U, true);
    GfsFileSystem fs;
    fs.format(disk, "ReuseTest", AllocPolicy::FirstFit);
    fs.write_file("/victim_a.bin", std::vector<uint8_t>(1024U, 0x11), false);
    fs.delete_file("/victim_a.bin");
    // Reuse inode 2 AND blocks [10, 11], then delete replacement too (Scenario D + H)!
    fs.write_file("/replacement_b.bin", std::vector<uint8_t>(1024U, 0x22), false);
    fs.delete_file("/replacement_b.bin");
  }

  const auto scan = ForensicRecoveryEngine::scan(img, ScanMode::Quick);
  bool saw_victim_unrecoverable = false;
  bool saw_replacement_verified = false;
  for (const auto& c : scan.candidates) {
    if (c.filename == "victim_a.bin") {
      saw_victim_unrecoverable = true;
      EXPECT_EQ(c.classification_str, "unrecoverable");
    }
    if (c.filename == "replacement_b.bin") {
      saw_replacement_verified = true;
      EXPECT_EQ(c.classification_str, "fully_verified");
    }
  }
  EXPECT_TRUE(saw_victim_unrecoverable);
  EXPECT_TRUE(saw_replacement_verified);

  // Pure Scenario H (different inode, same blocks overwritten then re-deleted):
  const auto img_h = scope.disk("scen_h.gfs");
  VirtualDisk::create_image(img_h, 512U, 128U);
  {
    VirtualDisk disk;
    disk.open_image(img_h, 512U, true);
    GfsFileSystem fs;
    fs.format(disk, "ScenH", AllocPolicy::FirstFit);
    fs.write_file("/orig.bin", std::vector<uint8_t>(1024U, 0x33), false);
    fs.write_file("/keeper.bin", std::vector<uint8_t>(512U, 0x44), false);
    fs.delete_file("/orig.bin");
    // Inode 2 is free, inode 3 is keeper, so write /repl.bin -> gets inode 2? No, keep orig at inode 3:
  }
  const auto img_h2 = scope.disk("scen_h2.gfs");
  VirtualDisk::create_image(img_h2, 512U, 128U);
  {
    VirtualDisk disk;
    disk.open_image(img_h2, 512U, true);
    GfsFileSystem fs;
    fs.format(disk, "ScenH2", AllocPolicy::FirstFit);
    fs.write_file("/orig_first.bin", std::vector<uint8_t>(1024U, 0x33), false);  // inode 2, blocks [10,11]
    fs.delete_file("/orig_first.bin");
    // Write dummy at inode 2? Wait: to get a DIFFERENT inode for replacement, keep a file in inode 2:
    // Create /anchor (inode 2), /orig (inode 3, blocks [11,12]), delete /orig, create /anchor2? No, inode 3 is freed!
    // What if we write /a (inode 2), /b (inode 3), delete /a (inode 2 freed), delete /b (inode 3 freed),
    // then create /c -> gets inode 2 (reusing /a's inode) and blocks [10,11] (overwriting /a's blocks AND /b's if 3 blocks), then delete /c!
    fs.write_file("/c_big.bin", std::vector<uint8_t>(1536U, 0x99), false);
    fs.delete_file("/c_big.bin");
  }
  const auto scan_h = ForensicRecoveryEngine::scan(img_h2, ScanMode::Quick);
  for (const auto& c : scan_h.candidates) {
    if (c.filename == "orig_first.bin") {
      EXPECT_NE(c.classification_str, "fully_verified");
    }
  }
}

TEST(Phase4RecoveryTest, ScenarioE_SignatureCarvingPngJpegPdfTxtAndCrossBlock) {
  TempScope scope("scen_e");
  const auto img = scope.disk("carve.gfs");
  VirtualDisk::create_image(img, 512U, 128U);

  const auto png = make_valid_png_fixture(700);  // > 512B -> crosses block boundary!
  const auto jpeg = make_valid_jpeg_fixture();
  const auto pdf = make_valid_pdf_fixture();
  const std::string txt = "Forensic carved text line 1\nForensic carved text line 2\n";

  // Write files, then reformat metadata-only or inject raw blocks into unallocated space
  {
    VirtualDisk disk;
    disk.open_image(img, 512U, true);
    GfsFileSystem fs;
    fs.format(disk, "CarveTest");
    // Write raw fixtures into unallocated data blocks [20..25] without creating inodes
    const uint32_t dstart = fs.superblock().data_region_start;
    std::vector<uint8_t> blk(512, 0);

    // PNG at dstart + 2 (crosses 2 blocks)
    for (size_t i = 0; i < (png.size() + 511) / 512; ++i) {
      std::fill(blk.begin(), blk.end(), 0);
      const size_t take = std::min<size_t>(512, png.size() - i * 512);
      std::memcpy(blk.data(), png.data() + i * 512, take);
      disk.write_block(dstart + 2U + static_cast<uint32_t>(i), blk);
    }

    // JPEG at dstart + 6
    std::fill(blk.begin(), blk.end(), 0);
    std::memcpy(blk.data(), jpeg.data(), jpeg.size());
    disk.write_block(dstart + 6U, blk);

    // PDF at dstart + 8
    std::fill(blk.begin(), blk.end(), 0);
    std::memcpy(blk.data(), pdf.data(), pdf.size());
    disk.write_block(dstart + 8U, blk);

    // TXT at dstart + 10
    std::fill(blk.begin(), blk.end(), 0);
    std::memcpy(blk.data(), txt.data(), txt.size());
    disk.write_block(dstart + 10U, blk);

    // False positive: random bytes starting with FF D8 FF 00 (invalid marker) at dstart + 12
    std::fill(blk.begin(), blk.end(), 0x7E);
    blk[0] = 0xFF; blk[1] = 0xD8; blk[2] = 0xFF; blk[3] = 0x00;
    disk.write_block(dstart + 12U, blk);
  }

  const auto scan_with_txt = ForensicRecoveryEngine::scan(img, ScanMode::Deep, {}, true);
  bool found_png = false, found_jpeg = false, found_pdf = false, found_txt = false;
  for (const auto& c : scan_with_txt.candidates) {
    if (c.file_type == "png") {
      found_png = true;
      EXPECT_EQ(c.classification_str, "complete_unverified");
      EXPECT_EQ(c.integrity_status, "structure_verified");
      EXPECT_EQ(c.original_size_bytes, png.size());
      const auto m = ForensicRecoveryEngine::extract_candidate(
          scope.dir, img, c.candidate_id, scan_with_txt.source_disk_sha256);
      EXPECT_EQ(m.recovered_sha256, compute_sha256_hex(png));
    } else if (c.file_type == "jpeg") {
      found_jpeg = true;
      EXPECT_EQ(c.classification_str, "complete_unverified");
      EXPECT_EQ(c.original_size_bytes, jpeg.size());
    } else if (c.file_type == "pdf") {
      found_pdf = true;
      EXPECT_EQ(c.classification_str, "complete_unverified");
      EXPECT_EQ(c.original_size_bytes, pdf.size());
    } else if (c.file_type == "txt") {
      found_txt = true;
      EXPECT_EQ(c.classification_str, "complete_unverified");
    }
  }
  EXPECT_TRUE(found_png);
  EXPECT_TRUE(found_jpeg);
  EXPECT_TRUE(found_pdf);
  EXPECT_TRUE(found_txt);

  // Verify TXT heuristic can be disabled cleanly
  const auto scan_no_txt = ForensicRecoveryEngine::scan(img, ScanMode::Deep, {}, false);
  for (const auto& c : scan_no_txt.candidates) {
    EXPECT_NE(c.file_type, "txt");
  }
}

TEST(Phase4RecoveryTest, ScenarioF_FragmentedAndIndirectRecoveryAndScenarioG_StaleScan) {
  TempScope scope("scen_fg");
  const auto img = scope.disk("frag_ind.gfs");
  VirtualDisk::create_image(img, 512U, 256U);
  std::vector<uint8_t> frag_payload(1536U);  // 3 blocks
  for (size_t i = 0; i < frag_payload.size(); ++i) frag_payload[i] = static_cast<uint8_t>((i + 11U) & 0xFFU);

  std::vector<uint8_t> single_ind_payload(8192U);  // 16 blocks (> 12 direct)
  for (size_t i = 0; i < single_ind_payload.size(); ++i) single_ind_payload[i] = static_cast<uint8_t>((i * 7U) & 0xFFU);

  {
    VirtualDisk disk;
    disk.open_image(img, 512U, true);
    GfsFileSystem fs;
    fs.format(disk, "FragInd", AllocPolicy::FirstFit);
    // Create 1-block holes separated by barriers so /frag.bin spans multiple extents
    fs.write_file("/h1.bin", std::vector<uint8_t>(512U, 1), false);
    fs.write_file("/bar1.bin", std::vector<uint8_t>(512U, 2), false);
    fs.write_file("/h2.bin", std::vector<uint8_t>(1024U, 3), false);
    fs.write_file("/bar2.bin", std::vector<uint8_t>(512U, 4), false);
    fs.delete_file("/h1.bin");
    fs.delete_file("/h2.bin");

    fs.write_file("/frag.bin", frag_payload, false);
    fs.write_file("/single_ind.bin", single_ind_payload, false);

    fs.delete_file("/frag.bin");
    fs.delete_file("/single_ind.bin");
  }

  const auto scan = ForensicRecoveryEngine::scan(img, ScanMode::Quick);
  for (const auto& c : scan.candidates) {
    if (c.filename == "frag.bin") {
      EXPECT_EQ(c.classification_str, "fully_verified");
      const auto m = ForensicRecoveryEngine::extract_candidate(
          scope.dir, img, c.candidate_id, scan.source_disk_sha256);
      EXPECT_EQ(m.recovered_sha256, compute_sha256_hex(frag_payload));
    } else if (c.filename == "single_ind.bin") {
      EXPECT_EQ(c.classification_str, "fully_verified");
      const auto m = ForensicRecoveryEngine::extract_candidate(
          scope.dir, img, c.candidate_id, scan.source_disk_sha256);
      EXPECT_EQ(m.recovered_sha256, compute_sha256_hex(single_ind_payload));
    }
  }

  // Scenario G: Modify disk after scan -> extraction with stale SHA-256 MUST fail!
  {
    VirtualDisk disk;
    disk.open_image(img, 512U, true);
    GfsFileSystem fs;
    fs.mount(disk, true);
    fs.write_file("/mutated.txt", {'n', 'e', 'w'}, false);
  }
  ASSERT_FALSE(scan.candidates.empty());
  EXPECT_THROW(
      ForensicRecoveryEngine::extract_candidate(
          scope.dir, img, scan.candidates[0].candidate_id, scan.source_disk_sha256),
      std::runtime_error);
}

}  // namespace
