#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ghostfs {

enum class FsckSeverity {
  Information,
  Warning,
  Error,
  Critical,
};

enum class FsckRepairability {
  AutomaticallyRepairable,
  RepairableWithConfirmation,
  AmbiguousManualRequired,
  NotRepairable,
};

struct FsckIssue {
  std::string issue_id;
  std::string issue_type;
  std::string severity;       // "information" | "warning" | "error" | "critical"
  std::string description;
  std::string affected_structure;
  uint32_t affected_block = 0;
  uint32_t affected_inode = 0;
  uint64_t byte_offset = 0;
  std::string expected_value;
  std::string observed_value;
  std::string evidence;
  std::string repairability;  // "automatically_repairable" | "repairable_with_confirmation" | "ambiguous_manual_required" | "not_repairable"
  std::string recommended_action;
  std::string data_loss_risk;
};

struct FsckResult {
  std::string disk_name;
  std::string disk_sha256;
  std::string check_mode;  // "fast" | "full"
  bool geometry_valid = false;
  bool primary_sb_valid = false;
  bool backup_sb_valid = false;
  bool dirty_journal = false;
  std::string consistency_status;  // "clean" | "inconsistent" | "unrecoverable_geometry" | "payload_damaged"
  uint32_t info_count = 0;
  uint32_t warning_count = 0;
  uint32_t error_count = 0;
  uint32_t critical_count = 0;
  uint32_t total_issues = 0;
  uint32_t preserved_files_count = 0;
  uint32_t damaged_files_count = 0;
  std::vector<std::string> damaged_file_paths;
  std::vector<FsckIssue> issues;
};

class GfsckEngine {
 public:
  static FsckResult check(
      const std::filesystem::path& disk_path,
      const std::string& mode = "full");
};

}  // namespace ghostfs
