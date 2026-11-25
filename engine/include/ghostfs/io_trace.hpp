#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "ghostfs/comparison.hpp"
#include "ghostfs/disk_scheduler.hpp"

namespace ghostfs {

struct WorkloadGenerationResult {
  std::string source_type;  // "generated" | "files" | "recovery" | "metadata"
  std::string disk_name;
  std::string source_disk_sha256;
  uint32_t min_cylinder = 0;
  uint32_t max_cylinder = 199;
  uint32_t blocks_per_cylinder = 1;
  std::vector<std::string> notes;
  std::vector<std::string> warnings;
  std::vector<IoRequest> requests;
};

struct AllocationSchedulingPolicyResult {
  std::string policy;
  double external_fragmentation_pct = 0.0;
  double internal_fragmentation_pct = 0.0;
  uint32_t fragmented_file_count = 0;
  uint32_t total_file_extents = 0;
  uint32_t request_count = 0;
  std::vector<IoRequest> requests;
  MultiAlgorithmComparisonResult scheduling;
};

struct AllocationSchedulingResult {
  uint32_t block_size = 4096;
  uint32_t total_blocks = 128;
  uint32_t blocks_per_cylinder = 1;
  std::vector<AllocationSchedulingPolicyResult> policies;
};

class IoTraceBuilder {
 public:
  static WorkloadGenerationResult generate_synthetic(
      const std::string& pattern,  // "sequential" | "random" | "clustered" | "mixed" | "hotspot" | "fragmented"
      uint32_t count,
      uint32_t min_cylinder,
      uint32_t max_cylinder,
      uint64_t seed);

  static WorkloadGenerationResult from_gfs_files(
      const std::filesystem::path& disk_path,
      const std::vector<std::string>& file_paths,
      bool include_metadata_blocks,
      uint32_t blocks_per_cylinder);

  static WorkloadGenerationResult from_recovery_candidate(
      const std::filesystem::path& disk_path,
      const std::string& candidate_id,
      bool enable_txt_heuristic,
      uint32_t blocks_per_cylinder);

  static WorkloadGenerationResult from_filesystem_metadata(
      const std::filesystem::path& disk_path,
      uint32_t blocks_per_cylinder);

  static AllocationSchedulingResult compare_allocation_and_scheduling(
      const std::filesystem::path& workspace,
      const std::filesystem::path& source_disk_path,
      uint32_t block_size,
      uint32_t total_blocks,
      const std::vector<WorkloadStep>& workload,
      uint32_t initial_head,
      SweepDirection direction,
      uint32_t blocks_per_cylinder);
};

}  // namespace ghostfs
