#include "ghostfs/fsck.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ghostfs/crc32.hpp"
#include "ghostfs/endian.hpp"
#include "ghostfs/gfs_format.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/virtual_disk.hpp"

namespace ghostfs {
using endian::read_u32_le;
namespace {

struct BlockOwnerRef {
  uint32_t inode_id = 0;
  std::string role;  // "file_data" | "dir_data" | "indirect" | "double_indirect"
};

std::string hex32(uint32_t v) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::uppercase << v;
  return oss.str();
}

bool get_bitmap_bit_safe(
    const VirtualDisk& disk,
    uint32_t bitmap_start,
    uint32_t bitmap_blocks,
    uint32_t bit_index) {
  const uint32_t bs = disk.block_size();
  const uint32_t bits_per_blk = bs * 8U;
  const uint32_t rel_blk = bit_index / bits_per_blk;
  if (rel_blk >= bitmap_blocks) return false;
  const uint32_t blk = bitmap_start + rel_blk;
  if (blk >= disk.total_blocks()) return false;
  std::vector<uint8_t> buf;
  disk.read_block(blk, buf);
  const uint32_t in_blk = bit_index % bits_per_blk;
  return (buf[in_blk / 8U] & static_cast<uint8_t>(1U << (in_blk % 8U))) != 0;
}

bool validate_geometry(const Superblock& sb, uint64_t file_size_bytes) {
  if (!is_supported_block_size(sb.block_size)) return false;
  if (sb.total_blocks < kMinDiskBlocks) return false;
  if (sb.total_blocks > kMaxDiskSizeBytes / sb.block_size) return false;
  if (sb.reserved_blocks != 3U) return false;
  if (sb.inode_bitmap_start != 3U || sb.inode_bitmap_blocks == 0U) return false;
  if (sb.block_bitmap_start != sb.inode_bitmap_start + sb.inode_bitmap_blocks ||
      sb.block_bitmap_blocks == 0U) return false;
  if (sb.inode_table_start != sb.block_bitmap_start + sb.block_bitmap_blocks ||
      sb.inode_table_blocks == 0U) return false;
  if (sb.data_region_start != sb.inode_table_start + sb.inode_table_blocks) return false;
  if (sb.journal_start <= sb.data_region_start || sb.journal_blocks == 0U) return false;
  if (static_cast<uint64_t>(sb.journal_start) + sb.journal_blocks != sb.total_blocks) return false;
  if (sb.data_blocks_count != sb.journal_start - sb.data_region_start) return false;
  if (sb.total_inodes == 0U) return false;
  (void)file_size_bytes;
  return true;
}

}  // namespace

FsckResult GfsckEngine::check(
    const std::filesystem::path& disk_path,
    const std::string& mode) {
  if (mode != "fast" && mode != "full") {
    throw std::runtime_error("ERR_INVALID_ARGUMENT: Check mode must be 'fast' or 'full'");
  }
  if (!std::filesystem::exists(disk_path) || !std::filesystem::is_regular_file(disk_path)) {
    throw std::runtime_error("ERR_NOT_FOUND: Disk image not found: " + disk_path.string());
  }

  FsckResult res{};
  res.disk_name = disk_path.filename().string();
  res.disk_sha256 = compute_file_sha256_streaming(disk_path);
  res.check_mode = mode;

  uint32_t next_issue_num = 1;
  auto add_issue = [&](FsckIssue iss) {
    iss.issue_id = "iss_" + std::to_string(next_issue_num++);
    if (iss.severity == "information") res.info_count++;
    else if (iss.severity == "warning") res.warning_count++;
    else if (iss.severity == "error") res.error_count++;
    else if (iss.severity == "critical") res.critical_count++;
    res.total_issues++;
    res.issues.push_back(std::move(iss));
  };

  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  const uint64_t actual_file_size = disk.file_size_bytes();
  if (actual_file_size < kBootBlockBytes) {
    add_issue(FsckIssue{
        "", "TRUNCATED_IMAGE", "critical",
        "Disk image is smaller than a 64-byte GFS1 BootBlock.",
        "boot_block", 0, 0, 0,
        ">= 64 bytes", std::to_string(actual_file_size) + " bytes",
        "File size check failed before BootBlock read.",
        "not_repairable", "Cannot repair severely truncated image.", "Complete data loss"});
    res.consistency_status = "unrecoverable_geometry";
    return res;
  }

  std::vector<uint8_t> boot_raw;
  disk.read_raw(0, kBootBlockBytes, boot_raw);
  BootBlock boot{};
  bool boot_crc_ok = false;
  const bool boot_magic_ok = deserialize_boot_block(boot_raw.data(), boot, boot_crc_ok);

  uint32_t bs = is_supported_block_size(boot.block_size) ? boot.block_size : kDefaultBlockSize;
  if (!boot_magic_ok || !boot_crc_ok) {
    // Probe block sizes for valid superblock if boot block is damaged
    for (uint32_t cand_bs : {4096U, 2048U, 1024U, 512U}) {
      if (actual_file_size >= static_cast<uint64_t>(cand_bs) * 3ULL) {
        std::vector<uint8_t> probe;
        disk.read_raw(cand_bs, kSuperblockBytes, probe);
        Superblock psb{};
        bool pcrc = false;
        if (deserialize_superblock(probe.data(), psb, pcrc) && pcrc) {
          bs = cand_bs;
          break;
        }
      }
    }
  }
  disk.set_block_size(bs);

  if (actual_file_size < static_cast<uint64_t>(bs) * 3ULL) {
    add_issue(FsckIssue{
        "", "TRUNCATED_IMAGE", "critical",
        "Disk image truncated before primary and backup superblocks.",
        "superblock", 1, 0, bs,
        ">= " + std::to_string(bs * 3ULL) + " bytes", std::to_string(actual_file_size) + " bytes",
        "File ends before block 3.",
        "not_repairable", "Insufficient disk bytes to locate superblocks.", "Complete data loss"});
    res.consistency_status = "unrecoverable_geometry";
    return res;
  }

  std::vector<uint8_t> sb1_raw, sb2_raw;
  disk.read_block(1, sb1_raw);
  disk.read_block(2, sb2_raw);

  Superblock sb1{}, sb2{};
  bool sb1_crc_ok = false, sb2_crc_ok = false;
  const bool sb1_magic_ok = deserialize_superblock(sb1_raw.data(), sb1, sb1_crc_ok);
  const bool sb2_magic_ok = deserialize_superblock(sb2_raw.data(), sb2, sb2_crc_ok);

  const bool sb1_geom_ok = sb1_magic_ok && sb1_crc_ok && validate_geometry(sb1, actual_file_size);
  const bool sb2_geom_ok = sb2_magic_ok && sb2_crc_ok && validate_geometry(sb2, actual_file_size);

  res.primary_sb_valid = sb1_geom_ok;
  res.backup_sb_valid = sb2_geom_ok;

  if (!sb1_geom_ok && !sb2_geom_ok) {
    add_issue(FsckIssue{
        "", "DUAL_SUPERBLOCK_UNRECOVERABLE", "critical",
        "Both primary (block 1) and backup (block 2) superblocks failed validation; trustworthy GFS1 geometry cannot be established.",
        "superblock", 1, 0, bs,
        "Valid GFS1 magic + CRC32 + geometry in block 1 or block 2",
        "Primary valid=" + std::string(sb1_geom_ok ? "true" : "false") +
            ", Backup valid=" + std::string(sb2_geom_ok ? "true" : "false"),
        "Neither superblock replica passed magic, CRC32, and region-boundary checks. Refusing to invent file-system geometry.",
        "not_repairable",
        "Manual forensic carving only (use Recovery Center Deep Scan).",
        "File-system structure cannot be safely mounted or repaired"});
    res.consistency_status = "unrecoverable_geometry";
    return res;
  }

  const Superblock& sb = sb1_geom_ok ? sb1 : sb2;
  if (!sb1_geom_ok) {
    add_issue(FsckIssue{
        "", "PRIMARY_SUPERBLOCK_CORRUPT", "critical",
        "Primary superblock (block 1) failed validation, but valid backup superblock (block 2) is available.",
        "primary_superblock", 1, 0, bs,
        "magic=GFS1, valid CRC32 & geometry",
        "magic=" + hex32(read_u32_le(sb1_raw.data())) + ", crc_ok=" + std::string(sb1_crc_ok ? "true" : "false"),
        "Backup superblock at block 2 passed magic, CRC32, and geometry validation.",
        "automatically_repairable",
        "Restore primary superblock (block 1) from valid backup superblock (block 2).",
        "None — backup replica is intact"});
  }
  if (!sb2_geom_ok) {
    add_issue(FsckIssue{
        "", "BACKUP_SUPERBLOCK_CORRUPT", "error",
        "Backup superblock (block 2) failed validation while primary superblock (block 1) is valid.",
        "backup_superblock", 2, 0, static_cast<uint64_t>(bs) * 2ULL,
        "magic=GFS1, valid CRC32 & geometry",
        "magic=" + hex32(read_u32_le(sb2_raw.data())) + ", crc_ok=" + std::string(sb2_crc_ok ? "true" : "false"),
        "Primary superblock at block 1 passed magic, CRC32, and geometry validation.",
        "automatically_repairable",
        "Restore backup superblock (block 2) from valid primary superblock (block 1).",
        "None — primary replica is intact"});
  }
  if (sb1_geom_ok && sb2_geom_ok) {
    if (sb1.block_size != sb2.block_size || sb1.total_blocks != sb2.total_blocks ||
        sb1.total_inodes != sb2.total_inodes || sb1.data_region_start != sb2.data_region_start ||
        sb1.journal_start != sb2.journal_start) {
      add_issue(FsckIssue{
          "", "SUPERBLOCK_REPLICA_MISMATCH", "error",
          "Primary and backup superblocks have differing geometry fields.",
          "superblock", 2, 0, static_cast<uint64_t>(bs) * 2ULL,
          "total_blocks=" + std::to_string(sb1.total_blocks),
          "backup total_blocks=" + std::to_string(sb2.total_blocks),
          "Primary and backup superblock structural fields diverge.",
          "repairable_with_confirmation",
          "Synchronize backup superblock from primary superblock after verifying disk size.",
          "Low if primary matches actual image size"});
    }
  }

  // Check truncation against superblock total_blocks
  const uint64_t expected_bytes = static_cast<uint64_t>(sb.total_blocks) * bs;
  if (actual_file_size < expected_bytes) {
    add_issue(FsckIssue{
        "", "TRUNCATED_IMAGE", "critical",
        "Disk image file size is smaller than superblock total_blocks * block_size.",
        "disk_geometry", sb.total_blocks - 1U, 0, actual_file_size,
        std::to_string(expected_bytes) + " bytes (" + std::to_string(sb.total_blocks) + " blocks)",
        std::to_string(actual_file_size) + " bytes (" + std::to_string(disk.total_blocks()) + " blocks)",
        "Trailing blocks [" + std::to_string(disk.total_blocks()) + ".." + std::to_string(sb.total_blocks - 1U) + "] are missing.",
        "not_repairable",
        "Cannot regenerate truncated blocks without original image.",
        "High — missing trailing blocks / journal"});
  } else {
    res.geometry_valid = true;
  }

  if (sb.alloc_policy > static_cast<uint8_t>(AllocPolicy::BitmapNextFit)) {
    add_issue(FsckIssue{
        "", "INVALID_ALLOC_POLICY", "warning",
        "Superblock alloc_policy is out of range.",
        "superblock", 1, 0, bs + 72,
        "0..3", std::to_string(sb.alloc_policy),
        "Unsupported allocation policy enum value.",
        "automatically_repairable",
        "Reset alloc_policy to FirstFit (0).",
        "None"});
  }

  // Dirty / Journal inspection
  if (sb.fs_state == static_cast<uint8_t>(FsState::Dirty)) {
    res.dirty_journal = true;
    add_issue(FsckIssue{
        "", "DIRTY_FILESYSTEM_STATE", "error",
        "Superblock fs_state is marked Dirty (uncommitted metadata transaction detected).",
        "superblock", 1, 0, bs + 73,
        "Clean (1)", "Dirty (2)",
        "Superblock fs_state == 2 indicates an interrupted write or unrecovered crash state.",
        "repairable_with_confirmation",
        "Run conservative consistency reconstruction and mark volume Clean only after all metadata checks pass.",
        "Low — incomplete in-flight write may remain unlinked"});
  }

  const uint32_t max_readable_blk = disk.total_blocks();
  if (sb.journal_start < max_readable_blk) {
    const uint32_t j_end = std::min(sb.journal_start + sb.journal_blocks, max_readable_blk);
    const uint32_t j_slots = bs / static_cast<uint32_t>(kJournalEntryBytes);
    for (uint32_t b = sb.journal_start; b < j_end; ++b) {
      std::vector<uint8_t> jbuf;
      disk.read_block(b, jbuf);
      for (uint32_t s = 0; s < j_slots; ++s) {
        const uint8_t* ptr = jbuf.data() + s * kJournalEntryBytes;
        bool all_zero = true;
        for (size_t k = 0; k < kJournalEntryBytes; ++k) {
          if (ptr[k] != 0) {
            all_zero = false;
            break;
          }
        }
        if (all_zero) continue;
        JournalEntry je{};
        bool jcrc_ok = false;
        deserialize_journal_entry(ptr, je, jcrc_ok);
        if (!jcrc_ok) {
          add_issue(FsckIssue{
              "", "INVALID_JOURNAL_CHECKSUM", "warning",
              "Journal entry failed CRC32 verification.",
              "journal", b, je.target_inode, static_cast<uint64_t>(b) * bs + s * kJournalEntryBytes,
              "Valid CRC32", "Mismatch (" + hex32(je.crc32) + ")",
              "Corrupt 64-byte journal slot; cannot trust this intent record for replay.",
              "repairable_with_confirmation",
              "Abort/clear corrupt journal slot during consistency reconstruction.",
              "None to metadata; in-flight operation cannot be replayed"});
        } else if (je.status == static_cast<uint8_t>(JournalStatus::Pending)) {
          res.dirty_journal = true;
          add_issue(FsckIssue{
              "", "PENDING_JOURNAL_ENTRY", "error",
              "Pending uncommitted journal intent record (seq=" + std::to_string(je.seq_no) +
                  ", op=" + std::to_string(je.op_type) + ", summary=" + je.summary + ").",
              "journal", b, je.target_inode, static_cast<uint64_t>(b) * bs + s * kJournalEntryBytes,
              "Committed (2)", "Pending (1)",
              "GFS1 64-byte journal entries record intent only (no physical before/after images for WAL redo/undo).",
              "repairable_with_confirmation",
              "Reconcile bitmaps/metadata conservatively and mark journal entry Aborted.",
              "Uncommitted operation may be rolled back"});
        }
      }
    }
  }

  // Read all inodes & build block ownership map
  std::vector<Inode> inodes(sb.total_inodes);
  std::vector<bool> inode_crc_valid(sb.total_inodes, false);
  const uint32_t inodes_per_blk = bs / static_cast<uint32_t>(kInodeBytes);
  for (uint32_t i = 0; i < sb.total_inodes; ++i) {
    const uint32_t blk = sb.inode_table_start + (i / inodes_per_blk);
    if (blk >= max_readable_blk) break;
    std::vector<uint8_t> ibuf;
    disk.read_block(blk, ibuf);
    const uint32_t off = (i % inodes_per_blk) * static_cast<uint32_t>(kInodeBytes);
    bool crc_ok = false;
    deserialize_inode(ibuf.data() + off, inodes[i], crc_ok);
    inode_crc_valid[i] = crc_ok;
  }

  std::map<uint32_t, std::vector<BlockOwnerRef>> block_owners;

  for (uint32_t i = 1; i < sb.total_inodes; ++i) {
    const Inode& ino = inodes[i];
    const uint32_t blk = sb.inode_table_start + (i / inodes_per_blk);
    const uint64_t off = static_cast<uint64_t>(blk) * bs + (i % inodes_per_blk) * kInodeBytes;

    if (ino.state == static_cast<uint8_t>(InodeState::Allocated)) {
      if (!inode_crc_valid[i]) {
        add_issue(FsckIssue{
            "", "INVALID_INODE_CHECKSUM", "error",
            "Allocated inode #" + std::to_string(i) + " failed CRC32 verification.",
            "inode", blk, i, off,
            "Valid CRC32", hex32(ino.inode_crc32),
            "Inode metadata checksum mismatch; fields may have been tampered with.",
            "ambiguous_manual_required",
            "Inspect inode #" + std::to_string(i) + " in Hex Viewer before trusting its pointers.",
            "Potential metadata/file loss"});
      }
      const std::string role =
          (ino.type == static_cast<uint8_t>(InodeType::Directory)) ? "dir_data" : "file_data";

      uint32_t valid_data_ptrs = 0;
      auto check_data_ptr = [&](uint32_t ptr) {
        if (ptr == 0) return;
        if (ptr < sb.data_region_start || ptr >= sb.journal_start) {
          add_issue(FsckIssue{
              "", "OUT_OF_RANGE_BLOCK_PTR", "error",
              "Inode #" + std::to_string(i) + " references out-of-range data block #" + std::to_string(ptr) + ".",
              "inode", blk, i, off,
              "[" + std::to_string(sb.data_region_start) + ".." + std::to_string(sb.journal_start - 1) + "]",
              std::to_string(ptr),
              "Block pointer points outside data region.",
              "ambiguous_manual_required",
              "Inspect inode #" + std::to_string(i) + "; clearing invalid pointer may lose file data.",
              "Data loss for affected block"});
        } else {
          valid_data_ptrs++;
          block_owners[ptr].push_back({i, role});
        }
      };

      for (uint32_t d = 0; d < kDirectBlockCount; ++d) {
        check_data_ptr(ino.direct_blocks[d]);
      }

      if (ino.indirect_block != 0) {
        if (ino.indirect_block < sb.data_region_start || ino.indirect_block >= sb.journal_start ||
            ino.indirect_block >= max_readable_blk) {
          add_issue(FsckIssue{
              "", "INVALID_INDIRECT_PTR", "error",
              "Inode #" + std::to_string(i) + " has out-of-range single-indirect block #" +
                  std::to_string(ino.indirect_block) + ".",
              "inode", blk, i, off,
              "[" + std::to_string(sb.data_region_start) + ".." + std::to_string(sb.journal_start - 1) + "]",
              std::to_string(ino.indirect_block),
              "Single-indirect block pointer outside valid data region.",
              "ambiguous_manual_required",
              "Inspect inode #" + std::to_string(i) + " indirect pointer.",
              "High — indirect data blocks unreachable"});
        } else {
          block_owners[ino.indirect_block].push_back({i, "indirect"});
          std::vector<uint8_t> ibuf;
          disk.read_block(ino.indirect_block, ibuf);
          const uint32_t ptrs = bs / 4U;
          for (uint32_t p = 0; p < ptrs; ++p) {
            const uint32_t dp = read_u32_le(ibuf.data() + p * 4U);
            if (dp != 0) check_data_ptr(dp);
          }
        }
      }

      if (ino.double_indirect_block != 0) {
        if (ino.double_indirect_block < sb.data_region_start ||
            ino.double_indirect_block >= sb.journal_start ||
            ino.double_indirect_block >= max_readable_blk) {
          add_issue(FsckIssue{
              "", "INVALID_INDIRECT_PTR", "error",
              "Inode #" + std::to_string(i) + " has out-of-range double-indirect block #" +
                  std::to_string(ino.double_indirect_block) + ".",
              "inode", blk, i, off,
              "Valid data region block", std::to_string(ino.double_indirect_block),
              "Double-indirect pointer out of bounds.",
              "ambiguous_manual_required",
              "Inspect double-indirect pointer.",
              "High"});
        } else {
          block_owners[ino.double_indirect_block].push_back({i, "double_indirect"});
          std::vector<uint8_t> dibuf;
          disk.read_block(ino.double_indirect_block, dibuf);
          const uint32_t ptrs = bs / 4U;
          for (uint32_t p = 0; p < ptrs; ++p) {
            const uint32_t s_blk = read_u32_le(dibuf.data() + p * 4U);
            if (s_blk == 0) continue;
            if (s_blk < sb.data_region_start || s_blk >= sb.journal_start || s_blk >= max_readable_blk) {
              add_issue(FsckIssue{
                  "", "INVALID_INDIRECT_PTR", "error",
                  "Double-indirect table on inode #" + std::to_string(i) + " points to invalid block #" +
                      std::to_string(s_blk),
                  "indirect_block", ino.double_indirect_block, i,
                  static_cast<uint64_t>(ino.double_indirect_block) * bs + p * 4U,
                  "Valid data block", std::to_string(s_blk),
                  "Nested indirect pointer out of range.",
                  "ambiguous_manual_required", "Inspect indirect block.", "High"});
            } else {
              block_owners[s_blk].push_back({i, "indirect"});
              std::vector<uint8_t> sibuf;
              disk.read_block(s_blk, sibuf);
              for (uint32_t q = 0; q < ptrs; ++q) {
                const uint32_t dp = read_u32_le(sibuf.data() + q * 4U);
                if (dp != 0) check_data_ptr(dp);
              }
            }
          }
        }
      }

      if (ino.type == static_cast<uint8_t>(InodeType::File)) {
        const uint64_t max_bytes_for_ptrs = static_cast<uint64_t>(valid_data_ptrs) * bs;
        if (ino.size_bytes > max_bytes_for_ptrs) {
          add_issue(FsckIssue{
              "", "INCONSISTENT_FILE_SIZE", "error",
              "Inode #" + std::to_string(i) + " size_bytes (" + std::to_string(ino.size_bytes) +
                  ") exceeds allocated block capacity (" + std::to_string(max_bytes_for_ptrs) + " B).",
              "inode", blk, i, off,
              "<= " + std::to_string(max_bytes_for_ptrs), std::to_string(ino.size_bytes),
              "Logical file size exceeds mapped data blocks.",
              "ambiguous_manual_required",
              "Verify true file length before truncating size_bytes.",
              "Tail truncation risk"});
        }
      }
    }

    // Inode bitmap consistency check
    const bool ino_bmp = get_bitmap_bit_safe(disk, sb.inode_bitmap_start, sb.inode_bitmap_blocks, i);
    const bool ino_alloc = (ino.state == static_cast<uint8_t>(InodeState::Allocated));
    if (ino_alloc && !ino_bmp) {
      add_issue(FsckIssue{
          "", "ALLOCATED_INODE_MARKED_FREE", "error",
          "Active inode #" + std::to_string(i) + " is marked Free (0) in inode bitmap.",
          "inode_bitmap", sb.inode_bitmap_start, i,
          static_cast<uint64_t>(sb.inode_bitmap_start) * bs + (i / 8U),
          "1 (Allocated)", "0 (Free)",
          "Inode table slot has state=Allocated while inode_bitmap bit is 0.",
          "automatically_repairable",
          "Reconstruct inode bitmap bit to 1 from validated inode table.",
          "None — prevents future inode reuse collision"});
    } else if (!ino_alloc && ino_bmp) {
      add_issue(FsckIssue{
          "", "UNALLOCATED_INODE_MARKED_ACTIVE", "warning",
          "Free/Deleted inode #" + std::to_string(i) + " is marked Allocated (1) in inode bitmap.",
          "inode_bitmap", sb.inode_bitmap_start, i,
          static_cast<uint64_t>(sb.inode_bitmap_start) * bs + (i / 8U),
          "0 (Free)", "1 (Allocated)",
          "Inode table slot is not Allocated while inode_bitmap bit is 1.",
          "repairable_with_confirmation",
          "Clear leaked inode bitmap bit to 0.",
          "None"});
    }
  }

  // Duplicate block ownership & Block bitmap consistency
  for (const auto& [blk_id, owners] : block_owners) {
    if (owners.size() > 1U) {
      std::ostringstream oss;
      for (size_t k = 0; k < owners.size(); ++k) {
        if (k > 0) oss << ", ";
        oss << "inode #" << owners[k].inode_id << " (" << owners[k].role << ")";
      }
      add_issue(FsckIssue{
          "", "DUPLICATE_BLOCK_OWNERSHIP", "critical",
          "Data block #" + std::to_string(blk_id) + " is claimed by multiple inodes: " + oss.str() + ".",
          "block_ownership", blk_id, owners[0].inode_id, static_cast<uint64_t>(blk_id) * bs,
          "Single owning inode", oss.str(),
          "Multiple allocated inodes point to the same physical block; automatic owner selection could destroy valid data.",
          "ambiguous_manual_required",
          "Inspect both inodes and block #" + std::to_string(blk_id) + " in Hex Viewer; do not arbitrarily free.",
          "High — one or more files share overwritten/cross-linked blocks"});
    }
  }

  for (uint32_t b = sb.data_region_start; b < std::min(sb.journal_start, max_readable_blk); ++b) {
    const bool bmp_used = get_bitmap_bit_safe(disk, sb.block_bitmap_start, sb.block_bitmap_blocks, b);
    const bool has_owner = (block_owners.find(b) != block_owners.end());
    if (has_owner && !bmp_used) {
      add_issue(FsckIssue{
          "", "REFERENCED_BLOCK_MARKED_FREE", "error",
          "Active block #" + std::to_string(b) + " owned by inode #" +
              std::to_string(block_owners[b][0].inode_id) + " is marked Free (0) in block bitmap.",
          "block_bitmap", sb.block_bitmap_start, block_owners[b][0].inode_id,
          static_cast<uint64_t>(sb.block_bitmap_start) * bs + (b / 8U),
          "1 (Allocated)", "0 (Free)",
          "Block is referenced by an allocated inode but block bitmap shows 0.",
          "automatically_repairable",
          "Set block bitmap bit for block #" + std::to_string(b) + " to 1.",
          "None — prevents overwriting active file data"});
    } else if (!has_owner && bmp_used) {
      add_issue(FsckIssue{
          "", "UNREFERENCED_ALLOCATED_BLOCK", "warning",
          "Block #" + std::to_string(b) + " is marked Allocated (1) in block bitmap but has no owning inode.",
          "block_bitmap", sb.block_bitmap_start, 0,
          static_cast<uint64_t>(sb.block_bitmap_start) * bs + (b / 8U),
          "0 (Free)", "1 (Allocated)",
          "No allocated inode references block #" + std::to_string(b) + " (space leak).",
          "repairable_with_confirmation",
          "Reclaim unreferenced block #" + std::to_string(b) + " in block bitmap.",
          "Low — block has no active owner"});
    }
  }

  // Directory graph validation (cycle-safe iterative BFS from root inode 1)
  std::set<uint32_t> reachable_inodes;
  if (sb.total_inodes <= kRootInodeId ||
      inodes[kRootInodeId].state != static_cast<uint8_t>(InodeState::Allocated) ||
      inodes[kRootInodeId].type != static_cast<uint8_t>(InodeType::Directory)) {
    add_issue(FsckIssue{
        "", "MISSING_ROOT_INODE", "critical",
        "Root directory inode (#1) is missing or not an allocated directory.",
        "inode", sb.inode_table_start, kRootInodeId,
        static_cast<uint64_t>(sb.inode_table_start) * bs + kInodeBytes,
        "Allocated Directory", "Invalid state/type",
        "Root directory cannot be traversed.",
        "not_repairable", "Cannot traverse directory tree without root inode.", "Critical"});
  } else {
    reachable_inodes.insert(kRootInodeId);
    std::set<uint32_t> visited_dirs;
    std::queue<std::pair<uint32_t, std::string>> q;
    q.push({kRootInodeId, "/"});
    visited_dirs.insert(kRootInodeId);

    while (!q.empty()) {
      const auto [dir_ino_id, dir_path] = q.front();
      q.pop();
      const Inode& dir_ino = inodes[dir_ino_id];
      std::set<std::string> seen_names;

      for (uint32_t d = 0; d < kDirectBlockCount; ++d) {
        const uint32_t dbl = dir_ino.direct_blocks[d];
        if (dbl == 0 || dbl < sb.data_region_start || dbl >= sb.journal_start || dbl >= max_readable_blk) {
          continue;
        }
        std::vector<uint8_t> dbuf;
        disk.read_block(dbl, dbuf);
        const uint32_t slots = bs / static_cast<uint32_t>(kDirEntryBytes);
        for (uint32_t s = 0; s < slots; ++s) {
          DirEntry de{};
          deserialize_dir_entry(dbuf.data() + s * kDirEntryBytes, de);
          if (de.state != static_cast<uint8_t>(DirEntryState::Active)) continue;
          if (de.name == "." || de.name == "..") continue;

          const uint64_t entry_off = static_cast<uint64_t>(dbl) * bs + s * kDirEntryBytes;
          if (!seen_names.insert(de.name).second) {
            add_issue(FsckIssue{
                "", "DUPLICATE_DIR_NAME", "error",
                "Duplicate active directory entry '" + de.name + "' in " + dir_path + ".",
                "directory_entry", dbl, de.inode_id, entry_off,
                "Unique filename in directory", de.name,
                "Multiple active slots share the same name in directory block #" + std::to_string(dbl) + ".",
                "repairable_with_confirmation",
                "Remove duplicate directory entry slot in " + dir_path + ".",
                "Low if pointing to same inode"});
          }

          if (de.inode_id == 0 || de.inode_id >= sb.total_inodes ||
              inodes[de.inode_id].state != static_cast<uint8_t>(InodeState::Allocated)) {
            add_issue(FsckIssue{
                "", "DANGLING_DIR_ENTRY", "error",
                "Directory entry '" + de.name + "' in " + dir_path + " references invalid/unallocated inode #" +
                    std::to_string(de.inode_id) + ".",
                "directory_entry", dbl, de.inode_id, entry_off,
                "Allocated inode in [1.." + std::to_string(sb.total_inodes - 1) + "]",
                "inode #" + std::to_string(de.inode_id),
                "Target inode does not exist or is not Allocated.",
                "repairable_with_confirmation",
                "Remove dangling directory entry '" + de.name + "' (preserving name in repair log).",
                "None — target inode has no active allocation"});
            continue;
          }

          const Inode& child = inodes[de.inode_id];
          if (child.parent_inode != dir_ino_id) {
            add_issue(FsckIssue{
                "", "INCONSISTENT_PARENT_INODE", "warning",
                "Inode #" + std::to_string(de.inode_id) + " ('" + de.name + "') has parent_inode=" +
                    std::to_string(child.parent_inode) + " instead of " + std::to_string(dir_ino_id) + ".",
                "inode", sb.inode_table_start + (de.inode_id / inodes_per_blk), de.inode_id,
                static_cast<uint64_t>(sb.inode_table_start + (de.inode_id / inodes_per_blk)) * bs +
                    (de.inode_id % inodes_per_blk) * kInodeBytes,
                std::to_string(dir_ino_id), std::to_string(child.parent_inode),
                "Directory entry parent disagrees with inode.parent_inode.",
                "automatically_repairable",
                "Update inode #" + std::to_string(de.inode_id) + " parent_inode to " + std::to_string(dir_ino_id) + ".",
                "None"});
          }

          reachable_inodes.insert(de.inode_id);
          if (child.type == static_cast<uint8_t>(InodeType::Directory)) {
            if (visited_dirs.count(de.inode_id) > 0) {
              add_issue(FsckIssue{
                  "", "DIRECTORY_CYCLE", "error",
                  "Directory cycle detected: '" + dir_path + (dir_path == "/" ? "" : "/") + de.name +
                      "' points back to already-visited directory inode #" + std::to_string(de.inode_id) + ".",
                  "directory_entry", dbl, de.inode_id, entry_off,
                  "Acyclic directory tree", "Cycle -> inode #" + std::to_string(de.inode_id),
                  "Iterative BFS detected loop in directory graph.",
                  "repairable_with_confirmation",
                  "Remove cyclic directory entry '" + de.name + "'.",
                  "None — breaks infinite traversal loop"});
            } else {
              visited_dirs.insert(de.inode_id);
              const std::string child_path = (dir_path == "/") ? ("/" + de.name) : (dir_path + "/" + de.name);
              q.push({de.inode_id, child_path});
            }
          }
        }
      }
    }

    // Check unreachable active inodes (Orphans)
    for (uint32_t i = 2; i < sb.total_inodes; ++i) {
      if (inodes[i].state == static_cast<uint8_t>(InodeState::Allocated) &&
          reachable_inodes.count(i) == 0) {
        const uint32_t iblk = sb.inode_table_start + (i / inodes_per_blk);
        const uint64_t ioff = static_cast<uint64_t>(iblk) * bs + (i % inodes_per_blk) * kInodeBytes;
        add_issue(FsckIssue{
            "", "ORPHANED_INODE", "error",
            "Allocated inode #" + std::to_string(i) + " (" + std::to_string(inodes[i].size_bytes) +
                " B) is unreachable from '/' directory tree.",
            "inode", iblk, i, ioff,
            "Reachable from '/'", "Unreachable orphan",
            "Inode is Allocated in inode table with no active directory entry pointing to it.",
            "repairable_with_confirmation",
            "Reconnect orphaned inode #" + std::to_string(i) + " under /lost+found/inode_" + std::to_string(i) + ".",
            "None — preserves surviving file contents"});
      }
    }
  }

  // G. Payload integrity check (full mode)
  if (mode == "full") {
    for (uint32_t i = 2; i < sb.total_inodes; ++i) {
      const Inode& ino = inodes[i];
      if (ino.state != static_cast<uint8_t>(InodeState::Allocated) ||
          ino.type != static_cast<uint8_t>(InodeType::File)) {
        continue;
      }
      // Reconstruct logical payload bytes
      std::vector<uint32_t> data_blks;
      bool ptr_err = false;
      for (uint32_t d = 0; d < kDirectBlockCount; ++d) {
        if (ino.direct_blocks[d] == 0) continue;
        if (ino.direct_blocks[d] >= max_readable_blk) {
          ptr_err = true;
          break;
        }
        data_blks.push_back(ino.direct_blocks[d]);
      }
      if (!ptr_err && ino.indirect_block != 0 && ino.indirect_block < max_readable_blk) {
        std::vector<uint8_t> ibuf;
        disk.read_block(ino.indirect_block, ibuf);
        for (uint32_t p = 0; p < bs / 4U; ++p) {
          const uint32_t dp = read_u32_le(ibuf.data() + p * 4U);
          if (dp != 0) {
            if (dp >= max_readable_blk) {
              ptr_err = true;
              break;
            }
            data_blks.push_back(dp);
          }
        }
      }
      if (ptr_err || static_cast<uint64_t>(data_blks.size()) * bs < ino.size_bytes) {
        res.damaged_files_count++;
        res.damaged_file_paths.push_back("inode#" + std::to_string(i));
        continue;
      }

      std::vector<uint8_t> payload;
      payload.reserve(static_cast<size_t>(ino.size_bytes));
      uint64_t rem = ino.size_bytes;
      for (uint32_t b : data_blks) {
        if (rem == 0) break;
        std::vector<uint8_t> buf;
        disk.read_block(b, buf);
        const size_t take = static_cast<size_t>(std::min<uint64_t>(rem, bs));
        payload.insert(payload.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(take));
        rem -= take;
      }
      const uint32_t actual_crc = payload.empty() ? 0U : compute_crc32(payload);
      if (actual_crc != ino.content_crc32) {
        res.damaged_files_count++;
        res.damaged_file_paths.push_back("inode#" + std::to_string(i));
        add_issue(FsckIssue{
            "", "PAYLOAD_CRC_MISMATCH", "error",
            "File payload for inode #" + std::to_string(i) + " failed content CRC32 verification.",
            "file_payload", data_blks.empty() ? 0U : data_blks[0], i,
            data_blks.empty() ? 0ULL : static_cast<uint64_t>(data_blks[0]) * bs,
            hex32(ino.content_crc32), hex32(actual_crc),
            "Stored content_crc32 does not match actual data block bytes. Metadata repair cannot restore damaged payload bytes.",
            "not_repairable",
            "Preserve surviving bytes; do not overwrite payload or falsify stored CRC32.",
            "Data corruption in file payload"});
      } else {
        res.preserved_files_count++;
      }
    }
  }

  if (res.critical_count == 0 && res.error_count == 0 && res.warning_count == 0) {
    res.consistency_status = "clean";
  } else if (res.damaged_files_count > 0 && res.error_count == res.damaged_files_count && res.critical_count == 0) {
    res.consistency_status = "payload_damaged";
  } else {
    res.consistency_status = "inconsistent";
  }
  return res;
}

}  // namespace ghostfs
