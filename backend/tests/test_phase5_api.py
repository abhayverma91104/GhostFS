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


def test_hex_viewer_endpoints_and_immutability(isolated_workspace: Path) -> None:
    disk_name = "hex_api.gfs"
    with TestClient(app) as client:
        assert (
            client.post(
                "/api/v1/disks",
                json={"name": disk_name, "size_bytes": 128 * 1024, "block_size": 1024},
            ).status_code
            == 201
        )
        b64 = base64.b64encode(b"HEX_VIEWER_SEARCH_VECTOR_123").decode("ascii")
        assert (
            client.post(
                f"/api/v1/disks/{disk_name}/fs/files",
                json={"path": "/sample.txt", "content_base64": b64, "overwrite": False},
            ).status_code
            == 201
        )

        disk_path = isolated_workspace / "disk_images" / disk_name
        sha_before = hashlib.sha256(disk_path.read_bytes()).hexdigest()

        # Block 1 read & structure decode
        blk_res = client.get(f"/api/v1/disks/{disk_name}/hex/blocks/1?bytes_per_row=16")
        assert blk_res.status_code == 200
        assert blk_res.json()["signature"]["file_type"] == "gfs1_superblock"

        str_res = client.get(f"/api/v1/disks/{disk_name}/hex/structures?block_id=1")
        assert str_res.status_code == 200
        assert str_res.json()["structure_type"] == "primary_superblock"
        assert str_res.json()["checksum_valid"] is True

        # Interpret bytes
        int_res = client.post(
            f"/api/v1/disks/{disk_name}/hex/interpret",
            json={"bytes": [0x31, 0x53, 0x46, 0x47]},
        )
        assert int_res.status_code == 200
        assert int_res.json()["uint32_le"] == 0x47465331

        # Search ASCII & reject bad hex
        sr_res = client.post(
            f"/api/v1/disks/{disk_name}/hex/search",
            json={"query_type": "ascii", "query_value": "HEX_VIEWER_SEARCH_VECTOR_123", "scope": "disk"},
        )
        assert sr_res.status_code == 200
        assert sr_res.json()["total_matches"] == 1

        bad_hex = client.post(
            f"/api/v1/disks/{disk_name}/hex/search",
            json={"query_type": "hex", "query_value": "XYZ", "scope": "disk"},
        )
        assert bad_hex.status_code == 400

        # Analyze & Compare
        an_res = client.post(
            f"/api/v1/disks/{disk_name}/hex/analyze",
            json={"offset": 1024, "length": 1024},
        )
        assert an_res.status_code == 200
        assert 0.0 <= an_res.json()["shannon_entropy"] <= 8.0

        cmp_res = client.post(
            f"/api/v1/disks/{disk_name}/hex/compare",
            json={"mode": "primary_vs_backup_sb"},
        )
        assert cmp_res.status_code == 200
        assert cmp_res.json()["matching_pct"] == 100.0

        # Logical file hex read
        f_res = client.get(f"/api/v1/disks/{disk_name}/hex/files?path=/sample.txt&offset=0&length=28")
        assert f_res.status_code == 200
        assert f_res.json()["length"] == 28

        sha_after = hashlib.sha256(disk_path.read_bytes()).hexdigest()
        assert sha_before == sha_after
