#pragma once

#include "ghostfs/binary_parser.hpp"
#include "ghostfs/gfs_format.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ghostfs {

inline constexpr size_t kMaxHexReadBytes = 65536U;      // 64 KiB max per request
inline constexpr size_t kMaxSearchPatternBytes = 4096U;
inline constexpr size_t kMaxSearchMatches = 500U;

struct HexRow {
  uint64_t abs_offset = 0;
  uint32_t block_id = 0;
  uint32_t block_offset = 0;
  uint64_t logical_offset = 0;
  std::vector<uint8_t> raw_bytes;
  std::vector<std::string> hex_bytes;
  std::string ascii;
};

struct SignatureCheckResult {
  bool detected = false;
  std::string file_type;
  std::string signature_hex;
  uint64_t offset = 0;
  uint32_t length = 0;
  bool structurally_valid = false;
  std::string validation_note;
  std::vector<std::string> warnings;
};

struct HexReadResult {
  std::string disk_name;
  std::string source_type;  // "disk_range" | "block" | "file" | "artifact"
  uint64_t start_offset = 0;
  uint64_t length = 0;
  uint32_t block_size = 0;
  uint32_t bytes_per_row = 16;
  uint64_t total_source_bytes = 0;
  uint32_t start_block = 0;
  std::string classification;
  uint32_t inode_id = 0;
  std::string owner_path;
  SignatureCheckResult signature;
  std::vector<HexRow> rows;
};

struct ByteInterpretation {
  size_t byte_count = 0;
  std::string hex;
  std::string binary;
  uint8_t u8 = 0;
  int8_t i8 = 0;
  bool has_16 = false;
  uint16_t u16_le = 0, u16_be = 0;
  int16_t i16_le = 0, i16_be = 0;
  bool has_32 = false;
  uint32_t u32_le = 0, u32_be = 0;
  int32_t i32_le = 0, i32_be = 0;
  float f32_le = 0.0f, f32_be = 0.0f;
  std::string f32_le_status, f32_be_status;
  bool has_64 = false;
  uint64_t u64_le = 0, u64_be = 0;
  int64_t i64_le = 0, i64_be = 0;
  double f64_le = 0.0, f64_be = 0.0;
  std::string f64_le_status, f64_be_status;
  std::string ascii;
  bool valid_utf8 = false;
  std::string utf8;
};

struct SearchMatch {
  uint64_t abs_offset = 0;
  uint32_t block_id = 0;
  uint32_t block_offset = 0;
  uint64_t logical_offset = 0;
  uint32_t match_length = 0;
  std::string matched_hex;
  std::string context_ascii;
};

struct SearchResult {
  std::string scope;
  std::string query_type;
  std::string source_disk_sha256;
  uint32_t total_matches = 0;
  bool truncated = false;
  uint32_t offset = 0;
  uint32_t limit = 50;
  uint64_t bytes_scanned = 0;
  std::vector<SearchMatch> matches;
};

struct ByteAnalysisMetrics {
  uint64_t total_bytes = 0;
  uint64_t zero_bytes = 0;
  uint64_t nonzero_bytes = 0;
  double zero_pct = 0.0;
  uint64_t printable_ascii_bytes = 0;
  double printable_ascii_pct = 0.0;
  uint32_t unique_byte_values = 0;
  double shannon_entropy = 0.0;
  uint32_t crc32 = 0;
  std::string sha256;
  std::array<uint64_t, 256> histogram{};
};

struct ByteDiffEntry {
  uint64_t offset = 0;
  bool has_a = false;
  bool has_b = false;
  uint8_t byte_a = 0;
  uint8_t byte_b = 0;
};

struct SuperblockFieldDiff {
  std::string field_name;
  std::string primary_value;
  std::string backup_value;
  bool differs = false;
  bool expected_difference = false;
};

struct CompareResult {
  std::string mode;
  uint64_t len_a = 0;
  uint64_t len_b = 0;
  uint64_t total_compared_bytes = 0;
  uint64_t matching_bytes = 0;
  uint64_t differing_bytes = 0;
  uint64_t missing_bytes = 0;
  bool has_first_diff = false;
  uint64_t first_diff_offset = 0;
  double matching_pct = 100.0;
  std::string sha256_a;
  std::string sha256_b;
  std::vector<ByteDiffEntry> diffs;
  std::vector<SuperblockFieldDiff> sb_field_diffs;
};

class HexInspector {
 public:
  static HexReadResult read_disk_range(
      const std::filesystem::path& disk_path,
      uint64_t offset,
      uint32_t length,
      uint32_t bytes_per_row = 16);

  static HexReadResult read_block_range(
      const std::filesystem::path& disk_path,
      uint32_t block_id,
      uint32_t block_count = 1,
      uint32_t bytes_per_row = 16);

  static HexReadResult read_logical_file(
      const std::filesystem::path& disk_path,
      const std::string& fs_path,
      uint64_t offset,
      uint32_t length,
      uint32_t bytes_per_row = 16);

  static HexReadResult read_artifact_range(
      const std::filesystem::path& artifact_path,
      uint64_t offset,
      uint32_t length,
      uint32_t bytes_per_row = 16);

  static ByteInterpretation interpret_bytes(const std::vector<uint8_t>& bytes);

  static DecodedStructure decode_block_structure(
      const std::filesystem::path& disk_path,
      uint32_t block_id,
      const std::string& force_type = "");

  static SignatureCheckResult inspect_signature_at(
      const std::filesystem::path& disk_path,
      uint64_t offset);

  static SearchResult search(
      const std::filesystem::path& disk_path,
      const std::string& query_type,   // "hex" | "ascii" | "utf8" | "signature"
      const std::string& query_value,  // hex string, text, or "png"|"jpeg"|"pdf"
      const std::string& scope,        // "disk" | "block_range" | "allocated" | "unallocated" | "file" | "metadata"
      uint32_t start_block = 0,
      uint32_t end_block = 0,
      const std::string& file_path = "",
      uint32_t offset = 0,
      uint32_t limit = 50);

  static ByteAnalysisMetrics analyze_bytes(const std::vector<uint8_t>& bytes);

  static ByteAnalysisMetrics analyze_disk_range(
      const std::filesystem::path& disk_path,
      uint64_t offset,
      uint32_t length);

  static CompareResult compare_byte_buffers(
      const std::vector<uint8_t>& a,
      const std::vector<uint8_t>& b,
      const std::string& mode,
      uint32_t max_diffs = 512);

  static CompareResult compare_disk_sources(
      const std::filesystem::path& disk_path,
      const std::string& mode,  // "blocks" | "ranges" | "files" | "primary_vs_backup_sb" | "file_vs_artifact"
      uint64_t a_start = 0,
      uint64_t a_len = 0,
      uint64_t b_start = 0,
      uint64_t b_len = 0,
      const std::string& path_a = "",
      const std::string& path_b = "",
      const std::filesystem::path& artifact_path = {});
};

}  // namespace ghostfs
