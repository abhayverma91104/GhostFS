#include "ghostfs/repair.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

#include "ghostfs/corruption.hpp"
#include "ghostfs/crc32.hpp"
#include "ghostfs/endian.hpp"
#include "ghostfs/engine.hpp"
#include "ghostfs/gfs_format.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/virtual_disk.hpp"

namespace ghostfs {
using endian::read_u32_le;
namespace {

int action_priority(const std::string& type) {
  if (type == "restore_primary_superblock" || type == "restore_backup_superblock" ||
      type == "sync_superblock_replica" || type == "reset_alloc_policy") {
    return 10;
  }
  if (type == "abort_corrupt_journal_slot" || type == "reconcile_journal_dirty_state") {
    return 20;
  }
  if (type == "reconstruct_block_bitmap" || type == "reclaim_unreferenced_block" ||
      type == "reconstruct_inode_bitmap" || type == "clear_leaked_inode_bitmap") {
    return 30;
  }
  if (type == "remove_dangling_dir_entry" || type == "remove_duplicate_dir_entry" ||
      type == "break_directory_cycle" || type == "fix_parent_inode") {
    return 40;
  }
  if (type == "reconnect_orphan_inode") {
    return 50;
  }
  return 99;
}

std::string bytes_hex_sample(const uint8_t* data, size_t len) {
  std::ostringstream oss;
  oss << std::hex << std::uppercase << std::setfill('0');
  const size_t n = std::min<size_t>(len, 16);
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) oss << ' ';
    oss << std::setw(2) << static_cast<unsigned>(data[i]);
  }
  return oss.str();
}

void set_bitmap_bit_and_write(
    VirtualDisk& disk,
    uint32_t bitmap_start,
    uint32_t bit_index,
    bool value) {
  const uint32_t bs = disk.block_size();
  const uint32_t bits_per_blk = bs * 8U;
  const uint32_t blk = bitmap_start + (bit_index / bits_per_blk);
  const uint32_t in_blk = bit_index % bits_per_blk;
  const uint32_t byte_idx = in_blk / 8U;
  const uint8_t mask = static_cast<uint8_t>(1U << (in_blk % 8U));
  std::vector<uint8_t> buf;
  disk.read_block(blk, buf);
  if (value) buf[byte_idx] |= mask;
  else buf[byte_idx] &= static_cast<uint8_t>(~mask);
  disk.write_block(blk, buf);
}

bool get_bitmap_bit_raw(
    const VirtualDisk& disk,
    uint32_t bitmap_start,
    uint32_t bit_index) {
  const uint32_t bs = disk.block_size();
  const uint32_t bits_per_blk = bs * 8U;
  const uint32_t blk = bitmap_start + (bit_index / bits_per_blk);
  const uint32_t in_blk = bit_index % bits_per_blk;
  std::vector<uint8_t> buf;
  disk.read_block(blk, buf);
  return (buf[in_blk / 8U] & static_cast<uint8_t>(1U << (in_blk % 8U))) != 0;
}

Superblock read_best_superblock(VirtualDisk& disk) {
  std::vector<uint8_t> boot_raw;
  disk.read_raw(0, kBootBlockBytes, boot_raw);
  BootBlock boot{};
  bool boot_crc = false;
  deserialize_boot_block(boot_raw.data(), boot, boot_crc);
  const uint32_t bs = is_supported_block_size(boot.block_size) ? boot.block_size : kDefaultBlockSize;
  disk.set_block_size(bs);

  std::vector<uint8_t> b1, b2;
  disk.read_block(1, b1);
  disk.read_block(2, b2);
  Superblock sb1{}, sb2{};
  bool c1 = false, c2 = false;
  const bool m1 = deserialize_superblock(b1.data(), sb1, c1);
  const bool m2 = deserialize_superblock(b2.data(), sb2, c2);
  if (m1 && c1) return sb1;
  if (m2 && c2) return sb2;
  throw std::runtime_error("ERR_UNREPAIRABLE: No valid superblock available");
}

void write_both_superblocks(VirtualDisk& disk, const Superblock& sb) {
  std::vector<uint8_t> buf(sb.block_size, 0U);
  serialize_superblock(sb, buf.data());
  disk.write_block(1, buf);
  disk.write_block(2, buf);
}

void recount_free_counts_and_save_sb(VirtualDisk& disk, Superblock& sb) {
  uint32_t free_blks = 0;
  for (uint32_t b = sb.data_region_start; b < sb.journal_start; ++b) {
    if (!get_bitmap_bit_raw(disk, sb.block_bitmap_start, b)) free_blks++;
  }
  uint32_t free_inos = 0;
  for (uint32_t i = 1; i < sb.total_inodes; ++i) {
    if (!get_bitmap_bit_raw(disk, sb.inode_bitmap_start, i)) free_inos++;
  }
  sb.free_blocks = free_blks;
  sb.free_inodes = free_inos;
  write_both_superblocks(disk, sb);
}

Inode read_inode(const VirtualDisk& disk, const Superblock& sb, uint32_t id, bool& crc_ok) {
  const uint32_t bs = sb.block_size;
  const uint32_t per_blk = bs / static_cast<uint32_t>(kInodeBytes);
  const uint32_t blk = sb.inode_table_start + (id / per_blk);
  const uint32_t off = (id % per_blk) * static_cast<uint32_t>(kInodeBytes);
  std::vector<uint8_t> buf;
  disk.read_block(blk, buf);
  Inode out{};
  deserialize_inode(buf.data() + off, out, crc_ok);
  return out;
}

void write_inode(VirtualDisk& disk, const Superblock& sb, const Inode& ino) {
  const uint32_t bs = sb.block_size;
  const uint32_t per_blk = bs / static_cast<uint32_t>(kInodeBytes);
  const uint32_t blk = sb.inode_table_start + (ino.inode_id / per_blk);
  const uint32_t off = (ino.inode_id % per_blk) * static_cast<uint32_t>(kInodeBytes);
  std::vector<uint8_t> buf;
  disk.read_block(blk, buf);
  serialize_inode(ino, buf.data() + off);
  disk.write_block(blk, buf);
}

uint32_t ensure_lost_and_found(VirtualDisk& disk, Superblock& sb) {
  bool root_ok = false;
  Inode root = read_inode(disk, sb, kRootInodeId, root_ok);
  const uint32_t bs = sb.block_size;
  const uint32_t slots = bs / static_cast<uint32_t>(kDirEntryBytes);

  // Check if lost+found already exists in root
  for (uint32_t d = 0; d < kDirectBlockCount; ++d) {
    if (root.direct_blocks[d] == 0) continue;
    std::vector<uint8_t> dbuf;
    disk.read_block(root.direct_blocks[d], dbuf);
    for (uint32_t s = 0; s < slots; ++s) {
      DirEntry de{};
      deserialize_dir_entry(dbuf.data() + s * kDirEntryBytes, de);
      if (de.state == static_cast<uint8_t>(DirEntryState::Active) && de.name == "lost+found" &&
          de.entry_type == static_cast<uint8_t>(InodeType::Directory)) {
        return de.inode_id;
      }
    }
  }

  // Allocate a free inode and a free data block for /lost+found
  uint32_t lf_ino_id = 0;
  for (uint32_t i = 2; i < sb.total_inodes; ++i) {
    bool ok = false;
    const Inode cand = read_inode(disk, sb, i, ok);
    if (!get_bitmap_bit_raw(disk, sb.inode_bitmap_start, i) &&
        cand.state != static_cast<uint8_t>(InodeState::Allocated)) {
      lf_ino_id = i;
      break;
    }
  }
  uint32_t lf_blk = 0;
  for (uint32_t b = sb.data_region_start; b < sb.journal_start; ++b) {
    if (!get_bitmap_bit_raw(disk, sb.block_bitmap_start, b)) {
      lf_blk = b;
      break;
    }
  }
  if (lf_ino_id == 0 || lf_blk == 0) {
    throw std::runtime_error("ERR_NO_SPACE: Cannot allocate inode/block for /lost+found");
  }

  set_bitmap_bit_and_write(disk, sb.inode_bitmap_start, lf_ino_id, true);
  set_bitmap_bit_and_write(disk, sb.block_bitmap_start, lf_blk, true);

  std::vector<uint8_t> lf_dir_buf(bs, 0U);
  DirEntry dot{};
  dot.inode_id = lf_ino_id;
  dot.entry_type = static_cast<uint8_t>(InodeType::Directory);
  dot.state = static_cast<uint8_t>(DirEntryState::Active);
  dot.name = ".";
  dot.name_len = 1;
  serialize_dir_entry(dot, lf_dir_buf.data());

  DirEntry dotdot{};
  dotdot.inode_id = kRootInodeId;
  dotdot.entry_type = static_cast<uint8_t>(InodeType::Directory);
  dotdot.state = static_cast<uint8_t>(DirEntryState::Active);
  dotdot.name = "..";
  dotdot.name_len = 2;
  serialize_dir_entry(dotdot, lf_dir_buf.data() + kDirEntryBytes);
  disk.write_block(lf_blk, lf_dir_buf);

  Inode lf_ino{};
  lf_ino.inode_id = lf_ino_id;
  lf_ino.type = static_cast<uint8_t>(InodeType::Directory);
  lf_ino.state = static_cast<uint8_t>(InodeState::Allocated);
  lf_ino.permissions = 0755;
  lf_ino.link_count = 2;
  lf_ino.parent_inode = kRootInodeId;
  lf_ino.direct_blocks[0] = lf_blk;
  lf_ino.generation = 1;
  write_inode(disk, sb, lf_ino);

  // Add "lost+found" entry to root directory
  for (uint32_t d = 0; d < kDirectBlockCount; ++d) {
    if (root.direct_blocks[d] == 0) continue;
    std::vector<uint8_t> dbuf;
    disk.read_block(root.direct_blocks[d], dbuf);
    for (uint32_t s = 0; s < slots; ++s) {
      DirEntry de{};
      deserialize_dir_entry(dbuf.data() + s * kDirEntryBytes, de);
      if (de.state != static_cast<uint8_t>(DirEntryState::Active)) {
        DirEntry lf_de{};
        lf_de.inode_id = lf_ino_id;
        lf_de.entry_type = static_cast<uint8_t>(InodeType::Directory);
        lf_de.state = static_cast<uint8_t>(DirEntryState::Active);
        lf_de.name = "lost+found";
        lf_de.name_len = 10;
        serialize_dir_entry(lf_de, dbuf.data() + s * kDirEntryBytes);
        disk.write_block(root.direct_blocks[d], dbuf);
        return lf_ino_id;
      }
    }
  }
  throw std::runtime_error("ERR_NO_SPACE: No free slot in root directory for /lost+found");
}

}  // namespace

RepairPlan RepairEngine::generate_plan(
    const std::filesystem::path& disk_path,
    const std::string& check_mode) {
  const FsckResult check = GfsckEngine::check(disk_path, check_mode);

  RepairPlan plan{};
  plan.plan_id = "plan_" + check.disk_sha256.substr(0, 12);
  plan.disk_name = check.disk_name;
  plan.disk_sha256_at_plan = check.disk_sha256;
  plan.check_mode = check_mode;
  plan.initial_check = check;

  uint32_t act_idx = 1;
  for (const auto& iss : check.issues) {
    if (iss.repairability == "ambiguous_manual_required" || iss.repairability == "not_repairable") {
      plan.unrepairable_or_ambiguous.push_back(iss);
      continue;
    }

    RepairAction act{};
    act.action_id = "act_" + std::to_string(act_idx++);
    act.issue_id = iss.issue_id;
    act.byte_offsets = {iss.byte_offset};
    act.existing_value = iss.observed_value;
    act.repairability = iss.repairability;
    act.selected_by_default = (iss.repairability == "automatically_repairable" ||
                               iss.repairability == "repairable_with_confirmation");
    act.data_loss_implication = iss.data_loss_risk;

    if (iss.issue_type == "PRIMARY_SUPERBLOCK_CORRUPT") {
      act.action_type = "restore_primary_superblock";
      act.target_structure = "primary_superblock (block 1)";
      act.proposed_value = "Copy validated backup superblock (block 2) -> block 1";
      act.justification = iss.evidence;
      act.preconditions = "Backup superblock (block 2) must pass magic, CRC32, and geometry checks.";
      act.expected_post_repair_state = "Primary superblock valid and synchronized with backup.";
    } else if (iss.issue_type == "BACKUP_SUPERBLOCK_CORRUPT" || iss.issue_type == "SUPERBLOCK_REPLICA_MISMATCH") {
      act.action_type = "restore_backup_superblock";
      act.target_structure = "backup_superblock (block 2)";
      act.proposed_value = "Copy validated primary superblock (block 1) -> block 2";
      act.justification = iss.evidence;
      act.preconditions = "Primary superblock (block 1) must be valid.";
      act.expected_post_repair_state = "Backup superblock synchronized with primary.";
    } else if (iss.issue_type == "INVALID_ALLOC_POLICY") {
      act.action_type = "reset_alloc_policy";
      act.target_structure = "superblock.alloc_policy";
      act.proposed_value = "0 (FirstFit)";
      act.justification = iss.evidence;
      act.preconditions = "Valid superblock geometry.";
      act.expected_post_repair_state = "Valid allocation policy enum.";
    } else if (iss.issue_type == "PENDING_JOURNAL_ENTRY" || iss.issue_type == "INVALID_JOURNAL_CHECKSUM") {
      act.action_type = "abort_corrupt_journal_slot";
      act.target_structure = "journal (block " + std::to_string(iss.affected_block) + ")";
      act.proposed_value = "Mark slot Aborted (3) with valid CRC32 (conservative consistency reconstruction)";
      act.justification = iss.evidence;
      act.preconditions = "GFS1 64-byte journal lacks physical undo/redo before-images.";
      act.expected_post_repair_state = "No pending or invalid journal entries remain.";
    } else if (iss.issue_type == "DIRTY_FILESYSTEM_STATE") {
      act.action_type = "reconcile_journal_dirty_state";
      act.target_structure = "superblock.fs_state";
      act.proposed_value = "Clean (1) after all metadata checks pass";
      act.justification = iss.evidence;
      act.preconditions = "All metadata consistency checks must pass post-repair.";
      act.expected_post_repair_state = "fs_state = Clean (1).";
    } else if (iss.issue_type == "REFERENCED_BLOCK_MARKED_FREE") {
      act.action_type = "reconstruct_block_bitmap";
      act.target_structure = "block_bitmap (inode #" + std::to_string(iss.affected_inode) + ")";
      act.proposed_value = "Set block bitmap bit = 1";
      act.justification = iss.evidence;
      act.preconditions = "No duplicate ownership conflict on block.";
      act.expected_post_repair_state = "Block bitmap matches inode ownership.";
    } else if (iss.issue_type == "UNREFERENCED_ALLOCATED_BLOCK") {
      act.action_type = "reclaim_unreferenced_block";
      act.target_structure = "block_bitmap (offset " + std::to_string(iss.byte_offset) + ")";
      act.proposed_value = "Clear leaked block bitmap bit = 0";
      act.justification = iss.evidence;
      act.preconditions = "Block not referenced by any allocated inode.";
      act.expected_post_repair_state = "Leaked block reclaimed to free pool.";
    } else if (iss.issue_type == "ALLOCATED_INODE_MARKED_FREE") {
      act.action_type = "reconstruct_inode_bitmap";
      act.target_structure = "inode_bitmap (inode #" + std::to_string(iss.affected_inode) + ")";
      act.proposed_value = "Set inode bitmap bit = 1";
      act.justification = iss.evidence;
      act.preconditions = "Inode table slot is Allocated.";
      act.expected_post_repair_state = "Inode bitmap matches inode table.";
    } else if (iss.issue_type == "UNALLOCATED_INODE_MARKED_ACTIVE") {
      act.action_type = "clear_leaked_inode_bitmap";
      act.target_structure = "inode_bitmap (inode #" + std::to_string(iss.affected_inode) + ")";
      act.proposed_value = "Clear inode bitmap bit = 0";
      act.justification = iss.evidence;
      act.preconditions = "Inode table slot is Free/Deleted.";
      act.expected_post_repair_state = "Inode bitmap matches inode table.";
    } else if (iss.issue_type == "DANGLING_DIR_ENTRY") {
      act.action_type = "remove_dangling_dir_entry";
      act.target_structure = "directory_entry (block " + std::to_string(iss.affected_block) + ")";
      act.proposed_value = "Mark dangling entry state = Deleted (2) to preserve forensic trace";
      act.justification = iss.evidence;
      act.preconditions = "Target inode does not exist or is unallocated.";
      act.expected_post_repair_state = "Dangling reference removed from active directory view.";
    } else if (iss.issue_type == "DUPLICATE_DIR_NAME") {
      act.action_type = "remove_duplicate_dir_entry";
      act.target_structure = "directory_entry (block " + std::to_string(iss.affected_block) + ")";
      act.proposed_value = "Mark duplicate slot state = Deleted (2)";
      act.justification = iss.evidence;
      act.preconditions = "Duplicate active name in same directory.";
      act.expected_post_repair_state = "Unique filenames in directory.";
    } else if (iss.issue_type == "DIRECTORY_CYCLE") {
      act.action_type = "break_directory_cycle";
      act.target_structure = "directory_entry (block " + std::to_string(iss.affected_block) + ")";
      act.proposed_value = "Mark cyclic directory entry state = Deleted (2)";
      act.justification = iss.evidence;
      act.preconditions = "Entry creates cycle in directory graph.";
      act.expected_post_repair_state = "Acyclic directory tree.";
    } else if (iss.issue_type == "INCONSISTENT_PARENT_INODE") {
      act.action_type = "fix_parent_inode";
      act.target_structure = "inode #" + std::to_string(iss.affected_inode);
      act.proposed_value = "Set parent_inode = " + iss.expected_value;
      act.justification = iss.evidence;
      act.preconditions = "Valid parent directory entry.";
      act.expected_post_repair_state = "Consistent parent_inode field.";
    } else if (iss.issue_type == "ORPHANED_INODE") {
      act.action_type = "reconnect_orphan_inode";
      act.target_structure = "inode #" + std::to_string(iss.affected_inode) + " -> /lost+found";
      act.proposed_value = "Link under /lost+found/inode_" + std::to_string(iss.affected_inode);
      act.justification = iss.evidence;
      act.preconditions = "Inode is Allocated and unreachable from '/'.";
      act.expected_post_repair_state = "Orphaned file accessible under /lost+found.";
    } else {
      continue;
    }

    plan.actions.push_back(std::move(act));
  }

  std::sort(plan.actions.begin(), plan.actions.end(), [](const RepairAction& a, const RepairAction& b) {
    return action_priority(a.action_type) < action_priority(b.action_type);
  });

  return plan;
}

RepairApplyResult RepairEngine::apply_plan(
    const std::filesystem::path& workspace,
    const std::string& experiment_id,
    const std::string& plan_id,
    const std::string& expected_disk_sha256,
    const std::vector<std::string>& selected_action_ids,
    bool dry_run,
    bool confirm) {
  SandboxExperiment exp = CorruptionEngine::get_experiment(workspace, experiment_id);
  const auto sb_dir = CorruptionEngine::sandbox_dir(workspace);
  const auto sb_path = resolve_workspace_path(sb_dir, exp.sandbox_filename);

  const std::string current_sha = compute_file_sha256_streaming(sb_path);
  if (!expected_disk_sha256.empty() && current_sha != expected_disk_sha256) {
    throw std::runtime_error(
        "ERR_STALE_PLAN: Sandbox disk SHA-256 changed since repair plan was generated; regenerate plan.");
  }

  const RepairPlan current_plan = generate_plan(sb_path, "full");
  if (!plan_id.empty() && plan_id != current_plan.plan_id) {
    throw std::runtime_error("ERR_STALE_PLAN: Repair plan ID does not match current disk state.");
  }

  std::unordered_set<std::string> selected_set(selected_action_ids.begin(), selected_action_ids.end());
  const bool use_defaults = selected_action_ids.empty();

  std::vector<RepairAction> to_apply;
  std::vector<std::string> skipped;
  for (const auto& act : current_plan.actions) {
    if ((use_defaults && act.selected_by_default) || (!use_defaults && selected_set.count(act.action_id) > 0)) {
      to_apply.push_back(act);
    } else {
      skipped.push_back(act.action_id);
    }
  }

  RepairApplyResult result{};
  result.plan_id = current_plan.plan_id;
  result.dry_run = dry_run;
  result.pre_repair_sha256 = current_sha;
  result.issues_before = current_plan.initial_check.total_issues;
  result.skipped_action_ids = skipped;

  if (dry_run) {
    result.post_repair_sha256 = current_sha;
    result.issues_remaining = current_plan.initial_check.total_issues;
    for (const auto& a : to_apply) {
      result.applied_action_ids.push_back(a.action_id);
      result.repaired_structures.push_back(a.target_structure);
    }
    result.post_check = current_plan.initial_check;
    return result;
  }

  if (!confirm) {
    throw std::runtime_error(
        "ERR_CONFIRMATION_REQUIRED: Explicit confirmation is required to apply repairs.");
  }

  // Save pre-repair snapshot for before/after comparison
  const std::string snap_name = exp.experiment_id + ".pre_repair.gfs";
  const auto snap_path = resolve_workspace_path(sb_dir, snap_name);
  std::filesystem::copy_file(sb_path, snap_path, std::filesystem::copy_options::overwrite_existing);
  exp.pre_repair_snapshot_filename = snap_name;
  exp.pre_repair_sha256 = current_sha;
  result.pre_repair_snapshot_filename = snap_name;
  CorruptionEngine::save_experiment(workspace, exp);

  bool wants_clean_state = false;

  {
    VirtualDisk disk;
    disk.open_image(sb_path, 512U, true);
    Superblock sb = read_best_superblock(disk);
    const uint32_t bs = sb.block_size;

    for (const auto& act : to_apply) {
      if (act.action_type == "restore_primary_superblock" ||
          act.action_type == "restore_backup_superblock" ||
          act.action_type == "sync_superblock_replica") {
        write_both_superblocks(disk, sb);
        result.applied_action_ids.push_back(act.action_id);
        result.repaired_structures.push_back(act.target_structure);
      } else if (act.action_type == "reset_alloc_policy") {
        sb.alloc_policy = static_cast<uint8_t>(AllocPolicy::FirstFit);
        write_both_superblocks(disk, sb);
        result.applied_action_ids.push_back(act.action_id);
        result.repaired_structures.push_back(act.target_structure);
      } else if (act.action_type == "abort_corrupt_journal_slot") {
        const uint64_t off = act.byte_offsets.empty() ? 0ULL : act.byte_offsets[0];
        const uint32_t blk = static_cast<uint32_t>(off / bs);
        const uint32_t in_blk = static_cast<uint32_t>(off % bs);
        if (blk >= sb.journal_start && blk < sb.journal_start + sb.journal_blocks) {
          std::vector<uint8_t> jbuf;
          disk.read_block(blk, jbuf);
          JournalEntry je{};
          bool jcrc = false;
          deserialize_journal_entry(jbuf.data() + in_blk, je, jcrc);
          je.status = static_cast<uint8_t>(JournalStatus::Aborted);
          serialize_journal_entry(je, jbuf.data() + in_blk);
          disk.write_block(blk, jbuf);
          result.applied_action_ids.push_back(act.action_id);
          result.repaired_structures.push_back(act.target_structure);
        }
      } else if (act.action_type == "reconcile_journal_dirty_state") {
        wants_clean_state = true;
        result.applied_action_ids.push_back(act.action_id);
        result.repaired_structures.push_back(act.target_structure);
      } else if (act.action_type == "reconstruct_block_bitmap" ||
                 act.action_type == "reclaim_unreferenced_block") {
        // Recompute full expected block bitmap from validated inodes (never touches ambiguous/duplicate blocks unless safe)
        for (const auto& iss : current_plan.initial_check.issues) {
          if (iss.issue_id == act.issue_id) {
            const uint64_t rel = iss.byte_offset - static_cast<uint64_t>(sb.block_bitmap_start) * bs;
            const uint32_t byte_idx = static_cast<uint32_t>(rel);
            const uint32_t start_b = byte_idx * 8U;
            // Re-scan which bits in [start_b .. start_b+7] are owned by allocated inodes
            std::set<uint32_t> owned;
            for (uint32_t i = 1; i < sb.total_inodes; ++i) {
              bool ok = false;
              const Inode ino = read_inode(disk, sb, i, ok);
              if (ino.state != static_cast<uint8_t>(InodeState::Allocated)) continue;
              for (uint32_t d = 0; d < kDirectBlockCount; ++d) {
                if (ino.direct_blocks[d] >= sb.data_region_start && ino.direct_blocks[d] < sb.journal_start) {
                  owned.insert(ino.direct_blocks[d]);
                }
              }
              if (ino.indirect_block >= sb.data_region_start && ino.indirect_block < sb.journal_start) {
                owned.insert(ino.indirect_block);
                std::vector<uint8_t> ibuf;
                disk.read_block(ino.indirect_block, ibuf);
                for (uint32_t p = 0; p < bs / 4U; ++p) {
                  const uint32_t dp = read_u32_le(ibuf.data() + p * 4U);
                  if (dp >= sb.data_region_start && dp < sb.journal_start) owned.insert(dp);
                }
              }
            }
            for (uint32_t bit = 0; bit < 8U; ++bit) {
              const uint32_t b = start_b + bit;
              if (b >= sb.data_region_start && b < sb.journal_start) {
                set_bitmap_bit_and_write(disk, sb.block_bitmap_start, b, owned.count(b) > 0);
              }
            }
            break;
          }
        }
        recount_free_counts_and_save_sb(disk, sb);
        result.applied_action_ids.push_back(act.action_id);
        result.repaired_structures.push_back(act.target_structure);
      } else if (act.action_type == "reconstruct_inode_bitmap" ||
                 act.action_type == "clear_leaked_inode_bitmap") {
        for (const auto& iss : current_plan.initial_check.issues) {
          if (iss.issue_id == act.issue_id && iss.affected_inode < sb.total_inodes) {
            const bool val = (act.action_type == "reconstruct_inode_bitmap");
            set_bitmap_bit_and_write(disk, sb.inode_bitmap_start, iss.affected_inode, val);
            break;
          }
        }
        recount_free_counts_and_save_sb(disk, sb);
        result.applied_action_ids.push_back(act.action_id);
        result.repaired_structures.push_back(act.target_structure);
      } else if (act.action_type == "remove_dangling_dir_entry" ||
                 act.action_type == "remove_duplicate_dir_entry" ||
                 act.action_type == "break_directory_cycle") {
        const uint64_t off = act.byte_offsets.empty() ? 0ULL : act.byte_offsets[0];
        const uint32_t blk = static_cast<uint32_t>(off / bs);
        const uint32_t in_blk = static_cast<uint32_t>(off % bs);
        std::vector<uint8_t> dbuf;
        disk.read_block(blk, dbuf);
        DirEntry de{};
        deserialize_dir_entry(dbuf.data() + in_blk, de);
        de.state = static_cast<uint8_t>(DirEntryState::Deleted);
        serialize_dir_entry(de, dbuf.data() + in_blk);
        disk.write_block(blk, dbuf);
        result.applied_action_ids.push_back(act.action_id);
        result.repaired_structures.push_back(act.target_structure);
      } else if (act.action_type == "fix_parent_inode") {
        for (const auto& iss : current_plan.initial_check.issues) {
          if (iss.issue_id == act.issue_id && iss.affected_inode < sb.total_inodes) {
            bool ok = false;
            Inode ino = read_inode(disk, sb, iss.affected_inode, ok);
            ino.parent_inode = static_cast<uint32_t>(std::stoul(iss.expected_value));
            write_inode(disk, sb, ino);
            break;
          }
        }
        result.applied_action_ids.push_back(act.action_id);
        result.repaired_structures.push_back(act.target_structure);
      } else if (act.action_type == "reconnect_orphan_inode") {
        for (const auto& iss : current_plan.initial_check.issues) {
          if (iss.issue_id == act.issue_id && iss.affected_inode < sb.total_inodes) {
            const uint32_t lf_ino_id = ensure_lost_and_found(disk, sb);
            bool lf_ok = false;
            Inode lf_ino = read_inode(disk, sb, lf_ino_id, lf_ok);
            bool orph_ok = false;
            Inode orph = read_inode(disk, sb, iss.affected_inode, orph_ok);

            std::vector<uint8_t> lf_buf;
            disk.read_block(lf_ino.direct_blocks[0], lf_buf);
            const uint32_t slots = bs / static_cast<uint32_t>(kDirEntryBytes);
            std::set<std::string> used_names;
            uint32_t free_slot = UINT32_MAX;
            for (uint32_t s = 0; s < slots; ++s) {
              DirEntry de{};
              deserialize_dir_entry(lf_buf.data() + s * kDirEntryBytes, de);
              if (de.state == static_cast<uint8_t>(DirEntryState::Active)) {
                used_names.insert(de.name);
              } else if (free_slot == UINT32_MAX) {
                free_slot = s;
              }
            }
            if (free_slot != UINT32_MAX) {
              std::string base = "inode_" + std::to_string(orph.inode_id);
              std::string name = base;
              int suffix = 1;
              while (used_names.count(name) > 0) {
                name = base + "_" + std::to_string(suffix++);
              }
              DirEntry new_de{};
              new_de.inode_id = orph.inode_id;
              new_de.entry_type = orph.type;
              new_de.state = static_cast<uint8_t>(DirEntryState::Active);
              new_de.name = name;
              new_de.name_len = static_cast<uint16_t>(name.size());
              serialize_dir_entry(new_de, lf_buf.data() + free_slot * kDirEntryBytes);
              disk.write_block(lf_ino.direct_blocks[0], lf_buf);

              orph.parent_inode = lf_ino_id;
              write_inode(disk, sb, orph);
              recount_free_counts_and_save_sb(disk, sb);
              result.reconnected_paths.push_back("/lost+found/" + name);
            }
            break;
          }
        }
        result.applied_action_ids.push_back(act.action_id);
        result.repaired_structures.push_back(act.target_structure);
      }
    }

    disk.sync();
    disk.close_image();
  }

  // If Dirty state reconciliation was requested, check whether all metadata errors are now resolved
  if (wants_clean_state) {
    const FsckResult mid_check = GfsckEngine::check(sb_path, "full");
    uint32_t remaining_meta_issues = 0;
    for (const auto& iss : mid_check.issues) {
      if (iss.issue_type == "DIRTY_FILESYSTEM_STATE" || iss.issue_type == "PAYLOAD_CRC_MISMATCH") {
        continue;
      }
      if (iss.severity == "error" || iss.severity == "critical") {
        remaining_meta_issues++;
      }
    }
    if (remaining_meta_issues == 0) {
      VirtualDisk disk;
      disk.open_image(sb_path, 512U, true);
      Superblock sb = read_best_superblock(disk);
      sb.fs_state = static_cast<uint8_t>(FsState::Clean);
      write_both_superblocks(disk, sb);
      disk.sync();
      disk.close_image();
    }
  }

  result.post_repair_sha256 = compute_file_sha256_streaming(sb_path);
  result.post_check = GfsckEngine::check(sb_path, "full");
  result.issues_remaining = result.post_check.total_issues;
  result.issues_resolved =
      (result.issues_before > result.issues_remaining) ? (result.issues_before - result.issues_remaining) : 0U;

  // Compute actual bytes/blocks changed between pre_repair snapshot and post_repair sandbox
  const auto cmp = compare_experiment(workspace, experiment_id, "corrupted_vs_repaired");
  result.bytes_changed = cmp.total_bytes_changed;
  result.blocks_changed = cmp.changed_blocks;

  exp.current_sandbox_sha256 = result.post_repair_sha256;
  CorruptionEngine::save_experiment(workspace, exp);

  return result;
}

ExperimentComparisonResult RepairEngine::compare_experiment(
    const std::filesystem::path& workspace,
    const std::string& experiment_id,
    const std::string& compare_mode) {
  const SandboxExperiment exp = CorruptionEngine::get_experiment(workspace, experiment_id);
  const auto sb_dir = CorruptionEngine::sandbox_dir(workspace);
  const auto orig_path = resolve_workspace_path(workspace / "disk_images", exp.source_disk_name);
  const auto cur_path = resolve_workspace_path(sb_dir, exp.sandbox_filename);

  std::filesystem::path src_p = orig_path;
  std::filesystem::path dst_p = cur_path;
  std::string src_label = "Original (" + exp.source_disk_name + ")";
  std::string dst_label = "Current Sandbox (" + exp.sandbox_filename + ")";

  if (compare_mode == "corrupted_vs_repaired") {
    if (exp.pre_repair_snapshot_filename.empty()) {
      throw std::runtime_error("ERR_NOT_FOUND: No pre-repair snapshot exists yet; run repair first.");
    }
    src_p = resolve_workspace_path(sb_dir, exp.pre_repair_snapshot_filename);
    src_label = "Pre-Repair Sandbox";
    dst_label = "Repaired Sandbox";
  } else if (compare_mode == "original_vs_repaired") {
    src_label = "Original (" + exp.source_disk_name + ")";
    dst_label = "Repaired Sandbox";
  }

  ExperimentComparisonResult res{};
  res.experiment_id = experiment_id;
  res.mode = compare_mode;
  res.source_label = src_label;
  res.target_label = dst_label;
  res.source_sha256 = compute_file_sha256_streaming(src_p);
  res.target_sha256 = compute_file_sha256_streaming(dst_p);
  res.identical = (res.source_sha256 == res.target_sha256);

  res.source_check = GfsckEngine::check(src_p, "full");
  res.target_check = GfsckEngine::check(dst_p, "full");

  VirtualDisk d1, d2;
  d1.open_image(src_p, 512U, false);
  d2.open_image(dst_p, 512U, false);
  const uint32_t bs = kDefaultBlockSize;
  d1.set_block_size(bs);
  d2.set_block_size(bs);

  const uint32_t max_blks = std::min(d1.total_blocks(), d2.total_blocks());
  std::vector<uint8_t> b1, b2;
  for (uint32_t b = 0; b < max_blks; ++b) {
    d1.read_block(b, b1);
    d2.read_block(b, b2);
    uint32_t diffs = 0;
    size_t first_diff = 0;
    for (size_t i = 0; i < bs; ++i) {
      if (b1[i] != b2[i]) {
        if (diffs == 0) first_diff = i;
        diffs++;
      }
    }
    if (diffs > 0) {
      res.total_bytes_changed += diffs;
      res.changed_blocks.push_back(b);
      if (res.block_diffs.size() < 64) {
        BlockChangeSummary s{};
        s.block_id = b;
        s.byte_offset = static_cast<uint64_t>(b) * bs + first_diff;
        s.bytes_differing = diffs;
        s.sample_before_hex = bytes_hex_sample(b1.data() + first_diff, bs - first_diff);
        s.sample_after_hex = bytes_hex_sample(b2.data() + first_diff, bs - first_diff);
        res.block_diffs.push_back(std::move(s));
      }
    }
  }
  return res;
}

}  // namespace ghostfs
