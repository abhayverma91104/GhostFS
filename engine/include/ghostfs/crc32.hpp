#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ghostfs {

/// Computes standard IEEE 802.3 CRC32 (polynomial 0xEDB88320).
uint32_t compute_crc32(const uint8_t* data, size_t length);

inline uint32_t compute_crc32(const std::vector<uint8_t>& data) {
  return compute_crc32(data.data(), data.size());
}

}  // namespace ghostfs
