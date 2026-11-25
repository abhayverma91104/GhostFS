# GhostFS — Pre-Phase 9 Acceptance Checklist

| Phase | Requirement | Implementation Location | Automated Test | Status | Evidence / Notes |
| :---: | :--- | :--- | :--- | :--- | :--- |
| 0–1 | Architecture, IPC, Workspace Confinement | `engine/src/common/engine.cpp`, `backend/services/engine_runner.py` | `EngineSmokeTest.*`, `PathGuardTest.*`, `test_health.py` | Passed | JSON stdin/stdout IPC, bounded timeouts |
| 2 | Virtual Disk & GFS1 Format (512–4096 B, direct/indirect, flock) | `engine/src/disk/virtual_disk.cpp`, `engine/src/filesystem/gfs_fs.cpp` | `Phase2DiskFsTest.*`, `test_phase2_api.py` | Passed | All 4 block sizes, CRC32, dirty journal detection |
| 3 | Block Allocation Policies & Fragmentation | `engine/src/allocator/allocator.cpp`, `fragmentation.cpp`, `comparison.cpp` | `Phase3AllocatorTest.*`, `test_phase3_api.py` | Passed | First/Best/Worst/Next Fit, deterministic tie-breaking |
| 4 | Forensic Recovery & Carving + Manifest Breakdown | `engine/src/recovery/recovery_engine.cpp`, `carver.cpp`, `metadata_recovery.cpp` | `Phase4RecoveryTest.*`, `test_phase4_api.py` | Passed | Verified vs unknown/missing/overwritten/substituted bytes |
| 5 | Read-Only Hex Viewer, GFS1 Decoder, Search, Entropy | `engine/src/hexviewer/hex_inspector.cpp`, `binary_parser.cpp` | `Phase5HexTest.*`, `test_phase5_api.py` | Passed | `/api/v1/disks/{disk_name}/hex/*`, LE/BE, Shannon entropy |
| 6 | Sandbox Corruption Lab, `gfsck`, Repair Engine | `engine/src/repair/corruption.cpp`, `fsck.cpp`, `repair.cpp` | `Phase6RepairTest.*`, `test_phase6_api.py` | Passed | Scenarios A–I, stale-plan rejection, symlink rejection |
| 7 | Disk Scheduling Simulator (6 Algs, Static & Dynamic) | `engine/src/scheduling/disk_scheduler.cpp`, `io_trace.cpp` | `Phase7SchedulingTest.*`, `test_phase7_api.py` | Passed | Exact textbook totals (640/236/331/382/299/322), dynamic test |
| 8 | Forensic Reports (PDF/JSON/CSV), Manifest Verify | `backend/services/reporting/*`, `backend/routes/reports.py` | `test_phase8_reports.py`, `test_phase9_reports_adversarial.py` | Passed | All 6 report types, `pypdf` verified, CSV injection protected |
| 9 | Adversarial Inputs, Scripts, Docker, Linux ASan/UBSan | `test_phase9_adversarial.cpp`, `scripts/*.sh`, `Dockerfile` | `Phase9AdversarialTest.*`, Docker ASan/UBSan | Passed | Ubuntu 24.04 aarch64 GCC 13.3.0 ASan (45/45) & UBSan (45/45) passed; clean-install (`/tmp/ghostfs_clean_test`) & PDF visual inspection verified |
