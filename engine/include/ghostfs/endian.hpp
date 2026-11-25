#pragma once

#include <cstdint>

namespace ghostfs::endian {

inline uint16_t read_u16_le(const uint8_t* buf) {
  return static_cast<uint16_t>(buf[0]) |
         (static_cast<uint16_t>(buf[1]) << 8U);
}

inline uint32_t read_u32_le(const uint8_t* buf) {
  return static_cast<uint32_t>(buf[0]) |
         (static_cast<uint32_t>(buf[1]) << 8U) |
         (static_cast<uint32_t>(buf[2]) << 16U) |
         (static_cast<uint32_t>(buf[3]) << 24U);
}

inline uint64_t read_u64_le(const uint8_t* buf) {
  return static_cast<uint64_t>(buf[0]) |
         (static_cast<uint64_t>(buf[1]) << 8U) |
         (static_cast<uint64_t>(buf[2]) << 16U) |
         (static_cast<uint64_t>(buf[3]) << 24U) |
         (static_cast<uint64_t>(buf[4]) << 32U) |
         (static_cast<uint64_t>(buf[5]) << 40U) |
         (static_cast<uint64_t>(buf[6]) << 48U) |
         (static_cast<uint64_t>(buf[7]) << 56U);
}

inline void write_u16_le(uint8_t* buf, uint16_t val) {
  buf[0] = static_cast<uint8_t>(val & 0xFFU);
  buf[1] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
}

inline void write_u32_le(uint8_t* buf, uint32_t val) {
  buf[0] = static_cast<uint8_t>(val & 0xFFU);
  buf[1] = static_cast<uint8_t>((val >> 8U) & 0xFFU);
  buf[2] = static_cast<uint8_t>((val >> 16U) & 0xFFU);
  buf[3] = static_cast<uint8_t>((val >> 24U) & 0xFFU);
}

inline void write_u64_le(uint8_t* buf, uint64_t val) {
  buf[0] = static_cast<uint8_t>(val & 0xFFULL);
  buf[1] = static_cast<uint8_t>((val >> 8U) & 0xFFULL);
  buf[2] = static_cast<uint8_t>((val >> 16U) & 0xFFULL);
  buf[3] = static_cast<uint8_t>((val >> 24U) & 0xFFULL);
  buf[4] = static_cast<uint8_t>((val >> 32U) & 0xFFULL);
  buf[5] = static_cast<uint8_t>((val >> 40U) & 0xFFULL);
  buf[6] = static_cast<uint8_t>((val >> 48U) & 0xFFULL);
  buf[7] = static_cast<uint8_t>((val >> 56U) & 0xFFULL);
}

}  // namespace ghostfs::endian
