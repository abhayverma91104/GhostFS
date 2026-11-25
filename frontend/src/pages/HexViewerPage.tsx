import { useCallback, useEffect, useState, type FormEvent } from 'react'
import {
  Activity,
  Binary,
  Check,
  ChevronLeft,
  ChevronRight,
  Copy,
  GitCompare,
  HardDrive,
  Layers,
  Lock,
  Search,
} from 'lucide-react'
import {
  Bar,
  BarChart,
  CartesianGrid,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from 'recharts'
import {
  analyzeHexRange,
  compareHexSources,
  decodeHexStructure,
  interpretHexBytes,
  readHexArtifact,
  readHexBlock,
  readHexFile,
  readHexRange,
  searchHex,
} from '../services/api'
import type {
  ByteAnalysisMetrics,
  ByteInterpretation,
  DecodedField,
  DecodedStructure,
  HexCompareResult,
  HexNavigationTarget,
  HexReadResult,
  HexSearchResult,
  ReportNavigationContext,
  VolumeStatus,
} from '../types/api'

interface HexViewerPageProps {
  activeDisk: VolumeStatus | null
  navigationTarget: HexNavigationTarget | null
  onGoToDiskManager: () => void
  onExportReport: (ctx: ReportNavigationContext) => void
  onNotify: (kind: 'success' | 'error', message: string) => void
}

function formatOffset(offset: number, hexMode: boolean): string {
  if (hexMode) {
    return offset.toString(16).toUpperCase().padStart(8, '0')
  }
  return String(offset).padStart(10, '0')
}

export function HexViewerPage({
  activeDisk,
  navigationTarget,
  onGoToDiskManager,
  onExportReport,
  onNotify,
}: HexViewerPageProps) {
  const [sourceType, setSourceType] = useState<'block' | 'disk_range' | 'file' | 'artifact'>('block')
  const [blockId, setBlockId] = useState<number>(1)
  const [rangeOffsetInput, setRangeOffsetInput] = useState<string>('0')
  const [filePathInput, setFilePathInput] = useState<string>('/')
  const [artifactInput, setArtifactInput] = useState<string>('')
  const [bytesPerRow, setBytesPerRow] = useState<8 | 16 | 32>(16)
  const [hexOffsets, setHexOffsets] = useState<boolean>(true)
  const [loading, setLoading] = useState<boolean>(false)

  const [hexData, setHexData] = useState<HexReadResult | null>(null)
  const [selStart, setSelStart] = useState<number | null>(null)
  const [selEnd, setSelEnd] = useState<number | null>(null)
  const [copiedBytes, setCopiedBytes] = useState<boolean>(false)

  const [rightTab, setRightTab] = useState<'inspector' | 'structure' | 'search' | 'analysis' | 'compare'>('inspector')
  const [endianness, setEndianness] = useState<'le' | 'be'>('le')
  const [interpretation, setInterpretation] = useState<ByteInterpretation | null>(null)
  const [structure, setStructure] = useState<DecodedStructure | null>(null)

  // Search state
  const [searchType, setSearchType] = useState<'hex' | 'ascii' | 'utf8' | 'signature'>('ascii')
  const [searchValue, setSearchValue] = useState<string>('GFS1')
  const [searchScope, setSearchScope] = useState<'disk' | 'block_range' | 'allocated' | 'unallocated' | 'file' | 'metadata'>('disk')
  const [searchRes, setSearchRes] = useState<HexSearchResult | null>(null)
  const [searching, setSearching] = useState<boolean>(false)
  const [activeMatchIdx, setActiveMatchIdx] = useState<number>(0)

  // Analysis state
  const [analysis, setAnalysis] = useState<ByteAnalysisMetrics | null>(null)

  // Compare state
  const [cmpMode, setCmpMode] = useState<'blocks' | 'ranges' | 'files' | 'primary_vs_backup_sb' | 'file_vs_artifact'>('primary_vs_backup_sb')
  const [cmpA, setCmpA] = useState<string>('1')
  const [cmpB, setCmpB] = useState<string>('2')
  const [cmpResult, setCmpResult] = useState<HexCompareResult | null>(null)

  const diskName = activeDisk?.disk_name ?? null

  const loadView = useCallback(
    async (
      targetType: 'block' | 'disk_range' | 'file' | 'artifact',
      opts: { blockId?: number; offset?: number; length?: number; path?: string; artifact?: string; bpr?: number } = {},
    ) => {
      if (!diskName) return
      setLoading(true)
      const bpr = opts.bpr ?? bytesPerRow
      try {
        let res: HexReadResult
        if (targetType === 'block') {
          const bId = opts.blockId ?? blockId
          res = await readHexBlock(diskName, bId, bpr, 1)
          const st = await decodeHexStructure(diskName, bId).catch(() => null)
          setStructure(st)
        } else if (targetType === 'disk_range') {
          const off = opts.offset ?? (Number.parseInt(rangeOffsetInput, 10) || 0)
          res = await readHexRange(diskName, off, opts.length ?? 4096, bpr)
          const st = await decodeHexStructure(diskName, res.start_block).catch(() => null)
          setStructure(st)
        } else if (targetType === 'file') {
          const p = opts.path ?? filePathInput
          res = await readHexFile(diskName, p, opts.offset ?? 0, opts.length ?? 4096, bpr)
          setStructure(null)
        } else {
          const art = opts.artifact ?? artifactInput
          res = await readHexArtifact(diskName, art, opts.offset ?? 0, opts.length ?? 4096, bpr)
          setStructure(null)
        }
        setHexData(res)
        setSelStart(null)
        setSelEnd(null)
        setInterpretation(null)
      } catch (err) {
        onNotify('error', err instanceof Error ? err.message : 'Failed to load hex data')
      } finally {
        setLoading(false)
      }
    },
    [diskName, bytesPerRow, blockId, rangeOffsetInput, filePathInput, artifactInput, onNotify],
  )

  useEffect(() => {
    if (!diskName) {
      setHexData(null)
      return
    }
    if (navigationTarget) {
      setSourceType(navigationTarget.sourceType)
      if (navigationTarget.sourceType === 'block' && navigationTarget.blockId !== undefined) {
        setBlockId(navigationTarget.blockId)
        void loadView('block', { blockId: navigationTarget.blockId })
      } else if (navigationTarget.sourceType === 'disk_range') {
        setRangeOffsetInput(String(navigationTarget.offset ?? 0))
        void loadView('disk_range', { offset: navigationTarget.offset ?? 0, length: navigationTarget.length ?? 4096 })
      } else if (navigationTarget.sourceType === 'file' && navigationTarget.filePath) {
        setFilePathInput(navigationTarget.filePath)
        void loadView('file', { path: navigationTarget.filePath, offset: navigationTarget.offset ?? 0 })
      } else if (navigationTarget.sourceType === 'artifact' && navigationTarget.artifactFilename) {
        setArtifactInput(navigationTarget.artifactFilename)
        void loadView('artifact', { artifact: navigationTarget.artifactFilename })
      }
    } else {
      void loadView('block', { blockId: 1 })
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [diskName, navigationTarget])

  // Interpret selected bytes whenever selection changes
  useEffect(() => {
    if (!diskName || !hexData || selStart === null) return
    const lo = Math.min(selStart, selEnd ?? selStart)
    const hi = Math.min(lo + 63, Math.max(selStart, selEnd ?? selStart))
    const selected: number[] = []
    for (const row of hexData.rows) {
      for (let i = 0; i < row.raw_bytes.length; i += 1) {
        const off = row.abs_offset + i
        if (off >= lo && off <= hi) {
          selected.push(row.raw_bytes[i])
        }
      }
    }
    if (selected.length === 0) return
    void interpretHexBytes(diskName, selected)
      .then((res) => setInterpretation(res))
      .catch(() => {})
  }, [diskName, hexData, selStart, selEnd])

  if (!activeDisk) {
    return (
      <section className="rounded-xl border border-dashed border-slate-700 bg-[#111827] p-10 text-center">
        <HardDrive className="mx-auto h-10 w-10 text-slate-500" aria-hidden="true" />
        <h2 className="mt-3 text-base font-semibold text-slate-100">No Virtual Disk Mounted</h2>
        <p className="mt-1 text-sm text-slate-400">
          Mount a `.gfs` disk in Disk Manager to inspect raw sectors, GFS1 structures, and entropy.
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

  function handleSelectByte(absOffset: number, shiftKey: boolean) {
    if (shiftKey && selStart !== null) {
      setSelEnd(absOffset)
    } else {
      setSelStart(absOffset)
      setSelEnd(absOffset)
    }
  }

  function handleHighlightField(field: DecodedField) {
    setSelStart(field.abs_offset)
    setSelEnd(field.abs_offset + Math.max(0, field.length - 1))
  }

  function handleGoToOffset(e: FormEvent) {
    e.preventDefault()
    const raw = rangeOffsetInput.trim()
    const parsed = raw.startsWith('0x') || raw.startsWith('0X')
      ? Number.parseInt(raw.slice(2), 16)
      : Number.parseInt(raw, 10)
    if (Number.isNaN(parsed) || parsed < 0) {
      onNotify('error', 'Enter a valid decimal or 0x-prefixed hex byte offset')
      return
    }
    setSourceType('disk_range')
    void loadView('disk_range', { offset: parsed, length: activeDisk?.block_size ?? 4096 })
  }

  async function handleRunSearch(e: FormEvent) {
    e.preventDefault()
    if (!diskName) return
    setSearching(true)
    try {
      const res = await searchHex(diskName, {
        query_type: searchType,
        query_value: searchValue,
        scope: searchScope,
        path: searchScope === 'file' ? filePathInput : '',
      })
      setSearchRes(res)
      setActiveMatchIdx(0)
      if (res.matches.length > 0) {
        const m = res.matches[0]
        setSourceType('block')
        setBlockId(m.block_id)
        await loadView('block', { blockId: m.block_id })
        setSelStart(m.abs_offset)
        setSelEnd(m.abs_offset + m.match_length - 1)
      }
      onNotify('success', `Search complete: ${res.total_matches} match(es) across ${res.bytes_scanned} bytes`)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Binary search failed')
    } finally {
      setSearching(false)
    }
  }

  async function handleAnalyzeCurrent() {
    if (!diskName || !hexData) return
    try {
      const len = Math.max(1, Math.min(65536, hexData.length))
      const m = await analyzeHexRange(diskName, hexData.start_offset, len)
      setAnalysis(m)
      setRightTab('analysis')
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Analysis failed')
    }
  }

  async function handleRunCompare(e: FormEvent) {
    e.preventDefault()
    if (!diskName) return
    try {
      const payload =
        cmpMode === 'primary_vs_backup_sb'
          ? { mode: cmpMode }
          : cmpMode === 'blocks'
            ? { mode: cmpMode, a_start: Number(cmpA) || 0, b_start: Number(cmpB) || 0 }
            : cmpMode === 'files'
              ? { mode: cmpMode, path_a: cmpA, path_b: cmpB }
              : cmpMode === 'file_vs_artifact'
                ? { mode: cmpMode, path_a: cmpA, artifact_filename: cmpB }
                : { mode: cmpMode, a_start: Number(cmpA) || 0, a_len: 512, b_start: Number(cmpB) || 0, b_len: 512 }
      const res = await compareHexSources(diskName, payload)
      setCmpResult(res)
    } catch (err) {
      onNotify('error', err instanceof Error ? err.message : 'Comparison failed')
    }
  }

  const selLo = selStart !== null ? Math.min(selStart, selEnd ?? selStart) : null
  const selHi = selStart !== null ? Math.max(selStart, selEnd ?? selStart) : null

  const entropyChartData = analysis
    ? Array.from({ length: 16 }, (_, bucket) => {
        let count = 0
        for (let i = bucket * 16; i < bucket * 16 + 16; i += 1) {
          count += analysis.histogram[i] ?? 0
        }
        return {
          range: `${(bucket * 16).toString(16).toUpperCase().padStart(2, '0')}-${(bucket * 16 + 15).toString(16).toUpperCase().padStart(2, '0')}`,
          count,
        }
      })
    : []

  return (
    <div className="space-y-5">
      {/* A. Source Toolbar */}
      <section className="rounded-xl border border-slate-800 bg-[#111827] p-4">
        <div className="flex flex-wrap items-center justify-between gap-3 border-b border-slate-800 pb-3">
          <div className="flex items-center gap-2.5">
            <Binary className="h-5 w-5 text-cyan-400" aria-hidden="true" />
            <h2 className="text-base font-semibold text-slate-100">GhostFS — Hex Inspector</h2>
          </div>
          <div className="flex items-center gap-2">
            {activeDisk && (
              <button
                type="button"
                onClick={() =>
                  onExportReport({
                    reportType: 'binary_analysis',
                    diskName: activeDisk.disk_name,
                    title: `Binary Analysis — ${activeDisk.disk_name}`,
                    binaryPayload: {
                      source_type: sourceType,
                      block_id: blockId,
                      offset: Number(rangeOffsetInput) || 0,
                      path: filePathInput,
                      artifact_filename: artifactInput,
                    },
                  })
                }
                className="rounded-md border border-emerald-500/40 bg-emerald-500/10 px-2.5 py-1 text-xs text-emerald-300 hover:bg-emerald-500/20"
              >
                Export Binary Analysis
              </button>
            )}
            <span className="inline-flex items-center gap-1.5 rounded-full border border-emerald-500/40 bg-emerald-500/10 px-2.5 py-0.5 text-xs font-medium text-emerald-300">
              <Lock className="h-3 w-3" aria-hidden="true" />
              <span>Read-only</span>
            </span>
          </div>
        </div>

        <div className="mt-3 flex flex-wrap items-end gap-3 text-xs">
          <div>
            <label htmlFor="hex-source-type" className="block text-slate-400">Source</label>
            <select
              id="hex-source-type"
              value={sourceType}
              onChange={(e) => setSourceType(e.target.value as 'block' | 'disk_range' | 'file' | 'artifact')}
              className="mt-1 rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
            >
              <option value="block">Physical Block</option>
              <option value="disk_range">Byte Range</option>
              <option value="file">Logical File</option>
              <option value="artifact">Recovered Artifact</option>
            </select>
          </div>

          {sourceType === 'block' && (
            <div className="flex items-end gap-1.5">
              <button
                type="button"
                aria-label="Previous block"
                disabled={blockId <= 0 || loading}
                onClick={() => {
                  const next = Math.max(0, blockId - 1)
                  setBlockId(next)
                  void loadView('block', { blockId: next })
                }}
                className="rounded-lg border border-slate-700 bg-slate-800 p-1.5 text-slate-200 disabled:opacity-40"
              >
                <ChevronLeft className="h-4 w-4" aria-hidden="true" />
              </button>
              <div>
                <label htmlFor="hex-block-id" className="block text-slate-400">Block #</label>
                <input
                  id="hex-block-id"
                  type="number"
                  min={0}
                  max={Math.max(0, activeDisk.total_blocks - 1)}
                  value={blockId}
                  onChange={(e) => setBlockId(Number(e.target.value))}
                  className="font-mono-tabular mt-1 w-24 rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
                />
              </div>
              <button
                type="button"
                aria-label="Next block"
                disabled={blockId >= activeDisk.total_blocks - 1 || loading}
                onClick={() => {
                  const next = Math.min(activeDisk.total_blocks - 1, blockId + 1)
                  setBlockId(next)
                  void loadView('block', { blockId: next })
                }}
                className="rounded-lg border border-slate-700 bg-slate-800 p-1.5 text-slate-200 disabled:opacity-40"
              >
                <ChevronRight className="h-4 w-4" aria-hidden="true" />
              </button>
              <button
                type="button"
                onClick={() => void loadView('block', { blockId })}
                className="rounded-lg bg-cyan-600 px-3 py-1.5 font-medium text-white hover:bg-cyan-500"
              >
                Load Block
              </button>
            </div>
          )}

          {sourceType === 'file' && (
            <div className="flex items-end gap-2">
              <div>
                <label htmlFor="hex-file-path" className="block text-slate-400">GFS File Path</label>
                <input
                  id="hex-file-path"
                  type="text"
                  value={filePathInput}
                  onChange={(e) => setFilePathInput(e.target.value)}
                  className="font-mono-tabular mt-1 w-52 rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
                />
              </div>
              <button
                type="button"
                onClick={() => void loadView('file', { path: filePathInput })}
                className="rounded-lg bg-cyan-600 px-3 py-1.5 font-medium text-white hover:bg-cyan-500"
              >
                Read Logical File
              </button>
            </div>
          )}

          {sourceType === 'artifact' && (
            <div className="flex items-end gap-2">
              <div>
                <label htmlFor="hex-art-name" className="block text-slate-400">Artifact Filename</label>
                <input
                  id="hex-art-name"
                  type="text"
                  value={artifactInput}
                  onChange={(e) => setArtifactInput(e.target.value)}
                  placeholder="rec_..._file.bin"
                  className="font-mono-tabular mt-1 w-52 rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
                />
              </div>
              <button
                type="button"
                onClick={() => void loadView('artifact', { artifact: artifactInput })}
                className="rounded-lg bg-cyan-600 px-3 py-1.5 font-medium text-white hover:bg-cyan-500"
              >
                Read Artifact
              </button>
            </div>
          )}

          <form onSubmit={handleGoToOffset} className="flex items-end gap-1.5">
            <div>
              <label htmlFor="hex-goto-offset" className="block text-slate-400">Go To Offset</label>
              <input
                id="hex-goto-offset"
                type="text"
                value={rangeOffsetInput}
                onChange={(e) => setRangeOffsetInput(e.target.value)}
                placeholder="0x1000 or 4096"
                className="font-mono-tabular mt-1 w-32 rounded-lg border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
              />
            </div>
            <button
              type="submit"
              className="rounded-lg border border-slate-700 bg-slate-800 px-2.5 py-1.5 text-slate-200 hover:border-cyan-500/50"
            >
              Go
            </button>
          </form>

          <div>
            <label htmlFor="hex-bpr" className="block text-slate-400">Bytes / Row</label>
            <select
              id="hex-bpr"
              value={bytesPerRow}
              onChange={(e) => {
                const next = Number(e.target.value) as 8 | 16 | 32
                setBytesPerRow(next)
                void loadView(sourceType, { bpr: next })
              }}
              className="mt-1 rounded-lg border border-slate-700 bg-[#0b0f17] px-2 py-1.5 text-slate-100"
            >
              <option value={8}>8 bytes</option>
              <option value={16}>16 bytes</option>
              <option value={32}>32 bytes</option>
            </select>
          </div>

          <button
            type="button"
            onClick={() => setHexOffsets((v) => !v)}
            className="rounded-lg border border-slate-700 bg-slate-800 px-2.5 py-1.5 text-slate-200 hover:border-cyan-500/50"
          >
            Offsets: {hexOffsets ? 'HEX' : 'DEC'}
          </button>

          <button
            type="button"
            onClick={() => void handleAnalyzeCurrent()}
            className="inline-flex items-center gap-1.5 rounded-lg border border-cyan-500/40 bg-cyan-500/10 px-3 py-1.5 font-medium text-cyan-200 hover:bg-cyan-500/20"
          >
            <Activity className="h-3.5 w-3.5" aria-hidden="true" />
            <span>Analyze Entropy</span>
          </button>
        </div>

        {/* 2x2 Summary Metadata Row (Matching Reference Mockup) */}
        {hexData && (
          <div className="mt-4 grid gap-3 border-t border-slate-800 pt-3 sm:grid-cols-2 lg:grid-cols-4">
            <div>
              <p className="text-xs text-slate-400">Selected Block / Source</p>
              <p className="font-mono-tabular mt-0.5 text-sm font-semibold text-slate-100">
                {hexData.source_type === 'file'
                  ? `${hexData.owner_path} (Blk ${hexData.start_block})`
                  : hexData.source_type === 'artifact'
                    ? hexData.disk_name
                    : `Block ${hexData.start_block}`}
              </p>
            </div>
            <div>
              <p className="text-xs text-slate-400">Window / Block Size</p>
              <p className="font-mono-tabular mt-0.5 text-sm font-semibold text-slate-100">
                {hexData.length} B {hexData.block_size > 0 ? `(${hexData.block_size}B blk)` : ''}
              </p>
            </div>
            <div>
              <p className="text-xs text-slate-400">Byte Offset</p>
              <p className="font-mono-tabular mt-0.5 text-sm font-semibold text-slate-100">
                0x{hexData.start_offset.toString(16).toUpperCase().padStart(8, '0')}
              </p>
            </div>
            <div>
              <p className="text-xs text-slate-400">Detected Signature / Region</p>
              <p className="font-mono-tabular mt-0.5 text-sm font-semibold text-cyan-300">
                {hexData.signature.detected
                  ? `${hexData.signature.file_type.toUpperCase()} (${hexData.signature.validation_note})`
                  : hexData.classification}
              </p>
            </div>
          </div>
        )}
      </section>

      <div className="grid gap-6 xl:grid-cols-12">
        {/* B. Main Hexadecimal Grid */}
        <section className="rounded-xl border border-slate-800 bg-[#0b0f17] p-4 xl:col-span-7">
          {hexData && hexData.rows.length > 0 ? (
            <>
              <div className="max-h-[520px] overflow-auto font-mono-tabular text-xs">
                <div className="mb-2 flex border-b border-slate-800 pb-1.5 text-[11px] font-semibold uppercase tracking-wider text-slate-400">
                  <span className="w-24 shrink-0">Offset</span>
                  <span className="flex-1">Hex Bytes</span>
                  <span className="w-40 shrink-0 pl-3">ASCII</span>
                </div>
                <div className="space-y-0.5">
                  {hexData.rows.map((row) => (
                    <div key={`${row.abs_offset}-${row.logical_offset}`} className="flex items-center py-0.5 hover:bg-slate-900/70">
                      <span className="w-24 shrink-0 select-none text-slate-500">
                        {formatOffset(
                          hexData.source_type === 'file' ? row.logical_offset : row.abs_offset,
                          hexOffsets,
                        )}
                      </span>
                      <div className="flex flex-1 flex-wrap gap-1">
                        {row.hex_bytes.map((hx, idx) => {
                          const byteOff = row.abs_offset + idx
                          const isSelected = selLo !== null && selHi !== null && byteOff >= selLo && byteOff <= selHi
                          return (
                            <button
                              key={idx}
                              type="button"
                              onClick={(e) => handleSelectByte(byteOff, e.shiftKey)}
                              className={`rounded px-1 py-0.5 transition-colors ${
                                isSelected
                                  ? 'bg-cyan-600/80 font-semibold text-white'
                                  : 'text-slate-200 hover:bg-slate-800'
                              }`}
                            >
                              {hx}
                            </button>
                          )
                        })}
                      </div>
                      <div className="w-40 shrink-0 pl-3 tracking-wider">
                        {row.ascii.split('').map((ch, idx) => {
                          const byteOff = row.abs_offset + idx
                          const isSelected = selLo !== null && selHi !== null && byteOff >= selLo && byteOff <= selHi
                          return (
                            <button
                              key={idx}
                              type="button"
                              onClick={(e) => handleSelectByte(byteOff, e.shiftKey)}
                              className={
                                isSelected
                                  ? 'rounded bg-cyan-600/80 font-semibold text-white'
                                  : 'text-cyan-300/90 hover:bg-slate-800'
                              }
                            >
                              {ch}
                            </button>
                          )
                        })}
                      </div>
                    </div>
                  ))}
                </div>
              </div>

              {/* Selected Bytes Bar */}
              <div className="mt-3 flex flex-wrap items-center justify-between gap-3 rounded-lg border border-slate-800 bg-[#111827] px-4 py-2.5 text-xs">
                <div>
                  {selLo !== null && selHi !== null && interpretation ? (
                    <>
                      <p className="text-slate-400">
                        Selected bytes · 0x{selLo.toString(16).toUpperCase().padStart(8, '0')}–0x
                        {selHi.toString(16).toUpperCase().padStart(8, '0')} ({interpretation.byte_count} bytes)
                      </p>
                      <p className="font-mono-tabular mt-0.5 font-semibold text-slate-100">
                        {interpretation.hex}
                      </p>
                    </>
                  ) : (
                    <p className="text-slate-400">
                      Click any byte (or Shift+Click for range) to inspect integer, float, and UTF-8 values.
                    </p>
                  )}
                </div>
                {interpretation && (
                  <button
                    type="button"
                    onClick={() => {
                      void navigator.clipboard?.writeText(interpretation.hex)
                      setCopiedBytes(true)
                      setTimeout(() => setCopiedBytes(false), 1500)
                    }}
                    className="inline-flex items-center gap-1 rounded border border-slate-700 bg-slate-800 px-2.5 py-1 text-slate-200 hover:border-cyan-500/50"
                  >
                    {copiedBytes ? <Check className="h-3.5 w-3.5 text-emerald-400" /> : <Copy className="h-3.5 w-3.5" />}
                    <span>{copiedBytes ? 'Copied' : 'Copy Hex'}</span>
                  </button>
                )}
              </div>
            </>
          ) : (
            <p className="py-12 text-center text-xs text-slate-400">
              {loading ? 'Reading binary bytes...' : 'No bytes loaded.'}
            </p>
          )}
        </section>

        {/* Right Forensic Panels: Inspector / GFS1 Structure / Search / Analysis / Compare */}
        <section className="rounded-xl border border-slate-800 bg-[#111827] p-4 xl:col-span-5">
          <div role="tablist" aria-label="Hex analysis tools" className="flex flex-wrap gap-1.5 border-b border-slate-800 pb-3 text-xs">
            {(
              [
                ['inspector', 'Data Inspector', Binary],
                ['structure', 'GFS1 Structure', Layers],
                ['search', 'Search', Search],
                ['analysis', 'Entropy', Activity],
                ['compare', 'Compare', GitCompare],
              ] as const
            ).map(([id, label, Icon]) => (
              <button
                key={id}
                type="button"
                role="tab"
                aria-selected={rightTab === id}
                onClick={() => setRightTab(id)}
                className={`inline-flex items-center gap-1 rounded-md px-2.5 py-1.5 font-medium ${
                  rightTab === id
                    ? 'border border-cyan-500/40 bg-cyan-500/15 text-cyan-200'
                    : 'text-slate-300 hover:bg-slate-800'
                }`}
              >
                <Icon className="h-3.5 w-3.5" aria-hidden="true" />
                <span>{label}</span>
              </button>
            ))}
          </div>

          {rightTab === 'inspector' && (
            <div className="mt-4 space-y-3 text-xs">
              <div className="flex items-center justify-between">
                <span className="font-semibold text-slate-200">Binary Data Inspector</span>
                <div className="flex gap-1">
                  <button
                    type="button"
                    onClick={() => setEndianness('le')}
                    className={`rounded px-2 py-1 ${endianness === 'le' ? 'bg-cyan-600 text-white' : 'bg-slate-800 text-slate-300'}`}
                  >
                    Little-Endian
                  </button>
                  <button
                    type="button"
                    onClick={() => setEndianness('be')}
                    className={`rounded px-2 py-1 ${endianness === 'be' ? 'bg-cyan-600 text-white' : 'bg-slate-800 text-slate-300'}`}
                  >
                    Big-Endian
                  </button>
                </div>
              </div>

              {interpretation ? (
                <dl className="grid grid-cols-2 gap-2 rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
                  <div><dt className="text-slate-400">Binary (byte 0)</dt><dd className="font-mono-tabular text-slate-100">{interpretation.binary}</dd></div>
                  <div><dt className="text-slate-400">uint8 / int8</dt><dd className="font-mono-tabular text-slate-100">{interpretation.uint8} / {interpretation.int8}</dd></div>
                  <div>
                    <dt className="text-slate-400">uint16 / int16</dt>
                    <dd className="font-mono-tabular text-slate-100">
                      {interpretation.has_16
                        ? `${endianness === 'le' ? interpretation.uint16_le : interpretation.uint16_be} / ${endianness === 'le' ? interpretation.int16_le : interpretation.int16_be}`
                        : 'Need >= 2 bytes'}
                    </dd>
                  </div>
                  <div>
                    <dt className="text-slate-400">uint32 / int32</dt>
                    <dd className="font-mono-tabular text-slate-100">
                      {interpretation.has_32
                        ? `${endianness === 'le' ? interpretation.uint32_le : interpretation.uint32_be} / ${endianness === 'le' ? interpretation.int32_le : interpretation.int32_be}`
                        : 'Need >= 4 bytes'}
                    </dd>
                  </div>
                  <div>
                    <dt className="text-slate-400">float32</dt>
                    <dd className="font-mono-tabular text-slate-100">
                      {interpretation.has_32
                        ? String(endianness === 'le' ? interpretation.float32_le : interpretation.float32_be)
                        : 'Need >= 4 bytes'}
                    </dd>
                  </div>
                  <div>
                    <dt className="text-slate-400">uint64</dt>
                    <dd className="font-mono-tabular text-slate-100">
                      {interpretation.has_64
                        ? String(endianness === 'le' ? interpretation.uint64_le : interpretation.uint64_be)
                        : 'Need >= 8 bytes'}
                    </dd>
                  </div>
                  <div className="col-span-2">
                    <dt className="text-slate-400">float64</dt>
                    <dd className="font-mono-tabular text-slate-100">
                      {interpretation.has_64
                        ? String(endianness === 'le' ? interpretation.float64_le : interpretation.float64_be)
                        : 'Need >= 8 bytes'}
                    </dd>
                  </div>
                  <div className="col-span-2">
                    <dt className="text-slate-400">ASCII / UTF-8</dt>
                    <dd className="font-mono-tabular break-all text-cyan-300">
                      ASCII: &quot;{interpretation.ascii}&quot; · {interpretation.valid_utf8 ? `UTF-8: "${interpretation.utf8}"` : 'Invalid UTF-8 sequence'}
                    </dd>
                  </div>
                </dl>
              ) : (
                <p className="text-slate-400">Select a byte or range in the hex grid to interpret values.</p>
              )}
            </div>
          )}

          {rightTab === 'structure' && (
            <div className="mt-4 space-y-3 text-xs">
              {structure ? (
                <>
                  <div className="flex items-center justify-between">
                    <span className="font-mono-tabular font-semibold text-cyan-300">
                      {structure.structure_type} (Block #{structure.block_id})
                    </span>
                    {structure.checksum_present && (
                      <span
                        className={`rounded px-2 py-0.5 text-[11px] ${
                          structure.checksum_valid
                            ? 'bg-emerald-500/15 text-emerald-300'
                            : 'bg-rose-500/15 text-rose-300'
                        }`}
                      >
                        {structure.checksum_valid ? 'CRC32 Valid' : 'CRC32 Mismatch'}
                      </span>
                    )}
                  </div>
                  <div className="max-h-[420px] space-y-1.5 overflow-y-auto">
                    {structure.fields.map((f) => (
                      <button
                        key={`${f.name}-${f.rel_offset}`}
                        type="button"
                        onClick={() => handleHighlightField(f)}
                        className="flex w-full items-start justify-between gap-2 rounded-lg border border-slate-800 bg-[#0b0f17] p-2.5 text-left hover:border-cyan-500/50"
                      >
                        <div>
                          <p className="font-mono-tabular font-semibold text-slate-100">{f.name}</p>
                          <p className="text-[11px] text-slate-400">{f.description}</p>
                        </div>
                        <div className="text-right">
                          <p className="font-mono-tabular text-cyan-300">{f.value}</p>
                          <p className="font-mono-tabular text-[11px] text-slate-500">
                            +0x{f.rel_offset.toString(16)} ({f.length}B)
                          </p>
                        </div>
                      </button>
                    ))}
                  </div>
                </>
              ) : (
                <p className="text-slate-400">Load a GFS1 metadata block (0, 1, 2, bitmaps, inode table, journal) to decode fields.</p>
              )}
            </div>
          )}

          {rightTab === 'search' && (
            <div className="mt-4 space-y-3 text-xs">
              <form onSubmit={(e) => void handleRunSearch(e)} className="space-y-2.5">
                <div className="grid grid-cols-2 gap-2">
                  <select
                    aria-label="Search query type"
                    value={searchType}
                    onChange={(e) => setSearchType(e.target.value as 'hex' | 'ascii' | 'utf8' | 'signature')}
                    className="rounded border border-slate-700 bg-[#0b0f17] px-2 py-1.5 text-slate-100"
                  >
                    <option value="ascii">ASCII Text</option>
                    <option value="utf8">UTF-8 Text</option>
                    <option value="hex">Hex Pattern</option>
                    <option value="signature">File Signature</option>
                  </select>
                  <select
                    aria-label="Search scope"
                    value={searchScope}
                    onChange={(e) => setSearchScope(e.target.value as typeof searchScope)}
                    className="rounded border border-slate-700 bg-[#0b0f17] px-2 py-1.5 text-slate-100"
                  >
                    <option value="disk">Entire Disk</option>
                    <option value="unallocated">Unallocated Blocks</option>
                    <option value="allocated">Allocated Blocks</option>
                    <option value="metadata">Metadata Regions</option>
                    <option value="file">Logical File</option>
                  </select>
                </div>
                <div className="flex gap-2">
                  <input
                    type="text"
                    aria-label="Search pattern"
                    value={searchValue}
                    onChange={(e) => setSearchValue(e.target.value)}
                    placeholder={searchType === 'hex' ? '89 50 4E 47' : searchType === 'signature' ? 'png | jpeg | pdf' : 'Search text...'}
                    className="font-mono-tabular flex-1 rounded border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
                  />
                  <button
                    type="submit"
                    disabled={searching}
                    className="rounded bg-cyan-600 px-3 py-1.5 font-medium text-white hover:bg-cyan-500 disabled:opacity-50"
                  >
                    {searching ? '...' : 'Search'}
                  </button>
                </div>
              </form>

              {searchRes && (
                <div className="space-y-2">
                  <p className="text-slate-400">
                    {searchRes.total_matches} match(es) ({searchRes.bytes_scanned} bytes scanned)
                  </p>
                  <ul className="max-h-72 space-y-1 overflow-y-auto">
                    {searchRes.matches.map((m, idx) => (
                      <li key={`${m.abs_offset}-${idx}`}>
                        <button
                          type="button"
                          onClick={async () => {
                            setActiveMatchIdx(idx)
                            setSourceType('block')
                            setBlockId(m.block_id)
                            await loadView('block', { blockId: m.block_id })
                            setSelStart(m.abs_offset)
                            setSelEnd(m.abs_offset + m.match_length - 1)
                          }}
                          className={`w-full rounded border p-2 text-left ${
                            activeMatchIdx === idx
                              ? 'border-cyan-500/60 bg-cyan-500/10'
                              : 'border-slate-800 bg-[#0b0f17]'
                          }`}
                        >
                          <p className="font-mono-tabular text-cyan-300">
                            Blk #{m.block_id} · 0x{m.abs_offset.toString(16).toUpperCase()}
                          </p>
                          <p className="font-mono-tabular truncate text-[11px] text-slate-400">
                            {m.context_ascii}
                          </p>
                        </button>
                      </li>
                    ))}
                  </ul>
                </div>
              )}
            </div>
          )}

          {rightTab === 'analysis' && (
            <div className="mt-4 space-y-3 text-xs">
              {analysis ? (
                <>
                  <dl className="grid grid-cols-2 gap-2 rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
                    <div>
                      <dt className="text-slate-400">Shannon Entropy</dt>
                      <dd className="font-mono-tabular text-base font-semibold text-cyan-300">
                        {analysis.shannon_entropy.toFixed(4)} bits/B
                      </dd>
                    </div>
                    <div>
                      <dt className="text-slate-400">Unique Byte Values</dt>
                      <dd className="font-mono-tabular text-base font-semibold text-slate-100">
                        {analysis.unique_byte_values} / 256
                      </dd>
                    </div>
                    <div>
                      <dt className="text-slate-400">Zero Bytes</dt>
                      <dd className="font-mono-tabular text-slate-200">
                        {analysis.zero_bytes} ({analysis.zero_pct.toFixed(1)}%)
                      </dd>
                    </div>
                    <div>
                      <dt className="text-slate-400">Printable ASCII</dt>
                      <dd className="font-mono-tabular text-slate-200">
                        {analysis.printable_ascii_bytes} ({analysis.printable_ascii_pct.toFixed(1)}%)
                      </dd>
                    </div>
                    <div className="col-span-2">
                      <dt className="text-slate-400">CRC32 / SHA-256</dt>
                      <dd className="font-mono-tabular break-all text-[11px] text-slate-300">
                        0x{analysis.crc32.toString(16).toUpperCase().padStart(8, '0')} · {analysis.sha256}
                      </dd>
                    </div>
                  </dl>
                  <div className="h-44 w-full">
                    <ResponsiveContainer width="100%" height="100%">
                      <BarChart data={entropyChartData}>
                        <CartesianGrid strokeDasharray="3 3" stroke="#1e293b" />
                        <XAxis dataKey="range" stroke="#94a3b8" fontSize={10} />
                        <YAxis stroke="#94a3b8" fontSize={10} />
                        <Tooltip contentStyle={{ backgroundColor: '#0f172a', borderColor: '#334155', fontSize: '11px' }} />
                        <Bar dataKey="count" name="Byte Count" fill="#22d3ee" />
                      </BarChart>
                    </ResponsiveContainer>
                  </div>
                  <p className="text-[11px] text-slate-400">
                    Entropy (0–8 bits/byte) is an analytical indicator of randomness, not definitive proof of encryption or compression.
                  </p>
                </>
              ) : (
                <p className="text-slate-400">Click Analyze Entropy in the toolbar to compute Shannon entropy and byte distribution for the current window.</p>
              )}
            </div>
          )}

          {rightTab === 'compare' && (
            <div className="mt-4 space-y-3 text-xs">
              <form onSubmit={(e) => void handleRunCompare(e)} className="space-y-2.5">
                <select
                  aria-label="Comparison mode"
                  value={cmpMode}
                  onChange={(e) => setCmpMode(e.target.value as typeof cmpMode)}
                  className="w-full rounded border border-slate-700 bg-[#0b0f17] px-2.5 py-1.5 text-slate-100"
                >
                  <option value="primary_vs_backup_sb">Primary vs Backup Superblock</option>
                  <option value="blocks">Compare Two Blocks</option>
                  <option value="files">Compare Two GFS Files</option>
                  <option value="file_vs_artifact">Compare GFS File vs Recovered Artifact</option>
                </select>
                {cmpMode !== 'primary_vs_backup_sb' && (
                  <div className="grid grid-cols-2 gap-2">
                    <input
                      type="text"
                      value={cmpA}
                      onChange={(e) => setCmpA(e.target.value)}
                      placeholder={cmpMode === 'blocks' ? 'Block A #' : 'Path A'}
                      className="font-mono-tabular rounded border border-slate-700 bg-[#0b0f17] px-2 py-1.5 text-slate-100"
                    />
                    <input
                      type="text"
                      value={cmpB}
                      onChange={(e) => setCmpB(e.target.value)}
                      placeholder={cmpMode === 'blocks' ? 'Block B #' : cmpMode === 'file_vs_artifact' ? 'Artifact filename' : 'Path B'}
                      className="font-mono-tabular rounded border border-slate-700 bg-[#0b0f17] px-2 py-1.5 text-slate-100"
                    />
                  </div>
                )}
                <button
                  type="submit"
                  className="w-full rounded bg-cyan-600 px-3 py-1.5 font-medium text-white hover:bg-cyan-500"
                >
                  Compare Read-Only
                </button>
              </form>

              {cmpResult && (
                <div className="space-y-2 rounded-lg border border-slate-800 bg-[#0b0f17] p-3">
                  <p className="font-mono-tabular font-semibold text-slate-100">
                    Match: {cmpResult.matching_pct.toFixed(2)}% ({cmpResult.matching_bytes}/{cmpResult.total_compared_bytes} B)
                  </p>
                  <p className="font-mono-tabular text-slate-400">
                    Differing: {cmpResult.differing_bytes} B · Missing: {cmpResult.missing_bytes} B
                    {cmpResult.has_first_diff ? ` · First diff @ 0x${cmpResult.first_diff_offset.toString(16)}` : ''}
                  </p>
                  {cmpResult.diffs.length > 0 && (
                    <ul className="max-h-40 space-y-1 overflow-y-auto pt-1 font-mono-tabular text-[11px]">
                      {cmpResult.diffs.slice(0, 32).map((d) => (
                        <li key={d.offset} className="text-rose-300">
                          0x{d.offset.toString(16).padStart(4, '0')}: A={d.has_a ? d.byte_a.toString(16).padStart(2, '0') : '--'} vs B={d.has_b ? d.byte_b.toString(16).padStart(2, '0') : '--'}
                        </li>
                      ))}
                    </ul>
                  )}
                </div>
              )}
            </div>
          )}
        </section>
      </div>
    </div>
  )
}
