#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ghostfs {

class Sha256Hasher {
 public:
  Sha256Hasher();
  void update(const uint8_t* data, size_t length);
  std::string finalize_hex();

 private:
  void transform(const uint8_t block[64]);

  std::array<uint32_t, 8> state_{};
  std::array<uint8_t, 64> buffer_{};
  size_t buffer_len_ = 0;
  uint64_t total_bits_ = 0;
};

std::string compute_sha256_hex(const uint8_t* data, size_t length);

inline std::string compute_sha256_hex(const std::vector<uint8_t>& data) {
  return compute_sha256_hex(data.data(), data.size());
}

}  // namespace ghostfs
