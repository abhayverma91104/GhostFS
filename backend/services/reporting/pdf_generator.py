from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, List, Tuple
from xml.sax.saxutils import escape

from reportlab.graphics.shapes import Drawing, Rect, String
from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import mm
from reportlab.platypus import (
    PageBreak,
    Paragraph,
    SimpleDocTemplate,
    Spacer,
    Table,
    TableStyle,
)

from services.reporting.evidence_manager import sha256_file

NAVY = colors.HexColor("#0F172A")
CYAN = colors.HexColor("#06B6D4")
SLATE_BG = colors.HexColor("#F1F5F9")
SLATE_BORDER = colors.HexColor("#CBD5E1")
SLATE_TEXT = colors.HexColor("#334155")
MUTED_TEXT = colors.HexColor("#64748B")
GREEN = colors.HexColor("#16A34A")
AMBER = colors.HexColor("#D97706")
RED = colors.HexColor("#DC2626")
BLUE = colors.HexColor("#2563EB")


def _safe(val: Any, max_len: int = 240) -> str:
    if val is None:
        return "—"
    s = str(val)
    if len(s) > max_len:
        s = s[: max_len - 3] + "..."
    return escape(s)


def _wrap_hash(h: str) -> str:
    if not h:
        return "—"
    return escape(h)


def _bar_chart_drawing(items: List[Tuple[str, float, Any]], width: float = 460.0) -> Drawing:
    row_h = 20.0
    height = max(40.0, len(items) * row_h + 16.0)
    d = Drawing(width, height)
    max_val = max([v for _, v, _ in items] + [1.0])
    label_w = 132.0
    val_w = 50.0
    bar_max_w = width - label_w - val_w - 12.0

    for idx, (label, val, col) in enumerate(items):
        y = height - (idx + 1) * row_h + 4.0
        d.add(String(0, y + 3.0, label[:22], fontName="Helvetica", fontSize=8.5, fillColor=SLATE_TEXT))
        d.add(Rect(label_w, y, bar_max_w, 11.0, fillColor=SLATE_BG, strokeColor=None, rx=3, ry=3))
        bw = max(4.0, (val / max_val) * bar_max_w) if val > 0 else 0.0
        if bw > 0:
            d.add(Rect(label_w, y, bw, 11.0, fillColor=col, strokeColor=None, rx=3, ry=3))
        val_str = f"{int(val)}" if float(val).is_integer() else f"{val:.1f}"
        d.add(
            String(
                label_w + bar_max_w + 8.0,
                y + 3.0,
                val_str,
                fontName="Helvetica-Bold",
                fontSize=8.5,
                fillColor=NAVY,
            )
        )
    return d


def generate_pdf_report(snapshot: Dict[str, Any], out_dir: Path) -> Dict[str, Any]:
    out_path = out_dir / "report.pdf"
    tmp_path = out_dir / ".report.pdf.tmp"
    report_id = str(snapshot.get("report_id", "rep_unknown"))
    report_type = str(snapshot.get("report_type", "complete_investigation"))
    created_at = str(snapshot.get("created_at_utc", ""))

    styles = getSampleStyleSheet()
    title_style = ParagraphStyle(
        "GhostTitle",
        parent=styles["Heading1"],
        fontName="Helvetica-Bold",
        fontSize=17,
        textColor=NAVY,
        spaceAfter=3,
    )
    sub_style = ParagraphStyle(
        "GhostSub",
        parent=styles["Normal"],
        fontName="Helvetica",
        fontSize=9,
        textColor=MUTED_TEXT,
        spaceAfter=10,
    )
    sec_style = ParagraphStyle(
        "GhostSec",
        parent=styles["Heading2"],
        fontName="Helvetica-Bold",
        fontSize=11.5,
        textColor=NAVY,
        spaceBefore=12,
        spaceAfter=6,
    )
    body_style = ParagraphStyle(
        "GhostBody",
        parent=styles["Normal"],
        fontName="Helvetica",
        fontSize=8.8,
        leading=12,
        textColor=SLATE_TEXT,
    )
    cell_style = ParagraphStyle(
        "GhostCell",
        parent=styles["Normal"],
        fontName="Helvetica",
        fontSize=7.8,
        leading=10,
        textColor=SLATE_TEXT,
    )
    mono_cell_style = ParagraphStyle(
        "GhostMonoCell",
        parent=styles["Normal"],
        fontName="Courier",
        fontSize=7.2,
        leading=9.5,
        textColor=NAVY,
    )

    def draw_header_footer(canvas_obj: Any, doc: Any) -> None:
        canvas_obj.saveState()
        w, h = A4
        if doc.page == 1:
            # Top navy banner matching reference mockup
            banner_h = 26 * mm
            canvas_obj.setFillColor(NAVY)
            canvas_obj.rect(0, h - banner_h, w, banner_h, fill=1, stroke=0)
            canvas_obj.setFillColor(CYAN)
            canvas_obj.rect(0, h - banner_h, w, 1.5, fill=1, stroke=0)
            canvas_obj.setFillColor(colors.white)
            canvas_obj.setFont("Helvetica-Bold", 16)
            canvas_obj.drawString(18 * mm, h - 13 * mm, "GhostFS")
            canvas_obj.setFillColor(colors.HexColor("#94A3B8"))
            canvas_obj.setFont("Helvetica", 8.5)
            canvas_obj.drawString(18 * mm, h - 19 * mm, "DIGITAL FORENSIC WORKSTATION")
        else:
            canvas_obj.setStrokeColor(SLATE_BORDER)
            canvas_obj.line(18 * mm, h - 14 * mm, w - 18 * mm, h - 14 * mm)
            canvas_obj.setFillColor(MUTED_TEXT)
            canvas_obj.setFont("Helvetica", 7.5)
            canvas_obj.drawString(18 * mm, h - 12 * mm, f"GhostFS Forensic Report | {report_id}")

        # Footer
        canvas_obj.setStrokeColor(SLATE_BORDER)
        canvas_obj.line(18 * mm, 14 * mm, w - 18 * mm, 14 * mm)
        canvas_obj.setFillColor(MUTED_TEXT)
        canvas_obj.setFont("Helvetica", 7.5)
        canvas_obj.drawString(18 * mm, 9.5 * mm, "GhostFS · Technical Investigation Report (Educational)")
        canvas_obj.drawRightString(w - 18 * mm, 9.5 * mm, f"Page {doc.page}")
        canvas_obj.restoreState()

    doc = SimpleDocTemplate(
        str(tmp_path),
        pagesize=A4,
        leftMargin=18 * mm,
        rightMargin=18 * mm,
        topMargin=30 * mm,
        bottomMargin=18 * mm,
        title=snapshot.get("title", "GhostFS Report"),
        author="GhostFS",
    )

    story: List[Any] = []
    ev = snapshot.get("evidence", {})

    # Title & Metadata
    story.append(Paragraph(_safe(snapshot.get("title", "Forensic Investigation Report"), 120), title_style))
    story.append(
        Paragraph(
            f"Report ID: {_safe(report_id, 64)} | Type: {_safe(report_type, 40)} | UTC: {_safe(created_at, 40)}",
            sub_style,
        )
    )

    if snapshot.get("case_id") or snapshot.get("investigator"):
        story.append(
            Paragraph(
                f"Case Ref: {_safe(snapshot.get('case_id') or '—', 64)} | Investigator: {_safe(snapshot.get('investigator') or '—', 64)}",
                body_style,
            )
        )
        story.append(Spacer(1, 4))

    # 3-Card KPI Summary
    disk_info = ev.get("disk_info", {})
    frag = ev.get("fragmentation", {})
    rec_scan = ev.get("recovery_scan", {})
    gfsck = ev.get("gfsck", {})
    sched_cmp = ev.get("scheduling_comparison", {})

    rec_sum = rec_scan.get("summary", {}) if isinstance(rec_scan, dict) else {}
    verified_rec_count = rec_sum.get("fully_verified", rec_scan.get("fully_verified_count", 0)) if rec_scan else 0

    kpi_1_lbl, kpi_1_val, kpi_1_col = "Source Disk", _safe(snapshot.get("source_disk_name") or "N/A", 24), NAVY
    kpi_2_lbl, kpi_2_val, kpi_2_col = "Metadata Issues", str(gfsck.get("total_issues", 0)) if gfsck else "N/A", GREEN if gfsck.get("total_issues", 0) == 0 else AMBER
    kpi_3_lbl, kpi_3_val, kpi_3_col = "Verified Recovery", str(verified_rec_count) if rec_scan else "N/A", GREEN

    if report_type == "disk_scheduling" and sched_cmp:
        algs = sched_cmp.get("algorithms", [])
        best = min(algs, key=lambda a: a["result"]["total_head_movement"]) if algs else None
        kpi_1_lbl, kpi_1_val = "Requests", str(sched_cmp.get("request_count", 0))
        kpi_2_lbl, kpi_2_val = "FCFS Movement", str(sched_cmp.get("fcfs_total_movement", 0))
        kpi_3_lbl, kpi_3_val = f"Best ({best['result']['algorithm']})" if best else "Best", str(best["result"]["total_head_movement"]) if best else "N/A"

    def kpi_cell(lbl: str, val: str, col: Any) -> Paragraph:
        hex_col = col.hexval() if hasattr(col, "hexval") else "#0F172A"
        return Paragraph(
            f"<font size='8' color='#64748B'>{escape(lbl)}</font><br/><font size='13' color='{hex_col}'><b>{escape(val)}</b></font>",
            body_style,
        )

    kpi_tbl = Table(
        [[kpi_cell(kpi_1_lbl, kpi_1_val, kpi_1_col), kpi_cell(kpi_2_lbl, kpi_2_val, kpi_2_col), kpi_cell(kpi_3_lbl, kpi_3_val, kpi_3_col)]],
        colWidths=[56 * mm, 56 * mm, 56 * mm],
    )
    kpi_tbl.setStyle(
        TableStyle(
            [
                ("BACKGROUND", (0, 0), (-1, -1), SLATE_BG),
                ("BOX", (0, 0), (0, 0), 0.5, SLATE_BORDER),
                ("BOX", (1, 0), (1, 0), 0.5, SLATE_BORDER),
                ("BOX", (2, 0), (2, 0), 0.5, SLATE_BORDER),
                ("TOPPADDING", (0, 0), (-1, -1), 7),
                ("BOTTOMPADDING", (0, 0), (-1, -1), 7),
                ("LEFTPADDING", (0, 0), (-1, -1), 8),
            ]
        )
    )
    story.append(kpi_tbl)
    story.append(Spacer(1, 8))

    sec_num = 1

    # Warnings banner if any
    if snapshot.get("warnings"):
        story.append(Paragraph("Warnings & Evidence Qualifications", sec_style))
        for w in snapshot["warnings"]:
            story.append(Paragraph(f"[WARNING] {_safe(w, 300)}", body_style))

    # 01 Source Disk Analysis
    if disk_info:
        story.append(Paragraph(f"{sec_num:02d} Source Disk & Fragmentation Analysis", sec_style))
        sec_num += 1
        rows = [
            ["Field", "Value"],
            ["Disk Image", Paragraph(_safe(snapshot.get("source_disk_name"), 80), cell_style)],
            ["Source SHA-256", Paragraph(_wrap_hash(snapshot.get("source_disk_sha256", "")), mono_cell_style)],
            ["File System", "GFS1"],
            ["Capacity / Block Size", f"{disk_info.get('total_bytes', 0)} B ({disk_info.get('block_size', 0)} B blocks, {disk_info.get('total_blocks', 0)} total)"],
            ["Allocation Policy", _safe(disk_info.get("alloc_policy", ""))],
            ["Allocated / Free Data Blocks", f"{disk_info.get('allocated_data_blocks', 0)} / {disk_info.get('free_blocks', 0)}"],
            ["External / Internal Frag", f"{float(frag.get('external_fragmentation_pct', 0.0)):.2f}% / {float(frag.get('internal_fragmentation_pct', 0.0)):.2f}%"],
            ["Fragmented Files / Total", f"{frag.get('fragmented_file_count', 0)} / {frag.get('total_files', 0)}"],
        ]
        t = Table(rows, colWidths=[52 * mm, 120 * mm], repeatRows=1)
        t.setStyle(
            TableStyle(
                [
                    ("BACKGROUND", (0, 0), (-1, 0), NAVY),
                    ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
                    ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
                    ("FONTSIZE", (0, 0), (-1, -1), 8.2),
                    ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, SLATE_BG]),
                    ("GRID", (0, 0), (-1, -1), 0.4, SLATE_BORDER),
                    ("TOPPADDING", (0, 0), (-1, -1), 4),
                    ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
                ]
            )
        )
        story.append(t)
        story.append(Spacer(1, 6))
        story.append(
            _bar_chart_drawing(
                [
                    ("Allocated Data", float(disk_info.get("allocated_data_blocks", 0)), BLUE),
                    ("Free Data", float(disk_info.get("free_blocks", 0)), GREEN),
                    ("Reserved Meta", float(disk_info.get("reserved_blocks", 0)), AMBER),
                ]
            )
        )

    # 02 Forensic Recovery Findings
    if rec_scan:
        story.append(Paragraph(f"{sec_num:02d} Forensic Recovery Findings", sec_style))
        sec_num += 1
        story.append(
            _bar_chart_drawing(
                [
                    ("Verified (CRC32)", float(rec_sum.get("fully_verified", rec_scan.get("fully_verified_count", 0))), GREEN),
                    ("Complete (Unverified)", float(rec_sum.get("complete_unverified", rec_scan.get("complete_unverified_count", 0))), CYAN),
                    ("Partial", float(rec_sum.get("partially_recoverable", rec_scan.get("partially_recoverable_count", 0))), AMBER),
                    (
                        "Overwritten / Unrec",
                        float(
                            rec_sum.get("overwritten", rec_scan.get("overwritten_count", 0))
                            + rec_sum.get("unrecoverable", rec_scan.get("unrecoverable_count", 0))
                        ),
                        RED,
                    ),
                ]
            )
        )
        story.append(Spacer(1, 4))
        cands = rec_scan.get("candidates", [])[:25]
        if cands:
            c_rows: List[List[Any]] = [["Candidate", "Method", "Classification", "Expected / Recov B", "Integrity"]]
            for c in cands:
                c_rows.append(
                    [
                        Paragraph(_safe(c.get("filename", ""), 48), cell_style),
                        _safe(c.get("recovery_method", ""), 16),
                        _safe(c.get("classification", ""), 24),
                        f"{c.get('original_size_bytes', 0)} / {c.get('recoverable_bytes', 0)}",
                        _safe(c.get("integrity_status", ""), 22),
                    ]
                )
            ct = Table(c_rows, colWidths=[45 * mm, 24 * mm, 38 * mm, 32 * mm, 33 * mm], repeatRows=1)
            ct.setStyle(
                TableStyle(
                    [
                        ("BACKGROUND", (0, 0), (-1, 0), NAVY),
                        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
                        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
                        ("FONTSIZE", (0, 0), (-1, -1), 7.8),
                        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, SLATE_BG]),
                        ("GRID", (0, 0), (-1, -1), 0.4, SLATE_BORDER),
                    ]
                )
            )
            story.append(ct)

    # 03 Corruption & Repair Findings
    if gfsck:
        story.append(Paragraph(f"{sec_num:02d} Corruption & Repair Analysis", sec_style))
        sec_num += 1
        exp = ev.get("corruption_experiment", {})
        story.append(
            Paragraph(
                f"Experiment: {_safe(exp.get('experiment_id', ''), 48)} | Consistency: <b>{_safe(gfsck.get('consistency_status', ''), 30)}</b> | "
                f"Issues: {gfsck.get('total_issues', 0)} (Critical {gfsck.get('critical_count', 0)}, Error {gfsck.get('error_count', 0)}, Warning {gfsck.get('warning_count', 0)})",
                body_style,
            )
        )
        story.append(
            Paragraph(
                f"Original SHA-256: <font name='Courier' size='7.5'>{_wrap_hash(exp.get('original_sha256', ''))}</font><br/>"
                f"Sandbox SHA-256: <font name='Courier' size='7.5'>{_wrap_hash(exp.get('current_sandbox_sha256', ''))}</font>",
                body_style,
            )
        )
        issues = gfsck.get("issues", [])[:20]
        if issues:
            i_rows: List[List[Any]] = [["ID", "Issue Type", "Severity", "Repairability", "Description"]]
            for iss in issues:
                i_rows.append(
                    [
                        _safe(iss.get("issue_id", ""), 12),
                        Paragraph(_safe(iss.get("issue_type", ""), 32), cell_style),
                        _safe(iss.get("severity", ""), 12),
                        Paragraph(_safe(iss.get("repairability", ""), 28), cell_style),
                        Paragraph(_safe(iss.get("description", ""), 90), cell_style),
                    ]
                )
            it = Table(i_rows, colWidths=[16 * mm, 38 * mm, 20 * mm, 36 * mm, 62 * mm], repeatRows=1)
            it.setStyle(
                TableStyle(
                    [
                        ("BACKGROUND", (0, 0), (-1, 0), NAVY),
                        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
                        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
                        ("FONTSIZE", (0, 0), (-1, -1), 7.6),
                        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, SLATE_BG]),
                        ("GRID", (0, 0), (-1, -1), 0.4, SLATE_BORDER),
                    ]
                )
            )
            story.append(Spacer(1, 4))
            story.append(it)
        story.append(Spacer(1, 3))
        story.append(
            Paragraph(
                "Note: GFS1 64-byte journal slots store intent only; journal recovery performs conservative consistency reconstruction, not full physical WAL replay. Metadata repair does not restore overwritten file payloads.",
                body_style,
            )
        )

    # 04 Disk Scheduling Analysis
    if sched_cmp:
        story.append(Paragraph(f"{sec_num:02d} Simulated Disk Scheduling Analysis", sec_style))
        sec_num += 1
        sc_cfg = ev.get("scheduling_config", {})
        story.append(
            Paragraph(
                f"Mode: {_safe(sc_cfg.get('scheduling_mode', 'static'), 16)} | Head: {sched_cmp.get('initial_head', 0)} | "
                f"Range: {sched_cmp.get('min_cylinder', 0)}–{sched_cmp.get('max_cylinder', 199)} | Direction: {_safe(sched_cmp.get('initial_direction', 'right'), 10)}",
                body_style,
            )
        )
        algs = sched_cmp.get("algorithms", [])
        if algs:
            min_m = min(a["result"]["total_head_movement"] for a in algs)
            chart_items = [
                (
                    a["result"]["algorithm"],
                    float(a["result"]["total_head_movement"]),
                    GREEN if a["result"]["total_head_movement"] == min_m else BLUE,
                )
                for a in algs
            ]
            story.append(Spacer(1, 4))
            story.append(_bar_chart_drawing(chart_items))
            s_rows: List[List[Any]] = [["Algorithm", "Total Move", "Avg / Req", "Dir Changes", "Circ Jump", "vs FCFS"]]
            for a in algs:
                r = a["result"]
                s_rows.append(
                    [
                        r.get("algorithm", ""),
                        str(r.get("total_head_movement", 0)),
                        f"{float(r.get('average_movement', 0.0)):.2f}",
                        str(r.get("direction_changes", 0)),
                        str(r.get("circular_repositioning_distance", 0)),
                        f"{float(a.get('reduction_vs_fcfs_pct', 0.0)):.1f}%",
                    ]
                )
            st = Table(s_rows, colWidths=[32 * mm, 28 * mm, 26 * mm, 28 * mm, 28 * mm, 30 * mm], repeatRows=1)
            st.setStyle(
                TableStyle(
                    [
                        ("BACKGROUND", (0, 0), (-1, 0), NAVY),
                        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
                        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
                        ("FONTSIZE", (0, 0), (-1, -1), 8),
                        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, SLATE_BG]),
                        ("GRID", (0, 0), (-1, -1), 0.4, SLATE_BORDER),
                    ]
                )
            )
            story.append(Spacer(1, 4))
            story.append(st)
        story.append(
            Paragraph(
                "Disclaimer: Simulated mechanical cylinder seek distances on virtual blocks; not measured physical HDD/SSD performance.",
                body_style,
            )
        )

    # 05 Binary Analysis Excerpt
    if "binary_entropy" in ev:
        story.append(Paragraph(f"{sec_num:02d} Binary & Entropy Analysis (Bounded Excerpt)", sec_style))
        sec_num += 1
        ent = ev["binary_entropy"]
        hx = ev.get("binary_hex_excerpt", {})
        shannon_val = float(ent.get("shannon_entropy", 0.0))
        ent_class = ent.get("classification") or (
            "Low / Structured" if shannon_val < 2.0 else ("Moderate" if shannon_val < 6.5 else "High / Random")
        )
        printable_pct = (
            float(ent["printable_ascii_pct"])
            if "printable_ascii_pct" in ent
            else float(ent.get("printable_ascii_ratio", 0.0)) * 100.0
        )
        null_pct = (
            float(ent["zero_pct"])
            if "zero_pct" in ent
            else float(ent.get("null_byte_ratio", 0.0)) * 100.0
        )
        story.append(
            Paragraph(
                f"Shannon Entropy: <b>{shannon_val:.4f}</b> bits/byte ({_safe(ent_class, 24)}) | "
                f"Printable ASCII: {printable_pct:.1f}% | "
                f"Null Bytes: {null_pct:.1f}%",
                body_style,
            )
        )
        rows_hx = hx.get("rows", [])[:8]
        if rows_hx:
            h_rows: List[List[Any]] = [["Offset", "Hex Bytes (Excerpt)", "ASCII"]]
            for r in rows_hx:
                off_str = r.get("offset_hex") or f"0x{int(r.get('abs_offset', 0)):08X}"
                raw_hx = r.get("hex_bytes", "")
                hx_str = " ".join(str(b) for b in raw_hx) if isinstance(raw_hx, list) else str(raw_hx)
                h_rows.append(
                    [
                        _safe(off_str, 16),
                        Paragraph(_safe(hx_str, 64), mono_cell_style),
                        Paragraph(_safe(r.get("ascii", ""), 24), mono_cell_style),
                    ]
                )
            ht = Table(h_rows, colWidths=[28 * mm, 102 * mm, 42 * mm], repeatRows=1)
            ht.setStyle(
                TableStyle(
                    [
                        ("BACKGROUND", (0, 0), (-1, 0), NAVY),
                        ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
                        ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
                        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, SLATE_BG]),
                        ("GRID", (0, 0), (-1, -1), 0.4, SLATE_BORDER),
                    ]
                )
            )
            story.append(Spacer(1, 4))
            story.append(ht)

    # Final Section: Evidence Integrity & Limitations
    story.append(Paragraph(f"{sec_num:02d} Evidence Integrity & Scope Limitations", sec_style))
    story.append(
        Paragraph(
            f"Snapshot SHA-256: <font name='Courier' size='7.5'>{_wrap_hash(snapshot.get('snapshot_sha256', ''))}</font><br/>"
            "External artifact hashes (PDF, JSON, CSV) are recorded in manifest.json. "
            "GhostFS is an educational custom file system and digital forensics workstation — not a legal forensic certification or host OS I/O scheduler.",
            body_style,
        )
    )
    if snapshot.get("notes"):
        story.append(Spacer(1, 4))
        story.append(Paragraph(f"Investigator Notes: {_safe(snapshot['notes'], 600)}", body_style))

    doc.build(story, onFirstPage=draw_header_footer, onLaterPages=draw_header_footer)
    os.replace(tmp_path, out_path)
    return {
        "filename": "report.pdf",
        "size_bytes": out_path.stat().st_size,
        "sha256": sha256_file(out_path),
    }
