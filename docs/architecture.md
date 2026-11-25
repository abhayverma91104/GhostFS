# GhostFS — System Architecture & GFS1 Binary Specification

> **Document Scope:** Approved Phase 0 system architecture, exact GFS1 binary on-disk layout, byte offsets, endianness, CRC32 integrity rules, metadata journal crash-recovery guarantees, and workspace safety boundaries.

---

## 1. High-Level System Architecture

GhostFS uses a strict three-tier architecture where **all file system logic, block allocation, forensic recovery, corruption analysis, hex extraction, and disk scheduling run in C++17**. Python/FastAPI acts solely as a validated orchestration, upload/download, and PDF/CSV report formatting layer.

```
React + TypeScript + Vite  <--REST/JSON & Multipart-->  FastAPI (Python 3)  <--execve + JSON stdin/stdout-->  C++17 Engine (ghostfs_engine)  <--pread/pwrite/fsync + flock-->  .gfs Virtual Disks
```

### Key Architectural Invariants
1. **Strict Workspace Confinement:** All `.gfs` files must reside inside `<workspace>/disk_images/` (and disposable corruption copies inside `<workspace>/disk_images/sandbox/`). Both FastAPI and the C++ engine canonicalize paths and reject `..`, symlinks escaping the root, and device paths (`/dev/*`).
2. **Engine Execution Boundary:** FastAPI invokes `ghostfs_engine` via `asyncio.create_subprocess_exec` (never `shell=True`), passing a JSON request via `stdin` and reading structured JSON from `stdout`.
3. **Concurrency & File Locking:** Every C++ command opens `<image>.gfs.lock` using POSIX `flock()` (`LOCK_SH` for read-only inspections; `LOCK_EX` for formatting, writes, deletes, and repairs) so concurrent API requests cannot interleave partial metadata updates.
4. **Non-Destructive Forensics:** Deleting a file clears its inode/block bitmap bits and marks its inode and directory entry as `Deleted` without zeroing data blocks.

---

## 2. GFS1 Binary Layout Specification

### 2.1 Endianness & Supported Block Geometries
- **Endianness:** All multi-byte integers (`uint16_t`, `uint32_t`, `uint64_t`) are serialized explicitly in **little-endian** byte order using portable shift-and-mask codecs (`read_le*` / `write_le*`), ensuring identical binary `.gfs` images across macOS/Linux ARM64 and x86-64 hosts.
- **Supported Block Sizes:** `512`, `1024`, `2048`, `4096` bytes (`4096` bytes default).
- **Disk Size Bounds:** Minimum `64` blocks (minimum practical GFS1 volume: e.g. `256 KB` at 4096B or `64 KB` at 1024B; UI default in `MB` / `KB`), maximum `512 MB` (`536,870,912` bytes) for safe lab execution.
- **Checksum Algorithm:** IEEE 802.3 CRC32 (polynomial `0xEDB88320`, initial `0xFFFFFFFF`, final XOR `0xFFFFFFFF`). Every CRC32 field covers all preceding bytes of its fixed-size header/record.

### 2.2 Contiguous LBA Region Layout

| Region | Block Range | Size Calculation | Purpose |
| :--- | :--- | :--- | :--- |
| **0. Boot / Reserved** | Block `0` | `1` block | `"GHOSTFS\0"` signature, format version, block size, total size, CRC32 |
| **1. Primary Superblock** | Block `1` | `1` block | Authoritative volume geometry, counts, allocation policy, flags, CRC32 |
| **2. Backup Superblock** | Block `2` | `1` block | Mirrored superblock updated on structural syncs for `fsck` repair |
| **3. Inode Bitmap** | `[3 .. 3 + inode_bitmap_blocks - 1]` | `ceil(total_inodes / (block_size * 8))` | 1 bit per inode (`1` = currently allocated, `0` = free or deleted) |
| **4. Block Bitmap** | `[block_bitmap_start .. +block_bitmap_blocks - 1]` | `ceil(total_blocks / (block_size * 8))` | 1 bit per block (`1` = reserved or allocated, `0` = free/unallocated) |
| **5. Inode Table** | `[inode_table_start .. +inode_table_blocks - 1]` | `ceil((total_inodes * 128) / block_size)` | Array of 128-byte `GfsInode` records (`0` reserved null, `1` = root `/`) |
| **6. Data Region** | `[data_region_start .. journal_start - 1]` | `total_blocks - data_region_start - journal_blocks` | File data blocks, directory entry blocks, single/double indirect pointer blocks |
| **7. Metadata Journal** | `[journal_start .. total_blocks - 1]` | `journal_blocks` (default `16`, or `4` on tiny test disks) | Write-ahead metadata transaction ring (`GfsJournalEntry` records) |

> **Inode Count Sizing:** By default, `total_inodes` is sized proportionally to disk blocks (e.g., `max(32, min(4096, total_blocks / 4))`), aligned to fill whole inode-table blocks (`inodes_per_block = block_size / 128`), and may also be configured explicitly at format time for testing inode exhaustion.

---

### 2.3 Exact Byte-Level Binary Structures

#### A. `GfsBootBlock` (Block `0`, `64` bytes used, zero-padded to `block_size`)

| Byte Offset | Size (B) | Type | Field | Description / Rule |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | `8` | `char[8]` | `signature` | `"GHOSTFS\0"` (`47 48 4F 53 54 46 53 00`) |
| `0x08` | `4` | `uint32_t` | `version` | `1` |
| `0x0C` | `4` | `uint32_t` | `block_size` | `512 \| 1024 \| 2048 \| 4096` |
| `0x10` | `8` | `uint64_t` | `total_size_bytes` | `total_blocks * block_size` |
| `0x18` | `8` | `uint64_t` | `created_at_ms` | Unix epoch milliseconds |
| `0x20` | `4` | `uint32_t` | `primary_sb_block` | `1` |
| `0x24` | `4` | `uint32_t` | `backup_sb_block` | `2` |
| `0x28` | `20` | `uint8_t[20]` | `reserved` | Zero-filled |
| `0x3C` | `4` | `uint32_t` | `crc32` | IEEE CRC32 over bytes `[0x00 .. 0x3B]` (`60` bytes) |

Total = `64` bytes.

#### B. `GfsSuperblock` (Blocks `1` & `2`, `128` bytes used, zero-padded to `block_size`)

| Byte Offset | Size (B) | Type | Field | Description / Rule |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | `4` | `uint32_t` | `magic` | `0x47465331` (`"GFS1"` in big-endian value, `31 53 46 47` LE) |
| `0x04` | `4` | `uint32_t` | `version` | `1` |
| `0x08` | `4` | `uint32_t` | `block_size` | `512 \| 1024 \| 2048 \| 4096` |
| `0x0C` | `4` | `uint32_t` | `total_blocks` | Total LBA blocks in image |
| `0x10` | `4` | `uint32_t` | `reserved_blocks` | `3` (Boot + Primary SB + Backup SB) |
| `0x14` | `4` | `uint32_t` | `inode_bitmap_start` | First block of inode bitmap (`3`) |
| `0x18` | `4` | `uint32_t` | `inode_bitmap_blocks` | Block count of inode bitmap |
| `0x1C` | `4` | `uint32_t` | `block_bitmap_start` | First block of block bitmap |
| `0x20` | `4` | `uint32_t` | `block_bitmap_blocks` | Block count of block bitmap |
| `0x24` | `4` | `uint32_t` | `inode_table_start` | First block of inode table |
| `0x28` | `4` | `uint32_t` | `inode_table_blocks` | Block count of inode table |
| `0x2C` | `4` | `uint32_t` | `data_region_start` | First allocatable data block |
| `0x30` | `4` | `uint32_t` | `data_blocks_count` | Number of data blocks |
| `0x34` | `4` | `uint32_t` | `journal_start` | First block of metadata journal |
| `0x38` | `4` | `uint32_t` | `journal_blocks` | Block count of metadata journal |
| `0x3C` | `4` | `uint32_t` | `total_inodes` | Total inode slots (including reserved slot `0`) |
| `0x40` | `4` | `uint32_t` | `free_inodes` | Remaining free inodes |
| `0x44` | `4` | `uint32_t` | `free_blocks` | Remaining free data blocks |
| `0x48` | `4` | `uint32_t` | `root_inode` | `1` |
| `0x4C` | `1` | `uint8_t` | `alloc_policy` | `0=FirstFit`, `1=BestFit`, `2=WorstFit`, `3=BitmapNextFit` |
| `0x4D` | `1` | `uint8_t` | `fs_state` | `1=Clean`, `2=Dirty`, `4=Corrupted` |
| `0x4E` | `2` | `uint16_t` | `mount_count` | Incremented on open/write sessions |
| `0x50` | `8` | `uint64_t` | `last_mount_ms` | Unix epoch ms |
| `0x58` | `8` | `uint64_t` | `last_write_ms` | Unix epoch ms |
| `0x60` | `20` | `char[20]` | `volume_label` | Null-terminated UTF-8 volume label (max 19 chars) |
| `0x74` | `4` | `uint32_t` | `next_alloc_cursor` | Persistent LBA cursor for Bitmap Next Fit |
| `0x78` | `4` | `uint32_t` | `next_journal_seq` | Monotonic journal sequence number |
| `0x7C` | `4` | `uint32_t` | `crc32` | IEEE CRC32 over bytes `[0x00 .. 0x7B]` (`124` bytes) |

Total = `128` bytes.

#### C. `GfsInode` (Inode Table entries, `128` bytes each)

| Byte Offset | Size (B) | Type | Field | Description / Rule |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | `4` | `uint32_t` | `inode_id` | Slot index (`0` invalid, `1` root `/`) |
| `0x04` | `1` | `uint8_t` | `type` | `0=Unused`, `1=File`, `2=Directory` |
| `0x05` | `1` | `uint8_t` | `state` | `0=Free`, `1=Allocated`, `2=Deleted` |
| `0x06` | `2` | `uint16_t` | `permissions` | POSIX permission bits (e.g. `0644`, `0755`) |
| `0x08` | `4` | `uint32_t` | `uid` | Owner user ID |
| `0x0C` | `4` | `uint32_t` | `gid` | Owner group ID |
| `0x10` | `8` | `uint64_t` | `size_bytes` | Logical file size in bytes (or directory entry byte span) |
| `0x18` | `8` | `uint64_t` | `created_at_ms` | Creation timestamp (epoch ms) |
| `0x20` | `8` | `uint64_t` | `modified_at_ms` | Modification timestamp (epoch ms) |
| `0x28` | `8` | `uint64_t` | `deleted_at_ms` | Deletion timestamp (`0` while active) |
| `0x30` | `4` | `uint32_t` | `link_count` | Directory/file link count |
| `0x34` | `4` | `uint32_t` | `parent_inode` | Parent directory inode ID (`1` for root's children) |
| `0x38` | `48` | `uint32_t[12]` | `direct_blocks` | 12 direct LBA block addresses (`0` = unassigned) |
| `0x68` | `4` | `uint32_t` | `indirect_block` | LBA of single indirect pointer block (`block_size / 4` `uint32_t` entries) |
| `0x6C` | `4` | `uint32_t` | `double_indirect_block` | LBA of double indirect pointer block |
| `0x70` | `4` | `uint32_t` | `content_crc32` | IEEE CRC32 of file payload bytes (`0` for empty file/dir) |
| `0x74` | `4` | `uint32_t` | `generation` | Incremented every time an inode slot is newly allocated |
| `0x78` | `4` | `uint32_t` | `flags` | Bit `0`: fragmented hint; reserved bits `1..31` |
| `0x7C` | `4` | `uint32_t` | `inode_crc32` | IEEE CRC32 over bytes `[0x00 .. 0x7B]` (`124` bytes) |

Total = `128` bytes (`4` inodes per 512B block, `32` inodes per 4096B block).

#### D. `GfsDirEntry` (Inside Directory Data Blocks, `64` bytes each)

| Byte Offset | Size (B) | Type | Field | Description / Rule |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | `4` | `uint32_t` | `inode_id` | Target inode ID (retained when deleted until slot reuse) |
| `0x04` | `1` | `uint8_t` | `entry_type` | `1=File`, `2=Directory` |
| `0x05` | `1` | `uint8_t` | `state` | `0=Empty`, `1=Active`, `2=Deleted` |
| `0x06` | `2` | `uint16_t` | `name_len` | UTF-8 name byte length (`1..55`) |
| `0x08` | `56` | `char[56]` | `name` | Null-terminated filename (retained on delete until slot reuse) |

Total = `64` bytes (`8` entries per 512B block, `64` entries per 4096B block).

#### E. `GfsJournalEntry` (Inside Metadata Journal Region, `64` bytes each)

| Byte Offset | Size (B) | Type | Field | Description / Rule |
| :--- | :--- | :--- | :--- | :--- |
| `0x00` | `8` | `uint64_t` | `seq_no` | Monotonic transaction sequence number |
| `0x08` | `8` | `uint64_t` | `timestamp_ms` | Transaction timestamp (epoch ms) |
| `0x10` | `1` | `uint8_t` | `op_type` | `1=Format`, `2=Mkdir`, `3=CreateWrite`, `4=Delete`, `5=Rename`, `6=Rmdir`, `7=Repair` |
| `0x11` | `1` | `uint8_t` | `status` | `1=Pending` (intent logged), `2=Committed`, `3=RolledBack/Aborted` |
| `0x12` | `2` | `uint16_t` | `reserved` | Zero |
| `0x14` | `4` | `uint32_t` | `target_inode` | Primary inode affected |
| `0x18` | `4` | `uint32_t` | `target_block` | Primary block affected (or `0`) |
| `0x1C` | `4` | `uint32_t` | `aux_value` | Secondary block/size/count context |
| `0x20` | `28` | `char[28]` | `summary` | Null-terminated short operation summary |
| `0x3C` | `4` | `uint32_t` | `crc32` | IEEE CRC32 over bytes `[0x00 .. 0x3B]` (`60` bytes) |

Total = `64` bytes.

---

## 3. Metadata Journal & Crash-Recovery Guarantees

### Protocol
Every mutating filesystem operation follows an ordered write-ahead protocol under an exclusive `flock(LOCK_EX)`:
1. **Mark Dirty & Log Intent (`Pending`):** Write a `GfsJournalEntry` with `status = Pending` (`1`) and valid `crc32` into the circular journal ring (`seq_no % journal_capacity`), set `superblock.fs_state = Dirty (2)` if transitioning from `Clean`, and `fsync()`.
2. **Apply Payload & Metadata:**
   - Write new file data blocks / indirect pointer blocks first (never touching existing active data before pointers are updated).
   - Update Inode Table, Directory Entry blocks, Inode Bitmap, and Block Bitmap.
3. **Commit Transaction:**
   - Write updated Primary Superblock (and Backup Superblock on structural changes) with `fs_state = Clean (1)` and updated `crc32`.
   - Write `GfsJournalEntry` with `status = Committed (2)` and `fsync()`.

### Guarantees & Documented Limitations (Crash Detection vs. Crash Recovery)
- **Crash Detection (Phase 2/3):**
  - Every metadata operation leaves a verifiable audit trail in the circular journal (`Pending` vs `Committed`) and marks `fs_state = Dirty` whenever an operation is interrupted mid-flight before step 3 completes.
  - Because data blocks are written *before* inode/directory pointers are published, a crash mid-write never attaches uninitialized garbage blocks to a committed file.
  - On mount/open, if `fs_state == Dirty` or any valid journal entry remains in `Pending` state, the engine sets `dirty_journal_detected = true` and **blocks all normal filesystem mutations (`ERR_DIRTY_FILESYSTEM`)** while still permitting safe read-only inspection (`status`, `list_directory`, `read_file`, `fragmentation_report`, `blocks_map`).
- **Crash Recovery (Phase 6 Scope):**
  - The journal records lightweight `64`-byte metadata operation intents rather than full physical block undo/redo before-images. Detecting a dirty journal is **crash detection**, not automatic crash recovery; repairing dirty/inconsistent metadata is handled by `fsck` / repair (Phase 6) or explicit reformatting.

---

## 5. Phase 3: Block Allocation, Fragmentation & Comparison

### 5.1 File-System Block Allocation Algorithms
Unlike fixed-partition RAM allocation (which assigns one contiguous memory partition per process), GFS1 allocates **fixed-size `block_size` blocks** from the contiguous data-region bitmap `[data_region_start .. journal_start - 1]`. Contiguous free runs (`FreeExtent{start_block, length}`) are discovered by scanning the block bitmap:

1. **First Fit (`first_fit`):** Selects the first free extent in ascending LBA order with `length >= needed`. Tie-breaker: lowest `start_block`.
2. **Best Fit (`best_fit`):** Scans all free extents and selects the smallest extent with `length >= needed`. Deterministic tie-breaker: lowest `start_block`.
3. **Worst Fit (`worst_fit`):** Scans all free extents and selects the largest extent with `length >= needed`. Deterministic tie-breaker: lowest `start_block`.
4. **Bitmap Next Fit (`bitmap_next_fit`):** Scans free extents starting from the superblock-persisted `next_alloc_cursor` (wrapping around `[data_region_start .. cursor)` if needed), and advances `next_alloc_cursor` past the allocated blocks.

**Fragmented Allocation Fallback:** When `total_free_blocks >= needed` but no single contiguous free extent has `length >= needed`, GFS1 falls back to multi-extent allocation rather than failing:
- `best_fit` / `worst_fit` sort free extents by `length` descending (lowest `start_block` tie-break) to minimize total extent count.
- `first_fit` collects blocks in ascending LBA order; `bitmap_next_fit` collects blocks circularly from `next_alloc_cursor`.
- Data payload blocks and indirect pointer blocks are requested via `BlockAllocator::allocate_blocks`, and pre-flight capacity checks guarantee zero leaked blocks or inodes on `ERR_NO_SPACE`.

### 5.2 Fragmentation Formulas
- **External Free-Space Fragmentation (%):**
  `external_fragmentation_pct = (free_blocks == 0) ? 0.0 : (1.0 - largest_free_extent / free_blocks) * 100.0`
- **Internal Fragmentation (Bytes & %):**
  Measured strictly across allocated **file-data payload blocks** (excluding directories, bitmaps, inode tables, and indirect pointer blocks):
  `internal_fragmentation_bytes = sum(file.allocated_data_blocks * block_size - file.size_bytes)`
  `internal_fragmentation_pct = (internal_fragmentation_bytes / sum(file.allocated_data_blocks * block_size)) * 100.0`
- **File Fragmentation:** A file is fragmented when its physical data block list has `extent_count > 1` (consecutive LBA breaks).
- **Complexity:** Bitmap extent scan is `O(N)` time and `O(E)` space (`N = data_blocks_count`, `E <= N/2` free extents).

### 5.3 Block Classification Rules
Blocks `[0 .. total_blocks - 1]` are classified deterministically into: `reserved` (0), `primary_superblock` (1), `backup_superblock` (2), `inode_bitmap`, `block_bitmap`, `inode_table`, `journal`, `directory_data`, `file_data`, `indirect_pointer`, `free`, `free_deleted_residual` (free in bitmap and explicitly referenced by a surviving `Deleted` inode), and `inconsistent` (bitmap/inode mismatch).

### 5.4 Phase 3 REST API Endpoints
- `PATCH /api/v1/disks/{name}/policy` — updates `alloc_policy` in superblocks without relocating existing files.
- `GET /api/v1/disks/{name}/blocks/map?offset=0&limit=256&type_filter=&file_inode=0` — paginated block map.
- `GET /api/v1/disks/{name}/blocks/{block_id}` — single block metadata & byte offset.
- `GET /api/v1/disks/{name}/blocks/extents` — contiguous free extents & external fragmentation.
- `GET /api/v1/disks/{name}/fragmentation` — full fragmentation & per-file mapping report.
- `POST /api/v1/disks/{name}/alloc-compare` — runs identical workload across all 4 policies on isolated sandbox copies.

---

## 6. Phase 4: Forensic Recovery Engine
See [forensic-recovery.md](forensic-recovery.md) for the complete specification of metadata-based recovery, signature-based carving (PNG/JPEG/PDF/TXT), evidence classification, stale-scan SHA-256 validation, JSON evidence manifests, and demo scenarios A–H.

> **Sanitizer Verification Note:** ASan/UBSan runtime execution timed out at dyld/atos startup on the macOS host during Phase 4; successful Linux ASan+UBSan execution is included in the final Phase 9 project acceptance checklist.

---

## 7. Phase 5: Hex Viewer & Forensic Binary Inspection
See [hex-inspector.md](hex-inspector.md) for the complete specification of read-only binary inspection (`block`, `disk_range`, `file`, `artifact`), GFS1 structure decoding (`boot`, `superblock`, `journal`, `inode`, `dirent`, `bitmap`, `indirect`), LE/BE multi-byte data interpretation, chunked pattern/text/integer search, Shannon entropy & 256-bin byte histogram analysis, binary comparison mode, and Phase 5 REST API endpoints (`/api/v1/disks/{disk_name}/hex/*`).

---

## 8. Phase 6: Corruption & Repair Laboratory
See [corruption-and-repair.md](corruption-and-repair.md) for the complete specification of the sandbox isolation model, controlled corruption scenarios (A–J), independent `gfsck` consistency checker, repair-plan generator, GFS1 repair engine, journal recovery guarantees/limitations, before-and-after comparison, `/api/v1/corruption/*` endpoints, and Playwright E2E test suite.

---

## 9. Phase 7: Disk Scheduling & I/O Analysis
See [disk-scheduling.md](disk-scheduling.md) for the complete specification of the C++17 educational disk scheduling simulator (`FCFS`, `SSTF`, `SCAN`, `C-SCAN`, `LOOK`, `C-LOOK`), mathematical formulas, GFS1 block-to-cylinder workload integration, 4-policy allocation + 6-algorithm scheduling comparison, `/api/v1/scheduling/*` endpoints, and textbook regression baseline.

---

## 10. Phase 8: Forensic Reports & Final Integration
See [reporting.md](reporting.md) for the complete specification of the centralized reporting subsystem, immutable evidence snapshots, ReportLab PDF generation, JSON/CSV exports, artifact integrity manifests, `/api/v1/reports/*` endpoints, reproducible complete investigation demo, and Phase 9 acceptance checklist.

---

## 4. Forensic Deletion Semantics (Phase 2 -> Phase 4 Bridge)

When deleting a file in GFS1:
1. All `direct_blocks`, `indirect_block`, `double_indirect_block`, and pointed data blocks have their bits cleared (`0`) in the Block Bitmap, and `superblock.free_blocks` increases accordingly.
2. **Data blocks are NOT zeroed or overwritten**; payload bytes remain intact on disk until reallocated by future writes.
3. The file's `GfsInode` sets `state = Deleted (2)` and `deleted_at_ms = now`, while retaining `size_bytes`, `parent_inode`, `direct_blocks`, `indirect_block`, `double_indirect_block`, `content_crc32`, and `generation` (updated `inode_crc32`). Its bit in the Inode Bitmap is cleared (`0`).
4. The parent directory's `GfsDirEntry` sets `state = Deleted (2)` while retaining `name`, `name_len`, `entry_type`, and `inode_id` until a subsequent directory creation reuses that slot.
5. When a previously free/deleted inode slot is reused, `generation` is incremented (`generation + 1`) and `deleted_at_ms` is reset to `0`, allowing future recovery analysis (Phase 4) to detect inode reuse and distinguish intact, partially overwritten, and fully overwritten files without any hidden backups.
