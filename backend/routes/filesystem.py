from __future__ import annotations

import base64
from pathlib import PurePosixPath
from typing import Any, Dict

from fastapi import APIRouter, File, Form, HTTPException, Query, UploadFile
from fastapi.responses import Response

from schemas.disks import normalize_disk_name
from schemas.filesystem import MkdirRequest, RenameRequest, WriteFileRequest
from services.engine_runner import run_engine

router = APIRouter(prefix="/disks/{disk_name}/fs", tags=["filesystem"])

MAX_UPLOAD_BYTES = 32 * 1024 * 1024  # 32 MiB per upload request


def _safe_disk_name(disk_name: str) -> str:
    try:
        return normalize_disk_name(disk_name)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@router.get("/tree")
async def list_directory(
    disk_name: str,
    path: str = Query(default="/"),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine("fs_list", {"disk_name": safe_name, "path": path})


@router.get("/stat")
async def stat_entry(
    disk_name: str,
    path: str = Query(...),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine("fs_stat", {"disk_name": safe_name, "path": path})


@router.post("/mkdir", status_code=201)
async def make_directory(disk_name: str, body: MkdirRequest) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "fs_mkdir",
        {"disk_name": safe_name, "path": body.path, "permissions": body.permissions},
    )


@router.post("/files", status_code=201)
async def write_file(disk_name: str, body: WriteFileRequest) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    try:
        base64.b64decode(body.content_base64, validate=True)
    except Exception as exc:
        raise HTTPException(status_code=400, detail=f"Invalid base64 payload: {exc}") from exc

    return await run_engine(
        "fs_write",
        {
            "disk_name": safe_name,
            "path": body.path,
            "content_base64": body.content_base64,
            "overwrite": body.overwrite,
            "permissions": body.permissions,
        },
    )


@router.post("/files/upload", status_code=201)
async def upload_file(
    disk_name: str,
    file: UploadFile = File(...),
    dir_path: str = Form(default="/"),
    overwrite: bool = Form(default=True),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    raw_bytes = await file.read()
    if len(raw_bytes) > MAX_UPLOAD_BYTES:
        raise HTTPException(status_code=413, detail="Uploaded file exceeds 32 MiB limit")

    filename = PurePosixPath(file.filename or "upload.bin").name
    if not filename or filename in (".", ".."):
        raise HTTPException(status_code=400, detail="Invalid uploaded filename")

    norm_dir = "/" if not dir_path or dir_path == "/" else dir_path.rstrip("/")
    target_path = f"/{filename}" if norm_dir == "/" else f"{norm_dir}/{filename}"

    encoded = base64.b64encode(raw_bytes).decode("ascii")
    return await run_engine(
        "fs_write",
        {
            "disk_name": safe_name,
            "path": target_path,
            "content_base64": encoded,
            "overwrite": overwrite,
        },
    )


@router.get("/files/read")
async def read_file(
    disk_name: str,
    path: str = Query(...),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    result = await run_engine("fs_read", {"disk_name": safe_name, "path": path})
    raw_bytes = base64.b64decode(result["content_base64"])
    text_preview: str | None = None
    is_text = False
    if len(raw_bytes) <= 256 * 1024:
        try:
            decoded = raw_bytes.decode("utf-8")
            # Treat as text preview when free of NUL bytes
            if "\x00" not in decoded:
                text_preview = decoded
                is_text = True
        except UnicodeDecodeError:
            is_text = False

    result["is_text"] = is_text
    result["text_preview"] = text_preview
    return result


@router.get("/files/download")
async def download_file(
    disk_name: str,
    path: str = Query(...),
) -> Response:
    safe_name = _safe_disk_name(disk_name)
    result = await run_engine("fs_read", {"disk_name": safe_name, "path": path})
    raw_bytes = base64.b64decode(result["content_base64"])
    entry = result["entry"]
    filename = PurePosixPath(entry["name"]).name or "download.bin"
    return Response(
        content=raw_bytes,
        media_type="application/octet-stream",
        headers={
            "Content-Disposition": f'attachment; filename="{filename}"',
            "X-Content-SHA256": str(entry.get("sha256", "")),
        },
    )


@router.post("/rename")
async def rename_entry(disk_name: str, body: RenameRequest) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "fs_rename",
        {"disk_name": safe_name, "old_path": body.old_path, "new_path": body.new_path},
    )


@router.delete("/delete")
async def delete_entry(
    disk_name: str,
    path: str = Query(...),
    recursive: bool = Query(default=False),
) -> Dict[str, Any]:
    safe_name = _safe_disk_name(disk_name)
    return await run_engine(
        "fs_delete",
        {"disk_name": safe_name, "path": path, "recursive": recursive},
    )
