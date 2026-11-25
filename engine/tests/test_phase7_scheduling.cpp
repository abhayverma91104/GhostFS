#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <numeric>
#include <set>
#include <vector>

#include "ghostfs/disk_scheduler.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/io_trace.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/virtual_disk.hpp"

namespace {

std::vector<ghostfs::IoRequest> make_reqs(const std::vector<uint32_t>& cyls) {
  std::vector<ghostfs::IoRequest> out;
  for (uint32_t i = 0; i < cyls.size(); ++i) {
    ghostfs::IoRequest r{};
    r.request_id = "req_" + std::to_string(i + 1U);
    r.original_index = i;
    r.cylinder = cyls[i];
    out.push_back(r);
  }
  return out;
}

void verify_invariants(
    const ghostfs::SimulationResult& res,
    const std::vector<ghostfs::IoRequest>& input) {
  EXPECT_EQ(res.serviced_count, input.size());
  EXPECT_EQ(res.service_sequence.size(), input.size());
  std::set<std::string> seen_ids;
  uint64_t step_sum = 0;
  for (const auto& st : res.steps) {
    EXPECT_GE(st.from_cylinder, res.min_cylinder);
    EXPECT_LE(st.from_cylinder, res.max_cylinder);
    EXPECT_GE(st.to_cylinder, res.min_cylinder);
    EXPECT_LE(st.to_cylinder, res.max_cylinder);
    step_sum += st.movement;
    if (st.is_service) {
      EXPECT_TRUE(seen_ids.insert(st.request_id).second);
    }
  }
  EXPECT_EQ(seen_ids.size(), input.size());
  EXPECT_EQ(step_sum, res.total_head_movement);
  for (uint32_t pos : res.head_path) {
    EXPECT_GE(pos, res.min_cylinder);
    EXPECT_LE(pos, res.max_cylinder);
  }
}

TEST(Phase7SchedulingTest, TextbookEightRequestQueueExactTotalsAndPaths) {
  const auto reqs = make_reqs({98, 183, 37, 122, 14, 124, 65, 67});
  ghostfs::SchedulerConfig cfg{};
  cfg.min_cylinder = 0;
  cfg.max_cylinder = 199;
  cfg.initial_head = 53;
  cfg.initial_direction = ghostfs::SweepDirection::Right;

  const auto cmp = ghostfs::DiskScheduler::compare_all(cfg, reqs);
  ASSERT_EQ(cmp.algorithms.size(), 6U);

  // FCFS = 640
  EXPECT_EQ(cmp.algorithms[0].result.algorithm, "FCFS");
  EXPECT_EQ(cmp.algorithms[0].result.total_head_movement, 640ULL);
  EXPECT_EQ(cmp.algorithms[0].result.head_path,
            (std::vector<uint32_t>{53, 98, 183, 37, 122, 14, 124, 65, 67}));
  verify_invariants(cmp.algorithms[0].result, reqs);

  // SSTF = 236
  EXPECT_EQ(cmp.algorithms[1].result.algorithm, "SSTF");
  EXPECT_EQ(cmp.algorithms[1].result.total_head_movement, 236ULL);
  EXPECT_EQ(cmp.algorithms[1].result.head_path,
            (std::vector<uint32_t>{53, 65, 67, 37, 14, 98, 122, 124, 183}));
  verify_invariants(cmp.algorithms[1].result, reqs);

  // SCAN = 331
  EXPECT_EQ(cmp.algorithms[2].result.algorithm, "SCAN");
  EXPECT_EQ(cmp.algorithms[2].result.total_head_movement, 331ULL);
  EXPECT_EQ(cmp.algorithms[2].result.head_path,
            (std::vector<uint32_t>{53, 65, 67, 98, 122, 124, 183, 199, 37, 14}));
  verify_invariants(cmp.algorithms[2].result, reqs);

  // C-SCAN = 382
  EXPECT_EQ(cmp.algorithms[3].result.algorithm, "C-SCAN");
  EXPECT_EQ(cmp.algorithms[3].result.total_head_movement, 382ULL);
  EXPECT_EQ(cmp.algorithms[3].result.head_path,
            (std::vector<uint32_t>{53, 65, 67, 98, 122, 124, 183, 199, 0, 14, 37}));
  EXPECT_EQ(cmp.algorithms[3].result.circular_repositioning_distance, 199ULL);
  verify_invariants(cmp.algorithms[3].result, reqs);

  // LOOK = 299
  EXPECT_EQ(cmp.algorithms[4].result.algorithm, "LOOK");
  EXPECT_EQ(cmp.algorithms[4].result.total_head_movement, 299ULL);
  EXPECT_EQ(cmp.algorithms[4].result.head_path,
            (std::vector<uint32_t>{53, 65, 67, 98, 122, 124, 183, 37, 14}));
  verify_invariants(cmp.algorithms[4].result, reqs);

  // C-LOOK = 322
  EXPECT_EQ(cmp.algorithms[5].result.algorithm, "C-LOOK");
  EXPECT_EQ(cmp.algorithms[5].result.total_head_movement, 322ULL);
  EXPECT_EQ(cmp.algorithms[5].result.head_path,
            (std::vector<uint32_t>{53, 65, 67, 98, 122, 124, 183, 14, 37}));
  EXPECT_EQ(cmp.algorithms[5].result.circular_repositioning_distance, 169ULL);
  verify_invariants(cmp.algorithms[5].result, reqs);
}

TEST(Phase7SchedulingTest, EdgeCasesEmptyDuplicatesBoundariesLeftwardAndValidation) {
  ghostfs::SchedulerConfig cfg{};
  cfg.min_cylinder = 0;
  cfg.max_cylinder = 199;
  cfg.initial_head = 50;

  // Empty queue -> no division by zero
  const auto empty_res = ghostfs::DiskScheduler::simulate(cfg, {});
  EXPECT_EQ(empty_res.total_head_movement, 0ULL);
  EXPECT_EQ(empty_res.average_movement, 0.0);
  EXPECT_EQ(empty_res.serviced_count, 0U);

  // Duplicates + at initial head + boundaries
  const auto dup_reqs = make_reqs({50, 0, 199, 50, 100});
  for (const auto alg : {
           ghostfs::SchedulingAlgorithm::FCFS,
           ghostfs::SchedulingAlgorithm::SSTF,
           ghostfs::SchedulingAlgorithm::SCAN,
           ghostfs::SchedulingAlgorithm::CSCAN,
           ghostfs::SchedulingAlgorithm::LOOK,
           ghostfs::SchedulingAlgorithm::CLOOK,
       }) {
    cfg.algorithm = alg;
    const auto res = ghostfs::DiskScheduler::simulate(cfg, dup_reqs);
    verify_invariants(res, dup_reqs);
  }

  // Leftward sweep on SCAN
  cfg.algorithm = ghostfs::SchedulingAlgorithm::SCAN;
  cfg.initial_direction = ghostfs::SweepDirection::Left;
  const auto left_reqs = make_reqs({20, 80});
  const auto left_res = ghostfs::DiskScheduler::simulate(cfg, left_reqs);
  // 50 -> 20 -> 0 -> 80 = 30 + 20 + 80 = 130
  EXPECT_EQ(left_res.total_head_movement, 130ULL);
  verify_invariants(left_res, left_reqs);

  // Invalid head / geometry / out-of-range request
  ghostfs::SchedulerConfig bad_head = cfg;
  bad_head.initial_head = 200;
  EXPECT_THROW(ghostfs::DiskScheduler::simulate(bad_head, left_reqs), std::runtime_error);
  EXPECT_THROW(ghostfs::DiskScheduler::simulate(cfg, make_reqs({250})), std::runtime_error);
}

TEST(Phase7SchedulingTest, DynamicArrivalTimeSchedulingDiffersFromStatic) {
  ghostfs::IoRequest r1{};
  r1.request_id = "dyn_1";
  r1.cylinder = 150;
  r1.arrival_time = 0;

  ghostfs::IoRequest r2{};
  r2.request_id = "dyn_2";
  r2.cylinder = 60;
  r2.arrival_time = 200;  // arrives long after r1 finishes

  ghostfs::SchedulerConfig cfg{};
  cfg.algorithm = ghostfs::SchedulingAlgorithm::SSTF;
  cfg.min_cylinder = 0;
  cfg.max_cylinder = 199;
  cfg.initial_head = 50;
  cfg.scheduling_mode = "dynamic";

  const auto dyn_res = ghostfs::DiskScheduler::simulate(cfg, {r1, r2});
  // In dynamic mode, r1 (150) must be serviced first because r2 (60) hasn't arrived at t=0
  EXPECT_EQ(dyn_res.service_sequence, (std::vector<uint32_t>{150, 60}));
  EXPECT_EQ(dyn_res.total_head_movement, 190ULL);

  cfg.scheduling_mode = "static";
  const auto stat_res = ghostfs::DiskScheduler::simulate(cfg, {r1, r2});
  // In static mode, both are available at t=0, so SSTF picks 60 first then 150
  EXPECT_EQ(stat_res.service_sequence, (std::vector<uint32_t>{60, 150}));
  EXPECT_EQ(stat_res.total_head_movement, 100ULL);
}

TEST(Phase7SchedulingTest, GfsFileRecoveryMetadataAndAllocationIntegrationAndImmutability) {
  const auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  const auto ws = std::filesystem::temp_directory_path() / ("ghostfs_p7_" + std::to_string(ts));
  std::filesystem::create_directories(ws / "disk_images" / "sandbox");
  const auto img = ws / "disk_images" / "sched_test.gfs";

  ghostfs::VirtualDisk::create_image(img, 4096U, 128U, true);
  {
    ghostfs::VirtualDisk disk;
    disk.open_image(img, 4096U, true);
    ghostfs::GfsFileSystem fs;
    fs.format(disk, "SchedDisk", ghostfs::AllocPolicy::FirstFit, 64U);
    fs.write_file("/a.bin", std::vector<uint8_t>(8192U, 0x11U), false);
    fs.write_file("/b.bin", std::vector<uint8_t>(4096U, 0x22U), false);
    fs.write_file("/c.bin", std::vector<uint8_t>(12288U, 0x33U), false);
    fs.delete_file("/b.bin");
    // Fragmented write into freed space + tail
    fs.write_file("/frag.bin", std::vector<uint8_t>(12288U, 0x44U), false);
    disk.sync();
    disk.close_image();
  }

  const std::string before_sha = ghostfs::compute_file_sha256_streaming(img);

  // File-based workload
  const auto fw = ghostfs::IoTraceBuilder::from_gfs_files(img, {"/frag.bin"}, true, 1U);
  EXPECT_GE(fw.requests.size(), 4U);

  // Metadata workload
  const auto mw = ghostfs::IoTraceBuilder::from_filesystem_metadata(img, 1U);
  EXPECT_EQ(mw.requests.size(), 7U);

  // Allocation + Scheduling comparison
  const std::vector<ghostfs::WorkloadStep> steps = {
      {"create", "/f1.bin", 8192ULL},
      {"create", "/f2.bin", 4096ULL},
      {"create", "/f3.bin", 8192ULL},
      {"delete", "/f2.bin", 0ULL},
      {"create", "/f4.bin", 12288ULL},
  };
  const auto alloc_sched = ghostfs::IoTraceBuilder::compare_allocation_and_scheduling(
      ws, img, 4096U, 128U, steps, 0U, ghostfs::SweepDirection::Right, 1U);
  EXPECT_EQ(alloc_sched.policies.size(), 4U);
  for (const auto& p : alloc_sched.policies) {
    EXPECT_EQ(p.scheduling.algorithms.size(), 6U);
  }

  // Source disk untouched
  EXPECT_EQ(ghostfs::compute_file_sha256_streaming(img), before_sha);

  std::error_code ec;
  std::filesystem::remove_all(ws, ec);
}

}  // namespace
