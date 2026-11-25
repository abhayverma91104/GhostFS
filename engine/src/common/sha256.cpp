#include "ghostfs/sha256.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace ghostfs {

namespace {

constexpr std::array<uint32_t, 64> kRoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

inline uint32_t rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32U - n));
}

}  // namespace

Sha256Hasher::Sha256Hasher() {
  state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
            0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
}

void Sha256Hasher::transform(const uint8_t block[64]) {
  std::array<uint32_t, 64> w{};
  for (size_t i = 0; i < 16; ++i) {
    const size_t idx = i * 4;
    w[i] = (static_cast<uint32_t>(block[idx]) << 24U) |
           (static_cast<uint32_t>(block[idx + 1]) << 16U) |
           (static_cast<uint32_t>(block[idx + 2]) << 8U) |
           static_cast<uint32_t>(block[idx + 3]);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3U);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10U);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
  uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

  for (size_t i = 0; i < 64; ++i) {
    const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t temp1 = h + S1 + ch + kRoundConstants[i] + w[i];
    const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = S0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256Hasher::update(const uint8_t* data, size_t length) {
  if (data == nullptr || length == 0) return;
  total_bits_ += static_cast<uint64_t>(length) * 8ULL;

  size_t offset = 0;
  if (buffer_len_ > 0) {
    const size_t take = std::min(length, 64 - buffer_len_);
    std::memcpy(buffer_.data() + buffer_len_, data, take);
    buffer_len_ += take;
    offset += take;
    if (buffer_len_ == 64) {
      transform(buffer_.data());
      buffer_len_ = 0;
    }
  }

  while (offset + 64 <= length) {
    transform(data + offset);
    offset += 64;
  }

  if (offset < length) {
    buffer_len_ = length - offset;
    std::memcpy(buffer_.data(), data + offset, buffer_len_);
  }
}

std::string Sha256Hasher::finalize_hex() {
  buffer_[buffer_len_++] = 0x80U;
  if (buffer_len_ > 56) {
    while (buffer_len_ < 64) buffer_[buffer_len_++] = 0x00U;
    transform(buffer_.data());
    buffer_len_ = 0;
  }
  while (buffer_len_ < 56) buffer_[buffer_len_++] = 0x00U;
  for (int i = 7; i >= 0; --i) {
    buffer_[56 + (7 - i)] = static_cast<uint8_t>((total_bits_ >> (i * 8)) & 0xFFU);
  }
  transform(buffer_.data());

  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (uint32_t word : state_) {
    oss << std::setw(8) << word;
  }
  return oss.str();
}

std::string compute_sha256_hex(const uint8_t* data, size_t length) {
  Sha256Hasher hasher;
  hasher.update(data, length);
  return hasher.finalize_hex();
}

}  // namespace ghostfs
