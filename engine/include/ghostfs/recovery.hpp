#pragma once

#include "ghostfs/gfs_format.hpp"
#include "ghostfs/gfs_fs.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ghostfs {

enum class ScanMode {
  Quick,
  Deep,
  Combined,
};

enum class RecoveryClassification {
  FullyVerified,
  CompleteUnverified,
  PartiallyRecoverable,
  Overwritten,
  Unrecoverable,
  Unknown,
};

struct ByteRange {
  uint64_t start_byte = 0;
  uint64_t end_byte = 0;  // exclusive
};

struct LogicalBlockStatus {
  uint32_t logical_index = 0;
  uint32_t physical_block = 0;
  uint64_t disk_byte_offset = 0;
  std::string state;  // "intact" | "overwritten" | "missing_indirect" | "unknown"
  std::string detail;
};

struct RecoveryCandidate {
  std::string candidate_id;
  std::string recovery_method;  // "metadata" | "carving" | "combined"
  std::string file_type;        // "png" | "jpeg" | "pdf" | "txt" | "unknown"
  std::string filename;
  std::string original_path;
  bool filename_inferred = false;

  uint32_t inode_id = 0;
  uint32_t generation = 0;
  uint64_t original_size_bytes = 0;
  uint64_t recoverable_bytes = 0;
  double recovery_pct = 0.0;

  uint64_t created_at_ms = 0;
  uint64_t modified_at_ms = 0;
  uint64_t deleted_at_ms = 0;
  uint32_t expected_crc32 = 0;
  bool has_expected_crc32 = false;
  uint32_t reconstructed_crc32 = 0;
  bool crc32_matched = false;
  std::string integrity_status;  // "crc32_verified" | "crc32_mismatch" | "unverified" | "partial" | "empty"

  RecoveryClassification classification = RecoveryClassification::Unknown;
  std::string classification_str;

  std::vector<uint32_t> source_blocks;
  std::vector<uint64_t> source_byte_offsets;
  std::vector<LogicalBlockStatus> block_statuses;
  std::vector<ByteRange> recoverable_ranges;
  std::vector<ByteRange> missing_ranges;
  std::vector<std::string> evidence;
  std::vector<std::string> warnings;
};

struct ForensicScanResult {
  std::string scan_id;
  std::string disk_name;
  std::string source_disk_sha256;
  std::string scan_mode;
  double scan_duration_ms = 0.0;
  uint32_t blocks_examined = 0;
  uint64_t bytes_examined = 0;
  uint32_t total_candidates = 0;

  uint32_t fully_verified_count = 0;
  uint32_t complete_unverified_count = 0;
  uint32_t partially_recoverable_count = 0;
  uint32_t overwritten_count = 0;
  uint32_t unrecoverable_count = 0;
  uint32_t unknown_count = 0;

  std::vector<std::string> warnings;
  std::vector<RecoveryCandidate> candidates;
};

struct RecoveryManifest {
  std::string recovery_id;
  std::string artifact_filename;
  std::string artifact_path;
  std::string manifest_path;
  std::string source_disk_name;
  std::string source_disk_sha256;
  std::string recovery_timestamp_utc;
  std::string recovery_method;
  std::string original_filename;
  uint32_t inode_id = 0;
  uint32_t generation = 0;
  uint64_t original_expected_size = 0;
  uint64_t recovered_size = 0;
  uint64_t verified_bytes = 0;
  uint64_t unknown_candidate_bytes = 0;
  uint64_t missing_bytes = 0;
  uint64_t overwritten_bytes = 0;
  uint64_t substituted_bytes = 0;
  std::vector<uint32_t> source_blocks;
  std::vector<uint64_t> source_byte_offsets;
  std::vector<ByteRange> missing_ranges;
  std::vector<ByteRange> substituted_ranges;
  bool has_original_crc32 = false;
  uint32_t original_crc32 = 0;
  uint32_t recovered_crc32 = 0;
  std::string recovered_sha256;
  std::string integrity_result;
  std::string classification;
  std::string engine_version;
  std::vector<std::string> warnings;
};

std::string classification_to_string(RecoveryClassification c);
ScanMode parse_scan_mode(const std::string& mode);
std::string compute_file_sha256_streaming(const std::filesystem::path& file_path);

class ForensicRecoveryEngine {
 public:
  static ForensicScanResult scan(
      const std::filesystem::path& disk_path,
      ScanMode mode,
      const std::vector<std::string>& file_type_filters = {},
      bool enable_txt_heuristic = true);

  static RecoveryManifest extract_candidate(
      const std::filesystem::path& workspace,
      const std::filesystem::path& disk_path,
      const std::string& candidate_id,
      const std::string& expected_source_sha256,
      bool zero_fill_missing = false,
      bool overwrite_existing = false,
      bool enable_txt_heuristic = true);
};

/// Internal helpers exposed for unit testing & modular decomposition.
std::vector<RecoveryCandidate> run_metadata_recovery(
    const VirtualDisk& disk,
    const GfsFileSystem& fs,
    uint32_t& out_blocks_examined,
    uint64_t& out_bytes_examined,
    std::vector<std::string>& out_warnings);

std::vector<RecoveryCandidate> run_signature_carving(
    const VirtualDisk& disk,
    const GfsFileSystem& fs,
    bool enable_txt_heuristic,
    uint32_t& out_blocks_examined,
    uint64_t& out_bytes_examined,
    std::vector<std::string>& out_warnings);

}  // namespace ghostfs
