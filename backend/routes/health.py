from __future__ import annotations

from fastapi import APIRouter

from app.config import settings
from schemas.health import EngineInfo, HealthResponse
from services.engine_runner import run_engine

router = APIRouter(tags=["system"])


@router.get("/health", response_model=HealthResponse)
async def get_health() -> HealthResponse:
    settings.ensure_runtime_dirs()
    engine_data = await run_engine("ping")
    return HealthResponse(
        status="ok",
        api_version="0.1.0",
        engine_reachable=True,
        engine_bin=str(settings.engine_bin),
        workspace_root=str(settings.workspace_root),
        disk_images_dir=str(settings.disk_images_dir),
        reports_dir=str(settings.reports_dir),
        engine_info=EngineInfo(**engine_data),
    )
