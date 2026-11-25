import { useEffect, useMemo, useState } from 'react'
import {
  AlertTriangle,
  Binary,
  CheckCircle2,
  Download,
  Eye,
  FileJson,
  FileSearch,
  Grid3x3,
  HardDrive,
  Search,
  ShieldAlert,
  ShieldCheck,
} from 'lucide-react'
import {
  extractRecoveryCandidates,
  getArtifactDownloadUrl,
  getArtifactPreview,
  getManifestDownloadUrl,
  startRecoveryScan,
} from '../services/api'
import type {
  ArtifactPreviewResponse,
  ForensicScanResult,
  HexNavigationTarget,
  RecoveryCandidate,
  RecoveryClassification,
  RecoveryManifest,
  ReportNavigationContext,
  VolumeStatus,
} from '../types/api'

interface RecoveryCenterPageProps {
  activeDisk: VolumeStatus | null
  onGoToDiskManager: () => void
  onHighlightInBlockExplorer: (blocks: number[], label: string) => void
  onOpenInHexViewer: (target: HexNavigationTarget) => void
  onExportReport: (ctx: ReportNavigationContext) => void
  onNotify: (kind: 'success' | 'error', message: string) => void
}

const CLASSIFICATION_BADGE: Record<
  RecoveryClassification,
  { label: string; badgeClass: string }
> = {
  fully_verified: {
    label: 'Fully Verified',
    badgeClass: 'border-emerald-500/40 bg-emerald-500/15 text-emerald-300',
  },
  complete_unverified: {
    label: 'Complete (Unverified)',
    badgeClass: 'border-cyan-500/40 bg-cyan-500/15 text-cyan-300',
  },
  partially_recoverable: {
    label: 'Partially Recoverable',
    badgeClass: 'border-amber-500/40 bg-amber-500/15 text-amber-300',
  },
  overwritten: {
    label: 'Overwritten',
    badgeClass: 'border-rose-500/40 bg-rose-500/15 text-rose-300',
  },
  unrecoverable: {
    label: 'Unrecoverable',
    badgeClass: 'border-rose-600/40 bg-rose-950/60 text-rose-300',
  },
  unknown: {
    label: 'Unknown',
    badgeClass: 'border-slate-600 bg-slate-800 text-slate-300',
  },
}

const LOGICAL_BLOCK_STYLE: Record<string, string> = {
  intact: 'border-emerald-500/60 bg-emerald-600/30 text-emerald-100',
  overwritten: 'border-rose-500/70 bg-rose-700/40 text-rose-100',
  missing_indirect: 'border-amber-500/70 border-dashed bg-amber-900/40 text-amber-100',
  unknown: 'border-slate-600 bg-slate-800 text-slate-300',
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`
}

export function RecoveryCenterPage({
  activeDisk,
  onGoToDiskManager,
  onHighlightInBlockExplorer,
  onOpenInHexViewer,
  onExportReport,
  onNotify,
}: RecoveryCenterPageProps) {
  const [scanMode, setScanMode] = useState<'quick' | 'deep' | 'combined'>('combined')
  const [enableTxtHeuristic, setEnableTxtHeuristic] = useState(true)
  const [selectedTypes, setSelectedTypes] = useState<string[]>([])
  const [scanning, setScanning] = useState(false)
  const [scanResult, setScanResult] = useState<ForensicScanResult | null>(null)

  const [filterClass, setFilterClass] = useState<string>('')
  const [filterType, setFilterType] = useState<string>('')
  const [searchQuery, setSearchQuery] = useState('')
  const [sortBy, setSortBy] = useState<'filename' | 'size' | 'recoverable'>('filename')
  const [selectedIds, setSelectedIds] = useState<string[]>([])
  const [inspectedCandidate, setInspectedCandidate] = useState<RecoveryCandidate | null>(null)

  const [zeroFillMissing, setZeroFillMissing] = useState(false)
  const [extracting, setExtracting] = useState(false)
  const [extractedArtifacts, setExtractedArtifacts] = useState<RecoveryManifest[]>([])
  const [preview, setPreview] = useState<ArtifactPreviewResponse | null>(null)

  const diskName = activeDisk?.disk_name ?? null

  useEffect(() => {
    setScanResult(null)
    setSelectedIds([])
    setInspectedCandidate(null)
    setPreview(null)
  }, [diskName])

  const filteredCandidates = useMemo(() => {
    if (!scanResult) return []
    const q = searchQuery.trim().toLowerCase()
    const list = scanResult.candidates.filter((c) => {
      if (filterClass && c.classification !== filterClass) return false
      if (filterType && c.file_type !== filterType) return false
      if (
        q &&
        !c.filename.toLowerCase().includes(q) &&
        !c.original_path.toLowerCase().includes(q) &&
        !c.candidate_id.toLowerCase().includes(q)
      ) {
        return false
      }
      return true
    })
    list.sort((a, b) => {
      if (sortBy === 'size') return b.original_size_bytes - a.original_size_bytes
      if (sortBy === 'recoverable') return b.recoverable_bytes - a.recoverable_bytes
      return a.filename.localeCompare(b.filename)
    })
    return list
  }, [scanResult, filterClass, filterType, searchQuery, sortBy])

  if (!activeDisk) {
    return (
      <section className="rounded-xl border border-dashed border-slate-700 bg-[#111827] p-10 text-center">
        <HardDrive className="mx-auto h-10 w-10 text-slate-500" aria-hidden="true" />
        <h2 className="mt-3 text-base font-semibold text-slate-100">No Virtual Disk Mounted</h2>
        <p className="mt-1 text-sm text-slate-400">
          Mount a `.gfs` disk image in the Disk Manager to perform read-only forensic recovery scans.
        </p>
        <button
          type="button"
          onClick={onGoToDiskManager}
          className="mt-5 rounded-lg bg-cyan-600 px-4 py-2 text-sm font-medium text-white hover:bg-cyan-500"
        >
          Open Disk Manager
        </button>
      </section>
    )
  }

  function toggleTypeFilter(t: string) {
    setSelectedTypes((prev) =>
      prev.includes(t) ? prev.filter((item) => item !== t) : [...prev, t],
    )
  }

  async function handleRunScan() {
    if (!diskName) return
    setScanning(true)
    setSelectedIds([])
    setInspectedCandidate(null)
    try {
      const res = await startRecoveryScan(diskName, {
        mode: scanMode,
        file_types: selectedTypes,
        enable_txt_heuristic: enableTxtHeuristic,
      })
      setScanResult(res)
      if (res.candidates.length > 0) {
        setInspectedCandidate(res.candidates[0])
      }
      onNotify(
        'success',
        `Scan ${res.scan_id} complete: ${res.total_candidates} candidate(s) found (${res.scan_duration_ms.toFixed(1)} ms)`,
      )
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Forensic scan failed')
    } finally {
      setScanning(false)
    }
  }

  async function handleExtractIds(ids: string[]) {
    if (!diskName || !scanResult || ids.length === 0) return
    setExtracting(true)
    try {
      const res = await extractRecoveryCandidates(diskName, {
        scan_id: scanResult.scan_id,
        candidate_ids: ids,
        zero_fill_missing: zeroFillMissing,
        enable_txt_heuristic: enableTxtHeuristic,
      })
      setExtractedArtifacts((prev) => [...res.artifacts, ...prev])
      onNotify(
        'success',
        `Extracted ${res.extracted_count} artifact(s) with JSON provenance manifests to reports/recovered/`,
      )
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Extraction failed')
    } finally {
      setExtracting(false)
    }
  }

  async function handlePreviewArtifact(artifactId: string) {
    try {
      const data = await getArtifactPreview(artifactId)
      setPreview(data)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to preview artifact')
    }
  }

  const verifiedIds =
    scanResult?.candidates
      .filter((c) => c.classification === 'fully_verified')
      .map((c) => c.candidate_id) ?? []

  return (
    <div className="space-y-6">
      {/* A. Scan Configuration */}
      <section className="rounded-xl border border-slate-800 bg-[#111827] p-5">
        <div className="flex flex-wrap items-start justify-between gap-4">
          <div>
            <h2 className="text-base font-semibold text-slate-100">
              Read-Only Forensic Scan Configuration — {activeDisk.disk_name}
            </h2>
            <p className="mt-1 text-xs text-slate-400">
              Source disk is opened read-only (<span className="font-mono-tabular">LOCK_SH</span>) and SHA-256 verified before extraction.
            </p>
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <button
              type="button"
              onClick={() =>
                onExportReport({
                  reportType: 'forensic_recovery',
                  diskName: activeDisk.disk_name,
                  title: `Forensic Recovery — ${activeDisk.disk_name}`,
                })
              }
              className="rounded-lg border border-emerald-500/40 bg-emerald-500/10 px-3 py-2 text-xs font-medium text-emerald-300 hover:bg-emerald-500/20"
            >
              Generate Recovery Report
            </button>
            <button
              type="button"
              disabled={scanning}
              onClick={() => void handleRunScan()}
              className="inline-flex items-center gap-2 rounded-lg bg-cyan-600 px-4 py-2 text-sm font-medium text-white hover:bg-cyan-500 disabled:opacity-50"
            >
            <FileSearch className={`h-4 w-4 ${scanning ? 'animate-pulse' : ''}`} aria-hidden="true" />
            <span>{scanning ? 'Scanning Unallocated Regions...' : 'Start Forensic Scan'}</span>
          </button>
          </div>
        </div>

        <div className="mt-4 grid gap-4 md:grid-cols-3">
          <fieldset className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
            <legend className="px-1 text-xs font-medium text-slate-300">Scan Mode</legend>
            <div className="mt-1 space-y-1.5 text-xs">
              {(
                [
                  ['quick', 'Quick Scan (Metadata & Inodes)'],
                  ['deep', 'Deep Scan (Signature Carving)'],
                  ['combined', 'Combined Scan (Correlated)'],
                ] as const
              ).map(([m, label]) => (
                <label key={m} className="flex cursor-pointer items-center gap-2 text-slate-200">
                  <input
                    type="radio"
                    name="scan_mode"
                    value={m}
                    checked={scanMode === m}
                    onChange={() => setScanMode(m)}
                  />
                  <span>{label}</span>
                </label>
              ))}
            </div>
          </fieldset>

          <fieldset className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
            <legend className="px-1 text-xs font-medium text-slate-300">File Type Filter (Optional)</legend>
            <div className="mt-1 flex flex-wrap gap-3 text-xs text-slate-200">
              {['png', 'jpeg', 'pdf', 'txt', 'unknown'].map((t) => (
                <label key={t} className="flex cursor-pointer items-center gap-1.5">
                  <input
                    type="checkbox"
                    checked={selectedTypes.includes(t)}
                    onChange={() => toggleTypeFilter(t)}
                  />
                  <span className="font-mono-tabular uppercase">{t}</span>
                </label>
              ))}
            </div>
            <p className="mt-2 text-[11px] text-slate-400">Leave all unchecked to include every type.</p>
          </fieldset>

          <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3 text-xs">
            <p className="font-medium text-slate-300">Carving &amp; Reconstruction Options</p>
            <label className="mt-2 flex cursor-pointer items-center gap-2 text-slate-200">
              <input
                type="checkbox"
                checked={enableTxtHeuristic}
                onChange={(e) => setEnableTxtHeuristic(e.target.checked)}
              />
              <span>Enable heuristic TXT carving (no magic signature)</span>
            </label>
            <label className="mt-2 flex cursor-pointer items-center gap-2 text-amber-200">
              <input
                type="checkbox"
                checked={zeroFillMissing}
                onChange={(e) => setZeroFillMissing(e.target.checked)}
              />
              <span>Zero-fill missing ranges on partial extraction (logged)</span>
            </label>
          </div>
        </div>
      </section>

      {/* B. Recovery Overview */}
      {scanResult && (
        <section className="space-y-4">
          <div className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4 xl:grid-cols-8">
            <div className="rounded-xl border border-slate-800 bg-[#111827] p-3.5">
              <p className="text-[11px] text-slate-400">Candidates</p>
              <p className="font-mono-tabular mt-1 text-lg font-semibold text-slate-100">
                {scanResult.total_candidates}
              </p>
            </div>
            <div className="rounded-xl border border-emerald-500/30 bg-[#111827] p-3.5">
              <p className="text-[11px] text-emerald-300">Fully Verified</p>
              <p className="font-mono-tabular mt-1 text-lg font-semibold text-emerald-300">
                {scanResult.summary.fully_verified}
              </p>
            </div>
            <div className="rounded-xl border border-cyan-500/30 bg-[#111827] p-3.5">
              <p className="text-[11px] text-cyan-300">Complete Unverified</p>
              <p className="font-mono-tabular mt-1 text-lg font-semibold text-cyan-300">
                {scanResult.summary.complete_unverified}
              </p>
            </div>
            <div className="rounded-xl border border-amber-500/30 bg-[#111827] p-3.5">
              <p className="text-[11px] text-amber-300">Partial</p>
              <p className="font-mono-tabular mt-1 text-lg font-semibold text-amber-300">
                {scanResult.summary.partially_recoverable}
              </p>
            </div>
            <div className="rounded-xl border border-rose-500/30 bg-[#111827] p-3.5">
              <p className="text-[11px] text-rose-300">Overwritten</p>
              <p className="font-mono-tabular mt-1 text-lg font-semibold text-rose-300">
                {scanResult.summary.overwritten}
              </p>
            </div>
            <div className="rounded-xl border border-rose-700/40 bg-[#111827] p-3.5">
              <p className="text-[11px] text-rose-400">Unrecoverable</p>
              <p className="font-mono-tabular mt-1 text-lg font-semibold text-rose-400">
                {scanResult.summary.unrecoverable}
              </p>
            </div>
            <div className="rounded-xl border border-slate-800 bg-[#111827] p-3.5">
              <p className="text-[11px] text-slate-400">Unknown</p>
              <p className="font-mono-tabular mt-1 text-lg font-semibold text-slate-300">
                {scanResult.summary.unknown}
              </p>
            </div>
            <div className="rounded-xl border border-slate-800 bg-[#111827] p-3.5">
              <p className="text-[11px] text-slate-400">Examined / Time</p>
              <p className="font-mono-tabular mt-1 text-sm font-semibold text-slate-100">
                {formatBytes(scanResult.bytes_examined)}
              </p>
              <p className="font-mono-tabular text-[11px] text-slate-400">
                {scanResult.scan_duration_ms.toFixed(1)} ms
              </p>
            </div>
          </div>

          <div className="rounded-xl border border-slate-800 bg-[#111827] px-4 py-2.5 text-xs text-slate-400">
            <span className="font-medium text-slate-300">Source SHA-256: </span>
            <span className="font-mono-tabular break-all text-cyan-300">{scanResult.source_disk_sha256}</span>
          </div>
        </section>
      )}

      {/* C & D. Results Table + Candidate Inspector */}
      {scanResult && (
        <div className="grid gap-6 xl:grid-cols-12">
          <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-7">
            <div className="flex flex-wrap items-center justify-between gap-3">
              <div className="flex flex-wrap items-center gap-2">
                <div className="relative">
                  <Search className="pointer-events-none absolute left-2.5 top-2 h-3.5 w-3.5 text-slate-500" aria-hidden="true" />
                  <input
                    type="search"
                    aria-label="Search candidates"
                    value={searchQuery}
                    onChange={(e) => setSearchQuery(e.target.value)}
                    placeholder="Search filename..."
                    className="rounded-lg border border-slate-700 bg-[#0b0f17] py-1.5 pl-8 pr-3 text-xs text-slate-100"
                  />
                </div>
                <select
                  aria-label="Filter by classification"
                  value={filterClass}
                  onChange={(e) => setFilterClass(e.target.value)}
                  className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-xs text-slate-100"
                >
                  <option value="">All Classifications</option>
                  {Object.entries(CLASSIFICATION_BADGE).map(([k, v]) => (
                    <option key={k} value={k}>
                      {v.label}
                    </option>
                  ))}
                </select>
                <select
                  aria-label="Filter by file type"
                  value={filterType}
                  onChange={(e) => setFilterType(e.target.value)}
                  className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-xs text-slate-100"
                >
                  <option value="">All Types</option>
                  <option value="png">PNG</option>
                  <option value="jpeg">JPEG</option>
                  <option value="pdf">PDF</option>
                  <option value="txt">TXT</option>
                  <option value="unknown">Unknown</option>
                </select>
                <select
                  aria-label="Sort candidates"
                  value={sortBy}
                  onChange={(e) => setSortBy(e.target.value as 'filename' | 'size' | 'recoverable')}
                  className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-xs text-slate-100"
                >
                  <option value="filename">Sort: Name</option>
                  <option value="size">Sort: Size</option>
                  <option value="recoverable">Sort: Recoverable</option>
                </select>
              </div>

              <div className="flex flex-wrap items-center gap-2">
                <button
                  type="button"
                  disabled={extracting || selectedIds.length === 0}
                  onClick={() => void handleExtractIds(selectedIds)}
                  className="rounded-lg border border-cyan-500/40 bg-cyan-500/10 px-3 py-1.5 text-xs font-medium text-cyan-200 hover:bg-cyan-500/20 disabled:opacity-40"
                >
                  Recover Selected ({selectedIds.length})
                </button>
                <button
                  type="button"
                  disabled={extracting || verifiedIds.length === 0}
                  onClick={() => void handleExtractIds(verifiedIds)}
                  className="rounded-lg bg-emerald-600 px-3 py-1.5 text-xs font-medium text-white hover:bg-emerald-500 disabled:opacity-40"
                >
                  Recover Verified ({verifiedIds.length})
                </button>
              </div>
            </div>

            {filteredCandidates.length === 0 ? (
              <p className="mt-6 text-center text-xs text-slate-400">No recovery candidates match the current filters.</p>
            ) : (
              <div className="mt-4 overflow-x-auto">
                <table className="w-full border-collapse text-left text-xs">
                  <caption className="sr-only">Forensic recovery candidates</caption>
                  <thead>
                    <tr className="border-b border-slate-800 text-slate-400">
                      <th scope="col" className="py-2 pr-2 font-medium">Sel</th>
                      <th scope="col" className="px-2 py-2 font-medium">Filename</th>
                      <th scope="col" className="px-2 py-2 font-medium">Type / Method</th>
                      <th scope="col" className="px-2 py-2 font-medium">Recoverable</th>
                      <th scope="col" className="px-2 py-2 font-medium">Classification</th>
                      <th scope="col" className="py-2 pl-2 font-medium">Blocks</th>
                    </tr>
                  </thead>
                  <tbody className="divide-y divide-slate-800/70">
                    {filteredCandidates.map((c) => {
                      const badge = CLASSIFICATION_BADGE[c.classification]
                      const checked = selectedIds.includes(c.candidate_id)
                      const isInspected = inspectedCandidate?.candidate_id === c.candidate_id
                      const extractable =
                        c.classification !== 'overwritten' && c.classification !== 'unrecoverable'
                      return (
                        <tr
                          key={c.candidate_id}
                          onClick={() => setInspectedCandidate(c)}
                          className={`cursor-pointer hover:bg-slate-800/40 ${
                            isInspected ? 'bg-cyan-500/10' : ''
                          }`}
                        >
                          <td className="py-2.5 pr-2" onClick={(e) => e.stopPropagation()}>
                            <input
                              type="checkbox"
                              aria-label={`Select ${c.filename}`}
                              disabled={!extractable}
                              checked={checked}
                              onChange={() =>
                                setSelectedIds((prev) =>
                                  prev.includes(c.candidate_id)
                                    ? prev.filter((id) => id !== c.candidate_id)
                                    : [...prev, c.candidate_id],
                                )
                              }
                            />
                          </td>
                          <td className="font-mono-tabular px-2 py-2.5 font-medium text-slate-100">
                            {c.filename}
                            {c.inode_id !== 0 && (
                              <span className="ml-1 text-[11px] text-slate-400">#{c.inode_id}</span>
                            )}
                          </td>
                          <td className="font-mono-tabular px-2 py-2.5 text-slate-300">
                            <span className="uppercase">{c.file_type}</span> · {c.recovery_method}
                          </td>
                          <td className="font-mono-tabular px-2 py-2.5 text-slate-300">
                            {formatBytes(c.recoverable_bytes)} / {formatBytes(c.original_size_bytes)}
                          </td>
                          <td className="px-2 py-2.5">
                            <span className={`inline-block rounded-full border px-2 py-0.5 text-[11px] ${badge.badgeClass}`}>
                              {badge.label}
                            </span>
                          </td>
                          <td className="font-mono-tabular py-2.5 pl-2 text-slate-400">
                            {c.source_blocks.length > 0
                              ? `${c.source_blocks[0]}..${c.source_blocks[c.source_blocks.length - 1]}`
                              : '—'}
                          </td>
                        </tr>
                      )
                    })}
                  </tbody>
                </table>
              </div>
            )}
          </section>

          {/* D & E. Candidate Inspector & Logical Block Visualization */}
          <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-5">
            <h2 className="text-sm font-semibold text-slate-100">Candidate Evidence Inspector</h2>
            {inspectedCandidate ? (
              <div className="mt-4 space-y-4">
                <dl className="grid grid-cols-2 gap-2.5 rounded-lg border border-slate-800 bg-[#0b0f17] p-3 text-xs">
                  <div>
                    <dt className="text-slate-400">Filename</dt>
                    <dd className="font-mono-tabular break-all text-slate-100">{inspectedCandidate.filename}</dd>
                  </div>
                  <div>
                    <dt className="text-slate-400">Integrity</dt>
                    <dd className="font-mono-tabular text-cyan-300">{inspectedCandidate.integrity_status}</dd>
                  </div>
                  <div>
                    <dt className="text-slate-400">Original Size</dt>
                    <dd className="font-mono-tabular text-slate-200">{inspectedCandidate.original_size_bytes} B</dd>
                  </div>
                  <div>
                    <dt className="text-slate-400">Recoverable</dt>
                    <dd className="font-mono-tabular text-slate-200">
                      {inspectedCandidate.recoverable_bytes} B ({inspectedCandidate.recovery_pct.toFixed(1)}%)
                    </dd>
                  </div>
                  {inspectedCandidate.has_expected_crc32 && (
                    <div className="col-span-2">
                      <dt className="text-slate-400">CRC32 (Expected vs Reconstructed)</dt>
                      <dd className="font-mono-tabular text-slate-200">
                        0x{inspectedCandidate.expected_crc32.toString(16).toUpperCase().padStart(8, '0')} vs 0x
                        {inspectedCandidate.reconstructed_crc32.toString(16).toUpperCase().padStart(8, '0')}
                      </dd>
                    </div>
                  )}
                </dl>

                {/* Logical Block Sequence Visualization */}
                {inspectedCandidate.block_statuses.length > 0 && (
                  <div>
                    <div className="flex items-center justify-between">
                      <h3 className="text-xs font-semibold uppercase tracking-wider text-slate-400">
                        Logical Block Sequence ({inspectedCandidate.block_statuses.length})
                      </h3>
                      {inspectedCandidate.source_blocks.length > 0 && (
                        <div className="flex flex-wrap items-center gap-3">
                          <button
                            type="button"
                            onClick={() =>
                              onOpenInHexViewer({
                                sourceType: 'block',
                                blockId: inspectedCandidate.source_blocks[0],
                              })
                            }
                            className="inline-flex items-center gap-1 text-xs text-cyan-300 hover:underline"
                          >
                            <Binary className="h-3.5 w-3.5" aria-hidden="true" />
                            <span>Inspect Blk {inspectedCandidate.source_blocks[0]} in Hex</span>
                          </button>
                          <button
                            type="button"
                            onClick={() =>
                              onHighlightInBlockExplorer(
                                inspectedCandidate.source_blocks,
                                `Recovery Candidate: ${inspectedCandidate.filename}`,
                              )
                            }
                            className="inline-flex items-center gap-1 text-xs text-cyan-300 hover:underline"
                          >
                            <Grid3x3 className="h-3.5 w-3.5" aria-hidden="true" />
                            <span>View in Block Explorer</span>
                          </button>
                        </div>
                      )}
                    </div>
                    <div className="mt-2 flex flex-wrap gap-1.5">
                      {inspectedCandidate.block_statuses.map((b) => (
                        <div
                          key={b.logical_index}
                          title={`Logical #${b.logical_index} -> Physical #${b.physical_block} (0x${b.disk_byte_offset.toString(16)}): ${b.detail}`}
                          className={`font-mono-tabular rounded border px-2 py-1 text-[11px] ${
                            LOGICAL_BLOCK_STYLE[b.state] ?? LOGICAL_BLOCK_STYLE.unknown
                          }`}
                        >
                          L{b.logical_index}:P{b.physical_block} ({b.state})
                        </div>
                      ))}
                    </div>
                  </div>
                )}

                {inspectedCandidate.evidence.length > 0 && (
                  <div className="rounded-lg border border-emerald-500/20 bg-emerald-500/5 p-3 text-xs text-emerald-200">
                    <p className="flex items-center gap-1.5 font-semibold">
                      <ShieldCheck className="h-3.5 w-3.5" aria-hidden="true" />
                      <span>Supporting Evidence</span>
                    </p>
                    <ul className="mt-1 list-disc space-y-0.5 pl-4">
                      {inspectedCandidate.evidence.map((e, i) => (
                        <li key={i}>{e}</li>
                      ))}
                    </ul>
                  </div>
                )}

                {inspectedCandidate.warnings.length > 0 && (
                  <div className="rounded-lg border border-amber-500/30 bg-amber-500/10 p-3 text-xs text-amber-200">
                    <p className="flex items-center gap-1.5 font-semibold">
                      <AlertTriangle className="h-3.5 w-3.5" aria-hidden="true" />
                      <span>Warnings &amp; Limitations</span>
                    </p>
                    <ul className="mt-1 list-disc space-y-0.5 pl-4">
                      {inspectedCandidate.warnings.map((w, i) => (
                        <li key={i}>{w}</li>
                      ))}
                    </ul>
                  </div>
                )}

                {inspectedCandidate.classification !== 'overwritten' &&
                  inspectedCandidate.classification !== 'unrecoverable' && (
                    <button
                      type="button"
                      disabled={extracting}
                      onClick={() => void handleExtractIds([inspectedCandidate.candidate_id])}
                      className="w-full rounded-lg bg-cyan-600 px-4 py-2 text-xs font-medium text-white hover:bg-cyan-500 disabled:opacity-50"
                    >
                      {extracting ? 'Extracting Artifact...' : 'Recover This Candidate'}
                    </button>
                  )}
              </div>
            ) : (
              <p className="mt-4 text-xs text-slate-400">
                Select a candidate from the results table to inspect its metadata, byte ranges, and block evidence.
              </p>
            )}
          </section>
        </div>
      )}

      {/* F. Extracted Artifacts & Safe Preview */}
      {extractedArtifacts.length > 0 && (
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5">
          <h2 className="text-sm font-semibold text-slate-100">
            Extracted Recovery Artifacts &amp; Provenance Manifests ({extractedArtifacts.length})
          </h2>
          <div className="mt-3 overflow-x-auto">
            <table className="w-full border-collapse text-left text-xs">
              <caption className="sr-only">Extracted recovery artifacts</caption>
              <thead>
                <tr className="border-b border-slate-800 text-slate-400">
                  <th scope="col" className="py-2 pr-3 font-medium">Artifact</th>
                  <th scope="col" className="px-3 py-2 font-medium">Recovered Size</th>
                  <th scope="col" className="px-3 py-2 font-medium">SHA-256</th>
                  <th scope="col" className="px-3 py-2 font-medium">Integrity</th>
                  <th scope="col" className="py-2 pl-3 text-right font-medium">Actions</th>
                </tr>
              </thead>
              <tbody className="divide-y divide-slate-800/70">
                {extractedArtifacts.map((m) => (
                  <tr key={m.recovery_id}>
                    <td className="font-mono-tabular py-2.5 pr-3 text-slate-100">{m.artifact_filename}</td>
                    <td className="font-mono-tabular px-3 py-2.5 text-slate-300">
                      {m.recovered_size} / {m.original_expected_size} B
                    </td>
                    <td className="font-mono-tabular px-3 py-2.5 text-cyan-300">
                      {m.recovered_sha256.slice(0, 16)}...
                    </td>
                    <td className="px-3 py-2.5">
                      <span className="inline-flex items-center gap-1 text-emerald-300">
                        <CheckCircle2 className="h-3.5 w-3.5" aria-hidden="true" />
                        <span>{m.integrity_result}</span>
                      </span>
                    </td>
                    <td className="py-2.5 pl-3 text-right">
                      <div className="inline-flex items-center gap-1.5">
                        <button
                          type="button"
                          onClick={() =>
                            onOpenInHexViewer({
                              sourceType: 'artifact',
                              artifactFilename: m.artifact_filename,
                            })
                          }
                          className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2 py-1 text-cyan-300 hover:border-cyan-500/50"
                        >
                          <Binary className="h-3.5 w-3.5" aria-hidden="true" />
                          <span>Hex</span>
                        </button>
                        <button
                          type="button"
                          onClick={() => void handlePreviewArtifact(m.recovery_id)}
                          className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2 py-1 text-slate-200 hover:border-cyan-500/50"
                        >
                          <Eye className="h-3.5 w-3.5" aria-hidden="true" />
                          <span>Preview</span>
                        </button>
                        <a
                          href={getArtifactDownloadUrl(m.recovery_id)}
                          download={m.artifact_filename}
                          className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2 py-1 text-slate-200 hover:border-cyan-500/50"
                        >
                          <Download className="h-3.5 w-3.5" aria-hidden="true" />
                          <span>File</span>
                        </a>
                        <a
                          href={getManifestDownloadUrl(m.recovery_id)}
                          download={`${m.recovery_id}.manifest.json`}
                          className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2 py-1 text-slate-200 hover:border-cyan-500/50"
                        >
                          <FileJson className="h-3.5 w-3.5" aria-hidden="true" />
                          <span>Manifest</span>
                        </a>
                      </div>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          {preview && (
            <div className="mt-4 rounded-lg border border-slate-700 bg-[#0b0f17] p-4">
              <div className="flex items-center justify-between">
                <h3 className="text-xs font-semibold text-slate-200">
                  Safe Preview — {preview.manifest.artifact_filename}
                </h3>
                <button
                  type="button"
                  onClick={() => setPreview(null)}
                  className="text-xs text-slate-400 hover:text-slate-200"
                >
                  Close Preview
                </button>
              </div>
              {preview.is_image && preview.image_base64 && preview.image_mime ? (
                <img
                  src={`data:${preview.image_mime};base64,${preview.image_base64}`}
                  alt={`Recovered ${preview.manifest.original_filename}`}
                  className="mt-3 max-h-64 rounded border border-slate-800 object-contain"
                />
              ) : preview.is_text && preview.text_preview !== null ? (
                <pre className="font-mono-tabular mt-3 max-h-64 overflow-auto whitespace-pre-wrap rounded border border-slate-800 bg-slate-950 p-3 text-xs text-slate-200">
                  {preview.text_preview}
                </pre>
              ) : (
                <p className="mt-2 flex items-center gap-1.5 text-xs text-slate-400">
                  <ShieldAlert className="h-4 w-4 text-amber-400" aria-hidden="true" />
                  <span>Binary or non-UTF-8 content; active scripts are never executed. Download the artifact to inspect externally.</span>
                </p>
              )}
            </div>
          )}
        </section>
      )}
    </div>
  )
}
