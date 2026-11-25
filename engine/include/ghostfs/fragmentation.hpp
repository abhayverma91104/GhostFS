#pragma once

#include "ghostfs/allocator.hpp"
#include "ghostfs/gfs_fs.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ghostfs {

struct BlockInfo {
  uint32_t block_id = 0;
  uint64_t byte_offset = 0;
  uint32_t block_size = 0;
  bool allocated = false;
  std::string classification;
  uint32_t inode_id = 0;
  std::string owner_path;
  uint32_t extent_start = 0;
  uint32_t extent_length = 0;
};

struct FileFragmentationInfo {
  uint32_t inode_id = 0;
  std::string name;
  std::string path;
  uint64_t size_bytes = 0;
  uint32_t allocated_data_blocks = 0;
  uint64_t allocated_payload_capacity_bytes = 0;
  uint64_t internal_fragmentation_bytes = 0;
  double internal_fragmentation_pct = 0.0;
  std::vector<uint32_t> physical_data_blocks;
  std::vector<uint32_t> indirect_blocks;
  uint32_t extent_count = 0;
  bool is_fragmented = false;
};

struct FragmentationReport {
  std::string disk_name;
  std::string alloc_policy;
  uint32_t block_size = 0;
  uint32_t total_blocks = 0;
  uint32_t reserved_metadata_blocks = 0;
  uint32_t journal_blocks = 0;
  uint32_t data_blocks_count = 0;
  uint32_t allocated_data_blocks = 0;
  uint32_t file_data_blocks = 0;
  uint32_t directory_data_blocks = 0;
  uint32_t indirect_pointer_blocks = 0;
  uint32_t free_blocks = 0;
  uint32_t free_deleted_residual_blocks = 0;
  uint32_t inconsistent_blocks = 0;

  uint32_t free_extent_count = 0;
  uint32_t largest_free_extent = 0;
  uint32_t smallest_free_extent = 0;
  double average_free_extent = 0.0;

  uint32_t fragmented_file_count = 0;
  uint32_t contiguous_file_count = 0;
  uint32_t total_files = 0;

  uint64_t total_allocated_payload_capacity_bytes = 0;
  uint64_t total_logical_file_bytes = 0;
  uint64_t internal_fragmentation_bytes = 0;
  double internal_fragmentation_pct = 0.0;
  double external_fragmentation_pct = 0.0;
  double disk_utilization_pct = 0.0;
  double data_utilization_pct = 0.0;

  std::vector<FreeExtent> free_extents;
  std::vector<FileFragmentationInfo> files;
};

struct BlockMapPage {
  uint32_t total_blocks = 0;
  uint32_t filtered_total = 0;
  uint32_t offset = 0;
  uint32_t limit = 0;
  uint32_t block_size = 0;
  std::vector<BlockInfo> blocks;
};

class FragmentationAnalyzer {
 public:
  static std::vector<BlockInfo> build_full_block_map(const GfsFileSystem& fs);
  static FragmentationReport analyze(const GfsFileSystem& fs);
  static BlockMapPage query_block_map(
      const GfsFileSystem& fs,
      uint32_t offset,
      uint32_t limit,
      const std::string& type_filter = "",
      uint32_t file_inode_filter = 0);
  static BlockInfo inspect_block(const GfsFileSystem& fs, uint32_t block_id);
};

}  // namespace ghostfs
