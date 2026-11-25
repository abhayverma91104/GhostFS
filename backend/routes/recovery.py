from __future__ import annotations

import base64
import json
from pathlib import Path
from typing import Any, Dict, List

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse, JSONResponse

from app.config import settings
from schemas.disks import normalize_disk_name
from schemas.recovery import RecoveryExtractRequest, RecoveryScanRequest
from services.engine_runner import run_engine

router = APIRouter(tags=["recovery"])

_SCANS: Dict[str, Dict[str, Any]] = {}
_ARTIFACTS: Dict[str, Dict[str, str]] = {}


def _safe_disk_name(disk_name: str) -> str:
    try:
        return normalize_disk_name(disk_name)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


def _resolve_artifact(artifact_id: str) -> Dict[str, str]:
    if artifact_id in _ARTIFACTS:
        return _ARTIFACTS[artifact_id]
    # Fallback: check reports/recovered/<artifact_id>.manifest.json on disk across restarts.
    if not artifact_id or "/" in artifact_id or "\\" in artifact_id or ".." in artifact_id:
        raise HTTPException(status_code=400, detail="Invalid artifact ID")
    manifest_path = (settings.recovered_dir / f"{artifact_id}.manifest.json").resolve()
    if manifest_path.parent != settings.recovered_dir.resolve() or not manifest_path.exists():
        raise HTTPException(status_code=404, detail="Recovery artifact not found")
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    art_path = (settings.recovered_dir / data["artifact_filename"]).resolve()
    if art_path.parent != settings.recovered_dir.resolve() or not art_path.exists():
        raise HTTPException(status_code=404, detail="Recovered payload file missing")
    rec = {"artifact_path": str(art_path), "manifest_path": str(manifest_path)}
    _ARTIFACTS[artifact_id] = rec
    return rec


@router.post("/disks/{disk_name}/recovery/scan")
async def start_recovery_scan(disk_name: str, body: RecoveryScanRequest) -> Dict[str, Any]:
    settings.ensure_runtime_dirs()
    safe_name = _safe_disk_name(disk_name)
    result = await run_engine(
        "recovery_scan",
        {
            "disk_name": safe_name,
            "mode": body.mode,
            "file_types": body.file_types,
            "enable_txt_heuristic": body.enable_txt_heuristic,
        },
    )
    _SCANS[result["scan_id"]] = result
    return result


@router.get("/disks/{disk_name}/recovery/scans/{scan_id}")
async def get_scan_summary(disk_name: str, scan_id: str) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    scan = _SCANS.get(scan_id)
    if not scan or scan["disk_name"] != safe_name:
        raise HTTPException(status_code=404, detail="Scan ID not found")
    return {k: v for k, v in scan.items() if k != "candidates"}


@router.get("/disks/{disk_name}/recovery/scans/{scan_id}/candidates")
async def list_scan_candidates(
    disk_name: str,
    scan_id: str,
    offset: int = Query(default=0, ge=0),
    limit: int = Query(default=50, ge=1, le=500),
    classification: str = Query(default=""),
    file_type: str = Query(default=""),
    search: str = Query(default=""),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    scan = _SCANS.get(scan_id)
    if not scan or scan["disk_name"] != safe_name:
        raise HTTPException(status_code=404, detail="Scan ID not found")

    cands = scan["candidates"]
    q = search.strip().lower()
    filtered = [
        c
        for c in cands
        if (not classification or c["classification"] == classification)
        and (not file_type or c["file_type"] == file_type)
        and (
            not q
            or q in c["filename"].lower()
            or q in c["original_path"].lower()
            or q in c["candidate_id"].lower()
        )
    ]
    return {
        "scan_id": scan_id,
        "total": len(filtered),
        "offset": offset,
        "limit": limit,
        "candidates": filtered[offset : offset + limit],
    }


@router.get("/disks/{disk_name}/recovery/candidates/{candidate_id}")
async def get_candidate_detail(
    disk_name: str,
    candidate_id: str,
    scan_id: str = Query(...),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    scan = _SCANS.get(scan_id)
    if not scan or scan["disk_name"] != safe_name:
        raise HTTPException(status_code=404, detail="Scan ID not found")
    for c in scan["candidates"]:
        if c["candidate_id"] == candidate_id:
            return c
    raise HTTPException(status_code=404, detail="Candidate not found in scan")


@router.post("/disks/{disk_name}/recovery/extract")
async def extract_candidates(disk_name: str, body: RecoveryExtractRequest) -> Dict[str, Any]:
    settings.ensure_runtime_dirs()
    safe_name = _safe_disk_name(disk_name)
    scan = _SCANS.get(body.scan_id)
    if not scan or scan["disk_name"] != safe_name:
        raise HTTPException(status_code=404, detail="Scan ID not found; run a new scan")

    manifests: List[Dict[str, Any]] = []
    for cid in body.candidate_ids:
        manifest = await run_engine(
            "recovery_extract",
            {
                "disk_name": safe_name,
                "candidate_id": cid,
                "expected_source_sha256": scan["source_disk_sha256"],
                "zero_fill_missing": body.zero_fill_missing,
                "overwrite_existing": body.overwrite_existing,
                "enable_txt_heuristic": body.enable_txt_heuristic,
            },
        )
        _ARTIFACTS[manifest["recovery_id"]] = {
            "artifact_path": manifest["artifact_path"],
            "manifest_path": manifest["manifest_path"],
        }
        manifests.append(manifest)

    return {"extracted_count": len(manifests), "artifacts": manifests}


@router.get("/recovery/artifacts/{artifact_id}")
async def get_artifact_preview(artifact_id: str) -> Dict[str, Any]:
    rec = _resolve_artifact(artifact_id)
    manifest = json.loads(Path(rec["manifest_path"]).read_text(encoding="utf-8"))
    raw = Path(rec["artifact_path"]).read_bytes()

    is_text = False
    text_preview: str | None = None
    is_image = False
    image_mime: str | None = None
    image_base64: str | None = None

    fname_lower = manifest["original_filename"].lower()
    if fname_lower.endswith(".png") and len(raw) <= 4 * 1024 * 1024:
        is_image = True
        image_mime = "image/png"
        image_base64 = base64.b64encode(raw).decode("ascii")
    elif (fname_lower.endswith(".jpg") or fname_lower.endswith(".jpeg")) and len(raw) <= 4 * 1024 * 1024:
        is_image = True
        image_mime = "image/jpeg"
        image_base64 = base64.b64encode(raw).decode("ascii")
    elif len(raw) <= 256 * 1024:
        try:
            decoded = raw.decode("utf-8")
            if "\x00" not in decoded:
                is_text = True
                text_preview = decoded
        except UnicodeDecodeError:
            is_text = False

    return {
        "manifest": manifest,
        "is_text": is_text,
        "text_preview": text_preview,
        "is_image": is_image,
        "image_mime": image_mime,
        "image_base64": image_base64,
    }


@router.get("/recovery/artifacts/{artifact_id}/manifest")
async def download_manifest(artifact_id: str) -> JSONResponse:
    rec = _resolve_artifact(artifact_id)
    manifest = json.loads(Path(rec["manifest_path"]).read_text(encoding="utf-8"))
    return JSONResponse(
        content=manifest,
        headers={
            "Content-Disposition": f'attachment; filename="{artifact_id}.manifest.json"',
            "X-Content-Type-Options": "nosniff",
        },
    )


@router.get("/recovery/artifacts/{artifact_id}/download")
async def download_artifact(artifact_id: str) -> FileResponse:
    rec = _resolve_artifact(artifact_id)
    manifest = json.loads(Path(rec["manifest_path"]).read_text(encoding="utf-8"))
    return FileResponse(
        path=rec["artifact_path"],
        media_type="application/octet-stream",
        filename=manifest["artifact_filename"],
        headers={"X-Content-Type-Options": "nosniff"},
    )
