import { useCallback, useEffect, useState, type FormEvent } from 'react'
import {
  Activity,
  Binary,
  FlaskConical,
  Grid3x3,
  HardDrive,
  Plus,
  RefreshCw,
  Search,
  Trash2,
} from 'lucide-react'
import {
  Bar,
  BarChart,
  CartesianGrid,
  Legend,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import {
  compareAllocPolicies,
  getBlockMap,
  getFragmentationReport,
  inspectBlock,
  setAllocPolicy,
} from '../services/api'
import type {
  AllocPolicy,
  BlockClassification,
  BlockInfo,
  BlockMapPage,
  BlockSize,
  ComparisonExperimentResult,
  FragmentationReport,
  HexNavigationTarget,
  ReportNavigationContext,
  VolumeStatus,
  WorkloadStep,
} from '../types/api'

interface BlockExplorerPageProps {
  activeDisk: VolumeStatus | null
  externalHighlightBlocks?: number[]
  externalHighlightLabel?: string | null
  onClearExternalHighlight?: () => void
  onUpdateVolume: (vol: VolumeStatus) => void
  onGoToDiskManager: () => void
  onOpenInHexViewer: (target: HexNavigationTarget) => void
  onExportReport: (ctx: ReportNavigationContext) => void
  onNotify: (kind: 'success' | 'error', message: string) => void
}

interface ClassificationMeta {
  label: string
  shortCode: string
  tileClass: string
  dotClass: string
}

const CLASSIFICATION_META: Record<BlockClassification, ClassificationMeta> = {
  reserved: {
    label: 'Boot / Reserved',
    shortCode: 'BT',
    tileClass: 'border-slate-500 bg-slate-700/80 text-slate-100',
    dotClass: 'bg-slate-500',
  },
  primary_superblock: {
    label: 'Primary Superblock',
    shortCode: 'S1',
    tileClass: 'border-indigo-400 bg-indigo-600/80 text-white',
    dotClass: 'bg-indigo-400',
  },
  backup_superblock: {
    label: 'Backup Superblock',
    shortCode: 'S2',
    tileClass: 'border-violet-400 bg-violet-600/70 text-white',
    dotClass: 'bg-violet-400',
  },
  inode_bitmap: {
    label: 'Inode Bitmap',
    shortCode: 'IB',
    tileClass: 'border-purple-400/70 bg-purple-700/60 text-purple-100',
    dotClass: 'bg-purple-400',
  },
  block_bitmap: {
    label: 'Block Bitmap',
    shortCode: 'BB',
    tileClass: 'border-fuchsia-400/70 bg-fuchsia-700/60 text-fuchsia-100',
    dotClass: 'bg-fuchsia-400',
  },
  inode_table: {
    label: 'Inode Table',
    shortCode: 'IT',
    tileClass: 'border-blue-400/70 bg-blue-700/60 text-blue-100',
    dotClass: 'bg-blue-400',
  },
  directory_data: {
    label: 'Directory Data',
    shortCode: 'DR',
    tileClass: 'border-amber-400/80 bg-amber-600/70 text-amber-50',
    dotClass: 'bg-amber-400',
  },
  file_data: {
    label: 'File Data',
    shortCode: 'FD',
    tileClass: 'border-cyan-400/80 bg-cyan-600/70 text-cyan-50',
    dotClass: 'bg-cyan-400',
  },
  indirect_pointer: {
    label: 'Indirect Pointer',
    shortCode: 'IN',
    tileClass: 'border-teal-400/80 bg-teal-600/70 text-teal-50',
    dotClass: 'bg-teal-400',
  },
  journal: {
    label: 'Metadata Journal',
    shortCode: 'JN',
    tileClass: 'border-sky-400/70 bg-sky-700/60 text-sky-100',
    dotClass: 'bg-sky-400',
  },
  free: {
    label: 'Free Block',
    shortCode: 'FR',
    tileClass: 'border-slate-700 bg-slate-900/90 text-slate-400',
    dotClass: 'bg-slate-800 border border-slate-600',
  },
  free_deleted_residual: {
    label: 'Free (Deleted Residual)',
    shortCode: 'DL',
    tileClass: 'border-orange-400/80 border-dashed bg-orange-950/70 text-orange-200',
    dotClass: 'bg-orange-500',
  },
  inconsistent: {
    label: 'Inconsistent / Unknown',
    shortCode: 'ER',
    tileClass: 'border-rose-500 bg-rose-700/80 text-white',
    dotClass: 'bg-rose-500',
  },
}

const PRESET_6_4_8_WORKLOAD: WorkloadStep[] = [
  { op: 'create', path: '/h1_6blk.bin', size_bytes: 6 * 1024 },
  { op: 'create', path: '/sep1.bin', size_bytes: 2 * 1024 },
  { op: 'create', path: '/h2_4blk.bin', size_bytes: 4 * 1024 },
  { op: 'create', path: '/sep2.bin', size_bytes: 2 * 1024 },
  { op: 'create', path: '/h3_8blk.bin', size_bytes: 8 * 1024 },
  { op: 'create', path: '/sep3.bin', size_bytes: 2 * 1024 },
  { op: 'delete', path: '/h1_6blk.bin', size_bytes: 0 },
  { op: 'delete', path: '/h2_4blk.bin', size_bytes: 0 },
  { op: 'delete', path: '/h3_8blk.bin', size_bytes: 0 },
  { op: 'create', path: '/probe_3blk.bin', size_bytes: 3 * 1024 },
]

export function BlockExplorerPage({
  activeDisk,
  externalHighlightBlocks = [],
  externalHighlightLabel = null,
  onClearExternalHighlight,
  onUpdateVolume,
  onGoToDiskManager,
  onOpenInHexViewer,
  onExportReport,
  onNotify,
}: BlockExplorerPageProps) {
  const [activeTab, setActiveTab] = useState<'grid' | 'compare'>('grid')
  const [report, setReport] = useState<FragmentationReport | null>(null)
  const [pageData, setPageData] = useState<BlockMapPage | null>(null)
  const [offset, setOffset] = useState(0)
  const [limit, setLimit] = useState(256)
  const [typeFilter, setTypeFilter] = useState<string>('')
  const [highlightInode, setHighlightInode] = useState<number>(0)
  const [highlightFreeExtents, setHighlightFreeExtents] = useState<boolean>(false)
  const [zoom, setZoom] = useState<'sm' | 'md' | 'lg'>('md')
  const [searchBlockInput, setSearchBlockInput] = useState('')
  const [selectedBlock, setSelectedBlock] = useState<BlockInfo | null>(null)
  const [hoveredBlock, setHoveredBlock] = useState<BlockInfo | null>(null)
  const [loading, setLoading] = useState(false)
  const [updatingPolicy, setUpdatingPolicy] = useState(false)

  // Comparison lab state
  const [useActiveDisk, setUseActiveDisk] = useState(false)
  const [cmpBlockSize, setCmpBlockSize] = useState<BlockSize>(1024)
  const [cmpTotalBlocks, setCmpTotalBlocks] = useState(128)
  const [workload, setWorkload] = useState<WorkloadStep[]>(PRESET_6_4_8_WORKLOAD)
  const [newStepOp, setNewStepOp] = useState<'create' | 'delete'>('create')
  const [newStepPath, setNewStepPath] = useState('/sample.bin')
  const [newStepBytes, setNewStepBytes] = useState(4096)
  const [runningCompare, setRunningCompare] = useState(false)
  const [cmpResult, setCmpResult] = useState<ComparisonExperimentResult | null>(null)

  const diskName = activeDisk?.disk_name ?? null

  const refreshAll = useCallback(async () => {
    if (!diskName) return
    setLoading(true)
    try {
      const [rep, map] = await Promise.all([
        getFragmentationReport(diskName),
        getBlockMap(diskName, { offset, limit, type_filter: typeFilter }),
      ])
      setReport(rep)
      setPageData(map)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to load block map')
    } finally {
      setLoading(false)
    }
  }, [diskName, offset, limit, typeFilter, onNotify])

  useEffect(() => {
    if (diskName) {
      void refreshAll()
    } else {
      setReport(null)
      setPageData(null)
      setSelectedBlock(null)
    }
  }, [diskName, refreshAll])

  useEffect(() => {
    if (externalHighlightBlocks.length > 0) {
      setActiveTab('grid')
      setTypeFilter('')
      setOffset(Math.floor(externalHighlightBlocks[0] / limit) * limit)
    }
  }, [externalHighlightBlocks, limit])

  if (!activeDisk) {
    return (
      <section className="rounded-xl border border-dashed border-slate-700 bg-[#111827] p-10 text-center">
        <HardDrive className="mx-auto h-10 w-10 text-slate-500" aria-hidden="true" />
        <h2 className="mt-3 text-base font-semibold text-slate-100">No Virtual Disk Mounted</h2>
        <p className="mt-1 text-sm text-slate-400">
          Create or mount a GFS1 `.gfs` disk in the Disk Manager to inspect block allocation and fragmentation.
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

  async function handlePolicySwitch(policy: AllocPolicy) {
    if (!diskName) return
    setUpdatingPolicy(true)
    try {
      const updated = await setAllocPolicy(diskName, policy)
      onUpdateVolume(updated)
      await refreshAll()
      onNotify('success', `Active policy updated to ${policy} (existing files unchanged)`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to update policy')
    } finally {
      setUpdatingPolicy(false)
    }
  }

  async function handleSearchBlock(e: FormEvent) {
    e.preventDefault()
    if (!diskName) return
    const id = Number.parseInt(searchBlockInput.trim(), 10)
    if (Number.isNaN(id) || id < 0) {
      onNotify('error', 'Enter a valid non-negative block number')
      return
    }
    try {
      const info = await inspectBlock(diskName, id)
      setSelectedBlock(info)
      setTypeFilter('')
      setOffset(Math.floor(id / limit) * limit)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Block lookup failed')
    }
  }

  async function handleSelectTile(blk: BlockInfo) {
    if (!diskName) return
    try {
      const fresh = await inspectBlock(diskName, blk.block_id)
      setSelectedBlock(fresh)
    } catch {
      setSelectedBlock(blk)
    }
  }

  function handleAddWorkloadStep(e: FormEvent) {
    e.preventDefault()
    const cleanPath = newStepPath.trim().startsWith('/') ? newStepPath.trim() : `/${newStepPath.trim()}`
    if (cleanPath.length < 2) return
    setWorkload((prev) => [
      ...prev,
      {
        op: newStepOp,
        path: cleanPath,
        size_bytes: newStepOp === 'create' ? Math.max(0, newStepBytes) : 0,
      },
    ])
  }

  async function handleRunComparison() {
    if (!diskName || workload.length === 0) return
    setRunningCompare(true)
    try {
      const res = await compareAllocPolicies(diskName, {
        block_size: cmpBlockSize,
        total_blocks: cmpTotalBlocks,
        use_active_disk: useActiveDisk,
        workload,
      })
      setCmpResult(res)
      onNotify('success', 'Completed 4-policy comparison experiment in isolated sandbox')
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Comparison experiment failed')
    } finally {
      setRunningCompare(false)
    }
  }

  const tileSizeClass =
    zoom === 'sm'
      ? 'h-6 w-6 text-[9px]'
      : zoom === 'lg'
        ? 'h-11 w-11 text-[11px]'
        : 'h-8 w-8 text-[10px]'

  const totalFiltered = pageData?.filtered_total ?? 0
  const hasPrev = offset > 0
  const hasNext = offset + limit < totalFiltered

  const compareChartData =
    cmpResult?.policies.map((p) => ({
      policy: p.policy.replace('_fit', '').replace('bitmap_', ''),
      extFragPct: Number(p.final_report.external_fragmentation_pct.toFixed(2)),
      fragmentedFiles: p.final_report.fragmented_file_count,
      extentsExamined: p.total_extents_examined,
    })) ?? []

  return (
    <div className="space-y-6">
      <div className="flex flex-wrap items-center justify-between gap-4 rounded-xl border border-slate-800 bg-[#111827] px-5 py-3">
        <div role="tablist" aria-label="Block Explorer views" className="flex items-center gap-2">
          <button
            type="button"
            role="tab"
            aria-selected={activeTab === 'grid'}
            onClick={() => setActiveTab('grid')}
            className={`inline-flex items-center gap-2 rounded-lg px-3.5 py-2 text-xs font-medium transition-colors ${
              activeTab === 'grid'
                ? 'border border-cyan-500/40 bg-cyan-500/15 text-cyan-200'
                : 'border border-transparent text-slate-300 hover:bg-slate-800'
            }`}
          >
            <Grid3x3 className="h-4 w-4" aria-hidden="true" />
            <span>Block Grid &amp; Fragmentation</span>
          </button>
          <button
            type="button"
            role="tab"
            aria-selected={activeTab === 'compare'}
            onClick={() => setActiveTab('compare')}
            className={`inline-flex items-center gap-2 rounded-lg px-3.5 py-2 text-xs font-medium transition-colors ${
              activeTab === 'compare'
                ? 'border border-cyan-500/40 bg-cyan-500/15 text-cyan-200'
                : 'border border-transparent text-slate-300 hover:bg-slate-800'
            }`}
          >
            <FlaskConical className="h-4 w-4" aria-hidden="true" />
            <span>Allocation Comparison Lab</span>
          </button>
        </div>

        <div className="flex items-center gap-3">
          <label htmlFor="blk-policy-select" className="text-xs text-slate-400">
            Active Policy:
          </label>
          <select
            id="blk-policy-select"
            disabled={updatingPolicy || activeDisk.dirty_journal_detected}
            value={activeDisk.alloc_policy}
            onChange={(e) => void handlePolicySwitch(e.target.value as AllocPolicy)}
            className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-xs text-slate-100 disabled:opacity-50"
          >
            <option value="first_fit">First Fit</option>
            <option value="best_fit">Best Fit</option>
            <option value="worst_fit">Worst Fit</option>
            <option value="bitmap_next_fit">Bitmap Next Fit</option>
          </select>
          <button
            type="button"
            onClick={() => void refreshAll()}
            disabled={loading}
            className="inline-flex items-center gap-1.5 rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-xs text-slate-200 hover:border-cyan-500/50"
          >
            <RefreshCw className={`h-3.5 w-3.5 ${loading ? 'animate-spin' : ''}`} aria-hidden="true" />
            <span>Refresh</span>
          </button>
        </div>
      </div>

      {activeTab === 'grid' && (
        <div className="space-y-6">
          {report && (
            <section className="grid gap-4 sm:grid-cols-2 lg:grid-cols-4">
              <div className="rounded-xl border border-slate-800 bg-[#111827] p-4">
                <p className="text-xs text-slate-400">Disk &amp; Data Utilization</p>
                <p className="font-mono-tabular mt-1.5 text-lg font-semibold text-slate-100">
                  {report.disk_utilization_pct.toFixed(1)}%
                </p>
                <p className="font-mono-tabular mt-1 text-xs text-slate-400">
                  {report.allocated_data_blocks} / {report.data_blocks_count} data blks ({report.indirect_pointer_blocks} ind)
                </p>
              </div>
              <div className="rounded-xl border border-slate-800 bg-[#111827] p-4">
                <p className="text-xs text-slate-400">Free Extents &amp; Largest Run</p>
                <p className="font-mono-tabular mt-1.5 text-lg font-semibold text-emerald-300">
                  {report.largest_free_extent} blks max
                </p>
                <p className="font-mono-tabular mt-1 text-xs text-slate-400">
                  {report.free_extent_count} extents · avg {report.average_free_extent.toFixed(1)} blks
                </p>
              </div>
              <div className="rounded-xl border border-slate-800 bg-[#111827] p-4">
                <p className="text-xs text-slate-400">External Free-Space Frag</p>
                <p className="font-mono-tabular mt-1.5 text-lg font-semibold text-amber-300">
                  {report.external_fragmentation_pct.toFixed(2)}%
                </p>
                <p className="font-mono-tabular mt-1 text-xs text-slate-400">
                  (1 - {report.largest_free_extent}/{report.free_blocks || 0}) x 100
                </p>
              </div>
              <div className="rounded-xl border border-slate-800 bg-[#111827] p-4">
                <p className="text-xs text-slate-400">Internal &amp; File Fragmentation</p>
                <p className="font-mono-tabular mt-1.5 text-lg font-semibold text-cyan-300">
                  {report.internal_fragmentation_bytes} B ({report.internal_fragmentation_pct.toFixed(1)}%)
                </p>
                <p className="font-mono-tabular mt-1 text-xs text-slate-400">
                  {report.fragmented_file_count} fragmented / {report.total_files} files
                </p>
                <button
                  type="button"
                  onClick={() =>
                    onExportReport({
                      reportType: 'disk_analysis',
                      diskName: activeDisk.disk_name,
                      title: `Allocation & Fragmentation — ${activeDisk.disk_name}`,
                    })
                  }
                  className="mt-2 rounded border border-emerald-500/40 bg-emerald-500/10 px-2 py-0.5 text-[11px] text-emerald-300 hover:bg-emerald-500/20"
                >
                  Export Fragmentation Report
                </button>
              </div>
            </section>
          )}

          <div className="grid gap-6 xl:grid-cols-12">
            <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-8">
              <div className="flex flex-wrap items-center justify-between gap-3">
                <form onSubmit={(e) => void handleSearchBlock(e)} className="flex items-center gap-2">
                  <label htmlFor="block-search-input" className="sr-only">Search block number</label>
                  <input
                    id="block-search-input"
                    type="text"
                    inputMode="numeric"
                    value={searchBlockInput}
                    onChange={(e) => setSearchBlockInput(e.target.value)}
                    placeholder="Jump to Block #"
                    className="font-mono-tabular w-36 rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-xs text-slate-100"
                  />
                  <button
                    type="submit"
                    className="inline-flex items-center gap-1 rounded-lg border border-slate-700 bg-slate-800 px-2.5 py-1.5 text-xs text-slate-200 hover:border-cyan-500/50"
                  >
                    <Search className="h-3.5 w-3.5" aria-hidden="true" />
                    <span>Go</span>
                  </button>
                </form>

                <div className="flex flex-wrap items-center gap-2">
                  <select
                    aria-label="Filter by block classification"
                    value={typeFilter}
                    onChange={(e) => {
                      setTypeFilter(e.target.value)
                      setOffset(0)
                    }}
                    className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-xs text-slate-100"
                  >
                    <option value="">All Block Types</option>
                    {Object.entries(CLASSIFICATION_META).map(([k, meta]) => (
                      <option key={k} value={k}>
                        {meta.label}
                      </option>
                    ))}
                  </select>

                  <select
                    aria-label="Highlight blocks of file"
                    value={highlightInode}
                    onChange={(e) => setHighlightInode(Number(e.target.value))}
                    className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-xs text-slate-100"
                  >
                    <option value={0}>Highlight File: None</option>
                    {report?.files.map((f) => (
                      <option key={f.inode_id} value={f.inode_id}>
                        {f.path} (inode #{f.inode_id})
                      </option>
                    ))}
                  </select>

                  <label className="flex items-center gap-1.5 rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-xs text-slate-300">
                    <input
                      type="checkbox"
                      checked={highlightFreeExtents}
                      onChange={(e) => setHighlightFreeExtents(e.target.checked)}
                    />
                    <span>Free Extents</span>
                  </label>

                  <select
                    aria-label="Grid zoom"
                    value={zoom}
                    onChange={(e) => setZoom(e.target.value as 'sm' | 'md' | 'lg')}
                    className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2 py-1.5 text-xs text-slate-100"
                  >
                    <option value="sm">Zoom S</option>
                    <option value="md">Zoom M</option>
                    <option value="lg">Zoom L</option>
                  </select>

                  <select
                    aria-label="Blocks per page"
                    value={limit}
                    onChange={(e) => {
                      setLimit(Number(e.target.value))
                      setOffset(0)
                    }}
                    className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2 py-1.5 text-xs text-slate-100"
                  >
                    <option value={128}>128 / page</option>
                    <option value={256}>256 / page</option>
                    <option value={512}>512 / page</option>
                  </select>
                </div>
              </div>

              <div className="mt-3 flex flex-wrap gap-2 rounded-lg border border-slate-800 bg-[#0b0f17]/70 p-2.5">
                {Object.entries(CLASSIFICATION_META).map(([key, meta]) => (
                  <span key={key} className="inline-flex items-center gap-1.5 text-[11px] text-slate-300">
                    <span className={`h-2.5 w-2.5 rounded-sm ${meta.dotClass}`} aria-hidden="true" />
                    <span>{meta.label}</span>
                  </span>
                ))}
              </div>

              {externalHighlightBlocks.length > 0 && (
                <div className="mt-3 flex items-center justify-between rounded-lg border border-cyan-500/40 bg-cyan-500/10 px-3 py-2 text-xs text-cyan-200">
                  <span>
                    Highlighting {externalHighlightBlocks.length} block(s) for {externalHighlightLabel ?? 'Recovery Candidate'}
                  </span>
                  {onClearExternalHighlight && (
                    <button
                      type="button"
                      onClick={onClearExternalHighlight}
                      className="rounded px-2 py-0.5 text-cyan-100 hover:bg-cyan-500/20"
                    >
                      Clear Highlight
                    </button>
                  )}
                </div>
              )}

              <div className="mt-4 flex flex-wrap gap-1.5">
                {pageData?.blocks.map((blk) => {
                  const meta = CLASSIFICATION_META[blk.classification] ?? CLASSIFICATION_META.inconsistent
                  const isSelected = selectedBlock?.block_id === blk.block_id
                  const isExternalHighlight = externalHighlightBlocks.includes(blk.block_id)
                  const isHighlightedFile = highlightInode !== 0 && blk.inode_id === highlightInode
                  const isFreeExtentHighlight =
                    highlightFreeExtents &&
                    (blk.classification === 'free' || blk.classification === 'free_deleted_residual')
                  const dimmed =
                    (externalHighlightBlocks.length > 0 && !isExternalHighlight) ||
                    (highlightInode !== 0 && !isHighlightedFile) ||
                    (highlightFreeExtents && !isFreeExtentHighlight)

                  return (
                    <button
                      key={blk.block_id}
                      type="button"
                      onClick={() => void handleSelectTile(blk)}
                      onMouseEnter={() => setHoveredBlock(blk)}
                      onMouseLeave={() => setHoveredBlock(null)}
                      title={`Block #${blk.block_id} · ${meta.label}${blk.owner_path ? ` · ${blk.owner_path}` : ''}`}
                      className={`font-mono-tabular flex items-center justify-center rounded border transition-transform ${tileSizeClass} ${meta.tileClass} ${
                        isSelected ? 'ring-2 ring-white scale-105' : ''
                      } ${isExternalHighlight || isHighlightedFile || isFreeExtentHighlight ? 'ring-2 ring-cyan-300' : ''} ${
                        dimmed ? 'opacity-25' : ''
                      }`}
                    >
                      {zoom === 'sm' ? '' : blk.block_id}
                    </button>
                  )
                })}
              </div>

              <div className="mt-4 flex flex-wrap items-center justify-between gap-3 border-t border-slate-800 pt-3 text-xs text-slate-400">
                <div className="font-mono-tabular">
                  {hoveredBlock ? (
                    <span>
                      Hover: Blk #{hoveredBlock.block_id} (0x{hoveredBlock.byte_offset.toString(16)}) ·{' '}
                      {CLASSIFICATION_META[hoveredBlock.classification]?.label}
                      {hoveredBlock.owner_path ? ` · ${hoveredBlock.owner_path}` : ''}
                    </span>
                  ) : (
                    <span>
                      Showing {pageData?.blocks.length ?? 0} of {totalFiltered} matching blocks (offset {offset})
                    </span>
                  )}
                </div>
                <div className="flex items-center gap-2">
                  <button
                    type="button"
                    disabled={!hasPrev}
                    onClick={() => setOffset(Math.max(0, offset - limit))}
                    className="rounded border border-slate-700 bg-slate-800 px-2.5 py-1 text-slate-200 disabled:opacity-40"
                  >
                    Previous
                  </button>
                  <button
                    type="button"
                    disabled={!hasNext}
                    onClick={() => setOffset(offset + limit)}
                    className="rounded border border-slate-700 bg-slate-800 px-2.5 py-1 text-slate-200 disabled:opacity-40"
                  >
                    Next
                  </button>
                </div>
              </div>
            </section>

            <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-4">
              <div className="flex items-center justify-between gap-2">
                <h2 className="text-sm font-semibold text-slate-100">Selected Block Inspector</h2>
                {selectedBlock && (
                  <button
                    type="button"
                    onClick={() =>
                      onOpenInHexViewer({
                        sourceType: 'block',
                        blockId: selectedBlock.block_id,
                      })
                    }
                    className="inline-flex items-center gap-1 rounded-md border border-slate-700 bg-slate-800/80 px-2.5 py-1 text-xs text-cyan-300 hover:border-cyan-500/50"
                  >
                    <Binary className="h-3.5 w-3.5" aria-hidden="true" />
                    <span>Open in Hex Viewer</span>
                  </button>
                )}
              </div>
              {selectedBlock ? (
                <dl className="mt-4 space-y-2.5 rounded-lg border border-slate-800 bg-[#0b0f17] p-4 text-xs">
                  <div className="flex justify-between gap-2">
                    <dt className="text-slate-400">Block Number</dt>
                    <dd className="font-mono-tabular font-semibold text-slate-100">#{selectedBlock.block_id}</dd>
                  </div>
                  <div className="flex justify-between gap-2">
                    <dt className="text-slate-400">Byte Offset</dt>
                    <dd className="font-mono-tabular text-slate-200">
                      0x{selectedBlock.byte_offset.toString(16).toUpperCase()} ({selectedBlock.byte_offset} B)
                    </dd>
                  </div>
                  <div className="flex justify-between gap-2">
                    <dt className="text-slate-400">Block Size</dt>
                    <dd className="font-mono-tabular text-slate-200">{selectedBlock.block_size} B</dd>
                  </div>
                  <div className="flex justify-between gap-2">
                    <dt className="text-slate-400">Bitmap State</dt>
                    <dd className={selectedBlock.allocated ? 'text-cyan-300' : 'text-emerald-300'}>
                      {selectedBlock.allocated ? 'Allocated (1)' : 'Free (0)'}
                    </dd>
                  </div>
                  <div className="flex justify-between gap-2">
                    <dt className="text-slate-400">Classification</dt>
                    <dd className="font-medium text-slate-100">
                      {CLASSIFICATION_META[selectedBlock.classification]?.label ?? selectedBlock.classification}
                    </dd>
                  </div>
                  <div className="flex justify-between gap-2">
                    <dt className="text-slate-400">Owning Inode / Path</dt>
                    <dd className="font-mono-tabular text-right text-slate-200">
                      {selectedBlock.inode_id !== 0
                        ? `#${selectedBlock.inode_id} (${selectedBlock.owner_path || 'unknown'})`
                        : 'System / Unowned'}
                    </dd>
                  </div>
                  <div className="flex justify-between gap-2">
                    <dt className="text-slate-400">Contiguous Extent</dt>
                    <dd className="font-mono-tabular text-slate-200">
                      {selectedBlock.extent_length > 0
                        ? `Blk ${selectedBlock.extent_start}..${selectedBlock.extent_start + selectedBlock.extent_length - 1} (len ${selectedBlock.extent_length})`
                        : '—'}
                    </dd>
                  </div>
                </dl>
              ) : (
                <p className="mt-4 text-xs text-slate-400">
                  Click any block tile in the grid or search by block number to inspect its physical offset, owner, and extent boundaries.
                </p>
              )}

              {report && report.free_extents.length > 0 && (
                <div className="mt-5">
                  <h3 className="text-xs font-semibold uppercase tracking-wider text-slate-400">
                    Contiguous Free Extents ({report.free_extents.length})
                  </h3>
                  <ul className="mt-2 max-h-48 space-y-1 overflow-y-auto text-xs">
                    {report.free_extents.map((ext) => (
                      <li
                        key={ext.start_block}
                        className="font-mono-tabular flex justify-between rounded border border-slate-800 bg-[#0b0f17] px-2.5 py-1 text-slate-300"
                      >
                        <span>
                          Blk {ext.start_block}..{ext.start_block + ext.length - 1}
                        </span>
                        <span className="text-emerald-300">{ext.length} blks</span>
                      </li>
                    ))}
                  </ul>
                </div>
              )}
            </section>
          </div>

          {report && (
            <section className="rounded-xl border border-slate-800 bg-[#111827] p-5">
              <h2 className="text-sm font-semibold text-slate-100">
                Per-File Fragmentation &amp; Physical Block Mappings ({report.files.length})
              </h2>
              {report.files.length === 0 ? (
                <p className="mt-3 text-xs text-slate-400">No regular files on volume yet.</p>
              ) : (
                <div className="mt-3 overflow-x-auto">
                  <table className="w-full border-collapse text-left text-xs">
                    <caption className="sr-only">Per-file fragmentation analysis</caption>
                    <thead>
                      <tr className="border-b border-slate-800 text-slate-400">
                        <th scope="col" className="py-2 pr-3 font-medium">File</th>
                        <th scope="col" className="px-3 py-2 font-medium">Logical / Capacity</th>
                        <th scope="col" className="px-3 py-2 font-medium">Internal Frag</th>
                        <th scope="col" className="px-3 py-2 font-medium">Extents</th>
                        <th scope="col" className="py-2 pl-3 font-medium">Physical Data Blocks</th>
                      </tr>
                    </thead>
                    <tbody className="divide-y divide-slate-800/70">
                      {report.files.map((f) => (
                        <tr
                          key={f.inode_id}
                          onClick={() => setHighlightInode(highlightInode === f.inode_id ? 0 : f.inode_id)}
                          className={`cursor-pointer hover:bg-slate-800/40 ${
                            highlightInode === f.inode_id ? 'bg-cyan-500/10' : ''
                          }`}
                        >
                          <td className="font-mono-tabular py-2.5 pr-3 text-slate-100">
                            {f.path} <span className="text-slate-400">(#{f.inode_id})</span>
                          </td>
                          <td className="font-mono-tabular px-3 py-2.5 text-slate-300">
                            {f.size_bytes} B / {f.allocated_payload_capacity_bytes} B
                          </td>
                          <td className="font-mono-tabular px-3 py-2.5 text-slate-300">
                            {f.internal_fragmentation_bytes} B ({f.internal_fragmentation_pct.toFixed(1)}%)
                          </td>
                          <td className="px-3 py-2.5">
                            <span
                              className={`rounded px-2 py-0.5 text-[11px] font-medium ${
                                f.is_fragmented
                                  ? 'bg-amber-500/15 text-amber-300'
                                  : 'bg-emerald-500/15 text-emerald-300'
                              }`}
                            >
                              {f.extent_count} {f.is_fragmented ? '(Fragmented)' : '(Contiguous)'}
                            </span>
                          </td>
                          <td className="font-mono-tabular py-2.5 pl-3 text-slate-400">
                            [{f.physical_data_blocks.slice(0, 12).join(', ')}
                            {f.physical_data_blocks.length > 12 ? ', ...' : ''}]
                          </td>
                        </tr>
                      ))}
                    </tbody>
                  </table>
                </div>
              )}
            </section>
          )}
        </div>
      )}

      {activeTab === 'compare' && (
        <div className="space-y-6">
          <div className="grid gap-6 xl:grid-cols-12">
            <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-5">
              <div className="flex items-center justify-between gap-2">
                <h2 className="text-sm font-semibold text-slate-100">Reproducible Workload Builder</h2>
                <button
                  type="button"
                  onClick={() => setWorkload(PRESET_6_4_8_WORKLOAD)}
                  className="rounded border border-cyan-500/40 bg-cyan-500/10 px-2.5 py-1 text-xs text-cyan-200 hover:bg-cyan-500/20"
                >
                  Load 6/4/8 Hole Preset
                </button>
              </div>
              <p className="mt-1 text-xs text-slate-400">
                Runs the identical create/delete sequence across all 4 policies on isolated disposable images in{' '}
                <span className="font-mono-tabular">disk_images/sandbox/</span>.
              </p>

              <div className="mt-4 space-y-3 rounded-lg border border-slate-800 bg-[#0b0f17] p-3 text-xs">
                <label className="flex items-center gap-2 text-slate-200">
                  <input
                    type="checkbox"
                    checked={useActiveDisk}
                    onChange={(e) => setUseActiveDisk(e.target.checked)}
                  />
                  <span>Clone active disk ({activeDisk.disk_name}) instead of fresh disk</span>
                </label>
                {!useActiveDisk && (
                  <div className="grid grid-cols-2 gap-3">
                    <div>
                      <label htmlFor="cmp-bs" className="block text-slate-400">Block Size</label>
                      <select
                        id="cmp-bs"
                        value={cmpBlockSize}
                        onChange={(e) => setCmpBlockSize(Number(e.target.value) as BlockSize)}
                        className="mt-1 w-full rounded border border-slate-700 bg-slate-900 px-2 py-1 text-slate-100"
                      >
                        <option value={512}>512B</option>
                        <option value={1024}>1024B</option>
                        <option value={2048}>2048B</option>
                        <option value={4096}>4096B</option>
                      </select>
                    </div>
                    <div>
                      <label htmlFor="cmp-blks" className="block text-slate-400">Total Blocks</label>
                      <input
                        id="cmp-blks"
                        type="number"
                        min={64}
                        max={4096}
                        value={cmpTotalBlocks}
                        onChange={(e) => setCmpTotalBlocks(Number(e.target.value))}
                        className="font-mono-tabular mt-1 w-full rounded border border-slate-700 bg-slate-900 px-2 py-1 text-slate-100"
                      />
                    </div>
                  </div>
                )}
              </div>

              <form onSubmit={handleAddWorkloadStep} className="mt-4 flex flex-wrap items-end gap-2 text-xs">
                <div>
                  <label htmlFor="step-op" className="block text-slate-400">Op</label>
                  <select
                    id="step-op"
                    value={newStepOp}
                    onChange={(e) => setNewStepOp(e.target.value as 'create' | 'delete')}
                    className="mt-1 rounded border border-slate-700 bg-[#0b0f17] px-2 py-1.5 text-slate-100"
                  >
                    <option value="create">Create</option>
                    <option value="delete">Delete</option>
                  </select>
                </div>
                <div className="flex-1">
                  <label htmlFor="step-path" className="block text-slate-400">Path</label>
                  <input
                    id="step-path"
                    type="text"
                    value={newStepPath}
                    onChange={(e) => setNewStepPath(e.target.value)}
                    className="font-mono-tabular mt-1 w-full rounded border border-slate-700 bg-[#0b0f17] px-2 py-1.5 text-slate-100"
                  />
                </div>
                {newStepOp === 'create' && (
                  <div className="w-24">
                    <label htmlFor="step-bytes" className="block text-slate-400">Bytes</label>
                    <input
                      id="step-bytes"
                      type="number"
                      min={0}
                      value={newStepBytes}
                      onChange={(e) => setNewStepBytes(Number(e.target.value))}
                      className="font-mono-tabular mt-1 w-full rounded border border-slate-700 bg-[#0b0f17] px-2 py-1.5 text-slate-100"
                    />
                  </div>
                )}
                <button
                  type="submit"
                  className="inline-flex items-center gap-1 rounded bg-slate-800 px-3 py-1.5 text-slate-200 border border-slate-700 hover:border-cyan-500/50"
                >
                  <Plus className="h-3.5 w-3.5" aria-hidden="true" />
                  <span>Add</span>
                </button>
              </form>

              <ul className="mt-3 max-h-56 space-y-1 overflow-y-auto text-xs">
                {workload.map((s, idx) => (
                  <li
                    key={`${s.op}-${s.path}-${idx}`}
                    className="font-mono-tabular flex items-center justify-between rounded border border-slate-800 bg-[#0b0f17] px-2.5 py-1.5"
                  >
                    <span>
                      {idx + 1}. {s.op.toUpperCase()} {s.path}
                      {s.op === 'create' ? ` (${s.size_bytes} B)` : ''}
                    </span>
                    <button
                      type="button"
                      aria-label={`Remove step ${idx + 1}`}
                      onClick={() => setWorkload((prev) => prev.filter((_, i) => i !== idx))}
                      className="text-rose-400 hover:text-rose-300"
                    >
                      <Trash2 className="h-3.5 w-3.5" aria-hidden="true" />
                    </button>
                  </li>
                ))}
              </ul>

              <button
                type="button"
                disabled={runningCompare || workload.length === 0}
                onClick={() => void handleRunComparison()}
                className="mt-4 inline-flex w-full items-center justify-center gap-2 rounded-lg bg-cyan-600 px-4 py-2.5 text-sm font-medium text-white hover:bg-cyan-500 disabled:opacity-50"
              >
                <Activity className="h-4 w-4" aria-hidden="true" />
                <span>{runningCompare ? 'Running 4-Policy Experiment...' : 'Run 4-Policy Comparison'}</span>
              </button>
            </section>

            <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-7">
              <h2 className="text-sm font-semibold text-slate-100">Comparison Results</h2>
              {cmpResult ? (
                <div className="mt-4 space-y-5">
                  <div className="h-60 w-full">
                    <ResponsiveContainer width="100%" height="100%">
                      <BarChart data={compareChartData}>
                        <CartesianGrid strokeDasharray="3 3" stroke="#1e293b" />
                        <XAxis dataKey="policy" stroke="#94a3b8" fontSize={12} />
                        <YAxis stroke="#94a3b8" fontSize={12} />
                        <Tooltip
                          contentStyle={{
                            backgroundColor: '#0f172a',
                            borderColor: '#334155',
                            color: '#f1f5f9',
                            fontSize: '12px',
                          }}
                        />
                        <Legend />
                        <Bar dataKey="extFragPct" name="External Frag %" fill="#f59e0b" />
                        <Bar dataKey="extentsExamined" name="Extents Examined" fill="#22d3ee" />
                        <Bar dataKey="fragmentedFiles" name="Fragmented Files" fill="#f43f5e" />
                      </BarChart>
                    </ResponsiveContainer>
                  </div>

                  <div className="overflow-x-auto">
                    <table className="w-full border-collapse text-left text-xs">
                      <caption className="sr-only">Policy comparison metrics</caption>
                      <thead>
                        <tr className="border-b border-slate-800 text-slate-400">
                          <th scope="col" className="py-2 pr-3 font-medium">Policy</th>
                          <th scope="col" className="px-3 py-2 font-medium">Success / Fail</th>
                          <th scope="col" className="px-3 py-2 font-medium">Ext Frag %</th>
                          <th scope="col" className="px-3 py-2 font-medium">Largest Free</th>
                          <th scope="col" className="px-3 py-2 font-medium">Search Effort</th>
                          <th scope="col" className="py-2 pl-3 font-medium">Elapsed</th>
                        </tr>
                      </thead>
                      <tbody className="divide-y divide-slate-800/70">
                        {cmpResult.policies.map((p) => (
                          <tr key={p.policy}>
                            <td className="font-mono-tabular py-2.5 pr-3 font-semibold text-cyan-200">
                              {p.policy}
                            </td>
                            <td className="font-mono-tabular px-3 py-2.5 text-slate-200">
                              {p.success_count} / {p.failure_count}
                            </td>
                            <td className="font-mono-tabular px-3 py-2.5 text-amber-300">
                              {p.final_report.external_fragmentation_pct.toFixed(2)}%
                            </td>
                            <td className="font-mono-tabular px-3 py-2.5 text-emerald-300">
                              {p.final_report.largest_free_extent} blks
                            </td>
                            <td className="font-mono-tabular px-3 py-2.5 text-slate-300">
                              {p.total_extents_examined} ext / {p.total_blocks_inspected} blks
                            </td>
                            <td className="font-mono-tabular py-2.5 pl-3 text-slate-400">
                              {p.measured_elapsed_ms.toFixed(2)} ms
                            </td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  </div>

                  <div className="space-y-2">
                    <h3 className="text-xs font-semibold uppercase tracking-wider text-slate-400">
                      Final Step Placement Trace
                    </h3>
                    <div className="grid gap-2 sm:grid-cols-2">
                      {cmpResult.policies.map((p) => {
                        const lastCreate = [...p.steps].reverse().find((s) => s.op === 'create')
                        return (
                          <div
                            key={p.policy}
                            className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3 text-xs"
                          >
                            <p className="font-mono-tabular font-semibold text-slate-100">{p.policy}</p>
                            <p className="font-mono-tabular mt-1 text-slate-300">
                              {lastCreate
                                ? `${lastCreate.path} -> [${lastCreate.blocks_allocated.join(', ')}] (${lastCreate.extent_count} ext)`
                                : 'No create steps'}
                            </p>
                          </div>
                        )
                      })}
                    </div>
                  </div>
                </div>
              ) : (
                <p className="mt-4 text-xs text-slate-400">
                  Configure a workload on the left and click Run 4-Policy Comparison to execute isolated C++ experiments.
                </p>
              )}
            </section>
          </div>
        </div>
      )}
    </div>
  )
}
