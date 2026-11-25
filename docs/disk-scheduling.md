# GhostFS Phase 7 — Disk Scheduling & I/O Analysis

This document specifies the C++17 educational disk scheduling simulator, implemented algorithms, mathematical formulas, GFS1 block-to-cylinder workload integration, allocation-policy integration, API contracts, and browser verification.

---

## 1. Educational Simulation Boundary

> **Important:** GhostFS operates on virtual `.gfs` disk image files stored on a host filesystem. The Phase 7 disk scheduling engine is an **educational simulator**, not a replacement for the host OS I/O scheduler. It does **not** control physical disk heads, reorder kernel block-layer requests, measure hardware seek latencies, or model SSD Flash Translation Layer (FTL) wear-leveling.

---

## 2. Implemented Algorithms & Rules

| Algorithm | Selection Rule | Boundary / Circular Rule | Time Complexity |
| :--- | :--- | :--- | :--- |
| **FCFS** | Services requests strictly in input/arrival order. | None. | $O(N)$ |
| **SSTF** | Selects pending request with minimum $|c_{\text{req}} - c_{\text{head}}|$. Tie-breaker: current sweep direction, then lower cylinder, then earlier queue index. | None. Susceptible to starvation under dynamic workloads. | $O(N^2)$ |
| **SCAN** | Sweeps in `initial_direction` servicing requests along the way. | Travels to `max_cylinder` (Right) or `min_cylinder` (Left) when reversal is required, then reverses. | $O(N \log N)$ |
| **C-SCAN** | Sweeps in `initial_direction` servicing requests. | Travels to boundary, wraps to opposite boundary (`circular_jump`), continues in same direction. | $O(N \log N)$ |
| **LOOK** | Sweeps in `initial_direction` servicing requests. | Reverses directly at the last pending request without visiting unused physical boundaries. | $O(N \log N)$ |
| **C-LOOK** | Sweeps in `initial_direction` servicing requests. | Jumps directly from last request in direction to the opposite extreme pending request (`circular_jump`). | $O(N \log N)$ |

- **Space Complexity**: $O(N)$ for pending items, `head_path`, and `steps`.
- **Static vs. Dynamic Mode**: Static mode (default) assumes all requests are available at $t = 0$. Dynamic mode (`scheduling_mode = "dynamic"`) only considers requests with `arrival_time <= current_time`.

---

## 3. Mathematical Formulas

- **Total Head Movement**:
  $$\text{Total} = \sum_{i=1}^{k} |c_i - c_{i-1}|$$
  Includes boundary visits and circular repositioning jumps (`C-SCAN` / `C-LOOK`).
- **Average Movement per Request**:
  $$\text{Average} = \begin{cases} \frac{\text{Total}}{N_{\text{serviced}}}, & N_{\text{serviced}} > 0 \\ 0, & N_{\text{serviced}} = 0 \end{cases}$$
- **Relative Movement Reduction vs. FCFS (%)**:
  $$\text{Reduction (\%)} = \frac{\text{FCFS} - \text{Alg}}{\text{FCFS}} \times 100 \quad (\text{defined as } 0 \text{ when } \text{FCFS} = 0)$$

---

## 4. GFS1 Block-to-Cylinder Mapping & Workloads

Logical block addresses (LBAs) from GFS1 files, recovery candidates, and metadata regions are mapped deterministically to simulated cylinders via:
$$\text{cylinder} = \left\lfloor \frac{\text{LBA}}{\text{blocks\_per\_cylinder}} \right\rfloor$$
GFS1 LBAs are virtual image offsets, not physical HDD cylinder locations.

Supported workload sources:
- **Manual Queue**, **Synthetic Generator** (`sequential`, `random`, `clustered`, `mixed`, `hotspot`, `fragmented` with deterministic `seed`), **GFS1 Files** (logical data + optional inode/indirect blocks), **Recovery Candidates** (surviving blocks only, excluding overwritten/missing ranges), **Metadata Regions**, and **4-Policy Allocation + 6-Algorithm Scheduling Comparison**.

---

## 5. Textbook Regression Baseline

For queue `[98, 183, 37, 122, 14, 124, 65, 67]`, `initial_head = 53`, `range = 0..199`, `direction = right`:
- **FCFS**: `640`
- **SSTF**: `236`
- **SCAN**: `331`
- **C-SCAN**: `382`
- **LOOK**: `299`
- **C-LOOK**: `322`

---

## 6. FastAPI Endpoints (`/api/v1`)

- `POST /api/v1/scheduling/simulate`
- `POST /api/v1/scheduling/compare`
- `POST /api/v1/scheduling/workloads/generate`
- `POST /api/v1/disks/{disk_name}/scheduling/workloads/files`
- `POST /api/v1/disks/{disk_name}/scheduling/workloads/recovery`
- `POST /api/v1/disks/{disk_name}/scheduling/workloads/metadata`
- `POST /api/v1/scheduling/allocation-compare`
