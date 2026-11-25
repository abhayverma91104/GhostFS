#include "ghostfs/binary_parser.hpp"

#include "ghostfs/crc32.hpp"
#include "ghostfs/endian.hpp"
#include "ghostfs/gfs_format.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ghostfs {

namespace {

std::string bytes_to_hex(const uint8_t* data, size_t len) {
  std::ostringstream oss;
  oss << std::hex << std::uppercase << std::setfill('0');
  for (size_t i = 0; i < len; ++i) {
    if (i > 0) oss << ' ';
    oss << std::setw(2) << static_cast<unsigned>(data[i]);
  }
  return oss.str();
}

std::string u32_hex(uint32_t v) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << v;
  return oss.str();
}

DecodedField make_field(
    const std::string& name,
    uint64_t base_offset,
    uint32_t rel_offset,
    uint32_t length,
    const uint8_t* buf,
    const std::string& value,
    const std::string& data_type,
    const std::string& desc) {
  DecodedField f{};
  f.name = name;
  f.abs_offset = base_offset + rel_offset;
  f.rel_offset = rel_offset;
  f.length = length;
  f.raw_hex = bytes_to_hex(buf + rel_offset, length);
  f.value = value;
  f.data_type = data_type;
  f.description = desc;
  return f;
}

}  // namespace

DecodedStructure BinaryStructureParser::decode_boot_block(
    const std::vector<uint8_t>& block_bytes,
    uint64_t base_offset) {
  if (block_bytes.size() < kBootBlockBytes) {
    throw std::invalid_argument("Block buffer smaller than 64-byte BootBlock");
  }
  const uint8_t* p = block_bytes.data();
  BootBlock boot{};
  bool crc_ok = false;
  const bool sig_ok = deserialize_boot_block(p, boot, crc_ok);

  DecodedStructure ds{};
  ds.structure_type = "boot_block";
  ds.block_id = 0U;
  ds.base_offset = base_offset;
  ds.checksum_present = true;
  ds.checksum_valid = crc_ok;

  if (!sig_ok) ds.warnings.push_back("Boot signature or version/block_size invalid.");
  if (!crc_ok) ds.warnings.push_back("BootBlock CRC32 mismatch.");

  auto f_sig = make_field(
      "signature", base_offset, 0x00, 8, p,
      std::string(boot.signature.data(), 7), "char[8]", "GFS ASCII signature ('GHOSTFS\\0')");
  f_sig.has_validation = true;
  f_sig.valid = (std::string(boot.signature.data(), 7) == "GHOSTFS");
  ds.fields.push_back(std::move(f_sig));

  ds.fields.push_back(make_field("version", base_offset, 0x08, 4, p, std::to_string(boot.version), "uint32_le", "GFS format version"));
  ds.fields.push_back(make_field("block_size", base_offset, 0x0C, 4, p, std::to_string(boot.block_size), "uint32_le", "Bytes per logical block"));
  ds.fields.push_back(make_field("total_size_bytes", base_offset, 0x10, 8, p, std::to_string(boot.total_size_bytes), "uint64_le", "Total virtual disk size in bytes"));
  ds.fields.push_back(make_field("created_at_ms", base_offset, 0x18, 8, p, std::to_string(boot.created_at_ms), "uint64_le", "Creation timestamp (epoch ms)"));
  ds.fields.push_back(make_field("primary_sb_block", base_offset, 0x20, 4, p, std::to_string(boot.primary_sb_block), "uint32_le", "Primary superblock LBA"));
  ds.fields.push_back(make_field("backup_sb_block", base_offset, 0x24, 4, p, std::to_string(boot.backup_sb_block), "uint32_le", "Backup superblock LBA"));

  auto f_crc = make_field("crc32", base_offset, 0x3C, 4, p, u32_hex(boot.crc32), "uint32_le", "IEEE 802.3 CRC32 over bytes [0x00..0x3B]");
  f_crc.has_validation = true;
  f_crc.valid = crc_ok;
  f_crc.validation_note = crc_ok ? "Matches computed CRC32" : "CRC32 mismatch";
  ds.fields.push_back(std::move(f_crc));

  return ds;
}

DecodedStructure BinaryStructureParser::decode_superblock(
    const std::vector<uint8_t>& block_bytes,
    uint32_t block_id,
    uint64_t base_offset,
    bool is_backup) {
  if (block_bytes.size() < kSuperblockBytes) {
    throw std::invalid_argument("Block buffer smaller than 128-byte Superblock");
  }
  const uint8_t* p = block_bytes.data();
  Superblock sb{};
  bool crc_ok = false;
  const bool hdr_ok = deserialize_superblock(p, sb, crc_ok);

  DecodedStructure ds{};
  ds.structure_type = is_backup ? "backup_superblock" : "primary_superblock";
  ds.block_id = block_id;
  ds.base_offset = base_offset;
  ds.checksum_present = true;
  ds.checksum_valid = crc_ok;

  if (!hdr_ok) ds.warnings.push_back("Superblock magic/version/block_size invalid.");
  if (!crc_ok) ds.warnings.push_back("Superblock CRC32 mismatch.");

  auto f_magic = make_field("magic", base_offset, 0x00, 4, p, u32_hex(sb.magic) + " (GFS1)", "uint32_le", "GFS1 magic identifier");
  f_magic.has_validation = true;
  f_magic.valid = (sb.magic == kSuperblockMagic);
  ds.fields.push_back(std::move(f_magic));

  ds.fields.push_back(make_field("version", base_offset, 0x04, 4, p, std::to_string(sb.version), "uint32_le", "Filesystem version"));
  ds.fields.push_back(make_field("block_size", base_offset, 0x08, 4, p, std::to_string(sb.block_size), "uint32_le", "Block size in bytes"));
  ds.fields.push_back(make_field("total_blocks", base_offset, 0x0C, 4, p, std::to_string(sb.total_blocks), "uint32_le", "Total LBA blocks"));
  ds.fields.push_back(make_field("reserved_blocks", base_offset, 0x10, 4, p, std::to_string(sb.reserved_blocks), "uint32_le", "Boot + Primary/Backup superblocks"));
  ds.fields.push_back(make_field("inode_bitmap_start", base_offset, 0x14, 4, p, std::to_string(sb.inode_bitmap_start), "uint32_le", "Inode bitmap start LBA"));
  ds.fields.push_back(make_field("inode_bitmap_blocks", base_offset, 0x18, 4, p, std::to_string(sb.inode_bitmap_blocks), "uint32_le", "Inode bitmap block count"));
  ds.fields.push_back(make_field("block_bitmap_start", base_offset, 0x1C, 4, p, std::to_string(sb.block_bitmap_start), "uint32_le", "Block bitmap start LBA"));
  ds.fields.push_back(make_field("block_bitmap_blocks", base_offset, 0x20, 4, p, std::to_string(sb.block_bitmap_blocks), "uint32_le", "Block bitmap block count"));
  ds.fields.push_back(make_field("inode_table_start", base_offset, 0x24, 4, p, std::to_string(sb.inode_table_start), "uint32_le", "Inode table start LBA"));
  ds.fields.push_back(make_field("inode_table_blocks", base_offset, 0x28, 4, p, std::to_string(sb.inode_table_blocks), "uint32_le", "Inode table block count"));
  ds.fields.push_back(make_field("data_region_start", base_offset, 0x2C, 4, p, std::to_string(sb.data_region_start), "uint32_le", "Data region start LBA"));
  ds.fields.push_back(make_field("data_blocks_count", base_offset, 0x30, 4, p, std::to_string(sb.data_blocks_count), "uint32_le", "Allocatable data blocks"));
  ds.fields.push_back(make_field("journal_start", base_offset, 0x34, 4, p, std::to_string(sb.journal_start), "uint32_le", "Metadata journal start LBA"));
  ds.fields.push_back(make_field("journal_blocks", base_offset, 0x38, 4, p, std::to_string(sb.journal_blocks), "uint32_le", "Metadata journal block count"));
  ds.fields.push_back(make_field("total_inodes", base_offset, 0x3C, 4, p, std::to_string(sb.total_inodes), "uint32_le", "Total inode slots"));
  ds.fields.push_back(make_field("free_inodes", base_offset, 0x40, 4, p, std::to_string(sb.free_inodes), "uint32_le", "Remaining free inodes"));
  ds.fields.push_back(make_field("free_blocks", base_offset, 0x44, 4, p, std::to_string(sb.free_blocks), "uint32_le", "Remaining free data blocks"));
  ds.fields.push_back(make_field("root_inode", base_offset, 0x48, 4, p, std::to_string(sb.root_inode), "uint32_le", "Root directory inode ID"));
  ds.fields.push_back(make_field("alloc_policy", base_offset, 0x4C, 1, p, alloc_policy_to_string(static_cast<AllocPolicy>(sb.alloc_policy)), "uint8", "Active block allocation policy"));
  ds.fields.push_back(make_field("fs_state", base_offset, 0x4D, 1, p, std::to_string(sb.fs_state), "uint8", "1=Clean, 2=Dirty, 4=Corrupted"));
  ds.fields.push_back(make_field("mount_count", base_offset, 0x4E, 2, p, std::to_string(sb.mount_count), "uint16_le", "Write-mount counter"));
  ds.fields.push_back(make_field("last_mount_ms", base_offset, 0x50, 8, p, std::to_string(sb.last_mount_ms), "uint64_le", "Last mount timestamp (epoch ms)"));
  ds.fields.push_back(make_field("last_write_ms", base_offset, 0x58, 8, p, std::to_string(sb.last_write_ms), "uint64_le", "Last write timestamp (epoch ms)"));
  ds.fields.push_back(make_field("volume_label", base_offset, 0x60, 20, p, sb.volume_label, "char[20]", "Null-terminated volume label"));
  ds.fields.push_back(make_field("next_alloc_cursor", base_offset, 0x74, 4, p, std::to_string(sb.next_alloc_cursor), "uint32_le", "Persistent Bitmap Next Fit LBA cursor"));
  ds.fields.push_back(make_field("next_journal_seq", base_offset, 0x78, 4, p, std::to_string(sb.next_journal_seq), "uint32_le", "Next journal sequence number"));

  auto f_crc = make_field("crc32", base_offset, 0x7C, 4, p, u32_hex(sb.crc32), "uint32_le", "IEEE 802.3 CRC32 over bytes [0x00..0x7B]");
  f_crc.has_validation = true;
  f_crc.valid = crc_ok;
  f_crc.validation_note = crc_ok ? "Matches computed CRC32" : "CRC32 mismatch";
  ds.fields.push_back(std::move(f_crc));

  return ds;
}

DecodedStructure BinaryStructureParser::decode_inode_table_block(
    const std::vector<uint8_t>& block_bytes,
    uint32_t block_id,
    uint64_t base_offset,
    uint32_t first_inode_id) {
  DecodedStructure ds{};
  ds.structure_type = "inode_table";
  ds.block_id = block_id;
  ds.base_offset = base_offset;

  const uint32_t slots = static_cast<uint32_t>(block_bytes.size() / kInodeBytes);
  for (uint32_t s = 0; s < slots; ++s) {
    const uint32_t slot_rel = s * static_cast<uint32_t>(kInodeBytes);
    const uint8_t* p = block_bytes.data() + slot_rel;
    bool all_zero = true;
    for (size_t i = 0; i < kInodeBytes; ++i) {
      if (p[i] != 0) {
        all_zero = false;
        break;
      }
    }
    if (all_zero) continue;

    Inode ino{};
    bool crc_ok = false;
    deserialize_inode(p, ino, crc_ok);
    if (!crc_ok) {
      ds.checksum_present = true;
      ds.checksum_valid = false;
      ds.warnings.push_back("Inode slot #" + std::to_string(first_inode_id + s) + " CRC32 mismatch.");
    } else {
      ds.checksum_present = true;
    }

    const std::string prefix = "inode[" + std::to_string(ino.inode_id) + "].";
    ds.fields.push_back(make_field(prefix + "inode_id", base_offset, slot_rel + 0x00, 4, block_bytes.data(), std::to_string(ino.inode_id), "uint32_le", "Inode identifier"));
    ds.fields.push_back(make_field(prefix + "type_state", base_offset, slot_rel + 0x04, 2, block_bytes.data(),
                                   "type=" + std::to_string(ino.type) + ", state=" + std::to_string(ino.state),
                                   "uint8[2]", "type (1=File,2=Dir), state (0=Free,1=Alloc,2=Deleted)"));
    ds.fields.push_back(make_field(prefix + "size_bytes", base_offset, slot_rel + 0x10, 8, block_bytes.data(), std::to_string(ino.size_bytes), "uint64_le", "Logical file size"));
    ds.fields.push_back(make_field(prefix + "direct_blocks[0..11]", base_offset, slot_rel + 0x38, 48, block_bytes.data(),
                                   std::to_string(ino.direct_blocks[0]) + ", " + std::to_string(ino.direct_blocks[1]) + ", ...",
                                   "uint32_le[12]", "Direct data block LBA pointers"));
    ds.fields.push_back(make_field(prefix + "indirect_block", base_offset, slot_rel + 0x68, 4, block_bytes.data(), std::to_string(ino.indirect_block), "uint32_le", "Single-indirect pointer LBA"));
    ds.fields.push_back(make_field(prefix + "double_indirect_block", base_offset, slot_rel + 0x6C, 4, block_bytes.data(), std::to_string(ino.double_indirect_block), "uint32_le", "Double-indirect pointer LBA"));
    ds.fields.push_back(make_field(prefix + "content_crc32", base_offset, slot_rel + 0x70, 4, block_bytes.data(), u32_hex(ino.content_crc32), "uint32_le", "Payload CRC32"));
    ds.fields.push_back(make_field(prefix + "generation", base_offset, slot_rel + 0x74, 4, block_bytes.data(), std::to_string(ino.generation), "uint32_le", "Inode reuse generation"));

    auto f_crc = make_field(prefix + "inode_crc32", base_offset, slot_rel + 0x7C, 4, block_bytes.data(), u32_hex(ino.inode_crc32), "uint32_le", "Inode metadata CRC32");
    f_crc.has_validation = true;
    f_crc.valid = crc_ok;
    ds.fields.push_back(std::move(f_crc));
  }
  return ds;
}

DecodedStructure BinaryStructureParser::decode_dir_block(
    const std::vector<uint8_t>& block_bytes,
    uint32_t block_id,
    uint64_t base_offset) {
  DecodedStructure ds{};
  ds.structure_type = "directory_data";
  ds.block_id = block_id;
  ds.base_offset = base_offset;

  const uint32_t slots = static_cast<uint32_t>(block_bytes.size() / kDirEntryBytes);
  for (uint32_t s = 0; s < slots; ++s) {
    const uint32_t rel = s * static_cast<uint32_t>(kDirEntryBytes);
    DirEntry de{};
    const bool ok = deserialize_dir_entry(block_bytes.data() + rel, de);
    if (de.state == 0 && de.inode_id == 0) continue;
    const std::string state_str = de.state == 1 ? "Active" : (de.state == 2 ? "Deleted" : "Empty");
    auto f = make_field(
        "entry[" + std::to_string(s) + "]",
        base_offset,
        rel,
        static_cast<uint32_t>(kDirEntryBytes),
        block_bytes.data(),
        "inode=" + std::to_string(de.inode_id) + ", state=" + state_str + ", name=\"" + de.name + "\"",
        "GfsDirEntry (64B)",
        "Directory entry slot");
    f.has_validation = true;
    f.valid = ok;
    ds.fields.push_back(std::move(f));
  }
  return ds;
}

DecodedStructure BinaryStructureParser::decode_indirect_block(
    const std::vector<uint8_t>& block_bytes,
    uint32_t block_id,
    uint64_t base_offset,
    uint32_t data_start,
    uint32_t data_end) {
  DecodedStructure ds{};
  ds.structure_type = "indirect_pointer";
  ds.block_id = block_id;
  ds.base_offset = base_offset;

  const uint32_t ptrs = static_cast<uint32_t>(block_bytes.size() / 4U);
  for (uint32_t i = 0; i < ptrs; ++i) {
    const uint32_t ptr = endian::read_u32_le(block_bytes.data() + i * 4U);
    if (ptr == 0U) continue;
    auto f = make_field(
        "ptr[" + std::to_string(i) + "]",
        base_offset,
        i * 4U,
        4U,
        block_bytes.data(),
        std::to_string(ptr),
        "uint32_le",
        "Indirect block pointer LBA");
    f.has_validation = true;
    f.valid = (ptr >= data_start && ptr < data_end);
    if (!f.valid) {
      f.validation_note = "Pointer outside data region";
      ds.warnings.push_back("Out-of-bounds pointer at slot " + std::to_string(i));
    }
    ds.fields.push_back(std::move(f));
  }
  return ds;
}

DecodedStructure BinaryStructureParser::decode_journal_block(
    const std::vector<uint8_t>& block_bytes,
    uint32_t block_id,
    uint64_t base_offset) {
  DecodedStructure ds{};
  ds.structure_type = "journal";
  ds.block_id = block_id;
  ds.base_offset = base_offset;

  const uint32_t slots = static_cast<uint32_t>(block_bytes.size() / kJournalEntryBytes);
  for (uint32_t s = 0; s < slots; ++s) {
    const uint32_t rel = s * static_cast<uint32_t>(kJournalEntryBytes);
    JournalEntry je{};
    bool crc_ok = false;
    deserialize_journal_entry(block_bytes.data() + rel, je, crc_ok);
    if (je.seq_no == 0 && je.op_type == 0) continue;
    ds.checksum_present = true;
    if (!crc_ok) ds.checksum_valid = false;
    auto f = make_field(
        "tx[" + std::to_string(je.seq_no) + "]",
        base_offset,
        rel,
        static_cast<uint32_t>(kJournalEntryBytes),
        block_bytes.data(),
        "status=" + std::to_string(je.status) + ", inode=" + std::to_string(je.target_inode) + ", \"" + je.summary + "\"",
        "GfsJournalEntry (64B)",
        "Metadata journal transaction");
    f.has_validation = true;
    f.valid = crc_ok;
    ds.fields.push_back(std::move(f));
  }
  return ds;
}

DecodedStructure BinaryStructureParser::decode_bitmap_block(
    const std::vector<uint8_t>& block_bytes,
    uint32_t block_id,
    uint64_t base_offset,
    const std::string& bitmap_kind,
    uint32_t total_items) {
  DecodedStructure ds{};
  ds.structure_type = bitmap_kind;
  ds.block_id = block_id;
  ds.base_offset = base_offset;

  uint32_t set_count = 0;
  for (uint8_t b : block_bytes) {
    for (int bit = 0; bit < 8; ++bit) {
      if ((b & (1U << bit)) != 0U) ++set_count;
    }
  }
  ds.fields.push_back(make_field(
      "bitmap_bytes",
      base_offset,
      0U,
      static_cast<uint32_t>(std::min<size_t>(block_bytes.size(), 32U)),
      block_bytes.data(),
      std::to_string(set_count) + " bits set in block (total volume items=" + std::to_string(total_items) + ")",
      "bitmap",
      bitmap_kind + " allocation bitfield"));
  return ds;
}

}  // namespace ghostfs
