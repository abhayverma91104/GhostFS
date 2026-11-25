# GhostFS Testing Guide

## Automated Test Suites
1. **C++17 Engine (45 tests)**:
   ```bash
   ctest --test-dir engine/build --output-on-failure
   ```
2. **FastAPI Backend (12 tests)**:
   ```bash
   backend/.venv/bin/pytest backend/tests -v
   ```
3. **Frontend TypeScript & Vite Build**:
   ```bash
   npm --prefix frontend run build
   ```
4. **All-in-One Script**:
   ```bash
   ./scripts/test.sh
   ```
5. **Playwright Browser E2E**:
   ```bash
   npx --prefix frontend playwright test --config frontend/playwright.config.ts
   ```
   (Set `PLAYWRIGHT_EXECUTABLE_PATH` if using a pre-installed local browser.)
6. **Linux ASan & UBSan**:
   ```bash
   ./scripts/sanitize.sh asan
   ./scripts/sanitize.sh ubsan
   ```
