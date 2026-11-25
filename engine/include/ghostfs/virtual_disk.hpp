#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace ghostfs {

/// Manages a virtual `.gfs` disk image with strict bounds validation and POSIX `flock` locking.
class VirtualDisk {
 public:
  VirtualDisk() = default;
  ~VirtualDisk();

  VirtualDisk(const VirtualDisk&) = delete;
  VirtualDisk& operator=(const VirtualDisk&) = delete;
  VirtualDisk(VirtualDisk&&) = delete;
  VirtualDisk& operator=(VirtualDisk&&) = delete;

  /// Creates a zero-initialized `.gfs` file of `total_blocks * block_size` bytes.
  static void create_image(
      const std::filesystem::path& path,
      uint32_t block_size,
      uint32_t total_blocks,
      bool overwrite = false);

  /// Opens an existing `.gfs` file and acquires a POSIX `flock` on `<path>.lock`.
  /// When `non_blocking_lock` is true, throws `std::runtime_error` immediately if locked.
  void open_image(
      const std::filesystem::path& path,
      uint32_t block_size_hint,
      bool write_mode,
      bool non_blocking_lock = false);

  /// Syncs pending writes and releases file descriptors and locks.
  void close_image();

  /// Updates the block size after reading BootBlock/Superblock header geometry.
  void set_block_size(uint32_t block_size);

  /// Reads exactly `block_size_` bytes at `block_index`. Throws on out-of-bounds.
  void read_block(uint32_t block_index, std::vector<uint8_t>& out_buffer) const;

  /// Writes `data.size() == block_size_` bytes at `block_index`. Throws on out-of-bounds or read-only.
  void write_block(uint32_t block_index, const std::vector<uint8_t>& data);

  /// Bounds-checked raw byte read for header inspection prior to full mount.
  void read_raw(uint64_t byte_offset, size_t length, std::vector<uint8_t>& out_buffer) const;

  void sync() const;

  bool is_open() const { return disk_fd_ >= 0; }
  bool is_writable() const { return write_mode_; }
  uint32_t block_size() const { return block_size_; }
  uint32_t total_blocks() const { return total_blocks_; }
  uint64_t file_size_bytes() const { return file_size_bytes_; }
  const std::filesystem::path& path() const { return path_; }

 private:
  int disk_fd_ = -1;
  int lock_fd_ = -1;
  bool write_mode_ = false;
  uint32_t block_size_ = 4096U;
  uint32_t total_blocks_ = 0U;
  uint64_t file_size_bytes_ = 0ULL;
  std::filesystem::path path_;
  std::filesystem::path lock_path_;
};

}  // namespace ghostfs
