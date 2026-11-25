#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ghostfs {

struct CorruptionRecord {
  std::string operation_id;
  std::string category;
  std::string scenario;
  std::string description;
  std::string original_disk_sha256;
  std::string before_sandbox_sha256;
  std::string after_sandbox_sha256;
  std::vector<uint64_t> affected_offsets;
  std::vector<uint32_t> affected_blocks;
  std::string original_bytes_hex;
  std::string replacement_bytes_hex;
  std::string timestamp_utc;
  uint64_t seed = 0;
  bool used_seed = false;
};

struct SandboxExperiment {
  std::string experiment_id;
  std::string source_disk_name;
  std::string sandbox_filename;
  std::string original_sha256;
  std::string initial_sandbox_sha256;
  std::string current_sandbox_sha256;
  std::string pre_repair_snapshot_filename;
  std::string pre_repair_sha256;
  std::string created_at_utc;
  std::string updated_at_utc;
  std::vector<CorruptionRecord> operations;
};

struct CorruptionInjectRequest {
  std::string category;
  std::string scenario;
  uint32_t target_block = 0;
  uint32_t target_inode = 0;
  std::string target_path;
  uint32_t secondary_inode = 0;
  uint32_t severity = 1;
  uint64_t seed = 42;
  bool confirm = false;
};

class CorruptionEngine {
 public:
  static std::filesystem::path sandbox_dir(const std::filesystem::path& workspace);

  static SandboxExperiment create_experiment(
      const std::filesystem::path& workspace,
      const std::string& source_disk_name,
      const std::string& sandbox_name = "");

  static std::vector<SandboxExperiment> list_experiments(
      const std::filesystem::path& workspace);

  static SandboxExperiment get_experiment(
      const std::filesystem::path& workspace,
      const std::string& experiment_id);

  static void delete_experiment(
      const std::filesystem::path& workspace,
      const std::string& experiment_id,
      bool confirm);

  static SandboxExperiment inject_corruption(
      const std::filesystem::path& workspace,
      const std::string& experiment_id,
      const CorruptionInjectRequest& req);

  static void save_experiment(
      const std::filesystem::path& workspace,
      const SandboxExperiment& exp);
};

}  // namespace ghostfs
