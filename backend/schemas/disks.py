from __future__ import annotations

import re
from typing import Literal
from pydantic import BaseModel, Field, field_validator

_DISK_NAME_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,60}$")

BlockSizeLiteral = Literal[512, 1024, 2048, 4096]
AllocPolicyLiteral = Literal["first_fit", "best_fit", "worst_fit", "bitmap_next_fit"]


def normalize_disk_name(raw_name: str) -> str:
    name = raw_name.strip()
    if not name or "/" in name or "\\" in name or ".." in name:
        raise ValueError("Invalid virtual disk name")
    if not name.endswith(".gfs"):
        name = f"{name}.gfs"
    stem = name[:-4]
    if not _DISK_NAME_RE.match(stem):
        raise ValueError(
            "Disk name may contain only letters, digits, '.', '_', and '-' (1-61 chars)"
        )
    return name


class DiskCreateRequest(BaseModel):
    name: str = Field(..., min_length=1, max_length=64)
    size_bytes: int = Field(..., ge=32768, le=512 * 1024 * 1024)
    block_size: BlockSizeLiteral = 4096
    label: str = Field(default="GhostFS", min_length=1, max_length=23)
    alloc_policy: AllocPolicyLiteral = "first_fit"
    inode_count: int = Field(default=0, ge=0, le=65536)
    overwrite: bool = False

    @field_validator("name")
    @classmethod
    def validate_name(cls, v: str) -> str:
        return normalize_disk_name(v)


class DiskFormatRequest(BaseModel):
    block_size: BlockSizeLiteral = 4096
    label: str = Field(default="GhostFS", min_length=1, max_length=23)
    alloc_policy: AllocPolicyLiteral = "first_fit"
    inode_count: int = Field(default=0, ge=0, le=65536)
