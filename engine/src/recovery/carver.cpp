#include "ghostfs/recovery.hpp"

#include "ghostfs/allocator.hpp"
#include "ghostfs/crc32.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace ghostfs {

namespace {

constexpr size_t kMaxCarvedFileSize = 16U * 1024U * 1024U;  // 16 MiB bound

uint32_t read_u32_be(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24U) |
         (static_cast<uint32_t>(p[1]) << 16U) |
         (static_cast<uint32_t>(p[2]) << 8U) |
         static_cast<uint32_t>(p[3]);
}

uint16_t read_u16_be(const uint8_t* p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8U) | p[1]);
}

std::string hex_id(uint64_t val) {
  std::ostringstream oss;
  oss << std::hex << val;
  return oss.str();
}

void populate_carved_blocks(
    RecoveryCandidate& cand,
    uint32_t extent_start_blk,
    uint32_t block_size,
    size_t rel_start,
    size_t length) {
  const uint32_t first_rel_blk = static_cast<uint32_t>(rel_start / block_size);
  const uint32_t last_rel_blk = static_cast<uint32_t>((rel_start + length - 1U) / block_size);
  for (uint32_t rb = first_rel_blk; rb <= last_rel_blk; ++rb) {
    const uint32_t phys = extent_start_blk + rb;
    cand.source_blocks.push_back(phys);
    cand.source_byte_offsets.push_back(static_cast<uint64_t>(phys) * block_size);
    LogicalBlockStatus lbs{};
    lbs.logical_index = rb - first_rel_blk;
    lbs.physical_block = phys;
    lbs.disk_byte_offset = static_cast<uint64_t>(phys) * block_size;
    lbs.state = "intact";
    lbs.detail = "Carved from contiguous unallocated block";
    cand.block_statuses.push_back(std::move(lbs));
  }
}

bool try_carve_png(
    const std::vector<uint8_t>& buf,
    size_t pos,
    size_t& out_end,
    bool& out_has_iend,
    bool& out_crcs_ok,
    std::string& out_warn) {
  static constexpr std::array<uint8_t, 8> kPngSig = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  if (pos + 33 > buf.size()) return false;  // Sig(8) + IHDR(25)
  if (std::memcmp(buf.data() + pos, kPngSig.data(), 8) != 0) return false;

  size_t cursor = pos + 8;
  bool seen_ihdr = false;
  out_has_iend = false;
  out_crcs_ok = true;

  while (cursor + 12 <= buf.size() && (cursor - pos) < kMaxCarvedFileSize) {
    const uint32_t chunk_len = read_u32_be(buf.data() + cursor);
    if (chunk_len > kMaxCarvedFileSize || cursor + 12ULL + chunk_len > buf.size()) {
      out_warn = "PNG chunk truncated before IEND inside contiguous free extent.";
      break;
    }
    const uint8_t* type_ptr = buf.data() + cursor + 4;
    for (int i = 0; i < 4; ++i) {
      if (!std::isalpha(type_ptr[i])) {
        return false;
      }
    }
    if (!seen_ihdr) {
      if (std::memcmp(type_ptr, "IHDR", 4) != 0 || chunk_len != 13U) {
        return false;
      }
      seen_ihdr = true;
    }

    const uint32_t stored_crc = read_u32_be(buf.data() + cursor + 8 + chunk_len);
    const uint32_t calc_crc = compute_crc32(type_ptr, 4U + chunk_len);
    if (stored_crc != calc_crc) {
      out_crcs_ok = false;
      out_warn = "One or more PNG chunk CRC32 values did not match.";
    }

    cursor += 12ULL + chunk_len;
    if (std::memcmp(type_ptr, "IEND", 4) == 0 && chunk_len == 0U) {
      out_has_iend = true;
      out_end = cursor;
      return true;
    }
  }

  if (seen_ihdr && cursor > pos + 33) {
    out_end = cursor;
    return true;
  }
  return false;
}

bool try_carve_jpeg(const std::vector<uint8_t>& buf, size_t pos, size_t& out_end) {
  if (pos + 4 > buf.size()) return false;
  if (buf[pos] != 0xFF || buf[pos + 1] != 0xD8 || buf[pos + 2] != 0xFF) return false;
  const uint8_t m3 = buf[pos + 3];
  if (m3 < 0xC0) return false;

  size_t cursor = pos + 2;
  bool seen_frame_or_table = false;
  bool seen_sos = false;

  while (cursor + 2 <= buf.size() && (cursor - pos) < kMaxCarvedFileSize) {
    if (buf[cursor] != 0xFF) return false;
    while (cursor < buf.size() && buf[cursor] == 0xFF) ++cursor;
    if (cursor >= buf.size()) return false;
    const uint8_t marker = buf[cursor++];

    if (marker == 0xD9) {  // EOI
      if (seen_sos || seen_frame_or_table) {
        out_end = cursor;
        return true;
      }
      return false;
    }
    if (marker == 0x00 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }
    if (cursor + 2 > buf.size()) return false;
    const uint16_t seg_len = read_u16_be(buf.data() + cursor);
    if (seg_len < 2 || cursor + seg_len > buf.size()) return false;

    if (marker == 0xDB || marker == 0xC4 || (marker >= 0xC0 && marker <= 0xC3) ||
        (marker >= 0xE0 && marker <= 0xEF)) {
      seen_frame_or_table = true;
    }

    cursor += seg_len;
    if (marker == 0xDA) {  // SOS -> scan entropy-coded data for EOI (FF D9)
      seen_sos = true;
      while (cursor + 1 < buf.size() && (cursor - pos) < kMaxCarvedFileSize) {
        if (buf[cursor] == 0xFF) {
          const uint8_t next = buf[cursor + 1];
          if (next == 0xD9) {
            out_end = cursor + 2;
            return true;
          }
          if (next == 0x00 || (next >= 0xD0 && next <= 0xD7) || next == 0xFF) {
            cursor += (next == 0xFF) ? 1 : 2;
            continue;
          }
          // Another marker inside stream (e.g. DNL or next scan)
          break;
        }
        ++cursor;
      }
      if (cursor + 1 >= buf.size()) return false;
    }
  }
  return false;
}

bool try_carve_pdf(const std::vector<uint8_t>& buf, size_t pos, size_t& out_end) {
  if (pos + 8 > buf.size()) return false;
  if (std::memcmp(buf.data() + pos, "%PDF-", 5) != 0) return false;
  if (!std::isdigit(buf[pos + 5]) || buf[pos + 6] != '.' || !std::isdigit(buf[pos + 7])) {
    return false;
  }

  const size_t max_end = std::min(buf.size(), pos + kMaxCarvedFileSize);
  const std::string_view view(reinterpret_cast<const char*>(buf.data() + pos), max_end - pos);

  size_t last_eof_end = 0;
  size_t search_from = 8;
  while (true) {
    const size_t eof_pos = view.find("%%EOF", search_from);
    if (eof_pos == std::string_view::npos) break;
    size_t end_after_eof = eof_pos + 5;
    while (end_after_eof < view.size() && (view[end_after_eof] == '\r' || view[end_after_eof] == '\n')) {
      ++end_after_eof;
    }
    last_eof_end = end_after_eof;
    // Check if an incremental PDF update follows nearby (contains "startxref" before next %%EOF).
    const size_t next_eof = view.find("%%EOF", end_after_eof);
    if (next_eof == std::string_view::npos) break;
    const auto between = view.substr(end_after_eof, next_eof - end_after_eof);
    if (between.find("startxref") == std::string_view::npos &&
        between.find("obj") == std::string_view::npos) {
      break;
    }
    search_from = end_after_eof;
  }

  if (last_eof_end == 0) return false;
  const auto doc = view.substr(0, last_eof_end);
  if (doc.find("obj") == std::string_view::npos &&
      doc.find("/Type") == std::string_view::npos &&
      doc.find("xref") == std::string_view::npos) {
    return false;
  }
  out_end = pos + last_eof_end;
  return true;
}

bool is_printable_txt_run(const uint8_t* data, size_t len, size_t& trimmed_len) {
  trimmed_len = len;
  while (trimmed_len > 0 && data[trimmed_len - 1] == 0x00) {
    --trimmed_len;
  }
  if (trimmed_len < 16U) return false;

  size_t printable = 0;
  size_t alnum = 0;
  for (size_t i = 0; i < trimmed_len; ++i) {
    const uint8_t c = data[i];
    if (c == 0x00) return false;
    if ((c >= 0x20 && c <= 0x7E) || c == '\n' || c == '\r' || c == '\t') {
      ++printable;
      if (std::isalnum(c)) ++alnum;
    } else if (c >= 0x80) {
      // Allow UTF-8 continuation/leading bytes conservatively
      ++printable;
    }
  }
  const double ratio = static_cast<double>(printable) / static_cast<double>(trimmed_len);
  return ratio >= 0.95 && alnum >= 4U;
}

}  // namespace

std::vector<RecoveryCandidate> run_signature_carving(
    const VirtualDisk& disk,
    const GfsFileSystem& fs,
    bool enable_txt_heuristic,
    uint32_t& out_blocks_examined,
    uint64_t& out_bytes_examined,
    std::vector<std::string>& out_warnings) {
  const Superblock& sb = fs.superblock();
  const auto free_extents = BlockAllocator::find_free_extents(
      fs.block_bitmap(), sb.data_region_start, sb.data_blocks_count);

  out_blocks_examined = 0U;
  out_bytes_examined = 0ULL;
  out_warnings.push_back(
      "Signature carving scans contiguous unallocated extents only; fragmented files without metadata cannot be automatically reassembled.");

  std::vector<RecoveryCandidate> carved;
  std::vector<uint8_t> blk_buf;

  for (const auto& ext : free_extents) {
    // Process in bounded chunks of at most 16 MiB per window to prevent unbounded memory allocation.
    const uint32_t max_blocks_per_window =
        std::max<uint32_t>(1U, static_cast<uint32_t>(kMaxCarvedFileSize / sb.block_size));

    uint32_t ext_offset_blk = 0U;
    while (ext_offset_blk < ext.length) {
      const uint32_t win_blocks = std::min<uint32_t>(max_blocks_per_window, ext.length - ext_offset_blk);
      const uint32_t win_start_blk = ext.start_block + ext_offset_blk;

      std::vector<uint8_t> buf;
      buf.reserve(static_cast<size_t>(win_blocks) * sb.block_size);
      for (uint32_t i = 0; i < win_blocks; ++i) {
        disk.read_block(win_start_blk + i, blk_buf);
        buf.insert(buf.end(), blk_buf.begin(), blk_buf.end());
        ++out_blocks_examined;
        out_bytes_examined += sb.block_size;
      }

      std::vector<bool> claimed_bytes(buf.size(), false);
      size_t pos = 0;
      while (pos + 4 <= buf.size()) {
        size_t end_pos = 0;
        bool has_iend = false;
        bool png_crcs_ok = false;
        std::string png_warn;

        if (try_carve_png(buf, pos, end_pos, has_iend, png_crcs_ok, png_warn)) {
          const uint64_t disk_off = static_cast<uint64_t>(win_start_blk) * sb.block_size + pos;
          const size_t sz = end_pos - pos;
          RecoveryCandidate c{};
          c.candidate_id = "carve_png_0x" + hex_id(disk_off);
          c.recovery_method = "carving";
          c.file_type = "png";
          c.filename = "carved_0x" + hex_id(disk_off) + ".png";
          c.original_path = "/" + c.filename;
          c.filename_inferred = true;
          c.original_size_bytes = sz;
          c.recoverable_bytes = sz;
          c.recovery_pct = 100.0;
          c.reconstructed_crc32 = compute_crc32(buf.data() + pos, sz);
          if (has_iend && png_crcs_ok) {
            c.integrity_status = "structure_verified";
            c.crc32_matched = false;
            c.classification = RecoveryClassification::CompleteUnverified;
            c.evidence.push_back(
                "Validated PNG signature, chunk structure, chunk CRC32s, and IEND footer "
                "(structural validation only; no original filesystem checksum available).");
          } else if (has_iend) {
            c.integrity_status = "unverified";
            c.classification = RecoveryClassification::CompleteUnverified;
            if (!png_warn.empty()) c.warnings.push_back(png_warn);
          } else {
            c.integrity_status = "partial";
            c.classification = RecoveryClassification::PartiallyRecoverable;
            c.warnings.push_back(png_warn.empty() ? "PNG truncated before IEND." : png_warn);
          }
          c.classification_str = classification_to_string(c.classification);
          c.recoverable_ranges.push_back(ByteRange{0ULL, sz});
          populate_carved_blocks(c, win_start_blk, sb.block_size, pos, sz);
          carved.push_back(std::move(c));

          std::fill(claimed_bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                    claimed_bytes.begin() + static_cast<std::ptrdiff_t>(end_pos),
                    true);
          pos = end_pos;
          continue;
        }

        if (try_carve_jpeg(buf, pos, end_pos)) {
          const uint64_t disk_off = static_cast<uint64_t>(win_start_blk) * sb.block_size + pos;
          const size_t sz = end_pos - pos;
          RecoveryCandidate c{};
          c.candidate_id = "carve_jpeg_0x" + hex_id(disk_off);
          c.recovery_method = "carving";
          c.file_type = "jpeg";
          c.filename = "carved_0x" + hex_id(disk_off) + ".jpg";
          c.original_path = "/" + c.filename;
          c.filename_inferred = true;
          c.original_size_bytes = sz;
          c.recoverable_bytes = sz;
          c.recovery_pct = 100.0;
          c.reconstructed_crc32 = compute_crc32(buf.data() + pos, sz);
          c.integrity_status = "unverified";
          c.classification = RecoveryClassification::CompleteUnverified;
          c.classification_str = classification_to_string(c.classification);
          c.recoverable_ranges.push_back(ByteRange{0ULL, sz});
          c.evidence.push_back("Validated JPEG SOI marker, segment headers, SOS stream, and EOI marker.");
          c.warnings.push_back("JPEG format lacks a whole-file integrity checksum; visually verify recovered image.");
          populate_carved_blocks(c, win_start_blk, sb.block_size, pos, sz);
          carved.push_back(std::move(c));

          std::fill(claimed_bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                    claimed_bytes.begin() + static_cast<std::ptrdiff_t>(end_pos),
                    true);
          pos = end_pos;
          continue;
        }

        if (try_carve_pdf(buf, pos, end_pos)) {
          const uint64_t disk_off = static_cast<uint64_t>(win_start_blk) * sb.block_size + pos;
          const size_t sz = end_pos - pos;
          RecoveryCandidate c{};
          c.candidate_id = "carve_pdf_0x" + hex_id(disk_off);
          c.recovery_method = "carving";
          c.file_type = "pdf";
          c.filename = "carved_0x" + hex_id(disk_off) + ".pdf";
          c.original_path = "/" + c.filename;
          c.filename_inferred = true;
          c.original_size_bytes = sz;
          c.recoverable_bytes = sz;
          c.recovery_pct = 100.0;
          c.reconstructed_crc32 = compute_crc32(buf.data() + pos, sz);
          c.integrity_status = "unverified";
          c.classification = RecoveryClassification::CompleteUnverified;
          c.classification_str = classification_to_string(c.classification);
          c.recoverable_ranges.push_back(ByteRange{0ULL, sz});
          c.evidence.push_back("Validated %PDF header, object/xref structure, and %%EOF trailer.");
          populate_carved_blocks(c, win_start_blk, sb.block_size, pos, sz);
          carved.push_back(std::move(c));

          std::fill(claimed_bytes.begin() + static_cast<std::ptrdiff_t>(pos),
                    claimed_bytes.begin() + static_cast<std::ptrdiff_t>(end_pos),
                    true);
          pos = end_pos;
          continue;
        }

        ++pos;
      }

      // Optional TXT heuristic on unclaimed blocks in this window.
      if (enable_txt_heuristic) {
        for (uint32_t b = 0; b < win_blocks; ) {
          const size_t b_start = static_cast<size_t>(b) * sb.block_size;
          bool any_claimed = false;
          for (size_t k = 0; k < sb.block_size; ++k) {
            if (claimed_bytes[b_start + k]) {
              any_claimed = true;
              break;
            }
          }
          if (any_claimed) {
            ++b;
            continue;
          }

          // Group consecutive unclaimed printable blocks
          uint32_t run_end_blk = b;
          while (run_end_blk < win_blocks) {
            const size_t cur_off = static_cast<size_t>(run_end_blk) * sb.block_size;
            bool blk_claimed = false;
            for (size_t k = 0; k < sb.block_size; ++k) {
              if (claimed_bytes[cur_off + k]) {
                blk_claimed = true;
                break;
              }
            }
            if (blk_claimed) break;
            size_t dummy_trim = 0;
            if (!is_printable_txt_run(buf.data() + cur_off, sb.block_size, dummy_trim)) {
              break;
            }
            // Stop TXT run if block ends with null padding (natural file end)
            const bool ends_with_null = (buf[cur_off + sb.block_size - 1] == 0x00);
            ++run_end_blk;
            if (ends_with_null) break;
          }

          if (run_end_blk > b) {
            const size_t raw_len = static_cast<size_t>(run_end_blk - b) * sb.block_size;
            size_t trimmed_len = 0;
            if (is_printable_txt_run(buf.data() + b_start, raw_len, trimmed_len)) {
              const uint64_t disk_off = static_cast<uint64_t>(win_start_blk + b) * sb.block_size;
              RecoveryCandidate c{};
              c.candidate_id = "carve_txt_0x" + hex_id(disk_off);
              c.recovery_method = "carving";
              c.file_type = "txt";
              c.filename = "carved_txt_0x" + hex_id(disk_off) + ".txt";
              c.original_path = "/" + c.filename;
              c.filename_inferred = true;
              c.original_size_bytes = trimmed_len;
              c.recoverable_bytes = trimmed_len;
              c.recovery_pct = 100.0;
              c.reconstructed_crc32 = compute_crc32(buf.data() + b_start, trimmed_len);
              c.integrity_status = "unverified";
              c.classification = RecoveryClassification::CompleteUnverified;
              c.classification_str = classification_to_string(c.classification);
              c.recoverable_ranges.push_back(ByteRange{0ULL, trimmed_len});
              c.evidence.push_back("Detected printable ASCII/UTF-8 text run (>= 95% printable ratio).");
              c.warnings.push_back(
                  "TXT files have no universal magic signature; this is a heuristic candidate.");
              populate_carved_blocks(c, win_start_blk, sb.block_size, b_start, trimmed_len);
              carved.push_back(std::move(c));
            }
            b = run_end_blk;
          } else {
            ++b;
          }
        }
      }

      ext_offset_blk += win_blocks;
    }
  }

  return carved;
}

}  // namespace ghostfs
