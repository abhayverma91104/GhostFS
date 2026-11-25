from __future__ import annotations

from typing import List, Literal
from pydantic import BaseModel, Field


class HexInterpretRequest(BaseModel):
    bytes: List[int] = Field(..., min_length=1, max_length=64)


class HexSearchRequest(BaseModel):
    query_type: Literal["hex", "ascii", "utf8", "signature"]
    query_value: str = Field(..., min_length=1, max_length=4096)
    scope: Literal["disk", "block_range", "allocated", "unallocated", "file", "metadata"] = "disk"
    start_block: int = Field(default=0, ge=0)
    end_block: int = Field(default=0, ge=0)
    path: str = Field(default="")
    offset: int = Field(default=0, ge=0)
    limit: int = Field(default=50, ge=1, le=200)


class HexAnalyzeRequest(BaseModel):
    offset: int = Field(..., ge=0)
    length: int = Field(..., ge=0, le=65536)


class HexCompareRequest(BaseModel):
    mode: Literal["blocks", "ranges", "files", "primary_vs_backup_sb", "file_vs_artifact"]
    a_start: int = Field(default=0, ge=0)
    a_len: int = Field(default=0, ge=0, le=65536)
    b_start: int = Field(default=0, ge=0)
    b_len: int = Field(default=0, ge=0, le=65536)
    path_a: str = Field(default="")
    path_b: str = Field(default="")
    artifact_filename: str = Field(default="")
