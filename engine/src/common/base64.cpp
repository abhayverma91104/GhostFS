#include "ghostfs/base64.hpp"

#include <array>
#include <stdexcept>

namespace ghostfs {

namespace {

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::array<int, 256> make_decode_table() {
  std::array<int, 256> table{};
  table.fill(-1);
  for (int i = 0; i < 64; ++i) {
    table[static_cast<uint8_t>(kAlphabet[i])] = i;
  }
  return table;
}

const std::array<int, 256> kDecodeTable = make_decode_table();

}  // namespace

std::string base64_encode(const std::vector<uint8_t>& data) {
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);
  for (size_t i = 0; i < data.size(); i += 3) {
    const uint32_t b0 = data[i];
    const uint32_t b1 = (i + 1 < data.size()) ? data[i + 1] : 0U;
    const uint32_t b2 = (i + 2 < data.size()) ? data[i + 2] : 0U;
    const uint32_t triple = (b0 << 16U) | (b1 << 8U) | b2;

    out.push_back(kAlphabet[(triple >> 18U) & 0x3FU]);
    out.push_back(kAlphabet[(triple >> 12U) & 0x3FU]);
    out.push_back((i + 1 < data.size()) ? kAlphabet[(triple >> 6U) & 0x3FU] : '=');
    out.push_back((i + 2 < data.size()) ? kAlphabet[triple & 0x3FU] : '=');
  }
  return out;
}

std::vector<uint8_t> base64_decode(const std::string& encoded) {
  std::string clean;
  clean.reserve(encoded.size());
  for (char c : encoded) {
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    clean.push_back(c);
  }
  if (clean.size() % 4 != 0) {
    throw std::invalid_argument("Invalid base64 length");
  }
  std::vector<uint8_t> out;
  out.reserve((clean.size() / 4) * 3);

  for (size_t i = 0; i < clean.size(); i += 4) {
    const char c0 = clean[i];
    const char c1 = clean[i + 1];
    const char c2 = clean[i + 2];
    const char c3 = clean[i + 3];

    const int v0 = kDecodeTable[static_cast<uint8_t>(c0)];
    const int v1 = kDecodeTable[static_cast<uint8_t>(c1)];
    if (v0 < 0 || v1 < 0) {
      throw std::invalid_argument("Invalid base64 character");
    }
    const int v2 = (c2 == '=') ? 0 : kDecodeTable[static_cast<uint8_t>(c2)];
    const int v3 = (c3 == '=') ? 0 : kDecodeTable[static_cast<uint8_t>(c3)];
    if ((c2 != '=' && v2 < 0) || (c3 != '=' && v3 < 0)) {
      throw std::invalid_argument("Invalid base64 character");
    }
    if (c2 == '=' && c3 != '=') {
      throw std::invalid_argument("Invalid base64 padding");
    }

    const uint32_t triple = (static_cast<uint32_t>(v0) << 18U) |
                            (static_cast<uint32_t>(v1) << 12U) |
                            (static_cast<uint32_t>(v2) << 6U) |
                            static_cast<uint32_t>(v3);

    out.push_back(static_cast<uint8_t>((triple >> 16U) & 0xFFU));
    if (c2 != '=') {
      out.push_back(static_cast<uint8_t>((triple >> 8U) & 0xFFU));
    }
    if (c3 != '=') {
      out.push_back(static_cast<uint8_t>(triple & 0xFFU));
    }
  }
  return out;
}

}  // namespace ghostfs
