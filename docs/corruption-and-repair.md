# GhostFS Phase 6 — Corruption & Repair Laboratory

This document specifies the sandbox safety model, controlled corruption engine, independent consistency checker (`gfsck`), issue classification, repair-plan generator, GFS1 repair engine, journal crash-recovery guarantees/limitations, before-and-after comparison, API contracts, and automated Playwright E2E testing.

---

## 1. Sandbox Safety Model

All destructive experiments execute **exclusively** on disposable sandbox copies inside `disk_images/sandbox/`:
1. Select an existing `.gfs` image in `disk_images/` (canonical path validated, symlinks rejected).
2. Acquire a shared read lock (`VirtualDisk::open_image(..., write_mode=false)`) and compute `original_sha256`.
3. Clone byte-for-byte into `disk_images/sandbox/<exp_id>_<sandbox_name>.gfs` and verify `initial_sandbox_sha256 == original_sha256`.
4. Inject corruption only on the sandbox copy under exclusive `flock`, recording every operation in `<exp_id>.exp.json`.
5. Original source disk images are never modified; `original_sha256` is preserved throughout all experiments.
6. Explicit `confirm=true` is required for corruption injection, repair execution, and experiment deletion.

---

## 2. Supported Corruption Scenarios (A–J)

| Category | Scenarios |
| :--- | :--- |
| **A. Primary Superblock** | `corrupt_magic` (`GFS1` -> `BAD1`), `corrupt_geometry` (`total_blocks`), `checksum_mismatch` |
| **B. Backup Superblock** | `corrupt_magic`, `inconsistent_replica`, `checksum_mismatch` |
| **C. Dual Superblock** | `corrupt_magic`, `corrupt_geometry` (tests graceful unrecoverable diagnosis) |
| **D. Block Bitmap** | `mark_allocated_as_free`, `mark_free_as_allocated`, `both` |
| **E. Inode Bitmap** | `mark_active_as_free`, `mark_unused_as_active` |
| **F. Inode Table** | `corrupt_checksum`, `out_of_range_pointer`, `duplicate_block_ownership`, `inconsistent_size`, `corrupt_indirect` |
| **G. Directory Entries** | `dangling_inode`, `duplicate_filename`, `directory_cycle`, `inconsistent_parent` |
| **H. Journal / Crash** | `pending_uncommitted` (Pending slot + `Dirty` state), `invalid_checksum` |
| **I. File Payload** | `bit_flip` (deterministic seeded payload byte corruption without touching `content_crc32`) |
| **J. Advanced** | `orphan_inode`, `multi_corruption` (Primary SB + Bitmap leak), `truncate_image` |

---

## 3. Independent Consistency Checker (`gfsck`)

`GfsckEngine::check(disk_path, mode)` opens the `.gfs` image **read-only** and never consults `.exp.json` corruption logs:
- **Geometry & Superblock Validation**: Verifies `BootBlock`, probes block sizes `[4096, 2048, 1024, 512]` if needed, validates Primary (Block 1) and Backup (Block 2) magic/CRC32/region boundaries, checks replica consistency, `alloc_policy`, `fs_state`, and image truncation (`file_size >= total_blocks * block_size`). If both superblocks are invalid, emits `DUAL_SUPERBLOCK_UNRECOVERABLE` (`critical`, `not_repairable`) without inventing geometry.
- **Inode & Block Ownership Map**: Scans all `total_inodes`, validates `inode_crc32`, traverses direct, single-indirect, and double-indirect pointers, constructs `block_owners[block_id]`, detects `OUT_OF_RANGE_BLOCK_PTR`, `INVALID_INDIRECT_PTR`, `INCONSISTENT_FILE_SIZE`, `DUPLICATE_BLOCK_OWNERSHIP`, and reconciles against `block_bitmap` and `inode_bitmap`. Deleted-file residual blocks (`bitmap == 0`) are treated as free space, not errors.
- **Directory Graph Validation (Cycle-Safe Iterative BFS)**: Traverses from root inode `#1` using `visited_dirs`, detecting `MISSING_ROOT_INODE`, `DANGLING_DIR_ENTRY`, `DUPLICATE_DIR_NAME`, `DIRECTORY_CYCLE`, `INCONSISTENT_PARENT_INODE`, and `ORPHANED_INODE`.
- **Journal Validation**: Checks 64-byte journal slot CRC32s and `Pending` / `Dirty` states.
- **Payload Integrity (`full` mode)**: Reconstructs regular file payloads in logical order, computes IEEE CRC32, and compares against `inode.content_crc32`.

---

## 4. Issue Classification & Repairability

- **Severities**: `information`, `warning`, `error`, `critical`.
- **Repairability**:
  - `automatically_repairable` (e.g., valid backup restores primary SB, bitmap bit matching unambiguous ownership, `parent_inode` fix).
  - `repairable_with_confirmation` (e.g., orphan reconnection to `/lost+found`, reclaiming unreferenced bitmap leaks, marking dangling/duplicate/cyclic directory slots `Deleted` to preserve forensic evidence, conservative journal reconciliation).
  - `ambiguous_manual_required` (e.g., `DUPLICATE_BLOCK_OWNERSHIP`, `INVALID_INODE_CHECKSUM`, `OUT_OF_RANGE_BLOCK_PTR` — never arbitrarily freed or overwritten).
  - `not_repairable` (e.g., `DUAL_SUPERBLOCK_UNRECOVERABLE`, `TRUNCATED_IMAGE`, `PAYLOAD_CRC_MISMATCH`).

---

## 5. Journal Recovery Guarantees & Limitations

- GFS1's 64-byte `JournalEntry` records **metadata operation intent** (`seq_no`, `op_type`, `status`, `target_inode`, `target_block`, `summary`, `crc32`), **not** full physical before/after block images.
- Therefore, Phase 6 implements **conservative consistency reconstruction** (marking `Pending`/corrupt journal slots `Aborted`, reconciling bitmaps/metadata from validated inodes, and setting `fs_state = Clean` only after all metadata checks pass), rather than claiming full WAL physical redo/undo replay.

---

## 6. Why Metadata Repair Does Not Recover Lost File Contents

Repairing superblocks, bitmaps, or directory links restores **structural reachability and allocation invariants**, not overwritten or bit-flipped data block payloads. When a file's data blocks suffer `PAYLOAD_CRC_MISMATCH`, `gfsck` preserves the surviving bytes, refuses to falsify `inode.content_crc32`, and reports the file as damaged even if metadata is `Clean`.

---

## 7. Repair Plan Ordering & Stale-Plan Protection

1. Superblock restoration (`restore_primary_superblock` / `restore_backup_superblock`)
2. Journal slot cleanup & `Dirty` state reconciliation
3. Block & inode bitmap reconstruction
4. Directory graph repair (`remove_dangling_dir_entry`, `remove_duplicate_dir_entry`, `break_directory_cycle`, `fix_parent_inode`)
5. Orphaned inode reconnection (`reconnect_orphan_inode` -> `/lost+found/inode_<id>`)

Before applying any non-dry-run plan, the engine verifies `current_sha256 == expected_disk_sha256` (`ERR_STALE_PLAN` if modified), saves `<exp_id>.pre_repair.gfs`, applies writes under exclusive `flock` + `fsync`, and runs a fresh `full` `gfsck` post-check.

---

## 8. Complexity

- `gfsck`: $O(N_{\text{blocks}} + N_{\text{inodes}} + N_{\text{dirents}})$ time and $O(N_{\text{blocks}})$ memory for ownership maps.
- `compare_experiment`: $O(N_{\text{blocks}} \cdot B)$ time, $O(B)$ streaming block buffer memory.

---

## 9. FastAPI Endpoints (`/api/v1/corruption/...`)

- `POST /api/v1/corruption/experiments`
- `GET /api/v1/corruption/experiments`
- `GET /api/v1/corruption/experiments/{experiment_id}`
- `POST /api/v1/corruption/experiments/{experiment_id}/inject`
- `POST /api/v1/corruption/experiments/{experiment_id}/check`
- `POST /api/v1/corruption/experiments/{experiment_id}/repair-plan`
- `POST /api/v1/corruption/experiments/{experiment_id}/repair`
- `GET /api/v1/corruption/experiments/{experiment_id}/comparison`
- `GET /api/v1/corruption/experiments/{experiment_id}/manifest`
- `GET /api/v1/corruption/experiments/{experiment_id}/download`
- `DELETE /api/v1/corruption/experiments/{experiment_id}?confirm=true`

---

## 10. Automated Browser Testing (Playwright)

Project-local `@playwright/test` and Chromium are configured in [`playwright.config.ts`](file:///Users/abhay/Documents/projects/GhostFS/frontend/playwright.config.ts) with E2E tests in [`ghostfs.spec.ts`](file:///Users/abhay/Documents/projects/GhostFS/frontend/e2e/ghostfs.spec.ts). Screenshots and test artifacts are written to `frontend/test-results/` (excluded from Git).
