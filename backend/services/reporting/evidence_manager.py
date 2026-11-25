from __future__ import annotations

import hashlib
import json
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, Optional

from app.config import settings
from schemas.disks import normalize_disk_name
from services.engine_runner import run_engine

REPORT_SCHEMA_VERSION = "1.0.0"


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def canonical_json_hash(obj: Any) -> str:
    encoded = json.dumps(obj, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def new_report_id() -> str:
    ts = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return f"rep_{ts}_{uuid.uuid4().hex[:8]}"


async def gather_available_evidence() -> Dict[str, Any]:
    disks_res = await run_engine("disk_list", {})
    disks = disks_res.get("disks", [])
    exp_res = await run_engine("corruption_list_experiments", {})
    experiments = exp_res.get("experiments", [])

    recovered_dir = settings.reports_dir / "recovered"
    manifests: List[Dict[str, Any]] = []
    if recovered_dir.exists():
        for mf in sorted(recovered_dir.glob("*.manifest.json"), reverse=True):
            try:
                manifests.append(json.loads(mf.read_text(encoding="utf-8")))
            except Exception:
                continue

    return {
        "disks": disks,
        "experiments": experiments,
        "recovery_manifests": manifests,
    }


async def build_evidence_snapshot(
    *,
    report_type: str,
    disk_name: Optional[str],
    title: str,
    case_id: str,
    investigator: str,
    notes: str,
    experiment_id: Optional[str] = None,
    recovery_scan_mode: Optional[str] = None,
    scheduling_payload: Optional[Dict[str, Any]] = None,
    binary_payload: Optional[Dict[str, Any]] = None,
    include_sections: Optional[List[str]] = None,
    expected_source_sha256: Optional[str] = None,
) -> Dict[str, Any]:
    health = await run_engine("ping", {})
    engine_version = str(health.get("version", "0.1.0"))
    warnings: List[str] = []
    evidence: Dict[str, Any] = {}
    source_disk_sha256 = ""
    safe_disk: Optional[str] = None

    if disk_name:
        safe_disk = normalize_disk_name(disk_name)
        disk_path = (settings.disk_images_dir / safe_disk).resolve()
        if not str(disk_path).startswith(str(settings.disk_images_dir.resolve())) or not disk_path.is_file():
            raise ValueError(f"Source disk image not found: {safe_disk}")
        if disk_path.is_symlink():
            raise ValueError("Symlink disk images are not permitted")
        source_disk_sha256 = sha256_file(disk_path)
        if expected_source_sha256 and expected_source_sha256 != source_disk_sha256:
            warnings.append(
                f"STALE EVIDENCE WARNING: Source disk SHA-256 changed since caller reference "
                f"(expected {expected_source_sha256[:12]}..., current {source_disk_sha256[:12]}...)."
            )

    want_all = not include_sections
    sec_set = set(include_sections or [])

    def want(name: str, default_for_type: bool) -> bool:
        if want_all:
            return default_for_type
        return name in sec_set

    # 1. Disk Analysis & Fragmentation
    if safe_disk and want(
        "disk_analysis",
        report_type in ("disk_analysis", "complete_investigation"),
    ):
        disk_info = await run_engine("disk_info", {"disk_name": safe_disk})
        frag = await run_engine("fragmentation_report", {"disk_name": safe_disk})
        bmap = await run_engine(
            "blocks_map",
            {"disk_name": safe_disk, "offset": 0, "limit": 256},
        )
        evidence["disk_info"] = disk_info
        evidence["fragmentation"] = frag
        evidence["block_map_sample"] = bmap.get("blocks", [])[:256]

    # 2. Forensic Recovery
    if safe_disk and want(
        "recovery",
        report_type in ("forensic_recovery", "complete_investigation"),
    ):
        mode = recovery_scan_mode or "combined"
        scan = await run_engine(
            "recovery_scan",
            {"disk_name": safe_disk, "mode": mode, "file_types": [], "enable_txt_heuristic": True},
        )
        recovered_dir = settings.reports_dir / "recovered"
        disk_manifests: List[Dict[str, Any]] = []
        if recovered_dir.exists():
            for mf in sorted(recovered_dir.glob("*.manifest.json"), reverse=True):
                try:
                    m = json.loads(mf.read_text(encoding="utf-8"))
                    if m.get("source_disk_name") == safe_disk:
                        disk_manifests.append(m)
                except Exception:
                    continue
        evidence["recovery_scan"] = scan
        evidence["recovery_manifests"] = disk_manifests[:100]

    # 3. Corruption & Repair
    if experiment_id and want(
        "corruption",
        report_type in ("corruption_repair", "complete_investigation"),
    ):
        exp = await run_engine("corruption_get_experiment", {"experiment_id": experiment_id})
        if safe_disk and exp.get("source_disk_name") != safe_disk:
            warnings.append(
                f"Experiment '{experiment_id}' belongs to source disk '{exp.get('source_disk_name')}', "
                f"not '{safe_disk}'."
            )
        if not source_disk_sha256:
            source_disk_sha256 = exp.get("original_sha256", "")
            safe_disk = exp.get("source_disk_name")
        gfsck = await run_engine("corruption_check", {"experiment_id": experiment_id, "mode": "full"})
        plan = await run_engine("corruption_repair_plan", {"experiment_id": experiment_id, "mode": "full"})
        cmp_data: Optional[Dict[str, Any]] = None
        try:
            cmp_mode = "corrupted_vs_repaired" if exp.get("pre_repair_snapshot_filename") else "original_vs_corrupted"
            cmp_data = await run_engine(
                "corruption_compare",
                {"experiment_id": experiment_id, "compare_mode": cmp_mode},
            )
        except Exception:
            cmp_data = None
        evidence["corruption_experiment"] = exp
        evidence["gfsck"] = gfsck
        evidence["repair_plan"] = plan
        if cmp_data:
            evidence["corruption_comparison"] = cmp_data

    # 4. Disk Scheduling
    if scheduling_payload and want(
        "scheduling",
        report_type in ("disk_scheduling", "complete_investigation"),
    ):
        sched_cmp = await run_engine("scheduling_compare", scheduling_payload)
        evidence["scheduling_comparison"] = sched_cmp
        evidence["scheduling_config"] = {
            "min_cylinder": scheduling_payload.get("min_cylinder", 0),
            "max_cylinder": scheduling_payload.get("max_cylinder", 199),
            "initial_head": scheduling_payload.get("initial_head", 53),
            "initial_direction": scheduling_payload.get("initial_direction", "right"),
            "scheduling_mode": scheduling_payload.get("scheduling_mode", "static"),
            "workload_source": scheduling_payload.get("workload_source", "manual"),
        }
    elif report_type == "disk_scheduling" and not scheduling_payload:
        warnings.append("No scheduling request queue provided; Disk Scheduling section omitted.")

    # 5. Binary Analysis
    if safe_disk and want(
        "binary",
        report_type in ("binary_analysis", "complete_investigation"),
    ):
        b_req = binary_payload or {"source_type": "block", "block_id": 1, "bytes_per_row": 16}
        src_type = b_req.get("source_type", "block")
        hex_read = await run_engine(
            "hex_read",
            {
                "disk_name": safe_disk,
                "source_type": src_type,
                "block_id": b_req.get("block_id", 1),
                "offset": b_req.get("offset", 0),
                "length": min(int(b_req.get("length", 256)), 512),
                "path": b_req.get("path", ""),
                "artifact_filename": b_req.get("artifact_filename", ""),
                "bytes_per_row": 16,
            },
        )
        hex_entropy = await run_engine(
            "hex_analyze",
            {
                "disk_name": safe_disk,
                "source_type": src_type,
                "block_id": b_req.get("block_id", 1),
                "offset": b_req.get("offset", 0),
                "length": min(int(b_req.get("length", 4096)), 65536),
                "path": b_req.get("path", ""),
                "artifact_filename": b_req.get("artifact_filename", ""),
            },
        )
        hex_struct = await run_engine(
            "hex_decode_structure",
            {
                "disk_name": safe_disk,
                "structure_type": b_req.get("structure_type", "superblock"),
                "block_id": b_req.get("block_id", 1),
                "target_id": b_req.get("block_id", 1),
            },
        )
        evidence["binary_hex_excerpt"] = hex_read
        evidence["binary_entropy"] = hex_entropy
        evidence["binary_structure"] = hex_struct

    snapshot_core = {
        "schema_version": REPORT_SCHEMA_VERSION,
        "report_type": report_type,
        "created_at_utc": utc_now_iso(),
        "engine_version": engine_version,
        "title": title.strip() or f"GhostFS {report_type.replace('_', ' ').title()} Report",
        "case_id": case_id.strip(),
        "investigator": investigator.strip(),
        "notes": notes.strip(),
        "source_disk_name": safe_disk or "",
        "source_disk_sha256": source_disk_sha256,
        "experiment_id": experiment_id or "",
        "warnings": warnings,
        "evidence": evidence,
    }
    snapshot_core["snapshot_sha256"] = canonical_json_hash(snapshot_core)
    return snapshot_core
