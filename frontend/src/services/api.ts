import type {
  AllocPolicy,
  ArtifactPreviewResponse,
  BlockInfo,
  BlockMapPage,
  BlockSize,
  ByteAnalysisMetrics,
  ByteInterpretation,
  ComparisonExperimentResult,
  DecodedStructure,
  DeleteEntryResponse,
  DirectoryListingResponse,
  DiskListItem,
  FileEntryInfo,
  FileReadResponse,
  ForensicScanResult,
  FragmentationReport,
  HealthResponse,
  HexCompareResult,
  HexReadResult,
  HexSearchResult,
  RecoveryManifest,
  VolumeStatus,
  WorkloadStep,
} from '../types/api'

const API_BASE = '/api/v1'

async function parseJsonOrThrow<T>(response: Response): Promise<T> {
  if (!response.ok) {
    let message = `Request failed with status ${response.status}`
    try {
      const errBody = (await response.json()) as {
        error?: { message?: string }
        detail?: string
      }
      if (errBody?.error?.message) {
        message = errBody.error.message
      } else if (typeof errBody?.detail === 'string') {
        message = errBody.detail
      }
    } catch {
      // Keep status message when response body is not JSON
    }
    throw new Error(message)
  }
  return (await response.json()) as T
}

export async function fetchHealth(): Promise<HealthResponse> {
  return parseJsonOrThrow<HealthResponse>(await fetch(`${API_BASE}/health`))
}

export async function listDisks(): Promise<DiskListItem[]> {
  const data = await parseJsonOrThrow<{ disks: DiskListItem[] }>(
    await fetch(`${API_BASE}/disks`),
  )
  return data.disks
}

export async function createDisk(payload: {
  name: string
  size_bytes: number
  block_size: BlockSize
  label: string
  alloc_policy: AllocPolicy
  inode_count?: number
}): Promise<VolumeStatus> {
  return parseJsonOrThrow<VolumeStatus>(
    await fetch(`${API_BASE}/disks`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function openDisk(diskName: string): Promise<VolumeStatus> {
  return parseJsonOrThrow<VolumeStatus>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/open`, {
      method: 'POST',
    }),
  )
}

export async function formatDisk(
  diskName: string,
  payload: {
    block_size: BlockSize
    label: string
    alloc_policy: AllocPolicy
    inode_count?: number
  },
): Promise<VolumeStatus> {
  return parseJsonOrThrow<VolumeStatus>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/format`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function deleteDisk(diskName: string): Promise<{ deleted: string }> {
  return parseJsonOrThrow<{ deleted: string }>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}?confirm=true`, {
      method: 'DELETE',
    }),
  )
}

export async function listDirectory(
  diskName: string,
  path: string,
): Promise<DirectoryListingResponse> {
  const qs = new URLSearchParams({ path })
  return parseJsonOrThrow<DirectoryListingResponse>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/fs/tree?${qs}`),
  )
}

export async function makeDirectory(
  diskName: string,
  path: string,
): Promise<{ entry: FileEntryInfo; volume: VolumeStatus }> {
  return parseJsonOrThrow<{ entry: FileEntryInfo; volume: VolumeStatus }>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/fs/mkdir`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path }),
    }),
  )
}

export async function writeTextFile(
  diskName: string,
  path: string,
  textContent: string,
  overwrite = true,
): Promise<{ entry: FileEntryInfo; volume: VolumeStatus }> {
  const bytes = new TextEncoder().encode(textContent)
  let binary = ''
  for (let i = 0; i < bytes.length; i += 1) {
    binary += String.fromCharCode(bytes[i])
  }
  const content_base64 = btoa(binary)

  return parseJsonOrThrow<{ entry: FileEntryInfo; volume: VolumeStatus }>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/fs/files`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path, content_base64, overwrite }),
    }),
  )
}

export async function uploadBinaryFile(
  diskName: string,
  file: File,
  dirPath: string,
  overwrite = true,
): Promise<{ entry: FileEntryInfo; volume: VolumeStatus }> {
  const form = new FormData()
  form.append('file', file)
  form.append('dir_path', dirPath)
  form.append('overwrite', String(overwrite))

  return parseJsonOrThrow<{ entry: FileEntryInfo; volume: VolumeStatus }>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/fs/files/upload`, {
      method: 'POST',
      body: form,
    }),
  )
}

export async function readFile(diskName: string, path: string): Promise<FileReadResponse> {
  const qs = new URLSearchParams({ path })
  return parseJsonOrThrow<FileReadResponse>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/fs/files/read?${qs}`),
  )
}

export function getDownloadUrl(diskName: string, path: string): string {
  const qs = new URLSearchParams({ path })
  return `${API_BASE}/disks/${encodeURIComponent(diskName)}/fs/files/download?${qs}`
}

export async function renameEntry(
  diskName: string,
  oldPath: string,
  newPath: string,
): Promise<{ entry: FileEntryInfo; volume: VolumeStatus }> {
  return parseJsonOrThrow<{ entry: FileEntryInfo; volume: VolumeStatus }>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/fs/rename`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ old_path: oldPath, new_path: newPath }),
    }),
  )
}

export async function deleteEntry(
  diskName: string,
  path: string,
  recursive = false,
): Promise<DeleteEntryResponse> {
  const qs = new URLSearchParams({ path, recursive: String(recursive) })
  return parseJsonOrThrow<DeleteEntryResponse>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/fs/delete?${qs}`, {
      method: 'DELETE',
    }),
  )
}

export async function setAllocPolicy(
  diskName: string,
  allocPolicy: AllocPolicy,
): Promise<VolumeStatus> {
  return parseJsonOrThrow<VolumeStatus>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/policy`, {
      method: 'PATCH',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ alloc_policy: allocPolicy }),
    }),
  )
}

export async function getBlockMap(
  diskName: string,
  params: { offset?: number; limit?: number; type_filter?: string; file_inode?: number } = {},
): Promise<BlockMapPage> {
  const qs = new URLSearchParams()
  if (params.offset !== undefined) qs.set('offset', String(params.offset))
  if (params.limit !== undefined) qs.set('limit', String(params.limit))
  if (params.type_filter) qs.set('type_filter', params.type_filter)
  if (params.file_inode) qs.set('file_inode', String(params.file_inode))
  return parseJsonOrThrow<BlockMapPage>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/blocks/map?${qs}`),
  )
}

export async function inspectBlock(diskName: string, blockId: number): Promise<BlockInfo> {
  return parseJsonOrThrow<BlockInfo>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/blocks/${blockId}`),
  )
}

export async function getFragmentationReport(diskName: string): Promise<FragmentationReport> {
  return parseJsonOrThrow<FragmentationReport>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/fragmentation`),
  )
}

export async function compareAllocPolicies(
  diskName: string,
  payload: {
    block_size: BlockSize
    total_blocks: number
    use_active_disk: boolean
    workload: WorkloadStep[]
  },
): Promise<ComparisonExperimentResult> {
  return parseJsonOrThrow<ComparisonExperimentResult>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/alloc-compare`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function startRecoveryScan(
  diskName: string,
  payload: {
    mode: 'quick' | 'deep' | 'combined'
    file_types: string[]
    enable_txt_heuristic: boolean
  },
): Promise<ForensicScanResult> {
  return parseJsonOrThrow<ForensicScanResult>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/recovery/scan`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function extractRecoveryCandidates(
  diskName: string,
  payload: {
    scan_id: string
    candidate_ids: string[]
    zero_fill_missing: boolean
    overwrite_existing?: boolean
    enable_txt_heuristic: boolean
  },
): Promise<{ extracted_count: number; artifacts: RecoveryManifest[] }> {
  return parseJsonOrThrow<{ extracted_count: number; artifacts: RecoveryManifest[] }>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/recovery/extract`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function getArtifactPreview(artifactId: string): Promise<ArtifactPreviewResponse> {
  return parseJsonOrThrow<ArtifactPreviewResponse>(
    await fetch(`${API_BASE}/recovery/artifacts/${encodeURIComponent(artifactId)}`),
  )
}

export function getArtifactDownloadUrl(artifactId: string): string {
  return `${API_BASE}/recovery/artifacts/${encodeURIComponent(artifactId)}/download`
}

export function getManifestDownloadUrl(artifactId: string): string {
  return `${API_BASE}/recovery/artifacts/${encodeURIComponent(artifactId)}/manifest`
}

export async function readHexBlock(
  diskName: string,
  blockId: number,
  bytesPerRow = 16,
  blockCount = 1,
): Promise<HexReadResult> {
  const qs = new URLSearchParams({
    block_count: String(blockCount),
    bytes_per_row: String(bytesPerRow),
  })
  return parseJsonOrThrow<HexReadResult>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/hex/blocks/${blockId}?${qs}`),
  )
}

export async function readHexRange(
  diskName: string,
  offset: number,
  length: number,
  bytesPerRow = 16,
): Promise<HexReadResult> {
  const qs = new URLSearchParams({
    offset: String(offset),
    length: String(length),
    bytes_per_row: String(bytesPerRow),
  })
  return parseJsonOrThrow<HexReadResult>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/hex/range?${qs}`),
  )
}

export async function readHexFile(
  diskName: string,
  path: string,
  offset = 0,
  length = 4096,
  bytesPerRow = 16,
): Promise<HexReadResult> {
  const qs = new URLSearchParams({
    path,
    offset: String(offset),
    length: String(length),
    bytes_per_row: String(bytesPerRow),
  })
  return parseJsonOrThrow<HexReadResult>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/hex/files?${qs}`),
  )
}

export async function readHexArtifact(
  diskName: string,
  artifactFilename: string,
  offset = 0,
  length = 4096,
  bytesPerRow = 16,
): Promise<HexReadResult> {
  const qs = new URLSearchParams({
    offset: String(offset),
    length: String(length),
    bytes_per_row: String(bytesPerRow),
  })
  return parseJsonOrThrow<HexReadResult>(
    await fetch(
      `${API_BASE}/disks/${encodeURIComponent(diskName)}/hex/artifacts/${encodeURIComponent(artifactFilename)}?${qs}`,
    ),
  )
}

export async function decodeHexStructure(
  diskName: string,
  blockId: number,
  forceType = '',
): Promise<DecodedStructure> {
  const qs = new URLSearchParams({ block_id: String(blockId), force_type: forceType })
  return parseJsonOrThrow<DecodedStructure>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/hex/structures?${qs}`),
  )
}

export async function interpretHexBytes(
  diskName: string,
  bytes: number[],
): Promise<ByteInterpretation> {
  return parseJsonOrThrow<ByteInterpretation>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/hex/interpret`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ bytes }),
    }),
  )
}

export async function searchHex(
  diskName: string,
  payload: {
    query_type: 'hex' | 'ascii' | 'utf8' | 'signature'
    query_value: string
    scope: 'disk' | 'block_range' | 'allocated' | 'unallocated' | 'file' | 'metadata'
    start_block?: number
    end_block?: number
    path?: string
    offset?: number
    limit?: number
  },
): Promise<HexSearchResult> {
  return parseJsonOrThrow<HexSearchResult>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/hex/search`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function analyzeHexRange(
  diskName: string,
  offset: number,
  length: number,
): Promise<ByteAnalysisMetrics> {
  return parseJsonOrThrow<ByteAnalysisMetrics>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/hex/analyze`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ offset, length }),
    }),
  )
}

export async function compareHexSources(
  diskName: string,
  payload: {
    mode: 'blocks' | 'ranges' | 'files' | 'primary_vs_backup_sb' | 'file_vs_artifact'
    a_start?: number
    a_len?: number
    b_start?: number
    b_len?: number
    path_a?: string
    path_b?: string
    artifact_filename?: string
  },
): Promise<HexCompareResult> {
  return parseJsonOrThrow<HexCompareResult>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/hex/compare`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

import type {
  ExperimentComparisonResult,
  FsckResult,
  RepairApplyResult,
  RepairPlan,
  SandboxExperiment,
} from '../types/api'

export async function listCorruptionExperiments(): Promise<{ experiments: SandboxExperiment[] }> {
  return parseJsonOrThrow<{ experiments: SandboxExperiment[] }>(
    await fetch(`${API_BASE}/corruption/experiments`),
  )
}

export async function createCorruptionExperiment(
  sourceDiskName: string,
  sandboxName = '',
): Promise<SandboxExperiment> {
  return parseJsonOrThrow<SandboxExperiment>(
    await fetch(`${API_BASE}/corruption/experiments`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ source_disk_name: sourceDiskName, sandbox_name: sandboxName }),
    }),
  )
}

export async function getCorruptionExperiment(experimentId: string): Promise<SandboxExperiment> {
  return parseJsonOrThrow<SandboxExperiment>(
    await fetch(`${API_BASE}/corruption/experiments/${encodeURIComponent(experimentId)}`),
  )
}

export async function injectCorruption(
  experimentId: string,
  payload: {
    category: string
    scenario: string
    target_block?: number
    target_inode?: number
    secondary_inode?: number
    severity?: number
    seed?: number
    confirm: boolean
  },
): Promise<SandboxExperiment> {
  return parseJsonOrThrow<SandboxExperiment>(
    await fetch(`${API_BASE}/corruption/experiments/${encodeURIComponent(experimentId)}/inject`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function checkCorruptionExperiment(
  experimentId: string,
  mode: 'fast' | 'full' = 'full',
): Promise<FsckResult> {
  return parseJsonOrThrow<FsckResult>(
    await fetch(`${API_BASE}/corruption/experiments/${encodeURIComponent(experimentId)}/check`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mode }),
    }),
  )
}

export async function generateRepairPlan(
  experimentId: string,
  mode: 'fast' | 'full' = 'full',
): Promise<RepairPlan> {
  return parseJsonOrThrow<RepairPlan>(
    await fetch(`${API_BASE}/corruption/experiments/${encodeURIComponent(experimentId)}/repair-plan`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mode }),
    }),
  )
}

export async function applyRepairPlan(
  experimentId: string,
  payload: {
    plan_id: string
    expected_disk_sha256: string
    selected_action_ids: string[]
    dry_run: boolean
    confirm: boolean
  },
): Promise<RepairApplyResult> {
  return parseJsonOrThrow<RepairApplyResult>(
    await fetch(`${API_BASE}/corruption/experiments/${encodeURIComponent(experimentId)}/repair`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function compareCorruptionExperiment(
  experimentId: string,
  compareMode: 'original_vs_corrupted' | 'corrupted_vs_repaired' | 'original_vs_repaired',
): Promise<ExperimentComparisonResult> {
  const qs = new URLSearchParams({ compare_mode: compareMode })
  return parseJsonOrThrow<ExperimentComparisonResult>(
    await fetch(
      `${API_BASE}/corruption/experiments/${encodeURIComponent(experimentId)}/comparison?${qs}`,
    ),
  )
}

export async function deleteCorruptionExperiment(
  experimentId: string,
  confirm = true,
): Promise<{ deleted: boolean; experiment_id: string }> {
  return parseJsonOrThrow<{ deleted: boolean; experiment_id: string }>(
    await fetch(
      `${API_BASE}/corruption/experiments/${encodeURIComponent(experimentId)}?confirm=${confirm ? 'true' : 'false'}`,
      { method: 'DELETE' },
    ),
  )
}

export function getExperimentManifestUrl(experimentId: string): string {
  return `${API_BASE}/corruption/experiments/${encodeURIComponent(experimentId)}/manifest`
}

export function getExperimentDownloadUrl(experimentId: string): string {
  return `${API_BASE}/corruption/experiments/${encodeURIComponent(experimentId)}/download`
}

import type {
  AllocationSchedulingResult,
  MultiAlgorithmComparisonResult,
  SchedulingAlgorithm,
  SimulationResult,
  SweepDirection,
  WorkloadGenerationResult,
} from '../types/api'

export async function simulateScheduling(payload: {
  algorithm: SchedulingAlgorithm
  min_cylinder: number
  max_cylinder: number
  initial_head: number
  initial_direction: SweepDirection
  requests: Array<number | { cylinder: number; logical_block?: number; op_type?: string; label?: string }>
}): Promise<SimulationResult> {
  return parseJsonOrThrow<SimulationResult>(
    await fetch(`${API_BASE}/scheduling/simulate`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function compareScheduling(payload: {
  min_cylinder: number
  max_cylinder: number
  initial_head: number
  initial_direction: SweepDirection
  requests: Array<number | { cylinder: number; logical_block?: number; op_type?: string; label?: string }>
}): Promise<MultiAlgorithmComparisonResult> {
  return parseJsonOrThrow<MultiAlgorithmComparisonResult>(
    await fetch(`${API_BASE}/scheduling/compare`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function generateSchedulingWorkload(payload: {
  pattern: 'sequential' | 'random' | 'clustered' | 'mixed' | 'hotspot' | 'fragmented'
  count: number
  min_cylinder: number
  max_cylinder: number
  seed: number
}): Promise<WorkloadGenerationResult> {
  return parseJsonOrThrow<WorkloadGenerationResult>(
    await fetch(`${API_BASE}/scheduling/workloads/generate`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function getFileSchedulingWorkload(
  diskName: string,
  payload: { file_paths: string[]; include_metadata_blocks: boolean; blocks_per_cylinder: number },
): Promise<WorkloadGenerationResult> {
  return parseJsonOrThrow<WorkloadGenerationResult>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/scheduling/workloads/files`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function getRecoverySchedulingWorkload(
  diskName: string,
  payload: { candidate_id: string; enable_txt_heuristic: boolean; blocks_per_cylinder: number },
): Promise<WorkloadGenerationResult> {
  return parseJsonOrThrow<WorkloadGenerationResult>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/scheduling/workloads/recovery`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function getMetadataSchedulingWorkload(
  diskName: string,
  blocksPerCylinder = 1,
): Promise<WorkloadGenerationResult> {
  return parseJsonOrThrow<WorkloadGenerationResult>(
    await fetch(`${API_BASE}/disks/${encodeURIComponent(diskName)}/scheduling/workloads/metadata`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ blocks_per_cylinder: blocksPerCylinder }),
    }),
  )
}

export async function compareAllocationAndScheduling(payload: {
  disk_name?: string
  block_size: BlockSize
  total_blocks: number
  initial_head: number
  initial_direction: SweepDirection
  blocks_per_cylinder: number
  workload: WorkloadStep[]
}): Promise<AllocationSchedulingResult> {
  return parseJsonOrThrow<AllocationSchedulingResult>(
    await fetch(`${API_BASE}/scheduling/allocation-compare`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

import type {
  AvailableEvidenceResponse,
  ReportManifest,
  ReportSnapshot,
  ReportType,
} from '../types/api'

export async function fetchAvailableEvidence(): Promise<AvailableEvidenceResponse> {
  return parseJsonOrThrow<AvailableEvidenceResponse>(await fetch(`${API_BASE}/reports/evidence`))
}

export async function previewReport(payload: {
  report_type: ReportType
  disk_name?: string | null
  title?: string
  case_id?: string
  investigator?: string
  notes?: string
  experiment_id?: string | null
  scheduling_payload?: Record<string, unknown> | null
  binary_payload?: Record<string, unknown> | null
  include_sections?: string[] | null
}): Promise<ReportSnapshot> {
  return parseJsonOrThrow<ReportSnapshot>(
    await fetch(`${API_BASE}/reports/preview`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function generateReport(payload: {
  report_type: ReportType
  disk_name?: string | null
  title?: string
  case_id?: string
  investigator?: string
  notes?: string
  experiment_id?: string | null
  scheduling_payload?: Record<string, unknown> | null
  binary_payload?: Record<string, unknown> | null
  include_sections?: string[] | null
  export_pdf?: boolean
  export_json?: boolean
  export_csv?: boolean
}): Promise<ReportManifest> {
  return parseJsonOrThrow<ReportManifest>(
    await fetch(`${API_BASE}/reports/generate`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    }),
  )
}

export async function listReports(): Promise<{ reports: ReportManifest[] }> {
  return parseJsonOrThrow<{ reports: ReportManifest[] }>(await fetch(`${API_BASE}/reports`))
}

export async function verifyReportIntegrity(reportId: string): Promise<{
  report_id: string
  verified: boolean
  checks: Array<{ artifact: string; expected_sha256: string; actual_sha256: string; matched: boolean }>
}> {
  return parseJsonOrThrow(
    await fetch(`${API_BASE}/reports/${encodeURIComponent(reportId)}/verify`, { method: 'POST' }),
  )
}

export async function deleteReport(
  reportId: string,
  confirm = true,
): Promise<{ deleted: boolean; report_id: string }> {
  return parseJsonOrThrow(
    await fetch(
      `${API_BASE}/reports/${encodeURIComponent(reportId)}?confirm=${confirm ? 'true' : 'false'}`,
      { method: 'DELETE' },
    ),
  )
}

export function getReportPdfUrl(reportId: string): string {
  return `${API_BASE}/reports/${encodeURIComponent(reportId)}/download/pdf`
}

export function getReportJsonUrl(reportId: string): string {
  return `${API_BASE}/reports/${encodeURIComponent(reportId)}/download/json`
}

export function getReportManifestUrl(reportId: string): string {
  return `${API_BASE}/reports/${encodeURIComponent(reportId)}/manifest`
}

export function getReportCsvExportUrl(reportId: string, exportId: string): string {
  return `${API_BASE}/reports/${encodeURIComponent(reportId)}/exports/${encodeURIComponent(exportId)}`
}
