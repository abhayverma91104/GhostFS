from __future__ import annotations

import base64
import hashlib
import io
from pathlib import Path

from fastapi.testclient import TestClient
from pypdf import PdfReader

from app.config import settings
from app.main import app


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def test_complete_reporting_pdf_parsing_csv_security_and_integrity() -> None:
    client = TestClient(app)
    disk_name = "pytest_phase8_report.gfs"
    disk_path = settings.disk_images_dir / disk_name
    for p in (disk_path, Path(f"{disk_path}.lock")):
        if p.exists():
            p.unlink()

    created_report_ids = []
    exp_id = None
    try:
        assert client.post(
            "/api/v1/disks",
            json={"name": disk_name, "size_bytes": 524288, "block_size": 4096},
        ).status_code == 201
        assert client.post(
            f"/api/v1/disks/{disk_name}/format",
            json={"volume_label": "ReportLab", "alloc_policy": "best_fit", "inode_count": 64},
        ).status_code == 200

        # Upload file with special chars & CSV-formula-like prefix in name
        b64 = base64.b64encode(b"GhostFS Phase 8 Evidence <tag>&markup").decode("ascii")
        assert client.post(
            f"/api/v1/disks/{disk_name}/fs/files",
            json={"path": "/=cmd_test,file.txt", "content_base64": b64, "overwrite": True},
        ).status_code == 201
        assert client.post(
            f"/api/v1/disks/{disk_name}/fs/files",
            json={"path": "/normal.txt", "content_base64": b64, "overwrite": True},
        ).status_code == 201
        assert client.delete(f"/api/v1/disks/{disk_name}/fs/delete?path=/normal.txt").status_code == 200

        orig_sha = _sha256_file(disk_path)

        # Create & corrupt sandbox experiment
        exp = client.post(
            "/api/v1/corruption/experiments",
            json={"source_disk_name": disk_name, "sandbox_name": "p8_sb"},
        ).json()
        exp_id = exp["experiment_id"]
        assert client.post(
            f"/api/v1/corruption/experiments/{exp_id}/inject",
            json={"category": "primary_superblock", "scenario": "corrupt_magic", "confirm": True},
        ).status_code == 200

        # Preview & Generate Complete Investigation Report
        payload = {
            "report_type": "complete_investigation",
            "disk_name": disk_name,
            "title": "Phase 8 <Complete> & Investigation",
            "case_id": "CASE-008",
            "investigator": "Lab Analyst",
            "notes": "Testing <script> & special chars",
            "experiment_id": exp_id,
            "scheduling_payload": {
                "min_cylinder": 0,
                "max_cylinder": 199,
                "initial_head": 53,
                "initial_direction": "right",
                "requests": [98, 183, 37, 122, 14, 124, 65, 67],
            },
        }
        prev = client.post("/api/v1/reports/preview", json=payload)
        assert prev.status_code == 200, prev.text
        assert prev.json()["source_disk_sha256"] == orig_sha

        gen = client.post("/api/v1/reports/generate", json=payload)
        assert gen.status_code == 200, gen.text
        manifest = gen.json()
        rid = manifest["report_id"]
        created_report_ids.append(rid)

        # PDF Output Verification via pypdf
        pdf_res = client.get(f"/api/v1/reports/{rid}/download/pdf")
        assert pdf_res.status_code == 200
        assert pdf_res.headers["content-type"] == "application/pdf"
        reader = PdfReader(io.BytesIO(pdf_res.content))
        assert len(reader.pages) >= 1
        full_text = "\n".join(page.extract_text() or "" for page in reader.pages)
        assert "GhostFS" in full_text
        assert rid in full_text
        assert orig_sha[:16] in full_text

        # CSV Formula Injection Protection Check
        csv_res = client.get(f"/api/v1/reports/{rid}/exports/file_metadata")
        assert csv_res.status_code == 200
        assert "'/=cmd_test" in csv_res.text or "/=cmd_test" in csv_res.text

        # Artifact Integrity Verification + Tamper Detection
        ver_ok = client.post(f"/api/v1/reports/{rid}/verify")
        assert ver_ok.status_code == 200
        assert ver_ok.json()["verified"] is True

        # Tamper with JSON artifact and verify detection
        ev_path = settings.reports_dir / "generated" / rid / "evidence.json"
        ev_path.write_text(ev_path.read_text(encoding="utf-8") + " ", encoding="utf-8")
        ver_bad = client.post(f"/api/v1/reports/{rid}/verify")
        assert ver_bad.status_code == 200
        assert ver_bad.json()["verified"] is False

        # Path traversal & delete confirmation checks
        assert client.get("/api/v1/reports/..%2F..%2Fetc/manifest").status_code in (400, 404)
        assert client.delete(f"/api/v1/reports/{rid}?confirm=false").status_code == 400

        # Source disk must remain untouched
        assert _sha256_file(disk_path) == orig_sha
    finally:
        for rid in created_report_ids:
            client.delete(f"/api/v1/reports/{rid}?confirm=true")
        if exp_id:
            client.delete(f"/api/v1/corruption/experiments/{exp_id}?confirm=true")
        for p in (disk_path, Path(f"{disk_path}.lock")):
            if p.exists():
                p.unlink()
