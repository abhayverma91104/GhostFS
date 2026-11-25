export interface EngineInfo {
  engine: string
  version: string
  fs_format: string
  cpp_standard: number
  pid: number
  timestamp_ms: number
  workspace: string
  workspace_ready: boolean
  modules: string[]
}

export interface HealthResponse {
  status: string
  api_version: string
  engine_reachable: boolean
  engine_bin: string
  workspace_root: string
  disk_images_dir: string
  reports_dir: string
  engine_info: EngineInfo
}

export type BlockSize = 512 | 1024 | 2048 | 4096
export type AllocPolicy = 'first_fit' | 'best_fit' | 'worst_fit' | 'bitmap_next_fit'

export interface GfsLayout {
  boot_block: number
  primary_superblock: number
  backup_superblock: number
  inode_bitmap_start: number
  inode_bitmap_blocks: number
  block_bitmap_start: number
  block_bitmap_blocks: number
  inode_table_start: number
  inode_table_blocks: number
  data_region_start: number
  data_blocks_count: number
  journal_start: number
  journal_blocks: number
}

export interface JournalEntry {
  seq_no: number
  timestamp_ms: number
  op_type: number
  status: number
  target_inode: number
  target_block: number
  aux_value: number
  summary: string
}

export interface VolumeStatus {
  disk_name: string
  volume_label: string
  block_size: BlockSize
  total_blocks: number
  reserved_blocks: number
  data_blocks_count: number
  free_blocks: number
  allocated_data_blocks: number
  total_inodes: number
  free_inodes: number
  used_inodes: number
  total_bytes: number
  usable_data_bytes: number
  used_data_bytes: number
  alloc_policy: AllocPolicy
  fs_state: number
  primary_sb_crc_ok: boolean
  backup_sb_crc_ok: boolean
  boot_crc_ok: boolean
  dirty_journal_detected: boolean
  mount_count: number
  last_mount_ms: number
  last_write_ms: number
  layout: GfsLayout
  recent_journal: JournalEntry[]
}

export interface DiskListItem extends Partial<VolumeStatus> {
  disk_name: string
  total_bytes: number
  formatted: boolean
  valid: boolean
  error?: string
}

export interface FileEntryInfo {
  inode_id: number
  name: string
  path: string
  type: 'file' | 'directory'
  permissions: number
  size_bytes: number
  created_at_ms: number
  modified_at_ms: number
  content_crc32: number
  generation: number
  direct_blocks: number[]
  indirect_block: number
  double_indirect_block: number
  all_data_blocks: number[]
  all_indirect_blocks: number[]
  sha256?: string
}

export interface DirectoryListingResponse {
  path: string
  entries: FileEntryInfo[]
  volume: VolumeStatus
}

export interface FileReadResponse {
  entry: FileEntryInfo
  content_base64: string
  is_text: boolean
  text_preview: string | null
}

export interface DeleteEntryResponse {
  deleted_path: string
  inode_id: number
  released_blocks_count: number
  released_blocks: number[]
  volume: VolumeStatus
}

export type BlockClassification =
  | 'reserved'
  | 'primary_superblock'
  | 'backup_superblock'
  | 'inode_bitmap'
  | 'block_bitmap'
  | 'inode_table'
  | 'directory_data'
  | 'file_data'
  | 'indirect_pointer'
  | 'journal'
  | 'free'
  | 'free_deleted_residual'
  | 'inconsistent'

export interface BlockInfo {
  block_id: number
  byte_offset: number
  block_size: number
  allocated: boolean
  classification: BlockClassification
  inode_id: number
  owner_path: string
  extent_start: number
  extent_length: number
}

export interface BlockMapPage {
  total_blocks: number
  filtered_total: number
  offset: number
  limit: number
  block_size: number
  blocks: BlockInfo[]
}

export interface FreeExtent {
  start_block: number
  length: number
}

export interface FileFragmentationInfo {
  inode_id: number
  name: string
  path: string
  size_bytes: number
  allocated_data_blocks: number
  allocated_payload_capacity_bytes: number
  internal_fragmentation_bytes: number
  internal_fragmentation_pct: number
  physical_data_blocks: number[]
  indirect_blocks: number[]
  extent_count: number
  is_fragmented: boolean
}

export interface FragmentationReport {
  disk_name: string
  alloc_policy: AllocPolicy
  block_size: number
  total_blocks: number
  reserved_metadata_blocks: number
  journal_blocks: number
  data_blocks_count: number
  allocated_data_blocks: number
  file_data_blocks: number
  directory_data_blocks: number
  indirect_pointer_blocks: number
  free_blocks: number
  free_deleted_residual_blocks: number
  inconsistent_blocks: number
  free_extent_count: number
  largest_free_extent: number
  smallest_free_extent: number
  average_free_extent: number
  fragmented_file_count: number
  contiguous_file_count: number
  total_files: number
  total_allocated_payload_capacity_bytes: number
  total_logical_file_bytes: number
  internal_fragmentation_bytes: number
  internal_fragmentation_pct: number
  external_fragmentation_pct: number
  disk_utilization_pct: number
  data_utilization_pct: number
  free_extents: FreeExtent[]
  files: FileFragmentationInfo[]
}

export interface WorkloadStep {
  op: 'create' | 'delete'
  path: string
  size_bytes: number
}

export interface WorkloadStepResult {
  op: string
  path: string
  size_bytes: number
  success: boolean
  error: string
  blocks_allocated: number[]
  extent_count: number
  blocks_inspected: number
  extents_examined: number
  used_fragmented_fallback: boolean
}

export interface PolicyComparisonResult {
  policy: AllocPolicy
  success_count: number
  failure_count: number
  total_blocks_inspected: number
  total_extents_examined: number
  measured_elapsed_ms: number
  final_report: FragmentationReport
  steps: WorkloadStepResult[]
}

export interface ComparisonExperimentResult {
  experiment_mode: 'fresh_disk' | 'cloned_active_disk'
  block_size: number
  total_blocks: number
  policies: PolicyComparisonResult[]
}

export type RecoveryClassification =
  | 'fully_verified'
  | 'complete_unverified'
  | 'partially_recoverable'
  | 'overwritten'
  | 'unrecoverable'
  | 'unknown'

export interface ByteRange {
  start_byte: number
  end_byte: number
}

export interface LogicalBlockStatus {
  logical_index: number
  physical_block: number
  disk_byte_offset: number
  state: 'intact' | 'overwritten' | 'missing_indirect' | 'unknown'
  detail: string
}

export interface RecoveryCandidate {
  candidate_id: string
  recovery_method: 'metadata' | 'carving' | 'combined'
  file_type: string
  filename: string
  original_path: string
  filename_inferred: boolean
  inode_id: number
  generation: number
  original_size_bytes: number
  recoverable_bytes: number
  recovery_pct: number
  created_at_ms: number
  modified_at_ms: number
  deleted_at_ms: number
  expected_crc32: number
  has_expected_crc32: boolean
  reconstructed_crc32: number
  crc32_matched: boolean
  integrity_status: string
  classification: RecoveryClassification
  source_blocks: number[]
  source_byte_offsets: number[]
  block_statuses: LogicalBlockStatus[]
  recoverable_ranges: ByteRange[]
  missing_ranges: ByteRange[]
  evidence: string[]
  warnings: string[]
}

export interface ForensicScanResult {
  scan_id: string
  disk_name: string
  source_disk_sha256: string
  scan_mode: 'quick' | 'deep' | 'combined'
  scan_duration_ms: number
  blocks_examined: number
  bytes_examined: number
  total_candidates: number
  summary: Record<RecoveryClassification, number>
  warnings: string[]
  candidates: RecoveryCandidate[]
}

export interface RecoveryManifest {
  recovery_id: string
  artifact_filename: string
  artifact_path: string
  manifest_path: string
  source_disk_name: string
  source_disk_sha256: string
  recovery_timestamp_utc: string
  recovery_method: string
  original_filename: string
  inode_id: number
  generation: number
  original_expected_size: number
  recovered_size: number
  verified_bytes: number
  unknown_candidate_bytes: number
  missing_bytes: number
  overwritten_bytes: number
  substituted_bytes: number
  source_blocks: number[]
  source_byte_offsets: number[]
  missing_ranges: ByteRange[]
  substituted_ranges: ByteRange[]
  has_original_crc32: boolean
  original_crc32: number
  recovered_crc32: number
  recovered_sha256: string
  integrity_result: string
  classification: RecoveryClassification
  engine_version: string
  warnings: string[]
}

export interface ArtifactPreviewResponse {
  manifest: RecoveryManifest
  is_text: boolean
  text_preview: string | null
  is_image: boolean
  image_mime: string | null
  image_base64: string | null
}

export interface HexRow {
  abs_offset: number
  block_id: number
  block_offset: number
  logical_offset: number
  raw_bytes: number[]
  hex_bytes: string[]
  ascii: string
}

export interface SignatureCheckResult {
  detected: boolean
  file_type: string
  signature_hex: string
  offset: number
  length: number
  structurally_valid: boolean
  validation_note: string
  warnings: string[]
}

export interface HexReadResult {
  disk_name: string
  source_type: 'disk_range' | 'block' | 'file' | 'artifact'
  start_offset: number
  length: number
  block_size: number
  bytes_per_row: number
  total_source_bytes: number
  start_block: number
  classification: string
  inode_id: number
  owner_path: string
  signature: SignatureCheckResult
  rows: HexRow[]
}

export interface ByteInterpretation {
  byte_count: number
  hex: string
  binary: string
  uint8: number
  int8: number
  has_16: boolean
  uint16_le?: number
  uint16_be?: number
  int16_le?: number
  int16_be?: number
  has_32: boolean
  uint32_le?: number
  uint32_be?: number
  int32_le?: number
  int32_be?: number
  float32_le?: number | string
  float32_be?: number | string
  float32_le_status?: string
  float32_be_status?: string
  has_64: boolean
  uint64_le?: number
  uint64_be?: number
  int64_le?: number
  int64_be?: number
  float64_le?: number | string
  float64_be?: number | string
  float64_le_status?: string
  float64_be_status?: string
  ascii: string
  valid_utf8: boolean
  utf8: string
}

export interface DecodedField {
  name: string
  abs_offset: number
  rel_offset: number
  length: number
  raw_hex: string
  value: string
  data_type: string
  description: string
  has_validation: boolean
  valid: boolean
  validation_note: string
}

export interface DecodedStructure {
  structure_type: string
  block_id: number
  base_offset: number
  checksum_present: boolean
  checksum_valid: boolean
  warnings: string[]
  fields: DecodedField[]
}

export interface HexSearchMatch {
  abs_offset: number
  block_id: number
  block_offset: number
  logical_offset: number
  match_length: number
  matched_hex: string
  context_ascii: string
}

export interface HexSearchResult {
  scope: string
  query_type: string
  source_disk_sha256: string
  total_matches: number
  truncated: boolean
  offset: number
  limit: number
  bytes_scanned: number
  matches: HexSearchMatch[]
}

export interface ByteAnalysisMetrics {
  total_bytes: number
  zero_bytes: number
  nonzero_bytes: number
  zero_pct: number
  printable_ascii_bytes: number
  printable_ascii_pct: number
  unique_byte_values: number
  shannon_entropy: number
  crc32: number
  sha256: string
  histogram: number[]
}

export interface ByteDiffEntry {
  offset: number
  has_a: boolean
  has_b: boolean
  byte_a: number
  byte_b: number
}

export interface SuperblockFieldDiff {
  field_name: string
  primary_value: string
  backup_value: string
  differs: boolean
  expected_difference: boolean
}

export interface HexCompareResult {
  mode: string
  len_a: number
  len_b: number
  total_compared_bytes: number
  matching_bytes: number
  differing_bytes: number
  missing_bytes: number
  has_first_diff: boolean
  first_diff_offset: number
  matching_pct: number
  sha256_a: string
  sha256_b: string
  diffs: ByteDiffEntry[]
  sb_field_diffs: SuperblockFieldDiff[]
}

export interface HexNavigationTarget {
  sourceType: 'block' | 'disk_range' | 'file' | 'artifact'
  blockId?: number
  offset?: number
  length?: number
  filePath?: string
  artifactFilename?: string
}

export type NavSectionId =
  | 'dashboard'
  | 'disk-manager'
  | 'file-explorer'
  | 'block-explorer'
  | 'recovery-center'
  | 'hex-viewer'
  | 'corruption-lab'
  | 'disk-scheduler'
  | 'forensic-reports'

export interface CorruptionRecord {
  operation_id: string
  category: string
  scenario: string
  description: string
  original_disk_sha256: string
  before_sandbox_sha256: string
  after_sandbox_sha256: string
  affected_offsets: number[]
  affected_blocks: number[]
  original_bytes_hex: string
  replacement_bytes_hex: string
  timestamp_utc: string
  seed: number
  used_seed: boolean
}

export interface SandboxExperiment {
  experiment_id: string
  source_disk_name: string
  sandbox_filename: string
  original_sha256: string
  initial_sandbox_sha256: string
  current_sandbox_sha256: string
  pre_repair_snapshot_filename: string
  pre_repair_sha256: string
  created_at_utc: string
  updated_at_utc: string
  operations: CorruptionRecord[]
}

export interface FsckIssue {
  issue_id: string
  issue_type: string
  severity: 'information' | 'warning' | 'error' | 'critical'
  description: string
  affected_structure: string
  affected_block: number
  affected_inode: number
  byte_offset: number
  expected_value: string
  observed_value: string
  evidence: string
  repairability:
    | 'automatically_repairable'
    | 'repairable_with_confirmation'
    | 'ambiguous_manual_required'
    | 'not_repairable'
  recommended_action: string
  data_loss_risk: string
}

export interface FsckResult {
  disk_name: string
  disk_sha256: string
  check_mode: 'fast' | 'full'
  geometry_valid: boolean
  primary_sb_valid: boolean
  backup_sb_valid: boolean
  dirty_journal: boolean
  consistency_status: 'clean' | 'inconsistent' | 'unrecoverable_geometry' | 'payload_damaged'
  info_count: number
  warning_count: number
  error_count: number
  critical_count: number
  total_issues: number
  preserved_files_count: number
  damaged_files_count: number
  damaged_file_paths: string[]
  issues: FsckIssue[]
}

export interface RepairAction {
  action_id: string
  issue_id: string
  action_type: string
  target_structure: string
  byte_offsets: number[]
  existing_value: string
  proposed_value: string
  justification: string
  preconditions: string
  dependencies: string[]
  expected_post_repair_state: string
  data_loss_implication: string
  repairability: string
  selected_by_default: boolean
}

export interface RepairPlan {
  plan_id: string
  disk_name: string
  disk_sha256_at_plan: string
  check_mode: string
  initial_check: FsckResult
  actions: RepairAction[]
  unrepairable_or_ambiguous: Array<{
    issue_id: string
    issue_type: string
    severity: string
    description: string
    repairability: string
    recommended_action: string
  }>
}

export interface RepairApplyResult {
  plan_id: string
  dry_run: boolean
  pre_repair_sha256: string
  post_repair_sha256: string
  pre_repair_snapshot_filename: string
  issues_before: number
  issues_resolved: number
  issues_remaining: number
  newly_detected_issues: number
  bytes_changed: number
  blocks_changed: number[]
  repaired_structures: string[]
  applied_action_ids: string[]
  skipped_action_ids: string[]
  reconnected_paths: string[]
  post_check: FsckResult
}

export interface BlockChangeSummary {
  block_id: number
  byte_offset: number
  bytes_differing: number
  sample_before_hex: string
  sample_after_hex: string
}

export interface ExperimentComparisonResult {
  experiment_id: string
  mode: string
  source_label: string
  target_label: string
  source_sha256: string
  target_sha256: string
  identical: boolean
  total_bytes_changed: number
  changed_blocks: number[]
  block_diffs: BlockChangeSummary[]
  source_check: FsckResult
  target_check: FsckResult
}

export type SchedulingAlgorithm = 'FCFS' | 'SSTF' | 'SCAN' | 'C-SCAN' | 'LOOK' | 'C-LOOK'
export type SweepDirection = 'left' | 'right'

export interface IoRequest {
  request_id: string
  original_index: number
  cylinder: number
  logical_block: number
  has_logical_block: boolean
  op_type: 'read' | 'write'
  arrival_time: number
  label: string
}

export interface SimulationStep {
  step_index: number
  from_cylinder: number
  to_cylinder: number
  movement: number
  direction: 'left' | 'right' | 'none'
  step_type: 'service' | 'boundary_visit' | 'circular_jump'
  is_service: boolean
  request_id: string
  original_index: number
  logical_block: number
  op_type: string
  cumulative_movement: number
}

export interface SimulationResult {
  algorithm: SchedulingAlgorithm
  scheduling_mode: string
  min_cylinder: number
  max_cylinder: number
  initial_head: number
  initial_direction: SweepDirection
  serviced_count: number
  total_head_movement: number
  average_movement: number
  max_single_step_movement: number
  min_single_step_movement: number
  direction_changes: number
  circular_repositioning_distance: number
  estimated_seek_time_ms: number
  median_exec_duration_us: number
  benchmark_repetitions: number
  original_requests: IoRequest[]
  service_sequence: number[]
  head_path: number[]
  steps: SimulationStep[]
}

export interface AlgorithmComparisonRow {
  result: SimulationResult
  reduction_vs_fcfs_pct: number
  rank_by_movement: number
}

export interface MultiAlgorithmComparisonResult {
  min_cylinder: number
  max_cylinder: number
  initial_head: number
  initial_direction: SweepDirection
  request_count: number
  fcfs_total_movement: number
  algorithms: AlgorithmComparisonRow[]
}

export interface WorkloadGenerationResult {
  source_type: string
  disk_name: string
  source_disk_sha256: string
  min_cylinder: number
  max_cylinder: number
  blocks_per_cylinder: number
  notes: string[]
  warnings: string[]
  requests: IoRequest[]
}

export interface AllocationSchedulingPolicyResult {
  policy: string
  external_fragmentation_pct: number
  internal_fragmentation_pct: number
  fragmented_file_count: number
  total_file_extents: number
  request_count: number
  requests: IoRequest[]
  scheduling: MultiAlgorithmComparisonResult
}

export interface AllocationSchedulingResult {
  block_size: number
  total_blocks: number
  blocks_per_cylinder: number
  policies: AllocationSchedulingPolicyResult[]
}

export type ReportType =
  | 'disk_analysis'
  | 'forensic_recovery'
  | 'corruption_repair'
  | 'disk_scheduling'
  | 'binary_analysis'
  | 'complete_investigation'

export interface ReportCsvExport {
  export_id: string
  filename: string
  relative_path: string
  row_count: number
  size_bytes: number
  sha256: string
}

export interface ReportManifest {
  report_id: string
  report_type: ReportType
  title: string
  case_id: string
  investigator: string
  created_at_utc: string
  schema_version: string
  engine_version: string
  source_disk_name: string
  source_disk_sha256: string
  experiment_id: string
  snapshot_sha256: string
  status: string
  warnings: string[]
  artifacts: {
    pdf?: { filename: string; size_bytes: number; sha256: string }
    json?: { filename: string; size_bytes: number; sha256: string }
    csv_exports?: ReportCsvExport[]
  }
}

export interface ReportSnapshot {
  report_id: string
  schema_version: string
  report_type: ReportType
  created_at_utc: string
  engine_version: string
  title: string
  case_id: string
  investigator: string
  notes: string
  source_disk_name: string
  source_disk_sha256: string
  experiment_id: string
  warnings: string[]
  snapshot_sha256: string
  evidence: Record<string, unknown>
}

export interface AvailableEvidenceResponse {
  disks: DiskListItem[]
  experiments: SandboxExperiment[]
  recovery_manifests: RecoveryManifest[]
  reports_count: number
}

export interface ReportNavigationContext {
  reportType: ReportType
  diskName?: string
  experimentId?: string
  schedulingPayload?: Record<string, unknown>
  binaryPayload?: Record<string, unknown>
  title?: string
}
