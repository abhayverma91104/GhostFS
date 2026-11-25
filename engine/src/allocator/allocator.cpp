#include "ghostfs/allocator.hpp"

#include <algorithm>
#include <stdexcept>

namespace ghostfs {

namespace {

inline bool get_bit(const std::vector<uint8_t>& bitmap, uint32_t idx) {
  return (bitmap[idx / 8U] & static_cast<uint8_t>(1U << (idx % 8U))) != 0;
}

inline void set_bit(std::vector<uint8_t>& bitmap, uint32_t idx) {
  bitmap[idx / 8U] |= static_cast<uint8_t>(1U << (idx % 8U));
}

}  // namespace

std::vector<FreeExtent> BlockAllocator::find_free_extents(
    const std::vector<uint8_t>& block_bitmap,
    uint32_t data_start,
    uint32_t data_count,
    uint32_t* out_blocks_inspected) {
  std::vector<FreeExtent> extents;
  if (data_count == 0U) {
    if (out_blocks_inspected) *out_blocks_inspected = 0U;
    return extents;
  }

  const uint32_t data_end = data_start + data_count;
  uint32_t inspected = 0U;
  bool in_free = false;
  uint32_t run_start = 0U;

  for (uint32_t b = data_start; b < data_end; ++b) {
    ++inspected;
    const bool used = get_bit(block_bitmap, b);
    if (!used && !in_free) {
      in_free = true;
      run_start = b;
    } else if (used && in_free) {
      extents.push_back(FreeExtent{run_start, b - run_start});
      in_free = false;
    }
  }
  if (in_free) {
    extents.push_back(FreeExtent{run_start, data_end - run_start});
  }
  if (out_blocks_inspected) {
    *out_blocks_inspected = inspected;
  }
  return extents;
}

AllocationTrace BlockAllocator::allocate_blocks(
    std::vector<uint8_t>& block_bitmap,
    uint32_t data_start,
    uint32_t data_count,
    uint32_t count,
    AllocPolicy policy,
    uint32_t& in_out_cursor) {
  AllocationTrace trace{};
  trace.requested_blocks = count;
  if (count == 0U) {
    trace.success = true;
    trace.new_cursor = in_out_cursor;
    return trace;
  }
  if (data_count == 0U) {
    return trace;
  }

  const uint32_t data_end = data_start + data_count;
  if (in_out_cursor < data_start || in_out_cursor >= data_end) {
    in_out_cursor = data_start;
  }

  uint32_t scan_inspected = 0U;
  const std::vector<FreeExtent> extents =
      find_free_extents(block_bitmap, data_start, data_count, &scan_inspected);
  trace.blocks_inspected = scan_inspected;

  uint64_t total_free = 0ULL;
  for (const auto& ext : extents) {
    total_free += ext.length;
  }
  if (total_free < count) {
    trace.success = false;
    trace.new_cursor = in_out_cursor;
    return trace;
  }

  bool found_contiguous = false;
  uint32_t chosen_start = 0U;
  uint32_t chosen_extent_len = 0U;

  if (policy == AllocPolicy::FirstFit) {
    for (const auto& ext : extents) {
      ++trace.extents_examined;
      if (ext.length >= count) {
        chosen_start = ext.start_block;
        chosen_extent_len = ext.length;
        found_contiguous = true;
        break;
      }
    }
  } else if (policy == AllocPolicy::BestFit) {
    for (const auto& ext : extents) {
      ++trace.extents_examined;
      if (ext.length >= count) {
        if (!found_contiguous || ext.length < chosen_extent_len ||
            (ext.length == chosen_extent_len && ext.start_block < chosen_start)) {
          chosen_start = ext.start_block;
          chosen_extent_len = ext.length;
          found_contiguous = true;
        }
      }
    }
  } else if (policy == AllocPolicy::WorstFit) {
    for (const auto& ext : extents) {
      ++trace.extents_examined;
      if (ext.length >= count) {
        if (!found_contiguous || ext.length > chosen_extent_len ||
            (ext.length == chosen_extent_len && ext.start_block < chosen_start)) {
          chosen_start = ext.start_block;
          chosen_extent_len = ext.length;
          found_contiguous = true;
        }
      }
    }
  } else if (policy == AllocPolicy::BitmapNextFit) {
    // Pass 1: inspect extents at or after `in_out_cursor`.
    for (const auto& ext : extents) {
      const uint32_t ext_end = ext.start_block + ext.length;
      if (ext_end <= in_out_cursor) continue;
      ++trace.extents_examined;
      const uint32_t eff_start = std::max(ext.start_block, in_out_cursor);
      const uint32_t eff_len = ext_end - eff_start;
      if (eff_len >= count) {
        chosen_start = eff_start;
        chosen_extent_len = eff_len;
        found_contiguous = true;
        break;
      }
    }
    // Pass 2: wrap around to [data_start, in_out_cursor).
    if (!found_contiguous) {
      for (const auto& ext : extents) {
        if (ext.start_block >= in_out_cursor) break;
        ++trace.extents_examined;
        if (ext.length >= count) {
          chosen_start = ext.start_block;
          chosen_extent_len = ext.length;
          found_contiguous = true;
          break;
        }
      }
    }
  }

  if (found_contiguous) {
    trace.success = true;
    trace.used_fragmented_fallback = false;
    trace.selected_extent_start = chosen_start;
    trace.selected_extent_length = chosen_extent_len;
    trace.allocated_blocks.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t blk = chosen_start + i;
      set_bit(block_bitmap, blk);
      trace.allocated_blocks.push_back(blk);
    }
    const uint32_t next_pos = chosen_start + count;
    in_out_cursor = (next_pos >= data_end) ? data_start : next_pos;
    trace.new_cursor = in_out_cursor;
    return trace;
  }

  // Fragmented allocation fallback: total_free >= count, but no single contiguous extent fits.
  trace.success = true;
  trace.used_fragmented_fallback = true;
  trace.allocated_blocks.reserve(count);

  if (policy == AllocPolicy::BestFit || policy == AllocPolicy::WorstFit) {
    std::vector<FreeExtent> sorted = extents;
    std::sort(sorted.begin(), sorted.end(), [](const FreeExtent& a, const FreeExtent& b) {
      if (a.length != b.length) return a.length > b.length;
      return a.start_block < b.start_block;
    });
    for (const auto& ext : sorted) {
      for (uint32_t i = 0; i < ext.length && trace.allocated_blocks.size() < count; ++i) {
        const uint32_t blk = ext.start_block + i;
        set_bit(block_bitmap, blk);
        trace.allocated_blocks.push_back(blk);
      }
      if (trace.allocated_blocks.size() >= count) break;
    }
  } else if (policy == AllocPolicy::BitmapNextFit) {
    for (uint32_t offset = 0; offset < data_count && trace.allocated_blocks.size() < count; ++offset) {
      const uint32_t blk = data_start + ((in_out_cursor - data_start + offset) % data_count);
      if (!get_bit(block_bitmap, blk)) {
        set_bit(block_bitmap, blk);
        trace.allocated_blocks.push_back(blk);
        const uint32_t next_pos = blk + 1U;
        in_out_cursor = (next_pos >= data_end) ? data_start : next_pos;
      }
    }
  } else {
    // FirstFit fragmented fallback: ascending LBA order.
    for (const auto& ext : extents) {
      for (uint32_t i = 0; i < ext.length && trace.allocated_blocks.size() < count; ++i) {
        const uint32_t blk = ext.start_block + i;
        set_bit(block_bitmap, blk);
        trace.allocated_blocks.push_back(blk);
      }
      if (trace.allocated_blocks.size() >= count) break;
    }
  }

  trace.new_cursor = in_out_cursor;
  return trace;
}

}  // namespace ghostfs
