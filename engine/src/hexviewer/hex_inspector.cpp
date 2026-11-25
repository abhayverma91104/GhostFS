#include "ghostfs/hex_inspector.hpp"

#include "ghostfs/crc32.hpp"
#include "ghostfs/fragmentation.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/sha256.hpp"
#include "ghostfs/virtual_disk.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ghostfs {

namespace {

std::string byte_to_hex(uint8_t b) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out(2, '0');
  out[0] = kHex[(b >> 4U) & 0x0FU];
  out[1] = kHex[b & 0x0FU];
  return out;
}

char to_printable_ascii(uint8_t b) {
  return (b >= 0x20U && b <= 0x7EU) ? static_cast<char>(b) : '.';
}

bool is_valid_utf8_sequence(const std::vector<uint8_t>& bytes) {
  size_t i = 0;
  while (i < bytes.size()) {
    const uint8_t b = bytes[i];
    size_t seq_len = 0;
    if (b <= 0x7FU) {
      seq_len = 1;
    } else if ((b & 0xE0U) == 0xC0U) {
      if (b < 0xC2U) return false;
      seq_len = 2;
    } else if ((b & 0xF0U) == 0xE0U) {
      seq_len = 3;
    } else if ((b & 0xF8U) == 0xF0U) {
      if (b > 0xF4U) return false;
      seq_len = 4;
    } else {
      return false;
    }
    if (i + seq_len > bytes.size()) return false;
    for (size_t k = 1; k < seq_len; ++k) {
      if ((bytes[i + k] & 0xC0U) != 0x80U) return false;
    }
    i += seq_len;
  }
  return true;
}

std::string classify_float_f32(float v) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v > 0 ? "+Infinity" : "-Infinity";
  return "finite";
}

std::string classify_float_f64(double v) {
  if (std::isnan(v)) return "NaN";
  if (std::isinf(v)) return v > 0 ? "+Infinity" : "-Infinity";
  return "finite";
}

std::vector<HexRow> build_rows(
    const std::vector<uint8_t>& bytes,
    uint64_t start_offset,
    uint32_t block_size,
    uint32_t bytes_per_row,
    uint64_t logical_base = 0) {
  if (bytes_per_row != 8U && bytes_per_row != 16U && bytes_per_row != 32U) {
    throw std::invalid_argument("ERR_BAD_PARAMS: bytes_per_row must be 8, 16, or 32");
  }
  std::vector<HexRow> rows;
  for (size_t i = 0; i < bytes.size(); i += bytes_per_row) {
    const size_t take = std::min<size_t>(bytes_per_row, bytes.size() - i);
    HexRow row{};
    row.abs_offset = start_offset + i;
    row.block_id = block_size > 0U ? static_cast<uint32_t>(row.abs_offset / block_size) : 0U;
    row.block_offset = block_size > 0U ? static_cast<uint32_t>(row.abs_offset % block_size) : 0U;
    row.logical_offset = logical_base + i;
    row.raw_bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(i),
                         bytes.begin() + static_cast<std::ptrdiff_t>(i + take));
    row.hex_bytes.reserve(take);
    row.ascii.reserve(take);
    for (uint8_t b : row.raw_bytes) {
      row.hex_bytes.push_back(byte_to_hex(b));
      row.ascii.push_back(to_printable_ascii(b));
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

SignatureCheckResult detect_signature_in_buffer(const std::vector<uint8_t>& buf, uint64_t abs_offset) {
  SignatureCheckResult res{};
  res.offset = abs_offset;
  if (buf.size() >= 8 && std::memcmp(buf.data(), "\x89PNG\r\n\x1a\n", 8) == 0) {
    res.detected = true;
    res.file_type = "png";
    res.signature_hex = "89 50 4E 47 0D 0A 1A 0A";
    res.length = 8;
    res.structurally_valid = (buf.size() >= 24 && std::memcmp(buf.data() + 12, "IHDR", 4) == 0);
    res.validation_note = res.structurally_valid
                              ? "PNG magic + IHDR header present"
                              : "PNG magic present; full chunk/IEND validation requires complete extent";
    res.warnings.push_back("Magic signature alone does not prove complete uncorrupted file contents.");
  } else if (buf.size() >= 3 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF) {
    res.detected = true;
    res.file_type = "jpeg";
    res.signature_hex = "FF D8 FF";
    res.length = 3;
    res.structurally_valid = (buf.size() >= 4 && buf[3] >= 0xC0);
    res.validation_note = res.structurally_valid
                              ? "JPEG SOI + valid marker prefix"
                              : "JPEG SOI detected; trailing marker unverified";
    res.warnings.push_back("Magic signature alone does not prove complete uncorrupted file contents.");
  } else if (buf.size() >= 5 && std::memcmp(buf.data(), "%PDF-", 5) == 0) {
    res.detected = true;
    res.file_type = "pdf";
    res.signature_hex = "25 50 44 46 2D";
    res.length = 5;
    res.structurally_valid = true;
    res.validation_note = "PDF header signature detected";
    res.warnings.push_back("Magic signature alone does not prove complete %%EOF document.");
  } else if (buf.size() >= 4 && std::memcmp(buf.data(), "1SFG", 4) == 0) {
    res.detected = true;
    res.file_type = "gfs1_superblock";
    res.signature_hex = "31 53 46 47";
    res.length = 4;
    res.structurally_valid = true;
    res.validation_note = "GFS1 Superblock little-endian magic";
  } else if (buf.size() >= 8 && std::memcmp(buf.data(), "GHOSTFS\0", 8) == 0) {
    res.detected = true;
    res.file_type = "gfs1_boot";
    res.signature_hex = "47 48 4F 53 54 46 53 00";
    res.length = 8;
    res.structurally_valid = true;
    res.validation_note = "GFS1 BootBlock ASCII signature";
  }
  return res;
}

std::vector<uint8_t> parse_hex_pattern(const std::string& hex_str) {
  std::string clean;
  for (char c : hex_str) {
    if (std::isspace(static_cast<unsigned char>(c))) continue;
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      throw std::invalid_argument("ERR_BAD_PARAMS: Invalid hexadecimal character in search pattern");
    }
    clean.push_back(c);
  }
  if (clean.empty() || (clean.size() % 2 != 0)) {
    throw std::invalid_argument("ERR_BAD_PARAMS: Hexadecimal pattern must have an even number of hex digits");
  }
  std::vector<uint8_t> out;
  out.reserve(clean.size() / 2);
  for (size_t i = 0; i < clean.size(); i += 2) {
    const uint8_t byte = static_cast<uint8_t>(std::stoul(clean.substr(i, 2), nullptr, 16));
    out.push_back(byte);
  }
  return out;
}

uint32_t probe_block_size(const VirtualDisk& disk) {
  std::vector<uint8_t> raw_boot;
  disk.read_raw(0ULL, kBootBlockBytes, raw_boot);
  BootBlock bb{};
  bool crc_ok = false;
  if (deserialize_boot_block(raw_boot.data(), bb, crc_ok) && is_supported_block_size(bb.block_size)) {
    return bb.block_size;
  }
  return kDefaultBlockSize;
}

}  // namespace

HexReadResult HexInspector::read_disk_range(
    const std::filesystem::path& disk_path,
    uint64_t offset,
    uint32_t length,
    uint32_t bytes_per_row) {
  if (length == 0U || length > kMaxHexReadBytes) {
    throw std::invalid_argument("ERR_BAD_PARAMS: Read length must be between 1 and 65536 bytes");
  }

  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  const uint32_t bs = probe_block_size(disk);
  disk.set_block_size(bs);

  if (offset > disk.file_size_bytes() || length > (disk.file_size_bytes() - offset)) {
    throw std::out_of_range("ERR_OUT_OF_BOUNDS: Byte range exceeds virtual disk bounds");
  }

  std::vector<uint8_t> buf;
  disk.read_raw(offset, length, buf);

  HexReadResult res{};
  res.disk_name = disk_path.filename().string();
  res.source_type = "disk_range";
  res.start_offset = offset;
  res.length = length;
  res.block_size = bs;
  res.bytes_per_row = bytes_per_row;
  res.total_source_bytes = disk.file_size_bytes();
  res.start_block = static_cast<uint32_t>(offset / bs);
  res.classification = "raw";
  res.signature = detect_signature_in_buffer(buf, offset);
  res.rows = build_rows(buf, offset, bs, bytes_per_row);

  // Best-effort block classification from GFS1 metadata if mountable.
  try {
    GfsFileSystem fs;
    fs.mount(disk, false);
    const auto binfo = FragmentationAnalyzer::inspect_block(fs, res.start_block);
    res.classification = binfo.classification;
    res.inode_id = binfo.inode_id;
    res.owner_path = binfo.owner_path;
  } catch (...) {
    // Allow raw hex inspection even when superblock/metadata is damaged.
  }

  return res;
}

HexReadResult HexInspector::read_block_range(
    const std::filesystem::path& disk_path,
    uint32_t block_id,
    uint32_t block_count,
    uint32_t bytes_per_row) {
  if (block_count == 0U) {
    throw std::invalid_argument("ERR_BAD_PARAMS: block_count must be >= 1");
  }
  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  const uint32_t bs = probe_block_size(disk);
  disk.set_block_size(bs);

  const uint64_t offset = static_cast<uint64_t>(block_id) * bs;
  const uint64_t req_len = static_cast<uint64_t>(block_count) * bs;
  if (req_len > kMaxHexReadBytes) {
    throw std::invalid_argument("ERR_BAD_PARAMS: Requested block range exceeds 64 KiB window");
  }

  auto res = read_disk_range(disk_path, offset, static_cast<uint32_t>(req_len), bytes_per_row);
  res.source_type = "block";
  return res;
}

HexReadResult HexInspector::read_logical_file(
    const std::filesystem::path& disk_path,
    const std::string& fs_path,
    uint64_t offset,
    uint32_t length,
    uint32_t bytes_per_row) {
  if (length == 0U || length > kMaxHexReadBytes) {
    throw std::invalid_argument("ERR_BAD_PARAMS: Read length must be between 1 and 65536 bytes");
  }

  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  GfsFileSystem fs;
  fs.mount(disk, false);

  FileEntryInfo info{};
  const std::vector<uint8_t> content = fs.read_file(fs_path, &info);
  if (offset > content.size() || length > (content.size() - offset)) {
    if (content.empty() && offset == 0) {
      HexReadResult empty_res{};
      empty_res.disk_name = disk_path.filename().string();
      empty_res.source_type = "file";
      empty_res.block_size = fs.superblock().block_size;
      empty_res.bytes_per_row = bytes_per_row;
      empty_res.total_source_bytes = 0;
      empty_res.classification = "file_data";
      empty_res.inode_id = info.inode_id;
      empty_res.owner_path = info.path;
      return empty_res;
    }
    throw std::out_of_range("ERR_OUT_OF_BOUNDS: Logical file offset/length out of bounds");
  }

  std::vector<uint8_t> slice(
      content.begin() + static_cast<std::ptrdiff_t>(offset),
      content.begin() + static_cast<std::ptrdiff_t>(offset + length));

  const uint32_t bs = fs.superblock().block_size;
  const uint32_t first_logical_blk = static_cast<uint32_t>(offset / bs);
  const uint32_t phys_blk =
      first_logical_blk < info.all_data_blocks.size() ? info.all_data_blocks[first_logical_blk] : 0U;

  HexReadResult res{};
  res.disk_name = disk_path.filename().string();
  res.source_type = "file";
  res.start_offset = offset;
  res.length = slice.size();
  res.block_size = bs;
  res.bytes_per_row = bytes_per_row;
  res.total_source_bytes = content.size();
  res.start_block = phys_blk;
  res.classification = "file_data";
  res.inode_id = info.inode_id;
  res.owner_path = info.path;
  res.signature = detect_signature_in_buffer(slice, offset);

  // Build rows with accurate physical block & disk offset per row!
  if (bytes_per_row != 8U && bytes_per_row != 16U && bytes_per_row != 32U) {
    throw std::invalid_argument("ERR_BAD_PARAMS: bytes_per_row must be 8, 16, or 32");
  }
  for (size_t i = 0; i < slice.size(); i += bytes_per_row) {
    const size_t take = std::min<size_t>(bytes_per_row, slice.size() - i);
    const uint64_t log_off = offset + i;
    const uint32_t l_blk = static_cast<uint32_t>(log_off / bs);
    const uint32_t b_off = static_cast<uint32_t>(log_off % bs);
    const uint32_t p_blk = (l_blk < info.all_data_blocks.size()) ? info.all_data_blocks[l_blk] : 0U;

    HexRow row{};
    row.logical_offset = log_off;
    row.block_id = p_blk;
    row.block_offset = b_off;
    row.abs_offset = static_cast<uint64_t>(p_blk) * bs + b_off;
    row.raw_bytes.assign(slice.begin() + static_cast<std::ptrdiff_t>(i),
                         slice.begin() + static_cast<std::ptrdiff_t>(i + take));
    for (uint8_t b : row.raw_bytes) {
      row.hex_bytes.push_back(byte_to_hex(b));
      row.ascii.push_back(to_printable_ascii(b));
    }
    res.rows.push_back(std::move(row));
  }
  return res;
}

HexReadResult HexInspector::read_artifact_range(
    const std::filesystem::path& artifact_path,
    uint64_t offset,
    uint32_t length,
    uint32_t bytes_per_row) {
  if (length == 0U || length > kMaxHexReadBytes) {
    throw std::invalid_argument("ERR_BAD_PARAMS: Read length must be between 1 and 65536 bytes");
  }
  if (!std::filesystem::exists(artifact_path)) {
    throw std::runtime_error("ERR_NOT_FOUND: Artifact does not exist");
  }
  const uint64_t file_size = std::filesystem::file_size(artifact_path);
  if (offset > file_size || length > (file_size - offset)) {
    throw std::out_of_range("ERR_OUT_OF_BOUNDS: Artifact range out of bounds");
  }
  std::ifstream ifs(artifact_path, std::ios::binary);
  ifs.seekg(static_cast<std::streamoff>(offset));
  std::vector<uint8_t> buf(length);
  ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(length));

  HexReadResult res{};
  res.disk_name = artifact_path.filename().string();
  res.source_type = "artifact";
  res.start_offset = offset;
  res.length = length;
  res.block_size = 0;
  res.bytes_per_row = bytes_per_row;
  res.total_source_bytes = file_size;
  res.classification = "recovered_artifact";
  res.signature = detect_signature_in_buffer(buf, offset);
  res.rows = build_rows(buf, offset, 0U, bytes_per_row, offset);
  return res;
}

ByteInterpretation HexInspector::interpret_bytes(const std::vector<uint8_t>& bytes) {
  if (bytes.empty()) {
    throw std::invalid_argument("ERR_BAD_PARAMS: At least 1 byte is required for interpretation");
  }
  ByteInterpretation out{};
  out.byte_count = bytes.size();
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (i > 0) out.hex += ' ';
    out.hex += byte_to_hex(bytes[i]);
    out.ascii.push_back(to_printable_ascii(bytes[i]));
  }
  for (int bit = 7; bit >= 0; --bit) {
    out.binary.push_back(((bytes[0] >> bit) & 1U) ? '1' : '0');
  }
  out.u8 = bytes[0];
  std::memcpy(&out.i8, &bytes[0], 1);

  if (bytes.size() >= 2) {
    out.has_16 = true;
    out.u16_le = static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8U);
    out.u16_be = (static_cast<uint16_t>(bytes[0]) << 8U) | static_cast<uint16_t>(bytes[1]);
    std::memcpy(&out.i16_le, &out.u16_le, 2);
    std::memcpy(&out.i16_be, &out.u16_be, 2);
  }
  if (bytes.size() >= 4) {
    out.has_32 = true;
    out.u32_le = static_cast<uint32_t>(bytes[0]) |
                 (static_cast<uint32_t>(bytes[1]) << 8U) |
                 (static_cast<uint32_t>(bytes[2]) << 16U) |
                 (static_cast<uint32_t>(bytes[3]) << 24U);
    out.u32_be = (static_cast<uint32_t>(bytes[0]) << 24U) |
                 (static_cast<uint32_t>(bytes[1]) << 16U) |
                 (static_cast<uint32_t>(bytes[2]) << 8U) |
                 static_cast<uint32_t>(bytes[3]);
    std::memcpy(&out.i32_le, &out.u32_le, 4);
    std::memcpy(&out.i32_be, &out.u32_be, 4);
    std::memcpy(&out.f32_le, &out.u32_le, 4);
    std::memcpy(&out.f32_be, &out.u32_be, 4);
    out.f32_le_status = classify_float_f32(out.f32_le);
    out.f32_be_status = classify_float_f32(out.f32_be);
  }
  if (bytes.size() >= 8) {
    out.has_64 = true;
    out.u64_le = 0ULL;
    out.u64_be = 0ULL;
    for (int i = 0; i < 8; ++i) {
      out.u64_le |= static_cast<uint64_t>(bytes[i]) << (i * 8);
      out.u64_be |= static_cast<uint64_t>(bytes[i]) << ((7 - i) * 8);
    }
    std::memcpy(&out.i64_le, &out.u64_le, 8);
    std::memcpy(&out.i64_be, &out.u64_be, 8);
    std::memcpy(&out.f64_le, &out.u64_le, 8);
    std::memcpy(&out.f64_be, &out.u64_be, 8);
    out.f64_le_status = classify_float_f64(out.f64_le);
    out.f64_be_status = classify_float_f64(out.f64_be);
  }

  out.valid_utf8 = is_valid_utf8_sequence(bytes);
  if (out.valid_utf8) {
    out.utf8.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return out;
}

DecodedStructure HexInspector::decode_block_structure(
    const std::filesystem::path& disk_path,
    uint32_t block_id,
    const std::string& force_type) {
  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  const uint32_t bs = probe_block_size(disk);
  disk.set_block_size(bs);

  std::vector<uint8_t> blk;
  disk.read_block(block_id, blk);
  const uint64_t base_offset = static_cast<uint64_t>(block_id) * bs;

  std::string kind = force_type;
  Superblock sb{};
  bool mounted = false;
  try {
    GfsFileSystem fs;
    fs.mount(disk, false);
    sb = fs.superblock();
    mounted = true;
    if (kind.empty()) {
      kind = FragmentationAnalyzer::inspect_block(fs, block_id).classification;
    }
  } catch (...) {
    if (kind.empty()) {
      if (block_id == 0U) kind = "boot_block";
      else if (block_id == 1U) kind = "primary_superblock";
      else if (block_id == 2U) kind = "backup_superblock";
      else kind = "raw";
    }
  }

  if (kind == "reserved" || kind == "boot_block") {
    return BinaryStructureParser::decode_boot_block(blk, base_offset);
  }
  if (kind == "primary_superblock") {
    return BinaryStructureParser::decode_superblock(blk, block_id, base_offset, false);
  }
  if (kind == "backup_superblock") {
    return BinaryStructureParser::decode_superblock(blk, block_id, base_offset, true);
  }
  if (kind == "inode_bitmap" && mounted) {
    return BinaryStructureParser::decode_bitmap_block(blk, block_id, base_offset, "inode_bitmap", sb.total_inodes);
  }
  if (kind == "block_bitmap" && mounted) {
    return BinaryStructureParser::decode_bitmap_block(blk, block_id, base_offset, "block_bitmap", sb.total_blocks);
  }
  if (kind == "inode_table" && mounted) {
    const uint32_t inodes_per_blk = bs / static_cast<uint32_t>(kInodeBytes);
    const uint32_t first_id = (block_id - sb.inode_table_start) * inodes_per_blk;
    return BinaryStructureParser::decode_inode_table_block(blk, block_id, base_offset, first_id);
  }
  if (kind == "directory_data") {
    return BinaryStructureParser::decode_dir_block(blk, block_id, base_offset);
  }
  if (kind == "indirect_pointer" && mounted) {
    return BinaryStructureParser::decode_indirect_block(
        blk, block_id, base_offset, sb.data_region_start, sb.data_region_start + sb.data_blocks_count);
  }
  if (kind == "journal") {
    return BinaryStructureParser::decode_journal_block(blk, block_id, base_offset);
  }

  DecodedStructure raw{};
  raw.structure_type = kind;
  raw.block_id = block_id;
  raw.base_offset = base_offset;
  raw.warnings.push_back("No fixed GFS1 metadata structure decoder applies to '" + kind + "' blocks.");
  return raw;
}

SignatureCheckResult HexInspector::inspect_signature_at(
    const std::filesystem::path& disk_path,
    uint64_t offset) {
  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  if (offset >= disk.file_size_bytes()) {
    throw std::out_of_range("ERR_OUT_OF_BOUNDS: Offset exceeds disk size");
  }
  const size_t read_len = static_cast<size_t>(std::min<uint64_t>(64ULL, disk.file_size_bytes() - offset));
  std::vector<uint8_t> buf;
  disk.read_raw(offset, read_len, buf);
  return detect_signature_in_buffer(buf, offset);
}

SearchResult HexInspector::search(
    const std::filesystem::path& disk_path,
    const std::string& query_type,
    const std::string& query_value,
    const std::string& scope,
    uint32_t start_block,
    uint32_t end_block,
    const std::string& file_path,
    uint32_t offset,
    uint32_t limit) {
  std::vector<uint8_t> pattern;
  if (query_type == "hex") {
    pattern = parse_hex_pattern(query_value);
  } else if (query_type == "ascii" || query_type == "utf8") {
    if (query_value.empty()) {
      throw std::invalid_argument("ERR_BAD_PARAMS: Search text cannot be empty");
    }
    pattern.assign(query_value.begin(), query_value.end());
  } else if (query_type == "signature") {
    if (query_value == "png") {
      pattern = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    } else if (query_value == "jpeg") {
      pattern = {0xFF, 0xD8, 0xFF};
    } else if (query_value == "pdf") {
      pattern = {'%', 'P', 'D', 'F', '-'};
    } else {
      throw std::invalid_argument("ERR_BAD_PARAMS: Signature search supports 'png', 'jpeg', or 'pdf'");
    }
  } else {
    throw std::invalid_argument("ERR_BAD_PARAMS: query_type must be 'hex', 'ascii', 'utf8', or 'signature'");
  }

  if (pattern.size() > kMaxSearchPatternBytes) {
    throw std::invalid_argument("ERR_BAD_PARAMS: Search pattern exceeds 4096 bytes");
  }

  SearchResult res{};
  res.scope = scope;
  res.query_type = query_type;
  res.source_disk_sha256 = compute_file_sha256_streaming(disk_path);
  res.offset = offset;
  res.limit = limit;

  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  const uint32_t bs = probe_block_size(disk);
  disk.set_block_size(bs);

  std::vector<SearchMatch> all_matches;
  auto push_match = [&](uint64_t abs_off, uint64_t log_off, const uint8_t* ctx_ptr, size_t ctx_len) {
    if (all_matches.size() >= kMaxSearchMatches) {
      res.truncated = true;
      return;
    }
    SearchMatch m{};
    m.abs_offset = abs_off;
    m.block_id = static_cast<uint32_t>(abs_off / bs);
    m.block_offset = static_cast<uint32_t>(abs_off % bs);
    m.logical_offset = log_off;
    m.match_length = static_cast<uint32_t>(pattern.size());
    for (size_t k = 0; k < std::min<size_t>(pattern.size(), 16U); ++k) {
      if (k > 0) m.matched_hex += ' ';
      m.matched_hex += byte_to_hex(pattern[k]);
    }
    for (size_t k = 0; k < ctx_len; ++k) {
      m.context_ascii.push_back(to_printable_ascii(ctx_ptr[k]));
    }
    all_matches.push_back(std::move(m));
  };

  if (scope == "file") {
    GfsFileSystem fs;
    fs.mount(disk, false);
    FileEntryInfo info{};
    const std::vector<uint8_t> content = fs.read_file(file_path, &info);
    res.bytes_scanned = content.size();
    for (size_t i = 0; i + pattern.size() <= content.size(); ++i) {
      if (std::memcmp(content.data() + i, pattern.data(), pattern.size()) == 0) {
        const uint32_t l_blk = static_cast<uint32_t>(i / bs);
        const uint32_t b_off = static_cast<uint32_t>(i % bs);
        const uint32_t p_blk = l_blk < info.all_data_blocks.size() ? info.all_data_blocks[l_blk] : 0U;
        const size_t ctx_len = std::min<size_t>(24U, content.size() - i);
        push_match(static_cast<uint64_t>(p_blk) * bs + b_off, i, content.data() + i, ctx_len);
        if (res.truncated) break;
      }
    }
  } else {
    // Build contiguous byte ranges to stream-scan
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    if (scope == "disk") {
      ranges.emplace_back(0ULL, disk.file_size_bytes());
    } else if (scope == "block_range") {
      if (start_block > end_block || end_block >= disk.total_blocks()) {
        throw std::out_of_range("ERR_OUT_OF_BOUNDS: Invalid block range for search");
      }
      ranges.emplace_back(
          static_cast<uint64_t>(start_block) * bs,
          static_cast<uint64_t>(end_block + 1U) * bs);
    } else {
      GfsFileSystem fs;
      fs.mount(disk, false);
      const auto& bm = fs.block_bitmap();
      const Superblock& sb = fs.superblock();
      auto bm_set = [&](uint32_t b) {
        return (bm[b / 8U] & static_cast<uint8_t>(1U << (b % 8U))) != 0;
      };
      if (scope == "metadata") {
        ranges.emplace_back(0ULL, static_cast<uint64_t>(sb.data_region_start) * bs);
        ranges.emplace_back(static_cast<uint64_t>(sb.journal_start) * bs,
                            static_cast<uint64_t>(sb.total_blocks) * bs);
      } else if (scope == "allocated" || scope == "unallocated") {
        const bool want_alloc = (scope == "allocated");
        const uint32_t d_end = sb.data_region_start + sb.data_blocks_count;
        bool in_run = false;
        uint32_t run_start = 0U;
        for (uint32_t b = sb.data_region_start; b < d_end; ++b) {
          if (bm_set(b) == want_alloc && !in_run) {
            in_run = true;
            run_start = b;
          } else if (bm_set(b) != want_alloc && in_run) {
            ranges.emplace_back(static_cast<uint64_t>(run_start) * bs, static_cast<uint64_t>(b) * bs);
            in_run = false;
          }
        }
        if (in_run) {
          ranges.emplace_back(static_cast<uint64_t>(run_start) * bs, static_cast<uint64_t>(d_end) * bs);
        }
      } else {
        throw std::invalid_argument("ERR_BAD_PARAMS: Unknown search scope: " + scope);
      }
    }

    // Streaming 64 KiB chunked scan with (pattern.size() - 1) overlap across chunks!
    constexpr size_t kChunk = 65536U;
    std::vector<uint8_t> window;
    std::vector<uint8_t> chunk_buf;

    for (const auto& [r_start, r_end] : ranges) {
      if (r_end <= r_start || res.truncated) continue;
      window.clear();
      uint64_t cursor = r_start;
      while (cursor < r_end && !res.truncated) {
        const size_t to_read = static_cast<size_t>(std::min<uint64_t>(kChunk, r_end - cursor));
        disk.read_raw(cursor, to_read, chunk_buf);
        res.bytes_scanned += to_read;

        const size_t prev_keep = window.size();
        const uint64_t window_base = cursor - prev_keep;
        window.insert(window.end(), chunk_buf.begin(), chunk_buf.end());

        // Only report matches whose start offset falls in [prev_keep, window.size() - pattern.size()]
        // OR in the overlap zone that wasn't already fully scanned:
        const size_t scan_limit = (window.size() >= pattern.size()) ? (window.size() - pattern.size() + 1U) : 0U;
        for (size_t i = 0; i < scan_limit; ++i) {
          if (std::memcmp(window.data() + i, pattern.data(), pattern.size()) == 0) {
            const size_t ctx_len = std::min<size_t>(24U, window.size() - i);
            push_match(window_base + i, window_base + i, window.data() + i, ctx_len);
            if (res.truncated) break;
          }
        }

        // Keep tail of length (pattern.size() - 1) for cross-chunk matches without re-reporting!
        const size_t keep = std::min<size_t>(pattern.size() > 0 ? pattern.size() - 1U : 0U, window.size());
        if (keep > 0) {
          window.erase(window.begin(), window.end() - static_cast<std::ptrdiff_t>(keep));
        } else {
          window.clear();
        }
        cursor += to_read;
      }
    }
  }

  res.total_matches = static_cast<uint32_t>(all_matches.size());
  if (offset < all_matches.size() && limit > 0U) {
    const size_t end = std::min<size_t>(all_matches.size(), static_cast<size_t>(offset) + limit);
    res.matches.assign(all_matches.begin() + static_cast<std::ptrdiff_t>(offset),
                       all_matches.begin() + static_cast<std::ptrdiff_t>(end));
  }
  return res;
}

ByteAnalysisMetrics HexInspector::analyze_bytes(const std::vector<uint8_t>& bytes) {
  ByteAnalysisMetrics m{};
  m.total_bytes = bytes.size();
  m.histogram.fill(0ULL);
  if (bytes.empty()) {
    m.sha256 = compute_sha256_hex(bytes);
    return m;
  }

  for (uint8_t b : bytes) {
    ++m.histogram[b];
    if (b == 0U) {
      ++m.zero_bytes;
    } else {
      ++m.nonzero_bytes;
    }
    if (b >= 0x20U && b <= 0x7EU) {
      ++m.printable_ascii_bytes;
    }
  }

  const double n = static_cast<double>(m.total_bytes);
  m.zero_pct = (static_cast<double>(m.zero_bytes) / n) * 100.0;
  m.printable_ascii_pct = (static_cast<double>(m.printable_ascii_bytes) / n) * 100.0;

  double entropy = 0.0;
  for (uint64_t count : m.histogram) {
    if (count == 0ULL) continue;
    ++m.unique_byte_values;
    const double p = static_cast<double>(count) / n;
    entropy -= p * std::log2(p);
  }
  m.shannon_entropy = entropy;
  m.crc32 = compute_crc32(bytes);
  m.sha256 = compute_sha256_hex(bytes);
  return m;
}

ByteAnalysisMetrics HexInspector::analyze_disk_range(
    const std::filesystem::path& disk_path,
    uint64_t offset,
    uint32_t length) {
  if (length > kMaxHexReadBytes) {
    throw std::invalid_argument("ERR_BAD_PARAMS: Analysis range exceeds 64 KiB window");
  }
  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  std::vector<uint8_t> buf;
  disk.read_raw(offset, length, buf);
  return analyze_bytes(buf);
}

CompareResult HexInspector::compare_byte_buffers(
    const std::vector<uint8_t>& a,
    const std::vector<uint8_t>& b,
    const std::string& mode,
    uint32_t max_diffs) {
  CompareResult res{};
  res.mode = mode;
  res.len_a = a.size();
  res.len_b = b.size();
  res.sha256_a = compute_sha256_hex(a);
  res.sha256_b = compute_sha256_hex(b);

  const size_t min_len = std::min(a.size(), b.size());
  const size_t max_len = std::max(a.size(), b.size());
  res.total_compared_bytes = max_len;
  res.missing_bytes = max_len - min_len;

  for (size_t i = 0; i < max_len; ++i) {
    const bool has_a = i < a.size();
    const bool has_b = i < b.size();
    if (has_a && has_b && a[i] == b[i]) {
      ++res.matching_bytes;
    } else {
      ++res.differing_bytes;
      if (!res.has_first_diff) {
        res.has_first_diff = true;
        res.first_diff_offset = i;
      }
      if (res.diffs.size() < max_diffs) {
        res.diffs.push_back(ByteDiffEntry{
            i,
            has_a,
            has_b,
            has_a ? a[i] : static_cast<uint8_t>(0),
            has_b ? b[i] : static_cast<uint8_t>(0),
        });
      }
    }
  }

  if (max_len > 0) {
    res.matching_pct = (static_cast<double>(res.matching_bytes) / static_cast<double>(max_len)) * 100.0;
  }
  return res;
}

CompareResult HexInspector::compare_disk_sources(
    const std::filesystem::path& disk_path,
    const std::string& mode,
    uint64_t a_start,
    uint64_t a_len,
    uint64_t b_start,
    uint64_t b_len,
    const std::string& path_a,
    const std::string& path_b,
    const std::filesystem::path& artifact_path) {
  VirtualDisk disk;
  disk.open_image(disk_path, 512U, false);
  const uint32_t bs = probe_block_size(disk);
  disk.set_block_size(bs);

  if (mode == "blocks") {
    std::vector<uint8_t> ba, bb;
    disk.read_block(static_cast<uint32_t>(a_start), ba);
    disk.read_block(static_cast<uint32_t>(b_start), bb);
    return compare_byte_buffers(ba, bb, mode);
  }
  if (mode == "ranges") {
    if (a_len > kMaxHexReadBytes || b_len > kMaxHexReadBytes) {
      throw std::invalid_argument("ERR_BAD_PARAMS: Range comparison exceeds 64 KiB limit");
    }
    std::vector<uint8_t> ba, bb;
    disk.read_raw(a_start, static_cast<size_t>(a_len), ba);
    disk.read_raw(b_start, static_cast<size_t>(b_len), bb);
    return compare_byte_buffers(ba, bb, mode);
  }
  if (mode == "primary_vs_backup_sb") {
    std::vector<uint8_t> ba, bb;
    disk.read_block(1U, ba);
    disk.read_block(2U, bb);
    CompareResult res = compare_byte_buffers(ba, bb, mode);
    const auto ds_a = BinaryStructureParser::decode_superblock(ba, 1U, bs, false);
    const auto ds_b = BinaryStructureParser::decode_superblock(bb, 2U, bs * 2ULL, true);
    for (size_t i = 0; i < std::min(ds_a.fields.size(), ds_b.fields.size()); ++i) {
      const bool differs = ds_a.fields[i].value != ds_b.fields[i].value;
      res.sb_field_diffs.push_back(SuperblockFieldDiff{
          ds_a.fields[i].name,
          ds_a.fields[i].value,
          ds_b.fields[i].value,
          differs,
          false,
      });
    }
    return res;
  }
  if (mode == "files") {
    GfsFileSystem fs;
    fs.mount(disk, false);
    const auto fa = fs.read_file(path_a);
    const auto fb = fs.read_file(path_b);
    return compare_byte_buffers(fa, fb, mode);
  }
  if (mode == "file_vs_artifact") {
    GfsFileSystem fs;
    fs.mount(disk, false);
    const auto fa = fs.read_file(path_a);
    std::ifstream ifs(artifact_path, std::ios::binary);
    if (!ifs) throw std::runtime_error("ERR_NOT_FOUND: Artifact file not found");
    const std::vector<uint8_t> fb((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return compare_byte_buffers(fa, fb, mode);
  }

  throw std::invalid_argument("ERR_BAD_PARAMS: Unsupported comparison mode: " + mode);
}

}  // namespace ghostfs
