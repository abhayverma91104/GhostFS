from __future__ import annotations

import json
import re
from typing import Any, Dict

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse, JSONResponse

from schemas.reports import ReportRequest
from services.reporting.evidence_manager import gather_available_evidence
from services.reporting.report_builder import (
    delete_report,
    generate_report,
    get_report_manifest,
    list_reports,
    preview_report,
    resolve_report_dir,
    verify_report_integrity,
)

router = APIRouter(prefix="/reports", tags=["reports"])

_EXPORT_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


@router.get("/evidence")
async def get_available_evidence() -> Dict[str, Any]:
    ev = await gather_available_evidence()
    reports = list_reports()
    return {**ev, "reports_count": len(reports)}


@router.post("/preview")
async def preview_report_endpoint(req: ReportRequest) -> Dict[str, Any]:
    try:
        return await preview_report(
            {
                "report_type": req.report_type,
                "disk_name": req.disk_name,
                "title": req.title,
                "case_id": req.case_id,
                "investigator": req.investigator,
                "notes": req.notes,
                "experiment_id": req.experiment_id,
                "recovery_scan_mode": req.recovery_scan_mode,
                "scheduling_payload": req.scheduling_payload,
                "binary_payload": req.binary_payload,
                "include_sections": req.include_sections,
                "expected_source_sha256": req.expected_source_sha256,
            }
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.post("/generate")
async def generate_report_endpoint(req: ReportRequest) -> Dict[str, Any]:
    try:
        return await generate_report(
            report_type=req.report_type,
            disk_name=req.disk_name,
            title=req.title,
            case_id=req.case_id,
            investigator=req.investigator,
            notes=req.notes,
            experiment_id=req.experiment_id,
            recovery_scan_mode=req.recovery_scan_mode,
            scheduling_payload=req.scheduling_payload,
            binary_payload=req.binary_payload,
            include_sections=req.include_sections,
            expected_source_sha256=req.expected_source_sha256,
            export_pdf=req.export_pdf,
            export_json=req.export_json,
            export_csv=req.export_csv,
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.get("")
def list_reports_endpoint() -> Dict[str, Any]:
    return {"reports": list_reports()}


@router.get("/{report_id}")
def get_report_endpoint(report_id: str) -> Dict[str, Any]:
    try:
        manifest = get_report_manifest(report_id)
        rdir = resolve_report_dir(report_id)
        ev_file = rdir / "evidence.json"
        evidence = json.loads(ev_file.read_text(encoding="utf-8")) if ev_file.is_file() else None
        return {"manifest": manifest, "snapshot": evidence}
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc


@router.get("/{report_id}/manifest")
def download_manifest_endpoint(report_id: str) -> JSONResponse:
    try:
        manifest = get_report_manifest(report_id)
        return JSONResponse(
            content=manifest,
            headers={"Content-Disposition": f'attachment; filename="{report_id}.manifest.json"'},
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc


@router.get("/{report_id}/download/pdf")
def download_pdf_endpoint(report_id: str) -> FileResponse:
    try:
        rdir = resolve_report_dir(report_id)
        pdf_path = rdir / "report.pdf"
        if not pdf_path.is_file() or pdf_path.is_symlink():
            raise HTTPException(status_code=404, detail="PDF artifact not found")
        return FileResponse(
            path=str(pdf_path),
            filename=f"{report_id}.pdf",
            media_type="application/pdf",
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.get("/{report_id}/download/json")
def download_json_endpoint(report_id: str) -> FileResponse:
    try:
        rdir = resolve_report_dir(report_id)
        j_path = rdir / "evidence.json"
        if not j_path.is_file() or j_path.is_symlink():
            raise HTTPException(status_code=404, detail="JSON artifact not found")
        return FileResponse(
            path=str(j_path),
            filename=f"{report_id}.evidence.json",
            media_type="application/json",
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.get("/{report_id}/exports")
def list_csv_exports_endpoint(report_id: str) -> Dict[str, Any]:
    try:
        manifest = get_report_manifest(report_id)
        return {"exports": manifest.get("artifacts", {}).get("csv_exports", [])}
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc


@router.get("/{report_id}/exports/{export_id}")
def download_csv_export_endpoint(report_id: str, export_id: str) -> FileResponse:
    if not _EXPORT_ID_RE.match(export_id):
        raise HTTPException(status_code=400, detail="Invalid export_id")
    try:
        rdir = resolve_report_dir(report_id)
        csv_path = (rdir / "exports" / f"{export_id}.csv").resolve()
        if not str(csv_path).startswith(str((rdir / "exports").resolve())) or not csv_path.is_file() or csv_path.is_symlink():
            raise HTTPException(status_code=404, detail="CSV export not found")
        return FileResponse(
            path=str(csv_path),
            filename=f"{report_id}_{export_id}.csv",
            media_type="text/csv; charset=utf-8",
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.post("/{report_id}/verify")
def verify_report_endpoint(report_id: str) -> Dict[str, Any]:
    try:
        return verify_report_integrity(report_id)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc


@router.delete("/{report_id}")
def delete_report_endpoint(report_id: str, confirm: bool = Query(False)) -> Dict[str, Any]:
    try:
        return delete_report(report_id, confirm)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except FileNotFoundError as exc:
        raise HTTPException(status_code=404, detail=str(exc)) from exc
