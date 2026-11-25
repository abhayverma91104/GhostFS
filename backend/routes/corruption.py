from __future__ import annotations

import re
from typing import Any, Dict, Literal

from fastapi import APIRouter, HTTPException, Query
from fastapi.responses import FileResponse, JSONResponse

from app.config import settings
from schemas.corruption import (
    ApplyRepairRequest,
    CheckRequest,
    CreateExperimentRequest,
    InjectCorruptionRequest,
    RepairPlanRequest,
)
from schemas.disks import normalize_disk_name
from services.engine_runner import run_engine

router = APIRouter(prefix="/corruption", tags=["corruption"])

_EXP_ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


def _safe_exp_id(exp_id: str) -> str:
    if not _EXP_ID_RE.match(exp_id):
        raise HTTPException(status_code=400, detail="Invalid experiment_id")
    return exp_id


@router.post("/experiments")
async def create_experiment(req: CreateExperimentRequest) -> Dict[str, Any]:
    try:
        src = normalize_disk_name(req.source_disk_name)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    return await run_engine(
        "corruption_create_experiment",
        {"source_disk_name": src, "sandbox_name": req.sandbox_name},
    )


@router.get("/experiments")
async def list_experiments() -> Dict[str, Any]:
    return await run_engine("corruption_list_experiments", {})


@router.get("/experiments/{experiment_id}")
async def get_experiment(experiment_id: str) -> Dict[str, Any]:
    eid = _safe_exp_id(experiment_id)
    return await run_engine("corruption_get_experiment", {"experiment_id": eid})


@router.post("/experiments/{experiment_id}/inject")
async def inject_corruption(experiment_id: str, req: InjectCorruptionRequest) -> Dict[str, Any]:
    eid = _safe_exp_id(experiment_id)
    if not req.confirm:
        raise HTTPException(
            status_code=400,
            detail="Explicit confirmation (confirm=true) is required before injecting corruption",
        )
    return await run_engine(
        "corruption_inject",
        {
            "experiment_id": eid,
            "category": req.category,
            "scenario": req.scenario,
            "target_block": req.target_block,
            "target_inode": req.target_inode,
            "secondary_inode": req.secondary_inode,
            "target_path": req.target_path,
            "severity": req.severity,
            "seed": req.seed,
            "confirm": req.confirm,
        },
    )


@router.post("/experiments/{experiment_id}/check")
async def check_experiment(experiment_id: str, req: CheckRequest) -> Dict[str, Any]:
    eid = _safe_exp_id(experiment_id)
    return await run_engine("corruption_check", {"experiment_id": eid, "mode": req.mode})


@router.post("/experiments/{experiment_id}/repair-plan")
async def generate_repair_plan(experiment_id: str, req: RepairPlanRequest) -> Dict[str, Any]:
    eid = _safe_exp_id(experiment_id)
    return await run_engine("corruption_repair_plan", {"experiment_id": eid, "mode": req.mode})


@router.post("/experiments/{experiment_id}/repair")
async def apply_repair(experiment_id: str, req: ApplyRepairRequest) -> Dict[str, Any]:
    eid = _safe_exp_id(experiment_id)
    if not req.dry_run and not req.confirm:
        raise HTTPException(
            status_code=400,
            detail="Explicit confirmation (confirm=true) is required to apply repairs",
        )
    return await run_engine(
        "corruption_repair",
        {
            "experiment_id": eid,
            "plan_id": req.plan_id,
            "expected_disk_sha256": req.expected_disk_sha256,
            "selected_action_ids": req.selected_action_ids,
            "dry_run": req.dry_run,
            "confirm": req.confirm,
        },
    )


@router.get("/experiments/{experiment_id}/comparison")
async def compare_experiment(
    experiment_id: str,
    compare_mode: Literal[
        "original_vs_corrupted",
        "corrupted_vs_repaired",
        "original_vs_repaired",
    ] = Query("original_vs_corrupted"),
) -> Dict[str, Any]:
    eid = _safe_exp_id(experiment_id)
    return await run_engine(
        "corruption_compare",
        {"experiment_id": eid, "compare_mode": compare_mode},
    )


@router.get("/experiments/{experiment_id}/manifest")
async def download_manifest(experiment_id: str) -> JSONResponse:
    eid = _safe_exp_id(experiment_id)
    data = await run_engine("corruption_get_experiment", {"experiment_id": eid})
    return JSONResponse(
        content=data,
        headers={
            "Content-Disposition": f'attachment; filename="{eid}.exp.json"',
        },
    )


@router.get("/experiments/{experiment_id}/download")
async def download_sandbox_disk(experiment_id: str) -> FileResponse:
    eid = _safe_exp_id(experiment_id)
    exp = await run_engine("corruption_get_experiment", {"experiment_id": eid})
    sb_dir = (settings.disk_images_dir / "sandbox").resolve()
    sb_path = (sb_dir / exp["sandbox_filename"]).resolve()
    if not str(sb_path).startswith(str(sb_dir)) or not sb_path.is_file() or sb_path.is_symlink():
        raise HTTPException(status_code=404, detail="Sandbox disk image not found")
    return FileResponse(
        path=str(sb_path),
        filename=exp["sandbox_filename"],
        media_type="application/octet-stream",
    )


@router.delete("/experiments/{experiment_id}")
async def delete_experiment(
    experiment_id: str,
    confirm: bool = Query(False),
) -> Dict[str, Any]:
    eid = _safe_exp_id(experiment_id)
    if not confirm:
        raise HTTPException(
            status_code=400,
            detail="Explicit confirmation (confirm=true) is required to delete an experiment",
        )
    return await run_engine("corruption_delete_experiment", {"experiment_id": eid, "confirm": True})
