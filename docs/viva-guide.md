# GhostFS — Viva & Oral Examination Guide (50 Questions & Answers)

## Basic (1–12)
1. **What is GhostFS?** An educational Operating Systems workstation implementing a custom binary file system (`GFS1`), block allocation policies, forensic recovery, binary hex inspection, sandbox corruption/repair (`gfsck`), disk scheduling simulation, and PDF/JSON/CSV reporting.
2. **What problem does it solve?** It lets students observe real on-disk metadata structures, fragmentation, unlinked residual data, corruption invariants, and seek scheduling on safe `.gfs` virtual disk images without risking physical disks.
3. **Why use C++17 for the core engine?** Direct deterministic control over fixed-width binary serialization (`endian.hpp`), POSIX `flock`/`pread`/`pwrite`, memory bounds, and zero-overhead algorithmic execution (`engine/src/`).
4. **Why use FastAPI?** Asynchronous subprocess orchestration (`engine_runner.py`), Pydantic schema validation, safe file streaming, and ReportLab PDF generation.
5. **Why use React + TypeScript?** Responsive interactive visualization of block grids, hex editors, SVG head trajectories, and strict type safety (`frontend/src/types/api.ts`).
6. **What is a virtual disk?** A regular host file (`disk_images/*.gfs`) sized to `total_blocks * block_size` bytes and accessed exclusively through `VirtualDisk` (`virtual_disk.cpp`).
7. **Why never touch physical disks?** Operating on `.gfs` files inside `disk_images/` prevents accidental host data loss and requires no root privileges.
8. **What block sizes does GFS1 support?** `512`, `1024`, `2048`, and `4096` bytes (`4096` default).
9. **What is the minimum disk size?** `64` blocks (`kMinDiskBlocks` in `gfs_format.hpp`).
10. **How does the frontend talk to C++?** React -> HTTP/JSON (`/api/v1/*`) -> FastAPI -> JSON stdin/stdout subprocess -> `ghostfs_engine`.
11. **What is `flock`?** A POSIX advisory file lock (`LOCK_SH` for reads, `LOCK_EX` for writes) on `<disk>.lock` (`VirtualDisk::open_image`).
12. **What is a workspace confinement guard?** `resolve_workspace_path` (`engine.cpp`) canonicalizes paths and rejects `..`, symlinks, and escapes outside workspace directories.

## Intermediate (13–28)
13. **What is a superblock?** A 128-byte header (`Superblock` in `gfs_format.hpp`, Blocks 1 and 2) storing magic `0x47465331` (`GFS1`), block geometry, free counts, `alloc_policy`, `fs_state`, and IEEE CRC32.
14. **Why keep a backup superblock?** If Block 1 is corrupted, `gfsck` / `RepairEngine` can validate Block 2 and restore Block 1 (`restore_primary_superblock`).
15. **What is an inode?** A 128-byte metadata record (`Inode` in `gfs_format.hpp`) storing `inode_id`, `type`, `state`, `size_bytes`, timestamps, `content_crc32`, `generation`, `direct_blocks[12]`, `indirect_block`, and `double_indirect_block`.
16. **What are bitmaps?** Bit arrays in `inode_bitmap` and `block_bitmap` tracking free (`0`) vs. allocated (`1`) inodes and blocks.
17. **How does direct addressing work?** `direct_blocks[0..11]` directly store up to 12 data block LBAs (`48 KiB` at `4096 B` blocks).
18. **How do single and double indirect addressing work?** `indirect_block` points to a block of `block_size / 4` `uint32_t` LBAs; `double_indirect_block` points to a block of single-indirect block LBAs.
19. **How does file deletion work in GFS1?** `GfsFileSystem::delete_file` clears bitmap bits and marks the inode `Deleted` and directory entry `Deleted` **without** zeroing data blocks, leaving residual bytes for forensic recovery.
20. **How do First Fit, Best Fit, Worst Fit, and Bitmap Next Fit differ?** First Fit picks the lowest-LBA extent `>= needed`; Best Fit picks the smallest sufficient extent; Worst Fit picks the largest extent; Next Fit resumes from `next_alloc_cursor` (`allocator.cpp`).
21. **What happens if no single contiguous extent is large enough?** `BlockAllocator` falls back to multi-extent (fragmented) allocation if `free_blocks >= needed`.
22. **How is external fragmentation calculated?** `(1.0 - largest_free_extent / free_blocks) * 100.0` (`fragmentation.cpp`).
23. **How is internal fragmentation calculated?** `sum(allocated_data_blocks * block_size - size_bytes)` across regular file payload blocks.
24. **How does subprocess IPC work?** `engine_runner.py` spawns `ghostfs_engine` with `asyncio.create_subprocess_exec`, sends one JSON object on stdin, and parses one JSON object from stdout within a bounded timeout.
25. **Why use `LOCK_SH` vs `LOCK_EX`?** Multiple readers (Hex Viewer, Recovery Scan, `gfsck`, Reports) can run concurrently with `LOCK_SH`, while writers (`write_file`, `inject_corruption`, `apply_plan`) require exclusive `LOCK_EX`.
26. **What is `fs_state = Dirty`?** Set when a metadata operation is interrupted before journal commit; normal mutations are blocked (`ERR_DIRTY_FILESYSTEM`) while read-only inspection remains permitted.
27. **How does the Hex Viewer remain read-only?** It opens images via `VirtualDisk::open_image(..., write_mode=false)` (`O_RDONLY`).
28. **What is `bytes_per_row`?** Configurable row width (`8`, `16`, or `32` bytes) in `HexInspector::read_block` / `read_disk_range`.

## Advanced (29–50)
29. **Why can deleted files sometimes be recovered?** Because `delete_file` preserves `Deleted` inode metadata and leaves data block payloads intact until overwritten by a later allocation.
30. **What happens when blocks are overwritten?** `metadata_recovery.cpp` detects that a referenced block is now owned by another `Allocated` inode and marks that logical block `"overwritten"`, classifying the file as `partially_recoverable` or `overwritten`.
31. **How is inode reuse detected?** If an inode slot's `state` is `Allocated` or its `generation` changed, the old deleted file's metadata no longer survives.
32. **What are the limits of historical allocation evidence?** If a block is currently `Free` (`bitmap == 0`) and unreferenced by any active inode, surviving metadata alone cannot prove it wasn't reused and re-deleted in the past; therefore Phase 5/6 marks its block state `"unknown"` unless full-file CRC32 matches (`fully_verified`).
33. **How does file carving work?** `carver.cpp` scans unallocated blocks for magic signatures (PNG `89 50 4E 47`, JPEG `FF D8 FF`, PDF `%PDF-`, and heuristic UTF-8 TXT).
34. **Why is TXT detection heuristic?** Plain text has no universal magic header; `carver.cpp` uses printable ASCII/UTF-8 density thresholds.
35. **What is the difference between CRC32 and SHA-256?** CRC32 (32-bit) is fast error-detection stored in GFS1 headers/inodes; SHA-256 (256-bit) is a cryptographic hash used for disk immutability, stale-scan checks, and report artifact manifests.
36. **Why does a carved PNG with valid chunk CRCs not prove `fully_verified`?** Valid PNG chunk CRCs prove the carved bytes form a valid PNG structure (`complete_unverified` / `structure_verified`), not that they match a specific deleted file's original SHA-256/CRC32.
37. **How does Shannon entropy work?** $H = -\sum_{i=0}^{255} p_i \log_2(p_i) \in [0, 8]$ bits/byte (`hex_inspector.cpp`); `0` = uniform, `< 3.5` = sparse, `3.5–6.5` = text/metadata, `>= 6.5` = compressed/encrypted/random.
38. **How does `hex_search` avoid missing cross-boundary patterns?** It streams 64 KiB chunks with `pattern_len - 1` overlap between consecutive windows.
39. **How does `gfsck` reconstruct block ownership?** `fsck.cpp` traverses every `Allocated` inode's direct, single-indirect, and double-indirect pointers into `block_owners[block_id]`.
40. **How are duplicate block references handled?** Blocks with `owners.size() > 1` are flagged `DUPLICATE_BLOCK_OWNERSHIP` (`ambiguous_manual_required`) and never arbitrarily freed.
41. **How does `gfsck` avoid infinite loops on directory cycles?** Iterative BFS with a `visited_dirs` set (`fsck.cpp`).
42. **What can the GFS1 journal recover, and why is it not full WAL replay?** GFS1's 64-byte `JournalEntry` stores operation intent only (no physical before/after block images), so `RepairEngine` performs conservative consistency reconstruction (aborting pending slots, reconstructing bitmaps, clearing `Dirty` only if metadata checks pass).
43. **Why can metadata repair fail to restore file contents?** Metadata repair restores pointers/bitmaps/directory links, not overwritten or bit-flipped payload bytes (`PAYLOAD_CRC_MISMATCH` remains `not_repairable`).
44. **How are stale scans and stale repair plans detected?** By comparing current disk SHA-256 against `expected_source_sha256` (`ERR_STALE_SCAN` / `ERR_STALE_PLAN`).
45. **Why must source disk images remain immutable during recovery, corruption lab, scheduling, and reporting?** Preserving forensic evidence integrity requires never mutating original evidence; all destructive tests run exclusively on clones in `disk_images/sandbox/`.
46. **How are report artifacts verified?** `manifest.json` stores external SHA-256 hashes of `report.pdf`, `evidence.json`, and `exports/*.csv`; `verify_report_integrity` recomputes and compares them.
47. **How is CSV formula injection prevented?** `csv_exporter.py` prefixes cells starting with `=`, `+`, `-`, `@`, `\t`, or `\r` with `'`.
48. **How does `DiskScheduler` compute textbook SCAN vs LOOK?** `SCAN` travels to `min_cylinder`/`max_cylinder` when reversal is required (`331`); `LOOK` reverses at the last pending request (`299`).
49. **Why is GhostFS disk scheduling a simulation?** `.gfs` files live on host OS filesystems (often SSDs); `DiskScheduler` models mechanical cylinder seeks (`floor(LBA / blocks_per_cylinder)`) for educational comparison.
50. **How does GhostFS differ from a production filesystem?** No kernel VFS mount, no multi-threaded page cache, 64-byte intent journal instead of full physical WAL, and single-workstation localhost scope.
