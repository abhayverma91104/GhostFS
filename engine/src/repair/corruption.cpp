#include "ghostfs/corruption.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "ghostfs/crc32.hpp"
#include "ghostfs/endian.hpp"
#include "ghostfs/engine.hpp"
#include "ghostfs/gfs_format.hpp"
#include "ghostfs/gfs_fs.hpp"
#include "ghostfs/recovery.hpp"
#include "ghostfs/virtual_disk.hpp"

namespace ghostfs {
using endian::write_u32_le;
namespace {

std::string utc_now_iso() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &t);
#else
  gmtime_r(&t, &tm_utc);
#endif
  std::ostringstream oss;
  oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%SZ");
  return oss.str();
}

void validate_identifier(const std::string& id, const std::string& label) {
  if (id.empty() || id.size() > 64) {
    throw std::runtime_error("ERR_INVALID_ARGUMENT: Invalid " + label);
  }
  for (unsigned char c : id) {
    if (!std::isalnum(c) && c != '_' && c != '-') {
      throw std::runtime_error("ERR_INVALID_ARGUMENT: Invalid character in " + label);
    }
  }
}

void reject_if_symlink(const std::filesystem::path& p) {
  std::error_code ec;
  if (std::filesystem::is_symlink(p, ec)) {
    throw std::runtime_error("ERR_SYMLINK_REJECTED: Symlinks are not permitted: " + p.string());
  }
}

std::string bytes_to_hex(const std::vector<uint8_t>& bytes, size_t max_bytes = 32) {
  std::ostringstream oss;
  oss << std::hex << std::uppercase << std::setfill('0');
  const size_t n = std::min(bytes.size(), max_bytes);
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) oss << ' ';
    oss << std::setw(2) << static_cast<unsigned>(bytes[i]);
  }
  if (bytes.size() > max_bytes) {
    oss << " ...";
  }
  return oss.str();
}

std::filesystem::path exp_json_path(
    const std::filesystem::path& sb_dir,
    const std::string& exp_id) {
  validate_identifier(exp_id, "experiment_id");
  return resolve_workspace_path(sb_dir, exp_id + ".exp.json");
}

nlohmann::json record_to_json(const CorruptionRecord& r) {
  return {
      {"operation_id", r.operation_id},
      {"category", r.category},
      {"scenario", r.scenario},
      {"description", r.description},
      {"original_disk_sha256", r.original_disk_sha256},
      {"before_sandbox_sha256", r.before_sandbox_sha256},
      {"after_sandbox_sha256", r.after_sandbox_sha256},
      {"affected_offsets", r.affected_offsets},
      {"affected_blocks", r.affected_blocks},
      {"original_bytes_hex", r.original_bytes_hex},
      {"replacement_bytes_hex", r.replacement_bytes_hex},
      {"timestamp_utc", r.timestamp_utc},
      {"seed", r.seed},
      {"used_seed", r.used_seed},
  };
}

CorruptionRecord json_to_record(const nlohmann::json& j) {
  CorruptionRecord r{};
  r.operation_id = j.value("operation_id", "");
  r.category = j.value("category", "");
  r.scenario = j.value("scenario", "");
  r.description = j.value("description", "");
  r.original_disk_sha256 = j.value("original_disk_sha256", "");
  r.before_sandbox_sha256 = j.value("before_sandbox_sha256", "");
  r.after_sandbox_sha256 = j.value("after_sandbox_sha256", "");
  r.affected_offsets = j.value("affected_offsets", std::vector<uint64_t>{});
  r.affected_blocks = j.value("affected_blocks", std::vector<uint32_t>{});
  r.original_bytes_hex = j.value("original_bytes_hex", "");
  r.replacement_bytes_hex = j.value("replacement_bytes_hex", "");
  r.timestamp_utc = j.value("timestamp_utc", "");
  r.seed = j.value("seed", 0ULL);
  r.used_seed = j.value("used_seed", false);
  return r;
}

nlohmann::json exp_to_json(const SandboxExperiment& e) {
  nlohmann::json ops = nlohmann::json::array();
  for (const auto& op : e.operations) {
    ops.push_back(record_to_json(op));
  }
  return {
      {"experiment_id", e.experiment_id},
      {"source_disk_name", e.source_disk_name},
      {"sandbox_filename", e.sandbox_filename},
      {"original_sha256", e.original_sha256},
      {"initial_sandbox_sha256", e.initial_sandbox_sha256},
      {"current_sandbox_sha256", e.current_sandbox_sha256},
      {"pre_repair_snapshot_filename", e.pre_repair_snapshot_filename},
      {"pre_repair_sha256", e.pre_repair_sha256},
      {"created_at_utc", e.created_at_utc},
      {"updated_at_utc", e.updated_at_utc},
      {"operations", ops},
  };
}

SandboxExperiment json_to_exp(const nlohmann::json& j) {
  SandboxExperiment e{};
  e.experiment_id = j.at("experiment_id").get<std::string>();
  e.source_disk_name = j.at("source_disk_name").get<std::string>();
  e.sandbox_filename = j.at("sandbox_filename").get<std::string>();
  e.original_sha256 = j.at("original_sha256").get<std::string>();
  e.initial_sandbox_sha256 = j.at("initial_sandbox_sha256").get<std::string>();
  e.current_sandbox_sha256 = j.value("current_sandbox_sha256", e.initial_sandbox_sha256);
  e.pre_repair_snapshot_filename = j.value("pre_repair_snapshot_filename", "");
  e.pre_repair_sha256 = j.value("pre_repair_sha256", "");
  e.created_at_utc = j.value("created_at_utc", "");
  e.updated_at_utc = j.value("updated_at_utc", "");
  if (j.contains("operations") && j["operations"].is_array()) {
    for (const auto& op : j["operations"]) {
      e.operations.push_back(json_to_record(op));
    }
  }
  return e;
}

std::string sanitize_gfs_filename(const std::string& raw, const std::string& fallback) {
  std::string out;
  for (unsigned char c : raw) {
    if (std::isalnum(c) || c == '_' || c == '-') {
      out.push_back(static_cast<char>(c));
    }
  }
  if (out.empty()) {
    out = fallback;
  }
  if (out.size() > 48) {
    out.resize(48);
  }
  if (out.size() < 4 || out.substr(out.size() - 4) != ".gfs") {
    out += ".gfs";
  }
  return out;
}

void write_inode_slot(
    VirtualDisk& disk,
    const Superblock& sb,
    const Inode& inode) {
  const uint32_t bs = sb.block_size;
  const uint32_t per_blk = bs / static_cast<uint32_t>(kInodeBytes);
  const uint32_t blk = sb.inode_table_start + (inode.inode_id / per_blk);
  const uint32_t off = (inode.inode_id % per_blk) * static_cast<uint32_t>(kInodeBytes);
  std::vector<uint8_t> buf;
  disk.read_block(blk, buf);
  serialize_inode(inode, buf.data() + off);
  disk.write_block(blk, buf);
}

Inode read_inode_slot(
    const VirtualDisk& disk,
    const Superblock& sb,
    uint32_t inode_id,
    bool& crc_ok) {
  const uint32_t bs = sb.block_size;
  const uint32_t per_blk = bs / static_cast<uint32_t>(kInodeBytes);
  const uint32_t blk = sb.inode_table_start + (inode_id / per_blk);
  const uint32_t off = (inode_id % per_blk) * static_cast<uint32_t>(kInodeBytes);
  std::vector<uint8_t> buf;
  disk.read_block(blk, buf);
  Inode out{};
  deserialize_inode(buf.data() + off, out, crc_ok);
  return out;
}

void set_bitmap_bit(
    VirtualDisk& disk,
    uint32_t bitmap_start,
    uint32_t bit_index,
    bool value) {
  const uint32_t bs = disk.block_size();
  const uint32_t bits_per_blk = bs * 8U;
  const uint32_t blk = bitmap_start + (bit_index / bits_per_blk);
  const uint32_t in_blk = bit_index % bits_per_blk;
  const uint32_t byte_idx = in_blk / 8U;
  const uint8_t mask = static_cast<uint8_t>(1U << (in_blk % 8U));
  std::vector<uint8_t> buf;
  disk.read_block(blk, buf);
  if (value) {
    buf[byte_idx] |= mask;
  } else {
    buf[byte_idx] &= static_cast<uint8_t>(~mask);
  }
  disk.write_block(blk, buf);
}

bool get_bitmap_bit(
    const VirtualDisk& disk,
    uint32_t bitmap_start,
    uint32_t bit_index) {
  const uint32_t bs = disk.block_size();
  const uint32_t bits_per_blk = bs * 8U;
  const uint32_t blk = bitmap_start + (bit_index / bits_per_blk);
  const uint32_t in_blk = bit_index % bits_per_blk;
  std::vector<uint8_t> buf;
  disk.read_block(blk, buf);
  return (buf[in_blk / 8U] & static_cast<uint8_t>(1U << (in_blk % 8U))) != 0;
}

}  // namespace

std::filesystem::path CorruptionEngine::sandbox_dir(const std::filesystem::path& workspace) {
  const std::filesystem::path dir = workspace / "disk_images" / "sandbox";
  std::filesystem::create_directories(dir);
  return dir;
}

void CorruptionEngine::save_experiment(
    const std::filesystem::path& workspace,
    const SandboxExperiment& exp) {
  const auto sb_dir = sandbox_dir(workspace);
  const auto meta_path = exp_json_path(sb_dir, exp.experiment_id);
  reject_if_symlink(meta_path);
  std::ofstream ofs(meta_path, std::ios::trunc);
  if (!ofs) {
    throw std::runtime_error("Failed to save experiment metadata: " + meta_path.string());
  }
  ofs << exp_to_json(exp).dump(2) << '\n';
}

SandboxExperiment CorruptionEngine::create_experiment(
    const std::filesystem::path& workspace,
    const std::string& source_disk_name,
    const std::string& sandbox_name) {
  const std::filesystem::path disks_dir = workspace / "disk_images";
  reject_if_symlink(disks_dir / source_disk_name);
  const std::filesystem::path src_path = resolve_workspace_path(disks_dir, source_disk_name);
  if (!std::filesystem::exists(src_path) || !std::filesystem::is_regular_file(src_path)) {
    throw std::runtime_error("ERR_NOT_FOUND: Source disk image not found: " + source_disk_name);
  }
  reject_if_symlink(src_path);

  VirtualDisk src_disk;
  src_disk.open_image(src_path, 512U, false);
  const std::string original_sha = compute_file_sha256_streaming(src_path);

  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  const std::string exp_id = "exp_" + original_sha.substr(0, 8) + "_" + std::to_string(ms);
  const std::string sb_filename = sanitize_gfs_filename(
      sandbox_name.empty() ? (exp_id + "_" + src_path.stem().string()) : (exp_id + "_" + sandbox_name),
      exp_id);

  const auto sb_dir = sandbox_dir(workspace);
  const std::filesystem::path sb_path = resolve_workspace_path(sb_dir, sb_filename);
  reject_if_symlink(sb_path);
  if (std::filesystem::exists(sb_path)) {
    throw std::runtime_error("ERR_ALREADY_EXISTS: Sandbox disk already exists");
  }

  std::filesystem::copy_file(src_path, sb_path, std::filesystem::copy_options::none);
  src_disk.close_image();

  const std::string initial_sha = compute_file_sha256_streaming(sb_path);
  if (initial_sha != original_sha) {
    std::error_code ec;
    std::filesystem::remove(sb_path, ec);
    throw std::runtime_error("ERR_CLONE_MISMATCH: Sandbox copy SHA-256 did not match source disk");
  }

  SandboxExperiment exp{};
  exp.experiment_id = exp_id;
  exp.source_disk_name = src_path.filename().string();
  exp.sandbox_filename = sb_filename;
  exp.original_sha256 = original_sha;
  exp.initial_sandbox_sha256 = initial_sha;
  exp.current_sandbox_sha256 = initial_sha;
  exp.created_at_utc = utc_now_iso();
  exp.updated_at_utc = exp.created_at_utc;
  save_experiment(workspace, exp);
  return exp;
}

std::vector<SandboxExperiment> CorruptionEngine::list_experiments(
    const std::filesystem::path& workspace) {
  const auto sb_dir = sandbox_dir(workspace);
  std::vector<SandboxExperiment> out;
  for (const auto& entry : std::filesystem::directory_iterator(sb_dir)) {
    if (!entry.is_regular_file()) continue;
    const std::string fname = entry.path().filename().string();
    if (fname.size() > 9 && fname.substr(fname.size() - 9) == ".exp.json") {
      try {
        std::ifstream ifs(entry.path());
        if (!ifs) continue;
        nlohmann::json j = nlohmann::json::parse(ifs);
        SandboxExperiment exp = json_to_exp(j);
        const auto sb_path = resolve_workspace_path(sb_dir, exp.sandbox_filename);
        if (std::filesystem::exists(sb_path)) {
          exp.current_sandbox_sha256 = compute_file_sha256_streaming(sb_path);
        }
        out.push_back(std::move(exp));
      } catch (...) {
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const SandboxExperiment& a, const SandboxExperiment& b) {
    return a.created_at_utc > b.created_at_utc;
  });
  return out;
}

SandboxExperiment CorruptionEngine::get_experiment(
    const std::filesystem::path& workspace,
    const std::string& experiment_id) {
  const auto sb_dir = sandbox_dir(workspace);
  const auto meta_path = exp_json_path(sb_dir, experiment_id);
  if (!std::filesystem::exists(meta_path)) {
    throw std::runtime_error("ERR_NOT_FOUND: Experiment not found: " + experiment_id);
  }
  reject_if_symlink(meta_path);
  std::ifstream ifs(meta_path);
  nlohmann::json j = nlohmann::json::parse(ifs);
  SandboxExperiment exp = json_to_exp(j);
  const auto sb_path = resolve_workspace_path(sb_dir, exp.sandbox_filename);
  if (std::filesystem::exists(sb_path)) {
    exp.current_sandbox_sha256 = compute_file_sha256_streaming(sb_path);
  }
  return exp;
}

void CorruptionEngine::delete_experiment(
    const std::filesystem::path& workspace,
    const std::string& experiment_id,
    bool confirm) {
  if (!confirm) {
    throw std::runtime_error("ERR_CONFIRMATION_REQUIRED: Deleting an experiment requires confirmation");
  }
  const auto sb_dir = sandbox_dir(workspace);
  const SandboxExperiment exp = get_experiment(workspace, experiment_id);
  std::error_code ec;
  const auto sb_path = resolve_workspace_path(sb_dir, exp.sandbox_filename);
  std::filesystem::remove(sb_path, ec);
  std::filesystem::remove(sb_path.string() + ".lock", ec);
  if (!exp.pre_repair_snapshot_filename.empty()) {
    const auto snap_path = resolve_workspace_path(sb_dir, exp.pre_repair_snapshot_filename);
    std::filesystem::remove(snap_path, ec);
  }
  std::filesystem::remove(exp_json_path(sb_dir, experiment_id), ec);
}

SandboxExperiment CorruptionEngine::inject_corruption(
    const std::filesystem::path& workspace,
    const std::string& experiment_id,
    const CorruptionInjectRequest& req) {
  if (!req.confirm) {
    throw std::runtime_error(
        "ERR_CONFIRMATION_REQUIRED: Explicit confirmation is required before injecting corruption");
  }
  SandboxExperiment exp = get_experiment(workspace, experiment_id);
  const auto sb_dir = sandbox_dir(workspace);
  const std::filesystem::path sb_path = resolve_workspace_path(sb_dir, exp.sandbox_filename);
  reject_if_symlink(sb_path);
  if (!std::filesystem::exists(sb_path)) {
    throw std::runtime_error("ERR_NOT_FOUND: Sandbox image missing for experiment " + experiment_id);
  }

  const std::string before_sha = compute_file_sha256_streaming(sb_path);
  std::mt19937_64 rng(req.seed);

  CorruptionRecord rec{};
  rec.operation_id = "op_" + std::to_string(exp.operations.size() + 1) + "_" +
                     std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch())
                                        .count());
  rec.category = req.category;
  rec.scenario = req.scenario;
  rec.original_disk_sha256 = exp.original_sha256;
  rec.before_sandbox_sha256 = before_sha;
  rec.timestamp_utc = utc_now_iso();
  rec.seed = req.seed;

  if (req.category == "advanced" && req.scenario == "truncate_image") {
    const uint64_t sz = std::filesystem::file_size(sb_path);
    const uint64_t cut = std::min<uint64_t>(
        sz / 2ULL,
        std::max<uint64_t>(512ULL, static_cast<uint64_t>(req.severity) * 4096ULL));
    if (sz <= cut) {
      throw std::runtime_error("ERR_INVALID_ARGUMENT: Sandbox image too small to truncate");
    }
    std::filesystem::resize_file(sb_path, sz - cut);
    rec.description = "Truncated sandbox disk image by " + std::to_string(cut) + " bytes";
    rec.affected_offsets.push_back(sz - cut);
  } else {
    VirtualDisk disk;
    disk.open_image(sb_path, 512U, true);
    std::vector<uint8_t> raw_boot;
    disk.read_raw(0, kBootBlockBytes, raw_boot);
    BootBlock boot{};
    bool boot_crc = false;
    deserialize_boot_block(raw_boot.data(), boot, boot_crc);
    const uint32_t bs = is_supported_block_size(boot.block_size) ? boot.block_size : kDefaultBlockSize;
    disk.set_block_size(bs);

    std::vector<uint8_t> sb1_buf;
    disk.read_block(1, sb1_buf);
    Superblock sb{};
    bool sb1_crc = false;
    if (!deserialize_superblock(sb1_buf.data(), sb, sb1_crc)) {
      std::vector<uint8_t> sb2_buf;
      disk.read_block(2, sb2_buf);
      bool sb2_crc = false;
      if (!deserialize_superblock(sb2_buf.data(), sb, sb2_crc)) {
        throw std::runtime_error("ERR_CORRUPT_GEOMETRY: Neither superblock has valid GFS1 magic");
      }
    }

    auto record_block_mutation = [&](uint32_t blk, const std::vector<uint8_t>& orig, const std::vector<uint8_t>& mod) {
      rec.affected_blocks.push_back(blk);
      for (size_t i = 0; i < std::min(orig.size(), mod.size()); ++i) {
        if (orig[i] != mod[i]) {
          rec.affected_offsets.push_back(static_cast<uint64_t>(blk) * bs + i);
          if (rec.affected_offsets.size() >= 16) break;
        }
      }
      if (rec.original_bytes_hex.empty()) {
        rec.original_bytes_hex = bytes_to_hex(orig, 16);
        rec.replacement_bytes_hex = bytes_to_hex(mod, 16);
      }
    };

    if (req.category == "primary_superblock") {
      std::vector<uint8_t> buf;
      disk.read_block(1, buf);
      std::vector<uint8_t> orig(buf.begin(), buf.begin() + 128);
      if (req.scenario == "corrupt_magic") {
        write_u32_le(buf.data(), 0x42414431U);
        rec.description = "Corrupted primary superblock magic signature (GFS1 -> BAD1)";
      } else if (req.scenario == "corrupt_geometry") {
        write_u32_le(buf.data() + 12, sb.total_blocks + 9999U);
        rec.description = "Corrupted primary superblock total_blocks geometry field";
      } else {
        buf[124] ^= 0xFFU;
        buf[125] ^= 0xA5U;
        rec.description = "Introduced primary superblock CRC32 checksum mismatch";
      }
      disk.write_block(1, buf);
      record_block_mutation(1, orig, std::vector<uint8_t>(buf.begin(), buf.begin() + 128));
    } else if (req.category == "backup_superblock") {
      std::vector<uint8_t> buf;
      disk.read_block(2, buf);
      std::vector<uint8_t> orig(buf.begin(), buf.begin() + 128);
      if (req.scenario == "corrupt_magic") {
        write_u32_le(buf.data(), 0x42414432U);
        rec.description = "Corrupted backup superblock magic signature";
      } else if (req.scenario == "inconsistent_replica") {
        Superblock mod_sb = sb;
        mod_sb.total_blocks = sb.total_blocks + 128U;
        serialize_superblock(mod_sb, buf.data());
        rec.description = "Created inconsistent backup superblock replica geometry";
      } else {
        buf[124] ^= 0xFFU;
        rec.description = "Introduced backup superblock CRC32 checksum mismatch";
      }
      disk.write_block(2, buf);
      record_block_mutation(2, orig, std::vector<uint8_t>(buf.begin(), buf.begin() + 128));
    } else if (req.category == "dual_superblock") {
      std::vector<uint8_t> b1, b2;
      disk.read_block(1, b1);
      disk.read_block(2, b2);
      std::vector<uint8_t> orig1(b1.begin(), b1.begin() + 16);
      write_u32_le(b1.data(), 0x42414431U);
      write_u32_le(b2.data(), 0x42414432U);
      if (req.scenario == "corrupt_geometry") {
        write_u32_le(b1.data() + 8, 99999U);
        write_u32_le(b2.data() + 8, 88888U);
      }
      disk.write_block(1, b1);
      disk.write_block(2, b2);
      record_block_mutation(1, orig1, std::vector<uint8_t>(b1.begin(), b1.begin() + 16));
      rec.affected_blocks.push_back(2);
      rec.description = "Corrupted both primary and backup superblocks";
    } else if (req.category == "block_bitmap") {
      uint32_t alloc_blk = 0;
      uint32_t free_blk = 0;
      for (uint32_t b = sb.data_region_start; b < sb.journal_start; ++b) {
        const bool used = get_bitmap_bit(disk, sb.block_bitmap_start, b);
        if (used && alloc_blk == 0 && b > sb.data_region_start) alloc_blk = b;
        if (!used && free_blk == 0) free_blk = b;
        if (alloc_blk != 0 && free_blk != 0) break;
      }
      if (req.target_block >= sb.data_region_start && req.target_block < sb.journal_start) {
        if (req.scenario == "mark_free_as_allocated") free_blk = req.target_block;
        else alloc_blk = req.target_block;
      }
      if (req.scenario == "mark_allocated_as_free" || req.scenario == "both") {
        if (alloc_blk == 0) throw std::runtime_error("ERR_NO_TARGET: No allocated data block to mark free");
        set_bitmap_bit(disk, sb.block_bitmap_start, alloc_blk, false);
        rec.affected_blocks.push_back(sb.block_bitmap_start);
        rec.affected_offsets.push_back(static_cast<uint64_t>(sb.block_bitmap_start) * bs + (alloc_blk / 8U));
      }
      if (req.scenario == "mark_free_as_allocated" || req.scenario == "both") {
        if (free_blk == 0) throw std::runtime_error("ERR_NO_TARGET: No free data block to mark allocated");
        set_bitmap_bit(disk, sb.block_bitmap_start, free_blk, true);
        rec.affected_blocks.push_back(sb.block_bitmap_start);
        rec.affected_offsets.push_back(static_cast<uint64_t>(sb.block_bitmap_start) * bs + (free_blk / 8U));
      }
      rec.description = "Injected block bitmap inconsistency (" + req.scenario + ")";
    } else if (req.category == "inode_bitmap") {
      uint32_t target_ino = req.target_inode;
      if (target_ino == 0) {
        for (uint32_t i = 2; i < sb.total_inodes; ++i) {
          const bool used = get_bitmap_bit(disk, sb.inode_bitmap_start, i);
          if ((req.scenario == "mark_active_as_free" && used) ||
              (req.scenario == "mark_unused_as_active" && !used)) {
            target_ino = i;
            break;
          }
        }
      }
      if (target_ino == 0 || target_ino >= sb.total_inodes) {
        throw std::runtime_error("ERR_NO_TARGET: No suitable inode found for inode bitmap corruption");
      }
      const bool new_val = (req.scenario == "mark_unused_as_active");
      set_bitmap_bit(disk, sb.inode_bitmap_start, target_ino, new_val);
      rec.affected_blocks.push_back(sb.inode_bitmap_start);
      rec.affected_offsets.push_back(static_cast<uint64_t>(sb.inode_bitmap_start) * bs + (target_ino / 8U));
      rec.description = "Injected inode bitmap mismatch on inode #" + std::to_string(target_ino);
    } else if (req.category == "inode") {
      uint32_t ino_id = req.target_inode;
      if (ino_id == 0) {
        for (uint32_t i = 2; i < sb.total_inodes; ++i) {
          bool ok = false;
          const Inode cand = read_inode_slot(disk, sb, i, ok);
          if (ok && cand.state == static_cast<uint8_t>(InodeState::Allocated) &&
              cand.type == static_cast<uint8_t>(InodeType::File)) {
            ino_id = i;
            break;
          }
        }
      }
      if (ino_id == 0 || ino_id >= sb.total_inodes) {
        throw std::runtime_error("ERR_NO_TARGET: No allocated file inode found");
      }
      bool ok = false;
      Inode ino = read_inode_slot(disk, sb, ino_id, ok);
      const uint32_t per_blk = bs / static_cast<uint32_t>(kInodeBytes);
      const uint32_t blk = sb.inode_table_start + (ino_id / per_blk);
      std::vector<uint8_t> orig_blk;
      disk.read_block(blk, orig_blk);

      if (req.scenario == "corrupt_checksum") {
        const uint32_t off = (ino_id % per_blk) * static_cast<uint32_t>(kInodeBytes);
        orig_blk[off + 124] ^= 0xFFU;
        disk.write_block(blk, orig_blk);
        rec.affected_blocks.push_back(blk);
        rec.affected_offsets.push_back(static_cast<uint64_t>(blk) * bs + off + 124);
        rec.description = "Corrupted inode #" + std::to_string(ino_id) + " CRC32 checksum";
      } else if (req.scenario == "out_of_range_pointer") {
        ino.direct_blocks[0] = sb.total_blocks + 500U;
        write_inode_slot(disk, sb, ino);
        rec.affected_blocks.push_back(blk);
        rec.description = "Injected out-of-range direct block pointer on inode #" + std::to_string(ino_id);
      } else if (req.scenario == "inconsistent_size") {
        ino.size_bytes = static_cast<uint64_t>(bs) * 200ULL;
        write_inode_slot(disk, sb, ino);
        rec.affected_blocks.push_back(blk);
        rec.description = "Injected inconsistent file size on inode #" + std::to_string(ino_id);
      } else if (req.scenario == "corrupt_indirect") {
        ino.indirect_block = sb.total_blocks + 42U;
        write_inode_slot(disk, sb, ino);
        rec.affected_blocks.push_back(blk);
        rec.description = "Injected invalid single-indirect block pointer on inode #" + std::to_string(ino_id);
      } else if (req.scenario == "duplicate_block_ownership") {
        uint32_t other_id = req.secondary_inode;
        uint32_t shared_blk = 0;
        for (uint32_t i = 2; i < sb.total_inodes; ++i) {
          if (i == ino_id) continue;
          bool ok2 = false;
          const Inode c2 = read_inode_slot(disk, sb, i, ok2);
          if (c2.state == static_cast<uint8_t>(InodeState::Allocated) &&
              c2.type == static_cast<uint8_t>(InodeType::File) && c2.direct_blocks[0] != 0) {
            if (other_id == 0) other_id = i;
            if (i == other_id) {
              shared_blk = c2.direct_blocks[0];
              break;
            }
          }
        }
        if (shared_blk == 0) {
          throw std::runtime_error("ERR_NO_TARGET: Need a second allocated file with data blocks for duplicate ownership");
        }
        ino.direct_blocks[0] = shared_blk;
        if (ino.size_bytes == 0) ino.size_bytes = bs;
        write_inode_slot(disk, sb, ino);
        rec.affected_blocks.push_back(blk);
        rec.affected_blocks.push_back(shared_blk);
        rec.description = "Injected duplicate block ownership (block #" + std::to_string(shared_blk) +
                          " claimed by inode #" + std::to_string(ino_id) + " and inode #" + std::to_string(other_id) + ")";
      } else {
        throw std::runtime_error("ERR_INVALID_ARGUMENT: Unknown inode scenario: " + req.scenario);
      }
    } else if (req.category == "directory") {
      bool ok = false;
      Inode root = read_inode_slot(disk, sb, kRootInodeId, ok);
      if (root.direct_blocks[0] == 0) {
        throw std::runtime_error("ERR_CORRUPT_GEOMETRY: Root directory has no data block");
      }
      const uint32_t dir_blk = root.direct_blocks[0];
      std::vector<uint8_t> buf;
      disk.read_block(dir_blk, buf);
      std::vector<uint8_t> orig = buf;
      const uint32_t slots = bs / static_cast<uint32_t>(kDirEntryBytes);

      if (req.scenario == "dangling_inode") {
        bool placed = false;
        for (uint32_t s = 0; s < slots; ++s) {
          DirEntry de{};
          deserialize_dir_entry(buf.data() + s * kDirEntryBytes, de);
          if (de.state != static_cast<uint8_t>(DirEntryState::Active)) {
            DirEntry bad{};
            bad.inode_id = sb.total_inodes + 17U;
            bad.entry_type = static_cast<uint8_t>(InodeType::File);
            bad.state = static_cast<uint8_t>(DirEntryState::Active);
            bad.name = "dangling_ghost.bin";
            bad.name_len = static_cast<uint16_t>(bad.name.size());
            serialize_dir_entry(bad, buf.data() + s * kDirEntryBytes);
            placed = true;
            break;
          }
        }
        if (!placed) throw std::runtime_error("ERR_NO_SPACE: No free root directory slot");
        disk.write_block(dir_blk, buf);
        record_block_mutation(dir_blk, orig, buf);
        rec.description = "Injected dangling directory entry in '/'";
      } else if (req.scenario == "duplicate_filename") {
        DirEntry first_active{};
        bool found = false;
        uint32_t empty_slot = UINT32_MAX;
        for (uint32_t s = 0; s < slots; ++s) {
          DirEntry de{};
          deserialize_dir_entry(buf.data() + s * kDirEntryBytes, de);
          if (!found && de.state == static_cast<uint8_t>(DirEntryState::Active) &&
              de.name != "." && de.name != "..") {
            first_active = de;
            found = true;
          } else if (empty_slot == UINT32_MAX && de.state != static_cast<uint8_t>(DirEntryState::Active)) {
            empty_slot = s;
          }
        }
        if (!found || empty_slot == UINT32_MAX) {
          throw std::runtime_error("ERR_NO_TARGET: Need an active entry and free slot in '/'");
        }
        serialize_dir_entry(first_active, buf.data() + empty_slot * kDirEntryBytes);
        disk.write_block(dir_blk, buf);
        record_block_mutation(dir_blk, orig, buf);
        rec.description = "Injected duplicate filename '" + first_active.name + "' in '/'";
      } else if (req.scenario == "directory_cycle") {
        uint32_t empty_slot = UINT32_MAX;
        for (uint32_t s = 0; s < slots; ++s) {
          DirEntry de{};
          deserialize_dir_entry(buf.data() + s * kDirEntryBytes, de);
          if (de.state != static_cast<uint8_t>(DirEntryState::Active)) {
            empty_slot = s;
            break;
          }
        }
        if (empty_slot == UINT32_MAX) throw std::runtime_error("ERR_NO_SPACE: No free directory slot");
        DirEntry cyc{};
        cyc.inode_id = kRootInodeId;
        cyc.entry_type = static_cast<uint8_t>(InodeType::Directory);
        cyc.state = static_cast<uint8_t>(DirEntryState::Active);
        cyc.name = "loop_to_root";
        cyc.name_len = static_cast<uint16_t>(cyc.name.size());
        serialize_dir_entry(cyc, buf.data() + empty_slot * kDirEntryBytes);
        disk.write_block(dir_blk, buf);
        record_block_mutation(dir_blk, orig, buf);
        rec.description = "Injected directory cycle ('/loop_to_root' -> inode #1)";
      } else if (req.scenario == "inconsistent_parent") {
        uint32_t target_ino = req.target_inode;
        if (target_ino == 0) {
          for (uint32_t i = 2; i < sb.total_inodes; ++i) {
            bool ok2 = false;
            const Inode c = read_inode_slot(disk, sb, i, ok2);
            if (ok2 && c.state == static_cast<uint8_t>(InodeState::Allocated)) {
              target_ino = i;
              break;
            }
          }
        }
        if (target_ino == 0) throw std::runtime_error("ERR_NO_TARGET: No allocated inode");
        bool ok2 = false;
        Inode c = read_inode_slot(disk, sb, target_ino, ok2);
        c.parent_inode = sb.total_inodes + 10U;
        write_inode_slot(disk, sb, c);
        rec.affected_blocks.push_back(sb.inode_table_start);
        rec.description = "Injected inconsistent parent_inode on inode #" + std::to_string(target_ino);
      } else {
        throw std::runtime_error("ERR_INVALID_ARGUMENT: Unknown directory corruption scenario: " + req.scenario);
      }
    } else if (req.category == "journal") {
      const uint32_t j_blk = sb.journal_start;
      std::vector<uint8_t> jbuf;
      disk.read_block(j_blk, jbuf);
      std::vector<uint8_t> orig = jbuf;
      JournalEntry je{};
      je.seq_no = sb.next_journal_seq;
      je.timestamp_ms = 1700000000000ULL;
      je.op_type = static_cast<uint8_t>(JournalOp::CreateWrite);
      je.status = static_cast<uint8_t>(JournalStatus::Pending);
      je.target_inode = (req.target_inode != 0) ? req.target_inode : 2U;
      je.target_block = (req.target_block != 0) ? req.target_block : sb.data_region_start;
      je.summary = "interrupted_tx";
      serialize_journal_entry(je, jbuf.data());
      if (req.scenario == "invalid_checksum") {
        jbuf[60] ^= 0xFFU;
        rec.description = "Injected invalid journal entry CRC32 checksum";
      } else {
        Superblock dirty_sb = sb;
        dirty_sb.fs_state = static_cast<uint8_t>(FsState::Dirty);
        std::vector<uint8_t> sb_buf(bs, 0U);
        serialize_superblock(dirty_sb, sb_buf.data());
        disk.write_block(1, sb_buf);
        disk.write_block(2, sb_buf);
        rec.affected_blocks.push_back(1);
        rec.affected_blocks.push_back(2);
        rec.description = "Simulated interrupted metadata transaction (Pending journal + Dirty state)";
      }
      disk.write_block(j_blk, jbuf);
      record_block_mutation(j_blk, orig, jbuf);
    } else if (req.category == "payload") {
      uint32_t ino_id = req.target_inode;
      Inode target_ino{};
      if (ino_id == 0) {
        for (uint32_t i = 2; i < sb.total_inodes; ++i) {
          bool ok = false;
          const Inode c = read_inode_slot(disk, sb, i, ok);
          if (ok && c.state == static_cast<uint8_t>(InodeState::Allocated) &&
              c.type == static_cast<uint8_t>(InodeType::File) &&
              c.size_bytes > 0 && c.direct_blocks[0] >= sb.data_region_start &&
              c.direct_blocks[0] < sb.journal_start) {
            ino_id = i;
            target_ino = c;
            break;
          }
        }
      } else {
        bool ok = false;
        target_ino = read_inode_slot(disk, sb, ino_id, ok);
      }
      if (ino_id == 0 || target_ino.direct_blocks[0] == 0) {
        throw std::runtime_error("ERR_NO_TARGET: No non-empty file found for payload corruption");
      }
      const uint32_t data_blk = target_ino.direct_blocks[0];
      std::vector<uint8_t> buf;
      disk.read_block(data_blk, buf);
      std::vector<uint8_t> orig = buf;
      const size_t flip_count = std::min<size_t>(
          static_cast<size_t>(std::max<uint32_t>(1U, req.severity)) * 4U,
          static_cast<size_t>(std::min<uint64_t>(target_ino.size_bytes, bs)));
      for (size_t k = 0; k < flip_count; ++k) {
        const size_t idx = static_cast<size_t>(rng() % std::min<uint64_t>(target_ino.size_bytes, bs));
        buf[idx] ^= static_cast<uint8_t>(0x5AU ^ (k & 0xFFU));
      }
      disk.write_block(data_blk, buf);
      record_block_mutation(data_blk, orig, buf);
      rec.used_seed = true;
      rec.description = "Corrupted " + std::to_string(flip_count) + " payload byte(s) in data block #" +
                        std::to_string(data_blk) + " (inode #" + std::to_string(ino_id) + ")";
    } else if (req.category == "advanced") {
      if (req.scenario == "orphan_inode") {
        bool ok = false;
        Inode root = read_inode_slot(disk, sb, kRootInodeId, ok);
        const uint32_t dir_blk = root.direct_blocks[0];
        std::vector<uint8_t> buf;
        disk.read_block(dir_blk, buf);
        std::vector<uint8_t> orig = buf;
        const uint32_t slots = bs / static_cast<uint32_t>(kDirEntryBytes);
        bool removed = false;
        uint32_t orphaned_id = 0;
        for (uint32_t s = 0; s < slots; ++s) {
          DirEntry de{};
          deserialize_dir_entry(buf.data() + s * kDirEntryBytes, de);
          if (de.state == static_cast<uint8_t>(DirEntryState::Active) &&
              de.entry_type == static_cast<uint8_t>(InodeType::File) &&
              (req.target_inode == 0 || de.inode_id == req.target_inode)) {
            orphaned_id = de.inode_id;
            std::fill(buf.begin() + s * kDirEntryBytes, buf.begin() + (s + 1) * kDirEntryBytes, 0U);
            removed = true;
            break;
          }
        }
        if (!removed) throw std::runtime_error("ERR_NO_TARGET: No active file entry in '/' to orphan");
        disk.write_block(dir_blk, buf);
        record_block_mutation(dir_blk, orig, buf);
        rec.description = "Orphaned active inode #" + std::to_string(orphaned_id) +
                          " by clearing its root directory entry without freeing inode/blocks";
      } else if (req.scenario == "multi_corruption") {
        std::vector<uint8_t> sb1;
        disk.read_block(1, sb1);
        std::vector<uint8_t> orig1(sb1.begin(), sb1.begin() + 16);
        write_u32_le(sb1.data(), 0x42414431U);
        disk.write_block(1, sb1);
        record_block_mutation(1, orig1, std::vector<uint8_t>(sb1.begin(), sb1.begin() + 16));
        for (uint32_t b = sb.data_region_start; b < sb.journal_start; ++b) {
          if (!get_bitmap_bit(disk, sb.block_bitmap_start, b)) {
            set_bitmap_bit(disk, sb.block_bitmap_start, b, true);
            rec.affected_blocks.push_back(sb.block_bitmap_start);
            break;
          }
        }
        rec.description = "Injected multiple simultaneous corruptions (Primary SB magic + Block Bitmap leak)";
      } else {
        throw std::runtime_error("ERR_INVALID_ARGUMENT: Unknown advanced scenario: " + req.scenario);
      }
    } else {
      throw std::runtime_error("ERR_INVALID_ARGUMENT: Unknown corruption category: " + req.category);
    }

    disk.sync();
    disk.close_image();
  }

  rec.after_sandbox_sha256 = compute_file_sha256_streaming(sb_path);
  exp.current_sandbox_sha256 = rec.after_sandbox_sha256;
  exp.updated_at_utc = utc_now_iso();
  exp.operations.push_back(std::move(rec));
  save_experiment(workspace, exp);
  return exp;
}

}  // namespace ghostfs
