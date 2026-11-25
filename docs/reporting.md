# GhostFS Phase 8 — Forensic Reports & Evidence Export

This document specifies the GhostFS centralized reporting subsystem, immutable evidence snapshots, ReportLab PDF generation, JSON/CSV export formats, artifact integrity manifests, API contracts, reproducible complete investigation demo, and Phase 9 acceptance checklist.

> **Educational Scope Disclaimer:** GhostFS is an educational custom file system and digital forensics workstation. Generated reports are educational investigation records and do **not** constitute legal forensic certification, production-grade recovery guarantees, or host OS I/O scheduling benchmarks.

---

## 1. Centralized Reporting Architecture

All reporting logic resides under [`backend/services/reporting/`](file:///Users/abhay/Documents/projects/GhostFS/backend/services/reporting/):
- [`evidence_manager.py`](file:///Users/abhay/Documents/projects/GhostFS/backend/services/reporting/evidence_manager.py): Collects authoritative C++17 engine outputs (`disk_info`, `fragmentation_report`, `blocks_map`, `recovery_scan`, `corruption_*`, `scheduling_compare`, `hex_*`), verifies source SHA-256 hashes, detects stale references, and produces an immutable `snapshot` with `snapshot_sha256`.
- [`pdf_generator.py`](file:///Users/abhay/Documents/projects/GhostFS/backend/services/reporting/pdf_generator.py): Generates print-friendly A4 multipage PDFs using ReportLab with vector bar charts, `repeatRows=1` pagination, XML-escaped user strings, wrapped SHA-256 values, page numbers, and explicit limitations.
- [`json_exporter.py`](file:///Users/abhay/Documents/projects/GhostFS/backend/services/reporting/json_exporter.py): Exports full-precision `evidence.json` with `allow_nan=False`.
- [`csv_exporter.py`](file:///Users/abhay/Documents/projects/GhostFS/backend/services/reporting/csv_exporter.py): Exports separate named UTF-8 CSV tables (`disk_blocks.csv`, `file_metadata.csv`, `recovery_candidates.csv`, `recovery_artifacts.csv`, `corruption_findings.csv`, `repair_actions.csv`, `scheduling_comparison.csv`, `scheduling_trace.csv`, `binary_entropy.csv`) with spreadsheet formula injection protection (`=`, `+`, `-`, `@`, `\t`, `\r` prefixed with `'`).
- [`report_builder.py`](file:///Users/abhay/Documents/projects/GhostFS/backend/services/reporting/report_builder.py): Orchestrates atomic report creation in `reports/generated/<report_id>/` using `asyncio.to_thread` for PDF generation, writes external `manifest.json` with SHA-256 hashes of every artifact, and provides `verify_report_integrity` and `delete_report`.

---

## 2. Supported Report Types

1. `disk_analysis` — Volume geometry, region distribution, and internal/external fragmentation.
2. `forensic_recovery` — Scan candidates, verified/unknown/missing/overwritten/substituted byte counts, and manifests.
3. `corruption_repair` — Sandbox ground-truth logs, independent `gfsck` findings, repair plans, and immutability checks.
4. `disk_scheduling` — Static/dynamic mode, 6-algorithm comparison, trajectories, and simulation disclaimer.
5. `binary_analysis` — Bounded hex/ASCII excerpts, GFS1 structure decoding, and Shannon entropy metrics.
6. `complete_investigation` — Consolidated multipage report assembling only sections with actual evidence.

---

## 3. API Contracts (`/api/v1/reports`)

- `GET /api/v1/reports/evidence`
- `POST /api/v1/reports/preview`
- `POST /api/v1/reports/generate`
- `GET /api/v1/reports`
- `GET /api/v1/reports/{report_id}`
- `GET /api/v1/reports/{report_id}/manifest`
- `GET /api/v1/reports/{report_id}/download/pdf`
- `GET /api/v1/reports/{report_id}/download/json`
- `GET /api/v1/reports/{report_id}/exports`
- `GET /api/v1/reports/{report_id}/exports/{export_id}`
- `POST /api/v1/reports/{report_id}/verify`
- `DELETE /api/v1/reports/{report_id}?confirm=true`

---

## 4. Reproducible Complete Investigation Demo

Run the end-to-end Python demonstration script:
```bash
/Users/abhay/Documents/projects/GhostFS/backend/.venv/bin/python /Users/abhay/Documents/projects/GhostFS/backend/scripts/run_complete_demo.py
```

---

## 5. Phase 9 Final Acceptance Checklist

- [ ] Linux ARM64/x86-64 portability verification
- [ ] Successful AddressSanitizer (ASan) runtime execution on Linux
- [ ] Successful UndefinedBehaviorSanitizer (UBSan) runtime execution on Linux
- [ ] Clean-install verification & dependency reproducibility (`CMakeLists.txt`, `requirements.txt`, `package.json`)
- [ ] Complete C++ (`ctest`), FastAPI (`pytest`), and Playwright E2E regression suite
- [ ] Security & path-confinement review (symlinks, traversal, CSV injection, XML escaping)
- [ ] Documentation review across `docs/` and `README.md`
- [ ] OS Lab syllabus mapping, final demonstration, and viva preparation
