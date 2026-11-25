from __future__ import annotations

import os
from pathlib import Path


def _default_workspace_root() -> Path:
    return Path(__file__).resolve().parents[2]


class Settings:
    """Resolved workspace paths and compiled C++ engine location."""

    def __init__(self) -> None:
        env_root = os.environ.get("GHOSTFS_WORKSPACE_ROOT")
        self.workspace_root: Path = (
            Path(env_root).resolve() if env_root else _default_workspace_root()
        )
        self.disk_images_dir: Path = self.workspace_root / "disk_images"
        self.sandbox_dir: Path = self.disk_images_dir / "sandbox"
        self.reports_dir: Path = self.workspace_root / "reports"
        self.recovered_dir: Path = self.reports_dir / "recovered"

        env_engine = os.environ.get("GHOSTFS_ENGINE_BIN")
        self.engine_bin: Path = (
            Path(env_engine).resolve()
            if env_engine
            else self.workspace_root / "engine" / "build" / "ghostfs_engine"
        )
        self.engine_timeout_sec: float = float(
            os.environ.get("GHOSTFS_ENGINE_TIMEOUT_SEC", "15")
        )

    def ensure_runtime_dirs(self) -> None:
        self.disk_images_dir.mkdir(parents=True, exist_ok=True)
        self.sandbox_dir.mkdir(parents=True, exist_ok=True)
        self.reports_dir.mkdir(parents=True, exist_ok=True)
        self.recovered_dir.mkdir(parents=True, exist_ok=True)


settings = Settings()
