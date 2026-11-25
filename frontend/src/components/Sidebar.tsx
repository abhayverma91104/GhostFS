import {
  Activity,
  Binary,
  Cpu,
  FileSearch,
  FileText,
  FolderTree,
  Grid3x3,
  HardDrive,
  ShieldAlert,
  type LucideIcon,
} from 'lucide-react'
import type { NavSectionId } from '../types/api'

export interface NavItem {
  id: NavSectionId
  label: string
  phase?: string
  icon: LucideIcon
}

export const NAV_ITEMS: NavItem[] = [
  { id: 'dashboard', label: 'Dashboard', icon: Activity },
  { id: 'disk-manager', label: 'Disk Manager', icon: HardDrive },
  { id: 'file-explorer', label: 'File Explorer', icon: FolderTree },
  { id: 'block-explorer', label: 'Block Explorer', icon: Grid3x3 },
  { id: 'recovery-center', label: 'Recovery Center', icon: FileSearch },
  { id: 'hex-viewer', label: 'Hex Viewer', icon: Binary },
  { id: 'corruption-lab', label: 'Corruption Lab', icon: ShieldAlert },
  { id: 'disk-scheduler', label: 'Disk Scheduler', icon: Cpu },
  { id: 'forensic-reports', label: 'Forensic Reports', icon: FileText },
]

interface SidebarProps {
  activeSection: NavSectionId
  onSelect: (id: NavSectionId) => void
}

export function Sidebar({ activeSection, onSelect }: SidebarProps) {
  return (
    <aside className="flex w-64 shrink-0 flex-col border-r border-slate-800 bg-[#0d131f]">
      <div className="flex items-center gap-3 border-b border-slate-800 px-5 py-4">
        <div className="flex h-9 w-9 items-center justify-center rounded-lg border border-cyan-500/40 bg-cyan-500/10 text-cyan-300">
          <HardDrive className="h-5 w-5" aria-hidden="true" />
        </div>
        <div>
          <p className="text-sm font-semibold tracking-wide text-slate-100">GhostFS</p>
          <p className="font-mono-tabular text-xs text-slate-400">GFS1 Forensics Lab</p>
        </div>
      </div>

      <nav aria-label="Primary" className="flex-1 overflow-y-auto px-3 py-4">
        <ul className="space-y-1">
          {NAV_ITEMS.map((item) => {
            const Icon = item.icon
            const isActive = activeSection === item.id
            return (
              <li key={item.id}>
                <button
                  type="button"
                  aria-current={isActive ? 'page' : undefined}
                  onClick={() => onSelect(item.id)}
                  className={`flex w-full items-center justify-between rounded-lg px-3 py-2.5 text-left text-sm transition-colors ${
                    isActive
                      ? 'border border-cyan-500/40 bg-cyan-500/10 font-medium text-cyan-200'
                      : 'border border-transparent text-slate-300 hover:border-slate-800 hover:bg-slate-800/50 hover:text-slate-100'
                  }`}
                >
                  <span className="flex items-center gap-2.5">
                    <Icon className="h-4 w-4 shrink-0" aria-hidden="true" />
                    <span>{item.label}</span>
                  </span>
                  {item.phase && (
                    <span className="font-mono-tabular rounded bg-slate-800/90 px-1.5 py-0.5 text-[11px] text-slate-400">
                      {item.phase}
                    </span>
                  )}
                </button>
              </li>
            )
          })}
        </ul>
      </nav>

      <div className="border-t border-slate-800 px-4 py-3 text-xs text-slate-400">
        <p className="font-medium text-slate-300">Workspace Safety</p>
        <p className="mt-0.5">Virtual .gfs images only. Physical disks locked out.</p>
      </div>
    </aside>
  )
}
