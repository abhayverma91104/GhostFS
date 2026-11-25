# GhostFS Final Demonstration Guide

## A. Three-Minute Demonstration
1. Start GhostFS (`./start.sh`) and open **Disk Manager**.
2. Create and format a `512 KB` disk (`demo3m.gfs`, `4096 B` blocks, `First Fit`).
3. In **File Explorer**, upload a text or PNG file, verify its SHA-256, then delete it.
4. In **Recovery Center**, run **Combined Scan**, inspect the `fully_verified` candidate and its original physical block locations, and extract it.

## B. Seven-Minute Demonstration
1. Populate a disk with multiple files, delete one, and write a larger file in **File Explorer**.
2. Open **Block Explorer** to inspect fragmentation metrics and run the 4-policy **Allocation Comparison**.
3. Open **Hex Viewer** on Block 1 (Primary Superblock `GFS1`) and on the fragmented file.
4. Open **Corruption Lab**, clone the disk to a sandbox, inject `Primary Superblock -> Corrupt Magic`, run `gfsck`, preview & apply the repair plan, and show the before/after region strip.
5. Open **Disk Scheduler** to compare all 6 algorithms on the textbook queue (`SSTF = 236`, `FCFS = 640`).

## C. Complete Technical Demonstration
Run the automated end-to-end investigation script:
```bash
./scripts/demo.sh
```
This creates a disposable disk, populates files, deletes & recovers with SHA-256 verification, injects & repairs sandbox corruption, runs all 6 scheduling algorithms, generates a complete PDF/JSON/CSV investigation report, and verifies all artifact SHA-256 hashes.
