#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ghostfs {

struct DecodedField {
  std::string name;
  uint64_t abs_offset = 0;
  uint32_t rel_offset = 0;
  uint32_t length = 0;
  std::string raw_hex;
  std::string value;
  std::string data_type;
  std::string description;
  bool has_validation = false;
  bool valid = true;
  std::string validation_note;
};

struct DecodedStructure {
  std::string structure_type;
  uint32_t block_id = 0;
  uint64_t base_offset = 0;
  bool checksum_present = false;
  bool checksum_valid = true;
  std::vector<std::string> warnings;
  std::vector<DecodedField> fields;
};

class BinaryStructureParser {
 public:
  static DecodedStructure decode_boot_block(const std::vector<uint8_t>& block_bytes, uint64_t base_offset);
  static DecodedStructure decode_superblock(
      const std::vector<uint8_t>& block_bytes,
      uint32_t block_id,
      uint64_t base_offset,
      bool is_backup);
  static DecodedStructure decode_inode_table_block(
      const std::vector<uint8_t>& block_bytes,
      uint32_t block_id,
      uint64_t base_offset,
      uint32_t first_inode_id);
  static DecodedStructure decode_dir_block(
      const std::vector<uint8_t>& block_bytes,
      uint32_t block_id,
      uint64_t base_offset);
  static DecodedStructure decode_indirect_block(
      const std::vector<uint8_t>& block_bytes,
      uint32_t block_id,
      uint64_t base_offset,
      uint32_t data_start,
      uint32_t data_end);
  static DecodedStructure decode_journal_block(
      const std::vector<uint8_t>& block_bytes,
      uint32_t block_id,
      uint64_t base_offset);
  static DecodedStructure decode_bitmap_block(
      const std::vector<uint8_t>& block_bytes,
      uint32_t block_id,
      uint64_t base_offset,
      const std::string& bitmap_kind,
      uint32_t total_items);
};

}  // namespace ghostfs
