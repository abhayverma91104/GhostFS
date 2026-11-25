from __future__ import annotations

import hashlib
from pathlib import Path

from fastapi.testclient import TestClient

from app.config import settings
from app.main import app


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def test_scheduling_textbook_compare_and_workloads() -> None:
    client = TestClient(app)

    # 1. Textbook comparison
    cmp_res = client.post(
        "/api/v1/scheduling/compare",
        json={
            "min_cylinder": 0,
            "max_cylinder": 199,
            "initial_head": 53,
            "initial_direction": "right",
            "requests": [98, 183, 37, 122, 14, 124, 65, 67],
        },
    )
    assert cmp_res.status_code == 200, cmp_res.text
    algs = {row["result"]["algorithm"]: row["result"]["total_head_movement"] for row in cmp_res.json()["algorithms"]}
    assert algs == {
        "FCFS": 640,
        "SSTF": 236,
        "SCAN": 331,
        "C-SCAN": 382,
        "LOOK": 299,
        "C-LOOK": 322,
    }

    # 2. Validation error on out-of-range request
    bad = client.post(
        "/api/v1/scheduling/simulate",
        json={"algorithm": "FCFS", "min_cylinder": 0, "max_cylinder": 199, "initial_head": 53, "requests": [300]},
    )
    assert bad.status_code == 400

    # 3. Synthetic workload generation
    gen = client.post(
        "/api/v1/scheduling/workloads/generate",
        json={"pattern": "clustered", "count": 12, "min_cylinder": 0, "max_cylinder": 199, "seed": 77},
    )
    assert gen.status_code == 200
    assert len(gen.json()["requests"]) == 12

    # 4. GFS1 file-based, recovery-based, metadata, and allocation-compare + immutability
    disk_name = "pytest_phase7_sched.gfs"
    disk_path = settings.disk_images_dir / disk_name
    for p in (disk_path, Path(f"{disk_path}.lock")):
        if p.exists():
            p.unlink()
    try:
        assert client.post(
            "/api/v1/disks",
            json={"name": disk_name, "size_bytes": 524288, "block_size": 4096},
        ).status_code == 201
        assert client.post(
            f"/api/v1/disks/{disk_name}/format",
            json={"volume_label": "SchedLab", "alloc_policy": "first_fit", "inode_count": 64},
        ).status_code == 200
        import base64
        b64 = base64.b64encode(b"GhostFS Scheduling Trace Payload").decode("ascii")
        assert client.post(
            f"/api/v1/disks/{disk_name}/fs/files",
            json={"path": "/trace.txt", "content_base64": b64, "overwrite": True},
        ).status_code == 201

        orig_sha = _sha256_file(disk_path)

        fw = client.post(
            f"/api/v1/disks/{disk_name}/scheduling/workloads/files",
            json={"file_paths": ["/trace.txt"], "include_metadata_blocks": True, "blocks_per_cylinder": 1},
        )
        assert fw.status_code == 200, fw.text
        assert len(fw.json()["requests"]) >= 2

        mw = client.post(
            f"/api/v1/disks/{disk_name}/scheduling/workloads/metadata",
            json={"blocks_per_cylinder": 1},
        )
        assert mw.status_code == 200
        assert len(mw.json()["requests"]) == 7

        ac = client.post(
            "/api/v1/scheduling/allocation-compare",
            json={
                "block_size": 4096,
                "total_blocks": 128,
                "initial_head": 0,
                "initial_direction": "right",
                "blocks_per_cylinder": 1,
                "workload": [
                    {"op": "create", "path": "/a.bin", "size_bytes": 8192},
                    {"op": "create", "path": "/b.bin", "size_bytes": 4096},
                    {"op": "delete", "path": "/a.bin", "size_bytes": 0},
                    {"op": "create", "path": "/c.bin", "size_bytes": 12288},
                ],
            },
        )
        assert ac.status_code == 200, ac.text
        assert len(ac.json()["policies"]) == 4

        assert _sha256_file(disk_path) == orig_sha
    finally:
        for p in (disk_path, Path(f"{disk_path}.lock")):
            if p.exists():
                p.unlink()
