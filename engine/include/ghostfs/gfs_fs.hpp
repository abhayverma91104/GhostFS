#pragma once

#include "ghostfs/allocator.hpp"
#include "ghostfs/gfs_format.hpp"
#include "ghostfs/virtual_disk.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ghostfs {

struct FileEntryInfo {
  uint32_t inode_id = 0;
  std::string name;
  std::string path;
  uint8_t type = 0;
  uint16_t permissions = 0;
  uint64_t size_bytes = 0;
  uint64_t created_at_ms = 0;
  uint64_t modified_at_ms = 0;
  uint32_t content_crc32 = 0;
  uint32_t generation = 0;
  std::vector<uint32_t> direct_blocks;
  uint32_t indirect_block = 0;
  uint32_t double_indirect_block = 0;
  std::vector<uint32_t> all_data_blocks;
  std::vector<uint32_t> all_indirect_blocks;
};

struct VolumeStatus {
  std::string disk_name;
  std::string volume_label;
  uint32_t block_size = 0;
  uint32_t total_blocks = 0;
  uint32_t reserved_blocks = 0;
  uint32_t data_blocks_count = 0;
  uint32_t free_blocks = 0;
  uint32_t allocated_data_blocks = 0;
  uint32_t total_inodes = 0;
  uint32_t free_inodes = 0;
  uint32_t used_inodes = 0;
  uint64_t total_bytes = 0;
  uint64_t usable_data_bytes = 0;
  uint64_t used_data_bytes = 0;
  std::string alloc_policy;
  uint8_t fs_state = 0;
  bool primary_sb_crc_ok = false;
  bool backup_sb_crc_ok = false;
  bool boot_crc_ok = false;
  bool dirty_journal_detected = false;
  uint16_t mount_count = 0;
  uint64_t last_mount_ms = 0;
  uint64_t last_write_ms = 0;
  Superblock superblock;
  std::vector<JournalEntry> recent_journal;
};

class GfsFileSystem {
 public:
  GfsFileSystem() = default;

  /// Formats an existing VirtualDisk image with the GFS1 layout.
  /// If `requested_inodes` > 0, sets exact inode limit (minimum 2).
  void format(
      VirtualDisk& disk,
      const std::string& volume_label,
      AllocPolicy policy = AllocPolicy::FirstFit,
      uint32_t requested_inodes = 0);

  /// Opens and validates an existing formatted `.gfs` disk image.
  void mount(VirtualDisk& disk, bool write_mode);

  /// Reads an already-opened VirtualDisk's GFS1 structures cleanly.
  VolumeStatus status() const;

  std::vector<FileEntryInfo> list_directory(const std::string& dir_path) const;
  FileEntryInfo stat_path(const std::string& path) const;

  FileEntryInfo mkdir(const std::string& path, uint16_t permissions = 0755);
  FileEntryInfo write_file(
      const std::string& path,
      const std::vector<uint8_t>& content,
      bool overwrite = true,
      uint16_t permissions = 0644);
  std::vector<uint8_t> read_file(const std::string& path, FileEntryInfo* out_info = nullptr) const;
  FileEntryInfo rename_path(const std::string& old_path, const std::string& new_path);

  struct DeleteResult {
    uint32_t inode_id = 0;
    uint32_t released_blocks_count = 0;
    std::vector<uint32_t> released_blocks;
  };

  DeleteResult delete_file(const std::string& path);
  DeleteResult delete_directory(const std::string& path, bool recursive = false);

  /// Test helper: writes an uncommitted Pending journal entry and Dirty superblock state.
  void inject_pending_journal_for_test(const std::string& summary);

  /// Updates the active allocation policy in both superblocks without relocating existing files.
  VolumeStatus set_alloc_policy(AllocPolicy policy);

  /// Read-only accessors for fragmentation and block-map analysis.
  const Superblock& superblock() const { return sb_; }
  const VirtualDisk& disk() const { return *disk_; }
  const std::vector<uint8_t>& block_bitmap() const { return block_bitmap_; }
  const std::vector<uint8_t>& inode_bitmap() const { return inode_bitmap_; }
  const AllocationTrace& last_alloc_trace() const { return last_alloc_trace_; }
  Inode read_inode_slot(uint32_t inode_id, bool* out_crc_ok = nullptr) const;
  std::vector<FileEntryInfo> list_all_entries_recursive() const;
  void collect_inode_blocks(
      const Inode& inode,
      std::vector<uint32_t>& out_data_blocks,
      std::vector<uint32_t>& out_indirect_blocks) const;

 private:
  VirtualDisk* disk_ = nullptr;
  bool write_mode_ = false;
  BootBlock boot_{};
  Superblock sb_{};
  bool boot_crc_ok_ = false;
  bool primary_sb_crc_ok_ = false;
  bool backup_sb_crc_ok_ = false;
  bool dirty_journal_detected_ = false;
  std::vector<uint8_t> inode_bitmap_;
  std::vector<uint8_t> block_bitmap_;
  AllocationTrace last_alloc_trace_{};

  void require_clean_for_write() const;

  static std::vector<std::string> split_path(const std::string& path);
  static std::string normalize_path(const std::string& path);

  void load_bitmaps();
  void flush_inode_bitmap();
  void flush_block_bitmap();
  void flush_superblocks();

  bool get_bitmap_bit(const std::vector<uint8_t>& bitmap, uint32_t idx) const;
  void set_bitmap_bit(std::vector<uint8_t>& bitmap, uint32_t idx, bool value);

  Inode read_inode(uint32_t inode_id) const;
  void write_inode(const Inode& inode);

  uint32_t allocate_inode_slot();
  void release_inode_slot(uint32_t inode_id);

  uint32_t allocate_one_data_block();
  std::vector<uint32_t> allocate_data_blocks(uint32_t count);
  void release_data_block(uint32_t block_idx);

  void collect_inode_block_lists(
      const Inode& inode,
      std::vector<uint32_t>& out_data_blocks,
      std::vector<uint32_t>& out_indirect_blocks) const;

  void write_data_to_blocks(const std::vector<uint32_t>& blocks, const std::vector<uint8_t>& data);
  void populate_inode_block_pointers(
      Inode& inode,
      const std::vector<uint32_t>& data_blocks,
      const std::vector<uint32_t>& indirect_blocks);

  uint32_t indirect_blocks_needed(uint32_t data_block_count) const;
  uint32_t resolve_path_inode(const std::string& norm_path, uint32_t* out_parent_inode = nullptr, std::string* out_name = nullptr) const;

  std::vector<DirEntry> read_dir_entries(const Inode& dir_inode) const;
  bool dir_has_free_slot(const Inode& dir_inode) const;
  void add_dir_entry(Inode& dir_inode, const DirEntry& new_entry);
  void mark_dir_entry_deleted(Inode& dir_inode, const std::string& name);
  FileEntryInfo build_file_info(const Inode& inode, const std::string& path) const;

  uint64_t begin_journal(JournalOp op, uint32_t target_inode, uint32_t target_block, uint32_t aux, const std::string& summary);
  void commit_journal(uint64_t seq_no, JournalOp op, uint32_t target_inode, uint32_t target_block, uint32_t aux, const std::string& summary);
  std::vector<JournalEntry> read_journal_entries() const;
};

}  // namespace ghostfs
