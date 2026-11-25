# GhostFS Security & Data-Integrity Model

1. **Workspace Confinement**: All disk images (`disk_images/`), sandbox clones (`disk_images/sandbox/`), recovered artifacts (`reports/recovered/`), and generated reports (`reports/generated/`) are confined via canonical path validation (`resolve_workspace_path` in C++ and `resolve_report_dir` in Python).
2. **Symlink Escape Prevention**: Symlinks are explicitly rejected before canonicalization (`std::filesystem::is_symlink` in `corruption.cpp`, `is_symlink()` in `reports.py`).
3. **Physical Disk Isolation**: GhostFS never opens block devices, physical disks, or arbitrary host paths.
4. **POSIX File Locking**: `VirtualDisk` acquires `LOCK_SH` for read-only inspection/recovery/reporting and `LOCK_EX` for writes/corruption/repair.
5. **No Shell Interpolation**: Backend invokes `ghostfs_engine` via `asyncio.create_subprocess_exec` with JSON over stdin/stdout and bounded timeouts.
6. **Export Security**:
   - ReportLab PDF generation escapes all user-supplied strings with `xml.sax.saxutils.escape`.
   - CSV exports prefix cells starting with `=`, `+`, `-`, `@`, `\t`, `\r` with `'` to prevent spreadsheet formula injection.
   - JSON exports enforce `allow_nan=False`.
7. **Localhost Binding**: Services bind to `127.0.0.1` by default; no public exposure or authentication system is included in this local educational workstation.
