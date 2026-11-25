from __future__ import annotations

from typing import List, Literal
from pydantic import BaseModel, Field


class RecoveryScanRequest(BaseModel):
    mode: Literal["quick", "deep", "combined"] = "combined"
    file_types: List[Literal["png", "jpeg", "pdf", "txt", "unknown"]] = Field(default_factory=list)
    enable_txt_heuristic: bool = True


class RecoveryExtractRequest(BaseModel):
    scan_id: str = Field(..., min_length=4, max_length=128)
    candidate_ids: List[str] = Field(..., min_length=1, max_length=200)
    zero_fill_missing: bool = False
    overwrite_existing: bool = False
    enable_txt_heuristic: bool = True
