# GhostFS — Phase 4: Forensic Recovery Engine Specification

## 1. Non-Negotiable Recovery Principles
- **Read-Only Source Access:** All scans and extractions open the `.gfs` disk image using `O_RDONLY` and `flock(LOCK_SH)`. Source disk images are never modified.
- **No Hidden Backups or Fabricated Bytes:** Recovery operates exclusively on surviving GFS1 disk bytes. Missing/overwritten byte ranges are reported explicitly and never silently fabricated.
- **Stale Scan Rejection:** Both scan and extraction compute a streaming SHA-256 digest of the `.gfs` file (`64 KiB` chunks). Extraction rejects stale scan candidates with `ERR_STALE_SCAN` if the source disk hash changed.

---

## 2. Metadata-Based Recovery Algorithm
1. **Inode & Directory Scan:** Reads all inodes (`1 .. total_inodes - 1`) with CRC32 validation and scans directory data blocks (for both active and un-overwritten deleted directories) to collect `Deleted` `DirEntry` hints.
2. **Superseded Directory Entry & Inode Reuse Handling:** Because GFS1 directory slots are allocated sequentially into `Empty` slots before `Deleted` slots, a later `Deleted` entry for the same `inode_id` supersedes an earlier one. Superseded entries are preserved and reported as `unrecoverable` (inode reused) so old and new file identities are never confused.
3. **Indirect & Double-Indirect Pointer Validation:** Before following `indirect_block` or `double_indirect_block`, the engine verifies the pointer block lies inside `[data_region_start, journal_start)` and has not been reallocated to an active inode or a newer deleted inode. If reused, direct blocks (`0..11`) are still evaluated for partial recovery while indirect ranges are marked `missing_indirect`.
4. **Overwrite & Re-Delete Detection:** Each data block is checked against:
   - Active inode ownership (`overwritten`),
   - Block bitmap allocation state (`overwritten`),
   - Newer deleted inode block history (`deleted_at_ms` / `modified_at_ms` comparison — detects overwrite-then-redelete),
   - Full-file `content_crc32` verification (`crc32_mismatch` downgrades to `overwritten`).
   - **Documented Limitation:** If an external/untracked tool overwrote a free block and freed it again without leaving a newer deleted inode, and the file was only partially overwritten or had no checksum, GFS1 metadata alone cannot prove historical block reuse outside full-file CRC32 verification.

---

## 3. Signature-Based File Carving (`carver.cpp`)
Scans contiguous unallocated data extents (`BlockAllocator::find_free_extents`) in bounded `<= 16 MiB` windows (never concatenating across unrelated free extents):
- **PNG:** Validates `\x89PNG\r\n\x1a\n`, `IHDR` (`13` bytes), chunk boundaries, per-chunk IEEE CRC32s, and `IEND`.
- **JPEG:** Validates `SOI` (`FF D8 FF`), length-prefixed marker segments (`DQT`/`DHT`/`SOF`/`APPn`), `SOS` entropy stream (skipping `FF 00` byte stuffing & restart markers), and `EOI` (`FF D9`).
- **PDF:** Validates `%PDF-1.x`/`2.x`, structural keywords (`obj`/`/Type`/`xref`), and `%%EOF` (extending across incremental updates within the same contiguous extent).
- **TXT (Optional Heuristic):** Can be disabled; checks `>= 95%` printable ASCII/UTF-8 ratio (`>= 16` bytes after trimming trailing `\x00`), explicitly labeled as a heuristic candidate (`TXT` has no universal magic signature).
- **Limitation:** Fragmented files whose metadata has been lost cannot be automatically reassembled by contiguous signature carving.

---

## 4. Recovery Classification Rules & Integrity Limitations
- `fully_verified`: All expected bytes reconstructed and original metadata `content_crc32` verified, or `0`-byte file with intact metadata.
- `complete_unverified`: All expected bytes reconstructed (including structurally valid carved PNGs with matching chunk CRC32s, JPEGs, PDFs, and heuristic TXTs), but no original filesystem checksum exists to prove original-content identity.
- `partially_recoverable`: Subset of logical blocks present (`unknown` historical integrity when unverified by whole-file CRC32); missing byte ranges explicitly recorded.
- `overwritten`: All blocks replaced or full-file CRC32 mismatch.
- `unrecoverable`: Inode reused or no valid block pointers remain.
- `unknown`: Insufficient evidence.

### Integrity & Evidence Limitations
- **CRC32 vs. Cryptographic Authentication:** IEEE 802.3 CRC32 detects accidental corruption and overwrites, not malicious tampering; SHA-256 is computed on extracted artifacts and source disk images for provenance.
- **Structural Validation vs. Original-Content Integrity:** Validating a carved PNG's chunk CRCs or JPEG/PDF markers proves internal format consistency, not that the bytes match a specific deleted filesystem entry.
- **Historical Allocation Evidence:** GFS1 does not maintain a full block-history log; free blocks in partially recoverable files are marked `unknown` rather than assumed intact when whole-file CRC32 cannot be checked.

---

## 5. Complexity & API Contracts
- **Complexity:** Metadata scan is `O(I + D)` (`I` = inodes, `D` = directory/indirect blocks); carving is `O(F)` (`F` = unallocated data bytes) with `O(min(F, 16 MiB))` memory bound.
- **API Endpoints:** `POST /api/v1/disks/{name}/recovery/scan`, `GET .../scans/{scan_id}`, `GET .../scans/{scan_id}/candidates`, `GET .../candidates/{candidate_id}`, `POST .../extract`, `GET /api/v1/recovery/artifacts/{id}`, `GET .../manifest`, `GET .../download`.
