from __future__ import annotations

from typing import Any, Dict

from fastapi import APIRouter, HTTPException

from schemas.disks import normalize_disk_name
from schemas.scheduling import (
    AllocSchedulingCompareRequest,
    CompareRequest,
    FileWorkloadRequest,
    GenerateWorkloadRequest,
    MetadataWorkloadRequest,
    RecoveryWorkloadRequest,
    SimulateRequest,
)
from services.engine_runner import run_engine

router = APIRouter(tags=["scheduling"])


def _safe_disk(name: str) -> str:
    try:
        return normalize_disk_name(name)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.post("/scheduling/simulate")
async def simulate_scheduling(req: SimulateRequest) -> Dict[str, Any]:
    return await run_engine(
        "scheduling_simulate",
        {
            "algorithm": req.algorithm,
            "min_cylinder": req.min_cylinder,
            "max_cylinder": req.max_cylinder,
            "initial_head": req.initial_head,
            "initial_direction": req.initial_direction,
            "scheduling_mode": req.scheduling_mode,
            "seek_time_per_cylinder_ms": req.seek_time_per_cylinder_ms,
            "benchmark_repetitions": req.benchmark_repetitions,
            "requests": [r.model_dump() if hasattr(r, "model_dump") else r for r in req.requests],
        },
    )


@router.post("/scheduling/compare")
async def compare_scheduling(req: CompareRequest) -> Dict[str, Any]:
    return await run_engine(
        "scheduling_compare",
        {
            "min_cylinder": req.min_cylinder,
            "max_cylinder": req.max_cylinder,
            "initial_head": req.initial_head,
            "initial_direction": req.initial_direction,
            "scheduling_mode": req.scheduling_mode,
            "seek_time_per_cylinder_ms": req.seek_time_per_cylinder_ms,
            "benchmark_repetitions": req.benchmark_repetitions,
            "requests": [r.model_dump() if hasattr(r, "model_dump") else r for r in req.requests],
        },
    )


@router.post("/scheduling/workloads/generate")
async def generate_workload(req: GenerateWorkloadRequest) -> Dict[str, Any]:
    return await run_engine(
        "scheduling_generate_workload",
        req.model_dump(),
    )


@router.post("/disks/{disk_name}/scheduling/workloads/files")
async def file_workload(disk_name: str, req: FileWorkloadRequest) -> Dict[str, Any]:
    return await run_engine(
        "scheduling_file_workload",
        {"disk_name": _safe_disk(disk_name), **req.model_dump()},
    )


@router.post("/disks/{disk_name}/scheduling/workloads/recovery")
async def recovery_workload(disk_name: str, req: RecoveryWorkloadRequest) -> Dict[str, Any]:
    return await run_engine(
        "scheduling_recovery_workload",
        {"disk_name": _safe_disk(disk_name), **req.model_dump()},
    )


@router.post("/disks/{disk_name}/scheduling/workloads/metadata")
async def metadata_workload(disk_name: str, req: MetadataWorkloadRequest) -> Dict[str, Any]:
    return await run_engine(
        "scheduling_metadata_workload",
        {"disk_name": _safe_disk(disk_name), **req.model_dump()},
    )


@router.post("/scheduling/allocation-compare")
async def allocation_and_scheduling_compare(
    req: AllocSchedulingCompareRequest,
) -> Dict[str, Any]:
    payload = req.model_dump()
    if req.disk_name:
        payload["disk_name"] = _safe_disk(req.disk_name)
    return await run_engine("scheduling_alloc_compare", payload)
