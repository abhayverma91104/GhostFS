import { useCallback, useEffect, useMemo, useState } from 'react'
import {
  Activity,
  BarChart3,
  Binary,
  Cpu,
  FlaskConical,
  Pause,
  Play,
  RotateCcw,
  SkipBack,
  SkipForward,
} from 'lucide-react'
import {
  compareAllocationAndScheduling,
  compareScheduling,
  generateSchedulingWorkload,
  getFileSchedulingWorkload,
  getMetadataSchedulingWorkload,
  getRecoverySchedulingWorkload,
  simulateScheduling,
} from '../services/api'
import type {
  AllocationSchedulingResult,
  HexNavigationTarget,
  IoRequest,
  MultiAlgorithmComparisonResult,
  ReportNavigationContext,
  SchedulingAlgorithm,
  SimulationResult,
  SweepDirection,
  VolumeStatus,
  WorkloadGenerationResult,
} from '../types/api'

interface DiskSchedulerPageProps {
  activeDisk: VolumeStatus | null
  onOpenInHexViewer: (target: HexNavigationTarget) => void
  onExportReport: (ctx: ReportNavigationContext) => void
  onNotify: (kind: 'success' | 'error', message: string) => void
}

const ALGORITHMS: SchedulingAlgorithm[] = ['FCFS', 'SSTF', 'SCAN', 'C-SCAN', 'LOOK', 'C-LOOK']
const DEFAULT_QUEUE = '98, 183, 37, 122, 14, 124, 65, 67'

export function DiskSchedulerPage({
  activeDisk,
  onOpenInHexViewer,
  onExportReport,
  onNotify,
}: DiskSchedulerPageProps) {
  const [queueText, setQueueText] = useState(DEFAULT_QUEUE)
  const [algorithm, setAlgorithm] = useState<SchedulingAlgorithm>('SSTF')
  const [minCyl, setMinCyl] = useState(0)
  const [maxCyl, setMaxCyl] = useState(199)
  const [initialHead, setInitialHead] = useState(53)
  const [direction, setDirection] = useState<SweepDirection>('right')

  const [workloadSource, setWorkloadSource] = useState<
    'manual' | 'generated' | 'files' | 'recovery' | 'metadata'
  >('manual')
  const [pattern, setPattern] = useState<
    'sequential' | 'random' | 'clustered' | 'mixed' | 'hotspot' | 'fragmented'
  >('clustered')
  const [genCount, setGenCount] = useState(12)
  const [seed, setSeed] = useState(42)
  const [includeMetaBlocks, setIncludeMetaBlocks] = useState(true)
  const [workloadMeta, setWorkloadMeta] = useState<WorkloadGenerationResult | null>(null)
  const [loadedRequests, setLoadedRequests] = useState<IoRequest[] | null>(null)

  const [simResult, setSimResult] = useState<SimulationResult | null>(null)
  const [cmpResult, setCmpResult] = useState<MultiAlgorithmComparisonResult | null>(null)
  const [allocSchedResult, setAllocSchedResult] = useState<AllocationSchedulingResult | null>(null)

  const [playStep, setPlayStep] = useState<number>(0)
  const [isPlaying, setIsPlaying] = useState(false)
  const [playSpeedMs, setPlaySpeedMs] = useState(500)
  const [busy, setBusy] = useState(false)

  const parsedQueue = useMemo(() => {
    if (loadedRequests) return loadedRequests
    return queueText
      .split(/[\s,]+/)
      .map((s) => s.trim())
      .filter((s) => s.length > 0)
      .map((s) => Number(s))
  }, [queueText, loadedRequests])

  const handleRunSimulation = useCallback(
    async (algOverride?: SchedulingAlgorithm) => {
      const targetAlg = algOverride ?? algorithm
      if (parsedQueue.some((r) => typeof r === 'number' && Number.isNaN(r))) {
        onNotify('error', 'Request queue contains non-numeric values')
        return
      }
      setBusy(true)
      try {
        const res = await simulateScheduling({
          algorithm: targetAlg,
          min_cylinder: minCyl,
          max_cylinder: maxCyl,
          initial_head: initialHead,
          initial_direction: direction,
          requests: parsedQueue,
        })
        setSimResult(res)
        setPlayStep(res.steps.length)
        setIsPlaying(false)
      } catch (err) {
        onNotify('error', err instanceof Error ? err.message : 'Simulation failed')
      } finally {
        setBusy(false)
      }
    },
    [algorithm, direction, initialHead, maxCyl, minCyl, onNotify, parsedQueue],
  )

  const handleCompareAll = useCallback(async () => {
    if (parsedQueue.some((r) => typeof r === 'number' && Number.isNaN(r))) {
      onNotify('error', 'Request queue contains non-numeric values')
      return
    }
    setBusy(true)
    try {
      const cmp = await compareScheduling({
        min_cylinder: minCyl,
        max_cylinder: maxCyl,
        initial_head: initialHead,
        initial_direction: direction,
        requests: parsedQueue,
      })
      setCmpResult(cmp)
      const activeRow = cmp.algorithms.find((a) => a.result.algorithm === algorithm)
      if (activeRow) {
        setSimResult(activeRow.result)
        setPlayStep(activeRow.result.steps.length)
      }
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Comparison failed')
    } finally {
      setBusy(false)
    }
  }, [algorithm, direction, initialHead, maxCyl, minCyl, onNotify, parsedQueue])

  useEffect(() => {
    void handleCompareAll()
    // Initial textbook load
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  useEffect(() => {
    if (!isPlaying || !simResult) return
    if (playStep >= simResult.steps.length) {
      setIsPlaying(false)
      return
    }
    const timer = window.setTimeout(() => {
      setPlayStep((s) => Math.min(simResult.steps.length, s + 1))
    }, playSpeedMs)
    return () => window.clearTimeout(timer)
  }, [isPlaying, playSpeedMs, playStep, simResult])

  const handleLoadWorkload = async () => {
    setBusy(true)
    try {
      let w: WorkloadGenerationResult
      if (workloadSource === 'generated') {
        w = await generateSchedulingWorkload({
          pattern,
          count: genCount,
          min_cylinder: minCyl,
          max_cylinder: maxCyl,
          seed,
        })
      } else if (workloadSource === 'files') {
        if (!activeDisk) throw new Error('Mount a GFS1 disk in Disk Manager first')
        w = await getFileSchedulingWorkload(activeDisk.disk_name, {
          file_paths: [],
          include_metadata_blocks: includeMetaBlocks,
          blocks_per_cylinder: 1,
        })
      } else if (workloadSource === 'recovery') {
        if (!activeDisk) throw new Error('Mount a GFS1 disk in Disk Manager first')
        w = await getRecoverySchedulingWorkload(activeDisk.disk_name, {
          candidate_id: '',
          enable_txt_heuristic: true,
          blocks_per_cylinder: 1,
        })
      } else if (workloadSource === 'metadata') {
        if (!activeDisk) throw new Error('Mount a GFS1 disk in Disk Manager first')
        w = await getMetadataSchedulingWorkload(activeDisk.disk_name, 1)
      } else {
        setLoadedRequests(null)
        setWorkloadMeta(null)
        setBusy(false)
        return
      }
      setWorkloadMeta(w)
      setLoadedRequests(w.requests)
      setMinCyl(w.min_cylinder)
      setMaxCyl(w.max_cylinder)
      setInitialHead(Math.min(initialHead, w.max_cylinder))
      setQueueText(w.requests.map((r) => r.cylinder).join(', '))
      onNotify('success', `Loaded ${w.requests.length} I/O requests (${w.source_type})`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to load workload')
    } finally {
      setBusy(false)
    }
  }

  const handleRunAllocSchedCompare = async () => {
    setBusy(true)
    try {
      const res = await compareAllocationAndScheduling({
        block_size: 4096,
        total_blocks: 128,
        initial_head: 0,
        initial_direction: direction,
        blocks_per_cylinder: 1,
        workload: [
          { op: 'create', path: '/seg1.bin', size_bytes: 12288 },
          { op: 'create', path: '/seg2.bin', size_bytes: 8192 },
          { op: 'create', path: '/seg3.bin', size_bytes: 16384 },
          { op: 'delete', path: '/seg2.bin', size_bytes: 0 },
          { op: 'create', path: '/frag.bin', size_bytes: 16384 },
        ],
      })
      setAllocSchedResult(res)
      onNotify('success', 'Completed 4-policy allocation + 6-algorithm scheduling experiment')
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Combined experiment failed')
    } finally {
      setBusy(false)
    }
  }

  const handleResetTextbook = () => {
    setWorkloadSource('manual')
    setLoadedRequests(null)
    setWorkloadMeta(null)
    setQueueText(DEFAULT_QUEUE)
    setMinCyl(0)
    setMaxCyl(199)
    setInitialHead(53)
    setDirection('right')
    setAlgorithm('SSTF')
  }

  const visibleSteps = useMemo(() => {
    if (!simResult) return []
    return simResult.steps.slice(0, playStep)
  }, [playStep, simResult])

  const minTotalInCompare = useMemo(() => {
    if (!cmpResult || cmpResult.algorithms.length === 0) return 0
    return Math.min(...cmpResult.algorithms.map((a) => a.result.total_head_movement))
  }, [cmpResult])

  const maxTotalInCompare = useMemo(() => {
    if (!cmpResult || cmpResult.algorithms.length === 0) return 1
    return Math.max(1, ...cmpResult.algorithms.map((a) => a.result.total_head_movement))
  }, [cmpResult])

  // SVG Head Movement Geometry
  const svgW = 820
  const svgH = Math.max(260, (simResult?.head_path.length ?? 2) * 34 + 50)
  const padX = 48
  const padY = 36
  const plotW = svgW - padX * 2
  const spanCyl = Math.max(1, (simResult?.max_cylinder ?? maxCyl) - (simResult?.min_cylinder ?? minCyl))
  const toX = (cyl: number) =>
    padX + ((cyl - (simResult?.min_cylinder ?? minCyl)) / spanCyl) * plotW
  const toY = (idx: number) => padY + idx * 32

  const axisTicks = useMemo(() => {
    const lo = simResult?.min_cylinder ?? minCyl
    const hi = simResult?.max_cylinder ?? maxCyl
    const mid1 = Math.round(lo + (hi - lo) * 0.25)
    const mid2 = Math.round(lo + (hi - lo) * 0.5)
    const mid3 = Math.round(lo + (hi - lo) * 0.75)
    return Array.from(new Set([lo, mid1, mid2, mid3, hi]))
  }, [maxCyl, minCyl, simResult])

  return (
    <div className="space-y-6">
      {/* Simulation Boundary Banner */}
      <section className="rounded-xl border border-cyan-500/30 bg-cyan-500/10 px-4 py-3 text-xs text-cyan-100">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="flex items-center gap-2">
            <Cpu className="h-4 w-4 shrink-0 text-cyan-300" aria-hidden="true" />
            <span>
              <strong>Educational Disk Scheduling Simulator:</strong> Models mechanical cylinder seek
              sequences in C++17. Does not control physical disk heads, reorder host OS I/O, or
              represent SSD flash translation behavior.
            </span>
          </div>
          {activeDisk && (
            <span className="font-mono-tabular rounded border border-slate-700 bg-[#0b0f17] px-2 py-0.5 text-[11px] text-slate-300">
              Active GFS1: {activeDisk.disk_name} ({activeDisk.total_blocks} blks)
            </span>
          )}
        </div>
      </section>

      {/* A. Configuration & B. Interactive Head Movement */}
      <div className="grid gap-6 xl:grid-cols-12">
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-5 space-y-4">
          <div className="flex items-center justify-between">
            <h2 className="text-sm font-semibold text-slate-100">Disk Scheduling Laboratory</h2>
            <button
              type="button"
              onClick={handleResetTextbook}
              className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2.5 py-1 text-xs text-slate-200 hover:border-cyan-500/50"
            >
              <RotateCcw className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Reset Example</span>
            </button>
          </div>

          <div className="space-y-3 text-xs">
            <div>
              <label htmlFor="sched-source" className="block text-slate-400">
                Workload Source
              </label>
              <div className="mt-1 flex gap-2">
                <select
                  id="sched-source"
                  value={workloadSource}
                  onChange={(e) =>
                    setWorkloadSource(
                      e.target.value as 'manual' | 'generated' | 'files' | 'recovery' | 'metadata',
                    )
                  }
                  className="flex-1 rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
                >
                  <option value="manual">A. Manual Request Queue</option>
                  <option value="generated">E. Reproducible Generated Workload</option>
                  <option value="files">B. GFS1 File-Based Block Mapping</option>
                  <option value="recovery">C. Recovery Candidate Surviving Blocks</option>
                  <option value="metadata">D. GFS1 Metadata Region Reads</option>
                </select>
                {workloadSource !== 'manual' && (
                  <button
                    type="button"
                    disabled={busy}
                    onClick={() => void handleLoadWorkload()}
                    className="rounded-lg bg-slate-700 px-3 py-2 font-medium text-white hover:bg-slate-600 disabled:opacity-50"
                  >
                    Load
                  </button>
                )}
              </div>
            </div>

            {workloadSource === 'generated' && (
              <div className="grid grid-cols-3 gap-2 rounded-lg border border-slate-800 bg-[#0b0f17] p-2.5">
                <div>
                  <label htmlFor="sched-pattern" className="block text-[11px] text-slate-400">
                    Pattern
                  </label>
                  <select
                    id="sched-pattern"
                    value={pattern}
                    onChange={(e) =>
                      setPattern(
                        e.target.value as
                          | 'sequential'
                          | 'random'
                          | 'clustered'
                          | 'mixed'
                          | 'hotspot'
                          | 'fragmented',
                      )
                    }
                    className="mt-1 w-full rounded border border-slate-700 bg-[#111827] px-2 py-1 text-slate-100"
                  >
                    <option value="sequential">Sequential</option>
                    <option value="random">Random</option>
                    <option value="clustered">Clustered</option>
                    <option value="mixed">Mixed</option>
                    <option value="hotspot">Hotspot</option>
                    <option value="fragmented">Fragmented</option>
                  </select>
                </div>
                <div>
                  <label htmlFor="sched-count" className="block text-[11px] text-slate-400">
                    Count
                  </label>
                  <input
                    id="sched-count"
                    type="number"
                    min={1}
                    max={100}
                    value={genCount}
                    onChange={(e) => setGenCount(Number(e.target.value) || 8)}
                    className="font-mono-tabular mt-1 w-full rounded border border-slate-700 bg-[#111827] px-2 py-1 text-slate-100"
                  />
                </div>
                <div>
                  <label htmlFor="sched-seed" className="block text-[11px] text-slate-400">
                    Seed
                  </label>
                  <input
                    id="sched-seed"
                    type="number"
                    value={seed}
                    onChange={(e) => setSeed(Number(e.target.value) || 0)}
                    className="font-mono-tabular mt-1 w-full rounded border border-slate-700 bg-[#111827] px-2 py-1 text-slate-100"
                  />
                </div>
              </div>
            )}

            {workloadSource === 'files' && (
              <label className="flex items-center gap-2 text-slate-300">
                <input
                  type="checkbox"
                  checked={includeMetaBlocks}
                  onChange={(e) => setIncludeMetaBlocks(e.target.checked)}
                />
                <span>Include inode &amp; indirect pointer block accesses</span>
              </label>
            )}

            <div>
              <label htmlFor="sched-queue" className="block text-slate-400">
                Request queue (cylinders {minCyl}–{maxCyl})
              </label>
              <input
                id="sched-queue"
                type="text"
                value={queueText}
                onChange={(e) => {
                  setLoadedRequests(null)
                  setQueueText(e.target.value)
                }}
                className="font-mono-tabular mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
              />
            </div>

            <div>
              <span className="block text-slate-400">Scheduling algorithm</span>
              <div className="mt-1.5 flex flex-wrap gap-2">
                {ALGORITHMS.map((alg) => (
                  <button
                    key={alg}
                    type="button"
                    onClick={() => {
                      setAlgorithm(alg)
                      void handleRunSimulation(alg)
                    }}
                    className={`rounded-full px-3 py-1 text-xs font-medium border ${
                      algorithm === alg
                        ? 'border-cyan-400 bg-cyan-500/20 text-cyan-200'
                        : 'border-slate-700 bg-[#0b0f17] text-slate-300 hover:border-slate-500'
                    }`}
                  >
                    {alg}
                  </button>
                ))}
              </div>
            </div>

            <div>
              <div className="flex items-center justify-between text-slate-400">
                <label htmlFor="sched-head">Initial head position</label>
                <span className="font-mono-tabular font-semibold text-slate-100">{initialHead}</span>
              </div>
              <input
                id="sched-head"
                type="range"
                min={minCyl}
                max={maxCyl}
                value={initialHead}
                onChange={(e) => setInitialHead(Number(e.target.value))}
                className="mt-1.5 w-full accent-cyan-400"
              />
              <div className="mt-1 grid grid-cols-3 gap-2">
                <div>
                  <label htmlFor="sched-min" className="block text-[11px] text-slate-500">
                    Min Cyl
                  </label>
                  <input
                    id="sched-min"
                    type="number"
                    min={0}
                    value={minCyl}
                    onChange={(e) => setMinCyl(Number(e.target.value) || 0)}
                    className="font-mono-tabular w-full rounded border border-slate-700 bg-[#0b0f17] px-2 py-1 text-slate-200"
                  />
                </div>
                <div>
                  <label htmlFor="sched-head-num" className="block text-[11px] text-slate-500">
                    Head
                  </label>
                  <input
                    id="sched-head-num"
                    type="number"
                    min={minCyl}
                    max={maxCyl}
                    value={initialHead}
                    onChange={(e) => setInitialHead(Number(e.target.value) || 0)}
                    className="font-mono-tabular w-full rounded border border-slate-700 bg-[#0b0f17] px-2 py-1 text-slate-200"
                  />
                </div>
                <div>
                  <label htmlFor="sched-max" className="block text-[11px] text-slate-500">
                    Max Cyl
                  </label>
                  <input
                    id="sched-max"
                    type="number"
                    min={1}
                    value={maxCyl}
                    onChange={(e) => setMaxCyl(Number(e.target.value) || 199)}
                    className="font-mono-tabular w-full rounded border border-slate-700 bg-[#0b0f17] px-2 py-1 text-slate-200"
                  />
                </div>
              </div>
            </div>

            <div>
              <span className="block text-slate-400">Initial sweep direction</span>
              <div className="mt-1.5 inline-flex rounded-lg border border-slate-700 bg-[#0b0f17] p-0.5">
                {(['left', 'right'] as const).map((d) => (
                  <button
                    key={d}
                    type="button"
                    onClick={() => setDirection(d)}
                    className={`rounded-md px-3 py-1 capitalize ${
                      direction === d ? 'bg-cyan-600 text-white' : 'text-slate-300'
                    }`}
                  >
                    {d}
                  </button>
                ))}
              </div>
            </div>

            <div className="flex flex-wrap gap-2 pt-1">
              <button
                type="button"
                disabled={busy}
                onClick={() => void handleRunSimulation()}
                className="flex-1 rounded-lg bg-cyan-600 px-3.5 py-2 font-medium text-white hover:bg-cyan-500 disabled:opacity-50"
              >
                Run Simulation
              </button>
              <button
                type="button"
                disabled={busy}
                onClick={() => void handleCompareAll()}
                className="flex-1 rounded-lg border border-cyan-500/40 bg-cyan-500/15 px-3.5 py-2 font-medium text-cyan-200 hover:bg-cyan-500/25 disabled:opacity-50"
              >
                Compare All Algorithms
              </button>
              <button
                type="button"
                onClick={() =>
                  onExportReport({
                    reportType: 'disk_scheduling',
                    diskName: activeDisk?.disk_name,
                    title: 'Disk Scheduling Simulation Report',
                    schedulingPayload: {
                      min_cylinder: minCyl,
                      max_cylinder: maxCyl,
                      initial_head: initialHead,
                      initial_direction: direction,
                      requests: parsedQueue,
                    },
                  })
                }
                className="rounded-lg border border-emerald-500/40 bg-emerald-500/10 px-3 py-2 text-xs font-medium text-emerald-300 hover:bg-emerald-500/20"
              >
                Export Report
              </button>
            </div>
          </div>
        </section>

        {/* B & C & E. Interactive Head Movement + Playback + Metrics */}
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-7 space-y-4">
          {simResult ? (
            <>
              <div className="grid grid-cols-2 gap-4 sm:grid-cols-4 border-b border-slate-800 pb-4">
                <div>
                  <p className="text-xs text-slate-400">Total head movement</p>
                  <p className="font-mono-tabular mt-1 text-2xl font-bold text-emerald-400">
                    {simResult.total_head_movement}
                  </p>
                  <p className="text-[11px] text-slate-500">Cylinder units</p>
                </div>
                <div>
                  <p className="text-xs text-slate-400">Average movement / req</p>
                  <p className="font-mono-tabular mt-1 text-2xl font-bold text-slate-100">
                    {simResult.average_movement.toFixed(2)}
                  </p>
                  <p className="text-[11px] text-slate-500">
                    {simResult.serviced_count} serviced
                  </p>
                </div>
                <div>
                  <p className="text-xs text-slate-400">Dir Changes / Jumps</p>
                  <p className="font-mono-tabular mt-1 text-xl font-semibold text-cyan-300">
                    {simResult.direction_changes} / {simResult.circular_repositioning_distance}
                  </p>
                  <p className="text-[11px] text-slate-500">
                    Max step: {simResult.max_single_step_movement}
                  </p>
                </div>
                <div>
                  <p className="text-xs text-slate-400">C++ Exec Time</p>
                  <p className="font-mono-tabular mt-1 text-xl font-semibold text-slate-200">
                    {simResult.median_exec_duration_us.toFixed(1)} µs
                  </p>
                  <p className="text-[11px] text-slate-500">{simResult.algorithm}</p>
                </div>
              </div>

              {/* Playback Controls */}
              <div className="flex flex-wrap items-center justify-between gap-2 rounded-lg border border-slate-800 bg-[#0b0f17] px-3 py-2 text-xs">
                <div className="flex items-center gap-1.5">
                  <button
                    type="button"
                    aria-label="Restart playback"
                    onClick={() => {
                      setIsPlaying(false)
                      setPlayStep(0)
                    }}
                    className="rounded border border-slate-700 bg-slate-800 p-1.5 text-slate-200 hover:border-cyan-500/50"
                  >
                    <RotateCcw className="h-3.5 w-3.5" aria-hidden="true" />
                  </button>
                  <button
                    type="button"
                    aria-label="Previous step"
                    onClick={() => {
                      setIsPlaying(false)
                      setPlayStep((s) => Math.max(0, s - 1))
                    }}
                    className="rounded border border-slate-700 bg-slate-800 p-1.5 text-slate-200 hover:border-cyan-500/50"
                  >
                    <SkipBack className="h-3.5 w-3.5" aria-hidden="true" />
                  </button>
                  <button
                    type="button"
                    aria-label={isPlaying ? 'Pause playback' : 'Play simulation'}
                    onClick={() => {
                      if (!isPlaying && playStep >= simResult.steps.length) {
                        setPlayStep(0)
                      }
                      setIsPlaying((p) => !p)
                    }}
                    className="inline-flex items-center gap-1 rounded bg-cyan-600 px-2.5 py-1.5 font-medium text-white hover:bg-cyan-500"
                  >
                    {isPlaying ? (
                      <Pause className="h-3.5 w-3.5" aria-hidden="true" />
                    ) : (
                      <Play className="h-3.5 w-3.5" aria-hidden="true" />
                    )}
                    <span>{isPlaying ? 'Pause' : 'Play'}</span>
                  </button>
                  <button
                    type="button"
                    aria-label="Next step"
                    onClick={() => {
                      setIsPlaying(false)
                      setPlayStep((s) => Math.min(simResult.steps.length, s + 1))
                    }}
                    className="rounded border border-slate-700 bg-slate-800 p-1.5 text-slate-200 hover:border-cyan-500/50"
                  >
                    <SkipForward className="h-3.5 w-3.5" aria-hidden="true" />
                  </button>
                </div>
                <div className="flex items-center gap-3 font-mono-tabular text-slate-300">
                  <span>
                    Step {playStep}/{simResult.steps.length}
                  </span>
                  <span>
                    Head:{' '}
                    {playStep === 0
                      ? simResult.initial_head
                      : visibleSteps[visibleSteps.length - 1]?.to_cylinder}
                  </span>
                  <span>
                    Accum:{' '}
                    {playStep === 0
                      ? 0
                      : visibleSteps[visibleSteps.length - 1]?.cumulative_movement}
                  </span>
                  <select
                    aria-label="Playback speed"
                    value={playSpeedMs}
                    onChange={(e) => setPlaySpeedMs(Number(e.target.value))}
                    className="rounded border border-slate-700 bg-[#111827] px-1.5 py-0.5 text-[11px]"
                  >
                    <option value={900}>0.5x</option>
                    <option value={500}>1x</option>
                    <option value={220}>2x</option>
                  </select>
                </div>
              </div>

              {/* SVG Head Movement Plot */}
              <div className="overflow-x-auto rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
                <svg
                  viewBox={`0 0 ${svgW} ${svgH}`}
                  className="w-full min-w-[560px]"
                  role="img"
                  aria-label="Disk head movement trajectory chart"
                >
                  {axisTicks.map((tick) => {
                    const x = toX(tick)
                    return (
                      <g key={tick}>
                        <text
                          x={x}
                          y={18}
                          textAnchor="middle"
                          fill="#94a3b8"
                          fontSize="12"
                          fontFamily="monospace"
                        >
                          {tick}
                        </text>
                        <line
                          x1={x}
                          y1={28}
                          x2={x}
                          y2={svgH - 12}
                          stroke="#1e293b"
                          strokeDasharray="4 4"
                        />
                      </g>
                    )
                  })}

                  {/* Path Segments */}
                  {visibleSteps.map((st, idx) => {
                    const x1 = toX(st.from_cylinder)
                    const y1 = toY(idx)
                    const x2 = toX(st.to_cylinder)
                    const y2 = toY(idx + 1)
                    const isJump = st.step_type === 'circular_jump'
                    const isBoundary = st.step_type === 'boundary_visit'
                    return (
                      <line
                        key={st.step_index}
                        x1={x1}
                        y1={y1}
                        x2={x2}
                        y2={y2}
                        stroke={isJump ? '#f59e0b' : isBoundary ? '#a78bfa' : '#06b6d4'}
                        strokeWidth={2.5}
                        strokeDasharray={isJump ? '6 4' : undefined}
                      />
                    )
                  })}

                  {/* Initial Head Marker */}
                  <circle cx={toX(simResult.initial_head)} cy={toY(0)} r={6.5} fill="#22c55e" />
                  <text
                    x={toX(simResult.initial_head) + 10}
                    y={toY(0) + 4}
                    fill="#f8fafc"
                    fontSize="12"
                    fontFamily="monospace"
                  >
                    {simResult.initial_head}
                  </text>

                  {/* Step Markers */}
                  {visibleSteps.map((st, idx) => {
                    const cx = toX(st.to_cylinder)
                    const cy = toY(idx + 1)
                    const fill =
                      st.step_type === 'circular_jump'
                        ? '#f59e0b'
                        : st.step_type === 'boundary_visit'
                          ? '#a78bfa'
                          : '#06b6d4'
                    return (
                      <g key={`pt_${st.step_index}`}>
                        <circle cx={cx} cy={cy} r={5.5} fill={fill} />
                        <text
                          x={cx > svgW - 80 ? cx - 34 : cx + 10}
                          y={cy + 4}
                          fill="#f8fafc"
                          fontSize="12"
                          fontFamily="monospace"
                        >
                          {st.to_cylinder}
                          {!st.is_service ? ` (${st.step_type === 'circular_jump' ? 'wrap' : 'end'})` : ''}
                        </text>
                      </g>
                    )
                  })}
                </svg>
              </div>

              <div className="rounded-lg border border-slate-800 bg-[#0b0f17] px-3 py-2 text-xs">
                <p className="text-slate-400">Head movement sequence</p>
                <p className="font-mono-tabular mt-1 text-slate-100">
                  {simResult.head_path.join(' → ')}
                </p>
                <p className="mt-1 text-[11px] text-slate-500">
                  Circular jumps count toward total movement. The simulation stops once all requests
                  are serviced.
                </p>
              </div>
            </>
          ) : (
            <p className="text-xs text-slate-400">Run a simulation to view head trajectory.</p>
          )}
        </section>
      </div>

      {/* F. Six-Algorithm Comparison Dashboard */}
      {cmpResult && (
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 space-y-4">
          <div className="flex flex-wrap items-center justify-between gap-2">
            <div>
              <h2 className="flex items-center gap-2 text-sm font-semibold text-slate-100">
                <BarChart3 className="h-4 w-4 text-cyan-300" aria-hidden="true" />
                <span>Algorithm Comparison (Total Head Movement)</span>
              </h2>
              <p className="text-xs text-slate-400">
                Lower is better for this workload. Ranking is specific to the selected queue and
                geometry; no single algorithm is universally optimal.
              </p>
            </div>
          </div>

          <div className="space-y-2.5">
            {cmpResult.algorithms.map((row) => {
              const total = row.result.total_head_movement
              const pct = Math.max(4, Math.round((total / maxTotalInCompare) * 100))
              const isBest = total === minTotalInCompare
              return (
                <div key={row.result.algorithm} className="space-y-1">
                  <div className="flex items-center justify-between text-xs">
                    <span className="font-medium text-slate-200">
                      {row.result.algorithm}
                      <span className="ml-2 text-[11px] text-slate-400">
                        (avg {row.result.average_movement.toFixed(2)} ·{' '}
                        {row.reduction_vs_fcfs_pct >= 0 ? '-' : '+'}
                        {Math.abs(row.reduction_vs_fcfs_pct).toFixed(1)}% vs FCFS)
                      </span>
                    </span>
                    <span className="font-mono-tabular font-semibold text-slate-100">{total}</span>
                  </div>
                  <div className="h-3.5 w-full rounded bg-[#0b0f17] overflow-hidden">
                    <div
                      className={`h-full rounded transition-all ${
                        isBest ? 'bg-emerald-500' : 'bg-slate-500'
                      }`}
                      style={{ width: `${pct}%` }}
                    />
                  </div>
                </div>
              )
            })}
          </div>
        </section>
      )}

      {/* D. Request Queue & G. GFS1 Workload Inspector */}
      <div className="grid gap-6 xl:grid-cols-12">
        {simResult && (
          <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-7">
            <h2 className="text-sm font-semibold text-slate-100">
              Execution Steps &amp; Request Queue ({simResult.steps.length} steps)
            </h2>
            <div className="mt-3 max-h-64 overflow-y-auto">
              <table className="w-full text-left text-xs">
                <thead className="border-b border-slate-800 text-slate-400">
                  <tr>
                    <th className="py-2 pr-2">Step</th>
                    <th className="px-2 py-2">Type</th>
                    <th className="px-2 py-2">From → To</th>
                    <th className="px-2 py-2">Move</th>
                    <th className="px-2 py-2">Cumulative</th>
                    <th className="pl-2 py-2">LBA</th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-slate-800/70 font-mono-tabular">
                  {simResult.steps.map((st) => (
                    <tr key={st.step_index}>
                      <td className="py-1.5 pr-2 text-slate-300">#{st.step_index}</td>
                      <td className="px-2 py-1.5 text-slate-200">{st.step_type}</td>
                      <td className="px-2 py-1.5 text-cyan-300">
                        {st.from_cylinder} → {st.to_cylinder}
                      </td>
                      <td className="px-2 py-1.5 text-slate-200">{st.movement}</td>
                      <td className="px-2 py-1.5 text-slate-300">{st.cumulative_movement}</td>
                      <td className="pl-2 py-1.5 text-slate-400">
                        {st.logical_block > 0 ? (
                          <button
                            type="button"
                            onClick={() =>
                              onOpenInHexViewer({ sourceType: 'block', blockId: st.logical_block })
                            }
                            className="inline-flex items-center gap-1 text-cyan-300 hover:underline"
                          >
                            <Binary className="h-3 w-3" aria-hidden="true" />
                            <span>Blk {st.logical_block}</span>
                          </button>
                        ) : (
                          '—'
                        )}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </section>
        )}

        {/* Section 7: Allocation Policy & Scheduling Integration */}
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-5 space-y-3">
          <div className="flex items-center justify-between gap-2">
            <div>
              <h2 className="flex items-center gap-1.5 text-sm font-semibold text-slate-100">
                <FlaskConical className="h-4 w-4 text-cyan-300" aria-hidden="true" />
                <span>Allocation + Scheduling Integration</span>
              </h2>
              <p className="text-[11px] text-slate-400">
                Runs identical workload across all 4 GFS1 allocation policies, then schedules reads.
              </p>
            </div>
            <button
              type="button"
              disabled={busy}
              onClick={() => void handleRunAllocSchedCompare()}
              className="rounded-lg bg-cyan-600 px-3 py-1.5 text-xs font-medium text-white hover:bg-cyan-500 disabled:opacity-50"
            >
              Run Combined
            </button>
          </div>

          {workloadMeta && (
            <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-2.5 text-[11px] text-slate-300">
              <p className="font-semibold text-cyan-300">
                GFS1 Workload: {workloadMeta.disk_name || workloadMeta.source_type}
              </p>
              {workloadMeta.notes.map((n, i) => (
                <p key={i} className="mt-0.5 text-slate-400">
                  {n}
                </p>
              ))}
            </div>
          )}

          {allocSchedResult ? (
            <div className="space-y-2 text-xs">
              {allocSchedResult.policies.map((p) => {
                const bestAlg = [...p.scheduling.algorithms].sort(
                  (a, b) => a.result.total_head_movement - b.result.total_head_movement,
                )[0]
                return (
                  <div
                    key={p.policy}
                    className="rounded-lg border border-slate-800 bg-[#0b0f17] p-2.5"
                  >
                    <div className="flex items-center justify-between">
                      <span className="font-mono-tabular font-semibold text-slate-100">
                        {p.policy}
                      </span>
                      <span className="font-mono-tabular text-cyan-300">
                        Ext Frag: {p.external_fragmentation_pct.toFixed(1)}% · Extents:{' '}
                        {p.total_file_extents}
                      </span>
                    </div>
                    <p className="font-mono-tabular mt-1 text-[11px] text-slate-400">
                      FCFS: {p.scheduling.fcfs_total_movement} · Best ({bestAlg?.result.algorithm}):{' '}
                      {bestAlg?.result.total_head_movement}
                    </p>
                  </div>
                )
              })}
            </div>
          ) : (
            <p className="flex items-center gap-1.5 text-xs text-slate-400">
              <Activity className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Click &ldquo;Run Combined&rdquo; to compare allocation policies vs I/O seek distance.</span>
            </p>
          )}
        </section>
      </div>
    </div>
  )
}
