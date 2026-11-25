#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "ghostfs/corruption.hpp"
#include "ghostfs/fsck.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/repair.hpp"
#include "ghostfs/sha256.hpp"
#include "ghostfs/virtual_disk.hpp"

namespace {

struct TempWorkspace {
  std::filesystem::path dir;
  TempWorkspace() {
    const auto ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    dir = std::filesystem::temp_directory_path() / ("ghostfs_p6_" + std::to_string(ts));
    std::filesystem::create_directories(dir / "disk_images" / "sandbox");
  }
  ~TempWorkspace() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

std::filesystem::path create_populated_disk(
    const std::filesystem::path& workspace,
    const std::string& name) {
  const auto img = workspace / "disk_images" / name;
  ghostfs::VirtualDisk::create_image(img, 4096U, 128U, true);
  ghostfs::VirtualDisk disk;
  disk.open_image(img, 4096U, true);
  ghostfs::GfsFileSystem fs;
  fs.format(disk, "LabDisk", ghostfs::AllocPolicy::FirstFit, 64U);
  fs.write_file("/alpha.txt", std::vector<uint8_t>{'A', 'L', 'P', 'H', 'A', '1', '2', '3'}, false);
  std::vector<uint8_t> beta(5000U, 0x42U);
  fs.write_file("/beta.bin", beta, false);
  fs.mkdir("/docs");
  fs.write_file("/docs/note.txt", std::vector<uint8_t>{'N', 'O', 'T', 'E'}, false);
  disk.sync();
  disk.close_image();
  return img;
}

TEST(Phase6RepairTest, CleanDiskProducesZeroIssuesAndSafetyChecks) {
  TempWorkspace ws;
  const auto orig = create_populated_disk(ws.dir, "clean.gfs");
  const std::string orig_sha = ghostfs::compute_file_sha256_streaming(orig);

  const auto clean_check = ghostfs::GfsckEngine::check(orig, "full");
  EXPECT_EQ(clean_check.total_issues, 0U);
  EXPECT_EQ(clean_check.consistency_status, "clean");
  EXPECT_EQ(clean_check.preserved_files_count, 3U);

  const auto exp = ghostfs::CorruptionEngine::create_experiment(ws.dir, "clean.gfs", "sb_clean");
  EXPECT_EQ(exp.original_sha256, orig_sha);
  EXPECT_EQ(exp.initial_sandbox_sha256, orig_sha);

  // Confirmation required for injection
  ghostfs::CorruptionInjectRequest no_conf{};
  no_conf.category = "primary_superblock";
  no_conf.scenario = "corrupt_magic";
  no_conf.confirm = false;
  EXPECT_THROW(
      ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, no_conf),
      std::runtime_error);

  // Path traversal rejected
  EXPECT_THROW(
      ghostfs::CorruptionEngine::create_experiment(ws.dir, "../etc/passwd", "bad"),
      std::runtime_error);

  // Original disk remains untouched
  EXPECT_EQ(ghostfs::compute_file_sha256_streaming(orig), orig_sha);
}

TEST(Phase6RepairTest, ScenarioA_PrimarySuperblockRecoveryAndDryRun) {
  TempWorkspace ws;
  const auto orig = create_populated_disk(ws.dir, "scen_a.gfs");
  const std::string orig_sha = ghostfs::compute_file_sha256_streaming(orig);

  auto exp = ghostfs::CorruptionEngine::create_experiment(ws.dir, "scen_a.gfs", "sb_a");
  ghostfs::CorruptionInjectRequest req{};
  req.category = "primary_superblock";
  req.scenario = "corrupt_magic";
  req.confirm = true;
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req);
  EXPECT_NE(exp.current_sandbox_sha256, orig_sha);

  const auto sb_path = ghostfs::CorruptionEngine::sandbox_dir(ws.dir) / exp.sandbox_filename;
  const auto chk = ghostfs::GfsckEngine::check(sb_path, "full");
  EXPECT_FALSE(chk.primary_sb_valid);
  EXPECT_TRUE(chk.backup_sb_valid);
  EXPECT_GE(chk.critical_count, 1U);

  const auto plan = ghostfs::RepairEngine::generate_plan(sb_path, "full");
  ASSERT_FALSE(plan.actions.empty());
  EXPECT_EQ(plan.actions[0].action_type, "restore_primary_superblock");

  // Dry run does not mutate sandbox
  const auto dry = ghostfs::RepairEngine::apply_plan(
      ws.dir, exp.experiment_id, plan.plan_id, plan.disk_sha256_at_plan, {}, true, false);
  EXPECT_TRUE(dry.dry_run);
  EXPECT_EQ(dry.post_repair_sha256, exp.current_sandbox_sha256);

  // Apply repair
  const auto rep = ghostfs::RepairEngine::apply_plan(
      ws.dir, exp.experiment_id, plan.plan_id, plan.disk_sha256_at_plan, {}, false, true);
  EXPECT_EQ(rep.post_check.total_issues, 0U);
  EXPECT_EQ(rep.post_check.consistency_status, "clean");
  EXPECT_EQ(rep.post_check.preserved_files_count, 3U);

  // Original untouched
  EXPECT_EQ(ghostfs::compute_file_sha256_streaming(orig), orig_sha);
}

TEST(Phase6RepairTest, ScenarioB_BitmapReconstructionAndIdempotence) {
  TempWorkspace ws;
  create_populated_disk(ws.dir, "scen_b.gfs");
  auto exp = ghostfs::CorruptionEngine::create_experiment(ws.dir, "scen_b.gfs", "sb_b");

  ghostfs::CorruptionInjectRequest req{};
  req.category = "block_bitmap";
  req.scenario = "both";
  req.confirm = true;
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req);

  const auto sb_path = ghostfs::CorruptionEngine::sandbox_dir(ws.dir) / exp.sandbox_filename;
  const auto plan = ghostfs::RepairEngine::generate_plan(sb_path, "full");
  EXPECT_GE(plan.initial_check.total_issues, 2U);

  const auto rep = ghostfs::RepairEngine::apply_plan(
      ws.dir, exp.experiment_id, plan.plan_id, plan.disk_sha256_at_plan, {}, false, true);
  EXPECT_EQ(rep.post_check.total_issues, 0U);
  EXPECT_EQ(rep.post_check.consistency_status, "clean");

  // Idempotence: second plan has zero actions
  const auto plan2 = ghostfs::RepairEngine::generate_plan(sb_path, "full");
  EXPECT_TRUE(plan2.actions.empty());
}

TEST(Phase6RepairTest, ScenarioC_OrphanedInodeRecoveryToLostAndFound) {
  TempWorkspace ws;
  create_populated_disk(ws.dir, "scen_c.gfs");
  auto exp = ghostfs::CorruptionEngine::create_experiment(ws.dir, "scen_c.gfs", "sb_c");

  ghostfs::CorruptionInjectRequest req{};
  req.category = "advanced";
  req.scenario = "orphan_inode";
  req.confirm = true;
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req);

  const auto sb_path = ghostfs::CorruptionEngine::sandbox_dir(ws.dir) / exp.sandbox_filename;
  const auto plan = ghostfs::RepairEngine::generate_plan(sb_path, "full");
  bool has_orphan = false;
  for (const auto& a : plan.actions) {
    if (a.action_type == "reconnect_orphan_inode") has_orphan = true;
  }
  EXPECT_TRUE(has_orphan);

  const auto rep = ghostfs::RepairEngine::apply_plan(
      ws.dir, exp.experiment_id, plan.plan_id, plan.disk_sha256_at_plan, {}, false, true);
  EXPECT_FALSE(rep.reconnected_paths.empty());
  EXPECT_EQ(rep.post_check.total_issues, 0U);
  EXPECT_EQ(rep.post_check.consistency_status, "clean");
}

TEST(Phase6RepairTest, ScenarioD_JournalPendingAndDirtyStateRecovery) {
  TempWorkspace ws;
  create_populated_disk(ws.dir, "scen_d.gfs");
  auto exp = ghostfs::CorruptionEngine::create_experiment(ws.dir, "scen_d.gfs", "sb_d");

  ghostfs::CorruptionInjectRequest req{};
  req.category = "journal";
  req.scenario = "pending_uncommitted";
  req.confirm = true;
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req);

  const auto sb_path = ghostfs::CorruptionEngine::sandbox_dir(ws.dir) / exp.sandbox_filename;
  const auto chk = ghostfs::GfsckEngine::check(sb_path, "full");
  EXPECT_TRUE(chk.dirty_journal);

  const auto plan = ghostfs::RepairEngine::generate_plan(sb_path, "full");
  const auto rep = ghostfs::RepairEngine::apply_plan(
      ws.dir, exp.experiment_id, plan.plan_id, plan.disk_sha256_at_plan, {}, false, true);
  EXPECT_EQ(rep.post_check.total_issues, 0U);
  EXPECT_FALSE(rep.post_check.dirty_journal);
  EXPECT_EQ(rep.post_check.consistency_status, "clean");
}

TEST(Phase6RepairTest, ScenarioE_DuplicateOwnershipRemainsAmbiguousAndPreserved) {
  TempWorkspace ws;
  create_populated_disk(ws.dir, "scen_e.gfs");
  auto exp = ghostfs::CorruptionEngine::create_experiment(ws.dir, "scen_e.gfs", "sb_e");

  ghostfs::CorruptionInjectRequest req{};
  req.category = "inode";
  req.scenario = "duplicate_block_ownership";
  req.confirm = true;
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req);

  const auto sb_path = ghostfs::CorruptionEngine::sandbox_dir(ws.dir) / exp.sandbox_filename;
  const auto plan = ghostfs::RepairEngine::generate_plan(sb_path, "full");
  bool found_dup = false;
  for (const auto& u : plan.unrepairable_or_ambiguous) {
    if (u.issue_type == "DUPLICATE_BLOCK_OWNERSHIP") {
      found_dup = true;
      EXPECT_EQ(u.repairability, "ambiguous_manual_required");
    }
  }
  EXPECT_TRUE(found_dup);
}

TEST(Phase6RepairTest, ScenarioF_PayloadCorruptionDistinguishedAndNeverFalsified) {
  TempWorkspace ws;
  create_populated_disk(ws.dir, "scen_f.gfs");
  auto exp = ghostfs::CorruptionEngine::create_experiment(ws.dir, "scen_f.gfs", "sb_f");

  ghostfs::CorruptionInjectRequest req{};
  req.category = "payload";
  req.scenario = "bit_flip";
  req.seed = 12345;
  req.confirm = true;
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req);

  const auto sb_path = ghostfs::CorruptionEngine::sandbox_dir(ws.dir) / exp.sandbox_filename;
  const auto plan = ghostfs::RepairEngine::generate_plan(sb_path, "full");
  EXPECT_EQ(plan.initial_check.damaged_files_count, 1U);
  EXPECT_EQ(plan.initial_check.consistency_status, "payload_damaged");
  EXPECT_TRUE(plan.actions.empty());
  ASSERT_FALSE(plan.unrepairable_or_ambiguous.empty());
  EXPECT_EQ(plan.unrepairable_or_ambiguous[0].issue_type, "PAYLOAD_CRC_MISMATCH");
  EXPECT_EQ(plan.unrepairable_or_ambiguous[0].repairability, "not_repairable");
}

TEST(Phase6RepairTest, ScenarioG_DualSuperblockFailsGracefully) {
  TempWorkspace ws;
  create_populated_disk(ws.dir, "scen_g.gfs");
  auto exp = ghostfs::CorruptionEngine::create_experiment(ws.dir, "scen_g.gfs", "sb_g");

  ghostfs::CorruptionInjectRequest req{};
  req.category = "dual_superblock";
  req.scenario = "corrupt_geometry";
  req.confirm = true;
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req);

  const auto sb_path = ghostfs::CorruptionEngine::sandbox_dir(ws.dir) / exp.sandbox_filename;
  const auto chk = ghostfs::GfsckEngine::check(sb_path, "full");
  EXPECT_EQ(chk.consistency_status, "unrecoverable_geometry");
  EXPECT_GE(chk.critical_count, 1U);
}

TEST(Phase6RepairTest, ScenarioH_StaleRepairPlanRejectedAndScenarioI_MultiCorruption) {
  TempWorkspace ws;
  create_populated_disk(ws.dir, "scen_hi.gfs");
  auto exp = ghostfs::CorruptionEngine::create_experiment(ws.dir, "scen_hi.gfs", "sb_hi");

  ghostfs::CorruptionInjectRequest req{};
  req.category = "advanced";
  req.scenario = "multi_corruption";
  req.confirm = true;
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req);

  const auto sb_path = ghostfs::CorruptionEngine::sandbox_dir(ws.dir) / exp.sandbox_filename;
  const auto stale_plan = ghostfs::RepairEngine::generate_plan(sb_path, "full");

  // Mutate sandbox again
  ghostfs::CorruptionInjectRequest req2{};
  req2.category = "directory";
  req2.scenario = "directory_cycle";
  req2.confirm = true;
  exp = ghostfs::CorruptionEngine::inject_corruption(ws.dir, exp.experiment_id, req2);

  // Old plan must be rejected as stale
  EXPECT_THROW(
      ghostfs::RepairEngine::apply_plan(
          ws.dir, exp.experiment_id, stale_plan.plan_id, stale_plan.disk_sha256_at_plan, {}, false, true),
      std::runtime_error);

  // Fresh plan resolves all 3 simultaneous corruptions
  const auto fresh_plan = ghostfs::RepairEngine::generate_plan(sb_path, "full");
  EXPECT_GE(fresh_plan.actions.size(), 3U);
  const auto rep = ghostfs::RepairEngine::apply_plan(
      ws.dir, exp.experiment_id, fresh_plan.plan_id, fresh_plan.disk_sha256_at_plan, {}, false, true);
  EXPECT_EQ(rep.post_check.total_issues, 0U);
  EXPECT_EQ(rep.post_check.consistency_status, "clean");

  // Comparison between pre-repair and repaired shows changed blocks
  const auto cmp = ghostfs::RepairEngine::compare_experiment(ws.dir, exp.experiment_id, "corrupted_vs_repaired");
  EXPECT_FALSE(cmp.identical);
  EXPECT_GT(cmp.total_bytes_changed, 0ULL);
}

}  // namespace
