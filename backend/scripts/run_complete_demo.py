from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.config import settings  # noqa: E402
from services.engine_runner import run_engine  # noqa: E402
from services.reporting.report_builder import generate_report, verify_report_integrity  # noqa: E402


def _sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


async def main() -> int:
    settings.ensure_runtime_dirs()
    disk_name = "demo_complete_investigation.gfs"
    disk_path = settings.disk_images_dir / disk_name
    for p in (disk_path, Path(f"{disk_path}.lock")):
        if p.exists():
            p.unlink()

    exp_id = None
    report_id = None
    try:
        # 1-2. Create, format, mkdir, upload representative files
        await run_engine("disk_create", {"disk_name": disk_name, "total_bytes": 524288, "block_size": 4096})
        await run_engine(
            "disk_format",
            {"disk_name": disk_name, "volume_label": "DemoGFS", "alloc_policy": "best_fit", "inode_count": 64},
        )
        await run_engine("fs_mkdir", {"disk_name": disk_name, "path": "/evidence"})
        payload_bytes = b"GhostFS Complete Investigation Payload Data 2026"
        expected_file_sha = hashlib.sha256(payload_bytes).hexdigest()
        b64 = base64.b64encode(payload_bytes).decode("ascii")
        await run_engine(
            "fs_write",
            {"disk_name": disk_name, "path": "/evidence/secret.txt", "content_base64": b64, "overwrite": True},
        )
        await run_engine(
            "fs_write",
            {"disk_name": disk_name, "path": "/keep.txt", "content_base64": b64, "overwrite": True},
        )

        # 4-8. Fragmentation, delete, recover, verify SHA-256
        frag = await run_engine("fragmentation_report", {"disk_name": disk_name})
        await run_engine("fs_delete", {"disk_name": disk_name, "path": "/evidence/secret.txt"})
        orig_disk_sha = _sha256_file(disk_path)
        scan = await run_engine(
            "recovery_scan",
            {"disk_name": disk_name, "mode": "combined", "file_types": [], "enable_txt_heuristic": True},
        )
        cand_id = scan["candidates"][0]["candidate_id"]
        rec_m = await run_engine(
            "recovery_extract",
            {
                "disk_name": disk_name,
                "candidate_id": cand_id,
                "expected_source_sha256": scan["source_disk_sha256"],
                "zero_fill_missing": False,
                "overwrite_existing": True,
                "enable_txt_heuristic": True,
            },
        )
        assert rec_m["recovered_sha256"] == expected_file_sha

        # 9. Hex inspection
        await run_engine("hex_read", {"disk_name": disk_name, "source_type": "block", "block_id": 1, "bytes_per_row": 16})

        # 10-14. Sandbox corruption & repair
        exp = await run_engine("corruption_create_experiment", {"source_disk_name": disk_name, "sandbox_name": "demo_sb"})
        exp_id = exp["experiment_id"]
        await run_engine(
            "corruption_inject",
            {"experiment_id": exp_id, "category": "primary_superblock", "scenario": "corrupt_magic", "confirm": True},
        )
        plan = await run_engine("corruption_repair_plan", {"experiment_id": exp_id, "mode": "full"})
        rep = await run_engine(
            "corruption_repair",
            {
                "experiment_id": exp_id,
                "plan_id": plan["plan_id"],
                "expected_disk_sha256": plan["disk_sha256_at_plan"],
                "selected_action_ids": [],
                "dry_run": False,
                "confirm": True,
            },
        )
        assert rep["post_check"]["consistency_status"] == "clean"

        # 15-20. Scheduling + Complete Investigation Report + Integrity + Immutability
        manifest = await generate_report(
            report_type="complete_investigation",
            disk_name=disk_name,
            title="GhostFS Complete Investigation Demonstration",
            case_id="DEMO-001",
            investigator="Automated Demo",
            notes="End-to-end Phase 8 verification",
            experiment_id=exp_id,
            scheduling_payload={
                "min_cylinder": 0,
                "max_cylinder": 199,
                "initial_head": 53,
                "initial_direction": "right",
                "requests": [98, 183, 37, 122, 14, 124, 65, 67],
            },
            export_pdf=True,
            export_json=True,
            export_csv=True,
        )
        report_id = manifest["report_id"]
        ver = verify_report_integrity(report_id)
        assert ver["verified"] is True
        assert _sha256_file(disk_path) == orig_disk_sha

        print(
            json.dumps(
                {
                    "ok": True,
                    "disk_sha256": orig_disk_sha,
                    "recovered_sha256_matched": True,
                    "post_repair_status": rep["post_check"]["consistency_status"],
                    "fragmentation_files": frag["total_files"],
                    "report_id": report_id,
                    "report_dir": str(settings.reports_dir / "generated" / report_id),
                    "artifacts_verified": ver["verified"],
                },
                indent=2,
            )
        )
        return 0
    finally:
        if exp_id:
            await run_engine("corruption_delete_experiment", {"experiment_id": exp_id, "confirm": True})
        for p in (disk_path, Path(f"{disk_path}.lock")):
            if p.exists():
                p.unlink()


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
