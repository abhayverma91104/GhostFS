from __future__ import annotations

import base64
import io
from pathlib import Path

from fastapi.testclient import TestClient
from pypdf import PdfReader

from app.config import settings
from app.main import app


def test_all_six_report_types_stale_mismatch_and_cleanup() -> None:
    client = TestClient(app)
    disk_name = "pytest_phase9_all6.gfs"
    disk_path = settings.disk_images_dir / disk_name
    for p in (disk_path, Path(f"{disk_path}.lock")):
        if p.exists():
            p.unlink()

    report_ids = []
    exp_id = None
    try:
        assert client.post(
            "/api/v1/disks",
            json={"name": disk_name, "size_bytes": 524288, "block_size": 4096},
        ).status_code == 201
        assert client.post(
            f"/api/v1/disks/{disk_name}/format",
            json={"volume_label": "AllSix", "alloc_policy": "first_fit", "inode_count": 64},
        ).status_code == 200
        b64 = base64.b64encode(b"Phase 9 All Six Reports Payload").decode("ascii")
        assert client.post(
            f"/api/v1/disks/{disk_name}/fs/files",
            json={"path": "/doc.txt", "content_base64": b64, "overwrite": True},
        ).status_code == 201

        exp = client.post(
            "/api/v1/corruption/experiments",
            json={"source_disk_name": disk_name, "sandbox_name": "p9_sb"},
        ).json()
        exp_id = exp["experiment_id"]

        for rtype in (
            "disk_analysis",
            "forensic_recovery",
            "corruption_repair",
            "disk_scheduling",
            "binary_analysis",
            "complete_investigation",
        ):
            payload = {
                "report_type": rtype,
                "disk_name": disk_name,
                "title": f"Test {rtype}",
                "experiment_id": exp_id if rtype in ("corruption_repair", "complete_investigation") else None,
                "scheduling_payload": {
                    "min_cylinder": 0,
                    "max_cylinder": 199,
                    "initial_head": 53,
                    "initial_direction": "right",
                    "requests": [98, 183, 37, 122, 14, 124, 65, 67],
                }
                if rtype in ("disk_scheduling", "complete_investigation")
                else None,
            }
            res = client.post("/api/v1/reports/generate", json=payload)
            assert res.status_code == 200, f"{rtype}: {res.text}"
            rid = res.json()["report_id"]
            report_ids.append(rid)

            pdf_res = client.get(f"/api/v1/reports/{rid}/download/pdf")
            assert pdf_res.status_code == 200
            reader = PdfReader(io.BytesIO(pdf_res.content))
            assert len(reader.pages) >= 1

        # Stale SHA-256 warning check
        stale_prev = client.post(
            "/api/v1/reports/preview",
            json={
                "report_type": "disk_analysis",
                "disk_name": disk_name,
                "expected_source_sha256": "0" * 64,
            },
        )
        assert stale_prev.status_code == 200
        assert any("STALE EVIDENCE" in w for w in stale_prev.json()["warnings"])

        # Delete reports & verify source disk still exists
        for rid in list(report_ids):
            assert client.delete(f"/api/v1/reports/{rid}?confirm=true").status_code == 200
            report_ids.remove(rid)
        assert disk_path.is_file()
    finally:
        for rid in report_ids:
            client.delete(f"/api/v1/reports/{rid}?confirm=true")
        if exp_id:
            client.delete(f"/api/v1/corruption/experiments/{exp_id}?confirm=true")
        for p in (disk_path, Path(f"{disk_path}.lock")):
            if p.exists():
                p.unlink()
