import { useEffect, useId, useRef, useState, type FormEvent } from 'react'
import {
  AlertTriangle,
  Binary,
  CheckCircle2,
  Database,
  HardDrive,
  Plus,
  RefreshCw,
  Trash2,
} from 'lucide-react'
import { ConfirmDialog } from '../components/ConfirmDialog'
import { createDisk, deleteDisk, formatDisk, openDisk, setAllocPolicy } from '../services/api'
import type {
  AllocPolicy,
  BlockSize,
  DiskListItem,
  HexNavigationTarget,
  ReportNavigationContext,
  VolumeStatus,
} from '../types/api'

interface DiskManagerPageProps {
  disks: DiskListItem[]
  activeDisk: VolumeStatus | null
  loadingDisks: boolean
  onRefreshDisks: () => Promise<void>
  onSelectActiveDisk: (status: VolumeStatus) => void
  onOpenExplorer: () => void
  onOpenInHexViewer: (target: HexNavigationTarget) => void
  onExportReport: (ctx: ReportNavigationContext) => void
  onNotify: (kind: 'success' | 'error', message: string) => void
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`
}

const SIZE_PRESETS = [
  { label: '256 KB (64 x 4KB blocks)', bytes: 256 * 1024 },
  { label: '512 KB', bytes: 512 * 1024 },
  { label: '1 MB', bytes: 1024 * 1024 },
  { label: '4 MB', bytes: 4 * 1024 * 1024 },
  { label: '16 MB', bytes: 16 * 1024 * 1024 },
  { label: '64 MB', bytes: 64 * 1024 * 1024 },
]

export function DiskManagerPage({
  disks,
  activeDisk,
  loadingDisks,
  onRefreshDisks,
  onSelectActiveDisk,
  onOpenExplorer,
  onOpenInHexViewer,
  onExportReport,
  onNotify,
}: DiskManagerPageProps) {
  const formHeadingId = useId()
  const nameInputId = useId()
  const nameHintId = useId()
  const sizeInputId = useId()
  const labelInputId = useId()
  const policyInputId = useId()
  const inodeInputId = useId()

  const [name, setName] = useState('')
  const [nameTouched, setNameTouched] = useState(false)
  const [sizeBytes, setSizeBytes] = useState<number>(1024 * 1024)
  const [blockSize, setBlockSize] = useState<BlockSize>(4096)
  const [label, setLabel] = useState('GhostFS')
  const [allocPolicy, setFormAllocPolicy] = useState<AllocPolicy>('first_fit')
  const [customInodes, setCustomInodes] = useState('')
  const [submitting, setSubmitting] = useState(false)

  const [pendingDelete, setPendingDelete] = useState<string | null>(null)
  const [pendingFormat, setPendingFormat] = useState<DiskListItem | null>(null)
  const [fmtBlockSize, setFmtBlockSize] = useState<BlockSize>(4096)
  const [fmtLabel, setFmtLabel] = useState('GhostFS')
  const [fmtPolicy, setFmtPolicy] = useState<AllocPolicy>('first_fit')
  const [busyAction, setBusyAction] = useState(false)

  const formatDialogRef = useRef<HTMLDialogElement>(null)
  const formatTitleId = useId()
  const nameInputRef = useRef<HTMLInputElement>(null)

  const trimmedName = name.trim()
  const nameInvalid =
    nameTouched &&
    (trimmedName.length === 0 || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,60}(\.gfs)?$/.test(trimmedName))

  useEffect(() => {
    const dialog = formatDialogRef.current
    if (!dialog) return
    if (pendingFormat && !dialog.open) {
      setFmtBlockSize((pendingFormat.block_size as BlockSize) || 4096)
      setFmtLabel(pendingFormat.volume_label || 'GhostFS')
      setFmtPolicy((pendingFormat.alloc_policy as AllocPolicy) || 'first_fit')
      dialog.showModal()
    } else if (!pendingFormat && dialog.open) {
      dialog.close()
    }
  }, [pendingFormat])

  useEffect(() => {
    const dialog = formatDialogRef.current
    if (!dialog) return
    const onClose = () => {
      dialog.returnValue = ''
      if (pendingFormat) setPendingFormat(null)
    }
    dialog.addEventListener('close', onClose)
    return () => dialog.removeEventListener('close', onClose)
  }, [pendingFormat])

  async function handleCreateSubmit(e: FormEvent) {
    e.preventDefault()
    setNameTouched(true)
    if (trimmedName.length === 0 || !/^[A-Za-z0-9][A-Za-z0-9._-]{0,60}(\.gfs)?$/.test(trimmedName)) {
      nameInputRef.current?.focus()
      return
    }
    const parsedInodes = customInodes.trim() === '' ? 0 : Number.parseInt(customInodes, 10)
    if (Number.isNaN(parsedInodes) || parsedInodes < 0) {
      onNotify('error', 'Custom inode count must be 0 (auto) or a positive integer >= 2')
      return
    }

    setSubmitting(true)
    try {
      const created = await createDisk({
        name: trimmedName,
        size_bytes: sizeBytes,
        block_size: blockSize,
        label: label.trim() || 'GhostFS',
        alloc_policy: allocPolicy,
        inode_count: parsedInodes,
      })
      onSelectActiveDisk(created)
      await onRefreshDisks()
      setName('')
      setNameTouched(false)
      setCustomInodes('')
      onNotify(
        'success',
        `Created and formatted ${created.disk_name} (${created.total_blocks} blocks @ ${created.block_size}B)`,
      )
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to create virtual disk')
    } finally {
      setSubmitting(false)
    }
  }

  async function handleMountDisk(diskName: string) {
    setBusyAction(true)
    try {
      const status = await openDisk(diskName)
      onSelectActiveDisk(status)
      onNotify('success', `Mounted ${status.disk_name} (${status.volume_label})`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to open virtual disk')
    } finally {
      setBusyAction(false)
    }
  }

  async function handleConfirmDelete() {
    if (!pendingDelete) return
    setBusyAction(true)
    try {
      await deleteDisk(pendingDelete)
      if (activeDisk?.disk_name === pendingDelete) {
        await onRefreshDisks()
      } else {
        await onRefreshDisks()
      }
      onNotify('success', `Deleted virtual disk ${pendingDelete}`)
      setPendingDelete(null)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to delete virtual disk')
    } finally {
      setBusyAction(false)
    }
  }

  async function handleConfirmFormat(e: FormEvent) {
    e.preventDefault()
    if (!pendingFormat) return
    setBusyAction(true)
    try {
      const formatted = await formatDisk(pendingFormat.disk_name, {
        block_size: fmtBlockSize,
        label: fmtLabel.trim() || 'GhostFS',
        alloc_policy: fmtPolicy,
      })
      onSelectActiveDisk(formatted)
      await onRefreshDisks()
      onNotify('success', `Reformatted ${formatted.disk_name} with ${formatted.block_size}B blocks`)
      setPendingFormat(null)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to format virtual disk')
    } finally {
      setBusyAction(false)
    }
  }

  async function handlePolicyChange(newPolicy: AllocPolicy) {
    if (!activeDisk) return
    setBusyAction(true)
    try {
      const updated = await setAllocPolicy(activeDisk.disk_name, newPolicy)
      onSelectActiveDisk(updated)
      await onRefreshDisks()
      onNotify(
        'success',
        `Updated active allocation policy to ${newPolicy} (applies to future allocations; existing files unchanged)`,
      )
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to update allocation policy')
    } finally {
      setBusyAction(false)
    }
  }

  return (
    <div className="space-y-6">
      <div className="grid gap-6 xl:grid-cols-12">
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-5">
          <div className="flex items-center gap-2">
            <Plus className="h-4 w-4 text-cyan-400" aria-hidden="true" />
            <h2 id={formHeadingId} className="text-base font-semibold text-slate-100">
              Create &amp; Format Virtual Disk (.gfs)
            </h2>
          </div>
          <p className="mt-1 text-xs text-slate-400">
            Creates an isolated virtual image inside <span className="font-mono-tabular">disk_images/</span> and formats it with GFS1.
          </p>

          <form
            aria-labelledby={formHeadingId}
            onSubmit={(e) => void handleCreateSubmit(e)}
            className="mt-4 space-y-4"
            noValidate
          >
            <div>
              <label htmlFor={nameInputId} className="block text-xs font-medium text-slate-300">
                Disk Image Name <span aria-hidden="true" className="text-rose-400">*</span>
              </label>
              <input
                ref={nameInputRef}
                id={nameInputId}
                name="disk_name"
                type="text"
                required
                value={name}
                onChange={(e) => setName(e.target.value)}
                onBlur={() => setNameTouched(true)}
                aria-describedby={nameHintId}
                aria-invalid={nameInvalid ? 'true' : undefined}
                autoCorrect="off"
                autoCapitalize="none"
                spellCheck={false}
                placeholder="case_evidence_01"
                className="mt-1.5 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100 placeholder:text-slate-500"
              />
              <p id={nameHintId} className={`mt-1 text-xs ${nameInvalid ? 'text-rose-400' : 'text-slate-400'}`}>
                {nameInvalid
                  ? 'Enter 1-61 alphanumeric characters, dots, dashes, or underscores (.gfs is added automatically).'
                  : 'Letters, digits, ".", "_", "-" only. Saved as <name>.gfs.'}
              </p>
            </div>

            <div className="grid gap-4 sm:grid-cols-2">
              <div>
                <label htmlFor={sizeInputId} className="block text-xs font-medium text-slate-300">
                  Disk Capacity
                </label>
                <select
                  id={sizeInputId}
                  name="size_bytes"
                  value={sizeBytes}
                  onChange={(e) => setSizeBytes(Number(e.target.value))}
                  className="mt-1.5 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100"
                >
                  {SIZE_PRESETS.map((preset) => (
                    <option key={preset.bytes} value={preset.bytes}>
                      {preset.label}
                    </option>
                  ))}
                </select>
              </div>

              <div>
                <label htmlFor={labelInputId} className="block text-xs font-medium text-slate-300">
                  Volume Label
                </label>
                <input
                  id={labelInputId}
                  name="label"
                  type="text"
                  maxLength={23}
                  value={label}
                  onChange={(e) => setLabel(e.target.value)}
                  className="mt-1.5 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100"
                />
              </div>
            </div>

            <fieldset className="rounded-lg border border-slate-800 bg-[#0b0f17]/70 p-3">
              <legend className="px-1 text-xs font-medium text-slate-300">Block Size (Bytes)</legend>
              <div className="mt-1 grid grid-cols-4 gap-2">
                {([512, 1024, 2048, 4096] as BlockSize[]).map((bs) => (
                  <label
                    key={bs}
                    className={`flex cursor-pointer items-center justify-center gap-1.5 rounded-md border px-2 py-1.5 text-xs font-mono-tabular ${
                      blockSize === bs
                        ? 'border-cyan-500/60 bg-cyan-500/15 text-cyan-200'
                        : 'border-slate-700 bg-slate-900 text-slate-300 hover:border-slate-600'
                    }`}
                  >
                    <input
                      type="radio"
                      name="block_size"
                      value={bs}
                      checked={blockSize === bs}
                      onChange={() => setBlockSize(bs)}
                      className="sr-only"
                    />
                    <span>{bs}B</span>
                  </label>
                ))}
              </div>
            </fieldset>

            <div className="grid gap-4 sm:grid-cols-2">
              <div>
                <label htmlFor={policyInputId} className="block text-xs font-medium text-slate-300">
                  Allocation Policy
                </label>
                <select
                  id={policyInputId}
                  name="alloc_policy"
                  value={allocPolicy}
                  onChange={(e) => setFormAllocPolicy(e.target.value as AllocPolicy)}
                  className="mt-1.5 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100"
                >
                  <option value="first_fit">First Fit</option>
                  <option value="best_fit">Best Fit</option>
                  <option value="worst_fit">Worst Fit</option>
                  <option value="bitmap_next_fit">Bitmap Next Fit</option>
                </select>
              </div>

              <div>
                <label htmlFor={inodeInputId} className="block text-xs font-medium text-slate-300">
                  Max Inodes (Optional)
                </label>
                <input
                  id={inodeInputId}
                  name="inode_count"
                  type="text"
                  inputMode="numeric"
                  value={customInodes}
                  onChange={(e) => setCustomInodes(e.target.value)}
                  placeholder="Auto"
                  className="mt-1.5 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100 placeholder:text-slate-500"
                />
              </div>
            </div>

            <button
              type="submit"
              disabled={submitting}
              className="inline-flex w-full items-center justify-center gap-2 rounded-lg bg-cyan-600 px-4 py-2.5 text-sm font-medium text-white transition-colors hover:bg-cyan-500 disabled:opacity-50"
            >
              <HardDrive className="h-4 w-4" aria-hidden="true" />
              <span>{submitting ? 'Creating & Formatting...' : 'Create & Format Disk Image'}</span>
            </button>
          </form>
        </section>

        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-7">
          <div className="flex items-center justify-between gap-4">
            <div>
              <h2 className="text-base font-semibold text-slate-100">Virtual Disk Images</h2>
              <p className="mt-0.5 text-xs text-slate-400">
                Stored in <span className="font-mono-tabular">disk_images/</span>. Select a volume to inspect or browse files.
              </p>
            </div>
            <button
              type="button"
              onClick={() => void onRefreshDisks()}
              disabled={loadingDisks}
              className="inline-flex items-center gap-1.5 rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-xs font-medium text-slate-200 hover:border-cyan-500/50 disabled:opacity-50"
            >
              <RefreshCw className={`h-3.5 w-3.5 ${loadingDisks ? 'animate-spin' : ''}`} aria-hidden="true" />
              <span>Scan Disks</span>
            </button>
          </div>

          {disks.length === 0 ? (
            <div className="mt-6 rounded-lg border border-dashed border-slate-700 bg-[#0b0f17]/60 p-8 text-center">
              <Database className="mx-auto h-8 w-8 text-slate-500" aria-hidden="true" />
              <p className="mt-3 text-sm font-medium text-slate-300">No virtual .gfs disk images found</p>
              <p className="mt-1 text-xs text-slate-400">
                Create your first GFS1 virtual disk on the left to begin filesystem and forensics labs.
              </p>
            </div>
          ) : (
            <div className="mt-4 overflow-x-auto">
              <table className="w-full border-collapse text-left text-xs">
                <caption className="sr-only">Available virtual GFS disk images</caption>
                <thead>
                  <tr className="border-b border-slate-800 text-slate-400">
                    <th scope="col" className="py-2.5 pr-3 font-medium">Image</th>
                    <th scope="col" className="px-3 py-2.5 font-medium">Geometry</th>
                    <th scope="col" className="px-3 py-2.5 font-medium">Data Usage</th>
                    <th scope="col" className="px-3 py-2.5 font-medium">Integrity</th>
                    <th scope="col" className="py-2.5 pl-3 text-right font-medium">Actions</th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-slate-800/80">
                  {disks.map((disk) => {
                    const isActive = activeDisk?.disk_name === disk.disk_name
                    return (
                      <tr key={disk.disk_name} className={isActive ? 'bg-cyan-500/5' : ''}>
                        <td className="py-3 pr-3">
                          <p className="font-mono-tabular font-semibold text-slate-100">
                            {disk.disk_name}
                          </p>
                          <p className="text-[11px] text-slate-400">
                            {disk.volume_label ?? (disk.valid ? 'GFS1' : 'Invalid / Unformatted')}
                          </p>
                        </td>
                        <td className="font-mono-tabular px-3 py-3 text-slate-300">
                          {disk.valid ? (
                            <>
                              <p>{formatBytes(disk.total_bytes)}</p>
                              <p className="text-[11px] text-slate-400">
                                {disk.total_blocks} blk x {disk.block_size}B
                              </p>
                            </>
                          ) : (
                            <span>{formatBytes(disk.total_bytes)}</span>
                          )}
                        </td>
                        <td className="font-mono-tabular px-3 py-3 text-slate-300">
                          {disk.valid ? (
                            <>
                              <p>
                                {disk.allocated_data_blocks} / {disk.data_blocks_count} blk
                              </p>
                              <p className="text-[11px] text-slate-400">
                                {disk.used_inodes} / {disk.total_inodes} inodes
                              </p>
                            </>
                          ) : (
                            <span className="text-rose-400">—</span>
                          )}
                        </td>
                        <td className="px-3 py-3">
                          {disk.valid && disk.primary_sb_crc_ok && !disk.dirty_journal_detected ? (
                            <span className="inline-flex items-center gap-1 rounded-full border border-emerald-500/40 bg-emerald-500/10 px-2 py-0.5 text-[11px] text-emerald-300">
                              <CheckCircle2 className="h-3 w-3" aria-hidden="true" />
                              <span>Clean</span>
                            </span>
                          ) : (
                            <span className="inline-flex items-center gap-1 rounded-full border border-amber-500/40 bg-amber-500/10 px-2 py-0.5 text-[11px] text-amber-300">
                              <AlertTriangle className="h-3 w-3" aria-hidden="true" />
                              <span>{disk.valid ? 'Dirty Journal' : 'Corrupt'}</span>
                            </span>
                          )}
                        </td>
                        <td className="py-3 pl-3 text-right">
                          <div className="inline-flex items-center gap-1.5">
                            <button
                              type="button"
                              disabled={!disk.valid || busyAction}
                              onClick={() => void handleMountDisk(disk.disk_name)}
                              className="rounded-md border border-cyan-500/40 bg-cyan-500/10 px-2.5 py-1 text-xs font-medium text-cyan-200 hover:bg-cyan-500/20 disabled:opacity-40"
                            >
                              {isActive ? 'Mounted' : 'Mount'}
                            </button>
                            <button
                              type="button"
                              disabled={busyAction}
                              onClick={() => setPendingFormat(disk)}
                              className="rounded-md border border-slate-700 bg-slate-800 px-2.5 py-1 text-xs text-slate-200 hover:border-slate-500 disabled:opacity-40"
                            >
                              Format
                            </button>
                            <button
                              type="button"
                              disabled={busyAction}
                              aria-label={`Delete ${disk.disk_name}`}
                              onClick={() => setPendingDelete(disk.disk_name)}
                              className="rounded-md border border-rose-500/30 bg-rose-500/10 p-1.5 text-rose-300 hover:bg-rose-500/20 disabled:opacity-40"
                            >
                              <Trash2 className="h-3.5 w-3.5" aria-hidden="true" />
                            </button>
                          </div>
                        </td>
                      </tr>
                    )
                  })}
                </tbody>
              </table>
            </div>
          )}
        </section>
      </div>

      {activeDisk && (
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5">
          <div className="flex flex-wrap items-center justify-between gap-4">
            <div>
              <h2 className="text-base font-semibold text-slate-100">
                Mounted Volume Layout &amp; Metadata Journal — {activeDisk.disk_name}
              </h2>
              <p className="mt-0.5 text-xs text-slate-400">
                GFS1 superblock geometry verified via IEEE CRC32 (Boot CRC:{' '}
                {activeDisk.boot_crc_ok ? 'OK' : 'FAIL'}, Primary SB:{' '}
                {activeDisk.primary_sb_crc_ok ? 'OK' : 'FAIL'}, Backup SB:{' '}
                {activeDisk.backup_sb_crc_ok ? 'OK' : 'FAIL'})
              </p>
            </div>
            <div className="flex flex-wrap items-center gap-3">
              <div className="flex items-center gap-2">
                <label htmlFor="active-policy-select" className="text-xs text-slate-400">
                  Policy:
                </label>
                <select
                  id="active-policy-select"
                  disabled={busyAction || activeDisk.dirty_journal_detected}
                  value={activeDisk.alloc_policy}
                  onChange={(e) => void handlePolicyChange(e.target.value as AllocPolicy)}
                  className="rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-xs text-slate-100 disabled:opacity-50"
                >
                  <option value="first_fit">First Fit</option>
                  <option value="best_fit">Best Fit</option>
                  <option value="worst_fit">Worst Fit</option>
                  <option value="bitmap_next_fit">Bitmap Next Fit</option>
                </select>
              </div>
              <button
                type="button"
                onClick={onOpenExplorer}
                className="rounded-lg bg-cyan-600 px-3.5 py-2 text-xs font-medium text-white hover:bg-cyan-500"
              >
                Browse Files in {activeDisk.disk_name}
              </button>
            </div>
          </div>

          <div className="mt-3 flex flex-wrap items-center gap-2">
            <span className="text-[11px] font-semibold uppercase tracking-wider text-slate-400">
              Hex Inspector:
            </span>
            <button
              type="button"
              onClick={() =>
                onOpenInHexViewer({
                  sourceType: 'block',
                  blockId: activeDisk.layout.primary_superblock,
                })
              }
              className="inline-flex items-center gap-1 rounded-md border border-slate-700 bg-slate-800/80 px-2.5 py-1 text-xs text-cyan-300 hover:border-cyan-500/50"
            >
              <Binary className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Primary Superblock (Blk {activeDisk.layout.primary_superblock})</span>
            </button>
            <button
              type="button"
              onClick={() =>
                onOpenInHexViewer({
                  sourceType: 'block',
                  blockId: activeDisk.layout.backup_superblock,
                })
              }
              className="inline-flex items-center gap-1 rounded-md border border-slate-700 bg-slate-800/80 px-2.5 py-1 text-xs text-cyan-300 hover:border-cyan-500/50"
            >
              <Binary className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Backup Superblock (Blk {activeDisk.layout.backup_superblock})</span>
            </button>
            <button
              type="button"
              onClick={() =>
                onOpenInHexViewer({
                  sourceType: 'block',
                  blockId: activeDisk.layout.journal_start,
                })
              }
              className="inline-flex items-center gap-1 rounded-md border border-slate-700 bg-slate-800/80 px-2.5 py-1 text-xs text-cyan-300 hover:border-cyan-500/50"
            >
              <Binary className="h-3.5 w-3.5" aria-hidden="true" />
              <span>Journal (Blk {activeDisk.layout.journal_start})</span>
            </button>
            <button
              type="button"
              onClick={() =>
                onExportReport({
                  reportType: 'disk_analysis',
                  diskName: activeDisk.disk_name,
                  title: `Disk Analysis — ${activeDisk.disk_name}`,
                })
              }
              className="inline-flex items-center gap-1 rounded-md border border-emerald-500/40 bg-emerald-500/10 px-2.5 py-1 text-xs text-emerald-300 hover:bg-emerald-500/20"
            >
              <span>Generate Disk Analysis Report</span>
            </button>
          </div>

          {activeDisk.dirty_journal_detected && (
            <div
              role="alert"
              className="mt-4 rounded-lg border border-amber-500/40 bg-amber-500/10 px-4 py-3 text-xs text-amber-200"
            >
              <p className="font-semibold">Crash Detection: Dirty Metadata Journal</p>
              <p className="mt-0.5">
                This volume has uncommitted journal entries and is write-protected to prevent further corruption.
                Read-only inspection is enabled; reformat this disk (or use Phase 6 Corruption Lab repair) before writing.
              </p>
            </div>
          )}

          <div className="mt-4 grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
            <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
              <p className="text-[11px] uppercase tracking-wider text-slate-400">Reserved &amp; Superblocks</p>
              <p className="font-mono-tabular mt-1 text-xs text-slate-200">
                Boot: Blk {activeDisk.layout.boot_block} · SB: Blk {activeDisk.layout.primary_superblock} /{' '}
                {activeDisk.layout.backup_superblock}
              </p>
            </div>
            <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
              <p className="text-[11px] uppercase tracking-wider text-slate-400">Bitmaps</p>
              <p className="font-mono-tabular mt-1 text-xs text-slate-200">
                Inode: Blk {activeDisk.layout.inode_bitmap_start} ({activeDisk.layout.inode_bitmap_blocks}) · Block:{' '}
                {activeDisk.layout.block_bitmap_start} ({activeDisk.layout.block_bitmap_blocks})
              </p>
            </div>
            <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
              <p className="text-[11px] uppercase tracking-wider text-slate-400">Inode Table &amp; Data</p>
              <p className="font-mono-tabular mt-1 text-xs text-slate-200">
                Inodes: Blk {activeDisk.layout.inode_table_start} ({activeDisk.layout.inode_table_blocks}) · Data:{' '}
                {activeDisk.layout.data_region_start} ({activeDisk.layout.data_blocks_count})
              </p>
            </div>
            <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
              <p className="text-[11px] uppercase tracking-wider text-slate-400">Metadata Journal Ring</p>
              <p className="font-mono-tabular mt-1 text-xs text-slate-200">
                Blk {activeDisk.layout.journal_start}..{activeDisk.layout.journal_start + activeDisk.layout.journal_blocks - 1}{' '}
                ({activeDisk.recent_journal.length} entries)
              </p>
            </div>
          </div>

          {activeDisk.recent_journal.length > 0 && (
            <div className="mt-4 overflow-x-auto">
              <table className="w-full border-collapse text-left text-xs">
                <caption className="sr-only">Recent metadata journal transactions</caption>
                <thead>
                  <tr className="border-b border-slate-800 text-slate-400">
                    <th scope="col" className="py-2 pr-3 font-medium">Seq</th>
                    <th scope="col" className="px-3 py-2 font-medium">Summary</th>
                    <th scope="col" className="px-3 py-2 font-medium">Status</th>
                    <th scope="col" className="px-3 py-2 font-medium">Inode</th>
                    <th scope="col" className="py-2 pl-3 font-medium">Timestamp</th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-slate-800/70">
                  {activeDisk.recent_journal.slice(0, 6).map((j) => (
                    <tr key={j.seq_no}>
                      <td className="font-mono-tabular py-2 pr-3 text-slate-300">#{j.seq_no}</td>
                      <td className="font-mono-tabular px-3 py-2 text-slate-200">{j.summary}</td>
                      <td className="px-3 py-2">
                        <span
                          className={`rounded px-1.5 py-0.5 text-[11px] font-medium ${
                            j.status === 2
                              ? 'bg-emerald-500/15 text-emerald-300'
                              : 'bg-amber-500/15 text-amber-300'
                          }`}
                        >
                          {j.status === 2 ? 'Committed' : 'Pending'}
                        </span>
                      </td>
                      <td className="font-mono-tabular px-3 py-2 text-slate-400">{j.target_inode}</td>
                      <td className="font-mono-tabular py-2 pl-3 text-slate-400">
                        {new Date(j.timestamp_ms).toLocaleTimeString()}
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </section>
      )}

      <ConfirmDialog
        open={pendingDelete !== null}
        title="Delete Virtual Disk Image?"
        description={`This permanently removes ${pendingDelete ?? ''} from disk_images/. All files and forensic blocks inside this virtual image will be lost.`}
        confirmLabel="Delete Disk"
        danger
        busy={busyAction}
        onConfirm={() => void handleConfirmDelete()}
        onCancel={() => setPendingDelete(null)}
      />

      <dialog
        ref={formatDialogRef}
        aria-labelledby={formatTitleId}
        className="m-auto w-full max-w-md rounded-xl border border-slate-700 bg-[#111827] p-6 text-slate-100 shadow-2xl"
      >
        <h1 id={formatTitleId} className="text-base font-semibold text-slate-100">
          Reformat {pendingFormat?.disk_name ?? 'Virtual Disk'}
        </h1>
        <p className="mt-1 text-xs text-amber-300">
          Formatting reinitializes GFS1 superblocks, bitmaps, and inode table, clearing active directory entries.
        </p>
        <form onSubmit={(e) => void handleConfirmFormat(e)} className="mt-4 space-y-4">
          <div>
            <label htmlFor="fmt-bs" className="block text-xs font-medium text-slate-300">
              Block Size
            </label>
            <select
              id="fmt-bs"
              value={fmtBlockSize}
              onChange={(e) => setFmtBlockSize(Number(e.target.value) as BlockSize)}
              className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100"
            >
              <option value={512}>512 bytes</option>
              <option value={1024}>1024 bytes</option>
              <option value={2048}>2048 bytes</option>
              <option value={4096}>4096 bytes</option>
            </select>
          </div>
          <div>
            <label htmlFor="fmt-label" className="block text-xs font-medium text-slate-300">
              Volume Label
            </label>
            <input
              id="fmt-label"
              type="text"
              maxLength={23}
              value={fmtLabel}
              onChange={(e) => setFmtLabel(e.target.value)}
              className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100"
            />
          </div>
          <div>
            <label htmlFor="fmt-policy" className="block text-xs font-medium text-slate-300">
              Allocation Policy
            </label>
            <select
              id="fmt-policy"
              value={fmtPolicy}
              onChange={(e) => setFmtPolicy(e.target.value as AllocPolicy)}
              className="mt-1 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100"
            >
              <option value="first_fit">First Fit</option>
              <option value="best_fit">Best Fit</option>
              <option value="worst_fit">Worst Fit</option>
              <option value="bitmap_next_fit">Bitmap Next Fit</option>
            </select>
          </div>
          <div className="flex justify-end gap-3 pt-2">
            <button
              type="button"
              autoFocus
              disabled={busyAction}
              onClick={() => setPendingFormat(null)}
              className="rounded-lg border border-slate-700 bg-slate-800 px-4 py-2 text-sm text-slate-200 hover:bg-slate-700"
            >
              Cancel
            </button>
            <button
              type="submit"
              disabled={busyAction}
              className="rounded-lg bg-amber-600 px-4 py-2 text-sm font-medium text-white hover:bg-amber-500 disabled:opacity-50"
            >
              {busyAction ? 'Formatting...' : 'Format Volume'}
            </button>
          </div>
        </form>
      </dialog>
    </div>
  )
}
