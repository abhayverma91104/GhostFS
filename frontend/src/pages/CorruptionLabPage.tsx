import { useCallback, useEffect, useMemo, useState } from 'react'
import {
  AlertTriangle,
  Binary,
  CheckCircle2,
  Download,
  FileJson,
  GitCompare,
  Play,
  Plus,
  RefreshCw,
  ShieldAlert,
  ShieldCheck,
  Trash2,
  Wrench,
} from 'lucide-react'
import { ConfirmDialog } from '../components/ConfirmDialog'
import {
  applyRepairPlan,
  checkCorruptionExperiment,
  compareCorruptionExperiment,
  createCorruptionExperiment,
  deleteCorruptionExperiment,
  generateRepairPlan,
  getExperimentDownloadUrl,
  getExperimentManifestUrl,
  injectCorruption,
  listCorruptionExperiments,
} from '../services/api'
import type {
  DiskListItem,
  ExperimentComparisonResult,
  FsckIssue,
  FsckResult,
  HexNavigationTarget,
  RepairApplyResult,
  RepairPlan,
  ReportNavigationContext,
  SandboxExperiment,
  VolumeStatus,
} from '../types/api'

interface CorruptionLabPageProps {
  disks: DiskListItem[]
  activeDisk: VolumeStatus | null
  onOpenInHexViewer: (target: HexNavigationTarget) => void
  onExportReport: (ctx: ReportNavigationContext) => void
  onNotify: (kind: 'success' | 'error', message: string) => void
}

const SCENARIOS_BY_CATEGORY: Record<string, Array<{ value: string; label: string }>> = {
  primary_superblock: [
    { value: 'corrupt_magic', label: 'Corrupt Primary Magic (GFS1 -> BAD1)' },
    { value: 'corrupt_geometry', label: 'Corrupt Primary Geometry (total_blocks)' },
    { value: 'checksum_mismatch', label: 'Primary Superblock CRC32 Mismatch' },
  ],
  backup_superblock: [
    { value: 'corrupt_magic', label: 'Corrupt Backup Magic' },
    { value: 'inconsistent_replica', label: 'Inconsistent Backup Replica Geometry' },
    { value: 'checksum_mismatch', label: 'Backup Superblock CRC32 Mismatch' },
  ],
  dual_superblock: [
    { value: 'corrupt_magic', label: 'Corrupt Both Superblock Magics' },
    { value: 'corrupt_geometry', label: 'Corrupt Both Superblock Geometries' },
  ],
  block_bitmap: [
    { value: 'mark_allocated_as_free', label: 'Mark Allocated Block as Free' },
    { value: 'mark_free_as_allocated', label: 'Mark Free Block as Allocated (Leak)' },
    { value: 'both', label: 'Both Referenced-Free & Unreferenced-Allocated' },
  ],
  inode_bitmap: [
    { value: 'mark_active_as_free', label: 'Mark Active Inode as Free' },
    { value: 'mark_unused_as_active', label: 'Mark Unused Inode as Active' },
  ],
  inode: [
    { value: 'corrupt_checksum', label: 'Corrupt Inode CRC32 Checksum' },
    { value: 'out_of_range_pointer', label: 'Out-of-Range Direct Block Pointer' },
    { value: 'duplicate_block_ownership', label: 'Duplicate Block Ownership (Cross-Link)' },
    { value: 'inconsistent_size', label: 'Inconsistent Logical File Size' },
    { value: 'corrupt_indirect', label: 'Corrupt Single-Indirect Pointer' },
  ],
  directory: [
    { value: 'dangling_inode', label: 'Dangling Directory Entry' },
    { value: 'duplicate_filename', label: 'Duplicate Active Filename' },
    { value: 'directory_cycle', label: 'Directory Cycle (loop -> root)' },
    { value: 'inconsistent_parent', label: 'Inconsistent Parent Inode' },
  ],
  journal: [
    { value: 'pending_uncommitted', label: 'Pending Uncommitted Transaction + Dirty State' },
    { value: 'invalid_checksum', label: 'Invalid Journal Entry CRC32' },
  ],
  payload: [{ value: 'bit_flip', label: 'Deterministic Payload Bit Flips' }],
  advanced: [
    { value: 'orphan_inode', label: 'Orphaned Active Inode (Unlink DirEntry)' },
    { value: 'multi_corruption', label: 'Simultaneous Primary SB + Bitmap Leak' },
    { value: 'truncate_image', label: 'Truncate Sandbox Disk Image' },
  ],
}

const SEVERITY_BADGE: Record<string, string> = {
  critical: 'border-rose-500/40 bg-rose-500/15 text-rose-300',
  error: 'border-amber-500/40 bg-amber-500/15 text-amber-300',
  warning: 'border-yellow-500/40 bg-yellow-500/15 text-yellow-300',
  information: 'border-cyan-500/40 bg-cyan-500/15 text-cyan-300',
}

export function CorruptionLabPage({
  disks,
  activeDisk,
  onOpenInHexViewer,
  onExportReport,
  onNotify,
}: CorruptionLabPageProps) {
  const [experiments, setExperiments] = useState<SandboxExperiment[]>([])
  const [selectedExp, setSelectedExp] = useState<SandboxExperiment | null>(null)
  const [sourceDisk, setSourceDisk] = useState<string>(activeDisk?.disk_name ?? '')
  const [sandboxName, setSandboxName] = useState('')

  const [category, setCategory] = useState<string>('primary_superblock')
  const [scenario, setScenario] = useState<string>('corrupt_magic')
  const [targetBlock, setTargetBlock] = useState<string>('0')
  const [targetInode, setTargetInode] = useState<string>('0')
  const [severity, setSeverity] = useState<number>(1)
  const [seed, setSeed] = useState<number>(42)

  const [checkMode, setCheckMode] = useState<'fast' | 'full'>('full')
  const [fsckResult, setFsckResult] = useState<FsckResult | null>(null)
  const [severityFilter, setSeverityFilter] = useState<string>('')
  const [inspectedIssue, setInspectedIssue] = useState<FsckIssue | null>(null)

  const [repairPlan, setRepairPlan] = useState<RepairPlan | null>(null)
  const [selectedActionIds, setSelectedActionIds] = useState<string[]>([])
  const [repairResult, setRepairResult] = useState<RepairApplyResult | null>(null)
  const [comparison, setComparison] = useState<ExperimentComparisonResult | null>(null)
  const [compareMode, setCompareMode] = useState<
    'original_vs_corrupted' | 'corrupted_vs_repaired' | 'original_vs_repaired'
  >('original_vs_corrupted')

  const [busy, setBusy] = useState(false)
  const [confirmAction, setConfirmAction] = useState<
    'inject' | 'repair' | 'delete' | null
  >(null)

  const refreshExperiments = useCallback(async () => {
    try {
      const res = await listCorruptionExperiments()
      setExperiments(res.experiments)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to load experiments')
    }
  }, [onNotify])

  useEffect(() => {
    void refreshExperiments()
  }, [refreshExperiments])

  useEffect(() => {
    if (!sourceDisk && (activeDisk?.disk_name || disks.length > 0)) {
      setSourceDisk(activeDisk?.disk_name ?? disks[0].disk_name)
    }
  }, [activeDisk, disks, sourceDisk])

  useEffect(() => {
    const opts = SCENARIOS_BY_CATEGORY[category] ?? []
    if (opts.length > 0 && !opts.some((o) => o.value === scenario)) {
      setScenario(opts[0].value)
    }
  }, [category, scenario])

  const filteredIssues = useMemo(() => {
    if (!fsckResult) return []
    if (!severityFilter) return fsckResult.issues
    return fsckResult.issues.filter((i) => i.severity === severityFilter)
  }, [fsckResult, severityFilter])

  const handleCreateExperiment = async () => {
    if (!sourceDisk) {
      onNotify('error', 'Select an original GFS1 disk image first')
      return
    }
    setBusy(true)
    try {
      const exp = await createCorruptionExperiment(sourceDisk, sandboxName.trim())
      setSelectedExp(exp)
      setFsckResult(null)
      setRepairPlan(null)
      setRepairResult(null)
      setComparison(null)
      setSandboxName('')
      await refreshExperiments()
      onNotify('success', `Sandbox clone '${exp.sandbox_filename}' created and SHA-256 verified`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to create sandbox')
    } finally {
      setBusy(false)
    }
  }

  const handleInject = async () => {
    if (!selectedExp) return
    setConfirmAction(null)
    setBusy(true)
    try {
      const updated = await injectCorruption(selectedExp.experiment_id, {
        category,
        scenario,
        target_block: Number(targetBlock) || 0,
        target_inode: Number(targetInode) || 0,
        severity,
        seed,
        confirm: true,
      })
      setSelectedExp(updated)
      setRepairPlan(null)
      setRepairResult(null)
      await refreshExperiments()
      onNotify('success', `Injected '${category}:${scenario}' on sandbox copy`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Corruption injection failed')
    } finally {
      setBusy(false)
    }
  }

  const handleRunCheck = async () => {
    if (!selectedExp) return
    setBusy(true)
    try {
      const res = await checkCorruptionExperiment(selectedExp.experiment_id, checkMode)
      setFsckResult(res)
      setInspectedIssue(res.issues[0] ?? null)
      onNotify('success', `gfsck (${checkMode}) completed: ${res.total_issues} issue(s)`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Consistency check failed')
    } finally {
      setBusy(false)
    }
  }

  const handleGeneratePlan = async () => {
    if (!selectedExp) return
    setBusy(true)
    try {
      const plan = await generateRepairPlan(selectedExp.experiment_id, checkMode)
      setRepairPlan(plan)
      setFsckResult(plan.initial_check)
      setSelectedActionIds(
        plan.actions.filter((a) => a.selected_by_default).map((a) => a.action_id),
      )
      onNotify('success', `Generated repair plan with ${plan.actions.length} action(s)`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Repair plan generation failed')
    } finally {
      setBusy(false)
    }
  }

  const handleApplyRepair = async (dryRun: boolean) => {
    if (!selectedExp || !repairPlan) return
    setConfirmAction(null)
    setBusy(true)
    try {
      const res = await applyRepairPlan(selectedExp.experiment_id, {
        plan_id: repairPlan.plan_id,
        expected_disk_sha256: repairPlan.disk_sha256_at_plan,
        selected_action_ids: selectedActionIds,
        dry_run: dryRun,
        confirm: !dryRun,
      })
      setRepairResult(res)
      if (!dryRun) {
        setFsckResult(res.post_check)
        await refreshExperiments()
        const cmp = await compareCorruptionExperiment(
          selectedExp.experiment_id,
          'corrupted_vs_repaired',
        )
        setCompareMode('corrupted_vs_repaired')
        setComparison(cmp)
        onNotify(
          'success',
          `Repair completed: ${res.issues_resolved} resolved, ${res.issues_remaining} remaining`,
        )
      } else {
        onNotify('success', `Dry-run preview: ${res.applied_action_ids.length} action(s) ready`)
      }
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Repair execution failed')
    } finally {
      setBusy(false)
    }
  }

  const handleCompare = async (
    mode: 'original_vs_corrupted' | 'corrupted_vs_repaired' | 'original_vs_repaired',
  ) => {
    if (!selectedExp) return
    setCompareMode(mode)
    setBusy(true)
    try {
      const cmp = await compareCorruptionExperiment(selectedExp.experiment_id, mode)
      setComparison(cmp)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Comparison failed')
    } finally {
      setBusy(false)
    }
  }

  const handleDeleteExperiment = async () => {
    if (!selectedExp) return
    setConfirmAction(null)
    setBusy(true)
    try {
      await deleteCorruptionExperiment(selectedExp.experiment_id, true)
      setSelectedExp(null)
      setFsckResult(null)
      setRepairPlan(null)
      setRepairResult(null)
      setComparison(null)
      await refreshExperiments()
      onNotify('success', 'Sandbox experiment deleted')
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Delete failed')
    } finally {
      setBusy(false)
    }
  }

  return (
    <div className="space-y-6">
      {/* Safety Banner */}
      <section className="rounded-xl border border-amber-500/30 bg-amber-500/10 px-4 py-3 text-xs text-amber-200">
        <div className="flex flex-wrap items-center justify-between gap-2">
          <div className="flex items-center gap-2">
            <ShieldAlert className="h-4 w-4 shrink-0 text-amber-300" aria-hidden="true" />
            <span>
              <strong>Sandbox Isolation Active:</strong> All corruption &amp; repair operations execute
              exclusively on disposable clones in <code className="font-mono-tabular">disk_images/sandbox/</code>.
              Original disk images remain read-only and SHA-256 preserved.
            </span>
          </div>
          {selectedExp && (
            <span className="font-mono-tabular rounded border border-emerald-500/40 bg-emerald-500/15 px-2 py-0.5 text-[11px] text-emerald-300">
              Orig SHA: {selectedExp.original_sha256.slice(0, 12)}...
            </span>
          )}
        </div>
      </section>

      {/* A. Experiment Setup & G. Experiment Management */}
      <div className="grid gap-6 xl:grid-cols-12">
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-5">
          <h2 className="text-sm font-semibold text-slate-100">Create Sandbox Experiment</h2>
          <div className="mt-3 space-y-3 text-xs">
            <div>
              <label htmlFor="lab-src-disk" className="block text-slate-400">
                Original GFS1 Disk Image
              </label>
              <select
                id="lab-src-disk"
                value={sourceDisk}
                onChange={(e) => setSourceDisk(e.target.value)}
                className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
              >
                <option value="">-- Select disk --</option>
                {disks.map((d) => (
                  <option key={d.disk_name} value={d.disk_name}>
                    {d.disk_name}
                  </option>
                ))}
              </select>
            </div>
            <div>
              <label htmlFor="lab-sb-name" className="block text-slate-400">
                Sandbox Name (optional)
              </label>
              <input
                id="lab-sb-name"
                type="text"
                value={sandboxName}
                onChange={(e) => setSandboxName(e.target.value)}
                placeholder="e.g. sb_superblock_demo"
                className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
              />
            </div>
            <button
              type="button"
              disabled={busy || !sourceDisk}
              onClick={() => void handleCreateExperiment()}
              className="inline-flex w-full items-center justify-center gap-1.5 rounded-lg bg-cyan-600 px-3.5 py-2 font-medium text-white hover:bg-cyan-500 disabled:opacity-50"
            >
              <Plus className="h-4 w-4" aria-hidden="true" />
              <span>Clone to Sandbox &amp; Verify SHA-256</span>
            </button>
          </div>
        </section>

        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-7">
          <div className="flex items-center justify-between">
            <h2 className="text-sm font-semibold text-slate-100">
              Sandbox Experiments ({experiments.length})
            </h2>
            <button
              type="button"
              onClick={() => void refreshExperiments()}
              className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2.5 py-1 text-xs text-slate-200 hover:border-cyan-500/50"
            >
              <RefreshCw className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Refresh</span>
            </button>
          </div>
          {experiments.length === 0 ? (
            <p className="mt-4 text-xs text-slate-400">
              No sandbox experiments yet. Clone an existing GFS1 disk image to begin.
            </p>
          ) : (
            <div className="mt-3 max-h-48 overflow-y-auto divide-y divide-slate-800/70">
              {experiments.map((exp) => {
                const active = selectedExp?.experiment_id === exp.experiment_id
                return (
                  <div
                    key={exp.experiment_id}
                    className={`flex flex-wrap items-center justify-between gap-2 py-2 text-xs ${
                      active ? 'bg-cyan-500/10 px-2 rounded' : ''
                    }`}
                  >
                    <div>
                      <button
                        type="button"
                        onClick={() => {
                          setSelectedExp(exp)
                          setFsckResult(null)
                          setRepairPlan(null)
                          setRepairResult(null)
                          setComparison(null)
                        }}
                        className="font-mono-tabular font-semibold text-cyan-300 hover:underline"
                      >
                        {exp.sandbox_filename}
                      </button>
                      <p className="text-[11px] text-slate-400">
                        Source: {exp.source_disk_name} · Ops: {exp.operations.length} ·{' '}
                        {exp.experiment_id}
                      </p>
                    </div>
                    <div className="flex items-center gap-1.5">
                      <a
                        href={getExperimentManifestUrl(exp.experiment_id)}
                        download={`${exp.experiment_id}.exp.json`}
                        className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2 py-1 text-slate-200 hover:border-cyan-500/50"
                      >
                        <FileJson className="h-3.5 w-3.5" aria-hidden="true" />
                        <span>Manifest</span>
                      </a>
                      <a
                        href={getExperimentDownloadUrl(exp.experiment_id)}
                        download={exp.sandbox_filename}
                        className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2 py-1 text-slate-200 hover:border-cyan-500/50"
                      >
                        <Download className="h-3.5 w-3.5" aria-hidden="true" />
                        <span>.gfs</span>
                      </a>
                      {active && (
                        <button
                          type="button"
                          aria-label="Delete selected experiment"
                          onClick={() => setConfirmAction('delete')}
                          className="inline-flex items-center gap-1 rounded border border-rose-500/40 bg-rose-500/10 px-2 py-1 text-rose-300 hover:bg-rose-500/20"
                        >
                          <Trash2 className="h-3.5 w-3.5" aria-hidden="true" />
                        </button>
                      )}
                    </div>
                  </div>
                )
              })}
            </div>
          )}
        </section>
      </div>

      {selectedExp && (
        <>
          {/* B. Experiment Overview & Corruption Injection */}
          <div className="grid gap-6 xl:grid-cols-12">
            <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-5">
              <h2 className="text-sm font-semibold text-slate-100">Inject Controlled Corruption</h2>
              <div className="mt-3 space-y-3 text-xs">
                <div>
                  <label htmlFor="corr-cat" className="block text-slate-400">
                    Corruption Category
                  </label>
                  <select
                    id="corr-cat"
                    value={category}
                    onChange={(e) => setCategory(e.target.value)}
                    className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
                  >
                    <option value="primary_superblock">A. Primary Superblock</option>
                    <option value="backup_superblock">B. Backup Superblock</option>
                    <option value="dual_superblock">C. Dual Superblock</option>
                    <option value="block_bitmap">D. Block Bitmap</option>
                    <option value="inode_bitmap">E. Inode Bitmap</option>
                    <option value="inode">F. Inode Table</option>
                    <option value="directory">G. Directory Entries</option>
                    <option value="journal">H. Journal / Crash</option>
                    <option value="payload">I. File Payload</option>
                    <option value="advanced">J. Advanced / Multi / Orphan</option>
                  </select>
                </div>
                <div>
                  <label htmlFor="corr-scen" className="block text-slate-400">
                    Scenario
                  </label>
                  <select
                    id="corr-scen"
                    value={scenario}
                    onChange={(e) => setScenario(e.target.value)}
                    className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-slate-100"
                  >
                    {(SCENARIOS_BY_CATEGORY[category] ?? []).map((s) => (
                      <option key={s.value} value={s.value}>
                        {s.label}
                      </option>
                    ))}
                  </select>
                </div>
                <div className="grid grid-cols-2 gap-2">
                  <div>
                    <label htmlFor="corr-blk" className="block text-slate-400">
                      Target Block (0=auto)
                    </label>
                    <input
                      id="corr-blk"
                      type="number"
                      min={0}
                      value={targetBlock}
                      onChange={(e) => setTargetBlock(e.target.value)}
                      className="font-mono-tabular mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
                    />
                  </div>
                  <div>
                    <label htmlFor="corr-ino" className="block text-slate-400">
                      Target Inode (0=auto)
                    </label>
                    <input
                      id="corr-ino"
                      type="number"
                      min={0}
                      value={targetInode}
                      onChange={(e) => setTargetInode(e.target.value)}
                      className="font-mono-tabular mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
                    />
                  </div>
                  <div>
                    <label htmlFor="corr-sev" className="block text-slate-400">
                      Severity
                    </label>
                    <input
                      id="corr-sev"
                      type="number"
                      min={1}
                      max={16}
                      value={severity}
                      onChange={(e) => setSeverity(Number(e.target.value) || 1)}
                      className="font-mono-tabular mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
                    />
                  </div>
                  <div>
                    <label htmlFor="corr-seed" className="block text-slate-400">
                      Deterministic Seed
                    </label>
                    <input
                      id="corr-seed"
                      type="number"
                      min={0}
                      value={seed}
                      onChange={(e) => setSeed(Number(e.target.value) || 0)}
                      className="font-mono-tabular mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
                    />
                  </div>
                </div>
                <button
                  type="button"
                  disabled={busy}
                  onClick={() => setConfirmAction('inject')}
                  className="inline-flex w-full items-center justify-center gap-1.5 rounded-lg border border-rose-500/40 bg-rose-500/15 px-3.5 py-2 font-medium text-rose-200 hover:bg-rose-500/25 disabled:opacity-50"
                >
                  <AlertTriangle className="h-4 w-4" aria-hidden="true" />
                  <span>Inject Corruption into Sandbox</span>
                </button>
              </div>
            </section>

            <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-7">
              <div className="flex items-center justify-between gap-2">
                <h2 className="text-sm font-semibold text-slate-100">Experiment Overview &amp; Ground-Truth Log</h2>
                <button
                  type="button"
                  onClick={() =>
                    onExportReport({
                      reportType: 'corruption_repair',
                      diskName: selectedExp.source_disk_name,
                      experimentId: selectedExp.experiment_id,
                      title: `Corruption & Repair — ${selectedExp.experiment_id}`,
                    })
                  }
                  className="rounded-md border border-emerald-500/40 bg-emerald-500/10 px-2.5 py-1 text-xs text-emerald-300 hover:bg-emerald-500/20"
                >
                  Generate Corruption Report
                </button>
              </div>
              <dl className="mt-3 grid grid-cols-2 gap-3 rounded-lg border border-slate-800 bg-[#0b0f17] p-3 text-xs">
                <div>
                  <dt className="text-slate-400">Original Source SHA-256</dt>
                  <dd className="font-mono-tabular break-all text-[11px] text-emerald-300">
                    {selectedExp.original_sha256}
                  </dd>
                </div>
                <div>
                  <dt className="text-slate-400">Current Sandbox SHA-256</dt>
                  <dd className="font-mono-tabular break-all text-[11px] text-cyan-300">
                    {selectedExp.current_sandbox_sha256}
                  </dd>
                </div>
                <div>
                  <dt className="text-slate-400">Operations / Updated</dt>
                  <dd className="font-mono-tabular text-slate-200">
                    {selectedExp.operations.length} op(s) · {selectedExp.updated_at_utc}
                  </dd>
                </div>
                <div>
                  <dt className="text-slate-400">Consistency Status</dt>
                  <dd className="font-mono-tabular text-slate-100">
                    {fsckResult ? fsckResult.consistency_status : 'Not checked yet'}
                  </dd>
                </div>
              </dl>
              <div className="mt-3 max-h-40 overflow-y-auto space-y-2">
                {selectedExp.operations.map((op) => (
                  <div
                    key={op.operation_id}
                    className="rounded border border-slate-800 bg-[#0b0f17] px-3 py-2 text-xs"
                  >
                    <p className="font-medium text-slate-100">
                      [{op.category}:{op.scenario}] {op.description}
                    </p>
                    <p className="font-mono-tabular mt-0.5 text-[11px] text-slate-400">
                      Blocks: [{op.affected_blocks.join(', ') || 'none'}] · After SHA:{' '}
                      {op.after_sandbox_sha256.slice(0, 16)}...
                    </p>
                  </div>
                ))}
              </div>
            </section>
          </div>

          {/* C. Consistency Checker & D. Repair Plan */}
          <section className="rounded-xl border border-slate-800 bg-[#111827] p-5">
            <div className="flex flex-wrap items-center justify-between gap-3">
              <div>
                <h2 className="text-sm font-semibold text-slate-100">
                  Independent Consistency Checker (gfsck) &amp; Repair Plan
                </h2>
                <p className="text-xs text-slate-400">
                  Reads binary disk structures independently without consulting corruption logs.
                </p>
              </div>
              <div className="flex flex-wrap items-center gap-2">
                <select
                  aria-label="Check Mode"
                  value={checkMode}
                  onChange={(e) => setCheckMode(e.target.value as 'fast' | 'full')}
                  className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-xs text-slate-100"
                >
                  <option value="full">Full Check (Metadata + Payload CRC32)</option>
                  <option value="fast">Fast Check (Metadata Only)</option>
                </select>
                <button
                  type="button"
                  disabled={busy}
                  onClick={() => void handleRunCheck()}
                  className="inline-flex items-center gap-1.5 rounded-lg bg-cyan-600 px-3 py-1.5 text-xs font-medium text-white hover:bg-cyan-500 disabled:opacity-50"
                >
                  <Play className="h-3.5 w-3.5" aria-hidden="true" />
                  <span>Run gfsck</span>
                </button>
                <button
                  type="button"
                  disabled={busy}
                  onClick={() => void handleGeneratePlan()}
                  className="inline-flex items-center gap-1.5 rounded-lg border border-cyan-500/40 bg-cyan-500/15 px-3 py-1.5 text-xs font-medium text-cyan-200 hover:bg-cyan-500/25 disabled:opacity-50"
                >
                  <Wrench className="h-3.5 w-3.5" aria-hidden="true" />
                  <span>Generate Repair Plan</span>
                </button>
              </div>
            </div>

            {fsckResult && (
              <div className="mt-4 space-y-4">
                <div className="grid grid-cols-2 gap-3 sm:grid-cols-5 text-xs">
                  <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
                    <span className="text-slate-400">Critical</span>
                    <p className="font-mono-tabular mt-1 text-lg font-semibold text-rose-300">
                      {fsckResult.critical_count}
                    </p>
                  </div>
                  <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
                    <span className="text-slate-400">Errors</span>
                    <p className="font-mono-tabular mt-1 text-lg font-semibold text-amber-300">
                      {fsckResult.error_count}
                    </p>
                  </div>
                  <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
                    <span className="text-slate-400">Warnings</span>
                    <p className="font-mono-tabular mt-1 text-lg font-semibold text-yellow-300">
                      {fsckResult.warning_count}
                    </p>
                  </div>
                  <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
                    <span className="text-slate-400">Preserved Files</span>
                    <p className="font-mono-tabular mt-1 text-lg font-semibold text-emerald-300">
                      {fsckResult.preserved_files_count}
                    </p>
                  </div>
                  <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
                    <span className="text-slate-400">Damaged Files</span>
                    <p className="font-mono-tabular mt-1 text-lg font-semibold text-rose-300">
                      {fsckResult.damaged_files_count}
                    </p>
                  </div>
                </div>

                <div className="flex items-center gap-2 text-xs">
                  <span className="text-slate-400">Filter severity:</span>
                  {(['', 'critical', 'error', 'warning'] as const).map((sev) => (
                    <button
                      key={sev || 'all'}
                      type="button"
                      onClick={() => setSeverityFilter(sev)}
                      className={`rounded px-2 py-0.5 ${
                        severityFilter === sev
                          ? 'bg-cyan-600 text-white'
                          : 'border border-slate-700 bg-slate-800 text-slate-300'
                      }`}
                    >
                      {sev || 'All'}
                    </button>
                  ))}
                </div>

                {filteredIssues.length === 0 ? (
                  <p className="flex items-center gap-1.5 text-xs text-emerald-300">
                    <CheckCircle2 className="h-4 w-4" aria-hidden="true" />
                    <span>No consistency issues detected for this filter.</span>
                  </p>
                ) : (
                  <div className="grid gap-4 lg:grid-cols-12">
                    <div className="max-h-64 overflow-y-auto lg:col-span-7 divide-y divide-slate-800/70 border border-slate-800 rounded-lg bg-[#0b0f17]">
                      {filteredIssues.map((iss) => (
                        <button
                          key={iss.issue_id}
                          type="button"
                          onClick={() => setInspectedIssue(iss)}
                          className={`w-full px-3 py-2 text-left text-xs hover:bg-slate-800/50 ${
                            inspectedIssue?.issue_id === iss.issue_id ? 'bg-cyan-500/10' : ''
                          }`}
                        >
                          <div className="flex items-center justify-between gap-2">
                            <span className="font-mono-tabular font-semibold text-slate-100">
                              {iss.issue_type}
                            </span>
                            <span
                              className={`rounded-full border px-2 py-0.5 text-[10px] ${
                                SEVERITY_BADGE[iss.severity] ?? SEVERITY_BADGE.information
                              }`}
                            >
                              {iss.severity}
                            </span>
                          </div>
                          <p className="mt-1 text-slate-400">{iss.description}</p>
                        </button>
                      ))}
                    </div>
                    <div className="lg:col-span-5 rounded-lg border border-slate-800 bg-[#0b0f17] p-3 text-xs">
                      {inspectedIssue ? (
                        <div className="space-y-2">
                          <div className="flex items-center justify-between">
                            <span className="font-mono-tabular font-semibold text-cyan-300">
                              {inspectedIssue.issue_id} · {inspectedIssue.affected_structure}
                            </span>
                            {inspectedIssue.affected_block > 0 && (
                              <button
                                type="button"
                                onClick={() =>
                                  onOpenInHexViewer({
                                    sourceType: 'block',
                                    blockId: inspectedIssue.affected_block,
                                  })
                                }
                                className="inline-flex items-center gap-1 text-cyan-300 hover:underline"
                              >
                                <Binary className="h-3.5 w-3.5" aria-hidden="true" />
                                <span>Blk {inspectedIssue.affected_block}</span>
                              </button>
                            )}
                          </div>
                          <p className="text-slate-300">{inspectedIssue.evidence}</p>
                          <p className="text-slate-400">
                            Expected: <span className="font-mono-tabular text-slate-200">{inspectedIssue.expected_value}</span>
                          </p>
                          <p className="text-slate-400">
                            Observed: <span className="font-mono-tabular text-rose-300">{inspectedIssue.observed_value}</span>
                          </p>
                          <p className="text-slate-400">
                            Repairability: <span className="font-mono-tabular text-amber-300">{inspectedIssue.repairability}</span>
                          </p>
                          <p className="text-emerald-300">{inspectedIssue.recommended_action}</p>
                        </div>
                      ) : (
                        <p className="text-slate-400">Select an issue to inspect evidence.</p>
                      )}
                    </div>
                  </div>
                )}
              </div>
            )}

            {repairPlan && (
              <div className="mt-5 border-t border-slate-800 pt-4 space-y-3">
                <div className="flex flex-wrap items-center justify-between gap-2">
                  <h3 className="text-xs font-semibold uppercase tracking-wider text-slate-300">
                    Repair Plan ({repairPlan.actions.length} Repairable,{' '}
                    {repairPlan.unrepairable_or_ambiguous.length} Ambiguous/Unrepairable)
                  </h3>
                  <div className="flex items-center gap-2">
                    <button
                      type="button"
                      disabled={busy || repairPlan.actions.length === 0}
                      onClick={() => void handleApplyRepair(true)}
                      className="rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-xs text-slate-200 hover:border-cyan-500/50 disabled:opacity-50"
                    >
                      Dry-Run Preview
                    </button>
                    <button
                      type="button"
                      disabled={busy || selectedActionIds.length === 0}
                      onClick={() => setConfirmAction('repair')}
                      className="inline-flex items-center gap-1 rounded-lg bg-emerald-600 px-3 py-1.5 text-xs font-medium text-white hover:bg-emerald-500 disabled:opacity-50"
                    >
                      <ShieldCheck className="h-3.5 w-3.5" aria-hidden="true" />
                      <span>Apply Selected Repairs</span>
                    </button>
                  </div>
                </div>
                {repairPlan.actions.map((act) => (
                  <label
                    key={act.action_id}
                    className="flex items-start gap-2.5 rounded-lg border border-slate-800 bg-[#0b0f17] p-3 text-xs cursor-pointer"
                  >
                    <input
                      type="checkbox"
                      checked={selectedActionIds.includes(act.action_id)}
                      onChange={(e) => {
                        setSelectedActionIds((prev) =>
                          e.target.checked
                            ? [...prev, act.action_id]
                            : prev.filter((id) => id !== act.action_id),
                        )
                      }}
                      className="mt-0.5"
                    />
                    <div className="flex-1">
                      <p className="font-mono-tabular font-semibold text-slate-100">
                        {act.action_type} — {act.target_structure}
                      </p>
                      <p className="mt-0.5 text-slate-300">{act.proposed_value}</p>
                      <p className="mt-0.5 text-[11px] text-slate-400">
                        Risk: {act.data_loss_implication}
                      </p>
                    </div>
                  </label>
                ))}
              </div>
            )}
          </section>

          {/* E. Repair Results & F. Before/After Visual Comparison */}
          {(repairResult || selectedExp.operations.length > 0) && (
            <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 space-y-4">
              <div className="flex flex-wrap items-center justify-between gap-2">
                <h2 className="flex items-center gap-2 text-sm font-semibold text-slate-100">
                  <GitCompare className="h-4 w-4 text-cyan-300" aria-hidden="true" />
                  <span>Before-and-After Forensic Comparison</span>
                </h2>
                <div className="flex flex-wrap gap-1.5 text-xs">
                  {(
                    [
                      ['original_vs_corrupted', 'Original vs Corrupted'],
                      ['corrupted_vs_repaired', 'Corrupted vs Repaired'],
                      ['original_vs_repaired', 'Original vs Repaired'],
                    ] as const
                  ).map(([m, lbl]) => (
                    <button
                      key={m}
                      type="button"
                      onClick={() => void handleCompare(m)}
                      className={`rounded-lg px-2.5 py-1 ${
                        compareMode === m
                          ? 'bg-cyan-600 text-white'
                          : 'border border-slate-700 bg-slate-800 text-slate-200'
                      }`}
                    >
                      {lbl}
                    </button>
                  ))}
                </div>
              </div>

              {/* Visual Region Strip matching reference diagram */}
              {comparison && (
                <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-4 space-y-3 text-xs">
                  <div className="grid grid-cols-12 gap-2 text-center font-medium">
                    <div className="col-span-2 rounded bg-slate-700/70 py-2 text-slate-200">
                      Boot
                    </div>
                    <div
                      className={`col-span-3 rounded py-2 ${
                        comparison.target_check.primary_sb_valid
                          ? 'bg-emerald-600/80 text-white'
                          : 'bg-rose-600/80 text-white'
                      }`}
                    >
                      Primary SB
                    </div>
                    <div
                      className={`col-span-3 rounded py-2 ${
                        comparison.target_check.backup_sb_valid
                          ? 'bg-emerald-600/80 text-white'
                          : 'bg-rose-600/80 text-white'
                      }`}
                    >
                      Backup SB
                    </div>
                    <div className="col-span-4 rounded bg-blue-600/80 py-2 text-white">
                      Remaining disk ({comparison.changed_blocks.length} blk diff)
                    </div>
                  </div>
                  <div className="flex flex-wrap items-center gap-4 text-[11px] text-slate-400">
                    <span className="inline-flex items-center gap-1">
                      <span className="h-2.5 w-2.5 rounded-full bg-emerald-500" /> Valid
                    </span>
                    <span className="inline-flex items-center gap-1">
                      <span className="h-2.5 w-2.5 rounded-full bg-rose-500" /> Corrupted
                    </span>
                    <span className="inline-flex items-center gap-1">
                      <span className="h-2.5 w-2.5 rounded-full bg-blue-500" /> Remaining disk
                    </span>
                    <span className="font-mono-tabular text-slate-300">
                      Bytes changed: {comparison.total_bytes_changed} · Issues:{' '}
                      {comparison.source_check.total_issues} → {comparison.target_check.total_issues}
                    </span>
                  </div>
                </div>
              )}

              {repairResult && (
                <div className="grid grid-cols-2 gap-3 sm:grid-cols-4 rounded-lg border border-slate-800 bg-[#0b0f17] p-3 text-xs">
                  <div>
                    <span className="text-slate-400">Issues Before → Remaining</span>
                    <p className="font-mono-tabular mt-1 font-semibold text-slate-100">
                      {repairResult.issues_before} → {repairResult.issues_remaining} (resolved{' '}
                      {repairResult.issues_resolved})
                    </p>
                  </div>
                  <div>
                    <span className="text-slate-400">Bytes / Blocks Changed</span>
                    <p className="font-mono-tabular mt-1 font-semibold text-cyan-300">
                      {repairResult.bytes_changed} B / {repairResult.blocks_changed.length} blk(s)
                    </p>
                  </div>
                  <div>
                    <span className="text-slate-400">Preserved / Damaged Files</span>
                    <p className="font-mono-tabular mt-1 font-semibold text-emerald-300">
                      {repairResult.post_check.preserved_files_count} intact /{' '}
                      {repairResult.post_check.damaged_files_count} damaged
                    </p>
                  </div>
                  <div>
                    <span className="text-slate-400">Post-Repair Status</span>
                    <p className="font-mono-tabular mt-1 font-semibold text-slate-100">
                      {repairResult.post_check.consistency_status}
                    </p>
                  </div>
                </div>
              )}
            </section>
          )}
        </>
      )}

      <ConfirmDialog
        open={confirmAction !== null}
        title={
          confirmAction === 'inject'
            ? 'Confirm Sandbox Corruption'
            : confirmAction === 'repair'
              ? 'Confirm Metadata Repair'
              : 'Delete Sandbox Experiment'
        }
        description={
          confirmAction === 'inject'
            ? `Inject '${category}:${scenario}' into sandbox '${selectedExp?.sandbox_filename ?? ''}'? Original disk '${selectedExp?.source_disk_name ?? ''}' will not be modified.`
            : confirmAction === 'repair'
              ? `Apply ${selectedActionIds.length} repair action(s) to '${selectedExp?.sandbox_filename ?? ''}'? A pre-repair snapshot will be saved.`
              : `Delete sandbox experiment '${selectedExp?.experiment_id ?? ''}' and its sandbox image?`
        }
        confirmLabel={
          confirmAction === 'inject'
            ? 'Inject Corruption'
            : confirmAction === 'repair'
              ? 'Apply Repairs'
              : 'Delete Experiment'
        }
        danger={confirmAction !== 'repair'}
        onCancel={() => setConfirmAction(null)}
        onConfirm={() => {
          if (confirmAction === 'inject') void handleInject()
          else if (confirmAction === 'repair') void handleApplyRepair(false)
          else if (confirmAction === 'delete') void handleDeleteExperiment()
        }}
      />
    </div>
  )
}
