#include "ghostfs/recovery.hpp"

#include "ghostfs/crc32.hpp"
#include "ghostfs/engine.hpp"
#include "ghostfs/sha256.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace ghostfs {

namespace {

std::string utc_now_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm utc_tm{};
#if defined(_WIN32)
  gmtime_s(&utc_tm, &t);
#else
  gmtime_r(&t, &utc_tm);
#endif
  std::ostringstream oss;
  oss << std::put_time(&utc_tm, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

std::string sanitize_filename(const std::string& raw) {
  std::string out;
  for (char c : raw) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  if (out.empty() || out == "." || out == "..") {
    out = "recovered.bin";
  }
  return out;
}

}  // namespace

std::string classification_to_string(RecoveryClassification c) {
  switch (c) {
    case RecoveryClassification::FullyVerified:
      return "fully_verified";
    case RecoveryClassification::CompleteUnverified:
      return "complete_unverified";
    case RecoveryClassification::PartiallyRecoverable:
      return "partially_recoverable";
    case RecoveryClassification::Overwritten:
      return "overwritten";
    case RecoveryClassification::Unrecoverable:
      return "unrecoverable";
    case RecoveryClassification::Unknown:
      return "unknown";
  }
  return "unknown";
}

ScanMode parse_scan_mode(const std::string& mode) {
  if (mode == "quick" || mode == "QUICK") return ScanMode::Quick;
  if (mode == "deep" || mode == "DEEP") return ScanMode::Deep;
  if (mode == "combined" || mode == "COMBINED") return ScanMode::Combined;
  throw std::invalid_argument("ERR_BAD_PARAMS: Scan mode must be 'quick', 'deep', or 'combined'");
}

std::string compute_file_sha256_streaming(const std::filesystem::path& file_path) {
  std::ifstream ifs(file_path, std::ios::binary);
  if (!ifs) {
    throw std::runtime_error("Failed to open file for SHA-256 hashing: " + file_path.filename().string());
  }
  Sha256Hasher hasher;
  std::vector<uint8_t> buf(65536U);
  while (ifs) {
    ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    const std::streamsize got = ifs.gcount();
    if (got > 0) {
      hasher.update(buf.data(), static_cast<size_t>(got));
    }
  }
  return hasher.finalize_hex();
}

ForensicScanResult ForensicRecoveryEngine::scan(
    const std::filesystem::path& disk_path,
    ScanMode mode,
    const std::vector<std::string>& file_type_filters,
    bool enable_txt_heuristic) {
  const auto t0 = std::chrono::steady_clock::now();

  ForensicScanResult result{};
  result.disk_name = disk_path.filename().string();
  result.source_disk_sha256 = compute_file_sha256_streaming(disk_path);
  result.scan_mode = (mode == ScanMode::Quick) ? "quick" : (mode == ScanMode::Deep ? "deep" : "combined");
  result.scan_id = "scan_" + result.source_disk_sha256.substr(0, 12) + "_" +
                   std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch())
                                      .count());

  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  GfsFileSystem fs;
  fs.mount(disk, false);

  if (fs.status().dirty_journal_detected) {
    result.warnings.push_back(
        "Source disk has dirty metadata journal state; recovery ran in read-only mode without altering source image.");
  }

  std::vector<RecoveryCandidate> meta_cands;
  std::vector<RecoveryCandidate> carve_cands;

  if (mode == ScanMode::Quick || mode == ScanMode::Combined) {
    uint32_t blks = 0;
    uint64_t bytes = 0;
    meta_cands = run_metadata_recovery(disk, fs, blks, bytes, result.warnings);
    result.blocks_examined += blks;
    result.bytes_examined += bytes;
  }

  if (mode == ScanMode::Deep || mode == ScanMode::Combined) {
    uint32_t blks = 0;
    uint64_t bytes = 0;
    carve_cands = run_signature_carving(disk, fs, enable_txt_heuristic, blks, bytes, result.warnings);
    result.blocks_examined += blks;
    result.bytes_examined += bytes;
  }

  // Merge and deduplicate in Combined mode.
  std::vector<RecoveryCandidate> merged = std::move(meta_cands);
  if (mode == ScanMode::Combined) {
    for (auto& cc : carve_cands) {
      bool matched_meta = false;
      if (!cc.source_blocks.empty()) {
        for (auto& mc : merged) {
          if (!mc.source_blocks.empty() &&
              mc.source_blocks.front() == cc.source_blocks.front() &&
              mc.classification != RecoveryClassification::Overwritten &&
              mc.classification != RecoveryClassification::Unrecoverable) {
            matched_meta = true;
            mc.recovery_method = "combined";
            mc.evidence.push_back("Corroborated by signature carving (" + cc.file_type + ")");
            break;
          }
        }
      }
      if (!matched_meta) {
        merged.push_back(std::move(cc));
      }
    }
  } else if (mode == ScanMode::Deep) {
    merged = std::move(carve_cands);
  }

  // Apply file-type filter if provided.
  if (!file_type_filters.empty()) {
    std::unordered_set<std::string> allowed(file_type_filters.begin(), file_type_filters.end());
    merged.erase(
        std::remove_if(merged.begin(), merged.end(), [&](const RecoveryCandidate& c) {
          return !allowed.count(c.file_type);
        }),
        merged.end());
  }

  for (const auto& c : merged) {
    switch (c.classification) {
      case RecoveryClassification::FullyVerified:
        ++result.fully_verified_count;
        break;
      case RecoveryClassification::CompleteUnverified:
        ++result.complete_unverified_count;
        break;
      case RecoveryClassification::PartiallyRecoverable:
        ++result.partially_recoverable_count;
        break;
      case RecoveryClassification::Overwritten:
        ++result.overwritten_count;
        break;
      case RecoveryClassification::Unrecoverable:
        ++result.unrecoverable_count;
        break;
      case RecoveryClassification::Unknown:
        ++result.unknown_count;
        break;
    }
  }

  result.candidates = std::move(merged);
  result.total_candidates = static_cast<uint32_t>(result.candidates.size());

  const auto t1 = std::chrono::steady_clock::now();
  result.scan_duration_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  return result;
}

RecoveryManifest ForensicRecoveryEngine::extract_candidate(
    const std::filesystem::path& workspace,
    const std::filesystem::path& disk_path,
    const std::string& candidate_id,
    const std::string& expected_source_sha256,
    bool zero_fill_missing,
    bool overwrite_existing,
    bool enable_txt_heuristic) {
  const std::string current_sha = compute_file_sha256_streaming(disk_path);
  if (!expected_source_sha256.empty() && current_sha != expected_source_sha256) {
    throw std::runtime_error(
        "ERR_STALE_SCAN: Source disk image SHA-256 changed since scan; re-scan before extracting");
  }

  const ScanMode mode =
      (candidate_id.rfind("carve_", 0) == 0) ? ScanMode::Deep : ScanMode::Combined;
  const auto scan_res = scan(disk_path, mode, {}, enable_txt_heuristic);

  const RecoveryCandidate* target = nullptr;
  for (const auto& c : scan_res.candidates) {
    if (c.candidate_id == candidate_id) {
      target = &c;
      break;
    }
  }
  if (target == nullptr) {
    throw std::runtime_error("ERR_NOT_FOUND: Recovery candidate not found: " + candidate_id);
  }
  if (target->classification == RecoveryClassification::Overwritten ||
      target->classification == RecoveryClassification::Unrecoverable) {
    throw std::runtime_error(
        "ERR_UNRECOVERABLE: Candidate '" + candidate_id + "' is " + target->classification_str +
        " and has no trustworthy data to extract");
  }

  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  GfsFileSystem fs;
  fs.mount(disk, false);
  const uint32_t bs = fs.superblock().block_size;

  std::vector<uint8_t> recovered_bytes;
  std::vector<ByteRange> substituted_ranges;
  std::vector<uint8_t> blk_buf;

  if (target->recovery_method == "carving") {
    // Carved candidate spans contiguous bytes starting at source_byte_offsets[0]
    if (target->original_size_bytes > 0ULL && !target->source_byte_offsets.empty()) {
      disk.read_raw(target->source_byte_offsets.front(),
                    static_cast<size_t>(target->original_size_bytes),
                    recovered_bytes);
    }
  } else {
    // Metadata / Combined recovery: reconstruct logical blocks strictly from intact blocks.
    for (const auto& lbs : target->block_statuses) {
      const uint64_t logical_start = static_cast<uint64_t>(lbs.logical_index) * bs;
      const uint64_t logical_end =
          std::min<uint64_t>(target->original_size_bytes, logical_start + bs);
      const size_t slice_len = static_cast<size_t>(logical_end - logical_start);

      if (lbs.state == "intact" || lbs.state == "unknown") {
        disk.read_block(lbs.physical_block, blk_buf);
        recovered_bytes.insert(
            recovered_bytes.end(),
            blk_buf.begin(),
            blk_buf.begin() + static_cast<std::ptrdiff_t>(slice_len));
      } else if (zero_fill_missing) {
        recovered_bytes.insert(recovered_bytes.end(), slice_len, 0x00U);
        if (!substituted_ranges.empty() && substituted_ranges.back().end_byte == logical_start) {
          substituted_ranges.back().end_byte = logical_end;
        } else {
          substituted_ranges.push_back(ByteRange{logical_start, logical_end});
        }
      }
    }
  }

  const std::filesystem::path recovered_dir = workspace / "reports" / "recovered";
  std::filesystem::create_directories(recovered_dir);

  const std::string safe_name = sanitize_filename(target->filename);
  const std::string recovery_id =
      "rec_" + current_sha.substr(0, 8) + "_" +
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count());

  std::string artifact_filename = recovery_id + "_" + safe_name;
  std::filesystem::path artifact_path = resolve_workspace_path(recovered_dir, artifact_filename);
  if (std::filesystem::exists(artifact_path) && !overwrite_existing) {
    throw std::runtime_error("ERR_ALREADY_EXISTS: Target recovery artifact already exists");
  }

  {
    std::ofstream ofs(artifact_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
      throw std::runtime_error("Failed to write recovered artifact");
    }
    if (!recovered_bytes.empty()) {
      ofs.write(reinterpret_cast<const char*>(recovered_bytes.data()),
                static_cast<std::streamsize>(recovered_bytes.size()));
    }
  }

  RecoveryManifest manifest{};
  manifest.recovery_id = recovery_id;
  manifest.artifact_filename = artifact_filename;
  manifest.artifact_path = artifact_path.string();
  manifest.source_disk_name = disk_path.filename().string();
  manifest.source_disk_sha256 = current_sha;
  manifest.recovery_timestamp_utc = utc_now_iso8601();
  manifest.recovery_method = target->recovery_method;
  manifest.original_filename = target->filename;
  manifest.inode_id = target->inode_id;
  manifest.generation = target->generation;
  manifest.original_expected_size = target->original_size_bytes;
  manifest.recovered_size = recovered_bytes.size();

  uint64_t sub_bytes = 0ULL;
  for (const auto& r : substituted_ranges) {
    sub_bytes += (r.end_byte >= r.start_byte) ? (r.end_byte - r.start_byte) : 0ULL;
  }
  uint64_t miss_bytes = 0ULL;
  for (const auto& r : target->missing_ranges) {
    miss_bytes += (r.end_byte >= r.start_byte) ? (r.end_byte - r.start_byte) : 0ULL;
  }
  uint64_t ow_bytes = 0ULL;
  if (target->recovery_method != "carving") {
    for (const auto& lbs : target->block_statuses) {
      if (lbs.state == "overwritten") {
        const uint64_t logical_start = static_cast<uint64_t>(lbs.logical_index) * bs;
        const uint64_t logical_end =
            std::min<uint64_t>(target->original_size_bytes, logical_start + bs);
        if (logical_end > logical_start) {
          ow_bytes += (logical_end - logical_start);
        }
      }
    }
  }
  const uint64_t payload_extracted =
      (recovered_bytes.size() >= sub_bytes) ? (recovered_bytes.size() - sub_bytes) : 0ULL;
  manifest.substituted_bytes = sub_bytes;
  manifest.missing_bytes = miss_bytes;
  manifest.overwritten_bytes = ow_bytes;
  if (target->classification == RecoveryClassification::FullyVerified) {
    manifest.verified_bytes = payload_extracted;
    manifest.unknown_candidate_bytes = 0ULL;
  } else {
    manifest.verified_bytes = 0ULL;
    manifest.unknown_candidate_bytes = payload_extracted;
  }

  manifest.source_blocks = target->source_blocks;
  manifest.source_byte_offsets = target->source_byte_offsets;
  manifest.missing_ranges = target->missing_ranges;
  manifest.substituted_ranges = substituted_ranges;
  manifest.has_original_crc32 = target->has_expected_crc32;
  manifest.original_crc32 = target->expected_crc32;
  manifest.recovered_crc32 = recovered_bytes.empty() ? 0U : compute_crc32(recovered_bytes);
  manifest.recovered_sha256 = compute_sha256_hex(recovered_bytes);
  manifest.integrity_result = target->integrity_status;
  manifest.classification = target->classification_str;
  manifest.engine_version = kEngineVersion;
  manifest.warnings = target->warnings;
  if (!substituted_ranges.empty()) {
    manifest.warnings.push_back(
        "Missing byte ranges were explicitly zero-filled (0x00) as requested; see substituted_ranges.");
  }

  const std::string manifest_filename = recovery_id + ".manifest.json";
  const std::filesystem::path manifest_path = resolve_workspace_path(recovered_dir, manifest_filename);
  manifest.manifest_path = manifest_path.string();

  nlohmann::json missing_arr = nlohmann::json::array();
  for (const auto& r : manifest.missing_ranges) {
    missing_arr.push_back({{"start_byte", r.start_byte}, {"end_byte", r.end_byte}});
  }
  nlohmann::json sub_arr = nlohmann::json::array();
  for (const auto& r : manifest.substituted_ranges) {
    sub_arr.push_back({{"start_byte", r.start_byte}, {"end_byte", r.end_byte}});
  }

  const nlohmann::json m_json = {
      {"recovery_id", manifest.recovery_id},
      {"artifact_filename", manifest.artifact_filename},
      {"source_disk_name", manifest.source_disk_name},
      {"source_disk_sha256", manifest.source_disk_sha256},
      {"recovery_timestamp_utc", manifest.recovery_timestamp_utc},
      {"recovery_method", manifest.recovery_method},
      {"original_filename", manifest.original_filename},
      {"inode_id", manifest.inode_id},
      {"generation", manifest.generation},
      {"original_expected_size", manifest.original_expected_size},
      {"recovered_size", manifest.recovered_size},
      {"verified_bytes", manifest.verified_bytes},
      {"unknown_candidate_bytes", manifest.unknown_candidate_bytes},
      {"missing_bytes", manifest.missing_bytes},
      {"overwritten_bytes", manifest.overwritten_bytes},
      {"substituted_bytes", manifest.substituted_bytes},
      {"source_blocks", manifest.source_blocks},
      {"source_byte_offsets", manifest.source_byte_offsets},
      {"missing_ranges", missing_arr},
      {"substituted_ranges", sub_arr},
      {"has_original_crc32", manifest.has_original_crc32},
      {"original_crc32", manifest.original_crc32},
      {"recovered_crc32", manifest.recovered_crc32},
      {"recovered_sha256", manifest.recovered_sha256},
      {"integrity_result", manifest.integrity_result},
      {"classification", manifest.classification},
      {"engine_version", manifest.engine_version},
      {"warnings", manifest.warnings},
      {"provenance_note",
       "Educational evidence provenance record generated by GhostFS; not a legal forensic certification."},
  };

  std::ofstream mfs(manifest_path, std::ios::trunc);
  if (!mfs) {
    throw std::runtime_error("Failed to write recovery manifest JSON");
  }
  mfs << m_json.dump(2) << '\n';

  return manifest;
}

}  // namespace ghostfs
