#include "ghostfs/crc32.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/hex_inspector.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/sha256.hpp"
#include "ghostfs/virtual_disk.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
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
    dir = std::filesystem::temp_directory_path() / ("ghostfs_p5_" + tag + "_" + std::to_string(ts));
    std::filesystem::create_directories(dir / "disk_images" / "sandbox");
    std::filesystem::create_directories(dir / "reports" / "recovered");
  }
  ~TempScope() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
  std::filesystem::path disk(const std::string& name) const { return dir / "disk_images" / name; }
};

TEST(Phase5HexTest, BinaryReadingBoundsAndFragmentedLogicalFile) {
  TempScope scope("read_frag");
  const auto img = scope.disk("frag.gfs");
  VirtualDisk::create_image(img, 512U, 128U);
  std::vector<uint8_t> frag_data(1536U);
  for (size_t i = 0; i < frag_data.size(); ++i) frag_data[i] = static_cast<uint8_t>((i * 3U + 1U) & 0xFFU);

  {
    VirtualDisk disk;
    disk.open_image(img, 512U, true);
    GfsFileSystem fs;
    fs.format(disk, "HexRead", AllocPolicy::FirstFit);
    fs.write_file("/h1.bin", std::vector<uint8_t>(512U, 0x11), false);
    fs.write_file("/bar.bin", std::vector<uint8_t>(512U, 0x22), false);
    fs.delete_file("/h1.bin");
    fs.write_file("/frag.bin", frag_data, false);
  }

  // Read across block boundary & final byte
  const auto cross = HexInspector::read_disk_range(img, 500ULL, 32U, 16U);
  EXPECT_EQ(cross.rows.size(), 2U);
  const auto tail = HexInspector::read_disk_range(img, 128ULL * 512ULL - 1ULL, 1U, 16U);
  EXPECT_EQ(tail.length, 1ULL);

  // Out-of-bounds / invalid lengths rejected
  EXPECT_THROW(HexInspector::read_disk_range(img, 128ULL * 512ULL, 1U), std::out_of_range);
  EXPECT_THROW(HexInspector::read_disk_range(img, 0ULL, 0U), std::invalid_argument);
  EXPECT_THROW(HexInspector::read_disk_range(img, 0ULL, 70000U), std::invalid_argument);

  // Logical file read reconstructs fragmented blocks in exact order
  const auto logical = HexInspector::read_logical_file(img, "/frag.bin", 0ULL, 1536U, 16U);
  std::vector<uint8_t> reassembled;
  for (const auto& r : logical.rows) {
    reassembled.insert(reassembled.end(), r.raw_bytes.begin(), r.raw_bytes.end());
  }
  EXPECT_EQ(reassembled, frag_data);
}

TEST(Phase5HexTest, BinaryInterpretationEndiannessFloatsAndUtf8) {
  const std::vector<uint8_t> bytes = {0x31, 0x53, 0x46, 0x47, 0x00, 0x00, 0x80, 0x3F};  // "1SFG" + 1.0f LE
  const auto bi = HexInspector::interpret_bytes(bytes);
  EXPECT_TRUE(bi.has_16);
  EXPECT_TRUE(bi.has_32);
  EXPECT_TRUE(bi.has_64);
  EXPECT_EQ(bi.u32_le, 0x47465331U);  // "GFS1"
  EXPECT_EQ(bi.u32_be, 0x31534647U);
  EXPECT_FALSE(bi.valid_utf8);  // 0x80 is invalid standalone UTF-8

  const auto utf8_bi = HexInspector::interpret_bytes({'O', 'K', '-', 0xE2, 0x9C, 0x93});
  EXPECT_TRUE(utf8_bi.valid_utf8);
  EXPECT_EQ(utf8_bi.utf8, "OK-\xE2\x9C\x93");

  // Insufficient bytes -> has_16/32/64 false
  const auto short_bi = HexInspector::interpret_bytes({0xFF});
  EXPECT_FALSE(short_bi.has_16);
  EXPECT_FALSE(short_bi.has_32);
  EXPECT_EQ(short_bi.u8, 255U);
  EXPECT_EQ(short_bi.i8, -1);
  EXPECT_FALSE(short_bi.valid_utf8);

  // Float32 1.0f LE = 00 00 80 3F
  const auto f_bi = HexInspector::interpret_bytes({0x00, 0x00, 0x80, 0x3F});
  EXPECT_FLOAT_EQ(f_bi.f32_le, 1.0f);
  EXPECT_EQ(f_bi.f32_le_status, "finite");
}

TEST(Phase5HexTest, StructureParsingSuperblocksInodeDirJournalAndChecksumMismatch) {
  TempScope scope("struct");
  const auto img = scope.disk("struct.gfs");
  VirtualDisk::create_image(img, 1024U, 128U);
  {
    VirtualDisk disk;
    disk.open_image(img, 1024U, true);
    GfsFileSystem fs;
    fs.format(disk, "StructVol");
    fs.write_file("/hello.txt", {'h', 'i'}, false);
  }

  const auto sb1 = HexInspector::decode_block_structure(img, 1U);
  EXPECT_EQ(sb1.structure_type, "primary_superblock");
  EXPECT_TRUE(sb1.checksum_present);
  EXPECT_TRUE(sb1.checksum_valid);

  const auto sb2 = HexInspector::decode_block_structure(img, 2U);
  EXPECT_EQ(sb2.structure_type, "backup_superblock");
  EXPECT_TRUE(sb2.checksum_valid);

  const auto cmp_sb = HexInspector::compare_disk_sources(img, "primary_vs_backup_sb");
  EXPECT_DOUBLE_EQ(cmp_sb.matching_pct, 100.0);
  EXPECT_FALSE(cmp_sb.sb_field_diffs.empty());
}

TEST(Phase5HexTest, SearchHexAsciiUtf8SignatureCrossBufferOverlappingAndFragmented) {
  TempScope scope("search");
  const auto img = scope.disk("search.gfs");
  VirtualDisk::create_image(img, 512U, 128U);
  const std::string marker = "GHOSTFS_DELETED_HEX_SEARCH_TARGET";
  {
    VirtualDisk disk;
    disk.open_image(img, 512U, true);
    GfsFileSystem fs;
    fs.format(disk, "SearchVol");
    // Fragmented active file containing "AAAA" across block boundary
    fs.write_file("/h1.bin", std::vector<uint8_t>(512U, 1), false);
    fs.write_file("/bar.bin", std::vector<uint8_t>(512U, 2), false);
    fs.delete_file("/h1.bin");
    std::vector<uint8_t> frag(1024U, 'B');
    frag[510] = 'A'; frag[511] = 'A'; frag[512] = 'A'; frag[513] = 'A';
    fs.write_file("/frag_search.bin", frag, false);
    // Write and delete marker AFTER frag_search so its unallocated block is not overwritten
    fs.write_file("/deleted.txt", std::vector<uint8_t>(marker.begin(), marker.end()), false);
    fs.delete_file("/deleted.txt");
  }

  const auto res_ascii = HexInspector::search(img, "ascii", marker, "unallocated");
  EXPECT_EQ(res_ascii.total_matches, 1U);

  // Overlapping matches: searching "AAA" inside "AAAA" yields 2 overlapping matches!
  const auto res_frag = HexInspector::search(img, "ascii", "AAA", "file", 0, 0, "/frag_search.bin");
  EXPECT_EQ(res_frag.total_matches, 2U);

  // Hex search for GFS1 magic 31 53 46 47
  const auto res_hex = HexInspector::search(img, "hex", "31 53 46 47", "metadata");
  EXPECT_GE(res_hex.total_matches, 2U);

  // Invalid hex pattern rejected
  EXPECT_THROW(HexInspector::search(img, "hex", "ZZZ", "disk"), std::invalid_argument);
}

TEST(Phase5HexTest, EntropyZeroAndUniform256AndVectorsAndImmutability) {
  // Empty range -> 0.0
  const auto empty_m = HexInspector::analyze_bytes({});
  EXPECT_EQ(empty_m.total_bytes, 0ULL);
  EXPECT_DOUBLE_EQ(empty_m.shannon_entropy, 0.0);

  // All-zero range -> entropy 0.0
  const auto zero_m = HexInspector::analyze_bytes(std::vector<uint8_t>(256U, 0x00));
  EXPECT_DOUBLE_EQ(zero_m.shannon_entropy, 0.0);
  EXPECT_DOUBLE_EQ(zero_m.zero_pct, 100.0);

  // Uniform 256-byte distribution -> entropy exactly 8.0 bits/byte
  std::vector<uint8_t> uniform(256U);
  for (size_t i = 0; i < 256U; ++i) uniform[i] = static_cast<uint8_t>(i);
  const auto uni_m = HexInspector::analyze_bytes(uniform);
  EXPECT_NEAR(uni_m.shannon_entropy, 8.0, 1e-9);
  EXPECT_EQ(uni_m.unique_byte_values, 256U);

  // Known CRC32 & SHA-256 test vectors for "123456789" / "abc"
  const std::vector<uint8_t> vec_123 = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(HexInspector::analyze_bytes(vec_123).crc32, 0xCBF43926U);
  const std::vector<uint8_t> vec_abc = {'a', 'b', 'c'};
  EXPECT_EQ(HexInspector::analyze_bytes(vec_abc).sha256,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

  // Source disk immutability across all Phase 5 operations
  TempScope scope("immut");
  const auto img = scope.disk("immut.gfs");
  VirtualDisk::create_image(img, 1024U, 64U);
  {
    VirtualDisk disk;
    disk.open_image(img, 1024U, true);
    GfsFileSystem fs;
    fs.format(disk, "ImmutVol");
    fs.write_file("/a.txt", {'a', 'b', 'c'}, false);
  }
  const std::string before = compute_file_sha256_streaming(img);
  (void)HexInspector::read_block_range(img, 1U, 1U);
  (void)HexInspector::decode_block_structure(img, 1U);
  (void)HexInspector::search(img, "ascii", "abc", "disk");
  (void)HexInspector::analyze_disk_range(img, 0ULL, 1024U);
  (void)HexInspector::compare_disk_sources(img, "primary_vs_backup_sb");
  const std::string after = compute_file_sha256_streaming(img);
  EXPECT_EQ(before, after);
}

}  // namespace
