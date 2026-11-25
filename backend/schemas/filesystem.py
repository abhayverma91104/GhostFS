from __future__ import annotations

from pydantic import BaseModel, Field


class MkdirRequest(BaseModel):
    path: str = Field(..., min_length=2, max_length=512)
    permissions: int = Field(default=0o755, ge=0, le=0o777)


class WriteFileRequest(BaseModel):
    path: str = Field(..., min_length=2, max_length=512)
    content_base64: str = Field(default="")
    overwrite: bool = True
    permissions: int = Field(default=0o644, ge=0, le=0o777)


class RenameRequest(BaseModel):
    old_path: str = Field(..., min_length=2, max_length=512)
    new_path: str = Field(..., min_length=2, max_length=512)
