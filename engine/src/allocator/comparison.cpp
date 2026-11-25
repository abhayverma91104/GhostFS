#include "ghostfs/comparison.hpp"

#include "ghostfs/virtual_disk.hpp"

#include <chrono>
#include <stdexcept>

namespace ghostfs {

namespace {

uint32_t count_extents(const std::vector<uint32_t>& blocks) {
  if (blocks.empty()) return 0U;
  uint32_t count = 1U;
  for (size_t i = 1; i < blocks.size(); ++i) {
    if (blocks[i] != blocks[i - 1] + 1U) ++count;
  }
  return count;
}

}  // namespace

ComparisonExperimentResult AllocationComparator::run_comparison(
    const std::filesystem::path& workspace,
    const std::filesystem::path& source_disk_path,
    uint32_t block_size,
    uint32_t total_blocks,
    const std::vector<WorkloadStep>& workload) {
  if (workload.empty()) {
    throw std::invalid_argument("ERR_BAD_PARAMS: Workload must contain at least one step");
  }

  const std::filesystem::path sandbox_dir = workspace / "disk_images" / "sandbox";
  std::filesystem::create_directories(sandbox_dir);

  const bool clone_mode = !source_disk_path.empty();
  uint32_t effective_bs = block_size;
  uint32_t effective_total_blocks = total_blocks;

  if (clone_mode) {
    VirtualDisk probe;
    probe.open_image(source_disk_path, 512U, false);
    GfsFileSystem probe_fs;
    probe_fs.mount(probe, false);
    effective_bs = probe_fs.superblock().block_size;
    effective_total_blocks = probe_fs.superblock().total_blocks;
  }

  ComparisonExperimentResult experiment{};
  experiment.experiment_mode = clone_mode ? "cloned_active_disk" : "fresh_disk";
  experiment.block_size = effective_bs;
  experiment.total_blocks = effective_total_blocks;

  const auto run_id = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::vector<AllocPolicy> policies = {
      AllocPolicy::FirstFit,
      AllocPolicy::BestFit,
      AllocPolicy::WorstFit,
      AllocPolicy::BitmapNextFit,
  };

  for (AllocPolicy policy : policies) {
    const std::string pname = alloc_policy_to_string(policy);
    const std::filesystem::path temp_img =
        sandbox_dir / ("cmp_" + pname + "_" + std::to_string(run_id) + ".gfs");
    const std::filesystem::path temp_lock = temp_img.string() + ".lock";

    auto cleanup = [&]() {
      std::error_code ec;
      std::filesystem::remove(temp_img, ec);
      std::filesystem::remove(temp_lock, ec);
    };

    try {
      if (clone_mode) {
        std::filesystem::copy_file(
            source_disk_path,
            temp_img,
            std::filesystem::copy_options::overwrite_existing);
      } else {
        VirtualDisk::create_image(temp_img, effective_bs, effective_total_blocks, true);
      }

      VirtualDisk disk;
      disk.open_image(temp_img, effective_bs, true);
      GfsFileSystem fs;
      if (clone_mode) {
        fs.mount(disk, true);
        fs.set_alloc_policy(policy);
      } else {
        fs.format(disk, "CmpLab", policy);
      }

      PolicyComparisonResult p_res{};
      p_res.policy = pname;

      const auto t0 = std::chrono::steady_clock::now();
      for (const auto& step : workload) {
        WorkloadStepResult sr{};
        sr.op = step.op;
        sr.path = step.path;
        sr.size_bytes = step.size_bytes;
        try {
          if (step.op == "create") {
            std::vector<uint8_t> payload(static_cast<size_t>(step.size_bytes), 0x42U);
            const auto info = fs.write_file(step.path, payload, true);
            const auto& trace = fs.last_alloc_trace();
            sr.success = true;
            sr.blocks_allocated = info.all_data_blocks;
            sr.extent_count = count_extents(info.all_data_blocks);
            sr.blocks_inspected = trace.blocks_inspected;
            sr.extents_examined = trace.extents_examined;
            sr.used_fragmented_fallback = trace.used_fragmented_fallback;
            p_res.total_blocks_inspected += trace.blocks_inspected;
            p_res.total_extents_examined += trace.extents_examined;
            ++p_res.success_count;
          } else if (step.op == "delete") {
            fs.delete_file(step.path);
            sr.success = true;
            ++p_res.success_count;
          } else {
            throw std::invalid_argument("Unsupported workload op: " + step.op);
          }
        } catch (const std::exception& err) {
          sr.success = false;
          sr.error = err.what();
          ++p_res.failure_count;
        }
        p_res.steps.push_back(std::move(sr));
      }
      const auto t1 = std::chrono::steady_clock::now();
      p_res.measured_elapsed_ms =
          std::chrono::duration<double, std::milli>(t1 - t0).count();
      p_res.final_report = FragmentationAnalyzer::analyze(fs);
      p_res.final_report.disk_name = pname + "_sandbox";
      experiment.policies.push_back(std::move(p_res));
    } catch (...) {
      cleanup();
      throw;
    }
    cleanup();
  }

  return experiment;
}

}  // namespace ghostfs
