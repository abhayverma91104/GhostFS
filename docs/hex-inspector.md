# GhostFS Phase 5 — Hex Viewer & Forensic Binary Inspection

This document covers the architecture, binary decoding logic, search and entropy algorithms, comparison methodology, API contracts, and limitations of the Phase 5 Hex Viewer & Forensic Analysis subsystem.

---

## 1. Read-Only Binary Inspection Architecture

All Phase 5 inspection operations are strictly **read-only**:
- **C++17 Engine** ([`hex_inspector.hpp`](file:///Users/abhay/Documents/projects/GhostFS/engine/include/ghostfs/hex_inspector.hpp), [`hex_inspector.cpp`](file:///Users/abhay/Documents/projects/GhostFS/engine/src/hexviewer/hex_inspector.cpp), [`binary_parser.hpp`](file:///Users/abhay/Documents/projects/GhostFS/engine/include/ghostfs/binary_parser.hpp), [`binary_parser.cpp`](file:///Users/abhay/Documents/projects/GhostFS/engine/src/hexviewer/binary_parser.cpp)):
  - Disk images (`.gfs`) are opened via `VirtualDisk::open(path, true)` (`read_only = true`, POSIX `O_RDONLY` / `std::ios::binary | std::ios::in`).
  - Recovered artifacts are read from `recovered_files/<artifact_filename>` with strict path-confinement validation (rejecting `..`, `/`, `\`, and non-regular files).
  - No write, truncate, or bitmap/superblock mutation is ever performed during inspection, decoding, search, entropy analysis, or comparison.
- **Immutability Guarantee**:
  - Comparing SHA-256 hashes of `.gfs` disk images before and after any sequence of `hex_read`, `hex_interpret`, `hex_decode_structure`, `hex_search`, `hex_analyze`, and `hex_compare` operations guarantees byte-for-byte immutability.

---

## 2. Supported Inspection Sources & Pagination

| Source Type | Description | Bounds & Clamping |
| :--- | :--- | :--- |
| `block` | Reads a single physical block (`block_id * block_size`, length `block_size`). | Rejects `block_id >= total_blocks`. |
| `disk_range` | Reads an arbitrary byte range `[offset, offset + length)` from the `.gfs` image. | Rejects `offset >= image_size`; clamps `length` to `min(length, 65536, image_size - offset)`. |
| `file` | Reads logical file contents in order by traversing direct, single-indirect, and double-indirect pointers, or reads raw physical blocks. | Rejects non-regular files or out-of-range offsets. |
| `artifact` | Reads a workspace-confined extracted recovery artifact from `recovered_files/`. | Rejects path traversal and reads within `[0, artifact_size)`. |

Each `hex_read` response returns:
- 16-byte aligned `HexRow` entries (`offset`, `offset_hex`, `hex_bytes`, `ascii`, `byte_count`).
- `signature`: automatic magic-byte header detection at offset 0 of the read window (`GFS1` superblock, `GFSB` boot block, `GFSJ` journal, `PNG`, `JPEG`, `PDF`, `ELF`, `ZIP`, `GZIP`, `RIFF`, `UTF-8 Text`, or `Unknown`).

---

## 3. GFS1 Binary Structure Decoding & Endianness

`BinaryParser` decodes on-disk GFS1 structures using explicit little-endian readers (`read_u16_le`, `read_u32_le`, `read_u64_le`) and reports exact byte offsets, field sizes, raw hex bytes, interpreted values, and IEEE CRC32 status:

| Structure | Target | Key Decoded Fields |
| :--- | :--- | :--- |
| `boot` | Block 0 | `magic` (`GFSB`), `version`, `block_size`, `total_blocks`, `boot_crc32` (verified). |
| `superblock` | Block 1 (Primary) / Block 2 (Backup) | `magic` (`GFS1`), `version`, `block_size`, `total_blocks`, `inode_count`, `free_blocks`, `free_inodes`, `alloc_policy`, `journal_start`, `journal_blocks`, `inode_bitmap_start`, `block_bitmap_start`, `inode_table_start`, `data_start`, `volume_label`, `superblock_crc32` (verified). |
| `journal` | `journal_start` block | `magic` (`GFSJ`), `sequence`, `state` (`0 = Clean`, `1 = Dirty`), `op_type`, `target_inode`, `target_block`, `journal_crc32` (verified). |
| `inode` | Inode table slot `inode_id` | `inode_id`, `type` (`1 = File`, `2 = Dir`), `state` (`0 = Free`, `1 = Allocated`, `2 = Deleted`), `generation`, `size_bytes`, `ctime`, `mtime`, `dtime`, `content_crc32`, `direct[12]`, `indirect_block`, `double_indirect_block`, `inode_crc32` (verified). |
| `dirent` | Directory data block | Decodes each 64-byte `DirEntry` slot: `inode_id`, `entry_type`, `state` (`1 = Active`, `2 = Deleted`), `name_len`, `name`, `entry_crc32` (verified). |
| `bitmap` | `inode_bitmap_start` or `block_bitmap_start` | Bitwise summary (`set_bits`, `clear_bits`, `total_bits`) plus per-byte hex/bit breakdown. |
| `indirect` | Single or double-indirect block | Array of 32-bit little-endian block pointers (`uint32_t`), highlighting non-zero entries. |

### Endianness & Multi-Byte Data Interpretation

Selecting 1–8 bytes in the Hex Grid (or passing raw bytes to `hex_interpret`) computes:
- **Little-Endian (LE)** and **Big-Endian (BE)** unsigned and signed integers (`uint8`/`int8`, `uint16`/`int16`, `uint32`/`int32`, `uint64`/`int64`).
- **IEEE 754** `float32` (4 bytes) and `float64` (8 bytes) in both LE and BE.
- **UNIX Timestamp** (UTC ISO-8601 from 32-bit or 64-bit integer when within plausible epoch range).
- **UTF-8 / ASCII** printable preview and raw hex string.

---

## 4. Search Algorithm & Complexity

`hex_search` supports three query modes over either the entire `.gfs` disk image or a specific byte range:
1. **Hex Byte Pattern** (e.g. `89 50 4E 47` or `89504E47`, with optional `??` single-byte wildcards).
2. **ASCII / UTF-8 Text** (case-sensitive or case-insensitive).
3. **32-bit / 64-bit Integer** (`u32_le`, `u32_be`, `u64_le`, `u64_be`).

- **Implementation**: Streaming chunked scan reading `64 KiB` windows with `(pattern_len - 1)` overlap across chunk boundaries so matches spanning block or chunk boundaries are never missed.
- **Complexity**:
  - Time: $O(N \cdot M)$ worst-case ($O(N)$ typical) where $N$ is the scanned byte length and $M$ is the pattern length.
  - Memory: $O(W + K)$ where $W = 64\text{ KiB}$ buffer size and $K$ is the capped match list (default max 200 matches).
- Each match returns `offset`, `offset_hex`, `block_id`, `block_offset`, and a surrounding `context_hex` snippet.

---

## 5. Shannon Entropy & Byte Distribution Analysis

`hex_analyze` computes statistical byte metrics over a block, disk byte range, file, or extracted artifact:
- **256-bin Byte Histogram**: Frequency $c_i$ for each byte value $i \in [0, 255]$.
- **Shannon Entropy ($H$)**:
  $$H = -\sum_{i=0}^{255} p_i \log_2(p_i), \quad p_i = \frac{c_i}{N} \quad (p_i > 0)$$
  Bounded in $[0.0, 8.0]$ bits/byte:
  - $H = 0.0$: Uniform / zero-filled block.
  - $0.0 < H < 3.5$: Low entropy (sparse structures, repetitive headers).
  - $3.5 \le H < 6.5$: Medium entropy (ASCII/UTF-8 text, structured metadata).
  - $H \ge 6.5$: High entropy (compressed payloads such as PNG/ZIP/GZIP, encrypted data, or pseudorandom bytes).
- **Additional Metrics**: `null_byte_ratio`, `printable_ascii_ratio`, `distinct_bytes`, and `classification`.

---

## 6. Binary Comparison Methodology

`hex_compare` compares any two byte sources (e.g., Block 1 Primary Superblock vs. Block 2 Backup Superblock, two disk offsets, or an original file vs. an extracted recovery artifact):
- Computes **SHA-256** and **CRC32** for both Source A and Source B.
- Performs byte-by-byte diffing up to a configurable limit (default 256 diff entries), returning `offset`, `byte_a`, `byte_b`, and XOR delta.
- When comparing superblocks (or when `compare_mode = "superblock"`), also produces a structured **Superblock Field Diff** table comparing every GFS1 `Superblock` field semantically.

---

## 7. FastAPI Endpoints (Phase 5)

All endpoints are mounted under `/api/hex`:
- `GET /api/hex/{disk_name}/block/{block_id}`
- `GET /api/hex/{disk_name}/range` (`offset`, `length`)
- `GET /api/hex/{disk_name}/file` (`path`, `offset`, `length`)
- `GET /api/hex/artifact/{filename}` (`offset`, `length`)
- `GET /api/hex/{disk_name}/decode` (`structure_type`, `target_id`)
- `POST /api/hex/interpret` (`hex_bytes`)
- `POST /api/hex/{disk_name}/search` (`query`, `mode`, `case_sensitive`, `start_offset`, `end_offset`, `max_results`)
- `POST /api/hex/{disk_name}/analyze` (`source_type`, `block_id`, `offset`, `length`, `path`, `artifact_filename`)
- `POST /api/hex/{disk_name}/compare` (Source A vs. Source B, optional `compare_mode`)

---

## 8. Forensic Limitations

1. **Entropy is not proof of encryption**: High entropy ($H \ge 6.5$) cannot distinguish compressed media (PNG `IDAT`, ZIP, GZIP) from encrypted payloads or random noise without structural/header context.
2. **Magic-byte signatures are heuristic**: A matching header (e.g., `89 50 4E 47`) at offset 0 of a block indicates a plausible signature, not verified file integrity.
3. **Directory entry & inode decoding reflect current on-disk bytes**: Deleted entries may have partially overwritten fields if blocks were reused after deletion.
4. **Pagination & diff caps**: `disk_range` reads are capped at 64 KiB per request and `hex_compare` byte diffs are capped (default 256 entries) to keep UI and API latency bounded.
