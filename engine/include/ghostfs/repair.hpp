#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "ghostfs/fsck.hpp"

namespace ghostfs {

struct RepairAction {
  std::string action_id;
  std::string issue_id;
  std::string action_type;
  std::string target_structure;
  std::vector<uint64_t> byte_offsets;
  std::string existing_value;
  std::string proposed_value;
  std::string justification;
  std::string preconditions;
  std::vector<std::string> dependencies;
  std::string expected_post_repair_state;
  std::string data_loss_implication;
  std::string repairability;
  bool selected_by_default = false;
};

struct RepairPlan {
  std::string plan_id;
  std::string disk_name;
  std::string disk_sha256_at_plan;
  std::string check_mode;
  FsckResult initial_check;
  std::vector<RepairAction> actions;
  std::vector<FsckIssue> unrepairable_or_ambiguous;
};

struct RepairApplyResult {
  std::string plan_id;
  bool dry_run = false;
  std::string pre_repair_sha256;
  std::string post_repair_sha256;
  std::string pre_repair_snapshot_filename;
  uint32_t issues_before = 0;
  uint32_t issues_resolved = 0;
  uint32_t issues_remaining = 0;
  uint32_t newly_detected_issues = 0;
  uint64_t bytes_changed = 0;
  std::vector<uint32_t> blocks_changed;
  std::vector<std::string> repaired_structures;
  std::vector<std::string> applied_action_ids;
  std::vector<std::string> skipped_action_ids;
  std::vector<std::string> reconnected_paths;
  FsckResult post_check;
};

struct BlockChangeSummary {
  uint32_t block_id = 0;
  uint64_t byte_offset = 0;
  uint32_t bytes_differing = 0;
  std::string sample_before_hex;
  std::string sample_after_hex;
};

struct ExperimentComparisonResult {
  std::string experiment_id;
  std::string mode;  // "original_vs_corrupted" | "corrupted_vs_repaired" | "original_vs_repaired"
  std::string source_label;
  std::string target_label;
  std::string source_sha256;
  std::string target_sha256;
  bool identical = false;
  uint64_t total_bytes_changed = 0;
  std::vector<uint32_t> changed_blocks;
  std::vector<BlockChangeSummary> block_diffs;
  FsckResult source_check;
  FsckResult target_check;
};

class RepairEngine {
 public:
  static RepairPlan generate_plan(
      const std::filesystem::path& disk_path,
      const std::string& check_mode = "full");

  static RepairApplyResult apply_plan(
      const std::filesystem::path& workspace,
      const std::string& experiment_id,
      const std::string& plan_id,
      const std::string& expected_disk_sha256,
      const std::vector<std::string>& selected_action_ids,
      bool dry_run,
      bool confirm);

  static ExperimentComparisonResult compare_experiment(
      const std::filesystem::path& workspace,
      const std::string& experiment_id,
      const std::string& compare_mode);
};

}  // namespace ghostfs
