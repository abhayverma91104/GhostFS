from __future__ import annotations

import csv
import os
from pathlib import Path
from typing import Any, Dict, List

from services.reporting.evidence_manager import sha256_file

_FORMULA_PREFIXES = ("=", "+", "-", "@", "\t", "\r")


def sanitize_csv_cell(value: Any) -> str:
    if value is None:
        return ""
    s = str(value)
    if s.startswith(_FORMULA_PREFIXES):
        return "'" + s
    return s


def _write_csv(
    exports_dir: Path,
    export_id: str,
    filename: str,
    headers: List[str],
    rows: List[List[Any]],
) -> Dict[str, Any]:
    exports_dir.mkdir(parents=True, exist_ok=True)
    out_path = exports_dir / filename
    tmp_path = exports_dir / f".{filename}.tmp"
    with tmp_path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.writer(f, quoting=csv.QUOTE_MINIMAL)
        writer.writerow(headers)
        for row in rows:
            writer.writerow([sanitize_csv_cell(cell) for cell in row])
    os.replace(tmp_path, out_path)
    return {
        "export_id": export_id,
        "filename": filename,
        "relative_path": f"exports/{filename}",
        "row_count": len(rows),
        "size_bytes": out_path.stat().st_size,
        "sha256": sha256_file(out_path),
    }


def export_csv_tables(snapshot: Dict[str, Any], out_dir: Path) -> List[Dict[str, Any]]:
    exports_dir = out_dir / "exports"
    ev = snapshot.get("evidence", {})
    results: List[Dict[str, Any]] = []

    if "block_map_sample" in ev:
        rows = [
            [
                b.get("block_id", 0),
                b.get("byte_offset", 0),
                b.get("allocated", False),
                b.get("classification", ""),
                b.get("inode_id", 0),
                b.get("owner_path", ""),
            ]
            for b in ev["block_map_sample"]
        ]
        results.append(
            _write_csv(
                exports_dir,
                "disk_blocks",
                "disk_blocks.csv",
                ["block_id", "byte_offset", "allocated", "classification", "inode_id", "owner_path"],
                rows,
            )
        )

    if "fragmentation" in ev:
        files = ev["fragmentation"].get("files", [])
        rows = [
            [
                f.get("inode_id", 0),
                f.get("path", ""),
                f.get("size_bytes", 0),
                f.get("allocated_data_blocks", 0),
                f.get("extent_count", 0),
                f.get("is_fragmented", False),
                f"{float(f.get('internal_fragmentation_pct', 0.0)):.2f}",
            ]
            for f in files
        ]
        results.append(
            _write_csv(
                exports_dir,
                "file_metadata",
                "file_metadata.csv",
                [
                    "inode_id",
                    "path",
                    "size_bytes",
                    "allocated_data_blocks",
                    "extent_count",
                    "is_fragmented",
                    "internal_frag_pct",
                ],
                rows,
            )
        )

    if "recovery_scan" in ev:
        cands = ev["recovery_scan"].get("candidates", [])
        rows = [
            [
                c.get("candidate_id", ""),
                c.get("filename", ""),
                c.get("recovery_method", ""),
                c.get("classification", ""),
                c.get("original_size_bytes", 0),
                c.get("recoverable_bytes", 0),
                c.get("integrity_status", ""),
            ]
            for c in cands
        ]
        results.append(
            _write_csv(
                exports_dir,
                "recovery_candidates",
                "recovery_candidates.csv",
                [
                    "candidate_id",
                    "filename",
                    "recovery_method",
                    "classification",
                    "original_size_bytes",
                    "recoverable_bytes",
                    "integrity_status",
                ],
                rows,
            )
        )

    if "recovery_manifests" in ev:
        rows = [
            [
                m.get("recovery_id", ""),
                m.get("original_filename", ""),
                m.get("recovered_size", 0),
                m.get("verified_bytes", 0),
                m.get("unknown_candidate_bytes", 0),
                m.get("missing_bytes", 0),
                m.get("overwritten_bytes", 0),
                m.get("substituted_bytes", 0),
                m.get("classification", ""),
                m.get("recovered_sha256", ""),
            ]
            for m in ev["recovery_manifests"]
        ]
        results.append(
            _write_csv(
                exports_dir,
                "recovery_artifacts",
                "recovery_artifacts.csv",
                [
                    "recovery_id",
                    "original_filename",
                    "recovered_size",
                    "verified_bytes",
                    "unknown_candidate_bytes",
                    "missing_bytes",
                    "overwritten_bytes",
                    "substituted_bytes",
                    "classification",
                    "recovered_sha256",
                ],
                rows,
            )
        )

    if "gfsck" in ev:
        issues = ev["gfsck"].get("issues", [])
        rows = [
            [
                i.get("issue_id", ""),
                i.get("issue_type", ""),
                i.get("severity", ""),
                i.get("affected_structure", ""),
                i.get("affected_block", 0),
                i.get("repairability", ""),
                i.get("description", ""),
            ]
            for i in issues
        ]
        results.append(
            _write_csv(
                exports_dir,
                "corruption_findings",
                "corruption_findings.csv",
                [
                    "issue_id",
                    "issue_type",
                    "severity",
                    "affected_structure",
                    "affected_block",
                    "repairability",
                    "description",
                ],
                rows,
            )
        )

    if "repair_plan" in ev:
        acts = ev["repair_plan"].get("actions", [])
        rows = [
            [
                a.get("action_id", ""),
                a.get("action_type", ""),
                a.get("target_structure", ""),
                a.get("existing_value", ""),
                a.get("proposed_value", ""),
                a.get("repairability", ""),
            ]
            for a in acts
        ]
        results.append(
            _write_csv(
                exports_dir,
                "repair_actions",
                "repair_actions.csv",
                [
                    "action_id",
                    "action_type",
                    "target_structure",
                    "existing_value",
                    "proposed_value",
                    "repairability",
                ],
                rows,
            )
        )

    if "scheduling_comparison" in ev:
        algs = ev["scheduling_comparison"].get("algorithms", [])
        rows = [
            [
                a["result"].get("algorithm", ""),
                a["result"].get("total_head_movement", 0),
                f"{float(a['result'].get('average_movement', 0.0)):.2f}",
                a["result"].get("direction_changes", 0),
                a["result"].get("circular_repositioning_distance", 0),
                f"{float(a.get('reduction_vs_fcfs_pct', 0.0)):.2f}",
                a.get("rank_by_movement", 0),
            ]
            for a in algs
        ]
        results.append(
            _write_csv(
                exports_dir,
                "scheduling_comparison",
                "scheduling_comparison.csv",
                [
                    "algorithm",
                    "total_head_movement",
                    "average_movement",
                    "direction_changes",
                    "circular_repositioning",
                    "reduction_vs_fcfs_pct",
                    "rank",
                ],
                rows,
            )
        )
        if algs:
            steps = algs[0]["result"].get("steps", [])
            t_rows = [
                [
                    s.get("step_index", 0),
                    s.get("step_type", ""),
                    s.get("from_cylinder", 0),
                    s.get("to_cylinder", 0),
                    s.get("movement", 0),
                    s.get("cumulative_movement", 0),
                ]
                for s in steps
            ]
            results.append(
                _write_csv(
                    exports_dir,
                    "scheduling_trace",
                    "scheduling_trace.csv",
                    ["step_index", "step_type", "from_cylinder", "to_cylinder", "movement", "cumulative_movement"],
                    t_rows,
                )
            )

    if "binary_entropy" in ev:
        hist = ev["binary_entropy"].get("histogram", [])
        rows = [[idx, cnt] for idx, cnt in enumerate(hist)]
        results.append(
            _write_csv(
                exports_dir,
                "binary_entropy",
                "binary_entropy.csv",
                ["byte_value", "frequency"],
                rows,
            )
        )

    return results
