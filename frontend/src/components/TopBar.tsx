import { AlertTriangle, CheckCircle2, HardDrive, RefreshCw } from 'lucide-react'
import type { HealthResponse, VolumeStatus } from '../types/api'

interface TopBarProps {
  title: string
  health: HealthResponse | null
  activeDisk: VolumeStatus | null
  loading: boolean
  error: string | null
  onRefresh: () => void
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`
}

export function TopBar({
  title,
  health,
  activeDisk,
  loading,
  error,
  onRefresh,
}: TopBarProps) {
  const isHealthy = Boolean(health?.engine_reachable && !error)

  return (
    <header className="flex flex-wrap items-center justify-between gap-4 border-b border-slate-800 bg-[#0f1623] px-6 py-3.5">
      <div>
        <h1 className="text-lg font-semibold text-slate-100">{title}</h1>
        <p className="mt-0.5 flex items-center gap-2 text-xs text-slate-400">
          <HardDrive className="h-3.5 w-3.5 text-cyan-400" aria-hidden="true" />
          <span>Active Disk:</span>
          {activeDisk ? (
            <span className="font-mono-tabular text-slate-200">
              {activeDisk.disk_name} ({activeDisk.volume_label}) · {activeDisk.block_size}B blocks ·{' '}
              {formatBytes(activeDisk.used_data_bytes)} / {formatBytes(activeDisk.usable_data_bytes)} used
            </span>
          ) : (
            <span className="font-mono-tabular text-slate-400">None mounted</span>
          )}
        </p>
      </div>

      <div className="flex items-center gap-3">
        <div
          className={`flex items-center gap-2 rounded-full border px-3 py-1 text-xs font-medium ${
            isHealthy
              ? 'border-emerald-500/40 bg-emerald-500/10 text-emerald-300'
              : 'border-rose-500/40 bg-rose-500/10 text-rose-300'
          }`}
        >
          {isHealthy ? (
            <CheckCircle2 className="h-3.5 w-3.5" aria-hidden="true" />
          ) : (
            <AlertTriangle className="h-3.5 w-3.5" aria-hidden="true" />
          )}
          <span>
            {loading
              ? 'Checking C++ Engine...'
              : isHealthy
                ? `Engine Online (${health?.engine_info.fs_format})`
                : 'Engine Unreachable'}
          </span>
        </div>

        <button
          type="button"
          onClick={onRefresh}
          disabled={loading}
          className="inline-flex items-center gap-1.5 rounded-lg border border-slate-700 bg-slate-800/80 px-3 py-1.5 text-xs font-medium text-slate-200 transition-colors hover:border-cyan-500/50 hover:bg-slate-800 disabled:opacity-50"
        >
          <RefreshCw className={`h-3.5 w-3.5 ${loading ? 'animate-spin' : ''}`} aria-hidden="true" />
          <span>Refresh</span>
        </button>
      </div>
    </header>
  )
}
