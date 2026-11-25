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


def test_corruption_lab_lifecycle_check_plan_repair_and_immutability() -> None:
    client = TestClient(app)
    disk_name = "pytest_phase6_lab.gfs"
    disk_path = settings.disk_images_dir / disk_name
    sb_dir = settings.disk_images_dir / "sandbox"

    for p in (disk_path, Path(f"{disk_path}.lock")):
        if p.exists():
            p.unlink()

    exp_id = None
    try:
        # 1. Create & populate original disk
        assert client.post(
            "/api/v1/disks",
            json={"name": disk_name, "size_bytes": 524288, "block_size": 4096},
        ).status_code == 201
        assert client.post(
            f"/api/v1/disks/{disk_name}/format",
            json={"volume_label": "Phase6Lab", "alloc_policy": "first_fit", "inode_count": 64},
        ).status_code == 200
        assert client.post(
            f"/api/v1/disks/{disk_name}/fs/files",
            json={"path": "/evidence.txt", "content": "GhostFS Phase 6 Lab Evidence", "overwrite": True},
        ).status_code == 201

        orig_sha = _sha256_file(disk_path)

        # 2. Create sandbox experiment
        c_res = client.post(
            "/api/v1/corruption/experiments",
            json={"source_disk_name": disk_name, "sandbox_name": "pytest_sb"},
        )
        assert c_res.status_code == 200, c_res.text
        exp = c_res.json()
        exp_id = exp["experiment_id"]
        assert exp["original_sha256"] == orig_sha
        assert exp["initial_sandbox_sha256"] == orig_sha

        # 3. Confirmation required for injection
        bad_inj = client.post(
            f"/api/v1/corruption/experiments/{exp_id}/inject",
            json={"category": "primary_superblock", "scenario": "corrupt_magic", "confirm": False},
        )
        assert bad_inj.status_code == 400

        # 4. Inject primary superblock corruption
        inj = client.post(
            f"/api/v1/corruption/experiments/{exp_id}/inject",
            json={"category": "primary_superblock", "scenario": "corrupt_magic", "confirm": True},
        )
        assert inj.status_code == 200, inj.text
        corrupted_sha = inj.json()["current_sandbox_sha256"]
        assert corrupted_sha != orig_sha

        # 5. Consistency check detects primary SB corruption
        chk = client.post(
            f"/api/v1/corruption/experiments/{exp_id}/check",
            json={"mode": "full"},
        )
        assert chk.status_code == 200, chk.text
        chk_data = chk.json()
        assert chk_data["primary_sb_valid"] is False
        assert chk_data["backup_sb_valid"] is True
        assert chk_data["total_issues"] >= 1

        # 6. Generate repair plan
        plan_res = client.post(
            f"/api/v1/corruption/experiments/{exp_id}/repair-plan",
            json={"mode": "full"},
        )
        assert plan_res.status_code == 200, plan_res.text
        plan = plan_res.json()
        assert len(plan["actions"]) >= 1
        assert plan["actions"][0]["action_type"] == "restore_primary_superblock"

        # 7. Dry run does not modify sandbox
        dry_res = client.post(
            f"/api/v1/corruption/experiments/{exp_id}/repair",
            json={
                "plan_id": plan["plan_id"],
                "expected_disk_sha256": plan["disk_sha256_at_plan"],
                "selected_action_ids": [],
                "dry_run": True,
                "confirm": False,
            },
        )
        assert dry_res.status_code == 200, dry_res.text
        assert dry_res.json()["post_repair_sha256"] == corrupted_sha

        # 8. Apply repair
        rep_res = client.post(
            f"/api/v1/corruption/experiments/{exp_id}/repair",
            json={
                "plan_id": plan["plan_id"],
                "expected_disk_sha256": plan["disk_sha256_at_plan"],
                "selected_action_ids": [],
                "dry_run": False,
                "confirm": True,
            },
        )
        assert rep_res.status_code == 200, rep_res.text
        rep = rep_res.json()
        assert rep["issues_remaining"] == 0
        assert rep["post_check"]["consistency_status"] == "clean"
        assert rep["post_check"]["preserved_files_count"] == 1

        # 9. Comparison & Manifest download
        cmp_res = client.get(
            f"/api/v1/corruption/experiments/{exp_id}/comparison?compare_mode=corrupted_vs_repaired"
        )
        assert cmp_res.status_code == 200, cmp_res.text
        assert cmp_res.json()["total_bytes_changed"] > 0

        man_res = client.get(f"/api/v1/corruption/experiments/{exp_id}/manifest")
        assert man_res.status_code == 200

        # 10. Original source disk must be 100% untouched
        assert _sha256_file(disk_path) == orig_sha
    finally:
        if exp_id:
            client.delete(f"/api/v1/corruption/experiments/{exp_id}?confirm=true")
        for p in (disk_path, Path(f"{disk_path}.lock")):
            if p.exists():
                p.unlink()
        if sb_dir.exists():
            pass
