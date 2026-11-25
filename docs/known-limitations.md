# GhostFS Known Limitations

1. **Educational Scope**: GhostFS is not a production POSIX/VFS kernel filesystem, not a legal forensic certification tool, and not a replacement for the host OS I/O scheduler.
2. **Journal Replay**: GFS1's 64-byte journal entries record metadata intent only (no physical before/after block images); `gfsck`/repair performs conservative consistency reconstruction rather than full transactional WAL replay.
3. **Unrecoverable Scenarios**: Overwritten file blocks, `PAYLOAD_CRC_MISMATCH`, `DUAL_SUPERBLOCK_UNRECOVERABLE`, `TRUNCATED_IMAGE`, and `DUPLICATE_BLOCK_OWNERSHIP` cannot be automatically repaired from metadata alone.
4. **Simulated Disk Scheduling**: Cylinder seeks are simulated over virtual GFS1 block mappings (`floor(LBA / blocks_per_cylinder)`), which do not reflect physical HDD geometry or SSD flash translation layers.
5. **macOS Host Sanitizers**: Apple's ASan runtime hung during `dyld`/`atos` startup on the macOS host; therefore ASan/UBSan runtime verification is executed inside the Linux ARM64 Docker container.
