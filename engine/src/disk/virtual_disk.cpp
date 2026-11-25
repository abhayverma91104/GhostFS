#include "ghostfs/virtual_disk.hpp"

#include "ghostfs/gfs_format.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ghostfs {

namespace {

std::runtime_error make_errno_error(const std::string& context, int err_num) {
  return std::runtime_error(context + ": " + std::strerror(err_num));
}

void write_exact(int fd, const uint8_t* data, size_t len, off_t offset) {
  size_t written = 0;
  while (written < len) {
    const ssize_t rc = ::pwrite(fd, data + written, len - written, offset + static_cast<off_t>(written));
    if (rc < 0) {
      if (errno == EINTR) continue;
      throw make_errno_error("pwrite failed", errno);
    }
    written += static_cast<size_t>(rc);
  }
}

void read_exact(int fd, uint8_t* data, size_t len, off_t offset) {
  size_t total_read = 0;
  while (total_read < len) {
    const ssize_t rc = ::pread(fd, data + total_read, len - total_read, offset + static_cast<off_t>(total_read));
    if (rc < 0) {
      if (errno == EINTR) continue;
      throw make_errno_error("pread failed", errno);
    }
    if (rc == 0) {
      throw std::runtime_error("Unexpected EOF while reading virtual disk block");
    }
    total_read += static_cast<size_t>(rc);
  }
}

}  // namespace

VirtualDisk::~VirtualDisk() {
  close_image();
}

void VirtualDisk::create_image(
    const std::filesystem::path& path,
    uint32_t block_size,
    uint32_t total_blocks,
    bool overwrite) {
  if (!is_supported_block_size(block_size)) {
    throw std::invalid_argument("Unsupported block size; expected 512, 1024, 2048, or 4096");
  }
  if (total_blocks < kMinDiskBlocks) {
    throw std::invalid_argument("Disk image must have at least 64 blocks");
  }
  const uint64_t total_bytes = static_cast<uint64_t>(block_size) * static_cast<uint64_t>(total_blocks);
  if (total_bytes > kMaxDiskSizeBytes) {
    throw std::invalid_argument("Disk image exceeds 512 MiB workspace limit");
  }

  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }

  const std::filesystem::path lock_path = path.string() + ".lock";
  const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
  if (lock_fd < 0) {
    throw make_errno_error("Failed to open lock file", errno);
  }
  if (::flock(lock_fd, LOCK_EX) < 0) {
    const int saved = errno;
    ::close(lock_fd);
    throw make_errno_error("Failed to acquire exclusive disk lock", saved);
  }

  const int flags = O_CREAT | O_RDWR | (overwrite ? O_TRUNC : O_EXCL);
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    const int saved = errno;
    ::flock(lock_fd, LOCK_UN);
    ::close(lock_fd);
    if (saved == EEXIST) {
      throw std::runtime_error("Virtual disk image already exists: " + path.filename().string());
    }
    throw make_errno_error("Failed to create virtual disk image", saved);
  }

  try {
    if (::ftruncate(fd, static_cast<off_t>(total_bytes)) < 0) {
      throw make_errno_error("ftruncate failed while sizing disk image", errno);
    }
    // Zero-initialize in chunks so allocated blocks start deterministic and clean.
    const std::vector<uint8_t> zero_chunk(65536U, 0U);
    uint64_t offset = 0;
    while (offset < total_bytes) {
      const size_t chunk_len = static_cast<size_t>(
          std::min<uint64_t>(zero_chunk.size(), total_bytes - offset));
      write_exact(fd, zero_chunk.data(), chunk_len, static_cast<off_t>(offset));
      offset += chunk_len;
    }
    ::fsync(fd);
  } catch (...) {
    ::close(fd);
    ::flock(lock_fd, LOCK_UN);
    ::close(lock_fd);
    throw;
  }

  ::close(fd);
  ::flock(lock_fd, LOCK_UN);
  ::close(lock_fd);
}

void VirtualDisk::open_image(
    const std::filesystem::path& path,
    uint32_t block_size_hint,
    bool write_mode,
    bool non_blocking_lock) {
  close_image();

  if (!std::filesystem::exists(path)) {
    throw std::runtime_error("Virtual disk image does not exist: " + path.filename().string());
  }
  if (!std::filesystem::is_regular_file(path)) {
    throw std::runtime_error("Target path is not a regular virtual disk file");
  }
  if (!is_supported_block_size(block_size_hint)) {
    throw std::invalid_argument("Unsupported block size hint");
  }

  lock_path_ = path.string() + ".lock";
  lock_fd_ = ::open(lock_path_.c_str(), O_CREAT | O_RDWR, 0644);
  if (lock_fd_ < 0) {
    throw make_errno_error("Failed to open disk lock file", errno);
  }

  int lock_op = write_mode ? LOCK_EX : LOCK_SH;
  if (non_blocking_lock) {
    lock_op |= LOCK_NB;
  }
  if (::flock(lock_fd_, lock_op) < 0) {
    const int saved = errno;
    ::close(lock_fd_);
    lock_fd_ = -1;
    if (saved == EWOULDBLOCK) {
      throw std::runtime_error("Virtual disk image is locked by another conflicting operation");
    }
    throw make_errno_error("Failed to acquire disk lock", saved);
  }

  const int open_flags = write_mode ? O_RDWR : O_RDONLY;
  disk_fd_ = ::open(path.c_str(), open_flags);
  if (disk_fd_ < 0) {
    const int saved = errno;
    ::flock(lock_fd_, LOCK_UN);
    ::close(lock_fd_);
    lock_fd_ = -1;
    throw make_errno_error("Failed to open virtual disk image", saved);
  }

  struct stat st{};
  if (::fstat(disk_fd_, &st) < 0) {
    const int saved = errno;
    close_image();
    throw make_errno_error("Failed to stat virtual disk image", saved);
  }
  if (st.st_size <= 0 || static_cast<uint64_t>(st.st_size) > kMaxDiskSizeBytes) {
    close_image();
    throw std::runtime_error("Virtual disk image size is invalid or exceeds 512 MiB limit");
  }

  path_ = path;
  write_mode_ = write_mode;
  file_size_bytes_ = static_cast<uint64_t>(st.st_size);
  set_block_size(block_size_hint);
}

void VirtualDisk::close_image() {
  if (disk_fd_ >= 0) {
    if (write_mode_) {
      ::fsync(disk_fd_);
    }
    ::close(disk_fd_);
    disk_fd_ = -1;
  }
  if (lock_fd_ >= 0) {
    ::flock(lock_fd_, LOCK_UN);
    ::close(lock_fd_);
    lock_fd_ = -1;
  }
  write_mode_ = false;
  total_blocks_ = 0;
  file_size_bytes_ = 0;
  path_.clear();
  lock_path_.clear();
}

void VirtualDisk::set_block_size(uint32_t block_size) {
  if (!is_supported_block_size(block_size)) {
    throw std::invalid_argument("Unsupported block size");
  }
  if (file_size_bytes_ % block_size != 0) {
    throw std::runtime_error("Disk image size is not an exact multiple of block size");
  }
  const uint64_t blocks = file_size_bytes_ / block_size;
  if (blocks < kMinDiskBlocks || blocks > 0xFFFFFFFFULL) {
    throw std::runtime_error("Disk image block count is out of valid GFS1 range");
  }
  block_size_ = block_size;
  total_blocks_ = static_cast<uint32_t>(blocks);
}

void VirtualDisk::read_block(uint32_t block_index, std::vector<uint8_t>& out_buffer) const {
  if (!is_open()) {
    throw std::runtime_error("Virtual disk is not open");
  }
  if (block_index >= total_blocks_) {
    throw std::out_of_range("Block index " + std::to_string(block_index) +
                            " out of bounds (total_blocks=" + std::to_string(total_blocks_) + ")");
  }
  out_buffer.resize(block_size_);
  const off_t offset = static_cast<off_t>(static_cast<uint64_t>(block_index) * block_size_);
  read_exact(disk_fd_, out_buffer.data(), block_size_, offset);
}

void VirtualDisk::write_block(uint32_t block_index, const std::vector<uint8_t>& data) {
  if (!is_open()) {
    throw std::runtime_error("Virtual disk is not open");
  }
  if (!write_mode_) {
    throw std::runtime_error("Virtual disk is opened read-only");
  }
  if (block_index >= total_blocks_) {
    throw std::out_of_range("Block index " + std::to_string(block_index) +
                            " out of bounds (total_blocks=" + std::to_string(total_blocks_) + ")");
  }
  if (data.size() != block_size_) {
    throw std::invalid_argument("Write buffer size does not match disk block size");
  }
  const off_t offset = static_cast<off_t>(static_cast<uint64_t>(block_index) * block_size_);
  write_exact(disk_fd_, data.data(), block_size_, offset);
}

void VirtualDisk::read_raw(uint64_t byte_offset, size_t length, std::vector<uint8_t>& out_buffer) const {
  if (!is_open()) {
    throw std::runtime_error("Virtual disk is not open");
  }
  if (byte_offset > file_size_bytes_ || length > (file_size_bytes_ - byte_offset)) {
    throw std::out_of_range("Raw disk read exceeds virtual disk bounds");
  }
  out_buffer.resize(length);
  if (length > 0) {
    read_exact(disk_fd_, out_buffer.data(), length, static_cast<off_t>(byte_offset));
  }
}

void VirtualDisk::sync() const {
  if (disk_fd_ >= 0 && write_mode_) {
    ::fsync(disk_fd_);
  }
}

}  // namespace ghostfs
