#include "ghostfs/recovery.hpp"

#include "ghostfs/crc32.hpp"
#include "ghostfs/endian.hpp"
#include "ghostfs/sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace ghostfs {

namespace {

inline bool bm_bit(const std::vector<uint8_t>& bm, uint32_t idx) {
  const size_t byte_idx = idx / 8U;
  if (byte_idx >= bm.size()) return false;
  return (bm[byte_idx] & static_cast<uint8_t>(1U << (idx % 8U))) != 0;
}

std::string infer_extension_type(const std::string& filename) {
  const auto dot = filename.rfind('.');
  if (dot == std::string::npos) return "unknown";
  std::string ext = filename.substr(dot + 1);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (ext == "png") return "png";
  if (ext == "jpg" || ext == "jpeg") return "jpeg";
  if (ext == "pdf") return "pdf";
  if (ext == "txt" || ext == "log" || ext == "md" || ext == "csv" || ext == "json") {
    return "txt";
  }
  return "unknown";
}

struct DeletedDirHint {
  uint32_t parent_inode = 0;
  std::string name;
  std::string full_path;
};

struct ActiveBlockOwner {
  uint32_t inode_id = 0;
  uint32_t generation = 0;
  bool is_indirect = false;
};

void append_range(std::vector<ByteRange>& ranges, uint64_t start_b, uint64_t end_b) {
  if (start_b >= end_b) return;
  if (!ranges.empty() && ranges.back().end_byte == start_b) {
    ranges.back().end_byte = end_b;
  } else {
    ranges.push_back(ByteRange{start_b, end_b});
  }
}

uint32_t ceil_div_u32(uint64_t num, uint32_t den) {
  if (num == 0ULL) return 0U;
  return static_cast<uint32_t>((num + den - 1ULL) / den);
}

}  // namespace

std::vector<RecoveryCandidate> run_metadata_recovery(
    const VirtualDisk& disk,
    const GfsFileSystem& fs,
    uint32_t& out_blocks_examined,
    uint64_t& out_bytes_examined,
    std::vector<std::string>& out_warnings) {
  const Superblock& sb = fs.superblock();
  const auto& ibm = fs.inode_bitmap();
  const auto& bbm = fs.block_bitmap();
  const uint32_t data_start = sb.data_region_start;
  const uint32_t data_end = sb.data_region_start + sb.data_blocks_count;
  const uint32_t ptrs_per_block = sb.block_size / 4U;
  const uint32_t entries_per_block = sb.block_size / static_cast<uint32_t>(kDirEntryBytes);

  out_blocks_examined = sb.inode_table_blocks;
  out_bytes_examined = static_cast<uint64_t>(sb.inode_table_blocks) * sb.block_size;

  // Map active directory paths and active block ownership.
  std::unordered_map<uint32_t, std::string> active_paths;
  active_paths[kRootInodeId] = "/";
  try {
    for (const auto& entry : fs.list_all_entries_recursive()) {
      active_paths[entry.inode_id] = entry.path;
    }
  } catch (...) {
    out_warnings.push_back("Directory tree traversal encountered damaged entries; some paths may be partial.");
  }

  std::unordered_map<uint32_t, ActiveBlockOwner> active_block_owners;
  std::vector<Inode> all_inodes(sb.total_inodes);
  std::vector<bool> inode_crc_valid(sb.total_inodes, false);

  for (uint32_t id = 1U; id < sb.total_inodes; ++id) {
    bool crc_ok = false;
    all_inodes[id] = fs.read_inode_slot(id, &crc_ok);
    inode_crc_valid[id] = crc_ok;

    if (bm_bit(ibm, id) && crc_ok && all_inodes[id].state == static_cast<uint8_t>(InodeState::Allocated)) {
      std::vector<uint32_t> dblks;
      std::vector<uint32_t> iblks;
      try {
        fs.collect_inode_blocks(all_inodes[id], dblks, iblks);
        for (uint32_t b : dblks) {
          active_block_owners[b] = ActiveBlockOwner{id, all_inodes[id].generation, false};
        }
        for (uint32_t b : iblks) {
          active_block_owners[b] = ActiveBlockOwner{id, all_inodes[id].generation, true};
        }
      } catch (...) {
        // Ignore broken active pointers here.
      }
    }
  }

  // Scan directory data blocks (both allocated and deleted directory inodes) for Deleted DirEntries.
  // Later directory slots supersede earlier ones for the same inode_id (if an inode was reused);
  // superseded names are preserved in `superseded_hints` so reused-inode victims are still reported.
  std::unordered_map<uint32_t, DeletedDirHint> dir_hints;
  std::vector<DeletedDirHint> superseded_hints;
  std::vector<uint8_t> blk_buf;
  for (uint32_t id = 1U; id < sb.total_inodes; ++id) {
    if (!inode_crc_valid[id]) continue;
    const Inode& dir = all_inodes[id];
    if (dir.type != static_cast<uint8_t>(InodeType::Directory)) continue;
    if (dir.state != static_cast<uint8_t>(InodeState::Allocated) &&
        dir.state != static_cast<uint8_t>(InodeState::Deleted)) {
      continue;
    }
    const std::string parent_path =
        active_paths.count(id) ? active_paths[id] : ("/lost+dir#" + std::to_string(id));

    for (uint32_t b : dir.direct_blocks) {
      if (b < data_start || b >= data_end) continue;
      // Only read dir block if dir is allocated OR the dir block itself hasn't been overwritten by another active file.
      if (dir.state == static_cast<uint8_t>(InodeState::Deleted) && active_block_owners.count(b)) {
        continue;
      }
      disk.read_block(b, blk_buf);
      ++out_blocks_examined;
      out_bytes_examined += sb.block_size;
      for (uint32_t s = 0; s < entries_per_block; ++s) {
        DirEntry de{};
        if (deserialize_dir_entry(blk_buf.data() + static_cast<size_t>(s) * kDirEntryBytes, de) &&
            de.state == static_cast<uint8_t>(DirEntryState::Deleted) &&
            de.inode_id != 0U && !de.name.empty()) {
          const std::string fp = (parent_path == "/") ? ("/" + de.name) : (parent_path + "/" + de.name);
          if (dir_hints.count(de.inode_id)) {
            superseded_hints.push_back(dir_hints[de.inode_id]);
          }
          dir_hints[de.inode_id] = DeletedDirHint{id, de.name, fp};
        }
      }
    }
  }

  // Build map of blocks referenced by newer deleted inodes (detects overwrite-then-redelete!).
  // For each block b, record the maximum deleted_at_ms / created_at_ms among all deleted inodes referencing b.
  struct BlockReuseHistory {
    uint32_t latest_inode_id = 0;
    uint64_t latest_timestamp_ms = 0;
  };
  std::unordered_map<uint32_t, BlockReuseHistory> deleted_block_history;
  for (uint32_t id = 1U; id < sb.total_inodes; ++id) {
    if (!inode_crc_valid[id]) continue;
    const Inode& ino = all_inodes[id];
    if (ino.state != static_cast<uint8_t>(InodeState::Deleted)) continue;
    const uint64_t ts = std::max(ino.deleted_at_ms, ino.modified_at_ms);
    for (uint32_t b : ino.direct_blocks) {
      if (b >= data_start && b < data_end) {
        auto& h = deleted_block_history[b];
        if (ts > h.latest_timestamp_ms || (ts == h.latest_timestamp_ms && id > h.latest_inode_id)) {
          h = BlockReuseHistory{id, ts};
        }
      }
    }
    if (ino.indirect_block >= data_start && ino.indirect_block < data_end) {
      auto& h = deleted_block_history[ino.indirect_block];
      if (ts > h.latest_timestamp_ms || (ts == h.latest_timestamp_ms && id > h.latest_inode_id)) {
        h = BlockReuseHistory{id, ts};
      }
    }
  }

  std::vector<RecoveryCandidate> candidates;

  // 1. Scan Deleted inodes in Inode Table.
  for (uint32_t id = 1U; id < sb.total_inodes; ++id) {
    if (!inode_crc_valid[id]) continue;
    const Inode& ino = all_inodes[id];
    if (ino.state != static_cast<uint8_t>(InodeState::Deleted)) continue;
    if (ino.type != static_cast<uint8_t>(InodeType::File)) continue;

    RecoveryCandidate cand{};
    cand.candidate_id = "meta_inode_" + std::to_string(id) + "_g" + std::to_string(ino.generation);
    cand.recovery_method = "metadata";
    cand.inode_id = id;
    cand.generation = ino.generation;
    cand.original_size_bytes = ino.size_bytes;
    cand.created_at_ms = ino.created_at_ms;
    cand.modified_at_ms = ino.modified_at_ms;
    cand.deleted_at_ms = ino.deleted_at_ms;
    cand.expected_crc32 = ino.content_crc32;
    cand.has_expected_crc32 = true;

    if (dir_hints.count(id)) {
      cand.filename = dir_hints[id].name;
      cand.original_path = dir_hints[id].full_path;
      cand.filename_inferred = false;
      cand.evidence.push_back("Matched deleted directory entry '" + cand.original_path + "'");
    } else {
      cand.filename = "deleted_inode_" + std::to_string(id) + ".bin";
      cand.original_path = "/" + cand.filename;
      cand.filename_inferred = true;
      cand.warnings.push_back("Original directory entry was overwritten; filename inferred from inode ID.");
    }
    cand.file_type = infer_extension_type(cand.filename);
    cand.evidence.push_back("Surviving deleted inode #" + std::to_string(id) +
                            " (generation " + std::to_string(ino.generation) + ", valid inode CRC32)");

    // Handle zero-length files explicitly.
    if (ino.size_bytes == 0ULL) {
      cand.recoverable_bytes = 0ULL;
      cand.recovery_pct = 100.0;
      cand.reconstructed_crc32 = 0U;
      cand.crc32_matched = (ino.content_crc32 == 0U);
      cand.integrity_status = "empty";
      cand.classification = RecoveryClassification::FullyVerified;
      cand.classification_str = classification_to_string(cand.classification);
      cand.evidence.push_back("Zero-byte file requires 0 data blocks.");
      candidates.push_back(std::move(cand));
      continue;
    }

    const uint32_t expected_blocks = ceil_div_u32(ino.size_bytes, sb.block_size);
    std::vector<uint32_t> logical_blocks(expected_blocks, 0U);
    std::vector<bool> indirect_missing(expected_blocks, false);

    // Direct pointers [0..11]
    const uint32_t direct_n = std::min<uint32_t>(expected_blocks, kDirectBlockCount);
    for (uint32_t i = 0; i < direct_n; ++i) {
      logical_blocks[i] = ino.direct_blocks[i];
    }

    // Single indirect pointers
    if (expected_blocks > kDirectBlockCount) {
      const uint32_t single_end = std::min<uint32_t>(expected_blocks, kDirectBlockCount + ptrs_per_block);
      const uint32_t ib = ino.indirect_block;
      const bool ib_in_range = (ib >= data_start && ib < data_end);
      const bool ib_reused_active = ib_in_range && active_block_owners.count(ib);
      const bool ib_reused_newer_del =
          ib_in_range && deleted_block_history.count(ib) &&
          deleted_block_history[ib].latest_inode_id != id &&
          deleted_block_history[ib].latest_timestamp_ms >= ino.deleted_at_ms;

      if (!ib_in_range || ib_reused_active || ib_reused_newer_del) {
        cand.warnings.push_back(
            "Single-indirect pointer block " + std::to_string(ib) +
            " is unavailable or reused; indirect block mappings cannot be trusted.");
        for (uint32_t i = kDirectBlockCount; i < single_end; ++i) {
          indirect_missing[i] = true;
        }
      } else {
        std::vector<uint8_t> ib_buf;
        disk.read_block(ib, ib_buf);
        ++out_blocks_examined;
        out_bytes_examined += sb.block_size;
        for (uint32_t i = kDirectBlockCount; i < single_end; ++i) {
          const uint32_t ptr = endian::read_u32_le(ib_buf.data() + static_cast<size_t>(i - kDirectBlockCount) * 4U);
          logical_blocks[i] = ptr;
        }
      }
    }

    // Double indirect pointers
    if (expected_blocks > kDirectBlockCount + ptrs_per_block) {
      const uint32_t dib = ino.double_indirect_block;
      const bool dib_in_range = (dib >= data_start && dib < data_end);
      const bool dib_reused_active = dib_in_range && active_block_owners.count(dib);
      if (!dib_in_range || dib_reused_active) {
        cand.warnings.push_back(
            "Double-indirect pointer block " + std::to_string(dib) +
            " is unavailable or reused.");
        for (uint32_t i = kDirectBlockCount + ptrs_per_block; i < expected_blocks; ++i) {
          indirect_missing[i] = true;
        }
      } else {
        std::vector<uint8_t> outer_buf;
        disk.read_block(dib, outer_buf);
        ++out_blocks_examined;
        out_bytes_examined += sb.block_size;
        uint32_t idx = kDirectBlockCount + ptrs_per_block;
        for (uint32_t outer_slot = 0; outer_slot < ptrs_per_block && idx < expected_blocks; ++outer_slot) {
          const uint32_t inner_blk = endian::read_u32_le(outer_buf.data() + static_cast<size_t>(outer_slot) * 4U);
          const uint32_t inner_end = std::min<uint32_t>(expected_blocks, idx + ptrs_per_block);
          const bool inner_ok = (inner_blk >= data_start && inner_blk < data_end) &&
                                !active_block_owners.count(inner_blk);
          if (!inner_ok) {
            cand.warnings.push_back("Inner indirect block " + std::to_string(inner_blk) + " is invalid or reused.");
            for (; idx < inner_end; ++idx) {
              indirect_missing[idx] = true;
            }
          } else {
            std::vector<uint8_t> inner_buf;
            disk.read_block(inner_blk, inner_buf);
            ++out_blocks_examined;
            out_bytes_examined += sb.block_size;
            for (uint32_t j = 0; j < ptrs_per_block && idx < inner_end; ++j, ++idx) {
              logical_blocks[idx] = endian::read_u32_le(inner_buf.data() + static_cast<size_t>(j) * 4U);
            }
          }
        }
      }
    }

    // Evaluate each logical block & reconstruct intact payload bytes to verify CRC32.
    std::vector<uint8_t> assembled;
    assembled.reserve(static_cast<size_t>(ino.size_bytes));
    uint32_t intact_block_count = 0U;
    uint32_t overwritten_block_count = 0U;
    uint32_t missing_block_count = 0U;

    for (uint32_t i = 0; i < expected_blocks; ++i) {
      const uint64_t logical_start = static_cast<uint64_t>(i) * sb.block_size;
      const uint64_t logical_end = std::min<uint64_t>(ino.size_bytes, logical_start + sb.block_size);
      const uint64_t slice_len = logical_end - logical_start;

      LogicalBlockStatus lbs{};
      lbs.logical_index = i;
      lbs.physical_block = logical_blocks[i];
      lbs.disk_byte_offset = static_cast<uint64_t>(lbs.physical_block) * sb.block_size;

      if (indirect_missing[i] || lbs.physical_block < data_start || lbs.physical_block >= data_end) {
        lbs.state = "missing_indirect";
        lbs.detail = indirect_missing[i] ? "Indirect pointer block reused/damaged" : "Block pointer out of data region";
        ++missing_block_count;
        append_range(cand.missing_ranges, logical_start, logical_end);
        cand.block_statuses.push_back(std::move(lbs));
        continue;
      }

      cand.source_blocks.push_back(lbs.physical_block);
      cand.source_byte_offsets.push_back(lbs.disk_byte_offset);

      if (active_block_owners.count(lbs.physical_block)) {
        const auto& owner = active_block_owners[lbs.physical_block];
        lbs.state = "overwritten";
        lbs.detail = "Block reallocated to active inode #" + std::to_string(owner.inode_id);
        ++overwritten_block_count;
        append_range(cand.missing_ranges, logical_start, logical_end);
      } else if (bm_bit(bbm, lbs.physical_block)) {
        lbs.state = "overwritten";
        lbs.detail = "Block is marked allocated in block bitmap";
        ++overwritten_block_count;
        append_range(cand.missing_ranges, logical_start, logical_end);
      } else if (deleted_block_history.count(lbs.physical_block) &&
                 deleted_block_history[lbs.physical_block].latest_inode_id != id &&
                 (deleted_block_history[lbs.physical_block].latest_timestamp_ms > ino.deleted_at_ms ||
                  (deleted_block_history[lbs.physical_block].latest_timestamp_ms == ino.deleted_at_ms &&
                   deleted_block_history[lbs.physical_block].latest_inode_id > id))) {
        lbs.state = "overwritten";
        lbs.detail = "Block was overwritten by newer deleted inode #" +
                     std::to_string(deleted_block_history[lbs.physical_block].latest_inode_id);
        ++overwritten_block_count;
        append_range(cand.missing_ranges, logical_start, logical_end);
      } else {
        lbs.state = "unknown";
        lbs.detail = "Unallocated block; historical non-reuse cannot be proven until full-file CRC32 is verified";
        ++intact_block_count;
        cand.recoverable_bytes += slice_len;
        append_range(cand.recoverable_ranges, logical_start, logical_end);

        disk.read_block(lbs.physical_block, blk_buf);
        ++out_blocks_examined;
        out_bytes_examined += sb.block_size;
        assembled.insert(assembled.end(), blk_buf.begin(), blk_buf.begin() + static_cast<std::ptrdiff_t>(slice_len));
      }

      cand.block_statuses.push_back(std::move(lbs));
    }

    cand.recovery_pct = (static_cast<double>(cand.recoverable_bytes) / static_cast<double>(ino.size_bytes)) * 100.0;

    if (intact_block_count == expected_blocks && assembled.size() == ino.size_bytes) {
      cand.reconstructed_crc32 = compute_crc32(assembled);
      cand.crc32_matched = (cand.reconstructed_crc32 == ino.content_crc32);
      if (cand.crc32_matched) {
        cand.integrity_status = "crc32_verified";
        cand.classification = RecoveryClassification::FullyVerified;
        for (auto& lbs : cand.block_statuses) {
          lbs.state = "intact";
          lbs.detail = "Verified by full-file CRC32 match";
        }
        cand.evidence.push_back("Reconstructed payload CRC32 matches original inode content_crc32.");
      } else {
        // All blocks appear free in bitmap, yet CRC32 failed -> data was overwritten & freed again without surviving metadata!
        cand.integrity_status = "crc32_mismatch";
        cand.classification = RecoveryClassification::Overwritten;
        cand.recoverable_bytes = 0ULL;
        cand.recovery_pct = 0.0;
        cand.recoverable_ranges.clear();
        cand.missing_ranges = {ByteRange{0ULL, ino.size_bytes}};
        for (auto& lbs : cand.block_statuses) {
          lbs.state = "overwritten";
          lbs.detail = "Block is free, but full-file CRC32 mismatch proves original contents were altered";
        }
        cand.warnings.push_back(
            "CRC32 mismatch despite free blocks: blocks were likely overwritten and re-deleted.");
      }
    } else if (intact_block_count > 0U) {
      cand.integrity_status = "partial";
      cand.classification = RecoveryClassification::PartiallyRecoverable;
      cand.warnings.push_back(
          std::to_string(overwritten_block_count + missing_block_count) + " of " +
          std::to_string(expected_blocks) + " blocks are overwritten or missing.");
    } else if (overwritten_block_count > 0U) {
      cand.integrity_status = "overwritten";
      cand.classification = RecoveryClassification::Overwritten;
      cand.warnings.push_back("All data blocks were overwritten by subsequent allocations.");
    } else {
      cand.integrity_status = "unrecoverable";
      cand.classification = RecoveryClassification::Unrecoverable;
      cand.warnings.push_back("No valid data block pointers remain.");
    }

    cand.classification_str = classification_to_string(cand.classification);
    candidates.push_back(std::move(cand));
  }

  // 2. Check Deleted DirEntries whose inode slot was reused (Scenario D — Inode Reuse).
  for (const auto& [target_inode_id, hint] : dir_hints) {
    if (target_inode_id == 0U || target_inode_id >= sb.total_inodes) continue;
    const Inode& current_ino = all_inodes[target_inode_id];
    const bool already_reported =
        (inode_crc_valid[target_inode_id] && current_ino.state == static_cast<uint8_t>(InodeState::Deleted) &&
         current_ino.type == static_cast<uint8_t>(InodeType::File));
    if (already_reported) continue;

    RecoveryCandidate reused{};
    reused.candidate_id = "meta_reused_inode_" + std::to_string(target_inode_id) + "_" + hint.name;
    reused.recovery_method = "metadata";
    reused.inode_id = target_inode_id;
    reused.generation = inode_crc_valid[target_inode_id] ? current_ino.generation : 0U;
    reused.filename = hint.name;
    reused.original_path = hint.full_path;
    reused.file_type = infer_extension_type(hint.name);
    reused.integrity_status = "unverified";
    reused.classification = RecoveryClassification::Unrecoverable;
    reused.classification_str = classification_to_string(reused.classification);
    reused.evidence.push_back("Surviving deleted directory entry '" + hint.full_path +
                              "' references inode #" + std::to_string(target_inode_id));
    reused.warnings.push_back(
        "Inode #" + std::to_string(target_inode_id) +
        " was reused (current state=" + std::to_string(current_ino.state) +
        ", generation=" + std::to_string(reused.generation) +
        "); original size and block pointers were overwritten.");
    candidates.push_back(std::move(reused));
  }

  for (const auto& hint : superseded_hints) {
    RecoveryCandidate reused{};
    reused.candidate_id = "meta_superseded_inode_" + std::to_string(hint.parent_inode) + "_" + hint.name;
    reused.recovery_method = "metadata";
    reused.inode_id = 0U;
    reused.filename = hint.name;
    reused.original_path = hint.full_path;
    reused.file_type = infer_extension_type(hint.name);
    reused.integrity_status = "unverified";
    reused.classification = RecoveryClassification::Unrecoverable;
    reused.classification_str = classification_to_string(reused.classification);
    reused.evidence.push_back("Surviving superseded deleted directory entry '" + hint.full_path + "'");
    reused.warnings.push_back("Inode slot was subsequently reused by a newer file.");
    candidates.push_back(std::move(reused));
  }

  return candidates;
}

}  // namespace ghostfs
