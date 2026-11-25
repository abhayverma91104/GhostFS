from __future__ import annotations

from typing import Any, Dict, List, Literal, Optional

from pydantic import BaseModel, Field

ReportType = Literal[
    "disk_analysis",
    "forensic_recovery",
    "corruption_repair",
    "disk_scheduling",
    "binary_analysis",
    "complete_investigation",
]


class ReportRequest(BaseModel):
    report_type: ReportType = "complete_investigation"
    disk_name: Optional[str] = Field(default=None, max_length=128)
    title: str = Field(default="", max_length=160)
    case_id: str = Field(default="", max_length=64)
    investigator: str = Field(default="", max_length=80)
    notes: str = Field(default="", max_length=2000)
    experiment_id: Optional[str] = Field(default=None, max_length=64)
    recovery_scan_mode: Optional[Literal["quick", "deep", "combined"]] = "combined"
    scheduling_payload: Optional[Dict[str, Any]] = None
    binary_payload: Optional[Dict[str, Any]] = None
    include_sections: Optional[List[str]] = None
    expected_source_sha256: Optional[str] = Field(default=None, max_length=64)
    export_pdf: bool = True
    export_json: bool = True
    export_csv: bool = True
