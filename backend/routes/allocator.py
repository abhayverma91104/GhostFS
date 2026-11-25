from __future__ import annotations

from typing import Any, Dict

from fastapi import APIRouter, HTTPException, Query

from schemas.allocator import AllocCompareRequest, SetPolicyRequest
from schemas.disks import normalize_disk_name
from services.engine_runner import run_engine

router = APIRouter(tags=["allocator"])

# In-memory cache of last comparison experiment per workspace/disk for retrieval.
_LAST_EXPERIMENTS: Dict[str, Dict[str, Any]] = {}


def _safe_disk_name(disk_name: str) -> str:
    try:
        return normalize_disk_name(disk_name)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.patch("/disks/{disk_name}/policy")
async def set_allocation_policy(disk_name: str, body: SetPolicyRequest) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "disk_set_policy",
        {"disk_name": safe_name, "alloc_policy": body.alloc_policy},
    )


@router.get("/disks/{disk_name}/blocks/map")
async def get_block_map(
    disk_name: str,
    offset: int = Query(default=0, ge=0),
    limit: int = Query(default=256, ge=1, le=2048),
    type_filter: str = Query(default=""),
    file_inode: int = Query(default=0, ge=0),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "blocks_map",
        {
            "disk_name": safe_name,
            "offset": offset,
            "limit": limit,
            "type_filter": type_filter,
            "file_inode": file_inode,
        },
    )


@router.get("/disks/{disk_name}/blocks/extents")
async def get_free_extents(disk_name: str) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    rep = await run_engine("fragmentation_report", {"disk_name": safe_name})
    return {
        "disk_name": rep["disk_name"],
        "free_blocks": rep["free_blocks"],
        "free_extent_count": rep["free_extent_count"],
        "largest_free_extent": rep["largest_free_extent"],
        "smallest_free_extent": rep["smallest_free_extent"],
        "average_free_extent": rep["average_free_extent"],
        "external_fragmentation_pct": rep["external_fragmentation_pct"],
        "free_extents": rep["free_extents"],
    }


@router.get("/disks/{disk_name}/blocks/{block_id}")
async def inspect_block(disk_name: str, block_id: int) -> Dict[str, Any]:
    if block_id < 0:
        raise HTTPException(status_code=400, detail="block_id must be >= 0")
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "block_inspect",
        {"disk_name": safe_name, "block_id": block_id},
    )


@router.get("/disks/{disk_name}/fragmentation")
async def get_fragmentation_report(disk_name: str) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine("fragmentation_report", {"disk_name": safe_name})


@router.get("/disks/{disk_name}/files/mappings")
async def get_file_mappings(disk_name: str) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    rep = await run_engine("fragmentation_report", {"disk_name": safe_name})
    return {"disk_name": rep["disk_name"], "files": rep["files"]}


@router.post("/disks/{disk_name}/alloc-compare")
async def compare_on_disk_or_fresh(
    disk_name: str,
    body: AllocCompareRequest,
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    res = await run_engine(
        "alloc_compare",
        {
            "disk_name": safe_name if body.use_active_disk else "",
            "block_size": body.block_size,
            "total_blocks": body.total_blocks,
            "workload": [s.model_dump() for s in body.workload],
        },
    )
    _LAST_EXPERIMENTS[safe_name] = res
    return res


@router.get("/disks/{disk_name}/alloc-compare/latest")
async def get_latest_comparison(disk_name: str) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    if safe_name not in _LAST_EXPERIMENTS:
        raise HTTPException(status_code=404, detail="No comparison experiment run yet for this disk")
    return _LAST_EXPERIMENTS[safe_name]


@router.post("/allocator/compare")
async def compare_fresh_disk(body: AllocCompareRequest) -> Dict[str, Any]:
    return await run_engine(
        "alloc_compare",
        {
            "disk_name": "",
            "block_size": body.block_size,
            "total_blocks": body.total_blocks,
            "workload": [s.model_dump() for s in body.workload],
        },
    )
