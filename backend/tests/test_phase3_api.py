from __future__ import annotations

import base64
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


def test_block_map_pagination_and_fragmentation_and_policy_switch(isolated_workspace: Path) -> None:
    disk_name = "frag_api.gfs"
    with TestClient(app) as client:
        assert (
            client.post(
                "/api/v1/disks",
                json={"name": disk_name, "size_bytes": 256 * 1024, "block_size": 1024},
            ).status_code
            == 201
        )

        # Write two files, check pagination
        b64 = base64.b64encode(b"A" * 1500).decode("ascii")
        assert (
            client.post(
                f"/api/v1/disks/{disk_name}/fs/files",
                json={"path": "/alpha.bin", "content_base64": b64, "overwrite": False},
            ).status_code
            == 201
        )

        page1 = client.get(f"/api/v1/disks/{disk_name}/blocks/map?offset=0&limit=32")
        assert page1.status_code == 200
        p1 = page1.json()
        assert p1["total_blocks"] == 256
        assert len(p1["blocks"]) == 32
        assert p1["blocks"][0]["classification"] == "reserved"
        assert p1["blocks"][1]["classification"] == "primary_superblock"
        assert p1["blocks"][2]["classification"] == "backup_superblock"

        # Inspect single block & out-of-bounds
        blk0 = client.get(f"/api/v1/disks/{disk_name}/blocks/0")
        assert blk0.status_code == 200
        assert blk0.json()["byte_offset"] == 0
        oob = client.get(f"/api/v1/disks/{disk_name}/blocks/9999")
        assert oob.status_code == 400

        # Fragmentation report
        frag = client.get(f"/api/v1/disks/{disk_name}/fragmentation")
        assert frag.status_code == 200
        rep = frag.json()
        assert rep["total_files"] == 1
        assert rep["internal_fragmentation_bytes"] == 2048 - 1500
        assert rep["files"][0]["path"] == "/alpha.bin"

        # Switch policy
        pol = client.patch(
            f"/api/v1/disks/{disk_name}/policy",
            json={"alloc_policy": "worst_fit"},
        )
        assert pol.status_code == 200
        assert pol.json()["alloc_policy"] == "worst_fit"


def test_alloc_comparison_endpoint_runs_all_four_policies(isolated_workspace: Path) -> None:
    disk_name = "cmp_base.gfs"
    with TestClient(app) as client:
        assert (
            client.post(
                "/api/v1/disks",
                json={"name": disk_name, "size_bytes": 128 * 1024, "block_size": 1024},
            ).status_code
            == 201
        )
        res = client.post(
            f"/api/v1/disks/{disk_name}/alloc-compare",
            json={
                "block_size": 1024,
                "total_blocks": 128,
                "use_active_disk": True,
                "workload": [
                    {"op": "create", "path": "/a.bin", "size_bytes": 4096},
                    {"op": "create", "path": "/b.bin", "size_bytes": 2048},
                    {"op": "create", "path": "/c.bin", "size_bytes": 6144},
                    {"op": "delete", "path": "/a.bin", "size_bytes": 0},
                    {"op": "create", "path": "/d.bin", "size_bytes": 3072},
                ],
            },
        )
        assert res.status_code == 200
        data = res.json()
        assert data["experiment_mode"] == "cloned_active_disk"
        assert len(data["policies"]) == 4
        names = {p["policy"] for p in data["policies"]}
        assert names == {"first_fit", "best_fit", "worst_fit", "bitmap_next_fit"}

        # Original disk must remain untouched (empty root)
        tree = client.get(f"/api/v1/disks/{disk_name}/fs/tree?path=/")
        assert tree.status_code == 200
        assert tree.json()["entries"] == []
