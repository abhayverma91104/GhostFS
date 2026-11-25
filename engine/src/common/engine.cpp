#include "ghostfs/base64.hpp"
#include "ghostfs/comparison.hpp"
#include "ghostfs/corruption.hpp"
#include "ghostfs/disk_scheduler.hpp"
#include "ghostfs/engine.hpp"
#include "ghostfs/fragmentation.hpp"
#include "ghostfs/fsck.hpp"
#include "ghostfs/gfs_format.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/hex_inspector.hpp"
#include "ghostfs/io_trace.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/repair.hpp"
#include "ghostfs/sha256.hpp"
#include "ghostfs/virtual_disk.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace ghostfs {

using json = nlohmann::json;

namespace {

json make_error(const std::string& command, const std::string& code, const std::string& message) {
  return json{
      {"ok", false},
      {"command", command},
      {"error",
       {
           {"code", code},
           {"message", message},
       }},
  };
}

std::string classify_error_code(const std::string& msg) {
  if (msg.rfind("ERR_", 0) == 0) {
    const auto colon = msg.find(':');
    if (colon != std::string::npos) {
      return msg.substr(0, colon);
    }
  }
  return "ERR_OPERATION_FAILED";
}

int64_t now_epoch_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::filesystem::path require_workspace(const json& req) {
  if (!req.contains("workspace") || !req["workspace"].is_string() ||
      req["workspace"].get<std::string>().empty()) {
    throw std::invalid_argument("ERR_MISSING_WORKSPACE: 'workspace' path is required");
  }
  const std::filesystem::path ws(req["workspace"].get<std::string>());
  if (!std::filesystem::exists(ws) || !std::filesystem::is_directory(ws)) {
    throw std::runtime_error("ERR_INVALID_WORKSPACE: Workspace directory does not exist");
  }
  return ws;
}

std::filesystem::path resolve_disk_path(const std::filesystem::path& workspace, const std::string& disk_name) {
  if (disk_name.empty() || disk_name.find('/') != std::string::npos ||
      disk_name.find('\\') != std::string::npos || disk_name == "." || disk_name == "..") {
    throw std::invalid_argument("ERR_INVALID_DISK_NAME: Disk name must be a single filename without path separators");
  }
  if (disk_name.size() < 5 || disk_name.substr(disk_name.size() - 4) != ".gfs") {
    throw std::invalid_argument("ERR_INVALID_DISK_NAME: Virtual disk filename must end with '.gfs'");
  }
  const std::filesystem::path disks_root = workspace / "disk_images";
  std::filesystem::create_directories(disks_root);
  return resolve_workspace_path(disks_root, disk_name);
}

json journal_to_json(const JournalEntry& j) {
  return json{
      {"seq_no", j.seq_no},
      {"timestamp_ms", j.timestamp_ms},
      {"op_type", j.op_type},
      {"status", j.status},
      {"target_inode", j.target_inode},
      {"target_block", j.target_block},
      {"aux_value", j.aux_value},
      {"summary", j.summary},
  };
}

json volume_status_to_json(const VolumeStatus& st) {
  json journal_arr = json::array();
  for (const auto& entry : st.recent_journal) {
    journal_arr.push_back(journal_to_json(entry));
  }
  return json{
      {"disk_name", st.disk_name},
      {"volume_label", st.volume_label},
      {"block_size", st.block_size},
      {"total_blocks", st.total_blocks},
      {"reserved_blocks", st.reserved_blocks},
      {"data_blocks_count", st.data_blocks_count},
      {"free_blocks", st.free_blocks},
      {"allocated_data_blocks", st.allocated_data_blocks},
      {"total_inodes", st.total_inodes},
      {"free_inodes", st.free_inodes},
      {"used_inodes", st.used_inodes},
      {"total_bytes", st.total_bytes},
      {"usable_data_bytes", st.usable_data_bytes},
      {"used_data_bytes", st.used_data_bytes},
      {"alloc_policy", st.alloc_policy},
      {"fs_state", st.fs_state},
      {"primary_sb_crc_ok", st.primary_sb_crc_ok},
      {"backup_sb_crc_ok", st.backup_sb_crc_ok},
      {"boot_crc_ok", st.boot_crc_ok},
      {"dirty_journal_detected", st.dirty_journal_detected},
      {"mount_count", st.mount_count},
      {"last_mount_ms", st.last_mount_ms},
      {"last_write_ms", st.last_write_ms},
      {"layout",
       {
           {"boot_block", 0},
           {"primary_superblock", 1},
           {"backup_superblock", 2},
           {"inode_bitmap_start", st.superblock.inode_bitmap_start},
           {"inode_bitmap_blocks", st.superblock.inode_bitmap_blocks},
           {"block_bitmap_start", st.superblock.block_bitmap_start},
           {"block_bitmap_blocks", st.superblock.block_bitmap_blocks},
           {"inode_table_start", st.superblock.inode_table_start},
           {"inode_table_blocks", st.superblock.inode_table_blocks},
           {"data_region_start", st.superblock.data_region_start},
           {"data_blocks_count", st.superblock.data_blocks_count},
           {"journal_start", st.superblock.journal_start},
           {"journal_blocks", st.superblock.journal_blocks},
       }},
      {"recent_journal", journal_arr},
  };
}

json file_info_to_json(const FileEntryInfo& info) {
  return json{
      {"inode_id", info.inode_id},
      {"name", info.name},
      {"path", info.path},
      {"type", info.type == static_cast<uint8_t>(InodeType::Directory) ? "directory" : "file"},
      {"permissions", info.permissions},
      {"size_bytes", info.size_bytes},
      {"created_at_ms", info.created_at_ms},
      {"modified_at_ms", info.modified_at_ms},
      {"content_crc32", info.content_crc32},
      {"generation", info.generation},
      {"direct_blocks", info.direct_blocks},
      {"indirect_block", info.indirect_block},
      {"double_indirect_block", info.double_indirect_block},
      {"all_data_blocks", info.all_data_blocks},
      {"all_indirect_blocks", info.all_indirect_blocks},
  };
}

json block_info_to_json(const BlockInfo& b) {
  return json{
      {"block_id", b.block_id},
      {"byte_offset", b.byte_offset},
      {"block_size", b.block_size},
      {"allocated", b.allocated},
      {"classification", b.classification},
      {"inode_id", b.inode_id},
      {"owner_path", b.owner_path},
      {"extent_start", b.extent_start},
      {"extent_length", b.extent_length},
  };
}

json fragmentation_to_json(const FragmentationReport& r) {
  json extents_arr = json::array();
  for (const auto& ext : r.free_extents) {
    extents_arr.push_back(json{{"start_block", ext.start_block}, {"length", ext.length}});
  }
  json files_arr = json::array();
  for (const auto& f : r.files) {
    files_arr.push_back(json{
        {"inode_id", f.inode_id},
        {"name", f.name},
        {"path", f.path},
        {"size_bytes", f.size_bytes},
        {"allocated_data_blocks", f.allocated_data_blocks},
        {"allocated_payload_capacity_bytes", f.allocated_payload_capacity_bytes},
        {"internal_fragmentation_bytes", f.internal_fragmentation_bytes},
        {"internal_fragmentation_pct", f.internal_fragmentation_pct},
        {"physical_data_blocks", f.physical_data_blocks},
        {"indirect_blocks", f.indirect_blocks},
        {"extent_count", f.extent_count},
        {"is_fragmented", f.is_fragmented},
    });
  }
  return json{
      {"disk_name", r.disk_name},
      {"alloc_policy", r.alloc_policy},
      {"block_size", r.block_size},
      {"total_blocks", r.total_blocks},
      {"reserved_metadata_blocks", r.reserved_metadata_blocks},
      {"journal_blocks", r.journal_blocks},
      {"data_blocks_count", r.data_blocks_count},
      {"allocated_data_blocks", r.allocated_data_blocks},
      {"file_data_blocks", r.file_data_blocks},
      {"directory_data_blocks", r.directory_data_blocks},
      {"indirect_pointer_blocks", r.indirect_pointer_blocks},
      {"free_blocks", r.free_blocks},
      {"free_deleted_residual_blocks", r.free_deleted_residual_blocks},
      {"inconsistent_blocks", r.inconsistent_blocks},
      {"free_extent_count", r.free_extent_count},
      {"largest_free_extent", r.largest_free_extent},
      {"smallest_free_extent", r.smallest_free_extent},
      {"average_free_extent", r.average_free_extent},
      {"fragmented_file_count", r.fragmented_file_count},
      {"contiguous_file_count", r.contiguous_file_count},
      {"total_files", r.total_files},
      {"total_allocated_payload_capacity_bytes", r.total_allocated_payload_capacity_bytes},
      {"total_logical_file_bytes", r.total_logical_file_bytes},
      {"internal_fragmentation_bytes", r.internal_fragmentation_bytes},
      {"internal_fragmentation_pct", r.internal_fragmentation_pct},
      {"external_fragmentation_pct", r.external_fragmentation_pct},
      {"disk_utilization_pct", r.disk_utilization_pct},
      {"data_utilization_pct", r.data_utilization_pct},
      {"free_extents", extents_arr},
      {"files", files_arr},
  };
}

json candidate_to_json(const RecoveryCandidate& c) {
  json bs_arr = json::array();
  for (const auto& b : c.block_statuses) {
    bs_arr.push_back(json{
        {"logical_index", b.logical_index},
        {"physical_block", b.physical_block},
        {"disk_byte_offset", b.disk_byte_offset},
        {"state", b.state},
        {"detail", b.detail},
    });
  }
  json rec_arr = json::array();
  for (const auto& r : c.recoverable_ranges) {
    rec_arr.push_back(json{{"start_byte", r.start_byte}, {"end_byte", r.end_byte}});
  }
  json miss_arr = json::array();
  for (const auto& r : c.missing_ranges) {
    miss_arr.push_back(json{{"start_byte", r.start_byte}, {"end_byte", r.end_byte}});
  }
  return json{
      {"candidate_id", c.candidate_id},
      {"recovery_method", c.recovery_method},
      {"file_type", c.file_type},
      {"filename", c.filename},
      {"original_path", c.original_path},
      {"filename_inferred", c.filename_inferred},
      {"inode_id", c.inode_id},
      {"generation", c.generation},
      {"original_size_bytes", c.original_size_bytes},
      {"recoverable_bytes", c.recoverable_bytes},
      {"recovery_pct", c.recovery_pct},
      {"created_at_ms", c.created_at_ms},
      {"modified_at_ms", c.modified_at_ms},
      {"deleted_at_ms", c.deleted_at_ms},
      {"expected_crc32", c.expected_crc32},
      {"has_expected_crc32", c.has_expected_crc32},
      {"reconstructed_crc32", c.reconstructed_crc32},
      {"crc32_matched", c.crc32_matched},
      {"integrity_status", c.integrity_status},
      {"classification", c.classification_str},
      {"source_blocks", c.source_blocks},
      {"source_byte_offsets", c.source_byte_offsets},
      {"block_statuses", bs_arr},
      {"recoverable_ranges", rec_arr},
      {"missing_ranges", miss_arr},
      {"evidence", c.evidence},
      {"warnings", c.warnings},
  };
}

json hex_read_to_json(const HexReadResult& hr) {
  json rows_arr = json::array();
  for (const auto& r : hr.rows) {
    rows_arr.push_back(json{
        {"abs_offset", r.abs_offset},
        {"block_id", r.block_id},
        {"block_offset", r.block_offset},
        {"logical_offset", r.logical_offset},
        {"raw_bytes", r.raw_bytes},
        {"hex_bytes", r.hex_bytes},
        {"ascii", r.ascii},
    });
  }
  return json{
      {"disk_name", hr.disk_name},
      {"source_type", hr.source_type},
      {"start_offset", hr.start_offset},
      {"length", hr.length},
      {"block_size", hr.block_size},
      {"bytes_per_row", hr.bytes_per_row},
      {"total_source_bytes", hr.total_source_bytes},
      {"start_block", hr.start_block},
      {"classification", hr.classification},
      {"inode_id", hr.inode_id},
      {"owner_path", hr.owner_path},
      {"signature",
       {
           {"detected", hr.signature.detected},
           {"file_type", hr.signature.file_type},
           {"signature_hex", hr.signature.signature_hex},
           {"offset", hr.signature.offset},
           {"length", hr.signature.length},
           {"structurally_valid", hr.signature.structurally_valid},
           {"validation_note", hr.signature.validation_note},
           {"warnings", hr.signature.warnings},
       }},
      {"rows", rows_arr},
  };
}

}  // namespace

std::filesystem::path resolve_workspace_path(
    const std::filesystem::path& workspace_root,
    const std::filesystem::path& relative_or_name) {
  if (workspace_root.empty() || relative_or_name.empty()) {
    throw std::invalid_argument("Workspace root and target path must be non-empty");
  }
  if (relative_or_name.is_absolute()) {
    throw std::invalid_argument("Absolute target paths are not allowed outside workspace");
  }

  const std::filesystem::path canon_root = std::filesystem::weakly_canonical(workspace_root);
  const std::filesystem::path canon_target =
      std::filesystem::weakly_canonical(canon_root / relative_or_name);

  auto root_it = canon_root.begin();
  auto target_it = canon_target.begin();
  for (; root_it != canon_root.end(); ++root_it, ++target_it) {
    if (target_it == canon_target.end() || *root_it != *target_it) {
      throw std::runtime_error("Path escapes workspace boundary");
    }
  }
  return canon_target;
}

std::string process_request_json(const std::string& request_json) {
  json req;
  try {
    req = json::parse(request_json);
  } catch (const json::parse_error& err) {
    return make_error("", "ERR_INVALID_JSON", err.what()).dump();
  }

  if (!req.is_object() || !req.contains("command") || !req["command"].is_string()) {
    return make_error("", "ERR_MISSING_COMMAND", "Request must be a JSON object with string 'command'").dump();
  }

  const std::string command = req["command"].get<std::string>();

  try {
    if (command == "ping" || command == "version") {
      bool workspace_exists = false;
      std::string workspace_path;
      if (req.contains("workspace") && req["workspace"].is_string()) {
        workspace_path = req["workspace"].get<std::string>();
        std::error_code ec;
        workspace_exists = std::filesystem::exists(workspace_path, ec) &&
                           std::filesystem::is_directory(workspace_path, ec);
      }

      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"engine", kEngineName},
               {"version", kEngineVersion},
               {"fs_format", kFileSystemFormat},
               {"cpp_standard", static_cast<long>(__cplusplus)},
               {"pid", static_cast<long>(::getpid())},
               {"timestamp_ms", now_epoch_ms()},
               {"workspace", workspace_path},
               {"workspace_ready", workspace_exists},
               {"modules", json::array({"disk", "filesystem", "allocator", "recovery", "forensic", "hexviewer", "scheduler"})},
           }},
      }.dump();
    }

    const bool is_disk_cmd =
        command == "disk_list" || command == "disk_create" || command == "disk_delete" ||
        command == "disk_format" || command == "disk_info" || command == "disk_set_policy" ||
        command == "fs_list" || command == "fs_stat" || command == "fs_mkdir" ||
        command == "fs_write" || command == "fs_read" || command == "fs_rename" ||
        command == "fs_delete" || command == "blocks_map" || command == "block_inspect" ||
        command == "fragmentation_report" || command == "alloc_compare" ||
        command == "recovery_scan" || command == "recovery_extract" ||
        command == "hex_read" || command == "hex_interpret" ||
        command == "hex_decode_structure" || command == "hex_search" ||
        command == "hex_analyze" || command == "hex_compare" ||
        command == "corruption_create_experiment" || command == "corruption_list_experiments" ||
        command == "corruption_get_experiment" || command == "corruption_delete_experiment" ||
        command == "corruption_inject" || command == "corruption_check" ||
        command == "corruption_repair_plan" || command == "corruption_repair" ||
        command == "corruption_compare" ||
        command == "scheduling_simulate" || command == "scheduling_compare" ||
        command == "scheduling_generate_workload" || command == "scheduling_file_workload" ||
        command == "scheduling_recovery_workload" || command == "scheduling_metadata_workload" ||
        command == "scheduling_alloc_compare";
    if (!is_disk_cmd) {
      return make_error(command, "ERR_UNKNOWN_COMMAND", "Unsupported engine command: " + command).dump();
    }

    const std::filesystem::path workspace = require_workspace(req);

    auto parse_io_requests = [](const json& arr) {
      std::vector<IoRequest> out;
      uint32_t idx = 0;
      for (const auto& item : arr) {
        IoRequest r{};
        if (item.is_number_unsigned() || item.is_number_integer()) {
          r.cylinder = item.get<uint32_t>();
          r.request_id = "req_" + std::to_string(idx + 1U);
          r.original_index = idx++;
        } else {
          r.request_id = item.value("request_id", "req_" + std::to_string(idx + 1U));
          r.original_index = idx++;
          r.cylinder = item.at("cylinder").get<uint32_t>();
          r.logical_block = item.value("logical_block", 0U);
          r.has_logical_block = item.value("has_logical_block", false);
          r.op_type = item.value("op_type", std::string("read"));
          r.arrival_time = item.value("arrival_time", 0ULL);
          r.label = item.value("label", std::string(""));
        }
        out.push_back(std::move(r));
      }
      return out;
    };

    auto serialize_io_requests = [](const std::vector<IoRequest>& reqs) {
      json arr = json::array();
      for (const auto& r : reqs) {
        arr.push_back({
            {"request_id", r.request_id},
            {"original_index", r.original_index},
            {"cylinder", r.cylinder},
            {"logical_block", r.logical_block},
            {"has_logical_block", r.has_logical_block},
            {"op_type", r.op_type},
            {"arrival_time", r.arrival_time},
            {"label", r.label},
        });
      }
      return arr;
    };

    auto serialize_sim_result = [&](const SimulationResult& s) {
      json steps_arr = json::array();
      for (const auto& st : s.steps) {
        steps_arr.push_back({
            {"step_index", st.step_index},
            {"from_cylinder", st.from_cylinder},
            {"to_cylinder", st.to_cylinder},
            {"movement", st.movement},
            {"direction", st.direction},
            {"step_type", st.step_type},
            {"is_service", st.is_service},
            {"request_id", st.request_id},
            {"original_index", st.original_index},
            {"logical_block", st.logical_block},
            {"op_type", st.op_type},
            {"cumulative_movement", st.cumulative_movement},
        });
      }
      return json{
          {"algorithm", s.algorithm},
          {"scheduling_mode", s.scheduling_mode},
          {"min_cylinder", s.min_cylinder},
          {"max_cylinder", s.max_cylinder},
          {"initial_head", s.initial_head},
          {"initial_direction", s.initial_direction},
          {"serviced_count", s.serviced_count},
          {"total_head_movement", s.total_head_movement},
          {"average_movement", s.average_movement},
          {"max_single_step_movement", s.max_single_step_movement},
          {"min_single_step_movement", s.min_single_step_movement},
          {"direction_changes", s.direction_changes},
          {"circular_repositioning_distance", s.circular_repositioning_distance},
          {"estimated_seek_time_ms", s.estimated_seek_time_ms},
          {"median_exec_duration_us", s.median_exec_duration_us},
          {"benchmark_repetitions", s.benchmark_repetitions},
          {"original_requests", serialize_io_requests(s.original_requests)},
          {"service_sequence", s.service_sequence},
          {"head_path", s.head_path},
          {"steps", steps_arr},
      };
    };

    auto serialize_multi_cmp = [&](const MultiAlgorithmComparisonResult& c) {
      json algs = json::array();
      for (const auto& row : c.algorithms) {
        algs.push_back({
            {"result", serialize_sim_result(row.result)},
            {"reduction_vs_fcfs_pct", row.reduction_vs_fcfs_pct},
            {"rank_by_movement", row.rank_by_movement},
        });
      }
      return json{
          {"min_cylinder", c.min_cylinder},
          {"max_cylinder", c.max_cylinder},
          {"initial_head", c.initial_head},
          {"initial_direction", c.initial_direction},
          {"request_count", c.request_count},
          {"fcfs_total_movement", c.fcfs_total_movement},
          {"algorithms", algs},
      };
    };

    if (command == "scheduling_simulate" || command == "scheduling_compare") {
      SchedulerConfig cfg{};
      cfg.min_cylinder = req.value("min_cylinder", 0U);
      cfg.max_cylinder = req.value("max_cylinder", 199U);
      cfg.initial_head = req.value("initial_head", 53U);
      cfg.initial_direction = parse_sweep_direction(req.value("initial_direction", std::string("right")));
      cfg.scheduling_mode = req.value("scheduling_mode", std::string("static"));
      cfg.seek_time_per_cylinder_ms = req.value("seek_time_per_cylinder_ms", 0.0);
      cfg.benchmark_repetitions = req.value("benchmark_repetitions", 1U);
      const auto reqs = parse_io_requests(req.at("requests"));

      if (command == "scheduling_simulate") {
        cfg.algorithm = parse_scheduling_algorithm(req.value("algorithm", std::string("FCFS")));
        const auto res = DiskScheduler::simulate(cfg, reqs);
        return json{{"ok", true}, {"command", command}, {"data", serialize_sim_result(res)}}.dump();
      } else {
        const auto cmp = DiskScheduler::compare_all(cfg, reqs);
        return json{{"ok", true}, {"command", command}, {"data", serialize_multi_cmp(cmp)}}.dump();
      }
    }

    if (command == "scheduling_generate_workload") {
      const std::string pattern = req.value("pattern", std::string("random"));
      const uint32_t count = req.value("count", 16U);
      const uint32_t min_cyl = req.value("min_cylinder", 0U);
      const uint32_t max_cyl = req.value("max_cylinder", 199U);
      const uint64_t seed = req.value("seed", 42ULL);
      const auto w = IoTraceBuilder::generate_synthetic(pattern, count, min_cyl, max_cyl, seed);
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"source_type", w.source_type},
               {"disk_name", w.disk_name},
               {"source_disk_sha256", w.source_disk_sha256},
               {"min_cylinder", w.min_cylinder},
               {"max_cylinder", w.max_cylinder},
               {"blocks_per_cylinder", w.blocks_per_cylinder},
               {"notes", w.notes},
               {"warnings", w.warnings},
               {"requests", serialize_io_requests(w.requests)},
           }},
      }.dump();
    }

    if (command == "scheduling_file_workload" ||
        command == "scheduling_recovery_workload" ||
        command == "scheduling_metadata_workload") {
      const std::string disk_name = req.at("disk_name").get<std::string>();
      const std::filesystem::path disk_path =
          resolve_workspace_path(workspace / "disk_images", disk_name);
      const uint32_t bpc = req.value("blocks_per_cylinder", 1U);
      WorkloadGenerationResult w{};
      if (command == "scheduling_file_workload") {
        const auto paths = req.value("file_paths", std::vector<std::string>{});
        const bool inc_meta = req.value("include_metadata_blocks", false);
        w = IoTraceBuilder::from_gfs_files(disk_path, paths, inc_meta, bpc);
      } else if (command == "scheduling_recovery_workload") {
        const std::string cid = req.value("candidate_id", std::string(""));
        const bool txt_h = req.value("enable_txt_heuristic", true);
        w = IoTraceBuilder::from_recovery_candidate(disk_path, cid, txt_h, bpc);
      } else {
        w = IoTraceBuilder::from_filesystem_metadata(disk_path, bpc);
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"source_type", w.source_type},
               {"disk_name", w.disk_name},
               {"source_disk_sha256", w.source_disk_sha256},
               {"min_cylinder", w.min_cylinder},
               {"max_cylinder", w.max_cylinder},
               {"blocks_per_cylinder", w.blocks_per_cylinder},
               {"notes", w.notes},
               {"warnings", w.warnings},
               {"requests", serialize_io_requests(w.requests)},
           }},
      }.dump();
    }

    if (command == "scheduling_alloc_compare") {
      const uint32_t block_size = req.value("block_size", kDefaultBlockSize);
      const uint32_t total_blocks = req.value("total_blocks", 128U);
      const uint32_t initial_head = req.value("initial_head", 0U);
      const SweepDirection dir = parse_sweep_direction(req.value("initial_direction", std::string("right")));
      const uint32_t bpc = req.value("blocks_per_cylinder", 1U);
      std::filesystem::path src_path;
      if (req.contains("disk_name") && req["disk_name"].is_string() &&
          !req["disk_name"].get<std::string>().empty()) {
        src_path = resolve_workspace_path(workspace / "disk_images", req["disk_name"].get<std::string>());
      }
      std::vector<WorkloadStep> steps;
      for (const auto& s : req.at("workload")) {
        WorkloadStep st{};
        st.op = s.at("op").get<std::string>();
        st.path = s.at("path").get<std::string>();
        st.size_bytes = s.value("size_bytes", 0ULL);
        steps.push_back(std::move(st));
      }
      const auto res = IoTraceBuilder::compare_allocation_and_scheduling(
          workspace, src_path, block_size, total_blocks, steps, initial_head, dir, bpc);
      json pols = json::array();
      for (const auto& p : res.policies) {
        pols.push_back({
            {"policy", p.policy},
            {"external_fragmentation_pct", p.external_fragmentation_pct},
            {"internal_fragmentation_pct", p.internal_fragmentation_pct},
            {"fragmented_file_count", p.fragmented_file_count},
            {"total_file_extents", p.total_file_extents},
            {"request_count", p.request_count},
            {"requests", serialize_io_requests(p.requests)},
            {"scheduling", serialize_multi_cmp(p.scheduling)},
        });
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"block_size", res.block_size},
               {"total_blocks", res.total_blocks},
               {"blocks_per_cylinder", res.blocks_per_cylinder},
               {"policies", pols},
           }},
      }.dump();
    }

    auto serialize_exp_json = [](const SandboxExperiment& e) {
      json ops = json::array();
      for (const auto& op : e.operations) {
        ops.push_back({
            {"operation_id", op.operation_id},
            {"category", op.category},
            {"scenario", op.scenario},
            {"description", op.description},
            {"original_disk_sha256", op.original_disk_sha256},
            {"before_sandbox_sha256", op.before_sandbox_sha256},
            {"after_sandbox_sha256", op.after_sandbox_sha256},
            {"affected_offsets", op.affected_offsets},
            {"affected_blocks", op.affected_blocks},
            {"original_bytes_hex", op.original_bytes_hex},
            {"replacement_bytes_hex", op.replacement_bytes_hex},
            {"timestamp_utc", op.timestamp_utc},
            {"seed", op.seed},
            {"used_seed", op.used_seed},
        });
      }
      return json{
          {"experiment_id", e.experiment_id},
          {"source_disk_name", e.source_disk_name},
          {"sandbox_filename", e.sandbox_filename},
          {"original_sha256", e.original_sha256},
          {"initial_sandbox_sha256", e.initial_sandbox_sha256},
          {"current_sandbox_sha256", e.current_sandbox_sha256},
          {"pre_repair_snapshot_filename", e.pre_repair_snapshot_filename},
          {"pre_repair_sha256", e.pre_repair_sha256},
          {"created_at_utc", e.created_at_utc},
          {"updated_at_utc", e.updated_at_utc},
          {"operations", ops},
      };
    };

    auto serialize_fsck_json = [](const FsckResult& f) {
      json iss_arr = json::array();
      for (const auto& iss : f.issues) {
        iss_arr.push_back({
            {"issue_id", iss.issue_id},
            {"issue_type", iss.issue_type},
            {"severity", iss.severity},
            {"description", iss.description},
            {"affected_structure", iss.affected_structure},
            {"affected_block", iss.affected_block},
            {"affected_inode", iss.affected_inode},
            {"byte_offset", iss.byte_offset},
            {"expected_value", iss.expected_value},
            {"observed_value", iss.observed_value},
            {"evidence", iss.evidence},
            {"repairability", iss.repairability},
            {"recommended_action", iss.recommended_action},
            {"data_loss_risk", iss.data_loss_risk},
        });
      }
      return json{
          {"disk_name", f.disk_name},
          {"disk_sha256", f.disk_sha256},
          {"check_mode", f.check_mode},
          {"geometry_valid", f.geometry_valid},
          {"primary_sb_valid", f.primary_sb_valid},
          {"backup_sb_valid", f.backup_sb_valid},
          {"dirty_journal", f.dirty_journal},
          {"consistency_status", f.consistency_status},
          {"info_count", f.info_count},
          {"warning_count", f.warning_count},
          {"error_count", f.error_count},
          {"critical_count", f.critical_count},
          {"total_issues", f.total_issues},
          {"preserved_files_count", f.preserved_files_count},
          {"damaged_files_count", f.damaged_files_count},
          {"damaged_file_paths", f.damaged_file_paths},
          {"issues", iss_arr},
      };
    };

    if (command == "corruption_create_experiment") {
      const std::string src_disk = req.at("source_disk_name").get<std::string>();
      const std::string sb_name = req.value("sandbox_name", std::string(""));
      const auto exp = CorruptionEngine::create_experiment(workspace, src_disk, sb_name);
      return json{{"ok", true}, {"command", command}, {"data", serialize_exp_json(exp)}}.dump();
    }

    if (command == "corruption_list_experiments") {
      const auto list = CorruptionEngine::list_experiments(workspace);
      json arr = json::array();
      for (const auto& e : list) arr.push_back(serialize_exp_json(e));
      return json{{"ok", true}, {"command", command}, {"data", {{"experiments", arr}}}}.dump();
    }

    if (command == "corruption_get_experiment") {
      const std::string exp_id = req.at("experiment_id").get<std::string>();
      const auto exp = CorruptionEngine::get_experiment(workspace, exp_id);
      return json{{"ok", true}, {"command", command}, {"data", serialize_exp_json(exp)}}.dump();
    }

    if (command == "corruption_delete_experiment") {
      const std::string exp_id = req.at("experiment_id").get<std::string>();
      const bool confirm = req.value("confirm", false);
      CorruptionEngine::delete_experiment(workspace, exp_id, confirm);
      return json{{"ok", true}, {"command", command}, {"data", {{"deleted", true}, {"experiment_id", exp_id}}}}.dump();
    }

    if (command == "corruption_inject") {
      const std::string exp_id = req.at("experiment_id").get<std::string>();
      CorruptionInjectRequest ireq{};
      ireq.category = req.at("category").get<std::string>();
      ireq.scenario = req.at("scenario").get<std::string>();
      ireq.target_block = req.value("target_block", 0U);
      ireq.target_inode = req.value("target_inode", 0U);
      ireq.secondary_inode = req.value("secondary_inode", 0U);
      ireq.target_path = req.value("target_path", std::string(""));
      ireq.severity = req.value("severity", 1U);
      ireq.seed = req.value("seed", 42ULL);
      ireq.confirm = req.value("confirm", false);
      const auto exp = CorruptionEngine::inject_corruption(workspace, exp_id, ireq);
      return json{{"ok", true}, {"command", command}, {"data", serialize_exp_json(exp)}}.dump();
    }

    if (command == "corruption_check") {
      const std::string exp_id = req.at("experiment_id").get<std::string>();
      const std::string mode = req.value("mode", std::string("full"));
      const auto exp = CorruptionEngine::get_experiment(workspace, exp_id);
      const auto sb_path = resolve_workspace_path(CorruptionEngine::sandbox_dir(workspace), exp.sandbox_filename);
      const auto f = GfsckEngine::check(sb_path, mode);
      return json{{"ok", true}, {"command", command}, {"data", serialize_fsck_json(f)}}.dump();
    }

    if (command == "corruption_repair_plan") {
      const std::string exp_id = req.at("experiment_id").get<std::string>();
      const std::string mode = req.value("mode", std::string("full"));
      const auto exp = CorruptionEngine::get_experiment(workspace, exp_id);
      const auto sb_path = resolve_workspace_path(CorruptionEngine::sandbox_dir(workspace), exp.sandbox_filename);
      const auto plan = RepairEngine::generate_plan(sb_path, mode);
      json acts = json::array();
      for (const auto& a : plan.actions) {
        acts.push_back({
            {"action_id", a.action_id},
            {"issue_id", a.issue_id},
            {"action_type", a.action_type},
            {"target_structure", a.target_structure},
            {"byte_offsets", a.byte_offsets},
            {"existing_value", a.existing_value},
            {"proposed_value", a.proposed_value},
            {"justification", a.justification},
            {"preconditions", a.preconditions},
            {"dependencies", a.dependencies},
            {"expected_post_repair_state", a.expected_post_repair_state},
            {"data_loss_implication", a.data_loss_implication},
            {"repairability", a.repairability},
            {"selected_by_default", a.selected_by_default},
        });
      }
      json unrep = json::array();
      for (const auto& u : plan.unrepairable_or_ambiguous) {
        unrep.push_back({
            {"issue_id", u.issue_id},
            {"issue_type", u.issue_type},
            {"severity", u.severity},
            {"description", u.description},
            {"repairability", u.repairability},
            {"recommended_action", u.recommended_action},
        });
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"plan_id", plan.plan_id},
               {"disk_name", plan.disk_name},
               {"disk_sha256_at_plan", plan.disk_sha256_at_plan},
               {"check_mode", plan.check_mode},
               {"initial_check", serialize_fsck_json(plan.initial_check)},
               {"actions", acts},
               {"unrepairable_or_ambiguous", unrep},
           }},
      }.dump();
    }

    if (command == "corruption_repair") {
      const std::string exp_id = req.at("experiment_id").get<std::string>();
      const std::string plan_id = req.value("plan_id", std::string(""));
      const std::string expected_sha = req.value("expected_disk_sha256", std::string(""));
      const std::vector<std::string> selected_ids =
          req.value("selected_action_ids", std::vector<std::string>{});
      const bool dry_run = req.value("dry_run", false);
      const bool confirm = req.value("confirm", false);
      const auto r = RepairEngine::apply_plan(
          workspace, exp_id, plan_id, expected_sha, selected_ids, dry_run, confirm);
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"plan_id", r.plan_id},
               {"dry_run", r.dry_run},
               {"pre_repair_sha256", r.pre_repair_sha256},
               {"post_repair_sha256", r.post_repair_sha256},
               {"pre_repair_snapshot_filename", r.pre_repair_snapshot_filename},
               {"issues_before", r.issues_before},
               {"issues_resolved", r.issues_resolved},
               {"issues_remaining", r.issues_remaining},
               {"newly_detected_issues", r.newly_detected_issues},
               {"bytes_changed", r.bytes_changed},
               {"blocks_changed", r.blocks_changed},
               {"repaired_structures", r.repaired_structures},
               {"applied_action_ids", r.applied_action_ids},
               {"skipped_action_ids", r.skipped_action_ids},
               {"reconnected_paths", r.reconnected_paths},
               {"post_check", serialize_fsck_json(r.post_check)},
           }},
      }.dump();
    }

    if (command == "corruption_compare") {
      const std::string exp_id = req.at("experiment_id").get<std::string>();
      const std::string mode = req.value("compare_mode", std::string("original_vs_corrupted"));
      const auto c = RepairEngine::compare_experiment(workspace, exp_id, mode);
      json bdiffs = json::array();
      for (const auto& bd : c.block_diffs) {
        bdiffs.push_back({
            {"block_id", bd.block_id},
            {"byte_offset", bd.byte_offset},
            {"bytes_differing", bd.bytes_differing},
            {"sample_before_hex", bd.sample_before_hex},
            {"sample_after_hex", bd.sample_after_hex},
        });
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"experiment_id", c.experiment_id},
               {"mode", c.mode},
               {"source_label", c.source_label},
               {"target_label", c.target_label},
               {"source_sha256", c.source_sha256},
               {"target_sha256", c.target_sha256},
               {"identical", c.identical},
               {"total_bytes_changed", c.total_bytes_changed},
               {"changed_blocks", c.changed_blocks},
               {"block_diffs", bdiffs},
               {"source_check", serialize_fsck_json(c.source_check)},
               {"target_check", serialize_fsck_json(c.target_check)},
           }},
      }.dump();
    }

    if (command == "hex_interpret") {
      std::vector<uint8_t> bytes;
      for (const auto& v : req.at("bytes")) {
        bytes.push_back(static_cast<uint8_t>(v.get<int>()));
      }
      const auto bi = HexInspector::interpret_bytes(bytes);
      json out{
          {"byte_count", bi.byte_count},
          {"hex", bi.hex},
          {"binary", bi.binary},
          {"uint8", bi.u8},
          {"int8", bi.i8},
          {"has_16", bi.has_16},
          {"has_32", bi.has_32},
          {"has_64", bi.has_64},
          {"ascii", bi.ascii},
          {"valid_utf8", bi.valid_utf8},
          {"utf8", bi.valid_utf8 ? bi.utf8 : ""},
      };
      if (bi.has_16) {
        out["uint16_le"] = bi.u16_le;
        out["uint16_be"] = bi.u16_be;
        out["int16_le"] = bi.i16_le;
        out["int16_be"] = bi.i16_be;
      }
      if (bi.has_32) {
        out["uint32_le"] = bi.u32_le;
        out["uint32_be"] = bi.u32_be;
        out["int32_le"] = bi.i32_le;
        out["int32_be"] = bi.i32_be;
        out["float32_le"] = (bi.f32_le_status == "finite") ? json(bi.f32_le) : json(bi.f32_le_status);
        out["float32_be"] = (bi.f32_be_status == "finite") ? json(bi.f32_be) : json(bi.f32_be_status);
        out["float32_le_status"] = bi.f32_le_status;
        out["float32_be_status"] = bi.f32_be_status;
      }
      if (bi.has_64) {
        out["uint64_le"] = bi.u64_le;
        out["uint64_be"] = bi.u64_be;
        out["int64_le"] = bi.i64_le;
        out["int64_be"] = bi.i64_be;
        out["float64_le"] = (bi.f64_le_status == "finite") ? json(bi.f64_le) : json(bi.f64_le_status);
        out["float64_be"] = (bi.f64_be_status == "finite") ? json(bi.f64_be) : json(bi.f64_be_status);
        out["float64_le_status"] = bi.f64_le_status;
        out["float64_be_status"] = bi.f64_be_status;
      }
      return json{{"ok", true}, {"command", command}, {"data", out}}.dump();
    }

    if (command == "alloc_compare") {
      const uint32_t block_size = req.value("block_size", kDefaultBlockSize);
      const uint32_t total_blocks = req.value("total_blocks", 128U);
      const std::string base_disk = req.value("disk_name", std::string(""));
      std::filesystem::path source_path;
      if (!base_disk.empty()) {
        source_path = resolve_disk_path(workspace, base_disk);
      }

      std::vector<WorkloadStep> workload;
      for (const auto& item : req.at("workload")) {
        WorkloadStep step{};
        step.op = item.at("op").get<std::string>();
        step.path = item.at("path").get<std::string>();
        step.size_bytes = item.value("size_bytes", 0ULL);
        workload.push_back(std::move(step));
      }

      const auto res = AllocationComparator::run_comparison(
          workspace, source_path, block_size, total_blocks, workload);
      json policies_arr = json::array();
      for (const auto& p : res.policies) {
        json steps_arr = json::array();
        for (const auto& s : p.steps) {
          steps_arr.push_back(json{
              {"op", s.op},
              {"path", s.path},
              {"size_bytes", s.size_bytes},
              {"success", s.success},
              {"error", s.error},
              {"blocks_allocated", s.blocks_allocated},
              {"extent_count", s.extent_count},
              {"blocks_inspected", s.blocks_inspected},
              {"extents_examined", s.extents_examined},
              {"used_fragmented_fallback", s.used_fragmented_fallback},
          });
        }
        policies_arr.push_back(json{
            {"policy", p.policy},
            {"success_count", p.success_count},
            {"failure_count", p.failure_count},
            {"total_blocks_inspected", p.total_blocks_inspected},
            {"total_extents_examined", p.total_extents_examined},
            {"measured_elapsed_ms", p.measured_elapsed_ms},
            {"final_report", fragmentation_to_json(p.final_report)},
            {"steps", steps_arr},
        });
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"experiment_mode", res.experiment_mode},
               {"block_size", res.block_size},
               {"total_blocks", res.total_blocks},
               {"policies", policies_arr},
           }},
      }.dump();
    }

    if (command == "disk_list") {
      const std::filesystem::path disks_root = workspace / "disk_images";
      std::filesystem::create_directories(disks_root);

      std::vector<std::filesystem::path> files;
      for (const auto& entry : std::filesystem::directory_iterator(disks_root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".gfs") {
          files.push_back(entry.path());
        }
      }
      std::sort(files.begin(), files.end());

      json items = json::array();
      for (const auto& p : files) {
        const std::string name = p.filename().string();
        const uint64_t file_bytes = std::filesystem::file_size(p);
        try {
          VirtualDisk disk;
          disk.open_image(p, 512U, false);
          GfsFileSystem fs;
          fs.mount(disk, false);
          json st = volume_status_to_json(fs.status());
          st["formatted"] = true;
          st["valid"] = true;
          items.push_back(st);
        } catch (const std::exception& err) {
          items.push_back(json{
              {"disk_name", name},
              {"total_bytes", file_bytes},
              {"formatted", false},
              {"valid", false},
              {"error", err.what()},
          });
        }
      }
      return json{{"ok", true}, {"command", command}, {"data", {{"disks", items}}}}.dump();
    }

    if (command == "disk_create") {
      const std::string disk_name = req.at("disk_name").get<std::string>();
      const uint32_t block_size = req.value("block_size", kDefaultBlockSize);
      const uint64_t total_bytes = req.at("total_bytes").get<uint64_t>();
      const std::string label = req.value("label", std::string("GhostFS"));
      const AllocPolicy policy = parse_alloc_policy(req.value("alloc_policy", std::string("first_fit")));
      const uint32_t inode_count = req.value("inode_count", 0U);
      const bool overwrite = req.value("overwrite", false);

      if (!is_supported_block_size(block_size)) {
        throw std::invalid_argument("ERR_INVALID_BLOCK_SIZE: Block size must be 512, 1024, 2048, or 4096");
      }
      if (total_bytes % block_size != 0) {
        throw std::invalid_argument("ERR_INVALID_SIZE: Disk size must be an exact multiple of block_size");
      }
      const uint64_t blocks64 = total_bytes / block_size;
      if (blocks64 < kMinDiskBlocks || blocks64 > 0xFFFFFFFFULL) {
        throw std::invalid_argument("ERR_INVALID_SIZE: Disk block count out of valid range");
      }

      const std::filesystem::path target = resolve_disk_path(workspace, disk_name);
      VirtualDisk::create_image(target, block_size, static_cast<uint32_t>(blocks64), overwrite);

      VirtualDisk disk;
      disk.open_image(target, block_size, true);
      GfsFileSystem fs;
      fs.format(disk, label, policy, inode_count);
      return json{{"ok", true}, {"command", command}, {"data", volume_status_to_json(fs.status())}}.dump();
    }

    const std::string disk_name = req.at("disk_name").get<std::string>();
    const std::filesystem::path disk_path = resolve_disk_path(workspace, disk_name);

    if (command == "disk_delete") {
      if (!std::filesystem::exists(disk_path)) {
        throw std::runtime_error("ERR_NOT_FOUND: Virtual disk does not exist");
      }
      std::filesystem::remove(disk_path);
      std::error_code ec;
      std::filesystem::remove(disk_path.string() + ".lock", ec);
      return json{{"ok", true}, {"command", command}, {"data", {{"deleted", disk_name}}}}.dump();
    }

    if (command == "disk_format") {
      const uint32_t block_size = req.value("block_size", kDefaultBlockSize);
      const std::string label = req.value("label", std::string("GhostFS"));
      const AllocPolicy policy = parse_alloc_policy(req.value("alloc_policy", std::string("first_fit")));
      const uint32_t inode_count = req.value("inode_count", 0U);

      VirtualDisk disk;
      disk.open_image(disk_path, block_size, true);
      GfsFileSystem fs;
      fs.format(disk, label, policy, inode_count);
      return json{{"ok", true}, {"command", command}, {"data", volume_status_to_json(fs.status())}}.dump();
    }

    if (command == "disk_info") {
      VirtualDisk disk;
      disk.open_image(disk_path, 512U, false);
      GfsFileSystem fs;
      fs.mount(disk, false);
      return json{{"ok", true}, {"command", command}, {"data", volume_status_to_json(fs.status())}}.dump();
    }

    if (command == "fs_list") {
      const std::string dir_path = req.value("path", std::string("/"));
      VirtualDisk disk;
      disk.open_image(disk_path, 512U, false);
      GfsFileSystem fs;
      fs.mount(disk, false);
      const auto entries = fs.list_directory(dir_path);
      json arr = json::array();
      for (const auto& e : entries) {
        arr.push_back(file_info_to_json(e));
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"path", dir_path},
               {"entries", arr},
               {"volume", volume_status_to_json(fs.status())},
           }},
      }.dump();
    }

    if (command == "fs_stat") {
      const std::string target_path = req.at("path").get<std::string>();
      VirtualDisk disk;
      disk.open_image(disk_path, 512U, false);
      GfsFileSystem fs;
      fs.mount(disk, false);
      const auto info = fs.stat_path(target_path);
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"entry", file_info_to_json(info)},
               {"volume", volume_status_to_json(fs.status())},
           }},
      }.dump();
    }

    if (command == "fs_mkdir") {
      const std::string target_path = req.at("path").get<std::string>();
      const uint16_t perms = static_cast<uint16_t>(req.value("permissions", 0755));
      VirtualDisk disk;
      disk.open_image(disk_path, 512U, true);
      GfsFileSystem fs;
      fs.mount(disk, true);
      const auto info = fs.mkdir(target_path, perms);
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"entry", file_info_to_json(info)},
               {"volume", volume_status_to_json(fs.status())},
           }},
      }.dump();
    }

    if (command == "fs_write") {
      const std::string target_path = req.at("path").get<std::string>();
      const std::string content_b64 = req.value("content_base64", std::string(""));
      const bool overwrite = req.value("overwrite", true);
      const uint16_t perms = static_cast<uint16_t>(req.value("permissions", 0644));
      const std::vector<uint8_t> bytes = base64_decode(content_b64);

      VirtualDisk disk;
      disk.open_image(disk_path, 512U, true);
      GfsFileSystem fs;
      fs.mount(disk, true);
      const auto info = fs.write_file(target_path, bytes, overwrite, perms);
      json entry_json = file_info_to_json(info);
      entry_json["sha256"] = compute_sha256_hex(bytes);
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"entry", entry_json},
               {"volume", volume_status_to_json(fs.status())},
           }},
      }.dump();
    }

    if (command == "fs_read") {
      const std::string target_path = req.at("path").get<std::string>();
      VirtualDisk disk;
      disk.open_image(disk_path, 512U, false);
      GfsFileSystem fs;
      fs.mount(disk, false);
      FileEntryInfo info{};
      const std::vector<uint8_t> bytes = fs.read_file(target_path, &info);
      json entry_json = file_info_to_json(info);
      entry_json["sha256"] = compute_sha256_hex(bytes);
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"entry", entry_json},
               {"content_base64", base64_encode(bytes)},
           }},
      }.dump();
    }

    if (command == "fs_rename") {
      const std::string old_path = req.at("old_path").get<std::string>();
      const std::string new_path = req.at("new_path").get<std::string>();
      VirtualDisk disk;
      disk.open_image(disk_path, 512U, true);
      GfsFileSystem fs;
      fs.mount(disk, true);
      const auto info = fs.rename_path(old_path, new_path);
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"entry", file_info_to_json(info)},
               {"volume", volume_status_to_json(fs.status())},
           }},
      }.dump();
    }

    if (command == "fs_delete") {
      const std::string target_path = req.at("path").get<std::string>();
      const bool recursive = req.value("recursive", false);
      VirtualDisk disk;
      disk.open_image(disk_path, 512U, true);
      GfsFileSystem fs;
      fs.mount(disk, true);
      const auto st = fs.stat_path(target_path);
      const auto res = (st.type == static_cast<uint8_t>(InodeType::Directory))
                           ? fs.delete_directory(target_path, recursive)
                           : fs.delete_file(target_path);
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"deleted_path", target_path},
               {"inode_id", res.inode_id},
               {"released_blocks_count", res.released_blocks_count},
               {"released_blocks", res.released_blocks},
               {"volume", volume_status_to_json(fs.status())},
           }},
      }.dump();
    }

    if (command == "disk_set_policy") {
      const AllocPolicy policy = parse_alloc_policy(req.at("alloc_policy").get<std::string>());
      VirtualDisk disk;
      disk.open_image(disk_path, 512U, true);
      GfsFileSystem fs;
      fs.mount(disk, true);
      const VolumeStatus st = fs.set_alloc_policy(policy);
      return json{{"ok", true}, {"command", command}, {"data", volume_status_to_json(st)}}.dump();
    }

    if (command == "blocks_map") {
      const uint32_t offset = req.value("offset", 0U);
      const uint32_t limit = req.value("limit", 256U);
      const std::string type_filter = req.value("type_filter", std::string(""));
      const uint32_t file_inode = req.value("file_inode", 0U);

      VirtualDisk disk;
      disk.open_image(disk_path, 512U, false);
      GfsFileSystem fs;
      fs.mount(disk, false);
      const auto page =
          FragmentationAnalyzer::query_block_map(fs, offset, limit, type_filter, file_inode);

      json arr = json::array();
      for (const auto& b : page.blocks) {
        arr.push_back(block_info_to_json(b));
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"total_blocks", page.total_blocks},
               {"filtered_total", page.filtered_total},
               {"offset", page.offset},
               {"limit", page.limit},
               {"block_size", page.block_size},
               {"blocks", arr},
           }},
      }.dump();
    }

    if (command == "block_inspect") {
      const uint32_t block_id = req.at("block_id").get<uint32_t>();
      VirtualDisk disk;
      disk.open_image(disk_path, 512U, false);
      GfsFileSystem fs;
      fs.mount(disk, false);
      const auto info = FragmentationAnalyzer::inspect_block(fs, block_id);
      return json{{"ok", true}, {"command", command}, {"data", block_info_to_json(info)}}.dump();
    }

    if (command == "fragmentation_report") {
      VirtualDisk disk;
      disk.open_image(disk_path, 512U, false);
      GfsFileSystem fs;
      fs.mount(disk, false);
      const auto report = FragmentationAnalyzer::analyze(fs);
      return json{{"ok", true}, {"command", command}, {"data", fragmentation_to_json(report)}}.dump();
    }

    if (command == "recovery_scan") {
      const ScanMode mode = parse_scan_mode(req.value("mode", std::string("combined")));
      const bool enable_txt = req.value("enable_txt_heuristic", true);
      std::vector<std::string> type_filters;
      if (req.contains("file_types") && req["file_types"].is_array()) {
        for (const auto& t : req["file_types"]) {
          if (t.is_string()) type_filters.push_back(t.get<std::string>());
        }
      }

      const auto res = ForensicRecoveryEngine::scan(disk_path, mode, type_filters, enable_txt);
      json cand_arr = json::array();
      for (const auto& c : res.candidates) {
        cand_arr.push_back(candidate_to_json(c));
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"scan_id", res.scan_id},
               {"disk_name", res.disk_name},
               {"source_disk_sha256", res.source_disk_sha256},
               {"scan_mode", res.scan_mode},
               {"scan_duration_ms", res.scan_duration_ms},
               {"blocks_examined", res.blocks_examined},
               {"bytes_examined", res.bytes_examined},
               {"total_candidates", res.total_candidates},
               {"summary",
                {
                    {"fully_verified", res.fully_verified_count},
                    {"complete_unverified", res.complete_unverified_count},
                    {"partially_recoverable", res.partially_recoverable_count},
                    {"overwritten", res.overwritten_count},
                    {"unrecoverable", res.unrecoverable_count},
                    {"unknown", res.unknown_count},
                }},
               {"warnings", res.warnings},
               {"candidates", cand_arr},
           }},
      }.dump();
    }

    if (command == "recovery_extract") {
      const std::string candidate_id = req.at("candidate_id").get<std::string>();
      const std::string expected_sha = req.value("expected_source_sha256", std::string(""));
      const bool zero_fill = req.value("zero_fill_missing", false);
      const bool overwrite = req.value("overwrite_existing", false);
      const bool enable_txt = req.value("enable_txt_heuristic", true);

      const auto m = ForensicRecoveryEngine::extract_candidate(
          workspace, disk_path, candidate_id, expected_sha, zero_fill, overwrite, enable_txt);

      json missing_arr = json::array();
      for (const auto& r : m.missing_ranges) {
        missing_arr.push_back({{"start_byte", r.start_byte}, {"end_byte", r.end_byte}});
      }
      json sub_arr = json::array();
      for (const auto& r : m.substituted_ranges) {
        sub_arr.push_back({{"start_byte", r.start_byte}, {"end_byte", r.end_byte}});
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"recovery_id", m.recovery_id},
               {"artifact_filename", m.artifact_filename},
               {"artifact_path", m.artifact_path},
               {"manifest_path", m.manifest_path},
               {"source_disk_name", m.source_disk_name},
               {"source_disk_sha256", m.source_disk_sha256},
               {"recovery_timestamp_utc", m.recovery_timestamp_utc},
               {"recovery_method", m.recovery_method},
               {"original_filename", m.original_filename},
               {"inode_id", m.inode_id},
               {"generation", m.generation},
               {"original_expected_size", m.original_expected_size},
               {"recovered_size", m.recovered_size},
               {"verified_bytes", m.verified_bytes},
               {"unknown_candidate_bytes", m.unknown_candidate_bytes},
               {"missing_bytes", m.missing_bytes},
               {"overwritten_bytes", m.overwritten_bytes},
               {"substituted_bytes", m.substituted_bytes},
               {"source_blocks", m.source_blocks},
               {"source_byte_offsets", m.source_byte_offsets},
               {"missing_ranges", missing_arr},
               {"substituted_ranges", sub_arr},
               {"has_original_crc32", m.has_original_crc32},
               {"original_crc32", m.original_crc32},
               {"recovered_crc32", m.recovered_crc32},
               {"recovered_sha256", m.recovered_sha256},
               {"integrity_result", m.integrity_result},
               {"classification", m.classification},
               {"engine_version", m.engine_version},
               {"warnings", m.warnings},
           }},
      }.dump();
    }

    if (command == "hex_read") {
      const std::string source_type = req.value("source_type", std::string("block"));
      const uint32_t bpr = req.value("bytes_per_row", 16U);
      HexReadResult hr{};
      if (source_type == "disk_range") {
        hr = HexInspector::read_disk_range(
            disk_path, req.at("offset").get<uint64_t>(), req.at("length").get<uint32_t>(), bpr);
      } else if (source_type == "block") {
        hr = HexInspector::read_block_range(
            disk_path, req.at("block_id").get<uint32_t>(), req.value("block_count", 1U), bpr);
      } else if (source_type == "file") {
        hr = HexInspector::read_logical_file(
            disk_path, req.at("path").get<std::string>(), req.value("offset", 0ULL),
            req.value("length", 4096U), bpr);
      } else if (source_type == "artifact") {
        const std::filesystem::path rec_dir = workspace / "reports" / "recovered";
        const std::filesystem::path art_path =
            resolve_workspace_path(rec_dir, req.at("artifact_filename").get<std::string>());
        hr = HexInspector::read_artifact_range(
            art_path, req.value("offset", 0ULL), req.value("length", 4096U), bpr);
      } else {
        throw std::invalid_argument("ERR_BAD_PARAMS: Unknown source_type: " + source_type);
      }
      return json{{"ok", true}, {"command", command}, {"data", hex_read_to_json(hr)}}.dump();
    }

    if (command == "hex_decode_structure") {
      const uint32_t block_id = req.at("block_id").get<uint32_t>();
      const std::string force_type = req.value("force_type", std::string(""));
      const auto ds = HexInspector::decode_block_structure(disk_path, block_id, force_type);
      json fields_arr = json::array();
      for (const auto& f : ds.fields) {
        fields_arr.push_back(json{
            {"name", f.name},
            {"abs_offset", f.abs_offset},
            {"rel_offset", f.rel_offset},
            {"length", f.length},
            {"raw_hex", f.raw_hex},
            {"value", f.value},
            {"data_type", f.data_type},
            {"description", f.description},
            {"has_validation", f.has_validation},
            {"valid", f.valid},
            {"validation_note", f.validation_note},
        });
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"structure_type", ds.structure_type},
               {"block_id", ds.block_id},
               {"base_offset", ds.base_offset},
               {"checksum_present", ds.checksum_present},
               {"checksum_valid", ds.checksum_valid},
               {"warnings", ds.warnings},
               {"fields", fields_arr},
           }},
      }.dump();
    }

    if (command == "hex_search") {
      const auto sr = HexInspector::search(
          disk_path,
          req.at("query_type").get<std::string>(),
          req.at("query_value").get<std::string>(),
          req.value("scope", std::string("disk")),
          req.value("start_block", 0U),
          req.value("end_block", 0U),
          req.value("path", std::string("")),
          req.value("offset", 0U),
          req.value("limit", 50U));
      json matches_arr = json::array();
      for (const auto& m : sr.matches) {
        matches_arr.push_back(json{
            {"abs_offset", m.abs_offset},
            {"block_id", m.block_id},
            {"block_offset", m.block_offset},
            {"logical_offset", m.logical_offset},
            {"match_length", m.match_length},
            {"matched_hex", m.matched_hex},
            {"context_ascii", m.context_ascii},
        });
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"scope", sr.scope},
               {"query_type", sr.query_type},
               {"source_disk_sha256", sr.source_disk_sha256},
               {"total_matches", sr.total_matches},
               {"truncated", sr.truncated},
               {"offset", sr.offset},
               {"limit", sr.limit},
               {"bytes_scanned", sr.bytes_scanned},
               {"matches", matches_arr},
           }},
      }.dump();
    }

    if (command == "hex_analyze") {
      const uint64_t off = req.at("offset").get<uint64_t>();
      const uint32_t len = req.at("length").get<uint32_t>();
      const auto m = HexInspector::analyze_disk_range(disk_path, off, len);
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"total_bytes", m.total_bytes},
               {"zero_bytes", m.zero_bytes},
               {"nonzero_bytes", m.nonzero_bytes},
               {"zero_pct", m.zero_pct},
               {"printable_ascii_bytes", m.printable_ascii_bytes},
               {"printable_ascii_pct", m.printable_ascii_pct},
               {"unique_byte_values", m.unique_byte_values},
               {"shannon_entropy", m.shannon_entropy},
               {"crc32", m.crc32},
               {"sha256", m.sha256},
               {"histogram", m.histogram},
           }},
      }.dump();
    }

    if (command == "hex_compare") {
      const std::string mode = req.at("mode").get<std::string>();
      std::filesystem::path art_path;
      if (req.contains("artifact_filename") && req["artifact_filename"].is_string() &&
          !req["artifact_filename"].get<std::string>().empty()) {
        art_path = resolve_workspace_path(
            workspace / "reports" / "recovered",
            req["artifact_filename"].get<std::string>());
      }
      const auto cr = HexInspector::compare_disk_sources(
          disk_path,
          mode,
          req.value("a_start", 0ULL),
          req.value("a_len", 0ULL),
          req.value("b_start", 0ULL),
          req.value("b_len", 0ULL),
          req.value("path_a", std::string("")),
          req.value("path_b", std::string("")),
          art_path);

      json diffs_arr = json::array();
      for (const auto& d : cr.diffs) {
        diffs_arr.push_back(json{
            {"offset", d.offset},
            {"has_a", d.has_a},
            {"has_b", d.has_b},
            {"byte_a", d.byte_a},
            {"byte_b", d.byte_b},
        });
      }
      json sb_diffs_arr = json::array();
      for (const auto& s : cr.sb_field_diffs) {
        sb_diffs_arr.push_back(json{
            {"field_name", s.field_name},
            {"primary_value", s.primary_value},
            {"backup_value", s.backup_value},
            {"differs", s.differs},
            {"expected_difference", s.expected_difference},
        });
      }
      return json{
          {"ok", true},
          {"command", command},
          {"data",
           {
               {"mode", cr.mode},
               {"len_a", cr.len_a},
               {"len_b", cr.len_b},
               {"total_compared_bytes", cr.total_compared_bytes},
               {"matching_bytes", cr.matching_bytes},
               {"differing_bytes", cr.differing_bytes},
               {"missing_bytes", cr.missing_bytes},
               {"has_first_diff", cr.has_first_diff},
               {"first_diff_offset", cr.first_diff_offset},
               {"matching_pct", cr.matching_pct},
               {"sha256_a", cr.sha256_a},
               {"sha256_b", cr.sha256_b},
               {"diffs", diffs_arr},
               {"sb_field_diffs", sb_diffs_arr},
           }},
      }.dump();
    }

    return make_error(command, "ERR_UNKNOWN_COMMAND", "Unsupported engine command: " + command).dump();
  } catch (const json::exception& err) {
    return make_error(command, "ERR_BAD_PARAMS", err.what()).dump();
  } catch (const std::exception& err) {
    return make_error(command, classify_error_code(err.what()), err.what()).dump();
  }
}

}  // namespace ghostfs
