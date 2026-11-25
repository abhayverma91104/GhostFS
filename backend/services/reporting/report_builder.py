from __future__ import annotations

import asyncio
import json
import os
import re
import shutil
from pathlib import Path
from typing import Any, Dict, List, Optional

from app.config import settings
from services.reporting.csv_exporter import export_csv_tables
from services.reporting.evidence_manager import (
    REPORT_SCHEMA_VERSION,
    build_evidence_snapshot,
    new_report_id,
    sha256_file,
)
from services.reporting.json_exporter import export_json_evidence
from services.reporting.pdf_generator import generate_pdf_report

_REPORT_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
_REPORT_SEM = asyncio.Semaphore(2)


def validate_report_id(report_id: str) -> str:
    if not _REPORT_ID_RE.match(report_id):
        raise ValueError("Invalid report_id")
    return report_id


def generated_reports_dir() -> Path:
    d = settings.reports_dir / "generated"
    d.mkdir(parents=True, exist_ok=True)
    return d


def resolve_report_dir(report_id: str) -> Path:
    rid = validate_report_id(report_id)
    base = generated_reports_dir().resolve()
    target = (base / rid).resolve()
    if not str(target).startswith(str(base) + os.sep) or target.is_symlink():
        raise ValueError("Invalid report path")
    return target


async def preview_report(params: Dict[str, Any]) -> Dict[str, Any]:
    snapshot = await build_evidence_snapshot(**params)
    snapshot["report_id"] = "preview"
    return snapshot


async def generate_report(
    *,
    export_pdf: bool = True,
    export_json: bool = True,
    export_csv: bool = True,
    **params: Any,
) -> Dict[str, Any]:
    async with _REPORT_SEM:
        report_id = new_report_id()
        report_dir = resolve_report_dir(report_id)
        if report_dir.exists():
            raise RuntimeError("Report ID collision; retry generation")

        tmp_dir = generated_reports_dir() / f".tmp_{report_id}"
        tmp_dir.mkdir(parents=True, exist_ok=True)
        try:
            snapshot = await build_evidence_snapshot(**params)
            snapshot["report_id"] = report_id

            artifacts: Dict[str, Any] = {}
            if export_json:
                artifacts["json"] = export_json_evidence(snapshot, tmp_dir)
            if export_csv:
                artifacts["csv_exports"] = export_csv_tables(snapshot, tmp_dir)
            if export_pdf:
                artifacts["pdf"] = await asyncio.to_thread(generate_pdf_report, snapshot, tmp_dir)

            manifest = {
                "report_id": report_id,
                "report_type": snapshot["report_type"],
                "title": snapshot["title"],
                "case_id": snapshot["case_id"],
                "investigator": snapshot["investigator"],
                "created_at_utc": snapshot["created_at_utc"],
                "schema_version": REPORT_SCHEMA_VERSION,
                "engine_version": snapshot["engine_version"],
                "source_disk_name": snapshot["source_disk_name"],
                "source_disk_sha256": snapshot["source_disk_sha256"],
                "experiment_id": snapshot["experiment_id"],
                "snapshot_sha256": snapshot["snapshot_sha256"],
                "status": "completed",
                "warnings": snapshot["warnings"],
                "artifacts": artifacts,
            }
            m_tmp = tmp_dir / ".manifest.json.tmp"
            m_tmp.write_text(json.dumps(manifest, indent=2, allow_nan=False) + "\n", encoding="utf-8")
            os.replace(m_tmp, tmp_dir / "manifest.json")

            os.replace(tmp_dir, report_dir)
            return manifest
        except Exception:
            shutil.rmtree(tmp_dir, ignore_errors=True)
            raise


def list_reports() -> List[Dict[str, Any]]:
    base = generated_reports_dir()
    out: List[Dict[str, Any]] = []
    for entry in sorted(base.iterdir(), reverse=True):
        if not entry.is_dir() or entry.name.startswith(".") or entry.is_symlink():
            continue
        mf = entry / "manifest.json"
        if mf.is_file() and not mf.is_symlink():
            try:
                out.append(json.loads(mf.read_text(encoding="utf-8")))
            except Exception:
                continue
    return out


def get_report_manifest(report_id: str) -> Dict[str, Any]:
    rdir = resolve_report_dir(report_id)
    mf = rdir / "manifest.json"
    if not mf.is_file() or mf.is_symlink():
        raise FileNotFoundError(f"Report not found: {report_id}")
    return json.loads(mf.read_text(encoding="utf-8"))


def verify_report_integrity(report_id: str) -> Dict[str, Any]:
    rdir = resolve_report_dir(report_id)
    manifest = get_report_manifest(report_id)
    artifacts = manifest.get("artifacts", {})
    checks: List[Dict[str, Any]] = []
    all_ok = True

    for key in ("pdf", "json"):
        if key in artifacts:
            meta = artifacts[key]
            fpath = rdir / meta["filename"]
            exists = fpath.is_file() and not fpath.is_symlink()
            actual_sha = sha256_file(fpath) if exists else ""
            matched = exists and (actual_sha == meta["sha256"])
            if not matched:
                all_ok = False
            checks.append(
                {
                    "artifact": meta["filename"],
                    "expected_sha256": meta["sha256"],
                    "actual_sha256": actual_sha,
                    "matched": matched,
                }
            )

    for csv_meta in artifacts.get("csv_exports", []):
        fpath = rdir / "exports" / csv_meta["filename"]
        exists = fpath.is_file() and not fpath.is_symlink()
        actual_sha = sha256_file(fpath) if exists else ""
        matched = exists and (actual_sha == csv_meta["sha256"])
        if not matched:
            all_ok = False
        checks.append(
            {
                "artifact": csv_meta["relative_path"],
                "expected_sha256": csv_meta["sha256"],
                "actual_sha256": actual_sha,
                "matched": matched,
            }
        )

    return {
        "report_id": report_id,
        "verified": all_ok,
        "checks": checks,
    }


def delete_report(report_id: str, confirm: bool) -> Dict[str, Any]:
    if not confirm:
        raise ValueError("Explicit confirmation (confirm=true) is required to delete a report")
    rdir = resolve_report_dir(report_id)
    if not rdir.exists():
        raise FileNotFoundError(f"Report not found: {report_id}")
    shutil.rmtree(rdir)
    return {"deleted": True, "report_id": report_id}
