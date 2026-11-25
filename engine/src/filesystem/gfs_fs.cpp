#include "ghostfs/gfs_fs.hpp"

#include "ghostfs/crc32.hpp"
#include "ghostfs/endian.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <stdexcept>

namespace ghostfs {

namespace {

uint64_t now_ms() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

uint32_t ceil_div(uint32_t num, uint32_t den) {
  return (num + den - 1U) / den;
}

}  // namespace

std::vector<std::string> GfsFileSystem::split_path(const std::string& path) {
  if (path.empty() || path[0] != '/') {
    throw std::invalid_argument("GFS path must be absolute starting with '/'");
  }
  std::vector<std::string> parts;
  size_t i = 1;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    if (i >= path.size()) break;
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    const std::string comp = path.substr(i, j - i);
    if (comp == "." || comp == "..") {
      throw std::invalid_argument("Relative path segments '.' and '..' are not allowed");
    }
    if (comp.size() > kMaxNameLen) {
      throw std::invalid_argument("Filename exceeds maximum GFS1 length of 55 bytes");
    }
    if (comp.find('\0') != std::string::npos) {
      throw std::invalid_argument("Filename cannot contain null bytes");
    }
    parts.push_back(comp);
    i = j;
  }
  return parts;
}

std::string GfsFileSystem::normalize_path(const std::string& path) {
  const auto parts = split_path(path);
  if (parts.empty()) return "/";
  std::string out;
  for (const auto& p : parts) {
    out += "/" + p;
  }
  return out;
}

bool GfsFileSystem::get_bitmap_bit(const std::vector<uint8_t>& bitmap, uint32_t idx) const {
  const size_t byte_idx = idx / 8U;
  const uint8_t bit_mask = static_cast<uint8_t>(1U << (idx % 8U));
  if (byte_idx >= bitmap.size()) {
    throw std::out_of_range("Bitmap index out of bounds");
  }
  return (bitmap[byte_idx] & bit_mask) != 0;
}

void GfsFileSystem::set_bitmap_bit(std::vector<uint8_t>& bitmap, uint32_t idx, bool value) {
  const size_t byte_idx = idx / 8U;
  const uint8_t bit_mask = static_cast<uint8_t>(1U << (idx % 8U));
  if (byte_idx >= bitmap.size()) {
    throw std::out_of_range("Bitmap index out of bounds");
  }
  if (value) {
    bitmap[byte_idx] |= bit_mask;
  } else {
    bitmap[byte_idx] &= static_cast<uint8_t>(~bit_mask);
  }
}

void GfsFileSystem::load_bitmaps() {
  inode_bitmap_.resize(static_cast<size_t>(sb_.inode_bitmap_blocks) * sb_.block_size);
  std::vector<uint8_t> blk;
  for (uint32_t i = 0; i < sb_.inode_bitmap_blocks; ++i) {
    disk_->read_block(sb_.inode_bitmap_start + i, blk);
    std::memcpy(inode_bitmap_.data() + static_cast<size_t>(i) * sb_.block_size, blk.data(), sb_.block_size);
  }

  block_bitmap_.resize(static_cast<size_t>(sb_.block_bitmap_blocks) * sb_.block_size);
  for (uint32_t i = 0; i < sb_.block_bitmap_blocks; ++i) {
    disk_->read_block(sb_.block_bitmap_start + i, blk);
    std::memcpy(block_bitmap_.data() + static_cast<size_t>(i) * sb_.block_size, blk.data(), sb_.block_size);
  }
}

void GfsFileSystem::flush_inode_bitmap() {
  std::vector<uint8_t> blk(sb_.block_size, 0U);
  for (uint32_t i = 0; i < sb_.inode_bitmap_blocks; ++i) {
    std::memcpy(blk.data(), inode_bitmap_.data() + static_cast<size_t>(i) * sb_.block_size, sb_.block_size);
    disk_->write_block(sb_.inode_bitmap_start + i, blk);
  }
}

void GfsFileSystem::flush_block_bitmap() {
  std::vector<uint8_t> blk(sb_.block_size, 0U);
  for (uint32_t i = 0; i < sb_.block_bitmap_blocks; ++i) {
    std::memcpy(blk.data(), block_bitmap_.data() + static_cast<size_t>(i) * sb_.block_size, sb_.block_size);
    disk_->write_block(sb_.block_bitmap_start + i, blk);
  }
}

void GfsFileSystem::flush_superblocks() {
  std::vector<uint8_t> blk(sb_.block_size, 0U);
  serialize_superblock(sb_, blk.data());
  disk_->write_block(1U, blk);
  disk_->write_block(2U, blk);
}

Inode GfsFileSystem::read_inode(uint32_t inode_id) const {
  if (inode_id == 0 || inode_id >= sb_.total_inodes) {
    throw std::out_of_range("Inode ID out of range: " + std::to_string(inode_id));
  }
  const uint32_t inodes_per_block = sb_.block_size / static_cast<uint32_t>(kInodeBytes);
  const uint32_t block_offset = inode_id / inodes_per_block;
  const uint32_t slot_in_block = inode_id % inodes_per_block;

  std::vector<uint8_t> blk;
  disk_->read_block(sb_.inode_table_start + block_offset, blk);

  Inode out{};
  bool crc_ok = false;
  deserialize_inode(blk.data() + static_cast<size_t>(slot_in_block) * kInodeBytes, out, crc_ok);
  if (!crc_ok) {
    throw std::runtime_error("Inode checksum mismatch on inode " + std::to_string(inode_id));
  }
  return out;
}

void GfsFileSystem::write_inode(const Inode& inode) {
  if (inode.inode_id == 0 || inode.inode_id >= sb_.total_inodes) {
    throw std::out_of_range("Inode ID out of range for write: " + std::to_string(inode.inode_id));
  }
  const uint32_t inodes_per_block = sb_.block_size / static_cast<uint32_t>(kInodeBytes);
  const uint32_t block_offset = inode.inode_id / inodes_per_block;
  const uint32_t slot_in_block = inode.inode_id % inodes_per_block;

  std::vector<uint8_t> blk;
  disk_->read_block(sb_.inode_table_start + block_offset, blk);
  serialize_inode(inode, blk.data() + static_cast<size_t>(slot_in_block) * kInodeBytes);
  disk_->write_block(sb_.inode_table_start + block_offset, blk);
}

void GfsFileSystem::format(
    VirtualDisk& disk,
    const std::string& volume_label,
    AllocPolicy policy,
    uint32_t requested_inodes) {
  if (!disk.is_open() || !disk.is_writable()) {
    throw std::runtime_error("VirtualDisk must be opened in write mode to format");
  }

  disk_ = &disk;
  write_mode_ = true;

  const uint32_t block_size = disk.block_size();
  const uint32_t total_blocks = disk.total_blocks();
  const uint32_t inodes_per_block = block_size / static_cast<uint32_t>(kInodeBytes);
  const uint32_t journal_blocks = (total_blocks >= 128U) ? 16U : 4U;

  uint32_t total_inodes = requested_inodes;
  if (total_inodes == 0U) {
    uint32_t target = std::max<uint32_t>(inodes_per_block, total_blocks / 4U);
    target = std::min<uint32_t>(target, 4096U);
    total_inodes = ceil_div(target, inodes_per_block) * inodes_per_block;
  } else if (total_inodes < 2U) {
    throw std::invalid_argument("total_inodes must be at least 2 (slot 0 reserved + root '/')");
  }

  const uint32_t inode_bitmap_start = 3U;
  const uint32_t inode_bitmap_blocks = ceil_div(total_inodes, block_size * 8U);
  const uint32_t block_bitmap_start = inode_bitmap_start + inode_bitmap_blocks;
  const uint32_t block_bitmap_blocks = ceil_div(total_blocks, block_size * 8U);
  const uint32_t inode_table_start = block_bitmap_start + block_bitmap_blocks;
  const uint32_t inode_table_blocks = ceil_div(total_inodes * static_cast<uint32_t>(kInodeBytes), block_size);
  const uint32_t data_region_start = inode_table_start + inode_table_blocks;

  if (data_region_start + journal_blocks + 4U > total_blocks) {
    throw std::invalid_argument("Disk size is too small for GFS1 metadata and data regions");
  }

  const uint32_t journal_start = total_blocks - journal_blocks;
  const uint32_t data_blocks_count = journal_start - data_region_start;
  const uint64_t ts = now_ms();

  boot_ = BootBlock{};
  boot_.version = kFormatVersion;
  boot_.block_size = block_size;
  boot_.total_size_bytes = disk.file_size_bytes();
  boot_.created_at_ms = ts;
  boot_.primary_sb_block = 1U;
  boot_.backup_sb_block = 2U;

  sb_ = Superblock{};
  sb_.magic = kSuperblockMagic;
  sb_.version = kFormatVersion;
  sb_.block_size = block_size;
  sb_.total_blocks = total_blocks;
  sb_.reserved_blocks = 3U;
  sb_.inode_bitmap_start = inode_bitmap_start;
  sb_.inode_bitmap_blocks = inode_bitmap_blocks;
  sb_.block_bitmap_start = block_bitmap_start;
  sb_.block_bitmap_blocks = block_bitmap_blocks;
  sb_.inode_table_start = inode_table_start;
  sb_.inode_table_blocks = inode_table_blocks;
  sb_.data_region_start = data_region_start;
  sb_.data_blocks_count = data_blocks_count;
  sb_.journal_start = journal_start;
  sb_.journal_blocks = journal_blocks;
  sb_.total_inodes = total_inodes;
  sb_.free_inodes = total_inodes - 2U;  // 0 reserved + 1 root
  sb_.free_blocks = data_blocks_count - 1U;  // root dir initial block
  sb_.root_inode = kRootInodeId;
  sb_.alloc_policy = static_cast<uint8_t>(policy);
  sb_.fs_state = static_cast<uint8_t>(FsState::Clean);
  sb_.mount_count = 0U;
  sb_.last_mount_ms = ts;
  sb_.last_write_ms = ts;
  sb_.volume_label = volume_label.empty() ? "GhostFS" : volume_label.substr(0, kMaxVolumeLabelLen);
  sb_.next_alloc_cursor = data_region_start + 1U;
  sb_.next_journal_seq = 1U;

  // Zero all metadata & journal blocks.
  const std::vector<uint8_t> zero_blk(block_size, 0U);
  for (uint32_t b = 0; b < data_region_start; ++b) {
    disk_->write_block(b, zero_blk);
  }
  for (uint32_t b = journal_start; b < total_blocks; ++b) {
    disk_->write_block(b, zero_blk);
  }

  // Initialize bitmaps.
  inode_bitmap_.assign(static_cast<size_t>(inode_bitmap_blocks) * block_size, 0U);
  set_bitmap_bit(inode_bitmap_, 0U, true);  // Reserved null slot
  set_bitmap_bit(inode_bitmap_, kRootInodeId, true);

  block_bitmap_.assign(static_cast<size_t>(block_bitmap_blocks) * block_size, 0U);
  for (uint32_t b = 0; b < data_region_start; ++b) {
    set_bitmap_bit(block_bitmap_, b, true);
  }
  for (uint32_t b = journal_start; b < total_blocks; ++b) {
    set_bitmap_bit(block_bitmap_, b, true);
  }

  // Allocate first data block for root directory '/'.
  const uint32_t root_data_block = data_region_start;
  set_bitmap_bit(block_bitmap_, root_data_block, true);
  disk_->write_block(root_data_block, zero_blk);

  Inode root{};
  root.inode_id = kRootInodeId;
  root.type = static_cast<uint8_t>(InodeType::Directory);
  root.state = static_cast<uint8_t>(InodeState::Allocated);
  root.permissions = 0755;
  root.created_at_ms = ts;
  root.modified_at_ms = ts;
  root.link_count = 2U;
  root.parent_inode = kRootInodeId;
  root.direct_blocks[0] = root_data_block;
  root.generation = 1U;

  // Write BootBlock, Bitmaps, Root Inode, Superblocks, and initial Journal entry.
  std::vector<uint8_t> boot_buf(block_size, 0U);
  serialize_boot_block(boot_, boot_buf.data());
  disk_->write_block(0U, boot_buf);

  flush_inode_bitmap();
  flush_block_bitmap();
  write_inode(root);

  const uint64_t seq = begin_journal(JournalOp::Format, kRootInodeId, root_data_block, block_size, "format GFS1");
  commit_journal(seq, JournalOp::Format, kRootInodeId, root_data_block, block_size, "format GFS1");

  boot_crc_ok_ = true;
  primary_sb_crc_ok_ = true;
  backup_sb_crc_ok_ = true;
  dirty_journal_detected_ = false;
  disk_->sync();
}

void GfsFileSystem::mount(VirtualDisk& disk, bool write_mode) {
  if (!disk.is_open()) {
    throw std::runtime_error("VirtualDisk must be open before mounting GfsFileSystem");
  }
  if (write_mode && !disk.is_writable()) {
    throw std::runtime_error("Disk opened read-only cannot mount in write mode");
  }

  disk_ = &disk;
  write_mode_ = write_mode;

  // Probe BootBlock first to determine authoritative block size.
  std::vector<uint8_t> raw_boot;
  disk_->read_raw(0, kBootBlockBytes, raw_boot);
  BootBlock probed_boot{};
  bool boot_crc_ok = false;
  const bool boot_struct_ok = deserialize_boot_block(raw_boot.data(), probed_boot, boot_crc_ok);
  if (boot_struct_ok && is_supported_block_size(probed_boot.block_size)) {
    disk_->set_block_size(probed_boot.block_size);
  }
  boot_ = probed_boot;
  boot_crc_ok_ = boot_crc_ok;

  std::vector<uint8_t> sb_buf;
  disk_->read_block(1U, sb_buf);
  bool primary_crc = false;
  if (!deserialize_superblock(sb_buf.data(), sb_, primary_crc)) {
    throw std::runtime_error("Invalid GFS1 primary superblock magic or version");
  }
  primary_sb_crc_ok_ = primary_crc;
  if (!primary_sb_crc_ok_) {
    throw std::runtime_error("Primary superblock CRC32 verification failed");
  }
  if (sb_.block_size != disk_->block_size() || sb_.total_blocks != disk_->total_blocks()) {
    throw std::runtime_error("Superblock geometry does not match virtual disk image size");
  }
  if (sb_.data_region_start >= sb_.journal_start || sb_.journal_start + sb_.journal_blocks > sb_.total_blocks) {
    throw std::runtime_error("Corrupted superblock region boundaries");
  }

  std::vector<uint8_t> backup_buf;
  disk_->read_block(2U, backup_buf);
  Superblock backup_sb{};
  bool backup_crc = false;
  const bool backup_ok = deserialize_superblock(backup_buf.data(), backup_sb, backup_crc);
  backup_sb_crc_ok_ = backup_ok && backup_crc;

  load_bitmaps();

  dirty_journal_detected_ = (sb_.fs_state != static_cast<uint8_t>(FsState::Clean));
  for (const auto& entry : read_journal_entries()) {
    if (entry.status == static_cast<uint8_t>(JournalStatus::Pending)) {
      dirty_journal_detected_ = true;
      break;
    }
  }

  if (write_mode_) {
    sb_.mount_count = static_cast<uint16_t>(sb_.mount_count + 1U);
    sb_.last_mount_ms = now_ms();
    flush_superblocks();
    disk_->sync();
  }
}

VolumeStatus GfsFileSystem::status() const {
  if (disk_ == nullptr || !disk_->is_open()) {
    throw std::runtime_error("File system is not mounted");
  }
  VolumeStatus st{};
  st.disk_name = disk_->path().filename().string();
  st.volume_label = sb_.volume_label;
  st.block_size = sb_.block_size;
  st.total_blocks = sb_.total_blocks;
  st.reserved_blocks = sb_.data_region_start + sb_.journal_blocks;
  st.data_blocks_count = sb_.data_blocks_count;
  st.free_blocks = sb_.free_blocks;
  st.allocated_data_blocks = (sb_.data_blocks_count >= sb_.free_blocks)
                                 ? (sb_.data_blocks_count - sb_.free_blocks)
                                 : 0U;
  st.total_inodes = sb_.total_inodes;
  st.free_inodes = sb_.free_inodes;
  st.used_inodes = (sb_.total_inodes >= sb_.free_inodes)
                       ? (sb_.total_inodes - sb_.free_inodes)
                       : 0U;
  st.total_bytes = static_cast<uint64_t>(sb_.total_blocks) * sb_.block_size;
  st.usable_data_bytes = static_cast<uint64_t>(sb_.data_blocks_count) * sb_.block_size;
  st.used_data_bytes = static_cast<uint64_t>(st.allocated_data_blocks) * sb_.block_size;
  st.alloc_policy = alloc_policy_to_string(static_cast<AllocPolicy>(sb_.alloc_policy));
  st.fs_state = sb_.fs_state;
  st.primary_sb_crc_ok = primary_sb_crc_ok_;
  st.backup_sb_crc_ok = backup_sb_crc_ok_;
  st.boot_crc_ok = boot_crc_ok_;
  st.dirty_journal_detected = dirty_journal_detected_;
  st.mount_count = sb_.mount_count;
  st.last_mount_ms = sb_.last_mount_ms;
  st.last_write_ms = sb_.last_write_ms;
  st.superblock = sb_;
  st.recent_journal = read_journal_entries();
  return st;
}

uint32_t GfsFileSystem::allocate_inode_slot() {
  if (sb_.free_inodes == 0) {
    throw std::runtime_error("ERR_NO_FREE_INODES: No free inodes remaining on volume");
  }
  for (uint32_t id = 1; id < sb_.total_inodes; ++id) {
    if (!get_bitmap_bit(inode_bitmap_, id)) {
      set_bitmap_bit(inode_bitmap_, id, true);
      sb_.free_inodes -= 1U;
      return id;
    }
  }
  throw std::runtime_error("ERR_NO_FREE_INODES: Inode bitmap exhausted");
}

void GfsFileSystem::release_inode_slot(uint32_t inode_id) {
  if (get_bitmap_bit(inode_bitmap_, inode_id)) {
    set_bitmap_bit(inode_bitmap_, inode_id, false);
    sb_.free_inodes += 1U;
  }
}

void GfsFileSystem::require_clean_for_write() const {
  if (!write_mode_) {
    throw std::runtime_error("File system mounted read-only");
  }
  if (dirty_journal_detected_ || sb_.fs_state != static_cast<uint8_t>(FsState::Clean)) {
    throw std::runtime_error(
        "ERR_DIRTY_FILESYSTEM: Disk image is marked Dirty due to an interrupted operation; "
        "read-only inspection is permitted, but mutations are blocked until repaired or reformatted");
  }
}

uint32_t GfsFileSystem::allocate_one_data_block() {
  const auto blocks = allocate_data_blocks(1U);
  return blocks.at(0);
}

std::vector<uint32_t> GfsFileSystem::allocate_data_blocks(uint32_t count) {
  if (count == 0) return {};
  if (sb_.free_blocks < count) {
    throw std::runtime_error("ERR_NO_SPACE: Insufficient disk space (need " +
                             std::to_string(count) + " blocks, free " +
                             std::to_string(sb_.free_blocks) + ")");
  }
  last_alloc_trace_ = BlockAllocator::allocate_blocks(
      block_bitmap_,
      sb_.data_region_start,
      sb_.data_blocks_count,
      count,
      static_cast<AllocPolicy>(sb_.alloc_policy),
      sb_.next_alloc_cursor);
  if (!last_alloc_trace_.success || last_alloc_trace_.allocated_blocks.size() != count) {
    throw std::runtime_error("ERR_NO_SPACE: Block allocator could not satisfy request");
  }
  sb_.free_blocks -= count;
  return last_alloc_trace_.allocated_blocks;
}

void GfsFileSystem::release_data_block(uint32_t block_idx) {
  if (block_idx < sb_.data_region_start || block_idx >= sb_.data_region_start + sb_.data_blocks_count) {
    return;
  }
  if (get_bitmap_bit(block_bitmap_, block_idx)) {
    set_bitmap_bit(block_bitmap_, block_idx, false);
    sb_.free_blocks += 1U;
  }
}

uint32_t GfsFileSystem::indirect_blocks_needed(uint32_t data_block_count) const {
  if (data_block_count <= kDirectBlockCount) return 0U;
  const uint32_t ptrs = sb_.block_size / 4U;
  uint32_t remaining = data_block_count - kDirectBlockCount;
  if (remaining <= ptrs) {
    return 1U;  // Single indirect only
  }
  remaining -= ptrs;
  const uint64_t max_double = static_cast<uint64_t>(ptrs) * static_cast<uint64_t>(ptrs);
  if (remaining > max_double) {
    throw std::runtime_error("File exceeds maximum GFS1 double-indirect capacity");
  }
  const uint32_t inner_tables = ceil_div(remaining, ptrs);
  return 1U + 1U + inner_tables;  // single indirect + double indirect root + inner tables
}

void GfsFileSystem::collect_inode_block_lists(
    const Inode& inode,
    std::vector<uint32_t>& out_data_blocks,
    std::vector<uint32_t>& out_indirect_blocks) const {
  out_data_blocks.clear();
  out_indirect_blocks.clear();

  for (uint32_t i = 0; i < kDirectBlockCount; ++i) {
    if (inode.direct_blocks[i] != 0U) {
      out_data_blocks.push_back(inode.direct_blocks[i]);
    }
  }

  const uint32_t ptrs = sb_.block_size / 4U;
  if (inode.indirect_block != 0U) {
    out_indirect_blocks.push_back(inode.indirect_block);
    std::vector<uint8_t> blk;
    disk_->read_block(inode.indirect_block, blk);
    for (uint32_t i = 0; i < ptrs; ++i) {
      const uint32_t ptr = endian::read_u32_le(blk.data() + static_cast<size_t>(i) * 4U);
      if (ptr != 0U) {
        out_data_blocks.push_back(ptr);
      }
    }
  }

  if (inode.double_indirect_block != 0U) {
    out_indirect_blocks.push_back(inode.double_indirect_block);
    std::vector<uint8_t> outer_blk;
    disk_->read_block(inode.double_indirect_block, outer_blk);
    for (uint32_t i = 0; i < ptrs; ++i) {
      const uint32_t inner_blk_idx = endian::read_u32_le(outer_blk.data() + static_cast<size_t>(i) * 4U);
      if (inner_blk_idx == 0U) continue;
      out_indirect_blocks.push_back(inner_blk_idx);
      std::vector<uint8_t> inner_blk;
      disk_->read_block(inner_blk_idx, inner_blk);
      for (uint32_t j = 0; j < ptrs; ++j) {
        const uint32_t ptr = endian::read_u32_le(inner_blk.data() + static_cast<size_t>(j) * 4U);
        if (ptr != 0U) {
          out_data_blocks.push_back(ptr);
        }
      }
    }
  }
}

void GfsFileSystem::populate_inode_block_pointers(
    Inode& inode,
    const std::vector<uint32_t>& data_blocks,
    const std::vector<uint32_t>& indirect_blocks) {
  inode.direct_blocks.fill(0U);
  inode.indirect_block = 0U;
  inode.double_indirect_block = 0U;

  const size_t direct_n = std::min<size_t>(data_blocks.size(), kDirectBlockCount);
  for (size_t i = 0; i < direct_n; ++i) {
    inode.direct_blocks[i] = data_blocks[i];
  }
  if (data_blocks.size() <= kDirectBlockCount) {
    return;
  }

  const uint32_t ptrs = sb_.block_size / 4U;
  size_t indirect_cursor = 0U;
  inode.indirect_block = indirect_blocks.at(indirect_cursor++);
  std::vector<uint8_t> single_buf(sb_.block_size, 0U);
  const size_t single_end = std::min<size_t>(data_blocks.size(), kDirectBlockCount + ptrs);
  for (size_t idx = kDirectBlockCount; idx < single_end; ++idx) {
    endian::write_u32_le(single_buf.data() + (idx - kDirectBlockCount) * 4U, data_blocks[idx]);
  }
  disk_->write_block(inode.indirect_block, single_buf);

  if (data_blocks.size() <= kDirectBlockCount + ptrs) {
    return;
  }

  inode.double_indirect_block = indirect_blocks.at(indirect_cursor++);
  std::vector<uint8_t> outer_buf(sb_.block_size, 0U);
  size_t data_idx = kDirectBlockCount + ptrs;
  size_t outer_slot = 0;
  while (data_idx < data_blocks.size()) {
    const uint32_t inner_blk_id = indirect_blocks.at(indirect_cursor++);
    endian::write_u32_le(outer_buf.data() + outer_slot * 4U, inner_blk_id);
    std::vector<uint8_t> inner_buf(sb_.block_size, 0U);
    for (size_t j = 0; j < ptrs && data_idx < data_blocks.size(); ++j, ++data_idx) {
      endian::write_u32_le(inner_buf.data() + j * 4U, data_blocks[data_idx]);
    }
    disk_->write_block(inner_blk_id, inner_buf);
    ++outer_slot;
  }
  disk_->write_block(inode.double_indirect_block, outer_buf);
}

void GfsFileSystem::write_data_to_blocks(const std::vector<uint32_t>& blocks, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> buf(sb_.block_size, 0U);
  for (size_t i = 0; i < blocks.size(); ++i) {
    std::fill(buf.begin(), buf.end(), 0U);
    const size_t offset = i * sb_.block_size;
    if (offset < data.size()) {
      const size_t copy_len = std::min<size_t>(sb_.block_size, data.size() - offset);
      std::memcpy(buf.data(), data.data() + offset, copy_len);
    }
    disk_->write_block(blocks[i], buf);
  }
}

std::vector<DirEntry> GfsFileSystem::read_dir_entries(const Inode& dir_inode) const {
  if (dir_inode.type != static_cast<uint8_t>(InodeType::Directory)) {
    throw std::runtime_error("Inode is not a directory");
  }
  std::vector<uint32_t> data_blocks;
  std::vector<uint32_t> indirect_blocks;
  collect_inode_block_lists(dir_inode, data_blocks, indirect_blocks);

  const uint32_t entries_per_block = sb_.block_size / static_cast<uint32_t>(kDirEntryBytes);
  std::vector<DirEntry> result;
  std::vector<uint8_t> blk;
  for (uint32_t b : data_blocks) {
    disk_->read_block(b, blk);
    for (uint32_t slot = 0; slot < entries_per_block; ++slot) {
      DirEntry entry{};
      if (deserialize_dir_entry(blk.data() + static_cast<size_t>(slot) * kDirEntryBytes, entry) &&
          entry.state == static_cast<uint8_t>(DirEntryState::Active)) {
        result.push_back(std::move(entry));
      }
    }
  }
  return result;
}

bool GfsFileSystem::dir_has_free_slot(const Inode& dir_inode) const {
  std::vector<uint32_t> data_blocks;
  std::vector<uint32_t> indirect_blocks;
  collect_inode_block_lists(dir_inode, data_blocks, indirect_blocks);

  const uint32_t entries_per_block = sb_.block_size / static_cast<uint32_t>(kDirEntryBytes);
  std::vector<uint8_t> blk;
  for (uint32_t b : data_blocks) {
    disk_->read_block(b, blk);
    for (uint32_t slot = 0; slot < entries_per_block; ++slot) {
      DirEntry existing{};
      deserialize_dir_entry(blk.data() + static_cast<size_t>(slot) * kDirEntryBytes, existing);
      if (existing.state == static_cast<uint8_t>(DirEntryState::Empty) ||
          existing.state == static_cast<uint8_t>(DirEntryState::Deleted)) {
        return true;
      }
    }
  }
  return false;
}

void GfsFileSystem::add_dir_entry(Inode& dir_inode, const DirEntry& new_entry) {
  std::vector<uint32_t> data_blocks;
  std::vector<uint32_t> indirect_blocks;
  collect_inode_block_lists(dir_inode, data_blocks, indirect_blocks);

  const uint32_t entries_per_block = sb_.block_size / static_cast<uint32_t>(kDirEntryBytes);
  std::vector<uint8_t> blk;

  // Prefer Empty slots first, then Deleted slots so forensic deleted entries persist longer.
  for (uint8_t target_state : {static_cast<uint8_t>(DirEntryState::Empty),
                               static_cast<uint8_t>(DirEntryState::Deleted)}) {
    for (uint32_t b : data_blocks) {
      disk_->read_block(b, blk);
      for (uint32_t slot = 0; slot < entries_per_block; ++slot) {
        DirEntry existing{};
        uint8_t* ptr = blk.data() + static_cast<size_t>(slot) * kDirEntryBytes;
        deserialize_dir_entry(ptr, existing);
        if (existing.state == target_state) {
          serialize_dir_entry(new_entry, ptr);
          disk_->write_block(b, blk);
          dir_inode.modified_at_ms = now_ms();
          dir_inode.size_bytes += kDirEntryBytes;
          write_inode(dir_inode);
          return;
        }
      }
    }
  }

  // Need to grow directory by one data block (+ optional indirect block).
  const uint32_t new_data_count = static_cast<uint32_t>(data_blocks.size() + 1U);
  const uint32_t needed_indirect_total = indirect_blocks_needed(new_data_count);
  const uint32_t extra_indirect = (needed_indirect_total > indirect_blocks.size())
                                      ? (needed_indirect_total - static_cast<uint32_t>(indirect_blocks.size()))
                                      : 0U;
  if (sb_.free_blocks < 1U + extra_indirect) {
    throw std::runtime_error("ERR_NO_SPACE: Insufficient space to grow directory");
  }

  const uint32_t new_data_blk = allocate_one_data_block();
  std::vector<uint8_t> fresh_blk(sb_.block_size, 0U);
  serialize_dir_entry(new_entry, fresh_blk.data());
  disk_->write_block(new_data_blk, fresh_blk);

  data_blocks.push_back(new_data_blk);
  for (uint32_t i = 0; i < extra_indirect; ++i) {
    indirect_blocks.push_back(allocate_one_data_block());
  }
  populate_inode_block_pointers(dir_inode, data_blocks, indirect_blocks);
  dir_inode.modified_at_ms = now_ms();
  dir_inode.size_bytes += kDirEntryBytes;
  write_inode(dir_inode);
}

void GfsFileSystem::mark_dir_entry_deleted(Inode& dir_inode, const std::string& name) {
  std::vector<uint32_t> data_blocks;
  std::vector<uint32_t> indirect_blocks;
  collect_inode_block_lists(dir_inode, data_blocks, indirect_blocks);

  const uint32_t entries_per_block = sb_.block_size / static_cast<uint32_t>(kDirEntryBytes);
  std::vector<uint8_t> blk;
  for (uint32_t b : data_blocks) {
    disk_->read_block(b, blk);
    for (uint32_t slot = 0; slot < entries_per_block; ++slot) {
      uint8_t* ptr = blk.data() + static_cast<size_t>(slot) * kDirEntryBytes;
      DirEntry entry{};
      if (deserialize_dir_entry(ptr, entry) &&
          entry.state == static_cast<uint8_t>(DirEntryState::Active) &&
          entry.name == name) {
        entry.state = static_cast<uint8_t>(DirEntryState::Deleted);
        serialize_dir_entry(entry, ptr);
        disk_->write_block(b, blk);
        dir_inode.modified_at_ms = now_ms();
        write_inode(dir_inode);
        return;
      }
    }
  }
  throw std::runtime_error("Directory entry not found: " + name);
}

uint32_t GfsFileSystem::resolve_path_inode(
    const std::string& norm_path,
    uint32_t* out_parent_inode,
    std::string* out_name) const {
  const auto parts = split_path(norm_path);
  if (parts.empty()) {
    if (out_parent_inode) *out_parent_inode = kRootInodeId;
    if (out_name) *out_name = "/";
    return kRootInodeId;
  }

  uint32_t current = kRootInodeId;
  for (size_t i = 0; i < parts.size(); ++i) {
    const Inode dir = read_inode(current);
    if (dir.state != static_cast<uint8_t>(InodeState::Allocated) ||
        dir.type != static_cast<uint8_t>(InodeType::Directory)) {
      throw std::runtime_error("Path component is not a directory: " + parts[i]);
    }
    const auto entries = read_dir_entries(dir);
    uint32_t next_inode = 0U;
    for (const auto& entry : entries) {
      if (entry.name == parts[i]) {
        next_inode = entry.inode_id;
        break;
      }
    }
    if (next_inode == 0U) {
      throw std::runtime_error("ERR_NOT_FOUND: Path does not exist: " + norm_path);
    }
    if (i + 1 == parts.size()) {
      if (out_parent_inode) *out_parent_inode = current;
      if (out_name) *out_name = parts[i];
    }
    current = next_inode;
  }
  return current;
}

FileEntryInfo GfsFileSystem::build_file_info(const Inode& inode, const std::string& path) const {
  FileEntryInfo info{};
  info.inode_id = inode.inode_id;
  info.path = path;
  if (path == "/") {
    info.name = "/";
  } else {
    const auto slash = path.rfind('/');
    info.name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  }
  info.type = inode.type;
  info.permissions = inode.permissions;
  info.size_bytes = inode.size_bytes;
  info.created_at_ms = inode.created_at_ms;
  info.modified_at_ms = inode.modified_at_ms;
  info.content_crc32 = inode.content_crc32;
  info.generation = inode.generation;
  info.indirect_block = inode.indirect_block;
  info.double_indirect_block = inode.double_indirect_block;
  for (uint32_t b : inode.direct_blocks) {
    if (b != 0U) info.direct_blocks.push_back(b);
  }
  collect_inode_block_lists(inode, info.all_data_blocks, info.all_indirect_blocks);
  return info;
}

std::vector<FileEntryInfo> GfsFileSystem::list_directory(const std::string& dir_path) const {
  const std::string norm = normalize_path(dir_path);
  const uint32_t dir_inode_id = resolve_path_inode(norm);
  const Inode dir = read_inode(dir_inode_id);
  if (dir.type != static_cast<uint8_t>(InodeType::Directory)) {
    throw std::runtime_error("Target path is not a directory: " + norm);
  }

  const auto entries = read_dir_entries(dir);
  std::vector<FileEntryInfo> out;
  out.reserve(entries.size());
  for (const auto& entry : entries) {
    const Inode child = read_inode(entry.inode_id);
    if (child.state != static_cast<uint8_t>(InodeState::Allocated)) continue;
    const std::string child_path = (norm == "/") ? ("/" + entry.name) : (norm + "/" + entry.name);
    out.push_back(build_file_info(child, child_path));
  }
  std::sort(out.begin(), out.end(), [](const FileEntryInfo& a, const FileEntryInfo& b) {
    if (a.type != b.type) return a.type > b.type;  // Directories first
    return a.name < b.name;
  });
  return out;
}

FileEntryInfo GfsFileSystem::stat_path(const std::string& path) const {
  const std::string norm = normalize_path(path);
  const uint32_t id = resolve_path_inode(norm);
  return build_file_info(read_inode(id), norm);
}

FileEntryInfo GfsFileSystem::mkdir(const std::string& path, uint16_t permissions) {
  require_clean_for_write();
  const std::string norm = normalize_path(path);
  const auto parts = split_path(norm);
  if (parts.empty()) {
    throw std::invalid_argument("Root directory '/' already exists");
  }

  std::string parent_path = "/";
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    parent_path += (i == 0 ? "" : "/") + parts[i];
  }
  const uint32_t parent_id = resolve_path_inode(parent_path);
  Inode parent = read_inode(parent_id);
  if (parent.type != static_cast<uint8_t>(InodeType::Directory)) {
    throw std::runtime_error("Parent is not a directory: " + parent_path);
  }

  const std::string& new_name = parts.back();
  for (const auto& existing : read_dir_entries(parent)) {
    if (existing.name == new_name) {
      throw std::runtime_error("ERR_ALREADY_EXISTS: Entry already exists: " + norm);
    }
  }

  if (sb_.free_inodes == 0) throw std::runtime_error("ERR_NO_FREE_INODES: No free inodes");
  if (sb_.free_blocks < 1U) throw std::runtime_error("ERR_NO_SPACE: Insufficient space for directory block");

  const uint64_t seq = begin_journal(JournalOp::Mkdir, parent_id, 0U, 0U, "mkdir " + new_name);

  const uint32_t new_id = allocate_inode_slot();
  const uint32_t dir_blk = allocate_one_data_block();
  const std::vector<uint8_t> zero_blk(sb_.block_size, 0U);
  disk_->write_block(dir_blk, zero_blk);

  Inode prev = read_inode(new_id);
  const uint64_t ts = now_ms();
  Inode dir{};
  dir.inode_id = new_id;
  dir.type = static_cast<uint8_t>(InodeType::Directory);
  dir.state = static_cast<uint8_t>(InodeState::Allocated);
  dir.permissions = permissions;
  dir.created_at_ms = ts;
  dir.modified_at_ms = ts;
  dir.deleted_at_ms = 0U;
  dir.link_count = 2U;
  dir.parent_inode = parent_id;
  dir.direct_blocks[0] = dir_blk;
  dir.generation = prev.generation + 1U;
  write_inode(dir);

  DirEntry entry{};
  entry.inode_id = new_id;
  entry.entry_type = static_cast<uint8_t>(InodeType::Directory);
  entry.state = static_cast<uint8_t>(DirEntryState::Active);
  entry.name_len = static_cast<uint16_t>(new_name.size());
  entry.name = new_name;
  add_dir_entry(parent, entry);

  flush_inode_bitmap();
  flush_block_bitmap();
  commit_journal(seq, JournalOp::Mkdir, new_id, dir_blk, 0U, "mkdir " + new_name);
  disk_->sync();
  return build_file_info(dir, norm);
}

FileEntryInfo GfsFileSystem::write_file(
    const std::string& path,
    const std::vector<uint8_t>& content,
    bool overwrite,
    uint16_t permissions) {
  require_clean_for_write();
  const std::string norm = normalize_path(path);
  const auto parts = split_path(norm);
  if (parts.empty()) {
    throw std::invalid_argument("Cannot write file at root '/'");
  }

  std::string parent_path = "/";
  for (size_t i = 0; i + 1 < parts.size(); ++i) {
    parent_path += (i == 0 ? "" : "/") + parts[i];
  }
  const uint32_t parent_id = resolve_path_inode(parent_path);
  Inode parent = read_inode(parent_id);
  if (parent.type != static_cast<uint8_t>(InodeType::Directory)) {
    throw std::runtime_error("Parent is not a directory: " + parent_path);
  }

  const std::string& file_name = parts.back();
  uint32_t existing_inode_id = 0U;
  for (const auto& entry : read_dir_entries(parent)) {
    if (entry.name == file_name) {
      if (entry.entry_type == static_cast<uint8_t>(InodeType::Directory)) {
        throw std::runtime_error("Target is an existing directory: " + norm);
      }
      if (!overwrite) {
        throw std::runtime_error("ERR_ALREADY_EXISTS: File already exists: " + norm);
      }
      existing_inode_id = entry.inode_id;
      break;
    }
  }

  const uint32_t data_blocks_needed = content.empty()
                                          ? 0U
                                          : ceil_div(static_cast<uint32_t>(content.size()), sb_.block_size);
  const uint32_t ind_blocks_needed = indirect_blocks_needed(data_blocks_needed);
  uint32_t dir_growth_blocks = 0U;
  if (existing_inode_id == 0U && !dir_has_free_slot(parent)) {
    std::vector<uint32_t> parent_data;
    std::vector<uint32_t> parent_ind;
    collect_inode_block_lists(parent, parent_data, parent_ind);
    const uint32_t new_parent_ind = indirect_blocks_needed(static_cast<uint32_t>(parent_data.size() + 1U));
    dir_growth_blocks = 1U + (new_parent_ind > parent_ind.size()
                                  ? new_parent_ind - static_cast<uint32_t>(parent_ind.size())
                                  : 0U);
  }
  const uint32_t total_blocks_needed = data_blocks_needed + ind_blocks_needed + dir_growth_blocks;

  if (existing_inode_id == 0U && sb_.free_inodes == 0U) {
    throw std::runtime_error("ERR_NO_FREE_INODES: No free inodes remaining");
  }

  if (sb_.free_blocks < total_blocks_needed) {
    throw std::runtime_error("ERR_NO_SPACE: Insufficient disk space for file payload");
  }

  const uint64_t seq = begin_journal(
      JournalOp::CreateWrite,
      existing_inode_id,
      0U,
      static_cast<uint32_t>(content.size()),
      "write " + file_name);

  std::vector<uint32_t> new_data_blocks = allocate_data_blocks(data_blocks_needed);
  std::vector<uint32_t> new_ind_blocks = allocate_data_blocks(ind_blocks_needed);
  write_data_to_blocks(new_data_blocks, content);

  const uint64_t ts = now_ms();
  const uint32_t crc = content.empty() ? 0U : compute_crc32(content);
  Inode file_inode{};

  if (existing_inode_id != 0U) {
    file_inode = read_inode(existing_inode_id);
    std::vector<uint32_t> old_data;
    std::vector<uint32_t> old_ind;
    collect_inode_block_lists(file_inode, old_data, old_ind);
    for (uint32_t b : old_data) release_data_block(b);
    for (uint32_t b : old_ind) release_data_block(b);

    file_inode.size_bytes = content.size();
    file_inode.modified_at_ms = ts;
    file_inode.content_crc32 = crc;
    populate_inode_block_pointers(file_inode, new_data_blocks, new_ind_blocks);
    write_inode(file_inode);
  } else {
    const uint32_t new_id = allocate_inode_slot();
    const Inode prev = read_inode(new_id);
    file_inode.inode_id = new_id;
    file_inode.type = static_cast<uint8_t>(InodeType::File);
    file_inode.state = static_cast<uint8_t>(InodeState::Allocated);
    file_inode.permissions = permissions;
    file_inode.size_bytes = content.size();
    file_inode.created_at_ms = ts;
    file_inode.modified_at_ms = ts;
    file_inode.deleted_at_ms = 0U;
    file_inode.link_count = 1U;
    file_inode.parent_inode = parent_id;
    file_inode.content_crc32 = crc;
    file_inode.generation = prev.generation + 1U;
    populate_inode_block_pointers(file_inode, new_data_blocks, new_ind_blocks);
    write_inode(file_inode);

    DirEntry entry{};
    entry.inode_id = new_id;
    entry.entry_type = static_cast<uint8_t>(InodeType::File);
    entry.state = static_cast<uint8_t>(DirEntryState::Active);
    entry.name_len = static_cast<uint16_t>(file_name.size());
    entry.name = file_name;
    add_dir_entry(parent, entry);
  }

  flush_inode_bitmap();
  flush_block_bitmap();
  commit_journal(
      seq,
      JournalOp::CreateWrite,
      file_inode.inode_id,
      new_data_blocks.empty() ? 0U : new_data_blocks.front(),
      static_cast<uint32_t>(content.size()),
      "write " + file_name);
  disk_->sync();
  return build_file_info(file_inode, norm);
}

std::vector<uint8_t> GfsFileSystem::read_file(const std::string& path, FileEntryInfo* out_info) const {
  const std::string norm = normalize_path(path);
  const uint32_t inode_id = resolve_path_inode(norm);
  const Inode inode = read_inode(inode_id);
  if (inode.state != static_cast<uint8_t>(InodeState::Allocated) ||
      inode.type != static_cast<uint8_t>(InodeType::File)) {
    throw std::runtime_error("Target path is not a regular file: " + norm);
  }

  if (out_info != nullptr) {
    *out_info = build_file_info(inode, norm);
  }

  std::vector<uint32_t> data_blocks;
  std::vector<uint32_t> ind_blocks;
  collect_inode_block_lists(inode, data_blocks, ind_blocks);

  std::vector<uint8_t> out;
  out.reserve(inode.size_bytes);
  std::vector<uint8_t> blk;
  for (uint32_t b : data_blocks) {
    if (out.size() >= inode.size_bytes) break;
    disk_->read_block(b, blk);
    const size_t remaining = static_cast<size_t>(inode.size_bytes) - out.size();
    const size_t take = std::min<size_t>(sb_.block_size, remaining);
    out.insert(out.end(), blk.begin(), blk.begin() + static_cast<std::ptrdiff_t>(take));
  }

  if (out.size() != inode.size_bytes) {
    throw std::runtime_error("File size mismatch while reading blocks for " + norm);
  }
  const uint32_t crc = out.empty() ? 0U : compute_crc32(out);
  if (crc != inode.content_crc32) {
    throw std::runtime_error("File content CRC32 mismatch on " + norm);
  }
  return out;
}

FileEntryInfo GfsFileSystem::rename_path(const std::string& old_path, const std::string& new_path) {
  require_clean_for_write();
  const std::string norm_old = normalize_path(old_path);
  const std::string norm_new = normalize_path(new_path);
  if (norm_old == "/") throw std::invalid_argument("Cannot rename root directory '/'");
  if (norm_new == "/") throw std::invalid_argument("Invalid destination path '/'");
  if (norm_old == norm_new) return stat_path(norm_old);

  uint32_t old_parent_id = 0U;
  std::string old_name;
  const uint32_t target_id = resolve_path_inode(norm_old, &old_parent_id, &old_name);
  Inode target = read_inode(target_id);

  // Prevent moving a directory inside itself.
  if (target.type == static_cast<uint8_t>(InodeType::Directory) &&
      norm_new.rfind(norm_old + "/", 0) == 0) {
    throw std::invalid_argument("Cannot move a directory into its own subdirectory");
  }

  const auto new_parts = split_path(norm_new);
  std::string new_parent_path = "/";
  for (size_t i = 0; i + 1 < new_parts.size(); ++i) {
    new_parent_path += (i == 0 ? "" : "/") + new_parts[i];
  }
  const uint32_t new_parent_id = resolve_path_inode(new_parent_path);
  Inode new_parent = read_inode(new_parent_id);
  if (new_parent.type != static_cast<uint8_t>(InodeType::Directory)) {
    throw std::runtime_error("Destination parent is not a directory: " + new_parent_path);
  }

  const std::string& new_name = new_parts.back();
  for (const auto& existing : read_dir_entries(new_parent)) {
    if (existing.name == new_name) {
      throw std::runtime_error("ERR_ALREADY_EXISTS: Destination already exists: " + norm_new);
    }
  }

  const uint64_t seq = begin_journal(JournalOp::Rename, target_id, 0U, 0U, "rename " + old_name);

  Inode old_parent = read_inode(old_parent_id);
  mark_dir_entry_deleted(old_parent, old_name);
  if (old_parent_id == new_parent_id) {
    new_parent = read_inode(new_parent_id);
  }

  DirEntry moved{};
  moved.inode_id = target_id;
  moved.entry_type = target.type;
  moved.state = static_cast<uint8_t>(DirEntryState::Active);
  moved.name_len = static_cast<uint16_t>(new_name.size());
  moved.name = new_name;
  add_dir_entry(new_parent, moved);

  target.parent_inode = new_parent_id;
  target.modified_at_ms = now_ms();
  write_inode(target);

  flush_block_bitmap();
  commit_journal(seq, JournalOp::Rename, target_id, 0U, 0U, "rename " + new_name);
  disk_->sync();
  return build_file_info(target, norm_new);
}

GfsFileSystem::DeleteResult GfsFileSystem::delete_file(const std::string& path) {
  require_clean_for_write();
  const std::string norm = normalize_path(path);
  if (norm == "/") throw std::invalid_argument("Cannot delete root directory as file");

  uint32_t parent_id = 0U;
  std::string name;
  const uint32_t target_id = resolve_path_inode(norm, &parent_id, &name);
  Inode target = read_inode(target_id);
  if (target.type != static_cast<uint8_t>(InodeType::File)) {
    throw std::runtime_error("Target is a directory; use directory delete: " + norm);
  }

  const uint64_t seq = begin_journal(JournalOp::Delete, target_id, 0U, 0U, "delete " + name);

  std::vector<uint32_t> data_blocks;
  std::vector<uint32_t> ind_blocks;
  collect_inode_block_lists(target, data_blocks, ind_blocks);

  // Forensic deletion: release block & inode bits WITHOUT zeroing payload blocks or inode pointers.
  DeleteResult result{};
  result.inode_id = target_id;
  for (uint32_t b : data_blocks) {
    release_data_block(b);
    result.released_blocks.push_back(b);
  }
  for (uint32_t b : ind_blocks) {
    release_data_block(b);
    result.released_blocks.push_back(b);
  }
  result.released_blocks_count = static_cast<uint32_t>(result.released_blocks.size());

  target.state = static_cast<uint8_t>(InodeState::Deleted);
  target.deleted_at_ms = now_ms();
  target.link_count = 0U;
  write_inode(target);
  release_inode_slot(target_id);

  Inode parent = read_inode(parent_id);
  mark_dir_entry_deleted(parent, name);

  flush_inode_bitmap();
  flush_block_bitmap();
  commit_journal(seq, JournalOp::Delete, target_id, 0U, result.released_blocks_count, "delete " + name);
  disk_->sync();
  return result;
}

GfsFileSystem::DeleteResult GfsFileSystem::delete_directory(const std::string& path, bool recursive) {
  require_clean_for_write();
  const std::string norm = normalize_path(path);
  if (norm == "/") throw std::invalid_argument("Cannot delete root directory '/'");

  uint32_t parent_id = 0U;
  std::string name;
  const uint32_t dir_id = resolve_path_inode(norm, &parent_id, &name);
  Inode dir = read_inode(dir_id);
  if (dir.type != static_cast<uint8_t>(InodeType::Directory)) {
    throw std::runtime_error("Target is not a directory: " + norm);
  }

  const auto children = read_dir_entries(dir);
  if (!children.empty() && !recursive) {
    throw std::runtime_error("ERR_DIR_NOT_EMPTY: Directory is not empty: " + norm);
  }

  DeleteResult total{};
  total.inode_id = dir_id;
  for (const auto& child : children) {
    const std::string child_path = norm + "/" + child.name;
    DeleteResult sub = (child.entry_type == static_cast<uint8_t>(InodeType::Directory))
                           ? delete_directory(child_path, true)
                           : delete_file(child_path);
    total.released_blocks.insert(
        total.released_blocks.end(),
        sub.released_blocks.begin(),
        sub.released_blocks.end());
  }

  const uint64_t seq = begin_journal(JournalOp::Rmdir, dir_id, 0U, 0U, "rmdir " + name);

  dir = read_inode(dir_id);
  std::vector<uint32_t> data_blocks;
  std::vector<uint32_t> ind_blocks;
  collect_inode_block_lists(dir, data_blocks, ind_blocks);
  for (uint32_t b : data_blocks) {
    release_data_block(b);
    total.released_blocks.push_back(b);
  }
  for (uint32_t b : ind_blocks) {
    release_data_block(b);
    total.released_blocks.push_back(b);
  }
  total.released_blocks_count = static_cast<uint32_t>(total.released_blocks.size());

  dir.state = static_cast<uint8_t>(InodeState::Deleted);
  dir.deleted_at_ms = now_ms();
  dir.link_count = 0U;
  write_inode(dir);
  release_inode_slot(dir_id);

  Inode parent = read_inode(parent_id);
  mark_dir_entry_deleted(parent, name);

  flush_inode_bitmap();
  flush_block_bitmap();
  commit_journal(seq, JournalOp::Rmdir, dir_id, 0U, total.released_blocks_count, "rmdir " + name);
  disk_->sync();
  return total;
}

uint64_t GfsFileSystem::begin_journal(
    JournalOp op,
    uint32_t target_inode,
    uint32_t target_block,
    uint32_t aux,
    const std::string& summary) {
  const uint64_t seq = sb_.next_journal_seq++;
  const uint32_t slots_per_block = sb_.block_size / static_cast<uint32_t>(kJournalEntryBytes);
  const uint32_t total_slots = sb_.journal_blocks * slots_per_block;
  const uint32_t slot_idx = static_cast<uint32_t>((seq - 1U) % total_slots);
  const uint32_t blk_idx = sb_.journal_start + (slot_idx / slots_per_block);
  const uint32_t in_blk_slot = slot_idx % slots_per_block;

  JournalEntry entry{};
  entry.seq_no = seq;
  entry.timestamp_ms = now_ms();
  entry.op_type = static_cast<uint8_t>(op);
  entry.status = static_cast<uint8_t>(JournalStatus::Pending);
  entry.target_inode = target_inode;
  entry.target_block = target_block;
  entry.aux_value = aux;
  entry.summary = summary.substr(0, kMaxJournalSummaryLen);

  std::vector<uint8_t> blk;
  disk_->read_block(blk_idx, blk);
  serialize_journal_entry(entry, blk.data() + static_cast<size_t>(in_blk_slot) * kJournalEntryBytes);
  disk_->write_block(blk_idx, blk);

  sb_.fs_state = static_cast<uint8_t>(FsState::Dirty);
  sb_.last_write_ms = entry.timestamp_ms;
  flush_superblocks();
  disk_->sync();
  return seq;
}

void GfsFileSystem::commit_journal(
    uint64_t seq_no,
    JournalOp op,
    uint32_t target_inode,
    uint32_t target_block,
    uint32_t aux,
    const std::string& summary) {
  const uint32_t slots_per_block = sb_.block_size / static_cast<uint32_t>(kJournalEntryBytes);
  const uint32_t total_slots = sb_.journal_blocks * slots_per_block;
  const uint32_t slot_idx = static_cast<uint32_t>((seq_no - 1U) % total_slots);
  const uint32_t blk_idx = sb_.journal_start + (slot_idx / slots_per_block);
  const uint32_t in_blk_slot = slot_idx % slots_per_block;

  sb_.fs_state = static_cast<uint8_t>(FsState::Clean);
  sb_.last_write_ms = now_ms();
  flush_superblocks();

  JournalEntry entry{};
  entry.seq_no = seq_no;
  entry.timestamp_ms = sb_.last_write_ms;
  entry.op_type = static_cast<uint8_t>(op);
  entry.status = static_cast<uint8_t>(JournalStatus::Committed);
  entry.target_inode = target_inode;
  entry.target_block = target_block;
  entry.aux_value = aux;
  entry.summary = summary.substr(0, kMaxJournalSummaryLen);

  std::vector<uint8_t> blk;
  disk_->read_block(blk_idx, blk);
  serialize_journal_entry(entry, blk.data() + static_cast<size_t>(in_blk_slot) * kJournalEntryBytes);
  disk_->write_block(blk_idx, blk);
}

std::vector<JournalEntry> GfsFileSystem::read_journal_entries() const {
  const uint32_t slots_per_block = sb_.block_size / static_cast<uint32_t>(kJournalEntryBytes);
  std::vector<JournalEntry> entries;
  std::vector<uint8_t> blk;
  for (uint32_t b = 0; b < sb_.journal_blocks; ++b) {
    disk_->read_block(sb_.journal_start + b, blk);
    for (uint32_t s = 0; s < slots_per_block; ++s) {
      JournalEntry entry{};
      bool crc_ok = false;
      deserialize_journal_entry(blk.data() + static_cast<size_t>(s) * kJournalEntryBytes, entry, crc_ok);
      if (crc_ok && entry.seq_no > 0U && entry.op_type != 0U) {
        entries.push_back(std::move(entry));
      }
    }
  }
  std::sort(entries.begin(), entries.end(), [](const JournalEntry& a, const JournalEntry& b) {
    return a.seq_no > b.seq_no;
  });
  if (entries.size() > 16U) entries.resize(16U);
  return entries;
}

void GfsFileSystem::inject_pending_journal_for_test(const std::string& summary) {
  if (!write_mode_) throw std::runtime_error("Write mode required");
  begin_journal(JournalOp::CreateWrite, 0U, 0U, 0U, summary);
}

VolumeStatus GfsFileSystem::set_alloc_policy(AllocPolicy policy) {
  require_clean_for_write();
  const uint64_t seq = begin_journal(
      JournalOp::Format,
      0U,
      0U,
      static_cast<uint32_t>(policy),
      "policy " + alloc_policy_to_string(policy));
  sb_.alloc_policy = static_cast<uint8_t>(policy);
  commit_journal(
      seq,
      JournalOp::Format,
      0U,
      0U,
      static_cast<uint32_t>(policy),
      "policy " + alloc_policy_to_string(policy));
  disk_->sync();
  return status();
}

Inode GfsFileSystem::read_inode_slot(uint32_t inode_id, bool* out_crc_ok) const {
  if (inode_id == 0U || inode_id >= sb_.total_inodes) {
    throw std::out_of_range("Inode ID out of range");
  }
  const uint32_t inodes_per_block = sb_.block_size / static_cast<uint32_t>(kInodeBytes);
  const uint32_t block_offset = inode_id / inodes_per_block;
  const uint32_t slot_in_block = inode_id % inodes_per_block;
  std::vector<uint8_t> blk;
  disk_->read_block(sb_.inode_table_start + block_offset, blk);
  Inode out{};
  bool crc_ok = false;
  deserialize_inode(blk.data() + static_cast<size_t>(slot_in_block) * kInodeBytes, out, crc_ok);
  if (out_crc_ok) *out_crc_ok = crc_ok;
  return out;
}

void GfsFileSystem::collect_inode_blocks(
    const Inode& inode,
    std::vector<uint32_t>& out_data_blocks,
    std::vector<uint32_t>& out_indirect_blocks) const {
  collect_inode_block_lists(inode, out_data_blocks, out_indirect_blocks);
}

std::vector<FileEntryInfo> GfsFileSystem::list_all_entries_recursive() const {
  std::vector<FileEntryInfo> out;
  std::vector<std::string> stack{"/"};
  while (!stack.empty()) {
    const std::string current = stack.back();
    stack.pop_back();
    const auto children = list_directory(current);
    for (const auto& entry : children) {
      out.push_back(entry);
      if (entry.type == static_cast<uint8_t>(InodeType::Directory)) {
        stack.push_back(entry.path);
      }
    }
  }
  return out;
}

}  // namespace ghostfs
