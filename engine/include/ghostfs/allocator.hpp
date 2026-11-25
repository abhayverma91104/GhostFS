#pragma once

#include "ghostfs/gfs_format.hpp"

#include <cstdint>
#include <vector>

namespace ghostfs {

struct FreeExtent {
  uint32_t start_block = 0;
  uint32_t length = 0;
};

struct AllocationTrace {
  bool success = false;
  bool used_fragmented_fallback = false;
  uint32_t requested_blocks = 0;
  uint32_t selected_extent_start = 0;
  uint32_t selected_extent_length = 0;
  uint32_t blocks_inspected = 0;
  uint32_t extents_examined = 0;
  uint32_t new_cursor = 0;
  std::vector<uint32_t> allocated_blocks;
};

class BlockAllocator {
 public:
  /// Scans [data_start, data_start + data_count) in `block_bitmap` and returns
  /// all contiguous free extents sorted by `start_block`.
  static std::vector<FreeExtent> find_free_extents(
      const std::vector<uint8_t>& block_bitmap,
      uint32_t data_start,
      uint32_t data_count,
      uint32_t* out_blocks_inspected = nullptr);

  /// Allocates `count` blocks from [data_start, data_start + data_count) using `policy`.
  /// On success, sets the corresponding bits in `block_bitmap` and updates `in_out_cursor`.
  /// On failure (insufficient total free blocks), leaves `block_bitmap` untouched.
  static AllocationTrace allocate_blocks(
      std::vector<uint8_t>& block_bitmap,
      uint32_t data_start,
      uint32_t data_count,
      uint32_t count,
      AllocPolicy policy,
      uint32_t& in_out_cursor);
};

}  // namespace ghostfs
