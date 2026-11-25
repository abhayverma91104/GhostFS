#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ghostfs {

inline constexpr uint32_t kDefaultBlockSize = 4096U;
inline constexpr uint32_t kFormatVersion = 1U;
inline constexpr uint32_t kSuperblockMagic = 0x47465331U;  // "GFS1"
inline constexpr uint32_t kRootInodeId = 1U;
inline constexpr uint32_t kDirectBlockCount = 12U;
inline constexpr uint32_t kMinDiskBlocks = 64U;
inline constexpr uint64_t kMaxDiskSizeBytes = 512ULL * 1024ULL * 1024ULL;  // 512 MiB

inline constexpr size_t kBootBlockBytes = 64U;
inline constexpr size_t kSuperblockBytes = 128U;
inline constexpr size_t kInodeBytes = 128U;
inline constexpr size_t kDirEntryBytes = 64U;
inline constexpr size_t kJournalEntryBytes = 64U;
inline constexpr size_t kMaxNameLen = 55U;
inline constexpr size_t kMaxVolumeLabelLen = 19U;
inline constexpr size_t kMaxJournalSummaryLen = 27U;

enum class InodeType : uint8_t {
  Unused = 0,
  File = 1,
  Directory = 2,
};

enum class InodeState : uint8_t {
  Free = 0,
  Allocated = 1,
  Deleted = 2,
};

enum class DirEntryState : uint8_t {
  Empty = 0,
  Active = 1,
  Deleted = 2,
};

enum class AllocPolicy : uint8_t {
  FirstFit = 0,
  BestFit = 1,
  WorstFit = 2,
  BitmapNextFit = 3,
};

enum class FsState : uint8_t {
  Clean = 1,
  Dirty = 2,
  Corrupted = 4,
};

enum class JournalOp : uint8_t {
  Format = 1,
  Mkdir = 2,
  CreateWrite = 3,
  Delete = 4,
  Rename = 5,
  Rmdir = 6,
  Repair = 7,
};

enum class JournalStatus : uint8_t {
  Pending = 1,
  Committed = 2,
  Aborted = 3,
};

struct BootBlock {
  std::array<char, 8> signature{'G', 'H', 'O', 'S', 'T', 'F', 'S', '\0'};
  uint32_t version = kFormatVersion;
  uint32_t block_size = kDefaultBlockSize;
  uint64_t total_size_bytes = 0;
  uint64_t created_at_ms = 0;
  uint32_t primary_sb_block = 1;
  uint32_t backup_sb_block = 2;
  uint32_t crc32 = 0;
};

struct Superblock {
  uint32_t magic = kSuperblockMagic;
  uint32_t version = kFormatVersion;
  uint32_t block_size = kDefaultBlockSize;
  uint32_t total_blocks = 0;
  uint32_t reserved_blocks = 3;
  uint32_t inode_bitmap_start = 3;
  uint32_t inode_bitmap_blocks = 0;
  uint32_t block_bitmap_start = 0;
  uint32_t block_bitmap_blocks = 0;
  uint32_t inode_table_start = 0;
  uint32_t inode_table_blocks = 0;
  uint32_t data_region_start = 0;
  uint32_t data_blocks_count = 0;
  uint32_t journal_start = 0;
  uint32_t journal_blocks = 0;
  uint32_t total_inodes = 0;
  uint32_t free_inodes = 0;
  uint32_t free_blocks = 0;
  uint32_t root_inode = kRootInodeId;
  uint8_t alloc_policy = static_cast<uint8_t>(AllocPolicy::FirstFit);
  uint8_t fs_state = static_cast<uint8_t>(FsState::Clean);
  uint16_t mount_count = 0;
  uint64_t last_mount_ms = 0;
  uint64_t last_write_ms = 0;
  std::string volume_label = "GhostFS";
  uint32_t next_alloc_cursor = 0;
  uint32_t next_journal_seq = 1;
  uint32_t crc32 = 0;
};

struct Inode {
  uint32_t inode_id = 0;
  uint8_t type = static_cast<uint8_t>(InodeType::Unused);
  uint8_t state = static_cast<uint8_t>(InodeState::Free);
  uint16_t permissions = 0644;
  uint32_t uid = 0;
  uint32_t gid = 0;
  uint64_t size_bytes = 0;
  uint64_t created_at_ms = 0;
  uint64_t modified_at_ms = 0;
  uint64_t deleted_at_ms = 0;
  uint32_t link_count = 0;
  uint32_t parent_inode = 0;
  std::array<uint32_t, kDirectBlockCount> direct_blocks{};
  uint32_t indirect_block = 0;
  uint32_t double_indirect_block = 0;
  uint32_t content_crc32 = 0;
  uint32_t generation = 0;
  uint32_t flags = 0;
  uint32_t inode_crc32 = 0;
};

struct DirEntry {
  uint32_t inode_id = 0;
  uint8_t entry_type = static_cast<uint8_t>(InodeType::Unused);
  uint8_t state = static_cast<uint8_t>(DirEntryState::Empty);
  uint16_t name_len = 0;
  std::string name;
};

struct JournalEntry {
  uint64_t seq_no = 0;
  uint64_t timestamp_ms = 0;
  uint8_t op_type = 0;
  uint8_t status = 0;
  uint16_t reserved = 0;
  uint32_t target_inode = 0;
  uint32_t target_block = 0;
  uint32_t aux_value = 0;
  std::string summary;
  uint32_t crc32 = 0;
};

bool is_supported_block_size(uint32_t block_size);
std::string alloc_policy_to_string(AllocPolicy policy);
AllocPolicy parse_alloc_policy(const std::string& name);

void serialize_boot_block(const BootBlock& boot, uint8_t* out_64);
bool deserialize_boot_block(const uint8_t* in_64, BootBlock& out, bool& crc_valid);

void serialize_superblock(const Superblock& sb, uint8_t* out_128);
bool deserialize_superblock(const uint8_t* in_128, Superblock& out, bool& crc_valid);

void serialize_inode(const Inode& inode, uint8_t* out_128);
bool deserialize_inode(const uint8_t* in_128, Inode& out, bool& crc_valid);

void serialize_dir_entry(const DirEntry& entry, uint8_t* out_64);
bool deserialize_dir_entry(const uint8_t* in_64, DirEntry& out);

void serialize_journal_entry(const JournalEntry& entry, uint8_t* out_64);
bool deserialize_journal_entry(const uint8_t* in_64, JournalEntry& out, bool& crc_valid);

}  // namespace ghostfs
