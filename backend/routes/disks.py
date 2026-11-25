from __future__ import annotations

from typing import Any, Dict

from fastapi import APIRouter, HTTPException, Query

from app.config import settings
from schemas.disks import DiskCreateRequest, DiskFormatRequest, normalize_disk_name
from services.engine_runner import run_engine

router = APIRouter(prefix="/disks", tags=["disks"])


def _safe_disk_name(disk_name: str) -> str:
    try:
        return normalize_disk_name(disk_name)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.get("")
async def list_disks() -> Dict[str, Any]:
    settings.ensure_runtime_dirs()
    return await run_engine("disk_list")


@router.post("", status_code=201)
async def create_disk(body: DiskCreateRequest) -> Dict[str, Any]:
    settings.ensure_runtime_dirs()
    if body.size_bytes % body.block_size != 0:
        raise HTTPException(
            status_code=400,
            detail="size_bytes must be an exact multiple of block_size",
        )
    return await run_engine(
        "disk_create",
        {
            "disk_name": body.name,
            "total_bytes": body.size_bytes,
            "block_size": body.block_size,
            "label": body.label,
            "alloc_policy": body.alloc_policy,
            "inode_count": body.inode_count,
            "overwrite": body.overwrite,
        },
    )


@router.get("/{disk_name}")
async def get_disk_info(disk_name: str) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine("disk_info", {"disk_name": safe_name})


@router.post("/{disk_name}/open")
async def open_disk(disk_name: str) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine("disk_info", {"disk_name": safe_name})


@router.post("/{disk_name}/format")
async def format_disk(disk_name: str, body: DiskFormatRequest) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "disk_format",
        {
            "disk_name": safe_name,
            "block_size": body.block_size,
            "label": body.label,
            "alloc_policy": body.alloc_policy,
            "inode_count": body.inode_count,
        },
    )


@router.delete("/{disk_name}")
async def delete_disk(
    disk_name: str,
    confirm: bool = Query(default=False),
) -> Dict[str, Any]:
    if not confirm:
        raise HTTPException(
            status_code=400,
            detail="Deleting a virtual disk requires ?confirm=true",
        )
    safe_name = _safe_disk_name(disk_name)
    return await run_engine("disk_delete", {"disk_name": safe_name})
