#include "ghostfs/fragmentation.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

namespace ghostfs {

namespace {

inline bool bitmap_bit(const std::vector<uint8_t>& bm, uint32_t idx) {
  const size_t byte_idx = idx / 8U;
  if (byte_idx >= bm.size()) return false;
  return (bm[byte_idx] & static_cast<uint8_t>(1U << (idx % 8U))) != 0;
}

uint32_t count_contiguous_extents(const std::vector<uint32_t>& blocks) {
  if (blocks.empty()) return 0U;
  uint32_t extents = 1U;
  for (size_t i = 1; i < blocks.size(); ++i) {
    if (blocks[i] != blocks[i - 1] + 1U) {
      ++extents;
    }
  }
  return extents;
}

}  // namespace

std::vector<BlockInfo> FragmentationAnalyzer::build_full_block_map(const GfsFileSystem& fs) {
  const Superblock& sb = fs.superblock();
  const auto& bm = fs.block_bitmap();
  const auto& ibm = fs.inode_bitmap();

  std::vector<BlockInfo> blocks(sb.total_blocks);
  for (uint32_t b = 0; b < sb.total_blocks; ++b) {
    blocks[b].block_id = b;
    blocks[b].byte_offset = static_cast<uint64_t>(b) * sb.block_size;
    blocks[b].block_size = sb.block_size;
    blocks[b].allocated = bitmap_bit(bm, b);
    blocks[b].classification = blocks[b].allocated ? "inconsistent" : "free";
  }

  // Classify fixed metadata & journal regions.
  if (sb.total_blocks > 0U) blocks[0].classification = "reserved";
  if (sb.total_blocks > 1U) blocks[1].classification = "primary_superblock";
  if (sb.total_blocks > 2U) blocks[2].classification = "backup_superblock";

  for (uint32_t i = 0; i < sb.inode_bitmap_blocks; ++i) {
    const uint32_t b = sb.inode_bitmap_start + i;
    if (b < sb.total_blocks) blocks[b].classification = "inode_bitmap";
  }
  for (uint32_t i = 0; i < sb.block_bitmap_blocks; ++i) {
    const uint32_t b = sb.block_bitmap_start + i;
    if (b < sb.total_blocks) blocks[b].classification = "block_bitmap";
  }
  for (uint32_t i = 0; i < sb.inode_table_blocks; ++i) {
    const uint32_t b = sb.inode_table_start + i;
    if (b < sb.total_blocks) blocks[b].classification = "inode_table";
  }
  for (uint32_t i = 0; i < sb.journal_blocks; ++i) {
    const uint32_t b = sb.journal_start + i;
    if (b < sb.total_blocks) blocks[b].classification = "journal";
  }

  const uint32_t data_end = sb.data_region_start + sb.data_blocks_count;
  auto in_data_region = [&](uint32_t b) {
    return b >= sb.data_region_start && b < data_end && b < sb.total_blocks;
  };

  // Map active directory tree entries (root '/' + recursive children).
  std::unordered_map<uint32_t, std::string> inode_paths;
  inode_paths[kRootInodeId] = "/";
  try {
    const auto all_entries = fs.list_all_entries_recursive();
    for (const auto& e : all_entries) {
      inode_paths[e.inode_id] = e.path;
    }
  } catch (...) {
    // Best-effort path mapping even on partially damaged directories.
  }

  // Attribute active allocated inodes first.
  for (uint32_t id = 1U; id < sb.total_inodes; ++id) {
    if (!bitmap_bit(ibm, id)) continue;
    bool crc_ok = false;
    const Inode inode = fs.read_inode_slot(id, &crc_ok);
    if (!crc_ok || inode.state != static_cast<uint8_t>(InodeState::Allocated)) continue;

    std::vector<uint32_t> data_blks;
    std::vector<uint32_t> ind_blks;
    try {
      fs.collect_inode_blocks(inode, data_blks, ind_blks);
    } catch (...) {
      continue;
    }

    const std::string owner = inode_paths.count(id) ? inode_paths[id] : ("inode#" + std::to_string(id));
    const std::string data_cls =
        (inode.type == static_cast<uint8_t>(InodeType::Directory)) ? "directory_data" : "file_data";

    const uint32_t data_extents_start = data_blks.empty() ? 0U : data_blks.front();
    for (uint32_t b : data_blks) {
      if (!in_data_region(b)) continue;
      blocks[b].inode_id = id;
      blocks[b].owner_path = owner;
      blocks[b].classification = blocks[b].allocated ? data_cls : "inconsistent";
      blocks[b].extent_start = data_extents_start;
    }
    for (uint32_t b : ind_blks) {
      if (!in_data_region(b)) continue;
      blocks[b].inode_id = id;
      blocks[b].owner_path = owner;
      blocks[b].classification = blocks[b].allocated ? "indirect_pointer" : "inconsistent";
    }
  }

  // Attribute free blocks that still retain residual pointers from Deleted inodes.
  for (uint32_t id = 1U; id < sb.total_inodes; ++id) {
    if (bitmap_bit(ibm, id)) continue;
    bool crc_ok = false;
    const Inode inode = fs.read_inode_slot(id, &crc_ok);
    if (!crc_ok || inode.state != static_cast<uint8_t>(InodeState::Deleted)) continue;

    for (uint32_t b : inode.direct_blocks) {
      if (b == 0U || !in_data_region(b)) continue;
      if (!blocks[b].allocated && blocks[b].classification == "free") {
        blocks[b].classification = "free_deleted_residual";
        blocks[b].inode_id = id;
        blocks[b].owner_path = "deleted:inode#" + std::to_string(id);
      }
    }
  }

  // Annotate contiguous free extents on free / free_deleted_residual blocks, and contiguous run lengths.
  const auto free_extents = BlockAllocator::find_free_extents(bm, sb.data_region_start, sb.data_blocks_count);
  for (const auto& ext : free_extents) {
    for (uint32_t i = 0; i < ext.length; ++i) {
      const uint32_t b = ext.start_block + i;
      if (b < sb.total_blocks && !blocks[b].allocated) {
        blocks[b].extent_start = ext.start_block;
        blocks[b].extent_length = ext.length;
      }
    }
  }

  // Compute contiguous extent span for allocated data blocks.
  uint32_t b = sb.data_region_start;
  while (b < data_end && b < sb.total_blocks) {
    if (blocks[b].allocated && blocks[b].inode_id != 0U) {
      const uint32_t owner_id = blocks[b].inode_id;
      const std::string& cls = blocks[b].classification;
      uint32_t end = b + 1U;
      while (end < data_end && end < sb.total_blocks &&
             blocks[end].allocated && blocks[end].inode_id == owner_id &&
             blocks[end].classification == cls) {
        ++end;
      }
      const uint32_t run_len = end - b;
      for (uint32_t k = b; k < end; ++k) {
        blocks[k].extent_start = b;
        blocks[k].extent_length = run_len;
      }
      b = end;
    } else {
      ++b;
    }
  }

  return blocks;
}

FragmentationReport FragmentationAnalyzer::analyze(const GfsFileSystem& fs) {
  const Superblock& sb = fs.superblock();
  const auto blocks = build_full_block_map(fs);
  const auto free_extents =
      BlockAllocator::find_free_extents(fs.block_bitmap(), sb.data_region_start, sb.data_blocks_count);

  FragmentationReport report{};
  report.disk_name = fs.disk().path().filename().string();
  report.alloc_policy = alloc_policy_to_string(static_cast<AllocPolicy>(sb.alloc_policy));
  report.block_size = sb.block_size;
  report.total_blocks = sb.total_blocks;
  report.reserved_metadata_blocks = sb.data_region_start;
  report.journal_blocks = sb.journal_blocks;
  report.data_blocks_count = sb.data_blocks_count;
  report.free_extents = free_extents;

  const uint32_t data_end = sb.data_region_start + sb.data_blocks_count;
  for (uint32_t b = sb.data_region_start; b < data_end && b < sb.total_blocks; ++b) {
    const auto& blk = blocks[b];
    if (blk.allocated) {
      ++report.allocated_data_blocks;
      if (blk.classification == "file_data") ++report.file_data_blocks;
      else if (blk.classification == "directory_data") ++report.directory_data_blocks;
      else if (blk.classification == "indirect_pointer") ++report.indirect_pointer_blocks;
      else if (blk.classification == "inconsistent") ++report.inconsistent_blocks;
    } else {
      ++report.free_blocks;
      if (blk.classification == "free_deleted_residual") {
        ++report.free_deleted_residual_blocks;
      }
    }
  }

  report.free_extent_count = static_cast<uint32_t>(free_extents.size());
  if (!free_extents.empty()) {
    uint32_t min_len = free_extents.front().length;
    uint32_t max_len = free_extents.front().length;
    uint64_t sum_len = 0ULL;
    for (const auto& ext : free_extents) {
      min_len = std::min(min_len, ext.length);
      max_len = std::max(max_len, ext.length);
      sum_len += ext.length;
    }
    report.smallest_free_extent = min_len;
    report.largest_free_extent = max_len;
    report.average_free_extent = static_cast<double>(sum_len) / static_cast<double>(free_extents.size());
  }

  if (report.free_blocks == 0U) {
    report.external_fragmentation_pct = 0.0;
  } else {
    report.external_fragmentation_pct =
        (1.0 - static_cast<double>(report.largest_free_extent) / static_cast<double>(report.free_blocks)) *
        100.0;
  }

  // Per-file fragmentation & internal fragmentation (file data blocks only, excluding directories/metadata).
  const auto all_entries = fs.list_all_entries_recursive();
  for (const auto& entry : all_entries) {
    if (entry.type != static_cast<uint8_t>(InodeType::File)) continue;

    FileFragmentationInfo fi{};
    fi.inode_id = entry.inode_id;
    fi.name = entry.name;
    fi.path = entry.path;
    fi.size_bytes = entry.size_bytes;
    fi.allocated_data_blocks = static_cast<uint32_t>(entry.all_data_blocks.size());
    fi.allocated_payload_capacity_bytes = static_cast<uint64_t>(fi.allocated_data_blocks) * sb.block_size;
    fi.internal_fragmentation_bytes =
        (fi.allocated_payload_capacity_bytes >= fi.size_bytes)
            ? (fi.allocated_payload_capacity_bytes - fi.size_bytes)
            : 0ULL;
    fi.internal_fragmentation_pct =
        (fi.allocated_payload_capacity_bytes > 0ULL)
            ? (static_cast<double>(fi.internal_fragmentation_bytes) /
               static_cast<double>(fi.allocated_payload_capacity_bytes)) *
                  100.0
            : 0.0;
    fi.physical_data_blocks = entry.all_data_blocks;
    fi.indirect_blocks = entry.all_indirect_blocks;
    fi.extent_count = count_contiguous_extents(fi.physical_data_blocks);
    fi.is_fragmented = fi.extent_count > 1U;

    if (fi.is_fragmented) {
      ++report.fragmented_file_count;
    } else {
      ++report.contiguous_file_count;
    }
    ++report.total_files;

    report.total_allocated_payload_capacity_bytes += fi.allocated_payload_capacity_bytes;
    report.total_logical_file_bytes += fi.size_bytes;
    report.internal_fragmentation_bytes += fi.internal_fragmentation_bytes;
    report.files.push_back(std::move(fi));
  }

  std::sort(report.files.begin(), report.files.end(), [](const FileFragmentationInfo& a, const FileFragmentationInfo& b) {
    return a.path < b.path;
  });

  if (report.total_allocated_payload_capacity_bytes > 0ULL) {
    report.internal_fragmentation_pct =
        (static_cast<double>(report.internal_fragmentation_bytes) /
         static_cast<double>(report.total_allocated_payload_capacity_bytes)) *
        100.0;
  }

  if (report.total_blocks > 0U) {
    report.disk_utilization_pct =
        (static_cast<double>(report.total_blocks - report.free_blocks) /
         static_cast<double>(report.total_blocks)) *
        100.0;
  }
  if (report.data_blocks_count > 0U) {
    report.data_utilization_pct =
        (static_cast<double>(report.allocated_data_blocks) /
         static_cast<double>(report.data_blocks_count)) *
        100.0;
  }

  return report;
}

BlockMapPage FragmentationAnalyzer::query_block_map(
    const GfsFileSystem& fs,
    uint32_t offset,
    uint32_t limit,
    const std::string& type_filter,
    uint32_t file_inode_filter) {
  const auto full = build_full_block_map(fs);
  std::vector<BlockInfo> filtered;
  filtered.reserve(full.size());

  for (const auto& blk : full) {
    if (!type_filter.empty() && blk.classification != type_filter) continue;
    if (file_inode_filter != 0U && blk.inode_id != file_inode_filter) continue;
    filtered.push_back(blk);
  }

  BlockMapPage page{};
  page.total_blocks = static_cast<uint32_t>(full.size());
  page.filtered_total = static_cast<uint32_t>(filtered.size());
  page.offset = offset;
  page.limit = limit;
  page.block_size = fs.superblock().block_size;

  if (offset < filtered.size() && limit > 0U) {
    const size_t end = std::min<size_t>(filtered.size(), static_cast<size_t>(offset) + limit);
    page.blocks.assign(filtered.begin() + static_cast<std::ptrdiff_t>(offset),
                       filtered.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return page;
}

BlockInfo FragmentationAnalyzer::inspect_block(const GfsFileSystem& fs, uint32_t block_id) {
  const auto full = build_full_block_map(fs);
  if (block_id >= full.size()) {
    throw std::out_of_range("ERR_OUT_OF_BOUNDS: Block index out of range");
  }
  return full[block_id];
}

}  // namespace ghostfs
