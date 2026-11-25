import { expect, test } from '@playwright/test'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const SCREENSHOT_DIR = path.resolve(__dirname, '../test-results/screenshots')
const TEST_DISK = 'e2e_phase6_test.gfs'

test('GhostFS End-to-End Forensic & Corruption Lab Flow', async ({ page, request }) => {
  // Ensure clean slate if leftover exists
  await request.delete(`http://127.0.0.1:8000/api/v1/disks/${TEST_DISK}`).catch(() => {})

  // 1. Dashboard
  await page.goto('/')
  await expect(page.getByText('GhostFS')).toBeVisible()
  await expect(page.getByText('Engine Online (GFS1)')).toBeVisible()
  await page.screenshot({ path: path.join(SCREENSHOT_DIR, '01-dashboard.png'), fullPage: true })

  // 2. Disk Manager: Create & Format dedicated test disk
  await page.getByRole('button', { name: 'Disk Manager' }).click()
  await page.getByLabel(/Disk Image Name/i).fill(TEST_DISK)
  await page.getByRole('button', { name: /Create & Format Disk Image/i }).click()
  await expect(page.getByText(`Mounted Volume Layout & Metadata Journal — ${TEST_DISK}`)).toBeVisible({
    timeout: 15000,
  })
  await page.screenshot({ path: path.join(SCREENSHOT_DIR, '02-disk-manager.png'), fullPage: true })

  // 3. File Explorer: Upload file, download check, inspect in Hex Viewer, delete
  await page.getByRole('button', { name: 'File Explorer' }).click()
  await page.getByLabel('Upload binary or text file').setInputFiles({
    name: 'e2e_note.txt',
    mimeType: 'text/plain',
    buffer: Buffer.from('GhostFS E2E Forensic Evidence Payload', 'utf-8'),
  })
  await expect(page.getByText('e2e_note.txt').first()).toBeVisible({ timeout: 10000 })
  await page.screenshot({ path: path.join(SCREENSHOT_DIR, '03-file-explorer.png'), fullPage: true })

  // Verify download link works via API
  const dlRes = await request.get(
    `http://127.0.0.1:8000/api/v1/disks/${TEST_DISK}/fs/files/download?path=/e2e_note.txt`,
  )
  expect(dlRes.ok()).toBeTruthy()
  expect(await dlRes.text()).toBe('GhostFS E2E Forensic Evidence Payload')

  // Block Explorer screenshot
  await page.getByRole('button', { name: 'Block Explorer' }).click()
  await expect(page.getByText('Selected Block Inspector').first()).toBeVisible({ timeout: 10000 })
  await page.screenshot({ path: path.join(SCREENSHOT_DIR, '04-block-explorer.png'), fullPage: true })

  // Select file & inspect in Hex Viewer
  await page.getByRole('button', { name: 'File Explorer' }).click()
  await page.locator('table').getByRole('button', { name: 'e2e_note.txt', exact: true }).click()
  await page.getByRole('button', { name: /Inspect in Hex Viewer/i }).click()
  await expect(page.getByText('GhostFS — Hex Inspector')).toBeVisible()
  await page.screenshot({ path: path.join(SCREENSHOT_DIR, '06-hex-viewer.png'), fullPage: true })

  // Delete file for Recovery Center
  await page.getByRole('button', { name: 'File Explorer' }).click()
  await page.getByRole('button', { name: 'Delete e2e_note.txt' }).click()
  await page.getByRole('button', { name: 'Confirm Forensic Delete' }).click()
  await expect(page.locator('table').getByText('e2e_note.txt')).toHaveCount(0)

  // 4. Recovery Center: Scan & Extract verified file
  await page.getByRole('button', { name: 'Recovery Center' }).click()
  await page.getByRole('button', { name: /Start Forensic Scan/i }).click()
  await expect(page.locator('table').getByText('e2e_note.txt').first()).toBeVisible({ timeout: 15000 })
  await page.locator('table').getByText('e2e_note.txt').first().click()
  await page.getByRole('button', { name: /Recover This Candidate/i }).click()
  await expect(page.getByRole('heading', { name: /Extracted Recovery Artifacts/i })).toBeVisible({
    timeout: 10000,
  })
  await page.screenshot({ path: path.join(SCREENSHOT_DIR, '05-recovery-center.png'), fullPage: true })

  // 5. Corruption Lab: Clone, Inject, Check, Plan, Dry-Run, Repair, Compare, Cleanup
  await page.getByRole('button', { name: 'Corruption Lab' }).click()
  await page.getByLabel('Original GFS1 Disk Image').selectOption(TEST_DISK)
  await page.getByLabel('Sandbox Name (optional)').fill('e2e_sb_demo')
  await page.getByRole('button', { name: /Clone to Sandbox/i }).click()
  await expect(page.getByText(/e2e_sb_demo/i).first()).toBeVisible({ timeout: 10000 })

  // Inject primary superblock corruption
  await page.getByRole('button', { name: /Inject Corruption into Sandbox/i }).click()
  await page.getByRole('dialog').getByRole('button', { name: 'Inject Corruption' }).click()
  await expect(page.getByText(/Corrupted primary superblock magic/i)).toBeVisible({ timeout: 10000 })

  // Run gfsck
  await page.getByRole('button', { name: /Run gfsck/i }).click()
  await expect(page.getByText('PRIMARY_SUPERBLOCK_CORRUPT')).toBeVisible({ timeout: 10000 })

  // Generate repair plan & Dry-Run
  await page.getByRole('button', { name: /Generate Repair Plan/i }).click()
  await expect(page.getByText(/restore_primary_superblock/i)).toBeVisible({ timeout: 10000 })
  await page.getByRole('button', { name: /Dry-Run Preview/i }).click()

  // Apply Repairs
  await page.getByRole('button', { name: /Apply Selected Repairs/i }).click()
  await page.getByRole('dialog').getByRole('button', { name: 'Apply Repairs' }).click()
  await expect(page.getByText(/Before-and-After Forensic Comparison/i)).toBeVisible({ timeout: 15000 })
  await page.screenshot({ path: path.join(SCREENSHOT_DIR, '07-corruption-lab.png'), fullPage: true })

  // Delete sandbox experiment before moving to Disk Scheduler
  await page.getByRole('button', { name: 'Delete selected experiment' }).click()
  await page.getByRole('dialog').getByRole('button', { name: 'Delete Experiment' }).click()

  // 6. Disk Scheduler: Textbook, Playback, Compare All, File-Based Workload, Allocation Integration
  await page.getByRole('button', { name: 'Disk Scheduler' }).click()
  await expect(page.getByText('Disk Scheduling Laboratory')).toBeVisible()
  await expect(page.getByText('236').first()).toBeVisible({ timeout: 10000 })
  await expect(page.getByText('640').first()).toBeVisible()
  await page.screenshot({
    path: path.join(SCREENSHOT_DIR, '08-disk-scheduler.png'),
    fullPage: true,
  })

  // Test playback step controls
  await page.getByRole('button', { name: 'Restart playback' }).click()
  await expect(page.getByText('Step 0/')).toBeVisible()
  await page.getByRole('button', { name: 'Next step' }).click()
  await expect(page.getByText('Step 1/')).toBeVisible()

  // Test combined allocation+scheduling comparison
  await page.getByRole('button', { name: 'Run Combined' }).click()
  await expect(page.getByText('first_fit').first()).toBeVisible({ timeout: 15000 })

  // 7. Forensic Reports: Preview, Generate PDF/JSON/CSV, Verify Integrity, Delete
  await page.getByRole('button', { name: 'Forensic Reports' }).click()
  await expect(page.getByText('Forensic Reports & Evidence Export')).toBeVisible()
  await page.getByLabel('Source Disk').selectOption(TEST_DISK)
  await page.getByLabel('Report Title').fill('E2E Investigation Report')
  await page.getByRole('button', { name: /Preview Snapshot/i }).click()
  await expect(page.getByText(/Snapshot SHA-256:/i)).toBeVisible({ timeout: 10000 })
  await page.getByRole('button', { name: /Generate Report/i }).click()
  await expect(page.getByText('E2E Investigation Report').first()).toBeVisible({ timeout: 15000 })
  await page.getByRole('button', { name: /^Verify$/i }).first().click()
  await expect(page.getByRole('button', { name: 'Verified' }).first()).toBeVisible({ timeout: 10000 })
  await page.screenshot({ path: path.join(SCREENSHOT_DIR, '09-forensic-reports.png'), fullPage: true })

  // Cleanup: Delete generated E2E report & test disk
  await page.locator('button[aria-label^="Delete report "]').first().click()
  await page.getByRole('dialog').getByRole('button', { name: 'Delete Report' }).click()
  await request.delete(`http://127.0.0.1:8000/api/v1/disks/${TEST_DISK}`).catch(() => {})
})
