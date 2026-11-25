import { defineConfig } from '@playwright/test'

const executablePath = process.env.PLAYWRIGHT_EXECUTABLE_PATH

export default defineConfig({
  testDir: './e2e',
  timeout: 60000,
  retries: 0,
  use: {
    baseURL: 'http://127.0.0.1:5173',
    headless: true,
    viewport: { width: 1440, height: 900 },
    ...(executablePath ? { launchOptions: { executablePath } } : {}),
  },
  outputDir: './test-results',
})
