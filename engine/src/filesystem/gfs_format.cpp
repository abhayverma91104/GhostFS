#include "ghostfs/gfs_format.hpp"

#include "ghostfs/crc32.hpp"
#include "ghostfs/endian.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ghostfs {

bool is_supported_block_size(uint32_t block_size) {
  return block_size == 512U || block_size == 1024U ||
         block_size == 2048U || block_size == 4096U;
}

std::string alloc_policy_to_string(AllocPolicy policy) {
  switch (policy) {
    case AllocPolicy::FirstFit:
      return "first_fit";
    case AllocPolicy::BestFit:
      return "best_fit";
    case AllocPolicy::WorstFit:
      return "worst_fit";
    case AllocPolicy::BitmapNextFit:
      return "bitmap_next_fit";
  }
  return "first_fit";
}

AllocPolicy parse_alloc_policy(const std::string& name) {
  if (name == "first_fit" || name == "FIRST_FIT") return AllocPolicy::FirstFit;
  if (name == "best_fit" || name == "BEST_FIT") return AllocPolicy::BestFit;
  if (name == "worst_fit" || name == "WORST_FIT") return AllocPolicy::WorstFit;
  if (name == "bitmap_next_fit" || name == "BITMAP_NEXT_FIT" || name == "bitmap") {
    return AllocPolicy::BitmapNextFit;
  }
  throw std::invalid_argument("Unsupported allocation policy: " + name);
}

void serialize_boot_block(const BootBlock& boot, uint8_t* out_64) {
  std::memset(out_64, 0, kBootBlockBytes);
  std::memcpy(out_64 + 0x00, boot.signature.data(), 8);
  endian::write_u32_le(out_64 + 0x08, boot.version);
  endian::write_u32_le(out_64 + 0x0C, boot.block_size);
  endian::write_u64_le(out_64 + 0x10, boot.total_size_bytes);
  endian::write_u64_le(out_64 + 0x18, boot.created_at_ms);
  endian::write_u32_le(out_64 + 0x20, boot.primary_sb_block);
  endian::write_u32_le(out_64 + 0x24, boot.backup_sb_block);
  const uint32_t crc = compute_crc32(out_64, 0x3C);
  endian::write_u32_le(out_64 + 0x3C, crc);
}

bool deserialize_boot_block(const uint8_t* in_64, BootBlock& out, bool& crc_valid) {
  std::memcpy(out.signature.data(), in_64 + 0x00, 8);
  out.version = endian::read_u32_le(in_64 + 0x08);
  out.block_size = endian::read_u32_le(in_64 + 0x0C);
  out.total_size_bytes = endian::read_u64_le(in_64 + 0x10);
  out.created_at_ms = endian::read_u64_le(in_64 + 0x18);
  out.primary_sb_block = endian::read_u32_le(in_64 + 0x20);
  out.backup_sb_block = endian::read_u32_le(in_64 + 0x24);
  out.crc32 = endian::read_u32_le(in_64 + 0x3C);

  const uint32_t actual_crc = compute_crc32(in_64, 0x3C);
  crc_valid = (actual_crc == out.crc32);
  const bool sig_ok = (std::memcmp(out.signature.data(), "GHOSTFS\0", 8) == 0);
  return sig_ok && out.version == kFormatVersion && is_supported_block_size(out.block_size);
}

void serialize_superblock(const Superblock& sb, uint8_t* out_128) {
  std::memset(out_128, 0, kSuperblockBytes);
  endian::write_u32_le(out_128 + 0x00, sb.magic);
  endian::write_u32_le(out_128 + 0x04, sb.version);
  endian::write_u32_le(out_128 + 0x08, sb.block_size);
  endian::write_u32_le(out_128 + 0x0C, sb.total_blocks);
  endian::write_u32_le(out_128 + 0x10, sb.reserved_blocks);
  endian::write_u32_le(out_128 + 0x14, sb.inode_bitmap_start);
  endian::write_u32_le(out_128 + 0x18, sb.inode_bitmap_blocks);
  endian::write_u32_le(out_128 + 0x1C, sb.block_bitmap_start);
  endian::write_u32_le(out_128 + 0x20, sb.block_bitmap_blocks);
  endian::write_u32_le(out_128 + 0x24, sb.inode_table_start);
  endian::write_u32_le(out_128 + 0x28, sb.inode_table_blocks);
  endian::write_u32_le(out_128 + 0x2C, sb.data_region_start);
  endian::write_u32_le(out_128 + 0x30, sb.data_blocks_count);
  endian::write_u32_le(out_128 + 0x34, sb.journal_start);
  endian::write_u32_le(out_128 + 0x38, sb.journal_blocks);
  endian::write_u32_le(out_128 + 0x3C, sb.total_inodes);
  endian::write_u32_le(out_128 + 0x40, sb.free_inodes);
  endian::write_u32_le(out_128 + 0x44, sb.free_blocks);
  endian::write_u32_le(out_128 + 0x48, sb.root_inode);
  out_128[0x4C] = sb.alloc_policy;
  out_128[0x4D] = sb.fs_state;
  endian::write_u16_le(out_128 + 0x4E, sb.mount_count);
  endian::write_u64_le(out_128 + 0x50, sb.last_mount_ms);
  endian::write_u64_le(out_128 + 0x58, sb.last_write_ms);

  const size_t label_copy = std::min(sb.volume_label.size(), kMaxVolumeLabelLen);
  if (label_copy > 0) {
    std::memcpy(out_128 + 0x60, sb.volume_label.data(), label_copy);
  }
  out_128[0x60 + label_copy] = '\0';

  endian::write_u32_le(out_128 + 0x74, sb.next_alloc_cursor);
  endian::write_u32_le(out_128 + 0x78, sb.next_journal_seq);
  const uint32_t crc = compute_crc32(out_128, 0x7C);
  endian::write_u32_le(out_128 + 0x7C, crc);
}

bool deserialize_superblock(const uint8_t* in_128, Superblock& out, bool& crc_valid) {
  out.magic = endian::read_u32_le(in_128 + 0x00);
  out.version = endian::read_u32_le(in_128 + 0x04);
  out.block_size = endian::read_u32_le(in_128 + 0x08);
  out.total_blocks = endian::read_u32_le(in_128 + 0x0C);
  out.reserved_blocks = endian::read_u32_le(in_128 + 0x10);
  out.inode_bitmap_start = endian::read_u32_le(in_128 + 0x14);
  out.inode_bitmap_blocks = endian::read_u32_le(in_128 + 0x18);
  out.block_bitmap_start = endian::read_u32_le(in_128 + 0x1C);
  out.block_bitmap_blocks = endian::read_u32_le(in_128 + 0x20);
  out.inode_table_start = endian::read_u32_le(in_128 + 0x24);
  out.inode_table_blocks = endian::read_u32_le(in_128 + 0x28);
  out.data_region_start = endian::read_u32_le(in_128 + 0x2C);
  out.data_blocks_count = endian::read_u32_le(in_128 + 0x30);
  out.journal_start = endian::read_u32_le(in_128 + 0x34);
  out.journal_blocks = endian::read_u32_le(in_128 + 0x38);
  out.total_inodes = endian::read_u32_le(in_128 + 0x3C);
  out.free_inodes = endian::read_u32_le(in_128 + 0x40);
  out.free_blocks = endian::read_u32_le(in_128 + 0x44);
  out.root_inode = endian::read_u32_le(in_128 + 0x48);
  out.alloc_policy = in_128[0x4C];
  out.fs_state = in_128[0x4D];
  out.mount_count = endian::read_u16_le(in_128 + 0x4E);
  out.last_mount_ms = endian::read_u64_le(in_128 + 0x50);
  out.last_write_ms = endian::read_u64_le(in_128 + 0x58);

  const char* label_ptr = reinterpret_cast<const char*>(in_128 + 0x60);
  const size_t label_len = ::strnlen(label_ptr, kMaxVolumeLabelLen + 1);
  out.volume_label.assign(label_ptr, std::min(label_len, kMaxVolumeLabelLen));

  out.next_alloc_cursor = endian::read_u32_le(in_128 + 0x74);
  out.next_journal_seq = endian::read_u32_le(in_128 + 0x78);
  out.crc32 = endian::read_u32_le(in_128 + 0x7C);

  const uint32_t actual_crc = compute_crc32(in_128, 0x7C);
  crc_valid = (actual_crc == out.crc32);

  return out.magic == kSuperblockMagic &&
         out.version == kFormatVersion &&
         is_supported_block_size(out.block_size);
}

void serialize_inode(const Inode& inode, uint8_t* out_128) {
  std::memset(out_128, 0, kInodeBytes);
  endian::write_u32_le(out_128 + 0x00, inode.inode_id);
  out_128[0x04] = inode.type;
  out_128[0x05] = inode.state;
  endian::write_u16_le(out_128 + 0x06, inode.permissions);
  endian::write_u32_le(out_128 + 0x08, inode.uid);
  endian::write_u32_le(out_128 + 0x0C, inode.gid);
  endian::write_u64_le(out_128 + 0x10, inode.size_bytes);
  endian::write_u64_le(out_128 + 0x18, inode.created_at_ms);
  endian::write_u64_le(out_128 + 0x20, inode.modified_at_ms);
  endian::write_u64_le(out_128 + 0x28, inode.deleted_at_ms);
  endian::write_u32_le(out_128 + 0x30, inode.link_count);
  endian::write_u32_le(out_128 + 0x34, inode.parent_inode);
  for (size_t i = 0; i < kDirectBlockCount; ++i) {
    endian::write_u32_le(out_128 + 0x38 + i * 4, inode.direct_blocks[i]);
  }
  endian::write_u32_le(out_128 + 0x68, inode.indirect_block);
  endian::write_u32_le(out_128 + 0x6C, inode.double_indirect_block);
  endian::write_u32_le(out_128 + 0x70, inode.content_crc32);
  endian::write_u32_le(out_128 + 0x74, inode.generation);
  endian::write_u32_le(out_128 + 0x78, inode.flags);
  const uint32_t crc = compute_crc32(out_128, 0x7C);
  endian::write_u32_le(out_128 + 0x7C, crc);
}

bool deserialize_inode(const uint8_t* in_128, Inode& out, bool& crc_valid) {
  out.inode_id = endian::read_u32_le(in_128 + 0x00);
  out.type = in_128[0x04];
  out.state = in_128[0x05];
  out.permissions = endian::read_u16_le(in_128 + 0x06);
  out.uid = endian::read_u32_le(in_128 + 0x08);
  out.gid = endian::read_u32_le(in_128 + 0x0C);
  out.size_bytes = endian::read_u64_le(in_128 + 0x10);
  out.created_at_ms = endian::read_u64_le(in_128 + 0x18);
  out.modified_at_ms = endian::read_u64_le(in_128 + 0x20);
  out.deleted_at_ms = endian::read_u64_le(in_128 + 0x28);
  out.link_count = endian::read_u32_le(in_128 + 0x30);
  out.parent_inode = endian::read_u32_le(in_128 + 0x34);
  for (size_t i = 0; i < kDirectBlockCount; ++i) {
    out.direct_blocks[i] = endian::read_u32_le(in_128 + 0x38 + i * 4);
  }
  out.indirect_block = endian::read_u32_le(in_128 + 0x68);
  out.double_indirect_block = endian::read_u32_le(in_128 + 0x6C);
  out.content_crc32 = endian::read_u32_le(in_128 + 0x70);
  out.generation = endian::read_u32_le(in_128 + 0x74);
  out.flags = endian::read_u32_le(in_128 + 0x78);
  out.inode_crc32 = endian::read_u32_le(in_128 + 0x7C);

  bool all_zero = true;
  for (size_t i = 0; i < kInodeBytes; ++i) {
    if (in_128[i] != 0U) {
      all_zero = false;
      break;
    }
  }
  if (all_zero) {
    crc_valid = true;
    return true;
  }

  const uint32_t actual_crc = compute_crc32(in_128, 0x7C);
  crc_valid = (actual_crc == out.inode_crc32);
  return true;
}

void serialize_dir_entry(const DirEntry& entry, uint8_t* out_64) {
  std::memset(out_64, 0, kDirEntryBytes);
  endian::write_u32_le(out_64 + 0x00, entry.inode_id);
  out_64[0x04] = entry.entry_type;
  out_64[0x05] = entry.state;
  const uint16_t len = static_cast<uint16_t>(std::min(entry.name.size(), kMaxNameLen));
  endian::write_u16_le(out_64 + 0x06, len);
  if (len > 0) {
    std::memcpy(out_64 + 0x08, entry.name.data(), len);
  }
  out_64[0x08 + len] = '\0';
}

bool deserialize_dir_entry(const uint8_t* in_64, DirEntry& out) {
  out.inode_id = endian::read_u32_le(in_64 + 0x00);
  out.entry_type = in_64[0x04];
  out.state = in_64[0x05];
  out.name_len = endian::read_u16_le(in_64 + 0x06);
  if (out.name_len > kMaxNameLen) {
    out.name.clear();
    return false;
  }
  out.name.assign(reinterpret_cast<const char*>(in_64 + 0x08), out.name_len);
  return true;
}

void serialize_journal_entry(const JournalEntry& entry, uint8_t* out_64) {
  std::memset(out_64, 0, kJournalEntryBytes);
  endian::write_u64_le(out_64 + 0x00, entry.seq_no);
  endian::write_u64_le(out_64 + 0x08, entry.timestamp_ms);
  out_64[0x10] = entry.op_type;
  out_64[0x11] = entry.status;
  endian::write_u16_le(out_64 + 0x12, entry.reserved);
  endian::write_u32_le(out_64 + 0x14, entry.target_inode);
  endian::write_u32_le(out_64 + 0x18, entry.target_block);
  endian::write_u32_le(out_64 + 0x1C, entry.aux_value);

  const size_t summary_len = std::min(entry.summary.size(), kMaxJournalSummaryLen);
  if (summary_len > 0) {
    std::memcpy(out_64 + 0x20, entry.summary.data(), summary_len);
  }
  out_64[0x20 + summary_len] = '\0';

  const uint32_t crc = compute_crc32(out_64, 0x3C);
  endian::write_u32_le(out_64 + 0x3C, crc);
}

bool deserialize_journal_entry(const uint8_t* in_64, JournalEntry& out, bool& crc_valid) {
  out.seq_no = endian::read_u64_le(in_64 + 0x00);
  out.timestamp_ms = endian::read_u64_le(in_64 + 0x08);
  out.op_type = in_64[0x10];
  out.status = in_64[0x11];
  out.reserved = endian::read_u16_le(in_64 + 0x12);
  out.target_inode = endian::read_u32_le(in_64 + 0x14);
  out.target_block = endian::read_u32_le(in_64 + 0x18);
  out.aux_value = endian::read_u32_le(in_64 + 0x1C);

  const char* sum_ptr = reinterpret_cast<const char*>(in_64 + 0x20);
  const size_t sum_len = ::strnlen(sum_ptr, kMaxJournalSummaryLen + 1);
  out.summary.assign(sum_ptr, std::min(sum_len, kMaxJournalSummaryLen));
  out.crc32 = endian::read_u32_le(in_64 + 0x3C);

  const uint32_t actual_crc = compute_crc32(in_64, 0x3C);
  crc_valid = (actual_crc == out.crc32);
  return true;
}

}  // namespace ghostfs
