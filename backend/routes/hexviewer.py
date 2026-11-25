from __future__ import annotations

from typing import Any, Dict

from fastapi import APIRouter, HTTPException, Query

from schemas.disks import normalize_disk_name
from schemas.hexviewer import (
    HexAnalyzeRequest,
    HexCompareRequest,
    HexInterpretRequest,
    HexSearchRequest,
)
from services.engine_runner import run_engine

router = APIRouter(prefix="/disks/{disk_name}/hex", tags=["hexviewer"])


def _safe_disk_name(disk_name: str) -> str:
    try:
        return normalize_disk_name(disk_name)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


def _safe_bpr(bpr: int) -> int:
    if bpr not in (8, 16, 32):
        raise HTTPException(status_code=400, detail="bytes_per_row must be 8, 16, or 32")
    return bpr


@router.get("/range")
async def read_hex_range(
    disk_name: str,
    offset: int = Query(..., ge=0),
    length: int = Query(default=4096, ge=1, le=65536),
    bytes_per_row: int = Query(default=16),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "hex_read",
        {
            "disk_name": safe_name,
            "source_type": "disk_range",
            "offset": offset,
            "length": length,
            "bytes_per_row": _safe_bpr(bytes_per_row),
        },
    )


@router.get("/blocks/{block_id}")
async def read_hex_block(
    disk_name: str,
    block_id: int,
    block_count: int = Query(default=1, ge=1, le=16),
    bytes_per_row: int = Query(default=16),
) -> Dict[str, Any]:
    if block_id < 0:
        raise HTTPException(status_code=400, detail="block_id must be >= 0")
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "hex_read",
        {
            "disk_name": safe_name,
            "source_type": "block",
            "block_id": block_id,
            "block_count": block_count,
            "bytes_per_row": _safe_bpr(bytes_per_row),
        },
    )


@router.get("/files")
async def read_hex_file(
    disk_name: str,
    path: str = Query(...),
    offset: int = Query(default=0, ge=0),
    length: int = Query(default=4096, ge=1, le=65536),
    bytes_per_row: int = Query(default=16),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "hex_read",
        {
            "disk_name": safe_name,
            "source_type": "file",
            "path": path,
            "offset": offset,
            "length": length,
            "bytes_per_row": _safe_bpr(bytes_per_row),
        },
    )


@router.get("/artifacts/{artifact_filename}")
async def read_hex_artifact(
    disk_name: str,
    artifact_filename: str,
    offset: int = Query(default=0, ge=0),
    length: int = Query(default=4096, ge=1, le=65536),
    bytes_per_row: int = Query(default=16),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "hex_read",
        {
            "disk_name": safe_name,
            "source_type": "artifact",
            "artifact_filename": artifact_filename,
            "offset": offset,
            "length": length,
            "bytes_per_row": _safe_bpr(bytes_per_row),
        },
    )


@router.get("/structures")
async def decode_structure(
    disk_name: str,
    block_id: int = Query(..., ge=0),
    force_type: str = Query(default=""),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "hex_decode_structure",
        {"disk_name": safe_name, "block_id": block_id, "force_type": force_type},
    )


@router.post("/interpret")
async def interpret_bytes(disk_name: str, body: HexInterpretRequest) -> Dict[str, Any]:
    _safe_disk_name(disk_name)
    for b in body.bytes:
        if b < 0 or b > 255:
            raise HTTPException(status_code=400, detail="Byte values must be 0..255")
    return await run_engine("hex_interpret", {"bytes": body.bytes})


@router.post("/search")
async def search_hex(disk_name: str, body: HexSearchRequest) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "hex_search",
        {
            "disk_name": safe_name,
            "query_type": body.query_type,
            "query_value": body.query_value,
            "scope": body.scope,
            "start_block": body.start_block,
            "end_block": body.end_block,
            "path": body.path,
            "offset": body.offset,
            "limit": body.limit,
        },
    )


@router.post("/analyze")
async def analyze_hex_range(disk_name: str, body: HexAnalyzeRequest) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "hex_analyze",
        {"disk_name": safe_name, "offset": body.offset, "length": body.length},
    )


@router.post("/compare")
async def compare_hex_sources(disk_name: str, body: HexCompareRequest) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "hex_compare",
        {
            "disk_name": safe_name,
            "mode": body.mode,
            "a_start": body.a_start,
            "a_len": body.a_len,
            "b_start": body.b_start,
            "b_len": body.b_len,
            "path_a": body.path_a,
            "path_b": body.path_b,
            "artifact_filename": body.artifact_filename,
        },
    )
