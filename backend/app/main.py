from __future__ import annotations

from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse

from app.config import settings
from routes.allocator import router as allocator_router
from routes.corruption import router as corruption_router
from routes.disks import router as disks_router
from routes.filesystem import router as filesystem_router
from routes.health import router as health_router
from routes.hexviewer import router as hex_router
from routes.recovery import router as recovery_router
from routes.reports import router as reports_router
from routes.scheduling import router as scheduling_router
from services.engine_runner import EngineExecutionError


@asynccontextmanager
async def lifespan(_app: FastAPI):
    settings.ensure_runtime_dirs()
    yield


app = FastAPI(
    title="GhostFS API",
    description="Digital Forensics & Custom File System Recovery Engine REST API",
    version="0.1.0",
    lifespan=lifespan,
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=[
        "http://localhost:5173",
        "http://127.0.0.1:5173",
    ],
    allow_credentials=True,
    allow_methods=["*"],
    allow_headers=["*"],
)


@app.exception_handler(EngineExecutionError)
async def engine_error_handler(_request: Request, exc: EngineExecutionError) -> JSONResponse:
    return JSONResponse(
        status_code=exc.status_code,
        content={"ok": False, "error": {"code": exc.code, "message": exc.message}},
    )


app.include_router(health_router, prefix="/api/v1")
app.include_router(disks_router, prefix="/api/v1")
app.include_router(filesystem_router, prefix="/api/v1")
app.include_router(allocator_router, prefix="/api/v1")
app.include_router(recovery_router, prefix="/api/v1")
app.include_router(hex_router, prefix="/api/v1")
app.include_router(corruption_router, prefix="/api/v1")
app.include_router(scheduling_router, prefix="/api/v1")
app.include_router(reports_router, prefix="/api/v1")
