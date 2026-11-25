#include "ghostfs/io_trace.hpp"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <vector>

#include "ghostfs/fragmentation.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/virtual_disk.hpp"

namespace ghostfs {
namespace {

uint32_t block_to_cylinder(uint32_t lba, uint32_t blocks_per_cylinder) {
  const uint32_t bpc = std::max<uint32_t>(1U, blocks_per_cylinder);
  return lba / bpc;
}

}  // namespace

WorkloadGenerationResult IoTraceBuilder::generate_synthetic(
    const std::string& pattern,
    uint32_t count,
    uint32_t min_cylinder,
    uint32_t max_cylinder,
    uint64_t seed) {
  if (min_cylinder > max_cylinder) {
    throw std::runtime_error("ERR_INVALID_GEOMETRY: min_cylinder must be <= max_cylinder");
  }
  if (count == 0 || count > 500U) {
    throw std::runtime_error("ERR_INVALID_ARGUMENT: count must be between 1 and 500");
  }

  WorkloadGenerationResult res{};
  res.source_type = "generated";
  res.min_cylinder = min_cylinder;
  res.max_cylinder = max_cylinder;
  res.notes.push_back("Synthetic workload pattern='" + pattern + "', seed=" + std::to_string(seed));

  const uint32_t span = max_cylinder - min_cylinder + 1U;
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<uint32_t> full_dist(min_cylinder, max_cylinder);

  for (uint32_t i = 0; i < count; ++i) {
    uint32_t cyl = min_cylinder;
    if (pattern == "sequential") {
      cyl = min_cylinder + (i * std::max<uint32_t>(1U, span / std::max<uint32_t>(1U, count))) % span;
    } else if (pattern == "random") {
      cyl = full_dist(rng);
    } else if (pattern == "clustered") {
      const uint32_t c1 = min_cylinder + span / 5U;
      const uint32_t c2 = min_cylinder + (span * 4U) / 5U;
      const uint32_t center = ((i % 2U) == 0U) ? c1 : c2;
      const int32_t jitter = static_cast<int32_t>(rng() % 11U) - 5;
      const int64_t raw = static_cast<int64_t>(center) + jitter;
      cyl = static_cast<uint32_t>(std::clamp<int64_t>(raw, min_cylinder, max_cylinder));
    } else if (pattern == "hotspot") {
      if ((i % 5U) != 0U) {
        const uint32_t hot = min_cylinder + span / 2U;
        const int32_t jitter = static_cast<int32_t>(rng() % 7U) - 3;
        cyl = static_cast<uint32_t>(std::clamp<int64_t>(static_cast<int64_t>(hot) + jitter, min_cylinder, max_cylinder));
      } else {
        cyl = full_dist(rng);
      }
    } else if (pattern == "mixed") {
      if ((i % 3U) == 0U) cyl = full_dist(rng);
      else cyl = min_cylinder + (i * 3U) % span;
    } else if (pattern == "fragmented") {
      // Alternating far-apart extents simulating fragmented file reads
      const uint32_t lo = min_cylinder + (i * 2U) % std::max<uint32_t>(1U, span / 3U);
      const uint32_t hi = max_cylinder - (i * 2U) % std::max<uint32_t>(1U, span / 3U);
      cyl = ((i % 2U) == 0U) ? lo : hi;
    } else {
      throw std::runtime_error("ERR_INVALID_ARGUMENT: Unknown workload pattern: " + pattern);
    }

    IoRequest req{};
    req.request_id = "req_" + std::to_string(i + 1U);
    req.original_index = i;
    req.cylinder = cyl;
    req.op_type = ((i % 4U) == 3U) ? "write" : "read";
    req.label = pattern + "#" + std::to_string(i + 1U);
    res.requests.push_back(std::move(req));
  }
  return res;
}

WorkloadGenerationResult IoTraceBuilder::from_gfs_files(
    const std::filesystem::path& disk_path,
    const std::vector<std::string>& file_paths,
    bool include_metadata_blocks,
    uint32_t blocks_per_cylinder) {
  if (!std::filesystem::exists(disk_path)) {
    throw std::runtime_error("ERR_NOT_FOUND: Disk image not found: " + disk_path.string());
  }
  const uint32_t bpc = std::max<uint32_t>(1U, blocks_per_cylinder);
  const std::string sha = compute_file_sha256_streaming(disk_path);

  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  GfsFileSystem fs;
  fs.mount(disk, false);
  const auto& sb = fs.superblock();

  WorkloadGenerationResult res{};
  res.source_type = "files";
  res.disk_name = disk_path.filename().string();
  res.source_disk_sha256 = sha;
  res.min_cylinder = 0;
  res.max_cylinder = std::max<uint32_t>(1U, block_to_cylinder(sb.total_blocks - 1U, bpc));
  res.blocks_per_cylinder = bpc;
  res.notes.push_back(
      "Simulated cylinder = floor(LBA / " + std::to_string(bpc) +
      "). GFS1 LBAs are virtual image offsets, not physical HDD cylinders.");

  std::vector<std::string> targets = file_paths;
  if (targets.empty()) {
    const auto frag = FragmentationAnalyzer::analyze(fs);
    for (const auto& f : frag.files) {
      targets.push_back(f.path);
    }
  }

  uint32_t idx = 0;
  for (const auto& p : targets) {
    const FileEntryInfo info = fs.stat_path(p);
    if (include_metadata_blocks) {
      const uint32_t inodes_per_blk = sb.block_size / static_cast<uint32_t>(kInodeBytes);
      const uint32_t inode_blk = sb.inode_table_start + (info.inode_id / inodes_per_blk);
      IoRequest ireq{};
      ireq.request_id = "req_" + std::to_string(idx + 1U);
      ireq.original_index = idx++;
      ireq.logical_block = inode_blk;
      ireq.has_logical_block = true;
      ireq.cylinder = block_to_cylinder(inode_blk, bpc);
      ireq.op_type = "read";
      ireq.label = p + " (inode #" + std::to_string(info.inode_id) + ")";
      res.requests.push_back(std::move(ireq));

      if (info.indirect_block != 0) {
        IoRequest ind{};
        ind.request_id = "req_" + std::to_string(idx + 1U);
        ind.original_index = idx++;
        ind.logical_block = info.indirect_block;
        ind.has_logical_block = true;
        ind.cylinder = block_to_cylinder(info.indirect_block, bpc);
        ind.op_type = "read";
        ind.label = p + " (indirect)";
        res.requests.push_back(std::move(ind));
      }
    }
    for (size_t k = 0; k < info.all_data_blocks.size(); ++k) {
      const uint32_t blk = info.all_data_blocks[k];
      IoRequest req{};
      req.request_id = "req_" + std::to_string(idx + 1U);
      req.original_index = idx++;
      req.logical_block = blk;
      req.has_logical_block = true;
      req.cylinder = block_to_cylinder(blk, bpc);
      req.op_type = "read";
      req.label = p + " [blk " + std::to_string(blk) + "]";
      res.requests.push_back(std::move(req));
    }
  }
  return res;
}

WorkloadGenerationResult IoTraceBuilder::from_recovery_candidate(
    const std::filesystem::path& disk_path,
    const std::string& candidate_id,
    bool enable_txt_heuristic,
    uint32_t blocks_per_cylinder) {
  const uint32_t bpc = std::max<uint32_t>(1U, blocks_per_cylinder);
  const auto scan_res = ForensicRecoveryEngine::scan(disk_path, ScanMode::Combined, {}, enable_txt_heuristic);
  const RecoveryCandidate* target = nullptr;
  for (const auto& c : scan_res.candidates) {
    if (c.candidate_id == candidate_id || candidate_id.empty()) {
      target = &c;
      break;
    }
  }
  if (!target) {
    throw std::runtime_error("ERR_NOT_FOUND: Recovery candidate not found: " + candidate_id);
  }

  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  GfsFileSystem fs;
  fs.mount(disk, false);
  const auto& sb = fs.superblock();

  WorkloadGenerationResult res{};
  res.source_type = "recovery";
  res.disk_name = disk_path.filename().string();
  res.source_disk_sha256 = scan_res.source_disk_sha256;
  res.min_cylinder = 0;
  res.max_cylinder = std::max<uint32_t>(1U, block_to_cylinder(sb.total_blocks - 1U, bpc));
  res.blocks_per_cylinder = bpc;

  if (!target->missing_ranges.empty()) {
    res.warnings.push_back(
        "Candidate has " + std::to_string(target->missing_ranges.size()) +
        " missing/overwritten byte range(s) excluded from simulated read workload.");
  }

  uint32_t idx = 0;
  if (!target->block_statuses.empty()) {
    for (const auto& bs : target->block_statuses) {
      if (bs.state == "overwritten" || bs.state == "missing_indirect") continue;
      if (bs.physical_block >= sb.total_blocks) continue;
      IoRequest req{};
      req.request_id = "req_" + std::to_string(idx + 1U);
      req.original_index = idx++;
      req.logical_block = bs.physical_block;
      req.has_logical_block = true;
      req.cylinder = block_to_cylinder(bs.physical_block, bpc);
      req.op_type = "read";
      req.label = target->filename + " L" + std::to_string(bs.logical_index) + " (" + bs.state + ")";
      res.requests.push_back(std::move(req));
    }
  } else {
    for (uint32_t blk : target->source_blocks) {
      if (blk >= sb.total_blocks) continue;
      IoRequest req{};
      req.request_id = "req_" + std::to_string(idx + 1U);
      req.original_index = idx++;
      req.logical_block = blk;
      req.has_logical_block = true;
      req.cylinder = block_to_cylinder(blk, bpc);
      req.op_type = "read";
      req.label = target->filename + " [blk " + std::to_string(blk) + "]";
      res.requests.push_back(std::move(req));
    }
  }
  return res;
}

WorkloadGenerationResult IoTraceBuilder::from_filesystem_metadata(
    const std::filesystem::path& disk_path,
    uint32_t blocks_per_cylinder) {
  const uint32_t bpc = std::max<uint32_t>(1U, blocks_per_cylinder);
  const std::string sha = compute_file_sha256_streaming(disk_path);

  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  GfsFileSystem fs;
  fs.mount(disk, false);
  const auto& sb = fs.superblock();

  WorkloadGenerationResult res{};
  res.source_type = "metadata";
  res.disk_name = disk_path.filename().string();
  res.source_disk_sha256 = sha;
  res.min_cylinder = 0;
  res.max_cylinder = std::max<uint32_t>(1U, block_to_cylinder(sb.total_blocks - 1U, bpc));
  res.blocks_per_cylinder = bpc;

  std::vector<std::pair<uint32_t, std::string>> regions = {
      {1U, "Primary Superblock"},
      {2U, "Backup Superblock"},
      {sb.inode_bitmap_start, "Inode Bitmap"},
      {sb.block_bitmap_start, "Block Bitmap"},
      {sb.inode_table_start, "Inode Table"},
      {sb.data_region_start, "Root Directory"},
      {sb.journal_start, "Metadata Journal"},
  };

  uint32_t idx = 0;
  for (const auto& [blk, lbl] : regions) {
    IoRequest req{};
    req.request_id = "req_" + std::to_string(idx + 1U);
    req.original_index = idx++;
    req.logical_block = blk;
    req.has_logical_block = true;
    req.cylinder = block_to_cylinder(blk, bpc);
    req.op_type = "read";
    req.label = lbl;
    res.requests.push_back(std::move(req));
  }
  return res;
}

AllocationSchedulingResult IoTraceBuilder::compare_allocation_and_scheduling(
    const std::filesystem::path& workspace,
    const std::filesystem::path& source_disk_path,
    uint32_t block_size,
    uint32_t total_blocks,
    const std::vector<WorkloadStep>& workload,
    uint32_t initial_head,
    SweepDirection direction,
    uint32_t blocks_per_cylinder) {
  const uint32_t bpc = std::max<uint32_t>(1U, blocks_per_cylinder);
  const auto cmp = AllocationComparator::run_comparison(
      workspace, source_disk_path, block_size, total_blocks, workload);

  AllocationSchedulingResult out{};
  out.block_size = cmp.block_size;
  out.total_blocks = cmp.total_blocks;
  out.blocks_per_cylinder = bpc;

  const uint32_t max_cyl = std::max<uint32_t>(1U, block_to_cylinder(cmp.total_blocks - 1U, bpc));
  const uint32_t clamped_head = std::min(initial_head, max_cyl);

  for (const auto& p : cmp.policies) {
    AllocationSchedulingPolicyResult row{};
    row.policy = p.policy;
    row.external_fragmentation_pct = p.final_report.external_fragmentation_pct;
    row.internal_fragmentation_pct = p.final_report.internal_fragmentation_pct;
    row.fragmented_file_count = p.final_report.fragmented_file_count;
    row.total_file_extents = 0;

    uint32_t idx = 0;
    for (const auto& f : p.final_report.files) {
      row.total_file_extents += f.extent_count;
      for (uint32_t blk : f.physical_data_blocks) {
        IoRequest req{};
        req.request_id = "req_" + std::to_string(idx + 1U);
        req.original_index = idx++;
        req.logical_block = blk;
        req.has_logical_block = true;
        req.cylinder = block_to_cylinder(blk, bpc);
        req.op_type = "read";
        req.label = f.path + " [blk " + std::to_string(blk) + "]";
        row.requests.push_back(std::move(req));
      }
    }
    row.request_count = static_cast<uint32_t>(row.requests.size());

    SchedulerConfig cfg{};
    cfg.min_cylinder = 0;
    cfg.max_cylinder = max_cyl;
    cfg.initial_head = clamped_head;
    cfg.initial_direction = direction;
    row.scheduling = DiskScheduler::compare_all(cfg, row.requests);
    out.policies.push_back(std::move(row));
  }

  return out;
}

}  // namespace ghostfs
