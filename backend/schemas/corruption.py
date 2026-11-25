from __future__ import annotations

from typing import List, Literal, Optional

from pydantic import BaseModel, Field


class CreateExperimentRequest(BaseModel):
    source_disk_name: str = Field(..., min_length=1, max_length=128)
    sandbox_name: str = Field(default="", max_length=64)


class InjectCorruptionRequest(BaseModel):
    category: Literal[
        "primary_superblock",
        "backup_superblock",
        "dual_superblock",
        "block_bitmap",
        "inode_bitmap",
        "inode",
        "directory",
        "journal",
        "payload",
        "advanced",
    ]
    scenario: str = Field(..., min_length=1, max_length=64)
    target_block: int = Field(default=0, ge=0)
    target_inode: int = Field(default=0, ge=0)
    secondary_inode: int = Field(default=0, ge=0)
    target_path: str = Field(default="", max_length=256)
    severity: int = Field(default=1, ge=1, le=32)
    seed: int = Field(default=42, ge=0)
    confirm: bool = False


class CheckRequest(BaseModel):
    mode: Literal["fast", "full"] = "full"


class RepairPlanRequest(BaseModel):
    mode: Literal["fast", "full"] = "full"


class ApplyRepairRequest(BaseModel):
    plan_id: str = Field(default="", max_length=64)
    expected_disk_sha256: str = Field(..., min_length=64, max_length=64)
    selected_action_ids: List[str] = Field(default_factory=list)
    dry_run: bool = False
    confirm: bool = False
