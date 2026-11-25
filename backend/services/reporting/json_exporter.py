from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Dict

from services.reporting.evidence_manager import sha256_file


def export_json_evidence(snapshot: Dict[str, Any], out_dir: Path) -> Dict[str, Any]:
    out_path = out_dir / "evidence.json"
    tmp_path = out_dir / ".evidence.json.tmp"
    payload = json.dumps(snapshot, indent=2, allow_nan=False) + "\n"
    tmp_path.write_text(payload, encoding="utf-8")
    os.replace(tmp_path, out_path)
    return {
        "filename": "evidence.json",
        "size_bytes": out_path.stat().st_size,
        "sha256": sha256_file(out_path),
    }
