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


def test_disk_lifecycle_and_reformat(isolated_workspace: Path) -> None:
    with TestClient(app) as client:
        create_res = client.post(
            "/api/v1/disks",
            json={
                "name": "lab_alpha",
                "size_bytes": 512 * 1024,
                "block_size": 4096,
                "label": "AlphaVol",
                "alloc_policy": "first_fit",
            },
        )
        assert create_res.status_code == 201
        created = create_res.json()
        assert created["disk_name"] == "lab_alpha.gfs"
        assert created["block_size"] == 4096
        assert created["total_blocks"] == 128

        list_res = client.get("/api/v1/disks")
        assert list_res.status_code == 200
        disks = list_res.json()["disks"]
        assert len(disks) == 1
        assert disks[0]["disk_name"] == "lab_alpha.gfs"

        # Reformat to 1024B blocks
        fmt_res = client.post(
            "/api/v1/disks/lab_alpha.gfs/format",
            json={"block_size": 1024, "label": "Reformatted", "alloc_policy": "best_fit"},
        )
        assert fmt_res.status_code == 200
        assert fmt_res.json()["block_size"] == 1024
        assert fmt_res.json()["total_blocks"] == 512

        # Delete requires confirm=true
        no_conf = client.delete("/api/v1/disks/lab_alpha.gfs")
        assert no_conf.status_code == 400
        del_res = client.delete("/api/v1/disks/lab_alpha.gfs?confirm=true")
        assert del_res.status_code == 200


def test_binary_upload_download_sha256_and_forensic_delete(isolated_workspace: Path) -> None:
    disk_name = "evidence.gfs"
    binary_payload = bytes((i * 17 + 5) % 256 for i in range(15000))
    expected_sha = hashlib.sha256(binary_payload).hexdigest()
    marker_text = b"GHOSTFS_DELETED_PAYLOAD_PERSISTS_IN_BLOCKS"

    with TestClient(app) as client:
        res = client.post(
            "/api/v1/disks",
            json={"name": disk_name, "size_bytes": 512 * 1024, "block_size": 1024},
        )
        assert res.status_code == 201

        assert client.post(f"/api/v1/disks/{disk_name}/fs/mkdir", json={"path": "/cases"}).status_code == 201
        assert client.post(f"/api/v1/disks/{disk_name}/fs/mkdir", json={"path": "/cases/bin"}).status_code == 201

        # Multipart binary upload
        up_res = client.post(
            f"/api/v1/disks/{disk_name}/fs/files/upload",
            data={"dir_path": "/cases/bin", "overwrite": "false"},
            files={"file": ("sample.bin", binary_payload, "application/octet-stream")},
        )
        assert up_res.status_code == 201
        up_entry = up_res.json()["entry"]
        assert up_entry["sha256"] == expected_sha
        assert up_entry["indirect_block"] != 0  # 15 blocks > 12 direct blocks

        # Duplicate upload with overwrite=false must fail cleanly
        dup_res = client.post(
            f"/api/v1/disks/{disk_name}/fs/files/upload",
            data={"dir_path": "/cases/bin", "overwrite": "false"},
            files={"file": ("sample.bin", binary_payload, "application/octet-stream")},
        )
        assert dup_res.status_code == 409

        # Download & verify exact SHA-256
        dl_res = client.get(f"/api/v1/disks/{disk_name}/fs/files/download?path=/cases/bin/sample.bin")
        assert dl_res.status_code == 200
        assert dl_res.content == binary_payload
        assert hashlib.sha256(dl_res.content).hexdigest() == expected_sha

        # Write text marker, then delete and verify raw bytes remain on .gfs image
        txt_b64 = base64.b64encode(marker_text).decode("ascii")
        w_res = client.post(
            f"/api/v1/disks/{disk_name}/fs/files",
            json={"path": "/cases/secret.txt", "content_base64": txt_b64, "overwrite": False},
        )
        assert w_res.status_code == 201

        del_res = client.delete(f"/api/v1/disks/{disk_name}/fs/delete?path=/cases/secret.txt")
        assert del_res.status_code == 200
        assert del_res.json()["released_blocks_count"] == 1

    # Reopen with a brand-new TestClient to confirm persistence across restarts
    with TestClient(app) as client2:
        tree_res = client2.get(f"/api/v1/disks/{disk_name}/fs/tree?path=/cases")
        assert tree_res.status_code == 200
        names = [e["name"] for e in tree_res.json()["entries"]]
        assert "bin" in names
        assert "secret.txt" not in names

    raw_disk_bytes = (isolated_workspace / "disk_images" / disk_name).read_bytes()
    assert marker_text in raw_disk_bytes


def test_path_traversal_and_invalid_inputs_rejected(isolated_workspace: Path) -> None:
    with TestClient(app) as client:
        bad = client.post(
            "/api/v1/disks",
            json={"name": "../escape", "size_bytes": 262144, "block_size": 4096},
        )
        assert bad.status_code == 422

        bad_size = client.post(
            "/api/v1/disks",
            json={"name": "odd", "size_bytes": 300000, "block_size": 4096},
        )
        assert bad_size.status_code == 400
