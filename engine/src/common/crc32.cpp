#include "ghostfs/crc32.hpp"

#include <array>

namespace ghostfs {

namespace {

constexpr std::array<uint32_t, 256> make_crc32_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      if ((crc & 1U) != 0U) {
        crc = (crc >> 1U) ^ 0xEDB88320U;
      } else {
        crc >>= 1U;
      }
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<uint32_t, 256> kCrc32Table = make_crc32_table();

}  // namespace

uint32_t compute_crc32(const uint8_t* data, size_t length) {
  if (data == nullptr || length == 0) {
    return 0U;
  }
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < length; ++i) {
    const uint8_t idx = static_cast<uint8_t>((crc ^ data[i]) & 0xFFU);
    crc = (crc >> 8U) ^ kCrc32Table[idx];
  }
  return crc ^ 0xFFFFFFFFU;
}

}  // namespace ghostfs
