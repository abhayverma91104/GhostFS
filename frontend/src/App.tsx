import { useCallback, useEffect, useState } from 'react'
import { CheckCircle2, Cpu, FolderLock, HardDrive, Layers, Terminal } from 'lucide-react'
import {
  Bar,
  BarChart,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import { NAV_ITEMS, Sidebar } from './components/Sidebar'
import { TopBar } from './components/TopBar'
import { BlockExplorerPage } from './pages/BlockExplorerPage'
import { CorruptionLabPage } from './pages/CorruptionLabPage'
import { DiskManagerPage } from './pages/DiskManagerPage'
import { DiskSchedulerPage } from './pages/DiskSchedulerPage'
import { FileExplorerPage } from './pages/FileExplorerPage'
import { ForensicReportsPage } from './pages/ForensicReportsPage'
import { HexViewerPage } from './pages/HexViewerPage'
import { RecoveryCenterPage } from './pages/RecoveryCenterPage'
import {
  fetchAvailableEvidence,
  fetchHealth,
  getFragmentationReport,
  listDisks,
  openDisk,
} from './services/api'
import type {
  AvailableEvidenceResponse,
  DiskListItem,
  FragmentationReport,
  HealthResponse,
  HexNavigationTarget,
  NavSectionId,
  ReportNavigationContext,
  VolumeStatus,
} from './types/api'

interface BannerNotice {
  kind: 'success' | 'error'
  message: string
}

export function App() {
  const [activeSection, setActiveSection] = useState<NavSectionId>('dashboard')
  const [health, setHealth] = useState<HealthResponse | null>(null)
  const [loadingHealth, setLoadingHealth] = useState<boolean>(true)
  const [healthError, setHealthError] = useState<string | null>(null)

  const [disks, setDisks] = useState<DiskListItem[]>([])
  const [loadingDisks, setLoadingDisks] = useState<boolean>(false)
  const [activeDisk, setActiveDisk] = useState<VolumeStatus | null>(null)
  const [fragReport, setFragReport] = useState<FragmentationReport | null>(null)
  const [notice, setNotice] = useState<BannerNotice | null>(null)
  const [recoveryHighlightBlocks, setRecoveryHighlightBlocks] = useState<number[]>([])
  const [recoveryHighlightLabel, setRecoveryHighlightLabel] = useState<string | null>(null)
  const [hexTarget, setHexTarget] = useState<HexNavigationTarget | null>(null)
  const [reportContext, setReportContext] = useState<ReportNavigationContext | null>(null)
  const [evidenceSummary, setEvidenceSummary] = useState<AvailableEvidenceResponse | null>(null)

  const openHexViewer = useCallback((target: HexNavigationTarget) => {
    setHexTarget({ ...target })
    setActiveSection('hex-viewer')
  }, [])

  const openReportsWithContext = useCallback((ctx: ReportNavigationContext) => {
    setReportContext({ ...ctx })
    setActiveSection('forensic-reports')
  }, [])

  const notify = useCallback((kind: 'success' | 'error', message: string) => {
    setNotice({ kind, message })
  }, [])

  const refreshDisks = useCallback(async () => {
    setLoadingDisks(true)
    try {
      const list = await listDisks()
      setDisks(list)
      setActiveDisk((prev) => {
        if (prev && list.some((d) => d.disk_name === prev.disk_name && d.valid)) {
          return prev
        }
        return null
      })
    } catch (err) {
      notify('error', err instanceof Error ? err.message : 'Failed to list virtual disks')
    } finally {
      setLoadingDisks(false)
    }
  }, [notify])

  const loadAll = useCallback(async () => {
    setLoadingHealth(true)
    setHealthError(null)
    try {
      const data = await fetchHealth()
      setHealth(data)
      await refreshDisks()
      try {
        setEvidenceSummary(await fetchAvailableEvidence())
      } catch {
        // Optional telemetry
      }
    } catch (err) {
      setHealth(null)
      setHealthError(err instanceof Error ? err.message : 'Failed to reach FastAPI backend')
    } finally {
      setLoadingHealth(false)
    }
  }, [refreshDisks])

  useEffect(() => {
    void loadAll()
  }, [loadAll])

  // Auto-open the first valid disk if none is mounted yet on initial startup.
  useEffect(() => {
    if (!activeDisk && disks.length > 0) {
      const firstValid = disks.find((d) => d.valid)
      if (firstValid) {
        void openDisk(firstValid.disk_name)
          .then((vol) => setActiveDisk(vol))
          .catch(() => {})
      }
    }
  }, [activeDisk, disks])

  // Keep Dashboard fragmentation telemetry synced with active disk
  useEffect(() => {
    if (!activeDisk) {
      setFragReport(null)
      return
    }
    void getFragmentationReport(activeDisk.disk_name)
      .then((rep) => setFragReport(rep))
      .catch(() => setFragReport(null))
  }, [activeDisk])

  const currentNav = NAV_ITEMS.find((item) => item.id === activeSection) ?? NAV_ITEMS[0]

  useEffect(() => {
    document.title = `${currentNav.label} | GhostFS Forensics Workstation`
  }, [currentNav.label])

  const blockChartData = fragReport
    ? [
        { region: 'Metadata', blocks: fragReport.reserved_metadata_blocks },
        { region: 'File Data', blocks: fragReport.file_data_blocks },
        { region: 'Dir Data', blocks: fragReport.directory_data_blocks },
        { region: 'Indirect', blocks: fragReport.indirect_pointer_blocks },
        { region: 'Journal', blocks: fragReport.journal_blocks },
        { region: 'Free', blocks: fragReport.free_blocks },
      ]
    : []

  return (
    <div className="flex min-h-screen bg-[#0b0f17] text-slate-100">
      <Sidebar activeSection={activeSection} onSelect={setActiveSection} />

      <div className="flex min-w-0 flex-1 flex-col">
        <TopBar
          title={currentNav.label}
          health={health}
          activeDisk={activeDisk}
          loading={loadingHealth}
          error={healthError}
          onRefresh={() => void loadAll()}
        />

        <main className="flex-1 space-y-6 p-6">
          {healthError && (
            <div
              role="alert"
              className="rounded-xl border border-rose-500/40 bg-rose-500/10 p-4 text-sm text-rose-200"
            >
              <p className="font-semibold">Backend / C++ Engine Verification Failed</p>
              <p className="mt-1 font-mono-tabular text-xs text-rose-300">{healthError}</p>
            </div>
          )}

          {notice && (
            <div
              role={notice.kind === 'error' ? 'alert' : 'status'}
              aria-live={notice.kind === 'error' ? 'assertive' : 'polite'}
              className={`flex items-center justify-between gap-4 rounded-xl border px-4 py-3 text-xs ${
                notice.kind === 'error'
                  ? 'border-rose-500/40 bg-rose-500/10 text-rose-200'
                  : 'border-emerald-500/40 bg-emerald-500/10 text-emerald-200'
              }`}
            >
              <span>{notice.message}</span>
              <button
                type="button"
                onClick={() => setNotice(null)}
                className="rounded px-2 py-0.5 text-slate-300 hover:bg-slate-800/60"
              >
                Dismiss
              </button>
            </div>
          )}

          {activeSection === 'dashboard' && (
            <div className="space-y-6">
              <section className="rounded-xl border border-slate-800 bg-[#111827] p-5">
                <div className="flex flex-wrap items-start justify-between gap-4">
                  <div>
                    <h2 className="text-base font-semibold text-slate-100">
                      System &amp; Active Volume Telemetry
                    </h2>
                    <p className="mt-1 text-sm text-slate-400">
                      Real measurements reported by the C++17 GFS1 engine over FastAPI.
                    </p>
                  </div>
                  <div className="flex flex-wrap items-center gap-2">
                    <button
                      type="button"
                      onClick={() => setActiveSection('disk-manager')}
                      className="rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-xs font-medium text-slate-200 hover:border-cyan-500/50"
                    >
                      Manage Disks
                    </button>
                    <button
                      type="button"
                      onClick={() => setActiveSection('file-explorer')}
                      className="rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-xs font-medium text-slate-200 hover:border-cyan-500/50"
                    >
                      File Explorer
                    </button>
                    <button
                      type="button"
                      onClick={() => setActiveSection('block-explorer')}
                      className="rounded-lg bg-cyan-600 px-3 py-1.5 text-xs font-medium text-white hover:bg-cyan-500"
                    >
                      Block Explorer
                    </button>
                  </div>
                </div>

                {health ? (
                  <div className="mt-5 grid gap-4 md:grid-cols-2 xl:grid-cols-4">
                    <div className="rounded-lg border border-slate-800 bg-[#0d131f] p-4">
                      <div className="flex items-center gap-2 text-xs text-slate-400">
                        <Cpu className="h-4 w-4 text-cyan-400" aria-hidden="true" />
                        <span>C++ Engine &amp; Policy</span>
                      </div>
                      <p className="font-mono-tabular mt-2 text-sm font-semibold text-slate-100">
                        {health.engine_info.engine} v{health.engine_info.version}
                      </p>
                      <p className="font-mono-tabular mt-1 text-xs text-cyan-300">
                        {activeDisk ? `Policy: ${activeDisk.alloc_policy}` : `C++ ${health.engine_info.cpp_standard}`}
                      </p>
                    </div>

                    <div className="rounded-lg border border-slate-800 bg-[#0d131f] p-4">
                      <div className="flex items-center gap-2 text-xs text-slate-400">
                        <HardDrive className="h-4 w-4 text-emerald-400" aria-hidden="true" />
                        <span>Disk Utilization</span>
                      </div>
                      <p className="font-mono-tabular mt-2 text-sm font-semibold text-slate-100">
                        {fragReport ? `${fragReport.disk_utilization_pct.toFixed(1)}% used` : `${disks.length} disk(s)`}
                      </p>
                      <p className="font-mono-tabular mt-1 text-xs text-slate-400">
                        {activeDisk
                          ? `${activeDisk.allocated_data_blocks} alloc · ${activeDisk.free_blocks} free blks`
                          : 'No active volume mounted'}
                      </p>
                    </div>

                    <div className="rounded-lg border border-slate-800 bg-[#0d131f] p-4">
                      <div className="flex items-center gap-2 text-xs text-slate-400">
                        <FolderLock className="h-4 w-4 text-indigo-400" aria-hidden="true" />
                        <span>External Fragmentation</span>
                      </div>
                      <p className="font-mono-tabular mt-2 text-sm font-semibold text-amber-300">
                        {fragReport ? `${fragReport.external_fragmentation_pct.toFixed(2)}%` : '—'}
                      </p>
                      <p className="font-mono-tabular mt-1 text-xs text-slate-400">
                        {fragReport
                          ? `Largest free extent: ${fragReport.largest_free_extent} blks`
                          : 'Mount a disk to inspect'}
                      </p>
                    </div>

                    <div className="rounded-lg border border-slate-800 bg-[#0d131f] p-4">
                      <div className="flex items-center gap-2 text-xs text-slate-400">
                        <Layers className="h-4 w-4 text-amber-400" aria-hidden="true" />
                        <span>Internal &amp; File Frag</span>
                      </div>
                      <p className="font-mono-tabular mt-2 text-sm font-semibold text-slate-100">
                        {fragReport
                          ? `${fragReport.internal_fragmentation_bytes} B (${fragReport.internal_fragmentation_pct.toFixed(1)}%)`
                          : '—'}
                      </p>
                      <p className="font-mono-tabular mt-1 text-xs text-slate-400">
                        {fragReport
                          ? `${fragReport.fragmented_file_count} fragmented / ${fragReport.total_files} files`
                          : 'Mount a disk to inspect'}
                      </p>
                    </div>
                  </div>
                ) : (
                  <div className="mt-5 rounded-lg border border-slate-800 bg-[#0d131f] p-4 text-sm text-slate-400">
                    {loadingHealth ? 'Querying /api/v1/health...' : 'No health payload available.'}
                  </div>
                )}
              </section>

              {fragReport && (
                <section className="rounded-xl border border-slate-800 bg-[#111827] p-5">
                  <h2 className="text-sm font-semibold text-slate-100">
                    Block Distribution — {fragReport.disk_name}
                  </h2>
                  <div className="mt-4 h-56 w-full">
                    <ResponsiveContainer width="100%" height="100%">
                      <BarChart data={blockChartData}>
                        <CartesianGrid strokeDasharray="3 3" stroke="#1e293b" />
                        <XAxis dataKey="region" stroke="#94a3b8" fontSize={12} />
                        <YAxis stroke="#94a3b8" fontSize={12} />
                        <Tooltip
                          contentStyle={{
                            backgroundColor: '#0f172a',
                            borderColor: '#334155',
                            color: '#f1f5f9',
                            fontSize: '12px',
                          }}
                        />
                        <Bar dataKey="blocks" name="Blocks" fill="#22d3ee" />
                      </BarChart>
                    </ResponsiveContainer>
                  </div>
                </section>
              )}

              {health && (
                <section className="rounded-xl border border-slate-800 bg-[#111827] p-5">
                  <div className="flex items-center gap-2 text-sm font-semibold text-slate-100">
                    <Terminal className="h-4 w-4 text-cyan-400" aria-hidden="true" />
                    <h2>Registered Engine Subsystems &amp; Workspace Paths</h2>
                  </div>
                  <div className="mt-4 grid gap-4 md:grid-cols-2">
                    <div className="rounded-lg border border-slate-800 bg-[#0d131f] p-4">
                      <h3 className="text-xs font-semibold uppercase tracking-wider text-slate-400">
                        Runtime Paths
                      </h3>
                      <dl className="mt-3 space-y-2 text-xs">
                        <div>
                          <dt className="text-slate-400">Engine Binary</dt>
                          <dd className="font-mono-tabular break-all text-slate-200">{health.engine_bin}</dd>
                        </div>
                        <div>
                          <dt className="text-slate-400">Disk Images Directory</dt>
                          <dd className="font-mono-tabular break-all text-slate-200">{health.disk_images_dir}</dd>
                        </div>
                      </dl>
                    </div>

                    <div className="rounded-lg border border-slate-800 bg-[#0d131f] p-4">
                      <h3 className="text-xs font-semibold uppercase tracking-wider text-slate-400">
                        Workspace Safety
                      </h3>
                      <p className="mt-2 flex items-center gap-1.5 text-sm font-semibold text-emerald-300">
                        <CheckCircle2 className="h-4 w-4" aria-hidden="true" />
                        <span>{health.engine_info.workspace_ready ? 'Confinement Verified' : 'Missing'}</span>
                      </p>
                      <ul className="mt-3 flex flex-wrap gap-2">
                        {health.engine_info.modules.map((mod) => (
                          <li
                            key={mod}
                            className="font-mono-tabular rounded-md border border-slate-700 bg-slate-800/70 px-2 py-0.5 text-xs text-slate-200"
                          >
                            {mod}
                          </li>
                        ))}
                      </ul>
                    </div>
                  </div>
                  {evidenceSummary && (
                    <div className="mt-4 grid gap-3 sm:grid-cols-4 rounded-lg border border-slate-800 bg-[#0d131f] p-3.5 text-xs">
                      <div>
                        <span className="text-slate-400">Recovered Artifacts</span>
                        <p className="font-mono-tabular mt-1 text-base font-semibold text-cyan-300">
                          {evidenceSummary.recovery_manifests.length}
                        </p>
                      </div>
                      <div>
                        <span className="text-slate-400">Corruption Experiments</span>
                        <p className="font-mono-tabular mt-1 text-base font-semibold text-amber-300">
                          {evidenceSummary.experiments.length}
                        </p>
                      </div>
                      <div>
                        <span className="text-slate-400">Generated Reports</span>
                        <p className="font-mono-tabular mt-1 text-base font-semibold text-emerald-300">
                          {evidenceSummary.reports_count}
                        </p>
                      </div>
                      <div className="flex items-center justify-end">
                        <button
                          type="button"
                          onClick={() =>
                            openReportsWithContext({
                              reportType: 'complete_investigation',
                              diskName: activeDisk?.disk_name,
                            })
                          }
                          className="rounded-lg bg-cyan-600 px-3 py-1.5 font-medium text-white hover:bg-cyan-500"
                        >
                          Open Forensic Reports
                        </button>
                      </div>
                    </div>
                  )}
                </section>
              )}
            </div>
          )}

          {activeSection === 'disk-manager' && (
            <DiskManagerPage
              disks={disks}
              activeDisk={activeDisk}
              loadingDisks={loadingDisks}
              onRefreshDisks={refreshDisks}
              onSelectActiveDisk={setActiveDisk}
              onOpenExplorer={() => setActiveSection('file-explorer')}
              onOpenInHexViewer={openHexViewer}
              onExportReport={openReportsWithContext}
              onNotify={notify}
            />
          )}

          {activeSection === 'file-explorer' && (
            <FileExplorerPage
              activeDisk={activeDisk}
              onUpdateVolume={setActiveDisk}
              onGoToDiskManager={() => setActiveSection('disk-manager')}
              onOpenInHexViewer={openHexViewer}
              onNotify={notify}
            />
          )}

          {activeSection === 'block-explorer' && (
            <BlockExplorerPage
              activeDisk={activeDisk}
              externalHighlightBlocks={recoveryHighlightBlocks}
              externalHighlightLabel={recoveryHighlightLabel}
              onClearExternalHighlight={() => {
                setRecoveryHighlightBlocks([])
                setRecoveryHighlightLabel(null)
              }}
              onUpdateVolume={setActiveDisk}
              onGoToDiskManager={() => setActiveSection('disk-manager')}
              onOpenInHexViewer={openHexViewer}
              onExportReport={openReportsWithContext}
              onNotify={notify}
            />
          )}

          {activeSection === 'recovery-center' && (
            <RecoveryCenterPage
              activeDisk={activeDisk}
              onGoToDiskManager={() => setActiveSection('disk-manager')}
              onHighlightInBlockExplorer={(blocks, label) => {
                setRecoveryHighlightBlocks(blocks)
                setRecoveryHighlightLabel(label)
                setActiveSection('block-explorer')
              }}
              onOpenInHexViewer={openHexViewer}
              onExportReport={openReportsWithContext}
              onNotify={notify}
            />
          )}

          {activeSection === 'hex-viewer' && (
            <HexViewerPage
              activeDisk={activeDisk}
              navigationTarget={hexTarget}
              onGoToDiskManager={() => setActiveSection('disk-manager')}
              onExportReport={openReportsWithContext}
              onNotify={notify}
            />
          )}

          {activeSection === 'corruption-lab' && (
            <CorruptionLabPage
              disks={disks}
              activeDisk={activeDisk}
              onOpenInHexViewer={openHexViewer}
              onExportReport={openReportsWithContext}
              onNotify={notify}
            />
          )}

          {activeSection === 'disk-scheduler' && (
            <DiskSchedulerPage
              activeDisk={activeDisk}
              onOpenInHexViewer={openHexViewer}
              onExportReport={openReportsWithContext}
              onNotify={notify}
            />
          )}

          {activeSection === 'forensic-reports' && (
            <ForensicReportsPage
              activeDisk={activeDisk}
              navigationContext={reportContext}
              onNotify={notify}
            />
          )}

          {activeSection !== 'dashboard' &&
            activeSection !== 'disk-manager' &&
            activeSection !== 'file-explorer' &&
            activeSection !== 'block-explorer' &&
            activeSection !== 'recovery-center' &&
            activeSection !== 'hex-viewer' &&
            activeSection !== 'corruption-lab' &&
            activeSection !== 'disk-scheduler' &&
            activeSection !== 'forensic-reports' && (
              <section className="rounded-xl border border-slate-800 bg-[#111827] p-6">
                <h2 className="text-base font-semibold text-slate-100">{currentNav.label}</h2>
                <p className="mt-2 text-sm text-slate-400">
                  Scheduled for <span className="font-mono-tabular text-cyan-300">{currentNav.phase}</span>.
                  No simulated or placeholder metrics are shown prior to C++ engine implementation.
                </p>
              </section>
            )}
        </main>
      </div>
    </div>
  )
}

export default App
