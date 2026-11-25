from __future__ import annotations

import asyncio
import json
from typing import Any, Dict

from app.config import settings


class EngineExecutionError(RuntimeError):
    """Raised when invoking ghostfs_engine fails or returns an error payload."""

    def __init__(self, code: str, message: str, status_code: int = 500) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.status_code = status_code


async def run_engine(command: str, payload: Dict[str, Any] | None = None) -> Dict[str, Any]:
    """Execute the compiled C++17 engine safely via subprocess with JSON stdin/stdout."""
    if not settings.engine_bin.exists():
        raise EngineExecutionError(
            "ERR_ENGINE_NOT_BUILT",
            f"Compiled C++ engine binary not found at {settings.engine_bin}",
            status_code=503,
        )

    request_obj: Dict[str, Any] = {
        "command": command,
        "workspace": str(settings.workspace_root),
    }
    if payload:
        request_obj.update(payload)

    stdin_bytes = json.dumps(request_obj).encode("utf-8")

    proc = await asyncio.create_subprocess_exec(
        str(settings.engine_bin),
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )

    try:
        stdout_bytes, stderr_bytes = await asyncio.wait_for(
            proc.communicate(input=stdin_bytes),
            timeout=settings.engine_timeout_sec,
        )
    except asyncio.TimeoutError as exc:
        proc.kill()
        await proc.wait()
        raise EngineExecutionError(
            "ERR_ENGINE_TIMEOUT",
            f"Engine command '{command}' timed out after {settings.engine_timeout_sec}s",
            status_code=504,
        ) from exc

    raw_stdout = stdout_bytes.decode("utf-8", errors="replace").strip()
    if not raw_stdout:
        stderr_text = stderr_bytes.decode("utf-8", errors="replace").strip()
        raise EngineExecutionError(
            "ERR_ENGINE_EMPTY_OUTPUT",
            f"Engine produced no JSON output (rc={proc.returncode}): {stderr_text}",
            status_code=502,
        )

    try:
        parsed = json.loads(raw_stdout)
    except json.JSONDecodeError as exc:
        raise EngineExecutionError(
            "ERR_ENGINE_INVALID_JSON",
            f"Engine emitted invalid JSON: {exc}",
            status_code=502,
        ) from exc

    if not parsed.get("ok", False):
        err = parsed.get("error") or {}
        code = str(err.get("code", "ERR_ENGINE_FAILURE"))
        message = str(err.get("message", "Unknown C++ engine failure"))
        status_map = {
            "ERR_NOT_FOUND": 404,
            "ERR_ALREADY_EXISTS": 409,
            "ERR_DIR_NOT_EMPTY": 409,
            "ERR_NO_SPACE": 409,
            "ERR_NO_FREE_INODES": 409,
            "ERR_DIRTY_FILESYSTEM": 409,
            "ERR_STALE_SCAN": 409,
            "ERR_UNRECOVERABLE": 422,
            "ERR_OUT_OF_BOUNDS": 400,
            "ERR_INVALID_JSON": 400,
            "ERR_BAD_PARAMS": 400,
            "ERR_INVALID_DISK_NAME": 400,
            "ERR_INVALID_BLOCK_SIZE": 400,
            "ERR_INVALID_SIZE": 400,
        }
        status_code = status_map.get(code, 400 if proc.returncode == 1 else 500)
        raise EngineExecutionError(code, message, status_code=status_code)

    return parsed.get("data", {})
