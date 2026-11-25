from __future__ import annotations

from typing import List, Literal, Optional, Union

from pydantic import BaseModel, Field


class IoRequestInput(BaseModel):
    request_id: str = Field(default="", max_length=64)
    cylinder: int = Field(..., ge=0)
    logical_block: int = Field(default=0, ge=0)
    has_logical_block: bool = False
    op_type: Literal["read", "write"] = "read"
    arrival_time: int = Field(default=0, ge=0)
    label: str = Field(default="", max_length=128)


class SimulateRequest(BaseModel):
    algorithm: Literal["FCFS", "SSTF", "SCAN", "C-SCAN", "LOOK", "C-LOOK"] = "FCFS"
    min_cylinder: int = Field(default=0, ge=0)
    max_cylinder: int = Field(default=199, ge=0)
    initial_head: int = Field(default=53, ge=0)
    initial_direction: Literal["left", "right"] = "right"
    scheduling_mode: Literal["static", "dynamic"] = "static"
    seek_time_per_cylinder_ms: float = Field(default=0.0, ge=0.0, le=100.0)
    benchmark_repetitions: int = Field(default=1, ge=1, le=50)
    requests: List[Union[int, IoRequestInput]] = Field(default_factory=list, max_length=10000)


class CompareRequest(BaseModel):
    min_cylinder: int = Field(default=0, ge=0)
    max_cylinder: int = Field(default=199, ge=0)
    initial_head: int = Field(default=53, ge=0)
    initial_direction: Literal["left", "right"] = "right"
    scheduling_mode: Literal["static", "dynamic"] = "static"
    seek_time_per_cylinder_ms: float = Field(default=0.0, ge=0.0, le=100.0)
    benchmark_repetitions: int = Field(default=1, ge=1, le=50)
    requests: List[Union[int, IoRequestInput]] = Field(default_factory=list, max_length=10000)


class GenerateWorkloadRequest(BaseModel):
    pattern: Literal["sequential", "random", "clustered", "mixed", "hotspot", "fragmented"] = "random"
    count: int = Field(default=16, ge=1, le=500)
    min_cylinder: int = Field(default=0, ge=0)
    max_cylinder: int = Field(default=199, ge=0)
    seed: int = Field(default=42, ge=0)


class FileWorkloadRequest(BaseModel):
    file_paths: List[str] = Field(default_factory=list)
    include_metadata_blocks: bool = False
    blocks_per_cylinder: int = Field(default=1, ge=1, le=1024)


class RecoveryWorkloadRequest(BaseModel):
    candidate_id: str = Field(default="", max_length=128)
    enable_txt_heuristic: bool = True
    blocks_per_cylinder: int = Field(default=1, ge=1, le=1024)


class MetadataWorkloadRequest(BaseModel):
    blocks_per_cylinder: int = Field(default=1, ge=1, le=1024)


class AllocWorkloadStepInput(BaseModel):
    op: Literal["create", "delete"]
    path: str = Field(..., min_length=1, max_length=255)
    size_bytes: int = Field(default=0, ge=0)


class AllocSchedulingCompareRequest(BaseModel):
    disk_name: Optional[str] = None
    block_size: Literal[512, 1024, 2048, 4096] = 4096
    total_blocks: int = Field(default=128, ge=64, le=8192)
    initial_head: int = Field(default=0, ge=0)
    initial_direction: Literal["left", "right"] = "right"
    blocks_per_cylinder: int = Field(default=1, ge=1, le=1024)
    workload: List[AllocWorkloadStepInput] = Field(..., min_length=1, max_length=64)
