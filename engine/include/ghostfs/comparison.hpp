#pragma once

#include "ghostfs/fragmentation.hpp"
#include "ghostfs/gfs_format.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ghostfs {

struct WorkloadStep {
  std::string op;    // "create" | "delete"
  std::string path;  // e.g. "/file_a.bin"
  uint64_t size_bytes = 0;
};

struct WorkloadStepResult {
  std::string op;
  std::string path;
  uint64_t size_bytes = 0;
  bool success = false;
  std::string error;
  std::vector<uint32_t> blocks_allocated;
  uint32_t extent_count = 0;
  uint32_t blocks_inspected = 0;
  uint32_t extents_examined = 0;
  bool used_fragmented_fallback = false;
};

struct PolicyComparisonResult {
  std::string policy;
  uint32_t success_count = 0;
  uint32_t failure_count = 0;
  uint64_t total_blocks_inspected = 0;
  uint64_t total_extents_examined = 0;
  double measured_elapsed_ms = 0.0;
  FragmentationReport final_report;
  std::vector<WorkloadStepResult> steps;
};

struct ComparisonExperimentResult {
  std::string experiment_mode;  // "fresh_disk" | "cloned_active_disk"
  uint32_t block_size = 0;
  uint32_t total_blocks = 0;
  std::vector<PolicyComparisonResult> policies;
};

class AllocationComparator {
 public:
  /// Runs the identical workload across all four allocation policies on isolated disposable
  /// `.gfs` images inside `workspace/disk_images/sandbox/`. Never mutates `source_disk_path` if given.
  static ComparisonExperimentResult run_comparison(
      const std::filesystem::path& workspace,
      const std::filesystem::path& source_disk_path,
      uint32_t block_size,
      uint32_t total_blocks,
      const std::vector<WorkloadStep>& workload);
};

}  // namespace ghostfs
