import { useCallback, useEffect, useMemo, useState } from 'react'
import {
  Download,
  Eye,
  FileJson,
  FileSpreadsheet,
  FileText,
  RefreshCw,
  Search,
  ShieldCheck,
  Trash2,
} from 'lucide-react'
import { ConfirmDialog } from '../components/ConfirmDialog'
import {
  deleteReport,
  fetchAvailableEvidence,
  generateReport,
  getReportCsvExportUrl,
  getReportJsonUrl,
  getReportManifestUrl,
  getReportPdfUrl,
  listReports,
  previewReport,
  verifyReportIntegrity,
} from '../services/api'
import type {
  AvailableEvidenceResponse,
  ReportManifest,
  ReportNavigationContext,
  ReportSnapshot,
  ReportType,
  VolumeStatus,
} from '../types/api'

interface ForensicReportsPageProps {
  activeDisk: VolumeStatus | null
  navigationContext: ReportNavigationContext | null
  onNotify: (kind: 'success' | 'error', message: string) => void
}

const REPORT_TYPES: Array<{ value: ReportType; label: string }> = [
  { value: 'complete_investigation', label: 'F. Complete Investigation Report' },
  { value: 'disk_analysis', label: 'A. Disk Analysis & Fragmentation Report' },
  { value: 'forensic_recovery', label: 'B. Forensic Recovery Report' },
  { value: 'corruption_repair', label: 'C. Corruption & Repair Report' },
  { value: 'disk_scheduling', label: 'D. Disk Scheduling Simulation Report' },
  { value: 'binary_analysis', label: 'E. Binary & Entropy Analysis Report' },
]

export function ForensicReportsPage({
  activeDisk,
  navigationContext,
  onNotify,
}: ForensicReportsPageProps) {
  const [evidence, setEvidence] = useState<AvailableEvidenceResponse | null>(null)
  const [reports, setReports] = useState<ReportManifest[]>([])

  const [reportType, setReportType] = useState<ReportType>('complete_investigation')
  const [diskName, setDiskName] = useState<string>(activeDisk?.disk_name ?? '')
  const [experimentId, setExperimentId] = useState<string>('')
  const [title, setTitle] = useState('')
  const [caseId, setCaseId] = useState('')
  const [investigator, setInvestigator] = useState('')
  const [notes, setNotes] = useState('')
  const [exportPdf, setExportPdf] = useState(true)
  const [exportJson, setExportJson] = useState(true)
  const [exportCsv, setExportCsv] = useState(true)

  const [preview, setPreview] = useState<ReportSnapshot | null>(null)
  const [busy, setBusy] = useState(false)
  const [searchQuery, setSearchQuery] = useState('')
  const [typeFilter, setTypeFilter] = useState<string>('')
  const [verifyState, setVerifyState] = useState<Record<string, boolean>>({})
  const [pendingDeleteId, setPendingDeleteId] = useState<string | null>(null)

  const refreshAll = useCallback(async () => {
    try {
      const [ev, reps] = await Promise.all([fetchAvailableEvidence(), listReports()])
      setEvidence(ev)
      setReports(reps.reports)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to load reporting data')
    }
  }, [onNotify])

  useEffect(() => {
    void refreshAll()
  }, [refreshAll])

  useEffect(() => {
    if (navigationContext) {
      setReportType(navigationContext.reportType)
      if (navigationContext.diskName) setDiskName(navigationContext.diskName)
      if (navigationContext.experimentId) setExperimentId(navigationContext.experimentId)
      if (navigationContext.title) setTitle(navigationContext.title)
    } else if (!diskName && activeDisk?.disk_name) {
      setDiskName(activeDisk.disk_name)
    }
  }, [activeDisk, diskName, navigationContext])

  const buildPayload = () => ({
    report_type: reportType,
    disk_name: diskName || null,
    title,
    case_id: caseId,
    investigator,
    notes,
    experiment_id: experimentId || null,
    scheduling_payload:
      navigationContext?.schedulingPayload ??
      (reportType === 'disk_scheduling' || reportType === 'complete_investigation'
        ? {
            min_cylinder: 0,
            max_cylinder: 199,
            initial_head: 53,
            initial_direction: 'right',
            requests: [98, 183, 37, 122, 14, 124, 65, 67],
          }
        : null),
    binary_payload: navigationContext?.binaryPayload ?? null,
  })

  const handlePreview = async () => {
    setBusy(true)
    try {
      const snap = await previewReport(buildPayload())
      setPreview(snap)
      onNotify('success', 'Report snapshot preview built from live C++ engine evidence')
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Preview failed')
    } finally {
      setBusy(false)
    }
  }

  const handleGenerate = async () => {
    setBusy(true)
    try {
      const manifest = await generateReport({
        ...buildPayload(),
        export_pdf: exportPdf,
        export_json: exportJson,
        export_csv: exportCsv,
      })
      await refreshAll()
      onNotify('success', `Generated report ${manifest.report_id}`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Report generation failed')
    } finally {
      setBusy(false)
    }
  }

  const handleVerify = async (reportId: string) => {
    try {
      const res = await verifyReportIntegrity(reportId)
      setVerifyState((prev) => ({ ...prev, [reportId]: res.verified }))
      onNotify(
        res.verified ? 'success' : 'error',
        res.verified
          ? `All artifacts for ${reportId} match manifest SHA-256 hashes`
          : `Integrity mismatch detected in ${reportId}!`,
      )
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Verification failed')
    }
  }

  const handleConfirmDelete = async () => {
    if (!pendingDeleteId) return
    const rid = pendingDeleteId
    setPendingDeleteId(null)
    try {
      await deleteReport(rid, true)
      await refreshAll()
      onNotify('success', `Deleted report ${rid} (source disks preserved)`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Delete failed')
    }
  }

  const filteredReports = useMemo(() => {
    return reports.filter((r) => {
      if (typeFilter && r.report_type !== typeFilter) return false
      if (searchQuery.trim()) {
        const q = searchQuery.toLowerCase()
        return (
          r.report_id.toLowerCase().includes(q) ||
          r.title.toLowerCase().includes(q) ||
          r.source_disk_name.toLowerCase().includes(q)
        )
      }
      return true
    })
  }, [reports, searchQuery, typeFilter])

  return (
    <div className="space-y-6">
      <div className="grid gap-6 xl:grid-cols-12">
        {/* A. Report Generation Panel */}
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-6 space-y-3 text-xs">
          <h2 className="text-sm font-semibold text-slate-100">
            Forensic Reports &amp; Evidence Export
          </h2>
          <div className="grid gap-3 sm:grid-cols-2">
            <div>
              <label htmlFor="rep-type" className="block text-slate-400">
                Report Type
              </label>
              <select
                id="rep-type"
                value={reportType}
                onChange={(e) => setReportType(e.target.value as ReportType)}
                className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
              >
                {REPORT_TYPES.map((rt) => (
                  <option key={rt.value} value={rt.value}>
                    {rt.label}
                  </option>
                ))}
              </select>
            </div>
            <div>
              <label htmlFor="rep-disk" className="block text-slate-400">
                Source Disk
              </label>
              <select
                id="rep-disk"
                value={diskName}
                onChange={(e) => setDiskName(e.target.value)}
                className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
              >
                <option value="">-- None / Scheduling Only --</option>
                {(evidence?.disks ?? []).map((d) => (
                  <option key={d.disk_name} value={d.disk_name}>
                    {d.disk_name}
                  </option>
                ))}
              </select>
            </div>
            <div>
              <label htmlFor="rep-exp" className="block text-slate-400">
                Corruption Experiment (optional)
              </label>
              <select
                id="rep-exp"
                value={experimentId}
                onChange={(e) => setExperimentId(e.target.value)}
                className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
              >
                <option value="">-- None --</option>
                {(evidence?.experiments ?? []).map((ex) => (
                  <option key={ex.experiment_id} value={ex.experiment_id}>
                    {ex.experiment_id} ({ex.source_disk_name})
                  </option>
                ))}
              </select>
            </div>
            <div>
              <label htmlFor="rep-title" className="block text-slate-400">
                Report Title
              </label>
              <input
                id="rep-title"
                type="text"
                value={title}
                onChange={(e) => setTitle(e.target.value)}
                placeholder="e.g. GFS1 Case Investigation"
                className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
              />
            </div>
            <div>
              <label htmlFor="rep-case" className="block text-slate-400">
                Case / Reference ID (optional)
              </label>
              <input
                id="rep-case"
                type="text"
                value={caseId}
                onChange={(e) => setCaseId(e.target.value)}
                placeholder="e.g. LAB-2026-01"
                className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
              />
            </div>
            <div>
              <label htmlFor="rep-inv" className="block text-slate-400">
                Investigator (optional)
              </label>
              <input
                id="rep-inv"
                type="text"
                value={investigator}
                onChange={(e) => setInvestigator(e.target.value)}
                placeholder="Optional name"
                className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
              />
            </div>
          </div>
          <div>
            <label htmlFor="rep-notes" className="block text-slate-400">
              Investigation Notes (optional)
            </label>
            <textarea
              id="rep-notes"
              rows={2}
              value={notes}
              onChange={(e) => setNotes(e.target.value)}
              className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] p-2.5 text-slate-100"
            />
          </div>
          <div className="flex flex-wrap items-center gap-4 text-slate-300">
            <label className="inline-flex items-center gap-1.5">
              <input
                type="checkbox"
                checked={exportPdf}
                onChange={(e) => setExportPdf(e.target.checked)}
              />
              <span>PDF (ReportLab)</span>
            </label>
            <label className="inline-flex items-center gap-1.5">
              <input
                type="checkbox"
                checked={exportJson}
                onChange={(e) => setExportJson(e.target.checked)}
              />
              <span>JSON Evidence</span>
            </label>
            <label className="inline-flex items-center gap-1.5">
              <input
                type="checkbox"
                checked={exportCsv}
                onChange={(e) => setExportCsv(e.target.checked)}
              />
              <span>CSV Tables</span>
            </label>
          </div>
          <div className="flex flex-wrap gap-2 pt-1">
            <button
              type="button"
              disabled={busy}
              onClick={() => void handlePreview()}
              className="inline-flex items-center gap-1.5 rounded-lg border border-slate-700 bg-slate-800 px-3.5 py-2 font-medium text-slate-200 hover:border-cyan-500/50 disabled:opacity-50"
            >
              <Eye className="h-4 w-4" aria-hidden="true" />
              <span>Preview Snapshot</span>
            </button>
            <button
              type="button"
              disabled={busy}
              onClick={() => void handleGenerate()}
              className="inline-flex items-center gap-1.5 rounded-lg bg-cyan-600 px-4 py-2 font-medium text-white hover:bg-cyan-500 disabled:opacity-50"
            >
              <FileText className="h-4 w-4" aria-hidden="true" />
              <span>{busy ? 'Generating...' : 'Generate Report'}</span>
            </button>
          </div>
        </section>

        {/* B & C. Evidence Availability & Snapshot Preview */}
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-6 space-y-3 text-xs">
          <h2 className="text-sm font-semibold text-slate-100">
            Available Evidence &amp; Snapshot Preview
          </h2>
          <div className="grid grid-cols-3 gap-2.5">
            <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-2.5">
              <span className="text-slate-400">GFS1 Disks</span>
              <p className="font-mono-tabular mt-1 text-lg font-semibold text-slate-100">
                {evidence?.disks.length ?? 0}
              </p>
            </div>
            <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-2.5">
              <span className="text-slate-400">Recovery Manifests</span>
              <p className="font-mono-tabular mt-1 text-lg font-semibold text-cyan-300">
                {evidence?.recovery_manifests.length ?? 0}
              </p>
            </div>
            <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-2.5">
              <span className="text-slate-400">Corruption Exps</span>
              <p className="font-mono-tabular mt-1 text-lg font-semibold text-amber-300">
                {evidence?.experiments.length ?? 0}
              </p>
            </div>
          </div>

          {preview ? (
            <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3 space-y-2">
              <div className="flex items-center justify-between">
                <span className="font-semibold text-cyan-300">{preview.title}</span>
                <span className="font-mono-tabular text-[11px] text-slate-400">
                  {preview.created_at_utc}
                </span>
              </div>
              <p className="font-mono-tabular text-[11px] text-slate-400 break-all">
                Source SHA-256: {preview.source_disk_sha256 || 'N/A'}
              </p>
              <p className="font-mono-tabular text-[11px] text-emerald-300 break-all">
                Snapshot SHA-256: {preview.snapshot_sha256}
              </p>
              <p className="text-slate-300">
                Included Evidence Keys:{' '}
                <span className="font-mono-tabular text-slate-100">
                  {Object.keys(preview.evidence).join(', ') || 'none'}
                </span>
              </p>
              {preview.warnings.length > 0 && (
                <div className="rounded border border-amber-500/40 bg-amber-500/10 p-2 text-amber-200">
                  {preview.warnings.map((w, i) => (
                    <p key={i}>{w}</p>
                  ))}
                </div>
              )}
            </div>
          ) : (
            <p className="text-slate-400">
              Click &ldquo;Preview Snapshot&rdquo; to inspect the exact immutable evidence payload
              before generating PDF/JSON/CSV artifacts.
            </p>
          )}
        </section>
      </div>

      {/* E. Report History */}
      <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 space-y-4">
        <div className="flex flex-wrap items-center justify-between gap-3">
          <h2 className="text-sm font-semibold text-slate-100">
            Generated Reports ({filteredReports.length})
          </h2>
          <div className="flex flex-wrap items-center gap-2 text-xs">
            <div className="relative">
              <Search className="absolute left-2.5 top-2 h-3.5 w-3.5 text-slate-400" aria-hidden="true" />
              <input
                type="text"
                placeholder="Search ID, title, disk..."
                value={searchQuery}
                onChange={(e) => setSearchQuery(e.target.value)}
                className="rounded-lg border border-slate-700 bg-[#0b0f17] pl-8 pr-3 py-1.5 text-slate-100"
              />
            </div>
            <select
              aria-label="Filter report type"
              value={typeFilter}
              onChange={(e) => setTypeFilter(e.target.value)}
              className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
            >
              <option value="">All Types</option>
              {REPORT_TYPES.map((rt) => (
                <option key={rt.value} value={rt.value}>
                  {rt.value}
                </option>
              ))}
            </select>
            <button
              type="button"
              onClick={() => void refreshAll()}
              className="inline-flex items-center gap-1 rounded-lg border border-slate-700 bg-slate-800 px-2.5 py-1.5 text-slate-200 hover:border-cyan-500/50"
            >
              <RefreshCw className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Refresh</span>
            </button>
          </div>
        </div>

        {filteredReports.length === 0 ? (
          <p className="text-xs text-slate-400">No generated reports match this filter.</p>
        ) : (
          <div className="space-y-3">
            {filteredReports.map((r) => (
              <div
                key={r.report_id}
                className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3.5 text-xs space-y-2"
              >
                <div className="flex flex-wrap items-center justify-between gap-2">
                  <div>
                    <p className="font-semibold text-slate-100">
                      {r.title}{' '}
                      <span className="font-mono-tabular text-cyan-300">({r.report_id})</span>
                    </p>
                    <p className="text-[11px] text-slate-400">
                      Type: {r.report_type} · Disk: {r.source_disk_name || 'N/A'} · Created:{' '}
                      {r.created_at_utc}
                    </p>
                  </div>
                  <div className="flex flex-wrap items-center gap-1.5">
                    {r.artifacts.pdf && (
                      <a
                        href={getReportPdfUrl(r.report_id)}
                        download={`${r.report_id}.pdf`}
                        className="inline-flex items-center gap-1 rounded border border-cyan-500/40 bg-cyan-500/15 px-2.5 py-1 text-cyan-200 hover:bg-cyan-500/25"
                      >
                        <Download className="h-3.5 w-3.5" aria-hidden="true" />
                        <span>PDF</span>
                      </a>
                    )}
                    {r.artifacts.json && (
                      <a
                        href={getReportJsonUrl(r.report_id)}
                        download={`${r.report_id}.evidence.json`}
                        className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2.5 py-1 text-slate-200 hover:border-cyan-500/50"
                      >
                        <FileJson className="h-3.5 w-3.5" aria-hidden="true" />
                        <span>JSON</span>
                      </a>
                    )}
                    <a
                      href={getReportManifestUrl(r.report_id)}
                      download={`${r.report_id}.manifest.json`}
                      className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2.5 py-1 text-slate-200 hover:border-cyan-500/50"
                    >
                      <FileText className="h-3.5 w-3.5" aria-hidden="true" />
                      <span>Manifest</span>
                    </a>
                    <button
                      type="button"
                      onClick={() => void handleVerify(r.report_id)}
                      className="inline-flex items-center gap-1 rounded border border-emerald-500/40 bg-emerald-500/15 px-2.5 py-1 text-emerald-200 hover:bg-emerald-500/25"
                    >
                      <ShieldCheck className="h-3.5 w-3.5" aria-hidden="true" />
                      <span>
                        {verifyState[r.report_id] === true
                          ? 'Verified'
                          : verifyState[r.report_id] === false
                            ? 'Mismatch'
                            : 'Verify'}
                      </span>
                    </button>
                    <button
                      type="button"
                      aria-label={`Delete report ${r.report_id}`}
                      onClick={() => setPendingDeleteId(r.report_id)}
                      className="inline-flex items-center gap-1 rounded border border-rose-500/40 bg-rose-500/10 px-2 py-1 text-rose-300 hover:bg-rose-500/20"
                    >
                      <Trash2 className="h-3.5 w-3.5" aria-hidden="true" />
                    </button>
                  </div>
                </div>
                {(r.artifacts.csv_exports?.length ?? 0) > 0 && (
                  <div className="flex flex-wrap items-center gap-1.5 pt-1">
                    <span className="text-[11px] text-slate-400">CSV Exports:</span>
                    {r.artifacts.csv_exports?.map((c) => (
                      <a
                        key={c.export_id}
                        href={getReportCsvExportUrl(r.report_id, c.export_id)}
                        download={`${r.report_id}_${c.export_id}.csv`}
                        className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-900 px-2 py-0.5 text-[11px] text-slate-300 hover:border-cyan-500/50"
                      >
                        <FileSpreadsheet className="h-3 w-3 text-emerald-400" aria-hidden="true" />
                        <span>
                          {c.filename} ({c.row_count})
                        </span>
                      </a>
                    ))}
                  </div>
                )}
              </div>
            ))}
          </div>
        )}
      </section>

      <ConfirmDialog
        open={pendingDeleteId !== null}
        title="Delete Generated Report"
        description={`Delete report '${pendingDeleteId ?? ''}'? Underlying source disk images, recovery artifacts, and sandbox experiments will be preserved.`}
        confirmLabel="Delete Report"
        danger
        onCancel={() => setPendingDeleteId(null)}
        onConfirm={() => void handleConfirmDelete()}
      />
    </div>
  )
}
