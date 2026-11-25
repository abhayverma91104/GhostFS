# Operating Systems Syllabus Mapping

| OS Concept | GhostFS Module | Source File & Symbol | Implemented / Notes |
| :--- | :--- | :--- | :--- |
| Unix/POSIX File I/O | Virtual Disk | `engine/src/disk/virtual_disk.cpp` (`VirtualDisk::open_image`, `read_block`, `write_block`) | POSIX `open`, `pread`, `pwrite`, `fsync` |
| Shared & Exclusive Locking | Virtual Disk | `engine/src/disk/virtual_disk.cpp` (`flock` `LOCK_SH` / `LOCK_EX`) | Prevents concurrent conflicting writes |
| Process Creation & IPC | Backend Runner | `backend/services/engine_runner.py` (`run_engine`) | `asyncio.create_subprocess_exec` JSON stdin/stdout |
| Contiguous / Extent Allocation | Allocator | `engine/src/allocator/allocator.cpp` (`FirstFit`, `BestFit`, `WorstFit`, `BitmapNextFit`) | Operates on real GFS1 block bitmap |
| Internal & External Fragmentation | Fragmentation | `engine/src/allocator/fragmentation.cpp` (`FragmentationAnalyzer::analyze`) | Measures slack bytes & free-extent dispersion |
| Inode & Indexed Block Addressing | File System | `engine/src/filesystem/gfs_fs.cpp` (`GfsFileSystem`) | 12 direct + single-indirect + double-indirect |
| Directory Structures | File System | `engine/src/filesystem/gfs_fs.cpp`, `gfs_format.cpp` | 64-byte `DirEntry` records |
| Crash Detection & Journal | File System / Repair | `engine/src/filesystem/gfs_fs.cpp`, `engine/src/repair/fsck.cpp` | `Dirty` flag + 64-byte intent records |
| Consistency Checking (`fsck`) | Repair Lab | `engine/src/repair/fsck.cpp` (`GfsckEngine::check`) | Ownership map + cycle-safe BFS |
| Disk Scheduling (6 Algorithms) | Scheduling | `engine/src/scheduling/disk_scheduler.cpp` (`DiskScheduler::simulate`) | Educational simulator (`FCFS`, `SSTF`, `SCAN`, `C-SCAN`, `LOOK`, `C-LOOK`) |

## OS Concepts Explicitly NOT Implemented
GhostFS does **not** implement CPU process scheduling, virtual memory / demand paging, page replacement algorithms, Banker's algorithm / deadlock detection, or kernel POSIX multithreading.
