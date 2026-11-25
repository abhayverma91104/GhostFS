from __future__ import annotations

from typing import List
from pydantic import BaseModel


class EngineInfo(BaseModel):
    engine: str
    version: str
    fs_format: str
    cpp_standard: int
    pid: int
    timestamp_ms: int
    workspace: str
    workspace_ready: bool
    modules: List[str]


class HealthResponse(BaseModel):
    status: str
    api_version: str
    engine_reachable: bool
    engine_bin: str
    workspace_root: str
    disk_images_dir: str
    reports_dir: str
    engine_info: EngineInfo
