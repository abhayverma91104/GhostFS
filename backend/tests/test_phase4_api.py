from __future__ import annotations

import base64
import hashlib
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from app.config import settings
from app.main import app


@pytest.fixture()
def isolated_workspace(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Path:
    (tmp_path / "disk_images" / "sandbox").mkdir(parents=True)
    (tmp_path / "reports" / "recovered").mkdir(parents=True)
    monkeypatch.setattr(settings, "workspace_root", tmp_path)
    monkeypatch.setattr(settings, "disk_images_dir", tmp_path / "disk_images")
    monkeypatch.setattr(settings, "sandbox_dir", tmp_path / "disk_images" / "sandbox")
    monkeypatch.setattr(settings, "reports_dir", tmp_path / "reports")
    monkeypatch.setattr(settings, "recovered_dir", tmp_path / "reports" / "recovered")
    return tmp_path


def test_recovery_scan_extract_manifest_download_and_immutability(isolated_workspace: Path) -> None:
    disk_name = "rec_api.gfs"
    secret = b"PHASE4_FORENSIC_API_PAYLOAD_VERIFY_SHA256\n" * 40
    expected_sha = hashlib.sha256(secret).hexdigest()

    with TestClient(app) as client:
        assert (
            client.post(
                "/api/v1/disks",
                json={"name": disk_name, "size_bytes": 128 * 1024, "block_size": 1024},
            ).status_code
            == 201
        )
        b64 = base64.b64encode(secret).decode("ascii")
        assert (
            client.post(
                f"/api/v1/disks/{disk_name}/fs/files",
                json={"path": "/evidence.txt", "content_base64": b64, "overwrite": False},
            ).status_code
            == 201
        )
        assert client.delete(f"/api/v1/disks/{disk_name}/fs/delete?path=/evidence.txt").status_code == 200

        disk_path = isolated_workspace / "disk_images" / disk_name
        disk_sha_before = hashlib.sha256(disk_path.read_bytes()).hexdigest()

        scan_res = client.post(
            f"/api/v1/disks/{disk_name}/recovery/scan",
            json={"mode": "combined", "file_types": [], "enable_txt_heuristic": True},
        )
        assert scan_res.status_code == 200
        scan = scan_res.json()
        assert scan["source_disk_sha256"] == disk_sha_before
        assert scan["summary"]["fully_verified"] == 1
        scan_id = scan["scan_id"]

        cands_res = client.get(
            f"/api/v1/disks/{disk_name}/recovery/scans/{scan_id}/candidates?offset=0&limit=10"
        )
        assert cands_res.status_code == 200
        cand = cands_res.json()["candidates"][0]
        assert cand["filename"] == "evidence.txt"

        ext_res = client.post(
            f"/api/v1/disks/{disk_name}/recovery/extract",
            json={"scan_id": scan_id, "candidate_ids": [cand["candidate_id"]]},
        )
        assert ext_res.status_code == 200
        manifest = ext_res.json()["artifacts"][0]
        assert manifest["recovered_sha256"] == expected_sha
        artifact_id = manifest["recovery_id"]

        prev_res = client.get(f"/api/v1/recovery/artifacts/{artifact_id}")
        assert prev_res.status_code == 200
        assert prev_res.json()["is_text"] is True

        dl_res = client.get(f"/api/v1/recovery/artifacts/{artifact_id}/download")
        assert dl_res.status_code == 200
        assert hashlib.sha256(dl_res.content).hexdigest() == expected_sha

        man_res = client.get(f"/api/v1/recovery/artifacts/{artifact_id}/manifest")
        assert man_res.status_code == 200
        assert man_res.json()["recovered_sha256"] == expected_sha

        # Source disk must be byte-for-byte immutable!
        disk_sha_after = hashlib.sha256(disk_path.read_bytes()).hexdigest()
        assert disk_sha_before == disk_sha_after

        # Mutate disk -> stale scan extraction MUST be rejected (409)
        assert (
            client.post(
                f"/api/v1/disks/{disk_name}/fs/files",
                json={"path": "/new.txt", "content_base64": base64.b64encode(b"hi").decode("ascii")},
            ).status_code
            == 201
        )
        stale_res = client.post(
            f"/api/v1/disks/{disk_name}/recovery/extract",
            json={"scan_id": scan_id, "candidate_ids": [cand["candidate_id"]]},
        )
        assert stale_res.status_code == 409
