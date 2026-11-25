import { useCallback, useEffect, useId, useRef, useState, type ChangeEvent, type FormEvent } from 'react'
import {
  Binary,
  ChevronRight,
  Download,
  Edit3,
  File as FileIcon,
  FilePlus2,
  Folder,
  FolderPlus,
  HardDrive,
  RefreshCw,
  Save,
  Trash2,
  Upload,
} from 'lucide-react'
import { ConfirmDialog } from '../components/ConfirmDialog'
import {
  deleteEntry,
  getDownloadUrl,
  listDirectory,
  makeDirectory,
  readFile,
  renameEntry,
  uploadBinaryFile,
  writeTextFile,
} from '../services/api'
import type {
  FileEntryInfo,
  FileReadResponse,
  HexNavigationTarget,
  VolumeStatus,
} from '../types/api'

interface FileExplorerPageProps {
  activeDisk: VolumeStatus | null
  onUpdateVolume: (volume: VolumeStatus) => void
  onGoToDiskManager: () => void
  onOpenInHexViewer: (target: HexNavigationTarget) => void
  onNotify: (kind: 'success' | 'error', message: string) => void
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`
}

export function FileExplorerPage({
  activeDisk,
  onUpdateVolume,
  onGoToDiskManager,
  onOpenInHexViewer,
  onNotify,
}: FileExplorerPageProps) {
  const [currentPath, setCurrentPath] = useState('/')
  const [entries, setEntries] = useState<FileEntryInfo[]>([])
  const [loading, setLoading] = useState(false)
  const [selectedFile, setSelectedFile] = useState<FileReadResponse | null>(null)
  const [editorText, setEditorText] = useState('')
  const [savingFile, setSavingFile] = useState(false)

  const [showNewFolder, setShowNewFolder] = useState(false)
  const [folderName, setFolderName] = useState('')
  const [showNewFile, setShowNewFile] = useState(false)
  const [newFileName, setNewFileName] = useState('')
  const [newFileContent, setNewFileContent] = useState('')

  const [pendingRename, setPendingRename] = useState<FileEntryInfo | null>(null)
  const [renameTarget, setRenameTarget] = useState('')
  const [pendingDelete, setPendingDelete] = useState<FileEntryInfo | null>(null)
  const [recursiveDelete, setRecursiveDelete] = useState(false)
  const [busyMutation, setBusyMutation] = useState(false)

  const uploadInputRef = useRef<HTMLInputElement>(null)
  const folderDialogRef = useRef<HTMLDialogElement>(null)
  const newFileDialogRef = useRef<HTMLDialogElement>(null)
  const renameDialogRef = useRef<HTMLDialogElement>(null)
  const folderTitleId = useId()
  const newFileTitleId = useId()
  const renameTitleId = useId()

  const diskName = activeDisk?.disk_name ?? null
  const isDirty = Boolean(activeDisk?.dirty_journal_detected)

  const refreshDirectory = useCallback(
    async (targetPath: string) => {
      if (!diskName) return
      setLoading(true)
      try {
        const res = await listDirectory(diskName, targetPath)
        setEntries(res.entries)
        setCurrentPath(res.path)
        onUpdateVolume(res.volume)
      } catch (err) {
        onNotify('error', err instanceof Error ? err.message : 'Failed to list directory')
      } finally {
        setLoading(false)
      }
    },
    [diskName, onUpdateVolume, onNotify],
  )

  useEffect(() => {
    setCurrentPath('/')
    setSelectedFile(null)
    if (diskName) {
      void refreshDirectory('/')
    } else {
      setEntries([])
    }
  }, [diskName, refreshDirectory])

  // Sync native <dialog> modals
  useEffect(() => {
    const d = folderDialogRef.current
    if (!d) return
    if (showNewFolder && !d.open) d.showModal()
    else if (!showNewFolder && d.open) d.close()
  }, [showNewFolder])

  useEffect(() => {
    const d = newFileDialogRef.current
    if (!d) return
    if (showNewFile && !d.open) d.showModal()
    else if (!showNewFile && d.open) d.close()
  }, [showNewFile])

  useEffect(() => {
    const d = renameDialogRef.current
    if (!d) return
    if (pendingRename && !d.open) {
      setRenameTarget(pendingRename.path)
      d.showModal()
    } else if (!pendingRename && d.open) {
      d.close()
    }
  }, [pendingRename])

  useEffect(() => {
    const pairs: Array<[HTMLDialogElement | null, () => void]> = [
      [folderDialogRef.current, () => setShowNewFolder(false)],
      [newFileDialogRef.current, () => setShowNewFile(false)],
      [renameDialogRef.current, () => setPendingRename(null)],
    ]
    const cleanups = pairs.map(([el, closeFn]) => {
      if (!el) return () => {}
      const handler = () => {
        el.returnValue = ''
        closeFn()
      }
      el.addEventListener('close', handler)
      return () => el.removeEventListener('close', handler)
    })
    return () => cleanups.forEach((fn) => fn())
  }, [])

  if (!activeDisk) {
    return (
      <section className="rounded-xl border border-dashed border-slate-700 bg-[#111827] p-10 text-center">
        <HardDrive className="mx-auto h-10 w-10 text-slate-500" aria-hidden="true" />
        <h2 className="mt-3 text-base font-semibold text-slate-100">No Virtual Disk Mounted</h2>
        <p className="mt-1 text-sm text-slate-400">
          Create or mount a GFS1 `.gfs` volume in the Disk Manager before exploring directories and files.
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

  const breadcrumbs = currentPath === '/' ? [] : currentPath.split('/').filter(Boolean)

  async function handleInspectFile(entry: FileEntryInfo) {
    if (!diskName) return
    if (entry.type === 'directory') {
      setSelectedFile(null)
      await refreshDirectory(entry.path)
      return
    }
    try {
      const res = await readFile(diskName, entry.path)
      setSelectedFile(res)
      setEditorText(res.text_preview ?? '')
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to read file')
    }
  }

  async function handleCreateFolder(e: FormEvent) {
    e.preventDefault()
    if (!diskName || !folderName.trim()) return
    const clean = folderName.trim().replace(/^\/+|\/+$/g, '')
    const fullPath = currentPath === '/' ? `/${clean}` : `${currentPath}/${clean}`
    setBusyMutation(true)
    try {
      await makeDirectory(diskName, fullPath)
      setShowNewFolder(false)
      setFolderName('')
      await refreshDirectory(currentPath)
      onNotify('success', `Created directory ${fullPath}`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to create directory')
    } finally {
      setBusyMutation(false)
    }
  }

  async function handleCreateTextFile(e: FormEvent) {
    e.preventDefault()
    if (!diskName || !newFileName.trim()) return
    const clean = newFileName.trim().replace(/^\/+/, '')
    const fullPath = currentPath === '/' ? `/${clean}` : `${currentPath}/${clean}`
    setBusyMutation(true)
    try {
      const res = await writeTextFile(diskName, fullPath, newFileContent, false)
      setShowNewFile(false)
      setNewFileName('')
      setNewFileContent('')
      await refreshDirectory(currentPath)
      onNotify(
        'success',
        `Created ${res.entry.name} (inode #${res.entry.inode_id}, SHA-256 ${res.entry.sha256?.slice(0, 12)}...)`,
      )
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to create file')
    } finally {
      setBusyMutation(false)
    }
  }

  async function handleFileUpload(e: ChangeEvent<HTMLInputElement>) {
    const file = e.target.files?.[0]
    e.target.value = ''
    if (!file || !diskName) return
    setBusyMutation(true)
    try {
      const res = await uploadBinaryFile(diskName, file, currentPath, true)
      await refreshDirectory(currentPath)
      onNotify(
        'success',
        `Uploaded ${res.entry.name} (${formatBytes(res.entry.size_bytes)}, SHA-256 ${res.entry.sha256?.slice(0, 12)}...)`,
      )
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to upload file')
    } finally {
      setBusyMutation(false)
    }
  }

  async function handleSaveEditedText() {
    if (!diskName || !selectedFile) return
    setSavingFile(true)
    try {
      const res = await writeTextFile(diskName, selectedFile.entry.path, editorText, true)
      const refreshed = await readFile(diskName, selectedFile.entry.path)
      setSelectedFile(refreshed)
      setEditorText(refreshed.text_preview ?? '')
      await refreshDirectory(currentPath)
      onNotify('success', `Saved ${res.entry.name} (${res.entry.all_data_blocks.length} blocks)`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to save file')
    } finally {
      setSavingFile(false)
    }
  }

  async function handleConfirmRename(e: FormEvent) {
    e.preventDefault()
    if (!diskName || !pendingRename || !renameTarget.trim()) return
    setBusyMutation(true)
    try {
      const res = await renameEntry(diskName, pendingRename.path, renameTarget.trim())
      if (selectedFile?.entry.path === pendingRename.path) {
        setSelectedFile(null)
      }
      setPendingRename(null)
      await refreshDirectory(currentPath)
      onNotify('success', `Renamed to ${res.entry.path}`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to rename entry')
    } finally {
      setBusyMutation(false)
    }
  }

  async function handleConfirmDelete() {
    if (!diskName || !pendingDelete) return
    setBusyMutation(true)
    try {
      const res = await deleteEntry(
        diskName,
        pendingDelete.path,
        pendingDelete.type === 'directory' ? recursiveDelete : false,
      )
      if (selectedFile?.entry.path === pendingDelete.path) {
        setSelectedFile(null)
      }
      setPendingDelete(null)
      setRecursiveDelete(false)
      await refreshDirectory(currentPath)
      onNotify(
        'success',
        `Forensically deleted ${res.deleted_path}: released ${res.released_blocks_count} block(s) without zeroing raw bytes`,
      )
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Failed to delete entry')
    } finally {
      setBusyMutation(false)
    }
  }

  return (
    <div className="space-y-6">
      {isDirty && (
        <div
          role="alert"
          className="rounded-xl border border-amber-500/40 bg-amber-500/10 px-4 py-3 text-xs text-amber-200"
        >
          <p className="font-semibold">Write-Protected Volume (Crash Detected)</p>
          <p className="mt-0.5">
            Uncommitted journal entries were detected on {activeDisk.disk_name}. Read-only browsing and downloads remain available, while file mutations are blocked until repaired or reformatted.
          </p>
        </div>
      )}
      <div className="flex flex-wrap items-center justify-between gap-4 rounded-xl border border-slate-800 bg-[#111827] px-5 py-3.5">
        <nav aria-label="Directory breadcrumb" className="flex flex-wrap items-center gap-1 text-sm">
          <button
            type="button"
            onClick={() => void refreshDirectory('/')}
            className="font-mono-tabular rounded px-2 py-1 text-cyan-300 hover:bg-slate-800"
          >
            /
          </button>
          {breadcrumbs.map((part, idx) => {
            const target = '/' + breadcrumbs.slice(0, idx + 1).join('/')
            return (
              <span key={target} className="flex items-center gap-1">
                <ChevronRight className="h-3.5 w-3.5 text-slate-500" aria-hidden="true" />
                <button
                  type="button"
                  onClick={() => void refreshDirectory(target)}
                  className="font-mono-tabular rounded px-2 py-1 text-slate-200 hover:bg-slate-800"
                >
                  {part}
                </button>
              </span>
            )
          })}
        </nav>

        <div className="flex flex-wrap items-center gap-2">
          <button
            type="button"
            disabled={busyMutation || isDirty}
            onClick={() => setShowNewFolder(true)}
            className="inline-flex items-center gap-1.5 rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-xs font-medium text-slate-200 hover:border-cyan-500/50 disabled:opacity-40"
          >
            <FolderPlus className="h-3.5 w-3.5 text-amber-400" aria-hidden="true" />
            <span>New Folder</span>
          </button>
          <button
            type="button"
            disabled={busyMutation || isDirty}
            onClick={() => setShowNewFile(true)}
            className="inline-flex items-center gap-1.5 rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-xs font-medium text-slate-200 hover:border-cyan-500/50 disabled:opacity-40"
          >
            <FilePlus2 className="h-3.5 w-3.5 text-cyan-400" aria-hidden="true" />
            <span>New Text File</span>
          </button>
          <input
            ref={uploadInputRef}
            type="file"
            className="sr-only"
            aria-label="Upload binary or text file"
            onChange={(e) => void handleFileUpload(e)}
          />
          <button
            type="button"
            disabled={busyMutation || isDirty}
            onClick={() => uploadInputRef.current?.click()}
            className="inline-flex items-center gap-1.5 rounded-lg bg-cyan-600 px-3 py-1.5 text-xs font-medium text-white hover:bg-cyan-500 disabled:opacity-40"
          >
            <Upload className="h-3.5 w-3.5" aria-hidden="true" />
            <span>Upload File</span>
          </button>
          <button
            type="button"
            disabled={loading}
            onClick={() => void refreshDirectory(currentPath)}
            aria-label="Refresh directory listing"
            className="rounded-lg border border-slate-700 bg-slate-800 p-1.5 text-slate-300 hover:border-cyan-500/50"
          >
            <RefreshCw className={`h-4 w-4 ${loading ? 'animate-spin' : ''}`} aria-hidden="true" />
          </button>
        </div>
      </div>

      <div className="grid gap-6 xl:grid-cols-12">
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-7">
          <h2 className="text-sm font-semibold text-slate-100">
            Directory Entries ({entries.length})
          </h2>

          {entries.length === 0 ? (
            <div className="mt-4 rounded-lg border border-dashed border-slate-700 bg-[#0b0f17]/60 p-8 text-center text-xs text-slate-400">
              Directory <span className="font-mono-tabular text-slate-200">{currentPath}</span> is empty.
            </div>
          ) : (
            <div className="mt-3 overflow-x-auto">
              <table className="w-full border-collapse text-left text-xs">
                <caption className="sr-only">Files and directories in {currentPath}</caption>
                <thead>
                  <tr className="border-b border-slate-800 text-slate-400">
                    <th scope="col" className="py-2 pr-3 font-medium">Name</th>
                    <th scope="col" className="px-3 py-2 font-medium">Inode</th>
                    <th scope="col" className="px-3 py-2 font-medium">Size</th>
                    <th scope="col" className="px-3 py-2 font-medium">Blocks</th>
                    <th scope="col" className="py-2 pl-3 text-right font-medium">Actions</th>
                  </tr>
                </thead>
                <tbody className="divide-y divide-slate-800/70">
                  {entries.map((entry) => (
                    <tr
                      key={entry.path}
                      className={selectedFile?.entry.path === entry.path ? 'bg-cyan-500/5' : ''}
                    >
                      <td className="py-2.5 pr-3">
                        <button
                          type="button"
                          onClick={() => void handleInspectFile(entry)}
                          className="flex items-center gap-2 font-medium text-slate-100 hover:text-cyan-300"
                        >
                          {entry.type === 'directory' ? (
                            <Folder className="h-4 w-4 shrink-0 text-amber-400" aria-hidden="true" />
                          ) : (
                            <FileIcon className="h-4 w-4 shrink-0 text-cyan-400" aria-hidden="true" />
                          )}
                          <span className="font-mono-tabular">{entry.name}</span>
                        </button>
                      </td>
                      <td className="font-mono-tabular px-3 py-2.5 text-slate-300">
                        #{entry.inode_id} (g{entry.generation})
                      </td>
                      <td className="font-mono-tabular px-3 py-2.5 text-slate-300">
                        {entry.type === 'directory' ? '—' : formatBytes(entry.size_bytes)}
                      </td>
                      <td className="font-mono-tabular px-3 py-2.5 text-slate-400">
                        {entry.all_data_blocks.length}
                        {entry.indirect_block !== 0 ? '+ind' : ''}
                      </td>
                      <td className="py-2.5 pl-3 text-right">
                        <div className="inline-flex items-center gap-1">
                          {entry.type === 'file' && (
                            <a
                              href={getDownloadUrl(activeDisk.disk_name, entry.path)}
                              download={entry.name}
                              aria-label={`Download ${entry.name}`}
                              className="rounded border border-slate-700 bg-slate-800 p-1.5 text-slate-300 hover:border-cyan-500/50 hover:text-cyan-200"
                            >
                              <Download className="h-3.5 w-3.5" aria-hidden="true" />
                            </a>
                          )}
                          <button
                            type="button"
                            disabled={isDirty}
                            aria-label={`Rename ${entry.name}`}
                            onClick={() => setPendingRename(entry)}
                            className="rounded border border-slate-700 bg-slate-800 p-1.5 text-slate-300 hover:border-cyan-500/50 disabled:opacity-40"
                          >
                            <Edit3 className="h-3.5 w-3.5" aria-hidden="true" />
                          </button>
                          <button
                            type="button"
                            disabled={isDirty}
                            aria-label={`Delete ${entry.name}`}
                            onClick={() => {
                              setRecursiveDelete(entry.type === 'directory')
                              setPendingDelete(entry)
                            }}
                            className="rounded border border-rose-500/30 bg-rose-500/10 p-1.5 text-rose-300 hover:bg-rose-500/20 disabled:opacity-40"
                          >
                            <Trash2 className="h-3.5 w-3.5" aria-hidden="true" />
                          </button>
                        </div>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </section>

        <section className="rounded-xl border border-slate-800 bg-[#111827] p-5 xl:col-span-5">
          <div className="flex items-center justify-between gap-2">
            <h2 className="text-sm font-semibold text-slate-100">File Metadata &amp; Block Inspector</h2>
            {selectedFile && (
              <button
                type="button"
                onClick={() =>
                  onOpenInHexViewer({
                    sourceType: 'file',
                    filePath: selectedFile.entry.path,
                  })
                }
                className="inline-flex items-center gap-1 rounded-md border border-slate-700 bg-slate-800/80 px-2.5 py-1 text-xs text-cyan-300 hover:border-cyan-500/50"
              >
                <Binary className="h-3.5 w-3.5" aria-hidden="true" />
                <span>Inspect in Hex Viewer</span>
              </button>
            )}
          </div>
          {selectedFile ? (
            <div className="mt-4 space-y-4">
              <dl className="grid grid-cols-2 gap-3 rounded-lg border border-slate-800 bg-[#0b0f17] p-3 text-xs">
                <div>
                  <dt className="text-slate-400">Path</dt>
                  <dd className="font-mono-tabular break-all text-slate-100">{selectedFile.entry.path}</dd>
                </div>
                <div>
                  <dt className="text-slate-400">Inode / Gen</dt>
                  <dd className="font-mono-tabular text-slate-100">
                    #{selectedFile.entry.inode_id} · gen {selectedFile.entry.generation}
                  </dd>
                </div>
                <div>
                  <dt className="text-slate-400">Size / CRC32</dt>
                  <dd className="font-mono-tabular text-slate-100">
                    {selectedFile.entry.size_bytes} B · 0x
                    {selectedFile.entry.content_crc32.toString(16).toUpperCase().padStart(8, '0')}
                  </dd>
                </div>
                <div>
                  <dt className="text-slate-400">SHA-256</dt>
                  <dd className="font-mono-tabular break-all text-[11px] text-cyan-300">
                    {selectedFile.entry.sha256}
                  </dd>
                </div>
                <div className="col-span-2">
                  <dt className="text-slate-400">Block Mapping</dt>
                  <dd className="font-mono-tabular mt-1 text-slate-200">
                    Data LBAs: [{selectedFile.entry.all_data_blocks.join(', ') || 'none'}]
                    {selectedFile.entry.indirect_block !== 0 &&
                      ` · Single Indirect: ${selectedFile.entry.indirect_block}`}
                    {selectedFile.entry.double_indirect_block !== 0 &&
                      ` · Double Indirect: ${selectedFile.entry.double_indirect_block}`}
                  </dd>
                </div>
              </dl>

              {selectedFile.is_text ? (
                <div>
                  <label htmlFor="file-editor" className="block text-xs font-medium text-slate-300">
                    UTF-8 File Contents
                  </label>
                  <textarea
                    id="file-editor"
                    rows={9}
                    value={editorText}
                    onChange={(e) => setEditorText(e.target.value)}
                    className="font-mono-tabular mt-1.5 w-full rounded-lg border border-slate-700 bg-[#0b0f17] p-3 text-xs text-slate-100"
                  />
                  <div className="mt-3 flex items-center justify-end gap-2">
                    <a
                      href={getDownloadUrl(activeDisk.disk_name, selectedFile.entry.path)}
                      download={selectedFile.entry.name}
                      className="inline-flex items-center gap-1.5 rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-xs text-slate-200 hover:border-cyan-500/50"
                    >
                      <Download className="h-3.5 w-3.5" aria-hidden="true" />
                      <span>Download</span>
                    </a>
                    <button
                      type="button"
                      disabled={savingFile || isDirty}
                      onClick={() => void handleSaveEditedText()}
                      className="inline-flex items-center gap-1.5 rounded-lg bg-cyan-600 px-3 py-1.5 text-xs font-medium text-white hover:bg-cyan-500 disabled:opacity-40"
                    >
                      <Save className="h-3.5 w-3.5" aria-hidden="true" />
                      <span>{savingFile ? 'Saving...' : 'Save Changes'}</span>
                    </button>
                  </div>
                </div>
              ) : (
                <div className="rounded-lg border border-slate-800 bg-[#0b0f17] p-4 text-xs text-slate-300">
                  <p>Binary file ({formatBytes(selectedFile.entry.size_bytes)}). SHA-256 verified by C++ engine.</p>
                  <a
                    href={getDownloadUrl(activeDisk.disk_name, selectedFile.entry.path)}
                    download={selectedFile.entry.name}
                    className="mt-3 inline-flex items-center gap-1.5 rounded-lg bg-cyan-600 px-3 py-1.5 text-xs font-medium text-white hover:bg-cyan-500"
                  >
                    <Download className="h-3.5 w-3.5" aria-hidden="true" />
                    <span>Download Binary File</span>
                  </a>
                </div>
              )}
            </div>
          ) : (
            <p className="mt-4 text-xs text-slate-400">
              Select a file from the directory listing to inspect its inode metadata, block pointers, SHA-256 digest, or edit text contents.
            </p>
          )}
        </section>
      </div>

      {/* New Folder Dialog */}
      <dialog
        ref={folderDialogRef}
        aria-labelledby={folderTitleId}
        className="m-auto w-full max-w-sm rounded-xl border border-slate-700 bg-[#111827] p-6 text-slate-100 shadow-2xl"
      >
        <h1 id={folderTitleId} className="text-base font-semibold">Create Directory</h1>
        <form onSubmit={(e) => void handleCreateFolder(e)} className="mt-4 space-y-4">
          <div>
            <label htmlFor="new-folder-name" className="block text-xs font-medium text-slate-300">
              Folder Name (inside {currentPath})
            </label>
            <input
              id="new-folder-name"
              type="text"
              required
              autoFocus
              maxLength={55}
              value={folderName}
              onChange={(e) => setFolderName(e.target.value)}
              className="mt-1.5 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100"
            />
          </div>
          <div className="flex justify-end gap-2">
            <button
              type="button"
              onClick={() => setShowNewFolder(false)}
              className="rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-sm text-slate-200"
            >
              Cancel
            </button>
            <button
              type="submit"
              disabled={busyMutation}
              className="rounded-lg bg-cyan-600 px-3 py-1.5 text-sm font-medium text-white hover:bg-cyan-500"
            >
              Create Folder
            </button>
          </div>
        </form>
      </dialog>

      {/* New Text File Dialog */}
      <dialog
        ref={newFileDialogRef}
        aria-labelledby={newFileTitleId}
        className="m-auto w-full max-w-md rounded-xl border border-slate-700 bg-[#111827] p-6 text-slate-100 shadow-2xl"
      >
        <h1 id={newFileTitleId} className="text-base font-semibold">Create Text File</h1>
        <form onSubmit={(e) => void handleCreateTextFile(e)} className="mt-4 space-y-4">
          <div>
            <label htmlFor="new-file-name" className="block text-xs font-medium text-slate-300">
              File Name (inside {currentPath})
            </label>
            <input
              id="new-file-name"
              type="text"
              required
              autoFocus
              maxLength={55}
              value={newFileName}
              onChange={(e) => setNewFileName(e.target.value)}
              placeholder="evidence_notes.txt"
              className="mt-1.5 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100"
            />
          </div>
          <div>
            <label htmlFor="new-file-body" className="block text-xs font-medium text-slate-300">
              Contents
            </label>
            <textarea
              id="new-file-body"
              rows={6}
              value={newFileContent}
              onChange={(e) => setNewFileContent(e.target.value)}
              className="font-mono-tabular mt-1.5 w-full rounded-lg border border-slate-700 bg-[#0b0f17] p-3 text-xs text-slate-100"
            />
          </div>
          <div className="flex justify-end gap-2">
            <button
              type="button"
              onClick={() => setShowNewFile(false)}
              className="rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-sm text-slate-200"
            >
              Cancel
            </button>
            <button
              type="submit"
              disabled={busyMutation}
              className="rounded-lg bg-cyan-600 px-3 py-1.5 text-sm font-medium text-white hover:bg-cyan-500"
            >
              Write File
            </button>
          </div>
        </form>
      </dialog>

      {/* Rename Dialog */}
      <dialog
        ref={renameDialogRef}
        aria-labelledby={renameTitleId}
        className="m-auto w-full max-w-md rounded-xl border border-slate-700 bg-[#111827] p-6 text-slate-100 shadow-2xl"
      >
        <h1 id={renameTitleId} className="text-base font-semibold">
          Rename / Move {pendingRename?.name ?? ''}
        </h1>
        <form onSubmit={(e) => void handleConfirmRename(e)} className="mt-4 space-y-4">
          <div>
            <label htmlFor="rename-target" className="block text-xs font-medium text-slate-300">
              Destination GFS Path
            </label>
            <input
              id="rename-target"
              type="text"
              required
              autoFocus
              value={renameTarget}
              onChange={(e) => setRenameTarget(e.target.value)}
              className="font-mono-tabular mt-1.5 w-full rounded-lg border border-slate-700 bg-[#0b0f17] px-3 py-2 text-sm text-slate-100"
            />
          </div>
          <div className="flex justify-end gap-2">
            <button
              type="button"
              onClick={() => setPendingRename(null)}
              className="rounded-lg border border-slate-700 bg-slate-800 px-3 py-1.5 text-sm text-slate-200"
            >
              Cancel
            </button>
            <button
              type="submit"
              disabled={busyMutation}
              className="rounded-lg bg-cyan-600 px-3 py-1.5 text-sm font-medium text-white hover:bg-cyan-500"
            >
              Apply Rename
            </button>
          </div>
        </form>
      </dialog>

      {/* Forensic Delete Dialog */}
      <ConfirmDialog
        open={pendingDelete !== null}
        title={`Delete ${pendingDelete?.name ?? ''}?`}
        description={
          pendingDelete?.type === 'directory'
            ? `Forensically deletes directory ${pendingDelete.path} and all nested entries, releasing blocks without zeroing raw payload data.`
            : `Forensically deletes ${pendingDelete?.path ?? ''}. Its inode and directory entry are marked Deleted and blocks are marked free without erasing raw bytes.`
        }
        confirmLabel="Confirm Forensic Delete"
        danger
        busy={busyMutation}
        onConfirm={() => void handleConfirmDelete()}
        onCancel={() => setPendingDelete(null)}
      />
    </div>
  )
}
