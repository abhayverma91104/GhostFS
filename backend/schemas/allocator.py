from __future__ import annotations

from typing import List, Literal
from pydantic import BaseModel, Field

from schemas.disks import AllocPolicyLiteral, BlockSizeLiteral


class SetPolicyRequest(BaseModel):
    alloc_policy: AllocPolicyLiteral


class WorkloadStepSchema(BaseModel):
    op: Literal["create", "delete"]
    path: str = Field(..., min_length=2, max_length=256)
    size_bytes: int = Field(default=0, ge=0, le=64 * 1024 * 1024)


class AllocCompareRequest(BaseModel):
    block_size: BlockSizeLiteral = 4096
    total_blocks: int = Field(default=128, ge=64, le=8192)
    use_active_disk: bool = False
    workload: List[WorkloadStepSchema] = Field(..., min_length=1, max_length=64)
